/* tbt_rcvq.c - kernel half: queue and apply FEC-recovered carrier bytes.
 *
 * The wire rebuild and the pure plan live in tbt_rcvq_wire.c and
 * tbt_rcvq_plan.c so the host tests link them without kernel headers.  This
 * file only runs inside tbtun.  It uses public tcp_sock fields and exported
 * helpers: __sk_mem_schedule() and __tcp_send_ack().  It deliberately does
 * not resolve tcp_send_rcvq() or tcp_rcv_established() with a kprobe.
 *
 * The caller (tbt_car_rx_body) holds lock_sock(sk); tcp_v4_rcv() puts new
 * input into sk_backlog while that lock is held, so rcv_nxt and the
 * out_of_order_queue are stable here.
 */
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_http.h"
#include "tbt_rcvq.h"

/* __tcp_send_ack() gained a flags argument upstream, and the 6.6/6.12
 * stable tarballs carry the backport; 6.1.y still has two arguments.  The
 * supported matrix is 6.1.y (two) and 6.6.162/6.12.116/7.x (three). */
static void tbt_rcvq_send_ack(struct sock *sk, u32 rcv_nxt)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    __tcp_send_ack(sk, rcv_nxt, 0);
#else
    __tcp_send_ack(sk, rcv_nxt);
#endif
}

/* Remove SACK blocks that the cumulative ACK now covers.  This mirrors
 * tcp_sack_remove(), which is static; only the blocks fully below rcv_nxt
 * are dropped, so a block that still names out-of-order bytes stays. */
static void tbt_rcvq_sack_remove(struct tcp_sock *tp)
{
    struct tcp_sack_block *sp = &tp->selective_acks[0];
    int num_sacks = tp->rx_opt.num_sacks;
    int this_sack;

    if (RB_EMPTY_ROOT(&tp->out_of_order_queue))
    {
        tp->rx_opt.num_sacks = 0;
        return;
    }
    for (this_sack = 0; this_sack < num_sacks;)
    {
        if (!before(tp->rcv_nxt, sp->start_seq))
        {
            int i;

            /* A SACK block that straddles rcv_nxt still names bytes above
             * the cumulative ACK.  Keep it instead of copying the kernel's
             * WARN_ON path: our injected run can split a block that the
             * normal input path would have merged first. */
            if (before(tp->rcv_nxt, sp->end_seq))
            {
                this_sack++;
                sp++;
                continue;
            }
            for (i = this_sack + 1; i < num_sacks; i++)
                tp->selective_acks[i - 1] = tp->selective_acks[i];
            num_sacks--;
            continue;
        }
        this_sack++;
        sp++;
    }
    tp->rx_opt.num_sacks = num_sacks;
}

struct tbt_rcvq_repair *tbt_rcvq_alloc(u16 wire_len)
{
    struct tbt_rcvq_repair *r;

    if (!wire_len || wire_len > TBT_HTTP_FEC_FRAME_MAX)
        return NULL;
    r = kmalloc(struct_size(r, wire, wire_len), GFP_KERNEL);
    if (!r)
        return NULL;
    r->wire_len = wire_len;
    r->delta = 0;
    r->inject_len = wire_len;
    INIT_LIST_HEAD(&r->node);
    return r;
}

void tbt_rcvq_free(struct tbt_rcvq_repair *r)
{
    if (!r)
        return;
    list_del_init(&r->node);
    kfree(r);
}

int tbt_rcvq_enqueue(struct tbt_carrier *c, struct tbt_rcvq_repair *r)
{
    unsigned long flags;

    if (!c || !r)
    {
        tbt_rcvq_free(r);
        return -EINVAL;
    }
    spin_lock_irqsave(&c->rcvq_lock, flags);
    if (c->rcvq_queued >= TBT_RCVQ_MAX_PENDING)
    {
        spin_unlock_irqrestore(&c->rcvq_lock, flags);
        tbt_rcvq_free(r);
        return -ENOSPC;
    }
    list_add_tail(&r->node, &c->rcvq_q);
    c->rcvq_queued++;
    spin_unlock_irqrestore(&c->rcvq_lock, flags);
    return 0;
}

int tbt_car_rcvq_apply(struct tbt_carrier *c, struct sock *sk,
                       const u8 *wire, u16 wire_len, u16 delta,
                       u16 inject_len)
{
    struct tcp_sock *tp;
    struct sk_buff *skb, *next;
    struct rb_node *p;
    u32 seq, gap, end;
    int ret = 0;

    if (!c || !sk || !wire || !inject_len)
        return -EINVAL;
    if ((u32)delta + inject_len > wire_len)
        return -EINVAL;
    lockdep_assert_held(&sk->sk_lock.slock);

    if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
        return -ENOTCONN;
    tp = tcp_sk(sk);
    if (RB_EMPTY_ROOT(&tp->out_of_order_queue))
        return -ENOENT;

    p = rb_first(&tp->out_of_order_queue);
    seq = READ_ONCE(tp->rcv_nxt);
    gap = TCP_SKB_CB(rb_to_skb(p))->seq - seq;
    if (!gap)
        return -ENOENT;
    /* A stale plan must not overwrite bytes that arrived since it was built.
     * The caller may fill only the current hole, never past its end. */
    if (inject_len > gap)
        return -E2BIG;

    skb = alloc_skb(inject_len, GFP_KERNEL);
    if (!skb)
        return -ENOMEM;
    skb_put_data(skb, wire + delta, inject_len);
    memset(TCP_SKB_CB(skb), 0, sizeof(struct tcp_skb_cb));
    TCP_SKB_CB(skb)->seq = seq;
    TCP_SKB_CB(skb)->end_seq = seq + inject_len;
    TCP_SKB_CB(skb)->ack_seq = tp->snd_una;
    TCP_SKB_CB(skb)->tcp_flags = TCPHDR_ACK;
    skb->ip_summed = CHECKSUM_UNNECESSARY;
    /* tcp_try_rmem_schedule() first checks the socket's own receive buffer,
     * then reserves global memory.  Do the same: a repair that would exceed
     * sk_rcvbuf is dropped and TCP retransmission remains the fallback. */
    if (atomic_read(&sk->sk_rmem_alloc) + skb->truesize > sk->sk_rcvbuf ||
        !sk_rmem_schedule(sk, skb, skb->truesize))
    {
        kfree_skb(skb);
        return -ENOMEM;
    }
    __skb_queue_tail(&sk->sk_receive_queue, skb);
    skb_set_owner_r(skb, sk);
    tp->bytes_received += inject_len;
    WRITE_ONCE(tp->rcv_nxt, seq + inject_len);

    /* Merge every out-of-order segment that now starts at rcv_nxt.  The ooo
     * skb already owns its rmem; moving it must not charge again, so this
     * uses tcp_add_receive_queue(), not tcp_queue_rcv(). */
    for (p = rb_first(&tp->out_of_order_queue); p; p = rb_first(&tp->out_of_order_queue))
    {
        next = rb_to_skb(p);
        end = TCP_SKB_CB(next)->end_seq;
        if (TCP_SKB_CB(next)->seq != tp->rcv_nxt)
            break;
        if (TCP_SKB_CB(next)->tcp_flags & TCPHDR_FIN)
            break;
        rb_erase(&next->rbnode, &tp->out_of_order_queue);
        if (tp->ooo_last_skb == next)
            tp->ooo_last_skb = rb_entry_safe(rb_last(&tp->out_of_order_queue),
                                             struct sk_buff, rbnode);
        __skb_queue_tail(&sk->sk_receive_queue, next);
        tp->bytes_received += end - tp->rcv_nxt;
        WRITE_ONCE(tp->rcv_nxt, end);
    }

    if (tp->rx_opt.num_sacks)
        tbt_rcvq_sack_remove(tp);
    tp->rcv_wup = tp->rcv_nxt;
    tbt_rcvq_send_ack(sk, tp->rcv_nxt);
    atomic64_inc(&c->sess->car_rcvq_injected_segs);
    atomic64_add(inject_len, &c->sess->car_rcvq_injected_bytes);
    return ret;
}

void tbt_rcvq_apply_pending(struct tbt_carrier *c, struct sock *sk)
{
    struct tbt_rcvq_repair *r;
    unsigned long flags;

    if (!c || !sk)
        return;
    for (;;)
    {
        spin_lock_irqsave(&c->rcvq_lock, flags);
        if (list_empty(&c->rcvq_q))
        {
            spin_unlock_irqrestore(&c->rcvq_lock, flags);
            break;
        }
        r = list_first_entry(&c->rcvq_q, struct tbt_rcvq_repair, node);
        list_del_init(&r->node);
        c->rcvq_queued--;
        spin_unlock_irqrestore(&c->rcvq_lock, flags);
        if (tbt_car_rcvq_apply(c, sk, r->wire, r->wire_len, r->delta,
                               r->inject_len))
            atomic64_inc(&c->sess->car_rcvq_skipped);
        tbt_rcvq_free(r);
    }
}
