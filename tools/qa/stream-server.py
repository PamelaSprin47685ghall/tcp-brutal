#!/usr/bin/env python3
"""stream-server.py - endless HTTP download source for the 0.3.3 QA tools.

It listens on a loopback address (default 127.0.0.1:5210, the far target the
tunnel configuration already names) and answers every GET with a body of
Content-Length 1 TiB. A client that runs for N seconds then closes is normal;
the body is larger than any test window.

The tool is deliberately small. It logs one line per accepted connection and
one line when the connection ends, so the caller can judge bytes without
trusting the rate. Writes are blocking; a dead client ends the thread.

usage: stream-server.py [--host 127.0.0.1] [--port 5210] [--seconds 900]
                        [--quiet]
"""

import argparse
import socket
import sys
import threading
import time

BODY_LEN = 1 << 40
CHUNK = b"tbtun-0.3.3-stream\n" * 4096  # 80 KiB, a fixed, harmless pattern
HEADER = (
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: {}\r\n"
    "Connection: close\r\n"
    "\r\n"
).format(BODY_LEN)


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


def serve(conn, peer, seconds, quiet):
    start = time.monotonic()
    sent = 0
    reason = "timeout"
    try:
        read_request(conn)
        conn.settimeout(10.0)
        conn.sendall(HEADER.encode("ascii"))
        while True:
            now = time.monotonic()
            if now - start >= seconds:
                reason = "window"
                break
            # Send a chunk and observe backpressure in the blocking call.
            conn.sendall(CHUNK)
            sent += len(CHUNK)
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
    if not quiet:
        log("END peer=%s:%s bytes=%d elapsed=%.3f reason=%s" %
            (peer[0], peer[1], sent, time.monotonic() - start, reason))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5210)
    ap.add_argument("--seconds", type=float, default=900.0,
                    help="per-connection send window (default 900)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(16)
    if not args.quiet:
        log("LISTEN %s:%d seconds=%g" % (args.host, args.port, args.seconds))

    try:
        while True:
            conn, peer = srv.accept()
            if not args.quiet:
                log("ACCEPT peer=%s:%s" % (peer[0], peer[1]))
            t = threading.Thread(target=serve,
                                 args=(conn, peer, args.seconds, args.quiet),
                                 daemon=True)
            t.start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
