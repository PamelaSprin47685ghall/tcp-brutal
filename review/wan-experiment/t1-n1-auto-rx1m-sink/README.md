# HND0 N=1 auto — ddef988 close-drain, sink byte-exact (2026-11-06)

## 结论

- **sink byte-exact PASS**：`SINK_BULK bytes=16777216 eof=1`。
- near `fin_seq=16777216 fin_w=1 rst_w=0 rxf=1/1`，sink 结束后 `live txq=0`；
  far `rx_delivered=16777216`，`deliver_timeouts=0`、`close_timeouts=0`、
  `close_detaches=0`，`live txq=0`。
- iperf3 `rc=0`，只作吞吐/信息；receiver 11,796,480 B 不是 byte-exact 判据
  （iperf3 server 在 TEST_END 主动 close 数据 socket，far 数据流 `rst_w=1`）。
- T1 的 sink close-drain 门禁通过；iperf3 吞吐受 window-verify QEMU 回归负载影响。

## 源码与模块 invariant

- `w-window ddef988f57af8a49d1da04c3b5bd374fa59cacbd`（父 f8023a2）
- `git archive ddef988 | sha256sum` =
  `7da4624b0ba63e5a04922e4024fef34aedda37243593f1db649fa985edfa514b`
- 两端同源、各自构建，`srcversion` 必须同为 `62E75E1CE05E5C4887C9AAA`：

| 端 | kernel | ko sha256 | srcversion | vermagic |
|---|---|---|---|---|
| near JYSZ | 7.2.9-1-MANJARO | `c476bd480d658be5e5607c043b58d4116fd469fdcb618d53a65c2e1ea3a43a7a` | `62E75E1C…` | 7.2.9-1-MANJARO |
| far HND0 | 7.0.0-42-generic | `9b06f5c4eb2b1f32ccdf35c7fe296b788eff1cd157724930309487c11acb0b13` | `62E75E1C…` | 7.0.0-42-generic |

`adv_probe`：near `c03ea90a…`（`-Werror` 直接过）；far `c4daa279…`
（Ubuntu glibc 的 `warn_unused_result` 使 `-Werror` 失败，改用
`-Werror -Wno-error=unused-result`；功能相同）。两端 adv_probe 源码同一份 ddef988。

## 配置（两端）

```
# near
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff

# far
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=1 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=1048576 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

- insmod `debug=2`；ports 快照见 `ports-{near,far}.txt`。
- near 加 `ip rule add to 45.11.1.62/32 lookup main pref 100`。

## Sink run（byte-exact 门禁）

- far：`adv_probe sink 5201 5`（systemd `tbtun-sink-5201`，5 ms/64 KiB 慢读，
  保证 close 时队列非空）；near root：`adv_probe sendclose 9001 16 1`。
- sender：`SENDCLOSE_OK bytes=16777216 txq_after=1113052`
  （close 时 near txq 非空，正是被测场景）。
- sink：`SINK_BULK bytes=16777216 eof=1`；没有 `SINK_CTRL`（单流场景）。
- FIN/RST 时序（`fin-timeline-{near,far}.txt`）：
  - near `1793923147.326`：`fin_seq=16777216 fin_q=1 fin_w=0 txq=1036242`
    —— FIN 已排队，数据仍在 drain。
  - near `1793923148.639`：`fin_w=1 txq=0 rxf=0/0` —— 数据排空后才写 FIN。
  - near `1793923150.931`：`fin_w=1 rxf=1/1 txq=0`；`1793923151.039` stream-gone。
  - far `1793923148.873`：`fin_w=1 rxf=1/1 tgt_state=7 rx_delivered=16777216 txq=0`；
    `1793923150.918` stream-gone。
  - 全程 `rst_w=0`；`close_timeouts=0`、`close_detaches=0`、`deliver_timeouts=0`。

关键计数（sink run）：

| 项 | near | far |
|---|---|---|
| `tx_ack` / `rx_ack` | 16777216 | 16777216 |
| `rx_delivered` | — | 16777216 |
| `fbk_rx` | 1110 | 1132 |
| `lost` / `carrier_lost` | 0 / 0 | 0 / 0 |
| `tx_window` | 1048576（cap） | 174914 |
| `bdp_bytes` / `k_x100` | 1190818 / 200 | 71264 / 200 |
| `data_tx` / `retx_frames` | 2070 / 13 | 0 / 0 |
| `holes_unrecoverable` | 0 | 0 |
| `deliver_timeouts` | 0 | 0 |
| `close_timeouts` / `close_detaches` | 0 / 0 | 0 / 0 |
| `live txq` after | 0 | 0 |

- near `reconcile live_bytes=16998513`（含 13 个 retx frame/overhead；far
  `rx_delivered` 精确 16,777,216）。
- CPU（sink 窗口 1793923138.266..1793923151.150，约 12.9 s）：
  near **10.57%**（user 8.36/sys 1.64/irq 0.29/softirq 0.27）；
  far **27.93%**（user 8.58/sys 17.89/softirq 1.46）。

## iperf3 16MiB（只报吞吐/rc）

- `rc=0`；sender 16,777,216 B / 6.001 s = 22.37 Mbit/s；
  receiver 11,796,480 B（70.3%）/ 6.48 s = 14.56 Mbit/s；server 日志 11.2 MiB。
- 原因：iperf3 server 在 TEST_END 主动 close 数据 socket；far 数据流
  `sid=781046513666 state=5 kill=5 rst_w=1`，near 残留 `txq=1113317`。
  这是 iperf3 特有 close 路径，不是 sink 门禁；控制流
  `sid=781046513665 fin_seq=338 fin_w=1 rxf=1/1` 正常。
- 计数：near `tx_window=1048576`、`bdp=1214152`、`k=200`、`fbk_rx=2565`、
  `holes=0`、`lost=0`；far `tx_window=218598`、`bdp=99699`、`fbk_rx=2684`、
  `holes=0`、`carrier_lost=0`、`lost=0`。
- window-verify 的 QEMU 回归同时在 near 主机运行，吞吐数字偏噪；不作门禁。
- CPU（iperf3 窗口 6.0 s）：near 10.17%，far 75.34%（短窗口，含 iperf3/tunnel）。

## 收尾

- 两端 `del 1` + `rmmod tbtun`；HND0 `tbtun-iperf3-5201`、`tbtun-sink-5201`、
  `tbtun-poll-far`、`tbtun-n1-lock` 全部停止并 reset-failed；
  `/tmp/tbtun-hnd0.lock` 删除；near `ip rule pref 100` 删除。
- HND0 frps/caddy/hysteria-server/xray 全 active；两端无 tbtun、
  无 9000/9001/5201 监听；`/tmp/tbtun-t1-close` 临时目录清理。
- 未跑 256，未合并/push。

## 文件

- `sink.out`、`sendclose-near.txt`：sink/sender 原始输出。
- `config-{near,far}.txt`、`ports-{near,far}.txt`：运行前快照。
- `{near,far}-before.txt`、`{near,far}-after.txt`（sink）、
  `{near,far}-after-iperf3.txt`：stats/peers/tunnels/config/ports/cpu。
- `poll-{near,far}.txt`：0.1 s 轮询原始流（sink + iperf3）。
- `streams-{near,far}-sink.txt`、`fin-timeline-{near,far}.txt`：sink 单流
  per-stream 行与 FIN/RST 时序。
- `streams-{near,far}-iperf3.txt`：iperf3 两条 stream 的 per-stream 行。
- `klog-{near,far}-run.txt`：本模块实例的 klog（filtered，无 TXSTALLKILL/
  RXHOLEKILL）；`klog-{near,far}-iperf3.txt` / `klog-teardown-*.txt`：全量/收尾。
- `iperf3-16MiB.json`、`iperf3-16MiB.err`、`iperf3-server.log`、`iperf3-summary.txt`、
  `rc.txt`、`start.ts`、`end.ts`。
- `module-{near,far}.txt`、`source.txt`、`cpu-sink-{near,far}.txt`、
  `cpu-iperf3-{near,far}.txt`、`cpu-summary.txt`。
