# 1 GiB acceptance run

- start: 2026-11-07T02:09:13Z
- end: 2026-11-07T02:14:33Z
- near: `root@192.168.123.65` (aarch64, 6.1.172)
- far: `root@45.11.1.62`
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal
- evidence: `/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z`

## Verdict

**FAIL**

| check | value |
|---|---|
| curl http | 200 |
| size_download | 66060090 |
| file size | 66060090 |
| near sha256 | bd5ad5d6b2460938b3363f6800d73cb3470b87ec82391868fa82597bb8492a2c |
| far sha256 | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 |
| elapsed | 317.797092 s |
| throughput | 1.66 Mbit/s |
| near CPU busy | 1.2% |
| far CPU busy | 3.9% |
| near boot_id stable | yes |
| feeder after run | pid=23184(python3) |

## Counter increments

```
near:
fec_proto_fail.0=0
fec_hdr_crc_fail.0=0
fec_payload_crc_fail.16=16
rx_errs=0
connect_fail=0
alloc_fail=0
join_bad=0
gaps_total=0
dead_now=
up_post=256
n_post=256
replaced_post=1
lost_post=1
sess_refs=
data_tx=0
retx_bytes=0
dup_bytes_ppm=0
far:
fec_proto_fail.0=0
fec_hdr_crc_fail.0=0
fec_payload_crc_fail.0=0
rx_errs=0
connect_fail=0
alloc_fail=0
join_bad=0
gaps_total=0
dead_now=
up_post=256
n_post=256
replaced_post=0
lost_post=1
sess_refs=
data_tx=0
retx_bytes=0
dup_bytes_ppm=0
```

## Checks

```
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_ssh PASS (root@192.168.123.65)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_ssh PASS (root@45.11.1.62)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_feeder PASS (pid=23184(python3) holds /dev/watchdog0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_tools PASS (curl sha256sum awk ss stat)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_tools PASS (busybox dd sha256sum curl ss awk)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_httpd PASS (busybox httpd)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_disk PASS (11676896KiB free under /root)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_port PASS (5210 already in use (reuse or stop the old server))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_loaded PASS (not loaded; will insmod /root/tbtun-0.2.0-fix2-20261107T020420Z/tbtun.ko)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_loaded PASS (already loaded)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_serve PASS (size=1073741824 sha=c6a256153f71328c...)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_size PASS (FAR_SIZE=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_srcversion PASS (loaded=86A8FDA3185B4E25E81D441 ko=86A8FDA3185B4E25E81D441)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_max_carriers PASS (max_carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_srcversion PASS (loaded=D23D3E58CFC8C12B3B72AB7 ko=D23D3E58CFC8C12B3B72AB7)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_max_carriers PASS (max_carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_join PASS (up=256 after 2s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_join PASS (up=256 after 2s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_kernel_source PASS (source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_kernel_source PASS (source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK http PASS (http=200)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK size_download FAIL (size_download=66060090 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_fetch_rc FAIL (rc=2)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK file_size FAIL (file=66060090 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK sha256 FAIL (near=bd5ad5d6b2460938... far=c6a256153f71328c...)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_fec_proto_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_fec_hdr_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_fec_payload_crc_fail.16 FAIL (delta=16)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_rx_errs PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_connect_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_alloc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_join_bad PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_gaps_total PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_fec_proto_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_fec_hdr_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_fec_payload_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_rx_errs PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_connect_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_alloc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_join_bad PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_gaps_total PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK dead_now_bound FAIL (near dead_now=? max=64)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK sess_refs SKIP (not exported by this build)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_kernel SKIP (empty window 2026-11-07T02:09:13Z..2026-11-07T02:14:33Z (source=journalctl); dmesg tail is still judged)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_kernel PASS (window 2026-11-07T02:09:13Z..2026-11-07T02:14:33Z source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_dmesg_tail PASS (no BUG/Oops/GPF in last 800 lines)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK far_dmesg_tail PASS (no BUG/Oops/GPF in last 800 lines)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK near_no_restart PASS (boot_id 0a91d2eb.. -> 0a91d2eb..)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/run.log:CHECK feeder_alive PASS (pid=23184(python3))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK tool_curl PASS (curl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK tool_sha256sum PASS (sha256sum)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK tool_awk PASS (awk)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK tool_stat PASS (stat)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK tool_ss PASS (ss)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK procfs PASS (/proc/net/tbtun/config)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK cfg_portblock PASS (portblock=30000-30511)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK listen PASS (127.0.0.1:9001)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK feeder PASS (pid=23184(python3) holds /dev/watchdog0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK join_up PASS (up=256 expected=256 after 0s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK join_n PASS (n=256 expected=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK curl_rc FAIL (rc=18 (28=max-time or stall, 18=partial, 56=recv failure))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK http PASS (http=200)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK size_download FAIL (size_download=66060090 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T020829Z/near-fetch.log:NEAR_CHECK file_size FAIL (file=66060090 expected=1073741824)
```

## Cleanup

see `cleanup.txt`
