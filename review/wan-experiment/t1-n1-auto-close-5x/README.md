# HND0 N=1 close 5x — variant index

T1 close-drain 的 5 连跑，源码 `w-window 4c58b0e`（父 ddef988），两端 per-end
构建，`srcversion=10C45DCB900C5C78F725596`。两个 sender 变体分目录保存：

| 子目录 | sender | 状态 | 说明 |
|---|---|---|---|
| `close/` | `adv_probe sendclose 9001 16 1`（16 MiB 后 `close()`） | **5/5 PASS** | 本轮；同一 tunnel 连续 5 轮，每轮 sink 重启 |
| `shutdown-wr/` | 16 MiB 后 `shutdown(SHUT_WR)`，再读 EOF | 由 hnd0-5x 独立跑 | 与 close 变体互补；结果见该子目录 |

`close/` 的门禁：每轮 sink `bytes=16777216 eof=1`；far `rx_delivered`
每轮 +16,777,216；near `fin_seq=16777216 fin_w=1`；far `fin_w=1 rxf=1/1`；
无 RST；`deliver_timeouts=0`、`close_timeouts=0`、`close_detaches=0`；
near/far live txq=0；holes=0、lost/carrier_lost=0；far `up=1 lost=0
join_bad=0`；无 TXSTALLKILL。详见 `close/README.md`。

单次 sink run（ddef988）证据在 `../t1-n1-auto-rx1m-sink/`（commit b3b33b2），
含 iperf3 16 MiB 吞吐/rc 记录。

不合并/push。
