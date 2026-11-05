#!/usr/bin/env python3
"""test_http_live_h11.py - independent h11 check of a captured carrier stream.

Reads the same file as `host/test_http_live`: the frozen proc text format
(req/resp/first_chunk/req_len/resp_len) or the binary `TBT-HTTP-CAPTURE 1`
framing. The binary form has full bodies, so h11 must accept exactly one
request and one response with at least two chunks and EndOfMessage. The proc
text form is a snapshot with only the first chunk; h11 must accept the headers
and that first chunk, and the full-body/2+ chunk assertion is reported as a
skip, not a pass.
"""
import sys

import h11


def fail(message):
    print(f"h11: FAIL {message}")
    return 1


def unescape(value: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(value):
        c = value[i]
        if c != "\\":
            out.append(ord(c))
            i += 1
            continue
        i += 1
        if i >= len(value):
            raise ValueError("trailing backslash")
        e = value[i]
        if e == "r":
            out.append(13)
        elif e == "n":
            out.append(10)
        elif e == "\\":
            out.append(92)
        elif e == "x":
            if i + 2 >= len(value):
                raise ValueError("short \\x escape")
            out.append(int(value[i + 1 : i + 3], 16))
            i += 2
        else:
            raise ValueError(f"unknown escape \\{e}")
        i += 1
    return bytes(out)


def parse_fields(data: bytes):
    fields = {}
    for raw in data.splitlines():
        line = raw.decode("latin-1")
        if "=" not in line:
            continue
        name, value = line.split("=", 1)
        if name in {"req", "resp", "first_chunk", "req_len", "resp_len"}:
            fields[name] = value
    return fields


def parse_text(data: bytes):
    fields = parse_fields(data)
    if "req" not in fields:
        raise ValueError("missing req field")
    req = unescape(fields["req"])
    if "req_len" in fields and int(fields["req_len"]) != len(req):
        raise ValueError("req_len does not match the unescaped request")
    resp = None
    if "resp" in fields:
        resp = unescape(fields["resp"])
        if "resp_len" in fields and int(fields["resp_len"]) != len(resp):
            raise ValueError("resp_len does not match the unescaped response")
    first = None
    if "first_chunk" in fields:
        first = unescape(fields["first_chunk"])
    return req, resp, first


def normalize_raw(data: bytes) -> bytes:
    end = data.find(b"\r\n\r\n")
    if end >= 0:
        return data[: end + 4]
    lines = [line.rstrip(b"\r") for line in data.splitlines() if line.rstrip(b"\r")]
    if not lines:
        raise ValueError("empty raw request header")
    return b"\r\n".join(lines) + b"\r\n\r\n"


def read_line(data: bytes, pos: int):
    end = data.find(b"\n", pos)
    if end < 0:
        raise ValueError("missing line terminator")
    line = data[pos:end]
    if line.endswith(b"\r"):
        line = line[:-1]
    return line, end + 1


def parse_binary(data: bytes):
    pos = 0
    line, pos = read_line(data, pos)
    if line != b"TBT-HTTP-CAPTURE 1":
        raise ValueError("bad magic")
    line, pos = read_line(data, pos)
    if not line.startswith(b"request "):
        raise ValueError("missing request length")
    req_len = int(line.split(b" ", 1)[1])
    req = data[pos : pos + req_len]
    pos += req_len
    line, pos = read_line(data, pos)
    if not line.startswith(b"response "):
        raise ValueError("missing response length")
    resp_len = int(line.split(b" ", 1)[1])
    resp = data[pos : pos + resp_len]
    pos += resp_len
    if data[pos:].strip(b"\r\n \t"):
        raise ValueError("trailing bytes after the response")
    return req, resp


def collect(conn, raw: bytes):
    conn.receive_data(raw)
    first = None
    body = bytearray()
    chunk_starts = 0
    ended = False
    while True:
        event = conn.next_event()
        if event is h11.NEED_DATA:
            break
        if isinstance(event, (h11.Request, h11.Response)):
            if first is not None:
                raise ValueError("more than one message")
            first = event
        elif isinstance(event, h11.Data):
            body += event.data
            if event.chunk_start:
                chunk_starts += 1
        elif isinstance(event, h11.EndOfMessage):
            ended = True
            break
        else:
            raise ValueError(f"unexpected event {event!r}")
    return first, bytes(body), chunk_starts, ended


def check_binary(req: bytes, resp: bytes):
    request, req_body, req_chunks, req_end = collect(h11.Connection(h11.SERVER), req)
    response, resp_body, resp_chunks, resp_end = collect(h11.Connection(h11.CLIENT), resp)
    if not isinstance(request, h11.Request):
        return fail("binary: no Request event")
    if not isinstance(response, h11.Response):
        return fail("binary: no Response event")
    if request.method != b"POST" or request.target != b"/api/v2/upload":
        return fail("binary: wrong request line")
    if response.status_code != 200:
        return fail(f"binary: status {response.status_code}")
    if not req_end or not resp_end:
        return fail("binary: missing EndOfMessage")
    if req_chunks < 2 or resp_chunks < 2:
        return fail(f"binary: chunk counts {req_chunks}/{resp_chunks} < 2")
    print(
        f"h11: PASS binary request: POST /api/v2/upload, {req_chunks} chunks, "
        f"{len(req_body)} body bytes, EndOfMessage"
    )
    print(
        f"h11: PASS binary response: 200, {resp_chunks} chunks, "
        f"{len(resp_body)} body bytes, EndOfMessage"
    )
    return 0


def check_text(req: bytes, resp, first):
    request, req_body, req_chunks, req_end = collect(
        h11.Connection(h11.SERVER), req + (first or b"")
    )
    if not isinstance(request, h11.Request):
        return fail("text: no Request event")
    if request.method != b"POST" or request.target != b"/api/v2/upload":
        return fail("text: wrong request line")
    if req_end:
        return fail("text: unexpected EndOfMessage in a header-only snapshot")
    print(
        f"h11: PASS text request: headers accepted "
        f"({req_chunks} chunk start(s), {len(req_body)} body bytes)"
    )
    if resp is not None:
        response, resp_body, resp_chunks, resp_end = collect(
            h11.Connection(h11.CLIENT), resp
        )
        if not isinstance(response, h11.Response):
            return fail("text: no Response event")
        if response.status_code != 200:
            return fail(f"text: status {response.status_code}")
        if resp_end:
            return fail("text: unexpected EndOfMessage in a header-only snapshot")
        print("h11: PASS text response: headers accepted")
    else:
        print("h11: SKIP text response: field missing")
    if first is not None:
        print("h11: SKIP text full body / 2+ chunks: proc snapshot has only first_chunk")
    else:
        print("h11: SKIP text first chunk: field missing")
    return 0


def check_raw(data: bytes):
    header = normalize_raw(data)
    request, body, chunks, ended = collect(h11.Connection(h11.SERVER), header)
    if not isinstance(request, h11.Request):
        return fail("raw: no Request event")
    if request.method != b"POST" or request.target != b"/api/v2/upload":
        return fail("raw: wrong request line")
    if ended:
        return fail("raw: unexpected EndOfMessage in a header-only snapshot")
    print("h11: PASS raw request: headers accepted")
    print("h11: SKIP raw response/body: current proc surface writes request headers only")
    return 0


def main():
    if len(sys.argv) != 2:
        print("usage: test_http_live_h11.py <capture>")
        return 2
    try:
        data = open(sys.argv[1], "rb").read()
        if data.startswith(b"TBT-HTTP-CAPTURE 1"):
            req, resp = parse_binary(data)
            return check_binary(req, resp)
        fields = parse_fields(data)
        if "req" in fields:
            req, resp, first = parse_text(data)
            return check_text(req, resp, first)
        if data.startswith(b"POST "):
            return check_raw(data)
        return fail("capture is neither binary, frozen text, nor a raw request header")
    except (OSError, ValueError) as exc:
        return fail(str(exc))


if __name__ == "__main__":
    sys.exit(main())
def main():
    if len(sys.argv) != 2:
        print("usage: test_http_live_h11.py <capture>")
        return 2
    try:
        data = open(sys.argv[1], "rb").read()
        if data.startswith(b"TBT-HTTP-CAPTURE 1"):
            req, resp = parse_binary(data)
            return check_binary(req, resp)
        req, resp, first = parse_text(data)
        return check_text(req, resp, first)
    except (OSError, ValueError) as exc:
        return fail(str(exc))


if __name__ == "__main__":
    sys.exit(main())
