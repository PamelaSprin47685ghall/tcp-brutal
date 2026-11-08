#!/usr/bin/env python3
"""Summarize the SC-LDPC HND0 bit-sliced A/B run.

Reads the unified runs CSV produced by remote-run-bs.sh and writes:

  metrics-bs-<tag>.csv    median/min/max per phase/variant/batch/metric
  ab-bs-<tag>.csv         paired base vs cand, phases t1b512 and t2b512
  batch-bs-<tag>.csv      cand batch 512 vs 1024, phase t1b1024
  correctness-bs-<tag>.csv  mismatch totals and recovery floor per cand group

Also prints a compact human-readable table.
"""
import argparse
import csv
import statistics
from collections import defaultdict


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


def stat(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None, None, None, 0
    return (statistics.median(vals), min(vals), max(vals), len(vals))


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
        raise SystemExit("no rows in " + args.runs)

    groups = defaultdict(list)
    for r in rows:
        key = (r["phase"], r["variant"], r["threads"], r["batch"],
               r["block"], r["L"], r["rate"], r["p"])
        groups[key].append(r)

    metrics_path = f"{args.outdir}/metrics-bs-{args.tag}.csv"
    with open(metrics_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "phase", "variant", "threads", "batch", "block", "L",
                    "rate", "p", "n", "metric", "median", "min", "max"])
        for key in sorted(groups, key=lambda k: (k[0], float(k[4]), float(k[5]),
                                                  float(k[6]), float(k[7]),
                                                  int(k[2]), int(k[3]), k[1])):
            phase, variant, threads, batch, block, L, rate, p = key
            rs = groups[key]
            for metric, nd in (("recovery", 6), ("iter", 2), ("enc", 3),
                               ("dec", 3), ("dec_scalar", 3), ("speedup", 2)):
                med, lo, hi, n = stat([num(r[metric]) for r in rs])
                if n == 0:
                    continue
                w.writerow([args.tag, phase, variant, threads, batch, block, L,
                            rate, p, n, metric, fmt(med, nd), fmt(lo, nd),
                            fmt(hi, nd)])

    # correctness totals per candidate group
    corr_path = f"{args.outdir}/correctness-bs-{args.tag}.csv"
    with open(corr_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "phase", "threads", "batch", "block", "L", "rate",
                    "p", "runs", "cw", "recovery_min", "unres_mismatch_sum",
                    "bit_mismatch_sum"])
        for key in sorted(groups, key=lambda k: (k[0], float(k[4]), float(k[5]),
                                                  float(k[6]), float(k[7]),
                                                  int(k[2]), int(k[3]))):
            phase, variant, threads, batch, block, L, rate, p = key
            if variant != "cand":
                continue
            rs = groups[key]
            unres = sum(int(num(r["unres_mismatch"]) or 0) for r in rs)
            bit = sum(int(num(r["bit_mismatch"]) or 0) for r in rs)
            cw = sum(int(num(r["cw"]) or 0) for r in rs)
            rec = [num(r["recovery"]) for r in rs]
            rec = [v for v in rec if v is not None]
            w.writerow([args.tag, phase, threads, batch, block, L, rate, p,
                        len(rs), cw, fmt(min(rec) if rec else None, 6), unres, bit])

    # paired A/B, base vs cand
    ab_path = f"{args.outdir}/ab-bs-{args.tag}.csv"
    with open(ab_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "phase", "threads", "block", "L", "rate", "p", "n",
                    "base_dec", "cand_dec", "dec_ratio", "dec_ratio_min",
                    "dec_ratio_max", "base_rec", "cand_rec", "rec_max_abs_delta",
                    "base_iter", "cand_iter", "iter_max_abs_delta",
                    "cand_speedup", "cand_speedup_min", "cand_speedup_max",
                    "unres_mismatch_sum", "bit_mismatch_sum"])
        for phase in ("t1b512", "t2b512"):
            pts = defaultdict(lambda: {"base": {}, "cand": {}})
            for r in rows:
                if r["phase"] != phase:
                    continue
                pt = (r["threads"], r["block"], r["L"], r["rate"], r["p"])
                if r["variant"] == "base":
                    pts[pt]["base"][r["round"]] = r
                elif r["variant"] == "cand" and r["batch"] == "512":
                    pts[pt]["cand"][r["round"]] = r
            for pt in sorted(pts, key=lambda k: (int(k[0]), float(k[1]),
                                                  float(k[2]), float(k[3]),
                                                  float(k[4]))):
                threads, block, L, rate, p = pt
                base = pts[pt]["base"]
                cand = pts[pt]["cand"]
                rounds = sorted(set(base) & set(cand), key=int)
                if not rounds:
                    continue
                ratios, bdecs, cdecs, brecs, crecs, biters, citers = [], [], [], [], [], [], []
                speedups = []
                recdelta = iterdelta = 0.0
                unres_sum = bit_sum = 0
                for rnd in rounds:
                    b = base[rnd]
                    c = cand[rnd]
                    bd, cd = num(b["dec"]), num(c["dec"])
                    if bd and cd is not None:
                        ratios.append(cd / bd)
                        bdecs.append(bd)
                        cdecs.append(cd)
                    br, cr = num(b["recovery"]), num(c["recovery"])
                    if br is not None and cr is not None:
                        brecs.append(br)
                        crecs.append(cr)
                        recdelta = max(recdelta, abs(cr - br))
                    bi, ci = num(b["iter"]), num(c["iter"])
                    if bi is not None and ci is not None:
                        biters.append(bi)
                        citers.append(ci)
                        iterdelta = max(iterdelta, abs(ci - bi))
                    sp = num(c["speedup"])
                    if sp is not None:
                        speedups.append(sp)
                    unres_sum += int(num(c["unres_mismatch"]) or 0)
                    bit_sum += int(num(c["bit_mismatch"]) or 0)
                if not ratios:
                    continue
                rmed, rlo, rhi, _ = stat(ratios)
                smed, slo, shi, _ = stat(speedups)
                w.writerow([args.tag, phase, threads, block, L, rate, p,
                            len(rounds),
                            fmt(stat(bdecs)[0], 3), fmt(stat(cdecs)[0], 3),
                            fmt(rmed, 4), fmt(rlo, 4), fmt(rhi, 4),
                            fmt(stat(brecs)[0], 6), fmt(stat(crecs)[0], 6),
                            fmt(recdelta, 6),
                            fmt(stat(biters)[0], 2), fmt(stat(citers)[0], 2),
                            fmt(iterdelta, 2),
                            fmt(smed, 2), fmt(slo, 2), fmt(shi, 2),
                            unres_sum, bit_sum])

    # batch 512 vs 1024, candidate only
    batch_path = f"{args.outdir}/batch-bs-{args.tag}.csv"
    with open(batch_path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["tag", "threads", "block", "L", "rate", "p", "n",
                    "b512_dec", "b1024_dec", "ratio_1024_512", "ratio_min",
                    "ratio_max", "b512_speedup", "b1024_speedup"])
        pts = defaultdict(lambda: {"512": {}, "1024": {}})
        for r in rows:
            if r["phase"] != "t1b1024" or r["variant"] != "cand":
                continue
            pt = (r["threads"], r["block"], r["L"], r["rate"], r["p"])
            if r["batch"] in ("512", "1024"):
                pts[pt][r["batch"]][r["round"]] = r
        for pt in sorted(pts, key=lambda k: (int(k[0]), float(k[1]), float(k[2]),
                                              float(k[3]), float(k[4]))):
            threads, block, L, rate, p = pt
            a, b = pts[pt]["512"], pts[pt]["1024"]
            rounds = sorted(set(a) & set(b), key=int)
            if not rounds:
                continue
            ratios, adecs, bdecs, asp, bsp = [], [], [], [], []
            for rnd in rounds:
                ad, bd = num(a[rnd]["dec"]), num(b[rnd]["dec"])
                if ad and bd is not None:
                    ratios.append(bd / ad)
                    adecs.append(ad)
                    bdecs.append(bd)
                sa, sb = num(a[rnd]["speedup"]), num(b[rnd]["speedup"])
                if sa is not None:
                    asp.append(sa)
                if sb is not None:
                    bsp.append(sb)
            if not ratios:
                continue
            rmed, rlo, rhi, _ = stat(ratios)
            w.writerow([args.tag, threads, block, L, rate, p, len(rounds),
                        fmt(stat(adecs)[0], 3), fmt(stat(bdecs)[0], 3),
                        fmt(rmed, 4), fmt(rlo, 4), fmt(rhi, 4),
                        fmt(stat(asp)[0], 2), fmt(stat(bsp)[0], 2)])

    # compact stdout table
    print(f"tag={args.tag}")
    print("\n== t1b512 base vs cand@512 ==")
    with open(ab_path, newline="") as f:
        for r in csv.DictReader(f):
            if r["phase"] != "t1b512":
                continue
            print(f"b{r['block']} L{r['L']} r{r['rate']} p{r['p']}: "
                  f"base {r['base_dec']} -> cand {r['cand_dec']} MB/s "
                  f"x{r['dec_ratio']} [{r['dec_ratio_min']},{r['dec_ratio_max']}], "
                  f"speedup {r['cand_speedup']}x, "
                  f"rec {r['base_rec']}/{r['cand_rec']}, "
                  f"mismatch {r['unres_mismatch_sum']}/{r['bit_mismatch_sum']}")
    print("\n== t2b512 base vs cand@512 ==")
    with open(ab_path, newline="") as f:
        for r in csv.DictReader(f):
            if r["phase"] != "t2b512":
                continue
            print(f"b{r['block']} L{r['L']} r{r['rate']} p{r['p']}: "
                  f"base {r['base_dec']} -> cand {r['cand_dec']} MB/s "
                  f"x{r['dec_ratio']}, speedup {r['cand_speedup']}x")
    print("\n== cand batch 512 vs 1024 ==")
    with open(batch_path, newline="") as f:
        for r in csv.DictReader(f):
            print(f"b{r['block']} L{r['L']} r{r['rate']} p{r['p']}: "
                  f"512 {r['b512_dec']} -> 1024 {r['b1024_dec']} MB/s "
                  f"x{r['ratio_1024_512']} [{r['ratio_min']},{r['ratio_max']}]")
    print(f"\nfiles: {metrics_path} {ab_path} {batch_path} {corr_path}")


if __name__ == "__main__":
    main()
