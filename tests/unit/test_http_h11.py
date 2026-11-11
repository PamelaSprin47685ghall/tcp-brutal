#!/usr/bin/env python3
"""test_http_h11.py - parse the emitted carrier messages with a real HTTP parser.

`tests/unit/test_http --emit-req` writes the request block and `--emit-resp` the
response block, each followed by a two-chunk body. (`--emit` writes both
back-to-back for a human or QEMU capture.) All three use the same tbt_http.c
functions the module calls. This script feeds the direction-specific bytes to
h11, so the
emission is checked by a third-party strict parser and not only by the test's
own grammar.

The Makefile runs this through `uv run --offline --with h11` and skips it with
a printed note when uv or a cached h11 is unavailable; h11 is not a build
dependency and nothing here needs network access. h11 0.16.0 was the version
used to validate the wire samples.
"""
import subprocess
import sys

import h11

EXPECTED_REQUEST_HEADERS = [
    (b"Host", b"203.0.113.9:8443"),
    (
        b"User-Agent",
        b"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        b"(KHTML, like Gecko) Chrome/131.0 Safari/537.36",
    ),
    (b"Accept", b"*/*"),
    (b"Accept-Encoding", b"identity"),
    (b"Content-Type", b"application/gzip"),
    (b"Transfer-Encoding", b"chunked"),
    (b"Connection", b"keep-alive"),
    (b"X-Request-Id", b"abcd234567abcdefghijklmnop"),
]
EXPECTED_RESPONSE_HEADERS = [
    (b"Content-Type", b"application/gzip"),
    (b"Transfer-Encoding", b"chunked"),
    (b"Connection", b"keep-alive"),
    (b"Server", b"nginx"),
    (b"Date", b"Tue, 14 Nov 2023 22:13:20 GMT"),
]
EXPECTED_BODY_LEN = 112  # PAD 32 + drip 1 + DATA10 42 + DATA5 37
EXPECTED_CHUNKS = 4


def fail(message):
    print(f"FAIL h11: {message}")
    return 1


def run(exe, mode):
    try:
        return subprocess.run(
            [exe, mode], check=True, stdout=subprocess.PIPE
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"cannot run {exe} {mode}: {exc}") from exc


def collect(conn, raw):
    """Feed raw bytes and return (first message event, body, chunk starts, end)."""
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
                return None, body, chunk_starts, False, "more than one message"
            first = event
        elif isinstance(event, h11.Data):
            body += event.data
            if event.chunk_start:
                chunk_starts += 1
        elif isinstance(event, h11.EndOfMessage):
            if event.headers.raw_items():
                return first, body, chunk_starts, False, "unexpected trailers"
            ended = True
            break
        else:
            return first, body, chunk_starts, False, f"unexpected event {event!r}"

    return first, body, chunk_starts, ended, None


def check_request(raw):
    if b"tbtun" in raw.lower():
        return fail("request bytes contain 'tbtun'")
    conn = h11.Connection(h11.SERVER)
    request, body, chunk_starts, ended, error = collect(conn, raw)
    if error:
        return fail(f"request: {error}")
    if request is None:
        return fail("request: h11 produced no Request event")
    if request.method != b"POST":
        return fail(f"request: method is {request.method!r}")
    if request.target != b"/api/v2/upload":
        return fail(f"request: target is {request.target!r}")
    if request.http_version != b"1.1":
        return fail(f"request: version is {request.http_version!r}")
    if request.headers.raw_items() != EXPECTED_REQUEST_HEADERS:
        return fail(f"request headers differ: {request.headers.raw_items()!r}")
    if len(body) != EXPECTED_BODY_LEN:
        return fail(f"request body length {len(body)} != {EXPECTED_BODY_LEN}")
    if chunk_starts != EXPECTED_CHUNKS:
        return fail(f"request: expected {EXPECTED_CHUNKS} chunk starts, saw {chunk_starts}")
    if not ended:
        return fail("request: no EndOfMessage")
    return 0


def check_response(raw):
    if b"tbtun" in raw.lower():
        return fail("response bytes contain 'tbtun'")
    conn = h11.Connection(h11.CLIENT)
    response, body, chunk_starts, ended, error = collect(conn, raw)
    if error:
        return fail(f"response: {error}")
    if response is None:
        return fail("response: h11 produced no Response event")
    if response.status_code != 200:
        return fail(f"response: status is {response.status_code}")
    if response.http_version != b"1.1":
        return fail(f"response: version is {response.http_version!r}")
    if response.headers.raw_items() != EXPECTED_RESPONSE_HEADERS:
        return fail(f"response headers differ: {response.headers.raw_items()!r}")
    if len(body) != EXPECTED_BODY_LEN:
        return fail(f"response body length {len(body)} != {EXPECTED_BODY_LEN}")
    if chunk_starts != EXPECTED_CHUNKS:
        return fail(f"response: expected {EXPECTED_CHUNKS} chunk starts, saw {chunk_starts}")
    if not ended:
        return fail("response: no EndOfMessage")
    return 0


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./tests/unit/test_http"
    try:
        request_raw = run(exe, "--emit-req")
        response_raw = run(exe, "--emit-resp")
    except RuntimeError as exc:
        return fail(str(exc))

    if check_request(request_raw) or check_response(response_raw):
        return 1

    print(
        "h11: accepted POST /api/v2/upload HTTP/1.1 and "
        f"HTTP/1.1 200 OK; {EXPECTED_CHUNKS} chunks and {EXPECTED_BODY_LEN} body bytes each, "
        "headers in order"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
