#!/usr/bin/env python3
"""Render review/wan-experiment/matrix/summary.md from results.jsonl."""
import json
import os

OUT = "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/matrix"
BASE = {"P1": 17.08, "P4": 30.03, "P8": 33.72}
ORDER = ["c1-raw-n1m0d0", "c2-raw-n4m1d0", "c3-raw-n8m1d0",
         "c4-httpkal-n4m1d0", "c5-raw-n4m1d1"]
TITLE = {
    "c1-raw-n1m0d0": "raw n=1 m=0 dup=0",
    "c2-raw-n4m1d0": "raw n=4 m=1 dup=0",
    "c3-raw-n8m1d0": "raw n=8 m=1 dup=0",
    "c4-httpkal-n4m1d0": "http-kal n=4 m=1 dup=0",
    "c5-raw-n4m1d1": "raw n=4 m=1 dup=1",
}


def kv(path):
    d = {}
    if not os.path.exists(path):
        return d
    with open(path) as f:
        for line in f:
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    d.setdefault(k, v)
    return d


def num(d, key):
    try:
        return int(d[key])
    except (KeyError, ValueError):
        try:
            return float(d[key])
        except (KeyError, ValueError):
            return None


def fmt(v, nd=3):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%%.%df" % nd) % v
    return str(v)


rows = [json.loads(l) for l in open(os.path.join(OUT, "results.jsonl")) if l.strip()]
by_key = {}
for r in rows:
    by_key[(r["config"], r["run"])] = r

per_cfg = {}
for cid in ORDER:
    vals = [by_key[(cid, i)].get("receiver_mbps") for i in (1, 2)
            if (cid, i) in by_key and by_key[(cid, i)].get("ok")]
    per_cfg[cid] = sum(vals) / len(vals) if vals else None

L = []
L.append("# tbtun WAN matrix summary")
L.append("")
L.append("Date: 2026-11-06. Hosts: JYSZ 7.2.9-1-MANJARO (near), HND0 7.0.0-42-generic "
         "(far, 45.11.1.62). tbtun.ko loaded on both; far target 127.0.0.1:5201 "
         "(iperf3 server, PID 699110).")
L.append("")
L.append("Method: every run does `del 1` on both ends, writes far, writes near, "
         "waits 3 s, reads back the canonical config, then runs "
         "`iperf3 -c 127.0.0.1 -p 9001 -t 15 -P 1 --json`. Receiver throughput is "
         "the HND0 iperf3 server report (bytes / duration) from the server log, "
         "not the client JSON. Each run uses its own portblock range "
         "(near 30100+100i, far 31100+100i). All 10 runs succeeded; no EOF, no "
         "Broken pipe, no retry. `ip route get 45.11.1.62` resolved via "
         "192.168.123.254 dev wlp4s0 (no Meta/198.18.x) before and after the "
         "matrix; HND0 load stayed 0.19-0.73.")
L.append("")
L.append("Common parameters: `keepalive=2000 hole_budget_ms=30000 "
         "carrier_stall_ms=15000 replay=8388608 nonce=00112233445566778899aabbccddeeff`; "
         "far `listen=0.0.0.0:9000 target=127.0.0.1:5201`; "
         "near `listen=127.0.0.1:9001 peer=45.11.1.62:9000`.")
L.append("")
L.append("## Throughput and wire cost")
L.append("")
L.append("| config | run | receiver Mbit/s | client Mbit/s | wire/submit | retx_ppm | dup_ppm | holes_unrec | carrier_lost | status |")
L.append("|---|---|---|---|---|---|---|---|---|---|")
for cid in ORDER:
    for i in (1, 2):
        r = by_key.get((cid, i))
        if not r:
            L.append("| %s | %d | - | - | - | - | - | - | - | MISSING |" % (cid, i))
            continue
        nd = r.get("delta", {}).get("near", {})
        sub = nd.get("submit_bytes")
        wire = nd.get("wire")
        ratio = (wire / sub) if sub else None
        status = "OK" if r.get("ok") else "FAIL: " + str(
            r.get("error") or r.get("iperf_stderr") or "no receiver")[:60]
        L.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            cid, i, fmt(r.get("receiver_mbps")), fmt(r.get("client_receiver_mbps")),
            fmt(ratio), fmt(nd.get("retx_data_ppm")), fmt(nd.get("dup_bytes_ppm")),
            fmt(nd.get("holes_unrecoverable")), fmt(nd.get("lost")), status))
L.append("")
L.append("Notes: `retx_ppm` and `dup_ppm` are the near `retx_data_ppm` and "
         "`dup_bytes_ppm` window values, not cumulative ratios. `carrier_lost` is "
         "the near `lost` delta (carrier loss/replacement events), not lost data; "
         "`holes_unrecoverable` is 0 in every run.")
L.append("")
L.append("## Resources and receive queue")
L.append("")
L.append("| config | run | near busy avg/max % | far busy avg/max % | far iperf3 % | far rxq MiB | far rx stall ms | near txwake_busy % | near win_block |")
L.append("|---|---|---|---|---|---|---|---|---|")
for cid in ORDER:
    for i in (1, 2):
        r = by_key.get((cid, i))
        if not r:
            continue
        nc = r.get("near_cpu") or {}
        fc = r.get("far_cpu") or {}
        fi = r.get("far_iperf_cpu") or {}
        fa = kv(os.path.join(OUT, "stats-%s-after-far.txt" % r["tag"]))
        na = kv(os.path.join(OUT, "stats-%s-after-near.txt" % r["tag"]))
        rxq = num(fa, "rxq")
        stall = num(fa, "rx_max_stall_us")
        tw = num(na, "txwake")
        twb = num(na, "txwake_busy")
        twp = (100.0 * twb / tw) if tw else None
        L.append("| %s | %d | %s/%s | %s/%s | %s | %s | %s | %s | %s |" % (
            cid, i, fmt(nc.get("avg"), 1), fmt(nc.get("max"), 1),
            fmt(fc.get("avg"), 1), fmt(fc.get("max"), 1), fmt(fi.get("avg"), 2),
            fmt((rxq or 0) / 1048576.0, 2) if rxq is not None else "-",
            fmt((stall or 0) / 1000.0, 1) if stall is not None else "-",
            fmt(twp, 1), fmt(num(na, "win_block"))))
L.append("")
L.append("## Direct-WAN baseline comparison")
L.append("")
L.append("The baseline numbers are iperf3 receiver means with 1/4/8 parallel TCP "
         "streams to HND0 (see `../baseline-direct`). The matrix above always uses "
         "`-P 1`; `n` is the number of tbtun carriers, not iperf3 streams. The "
         "comparable baseline is P1; P4/P8 are shown as reference ceilings.")
L.append("")
L.append("| config | tbtun mean Mbit/s | vs baseline P1 17.08 | vs baseline P4 30.03 | vs baseline P8 33.72 |")
L.append("|---|---|---|---|---|")
for cid in ORDER:
    v = per_cfg.get(cid)
    if v is None:
        continue
    L.append("| %s | %s | %.1f%% | %.1f%% | %.1f%% |" % (
        cid, fmt(v), 100 * v / BASE["P1"], 100 * v / BASE["P4"], 100 * v / BASE["P8"]))
L.append("")
L.append("All configurations are far below the direct baseline. The best matrix "
         "result (c5, dup=1) is still only %.1f%% of direct P1; the n=8 result is "
         "%.1f%% of direct P8." % (100 * per_cfg["c5-raw-n4m1d1"] / BASE["P1"],
                                   100 * per_cfg["c3-raw-n8m1d0"] / BASE["P8"]))
L.append("")
L.append("## Effect of each knob")
L.append("")
L.append("### n (carrier count)")
L.append("")
L.append("n=1 %.2f, n=4 %.2f, n=8 %.2f Mbit/s (means). Going from 1 to 8 carriers "
         "adds about %.0f%%, while the direct path gains about 97%% from 1 to 8 "
         "parallel streams. The near peer tables show why: total in-flight bytes "
         "across all carriers stay at 120-131 KB regardless of n (122880 B at n=1; "
         "122880/130807 B at n=4; 130860/131072 B at n=8), at RTT 234-300 ms. "
         "120 KB * 8 / 240-300 ms = 3.3-4.1 Mbit/s, which is the measured range "
         "(3.45-4.27). Adding carriers redistributes the same window instead of "
         "enlarging it. The far side also ends n>=4 runs with 3.1-3.7 MiB still "
         "queued (`rxq`) and 263-602 ms receive stalls, so the window is not "
         "released faster either. More carriers add reordering work, not delivered "
         "throughput." % (
             per_cfg["c1-raw-n1m0d0"], per_cfg["c2-raw-n4m1d0"],
             per_cfg["c3-raw-n8m1d0"],
             100 * (per_cfg["c3-raw-n8m1d0"] / per_cfg["c1-raw-n1m0d0"] - 1)))
L.append("")
L.append("### m=1 (spare carrier)")
L.append("")
L.append("c2/c3 run with m=1. `spare_activated=1` in c2-r1 and c3-r1; the spare "
         "also carries traffic (`tx_bytes` 348 KB / 592 KB at the near peers), so it "
         "is not a pure idle standby. m=1 does not raise throughput; its value is "
         "redundancy: every raw run lost carriers during the test, yet "
         "`holes_unrecoverable=0` throughout. The m=0 baseline (c1) also lost its "
         "only carrier once and recovered.")
L.append("")
L.append("### dup=1 (near-side duplication)")
L.append("")
L.append("c5 (dup=1) shows the cost directly: `wire/submit` 2.018 / 2.016 versus "
         "1.038 / 1.041 for c2 (dup=0); near `dup_bytes` 9.28 MB for 9.63 MB "
         "submitted, `inflate_ppm` 1.96e6. c5's total in-flight is larger (189175 B "
         "r1, 139264 B r2) than c2's 120-131 KB, which raises the window/RTT "
         "ceiling and is the plausible mechanism for the higher numbers. Receiver "
         "4.07 / 5.04 Mbit/s (mean 4.55) is 25% above c2's mean, but c5 ran with "
         "much lower external near load "
         "(near busy 10.9% / 34.5% versus 83.0% / 53.6% for c2) and c5-r2 ended "
         "with `rxq=0` while both c2 runs ended with 3.4-3.7 MiB backlog. With two "
         "samples and this confounding, the matrix does not establish a throughput "
         "gain for dup; the doubling of wire bytes is unambiguous.")
L.append("")
L.append("### http-kal")
L.append("")
L.append("http-kal (c4) gives 4.01 / 2.27 Mbit/s (mean 3.14), 14% below raw n=4 "
         "(3.65). The HTTP container is visible: near `http_req=5`, far "
         "`http_resp=5`, `pad_tx`/`drip_tx` non-zero, and wire/submit 1.052 / 1.118 "
         "versus 1.040 for raw. Its advantages are stability and lower far CPU: "
         "`carrier_lost=0` (every raw run lost carriers), `rxq=0`, no receive "
         "stall, far busy 9.5-9.9% versus 12-37% for raw. c4-r2's 2.27 Mbit/s came "
         "with 55258 ppm retransmission. No HTTP QoS gain is visible on this path.")
L.append("")
L.append("## Anomalies and limitations")
L.append("")
L.append("1. **raw carriers are lost and rebuilt during every raw run.** Before "
         "stats show `lost=0`; after stats show `lost` deltas of 1/5/9/8 (r1) and "
         "1/5/9/7 (r2) for c1/c2/c3/c5, i.e. about one loss per carrier for c1-c3 "
         "and 7-8 events over 5 carriers for c5 (dup=1), with matching `replaced` "
         "and changed source ports. http-kal shows `lost=0`. No data was lost (`holes_unrecoverable=0`), "
         "but each rebuild costs a 263-602 ms far receive stall. Working hypothesis "
         "to check later: raw carriers emit no PAD (`c8 pad_tx=0`) and "
         "`carrier_stall_ms=15000` coincides with the 15 s test window, so the stall "
         "timer may not be reset by data/ACK. This matrix did not change the common "
         "parameters to test that hypothesis.")
L.append("2. **The 15 s window does not reach steady state for most raw n>=4 "
         "runs (c2 both, c3 both, c5-r1).** Near "
         "submits 9.3-10.4 MB, far delivers 6.6-8.2 MB, and 3.1-3.7 MiB remain in "
         "`far rxq` when the client stops. The server-side receiver number is "
         "therefore a lower bound on what the tunnel could deliver with a longer run.")
L.append("3. **External near load.** Throughout the matrix the near host ran "
         "unrelated `cc1plus` / `test_multistream` / `test_nv12_pipeline` jobs "
         "(1-min load 3.5-9.0, machine busy 10.9-85%). They were not killed. The "
         "near CPU columns are machine-wide and are contaminated. The direct "
         "baseline was measured under similar load (`local_busy_median` 54.1%, "
         "machine busy near 100%) and still reached 17-34 Mbit/s, so load alone does "
         "not explain the ~4 Mbit/s tunnel result.")
L.append("4. **near iperf3 process CPU was not captured by the matrix sampler** "
         "(baseline initialized before the child existed). A supplementary 15 s run "
         "of c2 with the fixed sampler (`supp-c2-near-iperf-cpu.txt`) measured "
         "0.0% of one core (31 samples), machine busy 33.3%; that run reached "
         "5.38 Mbit/s. far iperf3 process CPU was 0.17-0.56% in the matrix. The "
         "client is not the bottleneck.")
L.append("5. **far `dup_bytes_ppm=1000000` under http-kal is a stale window value**; "
         "the near-side dup counters are the meaningful send-side figures. This "
         "field is not used for any conclusion above.")
L.append("6. All runs used 15 s as specified. Shorter runs had produced EOF during "
         "deployment; no failure occurred here.")
L.append("")
L.append("## Conclusion")
L.append("")
L.append("- tbtun does **not** reach the direct-WAN baseline. Single-stream receiver "
         "throughput is 3.14-4.55 Mbit/s, 18-27% of direct P1 (17.08) and 9-15% of "
         "direct P8 (33.72).")
L.append("- Increasing n from 1 to 8, adding m=1, enabling dup=1, or switching to "
         "http-kal produced no proportional throughput gain. dup=1 doubles wire "
         "bytes; http-kal removes carrier losses but is slower than raw here.")
L.append("- The measured limit is the tunnel's total in-flight window, about "
         "120-131 KB, which does not grow with n; at 234-300 ms RTT that caps "
         "single-stream throughput near 3.3-4.1 Mbit/s. far-side ordered delivery "
         "(`rxq` 3.1-3.7 MiB, stalls 0.26-0.60 s) and raw carrier rebuilds during "
         "the test compound it.")
L.append("- Follow-up (not done here): inspect raw carrier stall-timer reset and PAD "
         "behaviour, raise `rx_reorder_bytes` / carrier window, and rerun with a "
         "longer steady-state window on an idle near host.")
L.append("")
L.append("## Server log slices")
L.append("")
L.append("| tag | start offset | slice bytes |")
L.append("|---|---|---|")
for cid in ORDER:
    for i in (1, 2):
        r = by_key.get((cid, i))
        if not r:
            continue
        path = os.path.join(OUT, "server-%s.log" % r["tag"])
        size = os.path.getsize(path) if os.path.exists(path) else 0
        L.append("| %s | %s | %d |" % (r["tag"], r.get("server_log_offset", "-"), size))
L.append("")
L.append("## Raw data index")
L.append("")
L.append("`results.jsonl` (per-run parsed record), `config-*.txt`, `stats-*.txt`, "
         "`peers-*.txt`, `tunnels-*.txt`, `iperf-*.json` + `.stderr.txt`, "
         "`server-*.log` (server log slice per run), `cpu-*-far.csv`, "
         "`env-before.txt`, `console.log`, `supp-c2-near-iperf-cpu.txt`.")
L.append("")

with open(os.path.join(OUT, "summary.md"), "w") as f:
    f.write("\n".join(L))
print("wrote summary.md (%d lines)" % len(L))
