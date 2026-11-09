#!/usr/bin/env python3
"""alloc-rebalance-target.py - four-path source for the ALLOC acceptance.

The far tunnel forwards one TCP stream per tunnel stream to this listener.
Each path carries a deterministic, label-distinct body:

  /stream/a  A paced, open-ended body. It stops only when the test writes
             <outdir>/release-a, then ends the chunked body.
  /stream/b  A fixed --bytes-b body, after which the chunked stream is held
  /stream/c  open (no terminating 0-chunk) until <outdir>/release-<label>.
  /stream/d  The hold keeps the tunnel stream alive across the four-flow
             split even after its body is complete.

The sender hashes the body bytes it actually wrote; sha-<label>.txt and
bytes-<label>.txt let the near side prove byte-for-byte equality. A request
body is drained before the response starts and hashed into up-sha-<label>.txt
and up-bytes-<label>.txt, so the near TX direction is checked too.
sent-<label> marks a fixed body completed, so the test never releases a
stream mid-body.

usage: alloc-rebalance-target.py [--host 127.0.0.1] [--port 5210]
                                 [--outdir /tmp] [--bytes-b N] ...
                                 [--bytes-c N] [--bytes-d N]
                                 [--pace-a BYTES_PER_SEC]
                                 [--hold-timeout S]
"""

import argparse
import hashlib
import os
import socket
import sys
import threading
import time

CHUNK = 64 * 1024
LABELS = ("a", "b", "c", "d")


def log(msg):
    print("%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), msg),
          flush=True)


def read_request(conn):
    data = b""
    conn.settimeout(10.0)
    while b"\r\n\r\n" not in data and len(data) < 16384:
        part = conn.recv(4096)
        if not part:
            break
        data += part
    return data


def content_length(header):
    for line in header.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            try:
                return int(line.split(b":", 1)[1].strip())
            except ValueError:
                return 0
    return 0


def read_uplink(conn, initial, need, label, outdir):
    """Hash exactly `need` request-body bytes: the near TX direction."""
    digest = hashlib.sha256()
    got = 0
    try:
        if initial:
            take = initial[:need]
            digest.update(take)
            got = len(take)
        while got < need:
            part = conn.recv(min(65536, need - got))
            if not part:
                break
            digest.update(part)
            got += len(part)
    except (socket.timeout, OSError):
        pass
    with open(os.path.join(outdir, "up-sha-%s.txt" % label), "w") as fh:
        fh.write(digest.hexdigest() + "\n")
    with open(os.path.join(outdir, "up-bytes-%s.txt" % label), "w") as fh:
        fh.write("%d\n" % got)
    log("UP path=/stream/%s bytes=%d sha256=%s" %
        (label, got, digest.hexdigest()))


def label_of(request):
    first = request.split(b"\r\n", 1)[0]
    parts = first.split()
    if len(parts) < 2:
        return None
    for label in LABELS:
        if parts[1] == ("/stream/%s" % label).encode("ascii"):
            return label
    return None


def body_chunk(label, seq):
    """A deterministic, label-distinct 64 KiB block."""
    head = ("tbtun-alloc-%s-%016x\n" % (label, seq)).encode("ascii")
    return (head * (CHUNK // len(head) + 1))[:CHUNK]


def send_chunk(conn, data):
    conn.sendall(b"%x\r\n" % len(data) + data + b"\r\n")


def send_end(conn):
    conn.sendall(b"0\r\n\r\n")


def pace(start, sent, rate):
    if rate <= 0:
        return
    ahead = sent / rate - (time.monotonic() - start)
    if ahead > 0:
        time.sleep(ahead)


def released(outdir, label):
    return os.path.exists(os.path.join(outdir, "release-%s" % label))


def wait_release(outdir, label, deadline):
    while not released(outdir, label):
        if time.monotonic() >= deadline:
            return "timeout"
        time.sleep(0.2)
    return "release"


def write_result(outdir, label, sent, digest, reason):
    with open(os.path.join(outdir, "sha-%s.txt" % label), "w") as fh:
        fh.write(digest.hexdigest() + "\n")
    with open(os.path.join(outdir, "bytes-%s.txt" % label), "w") as fh:
        fh.write("%d\n" % sent)
    log("END path=/stream/%s bytes=%d sha256=%s reason=%s" %
        (label, sent, digest.hexdigest(), reason))


def serve(conn, peer, args):
    label = None
    digest = hashlib.sha256()
    sent = 0
    seq = 0
    reason = "bytes"
    try:
        request = read_request(conn)
        cut = request.find(b"\r\n\r\n")
        header = request[:cut + 4] if cut >= 0 else request
        initial = request[cut + 4:] if cut >= 0 else b""
        label = label_of(header)
        if label is None:
            log("REJECT peer=%s:%s request=%r" % (peer[0], peer[1],
                                                  request[:80]))
            return
        need = content_length(header)
        if need:
            # Read the request body before the response: a 200 sent early
            # may make the client stop uploading, and the near TX direction
            # must be hashed in full.
            conn.settimeout(120.0)
            read_uplink(conn, initial, need, label, args.outdir)
        header = (
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "Server: nginx\r\n"
            "\r\n"
        ).encode("ascii")
        conn.settimeout(30.0)
        conn.sendall(header)
        start = time.monotonic()
        deadline = start + args.hold_timeout

        if label == "a":
            while not released(args.outdir, "a"):
                data = body_chunk(label, seq)
                send_chunk(conn, data)
                digest.update(data)
                sent += len(data)
                seq += 1
                pace(start, sent, args.pace_a)
            reason = "release"
        else:
            total = getattr(args, "bytes_" + label)
            rate = getattr(args, "pace_" + label)
            while sent < total:
                take = min(CHUNK, total - sent)
                data = body_chunk(label, seq)[:take]
                send_chunk(conn, data)
                digest.update(data)
                sent += take
                seq += 1
                pace(start, sent, rate)
            with open(os.path.join(args.outdir, "sent-%s" % label), "w") as fh:
                fh.write("%d\n" % sent)
            reason = wait_release(args.outdir, label, deadline)
        send_end(conn)
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
        if label is not None:
            write_result(args.outdir, label, sent, digest, reason)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5210)
    ap.add_argument("--outdir", default="/tmp")
    ap.add_argument("--bytes-b", type=int, default=64 << 20)
    ap.add_argument("--bytes-c", type=int, default=64 << 20)
    ap.add_argument("--bytes-d", type=int, default=64 << 20)
    ap.add_argument("--pace-a", type=float, default=2 << 20)
    ap.add_argument("--pace-b", type=float, default=0.0)
    ap.add_argument("--pace-c", type=float, default=0.0)
    ap.add_argument("--pace-d", type=float, default=0.0)
    ap.add_argument("--hold-timeout", type=float, default=900.0)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    log("LISTEN %s:%d outdir=%s bytes=%d/%d/%d pace-a=%g" %
        (args.host, args.port, args.outdir, args.bytes_b, args.bytes_c,
         args.bytes_d, args.pace_a))
    try:
        while True:
            conn, peer = srv.accept()
            log("ACCEPT peer=%s:%s" % (peer[0], peer[1]))
            threading.Thread(target=serve, args=(conn, peer, args),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
