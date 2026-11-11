// SPDX-License-Identifier: GPL-2.0-only
/*
 * Borrowed-shell retransmission (experiment, module parameter shell_retx).
 *
 * The FEC layer already treats every 1079-byte HTTP chunk as an independent
 * (block, group, share) record. When a carrier has a TCP hole, the native
 * retransmission would send the same stale record again. This file instead
 * rewrites the payload of the skb that is about to be retransmitted with a
 * record taken from the FEC output FIFO, so the retransmission slot carries
 * fresh FEC work.
 *
 * Preconditions enforced before the payload is touched:
 *   - the feature is on and this carrier accepted the wrapper cc;
 *   - one chunk is one skb: every carrier write is MSG_EOR and the socket's
 *     GSO size is capped at its MSS (tbt_carrier.c), and this skb is exactly
 *     one chunk long, not GSO, not cloned/shared, and no clone of it is
 *     still in the host queue;
 *   - the skb is marked LOST and has never been retransmitted, so the native
 *     path will send it after this callback and no older copy is in flight;
 *   - the current payload is a full DATA/PARITY share chunk, not a control
 *     frame, and the socket is CHECKSUM_PARTIAL;
 *   - the current MSS can carry the chunk in one segment.
 *
 * Ownership: the stolen record lives in a 64-entry per-carrier ring keyed by
 * the skb's TCP sequence. It is resolved exactly once:
 *   - the sequence is ACKed without a DSACK: the record was delivered;
 *   - a DSACK arrives: the peer already had those bytes, so the rewritten
 *     record was discarded as a duplicate and returns to the replay queue;
 *   - the carrier dies or the connection resets: the record returns to the
 *     replay queue;
 *   - a live slot exceeds TBT_SHELL_SLOT_MAX_MS: it returns to replay (a
 *     later retransmission of the same skb then duplicates it, which the
 *     peer's share dedupe absorbs).
 * No free or allocation runs in the ACK callback: it only marks slots. The
 * session poll performs the queue work in process context.
 */

#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_http.h"

/* A live slot older than this returns to replay. It must exceed the largest
 * RTO backoff the carrier can use before the skb leaves the rtx queue; 10 s
 * is well above the 200 ms base and the usual 3-4 retries. */
#define TBT_SHELL_SLOT_MAX_MS 10000u

static DEFINE_MUTEX(tbt_shell_cc_lock);
static struct tcp_congestion_ops tbt_shell_cc;
static struct module *tbt_shell_cc_owner;
static bool tbt_shell_cc_registered;
static void (*tbt_shell_orig_cong_control)(struct sock *sk, u32 ack, int flag,
                                           const struct rate_sample *rs);

static struct tbt_shell_slot *
tbt_shell_find_slot_locked(struct tbt_shell_retx *sh, u32 seq)
{
    u32 i;

    for (i = 0; i < TBT_SHELL_RING; i++)
        if (sh->slot[i].state != TBT_SHELL_FREE &&
            sh->slot[i].seq == seq)
            return &sh->slot[i];
    return NULL;
}

static struct tbt_shell_slot *
tbt_shell_free_slot_locked(struct tbt_shell_retx *sh)
{
    u32 i;

    for (i = 0; i < TBT_SHELL_RING; i++)
    {
        u32 pos = (sh->next + i) % TBT_SHELL_RING;

        if (sh->slot[pos].state == TBT_SHELL_FREE)
        {
            sh->next = (pos + 1) % TBT_SHELL_RING;
            return &sh->slot[pos];
        }
    }
    return NULL;
}

/* A full DATA/PARITY share chunk has a FEC record body of exactly S bytes.
 * The chunk prefix (the HTTP size line) is compared against the freshly
 * encoded chunk, so a control frame or a chunk of another length can never
 * pass. */
static bool tbt_shell_payload_is_share(const struct tbt_shell_retx *sh,
                                       const u8 *buf, size_t len, size_t off)
{
    struct tbt_fec_hdr h;

    if (len != off + TBT_FEC_HDR_LEN + TBT_FEC_S)
        return false;
    if (off && memcmp(buf, sh->chunk, off))
        return false;
    if (tbt_fec_hdr_unpack(buf + off, &h))
        return false;
    if (tbt_fec_hdr_check(&h, TBT_FEC_S))
        return false;
    if (h.payload_len != TBT_FEC_S)
        return false;
    return h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY;
}

/* Resolve every slot the current ACK can decide. Runs in the ACK callback
 * with the socket locked. Only marks slots; the poll does the real work. */
static void tbt_shell_resolve(struct tbt_shell_retx *sh,
                              struct tbt_session *sess, struct tcp_sock *tp)
{
    unsigned long flags;
    u32 una = READ_ONCE(tp->snd_una);
    u32 dsack = READ_ONCE(tp->dsack_dups);
    bool new_dsack = dsack != sh->dsack_seen;
    u32 i;

    if (new_dsack)
    {
        sh->dsack_seen = dsack;
        atomic64_inc(&sess->shell_dsack_events);
    }
    spin_lock_irqsave(&sh->lock, flags);
    for (i = 0; i < TBT_SHELL_RING; i++)
    {
        struct tbt_shell_slot *s = &sh->slot[i];

        if (s->state != TBT_SHELL_LIVE)
            continue;
        if (!before(una, s->end_seq))
        {
            /* The whole range is acknowledged. A DSACK in this ACK means
             * the peer already had it before the rewrite; without one, the
             * native retransmission was accepted. */
            if (new_dsack)
            {
                s->state = TBT_SHELL_REQUEUE;
                atomic64_inc(&sess->shell_dsack_requeue);
            }
            else
            {
                s->state = TBT_SHELL_ACKED;
                atomic64_inc(&sess->shell_ack_ok);
                if (!s->seen_retx)
                    atomic64_inc(&sess->shell_ack_unconfirmed);
            }
        }
    }
    spin_unlock_irqrestore(&sh->lock, flags);
}

/* Find the skb the native retransmit path is about to send and that can be
 * rewritten safely. Called with the socket lock held, before
 * tcp_fastretrans_alert()/tcp_xmit_recovery(). */
static struct sk_buff *tbt_shell_pick_skb(struct sock *sk,
                                          struct tbt_shell_retx *sh,
                                          struct tbt_session *sess,
                                          struct tcp_sock *tp, size_t want_len)
{
    struct sk_buff *skb, *best = NULL;
    unsigned long flags;

    spin_lock_irqsave(&sh->lock, flags);
    skb = tp->retransmit_skb_hint ?: tcp_rtx_queue_head(sk);
    skb_rbtree_walk_from(skb)
    {
        struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
        struct tbt_shell_slot *owned;
        u8 sacked = cb->sacked;

        owned = tbt_shell_find_slot_locked(sh, cb->seq);
        if (owned)
        {
            /* The substitute is still on this skb. Observe a later
             * retransmission of the same payload once. No second record is
             * ever stolen for it. */
            if (owned->state == TBT_SHELL_LIVE && !owned->seen_retx &&
                (sacked & TCPCB_EVER_RETRANS))
            {
                owned->seen_retx = 1;
                atomic64_inc(&sess->shell_retx_same);
            }
            continue;
        }
        if (best)
            continue;
        if (sacked & (TCPCB_SACKED_ACKED | TCPCB_SACKED_RETRANS |
                      TCPCB_EVER_RETRANS))
            continue;
        if (!(sacked & TCPCB_LOST))
        {
            atomic64_inc(&sess->shell_skip_lost);
            continue;
        }
        if (skb->len != want_len)
        {
            atomic64_inc(&sess->shell_skip_len);
            continue;
        }
        if (skb_is_gso(skb))
        {
            atomic64_inc(&sess->shell_skip_gso);
            continue;
        }
        if (skb_cloned(skb))
        {
            atomic64_inc(&sess->shell_skip_cloned);
            continue;
        }
        if (skb_shared(skb))
        {
            atomic64_inc(&sess->shell_skip_shared);
            continue;
        }
        if (skb_zcopy(skb))
        {
            atomic64_inc(&sess->shell_skip_zcopy);
            continue;
        }
        if (skb->ip_summed == CHECKSUM_NONE)
            atomic64_inc(&sess->shell_csum_none);
        else if (skb->ip_summed == CHECKSUM_PARTIAL)
            atomic64_inc(&sess->shell_csum_partial);
        else
        {
            atomic64_inc(&sess->shell_skip_csum);
            continue;
        }
        if (skb_fclone_busy(sk, skb))
        {
            /* The original copy (or an older retransmission) is still in
             * the qdisc/driver. Rewriting now would race its bytes. */
            atomic64_inc(&sess->shell_skip_busy);
            continue;
        }
        best = skb;
    }
    spin_unlock_irqrestore(&sh->lock, flags);
    return best;
}

static size_t tbt_shell_encode(struct tbt_carrier *c,
                               struct tbt_shell_retx *sh, const u8 *rec,
                               u16 len)
{
    if (c->profile != 3)
        return tbt_http_fec_chunk_encode((char *)sh->chunk,
                                         sizeof(sh->chunk), rec, len);
    if (len > sizeof(sh->chunk))
        return 0;
    memcpy(sh->chunk, rec, len);
    return len;
}

/* The substitution proper. Every refusal returns the stolen record to the
 * FIFO (or a REQUEUE slot) and names the reason in a counter. */
static void tbt_shell_try(struct tbt_carrier *c, struct tbt_shell_retx *sh,
                          struct tbt_session *sess, struct sock *sk,
                          struct tcp_sock *tp)
{
    size_t off = c->profile != 3 ? 5 : 0;
    size_t want_len = off + TBT_FEC_HDR_LEN + TBT_FEC_S;
    struct tbt_fec_out *o;
    struct tbt_shell_slot *slot;
    struct sk_buff *skb;
    unsigned long flags;
    struct tbt_fec_hdr h;
    size_t n;
    u32 seq, end_seq;

    if (READ_ONCE(tp->mss_cache) < want_len)
    {
        atomic64_inc(&sess->shell_skip_mss);
        return;
    }
    skb = tbt_shell_pick_skb(sk, sh, sess, tp, want_len);
    if (!skb)
        return;

    /* Reserve a slot before taking a record, so every later refusal has a
     * place to park it. The poll never takes a FREE slot. */
    spin_lock_irqsave(&sh->lock, flags);
    slot = tbt_shell_free_slot_locked(sh);
    spin_unlock_irqrestore(&sh->lock, flags);
    if (!slot)
    {
        atomic64_inc(&sess->shell_ring_full);
        return;
    }

    o = tbt_fec_out_steal(sess);
    if (!o)
    {
        atomic64_inc(&sess->shell_no_record);
        return;
    }
    if (tbt_fec_hdr_unpack(o->data, &h) || tbt_fec_hdr_check(&h, TBT_FEC_S) ||
        h.payload_len != TBT_FEC_S ||
        (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY))
    {
        atomic64_inc(&sess->shell_rec_invalid);
        if (!tbt_fec_out_unsteal(sess, o))
            tbt_fec_out_drop_stolen(sess, o);
        return;
    }
    n = tbt_shell_encode(c, sh, o->data, o->len);
    if (n != want_len)
    {
        atomic64_inc(&sess->shell_encode_fail);
        if (!tbt_fec_out_unsteal(sess, o))
            tbt_fec_out_drop_stolen(sess, o);
        return;
    }
    if (skb_copy_bits(skb, 0, sh->check, want_len) ||
        !tbt_shell_payload_is_share(sh, sh->check, want_len, off))
    {
        /* The skb does not hold one full share chunk. Control frames and
         * coalesced/fragmented chunks land here; nothing is written. */
        atomic64_inc(&sess->shell_skip_prefix);
        if (!tbt_fec_out_unsteal(sess, o))
            tbt_fec_out_drop_stolen(sess, o);
        return;
    }

    seq = TCP_SKB_CB(skb)->seq;
    end_seq = TCP_SKB_CB(skb)->end_seq;
    spin_lock_irqsave(&sh->lock, flags);
    slot->seq = seq;
    slot->end_seq = end_seq;
    slot->block_id = h.block_id;
    slot->group = h.reserved;
    slot->share = h.share_index;
    slot->state = TBT_SHELL_RESERVED;
    slot->seen_retx = 0;
    slot->live_ns = ktime_get_ns();
    slot->rec = o;
    spin_unlock_irqrestore(&sh->lock, flags);

    if (skb_store_bits(skb, 0, sh->chunk, want_len))
    {
        atomic64_inc(&sess->shell_store_fail);
        spin_lock_irqsave(&sh->lock, flags);
        slot->state = TBT_SHELL_REQUEUE;
        spin_unlock_irqrestore(&sh->lock, flags);
        return;
    }
    /* Self-check: the bytes in the skb must be exactly the bytes
     * tbt_http_fec_chunk_encode() produced for this record. A framing or
     * layout mismatch becomes a counter, never silent corruption. */
    if (skb_copy_bits(skb, 0, sh->check, want_len) ||
        memcmp(sh->check, sh->chunk, want_len))
    {
        atomic64_inc(&sess->shell_selfcheck_fail);
        spin_lock_irqsave(&sh->lock, flags);
        slot->state = TBT_SHELL_REQUEUE;
        spin_unlock_irqrestore(&sh->lock, flags);
        return;
    }
    spin_lock_irqsave(&sh->lock, flags);
    slot->state = TBT_SHELL_LIVE;
    spin_unlock_irqrestore(&sh->lock, flags);
    atomic64_inc(&sess->shell_subst);
    atomic64_add(want_len, &sess->shell_subst_bytes);
}

/* The substitution runs from cong_control, not in_ack_event: BBR has no
 * in_ack_event of its own, and the classic fast-retransmit path marks the
 * head skb TCPCB_LOST inside tcp_fastretrans_alert(), which runs after
 * in_ack_event and before tcp_cong_control(). By the time this wrapper is
 * called, the loss decision is final and tcp_xmit_recovery() has not sent
 * anything yet. The original algorithm runs first so cwnd/pacing are the
 * values the retransmit loop will use. */
static void tbt_shell_cong_control(struct sock *sk, u32 ack, int flag,
                                   const struct rate_sample *rs)
{
    struct tbt_carrier *c;
    struct tbt_shell_retx *sh;
    struct tbt_session *sess;
    struct tcp_sock *tp;

    if (tbt_shell_orig_cong_control)
        tbt_shell_orig_cong_control(sk, ack, flag, rs);
    if (!READ_ONCE(tbt_shell_retx))
        return;
    c = READ_ONCE(sk->sk_user_data);
    if (!c || !(sh = READ_ONCE(c->shell)))
        return;
    sess = c->sess;
    if (!sess || !sess->fec || READ_ONCE(sess->fec_stopping))
        return;
    tp = tcp_sk(sk);
    if (!READ_ONCE(tp->lost_out) ||
        READ_ONCE(tp->retrans_out) >= READ_ONCE(tp->lost_out))
        return;
    if (tcp_snd_cwnd(tp) <= tcp_packets_in_flight(tp))
        return;
    tbt_shell_try(c, sh, sess, sk, tp);
}

/* in_ack_event runs after tcp_clean_rtx_queue() and before
 * tcp_fastretrans_alert(): it is the earliest point where this ACK's ACKed
 * and DSACK state is final, so it resolves ownership only. The rewrite
 * itself waits for cong_control. */
/* in_ack_event: resolve ownership only. */
static void tbt_shell_in_ack(struct sock *sk, u32 flags)
{
    struct tbt_carrier *c;
    struct tbt_shell_retx *sh;
    struct tbt_session *sess;
    struct tcp_sock *tp;

    (void)flags;
    if (!READ_ONCE(tbt_shell_retx))
        return;
    c = READ_ONCE(sk->sk_user_data);
    if (!c)
        return;
    sh = READ_ONCE(c->shell);
    if (!sh)
        return;
    sess = c->sess;
    if (!sess || !sess->fec || READ_ONCE(sess->fec_stopping))
        return;
    tp = tcp_sk(sk);

    tbt_shell_resolve(sh, sess, tp);
}

/* Before connect: Linux applies TCP_MAXSEG at handshake time; a call after
 * connect is ignored (measured on 7.2.9: a connected socket kept its old
 * mss_cache). The option value includes the 12-byte aligned timestamp option,
 * so 1079 + 12 produces an effective mss_cache of 1079; a literal 1079
 * produces 1067 and would split every 1079-byte chunk into two segments. */
void tbt_shell_retx_pre_connect(struct tbt_carrier *c, struct socket *s)
{
    u32 want;
    int v;

    if (!READ_ONCE(tbt_shell_retx) || !c || !s || !s->sk)
        return;
    want = c->profile != 3 ? TBT_SHELL_HTTP_CHUNK_LEN
                           : TBT_SHELL_RAW_CHUNK_LEN;
    v = (int)want + 12;
    if (sock_setsockopt(s, SOL_TCP, TCP_MAXSEG, KERNEL_SOCKPTR(&v), sizeof(v)))
        atomic64_inc(&c->sess->shell_prep_fail);
}

int tbt_shell_retx_install(struct tbt_carrier *c, struct socket *s)
{
    struct inet_connection_sock *icsk;
    struct tbt_shell_retx *sh;
    const struct tcp_congestion_ops *bbr;
    int err;

    if (!READ_ONCE(tbt_shell_retx) || !c || !s || !s->sk)
        return 0;
    if (c->shell)
        return 0;
    sh = kzalloc(sizeof(*sh), GFP_KERNEL);
    if (!sh)
        return -ENOMEM;
    spin_lock_init(&sh->lock);

    icsk = inet_csk(s->sk);
    bbr = READ_ONCE(icsk->icsk_ca_ops);
    if (!bbr || strcmp(bbr->name, "bbr"))
    {
        kfree(sh);
        return -EINVAL;
    }
    mutex_lock(&tbt_shell_cc_lock);
    if (!tbt_shell_cc_registered)
    {
        if (!bbr->cong_control)
        {
            err = -EINVAL;
            goto out_unlock;
        }
        tbt_shell_cc = *bbr;
        tbt_shell_orig_cong_control = bbr->cong_control;
        strscpy(tbt_shell_cc.name, "tbt_shell", TCP_CA_NAME_MAX);
        tbt_shell_cc.owner = THIS_MODULE;
        tbt_shell_cc.in_ack_event = tbt_shell_in_ack;
        tbt_shell_cc.cong_control = tbt_shell_cong_control;
        err = tcp_register_congestion_control(&tbt_shell_cc);
        if (err)
            goto out_unlock;
        tbt_shell_cc_owner = bbr->owner;
        if (tbt_shell_cc_owner && !try_module_get(tbt_shell_cc_owner))
        {
            tcp_unregister_congestion_control(&tbt_shell_cc);
            err = -ENOENT;
            goto out_unlock;
        }
        tbt_shell_cc_registered = true;
    }
    mutex_unlock(&tbt_shell_cc_lock);

    err = sock_setsockopt(s, SOL_TCP, TCP_CONGESTION,
                          KERNEL_SOCKPTR("tbt_shell"),
                          (unsigned int)strlen("tbt_shell"));
    if (err)
        err = sock_setsockopt(s, SOL_TCP, TCP_CONGESTION,
                              KERNEL_SOCKPTR("tbt_shell"),
                              sizeof("tbt_shell"));
    if (!err && READ_ONCE(icsk->icsk_ca_ops) != &tbt_shell_cc)
        err = -EINVAL;
    if (err)
    {
        atomic64_inc(&c->sess->shell_install_fail);
        kfree(sh);
        return err;
    }
    c->shell = sh;
    return 0;

out_unlock:
    mutex_unlock(&tbt_shell_cc_lock);
    kfree(sh);
    if (err != -EINVAL)
        atomic64_inc(&c->sess->shell_install_fail);
    return err;
}

/* Process-context half: free delivered records, return stranded ones to the
 * replay queue, and retire slots whose carrier is gone. Called from the 50 ms
 * session poll and from carrier close. */
static void tbt_shell_poll_carrier(struct tbt_carrier *c)
{
    struct tbt_shell_retx *sh = c->shell;
    struct tbt_session *sess = c->sess;
    u64 now = ktime_get_ns();
    u32 i;

    if (!sh || !sess)
        return;
    for (i = 0; i < TBT_SHELL_RING; i++)
    {
        struct tbt_shell_slot *s = &sh->slot[i];
        struct tbt_fec_out *rec = NULL;
        u8 state;
        unsigned long flags;

        spin_lock_irqsave(&sh->lock, flags);
        state = s->state;
        if (state == TBT_SHELL_LIVE &&
            (atomic_read(&c->stopping) || !tbt_carrier_sk_present(c) ||
             now - s->live_ns > (u64)TBT_SHELL_SLOT_MAX_MS * NSEC_PER_MSEC))
        {
            s->state = TBT_SHELL_REQUEUE;
            atomic64_inc(&sess->shell_timeout);
            state = TBT_SHELL_REQUEUE;
        }
        if (state == TBT_SHELL_ACKED || state == TBT_SHELL_REQUEUE)
        {
            s->state = TBT_SHELL_PROCESSING;
            rec = s->rec;
            s->rec = NULL;
            spin_unlock_irqrestore(&sh->lock, flags);

            if (state == TBT_SHELL_ACKED)
                tbt_fec_out_drop_stolen(sess, rec);
            else if (!tbt_fec_out_to_replay(sess, rec))
            {
                atomic64_inc(&sess->shell_requeue_fail);
                atomic64_inc(&sess->shell_stranded);
                tbt_fec_out_drop_stolen(sess, rec);
            }

            spin_lock_irqsave(&sh->lock, flags);
            s->state = TBT_SHELL_FREE;
            s->rec = NULL;
            spin_unlock_irqrestore(&sh->lock, flags);
        }
        else
            spin_unlock_irqrestore(&sh->lock, flags);
    }
}

void tbt_shell_retx_poll(struct tbt_session *sess)
{
    u16 i;

    if (!READ_ONCE(tbt_shell_retx) || !sess || !sess->fec)
        return;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, i);

        if (!c)
            continue;
        if (c->shell)
            tbt_shell_poll_carrier(c);
        tbt_carrier_put(c);
    }
}

/* Called before the carrier socket dies. Every owned record is returned to
 * the replay queue while the session can still dispatch it. */
void tbt_shell_retx_carrier_close(struct tbt_carrier *c)
{
    struct tbt_shell_retx *sh;
    struct tbt_session *sess;
    u32 i;

    if (!c)
        return;
    sh = c->shell;
    sess = c->sess;
    if (!sh || !sess)
        return;
    for (i = 0; i < TBT_SHELL_RING; i++)
    {
        struct tbt_shell_slot *s = &sh->slot[i];
        struct tbt_fec_out *rec = NULL;
        unsigned long flags;

        spin_lock_irqsave(&sh->lock, flags);
        if (s->rec)
        {
            rec = s->rec;
            s->rec = NULL;
            s->state = TBT_SHELL_PROCESSING;
        }
        spin_unlock_irqrestore(&sh->lock, flags);
        if (!rec)
            continue;
        if (!tbt_fec_out_to_replay(sess, rec))
        {
            atomic64_inc(&sess->shell_requeue_fail);
            tbt_fec_out_drop_stolen(sess, rec);
        }
        atomic64_inc(&sess->shell_carrier_lost);
        spin_lock_irqsave(&sh->lock, flags);
        s->state = TBT_SHELL_FREE;
        spin_unlock_irqrestore(&sh->lock, flags);
    }
}

/* RCU callback context: the records were drained by carrier_close(). */
void tbt_shell_retx_carrier_free(struct tbt_carrier *c)
{
    if (!c || !c->shell)
        return;
    {
        u32 i;

        for (i = 0; i < TBT_SHELL_RING; i++)
            WARN_ON_ONCE(c->shell->slot[i].rec != NULL);
    }
    kfree(c->shell);
    c->shell = NULL;
}

void tbt_shell_retx_exit(void)
{
    if (!tbt_shell_cc_registered)
        return;
    tcp_unregister_congestion_control(&tbt_shell_cc);
    if (tbt_shell_cc_owner)
        module_put(tbt_shell_cc_owner);
    tbt_shell_cc_owner = NULL;
    tbt_shell_cc_registered = false;
}
