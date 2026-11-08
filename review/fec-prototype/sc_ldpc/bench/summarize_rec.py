#!/usr/bin/env python3
"""Aggregate the per-seed recovery runs with a Wilson 99% interval."""
import argparse
import csv
import math
import statistics
from collections import defaultdict

Z = 2.5758293035489004


def wilson(p, n):
    if n <= 0:
        return 0.0, 1.0
    den = 1.0 + Z * Z / n
    center = (p + Z * Z / (2.0 * n)) / den
    half = Z / den * math.sqrt(p * (1.0 - p) / n + Z * Z / (4.0 * n * n))
    return max(0.0, center - half), min(1.0, center + half)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("outdir")
    ap.add_argument("tag")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.runs, newline="")))
    groups = defaultdict(list)
    for r in rows:
        groups[(r["block"], r["L"], r["rate"], r["p"])].append(r)

    out = f"{args.outdir}/rec-summary-{args.tag}.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "block", "L", "rate", "p", "seeds", "trials",
                    "recovered", "recovery", "wilson99_lo", "wilson99_hi",
                    "seed_min", "seed_median", "ge_0.999", "ge_0.999_lo"])
        print("tag=%s" % args.tag)
        print("block,L,rate,p,seeds,trials,recovered,recovery,lo99,hi99,"
              "seed_min,seed_med,pass_point,pass_lo")
        for key in sorted(groups, key=lambda k: (float(k[0]), float(k[1]),
                                                  float(k[2]), float(k[3]))):
            block, L, rate, p = key
            rs = groups[key]
            trials = 0
            recovered = 0
            vals = []
            for r in rs:
                n = int(float(r["trials"]))
                rec = float(r["recovery"])
                trials += n
                recovered += int(round(rec * n))
                vals.append(rec)
            agg = recovered / trials if trials else 0.0
            lo, hi = wilson(agg, trials)
            w.writerow([args.tag, block, L, rate, p, len(rs), trials,
                        recovered, f"{agg:.6f}", f"{lo:.6f}", f"{hi:.6f}",
                        f"{min(vals):.6f}", f"{statistics.median(vals):.6f}",
                        int(agg >= 0.999), int(lo >= 0.999)])
            print(f"{block},{L},{rate},{p},{len(rs)},{trials},{recovered},"
                  f"{agg:.6f},{lo:.6f},{hi:.6f},{min(vals):.6f},"
                  f"{statistics.median(vals):.6f},"
                  f"{int(agg >= 0.999)},{int(lo >= 0.999)}")
    print(f"file: {out}")


if __name__ == "__main__":
    main()
