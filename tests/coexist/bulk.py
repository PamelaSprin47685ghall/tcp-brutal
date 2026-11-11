#!/usr/bin/env python3
"""TCP bulk helpers for tests/coexist/coexist.sh.

Four roles, all plain TCP:

  sink PORT                 accept and drain forever
  send HOST PORT SECS       connect, blast for SECS, print bytes written
  src PORT SECS             listen on 127.0.0.1, accept one, blast for SECS
  recv HOST PORT SECS       connect, drain for SECS, print bytes received

The stock-TCP baseline and the coexistence run use `send`; the tunnel run
uses `src` behind the far end and `recv` at the near end. The sender counts
bytes handed to the socket, which is the application-level throughput the
measurement needs.
"""
import socket
import sys
import threading
import time

CHUNK = 1 << 16


def _serve(sock, handler):
    while True:
        conn, _ = sock.accept()
        threading.Thread(target=handler, args=(conn,), daemon=True).start()


def _drain(conn):
    with conn:
        while conn.recv(CHUNK):
            pass


def sink(port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", int(port)))
    srv.listen(16)
    _serve(srv, _drain)


def _blast(conn, secs):
    conn.settimeout(1.0)
    chunk = b"x" * CHUNK
    total = 0
    end = time.monotonic() + float(secs)
    while time.monotonic() < end:
        try:
            total += conn.send(chunk)
        except (socket.timeout, BlockingIOError):
            pass
    return total


def send(host, port, secs):
    conn = socket.create_connection((host, int(port)), timeout=5)
    total = _blast(conn, secs)
    conn.close()
    print(total)


def src(port, secs):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", int(port)))
    srv.listen(1)
    conn, _ = srv.accept()
    total = _blast(conn, secs)
    conn.close()
    srv.close()
    print(total)


def recv(host, port, secs):
    conn = socket.create_connection((host, int(port)), timeout=5)
    conn.settimeout(1.0)
    total = 0
    end = time.monotonic() + float(secs)
    while time.monotonic() < end:
        try:
            buf = conn.recv(CHUNK)
        except socket.timeout:
            continue
        if not buf:
            break
        total += len(buf)
    conn.close()
    print(total)


def main(argv):
    roles = {"sink": sink, "send": send, "src": src, "recv": recv}
    if len(argv) < 2 or argv[1] not in roles:
        print(__doc__, file=sys.stderr)
        return 2
    roles[argv[1]](*argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
