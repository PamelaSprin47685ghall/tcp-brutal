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

int main(void)
{
    case_shape_of();
    case_cfg_roundtrip();
    case_downlink_roundtrip();
    case_uplink_roundtrip();
    case_rlnc_dispatch();
    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
