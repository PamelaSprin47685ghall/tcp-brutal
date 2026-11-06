# HND0 carrier-flood hang: repro, fix, regression

Date: 2026-11-06. Tree: `w-hnd0-hang`, based on integration `7de0efe`.
QEMU: 2 vCPU, 512 MiB, KVM, kernel `7.2.9-1-MANJARO` (no KASAN, no lockdep,
no `CONFIG_WQ_CPU_INTENSIVE_REPORT`). HND0 was not used for any test; only its
previous-boot journal was read.

## What happened on HND0

HND0 (`45.11.1.62`, far side, 2 vCPU / 955 MiB) accepted 10138 carrier
connections between 06:22:54 and 06:23:24 UTC, logged
`tbt_accept_work ... hogged CPU` 19 times and
`tbt_replace_work ... hogged CPU` 259 times, and then stopped logging. The host
was hard-reset 62 min later. There was no OOM, panic, hung task or soft lockup
record; pstore was empty.

The boot ran `85f2011`, before the 256-carrier `path_id` fix. Carriers 16..255
failed HELLO encoding, so the near side replaced them 8851 times
(`up=16`, `lost=8864`, `http_parse_err=8864`) and the far side accepted every
replacement, held it as a provisional carrier for the 5 s join deadline, and
retired it.

The kernel did not deadlock; static lock-order analysis found no inversion.
The failure is a livelock plus unbounded memory:

- the far's provisional list had no bound (each entry ~37 KiB);
- `tbt_replace_work()` walked the whole `dead` graveyard on every pass and did
  three or four `cancel_work_sync()` plus a socket release per entry, while the
  graveyard was never freed before teardown (O(n^2) over the storm);
- the accept loop had no per-run budget;
- every accept and every failed dial wrote an unratelimited console line
  (330 accepts/s on HND0; the near retried up to 1280 dials/s);
- every socket state change re-armed the 50 ms poll with zero delay;
- debug-level hex dumps fired per rejected HTTP carrier.

## Repro

`tests/qemu/init` probe Q and `host/tbt_flood.c`. A far and a near tunnel carry
256 `http-kal` FEC-grouped carriers; 64 flood children connect to the far's
carrier port for 20 s, send a partial HTTP request (never HELLO), hold
10-100 ms, close with RST and reconnect; 12 waves take 8 carrier slots down so
the near side dials replacements. Probe Q asserts the far's `prov_peak`,
`prov_reject`, the drain to zero, a userspace heartbeat count, and that a
legitimate replacement still joins after the flood.

```sh
tests/qemu/run.sh --ko "$PWD/tbtun.ko" --probes Q --no-net --timeout 240
tests/qemu/run.sh --ko "$PWD/tbtun.ko" --probes A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P \
    --timeout 1800
```

## Results

| Run | Tree | Result | Log |
| --- | --- | --- | --- |
| probe N, 256 legitimate carriers | `7de0efe` | PASS | `console-N-baseline.log` |
| probe Q | `7de0efe` | OOM at 15.9 s, `slab_unreclaimable=101287` pages, no `TBT_PROBE_RESULT` | `console-Q-baseline.log` |
| probe Q | fix | PASS: `prov_now=0 peak=320 reject=8780`, 200/200 heartbeats, near recovers 255/256 | `console-Q-fix.log` |
| probes A-P | fix | 16 PASS / 0 FAIL / 0 SKIP, 280 s | `console-A-P-fix.log` |

The baseline Q log is 1.3 MiB of console output in 19 s; the fixed log is 73 KiB
for the whole 25 s probe.

The A-P log contains one `!tbt_stream_announced(fr->st, c)` `WARN_ON_ONCE`
during probe N. The same warning is in `console-N-baseline.log`, which was
captured from the unmodified `7de0efe` module, so it is a pre-existing
announce-bookkeeping race and not a result of this change. The `fec=off`
`!work->func` teardown warning that the first A-P run showed 52 times is fixed
by cancelling `w_fec_tx` only for a `fec=on` session.

## Fix

- Provisional carriers are capped at `n_car + max(16, n_car/4)` before any
  allocation; `prov_reject` counts refusals.
- Expired, refused or stopped provisional carriers are released and freed
  immediately instead of being parked on `dead`. The 50 ms poll holds
  `sess->lock` across its non-sleeping provisional branch, and
  `tbt_session_join()` refuses a carrier marked stopping under the same lock,
  so the free cannot race a poll or a late HELLO.
- `tbt_replace_work()` is a delayed work. A pass retires at most 16 carriers and
  releases at most 16 join-replaced carriers, then re-arms after 20 ms. The
  full-graveyard walk is gone; `dead_release` carries only the deferred join
  releases.
- Replacement dials are token-paced at `max(8, n_car/4)` per second. The first
  wave in `tbt_carriers_start()` is not paced. A near slot with no carrier is
  retried, which also fixes a slot that a port-allocation failure used to leave
  dead forever.
- The accept loop accepts at most 32 connections per run, re-queues, and calls
  `cond_resched()`.
- The poll's state-change wake is 10 ms instead of 0.
- Per-accept, per-failed-dial and per-rejected-carrier lines are ratelimited;
  the HTTP/raw hex dumps need `debug >= 3`.
- `prov_now`, `prov_peak` and `prov_reject` are in the `c3` stats line.

## Remaining risk

- The near side's retry for a carrier that never connected is still unpaced
  (200 ms per slot, up to 1280 attempts/s at 256 carriers when the far is
  unreachable). It did not hang the near side in this incident.
- Join-replaced carriers are still retained on `dead` until session teardown
  (~37 KiB each). The replacement pacing bounds the rate, not the lifetime; a
  very long flapping run can still grow memory, far slower than the flood.
- The cap is a hard admission limit. Under a sustained flood a legitimate new
  carrier is refused while the cap is full; recovery is bounded by the 5 s join
  deadline (Q recovered 255/256).
- The QEMU kernel cannot print the HND0 workqueue-hog line, so the local
  evidence is the pre-fix OOM/no-verdict and the post-fix counters, heartbeat
  count and probe verdict.
