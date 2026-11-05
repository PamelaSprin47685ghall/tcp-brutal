// SPDX-License-Identifier: GPL-2.0
/*
 * tbt_probe.c - throwaway probe: can a module serve an accepted socket's
 * read()/poll() from its own queue by replacing sock->ops?
 *
 * Not part of the module under test and not in the DKMS tarball. Its only job
 * is to answer one architectural question with running code, before tbtun
 * depends on the answer.
 *
 * What it builds:
 *   - a listener on 127.0.0.1:19200 (sock_create_kern/kernel_bind/kernel_listen)
 *   - an accept work item, scheduled from sk_data_ready
 *   - on accept: newsock->ops = &probe_ops, a __ro_after_init copy of
 *     inet_stream_ops whose .recvmsg and .poll are ours
 *   - a module-side queue holding a synthetic payload, released 1500 ms after
 *     accept, with wake_up_interruptible_poll(sk_sleep(sk), EPOLLIN|EPOLLRDNORM)
 *   - /dev/tbtprobe: GETFD, STATS, SHUTDOWN, RESET
 *
 * The payload is never put in sk_receive_queue. recvmsg copies it straight out
 * of the module buffer, so nothing here touches TCP's receive path.
 *
 * GETFD is the control that separates "ops replacement is broken" from "the
 * application's descriptor is a different socket". It wraps the accepted
 * socket in a file with sock_alloc_file() and installs it in the caller's fd
 * table, so a test process can read the very socket whose ops we replaced.
 * Without that, the application's fd is the client half of a loopback
 * connection and never reaches these ops at all.
 */
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <net/inet_common.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbt_probe_uapi.h"

#define PROBE_PORT 19200
#define PROBE_DELAY_MS 1500
#define PROBE_PAYLOAD_LEN 65536

struct probe_conn
{
    struct socket *sock; /* accepted; ops replaced */
    struct socket *lsock;
    struct socket *pending; /* accepted but not yet handed out */
    struct delayed_work release;
    struct mutex lock; /* guards off/ready/eof, the socket pointers and counters */
    struct file *outs[2]; /* files handed out by GETFD, newest last */
    size_t off;
    bool ready; /* payload released to readers */
    bool eof;   /* payload drained */
    bool handed_out;
    bool shutting;
    u64 recvmsg_calls;
    u64 poll_calls;
    u64 wakes;
    struct miscdevice mdev;
};

static struct probe_conn pc;
static struct proto_ops probe_ops __ro_after_init;
static struct work_struct accept_work;
static int probe_up;

static const char payload[] = "PROBE-OK\n";

/* Fill from stream offset `off`, so successive reads continue the pattern
 * instead of restarting it: a reader that consumes the payload in pieces must
 * see the same bytes as one that reads it whole. */
static void probe_fill(char *dst, size_t len, size_t off)
{
    size_t unit = sizeof(payload) - 1;
    size_t i;

    for (i = 0; i < len; i++)
        dst[i] = payload[(off + i) % unit];
}

/* Bytes currently readable without blocking. */
/* The socket behind the most recent fd we handed out, or NULL if the
 * application already closed every one of them. */
static struct socket *probe_socket(struct probe_conn *c)
{
    struct socket *s = NULL;

    if (c->outs[1])
        s = c->outs[1]->private_data;
    else if (c->outs[0])
        s = c->outs[0]->private_data;
    return s;
}

static size_t probe_avail(struct probe_conn *c)
{
    if (!c->ready)
        return 0;
    return PROBE_PAYLOAD_LEN - c->off;
}

static void probe_wake(struct probe_conn *c, __poll_t mask)
{
    struct socket *s;
    struct sock *sk;

    mutex_lock(&c->lock);
    s = probe_socket(c);
    mutex_unlock(&c->lock);
    if (!s)
        return;
    sk = READ_ONCE(s->sk);
    if (!sk)
        return;
    c->wakes++;
    wake_up_interruptible_poll(sk_sleep(sk), mask);
}

/* Runs 1500 ms after accept: release the payload and wake a blocked reader. */
static void probe_release_work(struct work_struct *w)
{
    struct probe_conn *c = &pc;

    mutex_lock(&c->lock);
    if (c->shutting)
    {
        mutex_unlock(&c->lock);
        return;
    }
    c->ready = true;
    mutex_unlock(&c->lock);

    pr_info("tbt_probe: payload released, waking waiters\n");
    probe_wake(c, EPOLLIN | EPOLLRDNORM);
}

/* ------------------------------------------------------------------------- */
/* Our proto_ops                                                             */

static int probe_recvmsg(struct socket *sock, struct msghdr *msg, size_t total_len, int flags)
{
    struct probe_conn *c = &pc;
    /* `sock` is the socket this call arrived on; it is alive for the duration
     * of the call because the caller holds the file. Keep using it rather than
     * c->sock, which may name a newer connection. */
    size_t want, avail, n;
    char *buf;

    mutex_lock(&c->lock);
    c->recvmsg_calls++;

    if (c->eof)
    {
        mutex_unlock(&c->lock);
        return 0;
    }

    for (;;)
    {
        avail = probe_avail(c);
        if (avail)
            break;
        if (c->shutting)
        {
            c->eof = true;
            mutex_unlock(&c->lock);
            return 0;
        }
        if (flags & MSG_DONTWAIT)
        {
            mutex_unlock(&c->lock);
            return -EAGAIN;
        }
        mutex_unlock(&c->lock);
        /* Wait on the socket's own wait queue: the same one poll_wait uses,
         * so a single wake serves both blocking reads and poll/epoll. */
        if (wait_event_interruptible(*sk_sleep(sock->sk),
                                     READ_ONCE(c->ready) || READ_ONCE(c->shutting)))
            return -ERESTARTSYS;
        mutex_lock(&c->lock);
    }

    want = min(total_len, avail);
    buf = kmalloc(want, GFP_KERNEL);
    if (!buf)
    {
        mutex_unlock(&c->lock);
        return -ENOMEM;
    }
    probe_fill(buf, want, c->off);
    if (copy_to_iter(buf, want, &msg->msg_iter) != want)
    {
        kfree(buf);
        mutex_unlock(&c->lock);
        return -EFAULT;
    }
    kfree(buf);

    n = want;
    if (!(flags & MSG_PEEK))
    {
        c->off += n;
        if (c->off >= PROBE_PAYLOAD_LEN)
            c->eof = true;
    }
    mutex_unlock(&c->lock);
    return (int)n;
}

static __poll_t probe_poll(struct file *file, struct socket *sock, poll_table *wait)
{
    struct probe_conn *c = &pc;
    __poll_t mask = 0;

    mutex_lock(&c->lock);
    c->poll_calls++;
    mutex_unlock(&c->lock);

    poll_wait(file, sk_sleep(sock->sk), wait);

    mutex_lock(&c->lock);
    if (probe_avail(c))
        mask |= EPOLLIN | EPOLLRDNORM;
    if (c->eof || c->shutting)
        mask |= EPOLLHUP | EPOLLRDHUP;
    mutex_unlock(&c->lock);
    return mask;
}

/* ------------------------------------------------------------------------- */
/* /dev/tbtprobe                                                             */

/*
 * Ownership rule, learned the hard way: once a socket has been handed out
 * through GETFD its lifetime belongs to the file. An application that closed
 * the fd has already freed it, and touching pc.sock then is a use-after-free
 * (kernel_sock_shutdown dereferences sock->ops->shutdown and oopsed at
 * offset 0x68 when this probe got it wrong). Resolve through the file, and
 * do nothing if there is no live file.
 */

static void probe_shutdown_socket(struct probe_conn *c)
{
    struct socket *s;

    mutex_lock(&c->lock);
    c->shutting = true;
    /*
     * Resolve the socket without taking a new file reference. fput() here
     * would run ->release (and so tcp_close) while this very call may be
     * running from that socket's tenth-generation sk_data_ready callback,
     * where lock_sock is already held. Shutdown does not free anything; the
     * file's owner still does, exactly once.
     */
    s = c->outs[1] ? c->outs[1]->private_data
      : c->outs[0] ? c->outs[0]->private_data
                   : NULL;
    if (s)
        c->sock = s;
    mutex_unlock(&c->lock);

    if (s)
    {
        kernel_sock_shutdown(s, SHUT_RDWR);
        probe_wake(c, EPOLLHUP | EPOLLRDHUP);
        pr_info("tbt_probe: socket shut down\n");
    }
    else
    {
        pr_info("tbt_probe: nothing to shut down (no live fd)\n");
    }
}

static long probe_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct probe_conn *c = &pc;
    struct tbt_probe_stats st = {};
    struct socket *s;
    struct file *nf;
    int fd;

    switch (cmd)
    {
    case TBT_PROBE_STATS:
        mutex_lock(&c->lock);
        st.recvmsg_calls = c->recvmsg_calls;
        st.poll_calls = c->poll_calls;
        st.wakes = c->wakes;
        st.bytes_served = c->off;
        st.ready = c->ready;
        st.eof = c->eof;
        st.handed_out = c->handed_out;
        mutex_unlock(&c->lock);
        if (copy_to_user((void __user *)arg, &st, sizeof(st)))
            return -EFAULT;
        return 0;

    case TBT_PROBE_GETFD:
        mutex_lock(&c->lock);
        s = c->pending;
        c->pending = NULL;
        if (!s)
        {
            mutex_unlock(&c->lock);
            return -EAGAIN;
        }
        /* Rearm the payload so the test can observe the wake from a clean
         * slate: first poll must see nothing, second must see data. */
        c->ready = false;
        c->eof = false;
        c->shutting = false;
        c->off = 0;
        c->handed_out = true;
        mutex_unlock(&c->lock);

        cancel_delayed_work_sync(&c->release);
        schedule_delayed_work(&c->release, msecs_to_jiffies(PROBE_DELAY_MS));

        nf = sock_alloc_file(s, 0, NULL);
        if (IS_ERR(nf))
            return PTR_ERR(nf);
        fd = get_unused_fd_flags(O_RDWR);
        if (fd < 0)
        {
            fput(nf);
            return fd;
        }
        fd_install(fd, nf);

        mutex_lock(&c->lock);
        if (c->outs[0])
            fput(c->outs[0]);
        c->outs[0] = c->outs[1];
        c->outs[1] = get_file(nf);
        c->sock = s;
        mutex_unlock(&c->lock);
        pr_info("tbt_probe: handed out accepted socket as fd %d\n", fd);
        return fd;

    case TBT_PROBE_SHUTDOWN:
        probe_shutdown_socket(c);
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations probe_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = probe_ioctl,
    .compat_ioctl = probe_ioctl,
};

/* ------------------------------------------------------------------------- */
/* Listen / accept                                                           */

static void probe_accept_work(struct work_struct *w)
{
    struct probe_conn *c = &pc;
    struct socket *newsock;
    int err;

    for (;;)
    {
        err = kernel_accept(c->lsock, &newsock, O_NONBLOCK);
        if (err)
            return;

        /* The replacement: a copy of the stock TCP ops with two methods ours.
         * Everything else (release, shutdown, setsockopt, ...) stays inet's. */
        WRITE_ONCE(newsock->ops, &probe_ops);

        mutex_lock(&c->lock);
        if (c->pending)
        {
            /* Probe keeps one connection; refuse extras politely. */
            mutex_unlock(&c->lock);
            sock_release(newsock);
            continue;
        }
        c->sock = newsock;
        c->pending = newsock;
        mutex_unlock(&c->lock);

        pr_info("tbt_probe: accepted sk=%px, ops=%px (inet_stream_ops=%px), peer port %u\n",
                newsock->sk, newsock->ops, &inet_stream_ops,
                ntohs(inet_sk(newsock->sk)->inet_dport));

        schedule_delayed_work(&c->release, msecs_to_jiffies(PROBE_DELAY_MS));
    }
}

static void probe_data_ready(struct sock *sk)
{
    schedule_work(&accept_work);
}

static int __init probe_init(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PROBE_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int err;

    mutex_init(&pc.lock);
    INIT_WORK(&accept_work, probe_accept_work);
    INIT_DELAYED_WORK(&pc.release, probe_release_work);

    probe_ops = inet_stream_ops;
    probe_ops.recvmsg = probe_recvmsg;
    probe_ops.poll = probe_poll;

    err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &pc.lsock);
    if (err)
        return err;
    err = kernel_bind(pc.lsock, (struct sockaddr_unsized *)&addr, sizeof(addr));
    if (err)
        goto fail;
    err = kernel_listen(pc.lsock, 8);
    if (err)
        goto fail;

    write_lock_bh(&pc.lsock->sk->sk_callback_lock);
    pc.lsock->sk->sk_data_ready = probe_data_ready;
    write_unlock_bh(&pc.lsock->sk->sk_callback_lock);

    pc.mdev.minor = MISC_DYNAMIC_MINOR;
    pc.mdev.name = "tbtprobe";
    pc.mdev.fops = &probe_fops;
    pc.mdev.mode = 0600;
    err = misc_register(&pc.mdev);
    if (err)
        goto fail;

    probe_up = 1;
    pr_info("tbt_probe: listening on 127.0.0.1:%d, delay %d ms\n",
            PROBE_PORT, PROBE_DELAY_MS);
    return 0;

fail:
    sock_release(pc.lsock);
    pc.lsock = NULL;
    return err;
}

static void __exit probe_exit(void)
{
    misc_deregister(&pc.mdev);
    cancel_delayed_work_sync(&pc.release);
    cancel_work_sync(&accept_work);
    if (pc.lsock)
        sock_release(pc.lsock);
    /* A handed-out socket belongs to its file; only release what we still own. */
    mutex_lock(&pc.lock);
    if (pc.sock && !pc.handed_out)
        sock_release(pc.sock);
    mutex_unlock(&pc.lock);
    probe_up = 0;
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tbtun probe: ops replacement on an accepted TCP socket");
