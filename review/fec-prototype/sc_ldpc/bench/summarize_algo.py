#!/usr/bin/env python3
"""Summarize the sclc-algo three-way HND0 A/B (base / fast / opt)."""
import argparse
import csv
import statistics
from collections import defaultdict

TARGET = 4.215  # MB/s, app 33.72 Mbit/s payload


def num(v):
    if v is None:
        return None
    v = v.strip()
    if v == "" or v == "NA":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def stat(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None, None, None, 0
    return statistics.median(vals), min(vals), max(vals), len(vals)


def fmt(v, nd):
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

    byvar = defaultdict(list)
    byround = defaultdict(dict)
    for r in rows:
        byvar[r["variant"]].append(r)
        byround[int(r["round"])][r["variant"]] = r

    order = [v for v in ("algo_base", "algo_fast", "algo_opt") if v in byvar]

    metrics_path = f"{args.outdir}/algo-metrics-{args.tag}.csv"
    with open(metrics_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "variant", "n", "dec_median", "dec_min", "dec_max",
                    "enc_median", "enc_min", "enc_max", "recovery_min",
                    "iter_median", "ge_4.215", "ge_4.0", "ge_3.5"])
        for v in order:
            rs = byvar[v]
            dmed, dlo, dhi, n = stat([num(r["dec"]) for r in rs])
            emed, elo, ehi, _ = stat([num(r["enc"]) for r in rs])
            recs = [num(r["recovery"]) for r in rs if num(r["recovery"]) is not None]
            iters = [num(r["iter"]) for r in rs]
            decs = [num(r["dec"]) for r in rs if num(r["dec"]) is not None]
            w.writerow([args.tag, v, n, fmt(dmed, 3), fmt(dlo, 3), fmt(dhi, 3),
                        fmt(emed, 3), fmt(elo, 3), fmt(ehi, 3),
                        fmt(min(recs) if recs else None, 6),
                        fmt(stat(iters)[0], 2),
                        sum(1 for d in decs if d >= TARGET),
                        sum(1 for d in decs if d >= 4.0),
                        sum(1 for d in decs if d >= 3.5)])

    # paired ratios per round
    pairs = [("algo_fast", "algo_base"), ("algo_opt", "algo_base"),
             ("algo_opt", "algo_fast")]
    paired_path = f"{args.outdir}/algo-paired-{args.tag}.csv"
    with open(paired_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "cand", "base", "n", "dec_ratio_median",
                    "dec_ratio_min", "dec_ratio_max", "base_dec_median",
                    "cand_dec_median"])
        for cand, base in pairs:
            ratios, bd, cd = [], [], []
            for rnd in sorted(byround):
                a = byround[rnd].get(cand)
                b = byround[rnd].get(base)
                if not a or not b:
                    continue
                av, bv = num(a["dec"]), num(b["dec"])
                if av is None or bv is None or bv == 0:
                    continue
                ratios.append(av / bv)
                bd.append(bv)
                cd.append(av)
            if not ratios:
                continue
            rmed, rlo, rhi, n = stat(ratios)
            w.writerow([args.tag, cand, base, n, fmt(rmed, 4), fmt(rlo, 4),
                        fmt(rhi, 4), fmt(stat(bd)[0], 3), fmt(stat(cd)[0], 3)])

    load_path = f"{args.outdir}/algo-load-{args.tag}.csv"
    loads = [num(r["load1"]) for r in rows if num(r["load1"]) is not None]
    lmed, llo, lhi, ln = stat(loads)
    with open(load_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "n", "load1_median", "load1_min", "load1_max"])
        w.writerow([args.tag, ln, fmt(lmed, 2), fmt(llo, 2), fmt(lhi, 2)])

    print(f"tag={args.tag} target={TARGET} MB/s")
    print("variant,n,dec_median,dec_min,dec_max,enc_median,rec_min,iter_med,"
          "ge_4.215,ge_4.0,ge_3.5")
    for v in order:
        rs = byvar[v]
        dmed, dlo, dhi, n = stat([num(r["dec"]) for r in rs])
        emed = stat([num(r["enc"]) for r in rs])[0]
        recs = [num(r["recovery"]) for r in rs if num(r["recovery"]) is not None]
        iters = [num(r["iter"]) for r in rs]
        decs = [num(r["dec"]) for r in rs if num(r["dec"]) is not None]
        print(f"{v},{n},{fmt(dmed, 3)},{fmt(dlo, 3)},{fmt(dhi, 3)},"
              f"{fmt(emed, 3)},{fmt(min(recs) if recs else None, 6)},"
              f"{fmt(stat(iters)[0], 2)},"
              f"{sum(1 for d in decs if d >= TARGET)},"
              f"{sum(1 for d in decs if d >= 4.0)},"
              f"{sum(1 for d in decs if d >= 3.5)}")
    print(f"load1 median={fmt(lmed, 2)} min={fmt(llo, 2)} max={fmt(lhi, 2)}")
    print(f"files: {metrics_path} {paired_path} {load_path}")


if __name__ == "__main__":
    main()
