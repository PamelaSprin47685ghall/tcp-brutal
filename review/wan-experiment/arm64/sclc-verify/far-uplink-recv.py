#!/usr/bin/env python3
"""Loopback-only HTTP receiver for the 0.3.3 uplink 1 GiB acceptance.

The far side of tbtun re-originates the tunnel's local request to
127.0.0.1:5210.  This process is that target for the uplink run: it reads a
Content-Length POST/PUT body and writes it to --out, then answers 200.  The
near side later compares sha256(near source) with sha256(far --out).

Deliberately narrow:

    HEAD       -> 200, no body (readiness)
    POST/PUT   -> stream the body to <out>.part, fsync, rename to <out>
    anything   -> 405

No application read timeout is set.  The body is written to a .part file and
renamed only after the full Content-Length arrived, so a short body remains
visible as evidence instead of being published as a valid result.  One
connection at a time; the verifier sends exactly one POST.

This is a verification helper, not product code.  It never listens on a
non-loopback address unless --bind says so.
"""

import argparse
import os
import re
import signal
import socket
import time

CHUNK = 1 << 20
HEAD_LIMIT = 65536
STOP = False


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def log(path, line):
    try:
        with open(path, "a") as f:
            f.write(line + "\n")
            f.flush()
    except OSError:
        pass


def read_head(conn):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < HEAD_LIMIT:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def reply(conn, code, reason, body=b""):
    conn.sendall(
        ("HTTP/1.1 %d %s\r\n"
         "Content-Length: %d\r\n"
         "Connection: close\r\n\r\n" % (code, reason, len(body))).encode()
        + body)
    try:
        conn.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def headers_of(head):
    out = {}
    lines = head.split(b"\r\n")
    request = lines[0].decode("latin1", "replace") if lines else ""
    for line in lines[1:]:
        if b":" not in line:
            continue
        key, value = line.split(b":", 1)
        out[key.strip().lower()] = value.strip()
    return request, out


def handle_post(conn, request, headers, raw_body_prefix, args):
    """Stream one request body.  Return (received, expected, result)."""
    match = re.search(rb"(\d+)", headers.get(b"content-length", b""))
    if not match:
        reply(conn, 411, "Length Required", b"Content-Length required\n")
        return 0, 0, "no_length"
    expected = int(match.group(1))
    if expected > args.max_bytes:
        reply(conn, 413, "Payload Too Large",
              ("max_bytes=%d\n" % args.max_bytes).encode())
        return 0, expected, "too_large"

    if b"100-continue" in headers.get(b"expect", b"").lower():
        conn.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")

    part = args.out + ".part"
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    received = min(len(raw_body_prefix), expected)
    with open(part, "wb") as f:
        if received:
            f.write(raw_body_prefix[:received])
        remaining = expected - received
        while remaining > 0:
            chunk = conn.recv(min(CHUNK, remaining))
            if not chunk:
                break
            f.write(chunk)
            received += len(chunk)
            remaining -= len(chunk)
        f.flush()
        os.fsync(f.fileno())

    if received == expected:
        os.replace(part, args.out)
        result = "ok"
        reply(conn, 200, "OK", ("recv=%d\n" % received).encode())
    else:
        result = "short"
        reply(conn, 400, "Bad Request", ("recv=%d expected=%d\n"
                                         % (received, expected)).encode())
    return received, expected, result


def main():
    global STOP
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5210)
    ap.add_argument("--out", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--pidfile", required=True)
    ap.add_argument("--max-bytes", type=int, default=2 << 30)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    srv.settimeout(1.0)

    with open(args.pidfile, "w") as f:
        f.write("%d\n" % os.getpid())

    def stop(_signum, _frame):
        global STOP
        STOP = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    log(args.log, "%s listen %s:%d out=%s max_bytes=%d pid=%d"
        % (now(), args.bind, args.port, args.out, args.max_bytes, os.getpid()))

    while not STOP:
        try:
            conn, _addr = srv.accept()
        except socket.timeout:
            continue
        except OSError as exc:
            if STOP:
                break
            log(args.log, "%s accept error: %s" % (now(), exc))
            continue
        conn.settimeout(None)
        t0 = time.time()
        try:
            raw = read_head(conn)
            head, _sep, prefix = raw.partition(b"\r\n\r\n")
            request, headers = headers_of(head)
            method = request.split(" ", 1)[0] if request else ""
            if method in ("POST", "PUT"):
                received, expected, result = handle_post(
                    conn, request, headers, prefix, args)
                log(args.log, "%s %s recv=%d expected=%d result=%s elapsed=%.3f out=%s"
                    % (now(), request, received, expected, result,
                       time.time() - t0, args.out))
                continue
            if method == "HEAD":
                reply(conn, 200, "OK")
                log(args.log, "%s %s -> 200" % (now(), request))
                continue
            reply(conn, 405, "Method Not Allowed", b"POST/PUT/HEAD only\n")
            log(args.log, "%s %s -> 405" % (now(), request))
        except (BrokenPipeError, ConnectionResetError) as exc:
            log(args.log, "%s client gone elapsed=%.3f: %s"
                % (now(), time.time() - t0, exc))
        except OSError as exc:
            log(args.log, "%s error elapsed=%.3f: %s"
                % (now(), time.time() - t0, exc))
        finally:
            try:
                conn.close()
            except OSError:
                pass

    srv.close()
    try:
        os.unlink(args.pidfile)
    except OSError:
        pass
    log(args.log, "%s stop pid=%d" % (now(), os.getpid()))


if __name__ == "__main__":
    main()
