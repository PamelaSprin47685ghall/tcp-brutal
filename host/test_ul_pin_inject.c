/*
 * test_ul_pin_inject.c - deterministic uplink carrier pinning and the exact
 * segment replay of 0.8.0 Step 3.
 *
 * Three contracts, all host-testable because the module and this test compile
 * the same code:
 *
 *   1. Mapping. A DATA share (share < n) owns carrier group*(n+m)+share and
 *      no other slot; a control record can only be placed on a parity slot.
 *   2. Position. Every full DATA chunk on one carrier has the same wire
 *      length, so the b-th chunk starts at body_start + b*chunk_len, including
 *      across the u32 wrap.
 *   3. Identity. The rebuilt chunk is byte-for-byte the segment the sender
 *      wrote. The sender side here is the module's own path: seal, pack, TX
 *      keystream, then tbt_fec_batch_build() for the HTTP framing. The rebuild
 *      must match it, verify as a record, and decode back to the plaintext.
 *
 * Build: make host-test.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_fec_batch.h"
#include "tbt_http.h"
#include "tbt_ul_pin.h"

static int failures;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            failures++;                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static u8 payload[TBT_FEC_S];
static u8 wire[4 * TBT_FEC_REC_MAX];
static u8 rebuilt[4 * TBT_FEC_REC_MAX];

/* One wire chunk built exactly as the sender builds it: seal the header over
 * the clear payload, pack it, run the TX keystream over the payload, then let
 * the batch builder add the chunk size line and the CRLF. */
static size_t tx_chunk(u8 *out, size_t cap, struct tbt_crypto_ctx *tx,
                       const struct tbt_ul_pin_desc *d, const u8 *pl)
{
    struct tbt_fec_batch_seg seg[TBT_BATCH_SEG_MAX];
    char scratch[TBT_BATCH_REC_MAX * TBT_HTTP_CHUNK_HDR_MAX];
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_REC_MAX];
    const u8 *recs[1];
    u16 lens[1];
    size_t total = 0, off = 0;
    unsigned int nseg, i;

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
    if (tbt_fec_hdr_seal(&h, pl))
        return 0;
    if (tbt_fec_hdr_pack(&h, rec))
        return 0;
    memcpy(rec + TBT_FEC_HDR_LEN, pl, d->payload_len);
    if (tx && tx->ready)
    {
        if (tbt_crypto_body_xor_flow(tx, h.sid, h.seq, false, h.share_index,
                                     rec + TBT_FEC_HDR_LEN, h.payload_len))
            return 0;
    }
    recs[0] = rec;
    lens[0] = (u16)(TBT_FEC_HDR_LEN + d->payload_len);
    nseg = tbt_fec_batch_build(seg, scratch, recs, lens, 1, true, &total);
    if (!nseg || total > cap)
        return 0;
    for (i = 0; i < nseg; i++)
    {
        memcpy(out + off, seg[i].base, seg[i].len);
        off += seg[i].len;
    }
    return off;
}

static void test_mapping(void)
{
    static const struct
    {
        u16 n, m, groups;
    } shapes[] = {
        { 1, 1, 1 }, { 4, 4, 1 }, { 8, 8, 2 }, { 8, 24, 1 }, { 8, 8, 16 },
        { 154, 102, 1 },
    };
    size_t si;

    for (si = 0; si < sizeof(shapes) / sizeof(shapes[0]); si++)
    {
        u16 n = shapes[si].n, m = shapes[si].m;
        u16 n_car = (u16)(shapes[si].groups * (n + m));
        u16 g, s, idx;

        for (g = 0; g < shapes[si].groups; g++)
            for (s = 0; s < (u16)(n + m); s++)
            {
                u32 c = tbt_ul_pin_carrier(g, s, n, m);

                CHECK(c == (u32)g * (n + m) + s,
                      "carrier(%u,%u,%u,%u)=%u", g, s, n, m, c);
                CHECK(c < n_car, "carrier %u outside %u carriers", c, n_car);
                CHECK(tbt_ul_pin_slot((u16)c, n, m) == s,
                      "slot(carrier)=%u, want %u", tbt_ul_pin_slot((u16)c, n, m),
                      s);
                CHECK(tbt_ul_pin_is_data((u16)c, n, m) == (s < n),
                      "is_data(carrier(%u,%u)) wrong", g, s);
            }

        /* Every carrier index classifies by its slot, and no rotation can
         * move a control record onto a DATA slot. */
        for (idx = 0; idx < n_car; idx++)
            CHECK(tbt_ul_pin_is_data(idx, n, m) ==
                      (tbt_ul_pin_slot(idx, n, m) < n),
                  "classification idx=%u", idx);

        if (m)
        {
            u8 seen[TBT_FEC_MAX_CARRIERS] = { 0 };
            u16 rot = 0, i, placed = 0;

            for (i = 0; i < 4u * n_car; i++)
            {
                u16 pick = tbt_ul_pin_ctrl_next(n_car, n, m, rot);

                CHECK(pick != TBT_UL_PIN_NONE, "no parity carrier in %u", n_car);
                if (pick == TBT_UL_PIN_NONE)
                    break;
                CHECK(!tbt_ul_pin_is_data(pick, n, m),
                      "control placed on DATA carrier %u", pick);
                seen[pick] = 1;
                rot = (u16)((pick + 1) % n_car);
            }
            for (i = 0; i < n_car; i++)
                if (!tbt_ul_pin_is_data(i, n, m))
                {
                    CHECK(seen[i], "parity carrier %u never scheduled", i);
                    placed++;
                }
            CHECK(placed == (u16)(shapes[si].groups * m),
                  "parity slots=%u, want %u", placed, shapes[si].groups * m);
        }
        else
        {
            CHECK(tbt_ul_pin_ctrl_next(n_car, n, 0, 0) == TBT_UL_PIN_NONE,
                  "m=0 must have no control slot");
        }
    }

    /* A session shorter than one group has no parity slot to offer. */
    CHECK(tbt_ul_pin_ctrl_next(4, 8, 8, 0) == TBT_UL_PIN_NONE,
          "partial group must not invent a parity carrier");
    CHECK(tbt_ul_pin_ctrl_next(0, 8, 8, 0) == TBT_UL_PIN_NONE,
          "no carriers must not invent one");
}

static void test_positions(void)
{
    static const u16 lens[] = { TBT_FEC_S, 1, 1000, TBT_FEC_S - 1 };
    size_t li;

    for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++)
    {
        u32 chunk_len = tbt_ul_pin_chunk_len(lens[li]);
        u32 body_start = 0xfffff000u; /* crosses the u32 wrap below */
        u32 cursor = body_start;
        u64 b;

        for (b = 0; b < 3000; b++)
        {
            CHECK(tbt_ul_pin_chunk_seq(body_start, b, chunk_len) == cursor,
                  "chunk_seq b=%llu start=%08x want %08x", (unsigned long long)b,
                  tbt_ul_pin_chunk_seq(body_start, b, chunk_len), cursor);
            cursor += chunk_len;
        }
    }

    /* The size line the rebuild emits and the arithmetic above must agree on
     * every payload length: 8240 bytes is four hex digits, 48 is two. */
    CHECK(tbt_ul_pin_chunk_len(TBT_FEC_S) == 8248,
          "full chunk len=%u", tbt_ul_pin_chunk_len(TBT_FEC_S));
    CHECK(tbt_ul_pin_chunk_len(0) == 2 + 2 + 48 + 2,
          "empty chunk len=%u", tbt_ul_pin_chunk_len(0));
    CHECK(tbt_ul_pin_chunk_len(16) == 2 + 2 + 64 + 2,
          "16-byte chunk len=%u", tbt_ul_pin_chunk_len(16));
}

static void test_slices(void)
{
    const u32 chunk_seq = 0xfffffff0u; /* wraps inside the chunk */
    const u32 chunk_len = 64;
    u32 rcv, ofo;

    /* Before the chunk: not this segment's hole. */
    CHECK(!tbt_ul_pin_straddles(chunk_seq - 1, chunk_seq, chunk_len),
          "rcv_nxt before the chunk");
    CHECK(tbt_ul_pin_straddles(chunk_seq, chunk_seq, chunk_len),
          "rcv_nxt at the chunk start");
    CHECK(tbt_ul_pin_straddles(chunk_seq + chunk_len - 1, chunk_seq, chunk_len),
          "rcv_nxt at the last byte");
    CHECK(!tbt_ul_pin_straddles(chunk_seq + chunk_len, chunk_seq, chunk_len),
          "rcv_nxt at the chunk end");

    for (rcv = 0; rcv < chunk_len; rcv += 7)
    {
        /* The out-of-order queue starts inside the chunk: the slice stops
         * there and never covers a byte the socket already has. */
        ofo = chunk_seq + rcv + 1;
        CHECK(tbt_ul_pin_slice_end(chunk_seq, chunk_len, ofo) == ofo,
              "slice end must be the ofo start");

        /* The ofo queue starts past the chunk: the slice is the chunk tail. */
        ofo = chunk_seq + chunk_len + 32;
        CHECK(tbt_ul_pin_slice_end(chunk_seq, chunk_len, ofo) ==
                  chunk_seq + chunk_len,
              "slice end must be the chunk end");
    }

    CHECK(tbt_ul_pin_share_len(TBT_FEC_S, 8, 7, 0) == TBT_FEC_S,
          "full tail share");
    CHECK(tbt_ul_pin_share_len(TBT_FEC_S, 8, 7, 100) == 100,
          "short tail share");
    CHECK(tbt_ul_pin_share_len(TBT_FEC_S, 8, 6, 100) == TBT_FEC_S,
          "a mid-block share is never short");
}

static void test_flags_proof(void)
{
    /* Without the stream end the LAST bit is unknown, so a replay must not be
     * attempted: the segment could not be promised byte-identical. */
    CHECK(!tbt_ul_pin_flags_known(false, 0, 0), "unknown end must refuse");
    CHECK(tbt_ul_pin_flags_known(true, 0, 1), "one-packet stream");
    CHECK(tbt_ul_pin_flags_known(true, 7, 8), "last packet of a stream");
    CHECK(!tbt_ul_pin_flags_known(true, 8, 8), "seq past the end is refused");
    CHECK(tbt_ul_pin_flags(7, 8) == TBT_FEC_FF_LAST, "final packet carries LAST");
    CHECK(tbt_ul_pin_flags(6, 8) == 0, "earlier packet carries no LAST");
}

static void test_identity(void)
{
    static const struct
    {
        u16 n, m;
    } shapes[] = { { 1, 1 }, { 4, 4 }, { 8, 8 }, { 8, 24 }, { 154, 102 } };
    static const u16 lens[] = { TBT_FEC_S, 1, 1000, TBT_FEC_S - 1 };
    size_t si, li;

    for (si = 0; si < sizeof(shapes) / sizeof(shapes[0]); si++)
        for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++)
        {
            u16 n = shapes[si].n, m = shapes[si].m;
            u16 plen = lens[li];
            u16 shares[2];
            size_t k;

            shares[0] = 0;
            shares[1] = (u16)(n - 1);
            for (k = 0; k < 2 && k < n; k++)
            {
                u16 share = shares[k];
                int ci;

                for (ci = 0; ci < 2; ci++)
                {
                    struct tbt_crypto_pair near, far;
                    struct tbt_ul_pin_desc d;
                    struct tbt_fec_hdr parsed;
                    u8 key[TBT_CRYPTO_KEY_LEN];
                    u8 tx_nonce[TBT_CRYPTO_NONCE_LEN] = { 1, 2, 3, 4,
                                                          5, 6, 7, 8 };
                    u8 rx_nonce[TBT_CRYPTO_NONCE_LEN] = { 9, 10, 11, 12,
                                                          13, 14, 15, 16 };
                    u8 plain[TBT_FEC_S];
                    u8 *rec, *body;
                    size_t hdr_line, tx_len;
                    u32 got_len = 0;
                    u16 i;
                    bool crypto = ci == 1;
                    int rc;

                    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
                        key[i] = (u8)(0x40 + i);
                    /* Both ends cross the same two nonces: the sender's TX
                     * context and the receiver's RX context are the halves
                     * that share one nonce. */
                    rc = tbt_crypto_pair_init(&near, crypto ? key : NULL,
                                              tx_nonce, rx_nonce);
                    CHECK(rc == 0, "near pair_init rc=%d", rc);
                    rc = tbt_crypto_pair_init(&far, crypto ? key : NULL,
                                              rx_nonce, tx_nonce);
                    CHECK(rc == 0, "far pair_init rc=%d", rc);

                    for (i = 0; i < TBT_FEC_S; i++)
                        payload[i] = (u8)(i * 7u + 3u);
                    memcpy(plain, payload, plen);

                    memset(&d, 0, sizeof(d));
                    d.sid = 0x123456789abull;
                    d.block_id = 0x2a;
                    d.group = (u16)(si % 3);
                    d.share = share;
                    d.n = n;
                    d.m = m;
                    d.payload_len = plen;
                    d.flags = (share == n - 1) ? TBT_FEC_FF_LAST : 0;
                    d.seq = tbt_fec_data_seq(0x1000, d.group, n, share);

                    tx_len = tx_chunk(wire, sizeof(wire),
                                      crypto ? &near.tx : NULL, &d, payload);
                    CHECK(tx_len == tbt_ul_pin_chunk_len(plen),
                          "sender len=%zu, predicted %u", tx_len,
                          tbt_ul_pin_chunk_len(plen));

                    rc = tbt_ul_pin_build(crypto ? &far.rx : NULL, crypto, &d,
                                          payload, rebuilt, sizeof(rebuilt),
                                          &got_len);
                    CHECK(rc == 0, "rebuild rc=%d n=%u m=%u share=%u plen=%u",
                          rc, n, m, share, plen);
                    CHECK(got_len == tx_len, "rebuild len=%u, sender=%zu",
                          got_len, tx_len);
                    CHECK(memcmp(wire, rebuilt, tx_len) == 0,
                          "segment differs n=%u m=%u share=%u plen=%u crypto=%d",
                          n, m, share, plen, crypto);

                    /* The rebuilt chunk is a valid record. The record starts
                     * after the chunk size line; the payload after the 48-byte
                     * FEC header. */
                    hdr_line = got_len - TBT_FEC_HDR_LEN - plen - 2;
                    rec = rebuilt + hdr_line;
                    body = rec + TBT_FEC_HDR_LEN;
                    rc = tbt_fec_rec_check(rec, (size_t)TBT_FEC_HDR_LEN + plen,
                                           TBT_FEC_PAYLOAD_MAX, &parsed);
                    CHECK(rc == 0, "rec_check rc=%d", rc);
                    CHECK(parsed.type == TBT_FEC_DATA, "type=%u", parsed.type);
                    CHECK(parsed.block_id == d.block_id, "block=%llu",
                          (unsigned long long)parsed.block_id);
                    CHECK(parsed.seq == d.seq, "seq=%llu",
                          (unsigned long long)parsed.seq);
                    CHECK(parsed.share_index == share, "share=%u",
                          parsed.share_index);
                    CHECK(parsed.payload_len == plen, "payload_len=%u",
                          parsed.payload_len);
                    CHECK(parsed.flags == d.flags, "flags=%u", parsed.flags);

                    /* The receiver decrypts first and verifies after: the
                     * payload CRC covers the clear bytes. */
                    if (crypto &&
                        tbt_crypto_body_xor_flow(&far.rx, parsed.sid,
                                                 parsed.seq, false,
                                                 parsed.share_index, body,
                                                 parsed.payload_len))
                        CHECK(0, "rx keystream failed");
                    rc = tbt_fec_hdr_verify(&parsed, body);
                    CHECK(rc == 0, "hdr_verify rc=%d", rc);
                    CHECK(memcmp(body, plain, plen) == 0,
                          "decoded payload differs");
                }
            }
        }
}

int main(void)
{
    test_mapping();
    test_positions();
    test_slices();
    test_flags_proof();
    test_identity();

    if (failures)
    {
        printf("test_ul_pin_inject: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ul_pin_inject: ok\n");
    return 0;
}
