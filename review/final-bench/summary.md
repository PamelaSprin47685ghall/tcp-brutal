# tbtun 0.0.1 final performance/resource acceptance

Run date: 2026-11-06 00:37–01:06 +08:00.
Worktree: `/home/kunweiz/tbtun-worktrees/final-bench`, branch `w-final-bench`,
HEAD `68bfbaa52c366f998527b4eb1050746283302757` (baseline master 68bfbaa).
Host kernel 7.2.9-1-MANJARO, QEMU KVM, guest 2 vCPU / 512 MiB, kernel
7.2.9-1-MANJARO.

Module under test: `tbtun.ko` sha256
`0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63`,
built by `source ~/.cache/tbtun-build/env.sh && make check`
(all host tests 0 failures: 357 + 7,550,734 fuzz + 249 + 52,792 + 10,026 +
181 + 68,580 + 124 checks).

`benches/qemu/init` sha256 was
`6c1d5a59bd171a42f6e5e931c633f5577a771ca4ca7a0ab62d63b0a87bced5dd`
before and after every throughput run; no edit landed during the runs.

All QEMU work was serialised with
`flock -x /tmp/tbtun-qemu.lock -c '<command>'`. Every throughput row records
`host_qemu_others=0`.

## 1. Throughput, 40 MiB strict send-then-read

Commands (one boot per row, exactly as the harness documents):

```sh
benches/run.sh throughput --n 1  --mib 40
benches/run.sh throughput --n 4  --mib 40
benches/run.sh throughput --n 8  --mib 40
benches/run.sh throughput --n 16 --mib 40
```

CSVs: `review/final-bench/csv/20261106T00{3712,4314,4635,4925}-throughput*.csv`
(sha256 in `csv/sha256.txt`); consoles under `review/final-bench/console/`.
The harness's `wire_per_app` column is a 2 s window ratio; `wire/app` below is
the whole-run `wire_bytes / app_bytes` the acceptance asks for.

| n | rc | strict | app Mbit/s | wall s | wire/app (whole) | wire/app (2 s col) | retx_ppm | hole/slow/pathloss | holes_unrec | carriers | kworker % | cpu total % | qemu RSS kB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1  | 0 | 1 | 3.11  | 222.26 | 1.0198 | 1.0194 | 0     | 0/0/0     | 0 | 1/1   | 98.6 | 107.6 | 307112 |
| 4  | 0 | 1 | 10.74 | 61.99  | 1.0138 | 1.0333 | 2333  | 9/15/0    | 0 | 4/4   | 99.1 | 109.6 | 330992 |
| 8  | 0 | 1 | 22.36 | 31.02  | 1.0123 | 1.0072 | 2041  | 0/21/0    | 0 | 8/8   | 98.7 | 108.4 | 332176 |
| 16 | 0 | 1 | 32.75 | 27.75  | 1.0268 | 1.0285 | 12719 | 0/131/0   | 0 | 16/16 | 98.2 | 109.9 | 338644 |

Control rows (one shaped 5-tuple): 2.43 / 2.73 / 2.58 / 2.63 Mbit/s; the
n=4..16 rows ran with no foreign QEMU.

Acceptance:

- n=1: `rc=0 strict=1`, wire/app 1.0198 < 1.1, retx 0 < 50k. **PASS**
  (the recorded 2026-11-05 failure `rc=3` no longer reproduces).
- n=4: `rc=0 strict=1`, app 10.74 >= 9.20, wire/app 1.0138 < 1.1,
  retx 2333 < 50k. **PASS on the first run; no rerun needed.**
- n=8: `rc=0 strict=1`, app 22.36, wire/app 1.0123, retx 2041. No oops, no
  timeout, QEMU RSS 332 MB. **PASS (record-only).**
- n=16: `rc=0 strict=1`, app 32.75, wire/app 1.0268, retx 12719, carriers
  16/16, `retain_bytes_max` 5.84 MB, QEMU RSS 339 MB. No oops, no timeout.
  **PASS (record-only).**

Scaling: 1 -> 4 -> 8 -> 16 carriers gives 3.11 -> 10.74 -> 22.36 -> 32.75
Mbit/s; 16 carriers is 10.5x the one-carrier app rate. Every row is byte-exact.

### CPU reading: the workqueue burns about one vCPU

`cpu_total_pct` is system+irq+softirq from the guest's `/proc/stat`; the guest
prints `clock ticks=100 ncpu=2`, so the unit is right. It reads
107.6/109.6/108.4/109.9% of one vCPU, and `kworker_pct` reads
98.6/99.1/98.7/98.2%: almost all of the kernel time is in workqueue context.
I checked the harness's field parse against `/proc/1/stat`: after stripping
`pid (comm)`, `a[12]`/`a[13]` are `utime`/`stime` (fields 14/15), so the
column is valid; the ~99% is real kworker CPU, not a misparse.

This is ~20-30x the kworker share recorded in `benches/README.md` (2.7% at
n=4, 4.3% at n=8) and ~6x its total CPU (17.7% at n=4). The current tree is
also 1.5x faster in wall clock (62 s vs 95-100 s), so the trade is throughput
for CPU: the pump/feedback cadence (2 ms `w_pump`, 10 ms `w_fbk`, 50 ms
`w_poll`) appears to poll aggressively and keeps a kworker on-CPU for the
whole transfer. It is not retransmit work: n=1 has `retx_ppm=0`,
`holes=0/0/0` and still shows 98.6%. Needs per-workqueue profiling (or ftrace)
and a bisect between the 04:44 sweep and the pre-parallel baseline before
release; not fixed here (acceptance only).

Idle control: the same guest with a raw n=4 tunnel configured and no transfer
measures `busy_pct=0.1 kworker_pct=0.0` over 20 s, both before and after the
configuration (`review/final-bench/idle-cpu-console.log`). The ~99% is
therefore strictly transfer-driven, not a timer that spins while configured.

## 2. Flush after two 40 MiB transfers

Custom guest (`review/final-bench/harness/flush-two-transfer-init.sh`), same
adversarial loopback topology, raw n=4 tunnel, `host/tbt_burst` strict phase
twice, then the operator write is timed with `/proc/uptime`:

```
XFER1_START uptime=3.48
XFER1_END rc=0 uptime=5.67
XFER2_START uptime=5.67
XFER2_END rc=0 uptime=8.27
FLUSH_RESULT hung=0 start=8.27 end=8.29 secs=0.02
FLUSH_CONFIG_AFTER (empty)
FLUSH_VERDICT PASS
```

Both 40 MiB transfers completed byte-exact (`tbt_burst rc=0`, strict phase
passed), and `echo flush > /proc/net/tbtun/config` returned in **0.02 s**
(< 1 s). The config is empty afterwards.

Required adversarial groups:

```sh
TBT_ADV_GROUPS="flush replay" adversarial/run.sh --no-build
```

```
ADV_RESULT init.flush_midstream PASS sha=0b8f0006a7a10fe3 flush mid-transfer: app closed cleanly, ECHO_ABORT op=read errno=104 bytes=1114112
ADV_RESULT init.echo_after_flush PASS sha=0b8f0006a7a10fe3 ECHO_OK bytes=1048576
ADV_RESULT init.cfg_replay PASS sha=0b8f0006a7a10fe3 line-by-line config replay rc=0, transfer closed cleanly: ECHO_ABORT op=read errno=104 bytes=1114112
ADV_RESULT init.echo_after_replay PASS sha=0b8f0006a7a10fe3 ECHO_OK bytes=1048576
ADV_RESULT ctl.close_flush PASS sha=0b8f0006a7a10fe3 the guest survived the close flush
```

`run.sh: 5 passed, 0 failed, 0 skipped`. **PASS.**

## 3. Writer backpressure and memory ledger

Required group:

```sh
TBT_ADV_GROUPS=writer adversarial/run.sh --no-build
```

```
ADV_RESULT pub.writer PASS ... attempted 100 MiB, accepted 74913400 bytes then blocked (errno=11), MemAvailable 399708 -> 267104 kB
ADV_RESULT pub.writer PASS ... attempted 1024 MiB, accepted 75187154 bytes then blocked (errno=11), MemAvailable 411484 -> 269784 kB
ADV_RESULT ctl.close_flush PASS ...
```

`run.sh: 3 passed, 0 failed, 0 skipped`.

Detailed ledger (`review/final-bench/harness/writer-memory-init.sh`, raw n=1
tunnel; a 0.2 s sampler records the minimum; quiesce is 5 s after close; the
final flush line was added on the second run):

| case | accepted bytes | errno | time to EAGAIN | MemAvailable before | min during | after close | quiesce (5 s) |
|---|---|---|---|---|---|---|---|
| 100 MiB | 75,105,611 | 11 (EAGAIN) | 16.48 s | 386,404 kB | 230,888 kB | 276,292 kB | 276,292 kB |
| 1 GiB   | 75,044,366 | 11 (EAGAIN) | 16.39 s | 406,804 kB | 233,384 kB | 280,148 kB | 279,744 kB |

```
MEM_BEFORE_FINAL_FLUSH 279380
MEM_AFTER_FINAL_FLUSH  410836
```

Repeat (first run, same shape): 100 MiB accepted 75,110,697 B, min 205,928 kB;
1 GiB accepted 74,913,400 B, min 235,676 kB.

Reading:

- The 1 GiB offer accepted the same ~75 MB as the 100 MiB offer: the public
  socket/module backpressure stops the writer at a bounded buffer, so offered
  volume does not become resident memory. `errno=11` (EAGAIN) is the observed
  flow-control boundary, not an OOM or a crash. The guest never came near OOM
  (minimum MemAvailable 230–233 MB of 512 MB).
- The bytes held after the app's close are released by a tunnel flush:
  279,380 -> 410,836 kB (+131 MB) with one `echo flush`. The second case's
  `before=406,804` is also post-flush and confirms the same recovery.
- So the memory is reclaimable, but the flush is the reclaim point; a closed
  stream alone does not return it within 5 s. That is a behaviour to document,
  not a leak: repeated cases do not grow without bound (both cases cap at the
  same ~75 MB).

## 4. Version and DKMS

Evidence: `review/final-bench/version-dkms.txt`.

| check | result |
|---|---|
| `./scripts/mkdkmsconf.sh --version` | `0.0.1` (rc 0) |
| `make dkms.conf` | rc 0; `PACKAGE_VERSION="0.0.1"` |
| `modinfo ./tbtun.ko` | `version: 0.0.1`, `vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload` |
| `make dkms-tarball` | rc 0; `dkms.tar.gz` sha256 `4aa0b69a5311e8eb55c541b39147859a37573476e3f938d735db1499713f65a5` |
| extracted tree rebuild | `tar xzf` -> `/tmp/tbtun-dkms-final-bench/dkms_source_tree`; `make KERNEL_DIR=<KDIR> all` rc 0 in 8.27 s; rebuilt `tbtun.ko` sha256 `e136b1fd47823d36d8c3e2dd5c4031e5e457ecb19ddbc5c90ae4e2607df2c385`, `version: 0.0.1`, same `srcversion 533B6F5D7025857930ACD8B` |
| real DKMS install | not attempted: `dkms` is not installed and `sudo` needs a password on this host (recorded environment blocker) |

The extracted-tree module differs from the in-tree `tbtun.ko` only in build
metadata (same `srcversion`, same vermagic, same version); the tarball is
buildable from a clean directory with the documented `KERNEL_DIR` invocation.

## 5. Artifacts

```
review/final-bench/
  summary.md                      this file
  provenance.txt                  HEAD, module sha, benches/qemu/init sha before/after
  version-dkms.txt                raw version/DKMS commands and outputs
  csv/                            throughput + carriers CSV copies, sha256.txt
  console/                        four throughput guest consoles
  adversarial-writer-results.txt  raw ADV_RESULT lines
  adversarial-flush-replay-results.txt
  adversarial-*-console.log       group consoles
  flush-two-transfer-console.log  custom two-transfer flush guest
  idle-cpu-console.log            no-transfer CPU control
  writer-memory-console.log       writer ledger, run 2 (has final flush)
  writer-memory-console-run1.log  writer ledger, run 1
  writer-memory-run2.log          run 2 summary lines
  harness/                        three custom init scripts and run-guest.sh
```

## 6. Known boundaries and risks

1. **CPU cost is a regression signal.** The measured kernel cost is ~1 vCPU
   (107.6–109.9%) at every carrier count, ~99% of it in kworker context,
   against 17.7% total / 2.7% kworker at n=4 in the recorded sweep. The
   column parse is valid. The higher throughput may be the flip side of a
   polling trade-off, but the ~20-30x workqueue CPU increase needs profiling
   and a bisect before release.
2. **`errno=11` is the designed backpressure boundary.** Both writer cases stop
   at ~75 MB accepted; memory stays bounded and is released by `flush`, not by
   stream close within 5 s.
3. **The whole-run `wire/app` is the acceptance number**, not the CSV's
   `wire_per_app` (2 s window). At n=4 the window column reads 1.0333 while the
   whole run is 1.0138.
4. **n=1 no longer fails**: 3.11 Mbit/s, rc=0, strict=1, retx=0, wire/app
   1.0198. The old 1.98 wire/app / rc=3 row is historical.
5. **DKMS install remains an environment blocker** (no passwordless sudo, dkms
   absent); the tarball build path is verified directly.
6. **No oops, no hang, no timeout, no OOM** in any of the four throughput
   boots, the two adversarial groups, the crash repro, or the custom
   guests. The only console warnings are the known `sch_htb quantum` notices.
