# HND0 N=1 sink 5x — sendclose 变体（0888ce4，J hole-floor 修复）

2026-11-06。源 `w-window 0888ce4`（receiver auto hole budget floor 1 s → 2 s）。
两端同源、各自构建。本目录只记 `adv_probe sendclose`（写满 16 MiB 后 `close()`）
变体；4c58b0e 的 close/shutdown-wr 证据分别在 `../t1-n1-auto-close-5x/close/`
与 `../t1-n1-auto-close-5x/shutdown-wr/`。

## 结论

- **连续 5 轮 5/5 PASS**。模块一次加载，每轮 far sink 重启、独立日志；
  sender 每轮 `adv_probe sendclose 9001 16 1`（写满 16 MiB 后 `close()`）。
- 每轮：sink `bytes=16777216 eof=1`；sendclose `bytes=16777216`，
  `txq_after` 非零（close 时 tunnel 队列仍有数据，正是被测场景）；
  far `rx_delivered` 增量 16,777,216；near `fin_seq=16777216 fin_w=1 rst_w=0`；
  far `fin_w=1 rxf=1/1`；两端 live `txq=0`；`lost`/`carrier_lost`=0；
  `holes_declared`/`holes_unrecoverable`=0；`deliver_timeouts`/`close_timeouts`/
  `close_detaches`=0；无 `TXSTALLKILL`。

## 源码与 per-end 模块

- 源：`0888ce40e439ea9e4f947b8694dcfc6d0785e027`（`w-window`，父 `8980c03`，
  基 `4c58b0e`）。改动：`TBT_RX_HOLE_FLOOR_US=2000000`（auto = max(2 s, 5×SRTT)）。
- `git archive 0888ce4 | sha256sum` =
  `47d8652fe5b708609026c4b3c0079aa301786e0c638d047da9cdc938a2c98962`。
- 构建：near `make KERNEL_DIR=$KDIR PAHOLE=$PAHOLE -j2`（7.2.9-1-MANJARO）；
  far `make -C /lib/modules/7.0.0-42-generic/build M=$PWD PAHOLE=false -j1 modules`。

| 端 | kernel | ko sha256 | srcversion | vermagic |
|---|---|---|---|---|
| near | 7.2.9-1-MANJARO | `0e4ae50c3529d8463eae746611e608fce2bf75845752229594a4414225801b7a` | `711422E4053E3F13B89BD4E` | `7.2.9-1-MANJARO SMP preempt mod_unload` |
| far | 7.0.0-42-generic | `95aefe350373e927ee513e67accf735243990e9466d4c8513a896b9b52c77201` | `711422E4053E3F13B89BD4E` | `7.0.0-42-generic SMP preempt mod_unload modversions` |

ko sha 依赖构建路径与内核，不能跨端比较；`srcversion` 相同即源码同一份。
window-verify 的 near 构建为 `a6e459f6…`（同一 0888ce4、同一 srcversion），
与本目录的 `0e4ae50c…` 只差构建路径。`adv_probe`：near `c03ea90a…`；
far `c4daa279…`（`-Werror -Wno-error=unused-result`，功能相同）。

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
2. 每轮：far 重启 `adv_probe sink 5201 5`（5 ms/64 KiB 慢读，保证 close 时
   tunnel 队列非空）；near 跑 `adv_probe sendclose 9001 16 1`（发 `B` + 16 MiB−1，
   然后 `close()`；`SO_SNDTIMEO=0` 使 backpressure 表现为阻塞而非 EAGAIN）。
3. 两端每 0.1 s 采集 `/proc/net/tbtun/{stats,peers,tunnels,config,ports}` 与
   `/proc/stat`；sink 输出 `SINK_BULK` 后 1 s 停止轮询，采集 after 快照。
4. 门禁按每轮增量：far `rx_delivered` 为 after−before；`lost`/`holes`/timeouts
   同理。模块只在开始时加载一次，轮间不重载。

## 每轮结果

| round | sink | sendclose bytes / txq_after | far rx 增量 | near fin | far fin | lost/holes/timeouts | CPU near/far | verdict |
|---|---|---|---|---|---|---|---|---|
| 1 | 16,777,216 / EOF | 16,777,216 / 810,213 | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0/0/0 | 10.15% / 26.74% | PASS |
| 2 | 16,777,216 / EOF | 16,777,216 / 1,113,317 | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0/0/0 | 10.41% / 25.59% | PASS |
| 3 | 16,777,216 / EOF | 16,777,216 / 842,981 | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0/0/0 | 10.09% / 27.00% | PASS |
| 4 | 16,777,216 / EOF | 16,777,216 / 630,254 | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0/0/0 | 10.15% / 26.21% | PASS |
| 5 | 16,777,216 / EOF | 16,777,216 / 401,143 | 16,777,216 | 16777216 / fin_w=1 / rst_w=0 | fin_w=1 / rxf=1/1 | 0/0/0 | 10.21% / 26.12% | PASS |

两端 live `txq=0`，`close_detaches=0`，`TXSTALLKILL` 0（`klog-{near,far}.txt`）。
每轮 far `fin_seq=0`（far 无数据方向）、`fin_w=1 rxf=1/1`，`tgt_state=7`。

## 收尾

- 两端 `del 1` + `rmmod tbtun`；9000/9001/5201 无监听；near `ip rule pref 100` 删除。
- HND0 `frps`、`caddy`、`hysteria-server`、`xray` 全 active；临时目录已清理。
- 未跑 256，未合并/push。

## 文件

- `source.txt`、`config-{near,far}.txt`、`ports-{near,far}.txt`、
  `module-{near,far}.txt`、`ko-sha-{near,far}.txt`。
- `round-1..5/`：`sink.out`、`sendclose.txt`、`gate-detail.txt`、`verdict.txt`、
  `stream-{near,far}.txt`、`config-{near,far}.txt`、`ports-{near,far}.txt`、
  `before-{near,far}.txt`、`after-{near,far}.txt`、`klog-{near,far}.txt`、
  `poll-{near,far}.txt`、`t0.txt`、`t1.txt`。
- `carrier-up-{near,far}.txt`、`cpu-summary.txt`、`summary.txt`、
  `teardown.txt`、`orchestrator.log`。
