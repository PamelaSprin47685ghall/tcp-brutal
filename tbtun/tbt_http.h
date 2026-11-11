/* tbt_http.h - the HTTP/1.1 QoS carrier container (DESIGN.md §3).
 *
 * Pure code: no kernel headers, no allocation, no static mutable state, no
 * timers. It compiles into the module and into host/test_http.c unchanged, so
 * a rule that passes the host test is the rule the module runs.
 *
 * What this unit owns:
 *   - the request header block of §3.2 (fixed order, canonical casing, no
 *     binary, no "tbtun" anywhere), the strict parser for that block, and a
 *     bounded skip-only scan for the far side's header/body stage change;
 *   - the response header block of §3.2 and the parser for it (§9.5);
 *   - the IMF-fixdate formatter the response header uses;
 *   - chunked transfer coding in both directions (§3.2 rule 5, §3.3): the
 *     size line, the CRLFs, the terminating 0-chunk and its trailer section;
 *   - the keep-alive PAD chunk and the drip chunk (§3.1.1, §3.7);
 *   - the one-chunk-per-record encoder and the record assembler the carrier
 *     uses on TX and RX (C8);
 *   - the per-message receiver bookkeeping (header stage, chunk stage, reset);
 *   - the cadence arithmetic for both, with no clock of its own.
 *
 * What it does not own: when to call any of this. tbt_carrier.c decides when a
 * segment starts, which carrier sends it, and what a decoded body means.
 *
 * Contract for every function below: it reads and writes only inside the
 * buffer and capacity it is given, never reads a byte past `len`, and has no
 * failure mode other than the documented return value. A parser reports
 * TBT_HTTP_NEED_MORE instead of guessing, so a caller can retry with more
 * bytes. The parser state itself holds no pointer; the slices it returns
 * (out_body, out->host) point into the caller's buffer and stay valid only
 * while that buffer does.
 */
#ifndef _TBT_HTTP_H
#define _TBT_HTTP_H

#include "tbt_rec.h"

/* The request shape of DESIGN.md §3.2. The path and the User-Agent are fixed
 * on purpose: §16.2 lists them as classifier inputs, and a value that varies
 * per carrier would be a signature of its own. */
#define TBT_HTTP_PATH "/api/v2/upload"
/* The body is a tbtun FEC record, not a gzip member: the value only has to be
 * a standard media type that a classifier expects to see on a long upload, and
 * one that a middlebox will not try to decode. No Content-Encoding is sent and
 * nothing is compressed. */
#define TBT_HTTP_CONTENT_TYPE "application/gzip"
#define TBT_HTTP_USER_AGENT                                                                    \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0 "     \
    "Safari/537.36"

/* Request header block bounds. TBT_HTTP_REQ_MAX is a compile-time ceiling so a
 * kernel caller can put the block on the stack; the builder returns 0 rather
 * than truncate, and the actual block is ~340 bytes. */
#define TBT_HTTP_REQ_MAX 4096 /* covers TBT_HTTP_HDR_MAX; requests are ~380 B */
#define TBT_HTTP_HOST_MAX 64  /* IPv4 or [IPv6] literal plus an optional ":port" */
#define TBT_HTTP_REQID_LEN 26 /* DESIGN §3.2: 26 lowercase base32 characters */

/* Response header block bound. The far side emits ~150 bytes; the bound only
 * exists so a hostile peer cannot make the parser scan forever. */
#define TBT_HTTP_HDR_MAX 4096

/* Response header block bound. The emitted block is 163 bytes; the ceiling is
 * for the stack buffer a caller allocates. */
#define TBT_HTTP_RESP_MAX 256

/* IMF-fixdate is exactly 29 bytes: "Thu, 01 Jan 1970 00:00:00 GMT". */
#define TBT_HTTP_DATE_LEN 29

/* Chunk coding bounds. TBT_CHUNK_MAX (tbt_uapi.h) is the chunk-data ceiling
 * of §3.3; a full record is TBT_REC_MAX. A size line is at most 16 hex digits
 * + CRLF; a decoder gives up on a size/extension line longer than this. */
#define TBT_HTTP_CHUNK_HDR_MAX 18
#define TBT_HTTP_CHUNK_LINE_MAX 1024
/* Worst-case encoded chunk: size line + one full record + CRLF. */
#define TBT_HTTP_FRAME_MAX (TBT_HTTP_CHUNK_HDR_MAX + TBT_REC_MAX + 2) /* 8244 */
/* Largest chunk-data body the size-line encoder accepts. A non-FEC session
 * still sends TBT_REC_MAX records, so the ceiling is the larger of the two
 * record formats; tbt_http_fec_frame() separately enforces TBT_FEC_REC_MAX. */
#define TBT_HTTP_CHUNK_DATA_MAX     (TBT_FEC_REC_MAX > TBT_CHUNK_MAX ? TBT_FEC_REC_MAX : TBT_CHUNK_MAX)
/* Same bound for a 40-byte FEC record. */
#define TBT_HTTP_FEC_FRAME_MAX (TBT_HTTP_CHUNK_HDR_MAX + TBT_FEC_REC_MAX + 2)
/* A FEC session never puts a DATA frame on a carrier: tbt_tx_submit() hands
 * every DATA byte to the FEC packetizer, and only control records use the
 * HTTP encoder. The largest control record is a full FBK (TBT_FBK_MAX body);
 * WINDOW and HELLO are smaller. A non-FEC session still needs
 * TBT_HTTP_FRAME_MAX, because its DATA record carries TBT_PAYLOAD_MAX. */
#define TBT_HTTP_CTL_FRAME_MAX \
    (TBT_HTTP_CHUNK_HDR_MAX + TBT_HDR_LEN + TBT_FBK_MAX + 2)

/* /proc/net/tbtun/http keeps the first chunk's size line and this many bytes
 * of its record: enough to prove the framing without dumping payload
 * (docs/http-profile-integration.md, "Capture format"). */
#define TBT_HTTP_CAP_CHUNK_DATA 128
#define TBT_HTTP_CAP_CHUNK (TBT_HTTP_CHUNK_HDR_MAX + 2 + TBT_HTTP_CAP_CHUNK_DATA)

/* Derived wire lengths.
 *   PAD chunk: "20\r\n" (4) + 32-byte PAD frame + CRLF = 38 bytes.
 *   drip chunk: "1\r\n" + one filler byte + CRLF = 6 bytes.
 *   terminator: "0\r\n" + CRLF = 5 bytes. */
#define TBT_HTTP_PAD_DATA_LEN TBT_HDR_LEN                      /* 32 */
#define TBT_HTTP_PAD_CHUNK_LEN (4 + TBT_HTTP_PAD_DATA_LEN + 2) /* 38 */
#define TBT_HTTP_DRIP_CHUNK_LEN 6
#define TBT_HTTP_TERM_LEN 5

/* Request inputs. The caller fills both fields; the builder validates them and
 * emits them verbatim, so a hostname, an IPv6 literal, or a request id with a
 * non-base32 byte can never reach the wire.
 *
 * host       IPv4 dotted quad, optionally ":port" (e.g. "203.0.113.9" or
 *            "203.0.113.9:8443"), NUL-terminated. §3.2 says the Host header
 *            must match the connection target; include the port whenever the
 *            target has a non-default one.
 * request_id exactly 26 characters from [a-z2-7] (lowercase RFC 4648 base32),
 *            NUL-terminated. The caller generates it; this unit has no random
 *            source and must stay pure. */
struct tbt_http_cfg
{
    char host[TBT_HTTP_HOST_MAX + 1];
    char request_id[TBT_HTTP_REQID_LEN + 1];
};

/* Response inputs. The far side emits one block per segment; the only field
 * that varies is the UTC time, supplied as epoch seconds so this unit needs no
 * clock, no timezone and no locale. */
struct tbt_http_resp_cfg
{
    u64 unix_secs;
};

/* Response view. Filled only when tbt_http_resp_parse() returns OK. */
struct tbt_http_resp
{
    u16 status;              /* always 200: other codes are TBT_HTTP_PROTO */
    bool chunked;            /* Transfer-Encoding: chunked was present */
    bool keep_alive;         /* HTTP/1.1 default, unless Connection: close */
    bool connection_close;   /* Connection: close was present */
    bool has_content_length; /* parser bookkeeping; TE + CL is rejected */
    size_t header_len;       /* status line + headers + final CRLF */
};

/* Every parser result. TBT_HTTP_OK is success, TBT_HTTP_NEED_MORE means the
 * input ended in the middle of a construct and the caller should append more
 * and retry, TBT_HTTP_PROTO is a permanent protocol error. */
enum tbt_http_status
{
    TBT_HTTP_OK = 0,
    TBT_HTTP_NEED_MORE,
    TBT_HTTP_PROTO,
};

/* Emit the request header block of DESIGN.md §3.2, including the final blank
 * line: "POST /api/v2/upload HTTP/1.1", then exactly these headers in this
 * order -- Host, User-Agent, Accept, Accept-Encoding, Content-Type,
 * Transfer-Encoding, Connection, X-Request-Id -- then the blank line.
 *
 * Returns the number of bytes written (no NUL is appended), or 0 if cfg->host
 * or cfg->request_id is invalid. A short `cap` is always 0, never a truncated
 * header: partial output must never reach the wire.
 *
 * The block identifies nothing about the carrier: no tbtun/carrier/index
 * header, no X-TBTUN-*, no per-carrier value. The carrier index travels only
 * inside the HELLO frame body (§3.3), never in HTTP. */
size_t tbt_http_req_header(char *buf, size_t cap, const struct tbt_http_cfg *cfg);

/* Request view. Filled only when tbt_http_req_parse() returns OK.
 *
 * host points into the caller's buffer at the Host value (not NUL-terminated);
 * host_len is its length. The far side can compare it with its own address
 * literal if it wants the §3.2 "matches the connection target" check; this
 * parser validates only that it is a canonical IPv4 literal with an optional
 * port. */
struct tbt_http_req
{
    bool chunked;    /* true on OK: Transfer-Encoding: chunked was present */
    bool keep_alive; /* true on OK: Connection: keep-alive was present */
    const char *host;
    size_t host_len;
    size_t header_len; /* request line + headers + final CRLF */
};

/* Parse the request header block the near side emits.
 *
 * Stateless and resumable by re-scan, like tbt_http_resp_parse(): on
 * TBT_HTTP_NEED_MORE the caller keeps the bytes and calls again with a longer
 * prefix; on OK it returns the header length and fills *out; on NEED_MORE or
 * PROTO it returns 0 and does not write *out.
 *
 * Strict on purpose. The far side accepts exactly this shape and nothing else:
 *   POST /api/v2/upload HTTP/1.1
 *   Host: <canonical IPv4 literal, optional :port>
 *   User-Agent: <TBT_HTTP_USER_AGENT>
 *   Accept: <the wildcard value of §3.2>
 *   Accept-Encoding: identity
 *   Content-Type: application/gzip
 *   Transfer-Encoding: chunked
 *   Connection: keep-alive
 *   X-Request-Id: <26 chars [a-z2-7]>
 *   <blank line>
 *
 * Exactly eight headers in that order; no extension headers, no
 * Content-Length, no Expect, no duplicate, no lowercase spelling. The request
 * line must be the exact literal (POST, the design path, HTTP/1.1). A header
 * block longer than TBT_HTTP_HDR_MAX is a protocol error. A bare LF, obs-fold
 * or whitespace before a colon is a protocol error.
 *
 * The block identifies nothing about the carrier: there is no
 * X-TBTUN-Carrier, no carrier index and no "tbtun" anywhere on the wire. The
 * carrier index is a HELLO body field (§3.3), never an HTTP header. */
size_t tbt_http_req_parse(const char *buf, size_t len, struct tbt_http_req *out, int *err);

/* Format a UTC epoch-second value as an IMF-fixdate:
 *
 *   Thu, 01 Jan 1970 00:00:00 GMT
 *
 * Exactly TBT_HTTP_DATE_LEN (29) bytes, no NUL, no timezone database, no
 * locale. Gregorian leap-year arithmetic; 1970-01-01 through
 * 9999-12-31 23:59:59 are representable. Returns 29, or 0 when cap < 29 or
 * the value is outside that range (the year would not be four digits). */
size_t tbt_http_http_date(char *buf, size_t cap, u64 unix_secs);

/* Emit the response header block of DESIGN.md §3.2, including the final blank
 * line:
 *
 *   HTTP/1.1 200 OK
 *   Content-Type: application/gzip
 *   Transfer-Encoding: chunked
 *   Connection: keep-alive
 *   Server: nginx
 *   Date: <cfg->unix_secs as IMF-fixdate>
 *   <blank line>
 *
 * Returns the bytes written (163 for a four-digit year), or 0 when cap is too
 * small, cfg is NULL, or the date is outside the representable range. */
size_t tbt_http_resp_header(char *buf, size_t cap, const struct tbt_http_resp_cfg *cfg);

/* Parse the response status line and headers from buf[0..len).
 *
 * Stateless and resumable by re-scan: on TBT_HTTP_NEED_MORE the caller keeps
 * the bytes and calls again with a longer prefix. On TBT_HTTP_OK it returns
 * the header length (the first body byte is at buf + header_len) and fills
 * *out. On TBT_HTTP_NEED_MORE or TBT_HTTP_PROTO it returns 0 and does not
 * write *out.
 *
 * Rules: HTTP/1.0 and HTTP/1.1 status lines are accepted, the status code must
 * be exactly 200, header names are case-insensitive, unknown headers are
 * skipped, obs-fold is rejected, a Transfer-Encoding value other than
 * "chunked" is rejected, and Transfer-Encoding together with Content-Length is
 * rejected (RFC 9112 §6.3). The block is bounded by TBT_HTTP_HDR_MAX.
 *
 * A 200 with no Transfer-Encoding is valid HTTP and parses with chunked=false;
 * a caller that expects this design's chunked response body must check that
 * flag (or require it before feeding tbt_http_chunk_feed). */
size_t tbt_http_resp_parse(const char *buf, size_t len, struct tbt_http_resp *out, int *err);

/* Scan for the CRLFCRLF that ends a request header block, without parsing or
 * validating it. Returns the header length (the offset just past the blank
 * line) and sets *err to OK. Returns 0 with NEED_MORE when no terminator is in
 * the bytes seen so far, or with PROTO when the block would exceed
 * TBT_HTTP_HDR_MAX. Resume-safe by re-scan: after NEED_MORE, call again with a
 * longer prefix. A bare LF is not a terminator, but this helper does not
 * reject it; use tbt_http_req_parse() when the block must be validated. */
size_t tbt_http_skip_request_header(const u8 *buf, size_t len, int *err);

/* Emit a chunk size line: lowercase hex, no leading zeros, CRLF. `body_len`
 * is the chunk-data length. Returns the bytes written, or 0 when body_len
 * exceeds TBT_CHUNK_MAX or cap is too small. body_len 0 emits "0\r\n", the
 * first half of the terminator; the empty trailer section is
 * tbt_http_chunk_tail(). */
size_t tbt_http_chunk_hdr(char *buf, size_t cap, size_t body_len);

/* Emit the two bytes CRLF that close a chunk's data, or the empty trailer
 * section after the 0-chunk. Returns 2, or 0 when cap < 2. */
size_t tbt_http_chunk_tail(char *buf, size_t cap);

/* Emit one complete PAD chunk (§3.7): "20\r\n", then the 32-byte TBT_F_PAD
 * frame (body_len 0), then CRLF. 38 bytes. Returns 38, or 0 when cap is too
 * small. The frame carries TBT_CONV and zero reserved bytes, exactly as
 * tbt_proto.c would encode it. */
size_t tbt_http_pad_chunk(char *buf, size_t cap);

/* Emit one drip chunk (§3.1.1): "1\r\n" + 0x23 (the TBT_F_PAD type byte) +
 * CRLF. 6 bytes. The body is container filler, not a frame: the receiver
 * recognises it by the announced chunk size (1), not by the length of one body
 * run, and discards every run of that chunk. Returns 6, or 0 when cap is too
 * small. */
size_t tbt_http_drip_chunk(char *buf, size_t cap);

/* Chunk decoder states. A zeroed struct starts in TBT_HTTP_CHUNK_SIZE;
 * tbt_http_chunk_reset() returns a used parser to that state. TBT_HTTP_CHUNK_BAD
 * is terminal: every later feed reports TBT_HTTP_PROTO. */
enum tbt_http_chunk_state
{
    TBT_HTTP_CHUNK_SIZE = 0,
    TBT_HTTP_CHUNK_SIZE_END,
    TBT_HTTP_CHUNK_EXT,
    TBT_HTTP_CHUNK_EXT_END,
    TBT_HTTP_CHUNK_DATA,
    TBT_HTTP_CHUNK_DATA_END,
    TBT_HTTP_CHUNK_DATA_END_LF,
    TBT_HTTP_CHUNK_TRAILER,
    TBT_HTTP_CHUNK_TRAILER_LINE,
    TBT_HTTP_CHUNK_TRAILER_LINE_LF,
    TBT_HTTP_CHUNK_TRAILER_BLANK_LF,
    TBT_HTTP_CHUNK_DONE,
    TBT_HTTP_CHUNK_BAD,
};

/* Chunk decoder. No pointers, no allocation: all partial state is these
 * integers, so it survives across reads exactly as tbt_rx needs. */
struct tbt_http_chunk_parser
{
    u32 state;          /* enum tbt_http_chunk_state */
    u32 size;           /* chunk size being accumulated */
    u32 chunk_size;     /* announced size of the chunk currently being read */
    u32 remaining;      /* body bytes still owed by the current chunk */
    u32 digits;         /* hex digits seen in the current size line */
    u32 line_bytes;     /* size line + extension length, bounded */
    u32 trailer_bytes;  /* trailer section length, bounded */
    u32 chunks;         /* completed data chunks, for diagnostics only */
};

/* Receiver bookkeeping for one direction's chunked HTTP messages.
 *
 * The caller owns the byte buffer and its offset. This struct only records
 * which stage the current message is in; it is deliberately not a feed
 * function, because buffering and resumption belong to the carrier's receive
 * path.
 *
 * The response-direction loop is:
 *
 *   1. If m->hdr_len == 0, call
 *      tbt_http_resp_parse(buf, len, &m->resp, &err).
 *        NEED_MORE -> keep the bytes and return.
 *        PROTO     -> kill the carrier.
 *        OK        -> require m->resp.chunked (this protocol has no
 *                     length-delimited body); set m->hdr_len to the returned
 *                     length and advance buf/len past the header.
 *   2. Feed the remaining bytes to tbt_http_chunk_feed(&m->chunk, ...).
 *        NEED_MORE -> keep the remaining bytes and return.
 *        PROTO     -> kill the carrier.
 *        DONE      -> tbt_http_msg_reset(m); the next bytes are the next
 *                     response's status line, so return to step 1. The chunk
 *                     parser never consumes past the terminator.
 *
 * The request direction uses tbt_http_req_parse() (validated) or
 * tbt_http_skip_request_header() (skip only) in step 1, then the same chunk
 * loop. */
struct tbt_http_msg
{
    struct tbt_http_resp resp;          /* filled by tbt_http_resp_parse */
    struct tbt_http_chunk_parser chunk; /* body state after the header */
    size_t hdr_len;                     /* 0 = header not consumed; >0 = consumed */
};

/* Return a message to the header stage for the next message on the carrier.
 * Equivalent to zeroing it and initializing the chunk parser. */
void tbt_http_msg_reset(struct tbt_http_msg *m);

/* Feed chunked bytes and yield one contiguous body run.
 *
 * Returns the number of bytes consumed from buf. On TBT_HTTP_OK,
 * *out_body / *out_body_len describe body bytes that lie wholly inside
 * buf[0..returned). The caller processes those bytes and feeds again with
 * buf + returned; a returned run is never copied. A zero-length run with OK
 * means the message is complete (state TBT_HTTP_CHUNK_DONE) and the caller
 * must tbt_http_chunk_reset() before parsing the next message; the parser
 * never consumes bytes past the terminator's blank line.
 *
 * TBT_HTTP_NEED_MORE consumes all len bytes and means the input ended before
 * the next body run or the message terminator; the parser is then in a
 * structure state or still owes body bytes. TBT_HTTP_PROTO is permanent.
 * The parser accepts uppercase hex and leading zeros on receive (RFC 9112
 * allows both; the encoder never emits them), accepts and ignores chunk
 * extensions, accepts and ignores trailer headers after the 0-chunk, rejects
 * any chunk size above TBT_CHUNK_MAX, and rejects a bare LF anywhere.
 *
 * A body run may be shorter than the chunk it belongs to when the input is
 * fragmented. The caller must therefore decide "filler or frame?" from the
 * announced chunk size in p->chunk_size, which is valid whenever a body run is
 * returned: a chunk of size 1 is the drip filler of §3.1.1 and every run of it
 * is discarded; a chunk of size >= TBT_HDR_LEN is a frame chunk even when this
 * particular run is one byte; a chunk of size 2..31 is a
 * framing error. The container itself accepts any size up to TBT_CHUNK_MAX and
 * leaves that policy to the caller, because it also decodes the far side's
 * response body. */
size_t tbt_http_chunk_feed(struct tbt_http_chunk_parser *p, const u8 *buf, size_t len,
                           const u8 **out_body, size_t *out_body_len, int *err);

/* Return a parser to the start of a message. Equivalent to zeroing it. */
void tbt_http_chunk_reset(struct tbt_http_chunk_parser *p);

/* True once the terminating 0-chunk and its trailer section were consumed. */
static inline bool tbt_http_chunk_done(const struct tbt_http_chunk_parser *p)
{
    return p && p->state == TBT_HTTP_CHUNK_DONE;
}

/* ------------------------------------------------------------------------- */
/* The carrier profile: one chunk per record, and the receive assembler.      */

/* Encode one frame as one HTTP chunk. The chunk data is exactly the record:
 * the 32-byte header followed by hdr->body_len body bytes, with no length
 * prefix of any kind (TBT_CONV 0x0005). hdr->body_len is authoritative and is
 * validated with tbt_hdr_check(); a TBT_F_PAD record must have body_len 0 and
 * is emitted as the canonical 38-byte PAD chunk.
 *
 * Returns the bytes written (size line + record + CRLF), or 0 on invalid
 * input or a short cap. A 0 return means the buffer must not be sent; partial
 * output is not a message. The maximum output is TBT_HTTP_FRAME_MAX. */
struct tbt_http_out
{
    const struct tbt_hdr *hdr; /* body_len is the body length */
    const u8 *body;            /* hdr->body_len bytes; may be NULL when 0 */
};

size_t tbt_http_frame_encode(char *buf, size_t cap, const struct tbt_http_out *out);

/* One chunk whose body is a complete 40-byte FEC record (T0 0x1001). The
 * HTTP request/response headers and the chunk framing stay plaintext; only the
 * FEC body may be encrypted by the caller. Returns the chunk length or 0. */
size_t tbt_http_fec_chunk_encode(char *buf, size_t cap, const u8 *rec,
                                 size_t rec_len);

/* Receive status. TBT_RX_OK means one record was produced, NEED_MORE means the
 * input ended before a record (or the message ended), PROTO is permanent. */
enum tbt_http_rx_status
{
    TBT_RX_OK = 0,
    TBT_RX_NEED_MORE,
    TBT_RX_PROTO,
};

enum tbt_http_rx_kind
{
    TBT_HTTP_RX_NONE = 0,
    TBT_HTTP_RX_DRIP,
    TBT_HTTP_RX_RECORD,
};

/* Receive assembler. The chunk parser handles HTTP framing; rec[] holds the
 * record being assembled across feed boundaries. One instance per carrier
 * direction, embedded in the carrier, not allocated by this unit.
 *
 * Invariants enforced by tbt_http_frame_decode():
 *   - one chunk carries exactly one record: a chunk of size 32..TBT_REC_MAX
 *     must announce exactly TBT_HDR_LEN + body_len for the header it carries;
 *     a chunk with two records, a record split across two chunks, or a size
 *     2..31 is TBT_RX_PROTO;
 *   - a chunk of size 1 is drip filler and is discarded;
 *   - a chunk of size 32 whose header is TBT_F_PAD is a PAD record and is
 *     passed through; PAD and drip may interleave between records.
 *
 * A zeroed struct is the initial state. Call tbt_http_rx_new_message() after
 * the terminating 0-chunk and after skipping the next request header block. */
struct tbt_http_rx
{
    struct tbt_http_chunk_parser chunk;
    /* The larger of the legacy and FEC record formats: one assembler serves
     * both, and shrinking it to the FEC record would reject a legal legacy
     * DATA chunk. */
    u8 rec[TBT_HTTP_CHUNK_DATA_MAX];
    u32 rec_len;     /* bytes assembled for the current record */
    u32 rec_need;    /* TBT_HDR_LEN + body_len once the header is known */
    u32 chunk_size;  /* announced size of the current chunk */
    u8 chunk_kind;   /* enum tbt_http_rx_kind */
    bool chunk_started;
};

/* Feed chunked bytes and yield at most one complete record.
 *
 * Returns bytes consumed from buf. On TBT_RX_OK, *out describes one validated
 * record; out->body points into rx->rec and is valid only until the next call.
 * On TBT_RX_NEED_MORE, consumed bytes were taken into the parser/assembler;
 * call again with the rest, or with more bytes. On TBT_RX_PROTO, the return is
 * 0 and the carrier must be discarded. The function never reads past len,
 * allocates nothing, and does not consume bytes past the message terminator:
 * when it returns NEED_MORE with tbt_http_rx_done(rx), the next bytes are the
 * next request header block.
 *
 * A record is returned as soon as its chunk body is complete; the chunk's
 * trailing CRLF is consumed on the next call. */
size_t tbt_http_frame_decode(struct tbt_http_rx *rx, const u8 *buf, size_t len,
                             struct tbt_rec *out, int *err);

/* FEC variant: one chunk == one 40-byte record. Drip (1 byte) and the old PAD
 * chunk (32 bytes) are skipped. On TBT_RX_OK, *rec points into rx->rec and
 * *rec_len is the record length; valid until the next call. */
size_t tbt_http_fec_frame_decode(struct tbt_http_rx *rx, const u8 *buf, size_t len,
                                 const u8 **rec, size_t *rec_len, int *err);

/* Reset for the next message after the terminator and the next request header.
 * Equivalent to zeroing the struct and initializing the chunk parser. */
void tbt_http_rx_new_message(struct tbt_http_rx *rx);

/* True once the terminating 0-chunk and its trailer section were consumed. */
static inline bool tbt_http_rx_done(const struct tbt_http_rx *rx)
{
    return rx && rx->chunk.state == TBT_HTTP_CHUNK_DONE;
}

/* Cadence arithmetic: true when at least `keepalive_ms` (or `drip_ms`) have
 * elapsed since last_tx_ns. Pure; the caller owns the clock and the timer.
 * An interval of 0 disables the cadence (always false). The subtraction is
 * unsigned and wrap-safe, so a clock that steps backwards asks for a PAD
 * rather than suppressing one forever. */
bool tbt_http_need_pad(u64 last_tx_ns, u64 now_ns, u32 keepalive_ms);
bool tbt_http_need_drip(u64 last_tx_ns, u64 now_ns, u32 drip_ms);

#endif /* _TBT_HTTP_H */
