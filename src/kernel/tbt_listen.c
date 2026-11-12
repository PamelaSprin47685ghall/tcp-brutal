/* tbt_listen.c - the kernel-side listeners for a tunnel.
 *
 * Owns:
 *   - creating, binding and listening on one socket per mapping slot through
 *     sock_create_kern/kernel_bind/kernel_listen (DESIGN.md §2.1, §8.5).
 *     A near tunnel listens on its public port (slot 0) plus every map= slot;
 *     a far tunnel listens only on its carrier port (slot 0).
 *   - the accept loop. One work item per listener runs from sk_data_ready,
 *     because accept may sleep and sk_data_ready runs in softirq.
 *   - closing accepted sockets when the hand-off refuses them.
 *
 * Must never:
 *   - call lock_sock or any sleeping function from sk_data_ready.
 *   - leave a work item queued after its tunnel is freed; tbt_listen_stop()
 *     cancels and flushes every listener before the caller drops the last
 *     reference.
 *   - accept a connection without a live tunnel: the work item re-checks the
 *     listener's socket and the tunnel's accepting flag.
 */
#include <linux/rculist.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"

// sk_data_ready runs in softirq, so it may only queue work. The stock callback
// (sock_def_readable) is not exported, and a NULL callback would leave
// listeners that never wake, so we install our own.
static void tbt_ready(struct sock *sk)
{
    struct tbt_listener *l;

    read_lock_bh(&sk->sk_callback_lock);
    l = sk->sk_user_data;
    if (!l || !l->t || !atomic_read(&l->t->accepting))
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    schedule_work(&l->w_accept);
    read_unlock_bh(&sk->sk_callback_lock);
}

/* One work item accepts at most this many connections, then re-queues. An
 * unbounded loop kept the worker on one CPU for as long as a connect flood
 * kept the backlog non-empty; the workqueue watchdog reported
 * tbt_accept_work hogging a CPU (HND0: 19 times in 22 s). Re-queuing puts the
 * item behind other work on the same pool, which is the fairness the loop
 * was missing. */
#define TBT_ACCEPT_BATCH 32

static void tbt_accept_one(struct tbt_listener *l)
{
    struct tbt_tunnel *t = l->t;
    struct socket *newsock;
    int budget = TBT_ACCEPT_BATCH;
    int err;

    for (;;)
    {
        if (!atomic_read(&t->accepting) || !READ_ONCE(l->sock))
            return;

        // Non-blocking: a spurious wake (peer RST before accept) must not park
        // the work item. The loop drains the backlog, then returns.
        err = kernel_accept(l->sock, &newsock, O_NONBLOCK);
        if (err)
        {
            if (err != -EAGAIN && err != -EWOULDBLOCK && err != -EINTR)
                pr_debug("tbtun: accept on tunnel %llu: %d\n", t->cfg.id, err);
            return;
        }

        // A near tunnel's listener faces applications: the accepted socket
        // becomes the public end of a stream. A far tunnel's listener faces
        // carriers: the accepted socket IS the carrier. Either way the socket
        // is never exposed to userspace.
        if (t->cfg.role == TBT_ROLE_FAR)
        {
            err = tbt_carrier_adopt(t, newsock);
        }
        else
        {
            /* This child inherited tbt_ready() and sk_user_data = l from the
             * listener. Until tbt_sock_attach() installs the stream's
             * callbacks, every byte the application sends into the accepted
             * socket fires tbt_ready() and schedules l->w_accept: a false
             * listener wakeup for as long as the connection waits in
             * sess->pending (up to the FEC close handshake wait). Restore the
             * core callbacks before the hand-off. The socket then holds no
             * reference to the listener, so a late softirq can never queue
             * work against a listener being torn down. */
            tbt_sk_ops_restore(newsock->sk, &l->def_ops);
            err = tbt_stream_start(t->sess, newsock, l->slot);
        }
        if (err)
        {
            pr_warn_ratelimited("tbtun: tunnel %llu refused an incoming connection: %d\n",
                                t->cfg.id, err);
            /* A refused near child was already restored above; a refused
             * carrier may still carry the inherited tbt_ready() and
             * sk_user_data = l if adopt failed before installing its own
             * callbacks. Restore is idempotent. Close with RST and never
             * leave module text (or the listener pointer) on a socket that
             * can outlive this tunnel. */
            tbt_sk_ops_restore(newsock->sk, &l->def_ops);
            tbt_sock_abort_on_close(newsock);
            sock_release(newsock);
            /* -ENOSPC is the provisional cap: the refusal was cheap, and the
             * backlog still has to be drained so a later accept can fire. */
            if (--budget == 0)
            {
                schedule_work(&l->w_accept);
                return;
            }
            continue;
        }
        t->streams++;
        pr_debug("tbtun: tunnel %llu accepted a connection on slot %u\n",
                 t->cfg.id, l->slot);
        if (--budget == 0)
        {
            schedule_work(&l->w_accept);
            return;
        }
        cond_resched();
    }
}

void tbt_accept_work(struct work_struct *w)
{
    struct tbt_listener *l = container_of(w, struct tbt_listener, w_accept);

    if (!READ_ONCE(l->sock) || !l->t)
        return;
    if (!atomic_read(&l->t->accepting))
        return;
    tbt_accept_one(l);
}

/* Bind one listener. `port` is host order; the slot is stored so an accepted
 * connection can name its mapping in OPEN. */
static int tbt_listener_start(struct tbt_listener *l, u8 slot, u16 port)
{
    struct tbt_tunnel *t = l->t;
    struct sockaddr_storage ss;
    int alen = tbt_addr_to_sockaddr(&t->cfg.listen, tbt_port_be16(port), &ss);
    struct socket *s;
    int backlog = 128;
    int err;

    /* The shared TBT_AF_* constants are the Linux ABI numbers; fail the build
     * rather than a bind if a future kernel renumbers a family. */
    BUILD_BUG_ON(TBT_AF_INET != AF_INET || TBT_AF_INET6 != AF_INET6);
    err = sock_create_kern(t->net, t->cfg.listen.family, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        return err;

    /* This is the only socket whose callbacks are still the sock_init_data()
     * defaults.  Accepted sockets inherit tbt_ready() from this listener, so
     * they must never be used as the snapshot source. */
    tbt_sk_ops_save(&l->def_ops, s->sk);
    if (WARN_ON_ONCE(!l->def_ops.data_ready || !l->def_ops.write_space ||
                     !l->def_ops.state_change || !l->def_ops.error_report))
    {
        sock_release(s);
        return -EINVAL;
    }

    /* A tunnel line that replaces an existing one rebinds the same port while
     * the old carrier connections are still in TIME_WAIT. Without REUSEADDR
     * the bind fails with EADDRINUSE for a minute (measured by probes C/D). */
    sock_set_reuseaddr(s->sk);

    /* The listener answers every SYN-ACK, and the handshake freezes the
     * connection's window scale and window_clamp from this socket's receive
     * buffer. Sizing only the accepted socket afterwards leaves the receive
     * window at 64 KiB (measured: SO_RCVBUF 4 MiB set after accept still
     * advertises rcv_wnd 65483), below a 218 ms BDP. A near listener accepts
     * the parent application connections, so it reserves the parent buffer
     * (8 MiB, doubled to 16 MiB); a far listener accepts carriers and gets
     * one carrier's 0.8 MiB floor. */
    if (t->cfg.role == TBT_ROLE_NEAR)
        tbt_parent_setsockbuf(s);
    else
        tbt_car_setsockbuf(s, t->cfg.carriers ? t->cfg.carriers
                                              : (u32)t->cfg.n + t->cfg.m);

    err = kernel_bind(s, (tbt_sockaddr *)&ss, alen);
    if (err)
        goto fail;

    /* A far listener accepts one connection per carrier slot; a client that
     * opens its whole carrier set at once can exceed the historical
     * 128-entry queue (256 by default). Size the far queue from the session's
     * slots so the burst is absorbed by the backlog instead of forced into
     * SYN retransmissions. The near side faces arbitrary applications and
     * keeps the usual 128. */
    if (t->cfg.role == TBT_ROLE_FAR)
    {
        u32 slots = t->cfg.carriers ? t->cfg.carriers
                                    : (u32)t->cfg.n + t->cfg.m;

        if (slots > (u32)backlog)
        {
            if (slots > (u32)TBT_MAX_CARRIERS_LIMIT)
                slots = TBT_MAX_CARRIERS_LIMIT;
            backlog = (int)slots;
        }
    }
    err = kernel_listen(s, backlog);
    if (err)
        goto fail;

    l->slot = slot;
    l->sock = s;
    atomic_set(&t->accepting, 0);
    // C1 drove accept explicitly; sk_data_ready is installed in C2 now that
    // the accepted socket has somewhere to go.
    write_lock_bh(&s->sk->sk_callback_lock);
    s->sk->sk_user_data = l;
    s->sk->sk_data_ready = tbt_ready;
    write_unlock_bh(&s->sk->sk_callback_lock);
    return 0;

fail:
    sock_release(s);
    return err;
}

int tbt_listen_start(struct tbt_tunnel *t)
{
    u16 ports[TBT_MAP_MAX];
    u8 n = 0;
    u8 i;
    int err;

    ports[n++] = tbt_port_host16(t->cfg.listen_port);
    if (t->cfg.role == TBT_ROLE_NEAR)
        for (i = 0; i < t->cfg.nmap && n < TBT_MAP_MAX; i++)
            ports[n++] = t->cfg.maps[i].listen_port;

    t->nlisteners = 0;
    for (i = 0; i < n; i++)
    {
        struct tbt_listener *l = &t->listeners[i];

        l->t = t;
        err = tbt_listener_start(l, i, ports[i]);
        if (err)
        {
            /* A partially started tunnel must not leave earlier listeners
             * bound: tunnel_add() will free the object. */
            tbt_listen_stop(t);
            return err;
        }
        t->nlisteners = (u8)(i + 1);
    }
    /* Every listener was born from sock_create_kern(), so the core callback
     * snapshot is identical; listener 0 is the session's restore source. */
    t->def_ops = t->listeners[0].def_ops;
    return 0;
}

void tbt_listen_stop(struct tbt_tunnel *t)
{
    u8 i;

    atomic_set(&t->accepting, 0);
    for (i = 0; i < t->nlisteners; i++)
    {
        struct tbt_listener *l = &t->listeners[i];
        struct socket *s = l->sock;

        if (!s)
            continue;

        /* Restore the core callbacks before cancelling.  A ready event that
         * lands after this must not schedule work against a tunnel being
         * freed. */
        tbt_sk_ops_restore(s->sk, &l->def_ops);
        cancel_work_sync(&l->w_accept);

        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        l->sock = NULL;
    }
    t->nlisteners = 0;
}
