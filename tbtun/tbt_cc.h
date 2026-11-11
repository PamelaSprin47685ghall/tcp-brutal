/* tbt_cc.h - loss-tolerant carrier congestion control: shared pure logic.
 *
 * tbtun registers this congestion control as "tbt_cc" and puts it on every
 * carrier socket. It replaces BBR on the carriers because the tunnel already
 * repairs loss at the FEC layer: a SACK loss must not halve the TCP window,
 * and the pacing rate must account for the loss the peer reported in FEC_FBK.
 *
 * The controller keeps the largest delivery rate of the last ten RTTs and
 * paces at
 *
 *     gain * max_bw / (1 - p),  clamped to [floor, tunnel_rate]
 *
 * with p the peer's loss estimate. The window is kept at twice the BDP, so a
 * repaired (but not retransmitted) segment never starves the ACK clock.
 *
 * This header is compiled by the module and by host/test_cc_shell.c. It uses
 * tbt_uapi.h types only: no kernel header, no state, no allocation. The
 * kernel half (ops, skb mutation, FEC replay) lives in tbt_cc.c.
 */
#ifndef _TBT_CC_H
#define _TBT_CC_H

#include "tbt_uapi.h"

/* The carrier asks for this name first and falls back to the stock BBR. */
#define TBT_CC_NAME "tbt_cc"
#define TBT_CC_FALLBACK "bbr"

/* One delivery-rate sample per RTT; the controller takes the maximum of the
 * last ten, the BBR window without BBR's phase machinery. */
#define TBT_CC_BW_WIN 10

/* Downlink shell retransmission tracks this many mutated TCP holes. A slot is
 * cleared when snd_una passes it or a DSACK names it; 64 slots cover many
 * RTTs of holes at the per-ACK mutation bound below. */
#define TBT_CC_MUT_SLOTS 64

/* Pacing gain above the measured rate. The (1 - p) divisor already pays for
 * the loss; a gain above 1 adds queue without raising goodput on a metered
 * link, so the default is exactly 1. */
#define TBT_CC_PACING_GAIN_PPM 1000000u

/* A silent carrier still gets this much pacing; below it the socket cannot
 * keep one 1 MiB record moving at a 1 s RTO. */
#define TBT_CC_PACE_FLOOR_BPS 1000000ULL

/* Never let the window fall below this many MSS: a repaired segment must not
 * collapse the carrier while the next ACK is in flight. */
#define TBT_CC_MIN_CWND_PKTS 16u

/* p is an estimate, not a guarantee. Above 90% loss the divisor would demand
 * a tenfold rate that no path can carry, so cap the compensation. */
#define TBT_CC_MAX_LOSS_PPM 900000u

/* Mutate at most this many holes per ACK: the ACK path runs in softirq and
 * must stay bounded even when a whole burst was lost. */
#define TBT_CC_SHELL_PER_ACK 4

/* One record written into a TCP hole. The ring is the only proof that the
 * hole carries a replacement record: skb->cb belongs to TCP and must never be
 * touched. `len` is the TCP payload length of the mutated segment. */
struct tbt_cc_mut
{
    u32 tcp_seq; /* TCP_SKB_CB(skb)->seq of the hole */
    u64 block_id;
    u16 group;
    u16 share;
    u16 len;
    u8 valid;
    /* A DSACK named this slot but the replay queue refused it (ENOMEM or a
     * full queue). snd_una must not retire the slot: the eviction path is the
     * retry, and losing it would strand the replacement record. */
    u8 pending;
};

/* Per-socket controller state. It lives in struct tbt_carrier so the ACK path
 * reads it without an RCU dereference; only this socket's ACK path writes it. */
struct tbt_cc_state
{
    u64 bw_win[TBT_CC_BW_WIN]; /* delivery rate per RTT, bytes/s */
    u64 max_bw;                /* maximum of the live window */
    u32 bw_next;
    u32 bw_filled;
    u32 srtt_us;               /* last SRTT seen by cong_control */
    u32 p_ppm;                 /* last loss estimate used */
};

/* Delivery rate of one sample: delivered packets x MSS over the interval.
 * rate_sample.delivered counts packets, not bytes. */
static inline u64 tbt_cc_bw_from_sample(int delivered, u32 mss, long interval_us)
{
    if (delivered <= 0 || interval_us <= 0 || !mss)
        return 0;
    return (u64)delivered * mss * 1000000ULL / (u64)interval_us;
}

/* Fold one ACK sample into the ten-RTT window. An app-limited sample says
 * nothing about the path and is skipped, and a sample shorter than the SRTT
 * belongs to the current round, so at most one slot advances per RTT. */
static inline void tbt_cc_bw_note(struct tbt_cc_state *st, int delivered,
                                  u32 mss, long interval_us, bool app_limited)
{
    u64 rate;
    u32 i;

    if (app_limited || delivered <= 0 || interval_us <= 0)
        return;
    if (st->srtt_us && (u64)interval_us < (u64)st->srtt_us)
        return;
    rate = tbt_cc_bw_from_sample(delivered, mss, interval_us);
    if (!rate)
        return;
    st->bw_win[st->bw_next] = rate;
    st->bw_next = (st->bw_next + 1) % TBT_CC_BW_WIN;
    if (st->bw_filled < TBT_CC_BW_WIN)
        st->bw_filled++;
    st->max_bw = 0;
    for (i = 0; i < st->bw_filled; i++)
    {
        if (st->bw_win[i] > st->max_bw)
            st->max_bw = st->bw_win[i];
    }
}

/* gain * max_bw / (1 - p), clamped. A zero max_bw means "no sample yet": the
 * caller must leave the socket alone instead of pacing at the floor. */
static inline u64 tbt_cc_pacing_bps(u64 max_bw, u32 p_ppm, u32 gain_ppm,
                                    u64 floor_bps, u64 cap_bps)
{
    u64 rate;
    u32 denom;

    if (!max_bw)
        return 0;
    if (p_ppm > TBT_CC_MAX_LOSS_PPM)
        p_ppm = TBT_CC_MAX_LOSS_PPM;
    denom = 1000000u - p_ppm;
    rate = max_bw * gain_ppm / 1000000u;
    if (denom)
        rate = rate * 1000000u / denom;
    if (rate < floor_bps)
        rate = floor_bps;
    if (cap_bps && rate > cap_bps)
        rate = cap_bps;
    return rate;
}

/* max(2 * BDP, min_cwnd) in packets, bounded by snd_cwnd_clamp. */
static inline u32 tbt_cc_cwnd_pkts(u64 pacing_bps, u32 srtt_us, u32 mss,
                                   u32 min_pkts, u32 clamp)
{
    u64 bdp;
    u64 pkts;

    if (!mss)
        return min_pkts ? min_pkts : 1;
    bdp = pacing_bps * (u64)srtt_us / 1000000ULL;
    pkts = bdp * 2 / mss;
    if (pkts < min_pkts)
        pkts = min_pkts;
    if (clamp && pkts > clamp)
        pkts = clamp;
    if (pkts > 0xffffffffULL)
        pkts = 0xffffffffULL;
    return (u32)pkts;
}

/* Serial-number comparison: true when a precedes b in the u32 sequence
 * space. Both ends of a window are less than 2^31 apart. */
static inline bool tbt_cc_seq_before(u32 a, u32 b)
{
    return (int)(a - b) < 0;
}

/* Do [seq, seq + len) and [start, end) share a byte? */
static inline bool tbt_cc_mut_overlaps(u32 seq, u16 len, u32 start, u32 end)
{
    if (!len || !end || !tbt_cc_seq_before(start, end))
        return false;
    return tbt_cc_seq_before(seq, end) &&
           tbt_cc_seq_before(start, (u32)(seq + len));
}

/* The ring slot that already covers this hole, or NULL. A hole that already
 * carries a replacement record must not be mutated a second time: the first
 * record has not reached the wire yet. */
static inline struct tbt_cc_mut *tbt_cc_mut_find(struct tbt_cc_mut *ring,
                                                 u32 seq, u16 len)
{
    u32 i;

    for (i = 0; i < TBT_CC_MUT_SLOTS; i++)
    {
        struct tbt_cc_mut *m = &ring[i];

        if (m->valid && tbt_cc_mut_overlaps(m->tcp_seq, m->len, seq,
                                            (u32)(seq + len)))
            return m;
    }
    return NULL;
}

/* The slot a DSACK block names, or NULL. A hit means the peer discarded the
 * segment we filled: the replacement record must be repaired through FEC. */
static inline struct tbt_cc_mut *tbt_cc_mut_dsack(struct tbt_cc_mut *ring,
                                                  u32 start, u32 end)
{
    u32 i;

    for (i = 0; i < TBT_CC_MUT_SLOTS; i++)
    {
        struct tbt_cc_mut *m = &ring[i];

        if (m->valid && tbt_cc_mut_overlaps(m->tcp_seq, m->len, start, end))
            return m;
    }
    return NULL;
}

/* Forget the slots snd_una has passed: their replacement records are now in
 * the peer's byte stream and need no rescue. */
static inline void tbt_cc_mut_ack(struct tbt_cc_mut *ring, u32 snd_una)
{
    u32 i;

    for (i = 0; i < TBT_CC_MUT_SLOTS; i++)
    {
        struct tbt_cc_mut *m = &ring[i];

        if (m->valid && !m->pending &&
            !tbt_cc_seq_before(snd_una, (u32)(m->tcp_seq + m->len)))
            m->valid = 0;
    }
}

/* Publish one mutated hole and return the slot it displaced (the caller
 * repairs that record if the slot was live). */
static inline struct tbt_cc_mut *tbt_cc_mut_put(struct tbt_cc_mut *ring,
                                                u32 *next, u32 tcp_seq,
                                                u64 block_id, u16 group,
                                                u16 share, u16 len)
{
    struct tbt_cc_mut *m = &ring[*next];

    m->tcp_seq = tcp_seq;
    m->block_id = block_id;
    m->group = group;
    m->share = share;
    m->len = len;
    m->valid = 1;
    m->pending = 0;
    *next = (*next + 1) % TBT_CC_MUT_SLOTS;
    return m;
}

/* An skb may be filled in place only when its data buffer is private and
 * linear, and the hole does not already carry a record. `already` is the
 * ring lookup: mutating a hole twice would strand the first record before it
 * is sent. The framed wire length is checked against the FIFO head by
 * tbt_fec_out_claim_shell(); this predicate states the whole rule for the
 * host test. */
static inline bool tbt_cc_shell_usable(bool cloned, bool linear, u32 skb_len,
                                       bool already)
{
    return !cloned && linear && !already && skb_len;
}

static inline bool tbt_cc_shell_eligible(bool cloned, bool linear, u32 skb_len,
                                         u32 wire_len, bool already)
{
    return tbt_cc_shell_usable(cloned, linear, skb_len, already) &&
           skb_len == wire_len;
}

/* Decimal hex digits of a chunk size line: lowercase, no leading zeros. */
static inline u16 tbt_cc_hex_digits(u32 v)
{
    u16 n = 1;

    while (v >= 16)
    {
        v >>= 4;
        n++;
    }
    return n;
}

/* Bytes before the record inside one carrier write. Raw records have no
 * framing; an HTTP chunk is "<hex size>\r\n" and a trailing CRLF. */
static inline u16 tbt_cc_wire_hdr_len(u16 rec_len, bool http)
{
    return http ? (u16)(tbt_cc_hex_digits(rec_len) + 2) : 0;
}

static inline u16 tbt_cc_wire_len(u16 rec_len, bool http)
{
    if (!http)
        return rec_len;
    return (u16)(tbt_cc_wire_hdr_len(rec_len, http) + rec_len + 2);
}

#ifdef __KERNEL__
int tbt_cc_register(void);
void tbt_cc_unregister(void);
#endif

#endif /* _TBT_CC_H */
