/* tbt_rcvq.h - repair a carrier's TCP receive queue with FEC-recovered bytes.
 *
 * A carrier's peer->module byte stream is a sequence of records.  When one
 * record is lost, the bytes after it sit in tp->out_of_order_queue and
 * tcp_read_sock() stops at the hole.  FEC recovers the lost share; this unit
 * rebuilds its exact wire bytes, maps the share to the carrier that wrote it,
 * and fills [rcv_nxt, ooo_first_seq) under the socket lock.  The kernel then
 * treats the later original segment as old data (TCP_OLD_DATA / D-SACK).
 *
 * Pure functions (wire rebuild, carrier pick, plan) compile on the host.
 * The apply/enqueue half is kernel-only.
 */
#ifndef _TBT_RCVQ_H
#define _TBT_RCVQ_H

#include "tbt_uapi.h"
#include "tbt_fec.h"
#include "tbt_crypto.h"

/* src_carrier[] uses this for a share that has not arrived. */
#define TBT_RCVQ_NO_CARRIER 0xffu

/* Carrier profile numbers from tbt_parse.c: 1 http-kal, 2 http-kal-drip,
 * 3 raw.  Both HTTP profiles wrap a record in one chunk; raw does not. */
#define TBT_RCVQ_PROFILE_HTTP 1u
#define TBT_RCVQ_PROFILE_RAW  3u

/* One queued repair per carrier.  The wire buffer is variable length. */
#define TBT_RCVQ_MAX_PENDING 8u

enum tbt_rcvq_skip
{
    TBT_RCVQ_OK = 0,
    TBT_RCVQ_SKIP_NOHOLE,   /* no out_of_order_queue, or gap == 0 */
    TBT_RCVQ_SKIP_OVERLAP,  /* record does not align with the hole */
    TBT_RCVQ_SKIP_DELTA,    /* record starts after the hole: wait for earlier */
    TBT_RCVQ_SKIP_CARRIER,  /* wrong carrier or stale generation */
    TBT_RCVQ_SKIP_RMEM,     /* receive-buffer reservation failed */
};

struct tbt_rcvq_plan_in
{
    u32 rcv_nxt;      /* tp->rcv_nxt, read under lock_sock */
    u32 ooo_seq;      /* first skb seq in out_of_order_queue, or rcv_nxt */
    u32 record_seq;   /* TCP seq of wire[0]; valid only when has_record_seq */
    u16 wire_len;     /* full record wire length */
    u8 carrier;       /* carrier the record was originally written on */
    u8 expect_carrier;/* carrier whose hole we are filling */
    u8 gen_ok;        /* carrier generation still matches */
    u8 has_record_seq;/* record_seq is known (FEC_MAP) */
};

struct tbt_rcvq_plan
{
    u16 delta;        /* first byte of wire[] that fills the hole */
    u16 inject_len;   /* bytes to inject; <= wire_len - delta */
    enum tbt_rcvq_skip skip;
};

/* Rebuild the exact wire bytes of one FEC DATA (or PARITY) share.
 *
 * h          header fields; h->payload_len bytes of plaintext follow.
 * plain      recovered plaintext payload (NULL only when payload_len == 0).
 * rx_crypto  this direction's RX context, or NULL when crypto is off.
 * profile    1/2 = HTTP chunk, 3 = raw record.
 * out/cap    destination and capacity; cap must hold the whole wire.
 * out_len    receives the wire length.
 *
 * The output is byte-identical to tbt_fec_make_record() followed by
 * tbt_http_fec_chunk_encode() on the sender: clear 48-byte header, ChaCha8
 * ciphertext, and the canonical lowercase chunk framing.  Returns 0, -EINVAL,
 * or -ENOSPC.
 */
int tbt_rcvq_rebuild_wire(const struct tbt_fec_hdr *h, const u8 *plain,
                          struct tbt_crypto_ctx *rx_crypto, u8 profile,
                          u8 *out, size_t cap, u16 *out_len);

/* Pick the one carrier in [base, base + span) that src_carrier[0..n) does not
 * name.  A present carrier outside the range makes the mapping inconsistent
 * (-EINVAL); zero or more than one missing carrier returns -ENOENT.  This is
 * the single-missing set-difference path; a FEC_MAP can override it later. */
int tbt_rcvq_pick_carrier(const u8 *src_carrier, u16 n, u16 base, u16 span,
                          u8 *carrier);

/* Pure plan: map a recovered record to the measured hole.  A record whose
 * start is unknown is accepted only when the hole is exactly one whole
 * record; a record with a known start accepts the MSS subrange case. */
enum tbt_rcvq_skip tbt_rcvq_plan(const struct tbt_rcvq_plan_in *in,
                                 struct tbt_rcvq_plan *out);

#ifdef __KERNEL__
struct tbt_carrier;
struct sock;

/* One rebuilt wire record waiting for c->w_rx.  The FEC decoder allocates it,
 * rebuilds into wire[], then enqueues it; the carrier RX work owns it until
 * tbt_car_rcvq_apply() returns. */
struct tbt_rcvq_repair
{
    struct list_head node;
    u16 wire_len;
    u16 delta;
    u16 inject_len;
    u8 wire[];
};

struct tbt_rcvq_repair *tbt_rcvq_alloc(u16 wire_len);
void tbt_rcvq_free(struct tbt_rcvq_repair *r);

/* Queue one rebuilt wire record for c->w_rx.  Takes ownership on success and
 * on failure.  Returns 0 or -ENOSPC/-ENOMEM. */
int tbt_rcvq_enqueue(struct tbt_carrier *c, struct tbt_rcvq_repair *r);

/* Called by tbt_car_rx_body() under lock_sock(sk), before tcp_read_sock(). */
void tbt_rcvq_apply_pending(struct tbt_carrier *c, struct sock *sk);

/* Fill the TCP hole with wire[delta .. delta + inject_len).  The caller holds
 * lock_sock(sk) and the plan was built for this socket.  Returns 0, or a
 * negative skip/error.  Only public tcp_sock fields and exported helpers are
 * used: no tcp_send_rcvq(), no tcp_rcv_established(), no kprobe. */
int tbt_car_rcvq_apply(struct tbt_carrier *c, struct sock *sk,
                       const u8 *wire, u16 wire_len, u16 delta,
                       u16 inject_len);
#endif

#endif /* _TBT_RCVQ_H */
