# 1 GiB acceptance run #8 (fix8, direct eth0) — FAIL

- run: 2026-11-07T04:43:00Z .. 04:52:40Z (transfer 04:44:08Z .. 04:52:00Z, 468.05 s)
- near: root@192.168.123.65 (aarch64, 6.1.172), fix8 srcversion 0EA220DC9D3198ED49BEFBA,
  sha256 c124ec451275bef2d63f2cbc7d2c835e489a4cc921e31b2cc179b451070c45d0
- far: root@45.11.1.62 (x86_64, 7.0.0-42), fix8 srcversion 377B3DD6D984F53EF52F14B
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, dev=eth0,
  carrier_stall_ms=0 (auto), TBT_FEC_CONV 0x1004
- harness exit 5

## Verdict: FAIL — stream reset at 128 MiB

| item | observed | expected |
|---|---|---|
| curl rc | 18 (`end of response with 945815750 bytes missing`) | 0 |
| http | 200 | 200 |
| size_download | 127926074 | 1073741824 |
| local file size | 127926074 | 1073741824 |
| near sha256 | 5bd3dfaac8c3defb8687134f3d300811ce9428c2b9437c4a15545ac721fdc874 | c6a25615.. |
| far sha256, first 127926074 B | 5bd3dfaac8c3defb8687134f3d300811ce9428c2b9437c4a15545ac721fdc874 | equal to near |
| near boot_id | unchanged 0a91d2eb.. | unchanged |
| feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |
| kernel | both windows non-empty, no BUG/Oops/WARNING | clean |

**Data integrity: PASS for the delivered prefix** (byte-exact).

## The two fixes under test both work

- **NACK reaches the wire.** near `fec_nack_tx=57 rx=17 retx=71 drop=0`; far
  `fec_nack_tx=17 rx=57 retx=1080 replay_hit=3831 nack_retx_bytes=8890560`.
  Every error sub-counter is 0: `fec_nack_enomem`, `fec_nack_einval`,
  `fec_nack_enospc`, `fec_nack_eother`, `fec_nack_bad`. Repair latency is
  sub-millisecond on the near side (`fec_nack_lat_ns=430,505`) and 1.52 s on
  the far side.
- **Carrier replacement is damped.** near `lost=5 replaced=5` (round 7: 173),
  far `lost=5`, `up=256` on both ends. dmesg shows pause/recover pairs
  (`carrier 137 stalled ... recovered after 323 ms`,
  `carrier 3 recovered after 2833 ms`), i.e. the pause path works.

## Finding — the replay ring misses, so the hole cannot be repaired

| c11 counter | near | far |
|---|---|---|
| `fec_replay_hit` | 71 | 3831 |
| `fec_replay_miss` | **497** | **3354** |
| `fec_nack_repair_ns` | **30,543,569,083 (30.5 s)** | 22,127,109 (22 ms) |
| `fec_reset_user` | **1** | 0 |
| `fec_reset_tcp_failed` | 0 | 0 |

`fec_replay_miss` is incremented when the sender has no replay ring, the
GFP_ATOMIC buffer fails, or `tbt_fec_replay_get()` no longer holds the
requested share. Roughly 47% of the requests (3354/7185) fall outside the
ring, so the hole at the delivery frontier cannot be filled. Near waits the
full 30.5 s repair window and then resets the stream (`fec_reset_user=1`),
which curl reports as rc=18. far `c6 holes_unrecoverable=1`; near `c6` all 0.

The ring is K=4 super-blocks capped at 9 MiB. At this shape one super-block is
32 groups x 8 shares x 8 KiB = 2 MiB, so K=4 is about 8 MiB — the same order as
the 8 MiB tx window. The sender keeps advancing while the receiver's frontier
is stuck, so the block the receiver needs is evicted exactly when it is asked
for. The ring has to be sized above the sender's window (or the sender must
stop advancing once an ACK-stalled frontier is older than the ring).

## Counters (recomputed)

- near c9: all 0; near c10: every error field 0
- far c9 `rx_errs` +1; far c10: every error field 0
- c3 both ends: `connect_fail` delta 0, `alloc_fail=0`, `join_bad=0`,
  `reorder_overflow=0`, `gaps_total=0`, `stream_reset_tcp_failed=0`;
  near `connect_retry=81`

## Performance baseline

- 127,926,074 B in 468.05 s -> 273,319 B/s = 2.19 Mbit/s
- near CPU busy 3.4%, far CPU busy 10.0%
- near loadavg 0.26 0.30 0.24; MemAvailable 7,541,872 kB
- repair amplification: far sent 8,890,560 B of replay for 127.9 MB delivered

## Harness gaps (unchanged)

- The harness does not judge c11; the numbers above are recomputed from
  `*-stats-{pre,post}.txt`.
- `delta_zero()` still greps `^<name>\.` against `c9.<name>.pre=...` lines and
  reports the three c9 counters as "missing".

## What was not done

- DKMS not run (gated on a 1 GiB PASS); `.65` still has no dkms.
- No rerun. Evidence kept: near `/root/dl-1g` (127,926,074 B byte-exact prefix),
  far `/root/accept-1g` (1 GiB source + httpd).
