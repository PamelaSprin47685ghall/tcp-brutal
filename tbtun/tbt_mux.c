/* tbt_mux.c - tbtun 0.8.0 single-FEC-stream multiplexing.
 *
 * The encoder is a pure function. The parser owns no memory and accepts any
 * push boundary. The flow table is caller-owned and tracks credit per
 * stream_id. Nothing here sleeps, allocates, or prints, so the same object
 * serves the kernel module and the host test.
 */
#include "tbt_mux.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

int tbt_mux_hdr_pack(const struct tbt_mux_hdr *h, u8 out[TBT_MUX_HDR_LEN])
{
    u16 len;
    u32 sid;

    if (!h || !out)
        return -EINVAL;

    len = (u16)h->len;
    sid = (u32)h->stream_id;

    out[0] = h->cmd;
    out[1] = h->slot;
    out[2] = (u8)(len >> 8);
    out[3] = (u8)len;
    out[4] = (u8)(sid >> 24);
    out[5] = (u8)(sid >> 16);
    out[6] = (u8)(sid >> 8);
    out[7] = (u8)sid;
    return 0;
}

int tbt_mux_hdr_unpack(const u8 in[TBT_MUX_HDR_LEN], struct tbt_mux_hdr *h)
{
    if (!in || !h)
        return -EINVAL;

    h->cmd = in[0];
    h->slot = in[1];
    h->len = (__be16)(((u16)in[2] << 8) | (u16)in[3]);
    h->stream_id = (__be32)(((u32)in[4] << 24) | ((u32)in[5] << 16) |
                            ((u32)in[6] << 8) | (u32)in[7]);
    return 0;
}

int tbt_mux_encode(u8 cmd, u8 slot, u32 stream_id, const u8 *payload, u16 len,
                   u8 *out, size_t cap)
{
    struct tbt_mux_hdr h;
    size_t body;
    size_t total;

    switch (cmd) {
    case TBT_MUX_OPEN:
        if (slot > TBT_MUX_SLOT_MAX || len != 0)
            return -EINVAL;
        break;
    case TBT_MUX_DATA:
        if (slot != 0 || len == 0 || len > TBT_MUX_PAYLOAD_MAX || !payload)
            return -EINVAL;
        break;
    case TBT_MUX_FIN:
    case TBT_MUX_RST:
        if (slot != 0 || len != 0)
            return -EINVAL;
        break;
    case TBT_MUX_WIN:
        /* len is a u16 credit, so it cannot exceed TBT_MUX_WINDOW_MAX. */
        if (slot != 0)
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }

    body = cmd == TBT_MUX_DATA ? len : 0;
    total = (size_t)TBT_MUX_HDR_LEN + body;
    if (!out)
        return -EINVAL;
    if (cap < total)
        return -ENOSPC;

    h.cmd = cmd;
    h.slot = slot;
    h.len = (__be16)len;
    h.stream_id = (__be32)stream_id;
    if (tbt_mux_hdr_pack(&h, out) < 0)
        return -EINVAL;
    if (body)
        memcpy(out + TBT_MUX_HDR_LEN, payload, body);
    return (int)total;
}

/* Check one parsed header against the command rules and report the number of
 * body bytes that follow it. */
static int tbt_mux_hdr_check(const struct tbt_mux_hdr *h, u16 *body_len,
                             const u8 *body, u16 body_cap)
{
    u16 len = (u16)h->len;

    switch (h->cmd) {
    case TBT_MUX_OPEN:
        if (h->slot > TBT_MUX_SLOT_MAX || len != 0)
            return -EPROTO;
        *body_len = 0;
        return 0;
    case TBT_MUX_DATA:
        if (h->slot != 0 || len == 0 || len > TBT_MUX_PAYLOAD_MAX)
            return -EPROTO;
        if (len > body_cap || !body)
            return -EPROTO;
        *body_len = len;
        return 0;
    case TBT_MUX_FIN:
    case TBT_MUX_RST:
        if (h->slot != 0 || len != 0)
            return -EPROTO;
        *body_len = 0;
        return 0;
    case TBT_MUX_WIN:
        if (h->slot != 0)
            return -EPROTO;
        *body_len = 0;
        return 0;
    default:
        return -EPROTO;
    }
}

void tbt_mux_parser_init(struct tbt_mux_parser *p, u8 *body, u16 cap)
{
    if (!p)
        return;

    memset(p->hdr, 0, sizeof(p->hdr));
    p->hdr_have = 0;
    p->body_len = 0;
    p->body_have = 0;
    p->body = body;
    p->body_cap = cap;
}

size_t tbt_mux_parser_push(struct tbt_mux_parser *p, const u8 *buf, size_t len,
                           tbt_mux_frame_cb cb, void *ctx, int *err)
{
    size_t used = 0;

    if (!p || !err)
        return 0;
    if (*err != 0)
        return 0;
    if (!buf && len != 0) {
        *err = -EINVAL;
        return 0;
    }

    while (used < len) {
        if (p->hdr_have < TBT_MUX_HDR_LEN) {
            struct tbt_mux_hdr h;
            size_t take = TBT_MUX_HDR_LEN - p->hdr_have;
            u16 body_len = 0;
            int rc;

            if (take > len - used)
                take = len - used;
            memcpy(p->hdr + p->hdr_have, buf + used, take);
            p->hdr_have += (u8)take;
            used += take;
            if (p->hdr_have < TBT_MUX_HDR_LEN)
                break;

            if (tbt_mux_hdr_unpack(p->hdr, &h) < 0 ||
                tbt_mux_hdr_check(&h, &body_len, p->body, p->body_cap) < 0) {
                /* Drop the rejected header. A caller that clears *err starts
                 * at the next byte and never re-delivers the bad frame. */
                p->hdr_have = 0;
                p->body_len = 0;
                p->body_have = 0;
                *err = -EPROTO;
                break;
            }

            p->body_len = body_len;
            p->body_have = 0;
            if (body_len != 0)
                continue;

            rc = cb ? cb(ctx, &h, NULL) : 0;
            p->hdr_have = 0;
            if (rc != 0) {
                *err = rc;
                break;
            }
        } else {
            struct tbt_mux_hdr h;
            size_t take = p->body_len - p->body_have;
            int rc;

            if (take > len - used)
                take = len - used;
            memcpy(p->body + p->body_have, buf + used, take);
            p->body_have += (u16)take;
            used += take;
            if (p->body_have < p->body_len)
                break;

            if (tbt_mux_hdr_unpack(p->hdr, &h) < 0) {
                *err = -EPROTO;
                break;
            }
            rc = cb ? cb(ctx, &h, p->body) : 0;
            p->hdr_have = 0;
            p->body_len = 0;
            p->body_have = 0;
            if (rc != 0) {
                *err = rc;
                break;
            }
        }
    }

    return used;
}

void tbt_mux_flows_init(struct tbt_mux_flows *f, u32 rx_window)
{
    if (!f)
        return;

    memset(f->s, 0, sizeof(f->s));
    f->count = 0;
    f->rx_window = rx_window ? rx_window : TBT_MUX_WINDOW_MAX;
}

struct tbt_mux_stream *tbt_mux_flow_find(struct tbt_mux_flows *f, u32 id)
{
    unsigned int i;

    if (!f)
        return NULL;

    for (i = 0; i < TBT_MUX_STREAMS; i++) {
        if (f->s[i].state != TBT_MUX_FREE && f->s[i].stream_id == id)
            return &f->s[i];
    }
    return NULL;
}

struct tbt_mux_stream *tbt_mux_flow_open(struct tbt_mux_flows *f, u32 id,
                                         u8 slot)
{
    unsigned int i;

    if (!f || slot > TBT_MUX_SLOT_MAX)
        return NULL;
    if (tbt_mux_flow_find(f, id))
        return NULL;

    for (i = 0; i < TBT_MUX_STREAMS; i++) {
        if (f->s[i].state != TBT_MUX_FREE)
            continue;
        f->s[i].stream_id = id;
        f->s[i].state = TBT_MUX_OPEN;
        f->s[i].slot = slot;
        f->s[i].tx_credit = 0;
        f->s[i].rx_unacked = 0;
        f->count++;
        return &f->s[i];
    }
    return NULL;
}

int tbt_mux_flow_close(struct tbt_mux_flows *f, u32 id)
{
    struct tbt_mux_stream *s = tbt_mux_flow_find(f, id);

    if (!s)
        return -ENOENT;
    memset(s, 0, sizeof(*s));
    if (f->count)
        f->count--;
    return 0;
}

int tbt_mux_tx_charge(struct tbt_mux_flows *f, u32 id, u16 len)
{
    struct tbt_mux_stream *s = tbt_mux_flow_find(f, id);

    if (!s)
        return -ENOENT;
    if (len > s->tx_credit)
        return -EAGAIN;
    s->tx_credit -= len;
    return 0;
}

int tbt_mux_tx_credit(struct tbt_mux_flows *f, u32 id, u16 credit)
{
    struct tbt_mux_stream *s = tbt_mux_flow_find(f, id);

    if (!s)
        return -ENOENT;
    if (s->tx_credit > 0xffffffffu - credit)
        s->tx_credit = 0xffffffffu;
    else
        s->tx_credit += credit;
    return 0;
}

int tbt_mux_rx_charge(struct tbt_mux_flows *f, u32 id, u16 len, u16 *grant)
{
    struct tbt_mux_stream *s = tbt_mux_flow_find(f, id);

    if (!s)
        return -ENOENT;
    if (!grant)
        return -EINVAL;

    *grant = 0;
    if (s->rx_unacked > 0xffffffffu - len)
        s->rx_unacked = 0xffffffffu;
    else
        s->rx_unacked += len;

    if (s->rx_unacked <= f->rx_window / 2)
        return 0;

    *grant = s->rx_unacked > TBT_MUX_WINDOW_MAX ? TBT_MUX_WINDOW_MAX
                                                : (u16)s->rx_unacked;
    s->rx_unacked -= *grant;
    return 1;
}
