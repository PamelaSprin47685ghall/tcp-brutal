# HND0 N=1 sink 5x — `shutdown(SHUT_WR)` 变体（4c58b0e）

2026-11-06。源 `w-window 4c58b0e`（carrier 接收唤醒修复），两端同源、各自构建。
本目录只记 `shutdown(SHUT_WR)` 变体；`sendclose`（`close()`）变体在 `../close/`。

## 结论

- **连续 5 轮 5/5 PASS**。模块一次加载，每轮 far sink 重启、独立日志；
  near sender 发 16 MiB 后 `shutdown(SHUT_WR)`，再读到 EOF。
- 每轮：sink `bytes=16777216 eof=1`；sender `bytes=16777216 eof=1`；
  far `rx_delivered` 增量 16,777,216；near `fin_seq=16777216 fin_w=1 rst_w=0`；
  far `fin_w=1 rxf=1/1`；两端 `rxf=1/1`、live `txq=0`；
  `lost`/`carrier_lost`=0；`holes_declared`/`holes_unrecoverable`=0；
  `deliver_timeouts`/`close_timeouts`/`close_detaches`=0；无 `TXSTALLKILL`。
- 每轮 sender 读到 0 字节回程、EOF=1，说明 sink 关闭后的回程 FIN 走完；
  near 的 FIN 序列号精确等于 16,777,216，说明 close-drain 没丢字节。

## 源码与 per-end 模块

- 源：`4c58b0eed002c1cd0c0fb15f9b04f97b44ff1beb`（`w-window`，父 `ddef988`）。
- `git archive 4c58b0e | sha256sum` =
  `a60f33ab5db7521d3108c37528a746e85ada9e7bd137b586ebac50af49c1dc76`。
- 两端 `tbt_carrier.c` md5 同为 `cd71ea20dff9471018690c19d5209d0a`。
- 构建：near `make KERNEL_DIR=$KDIR PAHOLE=$PAHOLE -j2`（7.2.9-1-MANJARO）；
  far `make -C /lib/modules/7.0.0-42-generic/build M=$PWD PAHOLE=false -j1 modules`。

| 端 | kernel | ko sha256 | srcversion | vermagic |
|---|---|---|---|---|
| near | 7.2.9-1-MANJARO | `b82999b66607fd411b1ca4ea1a1177421a376bd6ea2c790c62e1cea94a2afc0a` | `10C45DCB900C5C78F725596` | `7.2.9-1-MANJARO SMP preempt mod_unload` |
| far | 7.0.0-42-generic | `0da134805ffd13b78423cc0e3c6eee3896d354cd671bd5fa2d9edca797b9b87f` | `10C45DCB900C5C78F725596` | `7.0.0-42-generic SMP preempt mod_unload modversions` |

ko sha 依赖构建路径与内核，不能跨端比较；`srcversion` 相同即源码同一份。
`adv_probe`：near `c03ea90a…`（`-Werror` 直接过）；far `c4daa279…`
（Ubuntu glibc 的 `warn_unused_result` 使 `-Werror` 失败，改用
`-Werror -Wno-error=unused-result`；功能相同）。

## 配置

```
# near
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff

# far
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

- 两端 `insmod debug=2`；global ports：near `30000-30099`，far `31000-31099`。
- near 加 `ip rule add to 45.11.1.62/32 lookup main pref 100`，carrier 走 `wlp4s0`。

## 方法

1. far 先加载并写 config；near 后加载并写 config，等 `c3 up=1`。
2. 每轮：far 重启 `adv_probe sink 5201 5`（5 ms/64 KiB 慢读，保证 close 时队列非空）；
   near 用 Python sender 连 `127.0.0.1:9001`，发 16 MiB（首字节 `B`）后
   `shutdown(SHUT_WR)`，再读到 EOF。
3. 两端每 0.1 s 采集 `/proc/net/tbtun/{stats,peers,tunnels,config,ports}` 与 `/proc/stat`；
   sink 输出 `SINK_BULK` 后 1 s 停止轮询，采集 after 快照。
4. 门禁按每轮增量：far `rx_delivered` 为 after−before；`lost`/`holes`/timeouts 同理。
   模块只在开始时加载一次，轮间不重载，轮间只重启 sink。

## 每轮结果

| round | sink | sender | far rx 增量 | near fin | far fin/rxf | lost | holes | timeouts | verdict |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 16,777,216 / EOF | 16,777,216 / EOF / 17.04 s | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0 | 0 | 0 | PASS |
| 2 | 16,777,216 / EOF | 16,777,216 / EOF / 14.60 s | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0 | 0 | 0 | PASS |
| 3 | 16,777,216 / EOF | 16,777,216 / EOF / 24.65 s | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0 | 0 | 0 | PASS |
| 4 | 16,777,216 / EOF | 16,777,216 / EOF / 17.55 s | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0 | 0 | 0 | PASS |
| 5 | 16,777,216 / EOF | 16,777,216 / EOF / 19.66 s | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0 | 0 | 0 | PASS |

两端 live `txq=0`，`close_detaches=0`，`TXSTALLKILL` 0（`klog-{near,far}.txt`）。

CPU（每轮 before/after 的 `/proc/stat` 差值）：

| round | near | far | 窗口 |
|---|---|---|---|
| 1 | 8.85% | 28.86% | 1793925877.49..1793925894.88 |
| 2 | 8.89% | 27.08% | 1793925945.96..1793925961.25 |
| 3 | 8.99% | 30.41% | 1793926016.20..1793926040.95 |
| 4 | 8.90% | 31.75% | 1793926095.90..1793926114.09 |
| 5 | 8.86% | 31.18% | 1793926163.99..1793926183.72 |

## 附加观察：每轮重载模块（不属于本验收门禁）

为量化“每次重建 carrier”时的 accept/HELLO 路径，另跑了两组**每轮重载两端模块**
的变体。数据仍全部 16 MiB/EOF，但门禁失败：

| 组 | 结果 | 失败签名 |
|---|---|---|
| attempt-1 | 3/5 | r3 far `lost=1`；r5 near `holes_declared=1`（`holes_unrecoverable=0`） |
| attempt-2 | 1/5 | r2–r5 far `lost=1`；`klog-far` 每轮两次 `carrier ACCEPTED ... rport=30000` |

- far 两次 ACCEPTED 同源端口，间隔 0.88–2.2 s；near `c3 lost=0 replaced=0`，
  即 near 侧在 HELLO 阶段超时重拨，far 记一次 carrier lost。
- 数据最终仍 16 MiB + EOF，`deliver/close timeouts=0`；失败只是 `lost`/`holes`
  计数非零。主验收的 spec 不重建 carrier，因此 5/5 成立；该残余窗口属于
  “carrier 重建”路径，需单独处理，不能由本目录的 5/5 覆盖。
- 原始证据：`per-round-reload/attempt-{1,2}/round-{1..5}/`（gate-detail、
  sink/sender、klog-far）；大 poll 文件未保留。

## 收尾

- 两端 `del 1` + `rmmod tbtun`；9000/9001/5201 无监听；near `ip rule pref 100` 删除。
- HND0 `frps`、`caddy`、`hysteria-server`、`xray` 全 active；临时目录已清理。
- 未跑 256，未合并/push。

## 文件

- `source.txt`、`config-{near,far}.txt`、`ports-{near,far}.txt`、
  `module-{near,far}.txt`、`ko-sha-{near,far}.txt`。
- `round-1..5/`：`sink.out`、`sender.out`、`gate-detail.txt`、`verdict.txt`、
  `stream-{near,far}.txt`（最后一条 per-stream 行）、
  `config-{near,far}.txt`、`ports-{near,far}.txt`、
  `before-{near,far}.txt`、`after-{near,far}.txt`、`klog-{near,far}.txt`、
  `poll-{near,far}.txt`、`t0.txt`、`t1.txt`。
- `carrier-up-{near,far}.txt`、`cpu-summary.txt`、`summary.txt`、
  `teardown.txt`、`orchestrator.log`。
- `per-round-reload/attempt-{1,2}/`：附加观察的精简证据。
