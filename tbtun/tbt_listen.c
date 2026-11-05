/* tbt_listen.c - the kernel-side listener for a near tunnel.
 *
 * Owns:
 *   - creating, binding and listening on the tunnel's public socket through
 *     sock_create_kern/kernel_bind/kernel_listen (DESIGN.md §2.1, §8.5).
 *   - the accept loop. It runs from a work item scheduled by sk_data_ready,
 *     because accept may sleep and sk_data_ready runs in softirq.
 *   - closing accepted sockets. In C1 that is the whole data path: the
 *     connection is accepted, logged, and closed, so a client sees EOF
 *     (DESIGN.md §15 C1). C2 replaces the close with the tunnel hand-off.
 *
 * Must never:
 *   - call lock_sock or any sleeping function from sk_data_ready.
 *   - leave a work item queued after its tunnel is freed; tbt_listen_stop()
 *     cancels and flushes before the caller drops the last reference.
 *   - accept a connection without a live tunnel: the work item re-checks
 *     t->lsock under RCU before accepting.
 */
#include <linux/rculist.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"

struct tbt_accept_ctx
{
    struct tbt_tunnel *t;
};

// sk_data_ready runs in softirq, so it may only queue work. The stock callback
// (sock_def_readable) is not exported, and a NULL callback would leave
// listeners that never wake, so we install our own.
static void tbt_ready(struct sock *sk)
{
    struct tbt_tunnel *t;

    read_lock_bh(&sk->sk_callback_lock);
    t = sk->sk_user_data;
    if (!t || !atomic_read(&t->accepting))
    {
        read_unlock_bh(&sk->sk_callback_lock);
        return;
    }
    schedule_work(&t->w_accept);
    read_unlock_bh(&sk->sk_callback_lock);
}

static void tbt_accept_one(struct tbt_tunnel *t)
{
    struct socket *newsock;
    int err;

    for (;;)
    {
        if (!atomic_read(&t->accepting))
            return;

        // Non-blocking: a spurious wake (peer RST before accept) must not park
        // the work item. The loop drains the backlog, then returns.
        err = kernel_accept(t->lsock, &newsock, O_NONBLOCK);
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
        err = t->cfg.role == TBT_ROLE_FAR ? tbt_carrier_adopt(t, newsock)
                                          : tbt_stream_start(t->sess, newsock);
        if (err)
        {
            pr_warn_ratelimited("tbtun: tunnel %llu refused an incoming connection: %d\n",
                                t->cfg.id, err);
            /* This child inherited tbt_ready() and sk_user_data = t.  It is
             * not a listener.  Restore core callbacks and close it with RST,
             * or it can call module text after this tunnel is freed. */
            tbt_sk_ops_restore(newsock->sk, &t->def_ops);
            tbt_sock_abort_on_close(newsock);
            sock_release(newsock);
            continue;
        }
        t->streams++;
        pr_debug("tbtun: tunnel %llu accepted a connection\n", t->cfg.id);
    }
}

void tbt_accept_work(struct work_struct *w)
{
    struct tbt_tunnel *t = container_of(w, struct tbt_tunnel, w_accept);

    if (!READ_ONCE(t->lsock))
        return;
    if (!atomic_read(&t->accepting))
        return;
    tbt_accept_one(t);
}

int tbt_listen_start(struct tbt_tunnel *t)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = t->cfg.listen_port,
        .sin_addr.s_addr = t->cfg.listen_addr,
    };
    struct socket *s;
    int err;

    err = sock_create_kern(t->net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &s);
    if (err)
        return err;

    /* This is the only socket whose callbacks are still the sock_init_data()
     * defaults.  Accepted sockets inherit tbt_ready() from this listener, so
     * they must never be used as the snapshot source. */
    tbt_sk_ops_save(&t->def_ops, s->sk);
    if (WARN_ON_ONCE(!t->def_ops.data_ready || !t->def_ops.write_space ||
                     !t->def_ops.state_change || !t->def_ops.error_report))
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
     * buffer. Sizing only the accepted socket afterwards leaves the carrier's
     * advertised window at the default ~208 KiB, below a 218 ms BDP. */
    tbt_car_setsockbuf(s, &t->cfg, t->cfg.n);

    err = kernel_bind(s, (struct sockaddr_unsized *)&addr, sizeof(addr));
    if (err)
        goto fail;

    err = kernel_listen(s, 128);
    if (err)
        goto fail;

    t->lsock = s;
    atomic_set(&t->accepting, 0);
    // C1 drives accept explicitly; sk_data_ready is installed in C2 when the
    // accepted socket has somewhere to go. Until then a bare accept probe keeps
    // the path exercised from /proc-triggered work.
    write_lock_bh(&s->sk->sk_callback_lock);
    s->sk->sk_user_data = t;
    s->sk->sk_data_ready = tbt_ready;
    write_unlock_bh(&s->sk->sk_callback_lock);
    return 0;

fail:
    sock_release(s);
    return err;
}

void tbt_listen_stop(struct tbt_tunnel *t)
{
    struct socket *s = t->lsock;

    if (!s)
        return;

    /* Restore the core callbacks before cancelling.  A ready event that lands
     * after this must not schedule work against a tunnel being freed. */
    tbt_sk_ops_restore(s->sk, &t->def_ops);
    atomic_set(&t->accepting, 0);
    cancel_work_sync(&t->w_accept);

    kernel_sock_shutdown(s, SHUT_RDWR);
    sock_release(s);
    t->lsock = NULL;
}
