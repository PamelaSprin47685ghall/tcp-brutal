# 1 GiB acceptance run #7 (fix7, direct eth0) — FAIL

- run: 2026-11-07T04:13:02Z .. 04:28:28Z (transfer 04:14:40Z .. 04:27:42Z, 777.33 s)
- near: root@192.168.123.65 (aarch64, 6.1.172), fix7 srcversion 7F05230FD86BE43BC6A541F,
  sha256 af28dd7d72deca0fa09c9a5dec46a9e78da55e9a44d18a46e746f6237782ec41
- far: root@45.11.1.62 (x86_64, 7.0.0-42), fix7 srcversion 0E5C37320004439E2DDF405
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, dev=eth0,
  carrier_stall_ms=0 (auto), TBT_FEC_CONV 0x1004
- harness exit 5

## Verdict: FAIL — stream reset at 200 MiB

| item | observed | expected |
|---|---|---|
| curl rc | 18 (`end of response with 864026822 bytes missing`) | 0 |
| http | 200 | 200 |
| size_download | 209715002 | 1073741824 |
| local file size | 209715002 | 1073741824 |
| near sha256 | 7baca3ce667206e9886d5d5eb69815d68e3b26dfd605d7c7306e5897ee804186 | c6a25615.. |
| far sha256, first 209715002 B | 7baca3ce667206e9886d5d5eb69815d68e3b26dfd605d7c7306e5897ee804186 | equal to near |
| near boot_id | unchanged 0a91d2eb.. | unchanged |
| feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |
| kernel | both windows non-empty, no BUG/Oops/WARNING | clean |

**Data integrity: PASS for the delivered prefix.** 209,715,002 bytes are a
byte-exact prefix of the source. fix7 keeps the correctness fix5 established.

## Finding 1 — no NACK ever left either end

| c11 counter | near | far |
|---|---|---|
| `fec_nack_tx` | **0** | **0** |
| `fec_nack_drop` | **19** | **174** |
| `fec_nack_rx` | 0 | 0 |
| `fec_nack_retx` / `fec_replay_hit` / `fec_replay_miss` | 0 / 0 / 0 | 0 / 0 / 0 |
| `fec_nack_repair_ns` | 30,034,181,601 (30.03 s) | 1,935,023,468 |
| `fec_reset_tcp_failed` | 0 | 0 |
| `fec_reset_user` | **1** | 0 |

`fec_nack_drop` is incremented only when `tbt_fec_send_nack()` returns an error
(`tbt_fec_nack_pack` -> `tbt_fec_make_record` -> `tbt_fec_out_push`, the last a
GFP_ATOMIC kmalloc that can return -ENOMEM, -ENOSPC only for replay). Every
attempt failed, so no repair request reached the sender: `fec_nack_tx=0`,
`fec_replay_hit=0`, `fec_nack_retx=0`.

Near waited the full 30 s repair window (`fec_nack_repair_ns` = 30.03 s) and
then the stream was reset (`fec_reset_user=1`), which curl saw as rc=18.

## Finding 2 — carriers churned 173 times in 777 s

| counter | near | far |
|---|---|---|
| `lost` / `replaced` | 173 / 173 | 174 / 0 |
| `up` at end | 246 | 241 |
| `connect_retry` | 150 | 0 |
| `stall_declared` | see c3 | see c3 |

With `carrier_stall_ms=0` the threshold is `5 x RTO` clamped to 1..60 s and a
paused carrier is replaced after `min(2 x stall, 300 s)` without wire progress.
On a path with RTT 300 ms, RTTVAR > 1 s and 12-18% loss that still fires often:
173 replacements in 777 s. Each replacement drops the shares in flight.

## Finding 3 — the stream reset once a hole outlived the repair window

far `c6 holes_unrecoverable=1`; near `c6` all 0. Because no NACK was sent, the
lost shares could not be replayed and the 30 s window expired.

## Counters (recomputed)

- near c9: all 0 (`fec_proto_fail`, `fec_hdr_crc_fail`, `fec_payload_crc_fail`)
- near c10: every error field 0; `fec_decode_blocks=223`, `fec_decode_recovered=137`
- far c10: every error field 0; `fec_decode_blocks=1`, `fec_decode_recovered=1`
- far c9 `rx_errs` +8; near `rx_errs` 0
- c3 both ends: `connect_fail` delta 0, `alloc_fail=0`, `join_bad=0`,
  `reorder_overflow=0`, `gaps_total=0`, `stream_reset_tcp_failed=0`

## Performance baseline

- 209,715,002 B in 777.33 s -> 269,789 B/s = 2.16 Mbit/s
- near CPU busy 3.9%, far CPU busy 10.5%
- near loadavg 0.57 0.62 0.39; MemAvailable 7,546,128 kB
- near `replaced=173`, `connect_retry=150`; no ARQ retx (`retx_*` all 0)

## Harness gaps

- The harness does not judge c11 at all; this report's c11 numbers are
  recomputed from `*-stats-{pre,post}.txt`.
- `delta_zero()` still greps `^<name>\.` against `c9.<name>.pre=...` lines and
  reports the three c9 counters as "missing".

## What was not done

- DKMS not run (gated on a 1 GiB PASS); `.65` still has no dkms.
- No rerun. Evidence kept: near `/root/dl-1g` (209,715,002 B byte-exact prefix),
  far `/root/accept-1g` (1 GiB source + httpd).
