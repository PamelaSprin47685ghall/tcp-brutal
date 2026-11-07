# 1 GiB acceptance run

- start: 2026-11-07T08:07:46Z
- end: 2026-11-07T08:20:18Z
- near: `root@192.168.123.65` (aarch64, 6.1.172)
- far: `root@45.11.1.62`
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal
- evidence: `/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z`

## Verdict

**FAIL**

| check | value |
|---|---|
| curl http | 200 |
| size_download | 104711916 |
| file size | 104711916 |
| near sha256 | fe57db749cd4826aeb69cf8ef33379134d36010b395c8839a332fdd0304ffe05 |
| far sha256 | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 |
| elapsed | 746.833054 s |
| throughput | 1.12 Mbit/s |
| near CPU busy | 8.1% |
| far CPU busy | 55.0% |
| near boot_id stable | yes |
| feeder after run | pid=23184(python3) |
| near kernel window | PASS (source=journalctl, lines=72) |
| far kernel window | PASS (source=journalctl, lines=1580) |
| near bypass preflight | PASS (dev=eth0, rule=added) |
| near bypass pre-transfer | PASS (dev=eth0, rule=added) |

## Near path (dev= primary, ip rule insurance)

- near dev= (primary): eth0
- ip rule (insurance): `ip rule add to 45.11.1.62/32 lookup main pref 100`
- run-1g preflight: PASS (dev=eth0, rule=added)
- run-1g pre-transfer: PASS (dev=eth0, rule=added)

```
=== near-ip-rule-pre.txt ===
BYPASS_RULE_ADDED=1
BYPASS_DEV=eth0
=== ip rule show ===
0:	from all lookup local
100:	from all to 45.11.1.62 lookup main
9000:	from all to 198.18.0.0/30 lookup 2022
9001:	not from all dport 53 lookup main suppress_prefixlength 0
9001:	from all iif Meta goto 9010
9002:	not from all iif lo lookup 2022
9002:	from 0.0.0.0 iif lo lookup 2022
9002:	from 198.18.0.0/30 iif lo lookup 2022
9010:	from all nop
32766:	from all lookup main
32767:	from all lookup default
=== ip route get 45.11.1.62 ===
45.11.1.62 via 192.168.123.254 dev eth0 src 192.168.123.65 uid 0 
    cache 
=== near-ip-rule-pretransfer.txt ===
BYPASS_RULE_ADDED=1
BYPASS_DEV=eth0
=== ip rule show ===
0:	from all lookup local
100:	from all to 45.11.1.62 lookup main
9000:	from all to 198.18.0.0/30 lookup 2022
9001:	not from all dport 53 lookup main suppress_prefixlength 0
9001:	from all iif Meta goto 9010
9002:	not from all iif lo lookup 2022
9002:	from 0.0.0.0 iif lo lookup 2022
9002:	from 198.18.0.0/30 iif lo lookup 2022
9010:	from all nop
32766:	from all lookup main
32767:	from all lookup default
=== ip route get 45.11.1.62 ===
45.11.1.62 via 192.168.123.254 dev eth0 src 192.168.123.65 uid 0 
    cache 
=== near-evidence/near-ip-rule-pretransfer.txt ===
=== tag ===
pretransfer
=== ip rule show ===
0:	from all lookup local
100:	from all to 45.11.1.62 lookup main
9000:	from all to 198.18.0.0/30 lookup 2022
9001:	not from all dport 53 lookup main suppress_prefixlength 0
9001:	from all iif Meta goto 9010
9002:	not from all iif lo lookup 2022
9002:	from 0.0.0.0 iif lo lookup 2022
9002:	from 198.18.0.0/30 iif lo lookup 2022
9010:	from all nop
32766:	from all lookup main
32767:	from all lookup default
=== ip route get 45.11.1.62 ===
45.11.1.62 via 192.168.123.254 dev eth0 src 192.168.123.65 uid 0 
    cache 
=== dev ===
eth0
```

## Counter increments

```
near:
fec_proto_fail.0=0
fec_hdr_crc_fail.0=0
fec_payload_crc_fail.0=0
rx_errs=93
connect_fail=1
alloc_fail=0
join_bad=0
fec_nack_tx=16441
fec_nack_rx=6335
fec_nack_retx=21
fec_nack_drop=0
fec_replay_hit=21
fec_replay_miss=202624
fec_replay_miss_no_ring=0
fec_replay_miss_alloc=0
fec_replay_miss_get=202624
fec_nack_repair_ns=160919504375
fec_reset_tcp_failed=0
fec_reset_user=0
fec_nack_repair_max_ns_post=16599911071
gaps_total=0
dead_now=
up_post=251
n_post=256
replaced_post=102
lost_post=9
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
fec_nack_tx=6469
fec_nack_rx=16425
fec_nack_retx=323667
fec_nack_drop=0
fec_replay_hit=1145275
fec_replay_miss=959714
fec_replay_miss_no_ring=0
fec_replay_miss_alloc=0
fec_replay_miss_get=87548
fec_nack_repair_ns=223993246
fec_reset_tcp_failed=0
fec_reset_user=0
fec_nack_repair_max_ns_post=223993246
gaps_total=0
dead_now=
up_post=248
n_post=256
replaced_post=0
lost_post=95
sess_refs=
data_tx=0
retx_bytes=0
dup_bytes_ppm=0
```

## Checks

```
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_ssh PASS (root@192.168.123.65)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_ssh PASS (root@45.11.1.62)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_feeder PASS (pid=23184(python3) holds /dev/watchdog0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_tools PASS (curl sha256sum awk ss stat ip)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_bypass PASS (dev=eth0 rule=added to=45.11.1.62/32 pref=100)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_tools PASS (busybox dd sha256sum curl ss awk)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_httpd PASS (busybox httpd)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_disk PASS (10850912KiB free under /root)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_port PASS (5210 already in use (reuse or stop the old server))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_loaded PASS (not loaded; will insmod /root/tbtun-0.2.0-fix19-20261107T074843Z/tbtun.ko)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_loaded PASS (already loaded)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_serve PASS (size=1073741824 sha=c6a256153f71328c...)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_size PASS (FAR_SIZE=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_srcversion PASS (loaded=FB5F31B87EB79BFCA33E34A ko=FB5F31B87EB79BFCA33E34A)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_max_carriers PASS (max_carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_srcversion PASS (loaded=C0346E98476A3A31EF512A4 ko=C0346E98476A3A31EF512A4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_max_carriers PASS (max_carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_config_write PASS (config accepted (dev=eth0))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_cfg_carrier_stall PASS (carrier_stall_ms=0 (auto))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_carrier_stall PASS (carrier_stall_ms=0 (auto))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_cfg_dev PASS (dev=eth0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_join PASS (up=256 after 5s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_join PASS (up=256 after 5s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_bypass_pretransfer PASS (dev=eth0 rule=added)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_kernel_source PASS (source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_kernel_source PASS (source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK http PASS (http=200)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK size_download FAIL (size_download=104711916 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fetch_rc FAIL (rc=7)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK file_size FAIL (file=104711916 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK sha256 FAIL (near=fe57db749cd4826a... far=c6a256153f71328c...)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_proto_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_hdr_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_payload_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_rx_errs FAIL (delta=93)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_connect_fail FAIL (delta=1)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_alloc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_join_bad PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_nack_drop PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_replay_miss_no_ring PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_replay_miss_alloc PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_replay_miss_get FAIL (delta=202624)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_reset_tcp_failed PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_fec_reset_user PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_gaps_total PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_proto_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_hdr_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_payload_crc_fail.0 PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_rx_errs PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_connect_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_alloc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_join_bad PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_nack_drop PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_replay_miss_no_ring PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_replay_miss_alloc PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_replay_miss_get FAIL (delta=87548)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_reset_tcp_failed PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_fec_reset_user PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_gaps_total PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_nack_repair_rtt FAIL (repair_max=16599912us rtt_max=1879743us bound=15037944us (8x))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_nack_repair_rtt PASS (repair_max=223994us rtt_max=382684us bound=3061472us (8x))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK dead_now_bound FAIL (near dead_now=? max=64)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK sess_refs SKIP (not exported by this build)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_kernel PASS (window 2026-11-07T08:07:46Z..2026-11-07T08:20:18Z source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_kernel PASS (window 2026-11-07T08:07:46Z..2026-11-07T08:20:18Z source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_dmesg_tail PASS (no BUG/Oops/GPF in last 800 lines)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK far_dmesg_tail PASS (no BUG/Oops/GPF in last 800 lines)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK near_no_restart PASS (boot_id 0a91d2eb.. -> 0a91d2eb..)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/run.log:CHECK feeder_alive PASS (pid=23184(python3))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_curl PASS (curl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_sha256sum PASS (sha256sum)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_awk PASS (awk)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_stat PASS (stat)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_ss PASS (ss)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK tool_ip PASS (ip)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK bypass_route PASS (dev=eth0 rule=added to=45.11.1.62/32 pref=100)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK procfs PASS (/proc/net/tbtun/config)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_carriers PASS (carriers=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_n PASS (n=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_m PASS (m=4)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_fec PASS (fec=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_mode PASS (mode=grouped)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_w PASS (w=8)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_crypto PASS (crypto=on)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_profile PASS (profile=http-kal)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_portblock PASS (portblock=30000-30511)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_carrier_stall PASS (carrier_stall_ms=0 (auto))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK cfg_dev PASS (dev=eth0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK listen PASS (127.0.0.1:9001)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK feeder PASS (pid=23184(python3) holds /dev/watchdog0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK join_up PASS (up=256 expected=256 after 0s)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK join_n PASS (n=256 expected=256)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK bypass_route_pretransfer PASS (dev=eth0 rule=added)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK curl_rc FAIL (rc=28 (28=max-time or stall, 18=partial, 56=recv failure))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK http PASS (http=200)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK size_download FAIL (size_download=104711916 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK file_size FAIL (file=104711916 expected=1073741824)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK sha256 FAIL (near=fe57db749cd4826a... far=c6a256153f71328c...)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_proto_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_hdr_crc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_payload_crc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_rx_errs FAIL (delta=93)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_connect_fail FAIL (delta=1)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_alloc_fail PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_join_bad PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_gaps_total PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_nack_drop PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_replay_miss_no_ring PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_replay_miss_alloc PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_replay_miss_get FAIL (delta=202464)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_reset_tcp_failed PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK counter_fec_reset_user PASS (delta=0)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK nack_repair_rtt FAIL (repair_max=16599912us rtt_max=1879743us bound=15037944us (8x))
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK sess_refs SKIP (field not exported by this build)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK dead_now SKIP (c3 dead_now missing)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK kernel_window_source PASS (source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK kernel_clean PASS (window 2026-11-07T08:07:50Z..2026-11-07T08:20:17Z source=journalctl)
/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T080653Z/near-fetch.log:NEAR_CHECK no_restart PASS (boot_id 0a91d2eb.. -> 0a91d2eb..)
```

## Cleanup

skipped (--keep); module, config and files stay for forensics
