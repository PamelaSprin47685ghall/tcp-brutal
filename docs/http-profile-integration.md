# `http-kal` carrier integration (C8)

Status: the container is implemented and host-tested in `tbtun/tbt_http.c`; the
trunk owner wires it into `tbt_carrier.c`/`tbt_tx.c`/`tbt_rx.c`. This note is
the interface, not a second design.

## Capture format (frozen)

1. **Text, escaped, line-oriented.** No binary in proc. Each physical line is one
   `<field>=<value>`; header bytes are printed as ASCII with `\r` and `\n`
   escaped as the two-character sequences `\r` (backslash r) and `\n`, and any
   byte outside 0x20..0x7e as `\xNN`. This keeps `cat` readable and makes the
   capture unambiguous.
2. **Contents** (debug only, ≤ 4 KiB total): `req=<escaped request header block
   including the final CRLFCRLF>`, `resp=<escaped response header block>`,
   `first_chunk=<escaped first chunk size line + up to 128 bytes of the first
   record>` — enough to prove chunk framing without dumping payload. Plus
   `req_len=`, `resp_len=` as the unescaped lengths.
3. **Request and response are the near side's carrier 0 only** (one direction
   each per carrier; the far side's response is symmetric and covered by the
   parse tests). State that explicitly so nobody expects a full duplex
   transcript.
4. **Snapshot, not stream**: reading returns the current last-written values;
   never clears. `cat` twice gives the same text.
5. **Harness contract**: `host/test_http_live` accepts this exact text (reads
   `<field>=<value>` lines, unescapes), *or* a raw binary capture file with a
   `TBT-HTTP-CAPTURE 1` magic and `request <len>` / `response <len>` framing for
   offline byte dumps. Implement both; the proc text path is what the QEMU test
   will use.

The proc text order is `req`, `resp`, `first_chunk`, `req_len`, `resp_len`. A
literal backslash in a binary field is emitted as `\x5c` (the harness also
accepts `\\`). `first_chunk` is the first chunk of the request body and does
not include its trailing CRLF.

### Current module state

The module today writes only the raw request header block (`http_last_req`,
≤512 B, debug ≥2): `tbt_show_http()` does `seq_write(m, best->http_last_req,
best_len)`. It does not escape, does not emit `resp`, `first_chunk`, `req_len`
or `resp_len`, and does not store the response. The frozen format above is the
target.

Minimal change list for the module owner (stabilize):

1. Store the near-side carrier 0 request header, response header, and the first
   chunk prefix (size line + up to 128 bytes of the first record) per session.
2. Emit the five fields in any order (the frozen order is `req`, `resp`,
   `first_chunk`, `req_len`, `resp_len`), one physical line each.
3. Escape `\r`, `\n`, and any byte outside 0x20..0x7e as `\xNN`; emit a literal
   backslash as `\x5c`. `req` includes the final CRLFCRLF; `first_chunk` does
   not include its trailing CRLF.
4. `req_len`/`resp_len` are the unescaped lengths.
5. Snapshot semantics: reading returns the current values, never clears; two
   consecutive reads are identical.
6. Keep the file read-only `0400`, debug ≥2, ≤4 KiB total; request/response are
   the near side's carrier 0 only.

### Producing and verifying a capture

In the guest, after a transfer has written carrier 0:

```sh
cat /proc/net/tbtun/http > /dev/kmsg
```

or, when the probe's stdout is the console, simply `cat /proc/net/tbtun/http`.
The file is a snapshot, so read it after the transfer, not during it.

The QEMU harness logs the serial console. Extract the capture lines on the host:

```sh
sed -n '/^req=/,/^resp_len=/p' ~/.cache/tbtun-build/qemu/console.log > capture.txt
```

If the lines came through `/dev/kmsg`, strip the kernel prefix first:

```sh
sed -E 's/^\[[^]]*\] //; s/^<[0-9]+>//' capture.txt > capture.clean.txt
```

Then verify:

```sh
make host-test-live FILE=capture.txt
```

The target runs `host/test_http_live` and, when `uv` plus a cached `h11` are
available, the independent h11 oracle. It is deliberately not part of
`make check` or `make host-test`: it needs a capture.

For a raw binary capture (offline byte dump), the debug writer emits the
`TBT-HTTP-CAPTURE 1` framing. Copy it out as a file (base64 over the console if
there is no shared folder) and run the same target:

```sh
make host-test-live FILE=capture.bin
```

The proc text format is what the QEMU probe will use.

## What is already provided

```c
struct tbt_http_out {
    const struct tbt_hdr *hdr; /* body_len is the body length */
    const u8 *body;            /* hdr->body_len bytes; NULL when 0 */
};

size_t tbt_http_frame_encode(char *buf, size_t cap, const struct tbt_http_out *out);

enum tbt_http_rx_status { TBT_RX_OK = 0, TBT_RX_NEED_MORE, TBT_RX_PROTO };
enum tbt_http_rx_kind { TBT_HTTP_RX_NONE = 0, TBT_HTTP_RX_DRIP, TBT_HTTP_RX_RECORD };

struct tbt_http_rx {
    struct tbt_http_chunk_parser chunk;
    u8 rec[TBT_REC_MAX];
    u32 rec_len;
    u32 rec_need;
    u32 chunk_size;
    u8 chunk_kind;
    bool chunk_started;
};

size_t tbt_http_frame_decode(struct tbt_http_rx *rx, const u8 *buf, size_t len,
                             struct tbt_rec *out, int *err);
void tbt_http_rx_new_message(struct tbt_http_rx *rx);
bool tbt_http_rx_done(const struct tbt_http_rx *rx); /* static inline */

/* Header and timer side, already used by the HTTP profiles. */
size_t tbt_http_req_header(char *buf, size_t cap, const struct tbt_http_cfg *cfg);
size_t tbt_http_resp_header(char *buf, size_t cap, const struct tbt_http_resp_cfg *cfg);
size_t tbt_http_req_parse(const char *buf, size_t len, struct tbt_http_req *out, int *err);
size_t tbt_http_skip_request_header(const u8 *buf, size_t len, int *err);
size_t tbt_http_pad_chunk(char *buf, size_t cap);
size_t tbt_http_drip_chunk(char *buf, size_t cap);
bool tbt_http_need_pad(u64 last_tx_ns, u64 now_ns, u32 keepalive_ms);
bool tbt_http_need_drip(u64 last_tx_ns, u64 now_ns, u32 drip_ms);
```

`TBT_HTTP_FRAME_MAX` (8244) bounds one encoded chunk. `struct tbt_http_rx` is
about 8.3 KiB; put one in `struct tbt_carrier`, do not put it on the stack.

## Exact exposed structs

```c
struct tbt_http_cfg {
    char host[TBT_HTTP_HOST_MAX + 1];        /* "a.b.c.d" or "a.b.c.d:port" */
    char request_id[TBT_HTTP_REQID_LEN + 1]; /* 26 chars [a-z2-7] */
};

struct tbt_http_resp_cfg { u64 unix_secs; };

struct tbt_http_req {
    bool chunked;
    bool keep_alive;
    const char *host;   /* into the parsed buffer, not NUL-terminated */
    size_t host_len;
    size_t header_len;
};

struct tbt_http_resp {
    u16 status;
    bool chunked;
    bool keep_alive;
    bool connection_close;
    bool has_content_length;
    size_t header_len;
};

struct tbt_http_chunk_parser {
    u32 state;          /* enum tbt_http_chunk_state */
    u32 size;
    u32 chunk_size;
    u32 remaining;
    u32 digits;
    u32 line_bytes;
    u32 trailer_bytes;
    u32 chunks;
};

struct tbt_http_msg {
    struct tbt_http_resp resp;
    struct tbt_http_chunk_parser chunk;
    size_t hdr_len;     /* 0 = header stage; >0 = body stage */
};
```

`struct tbt_hdr` (32-byte frame header) and `struct tbt_rec` (header +
body pointer) come from `tbt_proto.h`/`tbt_rec.h`; the carrier already uses
them. `struct tbt_http_rx` and `struct tbt_http_out` are above.

## TX: the one place to wrap

`tbt_carrier_xmit()` is the single carrier write point. Keep the current `raw`
path (`tbt_raw_put_hdr` + iovec) unchanged. For `http-kal`/`http-kal-drip`:

1. Copy `fr->hdr` to a local `struct tbt_hdr h` and set `h.body_len = fr->len`
   (the current raw path sets this in `tbt_raw_put_hdr`; the HTTP encoder reads
   it from the header).
2. Fill `struct tbt_http_out { .hdr = &h, .body = fr->data }`.
3. `n = tbt_http_frame_encode(txbuf, cap, &out)`. `txbuf` should be a
   per-carrier buffer of `TBT_HTTP_FRAME_MAX` (or a kmalloc in the write path);
   do not put 8 KiB on the kernel stack. A 0 return is a bug or a short buffer:
   fail the carrier, never send partial output.
4. `tcp_sendmsg_locked()` one kvec of `n` bytes, as today. One call is one
   complete chunk, so a short send still means "kill this carrier" (the peer
   cannot resynchronise a half-chunk).
5. `fr->hdr.type == TBT_F_PAD` is special-cased by the encoder and produces the
   canonical 38-byte PAD chunk; its other header fields are intentionally not
   carried.

The encoder copies header+body into `txbuf` (the raw path avoids that copy with
iovecs). That is the C8 cost; a future zero-copy version can build the size
line and use three iovecs, but it must keep one chunk contiguous on the wire.

## RX: feed the carrier byte stream

Add `struct tbt_http_rx http_rx` to `struct tbt_carrier` (or one per receive
direction) and branch in the existing drain path (`tbt_car_rx_actor()` ->
`tbt_rx_skb()`):

```c
for (;;) {
    struct tbt_rec rec;
    int err;
    size_t n = tbt_http_frame_decode(&c->http_rx, data + used, len - used, &rec, &err);
    used += n;
    if (err == TBT_RX_OK) {
        /* rec.body points into c->http_rx.rec, valid until the next call. */
        handle_record(c, &rec);   /* the existing tbt_rx_frame path */
        continue;
    }
    if (err == TBT_RX_NEED_MORE) {
        if (tbt_http_rx_done(&c->http_rx)) {
            /* The next bytes are the next POST's request header. */
            skip_or_validate_next_header(...);
            tbt_http_rx_new_message(&c->http_rx);
            continue;
        }
        return used;              /* keep the unconsumed tail */
    }
    /* TBT_RX_PROTO: poison the carrier, never resync. */
    poison_carrier(c);
    return used;
}
```

`tbt_http_frame_decode()` already enforces the framing rules:

- one chunk carries exactly one record: chunk size 32..`TBT_REC_MAX` must equal
  `TBT_HDR_LEN + body_len` for the header it carries;
- two records in one chunk, a record split across two chunks, and a size 2..31
  are `TBT_RX_PROTO`;
- size 1 is drip and is discarded;
- size 32 with a `TBT_F_PAD` header is a PAD record and is yielded;
- PAD and drip may interleave between records;
- it never consumes past the terminating 0-chunk.

The record is returned as soon as its chunk body is complete; the chunk's
trailing CRLF is consumed on the next call. `rec.body` points into
`rx->rec`, so hand it to the existing record handler before the next decode.

### Message boundaries

After `tbt_http_rx_done()`, the next bytes on the request direction are a fresh
request header block. Use `tbt_http_req_parse()` to validate it (preferred) or
`tbt_http_skip_request_header()` to skip it, keep the tail on `NEED_MORE`, then
call `tbt_http_rx_new_message()` and return to the chunk stage. The chunk
parser never consumes the header, so the caller still has those bytes.

## Keepalive and drip

`tbt_http_need_pad(last_tx_ns, now_ns, keepalive_ms)` and
`tbt_http_need_drip(last_tx_ns, now_ns, drip_ms)` are pure arithmetic. Run the
check from the carrier's liveness work at `keepalive_ms/2` (DESIGN §9.7), not
from a busy loop.

- PAD: `tbt_http_pad_chunk()` -> 38 wire bytes (32-byte PAD frame); send it when
  the carrier has been idle for `keepalive_ms` and there is no queued frame.
- Drip: `tbt_http_drip_chunk()` -> 6 wire bytes (1-byte chunk, body `0x23`);
  use it for `http-kal-drip` spares at `drip_ms`.
- Update the per-carrier `last_tx_ns` on every successful write, including PAD
  and drip. The helpers do not read a clock.

## Error policy

`TBT_RX_PROTO`, a short/failed carrier write, or an invalid HTTP header means
the carrier is poisoned: set `stopping`, count it (`rx_parse_proto`,
`carrier_err`), close the socket, and replace from the spare pool
(`tbt_carrier_lost()` / the existing replacement path). Do not attempt to
resynchronise a chunk stream after a framing error; the next byte boundary is
ambiguous by construction.

## Two rules that must not be violated

1. **Never split a record across chunks.** The encoder emits one record per
   chunk; the decoder rejects a peer that splits one. This is a
   security-relevant stance, not an optimisation: it removes the ambiguous
   framing a hostile peer could use to smuggle bytes.
2. **Never put binary in the header block.** The request/response headers are
   ASCII, fixed order, no `tbtun`, no carrier index. The carrier index travels
   only in the HELLO frame body. The frame body is opaque binary inside
   `application/gzip`; that is normal, and the body is not a gzip member.

## State ownership

| State | Lives in |
|---|---|
| chunk parser + record assembly | `struct tbt_http_rx` in `struct tbt_carrier` |
| response header/body stage | `struct tbt_http_msg` (if the response direction is tracked separately) |
| request host/request id | `struct tbt_http_cfg`, per carrier/segment |
| response Date epoch | `struct tbt_http_resp_cfg`, per response |
| `last_tx_ns`, profile, carrier index | `struct tbt_carrier` (the caller's existing fields) |

No pointer into a caller buffer is retained between calls: `out->body` points
into `rx->rec` and is valid only until the next `tbt_http_frame_decode()`.
