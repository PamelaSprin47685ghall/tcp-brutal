#!/usr/bin/env python3
"""multiport-target.py - three deterministic HTTP origins for the 0.5.0
multi-port acceptance test.

Each port in --ports serves one fixed body whose bytes derive from the service
name, so the three sha256 digests differ. At startup the server writes
ready (port service sha256 bytes) and sha-<service>.txt. Every accepted
connection appends one line to conns-<service>.log, so the near side can
prove which origin answered and that a refused mapping never dialled its port.

A request line containing slow=1 streams the body with a short pause between
chunks, which keeps a concurrent round alive long enough to observe several
streams in /proc/net/tbtun/stats. The bytes are identical either way.

usage: multiport-target.py --dir DIR [--ports 5210,5211,5212]
                           [--bytes 1048576]
"""

import argparse
import hashlib
import os
import socket
import threading
import time

SERVICES = ("svc0", "svc1", "svc2")
CHUNK = 64 * 1024
SLOW_SECS = 0.3


def payload(service, nbytes):
    """Deterministic bytes, distinct per service."""
    seed = ("tbtun-multiport:" + service).encode("ascii")
    return hashlib.shake_128(seed).digest(nbytes)


def serve_one(lsock, service, port, body, outdir):
    while True:
        try:
            conn, peer = lsock.accept()
        except OSError:
            return
        try:
            # Log at accept, before any request bytes: a dial is the event
            # that matters for the refused-mapping check, and it must count
            # even if the client never sends a request.
            with open(os.path.join(outdir, "conns-%s.log" % service), "a") as f:
                f.write("%s\n" % peer[0])
            conn.settimeout(5.0)
            request = b""
            while b"\r\n\r\n" not in request and len(request) < 16384:
                part = conn.recv(4096)
                if not part:
                    break
                request += part
            line = request.split(b"\r\n", 1)[0]
            slow = b"slow=1" in line
            head = (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %d\r\n"
                "X-TBT-Service: %s\r\n"
                "X-TBT-Port: %d\r\n"
                "Connection: close\r\n"
                "\r\n" % (len(body), service, port)
            ).encode("ascii")
            conn.sendall(head)
            view = memoryview(body)
            off = 0
            while off < len(body):
                off += conn.send(view[off:off + CHUNK])
                if slow:
                    time.sleep(SLOW_SECS)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def listen(port):
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("127.0.0.1", port))
    lsock.listen(64)
    return lsock


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--ports", default="5210,5211,5212")
    ap.add_argument("--bytes", type=int, default=1048576)
    args = ap.parse_args()
    if args.bytes <= 0:
        raise SystemExit("--bytes must be positive")
    ports = [int(p) for p in args.ports.split(",") if p]
    if len(ports) != len(SERVICES):
        raise SystemExit("--ports needs exactly %d values" % len(SERVICES))
    os.makedirs(args.dir, exist_ok=True)

    servers = []
    for service, port in zip(SERVICES, ports):
        body = payload(service, args.bytes)
        digest = hashlib.sha256(body).hexdigest()
        with open(os.path.join(args.dir, "sha-%s.txt" % service), "w") as f:
            f.write(digest + "\n")
        servers.append((listen(port), service, port, body, digest))

    tmp = os.path.join(args.dir, "ready.tmp")
    with open(tmp, "w") as f:
        for _, service, port, _, digest in servers:
            f.write("%d %s %s %d\n" % (port, service, digest, args.bytes))
    os.replace(tmp, os.path.join(args.dir, "ready"))

    for lsock, service, port, body, _ in servers:
        threading.Thread(target=serve_one,
                         args=(lsock, service, port, body, args.dir),
                         daemon=True).start()
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
