# 1 GiB acceptance run — fix20 — FAIL (99.63%, early FIN)

- harness start: `2026-11-07T08:46:11Z`
- transfer: `2026-11-07T08:46:58Z` .. `2026-11-07T09:52:00Z` (3896.767151 s)
- near: `root@192.168.123.65` (aarch64, 6.1.172, fix20
  `9D5B8A616A565FE62DEA315`, ko sha256
  `fe076133e6fa8f050e51d523cb21485400d54a88160502a7aa229169d9050c88`)
- far: `root@45.11.1.62` (fix20 `B39C75B094C277E6177450C`, debug=0, 256 config)
- shape: 256 carriers, n=4 m=4, fec=on, grouped, w=8, crypto=on, http-kal,
  near `dev=eth0`, pref-100 bypass, single curl, 7200 s cap,
  stall 120 s below 64 KiB/s
- verdict: **FAIL**

## 1. Transfer result

| item | value |
|---|---|
| curl rc | 18 (`end of response with 3977607 bytes missing`) |
| http | 200 |
| size_download | 1,069,764,217 B of 1,073,741,824 (99.63%) |
| missing | 3,977,607 B (0.37%) |
| local file | 1,069,764,217 B |
| near sha256 | `35b6797e88ed6fbb3d88be687e63031d51b49a2e0ed787671dc57520150a043a` |
| far full sha256 | `c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93` |
| far prefix (first 1,069,764,217 B) sha256 | `35b6797e88ed6fbb3d88be687e63031d51b49a2e0ed787671dc57520150a043a` |
| elapsed | 3896.767151 s |
| average speed | 274,526 B/s = 2.20 Mbit/s |
| near CPU busy | 6.7% |
| far CPU busy | 31.6% |
| near MemAvailable | 7,512,000 kB |
| near boot_id | `0a91d2eb-3122-41eb-913c-024a4e1fe724` (unchanged) |
| feeder | `pid=23184(python3)` holds `/dev/watchdog0` |

**The delivered prefix is byte-exact.** `head -c 1069764217
/root/accept-1g/accept-1g.bin | sha256sum` on far equals the near file's
sha256 exactly. The tunnel did not corrupt or reorder data; the last 3.98 MB
never arrived because the far-side target connection ended early and the
tunnel propagated the FIN.

## 2. Failure point: far target closed the response early

- far `tx_ack=1,069,764,415` (the HTTP headers account for the 198-byte
  difference from the near's 1,069,764,217 app bytes).
- `fec_reset_user=1` on both ends (the normal "user/peer/application
  teardown" counter, `tbt_stream_kill()` with a reason other than
  `TBT_ST_HOLE`); `fec_reset_hole=0`, `holes_unrecoverable=0`,
  `fec_reset_tcp_failed=0` on both ends.
- No `fec reset: super ... not ready` / `fec deliver conflict` /
  `fec ordered walk hole` line in either kernel window. The FEC layer did
  not reset the stream; the teardown came from the target/application side.
- The far target is `busybox httpd` (v1.37.0, Ubuntu 1:1.37.0-7ubuntu1) on
  `127.0.0.1:5210`. It sent 1,069,764,415 bytes and closed with 3,977,607
  bytes of the 1 GiB file still unsent.
- The near-side per-stream snapshot at curl exit shows the teardown shape:
  `state=5 kill=5 attached=0x2 ... skerr=0 fin_seq=0 fin_q=0 fin_w=0
  rst_w=1`. `state=5`/`kill=5` is `TBT_ST_DEAD`; the stream was reset
  (`rst_w=1`, no FIN sequence), which is why curl reports rc=18 rather than
  a clean EOF. `fec_reset_user=1` on both ends is the matching
  user/peer/application teardown count; `fec_reset_hole=0`.
- The far target EOF trigger is **not** the busybox httpd's 60 s
  `DATA_WRITE_TIMEOUT`: a direct probe on far stopped reading for 65 s and
  the httpd then sent the complete file (4,096 + 1,073,737,926 bytes, then
  EOF). See `far-httpd-timeout-probe.txt`. The 60 s write-timeout
  hypothesis is falsified for this httpd build.
- The far TX path was under heavy pressure (`fec_nack_backpressure=1,492`,
  `fec_replay_queue_high=8,388,408` — the 8 MiB replay queue hit its bound,
  `fec_hol_wait=20,439,592`, `fec_flush_skip=6,469,181`,
  `fec_credit_wait=117,558`), and the far target connection ended at
  1,069,764,415 B. The per-stream `tgt_state`/`tgt_err`/kill reason on the
  far was gone by the post-run snapshot, so the first event on the far
  side is not captured. Next run: sample the far per-stream state at curl
  exit / stream teardown, or export the kill reason before reaping.

## 3. fix20 improvements (measured)

| field | fix19 | fix20 |
|---|---|---|
| delivered | 104,711,916 B (9.75%) | 1,069,764,217 B (99.63%) |
| near `rx_errs` / `rx_proto` | 93 / 93 | **0 / 0** |
| near `replaced` / `lost` | 102 / 9 | **9 / 9** |
| near `connect_fail` | +1 | 0 |
| far `fec_replay_miss` | 959,714 | 21,899 |
| far `fec_replay_miss_enospc` | 872,166 | **0** |
| far `fec_replay_miss_evicted` | 85 | **0** |
| far `fec_replay_miss_never_stored` | 87,463 | 21,899 |
| far `fec_nack_drop` | 0 | 0 |
| far `fec_nack_retx` | 323,667 | 825,558 |
| far `fec_nack_retx_bytes` | 2,071,297,322 | 6,795,028,339 |
| far wire | 2,292,002,003 | 9,003,107,202 |
| far wire / delivered | 21.9x | **8.4x** |
| far retx / delivered | 19.8x | **6.35x** |
| far `fec_nack_backpressure` | n/a | 1,492 |
| far `fec_replay_dedup` | n/a | 26,656 |
| far `fec_nack_filtered_inflight` | n/a | 123 |
| near `fec_nack_filtered_inflight` | n/a | 12,765,522 |
| near `fec_nack_filtered_dup` | n/a | 197,788 |
| near `fec_nack_filtered_young` | n/a | 398,466 |
| near `fec_nack_repair_max_ns` | 16.6 s | 4.53 s (<= 8x RTT 12.79 s) |
| UBSAN | shift-out-of-bounds `tbt_tx.c:2818` | **none** |
| near `c3.stall_declared` | 10 | 23 |
| far `c3.stall_declared` | 780 | 5,734 (5,727 recovered) |
| near `c3.una_stall_us` | 545 s | 9.3 s |
| far `c3.una_stall_us` | 540 s | 5.9 s |

The 256-bit `blocked` mask fix is effective: no UBSAN, no `rx_proto`, and
carrier replacement is back to the lost count. The NACK filter/dedup
eliminated `enospc` and `evicted`; the sender's replay miss rate is 2.6%
(21,899 / 839,318 lookups) and the near-side misses are the far's probes for
near TX frames that were never stored (388,028 `never_stored`; the near's
replay hit is 2 and its `fec_nack_rx=12,126`). Those near-side misses are a
control-direction artifact, not the data path.

## 4. Required-zero fields

| field | near | far |
|---|---|---|
| `fec_stream_refused` | 0 | 0 |
| `fec_replay_miss_enospc` | 0 | 0 |
| `fec_replay_miss_evicted` | 0 | 0 |
| `fec_nack_drop` | 0 | 0 |
| `fec_reset_hole` | 0 | 0 |
| `holes_unrecoverable` | 0 | 0 |
| `fec_reset_tcp_failed` | 0 | 0 |
| c10 hard errors | all 0 | all 0 |
| c9 proto/hdr/payload CRC | 0 / 0 / 0 | 0 / 0 / 0 |

Non-zero, classified:

- `fec_reset_user=1` on both ends: normal stream teardown (see §2).
- `fec_replay_miss_get=388,028` near / `21,899` far: all
  `never_stored`; no `enospc`, no `evicted`.
- `fec_nack_retx=825,558` far / `6,795,028,339` B: repair traffic, not an
  error; the amplification is down from 19.8x to 6.35x.
- `fec_nack_backpressure=1,492` far: the new bounded-reply path; it
  prevented `enospc`, but the underlying TX stall is the likely trigger for
  the source timeout.
- `dead_now` is not exported by fix20; the harness's `dead_now_bound` check
  FAILs on the missing field, which is a tool/field mismatch, not a product
  error. `sess_refs` is also not exported.

## 5. Kernel / restart / feeder

- No `BUG:`, `Oops`, `general protection fault`, `WARNING:`, or `UBSAN:` in
  either kernel window (near 35,361 B / far 1,030,390 B); the harness's
  patched kernel check reports PASS.
- near boot_id unchanged; feeder held `/dev/watchdog0` before, during and
  after the run.
- No external module load/unload during the run; the only harness was
  pid 988649 and its children.

## 6. Consequences

- 1 GiB acceptance: **FAIL** (size/sha incomplete; `fec_reset_user=1`;
  `fec_replay_miss_get` non-zero; `dead_now` field missing).
- DKMS seven-step flow: **not run** (PASS precondition not met).
- No retry (single-shot rule). No tag/push.
- Evidence kept: this directory plus near `/root/dl-1g`
  (1,069,764,217 B partial file and its `evidence/`), far
  `/root/accept-1g` (1 GiB source and httpd), far's 256 config.
- Next (not executed): remove the source-server 60 s write timeout from the
  acceptance path (use a sender without `SO_SNDTIMEO`, or a raw file
  sender), and/or make the far TX path avoid >60 s target-read stalls
  (data-over-replay priority, larger replay queue, or a backpressure bound
  below the source timeout). Then rerun 1 GiB and only on PASS run DKMS.
