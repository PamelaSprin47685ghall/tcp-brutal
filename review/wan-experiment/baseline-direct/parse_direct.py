#!/usr/bin/env python3
"""Summarize direct-WAN baseline, aligning HND0 CPU samples by epoch."""
import json
import statistics
import os

BASE = "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment"
OUT = f"{BASE}/baseline-direct"
CLK_TCK = 100

rows = json.load(open(f"{OUT}/summary.json"))

hnd0 = []
path = f"{OUT}/hnd0-cpu.log"
if os.path.exists(path):
    for line in open(path):
        p = line.split()
        if len(p) == 4:
            hnd0.append((int(p[0]) / 1000.0, int(p[1]), int(p[2]), int(p[3])))

out = []
print("| run | sender Mbit/s | receiver Mbit/s | retrans | mean RTT ms | local iperf3 CPU s | HND0 busy% | HND0 iperf3 CPU s |")
print("|---|---|---|---|---|---|---|---|")
for r in rows:
    win = [s for s in hnd0 if r["start_epoch"] <= s[0] <= r["end_epoch"]]
    busy = None
    scpu_s = None
    if len(win) >= 2:
        dt = win[-1][1] - win[0][1]
        di = win[-1][2] - win[0][2]
        busy = round(100.0 * (1 - di / dt), 1) if dt else None
        s0 = win[0][3]
        s1 = win[-1][3]
        if s0 >= 0 and s1 >= 0:
            scpu_s = round((s1 - s0) / CLK_TCK, 3)
    rtts = r.get("stream_mean_rtt_us") or []
    rtt_ms = [round(x / 1000.0, 1) for x in rtts]
    local_busy = [s["machine_busy_pct"] for s in r["samples"]]
    out.append({"tag": r["tag"], "par": r["par"], "sender_mbps": r["sender_mbps"],
                "receiver_mbps": r["receiver_mbps"], "retransmits": r["retransmits"],
                "rtt_ms": rtt_ms, "local_iperf3_cpu_s": r.get("local_iperf3_cpu_s"),
                "hnd0_busy_pct": busy, "hnd0_iperf3_cpu_s": scpu_s,
                "local_busy_median": round(statistics.median(local_busy), 1) if local_busy else None,
                "recv_interval_mbps": r.get("recv_interval_mbps"),
                "sent_MiB": round((r.get("sent_bytes") or 0) / 2**20, 1),
                "recv_MiB": round((r.get("recv_bytes") or 0) / 2**20, 1)})
    print(f"| {r['tag']} | {r['sender_mbps']:.2f} | {r['receiver_mbps']:.2f} | {r['retransmits']} | {rtt_ms} | {r.get('local_iperf3_cpu_s')} | {busy} | {scpu_s} |")

print("\nmeans:")
for p in (1, 4, 8):
    sub = [x for x in out if x["par"] == p]
    print(f"P{p}: receiver={statistics.mean([x['receiver_mbps'] for x in sub]):.2f} "
          f"sender={statistics.mean([x['sender_mbps'] for x in sub]):.2f} "
          f"retrans={[x['retransmits'] for x in sub]} "
          f"hnd0_busy={[x['hnd0_busy_pct'] for x in sub]}")

json.dump(out, open(f"{OUT}/parsed.json", "w"), indent=2)
