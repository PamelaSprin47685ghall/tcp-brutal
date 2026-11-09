#!/usr/bin/env python3
"""multistream-target.py - two-path HTTP source for the v7 multi-stream test.

It listens on the far tunnel's target port and answers
`GET /stream/a` and `GET /stream/b` with different deterministic bodies.
Two connections can run at once; each body is exactly --bytes long (or runs
for --seconds). The sender hashes what it actually wrote and leaves the
expected sha256/byte count in --outdir, so the near side can prove that each
application stream received its own body and not the other stream's.

usage: multistream-target.py [--host 127.0.0.1] [--port 5210]
                             [--bytes 1073741824] [--seconds 0]
                             [--outdir /tmp]
"""

import argparse
import hashlib
import os
import socket
import struct
import sys
import threading
import time

CHUNK = 80 * 1024


def log(msg):
    print("%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), msg),
          flush=True)


def read_request(conn):
    data = b""
    conn.settimeout(5.0)
    while b"\r\n\r\n" not in data and len(data) < 16384:
        part = conn.recv(4096)
        if not part:
            break
        data += part
    return data


def label_of(request):
    first = request.split(b"\r\n", 1)[0]
    parts = first.split()
    path = parts[1] if len(parts) >= 2 else b""
    if path == b"/stream/a":
        return "a"
    if path == b"/stream/b":
        return "b"
    return None


def body_chunk(label, seq):
    """A deterministic, label-distinct 80 KiB block."""
    head = ("tbtun-ms-%s-%016x\n" % (label, seq)).encode("ascii")
    return (head * (CHUNK // len(head) + 1))[:CHUNK]


def serve(conn, peer, args, outdir):
    request = read_request(conn)
    label = label_of(request)
    if label is None:
        log("REJECT peer=%s:%s request=%r" % (peer[0], peer[1], request[:80]))
        conn.close()
        return

    header = (
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n" % args.bytes
    ).encode("ascii")
    digest = hashlib.sha256()
    sent = 0
    seq = 0
    start = time.monotonic()
    reason = "bytes"
    try:
        conn.settimeout(30.0)
        conn.sendall(header)
        while sent < args.bytes:
            if args.seconds > 0 and time.monotonic() - start >= args.seconds:
                reason = "window"
                break
            take = min(CHUNK, args.bytes - sent)
            data = body_chunk(label, seq)[:take]
            conn.sendall(data)
            digest.update(data)
            sent += take
            seq += 1
    except (BrokenPipeError, ConnectionResetError, socket.timeout):
        reason = "client"
    except OSError as exc:
        reason = "error:%s" % exc
    finally:
        try:
            conn.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        conn.close()

    sha = digest.hexdigest()
    with open(os.path.join(outdir, "sha-%s.txt" % label), "w") as fh:
        fh.write(sha + "\n")
    with open(os.path.join(outdir, "bytes-%s.txt" % label), "w") as fh:
        fh.write("%d\n" % sent)
    log("END path=/stream/%s peer=%s:%s bytes=%d sha256=%s reason=%s" %
        (label, peer[0], peer[1], sent, sha, reason))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5210)
    ap.add_argument("--bytes", type=int, default=1 << 30)
    ap.add_argument("--seconds", type=float, default=0.0)
    ap.add_argument("--outdir", default="/tmp")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    log("LISTEN %s:%d bytes=%d seconds=%g outdir=%s" %
        (args.host, args.port, args.bytes, args.seconds, args.outdir))
    try:
        while True:
            conn, peer = srv.accept()
            log("ACCEPT peer=%s:%s" % (peer[0], peer[1]))
            threading.Thread(target=serve,
                             args=(conn, peer, args, args.outdir),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
