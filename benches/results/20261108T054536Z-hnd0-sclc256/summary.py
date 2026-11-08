#!/usr/bin/env python3
"""Build the HND0 SC-LDPC summary CSV from the five snapshot pairs."""
import sys

sys.path.insert(0, ".")
import analyze as a  # noqa: E402

ROWS = [
    # file, elapsed, carriers, mode, direction
    ("short256-5s.txt", 5.001626, 256, "sc-ldpc", "down"),
    ("steady256-60.txt", 60.001321, 256, "sc-ldpc", "down"),
    ("grouped32-5s.txt", 5.000837, 32, "grouped", "down"),
    ("grouped64-5s.txt", 5.001156, 64, "grouped", "down"),
    ("uplink256-60.txt", 60.040, 256, "sc-ldpc", "up"),
]
APP = {
    "short256-5s.txt": 14680064,
    "steady256-60.txt": 171966358,
    "grouped32-5s.txt": 14155776,
    "grouped64-5s.txt": 14155776,
    "uplink256-60.txt": 39321600,
}
COLS = ("name,carriers,mode,direction,elapsed_s,app_bytes,app_MB_s,app_Mbit_s,"
        "wire_bytes,wire_MB_s,wire_per_app,near_cpu_pct,far_cpu_pct,"
        "decode_side,decode_blocks,decode_blocks_per_s,decode_in_MB_s,"
        "outq_max,outq_bytes,replay_hit,replay_miss,nack_tx,nack_rx")


def d(x, y, k):
    return int(y.get(k, 0)) - int(x.get(k, 0))


def cpu_pct(b, x, el):
    cb, ca = a.cpu(b), a.cpu(x)
    bb = cb[0] + cb[1] + cb[2] + cb[5] + cb[6]
    ba = ca[0] + ca[1] + ca[2] + ca[5] + ca[6]
    return (ba - bb) / el / 100.0 * 100.0


print(COLS)
for name, el, carriers, mode, direction in ROWS:
    s = a.parse(name)
    nb, na, fb, fa = s["near_before"], s["near_after"], s["far_before"], s["far_after"]
    app = APP[name]
    # Wire is the sending side's carrier bytes: far for the downlink, near
    # for the uplink.
    wire = (a.peers_tx(fa) - a.peers_tx(fb)) if direction == "down" \
        else (a.peers_tx(na) - a.peers_tx(nb))
    if direction == "down":
        dec, dec_peer = na, nb
        tx, tx_peer = fa, fb          # far is the downlink sender
        outq = a.stat(tx, "tunnel 1 c11 ")
        nack_tx = d(a.stat(nb, "tunnel 1 c11 "), a.stat(na, "tunnel 1 c11 "), "fec_nack_tx")
        nack_rx = d(a.stat(fb, "tunnel 1 c11 "), a.stat(fa, "tunnel 1 c11 "), "fec_nack_rx")
        side = "near"
    else:
        dec, dec_peer = fa, fb
        tx, tx_peer = na, nb          # near is the uplink sender
        outq = a.stat(tx, "tunnel 1 c11 ")
        nack_tx = d(a.stat(fb, "tunnel 1 c11 "), a.stat(fa, "tunnel 1 c11 "), "fec_nack_tx")
        nack_rx = d(a.stat(nb, "tunnel 1 c11 "), a.stat(na, "tunnel 1 c11 "), "fec_nack_rx")
        side = "far"
    d10 = a.stat(dec, "tunnel 1 c10 ")
    d10p = a.stat(dec_peer, "tunnel 1 c10 ")
    drx = a.stat(dec, "tunnel 1 rx_work_runs")
    drxp = a.stat(dec_peer, "tunnel 1 rx_work_runs")
    blocks = d(d10p, d10, "fec_decode_blocks")
    drain = d(drxp, drx, "rx_drain_bytes")
    hit = d(a.stat(tx_peer, "tunnel 1 c11 "), outq, "fec_replay_hit")
    miss = d(a.stat(tx_peer, "tunnel 1 c11 "), outq, "fec_replay_miss")
    print("%s,%d,%s,%s,%.3f,%d,%.3f,%.2f,%d,%.3f,%.3f,%.1f,%.1f,%s,%d,%.1f,%.3f,%s,%s,%d,%d,%d,%d"
          % (name, carriers, mode, direction, el, app, app / el / 1e6, app * 8 / el / 1e6,
             wire, wire / el / 1e6, wire / app if app else 0,
             cpu_pct(nb, na, el), cpu_pct(fb, fa, el), side, blocks, blocks / el,
             drain / el / 1e6, outq.get("fec_outq_max"), outq.get("fec_outq_bytes"),
             hit, miss, nack_tx, nack_rx))
