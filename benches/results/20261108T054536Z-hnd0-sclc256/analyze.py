#!/usr/bin/env python3
"""Summarise one near/far snapshot pair: app/wire/CPU/decode/outq/replay/NACK.

The input file has the shape

    @@near_before
    @@stats / @@peers / @@cpu / @@clk
    @@near_after
    ...

so phase markers switch the target snapshot and section markers switch the
section inside it.
"""
import re
import sys

PHASES = {"near_before", "near_after", "far_before", "far_after"}
SECTIONS = {"stats", "peers", "cpu", "clk"}


def parse(path):
    snaps = {p: {} for p in PHASES}
    phase = section = None
    for ln in open(path):
        m = re.match(r"^@@(\w+)\s*$", ln)
        if m and m.group(1) in PHASES:
            phase = m.group(1)
            section = None
            continue
        if m and m.group(1) in SECTIONS:
            section = m.group(1)
            snaps[phase].setdefault(section, "")
            continue
        if phase and section:
            snaps[phase][section] += ln
    return snaps


def lines(s):
    return [ln for ln in s.splitlines() if ln.strip()]


def kv(line):
    d = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def stat(snap, prefix):
    for ln in lines(snap.get("stats", "")):
        if ln.startswith(prefix):
            return kv(ln)
    return {}


def peers_tx(snap):
    total = 0
    for ln in lines(snap.get("peers", "")):
        f = ln.split()
        if len(f) >= 14:
            try:
                total += int(f[13])
            except ValueError:
                pass
    return total


def cpu(snap):
    for ln in lines(snap.get("cpu", "")):
        if ln.startswith("cpu "):
            return [int(x) for x in ln.split()[1:]]
    return None


def main():
    path, elapsed = sys.argv[1], float(sys.argv[2])
    s = parse(path)
    nb, na, fb, fa = s["near_before"], s["near_after"], s["far_before"], s["far_after"]

    n10b, n10a = stat(nb, "tunnel 1 c10 "), stat(na, "tunnel 1 c10 ")
    nrxb, nrxa = stat(nb, "tunnel 1 rx_work_runs"), stat(na, "tunnel 1 rx_work_runs")
    n11b, n11a = stat(nb, "tunnel 1 c11 "), stat(na, "tunnel 1 c11 ")
    f11b, f11a = stat(fb, "tunnel 1 c11 "), stat(fa, "tunnel 1 c11 ")
    nc3a = stat(na, "tunnel 1 c3 ")

    blocks = int(n10a.get("fec_decode_blocks", 0)) - int(n10b.get("fec_decode_blocks", 0))
    recovered = int(n10a.get("fec_decode_recovered", 0)) - int(n10b.get("fec_decode_recovered", 0))
    wire = peers_tx(fa) - peers_tx(fb)
    nack_tx = int(n11a.get("fec_nack_tx", 0)) - int(n11b.get("fec_nack_tx", 0))
    nack_rx = int(f11a.get("fec_nack_rx", 0)) - int(f11b.get("fec_nack_rx", 0))
    # Downlink repair is the far sender's replay ring; the near ring only
    # serves the uplink ACK/NACK direction.
    frhit = int(f11a.get("fec_replay_hit", 0)) - int(f11b.get("fec_replay_hit", 0))
    frmiss = int(f11a.get("fec_replay_miss", 0)) - int(f11b.get("fec_replay_miss", 0))
    nrhit = int(n11a.get("fec_replay_hit", 0)) - int(n11b.get("fec_replay_hit", 0))
    nrmiss = int(n11a.get("fec_replay_miss", 0)) - int(n11b.get("fec_replay_miss", 0))

    def cpupct(b, a):
        cb, ca = cpu(b), cpu(a)
        if not cb or not ca:
            return 0.0
        busy_b = cb[0] + cb[1] + cb[2] + cb[5] + cb[6]
        busy_a = ca[0] + ca[1] + ca[2] + ca[5] + ca[6]
        return (busy_a - busy_b) / elapsed / 100.0 * 100.0

    print("elapsed_s=%.3f" % elapsed)
    # Decoder input rate: every wire byte the near side drained is a byte the
    # codec saw (DATA and PARITY both enter the block buffers). This口径 is
    # independent of the code family, unlike a block-capacity estimate.
    drain = int(nrxa.get("rx_drain_bytes", 0)) - int(nrxb.get("rx_drain_bytes", 0))
    print("decode_blocks=%d decode_blocks_per_s=%.1f decode_in_MB_s=%.3f "
          "decode_in_bytes_per_block=%d recovered=%d"
          % (blocks, blocks / elapsed, drain / 1e6 / elapsed,
             drain // blocks if blocks else 0, recovered))
    print("wire_bytes=%d wire_MB_s=%.3f" % (wire, wire / elapsed / 1e6))
    print("nack_tx(near)=%d nack_rx(far)=%d far_replay_hit=%d far_replay_miss=%d near_replay_hit=%d near_replay_miss=%d"
          % (nack_tx, nack_rx, frhit, frmiss, nrhit, nrmiss))
    print("near_cpu_pct=%.1f far_cpu_pct=%.1f" % (cpupct(nb, na), cpupct(fb, fa)))
    print("near_outq_max=%s near_outq_bytes=%s far_outq_max=%s far_outq_bytes=%s"
          % (n11a.get("fec_outq_max"), n11a.get("fec_outq_bytes"),
             f11a.get("fec_outq_max"), f11a.get("fec_outq_bytes")))
    print("far_replay_bytes=%s far_replay_queued=%s far_outq_ctl=%s"
          % (f11a.get("fec_replay_bytes"), f11a.get("fec_replay_queued"),
             f11a.get("fec_outq_ctl")))
    print("near_up=%s near_lost=%s near_replaced=%s far_up=%s"
          % (nc3a.get("up"), nc3a.get("lost"), nc3a.get("replaced"),
             stat(fa, "tunnel 1 c3 ").get("up")))


if __name__ == "__main__":
    main()
