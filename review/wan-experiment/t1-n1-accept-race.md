# T1 N=1 sink — accept 侧唤醒竞态记录（ddef988，HND0）

2026-11-06。记录一次同 spec 的独立尝试：**失败，不是通过**。同一份 ddef988
源码在另一次运行（w-hnd0 `b3b33b2`）通过，因此这是时序相关的残余窗口，
不能把单次通过当作 5/5 的证据。

原始 poll/klog 已按清理要求删除；本文只保留失败签名、时间线、构建不变式和
推断。goal-integrator 已把同一观察记入 T0 文档 `9e6c5d8`，本文是仓库内的
独立记录。

## 1. 源码与 per-end 模块

- 源码：`w-window ddef988f57af8a49d1da04c3b5bd374fa59cacbd`
  （`sock: drain a closed stream before its FIN, and bound the wait`）。
- `git archive ddef988 | sha256sum` =
  `7da4624b0ba63e5a04922e4024fef34aedda37243593f1db649fa985edfa514b`。
- 两端同源、各自构建；`srcversion` 必须相同：

| 端 | kernel | 构建 | ko sha256 | srcversion | vermagic |
|---|---|---|---|---|---|
| near JYSZ | 7.2.9-1-MANJARO | `make KERNEL_DIR=$KDIR PAHOLE=$PAHOLE -j2` | `3ae264e34f5af2bdfe6a6eddfdd730381cfca9aa67889d10af97590aab734f28` | `62E75E1CE05E5C4887C9AAA` | `7.2.9-1-MANJARO SMP preempt mod_unload` |
| far HND0 | 7.0.0-42-generic | `make -C /lib/modules/7.0.0-42-generic/build M=$PWD PAHOLE=false -j1 modules` | `461d7a943d86b744fa18254fa7e1849313c0cf94be4f130aca478cbee6a01641` | `62E75E1CE05E5C4887C9AAA` | `7.0.0-42-generic SMP preempt mod_unload modversions` |

ko sha 依赖 build path/kernel，不能跨端比较；`srcversion` 相同即源码同一。
`b3b33b2` 的成功运行使用 near `c476bd48…`、far `9b06f5c4…`，srcversion 同为
`62E75E1C`，与本次的差异只在构建路径。

## 2. 配置与测试方法

两端 `debug=2`；near 先写 global ports，far 先写 config、near 后写 config。

```
# near
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff

# far
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

- global ports：near `30000-30099`，far `31000-31099`。
- near 加 `ip rule add to 45.11.1.62/32 lookup main pref 100`；`ip route get`
  走 `wlp4s0`（非 mihomo）。
- HND0 sink：Python 单连接，`127.0.0.1:5201`，读满 16,777,216 B 后再要求一次
  `recv` 返回 EOF；结果写 JSON。
- near sender：Python 连接 `127.0.0.1:9001`，发 16,777,216 B，
  `shutdown(SHUT_WR)`，然后读到 EOF。
- 轮询：两端每 0.2 s 采集 `/proc/net/tbtun/{config,stats,peers,tunnels,ports}`
  与 `/proc/stat`。

## 3. 失败签名

### 3.1 sink 与 sender

- sink 日志只有一行：`listening 127.0.0.1:5201 expect=16777216 pid=887436
  at=1793923326.921504`；**没有 accept**。far 从未向 target 建连。
- sender 结果：
  `sent=6029312 expect=16777216 recv=0 eof=0
  err="BrokenPipeError(32, 'Broken pipe')" secs=15.005089`
  （`t0=1793923353.479695`，`t1=1793923368.484784`）。
  只写出 6,029,312 / 16,777,216 B，15.005 s 后 EPIPE。

### 3.2 near（JYSZ）

- `capture_runs=1 actor_calls=11 submit_bytes=1114059 tx=655730 wire=726073
  err=0 short=0`。
- `capture_runs=1`、`submit_bytes=1,114,059` 接近 `txq_max=1,114,400`：
  capture 在 txq 满后停了；sender 写出的 6,029,312 B 中约 4.9 MB 没有进入
  tbtun（在 public socket 队列或 sender 发送缓冲里）。
- `live rxq=0 txq=0 pub_state=1 pub_wmem=0 pub_una=2039359303
  pub_nxt=2039359303 car_wmem=788002`。
- `c3 n=1 up=0 lost=1 replaced=1 fbk_tx=1352 fbk_rx=0 retx_frames=73
  retx_bytes=598016 retx_hole=73 data_tx=82 tx_ack=0 rx_ack=0
  stall_declared=1 tx_window=1048576 bdp_bytes=257587 k_x100=500
  rtt_max_us=62 txq_max=1114400`。
- `ctl close_timeouts=0 close_detaches=0`。
- `c6 holes_declared=0 holes_repaired=0 holes_unrecoverable=1
  deliver_timeouts=0 deliver_checks=0`。
- 数据流：`sid=454226345984 state=5 kill=4 fin_seq=0 fin_q=0 fin_w=0
  rst_w=1 rxf=0/0`。
- klog：`393381.333326 carrier 0 DIALED lport=30000
  peer=45.11.1.62:9000`；393399.448–393411.536 连续
  `retx wndshort seq=0 len=8192 total=8224 wnd=63488 inflight=58934…598650
  wnd_avail=0`；`393414.243865 TXSTALLKILL sid=454226345984 ack=0
  txseq=1114059 age_us=15003346`；`393414.257821 carrier stall idx=0 gen=1
  una=4213657643 nxt=4213673139 wmem=798018 snd_wnd=63488 rcv_wnd=1048288
  state=1`；`393414.550361 carrier 0 DIALED lport=30001`。

`TXSTALLKILL` 的 15.003 s 等于 `carrier_stall_ms=15000`：ddef988
`tbt_tx_wait_budget_ns()` 在 `hole_budget_ms=0` 时取
`max(5 s, 5×RTO, carrier_stall_ms)`，本次为 15 s。这是等待预算的症状，
不是 close-drain 缺陷：`fin_seq=0`、`fin_w=0`、`rst_w=1`、
`close_timeouts=0`、`close_detaches=0`、`deliver_timeouts=0`。

### 3.3 far（HND0）

- `capture_runs=0 actor_calls=0 submit_bytes=0 tx=0 wire=55`。
- `rx_work_runs=4 rx_drain_entries=1 rx_drain_bytes=55 rx_adopt_events=2
  rx_records=1 rx_ready_calls=2 rx_reads=4 rx_read_bytes=55 rx_read_zero=3
  rx_delivered=0`。
- `streams_total=2 n_streams=0`。
- `c3 n=1 up=0 lost=2 fbk_tx=0 fbk_rx=0 retx_frames=0 data_tx=0
  tx_ack=0 rx_ack=0 tx_window=1048576 bdp_bytes=1376 k_x100=500
  rtt_max_us=259470`。
- `ctl close_timeouts=0 close_detaches=0`；`c6 holes_unrecoverable=0
  deliver_timeouts=0`。
- klog：`147063.893010 loaded`；`147072.877807 carrier ACCEPTED lport=9000
  rport=44798, awaiting HELLO`；`147105.875303 carrier ACCEPTED lport=9000
  rport=30001, awaiting HELLO`。没有 join/FBK 日志，也没有 HELLO 解析错误行。
- far peer（1793923335.986）：`state=up rtt_us=257382 recv_cnt=0
  tx_frames=1 tx_bytes=55`；1793923337.170：`rtt_us=259470 recv_cnt=0
  tx_frames=1 tx_bytes=55`。TCP 载体是 up 的，但 far 只发出了自己的
  HELLO，没有收到 near 的 FBK（`recv_cnt` 来自 FBK，与 far `fbk_rx=0`
  一致）；far 的应用侧读取只有最终的 55 B。

### 3.4 关键计数对照

| 项 | near | far |
|---|---|---|
| `tx_ack` / `rx_ack` | 0 / 0 | 0 / 0 |
| `fbk_rx` / `fbk_tx` | 0 / 1352 | 0 / 0 |
| `rx_read_bytes` | 55 | 55 |
| `lost` / `replaced` | 1 / 1 | 2 / 0 |
| `close_timeouts` / `close_detaches` | 0 / 0 | 0 / 0 |
| `deliver_timeouts` | 0 | 0 |
| `holes_unrecoverable` | 1 | 0 |
| 目标连接 | — | sink 未 accept |

far 的 `fbk_tx=0` 说明 far 在整次传输中没有发出任何反馈；near 的
`tx_ack=0`、`fbk_rx=0` 与此一致。sink 未 accept 说明 far 从未把数据流
OPEN 到 target。

## 4. 时间线

wall 为 CST；`epoch` 来自 poll/sender JSON；near/far klog 是各自主机的
monotonic，不能跨主机比较。

| wall (CST) | epoch | near klog | far klog | 事件 |
|---|---|---|---|---|
| 08:02:06.9 | 1793923326.92 | — | — | sink 在 127.0.0.1:5201 listening |
| ≈08:02:1x | — | 393381.021 loaded；393381.333 DIALED lport=30000 | 147063.893 loaded；147072.878 ACCEPTED rport=44798 | 载体建立；NAT 把 near 的 30000 映射为 44798 |
| 08:02:15.58 | 1793923335.58 | peer up；tx_frames=1 tx_bytes=55（HELLO） | — | near 的 HELLO 已写出 |
| 08:02:15.99 | 1793923335.99 | — | peer rtt=257382us；recv_cnt=0；rx_work_runs=2 | far accept 时的 work 已跑完；此后不再被唤醒；far 应用侧读 0 |
| 08:02:16.20 | 1793923336.20 | rx_read_bytes=55 | — | near 读到 far 的 HELLO |
| 08:02:33.48 | 1793923353.48 | — | — | sender 开始发 16 MiB |
| 08:02:48.48 | 1793923368.48 | 393414.244 TXSTALLKILL age=15.003s；393414.550 DIALED 30001 | — | sender 在 6,029,312 B 处 EPIPE；near 杀死数据流 |
| 08:02:49.08 | 1793923369.08 | — | rx_work_runs=3 | near 载体已死后，far 的 rx work 才再次运行 |
| 08:03:09.09 | 1793923389.09 | — | rx_work_runs=4 | far 读到 0/EOF（rx_read_zero=3） |
| ≈08:04–08:05 | — | del 1 + rmmod rc 0 | del 1 + rmmod rc 0 | 收尾；两端无 tbtun、无监听，HND0 服务 active |

决定性的一行是 far 的 `rx_work_runs`：在 08:02:15.99 到 08:02:49.08 的
约 33 s 里一直是 2，而 near 在这段时间向载体写入了 726,073 B
（`wire=726073`，TCP `wmem=798018`）。far 的 `rx_read_bytes` 没有增长。

## 5. [INFERENCE]

1. far 的 accepted carrier 最终只读到 55 B（`rx_records=1`、
   `rx_read_bytes=55`），但整次传输 `fbk_tx=0`，且没有 OPEN target，
   sink 未 accept。near 侧对应 `tx_ack=0`、`fbk_rx=0`。已删日志无法确定
   这 55 B 是 accept 时读到的，还是 near 关闭后第 3 次 work 才读到的；
   两种情况下 far 都没有在传输窗口内消费 DATA。两端 HELLO 交换不对称：
   near 读到了 far 的 HELLO（并给出 62us 的异常 RTT 样本，far 侧为
   257–259ms），far 没有对 near 的后续 DATA 产生任何应用侧读取。
2. far 的 rx work 在 accept 后停止调度：`rx_work_runs` 在传输窗口内保持 2，
   直到 near 的载体因 15 s 无进展被宣告丢失/关闭后才变成 3、4，而这两次
   读到的是 0/EOF。near 写入载体的 DATA 没有到达 far 的应用侧
   （`rx_read_bytes` 不增长）；这些字节要么还在 near 的 TCP 发送队列
   （`wmem=798,018`），要么已在 far 的接收队列但未被 `w_rx` 读取。
   near 的 `retx wndshort`（`wnd=63488`，`inflight` 增长到 598,650，
   `wnd_avail=0`）是这条路径的下游现象。
3. ddef988 的 `tbt_car_poll_work()`（`tbt_carrier.c:235`）只在 carrier 仍是
   `provisional` 时做 50 ms 救援：若
   `copied_seq != rcv_nxt` 或 `parser->have`，就 `schedule_work(&c->w_rx)`
   （`:263-273`）。若 HELLO 尚未读走，`provisional` 仍为真，这条救援本应
   在 50 ms 内重新调度；若 HELLO 已完成 join，`provisional` 清除，数据
   阶段改由 `sk_data_ready` 的 `tbt_car_ready()`（`:72-86`）调度。
   本次 `rx_work_runs` 在传输窗口内保持 2，说明上述两条路径都没有再次
   调度 `w_rx`；这是“accept/join 侧唤醒丢失”的直接观测，但具体是
   `data_ready` 未触发、`w_rx` 早退，还是 `w_poll` 未再武装，已删日志
   无法区分。
4. 由于 far 的 `w_rx` 不运行，`paused` 检查、`tbt_rx_parse_pending()` 和
   `tcp_read_sock()` 都没有机会执行；near 的 15 s 等待预算到期后
   `TXSTALLKILL`，数据流以 `rst_w=1` 结束。near 的
   `holes_unrecoverable=1` 属于 far→near 方向，与 sink 数据方向无关。
5. 候选丢失点：(a) accept 时的 `schedule_work(&c->w_rx)` 在 HELLO 入队
   之前跑完，且 `sk_data_ready` 没有再次触发；(b) `w_poll` 的 50 ms 救援
   没有重新武装或没有看到 `copied_seq != rcv_nxt`；(c) `w_rx` 运行了但
   在 `paused`/`lock_sock`/`tbt_rx_parse_pending()` 处早退且没有重排；
   (d) workqueue/锁饥饿把 `w_rx`/`w_poll` 延迟了 30 s 以上。A3 修 accept
   侧唤醒时，建议在 accept/join 之后继续覆盖数据阶段，并记录
   `rx_work_runs`、`copied_seq`/`rcv_nxt`、`parser->have`、`paused`、
   `w_rx`/`w_poll` 的调度与开始时间；只在 accept 时补一次
   `schedule_work` 不足以证明 join 后的数据路径也被覆盖。
6. 同源 ddef988 的 `b3b33b2` 运行通过（sink 16 MiB、far
   `rx_delivered=16777216`、timeouts=0），说明这不是确定性规格错误，而是
   时序窗口；一次通过不能排除本记录中的失败。

## 6. 复现条件与 A3 验收

- 推测的复现条件（本次时序）：ddef988 同源 per-end 构建；near 在 JYSZ
  主机（公网 NAT，RTT ≈218 ms），far 在 HND0；near config 写入后立即
  DIAL，far accept 的 work 先跑、HELLO 随后到达（推断）；随后 sender 发
  16 MiB。
- 该失败是间歇性的；不要用单次通过判定修复。A3 修复 accept 侧唤醒后，
  连续 5 次 sink 16 MiB 全部满足以下条件才算 5/5：
  `sink bytes=16777216`、`eof=1`、far `rx_delivered=16777216`、
  far `holes_unrecoverable=0`、far `fbk_rx>0`、`carrier_lost=0`、
  `lost=0`、`close_timeouts=0`、`deliver_timeouts=0`、near
  `fin_seq=16777216`、`fin_w=1`、`rst_w=0`，且 `W≈750 KB`
  （near `tx_window`）与无 `TXSTALLKILL`。
- 本记录**不是 gate 通过**，不替代 `b3b33b2` 的通过证据；它只说明 accept
  侧仍有残余时序窗口，供 A3 与 5/5 复跑参考。

## 7. 证据状态

- 本次失败的原始 poll/klog/sink/sender 文件已按清理要求删除；HND0 与
  JYSZ 均已卸载模块、删除临时目录、恢复 ip rule，HND0
  frps/caddy/hysteria-server/xray 保持 active。
- 交叉引用：goal-integrator 的 T0 文档 `9e6c5d8` 已记录同一观察；通过证据
  见 w-hnd0 `b3b33b2` 的 `review/wan-experiment/t1-n1-auto-rx1m-sink/`。
