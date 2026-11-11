#!/usr/bin/env python3
"""gen_sclc_delta.py - generate tbt_fec_sclc_delta.h from calibration CSVs.

The downlink table is the density-evolution margin delta(n) for the 1x256
SC-LDPC code, n = 1..192, stored as Q16.16 shares. The uplink table is the
conservative core bound m - floor(8192/nsym) for the 8x32 GF(16) code at
w = 32, stored as Q16.16 shares. The finite-length grid is
delta(P) = 0.60 * Qinv(P) / 16 shares over 256 shares.

Usage:
    python3 tools/gen_sclc_delta.py tests/calib/sclc-de.csv \
        tests/calib/sclc-up.csv src/kernel/tbt_fec_sclc_delta.h

Pass --check to compare the generated text with the file on disk without
writing it; a mismatch prints a unified diff and exits 1.
"""
import argparse
import csv
import difflib
import re
import statistics
import sys

DOWN_N = 192
UP_N = 24
LEN_N = 15

# Constructible uplink payload counts at w = 32, in ascending order.
UP_SET = (1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
          21, 22, 23, 24, 25, 28, 30)
UP_ANCHORS = {8: 22, 9: 21, 12: 17, 16: 12, 18: 10, 22: 5}
LEN_EPS_PPM = (500, 750, 1000, 1500, 2000, 3000, 4000, 5000, 7000,
               10000, 15000, 20000, 30000, 40000, 50000)
LEN_ANCHORS = {1000: 29.7, 10000: 22.3}
LEN_TOL = 0.2


def die(message):
    sys.exit("gen_sclc_delta: " + message)


def read_down(path):
    """Return delta(n) in Q16.16 for n = 1..192."""
    with open(path, newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != DOWN_N:
        die("%s: expected %d rows, got %d" % (path, DOWN_N, len(rows)))
    by_n = {}
    for row in rows:
        n = int(row["n"])
        if n in by_n:
            die("%s: duplicate n=%d" % (path, n))
        by_n[n] = round(float(row["delta"]) * 65536.0)
    if sorted(by_n) != list(range(1, DOWN_N + 1)):
        die("%s: n is not exactly 1..%d" % (path, DOWN_N))
    for n, q16 in by_n.items():
        if q16 <= 0 or q16 > 0xffffffff:
            die("%s: n=%d delta does not fit u32" % (path, n))
    return [by_n[n] for n in range(1, DOWN_N + 1)]


def read_up(path):
    """Return (n list, delta_core Q16.16 list) for the w = 32 scan."""
    with open(path, newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    found = {}
    for row in rows:
        if row["section"] != "construct" or row["param"] != "w=32":
            continue
        n = int(row["n"])
        if n not in UP_SET:
            continue
        match = re.search(r"nsym=(\d+)", row["note"])
        if not match:
            die("%s: n=%d has no nsym in the note" % (path, n))
        if n in found:
            die("%s: duplicate construct row for n=%d" % (path, n))
        m = int(row["m"])
        k_core = 8192 // int(match.group(1))
        found[n] = max(0, m - k_core) * 65536
    missing = [n for n in UP_SET if n not in found]
    if missing:
        die("%s: no w=32 construct row for n=%s" % (path, missing))
    for n, want in UP_ANCHORS.items():
        got = found[n] // 65536
        if got != want:
            die("%s: anchor n=%d is %d, expected %d" % (path, n, got, want))
    return list(UP_SET), [found[n] for n in UP_SET]


def len_grid():
    """Return (eps ppm list, Q16.16 shares over 256 list)."""
    dist = statistics.NormalDist()
    shares = []
    for ppm in LEN_EPS_PPM:
        delta = 0.60 * dist.inv_cdf(1.0 - ppm / 1e6) / 16.0
        shares.append(delta * 256.0)
    for ppm, want in LEN_ANCHORS.items():
        got = shares[LEN_EPS_PPM.index(ppm)]
        if abs(got - want) > LEN_TOL:
            die("finite-length anchor at %d ppm is %.3f, expected %.1f"
                % (ppm, got, want))
    q16 = [round(value * 65536.0) for value in shares]
    for value in q16:
        if value <= 0 or value > 0xffffffff:
            die("finite-length value does not fit u32")
    return list(LEN_EPS_PPM), q16


def wrap_ints(values, per_line):
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        text = ", ".join(str(value) for value in chunk)
        if start + per_line < len(values):
            text += ","
        lines.append("    " + text)
    return "\n".join(lines)


def render(down, up_n, up_delta, len_eps, len_q16):
    parts = []
    parts.append(
        "/* tbt_fec_sclc_delta.h - generated SC-LDPC margin tables. "
        "DO NOT EDIT.\n"
        " *\n"
        " * Sources, both under tests/calib:\n"
        " *   sclc-de.csv  commit b55ef67: BEC density evolution, "
        "downlink n=1..192\n"
        " *   sclc-up.csv  commit 4d072ce: 8x32 GF(16) construction scan, "
        "w=32\n"
        " *\n"
        " * Regenerate from the repository root:\n"
        " *   python3 tools/gen_sclc_delta.py tests/calib/sclc-de.csv \\\n"
        " *       tests/calib/sclc-up.csv src/kernel/tbt_fec_sclc_delta.h\n"
        " * Pass --check to compare without writing; a mismatch exits 1.\n"
        " *\n"
        " * down_q16      round(delta(n) * 65536) from the density "
        "evolution.\n"
        " * up_delta_q16  max(0, m - floor(8192 / nsym)) * 65536: the\n"
        " *               conservative 256-variable core bound. Zero means\n"
        " *               the online surplus correction must add margin.\n"
        " * len_q16       round(delta(P) * 256 * 65536), where\n"
        " *               delta(P) = 0.60 * Qinv(P) / 16 and "
        "P = eps_ppm / 1e6.\n"
        " */\n")
    parts.append("#ifndef _TBT_FEC_SCLC_DELTA_H\n"
                 "#define _TBT_FEC_SCLC_DELTA_H\n\n"
                 "#include \"tbt_uapi.h\"\n\n")
    parts.append("/* Downlink n in 1..192, n + m = 256. Index n - 1. */\n"
                 "#define TBT_FEC_SCLC_DELTA_DOWN_N %d\n"
                 "static const u32 tbt_fec_sclc_delta_down_q16[%d] = {\n"
                 "%s\n"
                 "};\n\n"
                 % (DOWN_N, DOWN_N, wrap_ints(down, 6)))
    parts.append("/* Uplink constructible n at w = 32, n + m = 32. */\n"
                 "#define TBT_FEC_SCLC_UP_N %d\n"
                 "static const u8 tbt_fec_sclc_up_n[%d] = {\n"
                 "%s\n"
                 "};\n\n"
                 % (UP_N, UP_N, wrap_ints(up_n, 12)))
    parts.append("/* Uplink conservative core margin, parallel to up_n. */\n"
                 "static const u32 tbt_fec_sclc_up_delta_q16[%d] = {\n"
                 "%s\n"
                 "};\n\n"
                 % (UP_N, wrap_ints(up_delta, 6)))
    parts.append("/* Finite-length margin over 256 shares. */\n"
                 "#define TBT_FEC_SCLC_LEN_N %d\n"
                 "static const u32 tbt_fec_sclc_len_eps_ppm[%d] = {\n"
                 "%s\n"
                 "};\n"
                 "static const u32 tbt_fec_sclc_len_q16[%d] = {\n"
                 "%s\n"
                 "};\n\n"
                 % (LEN_N, LEN_N, wrap_ints(len_eps, 6),
                    LEN_N, wrap_ints(len_q16, 6)))
    parts.append("#endif /* _TBT_FEC_SCLC_DELTA_H */\n")
    return "".join(parts)


def main():
    parser = argparse.ArgumentParser(
        description="Generate tbt_fec_sclc_delta.h from the calibration "
                    "CSVs.")
    parser.add_argument("down_csv")
    parser.add_argument("up_csv")
    parser.add_argument("header")
    parser.add_argument("--check", action="store_true",
                        help="compare with the existing header; exit 1 on "
                             "a mismatch")
    args = parser.parse_args()

    down = read_down(args.down_csv)
    up_n, up_delta = read_up(args.up_csv)
    len_eps, len_q16 = len_grid()
    text = render(down, up_n, up_delta, len_eps, len_q16)

    if args.check:
        try:
            with open(args.header, encoding="utf-8") as stream:
                current = stream.read()
        except OSError as error:
            print("gen_sclc_delta: %s: %s" % (args.header, error))
            return 1
        if current == text:
            print("%s: up to date" % args.header)
            return 0
        diff = difflib.unified_diff(
            current.splitlines(True), text.splitlines(True),
            fromfile=args.header, tofile=args.header + " (generated)")
        sys.stdout.writelines(diff)
        return 1

    with open(args.header, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)
    print("wrote %s" % args.header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
