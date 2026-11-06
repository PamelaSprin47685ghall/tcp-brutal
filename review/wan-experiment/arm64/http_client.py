#!/usr/bin/env python3
"""8-process HTTP/1.1 throughput client for the arm64 T5.2 acceptance baseline.

Runs on the near side (.65). One OS process per connection, so the 8 parallel
streams do not share a GIL. Each process holds a single keep-alive HTTP/1.1
connection to ``http_server.py`` and loops for ``--seconds``:

    up    POST /upload with an 8 MiB body per request
    down  GET  /download?bytes=8MiB per request
    both  one up phase, then one down phase, each --seconds long

Accounting is application level and honest about the deadline: bytes actually
written to / read from the socket are counted, while a request the deadline
cuts mid-body is counted in ``partial_requests`` and not in ``requests``. For
``up`` the client may therefore report a few hundred KiB more than the server
read, because bytes still sitting in the kernel buffer are dropped at close.

Exit status
-----------
0  every phase returned one result per process and moved bytes
1  a phase is missing a process result, or a phase moved zero bytes
2  usage error (argparse)

Usage
-----
    python3 http_client.py --host 45.11.1.62 --port 10000 \
        --procs 8 --mib 8 --seconds 10 --runs 2 --direction both --out client.json
"""

import argparse
import json
import multiprocessing as mp
import os
import queue
import socket
import sys
import time

SEND_CHUNK = 256 << 10  # deadline granularity while pushing an 8 MiB body
READ_BUF = 256 << 10


def proc_cpu_s():
    """utime+stime of this process, in seconds."""
    with open("/proc/self/stat") as fh:
        fields = fh.read().rsplit(") ", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


class Conn:
    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.sock.makefile("rb")
        self.dead = False

    def close(self):
        for closer in (self.reader.close, self.sock.close):
            try:
                closer()
            except OSError:
                pass


def read_head(reader):
    line = reader.readline(65536)
    if not line:
        raise ConnectionError("eof before status line")
    parts = line.split(None, 2)
    if len(parts) < 2:
        raise ValueError("bad status line %r" % line[:80])
    status = int(parts[1])
    headers = {}
    while True:
        line = reader.readline(65536)
        if not line:
            raise ConnectionError("eof inside headers")
        if line in (b"\r\n", b"\n"):
            break
        name, _, value = line.decode("latin-1").partition(":")
        headers[name.strip().lower()] = value.strip()
    return status, headers


def drain_body(reader, headers):
    if headers.get("transfer-encoding", "").lower() == "chunked":
        raise ValueError("unexpected chunked response")
    left = int(headers.get("content-length", "0") or 0)
    while left > 0:
        block = reader.read(min(left, 65536))
        if not block:
            raise ConnectionError("eof draining ack body")
        left -= len(block)


def upload_once(conn, body_mv, deadline):
    """Send one POST. Returns (body_bytes_written, completed)."""
    size = len(body_mv)
    conn.sock.sendall(
        b"POST /upload HTTP/1.1\r\nHost: baseline\r\nContent-Length: %d\r\n\r\n" % size
    )
    sent = 0
    while sent < size:
        if time.monotonic() >= deadline:
            return sent, False
        end = sent + SEND_CHUNK
        conn.sock.sendall(body_mv[sent:end] if end < size else body_mv[sent:])
        sent = end if end < size else size
    status, headers = read_head(conn.reader)
    drain_body(conn.reader, headers)
    if status != 200:
        raise OSError("upload status %d" % status)
    if headers.get("connection", "").lower() == "close":
        conn.dead = True
    return sent, True


def download_once(conn, view, size, deadline):
    """Receive one GET body. Returns (body_bytes_read, completed)."""
    conn.sock.sendall(b"GET /download?bytes=%d HTTP/1.1\r\nHost: baseline\r\n\r\n" % size)
    status, headers = read_head(conn.reader)
    if status != 200:
        raise OSError("download status %d" % status)
    if headers.get("transfer-encoding", "").lower() == "chunked":
        raise ValueError("unexpected chunked response")
    length = int(headers.get("content-length", "0") or 0)
    got = 0
    while got < length:
        if time.monotonic() >= deadline:
            return got, False
        block = conn.reader.readinto(view)
        if not block:
            raise ConnectionError("eof inside body")
        got += block
    if headers.get("connection", "").lower() == "close":
        conn.dead = True
    return got, True


def worker(idx, host, port, direction, seconds, mib, timeout, result_q):
    start_epoch = time.time()
    start_mono = time.monotonic()
    deadline = start_mono + seconds
    cpu_start = proc_cpu_s()

    payload = mib << 20
    body_mv = memoryview(bytes(payload))  # one allocation per process
    view = memoryview(bytearray(READ_BUF))

    stats = {
        "idx": idx,
        "direction": direction,
        "bytes": 0,
        "requests": 0,
        "partial_requests": 0,
        "errors": 0,
        "connects": 0,
        "first_error": None,
    }
    conn = None
    try:
        while True:
            if time.monotonic() >= deadline:
                break
            if conn is None:
                try:
                    conn = Conn(host, port, timeout)
                    stats["connects"] += 1
                except OSError as exc:
                    stats["errors"] += 1
                    if stats["first_error"] is None:
                        stats["first_error"] = repr(exc)
                    pause = min(0.05, max(0.0, deadline - time.monotonic()))
                    time.sleep(pause)
                    continue
            try:
                if direction == "up":
                    moved, done = upload_once(conn, body_mv, deadline)
                else:
                    moved, done = download_once(conn, view, payload, deadline)
            except (OSError, ValueError, ConnectionError) as exc:
                stats["errors"] += 1
                if stats["first_error"] is None:
                    stats["first_error"] = repr(exc)
                conn.close()
                conn = None
                continue
            stats["bytes"] += moved
            if done:
                stats["requests"] += 1
            elif moved:
                # A deadline cut with zero bytes moved is not a partial request.
                stats["partial_requests"] += 1
            if conn.dead or not done:
                conn.close()
                conn = None
    finally:
        if conn is not None:
            conn.close()
        stats["cpu_s"] = round(proc_cpu_s() - cpu_start, 4)
        stats["start_epoch"] = round(start_epoch, 4)
        stats["end_epoch"] = round(time.time(), 4)
        stats["active_s"] = round(time.monotonic() - start_mono, 4)
        result_q.put(stats)


def run_phase(direction, args):
    ctx = mp.get_context("fork")
    result_q = ctx.Queue()
    procs = [
        ctx.Process(
            target=worker,
            args=(idx, args.host, args.port, direction, args.seconds, args.mib, args.timeout, result_q),
            name="http-baseline-%d" % idx,
        )
        for idx in range(args.procs)
    ]

    started = time.monotonic()
    for proc in procs:
        proc.start()

    hard_deadline = started + args.seconds + args.timeout + 10
    results = []
    while len(results) < len(procs):
        left = hard_deadline - time.monotonic()
        if left <= 0:
            break
        try:
            results.append(result_q.get(timeout=left))
        except queue.Empty:
            break
    for proc in procs:
        proc.join(timeout=max(0.1, hard_deadline - time.monotonic()))
    for proc in procs:
        if proc.is_alive():
            proc.terminate()
            proc.join(timeout=2)
    wall = time.monotonic() - started

    results.sort(key=lambda r: r["idx"])
    total_bytes = sum(r["bytes"] for r in results)
    if results:
        span = max(r["end_epoch"] for r in results) - min(r["start_epoch"] for r in results)
    else:
        span = wall
    span = max(span, 1e-9)
    cpu_s = sum(r["cpu_s"] for r in results)

    aggregate = {
        "direction": direction,
        "procs_expected": args.procs,
        "procs_reported": len(results),
        "bytes": total_bytes,
        "mib": round(total_bytes / 2**20, 3),
        "mbps": round(total_bytes * 8 / span / 1e6, 3),
        "requests": sum(r["requests"] for r in results),
        "partial_requests": sum(r["partial_requests"] for r in results),
        "errors": sum(r["errors"] for r in results),
        "connects": sum(r["connects"] for r in results),
        "cpu_s": round(cpu_s, 4),
        "cpu_cores": round(cpu_s / span, 4),
        "span_s": round(span, 4),
        "parent_wall_s": round(wall, 4),
        "payload_mib": args.mib,
        "seconds": args.seconds,
    }
    print(json.dumps(aggregate, sort_keys=True), flush=True)
    return {"aggregate": aggregate, "procs": results}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="8-process HTTP/1.1 baseline client (one keep-alive connection per process)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default="45.11.1.62")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--procs", type=int, default=8)
    parser.add_argument("--mib", type=int, default=8, help="payload per request, MiB")
    parser.add_argument("--seconds", type=float, default=10.0, help="seconds per phase")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--direction", choices=("up", "down", "both"), default="both")
    parser.add_argument("--timeout", type=float, default=30.0, help="socket timeout, seconds")
    parser.add_argument("--out", default="", help="JSON output path")
    args = parser.parse_args(argv)

    if args.procs < 1 or args.mib < 1 or args.seconds <= 0 or args.runs < 1:
        parser.error("--procs, --mib, --seconds and --runs must be positive")

    phases = ("up", "down") if args.direction == "both" else (args.direction,)
    started = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    doc = {
        "tool": "http_client.py",
        "version": 1,
        "started": started,
        "config": {
            "host": args.host,
            "port": args.port,
            "procs": args.procs,
            "mib": args.mib,
            "seconds": args.seconds,
            "runs": args.runs,
            "direction": args.direction,
            "timeout_s": args.timeout,
            "python": sys.version.split()[0],
        },
        "runs": [],
    }

    rc = 0
    for run in range(1, args.runs + 1):
        entry = {"run": run, "phases": []}
        for direction in phases:
            entry["phases"].append(run_phase(direction, args))
        doc["runs"].append(entry)
        for phase in entry["phases"]:
            agg = phase["aggregate"]
            if agg["procs_reported"] != args.procs or agg["bytes"] == 0:
                rc = 1

    doc["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    doc["exit_code"] = rc
    if args.out:
        tmp = args.out + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(doc, fh, indent=1, sort_keys=True)
            fh.write("\n")
        os.replace(tmp, args.out)
    print(json.dumps({"event": "done", "runs": args.runs, "exit_code": rc}, sort_keys=True), flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
