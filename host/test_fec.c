/* test_fec.c - host conformance test for the FEC v6 wire and RLNC codec.
 *
 * Links tbtun/tbt_fec.c, the same object the module builds, so a wire rule
 * that passes here is a rule the kernel enforces. No root, no module, no
 * kernel headers, no fixture files: it runs in milliseconds and exits
 * non-zero on the first broken rule (it keeps going, to list every failure).
 *
 * The layout is frozen by T0 (review/goal-v6-interfaces.md). A failing check
 * here means the wire or the codec contract changed, not that the test is
 * stale.
 *
 *   make host-test
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"

static int failures;
static int checks;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static void ok_int(int got, int want, const char *what)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "got %d, want %d", got, want);
    ok(got == want, what, buf);
}

static void ok_u32(u32 got, u32 want, const char *what)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "got %u, want %u", got, want);
    ok(got == want, what, buf);
}

static void ok_u64(u64 got, u64 want, const char *what)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "got %llu, want %llu", (unsigned long long)got,
             (unsigned long long)want);
    ok(got == want, what, buf);
}

static void ok_u16le(const u8 *p, u16 v, const char *what)
{
    ok(p[0] == (u8)v && p[1] == (u8)(v >> 8), what, "u16 is not little-endian");
}

static void ok_u32le(const u8 *p, u32 v, const char *what)
{
    ok(p[0] == (u8)v && p[1] == (u8)(v >> 8) && p[2] == (u8)(v >> 16) &&
           p[3] == (u8)(v >> 24),
       what, "u32 is not little-endian");
}

static void ok_u64le(const u8 *p, u64 v, const char *what)
{
    int i;

    for (i = 0; i < 8; i++)
        if (p[i] != (u8)(v >> (8 * i)))
        {
            ok(0, what, "u64 is not little-endian");
            return;
        }
    ok(1, what, NULL);
}

static void *xalloc(size_t n)
{
    void *p = calloc(1, n);

    if (!p)
    {
        fprintf(stderr, "test_fec: out of memory\n");
        exit(2);
    }
    return p;
}

static void fill_pattern(u8 *p, size_t n, u32 seed)
{
    u32 x = seed * 2654435761u + 0x9e3779b9u;
    size_t i;

    for (i = 0; i < n; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (u8)(x >> 24);
    }
}

/* ------------------------------------------------------------------ *
 * 1. 40-byte header pack/unpack, boundary values, little-endian bytes.
 * ------------------------------------------------------------------ */

static int hdr_eq(const struct tbt_fec_hdr *a, const struct tbt_fec_hdr *b)
{
    return a->type == b->type && a->flags == b->flags &&
           a->share_index == b->share_index && a->n == b->n && a->m == b->m &&
           a->epoch == b->epoch && a->block_id == b->block_id && a->seq == b->seq &&
           a->win_len == b->win_len && a->payload_len == b->payload_len &&
           a->conv == b->conv && a->reserved == b->reserved &&
           a->payload_crc32c == b->payload_crc32c &&
           a->header_crc32c == b->header_crc32c;
}

static void hdr_roundtrip(const char *what, const struct tbt_fec_hdr *in)
{
    struct tbt_fec_hdr out;
    u8 wire[TBT_FEC_HDR_LEN];

    memset(&out, 0xa5, sizeof(out));
    ok_int(tbt_fec_hdr_pack(in, wire), 0, what);
    ok_int(tbt_fec_hdr_unpack(wire, &out), 0, what);
    ok(hdr_eq(in, &out), what, "round-trip changed a field");
}

static void test_header_roundtrip(void)
{
    struct tbt_fec_hdr lo, hi, le;
    u8 wire[TBT_FEC_HDR_LEN];

    memset(&lo, 0, sizeof(lo));
    lo.type = TBT_FEC_DATA;
    lo.flags = 0;
    lo.share_index = 0;
    lo.n = 1;
    lo.m = 1;
    lo.epoch = 0;
    lo.block_id = 0;
    lo.seq = 0;
    lo.win_len = 0;
    lo.payload_len = 0;
    lo.conv = TBT_FEC_CONV;
    lo.reserved = 0;
    lo.payload_crc32c = 0;
    lo.header_crc32c = 0;
    hdr_roundtrip("header low boundary", &lo);

    memset(&hi, 0, sizeof(hi));
    hi.type = TBT_FEC_PARITY;
    hi.flags = TBT_FEC_FF_LAST;
    hi.share_index = 255;
    hi.n = 224;
    hi.m = 32;
    hi.epoch = 65535;
    hi.block_id = ~(u64)0;
    hi.seq = (1ULL << 63) - 1;
    hi.win_len = 1024;
    hi.payload_len = 8192;
    hi.conv = TBT_FEC_CONV;
    hi.reserved = 0;
    hi.payload_crc32c = 0xdeadbeefu;
    hi.header_crc32c = 0x12345678u;
    hdr_roundtrip("header high boundary", &hi);

    /* Every multi-byte field at a non-symmetric value: the packed bytes must
     * be little-endian at the documented offsets. */
    memset(&le, 0, sizeof(le));
    le.type = TBT_FEC_DATA;
    le.flags = TBT_FEC_FF_LAST;
    le.share_index = 0x1234;
    le.n = 0xe0; /* 224 */
    le.m = 0x20; /* 32 */
    le.epoch = 0xbeef;
    le.block_id = 0x1122334455667788ULL;
    le.seq = 0x0102030405060708ULL;
    le.win_len = 0x0400;   /* 1024 */
    le.payload_len = 0x2000; /* 8192 */
    le.conv = TBT_FEC_CONV;
    le.reserved = 0;
    le.payload_crc32c = 0xdeadbeefu;
    le.header_crc32c = 0x12345678u;

    ok_int(tbt_fec_hdr_pack(&le, wire), 0, "header LE pack");
    ok(wire[TBT_FEC_H_TYPE] == TBT_FEC_DATA, "type at +0", NULL);
    ok(wire[TBT_FEC_H_FLAGS] == TBT_FEC_FF_LAST, "flags at +1", NULL);
    ok_u16le(wire + TBT_FEC_H_SHARE, le.share_index, "share_index at +2");
    ok(wire[TBT_FEC_H_N] == le.n && wire[TBT_FEC_H_M] == le.m, "n/m at +4/+5", NULL);
    ok_u16le(wire + TBT_FEC_H_EPOCH, le.epoch, "epoch at +6");
    ok_u64le(wire + TBT_FEC_H_BLOCK, le.block_id, "block_id at +8");
    ok_u64le(wire + TBT_FEC_H_SEQ, le.seq, "seq at +16");
    ok_u16le(wire + TBT_FEC_H_WINLEN, le.win_len, "win_len at +24");
    ok_u16le(wire + TBT_FEC_H_PAYLEN, le.payload_len, "payload_len at +26");
    ok_u16le(wire + TBT_FEC_H_CONV, le.conv, "conv at +28");
    ok_u16le(wire + TBT_FEC_H_RESERVED, le.reserved, "reserved at +30");
    ok_u32le(wire + TBT_FEC_H_PAYCRC, le.payload_crc32c, "payload_crc at +32");
    ok_u32le(wire + TBT_FEC_H_HDRCRC, le.header_crc32c, "header_crc at +36");

    /* The offsets themselves are part of the ABI. */
    ok(TBT_FEC_HDR_LEN == 40 && TBT_FEC_H_TYPE == 0 && TBT_FEC_H_FLAGS == 1 &&
           TBT_FEC_H_SHARE == 2 && TBT_FEC_H_N == 4 && TBT_FEC_H_M == 5 &&
           TBT_FEC_H_EPOCH == 6 && TBT_FEC_H_BLOCK == 8 && TBT_FEC_H_SEQ == 16 &&
           TBT_FEC_H_WINLEN == 24 && TBT_FEC_H_PAYLEN == 26 && TBT_FEC_H_CONV == 28 &&
           TBT_FEC_H_RESERVED == 30 && TBT_FEC_H_PAYCRC == 32 && TBT_FEC_H_HDRCRC == 36,
       "header offsets", NULL);
    /* 0x1003 grows the ACK body to 24 bytes and adds deliver_bytes. */
    ok(TBT_FEC_CONV == 0x1004u, "TBT_FEC_CONV", NULL);

    /* NULL arguments are refused, not dereferenced. */
    ok_int(tbt_fec_hdr_pack(NULL, wire), -EINVAL, "pack NULL header");
    ok_int(tbt_fec_hdr_unpack(NULL, &lo), -EINVAL, "unpack NULL bytes");
    ok_int(tbt_fec_hdr_unpack(wire, NULL), -EINVAL, "unpack NULL header");
}

/* ------------------------------------------------------------------ *
 * 2. Structural check.
 * ------------------------------------------------------------------ */

static struct tbt_fec_hdr data_hdr(void)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.n = 4;
    h.m = 4;
    h.epoch = 0; /* DATA never carries the block epoch */
    h.block_id = 1;
    h.seq = 1;
    h.payload_len = 64;
    h.conv = TBT_FEC_CONV;
    return h;
}

static struct tbt_fec_hdr parity_hdr(void)
{
    struct tbt_fec_hdr h = data_hdr();

    h.type = TBT_FEC_PARITY;
    h.share_index = h.n; /* first parity share */
    h.win_len = h.n;
    h.seq = 7;
    return h;
}

static struct tbt_fec_hdr ctl_hdr(u8 type, u16 len)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = type;
    h.payload_len = len;
    h.conv = TBT_FEC_CONV;
    return h;
}

static void test_hdr_check(void)
{
    struct tbt_fec_hdr h;

    /* DATA / PARITY bounds. */
    h = data_hdr();
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA baseline");
    h = data_hdr();
    h.flags = TBT_FEC_FF_LAST;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA LAST flag");

    h = data_hdr();
    h.n = 224;
    h.m = 32;
    h.share_index = 255;
    h.type = TBT_FEC_PARITY;
    h.win_len = 8;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "n+m=256 share 255 (u16, no truncation)");

    h = parity_hdr();
    h.n = 255;
    h.m = 1;
    h.share_index = 255;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "n=255 m=1 share 255");

    h = data_hdr();
    h.n = 0;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "n=0");
    h = data_hdr();
    h.m = 0;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "m=0");
    h = data_hdr();
    h.n = 255;
    h.m = 2;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "n+m=257");

    h = data_hdr();
    h.n = 224;
    h.m = 32;
    h.share_index = 256;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "share_index=256 with n+m=256");
    h = data_hdr();
    h.share_index = 8;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "share_index=n+m");

    /* DATA names a data share, PARITY a parity share. */
    h = data_hdr();
    h.share_index = 3;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA share_index=n-1");
    h = data_hdr();
    h.share_index = 4;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA share_index=n");
    h = parity_hdr();
    h.share_index = 3;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY share_index=n-1");
    h = parity_hdr();
    h.share_index = 4;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY share_index=n");
    h = parity_hdr();
    h.share_index = 7;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY share_index=n+m-1");

    /* DATA payload may be short anywhere (the packetizer flushes a partial
     * packet when the application goes quiet), but never zero or over S. */
    h = data_hdr();
    h.flags = TBT_FEC_FF_LAST;
    h.payload_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA payload_len=1 with LAST");
    h = data_hdr();
    h.payload_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA payload_len=1 without LAST");
    h = data_hdr();
    h.payload_len = 63;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA short payload without LAST");
    h = data_hdr();
    h.flags = TBT_FEC_FF_LAST;
    h.payload_len = 64;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA full payload with LAST");
    h = data_hdr();
    h.payload_len = 0;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA payload_len=0");
    h = data_hdr();
    h.payload_len = 65;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA payload_len=S+1");
    h = data_hdr();
    h.payload_len = 8192;
    ok_int(tbt_fec_hdr_check(&h, 8192), 0, "DATA payload_len=S=8192");
    h = data_hdr();
    h.payload_len = 8192;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA payload_len over configured S");

    /* epoch is block-level and PARITY only: a DATA record that carries it
     * could overwrite the block's last-share length mid-block. */
    h = data_hdr();
    h.epoch = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA epoch=1 refused");
    h = data_hdr();
    h.epoch = 64;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA epoch=S refused");
    h = parity_hdr();
    h.epoch = 64;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY epoch=S");
    h = parity_hdr();
    h.epoch = 65;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY epoch=S+1");

    h = data_hdr();
    h.win_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA win_len != 0");
    h = data_hdr();
    h.seq = 1ULL << 63;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "DATA seq=2^63");
    h = data_hdr();
    h.seq = (1ULL << 63) - 1;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "DATA seq=2^63-1");

    h = data_hdr();
    h.conv = 0x1000;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "bad conv");
    h = data_hdr();
    h.conv = 0x1001;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "0.1.0 FEC conv refused");
    h = data_hdr();
    h.flags = 0x02;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "flags bit1");
    h = data_hdr();
    h.flags = 0x03;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "flags bit0+bit1");
    h = data_hdr();
    h.reserved = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "reserved != 0");
    h = data_hdr();
    h.type = 0x36;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "unknown type 0x36");
    h = data_hdr();
    h.type = 0x00;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "unknown type 0x00");
    h = data_hdr();
    ok_int(tbt_fec_hdr_check(&h, 0), -EPROTO, "S=0 for a share");
    h = data_hdr();
    ok_int(tbt_fec_hdr_check(&h, 8193), -EPROTO, "S over TBT_FEC_PAYLOAD_MAX");
    ok_int(tbt_fec_hdr_check(NULL, 64), -EPROTO, "NULL header");

    /* PARITY: payload S for a real share; win_len 0 is the empty marker. */
    h = parity_hdr();
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY baseline");
    h = parity_hdr();
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY marker win_len=0");
    h = parity_hdr();
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "marker with epoch != 0");
    h = parity_hdr();
    h.win_len = 0;
    h.payload_len = 1;
    h.epoch = 0;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "marker with payload");
    h = parity_hdr();
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.share_index = (u16)(h.n + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "marker is parity 0");
    h = parity_hdr();
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.flags = TBT_FEC_FF_LAST;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "marker with flags");
    h = parity_hdr();
    h.win_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY win_len=1");
    h = parity_hdr();
    h.win_len = h.n;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "PARITY win_len=n");
    h = parity_hdr();
    h.win_len = (u16)(h.n + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY win_len=n+1");
    h = parity_hdr();
    h.win_len = TBT_FEC_MAX_W;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY win_len=TBT_FEC_MAX_W > n");
    h = parity_hdr();
    h.payload_len = 63;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY payload_len != S");
    h = parity_hdr();
    h.payload_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY short payload");
    h = parity_hdr();
    h.seq = 1ULL << 63;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "PARITY seq=2^63");

    /* Control frames carry no share and no block_id. */
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "ACK baseline");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.seq = 12345;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "ACK seq is next_deliver");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.n = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK n != 0");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.m = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK m != 0");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.share_index = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK share_index != 0");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.win_len = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK win_len != 0");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    h.block_id = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK block_id != 0");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN - 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK short body");
    h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "ACK long body");

    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN);
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "NACK 64");
    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN - 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "NACK 63");
    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "NACK 65");
    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN);
    h.reserved = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "NACK with header reserved != 0");
    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN);
    h.seq = 42;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "NACK seq must be 0");
    h = ctl_hdr(TBT_FEC_NACK, TBT_FEC_NACK_LEN);
    h.block_id = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "NACK names its block in block_id");

    h = ctl_hdr(TBT_FEC_CFG, TBT_FEC_CFG_LEN);
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "CFG 32");
    h = ctl_hdr(TBT_FEC_CFG, TBT_FEC_CFG_LEN - 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "CFG 31");
    h = ctl_hdr(TBT_FEC_CFG, TBT_FEC_CFG_LEN + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "CFG 33");
    h = ctl_hdr(TBT_FEC_CFG, TBT_FEC_CFG_LEN);
    h.seq = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "CFG seq != 0");

    h = ctl_hdr(TBT_FEC_FBK, TBT_FEC_FBK_LEN);
    ok_int(tbt_fec_hdr_check(&h, 64), 0, "FBK 48");
    h = ctl_hdr(TBT_FEC_FBK, TBT_FEC_FBK_LEN - 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "FBK 47");
    h = ctl_hdr(TBT_FEC_FBK, TBT_FEC_FBK_LEN + 1);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "FBK 49");
    h = ctl_hdr(TBT_FEC_FBK, TBT_FEC_FBK_LEN);
    h.seq = 1;
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "FBK seq != 0");

    /* TBT_FEC_PAD (0x23) is gone: the 40-byte FEC parser no longer accepts
     * the type, and 0x23 belongs to the old 32-byte container only. */
    h = ctl_hdr(0x23, 0);
    ok_int(tbt_fec_hdr_check(&h, 64), -EPROTO, "FEC PAD 0x23 refused");
}

/* ------------------------------------------------------------------ *
 * 2b. ACK frontier -> byte offset: hostile u64 must clamp, never wrap.
 * ------------------------------------------------------------------ */

static void test_ack_bytes(void)
{
    ok_u64(tbt_fec_ack_bytes(0, 0, 0), 0, "ack 0/0/0");
    ok_u64(tbt_fec_ack_bytes(0, 100, 1000), 0, "frontier before pkt_base");
    ok_u64(tbt_fec_ack_bytes(100, 100, 0), 0, "frontier == base, tx_seq 0");
    ok_u64(tbt_fec_ack_bytes(1, 0, 8192), 8192, "one full packet");
    ok_u64(tbt_fec_ack_bytes(2, 0, 8192), 8192, "frontier past tx_seq clamps");
    ok_u64(tbt_fec_ack_bytes(UINT64_MAX, 0, 1000), 1000, "U64_MAX frontier");
    ok_u64(tbt_fec_ack_bytes(UINT64_MAX, 100, 1000), 1000, "U64_MAX with base");
    ok_u64(tbt_fec_ack_bytes(UINT64_MAX, UINT64_MAX, 1000), 0,
           "U64_MAX frontier and base");
    ok_u64(tbt_fec_ack_bytes(9, 0, 81920), 73728, "rel below tx_seq");
    ok_u64(tbt_fec_ack_bytes(10, 0, 81920), 81920, "rel at tx_seq");
    ok_u64(tbt_fec_ack_bytes(11, 0, 81920), 81920, "rel past tx_seq");
    ok_u64(tbt_fec_ack_bytes(3, 0, 16384), 16384, "saturate one past limit");
    ok_u64(tbt_fec_ack_bytes(2, 0, 16384), 16384, "rel at tx_seq/S");
    ok_u64(tbt_fec_ack_bytes(UINT64_MAX / 2, 0, UINT64_MAX / 2),
           UINT64_MAX / 2, "huge frontier saturates to tx_seq");
    ok_u64(tbt_fec_ack_bytes(105, 100, 5 * 8192), 5 * 8192,
           "stream-relative base");
    ok_u64(tbt_fec_ack_bytes(106, 100, 5 * 8192), 5 * 8192,
           "stream-relative overrun clamps");
}

static void test_fin_valid(void)
{
    ok(tbt_fec_fin_valid(0, 0, 0), "FIN 0/0 span 0", NULL);
    ok(tbt_fec_fin_valid(8, 0, 32), "FIN base 0 aligned", NULL);
    ok(tbt_fec_fin_valid(64, 32, 32), "FIN base 32 aligned", NULL);
    ok(!tbt_fec_fin_valid(31, 32, 32), "FIN end before base", NULL);
    ok(tbt_fec_fin_valid(32, 32, 32), "FIN empty range", NULL);
    ok(!tbt_fec_fin_valid(64, 33, 32), "FIN base not block-aligned", NULL);
    ok(tbt_fec_fin_valid(64, 33, 0), "FIN base unaligned without span", NULL);
    ok(!tbt_fec_fin_valid(0, UINT64_MAX, 0), "FIN U64_MAX base", NULL);
    ok(tbt_fec_fin_valid(UINT64_MAX, 0, 0), "FIN U64_MAX end", NULL);
}

/* ------------------------------------------------------------------ *
 * 3. CRC32C, seal, verify, and the header-before-payload order.
 * ------------------------------------------------------------------ */

static void test_crc(void)
{
    static const char vec[] = "123456789";
    struct tbt_fec_hdr h, h2;
    u8 payload[64];
    u8 wire[TBT_FEC_HDR_LEN];
    u8 fix[TBT_FEC_HDR_LEN];

    ok_u32(tbt_fec_crc32c(0, vec, 9), 0xe3069283u, "CRC32C vector");
    ok_u32(tbt_fec_crc32c(tbt_fec_crc32c(0, vec, 4), vec + 4, 5), 0xe3069283u,
           "CRC32C chaining");
    ok_u32(tbt_fec_crc32c(0, vec, 0), 0, "CRC32C empty keeps seed");

    h = data_hdr();
    h.payload_len = sizeof(payload);
    fill_pattern(payload, sizeof(payload), 7);
    ok_int(tbt_fec_hdr_seal(&h, payload), 0, "seal");
    ok_int(tbt_fec_hdr_verify(&h, payload), 0, "verify accepts");

    payload[10] ^= 0x01;
    ok_int(tbt_fec_hdr_verify(&h, payload), -EBADMSG, "payload CRC mismatch");
    payload[10] ^= 0x01;

    ok_int(tbt_fec_hdr_pack(&h, wire), 0, "pack for header flip");
    wire[TBT_FEC_H_EPOCH] ^= 0xff;
    ok_int(tbt_fec_hdr_unpack(wire, &h2), 0, "unpack flipped header");
    ok_int(tbt_fec_hdr_verify(&h2, payload), -EBADMSG, "header CRC mismatch");

    /* Corrupt both. The header check runs first, so the result is still
     * -EBADMSG; repairing the header CRC must then expose the payload. */
    payload[10] ^= 0x01;
    ok_int(tbt_fec_hdr_verify(&h2, payload), -EBADMSG, "both CRCs corrupt");
    h2.header_crc32c = 0;
    ok_int(tbt_fec_hdr_pack(&h2, fix), 0, "pack corrupted header");
    h2.header_crc32c = tbt_fec_crc32c(0, fix, TBT_FEC_H_HDRCRC);
    ok_int(tbt_fec_hdr_verify(&h2, payload), -EBADMSG,
           "payload still fails after the header CRC is repaired");

    /* A NULL payload is legal only when payload_len is zero. */
    h = data_hdr();
    h.payload_len = 0;
    ok_int(tbt_fec_hdr_seal(&h, NULL), 0, "seal empty payload");
    ok_int(tbt_fec_hdr_verify(&h, NULL), 0, "verify empty payload");
    h = data_hdr();
    h.payload_len = 4;
    ok_int(tbt_fec_hdr_seal(&h, NULL), -EINVAL, "seal NULL payload with length");
    {
        u8 small[4] = { 1, 2, 3, 4 };

        h = data_hdr();
        h.payload_len = sizeof(small);
        ok_int(tbt_fec_hdr_seal(&h, small), 0, "seal small payload");
        ok_int(tbt_fec_hdr_verify(&h, NULL), -EINVAL,
               "verify NULL payload with length");
    }
    ok_int(tbt_fec_hdr_verify(NULL, payload), -EPROTO, "verify NULL header");
}

/* ------------------------------------------------------------------ *
 * 4. 256-bit carrier bitmap.
 * ------------------------------------------------------------------ */

static void test_bitmap(void)
{
    u8 bm[TBT_FEC_BITMAP_BYTES];

    memset(bm, 0, sizeof(bm));
    ok_u32(tbt_fec_bitmap_count(bm), 0, "empty bitmap count");

    tbt_fec_bitmap_set(bm, 0);
    tbt_fec_bitmap_set(bm, 127);
    tbt_fec_bitmap_set(bm, 128);
    tbt_fec_bitmap_set(bm, 255);
    ok_u32(tbt_fec_bitmap_count(bm), 4, "four bits set");
    ok(tbt_fec_bitmap_test(bm, 0), "bit 0", NULL);
    ok(tbt_fec_bitmap_test(bm, 127), "bit 127", NULL);
    ok(tbt_fec_bitmap_test(bm, 128), "bit 128", NULL);
    ok(tbt_fec_bitmap_test(bm, 255), "bit 255", NULL);
    ok(!tbt_fec_bitmap_test(bm, 1), "bit 1 clear", NULL);
    ok(!tbt_fec_bitmap_test(bm, 256), "bit 256 is out of range", NULL);

    tbt_fec_bitmap_set(bm, 256);
    ok_u32(tbt_fec_bitmap_count(bm), 4, "set 256 is a no-op");
    ok(!tbt_fec_bitmap_test(bm, 256), "test 256 is false", NULL);
    tbt_fec_bitmap_clear(bm, 256);
    ok_u32(tbt_fec_bitmap_count(bm), 4, "clear 256 is a no-op");

    tbt_fec_bitmap_clear(bm, 127);
    ok_u32(tbt_fec_bitmap_count(bm), 3, "clear bit 127");
    ok(!tbt_fec_bitmap_test(bm, 127), "bit 127 cleared", NULL);

    memset(bm, 0xff, sizeof(bm));
    ok_u32(tbt_fec_bitmap_count(bm), 256, "all-ones bitmap count");
    memset(bm, 0, sizeof(bm));
    ok_u32(tbt_fec_bitmap_count(bm), 0, "all-zero bitmap count");
    ok_u32(tbt_fec_bitmap_count(NULL), 0, "NULL bitmap count");
}

/* ------------------------------------------------------------------ *
 * 5. Control bodies.
 * ------------------------------------------------------------------ */

static void cfg_fix_crc(u8 *buf)
{
    u32 crc = tbt_fec_crc32c(0, buf, 24);

    buf[24] = (u8)crc;
    buf[25] = (u8)(crc >> 8);
    buf[26] = (u8)(crc >> 16);
    buf[27] = (u8)(crc >> 24);
}

static void test_control(void)
{
    struct tbt_fec_ack a, a2;
    struct tbt_fec_nack nk, nk2;
    struct tbt_fec_cfg c, c2;
    struct tbt_fec_fbk f, f2;
    u8 buf[64];
    u8 cbuf[TBT_FEC_CFG_LEN];
    u8 fbuf[TBT_FEC_FBK_LEN];
    int n;

    /* ACK: 24 bytes, little-endian. The first 16 keep their 0x1002 offsets;
     * deliver_bytes sits at +16 so a reader of the frontier still parses. */
    memset(&a, 0, sizeof(a));
    a.next_deliver = 0x1122334455667788ULL;
    a.credit_packets = 0xdeadbeefu;
    a.rtt_us = 123456;
    a.deliver_bytes = 0x0102030405060708ULL;
    ok_int(tbt_fec_ack_pack(&a, buf), 0, "ACK pack");
    ok_u64le(buf + 0, a.next_deliver, "ACK next_deliver at +0");
    ok_u32le(buf + 8, a.credit_packets, "ACK credit at +8");
    ok_u32le(buf + 12, a.rtt_us, "ACK rtt at +12");
    ok_u64le(buf + 16, a.deliver_bytes, "ACK deliver_bytes at +16");
    ok(TBT_FEC_ACK_LEN == 24, "ACK body is 24 bytes", NULL);
    memset(&a2, 0, sizeof(a2));
    ok_int(tbt_fec_ack_unpack(buf, &a2), 0, "ACK unpack");
    ok(a2.next_deliver == a.next_deliver && a2.credit_packets == a.credit_packets &&
           a2.rtt_us == a.rtt_us && a2.deliver_bytes == a.deliver_bytes,
       "ACK round-trip", NULL);
    ok_int(tbt_fec_ack_pack(NULL, buf), -EINVAL, "ACK pack NULL");
    ok_int(tbt_fec_ack_unpack(NULL, &a2), -EINVAL, "ACK unpack NULL");

    /* NACK: two 32-byte bitmaps; block_id lives in the record header. */
    memset(&nk, 0, sizeof(nk));
    tbt_fec_bitmap_set(nk.group_mask, 0);
    tbt_fec_bitmap_set(nk.group_mask, 255);
    tbt_fec_bitmap_set(nk.share_mask, 1);
    tbt_fec_bitmap_set(nk.share_mask, 254);
    memset(buf, 0, sizeof(buf));
    ok_int(tbt_fec_nack_pack(&nk, buf), TBT_FEC_NACK_LEN, "NACK pack length");
    ok(TBT_FEC_NACK_LEN == 64, "NACK body is 64 bytes", NULL);
    ok(tbt_fec_bitmap_test(buf, 0), "NACK group 0 at +0", NULL);
    ok(tbt_fec_bitmap_test(buf, 255), "NACK group 255 at +31", NULL);
    ok(tbt_fec_bitmap_test(buf + TBT_FEC_BITMAP_BYTES, 1),
       "NACK share 1 at +32", NULL);
    ok(tbt_fec_bitmap_test(buf + TBT_FEC_BITMAP_BYTES, 254),
       "NACK share 254 at +63", NULL);
    memset(&nk2, 0, sizeof(nk2));
    ok_int(tbt_fec_nack_unpack(buf, &nk2), 0, "NACK unpack");
    ok(!memcmp(&nk, &nk2, sizeof(nk)), "NACK round-trip", NULL);
    ok_int(tbt_fec_nack_pack(NULL, buf), -EINVAL, "NACK pack NULL");
    ok_int(tbt_fec_nack_unpack(NULL, &nk2), -EPROTO, "NACK unpack NULL");
    /* Regression: the sender must treat this positive length as success.
     * The 0x1004 NACK never reached the wire because `if (pack(...))` read
     * the 64-byte body length as an error. */
    n = tbt_fec_nack_pack(&nk, buf);
    ok(n == TBT_FEC_NACK_LEN && n > 0,
       "NACK pack success is a positive length, not an error", NULL);
    /* The 1 GiB run's regression: `if (pack(...))` read this positive length
     * as a failure, so every NACK was dropped before the out queue. */
    ok_int(n ? -EINVAL : 0, -EINVAL,
           "old boolean check would drop the NACK");
    ok_int(n != TBT_FEC_NACK_LEN ? -EINVAL : 0, 0,
           "fixed length check accepts the NACK");

    /* FEC_CFG: 32 bytes, internal CRC over +0..+23. */
    memset(&c, 0, sizeof(c));
    c.epoch = 0x12345678u;
    c.n = 224;
    c.m = 32;
    c.w = 384;
    c.k = 192;
    c.d = 64;
    c.seed = 0x1122334455667788ULL;
    ok_int(tbt_fec_cfg_pack(&c, cbuf), 0, "CFG pack");
    ok_u32le(cbuf + 0, c.epoch, "CFG epoch at +0");
    ok_u16le(cbuf + 4, c.n, "CFG n at +4");
    ok_u16le(cbuf + 6, c.m, "CFG m at +6");
    ok_u16le(cbuf + 8, c.w, "CFG w at +8");
    ok_u16le(cbuf + 10, c.k, "CFG k at +10");
    ok(cbuf[12] == 64, "CFG d at +12", NULL);
    ok_u64le(cbuf + 16, c.seed, "CFG seed at +16");
    ok_u32le(cbuf + 24, tbt_fec_crc32c(0, cbuf, 24), "CFG CRC at +24");
    ok_u32le(cbuf + 28, 0, "CFG reserved2 at +28");
    memset(&c2, 0, sizeof(c2));
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), 0, "CFG unpack");
    ok(c2.epoch == c.epoch && c2.n == c.n && c2.m == c.m && c2.w == c.w && c2.k == c.k &&
           c2.d == c.d && c2.seed == c.seed,
       "CFG round-trip", NULL);

    cbuf[0] ^= 1;
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG body CRC mismatch");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[28] = 1;
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG reserved2 != 0");

    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[4] = 0;
    cbuf[5] = 0;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG n=0");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[6] = 0;
    cbuf[7] = 0;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG m=0");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[4] = 255;
    cbuf[5] = 0;
    cbuf[6] = 2;
    cbuf[7] = 0;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG n+m=257");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[10] = 0;
    cbuf[11] = 0;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG k=0");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[8] = 100;
    cbuf[9] = 0;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG w<k");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[8] = 0x01;
    cbuf[9] = 0x04; /* 1025 */
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG w over TBT_FEC_MAX_W");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[8] = 200;
    cbuf[9] = 0;
    cbuf[12] = 255;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG d>w");
    tbt_fec_cfg_pack(&c, cbuf);
    cbuf[8] = 8;
    cbuf[9] = 0;
    cbuf[10] = 8;
    cbuf[11] = 0;
    cbuf[12] = 8;
    cfg_fix_crc(cbuf);
    ok_int(tbt_fec_cfg_unpack(cbuf, &c2), -EPROTO, "CFG d==w with w>1");

    /* FEC_FBK: 48 bytes including the 32-byte bitmap. */
    ok((int)sizeof(struct tbt_fec_fbk) == TBT_FEC_FBK_LEN, "FBK struct is 48 bytes", NULL);
    ok(TBT_FEC_FBK_LEN == 48, "FBK length constant", NULL);
    memset(&f, 0, sizeof(f));
    f.epoch = 0xbeef;
    f.m_current = 4;
    f.m_target = 6;
    f.p_ewma_ppm = 500000;
    f.expected_shares = 1000;
    f.received_shares = 900;
    tbt_fec_bitmap_set(f.carrier_bitmap, 0);
    tbt_fec_bitmap_set(f.carrier_bitmap, 255);
    ok_int(tbt_fec_fbk_pack(&f, fbuf), 0, "FBK pack");
    ok_u16le(fbuf + 0, f.epoch, "FBK epoch at +0");
    ok(fbuf[2] == 4 && fbuf[3] == 6, "FBK m_current/m_target at +2/+3", NULL);
    ok_u32le(fbuf + 4, f.p_ewma_ppm, "FBK p_ewma at +4");
    ok_u16le(fbuf + 8, f.expected_shares, "FBK expected at +8");
    ok_u16le(fbuf + 10, f.received_shares, "FBK received at +10");
    ok_u32le(fbuf + 12, 0, "FBK reserved at +12");
    ok((fbuf[16] & 1) && (fbuf[16 + 31] & 0x80), "FBK bitmap bits 0 and 255", NULL);
    memset(&f2, 0, sizeof(f2));
    ok_int(tbt_fec_fbk_unpack(fbuf, &f2), 0, "FBK unpack");
    ok(f2.epoch == f.epoch && f2.m_current == f.m_current && f2.m_target == f.m_target &&
           f2.p_ewma_ppm == f.p_ewma_ppm && f2.expected_shares == f.expected_shares &&
           f2.received_shares == f.received_shares &&
           !memcmp(f2.carrier_bitmap, f.carrier_bitmap, TBT_FEC_BITMAP_BYTES),
       "FBK round-trip", NULL);
    fbuf[12] = 1;
    ok_int(tbt_fec_fbk_unpack(fbuf, &f2), -EPROTO, "FBK reserved != 0");
}

/* ------------------------------------------------------------------ *
 * 6. RLNC single group.
 * ------------------------------------------------------------------ */

struct rlnc_fixture
{
    struct tbt_fec_rlnc *ctx;
    u32 L;
    size_t s;
    u8 *data[TBT_FEC_MAX_SHARES];
    u8 *parity[TBT_FEC_MAX_SHARES];
    u8 *orig[TBT_FEC_MAX_SHARES];
};

static void fixture_init(struct rlnc_fixture *f, u16 n, u16 m, u16 k, u16 w, u16 d,
                         size_t s, u32 L, u64 nonce)
{
    u32 i;
    u32 pcount;

    memset(f, 0, sizeof(*f));
    f->L = L;
    f->s = s;
    f->ctx = tbt_fec_rlnc_new(n, m, k, w, s, d, nonce);
    if (!f->ctx)
        return;
    for (i = 0; i < L; i++)
    {
        f->data[i] = xalloc(s);
        f->orig[i] = xalloc(s);
        fill_pattern(f->orig[i], s, 0x1000u + i);
        memcpy(f->data[i], f->orig[i], s);
    }
    pcount = tbt_fec_rlnc_parity_count(f->ctx, L);
    for (i = 0; i < pcount; i++)
        f->parity[i] = xalloc(s);
}

static void fixture_free(struct rlnc_fixture *f)
{
    u32 i;

    tbt_fec_rlnc_free(f->ctx);
    f->ctx = NULL;
    for (i = 0; i < TBT_FEC_MAX_SHARES; i++)
    {
        free(f->data[i]);
        free(f->parity[i]);
        free(f->orig[i]);
    }
}

static void fixture_reset(struct rlnc_fixture *f)
{
    u32 i;

    for (i = 0; i < f->L; i++)
        memcpy(f->data[i], f->orig[i], f->s);
}

static int fixture_exact(const struct rlnc_fixture *f)
{
    u32 i;

    for (i = 0; i < f->L; i++)
        if (memcmp(f->data[i], f->orig[i], f->s))
            return 0;
    return 1;
}

/* Return a data index inside window g that at least one parity row in
 * [r0, r1) covers. The codec's band now covers every own-data column, but
 * this helper still keeps the tests honest if that invariant regresses. */
static int pick_covered(struct tbt_fec_rlnc *ctx, u32 g, u32 r0, u32 r1, u32 k, u32 w,
                        u32 L, u32 *idxbuf)
{
    int ws = (int)(g * k) + (int)k - (int)w;
    int lo = ws < 0 ? 0 : ws;
    int hi = ws + (int)w;
    int abs;
    u32 r;
    u32 i;

    if (hi > (int)L)
        hi = (int)L;
    for (abs = lo; abs < hi; abs++)
    {
        for (r = r0; r < r1; r++)
        {
            u32 nidx = tbt_fec_rlnc_coeffs(ctx, g, r, idxbuf, TBT_FEC_MAX_D);

            for (i = 0; i < nidx; i++)
                if (ws + (int)idxbuf[i] == abs)
                    return abs;
        }
    }
    return -1;
}

static int pick_covered_unerased(struct tbt_fec_rlnc *ctx, u32 g, u32 r0, u32 r1,
                                 u32 k, u32 w, u32 L, u32 *idxbuf, const u8 *erased)
{
    int ws = (int)(g * k) + (int)k - (int)w;
    int lo = ws < 0 ? 0 : ws;
    int hi = ws + (int)w;
    int abs;
    u32 r;
    u32 i;

    if (hi > (int)L)
        hi = (int)L;
    for (abs = lo; abs < hi; abs++)
    {
        if (erased[abs])
            continue;
        for (r = r0; r < r1; r++)
        {
            u32 nidx = tbt_fec_rlnc_coeffs(ctx, g, r, idxbuf, TBT_FEC_MAX_D);

            for (i = 0; i < nidx; i++)
                if (ws + (int)idxbuf[i] == abs)
                    return abs;
        }
    }
    return -1;
}

static void rlnc_single_case(u16 n, u16 m, u16 k, u16 w, u16 d, size_t s, u64 nonce,
                             const char *label)
{
    struct rlnc_fixture f;
    u8 dp[TBT_FEC_MAX_SHARES];
    u8 pp[TBT_FEC_MAX_SHARES];
    u32 idxbuf[TBT_FEC_MAX_D];
    int target;
    int rc;

    fixture_init(&f, n, m, k, w, d, s, k, nonce);
    ok(f.ctx != NULL, label, "rlnc_new");
    if (!f.ctx)
        return;
    ok_int(tbt_fec_rlnc_encode(f.ctx, f.data, k, f.parity), (int)m, label);

    memset(dp, 1, k);
    memset(pp, 1, m);
    target = pick_covered(f.ctx, 0, 0, m, k, w, k, idxbuf);
    ok(target >= 0, label, "no parity row covers any data packet");
    if (target < 0)
    {
        fixture_free(&f);
        return;
    }
    dp[target] = 0;
    memset(f.data[target], 0xAA, s);
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, k);
    ok_int(rc, 1, label);
    ok_u32(tbt_fec_rlnc_recovered(f.ctx), 1, label);
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), k, label);
    ok(fixture_exact(&f), label, "recovered bytes differ");

    /* All parity erased, data complete: nothing to recover, nothing written. */
    fixture_reset(&f);
    memset(dp, 1, k);
    memset(pp, 0, m);
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, k);
    ok_int(rc, 0, label);
    ok_u32(tbt_fec_rlnc_recovered(f.ctx), 0, label);
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), k, label);
    ok(fixture_exact(&f), label, "data changed with no parity");

    fixture_free(&f);
}

/* The production failure: one DATA share is lost, every parity row arrives.
 * Every data index must be recoverable, not just the ones a random matrix
 * happened to cover. */
static void rlnc_every_single_loss(u16 n, u16 m, u16 k, u16 w, u16 d, size_t s,
                                   const char *label)
{
    struct rlnc_fixture f;
    u8 dp[TBT_FEC_MAX_SHARES];
    u8 pp[TBT_FEC_MAX_SHARES];
    u32 target;
    int rc;

    fixture_init(&f, n, m, k, w, d, s, k, 0);
    ok(f.ctx != NULL, label, "rlnc_new");
    if (!f.ctx)
        return;
    ok_int(tbt_fec_rlnc_encode(f.ctx, f.data, k, f.parity), (int)m, label);
    for (target = 0; target < k; target++)
    {
        char detail[64];

        fixture_reset(&f);
        memset(dp, 1, k);
        memset(pp, 1, m);
        dp[target] = 0;
        memset(f.data[target], 0xAA, s);
        rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, k);
        snprintf(detail, sizeof(detail), "lost share %u not recovered", target);
        ok(rc == 1, label, detail);
        ok(fixture_exact(&f), label, "single-loss bytes differ");
        if (rc != 1 || !fixture_exact(&f))
            break;
    }
    fixture_free(&f);
}

/* The n4m4 band is the complement of the identity, so any subset of up to
 * four data erasures is recoverable when every parity row arrives. This is
 * the 1/4 DATA + 4/4 PARITY case the QEMU gate can produce when a carrier
 * dies during startup. */
static void rlnc_erasure_case(u16 n, u16 m, u16 k, u16 w, u16 d, size_t s,
                              u32 nerase, const char *label)
{
    struct rlnc_fixture f;
    u8 dp[TBT_FEC_MAX_SHARES];
    u8 pp[TBT_FEC_MAX_SHARES];
    char detail[64];
    u32 i;
    int rc;

    fixture_init(&f, n, m, k, w, d, s, k, 0);
    ok(f.ctx != NULL, label, "rlnc_new");
    if (!f.ctx)
        return;
    ok_int(tbt_fec_rlnc_encode(f.ctx, f.data, k, f.parity), (int)m, label);
    memset(dp, 1, k);
    memset(pp, 1, m);
    for (i = 0; i < nerase && i < k; i++)
    {
        dp[i] = 0;
        memset(f.data[i], 0xAA, s);
    }
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, k);
    snprintf(detail, sizeof(detail), "%u erasures not recovered", nerase);
    ok(rc == (int)nerase, label, detail);
    ok(fixture_exact(&f), label, "erasure bytes differ");
    fixture_free(&f);
}

/* Every own-data column appears in at least one parity row, and the rows span
 * the whole data space. k is small enough for a u32 row mask. */
static void rlnc_band_case(u16 n, u16 m, u16 k, u16 w, u16 d, size_t s,
                           const char *label)
{
    struct rlnc_fixture f;
    u32 rows[TBT_FEC_MAX_SHARES];
    u32 idxbuf[TBT_FEC_MAX_D];
    int ws = (int)k - (int)w;
    u32 rank = 0;
    u32 r;
    u32 c;

    fixture_init(&f, n, m, k, w, d, s, k, 0);
    ok(f.ctx != NULL, label, "rlnc_new");
    if (!f.ctx)
        return;
    memset(rows, 0, sizeof(rows));
    for (r = 0; r < m; r++)
    {
        u32 nidx = tbt_fec_rlnc_coeffs(f.ctx, 0, r, idxbuf, TBT_FEC_MAX_D);
        u32 i;

        ok(nidx > 0, label, "empty parity row");
        for (i = 0; i < nidx; i++)
        {
            int abs = ws + (int)idxbuf[i];

            if (abs >= 0 && abs < (int)k)
                rows[r] |= 1u << abs;
        }
    }
    for (c = 0; c < k; c++)
    {
        int hit = 0;

        for (r = 0; r < m; r++)
            if ((rows[r] >> c) & 1u)
                hit = 1;
        ok(hit, label, "a data column is in no parity row");
    }
    for (c = 0; c < k; c++)
    {
        u32 bit = 1u << c;
        u32 p;

        for (p = rank; p < m; p++)
            if (rows[p] & bit)
                break;
        if (p == m)
            continue;
        {
            u32 t = rows[rank];

            rows[rank] = rows[p];
            rows[p] = t;
        }
        for (p = 0; p < m; p++)
            if (p != rank && (rows[p] & bit))
                rows[p] ^= rows[rank];
        rank++;
    }
    ok_u32(rank, m < k ? m : k, label);
    fixture_free(&f);
}

static void test_rlnc_single(void)
{
    struct tbt_fec_rlnc *ctx;

    rlnc_single_case(4, 4, 4, 8, 4, 64, 0x1234, "rlnc 4x4");
    rlnc_single_case(5, 3, 5, 10, 5, 64, 0x1234, "rlnc 5x3");
    rlnc_single_case(3, 5, 3, 6, 3, 64, 0x1234, "rlnc 3x5");
    rlnc_single_case(2, 6, 2, 4, 2, 64, 0x1234, "rlnc 2x6");
    rlnc_band_case(4, 4, 4, 8, 4, 64, "rlnc 4x4 band");
    rlnc_band_case(5, 3, 5, 10, 5, 64, "rlnc 5x3 band");
    rlnc_band_case(3, 5, 3, 6, 3, 64, "rlnc 3x5 band");
    rlnc_band_case(2, 6, 2, 4, 2, 64, "rlnc 2x6 band");
    /* The grouped n4m4 shape: 3 DATA + 4 PARITY must decode for every one of
     * the four possible lost shares. */
    rlnc_every_single_loss(4, 4, 4, 8, 4, 64, "rlnc 4x4 single loss");
    rlnc_erasure_case(4, 4, 4, 8, 4, 64, 1, "rlnc 4x4 erasures");
    rlnc_erasure_case(4, 4, 4, 8, 4, 64, 2, "rlnc 4x4 erasures");
    rlnc_erasure_case(4, 4, 4, 8, 4, 64, 3, "rlnc 4x4 erasures");
    rlnc_erasure_case(4, 4, 4, 8, 4, 64, 4, "rlnc 4x4 erasures");
    rlnc_every_single_loss(192, 64, 192, 384, 64, 16, "rlnc 192x64 single loss");
    rlnc_every_single_loss(192, 64, 192, 192, 96, 16,
                           "rlnc 192x64 w=n single loss");

    /* 192 data, 64 parity, S=16: erase 64 data and supply 64 parity rows.
     * The band's first 64 columns are the full 64x64 circulant, so any nonce
     * gives the same full-rank matrix. */
    {
        struct rlnc_fixture f;
        u8 dp[192];
        u8 pp[64];
        u32 i;
        int rc;

        fixture_init(&f, 192, 64, 192, 384, 64, 16, 192, 39);
        ok(f.ctx != NULL, "rlnc 192x64", "rlnc_new");
        if (f.ctx)
        {
            ok_int(tbt_fec_rlnc_encode(f.ctx, f.data, 192, f.parity), 64, "rlnc 192x64");
            memset(dp, 1, sizeof(dp));
            memset(pp, 1, sizeof(pp));
            for (i = 0; i < 64; i++)
            {
                dp[i] = 0;
                memset(f.data[i], 0xAA, 16);
            }
            rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 192);
            ok_int(rc, 64, "rlnc 192x64 erase 64 data");
            ok_u32(tbt_fec_rlnc_recovered(f.ctx), 64, "rlnc 192x64");
            ok_u32(tbt_fec_rlnc_frontier(f.ctx), 192, "rlnc 192x64");
            ok(fixture_exact(&f), "rlnc 192x64", "recovered bytes differ");
        }
        fixture_free(&f);
    }

    /* d == w is refused for w > 1; d == w == 1 is the only legal form. */
    ok(tbt_fec_rlnc_new(4, 4, 4, 8, 64, 8, 0) == NULL, "d==w refused for w>1", NULL);
    ctx = tbt_fec_rlnc_new(4, 4, 1, 1, 64, 1, 0);
    ok(ctx != NULL, "d==w==1 accepted", NULL);
    tbt_fec_rlnc_free(ctx);
    ok(tbt_fec_rlnc_new(0, 4, 4, 8, 64, 4, 0) == NULL, "n=0 refused", NULL);
    ok(tbt_fec_rlnc_new(4, 0, 4, 8, 64, 4, 0) == NULL, "m=0 refused", NULL);
    ok(tbt_fec_rlnc_new(255, 2, 4, 8, 64, 4, 0) == NULL, "n+m=257 refused", NULL);
    ok(tbt_fec_rlnc_new(4, 4, 4, 8, 0, 4, 0) == NULL, "s=0 refused", NULL);
    ok(tbt_fec_rlnc_new(4, 4, 4, 8, 64, 0, 0) == NULL, "d=0 refused", NULL);
    ctx = tbt_fec_rlnc_new(4, 4, 4, 8, 64, 4, 0);
    ok(ctx != NULL, "valid ctx accepted", NULL);
    tbt_fec_rlnc_free(ctx);
}

/* ------------------------------------------------------------------ *
 * 7. Loss, reorder, late parity, duplicates, too many erasures.
 * ------------------------------------------------------------------ */

static void test_rlnc_loss_reorder(void)
{
    struct rlnc_fixture f;
    u8 dp[16];
    u8 pp[16];
    u32 idxbuf[TBT_FEC_MAX_D];
    u8 erased[16];
    int targets[4];
    int rc;
    int g;
    u32 i;
    u32 front;
    u32 rec;

    fixture_init(&f, 4, 4, 4, 8, 4, 64, 16, 0x1234);
    ok(f.ctx != NULL, "rlnc 4x4 L=16", "rlnc_new");
    if (!f.ctx)
        return;
    ok_int(tbt_fec_rlnc_encode(f.ctx, f.data, 16, f.parity), 16, "rlnc 4x4 L=16");

    /* (a) one covered data erased per group, all parity present. */
    memset(dp, 1, sizeof(dp));
    memset(pp, 1, sizeof(pp));
    memset(erased, 0, sizeof(erased));
    for (g = 0; g < 4; g++)
    {
        int t = pick_covered_unerased(f.ctx, (u32)g, 0, 4, 4, 8, 16, idxbuf, erased);

        ok(t >= 0, "4x4 L=16 one erasure per group", "no covered data in this window");
        if (t < 0)
        {
            fixture_free(&f);
            return;
        }
        targets[g] = t;
        erased[t] = 1;
        dp[t] = 0;
        memset(f.data[t], 0xAA, 64);
    }
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 16);
    ok_int(rc, 4, "4x4 L=16 one erasure per group");
    ok_u32(tbt_fec_rlnc_recovered(f.ctx), 4, "4x4 L=16 one erasure per group");
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), 16, "4x4 L=16 one erasure per group");
    ok(fixture_exact(&f), "4x4 L=16 one erasure per group", "recovered bytes differ");

    /* (b) all parity erased, data complete: exact, zero recovered. */
    fixture_reset(&f);
    memset(dp, 1, sizeof(dp));
    memset(pp, 0, sizeof(pp));
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 16);
    ok_int(rc, 0, "4x4 L=16 no parity");
    ok_u32(tbt_fec_rlnc_recovered(f.ctx), 0, "4x4 L=16 no parity");
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), 16, "4x4 L=16 no parity");
    ok(fixture_exact(&f), "4x4 L=16 no parity", "data changed");

    /* (c) data arrives first, parity later: the first decode recovers
     * nothing, the second fills the one hole. */
    memset(pp, 0, sizeof(pp));
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 16);
    ok_int(rc, 0, "4x4 L=16 data before parity");
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), 16, "4x4 L=16 data before parity");

    targets[0] = pick_covered(f.ctx, 0, 0, 4, 4, 8, 16, idxbuf);
    ok(targets[0] >= 0, "4x4 L=16 late parity", "no covered data in group 0");
    if (targets[0] >= 0)
    {
        dp[targets[0]] = 0;
        memset(f.data[targets[0]], 0xAA, 64);
        memset(pp, 1, sizeof(pp));
        rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 16);
        ok_int(rc, 1, "4x4 L=16 late parity");
        ok_u32(tbt_fec_rlnc_recovered(f.ctx), 1, "4x4 L=16 late parity");
        ok(fixture_exact(&f), "4x4 L=16 late parity", "recovered bytes differ");

        /* (d) duplicate indices do not move the frontier or rewrite data. */
        front = tbt_fec_rlnc_frontier(f.ctx);
        rec = tbt_fec_rlnc_recovered(f.ctx);
        ok_int(tbt_fec_rlnc_advance(f.ctx, TBT_FEC_RLNC_DATA_KNOWN, (u32)targets[0]),
               (int)front, "4x4 duplicate data index");
        ok_int(tbt_fec_rlnc_advance(f.ctx, TBT_FEC_RLNC_DATA_KNOWN, (u32)targets[0]),
               (int)front, "4x4 duplicate data index twice");
        ok_int(tbt_fec_rlnc_advance(f.ctx, TBT_FEC_RLNC_PARITY_KNOWN, 0), (int)front,
               "4x4 duplicate parity index");
        ok_int(tbt_fec_rlnc_advance(f.ctx, TBT_FEC_RLNC_PARITY_KNOWN, 0), (int)front,
               "4x4 duplicate parity index twice");
        ok_u32(tbt_fec_rlnc_recovered(f.ctx), rec, "4x4 duplicate recovered count");
        ok_u32(tbt_fec_rlnc_frontier(f.ctx), front, "4x4 duplicate frontier");
        ok(fixture_exact(&f), "4x4 duplicate indices", "data changed");
    }

    /* (e) too many erasures: all parity plus two data in one group. The
     * decode is STUCK and writes no byte. */
    fixture_reset(&f);
    memset(dp, 1, sizeof(dp));
    memset(pp, 0, sizeof(pp));
    dp[0] = 0;
    dp[1] = 0;
    memset(f.data[0], 0xAA, 64);
    memset(f.data[1], 0xAA, 64);
    rc = tbt_fec_rlnc_decode(f.ctx, f.data, f.parity, dp, pp, 16);
    ok(rc == TBT_FEC_RLNC_ERR_STUCK, "4x4 too many erasures is STUCK", "decode returned data");
    ok_int(rc, -EAGAIN, "4x4 STUCK is -EAGAIN");
    ok_u32(tbt_fec_rlnc_recovered(f.ctx), 0, "4x4 STUCK recovers nothing");
    ok_u32(tbt_fec_rlnc_frontier(f.ctx), 0, "4x4 STUCK frontier stays at the hole");
    ok_u32(tbt_fec_rlnc_stuck_at(f.ctx), 0, "4x4 STUCK stuck_at");
    ok(f.data[0][0] == 0xAA && f.data[1][0] == 0xAA, "4x4 STUCK writes no erased packet",
       NULL);
    for (i = 2; i < 16; i++)
        if (memcmp(f.data[i], f.orig[i], 64))
        {
            ok(0, "4x4 STUCK leaves known data untouched", "a known packet changed");
            break;
        }
    ok(i == 16, "4x4 STUCK leaves known data untouched", NULL);

    fixture_free(&f);
}

/* ------------------------------------------------------------------ *
 * 8. Grouped mode: 32 groups x 8 shares.
 * ------------------------------------------------------------------ */

static void test_grouped(void)
{
    struct tbt_fec_grouped *grp;
    u16 gi;
    u16 s;

    grp = tbt_fec_grouped_new(32, 4, 4, 4, 8, 64, 4, 0x0123456789abcdefULL);
    ok(grp != NULL, "grouped_new 32x8", NULL);
    if (!grp)
        return;
    ok_u32(tbt_fec_grouped_count(grp), 32, "grouped count");
    ok(tbt_fec_grouped_ctx(grp, 0) != NULL, "grouped ctx 0", NULL);
    ok(tbt_fec_grouped_ctx(grp, 31) != NULL, "grouped ctx 31", NULL);
    ok(tbt_fec_grouped_ctx(grp, 32) == NULL, "grouped ctx 32 is NULL", NULL);
    ok(tbt_fec_grouped_ctx(grp, 255) == NULL, "grouped ctx 255 is NULL", NULL);

    for (gi = 0; gi < 32; gi++)
    {
        struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(grp, gi);
        u8 *data[8];
        u8 *parity[8];
        u8 *orig[8];
        u8 dp[8];
        u8 pp[8];
        u32 idxbuf[TBT_FEC_MAX_D];
        int target;
        int rc;
        int i;

        for (i = 0; i < 8; i++)
        {
            data[i] = xalloc(64);
            orig[i] = xalloc(64);
            parity[i] = xalloc(64);
            fill_pattern(orig[i], 64, 0x2000u + (u32)gi * 16u + (u32)i);
            memcpy(data[i], orig[i], 64);
        }
        ok_int(tbt_fec_rlnc_encode(ctx, data, 8, parity), 8, "grouped encode L=8");

        /* Keep the four parity rows of window 0, drop all of window 1. */
        memset(dp, 1, sizeof(dp));
        memset(pp, 1, sizeof(pp));
        pp[4] = pp[5] = pp[6] = pp[7] = 0;
        target = pick_covered(ctx, 0, 0, 4, 4, 8, 8, idxbuf);
        ok(target >= 0, "grouped erase one data", "no covered data in window 0");
        if (target >= 0)
        {
            dp[target] = 0;
            memset(data[target], 0xAA, 64);
            rc = tbt_fec_rlnc_decode(ctx, data, parity, dp, pp, 8);
            ok_int(rc, 1, "grouped decode one erasure");
            ok_u32(tbt_fec_rlnc_recovered(ctx), 1, "grouped recovered");
            ok_u32(tbt_fec_rlnc_frontier(ctx), 8, "grouped frontier");
            for (i = 0; i < 8; i++)
                if (memcmp(data[i], orig[i], 64))
                {
                    ok(0, "grouped recovered bytes differ", "a packet changed");
                    break;
                }
            ok(i == 8, "grouped recovered bytes differ", NULL);
        }
        for (i = 0; i < 8; i++)
        {
            free(data[i]);
            free(orig[i]);
            free(parity[i]);
        }
    }
    tbt_fec_grouped_free(grp);

    /* seq % groups is the stripe; 0..255 covers every group eight times. */
    for (s = 0; s < 256; s++)
        ok_u32(tbt_fec_group_of_seq(s, 32), (u32)(s % 32), "group_of_seq 32");
    ok_u32(tbt_fec_group_of_seq(0, 0), 0, "group_of_seq zero groups");

    ok(tbt_fec_grouped_new(0, 4, 4, 4, 8, 64, 4, 0) == NULL, "grouped 0 groups refused",
       NULL);
    ok(tbt_fec_grouped_new(TBT_FEC_MAX_GROUPS + 1, 4, 4, 4, 8, 64, 4, 0) == NULL,
       "grouped over 256 groups refused", NULL);
}

/* ------------------------------------------------------------------ *
 * 9. Carrier/share/block mapping at 256 carriers.
 * ------------------------------------------------------------------ */

static void test_group_mapping(void)
{
    u16 c;
    u64 seq;
    u64 prev = 0;
    int mono = 1;

    for (c = 0; c < 256; c++)
    {
        ok_u32(tbt_fec_group_of_carrier(c, 8), (u32)(c / 8), "group_of_carrier n+m=8");
        ok_u32(tbt_fec_share_of_carrier(c, 8), (u32)(c % 8), "share_of_carrier n+m=8");
    }
    ok_u32(tbt_fec_group_of_carrier(0, 8), 0, "carrier 0 group");
    ok_u32(tbt_fec_share_of_carrier(0, 8), 0, "carrier 0 share");
    ok_u32(tbt_fec_group_of_carrier(255, 8), 31, "carrier 255 group");
    ok_u32(tbt_fec_share_of_carrier(255, 8), 7, "carrier 255 share");

    ok_u32(tbt_fec_group_of_carrier(0, 256), 0, "single carrier 0 group");
    ok_u32(tbt_fec_share_of_carrier(0, 256), 0, "single carrier 0 share");
    ok_u32(tbt_fec_group_of_carrier(255, 256), 0, "single carrier 255 group");
    ok_u32(tbt_fec_share_of_carrier(255, 256), 255, "single carrier 255 share");
    ok_u32(tbt_fec_group_of_carrier(7, 0), 0, "zero shares group");
    ok_u32(tbt_fec_share_of_carrier(7, 0), 0, "zero shares share");

    ok_u32(tbt_fec_group_of_seq(0, 32), 0, "seq 0 group");
    ok_u32(tbt_fec_group_of_seq(31, 32), 31, "seq 31 group");
    ok_u32(tbt_fec_group_of_seq(32, 32), 0, "seq 32 group");
    ok_u32(tbt_fec_group_of_seq(255, 32), 31, "seq 255 group");
    ok_u32(tbt_fec_group_of_seq(1ULL << 32, 32), 0, "seq 2^32 group");
    ok_u32(tbt_fec_group_of_seq(1ULL << 32, 1), 0, "seq single group");

    ok_u64(tbt_fec_block_of_seq(0, 32, 4), 0, "block seq 0");
    ok_u64(tbt_fec_block_of_seq(127, 32, 4), 0, "block seq 127");
    ok_u64(tbt_fec_block_of_seq(128, 32, 4), 1, "block seq 128");
    ok_u64(tbt_fec_block_of_seq(255, 32, 4), 1, "block seq 255");
    ok_u64(tbt_fec_block_of_seq(256, 32, 4), 2, "block seq 256");
    ok_u64(tbt_fec_block_of_seq(1ULL << 32, 32, 4), (1ULL << 32) / 128, "block seq 2^32");
    ok_u64(tbt_fec_block_of_seq(1ULL << 32, 1, 192), (1ULL << 32) / 192, "block single");
    ok_u64(tbt_fec_block_of_seq(5, 0, 4), 0, "block zero groups");
    ok_u64(tbt_fec_block_of_seq(5, 32, 0), 0, "block zero k");

    for (seq = 0; seq < 4096; seq++)
    {
        u64 b = tbt_fec_block_of_seq(seq, 32, 4);

        if (b < prev)
            mono = 0;
        prev = b;
    }
    ok(mono, "block_id is monotonic", NULL);
}

/* ------------------------------------------------------------------ *
 * 9b. Grouped reassembly: 32 groups x (4 data + 4 parity), striped seq.
 *
 * This mirrors tbt_fec_data.c exactly: a block of every group covers one
 * contiguous span of groups*n global packets, and the receiver may only hand
 * that span to the stream once every group's block is decoded. The test
 * erases one DATA share per block (all parity present), recovers it, and
 * compares the reassembled stream byte for byte, including a short final
 * super-block whose groups have different real lengths.
 * ------------------------------------------------------------------ */

static void test_group_reassembly(void)
{
    enum { G = 32, N = 4, M = 4, S = 64 };
    const u64 base = 0x5a5a5a5a5a5a5a5aULL;
    const u64 golden = 0x9e3779b97f4a7c15ULL;
    const u64 total_pkts = 3 * G * N + 37; /* 421: short final super-block */
    const u64 final_block = tbt_fec_final_block_of_total(total_pkts, G, N);
    struct tbt_fec_grouped *codec;
    u8 *stream, *out, *present;
    u64 seq, block;
    u32 idxbuf[TBT_FEC_MAX_D];
    u16 sum = 0;
    int ok_all = 1;

    codec = tbt_fec_grouped_new(G, N, M, N, 2 * N, S, N, base);
    ok(codec != NULL, "reassembly codec 32x8", NULL);
    if (!codec)
        return;
    stream = xalloc(total_pkts * S);
    out = xalloc(total_pkts * S);
    present = xalloc(total_pkts);
    fill_pattern(stream, total_pkts * S, 0x600d);

    /* Tail arithmetic: the per-group lengths of the final super-block must
     * add up to the packets that follow the last full super-block. */
    for (seq = 0; seq < G; seq++)
    {
        u16 len = tbt_fec_group_tail_len(total_pkts, G, N, (u16)seq);

        sum = (u16)(sum + len);
        ok(len <= N, "tail length <= n", NULL);
    }
    ok_u32(sum, (u32)(total_pkts - final_block * G * N),
           "tail lengths sum to the final super-block");
    ok_u32(tbt_fec_group_tail_len(0, G, N, 0), 0, "tail length of an empty stream");
    ok_u32(tbt_fec_group_tail_len(G * N, G, N, 0), N, "exact multiple is a full block");

    /* seq -> (group, block, p) -> seq must round-trip for every packet. */
    for (seq = 0; seq < total_pkts; seq++)
    {
        u16 g = tbt_fec_group_of_seq(seq, G);
        u64 b = tbt_fec_block_of_seq(seq, G, N);
        u16 p = (u16)((seq / G) % N);

        if (tbt_fec_seq_of_block_pkt(b, g, p, G, N) != seq)
        {
            ok(0, "seq_of_block_pkt round-trip", "mapping is not an inverse");
            break;
        }
    }
    ok(seq == total_pkts, "seq_of_block_pkt round-trip", NULL);

    for (block = 0; block <= final_block; block++)
    {
        u16 g;

        for (g = 0; g < G; g++)
        {
            u16 len = block == final_block ?
                tbt_fec_group_tail_len(total_pkts, G, N, g) : N;
            struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(codec, g);
            u8 *data[N], *orig[N], *parity[M];
            u8 dp[N], pp[M];
            u16 drop = 0, p;
            int target, rc;

            if (!len)
                continue; /* this group has no packet in the final block */
            for (p = 0; p < N; p++)
            {
                data[p] = xalloc(S);
                orig[p] = xalloc(S);
                if (p < M)
                    parity[p] = xalloc(S);
                if (p < len)
                {
                    u64 s = tbt_fec_seq_of_block_pkt(block, g, p, G, N);
                    u16 plen = s + 1 == total_pkts ? 17 : S;

                    memcpy(orig[p], stream + s * S, plen);
                    memcpy(data[p], orig[p], plen);
                }
            }
            tbt_fec_rlnc_set_nonce(ctx, base ^ ((u64)g << 32) ^
                                            (block * golden));
            ok_int(tbt_fec_rlnc_encode(ctx, data, N, parity), M,
                   "grouped block encode");

            /* Erase one DATA share that at least one parity row covers. A
             * d < w code cannot recover an arbitrary erasure, so the test
             * asks the coefficient set first, exactly as the data plane's
             * feedback would. */
            memset(dp, 1, sizeof(dp));
            memset(pp, 1, sizeof(pp));
            target = pick_covered(ctx, 0, 0, M, N, 2 * N, len, idxbuf);
            if (target >= 0)
            {
                drop = (u16)target;
                dp[drop] = 0;
                memset(data[drop], 0xAA, S);
            }
            rc = tbt_fec_rlnc_decode(ctx, data, parity, dp, pp, N);
            if (target >= 0)
                ok_int(rc, 1, "grouped block decode one erasure");
            else
                ok_int(rc, 0, "grouped block with no covered erasure");
            for (p = 0; p < len; p++)
            {
                if (memcmp(data[p], orig[p], S))
                {
                    ok(0, "grouped block recovered bytes", "a packet changed");
                    ok_all = 0;
                    break;
                }
            }
            for (p = 0; p < len; p++)
            {
                u64 s = tbt_fec_seq_of_block_pkt(block, g, p, G, N);
                u16 plen = s + 1 == total_pkts ? 17 : S;

                memcpy(out + s * S, data[p], plen);
                present[s] = 1;
            }
            for (p = 0; p < N; p++)
            {
                free(data[p]);
                free(orig[p]);
                if (p < M)
                    free(parity[p]);
            }
        }
    }
    for (seq = 0; seq < total_pkts; seq++)
    {
        if (!present[seq])
        {
            ok(0, "grouped reassembly delivered every seq", "a seq is missing");
            break;
        }
        if (memcmp(out + seq * S, stream + seq * S,
                   seq + 1 == total_pkts ? 17 : S))
        {
            ok(0, "grouped reassembly is byte-exact", "bytes differ");
            break;
        }
    }
    ok(seq == total_pkts, "grouped reassembly is byte-exact", NULL);
    ok(ok_all, "grouped blocks recovered byte-exact", NULL);

    free(stream);
    free(out);
    free(present);
    tbt_fec_grouped_free(codec);
}

/* ------------------------------------------------------------------ *
 * 10. Dynamic parameters.
 * ------------------------------------------------------------------ */

static void check_grouped(u32 p, u16 m, u16 n, u16 w, u16 d)
{
    struct tbt_fec_params par;

    memset(&par, 0, sizeof(par));
    ok_int(tbt_fec_params_grouped(p, 32, 8, &par), 0, "params_grouped 32x8");
    ok(par.codec == TBT_FEC_CODEC_RLNC_GROUPED, "grouped codec", NULL);
    ok_u32(par.groups, 32, "grouped groups");
    ok_u32(par.group_size, 8, "grouped group_size");
    ok_u32(par.m, m, "grouped m");
    ok_u32(par.n, n, "grouped n");
    ok_u32(par.k, n, "grouped k");
    ok_u32(par.w, w, "grouped w");
    ok_u32(par.d, d, "grouped d");
    ok_int(tbt_fec_params_check(&par), 0, "grouped params_check");
}

static void test_params(void)
{
    static const u32 p_vals[] = { 0, 10000, 10001, 200000, 200001, 300000, 300001, 500000 };
    static const u16 p_m[] = { 3, 3, 4, 4, 5, 5, 6, 6 };
    struct tbt_fec_params par;
    int i;

    for (i = 0; i < 8; i++)
        ok_u32(tbt_fec_m_for_p(p_vals[i], 8), p_m[i], "m_for_p at the threshold");

    check_grouped(0, 3, 5, 10, 5);
    check_grouped(10001, 4, 4, 8, 4);
    check_grouped(200001, 5, 3, 6, 3);
    check_grouped(500000, 6, 2, 4, 2);

    /* Single 256-share profile: only p <= 20% and exactly 256 carriers. */
    memset(&par, 0, sizeof(par));
    ok_int(tbt_fec_params_single(0, 256, &par), 0, "single p=0");
    ok(par.codec == TBT_FEC_CODEC_RLNC_SINGLE, "single codec", NULL);
    ok_u32(par.groups, 1, "single groups");
    ok_u32(par.group_size, 256, "single group_size");
    ok_u32(par.n, 192, "single n");
    ok_u32(par.m, 64, "single m");
    ok_u32(par.k, 192, "single k");
    ok_u32(par.w, 384, "single w");
    ok_u32(par.d, 64, "single d");
    ok_int(tbt_fec_params_check(&par), 0, "single params_check");
    ok_int(tbt_fec_params_single(200000, 256, &par), 0, "single p=20%");
    ok_int(tbt_fec_params_single(200001, 256, &par), -ERANGE, "single p>20%");
    ok_int(tbt_fec_params_single(0, 255, &par), -ERANGE, "single carriers!=256");

    /* Policy: 256 carriers and p<=20% -> single; otherwise grouped. */
    memset(&par, 0, sizeof(par));
    ok_int(tbt_fec_params_dynamic(0, 256, &par), 0, "dynamic 256 p=0");
    ok(par.codec == TBT_FEC_CODEC_RLNC_SINGLE, "dynamic 256 p=0 is single", NULL);
    ok_int(tbt_fec_params_dynamic(200000, 256, &par), 0, "dynamic 256 p=20%");
    ok(par.codec == TBT_FEC_CODEC_RLNC_SINGLE, "dynamic 256 p=20% is single", NULL);
    ok_int(tbt_fec_params_dynamic(200001, 256, &par), 0, "dynamic 256 p>20%");
    ok(par.codec == TBT_FEC_CODEC_RLNC_GROUPED, "dynamic 256 p>20% is grouped", NULL);
    ok_u32(par.groups, 32, "dynamic 256 p>20% groups");
    ok_u32(par.m, 5, "dynamic 256 p>20% m");
    ok_u32(par.n, 3, "dynamic 256 p>20% n");
    ok_u32(par.w, 6, "dynamic 256 p>20% w");
    ok_u32(par.d, 3, "dynamic 256 p>20% d");
    ok_int(tbt_fec_params_check(&par), 0, "dynamic 256 p>20% check");
    ok_int(tbt_fec_params_dynamic(500000, 256, &par), 0, "dynamic 256 p=50%");
    ok_u32(par.m, 6, "dynamic 256 p=50% m");
    ok_u32(par.n, 2, "dynamic 256 p=50% n");
    ok_u32(par.w, 4, "dynamic 256 p=50% w");
    ok_u32(par.d, 2, "dynamic 256 p=50% d");

    ok_int(tbt_fec_params_dynamic(500000, 8, &par), 0, "dynamic 8 p=50%");
    ok(par.codec == TBT_FEC_CODEC_RLNC_GROUPED, "dynamic 8 p=50% grouped", NULL);
    ok_u32(par.groups, 1, "dynamic 8 p=50% groups");
    ok_u32(par.group_size, 8, "dynamic 8 p=50% group_size");
    ok_u32(par.m, 6, "dynamic 8 p=50% m");
    ok_u32(par.n, 2, "dynamic 8 p=50% n");
    ok_u32(par.w, 4, "dynamic 8 p=50% w");
    ok_u32(par.d, 2, "dynamic 8 p=50% d");
    ok_int(tbt_fec_params_check(&par), 0, "dynamic 8 p=50% check");

    ok_int(tbt_fec_params_dynamic(0, 8, &par), 0, "dynamic 8 p=0");
    ok_u32(par.groups, 1, "dynamic 8 p=0 groups");
    ok_u32(par.m, 3, "dynamic 8 p=0 m");
    ok_u32(par.n, 5, "dynamic 8 p=0 n");
    ok_u32(par.w, 10, "dynamic 8 p=0 w");
    ok_u32(par.d, 5, "dynamic 8 p=0 d");

    ok_int(tbt_fec_params_dynamic(500000, 4, &par), 0, "dynamic 4 p=50%");
    ok_u32(par.groups, 1, "dynamic 4 groups");
    ok_u32(par.group_size, 4, "dynamic 4 group_size");
    ok_u32(par.m, 3, "dynamic 4 m is clamped");
    ok_u32(par.n, 1, "dynamic 4 n");
    ok_u32(par.w, 2, "dynamic 4 w");
    ok_u32(par.d, 1, "dynamic 4 d");
    ok_int(tbt_fec_params_check(&par), 0, "dynamic 4 check");

    ok_int(tbt_fec_params_dynamic(0, 16, &par), 0, "dynamic 16");
    ok_u32(par.groups, 2, "dynamic 16 groups");
    ok_int(tbt_fec_params_dynamic(0, 1, &par), -EINVAL, "dynamic 1 carrier");
    ok_int(tbt_fec_params_dynamic(0, 257, &par), -EINVAL, "dynamic 257 carriers");

    /* SC-LDPC is reserved for D and never accepted here. */
    ok_int(tbt_fec_params_dynamic(0, 256, &par), 0, "dynamic for check");
    par.codec = TBT_FEC_CODEC_SC_LDPC;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params_check rejects SC-LDPC");
    par.codec = 9;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params_check rejects an unknown codec");
    ok_int(tbt_fec_params_check(NULL), -EINVAL, "params_check NULL");

    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.codec = TBT_FEC_CODEC_RLNC_SINGLE;
    par.groups = 2;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "single with groups != 1");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.n = 0;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params n=0");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.n = 7;
    par.m = 2;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params n+m != group_size");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.d = par.w;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params d==w");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.w = TBT_FEC_MAX_W + 1;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params w over max");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.k = 0;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params k=0");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.d = 0;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params d=0");
    ok_int(tbt_fec_params_grouped(0, 32, 8, &par), 0, "grouped for check");
    par.groups = 0;
    ok_int(tbt_fec_params_check(&par), -EINVAL, "params groups=0");
    ok_int(tbt_fec_params_grouped(0, 0, 8, &par), -EINVAL, "params_grouped 0 groups");
    ok_int(tbt_fec_params_grouped(0, 33, 8, &par), -EINVAL, "params_grouped 33x8");
}

/* ------------------------------------------------------------------ *
 * 11. Codec ops table.
 * ------------------------------------------------------------------ */

static void test_ops(void)
{
    const struct tbt_fec_ops *ops = &tbt_fec_ops_rlnc;
    u8 *data[4];
    u8 *parity[4];
    u8 *orig[4];
    u8 dp[4];
    u8 pp[4];
    u32 idx[TBT_FEC_MAX_D];
    void *ctx;
    int i;
    int rc;

    ok(ops->name != NULL && !strcmp(ops->name, "rlnc"), "ops name", NULL);
    ok_u32(ops->mode, 1, "ops mode");
    ok(ops->new != NULL && ops->free != NULL && ops->encode != NULL && ops->decode != NULL,
       "ops callbacks present", NULL);

    ctx = ops->new(4, 4, 4, 8, 64, 4, 0x1234);
    ok(ctx != NULL, "ops new", NULL);
    if (!ctx)
        return;
    ok_u32(ops->parity_count(ctx, 4), 4, "ops parity_count");
    ok_u32(ops->coeffs(ctx, 0, 0, idx, TBT_FEC_MAX_D), 4, "ops coeffs");

    for (i = 0; i < 4; i++)
    {
        data[i] = xalloc(64);
        orig[i] = xalloc(64);
        parity[i] = xalloc(64);
        fill_pattern(orig[i], 64, (u32)i + 1);
        memcpy(data[i], orig[i], 64);
    }
    ok_int(ops->encode(ctx, data, 4, parity), 4, "ops encode");
    memset(dp, 1, sizeof(dp));
    memset(pp, 1, sizeof(pp));
    dp[0] = 0;
    memset(data[0], 0xAA, 64);
    rc = ops->decode(ctx, data, parity, dp, pp, 4);
    ok_int(rc, 1, "ops decode");
    ok_u32(ops->recovered(ctx), 1, "ops recovered");
    ok_u32(ops->frontier(ctx), 4, "ops frontier");
    ok(ops->is_known(ctx, 0) && !ops->is_known(ctx, 4), "ops is_known", NULL);
    ok_int(ops->advance(ctx, TBT_FEC_RLNC_DATA_KNOWN, 0), 4, "ops advance");
    ok_u32(ops->stuck_at(ctx), 4, "ops stuck_at");
    ok(!memcmp(data[0], orig[0], 64), "ops recovered bytes", NULL);

    ops->free(ctx);
    ok(ops->new(4, 4, 4, 8, 64, 8, 0) == NULL, "ops new refuses d==w", NULL);
    for (i = 0; i < 4; i++)
    {
        free(data[i]);
        free(orig[i]);
        free(parity[i]);
    }
}

/* ------------------------------------------------------------------ *
 * 12. Record framing: control frames are read only at record boundaries.
 * ------------------------------------------------------------------ */

enum
{
    REC_NEED_MORE = 0,
    REC_EMITTED = 1,
    REC_BAD = -1,
};

struct fec_record
{
    u8 type;
    u16 payload_len;
    u8 hdr[TBT_FEC_HDR_LEN];
    u8 payload[TBT_FEC_PAYLOAD_MAX];
};

struct fec_reader
{
    u8 buf[TBT_FEC_REC_MAX];
    size_t have;
};

/* Pull one complete record. The header's payload_len is the only boundary:
 * a control frame's first byte inside a partial DATA record is payload. */
static int reader_pull(struct fec_reader *r, struct fec_record *out)
{
    struct tbt_fec_hdr h;
    size_t need;

    if (r->have < TBT_FEC_HDR_LEN)
        return REC_NEED_MORE;
    if (tbt_fec_hdr_unpack(r->buf, &h))
        return REC_BAD;
    if (h.conv != TBT_FEC_CONV || h.payload_len > TBT_FEC_PAYLOAD_MAX)
        return REC_BAD;
    need = TBT_FEC_HDR_LEN + h.payload_len;
    if (r->have < need)
        return REC_NEED_MORE;
    out->type = h.type;
    out->payload_len = h.payload_len;
    memcpy(out->hdr, r->buf, TBT_FEC_HDR_LEN);
    memcpy(out->payload, r->buf + TBT_FEC_HDR_LEN, h.payload_len);
    memmove(r->buf, r->buf + need, r->have - need);
    r->have -= need;
    return REC_EMITTED;
}

static int reader_feed(struct fec_reader *r, const u8 *p, size_t len,
                       struct fec_record *ev, int max)
{
    int n = 0;

    if (r->have + len > sizeof(r->buf))
        return REC_BAD;
    memcpy(r->buf + r->have, p, len);
    r->have += len;
    while (n < max && reader_pull(r, &ev[n]) == REC_EMITTED)
        n++;
    return n;
}

static size_t build_data_record(u8 *out, u16 payload_len, u32 seed)
{
    struct tbt_fec_hdr h = data_hdr();
    u8 payload[TBT_FEC_PAYLOAD_MAX];

    h.payload_len = payload_len;
    if (payload_len < 64)
        h.flags = TBT_FEC_FF_LAST;
    fill_pattern(payload, payload_len, seed);
    tbt_fec_hdr_seal(&h, payload);
    tbt_fec_hdr_pack(&h, out);
    memcpy(out + TBT_FEC_HDR_LEN, payload, payload_len);
    return TBT_FEC_HDR_LEN + payload_len;
}

static size_t build_ack_record(u8 *out, u64 next_deliver)
{
    struct tbt_fec_hdr h = ctl_hdr(TBT_FEC_ACK, TBT_FEC_ACK_LEN);
    struct tbt_fec_ack a;
    u8 body[TBT_FEC_ACK_LEN];

    memset(&a, 0, sizeof(a));
    a.next_deliver = next_deliver;
    a.credit_packets = 16;
    a.rtt_us = 1234;
    h.seq = next_deliver;
    tbt_fec_ack_pack(&a, body);
    tbt_fec_hdr_seal(&h, body);
    tbt_fec_hdr_pack(&h, out);
    memcpy(out + TBT_FEC_HDR_LEN, body, TBT_FEC_ACK_LEN);
    return TBT_FEC_HDR_LEN + TBT_FEC_ACK_LEN;
}

static void test_framing(void)
{
    u8 rec[TBT_FEC_REC_MAX * 2];
    struct fec_reader r;
    struct fec_record ev[4];
    size_t n1, n2;
    int n;

    /* A DATA record cut in the middle of its payload: NEED_MORE, no event.
     * The ACK bytes that follow must not be parsed as a header. */
    n1 = build_data_record(rec, 64, 11);
    n2 = build_ack_record(rec + n1, 100);
    memset(&r, 0, sizeof(r));
    memset(ev, 0, sizeof(ev));
    n = reader_feed(&r, rec, TBT_FEC_HDR_LEN + 32, ev, 4);
    ok_int(n, 0, "partial DATA emits nothing");
    ok(r.have == TBT_FEC_HDR_LEN + 32, "partial DATA stays buffered", NULL);
    ok(ev[0].type == 0, "no record emitted before the payload ends", NULL);

    n = reader_feed(&r, rec + TBT_FEC_HDR_LEN + 32, (n1 - TBT_FEC_HDR_LEN - 32) + n2, ev, 4);
    ok_int(n, 2, "DATA then ACK after the payload completes");
    ok(ev[0].type == TBT_FEC_DATA && ev[0].payload_len == 64, "first event is DATA", NULL);
    ok(ev[1].type == TBT_FEC_ACK && ev[1].payload_len == TBT_FEC_ACK_LEN,
       "second event is ACK", NULL);
    ok(r.have == 0, "reader drained", NULL);

    /* Two records back-to-back in one feed. */
    n1 = build_data_record(rec, 40, 21);
    n2 = build_ack_record(rec + n1, 7);
    memset(&r, 0, sizeof(r));
    memset(ev, 0, sizeof(ev));
    n = reader_feed(&r, rec, n1 + n2, ev, 4);
    ok_int(n, 2, "two records in one feed");
    ok(ev[0].type == TBT_FEC_DATA && ev[0].payload_len == 40, "back-to-back DATA length",
       NULL);
    ok(ev[1].type == TBT_FEC_ACK && ev[1].payload_len == TBT_FEC_ACK_LEN,
       "back-to-back ACK length", NULL);

    /* A one-byte last DATA record is a complete record too. */
    n1 = build_data_record(rec, 1, 31);
    memset(&r, 0, sizeof(r));
    memset(ev, 0, sizeof(ev));
    n = reader_feed(&r, rec, n1, ev, 4);
    ok_int(n, 1, "one-byte DATA record emits");
    ok(ev[0].payload_len == 1, "one-byte payload boundary", NULL);
}

static void test_replay(void)
{
    struct tbt_fec_replay *r;
    u8 rec[96], out[96];
    u16 len = 80;
    int i;

    r = tbt_fec_replay_new(2, 4, 2, 64);
    ok(r != NULL, "replay ring allocated", NULL);
    ok_u32(tbt_fec_replay_k(r), TBT_FEC_REPLAY_K, "replay K");
    ok(tbt_fec_replay_bytes(2, 4, 2, 64, TBT_FEC_REPLAY_K) <=
           TBT_FEC_REPLAY_BUDGET,
       "replay fits the budget", NULL);
    for (i = 0; i < (int)len; i++)
        rec[i] = (u8)(i * 7 + 3);
    ok_int(tbt_fec_replay_store(r, 7, 1, 5, rec, len), 0, "replay store");
    ok(tbt_fec_replay_has(r, 7, 1, 5), "replay has", NULL);
    memset(out, 0, sizeof(out));
    ok_int(tbt_fec_replay_get(r, 7, 1, 5, out, sizeof(out)), len,
           "replay get length");
    ok(!memcmp(out, rec, len), "replay get bytes", NULL);
    ok_int(tbt_fec_replay_get(r, 7, 1, 4, out, sizeof(out)), -ENODATA,
           "replay never stored");
    ok_int(tbt_fec_replay_get(r, 6, 1, 5, out, sizeof(out)), -ESTALE,
           "replay evicted block");
    ok_int(tbt_fec_replay_get(r, 7, 1, 5, out, 1), -ENOSPC,
           "replay short buffer");
    ok_int(tbt_fec_replay_store(r, 7, 2, 0, rec, len), -EINVAL,
           "replay group out of range");
    ok_int(tbt_fec_replay_store(r, 7, 0, 6, rec, len), -EINVAL,
           "replay share out of range");
    ok_int(tbt_fec_replay_store(r, 7, 0, 0, rec, 105), -EINVAL,
           "replay record too long");
    /* A block K later reuses the slot: the old block is gone. */
    ok_int(tbt_fec_replay_store(r, 7 + TBT_FEC_REPLAY_K, 1, 5, rec, len), 0,
           "replay evicting store");
    ok(!tbt_fec_replay_has(r, 7, 1, 5), "replay evicted", NULL);
    ok(tbt_fec_replay_has(r, 7 + TBT_FEC_REPLAY_K, 1, 5), "replay new block",
       NULL);
    tbt_fec_replay_free(r);

    /* Every block the receiver may NACK (K) is resident; one past K evicts
     * the oldest. The data plane caps credit to K-2 super-blocks, so the
     * sender never advances that far while the receiver is still NACKing. */
    r = tbt_fec_replay_new(1, 4, 2, 64);
    for (i = 0; i < TBT_FEC_REPLAY_K; i++)
        ok_int(tbt_fec_replay_store(r, (u64)i, 0, 0, rec, len), 0,
               "store a full NACK window");
    for (i = 0; i < TBT_FEC_REPLAY_K; i++)
        ok(tbt_fec_replay_has(r, (u64)i, 0, 0), "window block resident",
           NULL);
    ok_int(tbt_fec_replay_get(r, 0, 0, 1, out, sizeof(out)), -ENODATA,
           "resident block with an unstored share is -ENODATA");
    ok_int(tbt_fec_replay_store(r, TBT_FEC_REPLAY_K, 0, 0, rec, len), 0,
           "store one past the window");
    ok(!tbt_fec_replay_has(r, 0, 0, 0), "oldest window block evicted", NULL);
    ok_int(tbt_fec_replay_get(r, 0, 0, 0, out, sizeof(out)), -ESTALE,
           "evicted block is not reported as never stored");
    ok_int(tbt_fec_replay_get(r, 0, 0, 1, out, sizeof(out)), -ESTALE,
           "evicted block's unstored share is -ESTALE");
    tbt_fec_replay_free(r);

    /* A 100-byte short stream is one packet in group 0 of a 32-group,
     * n=4 shape. Before the stream end is known, an unstarted group may be
     * legitimately empty: it must not be NACKed. After FIN, only the groups
     * that really have packets may be requested. */
    ok(!tbt_fec_nack_unstarted_group(1, 32, 4, 0, 0, false),
       "sparse: no NACK before total_known (group 0)", NULL);
    ok(!tbt_fec_nack_unstarted_group(1, 32, 4, 0, 1, false),
       "sparse: no NACK before total_known (empty group)", NULL);
    ok(tbt_fec_nack_unstarted_group(1, 32, 4, 0, 0, true),
       "sparse: group 0 has the packet", NULL);
    ok(!tbt_fec_nack_unstarted_group(1, 32, 4, 0, 1, true),
       "sparse: final empty group is not NACKed", NULL);
    ok(tbt_fec_nack_unstarted_group(1024, 32, 4, 0, 1, true),
       "non-final missing block may be requested after total_known", NULL);

    /* The production 32-group shape (256 carriers, n=4, m=4) keeps K=7
     * inside the 15 MiB budget; credit is capped to K-2 super-blocks. */
    ok_u32(tbt_fec_replay_k_for(32, 4, 4, 8192), TBT_FEC_REPLAY_K,
           "production K from the shape");
    ok(tbt_fec_replay_k_for(32, 4, 4, 8192) >= 6,
       "production K is at least 6", NULL);
    ok((u32)(tbt_fec_replay_k_for(32, 4, 4, 8192) - 2) + 2 <=
           tbt_fec_replay_k_for(32, 4, 4, 8192),
       "credit window plus slack fits the ring", NULL);
    ok_u32(tbt_fec_replay_k_for(256, 128, 128, 8192), 0,
           "over-budget shape has no K");
    r = tbt_fec_replay_new(32, 4, 4, 8192);
    ok(r != NULL, "production replay ring allocated", NULL);
    ok_u32(tbt_fec_replay_k(r), TBT_FEC_REPLAY_K, "production replay K=7");
    ok(tbt_fec_replay_bytes(32, 4, 4, 8192, TBT_FEC_REPLAY_K) <=
           TBT_FEC_REPLAY_BUDGET,
       "production replay fits the budget", NULL);
    tbt_fec_replay_free(r);

    /* A shape that cannot hold one super-block in the budget gets no ring. */
    r = tbt_fec_replay_new(256, 128, 128, 8192);
    ok(r == NULL, "replay refuses a shape over budget", NULL);
    tbt_fec_replay_free(r);
    ok_u32(tbt_fec_replay_k(NULL), 0, "replay K of NULL");
}

static void test_tail_guard(void)
{
    /* total_pkts=2, groups=4, n=4: final block 0; groups 0/1 have one packet,
     * groups 2/3 are empty. A tail of 0 with a present DATA share is a stale
     * stream end and must not mark the block empty. */
    ok_u32(tbt_fec_blk_real_len_from_tail(0, 4, true, 2, 4, 0, 0, true), 1,
           "final tail 1 for a group with a packet");
    ok_u32(tbt_fec_blk_real_len_from_tail(0, 4, true, 2, 4, 0, 2, false), 0,
           "final empty group has length 0");
    ok_u32(tbt_fec_blk_real_len_from_tail(0, 4, true, 2, 4, 0, 2, true), 4,
           "stale tail 0 with a present DATA share keeps n");
    ok_u32(tbt_fec_blk_real_len_from_tail(1, 4, true, 2, 4, 0, 2, true), 1,
           "parity win_len wins over the tail");
    ok_u32(tbt_fec_blk_real_len_from_tail(0, 4, false, 0, 4, 0, 2, true), 4,
           "unknown total keeps n");
}

static void test_nack_pacing(void)
{
    struct tbt_fec_nack_state st;
    struct tbt_fec_nack n, n2;
    u64 t = 1000000000ULL;
    u32 i;

    memset(&st, 0, sizeof(st));
    memset(&n, 0, sizeof(n));
    memset(&n2, 0, sizeof(n2));
    tbt_fec_bitmap_set(n.group_mask, 0);
    tbt_fec_bitmap_set(n.share_mask, 2);
    tbt_fec_bitmap_set(n2.group_mask, 1);
    tbt_fec_bitmap_set(n2.share_mask, 3);

    ok(tbt_fec_nack_due(&st, t, 7, &n), "first NACK due", NULL);
    ok(st.tries == 1 && st.block_id == 7 && st.valid, "first NACK state", NULL);
    ok(!tbt_fec_nack_due(&st, t + 5 * 1000000ULL, 7, &n),
       "same NACK rate limited", NULL);
    ok(tbt_fec_nack_due(&st, t + TBT_FEC_NACK_RETRY_MS * 1000000ULL, 7, &n),
       "same NACK retried", NULL);
    ok(st.tries == 2, "retry count", NULL);
    /* No total cap: a hundred retries later it is still due. */
    t += TBT_FEC_NACK_RETRY_MS * 1000000ULL;
    for (i = 0; i < 100; i++)
    {
        t += TBT_FEC_NACK_RETRY_MS * 1000000ULL;
        ok(tbt_fec_nack_due(&st, t, 7, &n), "retry has no total cap", NULL);
    }
    /* A different block is a new request, subject to the global minimum. */
    ok(!tbt_fec_nack_due(&st, t + 1 * 1000000ULL, 8, &n2),
       "new block waits for the global minimum", NULL);
    ok(tbt_fec_nack_due(&st, t + TBT_FEC_NACK_MIN_MS * 1000000ULL, 8, &n2),
       "new block after the global minimum", NULL);
    ok(st.block_id == 8 && st.tries == 1, "new block resets the retry count",
       NULL);
    tbt_fec_nack_reset(&st);
    ok(!st.valid && st.tries == 0, "NACK state reset", NULL);
}

int main(void)
{
    test_header_roundtrip();
    test_hdr_check();
    test_ack_bytes();
    test_fin_valid();
    test_crc();
    test_bitmap();
    test_control();
    test_rlnc_single();
    test_rlnc_loss_reorder();
    test_grouped();
    test_group_mapping();
    test_group_reassembly();
    test_params();
    test_ops();
    test_framing();
    test_replay();
    test_tail_guard();
    test_nack_pacing();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
