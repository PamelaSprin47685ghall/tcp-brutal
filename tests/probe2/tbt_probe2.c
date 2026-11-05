/* tbt_probe2.c - injection probe: tcp_send_rcvq() on a LIVE accepted socket.
 *
 * Companion to tbt_probe.c. That one settled proto_ops replacement; this one
 * settles the delivery half: pushing tunnel bytes into a socket's receive
 * queue instead of sending them.
 *
 * Design notes, each learned by getting it wrong first:
 *   - Several connections are kept alive at once (P2_SLOTS). A probe that keeps
 *     only one and refuses the next accept makes every later phase talk to a
 *     socket the module already released; the client then sees resets, which
 *     looks like a kernel fault and is not one.
 *   - GETFD hands the accepted socket to the test process through
 *     sock_alloc_file(). The test's own connect() fd is the *client* half, a
 *     different socket; reading it can never see injected bytes.
 *   - The slot keeps the file, so STATUS/INJECT still reach the socket after
 *     it has been handed out.
 *   - `arm_repair` defaults to 0. tcp_send_rcvq() itself does not require
 *     repair mode; repair=1 additionally makes the socket's own recvmsg return
 *     -EPERM and its close send no FIN, which would confound the assertions.
 */
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <net/inet_common.h>
#include <net/inet_connection_sock.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbt_probe2_uapi.h"

#define P2_SLOTS 8

static int port = 19210;
module_param(port, int, 0444);
MODULE_PARM_DESC(port, "listen port (default 19210)");

static bool arm_repair;
module_param(arm_repair, bool, 0444);
MODULE_PARM_DESC(arm_repair, "set tp->repair before injecting (default 0)");

struct p2_slot
{
    struct socket *sock; /* accepted */
    struct file *file;   /* set once the fd has been handed out */
    bool used;
    int accepted_when;
    u64 inject_calls;
    u64 inject_bytes;
    u64 inject_errors;
};

static struct p2_slot slots[P2_SLOTS];
static int accept_seq;

static struct socket *lsock;
static struct mutex p2_lock;
static struct work_struct accept_work;
static struct work_struct inject_work;
static struct kprobe kp;
static int (*send_rcvq)(struct sock *, struct msghdr *, size_t);

/* Pending injection, run by inject_work (it may sleep in lock_sock). */
static int inj_slot;
static size_t inj_len;
static u8 inj_byte;
static int inj_err;

static u32 app_tgid;

static int cur_slot(void)
{
    int best = -1, i;

    for (i = 0; i < P2_SLOTS; i++)
    {
        if (!slots[i].used)
            continue;
        if (best < 0 || slots[i].accepted_when > slots[best].accepted_when)
            best = i;
    }
    return best;
}

/* The socket behind a slot: through the file when one exists, because after
 * GETFD the file is what keeps the socket alive. */
static struct socket *slot_socket(int i)
{
    if (i < 0 || i >= P2_SLOTS || !slots[i].used)
        return NULL;
    if (slots[i].file)
        return slots[i].file->private_data;
    return slots[i].sock;
}

static void p2_data_ready(struct sock *sk)
{
    schedule_work(&accept_work);
}

static void p2_accept_work(struct work_struct *w)
{
    struct socket *newsock;
    int i;

    for (;;)
    {
        if (kernel_accept(lsock, &newsock, O_NONBLOCK))
            return;

        mutex_lock(&p2_lock);
        for (i = 0; i < P2_SLOTS; i++)
        {
            if (!slots[i].used)
                break;
        }
        if (i == P2_SLOTS)
        {
            mutex_unlock(&p2_lock);
            sock_release(newsock);
            continue;
        }
        slots[i].used = true;
        slots[i].sock = newsock;
        slots[i].file = NULL;
        slots[i].accepted_when = ++accept_seq;
        slots[i].inject_calls = 0;
        slots[i].inject_bytes = 0;
        slots[i].inject_errors = 0;
        if (arm_repair)
        {
            tcp_sk(newsock->sk)->repair = 1;
            tcp_sk(newsock->sk)->repair_queue = TCP_RECV_QUEUE;
        }
        mutex_unlock(&p2_lock);

        pr_info("tbt_probe2: accepted slot %d sk=%px rcv_nxt=%u\n", i, newsock->sk,
                tcp_sk(newsock->sk)->rcv_nxt);
    }
}

static void p2_inject_work(struct work_struct *w)
{
    struct socket *s;
    struct sock *sk;
    struct msghdr msg = {};
    struct kvec iov;
    char *buf;
    u32 before;

    mutex_lock(&p2_lock);
    s = slot_socket(inj_slot);
    mutex_unlock(&p2_lock);
    if (!s)
    {
        inj_err = -ENOTCONN;
        return;
    }
    sk = s->sk;

    buf = kmalloc(inj_len, GFP_KERNEL);
    if (!buf)
    {
        inj_err = -ENOMEM;
        return;
    }
    memset(buf, inj_byte, inj_len);
    iov.iov_base = buf;
    iov.iov_len = inj_len;
    iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, inj_len);

    before = tcp_sk(sk)->rcv_nxt;
    lock_sock(sk);
    inj_err = send_rcvq(sk, &msg, inj_len);
    release_sock(sk);
    kfree(buf);

    mutex_lock(&p2_lock);
    if (inj_slot >= 0 && slots[inj_slot].used)
    {
        struct p2_slot *sl = &slots[inj_slot];

        sl->inject_calls++;
        if (inj_err > 0)
            sl->inject_bytes += inj_err;
        else
            sl->inject_errors++;
    }
    mutex_unlock(&p2_lock);

    if (inj_err < 0)
        pr_warn("tbt_probe2: inject %zu into slot %d failed: %d\n", inj_len, inj_slot,
                inj_err);
    else if (tcp_sk(sk)->rcv_nxt != before + (u32)inj_err)
        pr_warn("tbt_probe2: slot %d rcv_nxt %u -> %u after %d injected bytes\n", inj_slot,
                before, tcp_sk(sk)->rcv_nxt, inj_err);
}

static void p2_fill_status(struct tbt_p2_status *st, struct socket *s, struct p2_slot *sl)
{
    struct sock *sk = s->sk;
    const struct tcp_sock *tp = tcp_sk(sk);

    memset(st, 0, sizeof(*st));
    st->rcv_nxt = tp->rcv_nxt;
    st->copied_seq = tp->copied_seq;
    st->rcv_wup = tp->rcv_wup;
    st->rcv_wnd = tp->rcv_wnd;
    st->rcv_ssthresh = tp->rcv_ssthresh;
    st->rcv_mss = inet_csk(sk)->icsk_ack.rcv_mss;
    st->rcvbuf = sk->sk_rcvbuf;
    st->mem_alloc = atomic_read(&sk->sk_rmem_alloc);
    st->state = sk->sk_state;
    st->sock_done = sock_flag(sk, SOCK_DONE);
    st->shutdown = READ_ONCE(sk->sk_shutdown);
    st->sk_err = READ_ONCE(sk->sk_err);
    st->qlen = skb_queue_len(&sk->sk_receive_queue);
    st->bytes_received = tp->bytes_received;
    st->urg_data = tp->urg_data;
    st->repair = tp->repair;
    st->repair_queue = tp->repair_queue;
    st->inject_calls = sl ? sl->inject_calls : 0;
    st->inject_bytes = sl ? sl->inject_bytes : 0;
    st->inject_errors = sl ? sl->inject_errors : 0;
    st->snd_wnd = tp->snd_wnd;
    st->snd_nxt = tp->snd_nxt;
    st->snd_una = tp->snd_una;
    st->write_seq = tp->write_seq;
    st->packets_out = tp->packets_out;
    st->retrans = inet_csk(sk)->icsk_retransmits;
    st->wmem_queued = sk->sk_wmem_queued;
    st->tcp_header_len = tp->tcp_header_len;
    st->app_tgid = app_tgid;
}

/* What the head of the receive queue looks like: the fields that decide
 * whether tcp_recvmsg_locked can consume it. */
static int p2_fill_skbinfo(struct tbt_p2_skbinfo *si, struct socket *s)
{
    struct sock *sk = s->sk;
    struct sk_buff *skb;

    memset(si, 0, sizeof(*si));
    si->rcv_nxt = tcp_sk(sk)->rcv_nxt;
    si->copied_seq = tcp_sk(sk)->copied_seq;
    si->urg_data = tcp_sk(sk)->urg_data;
    si->qlen = skb_queue_len(&sk->sk_receive_queue);

    skb = skb_peek(&sk->sk_receive_queue);
    if (!skb)
        return -ENOENT;

    si->seq = TCP_SKB_CB(skb)->seq;
    si->end_seq = TCP_SKB_CB(skb)->end_seq;
    si->ack_seq = TCP_SKB_CB(skb)->ack_seq;
    si->tcp_flags = TCP_SKB_CB(skb)->tcp_flags;
    si->len = skb->len;
    si->data_len = skb->data_len;
    si->truesize = skb->truesize;
    si->users = refcount_read(&skb->users);
    si->offset_from_copied = si->seq - si->copied_seq;
    return 0;
}

static long p2_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int i = cur_slot();
    struct socket *s;

    switch (cmd)
    {
    case TBT_P2_GETFD:
    {
        struct file *nf;
        int fd;

        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (!s || slots[i].file)
        {
            mutex_unlock(&p2_lock);
            return -EALREADY;
        }
        nf = sock_alloc_file(s, 0, NULL);
        if (IS_ERR(nf))
        {
            mutex_unlock(&p2_lock);
            return PTR_ERR(nf);
        }
        fd = get_unused_fd_flags(O_RDWR);
        if (fd < 0)
        {
            mutex_unlock(&p2_lock);
            fput(nf);
            return fd;
        }
        slots[i].file = nf;
        fd_install(fd, nf);
        mutex_unlock(&p2_lock);
        pr_info("tbt_probe2: slot %d handed out as fd %d\n", i, fd);
        return fd;
    }

    case TBT_P2_INJECT:
    {
        struct tbt_p2_inject req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        if (!req.len || req.len > (1u << 20))
            return -EINVAL;
        mutex_lock(&p2_lock);
        if (!slot_socket(i))
        {
            mutex_unlock(&p2_lock);
            return -ENOTCONN;
        }
        inj_slot = i;
        inj_len = req.len;
        inj_byte = req.byte;
        inj_err = 0;
        mutex_unlock(&p2_lock);

        schedule_work(&inject_work);
        flush_work(&inject_work);
        return inj_err < 0 ? inj_err : 0;
    }

    case TBT_P2_STATUS:
    {
        struct tbt_p2_status st;

        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (!s)
        {
            mutex_unlock(&p2_lock);
            return -ENOTCONN;
        }
        p2_fill_status(&st, s, &slots[i]);
        mutex_unlock(&p2_lock);
        return copy_to_user((void __user *)arg, &st, sizeof(st)) ? -EFAULT : 0;
    }

    case TBT_P2_SKBINFO:
    {
        struct tbt_p2_skbinfo si;
        int rc;

        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (!s)
        {
            mutex_unlock(&p2_lock);
            return -ENOTCONN;
        }
        rc = p2_fill_skbinfo(&si, s);
        mutex_unlock(&p2_lock);
        if (rc)
            return rc;
        return copy_to_user((void __user *)arg, &si, sizeof(si)) ? -EFAULT : 0;
    }

    case TBT_P2_DISARM:
        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (s)
        {
            tcp_sk(s->sk)->repair = 0;
            tcp_sk(s->sk)->repair_queue = TCP_NO_QUEUE;
        }
        mutex_unlock(&p2_lock);
        return s ? 0 : -ENOTCONN;

    case TBT_P2_ARM:
        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (s)
        {
            tcp_sk(s->sk)->repair = 1;
            tcp_sk(s->sk)->repair_queue = TCP_RECV_QUEUE;
        }
        mutex_unlock(&p2_lock);
        return s ? 0 : -ENOTCONN;

    case TBT_P2_FORCE_SEND:
    {
        struct msghdr msg = {};
        struct kvec iov;
        static const char hello[] = "SERVER-HELLO-0123456789";
        int rc;

        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (!s)
        {
            mutex_unlock(&p2_lock);
            return -ENOTCONN;
        }
        tcp_sk(s->sk)->repair = 0;
        tcp_sk(s->sk)->repair_queue = TCP_NO_QUEUE;
        iov.iov_base = (void *)hello;
        iov.iov_len = sizeof(hello) - 1;
        iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &iov, 1, sizeof(hello) - 1);
        rc = s->sk->sk_prot->sendmsg(s->sk, &msg, sizeof(hello) - 1);
        mutex_unlock(&p2_lock);
        return rc;
    }

    case TBT_P2_CLAIM:
    {
        struct tbt_p2_claim req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        app_tgid = req.tgid;
        return 0;
    }

    case TBT_P2_CLOSE:
        mutex_lock(&p2_lock);
        s = slot_socket(i);
        if (s)
        {
            struct sock *sk = s->sk;

            lock_sock(sk);
            sk->sk_prot->close(sk, 0);
            release_sock(sk);
        }
        if (i >= 0)
        {
            if (slots[i].file)
            {
                fput(slots[i].file);
                slots[i].file = NULL;
            }
            slots[i].sock = NULL;
            slots[i].used = false;
        }
        mutex_unlock(&p2_lock);
        return s ? 0 : -ENOTCONN;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations p2_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = p2_ioctl,
    .compat_ioctl = p2_ioctl,
};

static struct miscdevice p2_mdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "tbtprobe2",
    .fops = &p2_fops,
    .mode = 0600,
};

static int __init p2_init(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((u16)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int err;

    kp.symbol_name = "tcp_send_rcvq";
    err = register_kprobe(&kp);
    if (err)
    {
        pr_err("tbt_probe2: cannot resolve tcp_send_rcvq: %d\n", err);
        return err;
    }
    send_rcvq = (void *)kp.addr;
    unregister_kprobe(&kp);
    pr_info("tbt_probe2: tcp_send_rcvq at %ps (arm_repair=%d)\n", send_rcvq, arm_repair);

    mutex_init(&p2_lock);
    INIT_WORK(&accept_work, p2_accept_work);
    INIT_WORK(&inject_work, p2_inject_work);

    err = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &lsock);
    if (err)
        return err;
    err = kernel_bind(lsock, (struct sockaddr_unsized *)&addr, sizeof(addr));
    if (err)
        goto fail;
    err = kernel_listen(lsock, 32);
    if (err)
        goto fail;
    write_lock_bh(&lsock->sk->sk_callback_lock);
    lsock->sk->sk_data_ready = p2_data_ready;
    write_unlock_bh(&lsock->sk->sk_callback_lock);

    err = misc_register(&p2_mdev);
    if (err)
        goto fail;
    pr_info("tbt_probe2: listening on 127.0.0.1:%d\n", port);
    return 0;

fail:
    sock_release(lsock);
    lsock = NULL;
    return err;
}

static void __exit p2_exit(void)
{
    int i;

    misc_deregister(&p2_mdev);
    cancel_work_sync(&inject_work);
    cancel_work_sync(&accept_work);
    for (i = 0; i < P2_SLOTS; i++)
    {
        if (slots[i].file)
            fput(slots[i].file);
        else if (slots[i].sock)
            sock_release(slots[i].sock);
    }
    if (lsock)
        sock_release(lsock);
}

module_init(p2_init);
module_exit(p2_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tbtun probe 2: tcp_send_rcvq on a live accepted socket");
