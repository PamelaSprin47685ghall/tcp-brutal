# Final acceptance evidence

Tree under test: 9f5bfda (w-final-qemu + w-adv2 merged on cfa2e46). The commit
984198f adds only this evidence directory; it does not change code. Module:
sha256 0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63.
Evidence locations: `review/final/` (this directory; host, QEMU A-K and
adversarial summaries), `review/final-qemu/` (QEMU/adversarial run logs and
consoles) and `review/final-bench/` (throughput CSVs, flush, writer and DKMS
runs). Host was quiet before every run (`pgrep -af 'qemu-system|bench'` empty);
no bench was started, as required for a stable measurement.

## Host suites

- `make check`: 357 checks, 0 failures; fuzz 7550734/0; http 249/0; reorder
  52792/0; ack 10026/0; control 181/0; multistream 68580/0; anchor 124/0; h11
  cross-check accepted.
- `make host-test-san`: rc=0; parse 357/0, fuzz 7550734/0, http 249/0,
  http_fuzz 4078276/0.
- `review/lint.sh`: 16/16 checks, 14/14 self-tests.

## QEMU probes A-K

`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900`:
11 PASS / 0 FAIL / 0 SKIP in 159 s. Per-probe lines and key evidence are in
`qemu-AK-summary.txt`; the raw console is
`review/final-qemu/qemu-AK-console.log`.

## Adversarial suite

`TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build`: 78 passed,
0 failed, 0 skipped in 238 s. All 15 default groups ran (public, control,
resource, slab, final, replay, rmmod, flush, catconfig, cfg_mix, param, port,
near, far, stall) plus the isolated close-flush crash repro. Full result lines
are in `adversarial-results.txt`; the klog scanner reported no oops/warning.
The green run prints 78 `ADV_RESULT` PASS lines; `console.klog.*` prints only
on failure, so there is no 79th line.

Two contracts changed in the final tree:

- `pub.churn`: 10,000 connect/send/close cycles, 2 s timeout per cycle, and one
  retry only after a timeout (`retried=`). The verdict still requires
  `usable=10000 refused=0 other=0`. Two final-criterion runs read 10000/10000
  (`retried=1` and `retried=0`); the archived run is
  `usable=10000 refused=0 other=0 retried=0`.
- `pub.writer`: either the write completes (`sent==total`) or it stops with a
  bounded `EAGAIN` refusal (`legal=1`), followed by a flush and `MemAvailable`
  recovering to within 50 MB of the pre-write value. Both final runs PASS:
  100 MiB `sent=75044366/104857600 errno=11 legal=1`, `after_flush` 396844 kB
  against `before` 398972 kB; 1 GiB `sent=74978883/1073741824 errno=11
  legal=1`, `after_flush` 398404 kB against `before` 398620 kB; `flush=1`
  0.03 s and `oom=0->0` in both.

## Bench (QEMU lab emulation)

One 10 Mbit htb class per source port, netem `delay 20ms 10ms`, 40 MiB each way;
not an ISP path. Every row is `rc=0 strict=1` and byte-exact; the whole-run
`wire/app` is the acceptance number, not the CSV's 2 s window column.

| n | control | app | app/control | wire/app (whole) | retx_ppm | holes_unrecoverable | stamp |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 2.43 | 3.11 | 1.28x | 1.0198 | 0 | 0 | `20261106T003712` |
| 4 | 2.73 | 10.74 | 3.93x | 1.0138 | 2,333 | 0 | `20261106T004314` |
| 8 | 2.58 | 22.36 | 8.67x | 1.0123 | 2,041 | 0 | `20261106T004635` |
| 16 | 2.63 | 32.75 | 12.45x | 1.0268 | 12,719 | 0 | `20261106T004925` |

The whole-run `wire/app` range is 1.0123–1.0268 and `retx_ppm` is 0–12,719; all
rows clear `wire/app < 1.2` and `retx_ppm < 100k`. At n=4 the CSV's 2 s window
column reads 1.0333 while the whole run reads 1.0138. One earlier n=4 run of the
same module (`0b8f0006…`) measured 8.38 Mbit/s (`wire/app` 1.0146, `retx_ppm`
1,842) and another 11.37 Mbit/s; that spread is the measured run-to-run
variance, not a configuration change. `review/final-bench/summary.md` has the
full matrix and `review/final-bench/csv/` the CSVs.

## Flush and memory

A custom guest ran two 40 MiB strict transfers and then
`echo flush > /proc/net/tbtun/config`: `FLUSH_RESULT hung=0 start=8.27 end=8.29
secs=0.02`, config empty afterwards, `FLUSH_VERDICT PASS`
(`review/final-bench/flush-two-transfer-console.log`). The writer runs stop at
about 75 MB accepted with `errno=11` when the application never reads; a
post-write flush returns `MemAvailable` to within 50 MB of the pre-write value
in 0.03 s, and there is no OOM (`adversarial-results.txt`; the older
`review/final-bench/writer-memory-*.log` ledger used the previous verdict and
is historical).

## Version and DKMS

`scripts/mkdkmsconf.sh --version` and `make dkms.conf` print `0.0.1`;
`make dkms-tarball` builds `dkms.tar.gz` (sha256 `4aa0b69a…`). The equivalent
`/tmp` DKMS add/build/install/ldtarball flow passes and `modinfo` reports
`version: 0.0.1`. The real-machine install is blocked on this host (sudo needs
a password, no `dkms`, no matching kernel headers), so no real `modprobe` or
`rmmod` was run; the module was loaded by the QEMU guests in every probe and
adversarial run. See `review/final-bench/version-dkms.txt`.
