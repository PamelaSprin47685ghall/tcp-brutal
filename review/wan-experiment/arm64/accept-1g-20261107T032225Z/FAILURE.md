# 1 GiB acceptance run #5 (fix5, direct eth0) — FAIL

- run: 2026-11-07T03:22:25Z .. 03:32:51Z (transfer 03:23:08Z .. 03:32:40Z)
- near: root@192.168.123.65 (aarch64, 6.1.172), fix5 srcversion 9043190A7F8C199B8C6D74D,
  sha256 690c40468ed8c2c594f4d2f1b4ab23069a2d542bb9c094f0f4abb7fa7a7a5986
- far: root@45.11.1.62 (x86_64, 7.0.0-42), fix5 srcversion 0498A16AD1CD774A172B2D2
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, dev=eth0, direct path
- harness exit 5

## Verdict: FAIL — the stream was killed at 136 MiB

| item | observed | expected |
|---|---|---|
| curl rc | 18 (`end of response with 931135686 bytes missing`) | 0 |
| http | 200 | 200 |
| size_download | 142606138 | 1073741824 |
| local file size | 142606138 | 1073741824 |
| near sha256 | 3850cebcce2c9cd1d6d4b236c53f2d5f34ca642685ce6daf1f73de9f37f58a13 | c6a25615.. |
| far sha256, first 142606138 B | 3850cebcce2c9cd1d6d4b236c53f2d5f34ca642685ce6daf1f73de9f37f58a13 | equal to near |
| near boot_id | unchanged 0a91d2eb.. | unchanged |
| feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |
| kernel | both windows non-empty, no BUG/Oops/WARNING | clean |

**Data integrity: PASS for the delivered prefix.** The 142,606,138 bytes are a
byte-exact prefix of the source (same sha256 on both ends). fix5 removed the
round 1/2/4 corruption.

## Finding 1 — the far carrier stall timer killed healthy carriers

| counter | near | far |
|---|---|---|
| `lost` / `replaced` | 6 / 6 | 4 / 0 |
| `stall_declared` | 0 | **4** |
| `una_stall_us` | 9,673,267 | 7,670,435 |
| `rtt_mean_us` / `rttvar_us` / `rtt_max_us` | 316,084 / 1,325,389 / 4,874,656 | 315,363 / 2,848,336 / 653,186 |
| `rto_us` | 500,000 | 500,000 |

far `stall_declared` (4) equals far `lost` (4): `tbt_car_poll_one()` marks a
carrier lost when `una != nxt`, the window is open, and no ACK moves for
`carrier_stall_ms` (the config sets 15000 ms). With RTTVAR 1.3-2.8 s and RTT max
4.9 s one loss episode backs the RTO off 0.5 -> 1 -> 2 -> 4 -> 8 -> 16 s, so 15 s
sits inside the backoff. near's 6 losses have `stall_declared=0`: they are the
peer closing the socket after far declared it dead.

## Finding 2 — the lost carriers cost an unrecoverable block

far `c6 holes_unrecoverable=1`; the 30 s bounded reset then killed the stream
(rc=18, not the 1800 s max-time). With no NACK (B-01) and exact-seq delivery
(B-07) the block is permanent.

## Finding 3 — the direct path is slow: 2.01 Mbit/s, 12-18% loss

- transfer 142,606,138 B in 567.95 s -> 251,090 B/s = 2.01 Mbit/s
- ping far: 18% loss with the tunnel up, 12% with it down; rtt 297-300 ms
- ping 1.1.1.1: 6.7% loss, rtt 271 ms
- iperf3 near->far 5 s upload: 5.03 Mbit/s sender / 2.59 Mbit/s receiver, 206 retransmits
- iperf3 far->near (-R) while the tunnel ran: 0.397 Mbit/s, 25 retransmits

1 GiB inside the 1800 s budget needs >= 4.8 Mbit/s sustained; this path does not
provide it. Per the current instruction, slow throughput is treated as an error,
not as a performance note: either the path must improve or the budget/acceptance
shape must change.

## Counters (recomputed)

- near c9: `fec_proto_fail=0 fec_hdr_crc_fail=0 fec_payload_crc_fail=0`
- near c10: every field 0 (`fec_deliver_ooo`, `fec_late_data`, `fec_deliver_dup`,
  `fec_dup_share`, `fec_tx_share_gap`, `fec_slot_past_cut`, `fec_epoch_conflict`,
  `fec_deliver_bad`, `fec_fin_bad`, `fec_ack_bad`, `fec_decode_nomem`,
  `fec_deliver_nomem`, `fec_empty_block`)
- far c10: every field 0 except `fec_decode_blocks=1`, `fec_decode_recovered=1`,
  `fec_late_share=4`, `fec_pad_share=31`
- far c9: `rx_errs` +1 (one record error); everything else 0
- c3 both ends: `connect_fail=0 alloc_fail=0 join_bad=0 reorder_overflow=0`,
  `gaps_total=0`; `data_tx=0 retx_bytes=0 dup_bytes_ppm=0`

## Performance baseline (partial, but slow is an error)

- 2.01 Mbit/s, 567.95 s for 136 MiB
- near CPU busy 1.5%, far CPU busy 4.7%
- near loadavg 0.20 0.13 0.14; MemAvailable 7,550,476 kB
- near `replaced=6`; no retransmission counters exist (no NACK)

## Harness defects (unchanged)

`near-fetch.sh` `delta_zero()` greps `^<name>\.` while the lines are
`c9.<name>.pre=...`, so the three c9 counters are reported as "missing" although
their deltas (0/0/0) are in the same log.

## What was not done

- DKMS not run (gated on a 1 GiB PASS). `.65` still has no dkms.
- No rerun. Evidence kept: near `/root/dl-1g` (142,606,138 B, byte-exact prefix),
  far `/root/accept-1g` (1 GiB source + httpd).
