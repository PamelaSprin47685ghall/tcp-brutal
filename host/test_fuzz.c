/* test_fuzz.c - hostile-input tests for the peer-controlled parsers:
 * tbt_rec_scan() (record framing), tbt_hdr_check() (header validation),
 * tbt_fec_alloc_unpack()/tbt_fec_cfg_unpack() (0.4.0 v7 control bodies) and
 * tbt_parse_line() (the /proc config grammar).
 *
 * Deterministic: xorshift64 seeded with 1, no fixtures, no external
 * dependencies. It links tbtun/tbt_rec.c and tbtun/tbt_proto.c, the same
 * objects the module is built from, so a rule that passes here is a rule the
 * kernel enforces.
 *
 * What is asserted:
 *   - 200k random buffers, each fed through a copy of the per-carrier
 *     accumulation loop with two random split sizes (1..4096) and one-shot;
 *     every split must produce the same records, the same consumed count and
 *     the same verdict. The same bytes are also scanned from an exact-size
 *     heap copy so that -fsanitize=address sees any read past `len`.
 *   - 200k random u16 body lengths: a length above TBT_PAYLOAD_MAX is always
 *     PROTO, a truncated body is always NEED_MORE, and no call ever consumes
 *     more than it was given.
 *   - an adversarial set: body_len extremes, type/length and flags/type
 *     contradictions, seq wrap, a sid mismatch across OPEN/DATA, truncated
 *     FBK/HELLO/PAD/WINDOW records.
 *   - a header matrix: every type against every body length
 *     0..TBT_PAYLOAD_MAX, with the accepted set compared against DESIGN.md
 *     §3.3's bounds, plus one accept/reject pair per range rule (path_id,
 *     path_ack_count, path_ack_mask, flags, sid_hi, reserved2, conv).
 *   - 20k mutated FEC_ALLOC bodies: random mask flips, all-zero/all-one
 *     masks, every flag value, reserved bytes and reserved2, CRC faults and
 *     a full pack/unpack round trip for every accepted body.
 *   - 40k mutated FEC_CFG bodies seeded from the RLNC, SC-LDPC binary and
 *     SC-LDPC GF(16) profiles, bombarding n=0, m=0, n+m=65536, n+m>256 and
 *     groups=0; every verdict is compared with an independent reading of the
 *     documented rules, and every accepted body must repack byte for byte.
 *   - 8k mutated /proc lines aimed at portblock=, adaptive=, L= and
 *     mode=sc-ldpc, plus a canonical render/parse fixed-point check on every
 *     accepted tunnel line.
 *
 *   make host-test          # normal build
 *   make host-test-san      # the same, with ASan and UBSan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "tbt_rec.h"
#include "tbt_fec.h"
#include "tbt_parse.h"

#define FUZZ_BUFFERS 200000
#define FUZZ_BODY_LENGTHS 200000

/* Documented body sizes (DESIGN.md §3.3). Repeated as literals on purpose:
 * the test must fail if the header's constants drift from the design. */
#define DOC_DATA_MAX 8192u
#define DOC_FBK_ENTRY 18u
#define DOC_FBK_MAX_ENTRIES 16u
#define DOC_WINDOW_LEN 138u /* 8 + 8*16 + 2 */
#define DOC_HELLO_LEN 23u   /* old ARQ: 1 + 1 + 16 + 2 + 2 + 1 */
#define DOC_HELLO_LEN_V6 66u /* FEC v6: legacy 23 + crypto fields */
#define DOC_OPEN_BODY_LEN 32u /* 0.4.0: 256-bit physical group bitmap */

static u64 rng_state = 1;
static unsigned long case_no;
static long checks;
static int failures;

static u64 rnd64(void)
{
    u64 x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static size_t rnd_below(size_t n)
{
    return n ? (size_t)(rnd64() % n) : 0;
}

static const char *st_name(int st)
{
    switch (st)
    {
    case TBT_REC_OK:
        return "OK";
    case TBT_REC_NEED_MORE:
        return "NEED_MORE";
    case TBT_REC_PROTO:
        return "PROTO";
    default:
        return "?";
    }
}

static void check(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    if (detail && *detail)
        printf("FAIL seed=1 case=%lu: %s: %s\n", case_no, what, detail);
    else
        printf("FAIL seed=1 case=%lu: %s\n", case_no, what);
}

/* One record, written straight into the wire offsets (never through
 * tbt_hdr_encode, so a codec bug cannot hide itself). The body is left
 * untouched: tbt_rec_scan() must not read it. */
static size_t rec_make(u8 *dst, u8 type, u8 flags, u8 ackc, u32 sid, u64 seq, u16 len)
{
    memset(dst, 0, TBT_HDR_LEN);
    dst[TBT_H_TYPE] = type;
    dst[TBT_H_FLAGS] = flags;
    dst[TBT_H_ACKC] = ackc;
    tbt_put_le16(dst + TBT_H_BODYLEN, len);
    tbt_put_le64(dst + TBT_H_SEQ, seq);
    tbt_put_le32(dst + TBT_H_SIDLO, sid);
    tbt_put_le16(dst + TBT_H_CONV, TBT_CONV);
    return TBT_HDR_LEN + (size_t)len;
}

/* Scan a heap copy of exactly `len` bytes: under ASan any read past the given
 * length is a failure, not a silent pass inside a larger static buffer. The
 * body pointer in *rec points into the freed copy; callers only read fields. */
static size_t scan_copy(const u8 *buf, size_t len, struct tbt_rec *rec, int *st)
{
    u8 *copy = malloc(len ? len : 1);
    size_t used;

    if (!copy)
    {
        printf("FAIL seed=1 case=%lu: out of memory\n", case_no);
        exit(2);
    }
    memcpy(copy, buf, len);
    used = tbt_rec_scan(copy, len, rec, st);
    free(copy);
    return used;
}

static void expect(const char *what, const u8 *buf, size_t len, int want)
{
    struct tbt_rec rec;
    int st = -1;
    size_t used = scan_copy(buf, len, &rec, &st);

    checks++;
    if (st != want)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: want %s, got %s\n", case_no, what, st_name(want), st_name(st));
        return;
    }
    checks++;
    if (want == TBT_REC_OK)
    {
        if (used <= len && used == (size_t)(TBT_HDR_LEN + rec.body_len))
            return;
        failures++;
        printf("FAIL seed=1 case=%lu: %s: consumed %zu of %zu, body_len %u\n", case_no, what, used, len,
               rec.body_len);
    }
    else if (used != 0)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: refused but consumed %zu\n", case_no, what, used);
    }
}

/* The accepted set DESIGN.md §3.3 documents, independent of the codec. */
static int doc_accepts(u8 type, u8 ackc, unsigned len)
{
    if (len > DOC_DATA_MAX)
        return 0;
    switch (type)
    {
    case TBT_F_DATA:
        return len >= 1;
    case TBT_F_OPEN:
        /* 0.4.0: exactly the 32-byte physical group bitmap. A non-FEC
         * session sends 32 zero bytes, so one length rule covers both. */
        return len == DOC_OPEN_BODY_LEN;
    case TBT_F_RST:
    case TBT_F_PAD:
        return len == 0;
    case TBT_F_FIN:
        /* 0.2.0: pkt_end at +0, pkt_base at +8, exactly 16 bytes. */
        return len == TBT_FIN_BODY_LEN;
    case TBT_F_RETIRE:
        return len == 1; /* u8 carrier index; seq is the generation */
    case TBT_F_SACK:
        return len == 1; /* u8 state */
    case TBT_F_FBK:
        return ackc <= DOC_FBK_MAX_ENTRIES && len == (unsigned)ackc * DOC_FBK_ENTRY;
    case TBT_F_WINDOW:
        return len == DOC_WINDOW_LEN;
    case TBT_F_HELLO:
        /* The framing layer accepts both the old 23-byte body and the FEC v6
         * 66-byte body; tbt_rx_hello() enforces the session's exact length. */
        return len >= DOC_HELLO_LEN && len <= DOC_HELLO_LEN_V6;
    default:
        return 0;
    }
}

static const u8 all_types[] = {
    TBT_F_DATA, TBT_F_OPEN, TBT_F_FIN,   TBT_F_RST,    TBT_F_FBK,
    TBT_F_WINDOW, TBT_F_HELLO, TBT_F_PAD, TBT_F_RETIRE, TBT_F_SACK,
};

/* The bounds table must say exactly what the design says. */
static void test_bounds_table(void)
{
    static const struct
    {
        u8 type;
        u16 min, max;
    } want[] = {
        { TBT_F_DATA, 1, DOC_DATA_MAX },
        { TBT_F_OPEN, DOC_OPEN_BODY_LEN, DOC_OPEN_BODY_LEN },
        { TBT_F_FIN, TBT_FIN_BODY_LEN, TBT_FIN_BODY_LEN },
        { TBT_F_RST, 0, 0 },
        { TBT_F_FBK, 0, DOC_FBK_ENTRY * DOC_FBK_MAX_ENTRIES },
        { TBT_F_WINDOW, DOC_WINDOW_LEN, DOC_WINDOW_LEN },
        { TBT_F_HELLO, DOC_HELLO_LEN, DOC_HELLO_LEN_V6 },
        { TBT_F_PAD, 0, 0 },
        { TBT_F_RETIRE, 1, 1 },
        { TBT_F_SACK, 1, 1 },
    };
    size_t i;
    u16 min = 0xffff, max = 0xffff;
    char d[64];

    case_no++;
    check(TBT_HDR_LEN == 32 && TBT_CHUNK_MAX == 8228 && TBT_REC_MAX == 8224 && TBT_CONV == 0x0007,
          "design constants", NULL);
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
    {
        case_no++;
        snprintf(d, sizeof(d), "type=0x%02x", want[i].type);
        check(tbt_body_bounds(want[i].type, &min, &max), "bounds: known type", d);
        snprintf(d, sizeof(d), "type=0x%02x min=%u max=%u", want[i].type, min, max);
        check(min == want[i].min && max == want[i].max, "bounds: value", d);
    }
    case_no++;
    check(!tbt_body_bounds(0x00, &min, &max), "bounds: type 0x00 unknown", NULL);
    case_no++;
    check(!tbt_body_bounds(0xff, &min, &max), "bounds: type 0xff unknown", NULL);
    /* 0x11/0x12 were PREDICT/CANCEL before TBT_CONV 0x0005; a retired type
     * must not pass a bound check by accident. */
    case_no++;
    check(!tbt_body_bounds(0x11, &min, &max), "bounds: retired PREDICT unknown", NULL);
    case_no++;
    check(!tbt_body_bounds(0x12, &min, &max), "bounds: retired CANCEL unknown", NULL);
}

/* Every type against every body length. flags/ackc are fixed per pass, so the
 * exact-length rule for FBK collapses to a single accepted value. */
static void matrix_pass(u8 flags, u8 ackc)
{
    static u8 rec[TBT_REC_MAX];
    size_t t;
    unsigned len;

    for (t = 0; t < sizeof(all_types); t++)
    {
        for (len = 0; len <= TBT_PAYLOAD_MAX; len++)
        {
            struct tbt_rec r;
            int st = -1;
            size_t n = rec_make(rec, all_types[t], flags, ackc, 1, 0, (u16)len);
            size_t used = tbt_rec_scan(rec, n, &r, &st);
            char d[96];

            case_no++;
            snprintf(d, sizeof(d), "type=0x%02x flags=0x%02x ackc=%u len=%u", all_types[t], flags, ackc, len);
            check((st == TBT_REC_OK) == doc_accepts(all_types[t], ackc, len), "matrix: acceptance", d);
            check(used <= n, "matrix: consumed more than given", d);
            if (st == TBT_REC_OK)
                check(used == n && r.body_len == len, "matrix: record shape", d);
            else
                check(used == 0 && st == TBT_REC_PROTO, "matrix: refusal shape", d);
        }
    }
}

/* One accept/reject pair per range rule, so a dropped check fails the build. */
static void test_header_ranges(void)
{
    static u8 rec[TBT_REC_MAX];
    size_t n;

    n = rec_make(rec, TBT_F_PAD, 0, 0, 1, 0, 0);

    /* path_id names a carrier, and the carrier layer has 256 slots: a FEC v6
     * session with carriers=256 sends path_id 16..255. The 16-carrier limit
     * belongs to the old ARQ's ack fields, not to the index. */
    rec[TBT_H_PATH] = TBT_LEGACY_MAX_CARRIERS - 1;
    case_no++;
    expect("path_id 15", rec, n, TBT_REC_OK);
    rec[TBT_H_PATH] = TBT_LEGACY_MAX_CARRIERS;
    case_no++;
    expect("path_id 16", rec, n, TBT_REC_OK);
    rec[TBT_H_PATH] = 200;
    case_no++;
    expect("path_id 200", rec, n, TBT_REC_OK);
    rec[TBT_H_PATH] = 0xff;
    case_no++;
    expect("path_id 0xff", rec, n, TBT_REC_OK);
    rec[TBT_H_PATH] = 0;

    /* path_ack_count counts feedback entries: 0..TBT_LEGACY_MAX_CARRIERS. */
    rec[TBT_H_ACKC] = TBT_LEGACY_MAX_CARRIERS;
    case_no++;
    expect("path_ack_count at the limit", rec, n, TBT_REC_OK);
    rec[TBT_H_ACKC] = TBT_LEGACY_MAX_CARRIERS + 1;
    case_no++;
    expect("path_ack_count over the limit", rec, n, TBT_REC_PROTO);
    rec[TBT_H_ACKC] = 0xff;
    case_no++;
    expect("path_ack_count 0xff", rec, n, TBT_REC_PROTO);
    rec[TBT_H_ACKC] = 0;

    /* All 16 mask bits name a carrier in this revision; the check exists so a
     * future limit below 16 starts rejecting the spare bits. */
    tbt_put_le16(rec + TBT_H_ACKM, 0xffff);
    case_no++;
    expect("path_ack_mask all bits", rec, n, TBT_REC_OK);
    tbt_put_le16(rec + TBT_H_ACKM, 0);

    /* body_len ceiling, independent of the per-type bounds. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, TBT_PAYLOAD_MAX);
    case_no++;
    expect("body_len at TBT_PAYLOAD_MAX", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, TBT_PAYLOAD_MAX + 1);
    case_no++;
    expect("body_len over TBT_PAYLOAD_MAX", rec, TBT_HDR_LEN, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, 0xffff);
    case_no++;
    expect("body_len 0xffff", rec, TBT_HDR_LEN, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, 0);
    case_no++;
    expect("DATA with an empty body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, 1);
    case_no++;
    expect("DATA with one byte", rec, n, TBT_REC_OK);

    /* RETIRE carries exactly one byte: the carrier index. */
    n = rec_make(rec, TBT_F_RETIRE, 0, 0, 1, 0, 1);
    case_no++;
    expect("RETIRE with its index", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_RETIRE, 0, 0, 1, 0, 0);
    case_no++;
    expect("RETIRE without a body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_RETIRE, 0, 0, 1, 0, 2);
    case_no++;
    expect("RETIRE body 2", rec, n, TBT_REC_PROTO);

    n = rec_make(rec, TBT_F_SACK, 0, 0, 1, 0, 1);
    case_no++;
    expect("SACK body 1", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_SACK, 0, 0, 0, 0, 0);
    case_no++;
    expect("SACK body 0", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_SACK, 0, 0, 2, 0, 0);
    case_no++;
    expect("SACK body 2", rec, n, TBT_REC_PROTO);
    case_no++;
    expect("RETIRE with two bytes", rec, n, TBT_REC_PROTO);

    /* Flags: every defined bit is legal, the two spare bits are not. */
    n = rec_make(rec, TBT_F_PAD, TBT_FF_MASK, 0, 1, 0, 0);
    case_no++;
    expect("all defined flags", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_PAD, TBT_FF_MASK | 0x40, 0, 1, 0, 0);
    case_no++;
    expect("flag bit 6", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_PAD, 0x80, 0, 1, 0, 0);
    case_no++;
    expect("flag bit 7", rec, n, TBT_REC_PROTO);

    /* v4 uses the full 40-bit sid. */
    n = rec_make(rec, TBT_F_PAD, 0, 0, 1, 0, 0);
    rec[TBT_H_SIDHI] = 1;
    case_no++;
    expect("sid_hi 1", rec, n, TBT_REC_OK);
    rec[TBT_H_SIDHI] = 0xff;
    rec[TBT_H_SIDLO + 3] = 0xff;
    rec[TBT_H_SIDLO + 2] = 0xff;
    rec[TBT_H_SIDLO + 1] = 0xff;
    rec[TBT_H_SIDLO + 0] = 0xff;
    case_no++;
    expect("sid_hi 0xff full", rec, n, TBT_REC_OK);
    rec[TBT_H_SIDHI] = 0;

    /* reserved2 must be zero. */
    rec[TBT_H_RES2] = 1;
    case_no++;
    expect("reserved2 1", rec, n, TBT_REC_PROTO);
    rec[TBT_H_RES2] = 0xff;
    case_no++;
    expect("reserved2 0xff", rec, n, TBT_REC_PROTO);
    rec[TBT_H_RES2] = 0;
    case_no++;
    expect("reserved2 0", rec, n, TBT_REC_OK);

    /* conv must match exactly. */
    tbt_put_le16(rec + TBT_H_CONV, TBT_CONV - 1);
    case_no++;
    expect("conv TBT_CONV-1", rec, n, TBT_REC_PROTO);
    tbt_put_le16(rec + TBT_H_CONV, TBT_CONV + 1);
    case_no++;
    expect("conv TBT_CONV+1", rec, n, TBT_REC_PROTO);
    tbt_put_le16(rec + TBT_H_CONV, TBT_CONV);
}

static void test_matrix(void)
{
    u8 ackc;

    test_header_ranges();
    matrix_pass(0, 0);
    matrix_pass(TBT_FF_MUST_ORDER, 0);
    matrix_pass(TBT_FF_MUST_ORDER | TBT_FF_KEEP, 0);
    for (ackc = 1; ackc <= DOC_FBK_MAX_ENTRIES; ackc++)
        matrix_pass(0, ackc);
}

static void test_adversarial(void)
{
    static u8 rec[TBT_REC_MAX];
    static u8 two[2 * TBT_REC_MAX];
    struct tbt_rec r;
    int st;
    size_t n, used, cut;
    unsigned i;
    char d[96];

    /* Nothing can be judged before the whole header has arrived. */
    for (n = 0; n < TBT_HDR_LEN; n++)
    {
        case_no++;
        snprintf(d, sizeof(d), "header truncated to %zu", n);
        expect(d, rec, n, TBT_REC_NEED_MORE);
    }

    /* body_len extremes: the ceiling is TBT_PAYLOAD_MAX. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, TBT_PAYLOAD_MAX);
    case_no++;
    expect("body_len maximum (DATA)", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, TBT_PAYLOAD_MAX + 1);
    case_no++;
    expect("body_len one over the maximum", rec, TBT_HDR_LEN, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, 0xffff);
    case_no++;
    expect("body_len 0xffff", rec, TBT_HDR_LEN, TBT_REC_PROTO);

    /* A valid header whose body has not fully arrived is NEED_MORE, not OK. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, 64);
    case_no++;
    expect("body one byte short", rec, n - 1, TBT_REC_NEED_MORE);
    case_no++;
    expect("body complete", rec, n, TBT_REC_OK);

    /* The two boundary records that are legal. */
    n = rec_make(rec, TBT_F_PAD, 0, 0, 1, 0, 0);
    case_no++;
    expect("header-only record (PAD)", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, DOC_DATA_MAX);
    case_no++;
    expect("full-size record (DATA)", rec, n, TBT_REC_OK);

    /* Type/length contradictions. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, 0, DOC_DATA_MAX + 1);
    case_no++;
    expect("DATA one byte over payload max", rec, TBT_HDR_LEN, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, DOC_OPEN_BODY_LEN);
    case_no++;
    expect("OPEN with the 32-byte bitmap", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, DOC_OPEN_BODY_LEN - 1);
    case_no++;
    expect("OPEN one byte short", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, DOC_OPEN_BODY_LEN + 1);
    case_no++;
    expect("OPEN one byte long", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, 0);
    case_no++;
    expect("OPEN without a body", rec, n, TBT_REC_PROTO);
    /* The bitmap bits are opaque to the framing layer: a random 32-byte
     * body is a well-formed record and never changes the consumed length. */
    for (i = 0; i < 256; i++)
    {
        size_t k;

        n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, DOC_OPEN_BODY_LEN);
        for (k = 0; k < DOC_OPEN_BODY_LEN; k++)
            rec[TBT_HDR_LEN + k] = (u8)rnd64();
        case_no++;
        expect("OPEN with a random 32-byte bitmap", rec, n, TBT_REC_OK);
    }
    n = rec_make(rec, TBT_F_FIN, 0, 0, 1, 0, 1);
    case_no++;
    expect("FIN with a body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FIN, 0, 0, 1, 0, TBT_FIN_BODY_LEN);
    case_no++;
    expect("FIN with the 16-byte body", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_FIN, 0, 0, 1, 0, TBT_FIN_BODY_LEN - 1);
    case_no++;
    expect("FIN one byte short", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FIN, 0, 0, 1, 0, TBT_FIN_BODY_LEN + 1);
    case_no++;
    expect("FIN one byte long", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_RST, 0, 0, 1, 0, 1);
    case_no++;
    expect("RST with a body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_PAD, 0, 0, 1, 0, 1);
    case_no++;
    expect("PAD with a body", rec, n, TBT_REC_PROTO);

    n = rec_make(rec, TBT_F_FBK, 0, 0, 1, 0, 1);
    case_no++;
    expect("FBK 1 byte, nr=0", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FBK, 0, 2, 1, 0, DOC_FBK_ENTRY);
    case_no++;
    expect("FBK nr=2 with one entry", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FBK, 0, 2, 1, 0, 2 * DOC_FBK_ENTRY);
    case_no++;
    expect("FBK nr=2 with two entries", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_FBK, 0, 2, 1, 0, 2 * DOC_FBK_ENTRY - 1);
    case_no++;
    expect("FBK nr=2 with a ragged body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FBK, 0, 17, 1, 0, 17 * DOC_FBK_ENTRY);
    case_no++;
    expect("FBK nr above the carrier limit", rec, n, TBT_REC_PROTO);

    n = rec_make(rec, TBT_F_WINDOW, 0, 0, 1, 0, DOC_WINDOW_LEN - 1);
    case_no++;
    expect("WINDOW one byte short", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_WINDOW, 0, 0, 1, 0, DOC_WINDOW_LEN + 1);
    case_no++;
    expect("WINDOW one byte long", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_HELLO, 0, 0, 1, 0, DOC_HELLO_LEN - 1);
    case_no++;
    expect("HELLO one byte short", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_HELLO, 0, 0, 1, 0, DOC_HELLO_LEN + 1);
    case_no++;
    expect("HELLO v6 first byte past the legacy body", rec, n, TBT_REC_OK);
    n = rec_make(rec, TBT_F_HELLO, 0, 0, 1, 0, DOC_HELLO_LEN_V6 + 1);
    case_no++;
    expect("HELLO one byte past the v6 body", rec, n, TBT_REC_PROTO);

    /* seq wrap: no rule depends on seq, and the value must survive. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, ~(u64)0, 1);
    case_no++;
    expect("DATA at seq 2^64-1", rec, n, TBT_REC_OK);
    st = -1;
    used = scan_copy(rec, n, &r, &st);
    case_no++;
    check(st == TBT_REC_OK && used == n && r.hdr.seq == ~(u64)0, "seq wrap survives decode", NULL);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, ~(u64)0, DOC_OPEN_BODY_LEN);
    case_no++;
    expect("OPEN at seq 2^64-1", rec, n, TBT_REC_OK);

    /* Wire order is little-endian byte by byte (DESIGN.md §3.3). Build the
     * header from literals so a symmetric encoder/decoder bug cannot hide. */
    memset(rec, 0, TBT_HDR_LEN + 5);
    rec[TBT_H_TYPE] = TBT_F_DATA;
    rec[TBT_H_BODYLEN] = 0x05;
    rec[TBT_H_BODYLEN + 1] = 0x00;
    for (i = 0; i < 8; i++)
        rec[TBT_H_SEQ + i] = (u8)(8 - i);
    rec[TBT_H_SIDLO + 0] = 0x44;
    rec[TBT_H_SIDLO + 1] = 0x33;
    rec[TBT_H_SIDLO + 2] = 0x22;
    rec[TBT_H_SIDLO + 3] = 0x11;
    rec[TBT_H_CONV] = (u8)(TBT_CONV & 0xff);
    rec[TBT_H_CONV + 1] = (u8)(TBT_CONV >> 8);
    st = -1;
    checks++;
    if (tbt_rec_scan(rec, TBT_HDR_LEN + 5, &r, &st) != TBT_HDR_LEN + 5 ||
        st != TBT_REC_OK || r.hdr.seq != 0x0102030405060708ull || r.hdr.sid_lo != 0x11223344 ||
        r.hdr.body_len != 5 || r.hdr.conv != TBT_CONV)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: little-endian wire order\n", case_no);
    }

    /* OPEN names sid 7, DATA names sid 9: both records are well formed. The
     * "one sid per session" rule lives in tbt_rx_bytes (saw_open + adopt),
     * not in the stateless record layer, and this test pins that split. */
    n = rec_make(two, TBT_F_OPEN, 0, 0, 7, 0, DOC_OPEN_BODY_LEN);
    used = rec_make(two + n, TBT_F_DATA, 0, 0, 9, 1, 4);
    st = -1;
    checks++;
    if (tbt_rec_scan(two, n + used, &r, &st) != n || st != TBT_REC_OK || r.hdr.sid_lo != 7)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: OPEN sid\n", case_no);
    }
    st = -1;
    checks++;
    if (tbt_rec_scan(two + n, used, &r, &st) != used || st != TBT_REC_OK || r.hdr.sid_lo != 9)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: DATA sid after OPEN\n", case_no);
    }

    /* Version mismatch is refused whatever the type. */
    n = rec_make(rec, TBT_F_PAD, 0, 0, 1, 0, 0);
    tbt_put_le16(rec + TBT_H_CONV, 0);
    case_no++;
    expect("conv 0", rec, n, TBT_REC_PROTO);
    tbt_put_le16(rec + TBT_H_CONV, TBT_CONV + 1);
    case_no++;
    expect("conv + 1", rec, n, TBT_REC_PROTO);

    /* Types this revision does not define. */
    for (i = 0; i < 6; i++)
    {
        static const u8 bad[] = { 0x00, 0x0f, 0x16, 0x1f, 0x25, 0xff };

        n = rec_make(rec, bad[i], 0, 0, 1, 0, 0);
        case_no++;
        snprintf(d, sizeof(d), "unknown type 0x%02x", bad[i]);
        expect(d, rec, n, TBT_REC_PROTO);
    }

    /* Truncated records: with body_len intact, never OK; with body_len
     * adjusted to the short body, the type/length rule refuses it. */
    {
        static const struct
        {
            u8 type;
            u8 flags;
            u8 ackc;
            u16 len;
        } full[] = {
            { TBT_F_FBK, 0, 2, 2 * DOC_FBK_ENTRY },
            { TBT_F_HELLO, 0, 0, DOC_HELLO_LEN },
            { TBT_F_PAD, 0, 0, 0 },
            { TBT_F_WINDOW, 0, 0, DOC_WINDOW_LEN },
        };
        size_t k;

        for (k = 0; k < sizeof(full) / sizeof(full[0]); k++)
        {
            n = rec_make(rec, full[k].type, full[k].flags, full[k].ackc, 1, 0, full[k].len);
            for (cut = 1; cut <= n; cut++)
            {
                case_no++;
                snprintf(d, sizeof(d), "truncated record type=0x%02x cut=%zu", full[k].type, cut);
                expect(d, rec, n - cut, TBT_REC_NEED_MORE);
                snprintf(d, sizeof(d), "adjusted body_len type=0x%02x cut=%zu", full[k].type, cut);
                if (n - cut < TBT_HDR_LEN)
                {
                    expect(d, rec, n - cut, TBT_REC_NEED_MORE);
                }
                else
                {
                    tbt_put_le16(rec + TBT_H_BODYLEN, (u16)(n - cut - TBT_HDR_LEN));
                    expect(d, rec, n - cut, TBT_REC_PROTO);
                    tbt_put_le16(rec + TBT_H_BODYLEN, full[k].len);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Random buffers through a copy of the per-carrier accumulation loop. */

struct verdict
{
    size_t consumed;
    unsigned long records;
    u64 digest;
    int status;
    int first_status;
    size_t first_used;
};

static u64 digest_add(u64 h, u64 v)
{
    unsigned i;

    for (i = 0; i < 8; i++)
    {
        h ^= (u8)(v >> (8 * i));
        h *= 1099511628211ull;
    }
    return h;
}

static void run_stream(const u8 *buf, size_t len, size_t chunk, struct verdict *v)
{
    static u8 acc[TBT_CHUNK_MAX];
    size_t have = 0;
    size_t off = 0;
    int scanned = 0;

    memset(v, 0, sizeof(*v));
    v->digest = 14695981039346656037ull;
    v->status = TBT_REC_NEED_MORE;
    v->first_status = TBT_REC_NEED_MORE;

    while (off < len)
    {
        size_t take = len - off;
        size_t used;

        if (take > chunk)
            take = chunk;
        if (take > sizeof(acc) - have)
            take = sizeof(acc) - have;
        memcpy(acc + have, buf + off, take);
        have += take;
        off += take;

        for (;;)
        {
            struct tbt_rec rec;
            int st = -1;

            used = tbt_rec_scan(acc, have, &rec, &st);
            if (!scanned)
            {
                v->first_status = st;
                v->first_used = used;
                scanned = 1;
            }
            checks++;
            if (used > have)
            {
                failures++;
                printf("FAIL seed=1 case=%lu: split %zu: consumed %zu of %zu\n", case_no, chunk, used, have);
                v->status = -1;
                return;
            }
            if (st == TBT_REC_NEED_MORE)
            {
                if (used != 0)
                {
                    failures++;
                    printf("FAIL seed=1 case=%lu: NEED_MORE consumed %zu\n", case_no, used);
                }
                break;
            }
            if (st == TBT_REC_PROTO)
            {
                if (used != 0)
                {
                    failures++;
                    printf("FAIL seed=1 case=%lu: PROTO consumed %zu\n", case_no, used);
                }
                v->status = TBT_REC_PROTO;
                return;
            }
            if (st != TBT_REC_OK || used == 0 || rec.body_len != used - TBT_HDR_LEN)
            {
                failures++;
                printf("FAIL seed=1 case=%lu: OK with used=%zu body_len=%u status=%d\n", case_no, used,
                       rec.body_len, st);
                v->status = -1;
                return;
            }
            v->consumed += used;
            v->records++;
            v->digest = digest_add(v->digest, rec.hdr.type);
            v->digest = digest_add(v->digest, rec.hdr.flags);
            v->digest = digest_add(v->digest, rec.hdr.seq);
            v->digest = digest_add(v->digest, rec.hdr.sid_lo);
            v->digest = digest_add(v->digest, rec.body_len);
            have -= used;
            memmove(acc, acc + used, have);
        }
        if (off < len && have == sizeof(acc))
        {
            /* The module returns -EPROTO here: its buffer holds a whole chunk
             * and still no record. */
            v->status = TBT_REC_PROTO;
            return;
        }
    }
}

/* A record that fits in `room`, or 0. Types and bodies follow the design, so
 * the split comparison gets to work through real record boundaries. */
static size_t gen_record(u8 *buf, size_t room)
{
    u8 type = all_types[rnd_below(sizeof(all_types))];
    u8 flags = 0, ackc = 0;
    u16 len = 0;
    size_t body_room;

    if (room < TBT_HDR_LEN)
        return 0;
    body_room = room - TBT_HDR_LEN;

    switch (type)
    {
    case TBT_F_DATA:
        len = (u16)(1 + rnd_below(body_room < DOC_DATA_MAX ? body_room : DOC_DATA_MAX));
        break;
    case TBT_F_OPEN:
        len = DOC_OPEN_BODY_LEN;
        break;
    case TBT_F_RETIRE:
        len = 1;
        break;
    case TBT_F_FBK:
        ackc = (u8)rnd_below(DOC_FBK_MAX_ENTRIES + 1);
        len = (u16)(ackc * DOC_FBK_ENTRY);
        break;
    case TBT_F_WINDOW:
        len = DOC_WINDOW_LEN;
        break;
    case TBT_F_HELLO:
        len = DOC_HELLO_LEN;
        break;
    default:
        break;
    }
    if ((size_t)len + TBT_HDR_LEN > room)
        return 0;
    return rec_make(buf, type, flags, ackc, (u32)rnd64(), rnd64(), len);
}

static void gen_buffer(u8 *buf, size_t len)
{
    size_t i;
    unsigned mode;

    for (i = 0; i + 8 <= len; i += 8)
    {
        u64 v = rnd64();

        memcpy(buf + i, &v, 8);
    }
    if (i < len)
    {
        u64 v = rnd64();

        memcpy(buf + i, &v, len - i);
    }

    /* One third stays pure noise, one third starts with a well-formed
     * record, one third is a short chain of them. */
    mode = (unsigned)(rnd64() % 3);
    if (mode == 1)
    {
        gen_record(buf, len);
    }
    else if (mode == 2)
    {
        size_t off = 0;
        unsigned n = 1 + (unsigned)rnd_below(8);

        while (n-- && off < len)
        {
            size_t used = gen_record(buf + off, len - off);

            if (!used)
                break;
            off += used;
        }
    }
}

static void same_verdict(const struct verdict *a, const struct verdict *b, const char *what)
{
    case_no++;
    check(a->status == b->status && a->consumed == b->consumed && a->records == b->records &&
              a->digest == b->digest,
          what, NULL);
}

static void test_random_buffers(void)
{
    static u8 buf[TBT_CHUNK_MAX + 64];
    size_t i;

    for (i = 0; i < FUZZ_BUFFERS; i++)
    {
        struct verdict split_a, split_b, oneshot;
        struct tbt_rec rec;
        size_t len = rnd_below(sizeof(buf) + 1);
        size_t chunk_a, chunk_b;
        int st = -1;
        size_t used;

        case_no = i + 1;
        gen_buffer(buf, len);

        /* Exact-size heap copy: ASan turns any read past `len` into a failure. */
        used = scan_copy(buf, len, &rec, &st);
        check(used <= len, "direct scan: consumed more than given", NULL);

        chunk_a = 1 + rnd_below(4096);
        chunk_b = 1 + rnd_below(4096);
        run_stream(buf, len, chunk_a, &split_a);
        run_stream(buf, len, chunk_b, &split_b);
        run_stream(buf, len, len ? len : 1, &oneshot);

        same_verdict(&split_a, &split_b, "split patterns disagree");
        same_verdict(&split_a, &oneshot, "split vs one-shot disagree");
        check(oneshot.first_status == st && oneshot.first_used == used, "one-shot first scan vs direct scan", NULL);
    }
}

/* Random u16 body lengths: the ceiling rule must hold for every value. The
 * header is always fully present; only the body tail varies. */
static void test_body_lengths(void)
{
    static const u16 edge[] = { 0,    1,    2,    16,   23,   31,   32,   128,   137,
                                138,  139,  8191, 8192, 8193, 8224, 8228, 0x7fff, 0xffff };
    static u8 buf[TBT_REC_MAX];
    size_t i;

    for (i = 0; i < FUZZ_BODY_LENGTHS; i++)
    {
        struct tbt_rec rec;
        u16 body;
        size_t tail, len, used, j;
        int conv_ok;
        int st = -1;

        case_no = i + 1;
        if (rnd64() & 1)
            body = (u16)rnd64();
        else
            body = edge[rnd_below(sizeof(edge) / sizeof(edge[0]))];
        tail = rnd_below(TBT_PAYLOAD_MAX + 1);
        len = TBT_HDR_LEN + tail;

        memset(buf, 0, TBT_HDR_LEN);
        buf[TBT_H_TYPE] = TBT_F_DATA;
        tbt_put_le16(buf + TBT_H_BODYLEN, body);
        for (j = TBT_HDR_LEN; j < len; j++)
            buf[j] = (u8)rnd64();
        conv_ok = (int)(rnd64() & 1);
        if (conv_ok)
            tbt_put_le16(buf + TBT_H_CONV, TBT_CONV);

        used = scan_copy(buf, len, &rec, &st);
        checks++;
        if (used > len)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: body_len %u consumed %zu of %zu\n", case_no, body, used, len);
            continue;
        }
        if (!conv_ok || body == 0 || body > TBT_PAYLOAD_MAX)
        {
            checks++;
            if (st != TBT_REC_PROTO || used != 0)
            {
                failures++;
                printf("FAIL seed=1 case=%lu: body_len %u outside the DATA bounds gave %s, %zu bytes\n",
                       case_no, body,
                       st_name(st), used);
            }
        }
        else if (tail < body)
        {
            checks++;
            if (st != TBT_REC_NEED_MORE || used != 0)
            {
                failures++;
                printf("FAIL seed=1 case=%lu: body_len %u truncated gave %s, %zu bytes\n", case_no, body,
                       st_name(st), used);
            }
        }
        else
        {
            checks++;
            if (st != TBT_REC_OK || used != (size_t)TBT_HDR_LEN + body)
            {
                failures++;
                printf("FAIL seed=1 case=%lu: body_len %u complete gave %s, %zu bytes\n", case_no, body,
                       st_name(st), used);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 0.4.0 v7 control bodies and the /proc config grammar. */

#define ALLOC_FUZZ 20000

static void alloc_fix_crc(u8 *buf)
{
    tbt_put_le32(buf + TBT_FEC_ALLOC_CRC,
                 tbt_fec_crc32c(0, buf, TBT_FEC_ALLOC_CRC_BYTES));
}

static int alloc_unpack_copy(const u8 *src, struct tbt_fec_alloc *a)
{
    u8 *copy = malloc(TBT_FEC_ALLOC_LEN);
    int rc;

    if (!copy)
    {
        printf("FAIL seed=1 case=%lu: out of memory\n", case_no);
        exit(2);
    }
    memcpy(copy, src, TBT_FEC_ALLOC_LEN);
    rc = tbt_fec_alloc_unpack(copy, a);
    free(copy);
    return rc;
}

static void alloc_expect(const char *what, const u8 *buf, int want)
{
    struct tbt_fec_alloc a;
    int rc = alloc_unpack_copy(buf, &a);

    checks++;
    if (rc != want)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: want %s, got %d\n", case_no, what,
               want ? "EPROTO" : "OK", rc);
    }
}

static void alloc_seed(struct tbt_fec_alloc *a)
{
    memset(a, 0, sizeof(*a));
    a->epoch = 1;
    tbt_fec_bitmap_set(a->group_mask, 0);
    tbt_fec_bitmap_set(a->group_mask, 5);
    tbt_fec_bitmap_set(a->group_mask, 255);
    a->flags = TBT_FEC_ALLOC_F_ACK;
    a->switch_pkt = 0x1122334455667788ull;
    a->block_base = 0x0102030405060708ull;
}

/* FEC_ALLOC (0x36): every mask shape, every flag value, every reserved byte,
 * both CRCs and 20k random bodies. An accepted body must repack unchanged. */
static void test_alloc_body(void)
{
    static const u8 extra[] = { 1, 0x80, 0xff };
    struct tbt_fec_alloc a, b;
    u8 seed[TBT_FEC_ALLOC_LEN], buf[TBT_FEC_ALLOC_LEN], out[TBT_FEC_ALLOC_LEN];
    unsigned i, p, k;

    case_no++;
    check(TBT_FEC_ALLOC == 0x36 && TBT_FEC_ALLOC_LEN == 64 &&
              TBT_FEC_ALLOC_CRC_BYTES == 56 && TBT_FEC_BITMAP_BYTES == 32,
          "ALLOC wire constants", NULL);

    alloc_seed(&a);
    case_no++;
    check(tbt_fec_alloc_pack(&a, seed) == TBT_FEC_ALLOC_LEN,
          "ALLOC pack length", NULL);
    alloc_expect("ALLOC seed", seed, 0);

    memset(&b, 0, sizeof(b));
    case_no++;
    check(alloc_unpack_copy(seed, &b) == 0 && b.epoch == a.epoch &&
              b.flags == a.flags && b.switch_pkt == a.switch_pkt &&
              b.block_base == a.block_base &&
              !memcmp(b.group_mask, a.group_mask, TBT_FEC_BITMAP_BYTES),
          "ALLOC round trip", NULL);

    /* The bitmap is opaque here, but it must name at least one group. */
    memcpy(buf, seed, sizeof(buf));
    memset(buf + TBT_FEC_ALLOC_GROUPS, 0, TBT_FEC_BITMAP_BYTES);
    alloc_fix_crc(buf);
    case_no++;
    alloc_expect("ALLOC empty group mask", buf, -EPROTO);

    memcpy(buf, seed, sizeof(buf));
    memset(buf + TBT_FEC_ALLOC_GROUPS, 0xff, TBT_FEC_BITMAP_BYTES);
    alloc_fix_crc(buf);
    case_no++;
    alloc_expect("ALLOC all-one group mask", buf, 0);

    for (p = 0; p < TBT_FEC_MAX_CARRIERS; p++)
    {
        memcpy(buf, seed, sizeof(buf));
        memset(buf + TBT_FEC_ALLOC_GROUPS, 0, TBT_FEC_BITMAP_BYTES);
        buf[TBT_FEC_ALLOC_GROUPS + p / 8] = (u8)(1u << (p % 8));
        alloc_fix_crc(buf);
        case_no++;
        alloc_expect("ALLOC single-bit group mask", buf, 0);
    }

    for (i = 0; i < 256; i++)
    {
        memcpy(buf, seed, sizeof(buf));
        buf[TBT_FEC_ALLOC_FLAGS] = (u8)i;
        alloc_fix_crc(buf);
        case_no++;
        alloc_expect("ALLOC flags", buf,
                     (i & ~TBT_FEC_ALLOC_F_MASK) ? -EPROTO : 0);
    }

    for (i = 0; i < 3; i++)
        for (k = 0; k < sizeof(extra); k++)
        {
            memcpy(buf, seed, sizeof(buf));
            buf[TBT_FEC_ALLOC_RESERVED + i] = extra[k];
            alloc_fix_crc(buf);
            case_no++;
            alloc_expect("ALLOC reserved byte", buf, -EPROTO);
        }

    for (i = 0; i < 4; i++)
        for (k = 0; k < sizeof(extra); k++)
        {
            memcpy(buf, seed, sizeof(buf));
            buf[TBT_FEC_ALLOC_RESERVED2 + i] = extra[k];
            alloc_fix_crc(buf);
            case_no++;
            alloc_expect("ALLOC reserved2 byte", buf, -EPROTO);
        }

    for (i = 0; i < 4; i++)
    {
        memcpy(buf, seed, sizeof(buf));
        buf[TBT_FEC_ALLOC_CRC + i] ^= 0xff;
        case_no++;
        alloc_expect("ALLOC CRC byte", buf, -EPROTO);
    }
    for (i = 0; i < TBT_FEC_ALLOC_CRC_BYTES; i++)
    {
        memcpy(buf, seed, sizeof(buf));
        buf[i] ^= 0x01;
        case_no++;
        alloc_expect("ALLOC body bit flip", buf, -EPROTO);
    }

    for (i = 0; i < ALLOC_FUZZ; i++)
    {
        unsigned muts = 1 + (unsigned)rnd_below(6);
        int crc_ok, mask_ok, flags_ok, res_ok, want;

        case_no++;
        memcpy(buf, seed, sizeof(buf));
        while (muts--)
            buf[rnd_below(TBT_FEC_ALLOC_LEN)] ^= (u8)(1 + rnd_below(255));
        if (rnd64() & 3)
            alloc_fix_crc(buf);
        crc_ok = tbt_le32(buf + TBT_FEC_ALLOC_CRC) ==
                 tbt_fec_crc32c(0, buf, TBT_FEC_ALLOC_CRC_BYTES);
        mask_ok = tbt_fec_bitmap_count(buf + TBT_FEC_ALLOC_GROUPS) != 0;
        flags_ok = !(buf[TBT_FEC_ALLOC_FLAGS] & ~TBT_FEC_ALLOC_F_MASK);
        res_ok = !buf[TBT_FEC_ALLOC_RESERVED] &&
                 !buf[TBT_FEC_ALLOC_RESERVED + 1] &&
                 !buf[TBT_FEC_ALLOC_RESERVED + 2] &&
                 tbt_le32(buf + TBT_FEC_ALLOC_RESERVED2) == 0;
        want = (crc_ok && mask_ok && flags_ok && res_ok) ? 0 : -EPROTO;
        alloc_expect("ALLOC random body", buf, want);
        if (!want)
        {
            memset(&b, 0, sizeof(b));
            case_no++;
            check(alloc_unpack_copy(buf, &b) == 0 &&
                      tbt_fec_alloc_pack(&b, out) == TBT_FEC_ALLOC_LEN &&
                      !memcmp(out, buf, TBT_FEC_ALLOC_LEN),
                  "ALLOC accepted body repacks", NULL);
        }
    }
}

#define CFG_FUZZ 40000
#define PARSE_FUZZ 8000

static void cfg_fix_crc(u8 *buf)
{
    tbt_put_le32(buf + TBT_FEC_CFG_CRC,
                 tbt_fec_crc32c(0, buf, TBT_FEC_CFG_CRC_BYTES));
}

static int cfg_unpack_copy(const u8 *src, struct tbt_fec_cfg *c)
{
    u8 *copy = malloc(TBT_FEC_CFG_LEN);
    int rc;

    if (!copy)
    {
        printf("FAIL seed=1 case=%lu: out of memory\n", case_no);
        exit(2);
    }
    memcpy(copy, src, TBT_FEC_CFG_LEN);
    rc = tbt_fec_cfg_unpack(copy, c);
    free(copy);
    return rc;
}

struct cfg_seed
{
    u16 n, m, w, k;
    u8 d, flags;
};

/* Three legal shapes, one per encoding the FEC_CFG body shares. */
static const struct cfg_seed cfg_seeds[3] = {
    { 8, 8, 64, 32, 4, TBT_FEC_CFG_F_APPLY },      /* RLNC */
    { 128, 128, 256, 500, 0, TBT_FEC_CFG_F_INIT }, /* SC-LDPC binary */
    { 8, 24, 16, 128, 64, TBT_FEC_CFG_F_APPLY },   /* SC-LDPC GF(16) */
};

static const u16 cfg_edges[] = { 0,    1,    2,    3,    5,    15,   16,
                                 17,   31,   32,   100,  127,  128,  129,
                                 255,  256,  257,  511,  512,  1000, 1001,
                                 1024, 1025, 0x7fff, 0xffff };

static u16 cfg_rand_u16(void)
{
    if (rnd64() & 3)
        return cfg_edges[rnd_below(sizeof(cfg_edges) / sizeof(cfg_edges[0]))];
    return (u16)rnd64();
}

static u16 cfg_groups_for(u16 n, u16 m)
{
    u32 shares = (u32)n + m;

    if (!shares || TBT_FEC_MAX_CARRIERS % shares)
        return 0;
    return (u16)(TBT_FEC_MAX_CARRIERS / shares);
}

static void cfg_pack_seed(unsigned which, u8 *buf)
{
    const struct cfg_seed *s = &cfg_seeds[which];
    struct tbt_fec_cfg c;

    memset(&c, 0, sizeof(c));
    c.epoch = 7;
    c.n = s->n;
    c.m = s->m;
    c.w = s->w;
    c.k = s->k;
    c.d = s->d;
    c.flags = s->flags;
    c.groups = cfg_groups_for(s->n, s->m);
    c.switch_pkt = 0x0102030405060708ull;
    c.block_base = 0x1112131415161718ull;
    c.seed = 0x2122232425262728ull;
    if (tbt_fec_cfg_pack(&c, buf))
    {
        printf("FAIL seed=1 case=%lu: CFG seed %u does not pack\n", case_no,
               which);
        exit(2);
    }
}

static void cfg_expect(const char *what, const u8 *buf, int want)
{
    struct tbt_fec_cfg c;
    int rc = cfg_unpack_copy(buf, &c);

    checks++;
    if (rc != want)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: want %s, got %d\n", case_no, what,
               want ? "EPROTO" : "OK", rc);
    }
}

/* The documented acceptance rules, re-derived from tbt_uapi.h. Kept apart
 * from tbt_fec_cfg_unpack() so a drift on either side fails the fuzz. */
static int cfg_doc_accepts(const u8 *b)
{
    u16 n = tbt_le16(b + TBT_FEC_CFG_N);
    u16 m = tbt_le16(b + TBT_FEC_CFG_M);
    u16 w = tbt_le16(b + TBT_FEC_CFG_W);
    u16 k = tbt_le16(b + TBT_FEC_CFG_K);
    u16 groups = tbt_le16(b + TBT_FEC_CFG_GROUPS);
    u8 d = b[TBT_FEC_CFG_D];
    u32 shares = (u32)n + m;

    if (tbt_le32(b + TBT_FEC_CFG_CRC) !=
        tbt_fec_crc32c(0, b, TBT_FEC_CFG_CRC_BYTES))
        return 0;
    if (tbt_le32(b + TBT_FEC_CFG_RESERVED) != 0)
        return 0;
    if (b[TBT_FEC_CFG_FLAGS] & ~TBT_FEC_CFG_F_MASK)
        return 0;
    if (!n || !m || shares > TBT_FEC_MAX_SHARES)
        return 0;
    if (!k || w > TBT_FEC_MAX_W)
        return 0;
    if (d == 0)
    {
        if (k < 100 || k > 1000)
            return 0;
    }
    else if (w < k)
    {
        if (k < 16 || (k & 1) || w < 2)
            return 0;
    }
    else
    {
        if (d > w)
            return 0;
        if (w > 1 && d == w)
            return 0;
    }
    if (!groups || TBT_FEC_MAX_CARRIERS % shares ||
        groups != TBT_FEC_MAX_CARRIERS / shares)
        return 0;
    return 1;
}

/* FEC_CFG (0x34): the three (d, k, w, n, m, groups) families, the zero and
 * overflow edges that would divide by zero downstream, and 40k mutations
 * checked against cfg_doc_accepts(). */
static void test_cfg_body(void)
{
    static u8 seeds[3][TBT_FEC_CFG_LEN];
    static u8 buf[TBT_FEC_CFG_LEN], out[TBT_FEC_CFG_LEN];
    unsigned i, s;

    case_no++;
    check(TBT_FEC_CFG_LEN == 48 && TBT_FEC_CONV == 0x1007u &&
              TBT_FEC_CFG_CRC_BYTES == 40 && TBT_FEC_MAX_SHARES == 256,
          "FEC_CFG wire constants", NULL);

    for (s = 0; s < 3; s++)
    {
        struct tbt_fec_cfg c;
        u16 groups = cfg_groups_for(cfg_seeds[s].n, cfg_seeds[s].m);

        cfg_pack_seed(s, seeds[s]);
        memset(&c, 0, sizeof(c));
        case_no++;
        check(cfg_unpack_copy(seeds[s], &c) == 0 &&
                  c.n == cfg_seeds[s].n && c.m == cfg_seeds[s].m &&
                  c.w == cfg_seeds[s].w && c.k == cfg_seeds[s].k &&
                  c.d == cfg_seeds[s].d && c.flags == cfg_seeds[s].flags &&
                  c.groups == groups,
              "CFG seed round trip", NULL);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 0);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG n=0", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_M, 0);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG m=0", buf, -EPROTO);

        /* u16 wrap edges: the shares sum must be computed in 32 bits. */
        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 1);
        tbt_put_le16(buf + TBT_FEC_CFG_M, 0xffff);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG n+m=65536", buf, -EPROTO);
        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 0xffff);
        tbt_put_le16(buf + TBT_FEC_CFG_M, 1);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG n+m=65536 (reversed)", buf, -EPROTO);
        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 0xffff);
        tbt_put_le16(buf + TBT_FEC_CFG_M, 0xffff);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG n+m=131070", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 200);
        tbt_put_le16(buf + TBT_FEC_CFG_M, 100);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG shares over 256", buf, -EPROTO);

        /* 256 is not divisible by 5, so no groups value can describe it. */
        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_N, 3);
        tbt_put_le16(buf + TBT_FEC_CFG_M, 2);
        tbt_put_le16(buf + TBT_FEC_CFG_GROUPS, 51);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG shares do not divide 256", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_GROUPS, 0);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG groups=0", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_GROUPS,
                     (u16)(cfg_groups_for(cfg_seeds[s].n, cfg_seeds[s].m) + 1));
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG groups mismatch", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_K, 0);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG k=0", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        tbt_put_le16(buf + TBT_FEC_CFG_W, TBT_FEC_MAX_W + 1);
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG w over the cap", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        buf[TBT_FEC_CFG_FLAGS] |= 0x08;
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG unknown flag", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        buf[TBT_FEC_CFG_FLAGS] = 0xff;
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG all flags", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        buf[TBT_FEC_CFG_RESERVED] = 1;
        cfg_fix_crc(buf);
        case_no++;
        cfg_expect("CFG reserved", buf, -EPROTO);

        memcpy(buf, seeds[s], sizeof(buf));
        buf[TBT_FEC_CFG_CRC + 1] ^= 0x80;
        case_no++;
        cfg_expect("CFG CRC fault", buf, -EPROTO);

        /* The discriminator between the three encodings. */
        memcpy(buf, seeds[s], sizeof(buf));
        if (s == 0)
        {
            buf[TBT_FEC_CFG_D] = (u8)(cfg_seeds[s].w + 1);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG RLNC d over w", buf, -EPROTO);
            memcpy(buf, seeds[s], sizeof(buf));
            buf[TBT_FEC_CFG_D] = (u8)cfg_seeds[s].w;
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG RLNC d=w", buf, -EPROTO);
        }
        else if (s == 1)
        {
            tbt_put_le16(buf + TBT_FEC_CFG_K, 99);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG binary k below 100", buf, -EPROTO);
            memcpy(buf, seeds[s], sizeof(buf));
            tbt_put_le16(buf + TBT_FEC_CFG_K, 1001);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG binary k over 1000", buf, -EPROTO);
        }
        else
        {
            tbt_put_le16(buf + TBT_FEC_CFG_K, 15);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG GF16 k=15", buf, -EPROTO);
            memcpy(buf, seeds[s], sizeof(buf));
            tbt_put_le16(buf + TBT_FEC_CFG_K, 17);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG GF16 odd k", buf, -EPROTO);
            memcpy(buf, seeds[s], sizeof(buf));
            tbt_put_le16(buf + TBT_FEC_CFG_W, 1);
            cfg_fix_crc(buf);
            case_no++;
            cfg_expect("CFG GF16 w=1", buf, -EPROTO);
        }
    }

    for (i = 0; i < CFG_FUZZ; i++)
    {
        struct tbt_fec_cfg c;
        unsigned muts = 1 + (unsigned)rnd_below(5);
        int rc, want;

        case_no = i + 1;
        s = (unsigned)rnd_below(3);
        memcpy(buf, seeds[s], sizeof(buf));
        while (muts--)
        {
            switch (rnd_below(7))
            {
            case 0:
                tbt_put_le16(buf + TBT_FEC_CFG_N, cfg_rand_u16());
                break;
            case 1:
                tbt_put_le16(buf + TBT_FEC_CFG_M, cfg_rand_u16());
                break;
            case 2:
                tbt_put_le16(buf + TBT_FEC_CFG_W, cfg_rand_u16());
                break;
            case 3:
                tbt_put_le16(buf + TBT_FEC_CFG_K, cfg_rand_u16());
                break;
            case 4:
                buf[TBT_FEC_CFG_D] = (u8)rnd64();
                break;
            case 5:
                tbt_put_le16(buf + TBT_FEC_CFG_GROUPS, cfg_rand_u16());
                break;
            default:
                buf[TBT_FEC_CFG_FLAGS] = (u8)rnd64();
                break;
            }
        }
        if (!(rnd64() & 7))
            buf[TBT_FEC_CFG_CRC + rnd_below(4)] ^= (u8)(1 + rnd_below(255));
        else
            cfg_fix_crc(buf);
        want = cfg_doc_accepts(buf);
        memset(&c, 0, sizeof(c));
        rc = cfg_unpack_copy(buf, &c);
        checks++;
        if ((rc == 0) != (want != 0) || (rc != 0 && rc != -EPROTO))
        {
            failures++;
            printf("FAIL seed=1 case=%lu: CFG random body: want %s, got %d\n",
                   case_no, want ? "OK" : "EPROTO", rc);
            continue;
        }
        if (!want)
            continue;
        checks++;
        if (!c.n || !c.m || (u32)c.n + c.m > TBT_FEC_MAX_SHARES ||
            !c.groups || !c.k || c.w > TBT_FEC_MAX_W)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: CFG accepted an unusable shape\n",
                   case_no);
            continue;
        }
        checks++;
        if (tbt_fec_cfg_pack(&c, out) || memcmp(out, buf, sizeof(out)))
        {
            failures++;
            printf("FAIL seed=1 case=%lu: CFG accepted body does not repack\n",
                   case_no);
        }
    }
}

/* Parse one line in place and hold every accepted tunnel to the invariants
 * the parser promises plus the canonical render/parse fixed point. */
static int parse_case(const char *text, const char *what)
{
    char buf[TBT_LINE_MAX + 1];
    char rendered[TBT_LINE_MAX];
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    size_t len = strlen(text);
    int rc;

    if (len > TBT_LINE_MAX - 1)
        len = TBT_LINE_MAX - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    rc = tbt_parse_line(buf, &cmd, &perr);
    checks++;
    if (rc != 0 && rc != TBT_PARSE_EINVAL && rc != TBT_PARSE_E2BIG &&
        rc != TBT_PARSE_EAFNOSUPPORT)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: unexpected rc=%d\n", case_no, what,
               rc);
        return rc;
    }
    if (rc)
    {
        checks++;
        if (!perr.why)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: refusal without a reason\n",
                   case_no, what);
        }
        return rc;
    }
    checks++;
    if (perr.key || perr.why)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: success left an error behind\n",
               case_no, what);
        return rc;
    }
    checks++;
    if (cmd.kind < TBT_CMD_NONE || cmd.kind > TBT_CMD_SESSION)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: unknown command kind\n", case_no,
               what);
        return rc;
    }
    if (cmd.kind != TBT_CMD_TUNNEL)
        return rc;

    checks++;
    if (!cmd.cfg.role || cmd.cfg.role > TBT_ROLE_FAR || !cmd.cfg.profile ||
        cmd.cfg.profile > 3 || !cmd.cfg.mode || cmd.cfg.mode > 3 ||
        !cmd.cfg.n || cmd.cfg.n + cmd.cfg.m > TBT_MAX_CARRIERS_LIMIT ||
        !cmd.cfg.carriers ||
        cmd.cfg.carriers % (cmd.cfg.n + cmd.cfg.m) ||
        (cmd.cfg.fec && (cmd.cfg.m < 1 ||
                         cmd.cfg.carriers < cmd.cfg.n + cmd.cfg.m)) ||
        (cmd.cfg.adaptive && !cmd.cfg.fec) ||
        cmd.cfg.portblock_lo > cmd.cfg.portblock_hi)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: accepted config violates its invariants\n",
               case_no, what);
        return rc;
    }

    rc = tbt_render_cfg(&cmd.cfg, rendered, sizeof(rendered));
    checks++;
    if (rc <= 0 || rc >= (int)sizeof(rendered))
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: render failed (%d)\n", case_no, what,
               rc);
        return rc;
    }
    {
        char text2[TBT_LINE_MAX], rendered2[TBT_LINE_MAX];
        struct tbt_cmd again;
        struct tbt_parse_error perr2 = { NULL, NULL };
        int rc2;

        memcpy(text2, rendered, (size_t)rc + 1);
        rc2 = tbt_parse_line(text2, &again, &perr2);
        checks++;
        if (rc2)
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: canonical line was refused (%d)\n",
                   case_no, what, rc2);
            return rc;
        }
        rc2 = tbt_render_cfg(&again.cfg, rendered2, sizeof(rendered2));
        checks++;
        if (rc2 != rc || memcmp(rendered, rendered2, (size_t)rc))
        {
            failures++;
            printf("FAIL seed=1 case=%lu: %s: canonical form is not a fixed point\n",
                   case_no, what);
        }
    }
    return 0;
}

static void parse_expect(const char *what, const char *text, int want)
{
    int rc = parse_case(text, what);

    checks++;
    if (rc != want)
    {
        failures++;
        printf("FAIL seed=1 case=%lu: %s: want rc=%d, got rc=%d\n", case_no,
               what, want, rc);
    }
}

static void parse_suffix(const char *what, const char *extra, int want)
{
    char line[TBT_LINE_MAX];

    snprintf(line, sizeof(line),
             "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 %s", extra);
    case_no++;
    parse_expect(what, line, want);
}

/* portblock=, adaptive=, L= and mode=sc-ldpc: values that must be refused,
 * the legal boundary on both sides, and 8k mutated lines. */
static void test_parse_bomb(void)
{
    static const char *const key_values[] = {
        "portblock=0-0",         "portblock=40000-44095",
        "portblock=40000-40003", "portblock=40000-40004",
        "portblock=40000-39999", "portblock=0-5",
        "portblock=",            "portblock=-",
        "portblock=65536-65536", "adaptive=on",
        "adaptive=off",          "adaptive=maybe",
        "adaptive=",             "L=0",
        "L=4096",                "L=4097",
        "L=65536",               "L=-1",
        "L=0x10",                "mode=sc-ldpc",
        "mode=SC-LDPC",          "mode=scldpc",
        "mode=sc-ldpcx",         "mode=",
        "mode=sc-ldpc w=100",    "mode=sc-ldpc w=128",
        "mode=sc-ldpc w=512",    "mode=sc-ldpc w=1000",
    };
    static const char charset[] = "0123456789abcdefx-= .:\t#";
    char line[TBT_LINE_MAX];
    unsigned i;

    case_no++;
    parse_expect("valid 0.4.0 near line",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=4 m=1 fec=on adaptive=on mode=rlnc w=8 carriers=5"
                 " crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000"
                 " drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000"
                 " hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0"
                 " tx_retain_bytes=0 carrier_stall_ms=0"
                 " portblock=40000-44095\n",
                 0);

    parse_suffix("portblock 0-0", "portblock=0-0", 0);
    parse_suffix("portblock exact fit", "portblock=40000-40004", 0);
    parse_suffix("portblock one short", "portblock=40000-40003",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock reversed", "portblock=40000-39999",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock zero start", "portblock=0-5", TBT_PARSE_EINVAL);
    parse_suffix("portblock missing hi", "portblock=40000",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock empty", "portblock=", TBT_PARSE_EINVAL);
    parse_suffix("portblock dash only", "portblock=-", TBT_PARSE_EINVAL);
    parse_suffix("portblock double dash", "portblock=40000--40010",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock u16 overflow", "portblock=65536-65536",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock too long",
                 "portblock=0000000000000000000000000001-000000000002",
                 TBT_PARSE_EINVAL);
    parse_suffix("portblock hex", "portblock=0x9c40-0x9c44", 0);

    parse_expect("portblock holds every carrier",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=128 m=128 fec=on portblock=40000-40255",
                 0);
    parse_expect("portblock one short of carriers",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=128 m=128 fec=on portblock=40000-40254",
                 TBT_PARSE_EINVAL);
    parse_expect("far portblock is recorded, not sized",
                 "tunnel 2 role=far listen=10.0.0.1:1 target=10.0.0.2:2"
                 " n=128 m=128 fec=on portblock=40000-40000",
                 0);

    parse_suffix("adaptive without fec", "adaptive=on", TBT_PARSE_EINVAL);
    parse_suffix("adaptive off", "adaptive=off", 0);
    parse_suffix("adaptive with fec", "fec=on adaptive=on", 0);
    parse_suffix("adaptive empty", "adaptive=", TBT_PARSE_EINVAL);
    parse_suffix("adaptive maybe", "adaptive=maybe", TBT_PARSE_EINVAL);
    parse_suffix("adaptive numeric", "adaptive=1", TBT_PARSE_EINVAL);
    parse_suffix("adaptive uppercase", "adaptive=ON", TBT_PARSE_EINVAL);
    parse_suffix("adaptive trailing token", "fec=on adaptive=on extra",
                 TBT_PARSE_EINVAL);

    parse_suffix("L zero", "L=0", 0);
    parse_suffix("L max", "L=4096", 0);
    parse_suffix("L over max", "L=4097", TBT_PARSE_EINVAL);
    parse_suffix("L u16 overflow", "L=65536", TBT_PARSE_EINVAL);
    parse_suffix("L negative", "L=-1", TBT_PARSE_EINVAL);
    parse_suffix("L empty", "L=", TBT_PARSE_EINVAL);
    parse_suffix("L hex", "L=0x10", 0);
    parse_suffix("L trailing", "L=16x", TBT_PARSE_EINVAL);

    parse_suffix("mode sc-ldpc", "mode=sc-ldpc", 0);
    parse_suffix("mode sc-ldpc w=128", "mode=sc-ldpc w=128", 0);
    parse_suffix("mode sc-ldpc w=256", "mode=sc-ldpc w=256", 0);
    parse_suffix("mode sc-ldpc w=512", "mode=sc-ldpc w=512", 0);
    parse_suffix("mode sc-ldpc w=100", "mode=sc-ldpc w=100",
                 TBT_PARSE_EINVAL);
    parse_suffix("mode uppercase", "mode=SC-LDPC", TBT_PARSE_EINVAL);
    parse_suffix("mode glued", "mode=scldpc", TBT_PARSE_EINVAL);
    parse_suffix("mode suffix", "mode=sc-ldpcx", TBT_PARSE_EINVAL);
    parse_suffix("mode empty", "mode=", TBT_PARSE_EINVAL);

    parse_expect("downlink adaptive profile",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=128 m=128 fec=on mode=sc-ldpc adaptive=on w=256"
                 " carriers=256",
                 0);
    parse_expect("uplink adaptive profile",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=8 m=24 fec=on mode=sc-ldpc adaptive=on w=128"
                 " carriers=256 L=16",
                 0);
    parse_expect("uplink adaptive L wrong",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=8 m=24 fec=on mode=sc-ldpc adaptive=on w=128"
                 " carriers=256 L=15",
                 TBT_PARSE_EINVAL);
    parse_expect("adaptive off-ladder n/m",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=14 m=18 fec=on mode=sc-ldpc adaptive=on w=128"
                 " carriers=256 L=16",
                 TBT_PARSE_EINVAL);

    for (i = 0; i < PARSE_FUZZ; i++)
    {
        static const char *const base[] = {
            "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
            " n=4 m=1 fec=off adaptive=off mode=rlnc w=8 carriers=5"
            " crypto=off crypto_key=- dup=1 profile=http-kal keepalive=5000"
            " drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000"
            " hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0"
            " tx_retain_bytes=0 carrier_stall_ms=0 portblock=40000-44095",
            "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
            " n=128 m=128 fec=on adaptive=on mode=sc-ldpc w=256"
            " carriers=256 portblock=40000-44095",
            "tunnel 2 role=far listen=10.0.0.1:1 target=10.0.0.2:2"
            " n=8 m=24 fec=on adaptive=on mode=sc-ldpc w=128 carriers=256"
            " L=16",
        };
        size_t len;

        case_no = i + 1;
        snprintf(line, sizeof(line), "%s", base[i % 3]);
        len = strlen(line);
        if ((rnd64() & 3) && len)
        {
            line[rnd_below(len)] = charset[rnd_below(sizeof(charset) - 1)];
        }
        else if (!(rnd64() & 1))
        {
            snprintf(line + len, sizeof(line) - len, " %s",
                     key_values[rnd_below(sizeof(key_values) /
                                          sizeof(key_values[0]))]);
        }
        else
        {
            size_t j, n = rnd_below(160);

            for (j = 0; j < n; j++)
                line[j] = charset[rnd_below(sizeof(charset) - 1)];
            line[n] = '\0';
        }
        parse_case(line, "random config line");
    }
}

int main(void)
{
    test_bounds_table();
    test_matrix();
    test_adversarial();
    test_random_buffers();
    test_body_lengths();
    test_alloc_body();
    test_cfg_body();
    test_parse_bomb();

    printf("test_fuzz: seed=1 checks=%ld failures=%d buffers=%d bodies=%d"
           " alloc=%d cfg=%d parse=%d\n",
           checks, failures, FUZZ_BUFFERS, FUZZ_BODY_LENGTHS, ALLOC_FUZZ,
           CFG_FUZZ, PARSE_FUZZ);
    return failures ? 1 : 0;
}
