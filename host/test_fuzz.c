/* test_fuzz.c - hostile-input tests for the two peer-controlled parsers:
 * tbt_rec_scan() (record framing) and tbt_hdr_check() (header validation).
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
 *
 *   make host-test          # normal build
 *   make host-test-san      # the same, with ASan and UBSan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_rec.h"

#define FUZZ_BUFFERS 200000
#define FUZZ_BODY_LENGTHS 200000

/* Documented body sizes (DESIGN.md §3.3). Repeated as literals on purpose:
 * the test must fail if the header's constants drift from the design. */
#define DOC_DATA_MAX 8192u
#define DOC_FBK_ENTRY 18u
#define DOC_FBK_MAX_ENTRIES 16u
#define DOC_WINDOW_LEN 138u /* 8 + 8*16 + 2 */
#define DOC_HELLO_LEN 23u   /* 1 + 1 + 16 + 2 + 2 + 1 */

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
    case TBT_F_FIN:
    case TBT_F_RST:
    case TBT_F_PAD:
        return len == 0;
    case TBT_F_RETIRE:
        return len == 1; /* u8 carrier index; seq is the generation */
    case TBT_F_SACK:
        return len == 1; /* u8 state */
    case TBT_F_FBK:
        return ackc <= DOC_FBK_MAX_ENTRIES && len == (unsigned)ackc * DOC_FBK_ENTRY;
    case TBT_F_WINDOW:
        return len == DOC_WINDOW_LEN;
    case TBT_F_HELLO:
        return len == DOC_HELLO_LEN;
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
        { TBT_F_OPEN, 0, 0 },
        { TBT_F_FIN, 0, 0 },
        { TBT_F_RST, 0, 0 },
        { TBT_F_FBK, 0, DOC_FBK_ENTRY * DOC_FBK_MAX_ENTRIES },
        { TBT_F_WINDOW, DOC_WINDOW_LEN, DOC_WINDOW_LEN },
        { TBT_F_HELLO, DOC_HELLO_LEN, DOC_HELLO_LEN },
        { TBT_F_PAD, 0, 0 },
        { TBT_F_RETIRE, 1, 1 },
        { TBT_F_SACK, 1, 1 },
    };
    size_t i;
    u16 min = 0xffff, max = 0xffff;
    char d[64];

    case_no++;
    check(TBT_HDR_LEN == 32 && TBT_CHUNK_MAX == 8228 && TBT_REC_MAX == 8224 && TBT_CONV == 0x0005,
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

    /* path_id names a carrier: 0..TBT_LEGACY_MAX_CARRIERS-1. The old ARQ
     * record stays at 16 even though the carrier layer grows to 256. */
    rec[TBT_H_PATH] = TBT_LEGACY_MAX_CARRIERS - 1;
    case_no++;
    expect("path_id at the limit", rec, n, TBT_REC_OK);
    rec[TBT_H_PATH] = TBT_LEGACY_MAX_CARRIERS;
    case_no++;
    expect("path_id over the limit", rec, n, TBT_REC_PROTO);
    rec[TBT_H_PATH] = 0xff;
    case_no++;
    expect("path_id 0xff", rec, n, TBT_REC_PROTO);
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
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, 0, 1);
    case_no++;
    expect("OPEN with a body", rec, n, TBT_REC_PROTO);
    n = rec_make(rec, TBT_F_FIN, 0, 0, 1, 0, 1);
    case_no++;
    expect("FIN with a body", rec, n, TBT_REC_PROTO);
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
    expect("HELLO one byte long", rec, n, TBT_REC_PROTO);

    /* seq wrap: no rule depends on seq, and the value must survive. */
    n = rec_make(rec, TBT_F_DATA, 0, 0, 1, ~(u64)0, 1);
    case_no++;
    expect("DATA at seq 2^64-1", rec, n, TBT_REC_OK);
    st = -1;
    used = scan_copy(rec, n, &r, &st);
    case_no++;
    check(st == TBT_REC_OK && used == n && r.hdr.seq == ~(u64)0, "seq wrap survives decode", NULL);
    n = rec_make(rec, TBT_F_OPEN, 0, 0, 1, ~(u64)0, 0);
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
    n = rec_make(two, TBT_F_OPEN, 0, 0, 7, 0, 0);
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

int main(void)
{
    test_bounds_table();
    test_matrix();
    test_adversarial();
    test_random_buffers();
    test_body_lengths();

    printf("test_fuzz: seed=1 checks=%ld failures=%d buffers=%d bodies=%d\n", checks, failures, FUZZ_BUFFERS,
           FUZZ_BODY_LENGTHS);
    return failures ? 1 : 0;
}
