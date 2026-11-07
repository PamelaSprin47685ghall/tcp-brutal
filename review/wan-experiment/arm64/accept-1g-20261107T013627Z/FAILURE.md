# 1 GiB acceptance run — FAIL

- run: 2026-11-07T01:36:27Z .. 01:39:31Z (transfer 01:37:15Z .. 01:39:31Z)
- near: root@192.168.123.65 (aarch64, 6.1.172), module 0.2.0 srcversion E8A17366F0F61FDC30C3C70,
  sha256 2b15d2cadb4e8ce306f8de4e726994b37256829c0ade19c0ee2b28c698ddd982
- far: root@45.11.1.62 (x86_64, 7.0.0-42), module 0.2.0 srcversion 72411330E2541E41EBD374E
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, nonce 00112233..eeff
- harness: review/wan-experiment/arm64/prod-1g/run-1g.sh (exit 1)

## Verdict: FAIL

| item | observed | expected |
|---|---|---|
| curl rc | 28 (Operation too slow) | 0 |
| http | 200 | 200 |
| size_download | 57671482 | 1073741824 |
| local file size | 57671482 | 1073741824 |
| near sha256 | 17ecbda3f6b2ca70f6cf3776c5032d412f540b072b8f6894a1ed2afa7ee41d1f | c6a25615.. |
| far sha256 | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 | c6a25615.. |
| near fec_payload_crc_fail | +145 | 0 |
| near rx_errs / connect_fail / alloc_fail / join_bad / gaps_total / reorder_overflow | 0 / 0 / 0 / 0 / 0 / 0 | 0 |
| far (all error counters) | 0 | 0 |
| kernel window (near, far) | no BUG/Oops/WARNING/GPF | clean |
| near boot_id | unchanged (0a91d2eb..) | unchanged |
| watchdog feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |

Last log line of the transfer:
```
curl: (28) Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds
```
`NEAR_FETCH_RC=124` in run.log is the harness's own ssh timeout, not the near-side verdict.

## Finding 1 — silent data loss (critical)

The 57,671,482 bytes curl received are NOT a prefix of the source. The first
18,956,090 bytes are byte-identical; from there the stream is missing source
bytes:

- near[18,956,090] == far[18,964,282] -> 8,192 bytes absent
- shift stays 8,192 through near offset 22,024,192, then becomes 24,576
- 24,576 bytes absent for the rest of the delivered prefix (probes up to 56,627,200)

Three 8,192-byte deletions (TBT_FEC_S-sized share payloads), all between
~18.96 MB and ~23.07 MB. No counter reports a gap: gaps_total=0, rx_errs=0,
holes_*=0, reorder_overflow=0, ooo_bytes=0, ooo_frames=0. The stream advanced
as if the bytes had been delivered.

Evidence: `first-divergence.txt`, `data-loss-shift-map.txt`,
`near-chunks.txt`, `far-chunks-all.txt`, `near-partial-sha256.txt`,
`far-prefix-sha256.txt`.

## Finding 2 — hard stall at 57,671,680 delivered bytes

- near `rx_delivered=57671680` == far `tx_ack=57671680`; delivery never advanced.
- far `submit_bytes=66256896` -> 8.58 MB unacked, parked in the 8 MB tx window
  (`tx_window=8388608`, `txq_max=8454432`).
- near `fec_payload_crc_fail=145` is the only error counter that moved.
- far `fec_credit_wait=3761`, `una_stall_us=4859970`; near `una_stall_us=1210115`.
- No NACK/retransmission exists (B-01) and delivery is exact-seq (B-07), so one
  unrecoverable group stops the stream for good; curl then trips its 120 s
  64 KiB/s limit.

Timing [INFERENCE from curl's 120 s speed window]: 57.67 MB delivered in the
first ~14 s (~33 Mbit/s burst), then nothing for the remaining 120 s — that is
why the speed limit fired at t=134 s. Wire counters are consistent: 133 MB
written by far over 134 s, 57.67 MB acked, 8.58 MB parked in the tx window.

## Finding 3 — harness defects (did not cause the data loss, did hide the verdict)

1. `ssh_near()` wraps every ssh in `timeout $SSH_TMO` (20 s). The transfer step
   uses it, so the ssh client is killed at 20 s while near-fetch.sh keeps
   running: `NEAR_FETCH_RC=124` is `timeout`, and near-fetch's own
   `NEAR_RESULT`/`near-verdict.txt` were never collected.
2. `near_busy()` prints without a trailing newline; `read -r nb ni < <(...)`
   returns 1 at EOF and `set -e` aborts run-1g.sh before `perf.txt`,
   `SUMMARY.md` and `cleanup_all`. That is why the tunnel was left up.
3. `journalctl -k --since` returned `-- No entries --` on both hosts, so the
   kernel-window check was vacuous; only the dmesg tail (800 lines) actually
   covered the window.

## Performance baseline (partial, recorded not judged)

- elapsed 134.13 s, average 429,954 B/s (3.44 Mbit/s) over the delivered prefix
- burst phase ~33 Mbit/s (57.67 MB in ~14 s), then zero delivery
- near CPU busy: see run.log/perf (not written — harness aborted before perf)
- far wire 133,231,038 B for 66,256,896 B submitted (2x FEC), ~8 Mbit/s over 134 s
- near RTT mean 2.65 ms / max 5.1 ms; far RTT mean 284 ms / max 782 ms, rttvar 3.9 ms
- near MemAvailable 7,583,604 kB; far MemAvailable 595,504 kB
- slab `tbt_*`: not exported by this build (no lines)

## CPU / memory (recomputed from procstat pre/post, same formula as run-1g.sh)

- near CPU busy: 3.6% (3921 busy / 109154 total jiffies, near-procstat-pre/post.txt)
- far CPU busy: 6.4% (1740 busy / 27131 total jiffies, far-procstat-pre/post.txt)
- near loadavg after: 0.27 0.22 0.14
- near MemTotal 8,104,768 kB, MemAvailable 7,583,604 kB
- far MemTotal 978,880 kB, MemAvailable 595,504 kB
- slab: `/proc/slabinfo` has no `tbt_carrier|tbt_session|tbt_stream` lines in this build (slab objects are kmalloc-backed)
