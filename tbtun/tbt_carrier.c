/* tbt_carrier.c - carrier TCP connections, their container, and their health.
 *
 * C3 scope: one session owns up to `n` carriers, indexed 0..n-1 by the frame
 * header's path_id. A carrier is a plain TCP connection whose byte stream is a
 * sequence of `32-byte header | payload` records; the first record in each
 * direction is HELLO, which carries the session nonce and the carrier index.
 *
 * Owns:
 *   - creating a near-side carrier: sock_create_kern, bind from the port
 *     block, kernel_connect, then HELLO, all from a work item because
 *     kernel_connect blocks.
 *   - adopting a far-side carrier: the accepted socket becomes a provisional
 *     carrier until its HELLO names a slot (DESIGN.md §9.5).
 *   - writing records with tcp_sendmsg_locked through a private struct proto,
 *     so a carrier never needs a kprobe and never exposes a userspace API.
 *   - draining carrier input with tcp_read_sock and feeding tbt_rx.
 *   - liveness: a 50 ms poll that re-arms queues and detects a dead or
 *     ACK-stalled carrier, then asks the session's replacement worker to dial
 *     a new one at the same index (DESIGN.md §9.7).
 *
 * Must never:
 *   - run kernel_connect or any sleeping call from sk_data_ready (softirq).
 *   - free a carrier object before session teardown. A work item scheduled
 *     just before `stopping` was set may still run; it must find valid memory
 *     and return early (see tbt_carrier_retire).
 *   - let a failed connect leak a socket or a port-block bit.
 *   - assume the whole record fits in one read; the parser state is per
 *     carrier.
 */
#include <linux/ktime.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"

/* A connect that has not completed inside this window is retried by w_poll.
 * The kernel default is the full SYN retry schedule (~130 s), which a session
 * teardown would inherit: tbt_carriers_stop() cancels w_connect, and flush
 * must return in bounded time even when a carrier is dialling an unreachable
 * peer. 1 s is above any ordinary LAN/WAN SYN round trip and below the
 * operator-visible flush budget. */
#define TBT_CONNECT_TIMEOUT_MS 1000

// A private copy of tcp_prot so carriers carry our sendmsg/recvmsg. Same shape
// as the deleted brutal_sockopt.c used, for a different reason: there we wanted
// a setsockopt hook, here we want the TX/RX to be ours without a kprobe.
static struct proto tbt_tcp_prot __ro_after_init;

static void tbt_car_connect_work(struct work_struct *w);
static int tbt_carrier_send(struct tbt_carrier *c, const void *data, size_t len);
static void tbt_pump_work(struct work_struct *w);

/* The effective container for a slot. The M spares of an http-kal tunnel use
 * the drip cadence: same container family, cheaper keep-alive. Both ends derive
 * it from the index, and HELLO carries the result so a mismatch is refused. */
u8 tbt_carrier_profile(const struct tbt_session *sess, u8 index, bool spare)
{
    u8 p = sess->tunnel->cfg.profile;

    (void)index;
    if (p == 1 && spare)
        p = 2;
    return p;
}

static void tbt_carrier_install_proto(struct sock *sk)
{
    sk->sk_prot = &tbt_tcp_prot;
}

static void tbt_car_ready(struct sock *sk)
{
    struct tbt_carrier *c;

    read_lock_bh(&sk->sk_callback_lock);
    c = sk->sk_user_data;
    if (!c || atomic_read(&c->stopping))
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    atomic64_inc(&c->sess->rx_ready_calls);
    schedule_work(&c->w_rx);
    read_unlock_bh(&sk->sk_callback_lock);
}

/* The carrier's window opened. tbt_tx_pump stops taking frames once
 * sk_stream_memory_free() is false (the socket's sndbuf is full) and has no
 * other way to learn that it drained; without this the stream stalls after
 * the first buffer-full. The callback runs in softirq, so it only queues
 * work; the pump's 2 ms w_pump is the bounded fallback. */
static void tbt_car_write_space(struct sock *sk)
{
    struct tbt_carrier *c;

    read_lock_bh(&sk->sk_callback_lock);
    c = sk->sk_user_data;
    if (!c)
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    atomic64_inc(&c->sess->carrier_ws_calls);
    if (atomic_read(&c->stopping))
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    atomic64_inc(&c->sess->carrier_write_space_runs);
    /* The pump armed SOCK_NOSPACE when it stopped at its high-water mark.
     * Clear it once the queue is back below that mark; otherwise every ACK
     * would keep calling us for nothing. */
    if (sk->sk_socket && sk_stream_memory_free(sk))
        clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

    /* A pump that called tcp_sendmsg_locked() may be asleep in
     * sk_stream_wait_memory() on sk_sleep(). Stock sk_stream_write_space would
     * wake it; it is not exported, so do the same wake here. */
    if (sk->sk_socket)
    {
        struct socket_wq *wq;

        rcu_read_lock();
        wq = rcu_dereference(sk->sk_wq);
        if (skwq_has_sleeper(wq))
            wake_up_interruptible_poll(&wq->wait,
                                       EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND);
        rcu_read_unlock();
    }
    schedule_work(&c->w_tx);
    read_unlock_bh(&sk->sk_callback_lock);
}

/* A carrier never sends FIN; any state change means the connection is gone
 * (peer close, RST, or an error). Check it promptly instead of waiting for the
 * 50 ms poll. */
static void tbt_car_state_change(struct sock *sk)
{
    struct tbt_carrier *c;

    read_lock_bh(&sk->sk_callback_lock);
    c = sk->sk_user_data;
    if (!c || atomic_read(&c->stopping))
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    schedule_delayed_work(&c->w_poll, 0);
    read_unlock_bh(&sk->sk_callback_lock);
}

/* Correctness wake for a carrier whose TCP send memory is full. The kernel's
 * tcp_check_space() is only called from tcp_ack when send data is pending, so
 * once the pump has handed every frame to TCP and the queue is empty, an ACK
 * that frees memory does not call sk_write_space. A short timer, armed only
 * while frames are queued and a carrier is full, is the wake that exists; the
 * write-space callback stays as the cheap fast path.
 *
 * `tx_queued` counts payload bytes, so a pinned HELLO/OPEN/FIN/RST (body_len
 * zero) adds nothing to it.  A RST left alone in the queue after the stream's
 * data frames were purged therefore did not wake this timer at all: the frame
 * sat until some unrelated sk_write_space, which a quiet carrier never
 * produces.  `ctl_queued` is the pinned-frame count, incremented in
 * tbt_tx_control_pinned() and decremented on write/failure/drop (and zeroed
 * with the whole queue in tbt_tx_free_queue), so it is the missing term. */
static void tbt_pump_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(to_delayed_work(w), struct tbt_carrier, w_pump);
    struct tbt_session *sess = c->sess;

    if (!sess || !sess->tunnel || READ_ONCE(sess->closing))
        return;
    if (atomic_read(&c->stopping))
        return;
    atomic64_inc(&sess->pump_hw_fired);
    if (READ_ONCE(sess->tx_queued) || READ_ONCE(sess->ctl_queued))
        tbt_tx_pump(sess);
}

static void tbt_car_tx_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(w, struct tbt_carrier, w_tx);

    atomic64_inc(&c->sess->tx_work_runs);
    /* The pump also writes HELLO/OPEN, so a connected carrier is enough; `up`
     * only gates DATA and would deadlock the handshake if required here. */
    if (atomic_read(&c->stopping) || !atomic_read(&c->connected))
        return;
    tbt_tx_pump(c->sess);
}

/* A peer that is not reading cannot advance snd_una; the same rule
 * tbt_tx_stall_check() applies to the stream timer.  Scan the live streams so
 * a per-stream pause is seen even when the session mirror has moved on. */
static bool tbt_car_paused(struct tbt_session *sess)
{
    struct tbt_stream *st;
    bool paused = READ_ONCE(sess->peer_paused);
    int i;

    if (paused)
        return true;
    rcu_read_lock();
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry_rcu(st, &sess->streams[i], node)
        {
            if (READ_ONCE(st->peer_paused) || READ_ONCE(st->paused))
            {
                paused = true;
                break;
            }
        }
        if (paused)
            break;
    }
    rcu_read_unlock();
    return paused;
}

/* C2/C3 liveness net. The event-driven path is the normal driver, but a wake
 * can be coalesced with a run that then finds nothing, or lost against a pause
 * transition; a queue then sits full with no scheduled reader. Re-check the
 * three queues every 50 ms and re-arm the work item that has something to do.
 * It never copies and never takes a socket lock.
 *
 * It is also the carrier's health check: a closed socket, or unacknowledged
 * data that has not moved for carrier_stall_ms, marks the carrier lost and
 * asks for a replacement. A provisional carrier that never presents a HELLO
 * is retired when its join deadline expires.
 *
 * Deletion condition: remove the queue re-arm when probes B/C/D and T7 record
 * poll_rescues = 0; the health check has no other driver. */
static void tbt_car_poll_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(to_delayed_work(w), struct tbt_carrier, w_poll);
    struct tbt_session *sess = c->sess;
    struct tbt_stream *st;
    struct tcp_sock *tp;
    struct sock *sk;
    u64 now = ktime_get_ns();
    bool rescued = false;
    int i;

    if (!sess || !sess->tunnel || READ_ONCE(sess->closing))
        return;
    if (atomic_read(&c->stopping))
    {
        schedule_work(&sess->w_replace);
        return;
    }
    if (c->provisional)
    {
        struct sock *psk = READ_ONCE(c->sk);

        if (now > c->join_deadline_ns)
        {
            atomic_set(&c->stopping, 1);
            schedule_work(&sess->w_replace);
            return;
        }
        /* The one-shot w_rx may have run before HELLO was queued.  The poll
         * is the only rescue while the carrier is provisional; without this
         * check a lost wake leaves HELLO unread until the join deadline.
         * parser->have covers a HELLO whose bytes were copied out but whose
         * record could not be parsed yet (copied_seq == rcv_nxt). */
        if (psk && READ_ONCE(psk->sk_state) == TCP_ESTABLISHED &&
            (READ_ONCE(tcp_sk(psk)->copied_seq) !=
                 READ_ONCE(tcp_sk(psk)->rcv_nxt) ||
             c->parser->have))
            schedule_work(&c->w_rx);
        schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
        return;
    }
    if (!atomic_read(&c->connected))
    {
        /* The connect failed (or has not been attempted yet). Retry from here:
         * the session may have been created before the far listener was up. */
        if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && now >= c->retry_at_ns)
        {
            c->retry_at_ns = now + 200 * NSEC_PER_MSEC;
            schedule_work(&c->w_connect);
        }
        schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
        return;
    }

    sk = READ_ONCE(c->sk);
    if (!sk)
    {
        tbt_carrier_lost(sess, c);
        return;
    }
    if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
    {
        tbt_carrier_lost(sess, c);
        return;
    }
    tp = tcp_sk(sk);
    {
        u32 una = READ_ONCE(tp->snd_una);
        u32 nxt = READ_ONCE(tp->snd_nxt);
        u32 wnd = READ_ONCE(tp->snd_wnd);
        u32 wmem = READ_ONCE(sk->sk_wmem_queued);
        bool progress = una != c->last_una;
        bool paused = tbt_car_paused(sess);
        /* Explicit carrier_stall_ms wins; auto is 3 x this carrier's RTO
         * (SRTT + 4 x RTTVAR, floored at 200 ms), itself floored at 3 s so a
         * brief scheduling delay cannot retire a healthy carrier. */
        u64 stall_ns;
        {
            u32 ms = sess->tunnel->cfg.carrier_stall_ms;

            if (ms)
            {
                stall_ns = (u64)ms * NSEC_PER_MSEC;
            }
            else
            {
                u32 srtt = READ_ONCE(tp->srtt_us) >> 3;
                u32 mdev = READ_ONCE(tp->mdev_us);
                u64 rto = max_t(u64, (u64)srtt + 4ULL * mdev, 200000ULL);

                stall_ns = max_t(u64, 3 * rto * NSEC_PER_USEC,
                                 3000ULL * NSEC_PER_MSEC);
            }
        }
        /* Start of the current "unacked bytes, no progress, window open"
         * interval; c->last_una_ns carries it between polls. */
        u64 stall_since_ns = c->last_una_ns;

        if (progress)
        {
            c->last_una = una;
            /* Real progress, not a backpressure refresh: this is the only
             * evidence that a carrier is still delivering our bytes, and
             * tbt_tx reads it for retransmit pacing. */
            c->last_una_move_ns = now;
        }
        c->last_nxt = nxt;

        /* Stall clock. An idle carrier cannot stall: with una == nxt nothing
         * is on the wire. Refreshing the clock here is what the old code
         * missed; without it the whole idle gap before the next burst counted
         * as stall time, and the first DATA frame after the gap killed a
         * healthy carrier (measured on the WAN: lost = n+m, replaced = n+m-1,
         * holes_unrecoverable = 0; http-kal lost = 0).
         *
         * carrier_stall_ms must exceed several RTTs plus the feedback
         * cadence: an ACK cannot arrive before one RTT, and the uack/FBK
         * feedback shares the ordered stream with the data. The 3000 ms
         * default is ~13 RTT on the measured 218 ms WAN path; the WAN
         * experiment that exposed the false kills used 15000 ms.
         *
         * sk_wmem_queued cannot distinguish backpressure from loss: it counts
         * data TCP still holds for retransmission, so it is positive for any
         * carrier with unacked bytes, blackholed or merely flow-controlled.
         * The old `nxt == last_nxt && wmem > 0` test was therefore true for
         * every real stall and refreshed the clock forever, masking the loss
         * it was meant to detect. Only a delivery pause or a closed peer
         * window lets the poll tell backpressure from loss. */
        if (una == nxt)
        {
            stall_since_ns = now;
            atomic64_inc(&sess->stall_idle_skips);
        }
        else if (progress)
        {
            stall_since_ns = now;
        }
        else if (paused || !wnd)
        {
            /* Delivery backpressure, not loss: while the peer reports a full
             * queue (or our own delivery queue is full) no ack can move.
             * Restart the window so a resume gets a full carrier_stall_ms,
             * and count the deferred decision in stats. */
            stall_since_ns = now;
            atomic64_inc(&sess->stall_paused_skips);
        }
        else
        {
            if (!stall_since_ns)
                stall_since_ns = now;
            else if (now - stall_since_ns > stall_ns)
            {
                atomic64_inc(&sess->stall_declared);
                if (debug >= 2)
                    pr_info_ratelimited("tbtun: carrier stall idx=%u gen=%u una=%u nxt=%u "
                                        "wmem=%u snd_wnd=%u rcv_wnd=%u state=%u\n",
                                        c->index, c->gen, una, nxt, wmem, wnd,
                                        READ_ONCE(tp->rcv_wnd), READ_ONCE(sk->sk_state));
                tbt_carrier_lost(sess, c);
                return;
            }
        }
        c->last_una_ns = stall_since_ns;
    }

    /* Keep-alive cadence for the HTTP container. PAD is a real 38-byte chunk
     * carrying a PAD record; drip is the 6-byte filler of the spare cadence.
     * Both update last_tx_ns, and neither touches record framing. */
    if (c->profile != 3 && atomic_read(&c->up))
    {
        u32 drip_ms = sess->tunnel->cfg.drip_ms;
        u32 ka_ms = sess->tunnel->cfg.keepalive_ms;

        if (c->profile == 2 && tbt_http_need_drip(c->last_tx_ns, now, drip_ms))
        {
            char b[TBT_HTTP_DRIP_CHUNK_LEN];
            size_t n = tbt_http_drip_chunk(b, sizeof(b));

            if (n && !tbt_carrier_send(c, b, n))
            {
                c->last_tx_ns = ktime_get_ns();
                atomic64_inc(&sess->drip_tx);
            }
        }
        else if (tbt_http_need_pad(c->last_tx_ns, now, ka_ms) &&
                 !READ_ONCE(sess->tx_queued))
        {
            if (!tbt_tx_control_pinned(sess, NULL, c->index, TBT_F_PAD, 0, 0, NULL, 0,
                                       false, GFP_ATOMIC))
                atomic64_inc(&sess->pad_tx);
        }
    }
    if (READ_ONCE(tp->copied_seq) != READ_ONCE(tp->rcv_nxt))
    {
        schedule_work(&c->w_rx);
        rescued = true;
    }
    else if (c->parser->have)
    {
        /* The parser is holding a record that returned -EAGAIN: its bytes
         * were already copied out of the socket, so copied_seq == rcv_nxt and
         * neither sk_data_ready nor the check above will ever wake this work
         * item again. Without this retry the record (and every record behind
         * it, including FBK) waits for new data that may never come: the peer
         * is stalled on the very feedback that is queued behind it
         * (measured: probe J, far fbk_rx=0, tx_ack=0, parser stuck at
         * 98,304, TXSTALLKILL after 5 s; debug=3 passes only because the
         * extra logging slows the sender enough to keep bytes arriving). */
        schedule_work(&c->w_rx);
        rescued = true;
    }
    if (READ_ONCE(sess->tx_queued))
    {
        schedule_work(&c->w_tx);
        rescued = true;
    }
    rcu_read_lock();
    for (i = 0; i < TBT_STREAM_HT; i++)
        hlist_for_each_entry_rcu(st, &sess->streams[i], node)
            if (!list_empty(&st->rx_q))
            {
                schedule_work(&st->w_drain);
                rescued = true;
            }
    rcu_read_unlock();
    if (rescued)
        atomic64_inc(&sess->poll_rescues);
    schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
}

/* One non-EAGAIN receive error: count it and stop this carrier. Shared by the
 * actor and the pending-parse retry so the two cannot drift. */
static void tbt_car_rx_error(struct tbt_carrier *c, int err)
{
    atomic64_inc(&c->sess->rx_errs);
    c->sess->rx_last_err = err;
    if (err == -EALREADY)
    {
        /* A duplicate (nonce, index) join: the session already counts it as
         * session_join_dup. Close this carrier, not the session. */
        atomic_set(&c->stopping, 1);
        return;
    }
    /* A protocol error is a per-carrier event: mark the carrier dead and stop
     * reading it. The stream lives or dies by its own rules. */
    atomic64_inc(&c->sess->rx_parse_proto);
    atomic_set(&c->stopping, 1);
}

static int tbt_car_rx_actor(read_descriptor_t *desc, struct sk_buff *skb, unsigned int offset, size_t len)
{
    struct tbt_carrier *c = desc->arg.data;
    size_t used = 0;
    int err = 0;

    if (!c || atomic_read(&c->stopping))
        return 0;
    atomic64_inc(&c->sess->rx_drain_entries);
    atomic64_add(len, &c->sess->rx_drain_bytes);

    while (used < len)
    {
        size_t n = 0;

        err = tbt_rx_skb(c, skb, offset + used, len - used, &n);
        used += n;
        if (err)
            break;
    }
    if (err)
    {
        if (err == -EAGAIN)
        {
            /* The stream's delivery ring is full. The bytes that were copied
             * into the parser buffer are accounted for below; the record that
             * did not fit stays there for the resume. */
            desc->count += used;
            return used;
        }
        tbt_car_rx_error(c, err);
        return 0;
    }
    desc->count += used;
    return used;
}

static void tbt_car_rx_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(w, struct tbt_carrier, w_rx);
    read_descriptor_t desc = {};
    struct tbt_stream *st;
    struct sock *sk;
    int i, drained;

    atomic64_inc(&c->sess->rx_work_runs);
    sk = READ_ONCE(c->sk);
    if (atomic_read(&c->stopping) || !sk)
    {
        atomic64_inc(&c->sess->rx_work_early);
        return;
    }

    /* Pause rule (tbt_rx.c): if the app-facing queue is full, stop reading the
     * carrier. TCP then advertises a closed window and the far side slows
     * down; no byte is dropped. The sender's delivered-offset window keeps the
     * queue below this mark in normal operation, so this is a safety net.
     *
     * A provisional carrier has no stream data yet; its only job is to read
     * HELLO. Another stream's delivery backpressure must not hide it. */
    if (!c->provisional)
    {
        rcu_read_lock();
        for (i = 0; i < TBT_STREAM_HT; i++)
        {
            hlist_for_each_entry_rcu(st, &c->sess->streams[i], node)
            {
                if (READ_ONCE(st->paused) && READ_ONCE(st->sk))
                {
                    atomic64_inc(&c->sess->rx_paused_skips);
                    rcu_read_unlock();
                    return;
                }
            }
        }
        rcu_read_unlock();
    }
    desc.arg.data = c;

    atomic64_inc(&c->sess->rx_reads);
    lock_sock(sk);
    /* Retry a record an earlier -EAGAIN left in the parser before reading the
     * socket. Without this, the record is unreachable: its bytes are already
     * out of the socket (copied_seq == rcv_nxt), so neither sk_data_ready nor
     * the poll's copied_seq check will ever enter the actor again, and every
     * record behind it -- FBK included -- waits for data the peer cannot send
     * because it is waiting for that very feedback. -EAGAIN here means the
     * reason has not cleared; the 50 ms poll and the drain resume will retry. */
    {
        int perr = tbt_rx_parse_pending(c);

        if (perr == -EAGAIN)
        {
            release_sock(sk);
            return;
        }
        if (perr)
        {
            release_sock(sk);
            tbt_car_rx_error(c, perr);
            return;
        }
    }
    drained = tcp_read_sock(sk, &desc, tbt_car_rx_actor);
    release_sock(sk);
    if (drained > 0)
        atomic64_add(drained, &c->sess->rx_read_bytes);
    else
        atomic64_inc(&c->sess->rx_read_zero);
    c->sess->rx_last_drained = drained > 0 ? drained : 0;
    if (drained > 0)
        c->last_rx_ns = ktime_get_ns();

    /* The peer closed the connection: a carrier never sends FIN, so a zero
     * read with a non-established socket is the end of this path. */
    if (drained <= 0 && READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
    {
        tbt_carrier_lost(c->sess, c);
    }
    else if (READ_ONCE(c->sk) == sk &&
             READ_ONCE(tcp_sk(sk)->copied_seq) !=
                 READ_ONCE(tcp_sk(sk)->rcv_nxt))
    {
        /* Bytes arrived after tcp_read_sock() stopped but before this work
         * returned. Re-arm; sk_data_ready may have been coalesced.
         *
         * Never re-arm for parser->have alone: those bytes were already
         * copied out of the socket (copied_seq == rcv_nxt), so a re-run can
         * copy nothing. It spins at 100% of a kworker until the rest of the
         * record arrives, which is the normal case on a path whose segments
         * are smaller than a record. The 50 ms poll already retries a held
         * record, and sk_data_ready runs this work when the tail arrives. */
        schedule_work(&c->w_rx);
    }

    /* Parsing may have enqueued a control frame (OPEN, FBK). The enqueue
     * already asked w_tx to drain it; never pump from this work item, because
     * pumping the carrier whose lock this work item holds deadlocks against
     * the peer window that only this work item's next read can open. */
}

/* One non-blocking write of a byte range on a carrier. -EAGAIN means the
 * carrier's TCP buffer is full and the caller must retry later. */
static int tbt_carrier_send(struct tbt_carrier *c, const void *data, size_t len)
{
    struct msghdr msg = {};
    struct kvec iov = { .iov_base = (void *)data, .iov_len = len };
    int err;

    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, len);
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(c->sk);
    err = tcp_sendmsg_locked(c->sk, &msg, len);
    release_sock(c->sk);
    if (err < 0)
        return err;
    if ((size_t)err != len)
        return -EIO;
    return 0;
}

/* The request/response header block for this carrier segment. The near side
 * emits the §3.2 POST block (Host is the peer literal it dialled, X-Request-Id
 * is fresh per segment); the far side emits the §3.2 200 block with a Date.
 * A 0 return is a builder rejection and poisons the carrier: partial header
 * output must never reach the wire. */
static size_t tbt_http_header_build(struct tbt_carrier *c, char *buf, size_t cap)
{
    struct tbt_tunnel *t = c->sess->tunnel;

    if (t->cfg.role == TBT_ROLE_NEAR)
    {
        static const char b32[] = "abcdefghijklmnopqrstuvwxyz234567";
        struct tbt_http_cfg cfg = {};
        int i;

        snprintf(cfg.host, sizeof(cfg.host), "%pI4:%u", &t->cfg.peer_addr,
                 ntohs(t->cfg.peer_port));
        for (i = 0; i < TBT_HTTP_REQID_LEN; i++)
            cfg.request_id[i] = b32[get_random_u32() % 32];
        return tbt_http_req_header(buf, cap, &cfg);
    }
    else
    {
        struct tbt_http_resp_cfg cfg = { .unix_secs = ktime_get_real_seconds() };

        return tbt_http_resp_header(buf, cap, &cfg);
    }
}

/* Keep the size line and the first record bytes of the near side's carrier 0
 * chunk for /proc/net/tbtun/http. A snapshot, not a transcript: the length is
 * published after the bytes, so a reader that sees it has them. */
static void tbt_http_capture_chunk(struct tbt_session *sess, const char *chunk, size_t n)
{
    size_t cap = min_t(size_t, n, sizeof(sess->http_cap_chunk));

    memcpy(sess->http_cap_chunk, chunk, cap);
    WRITE_ONCE(sess->http_cap_chunk_len, (u16)cap);
}

// Write one whole record. Backpressure is the caller's problem: tbt_tx only
// calls this while sk_wmem_queued is below the high-water mark.
int tbt_carrier_xmit(struct tbt_session *sess, struct tbt_carrier *c, struct tbt_frame *fr)
{
    struct msghdr msg = {};
    struct kvec iov[2];
    u8 hdrbuf[TBT_HDR_LEN];
    size_t total;
    int err;

    if (!c || !c->sk)
        return -ENOTCONN;
    /* Debug-only deterministic loss (DESIGN 8.3 `carrier <n> drop_ppm`). Drop
     * the whole DATA record before it reaches the socket; a partial record
     * would desynchronise the peer's parser, so this is deliberately
     * all-or-nothing. Control records (HELLO, OPEN, FBK, FIN, RST) are exempt:
     * the injector exists to create reordering, and dropping a handshake would
     * turn a loss probe into a carrier-replacement probe. */
    if (c->drop_ppm && fr->hdr.type == TBT_F_DATA &&
        get_random_u32() % TBT_DROP_PPM_MAX < c->drop_ppm)
    {
        pr_warn_once("tbtun: dropping frames: debug_loss=1 and drop_ppm=%u\n", c->drop_ppm);
        c->dropped_by_ppm++;
        sess->dropped_by_ppm++;
        fr->dropped = true;
        c->drop_snd_nxt = READ_ONCE(tcp_sk(c->sk)->snd_nxt);
        /* The injector's drop is a real record loss and must stay in the hole
         * gate's evidence (sent_below - recv_cnt). Only *failed writes* are
         * excluded now; counting the attempt at the entry made every -EAGAIN
         * a permanent recv_lag (review 14). */
        c->data_frames++;
        /* The frame is gone as if the wire had lost it: report the write as
         * done so the sender records a real hole (sends, path, seq) and the
         * retransmit timer owns the repair. Returning -EAGAIN instead let the
         * pump quietly re-send on another path, so the injector produced no
         * hole at all and probe G had nothing to declare. The earlier false
         * "received" verdict came from the per-path high-water, not from this
         * return; the recv_lag check now scopes that proof to lossless paths. */
        return 0;
    }
    if (c->profile != 3)
    {
        /* HTTP/1.1 container: one chunk per record, with one request (near) or
         * response (far) header block at the start of the segment. The far
         * side must not answer before the request header has been parsed, so
         * its first write waits here. */
        struct tbt_hdr h = fr->hdr;
        struct tbt_http_out out;
        size_t n;

        if (!c->http_started)
        {
            /* 8 KiB on the stack trips -Wframe-larger-than; the pump is
             * sleepable, so allocate. */
            char *hdr = kmalloc(TBT_HTTP_REQ_MAX, GFP_KERNEL);
            size_t hlen;

            if (!hdr)
                return -ENOMEM;
            if (sess->tunnel->cfg.role == TBT_ROLE_FAR && !c->http_req_done)
            {
                kfree(hdr);
                return -EAGAIN;
            }
            hlen = tbt_http_header_build(c, hdr, TBT_HTTP_REQ_MAX);
            if (!hlen)
            {
                kfree(hdr);
                atomic64_inc(&sess->http_parse_err);
                return -EIO;
            }
            /* /proc/net/tbtun/http keeps the near side's carrier 0 segment:
             * one direction each, not a full duplex transcript. The length is
             * published after the bytes so a reader that sees it has them. */
            if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && c->index == 0)
            {
                size_t cap = min_t(size_t, hlen, sizeof(sess->http_cap_req));

                memcpy(sess->http_cap_req, hdr, cap);
                WRITE_ONCE(sess->http_cap_req_len, (u16)cap);
            }
            err = tbt_carrier_send(c, hdr, hlen);
            kfree(hdr);
            if (err)
                return err;
            c->http_started = true;
            c->last_tx_ns = ktime_get_ns();
            c->tx_bytes += hlen;
            sess->carrier_bytes += hlen;
            if (sess->tunnel->cfg.role == TBT_ROLE_NEAR)
                sess->http_req++;
            else
                sess->http_resp++;
        }
        h.body_len = fr->len;
        out.hdr = &h;
        out.body = fr->len ? fr->data : NULL;
        n = tbt_http_frame_encode(c->http_tx, TBT_HTTP_FRAME_MAX, &out);
        if (!n)
        {
            /* The encoder never emits partial output; a 0 is a bug or a short
             * buffer, and either way this carrier must not be trusted. */
            atomic64_inc(&sess->http_parse_err);
            return -EIO;
        }
        if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && c->index == 0)
            tbt_http_capture_chunk(sess, c->http_tx, n);
        err = tbt_carrier_send(c, c->http_tx, n);
        if (err)
            return err;
        c->tx_frames++;
        c->tx_bytes += n;
        if (fr->hdr.type == TBT_F_DATA)
            c->data_frames++;
        c->last_tx_ns = ktime_get_ns();
        sess->carrier_bytes += n;
        return 0;
    }
    /* hdrbuf is the header alone; the payload stays in its own iovec so this
     * never copies application bytes onto the stack. The header's body_len
     * tells the receiver where the record ends. */
    BUILD_BUG_ON(sizeof(hdrbuf) != TBT_HDR_LEN);
    total = tbt_raw_put_hdr(hdrbuf, &fr->hdr, fr->len) + fr->len;
    iov[0].iov_base = hdrbuf;
    iov[0].iov_len = TBT_HDR_LEN;
    iov[1].iov_base = fr->data;
    iov[1].iov_len = fr->len;

    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, iov, 2, total);
    /* Never sleep in the pump: a carrier whose TCP send buffer is full must
     * return to the caller so the other carriers can be used and
     * sk_write_space can drive the retry. A blocking send here serialised the
     * four carriers behind the slowest one. */
    /* A previous record's tail must reach the wire first: the peer parses a
     * byte stream, so a record may never be interleaved or reordered. */
    if (c->part_len)
    {
        while (c->part_off < c->part_len)
        {
            struct msghdr pm = {};
            struct kvec pkv;

            pkv.iov_base = c->part + c->part_off;
            pkv.iov_len = c->part_len - c->part_off;
            iov_iter_kvec(&pm.msg_iter, ITER_SOURCE, &pkv, 1, pkv.iov_len);
            pm.msg_flags = MSG_DONTWAIT;
            lock_sock(c->sk);
            err = tcp_sendmsg_locked(c->sk, &pm, pkv.iov_len);
            release_sock(c->sk);
            if (err < 0)
                return err == -EAGAIN ? -EAGAIN : err;
            if (!err)
                return -EAGAIN;
            c->part_off += err;
        }
        c->part_len = c->part_off = 0;
    }
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(c->sk);
    err = tcp_sendmsg_locked(c->sk, &msg, total);
    release_sock(c->sk);
    if (err == -EAGAIN)
        return -EAGAIN;
    if (err < 0)
    {
        sess->carrier_err++;
        return err;
    }
    if ((size_t)err != total)
    {
        /* Normal backpressure wrote a prefix. Keep the unwritten tail for the
         * next call; the record is on the wire in order and the carrier is
         * healthy. */
        size_t done = (size_t)err;
        size_t h = done < TBT_HDR_LEN ? done : TBT_HDR_LEN;
        size_t tail_h = TBT_HDR_LEN - h;
        size_t tail_d = total - done - tail_h;

        sess->carrier_short++;
        if (tail_h > TBT_REC_MAX || tail_d > TBT_REC_MAX - tail_h)
            return -EIO; /* impossible: total <= TBT_REC_MAX */
        if (tail_h)
            memcpy(c->part, hdrbuf + h, tail_h);
        /* The payload tail starts where the write stopped, not at fr->data:
         * a short write that consumed part of the payload would otherwise
         * duplicate the first (done - TBT_HDR_LEN) bytes and drop the last
         * as many (measured: probe I, return records at seq 1.22-1.28 MB had
         * the right length and the wrong content; the far short-wrote 4154
         * times, the near none). */
        if (tail_d)
            memcpy(c->part + tail_h, fr->data + (done - h), tail_d);
        c->part_len = (u16)(tail_h + tail_d);
        c->part_off = 0;
    }
    c->tx_frames++;
    c->tx_bytes += total;
    if (fr->hdr.type == TBT_F_DATA)
        c->data_frames++;
    c->last_tx_ns = ktime_get_ns();
    sess->carrier_bytes += total;
    return 0;
}

// ---------------------------------------------------------------------------
// Carrier lifecycle

struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c;

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c)
        return NULL;
    /* Before the first allocation that can fail: the failure path frees
     * through tbt_carrier_free(), which unlinks the node, and an all-zero
     * node is not a valid list head. */
    INIT_LIST_HEAD(&c->node);
    c->http_tx = kmalloc(TBT_HTTP_FRAME_MAX, GFP_KERNEL);
    c->http_hdr = kmalloc(TBT_HTTP_HDR_MAX, GFP_KERNEL);
    c->part = kmalloc(TBT_REC_MAX, GFP_KERNEL);
    c->parser = kmalloc(sizeof(*c->parser), GFP_KERNEL);
    c->http_rx = kmalloc(sizeof(*c->http_rx), GFP_KERNEL);
    if (!c->http_tx || !c->http_hdr || !c->part || !c->parser || !c->http_rx)
    {
        /* ctor-fail: never linked, so the helper's unlink is a no-op. */
        tbt_carrier_free(c);
        return NULL;
    }
    c->sess = sess;
    c->index = index;
    c->gen = ++sess->carrier_gen;
    /* 0xff is the provisional far-side index: not a spare, and the join sets
     * the real role when HELLO names the slot. */
    c->spare = index < sess->n_car && index >= sess->n_pri;
    c->active = !c->spare;
    c->profile = tbt_carrier_profile(sess, index, c->spare);
    c->state = TBT_CAR_CONNECTING;
    c->class = sess->tunnel ? sess->tunnel->cfg.profile : 1;
    c->join_deadline_ns = ktime_get_ns() + (u64)TBT_JOIN_TIMEOUT_MS * NSEC_PER_MSEC;
    c->last_una_move_ns = ktime_get_ns();
    INIT_WORK(&c->w_connect, tbt_car_connect_work);
    INIT_WORK(&c->w_rx, tbt_car_rx_work);
    INIT_WORK(&c->w_tx, tbt_car_tx_work);
    INIT_DELAYED_WORK(&c->w_poll, tbt_car_poll_work);
    INIT_DELAYED_WORK(&c->w_pump, tbt_pump_work);
    return c;
}

/* Mark a carrier unusable once, count it, and ask the replacement worker to
 * put a new one at the same index. Safe from work, softirq and /proc context;
 * it never sleeps. */
void tbt_carrier_lost(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!sess || !c || READ_ONCE(sess->closing) || atomic_read(&c->stopping))
        return;
    atomic_set(&c->stopping, 1);
    c->state = TBT_CAR_STALLED;
    if (!c->lost_counted)
    {
        c->lost_counted = true;
        atomic64_inc(&sess->carrier_lost);
    }
    /* Tell the far to free this (index, gen) before the replacement dials.
     * Without it the replacement's HELLO hits join_dup and the two sides
     * storm (measured: 15 join_dup, 191 replacements, 32 s receiver hole). */
    if (sess->tunnel && sess->tunnel->cfg.role == TBT_ROLE_NEAR)
    {
        int j;

        for (j = 0; j < sess->n_car; j++)
        {
            struct tbt_carrier *live = READ_ONCE(sess->car[j]);
            u8 idx = c->index;

            if (!live || live == c || !atomic_read(&live->up) ||
                atomic_read(&live->stopping))
                continue;
            tbt_tx_control_pinned(sess, NULL, (u8)j, TBT_F_RETIRE, 0,
                                  (u64)(c->gen & 0xf), &idx, 1, true,
                                  GFP_ATOMIC);
            break;
        }
    }
    /* Frames last sent here are re-sent now, not after the retransmit timer:
     * a dead path is a known loss, not a suspected one. */
    tbt_tx_path_lost(sess, c->index);
    /* A join_dup storm is a retirement that has not landed; retry slowly and
     * fail the stream cleanly after a bounded number of rounds. */
    if (sess->join_retries < U8_MAX)
        sess->join_retries++;
    if (sess->join_retries > 20)
    {
        struct tbt_stream *snap[TBT_STREAM_SNAP_MAX];
        int n, i;

        n = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
        for (i = 0; i < n; i++)
        {
            tbt_stream_kill(snap[i], TBT_ST_DEAD);
            tbt_stream_put(snap[i]);
        }
        return;
    }
    c->retry_at_ns = ktime_get_ns() + 50 * NSEC_PER_MSEC;
    schedule_work(&sess->w_replace);
}

/* Release everything a carrier owns: its socket, its port and its parser
 * state. The struct stays where the caller put it, and c->node is untouched,
 * so a caller may walk a list and call this for every element. Idempotent.
 * Called with no lock held; it cancels work and therefore sleeps. */
static void tbt_carrier_release(struct tbt_session *sess, struct tbt_carrier *c)
{
    struct socket *s;

    if (!c)
        return;
    atomic_set(&c->stopping, 1);
    cancel_delayed_work_sync(&c->w_poll);
    cancel_work_sync(&c->w_connect);
    cancel_work_sync(&c->w_rx);
    /* w_tx and w_pump take pump_lock; callers of retire may hold it
     * (carriers_start/carriers_stop). Those callers cancel both before the
     * lock. */

    s = c->sock;
    if (s)
    {
        if (s->sk)
        {
            /* Restore the core callbacks.  An accepted carrier inherited
             * tbt_ready(); a dialled one was replaced in w_connect.  Use the
             * session snapshot, never an accepted socket's inherited
             * pointer. */
            tbt_sk_ops_restore(s->sk, &sess->def_ops);
            /* tbt_tcp_prot lives in module data.  A socket left in FIN_WAIT
             * would call sk->sk_prot->destroy() after unload. */
            sock_replace_proto(s->sk, &tcp_prot);
            /* A callback may have queued w_rx before the restore. */
            cancel_work_sync(&c->w_rx);
        }
        tbt_sock_abort_on_close(s);
        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        c->sock = NULL;
        c->sk = NULL;
    }
    atomic_set(&c->connected, 0);
    atomic_set(&c->up, 0);
    if (c->parser->have)
        sess->recv_partial_discard++;
    c->parser->have = 0;
    if (c->local_port)
    {
        tbt_port_free(c->local_port);
        c->local_port = 0;
    }
}

/* Move a carrier's cumulative counters into the session's retired totals,
 * once. A retired slot leaves sess->car[], so /proc/net/tbtun/peers stops
 * listing it; without this the session aggregate could no longer be
 * reconciled with the live rows. The caller serialises with the pump (retire
 * runs under pump_lock or after the pump that owned the slot finished), so
 * the values are final when this reads them. */
void tbt_carrier_stats_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!sess || !c)
        return;
    if (test_and_set_bit(0, &c->stats_retired))
        return;
    atomic64_add(READ_ONCE(c->tx_bytes), &sess->retired_tx_bytes);
    atomic64_add(READ_ONCE(c->data_frames), &sess->retired_data_frames);
    atomic64_add(READ_ONCE(c->retx_data), &sess->retired_retx_data);
}

/* Release the carrier and park the struct on sess->dead until teardown: a
 * work item scheduled just before `stopping` was set may still run, and it
 * must find valid memory (it checks stopping and returns). Called with no
 * lock held; it cancels work and releases sockets. */
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!c)
        return;
    tbt_carrier_stats_retire(sess, c);
    tbt_carrier_release(sess, c);
    spin_lock(&sess->lock);
    list_del_init(&c->node);
    list_add_tail(&c->node, &sess->dead);
    spin_unlock(&sess->lock);
}

/* Free a carrier that is on no session list and unreachable from every work
 * item. The node is unlinked first (a carrier that was never installed is
 * self-linked, which is also a valid no-op), so the caller cannot leave a
 * freed node on a list. */
void tbt_carrier_free(struct tbt_carrier *c)
{
    if (!c)
        return;
    list_del_init(&c->node);
    kfree(c->http_tx);
    kfree(c->http_hdr);
    kfree(c->part);
    kfree(c->parser);
    kfree(c->http_rx);
    /* retire-drain */
    kfree(c);
}

/* Turn a port-allocation failure into an operator-visible reason.  The
 * carrier stays alive and retries; nothing is killed.  The warning is
 * ratelimited because the retry loop runs every 200 ms. */
static void tbt_port_alloc_note(struct tbt_session *sess, u8 index,
                                enum tbt_port_alloc_err why)
{
    u16 lo, hi;
    bool configured;

    switch (why)
    {
    case TBT_PORT_ALLOC_EXHAUSTED:
        atomic64_inc(&sess->port_exhaust);
        tbt_portmap_get(&lo, &hi, &configured);
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u: source port block %u-%u is exhausted; carrier retries\n",
                            sess->tunnel->cfg.id, index, lo, hi);
        break;
    case TBT_PORT_ALLOC_NOMEM:
        atomic64_inc(&sess->port_alloc_fail);
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u: cannot allocate the source port bitmap; carrier retries\n",
                            sess->tunnel->cfg.id, index);
        break;
    default:
        break;
    }
}

/* The near side dials n carriers. The far side dials none: its carriers are
 * accepted and join the session by HELLO. */
int tbt_carriers_start(struct tbt_tunnel *t, struct tbt_session *sess)
{
    int i;

    if (t->cfg.role != TBT_ROLE_NEAR)
        return 0;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_new(sess, (u8)i);

        if (!c)
        {
            atomic64_inc(&sess->carrier_alloc_fail);
            return -ENOMEM;
        }
        sess->car[i] = c;
        schedule_work(&c->w_connect);
    }
    return 0;
}

/* Replacement worker: for every dead slot, dial a fresh carrier at the same
 * index (near side), or clear the slot and wait for the peer's new carrier to
 * join by HELLO (far side). The swap is serialised with the pump, so no pump
 * can be using the old carrier when it is retired. The new port is allocated
 * while the old carrier still holds its own, so the replacement never reuses
 * the dead 4-tuple (a TIME_WAIT tuple makes connect fail with EADDRNOTAVAIL);
 * the old socket is released before the new connect starts. */
void tbt_replace_work(struct work_struct *w)
{
    struct tbt_session *sess = container_of(w, struct tbt_session, w_replace);
    struct tbt_carrier *retire[TBT_MAX_CARRIERS_LIMIT];
    struct tbt_carrier *start[TBT_MAX_CARRIERS_LIMIT];
    struct tbt_carrier *c, *tmp;
    LIST_HEAD(victims);
    u64 now = ktime_get_ns();
    int nret = 0, nstart = 0, i;

    if (!sess->tunnel || READ_ONCE(sess->closing))
        return;
    mutex_lock(&sess->pump_lock);
    for (i = 0; i < sess->n_car; i++)
    {
        c = sess->car[i];
        if (!c || !atomic_read(&c->stopping))
            continue;
        /* A dead primary is covered by an idle spare first: that is the whole
         * point of M, and it costs no new dial and no new 5-tuple. */
        if (i < sess->n_pri)
        {
            int j;

            for (j = sess->n_pri; j < sess->n_car; j++)
            {
                struct tbt_carrier *sp = sess->car[j];

                if (!sp || sp->active || atomic_read(&sp->stopping) ||
                    !atomic_read(&sp->up))
                    continue;
                sp->active = true;
                sp->state = TBT_CAR_UP;
                sess->spare_activated++;
                sess->car[i] = NULL;
                goto retire_one;
            }
        }
        if (sess->tunnel->cfg.role == TBT_ROLE_NEAR)
        {
            struct tbt_carrier *nc = tbt_carrier_new(sess, (u8)i);
            enum tbt_port_alloc_err perr = TBT_PORT_ALLOC_OK;

            if (nc)
                nc->local_port = tbt_port_alloc(&perr);
            if (nc && nc->local_port)
            {
                sess->car[i] = nc;
                atomic64_inc(&sess->carrier_replaced);
                start[nstart++] = nc;
            }
            else
            {
                if (nc)
                {
                    tbt_port_alloc_note(sess, (u8)i, perr);
                    tbt_carrier_free(nc);
                }
                sess->car[i] = NULL;
                atomic64_inc(&sess->carrier_alloc_fail);
            }
        }
        else
        {
            /* The far side never dials; the near side's replacement joins. */
            sess->car[i] = NULL;
        }
retire_one:
        retire[nret++] = c;
    }
    spin_lock(&sess->lock);
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        if (atomic_read(&c->stopping) || now > c->join_deadline_ns)
            list_move_tail(&c->node, &victims);
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->pump_lock);

    for (i = 0; i < nret; i++)
    {
        cancel_work_sync(&retire[i]->w_tx);
        cancel_delayed_work_sync(&retire[i]->w_pump);
        tbt_carrier_retire(sess, retire[i]);
    }
    list_for_each_entry_safe(c, tmp, &victims, node)
    {
        list_del_init(&c->node);
        cancel_work_sync(&c->w_tx);
        cancel_delayed_work_sync(&c->w_pump);
        tbt_carrier_retire(sess, c);
    }
    /* Carriers the join path replaced: release their sockets and ports now,
     * not at session teardown. tbt_carrier_release is idempotent and leaves
     * c->node alone, so this walk cannot re-add an entry to the list it is
     * walking (retire() would, and the walk would then never end). */
    list_for_each_entry(c, &sess->dead, node)
        tbt_carrier_release(sess, c);
    /* Dial only after every old socket (and its port) is gone. */
    for (i = 0; i < nstart; i++)
        schedule_work(&start[i]->w_connect);
    if (nret)
        tbt_tx_wake(sess);
}

void tbt_carriers_stop(struct tbt_session *sess)
{
    struct tbt_carrier *c, *tmp;
    int i;

    /* Stop every writer before any socket goes away: a pump runs outside its
     * own carrier's work item, so retiring one carrier while another's w_tx is
     * writing to it would use a released socket. cancelling w_tx cannot be
     * done while holding pump_lock (the work waits on it), so do it first. */
    for (i = 0; i < TBT_MAX_CARRIERS_LIMIT; i++)
    {
        c = sess->car[i];
        if (!c)
            continue;
        atomic_set(&c->stopping, 1);
        cancel_work_sync(&c->w_tx);
        cancel_delayed_work_sync(&c->w_pump);
    }
    list_for_each_entry(c, &sess->prov, node)
    {
        atomic_set(&c->stopping, 1);
        cancel_work_sync(&c->w_tx);
        cancel_delayed_work_sync(&c->w_pump);
    }

    mutex_lock(&sess->pump_lock);
    for (i = 0; i < TBT_MAX_CARRIERS_LIMIT; i++)
    {
        if (sess->car[i])
        {
            c = sess->car[i];
            sess->car[i] = NULL;
            tbt_carrier_retire(sess, c);
        }
    }
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        list_del_init(&c->node);
        tbt_carrier_retire(sess, c);
    }
    mutex_unlock(&sess->pump_lock);

    list_for_each_entry_safe(c, tmp, &sess->dead, node)
    {
        /* retire() re-links the node onto sess->dead; the free helper unlinks
         * it again, so the list never keeps a freed node. */
        tbt_carrier_retire(sess, c);
        /* A w_poll that passed its stopping check can queue w_tx after the
         * pre-lock cancel.  Nothing can queue it now; cancel before free. */
        cancel_work_sync(&c->w_tx);
        cancel_delayed_work_sync(&c->w_pump);
        tbt_carrier_free(c);
    }
    /* Callbacks installed on sockets may still be called from softirq paths
     * that took their reference before sk_user_data was cleared. */
    synchronize_rcu();
}

/* A slot is usable for a joining carrier when it is empty or its current
 * occupant is already dead. The join path uses this instead of a plain NULL
 * test: the near side's replacement can connect before the far side's 50 ms
 * poll has noticed the old socket close. */
bool tbt_carrier_dead(struct tbt_carrier *c)
{
    struct sock *sk;

    if (!c)
        return true;
    if (atomic_read(&c->stopping))
        return true;
    sk = READ_ONCE(c->sk);
    if (!sk)
        return true;
    return READ_ONCE(sk->sk_state) != TCP_ESTABLISHED;
}

/* Socket buffer budget for one carrier. The kernel doubles SO_SNDBUF and
 * SO_RCVBUF, so pass half of the byte budget: one carrier then holds its
 * share of the sender window (tx_window_bytes, or rx_reorder_bytes while the
 * window is auto) plus room for two maximum records.
 *
 * The buffer is not the queueing delay any more. The delay the peer sees is
 * in-flight bytes divided by the rate, and the sender window bounds in-flight
 * to tx_window_bytes; a buffer large enough to hold one carrier's share adds
 * no delay. Before the window existed the pump could fill a 1 MiB buffer per
 * carrier and push the uack/FBK feedback 2-4 s behind the write (measured:
 * probe I interleaved, uack frozen at 46,142,674 for 4.5 s while the peer had
 * already received 47,129,015). The old fixed 256 KiB reached the kernel as
 * ~512 KiB of unacked bytes per carrier, which capped a 1-8 MiB window.
 *
 * The buffer must be set before the TCP handshake, not after: the SYN/SYN-ACK
 * fixes the window scale and window_clamp from the socket's receive buffer,
 * and raising sk_rcvbuf later cannot widen them. That is why the far listener
 * (which answers every SYN-ACK) and the dialled socket before kernel_connect
 * both call this. */
void tbt_car_setsockbuf(struct socket *s, const struct tbt_cfg *cfg, u16 n_pri)
{
    u32 win = cfg->tx_window_bytes;
    u32 per;
    int v;

    if (!win)
        win = cfg->rx_reorder_bytes;
    per = win / (n_pri ? n_pri : 1) + 2 * TBT_CHUNK_MAX;
    /* SO_SNDBUF/SO_RCVBUF double the value in the kernel: pass half the byte
     * budget we want, so one carrier can hold its share of the window. */
    v = clamp_t(u32, per / 2, 128 * 1024, 4 * 1024 * 1024);

    /* The plain option is capped at net.core.wmem_max/rmem_max (208 KiB on a
     * default boot), so a 256 KiB request reached the kernel as 208 KiB and
     * 2 x 208 KiB was the most a carrier could hold -- less than one BDP on
     * the 218 ms WAN. The FORCE option needs CAP_NET_ADMIN, which the
     * module's workqueue has in the init namespace; a namespace that lacks it
     * falls back to the capped option rather than failing the carrier. */
    if (sock_setsockopt(s, SOL_SOCKET, SO_SNDBUFFORCE, KERNEL_SOCKPTR(&v), sizeof(v)))
        sock_setsockopt(s, SOL_SOCKET, SO_SNDBUF, KERNEL_SOCKPTR(&v), sizeof(v));
    if (sock_setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, KERNEL_SOCKPTR(&v), sizeof(v)))
        sock_setsockopt(s, SOL_SOCKET, SO_RCVBUF, KERNEL_SOCKPTR(&v), sizeof(v));
}

static void tbt_car_connect_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(w, struct tbt_carrier, w_connect);
    struct tbt_session *sess = c->sess;
    struct tbt_tunnel *t = sess->tunnel;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = t->cfg.peer_port,
        .sin_addr.s_addr = t->cfg.peer_addr,
    };
    /* Source address: the configured local address for the carrier, else the
     * wildcard, which lets the route pick the right source toward `peer`. */
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = t->cfg.srcaddrs[0] ? t->cfg.srcaddrs[0] : htonl(INADDR_ANY),
    };
    struct socket *s;
    int err;

    if (t->cfg.role == TBT_ROLE_FAR)
        return;
    if (atomic_read(&c->stopping) || READ_ONCE(c->sock))
        return;

    if (!c->local_port)
    {
        enum tbt_port_alloc_err perr = TBT_PORT_ALLOC_OK;

        c->local_port = tbt_port_alloc(&perr);
        if (!c->local_port)
        {
            atomic64_inc(&sess->carrier_alloc_fail);
            tbt_port_alloc_note(sess, c->index, perr);
            c->retry_at_ns = ktime_get_ns() + 200 * NSEC_PER_MSEC;
            schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
            return;
        }
    }
    local.sin_port = htons(c->local_port);

    err = sock_create_kern(sess->net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        goto fail;
    /* The replacement carrier often reuses the port the dead one just freed,
     * which may still be in TIME_WAIT. */
    sock_set_reuseaddr(s->sk);
    err = kernel_bind(s, (struct sockaddr_unsized *)&local, sizeof(local));
    if (err)
        goto release;
    /* Before the SYN: the handshake freezes the window scale from this
     * socket's receive buffer. */
    tbt_car_setsockbuf(s, &t->cfg, sess->n_pri);
    s->sk->sk_sndtimeo = msecs_to_jiffies(TBT_CONNECT_TIMEOUT_MS);
    err = kernel_connect(s, (struct sockaddr_unsized *)&addr, sizeof(addr), 0);
    if (err)
        goto release;

    tcp_sk(s->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(s->sk);
    c->sock = s;
    c->sk = s->sk;

    write_lock_bh(&s->sk->sk_callback_lock);
    s->sk->sk_user_data = c;
    s->sk->sk_data_ready = tbt_car_ready;
    s->sk->sk_write_space = tbt_car_write_space;
    s->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&s->sk->sk_callback_lock);

    atomic_set(&c->connected, 1);
    c->state = TBT_CAR_CONNECTING; /* up after the pump writes HELLO */
    c->last_una = tcp_sk(s->sk)->snd_una;
    c->last_una_ns = ktime_get_ns();
    c->last_una_move_ns = ktime_get_ns();
    /* The peer may have written its HELLO before our sk_data_ready was
     * installed (the callback goes in after kernel_connect returns, and the
     * stock callback already fired for the queued data). Schedule the read
     * work unconditionally, exactly as the accept side does, or the HELLO sits
     * in the receive queue and the carrier never comes up (measured: probe I,
     * the near dialled 73 carriers, every far accept waited for HELLO). */
    schedule_work(&c->w_rx);
    schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
    pr_info("tbtun: tunnel %llu carrier %u DIALED lport=%u peer=%pI4:%u\n", t->cfg.id,
            c->index, ntohs(inet_sk(s->sk)->inet_sport), &t->cfg.peer_addr,
            ntohs(t->cfg.peer_port));
    tbt_tx_hello(sess, c->index, GFP_KERNEL);
    tbt_tx_wake(sess);
    return;

release:
    tbt_sock_abort_on_close(s);
    sock_release(s);
fail:
    /* Give the port block bit back: a failed carrier must not leak a slot that
     * the operator configured as part of a fixed block. */
    if (c->local_port)
    {
        tbt_port_free(c->local_port);
        c->local_port = 0;
    }
    atomic64_inc(&sess->connect_fail);
    c->retry_at_ns = ktime_get_ns() + 200 * NSEC_PER_MSEC;
    schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
    pr_warn("tbtun: tunnel %llu carrier %u failed to connect: %d\n", t->cfg.id, c->index, err);
}

/* HELLO: the first record on every carrier, in both directions. The body
 * carries the wire version, the sender's role, the session nonce, the two
 * payload ceilings and the last byte `profile | (gen << 2)`: profile in bits
 * 1:0, carrier generation in bits 5:2, bits 7:6 zero (DESIGN.md §3.3). The
 * header's path_id is the carrier index. */
int tbt_tx_hello(struct tbt_session *sess, u8 index, gfp_t gfp)
{
    u8 body[TBT_HELLO_BODY_LEN];
    struct tbt_tunnel *t = sess->tunnel;

    body[0] = TBT_WIRE_VERSION;
    body[1] = t->cfg.role;
    memcpy(body + 2, sess->nonce, sizeof(sess->nonce));
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);
    body[22] = (sess->car[index] ? sess->car[index]->profile
                                 : sess->tunnel->cfg.profile) |
               (u8)(((sess->car[index] ? sess->car[index]->gen : 0) & 0xf) << 2);
    return tbt_tx_control_pinned(sess, NULL, index, TBT_F_HELLO, 0, 0, body,
                                 sizeof(body), false, gfp);
}

void tbt_carrier_resume(struct tbt_session *sess)
{
    struct tbt_carrier *c;
    int i;

    if (READ_ONCE(sess->closing))
        return;
    atomic64_inc(&sess->rx_resume_calls);
    spin_lock(&sess->lock);
    for (i = 0; i < sess->n_car; i++)
    {
        c = READ_ONCE(sess->car[i]);
        if (c && READ_ONCE(c->sk) && !atomic_read(&c->stopping))
        {
            if (!schedule_work(&c->w_rx))
                atomic64_inc(&sess->rx_resume_busy);
        }
    }
    list_for_each_entry(c, &sess->prov, node)
    {
        if (READ_ONCE(c->sk) && !atomic_read(&c->stopping))
        {
            if (!schedule_work(&c->w_rx))
                atomic64_inc(&sess->rx_resume_busy);
        }
    }
    spin_unlock(&sess->lock);
}

/* Diagnostic snapshot for /proc/net/tbtun/stats: whether the callbacks the
 * data path depends on are still the ones this module installed, and why the
 * first carrier is (not) being read. */
void tbt_carrier_snapshot(struct tbt_session *sess)
{
    struct tbt_carrier *c = NULL;
    struct tcp_sock *tp;
    int i;

    for (i = 0; i < sess->n_car && !c; i++)
        c = READ_ONCE(sess->car[i]);
    if (!c)
        return;
    if (!c->sk)
        return;
    tp = tcp_sk(c->sk);
    sess->snap_car_state = READ_ONCE(c->sk->sk_state);
    sess->snap_car_stopping = atomic_read(&c->stopping);
    sess->snap_car_ud = READ_ONCE(c->sk->sk_user_data) != NULL;
    sess->snap_car_ready_ok = READ_ONCE(c->sk->sk_data_ready) == tbt_car_ready;
    sess->snap_car_ws_ok = READ_ONCE(c->sk->sk_write_space) == tbt_car_write_space;
    sess->snap_txw_pending = work_pending(&c->w_tx);
    sess->snap_txw_busy = work_busy(&c->w_tx);
    if (READ_ONCE(c->sk))
    {
        struct sock *sk = READ_ONCE(c->sk);

        sock_hold(sk);
        sess->snap_car_wmem = READ_ONCE(sk->sk_wmem_queued);
        sess->snap_car_rcv_nxt = READ_ONCE(tcp_sk(sk)->rcv_nxt);
        sess->snap_car_copied = READ_ONCE(tcp_sk(sk)->copied_seq);
        sess->snap_car_rcvbuf = READ_ONCE(sk->sk_rcvbuf);
        sess->snap_car_sndbuf = READ_ONCE(sk->sk_sndbuf);
        sock_put(sk);
    }
}

// Called once at load, from tbt_core.c, before any carrier exists.
int tbt_carrier_init(void)
{
    tbt_tcp_prot = tcp_prot; // field-by-field copy of the live TCP proto
    return 0;
}

// ---------------------------------------------------------------------------
// Far side.
//
// The far tunnel listens on its carrier port; an incoming connection there IS
// a carrier. It starts as a provisional object with no slot: the first record
// must be HELLO, which names the index. tbt_session_join() installs it, rejects
// a duplicate (nonce, index), and opens the one target connection per session.

int tbt_carrier_adopt(struct tbt_tunnel *t, struct socket *newsock)
{
    struct tbt_session *sess = t->sess;
    struct tbt_carrier *c;

    if (!sess)
        return -ENOTCONN;
    c = tbt_carrier_new(sess, 0xff);
    if (!c)
        return -ENOMEM;
    c->provisional = true;
    tcp_sk(newsock->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(newsock->sk);
    c->sock = newsock;
    c->sk = newsock->sk;

    write_lock_bh(&newsock->sk->sk_callback_lock);
    newsock->sk->sk_user_data = c;
    newsock->sk->sk_data_ready = tbt_car_ready;
    newsock->sk->sk_write_space = tbt_car_write_space;
    newsock->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&newsock->sk->sk_callback_lock);

    /* The listener already fixed the window scale; this sizes the accepted
     * socket's buffers to the same share. */
    tbt_car_setsockbuf(newsock, &sess->tunnel->cfg, sess->n_pri);
    atomic_set(&c->connected, 1);
    spin_lock(&sess->lock);
    list_add_tail(&c->node, &sess->prov);
    spin_unlock(&sess->lock);
    sess->rx_adopt_events++;
    /* Assert the wiring we just installed. A carrier that is connected but never
     * given our callbacks/proto looks exactly like a silent peer. */
    WARN_ON_ONCE(newsock->sk->sk_data_ready != tbt_car_ready);
    WARN_ON_ONCE(newsock->sk->sk_prot == &tcp_prot);
    schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
    schedule_work(&c->w_rx);
    pr_info("tbtun: tunnel %llu carrier ACCEPTED lport=%u rport=%u, awaiting HELLO\n",
            t->cfg.id, ntohs(inet_sk(newsock->sk)->inet_sport),
            ntohs(inet_sk(newsock->sk)->inet_dport));
    return 0;
}

void tbt_target_work_fn(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_target);
    struct tbt_session *sess = st->sess;
    struct tbt_tunnel *t = sess->tunnel;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = t->cfg.target_port,
        .sin_addr.s_addr = t->cfg.target_addr,
    };
    struct socket *s;
    int err;

    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        goto out;
    err = sock_create_kern(t->net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        goto fail;
    /* Same bound as a carrier dial: session teardown cancels w_target, and
     * the default connect timeout is the full SYN retry schedule. */
    s->sk->sk_sndtimeo = msecs_to_jiffies(TBT_CONNECT_TIMEOUT_MS);
    err = kernel_connect(s, (struct sockaddr_unsized *)&addr, sizeof(addr), 0);
    if (err)
    {
        sock_release(s);
        goto fail;
    }
    tcp_sk(s->sk)->nonagle = TCP_NAGLE_OFF;
    WRITE_ONCE(st->target, s);
    err = tbt_target_attach(st, s);
    if (err == -ECANCELED)
    {
        WRITE_ONCE(st->target, NULL);
        tbt_sock_abort_on_close(s);
        sock_release(s);
        goto fail;
    }
    if (err)
        pr_debug("tbtun: far tunnel %llu target waiting for OPEN\n", t->cfg.id);
    tbt_tx_pump(sess);
    tbt_stream_put(st);
    return;
fail:
    WRITE_ONCE(st->target_started, false);
out:
    tbt_stream_put(st);
}

int tbt_carrier_open_target(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    bool start = false;

    if (!sess)
        return -ENOTCONN;
    if (sess->tunnel->cfg.role != TBT_ROLE_FAR)
        return 0;
    spin_lock(&sess->lock);
    if (!st->target_started && !st->target)
    {
        st->target_started = true;
        start = true;
    }
    spin_unlock(&sess->lock);
    if (start)
    {
        refcount_inc(&st->refs);
        if (!queue_work(system_dfl_wq, &st->w_target))
            tbt_stream_put(st);
    }
    return 0;
}
