#!/usr/bin/env python3
"""monkey.py - HANG and FLAKY detector for the tbtun 0.10.0 test tunnel.

It runs on a near host (lubancat or 760K) and keeps two streams alive:

  probe   one `curl` through the sidecar every PROBE_EVERY s, each with a
          20 s cap. The result line records rc, HTTP status, time_total and
          time_starttransfer.
  bulk    back-to-back 16 MiB GETs of the far-side file. The reader records
          the received byte count at least once a second.

It writes one JSONL timeline plus one evidence directory per event. It never
prints or stores a tunnel secret: every captured text passes redact() first.

HANG is three signals, each recorded with its own name:
  hang_probe        a probe exits rc=28 after at least 19.5 s
  hang_bulk_stall   bulk bytes stop for 15 s while a :19000 socket stays ESTABLISHED
  hang_bulk_short   a transfer ends with fewer bytes than its Content-Length

FLAKY is any other curl rc, any HTTP status other than 200, and any bulk
socket error. A flaky event that repeats inside DEDUP s folds into one row.

Usage:
  monkey.py run --run R [--seconds S] [--label LBC] [--probe-every 2]
  monkey.py mark --run R --text "dim1 start"
  monkey.py stop --run R
  monkey.py report --run R
"""

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time

PROXY = os.environ.get("MONKEY_PROXY", "127.0.0.1:7899")
PROBE_URL = os.environ.get("MONKEY_PROBE_URL", "https://example.com")
BULK_URL = os.environ.get("MONKEY_BULK_URL", "http://127.0.0.1:18089/16m.bin")
BULK_HOST = os.environ.get("MONKEY_BULK_HOST", "127.0.0.1:18089")
BULK_EXPECT = int(os.environ.get("MONKEY_BULK_EXPECT", "16777216"))
FAR = os.environ.get("MONKEY_FAR", "root@91.213.174.118")
CARRIER_PORT = int(os.environ.get("MONKEY_CARRIER_PORT", "19002"))
FAR_TUNNEL = os.environ.get("MONKEY_FAR_TUNNEL", "tunnel 1")
NEAR_PORT = 19000
ROOT = os.environ.get("MONKEY_ROOT", "/root/monkey")

PROBE_CAP = 20.0
PROBE_HANG_MIN = 19.5
STALL_S = 15.0
DEDUP_S = 10.0
BULK_DEADLINE = 300.0
PROBE_CONCURRENCY = 12
SLOW_S = 2.0

# Curl exposes the breakdown of one upper-level request. The four numbers
# point at different roots: namelookup is the DNS path, connect is the new
# TCP plus the single-stream admission, appconnect adds the TLS handshake
# and the tunnel round trips, starttransfer adds the first body block.
PROBE_W = ("%{http_code} %{time_total} %{time_starttransfer} "
           "%{size_download} %{time_connect} %{time_namelookup} "
           "%{time_appconnect} %{errormsg}")

RED_HEX = re.compile(r"\b[0-9a-fA-F]{24,}\b")
RED_UUID = re.compile(
    r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b")
RED_KV = re.compile(
    r"\b(crypto_key|nonce|uuid|password|KEY1|KEY2|NONCE1|NONCE2)=\S+",
    re.IGNORECASE)


def redact(text):
    if not text:
        return text or ""
    text = RED_KV.sub(r"\1=<redacted>", text)
    text = RED_UUID.sub("<uuid>", text)
    return RED_HEX.sub("<hex>", text)


def now():
    return time.time()


def mono():
    return time.monotonic()


def sh(argv, timeout=15, stdin=None):
    """Run argv, return (rc, stdout+stderr as text). Never raises."""
    try:
        p = subprocess.run(argv, capture_output=True, timeout=timeout,
                           input=stdin, text=True, errors="replace")
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT after %ss: %s" % (timeout, " ".join(argv))
    except Exception as exc:                                  # noqa: BLE001
        return 125, "ERROR %s: %s" % (type(exc).__name__, exc)


def ssh_far(script, timeout=12):
    return sh(["timeout", str(timeout), "ssh", "-o", "BatchMode=yes",
               "-o", "ConnectTimeout=5", "-o", "StrictHostKeyChecking=no",
               FAR, script], timeout=timeout + 3)


class Timeline:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.fh = open(path, "a", buffering=1)
        self.t0 = mono()
        self.marks = Marks(os.path.join(os.path.dirname(path), "marks.jsonl"))
        self.mark_pos = 0

    def add(self, kind, **fields):
        rec = {"t": round(now(), 3), "m": round(mono() - self.t0, 3),
               "kind": kind}
        rec.update(fields)
        line = json.dumps(rec, sort_keys=True, default=str)
        with self.lock:
            self.fh.write(line + "\n")

    def poll_marks(self):
        for mark in self.marks.read_new(self.mark_pos):
            self.mark_pos += 1
            self.add("mark", text=mark.get("text", ""),
                     source=mark.get("source", "chaos"))


class Marks:
    """Chaos actions append here; the timeline folds them in as marks."""

    def __init__(self, path):
        self.path = path

    def add(self, text, source="chaos"):
        rec = {"t": round(now(), 3), "text": text, "source": source}
        with open(self.path, "a") as fh:
            fh.write(json.dumps(rec) + "\n")

    def read_new(self, pos):
        try:
            with open(self.path) as fh:
                lines = fh.read().splitlines()
        except OSError:
            return []
        out = []
        for line in lines[pos:]:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
        return out


class Control:
    """Live switches: {"probe": bool, "bulk": bool, "note": str}."""

    def __init__(self, path, default=None):
        self.path = path
        self.value = {"probe": True, "bulk": True}
        if default:
            self.value.update(default)

    def refresh(self):
        try:
            with open(self.path) as fh:
                data = json.load(fh)
            if isinstance(data, dict):
                self.value.update(data)
        except (OSError, ValueError):
            pass
        return self.value


class Evidence:
    def __init__(self, root, tl, host_label):
        self.root = root
        self.tl = tl
        self.label = host_label
        self.lock = threading.Lock()
        self.seq = 0
        self.index = open(os.path.join(root, "index.jsonl"), "a", buffering=1)
        self.queue = []
        self.cv = threading.Condition()
        self.worker = threading.Thread(target=self._loop, daemon=True)
        self.worker.start()

    def want(self, etype, detail, extra=None):
        with self.cv:
            self.queue.append((etype, detail, extra or {}))
            self.cv.notify()

    def _loop(self):
        while True:
            with self.cv:
                while not self.queue:
                    self.cv.wait()
                etype, detail, extra = self.queue.pop(0)
            try:
                self._collect(etype, detail, extra)
            except Exception as exc:                          # noqa: BLE001
                self.tl.add("snapshot_error", etype=etype,
                            error="%s: %s" % (type(exc).__name__, exc))

    def _collect(self, etype, detail, extra):
        t_trigger = now()
        with self.lock:
            self.seq += 1
            seq = self.seq
        name = "%04d-%s" % (seq, re.sub(r"[^A-Za-z0-9_.-]+", "-", etype))
        path = os.path.join(self.root, name)
        os.makedirs(path, exist_ok=True)
        meta = {"seq": seq, "type": etype, "detail": detail,
                "host": self.label, "t_trigger": round(t_trigger, 3),
                "extra": extra}
        self._write(path, "meta.json", json.dumps(meta, indent=1,
                                                  sort_keys=True, default=str))

        # Near side first: it is the moment we can still lose.
        client_pid, client_log = find_client()
        self._capture(path, "near_ss_all.txt",
                      ["ss", "-tinm", "state", "established"], 10)
        self._capture(path, "near_ss_carriers.txt",
                      ["ss", "-tinm", "state", "established",
                       "( dport = :%d )" % CARRIER_PORT], 10)
        self._capture(path, "near_ss_tunnel.txt",
                      ["ss", "-tinm", "state", "established",
                       "( sport = :%d or dport = :%d )"
                       % (NEAR_PORT, NEAR_PORT)], 10)
        self._capture(path, "near_ss_summary.txt", ["ss", "-s"], 10)
        if client_pid:
            self._capture(path, "near_client_ps.txt",
                          ["ps", "-o", "pid,ppid,stat,etime,rss,nlwp,cmd",
                           "-p", str(client_pid)], 5)
            self._capture(path, "near_client_status.txt",
                          ["cat", "/proc/%d/status" % client_pid], 5)
            self._capture(path, "near_client_wchan.txt",
                          ["cat", "/proc/%d/wchan" % client_pid], 5)
            self._capture(path, "near_client_stack.txt",
                          ["cat", "/proc/%d/stack" % client_pid], 5)
        if client_log:
            tail = tail_file(client_log, 500)
            self._write(path, "near_client_log_tail.txt", redact(tail))
        mlog = newest_glob("/root/tbtun-090/logs/mihomo*.log")
        if mlog:
            self._write(path, "near_mihomo_log_tail.txt",
                        redact(tail_file(mlog, 300)))
            meta["mihomo_log"] = mlog

        # Near timeline slice: last 200 records.
        self._write(path, "timeline_slice.jsonl",
                    redact(tail_file(self.tl.path, 200)))

        # Far side: one ssh, one script, so the two samples stay close in time.
        far_script = r"""
echo "== date =="; date -u +%FT%TZ
echo "== stats =="; cat /proc/net/tbtun/stats 2>&1
echo "== tunnels =="; cat /proc/net/tbtun/tunnels 2>&1
echo "== config =="; cat /proc/net/tbtun/config 2>&1
echo "== dmesg =="; dmesg 2>&1 | tail -n 400
echo "== ss carriers =="; ss -tinm state established \
    '( sport = :19002 or sport = :19012 or dport = :19002 or dport = :19012 )' 2>&1
echo "== ss target =="; ss -tinm state established '( sport = :19001 or dport = :19001 )' 2>&1
echo "== load =="; cat /proc/loadavg; free -m | head -3
"""
        rc, out = ssh_far(far_script, timeout=14)
        self._write(path, "far_snapshot.txt",
                    redact("rc=%d\n%s" % (rc, out)))
        self._write(path, "far_dmesg_tail.txt",
                    redact(far_tail("dmesg 2>&1 | tail -n 200")))
        self._write(path, "far_stats.txt",
                    redact(far_tail("cat /proc/net/tbtun/stats 2>&1")))

        meta["t_snapshot"] = round(now(), 3)
        meta["client_pid"] = client_pid
        meta["client_log"] = client_log
        self._write(path, "meta.json", json.dumps(meta, indent=1,
                                                  sort_keys=True, default=str))
        rec = {"t": round(t_trigger, 3), "seq": seq, "type": etype,
               "detail": detail, "dir": name,
               "t_snapshot": meta["t_snapshot"]}
        with self.lock:
            self.index.write(json.dumps(rec, sort_keys=True) + "\n")
        self.tl.add("snapshot", seq=seq, type=etype, detail=detail, dir=name)

    def _capture(self, path, name, argv, timeout):
        rc, out = sh(argv, timeout=timeout)
        self._write(path, name, redact("rc=%d\n%s" % (rc, out)))

    def _write(self, path, name, text):
        with open(os.path.join(path, name), "w") as fh:
            fh.write(text if text.endswith("\n") else text + "\n")


def far_tail(script):
    rc, out = ssh_far(script, timeout=10)
    return "rc=%d\n%s" % (rc, out)


def tail_file(path, lines):
    try:
        p = subprocess.run(["tail", "-n", str(lines), path],
                           capture_output=True, text=True, errors="replace",
                           timeout=10)
        return p.stdout or ""
    except Exception:                                         # noqa: BLE001
        return ""


def newest_glob(pattern):
    import glob
    files = [f for f in glob.glob(pattern) if os.path.isfile(f)]
    if not files:
        return None
    return max(files, key=os.path.getmtime)


def find_client():
    rc, out = sh(["pgrep", "-x", "tbt_client"], timeout=5)
    pids = [ln.strip() for ln in out.splitlines() if ln.strip().isdigit()]
    if not pids:
        return None, None
    pid = int(pids[0])
    log = None
    try:
        log = os.readlink("/proc/%d/fd/1" % pid)
        if not log.startswith("/"):
            log = None
    except OSError:
        log = None
    return pid, log


class Detector:
    def __init__(self, tl, ev, control):
        self.tl = tl
        self.ev = ev
        self.control = control
        self.last = {}
        self.bulk_port = None
        self.bulk = {"bytes": 0, "total": 0, "state": "idle"}
        self.smux = []
        self.n19000_max = 0

    def emit(self, etype, detail, extra=None, dedupe=DEDUP_S):
        key = etype
        stamp = mono()
        prev = self.last.get(key, -1e9)
        self.tl.add("event", type=etype, detail=detail,
                    extra=extra or {}, deduped=(stamp - prev < dedupe))
        if stamp - prev >= dedupe:
            self.last[key] = stamp
            self.ev.want(etype, detail, extra)

    # -- probe ---------------------------------------------------------
    def probe(self):
        sem = threading.Semaphore(PROBE_CONCURRENCY)

        def one():
            with sem:
                if not self.control.value.get("probe", True):
                    return
                before, _ = tunnel_sockets()
                t0 = mono()
                rc, out = sh([
                    "curl", "-sS", "-o", "/dev/null", "--max-time",
                    "%d" % PROBE_CAP, "-x", "http://" + PROXY,
                    "-w", PROBE_W,
                    PROBE_URL], timeout=PROBE_CAP + 8)
                wall = mono() - t0
                after, q = tunnel_sockets()
                parts = out.strip().split(" ", 7)
                rec = {"rc": rc, "wall": round(wall, 3),
                       "code": _int(parts, 0), "tt": _float(parts, 1),
                       "ttfb": _float(parts, 2), "size": _int(parts, 3),
                       "tcp": _float(parts, 4),
                       "dns": _float(parts, 5),
                       "tls": _float(parts, 6, -1.0),
                       "n19000_before": before, "n19000_after": after,
                       "n19000_delta": after - before,
                       "err": (parts[7] if len(parts) > 7 else "").strip()[:160]}
                self.tl.add("probe", **rec)
                if rc == 28 and rec["tt"] >= PROBE_HANG_MIN:
                    self.emit("hang_probe",
                              "curl rc=28 tt=%.2f ttfb=%.2f" % (rec["tt"],
                                                               rec["ttfb"]),
                              {"probe": rec})
                elif rc != 0:
                    self.emit("flaky_probe_rc", "rc=%d tt=%.2f" % (rc, rec["tt"]),
                              {"probe": rec})
                elif rec["code"] != 200:
                    self.emit("flaky_http_status",
                              "code=%s tt=%.2f" % (rec["code"], rec["tt"]),
                              {"probe": rec})
                # Independent slow classes. Each one names a different root.
                if rec["dns"] >= SLOW_S:
                    self.emit("slow_dns", "namelookup=%.2f s" % rec["dns"],
                              {"probe": rec})
                if rec["tcp"] >= SLOW_S:
                    self.emit("slow_connect", "connect=%.2f s" % rec["tcp"],
                              {"probe": rec})
                if rec["tls"] >= SLOW_S:
                    self.emit("slow_appconnect", "appconnect=%.2f s" % rec["tls"],
                              {"probe": rec})
                # Stream admission: a new :19000 socket for each request means
                # the smux session is not reused.
                self.n19000_max = max(self.n19000_max, after)
                self.smux.append(1 if rec["n19000_delta"] > 0 else 0)
                del self.smux[:-10]
                if len(self.smux) == 10 and all(self.smux):
                    self.emit("smux_no_reuse",
                              "10 probes each added a :19000 socket (now %d)"
                              % after,
                              {"n19000_after": after,
                               "n19000_max": self.n19000_max})

        while not STOP.is_set():
            interval = 2.0
            self.control.refresh()
            if self.control.value.get("probe", True):
                threading.Thread(target=one, daemon=True).start()
                interval = 2.0 if not self.control.value.get("probe_every") \
                    else float(self.control.value["probe_every"])
            else:
                interval = 0.5
            sleep_until(mono() + interval)

    # -- bulk ----------------------------------------------------------
    def bulk_loop(self):
        while not STOP.is_set():
            self.control.refresh()
            if not self.control.value.get("bulk", True):
                self.bulk["state"] = "paused"
                time.sleep(0.5)
                continue
            try:
                self.bulk_once()
            except Exception as exc:                          # noqa: BLE001
                self.emit("flaky_bulk_error",
                          "%s: %s" % (type(exc).__name__, exc),
                          {"bulk_bytes": self.bulk["bytes"]})
                time.sleep(1.0)

    def bulk_once(self):
        self.bulk = {"bytes": 0, "total": 0, "state": "connect"}
        s = socket.create_connection(("127.0.0.1", int(PROXY.split(":")[1])),
                                     timeout=10)
        self.bulk_port = s.getsockname()[1]
        req = ("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: monkey/1\r\n"
               "Accept: */*\r\nConnection: close\r\n\r\n" % (BULK_URL, BULK_HOST))
        s.sendall(req.encode())
        head = b""
        deadline = mono() + 30
        while b"\r\n\r\n" not in head:
            if mono() > deadline:
                raise TimeoutError("no HTTP header in 30 s")
            chunk = s.recv(65536)
            if not chunk:
                raise ConnectionError("EOF before HTTP header")
            head += chunk
        raw_head, rest = head.split(b"\r\n\r\n", 1)
        lines = raw_head.decode("latin-1").split("\r\n")
        status = int(lines[0].split(" ")[1]) if " " in lines[0] else 0
        hdrs = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                hdrs[k.strip().lower()] = v.strip()
        expect = int(hdrs.get("content-length", "0") or 0) or BULK_EXPECT
        got = len(rest)
        self.bulk.update(bytes=got, total=expect, state="body", status=status)
        t_start = mono()
        last_progress = t_start
        last_sample = t_start
        stall_reported = False
        s.settimeout(1.0)
        while got < expect:
            if STOP.is_set():
                break
            if mono() - t_start > BULK_DEADLINE:
                self.emit("flaky_bulk_deadline",
                          "no completion in %.0f s at %d/%d"
                          % (BULK_DEADLINE, got, expect))
                break
            try:
                chunk = s.recv(262144)
            except socket.timeout:
                chunk = None
            except ConnectionResetError as exc:
                self.emit("flaky_bulk_reset", str(exc), {"bulk": dict(self.bulk)})
                break
            t = mono()
            if chunk:
                if got == 0 and status != 200:
                    self.emit("flaky_http_status",
                              "bulk code=%d" % status, {"status": status})
                got += len(chunk)
                self.bulk["bytes"] = got
                last_progress = t
                if stall_reported:
                    self.tl.add("bulk_resumed", bytes=got)
                    stall_reported = False
            elif chunk == b"":
                self.bulk["state"] = "eof"
                break
            if t - last_sample >= 1.0:
                last_sample = t
                self.tl.add("bulk_progress", bytes=got, total=expect,
                            state=self.bulk["state"],
                            local_port=self.bulk_port)
            if t - last_progress >= STALL_S and not stall_reported:
                stall_reported = True
                est, q = tunnel_sockets()
                detail = ("no byte for %.1f s at %d/%d; :19000 est=%d q=%s"
                          % (t - last_progress, got, expect, est, q))
                etype = "hang_bulk_stall" if est > 0 else "flaky_bulk_stall_dead"
                self.emit(etype, detail,
                          {"bytes": got, "total": expect,
                           "bulk_local_port": self.bulk_port,
                           "sockets_19000": est, "recvq_19000": q})
        if got != expect and self.bulk["state"] == "eof" and status == 200:
            self.emit("hang_bulk_short",
                      "clean EOF at %d/%d" % (got, expect),
                      {"bytes": got, "total": expect,
                       "bulk_local_port": self.bulk_port})
        elif got == expect:
            self.bulk["state"] = "done"
            self.tl.add("bulk_done", bytes=got, total=expect,
                        seconds=round(mono() - t_start, 3))
        try:
            s.close()
        except OSError:
            pass
        time.sleep(0.2)

    # -- 1 s socket sampler -------------------------------------------
    def sampler(self):
        last_pulse = 0.0
        while not STOP.is_set():
            carriers = carrier_state()
            est, q = tunnel_sockets()
            self.bulk_sock = (est, q)
            t = mono()
            if t - last_pulse >= 5.0:
                last_pulse = t
                self.tl.add("pulse", carriers=carriers, tunnel_socks=est,
                            tunnel_recvq=q, bulk=self.bulk.get("state"),
                            bulk_bytes=self.bulk.get("bytes"),
                            client=client_alive(), probe_on=self.control.value.get("probe"),
                            bulk_on=self.control.value.get("bulk"))
            time.sleep(1.0)


def client_alive():
    rc, _ = sh(["pgrep", "-x", "tbt_client"], timeout=3)
    return rc == 0


def carrier_state():
    rc, out = sh(["ss", "-Htn", "state", "established",
                  "( dport = :%d )" % CARRIER_PORT], timeout=5)
    return len([ln for ln in out.splitlines() if ln.strip()])


def tunnel_sockets():
    rc, out = sh(["ss", "-Htn", "state", "established",
                  "( sport = :%d or dport = :%d )" % (NEAR_PORT, NEAR_PORT)],
                 timeout=5)
    rows = [ln for ln in out.splitlines() if ln.strip()]
    q = 0
    for ln in rows:
        f = ln.split()
        if len(f) >= 2 and f[0].isdigit():
            q += int(f[0])
    return len(rows), q


def _int(parts, i):
    try:
        return int(parts[i])
    except (IndexError, ValueError):
        return 0


def _float(parts, i, default=0.0):
    try:
        return float(parts[i])
    except (IndexError, ValueError):
        return default


def sleep_until(t_wake):
    while not STOP.is_set():
        left = t_wake - mono()
        if left <= 0:
            return
        time.sleep(min(left, 0.25))


class Stop(Exception):
    pass


class _Signal:
    def __init__(self):
        self.reason = None

    def __call__(self, signum, _frame):
        self.reason = signum
        STOP.set()


SIG = _Signal()


def cmd_run(args):
    run_dir = os.path.join(ROOT, "evidence", args.run)
    os.makedirs(run_dir, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "run"), exist_ok=True)
    signal.signal(signal.SIGTERM, SIG)
    signal.signal(signal.SIGINT, SIG)
    tl = Timeline(os.path.join(run_dir, "timeline.jsonl"))
    label = args.label or socket.gethostname()
    ev = Evidence(run_dir, tl, label)
    control = Control(os.path.join(run_dir, "control.json"))
    det = Detector(tl, ev, control)
    status_path = os.path.join(run_dir, "status.json")

    with open(os.path.join(run_dir, "env.txt"), "w") as fh:
        fh.write("label=%s\nhost=%s\nrun=%s\nproxy=%s\nprobe=%s\nbulk=%s\n"
                 "carrier_port=%d\nfar=%s\nfar_tunnel=%s\nprobe_cap=%.0f\n"
                 "stall_s=%.0f\n"
                 % (label, socket.gethostname(), args.run, PROXY, PROBE_URL,
                    BULK_URL, CARRIER_PORT, FAR, FAR_TUNNEL, PROBE_CAP, STALL_S))
    rc, ver = sh(["uname", "-a"], timeout=5)
    with open(os.path.join(run_dir, "env.txt"), "a") as fh:
        fh.write(ver)

    t_start = now()
    tl.add("run_start", label=label, run=args.run, pid=os.getpid(),
           probe_url=PROBE_URL, bulk_url=BULK_URL, carrier_port=CARRIER_PORT)
    ev.want("baseline", "run start")
    threading.Thread(target=det.probe, daemon=True).start()
    threading.Thread(target=det.bulk_loop, daemon=True).start()
    threading.Thread(target=det.sampler, daemon=True).start()

    deadline = mono() + args.seconds if args.seconds else None
    last_status = 0.0
    while not STOP.is_set():
        tl.poll_marks()
        if deadline and mono() > deadline:
            break
        if mono() - last_status >= 5.0:
            last_status = mono()
            carriers = carrier_state()
            est, q = tunnel_sockets()
            status = {"run": args.run, "label": label, "t": round(now(), 3),
                      "elapsed": round(now() - t_start, 1),
                      "carriers_est": carriers, "tunnel_socks": est,
                      "tunnel_recvq": q, "client": client_alive(),
                      "events": len(ev.queue), "bulk": det.bulk}
            tmp = status_path + ".tmp"
            with open(tmp, "w") as fh:
                json.dump(status, fh)
            os.replace(tmp, status_path)
        time.sleep(0.5)

    STOP.set()
    time.sleep(1.5)
    tl.add("run_end", elapsed=round(now() - t_start, 1),
           signal=SIG.reason, events=len(ev.queue))
    ev.want("final", "run end")
    time.sleep(3.0)
    return 0


def cmd_mark(args):
    run_dir = os.path.join(ROOT, "evidence", args.run)
    os.makedirs(run_dir, exist_ok=True)
    Marks(os.path.join(run_dir, "marks.jsonl")).add(args.text)
    print("mark: %s" % args.text)


def cmd_set(args):
    run_dir = os.path.join(ROOT, "evidence", args.run)
    path = os.path.join(run_dir, "control.json")
    try:
        with open(path) as fh:
            cur = json.load(fh)
    except (OSError, ValueError):
        cur = {"probe": True, "bulk": True}
    for item in args.kv:
        key, _, value = item.partition("=")
        if value.lower() in ("0", "false", "off"):
            cur[key] = False
        elif value.lower() in ("1", "true", "on"):
            cur[key] = True
        else:
            cur[key] = value
    with open(path, "w") as fh:
        json.dump(cur, fh)
    print("control: %s" % json.dumps(cur, sort_keys=True))


def cmd_stop(args):
    run_dir = os.path.join(ROOT, "evidence", args.run)
    pidfile = os.path.join(run_dir, "harness.pid")
    try:
        with open(pidfile) as fh:
            pid = int(fh.read().strip())
    except (OSError, ValueError):
        print("no pidfile", file=sys.stderr)
        return 1
    try:
        os.kill(pid, signal.SIGTERM)
        print("signalled %d" % pid)
    except OSError as exc:
        print("kill: %s" % exc, file=sys.stderr)
        return 1
    return 0


def cmd_report(args):
    run_dir = os.path.join(ROOT, "evidence", args.run)
    counts = {}
    total = {"probe": 0, "probe_err": 0, "bulk_done": 0, "bulk_bytes": 0}
    with open(os.path.join(run_dir, "timeline.jsonl")) as fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec["kind"] == "event" and not rec.get("deduped"):
                counts[rec["type"]] = counts.get(rec["type"], 0) + 1
            elif rec["kind"] == "probe":
                total["probe"] += 1
                if rec["rc"]:
                    total["probe_err"] += 1
            elif rec["kind"] == "bulk_done":
                total["bulk_done"] += 1
                total["bulk_bytes"] += rec["bytes"]
    print(json.dumps({"run": args.run, "events": counts, "totals": total},
                     indent=1, sort_keys=True))
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("--run", required=True)
    p.add_argument("--seconds", type=int, default=0)
    p.add_argument("--label", default="")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("mark")
    p.add_argument("--run", required=True)
    p.add_argument("--text", required=True)
    p.set_defaults(fn=cmd_mark)
    p = sub.add_parser("set")
    p.add_argument("--run", required=True)
    p.add_argument("kv", nargs="+")
    p.set_defaults(fn=cmd_set)
    p = sub.add_parser("stop")
    p.add_argument("--run", required=True)
    p.set_defaults(fn=cmd_stop)
    p = sub.add_parser("report")
    p.add_argument("--run", required=True)
    p.set_defaults(fn=cmd_report)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
