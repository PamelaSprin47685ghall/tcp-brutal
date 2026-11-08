#!/usr/bin/env python3
"""perf-stats.py - medians, dispersion, and the 5% verdict for perf-ab rows.

The unit of measurement is one steady window (one CSV row). The script
groups rows by (dataset, version), keeps rows with valid=1, and computes:

    median        the centre of the sample
    min/max       the observed range
    p25/p75       the interquartile range (linear interpolation)
    MAD           median absolute deviation, a robust spread
    rel_range     (max - min) / median * 100
    rel_mad       MAD / median * 100

Two versions differ only if BOTH hold:

    |delta| >= THRESHOLD percent (default 5)
    |delta| >  dispersion, where dispersion = max(rel_range of both sides)

A difference below 5% is reported as SAME: it is not an improvement. A
difference above 5% but inside the observed range is INCONCLUSIVE: n is
too small or the link moved. The script never turns noise into a win.

Usage:
    perf-stats.py RUNS.csv [RUNS2.csv ...] [--out DIR]
                  [--baseline 0.3.0] [--pairs A:B,C:D]
                  [--threshold 5] [--latency LATENCY.csv]

Outputs in DIR:
    summary.md, summary.csv, verdict.txt, summary.json
"""

import argparse
import csv
import json
import os
import sys

METRICS = [
    ("app_mbps", "app Mbit/s", True),
    ("wchar_mbps", "wchar Mbit/s", True),
    ("wire_mbps", "wire Mbit/s", True),
    ("wire_over_app", "wire/app", False),
    ("cpu_near_pct", "near CPU %", False),
    ("cpu_far_pct", "far CPU %", False),
    ("rtt_near_us", "carrier RTT us", False),
]


def as_float(value):
    if value is None:
        return None
    text = str(value).strip()
    if text in ("", "-", "?", "None", "nan"):
        return None
    try:
        return float(text)
    except ValueError:
        return None


def median(values):
    values = sorted(values)
    n = len(values)
    if n == 0:
        return None
    if n % 2:
        return values[n // 2]
    return (values[n // 2 - 1] + values[n // 2]) / 2.0


def percentile(values, p):
    values = sorted(values)
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    k = (len(values) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(values) - 1)
    frac = k - lo
    return values[lo] + (values[hi] - values[lo]) * frac


def spread(values):
    med = median(values)
    if med in (None, 0):
        return None, None, None
    mad = median([abs(v - med) for v in values])
    rel_range = (max(values) - min(values)) / med * 100.0
    rel_mad = mad / med * 100.0
    return rel_range, rel_mad, mad


def load_rows(paths):
    rows = []
    for path in paths:
        with open(path, newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                row["_file"] = os.path.dirname(os.path.abspath(path))
                rows.append(row)
    return rows


def group_rows(rows):
    groups = {}
    for row in rows:
        key = (row.get("dataset") or "ab", row.get("version") or "?")
        groups.setdefault(key, []).append(row)
    return groups


def stats_for(rows, metric, clean_only=False):
    values = []
    for row in rows:
        if str(row.get("valid", "")).strip() != "1":
            continue
        if clean_only and str(row.get("noisy", "")).strip() == "1":
            continue
        value = as_float(row.get(metric))
        if value is not None:
            values.append(value)
    if not values:
        return None
    rel_range, rel_mad, mad = spread(values)
    return {
        "n": len(values),
        "median": median(values),
        "min": min(values),
        "max": max(values),
        "p25": percentile(values, 25),
        "p75": percentile(values, 75),
        "mad": mad,
        "rel_range": rel_range,
        "rel_mad": rel_mad,
        "values": values,
    }


def classify(base, new, threshold):
    if not base or not new:
        return "INSUFFICIENT (no data)", None, None
    if base["n"] < 3 or new["n"] < 3:
        return "INSUFFICIENT (n<3)", None, None
    if base["median"] in (None, 0):
        return "INSUFFICIENT (base=0)", None, None
    delta = (new["median"] - base["median"]) / base["median"] * 100.0
    disp = max(base["rel_range"] or 0.0, new["rel_range"] or 0.0)
    if abs(delta) < threshold:
        return "SAME (<%.0f%%)" % threshold, delta, disp
    if abs(delta) > disp:
        return ("BETTER" if delta > 0 else "WORSE"), delta, disp
    return "INCONCLUSIVE (inside dispersion)", delta, disp


def fmt(value, digits=3):
    if value is None:
        return "-"
    if isinstance(value, int):
        return str(value)
    return ("%." + str(digits) + "f") % value


def parse_pairs(text, versions, baseline):
    pairs = []
    if text:
        for item in text.split(","):
            item = item.strip()
            if not item:
                continue
            left, _, right = item.partition(":")
            if left and right:
                pairs.append((left, right))
        return pairs
    if baseline in versions:
        for version in versions:
            if version != baseline:
                pairs.append((baseline, version))
    for index in range(len(versions) - 1):
        pair = (versions[index], versions[index + 1])
        if pair not in pairs:
            pairs.append(pair)
    return pairs


def latency_stats(path):
    if not path:
        return None
    if not os.path.exists(path):
        return None
    phases = {}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            phase = row.get("phase") or "?"
            slot = phases.setdefault(phase, {"connect": [], "ttfb": [], "total": [], "bad": 0})
            if str(row.get("http", "")).strip() != "200":
                slot["bad"] += 1
                continue
            times = [as_float(row.get(column)) for column in
                     ("connect_s", "ttfb_s", "total_s")]
            if any(value is None for value in times):
                continue
            for key, value in zip(("connect", "ttfb", "total"), times):
                slot[key].append(value * 1000.0)
    result = {}
    for phase, slot in phases.items():
        result[phase] = {}
        for key in ("connect", "ttfb", "total"):
            values = slot[key]
            result[phase][key] = {
                "n": len(values),
                "p50": percentile(values, 50),
                "p90": percentile(values, 90),
                "p99": percentile(values, 99),
                "max": max(values) if values else None,
            }
        result[phase]["bad_http"] = slot["bad"]
    return result


def build(rows, threshold, baseline, pairs_text, latency):
    groups = group_rows(rows)
    versions = []
    datasets = []
    for dataset, version in sorted(groups):
        if dataset not in datasets:
            datasets.append(dataset)
        if version not in versions:
            versions.append(version)
    summary_rows = []
    group_stats = {}
    for key, members in sorted(groups.items()):
        dataset, version = key
        invalid = sum(1 for row in members if str(row.get("valid", "")).strip() != "1")
        noisy = sum(1 for row in members if str(row.get("noisy", "")).strip() == "1")
        clean = [row for row in members if str(row.get("noisy", "")).strip() != "1"]
        per_metric = {}
        for metric, label, higher_is_better in METRICS:
            clean_stats = stats_for(members, metric, clean_only=True)
            all_stats = stats_for(members, metric)
            if all_stats is None:
                continue
            # Primary sample is clean rows: a carrier stall is a link event,
            # not a property of the version. The all-valid median stays in
            # the table so a reader can see what the exclusion did.
            stats = clean_stats
            per_metric[metric] = {
                "label": label,
                "higher_is_better": higher_is_better,
                "stats": stats,
                "stats_all": all_stats,
            }
            summary_rows.append({
                "dataset": dataset,
                "version": version,
                "metric": metric,
                "n": stats["n"] if stats else 0,
                "median": stats["median"] if stats else None,
                "min": stats["min"] if stats else None,
                "max": stats["max"] if stats else None,
                "p25": stats["p25"] if stats else None,
                "p75": stats["p75"] if stats else None,
                "mad": stats["mad"] if stats else None,
                "rel_range_pct": stats["rel_range"] if stats else None,
                "rel_mad_pct": stats["rel_mad"] if stats else None,
                "n_all": all_stats["n"],
                "median_all": all_stats["median"],
                "rel_range_all_pct": all_stats["rel_range"],
                "invalid": invalid,
                "noisy": noisy,
            })
        counters = {}
        window_sum = 0.0
        app_sum = 0.0
        clean_n = 0
        has_ack = False
        has_link = False
        has_retx = False
        for row in members:
            if str(row.get("valid", "")).strip() != "1":
                continue
            if str(row.get("noisy", "")).strip() == "1":
                continue
            clean_n += 1
            window_sum += as_float(row.get("window_s")) or 0.0
            app_sum += as_float(row.get("app_bytes")) or 0.0
            if as_float(row.get("ack_tx")) is not None:
                has_ack = True
            if as_float(row.get("retx_bytes")) is not None:
                has_retx = True
            if any(as_float(row.get(name)) is not None for name in
                   ("c3_lost", "c3_stall_declared", "c6_holes", "nack_tx")):
                has_link = True
            for name in ("ack_tx", "ack_rx", "nack_tx", "nack_rx",
                         "retx_bytes", "c3_retx_bytes", "c3_lost",
                         "c3_replaced", "c3_stall_declared", "c6_holes",
                         "c9_drop_sid", "c9_stream_refused"):
                counters[name] = counters.get(name, 0.0) + (as_float(row.get(name)) or 0.0)
        counters["windows"] = clean_n
        counters["has_ack"] = has_ack
        counters["has_link"] = has_link
        counters["has_retx"] = has_retx
        counters["ack_tx_per_s"] = counters.get("ack_tx", 0.0) / window_sum if window_sum else None
        counters["ack_rx_per_s"] = counters.get("ack_rx", 0.0) / window_sum if window_sum else None
        counters["retx_ppm"] = counters.get("retx_bytes", 0.0) / app_sum * 1e6 if app_sum else None
        group_stats[key] = {
            "n": len(members),
            "invalid": invalid,
            "noisy": noisy,
            "metrics": per_metric,
            "counters": counters,
        }

    verdicts = []
    for dataset in datasets:
        known = [v for v in versions if (dataset, v) in group_stats]
        if dataset == "direct":
            continue
        for left, right in parse_pairs(pairs_text, known, baseline):
            if (dataset, left) not in group_stats or (dataset, right) not in group_stats:
                continue
            base_metric = group_stats[(dataset, left)]["metrics"].get("app_mbps")
            new_metric = group_stats[(dataset, right)]["metrics"].get("app_mbps")
            base_stats = base_metric.get("stats") if base_metric else None
            new_stats = new_metric.get("stats") if new_metric else None
            state, delta, disp = classify(base_stats, new_stats, threshold)
            verdicts.append({
                "dataset": dataset,
                "a": left,
                "b": right,
                "metric": "app_mbps",
                "state": state,
                "delta_pct": delta,
                "dispersion_pct": disp,
                "median_a": base_stats["median"] if base_stats else None,
                "median_b": new_stats["median"] if new_stats else None,
            })

    drift = []
    sessions = sorted({row.get("_file") or "?" for row in rows})
    for session in sessions:
        for (dataset, version) in sorted({(row.get("dataset") or "ab", row.get("version") or "?")
                                           for row in rows if (row.get("_file") or "?") == session}):
            members = [row for row in rows
                       if (row.get("_file") or "?") == session
                       and (row.get("dataset") or "ab") == dataset
                       and (row.get("version") or "?") == version]
            blocks = {}
            for row in members:
                if str(row.get("valid", "")).strip() != "1":
                    continue
                if str(row.get("noisy", "")).strip() == "1":
                    continue
                value = as_float(row.get("app_mbps"))
                if value is None:
                    continue
                block = row.get("block") or "?"
                blocks.setdefault(block, []).append(value)
            if len(blocks) < 2:
                continue
            block_medians = {block: median(values) for block, values in sorted(blocks.items())}
            low = min(block_medians.values())
            if low in (None, 0):
                continue
            span = (max(block_medians.values()) - low) / low * 100.0
            drift.append({
                "session": os.path.basename(session) or session,
                "dataset": dataset,
                "version": version,
                "block_medians": block_medians,
                "span_pct": span,
                "state": "DRIFT" if span > 5.0 else "stable",
            })

    return {
        "threshold_pct": threshold,
        "datasets": datasets,
        "versions": versions,
        "groups": group_stats,
        "summary_rows": summary_rows,
        "verdicts": verdicts,
        "drift": drift,
        "latency": latency,
    }


def write_csv(path, rows):
    fields = [
        "dataset", "version", "metric", "n", "median", "min", "max", "p25",
        "p75", "mad", "rel_range_pct", "rel_mad_pct", "n_all", "median_all",
        "rel_range_all_pct", "invalid", "noisy",
    ]
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field) for field in fields})


def write_markdown(path, data):
    lines = []
    lines.append("# tbtun same-link performance summary")
    lines.append("")
    lines.append("- verdict threshold: %.0f%% (a smaller difference is SAME)"
                 % data["threshold_pct"])
    if data.get("valid_recomputed"):
        lines.append("- valid recomputed from window_s/app/wchar/stream_alive "
                     "with %d..%d s; %d row(s) changed" % (
                         data.get("secs", 60), data.get("secs", 60) + 20,
                         data.get("valid_changed", 0)))
    lines.append("- unit: one steady window (one valid CSV row); median across rows")
    lines.append("- dispersion: (max-min)/median over the same rows")
    lines.append("")
    lines.append("## Steady windows")
    lines.append("")
    lines.append("| dataset | version | n clean/valid | invalid | noisy | app median (clean) | all-valid median | range (clean) | rel range | wchar median | wire median | wire/app | CPU near | CPU far | RTT us |")
    lines.append("|---|---|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|")
    for (dataset, version), group in sorted(data["groups"].items()):
        metrics = group["metrics"]

        def med(name, digits=3):
            metric = metrics.get(name)
            if not metric or not metric["stats"]:
                return "-"
            return fmt(metric["stats"]["median"], digits)

        app = metrics.get("app_mbps")
        if app and app["stats"]:
            app_range = "%s..%s" % (fmt(app["stats"]["min"], 3), fmt(app["stats"]["max"], 3))
            rel = fmt(app["stats"]["rel_range"], 1) + "%"
            clean_n = app["stats"]["n"]
        else:
            app_range = "-"
            rel = "-"
            clean_n = 0
        all_median = fmt(app["stats_all"]["median"], 3) if app and app["stats_all"] else "-"
        all_n = app["stats_all"]["n"] if app and app["stats_all"] else 0
        lines.append("| %s | %s | %d/%d | %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            dataset, version, clean_n, all_n, group["invalid"], group["noisy"],
            med("app_mbps"), all_median, app_range, rel, med("wchar_mbps"), med("wire_mbps"),
            med("wire_over_app", 4), med("cpu_near_pct", 1), med("cpu_far_pct", 1),
            med("rtt_near_us", 0),
        ))
    lines.append("")
    lines.append("## Pairwise (app goodput)")
    lines.append("")
    lines.append("| dataset | A | B | median A | median B | delta | dispersion | verdict |")
    lines.append("|---|---|---|---:|---:|---:|---:|---|")
    for item in data["verdicts"]:
        delta = fmt(item["delta_pct"], 2)
        disp = fmt(item["dispersion_pct"], 2)
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            item["dataset"], item["a"], item["b"],
            fmt(item["median_a"], 3), fmt(item["median_b"], 3),
            delta + "%" if delta != "-" else "-",
            disp + "%" if disp != "-" else "-",
            item["state"],
        ))
    if not data["verdicts"]:
        lines.append("| - | - | - | - | - | - | - | no comparable pair |")
    lines.append("")
    lines.append("## Block drift")
    lines.append("")
    if data["drift"]:
        lines.append("| session | dataset | version | block medians | span | state |")
        lines.append("|---|---|---|---|---:|---|")
        for item in data["drift"]:
            blocks = ", ".join("%s=%s" % (k, fmt(v, 3)) for k, v in item["block_medians"].items())
            lines.append("| %s | %s | %s | %s | %s%% | %s |" % (
                item.get("session", "-"), item["dataset"], item["version"], blocks,
                fmt(item["span_pct"], 2), item["state"],
            ))
    else:
        lines.append("fewer than two blocks with clean windows; no drift estimate")
    lines.append("")
    lines.append("## ACK and link counters (clean windows, summed)")
    lines.append("")
    lines.append("| dataset | version | windows | ack_tx/s | ack_rx/s | nack_tx | retx ppm | lost | stalls | holes | drop_sid |")
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for (dataset, version), group in sorted(data["groups"].items()):
        c = group.get("counters") or {}
        if not c.get("windows"):
            lines.append("| %s | %s | 0 | - | - | - | - | - | - | - | - |" % (dataset, version))
            continue
        ack_tx = fmt(c.get("ack_tx_per_s"), 2) if c.get("has_ack") else "-"
        ack_rx = fmt(c.get("ack_rx_per_s"), 2) if c.get("has_ack") else "-"
        link = c.get("has_link")
        lines.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            dataset, version, c["windows"],
            ack_tx, ack_rx,
            fmt(c.get("nack_tx"), 0) if link else "-",
            fmt(c.get("retx_ppm"), 0) if c.get("has_retx") else "-",
            fmt(c.get("c3_lost"), 0) if link else "-",
            fmt(c.get("c3_stall_declared"), 0) if link else "-",
            fmt(c.get("c6_holes"), 0) if link else "-",
            fmt(c.get("c9_drop_sid"), 0) if link else "-",
        ))
    lines.append("")
    latency = data.get("latency")
    if latency:
        lines.append("## Direct latency (HEAD, ms)")
        lines.append("")
        lines.append("| phase | n | connect p50 | ttfb p50 | ttfb p90 | ttfb p99 | total p99 | non-200 |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|")
        for phase, values in sorted(latency.items()):
            lines.append("| %s | %d | %s | %s | %s | %s | %s | %d |" % (
                phase, values["connect"]["n"],
                fmt(values["connect"]["p50"], 2), fmt(values["ttfb"]["p50"], 2),
                fmt(values["ttfb"]["p90"], 2), fmt(values["ttfb"]["p99"], 2),
                fmt(values["total"]["p99"], 2), values["bad_http"],
            ))
        lines.append("")
    lines.append("## Reading")
    lines.append("")
    lines.append("- `SAME` means the medians differ by less than the threshold; it is not a win.")
    lines.append("- `INCONCLUSIVE` means the difference is inside the observed range; add runs or re-run when the link is quieter.")
    lines.append("- `noisy` counts rows with carrier loss, replacement, stall, a hole, a reset, or an app/wchar mismatch.")
    lines.append("- The direct dataset is a control, not a version: one TCP flow is not the 256-carrier shape.")
    lines.append("")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def recompute_valid(rows, secs):
    """Re-apply the 60 s window rule to an older runs.csv.

    The scripts used to require 58..65 s, but the real snapshots take
    66..73 s.  This helper keeps the raw file intact and only rewrites the
    in-memory valid flag: window in [secs, secs+20], app>0, wchar>0,
    stream_alive=1.
    """
    changed = 0
    for row in rows:
        old = str(row.get("valid", "")).strip()
        window = as_float(row.get("window_s"))
        app = as_float(row.get("app_bytes"))
        wchar = as_float(row.get("wchar_bytes"))
        alive = str(row.get("stream_alive", "")).strip() == "1"
        new = "1" if (
            window is not None and secs <= window <= secs + 20
            and app is not None and app > 0
            and wchar is not None and wchar > 0
            and alive
        ) else "0"
        if new != old:
            changed += 1
        row["valid"] = new
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", help="runs.csv files")
    parser.add_argument("--out", default=None)
    parser.add_argument("--baseline", default="0.3.0")
    parser.add_argument("--pairs", default=None, help="A:B,C:D")
    parser.add_argument("--threshold", type=float, default=5.0)
    parser.add_argument("--latency", default=None)
    parser.add_argument("--recompute-valid", action="store_true",
                        help="re-apply the window rule to an old runs.csv")
    parser.add_argument("--secs", type=int, default=60,
                        help="nominal window for --recompute-valid (default 60)")
    args = parser.parse_args()

    rows = load_rows(args.runs)
    if not rows:
        sys.exit("no rows in %s" % ", ".join(args.runs))
    if args.latency and not os.path.exists(args.latency):
        sys.exit("latency file not found: %s" % args.latency)
    changed = 0
    if args.recompute_valid:
        changed = recompute_valid(rows, args.secs)
    latency = latency_stats(args.latency) if args.latency else None
    data = build(rows, args.threshold, args.baseline, args.pairs, latency)
    data["valid_recomputed"] = bool(args.recompute_valid)
    data["valid_changed"] = changed
    data["secs"] = args.secs

    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.runs[0])), "analysis")
    os.makedirs(out, exist_ok=True)
    write_csv(os.path.join(out, "summary.csv"), data["summary_rows"])
    write_markdown(os.path.join(out, "summary.md"), data)
    with open(os.path.join(out, "verdict.txt"), "w", encoding="utf-8") as handle:
        for item in data["verdicts"]:
            handle.write("PAIR %s %s %s delta_pct=%s dispersion_pct=%s verdict=%s\n" % (
                item["dataset"], item["a"], item["b"],
                fmt(item["delta_pct"], 2), fmt(item["dispersion_pct"], 2), item["state"],
            ))
        for item in data["drift"]:
            handle.write("DRIFT %s %s %s span_pct=%s state=%s\n" % (
                item.get("session", "-"), item["dataset"], item["version"],
                fmt(item["span_pct"], 2), item["state"],
            ))
        for (dataset, version), group in sorted(data["groups"].items()):
            counters = group.get("counters") or {}
            if not counters.get("windows"):
                continue
            ack_tx = fmt(counters.get("ack_tx_per_s"), 2) if counters.get("has_ack") else "-"
            ack_rx = fmt(counters.get("ack_rx_per_s"), 2) if counters.get("has_ack") else "-"
            retx = fmt(counters.get("retx_ppm"), 0) if counters.get("has_retx") else "-"
            link = counters.get("has_link")
            handle.write("COUNTERS %s %s windows=%s ack_tx_s=%s ack_rx_s=%s retx_ppm=%s lost=%s stalls=%s holes=%s\n" % (
                dataset, version, counters["windows"],
                ack_tx, ack_rx, retx,
                fmt(counters.get("c3_lost"), 0) if link else "-",
                fmt(counters.get("c3_stall_declared"), 0) if link else "-",
                fmt(counters.get("c6_holes"), 0) if link else "-",
            ))
    json_data = dict(data)
    json_data["groups"] = {
        "%s|%s" % key: value for key, value in data["groups"].items()
    }
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as handle:
        json.dump(json_data, handle, indent=2, sort_keys=True, default=str)

    print(os.path.join(out, "summary.md"))
    for item in data["verdicts"]:
        delta = fmt(item["delta_pct"], 2)
        disp = fmt(item["dispersion_pct"], 2)
        print("PAIR %s %s->%s delta=%s disp=%s %s" % (
            item["dataset"], item["a"], item["b"],
            delta + "%" if delta != "-" else "-",
            disp + "%" if disp != "-" else "-",
            item["state"],
        ))


if __name__ == "__main__":
    main()
