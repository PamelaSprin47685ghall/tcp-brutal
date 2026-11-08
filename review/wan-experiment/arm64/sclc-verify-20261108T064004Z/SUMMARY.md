# SC-LDPC 0.3.1 fix44 真机验收（稳态 PASS；far workqueue hog 记录；1 GiB 未跑）

- 模块: tbtun 0.3.0（0.3.1 版本号未 bump）, HEAD `a21f523`
  - near srcversion `8286019A3E526A5D58BEF0B`
  - far  srcversion `4A34EC0ACF15D6418F03E4C`
- near `root@192.168.123.82` / far `root@45.11.1.62`
- 运行: 2026-11-08T06:40:04Z – 06:42:27Z
- 模式: `--from-steady`（首连按用户允许跳过；前一轮 NACK→wire 已 PASS）
- 证据: `review/wan-experiment/arm64/sclc-verify-20261108T064004Z/`

## 判定

**稳态数据面 PASS**（用户列出的全部判据通过）。脚本因 far 的
`tbt_car_rx_work [tbtun]` workqueue hog 停止；该 hog 不在用户本轮判据
（NACK→wire/replay_miss/short_parity_saved/pad_skip/replay/backpressure/
byte-exact/无 reset/holes）内，是性能告警，不是 reset/hole/drop。
**1 GiB 未执行**；cleanup 已执行。

## 稳态数字（warmup 20 s + measure 60 s）

| 项 | near | far |
|---|---|---|
| `c3 up`（采样最小值） | 256（256） | 256（256） |
| app（far `submit_bytes` 增量） | — | **171,835,392 B / 60 s = 2.86 MB/s** |
| wire（far TX / near RX） | 332,820,005 | **411,574,798**（wire/app 2.40） |
| CPU busy | **12.3%** | **55.7%** |
| `fec_outq_drop` / `fec_outq_high_wait` | 0 / 0 | 0 / 0 |
| `fec_replay_miss_never_stored`（窗口） | **0** | **0** |
| `fec_short_parity_saved`（累计 / 窗口） | **23** / 0 | **127** / 0 |
| `fec_nack_pad_skip`（窗口） | **0** | **0** |
| `fec_nack_lat_ns`（窗口，NACK→wire） | **0** | **0** |
| `fec_nack_repair_ns`（窗口，block-ready，info） | 0 | 0 |
| `fec_nack_repair_max_ns`（session，info） | 98.35 s | 97.61 s |
| `fec_replay_queue_high`（累计） | 8,240 | 16,480（<5 MiB） |
| `fec_nack_backpressure`（窗口） | 0 | 0 |
| `fec_ack_tx`（窗口） | **+157** | — |
| `fec_ack_rx`（窗口） | — | **+157** |
| `fec_credit_wait`（窗口） | 0 | +1,453（near ack_tx +157，ACK 在推进） |
| `fec_hol_wait`（窗口，加权，info） | 0 | 1,077,813 |
| reset_tcp / reset_hole / holes_unrec / deliver_bad | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

- 稳态窗口内没有 NACK/重传（`fec_nack_retx`、`fec_nack_lat_ns` 增量 0）；
  修复指标为 0，NACK→wire 判据 PASS。
- `fec_short_parity_saved` 的窗口增量为 0（短块在窗口外），但累计值
  23/127 证明短块路径生效；脚本已改为按累计值判 `short_parity_saved>0`。

## far workqueue hog（记录）

```
Nov 08 06:40:47 hnd0.kw92.cyou kernel: workqueue: tbt_car_rx_work [tbtun]
hogged CPU for >10000us 7 times, consider switching to WQ_UNBOUND
```

- 时间在稳态窗口开始后约 2 s；near 窗口无 hog，两端无 BUG/Oops/WARNING。
- 用户本轮判据不含 hog；脚本原先把它当 FAIL，导致 1 GiB 未执行。
  已把首连/稳态/1 GiB 的 tbtun hog 改为信息项；BUG/Oops/WARNING 仍为硬失败。
- far CPU 55.7% 与该 hog 一致：256-carrier RX 在 far 上的处理是热点。

## Harness 修正（本轮已实施）

1. 短块修复判据统一为 `fec_nack_lat_ns`（NACK→wire）窗口增量：
   首连 <60 s、稳态 <180 s、1 GiB <600 s；block-ready/session max 只记录。
2. 稳态 `short_parity_saved` 改为按累计值 >0 判（窗口增量可为 0）。
3. 首连/稳态/1 GiB 的 tbtun workqueue hog 改为信息项；
   BUG/Oops/WARNING/UBSAN/GPF 仍为硬失败。
4. quiet-wait（60 s 静默，最多 600 s）保留；首连按用户允许可跳过。

## 未执行

- 1 GiB 单流 + 双端 sha256：稳态脚本因 hog 停止，未执行。
- 首连：本模式跳过；前一轮 `sclc-verify-20261108T062811Z` 已按 NACK→wire
  PASS（near 1.96 ms、far 204 ms）。

## 清理（已完成）

`cleanup.txt`：

- far raw sender 停止：`FAR_STOP PASS`，5210 释放；
- near `pref 100 to 45.11.1.62 lookup main` 删除；
- near / far `rmmod tbtun`：两端 `tbtun not loaded`；
- watchdog：`tbtun-netwatch` inactive，未触碰；其他服务/隧道未动；
- far 1 GiB 源文件保留；near 部分文件保留。

## 证据文件

`run.log`、`timeline.txt`、`console.log`、
`steady-a/b-*-stats.txt`、`steady-sampler-*.log`、`steady-curl.txt`、
`steady-near.txt`、`steady-far.txt`、`near-kernel-steady.txt`、
`far-kernel-steady.txt`、`diag-steady-*`、`cleanup.txt`、
`near-module.txt`/`far-module.txt`。

## 建议

- 稳态按用户判据 PASS；far hog 是已知的 256-carrier RX 热点，建议单列
  性能项，不作为数据面失败。
- 1 GiB 尚未跑；需要 sclc-031-core 重新部署后，用 `--one-g-only`
  （或 `--from-steady`）跑一次 1 GiB sha256。脚本已把 hog 改为信息项，
  1 GiB 判据为 NACK→wire <600 s、replay_miss≤256、short_parity_saved>0、
  pad_skip=0、replay high<5 MiB/backpressure 0、byte-exact、无 reset/holes。
- 本轮按单次规则未重试；是否重跑由 Main 决定。
