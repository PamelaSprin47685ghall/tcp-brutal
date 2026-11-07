#!/usr/bin/env python3
"""Minimal single-stream HTTP sender for the 1 GiB acceptance source.

Replaces busybox httpd on the far host's 127.0.0.1:5210. The point is the
absence of an application-level write timeout: a blocking sendall() waits as
long as the peer needs, so a slow tunnel cannot make the source close the
response early.

    GET   -> HTTP/1.1 200 + Content-Length + the file body, then SHUT_WR
    HEAD  -> the same headers, no body
    other -> 405

One connection at a time; the readiness HEAD and the real GET are sequential.
No SO_SNDTIMEO is set anywhere on the body path.
"""

import argparse
import os
import socket
import time

CHUNK = 1 << 20
DRAIN_TIMEOUT = 5


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def read_request(conn):
    data = b""
    while b"\r\n\r\n" not in data and len(data) < 65536:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def log(path, line):
    try:
        with open(path, "a") as f:
            f.write(line + "\n")
            f.flush()
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", required=True)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5210)
    ap.add_argument("--log", default="/root/accept-1g/raw-serve.log")
    ap.add_argument("--pidfile", default="/root/accept-1g/raw-serve.pid")
    args = ap.parse_args()

    size = os.path.getsize(args.file)
    with open(args.pidfile, "w") as f:
        f.write("%d\n" % os.getpid())

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    log(args.log, "%s listen %s:%d file=%s size=%d pid=%d"
        % (now(), args.bind, args.port, args.file, size, os.getpid()))

    while True:
        try:
            conn, _addr = srv.accept()
        except OSError as e:
            log(args.log, "%s accept error: %s" % (now(), e))
            continue
        conn.settimeout(None)  # no application write timeout
        t0 = time.time()
        sent = 0
        req = ""
        try:
            raw = read_request(conn)
            req = raw.split(b"\r\n", 1)[0].decode("latin1", "replace") if raw else ""
            is_head = req.startswith("HEAD ")
            is_get = req.startswith("GET ")
            if not (is_head or is_get):
                body = b"405 Method Not Allowed\n"
                conn.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n"
                             b"Content-Length: %d\r\nConnection: close\r\n\r\n"
                             % len(body))
                conn.sendall(body)
                conn.shutdown(socket.SHUT_WR)
                log(args.log, "%s %s -> 405" % (now(), req))
                continue
            hdr = ("HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %d\r\n"
                   "Connection: close\r\n\r\n" % size).encode()
            conn.sendall(hdr)
            if is_get:
                with open(args.file, "rb") as f:
                    while True:
                        chunk = f.read(CHUNK)
                        if not chunk:
                            break
                        conn.sendall(chunk)
                        sent += len(chunk)
            conn.shutdown(socket.SHUT_WR)
            # Let the peer read the FIN; no data is expected back.
            conn.settimeout(DRAIN_TIMEOUT)
            try:
                while conn.recv(4096):
                    pass
            except (socket.timeout, OSError):
                pass
            log(args.log, "%s %s sent=%d elapsed=%.3f"
                % (now(), req, sent, time.time() - t0))
        except (BrokenPipeError, ConnectionResetError) as e:
            log(args.log, "%s %s client gone after sent=%d elapsed=%.3f: %s"
                % (now(), req, sent, time.time() - t0, e))
        except OSError as e:
            log(args.log, "%s %s error after sent=%d: %s"
                % (now(), req, sent, e))
        finally:
            try:
                conn.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()
