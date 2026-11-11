/* test_rcvq_wire.c - byte-identity check for tbt_rcvq_rebuild_wire().
 *
 * The sender path is reproduced literally: seal, pack, copy the clear
 * payload, encrypt DATA/PARITY bodies with the TX context, then
 * tbt_http_fec_chunk_encode() for the HTTP profiles or the bare record for
 * profile 3.  The rebuild must produce the same bytes from the recovered
 * plaintext with an RX context built from the same key and nonce.
 *
 * Matrix: payload len {1, 137, 1200, 8192} x {DATA, PARITY} x crypto
 * {off, on} x profile {1, 2, 3}.  Every case checks the wire length against
 * hexdigits(48 + len) + 2 + (48 + len) + 2 (HTTP) or 48 + len (raw), then
 * memcmp.
 *
 * This file has no main(): host/test_rcvq.c calls test_rcvq_wire() as one
 * part of the receive-queue suite.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_rcvq.h"
#include "tbt_http.h"

static int checks;
static int failures;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "",
           detail ? detail : "");
}

static void ok_size(size_t got, size_t want, const char *what)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "got %zu, want %zu", got, want);
    ok(got == want, what, buf);
}

static void ok_rc(int got, int want, const char *what)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "got %d, want %d", got, want);
    ok(got == want, what, buf);
}

static void *xalloc(size_t len)
{
    void *p = malloc(len ? len : 1);

    if (!p)
    {
        fprintf(stderr, "test_rcvq_wire: out of memory\n");
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
        x = x * 1664525u + 1013904223u;
        p[i] = (u8)(x >> 24);
    }
}

/* A plausible share header.  Structural validity is not what this test
 * measures: the share is assumed to have come out of FEC recovery, and the
 * rebuild must reproduce whatever header fields the sender sealed. */
static void make_hdr(struct tbt_fec_hdr *h, u8 type, u16 len)
{
    memset(h, 0, sizeof(*h));
    h->type = type;
    h->flags = TBT_FEC_FF_LAST;
    h->n = 5;
    h->m = 3;
    h->block_id = 42;
    h->payload_len = len;
    h->conv = TBT_FEC_CONV;
    h->sid = 0x000000a1b2c3d4e5ULL;
    if (type == TBT_FEC_PARITY)
    {
        h->share_index = (u16)(h->n + 2);
        h->seq = h->block_id * h->n;
    }
    else
    {
        h->share_index = 2;
        h->seq = h->block_id * h->n + h->share_index;
    }
}

/* The sender, step for step.  `tx` is NULL when crypto is off. */
static size_t sender_wire(const struct tbt_fec_hdr *h, const u8 *plain,
                          struct tbt_crypto_ctx *tx, u8 profile, u8 *wire,
                          size_t cap)
{
    struct tbt_fec_hdr hh = *h;
    u8 rec[TBT_FEC_REC_MAX];
    u16 len = h->payload_len;
    u16 rec_len;

    if (tbt_fec_hdr_seal(&hh, plain))
        return 0;
    if (tbt_fec_hdr_pack(&hh, rec))
        return 0;
    if (len)
        memcpy(rec + TBT_FEC_HDR_LEN, plain, len);
    if (tx && (hh.type == TBT_FEC_DATA || hh.type == TBT_FEC_PARITY))
    {
        if (tbt_crypto_body_xor_flow(tx, hh.sid, hh.seq,
                                     hh.type == TBT_FEC_PARITY,
                                     hh.share_index,
                                     rec + TBT_FEC_HDR_LEN, len))
            return 0;
    }
    rec_len = (u16)(TBT_FEC_HDR_LEN + len);
    if (profile == TBT_RCVQ_PROFILE_RAW)
    {
        if (cap < rec_len)
            return 0;
        memcpy(wire, rec, rec_len);
        return rec_len;
    }
    return tbt_http_fec_chunk_encode((char *)wire, cap, rec, rec_len);
}

static size_t hex_digits(size_t v)
{
    size_t n = 1;

    while (v > 0xf)
    {
        v >>= 4;
        n++;
    }
    return n;
}

static size_t http_wire_len(size_t rec_len)
{
    return hex_digits(rec_len) + 2 + rec_len + 2;
}

static const char *type_name(u8 type)
{
    return type == TBT_FEC_PARITY ? "PARITY" : "DATA";
}

static void run_case(u16 len, u8 type, bool crypto, u8 profile)
{
    static const u8 key[TBT_CRYPTO_KEY_LEN] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
        0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
    };
    static const u8 nonce[TBT_CRYPTO_NONCE_LEN] = {
        0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67,
    };
    const size_t cap = TBT_HTTP_FEC_FRAME_MAX + 8;
    struct tbt_fec_hdr h;
    struct tbt_crypto_ctx tx;
    struct tbt_crypto_ctx rx;
    struct tbt_crypto_ctx *txp = NULL;
    struct tbt_crypto_ctx *rxp = NULL;
    u8 *plain = xalloc(len);
    u8 *wire = xalloc(cap);
    u8 *out = xalloc(cap);
    size_t want;
    size_t sent;
    u16 out_len = 0;
    int rc;
    char what[160];

    fill_pattern(plain, len,
                 (u32)len ^ (u32)type ^ (crypto ? 0x10000u : 0u) ^ profile);
    make_hdr(&h, type, len);

    if (crypto)
    {
        ok_rc(tbt_crypto_init(&tx, key, nonce, 0), 0, "tx crypto init");
        ok_rc(tbt_crypto_init(&rx, key, nonce, 0), 0, "rx crypto init");
        txp = &tx;
        rxp = &rx;
    }

    memset(wire, 0, cap);
    sent = sender_wire(&h, plain, txp, profile, wire, cap);
    want = profile == TBT_RCVQ_PROFILE_RAW ? (size_t)TBT_FEC_HDR_LEN + len
                                           : http_wire_len(TBT_FEC_HDR_LEN + len);
    snprintf(what, sizeof(what), "sender len %u type %s crypto %s profile %u",
             (unsigned)len, type_name(type), crypto ? "on" : "off",
             (unsigned)profile);
    ok_size(sent, want, what);

    memset(out, 0, cap);
    rc = tbt_rcvq_rebuild_wire(&h, plain, rxp, profile, out, cap, &out_len);
    snprintf(what, sizeof(what), "rebuild rc len %u type %s crypto %s profile %u",
             (unsigned)len, type_name(type), crypto ? "on" : "off",
             (unsigned)profile);
    ok_rc(rc, 0, what);
    snprintf(what, sizeof(what), "wire len len %u type %s crypto %s profile %u",
             (unsigned)len, type_name(type), crypto ? "on" : "off",
             (unsigned)profile);
    ok_size(out_len, want, what);
    snprintf(what, sizeof(what), "bytes len %u type %s crypto %s profile %u",
             (unsigned)len, type_name(type), crypto ? "on" : "off",
             (unsigned)profile);
    ok(sent == want && out_len == want && memcmp(wire, out, want) == 0, what,
       "wire bytes differ");

    free(plain);
    free(wire);
    free(out);
}

/* Empty payload: the sender passes plain == NULL; rebuild must accept it. */
static void run_empty(void)
{
    const size_t cap = TBT_HTTP_FEC_FRAME_MAX + 8;
    struct tbt_fec_hdr h;
    u8 *wire = xalloc(cap);
    u8 *out = xalloc(cap);
    size_t want;
    u16 out_len = 0;

    make_hdr(&h, TBT_FEC_DATA, 0);
    want = (size_t)TBT_FEC_HDR_LEN;
    ok_size(sender_wire(&h, NULL, NULL, TBT_RCVQ_PROFILE_RAW, wire, cap), want,
            "empty sender len");
    ok_rc(tbt_rcvq_rebuild_wire(&h, NULL, NULL, TBT_RCVQ_PROFILE_RAW, out, cap,
                                &out_len),
          0, "empty rebuild rc");
    ok_size(out_len, want, "empty wire len");
    ok(memcmp(wire, out, want) == 0, "empty wire bytes", "wire bytes differ");
    free(wire);
    free(out);
}

static void run_errors(void)
{
    const size_t cap = TBT_HTTP_FEC_FRAME_MAX + 8;
    struct tbt_fec_hdr h;
    u8 *plain = xalloc(TBT_FEC_S + 1);
    u8 *out = xalloc(cap);
    u16 out_len = 0;

    make_hdr(&h, TBT_FEC_DATA, 137);
    ok_rc(tbt_rcvq_rebuild_wire(&h, NULL, NULL, TBT_RCVQ_PROFILE_RAW, out, cap,
                                &out_len),
          -EINVAL, "NULL plain with payload");
    ok_rc(tbt_rcvq_rebuild_wire(NULL, plain, NULL, TBT_RCVQ_PROFILE_RAW, out,
                                cap, &out_len),
          -EINVAL, "NULL header");
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, TBT_RCVQ_PROFILE_RAW, NULL,
                                cap, &out_len),
          -EINVAL, "NULL output");
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, TBT_RCVQ_PROFILE_RAW, out,
                                cap, NULL),
          -EINVAL, "NULL out_len");
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, 0, out, cap, &out_len),
          -EINVAL, "profile 0");
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, 4, out, cap, &out_len),
          -EINVAL, "profile 4");

    make_hdr(&h, TBT_FEC_DATA, TBT_FEC_S + 1);
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, TBT_RCVQ_PROFILE_RAW, out,
                                cap, &out_len),
          -EINVAL, "payload above S");

    make_hdr(&h, TBT_FEC_DATA, 137);
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, TBT_RCVQ_PROFILE_RAW, out,
                                TBT_FEC_HDR_LEN + 137 - 1, &out_len),
          -ENOSPC, "raw cap one short");
    ok_rc(tbt_rcvq_rebuild_wire(&h, plain, NULL, TBT_RCVQ_PROFILE_HTTP, out,
                                http_wire_len(TBT_FEC_HDR_LEN + 137) - 1,
                                &out_len),
          -ENOSPC, "HTTP cap one short");

    free(plain);
    free(out);
}

int test_rcvq_wire(void)
{
    static const u16 lens[] = {1, 137, 1200, TBT_FEC_S};
    static const u8 types[] = {TBT_FEC_DATA, TBT_FEC_PARITY};
    static const u8 profiles[] = {TBT_RCVQ_PROFILE_HTTP, 2u,
                                  TBT_RCVQ_PROFILE_RAW};
    size_t i;
    size_t t;
    size_t p;
    int crypto;

    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
        for (t = 0; t < sizeof(types) / sizeof(types[0]); t++)
            for (crypto = 0; crypto < 2; crypto++)
                for (p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++)
                    run_case(lens[i], types[t], crypto != 0, profiles[p]);

    run_empty();
    run_errors();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures)
        return 1;
    printf("WIRE_TEST_PASS\n");
    return 0;
}
