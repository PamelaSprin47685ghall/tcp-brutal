#!/usr/bin/env python3
"""Summarize the sclc-encode HND0 A/B (scalar_ref vs enc64/e2e/sse2)."""
import argparse
import csv
import statistics
from collections import defaultdict

APP = 4.215  # MB/s payload


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def stat(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None, None, None, 0
    return statistics.median(vals), min(vals), max(vals), len(vals)


def fmt(v, nd=3):
    return "" if v is None else f"{v:.{nd}f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("outdir")
    ap.add_argument("tag")
    args = ap.parse_args()

    with open(args.runs, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise SystemExit("no rows")

    groups = defaultdict(list)
    for r in rows:
        groups[(r["threads"], r["rate"])].append(r)

    metrics = ("scalar_ref", "enc64", "e2e", "sse2", "enc64_mt")
    mpath = f"{args.outdir}/enc-metrics-{args.tag}.csv"
    with open(mpath, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "threads", "rate", "n", "metric", "median", "min", "max"])
        for key in sorted(groups, key=lambda k: (int(k[0]), float(k[1]))):
            threads, rate = key
            rs = groups[key]
            for m in metrics:
                med, lo, hi, n = stat([num(r[m]) for r in rs])
                if n:
                    w.writerow([args.tag, threads, rate, n, m, fmt(med), fmt(lo), fmt(hi)])

    rpath = f"{args.outdir}/enc-ratios-{args.tag}.csv"
    with open(rpath, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "threads", "rate", "n", "target_wire", "scalar_med",
                    "e2e_med", "e2e_ratio", "e2e_ratio_min", "e2e_ratio_max",
                    "enc64_ratio", "sse2_ratio", "e2e_ge_target"])
        for key in sorted(groups, key=lambda k: (int(k[0]), float(k[1]))):
            threads, rate = key
            rs = groups[key]
            er, cr, sr = [], [], []
            for r in rs:
                sc = num(r["scalar_ref"])
                if not sc:
                    continue
                er.append(num(r["e2e"]) / sc)
                cr.append(num(r["enc64"]) / sc)
                sr.append(num(r["sse2"]) / sc)
            if not er:
                continue
            target = APP / float(rate)
            emed = stat([num(r["e2e"]) for r in rs])[0]
            w.writerow([args.tag, threads, rate, len(er), fmt(target),
                        fmt(stat([num(r["scalar_ref"]) for r in rs])[0]),
                        fmt(emed), fmt(stat(er)[0], 2), fmt(stat(er)[1], 2),
                        fmt(stat(er)[2], 2), fmt(stat(cr)[0], 2),
                        fmt(stat(sr)[0], 2),
                        sum(1 for r in rs if num(r["e2e"]) is not None
                            and num(r["e2e"]) >= target)])

    loads = [num(r["load1"]) for r in rows]
    lmed, llo, lhi, ln = stat(loads)
    lpath = f"{args.outdir}/enc-load-{args.tag}.csv"
    with open(lpath, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "n", "load1_median", "load1_min", "load1_max"])
        w.writerow([args.tag, ln, fmt(lmed, 2), fmt(llo, 2), fmt(lhi, 2)])

    print(f"tag={args.tag} app_payload={APP} MB/s")
    print("threads,rate,target,scalar_med,enc64_med,e2e_med,sse2_med,"
          "e2e_ratio,enc64_ratio,e2e_ge_target")
    for key in sorted(groups, key=lambda k: (int(k[0]), float(k[1]))):
        threads, rate = key
        rs = groups[key]
        target = APP / float(rate)
        smed = stat([num(r["scalar_ref"]) for r in rs])[0]
        emed = stat([num(r["enc64"]) for r in rs])[0]
        e2emed = stat([num(r["e2e"]) for r in rs])[0]
        smed2 = stat([num(r["sse2"]) for r in rs])[0]
        ratios = [num(r["e2e"]) / num(r["scalar_ref"]) for r in rs
                  if num(r["e2e"]) is not None and num(r["scalar_ref"])]
        print(f"{threads},{rate},{fmt(target)},{fmt(smed)},{fmt(emed)},"
              f"{fmt(e2emed)},{fmt(smed2)},{fmt(stat(ratios)[0], 2)},"
              f"{fmt(stat([num(r['enc64']) / num(r['scalar_ref']) for r in rs
                           if num(r['enc64']) is not None and num(r['scalar_ref'])])[0], 2)},"
              f"{sum(1 for r in rs if num(r['e2e']) is not None and num(r['e2e']) >= target)}")
    print(f"load1 median={fmt(lmed, 2)} min={fmt(llo, 2)} max={fmt(lhi, 2)}")
    print(f"files: {mpath} {rpath} {lpath}")


if __name__ == "__main__":
    main()
