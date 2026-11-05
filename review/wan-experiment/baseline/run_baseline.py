#!/usr/bin/env python3
"""Direct-path TCP baseline: iperf3 3.20 client (local build) -> HND0 45.11.1.62:10000.

Saves raw JSON per run plus CPU samples. No system changes, no sudo.
"""
import json
import os
import subprocess
import sys
import threading
import time

OUT = "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/baseline"
IPERF = "/tmp/iperf3-local/bin/iperf3"
HOST = "45.11.1.62"
PORT = "10000"
DUR = 10


def cpu_times(pid=None):
    if pid is None:
        with open("/proc/stat") as f:
            parts = f.readline().split()
        vals = [int(x) for x in parts[1:]]
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
        return {"idle": idle, "total": sum(vals)}
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().split()
        # utime=14, stime=15 (1-based fields 14,15 -> index 13,14)
        return {"cpu": int(fields[13]) + int(fields[14])}
    except (FileNotFoundError, ProcessLookupError):
        return None


def sample(stop, samples, pid_holder):
    prev_m = cpu_times()
    prev_p = None
    t_prev = time.time()
    while not stop.is_set():
        time.sleep(0.25)
        cur_m = cpu_times()
        t_cur = time.time()
        dt = t_cur - t_prev
        if dt <= 0:
            continue
        busy = 100.0 * (1 - (cur_m["idle"] - prev_m["idle"]) / max(1, cur_m["total"] - prev_m["total"]))
        rec = {"t": round(t_cur - t0, 3), "machine_busy_pct": round(busy, 2)}
        pid = pid_holder[0]
        if pid:
            cur_p = cpu_times(pid)
            if cur_p and prev_p:
                rec["iperf3_cpu_pct_one_core"] = round(
                    100.0 * (cur_p["cpu"] - prev_p["cpu"]) / os.sysconf("SC_CLK_TCK") / dt, 2)
            prev_p = cur_p
        samples.append(rec)
        prev_m = cur_m
        t_prev = t_cur


t0 = time.time()
results = []
for par in (1, 4, 8):
    for run in (1, 2):
        tag = f"p{par}-r{run}"
        cmd = [IPERF, "-c", HOST, "-p", PORT, "-t", str(DUR), "-P", str(par), "--json"]
        stop = threading.Event()
        samples = []
        pid_holder = [None]
        th = threading.Thread(target=sample, args=(stop, samples, pid_holder), daemon=True)
        th.start()
        t_start = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pid_holder[0] = p.pid
        stdout, stderr = p.communicate()
        wall = time.time() - t_start
        stop.set()
        th.join(timeout=2)
        with open(f"{OUT}/iperf3-{tag}.json", "w") as f:
            f.write(stdout)
        with open(f"{OUT}/iperf3-{tag}.stderr.txt", "w") as f:
            f.write(stderr)
        rec = {"tag": tag, "par": par, "run": run, "rc": p.returncode,
               "wall_s": round(wall, 3), "samples": samples}
        try:
            j = json.loads(stdout)
            end = j.get("end", {})
            ss = end.get("sum_sent", {})
            sr = end.get("sum_received", {})
            streams = end.get("streams", [])
            rec.update({
                "sender_mbps": round(ss.get("bits_per_second", 0) / 1e6, 3),
                "receiver_mbps": round(sr.get("bits_per_second", 0) / 1e6, 3),
                "sent_bytes": ss.get("bytes"),
                "recv_bytes": sr.get("bytes"),
                "retransmits": ss.get("retransmits"),
                "sender_cpu_pct": ss.get("cpu_utilization_percent"),
                "receiver_cpu_pct": sr.get("cpu_utilization_percent"),
                "stream_retrans": [s.get("sender", {}).get("retransmits") for s in streams],
                "stream_mean_rtt_ms": [s.get("sender", {}).get("mean_rtt") for s in streams],
                "stream_max_rtt_ms": [s.get("sender", {}).get("max_rtt") for s in streams],
                "stream_min_rtt_ms": [s.get("sender", {}).get("min_rtt") for s in streams],
                "version": j.get("start", {}).get("version"),
            })
        except Exception as e:
            rec["parse_error"] = repr(e)
        results.append(rec)
        print(json.dumps(rec, indent=1), flush=True)

with open(f"{OUT}/summary.json", "w") as f:
    json.dump(results, f, indent=2)
