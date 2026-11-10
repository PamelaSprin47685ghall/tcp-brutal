/* tbt_sock.c - the stream's output socket, in both directions.
 *
 * THE MODEL (DESIGN.md §8.5, corrected by measurement)
 *   The module owns the socket an application talks to. That socket is the
 *   passive end of a TCP connection whose other end is the application's own
 *   client socket, so the application's writes never reach this socket's
 *   sendmsg: they arrive here as ordinary TCP input. Capture is therefore
 *   receive-side, and the same mechanism serves both ends of the tunnel:
 *
 *     app -> tunnel (near) : tcp_read_sock() drains the accepted socket's
 *                            receive queue into tbt_tx. The app's send() only
 *                            ever ran on its own client socket.
 *     target -> tunnel (far): tcp_read_sock() drains the target connection's
 *                            receive queue into tbt_tx, identically.
 *
 *     tunnel -> app (near) : bytes are written to the accepted socket with
 *                            tcp_sendmsg_locked(), so they travel over the TCP
 *                            connection to the application's client socket,
 *                            whose read() returns them as ordinary data.
 *     tunnel -> target (far): bytes are written to the target connection,
 *                            identically.
 *
 *   WHY NO RECEIVE-QUEUE INJECTION
 *   Two measurements closed that door. docs/recvmsg-probe.md showed the
 *   application's descriptor is its *client* socket, a different object from
 *   the accepted socket, so ops replacement on the accepted socket serves the
 *   module, not the app. docs/inject-probe.md then showed that appending to
 *   the accepted socket's receive queue does reach the app, but is unsafe on a
 *   duplex connection: bytes the peer sends while we inject land behind an
 *   rcv_nxt we advanced and are dropped as TCP_OLD_DATA. Transmitting on the
 *   accepted socket has neither problem: capture reads the receive queue,
 *   delivery writes the send queue, and stock TCP owns window, retransmission
 *   and backpressure.
 *
 * C3 adds reordering ahead of delivery: the RX side (tbt_rx.c) fills rx_q only
 * with the contiguous run, and this file drains rx_q in order. A short
 * tcp_sendmsg_locked() keeps the frame's offset and is retried when the peer's
 * window opens.
 *
 * Must never:
 *   - run a blocking call from softirq; the callbacks only schedule work or
 *     wake a waiter.
 *   - drop a frame on a short write; the frame keeps its offset and stays
 *     queued.
 *   - report EOF before every byte that precedes the FIN has been delivered.
 */
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_map.h"

/* One bit per stream work item; bit 1 means an instance is queued or running
 * and owns exactly one stream reference (review/s2-merged-design.md §1.4).
 * The entry point clears the bit first and puts the reference last, so a wake
 * that arrives while the work runs takes a second reference and is served by
 * a second run.
 *
 * The reference is taken before the bit is published, so a canceller that
 * sees the bit always has a reference it may return. cancel_work_sync()
 * disables a work item while it runs, and a queue that races the cancel is
 * dropped silently; the work_pending() check returns that reference here
 * instead of leaving it to an instance that never exists. */
bool tbt_stream_schedule(struct tbt_stream *st, struct work_struct *w,
                         unsigned long bit)
{
    if (!st)
        return false;
    if (test_bit(bit, &st->attached))
        return true;
    if (!refcount_inc_not_zero(&st->refs))
        return false;
    if (test_and_set_bit(bit, &st->attached))
    {
        /* Another schedule owns the bit and its reference. */
        tbt_stream_put(st);
        return true;
    }
    queue_work(TBT_WQ, w);
    /* queue_work() returns %false both when the work was already pending (a
     * queued instance owns a reference and will clear the bit) and when a
     * concurrent cancel disabled the work and the queue was dropped. The
     * entry clears the bit before it runs, so checking the bit after the
     * queue separates the two: a dropped queue leaves the bit set with no
     * pending instance. */
    if (!work_pending(w) && test_and_clear_bit(bit, &st->attached))
    {
        tbt_stream_put(st);
        return false;
    }
    return true;
}

/* Cancel a queued instance and return its reference. Only a pending instance
 * is owned by the canceller; a running one returns its own reference at the
 * end of its entry wrapper, and cancel_work_sync() waits for that. */
bool tbt_stream_cancel(struct tbt_stream *st, struct work_struct *w,
                       unsigned long bit)
{
    bool canceled;

    if (!st)
        return false;
    canceled = cancel_work_sync(w);
    if (canceled && test_and_clear_bit(bit, &st->attached))
        tbt_stream_put(st);
    return canceled;
}

// ---------------------------------------------------------------------------
// Capture: bytes the peer delivered into sk_receive_queue -> the tunnel.
//
// On the public socket the peer is the application's own client, so everything
// it sends lands here as normal TCP input. tcp_read_sock() walks from
// tp->copied_seq and consumes whole skbs; delivery uses the send queue, so the
// two directions never touch the same queue or cursor.
static int tbt_pub_actor(read_descriptor_t *desc, struct sk_buff *skb,
                         unsigned int offset, size_t len)
{
    struct tbt_stream *st = desc->arg.data;
    size_t taken;

    if (!st)
        return 0;
    atomic64_inc(&st->sess->pub_actor_calls);
    if (!len)
        atomic64_inc(&st->sess->dbg_empty_len);
    taken = tbt_tx_submit(st, skb, offset, len);
    desc->count += taken;
    return taken;
}

static void tbt_pub_capture_body(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);
    read_descriptor_t desc = {};
    bool fin = false;

    might_sleep();
    if (!sk)
        return;
    /* A paused capture is not a reason to skip this wake. The pump clears
     * tx_paused and schedules this work, but that resume can be missed: the
     * ring may never fall to half while the application's window is shut, so
     * no new submit arms the pump, and the pump is not otherwise woken. The
     * 1 GiB field run froze for 200 s while the application was down to one
     * 2 KB zero-window probe per RTO and no resume was in sight. Reading
     * while paused is bounded: the actor returns 0 once
     * tbt_fec_tx_submit() cannot take a byte, and tcp_read_sock() stops on
     * that return, so the attempt costs one socket lock and no copy. */
    read_lock_bh(&sk->sk_callback_lock);
    if (sk->sk_user_data != st)
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    read_unlock_bh(&sk->sk_callback_lock);

    desc.arg.data = st;
    atomic64_inc(&st->sess->pub_capture_runs);
    lock_sock(sk);
    st->sess->pub_last_rcv_nxt = tcp_sk(sk)->rcv_nxt;
    st->sess->pub_last_copied = tcp_sk(sk)->copied_seq;
    st->sess->pub_last_qlen = sk->sk_receive_queue.qlen;
    tcp_read_sock(sk, &desc, tbt_pub_actor);
    st->sess->pub_last_after = tcp_sk(sk)->copied_seq;
    if (!READ_ONCE(sk->sk_err) &&
        tbt_stream_peer_eof(
            (READ_ONCE(sk->sk_shutdown) & RCV_SHUTDOWN) != 0,
            READ_ONCE(sk->sk_state) == TCP_CLOSE,
            READ_ONCE(st->state) == TBT_ST_DEAD) &&
        tcp_sk(sk)->copied_seq == tcp_sk(sk)->rcv_nxt)
        fin = true;
    release_sock(sk);

    /* FIN is stream-ordered: emit it only after every byte before the peer's
     * FIN has been submitted, so its seq is the final tx_seq.  w_ctrl only
     * pokes this work item; doing it here also covers a FIN that arrived
     * before tbt_sock_attach() installed the state-change callback. */
    if (fin && READ_ONCE(st->state) != TBT_ST_DEAD &&
        !READ_ONCE(st->tx_fin_sent))
    {
        int queued;

        /* Field diagnosis: an early FIN delivered 313 MB instead of 1 GiB
         * with no reset counter. Name the exact evidence here so the next run
         * can tell a real target EOF from a misread socket state. */
        pr_info_ratelimited("tbtun: pub FIN sid=%llu shutdown=%u state=%u err=%d copied=%u rcv_nxt=%u st=%u paused=%d rx_seen=%d rx_done=%d rx_next=%llu rx_fin_seq=%llu tx_seq=%llu\n",
                            (unsigned long long)st->sid,
                            READ_ONCE(sk->sk_shutdown),
                            READ_ONCE(sk->sk_state), READ_ONCE(sk->sk_err),
                            tcp_sk(sk)->copied_seq, tcp_sk(sk)->rcv_nxt,
                            READ_ONCE(st->state),
                            READ_ONCE(st->tx_paused) ? 1 : 0,
                            READ_ONCE(st->rx_fin_seen) ? 1 : 0,
                            READ_ONCE(st->rx_fin_done) ? 1 : 0,
                            (unsigned long long)READ_ONCE(st->rx_next),
                            (unsigned long long)READ_ONCE(st->rx_fin_seq),
                            (unsigned long long)READ_ONCE(st->tx_seq));
        WRITE_ONCE(st->tx_fin_sent, true);
        queued = tbt_tx_fin(st->sess, st, GFP_KERNEL, "pub");
        if (queued > 0)
        {
            WRITE_ONCE(st->tx_fin_queued, true);
            /* Never detach on the queued copy: the pump still has to write it
             * after tx_ack covers tx_seq.  maybe_detach() waits for both. */
            tbt_stream_maybe_detach(st);
        }
        /* queued == 0: no carrier was usable.  tx_fin_sent stays set and the
         * FBK tick retries; never detach without a FIN in the queue. */
    }
}

static void tbt_pub_capture_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_read);
    bool owned = test_and_clear_bit(TBT_STREAM_F_SCHED_READ,
                                    &st->attached);

    tbt_pub_capture_body(st);
    if (owned)
        tbt_stream_put(st);
}

// ---------------------------------------------------------------------------
// Delivery: tunnel -> app, by ordinary transmit

// softirq context. Stock TCP's data-ready callback is sock_def_readable, which
// is not exported; this is its equivalent for a socket whose reader is an
// ordinary process on the other end of the connection: wake the waiters.
static void tbt_pub_ready(struct sock *sk)
{
    read_lock_bh(&sk->sk_callback_lock);
    if (sk->sk_user_data)
    {
        struct tbt_stream *st = sk->sk_user_data;

        tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    }
    read_unlock_bh(&sk->sk_callback_lock);
    // These bytes are ours to take, not the app's to read. The app is woken by
    // its own socket when our transmit reaches it.
}

// Peer's window opened: retry our own delivery queue if anything is pending.
static void tbt_pub_write_space(struct sock *sk)
{
    read_lock_bh(&sk->sk_callback_lock);
    if (sk->sk_user_data)
    {
        struct tbt_stream *st = sk->sk_user_data;

        tbt_stream_schedule(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
    }
    read_unlock_bh(&sk->sk_callback_lock);
}

static void tbt_pub_state_change(struct sock *sk)
{
    read_lock_bh(&sk->sk_callback_lock);
    if (sk->sk_user_data)
    {
        struct tbt_stream *st = sk->sk_user_data;

        pr_info_ratelimited("tbtun: pub state_change sid=%llu shutdown=%u state=%u err=%d copied=%u rcv_nxt=%u st=%u\n",
                            (unsigned long long)st->sid,
                            READ_ONCE(sk->sk_shutdown), READ_ONCE(sk->sk_state),
                            READ_ONCE(sk->sk_err), tcp_sk(sk)->copied_seq,
                            tcp_sk(sk)->rcv_nxt, READ_ONCE(st->state));
        tbt_stream_schedule(st, &st->w_ctrl, TBT_STREAM_F_SCHED_CTRL);
    }
    read_unlock_bh(&sk->sk_callback_lock);
}

// Deliver one run. Returns the number of bytes accepted (>= 0) or a negative
// errno. The caller keeps the frame until the whole run is consumed.
//
// Both ends use the ordinary transmit path on the socket the stream owns: the
// accepted public socket on the near side, the target connection on the far
// side. tcp_sendmsg_locked() takes the socket lock, sleeps on the socket's own
// send memory when the peer's window is closed, and gives us stock TCP window,
// retransmission and backpressure. A short count is still honoured so the
// caller can keep the remainder.
int tbt_sock_deliver(struct tbt_stream *st, const u8 *data, size_t len)
{
    struct msghdr msg = {};
    struct kvec iov = { .iov_base = (void *)data, .iov_len = len };
    struct sock *sk = READ_ONCE(st->sk);
    int err;

    might_sleep();
    if (!sk)
        return -ENOTCONN;
    if (!len)
        return 0;
    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, len);
    /* Never sleep here. This work item is cancelled by the reset path
     * (tbt_kill_work -> tbt_sock_detach), and a blocking send waits for the
     * peer's window with no bound; the kernel then reports the kworker blocked
     * for minutes. -EAGAIN leaves the frame queued, and the output socket's
     * sk_write_space callback schedules this work again. */
    msg.msg_flags = MSG_DONTWAIT;
    lock_sock(sk);
    err = tcp_sendmsg_locked(sk, &msg, len);
    release_sock(sk);
    return err;
}

void tbt_sock_kick(struct tbt_stream *st)
{
    if (st)
        tbt_stream_schedule(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
}

// Peer sent FIN: after the queue drains, half-close the write side so the app
// (or target) sees a clean EOF while the other direction keeps flowing.
void tbt_sock_shutdown_wr(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);
    struct socket *sock;

    if (!sk || !(sock = sk->sk_socket))
        return;
    pr_info_ratelimited("tbtun: shutdown_wr sid=%llu shutdown=%u state=%u err=%d rx_seen=%d rx_done=%d rx_next=%llu rx_fin_seq=%llu st=%u\n",
                        (unsigned long long)st->sid,
                        READ_ONCE(sk->sk_shutdown), READ_ONCE(sk->sk_state),
                        READ_ONCE(sk->sk_err),
                        READ_ONCE(st->rx_fin_seen) ? 1 : 0,
                        READ_ONCE(st->rx_fin_done) ? 1 : 0,
                        (unsigned long long)READ_ONCE(st->rx_next),
                        (unsigned long long)READ_ONCE(st->rx_fin_seq),
                        READ_ONCE(st->state));
    kernel_sock_shutdown(sock, SHUT_WR);
}

static void tbt_drain_body(struct tbt_stream *st)
{
    struct tbt_frame *fr;
    bool eof = false;

    might_sleep();

    for (;;)
    {
        size_t left;
        bool complete = false;
        int done;

        spin_lock_bh(&st->rx_lock);
        fr = list_first_entry_or_null(&st->rx_q, struct tbt_frame, list);
        if (fr)
            left = fr->len - fr->off;
        spin_unlock_bh(&st->rx_lock);
        if (!fr)
            break;

        /* Delivery must be contiguous and must not hold rx_lock while taking
         * the socket lock, so the frame stays on the queue until fully used.
         * A prefix accepted by tcp_sendmsg_locked() advances fr->off; the rest
         * is retried when the output socket's window opens (sk_write_space
         * schedules this work again). */
        done = tbt_sock_deliver(st, fr->data + fr->off, left);
        if (done == -EPIPE || done == -ECONNRESET || done == -ESHUTDOWN)
        {
            /* The target is gone (EPIPE/ECONNRESET) with frames still queued:
             * they can never be delivered. Abort the stream now so the peer
             * resets its application, instead of holding the backlog until
             * its own stall budget (measured: the far held 2.65 MB with
             * tgt_state=7 while the near kept sending). */
            tbt_stream_kill(st, TBT_ST_DEAD);
            break;
        }
        if (done <= 0)
            break;

        spin_lock_bh(&st->rx_lock);
        fr->off += done;
        st->rx_queued -= done;
        st->rx_delivered += done;
        atomic64_add(done, &st->sess->rx_delivered_bytes);
        if (fr->off == fr->len)
        {
            list_del(&fr->list);
            complete = true;
        }
        spin_unlock_bh(&st->rx_lock);
        if (complete)
            kfree(fr);
        if (!complete)
            break;

        spin_lock_bh(&st->rx_lock);
        if (st->paused && st->rx_queued < TBT_RX_READY_LOW)
        {
            WRITE_ONCE(st->paused, false);
            spin_unlock_bh(&st->rx_lock);
            tbt_carrier_resume(st->sess);
            continue;
        }
        spin_unlock_bh(&st->rx_lock);
    }

    /* EOF is stream-ordered. The peer's FIN carries the byte offset that
     * follows the last DATA byte, so it takes effect only when the ready queue
     * is empty and every earlier byte has arrived (rx_next reached it). */
    spin_lock_bh(&st->rx_lock);
    if (st->rx_fin_seen && !st->rx_fin_done && list_empty(&st->rx_q) &&
        st->rx_next >= st->rx_fin_seq)
    {
        pr_info_ratelimited("tbtun: peer FIN sid=%llu seq=%llu rx_next=%llu state=%u tx_fin_written=%d\n",
                            (unsigned long long)st->sid,
                            (unsigned long long)st->rx_fin_seq,
                            (unsigned long long)st->rx_next,
                            READ_ONCE(st->state),
                            READ_ONCE(st->tx_fin_written) ? 1 : 0);
        st->rx_fin_done = true;
        st->rx_fin_done_ns = ktime_get_ns();
        eof = true;
    }
    /* If the last delivered frame took the queue below the low-water mark, the
     * in-loop check may not have run (partial frame, or the loop exited right
     * after the last frame). Resume the carriers here too; a pause that is
     * never cleared is indistinguishable from a dead carrier. */
    if (st->paused && st->rx_queued < TBT_RX_READY_LOW)
    {
        WRITE_ONCE(st->paused, false);
        spin_unlock_bh(&st->rx_lock);
        tbt_carrier_resume(st->sess);
    }
    else
    {
        spin_unlock_bh(&st->rx_lock);
    }
    if (eof)
    {
        tbt_sock_shutdown_wr(st);
        /* rx_fin_done is half of the detach gate; the FBK tick owns the
         * other half (tx_fin_written) and the close bound. Wake it even when
         * the session had no stream while it stopped. */
        tbt_fbk_kick(st->sess, false);
        /* The peer's FIN is delivered; this is one half of the close.  Leave
         * the session only after our own FIN is written as well, so its order
         * gate is not released while DATA is still unacknowledged.  detach()
         * only takes sess->lock and stamps the tombstone, so it is safe
         * inside w_drain. */
        tbt_stream_maybe_detach(st);
    }
}

static void tbt_drain_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_drain);
    bool owned = test_and_clear_bit(TBT_STREAM_F_SCHED_DRAIN,
                                    &st->attached);

    tbt_drain_body(st);
    if (owned)
        tbt_stream_put(st);
}

/* RST is the last control frame the stream will ever send. A full control
 * queue or a transient allocation failure must not turn it into a silent
 * loss: retry after the pump drains, bounded, then count the loss. */
static void tbt_tx_rst_retry(struct tbt_session *sess, struct tbt_stream *st)
{
    int i, rc = 0;

    for (i = 0; i <= TBT_CTL_MAX_RETRIES; i++)
    {
        rc = tbt_tx_rst(sess, st, GFP_KERNEL);
        if (rc != -ENOSPC && rc != -ENOMEM)
            return;
        msleep(TBT_CTL_RETRY_MS);
    }
    atomic64_inc(&sess->ctl_lost);
}

static void tbt_ctrl_body(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    might_sleep();
    if (!sk)
        return;
    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        return;
    if (READ_ONCE(sk->sk_err))
    {
        pr_info_ratelimited("tbtun: ctrl err sid=%llu err=%d state=%u rx_seen=%d rx_next=%llu rx_fin_seq=%llu\n",
                            (unsigned long long)st->sid, READ_ONCE(sk->sk_err),
                            READ_ONCE(sk->sk_state),
                            READ_ONCE(st->rx_fin_seen) ? 1 : 0,
                            (unsigned long long)READ_ONCE(st->rx_next),
                            (unsigned long long)READ_ONCE(st->rx_fin_seq));
        tbt_tx_rst_retry(st->sess, st);
        tbt_stream_kill(st, TBT_ST_DEAD);
        return;
    }
    if (tbt_stream_peer_eof(
            (READ_ONCE(sk->sk_shutdown) & RCV_SHUTDOWN) != 0,
            READ_ONCE(sk->sk_state) == TCP_CLOSE, false))
    {
        /* The peer's FIN is graceful, whatever the TCP state name is: it may
         * already be CLOSE after FIN_WAIT2 -> tcp_time_wait -> tcp_done.  The
         * capture work item emits the FIN once the preceding bytes are in. */
        pr_info_ratelimited("tbtun: ctrl peer_eof sid=%llu shutdown=%u state=%u err=%d rx_queued=%llu copied=%u rcv_nxt=%u\n",
                            (unsigned long long)st->sid,
                            READ_ONCE(sk->sk_shutdown), READ_ONCE(sk->sk_state),
                            READ_ONCE(sk->sk_err),
                            (unsigned long long)READ_ONCE(st->rx_queued),
                            tcp_sk(sk)->copied_seq, tcp_sk(sk)->rcv_nxt);
        tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
        /* A target that closed with our delivery queue non-empty cannot take
         * those bytes any more; let w_drain discover the write error and
         * abort the stream instead of leaving the queue parked. */
        if (READ_ONCE(st->rx_queued))
            tbt_stream_schedule(st, &st->w_drain,
                                TBT_STREAM_F_SCHED_DRAIN);
        return;
    }
    /* TCP_CLOSE without sk_err is a graceful or local close, not a reset.
     * A local abort is owned by tbt_stream_kill(); never turn it into a
     * tunnel RST here. */
}

static void tbt_ctrl_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_ctrl);
    bool owned = test_and_clear_bit(TBT_STREAM_F_SCHED_CTRL,
                                    &st->attached);

    tbt_ctrl_body(st);
    if (owned)
        tbt_stream_put(st);
}

// ---------------------------------------------------------------------------
// Attach / detach

/* Work items are initialised when the stream object is born, not when a socket
 * is attached: tbt_rx may queue a frame (and tbt_rx_flush may schedule w_drain)
 * before the far side's target connection exists. INIT_WORK on an already
 * queued work item is a corruption, so this is the only initialiser. */
static void tbt_kill_work(struct work_struct *w);
static void tbt_stream_release_socket_now(struct tbt_stream *st);

void tbt_stream_init_work(struct tbt_stream *st)
{
    INIT_WORK(&st->w_read, tbt_pub_capture_work);
    INIT_WORK(&st->w_drain, tbt_drain_work);
    INIT_WORK(&st->w_ctrl, tbt_ctrl_work);
    INIT_WORK(&st->w_kill, tbt_kill_work);
    INIT_WORK(&st->w_target, tbt_target_work_fn);
}

/* Wake every waiter on a dying socket without sending a FIN. SHUT_RD alone
 * clears the receive half and wakes a blocked recvmsg, but a sender waits on
 * SEND_SHUTDOWN or sk_err, so raise ECONNRESET as well. A SHUT_RDWR here sent
 * a real FIN first: the 1 GiB far-host loss ended as curl rc=18 (clean EOF)
 * instead of a reset, and the application could no longer tell a truncated
 * stream from a complete one. */
static void tbt_sock_kill_wake(struct sock *sk)
{
    struct socket *sock = sk->sk_socket;

    if (!sock)
        return;
    kernel_sock_shutdown(sock, SHUT_RD);
    lock_sock(sk);
    if (!READ_ONCE(sk->sk_err))
        WRITE_ONCE(sk->sk_err, ECONNRESET);
    release_sock(sk);
    if (sk->sk_error_report)
        sk->sk_error_report(sk);
}

/* A killed stream must reach the application as a reset, not a half-close: a
 * FIN leaves the application free to keep sending into a socket whose reader
 * is gone, and a blocked send() then hangs until its own timeout (measured:
 * probe H took 41 s instead of 0.5 s). Detach first (clears callbacks and
 * cancels the other work items), then wake without FIN and close with
 * SO_LINGER=0, which is the exported way to make tcp_close send RST. */
static void tbt_kill_body(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct sock *sk;

    might_sleep();
    /* Wake anything asleep on the socket first: a delivery blocked in
     * tcp_sendmsg_locked() would otherwise keep w_drain running and the
     * release would wait forever in cancel_work_sync(). This runs in the kill
     * work item, never at the call site: kill() can be called from a carrier
     * RX actor that already holds another socket's lock, and taking this
     * socket's lock there is a recursive k-sk_lock-AF_INET nesting. */
    sk = READ_ONCE(st->sk);
    if (sk)
        tbt_sock_kill_wake(sk);
    tbt_tx_rst_retry(sess, st);
    tbt_stream_cancel(st, &st->w_target, TBT_STREAM_F_SCHED_TARGET);

    /* Wake anything asleep on the stream socket: a delivery blocked in
     * tcp_sendmsg_locked()/lock_sock() would otherwise keep w_drain running
     * and release_socket_now() would wait forever in cancel_work_sync().
     * This must not run in tbt_stream_kill(): the RST path calls it from the
     * carrier receive work under lock_sock(carrier), and shutdown takes
     * lock_sock(stream) -- a nested sk_lock-AF_INET lockdep report. w_target
     * is cancelled above, so st->sk cannot change before release closes it. */
    sk = READ_ONCE(st->sk);
    if (sk)
        tbt_sock_kill_wake(sk);

    /* No self cancel: this is the w_kill work item. */
    tbt_stream_release_socket_now(st);

    if (!READ_ONCE(sess->closing))
    {
        tbt_session_arm(sess, TBT_SW_REAP,
                        msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
        if (sess->tunnel && sess->tunnel->cfg.role == TBT_ROLE_NEAR)
            tbt_session_arm(sess, TBT_SW_PENDING, 0);
    }
}

static void tbt_kill_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_kill);
    bool owned = test_and_clear_bit(TBT_STREAM_F_SCHED_KILL,
                                    &st->attached);

    tbt_kill_body(st);
    if (owned)
        tbt_stream_put(st);
}

/* The parent stream carries the application's bytes: the near accepted
 * socket and the far target socket both use it. The kernel doubles
 * SO_*BUF, so 32 MiB here is a 64 MiB buffer per direction: the 400 ms x
 * 128 MB/s BDP needs 64 MiB of source in flight. FORCE ignores
 * rmem_max/wmem_max; when the caller lacks the capability the capped
 * option is the fallback, never a failed attach. */
void tbt_parent_setsockbuf(struct socket *s)
{
    int v = 32 * 1024 * 1024;

    if (!s)
        return;
    if (sock_setsockopt(s, SOL_SOCKET, SO_SNDBUFFORCE, KERNEL_SOCKPTR(&v),
                        sizeof(v)))
        sock_setsockopt(s, SOL_SOCKET, SO_SNDBUF, KERNEL_SOCKPTR(&v),
                        sizeof(v));
    if (sock_setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, KERNEL_SOCKPTR(&v),
                        sizeof(v)))
        sock_setsockopt(s, SOL_SOCKET, SO_RCVBUF, KERNEL_SOCKPTR(&v),
                        sizeof(v));
}

void tbt_sock_attach(struct tbt_stream *st)
{
    struct sock *sk = st->sk;

    if (!sk)
        return;
    /* Both the target work item and an OPEN can call this; only the first may
     * install the callbacks. */
    if (test_and_set_bit(TBT_STREAM_F_ATTACHED, &st->attached))
        return;

    /* The output socket is the parent stream's window into the tunnel; the
     * shared setter gives it the 16 MiB buffer the listener reserved. */
    if (sk->sk_socket)
        tbt_parent_setsockbuf(sk->sk_socket);
    write_lock_bh(&sk->sk_callback_lock);
    sk->sk_user_data = st;
    sk->sk_data_ready = tbt_pub_ready;
    sk->sk_write_space = tbt_pub_write_space;
    sk->sk_state_change = tbt_pub_state_change;
    write_unlock_bh(&sk->sk_callback_lock);

    /* A FIN/data or a queued frame may predate this socket. */
    tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    tbt_stream_schedule(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
}

void tbt_sock_detach(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (!sk)
        return;
    if (!test_and_clear_bit(TBT_STREAM_F_ATTACHED, &st->attached))
        return;
    /* Restore the core callbacks.  Writing our own functions back would leave
     * module text on a socket that can outlive the module (F2). */
    tbt_sk_ops_restore(sk, &st->sess->def_ops);

    /* Order matters: ctrl can requeue read/drain; read can emit FIN; drain
     * can queue w_kill (already cancelled by the caller). */
    tbt_stream_cancel(st, &st->w_ctrl, TBT_STREAM_F_SCHED_CTRL);
    tbt_stream_cancel(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    tbt_stream_cancel(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
}

/* Save the core callbacks before this module replaces them.  Call this only
 * on a fresh sock_create_kern() socket.  An accepted socket already inherited
 * tbt_ready() from its listener. */
void tbt_sk_ops_save(struct tbt_sk_ops *ops, struct sock *sk)
{
    ops->data_ready = READ_ONCE(sk->sk_data_ready);
    ops->write_space = READ_ONCE(sk->sk_write_space);
    ops->state_change = READ_ONCE(sk->sk_state_change);
    ops->error_report = READ_ONCE(sk->sk_error_report);
}

/* Never install a NULL callback: a NULL sk_data_ready is a worse crash than
 * a stale one.  All four are non-NULL on a sock_init_data() socket. */
void tbt_sk_ops_restore(struct sock *sk, const struct tbt_sk_ops *ops)
{
    write_lock_bh(&sk->sk_callback_lock);
    sk->sk_user_data = NULL;
    if (ops->data_ready)
        WRITE_ONCE(sk->sk_data_ready, ops->data_ready);
    if (ops->write_space)
        WRITE_ONCE(sk->sk_write_space, ops->write_space);
    if (ops->state_change)
        WRITE_ONCE(sk->sk_state_change, ops->state_change);
    if (ops->error_report)
        WRITE_ONCE(sk->sk_error_report, ops->error_report);
    write_unlock_bh(&sk->sk_callback_lock);
}

/* Close with RST.  __tcp_close() then reaches TCP_CLOSE and unhashes the
 * socket before sock_release() returns.  A plain shutdown + release leaves
 * FIN_WAIT1/2 for up to 60 s, still hashed and still able to receive data. */
void tbt_sock_abort_on_close(struct socket *sock)
{
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };

    if (!sock || !sock->sk)
        return;
    sock_setsockopt(sock, SOL_SOCKET, SO_LINGER, KERNEL_SOCKPTR(&lg),
                    sizeof(lg));
}

/* Release the one socket this stream owns, without cancelling w_kill.  Called
 * from w_kill itself, so cancel_work_sync(&st->w_kill) would self-deadlock. */
static void tbt_stream_release_socket_now(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct socket *target, *sock, *other = NULL;
    struct sock *sk;
    bool kick = false;

    /* A target work that published st->target but not st->sk (or a kill that
     * raced that publish) must still close the target. Adopt the target's sk
     * for detach/restore, then clear both under the session lock. */
    spin_lock(&sess->lock);
    target = READ_ONCE(st->target);
    if (!READ_ONCE(st->sk) && target && target->sk)
        WRITE_ONCE(st->sk, target->sk);
    sk = READ_ONCE(st->sk);
    if (target)
    {
        st->target = NULL;
        st->target_started = false;
        kick = true;
    }
    spin_unlock(&sess->lock);

    if (!sk && !target)
        return;

    /* Detach/restore/cancel while st->sk is still visible to the work items. */
    tbt_sock_detach(st);
    if (target && target->sk && (!sk || target->sk != sk))
        tbt_sk_ops_restore(target->sk, &sess->def_ops);

    spin_lock(&sess->lock);
    WRITE_ONCE(st->sk, NULL);
    spin_unlock(&sess->lock);

    sock = target ? target : (sk && sk->sk_socket ? sk->sk_socket : NULL);
    if (sk && sk->sk_socket && sk->sk_socket != sock)
        other = sk->sk_socket;

    if (other)
    {
        tbt_sock_abort_on_close(other);
        sock_release(other);
    }
    if (sock)
    {
        tbt_sock_abort_on_close(sock);
        sock_release(sock);
    }

    /* The old target is unpublished; a new far-side stream may be waiting. */
    (void)kick;
}

/* Public entry for session destroy / error paths.  It waits for w_kill first;
 * w_kill itself must use the _now variant. */
void tbt_stream_release_socket(struct tbt_stream *st)
{
    tbt_stream_cancel(st, &st->w_kill, TBT_STREAM_F_SCHED_KILL);
    tbt_stream_release_socket_now(st);
}

// ---------------------------------------------------------------------------
// Near side: a freshly accepted public connection

struct tbt_pending
{
    struct list_head node;
    struct socket *sock;
    u8 slot; /* near mapping slot this connection arrived on */
};

/* The near allocator's view of a sid: any object still in the hash owns the
 * id, including a tombstone the reaper has not collected yet. */
static bool tbt_sid_in_use_locked(struct tbt_session *sess, u64 sid)
{
    struct tbt_stream *st;

    hlist_for_each_entry(st, &sess->streams[TBT_STREAM_BUCKET(sid)], node)
        if (st->sid == sid)
            return true;
    return false;
}

/* A new stream may arrive while the previous flow is still live only because
 * its stream has not detached yet. If that old stream has already seen the
 * peer FIN (or is a tombstone), stop it now so its flow is released and the
 * sequential stream can admit; a genuinely concurrent live stream is left
 * alone and the new one is refused. */
static bool tbt_fec_retire_closing(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;
    struct tbt_stream *st;
    u64 sid;
    bool old = false;

    /* SC-LDPC shares one plane, so there is no group to reclaim and no
     * reason to kill a live stream when another connection arrives. */
    if (!sess || sess->fec_sclc)
        return false;
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    sid = f ? f->sid : 0;
    spin_unlock(&sess->lock);
    if (!sid)
        return false;
    st = tbt_stream_get(sess, sid, false, false);
    if (!st)
        return false;
    old = READ_ONCE(st->rx_fin_seen) ||
          READ_ONCE(st->state) == TBT_ST_DEAD ||
          READ_ONCE(st->tombstone_until_ns);
    if (old)
    {
        if (READ_ONCE(st->state) != TBT_ST_DEAD)
            tbt_stream_kill(st, TBT_ST_DEAD);
        /* kill() detaches, but a racing DYING winner may have returned early;
         * detach is idempotent and guarantees the flow is gone before retry. */
        tbt_fec_flow_detach(st);
    }
    tbt_stream_put(st);
    return old;
}

static int tbt_stream_start_now(struct tbt_session *sess, struct socket *sock,
                                u8 slot)
{
    struct tbt_stream *st;
    struct tbt_fec_flow *flow = NULL;
    u64 sid = 0;
    int tries;

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing) ||
        sess->n_active >= TBT_STREAMS_MAX)
    {
        spin_unlock(&sess->lock);
        return -EBUSY;
    }
    /* Never hand out a sid that is still live or tombstoned. Reusing a
     * tombstone would let the far side drop the new OPEN while its own
     * tombstone is armed, and a late frame of the old stream could enter the
     * new one. The 24-bit counter leaves ample room; n_streams bounds the
     * skip on the rare 24-bit wrap. */
    for (tries = 0; tries <= sess->n_streams + 1; tries++)
    {
        u64 cand = ((u64)(sess->sid_prefix & 0xffff) << 24) |
                   (sess->next_sid & 0xffffff);

        sess->next_sid++;
        /* sid 0 is the "no stream" sentinel in sess->far_sid. */
        if (!cand)
            continue;
        if (!tbt_sid_in_use_locked(sess, cand))
        {
            sid = cand;
            break;
        }
    }
    if (!sid)
    {
        spin_unlock(&sess->lock);
        pr_warn_ratelimited("tbtun: tunnel %llu sid allocator exhausted at %u hash entries\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            sess->n_streams);
        return -EAGAIN;
    }
    spin_unlock(&sess->lock);

    /* Admit the FEC flow before the stream exists: its group bitmap travels
     * in the OPEN, and the packetizer must never see a stream without one.
     * The planes are allocated with GFP_KERNEL, so this runs in the pending
     * work item, not in the accept callback. */
    if (sess->fec)
    {
        int err;

        flow = tbt_fec_flow_admit(sess, sid,
                                   (u16)tbt_fec_groups_per_flow);
        if (IS_ERR(flow) && PTR_ERR(flow) == -ENOSPC &&
            tbt_fec_retire_closing(sess))
            flow = tbt_fec_flow_admit(sess, sid,
                                       (u16)tbt_fec_groups_per_flow);
        if (IS_ERR(flow))
        {
            err = PTR_ERR(flow);
            /* No group is free, or the shape cannot carry a second flow:
             * queueing the socket would not help. The pending path resets
             * the connection, and the counter says why. */
            if (err == -ENOSPC)
                atomic64_inc(&sess->fec_stream_refused);
            return err == -ENOSPC ? -EOPNOTSUPP : -EINVAL;
        }
    }

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing) || sess->n_active >= TBT_STREAMS_MAX)
    {
        spin_unlock(&sess->lock);
        /* Abort consumes the admission reference; a bare put here would
         * leave the unlinked flow's planes alive forever. */
        if (flow)
            tbt_fec_flow_abort(sess, flow);
        return -EBUSY;
    }
    sess->far_sid = sid;
    spin_unlock(&sess->lock);

    st = tbt_stream_get(sess, sid, true, true);
    if (!st)
    {
        if (flow)
            tbt_fec_flow_abort(sess, flow);
        return -ENOMEM;
    }
    if (flow && tbt_fec_flow_attach(st, flow)) /* moves the reference */
    {
        /* The shared mux leg could not be built. The stream never reached
         * OPEN, so release it before the pending path resets the socket. */
        tbt_stream_kill(st, TBT_ST_DEAD);
        tbt_stream_put(st);
        return -ENOMEM;
    }
    st->sk = sock->sk;
    /* Slot 0 with target= unset sends 0: the far side's default target, the
     * exact 0.4.0 behavior. Every other slot names its configured port. */
    st->target_port = tbt_map_target_for_slot(&sess->tunnel->cfg, slot);
    st->route_slot = slot;
    WRITE_ONCE(st->state, TBT_ST_OPEN);

    /* OPEN goes to every carrier that has sent HELLO; the pump writes it
     * before any DATA because it is inserted at the head of the queue. */
    tbt_tx_announce(sess, st, GFP_KERNEL);
    tbt_sock_attach(st);
    /* Data may already be queued from before the accept completed; take it now
     * rather than waiting for the next packet to arrive. */
    tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    tbt_fbk_kick(sess, false);
    tbt_stream_put(st);
    return 0;
}

static void tbt_pending_body(struct tbt_session *sess)
{

    for (;;)
    {
        struct tbt_pending *p;
        struct socket *sock;
        int err;

        spin_lock(&sess->lock);
        if (READ_ONCE(sess->closing) || list_empty(&sess->pending))
        {
            spin_unlock(&sess->lock);
            return;
        }
        p = list_first_entry(&sess->pending, struct tbt_pending, node);
        list_del(&p->node);
        sess->n_pending--;
        spin_unlock(&sess->lock);

        sock = p->sock;

        if (!sock->sk || READ_ONCE(sock->sk->sk_state) == TCP_CLOSE)
        {
            kfree(p);
            if (sock->sk)
                tbt_sk_ops_restore(sock->sk, &sess->def_ops);
            tbt_sock_abort_on_close(sock);
            sock_release(sock);
            continue;
        }

        err = tbt_stream_start_now(sess, sock, p->slot);
        if (err == -EOPNOTSUPP)
        {
            /* The FEC flow table has no group left for this stream. Reset the
             * connection now: queueing it cannot help until another flow
             * detaches, and a two-connection protocol (iperf3's control
             * connection plus its data connection) must see the refusal as an
             * error instead of hanging. */
            kfree(p);
            if (sock->sk)
                tbt_sk_ops_restore(sock->sk, &sess->def_ops);
            tbt_sock_abort_on_close(sock);
            sock_release(sock);
            continue;
        }
        if (err == -EBUSY)
        {
            /* The session is full or closing; keep the accepted socket
             * queued instead of refusing it. */
            spin_lock(&sess->lock);
            list_add(&p->node, &sess->pending);
            sess->n_pending++;
            spin_unlock(&sess->lock);
            tbt_session_arm(sess, TBT_SW_PENDING, msecs_to_jiffies(1));
            return;
        }
        kfree(p);
        if (err)
        {
            if (sock->sk)
                tbt_sk_ops_restore(sock->sk, &sess->def_ops);
            tbt_sock_abort_on_close(sock);
            sock_release(sock);
            continue;
        }
        /* More accepted sockets may be queued behind this one. */
    }
}

void tbt_pending_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_pending);
    bool owned = test_and_clear_bit(TBT_SW_PENDING, &sess->sched);

    tbt_pending_body(sess);
    if (owned)
        tbt_session_put(sess);
}

void tbt_pending_drain(struct tbt_session *sess)
{
    for (;;)
    {
        struct tbt_pending *p;
        struct socket *sock;

        spin_lock(&sess->lock);
        if (list_empty(&sess->pending))
        {
            spin_unlock(&sess->lock);
            return;
        }
        p = list_first_entry(&sess->pending, struct tbt_pending, node);
        list_del(&p->node);
        sess->n_pending--;
        spin_unlock(&sess->lock);

        sock = p->sock;
        kfree(p);
        if (sock->sk)
            tbt_sk_ops_restore(sock->sk, &sess->def_ops);
        tbt_sock_abort_on_close(sock);
        sock_release(sock);
    }
}

int tbt_stream_start(struct tbt_session *sess, struct socket *sock, u8 slot)
{
    struct tbt_pending *p;

    p = kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p)
        return -ENOMEM;
    p->sock = sock;
    p->slot = slot;

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing))
    {
        spin_unlock(&sess->lock);
        kfree(p);
        return -ENOTCONN;
    }
    if (sess->n_pending >= TBT_PENDING_MAX)
    {
        spin_unlock(&sess->lock);
        kfree(p);
        return -EBUSY;
    }
    list_add_tail(&p->node, &sess->pending);
    sess->n_pending++;
    spin_unlock(&sess->lock);

    tbt_session_arm(sess, TBT_SW_PENDING, 0);
    return 0; /* ownership transferred; accept loop must not release */
}

// ---------------------------------------------------------------------------
// Far side: the stream bound to the target connection.

void tbt_far_open_seen(struct tbt_stream *st, u8 route_slot, u16 target_port)
{
    struct tbt_session *sess = st->sess;

    struct socket *tg;

    /* A later OPEN can arrive while this publisher is between
     * tbt_stream_get_created() and this call. In v7 every live flow is routed
     * by sid and far_sid is only the last/primary compatibility pointer, so a
     * newer OPEN must not kill this stream: both flows are valid. Only a
     * stream another path already marked DEAD is refused. */
    spin_lock(&sess->lock);
    if (READ_ONCE(st->state) == TBT_ST_DEAD)
    {
        spin_unlock(&sess->lock);
        return;
    }
    /* tbt_session_bind_sid() already resolved the wire's slot and port against
     * this far tunnel's config, so both values are final here. */
    st->route_slot = route_slot;
    st->target_port = target_port;
    WRITE_ONCE(st->state, TBT_ST_OPEN);
    spin_unlock(&sess->lock);

    tbt_tx_announce(sess, st, GFP_ATOMIC);
    tg = READ_ONCE(st->target);
    if (tg)
        tbt_target_attach(st, tg);
    else
        tbt_carrier_open_target(st);
    /* A new far stream must restart the FBK tick: it is the only driver of
     * tbt_tx_fin() retries and tbt_tx_close_check(). The tick stops when the
     * session had no streams, so without this a new stream inherits a dead
     * timer until an unrelated carrier loss happens to kick it. */
    tbt_fbk_kick(sess, false);
}

int tbt_target_attach(struct tbt_stream *st, struct socket *s)
{
    struct tbt_session *sess = st->sess;
    bool attach = false;
    int err = 0;

    /* Publish sk and target together. tbt_stream_release_socket_now() reads
     * both under the same lock; a window with target set and sk NULL let the
     * killer return early and leave the old target in FIN_WAIT/CLOSE_WAIT. */
    spin_lock(&sess->lock);
    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        err = -ECANCELED;
    else if (st->sk && st->sk != s->sk)
        err = -EALREADY;
    else
    {
        st->sk = s->sk;
        WRITE_ONCE(st->target, s);
        attach = true;
    }
    spin_unlock(&sess->lock);
    if (attach)
        tbt_sock_attach(st);
    return err;
}

void tbt_target_detach(struct tbt_stream *st)
{
    tbt_sock_detach(st);
    WRITE_ONCE(st->target, NULL);
    WRITE_ONCE(st->target_started, false);
}

// ---------------------------------------------------------------------------
// Public socket sk_prot.
//
// MEASURED CORRECTION (DESIGN.md §8.5): the accepted socket is the *passive*
// end of the application's connection. Everything the application writes
// arrives as ordinary TCP input and lands in sk_receive_queue; sk_prot->sendmsg
// on this socket is never called during a transfer. So capture is receive-side
// (tcp_read_sock above), and no sendmsg override is needed or wanted.
