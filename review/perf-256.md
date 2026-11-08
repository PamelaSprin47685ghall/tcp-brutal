# 256-carrier 性能归因（fix39，2026-11-08）

分析对象：`00d34cf`（fix39）。数据：
`review/wan-experiment/arm64/steady-256-20261108/`（60 s 稳态）与
`benches/baseline/remote-detail.csv`（0.2.1 fix31 的 5 s 短测）。
本文只读分析，未改 `tbt_fec_data.c` / `tbt_carrier.c` / `tbt_tx.c`。

## 结论

1. **0.2.1 的 86.8% 不是 CPU 或 TX 队列瓶颈**，而是 FEC 2x 冗余加
   10.9% 额外 wire 的算术结果。TX 用户态路径（per-record 分配、HTTP
   framing、FEC encode、crypto、CRC、缓冲复用）合计 <5% 单核。
2. **fix39 的 60 s 23.9 Mbit/s 是另一个问题**：链路容量、丢包与
   super-block 完成节奏，不是 TX 代码。它不能用来解释 0.2.1 的 86.8%。
3. **`txq_max` 16.84 MB 与 FEC 数据无关**，是 ARQ 窗口的派生值；
   现场“满”是读数误读。
4. 达标 90% 只差 **+3.7% app**：retx 4.9%→约 1.3%，或 wire +3.7%，
   任一即可。真正的大杠杆是 credit 窗口、capture ring、丢包/修复延迟
   与自适应 FEC 形状，不是逐 record 微优化。

## 1. 86.8% 的分解

0.2.1（fix31）5 s 短测，256 carriers、n=4 m=4、http-kal、crypto=on：

| 指标 | 直连 P32 | 隧道 256 | 比值 |
|---|---:|---:|---:|
| app Mbit/s | 224.2 | 97.3 | 43.4% |
| wire Mbit/s | 224.2 | 215.8 | 96.3% |
| wire/app | 1.000 | 2.218 | — |
| far CPU（单核%） | 132.5 | 146.6 | — |
| near CPU（单核%） | 76.1 | 90.6 | — |

- 目标：app ≥ 直连/2 × 90% = 224.2/2 × 0.9 = **100.9 Mbit/s**。
- 当前 97.3 / 112.1 = **86.8%**，差 **+3.7%**。
- 若 wire 不变，app/wire 要从 45.1% 提到 46.8%，即 wire/app
  2.218→**2.139（-3.6%）**。这等价于 retx 4.9%→约 1.3%。
- 若 overhead 不变，wire 要从 215.8 提到 **223.8（+3.7%）**，即隧道
  wire 追平 32 条直连的聚合。256 carriers 有这个潜力。
- 两条路任一即可到 90%；同时做可到 93-95%。

固定 2x 是 n=4 m=4 的理论下限。**在 n=4 m=4 下，app 永远不可能超过
wire 的 50%**；要大幅超过 90%，只能降冗余（P5）。

## 2. 长测与短测不可比

fix39 60 s measure 窗口：

| 指标 | 5 s 短测（fix31） | 60 s 长测（fix39） | 长测/短测 |
|---|---:|---:|---:|
| app Mbit/s | 97.3 | 23.9 | 24.6% |
| wire Mbit/s | 215.8 | 48.5 | 22.5% |
| wire/app | 2.218 | 2.027 | — |
| retx（app 占比） | 4.9% | 3.7% | — |
| far CPU | 1.466 核 | 1.265 核 | 86% |
| near CPU | 0.906 核 | 0.758 核 | 84% |

- 长测 wire 只有短测的 **22.5%**，但 far CPU 只低 14%。每 Mbit 的
  far CPU 从 0.0151 核升到 **0.026 核**：长测的 CPU 大量花在等待、
  重传、空转，而不是有效发送。
- 长测 wire/app 2.027 已接近 2.0 理论下限；framing/重传合计只有
  1.4%。**长测的 overhead 不是问题，wire 总量才是。**
- 长测没有同期直连 60 s 对照。若链路当时只有约 50 Mbit/s，23.9
  Mbit/s app 已是链路极限，与代码无关；若直连仍有 200 Mbit/s，
  则是 credit/HOL 问题。**下一次长测必须带直连对照**（见 §7）。

## 3. far CPU 归属

fix39 60 s 窗口 `/proc/stat` 差值（far 是 2 核）：

| 项 | 核 | 说明 |
|---|---:|---|
| user | 0.521 | far-raw-serve.py（1 GiB/84 s ≈ 12 MB/s 的 Python HTTP 源） |
| system | 0.431 | 256 条 TCP 的 sendmsg/skb/内存分配 + Python syscall |
| softirq | 0.314 | 256 条 TCP 的收包/ACK |
| **busy** | **1.265** | 2 核的 **65.7%** |

near 是 8 核：busy 0.758 核 = 9.7%；user 0.356、system 0.254、
softirq 0.123。

关键读数：

- far 的 user 0.52 核是**源应用**，不是 tbtun。短测直连的 far CPU
  已是 132.5%（1.325 核），说明源本身在 28 MB/s 下就占 1.3 核；
  隧道 256 只多 **0.14 核**。
- tbtun 的用户态 TX 路径按微基准只有 0.01-0.02 核（735 record/s ×
  约 13 µs ≈ 1% 单核）。system/softirq 的 0.75 核主要是 256 条
  carrier 的 TCP 栈成本，不是 FEC/framing。
- 结论：**CPU 有约 0.7 核空闲，不是 256 的吞吐瓶颈**。用 perf 采样
  确认 system/softirq 的归属之前，不要按 CPU 假设改 TX 代码。

## 4. txq_max 误读

```
tbt_tx.c:824   txq_max = tx_window + 8 * TBT_CHUNK_MAX
               = 16,777,216 + 65,824 = 16,843,040 B
```

- 16 MiB 来自 `tbt_tx_reorder_floor(256) = 256*8*8192`，同时是
  auto window 的 floor 与 cap，所以被钉死。
- **FEC 数据不进 `tx_q`**（`tbt_tx.c:3603-3604` 直接转
  `tbt_fec_tx_submit`），`tx_queued` 在稳态只累计 HELLO/OPEN/FIN/PAD。
  `far-after` 的 live `txq=0`。
- FEC 的真实队列：`fec_outq` 结束时只剩 5 个 carrier 有 90-420 KB；
  `fec_replay_skip=0`、`fec_outq_drop=0`、`fec_replay_high_water=0`。
- 因此“txq_max 16.8 MB 满”不是发送侧瓶颈。要核实队列，应看
  `/proc/net/tbtun/fec` 的 `fec_outq_bytes`、`fec_out_ctl_bytes`、
  `fec_replay_bytes`。

## 5. 每 record 成本与微基准

每 record（8240 B）在 TX 路径上的工作量：

- 8 次 8 KiB 级拷贝：ring→pkt→rec→replay→out→batch→skb。
- 1 次 `kmalloc(8288, GFP_ATOMIC)`，落 kmalloc-16k（每 record 实占
  16 KiB）。
- FEC：GF(2) RLNC，n4m4 w8 每 block 4 次 memset + 12 次 XOR（各
  8192 B），7.0x 内存流量；`fec.encode.n4m4.w8.k4.s1500` 4495 ns/组
  （32 KiB app）≈ 1.1 µs/record。
- crypto：ChaCha8，每 record 复制 64 B 状态并改 counter；8192 B
  约 6.0 µs；无 SIMD、无 AEAD tag。
- CRC：内核走 `crc32c()`（x86 SSE4.2 硬件加速，约 1 µs/record）。
  MICROBENCH 的 310 MB/s 是 host 查表回退，**不代表真机**。
- HTTP：chunked，每 record 只多 8 B（0.097%）；响应头每 carrier
  一次 153 B。

合计约 10-13 µs/record：短测 3400 record/s ≈ 3.4-4.4% 单核；长测
735 record/s ≈ 0.7-1%。

`host/bench_txpath.c`（本仓库新增）实测：

| 行 | ns/op | 折算 |
|---|---:|---:|
| `txpath.push_alloc.8k` | 63.2 | 63.2 ns/record |
| `txpath.batch_alloc8.8k` | 688.5/batch | 86.1 ns/record |
| `txpath.batch_reuse8.8k` | 678.4/batch | 84.8 ns/record |
| `txpath.chunk_encode.8k` | 73.5 | 73.5 ns/record |
| `txpath.copy.8k` | 67.9 | memcpy 下界 |

- 当前 = push_alloc + batch_alloc/8 = **149.3 ns/record**
  = 0.05% 单核 @3400 record/s。
- 缓冲复用 = **84.8 ns/record**，节省 64.5 ns = **0.02% 单核**。
- 结论：per-record 分配/HTTP framing/缓冲复用这个“最小优化”不值得
  做，吞吐无感。host malloc 比内核 kmalloc 快且缓存热；即使放大
  20 倍也只有约 1% 单核。

## 6. 优化清单

| 优先级 | 项 | 改法 | 预期收益 | 风险 | 归属 |
|---|---|---|---|---|---|
| P0 | 扩大 FEC credit 与 replay 预算 | `TBT_FEC_RX_BUDGET` 16→32 MiB、`TBT_FEC_REPLAY_BUDGET` 15→30 MiB；credit 640→1536 packets（5→12 MiB）；两端同版本 | 若链路有容量，wire 6→最高 13 MB/s；若链路只有约 50 Mbit/s，收益为零（正好判定链路 vs 代码） | 中：每会话 +16 MiB arena、+15 MiB replay；两端不一致会 `fec_drop_window` | 0.3.0 压榨 |
| P1 | capture ring 1 MiB→4 MiB | `TBT_FEC_TX_RING_MAX` 1→4 MiB，或 ring=credit·S | 消除 0.17 s 级停顿回压，+10-20% 平滑吞吐 | 低：+3 MiB，首包延迟略变 | 0.3.0 压榨（单常量，收益/改动比最高） |
| P2 | 降 carrier TCP 丢包/straggler | per-carrier sndbuf/rcvbuf 512 KiB→1 MiB；或调 pacing；或减 carrier 换窗口 | retx 4.9%→2-3%，app +2-3% | 中：+128 MiB；减 carrier 降聚合 | 0.3.0 压榨 |
| P3 | NACK/修复并行与更早 parity | 允许多 block 并行 NACK；或首次 NACK 延迟半个 RTT；replay 独立配额 | 修复串行化 -30-50%，长测 +20-40% | 中高：乱序/重复/协议行为 | 0.3.0 策略 |
| P4 | TX 微优化 | out_push 预留 chunk 头、多 iovec、记录池、先查 sk_wmem 再编码、`carrier_bytes` 原子化 | CPU -3-5%；吞吐 <1% | 低-中：short-write 尾部 `c->part` 最敏感 | 0.3.0 压榨，最低优先级 |
| P5 | 自适应 FEC 降冗余 | n4m4→n8m4（1.5x）或按丢包调形（已有 adaptive-fec 设计） | app/wire 45%→67%，app +33-48% | 高：协议/形状切换/两端一致 | 0.3.0 设计变更 |

P0/P1/P4 与 P2 的缓冲调参属于“0.3.0 压榨”；P5 是形状自适应，另有
设计文档；P3 是协议策略。

## 7. 下一步实验

1. **同期直连 60 s**：同一 far 源、32 条并发 curl，确定链路容量上限。
   没有这个对照，23.9 Mbit/s 不能归因于代码。
2. **far `perf top -g` 10 s**：确认 0.43 system + 0.31 softirq 的归属
   （workqueue、tcp_sendmsg、qdisc、Python）。
3. **P0 5 s 短测 A/B**：`TBT_FEC_RX_BUDGET` 16→32 MiB +
   `TBT_FEC_REPLAY_BUDGET` 15→30 MiB（credit 5→12 MiB）。
4. **P1 5 s 短测 A/B**：`TBT_FEC_TX_RING_MAX` 1→4 MiB。
5. **P2 5 s 短测 A/B**：per-carrier sndbuf/rcvbuf 512 KiB→1 MiB。

以上 3-5 项都在 `tbt_fec_data.c` / `tbt_carrier.c` / `tbt_tx.c`，owner
是 sclc 数据面；本报告不改热点代码，等 owner 释放后再派。

## 8. 复现

```sh
make host-bench-txpath
taskset -c 2 ./host/bench_txpath
```

输出 CSV 五列，stderr 给每 record 折算与 3400 record/s 下的 CPU 占比。
