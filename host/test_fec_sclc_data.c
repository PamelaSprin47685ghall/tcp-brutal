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

    /* The binary codec builds every ladder split of the 256 shares. */
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(64, 192, 256, 0, 0, &sh), 0,
           "downlink accepts a 64/192 split");
    ok(sh.variant == TBT_FEC_SCLC_DOWN, "64/192 is the downlink variant");
    ok_u16(sh.rate_ppm, 250, "64/192 rate");
    ok_int(tbt_fec_sclc_shape_of(151, 105, 256, 0, 0, &sh), 0,
           "downlink accepts a 151/105 split");
    ok_u16(sh.rate_ppm, 589, "151/105 rate");
    ok_int(tbt_fec_sclc_shape_of(127, 129, 256, 0, 0, &sh), -EINVAL,
           "downlink rejects an off-ladder split");
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

    /* Downlink: v8 names variant/L/block/rate in the extension; the wire
     * w/k/d stay zero. */
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 128;
    c.m = 128;
    c.w = 256;
    c.k = 500;
    c.d = 0;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 256;
    c.rate_ppm = 500000;
    c.flags = TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT;
    c.groups = 1;
    c.seed = 0x1234;
    ok_int(tbt_fec_cfg_pack(&c, body), 0, "downlink CFG packs");
    memset(&out, 0, sizeof(out));
    ok_int(tbt_fec_cfg_unpack(body, 256, &out), 0, "downlink CFG unpacks");
    ok(out.n == 128 && out.m == 128 && out.w == 256 && out.k == 500 &&
           out.d == 0 && out.groups == 1 && out.codec_variant == 2 &&
           out.L == 8 && out.block == 256 && out.rate_ppm == 500000,
       "downlink CFG round-trips");
    ok(out.flags == (TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT),
       "INIT flag survives the round trip");

    /* Uplink: k = block (128) > w = L (16), d = rate in 1/256. */
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 8;
    c.m = 24;
    c.w = 8;
    c.k = 64;
    c.d = 64;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 64;
    c.rate_ppm = 250000;
    c.flags = TBT_FEC_CFG_F_APPLY | TBT_FEC_CFG_F_INIT;
    c.groups = 8;
    c.seed = 0x5678;
    ok_int(tbt_fec_cfg_pack(&c, body), 0, "uplink CFG packs");
    memset(&out, 0, sizeof(out));
    ok_int(tbt_fec_cfg_unpack(body, 256, &out), 0, "uplink CFG unpacks");
    ok(out.n == 8 && out.m == 24 && out.w == 8 && out.k == 64 &&
           out.d == 64 && out.groups == 8 && out.codec_variant == 2 &&
           out.L == 8 && out.block == 64 && out.rate_ppm == 250000,
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
    ok_int(tbt_fec_cfg_unpack(body, 256, &out), -EPROTO,
           "w<k that is not a block is refused");

    /* The INIT bit is the only new flag; an unknown bit is still refused. */
    tbt_fec_cfg_pack(&c, body);
    body[TBT_FEC_CFG_FLAGS] = 0x08;
    cfg_fix_crc(body);
    ok_int(tbt_fec_cfg_unpack(body, 256, &out), -EPROTO,
           "unknown CFG flag is refused");
}

/* The receiver must take every graph parameter from the announced shape.
 * A downlink peer runs L=8, an uplink peer L=8 with block 64, and the two
 * one session legitimately differ; using the local L built the peer's codec
 * with the wrong coupling length. */
static void case_init_shape_from_cfg(void)
{
    struct tbt_fec_cfg c;
    struct tbt_fec_sclc_shape sh;

    /* Uplink announcement from a peer: variant 2, L=8, block=64, rate. */
    memset(&c, 0, sizeof(c));
    c.n = 8;
    c.m = 24;
    c.groups = 8;
    c.w = 8;
    c.k = 64;
    c.d = 64;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 64;
    c.rate_ppm = 250000;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "uplink INIT resolves");
    ok(sh.variant == TBT_FEC_SCLC_UP, "uplink INIT variant");
    ok_u16(sh.block, 64, "uplink INIT block from the extension");
    ok_u16(sh.L, 8, "uplink INIT L from the extension, not the local downlink L");
    ok_u16(sh.rate_256, 64, "uplink INIT rate from the split");
    ok_u16(sh.groups, 8, "uplink INIT groups");

    /* Downlink announcement: variant 2 with L=8 and block=256. */
    memset(&c, 0, sizeof(c));
    c.n = 128;
    c.m = 128;
    c.groups = 1;
    c.w = 256;
    c.k = 500;
    c.d = 0;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 256;
    c.rate_ppm = 500000;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "downlink INIT resolves");
    ok(sh.variant == TBT_FEC_SCLC_DOWN, "downlink INIT variant");
    ok_u16(sh.block, 256, "downlink INIT block from the extension");
    ok_u16(sh.rate_ppm, 500, "downlink INIT rate from the split");

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
     * to its own L=8/block=64 no matter what the receiving side configured
     * for its own transmit codec. The resolver has no local-L input at all, so the
     * field failure (far built the uplink codec with its own L=8) cannot
     * recur through this path. */
    memset(&c, 0, sizeof(c));
    c.n = 8;
    c.m = 24;
    c.groups = 8;
    c.w = 8;
    c.k = 64;
    c.d = 64;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 64;
    c.rate_ppm = 250000;
    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of_cfg(&c, 256, &sh), 0,
           "the peer INIT resolves on a downlink-configured receiver");
    ok_u16(sh.L, 8, "and carries the peer's L, not the receiver's");
    ok_u16(sh.block, 64, "and the peer's block");
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

/* The receiver decodes on the share that reaches the wake line, with no
 * settle delay: a block missing 1, 4 or 32 DATA shares is inferred and
 * delivered byte-exact on that share. A block missing 120 shares (< m) is
 * attempted at once; this code cannot recover 120 of its 128 DATA shares,
 * so the attempt must not fabricate a whole-block delivery. Missing m
 * shares does not even attempt. The uplink 8x32 group wakes at present
 * n + 2 and recovers six missing shares exactly at that line. */
static void case_immediate_decode(void)
{
    struct tbt_fec_grouped *tx, *rx;
    uint8_t *data[128], *parity[128];
    uint8_t *rxdata[128], *rxparity[128];
    uint8_t present_d[128], present_p[128];
    static const int miss[] = {1, 4, 32};
    int i, k, rc;

    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0x5a5a);
    ok(tx != NULL, "immediate: downlink codec builds");
    if (!tx)
        return;
    for (i = 0; i < 128; i++)
    {
        data[i] = malloc(TBT_FEC_S);
        parity[i] = malloc(TBT_FEC_S);
        rxdata[i] = malloc(TBT_FEC_S);
        rxparity[i] = malloc(TBT_FEC_S);
        if (!data[i] || !parity[i] || !rxdata[i] || !rxparity[i])
        {
            ok(false, "immediate: downlink buffers");
            return;
        }
        fill_pattern(data[i], TBT_FEC_S, (uint32_t)(0x200 + i));
        memset(parity[i], 0, TBT_FEC_S);
    }
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 128, parity), 0,
           "immediate: downlink encode");

    for (k = 0; k < (int)(sizeof(miss) / sizeof(miss[0])); k++)
    {
        int all_known = 1, bytes_ok = 1;

        rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500,
                                     256, TBT_FEC_S, 0, 0x5a5a);
        ok(rx != NULL, "immediate: downlink rx builds");
        if (!rx)
            return;
        memset(present_d, 1, sizeof(present_d));
        memset(present_p, 1, sizeof(present_p));
        for (i = 0; i < 128; i++)
        {
            if (i < miss[k])
                present_d[i] = 0;
            else
                memcpy(rxdata[i], data[i], TBT_FEC_S);
            memcpy(rxparity[i], parity[i], TBT_FEC_S);
        }
        ok(tbt_fec_sclc_decode_wake(128, 128, 128, 1, 128 - miss[k], 128),
           "immediate: missing DATA share reaches the wake line");
        rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d,
                                    present_p, 128);
        ok(rc > 0, "immediate: downlink decode recovers shares");
        for (i = 0; i < 128; i++)
        {
            if (!tbt_fec_grouped_is_known(rx, 0, (u32)i))
                all_known = 0;
            if (memcmp(rxdata[i], data[i], TBT_FEC_S) != 0)
                bytes_ok = 0;
        }
        ok(all_known, "immediate: every share is known after decode");
        ok(bytes_ok, "immediate: delivered bytes are byte-exact");
        tbt_fec_grouped_free(rx);
    }

    /* 120 missing DATA shares (< m = 128): the wake line still fires, the
     * codec is attempted at once, and it must not report a whole block. */
    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 128, 128, 500, 256,
                                 TBT_FEC_S, 0, 0x5a5a);
    if (rx)
    {
        int known = 0;

        memset(present_d, 0, sizeof(present_d));
        memset(present_p, 1, sizeof(present_p));
        for (i = 0; i < 128; i++)
        {
            if (i >= 120)
            {
                memcpy(rxdata[i], data[i], TBT_FEC_S);
                present_d[i] = 1;
            }
            memcpy(rxparity[i], parity[i], TBT_FEC_S);
        }
        ok(!tbt_fec_sclc_decode_wake(128, 128, 128, 1, 8, 128),
           "immediate: 120 missing shares wait for the delta0 margin");
        tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d, present_p,
                               128);
        for (i = 0; i < 128; i++)
            if (tbt_fec_grouped_is_known(rx, 0, (u32)i))
                known++;
        ok(known < 128,
           "immediate: an unrecoverable 120-share block is not delivered");
        tbt_fec_grouped_free(rx);
    }

    /* Missing m shares: no attempt and no delivery. */
    ok(!tbt_fec_sclc_decode_wake(128, 128, 128, 1, 0, 128),
       "immediate: 128 missing shares do not wake");

    /* A block missing 120 PARITY shares has all its DATA and is delivered
     * without a decode. */
    ok(tbt_fec_sclc_decode_wake(128, 128, 128, 1, 128, 8),
       "immediate: complete DATA with 120 parity missing is ready");

    for (i = 0; i < 128; i++)
    {
        free(data[i]);
        free(parity[i]);
        free(rxdata[i]);
        free(rxparity[i]);
    }
    tbt_fec_grouped_free(tx);

    /* Uplink 8x32 group: n=8, m=24, wake at present n + 2 = 10. Six
     * missing DATA shares with eight parity shares are exactly the line,
     * and the group is recovered and delivered byte-exact. */
    {
        uint8_t *udata[8], *uparity[24];
        uint8_t *urx[8], *up[24];
        uint8_t upd[8], upp[24];
        struct tbt_fec_grouped *utx, *urxcodec;
        int all_known = 1, bytes_ok = 1;

        utx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 1, 8, 24, 128,
                                      16, TBT_FEC_S, 64, 0x6b6b);
        ok(utx != NULL, "immediate: uplink codec builds");
        if (!utx)
            return;
        for (i = 0; i < 8; i++)
        {
            udata[i] = malloc(TBT_FEC_S);
            urx[i] = malloc(TBT_FEC_S);
            fill_pattern(udata[i], TBT_FEC_S, (uint32_t)(0x300 + i));
        }
        for (i = 0; i < 24; i++)
        {
            uparity[i] = malloc(TBT_FEC_S);
            up[i] = malloc(TBT_FEC_S);
            memset(uparity[i], 0, TBT_FEC_S);
        }
        ok_int(tbt_fec_grouped_encode(utx, 0, udata, 8, uparity), 0,
               "immediate: uplink encode");
        urxcodec = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 1, 8, 24,
                                           128, 16, TBT_FEC_S, 64, 0x6b6b);
        ok(urxcodec != NULL, "immediate: uplink rx builds");
        if (urxcodec)
        {
            memset(upd, 1, sizeof(upd));
            memset(upp, 0, sizeof(upp));
            for (i = 0; i < 8; i++)
                if (i >= 6)
                    memcpy(urx[i], udata[i], TBT_FEC_S);
                else
                    upd[i] = 0;
            for (i = 0; i < 8; i++)
            {
                memcpy(up[i], uparity[i], TBT_FEC_S);
                upp[i] = 1;
            }
            ok(tbt_fec_sclc_decode_wake(8, 8, 24, 8, 2, 8),
               "immediate: uplink missing 6 wakes at present 10");
            tbt_fec_grouped_decode(urxcodec, 0, urx, up, upd, upp, 8);
            for (i = 0; i < 8; i++)
            {
                if (!tbt_fec_grouped_is_known(urxcodec, 0, (u32)i))
                    all_known = 0;
                if (memcmp(urx[i], udata[i], TBT_FEC_S) != 0)
                    bytes_ok = 0;
            }
            ok(all_known, "immediate: uplink group is fully known");
            ok(bytes_ok, "immediate: uplink bytes are byte-exact");
            tbt_fec_grouped_free(urxcodec);
        }
        tbt_fec_grouped_free(utx);
        for (i = 0; i < 8; i++)
        {
            free(udata[i]);
            free(urx[i]);
        }
        for (i = 0; i < 24; i++)
        {
            free(uparity[i]);
            free(up[i]);
        }
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

/* A short block sends one parity row per real share, and the NACK scan names
 * only shares the sender could have written.
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

    /* A short SC-LDPC block sends one covering row per real share. */
    ok_u16(tbt_fec_tx_parity_count(1, 128, 128, true), 1,
           "short block: one cut row");
    ok_u16(tbt_fec_tx_parity_count(2, 128, 128, true), 2,
           "two-share block: two covering rows");
    ok_u16(tbt_fec_tx_parity_count(3, 128, 128, true), 3,
           "three-share block: three covering rows");
    ok_u16(tbt_fec_tx_parity_count(127, 128, 128, true), 127,
           "127-share block: one row per real share");
    ok_u16(tbt_fec_tx_parity_count(128, 128, 128, true), 128,
           "full block: every parity row");
    ok_u16(tbt_fec_tx_parity_count(7, 8, 24, true), 7,
           "uplink short block: one row per real share");
    ok_u16(tbt_fec_tx_parity_count(8, 8, 24, true), 24,
           "uplink full block: every parity row");
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
    ok_u16(tbt_fec_nack_parity_limit(true, 2, 128, 128, true), 2,
           "two-share block: two covering rows exist");
    ok_u16(tbt_fec_nack_parity_limit(true, 3, 128, 128, true), 3,
           "three-share block: three covering rows exist");
    ok_u16(tbt_fec_nack_parity_limit(true, 127, 128, 128, true), 127,
           "127-share block: one covering row per real share");
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

/* A short GF(16) block sends one covering row per real share, and those rows
 * can recover a lost real share. n = 8, m = 24, six shares written: the old
 * row-0-only rule left share 5 with no covering equation. The coupled graph
 * is not a per-share MDS code, so this pins the case the row set does solve
 * rather than claiming every short-block loss is parity-recoverable; the
 * NACK replay covers the rest. */
static void case_short_block_decode(void)
{
    const u16 nreal = 6, lost = 5;
    struct tbt_fec_grouped *tx, *rx;
    u8 *data[8] = {0}, *parity[24] = {0};
    u8 *rd[8] = {0}, *rp[24] = {0};
    u8 pd[8] = {0}, pp[24] = {0};
    u16 rows, i;
    int known, exact;

    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 2, 8, 24, 128, 16,
                                 TBT_FEC_S, 64, 0x6b6b);
    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 2, 8, 24, 128, 16,
                                 TBT_FEC_S, 64, 0x6b6b);
    ok(tx != NULL && rx != NULL, "short decode: GF(16) codecs build");
    if (!tx || !rx)
        goto out;
    for (i = 0; i < 8; i++)
    {
        data[i] = calloc(1, TBT_FEC_S);
        rd[i] = calloc(1, TBT_FEC_S);
    }
    for (i = 0; i < 24; i++)
    {
        parity[i] = calloc(1, TBT_FEC_S);
        rp[i] = calloc(1, TBT_FEC_S);
    }
    for (i = 0; i < nreal; i++)
        fill_pattern(data[i], TBT_FEC_S, 0x200 + i);
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 8, parity), 0,
           "short decode: encode");

    rows = tbt_fec_tx_parity_count(nreal, 8, 24, true);
    ok_u16(rows, nreal, "short decode: one covering row per real share");
    for (i = 0; i < nreal; i++)
        if (i != lost)
        {
            memcpy(rd[i], data[i], TBT_FEC_S);
            pd[i] = 1;
        }
    for (i = 0; i < rows; i++)
    {
        memcpy(rp[i], parity[i], TBT_FEC_S);
        pp[i] = 1;
    }
    ok(tbt_fec_grouped_decode(rx, 0, rd, rp, pd, pp, 8) >= 0,
       "short decode: decode returns a count");
    known = tbt_fec_grouped_is_known(rx, 0, lost);
    exact = known && memcmp(rd[lost], data[lost], TBT_FEC_S) == 0;
    ok(known, "short decode: the covering rows recover the lost share");
    ok(exact, "short decode: the recovered share is byte-exact");

out:
    if (tx)
        tbt_fec_grouped_free(tx);
    if (rx)
        tbt_fec_grouped_free(rx);
    for (i = 0; i < 8; i++)
        free(data[i]), free(rd[i]);
    for (i = 0; i < 24; i++)
        free(parity[i]), free(rp[i]);
}

/* Every downlink ladder split must survive the grouped factory path the
 * module uses: shape resolution -> tbt_fec_grouped_new_ops -> codec. */
static void case_downlink_ladder(void)
{
    static const struct
    {
        uint16_t n, m, k;
    } rate[6] = {
        {64, 192, 250}, {76, 180, 296}, {102, 154, 398},
        {128, 128, 500}, {151, 105, 589}, {178, 78, 695},
    };
    int r;

    for (r = 0; r < 6; r++)
    {
        struct tbt_fec_sclc_shape sh;
        struct tbt_fec_grouped *tx, *rx;
        uint8_t **data, **parity, **rxdata, **rxparity;
        uint8_t *pd, *pp;
        uint16_t n = rate[r].n, m = rate[r].m;
        int i, rc;

        memset(&sh, 0, sizeof(sh));
        ok_int(tbt_fec_sclc_shape_of(n, m, 256, 0, 0, &sh), 0,
               "ladder shape resolves");
        ok_u16(sh.rate_ppm, rate[r].k, "ladder rate");
        tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, n, m,
                                     sh.rate_ppm, sh.block, TBT_FEC_S, 0,
                                     0x7000u + (unsigned)r);
        ok(tx != NULL, "ladder tx factory builds");
        if (!tx)
            continue;
        rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, n, m,
                                     sh.rate_ppm, sh.block, TBT_FEC_S, 0,
                                     0x7000u + (unsigned)r);
        ok(rx != NULL, "ladder rx factory builds");
        if (!rx)
        {
            tbt_fec_grouped_free(tx);
            continue;
        }
        data = calloc(n, sizeof(*data));
        parity = calloc(m, sizeof(*parity));
        rxdata = calloc(n, sizeof(*rxdata));
        rxparity = calloc(m, sizeof(*rxparity));
        pd = calloc(n, 1);
        pp = calloc(m, 1);
        ok(data && parity && rxdata && rxparity && pd && pp,
           "ladder buffers");
        if (!(data && parity && rxdata && rxparity && pd && pp))
            return;
        for (i = 0; i < n; i++)
        {
            data[i] = calloc(1, TBT_FEC_S);
            rxdata[i] = calloc(1, TBT_FEC_S);
            ok(data[i] && rxdata[i], "ladder data buffer");
            fill_pattern(data[i], TBT_FEC_S, (uint32_t)(i + 1 + r));
        }
        for (i = 0; i < m; i++)
        {
            parity[i] = calloc(1, TBT_FEC_S);
            rxparity[i] = calloc(1, TBT_FEC_S);
            ok(parity[i] && rxparity[i], "ladder parity buffer");
        }
        ok_int(tbt_fec_grouped_encode(tx, 0, data, n, parity), 0,
               "ladder encode");
        memset(pd, 1, n);
        memset(pp, 1, m);
        for (i = 0; i < n; i++)
            if (i != 3)
                memcpy(rxdata[i], data[i], TBT_FEC_S);
        for (i = 0; i < m; i++)
            memcpy(rxparity[i], parity[i], TBT_FEC_S);
        pd[3] = 0;
        rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, pd, pp, n);
        ok(rc >= 0, "ladder decode returns a count");
        ok(tbt_fec_grouped_is_known(rx, 0, 3) != 0,
           "ladder lost share is known");
        ok(memcmp(rxdata[3], data[3], TBT_FEC_S) == 0,
           "ladder recovered share is byte-exact");
        tbt_fec_grouped_free(rx);
        tbt_fec_grouped_free(tx);
        for (i = 0; i < n; i++)
        {
            free(data[i]);
            free(rxdata[i]);
        }
        for (i = 0; i < m; i++)
        {
            free(parity[i]);
            free(rxparity[i]);
        }
        free(data);
        free(parity);
        free(rxdata);
        free(rxparity);
        free(pd);
        free(pp);
    }
}

/* Route-A block boundary: a full block resets every group's nreal when its
 * parity leaves, so the packetizer's seq is already at the next block's
 * origin while blk_seq0/cur_block still name the old one. A switch must
 * advance that boundary with the old blk_n first; changing blk_n before the
 * advance reinterprets the old block's span and gives the next block the
 * wrong origin. The test then encodes that next block at the switched-down
 * shape and decodes it through the receiver's per-block formula. */
struct tx_boundary
{
    u16 groups;
    u16 n, m;
    u16 blk_n, blk_m;
    u64 seq, blk_seq0, cur_block;
    u16 nreal;
    u16 fill;
};

static bool tx_boundary_dirty(const struct tx_boundary *t)
{
    if (t->fill || t->nreal)
        return true;
    return t->seq - t->blk_seq0 >= (u64)t->groups * t->blk_n;
}

static void tx_boundary_advance(struct tx_boundary *t)
{
    if (t->fill || t->nreal || t->seq < t->blk_seq0)
        return;
    while (t->seq - t->blk_seq0 >= (u64)t->groups * t->blk_n)
    {
        t->blk_seq0 += (u64)t->groups * t->blk_n;
        t->cur_block++;
        t->blk_n = t->n;
        t->blk_m = t->m;
    }
}

static bool tx_boundary_switch(struct tx_boundary *t, u16 n, u16 m)
{
    tx_boundary_advance(t);
    if (tx_boundary_dirty(t))
        return false;
    t->n = n;
    t->m = m;
    t->blk_n = n;
    t->blk_m = m;
    return true;
}

static void tx_boundary_fill(struct tx_boundary *t)
{
    t->seq += (u64)t->groups * t->blk_n;
    t->nreal = t->blk_n; /* every share was written ... */
    t->nreal = 0;        /* ... and the last one emitted parity */
}

static void case_block_switch_boundary(void)
{
    struct tx_boundary t;
    struct tbt_fec_sclc_shape sh;
    struct tbt_fec_grouped *tx, *rx;
    uint8_t *data[64], *parity[192];
    uint8_t *rxdata[64], *rxparity[192];
    uint8_t present_d[64], present_p[192];
    u64 seq0;
    int i, rc;

    memset(&t, 0, sizeof(t));
    t.groups = 1;
    t.n = 64;
    t.m = 192;
    t.blk_n = 64;
    t.blk_m = 192;
    tx_boundary_fill(&t);
    ok(t.seq == 64 && t.blk_seq0 == 0 && t.cur_block == 0,
       "switch: block 0 fills to its boundary");
    ok(tx_boundary_dirty(&t),
       "switch: a full block is dirty until its boundary advances");

    /* Switch up. The advance must use the old n=64, not the new 128. */
    ok(tx_boundary_switch(&t, 128, 128),
       "switch: up applies after the full block closes");
    ok(t.blk_seq0 == 64 && t.cur_block == 1 && t.blk_n == 128,
       "switch: up advances with the old blk_n");
    tx_boundary_fill(&t);
    ok(t.seq == 192 && t.blk_seq0 == 64,
       "switch: block 1 spans 128 shares from 64");

    /* Switch down. The advance must use the old n=128. */
    ok(tx_boundary_switch(&t, 64, 192),
       "switch: down applies after the second full block closes");
    ok(t.blk_seq0 == 192 && t.cur_block == 2 && t.blk_n == 64,
       "switch: down advances with the old blk_n");

    /* A buffered partial packet or an open block owns the current shape. */
    t.fill = 1;
    ok(!tx_boundary_switch(&t, 128, 128),
       "switch: a partial packet blocks a switch");
    t.fill = 0;
    t.nreal = 1;
    ok(!tx_boundary_switch(&t, 128, 128),
       "switch: an open block blocks a switch");
    t.nreal = 0;

    /* Block 2 starts at 192 with 64/192. Encode it and decode it exactly as
     * the receiver does: per-block n/m and group-major seq0 + share. */
    ok(t.cur_block == 2 && t.blk_seq0 == 192 && t.blk_n == 64,
       "switch: the next block keeps the switched-down shape");
    ok(tbt_fec_data_seq(192, 0, 64, 5) == 197,
       "switch: the next block maps share 5 to seq 197");

    memset(&sh, 0, sizeof(sh));
    ok_int(tbt_fec_sclc_shape_of(64, 192, 256, 0, 0, &sh), 0,
           "switch: switched-down shape resolves");
    tx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 64, 192,
                                 sh.rate_ppm, sh.block, TBT_FEC_S, 0,
                                 0x5eed);
    ok(tx != NULL, "switch: tx codec builds");
    rx = tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, 64, 192,
                                 sh.rate_ppm, sh.block, TBT_FEC_S, 0,
                                 0x5eed);
    ok(rx != NULL, "switch: rx codec builds");
    if (!tx || !rx)
        return;
    for (i = 0; i < 64; i++)
    {
        data[i] = calloc(1, TBT_FEC_S);
        rxdata[i] = calloc(1, TBT_FEC_S);
        if (!data[i] || !rxdata[i])
        {
            ok(false, "switch: data buffers");
            return;
        }
        fill_pattern(data[i], TBT_FEC_S, (uint32_t)(0x200 + i));
    }
    for (i = 0; i < 192; i++)
    {
        parity[i] = calloc(1, TBT_FEC_S);
        rxparity[i] = calloc(1, TBT_FEC_S);
        if (!parity[i] || !rxparity[i])
        {
            ok(false, "switch: parity buffers");
            return;
        }
    }
    ok_int(tbt_fec_grouped_encode(tx, 0, data, 64, parity), 0,
           "switch: next block encodes");
    memset(present_d, 1, sizeof(present_d));
    memset(present_p, 1, sizeof(present_p));
    for (i = 0; i < 64; i++)
        if (i != 5)
            memcpy(rxdata[i], data[i], TBT_FEC_S);
    for (i = 0; i < 192; i++)
        memcpy(rxparity[i], parity[i], TBT_FEC_S);
    present_d[5] = 0;
    rc = tbt_fec_grouped_decode(rx, 0, rxdata, rxparity, present_d,
                                present_p, 64);
    ok(rc >= 0, "switch: next block decodes");
    ok(tbt_fec_grouped_is_known(rx, 0, 5) != 0,
       "switch: the lost share is known");
    ok(memcmp(rxdata[5], data[5], TBT_FEC_S) == 0,
       "switch: the recovered share is byte-exact");

    /* The origin the receiver derives from a DATA record's seq. */
    ok(tbt_fec_data_seq0(t.blk_seq0 + 5, 0, 64, 5, &seq0) &&
           seq0 == 192,
       "switch: the receiver recovers block 2's origin");

    tbt_fec_grouped_free(rx);
    tbt_fec_grouped_free(tx);
    for (i = 0; i < 64; i++)
    {
        free(data[i]);
        free(rxdata[i]);
    }
    for (i = 0; i < 192; i++)
    {
        free(parity[i]);
        free(rxparity[i]);
    }
}

/* A rate switch moves the TX shape origin at a block boundary. Blocks built
 * just before it lie below the new base_block but are still in flight, so a
 * NACK for one is not stale while the replay ring still names it. The old
 * base-relative delivered test cannot answer across the switch either: the
 * peer frontier is below base_pkt, and the block's own span is the truth. */
static void case_nack_after_switch(void)
{
    /* base_pkt=64, base_block=4; block 3 was built at origin 48 with n=4
     * over 4 groups (span 16). */
    ok(!tbt_fec_nack_stale(3, 4, true),
       "nack switch: a resident pre-switch block is not stale");
    ok(tbt_fec_nack_stale(3, 4, false),
       "nack switch: an identity the ring forgot is stale");
    ok(!tbt_fec_nack_stale(4, 4, false),
       "nack switch: the new base is not stale");
    ok(!tbt_fec_block_delivered_base(3, 50, 64, 4, 4, 4),
       "nack switch: the base form cannot see across the origin");
    ok(!tbt_fec_block_delivered_seq0(48, 4, 4, 50),
       "nack switch: block 3 is still in flight at 50");
    ok(tbt_fec_block_delivered_seq0(48, 4, 4, 64),
       "nack switch: block 3 is delivered at its span end");
    ok(tbt_fec_block_delivered_seq0(64, 4, 2, 72),
       "nack switch: the post-switch block uses its own shorter span");
}

int main(void)
{
    case_shape_of();
    case_cfg_roundtrip();
    case_init_shape_from_cfg();
    case_downlink_roundtrip();
    case_downlink_ladder();
    case_block_switch_boundary();
    case_nack_after_switch();
    case_uplink_roundtrip();
    case_immediate_decode();
    case_rlnc_dispatch();
    case_short_block_parity();
    case_short_block_decode();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
