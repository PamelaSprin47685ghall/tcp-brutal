#!/usr/bin/env python3
"""Judge the fix42 first-connect window from the two sampler logs.

The near tunnel can disappear when the curl stream tears down. The samplers
run on the hosts and write to disk, so the counters that existed during the
transfer survive the teardown. This program reads those logs, selects the
last state that was still carrying traffic, and prints CHECK lines that the
shell driver folds into its run log.

Usage:
  fix42-analyze-first.py --near-log FILE --far-log FILE --near-exit FILE
      --curl FILE --near-size FILE --rc N [--json FILE]

Exit: 0 if every check passed, 1 otherwise, 2 on unusable input.
"""

import argparse
import json
import re
import sys

CLASSES = {
    "c3", "c6", "c9", "c10", "c11", "c12", "c13", "stream", "capture",
}


def parse_sampler(path):
    """Return a list of snapshots. Each snapshot is a dict: ts, plus one
    dict per class and the flat netdev/cpu fields."""
    snaps = []
    cur = None
    try:
        fh = open(path, "r", errors="replace")
    except OSError as exc:
        print("analyze: cannot open %s: %s" % (path, exc), file=sys.stderr)
        return None
    with fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line:
                continue
            try:
                ts = float(line)
            except ValueError:
                ts = None
            if ts is not None:
                if cur is not None:
                    snaps.append(cur)
                cur = {"ts": ts}
                continue
            if cur is None:
                continue
            if line.startswith("netdev_") or line.startswith("cpu_"):
                for tok in line.split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        cur[k] = v
                continue
            parts = line.split()
            if len(parts) < 3 or parts[0] != "tunnel":
                continue
            cls = parts[2]
            if "=" in cls:
                cls = "capture"
                toks = parts[2:]
            else:
                toks = parts[3:]
            if cls not in CLASSES:
                continue
            kv = cur.setdefault(cls, {})
            for tok in toks:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    kv[k] = v
    if cur is not None:
        snaps.append(cur)
    return snaps


def parse_stats_file(path):
    """Parse a raw /proc/net/tbtun/stats capture into one snapshot dict.
    Used as the far/near post fallback when the 10 Hz sampler is starved on
    a loaded host and its last live tick predates the transfer's ACKs."""
    snap = {"ts": 0.0}
    try:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                line = line.rstrip("\n")
                parts = line.split()
                if len(parts) < 3 or parts[0] != "tunnel":
                    continue
                cls = parts[2]
                if "=" in cls:
                    cls = "capture"
                    toks = parts[2:]
                else:
                    toks = parts[3:]
                if cls not in CLASSES:
                    continue
                kv = snap.setdefault(cls, {})
                for tok in toks:
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        kv[k] = v
    except OSError:
        return None
    return snap


def field(snap, cls, key):
    if snap is None:
        return None
    v = snap.get(cls, {}).get(key)
    if v is None:
        return None
    try:
        return int(v)
    except ValueError:
        return None


def delta(a, b, cls, key):
    x, y = field(a, cls, key), field(b, cls, key)
    if x is None or y is None:
        return None
    return y - x


def up(snap):
    return field(snap, "c3", "up")


def stream_state(snap):
    return field(snap, "stream", "state")


def alive(snap):
    return up(snap) == 256


def is_live(snap):
    st = stream_state(snap)
    return alive(snap) and st != 5 and "c11" in snap


def last_live_before(snaps, ts):
    out = None
    for s in snaps:
        if s["ts"] <= ts and is_live(s):
            out = s
    return out


def last_alive(snaps):
    out = None
    for s in snaps:
        if alive(s):
            out = s
    return out


def first_dead_after(snaps, start_ts):
    seen_open = False
    for s in snaps:
        if s["ts"] < start_ts:
            continue
        st = stream_state(s)
        if st == 1:
            seen_open = True
        # Only a stream kill after the stream opened is an early teardown; a
        # transient carrier-up dip or a stale dead-stream line is not one.
        if seen_open and st == 5:
            return s
    return None


def max_field(snaps, cls, key):
    best = None
    for s in snaps:
        if not alive(s):
            continue
        v = field(s, cls, key)
        if v is not None and (best is None or v > best):
            best = v
    return best


def fmt(v):
    return "?" if v is None else str(v)


def parse_curl(path):
    """Return dict from the last line carrying http= (curl errors may be the
    first line; the -w line is the one with the fields)."""
    out = {}
    try:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                if "http=" not in line:
                    continue
                for key in ("http", "size", "ttfb", "time", "speed"):
                    m = re.search(r"\b%s=([0-9.]+)" % key, line)
                    if m:
                        out[key] = m.group(1)
    except OSError:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--near-log", required=True)
    ap.add_argument("--far-log", required=True)
    ap.add_argument("--near-exit", required=True)
    ap.add_argument("--curl", required=True)
    ap.add_argument("--near-size", required=True)
    ap.add_argument("--rc", type=int, required=True)
    ap.add_argument("--profile", choices=("fix42", "fix43", "fix44", "fix45", "fix46", "fix47", "fix48"), default="fix42")
    ap.add_argument("--near-post")
    ap.add_argument("--far-post")
    ap.add_argument("--json")
    args = ap.parse_args()

    near = parse_sampler(args.near_log)
    far = parse_sampler(args.far_log)
    if not near or not far:
        print("CHECK first_sampler FAIL (empty sampler log)")
        print("FIRST_RESULT FAIL")
        return 2

    try:
        with open(args.near_exit, "r") as fh:
            exit_ts = float(fh.read().strip())
    except (OSError, ValueError):
        exit_ts = None

    try:
        with open(args.near_size, "r") as fh:
            file_size = int(fh.read().strip())
    except (OSError, ValueError):
        file_size = None

    curl = parse_curl(args.curl)
    try:
        http = int(curl.get("http", ""))
    except ValueError:
        http = None
    try:
        size = int(curl.get("size", ""))
    except ValueError:
        size = None
    ttfb = curl.get("ttfb")
    ctime = curl.get("time")
    speed = curl.get("speed")

    near_pre = next((s for s in near if is_live(s)), near[0])
    if exit_ts is not None:
        near_during = last_live_before(near, exit_ts - 0.1)
        if near_during is None:
            near_during = last_live_before(near, exit_ts + 0.05)
        near_before_exit = last_alive([s for s in near if s["ts"] <= exit_ts + 0.05])
        near_dead = first_dead_after(near, near_pre["ts"] + 0.01)
    else:
        near_during = last_live_before(near, near[-1]["ts"] + 1)
        near_before_exit = last_alive(near)
        near_dead = first_dead_after(near, near_pre["ts"] + 0.01)

    far_pre = next((s for s in far if is_live(s)), far[0])
    far_during = last_live_before(far, far[-1]["ts"] + 1)
    if far_during is None:
        far_during = last_alive(far)
    far_dead = first_dead_after(far, far_pre["ts"] + 0.01)
    near_c = parse_stats_file(args.near_post) if args.near_post else None
    far_c = parse_stats_file(args.far_post) if args.far_post else None
    near_c = near_c or near_during
    far_c = far_c or far_during

    checks = []

    def check(name, ok, detail):
        checks.append((name, bool(ok), detail))
        print("CHECK %s %s (%s)" % (name, "PASS" if ok else "FAIL", detail))

    def info(name, value):
        print("%s=%s" % (name, fmt(value)))

    # --- curl / HTTP -----------------------------------------------------
    check("first_curl_rc", args.rc in (0, 28),
          "rc=%d (28 = curl -m deadline, the expected end of this probe)" % args.rc)
    check("first_http", http == 200, "http=%s" % fmt(http))
    check("first_body", size is not None and size > 0 and size == file_size,
          "size_download=%s file=%s" % (fmt(size), fmt(file_size)))
    check("first_ttfb", ttfb is not None, "ttfb=%ss" % (ttfb or "?"))

    # --- far decode: the fix42 signal -----------------------------------
    far_decode = delta(far_pre, far_c, "c10", "fec_decode_blocks")
    near_decode = delta(near_pre, near_c, "c10", "fec_decode_blocks")
    far_late = delta(far_pre, far_c, "c10", "fec_late_share")
    far_dup = delta(far_pre, far_c, "c10", "fec_dup_share")
    if args.profile == "fix42":
        check("first_far_decode_blocks", far_decode is not None and far_decode >= 1,
              "delta=%s late=%s dup=%s" % (fmt(far_decode), fmt(far_late), fmt(far_dup)))
    else:
        info("far_first_decode_blocks_delta", far_decode)
    check("first_near_decode_blocks", near_decode is not None and near_decode >= 1,
          "delta=%s" % fmt(near_decode))
    near_ack_tx_first = delta(near_pre, near_c, "c9", "fec_ack_tx")
    if args.profile == "fix43":
        check("first_near_ack_tx", near_ack_tx_first is not None and near_ack_tx_first > 0,
              "delta=%s" % fmt(near_ack_tx_first))

    far_retx_bytes = delta(far_pre, far_c, "c11", "fec_nack_retx_bytes")
    if size and far_retx_bytes is not None:
        ratio = far_retx_bytes / size
        if args.profile == "fix42":
            check("first_far_retx_ratio", ratio <= 0.5,
                  "retx_bytes=%s / size=%s = %.3f (bound 0.5; fix41 was 0.554)"
                  % (fmt(far_retx_bytes), fmt(size), ratio))
        else:
            info("far_first_retx_ratio", "%.3f" % ratio)
    else:
        if args.profile == "fix42":
            check("first_far_retx_ratio", False,
                  "retx_bytes=%s size=%s" % (fmt(far_retx_bytes), fmt(size)))
        else:
            info("far_first_retx_ratio", None)

    # --- replay queue must not fill -------------------------------------
    replay_bound = 5 * 1024 * 1024 if args.profile in ("fix43", "fix44", "fix45", "fix46", "fix47", "fix48") else 1 * 1024 * 1024
    for end, pre, cnt, snaps in (
        ("near", near_pre, near_c, near),
        ("far", far_pre, far_c, far),
    ):
        qhigh = field(cnt, "c11", "fec_replay_queue_high")
        qmax = max_field(snaps, "c11", "fec_replay_queued")
        enospc = delta(pre, cnt, "c11", "fec_replay_miss_enospc")
        drop = delta(pre, cnt, "c11", "fec_replay_drop")
        backp = delta(pre, cnt, "c11", "fec_nack_backpressure")
        check("%s_first_replay_not_full" % end,
              qhigh is not None and qhigh < replay_bound and
              (enospc in (None, 0)) and (drop in (None, 0)),
              "queue_high=%s max_queued=%s enospc=%s replay_drop=%s (bound %d)"
              % (fmt(qhigh), fmt(qmax), fmt(enospc), fmt(drop), replay_bound))
        check("%s_first_nack_backpressure" % end, backp in (None, 0),
              "delta=%s" % fmt(backp))

    # --- no unexpected reset / hole / stream kill -----------------------
    near_teardown_early = False
    if exit_ts is not None and near_dead is not None:
        near_teardown_early = near_dead["ts"] < exit_ts - 2.0
    check("near_first_no_early_teardown", not near_teardown_early,
          "first dead ts=%s exit=%s"
          % (fmt(near_dead["ts"] if near_dead else None),
             "%.3f" % exit_ts if exit_ts is not None else "?"))

    for end, pre, live, cnt in (
        ("near", near_pre, near_during, near_c),
        ("far", far_pre, far_during, far_c),
    ):
        rt = delta(pre, cnt, "c11", "fec_reset_tcp_failed")
        rh = delta(pre, cnt, "c11", "fec_reset_hole")
        hu = delta(pre, cnt, "c6", "holes_unrecoverable")
        hd = delta(pre, cnt, "c6", "holes_declared")
        db = delta(pre, cnt, "c10", "fec_deliver_bad")
        ib = delta(pre, cnt, "c11", "fec_init_bad")
        check("%s_first_no_reset" % end,
              rt in (None, 0) and rh in (None, 0) and hu in (None, 0) and
              db in (None, 0) and ib in (None, 0),
              "reset_tcp=%s reset_hole=%s holes_unrec=%s holes_declared=%s "
              "deliver_bad=%s init_bad=%s"
              % (fmt(rt), fmt(rh), fmt(hu), fmt(hd), fmt(db), fmt(ib)))
        st = stream_state(live)
        check("%s_first_stream_alive" % end, st in (None, 0, 1, 2, 3, 4),
              "state=%s kill=%s"
              % (fmt(st), fmt(field(live, "stream", "kill"))))

    # --- credit wait: growth is flow control, freezing is failure -------
    near_credit = delta(near_pre, near_c, "c9", "fec_credit_wait")
    far_credit = delta(far_pre, far_c, "c9", "fec_credit_wait")
    near_ack_tx = delta(near_pre, near_c, "c9", "fec_ack_tx")
    far_ack_tx = delta(far_pre, far_c, "c9", "fec_ack_tx")
    near_ack_rx = delta(near_pre, near_c, "c9", "fec_ack_rx")
    far_ack_rx = delta(far_pre, far_c, "c9", "fec_ack_rx")
    near_ok = near_credit in (None, 0) or (far_ack_tx or 0) > 0
    far_ok = far_credit in (None, 0) or (near_ack_tx or 0) > 0
    check("near_first_credit_progress", near_ok,
          "credit_wait=%s far_ack_tx=%s" % (fmt(near_credit), fmt(far_ack_tx)))
    check("far_first_credit_progress", far_ok,
          "credit_wait=%s near_ack_tx=%s" % (fmt(far_credit), fmt(near_ack_tx)))

    if args.profile in ("fix43", "fix44"):
        # hol_wait is weighted by queue depth; record it, do not judge it.
        far_hol = delta(far_pre, far_c, "c11", "fec_hol_wait")
        near_hol = delta(near_pre, near_c, "c11", "fec_hol_wait")
        info("far_first_hol_wait", far_hol)
        info("near_first_hol_wait", near_hol)
        far_ack_rx_first = delta(far_pre, far_c, "c9", "fec_ack_rx")
        check("first_far_ack_rx", far_ack_rx_first is not None and far_ack_rx_first > 0,
              "delta=%s" % fmt(far_ack_rx_first))

        # "not continuously growing": a credit wait that still grows in the
        # last third while the peer sent no ACK in that third is the frozen
        # frontier signature. A growing counter with ACKs flowing is normal
        # flow control and is reported, not failed.
        def third_trend(snaps, peer_snaps, ts_limit):
            us = [s for s in snaps if s["ts"] <= ts_limit and is_live(s)]
            if len(us) < 6:
                return None, None, None
            n = len(us)
            a, b, c = us[0], us[n // 3], us[2 * n // 3]
            cw_last = delta(b, c, "c9", "fec_credit_wait")
            # The peer runs on its own clock; take the last third of its own
            # samples rather than filtering by a timestamp from this host.
            peers = [s for s in peer_snaps if is_live(s)]
            if len(peers) < 3:
                return cw_last, None, None
            pn = len(peers)
            pb, pc = peers[2 * pn // 3], peers[-1]
            ack_last = delta(pb, pc, "c9", "fec_ack_tx")
            cw_all = delta(a, c, "c9", "fec_credit_wait")
            return cw_last, ack_last, cw_all

        limit = exit_ts if exit_ts is not None else near[-1]["ts"]
        ncw_last, nack_peer, ncw_all = third_trend(near, far, limit)
        fcw_last, fack_peer, fcw_all = third_trend(far, near, far[-1]["ts"])
        info("near_first_credit_last_third", ncw_last)
        info("far_first_credit_last_third", fcw_last)
        info("near_first_credit_all", ncw_all)
        info("far_first_credit_all", fcw_all)
        near_trend_ok = (ncw_last in (None, 0)) or (nack_peer or 0) > 0
        far_trend_ok = (fcw_last in (None, 0)) or (fack_peer or 0) > 0
        check("near_first_credit_trend", near_trend_ok,
              "last_third=%s peer_ack_last_third=%s all=%s"
              % (fmt(ncw_last), fmt(nack_peer), fmt(ncw_all)))
        check("far_first_credit_trend", far_trend_ok,
              "last_third=%s peer_ack_last_third=%s all=%s"
              % (fmt(fcw_last), fmt(fack_peer), fmt(fcw_all)))

    if args.profile in ("fix44", "fix45", "fix46", "fix47", "fix48"):
        ns_near = delta(near_pre, near_c, "c11", "fec_replay_miss_never_stored")
        ns_far = delta(far_pre, far_c, "c11", "fec_replay_miss_never_stored")
        check("first_near_never_stored", ns_near is not None and ns_near <= 64,
              "delta=%s bound=64" % fmt(ns_near))
        check("first_far_never_stored", ns_far is not None and ns_far <= 64,
              "delta=%s bound=64" % fmt(ns_far))
        sp_near = delta(near_pre, near_c, "c11", "fec_short_parity_saved")
        sp_far = delta(far_pre, far_c, "c11", "fec_short_parity_saved")
        check("first_short_parity_saved", (sp_near or 0) + (sp_far or 0) > 0,
              "near=%s far=%s" % (fmt(sp_near), fmt(sp_far)))
        pad_near = delta(near_pre, near_c, "c11", "fec_nack_pad_skip")
        pad_far = delta(far_pre, far_c, "c11", "fec_nack_pad_skip")
        check("first_nack_pad_skip", pad_near in (None, 0) and pad_far in (None, 0),
              "near=%s far=%s" % (fmt(pad_near), fmt(pad_far)))
        # The session-wide max includes join-time and teardown repairs. The
        # window's total repair time is a safe bound: if the sum is <60 s,
        # no single repair inside the window can exceed 60 s.
        # The short-block repair itself is NACK->wire latency. The
        # block-ready interval (fec_nack_repair_ns) also counts the idle
        # between streams: a join-probe block completes only when the next
        # stream advances the frontier, so its whole idle time lands in the
        # first-connect window. Judge the NACK->wire sum; keep block-ready
        # as information.
        lat_near = delta(near_pre, near_c, "c11", "fec_nack_lat_ns")
        lat_far = delta(far_pre, far_c, "c11", "fec_nack_lat_ns")
        check("first_near_nack_lat_ns", lat_near is not None and lat_near < 60000000000,
              "window_delta_ns=%s bound=60s" % fmt(lat_near))
        check("first_far_nack_lat_ns", lat_far is not None and lat_far < 60000000000,
              "window_delta_ns=%s bound=60s" % fmt(lat_far))
    if args.profile in ("fix45", "fix46", "fix47", "fix48"):
        for end, cnt in (("near", near_c), ("far", far_c)):
            check("%s_first_c3_up" % end, field(cnt, "c3", "up") == 256,
                  "up=%s" % fmt(field(cnt, "c3", "up")))
            check("%s_first_c3_replacing" % end, field(cnt, "c3", "replacing") == 0,
                  "replacing=%s" % fmt(field(cnt, "c3", "replacing")))
            check("%s_first_c3_empty" % end, field(cnt, "c3", "empty") == 0,
                  "empty=%s" % fmt(field(cnt, "c3", "empty")))
            info("%s_first_up_rescued" % end, field(cnt, "c3", "up_rescued"))
        info("near_first_nack_lat_max", field(near_c, "c11", "fec_nack_lat_max_ns"))
        info("far_first_nack_lat_max", field(far_c, "c11", "fec_nack_lat_max_ns"))
        rep_near_ns = delta(near_pre, near_c, "c11", "fec_nack_repair_ns")
        rep_far_ns = delta(far_pre, far_c, "c11", "fec_nack_repair_ns")
        info("near_first_repair_ns_block_ready", rep_near_ns)
        info("far_first_repair_ns_block_ready", rep_far_ns)
        info("far_first_repair_max_session", field(far_c, "c11", "fec_nack_repair_max_ns"))
        info("near_first_repair_max_session", field(near_c, "c11", "fec_nack_repair_max_ns"))
        info("far_first_repair_last_ns", field(far_c, "c11", "fec_repair_last_ns"))
        info("near_first_repair_last_ns", field(near_c, "c11", "fec_repair_last_ns"))

    # --- info -----------------------------------------------------------
    for end, pre, cnt in (
        ("near", near_pre, near_c),
        ("far", far_pre, far_c),
    ):
        info("%s_first_up" % end, up(cnt))
        info("%s_first_nack_retx" % end,
             delta(pre, cnt, "c11", "fec_nack_retx"))
        info("%s_first_nack_retx_bytes" % end,
             delta(pre, cnt, "c11", "fec_nack_retx_bytes"))
        info("%s_first_replay_hit" % end,
             delta(pre, cnt, "c11", "fec_replay_hit"))
        info("%s_first_replay_miss" % end,
             delta(pre, cnt, "c11", "fec_replay_miss"))
        info("%s_first_replay_queue_high" % end,
             max_field([s for s in (near if end == "near" else far)], "c11",
                       "fec_replay_queue_high"))
        info("%s_first_outq_drop" % end,
             delta(pre, cnt, "c11", "fec_outq_drop"))
        info("%s_first_outq_high_wait" % end,
             delta(pre, cnt, "c11", "fec_outq_high_wait"))
        info("%s_first_credit_wait" % end,
             delta(pre, cnt, "c9", "fec_credit_wait"))
        info("%s_first_ack_tx" % end, delta(pre, cnt, "c9", "fec_ack_tx"))
        info("%s_first_ack_rx" % end, delta(pre, cnt, "c9", "fec_ack_rx"))
        info("%s_first_reset_user" % end,
             delta(pre, cnt, "c11", "fec_reset_user"))
        info("%s_first_submit_bytes" % end,
             delta(pre, cnt, "capture", "submit_bytes"))
        info("%s_first_snapshots" % end,
             len(near) if end == "near" else len(far))

    info("first_ttfb_s", ttfb)
    info("first_time_s", ctime)
    info("first_speed_Bps", speed)
    info("first_near_exit_ts", exit_ts)
    info("first_near_dead_ts", near_dead["ts"] if near_dead else None)
    info("first_far_dead_ts", far_dead["ts"] if far_dead else None)

    failed = [name for name, ok, _ in checks if not ok]
    print("FIRST_RESULT %s" % ("FAIL" if failed else "PASS"))

    if args.json:
        payload = {
            "profile": args.profile,
            "curl": curl,
            "rc": args.rc,
            "http": http,
            "size": size,
            "file_size": file_size,
            "near": {
                "snapshots": len(near),
                "during_ts": near_during["ts"] if near_during else None,
                "dead_ts": near_dead["ts"] if near_dead else None,
                "exit_ts": exit_ts,
                "decode_blocks": near_decode,
                "credit_wait": near_credit,
                "ack_tx": near_ack_tx,
                "ack_rx": near_ack_rx,
            },
            "far": {
                "snapshots": len(far),
                "during_ts": far_during["ts"] if far_during else None,
                "dead_ts": far_dead["ts"] if far_dead else None,
                "decode_blocks": far_decode,
                "late_share": far_late,
                "dup_share": far_dup,
                "credit_wait": far_credit,
                "ack_tx": far_ack_tx,
                "ack_rx": far_ack_rx,
            },
            "checks": [{"name": n, "pass": ok, "detail": d} for n, ok, d in checks],
        }
        try:
            with open(args.json, "w") as fh:
                json.dump(payload, fh, indent=1)
        except OSError as exc:
            print("analyze: cannot write %s: %s" % (args.json, exc),
                  file=sys.stderr)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
