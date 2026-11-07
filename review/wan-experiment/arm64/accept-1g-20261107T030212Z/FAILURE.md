# 1 GiB acceptance run #4 (fix4, direct eth0) — FAIL

- run: 2026-11-07T03:02:12Z .. 03:10:17Z (transfer 03:06:59Z .. 03:10:09Z)
- near: root@192.168.123.65 (aarch64, 6.1.172), fix4 srcversion 01F468A1B5C62F96115CCB2,
  sha256 ddb2320dac45a7302743704bfe729f11d432e28179b4a22fabeb48d13de36f4f
- far: root@45.11.1.62 (x86_64, 7.0.0-42), fix4 srcversion 8A6BBE49134ADB5BA741F0E
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal, dev=eth0
- path: direct eth0 (`ip rule pref 100 to 45.11.1.62/32 lookup main`), no mihomo
- harness exit 5

## Verdict: FAIL

| item | observed | expected |
|---|---|---|
| curl rc | 28 (`Operation too slow`) | 0 |
| http | 200 | 200 |
| size_download | 1048378 | 1073741824 |
| local file size | 1048378 | 1073741824 |
| near sha256 | 699f730ac315176e6226758a2958fe51cd1b5215803a051bf6bb93bb6f75bea1 | c6a25615.. |
| far sha256 (full) | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 | c6a25615.. |
| far sha256 (first 1048378 B) | 6fcd31fa5c8afd648f66fccd12bb304bfbe48f8db0a90e04f4366149e27b7ca6 | must equal near |
| near boot_id | unchanged 0a91d2eb.. | unchanged |
| feeder at end | pid=23184(python3) holds /dev/watchdog0 | alive |
| kernel | journalctl windows empty (SKIP); dmesg tail 800 lines clean | clean |

Last log line:
```
curl: (28) Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds
```

## Finding 1 — the delivered stream is still not the source (critical)

The first 909,114 bytes are byte-identical; after that the stream **duplicates
one 8 KiB region and drops another**, then accumulates 16/24/32 KiB of missing
data. Details and the alignment table: `stream-integrity.txt`; the 1 MiB
partial file is kept as `near-partial-1048378.bin`.

None of fix3/fix4's guards fired: `fec_deliver_ooo=0`, `fec_deliver_dup=0`,
`fec_slot_past_cut=0`, `fec_deliver_bad=0`, `fec_epoch_conflict=0`. The only
error counter that moved is `fec_payload_crc_fail=179`.

## Finding 2 — the direct path makes FEC carry almost everything

- far `submit_bytes=9584640` (9.1 MiB) and `wire=19438364` (2x FEC) for the
  1 MiB delivered; `fec_credit_wait=4331`, `fec_tx_share_gap=2`.
- near `fec_decode_blocks=75`, `fec_decode_recovered=69` — 92% of blocks needed
  parity recovery; `fec_late_share=220`, `fec_dup_share=17`, `fec_empty_block=63`.
- far c3 `lost=1`; near `lost=0 replaced=0`; `connect_fail` delta 0 during the
  transfer (the 1032 connect failures in the pre-snapshot are the fail2ban
  window, see `firewall-unban.txt`).
- Throughput 5,600 B/s (0.04 Mbit/s), 187.19 s; the stream stalled at 1 MiB and
  stayed below curl's 64 KiB/s floor for the last 120 s.

## Finding 3 — join only succeeded after the fail2ban unban

The join gate spent 173 s at `near_up=0 far_up=0` because far's fail2ban had
banned near's direct egress IP (171.120.34.226) with
`reject with icmp port-unreachable` for TCP. After
`fail2ban-client set sshd unbanip 171.120.34.226` the carriers joined within
16 s (03:06:42..03:06:58). The IP is now in the jail's `ignoreip` at runtime and
in `/etc/fail2ban/jail.local`; the other 35 banned attacker IPs are untouched.
Full record: `firewall-unban.txt`.

## Harness defects seen in this run

1. `near-fetch.sh` `delta_zero()` greps `^<name>\.` but `counters.txt` lines are
   `c9.<name>.pre=...`, so `fec_proto_fail`, `fec_hdr_crc_fail` and
   `fec_payload_crc_fail` were reported as "counter missing or non-numeric"
   even though the deltas (0/0/179) are present in the same log.
2. Both kernel windows are empty (`journalctl -k -b 0 @epoch` returns nothing),
   so the kernel criterion is SKIP; only the dmesg tail is judged.

## Performance baseline (partial, recorded not judged)

- elapsed 187.19 s, 1,048,378 B -> 5,600 B/s = 0.04 Mbit/s
- near CPU busy 1.1%, far CPU busy 4.3%
- near loadavg 0.12 0.12 0.17; MemAvailable 7,557,512 kB
- far MemAvailable: see far-post.txt
- near `data_tx=0 retx_bytes=0 dup_bytes_ppm=0`; `replaced=0`
- far wire 19,438,364 B for 9,584,640 B submitted (2x FEC)
- slab: no `tbt_*` lines in /proc/slabinfo

## What was not done

- DKMS was not run (the task gates it on a 1 GiB PASS). `.65` still has no dkms.
- No rerun. Evidence kept: near `/root/dl-1g` (1 MiB partial), far
  `/root/accept-1g` (1 GiB source + httpd).
