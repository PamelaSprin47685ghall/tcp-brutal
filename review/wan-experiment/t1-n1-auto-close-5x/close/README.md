# HND0 N=1 sink 5x — sendclose (close) variant — 4c58b0e

## 结论：5/5 PASS

同一 tunnel 连续 5 轮，每轮 sink 都读满 16,777,216 B 并收到 EOF；near FIN
在数据排空后才写；far `rx_delivered` 每轮 +16,777,216；无 RST、无
deliver/close timeout、无 detach、near/far live txq=0、holes=0、lost=0。
accept wake race 未复现（far `rx_work_runs` 每轮正常前进，sink 每轮 accept）。

## 源码与模块 invariant

- `w-window 4c58b0e`（父 ddef988，accept wake race 修复：
  `tbt_car_schedule_rx()` 的 `rx_again` 重消费）
- `git archive 4c58b0e | sha256sum` =
  `a60f33ab5db7521d3108c37528a746e85ada9e7bd137b586ebac50af49c1dc76`
- 两端同源、各自构建，`srcversion` 同为 `10C45DCB900C5C78F725596`：

| 端 | kernel | ko sha256 | vermagic |
|---|---|---|---|
| near JYSZ | 7.2.9-1-MANJARO | `c14bafee9a858a4b406edd580b91906734a6ebbbf27dc0e2bdd7d14c8aafb437` | 7.2.9-1-MANJARO |
| far HND0 | 7.0.0-42-generic | `0da134805ffd13b78423cc0e3c6eee3896d354cd671bd5fa2d9edca797b9b87f` | 7.0.0-42-generic |

`adv_probe`：near `c03ea90a…`，far `c4daa279…`（far 用
`-Werror -Wno-error=unused-result`，同 ddef988 证据的编译口径）。

## 配置

```
# near
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff

# far
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

- 同一 tunnel 保持 5 轮；每轮重启 sink（`adv_probe sink 5201 5`），
  sender 每轮 `adv_probe sendclose 9001 16 1`（写满 16 MiB 后 `close()`）。
- `debug=2`；near 加 `ip rule pref 100` 绕 mihomo。

## 5 轮结果

| 轮 | sink bytes/eof | sender bytes / txq_after | far rx_delivered（累计/增量） | near fin | far fin | timeouts | live txq | CPU near/far | rx_work_runs | 判定 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 16777216 / 1 | 16777216 / 1022940 | 16777216 / 16777216 | `fin_seq=16777216 fin_w=1` | `fin_w=1 rxf=1/1` | 0/0/0 | 0/0 | 10.13% / 32.64% | 3623 | PASS |
| 2 | 16777216 / 1 | 16777216 / 1104860 | 33554432 / 16777216 | 同上 | 同上 | 0/0/0 | 0/0 | 8.99% / 33.16% | 8186 | PASS |
| 3 | 16777216 / 1 | 16777216 / 761326 | 50331648 / 16777216 | 同上 | 同上 | 0/0/0 | 0/0 | 9.06% / 30.34% | 12910 | PASS |
| 4 | 16777216 / 1 | 16777216 / 1113052 | 67108864 / 16777216 | 同上 | 同上 | 0/0/0 | 0/0 | 8.97% / 32.58% | 17030 | PASS |
| 5 | 16777216 / 1 | 16777216 / 1006821 | 83886080 / 16777216 | 同上 | 同上 | 0/0/0 | 0/0 | 8.94% / 31.06% | 22255 | PASS |

- 每轮 near `tx_ack=16777216`、`fbk_rx` 1213/2129/3174/4286/5147；
  far `fbk_rx` 1283/2342/3409/4455/5452。
- 每轮 far `up=1 lost=0 join_bad=0`；`holes_unrecoverable=0`、
  `carrier_lost=0`、`lost=0`；`deliver_timeouts=0`、`close_timeouts=0`、
  `close_detaches=0`；`rst_w=0`。
- near FIN 时序：`fin_q=1 fin_w=0` 时 txq 仍非 0，drain 到 0 后
  `fin_w=1`，再 `rxf=1/1` 并 detach。far 的 `fin_seq=0`（far 无数据方向），
  `fin_w=1 rxf=1/1`，`tgt_state=7`（sink 读完后关闭）。
- `klog-near-run.txt` / `klog-far-run.txt`（从 4c58b0e 模块 load 起）无
  `TXSTALLKILL`/`RXHOLEKILL`。`klog-near-all.txt` 里 08:03 的
  `TXSTALLKILL` 是更早 ddef988 实例（hnd0-sink 的 accept race 轮次），
  早于本实例 08:13:27 的 load。

## 收尾

- 两端 `del 1` + `rmmod tbtun`；far sink/poll/iperf3/lock unit 全停并
  reset-failed；`/tmp/tbtun-hnd0.lock` 删除；near `ip rule pref 100` 删除。
- HND0 frps/caddy/hysteria-server/xray active；无 9000/9001/5201 监听；
  临时目录清理。
- 未跑 256，未合并/push。

## 文件

- `round-N/`：`sink.out`、`sendclose.txt`、`stats-before/after-{near,far}.txt`、
  `klog-{near,far}.txt`、`fin-timeline-{near,far}.txt`、`cpu-{near,far}.txt`、
  `summary.txt`、`summary.json`、`t0/t1.txt`。
- `poll-{near,far}.txt`：0.1 s 连续轮询（5 轮）。
- `config-{near,far}.txt`、`ports-{near,far}.txt`、`module-{near,far}.txt`、
  `source.txt`、`klog-{near,far}-all.txt`、`klog-{near,far}-run.txt`。
- 单次 run 的 iperf3 吞吐/rc 证据在 `../t1-n1-auto-rx1m-sink/`（b3b33b2）；
  本轮 5x 只跑 sink 门禁。
