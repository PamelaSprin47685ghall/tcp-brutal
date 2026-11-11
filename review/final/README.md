# Final acceptance evidence

Tree under test: f1282df (`w-docs5` merged on 3ba0e6a as 5552448, then
`w-evidence2` merged as f1282df, both `--no-ff`). No product code changed:
the diff against 3ba0e6a touches only `adversarial/` and `review/`, and the
module is byte-identical to the earlier b57b438 build. Module: sha256
0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63.
Evidence locations: `review/final/` (this directory; host, QEMU A-K and
adversarial summaries), `review/final-qemu/` (QEMU/adversarial run logs and
consoles, `*-f1282df.log` / `*-f1282df.txt`), `review/dkms-qemu/` (DKMS
install/load/unload on the clean guest kernel) and `review/final-bench/`
(throughput CSVs and flush runs). Host was quiet before every
run (`pgrep -af 'qemu-system|bench'` empty); no bench was started, as required
for a stable measurement.

## Host suites

- `make check`: 357 checks, 0 failures; fuzz 7550734/0; http 249/0; reorder
  52792/0; ack 10026/0; control 181/0; multistream 68580/0; anchor 124/0; h11
  cross-check accepted.
- `make host-test-san`: rc=0; parse 357/0, fuzz 7550734/0, http 249/0,
  http_fuzz 4078276/0.
- `review/lint.sh`: 16/16 checks, 14/14 self-tests.

`test_parse` reports 357 checks; 365 was the older snapshot taken before the
five dead keys `framing`/`chunk`/`max_retry`/`dev`/`uid` and their parse cases
were deleted. The deleted cases covered keys already removed from the grammar,
not behavioural coverage.

Full per-step lineage and the evidence boundary: `review/test_parse-lineage.md`.
The older `review/final-checklist.md` and `review/final-qemu/summary.md` are
historical; both now carry a SUPERSEDED header. Do not use their verdicts.

## QEMU probes A-K

`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900`:
11 PASS / 0 FAIL / 0 SKIP in 165.7 s, run under
`flock -x /tmp/tbtun-qemu.lock`. Per-probe lines and key evidence are in
`qemu-AK-summary.txt`; the raw console is
removed by the 2026-11-12 repository slimming.

## Adversarial suite

`TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build`: 91 passed,
0 failed, 0 skipped in 284.4 s (run.sh wall 4m44.4s). All 18 groups ran
(public, control, resource, slab, final, replay, rmmod, flush, catconfig,
cfg_mix, param, port, near, far, stall, httptorn, hole, create) plus the
isolated close-flush crash repro. Full result lines are in
`adversarial-results.txt`; the klog scanner reported no oops/warning and no
`console.klog.*` line. The green run prints 91 `ADV_RESULT` PASS lines;
`console.klog.*` prints only on failure, so there is no 92nd line.

`w-evidence2` added 13 runtime cases (78 + 13 = 91). Their key lines:

- `pub.writer_drain` (100 MiB, draining reader): `sent=104857600/104857600
  got=104857600/104857600 errno=0/0 eagain=0 blocked=0 secs=1.40 mbps=71.5
  oom=0->0`. `pub.writer_drain_1g` (1 GiB): `sent=1073741824/1073741824
  got=1073741824/1073741824 errno=0/0 eagain=0 blocked=0 secs=14.64 mbps=69.9
  oom=0->0`. Both flush `MemAvailable` back to within 50 MB.
- `pub.writer` keeps the blocked-writer contract: 100 MiB
  `sent=73400320/104857600 errno=0 eagain=0 blocked=1 child_state=running`,
  1 GiB `sent=74448896/1073741824 errno=0 eagain=0 blocked=1`; the probe sets
  no `SO_SNDTIMEO`, so `EAGAIN` would be a failure. `oom=0->0` in both.
- `cfg.big_write` writes 33,335 bytes as 64 maximal tunnel lines (rc=0);
  `cfg.big_roundtrip` serves them in 1000-byte reads and in one 64 KiB read,
  both byte-identical, 64 lines, larger than 32768; `cfg.big_replay` re-renders
  the same 33,335 bytes into the config after a flush; `cfg.echo_after_big`
  echoes 1 MiB byte-exact.
- `cat.http_torn`: 100 reads of `/proc/net/tbtun/http` during an active
  stream — `complete=100 empty=0 partial=0`.
- `hole.created`: a missing offset 0 turns `holes_unrecoverable` 0 -> 1;
  `hole.age`: one `RXHOLEKILL` with `age_us=109970` against
  `hole_budget_ms=100`, i.e. the measured hole, not the ktime epoch.
- `create.baseline` echoes 1 MiB; `create.failure` refuses a new id on a held
  port with no row left and the running tunnels still echoing, and the retry
  after release succeeds; `create.replace` refuses the replacement with
  `old_survived=0`, the destroy-on-failure contract.
- `ctl.release_overflow`: a valid buffered line followed by 1100 bytes gives
  `write2=-1 errno=7` and tunnel 77 is never applied.

`pub.churn` still reads `usable=10000 refused=0 other=0 retried=0`.

## Finding → runtime evidence

Each code finding from `review/findings.md` is paired with the final-tree
runtime test that exercises its path and a fragment of the observed result.
`代码级/不可运行时区分` marks a removal or internal ordering change that leaves
no runtime-distinguishable signal; the reason is stated in the result cell.

| Finding | Runtime evidence | Result line |
|---|---|---|
| 1 orphaned carrier | `init.rmmod`, `near.fin_eagain` | `rmmod rc=0, app closed cleanly`; `fin_seen=1 eof=1 child_rc=0`, `holes_unrecoverable 0 -> 0` |
| 2 trim UAF | `near.fin_eagain`, `init.slab_leak` | `fin_seen=1 eof=1 child_rc=0`; `32k slabs grew by 0 objects over 100 connections` |
| 3 create failure / old leak | `create.failure`, `create.replace` | `new-id create refused, no tunnel 9, tunnels 1/2 still echo, retry after release works`; `old tunnel survived=0 usable=0` |
| 4 callback teardown | `ctl.close_flush`, `init.rmmod` | `the guest survived the close flush`; `rmmod rc=0, app closed cleanly` |
| 5 double stream insert | `pub.concurrent` | `n=100 connected=100 echoed=100 short=0 corrupt=0 timeout=0 isolation=1` |
| 6 pump-lock deadlock | `ctl.close_flush`, `near.fin_eagain` | `the guest survived the close flush`; `fin_seen=1 eof=1 child_rc=0` |
| 7 sk release order | `init.rmmod`, `ctl.close_flush` | `rmmod rc=0, app closed cleanly`; `the guest survived the close flush` |
| 8 accept before session | `pub.concurrent` | `n=100 connected=100 echoed=100 short=0 corrupt=0 timeout=0 isolation=1` |
| 9 short write | `init.echo_sane`, `pub.writer_drain` | `ECHO_OK bytes=2097152`; `sent=104857600/104857600 got=104857600/104857600 errno=0/0 eagain=0 blocked=0 secs=1.40` |
| 10 peers c->sk | `cat.cfg_self`, `far.data_path` | `cat config > config rc=0, 2 tunnels still present`; `anchor echo: 8 bytes 'pingpong' corrupt=0` |
| 11 RETIRE / TBT_CONV | `far.anchor`, QEMU `E`/`K` | `carrier joined index 0, far generation 4`; `K PASS`, `spare_activated=1 replaced=0 lost=2` |
| 12 HELLO bit layout | `far.hello_profile9`, `far.hello_profile0` | `HELLO byte 9 = profile 1, generation 2: accepted as a generation`; `profile 0 must be refused` |
| 13 PREDICT inert | 代码级/不可运行时区分 | `grep -rn 'predict_' tbtun/ is empty; 0x11/0x12 removed, no runtime path` |
| 14 data_frames before send | `far.data_path`, `near.fin_eagain` | `submit_bytes=12 tx_ack=0`; `tx_ack=524288 txq=0` |
| 15 fbk_mask unused | 代码级/不可运行时区分 | `grep -rn fbk_mask tbtun/ is empty; parameter and call-site argument removed, no runtime path` |
| 16 first-HELLO nonce | `far.anchor`, `far.dup_index`, `far.nonce_pinned`, `far.src_anchor` | `joined index 0, far generation 4`; `join_src_reject 0 -> 1, join_bad 0 -> 1` |
| 17 http proc entry | `init.rmmod`, `init.reload` | `rmmod rc=0, app closed cleanly`; `module reloaded` |
| 18 module parameters | `init.max_carriers`, `init.max_tunnels` | `n=8 refused with max_carriers=4`; `64 tunnels accepted before the limit` |
| 19 one-shot config read | `cfg.big_roundtrip` | `one-shot read 33335 bytes; expected 33335 bytes; byte-identical` |
| 20 overlapping reorder | `far.overlap` | `echoed 49152/49152 bytes corrupt=0`; `ooo_bytes 0 -> 0`; `holes_unrecoverable 0 -> 0` |
| 21 non-atomic counters | QEMU `C`/`E` | `C: d_wire=0 d_data=0 d_retx=0`; `E: d_wire=0 d_data=0 d_retx=0`; `live_bytes-peers_tx_bytes=0` |
| 22 pinned control on -EAGAIN | `near.fin_eagain`, `near.rst_eagain` | `fin_seen=1 eof=1 child_rc=0`; `rst_seen=1 eof=0 child_rc=0` |
| 23 zero reorder/hole budgets | `test_parse` | `357 checks, 0 failures` |
| 24 hole age | `hole.age` | `RXHOLEKILL x1 age_us=109970 with hole_budget_ms=100` |
| 25 ports 0-0 | `init.cfg_roundtrip` | `the canonical config replays line by line` |
| 26 carrier SO_REUSEADDR | QEMU `E`/`K` | `TBT_PROBE_RESULT E PASS`; `TBT_PROBE_RESULT K PASS` |
| 27 zero-length DATA | `test_fuzz` | `checks=7550734 failures=0` |
| 28 http header buffer | `far.headers_big`, `near.resp_hdr_big` | `9000-byte header block must be PROTO`; `response header > carrier buffer: carrier poisoned` |
| 29 render > line max | `cfg.big_write`, `test_parse` | `wrote 33335 bytes / 64 maximal tunnel lines, rc=0`; `357 checks, 0 failures` |
| 30 32 KiB truncation | `cfg.big_roundtrip` | `served in 1000-byte reads: 33335 bytes, 64 lines`; `byte-identical and larger than 32768` |
| 31 release overflow | `ctl.release_overflow` | `write2=-1 errno=7; tunnel 77 applied=0` |
| 32 blank line to /ports | `cat.cfg_mix` | `bad line not applied, far line applied (write rc=0)` |
| 33 torn http read | `cat.http_torn` | `complete=100 empty=0 partial=0 first_bad=-1` |
| 34 partial procfs init | `init.rmmod`, `init.reload` | `rmmod rc=0, app closed cleanly`; `module reloaded` |
| 35 dead keys/functions | 代码级/不可运行时区分 | `five dead keys and tbt_passive_port()/tbt_tunnel_flush() removed; grep is empty, no runtime path` |
| 36a snd_una gate | `near.fin_eagain` | `pump_hiwat 0 -> 4956 (must grow)`; `holes_unrecoverable 0 -> 0` |
| 36b trim vs batch | `near.fin_eagain` | `txq=0`; `holes_unrecoverable 0 -> 0`; `tx_ack=524288` |
| 36c teardown | `init.rmmod`, `ctl.close_flush` | `rmmod rc=0, app closed cleanly`; `the guest survived the close flush` |
| 36d short-write accounting | `near.fin_eagain` | `tx_ack=524288 txq=0`; `wire=536418`; `holes_unrecoverable 0 -> 0` |
| 37 http_rx NULL | `near.resp_hdr_big`, `far.headers_big` | `response header > carrier buffer: carrier poisoned`; `9000-byte header block must be PROTO` |
| 38 buffer leak | `init.slab_leak`, `init.slab_after_flush` | `32k slabs grew by 0 objects over 100 connections`; `after flush: 32k objects 0 -> 0` |
| 39 constructor order | `create.failure` | `held port: new-id create refused, no tunnel 9, tunnels 1/2 still echo, retry after release works` |

## Bench (QEMU lab emulation)

Measured on the same module sha under b57b438; the module is byte-identical,
so these numbers still describe this tree. One 10 Mbit htb class per source
port, netem `delay 20ms 10ms`, 40 MiB each way; not an ISP path. Every row is
`rc=0 strict=1` and byte-exact; the whole-run `wire/app` is the acceptance
number, not the CSV's 2 s window column.

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
(`review/final-bench/flush-two-transfer-console.log`). The blocked writer runs
stall at 73.4 MB and 74.4 MB accepted (70.0 and 71.0 MiB), still blocked
inside `write()` with `errno=0`, when the application never reads; a
post-write flush returns
`MemAvailable` to within 50 MB of the pre-write value in 0.03 s, and there is
no OOM (`adversarial-results.txt`; the older
`review/final-bench/writer-memory-*.log` ledger used the previous verdict and
is historical). The draining writer completes both 100 MiB and 1 GiB instead
(above), which is the completion half of the same contract.

## Version and DKMS

`scripts/mkdkmsconf.sh --version` and `make dkms.conf` print `0.0.1`;
`make dkms-tarball` builds `dkms.tar.gz` (sha256 `4aa0b69a…`). The equivalent
`/tmp` DKMS add/build/install/ldtarball flow passes and `modinfo` reports
`version: 0.0.1`. The host cannot run the real flow (uid 1000, no `dkms`, no
`/lib/modules/7.2.9-1-MANJARO/build`, `sudo` needs a password), so
`w-dkms-qemu` ran it in a QEMU guest on the clean distribution kernel
`7.2.9-1-MANJARO` with the real dkms 3.4.4: `scripts/install_dkms.sh`
staged/built/installed `tbtun/0.0.1`, `modprobe` published `/proc/net/tbtun`,
a tunnel line round-tripped canonically, `rmmod` unloaded it, `modprobe` again
reloaded it, `rmmod` unloaded it, and `--uninstall` removed every DKMS entry,
source tree and module file. 38 steps, 39 checks, 0 failures; console and
per-step logs in `review/dkms-qemu/`. See also
`review/final-bench/version-dkms.txt`.
