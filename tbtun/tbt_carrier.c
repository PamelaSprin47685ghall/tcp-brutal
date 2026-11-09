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
 *   - liveness: a 50 ms poll that re-arms queues and asks the session's
 *     replacement worker to dial a new carrier when one is dead. An
 *     ACK-stalled carrier is killed under ARQ; under FEC it is only paused,
 *     because parity can rebuild the shares it owed (DESIGN.md §9.7).
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
#include <linux/limits.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_carrier_policy.h"
#include "tbt_fec_batch.h"
#include "tbt_tx_acct.h"

/* One blocking wait inside kernel_connect(). A connect that has not completed
 * inside this window is retried by the session poll.
 * The kernel default is the full SYN retry schedule (~130 s), which a session
 * teardown would inherit: tbt_carriers_stop() cancels w_connect, and flush
 * must return in bounded time even when a carrier is dialling an unreachable
 * peer. 1 s is above any ordinary LAN/WAN SYN round trip and below the
 * operator-visible flush budget. */
#define TBT_CONNECT_TIMEOUT_MS 1000

/* Total budget for one dial, spent as several TBT_CONNECT_TIMEOUT_MS waits.
 * A single window is not enough on a WAN: the kernel retransmits a lost SYN
 * after 1 s, so one drop costs a full window plus the RTT and the dial is
 * thrown away for a connection that was about to complete. 4 s covers the
 * first two retransmissions and stays under TBT_JOIN_TIMEOUT_MS, the far
 * side's deadline for a carrier that has not presented its HELLO yet. */
#define TBT_CONNECT_BUDGET_MS 4000

/* Stall detection runs off this carrier's own RTO, not a fixed clock: k RTOs
 * of silence with unacknowledged bytes and an open window pause the carrier.
 * Five RTOs tolerate one lost ACK or a scheduling delay; 1 s keeps a very fast
 * path from pausing on a single hiccup, and 60 s keeps a slow one from hiding a
 * stall for minutes. A configured carrier_stall_ms can only lower the result. */
#define TBT_STALL_K 5
#define TBT_STALL_MIN_NS (1ULL * NSEC_PER_SEC)
#define TBT_STALL_MAX_NS (60ULL * NSEC_PER_SEC)

/* Final backstop: a carrier that stayed paused through every economic
 * decision and shows no wire life either is dead even if the replacement
 * could not run (allocation failure, a closing session). Four stall windows,
 * capped at 300 s, so the economic rule always gets the first word. */
#define TBT_STALL_DEAD_MAX_NS (300ULL * NSEC_PER_SEC)
/* tcp_retries2 defaults to 15; at that count TCP itself is about to give up,
 * so a wire that has not moved either way is dead. */
#define TBT_STALL_RETX_MAX 15
/* A carrier that wrote its HELLO but never queued an OPEN is connected and
 * invisible to the scheduler. Wait one poll interval before repairing it so
 * the normal HELLO->OPEN window inside one pump run is never touched. */
#define TBT_UP_RESCUE_NS (100ULL * NSEC_PER_MSEC)
/* Growth is paced like a dial: one new carrier per second at most, so a peer
 * that refuses the new index cannot turn growth into a connect storm. */
#define TBT_CARRIER_ADD_COOLDOWN_MS 1000

static u64 tbt_stall_dead_ns(u64 stall_ns)
{
    return min_t(u64, 4 * stall_ns, TBT_STALL_DEAD_MAX_NS);
}

// A private copy of tcp_prot so carriers carry our sendmsg/recvmsg. Same shape
// as the deleted brutal_sockopt.c used, for a different reason: there we wanted
// a setsockopt hook, here we want the TX/RX to be ours without a kprobe.
static struct proto tbt_tcp_prot __ro_after_init;

static void tbt_car_connect_work(struct work_struct *w);
int tbt_carrier_send_raw(struct tbt_carrier *c, const void *data, size_t len);
static void tbt_pump_work(struct work_struct *w);
static bool tbt_carrier_arm_connect(struct tbt_carrier *c);
static bool tbt_dial_take(struct tbt_session *sess);
static u32 tbt_dial_rate(const struct tbt_session *sess);
static bool tbt_stall_should_replace(struct tbt_carrier *c, u64 now);
static void tbt_stall_grid_note(struct tbt_session *sess, u64 t_ns,
                                bool recovered);
static bool tbt_carriers_all_dead(struct tbt_session *sess);
static void tbt_carrier_capacity_check(struct tbt_session *sess);

/* Drop every stream's "announced on this slot" mark. A replacement carrier
 * has a new generation, but the mark stores only its low four bits: a
 * generation 16, 32, ... greater than the dead one looks identical and
 * tbt_tx_announce_carrier() would skip the OPEN, leaving a connected carrier
 * at up=0 forever. The slot changed hands, so the old mark is stale by
 * definition. Safe from work and softirq: an RCU walk plus an atomic bit
 * clear, and the worst race with a concurrent OPEN write is one duplicate
 * OPEN, which the peer's bind treats as idempotent. */
static void tbt_carrier_unannounce(struct tbt_session *sess, u8 index)
{
    struct tbt_stream *st;
    int i;

    if (index >= TBT_MAX_CARRIERS_LIMIT)
        return;
    rcu_read_lock();
    for (i = 0; i < TBT_STREAM_HT; i++)
        hlist_for_each_entry_rcu(st, &sess->streams[i], node)
            clear_bit(index, st->announced_mask);
    rcu_read_unlock();
}

/* Repair a carrier that is connected and has written its HELLO, but whose
 * up flag never came back: the OPEN that tbt_tx_announce_carrier() queued at
 * HELLO time was refused (the control queue was full) or skipped (the
 * four-bit generation looked already announced). Nothing else retries that
 * OPEN, so without this the carrier stays out of the stripe for the rest of
 * the session while its socket is healthy. Called from the 50 ms poll. */
static void tbt_carrier_rescue_up(struct tbt_session *sess,
                                  struct tbt_carrier *c)
{
    struct tbt_stream **snap;
    int n, i;

    snap = kmalloc_array(TBT_STREAM_SNAP_MAX, sizeof(*snap), GFP_KERNEL);
    if (!snap)
        return; /* the next poll retries */
    n = tbt_session_stream_snapshot(sess, snap, TBT_STREAM_SNAP_MAX);
    if (!n)
    {
        /* No stream needs an OPEN, so there is nothing left to announce;
         * the HELLO write branch only left up==0 because it snapshotted a
         * stream that has since died. The carrier is usable for control
         * frames, and the next stream announces itself on creation. */
        atomic_set(&c->up, 1);
        atomic64_inc(&sess->up_rescued);
        kfree(snap);
        return;
    }
    for (i = 0; i < n; i++)
    {
        clear_bit(c->index, snap[i]->announced_mask);
        tbt_tx_announce(sess, snap[i], GFP_KERNEL);
        tbt_stream_put(snap[i]);
    }
    kfree(snap);
    atomic64_inc(&sess->up_rescued);
}

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

/* Take one reference on the published socket. The RCU read section covers
 * only "read the pointer + take a reference"; the caller may sleep, take the
 * socket lock and send until tbt_carrier_sk_put(). NULL means the carrier has
 * no usable socket: never published, being released, or already unpublishable.
 * refcount_inc_not_zero() rather than sock_hold(): the pointer is found by
 * RCU, not by a locked hash lookup, so the last sock_put() may already have
 * run. */
struct sock *tbt_carrier_sk_get(struct tbt_carrier *c)
{
    struct sock *sk;

    if (!c)
        return NULL;
    rcu_read_lock();
    sk = rcu_dereference(c->sk);
    if (sk && !refcount_inc_not_zero(&sk->sk_refcnt))
        sk = NULL;
    rcu_read_unlock();
    return sk;
}

void tbt_carrier_sk_put(struct sock *sk)
{
    if (sk)
        sock_put(sk);
}

/* Take one object reference from sess->car[]. The container read and the
 * refcount increment are one RCU-protected step: retire removes the slot with
 * rcu_assign_pointer() and only then drops the installer reference, so a
 * reader either increments before the last put or sees a zero count. */
struct tbt_carrier *tbt_carrier_get(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c;

    if (!sess || index >= sess->n_car)
        return NULL;
    rcu_read_lock();
    c = rcu_dereference(sess->car[index]);
    if (c && !refcount_inc_not_zero(&c->refs))
        c = NULL;
    rcu_read_unlock();
    return c;
}

static void tbt_carrier_free_rcu(struct rcu_head *head)
{
    struct tbt_carrier *c = container_of(head, struct tbt_carrier, rcu);

    tbt_carrier_free(c);
}

void tbt_carrier_put(struct tbt_carrier *c)
{
    if (c && refcount_dec_and_test(&c->refs))
        call_rcu(&c->rcu, tbt_carrier_free_rcu);
}

u32 tbt_carrier_gen(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c = tbt_carrier_get(sess, index);
    u32 gen;

    if (!c)
        return 0;
    gen = READ_ONCE(c->gen);
    tbt_carrier_put(c);
    return gen;
}

/* rx_again is a two-bit state machine, not a boolean:
 *   bit 0: a wake arrived that this carrier has not consumed yet;
 *   bit 1: a w_rx instance is queued or running.
 * schedule_work() does not serialise a work item against itself: queueing it
 * while it runs puts a second instance on another worker. Two parsers then
 * share c->parser and c->http_rx->rec, so one can overwrite the record buffer
 * between decode and tbt_fec_rx_record() -- a payload that decrypts twice or
 * a payload spliced from two chunks (header CRC valid, payload CRC bad).
 * The running bit keeps at most one parser; a wake during a run is coalesced
 * into bit 0 and the entry wrapper re-queues after the run. */
#define TBT_RX_AGAIN_BIT   0
#define TBT_RX_RUNNING_BIT 1

static void tbt_car_schedule_rx(struct tbt_carrier *c)
{
    if (atomic_fetch_or((int)BIT(TBT_RX_RUNNING_BIT), &c->rx_again) &
        (int)BIT(TBT_RX_RUNNING_BIT))
    {
        atomic_fetch_or((int)BIT(TBT_RX_AGAIN_BIT), &c->rx_again);
        return;
    }
    if (!schedule_work(&c->w_rx))
    {
        /* A queued instance already owns the running bit. */
        atomic_fetch_or((int)BIT(TBT_RX_AGAIN_BIT), &c->rx_again);
    }
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
    tbt_car_schedule_rx(c);
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
    tbt_tx_wake(c->sess);
    if (c->sess->fec)
    {
        /* Wake this carrier's own FEC drain now that the socket has room.
         * The packetizer still runs on the session work; the drain is the
         * per-carrier writer that can use the freed send memory immediately. */
        tbt_fec_out_kick(c->sess, c->index);
        /* Through the session bit, not schedule_work(): a raw queue takes no
         * session reference and is silently dropped while a cancel has the
         * work disabled. */
        tbt_session_arm(c->sess, TBT_SW_FEC_TX, 0);
    }
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
    /* The session poll is the only path that notices a closed socket while no
     * data is moving. Arm it at 10 ms, never at zero: under a connect flood
     * every accepted socket changes state, and a 0-delay re-arm turned the
     * 50 ms liveness pass into a continuous loop over every carrier and every
     * provisional. If a pass is already queued, tbt_session_arm() keeps its
     * schedule; a close is then noticed by that pass. */
    if (c->sess && !READ_ONCE(c->sess->closing))
        tbt_session_arm(c->sess, TBT_SW_POLL, msecs_to_jiffies(10));
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

/* Peripheral BBR hold (hold_enable). BBR resets cwnd/pacing after an idle
 * period; this poll restores the last good values while the carrier has
 * nothing real to send. It never touches delivered/rate_delivered and never
 * changes the TCP sequence space. */
#define TBT_HOLD_ENTER_NS (200ULL * NSEC_PER_MSEC)
#define TBT_HOLD_MAX_NS (10ULL * NSEC_PER_SEC)
#define TBT_HOLD_LOSS_GUARD_NS (1ULL * NSEC_PER_SEC)

static bool tbt_car_hold_idle(struct tbt_session *sess)
{
    struct tbt_fec_tx *tx;
    bool busy = false;

    if (!tbt_hold_session_idle(READ_ONCE(sess->tx_queued),
                               READ_ONCE(sess->ctl_real)))
        return false;
    if (!sess->fec)
        return true;
    rcu_read_lock();
    tx = rcu_dereference(sess->fec_tx);
    if (tx && (READ_ONCE(tx->head) != READ_ONCE(tx->tail) ||
               READ_ONCE(tx->fill)))
        busy = true;
    rcu_read_unlock();
    if (busy)
        return false;
    return !READ_ONCE(sess->fec_outq_queued) &&
           !READ_ONCE(sess->fec_out_ctl_depth) &&
           !READ_ONCE(sess->fec_replay_queued);
}

static void tbt_car_hold_apply(struct tbt_carrier *c, struct tcp_sock *tp,
                               struct sock *sk)
{
    u32 cwnd = c->hold_cwnd;
    enum tbt_hold_apply gate;

    if (!cwnd)
        return;
    if (READ_ONCE(tp->snd_cwnd_clamp))
        cwnd = min(cwnd, READ_ONCE(tp->snd_cwnd_clamp));
    if (!cwnd)
        cwnd = 1;
    lock_sock(sk);
    /* Re-check under the socket lock. A non-PAD writer clears hold_active
     * before it queues bytes, so this catches the real race. The in-flight
     * test must not end the hold: PAD is the only frame that leaves it active
     * with packets_out set, and its writer exempts TBT_F_PAD on purpose. */
    gate = tbt_hold_apply_gate(
        READ_ONCE(c->hold_active),
        inet_csk(sk)->icsk_ca_state == TCP_CA_Open,
        READ_ONCE(tp->snd_una) == READ_ONCE(tp->snd_nxt),
        READ_ONCE(tp->packets_out));
    if (gate == TBT_HOLD_APPLY_EXIT)
    {
        release_sock(sk);
        WRITE_ONCE(c->hold_active, false);
        WRITE_ONCE(c->hold_since_ns, 0);
        return;
    }
    if (gate == TBT_HOLD_APPLY_SKIP)
    {
        release_sock(sk);
        return;
    }
    tcp_snd_cwnd_set(tp, cwnd);
    if (c->hold_pacing)
        (void)cmpxchg(&sk->sk_pacing_rate, READ_ONCE(sk->sk_pacing_rate),
                      c->hold_pacing);
    release_sock(sk);
}

static void tbt_car_hold_check(struct tbt_carrier *c, struct tcp_sock *tp,
                               struct sock *sk, u64 now)
{
    bool enable = READ_ONCE(tbt_hold_enable);
    bool real_data;
    bool loss = READ_ONCE(c->stalled) || READ_ONCE(tp->lost_out) ||
                inet_csk(sk)->icsk_ca_state != TCP_CA_Open;
    bool recent_loss;
    bool no_inflight;
    bool ca_open;
    u32 mss, wnd_pkts;

    if (loss)
        WRITE_ONCE(c->hold_last_loss_ns, now);
    recent_loss = c->hold_last_loss_ns && now >= c->hold_last_loss_ns &&
                  now - c->hold_last_loss_ns < TBT_HOLD_LOSS_GUARD_NS;
    if (!enable)
    {
        WRITE_ONCE(c->hold_active, false);
        WRITE_ONCE(c->hold_since_ns, 0);
        return;
    }
    real_data = !tbt_car_hold_idle(c->sess);
    mss = READ_ONCE(tp->mss_cache) ?: 1;
    wnd_pkts = READ_ONCE(tp->snd_wnd) / mss;
    no_inflight = READ_ONCE(tp->snd_una) == READ_ONCE(tp->snd_nxt) &&
                  READ_ONCE(tp->packets_out) == 0;
    ca_open = inet_csk(sk)->icsk_ca_state == TCP_CA_Open;

    if (c->hold_active)
    {
        if (!tbt_hold_keep(true, real_data, loss, wnd_pkts < c->hold_cwnd,
                           now, c->hold_until_ns))
        {
            WRITE_ONCE(c->hold_active, false);
            WRITE_ONCE(c->hold_since_ns, 0);
            return;
        }
        tbt_car_hold_apply(c, tp, sk);
        return;
    }
    if (!tbt_hold_enter(enable, no_inflight, !real_data, ca_open, loss,
                        recent_loss, c->hold_since_ns, now,
                        TBT_HOLD_ENTER_NS))
    {
        if (enable && no_inflight && !real_data && !loss && ca_open)
        {
            if (!c->hold_since_ns)
                WRITE_ONCE(c->hold_since_ns, now);
        }
        else
        {
            WRITE_ONCE(c->hold_since_ns, 0);
        }
        return;
    }
    {
        u32 cwnd = READ_ONCE(tp->snd_cwnd);
        unsigned long pacing = READ_ONCE(sk->sk_pacing_rate);

        if (!cwnd)
            return;
        if (wnd_pkts)
            cwnd = min(cwnd, wnd_pkts);
        if (READ_ONCE(tp->snd_cwnd_clamp))
            cwnd = min(cwnd, READ_ONCE(tp->snd_cwnd_clamp));
        if (sk->sk_sndbuf > 0)
            cwnd = min(cwnd, (u32)(sk->sk_sndbuf / mss));
        if (!cwnd)
            cwnd = 1;
        if (sk->sk_max_pacing_rate && pacing > sk->sk_max_pacing_rate)
            pacing = sk->sk_max_pacing_rate;
        c->hold_cwnd = cwnd;
        c->hold_pacing = pacing;
        WRITE_ONCE(c->hold_until_ns, now + TBT_HOLD_MAX_NS);
        WRITE_ONCE(c->hold_active, true);
        tbt_car_hold_apply(c, tp, sk);
    }
}

/* C2/C3 liveness net, for one carrier. The session's 50 ms poll calls this for
 * every slot and every provisional carrier; it never re-arms itself, so a
 * session pays one timer instead of one per carrier.
 *
 * The event-driven path is the normal driver, but a wake can be coalesced with
 * a run that then finds nothing, or lost against a pause transition; a queue
 * then sits full with no scheduled reader. Re-check the three queues every
 * 50 ms and re-arm the work item that has something to do. It never copies
 * application bytes; the only socket lock it takes is the drip write.
 *
 * It is also the carrier's health check. A closed socket marks the carrier
 * lost and asks for a replacement. Unacknowledged data that has not moved for
 * k RTOs is treated by container: ARQ kills the carrier and repaths its
 * frames; FEC pauses it instead (no RETIRE, no replacement), because the
 * shares it owed are rebuilt from parity and a dead carrier would throw them
 * away. The same poll resumes a paused carrier when snd_una moves, and
 * declares it dead after stall_dead when it never does. A provisional carrier
 * that never presents a HELLO is retired when its join deadline expires.
 *
 * Deletion condition: remove the queue re-arm when probes B/C/D and T7 record
 * poll_rescues = 0; the health check has no other driver. */
static void tbt_car_poll_one(struct tbt_carrier *c)
{
    struct tbt_session *sess = c->sess;
    struct tbt_stream *st;
    struct tcp_sock *tp;
    struct sock *sk = NULL;
    u64 now = ktime_get_ns();
    bool rescued = false;
    int i;

    if (!sess || !sess->tunnel || READ_ONCE(sess->closing))
        return;
    /* The caller holds one carrier reference for the whole pass, so `c` stays
     * valid; the socket needs its own reference because release clears c->sk
     * and closes the socket concurrently. */
    sk = tbt_carrier_sk_get(c);
    if (sess->diag_poll_round <= 2)
    {
        TBT_DIAG("poll r=%u tunnel=%llu carrier=%u gen=%u connected=%d sock=%d sk_state=%d hello_sent=%d up=%d stopping=%d prov=%d retry_in_ms=%lld state=%u",
                 sess->diag_poll_round,
                 (unsigned long long)sess->tunnel->cfg.id, c->index, c->gen,
                 atomic_read(&c->connected), !!sk,
                 sk ? READ_ONCE(sk->sk_state) : -1,
                 READ_ONCE(c->hello_sent), atomic_read(&c->up),
                 atomic_read(&c->stopping), c->provisional,
                 (long long)(c->retry_at_ns - now) / NSEC_PER_MSEC, c->state);
    }
    if (atomic_read(&c->stopping))
    {
        tbt_carrier_sk_put(sk);
        tbt_session_arm(sess, TBT_SW_REPLACE, 0);
        return;
    }
    if (c->provisional)
    {
        if (now > c->join_deadline_ns)
        {
            atomic_set(&c->stopping, 1);
            tbt_carrier_sk_put(sk);
            tbt_session_arm(sess, TBT_SW_REPLACE, 0);
            return;
        }
        /* The one-shot w_rx may have run before HELLO was queued.  The poll
         * is the only rescue while the carrier is provisional; without this
         * check a lost wake leaves HELLO unread until the join deadline.
         * parser->have covers a HELLO whose bytes were copied out but whose
         * record could not be parsed yet (copied_seq == rcv_nxt). */
        if (sk && READ_ONCE(sk->sk_state) == TCP_ESTABLISHED &&
            (READ_ONCE(tcp_sk(sk)->copied_seq) !=
                 READ_ONCE(tcp_sk(sk)->rcv_nxt) ||
             c->parser->have))
            tbt_car_schedule_rx(c);
        tbt_carrier_sk_put(sk);
        return;
    }
    if (!atomic_read(&c->connected))
    {
        /* The connect failed (or has not been attempted yet). Retry from here:
         * the session may have been created before the far listener was up.
         * The retry spends a dial token: 256 slots x 5 Hz was 1280 connect
         * attempts/s against a peer that could not complete them. The first
         * wave spends the same tokens: tbt_carriers_start() installs every
         * carrier unconnected and lets this poll pace the dials. */
        if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && now >= c->retry_at_ns)
        {
            if (tbt_dial_take(sess))
            {
                c->retry_at_ns = now + 200 * NSEC_PER_MSEC;
                TBT_DIAG("poll dial tunnel=%llu carrier=%u gen=%u r=%u",
                         (unsigned long long)sess->tunnel->cfg.id, c->index,
                         c->gen, sess->diag_poll_round);
                tbt_carrier_arm_connect(c);
            }
            else
            {
                /* Bucket empty: look again after it has refilled a little. */
                c->retry_at_ns = now + 20 * NSEC_PER_MSEC;
            }
        }
        tbt_carrier_sk_put(sk);
        return;
    }

    if (!sk)
    {
        if (READ_ONCE(c->stalled))
            tbt_stall_grid_note(sess, now - c->stall_since_ns, false);
        tbt_carrier_lost(sess, c);
        return;
    }
    if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
    {
        if (READ_ONCE(c->stalled))
            tbt_stall_grid_note(sess, now - c->stall_since_ns, false);
        tbt_carrier_sk_put(sk);
        tbt_carrier_lost(sess, c);
        return;
    }
    /* A connected carrier with a written HELLO and up==0 is not schedulable:
     * the OPEN queued at HELLO time was refused (control queue full) or
     * skipped (four-bit generation looked already announced), and no other
     * path retries it. Repair it once per poll interval, and only when the
     * control queue has room, so the retry cannot pile onto the same
     * backpressure that caused it. */
    if (READ_ONCE(c->hello_sent) && !atomic_read(&c->up))
    {
        if (!c->up_rescue_ns)
            c->up_rescue_ns = now;
        else if (now - c->up_rescue_ns > TBT_UP_RESCUE_NS &&
                 READ_ONCE(sess->ctl_queued) < TBT_CTL_QUEUE_MAX)
        {
            c->up_rescue_ns = now;
            tbt_carrier_rescue_up(sess, c);
        }
    }
    else
    {
        c->up_rescue_ns = 0;
    }
    tp = tcp_sk(sk);
    tbt_car_hold_check(c, tp, sk, now);
    {
        u32 una = READ_ONCE(tp->snd_una);
        u32 nxt = READ_ONCE(tp->snd_nxt);
        u32 wnd = READ_ONCE(tp->snd_wnd);
        u32 wmem = READ_ONCE(sk->sk_wmem_queued);
        bool progress = una != c->last_una;
        bool paused = tbt_car_paused(sess);
        /* Auto is k x this carrier's RTO (SRTT + 4 x RTTVAR, floored at
         * 200 ms), clamped to [1 s, 60 s]. A configured carrier_stall_ms is a
         * cap, never a replacement: it can lower the threshold, not raise it. */
        u64 stall_ns;
        {
            u32 ms = sess->tunnel->cfg.carrier_stall_ms;
            u32 srtt = READ_ONCE(tp->srtt_us) >> 3;
            u32 mdev = READ_ONCE(tp->mdev_us);
            u64 rto = max_t(u64, (u64)srtt + 4ULL * mdev, 200000ULL);

            stall_ns = clamp_t(u64, TBT_STALL_K * rto * NSEC_PER_USEC,
                               TBT_STALL_MIN_NS, TBT_STALL_MAX_NS);
            if (ms)
                stall_ns = min_t(u64, stall_ns, (u64)ms * NSEC_PER_MSEC);
        }
        /* Start of the current "unacked bytes, no progress, window open"
         * interval; c->last_una_ns carries it between polls. */
        u64 stall_since_ns = c->last_una_ns;

        /* Long stall: replace this carrier at its own index. Growing the
         * stripe (tbt_carrier_add) stays disabled until the far side extends
         * its n_car on join; until then a new index would only be refused by
         * the peer, so no dial is spent on it. The decision is the
         * optimal-stopping rule above, not a fixed timer, and backpressure is
         * never taken here. */
        if (READ_ONCE(c->stalled) && !paused && wnd &&
            tbt_stall_should_replace(c, now))
        {
            atomic64_inc(&sess->stall_replaced);
            if (READ_ONCE(sess->stall_replace_reason) ==
                TBT_STALL_REPLACE_RETRANS)
                atomic64_inc(&sess->stall_replace_retrans);
            else
                atomic64_inc(&sess->stall_replace_econ);
            tbt_stall_grid_note(sess, now - c->stall_since_ns, false);
            sess->last_stall_replace_ns = now;
            tbt_carrier_sk_put(sk);
            tbt_carrier_lost(sess, c);
            return;
        }

        /* Bound on a paused carrier. Keepalive normally closes a half-open
         * socket first, but a path can black-hole without the socket ever
         * seeing an error. When the pause outlives the bound and the wire
         * shows no life (snd_una frozen, packets_out unchanged or the RTO
         * retransmit count at its limit), the stall was a death after all:
         * count stall_dead and take the normal loss path. A zero peer window
         * or our own delivery pause is evidence the peer is alive, so neither
         * ever feeds the dead bound. */
        if (READ_ONCE(c->stalled) &&
            !paused && wnd &&
            now - c->stall_since_ns > tbt_stall_dead_ns(stall_ns) &&
            una == c->last_una &&
            (READ_ONCE(tp->packets_out) == c->stall_packets_out ||
             READ_ONCE(inet_csk(sk)->icsk_retransmits) >= TBT_STALL_RETX_MAX))
        {
            atomic64_inc(&sess->stall_dead);
            atomic64_inc(&sess->stall_replace_dead);
            WRITE_ONCE(sess->stall_replace_reason, TBT_STALL_REPLACE_DEAD);
            tbt_stall_grid_note(sess, now - c->stall_since_ns, false);
            sess->last_stall_replace_ns = now;
            tbt_carrier_sk_put(sk);
            tbt_carrier_lost(sess, c);
            return;
        }

        if (progress)
        {
            c->last_una = una;
            /* Real progress, not a backpressure refresh: this is the only
             * evidence that a carrier is still delivering our bytes, and
             * tbt_tx reads it for retransmit pacing. */
            c->last_una_move_ns = now;
            /* A stalled carrier is paused, not dead: the path that was
             * avoided is delivering again, so put it back in the stripe. */
            if (READ_ONCE(c->stalled))
            {
                u64 recover_ns = now - c->stall_since_ns;

                WRITE_ONCE(c->stalled, false);
                atomic64_inc(&sess->stall_recovered);
                /* Feed the survival estimate: a path that came back tells
                 * the next decision how long this kind of dip lasts. */
                tbt_stall_grid_note(sess, recover_ns, true);
                pr_info_ratelimited("tbtun: carrier %u recovered after %llu ms: back in the stripe\n",
                                    c->index,
                                    (unsigned long long)(recover_ns /
                                                         NSEC_PER_MSEC));
            }
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
         * feedback shares the ordered stream with the data. The default 0
         * derives 3 x this carrier's RTO (SRTT + 4 x RTTVAR, floored at
         * 200 ms), itself floored at 3 s, so the timer follows the path
         * instead of a constant.
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
                if (debug >= 2 && c->dbg_stall_dumped != (u8)(c->gen & 0xff))
                {
                    /* One dump per generation. A carrier that stops being
                     * acknowledged is the WAN's remaining failure mode, and the
                     * counters above cannot tell a peer that stopped reading
                     * from a TCP that stopped sending: the queue, the window and
                     * the retransmit state can. */
                    c->dbg_stall_dumped = (u8)(c->gen & 0xff);
                    pr_info("tbtun: carrier stall idx=%u gen=%u state=%u "
                            "local=%pI4:%u peer=%pI4:%u "
                            "una=%u nxt=%u write_seq=%u "
                            "wmem=%u sndbuf=%u rcvbuf=%u "
                            "snd_wnd=%u rcv_wnd=%u rcv_nxt=%u copied=%u "
                            "packets_out=%u retrans=%u "
                            "unacked_ms=%llu\n",
                            c->index, c->gen, READ_ONCE(sk->sk_state),
                            &inet_sk(sk)->inet_saddr,
                            ntohs(inet_sk(sk)->inet_sport),
                            &inet_sk(sk)->inet_daddr,
                            ntohs(inet_sk(sk)->inet_dport), una, nxt,
                            READ_ONCE(tp->write_seq), wmem,
                            READ_ONCE(sk->sk_sndbuf), READ_ONCE(sk->sk_rcvbuf),
                            wnd, READ_ONCE(tp->rcv_wnd),
                            READ_ONCE(tp->rcv_nxt), READ_ONCE(tp->copied_seq),
                            READ_ONCE(tp->packets_out),
                            inet_csk(sk)->icsk_retransmits,
                            (unsigned long long)((now - stall_since_ns) /
                                                 NSEC_PER_MSEC));
                }
                tbt_carrier_sk_put(sk);
                if (sess->fec)
                {
                    /* FEC: a stalled carrier loses shares, not its life. Pause
                     * it; the FEC data plane treats the shares already in
                     * flight as lost and parity rebuilds the block. Killing
                     * the carrier here would discard shares the block can
                     * still rebuild. stall_dead bounds how long it may stay
                     * paused. */
                    tbt_carrier_stall(sess, c);
                }
                else
                {
                    /* ARQ has no parity to lean on, so an ACK-stalled path is a
                     * dead one: kill it and repath its frames now. */
                    tbt_carrier_lost(sess, c);
                }
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

        if (!sess->fec && c->profile == 2 &&
            tbt_http_need_drip(c->last_tx_ns, now, drip_ms))
        {
            char b[TBT_HTTP_DRIP_CHUNK_LEN];
            size_t n = tbt_http_drip_chunk(b, sizeof(b));

            if (n && mutex_trylock(&sess->pump_lock))
            {
                int err = tbt_carrier_send_raw(c, b, n);

                mutex_unlock(&sess->pump_lock);
                if (!err)
                {
                    c->last_tx_ns = ktime_get_ns();
                    atomic64_inc(&sess->drip_tx);
                }
            }
            /* trylock failed: the pump owns this byte stream right now; the
             * next 50 ms pass drips instead. */
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
        if (tbt_carrier_arm_rx(c))
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
        if (tbt_carrier_arm_rx(c))
            rescued = true;
    }
    if (READ_ONCE(sess->tx_queued))
    {
        if (tbt_carrier_arm_tx(c))
            rescued = true;
    }
    rcu_read_lock();
    for (i = 0; i < TBT_STREAM_HT; i++)
        hlist_for_each_entry_rcu(st, &sess->streams[i], node)
            if (!list_empty(&st->rx_q))
            {
                if (tbt_stream_schedule(st, &st->w_drain,
                                        TBT_STREAM_F_SCHED_DRAIN))
                    rescued = true;
            }
    rcu_read_unlock();
    if (rescued)
        atomic64_inc(&sess->poll_rescues);
    if (sess->fec)
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    tbt_carrier_sk_put(sk);
}

/* The session's liveness pass: every carrier slot, then every provisional
 * carrier, once per 50 ms. Each slot is held with a carrier reference, so a
 * replacement can remove and put its object while this pass runs.
 *
 * The provisional list is walked by rotating its head to the tail: each
 * element is moved under sess->lock. A provisional carrier's branch never
 * sleeps, so it is polled with the lock held: w_replace() cannot move it to
 * its victims list and free it while this pass still holds the pointer. A
 * joined carrier (which the new adopt order never puts on prov) takes socket
 * locks, so it is polled with the lock dropped and a reference held. */
static void tbt_session_poll_body(struct tbt_session *sess)
{
    struct tbt_carrier *c;
    unsigned int nprov = 0;
    int i;

    if (!sess->tunnel || READ_ONCE(sess->closing))
        return;
    /* Single writer: one delayed work owns this pass. The per-carrier poll
     * reads it only to bound its first-pass snapshots. */
    sess->diag_poll_round++;
    for (i = 0; i < sess->n_car; i++)
    {
        c = tbt_carrier_get(sess, (u8)i);
        if (c)
        {
            tbt_car_poll_one(c);
            tbt_carrier_put(c);
        }
    }
    spin_lock(&sess->lock);
    list_for_each_entry(c, &sess->prov, node)
        nprov++;
    spin_unlock(&sess->lock);
    while (nprov--)
    {
        spin_lock(&sess->lock);
        if (list_empty(&sess->prov))
        {
            spin_unlock(&sess->lock);
            break;
        }
        c = list_first_entry(&sess->prov, struct tbt_carrier, node);
        list_move_tail(&c->node, &sess->prov);
        if (c->provisional)
        {
            tbt_car_poll_one(c);
            spin_unlock(&sess->lock);
            continue;
        }
        if (!refcount_inc_not_zero(&c->refs))
        {
            spin_unlock(&sess->lock);
            continue;
        }
        spin_unlock(&sess->lock);
        tbt_car_poll_one(c);
        tbt_carrier_put(c);
    }
    /* Backpressure is the last resort: only a session with no usable carrier
     * and no room to grow reaches it. The check also grows the stripe when
     * it can, so the next pass has capacity without touching the user. */
    tbt_carrier_capacity_check(sess);
    if (!READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_POLL, msecs_to_jiffies(50));
}

void tbt_session_poll_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_poll);
    bool owned = test_and_clear_bit(TBT_SW_POLL, &sess->sched);

    tbt_session_poll_body(sess);
    if (owned)
        tbt_session_put(sess);
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
    /* One work item may not parse a whole multi-megabyte receive queue: the
     * far side reported tbt_car_rx_work hogging a worker for >10 ms. Stop
     * between pieces once the body's deadline is spent. A 0-byte actor
     * return is "nothing consumed" to tcp_read_sock(): it stops the walk and
     * leaves the remaining bytes in the socket, and tbt_car_rx_body()
     * re-arms because copied_seq != rcv_nxt. */
    if (tbt_rx_budget_stopped(&c->rx_budget, ktime_get_ns()))
    {
        atomic64_inc(&c->sess->rx_budget_yields);
        return 0;
    }
    atomic64_inc(&c->sess->rx_drain_entries);
    atomic64_add(len, &c->sess->rx_drain_bytes);

    while (used < len)
    {
        size_t n = 0;
        size_t take = min_t(size_t, len - used, TBT_RX_RUN_SLICE);

        err = tbt_rx_skb(c, skb, offset + used, take, &n);
        used += n;
        tbt_rx_budget_charge(&c->rx_budget, n);
        if (err)
            break;
        /* A short return is legal: tcp_read_sock() advances copied_seq and
         * calls the actor again for the same skb. The next call sees the
         * deadline and returns 0, so the yield is always between pieces. */
        if (tbt_rx_budget_stopped(&c->rx_budget, ktime_get_ns()))
            break;
    }
    if (err)
    {
        if (err == -EAGAIN)
        {
            /* The delivery ring is full. Return the consumed byte count so
             * tcp_read_sock() advances copied_seq past exactly those bytes;
             * the record that did not fit stays in the parser for the
             * resume. */
            return used;
        }
        tbt_car_rx_error(c, err);
        return 0;
    }
    return used;
}

static void tbt_car_rx_body(struct tbt_carrier *c)
{
    read_descriptor_t desc = {};
    struct tbt_stream *st;
    struct sock *sk;
    int i, drained;

    atomic64_inc(&c->sess->rx_work_runs);
    /* Consume the wake this run was queued for; the running bit stays set
     * until the wrapper returns. */
    atomic_fetch_and(~(int)BIT(TBT_RX_AGAIN_BIT), &c->rx_again);
    sk = tbt_carrier_sk_get(c);
    if (atomic_read(&c->stopping) || !sk)
    {
        atomic64_inc(&c->sess->rx_work_early);
        tbt_carrier_sk_put(sk);
        return;
    }
    /* First window only: before the peer's HELLO has been parsed. This is the
     * run that hands the first bytes to the container parser; if the process
     * dies inside that parse, this line is the last evidence of the state it
     * started from. */
    if (!READ_ONCE(c->saw_hello))
        TBT_DIAG("rx work tunnel=%llu carrier=%u gen=%u sk_state=%u copied=%u rcv_nxt=%u have=%zu http_have=%u",
                 (unsigned long long)c->sess->tunnel->cfg.id, c->index, c->gen,
                 READ_ONCE(sk->sk_state),
                 READ_ONCE(tcp_sk(sk)->copied_seq),
                 READ_ONCE(tcp_sk(sk)->rcv_nxt), c->parser->have,
                 c->http_have);

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
                    tbt_carrier_sk_put(sk);
                    return;
                }
            }
        }
        rcu_read_unlock();
    }
    desc.arg.data = c;
    /* tcp_read_sock() stops after one whole skb when count is zero; any
     * non-zero value lets it walk more. SIZE_MAX also stays non-zero if a
     * core variant subtracts the used bytes (the splice actor does), so this
     * is "not exhausted" on every kernel this module supports. The actor
     * stops itself on the time budget instead. */
    desc.count = SIZE_MAX;

    atomic64_inc(&c->sess->rx_reads);
    tbt_rx_budget_start(&c->rx_budget, ktime_get_ns());
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
            tbt_carrier_sk_put(sk);
            return;
        }
        if (perr)
        {
            release_sock(sk);
            tbt_car_rx_error(c, perr);
            tbt_carrier_sk_put(sk);
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
    else
    {
        /* One rule decides the wake this run owes (tbt_rx_budget.h):
         *
         *   - unread socket bytes: re-arm now. This covers both a budgeted
         *     yield and bytes that arrived after tcp_read_sock() stopped
         *     (sk_data_ready may have been coalesced); the two must not
         *     diverge, or a budget stop would strand the remainder.
         *   - parser->have alone: wait for the 50 ms poll or sk_data_ready.
         *     Those bytes are already out of the socket (copied_seq ==
         *     rcv_nxt), so a re-run copies nothing and spins at 100% of a
         *     kworker until the tail arrives. */
        enum tbt_rx_resume plan =
            tbt_rx_resume_plan(atomic_read(&c->stopping),
                               tbt_carrier_sk_present(c),
                               READ_ONCE(tcp_sk(sk)->copied_seq) !=
                                   READ_ONCE(tcp_sk(sk)->rcv_nxt),
                               c->parser->have != 0);

        if (plan == TBT_RX_RESUME_ARM)
            tbt_carrier_arm_rx(c);
    }

    tbt_carrier_sk_put(sk);

    /* Parsing may have enqueued a control frame (OPEN, FBK). The enqueue
     * already asked the session pump to drain it; never pump from this work item, because
     * pumping the carrier whose lock this work item holds deadlocks against
     * the peer window that only this work item's next read can open. */
}

static void tbt_car_rx_work(struct work_struct *w)
{
    struct tbt_carrier *c = container_of(w, struct tbt_carrier, w_rx);

    tbt_car_rx_body(c);
    /* Publish "not running" first: a wake after this point either queues a
     * new instance directly or sets the again bit, which this thread then
     * consumes. A wake that arrived during the run is coalesced here. */
    atomic_fetch_and(~(int)BIT(TBT_RX_RUNNING_BIT), &c->rx_again);
    if ((atomic_fetch_and(~(int)BIT(TBT_RX_AGAIN_BIT), &c->rx_again) &
         (int)BIT(TBT_RX_AGAIN_BIT)) &&
        !atomic_read(&c->stopping))
        tbt_car_schedule_rx(c);
}

/* One non-blocking write of a byte range on a carrier. -EAGAIN means the
 * carrier's TCP buffer is full and the caller must retry later. */
int tbt_carrier_send_raw(struct tbt_carrier *c, const void *data, size_t len)
{
    struct sock *sk;
    struct msghdr msg = {};
    struct kvec iov = { .iov_base = (void *)data, .iov_len = len };
    int err;

    if (!c)
        return -ENOTCONN;
    lockdep_assert_held(&c->sess->pump_lock);
    sk = rcu_dereference_protected(c->sk,
                                   lockdep_is_held(&c->sess->pump_lock));
    if (!sk)
        return -ENOTCONN;
    mutex_lock(&c->tx_lock);
    if (!tbt_carrier_sk_present(c))
    {
        mutex_unlock(&c->tx_lock);
        return -ENOTCONN;
    }
    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, len);
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(sk);
    err = tcp_sendmsg_locked(sk, &msg, len);
    release_sock(sk);
    mutex_unlock(&c->tx_lock);
    if (err < 0)
        return err;
    if ((size_t)err != len)
        return -EIO;
    return 0;
}

/* Flush the carrier's pending byte tail. A record may never be interleaved,
 * so every writer on this carrier calls this before a new record. The caller
 * holds sess->pump_lock (or is the only writer). */
static int tbt_carrier_part_flush(struct tbt_carrier *c, struct sock *sk)
{
    lockdep_assert_held(&c->tx_lock);
    while (c->part_off < c->part_len)
    {
        struct msghdr msg = {};
        struct kvec iov = {
            .iov_base = c->part + c->part_off,
            .iov_len = c->part_len - c->part_off,
        };
        int err;

        iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, iov.iov_len);
        msg.msg_flags = MSG_DONTWAIT;
        lock_sock(sk);
        err = tcp_sendmsg_locked(sk, &msg, iov.iov_len);
        release_sock(sk);
        if (err < 0)
            return err == -EAGAIN ? -EAGAIN : err;
        if (!err)
            return -EAGAIN;
        c->part_off += (u32)err;
    }
    c->part_len = c->part_off = 0;
    return 0;
}

/* Ask the stack to call tbt_car_write_space() on the next ACK. */
static void tbt_carrier_arm_nospace(struct tbt_carrier *c, struct sock *sk)
{
    if (sk && sk->sk_socket)
    {
        set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
        atomic64_inc(&c->sess->carrier_nospace_armed);
    }
}

/* Commit a segment list with one non-blocking socket write. Flush an older
 * tail first, then pre-check the socket's free send memory so ordinary
 * backpressure is -EAGAIN, not a partially written batch. A kernel short
 * write keeps the unwritten tail in c->part; the next writer on this carrier
 * flushes it before any new bytes, so records stay in order and a control
 * frame is never spliced into the middle of a DATA record.
 *
 * The payload segments point straight at the caller's records: a full write
 * copies no record byte and allocates nothing. Only a short write copies its
 * tail, and a batch tail always fits c->part because total is bounded by it. */
static int tbt_carrier_send_segs(struct tbt_carrier *c, struct sock *sk,
                                 const struct tbt_fec_batch_seg *segs,
                                 unsigned int nseg, size_t total)
{
    struct msghdr msg = {};
    struct kvec iov[TBT_BATCH_SEG_MAX];
    u32 wmem, sndbuf;
    u64 mem_avail;
    unsigned int i;
    int err;

    lockdep_assert_held(&c->tx_lock);
    if (!sk)
        return -ENOTCONN;
    if (!nseg || nseg > TBT_BATCH_SEG_MAX || total > TBT_CAR_PART_MAX)
        return -EINVAL;
    err = tbt_carrier_part_flush(c, sk);
    if (err)
    {
        if (err == -EAGAIN)
            tbt_carrier_arm_nospace(c, sk);
        return err;
    }
    wmem = READ_ONCE(sk->sk_wmem_queued);
    sndbuf = READ_ONCE(sk->sk_sndbuf);
    mem_avail = sndbuf > wmem ? sndbuf - wmem : 0;
    if (mem_avail < total)
    {
        /* The write needs its whole byte range to fit, so this can refuse
         * while sk_stream_memory_free() is still true.  Arm SOCK_NOSPACE:
         * the next ACK calls tbt_car_write_space(), which kicks the FEC
         * drain immediately.  The drain keeps its 2 ms timer as the bounded
         * fallback for a stack that never calls back. */
        tbt_carrier_arm_nospace(c, sk);
        return -EAGAIN;
    }
    for (i = 0; i < nseg; i++)
    {
        iov[i].iov_base = (void *)segs[i].base;
        iov[i].iov_len = segs[i].len;
    }
    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, iov, nseg, total);
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(sk);
    err = tcp_sendmsg_locked(sk, &msg, total);
    release_sock(sk);
    if (err < 0)
    {
        if (err == -EAGAIN)
            tbt_carrier_arm_nospace(c, sk);
        return err == -EAGAIN ? -EAGAIN : err;
    }
    if ((size_t)err < total)
    {
        u32 tail;

        if (!tbt_fec_batch_tail(segs, nseg, total, (size_t)err, c->part,
                                TBT_CAR_PART_MAX, &tail))
            return -EIO;
        c->part_len = tail;
        c->part_off = 0;
    }
    return 0;
}

/* Write one whole record or HTTP frame. The batch helper has the locking,
 * tail-flush, memory-precheck and short-write rules; this wrapper supplies
 * the one-segment list a single write needs. */
static int tbt_carrier_send_whole(struct tbt_carrier *c, struct sock *sk,
                                  const void *data, size_t len)
    {
    struct tbt_fec_batch_seg seg;

    if (len > TBT_CAR_PART_MAX)
        return -EINVAL;
    seg.base = data;
    seg.len = (u32)len;
    return tbt_carrier_send_segs(c, sk, &seg, 1, len);
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

/* Start the HTTP container for this carrier if it has not started yet. The
 * near side writes the request block, the far side the response block; a far
 * carrier waits until the request header has been parsed. Returns 0, -EAGAIN
 * when the far side is not ready, or a negative write error. */
static int tbt_carrier_http_begin(struct tbt_carrier *c, struct sock *sk)
{
    struct tbt_session *sess = c->sess;
    lockdep_assert_held(&c->tx_lock);
    char *hdr;
    size_t hlen;
    int err;

    mutex_lock(&c->http_lock);
    if (c->http_started)
    {
        mutex_unlock(&c->http_lock);
        return 0;
    }
    TBT_DIAG("http begin tunnel=%llu carrier=%u gen=%u role=%u req_done=%d profile=%u",
             (unsigned long long)sess->tunnel->cfg.id, c->index, c->gen,
             sess->tunnel->cfg.role, c->http_req_done, c->profile);
    /* 8 KiB on the stack trips -Wframe-larger-than; the caller is sleepable,
     * so allocate. */
    hdr = kmalloc(TBT_HTTP_REQ_MAX, GFP_KERNEL);
    if (!hdr)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u gen %u: HTTP header alloc failed: -ENOMEM\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            c->index, c->gen);
        mutex_unlock(&c->http_lock);
        return -ENOMEM;
    }
    if (sess->tunnel->cfg.role == TBT_ROLE_FAR && !c->http_req_done)
    {
        kfree(hdr);
        mutex_unlock(&c->http_lock);
        return -EAGAIN;
    }
    hlen = tbt_http_header_build(c, hdr, TBT_HTTP_REQ_MAX);
    if (!hlen)
    {
        kfree(hdr);
        mutex_unlock(&c->http_lock);
        atomic64_inc(&sess->http_parse_err);
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u gen %u: HTTP header build failed: -EIO\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            c->index, c->gen);
        return -EIO;
    }
    if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && c->index == 0)
    {
        size_t cap = min_t(size_t, hlen, sizeof(sess->http_cap_req));

        memcpy(sess->http_cap_req, hdr, cap);
        WRITE_ONCE(sess->http_cap_req_len, (u16)cap);
    }
    err = tbt_carrier_send_whole(c, sk, hdr, hlen);
    TBT_DIAG("http header write tunnel=%llu carrier=%u gen=%u ret=%d hlen=%zu",
             (unsigned long long)sess->tunnel->cfg.id, c->index, c->gen, err,
             hlen);
    kfree(hdr);
    if (err)
    {
        mutex_unlock(&c->http_lock);
        return err;
    }
    c->http_started = true;
    c->last_tx_ns = ktime_get_ns();
    c->tx_bytes += hlen;
    sess->carrier_bytes += hlen;
    if (sess->tunnel->cfg.role == TBT_ROLE_NEAR)
        sess->http_req++;
    else
        sess->http_resp++;
    mutex_unlock(&c->http_lock);
    return 0;
}

/* Write one FEC record. raw profile sends the 40-byte record directly; the
 * HTTP profile wraps exactly this record in one chunk. Kept as a wrapper over
 * the batch path so both use the same per-carrier lock and tail handling. */
int tbt_carrier_send_fec(struct tbt_carrier *c, const u8 *rec, size_t len)
{
    u16 l;

    if (!c || !rec || len > U16_MAX)
        return -EINVAL;
    l = (u16)len;
    return tbt_carrier_send_fec_batch(c, &rec, &l, 1, NULL);
}


/* Write up to TBT_FEC_BATCH_MAX FEC records with one HTTP header and one
 * socket write.
 * Only c->tx_lock is held; sess->pump_lock is not required, so each carrier's
 * drain work can run on its own CPU. The pump's ARQ/control writes take the
 * same c->tx_lock, which keeps one carrier's byte stream ordered without
 * serialising all 256 carriers. */
int tbt_carrier_send_fec_batch(struct tbt_carrier *c,
                               const u8 *const *recs, const u16 *lens,
                               u16 count, u16 *sent)
{
    struct tbt_session *sess;
    struct sock *sk;
    char hdr[TBT_BATCH_REC_MAX][TBT_HTTP_CHUNK_HDR_MAX];
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    size_t total = 0;
    unsigned int nseg;
    bool http;
    int err = 0;

    if (sent)
        *sent = 0;
    if (!c || !recs || !lens || !count || count > TBT_FEC_BATCH_MAX)
        return -EINVAL;
    /* Real FEC records are entering the byte stream: stop restoring the idle
     * BBR window; the ACKs for these records own the next decision. */
    WRITE_ONCE(c->hold_active, false);
    WRITE_ONCE(c->hold_since_ns, 0);
    sess = c->sess;
    sk = tbt_carrier_sk_get(c);
    if (!sk)
        return -ENOTCONN;

    mutex_lock(&c->tx_lock);
    if (atomic_read(&c->stopping) || !tbt_carrier_sk_present(c))
    {
        err = -ENOTCONN;
        goto out;
    }

    http = c->profile != 3;
    if (http)
    {
        /* The header block is part of this carrier's byte stream: write it
         * once before the chunks. */
        err = tbt_carrier_http_begin(c, sk);
        if (err)
            goto out;
    }
    /* The records are already one contiguous buffer each (struct tbt_fec_out
     * data[]), so the layout is a segment list over them: no batch buffer,
     * no record memcpy. */
    nseg = tbt_fec_batch_build(seg, &hdr[0][0], recs, lens, count, http, &total);
    if (!nseg)
    {
        if (http)
            atomic64_inc(&sess->http_parse_err);
        err = http ? -EIO : -EINVAL;
        goto out;
    }

    err = tbt_carrier_send_segs(c, sk, seg, nseg, total);
    if (!err)
    {
        c->tx_frames += count;
        c->tx_bytes += total;
        c->last_tx_ns = ktime_get_ns();
        sess->carrier_bytes += total;
        if (sent)
            *sent = count;
    }

out:
    mutex_unlock(&c->tx_lock);
    tbt_carrier_sk_put(sk);
    return err;
}

// Write one whole record while c->tx_lock is held. Backpressure is the
// caller's problem: tbt_tx only calls this while sk_wmem_queued is below the
// high-water mark.
static int tbt_carrier_xmit_locked(struct tbt_session *sess,
                                   struct tbt_carrier *c,
                                   struct tbt_frame *fr, struct sock *sk)
{
    struct msghdr msg = {};
    struct kvec iov[2];
    u8 hdrbuf[TBT_HDR_LEN];
    size_t total;
    int err;
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
        c->drop_snd_nxt = READ_ONCE(tcp_sk(sk)->snd_nxt);
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

        err = tbt_carrier_http_begin(c, sk);
        if (err)
            return err;


        h.body_len = fr->len;
        out.hdr = &h;
        out.body = fr->len ? fr->data : NULL;
        n = tbt_http_frame_encode(c->http_tx,
                                  sess->fec ? TBT_HTTP_CTL_FRAME_MAX
                                            : TBT_HTTP_FRAME_MAX,
                                  &out);
        if (!n)
        {
            /* The encoder never emits partial output; a 0 is a bug or a short
             * buffer, and either way this carrier must not be trusted. */
            atomic64_inc(&sess->http_parse_err);
            return -EIO;
        }
        if (sess->tunnel->cfg.role == TBT_ROLE_NEAR && c->index == 0)
            tbt_http_capture_chunk(sess, c->http_tx, n);
        err = tbt_carrier_send_whole(c, sk, c->http_tx, n);
        if (fr->hdr.type == TBT_F_HELLO && err != -EAGAIN)
            TBT_DIAG("HELLO chunk write tunnel=%llu carrier=%u gen=%u ret=%d len=%zu write_seq=%u snd_una=%u sndbuf=%u wmem=%u",
                     (unsigned long long)sess->tunnel->cfg.id, c->index,
                     c->gen, err, n,
                     READ_ONCE(tcp_sk(sk)->write_seq),
                     READ_ONCE(tcp_sk(sk)->snd_una),
                     READ_ONCE(sk->sk_sndbuf),
                     READ_ONCE(sk->sk_wmem_queued));
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
    err = tbt_carrier_part_flush(c, sk);
    if (err)
        return err;
    /* Record-level all-or-nothing for DATA. A short write leaves a partial
     * record in the ordered stream; the peer's parser cannot skip it, so
     * every control record written after it (FBK, SACK, FIN) waits behind an
     * incomplete record. Measured on a 64 KiB carrier: the far parsed 0 of
     * 253 FBKs, the near-to-far uack never advanced, and the strict phase
     * ended in EOF. Check the socket's free send memory and the peer's
     * remaining window for the whole record first; if it does not fit, let
     * the pump requeue it and retry on write_space. The c->part path above
     * stays as a safety net for a kernel-side short write. */
    if (fr->hdr.type == TBT_F_DATA)
    {
        struct tcp_sock *tp = tcp_sk(sk);
        u32 una = READ_ONCE(tp->snd_una);
        u32 queued = READ_ONCE(tp->write_seq);
        u32 wnd = READ_ONCE(tp->snd_wnd);
        u32 wmem = READ_ONCE(sk->sk_wmem_queued);
        u32 sndbuf = READ_ONCE(sk->sk_sndbuf);
        u64 mem_avail = sndbuf > wmem ? sndbuf - wmem : 0;
        u64 wnd_avail = wnd > (u32)(queued - una) ?
                        wnd - (u32)(queued - una) : 0;

        if (mem_avail < total)
        {
            if (fr->retx_copy)
            {
                sess->dbg_retx_memshort++;
                if (debug >= 1)
                    pr_info_ratelimited("tbtun: retx memshort seq=%llu len=%u total=%zu mem_avail=%llu sndbuf=%u wmem=%u wnd_avail=%llu\n",
                                        fr->hdr.seq, fr->len, total, mem_avail,
                                        sndbuf, wmem, wnd_avail);
            }
            return -EAGAIN;
        }
        /* A retransmit copy is admitted past the peer window.
         * tcp_sendmsg_locked() queues into the socket's write memory
         * regardless of the advertised window, and TCP sends the record when
         * the window opens; the record stays atomic because it is one
         * contiguous byte range in the write queue. Refusing here kept the
         * copy out of the queue exactly while the window was closed, so the
         * next window update had nothing to carry and the receiver's hole
         * budget expired first (measured: retx_evident=255, retx_cand=6,
         * retx_frames=0, far holes_unrecoverable=1). New data keeps the
         * window test: it must not park its tail in zero-window persist. */
        if (wnd_avail < total)
        {
            if (!fr->retx_copy)
                return -EAGAIN;
            sess->dbg_retx_wndshort++;
            if (debug >= 1)
                pr_info_ratelimited("tbtun: retx wndshort seq=%llu len=%u total=%zu wnd=%u inflight=%u wnd_avail=%llu mem_avail=%llu\n",
                                    fr->hdr.seq, fr->len, total, wnd,
                                    (u32)(queued - una), wnd_avail, mem_avail);
        }
    }
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(sk);
    err = tcp_sendmsg_locked(sk, &msg, total);
    release_sock(sk);
    if (err == -EAGAIN)
        return -EAGAIN;
    if (err < 0)
    {
        sess->carrier_err++;
        if (fr->hdr.type == TBT_F_HELLO)
            pr_warn_ratelimited("tbtun: tunnel %llu carrier %u gen %u: HELLO write failed: %d total=%zu\n",
                                (unsigned long long)sess->tunnel->cfg.id,
                                c->index, c->gen, err, total);
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
        /* total may be a multi-frame batch; c->part is one frame. A tail
         * larger than that cannot be replayed without another buffer. */
        if (tail_h + tail_d > TBT_CAR_PART_MAX)
            return -EIO;
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
        c->part_len = (u32)(tail_h + tail_d);
        c->part_off = 0;
    }
    if (fr->hdr.type == TBT_F_HELLO)
        TBT_DIAG("HELLO raw write tunnel=%llu carrier=%u gen=%u ret=%d total=%zu short=%d write_seq=%u snd_una=%u",
                 (unsigned long long)sess->tunnel->cfg.id, c->index, c->gen,
                 err, total, (size_t)err != total,
                 READ_ONCE(tcp_sk(sk)->write_seq),
                 READ_ONCE(tcp_sk(sk)->snd_una));
    c->tx_frames++;
    c->tx_bytes += total;
    if (fr->hdr.type == TBT_F_DATA)
        c->data_frames++;
    c->last_tx_ns = ktime_get_ns();
    sess->carrier_bytes += total;
    return 0;
}

int tbt_carrier_xmit(struct tbt_session *sess, struct tbt_carrier *c,
                     struct tbt_frame *fr)
{
    struct sock *sk;
    int err;

    if (!c)
        return -ENOTCONN;
    /* PAD is keepalive, not real data: it must not cancel a hold. Any other
     * record means the carrier is active again. */
    if (fr && fr->hdr.type != TBT_F_PAD)
    {
        WRITE_ONCE(c->hold_active, false);
        WRITE_ONCE(c->hold_since_ns, 0);
    }
    lockdep_assert_held(&sess->pump_lock);
    sk = rcu_dereference_protected(c->sk,
                                   lockdep_is_held(&sess->pump_lock));
    if (!sk)
        return -ENOTCONN;
    /* The pump and a carrier's FEC drain share this byte stream. The pump
     * holds pump_lock; the drain holds only this carrier's tx_lock. Both take
     * tx_lock around one complete record, so records never interleave. */
    mutex_lock(&c->tx_lock);
    if (atomic_read(&c->stopping) || !tbt_carrier_sk_present(c))
        err = -ENOTCONN;
    else
        err = tbt_carrier_xmit_locked(sess, c, fr, sk);
    mutex_unlock(&c->tx_lock);
    return err;
}

// ---------------------------------------------------------------------------
// Carrier lifecycle

/* The caller holds pump_lock. Every carrier is a fresh allocation: object
 * identity never changes, so a reader that obtained a pointer from car[],
 * prov, /proc or a work item keeps a stable object until its own put. */
static struct tbt_carrier *tbt_carrier_new_locked(struct tbt_session *sess,
                                                  u8 index)
{
    struct tbt_carrier *c;
    bool spare = index < sess->n_car && index >= sess->n_pri;
    u8 profile = tbt_carrier_profile(sess, index, spare);

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c)
        return NULL;
    /* Before the first allocation that can fail: the failure path frees
     * through tbt_carrier_free(), which unlinks the nodes and returns the
     * session reference, and an all-zero node is not a valid list head. */
    INIT_LIST_HEAD(&c->node);
    INIT_LIST_HEAD(&c->release_node);
    refcount_set(&c->refs, 1);
    refcount_inc(&sess->refs);
    c->sess = sess;
    /* The raw container (profile 3) writes the 32-byte record header
     * directly: it has no HTTP framing to encode and no chunk parser to run.
     * Its three HTTP buffers would never be read, so a 256-carrier raw
     * session would hold 36 KiB of dead slab per carrier. */
    if (profile != 3)
    {
        /* A FEC session carries no ARQ DATA frame, so its encoder buffer only
         * has to hold a control record. The encode call below is handed the
         * same cap; a record that does not fit returns 0 and poisons the
         * carrier instead of overflowing the buffer. */
        c->http_tx = kmalloc(sess->fec ? TBT_HTTP_CTL_FRAME_MAX
                                       : TBT_HTTP_FRAME_MAX,
                             GFP_KERNEL);
        c->http_hdr = kmalloc(TBT_HTTP_HDR_MAX, GFP_KERNEL);
        c->http_rx = kzalloc(sizeof(*c->http_rx), GFP_KERNEL);
    }
    c->part = kmalloc(TBT_CAR_PART_MAX, GFP_KERNEL);
    /* Zeroed is the defined initial state: have == 0 for the parser and a
     * SIZE-state chunk parser for the assembler. kmalloc() would hand the
     * carrier whatever the previous slab user left there; a dirty `have`
     * then drives an unbounded memmove() in tbt_http_rx_parse(). */
    c->parser = kzalloc(sizeof(*c->parser), GFP_KERNEL);
    if (!c->part || !c->parser ||
        (profile != 3 && (!c->http_tx || !c->http_hdr || !c->http_rx)))
    {
        /* ctor-fail: never linked, so the helper's unlink is a no-op. */
        tbt_carrier_free(c);
        return NULL;
    }
    c->index = index;
    c->gen = ++sess->carrier_gen;
    /* 0xff is the provisional far-side index: not a spare, and the join sets
     * the real role when HELLO names the slot. */
    c->spare = spare;
    c->active = !c->spare;
    c->profile = profile;
    c->state = TBT_CAR_CONNECTING;
    c->class = sess->tunnel ? sess->tunnel->cfg.profile : 1;
    c->join_deadline_ns = ktime_get_ns() + (u64)TBT_JOIN_TIMEOUT_MS * NSEC_PER_MSEC;
    c->last_una_move_ns = ktime_get_ns();
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u allocated spare=%d profile=%u state=%u join_deadline_ms=%u parser_have=%zu http_chunk_state=%d",
             (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
             index, c->gen, c->spare, c->profile, c->state,
             TBT_JOIN_TIMEOUT_MS, c->parser->have,
             c->http_rx ? c->http_rx->chunk.state : -1);
    mutex_init(&c->http_lock);
    mutex_init(&c->tx_lock);
    INIT_WORK(&c->w_connect, tbt_car_connect_work);
    INIT_WORK(&c->w_rx, tbt_car_rx_work);
    INIT_DELAYED_WORK(&c->w_pump, tbt_pump_work);
    return c;
}

struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c;

    /* pump_lock serialises the birth with a running pump and with the slot
     * swap in w_replace. */
    mutex_lock(&sess->pump_lock);
    c = tbt_carrier_new_locked(sess, index);
    mutex_unlock(&sess->pump_lock);
    return c;
}

/* Pause a live carrier whose snd_una stopped moving. This is the soft half of
 * a loss: the pump stops giving the path new frames, but nothing about the
 * carrier dies. No RETIRE, no replacement, no socket teardown, and the slot
 * keeps its generation; the poll clears c->stalled and counts stall_recovered
 * when snd_una moves again. Only the FEC container calls this: under ARQ the
 * same stall keeps the kill-and-repath behaviour of tbt_carrier_lost().
 *
 * The shares already in flight belong to the FEC data plane: it treats them as
 * lost and parity rebuilds them (or its 30 s bound resets the block). This
 * function deliberately drops nothing itself, so it never takes pump_lock and
 * never sleeps. */
void tbt_carrier_stall(struct tbt_session *sess, struct tbt_carrier *c)
{
    struct sock *sk;

    if (!sess || !c || READ_ONCE(sess->closing) || atomic_read(&c->stopping))
        return;
    if (READ_ONCE(c->stalled))
    {
        /* Already paused: refresh the stall clock only. The poll re-enters
         * this branch every carrier_stall_ms while the path stays silent, and
         * one stall must not be counted twice. */
        c->last_una_ns = ktime_get_ns();
        return;
    }
    WRITE_ONCE(c->stalled, true);
    c->stall_since_ns = ktime_get_ns();
    /* Baseline for the stall_dead test: a wire that still moves packets is
     * alive even when no byte is acknowledged yet. */
    sk = tbt_carrier_sk_get(c);
    if (sk)
    {
        c->stall_packets_out = READ_ONCE(tcp_sk(sk)->packets_out);
        tbt_carrier_sk_put(sk);
    }
    /* Restart the window, so the 50 ms poll does not re-declare the same
     * stall on its next pass. */
    c->last_una_ns = c->stall_since_ns;
    atomic64_inc(&sess->stall_declared);
    pr_info_ratelimited("tbtun: carrier %u stalled: repath paused, new frames avoid it\n",
                        c->index);
}

/* Discrete RTO grid for the survival estimate. A stall that recovered or was
 * replaced at t_ns has survived every grid point below t_ns. */
static const u64 tbt_stall_grid_ns[TBT_STALL_GRID] = {
    500ULL * NSEC_PER_MSEC, 1ULL * NSEC_PER_SEC, 2ULL * NSEC_PER_SEC,
    4ULL * NSEC_PER_SEC, 8ULL * NSEC_PER_SEC, 16ULL * NSEC_PER_SEC,
};

/* Session send rate in bytes per second, measured from the DATA bytes the
 * pump actually handed to carriers. window/RTT is only the initial value: it
 * describes what the path could carry, and it overstates a receive-heavy
 * session (the far side of a download sends little but has a large window),
 * which made L_wait look far larger than the bytes at stake. */
static u64 tbt_session_rate_bps(struct tbt_session *sess)
{
    u64 now = ktime_get_ns();
    u64 tx = READ_ONCE(sess->stats_tx);

    if (sess->rate_sample_ns && now > sess->rate_sample_ns)
    {
        u64 dt = now - sess->rate_sample_ns;

        if (dt >= 200ULL * NSEC_PER_MSEC)
        {
            u64 db = tx - sess->rate_sample_bytes;
            u64 r = db * NSEC_PER_SEC / dt;

            sess->rate_bps_ewma = sess->rate_bps_ewma ?
                (7 * sess->rate_bps_ewma + r) / 8 : r;
            sess->rate_sample_ns = now;
            sess->rate_sample_bytes = tx;
        }
    }
    else
    {
        sess->rate_sample_ns = now;
        sess->rate_sample_bytes = tx;
    }
    if (sess->rate_bps_ewma)
        return sess->rate_bps_ewma;
    {
        u32 win = READ_ONCE(sess->tx_window);
        u32 rtt = READ_ONCE(sess->rtt_mean_us);

        return win && rtt ? (u64)win * 1000000ULL / rtt : 0;
    }
}

/* Record one finished stall in the survival grid. recovered == false is a
 * censored observation (replaced or declared dead): it counts in the risk set
 * at every grid point it reached, but not as an event, so a replacement never
 * looks like a recovery. */
static void tbt_stall_grid_note(struct tbt_session *sess, u64 t_ns,
                                bool recovered)
{
    int i, bucket = TBT_STALL_GRID - 1;

    for (i = 0; i < TBT_STALL_GRID; i++)
    {
        if (t_ns > tbt_stall_grid_ns[i])
            sess->stall_grid_risk[i]++;
        else
        {
            bucket = i;
            break;
        }
    }
    if (recovered)
        sess->stall_grid_events[bucket]++;
    else
        sess->stall_grid_censored[bucket]++;
    sess->stall_recover_samples++;
    if (recovered)
        sess->stall_recover_ns_ewma =
            sess->stall_recover_samples > 1 ?
            (7 * sess->stall_recover_ns_ewma + t_ns) / 8 : t_ns;
}

/* P(recovery time > t_ns). Prior: RTO backoff, a Pareto-like tail T/(T+t)
 * with T = max(2 RTO, 3 RTTVAR), which makes the conditional survival rise
 * with t (the longer the silence, the longer the remaining wait is likely to
 * be). The RTTVAR term matters on a WAN with 1-2.8 s of jitter: 2 RTO alone
 * described a healthy path's own retransmission schedule as nearly permanent.
 * Once enough finished stalls exist, the Kaplan-Meier product over the grid
 * replaces the prior. */
static u32 tbt_stall_survival_ppm(struct tbt_session *sess, u64 t_ns,
                                  u64 rto_ns, u64 rttvar_ns, u32 *bucket)
{
    u64 t0 = max_t(u64, 2 * (rto_ns ? rto_ns : NSEC_PER_SEC), 3 * rttvar_ns);
    u64 s = 1000000;
    int i, b = TBT_STALL_GRID - 1;

    for (i = 0; i < TBT_STALL_GRID; i++)
    {
        if (t_ns <= tbt_stall_grid_ns[i])
        {
            b = i;
            break;
        }
    }
    if (bucket)
        *bucket = (u32)b;
    if (sess->stall_recover_samples < TBT_STALL_GRID + 2)
        return (u32)(1000000ULL * t0 / (t0 + t_ns));
    for (i = 0; i <= b; i++)
    {
        u32 n = sess->stall_grid_risk[i];
        u32 d = sess->stall_grid_events[i];

        if (n && d)
            s = s * (n - min(d, n)) / n;
    }
    return (u32)min_t(u64, s, 1000000);
}

/* Bytes a replacement costs: the new carrier's handshake plus its slow-start
 * ramp at the carrier's share of the rate, plus the bytes already in flight
 * that the loss must repeat (ARQ retain copies, FEC NACK shares). The last
 * computed value is smoothed into replace_cost_ewma. */
static u64 tbt_stall_replace_cost(struct tbt_session *sess,
                                  struct tcp_sock *tp, u64 r_c)
{
    u64 count = atomic64_read(&sess->connect_ns_count);
    u64 connect_ns = 100ULL * NSEC_PER_MSEC; /* default before samples */
    u64 slow_ns;
    u64 inflight;
    u64 cost;

    if (count)
        connect_ns = atomic64_read(&sess->connect_ns_sum) / count;
    /* Slow start back to the current rate: about three RTTs of ramp. */
    slow_ns = 3ULL * (u64)(READ_ONCE(tp->srtt_us) >> 3) * NSEC_PER_USEC;
    inflight = (u64)READ_ONCE(tp->packets_out) * READ_ONCE(tp->mss_cache);
    cost = r_c * (connect_ns + slow_ns) / NSEC_PER_SEC + inflight;
    sess->replace_cost_ewma = sess->replace_cost_ewma ?
        (7 * sess->replace_cost_ewma + cost) / 8 : cost;
    return cost;
}

/* Optimal-stopping rule. Waiting one decision window (one RTO) costs
 *   L_wait(t) = R_c * integral_0^dt S(y|t) dy + P(Y > dt | t) * V(t+dt)
 * and replacing costs
 *   L_replace = C_r + R_c * T_r.
 * V(t+dt) is L_replace itself: a path that stays silent ends in a
 * replacement anyway, only later. The integral is the trapezoid over the
 * conditional survival. Replace only when L_wait exceeds L_replace by the
 * hysteresis margin, and only after two RTOs of silence and one T_r since
 * the last replacement. TCP retransmission exhaustion skips the economics.
 *
 * The far side never replaces: it cannot dial, so a replacement there only
 * clears the slot and forces the near side to redial, discarding the shares
 * already in flight. It waits for the near side's RETIRE or recovery. */
#define TBT_STALL_EPS_PCT 20
static bool tbt_stall_should_replace(struct tbt_carrier *c, u64 now)
{
    struct tbt_session *sess = c->sess;
    struct sock *sk;
    struct tcp_sock *tp;
    u64 t_ns, rto_ns, rttvar_ns, dt, t_r, r_c, rate, c_r, l_replace, l_wait;
    u64 s_t, s_next, p_wait, mean_ns;
    u32 retrans, bucket;
    u8 reason;

    sk = tbt_carrier_sk_get(c);
    if (!sk)
        return true; /* no socket: nothing left to wait for */
    if (!sess->tunnel || sess->tunnel->cfg.role == TBT_ROLE_FAR)
    {
        WRITE_ONCE(sess->stall_replace_reason, TBT_STALL_REPLACE_NONE);
        tbt_carrier_sk_put(sk);
        return false;
    }
    tp = tcp_sk(sk);
    rttvar_ns = (u64)READ_ONCE(tp->mdev_us) * NSEC_PER_USEC;
    rto_ns = max_t(u64, (u64)(READ_ONCE(tp->srtt_us) >> 3) +
                        4ULL * READ_ONCE(tp->mdev_us),
                   200000ULL) * NSEC_PER_USEC;
    t_ns = now - c->stall_since_ns;
    retrans = READ_ONCE(inet_csk(sk)->icsk_retransmits);
    /* Debounce: two RTOs of silence before the first judgment, so one lost
     * ACK plus the RTO backoff cannot look like a dead path; and give a
     * replacement T_r to ramp before replacing again. */
    dt = rto_ns;
    {
        u64 count = atomic64_read(&sess->connect_ns_count);

        t_r = (count ? atomic64_read(&sess->connect_ns_sum) / count :
                       100ULL * NSEC_PER_MSEC) +
              3ULL * (u64)(READ_ONCE(tp->srtt_us) >> 3) * NSEC_PER_USEC;
    }
    if (retrans < TBT_STALL_RETX_MAX &&
        (t_ns < 2 * dt ||
         (sess->last_stall_replace_ns &&
          now - sess->last_stall_replace_ns < t_r)))
    {
        WRITE_ONCE(sess->stall_replace_reason, TBT_STALL_REPLACE_NONE);
        tbt_carrier_sk_put(sk);
        return false;
    }
    rate = tbt_session_rate_bps(sess);
    /* The carrier's share of the measured session rate. */
    {
        u16 ncar = READ_ONCE(sess->n_car);

        r_c = rate / (ncar ? ncar : 1);
    }
    s_t = tbt_stall_survival_ppm(sess, t_ns, rto_ns, rttvar_ns, &bucket);
    s_next = tbt_stall_survival_ppm(sess, t_ns + dt, rto_ns, rttvar_ns, NULL);
    p_wait = s_t ? min_t(u64, s_next * 1000000 / s_t, 1000000) : 0;
    mean_ns = dt * (1000000 + p_wait) / 2000000;
    c_r = tbt_stall_replace_cost(sess, tp, r_c);
    l_replace = c_r + r_c * t_r / NSEC_PER_SEC;
    l_wait = r_c * mean_ns / NSEC_PER_SEC +
             p_wait * l_replace / 1000000;
    if (retrans >= TBT_STALL_RETX_MAX)
        reason = TBT_STALL_REPLACE_RETRANS;
    else if (rate && l_wait * 100 > l_replace * (100 + TBT_STALL_EPS_PCT))
        reason = TBT_STALL_REPLACE_ECONOMIC;
    else
        reason = TBT_STALL_REPLACE_NONE;
    sess->stall_survival_bucket = bucket;
    sess->stall_wait_loss_bytes = l_wait;
    sess->replace_cost_bytes = c_r;
    WRITE_ONCE(sess->stall_replace_reason, reason);
    tbt_carrier_sk_put(sk);
    return reason != TBT_STALL_REPLACE_NONE;
}

/* True when no slot can still carry bytes: every carrier in sess->car[] has
 * been marked stopping. This is the only carrier-side state that may reset a
 * stream. One dead path is a repath, and a slot whose replacement is still
 * dialling holds a fresh, non-stopping object, so neither counts as a failure
 * of the transport. Called from tbt_carrier_lost(), which may run in softirq,
 * so it uses only RCU and refcounts. */
static bool tbt_carriers_all_dead(struct tbt_session *sess)
{
    int i;

    for (i = 0; i < READ_ONCE(sess->n_car); i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (!atomic_read(&c->stopping))
        {
            tbt_carrier_put(c);
            return false;
        }
        tbt_carrier_put(c);
    }
    return true;
}

/* Grow an ARQ stripe by one carrier at the next free index. FEC never grows:
 * its share mapping is fixed at cfg.carriers, so a new index would renumber
 * the blocks already in flight; it replaces the stalled carrier instead.
 * Returns 0, -EOPNOTSUPP (not an ARQ near session), -ENOSPC (module limit),
 * -EBUSY, or -ENOMEM. Runs from the poll work, so it may sleep.
 *
 * Reserved until the far side can extend its own n_car on join: today
 * tbt_session_join() refuses an index >= sess->n_car, so a grown stripe would
 * spend dials on carriers the peer rejects. No caller; carrier_add and
 * carrier_add_fail therefore stay 0. */
static int __maybe_unused tbt_carrier_add(struct tbt_session *sess)
{
    struct tbt_carrier *c;
    u16 idx;

    if (!sess || READ_ONCE(sess->closing) || sess->fec)
        return -EOPNOTSUPP;
    if (!sess->tunnel || sess->tunnel->cfg.role != TBT_ROLE_NEAR)
        return -EOPNOTSUPP;
    idx = READ_ONCE(sess->n_car);
    if (idx >= TBT_MAX_CARRIERS_LIMIT || idx >= READ_ONCE(max_carriers))
        return -ENOSPC;
    if (READ_ONCE(sess->car[idx]))
        return -EBUSY;
    if (sess->last_carrier_add_ns &&
        ktime_get_ns() - sess->last_carrier_add_ns <
            (u64)TBT_CARRIER_ADD_COOLDOWN_MS * NSEC_PER_MSEC)
        return -EAGAIN;
    mutex_lock(&sess->pump_lock);
    if (READ_ONCE(sess->n_car) != idx || READ_ONCE(sess->car[idx]))
    {
        mutex_unlock(&sess->pump_lock);
        return -EBUSY;
    }
    c = tbt_carrier_new_locked(sess, (u8)idx);
    if (!c)
    {
        mutex_unlock(&sess->pump_lock);
        return -ENOMEM;
    }
    /* New capacity is primary: index == n_car, so new_locked() derived
     * spare = false and the full profile. */
    rcu_assign_pointer(sess->car[idx], c);
    WRITE_ONCE(sess->n_car, (u16)(idx + 1));
    mutex_unlock(&sess->pump_lock);
    sess->last_carrier_add_ns = ktime_get_ns();
    atomic64_inc(&sess->carrier_add);
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u added n_car=%u",
             (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
             c->index, c->gen, READ_ONCE(sess->n_car));
    return 0;
}

/* Backpressure is the last resort. Growth is disabled until the far side can
 * extend n_car, so "no carrier can carry" is the whole condition: every slot
 * is stopping and the replacement worker has not produced a live one yet.
 * Count the edge once; the user socket's own window does the actual stopping,
 * and this layer never drops a frame or resets a stream for it. */
static void tbt_carrier_capacity_check(struct tbt_session *sess)
{
    if (!sess || !sess->tunnel || READ_ONCE(sess->closing))
        return;
    if (!tbt_carriers_all_dead(sess))
    {
        WRITE_ONCE(sess->backpressure, false);
        return;
    }
    if (!READ_ONCE(sess->backpressure))
    {
        WRITE_ONCE(sess->backpressure, true);
        atomic64_inc(&sess->backpressure_enter);
        pr_info_ratelimited("tbtun: tunnel %llu no carrier can carry: user side backpressured\n",
                            (unsigned long long)sess->tunnel->cfg.id);
    }
}

/* Mark a carrier unusable once, count it, and ask the replacement worker to
 * put a new one at the same index. Safe from work, softirq and /proc context;
 * it never sleeps. */
void tbt_carrier_lost(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!sess || !c || READ_ONCE(sess->closing) || atomic_read(&c->stopping))
        return;
    atomic_set(&c->stopping, 1);
    /* The replacement has a different generation; drop the four-bit announce
     * marks for this slot or the new carrier can be mistaken for the old one
     * and never queue its OPEN (observed: carrier 3 gen 4 lost, replacement
     * gen 292 = 4 mod 16, up stayed 0). */
    tbt_carrier_unannounce(sess, c->index);
    c->state = TBT_CAR_STALLED;
    if (!c->lost_counted)
    {
        c->lost_counted = true;
        atomic64_inc(&sess->carrier_lost);
        /* Presence only; the socket state is in the debug snapshot and in
         * /proc instead. */
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u gen %u lost: sock=%d connected=%d hello_sent=%d up=%d\n",
                            (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id : 0),
                            c->index, c->gen,
                            tbt_carrier_sk_present(c),
                            atomic_read(&c->connected),
                            READ_ONCE(c->hello_sent), atomic_read(&c->up));
    }
    /* Tell the far to free this (index, gen) before the replacement dials.
     * Without it the replacement's HELLO hits join_dup and the two sides
     * storm (measured: 15 join_dup, 191 replacements, 32 s receiver hole). */
    if (sess->tunnel && sess->tunnel->cfg.role == TBT_ROLE_NEAR)
    {
        int j;

        for (j = 0; j < sess->n_car; j++)
        {
            struct tbt_carrier *live = tbt_carrier_get(sess, (u8)j);
            u8 idx = c->index;

            if (!live)
                continue;
            if (live == c || !atomic_read(&live->up) ||
                atomic_read(&live->stopping))
            {
                tbt_carrier_put(live);
                continue;
            }
            tbt_tx_control_pinned(sess, NULL, (u8)j, TBT_F_RETIRE, 0,
                                  (u64)(c->gen & 0xf), &idx, 1, true,
                                  GFP_ATOMIC);
            tbt_carrier_put(live);
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
    /* Containment: a tunnel reset must be a subset of a TCP failure. One dead
     * path is a repath, so it never resets a stream while any other carrier
     * can still carry bytes. Only a session whose every carrier has failed
     * may reset its streams here. */
    if (sess->join_retries > 20 && tbt_carriers_all_dead(sess))
    {
        struct tbt_stream *batch[16];
        unsigned int round;
        int n, i;

        atomic64_inc(&sess->stream_reset_tcp_failed);
        /* Carrier loss runs in atomic context. A small stack batch kills
         * every stream without allocating; the killable snapshot skips a
         * stream another killer already owns, so each round makes progress.
         * n_active is bounded by TBT_STREAMS_MAX, hence the round cap. */
        for (round = 0; round <= TBT_STREAMS_MAX / ARRAY_SIZE(batch);
             round++)
        {
            n = tbt_session_stream_snapshot_killable(sess, batch,
                                                     ARRAY_SIZE(batch));
            if (!n)
                break;
            for (i = 0; i < n; i++)
            {
                tbt_stream_kill(batch[i], TBT_ST_DEAD);
                tbt_stream_put(batch[i]);
            }
            if (n < (int)ARRAY_SIZE(batch))
                break;
        }
        return;
    }
    c->retry_at_ns = ktime_get_ns() + 50 * NSEC_PER_MSEC;
    tbt_session_arm(sess, TBT_SW_REPLACE, 0);
}

/* Stop every work item that can still touch this carrier. Sets stopping under
 * sess->lock, so tbt_carrier_arm_*() and this function cannot interleave; the
 * cancels run outside the lock because the session pump/w_pump take pump_lock and
 * cancel_work_sync() sleeps. The caller must not hold pump_lock or tx_lock. */
void tbt_carrier_quiesce(struct tbt_carrier *c)
{
    struct tbt_session *sess;

    if (!c)
        return;
    sess = c->sess;
    spin_lock(&sess->lock);
    atomic_set(&c->stopping, 1);
    spin_unlock(&sess->lock);
    cancel_work_sync(&c->w_connect);
    cancel_work_sync(&c->w_rx);
    cancel_delayed_work_sync(&c->w_pump);
}

/* Stage 1 of release: make c->sk unreachable and stop the callbacks. No
 * socket is closed here; the caller runs one synchronize_rcu() for the whole
 * batch, then tbt_carrier_close() on each object. */
static void tbt_carrier_unpublish(struct tbt_session *sess,
                                  struct tbt_carrier *c)
{
    struct socket *s = c->sock;

    if (!c)
        return;
    /* Take the per-carrier write lock before unpublishing. A FEC batch that
     * already obtained a socket reference either finishes before this section
     * or sees sk_present()==false after it and drops the batch. */
    mutex_lock(&c->tx_lock);
    if (s && s->sk)
    {
        /* write_lock_bh() inside restore waits for every in-flight callback;
         * after it returns no new pump/rx wake can be queued from sk_user_data. */
        tbt_sk_ops_restore(s->sk, &sess->def_ops);
        sock_replace_proto(s->sk, &tcp_prot);
    }
    rcu_assign_pointer(c->sk, NULL);
    mutex_unlock(&c->tx_lock);

    /* A callback that ran before the restore may have queued w_rx/w_pump or the session pump
     * after quiesce() cancelled them. They all check sk_present()/stopping
     * and return without touching the socket. */
    cancel_work_sync(&c->w_rx);
    cancel_delayed_work_sync(&c->w_pump);
}

/* Stage 2 of release: close the socket and drop the carrier's runtime state.
 * Called only after synchronize_rcu(), so no reader can still obtain the
 * socket pointer. Idempotent. */
static void tbt_carrier_close(struct tbt_carrier *c)
{
    struct tbt_session *sess = c->sess;
    struct socket *s = c->sock;

    if (!c)
        return;
    if (s)
    {
        tbt_sock_abort_on_close(s);
        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        c->sock = NULL;
    }
    atomic_set(&c->connected, 0);
    atomic_set(&c->up, 0);
    if (c->parser->have)
        sess->recv_partial_discard++;
    c->parser->have = 0;
    tbt_http_rx_new_message(c->http_rx);
    if (c->part_len || c->part_off)
        sess->recv_partial_discard++;
    c->part_len = c->part_off = 0;
    if (c->local_port)
    {
        tbt_port_free(c->local_port);
        c->local_port = 0;
    }
    WRITE_ONCE(c->released, true);
}

/* Single-carrier release: quiesce, unpublish, one grace period, close. Only
 * for callers that cannot batch; the replacement and teardown paths use
 * tbt_carrier_batch_release() so one grace period covers the whole batch. */
static void tbt_carrier_release(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!c)
        return;
    tbt_carrier_quiesce(c);
    tbt_carrier_unpublish(sess, c);
    synchronize_rcu();
    tbt_carrier_close(c);
}

/* Move a carrier's cumulative counters into the session's retired totals,
 * once. A retired slot leaves sess->car[], so /proc/net/tbtun/peers stops
 * listing it; without this the session aggregate could no longer be
 * reconciled with the live rows. The caller serialises with the pump (retire
 * runs after the slot was swapped out), so the values are final here. */
void tbt_carrier_stats_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!sess || !c)
        return;
    if (test_and_set_bit(0, &c->stats_retired))
        return;
    atomic64_inc(&sess->carrier_retired);
    atomic64_add(READ_ONCE(c->tx_bytes), &sess->retired_tx_bytes);
    atomic64_add(READ_ONCE(c->data_frames), &sess->retired_data_frames);
    atomic64_add(READ_ONCE(c->retx_data), &sess->retired_retx_data);
}

/* Retire one carrier that is already off every container list and whose
 * installer reference is being handed over to this function. */
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c)
{
    if (!c)
        return;
    tbt_carrier_stats_retire(sess, c);
    tbt_carrier_release(sess, c);
    tbt_carrier_put(c);
}

/* Release a batch with one grace period for all of it. Every member is
 * already off sess->car/sess->prov; the list itself is caller-owned. */
static void tbt_carrier_batch_release(struct tbt_session *sess,
                                      struct list_head *batch)
{
    struct tbt_carrier *c, *tmp;

    list_for_each_entry(c, batch, node)
        tbt_carrier_quiesce(c);
    list_for_each_entry(c, batch, node)
        tbt_carrier_unpublish(sess, c);
    synchronize_rcu();
    list_for_each_entry_safe(c, tmp, batch, node)
    {
        list_del_init(&c->node);
        tbt_carrier_stats_retire(sess, c);
        tbt_carrier_close(c);
        tbt_carrier_put(c);
    }
}

/* Free a carrier that is on no session list and unreachable from every work
 * item. The nodes are unlinked first (a carrier that was never installed is
 * self-linked, which is also a valid no-op), so the caller cannot leave a
 * freed node on a list. The session reference taken at birth is returned
 * here; the object itself is freed by the last tbt_carrier_put(). */
void tbt_carrier_free(struct tbt_carrier *c)
{
    if (!c)
        return;
    list_del_init(&c->node);
    list_del_init(&c->release_node);
    kfree(c->http_tx);
    kfree(c->http_hdr);
    kfree(c->part);
    kfree(c->parser);
    kfree(c->http_rx);
    if (c->sess)
        tbt_session_put(c->sess);
    /* refcount-zero: last put's RCU callback, or a never-published discard */
    kfree(c);
}

/* Arm the session's ARQ/control pump. The sess->lock section is the
 * handshake with tbt_carrier_quiesce(): either this queues before stopping
 * is visible, or it sees stopping and refuses. Any context, including
 * softirq; never sleeps. */
bool tbt_carrier_arm_tx(struct tbt_carrier *c)
{
    struct tbt_session *sess;

    if (!c)
        return false;
    sess = c->sess;
    spin_lock(&sess->lock);
    if (atomic_read(&c->stopping) || READ_ONCE(c->released))
    {
        spin_unlock(&sess->lock);
        return false;
    }
    tbt_session_arm(sess, TBT_SW_TX, 0);
    spin_unlock(&sess->lock);
    return true;
}

/* Dial work: same stopping/released handshake as the other carrier arms. */
static bool tbt_carrier_arm_connect(struct tbt_carrier *c)
{
    struct tbt_session *sess;

    if (!c)
        return false;
    sess = c->sess;
    spin_lock(&sess->lock);
    if (atomic_read(&c->stopping) || READ_ONCE(c->released))
    {
        spin_unlock(&sess->lock);
        return false;
    }
    schedule_work(&c->w_connect);
    spin_unlock(&sess->lock);
    return true;
}

bool tbt_carrier_arm_pump(struct tbt_carrier *c, unsigned int ms)
{
    struct tbt_session *sess;

    if (!c)
        return false;
    sess = c->sess;
    spin_lock(&sess->lock);
    if (atomic_read(&c->stopping) || READ_ONCE(c->released))
    {
        spin_unlock(&sess->lock);
        return false;
    }
    mod_delayed_work(TBT_WQ, &c->w_pump, msecs_to_jiffies(ms));
    spin_unlock(&sess->lock);
    return true;
}

bool tbt_carrier_arm_rx(struct tbt_carrier *c)
{
    struct tbt_session *sess;

    if (!c)
        return false;
    sess = c->sess;
    spin_lock(&sess->lock);
    if (atomic_read(&c->stopping) || READ_ONCE(c->released))
    {
        spin_unlock(&sess->lock);
        return false;
    }
    tbt_car_schedule_rx(c);
    spin_unlock(&sess->lock);
    return true;
}

/* Turn a port-allocation failure into an operator-visible reason.  The
 * carrier stays alive and retries; nothing is killed.  The warning is
 * ratelimited because the retry loop runs every 200 ms. */
static void tbt_port_alloc_note(struct tbt_session *sess, u8 index,
                                enum tbt_port_alloc_err why)
{
    u16 lo, hi;
    u64 id = sess->tunnel ? sess->tunnel->cfg.id : 0;

    switch (why)
    {
    case TBT_PORT_ALLOC_EXHAUSTED:
        atomic64_inc(&sess->port_exhaust);
        /* Name the window that ran dry: the tunnel's own block when set,
         * else the global fallback. */
        if (sess->tunnel &&
            (sess->tunnel->cfg.portblock_lo || sess->tunnel->cfg.portblock_hi))
        {
            lo = sess->tunnel->cfg.portblock_lo;
            hi = sess->tunnel->cfg.portblock_hi;
        }
        else
        {
            bool configured;

            tbt_portmap_get(&lo, &hi, &configured);
        }
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u: source port block %u-%u is exhausted; carrier retries\n",
                            id, index, lo, hi);
        break;
    case TBT_PORT_ALLOC_NOMEM:
        atomic64_inc(&sess->port_alloc_fail);
        pr_warn_ratelimited("tbtun: tunnel %llu carrier %u: cannot allocate the source port bitmap; carrier retries\n",
                            id, index);
        break;
    default:
        break;
    }
}

/* Dials per second: n_car/4, floored. Every dial — first wave, replacement,
 * failed-connect retry — spends a token from this one bucket, so losing
 * every carrier cannot turn into a connect storm. */
#define TBT_DIAL_RATE_MIN 8
/* Tokens the bucket starts with: enough for a small session's whole first
 * wave, small enough that a 256-carrier session cannot burst. */
#define TBT_DIAL_FIRST_BURST 16

static u32 tbt_dial_rate(const struct tbt_session *sess)
{
    u32 rate = sess->n_car / 4;

    return rate < TBT_DIAL_RATE_MIN ? TBT_DIAL_RATE_MIN : rate;
}

/* The near side dials n carriers. The far side dials none: its carriers are
 * accepted and join the session by HELLO.
 *
 * The first wave is paced, not fired: every carrier is installed unconnected
 * and the 50 ms session poll drives the dials through the same token bucket
 * replacements and failed-connect retries use (tbt_dial_take). A 256-carrier
 * session therefore dials at max(8, n/4) per second instead of scheduling 256
 * connects at once — the unpaced first wave was the connect storm that took
 * the far listener down. The bucket is seeded with a small burst so a
 * two-carrier session is not delayed by the refill rate; the refill cap
 * still bounds the steady rate. */
int tbt_carriers_start(struct tbt_tunnel *t, struct tbt_session *sess)
{
    int i;
    u32 rate;

    if (t->cfg.role != TBT_ROLE_NEAR)
        return 0;
    rate = tbt_dial_rate(sess);
    TBT_DIAG("tunnel=%llu carriers_start enter n_car=%u n_pri=%u n=%u m=%u carriers=%u fec=%d crypto=%d profile=%u dial_rate=%u burst=%u",
             (unsigned long long)t->cfg.id, sess->n_car, sess->n_pri,
             t->cfg.n, t->cfg.m, t->cfg.carriers, sess->fec, sess->crypto,
             t->cfg.profile, rate, TBT_DIAL_FIRST_BURST);
    spin_lock(&sess->dial_lock);
    sess->dial_refill_ns = ktime_get_ns();
    sess->dial_tokens = rate < TBT_DIAL_FIRST_BURST ? rate
                                                  : TBT_DIAL_FIRST_BURST;
    spin_unlock(&sess->dial_lock);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_new(sess, (u8)i);

        if (!c)
        {
            atomic64_inc(&sess->carrier_alloc_fail);
            pr_warn_ratelimited("tbtun: tunnel %llu: carrier %d alloc failed: -ENOMEM\n",
                                (unsigned long long)t->cfg.id, i);
            return -ENOMEM;
        }
        rcu_assign_pointer(sess->car[i], c);
        TBT_DIAG("tunnel=%llu carrier=%d gen=%u spare=%d profile=%u state=%u installed",
                 (unsigned long long)t->cfg.id, i, c->gen, c->spare,
                 c->profile, c->state);
    }
    TBT_DIAG("tunnel=%llu carriers_start done n_car=%u",
             (unsigned long long)t->cfg.id, sess->n_car);
    return 0;
}

/* Replacement worker: for every dead slot, dial a fresh carrier at the same
 * index (near side), or clear the slot and wait for the peer's new carrier to
 * join by HELLO (far side). The swap is serialised with the pump, so no pump
 * can be using the old carrier when it is retired. The new port is allocated
 * while the old carrier still holds its own, so the replacement never reuses
 * the dead 4-tuple (a TIME_WAIT tuple makes connect fail with EADDRNOTAVAIL);
 * the old socket is released before the new connect starts. */
/* A pass retires at most this many carriers. Each retire cancels up to four
 * work items and releases a socket; retiring a whole flood of expired
 * provisionals in one item made that item run for tens of milliseconds and
 * starve the rx work that completes a legitimate HELLO (HND0: replace_work
 * hogged CPU 259 times in 15 s). The remainder is re-armed. */
#define TBT_REPLACE_BATCH 16
/* Deferred-pass delay: long enough for rx/tx work to run, short enough that a
 * legitimate replacement is not visibly late. */
#define TBT_REPLACE_RETRY_MS 20
/* Take one dial token. Shared by w_replace(), the poll's retry for a
 * carrier that never connected and the paced first wave in
 * tbt_carriers_start(), so the bucket is locked. */
static bool tbt_dial_take(struct tbt_session *sess)
{
    u64 now = ktime_get_ns();
    u32 rate = tbt_dial_rate(sess);
    u64 add;
    bool ok;

    spin_lock(&sess->dial_lock);
    if (!sess->dial_refill_ns)
    {
        sess->dial_refill_ns = now;
        sess->dial_tokens = rate;
    }
    add = (u64)rate * (now - sess->dial_refill_ns) / NSEC_PER_SEC;
    if (add)
    {
        u64 tokens = (u64)sess->dial_tokens + add;

        sess->dial_tokens = tokens > rate ? rate : (u32)tokens;
        sess->dial_refill_ns = now;
    }
    ok = sess->dial_tokens != 0;
    if (ok)
        sess->dial_tokens--;
    spin_unlock(&sess->dial_lock);
    return ok;
}

/* Bound the far side's provisional list. Each entry costs ~37 KiB of buffers
 * and a slot on a list the 50 ms poll walks; an unauthenticated peer can open
 * connections without ever sending HELLO, so the list must not grow without
 * limit. One full replacement wave plus slack is enough for a legitimate
 * session; anything beyond it is refused before a carrier is allocated. */
static u32 tbt_prov_cap(const struct tbt_session *sess)
{
    u32 slack = sess->n_car / 4;

    if (slack < 16)
        slack = 16;
    return (u32)sess->n_car + slack;
}

/* Free a carrier that was never installed: return its port first, because
 * tbt_carrier_free() only frees the object and its buffers. */
static void tbt_carrier_discard(struct tbt_carrier *c)
{
    if (!c)
        return;
    if (c->local_port)
    {
        tbt_port_free(c->local_port);
        c->local_port = 0;
    }
    tbt_carrier_free(c);
}

static void tbt_replace_body(struct tbt_session *sess)
{
    struct tbt_carrier **start;
    struct tbt_carrier *c, *tmp;
    LIST_HEAD(dead_now);
    LIST_HEAD(victims);
    LIST_HEAD(batch);
    u64 now = ktime_get_ns();
    bool near, more = false, retry_slow = false;
    int nstart = 0, retired = 0, nvictims = 0, i;

    if (!sess->tunnel || READ_ONCE(sess->closing))
        return;
    near = sess->tunnel->cfg.role == TBT_ROLE_NEAR;
    /* n_car pointers do not belong on the kernel stack. The allocation may
     * sleep; this is a work item, not softirq. If it fails, retry shortly:
     * the poll also re-arms this work while a stopping carrier remains. */
    start = kcalloc((size_t)sess->n_car, sizeof(*start), GFP_KERNEL);
    if (!start)
    {
        tbt_session_arm(sess, TBT_SW_REPLACE, msecs_to_jiffies(50));
        return;
    }
    mutex_lock(&sess->pump_lock);
    for (i = 0; i < sess->n_car && retired < TBT_REPLACE_BATCH; i++)
    {
        struct tbt_carrier *nc = NULL;
        bool stopping, drop = false;

        spin_lock(&sess->lock);
        c = sess->car[i];
        if (!c)
        {
            spin_unlock(&sess->lock);
            if (!near)
                continue;
            /* A near slot with no carrier is a dial that has not happened
             * yet: the first attempt failed, the port block was exhausted,
             * or a previous pass paced it. Retry it here, paced. */
            if (!tbt_dial_take(sess))
            {
                more = true;
                continue;
            }
            nc = tbt_carrier_new_locked(sess, (u8)i);
            if (nc)
            {
                enum tbt_port_alloc_err perr = TBT_PORT_ALLOC_OK;

                nc->local_port = tbt_port_alloc(sess->tunnel, &perr);
                if (!tbt_carrier_dial_ready(nc->local_port,
                                            perr != TBT_PORT_ALLOC_OK))
                {
                    tbt_port_alloc_note(sess, (u8)i, perr);
                    tbt_carrier_discard(nc);
                    nc = NULL;
                    retry_slow = true;
                }
            }
            if (!nc)
            {
                atomic64_inc(&sess->carrier_alloc_fail);
                continue;
            }
            spin_lock(&sess->lock);
            if (!sess->car[i])
            {
                rcu_assign_pointer(sess->car[i], nc);
                atomic64_inc(&sess->carrier_replaced);
                /* Keep the object alive across the unlock below; join() may
                 * replace this slot before the dial is scheduled. */
                refcount_inc(&nc->refs);
                start[nstart++] = nc;
                nc = NULL;
            }
            spin_unlock(&sess->lock);
            if (nc)
                tbt_carrier_discard(nc); /* join() won the slot */
            continue;
        }
        stopping = atomic_read(&c->stopping);
        if (!stopping)
        {
            spin_unlock(&sess->lock);
            continue;
        }
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
                rcu_assign_pointer(sess->car[i], NULL);
                list_add_tail(&c->node, &dead_now);
                retired++;
                c = NULL;
                break;
            }
        }
        spin_unlock(&sess->lock);
        if (!c)
            continue;
        if (near)
        {
            if (!tbt_dial_take(sess))
            {
                /* Leave the stopping carrier in place. Nothing pumps a
                 * stopping carrier, and the poll asks for another pass. */
                more = true;
                continue;
            }
            nc = tbt_carrier_new_locked(sess, (u8)i);
            if (nc)
            {
                enum tbt_port_alloc_err perr = TBT_PORT_ALLOC_OK;

                nc->local_port = tbt_port_alloc(sess->tunnel, &perr);
                if (!tbt_carrier_dial_ready(nc->local_port,
                                            perr != TBT_PORT_ALLOC_OK))
                {
                    tbt_port_alloc_note(sess, (u8)i, perr);
                    tbt_carrier_discard(nc);
                    nc = NULL;
                    retry_slow = true;
                }
            }
            if (!nc)
                atomic64_inc(&sess->carrier_alloc_fail);
            spin_lock(&sess->lock);
            if (sess->car[i] == c)
            {
                /* NULL: the next pass dials it. */
                rcu_assign_pointer(sess->car[i], nc);
                list_add_tail(&c->node, &dead_now);
                retired++;
                if (nc)
                {
                    atomic64_inc(&sess->carrier_replaced);
                    refcount_inc(&nc->refs);
                    start[nstart++] = nc;
                }
            }
            else
            {
                /* join() installed a carrier at this index while the new one
                 * was being allocated; keep it and drop ours. */
                drop = true;
            }
            spin_unlock(&sess->lock);
            if (drop)
                tbt_carrier_discard(nc);
        }
        else
        {
            spin_lock(&sess->lock);
            if (sess->car[i] == c)
            {
                /* The far side never dials; the near side's replacement
                 * joins. */
                rcu_assign_pointer(sess->car[i], NULL);
                list_add_tail(&c->node, &dead_now);
                retired++;
            }
            spin_unlock(&sess->lock);
        }
    }
    if (i < sess->n_car)
        more = true;
    spin_lock(&sess->lock);
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        if (c->joined)
        {
            /* join() installed it in car[]; it is not a provisional victim.
             * The new adopt order makes this unreachable. */
            WARN_ON_ONCE(1);
            list_del_init(&c->node);
            if (atomic_read(&sess->n_prov) > 0)
                atomic_dec(&sess->n_prov);
            continue;
        }
        if (atomic_read(&c->stopping) || now > c->join_deadline_ns)
        {
            /* Stopping is what tbt_session_join() checks under this same
             * lock: a HELLO racing with this move must be refused instead of
             * installing a carrier that is about to be freed. */
            atomic_set(&c->stopping, 1);
            list_move_tail(&c->node, &victims);
            atomic_dec(&sess->n_prov);
            if (++nvictims >= TBT_REPLACE_BATCH)
            {
                more = true;
                break;
            }
        }
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->pump_lock);

    /* Retired slots and provisional victims are already off every container
     * list. Carriers the join path replaced are queued on dead_release; move
     * them into the same batch so one grace period covers every object. */
    list_splice_tail_init(&dead_now, &batch);
    list_splice_tail_init(&victims, &batch);
    i = 0;
    while (i < TBT_REPLACE_BATCH)
    {
        struct tbt_carrier *r;

        spin_lock(&sess->lock);
        if (list_empty(&sess->dead_release))
        {
            spin_unlock(&sess->lock);
            break;
        }
        r = list_first_entry(&sess->dead_release, struct tbt_carrier,
                             release_node);
        list_del_init(&r->release_node);
        list_add_tail(&r->node, &batch);
        spin_unlock(&sess->lock);
        i++;
    }
    spin_lock(&sess->lock);
    more |= !list_empty(&sess->dead_release);
    spin_unlock(&sess->lock);
    tbt_carrier_batch_release(sess, &batch);
    /* Dial only after every old socket (and its port) is gone. */
    for (i = 0; i < nstart; i++)
    {
        tbt_carrier_arm_connect(start[i]);
        tbt_carrier_put(start[i]);
    }
    if (retired)
        tbt_tx_wake(sess);
    kfree(start);
    if (more && !READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_REPLACE,
                        msecs_to_jiffies(retry_slow ? 200
                                                    : TBT_REPLACE_RETRY_MS));
}

void tbt_replace_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_replace);
    bool owned = test_and_clear_bit(TBT_SW_REPLACE, &sess->sched);

    tbt_replace_body(sess);
    if (owned)
        tbt_session_put(sess);
}

void tbt_carriers_stop(struct tbt_session *sess)
{
    struct tbt_carrier *c, *tmp;
    LIST_HEAD(batch);
    int i;

    /* Stop the session poll first: it reads sess->car[] and sess->prov and can
     * ask for a replacement, so it must not run while the arrays are drained. */
    tbt_session_cancel(sess, TBT_SW_POLL);

    /* Swap every container under pump_lock, so no pump can still hold a
     * carrier after this section. The cancels and the socket release happen
     * outside it: the session pump/w_pump take pump_lock, and cancel_work_sync() sleeps. */
    mutex_lock(&sess->pump_lock);
    spin_lock(&sess->lock);
    for (i = 0; i < TBT_MAX_CARRIERS_LIMIT; i++)
    {
        if (sess->car[i])
        {
            c = sess->car[i];
            rcu_assign_pointer(sess->car[i], NULL);
            atomic_set(&c->stopping, 1);
            list_add_tail(&c->node, &batch);
        }
    }
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        if (c->joined)
        {
            WARN_ON_ONCE(1);
            list_del_init(&c->node);
            if (atomic_read(&sess->n_prov) > 0)
                atomic_dec(&sess->n_prov);
            continue;
        }
        list_del_init(&c->node);
        atomic_set(&c->stopping, 1);
        list_add_tail(&c->node, &batch);
    }
    atomic_set(&sess->n_prov, 0);
    list_for_each_entry_safe(c, tmp, &sess->dead_release, release_node)
    {
        list_del_init(&c->release_node);
        list_add_tail(&c->node, &batch);
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->pump_lock);

    tbt_carrier_batch_release(sess, &batch);
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
    sk = tbt_carrier_sk_get(c);
    if (!sk)
        return true;
    {
        bool dead = READ_ONCE(sk->sk_state) != TCP_ESTABLISHED;

        tbt_carrier_sk_put(sk);
        return dead;
    }
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
/* TCP keepalive on every carrier: 30 s idle, 10 s between probes, 3 probes.
 * A half-open path (peer gone, no FIN, no RST) otherwise leaves snd_una frozen
 * and packets_out unchanged until the poll's stall_dead bound expires; the
 * probes make the kernel close the socket, which the poll sees as a dead
 * carrier. */
#define TBT_KEEPIDLE_S 30
#define TBT_KEEPINTVL_S 10
#define TBT_KEEPCNT 3

/* Turn keepalive on and set its cadence. A failure is recorded, never fatal:
 * a carrier without keepalive still works, it just falls back to the poll's
 * bound. Called after sock_create_kern() and before connect (dial), and right
 * after an accepted socket is wired (adopt). */
static void tbt_carrier_keepalive(struct socket *s)
{
    struct sock *sk;
    int err;

    if (!s || !s->sk)
        return;
    sk = s->sk;
    sock_set_keepalive(sk);
    err = tcp_sock_set_keepidle(sk, TBT_KEEPIDLE_S);
    if (!err)
        err = tcp_sock_set_keepintvl(sk, TBT_KEEPINTVL_S);
    if (!err)
        err = tcp_sock_set_keepcnt(sk, TBT_KEEPCNT);
    if (err)
        pr_info_ratelimited("tbtun: carrier keepalive setup failed: %d\n", err);
}

void tbt_car_setsockbuf(struct socket *s, const struct tbt_cfg *cfg, u16 n_pri)
{
    u32 win = cfg->tx_window_bytes;
    u32 n = n_pri ? n_pri : 1;
    u32 share, budget, per;
    int v;

    if (!win)
        win = cfg->rx_reorder_bytes;
    /* Two bounds, both divided by the carrier count:
     *   share  = this carrier's slice of the reorder/window budget + slack;
     *   budget = the module's total carrier-buffer budget (64 MiB), so
     *            128 carriers cannot turn an 8 MiB-per-carrier setting into
     *            a 1 GiB allocation.
     * The kernel doubles SO_*BUF, so pass half of the byte target. */
    share = win / n + 2 * TBT_CHUNK_MAX;
    budget = TBT_CAR_SOCKBUF_TOTAL / n;
    per = max(share, budget);
    v = clamp_t(u32, per / 2, 64 * 1024, 1024 * 1024);

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

/* Dial `addr` until the handshake finishes or TBT_CONNECT_BUDGET_MS is spent.
 *
 * kernel_connect() on a blocking socket does not report a timeout as
 * -ETIMEDOUT: __inet_stream_connect() arms `err = -EINPROGRESS` before it
 * waits, and the expiry path returns that value unchanged (net/ipv4/af_inet.c,
 * `if (!timeo || !inet_wait_for_connect(...)) goto out;`). The socket is still
 * connecting at that moment -- the SYN may be on its second transmission -- so
 * treating the return as fatal released the socket (RST) and dialled a new
 * 5-tuple, and the peer's accepted socket was left waiting for a HELLO that
 * could never arrive. Measured on the WAN smoke: connect_fail=41 with
 * -EINPROGRESS, one carrier replacing itself every second while the far
 * logged `awaiting HELLO`. A second call re-enters SS_CONNECTING and waits
 * another window; -EALREADY is the same "still in progress" answer. */
static int tbt_connect_bounded(struct tbt_session *sess, struct socket *s,
                               const struct sockaddr_in *addr)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(TBT_CONNECT_BUDGET_MS);

    for (;;)
    {
        int err = kernel_connect(s, (tbt_sockaddr *)addr,
                                 sizeof(*addr), 0);

        if (!err || err == -EISCONN)
            return 0;
        if (err != -EINPROGRESS && err != -EALREADY)
            return err;
        if (time_after_eq(jiffies, deadline))
            return -ETIMEDOUT;
        atomic64_inc(&sess->connect_retry);
    }
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
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u enter stopping=%d sock=%d lport=%u retry_at=%lld",
             (unsigned long long)t->cfg.id, c->index, c->gen,
             atomic_read(&c->stopping), tbt_carrier_sk_present(c), c->local_port,
             (long long)(c->retry_at_ns - ktime_get_ns()) / NSEC_PER_MSEC);
    if (atomic_read(&c->stopping) || tbt_carrier_sk_present(c))
    {
        TBT_DIAG("tunnel=%llu carrier=%u gen=%u skip stopping=%d sock=%d",
                 (unsigned long long)t->cfg.id, c->index, c->gen,
                 atomic_read(&c->stopping), tbt_carrier_sk_present(c));
        return;
    }

    if (!c->local_port)
    {
        enum tbt_port_alloc_err perr = TBT_PORT_ALLOC_OK;

        c->local_port = tbt_port_alloc(t, &perr);
        if (!tbt_carrier_dial_ready(c->local_port,
                                    perr != TBT_PORT_ALLOC_OK))
        {
            /* A port must never leak when the reason is not OK but the
             * allocator still handed one out. */
            if (c->local_port)
            {
                tbt_port_free(c->local_port);
                c->local_port = 0;
            }
            atomic64_inc(&sess->carrier_alloc_fail);
            tbt_port_alloc_note(sess, c->index, perr);
            c->retry_at_ns = ktime_get_ns() + 200 * NSEC_PER_MSEC;
            return;
        }
    }
    local.sin_port = htons(c->local_port);

    err = sock_create_kern(sess->net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        goto fail;
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u sock_create ok",
             (unsigned long long)t->cfg.id, c->index, c->gen);
    /* Keep the dialling socket on the configured interface. A policy route
     * (TUN/proxy) can otherwise capture the connect and answer it locally,
     * leaving the peer with no SYN to accept. */
    if (t->cfg.dev_set)
    {
        err = sock_setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
                              KERNEL_SOCKPTR(t->cfg.dev),
                              strlen(t->cfg.dev) + 1);
        if (err)
        {
            TBT_DIAG("tunnel=%llu carrier=%u gen=%u SO_BINDTODEVICE dev=%s ret=%d",
                     (unsigned long long)t->cfg.id, c->index, c->gen,
                     t->cfg.dev, err);
            goto release;
        }
    }
    /* The replacement carrier often reuses the port the dead one just freed,
     * which may still be in TIME_WAIT. */
    sock_set_reuseaddr(s->sk);
    err = kernel_bind(s, (tbt_sockaddr *)&local, sizeof(local));
    if (err)
        goto release;
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u bind ok local=%pI4:%u peer=%pI4:%u",
             (unsigned long long)t->cfg.id, c->index, c->gen,
             &local.sin_addr, ntohs(local.sin_port), &addr.sin_addr,
             ntohs(addr.sin_port));
    /* Before the SYN: the handshake freezes the window scale from this
     * socket's receive buffer. */
    tbt_car_setsockbuf(s, &t->cfg, sess->n_pri);
    tbt_carrier_keepalive(s);
    s->sk->sk_sndtimeo = msecs_to_jiffies(TBT_CONNECT_TIMEOUT_MS);
    {
        u64 t0 = ktime_get_ns();
        u64 elapsed;

        err = tbt_connect_bounded(sess, s, &addr);
        elapsed = ktime_get_ns() - t0;
        TBT_DIAG("tunnel=%llu carrier=%u gen=%u connect ret=%d elapsed_ms=%llu sk_state=%u",
                 (unsigned long long)t->cfg.id, c->index, c->gen, err,
                 elapsed / NSEC_PER_MSEC,
                 READ_ONCE(s->sk->sk_state));
        /* The economic stall rule charges a replacement its handshake: keep
         * the observed cost of successful dials. */
        if (!err)
        {
            atomic64_add(elapsed, &sess->connect_ns_sum);
            atomic64_inc(&sess->connect_ns_count);
        }
    }
    if (err)
        goto release;

    tcp_sk(s->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(s->sk);
    c->sock = s;
    rcu_assign_pointer(c->sk, s->sk);

    write_lock_bh(&s->sk->sk_callback_lock);
    s->sk->sk_user_data = c;
    s->sk->sk_data_ready = tbt_car_ready;
    s->sk->sk_write_space = tbt_car_write_space;
    s->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&s->sk->sk_callback_lock);
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u callbacks installed lport=%u",
             (unsigned long long)t->cfg.id, c->index, c->gen,
             ntohs(inet_sk(s->sk)->inet_sport));


    atomic_set(&c->connected, 1);
    c->state = TBT_CAR_CONNECTING; /* up after the pump writes HELLO */
    c->last_una = tcp_sk(s->sk)->snd_una;
    c->last_una_ns = ktime_get_ns();
    c->last_una_move_ns = ktime_get_ns();
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u connected=1 state=%u snd_una=%llu tx_ready=%d key_id=%u",
             (unsigned long long)t->cfg.id, c->index, c->gen, c->state,
             (unsigned long long)c->last_una, READ_ONCE(sess->crypto_tx_ready),
             sess->crypto_key_id);
    /* The peer may have written its HELLO before our sk_data_ready was
     * installed (the callback goes in after kernel_connect returns, and the
     * stock callback already fired for the queued data). Schedule the read
     * work unconditionally, exactly as the accept side does, or the HELLO sits
     * in the receive queue and the carrier never comes up (measured: probe I,
     * the near dialled 73 carriers, every far accept waited for HELLO). */
    tbt_carrier_arm_rx(c);
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u rx scheduled",
             (unsigned long long)t->cfg.id, c->index, c->gen);
    /* One line per dial: 256 of them at a 256-carrier start, so it follows the
     * same debug>=1 gate as the rest of the bring-up trace. */
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u dialed lport=%u peer=%pI4:%u",
             (unsigned long long)t->cfg.id, c->index, c->gen,
             ntohs(inet_sk(s->sk)->inet_sport), &t->cfg.peer_addr,
             ntohs(t->cfg.peer_port));
    {
        int herr = tbt_tx_hello(sess, c->index, GFP_KERNEL);

        TBT_DIAG("tunnel=%llu carrier=%u gen=%u hello enqueued ret=%d ctl_queued=%u tx_queued=%u",
                 (unsigned long long)t->cfg.id, c->index, c->gen, herr,
                 READ_ONCE(sess->ctl_queued), READ_ONCE(sess->tx_queued));
    }
    tbt_tx_wake(sess);
    TBT_DIAG("tunnel=%llu carrier=%u gen=%u connect work done",
             (unsigned long long)t->cfg.id, c->index, c->gen);
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
    /* The poll retries every 200 ms per slot: at 256 carriers an unratelimited
     * line here is ~1280 console writes per second, which by itself pins a
     * vCPU and floods the journal. */
    pr_warn_ratelimited("tbtun: tunnel %llu carrier %u failed to connect: %d\n",
                        t->cfg.id, c->index, err);
}

/* HELLO: the first record on every carrier, in both directions. The body
 * carries the wire version, the sender's role, the session nonce, the two
 * payload ceilings and the last byte `profile | (gen << 2)`: profile in bits
 * 1:0, carrier generation in bits 5:2, bits 7:6 zero (DESIGN.md §3.3). The
 * header's path_id is the carrier index. */
int tbt_tx_hello(struct tbt_session *sess, u8 index, gfp_t gfp)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    struct tbt_tunnel *t = sess->tunnel;
    u16 blen = sess->fec ? TBT_HELLO_BODY_LEN_V6 : TBT_HELLO_BODY_LEN;
    struct tbt_carrier *hc = tbt_carrier_get(sess, index);
    int rc;

    TBT_DIAG("HELLO build tunnel=%llu carrier=%u gen=%u fec=%d profile=%u blen=%u key_id=%u have_key=%d tx_ready=%d",
             (unsigned long long)t->cfg.id, index, hc ? hc->gen : 0, sess->fec,
             hc ? hc->profile : t->cfg.profile, blen, sess->crypto_key_id,
             sess->crypto_have_key, READ_ONCE(sess->crypto_tx_ready));

    memset(body, 0, sizeof(body));
    body[0] = sess->fec ? TBT_WIRE_VERSION_V6 : TBT_WIRE_VERSION;
    body[1] = t->cfg.role;
    memcpy(body + 2, sess->nonce, sizeof(sess->nonce));
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);
    body[22] = (hc ? hc->profile : t->cfg.profile) |
               (u8)(((hc ? hc->gen : 0) & 0xf) << 2);
    if (sess->fec)
        tbt_crypto_hello_fill(body, sess->crypto, sess->crypto_key_id,
                              sess->crypto_tx_nonce,
                              sess->crypto_key_id == TBT_CRYPTO_KEY_ID_HELLO
                                  ? sess->crypto_key
                                  : NULL);
    TBT_DIAG("HELLO body filled tunnel=%llu carrier=%u gen=%u profile_gen_byte=%u",
             (unsigned long long)t->cfg.id, index, hc ? hc->gen : 0, body[22]);
    rc = tbt_tx_control_pinned(sess, NULL, index, TBT_F_HELLO, 0, 0, body,
                               blen, false, gfp);
    /* body may carry the session key in clear; do not leave it on the stack. */
    tbt_memzero_explicit(body, sizeof(body));
    tbt_carrier_put(hc);
    return rc;
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
        c = rcu_dereference_protected(sess->car[i],
                                      lockdep_is_held(&sess->lock));
        if (c && tbt_carrier_sk_present(c) && !atomic_read(&c->stopping))
        {
            tbt_car_schedule_rx(c);
        }
    }
    list_for_each_entry(c, &sess->prov, node)
    {
        if (tbt_carrier_sk_present(c) && !atomic_read(&c->stopping))
            tbt_car_schedule_rx(c);
    }
    spin_unlock(&sess->lock);
}

/* Diagnostic snapshot for /proc/net/tbtun/stats: whether the callbacks the
 * data path depends on are still the ones this module installed, and why the
 * first carrier is (not) being read. */
void tbt_carrier_snapshot(struct tbt_session *sess)
{
    struct tbt_carrier *c = NULL;
    struct sock *sk;
    int i;

    for (i = 0; i < sess->n_car && !c; i++)
        c = tbt_carrier_get(sess, (u8)i);
    if (!c)
        return;
    sk = tbt_carrier_sk_get(c);
    if (!sk)
    {
        tbt_carrier_put(c);
        return;
    }
    sess->snap_car_state = READ_ONCE(sk->sk_state);
    sess->snap_car_stopping = atomic_read(&c->stopping);
    sess->snap_car_ud = READ_ONCE(sk->sk_user_data) != NULL;
    sess->snap_car_ready_ok = READ_ONCE(sk->sk_data_ready) == tbt_car_ready;
    sess->snap_car_ws_ok = READ_ONCE(sk->sk_write_space) == tbt_car_write_space;
    sess->snap_txw_pending = work_pending(&sess->w_tx.work);
    sess->snap_txw_busy = work_busy(&sess->w_tx.work);
    sess->snap_car_wmem = READ_ONCE(sk->sk_wmem_queued);
    sess->snap_car_rcv_nxt = READ_ONCE(tcp_sk(sk)->rcv_nxt);
    sess->snap_car_copied = READ_ONCE(tcp_sk(sk)->copied_seq);
    sess->snap_car_rcvbuf = READ_ONCE(sk->sk_rcvbuf);
    sess->snap_car_sndbuf = READ_ONCE(sk->sk_sndbuf);
    tbt_carrier_sk_put(sk);
    tbt_carrier_put(c);
}

/* The four buffers are pointers, not inline arrays. If someone turns one of
 * them into an array, kzalloc() still zeroes the whole struct and the buffer
 * storage, and the "birth is all zero" invariant silently changes meaning.
 * Reject that at compile time, and pin the buffer sizes to the largest record
 * each one must hold. */
static void tbt_carrier_birth_checks(void)
{
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->part), u8 *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->parser), struct tbt_parser *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_rx), struct tbt_http_rx *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_tx), char *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_hdr), char *));
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_HTTP_FEC_FRAME_MAX);
    BUILD_BUG_ON(TBT_BATCH_REC_MAX != TBT_FEC_BATCH_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
    /* The FEC-session encoder buffer holds every record type that can still
     * reach a carrier once DATA bypasses the ARQ queue. If a new control
     * frame grows past TBT_FBK_MAX this refuses to build instead of letting
     * tbt_http_frame_encode() truncate it at run time. */
    BUILD_BUG_ON(TBT_HTTP_CTL_FRAME_MAX <
                 TBT_HTTP_CHUNK_HDR_MAX + TBT_HDR_LEN + TBT_FBK_MAX + 2);
    BUILD_BUG_ON(TBT_HTTP_CTL_FRAME_MAX <
                 TBT_HTTP_CHUNK_HDR_MAX + TBT_HDR_LEN +
                     TBT_WINDOW_BODY_LEN + 2);
    BUILD_BUG_ON(TBT_HTTP_CTL_FRAME_MAX <
                 TBT_HTTP_CHUNK_HDR_MAX + TBT_HDR_LEN +
                     TBT_HELLO_BODY_LEN_V6 + 2);
    BUILD_BUG_ON(TBT_HTTP_CTL_FRAME_MAX >= TBT_HTTP_FRAME_MAX);
}

// Called once at load, from tbt_core.c, before any carrier exists.
int tbt_carrier_init(void)
{
    /* Every per-carrier field must be at least as wide as the carrier limit.
     * A 16-slot array or a single `unsigned long` here takes a write at index
     * 16..255 past its own end and corrupts the struct around it (measured
     * twice: `announced_gen[c->index]` overwrote tx_paused, and
     * `path_lost_mask` overwrote the session's control scheduler). */
    BUILD_BUG_ON(ARRAY_SIZE(((struct tbt_stream *)0)->announced_gen) <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_stream *)0)->announced_mask) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_stream *)0)->tx_fin_mask) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_stream *)0)->tx_fin_inflight) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_stream *)0)->tx_rst_mask) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_stream *)0)->tx_rst_inflight) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(sizeof(((struct tbt_session *)0)->path_lost_mask) * 8 <
                 TBT_MAX_CARRIERS_LIMIT);
    /* The debug injector indexes this array with the carrier action's u16
     * index after the tunnel's n_car bound check; it must cover the limit. */
    BUILD_BUG_ON(ARRAY_SIZE(((struct tbt_session *)0)->fec_drop_ppm) <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(ARRAY_SIZE(((struct tbt_stream *)0)->sched.order) <
                 TBT_MAX_CARRIERS_LIMIT);
    BUILD_BUG_ON(TBT_STREAM_F_SCHED_TARGET >= BITS_PER_LONG);
    tbt_carrier_birth_checks();
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
    /* Refuse before allocating ~37 KiB of carrier buffers. A peer that opens
     * connections and never sends HELLO must not grow the provisional list
     * without bound; see tbt_prov_cap(). */
    if (atomic_read(&sess->n_prov) >= (int)tbt_prov_cap(sess))
    {
        sess->prov_reject++;
        return -ENOSPC;
    }
    c = tbt_carrier_new(sess, 0xff);
    if (!c)
        return -ENOMEM;
    c->provisional = true;
    tcp_sk(newsock->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(newsock->sk);
    c->sock = newsock;

    /* Publish the object on prov before any callback can find it. A softirq
     * that arrives in the window below still runs the listener's inherited
     * tbt_ready() and never touches c. */
    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing) ||
        atomic_read(&sess->n_prov) >= (int)tbt_prov_cap(sess))
    {
        spin_unlock(&sess->lock);
        sess->prov_reject++;
        /* Nothing has seen c->sk yet. Restore the inherited callbacks and
         * drop the carrier object, but leave the socket to the accept loop:
         * it closes every socket for which adopt returned an error. */
        tbt_carrier_unpublish(sess, c);
        tbt_carrier_free(c);
        return -ENOSPC;
    }
    {
        int n = atomic_inc_return(&sess->n_prov);

        if ((u32)n > sess->prov_peak)
            sess->prov_peak = (u32)n;
    }
    list_add_tail(&c->node, &sess->prov);
    spin_unlock(&sess->lock);

    /* Only now may a softirq find c through sk_user_data. */
    rcu_assign_pointer(c->sk, newsock->sk);

    write_lock_bh(&newsock->sk->sk_callback_lock);
    newsock->sk->sk_user_data = c;
    newsock->sk->sk_data_ready = tbt_car_ready;
    newsock->sk->sk_write_space = tbt_car_write_space;
    newsock->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&newsock->sk->sk_callback_lock);

    /* The listener already fixed the window scale; this sizes the accepted
     * socket's buffers to the same share. */
    tbt_car_setsockbuf(newsock, &sess->tunnel->cfg, sess->n_pri);
    tbt_carrier_keepalive(newsock);
    atomic_set(&c->connected, 1);
    sess->rx_adopt_events++;
    /* Assert the wiring we just installed. A carrier that is connected but never
     * given our callbacks/proto looks exactly like a silent peer. */
    WARN_ON_ONCE(newsock->sk->sk_data_ready != tbt_car_ready);
    WARN_ON_ONCE(newsock->sk->sk_prot == &tcp_prot);
    tbt_carrier_arm_rx(c);
    /* One line per connection is a console flood under a connect storm (HND0
     * logged 330 accepts/s). Keep the event visible but bounded. */
    pr_info_ratelimited("tbtun: tunnel %llu carrier ACCEPTED lport=%u rport=%u, awaiting HELLO\n",
                        t->cfg.id, ntohs(inet_sk(newsock->sk)->inet_sport),
                        ntohs(inet_sk(newsock->sk)->inet_dport));
    return 0;
}

static void tbt_target_body(struct tbt_stream *st)
{
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
        return;
    err = sock_create_kern(t->net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        goto fail;
    /* Same bound as a carrier dial: session teardown cancels w_target, and
     * the default connect timeout is the full SYN retry schedule. */
    s->sk->sk_sndtimeo = msecs_to_jiffies(TBT_CONNECT_TIMEOUT_MS);
    err = tbt_connect_bounded(sess, s, &addr);
    if (err)
    {
        sock_release(s);
        goto fail;
    }
    tcp_sk(s->sk)->nonagle = TCP_NAGLE_OFF;
    err = tbt_target_attach(st, s);
    if (err)
    {
        /* Attach publishes st->sk and st->target under sess->lock. A failed
         * attach owns no stream reference: close the new socket here, or a
         * concurrent stream kill sees a target with no sk and leaks it. */
        tbt_sock_abort_on_close(s);
        sock_release(s);
        if (err == -EALREADY)
            return;
        goto fail;
    }
    tbt_tx_pump(sess);
    return;
fail:
    pr_debug("tbtun: tunnel %llu target connect failed err=%d\n",
             (unsigned long long)t->cfg.id, err);
    WRITE_ONCE(st->target_started, false);
}

void tbt_target_work_fn(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_target);
    bool owned = test_and_clear_bit(TBT_STREAM_F_SCHED_TARGET,
                                    &st->attached);

    tbt_target_body(st);
    if (owned)
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
        if (!tbt_stream_schedule(st, &st->w_target,
                                 TBT_STREAM_F_SCHED_TARGET))
        {
            /* The stream is already leaving; let a later OPEN retry if it
             * still can. */
            spin_lock(&sess->lock);
            if (!st->target && st->target_started)
                st->target_started = false;
            spin_unlock(&sess->lock);
        }
    }
    return 0;
}
