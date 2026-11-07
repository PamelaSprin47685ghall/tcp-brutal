# 1 GiB acceptance run #2 — FAIL

- run: 2026-11-07T02:08:29Z .. 02:15:00Z (transfer 02:09:13Z .. 02:14:33Z, 317.80 s)
- near: root@192.168.123.65 (aarch64, 6.1.172), module 0.2.0 fix2
  srcversion 86A8FDA3185B4E25E81D441, sha256 c55334fa508f4ebd61d306a97dc0f542a95a34fd3f64ef3f0b1c3e7ffe0344e2
- far: root@45.11.1.62 (x86_64, 7.0.0-42), module 0.2.0 fix2 srcversion D23D3E58CFC8C12B3B72AB7
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, TBT_FEC_CONV 0x1003
- harness: review/wan-experiment/arm64/prod-1g/run-1g.sh, exit 5

## Verdict: FAIL

| item | observed | expected |
|---|---|---|
| curl rc | 18 (`end of response with 1007681734 bytes missing`) | 0 |
| http | 200 | 200 |
| size_download | 66060090 | 1073741824 |
| local file size | 66060090 | 1073741824 |
| near sha256 | bd5ad5d6b2460938b3363f6800d73cb3470b87ec82391868fa82597bb8492a2c | c6a25615.. |
| far sha256 (full) | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 | c6a25615.. |
| far sha256 (first 66060090 B) | 7f7cdaff3c065beaf672549cb7a83648748efca47abc1b2f481d2147cd62704d | must equal near |
| near boot_id | unchanged 0a91d2eb.. | unchanged |
| watchdog feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |

Last lines:
```
NEAR_CURL_RC=18
   curl: http=200 size=66060090 time=317.797092 speed=207868
   curl stderr: curl: (18) end of response with 1007681734 bytes missing
```

## Finding 1 — delivered bytes are still not the source prefix (critical)

`sha256(far[0:66060090]) = 7f7cdaff..` != `sha256(near file) = bd5ad5d6..`.
So the 66,060,090 bytes curl received are not the first 66,060,090 source
bytes. The fix's specific counter for the round-1 deletion,
`fec_slot_past_cut`, stayed 0 on both ends, so this is not the same branch —
or the divergence sits at the teardown tail.

Divergence point unknown: run-1g.sh ran `cleanup_all` despite FAIL and deleted
`near:/root/dl-1g` before the partial file could be copied. The only surviving
near artifact is the hash (`near-sha256.txt`). Harness defect, see Finding 4.

## Finding 2 — stream torn down after 66,060,090 bytes

- near `rx_delivered=66060288` (198 B of HTTP response headers + body); the
  stream is gone at the post snapshot (`streams_active=0 n_streams=0`).
- far `c6 holes_unrecoverable=1`; the 30 s bounded reset (commit ae1d49a) is
  the only path that ends a stream this way.
- curl saw a *complete but short* HTTP response, i.e. a FIN from the near-side
  listener, not a stall and not a timeout.
- far `submit_bytes=74694656`, `wire=150476945` (2x FEC), `fec_credit_wait=7411`;
  near `rx_read_bytes=150476831`, `fec_payload_crc_fail=16`.

## Finding 3 — carrier 250 lost, then replaced

```
near [11721.191997] tbtun: tunnel 1 carrier 250 gen 251 lost: sock=1 connected=1 hello_sent=1 up=1
far  [67751.369639] tbtun: tunnel 1 carrier 250 gen 247 lost: sock=1 connected=1 hello_sent=1 up=1
far  [67752.347871] tbtun: tunnel 1 carrier ACCEPTED lport=9000 rport=38530, awaiting HELLO
```
near c3 `lost=1 replaced=1`; far c3 `lost=1 replaced=0`.

Timing [INFERENCE]: module load was uptime 11386 (~02:09:05), so 11721 is
~02:14:40 — about 7 s after curl had already failed. The carrier loss is more
likely a consequence of the teardown than its trigger.

## Finding 4 — harness defects (again)

1. `near-fetch.sh` is `#!/bin/sh` but uses bash substring expansion
   `${got_hash:0:16}` / `${FAR_HASH:0:16}` (line 303/304). Under dash:
   `/root/near-fetch.sh: 303: Bad substitution` -> the script dies right after
   the sha256, so the near-side counters, kernel window and verdict were never
   produced (`near-verdict.txt` missing, `near-fetch.log` truncated).
2. `run-1g.sh` runs `cleanup_all` on FAIL (KEEP=0), deleting the near partial
   file that is the primary forensic artifact. On FAIL it must keep it.
3. near kernel window is empty again (`SKIP`); only the dmesg tail covered it.

## c10 counters (recomputed from *-stats-pre/post)

Hard errors — all zero on both ends: `fec_decode_nomem`, `fec_deliver_nomem`,
`fec_deliver_dup`, `fec_deliver_bad`, `fec_epoch_conflict`, `fec_slot_past_cut`,
`fec_fin_bad`, `fec_ack_bad`, `fec_tx_share_gap`.

Non-zero:
| counter | near | far |
|---|---|---|
| `fec_payload_crc_fail` (c9) | +16 | 0 |
| `fec_late_share` | +1943 | +1 |
| `fec_empty_block` | +31 | 0 |
| `fec_pad_share` | 0 | +31 |
| `fec_decode_blocks` | +8 | 0 |
| `fec_decode_recovered` | +6 | 0 |

c3: both ends `connect_fail=0 alloc_fail=0 join_bad=0 reorder_overflow=0`;
`gaps_total=0`; `rx_errs=0`.

## Performance baseline (partial)

- elapsed 317.80 s, 66,060,090 B -> 207,868 B/s = 1.66 Mbit/s
- near CPU busy 1.2%, far CPU busy 3.9%
- near loadavg 0.32 0.45 0.26; MemAvailable 7,579,104 kB
- far MemTotal 978,880 kB, MemAvailable 593,380 kB
- near `data_tx=0 retx_bytes=0 dup_bytes_ppm=0`; `replaced=1`
- far wire 150,476,945 B for 74,694,656 B submitted (2x FEC), 1.66 Mbit/s effective
- slab: no `tbt_*` lines in /proc/slabinfo
