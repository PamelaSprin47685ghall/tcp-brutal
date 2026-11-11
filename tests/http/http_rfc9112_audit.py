#!/usr/bin/env python3
"""http_rfc9112_audit.py - byte-exact RFC 9112 audit of the HTTP carrier.

Runs tests/http_rfc9112_gen, which drives the real src/kernel/tbt_http.c encoders
(the same object the module links) over several keep-alive connections. Each
connection carries many request/response cycles: ARQ HELLO/OPEN/DATA, a
three-record FEC batch, keep-alive PAD/drip probes and the terminating
0-chunk.

This script is the authority, not a helper for the C test:

  - a strict RFC 9112 state machine parses every byte of both directions
    (request line / status line, field syntax, CRLF only, chunk-size hex,
    chunk-data boundaries, trailer section);
  - semantic checks assert the design contract: ASCII headers, exactly one
    Host, `Connection: keep-alive`, `Transfer-Encoding: chunked` with no
    Content-Length, status 200, zero residue between cycles and at EOF;
  - every chunk body is classified as drip, PAD, a 32-byte ARQ record or a
    48-byte FEC record, and the record framing and CRCs are verified;
  - framing bytes are proved to contain no binary byte and no TBT marker, so
    a record header can never leak outside chunk data;
  - when h11 is importable the same streams are replayed through it; when
    http.client is available its header parser cross-checks every message.

usage: http_rfc9112_audit.py [GEN_EXE]

GEN_EXE defaults to ./tests/http_rfc9112_gen. h11 is optional here (the
Makefile supplies it through uv when its cache has it); the audit passes
without h11 and prints that the cross-check was skipped.
"""
import http.client
import io
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from email.utils import formatdate
from pathlib import Path

TBT_CONV = 0x000D
TBT_FEC_CONV = 0x100C
TBT_HDR_LEN = 32
TBT_REC_MAX = 32 + 8192
TBT_FEC_HDR_LEN = 48
TBT_FEC_REC_MAX = 48 + 1024

ARQ_TYPES = {
    0x10: "DATA",
    0x13: "OPEN",
    0x14: "FIN",
    0x15: "RST",
    0x20: "FBK",
    0x21: "WINDOW",
    0x22: "HELLO",
    0x23: "PAD",
    0x24: "RETIRE",
    0x25: "SACK",
}
ARQ_BOUNDS = {
    0x10: (1, 8192),
    0x13: (36, 36),
    0x14: (16, 16),
    0x15: (0, 0),
    0x20: (0, 288),
    0x21: (138, 138),
    0x22: (23, 90),
    0x23: (0, 0),
    0x24: (1, 1),
    0x25: (1, 1),
}
FEC_TYPES = {
    0x30: "DATA",
    0x31: "PARITY",
    0x32: "ACK",
    0x33: "NACK",
    0x34: "CFG",
    0x35: "FBK",
}
FEC_CONTROL_LEN = {0x32: 24, 0x33: 64, 0x34: 64, 0x35: 64}

TOKEN = re.compile(rb"[!#$%&'*+\-.^_`|~0-9A-Za-z]+")
REQ_LINE = re.compile(rb"POST /api/v2/upload HTTP/1\.1")
STATUS_LINE = re.compile(rb"HTTP/1\.1 200 (?:[ -~]*)\Z")
HEX_LINE = re.compile(rb"[0-9a-f]+")
DATE_RE = re.compile(
    rb"(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), [0-9]{2} "
    rb"(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
    rb"[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT\Z"
)

UA = (
    b"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    b"(KHTML, like Gecko) Chrome/131.0 Safari/537.36"
)
REQ_HEADER_NAMES = [
    b"Host",
    b"User-Agent",
    b"Accept",
    b"Accept-Encoding",
    b"Content-Type",
    b"Transfer-Encoding",
    b"Connection",
    b"X-Request-Id",
]
RESP_HEADER_NAMES = [
    b"Content-Type",
    b"Transfer-Encoding",
    b"Connection",
    b"Server",
    b"Date",
]


class Proto(Exception):
    """A strict RFC 9112 or design-contract violation."""


class Audit:
    def __init__(self):
        self.checks = 0
        self.failures = []
        self.h11_used = False

    def ok(self, cond, what):
        self.checks += 1
        if not cond:
            self.failures.append(what)
        return bool(cond)

    def need(self, cond, what):
        if not self.ok(cond, what):
            raise Proto(what)


def le16(b, off):
    return b[off] | (b[off + 1] << 8)


def le32(b, off):
    return (
        b[off]
        | (b[off + 1] << 8)
        | (b[off + 2] << 16)
        | (b[off + 3] << 24)
    )


def le64(b, off):
    return le32(b, off) | (le32(b, off + 4) << 32)


def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC_TABLE = _crc32c_table()


def crc32c(data, seed=0):
    """Standard CRC32C (Castagnoli), reflected, init ~seed, final xor."""
    crc = seed ^ 0xFFFFFFFF
    for byte in data:
        crc = _CRC_TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def read_line(data, pos, ctx):
    """Return (line without CRLF, next offset). Reject anything but CRLF."""
    idx = data.find(b"\n", pos)
    if idx < 0:
        raise Proto(f"{ctx}: unterminated line at byte {pos}")
    if idx == pos or data[idx - 1] != 0x0D:
        raise Proto(f"{ctx}: bare LF at byte {idx}")
    line = data[pos : idx - 1]
    if b"\r" in line:
        raise Proto(f"{ctx}: bare CR inside a line at byte {pos}")
    return line, idx + 1


def parse_field_line(line, audit, ctx):
    audit.need(
        line[:1] not in (b" ", b"\t"),
        f"{ctx}: obs-fold is refused",
    )
    colon = line.find(b":")
    audit.need(colon > 0, f"{ctx}: field line has a name before ':'")
    name = line[:colon]
    audit.need(
        TOKEN.fullmatch(name) is not None,
        f"{ctx}: field-name {name!r} is a token",
    )
    rest = line[colon + 1 :]
    audit.need(
        rest.startswith(b" ") and not rest.startswith(b"  "),
        f"{ctx}: exactly one SP after ':'",
    )
    value = rest[1:]
    audit.need(
        value == value.strip(b" "),
        f"{ctx}: field-value has no leading or trailing SP",
    )
    audit.need(
        b"\t" not in value,
        f"{ctx}: emitter uses SP only, no HTAB in a field-value",
    )
    audit.need(
        all(0x20 <= c <= 0x7E for c in value),
        f"{ctx}: field-value is printable ASCII",
    )
    return name, value


def parse_headers(data, pos, audit, ctx):
    start = pos
    headers = []
    while True:
        line, pos = read_line(data, pos, ctx)
        if not line:
            return headers, pos, data[start:pos]
        name, value = parse_field_line(line, audit, ctx)
        headers.append((name, value))


def parse_arq_record(body, audit, ctx):
    audit.need(len(body) >= TBT_HDR_LEN, f"{ctx}: ARQ record has a 32-byte header")
    typ = body[0]
    flags = body[1]
    path_id = body[2]
    ack_count = body[3]
    ack_mask = le16(body, 4)
    body_len = le16(body, 6)
    seq = le64(body, 8)
    uack = le64(body, 16)
    sid = le32(body, 24) | (body[28] << 32)
    reserved2 = body[29]
    conv = le16(body, 30)

    audit.need(typ in ARQ_TYPES, f"{ctx}: ARQ type 0x{typ:02x} is defined")
    audit.need(TBT_HDR_LEN + body_len == len(body), f"{ctx}: ARQ body_len matches chunk")
    audit.need(conv == TBT_CONV, f"{ctx}: ARQ conv is 0x000d")
    audit.need(flags & ~0x3F == 0, f"{ctx}: ARQ flags fit TBT_FF_MASK")
    audit.need(ack_count <= 16, f"{ctx}: path_ack_count <= 16")
    audit.need(ack_mask & ~0xFFFF == 0, f"{ctx}: path_ack_mask is 16 bits")
    audit.need(reserved2 == 0, f"{ctx}: ARQ reserved2 is zero")
    lo, hi = ARQ_BOUNDS[typ]
    audit.need(lo <= body_len <= hi, f"{ctx}: ARQ body_len inside its type bounds")
    return {
        "type": typ,
        "name": ARQ_TYPES[typ],
        "flags": flags,
        "path": path_id,
        "ack_count": ack_count,
        "ack_mask": ack_mask,
        "body_len": body_len,
        "seq": seq,
        "uack": uack,
        "sid": sid,
        "conv": conv,
        "body": body[TBT_HDR_LEN:],
    }


def parse_fec_record(body, audit, ctx):
    audit.need(len(body) >= TBT_FEC_HDR_LEN, f"{ctx}: FEC record has a 48-byte header")
    typ = body[0]
    flags = body[1]
    share = le16(body, 2)
    n = body[4]
    m = body[5]
    epoch = le16(body, 6)
    block_id = le64(body, 8)
    seq = le64(body, 16)
    win_len = le16(body, 24)
    payload_len = le16(body, 26)
    conv = le16(body, 28)
    reserved = le16(body, 30)
    sid = le64(body, 32)
    payload_crc = le32(body, 40)
    header_crc = le32(body, 44)

    audit.need(typ in FEC_TYPES, f"{ctx}: FEC type 0x{typ:02x} is defined")
    audit.need(conv == TBT_FEC_CONV, f"{ctx}: FEC conv is 0x100c")
    audit.need(reserved == 0, f"{ctx}: FEC reserved is zero")
    audit.need(flags & ~0x03 == 0, f"{ctx}: FEC flags fit TBT_FEC_FF_MASK")
    audit.need(
        TBT_FEC_HDR_LEN + payload_len == len(body),
        f"{ctx}: FEC payload_len matches chunk",
    )
    audit.need(
        header_crc == crc32c(body[:44]),
        f"{ctx}: FEC header_crc32c is correct",
    )
    audit.need(
        payload_crc == crc32c(body[TBT_FEC_HDR_LEN:]),
        f"{ctx}: FEC payload_crc32c is correct",
    )
    audit.need(payload_len <= 1024, f"{ctx}: FEC payload_len <= S")

    if typ in (0x30, 0x31):
        audit.need(n >= 1 and m >= 1, f"{ctx}: FEC n and m are non-zero")
        audit.need(n + m <= 256, f"{ctx}: FEC n+m <= 256")
        audit.need(share < n + m, f"{ctx}: FEC share_index < n+m")
        audit.need(seq < (1 << 63), f"{ctx}: FEC seq fits the signed bound")
        if typ == 0x30:
            audit.need(share < n, f"{ctx}: DATA names a data share")
            audit.need(payload_len >= 1, f"{ctx}: DATA carries a payload")
            if flags & 0x02:
                audit.need(
                    payload_len == 1024 and 1 <= win_len <= 1024,
                    f"{ctx}: PADDED DATA is a full slot with a real win_len",
                )
            else:
                audit.need(win_len == 0, f"{ctx}: DATA win_len is zero")
            audit.need(epoch == 0, f"{ctx}: DATA epoch is zero")
        else:
            audit.need(share >= n, f"{ctx}: PARITY names a parity share")
            audit.need(flags & 0x02 == 0, f"{ctx}: PARITY is not PADDED")
            if win_len == 0:
                audit.need(
                    share == n and flags == 0 and payload_len == 0 and epoch == 0,
                    f"{ctx}: PARITY empty-block marker is canonical",
                )
            else:
                audit.need(win_len <= n, f"{ctx}: PARITY win_len <= n")
                audit.need(payload_len == 1024, f"{ctx}: PARITY payload is S")
                audit.need(epoch <= 1024, f"{ctx}: PARITY epoch <= S")
    else:
        audit.need(
            n == 0 and m == 0 and share == 0 and win_len == 0,
            f"{ctx}: FEC control record carries no share",
        )
        audit.need(
            FEC_CONTROL_LEN[typ] == payload_len,
            f"{ctx}: FEC control body length is exact",
        )

    return {
        "type": typ,
        "name": FEC_TYPES[typ],
        "flags": flags,
        "share": share,
        "n": n,
        "m": m,
        "epoch": epoch,
        "block_id": block_id,
        "seq": seq,
        "win_len": win_len,
        "payload_len": payload_len,
        "sid": sid,
        "body": body[TBT_FEC_HDR_LEN:],
    }


@dataclass
class Chunk:
    size: int
    body: bytes
    kind: str
    arq: dict = None
    fec: dict = None


@dataclass
class Message:
    start_line: bytes
    headers: list
    header_bytes: bytes
    chunks: list
    trailer: list
    framing: bytes
    start: int
    end: int

    @property
    def body_len(self):
        return sum(c.size for c in self.chunks)


def parse_chunks(data, pos, audit, ctx):
    chunks = []
    trailer = []
    framing = bytearray()
    while True:
        line_start = pos
        line, pos = read_line(data, pos, f"{ctx} chunk-size")
        framing += data[line_start:pos]
        audit.need(
            HEX_LINE.fullmatch(line) is not None,
            f"{ctx}: chunk-size is 1+ lowercase HEXDIG and no extension",
        )
        audit.need(
            len(line) == 1 or line[:1] != b"0",
            f"{ctx}: chunk-size has no leading zero",
        )
        size = int(line, 16)
        audit.need(
            size <= TBT_FEC_REC_MAX,
            f"{ctx}: chunk-size <= TBT_FEC_REC_MAX",
        )
        if size == 0:
            while True:
                tstart = pos
                tline, pos = read_line(data, pos, f"{ctx} trailer")
                framing += data[tstart:pos]
                if not tline:
                    break
                parse_field_line(tline, audit, f"{ctx} trailer")
                trailer.append(tline)
            chunks.append(Chunk(0, b"", "terminator"))
            break
        audit.need(
            pos + size + 2 <= len(data),
            f"{ctx}: chunk-data is complete",
        )
        body = data[pos : pos + size]
        pos += size
        audit.need(
            data[pos : pos + 2] == b"\r\n",
            f"{ctx}: chunk-data is followed by exactly CRLF",
        )
        framing += b"\r\n"
        pos += 2
        chunks.append(classify_chunk(body, audit, f"{ctx} chunk {len(chunks)}"))
    return chunks, trailer, pos, bytes(framing)


def classify_chunk(body, audit, ctx):
    if len(body) == 1:
        audit.need(body == b"#", f"{ctx}: the one-byte drip payload is 0x23")
        return Chunk(1, body, "drip")
    if len(body) == TBT_HDR_LEN and body[0] == 0x23:
        rec = parse_arq_record(body, audit, ctx)
        audit.need(rec["body_len"] == 0, f"{ctx}: PAD record body_len is zero")
        return Chunk(len(body), body, "pad", arq=rec)
    if TBT_HDR_LEN <= len(body) <= TBT_REC_MAX and body[0] in ARQ_TYPES:
        rec = parse_arq_record(body, audit, ctx)
        return Chunk(len(body), body, f"arq:{rec['name']}", arq=rec)
    if TBT_FEC_HDR_LEN <= len(body) <= TBT_FEC_REC_MAX and body[0] in FEC_TYPES:
        rec = parse_fec_record(body, audit, ctx)
        return Chunk(len(body), body, f"fec:{rec['name']}", fec=rec)
    raise Proto(
        f"{ctx}: chunk body is neither drip, PAD, an ARQ record nor a FEC "
        f"record (first byte 0x{body[0]:02x}, {len(body)} bytes)"
    )


def parse_message(data, pos, direction, audit, ctx):
    start = pos
    line, pos = read_line(data, pos, f"{ctx} start-line")
    if direction == "request":
        audit.need(
            REQ_LINE.fullmatch(line) is not None,
            f"{ctx}: request line is the exact POST /api/v2/upload HTTP/1.1",
        )
        parts = line.split(b" ")
        audit.need(len(parts) == 3, f"{ctx}: request line has three fields")
        audit.need(TOKEN.fullmatch(parts[0]) is not None, f"{ctx}: method is a token")
        audit.need(parts[2] == b"HTTP/1.1", f"{ctx}: version is HTTP/1.1")
    else:
        audit.need(
            STATUS_LINE.fullmatch(line) is not None,
            f"{ctx}: status line is HTTP/1.1 200 with a printable reason",
        )
    headers, pos, header_bytes = parse_headers(data, pos, audit, ctx)
    chunks, trailer, body_end, chunk_framing = parse_chunks(
        data, pos, audit, ctx
    )
    framing = header_bytes + chunk_framing
    msg = Message(
        start_line=line,
        headers=headers,
        header_bytes=header_bytes,
        chunks=chunks,
        trailer=trailer,
        framing=framing,
        start=start,
        end=body_end,
    )
    return msg, body_end, start


@dataclass
class Carrier:
    index: int
    up_name: str
    down_name: str
    host: str
    reqids: list
    dates: list


def parse_manifest(path, audit):
    try:
        text = Path(path).read_bytes().decode("ascii")
    except (OSError, UnicodeDecodeError) as exc:
        raise Proto(f"manifest: {exc}") from exc
    lines = text.splitlines()
    audit.need(lines and lines[0] == "version 1", "manifest: version 1")
    audit.need(len(lines) >= 3, "manifest: has the header lines")
    carriers = None
    rounds = None
    rows = []
    for line in lines[1:]:
        parts = line.split(" ")
        audit.need(parts and all(parts), "manifest: no empty field")
        if parts[0] == "carriers":
            audit.need(len(parts) == 2, "manifest: carriers line")
            carriers = int(parts[1])
        elif parts[0] == "rounds":
            audit.need(len(parts) == 2, "manifest: rounds line")
            rounds = int(parts[1])
        else:
            audit.need(parts[0] == "carrier", "manifest: known line kind")
            rows.append(parts)
    audit.need(carriers is not None and carriers > 0, "manifest: carrier count")
    audit.need(rounds is not None and rounds > 0, "manifest: round count")
    audit.need(len(rows) == carriers, "manifest: one row per carrier")
    out = []
    for row in rows:
        audit.need(len(row) == 5 + 2 * rounds, "manifest: carrier row arity")
        idx = int(row[1])
        up = row[2]
        down = row[3]
        host = row[4]
        reqids = row[5 : 5 + rounds]
        dates = [int(v) for v in row[5 + rounds :]]
        audit.need(idx == len(out), "manifest: carrier indices are dense")
        audit.need(
            all(re.fullmatch(r"[a-z2-7]{26}", rid) is not None for rid in reqids),
            "manifest: request ids are 26 base32 characters",
        )
        audit.need(len(set(reqids)) == rounds, "manifest: request ids rotate")
        out.append(Carrier(idx, up, down, host, reqids, dates))
    audit.need(
        len({c.host for c in out}) == carriers,
        "manifest: every carrier has a distinct Host",
    )
    return carriers, rounds, out


def expected_kinds(round_no):
    probes = {
        0: ["pad"],
        1: ["drip"],
        2: ["pad", "drip"],
    }[round_no % 3]
    base = [] if round_no else ["arq:HELLO"]
    return base + [
        "arq:OPEN",
        "arq:DATA",
        "fec:DATA",
        "fec:DATA",
        "fec:PARITY",
    ] + probes + ["terminator"]


def framing_is_ascii(framing):
    return all(c in (0x09, 0x0A, 0x0D) or 0x20 <= c <= 0x7E for c in framing)


def audit_message(msg, direction, carrier, round_no, audit, ctx):
    if direction == "request":
        expected_line = b"POST /api/v2/upload HTTP/1.1"
        expected_headers = [
            (b"Host", carrier.host.encode("ascii")),
            (b"User-Agent", UA),
            (b"Accept", b"*/*"),
            (b"Accept-Encoding", b"identity"),
            (b"Content-Type", b"application/gzip"),
            (b"Transfer-Encoding", b"chunked"),
            (b"Connection", b"keep-alive"),
            (b"X-Request-Id", carrier.reqids[round_no].encode("ascii")),
        ]
        expected_names = REQ_HEADER_NAMES
    else:
        expected_line = b"HTTP/1.1 200 OK"
        expected_headers = [
            (b"Content-Type", b"application/gzip"),
            (b"Transfer-Encoding", b"chunked"),
            (b"Connection", b"keep-alive"),
            (b"Server", b"nginx"),
            (
                b"Date",
                formatdate(carrier.dates[round_no], usegmt=True).encode("ascii"),
            ),
        ]
        expected_names = RESP_HEADER_NAMES

    audit.need(msg.start_line == expected_line, f"{ctx}: exact start line")
    audit.need(
        msg.headers == expected_headers,
        f"{ctx}: exact headers, order and rotation values",
    )
    audit.need(
        [name for name, _ in msg.headers] == expected_names,
        f"{ctx}: header name sequence matches the design",
    )
    audit.need(
        len(msg.header_bytes) <= 4096,
        f"{ctx}: header block stays inside TBT_HTTP_HDR_MAX",
    )
    audit.need(
        msg.header_bytes.endswith(b"\r\n\r\n"),
        f"{ctx}: header block ends with a blank CRLF line",
    )
    if direction == "request":
        audit.need(
            sum(1 for name, _ in msg.headers if name == b"Host") == 1,
            f"{ctx}: exactly one Host header",
        )
    else:
        date = dict(msg.headers)[b"Date"]
        audit.need(
            DATE_RE.fullmatch(date) is not None,
            f"{ctx}: Date is an IMF-fixdate",
        )
    header_map = dict(msg.headers)
    audit.need(
        header_map.get(b"Transfer-Encoding") == b"chunked",
        f"{ctx}: Transfer-Encoding is chunked",
    )
    audit.need(
        b"Content-Length" not in header_map,
        f"{ctx}: no Content-Length with chunked body",
    )
    audit.need(
        header_map.get(b"Connection") == b"keep-alive",
        f"{ctx}: Connection is keep-alive",
    )
    audit.need(
        not any(b"tbtun" in n.lower() or b"tbtun" in v.lower()
                for n, v in msg.headers),
        f"{ctx}: no tbtun marker in a header",
    )

    kinds = [c.kind for c in msg.chunks]
    audit.need(
        kinds == expected_kinds(round_no),
        f"{ctx}: chunk sequence is {expected_kinds(round_no)} (saw {kinds})",
    )
    audit.need(
        msg.chunks and msg.chunks[-1].kind == "terminator",
        f"{ctx}: body ends with the terminating 0-chunk",
    )
    audit.need(
        msg.trailer == [],
        f"{ctx}: trailer section is empty",
    )
    audit.need(
        framing_is_ascii(msg.framing),
        f"{ctx}: framing is printable ASCII with CR/LF only",
    )
    audit.need(
        b"tbtun" not in msg.framing.lower() and b"TBT" not in msg.framing,
        f"{ctx}: no TBT marker outside chunk data",
    )

    arq = [c.arq for c in msg.chunks if c.arq is not None]
    fec = [c.fec for c in msg.chunks if c.fec is not None]
    for rec in arq:
        if rec["name"] in ("HELLO", "OPEN", "DATA"):
            audit.need(
                rec["path"] == carrier.index,
                f"{ctx}: {rec['name']} names this carrier",
            )
            audit.need(
                rec["sid"] == 0x510000 + carrier.index,
                f"{ctx}: {rec['name']} carries this carrier's sid",
            )
    if round_no == 0:
        hello = next(r for r in arq if r["name"] == "HELLO")
        audit.need(
            hello["body_len"] == 90,
            f"{ctx}: HELLO uses the 90-byte v9 body",
        )
        audit.need(
            hello["body"][0] == 9,
            f"{ctx}: HELLO body[0] is TBT_WIRE_VERSION_V6",
        )
        audit.need(
            hello["body"][1] == (2 if direction == "request" else 3),
            f"{ctx}: HELLO names role near/far for its direction",
        )
        audit.need(
            hello["body"][22] == 1,
            f"{ctx}: HELLO names the http-kal profile",
        )
        audit.need(hello["seq"] == 0, f"{ctx}: HELLO seq is zero")
    opened = next(r for r in arq if r["name"] == "OPEN")
    audit.need(
        opened["body_len"] == 36 and opened["body"][:32].count(0) < 32,
        f"{ctx}: OPEN carries a non-empty 32-byte group bitmap",
    )
    audit.need(
        sum(bin(b).count("1") for b in opened["body"][:32]) == 1,
        f"{ctx}: OPEN bitmap names one group",
    )
    audit.need(
        opened["seq"] == 1 + round_no,
        f"{ctx}: OPEN seq rotates with the round",
    )
    data = next(r for r in arq if r["name"] == "DATA")
    audit.need(
        data["body_len"] == 37 and data["seq"] == 1 + round_no,
        f"{ctx}: ARQ DATA body and seq rotate with the round",
    )

    audit.need(
        [r["name"] for r in fec] == ["DATA", "DATA", "PARITY"],
        f"{ctx}: FEC batch is DATA, DATA, PARITY",
    )
    audit.need(
        all(r["block_id"] == round_no + 1 for r in fec),
        f"{ctx}: the three FEC records share the round's block id",
    )
    audit.need(
        all(r["sid"] == 0x510000 + carrier.index for r in fec),
        f"{ctx}: every FEC record carries this carrier's sid",
    )
    audit.need(
        [r["share"] for r in fec] == [0, 1, 4],
        f"{ctx}: FEC shares are the two data shares and parity 0",
    )
    audit.need(
        [r["seq"] for r in fec] == [round_no * 100, round_no * 100 + 1,
                                    round_no * 100 + 4],
        f"{ctx}: FEC seq rotates with the round",
    )
    audit.need(
        fec[0]["payload_len"] == 64 and fec[1]["payload_len"] == 64,
        f"{ctx}: FEC DATA payload length is 64",
    )
    audit.need(
        fec[2]["win_len"] == 4 and fec[2]["payload_len"] == 1024,
        f"{ctx}: FEC PARITY is a full 4-share block",
    )


def http_client_check(msg, direction, audit, ctx):
    try:
        parsed = http.client.parse_headers(io.BytesIO(msg.header_bytes))
    except Exception as exc:  # the stdlib parser is the oracle here
        audit.need(False, f"{ctx}: http.client.parse_headers accepted the block ({exc})")
        return
    audit.need(
        parsed.get("Transfer-Encoding") == "chunked",
        f"{ctx}: http.client sees Transfer-Encoding: chunked",
    )
    audit.need(
        parsed.get("Connection") == "keep-alive",
        f"{ctx}: http.client sees Connection: keep-alive",
    )
    audit.need(
        parsed.get("Content-Length") is None,
        f"{ctx}: http.client sees no Content-Length",
    )
    if direction == "request":
        audit.need(
            parsed.get("Host") is not None,
            f"{ctx}: http.client sees the Host header",
        )
    else:
        audit.need(
            parsed.get("Date") is not None,
            f"{ctx}: http.client sees the Date header",
        )


def h11_check(data, direction, msgs, audit, ctx):
    try:
        import h11
    except ImportError:
        return None
    audit.h11_used = True
    for i, msg in enumerate(msgs):
        cctx = f"{ctx} h11 cycle {i}"
        # h11 tracks a whole connection, and this audit feeds one direction
        # only: a fresh connection per message is the only way to use it as a
        # single-message oracle. Connection reuse is proved by the strict
        # parser, which consumes the stream with zero residue.
        conn = h11.Connection(h11.SERVER if direction == "request" else h11.CLIENT)
        conn.receive_data(data[msg.start : msg.end])
        event = None
        body = bytearray()
        chunk_starts = 0
        ended = False
        try:
            while True:
                ev = conn.next_event()
                if ev is h11.NEED_DATA:
                    break
                if isinstance(ev, (h11.Request, h11.Response)):
                    event = ev
                elif isinstance(ev, h11.Data):
                    body += ev.data
                    if ev.chunk_start:
                        chunk_starts += 1
                elif isinstance(ev, h11.EndOfMessage):
                    audit.need(
                        not ev.headers.raw_items(),
                        f"{cctx}: no trailer header",
                    )
                    ended = True
                    break
                else:
                    audit.need(False, f"{cctx}: h11 produced {ev!r}")
                    return False
        except Exception as exc:
            audit.need(False, f"{cctx}: h11 rejected the message ({exc})")
            return False

        trailing = getattr(conn, "trailing_data", (b"", False))
        audit.need(event is not None, f"{cctx}: h11 produced a start event")
        audit.need(ended, f"{cctx}: h11 reached EndOfMessage")
        audit.need(
            event.headers.raw_items() == msg.headers,
            f"{cctx}: header block matches the strict parser",
        )
        audit.need(
            len(body) == msg.body_len,
            f"{cctx}: h11 body length matches the strict parser",
        )
        audit.need(
            chunk_starts == sum(1 for c in msg.chunks if c.size > 0),
            f"{cctx}: h11 counted every non-zero chunk",
        )
        audit.need(
            trailing[0] == b"" and not trailing[1],
            f"{cctx}: h11 leaves no trailing byte and no close",
        )
        if direction == "request":
            audit.need(
                event.method == b"POST"
                and event.target == b"/api/v2/upload"
                and event.http_version == b"1.1",
                f"{cctx}: h11 request line",
            )
        else:
            audit.need(
                event.status_code == 200 and event.http_version == b"1.1",
                f"{cctx}: h11 status line",
            )
    return True


def audit_stream(data, direction, carrier, rounds, audit):
    ctx = f"carrier {carrier.index} {direction}"
    pos = 0
    msgs = []
    while pos < len(data):
        msg, pos, _ = parse_message(data, pos, direction, audit, ctx)
        msgs.append(msg)
    audit.need(pos == len(data), f"{ctx}: parser consumed the whole stream")
    audit.need(
        len(msgs) == rounds,
        f"{ctx}: exactly {rounds} keep-alive messages, saw {len(msgs)}",
    )
    for i, msg in enumerate(msgs):
        audit_message(msg, direction, carrier, i, audit, f"{ctx} cycle {i}")
        http_client_check(msg, direction, audit, f"{ctx} cycle {i}")

    if direction == "request":
        seen = [dict(m.headers)[b"X-Request-Id"].decode("ascii") for m in msgs]
        audit.need(
            seen == carrier.reqids,
            f"{ctx}: X-Request-Id rotates exactly as manifest says",
        )
        audit.need(
            len(set(seen)) == len(seen),
            f"{ctx}: every request id on this connection is distinct",
        )
        audit.need(
            all(dict(m.headers)[b"Host"] == carrier.host.encode("ascii")
                for m in msgs),
            f"{ctx}: Host matches the manifest target on every cycle",
        )
    else:
        seen = [dict(m.headers)[b"Date"] for m in msgs]
        expected = [
            formatdate(d, usegmt=True).encode("ascii") for d in carrier.dates
        ]
        audit.need(seen == expected, f"{ctx}: Date rotates exactly as manifest says")
        audit.need(seen == sorted(seen), f"{ctx}: Date increases across cycles")
        audit.need(
            len(set(seen)) == len(seen),
            f"{ctx}: every Date on this connection is distinct",
        )

    audit.need(
        all(m.body_len > 0 for m in msgs),
        f"{ctx}: every message carries a non-empty chunked body",
    )
    h11_check(data, direction, msgs, audit, ctx)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tests/http_rfc9112_gen"
    audit = Audit()
    with tempfile.TemporaryDirectory(prefix="tbt-http-rfc9112-") as tmp:
        proc = subprocess.run(
            [exe, tmp], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        if proc.returncode != 0:
            print("FAIL http-rfc9112: generator failed")
            print(proc.stderr.decode("utf-8", "replace").strip())
            return 1
        try:
            carriers, rounds, rows = parse_manifest(
                Path(tmp) / "manifest.txt", audit
            )
        except Proto as exc:
            print(f"FAIL http-rfc9112: {exc}")
            return 1
        every_id = [rid for carrier in rows for rid in carrier.reqids]
        audit.need(
            len(set(every_id)) == len(every_id),
            "manifest: request ids rotate across every carrier",
        )
        for carrier in rows:
            for direction, name in (
                ("request", carrier.up_name),
                ("response", carrier.down_name),
            ):
                path = Path(tmp) / name
                data = path.read_bytes()
                audit.need(data, f"carrier {carrier.index} {direction}: non-empty stream")
                try:
                    audit_stream(data, direction, carrier, rounds, audit)
                except Proto as exc:
                    audit.ok(False, f"carrier {carrier.index} {direction}: {exc}")

    if audit.failures:
        for what in audit.failures[:20]:
            print(f"FAIL {what}")
        if len(audit.failures) > 20:
            print(f"FAIL ... and {len(audit.failures) - 20} more")
        print(
            f"FAIL http-rfc9112: {audit.checks} checks, "
            f"{len(audit.failures)} failures"
        )
        return 1
    print(
        f"PASS http-rfc9112: {audit.checks} checks, "
        f"{carriers} carriers x {rounds} rounds x 2 directions, "
        "RFC 9112 strict + http.client"
        + (" + h11" if audit.h11_used else " (h11 unavailable, skipped)"),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
