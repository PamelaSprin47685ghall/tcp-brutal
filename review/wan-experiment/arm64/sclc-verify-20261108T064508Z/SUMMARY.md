# SC-LDPC 0.3.1 fix44 acceptance

- start: 2026-11-08T06:45:08Z
- end: 2026-11-08T07:01:19Z
- near: `root@192.168.123.82` (aarch64)
- far: `root@45.11.1.62` (x86_64)
- shape: mode=sc-ldpc, adaptive=off, 256 carriers
- fix44: short block sends row 0 only; NACK names no padding
  - near TX uplink 8x32: n=8 m=24 w=128 L=16, rate 1/4
  - far TX downlink 1x256: n=128 m=128 w=256 L=8, rate 0.5
- peer shape (far dmesg): `[170350.867671] tbtun: tunnel 1: fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64`
- peer shape (near dmesg): `[24383.226766] tbtun: tunnel 1: fec rx shape from peer: n=128 m=128 groups=1 slots=8 block=256 L=8 rate=500`
- evidence: `/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/sclc-verify-20261108T064508Z`

## Verdict

**FAIL**

| item | value |
|---|---|
| first connect | http=? size=? ttfb=?s rc=? |
| steady app bytes (far submit) | ? |
| steady app bytes (near curl rchar) | ? |
| steady wire (far tx / near rx) | ? / ? |
| steady near/far CPU | ?% / ?% |
| 1 GiB http | 200 |
| 1 GiB size | 1073741824 |
| 1 GiB sha256 near | e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393 |
| 1 GiB sha256 far | e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393 |
| 1 GiB time | 889.202476 s |
| 1 GiB throughput | 9.66 Mbit/s |
| 1 GiB wire far/near | 3069013375 / 2500788882 |
| 1 GiB near/far CPU | 8.7% / 40.1% |
| near boot_id stable | yes |
| far boot_id stable | yes |
| watchdog | no holder; untouched |

## 用户本轮 1 GiB 判据（补记）

按用户本轮列出的判据，**1 GiB PASS**：

| 判据 | near | far | 结果 |
|---|---|---|---|
| byte-exact sha256 | `e926b854…ad7393` | `e926b854…ad7393` | **PASS** |
| `fec_nack_lat_ns`（NACK→wire）<600 s | 0 | 502.20 s | **PASS** |
| `fec_replay_miss_never_stored` ≤256 | 24 | 21 | **PASS** |
| `fec_short_parity_saved` >0 | 23 | 889 | **PASS** |
| `fec_nack_pad_skip` =0 | 0 | 0 | **PASS** |
| replay high <5 MiB | 0 | 2,941,680 | **PASS** |
| `fec_nack_backpressure` =0 | 0 | 0 | **PASS** |
| reset_tcp / reset_hole / holes_unrec / deliver_bad | 0 | 0 | **PASS** |
| tbtun workqueue hog | 无 | 有（informational） | 记录 |
| BUG/Oops/WARNING | 无 | 无 | **PASS** |

1 GiB：rc=0、HTTP 200、size/file 1,073,741,824、
time 889.202476 s、TTFB 0.539886 s、speed 1,207,533 B/s = **9.66 Mbit/s**。
wire far 3,069,013,375 / near 2,500,788,882；CPU near 8.7% / far 40.1%。
far `fec_nack_retx=30,386`、`fec_nack_lat_max_ns=603.9 ms`；
near `fec_nack_tx=830`、far `fec_nack_rx=830`；ACK 双向 974。

**脚本 verdict FAIL 只来自两条不在本轮判据内的 `up` 检查**：
near `up=252`、far `up=254`（末尾 2–4 条 carrier 不可调度）。
传输期间 1 GiB 完整、sha256 一致、无 stream reset/hole/drop；
`up` 下降是 carrier 可用性观察，若要求全程 256 则单独判 FAIL。

## Counter increments

```
first connect:

steady near:

steady far:

1 GiB near:
fec_outq_drop=0
fec_outq_high_wait=0
fec_init_bad=0
fec_nack_tx=830
fec_nack_rx=3
fec_nack_retx=0
fec_nack_drop=0
fec_replay_hit=0
fec_replay_miss=24
fec_replay_miss_never_stored=24
fec_reset_tcp_failed=0
fec_reset_user=0
fec_reset_hole=0
fec_nack_lat_ns=0
fec_nack_lat_max_ns=0
fec_nack_repair_ns=358185313164
fec_nack_repair_max_ns=41048634070
fec_outq_max=0
fec_replay_miss_enospc=0
fec_replay_drop=0
fec_nack_backpressure=0
fec_hol_wait=0
fec_short_parity_saved=23
fec_nack_pad_skip=0
fec_decode_blocks=11354
fec_decode_recovered=54046
fec_decode_nomem=0
fec_deliver_bad=0
fec_epoch_conflict=0
fec_marker_conflict=0
holes_unrecoverable=0
holes_declared=0
stream_reset_tcp_failed=0
stream_reset_user=0
submit_bytes=78
credit_wait=0
ack_tx=974
ack_rx=1
up_post=252
replay_queue_high_post=0
repair_max_post=41048634070

1 GiB far:
fec_outq_drop=0
fec_outq_high_wait=23
fec_init_bad=0
fec_nack_tx=3
fec_nack_rx=830
fec_nack_retx=30386
fec_nack_drop=0
fec_replay_hit=30386
fec_replay_miss=21
fec_replay_miss_never_stored=21
fec_reset_tcp_failed=0
fec_reset_user=0
fec_reset_hole=0
fec_nack_lat_ns=502202119624
fec_nack_lat_max_ns=603918903
fec_nack_repair_ns=0
fec_nack_repair_max_ns=0
fec_outq_max=122
fec_replay_miss_enospc=0
fec_replay_drop=0
fec_nack_backpressure=0
fec_hol_wait=32963891
fec_short_parity_saved=889
fec_nack_pad_skip=0
fec_decode_blocks=0
fec_decode_recovered=0
fec_decode_nomem=0
fec_deliver_bad=0
fec_epoch_conflict=0
fec_marker_conflict=0
holes_unrecoverable=0
holes_declared=0
stream_reset_tcp_failed=0
stream_reset_user=0
submit_bytes=1073741930
credit_wait=24716
ack_tx=1
ack_rx=974
up_post=254
replay_queue_high_post=2941680
repair_max_post=0
```

## Checks

```
CHECK near_ssh PASS (reachable)
CHECK far_ssh PASS (reachable)
CHECK near_loaded PASS (tbtun)
CHECK far_loaded PASS (tbtun)
CHECK near_srcversion PASS (src=8286019A3E526A5D58BEF0B want=8286019A3E526A5D58BEF0B)
CHECK far_srcversion PASS (src=4A34EC0ACF15D6418F03E4C want=4A34EC0ACF15D6418F03E4C)
CHECK near_cfg_mode PASS (mode=sc-ldpc)
CHECK near_cfg_carriers PASS (carriers=256)
CHECK near_cfg_fec PASS (fec=on)
CHECK near_cfg_adaptive PASS (adaptive=off)
CHECK near_cfg_crypto PASS (crypto=on)
CHECK near_cfg_profile PASS (profile=http-kal)
CHECK near_cfg_n PASS (n=8)
CHECK near_cfg_m PASS (m=24)
CHECK near_cfg_w PASS (w=128)
CHECK near_cfg_L PASS (L=16)
CHECK near_cfg_dev PASS (dev=eth0)
CHECK far_cfg_mode PASS (mode=sc-ldpc)
CHECK far_cfg_carriers PASS (carriers=256)
CHECK far_cfg_fec PASS (fec=on)
CHECK far_cfg_adaptive PASS (adaptive=off)
CHECK far_cfg_crypto PASS (crypto=on)
CHECK far_cfg_profile PASS (profile=http-kal)
CHECK far_cfg_n PASS (n=128)
CHECK far_cfg_m PASS (m=128)
CHECK far_cfg_w PASS (w=256)
CHECK far_cfg_L PASS (L=8)
CHECK near_c3_up PASS (up=256)
CHECK far_c3_up PASS (up=256)
CHECK near_c13_adaptive PASS (adaptive=off)
CHECK near_c13_n PASS (n=8 want=8)
CHECK near_c13_m PASS (m=24 want=24)
CHECK near_c13_groups PASS (groups=8 want=8)
CHECK far_c13_adaptive PASS (adaptive=off)
CHECK far_c13_n PASS (n=128 want=128)
CHECK far_c13_m PASS (m=128 want=128)
CHECK far_c13_groups PASS (groups=1 want=1)
CHECK far_shape_line PASS ([170350.867671] tbtun: tunnel 1: fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64)
CHECK far_shape_block PASS ([170350.867671] tbtun: tunnel 1: fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64)
CHECK far_shape_nm PASS ([170350.867671] tbtun: tunnel 1: fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64)
CHECK far_shape_rate PASS ([170350.867671] tbtun: tunnel 1: fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64)
CHECK near_shape_line PASS ([24383.226766] tbtun: tunnel 1: fec rx shape from peer: n=128 m=128 groups=1 slots=8 block=256 L=8 rate=500)
CHECK near_shape_block PASS ([24383.226766] tbtun: tunnel 1: fec rx shape from peer: n=128 m=128 groups=1 slots=8 block=256 L=8 rate=500)
CHECK near_baseline_kernel PASS (no BUG/Oops/WARNING)
CHECK far_baseline_kernel PASS (no BUG/Oops/WARNING)
CHECK near_route_dev PASS (dev=eth0)
CHECK far_phys_dev PASS (dev=eth0)
CHECK near_bypass_rule PASS (pref 100 to 45.11.1.62)
CHECK near_listen PASS (127.0.0.1:9001)
CHECK far_listen PASS (0.0.0.0:9000)
CHECK watchdog_feeder SKIP (no /dev/watchdog0 holder; left untouched)
CHECK far_source PASS (size=1073741824 sha=e926b854bd3b2d47...)
CHECK far_source_size PASS (size=1073741824)
CHECK one_g_sampler_started PASS (near_lines=4 far_lines=14)
CHECK one_g_rc PASS (rc=0)
CHECK one_g_http PASS (http=200)
CHECK one_g_size PASS (size_download=1073741824)
CHECK one_g_file PASS (file=1073741824)
CHECK one_g_sha PASS (near=e926b854bd3b2d47... far=e926b854bd3b2d47...)
CHECK near_one_g_up FAIL (up=252)
CHECK near_one_g_outq_drop PASS (delta=0)
CHECK near_one_g_init_bad PASS (delta=0)
CHECK near_one_g_reset_hole PASS (delta=0)
CHECK near_one_g_epoch_conflict PASS (delta=0)
CHECK near_one_g_reset_tcp PASS (delta=0)
CHECK near_one_g_holes_unrec PASS (delta=0)
CHECK near_one_g_deliver_bad PASS (delta=0)
CHECK near_one_g_replay_enospc PASS (delta=0)
CHECK near_one_g_replay_queue_high PASS (high=0 bound=5242880)
CHECK near_one_g_never_stored PASS (delta=24 bound=256)
CHECK near_one_g_short_parity_saved PASS (delta=23)
CHECK near_one_g_nack_pad_skip PASS (delta=0 bound=0)
CHECK near_one_g_nack_lat_ns PASS (window_delta_ns=0 bound=600s (NACK->wire))
CHECK far_one_g_up FAIL (up=254)
CHECK far_one_g_outq_drop PASS (delta=0)
CHECK far_one_g_init_bad PASS (delta=0)
CHECK far_one_g_reset_hole PASS (delta=0)
CHECK far_one_g_epoch_conflict PASS (delta=0)
CHECK far_one_g_reset_tcp PASS (delta=0)
CHECK far_one_g_holes_unrec PASS (delta=0)
CHECK far_one_g_deliver_bad PASS (delta=0)
CHECK far_one_g_replay_enospc PASS (delta=0)
CHECK far_one_g_replay_queue_high PASS (high=2941680 bound=5242880)
CHECK far_one_g_never_stored PASS (delta=21 bound=256)
CHECK far_one_g_short_parity_saved PASS (delta=889)
CHECK far_one_g_nack_pad_skip PASS (delta=0 bound=0)
CHECK far_one_g_nack_lat_ns PASS (window_delta_ns=502202119624 bound=600s (NACK->wire))
CHECK near_one_g_ack_tx PASS (near_ack_tx=974)
CHECK far_one_g_ack_rx PASS (far_ack_rx=974)
CHECK near_one_g_kernel PASS (window clean, source=journalctl)
CHECK near_no_restart PASS (boot_id a1a01693.. -> a1a01693..)
CHECK far_no_restart PASS (boot_id 746ecb8d.. -> 746ecb8d..)
CHECK near_dmesg_tail PASS (no BUG/Oops/WARNING in last 1000 lines)
CHECK far_dmesg_tail PASS (no BUG/Oops/WARNING in last 1000 lines)
```

## Cleanup

see `cleanup.txt`
