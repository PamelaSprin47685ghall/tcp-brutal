/* test_fec_sclc_data.c - SC-LDPC data-plane contracts (mode 3).
 *
 * The codecs have their own tests (test_fec_sclc, test_sclc_uplink). This
 * one covers the seam the data plane depends on:
 *
 *   - tbt_fec_sclc_shape_of() names the variant and its graph parameters
 *     from the configured shape alone, because the wire carries no codec
 *     field;
 *   - FEC_CFG pack/unpack accepts both SC-LDPC encodings (downlink
 *     w=256/k=500/d=0, uplink w=16/k=128/d=64) and still rejects an RLNC
 *     body whose w < k is not a block;
 *   - tbt_fec_grouped_new_ops() builds the right codec and the dispatch
 *     functions encode/decode through it;
 *   - a block that decodes only partly must not report success: the data
 *     plane asks is_known() per share, because both SC-LDPC decoders return
 *     a count, not a whole-block status.
 *
 *   make host-test
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_sclc_uplink.h"

static int checks;
static int failures;

static void ok(bool cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", what);
}

static void ok_int(int got, int want, const char *what)
{
    char buf[96];

    checks++;
    if (got == want)
        return;
    failures++;
    snprintf(buf, sizeof(buf), "%s: got %d, want %d", what, got, want);
    printf("FAIL %s\n", buf);
}

static void ok_u16(uint16_t got, uint16_t want, const char *what)
{
    char buf[96];

    checks++;
    if (got == want)
        return;
    failures++;
    snprintf(buf, sizeof(buf), "%s: got %u, want %u", what, got, want);
    printf("FAIL %s\n", buf);
}

static void fill_pattern(uint8_t *p, size_t len, uint32_t seed)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        seed = seed * 1664525u + 1013904223u;
        p[i] = (uint8_t)(seed >> 24);
    }
}

/* ---- shape selection ------------------------------------------------ */

static void case_shape_of(void)
{
    struct tbt_fec_sclc_shape sh;

    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(128, 128, 256, 0, 0, &sh), 0,
           "downlink shape resolves");
    ok(sh.variant == TBT_FEC_SCLC_DOWN, "downlink variant");
    ok_u16(sh.groups, 1, "downlink one group");
    ok_u16(sh.block, TBT_FEC_SCLC_BLOCK_DOWN, "downlink default block");
    ok_u16(sh.L, TBT_FEC_SCLC_L_DOWN, "downlink default L");
    ok_u16(sh.rate_ppm, 500, "downlink rate from n/(n+m)");

    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(8, 24, 256, 0, 0, &sh), 0,
           "uplink shape resolves");
    ok(sh.variant == TBT_FEC_SCLC_UP, "uplink variant");
    ok_u16(sh.groups, 8, "uplink eight groups");
    ok_u16(sh.block, TBT_FEC_SCLC_BLOCK_UP, "uplink default block");
    ok_u16(sh.L, TBT_FEC_SCLC_L_UP, "uplink default L");
    ok_u16(sh.rate_256, 64, "uplink rate in 1/256");

    /* The binary codec's graph is 128+128; any other split of 256 shares is
     * a shape error, not an allocation failure. */
    ok_int(tbt_fec_sclc_shape_of(64, 192, 256, 0, 0, &sh), -EINVAL,
           "downlink rejects a 64/192 split");
    /* One group of fewer than 256 shares is neither shipped shape. */
    ok_int(tbt_fec_sclc_shape_of(4, 4, 8, 0, 0, &sh), -EINVAL,
           "one small group is not a shape");
    /* carriers must be a whole number of blocks */
    ok_int(tbt_fec_sclc_shape_of(8, 24, 250, 0, 0, &sh), -EINVAL,
           "carriers must divide n+m");
    /* explicit block/L win */
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(128, 128, 256, 512, 16, &sh), 0,
           "downlink explicit block/L");
    ok_u16(sh.block, 512, "explicit block kept");
    ok_u16(sh.L, 16, "explicit L kept");
}

/* ---- FEC_CFG -------------------------------------------------------- */

static void cfg_fix_crc(uint8_t *buf)
{
    uint32_t crc = tbt_fec_crc32c(0, buf, TBT_FEC_CFG_CRC_BYTES);

    buf[TBT_FEC_CFG_CRC + 0] = (uint8_t)(crc);
    buf[TBT_FEC_CFG_CRC + 1] = (uint8_t)(crc >> 8);
    buf[TBT_FEC_CFG_CRC + 2] = (uint8_t)(crc >> 16);
    buf[TBT_FEC_CFG_CRC + 3] = (uint8_t)(crc >> 24);
}

static void case_cfg_roundtrip(void)
{
    struct tbt_fec_cfg c, out;
    uint8_t body[TBT_FEC_CFG_LEN];

    /* Downlink: w=256, k=500 (rate), d=0. The old w >= k rule refused it
     * before the codec was ever built. */
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 128;
    c.m = 128;
    c.w = 256;
    c.k = 500;
    c.d = 0;
    c.flags = TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT;
    c.groups = 1;
    c.seed = 0x1234;
    ok_int(tbt_fec_cfg_pack(&c, body), 0, "downlink CFG packs");
    memset(&out, 0, sizeof(out));
    ok_int(tbt_fec_cfg_unpack(body, &out), 0, "downlink CFG unpacks");
    ok(out.n == 128 && out.m == 128 && out.w == 256 && out.k == 500 &&
           out.d == 0 && out.groups == 1,
       "downlink CFG round-trips");
    ok(out.flags == (TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT),
       "INIT flag survives the round trip");

    /* Uplink: k = block (128) > w = L (16), d = rate in 1/256. */
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 8;
    c.m = 24;
    c.w = 16;
    c.k = 128;
    c.d = 64;
    c.flags = TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT;
    c.groups = 8;
    c.seed = 0x5678;
    ok_int(tbt_fec_cfg_pack(&c, body), 0, "uplink CFG packs");
    memset(&out, 0, sizeof(out));
    ok_int(tbt_fec_cfg_unpack(body, &out), 0, "uplink CFG unpacks");
    ok(out.n == 8 && out.m == 24 && out.w == 16 && out.k == 128 &&
           out.d == 64 && out.groups == 8,
       "uplink CFG round-trips");

    /* An RLNC body with w < k stays invalid: k=4, w=2 is not a codeword
     * block the GF(16) factory accepts (block >= 16, even). */
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 4;
    c.m = 4;
    c.w = 2;
    c.k = 4;
    c.d = 4;
    c.flags = TBT_FEC_CFG_F_APPLY;
    c.groups = 32;
    ok_int(tbt_fec_cfg_pack(&c, body), 0, "bad RLNC CFG packs");
    memset(&out, 0, sizeof(out));
    ok_int(tbt_fec_cfg_unpack(body, &out), -EPROTO,
           "w<k that is not a block is refused");

    /* The INIT bit is the only new flag; an unknown bit is still refused. */
    tbt_fec_cfg_pack(&c, body);
    body[TBT_FEC_CFG_FLAGS] = 0x08;
    cfg_fix_crc(body);
    ok_int(tbt_fec_cfg_unpack(body, &out), -EPROTO,
           "unknown CFG flag is refused");
}

/* The receiver must take every graph parameter from the announced shape.
 * A downlink peer runs L=8, an uplink peer L=16, and the two directions of
 * one session legitimately differ; using the local L built the peer's codec
 * with the wrong coupling length. */
static void case_init_shape_from_cfg(void)
{
    struct tbt_fec_cfg c;
    struct tbt_fec_sclc_shape sh;

    /* Uplink announcement from a peer: k=block, w=L, d=rate/256. */
    memset(&c, 0, sizeof(c));
    c.n = 8;
    c.m = 24;
    c.groups = 8;
    c.w = 16;
    c.k = 128;
    c.d = 64;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "uplink INIT resolves");
    ok(sh.variant == TBT_FEC_SCLC_UP, "uplink INIT variant");
    ok_u16(sh.block, 128, "uplink INIT block from k");
    ok_u16(sh.L, 16, "uplink INIT L from w, not the local 8");
    ok_u16(sh.rate_256, 64, "uplink INIT rate from d");
    ok_u16(sh.groups, 8, "uplink INIT groups");

    /* Downlink announcement: w=block, k=rate, d=0. The binary codec has no
     * L, so the default is the only value. */
    memset(&c, 0, sizeof(c));
    c.n = 128;
    c.m = 128;
    c.groups = 1;
    c.w = 256;
    c.k = 500;
    c.d = 0;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "downlink INIT resolves");
    ok(sh.variant == TBT_FEC_SCLC_DOWN, "downlink INIT variant");
    ok_u16(sh.block, 256, "downlink INIT block from w");
    ok_u16(sh.rate_ppm, 500, "downlink INIT rate from k");

    /* An RLNC body is not an SC-LDPC announcement. */
    memset(&c, 0, sizeof(c));
    c.n = 4;
    c.m = 4;
    c.groups = 32;
    c.w = 8;
    c.k = 4;
    c.d = 4;
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), -EINVAL,
           "RLNC body is not an SC-LDPC INIT");

    /* The point is the direction of the lookup: the same INIT body resolves
     * to L=16 no matter what the receiving side configured for its own
     * transmit codec. The resolver has no local-L input at all, so the
     * field failure (far built the uplink codec with its own L=8) cannot
     * recur through this path. */
    memset(&c, 0, sizeof(c));
    c.n = 8;
    c.m = 24;
    c.groups = 8;
    c.w = 16;
    c.k = 128;
    c.d = 64;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "the peer INIT resolves on a downlink-configured receiver");
    ok_u16(sh.L, 16, "and carries the peer's L, not the receiver's");
    ok_u16(sh.block, 128, "and the peer's block");
}

/* ---- downlink: one group of 128+128 --------------------------------- */

static void case_downlink_roundtrip(void)
{
    struct tbt_fec_grouped *tx, *rx;
    struct tbt_fec_sclc_shape sh;
    uint8_t *data[128], *parity[128];
    uint8_t *rxdata[128], *rxparity[128];
    uint8_t present_d[128], present_p[128];
    int i, rc;

    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(128, 128, 256, 0, 0, &sh), 0,
           "downlink shape for codec");
    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0xabc);
    ok(tx != NULL, "downlink tx codec builds");
    if (!tx)
        return;
    ok_int(tbt_fec_grouped_mode(tx), 3, "downlink container is mode 3");
    ok(tbt_fec_grouped_ctx(tx, 0) == NULL,
       "the RLNC accessor does not alias a block codec");
    ok_u16(tbt_fec_grouped_band(tx, 0), 0, "a block codec has no band");

    for (i = 0; i < 128; i++)
    {
        data[i] = malloc(TBT_FEC_S);
        parity[i] = malloc(TBT_FEC_S);
        rxdata[i] = malloc(TBT_FEC_S);
        rxparity[i] = malloc(TBT_FEC_S);
        if (!data[i] || !parity[i] || !rxdata[i] || !rxparity[i])
        {
            ok(false, "downlink buffers");
            return;
        }
        fill_pattern(data[i], TBT_FEC_S, (uint32_t)(i + 1));
        memset(parity[i], 0, TBT_FEC_S);
        memset(rxdata[i], 0, TBT_FEC_S);
        memset(rxparity[i], 0, TBT_FEC_S);
    }
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 128, parity), 0,
           "downlink encode");

    /* One lost DATA share, every parity share present. */
    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0xabc);
    ok(rx != NULL, "downlink rx codec builds");
    if (!rx)
        return;
    memset(present_d, 1, sizeof(present_d));
    memset(present_p, 1, sizeof(present_p));
    for (i = 0; i < 128; i++)
    {
        if (i != 7)
            memcpy(rxdata[i], data[i], TBT_FEC_S);
        memcpy(rxparity[i], parity[i], TBT_FEC_S);
    }
    present_d[7] = 0;
    rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d,
                                present_p, 128);
    ok(rc >= 0, "downlink decode returns a count");
    ok(tbt_fec_grouped_is_known(rx, 0, 7) != 0,
       "the lost share is known after decode");
    ok(memcmp(rxdata[7], data[7], TBT_FEC_S) == 0,
       "the recovered share is byte-exact");
    tbt_fec_grouped_free(rx);

    /* Partial recovery: only the first three parity shares, so the binary
     * codec can restore DATA shares 0..2 and nothing else. The data plane
     * must see is_known()==0 for the rest. */
    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0xabc);
    ok(rx != NULL, "downlink partial rx codec builds");
    if (!rx)
        return;
    memset(present_d, 1, sizeof(present_d));
    memset(present_p, 0, sizeof(present_p));
    for (i = 0; i < 128; i++)
    {
        if (i >= 10)
            memcpy(rxdata[i], data[i], TBT_FEC_S);
        memcpy(rxparity[i], parity[i], TBT_FEC_S);
    }
    for (i = 0; i < 10; i++)
        present_d[i] = 0;
    for (i = 0; i < 3; i++)
        present_p[i] = 1;
    rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d,
                                present_p, 128);
    {
        int known = 0;

        for (i = 0; i < 10; i++)
            if (tbt_fec_grouped_is_known(rx, 0, (u32)i))
                known++;
        /* The point of the case is the mismatch between a count and a whole
         * block: with three parity shares the block cannot be complete. */
        ok(known < 10, "three parity shares cannot restore ten lost shares");
        /* The binary decoder reports -EAGAIN when it recovered nothing, and
         * a positive count for a partial recovery. Neither is a whole-block
         * status, which is why the data plane asks is_known(). */
        ok(rc == -EAGAIN || rc >= 0, "partial decode returns a count or EAGAIN");
    }
    tbt_fec_grouped_free(rx);

    tbt_fec_grouped_free(tx);
    for (i = 0; i < 128; i++)
    {
        free(data[i]);
        free(parity[i]);
        free(rxdata[i]);
        free(rxparity[i]);
    }
}

/* ---- uplink: one group of 8+24 -------------------------------------- */

static void case_uplink_roundtrip(void)
{
    struct tbt_fec_grouped *tx, *rx;
    uint8_t *data[8], *parity[24];
    uint8_t *rxdata[8], *rxparity[24];
    uint8_t present_d[8], present_p[24];
    int i, rc;

    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 1, 8, 24, 128, 16,
                                 TBT_FEC_S, 64, 0xdef);
    ok(tx != NULL, "uplink tx codec builds");
    if (!tx)
        return;
    ok_int(tbt_fec_grouped_mode(tx), 3, "uplink container is mode 3");

    for (i = 0; i < 8; i++)
    {
        data[i] = malloc(TBT_FEC_S);
        rxdata[i] = malloc(TBT_FEC_S);
        if (!data[i] || !rxdata[i])
        {
            ok(false, "uplink data buffers");
            return;
        }
        fill_pattern(data[i], TBT_FEC_S, (uint32_t)(0x100 + i));
        memset(rxdata[i], 0, TBT_FEC_S);
    }
    for (i = 0; i < 24; i++)
    {
        parity[i] = malloc(TBT_FEC_S);
        rxparity[i] = malloc(TBT_FEC_S);
        if (!parity[i] || !rxparity[i])
        {
            ok(false, "uplink parity buffers");
            return;
        }
        memset(parity[i], 0, TBT_FEC_S);
        memset(rxparity[i], 0, TBT_FEC_S);
    }
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 8, parity), 0,
           "uplink encode");

    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 1, 8, 24, 128, 16,
                                 TBT_FEC_S, 64, 0xdef);
    ok(rx != NULL, "uplink rx codec builds");
    if (!rx)
        return;
    memset(present_d, 1, sizeof(present_d));
    memset(present_p, 1, sizeof(present_p));
    for (i = 0; i < 8; i++)
        if (i != 3)
            memcpy(rxdata[i], data[i], TBT_FEC_S);
    for (i = 0; i < 24; i++)
        memcpy(rxparity[i], parity[i], TBT_FEC_S);
    present_d[3] = 0;
    rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d, present_p,
                                8);
    ok(rc >= 0, "uplink decode returns a count");
    ok(tbt_fec_grouped_is_known(rx, 0, 3) != 0,
       "the lost uplink share is known");
    ok(memcmp(rxdata[3], data[3], TBT_FEC_S) == 0,
       "the recovered uplink share is byte-exact");
    tbt_fec_grouped_free(rx);
    tbt_fec_grouped_free(tx);

    for (i = 0; i < 8; i++)
    {
        free(data[i]);
        free(rxdata[i]);
    }
    for (i = 0; i < 24; i++)
    {
        free(parity[i]);
        free(rxparity[i]);
    }
}

/* An RLNC container still dispatches through the same accessors, so the data
 * plane has one path for every codec. */
static void case_rlnc_dispatch(void)
{
    struct tbt_fec_grouped *g = tbt_fec_grouped_new(1, 4, 4, 4, 8, TBT_FEC_S,
                                                    4, 1);
    uint8_t *data[4], *parity[4], present_d[4], present_p[4];
    int i;

    ok(g != NULL, "rlnc container builds");
    if (!g)
        return;
    ok_int(tbt_fec_grouped_mode(g), 1, "rlnc container is mode 1");
    ok(tbt_fec_grouped_ctx(g, 0) != NULL, "the rlnc accessor still works");
    for (i = 0; i < 4; i++)
    {
        data[i] = calloc(1, TBT_FEC_S);
        parity[i] = calloc(1, TBT_FEC_S);
        if (!data[i] || !parity[i])
        {
            ok(false, "rlnc buffers");
            return;
        }
        fill_pattern(data[i], TBT_FEC_S, (uint32_t)(0x200 + i));
    }
    memset(present_d, 1, sizeof(present_d));
    memset(present_p, 1, sizeof(present_p));
    ok(tbt_fec_grouped_encode(g, 0, data, 4, parity) > 0, "rlnc encode");
    tbt_fec_grouped_free(g);
    for (i = 0; i < 4; i++)
    {
        free(data[i]);
        free(parity[i]);
    }
}

/* 0.3.1: a short block sends one cut row, and the NACK scan names only
 * shares the sender could have written.
 *
 * The sender's data-plane code is kernel-only, so the decisions it makes are
 * the two pure helpers in tbt_fec.h; this case pins them and the exact bytes
 * of the one record a short block still sends. */
static void case_short_block_parity(void)
{
    struct tbt_fec_grouped *tx;
    struct tbt_fec_hdr h, back;
    uint8_t *data[128], *parity[128];
    uint8_t wire[TBT_FEC_HDR_LEN], wire2[TBT_FEC_HDR_LEN];
    int i;

    /* The sender keeps row 0 alone for a short SC-LDPC block. */
    ok_u16(tbt_fec_tx_parity_count(1, 128, 128, true), 1,
           "short block: one cut row");
    ok_u16(tbt_fec_tx_parity_count(127, 128, 128, true), 1,
           "127-share short block: one cut row");
    ok_u16(tbt_fec_tx_parity_count(128, 128, 128, true), 128,
           "full block: every parity row");
    ok_u16(tbt_fec_tx_parity_count(1, 128, 4, false), 4,
           "RLNC short block: every row");
    ok_u16(tbt_fec_tx_parity_count(1, 128, 0, true), 0,
           "no parity shape: no row");

    /* The NACK scan names a DATA share only when it is proven real. */
    ok_u16(tbt_fec_nack_data_limit(false, 128, 0), 0,
           "cut unknown, no DATA seen: no DATA NACK");
    ok_u16(tbt_fec_nack_data_limit(false, 128, 1), 1,
           "cut unknown, share 0 seen: only share 0 is proven");
    ok_u16(tbt_fec_nack_data_limit(false, 128, 64), 64,
           "cut unknown, share 63 seen: gaps below it are proven");
    ok_u16(tbt_fec_nack_data_limit(true, 7, 1), 7,
           "cut known: every real share may be named");
    ok_u16(tbt_fec_nack_data_limit(false, 7, 9), 7,
           "the proven range is capped at the block's real length");

    /* Parity requests converge on the rows the sender kept. */
    ok_u16(tbt_fec_nack_parity_limit(false, 128, 128, 128, true), 1,
           "cut unknown: only the cut row");
    ok_u16(tbt_fec_nack_parity_limit(true, 1, 128, 128, true), 1,
           "short block: only the cut row exists");
    ok_u16(tbt_fec_nack_parity_limit(true, 128, 128, 128, true), 128,
           "full block: every row may be named");
    ok_u16(tbt_fec_nack_parity_limit(true, 1, 128, 4, false), 4,
           "RLNC short block: every row may be named");

    /* The one record a short block sends is a normal PARITY row: same
     * 48-byte layout, win_len states the cut, epoch states the last share's
     * length, and the payload is the codec's row 0 byte for byte. */
    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0xabc);
    ok(tx != NULL, "short-block codec builds");
    if (!tx)
        return;
    for (i = 0; i < 128; i++)
    {
        data[i] = calloc(1, TBT_FEC_S);
        parity[i] = calloc(1, TBT_FEC_S);
        if (!data[i] || !parity[i])
        {
            ok(false, "short-block buffers");
            tbt_fec_grouped_free(tx);
            return;
        }
    }
    fill_pattern(data[0], TBT_FEC_S, 0x51);
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 128, parity), 0,
           "short-block encode");

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = 128; /* parity 0 */
    h.n = 128;
    h.m = 128;
    h.block_id = 7;
    h.seq = tbt_fec_seq_of_block_pkt(7, 0, 0, 1, 128);
    h.win_len = 1;
    h.payload_len = TBT_FEC_S;
    h.epoch = 4;
    h.conv = TBT_FEC_CONV;
    h.sid = 0x1234;
    ok_int(tbt_fec_hdr_seal(&h, parity[0]), 0, "cut row seals");
    ok_int(tbt_fec_hdr_pack(&h, wire), 0, "cut row packs");
    ok_int(tbt_fec_hdr_unpack(wire, &back), 0, "cut row unpacks");
    ok_int(tbt_fec_hdr_pack(&back, wire2), 0, "cut row re-packs");
    ok(memcmp(wire, wire2, TBT_FEC_HDR_LEN) == 0,
       "cut row is byte-exact through pack/unpack/pack");
    ok_int(tbt_fec_hdr_check(&back, TBT_FEC_S), 0,
           "cut row passes the frozen wire check");
    ok_int(tbt_fec_hdr_verify(&back, parity[0]), 0,
           "cut row payload verifies byte-exact");
    ok_u16(back.win_len, 1, "cut row states the real share count");
    ok_u16(back.epoch, 4, "cut row states the last share's length");
    ok(back.share_index == 128 && back.payload_len == TBT_FEC_S,
       "cut row is parity 0 with a full slot");

    tbt_fec_grouped_free(tx);
    for (i = 0; i < 128; i++)
    {
        free(data[i]);
        free(parity[i]);
    }
}

int main(void)
{
    case_shape_of();
    case_cfg_roundtrip();
    case_init_shape_from_cfg();
    case_downlink_roundtrip();
    case_uplink_roundtrip();
    case_rlnc_dispatch();
    case_short_block_parity();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
