#!/usr/bin/env python3
"""HTTP/1.1 throughput server for the arm64 T5.2 acceptance baseline.

Runs on the far side (HND0). The near side runs 8 parallel ``http_client.py``
processes, one keep-alive HTTP/1.1 connection each, and reports application
level Mbit/s. This server replaces iperf3: it measures plain TCP plus HTTP
framing, nothing else.

Endpoints
---------
POST /upload              read ``Content-Length`` bytes, discard, 200 OK
GET  /download?bytes=N    write N bytes, 200 OK, keep-alive
GET  /health              JSON counters, used as a readiness probe

Counters are written atomically to ``--stats-out`` every ``--stats-interval``
seconds and on SIGTERM/SIGINT, so a ``systemctl stop`` leaves a final snapshot.

Usage
-----
    python3 http_server.py --port 10000 --stats-out /tmp/tbt-http-baseline/stats.json
"""

import argparse
import fcntl
import json
import os
import signal
import socket
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

CHUNK = 1 << 20  # 1 MiB write unit and read unit


def acquire_lock(path):
    """Hold an exclusive flock for the life of the process.

    The fd is deliberately never closed: the kernel releases the lock when the
    process dies, which makes the lock lifetime exactly the server lifetime.
    Exit code 75 means the machine lock is held by someone else.
    """
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        os.close(fd)
        print(
            json.dumps({"event": "lock-busy", "path": path, "error": repr(exc)}),
            file=sys.stderr,
            flush=True,
        )
        raise SystemExit(75)
    line = "pid=%d time=%s purpose=http-baseline-server host=%s\n" % (
        os.getpid(),
        time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        socket.gethostname(),
    )
    try:
        os.write(fd, line.encode())
    except OSError:
        pass
    return fd


class Counters:
    """Mutable request/byte counters shared by all handler threads."""

    def __init__(self):
        self.lock = threading.Lock()
        self.started = time.time()
        self.conns_opened = 0
        self.conns_active = 0
        self.requests = 0
        self.uploads = 0
        self.downloads = 0
        self.health = 0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.errors = 0
        self.client_aborts = 0

    def add(self, **kw):
        with self.lock:
            for key, value in kw.items():
                setattr(self, key, getattr(self, key) + value)

    def snapshot(self):
        with self.lock:
            return {
                "uptime_s": round(time.time() - self.started, 3),
                "conns_opened": self.conns_opened,
                "conns_active": self.conns_active,
                "requests": self.requests,
                "uploads": self.uploads,
                "downloads": self.downloads,
                "health": self.health,
                "rx_bytes": self.rx_bytes,
                "tx_bytes": self.tx_bytes,
                "errors": self.errors,
                "client_aborts": self.client_aborts,
            }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "tbt-baseline-http/1.0"
    disable_nagle_algorithm = True  # setsockopt(TCP_NODELAY) on the accepted socket
    timeout = 300  # a stalled peer must not pin a thread forever

    # -- connection plumbing ------------------------------------------------
    def setup(self):
        super().setup()
        self.server.counters.add(conns_opened=1, conns_active=1)

    def finish(self):
        try:
            super().finish()
        except OSError:
            pass
        finally:
            self.server.counters.add(conns_active=-1)

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write(
                "%s [%s] %s\n" % (time.strftime("%H:%M:%S"), self.address_string(), fmt % args)
            )

    def handle(self):
        # A client that hits its deadline closes mid-request. That is an RST on
        # the next readline, not an error worth a traceback.
        try:
            super().handle()
        except (ConnectionError, TimeoutError):
            self.close_connection = True

    # -- response helpers ---------------------------------------------------
    def _head(self, status, length, ctype="application/octet-stream"):
        """Write a status line and headers. Returns False if the peer is gone."""
        try:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(length))
            self.end_headers()
            return True
        except OSError:
            self.close_connection = True
            return False

    def _simple(self, status, body):
        if not self._head(status, len(body), "text/plain"):
            return False
        try:
            self.wfile.write(body)
            return True
        except OSError:
            self.close_connection = True
            return False

    def _json(self, status, obj):
        body = json.dumps(obj, sort_keys=True).encode()
        if not self._head(status, len(body), "application/json"):
            return False
        try:
            self.wfile.write(body)
            return True
        except OSError:
            self.close_connection = True
            return False

    # -- endpoints ----------------------------------------------------------
    def do_POST(self):
        counters = self.server.counters
        if urlsplit(self.path).path != "/upload":
            counters.add(errors=1)
            self._simple(404, b"not found")
            return
        raw = self.headers.get("Content-Length")
        try:
            length = int(raw) if raw is not None else 0
            if length < 0:
                raise ValueError(raw)
        except ValueError:
            counters.add(errors=1)
            self._simple(400, b"bad content-length")
            return

        remaining = length
        try:
            while remaining > 0:
                block = self.rfile.read(min(CHUNK, remaining))
                if not block:
                    # Peer closed mid-body (normal when the client hits its deadline).
                    counters.add(client_aborts=1)
                    self.close_connection = True
                    return
                remaining -= len(block)
                counters.add(rx_bytes=len(block))
        except (ConnectionError, TimeoutError):
            counters.add(client_aborts=1)
            self.close_connection = True
            return

        if self._simple(200, b"ok"):
            counters.add(requests=1, uploads=1)
        else:
            counters.add(client_aborts=1)

    def do_GET(self):
        counters = self.server.counters
        parts = urlsplit(self.path)

        if parts.path == "/health":
            counters.add(health=1)
            self._json(200, counters.snapshot())
            return

        if parts.path != "/download":
            counters.add(errors=1)
            self._simple(404, b"not found")
            return

        try:
            count = int(parse_qs(parts.query).get("bytes", ["0"])[0])
            if count < 0:
                raise ValueError(count)
        except ValueError:
            counters.add(errors=1)
            self._simple(400, b"bad bytes")
            return

        if not self._head(200, count):
            counters.add(client_aborts=1)
            return

        sent = self._send_block(count)
        counters.add(tx_bytes=sent)
        if sent == count:
            counters.add(requests=1, downloads=1)
        else:
            counters.add(client_aborts=1)
            self.close_connection = True

    def _send_block(self, count):
        """Write exactly `count` bytes from the preallocated block."""
        block = self.server.block
        sent = 0
        try:
            while sent < count:
                left = count - sent
                if left >= CHUNK:
                    self.wfile.write(block)
                    sent += CHUNK
                else:
                    self.wfile.write(block[:left])
                    sent += left
        except (ConnectionError, TimeoutError):
            pass
        return sent


class Server(ThreadingHTTPServer):
    allow_reuse_address = True  # SO_REUSEADDR, explicitly
    daemon_threads = True
    request_queue_size = 128

    def __init__(self, address, counters, block, verbose):
        self.counters = counters
        self.block = block
        self.verbose = verbose
        super().__init__(address, Handler)

    def server_bind(self):
        # Same as HTTPServer.server_bind but without socket.getfqdn(), which can
        # stall for seconds on a host with slow reverse DNS.
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host or "0.0.0.0"
        self.server_port = port


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="HTTP/1.1 baseline server (POST /upload, GET /download)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--stats-out", default="", help="JSON counter snapshot path")
    parser.add_argument("--stats-interval", type=float, default=5.0)
    parser.add_argument("--lock", default="", help="flock this path for the server lifetime")
    parser.add_argument("--verbose", action="store_true", help="log every request")
    args = parser.parse_args(argv)

    lock_fd = acquire_lock(args.lock) if args.lock else None
    counters = Counters()
    block = bytes(CHUNK)  # one 1 MiB zero block, reused for every download
    server = Server((args.host, args.port), counters, block, args.verbose)

    def dump(event):
        snap = counters.snapshot()
        snap["event"] = event
        print(json.dumps(snap, sort_keys=True), flush=True)
        if not args.stats_out:
            return
        tmp = args.stats_out + ".tmp"
        try:
            with open(tmp, "w") as fh:
                json.dump(snap, fh, indent=1, sort_keys=True)
                fh.write("\n")
            os.replace(tmp, args.stats_out)
        except OSError as exc:
            print("stats write failed: %r" % (exc,), file=sys.stderr, flush=True)

    def on_signal(signum, _frame):
        dump("signal:%s" % signal.Signals(signum).name)
        os._exit(0)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    stop = threading.Event()

    def ticker():
        while not stop.wait(args.stats_interval):
            dump("interval")

    threading.Thread(target=ticker, daemon=True).start()

    print(
        json.dumps(
            {
                "event": "listening",
                "host": args.host,
                "port": server.server_port,
                "pid": os.getpid(),
                "python": sys.version.split()[0],
                "chunk_bytes": CHUNK,
                "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            },
            sort_keys=True,
        ),
        flush=True,
    )
    dump("start")
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        stop.set()
        dump("exit")


if __name__ == "__main__":
    main()
