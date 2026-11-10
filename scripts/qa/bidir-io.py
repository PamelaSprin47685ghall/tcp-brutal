#!/usr/bin/env python3
"""bidir-io.py - bounded HTTP data mover for the 0.7.0 QA scripts.

Two subcommands speak the one container tbtun carries (plain HTTP/1.1 with
Content-Length) so the same tool drives both tunnel directions:

  serve   the far target. GET /qa/dl/<n> sends exactly n bytes of a 1 MiB
          block; PUT /qa/ul/<n> reads exactly n bytes. Every connection
          hashes what it moved and appends one result line to
          <result-dir>/server-<role>.txt. Writes <ready-file> once listening.
  client  the near application. Moves exactly n bytes and prints one result
          line: role=..., bytes=..., sha256=..., secs=..., bps=...

The byte count is fixed by the request, never by wall-clock time, so the
sender's hash and the receiver's hash cover the same range. --seconds is an
optional cutoff for load-only runs (memory soak); a cut transfer reports
truncated=1 and is not used for an integrity verdict. --no-hash skips the
hash when the run measures memory, not bytes.

Result line: role=dl|ul bytes=<n> sha256=<hex|-> secs=<float> bps=<float>
             truncated=0|1
"""

import argparse
import hashlib
import os
import socket
import sys
import threading
import time
import urllib.parse

CHUNK = 1 << 20
IDLE_TIMEOUT = 60.0
BLOCK = os.urandom(CHUNK)
RESULT_LOCK = threading.Lock()


def stamp():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def log(msg):
    print("%s %s" % (stamp(), msg), flush=True)


def result_line(role, nbytes, digest, secs, truncated):
    bps = int(nbytes / secs) if secs > 0 else 0
    return ("role=%s bytes=%d sha256=%s secs=%.3f bps=%d truncated=%d"
            % (role, nbytes, digest if digest else "-", secs, bps,
               1 if truncated else 0))


def append_result(result_dir, role, line):
    path = os.path.join(result_dir, "server-%s.txt" % role)
    with RESULT_LOCK:
        with open(path, "a") as f:
            f.write(line + "\n")
            f.flush()


def read_head(conn, limit=65536):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < limit:
        part = conn.recv(4096)
        if not part:
            break
        data += part
    head, _, rest = data.partition(b"\r\n\r\n")
    return head, rest


def header_value(head, name):
    want = name.lower().encode() + b":"
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(want):
            return line.split(b":", 1)[1].strip()
    return None


def role_and_bytes(head):
    parts = head.split(b"\r\n", 1)[0].split()
    if len(parts) < 2:
        return None, 0
    method = parts[0]
    path = parts[1].decode("latin1", "replace")
    if path.startswith("http://"):
        path = urllib.parse.urlsplit(path).path
    role = None
    if path.startswith("/qa/dl/"):
        role = "dl"
    elif path.startswith("/qa/ul/"):
        role = "ul"
    try:
        nbytes = int(path.rsplit("/", 1)[1])
    except (IndexError, ValueError):
        return None, 0
    if nbytes < 0:
        return None, 0
    return (role, nbytes) if method in (b"GET", b"PUT") else (None, 0)


def serve_one(conn, peer, args):
    role = None
    moved = 0
    truncated = True
    hasher = None if args.no_hash else hashlib.sha256()
    t0 = time.monotonic()
    try:
        conn.settimeout(IDLE_TIMEOUT)
        head, rest = read_head(conn)
        role, want = role_and_bytes(head)
        if role is None:
            body = b"400 bad request\n"
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: %d\r\n"
                         b"Connection: close\r\n\r\n" % len(body) + body)
            return
        if role == "dl":
            hdr = ("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n" % want)
            conn.sendall(hdr.encode("ascii"))
            left = want
            while left:
                n = min(left, CHUNK)
                conn.sendall(BLOCK if n == CHUNK else BLOCK[:n])
                if hasher:
                    hasher.update(BLOCK if n == CHUNK else BLOCK[:n])
                moved += n
                left -= n
            truncated = False
        else:
            left = want
            if rest:
                part = rest[:left]
                if hasher:
                    hasher.update(part)
                moved += len(part)
                left -= len(part)
            while left:
                part = conn.recv(min(left, CHUNK))
                if not part:
                    break
                if hasher:
                    hasher.update(part)
                moved += len(part)
                left -= len(part)
            truncated = left != 0
            body = result_line("ul", moved, hasher.hexdigest() if hasher else "",
                               time.monotonic() - t0, truncated).encode()
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                         b"Connection: close\r\n\r\n" % len(body) + body)
    except (BrokenPipeError, ConnectionResetError, socket.timeout) as exc:
        log("serve %s:%s role=%s: %s" % (peer[0], peer[1], role, exc))
    finally:
        try:
            conn.close()
        except OSError:
            pass
        if role is not None:
            secs = time.monotonic() - t0
            line = result_line(role, moved, hasher.hexdigest() if hasher else "",
                               secs, truncated)
            append_result(args.result_dir, role,
                          line + " part=%s:%d" % (peer[0], peer[1]))
            log("serve peer=%s:%s %s" % (peer[0], peer[1], line))


def do_serve(args):
    os.makedirs(args.result_dir, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(16)
    if args.ready_file:
        with open(args.ready_file, "w") as f:
            f.write("listen %s:%d\n" % (args.bind, args.port))
    log("listen %s:%d result-dir=%s" % (args.bind, args.port, args.result_dir))
    while True:
        conn, peer = srv.accept()
        threading.Thread(target=serve_one, args=(conn, peer, args),
                         daemon=True).start()


def connect(args):
    last = None
    for _ in range(50):
        try:
            return socket.create_connection((args.host, args.port), timeout=30)
        except OSError as exc:
            last = exc
            time.sleep(0.2)
    raise SystemExit("connect %s:%d failed: %s" % (args.host, args.port, last))


def read_response_head(sock):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < 65536:
        part = sock.recv(4096)
        if not part:
            break
        data += part
    head, _, rest = data.partition(b"\r\n\r\n")
    return head, rest


def do_client(args):
    role = args.role
    want = args.bytes
    if want <= 0:
        raise SystemExit("--bytes must be positive")
    path = "/qa/%s/%d" % (role, want)
    hostport = "%s:%d" % (args.host, args.port)
    sock = connect(args)
    sock.settimeout(IDLE_TIMEOUT)
    hasher = None if args.no_hash else hashlib.sha256()
    moved = 0
    truncated = False
    deadline = time.monotonic() + args.seconds if args.seconds > 0 else None
    t0 = time.monotonic()
    try:
        if role == "dl":
            req = ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
                   % (path, hostport)).encode("ascii")
            sock.sendall(req)
            head, rest = read_response_head(sock)
            status = head.split(b"\r\n", 1)[0]
            if b" 200 " not in status + b" ":
                raise SystemExit("bad response: %s" % status.decode("latin1"))
            clen = header_value(head, "content-length")
            if clen is None or int(clen) != want:
                raise SystemExit("content-length %r != %d" % (clen, want))
            buf = rest[:want]
            moved = len(buf)
            if hasher:
                hasher.update(buf)
            while moved < want:
                if deadline and time.monotonic() >= deadline:
                    truncated = True
                    break
                part = sock.recv(min(CHUNK, want - moved))
                if not part:
                    break
                moved += len(part)
                if hasher:
                    hasher.update(part)
            if moved < want:
                truncated = True
        else:
            req = ("PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %d\r\n"
                   "Connection: close\r\n\r\n"
                   % (path, hostport, want)).encode("ascii")
            sock.sendall(req)
            left = want
            while left:
                if deadline and time.monotonic() >= deadline:
                    truncated = True
                    break
                n = min(left, CHUNK)
                block = BLOCK if n == CHUNK else BLOCK[:n]
                sock.sendall(block)
                if hasher:
                    hasher.update(block)
                moved += n
                left -= n
            truncated = truncated or left != 0
            sock.shutdown(socket.SHUT_WR)
            read_response_head(sock)
    finally:
        try:
            sock.close()
        except OSError:
            pass
    secs = time.monotonic() - t0
    print(result_line(role, moved, hasher.hexdigest() if hasher else "",
                      secs, truncated), flush=True)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    srv = sub.add_parser("serve")
    srv.add_argument("--bind", default="127.0.0.1")
    srv.add_argument("--port", type=int, required=True)
    srv.add_argument("--result-dir", required=True)
    srv.add_argument("--ready-file", default="")
    srv.add_argument("--no-hash", action="store_true")
    srv.set_defaults(func=do_serve)

    cli = sub.add_parser("client")
    cli.add_argument("--host", default="127.0.0.1")
    cli.add_argument("--port", type=int, required=True)
    cli.add_argument("--role", choices=("dl", "ul"), required=True)
    cli.add_argument("--bytes", type=int, required=True)
    cli.add_argument("--seconds", type=float, default=0)
    cli.add_argument("--no-hash", action="store_true")
    cli.set_defaults(func=do_client)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
