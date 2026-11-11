#!/usr/bin/env python3
"""monkey-clients.py - stream shapes that the shell driver cannot express.

All connections use the sidecar as an HTTP proxy, so they cross the tunnel
the same way a browser does.

  persistent  open one connection, finish a request, hold it idle, then send
              a second request on the same connection. This probes the
              25 ms cell-tail seal and the keepalive path.
  halfclose   send a request, shutdown(SHUT_WR), then read for N s. This
              probes the far-side clean-EOF and HUP path.
  earlyclose  request a large body and close after N bytes with SO_LINGER 0,
              so the tunnel sees an RST in the middle of a response.
  aborts      N concurrent short requests with a random 1..3 s cap.
"""

import argparse
import os
import random
import socket
import sys
import threading
import time

PROXY_HOST = os.environ.get("MONKEY_PROXY_HOST", "127.0.0.1")
PROXY_PORT = int(os.environ.get("MONKEY_PROXY_PORT", "7899"))
OK_URL = os.environ.get("MONKEY_OK_URL", "http://127.0.0.1:18089/ok.txt")
BIG_URL = os.environ.get("MONKEY_BIG_URL", "http://127.0.0.1:18089/16m.bin")
BIG_HOST = os.environ.get("MONKEY_BIG_HOST", "127.0.0.1:18089")


def now():
    return round(time.time(), 3)


def connect(timeout=10):
    s = socket.create_connection((PROXY_HOST, PROXY_PORT), timeout=timeout)
    return s


def request(s, url, host, extra=""):
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: monkey/1\r\n"
               "Accept: */*\r\n%s\r\n" % (url, host, extra)).encode())


def read_head(s, limit=65536):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("EOF in head")
        buf += chunk
        if len(buf) > limit:
            raise ValueError("head too long")
    head, rest = buf.split(b"\r\n\r\n", 1)
    line = head.split(b"\r\n")[0].decode("latin-1")
    status = int(line.split(" ")[1]) if " " in line else 0
    return status, rest


def cmd_persistent(args):
    out = {"t0": now(), "hold": args.hold, "requests": []}
    try:
        s = connect()
        s.settimeout(30)
        request(s, OK_URL, BIG_HOST, "Connection: keep-alive\r\n")
        status, rest = read_head(s)
        out["requests"].append({"seq": 0, "status": status,
                                "t": now(), "body": len(rest)})
        time.sleep(args.hold)
        s.settimeout(args.second_timeout)
        t = time.monotonic()
        request(s, OK_URL, BIG_HOST, "Connection: keep-alive\r\n")
        status, rest = read_head(s)
        out["requests"].append({"seq": 1, "status": status, "t": now(),
                                "body": len(rest),
                                "wait_s": round(time.monotonic() - t, 3)})
        s.close()
        out["ok"] = True
    except Exception as exc:                                  # noqa: BLE001
        out["ok"] = False
        out["error"] = "%s: %s" % (type(exc).__name__, exc)
    print(out)
    return 0 if out.get("ok") else 1


def cmd_halfclose(args):
    out = {"t0": now()}
    try:
        s = connect()
        s.settimeout(10)
        request(s, BIG_URL, BIG_HOST, "Connection: close\r\n")
        s.shutdown(socket.SHUT_WR)
        s.settimeout(1.0)
        got = 0
        status = None
        deadline = time.monotonic() + args.hold
        idle = 0
        while time.monotonic() < deadline:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                idle += 1
                continue
            if not chunk:
                out["eof"] = True
                break
            if status is None and got == 0:
                try:
                    status = int(chunk.split(b" ")[1])
                except (IndexError, ValueError):
                    status = 0
            got += len(chunk)
            idle = 0
        out.update(status=status, bytes=got, idle_s=idle,
                   waited_s=round(time.monotonic() - (deadline - args.hold), 3))
        s.close()
        out["ok"] = True
    except Exception as exc:                                  # noqa: BLE001
        out["ok"] = False
        out["error"] = "%s: %s" % (type(exc).__name__, exc)
    print(out)
    return 0


def cmd_earlyclose(args):
    out = {"t0": now()}
    try:
        s = connect()
        s.settimeout(30)
        request(s, BIG_URL, BIG_HOST, "Connection: close\r\n")
        got = 0
        while got < args.after:
            chunk = s.recv(65536)
            if not chunk:
                break
            got += len(chunk)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     b"\x01\x00\x00\x00\x00\x00\x00\x00")
        s.close()
        out.update(bytes=got, closed_hard=True, ok=True)
    except Exception as exc:                                  # noqa: BLE001
        out["ok"] = False
        out["error"] = "%s: %s" % (type(exc).__name__, exc)
    print(out)
    return 0


def cmd_aborts(args):
    rng = random.Random(args.seed)
    results = []
    lock = threading.Lock()

    def one(idx):
        cap = rng.uniform(1.0, 3.0)
        t0 = time.monotonic()
        rec = {"i": idx, "cap": round(cap, 2)}
        try:
            s = connect(timeout=5)
            s.settimeout(cap)
            request(s, BIG_URL, BIG_HOST, "Connection: close\r\n")
            status, rest = read_head(s)
            got = len(rest)
            rec["status"] = status
            while time.monotonic() - t0 < cap:
                chunk = s.recv(65536)
                if not chunk:
                    rec["eof"] = True
                    break
                got += len(chunk)
            rec["bytes"] = got
            rec["ok"] = True
        except socket.timeout:
            rec["ok"] = False
            rec["error"] = "timeout"
        except Exception as exc:                              # noqa: BLE001
            rec["ok"] = False
            rec["error"] = "%s: %s" % (type(exc).__name__, exc)
        finally:
            try:
                s.close()
            except Exception:                                 # noqa: BLE001
                pass
        rec["elapsed"] = round(time.monotonic() - t0, 3)
        with lock:
            results.append(rec)

    rounds = args.rounds
    for r in range(rounds):
        threads = [threading.Thread(target=one, args=(r * args.clients + i,))
                   for i in range(args.clients)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    bad = [r for r in results if not r.get("ok")]
    print({"clients": args.clients, "rounds": rounds, "total": len(results),
           "bad": len(bad), "sample_bad": bad[:5]})
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("persistent")
    p.add_argument("--hold", type=int, default=300)
    p.add_argument("--second-timeout", type=float, default=30.0)
    p.set_defaults(fn=cmd_persistent)
    p = sub.add_parser("halfclose")
    p.add_argument("--hold", type=int, default=60)
    p.set_defaults(fn=cmd_halfclose)
    p = sub.add_parser("earlyclose")
    p.add_argument("--after", type=int, default=1024)
    p.set_defaults(fn=cmd_earlyclose)
    p = sub.add_parser("aborts")
    p.add_argument("--clients", type=int, default=32)
    p.add_argument("--rounds", type=int, default=1)
    p.add_argument("--seed", type=int, default=1)
    p.set_defaults(fn=cmd_aborts)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
