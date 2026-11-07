# 1 GiB acceptance run — FAIL

- harness start: `2026-11-07T08:06:53Z`
- transfer: `2026-11-07T08:07:46Z` .. `2026-11-07T08:20:18Z` (746.833 s)
- near: `root@192.168.123.65` (aarch64, 6.1.172, fix19 `FB5F31B87EB79BFCA33E34A`,
  ko sha256 `107d1e8075e710db49e13c1aedc1dc0ed89a8ae4b65c6c0e01112eb32b42eac6`)
- far: `root@45.11.1.62` (fix19 `C0346E98476A3A31EF512A4`, debug=0, 256 config)
- shape: 256 carriers, n=4 m=4, fec=on, grouped, w=8, crypto=on, http-kal,
  near `dev=eth0`, pref-100 bypass, single curl, 7200 s cap,
  stall 120 s below 64 KiB/s
- verdict: **FAIL**

## 1. Transfer result

| item | value |
|---|---|
| curl rc | 28 (`Operation too slow ... last 120 seconds`) |
| http | 200 |
| size_download | 104,711,916 B (9.75% of 1,073,741,824) |
| local file | 104,711,916 B |
| near sha256 | `fe57db749cd4826aeb69cf8ef33379134d36010b395c8839a332fdd0304ffe05` |
| far sha256 | `c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93` |
| elapsed | 746.833054 s |
| average speed | 140,207 B/s = 1.12 Mbit/s |
| near CPU busy | 8.1% |
| far CPU busy | 55.0% |
| near MemAvailable | 7,533,608 kB |
| near loadavg | 0.54 0.60 0.42 |
| near boot_id | `0a91d2eb-3122-41eb-913c-024a4e1fe724` (unchanged) |
| feeder | `pid=23184(python3)` holds `/dev/watchdog0` |

The transfer started at ~13.5 Mbit/s (21.9 MB in the first 13 s) and decayed
to below 64 KiB/s for the last 120 s. curl never completed.

## 2. Kernel: UBSAN shift-out-of-bounds (new defect)

far kernel log, `2026-11-07T08:11:50Z`:

```
UBSAN: shift-out-of-bounds in tbtun/tbt_tx.c:2818:32
shift exponent 163 is too large for 64-bit type 'long unsigned int'
CPU: 1 ... Tainted: G        W  OE
Workqueue: events_unbound tbt_pump_work [tbtun]
 tbt_tx_pump.cold+0x301/0xa17 [tbtun]
 tbt_pump_work+0x60/0x80 [tbtun]
```

Source (`tbtun/tbt_tx.c`):

- `2516: u16 blocked = 0;`
- `2818: blocked |= BIT(c->index);`
- `2219: tbt_sched_pick(..., u16 blocked, ...)`
- `2243: if (blocked & BIT(idx))`

`c->index` ranges 0..255 (`TBT_MAX_CARRIERS_LIMIT 256`), but `blocked` is
`u16`. For index >= 64 the shift is undefined (UBSAN fired on index 163);
for index >= 16 the bit is truncated, so the scheduler's blocked mask only
ever describes carriers 0..15. `TBT_ARQ_MAX_PATHS` is
`TBT_LEGACY_MAX_CARRIERS` (16); the scheduler order covers all 256.

This is the only kernel splat in the window. There is no Oops, BUG, GPF or
`WARNING:` line; the harness's kernel check greps only
`BUG:|Oops|WARNING:|general protection fault`, so it reported
`near_kernel PASS` / `far_kernel PASS` and missed the UBSAN. The harness
patterns are being extended to include `UBSAN:`.

## 3. Counters (deltas across the transfer)

Full dump: `counters-full.txt`.

Required-zero fields:

| field | near | far |
|---|---|---|
| `c9.fec_stream_refused` | 0 | 0 |
| `c11.fec_replay_miss_evicted` | 0 | **85** |
| `c11.fec_reset_hole` | 0 | 0 |
| `c6.holes_unrecoverable` | 0 | 0 |
| `c11.fec_replay_miss_get` | **202,624** | **87,548** |
| `c11.fec_replay_miss_enospc` | 0 | **872,166** |
| `c11.fec_replay_miss_never_stored` | **202,624** | **87,463** |
| `c11.fec_nack_drop` | 0 | 0 |
| `c11.fec_reset_tcp_failed` / `fec_reset_user` | 0 / 0 | 0 / 0 |
| `c10` hard errors (`decode_nomem`, `deliver_nomem`, `deliver_bad`, `epoch_conflict`, `slot_past_cut`, `fin_bad`, `ack_bad`, `tx_share_gap`) | all 0 | all 0 |
| `c9.fec_proto_fail` / `hdr_crc_fail` / `payload_crc_fail` | 0 / 0 / 0 | 0 / 0 / 0 |

Non-zero repair traffic (not error counters, but the failure mechanism):

| field | near | far |
|---|---|---|
| `c11.fec_nack_tx` | 16,441 | 6,469 |
| `c11.fec_nack_rx` | 6,335 | 16,425 |
| `c11.fec_nack_retx` | 21 | 323,667 |
| `c11.fec_nack_retx_bytes` | 840 | 2,071,297,322 |
| `c11.fec_replay_hit` | 21 | 1,145,275 |
| `c11.fec_nack_repair_max_ns` | 16,599,911,071 (16.6 s) | 223,993,246 (0.224 s) |
| `c11.fec_nack_lat_max_ns` | 258,128 | 2,924,497,419 (2.92 s) |
| `c9.fec_short_tx` | 1 | 168 |
| `c9.fec_credit_wait` | 0 | 11,333 |
| `c10.fec_late_share` | 336,273 | 25 |
| `c10.fec_empty_block` | 31 | 4,913 |

Carrier and stream state:

| field | near | far |
|---|---|---|
| `c3.up` | 256 -> 251 | 256 -> 248 |
| `c3.lost` | 9 | 95 |
| `c3.replaced` | 102 | 0 |
| `c3.carrier_retired` | 102 | 102 |
| `c3.connect_fail` | +1 | 0 |
| `c3.connect_retry` | +42 | 0 |
| `c3.stall_declared` / `stall_recovered` | 10 / 9 | 780 / 778 |
| `c3.stall_dead` / `stall_replaced` | 0 / 1 | 1 / 0 |
| `c3.una_stall_us` | 545,163,185 (545 s) | 540,197,973 (540 s) |
| `c3.tx_window` | 4,998,280 -> 3,842,935 | 2,896,690 -> 5,292,570 |
| `c3.bdp_bytes` | 992,496 -> 722,986 | 579,318 -> 986,973 |
| `c3.rtt_max_us` | 1,879,743 | 382,684 |
| `rx.rx_errs` / `rx.rx_proto` / `rx.rx_last_err` | 93 / 93 / -71 (EPROTO) | 0 / 0 / 0 |
| `c8.http_req` / `c8.http_resp` | +102 / 0 | 0 / +95 |
| `c12.fec_outq` at post | empty | empty |

`rx_proto` is incremented by `tbt_car_rx_error()` for a non-EALREADY receive
error (`tbtun/tbt_carrier.c:848-858`), and that path marks the carrier dead.
93 protocol errors therefore retired near carriers; `c3.replaced=102`.

## 4. Amplification and repair storm

- far wire: 2,292,002,003 B for 104,712,114 B delivered = **21.9x**.
- far retransmit: 2,071,297,322 B / 323,667 frames = **19.8x** the delivered
  bytes.
- near read from carriers: 2,272,461,538 B for 104,712,114 B delivered.
- far replay misses: 959,714; `enospc` 872,166 (the replay ring was full for
  91% of the misses); `never_stored` 87,463; `get` 87,548; `evicted` 85.
- far `fec_hol_wait` +5,438,740; `fec_flush_skip` +956,167;
  `fec_nack_empty_reply` +13,379.
- near `fec_nack_repair_max_ns=16.6 s` exceeds the harness bound
  `8 * rtt_max = 15.04 s` (FAIL); far's 0.224 s passes.
- far CPU 55%, near CPU 8%; the far side spends most of the run replaying.

Interpretation (conditional): the run delivered only 104.7 MB because the
repair path could not keep up with the path loss: the replay ring filled
(`enospc`), NACK repair ran at 20x amplification, carrier churn (93 near RX
protocol errors, 95 far carrier losses) kept resetting the stripe, and the
stream's unacked window stalled for 545 s. The UBSAN at `tbt_tx.c:2818`
coincides with the start of the carrier storm (08:11:50) and is a concrete
256-carrier scheduler defect, but this run does not isolate its contribution
from the path loss; both are recorded, neither is claimed as the sole cause.

## 5. Non-findings / guards

- No `BUG:`, `Oops`, `general protection fault`, or `WARNING:` line in either
  kernel window. The UBSAN report is the only splat (see §2).
- near boot_id unchanged; no reboot.
- The hardware watchdog feeder held `/dev/watchdog0` before, during and after
  the run.
- No external module load/unload during the run: near dmesg has exactly one
  `tbtun 0.2.0 loaded` at `32888.406122` (this run's insmod) and no
  `tbtun: unloaded` until cleanup; far has no load/unload in the window.
- The earlier `final-run` harness (pid 987640) was killed at 08:06:31Z before
  this run started; its evidence directory
  `accept-1g-20261107T080442Z` is frozen (last write 08:05:54Z) and is not
  part of this verdict.

## 6. Consequences

- 1 GiB acceptance: **FAIL** (size/sha incomplete; `fec_replay_miss_evicted`
  non-zero; `fec_replay_miss_get/enospc` non-zero; near `rx_errs=93`;
  near `connect_fail` +1; repair max 16.6 s; UBSAN).
- DKMS seven-step flow: **not run** (PASS precondition not met).
- No retry (single-shot rule).
- Evidence kept: this directory plus near `/root/dl-1g` (104,711,916 B
  partial file and its `evidence/` subdirectory); far `/root/accept-1g`
  (1 GiB source and httpd) and far's 256 config stay in place.
- Near module/tunnel and the pref-100 ip rule were removed after the
  evidence snapshot; the partial file is kept for forensics.
