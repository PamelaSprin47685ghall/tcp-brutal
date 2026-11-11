/* test_mux.c - host conformance test for the 0.8.0 single-stream mux.
 *
 * Links tbtun/tbt_mux.c, the same object the module builds. No root, no
 * module, no kernel headers, no fixture files: it runs in milliseconds and
 * exits non-zero when a check fails (it keeps going, to list every failure).
 *
 *   cc -std=gnu99 -Wall -Wextra -Werror -I tbtun -o /tmp/test_mux \
 *       host/test_mux.c tbtun/tbt_mux.c
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_mux.h"

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

static void ok_bytes(const u8 *got, const u8 *want, size_t n, const char *what)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            char buf[80];

            snprintf(buf, sizeof(buf), "byte %zu got 0x%02x, want 0x%02x", i,
                     got[i], want[i]);
            ok(0, what, buf);
            return;
        }
    }
    ok(1, what, NULL);
}

static void test_header(void)
{
    struct tbt_mux_hdr h;
    u8 out[9];
    static const u8 data_wire[8] = { 0x02, 0x00, 0x04, 0x00,
                                     0x01, 0x02, 0x03, 0x04 };
    static const u8 open_wire[8] = { 0x01, 0x07, 0x00, 0x00,
                                     0xde, 0xad, 0xbe, 0xef };

    ok_int((int)sizeof(struct tbt_mux_hdr), TBT_MUX_HDR_LEN,
           "struct tbt_mux_hdr is 8 bytes");

    memset(out, 0xa5, sizeof(out));
    memset(&h, 0, sizeof(h));
    h.cmd = TBT_MUX_DATA;
    h.slot = 0;
    h.len = (__be16)0x0400;
    h.stream_id = (__be32)0x01020304;
    ok_int(tbt_mux_hdr_pack(&h, out), 0, "pack DATA returns 0");
    ok_bytes(out, data_wire, sizeof(data_wire), "pack DATA wire bytes");
    ok_int(out[8], 0xa5, "pack writes exactly 8 bytes");

    memset(&h, 0, sizeof(h));
    ok_int(tbt_mux_hdr_unpack(data_wire, &h), 0, "unpack DATA returns 0");
    ok_int(h.cmd, TBT_MUX_DATA, "unpack cmd");
    ok_int(h.slot, 0, "unpack slot");
    ok_int((int)(u16)h.len, 0x0400, "unpack len");
    ok_u32((u32)h.stream_id, 0x01020304u, "unpack stream_id 0x01020304");

    memset(&h, 0, sizeof(h));
    h.cmd = TBT_MUX_OPEN;
    h.slot = 7;
    h.len = 0;
    h.stream_id = (__be32)0xdeadbeefu;
    ok_int(tbt_mux_hdr_pack(&h, out), 0, "pack OPEN returns 0");
    ok_bytes(out, open_wire, sizeof(open_wire), "pack OPEN wire bytes");

    memset(&h, 0, sizeof(h));
    ok_int(tbt_mux_hdr_unpack(open_wire, &h), 0, "unpack OPEN returns 0");
    ok_int(h.cmd, TBT_MUX_OPEN, "unpack OPEN cmd");
    ok_int(h.slot, 7, "unpack OPEN slot");
    ok_int((int)(u16)h.len, 0, "unpack OPEN len");
    ok_u32((u32)h.stream_id, 0xdeadbeefu, "unpack OPEN stream_id");

    ok_int(tbt_mux_hdr_pack(NULL, out), -EINVAL, "pack NULL header refused");
    ok_int(tbt_mux_hdr_unpack(data_wire, NULL), -EINVAL,
           "unpack NULL header refused");
}

static void test_encode(void)
{
    u8 buf[TBT_MUX_FRAME_MAX + 8];
    u8 payload[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    u8 big[TBT_MUX_PAYLOAD_MAX];
    int n;

    memset(big, 0x5a, sizeof(big));

    n = tbt_mux_encode(TBT_MUX_OPEN, 3, 0x01020304, NULL, 0, buf, sizeof(buf));
    {
        static const u8 want[8] = { 0x01, 0x03, 0x00, 0x00,
                                    0x01, 0x02, 0x03, 0x04 };

        ok_int(n, TBT_MUX_HDR_LEN, "encode OPEN length");
        ok_bytes(buf, want, sizeof(want), "encode OPEN wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x01020304, payload, 4, buf,
                       sizeof(buf));
    {
        static const u8 want[12] = { 0x02, 0x00, 0x00, 0x04, 0x01, 0x02,
                                     0x03, 0x04, 0xaa, 0xbb, 0xcc, 0xdd };

        ok_int(n, 12, "encode DATA length");
        ok_bytes(buf, want, sizeof(want), "encode DATA wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_FIN, 0, 0x01020304, NULL, 0, buf, sizeof(buf));
    {
        static const u8 want[8] = { 0x03, 0x00, 0x00, 0x00,
                                    0x01, 0x02, 0x03, 0x04 };

        ok_int(n, TBT_MUX_HDR_LEN, "encode FIN length");
        ok_bytes(buf, want, sizeof(want), "encode FIN wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_RST, 0, 0x01020304, NULL, 0, buf, sizeof(buf));
    {
        static const u8 want[8] = { 0x04, 0x00, 0x00, 0x00,
                                    0x01, 0x02, 0x03, 0x04 };

        ok_int(n, TBT_MUX_HDR_LEN, "encode RST length");
        ok_bytes(buf, want, sizeof(want), "encode RST wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_WIN, 0, 0x01020304, NULL, 0x1234, buf,
                       sizeof(buf));
    {
        static const u8 want[8] = { 0x05, 0x00, 0x12, 0x34,
                                    0x01, 0x02, 0x03, 0x04 };

        ok_int(n, TBT_MUX_HDR_LEN, "encode WIN length");
        ok_bytes(buf, want, sizeof(want), "encode WIN wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_WIN, 0, 0x01020304, NULL, TBT_MUX_WINDOW_MAX,
                       buf, sizeof(buf));
    {
        static const u8 want[8] = { 0x05, 0x00, 0xff, 0xff,
                                    0x01, 0x02, 0x03, 0x04 };

        ok_int(n, TBT_MUX_HDR_LEN, "encode max WIN length");
        ok_bytes(buf, want, sizeof(want), "encode max WIN wire bytes");
    }

    n = tbt_mux_encode(TBT_MUX_DATA, 0, 1, big, TBT_MUX_PAYLOAD_MAX, buf,
                       sizeof(buf));
    ok_int(n, TBT_MUX_FRAME_MAX, "encode max DATA length");
    ok_int((int)buf[2], 0x04, "max DATA len high byte");
    ok_int((int)buf[3], 0x00, "max DATA len low byte");

    ok_int(tbt_mux_encode(TBT_MUX_DATA, 0, 1, payload, 4, buf,
                          TBT_MUX_HDR_LEN + 3),
           -ENOSPC, "encode DATA refuses a short buffer");
    ok_int(tbt_mux_encode(TBT_MUX_DATA, 0, 1, payload, 4, NULL, sizeof(buf)),
           -EINVAL, "encode refuses NULL out");

    ok_int(tbt_mux_encode(TBT_MUX_DATA, 0, 1, payload, 0, buf, sizeof(buf)),
           -EINVAL, "DATA len 0 refused");
    ok_int(tbt_mux_encode(TBT_MUX_DATA, 0, 1, payload,
                          TBT_MUX_PAYLOAD_MAX + 1, buf, sizeof(buf)),
           -EINVAL, "DATA len 1025 refused");
    ok_int(tbt_mux_encode(TBT_MUX_DATA, 1, 1, payload, 4, buf, sizeof(buf)),
           -EINVAL, "DATA slot 1 refused");
    ok_int(tbt_mux_encode(TBT_MUX_DATA, 0, 1, NULL, 4, buf, sizeof(buf)),
           -EINVAL, "DATA without payload refused");
    ok_int(tbt_mux_encode(TBT_MUX_OPEN, 8, 1, NULL, 0, buf, sizeof(buf)),
           -EINVAL, "OPEN slot 8 refused");
    ok_int(tbt_mux_encode(TBT_MUX_OPEN, 0, 1, NULL, 1, buf, sizeof(buf)),
           -EINVAL, "OPEN with len refused");
    ok_int(tbt_mux_encode(TBT_MUX_FIN, 0, 1, NULL, 1, buf, sizeof(buf)),
           -EINVAL, "FIN with len refused");
    ok_int(tbt_mux_encode(TBT_MUX_RST, 1, 1, NULL, 0, buf, sizeof(buf)),
           -EINVAL, "RST with slot refused");
    ok_int(tbt_mux_encode(TBT_MUX_WIN, 1, 1, NULL, 0, buf, sizeof(buf)),
           -EINVAL, "WIN with slot refused");
    ok_int(tbt_mux_encode(0, 0, 1, NULL, 0, buf, sizeof(buf)), -EINVAL,
           "cmd 0 refused");
    ok_int(tbt_mux_encode(6, 0, 1, NULL, 0, buf, sizeof(buf)), -EINVAL,
           "cmd 6 refused");
}

#define CAP_FRAMES 32

struct capture {
    struct tbt_mux_hdr h[CAP_FRAMES];
    u8 payload[CAP_FRAMES][TBT_MUX_PAYLOAD_MAX];
    int n;
    int stop_after;
    int abort_rc;
};

static int capture_cb(void *ctx, const struct tbt_mux_hdr *h,
                      const u8 *payload)
{
    struct capture *c = ctx;
    int idx = c->n;

    if (idx < CAP_FRAMES) {
        c->h[idx] = *h;
        if (h->cmd == TBT_MUX_DATA && payload)
            memcpy(c->payload[idx], payload, (u16)h->len);
    }
    c->n++;
    if (c->stop_after > 0 && c->n >= c->stop_after)
        return c->abort_rc != 0 ? c->abort_rc : -1;
    return 0;
}

static void capture_reset(struct capture *c)
{
    memset(c, 0, sizeof(*c));
}

static size_t feed_chunks(struct tbt_mux_parser *p, const u8 *buf, size_t len,
                          size_t chunk, tbt_mux_frame_cb cb, void *ctx,
                          int *err)
{
    size_t used = 0;

    while (used < len && *err == 0) {
        size_t take = len - used;
        size_t step;

        if (chunk != 0 && take > chunk)
            take = chunk;
        step = tbt_mux_parser_push(p, buf + used, take, cb, ctx, err);
        used += step;
        if (step == 0 && *err == 0)
            break;
    }
    return used;
}

static void test_parser(void)
{
    u8 body[TBT_MUX_PAYLOAD_MAX];
    u8 frame[TBT_MUX_FRAME_MAX];
    u8 wire[4 * TBT_MUX_FRAME_MAX];
    struct tbt_mux_parser p;
    struct capture c;
    u8 payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    int err;
    int n;
    size_t used;
    size_t total = 0;
    int i;

    /* One complete DATA frame in a single push. */
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x01020304, payload, 4, frame,
                       sizeof(frame));
    capture_reset(&c);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = tbt_mux_parser_push(&p, frame, (size_t)n, capture_cb, &c, &err);
    ok_int(err, 0, "whole frame: no error");
    ok_int((int)used, n, "whole frame: all bytes consumed");
    ok_int(c.n, 1, "whole frame: one callback");
    ok_int(c.h[0].cmd, TBT_MUX_DATA, "whole frame: cmd");
    ok_int((int)(u16)c.h[0].len, 4, "whole frame: len");
    ok_u32((u32)c.h[0].stream_id, 0x01020304u, "whole frame: stream_id");
    ok_bytes(c.payload[0], payload, 4, "whole frame: payload");

    /* A DATA body of exactly 1024 bytes fits a 1024-byte body buffer. */
    {
        u8 full[TBT_MUX_PAYLOAD_MAX];

        memset(full, 0x3c, sizeof(full));
        n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x44, full,
                           TBT_MUX_PAYLOAD_MAX, frame, sizeof(frame));
        capture_reset(&c);
        tbt_mux_parser_init(&p, body, sizeof(body));
        err = 0;
        used = tbt_mux_parser_push(&p, frame, (size_t)n, capture_cb, &c,
                                   &err);
        ok_int(err, 0, "max DATA: no error");
        ok_int((int)used, n, "max DATA: all bytes consumed");
        ok_int(c.n, 1, "max DATA: one frame");
        ok_int((int)(u16)c.h[0].len, TBT_MUX_PAYLOAD_MAX, "max DATA: len");
        ok(memcmp(c.payload[0], full, sizeof(full)) == 0,
           "max DATA: payload", NULL);
    }

    /* A zero-body OPEN frame split at every byte. */
    n = tbt_mux_encode(TBT_MUX_OPEN, 5, 0x0a0b0c0d, NULL, 0, frame,
                       sizeof(frame));
    capture_reset(&c);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    for (i = 0; i < n; i++) {
        used = tbt_mux_parser_push(&p, frame + i, 1, capture_cb, &c, &err);
        ok_int((int)used, 1, "OPEN byte-wise: one byte consumed");
        ok_int(c.n, i + 1 == n ? 1 : 0, "OPEN byte-wise: delivery point");
    }
    ok_int(err, 0, "OPEN byte-wise: no error");
    ok_int(c.n, 1, "OPEN byte-wise: one callback");
    ok_int(c.h[0].slot, 5, "OPEN byte-wise: slot");
    ok_u32((u32)c.h[0].stream_id, 0x0a0b0c0du, "OPEN byte-wise: stream_id");

    /* A DATA frame split 3+5 in the header and 2+2 in the body. */
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x01020304, payload, 4, frame,
                       sizeof(frame));
    capture_reset(&c);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = tbt_mux_parser_push(&p, frame, 3, capture_cb, &c, &err);
    ok_int((int)used, 3, "mixed split: header part 1 consumed");
    ok_int(c.n, 0, "mixed split: no frame yet");
    used = tbt_mux_parser_push(&p, frame + 3, 5, capture_cb, &c, &err);
    ok_int((int)used, 5, "mixed split: header part 2 consumed");
    ok_int(c.n, 0, "mixed split: body still open");
    used = tbt_mux_parser_push(&p, frame + 8, 2, capture_cb, &c, &err);
    ok_int((int)used, 2, "mixed split: body part 1 consumed");
    ok_int(c.n, 0, "mixed split: frame still open");
    used = tbt_mux_parser_push(&p, frame + 10, 2, capture_cb, &c, &err);
    ok_int((int)used, 2, "mixed split: body part 2 consumed");
    ok_int(c.n, 1, "mixed split: frame delivered");
    ok_bytes(c.payload[0], payload, 4, "mixed split: payload");

    /* Three frames in one push, then a partial fourth header. */
    total = 0;
    n = tbt_mux_encode(TBT_MUX_OPEN, 1, 0x11111111, NULL, 0, wire + total,
                       sizeof(wire) - total);
    total += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x11111111, payload, 4, wire + total,
                       sizeof(wire) - total);
    total += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_FIN, 0, 0x11111111, NULL, 0, wire + total,
                       sizeof(wire) - total);
    total += (size_t)n;

    capture_reset(&c);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = tbt_mux_parser_push(&p, wire, total, capture_cb, &c, &err);
    ok_int(err, 0, "three frames: no error");
    ok_int((int)used, (int)total, "three frames: all bytes consumed");
    ok_int(c.n, 3, "three frames: three callbacks");
    ok_int(c.h[0].cmd, TBT_MUX_OPEN, "three frames: frame 0 cmd");
    ok_int(c.h[1].cmd, TBT_MUX_DATA, "three frames: frame 1 cmd");
    ok_int(c.h[2].cmd, TBT_MUX_FIN, "three frames: frame 2 cmd");

    /* Header split across pushes, then the next frame's first bytes. */
    {
        u8 two[2 * TBT_MUX_FRAME_MAX];
        size_t two_len = 0;

        n = tbt_mux_encode(TBT_MUX_FIN, 0, 1, NULL, 0, two, sizeof(two));
        two_len += (size_t)n;
        n = tbt_mux_encode(TBT_MUX_DATA, 0, 2, payload, 4, two + two_len,
                           sizeof(two) - two_len);
        two_len += (size_t)n;

        capture_reset(&c);
        tbt_mux_parser_init(&p, body, sizeof(body));
        err = 0;
        used = tbt_mux_parser_push(&p, two, two_len - 2, capture_cb, &c,
                                   &err);
        ok_int((int)used, (int)(two_len - 2),
               "trailing header: partial bytes consumed");
        ok_int(c.n, 1, "trailing header: only the FIN delivered");
        used = tbt_mux_parser_push(&p, two + two_len - 2, 2, capture_cb, &c,
                                   &err);
        ok_int((int)used, 2, "trailing header: tail consumed");
        ok_int(c.n, 2, "trailing header: DATA delivered");
        ok_bytes(c.payload[1], payload, 4, "trailing header: DATA payload");
    }

    /* Malformed headers stop the parser with -EPROTO. */
    {
        static const u8 bad_cmd[8] = { 0x09, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x01 };
        static const u8 data_zero[8] = { 0x02, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x01 };
        static const u8 data_1025[8] = { 0x02, 0x00, 0x04, 0x01,
                                         0x00, 0x00, 0x00, 0x01 };
        static const u8 data_slot[8] = { 0x02, 0x01, 0x00, 0x04,
                                         0x00, 0x00, 0x00, 0x01 };
        static const u8 open_slot[8] = { 0x01, 0x08, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x01 };
        static const u8 open_len[8] = { 0x01, 0x00, 0x00, 0x01,
                                        0x00, 0x00, 0x00, 0x01 };
        static const u8 fin_len[8] = { 0x03, 0x00, 0x00, 0x01,
                                       0x00, 0x00, 0x00, 0x01 };
        static const u8 win_slot[8] = { 0x05, 0x01, 0x00, 0x01,
                                        0x00, 0x00, 0x00, 0x01 };

        static const struct {
            const u8 *wire;
            const char *what;
        } cases[] = {
            { bad_cmd, "bad cmd is -EPROTO" },
            { data_zero, "DATA len 0 is -EPROTO" },
            { data_1025, "DATA len 1025 is -EPROTO" },
            { data_slot, "DATA slot 1 is -EPROTO" },
            { open_slot, "OPEN slot 8 is -EPROTO" },
            { open_len, "OPEN with len is -EPROTO" },
            { fin_len, "FIN with len is -EPROTO" },
            { win_slot, "WIN with slot is -EPROTO" },
        };

        for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
            capture_reset(&c);
            tbt_mux_parser_init(&p, body, sizeof(body));
            err = 0;
            used = tbt_mux_parser_push(&p, cases[i].wire, 8, capture_cb, &c,
                                       &err);
            ok_int(err, -EPROTO, cases[i].what);
            ok_int((int)used, 8, "malformed header consumes its 8 bytes");
            ok_int(c.n, 0, "malformed header delivers no frame");
        }

        /* After a protocol error, a caller that clears *err starts at the
         * next byte and never sees the rejected header delivered. */
        capture_reset(&c);
        tbt_mux_parser_init(&p, body, sizeof(body));
        err = 0;
        used = tbt_mux_parser_push(&p, bad_cmd, 8, capture_cb, &c, &err);
        ok_int(err, -EPROTO, "recovery: bad header rejected");
        err = 0;
        n = tbt_mux_encode(TBT_MUX_FIN, 0, 0x33, NULL, 0, frame,
                           sizeof(frame));
        used = tbt_mux_parser_push(&p, frame, (size_t)n, capture_cb, &c,
                                   &err);
        ok_int(err, 0, "recovery: good frame parses");
        ok_int((int)used, n, "recovery: good frame consumed");
        ok_int(c.n, 1, "recovery: only the good frame is delivered");
        ok_int(c.h[0].cmd, TBT_MUX_FIN, "recovery: delivered cmd");
        ok_u32((u32)c.h[0].stream_id, 0x33, "recovery: delivered stream_id");
    }

    /* DATA body larger than the caller buffer is refused at the header. */
    {
        u8 nine[9];

        memset(nine, 0x5a, sizeof(nine));
        n = tbt_mux_encode(TBT_MUX_DATA, 0, 1, nine, 9, frame, sizeof(frame));
    }
    capture_reset(&c);
    tbt_mux_parser_init(&p, body, 8);
    err = 0;
    used = tbt_mux_parser_push(&p, frame, (size_t)n, capture_cb, &c, &err);
    ok_int(err, -EPROTO, "DATA body over body_cap is -EPROTO");
    ok_int((int)used, 8, "oversized DATA body stops at the header");
    ok_int(c.n, 0, "oversized DATA body delivers no frame");

    /* A DATA body with no caller storage is refused too. */
    capture_reset(&c);
    tbt_mux_parser_init(&p, NULL, 0);
    err = 0;
    used = tbt_mux_parser_push(&p, frame, (size_t)n, capture_cb, &c, &err);
    ok_int(err, -EPROTO, "DATA without body storage is -EPROTO");
    ok_int((int)used, 8, "DATA without body storage stops at the header");

    /* A non-zero callback return stops the parser and is reported as *err. */
    total = 0;
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x22, payload, 2, wire + total,
                       sizeof(wire) - total);
    total += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_FIN, 0, 0x22, NULL, 0, wire + total,
                       sizeof(wire) - total);
    total += (size_t)n;

    capture_reset(&c);
    c.stop_after = 1;
    c.abort_rc = -42;
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = tbt_mux_parser_push(&p, wire, total, capture_cb, &c, &err);
    ok_int(err, -42, "callback abort becomes *err");
    ok_int((int)used, 10, "callback abort consumes through the frame");
    ok_int(c.n, 1, "callback abort stops after one frame");

    err = 0;
    c.stop_after = 0;
    used = tbt_mux_parser_push(&p, wire + 10, total - 10, capture_cb, &c,
                               &err);
    ok_int(err, 0, "parser resumes after a cleared abort");
    ok_int((int)used, 8, "resume consumes the next frame");
    ok_int(c.n, 2, "resume delivers the next frame");
    ok_int(c.h[1].cmd, TBT_MUX_FIN, "resume delivers FIN");

    /* A pre-set error stops the parser without consuming input. */
    capture_reset(&c);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = -5;
    used = tbt_mux_parser_push(&p, wire, total, capture_cb, &c, &err);
    ok_int((int)used, 0, "pre-set *err consumes nothing");
    ok_int(err, -5, "pre-set *err is preserved");
    ok_int(c.n, 0, "pre-set *err delivers no frame");
}

static void test_flows(void)
{
    struct tbt_mux_flows f;
    struct tbt_mux_stream *s;
    u16 grant;
    unsigned int i;

    tbt_mux_flows_init(&f, 1024);
    ok_u32(f.rx_window, 1024, "flows init keeps rx_window");
    ok_int(f.count, 0, "flows init count is 0");

    s = tbt_mux_flow_open(&f, 0x100, 2);
    ok(s != NULL, "open returns a stream", NULL);
    if (s) {
        ok_int(s->state, TBT_MUX_OPEN, "open state");
        ok_int(s->slot, 2, "open slot");
        ok_u32(s->tx_credit, 0, "open tx_credit");
        ok_u32(s->rx_unacked, 0, "open rx_unacked");
        ok_u32(s->stream_id, 0x100, "open stream_id");
    }
    ok_int(f.count, 1, "open count");
    ok(tbt_mux_flow_open(&f, 0x100, 3) == NULL, "duplicate OPEN returns NULL",
       NULL);
    ok_int(f.count, 1, "duplicate OPEN keeps count");
    ok(tbt_mux_flow_open(&f, 0x101, 8) == NULL, "OPEN slot 8 returns NULL",
       NULL);

    for (i = 1; i < TBT_MUX_STREAMS; i++) {
        s = tbt_mux_flow_open(&f, 0x100 + i, (u8)(i % 8));
        ok(s != NULL, "table fill open", NULL);
    }
    ok_int(f.count, TBT_MUX_STREAMS, "table full count");
    ok(tbt_mux_flow_open(&f, 0x999, 0) == NULL, "full table returns NULL",
       NULL);
    ok_int(f.count, TBT_MUX_STREAMS, "failed open keeps count");

    s = tbt_mux_flow_find(&f, 0x100);
    ok(s != NULL, "find returns the stream", NULL);
    ok_int(tbt_mux_tx_charge(&f, 0x100, 1), -EAGAIN,
           "zero credit refuses 1 byte");
    ok_int(tbt_mux_tx_credit(&f, 0x100, 100), 0, "grant 100");
    ok_u32(s ? s->tx_credit : 0, 100, "credit after grant");
    ok_int(tbt_mux_tx_charge(&f, 0x100, 101), -EAGAIN,
           "charge over credit is -EAGAIN");
    ok_u32(s ? s->tx_credit : 0, 100, "failed charge keeps credit");
    ok_int(tbt_mux_tx_charge(&f, 0x100, 100), 0, "charge exact credit");
    ok_u32(s ? s->tx_credit : 0, 0, "credit after exact charge");
    ok_int(tbt_mux_tx_charge(&f, 0x100, 0), 0, "zero charge succeeds");

    if (s) {
        s->tx_credit = 0xffffffffu - 0xffffu;
        ok_int(tbt_mux_tx_credit(&f, 0x100, 0xffff), 0,
               "grant up to exactly u32 max");
        ok_u32(s->tx_credit, 0xffffffffu, "exact u32 max is not clamped");
        s->tx_credit = 0xffffffffu - 0xfffeu;
        ok_int(tbt_mux_tx_credit(&f, 0x100, 0xffff), 0,
               "grant past u32 max");
        ok_u32(s->tx_credit, 0xffffffffu, "credit saturates at u32 max");
    }

    ok_int(tbt_mux_tx_charge(&f, 0x777, 1), -ENOENT,
           "charge unknown stream is -ENOENT");
    ok_int(tbt_mux_tx_credit(&f, 0x777, 1), -ENOENT,
           "credit unknown stream is -ENOENT");

    grant = 0xffff;
    ok_int(tbt_mux_rx_charge(&f, 0x101, 512, &grant), 0,
           "half window does not grant");
    ok_u32(grant, 0, "no grant stores 0");
    s = tbt_mux_flow_find(&f, 0x101);
    ok_u32(s ? s->rx_unacked : 0, 512, "rx_unacked after half window");
    ok_int(tbt_mux_rx_charge(&f, 0x101, 1, &grant), 1,
           "one byte past half window grants");
    ok_u32(grant, 513, "grant is the whole unacked sum");
    ok_u32(s ? s->rx_unacked : 0, 0, "rx_unacked clears after grant");
    ok_int(tbt_mux_rx_charge(&f, 0x101, 1, NULL), -EINVAL,
           "NULL grant is -EINVAL");
    ok_int(tbt_mux_rx_charge(&f, 0x777, 1, &grant), -ENOENT,
           "rx charge unknown stream is -ENOENT");

    ok_int(tbt_mux_flow_close(&f, 0x100), 0, "close returns 0");
    ok(tbt_mux_flow_find(&f, 0x100) == NULL, "closed stream is not found",
       NULL);
    ok_int(f.count, TBT_MUX_STREAMS - 1, "close decrements count");
    ok_int(tbt_mux_flow_close(&f, 0x100), -ENOENT, "double close is -ENOENT");

    s = tbt_mux_flow_open(&f, 0x100, 0);
    ok(s != NULL, "reopen after close", NULL);
    ok_int(f.count, TBT_MUX_STREAMS, "reopen restores count");

    {
        struct tbt_mux_flows g;

        tbt_mux_flows_init(&g, 0);
        ok_u32(g.rx_window, TBT_MUX_WINDOW_MAX,
               "rx_window 0 selects the default");
    }
}

static void same_capture(const struct capture *a, const struct capture *b,
                         const char *what)
{
    int i;

    ok_int(a->n, b->n, what);
    if (a->n != b->n)
        return;
    for (i = 0; i < a->n && i < CAP_FRAMES; i++) {
        char detail[64];
        int same;

        snprintf(detail, sizeof(detail), "frame %d header", i);
        same = a->h[i].cmd == b->h[i].cmd && a->h[i].slot == b->h[i].slot &&
               (u16)a->h[i].len == (u16)b->h[i].len &&
               (u32)a->h[i].stream_id == (u32)b->h[i].stream_id;
        ok(same, what, detail);
        if (a->h[i].cmd == TBT_MUX_DATA) {
            snprintf(detail, sizeof(detail), "frame %d payload", i);
            ok(memcmp(a->payload[i], b->payload[i], (u16)a->h[i].len) == 0,
               what, detail);
        }
    }
}

static void test_end_to_end(void)
{
    static struct capture whole;
    static struct capture split;
    static const u8 msg[3][20] = {
        { 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A',
          'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A', 'A' },
        { 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B',
          'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B', 'B' },
        { 'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C',
          'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C', 'C' },
    };
    static const u32 id[3] = { 0x0101, 0x0202, 0x0303 };
    static const u8 slot[3] = { 1, 2, 3 };
    static const size_t chunk[3] = { 7, 7, 6 };
    u8 wire[4096];
    u8 body[TBT_MUX_PAYLOAD_MAX];
    u8 out[3][20];
    size_t off[3] = { 0, 0, 0 };
    size_t out_len[3] = { 0, 0, 0 };
    size_t total = 0;
    size_t used;
    struct tbt_mux_parser p;
    int err;
    int n;
    int i;
    int k;

    for (i = 0; i < 3; i++) {
        n = tbt_mux_encode(TBT_MUX_OPEN, slot[i], id[i], NULL, 0,
                           wire + total, sizeof(wire) - total);
        ok(n > 0, "e2e: encode OPEN", NULL);
        total += (size_t)n;
    }

    for (k = 0; k < 3; k++) {
        for (i = 0; i < 3; i++) {
            n = tbt_mux_encode(TBT_MUX_DATA, 0, id[i], msg[i] + off[i],
                               (u16)chunk[k], wire + total,
                               sizeof(wire) - total);
            ok(n > 0, "e2e: encode DATA", NULL);
            total += (size_t)n;
            off[i] += chunk[k];
        }
    }

    for (i = 0; i < 3; i++) {
        n = tbt_mux_encode(TBT_MUX_FIN, 0, id[i], NULL, 0, wire + total,
                           sizeof(wire) - total);
        ok(n > 0, "e2e: encode FIN", NULL);
        total += (size_t)n;
    }

    capture_reset(&whole);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = tbt_mux_parser_push(&p, wire, total, capture_cb, &whole, &err);
    ok_int(err, 0, "e2e whole: no error");
    ok_int((int)used, (int)total, "e2e whole: all bytes consumed");
    ok_int(whole.n, 15, "e2e whole: 15 frames");

    capture_reset(&split);
    tbt_mux_parser_init(&p, body, sizeof(body));
    err = 0;
    used = feed_chunks(&p, wire, total, 1, capture_cb, &split, &err);
    ok_int(err, 0, "e2e byte-wise: no error");
    ok_int((int)used, (int)total, "e2e byte-wise: all bytes consumed");
    ok_int(split.n, 15, "e2e byte-wise: 15 frames");
    same_capture(&whole, &split, "e2e: byte-wise matches whole input");

    for (i = 0; i < whole.n && i < CAP_FRAMES; i++) {
        if (whole.h[i].cmd != TBT_MUX_DATA)
            continue;
        for (k = 0; k < 3; k++) {
            if ((u32)whole.h[i].stream_id != id[k])
                continue;
            if (out_len[k] + (u16)whole.h[i].len <= sizeof(out[k])) {
                memcpy(out[k] + out_len[k], whole.payload[i],
                       (u16)whole.h[i].len);
                out_len[k] += (u16)whole.h[i].len;
            }
            break;
        }
    }

    for (i = 0; i < 3; i++) {
        char detail[64];

        snprintf(detail, sizeof(detail), "stream %d length", i);
        ok_int((int)out_len[i], 20, "e2e: reconstructed length");
        snprintf(detail, sizeof(detail), "stream %d bytes", i);
        ok(memcmp(out[i], msg[i], 20) == 0, "e2e: reconstructed bytes",
           detail);
    }

    ok_int(whole.h[0].cmd, TBT_MUX_OPEN, "e2e: first frame is OPEN");
    ok_int(whole.h[0].slot, 1, "e2e: first OPEN slot");
    ok_u32((u32)whole.h[0].stream_id, id[0], "e2e: first OPEN stream");
    ok_int(whole.h[12].cmd, TBT_MUX_FIN, "e2e: frame 12 is FIN");
}

int main(void)
{
    test_header();
    test_encode();
    test_parser();
    test_flows();
    test_end_to_end();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
