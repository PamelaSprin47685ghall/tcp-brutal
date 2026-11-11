/* test_client_wire.c - 0.7.3/0.8.x/0.9.0 wire rejection matrix.
 *
 * The 0.8.0 constant-frame cut is a hard revision change: TBT_CONV moves to
 * 0x000b, S falls to 1024, short DATA gains PADDED, and the uplink codec
 * moves to block=64/L=8. This test feeds the old shapes to the new checks
 * and the new shapes to a frozen snapshot of the old checks, so a silent
 * reinterpretation shows up as a wrong verdict instead of a passing tunnel.
 * 0.9.0 keeps that frame shape and moves TBT_CONV to 0x000c for the 90-byte
 * HELLO, so a 0.8.x peer is refused at its first HELLO header.
 *
 * The "old" helpers below are a deliberate 0.7.3 rule snapshot: constants
 * and the fields that the two cuts disagree on. They are not the old
 * implementation, but they pin the refusal contract this branch relies on.
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_fec.h"
#include "tbt_http.h"
#include "tbt_proto.h"
#include "tbt_uapi.h"

#include <stdio.h>
#include <string.h>

#define OLD_CONV 0x000au
#define PREV_CONV 0x000bu /* 0.8.x: same frames, 66-byte HELLO */
#define OLD_S 8192u
#define OLD_FF_MASK 0x01u
#define OLD_BLOCK_UP 128u
#define OLD_L_UP 16u

static int checks;
static int failures;

static void ok(int cond, const char *name)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", name);
}

/* 0.7.3 rule snapshot: session header accepts only conv 0x000a. */
static int old_hdr_check(const struct tbt_hdr *h)
{
    if (!h || h->conv != OLD_CONV)
        return -EPROTO;
    if (h->type != TBT_F_HELLO || h->body_len != TBT_HELLO_BODY_LEN_V6)
        return -EPROTO;
    return 0;
}

/* 0.7.3 rule snapshot: DATA may be short but has no PADDED flag and its
 * body is at most 8192 bytes. */
static int old_fec_check(const struct tbt_fec_hdr *h)
{
    if (!h || h->conv != TBT_FEC_CONV)
        return -EPROTO;
    if (h->flags & ~OLD_FF_MASK)
        return -EPROTO;
    if (h->type == TBT_FEC_DATA)
    {
        if (!h->payload_len || h->payload_len > OLD_S)
            return -EPROTO;
        if (h->win_len != 0)
            return -EPROTO;
    }
    return 0;
}

/* 0.7.3 CFG snapshot: the uplink block is 128 with L=16, and the downlink
 * ladder is 256/512. Fields are read from the wire body. */
static int old_cfg_check(const u8 body[TBT_FEC_CFG_LEN], u16 carriers)
{
    u8 variant = body[TBT_FEC_CFG_CODEC_VARIANT];
    u8 L = body[TBT_FEC_CFG_L];
    u16 block = tbt_le16(body + TBT_FEC_CFG_BLOCK);
    u16 n = tbt_le16(body + TBT_FEC_CFG_N);
    u16 m = tbt_le16(body + TBT_FEC_CFG_M);
    u16 groups = tbt_le16(body + TBT_FEC_CFG_GROUPS);
    u32 rate = tbt_le32(body + TBT_FEC_CFG_RATE_PPM);
    u32 shares = (u32)n + m;

    (void)carriers;
    if (variant != 2 || !n || !m || !groups || shares > TBT_FEC_MAX_SHARES)
        return -EPROTO;
    if (L != 8 && L != 16)
        return -EPROTO;
    if (block != 256 && block != 512 &&
        !(block == OLD_BLOCK_UP && groups > 1 && L == OLD_L_UP))
        return -EPROTO;
    if (rate < 250000u || rate > 700000u)
        return -EPROTO;
    if ((u32)(((u64)rate * shares) / 1000000u) != n)
        return -EPROTO;
    return 0;
}

static void make_cfg(struct tbt_fec_cfg *c, u16 n, u16 m, u16 groups,
                     u16 block, u8 L)
{
    memset(c, 0, sizeof(*c));
    c->epoch = 1;
    c->n = n;
    c->m = m;
    c->groups = groups;
    c->flags = TBT_FEC_CFG_F_INIT;
    c->seed = 7;
    c->codec_variant = 2;
    c->L = L;
    c->block = block;
    c->rate_ppm = (u32)(((u64)n * 1000000u + (n + m) - 1u) / (n + m));
}

int main(void)
{
    struct tbt_hdr h;
    struct tbt_fec_hdr f;
    struct tbt_fec_cfg c;
    struct tbt_fec_cfg out;
    struct tbt_fec_sclc_shape sh;
    u8 body[TBT_FEC_CFG_LEN];
    char chunk[64];
    struct tbt_http_chunk_parser p;
    const u8 *cbody = NULL;
    size_t clen = 0;
    int err = 0;

    /* Pin the new constants first: a build that still links the old headers
     * must not pass this test. */
    ok(TBT_CONV == 0x000cu, "TBT_CONV is 0x000c");
    ok(TBT_FEC_PAYLOAD_MAX == 1024u, "S is 1024");
    ok(TBT_FEC_REC_MAX == 1072u, "record max is 1072");
    ok(TBT_FEC_FF_MASK == 0x03u, "PADDED is in the flag mask");
    ok(TBT_FEC_SCLC_BLOCK_UP == 64u, "uplink block is 64");
    ok(TBT_FEC_SCLC_L_UP == 8u, "uplink L is 8");
    ok(TBT_HTTP_CHUNK_DATA_MAX == 8228u, "chunk data max is 8228");

    /* Session gate: HELLO conv 0x000a (0.7.3), 0x000b (0.8.x), 0x000c. */
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_HELLO;
    h.body_len = TBT_HELLO_BODY_LEN_V6;
    h.conv = OLD_CONV;
    ok(tbt_hdr_check(&h) == -EPROTO, "0.7.3 HELLO rejected by 0.9");
    ok(old_hdr_check(&h) == 0, "0.7.3 HELLO accepted by 0.7.3 snapshot");
    h.conv = PREV_CONV;
    ok(tbt_hdr_check(&h) == -EPROTO, "0.8.x HELLO rejected by 0.9");
    h.conv = TBT_CONV;
    ok(tbt_hdr_check(&h) == 0, "0.9 HELLO accepted by 0.9");
    ok(old_hdr_check(&h) == -EPROTO, "0.9 HELLO rejected by 0.7.3 snapshot");

    /* Full 0.7.3 DATA: 8192 bytes exceeds the 0.8 slot. */
    memset(&f, 0, sizeof(f));
    f.type = TBT_FEC_DATA;
    f.n = 8;
    f.m = 24;
    f.share_index = 0;
    f.seq = 0;
    f.payload_len = OLD_S;
    f.conv = TBT_FEC_CONV;
    f.sid = 1;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == -EPROTO,
       "0.7.3 8192-byte DATA rejected by 0.8");
    ok(old_fec_check(&f) == 0, "0.7.3 8192-byte DATA accepted by old snapshot");

    /* The narrow seam: a non-PADDED short DATA <= S is still legal in 0.8.
     * It cannot reach this point through a 0.8 session because HELLO already
     * failed, so name it instead of hiding it. */
    f.payload_len = 512;
    f.win_len = 0;
    f.flags = 0;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == 0,
       "0.7.3 short DATA is still wire-legal in 0.8");
    ok(old_fec_check(&f) == 0, "short DATA accepted by old snapshot");

    /* 0.8 PADDED DATA. */
    f.payload_len = TBT_FEC_PAYLOAD_MAX;
    f.win_len = 512;
    f.flags = TBT_FEC_FF_PADDED;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == 0,
       "0.8 PADDED DATA accepted by 0.8");
    ok(old_fec_check(&f) == -EPROTO,
       "0.8 PADDED DATA rejected by 0.7.3 flags");
    f.payload_len = 512;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == -EPROTO,
       "PADDED with a short body rejected");
    f.payload_len = TBT_FEC_PAYLOAD_MAX;
    f.win_len = 0;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == -EPROTO,
       "PADDED with win_len 0 rejected");
    f.win_len = 512;
    f.epoch = 0;
    f.type = TBT_FEC_PARITY;
    f.share_index = 8;
    f.flags = TBT_FEC_FF_PADDED;
    ok(tbt_fec_hdr_check(&f, TBT_FEC_PAYLOAD_MAX) == -EPROTO,
       "PADDED PARITY rejected");

    /* Uplink CFG: 128/16 vs 64/8. */
    make_cfg(&c, 8, 24, 8, OLD_BLOCK_UP, OLD_L_UP);
    ok(tbt_fec_cfg_pack(&c, body) == 0, "pack 0.7.3 CFG");
    ok(tbt_fec_cfg_unpack(body, 256, &out) == -EPROTO,
       "0.7.3 uplink CFG rejected by 0.8");
    ok(old_cfg_check(body, 256) == 0,
       "0.7.3 uplink CFG accepted by old snapshot");
    make_cfg(&c, 8, 24, 8, TBT_FEC_SCLC_BLOCK_UP, TBT_FEC_SCLC_L_UP);
    ok(tbt_fec_cfg_pack(&c, body) == 0, "pack 0.8 CFG");
    ok(tbt_fec_cfg_unpack(body, 256, &out) == 0,
       "0.8 uplink CFG accepted by 0.8");
    ok(tbt_fec_sclc_shape_of_cfg(&out, 256, &sh) == 0,
       "0.8 uplink CFG resolves to a shape");
    ok(old_cfg_check(body, 256) == -EPROTO,
       "0.8 uplink CFG rejected by 0.7.3 snapshot");

    /* The downlink 256-share ladder did not move. */
    make_cfg(&c, 128, 128, 1, 256, 8);
    ok(tbt_fec_cfg_pack(&c, body) == 0, "pack downlink CFG");
    ok(tbt_fec_cfg_unpack(body, 256, &out) == 0,
       "128/128 downlink CFG still accepted by 0.8");
    ok(tbt_fec_sclc_shape_of_cfg(&out, 256, &sh) == 0,
       "128/128 downlink shape still resolves");

    /* HTTP chunk ceiling: a 0.7.3 full record is one chunk over the 0.8
     * limit; the 0.8 record fits. */
    memset(chunk, 0, sizeof(chunk));
    ok(tbt_http_chunk_hdr(chunk, sizeof(chunk), OLD_S + TBT_FEC_HDR_LEN) == 0,
       "8240-byte chunk rejected by 0.8 encoder");
    ok(tbt_http_chunk_hdr(chunk, sizeof(chunk), TBT_FEC_REC_MAX) > 0,
       "1072-byte chunk encoded by 0.8");
    memset(&p, 0, sizeof(p));
    /* "2030" is 8240 decimal: the old full record's chunk size. */
    ok(tbt_http_chunk_feed(&p, (const u8 *)"2030\r\n", 6, &cbody, &clen,
                           &err) == 0 &&
           err == TBT_HTTP_PROTO,
       "8240 size line rejected as PROTO");

    printf("wire-073-080: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
