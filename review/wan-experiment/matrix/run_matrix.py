#!/usr/bin/env python3
"""tbtun WAN matrix: 5 configs x 2 runs x 15 s through the real tunnel.

Receiver throughput is taken from the HND0 iperf3 server log (server side),
not from the client JSON. Every run records both ends' /proc/net/tbtun stats,
peers, tunnels, and CPU samples.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time

ROOT = "/home/kunweiz/Desktop/vibe/tcp-brutal"
OUT = os.path.join(ROOT, "review/wan-experiment/matrix")
IPERF = "/tmp/iperf3-local/bin/iperf3"
HND0 = "root@45.11.1.62"
FAR_LOG = "/tmp/tbtun-exp-70031a8/iperf3-server.log"
FAR_IPERF_PID = 699110
DUR = 15
RUNS = 2
WAIT_CARRIER = 3.0

NONCE = "00112233445566778899aabbccddeeff"
COMMON = ("keepalive=2000 hole_budget_ms=30000 carrier_stall_ms=15000 "
          "replay=8388608 nonce=" + NONCE)

CONFIGS = [
    dict(cid="c1-raw-n1m0d0", profile="raw", n=1, m=0, dup=0),
    dict(cid="c2-raw-n4m1d0", profile="raw", n=4, m=1, dup=0),
    dict(cid="c3-raw-n8m1d0", profile="raw", n=8, m=1, dup=0),
    dict(cid="c4-httpkal-n4m1d0", profile="http-kal", n=4, m=1, dup=0),
    dict(cid="c5-raw-n4m1d1", profile="raw", n=4, m=1, dup=1),
]

RECV_RE = re.compile(
    r"\[\s*\d+\]\s+([\d.]+)-([\d.]+)\s+sec\s+([\d.]+)\s+(\w+Bytes)\s+"
    r"([\d.]+)\s+(\w+bits/sec)\s+receiver")

HND0_SAMPLER = r'''
import os, sys, time
pid = int(sys.argv[1])
def stat():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    return v[3] + (v[4] if len(v) > 4 else 0), sum(v)
def pc():
    try:
        with open("/proc/%d/stat" % pid) as f:
            p = f.read().split()
        return int(p[13]) + int(p[14])
    except OSError:
        return None
HZ = os.sysconf("SC_CLK_TCK")
t0 = time.time(); a = stat(); ap = pc(); tp = t0
print("t,busy_pct,iperf3_cpu_pct")
while time.time() - t0 < 18.0:
    time.sleep(0.5)
    b = stat(); t = time.time(); dt = max(1e-6, t - tp)
    p = pc()
    if p is None or ap is None:
        print("%.2f,%.2f," % (t - t0, 100.0 * (1 - (b[0] - a[0]) / max(1, b[1] - a[1]))))
    else:
        print("%.2f,%.2f,%.2f" % (t - t0,
              100.0 * (1 - (b[0] - a[0]) / max(1, b[1] - a[1])),
              100.0 * (p - ap) / HZ / dt))
        ap = p
    a = b; tp = t
'''


def run(cmd, data=None, timeout=60, cwd=None):
    return subprocess.run(cmd, input=data, text=True, capture_output=True,
                          timeout=timeout, cwd=cwd)


def ssh(cmd, data=None, timeout=60):
    return run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                HND0, cmd], data=data, timeout=timeout)


def near_cat(name):
    return run(["sudo", "-n", "/usr/bin/cat", "/proc/net/tbtun/" + name])


def near_write_cfg(text):
    return run(["sudo", "-n", "/usr/bin/tee", "/proc/net/tbtun/config"],
               data=text)


def far_write_cfg(text):
    return ssh("tee /proc/net/tbtun/config", data=text)


def far_cat(name):
    return ssh("cat /proc/net/tbtun/" + name)


def del_tunnels():
    e1 = near_write_cfg("del 1\n")
    e2 = far_write_cfg("del 1\n")
    if e1.returncode or e2.returncode:
        raise RuntimeError("del failed: near=%r far=%r" % (e1.stderr, e2.stderr))
    time.sleep(1.0)


def config_text(cfg, role, portblock):
    if role == "near":
        head = "tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000"
    else:
        head = "tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201"
    return ("%s n=%d m=%d dup=%d profile=%s portblock=%s %s\n"
            % (head, cfg["n"], cfg["m"], cfg["dup"], cfg["profile"],
               portblock, COMMON))


def parse_kv(text):
    d = {}
    for line in text.splitlines():
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                d.setdefault(k, v)
    return d


def numeric(d, keys):
    out = {}
    for k in keys:
        try:
            out[k] = int(d[k])
        except (KeyError, ValueError):
            try:
                out[k] = float(d[k])
            except (KeyError, ValueError):
                pass
    return out


def parse_receiver(text):
    """Return (bytes, seconds, mbps) for the last receiver line."""
    best = None
    for m in RECV_RE.finditer(text):
        start, end, val, unit, rate, runit = m.groups()
        mult = {"Bytes": 1, "KBytes": 1024, "MBytes": 1024 ** 2,
                "GBytes": 1024 ** 3}[unit]
        nbytes = float(val) * mult
        secs = float(end) - float(start)
        if secs <= 0:
            continue
        best = (nbytes, secs, nbytes * 8.0 / secs / 1e6)
    return best


class Sampler(threading.Thread):
    def __init__(self, pid_holder):
        super().__init__(daemon=True)
        self.pid_holder = pid_holder
        self.stop = threading.Event()
        self.samples = []

    def _stat(self):
        with open("/proc/stat") as f:
            v = [int(x) for x in f.readline().split()[1:]]
        return v[3] + (v[4] if len(v) > 4 else 0), sum(v)

    def _pcpu(self, pid):
        try:
            with open("/proc/%d/stat" % pid) as f:
                p = f.read().split()
            return int(p[13]) + int(p[14])
        except (OSError, IndexError):
            return None

    def run(self):
        hz = os.sysconf("SC_CLK_TCK")
        t0 = time.time()
        a = self._stat()
        ap = self._pcpu(self.pid_holder[0]) if self.pid_holder[0] else None
        tp = t0
        while not self.stop.wait(0.5):
            b = self._stat()
            t = time.time()
            dt = max(1e-6, t - tp)
            rec = {"t": round(t - t0, 2),
                   "busy_pct": round(100.0 * (1 - (b[0] - a[0]) / max(1, b[1] - a[1])), 2)}
            pid = self.pid_holder[0]
            if pid:
                p = self._pcpu(pid)
                if p is not None:
                    if ap is None:
                        ap = p  # first sample after the child appears
                    else:
                        rec["iperf_cpu_pct"] = round(100.0 * (p - ap) / hz / dt, 2)
                        ap = p
            self.samples.append(rec)
            a = b
            tp = t


def cpu_summary(samples, field="busy_pct"):
    vals = [s[field] for s in samples if field in s]
    if not vals:
        return None
    return {"avg": round(sum(vals) / len(vals), 2), "max": round(max(vals), 2),
            "n": len(vals)}


def far_cpu_start():
    import base64
    b64 = base64.b64encode(HND0_SAMPLER.encode()).decode()
    cmd = "echo %s | base64 -d | python3 - %d" % (b64, FAR_IPERF_PID)
    return subprocess.Popen(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", HND0, cmd],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def parse_far_cpu(text):
    samples = []
    for line in text.splitlines():
        parts = line.strip().split(",")
        if len(parts) < 2:
            continue
        try:
            rec = {"t": float(parts[0]), "busy_pct": float(parts[1])}
        except ValueError:
            continue
        if len(parts) > 2 and parts[2]:
            try:
                rec["iperf_cpu_pct"] = float(parts[2])
            except ValueError:
                pass
        samples.append(rec)
    return samples


def save(path, text):
    with open(os.path.join(OUT, path), "w") as f:
        f.write(text)


def run_one(cfg, run_idx, attempt, pb_idx):
    tag = "%s-r%d-a%d" % (cfg["cid"], run_idx, attempt)
    pb_near = "%d-%d" % (30100 + pb_idx * 100, 30199 + pb_idx * 100)
    pb_far = "%d-%d" % (31100 + pb_idx * 100, 31199 + pb_idx * 100)
    rec = {"tag": tag, "config": cfg["cid"], "run": run_idx,
           "attempt": attempt, "portblock_near": pb_near,
           "portblock_far": pb_far, "dur": DUR, "ts": time.strftime("%FT%T%z")}

    del_tunnels()
    far_text = config_text(cfg, "far", pb_far)
    near_text = config_text(cfg, "near", pb_near)
    e = far_write_cfg(far_text)
    if e.returncode:
        rec["error"] = "far config write failed: %r" % e.stderr
        return rec
    e = near_write_cfg(near_text)
    if e.returncode:
        rec["error"] = "near config write failed: %r" % e.stderr
        return rec
    time.sleep(WAIT_CARRIER)

    cf = far_cat("config")
    cn = near_cat("config")
    save("config-%s-far.txt" % tag, cf.stdout)
    save("config-%s-near.txt" % tag, cn.stdout)
    rec["config_far"] = cf.stdout.strip()
    rec["config_near"] = cn.stdout.strip()
    for name, text in (("far", cf.stdout), ("near", cn.stdout)):
        want = "n=%d m=%d dup=%d profile=%s" % (cfg["n"], cfg["m"],
                                                cfg["dup"], cfg["profile"])
        if want not in text:
            rec["error"] = "%s config readback mismatch: want %r got %r" % (
                name, want, text.strip())
            return rec

    before = {}
    for side, fn in (("near", near_cat), ("far", far_cat)):
        for name in ("stats", "peers", "tunnels"):
            p = fn(name)
            save("%s-%s-before-%s.txt" % (name, tag, side), p.stdout)
            before[side + "_" + name] = p.stdout
    rec["near_before"] = numeric(parse_kv(before["near_stats"]),
                                 ["submit_bytes", "tx", "wire", "retx_frames",
                                  "retx_bytes", "retx_data_ppm", "dup_bytes_ppm",
                                  "holes_unrecoverable", "lost", "win_block",
                                  "rx_delivered", "rx_read_zero", "dup_bytes"])
    rec["far_before"] = numeric(parse_kv(before["far_stats"]),
                                ["rx_delivered", "rx_read_zero",
                                 "holes_unrecoverable", "lost", "dup_bytes_ppm",
                                 "submit_bytes", "wire", "retx_frames"])

    off = ssh("stat -c %%s %s" % FAR_LOG).stdout.strip()
    try:
        off = int(off)
    except ValueError:
        off = 0
    rec["server_log_offset"] = off

    fcpu = far_cpu_start()
    time.sleep(0.7)
    pid_holder = [None]
    sampler = Sampler(pid_holder)
    sampler.start()
    t_start = time.time()
    try:
        p = run([IPERF, "-c", "127.0.0.1", "-p", "9001", "-t", str(DUR),
                 "-P", "1", "--json"], timeout=DUR + 40)
        rec["iperf_rc"] = p.returncode
        rec["iperf_wall_s"] = round(time.time() - t_start, 2)
        save("iperf-%s.json" % tag, p.stdout)
        save("iperf-%s.stderr.txt" % tag, p.stderr)
        if p.stderr.strip():
            rec["iperf_stderr"] = p.stderr.strip()
        try:
            j = json.loads(p.stdout)
            rec["client_receiver_mbps"] = round(
                j.get("end", {}).get("sum_received", {}).get("bits_per_second", 0) / 1e6, 3)
            rec["client_receiver_bytes"] = j.get("end", {}).get("sum_received", {}).get("bytes")
            rec["client_seconds"] = j.get("end", {}).get("sum_received", {}).get("seconds")
        except (ValueError, AttributeError):
            rec["client_json_error"] = True
    except subprocess.TimeoutExpired:
        rec["iperf_rc"] = -1
        rec["iperf_stderr"] = "timeout after %d s" % (DUR + 40)
        save("iperf-%s.stderr.txt" % tag, rec["iperf_stderr"])
    finally:
        sampler.stop.set()
        sampler.join(timeout=3)
        try:
            fout, ferr = fcpu.communicate(timeout=25)
        except subprocess.TimeoutExpired:
            fcpu.kill()
            fout, ferr = "", "far cpu sampler timeout"
    save("cpu-%s-far.csv" % tag, fout)
    rec["far_cpu_samples"] = parse_far_cpu(fout)
    rec["near_cpu_samples"] = sampler.samples
    rec["near_cpu"] = cpu_summary(sampler.samples, "busy_pct")
    rec["near_iperf_cpu"] = cpu_summary(sampler.samples, "iperf_cpu_pct")
    rec["far_cpu"] = cpu_summary(rec["far_cpu_samples"], "busy_pct")
    rec["far_iperf_cpu"] = cpu_summary(rec["far_cpu_samples"], "iperf_cpu_pct")

    after = {}
    for side, fn in (("near", near_cat), ("far", far_cat)):
        for name in ("stats", "peers", "tunnels"):
            p = fn(name)
            save("%s-%s-after-%s.txt" % (name, tag, side), p.stdout)
            after[side + "_" + name] = p.stdout
    rec["near_after"] = numeric(parse_kv(after["near_stats"]),
                                ["submit_bytes", "tx", "wire", "retx_frames",
                                 "retx_bytes", "retx_data_ppm", "dup_bytes_ppm",
                                 "holes_unrecoverable", "lost", "win_block",
                                 "rx_delivered", "rx_read_zero", "dup_bytes"])
    rec["far_after"] = numeric(parse_kv(after["far_stats"]),
                               ["rx_delivered", "rx_read_zero",
                                "holes_unrecoverable", "lost", "dup_bytes_ppm",
                                "submit_bytes", "wire", "retx_frames"])

    delta = {}
    for side in ("near", "far"):
        b = rec[side + "_before"]
        a = rec[side + "_after"]
        d = {}
        for k, v in a.items():
            if k in b and isinstance(v, int) and isinstance(b[k], int):
                d[k] = v - b[k]
            else:
                d[k] = v
        delta[side] = d
    rec["delta"] = delta

    inc = ssh("tail -c +%d %s" % (off + 1, FAR_LOG)).stdout
    save("server-%s.log" % tag, inc)
    r = parse_receiver(inc)
    if r:
        rec["receiver_bytes"], rec["receiver_seconds"], rec["receiver_mbps"] = (
            int(r[0]), round(r[1], 3), round(r[2], 3))
    else:
        rec["receiver_missing"] = True

    rec["ok"] = bool(rec.get("receiver_mbps") and rec.get("iperf_rc") == 0)
    return rec


def summarize(results):
    latest = {}
    for r in results:
        latest[r["tag"]] = r
    rows = {}
    for r in latest.values():
        key = (r["config"], r["run"])
        cur = rows.get(key)
        if cur is None or (not cur.get("ok") and r.get("ok")):
            rows[key] = r
    lines = ["# tbtun WAN matrix summary", "",
             "Receiver throughput is the HND0 iperf3 server report (bytes/duration).",
             "Client JSON receiver is reference only. Duration target: %d s." % DUR, "",
             "| config | run | receiver Mbit/s | client Mbit/s | wire/submit | retx_ppm | dup_ppm | holes_unrec | carrier_lost | CPU near avg/max % | CPU far avg/max % | near iperf % | far iperf % | status |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    order = [(c["cid"], i) for c in CONFIGS for i in range(1, RUNS + 1)]
    for cid, i in order:
        r = rows.get((cid, i))
        if not r:
            lines.append("| %s | %d | - | - | - | - | - | - | - | - | - | - | - | MISSING |" % (cid, i))
            continue
        d = r.get("delta", {})
        nd = d.get("near", {})
        fd = d.get("far", {})
        sub = nd.get("submit_bytes")
        wire = nd.get("wire")
        ratio = ("%.3f" % (wire / sub)) if sub else "-"
        near_cpu = r.get("near_cpu") or {}
        far_cpu = r.get("far_cpu") or {}
        ni = r.get("near_iperf_cpu") or {}
        fi = r.get("far_iperf_cpu") or {}
        status = "OK" if r.get("ok") else ("FAIL: " + str(r.get("error") or r.get("iperf_stderr") or "no receiver")[:60])
        lines.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s/%s | %s/%s | %s | %s | %s |" % (
            cid, i,
            r.get("receiver_mbps", "-"), r.get("client_receiver_mbps", "-"),
            ratio, nd.get("retx_data_ppm", "-"), nd.get("dup_bytes_ppm", "-"),
            nd.get("holes_unrecoverable", fd.get("holes_unrecoverable", "-")),
            nd.get("lost", fd.get("lost", "-")),
            near_cpu.get("avg", "-"), near_cpu.get("max", "-"),
            far_cpu.get("avg", "-"), far_cpu.get("max", "-"),
            ni.get("avg", "-"), fi.get("avg", "-"), status))
    lines.append("")
    return "\n".join(lines) + "\n"


def main():
    os.makedirs(OUT, exist_ok=True)
    probe = near_cat("config")
    if probe.returncode:
        sys.exit("near /proc/net/tbtun not readable via sudo -n: %r" % probe.stderr)

    results_path = os.path.join(OUT, "results.jsonl")
    results = []
    if os.path.exists(results_path):
        with open(results_path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        results.append(json.loads(line))
                    except ValueError:
                        pass

    done = set()
    for r in results:
        if r.get("ok"):
            done.add((r["config"], r["run"]))

    pb_idx = 0
    for cfg in CONFIGS:
        for run_idx in range(1, RUNS + 1):
            if (cfg["cid"], run_idx) in done:
                print("skip %s r%d (already ok)" % (cfg["cid"], run_idx))
                continue
            ok = False
            for attempt in (1, 2):
                pb_idx += 1
                print("run %s r%d attempt %d portblock %d-%d/%d-%d" % (
                    cfg["cid"], run_idx, attempt,
                    30100 + (pb_idx - 1) * 100, 30199 + (pb_idx - 1) * 100,
                    31100 + (pb_idx - 1) * 100, 31199 + (pb_idx - 1) * 100),
                    flush=True)
                try:
                    r = run_one(cfg, run_idx, attempt, pb_idx - 1)
                except Exception as exc:  # keep the matrix alive
                    r = {"tag": "%s-r%d-a%d" % (cfg["cid"], run_idx, attempt),
                         "config": cfg["cid"], "run": run_idx, "attempt": attempt,
                         "ok": False, "error": "%s: %s" % (type(exc).__name__, exc)}
                with open(results_path, "a") as f:
                    f.write(json.dumps(r) + "\n")
                results.append(r)
                print("  -> ok=%s receiver=%s client=%s rc=%s err=%s" % (
                    r.get("ok"), r.get("receiver_mbps"), r.get("client_receiver_mbps"),
                    r.get("iperf_rc"), r.get("error") or r.get("iperf_stderr")),
                    flush=True)
                if r.get("ok"):
                    ok = True
                    break
            if not ok:
                print("  !! %s r%d FAILED both attempts" % (cfg["cid"], run_idx),
                      flush=True)

    save("summary.md", summarize(results))
    print("summary written", flush=True)


if __name__ == "__main__":
    main()
