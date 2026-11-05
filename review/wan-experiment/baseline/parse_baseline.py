#!/usr/bin/env python3
"""Summarize saved baseline JSON: per-run rates, retransmits, RTT, interval series."""
import json
import glob
import os
import statistics

OUT = "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/baseline"

rows = []
for path in sorted(glob.glob(f"{OUT}/iperf3-p*-r*.json")):
    tag = os.path.basename(path)[len("iperf3-"):-len(".json")]
    with open(path) as f:
        j = json.load(f)
    end = j.get("end", {})
    ss = end.get("sum_sent", {})
    sr = end.get("sum_received", {})
    streams = end.get("streams", [])
    recv_iv = [iv["sum"]["bits_per_second"] / 1e6 for iv in j.get("intervals", [])]
    send_iv = [iv["sum"]["bits_per_second"] / 1e6 for iv in j.get("intervals", [])]
    rows.append({
        "tag": tag,
        "sender_mbps": round(ss.get("bits_per_second", 0) / 1e6, 2),
        "receiver_mbps": round(sr.get("bits_per_second", 0) / 1e6, 2),
        "sent_MiB": round(ss.get("bytes", 0) / 2**20, 1),
        "recv_MiB": round(sr.get("bytes", 0) / 2**20, 1),
        "retransmits": ss.get("retransmits"),
        "mean_rtt_us": [s.get("sender", {}).get("mean_rtt") for s in streams],
        "min_rtt_us": [s.get("sender", {}).get("min_rtt") for s in streams],
        "max_rtt_us": [s.get("sender", {}).get("max_rtt") for s in streams],
        "snd_cwnd": [s.get("sender", {}).get("max_snd_cwnd") for s in streams],
        "recv_interval_mbps": [round(x, 1) for x in recv_iv],
        "send_interval_mbps": [round(x, 1) for x in send_iv],
        "keys_sum_sent": sorted(ss.keys()),
        "keys_stream_sender": sorted(streams[0]["sender"].keys()) if streams else [],
    })

print(json.dumps(rows, indent=1))
with open(f"{OUT}/parsed.json", "w") as f:
    json.dump(rows, f, indent=2)

# Console table
print("\n| run | sender Mbit/s | receiver Mbit/s | retrans | recv/s interval Mbit/s |")
print("|---|---|---|---|---|")
for r in rows:
    print(f"| {r['tag']} | {r['sender_mbps']} | {r['receiver_mbps']} | {r['retransmits']} | {r['recv_interval_mbps']} |")
