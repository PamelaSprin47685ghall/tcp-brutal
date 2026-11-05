#!/usr/bin/env python3
"""Direct-WAN TCP baseline: iperf3 3.20 -> HND0 45.11.1.62:10000, bypassing mihomo.

Records per-run JSON, wall-clock epochs (for aligning HND0 CPU samples),
local /proc/stat samples, and local iperf3 process CPU.
"""
import json
import os
import subprocess
import threading
import time

OUT = "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/baseline-direct"
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
        return {"cpu": int(fields[13]) + int(fields[14])}
    except (FileNotFoundError, ProcessLookupError):
        return None


t0 = time.time()
results = []
for par in (1, 4, 8):
    for run in (1, 2):
        tag = f"p{par}-r{run}"
        cmd = [IPERF, "-c", HOST, "-p", PORT, "-t", str(DUR), "-P", str(par), "--json"]
        stop = threading.Event()
        samples = []
        pid_holder = [None]
        proc_cpu_before = [None]

        def sample():
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

        th = threading.Thread(target=sample, daemon=True)
        th.start()
        t_start_epoch = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        pid_holder[0] = p.pid
        proc_cpu_before[0] = cpu_times(p.pid)
        stdout, stderr = p.communicate()
        t_end_epoch = time.time()
        proc_cpu_after = cpu_times(p.pid)
        wall = t_end_epoch - t_start_epoch
        stop.set()
        th.join(timeout=2)
        with open(f"{OUT}/iperf3-{tag}.json", "w") as f:
            f.write(stdout)
        with open(f"{OUT}/iperf3-{tag}.stderr.txt", "w") as f:
            f.write(stderr)
        proc_cpu_s = None
        if proc_cpu_before[0] and proc_cpu_after:
            proc_cpu_s = round((proc_cpu_after["cpu"] - proc_cpu_before[0]["cpu"]) / os.sysconf("SC_CLK_TCK"), 3)
        rec = {"tag": tag, "par": par, "run": run, "rc": p.returncode,
               "wall_s": round(wall, 3),
               "start_epoch": round(t_start_epoch, 3), "end_epoch": round(t_end_epoch, 3),
               "local_iperf3_cpu_s": proc_cpu_s,
               "samples": samples}
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
                "stream_retrans": [s.get("sender", {}).get("retransmits") for s in streams],
                "stream_mean_rtt_us": [s.get("sender", {}).get("mean_rtt") for s in streams],
                "stream_max_rtt_us": [s.get("sender", {}).get("max_rtt") for s in streams],
                "stream_min_rtt_us": [s.get("sender", {}).get("min_rtt") for s in streams],
                "stream_cwnd": [s.get("sender", {}).get("max_snd_cwnd") for s in streams],
                "recv_interval_mbps": [round(iv["sum"]["bits_per_second"] / 1e6, 2) for iv in j.get("intervals", [])],
                "send_interval_mbps": [round(iv["sum"]["bits_per_second"] / 1e6, 2) for iv in j.get("intervals", [])],
                "version": j.get("start", {}).get("version"),
            })
        except Exception as e:
            rec["parse_error"] = repr(e)
        results.append(rec)
        print(json.dumps(rec, indent=1), flush=True)

with open(f"{OUT}/summary.json", "w") as f:
    json.dump(results, f, indent=2)
