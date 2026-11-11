/* tbt_ul_pin.c - the exact chunk rebuild and the in-place hole fill of
 * 0.8.0 Step 3.
 *
 * The rebuild half is pure C: it takes the share the codec recovered from
 * parity and writes the bytes the near side put on that carrier. The kernel
 * half hands the missing slice to the carrier socket's own input path, so
 * TCP trims duplicates, merges the out-of-order tree, advances rcv_nxt, and
 * sends the cumulative ACK through the code it always uses.
 */
#ifdef __KERNEL__
#include <linux/skbuff.h>
#include <net/tcp.h>

#include "tbtun.h"
#else
#include <errno.h>
#include <string.h>
#endif

#include "tbt_ul_pin.h"
#include "tbt_http.h"

int tbt_ul_pin_build(struct tbt_crypto_ctx *rx, bool crypto,
                     const struct tbt_ul_pin_desc *d, const u8 *payload,
                     u8 *out, size_t cap, u32 *out_len)
{
    struct tbt_fec_hdr h;
    size_t hdr_len, body_len, tail;

    if (!d || !out || !out_len || (!payload && d->payload_len))
        return -EINVAL;
    if (!d->n || !d->m || d->share >= d->n || !d->payload_len ||
        d->payload_len > TBT_FEC_PAYLOAD_MAX)
        return -EINVAL;
    *out_len = 0;
    body_len = (size_t)TBT_FEC_HDR_LEN + d->payload_len;
    hdr_len = tbt_http_chunk_hdr((char *)out, cap, body_len);
    if (!hdr_len || cap < hdr_len + body_len + 2)
        return -ENOSPC;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = d->flags;
    h.share_index = d->share;
    h.n = (u8)d->n;
    h.m = (u8)d->m;
    h.block_id = d->block_id;
    h.seq = d->seq;
    h.payload_len = d->payload_len;
    h.conv = TBT_FEC_CONV;
    h.reserved = d->group;
    h.sid = d->sid;
    /* Header and payload CRCs cover the clear bytes, so seal before the body
     * transform. That is the sender's order as well. */
    if (tbt_fec_hdr_seal(&h, payload))
        return -EINVAL;
    if (tbt_fec_hdr_pack(&h, out + hdr_len))
        return -EINVAL;
    memcpy(out + hdr_len + TBT_FEC_HDR_LEN, payload, d->payload_len);
    /* The RX context's keystream is the sender's TX keystream. Applying it to
     * the plaintext produces the ciphertext the lost segment carried. */
    if (crypto && rx &&
        tbt_crypto_body_xor_flow(rx, h.sid, h.seq, false, h.share_index,
                                 out + hdr_len + TBT_FEC_HDR_LEN,
                                 h.payload_len))
        return -EINVAL;
    tail = tbt_http_chunk_tail((char *)out + hdr_len + body_len, 2);
    if (!tail)
        return -ENOSPC;
    *out_len = (u32)(hdr_len + body_len + 2);
    return 0;
}

#ifdef __KERNEL__

void tbt_ul_pin_body_start(struct tbt_carrier *c, u32 seq)
{
    /* One writer (this carrier's RX work). The FEC RX work reads the pair
     * with READ_ONCE, so the position is published before the valid flag. */
    WRITE_ONCE(c->rx_body_start_seq, seq);
    WRITE_ONCE(c->rx_body_seq, seq);
    WRITE_ONCE(c->rx_chunk_len, 0);
    WRITE_ONCE(c->rx_body_chunks, 0);
    smp_store_release(&c->rx_body_seq_set, true);
}

void tbt_ul_pin_chunk_seen(struct tbt_carrier *c, u32 wire_len)
{
    if (!READ_ONCE(c->rx_body_seq_set) || !wire_len)
        return;
    /* The first complete chunk fixes the constant every later chunk repeats;
     * the running value stays exact for a short tail chunk as well. */
    if (!READ_ONCE(c->rx_chunk_len))
        WRITE_ONCE(c->rx_chunk_len, wire_len);
    WRITE_ONCE(c->rx_body_seq, READ_ONCE(c->rx_body_seq) + wire_len);
    WRITE_ONCE(c->rx_body_chunks, READ_ONCE(c->rx_body_chunks) + 1);
}

void tbt_ul_pin_body_end(struct tbt_carrier *c)
{
    /* A new HTTP message starts with a new header block: the old body origin
     * names bytes that no longer belong to this stream. */
    smp_store_release(&c->rx_body_seq_set, false);
}

/* Hand the missing slice to the socket's own input path.
 *
 * tcp_rcv_established() takes its slow path (the 24-byte header keeps header
 * prediction off), validates the segment against rcv_nxt, then
 * tcp_data_queue() queues it, trims what the socket already has, merges the
 * out-of-order tree, and schedules an immediate ACK when that tree empties.
 * release_sock() runs tcp_release_cb(), which sends the ACK. */
static int tbt_ul_pin_inject(struct tbt_carrier *c, u32 chunk_seq,
                             const u8 *chunk, u32 chunk_len)
{
    struct sock *sk = tbt_carrier_sk_get(c);
    struct sk_buff *skb;
    struct tcp_sock *tp;
    struct tcphdr *th;
    u32 rcv_nxt, end, slice, off;
    int err = -EAGAIN;

    if (!sk)
        return -ENOTCONN;
    skb = alloc_skb(TBT_UL_PIN_TCP_HDR + chunk_len, GFP_KERNEL);
    if (!skb)
    {
        tbt_carrier_sk_put(sk);
        return -ENOMEM;
    }
    lock_sock(sk);
    if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
        goto out;
    tp = tcp_sk(sk);
    rcv_nxt = tp->rcv_nxt;
    if (!tbt_ul_pin_straddles(rcv_nxt, chunk_seq, chunk_len))
    {
        atomic64_inc(&c->sess->ul_pin_skip_window);
        goto out;
    }
    /* The peer must already be past the hole. An empty out-of-order tree
     * means snd_nxt may still sit at rcv_nxt, and an ACK for bytes the peer
     * never sent is the one answer TCP must never give. */
    if (RB_EMPTY_ROOT(&tp->out_of_order_queue))
    {
        atomic64_inc(&c->sess->ul_pin_skip_no_ofo);
        goto out;
    }
    {
        struct sk_buff *ofo =
            rb_entry(rb_first(&tp->out_of_order_queue), struct sk_buff, rbnode);

        end = tbt_ul_pin_slice_end(chunk_seq, chunk_len,
                                   TCP_SKB_CB(ofo)->seq);
    }
    slice = end - rcv_nxt;
    off = rcv_nxt - chunk_seq;
    if (!slice || off + slice > chunk_len)
    {
        atomic64_inc(&c->sess->ul_pin_skip_window);
        goto out;
    }

    th = skb_put(skb, TBT_UL_PIN_TCP_HDR);
    memset(th, 0, TBT_UL_PIN_TCP_HDR);
    th->doff = TBT_UL_PIN_TCP_HDR / 4;
    th->ack = 1;
    th->seq = htonl(rcv_nxt);
    /* An old ACK: tcp_ack() then leaves snd_una and the congestion state
     * alone, exactly like a data segment whose ACK was overtaken. */
    th->ack_seq = htonl(tp->snd_una);
    /* Echo the window the peer last advertised. A different value would be a
     * spurious window update from a segment the peer never sent. */
    th->window = htons((u16)min_t(u32, 0xffffu,
                                  tp->snd_wnd >> tp->rx_opt.snd_wscale));
    memcpy(skb_put(skb, slice), chunk + off, slice);
    skb_reset_transport_header(skb);
    skb->ip_summed = CHECKSUM_UNNECESSARY;
    skb->dev = NULL;
    memset(TCP_SKB_CB(skb), 0, sizeof(*TCP_SKB_CB(skb)));
    TCP_SKB_CB(skb)->seq = rcv_nxt;
    TCP_SKB_CB(skb)->end_seq = rcv_nxt + slice;
    TCP_SKB_CB(skb)->ack_seq = tp->snd_una;
    TCP_SKB_CB(skb)->tcp_flags = TCPHDR_ACK;

    sk->sk_backlog_rcv(sk, skb);
    skb = NULL;
    err = 0;
out:
    release_sock(sk);
    kfree_skb(skb);
    tbt_carrier_sk_put(sk);
    return err;
}

int tbt_ul_pin_repair(struct tbt_session *sess, struct tbt_fec_flow *f,
                      u16 group, u16 share, u64 block_id, u64 seq0, u16 n,
                      u16 m, u16 real, u16 short_len, const u8 *payload)
{
    struct tbt_fec_rx *rx = f ? f->rx : NULL;
    struct tbt_carrier *c;
    struct tbt_ul_pin_desc d;
    u32 chunk_seq, chunk_len, len = 0;
    u64 seq;
    u16 plen;
    u8 *chunk;
    int err;

    if (!sess || !rx || !sess->fec || !sess->n_car || !n || !m ||
        share >= n || !payload || group >= rx->groups)
        return -EINVAL;
    /* The far side receives the uplink. A raw-profile carrier has no chunk
     * positions to name, and a closed socket has no hole to fill. */
    if (!sess->tunnel || sess->tunnel->cfg.role != TBT_ROLE_FAR)
        return -EOPNOTSUPP;
    if (sess->crypto && !sess->crypto_ready)
        return -EAGAIN;
    seq = tbt_fec_data_seq(seq0, group, n, share);
    /* Neither parity nor the block states the LAST bit, so a replay is only
     * possible once the stream end is known and the flag is exact. Without
     * that proof the segment cannot be promised byte-identical, so it is not
     * replayed at all: the carrier's TCP retransmission owns that recovery.
     * The flag states nothing about the hole's position; a stream whose LAST
     * arrived while an earlier gap is still open takes the normal replay path
     * for every seq below total_pkts. One rx plane serves one stream, so the
     * value cannot belong to a later stream on the same sid. */
    if (!tbt_ul_pin_flags_known(READ_ONCE(rx->total_known),
                                seq, READ_ONCE(rx->total_pkts)))
    {
        atomic64_inc(&sess->ul_pin_skip_model);
        return -EAGAIN;
    }
    /* The carrier is the session's own socket, named by the share's slot.
     * No lookup, no address match, nothing a peer could redirect. */
    c = tbt_carrier_get(sess, (u8)tbt_ul_pin_carrier(rx->group_map[group],
                                                    share, n, m));
    if (!c)
        return -ENODEV;
    if (c->profile == 3 || !atomic_read(&c->connected) ||
        atomic_read(&c->stopping))
    {
        tbt_carrier_put(c);
        return -ENODEV;
    }
    if (!smp_load_acquire(&c->rx_body_seq_set) || !READ_ONCE(c->rx_chunk_len))
    {
        atomic64_inc(&sess->ul_pin_skip_model);
        tbt_carrier_put(c);
        return -EAGAIN;
    }
    /* The b-th chunk on this carrier starts at body_start + b*chunk_len. The
     * running cursor is the authority: when the two disagree a block was
     * skipped on this carrier (an early close), and no byte position is
     * provable, so nothing is replayed. */
    chunk_seq = tbt_ul_pin_chunk_seq(READ_ONCE(c->rx_body_start_seq),
                                     block_id, READ_ONCE(c->rx_chunk_len));
    if (chunk_seq != READ_ONCE(c->rx_body_seq))
    {
        atomic64_inc(&sess->ul_pin_skip_model);
        tbt_carrier_put(c);
        return -EAGAIN;
    }
    plen = tbt_ul_pin_share_len(rx->s, real, share, short_len);
    chunk_len = tbt_ul_pin_chunk_len(plen);
    chunk = kmalloc(chunk_len, GFP_KERNEL);
    if (!chunk)
    {
        tbt_carrier_put(c);
        return -ENOMEM;
    }
    memset(&d, 0, sizeof(d));
    d.sid = rx->sid;
    d.block_id = block_id;
    d.seq = seq;
    d.group = group;
    d.share = share;
    d.n = n;
    d.m = m;
    d.payload_len = plen;
    d.flags = tbt_ul_pin_flags(seq, READ_ONCE(rx->total_pkts));
    err = tbt_ul_pin_build(&sess->crypto_ctx.rx, sess->crypto, &d, payload,
                           chunk, chunk_len, &len);
    if (err)
    {
        kfree(chunk);
        tbt_carrier_put(c);
        return err;
    }
    err = tbt_ul_pin_inject(c, chunk_seq, chunk, len);
    kfree(chunk);
    if (!err)
    {
        atomic64_inc(&sess->ul_pin_inject);
        atomic64_add(len, &sess->ul_pin_bytes);
    }
    tbt_carrier_put(c);
    return err;
}

#endif /* __KERNEL__ */
