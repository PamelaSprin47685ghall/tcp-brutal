/* tbt_cc.c - the carrier's loss-tolerant congestion control and the downlink
 * shell retransmission.
 *
 * WHY A PRIVATE CONGESTION CONTROL
 *   The carrier TCP is not the reliability layer: every record it carries is
 *   also stored in the FEC replay ring, and the receiver repairs a missing
 *   share with a NACK. A SACK loss therefore means "one copy was lost", not
 *   "the path is congested". BBR still halves its window on such an event and
 *   then idles while the repair is in flight. tbt_cc instead:
 *
 *     - keeps the maximum delivery rate of the last ten RTTs (max_bw);
 *     - reads the peer's FEC_FBK loss estimate p for this direction;
 *     - paces at gain * max_bw / (1 - p), clamped to
 *       [TBT_CC_PACE_FLOOR_BPS, tunnel rate];
 *     - keeps snd_cwnd at max(2 * BDP, TBT_CC_MIN_CWND_PKTS) and reports
 *       ssthresh/undo_cwnd as that same window, so SACK loss never shrinks
 *       it;
 *     - ignores CA_EVENT_CWND_RESTART, which is the event the peripheral
 *       BBR hold (tbt_carrier.c) would otherwise use to collapse the idle
 *       window.
 *
 * SHELL RETRANSMISSION
 *   A TCP hole is a segment the peer did not receive, so its bytes are free
 *   to carry something else. in_ack_event walks the retransmit queue up to
 *   highest_sack, and for an un-SACKed, un-retransmitted, linear, unshared
 *   skb whose length equals one complete framed record, it pops the session
 *   FIFO head and copies that record into skb->data. The original record is
 *   still in the FEC replay ring; the peer NACKs it and gets a repair. This
 *   turns a retransmission slot into new payload instead of a duplicate.
 *
 *   If the original segment was only reordered, the peer's TCP sees the
 *   mutated retransmission as a duplicate and answers with DSACK. The
 *   mut_ring records every mutated hole, so a DSACK naming one means the
 *   replacement record was discarded: it is pushed back onto the FEC replay
 *   queue at once (car_shell_dsack_rescued).
 *
 * The ring never touches skb->cb: those bytes belong to TCP.
 */
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <net/checksum.h>
#include <net/tcp.h>

#include "tbtun.h"

/* The tunnel's own rate, used as the upper pacing clamp: the operator's
 * tunnel_rate first, then the measured session rate, then the window/RTT
 * estimate. A zero value means "not known yet" and leaves pacing uncapped. */
static u64 tbt_cc_tunnel_cap(struct tbt_session *sess)
{
    u32 cfg_rate = READ_ONCE(sess->tunnel->cfg.tunnel_rate);
    u64 rate = READ_ONCE(sess->rate_bps_ewma);
    u32 win, rtt;

    if (cfg_rate)
        return cfg_rate;
    if (rate)
        return rate;
    win = READ_ONCE(sess->tx_window);
    rtt = READ_ONCE(sess->rtt_mean_us);
    if (win && rtt)
        return (u64)win * 1000000ULL / rtt;
    return 0;
}

static u32 tbt_cc_srtt_us(const struct tcp_sock *tp)
{
    u32 srtt = READ_ONCE(tp->srtt_us) >> 3;

    return srtt ? srtt : 1;
}

/* Push one record named by the mut_ring back onto the FEC replay queue. The
 * record is looked up in the primary TX plane's replay ring: 0.7.x runs one
 * FEC flow per session, and the mut_ring stores no sid. */
static void tbt_cc_rescue(struct tbt_session *sess, struct tbt_cc_mut *m)
{
    if (!m || !m->valid)
        return;
    if (!tbt_fec_replay_share(sess, m->block_id, m->group, m->share))
    {
        m->valid = 0;
        m->pending = 0;
        atomic64_inc(&sess->car_shell_dsack_rescued);
    }
    else
    {
        /* Keep the slot alive for the eviction-path retry: snd_una may
         * already be past this hole, and mut_ack skips pending slots. */
        m->pending = 1;
    }
}

/* (a) DSACK: the peer discarded the segment we filled. Repair the replacement
 * record now; the hole's original record is already in the replay ring. */
static void tbt_cc_dsack_rescue(struct tcp_sock *tp, struct tbt_carrier *c,
                                struct tbt_session *sess)
{
    struct tbt_cc_mut *m;
    u32 start = tp->duplicate_sack[0].start_seq;
    u32 end = tp->duplicate_sack[0].end_seq;

    if (!tp->rx_opt.dsack || !end || !tbt_cc_seq_before(start, end))
        return;
    m = tbt_cc_mut_dsack(c->mut_ring, start, end);
    if (m)
        tbt_cc_rescue(sess, m);
}

/* (b) Fill unacknowledged holes with fresh FIFO records of the same wire
 * length. Runs with the socket lock and the carrier callback lock held. */
static void tbt_cc_shell_fill(struct sock *sk, struct tcp_sock *tp,
                              struct tbt_carrier *c, struct tbt_session *sess)
{
    struct sk_buff *skb;
    bool http = READ_ONCE(c->profile) != 3;
    int budget = TBT_CC_SHELL_PER_ACK;

    tbt_cc_mut_ack(c->mut_ring, READ_ONCE(tp->snd_una));
    for (skb = tcp_rtx_queue_head(sk);
         skb && skb != tp->highest_sack && budget > 0;
         skb = skb_rb_next(skb))
    {
        struct tcp_skb_cb *cb = TCP_SKB_CB(skb);
        struct tbt_fec_out *o;
        struct tbt_fec_hdr fh;
        struct tbt_cc_mut *old;
        u16 hdr;

        if (cb->sacked & (TCPCB_SACKED_ACKED | TCPCB_SACKED_RETRANS))
            continue;
        if (!skb->len || skb->len > 0xffff)
            continue;
        if (!tbt_cc_shell_usable(skb_cloned(skb), !skb_is_nonlinear(skb),
                                 skb->len,
                                 tbt_cc_mut_find(c->mut_ring, cb->seq,
                                                 (u16)skb->len) != NULL))
            continue;
        /* The claim returns a record only when its framed wire length equals
         * this skb's payload length and the payload still starts with the
         * matching record/chunk header. */
        o = tbt_fec_out_claim_shell(sess, skb->data, (u16)skb->len, http);
        if (!o)
            continue;
        if (tbt_fec_hdr_unpack(o->data, &fh) || fh.type != TBT_FEC_DATA)
        {
            /* The claim validated the framing; this cannot happen. Free the
             * queue reference without touching the skb. */
            tbt_fec_out_shell_done(sess, o);
            continue;
        }
        hdr = tbt_cc_wire_hdr_len(o->len, http);
        /* A slot about to be recycled may still name a record that has not
         * reached the wire. Rescue it before the entry is overwritten. */
        old = &c->mut_ring[c->mut_next];
        if (old->valid)
            tbt_cc_rescue(sess, old);
        memcpy(skb->data + hdr, o->data, o->len);
        if (skb->ip_summed != CHECKSUM_PARTIAL)
            skb->csum = csum_partial(skb->data, skb->len, 0);
        tbt_cc_mut_put(c->mut_ring, &c->mut_next, cb->seq, fh.block_id,
                       fh.reserved, fh.share_index, (u16)skb->len);
        atomic64_inc(&sess->car_shell_retrans_mutated);
        tbt_fec_out_shell_done(sess, o);
        budget--;
    }
}

/* Called at the end of tcp_ack() with the socket lock held. Everything below
 * is non-sleeping: the callback lock is held only while reading the carrier,
 * and the FEC helpers use spinlocks and GFP_ATOMIC. */
static void tbt_cc_in_ack_event(struct sock *sk, u32 flags)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct tbt_carrier *c;
    struct tbt_session *sess;

    (void)flags;
    read_lock_bh(&sk->sk_callback_lock);
    c = sk->sk_user_data;
    if (!c || !tbt_carrier_owns_sk(sk) || !c->sess)
        goto out;
    sess = c->sess;
    if (!READ_ONCE(sess->fec) || READ_ONCE(sess->fec_stopping) ||
        READ_ONCE(sess->closing))
        goto out;
    tbt_cc_dsack_rescue(tp, c, sess);
    tbt_cc_shell_fill(sk, tp, c, sess);
out:
    read_unlock_bh(&sk->sk_callback_lock);
}

/* Defensive: a socket that selected tbt_cc without carrying the private
 * carrier proto is not a tunnel carrier. Keep the classic AIMD response so an
 * accidental system-wide selection cannot apply the carrier control law to an
 * ordinary socket. tbt_carrier_owns_sk() is a correctness gate, not a
 * fairness mechanism. */
static void tbt_cc_foreign_avoid(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    u32 acked = rs->acked_sacked;

    if (!acked)
        return;
    if (tcp_in_slow_start(tp))
        tcp_slow_start(tp, acked);
    else
        tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked);
}

/* Pacing and window from the rate window and the peer's loss estimate. The
 * core calls this instead of the classic congestion-avoid path. */
static void tbt_cc_cong_control(struct sock *sk, u32 ack, int flag,
                                const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct tbt_carrier *c;
    struct tbt_session *sess;
    u32 srtt, mss, p_ppm, cwnd;
    u64 pacing, cap;

    (void)ack;
    (void)flag;
    read_lock_bh(&sk->sk_callback_lock);
    if (!tbt_carrier_owns_sk(sk))
    {
        /* Not a carrier socket: keep the classic window behaviour. */
        tbt_cc_foreign_avoid(sk, rs);
        goto out;
    }
    c = sk->sk_user_data;
    if (!c || !c->sess)
        goto out;
    sess = c->sess;
    if (sk->sk_pacing_status != SK_PACING_NEEDED)
        WRITE_ONCE(sk->sk_pacing_status, SK_PACING_NEEDED);
    srtt = tbt_cc_srtt_us(tp);
    mss = READ_ONCE(tp->mss_cache);
    c->cc.srtt_us = srtt;
    tbt_cc_bw_note(&c->cc, rs->delivered, mss, rs->interval_us,
                   rs->is_app_limited);
    if (!c->cc.max_bw)
        goto out;
    p_ppm = READ_ONCE(sess->cc_loss_ppm);
    c->cc.p_ppm = p_ppm;
    cap = tbt_cc_tunnel_cap(sess);
    pacing = tbt_cc_pacing_bps(c->cc.max_bw, p_ppm, TBT_CC_PACING_GAIN_PPM,
                               TBT_CC_PACE_FLOOR_BPS, cap);
    WRITE_ONCE(sk->sk_pacing_rate, pacing);
    cwnd = tbt_cc_cwnd_pkts(pacing, srtt, mss, TBT_CC_MIN_CWND_PKTS,
                            READ_ONCE(tp->snd_cwnd_clamp));
    tcp_snd_cwnd_set(tp, cwnd);
    /* Keep the threshold above the window: a SACK loss must not start a
     * classic slow start behind the controller's back. */
    WRITE_ONCE(tp->snd_ssthresh, cwnd);
out:
    read_unlock_bh(&sk->sk_callback_lock);
}

/* A SACK loss never shrinks the carrier window; the pacing rate carries the
 * path's capacity and the FEC layer repairs the record. A foreign socket gets
 * the stock Reno response instead. */
static u32 tbt_cc_ssthresh(struct sock *sk)
{
    if (!tbt_carrier_owns_sk(sk))
        return tcp_reno_ssthresh(sk);
    return READ_ONCE(tcp_sk(sk)->snd_cwnd);
}

static u32 tbt_cc_undo_cwnd(struct sock *sk)
{
    if (!tbt_carrier_owns_sk(sk))
        return tcp_reno_undo_cwnd(sk);
    return READ_ONCE(tcp_sk(sk)->snd_cwnd);
}

static void tbt_cc_cwnd_event(struct sock *sk, enum tcp_ca_event ev)
{
    (void)sk;
    /* CA_EVENT_CWND_RESTART is the idle restart that BBR collapses to the
     * initial window. tbt_carrier.c's peripheral hold restores the saved
     * window; the controller must not fight it. No other event changes the
     * window either: loss is not congestion here. */
    if (ev == CA_EVENT_CWND_RESTART)
        return;
}

static struct tcp_congestion_ops tbt_cc_ops = {
    .name = TBT_CC_NAME,
    .owner = THIS_MODULE,
    .cong_control = tbt_cc_cong_control,
    .ssthresh = tbt_cc_ssthresh,
    .undo_cwnd = tbt_cc_undo_cwnd,
    .cwnd_event = tbt_cc_cwnd_event,
    .in_ack_event = tbt_cc_in_ack_event,
};

/* Called from tbt_core.c after the FEC workqueue exists and before any
 * carrier socket can be created. */
int tbt_cc_register(void)
{
    int err = tcp_register_congestion_control(&tbt_cc_ops);

    if (err)
        pr_err("tbtun: cannot register the %s congestion control: %d\n",
               TBT_CC_NAME, err);
    return err;
}

/* Called after every tunnel is torn down, so no socket can still hold these
 * ops. */
void tbt_cc_unregister(void)
{
    tcp_unregister_congestion_control(&tbt_cc_ops);
}
