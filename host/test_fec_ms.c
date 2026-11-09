/* test_fec_ms.c - host conformance test for the SC-LDPC stream mux.
 *
 * Links tbtun/tbt_fec_ms.c and tbtun/tbt_fec.c, the same objects the module
 * builds, so a framing rule that passes here is a rule the kernel enforces.
 * No root, no module, no kernel headers, no fixture files. It exits non-zero
 * when any check fails.
 *
 *   make host/test_fec_ms && ./host/test_fec_ms
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec_ms.h"

static int failures;
static int checks;
static const char *phase = "startup";

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    if (failures <= 100)
        printf("FAIL [%s] %s%s%s\n", phase, what,
               detail && *detail ? ": " : "", detail ? detail : "");
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

    snprintf(buf, sizeof(buf), "got %llu, want %llu",
             (unsigned long long)got, (unsigned long long)want);
    ok(got == want, what, buf);
}

static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);

    if (!p)
    {
        fprintf(stderr, "test_fec_ms: out of memory\n");
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
        p[i] = (u8)x;
    }
}

static u32 rnd_next(u32 *state)
{
    u32 x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ------------------------------------------------------------------ *
 * Header framing
 * ------------------------------------------------------------------ */

static int repack_check(struct tbt_ms_hdr *h, u32 max_payload)
{
    u8 raw[TBT_MS_HDR_LEN];

    if (tbt_ms_hdr_pack(h, raw))
        return -EINVAL;
    h->crc32c = (u32)raw[28] | ((u32)raw[29] << 8) |
                ((u32)raw[30] << 16) | ((u32)raw[31] << 24);
    return tbt_ms_hdr_check(h, max_payload);
}

static void test_header(void)
{
    struct tbt_ms_hdr h, u;
    u8 raw[TBT_MS_HDR_LEN];

    memset(&h, 0, sizeof(h));
    h.magic = TBT_MS_MAGIC;
    h.ver = TBT_MS_VER;
    h.type = TBT_MS_DATA;
    h.len = 5;
    h.sid = 0x1122334455ULL;
    h.off = 0x1122334455667788ULL;
    ok_int(tbt_ms_hdr_pack(&h, raw), 0, "header pack");
    ok(raw[0] == 'T' && raw[1] == 'M' && raw[2] == 'S' && raw[3] == '1',
       "magic is the little-endian TMS1 bytes", NULL);
    ok(raw[4] == TBT_MS_VER, "version byte", NULL);
    ok(raw[5] == TBT_MS_DATA, "type byte", NULL);
    ok(raw[6] == 0 && raw[7] == 0, "flags are zero", NULL);
    ok(raw[8] == 5 && raw[9] == 0 && raw[10] == 0 && raw[11] == 0,
       "len is little-endian", NULL);
    ok(raw[12] == 0x55 && raw[13] == 0x44 && raw[14] == 0x33 &&
           raw[15] == 0x22 && raw[16] == 0x11,
       "sid is little-endian", NULL);
    ok_u32((u32)raw[28] | ((u32)raw[29] << 8) | ((u32)raw[30] << 16) |
               ((u32)raw[31] << 24),
           tbt_fec_crc32c(0, raw, TBT_MS_HDR_LEN - sizeof(u32)),
           "header CRC covers bytes 0..27");
    ok_int(tbt_ms_hdr_unpack(raw, &u), 0, "header unpack");
    ok_int(tbt_ms_hdr_check(&u, TBT_MS_DATA_MAX), 0,
           "unpacked header passes check");
    ok(u.magic == h.magic && u.ver == h.ver && u.type == h.type &&
           u.flags == h.flags && u.len == h.len && u.sid == h.sid &&
           u.off == h.off,
       "header pack/unpack round trip", NULL);
    ok_u32(u.crc32c,
           (u32)raw[28] | ((u32)raw[29] << 8) | ((u32)raw[30] << 16) |
               ((u32)raw[31] << 24),
           "unpacked CRC is the packed CRC");

    ok_int(tbt_ms_hdr_pack(NULL, raw), -EINVAL, "pack rejects a NULL header");
    ok_int(tbt_ms_hdr_pack(&h, NULL), -EINVAL, "pack rejects NULL output");
    ok_int(tbt_ms_hdr_unpack(NULL, &u), -EINVAL,
           "unpack rejects NULL input");
    ok_int(tbt_ms_hdr_unpack(raw, NULL), -EINVAL,
           "unpack rejects NULL output");
    ok_int(tbt_ms_hdr_check(NULL, TBT_MS_DATA_MAX), -EINVAL,
           "check rejects a NULL header");

    raw[28] ^= 1;
    ok_int(tbt_ms_hdr_unpack(raw, &u), 0, "unpack a CRC-corrupt header");
    ok_int(tbt_ms_hdr_check(&u, TBT_MS_DATA_MAX), -EPROTO,
           "check rejects a bad CRC");
    raw[28] ^= 1;

    h = u;
    h.magic = 0;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "bad magic");
    h.magic = TBT_MS_MAGIC;
    h.ver = TBT_MS_VER + 1;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "bad version");
    h.ver = TBT_MS_VER;
    h.type = 0;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "bad type 0");
    h.type = 4;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "bad type 4");
    h.type = TBT_MS_DATA;
    h.flags = 1;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "nonzero flags");
    h.flags = 0;
    h.sid = 0;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "sid 0");
    h.sid = 7;
    h.len = 0;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "DATA len 0");
    h.len = TBT_MS_DATA_MAX + 1;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO,
           "DATA len above the maximum payload");
    h.len = TBT_MS_DATA_MAX;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), 0,
           "DATA len at the maximum payload");
    ok_int(repack_check(&h, TBT_MS_DATA_MAX + 1), -EPROTO,
           "a maximum payload above the wire ceiling is rejected");
    h.len = 100;
    ok_int(repack_check(&h, 99), -EPROTO,
           "DATA len above the configured maximum");
    h.len = 8;
    h.off = ~(u64)0 - 3;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO,
           "DATA offset plus length wraps");

    memset(&h, 0, sizeof(h));
    h.magic = TBT_MS_MAGIC;
    h.ver = TBT_MS_VER;
    h.type = TBT_MS_FIN;
    h.sid = 9;
    h.off = 1234;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), 0, "FIN header with len 0");
    h.len = 1;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "FIN with payload");
    h.len = 0;
    h.type = TBT_MS_RST;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), 0, "RST header with len 0");
    h.len = 1;
    ok_int(repack_check(&h, TBT_MS_DATA_MAX), -EPROTO, "RST with payload");
}

/* ------------------------------------------------------------------ *
 * Sender ring and window reports
 * ------------------------------------------------------------------ */

static void test_tx_ring(void)
{
    struct tbt_ms_tx *tx;
    u64 a = 0x1000000000ULL + 1, b = 0x1000000000ULL + 2;
    u64 c = 0x1000000000ULL + 3, d = 0x1000000000ULL + 4;
    u8 buf[32768];
    const u8 *blk;
    u32 blen;
    u8 *p;
    u32 got;

    tx = tbt_ms_tx_new(3, TBT_MS_HDR_LEN + TBT_MS_DATA_MAX, 32768, 1000);
    ok(tx != NULL, "tx constructor", NULL);
    if (!tx)
        return;
    ok(tbt_ms_tx_new(0, 65536, 32768, 1000) == NULL,
       "tx constructor rejects 0 streams", NULL);
    ok(tbt_ms_tx_new(TBT_MS_MAX_STREAMS + 1, 65536, 32768, 1000) == NULL,
       "tx constructor rejects too many streams", NULL);
    ok(tbt_ms_tx_new(1, TBT_MS_HDR_LEN + TBT_MS_DATA_MAX - 1, 32768, 1) ==
           NULL,
       "tx constructor rejects a short block", NULL);
    ok(tbt_ms_tx_new(1, 65536, 2 * TBT_MS_DATA_MAX - 1, 1) == NULL,
       "tx constructor rejects a short window", NULL);

    ok_int(tbt_ms_tx_open(tx, a), 0, "tx open");
    ok_int(tbt_ms_tx_open(tx, a), -EEXIST, "tx duplicate open is refused");
    ok_u32(tbt_ms_tx_space(tx, a), 32768, "a fresh stream has a full window");
    p = NULL;
    got = tbt_ms_tx_reserve(tx, a, 100, &p);
    ok_u32(got, 100, "reserve returns the requested tail room");
    ok(p != NULL, "reserve returns a pointer", NULL);
    tbt_ms_tx_commit(tx, a, got);
    ok_u32(tbt_ms_tx_space(tx, a), 32668, "commit charges the window");
    fill_pattern(buf, sizeof(buf), 1);
    ok_u32(tbt_ms_tx_write(tx, a, buf, 1000), 1000, "tx write accepts bytes");
    ok_u32(tbt_ms_tx_queued(tx), 1100, "queued bytes count the accepted tail");
    ok(tbt_ms_tx_pending(tx, a), "a stream with bytes is pending", NULL);
    ok(!tbt_ms_tx_has_control(tx), "no control frame yet", NULL);
    ok_int(tbt_ms_tx_fin(tx, a), 0, "queue FIN");
    ok_int(tbt_ms_tx_fin(tx, a), 0, "FIN is idempotent");
    ok(tbt_ms_tx_has_control(tx), "FIN is pending control", NULL);
    ok_u32(tbt_ms_tx_space(tx, a), 0, "no data after FIN");
    ok_u32(tbt_ms_tx_write(tx, a, buf, 10), 0, "write after FIN is refused");

    ok_int(tbt_ms_tx_open(tx, b), 0, "tx open b");
    fill_pattern(buf, sizeof(buf), 2);
    ok_u32(tbt_ms_tx_write(tx, b, buf, 1000), 1000, "tx write b");
    ok_u32(tbt_ms_tx_space(tx, b), 31768, "space tracks the accepted bytes");
    ok_int(tbt_ms_tx_window(tx, b, 0, 0), 0,
           "a zero credit report is ignored");
    ok_u32(tbt_ms_tx_space(tx, b), 31768, "zero credit keeps the old credit");
    ok_int(tbt_ms_tx_window(tx, b, 500, 4096), 0, "tx window report");
    ok_u32(tbt_ms_tx_space(tx, b), 4096 - 500,
           "space follows delivered plus credit");
    ok_int(tbt_ms_tx_window(tx, b, 400, 4096), 0,
           "a delivered regression is ignored");
    ok_u32(tbt_ms_tx_space(tx, b), 4096 - 500,
           "ignored regression does not change space");
    ok_int(tbt_ms_tx_window(tx, b, 500, 32768), 0,
           "a larger credit is accepted");
    ok_u32(tbt_ms_tx_space(tx, b), 32268,
           "space uses the new credit");
    ok_int(tbt_ms_tx_window(tx, b, 2000, 32768), 0,
           "delivered beyond the sent tail is clamped");
    ok_u32(tbt_ms_tx_space(tx, b), 32768,
           "a clamped report keeps a full credit window");
    got = 32768;
    ok_u32(tbt_ms_tx_write(tx, b, buf, got), got, "fill the stream ring");
    ok_u32(tbt_ms_tx_space(tx, b), 0, "stream ring is full");

    ok_int(tbt_ms_tx_open(tx, c), 0, "tx open c");
    ok_int(tbt_ms_tx_open(tx, d), -ENOSPC, "stream table is bounded");
    ok_u32(tbt_ms_tx_write(tx, c, buf, 50), 50, "tx write c");
    ok_u32(tbt_ms_tx_queued(tx), 1100 + 33768 + 50,
           "queued sums every stream");
    ok_int(tbt_ms_tx_rst(tx, c), 0, "queue RST");
    ok_u32(tbt_ms_tx_queued(tx), 1100 + 33768,
           "RST drops the unsent stream bytes");
    ok(tbt_ms_tx_has_control(tx), "RST is pending control", NULL);
    blen = 0;
    blk = NULL;
    ok_int(tbt_ms_tx_block(tx, &blk, &blen), 0, "tx block");
    ok(blen > 0 && blen <= TBT_MS_HDR_LEN + TBT_MS_DATA_MAX,
       "tx block length is bounded", NULL);
    ok(!tbt_ms_tx_pending(tx, c), "RST leaves the stream unpended", NULL);
    ok(!tbt_ms_tx_pending(tx, a), "FIN leaves the stream unpended", NULL);
    ok(tbt_ms_tx_pending(tx, b), "unpacked bytes keep b pending", NULL);
    ok_int(tbt_ms_tx_fin(tx, a), 0, "FIN after the wire stays idempotent");
    ok(!tbt_ms_tx_has_control(tx), "a sent FIN queues no control", NULL);
    ok(!tbt_ms_tx_pending(tx, a), "a sent FIN does not repend the stream",
       NULL);

    ok_int(tbt_ms_tx_close(tx, c), 0, "tx close");
    ok_int(tbt_ms_tx_close(tx, c), -ENOENT, "tx close twice is -ENOENT");
    ok_int(tbt_ms_tx_open(tx, c), 0, "reopen after close");
    ok_int(tbt_ms_tx_window(tx, 0xdeadbeef, 0, 0), -ENOENT,
           "window report for an unknown stream");
    ok_u32(tbt_ms_tx_space(tx, 0xdeadbeef), 0, "space of an unknown stream");
    ok(!tbt_ms_tx_pending(tx, 0xdeadbeef), "pending of an unknown stream",
       NULL);
    ok_u32(tbt_ms_tx_write(tx, 0xdeadbeef, buf, 8), 0,
           "write to an unknown stream");
    tbt_ms_tx_free(tx);
    tbt_ms_tx_free(NULL);
}

/* ------------------------------------------------------------------ *
 * Link harness: one sender, one receiver, loss-free in-order delivery
 * ------------------------------------------------------------------ */

#define TEST_MAX_STREAMS 16

struct test_stream
{
    u64 sid;
    u32 size;
    bool want_fin;
    bool want_rst;
    bool submitted;
    bool rst_seen;
    u8 *data;
    u32 written;
    u32 delivered;
    u64 last_win;
};

struct test_link
{
    struct tbt_ms_tx *tx;
    struct tbt_ms_rx *rx;
    u32 block_bytes;
    u32 window;
    u16 n;
    u32 rng;
    struct test_stream s[TEST_MAX_STREAMS];
};

static void link_init(struct test_link *l, u16 n, u32 block_bytes,
                      u32 window, u32 quantum, u32 seed)
{
    u16 i;

    memset(l, 0, sizeof(*l));
    l->n = n;
    l->block_bytes = block_bytes;
    l->window = window;
    l->rng = seed ? seed : 0x9e3779b9u;
    l->tx = tbt_ms_tx_new(n, block_bytes, window, quantum);
    l->rx = tbt_ms_rx_new(n, window);
    ok(l->tx != NULL, "link tx constructor", NULL);
    ok(l->rx != NULL, "link rx constructor", NULL);
    if (!l->tx || !l->rx)
    {
        fprintf(stderr, "test_fec_ms: link allocation failed\n");
        exit(2);
    }
    for (i = 0; i < n; i++)
        l->s[i].sid = 0x1000000000ULL + (u64)i * 0x1000003ULL + 7;
}

static void link_add(struct test_link *l, u16 i, u32 size, bool fin,
                     bool rst, bool preopen)
{
    struct test_stream *st = &l->s[i];

    st->size = size;
    st->want_fin = fin;
    st->want_rst = rst;
    st->data = xalloc(size ? size : 1);
    fill_pattern(st->data, size ? size : 1,
                 (u32)(st->sid ^ (st->sid >> 32)) + 0x51u * i);
    ok_int(tbt_ms_tx_open(l->tx, st->sid), 0, "link tx open");
    (void)preopen; /* RX legs are explicit; an unknown sid is dropped. */
    ok_int(tbt_ms_rx_open(l->rx, st->sid), 0, "link rx open");
}

static void link_free(struct test_link *l)
{
    u16 i;

    for (i = 0; i < l->n; i++)
        free(l->s[i].data);
    tbt_ms_tx_free(l->tx);
    tbt_ms_rx_free(l->rx);
    memset(l, 0, sizeof(*l));
}

/* Parse one packed block: every byte is a complete header plus payload. */
static int block_walk(const struct test_link *l, const u8 *b, u32 len,
                      u64 *payload, u32 *frames)
{
    u32 off = 0;
    u32 nframes = 0;
    u16 i;

    memset(payload, 0, (size_t)l->n * sizeof(*payload));
    while (off < len)
    {
        struct tbt_ms_hdr h;
        u32 plen;
        bool known = false;

        if (len - off < TBT_MS_HDR_LEN)
            return -1;
        if (tbt_ms_hdr_unpack(b + off, &h))
            return -1;
        if (tbt_ms_hdr_check(&h, TBT_MS_DATA_MAX))
            return -1;
        plen = h.type == TBT_MS_DATA ? h.len : 0;
        if (plen > len - off - TBT_MS_HDR_LEN)
            return -1;
        for (i = 0; i < l->n; i++)
            if (l->s[i].sid == h.sid)
            {
                payload[i] += plen;
                known = true;
                break;
            }
        if (!known)
            return -1;
        nframes++;
        off += TBT_MS_HDR_LEN + plen;
    }
    *frames = nframes;
    return off == len ? 0 : -1;
}

static void feed_block(struct test_link *l, const u8 *buf, u32 len,
                       u32 fixed)
{
    u32 off = 0;

    while (off < len)
    {
        u32 remain = len - off;
        u32 chunk;

        if (fixed)
        {
            chunk = remain < fixed ? remain : fixed;
        }
        else
        {
            u32 r = rnd_next(&l->rng);

            if ((r & 3) == 0)
                chunk = 1 + (r >> 8) % 40;
            else
            {
                u32 cap = remain < TBT_MS_DATA_MAX ? remain
                                                   : TBT_MS_DATA_MAX;

                chunk = 1 + (r >> 8) % cap;
            }
            if (chunk > remain)
                chunk = remain;
        }
        ok_u32(tbt_ms_rx_feed(l->rx, buf + off, chunk), chunk,
               "feed consumes exactly the offered run");
        off += chunk;
    }
}

static bool pack_all(struct test_link *l, u32 fixed, u64 *payload)
{
    bool any = false;
    u16 i;

    for (;;)
    {
        const u8 *buf = NULL;
        u32 len = 0;
        u32 frames = 0;
        u64 got[TEST_MAX_STREAMS];

        ok_int(tbt_ms_tx_block(l->tx, &buf, &len), 0, "tx block");
        if (!len)
            break;
        ok(len <= l->block_bytes, "block never exceeds block_bytes", NULL);
        ok_int(block_walk(l, buf, len, got, &frames), 0,
               "block holds only whole subframes");
        ok(frames > 0, "a nonempty block holds a subframe", NULL);
        if (payload)
            for (i = 0; i < l->n; i++)
                payload[i] += got[i];
        feed_block(l, buf, len, fixed);
        any = true;
    }
    return any;
}

static void drain_stream(struct test_link *l, u16 i)
{
    struct test_stream *st = &l->s[i];

    for (;;)
    {
        const u8 *p = NULL;
        u32 len = 0;
        int rc;

        rc = tbt_ms_rx_peek(l->rx, st->sid, &p, 1u << 20, &len);
        if (rc == -ENOENT)
            return;
        ok_int(rc, 0, "peek a known stream");
        if (rc)
            return;
        if (!len)
            break;
        ok_u64(tbt_ms_rx_delivered(l->rx, st->sid), st->delivered,
               "delivered offset is monotone");
        if (st->delivered + len > st->size)
        {
            ok(0, "stream does not deliver beyond its payload", NULL);
            return;
        }
        ok(memcmp(p, st->data + st->delivered, len) == 0,
           "delivered bytes match the source", NULL);
        ok_int(tbt_ms_rx_consume(l->rx, st->sid, len), 0,
               "consume the delivered prefix");
        st->delivered += len;
    }
}

static void consume_expected(struct tbt_ms_rx *rx, u64 sid, const u8 *data,
                             u32 size, u32 *done, const char *what)
{
    for (;;)
    {
        const u8 *p = NULL;
        u32 len = 0;

        if (tbt_ms_rx_peek(rx, sid, &p, 1u << 20, &len))
        {
            ok(0, what, "peek failed");
            return;
        }
        if (!len)
            break;
        if (*done + len > size)
        {
            ok(0, what, "stream delivered beyond its payload");
            return;
        }
        ok(memcmp(p, data + *done, len) == 0, what, NULL);
        if (tbt_ms_rx_consume(rx, sid, len))
        {
            ok(0, what, "consume failed");
            return;
        }
        *done += len;
    }
}

static bool link_done(const struct test_link *l)
{
    u16 i;

    if (tbt_ms_tx_queued(l->tx) || tbt_ms_tx_has_control(l->tx))
        return false;
    for (i = 0; i < l->n; i++)
    {
        const struct test_stream *st = &l->s[i];

        if (!st->submitted || tbt_ms_tx_pending(l->tx, st->sid))
            return false;
        if (st->want_rst)
        {
            if (!st->rst_seen)
                return false;
        }
        else
        {
            u64 end = 0;

            if (!tbt_ms_rx_fin(l->rx, st->sid, &end))
                return false;
            if (end != st->size)
                return false;
            if (st->delivered != st->size)
                return false;
        }
    }
    return true;
}

static bool pump(struct test_link *l, u32 fixed, u32 max_iters)
{
    u32 iter;
    u16 i;

    for (iter = 0; iter < max_iters; iter++)
    {
        bool packed;

        for (i = 0; i < l->n; i++)
        {
            struct test_stream *st = &l->s[i];

            if (st->submitted)
                continue;
            if (st->written == st->size)
            {
                int rc;

                if (st->want_rst)
                    rc = tbt_ms_tx_rst(l->tx, st->sid);
                else if (st->want_fin)
                    rc = tbt_ms_tx_fin(l->tx, st->sid);
                else
                    rc = 0;
                ok_int(rc, 0, "submit stream control");
                st->submitted = true;
                continue;
            }
            else
            {
                u32 space = tbt_ms_tx_space(l->tx, st->sid);
                u32 want = st->size - st->written;
                u32 got;

                if (!space)
                    continue;
                if (want > space)
                    want = space;
                if (want > 4096)
                    want = 4096;
                got = tbt_ms_tx_write(l->tx, st->sid,
                                      st->data + st->written, want);
                ok_u32(got, want, "tx write accepts the offered bytes");
                st->written += got;
            }
        }
        packed = pack_all(l, fixed, NULL);
        for (i = 0; i < l->n; i++)
        {
            struct test_stream *st = &l->s[i];

            if (st->want_rst && tbt_ms_rx_reset(l->rx, st->sid))
            {
                if (!st->rst_seen)
                {
                    st->rst_seen = true;
                    ok_u32(tbt_ms_rx_avail(l->rx, st->sid), 0,
                           "RST leaves an empty stream buffer");
                    ok_u64(tbt_ms_rx_delivered(l->rx, st->sid), 0,
                           "RST resets the delivered offset");
                }
                continue;
            }
            drain_stream(l, i);
        }
        for (i = 0; i < l->n; i++)
        {
            struct test_stream *st = &l->s[i];
            u64 delivered = 0;
            u32 credit = 0;

            if (st->want_rst && st->rst_seen)
                continue;
            if (!tbt_ms_rx_win(l->rx, st->sid, &delivered, &credit, !packed))
                continue;
            ok(delivered >= st->last_win,
               "window delivered offset is monotone", NULL);
            st->last_win = delivered;
            ok_int(tbt_ms_tx_window(l->tx, st->sid, delivered, credit), 0,
                   "apply a window report");
            tbt_ms_rx_win_ack(l->rx, st->sid, delivered);
        }
        if (link_done(l))
            return true;
    }
    return false;
}

static void check_link_complete(struct test_link *l, const char *what)
{
    char detail[128];
    u16 i;

    snprintf(detail, sizeof(detail), "%s transfer did not finish", what);
    ok(pump(l, 0, 200000), what, detail);
    ok_u32(tbt_ms_tx_queued(l->tx), 0, "all queued bytes left the sender");
    ok(!tbt_ms_tx_has_control(l->tx), "all control frames left the sender",
       NULL);
    for (i = 0; i < l->n; i++)
    {
        struct test_stream *st = &l->s[i];

        ok_u32(tbt_ms_rx_streams(l->rx), l->n,
               "every stream has a receiver leg");
        if (st->want_rst)
        {
            ok(tbt_ms_rx_reset(l->rx, st->sid), "RST reached the receiver",
               NULL);
            ok_u32(tbt_ms_rx_avail(l->rx, st->sid), 0,
                   "a reset stream buffers nothing");
        }
        else
        {
            u64 end = 0;

            ok(tbt_ms_rx_fin(l->rx, st->sid, &end),
               "FIN reached the receiver after all bytes", NULL);
            ok_u64(end, st->size, "FIN names the exact end offset");
            ok_u64(tbt_ms_rx_delivered(l->rx, st->sid), st->size,
                   "every stream byte was delivered");
            ok_u32(tbt_ms_rx_avail(l->rx, st->sid), 0,
                   "no stream bytes remain buffered");
        }
    }
}

/* ------------------------------------------------------------------ *
 * Boundary-sized payloads and single maximum subframes
 * ------------------------------------------------------------------ */

static void test_boundary_frames(void)
{
    static const u32 sizes[3] = { 8191, 8192, 8193 };
    struct test_link l;
    const u8 *buf = NULL;
    u32 len = 0, off;
    u32 first[3] = { 0, 0, 0 }, second[3] = { 0, 0, 0 };
    u32 frames[3] = { 0, 0, 0 };
    u16 i;

    link_init(&l, 3, 1u << 20, 65536, TBT_MS_DATA_MAX, 0x1111u);
    for (i = 0; i < 3; i++)
        link_add(&l, i, sizes[i], true, false, i == 0);
    for (i = 0; i < 3; i++)
        ok_u32(tbt_ms_tx_write(l.tx, l.s[i].sid, l.s[i].data, sizes[i]),
               sizes[i], "boundary payload accepted");
    for (i = 0; i < 3; i++)
        ok_int(tbt_ms_tx_fin(l.tx, l.s[i].sid), 0, "boundary FIN");
    ok_int(tbt_ms_tx_block(l.tx, &buf, &len), 0, "boundary block");
    ok(len > 0 && len <= (1u << 20), "boundary block length", NULL);
    off = 0;
    while (off < len)
    {
        struct tbt_ms_hdr h;
        u32 plen;
        u16 k;

        ok_int(tbt_ms_hdr_unpack(buf + off, &h), 0, "boundary header unpack");
        ok_int(tbt_ms_hdr_check(&h, TBT_MS_DATA_MAX), 0,
               "boundary header check");
        plen = h.type == TBT_MS_DATA ? h.len : 0;
        for (k = 0; k < 3; k++)
            if (l.s[k].sid == h.sid)
                break;
        ok(k < 3, "boundary block names a known stream", NULL);
        if (k < 3 && h.type == TBT_MS_DATA)
        {
            if (!frames[k])
                first[k] = plen;
            else if (frames[k] == 1)
                second[k] = plen;
            frames[k]++;
        }
        off += TBT_MS_HDR_LEN + plen;
    }
    ok_u32(off, len, "boundary block has no padding");
    ok_u32(frames[0], 1, "8191 fits one subframe");
    ok_u32(first[0], 8191, "8191 subframe length");
    ok_u32(frames[1], 1, "8192 fits one subframe");
    ok_u32(first[1], 8192, "8192 subframe length");
    ok_u32(frames[2], 2, "8193 splits at the maximum payload");
    ok_u32(first[2], 8192, "8193 first subframe is the maximum");
    ok_u32(second[2], 1, "8193 second subframe is the remainder");

    feed_block(&l, buf, len, 0);
    for (i = 0; i < 3; i++)
    {
        u64 end = 0;

        drain_stream(&l, i);
        ok_u64(l.s[i].delivered, sizes[i], "boundary stream delivered");
        ok(tbt_ms_rx_fin(l.rx, l.s[i].sid, &end),
           "boundary stream sees FIN", NULL);
        ok_u64(end, sizes[i], "boundary FIN offset");
    }
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Concurrent streams
 * ------------------------------------------------------------------ */

static u32 multi_size(u16 i)
{
    switch (i % 5)
    {
    case 0:
        return 8191;
    case 1:
        return 8192;
    case 2:
        return 8193;
    case 3:
        return 1u + (u32)i * 37u % 64u;
    default:
        return 300000u + (u32)(i / 5) * 8192u;
    }
}

static void test_concurrency(u16 n, u32 block_bytes)
{
    struct test_link l;
    char name[64];
    u16 i;
    u64 seen = 0;
    u64 sid = 0;

    snprintf(name, sizeof(name), "%u concurrent streams", (unsigned)n);
    link_init(&l, n, block_bytes, 32768, 1024, 0x2222u + n);
    for (i = 0; i < n; i++)
        link_add(&l, i, multi_size(i), true, false, (i & 1) == 0);
    check_link_complete(&l, name);
    ok_u32(tbt_ms_rx_streams(l.rx), n, "leg count matches the stream count");
    while (tbt_ms_rx_next(l.rx, &sid))
    {
        seen++;
        if (seen > n)
            break;
    }
    ok_u64(seen, n, "delivery walk observes every finished stream once");
    ok_int(tbt_ms_rx_next(l.rx, &sid), 0, "delivery walk then returns 0");
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * FEC share slicing: 8192-byte chunks, last short, in order
 * ------------------------------------------------------------------ */

static void test_share_slicing(void)
{
    static const u32 sizes[4] = { 5000, 10000, 1, 8193 };
    struct test_link l;
    u16 i;

    link_init(&l, 4, 1u << 20, 65536, 8192, 0x3333u);
    for (i = 0; i < 4; i++)
        link_add(&l, i, sizes[i], true, false, false);
    for (i = 0; i < 4; i++)
    {
        u32 want = sizes[i];

        ok_u32(tbt_ms_tx_write(l.tx, l.s[i].sid, l.s[i].data, want), want,
               "share-sliced write");
        ok_int(tbt_ms_tx_fin(l.tx, l.s[i].sid), 0, "share-sliced FIN");
        l.s[i].submitted = true;
        l.s[i].written = want;
    }
    /* Pack every block once, cut each into 8192-byte pieces, and feed in
     * order: the FEC plane never reorders what this parser sees. */
    for (;;)
    {
        const u8 *buf = NULL;
        u32 len = 0;

        ok_int(tbt_ms_tx_block(l.tx, &buf, &len), 0, "slice block");
        if (!len)
            break;
        feed_block(&l, buf, len, TBT_MS_DATA_MAX);
    }
    for (i = 0; i < 4; i++)
    {
        u64 end = 0;

        drain_stream(&l, i);
        ok_u64(l.s[i].delivered, sizes[i], "share-sliced stream delivered");
        ok(tbt_ms_rx_fin(l.rx, l.s[i].sid, &end), "share-sliced FIN", NULL);
        ok_u64(end, sizes[i], "share-sliced FIN offset");
    }
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Backpressure
 * ------------------------------------------------------------------ */

static void test_backpressure(void)
{
    struct test_link l;
    u64 a_payload = 0, b_payload = 0;
    u64 packed[TEST_MAX_STREAMS];
    u32 iter;
    bool b_done = false;

    link_init(&l, 2, 65536, 3u * TBT_MS_DATA_MAX, 1024, 0x4444u);
    link_add(&l, 0, 100000, true, false, false);
    link_add(&l, 1, 200000, true, false, false);
    /* Both streams start with one full window of accepted bytes. */
    ok_u32(tbt_ms_tx_write(l.tx, l.s[0].sid, l.s[0].data, l.window),
           l.window, "backpressure fills stream A's window");
    l.s[0].written = l.window;
    ok_u32(tbt_ms_tx_write(l.tx, l.s[1].sid, l.s[1].data, l.window),
           l.window, "backpressure fills stream B's window");
    l.s[1].written = l.window;

    for (iter = 0; iter < 60000 && !b_done; iter++)
    {
        u32 space, want, got;

        if (l.s[1].written == l.s[1].size)
        {
            if (!l.s[1].submitted)
            {
                ok_int(tbt_ms_tx_fin(l.tx, l.s[1].sid), 0,
                       "backpressure FIN for B");
                l.s[1].submitted = true;
            }
        }
        else
        {
            space = tbt_ms_tx_space(l.tx, l.s[1].sid);
            want = l.s[1].size - l.s[1].written;
            if (want > space)
                want = space;
            if (want > 8192)
                want = 8192;
            if (want)
            {
                got = tbt_ms_tx_write(l.tx, l.s[1].sid,
                                      l.s[1].data + l.s[1].written, want);
                ok_u32(got, want, "backpressure writes B");
                l.s[1].written += got;
            }
        }
        memset(packed, 0, sizeof(packed));
        (void)pack_all(&l, 0, packed);
        a_payload += packed[0];
        b_payload += packed[1];
        drain_stream(&l, 1);
        {
            u64 delivered = 0;
            u32 credit = 0;

            if (tbt_ms_rx_win(l.rx, l.s[1].sid, &delivered, &credit, true))
            {
                ok_int(tbt_ms_tx_window(l.tx, l.s[1].sid, delivered,
                                        credit),
                       0, "backpressure window for B");
                tbt_ms_rx_win_ack(l.rx, l.s[1].sid, delivered);
            }
        }
        ok(tbt_ms_rx_avail(l.rx, l.s[0].sid) <= l.window,
           "stream A buffers no more than its window", NULL);
        ok_u64(tbt_ms_rx_delivered(l.rx, l.s[0].sid), 0,
               "stream A delivers nothing while unconsumed");
        ok_u32(tbt_ms_tx_space(l.tx, l.s[0].sid), 0,
               "stream A has no credit");
        {
            u64 end = 0;

            b_done = l.s[1].submitted &&
                     tbt_ms_rx_fin(l.rx, l.s[1].sid, &end) &&
                     end == l.s[1].size &&
                     !tbt_ms_tx_pending(l.tx, l.s[1].sid);
        }
    }
    ok(b_done, "free-credit stream B completes while A is blocked", NULL);
    ok_u64(a_payload, l.window,
           "blocked stream A packs exactly one window");
    ok(b_payload > l.window, "stream B moves past the window on new credit",
       NULL);
    ok_u32(tbt_ms_rx_avail(l.rx, l.s[0].sid), l.window,
           "stream A's whole window reached the receiver");

    drain_stream(&l, 0);
    ok_u64(l.s[0].delivered, l.window, "stream A drains its first window");
    {
        u64 delivered = 0;
        u32 credit = 0;

        ok_int(tbt_ms_rx_win(l.rx, l.s[0].sid, &delivered, &credit, true), 1,
               "forced window report for A");
        ok_u64(delivered, l.window, "force reports the consumed offset");
        ok_u32(credit, l.window, "window report names the ring size");
        ok_int(tbt_ms_tx_window(l.tx, l.s[0].sid, delivered, credit), 0,
               "apply A's window report");
        tbt_ms_rx_win_ack(l.rx, l.s[0].sid, delivered);
    }
    ok(tbt_ms_tx_space(l.tx, l.s[0].sid) > 0,
       "consumed and reported A regains credit", NULL);
    l.s[1].submitted = true;
    ok(pump(&l, 0, 100000), "stream A completes after its window report",
       NULL);
    ok_u64(l.s[0].delivered, l.s[0].size, "stream A delivers every byte");
    ok_u64(l.s[1].delivered, l.s[1].size, "stream B delivers every byte");
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Round-robin fairness
 * ------------------------------------------------------------------ */

static void test_fairness(void)
{
    struct test_link l;
    u64 total[TEST_MAX_STREAMS];
    u32 b;
    u32 frames = 0;

    link_init(&l, 2, 65536, 262144, TBT_MS_QUANTUM, 0x5555u);
    link_add(&l, 0, 200000, true, false, false);
    link_add(&l, 1, 200000, true, false, false);
    ok_u32(tbt_ms_tx_write(l.tx, l.s[0].sid, l.s[0].data, 200000), 200000,
           "fairness write A");
    ok_u32(tbt_ms_tx_write(l.tx, l.s[1].sid, l.s[1].data, 200000), 200000,
           "fairness write B");
    memset(total, 0, sizeof(total));
    for (b = 0; b < 8; b++)
    {
        const u8 *buf = NULL;
        u32 len = 0;
        u64 got[TEST_MAX_STREAMS];

        ok_int(tbt_ms_tx_block(l.tx, &buf, &len), 0, "fairness block");
        ok(len > 0, "fairness block is nonempty", NULL);
        ok_int(block_walk(&l, buf, len, got, &frames), 0,
               "fairness block framing");
        total[0] += got[0];
        total[1] += got[1];
    }
    ok(total[0] > 0 && total[1] > 0, "both fair streams made progress", NULL);
    ok(total[0] * 4 >= total[1] && total[1] * 4 >= total[0],
       "eight blocks keep both streams within a 4:1 ratio", NULL);
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Mixed FIN and mid-stream RST
 * ------------------------------------------------------------------ */

static void test_mixed_controls(void)
{
    static const u32 sizes[4] = { 5000, 30000, 8193, 20000 };
    struct test_link l;
    u8 frame[TBT_MS_HDR_LEN + 4];
    struct tbt_ms_hdr h;
    u16 i;

    link_init(&l, 4, 65536, 32768, 1024, 0x6666u);
    link_add(&l, 0, sizes[0], true, false, true);
    link_add(&l, 1, sizes[1], false, true, false);
    link_add(&l, 2, sizes[2], true, false, false);
    link_add(&l, 3, sizes[3], true, false, true);
    check_link_complete(&l, "mixed FIN/RST");

    ok(tbt_ms_rx_reset(l.rx, l.s[1].sid), "mid-stream RST observed", NULL);
    ok_u32(tbt_ms_rx_avail(l.rx, l.s[1].sid), 0, "RST stream is empty");
    /* Later DATA for a reset stream is ignored, never resurfacing bytes. */
    memset(&h, 0, sizeof(h));
    h.magic = TBT_MS_MAGIC;
    h.ver = TBT_MS_VER;
    h.type = TBT_MS_DATA;
    h.len = 4;
    h.sid = l.s[1].sid;
    h.off = 0;
    ok_int(tbt_ms_hdr_pack(&h, frame), 0, "late DATA header");
    ok_u32(tbt_ms_rx_feed(l.rx, frame, sizeof(frame)), sizeof(frame),
           "late DATA subframe consumed");
    ok_u32(tbt_ms_rx_avail(l.rx, l.s[1].sid), 0,
           "late DATA is ignored after RST");
    for (i = 0; i < 4; i++)
        if (i != 1)
        {
            u64 end = 0;

            ok(tbt_ms_rx_fin(l.rx, l.s[i].sid, &end),
               "non-reset stream finishes", NULL);
            ok_u64(end, sizes[i], "non-reset FIN offset");
        }
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Broken-leg isolation
 * ------------------------------------------------------------------ */

static int find_frame(const u8 *b, u32 len, u64 sid, u16 nth, u32 *pos)
{
    u32 off = 0;
    u16 seen = 0;

    while (off < len)
    {
        struct tbt_ms_hdr h;
        u32 plen;

        if (len - off < TBT_MS_HDR_LEN)
            return -1;
        if (tbt_ms_hdr_unpack(b + off, &h))
            return -1;
        if (tbt_ms_hdr_check(&h, TBT_MS_DATA_MAX))
            return -1;
        plen = h.type == TBT_MS_DATA ? h.len : 0;
        if (h.sid == sid && h.type == TBT_MS_DATA && ++seen == nth)
        {
            *pos = off;
            return 0;
        }
        off += TBT_MS_HDR_LEN + plen;
    }
    return -1;
}

static void test_broken_isolation(void)
{
    struct tbt_ms_tx *tx;
    struct tbt_ms_rx *rx;
    u64 sa = 0x1000000000ULL + 0x41, sb = 0x1000000000ULL + 0x42;
    u32 asize = 40000, bsize = 20000;
    u8 *a = xalloc(asize), *b = xalloc(bsize);
    u8 *wire;
    u32 total = 0, corrupt_at = 0;
    u32 a_done = 0, b_done = 0, a_before;

    fill_pattern(a, asize, 0xaaaa1u);
    fill_pattern(b, bsize, 0xbbbb2u);
    tx = tbt_ms_tx_new(2, 65536, 65536, 1000);
    rx = tbt_ms_rx_new(2, 65536);
    ok(tx != NULL && rx != NULL, "broken-isolation constructors", NULL);
    if (!tx || !rx)
    {
        free(a);
        free(b);
        return;
    }
    ok_int(tbt_ms_tx_open(tx, sa), 0, "broken-isolation open A");
    ok_int(tbt_ms_tx_open(tx, sb), 0, "broken-isolation open B");
    ok_int(tbt_ms_rx_open(rx, sa), 0, "broken-isolation rx open A");
    ok_int(tbt_ms_rx_open(rx, sb), 0, "broken-isolation rx open B");
    ok_u32(tbt_ms_tx_write(tx, sa, a, asize), asize, "broken-isolation data A");
    ok_u32(tbt_ms_tx_write(tx, sb, b, bsize), bsize, "broken-isolation data B");
    wire = xalloc(asize + bsize + 8192);

    for (;;)
    {
        const u8 *buf = NULL;
        u32 len = 0;
        u32 off = 0;

        ok_int(tbt_ms_tx_block(tx, &buf, &len), 0, "broken-isolation block");
        if (!len)
            break;
        /* The block is a suffix of the shared wire; append it. */
        for (off = 0; off < len; off++)
            wire[total + off] = buf[off];
        total += len;
    }
    ok(total > 0, "broken-isolation wire is nonempty", NULL);
    ok_int(find_frame(wire, total, sa, 2, &corrupt_at), 0,
           "find the second A subframe");
    ok_int(tbt_ms_rx_feed(rx, wire, corrupt_at), corrupt_at,
           "feed the prefix before the corrupt header");
    consume_expected(rx, sa, a, asize, &a_done, "A prefix is intact");
    consume_expected(rx, sb, b, bsize, &b_done, "B prefix is intact");
    a_before = a_done;
    wire[corrupt_at + 28] ^= 0x5a;
    ok_u32(tbt_ms_rx_feed(rx, wire + corrupt_at, total - corrupt_at),
           total - corrupt_at, "feed the corrupt subframe and the rest");
    ok(tbt_ms_rx_broken(rx, sa), "the corrupt subframe breaks only stream A",
       NULL);
    ok(!tbt_ms_rx_broken(rx, sb), "stream B is not broken", NULL);
    consume_expected(rx, sa, a, asize, &a_done,
                     "a broken stream delivers nothing later");
    ok_u32(a_done, a_before, "a broken stream accepts no later payload");
    consume_expected(rx, sb, b, bsize, &b_done, "B bytes stay isolated");
    ok_u32(b_done, bsize, "B delivers every byte despite A's bad header");
    tbt_ms_tx_free(tx);
    tbt_ms_rx_free(rx);
    free(a);
    free(b);
    free(wire);
}

/* ------------------------------------------------------------------ *
 * Receiver edges: auto-create, RST state, bounded legs, delivery walk
 * ------------------------------------------------------------------ */

static u32 make_frame(u8 *out, u8 type, u64 sid, u64 off, const u8 *payload,
                      u32 len)
{
    struct tbt_ms_hdr h;

    memset(&h, 0, sizeof(h));
    h.magic = TBT_MS_MAGIC;
    h.ver = TBT_MS_VER;
    h.type = type;
    h.len = len;
    h.sid = sid;
    h.off = off;
    tbt_ms_hdr_pack(&h, out);
    if (len && payload)
        memcpy(out + TBT_MS_HDR_LEN, payload, len);
    return TBT_MS_HDR_LEN + len;
}

static void test_rx_edges(void)
{
    struct tbt_ms_rx *rx;
    u8 payload[128];
    u8 frame[TBT_MS_HDR_LEN + 128];
    u64 a = 0x1000000000ULL + 0xa1, b = 0x1000000000ULL + 0xa2;
    u64 c = 0x1000000000ULL + 0xa3;
    u32 n;

    fill_pattern(payload, sizeof(payload), 0x7777u);
    rx = tbt_ms_rx_new(2, 16384);
    ok(rx != NULL, "rx constructor", NULL);
    if (!rx)
        return;
    ok(tbt_ms_rx_new(0, 16384) == NULL, "rx rejects 0 streams", NULL);
    ok(tbt_ms_rx_new(TBT_MS_MAX_STREAMS + 1, 16384) == NULL,
       "rx rejects too many streams", NULL);

    n = make_frame(frame, TBT_MS_DATA, a, 0, payload, 100);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "DATA before open is consumed");
    ok_u32(tbt_ms_rx_streams(rx), 0, "an unknown sid does not create a leg");
    ok_u32(tbt_ms_rx_avail(rx, a), 0, "a dropped frame buffers nothing");
    ok_int(tbt_ms_rx_open(rx, a), 0, "explicit open creates the leg");
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "DATA after open");
    ok_u32(tbt_ms_rx_streams(rx), 1, "explicit open owns one leg");
    ok_u32(tbt_ms_rx_avail(rx, a), 100, "the leg buffers payload");
    ok_u64(tbt_ms_rx_delivered(rx, a), 0, "the leg starts at 0");
    {
        const u8 *p = NULL;
        u32 len = 0;

        ok_int(tbt_ms_rx_peek(rx, a, &p, 4096, &len), 0, "rx peek");
        ok_u32(len, 100, "rx peek length");
        ok(memcmp(p, payload, 100) == 0, "rx peek contents", NULL);
        ok_int(tbt_ms_rx_consume(rx, a, 100), 0, "rx consume");
        ok_u64(tbt_ms_rx_delivered(rx, a), 100, "delivered after consume");
    }
    {
        u64 delivered = 0;
        u32 credit = 0;

        ok_int(tbt_ms_rx_win(rx, a, &delivered, &credit, false), 0,
               "a small consumption is not reportable");
        ok_int(tbt_ms_rx_win(rx, a, &delivered, &credit, true), 1,
               "a forced window report is always due");
        ok_u64(delivered, 100, "window report names the consumed offset");
        ok_u32(credit, 16384, "window report names the ring size");
        tbt_ms_rx_win_ack(rx, a, delivered);
        ok_int(tbt_ms_rx_win(rx, a, &delivered, &credit, false), 0,
               "ack clears the pending report");
    }
    n = make_frame(frame, TBT_MS_FIN, a, 100, NULL, 0);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "rx FIN feed");
    {
        u64 end = 0;

        ok(tbt_ms_rx_fin(rx, a, &end), "rx FIN completes", NULL);
        ok_u64(end, 100, "rx FIN end offset");
    }

    n = make_frame(frame, TBT_MS_RST, b, 0, NULL, 0);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "RST before open is consumed");
    ok_u32(tbt_ms_rx_streams(rx), 1, "RST does not create a leg");
    ok_int(tbt_ms_rx_open(rx, b), 0, "explicit open before RST");
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "RST after open");
    ok(tbt_ms_rx_reset(rx, b), "rx reset state", NULL);
    ok_u32(tbt_ms_rx_avail(rx, b), 0, "reset leg is empty");
    n = make_frame(frame, TBT_MS_DATA, b, 0, payload, 50);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "late DATA for a reset leg");
    ok_u32(tbt_ms_rx_avail(rx, b), 0, "late DATA is ignored after RST");
    ok(tbt_ms_rx_reset(rx, b), "late DATA keeps the reset state", NULL);

    n = make_frame(frame, TBT_MS_DATA, c, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "third sid is over the limit");
    ok_u32(tbt_ms_rx_streams(rx), 2, "leg count stays bounded");
    {
        const u8 *p = NULL;
        u32 len = 0;

        ok_int(tbt_ms_rx_peek(rx, c, &p, 0, &len), -ENOENT,
               "an over-limit sid has no leg");
    }

    ok_int(tbt_ms_rx_close(rx, a), 0, "rx close");
    ok_u32(tbt_ms_rx_streams(rx), 1, "rx close drops the leg");
    ok_int(tbt_ms_rx_close(rx, a), -ENOENT, "rx close twice");
    ok_int(tbt_ms_rx_close(rx, 0xdeadbeef), -ENOENT, "rx close unknown");

    /* The delivery walk round-robins bytes and reports a FIN once. */
    tbt_ms_rx_free(rx);
    rx = tbt_ms_rx_new(2, 16384);
    ok(rx != NULL, "rx walk constructor", NULL);
    if (!rx)
        return;
    ok_int(tbt_ms_rx_open(rx, a), 0, "walk open A");
    ok_int(tbt_ms_rx_open(rx, b), 0, "walk open B");
    n = make_frame(frame, TBT_MS_DATA, a, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "walk DATA A");
    n = make_frame(frame, TBT_MS_DATA, b, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "walk DATA B");
    {
        u64 sid = 0;
        u32 first = 0, second = 0;

        ok_int(tbt_ms_rx_next(rx, &sid), 1, "delivery walk first leg");
        first = sid == a ? 1 : 2;
        ok_int(tbt_ms_rx_next(rx, &sid), 1, "delivery walk second leg");
        second = sid == a ? 1 : 2;
        ok(first != second, "delivery walk alternates streams", NULL);
        ok_int(tbt_ms_rx_next(rx, &sid), 1, "delivery walk repeats a leg");
    }
    tbt_ms_rx_free(rx);
}

static void test_rx_ordering(void)
{
    struct tbt_ms_rx *rx;
    u8 payload[16];
    u8 frame[TBT_MS_HDR_LEN + 16];
    u64 a = 0x1000000000ULL + 0xb1, b = 0x1000000000ULL + 0xb2;
    u32 n;

    fill_pattern(payload, sizeof(payload), 0x8888u);
    rx = tbt_ms_rx_new(2, 32);
    ok(rx != NULL, "ordering rx constructor", NULL);
    if (!rx)
        return;
    ok_int(tbt_ms_rx_open(rx, a), 0, "ordering open A");
    ok_int(tbt_ms_rx_open(rx, b), 0, "ordering open B");
    n = make_frame(frame, TBT_MS_DATA, a, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "ordering first A frame");
    n = make_frame(frame, TBT_MS_DATA, b, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "ordering first B frame");
    n = make_frame(frame, TBT_MS_DATA, a, 20, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "ordering out-of-order A frame");
    ok(tbt_ms_rx_broken(rx, a), "an out-of-order frame breaks its leg", NULL);
    ok(!tbt_ms_rx_broken(rx, b), "an out-of-order frame leaves B alone",
       NULL);
    ok_u32(tbt_ms_rx_avail(rx, a), 10, "A keeps the bytes before the gap");
    ok_u32(tbt_ms_rx_avail(rx, b), 10, "B keeps every byte");
    tbt_ms_rx_free(rx);

    rx = tbt_ms_rx_new(1, 16);
    ok(rx != NULL, "full-ring rx constructor", NULL);
    if (!rx)
        return;
    ok_int(tbt_ms_rx_open(rx, a), 0, "full-ring open A");
    n = make_frame(frame, TBT_MS_DATA, a, 0, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "fill the small leg ring");
    n = make_frame(frame, TBT_MS_DATA, a, 10, payload, 10);
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "overflow the small leg ring");
    ok(tbt_ms_rx_broken(rx, a), "a full ring breaks the leg", NULL);
    ok_u32(tbt_ms_rx_avail(rx, a), 10, "a full ring keeps buffered bytes");
    tbt_ms_rx_free(rx);
}

/* A late subframe for a detached stream must not recreate a leg: churn would
 * otherwise fill the fixed leg table with ghosts of closed streams. */
static void test_late_after_close(void)
{
    struct tbt_ms_tx *tx;
    struct tbt_ms_rx *rx;
    u8 payload[16] = {1, 2, 3, 4};
    u8 frame[TBT_MS_HDR_LEN + sizeof(payload)];
    u64 sid = 0x1000000000ULL + 0x77;
    const u8 *buf = NULL;
    u32 len = 0, n;

    tx = tbt_ms_tx_new(2, 65536, 65536, 1000);
    rx = tbt_ms_rx_new(2, 65536);
    ok(tx != NULL && rx != NULL, "late-close constructors", NULL);
    if (!tx || !rx)
        return;
    ok_int(tbt_ms_tx_open(tx, sid), 0, "late-close open");
    ok_u32(tbt_ms_tx_write(tx, sid, payload, sizeof(payload)), sizeof(payload),
           "late-close write");
    ok_int(tbt_ms_tx_block(tx, &buf, &len), 0, "late-close block");
    ok_u32(tbt_ms_rx_feed(rx, buf, len), len, "late-close feed");
    ok_int(tbt_ms_rx_open(rx, sid), 0, "late-close explicit open");
    ok_int(tbt_ms_rx_close(rx, sid), 0, "late-close close");
    ok_u32(tbt_ms_rx_streams(rx), 0, "close removes the leg");
    n = make_frame(frame, TBT_MS_DATA, sid, sizeof(payload), payload,
                   sizeof(payload));
    ok_u32(tbt_ms_rx_feed(rx, frame, n), n, "late frame is consumed");
    ok_u32(tbt_ms_rx_streams(rx), 0, "a late frame does not recreate the leg");
    ok_int(tbt_ms_rx_open(rx, sid), 0, "an explicit reopen is allowed");
    ok_u32(tbt_ms_rx_streams(rx), 1, "explicit reopen creates one leg");
    tbt_ms_tx_free(tx);
    tbt_ms_rx_free(rx);
}

/* ------------------------------------------------------------------ *
 * Sixteen-stream randomized stress
 * ------------------------------------------------------------------ */

static void test_stress16(void)
{
    struct test_link l;
    u16 i;
    u32 seed = 0x1234abcd;

    link_init(&l, 16, 1u << 20, 65536, 8192, seed);
    for (i = 0; i < 16; i++)
    {
        u32 size = 1 + rnd_next(&seed) % 150000;

        link_add(&l, i, size, true, false, (i % 3) == 0);
    }
    ok(pump(&l, 0, 300000), "16-stream randomized stress finishes", NULL);
    for (i = 0; i < 16; i++)
    {
        u64 end = 0;

        ok_u64(l.s[i].delivered, l.s[i].size,
               "stress stream delivered its exact total");
        ok(tbt_ms_rx_fin(l.rx, l.s[i].sid, &end), "stress stream sees FIN",
           NULL);
        ok_u64(end, l.s[i].size, "stress FIN end offset");
    }
    ok_u32(tbt_ms_rx_streams(l.rx), 16, "stress keeps all sixteen legs");
    link_free(&l);
}

/* ------------------------------------------------------------------ *
 * Ring lifecycle: the kernel attach/detach path in allocation-free form
 * ------------------------------------------------------------------ */

static void test_ring_lifecycle(void)
{
    struct tbt_ms_tx *tx;
    struct tbt_ms_tx_ring a, b, c, out;
    const u8 *blk = NULL;
    u32 len = 0;
    u8 payload[64];

    phase = "test_ring_lifecycle";
    memset(payload, 0x5a, sizeof(payload));
    tx = tbt_ms_tx_new(2, 65536, 65536, 8192);
    ok(tx != NULL, "ring constructors", NULL);
    if (!tx)
        return;

    ok_int(tbt_ms_tx_ring_new(NULL, 1, &a), -EINVAL, "ring_new rejects no mux");
    ok_int(tbt_ms_tx_ring_new(tx, 0, &a), -EINVAL, "ring_new rejects sid 0");
    ok_int(tbt_ms_tx_ring_new(tx, 1, NULL), -EINVAL,
           "ring_new rejects no output");
    ok_int(tbt_ms_tx_install(tx, NULL), -EINVAL, "install rejects no ring");
    ok_int(tbt_ms_tx_remove(tx, 1, NULL), -ENOENT,
           "remove rejects no output before it looks");

    ok_int(tbt_ms_tx_ring_new(tx, 1, &a), 0, "ring_new a");
    ok(a.ring != NULL && a.cap == 65536 + 2 * TBT_MS_DATA_MAX,
       "ring_new sizes window plus slack", NULL);
    ok_int(tbt_ms_tx_install(tx, &a), 0, "install a");
    ok_int(tbt_ms_tx_install(tx, &a), -EEXIST, "duplicate install");
    ok_u32(tbt_ms_tx_write(tx, 1, payload, sizeof(payload)), sizeof(payload),
           "write through the installed ring");
    ok_int(tbt_ms_tx_block(tx, &blk, &len), 0, "block with one leg");
    ok(len > 0 && blk != NULL, "block packs the installed ring", NULL);
    ok_int(tbt_ms_tx_remove(tx, 99, &out), -ENOENT, "remove an unknown sid");
    ok_int(tbt_ms_tx_remove(tx, 1, &out), 0, "remove a");
    ok(out.ring == a.ring && out.cap == a.cap && out.sid == 1,
       "remove hands the exact ring back", NULL);
    ok_int(tbt_ms_tx_remove(tx, 1, &out), -ENOENT, "removed sid is gone");
    /* `out` still owns the ring, and `a` aliases it: reinstall, then free
     * once through the second remove so the test never double-frees. */
    ok_int(tbt_ms_tx_install(tx, &a), 0, "reinstall after remove");
    ok_int(tbt_ms_tx_remove(tx, 1, &out), 0, "remove the reinstall");
    tbt_ms_tx_ring_free(&out);

    ok_int(tbt_ms_tx_ring_new(tx, 2, &b), 0, "ring_new b");
    ok_int(tbt_ms_tx_ring_new(tx, 3, &c), 0, "ring_new c");
    ok_int(tbt_ms_tx_install(tx, &b), 0, "install b");
    ok_int(tbt_ms_tx_install(tx, &c), 0, "install c");
    ok_int(tbt_ms_tx_ring_new(tx, 4, &a), 0, "ring_new over capacity");
    ok_int(tbt_ms_tx_install(tx, &a), -ENOSPC, "install refuses a full table");
    tbt_ms_tx_ring_free(&a);
    ok_int(tbt_ms_tx_remove(tx, 2, &out), 0, "remove b");
    tbt_ms_tx_ring_free(&out);
    ok_int(tbt_ms_tx_remove(tx, 3, &out), 0, "remove c");
    tbt_ms_tx_ring_free(&out);
    ok_u32(tbt_ms_tx_queued(tx), 0, "no queued bytes after the removals");
    tbt_ms_tx_free(tx);
}

/* ------------------------------------------------------------------ *
 * Attach/detach concurrency: install and remove rings while another
 * caller packs blocks. The kernel holds tx->ms_lock across these calls;
 * the test models that contract with one mutex.
 * ------------------------------------------------------------------ */

#define RING_THREADS 4
#define RING_ROUNDS 4000

struct ring_stress
{
    struct tbt_ms_tx *tx;
    pthread_mutex_t *lock; /* the kernel's one tx->ms_lock */
    u64 sid;
    int failures;
};

static void *ring_worker(void *arg)
{
    struct ring_stress *s = arg;
    u8 payload[64];
    u32 i;

    memset(payload, (int)(s->sid & 0xff), sizeof(payload));
    for (i = 0; i < RING_ROUNDS; i++)
    {
        struct tbt_ms_tx_ring ring, out;
        const u8 *blk = NULL;
        u32 len = 0;
        int rc;

        if (tbt_ms_tx_ring_new(s->tx, s->sid, &ring))
        {
            s->failures++;
            break;
        }
        pthread_mutex_lock(s->lock);
        rc = tbt_ms_tx_install(s->tx, &ring);
        if (!rc)
        {
            (void)tbt_ms_tx_write(s->tx, s->sid, payload, sizeof(payload));
            (void)tbt_ms_tx_block(s->tx, &blk, &len);
        }
        pthread_mutex_unlock(s->lock);
        if (rc)
        {
            tbt_ms_tx_ring_free(&ring);
            s->failures++;
            break;
        }
        pthread_mutex_lock(s->lock);
        rc = tbt_ms_tx_remove(s->tx, s->sid, &out);
        pthread_mutex_unlock(s->lock);
        if (rc || out.ring != ring.ring || out.cap != ring.cap)
        {
            if (!rc)
                tbt_ms_tx_ring_free(&out);
            else
                tbt_ms_tx_ring_free(&ring);
            s->failures++;
            break;
        }
        tbt_ms_tx_ring_free(&out);
    }
    return NULL;
}

static void test_ring_concurrent(void)
{
    struct tbt_ms_tx *tx;
    struct ring_stress rings[RING_THREADS];
    pthread_t th[RING_THREADS];
    pthread_mutex_t lock;
    int i, spawned = 0, failures = 0;

    phase = "test_ring_concurrent";
    tx = tbt_ms_tx_new(RING_THREADS, 65536, 65536, 8192);
    ok(tx != NULL, "concurrent ring constructors", NULL);
    if (!tx)
        return;
    ok_int(pthread_mutex_init(&lock, NULL), 0, "one mux lock");
    for (i = 0; i < RING_THREADS; i++)
    {
        rings[i].tx = tx;
        rings[i].lock = &lock;
        rings[i].sid = 0x2000000000ULL + (u64)i;
        rings[i].failures = 0;
        if (pthread_create(&th[i], NULL, ring_worker, &rings[i]))
            break;
        spawned++;
    }
    for (i = 0; i < spawned; i++)
    {
        pthread_join(th[i], NULL);
        failures += rings[i].failures;
    }
    pthread_mutex_destroy(&lock);
    ok_int(spawned, RING_THREADS, "all ring workers started");
    ok_int(failures, 0, "no install/remove race lost a ring");
    ok_u32(tbt_ms_tx_queued(tx), 0, "workers leave no queued bytes");
    {
        struct tbt_ms_tx_ring ring, out;

        ok_int(tbt_ms_tx_ring_new(tx, 0x3000000000ULL, &ring), 0,
               "ring_new after the churn");
        ok_int(tbt_ms_tx_install(tx, &ring), 0, "install after the churn");
        ok_int(tbt_ms_tx_remove(tx, 0x3000000000ULL, &out), 0,
               "remove after the churn");
        tbt_ms_tx_ring_free(&out);
    }
    tbt_ms_tx_free(tx);
}

/* ------------------------------------------------------------------ *
 * Admission ledger: the sequences the flow table runs under
 * fec_alloc_lock + sess->lock
 * ------------------------------------------------------------------ */

static void test_admit_ledger(void)
{
    struct tbt_ms_admit a = { 0, 0 };

    phase = "test_admit_ledger";
    ok(tbt_ms_admit_last(&a), "empty ledger is last", NULL);
    tbt_ms_admit_claim(&a);
    ok(a.pending == 1 && a.refs == 0, "claim takes a pending slot", NULL);
    ok(!tbt_ms_admit_last(&a), "a pending slot keeps the plane", NULL);
    tbt_ms_admit_attach(&a);
    ok(a.pending == 0 && a.refs == 1, "attach moves pending to refs", NULL);
    tbt_ms_admit_detach(&a);
    ok(tbt_ms_admit_last(&a), "last detach ends the plane", NULL);
    tbt_ms_admit_abort(&a);
    tbt_ms_admit_detach(&a);
    ok(a.pending == 0 && a.refs == 0,
       "extra abort/detach do not underflow", NULL);

    /* Two carriers admit; one aborts before the other attaches. */
    tbt_ms_admit_claim(&a);
    tbt_ms_admit_claim(&a);
    tbt_ms_admit_abort(&a);
    tbt_ms_admit_attach(&a);
    ok(!tbt_ms_admit_last(&a) && a.refs == 1,
       "abort before attach leaves the plane alive", NULL);
    tbt_ms_admit_detach(&a);
    ok(tbt_ms_admit_last(&a), "plane ends after that leg", NULL);

    /* The lookup winner claims its own slot; the admitting loser aborts. */
    tbt_ms_admit_claim(&a); /* the carrier that admitted the flow */
    tbt_ms_admit_claim(&a); /* the lookup winner's claim_slot */
    tbt_ms_admit_attach(&a); /* the lookup winner attaches */
    tbt_ms_admit_abort(&a); /* the admitting loser gives its slot back */
    ok(!tbt_ms_admit_last(&a) && a.refs == 1,
       "a losing admission cannot end a lookup-attached plane", NULL);
    tbt_ms_admit_detach(&a);
    ok(tbt_ms_admit_last(&a), "lookup-attached plane ends at detach", NULL);

    a.pending = (u16)~0u;
    tbt_ms_admit_claim(&a);
    ok(a.pending == (u16)~0u, "claim saturates instead of wrapping", NULL);
    a.pending = 0;
    a.refs = (u16)~0u;
    tbt_ms_admit_attach(&a);
    ok(a.refs == (u16)~0u, "attach saturates instead of wrapping", NULL);
}

struct admit_stress
{
    struct tbt_ms_admit *a;
    pthread_mutex_t *lock;
    unsigned int rounds;
    int attach;
};

static void *admit_worker(void *arg)
{
    struct admit_stress *s = arg;
    unsigned int i;

    for (i = 0; i < s->rounds; i++)
    {
        pthread_mutex_lock(s->lock);
        tbt_ms_admit_claim(s->a);
        pthread_mutex_unlock(s->lock);
        if (s->attach)
        {
            pthread_mutex_lock(s->lock);
            tbt_ms_admit_attach(s->a);
            pthread_mutex_unlock(s->lock);
            pthread_mutex_lock(s->lock);
            tbt_ms_admit_detach(s->a);
            pthread_mutex_unlock(s->lock);
        }
        else
        {
            pthread_mutex_lock(s->lock);
            tbt_ms_admit_abort(s->a);
            pthread_mutex_unlock(s->lock);
        }
    }
    return NULL;
}

static void test_admit_concurrent(void)
{
    struct tbt_ms_admit a = { 0, 0 };
    struct admit_stress args[4];
    pthread_mutex_t lock;
    pthread_t th[4];
    unsigned int i;
    int spawned = 0;

    phase = "test_admit_concurrent";
    ok_int(pthread_mutex_init(&lock, NULL), 0, "admission stress lock");
    for (i = 0; i < 4; i++)
    {
        args[i].a = &a;
        args[i].lock = &lock;
        args[i].rounds = 20000;
        args[i].attach = (i % 2) != 0;
        if (pthread_create(&th[i], NULL, admit_worker, &args[i]))
            break;
        spawned++;
    }
    for (i = 0; i < (unsigned int)spawned; i++)
        pthread_join(th[i], NULL);
    pthread_mutex_destroy(&lock);
    ok_int(spawned, 4, "all admission workers started");
    ok(tbt_ms_admit_last(&a),
       "claims, attaches and aborts balance under churn", NULL);
}

int main(void)
{
#define RUN(fn)                                                                \
    do                                                                         \
    {                                                                          \
        phase = #fn;                                                           \
        fn();                                                                  \
    } while (0)

    RUN(test_header);
    RUN(test_tx_ring);
    RUN(test_ring_lifecycle);
    RUN(test_ring_concurrent);
    RUN(test_admit_ledger);
    RUN(test_admit_concurrent);
    RUN(test_boundary_frames);
    phase = "test_concurrency(1)";
    test_concurrency(1, 1u << 20);
    phase = "test_concurrency(2)";
    test_concurrency(2, 65536);
    phase = "test_concurrency(4)";
    test_concurrency(4, 1u << 20);
    phase = "test_concurrency(8)";
    test_concurrency(8, 65536);
    phase = "test_concurrency(16)";
    test_concurrency(16, 1u << 20);
    RUN(test_share_slicing);
    RUN(test_backpressure);
    RUN(test_fairness);
    RUN(test_mixed_controls);
    RUN(test_broken_isolation);
    RUN(test_late_after_close);
    RUN(test_rx_edges);
    RUN(test_rx_ordering);
    RUN(test_stress16);

#undef RUN

    printf("test_fec_ms: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
