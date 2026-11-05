# HND0 N=1 auto — f8023a2, 1 MiB reorder (2026-11-06)

## 结论（先说失败点）

- `rc=0`，控制流不再被 `TXSTALLKILL` 杀死；far `holes_unrecoverable=0`、
  `fbk_rx=1089`、`carrier_lost=0`、`lost=0`；near `tx_window=1048576`
  （1 MiB cap），`bdp_bytes=1321474`、`k_x100=200`。
- **但 16 MiB 没有逐字节到达**：iperf3 sender 发 16,777,216 B，
  receiver 只收到 13,500,416 B（80.47%），server 日志 12.9 MiB。
  这是 public socket close 时未排空接收队列/txq 的截断，不是控制流饥饿。
- 因此本轮不能判 G1 通过；需要 A 修 close-drain 后重跑。

## 源码与模块 invariant

- `w-window f8023a20ebae167c52cc130ebe0dc02a8b1b40c9`
- `git archive f8023a2 | sha256sum` =
  `3a9781415c03feff7278eeba0732b2af951aa3b13b7161a20feac6af7e85adc8`
- 两端同源、各自构建，`srcversion` 必须同为 `475868A5062251A7C237E22`：

| 端 | kernel | ko sha256 | srcversion | vermagic |
|---|---|---|---|---|
| near JYSZ | 7.2.9-1-MANJARO | `cd1999a40b036a2bffb1988d078479cd1067e6e985864be04fe523687116ba6c` | `475868A5…` | 7.2.9-1-MANJARO |
| far HND0 | 7.0.0-42-generic | `b0ad315a7c0dfb3cb27c051fcdf583616ed5b2316ed9590e1c66a3fb1c328f70` | `475868A5…` | 7.0.0-42-generic |

ko sha 依赖 build path/kernel，不能跨端用单一 sha；`srcversion` 相同即源码同一。
near ko 取自 `window-verify` 的 f8023a2 全量重编（`/home/kunweiz/tbtun-worktrees/window/tbtun.ko`），
far ko 在 HND0 用同一份 `git archive f8023a2` 源码构建。

## 配置（两端）

```
# near
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff

# far
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

- insmod 参数：`debug=2`（两端 `/sys/module/tbtun/parameters/debug` = 2）。
- global ports：near `ports 30000-30099`，far `ports 31000-31099`。
- far 先写 config，near 后写；near 先加 `ip rule add to 45.11.1.62/32 lookup main pref 100`。

## 运行

```
LD_LIBRARY_PATH=/tmp/tbtun-iperf3/usr/lib/x86_64-linux-gnu \
  /tmp/tbtun-iperf3/usr/bin/iperf3 -c 127.0.0.1 -p 9001 -n 16M --json
```

- `rc=0`，`sum_sent.bytes=16777216`，4.0006 s，33.55 Mbit/s（sender）。
- `sum_received.bytes=13500416`，25.38 Mbit/s；server 日志 receiver 12.9 MiB。
- 两端每 0.2 s 轮询 `stats/peers/tunnels/config/ports`，含两条 stream 的
  per-stream 行（见 `poll-near.txt` / `poll-far.txt`，时间线见 `streams-*.txt`）。

## 关键计数

| 项 | near | far |
|---|---|---|
| `holes_unrecoverable` | 0 | 0 |
| `fbk_rx` | 1069 | 1089 |
| `carrier_lost` / `lost` | 0 / 0 | 0 / 0 |
| `tx_window` | 1048576（cap） | 210880 |
| `bdp_bytes` / `k_x100` | 1321474 / 200 | 99902 / 200 |
| `retx_frames` | 11 | 8 |
| `hole_checks` gate/depth/time | 474 / 359 / 90 / 25 | 110 / 76 / 34 / 0 |

- 本轮 filtered klog（`klog-*-run.txt`）无 `TXSTALLKILL`/`RXHOLEKILL`。
- `klog-near.txt` 全量 dmesg 里 07:16:36 的 `TXSTALLKILL` 是 E 上一轮
  （cb5771e）的旧记录，不在本模块实例内；本实例 07:30:40 加载。

## 失败分析：close 截断

- near `submit_bytes=15,996,575`（客户端写了 16,777,216 B，
  **780,641 B 在 public socket 接收队列里没被 capture 读走**）。
- near `tx=14,007,990`、`wire=14,118,309`；流结束后 `txq=1,113,317`
  仍留在 session txq，`reconcile live_bytes=14,118,309 live_data=1734 retired=0`。
- far `rx_delivered=13,614,558`；far 数据流 `sid=552708603905` 在
  `1793921475.999` 变 `state=5 kill=5`（`TBT_ST_DEAD`，app close）时 `rxq=24576`。
- 数据流 sid=...905 在 near 也是 `state=5 kill=5`；控制流 sid=...904
  一直 `state=1 kill=0` 到结果交换完成（far `tgt_state` 1→8→7），所以 rc=0。
- 机制：iperf3 `-n 16M` 写完即 close data socket；tbtun 在 public socket close
  时直接 `tbt_stream_kill(st, TBT_ST_DEAD)`，没有先排空 public 接收队列和
  session txq。FIN/RST 越过仍在队列里的数据，far 只收到 13.5 MB。
  这是 close-drain 缺陷，不是 f8023a2 修掉的控制流 starvation。
- 建议 A：public socket EOF/close 时先 drain capture + txq，再发 FIN；
  只有 EPIPE/ECONNRESET/ESHUTDOWN 才 RST；`del 1`/rmmod 后 txq 必须归零
  （本轮 `del 1` 后 rmmod 无 warning，但 1.11 MB 是静默丢的）。

## CPU

- near `/proc/stat`（run 窗口 5.25 s）：busy 814 / 8424 jiffies = **9.66%**
  （user 8.25%、sys 0.96%、irq 0.20%、softirq 0.25%）。
- far `/proc/stat`（同窗口）：busy 221 / 1018 jiffies = **21.71%**
  （user 5.01%、sys 13.46%、softirq 3.14%）。
- iperf3 JSON：host_total 15.13%、remote_total 2.46%（进程视角）。

## 收尾

- 两端 `del 1` + `rmmod tbtun`；HND0 `tbtun-iperf3-5201` 停止并 reset-failed；
  `tbtun-n1-lock` 停止，`/tmp/tbtun-hnd0.lock` 删除；near `ip rule pref 100` 删除。
- HND0 frps/caddy/hysteria-server/xray 全 active；两端无 tbtun、无 9000/9001/5201
  监听；`/tmp/tbtun-t1-f8023a2` 构建/日志目录已清理。
- 未跑 256，未合并/push。

## 文件

- `config-{near,far}.txt`、`ports-{near,far}.txt`：运行前快照。
- `{near,far}-before.txt` / `{near,far}-after.txt`：stats/peers/tunnels/config/ports/cpu。
- `poll-{near,far}.txt`：0.2 s 轮询原始流；`streams-{near,far}.txt`：per-stream 行时间线。
- `klog-{near,far}-run.txt`：本模块实例的 klog；`klog-{near,far}.txt`：全量 dmesg。
- `iperf3-16MiB.json`、`iperf3-16MiB.err`、`iperf3-server.log`、`rc.txt`、`start.ts`、`end.ts`。
- `module-{near,far}.txt`、`source.txt`、`cpu-{near,far}.txt`、`cpu-summary.txt`。
