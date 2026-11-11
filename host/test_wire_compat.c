/* test_wire_compat.c - cross-version wire refusal regression.
 *
 * The 0.8.0 wire (TBT_CONV 0x000a, TBT_FEC_CONV 0x100b, HELLO v8) is a hard
 * cutover: a peer from 0.7.0 or older must be refused by the first record,
 * never guessed at. This test builds the historical byte vectors and feeds
 * them to the same functions the receive path runs:
 *
 *   0.1.x legacy    TBT_CONV 0x0005, FEC 0x1001..0x1003, HELLO v1
 *   0.2.x legacy    TBT_CONV 0x0006, FEC 0x1004/0x1005, HELLO v1/v2
 *   0.3.x           TBT_CONV 0x0006, FEC 0x1006, HELLO v3, empty OPEN body
 *   future          HELLO v99 and unknown FEC flags/types
 *
 * Every vector must fail explicitly (a negative errno), and a control vector
 * built with the current constants must pass, so the test measures the
 * revision check rather than a parser that refuses everything. Buffers are
 * allocated at exactly the historical length: ASan turns a read past a
 * 40-byte FEC v0.2.4 header or a short OPEN body into a test failure.
 *
 *   make host-test
 *   make host-test-san
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_rec.h"

static int failures;
static int checks;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "",
           detail ? detail : "");
}

static void *xalloc(size_t len)
{
    void *p = malloc(len ? len : 1);

    if (!p)
    {
        fprintf(stderr, "test_wire_compat: out of memory\n");
        exit(2);
    }
    memset(p, 0, len ? len : 1);
    return p;
}

static void fill_pattern(u8 *p, size_t len, u32 seed)
{
    u32 x = seed * 2654435761u + 0x9e3779b9u;
    size_t i;

    for (i = 0; i < len; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (u8)(x >> 24);
    }
}

/* ------------------------------------------------------------------ *
 * Legacy (32-byte) record framing, TBT_CONV 0x0005/0x0006.
 * ------------------------------------------------------------------ */

static u8 *legacy_record(u8 type, u16 conv, const u8 *body, u16 body_len)
{
    struct tbt_hdr h;
    u8 *rec = xalloc(TBT_HDR_LEN + body_len);

    memset(&h, 0, sizeof(h));
    h.type = type;
    h.conv = conv;
    h.body_len = body_len;
    tbt_hdr_encode(rec, &h);
    if (body_len)
        memcpy(rec + TBT_HDR_LEN, body, body_len);
    return rec;
}

static void expect_legacy_proto(u8 type, u16 conv, const u8 *body,
                                u16 body_len, const char *what)
{
    struct tbt_rec rec;
    size_t len = (size_t)TBT_HDR_LEN + body_len;
    int err = -1;
    u8 *raw = legacy_record(type, conv, body, body_len);

    memset(&rec, 0, sizeof(rec));
    ok(tbt_rec_scan(raw, len, &rec, &err) == 0 && err == TBT_REC_PROTO,
       what, "legacy record was not refused at framing");
    free(raw);
}

static void expect_legacy_ok(u8 type, u16 conv, const u8 *body, u16 body_len,
                             const char *what)
{
    struct tbt_rec rec;
    size_t len = (size_t)TBT_HDR_LEN + body_len;
    int err = -1;
    u8 *raw = legacy_record(type, conv, body, body_len);

    memset(&rec, 0, sizeof(rec));
    ok(tbt_rec_scan(raw, len, &rec, &err) == len && err == TBT_REC_OK,
       what, "current-layout record was refused");
    free(raw);
}

/* ------------------------------------------------------------------ *
 * HELLO body gate. tbt_rec_scan() bounds the body; tbt_hello_admit() is the
 * exact session gate tbt_rx_hello() runs. A historical HELLO must pass the
 * former and fail the latter, never the reverse.
 * ------------------------------------------------------------------ */

static u8 *hello_record(u8 version, u16 body_len, u8 role)
{
    u8 *body = xalloc(body_len);
    u8 *rec;

    body[0] = version;
    body[1] = role;
    if (body_len >= TBT_HELLO_BODY_LEN)
    {
        /* The fields a current peer also validates, so the vector differs
         * from a valid HELLO only in the version byte. */
        body[18] = (u8)(TBT_PAYLOAD_MAX & 0xff);
        body[19] = (u8)(TBT_PAYLOAD_MAX >> 8);
        body[20] = (u8)(TBT_CHUNK_MAX & 0xff);
        body[21] = (u8)(TBT_CHUNK_MAX >> 8);
        body[22] = 1; /* http-kal profile, generation 0 */
    }
    rec = legacy_record(TBT_F_HELLO, TBT_CONV, body, body_len);
    free(body);
    return rec;
}

static void expect_hello_refused(u8 version, u16 body_len, const char *what)
{
    struct tbt_rec rec;
    size_t len = (size_t)TBT_HDR_LEN + body_len;
    int err = -1;
    u8 *raw = hello_record(version, body_len, TBT_ROLE_FAR);

    memset(&rec, 0, sizeof(rec));
    ok(tbt_rec_scan(raw, len, &rec, &err) == len && err == TBT_REC_OK,
       what, "framing refused the HELLO before the version gate");
    ok(!tbt_hello_admit(true, rec.body_len, rec.body, TBT_ROLE_FAR),
       what, "0.4.0 FEC HELLO gate accepted a historical version");
    free(raw);
}

static void expect_hello_ok(u8 version, u16 body_len, bool fec, u8 want_role,
                            const char *what)
{
    struct tbt_rec rec;
    size_t len = (size_t)TBT_HDR_LEN + body_len;
    int err = -1;
    u8 *raw = hello_record(version, body_len, want_role);

    memset(&rec, 0, sizeof(rec));
    ok(tbt_rec_scan(raw, len, &rec, &err) == len && err == TBT_REC_OK,
       what, "current HELLO failed framing");
    ok(tbt_hello_admit(fec, rec.body_len, rec.body, want_role), what,
       "current HELLO failed the version gate");
    free(raw);
}

/* ------------------------------------------------------------------ *
 * FEC (48-byte) records.
 * ------------------------------------------------------------------ */

static void fec_current_header(struct tbt_fec_hdr *h, u8 type, u16 conv)
{
    memset(h, 0, sizeof(*h));
    h->type = type;
    h->conv = conv;
    h->sid = 7;

    switch (type)
    {
    case TBT_FEC_DATA:
        h->n = 8;
        h->m = 8;
        h->share_index = 0;
        h->payload_len = 100;
        break;
    case TBT_FEC_PARITY:
        h->n = 8;
        h->m = 8;
        h->share_index = 8;
        h->seq = 8;
        h->win_len = 8;
        h->payload_len = TBT_FEC_PAYLOAD_MAX;
        break;
    case TBT_FEC_ACK:
        h->seq = 11;
        h->payload_len = TBT_FEC_ACK_LEN;
        break;
    case TBT_FEC_NACK:
        h->block_id = 5;
        h->payload_len = TBT_FEC_NACK_LEN;
        break;
    case TBT_FEC_CFG:
        h->payload_len = TBT_FEC_CFG_LEN;
        break;
    case TBT_FEC_FBK:
        h->payload_len = TBT_FEC_FBK_LEN;
        break;
    default:
        break;
    }
}

/* Seal and pack a header with a payload of exactly payload_len bytes. The
 * record is allocated at exactly that length, so the sanitizer catches a
 * receiver that reads farther than the record it was given. */
static u8 *fec_record(const struct tbt_fec_hdr *in, const u8 *payload)
{
    struct tbt_fec_hdr h = *in;
    u8 *rec = xalloc((size_t)TBT_FEC_HDR_LEN + h.payload_len);

    if (tbt_fec_hdr_seal(&h, payload))
    {
        fprintf(stderr, "test_wire_compat: cannot seal record\n");
        exit(2);
    }
    tbt_fec_hdr_pack(&h, rec);
    if (h.payload_len)
        memcpy(rec + TBT_FEC_HDR_LEN, payload, h.payload_len);
    return rec;
}

static void expect_fec_rc(const u8 *rec, size_t len, int want,
                          const char *what)
{
    struct tbt_fec_hdr h;
    int rc;

    memset(&h, 0, sizeof(h));
    rc = tbt_fec_rec_check(rec, len, TBT_FEC_S, &h);
    ok(rc == want, what, "FEC record got the wrong refusal");
}

/* Historical FEC headers kept their type and body shape from M5/M6 onward,
 * so one current-valid header with only conv rewritten is the strongest
 * vector: any accepted field or CRC other than conv would let it through. */
static void expect_historical_fec(u16 conv, u8 type)
{
    struct tbt_fec_hdr h;
    struct tbt_fec_hdr parsed;
    u8 *payload;
    u8 *rec;
    char what[96];

    fec_current_header(&h, type, conv);
    payload = xalloc(h.payload_len ? h.payload_len : 1);
    fill_pattern(payload, h.payload_len, type);
    rec = fec_record(&h, payload);

    snprintf(what, sizeof(what), "FEC 0x%04x type 0x%02x is refused", conv,
             type);
    memset(&parsed, 0, sizeof(parsed));
    ok(tbt_fec_hdr_unpack(rec, &parsed) == 0, what,
       "historical header could not be unpacked");
    ok(tbt_fec_hdr_check(&parsed, TBT_FEC_S) != 0, what,
       "structural check accepted a historical conv");
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, -EPROTO, what);
    free(rec);
    free(payload);
}

static void expect_current_fec_ok(u8 type, const char *what)
{
    struct tbt_fec_hdr h;
    u8 *payload;
    u8 *rec;

    fec_current_header(&h, type, TBT_FEC_CONV);
    payload = xalloc(h.payload_len ? h.payload_len : 1);
    fill_pattern(payload, h.payload_len, type);
    rec = fec_record(&h, payload);
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, 0, what);
    free(rec);
    free(payload);
}

/* ------------------------------------------------------------------ *
 * Control bodies.
 * ------------------------------------------------------------------ */

static void cfg_body(u8 out[TBT_FEC_CFG_LEN])
{
    struct tbt_fec_cfg c;

    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = 8;
    c.m = 8;
    c.w = 8;
    c.k = 8;
    c.d = 1;
    c.flags = TBT_FEC_CFG_F_APPLY;
    c.groups = 16;
    c.switch_pkt = 10;
    c.block_base = 2;
    c.seed = 0x1234;
    tbt_fec_cfg_pack(&c, out);
}

static void fbk_body(u8 out[TBT_FEC_FBK_LEN])
{
    struct tbt_fec_fbk f;

    memset(&f, 0, sizeof(f));
    f.epoch_lo = 1;
    f.m_current = 8;
    f.m_target = 8;
    f.flags = TBT_FEC_FBK_F_P_VALID;
    f.n_current = 8;
    tbt_fec_fbk_pack(&f, out);
}

static void test_legacy_framing(void)
{
    u8 hello23[TBT_HELLO_BODY_LEN];
    u8 data[TBT_PAYLOAD_MAX];
    u8 open[TBT_OPEN_BODY_LEN];
    u8 fin[TBT_FIN_BODY_LEN];
    u8 *raw;

    memset(hello23, 0, sizeof(hello23));
    memset(data, 0, sizeof(data));
    memset(open, 0, sizeof(open));
    memset(fin, 0, sizeof(fin));

    /* 0.1.x and 0.2.x/0.3.x record revisions. The body is otherwise what the
     * current framing layer accepts, so only the conv can be the refusal. */
    expect_legacy_proto(TBT_F_HELLO, 0x0005, hello23, sizeof(hello23),
                        "TBT_CONV 0x0005 HELLO is refused");
    expect_legacy_proto(TBT_F_HELLO, 0x0006, hello23, sizeof(hello23),
                        "TBT_CONV 0x0006 HELLO is refused");
    expect_legacy_proto(TBT_F_DATA, 0x0005, data, 64,
                        "TBT_CONV 0x0005 DATA is refused");
    expect_legacy_proto(TBT_F_DATA, 0x0006, data, 64,
                        "TBT_CONV 0x0006 DATA is refused");
    expect_legacy_proto(TBT_F_OPEN, 0x0006, NULL, 0,
                        "0.3.x empty OPEN is refused");
    expect_legacy_proto(TBT_F_FIN, 0x0005, NULL, 0,
                        "0.1.x empty FIN is refused");

    /* Current framing still accepts the current layouts; this is the guard
     * against a framing test that would pass by refusing everything. */
    expect_legacy_ok(TBT_F_OPEN, TBT_CONV, open, sizeof(open),
                     "0.4.0 OPEN bitmap passes framing");
    expect_legacy_ok(TBT_F_FIN, TBT_CONV, fin, sizeof(fin),
                     "0.4.0 FIN body passes framing");

    /* 0.3.x OPEN carried no body. Every truncation below 32 bytes is refused;
     * the buffers are exact, so a checker that read a whole bitmap from a
     * one-byte body would trip ASan here. */
    {
        char what[80];
        u16 len;

        for (len = 0; len < TBT_OPEN_BODY_LEN; len++)
        {
            snprintf(what, sizeof(what),
                     "short OPEN body (%u of %u bytes) is refused", len,
                     TBT_OPEN_BODY_LEN);
            expect_legacy_proto(TBT_F_OPEN, TBT_CONV, open, len, what);
        }
    }

    /* Removed placeholder types and reserved header fields. */
    expect_legacy_proto(0x11, TBT_CONV, NULL, 0,
                        "removed PREDICT type is refused");
    expect_legacy_proto(0x12, TBT_CONV, NULL, 0,
                        "removed CANCEL type is refused");

    raw = legacy_record(TBT_F_HELLO, TBT_CONV, hello23, sizeof(hello23));
    raw[TBT_H_RES2] = 1;
    {
        struct tbt_rec rec;
        int err = -1;

        ok(tbt_rec_scan(raw, TBT_HDR_LEN + sizeof(hello23), &rec, &err) == 0 &&
               err == TBT_REC_PROTO,
           "nonzero legacy reserved2 is refused", "scan accepted it");
    }
    free(raw);

    raw = legacy_record(TBT_F_HELLO, TBT_CONV, hello23, sizeof(hello23));
    raw[TBT_H_FLAGS] = 0x40;
    {
        struct tbt_rec rec;
        int err = -1;

        ok(tbt_rec_scan(raw, TBT_HDR_LEN + sizeof(hello23), &rec, &err) == 0 &&
               err == TBT_REC_PROTO,
           "unknown legacy flag bit is refused", "scan accepted it");
    }
    free(raw);

    raw = legacy_record(TBT_F_HELLO, TBT_CONV, hello23, sizeof(hello23));
    raw[TBT_H_ACKC] = TBT_LEGACY_MAX_CARRIERS + 1;
    {
        struct tbt_rec rec;
        int err = -1;

        ok(tbt_rec_scan(raw, TBT_HDR_LEN + sizeof(hello23), &rec, &err) == 0 &&
               err == TBT_REC_PROTO,
           "legacy path_ack_count above 16 is refused", "scan accepted it");
    }
    free(raw);
}

static void test_hello_versions(void)
{
    ok(TBT_CONV == 0x000au && TBT_FEC_CONV == 0x100bu &&
           TBT_WIRE_VERSION_V6 == 8,
       "0.8.0 wire constants", "the current constants are not the v11 cut");

    expect_hello_refused(1, TBT_HELLO_BODY_LEN, "HELLO v1 (0.1.x/0.2.x ARQ)");
    expect_hello_refused(2, TBT_HELLO_BODY_LEN_V6, "HELLO v2 (0.2.x FEC)");
    expect_hello_refused(3, TBT_HELLO_BODY_LEN_V6, "HELLO v3 (0.3.x FEC)");
    expect_hello_refused(4, TBT_HELLO_BODY_LEN_V6, "HELLO v4 (0.4.0 FEC)");
    expect_hello_refused(5, TBT_HELLO_BODY_LEN_V6, "HELLO v5 (0.5.0 FEC)");
    expect_hello_refused(6, TBT_HELLO_BODY_LEN_V6, "HELLO v6 (0.6.0 FEC)");
    expect_hello_refused(7, TBT_HELLO_BODY_LEN_V6, "HELLO v7 (0.7.0 FEC)");
    expect_hello_refused(99, TBT_HELLO_BODY_LEN_V6, "HELLO v99 (future)");

    expect_hello_ok(TBT_WIRE_VERSION_V6, TBT_HELLO_BODY_LEN_V6, true,
                    TBT_ROLE_FAR, "HELLO v8 passes the FEC gate");
    expect_hello_ok(TBT_WIRE_VERSION, TBT_HELLO_BODY_LEN, false,
                    TBT_ROLE_FAR, "HELLO v1 still passes the ARQ gate");

    /* Wrong role and a body outside the framing bounds are refused before
     * the version gate can see them. */
    {
        struct tbt_rec rec;
        u8 *raw = hello_record(TBT_WIRE_VERSION_V6, TBT_HELLO_BODY_LEN_V6,
                               TBT_ROLE_FAR);
        int err = -1;

        ok(tbt_rec_scan(raw, TBT_HDR_LEN + TBT_HELLO_BODY_LEN_V6, &rec,
                        &err) == TBT_HDR_LEN + TBT_HELLO_BODY_LEN_V6 &&
               err == TBT_REC_OK,
           "HELLO v7 framing passes", "scan refused it");
        ok(!tbt_hello_admit(true, rec.body_len, rec.body, TBT_ROLE_NEAR),
           "HELLO v7 with the wrong role is refused", "gate accepted it");
        free(raw);
    }

    {
        u8 body[TBT_HELLO_BODY_LEN_V6 + 1];
        memset(body, 0, sizeof(body));
        body[0] = TBT_WIRE_VERSION_V6;
        body[1] = TBT_ROLE_FAR;
        expect_legacy_proto(TBT_F_HELLO, TBT_CONV, body,
                            TBT_HELLO_BODY_LEN - 1,
                            "one-byte-short legacy HELLO is refused");
        expect_legacy_proto(TBT_F_HELLO, TBT_CONV, body,
                            TBT_HELLO_BODY_LEN_V6 + 1,
                            "one-byte-long HELLO is refused");
    }
}

static void test_hello_crypto(void)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];

    memset(body, 0, sizeof(body));
    body[0] = TBT_WIRE_VERSION_V6;
    body[1] = TBT_ROLE_FAR;
    body[18] = (u8)(TBT_PAYLOAD_MAX & 0xff);
    body[19] = (u8)(TBT_PAYLOAD_MAX >> 8);
    body[20] = (u8)(TBT_CHUNK_MAX & 0xff);
    body[21] = (u8)(TBT_CHUNK_MAX >> 8);
    body[22] = 1;
    ok(tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) == 0,
       "HELLO v7 crypto fields, all off, pass", "parse refused them");

    body[TBT_CRYPTO_HELLO_RESERVED] = 1;
    ok(tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) ==
           -EPROTO,
       "nonzero HELLO crypto reserved byte is refused", "parse accepted it");
    body[TBT_CRYPTO_HELLO_RESERVED] = 0;

    body[TBT_CRYPTO_HELLO_CRYPTO] = 0x04;
    ok(tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) ==
           -EPROTO,
       "unknown HELLO crypto flag bit is refused", "parse accepted it");
    body[TBT_CRYPTO_HELLO_CRYPTO] = 0;

    body[TBT_CRYPTO_HELLO_KEY_ID] = TBT_CRYPTO_KEY_ID_HELLO;
    ok(tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) ==
           -EPROTO,
       "key_id=1 without the key-in-HELLO bit is refused",
       "parse accepted the mismatch");
    body[TBT_CRYPTO_HELLO_KEY_ID] = 0;

    ok(tbt_crypto_hello_parse(body, TBT_CRYPTO_HELLO_LEN - 1, NULL, NULL, NULL,
                              NULL) == -EPROTO,
       "truncated HELLO v7 crypto body is refused", "parse accepted it");
}

static void test_fec_headers(void)
{
    static const u8 types[] = {TBT_FEC_DATA, TBT_FEC_PARITY, TBT_FEC_CFG,
                               TBT_FEC_FBK, TBT_FEC_NACK};
    static const u16 old_convs[] = {0x1005, 0x1006, 0x1009, 0x100a};
    size_t i, j;

    for (j = 0; j < sizeof(old_convs) / sizeof(old_convs[0]); j++)
        for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            expect_historical_fec(old_convs[j], types[i]);

    expect_current_fec_ok(TBT_FEC_DATA, "0.8.0 DATA header passes");
    expect_current_fec_ok(TBT_FEC_PARITY, "0.8.0 PARITY header passes");
    expect_current_fec_ok(TBT_FEC_ACK, "0.8.0 ACK header passes");
    expect_current_fec_ok(TBT_FEC_NACK, "0.8.0 NACK header passes");
    expect_current_fec_ok(TBT_FEC_CFG, "0.8.0 CFG header passes");
    expect_current_fec_ok(TBT_FEC_FBK, "0.8.0 FBK header passes");
}

static void test_fec_short_records(void)
{
    struct tbt_fec_hdr h;
    u8 *payload;
    u8 *rec;
    u8 short40[40];
    u8 padded48[TBT_FEC_HDR_LEN];

    /* 0.2.0..0.2.1 header was 40 bytes: +32 payload CRC, +36 header CRC, no
     * sid. The record buffer is exactly 40 bytes, so the gate must reject on
     * length before tbt_fec_hdr_unpack() reads +40..+47. */
    memset(short40, 0, sizeof(short40));
    short40[TBT_FEC_H_TYPE] = TBT_FEC_DATA;
    short40[TBT_FEC_H_N] = 8;
    short40[TBT_FEC_H_M] = 8;
    short40[TBT_FEC_H_PAYLEN] = 100;
    short40[28] = 0x04;
    short40[29] = 0x10; /* FEC conv 0x1004 */
    expect_fec_rc(short40, sizeof(short40), -EPROTO,
                  "40-byte FEC 0x1004 header is refused without a read past 40");

    /* The same 40 bytes inside a 48-byte buffer still fail on conv, which
     * proves the length gate was not hiding a conv path that accepts it. */
    memset(padded48, 0, sizeof(padded48));
    memcpy(padded48, short40, sizeof(short40));
    expect_fec_rc(padded48, sizeof(padded48), -EPROTO,
                  "40-byte FEC 0x1004 header padded to 48 is still refused");

    fec_current_header(&h, TBT_FEC_DATA, TBT_FEC_CONV);
    payload = xalloc(h.payload_len);
    fill_pattern(payload, h.payload_len, 0x40);
    rec = fec_record(&h, payload);

    expect_fec_rc(rec, TBT_FEC_HDR_LEN - 1, -EPROTO,
                  "47-byte truncated FEC record is refused");
    expect_fec_rc(rec, TBT_FEC_HDR_LEN, 0,
                  "header-only FEC record, no payload read, is admitted");
    free(rec);
    free(payload);
}

static void test_fec_tamper(void)
{
    struct tbt_fec_hdr h;
    u8 *payload;
    u8 *rec;

    fec_current_header(&h, TBT_FEC_DATA, TBT_FEC_CONV);
    payload = xalloc(h.payload_len);
    fill_pattern(payload, h.payload_len, 0x55);
    rec = fec_record(&h, payload);

    rec[TBT_FEC_H_RESERVED] = 1;
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, -EPROTO,
                  "DATA group that underflows block_seq0 is refused");
    rec[TBT_FEC_H_RESERVED] = 0;

    /* +30 is the group for DATA/PARITY. A group that fits the block is
     * legal; a control record must still keep it zero. */
    {
        struct tbt_fec_hdr gh;
        u8 *gpay, *grec;

        fec_current_header(&gh, TBT_FEC_DATA, TBT_FEC_CONV);
        gh.reserved = 1;
        gh.seq = gh.n; /* block_seq0 = 0 */
        gpay = xalloc(gh.payload_len);
        fill_pattern(gpay, gh.payload_len, 0x30);
        grec = fec_record(&gh, gpay);
        expect_fec_rc(grec, TBT_FEC_HDR_LEN + gh.payload_len, 0,
                      "DATA group 1 with block_seq0=0 passes");
        free(grec);
        free(gpay);

        fec_current_header(&gh, TBT_FEC_ACK, TBT_FEC_CONV);
        gh.reserved = 1;
        gpay = xalloc(gh.payload_len);
        fill_pattern(gpay, gh.payload_len, 0x31);
        grec = fec_record(&gh, gpay);
        expect_fec_rc(grec, TBT_FEC_HDR_LEN + gh.payload_len, -EPROTO,
                      "control record with a nonzero group is refused");
        free(grec);
        free(gpay);
    }

    rec[TBT_FEC_H_FLAGS] = 0x02;
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, -EPROTO,
                  "unknown FEC header flag bit is refused");
    rec[TBT_FEC_H_FLAGS] = 0;

    rec[TBT_FEC_H_SEQ] ^= 0x01;
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, -EBADMSG,
                  "tampered FEC header CRC is refused");
    rec[TBT_FEC_H_SEQ] ^= 0x01;

    rec[TBT_FEC_HDR_LEN] ^= 0xff;
    expect_fec_rc(rec, TBT_FEC_HDR_LEN + h.payload_len, 0,
                  "payload tamper leaves the header gate intact");
    {
        struct tbt_fec_hdr parsed;

        ok(tbt_fec_hdr_unpack(rec, &parsed) == 0 &&
               tbt_fec_hdr_verify(&parsed, rec + TBT_FEC_HDR_LEN) == -EBADMSG,
           "tampered FEC payload CRC is refused",
           "payload verify accepted the change");
    }
    free(rec);
    free(payload);
}

static void test_control_bodies(void)
{
    u8 cfg[TBT_FEC_CFG_LEN];
    u8 fbk[TBT_FEC_FBK_LEN];
    struct tbt_fec_cfg c;

    cfg_body(cfg);
    ok(tbt_fec_cfg_unpack(cfg, TBT_FEC_MAX_CARRIERS, &c) == 0,
       "current FEC_CFG body passes",
       "cfg unpack refused it");

    cfg[4] ^= 0x01;
    ok(tbt_fec_cfg_unpack(cfg, TBT_FEC_MAX_CARRIERS, &c) == -EPROTO,
       "tampered FEC_CFG body CRC is refused", "cfg unpack accepted it");
    cfg[4] ^= 0x01;

    cfg[TBT_FEC_CFG_RESERVED] = 1;
    ok(tbt_fec_cfg_unpack(cfg, TBT_FEC_MAX_CARRIERS, &c) == -EPROTO,
       "nonzero FEC_CFG reserved is refused", "cfg unpack accepted it");
    cfg[TBT_FEC_CFG_RESERVED] = 0;

    cfg_body(cfg); /* restore the CRC, then set an unknown flag */
    cfg[TBT_FEC_CFG_FLAGS] = 0x08;
    tbt_put_le32(cfg + TBT_FEC_CFG_CRC,
                 tbt_fec_crc32c(0, cfg, TBT_FEC_CFG_CRC_BYTES));
    ok(tbt_fec_cfg_unpack(cfg, TBT_FEC_MAX_CARRIERS, &c) == -EPROTO,
       "unknown FEC_CFG flag is refused", "cfg unpack accepted it");

    fbk_body(fbk);
    fbk[TBT_FEC_FBK_FLAGS] = 0x20;
    {
        struct tbt_fec_fbk f;

        ok(tbt_fec_fbk_unpack(fbk, &f) == -EPROTO,
           "unknown FEC_FBK flag is refused", "fbk unpack accepted it");
    }
}

int main(void)
{
    test_legacy_framing();
    test_hello_versions();
    test_hello_crypto();
    test_fec_headers();
    test_fec_short_records();
    test_fec_tamper();
    test_control_bodies();

    if (failures)
    {
        printf("test_wire_compat: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_wire_compat: %d checks passed\n", checks);
    return 0;
}
