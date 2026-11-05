# HND0 生产 carrier profile：N/M/W/d 与三档配置

**日期**：2026-11-06
**代码基线**：`w-window` HEAD `45cb119` + 工作树未提交的 retx 诊断改动（worktree `/home/kunweiz/tbtun-worktrees/window`，2026-11-06）
**实测基线**：`review/wan-experiment/summary.md`（HEAD `8d676dd`），RTT 185.5-256.2 ms，均值 217.9 ms（summary.md:67）
**HND0**：45.11.1.62，2 vCPU，955 MiB，`7.0.0-42-generic` x86_64（summary.md:30）

> 第 5 节的命令由 `wan-deploy` 执行；本文件只记录，不执行构建、加载或远端命令。
>
> 行号取自 2026-11-06 的工作树；`tbt_tx.c` 的未提交改动会让行号移动，代码合入后请以符号名（`tbt_tx_window`、`tbt_late_tab`、`tbt_rx_state` 等）为准。

## 0. 一句话结论

三档里 **N=4/M=1** 与 **N=8/M=2** 可以直接配置；**N=16/M=4 不可配置**（`n+m=20 > 16`），最近可用点是 **N=12/M=4**（保留 4 个 spare）或 **N=16/M=0**（保留 16 个数据载体）；并且 `W=2N` 帧在 218 ms 下的单流上限只有 2.4-9.6 Mbit/s，**17-34 Mbit/s 只能作为 8 并发流的聚合验收目标**。

这条结论有两个直接后果：

- 本配置把发送窗口固定在 `2N` 帧（`16N` KiB），不是自动窗口。N=4/8 的 W=64/128 KiB 低于直连 BDP（约 465 KiB），单流不可能达到 17 Mbit/s。新树自己也把旧的 128 KiB 窗口认定为 WAN 结果的原因（DESIGN.md:949-967）。
- N=16/M=4 不是“调大 `max_carriers` 就能解决”的问题。`path_ack_mask` 是 16 位，ABI 上限就是 16（DESIGN.md:1004-1010）；要 20 个载体必须改线上格式，不在本次生产配置范围内。

## 1. N=16/M=4 不可行

### 1.1 硬上限

| 位置 | 事实 |
|---|---|
| `tbtun/tbt_uapi.h:113` | `#define TBT_MAX_CARRIERS_LIMIT 16`，注释写明 `path_ack_mask` 是 16 位 |
| `tbtun/tbt_parse.c:537-538` | `cfg->n + cfg->m > TBT_MAX_CARRIERS_LIMIT` 时返回 `tbt_fail(e, "n", "n + m exceeds the 16-carrier limit")` |
| `tbtun/tbt_session.c:122-125` | 建会话前再查一次 `n + m > TBT_MAX_CARRIERS_LIMIT` 与 `max_carriers` |
| `tbtun/tbt_core.c:82` | `BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16)`，注释 `path_ack_mask is u16` |
| `tbtun/tbt_core.c:34` | `max_carriers` 模块参数默认等于 `TBT_MAX_CARRIERS_LIMIT` |

`n=16 m=4` 的行会在解析阶段被整行拒绝（`-EINVAL`，该行不生效，DESIGN.md:1609-1613）。如果加载时把 `max_carriers` 调低于 16，连 `n+m=16` 也会被拒；部署前读 `/sys/module/tbtun/parameters/max_carriers` 确认。

### 1.2 替代点

| 方案 | n | m | n+m | 数据载体 | spare | W | 说明 |
|---|---:|---:|---:|---:|---:|---:|---|
| 请求 | 16 | 4 | 20 | — | — | — | **拒绝**：`n + m exceeds the 16-carrier limit` |
| P12（替代 A） | 12 | 4 | 16 | 12 | 4 | 192 KiB | 保留请求里的 4 个 spare，数据载体少 4 个 |
| P16（替代 B） | 16 | 0 | 16 | 16 | 0 | 256 KiB | 数据载体满 16，没有热 spare |

选择建议：

- **生产优先 P12（N=12/M=4）**。它与请求的 N=16/M=4 有同样的 4 个 spare，而 raw 载体在真实 WAN 上每轮都有 `lost`/`replaced`（summary.md:166：r1 为 1/5/9/8，r2 为 1/5/9/7），spare 有实际价值。
- **P16（N=16/M=0）只用于最大 N 的受控测试**。没有 spare 时，一个载体丢失只能靠同索引重新拨号替换（DESIGN.md:1218-1223），恢复期间该索引没有备用路径。
- 两档都占满 16 个端口。默认全局端口段是 40000-40063，正好 16 个（DESIGN.md:1276-1280）；同一个模块上再建第二个隧道会 `port_exhaust`。

### 1.3 吞吐含义

窗口是每条流各自的字节预算：`seq + len <= tx_ack + W`（DESIGN.md:829-830，`tbtun/tbt_tx.c:645-649`）。所以 8 条流的总 in-flight 是 `8W`，不是 `W`。

按 218 ms 的乐观 RTT 估算单流上限 `W / RTT`（[估算]，实测有效 RTT 为 234-300 ms，实际更低）：

| 方案 | W | 单流上限 | 8 流聚合上限 |
|---|---:|---:|---:|
| P4（N=4/M=1） | 64 KiB | 2.40 Mbit/s | 19.2 Mbit/s |
| P8（N=8/M=2） | 128 KiB | 4.81 Mbit/s | 38.5 Mbit/s |
| P12（N=12/M=4） | 192 KiB | 7.21 Mbit/s | 57.7 Mbit/s |
| P16（N=16/M=0） | 256 KiB | 9.62 Mbit/s | 77.0 Mbit/s |

“8 流聚合上限”是按每流窗口独立算的乐观值；会话级 `txq_max`（`W + 65824` B）和 64 帧调度窗口（DESIGN.md:1058）可能让它更低，必须实测（§6）。

直连基线是 P1/P4/P8 = 17.084/30.031/33.722 Mbit/s（summary.md:87-89）。因此：

- **单流 17 Mbit/s 在这三档下都达不到**；最高档 P16 也只有 9.6 Mbit/s。
- **8 流聚合 17-34 Mbit/s 是可达的**：P4 的 19.2 刚过 17；P8/P12/P16 的上限都高于 34，实际由链路和直连基线封顶。
- 若验收必须是单流 17 Mbit/s，W 至少要 465 KiB（`218 ms × 17.08 Mbit/s`），即 `tx_window_bytes >= 524288`；有效 RTT 300 ms 时需 `>= 655360`。同时 `rx_reorder_bytes >= W`。这与“2N 帧”规则冲突，必须二选一；见 §7 风险 4。

## 2. 定义与代码依据

### 2.1 符号

| 符号 | 含义 | 依据 |
|---|---|---|
| `N` | 数据载体数，索引 `0..n-1` 默认承载 DATA | `tbtun/tbt_session.c:1001-1002`；`tbtun/tbtun.h:457-458` |
| `M` | 热 spare 数，索引 `n..n+m-1`；连上、HELLO、不承载 DATA，需要时激活 | 同上；DESIGN.md:578-583 |
| `n+m` | 实际拨号的载体总数，硬上限 16 | `tbtun/tbt_uapi.h:113`；`tbtun/tbt_parse.c:537` |
| `d` | 每帧的额外副本数，`cfg.dup` 取 0..3；总副本 = `1+d` | `tbtun/tbt_parse.c:439-446`；DESIGN.md:583 |
| `W` | 发送窗口，字节；每条流用自己的 `tx_ack` 比较 | `tbtun/tbt_tx.c:583-616, 645-649`；DESIGN.md:829-830 |
| `R` | `rx_reorder_bytes`，重排树预算；每流一份，全会话再共享一份 | `tbtun/tbt_rx.c:270-279`；README.zh.md:172 |
| `replay` | 已 ack 帧的保留预算，默认 8 MiB；GC 按每条流比较 | `tbtun/tbt_tx.c:375-387`；DESIGN.md:2195-2198 |
| `txq_max` | 发送队列上限 = `W + 8 × TBT_CHUNK_MAX` = `W + 8×8228` B | `tbtun/tbt_tx.c:634` |
| 就绪队列 | `TBT_RX_RING_MAX` = 64 MiB/流，按负载字节计 | `tbtun/tbtun.h:179` |
| 载体固定堆 | 约 69 KiB/载体 | `tbtun/tbt_carrier.c:946-970`（[估算]，见 §4.1） |
| 载体 socket 缓冲 | 每方向 `setsockopt` 1 MiB，内核翻倍 → 2 MiB；4 MiB/载体 | `tbtun/tbt_carrier.c:1407-1436` |

### 2.2 W 的来源、下限与上限

`tbt_tx_window()` 的顺序是：显式 `cfg.tx_window_bytes` 优先；否则用 BBR 样本推 `k × mean_BDP`；没有样本时用 1 MiB 启动值；最后套两个边界（`tbtun/tbt_tx.c:583-616`）：

- 下限：`2 × TBT_PAYLOAD_MAX` = 16384 B，保证两帧能过（`tbtun/tbt_tx.c:608-611`）。
- 上限：`R = rx_reorder_bytes`（`tbtun/tbt_tx.c:612-614`）。

本配置取 `tx_window_bytes = 16384 × N`，即 `2N` 个满 8 KiB 帧：

| N | `tx_window_bytes` | 帧数（按 8 KiB） |
|---:|---:|---:|
| 4 | 65536 | 8 |
| 8 | 131072 | 16 |
| 12 | 196608 | 24 |
| 16 | 262144 | 32 |

解析器在整行解析完后检查 `tx_window_bytes <= rx_reorder_bytes`，两种键顺序都生效（`tbtun/tbt_parse.c:542-544`）；显式值超过 `R` 返回 `-EINVAL`。本文件所有档都满足：

- P4：65536 <= 262144
- P8：131072 <= 262144
- P12：196608 <= 262144
- P16：262144 == 262144（零余量，见 §7 风险 2）

两端必须配置相同的 `R`：接收端的重排预算不在线上传输，发送端只能用自己的 `R` 给自己封顶（DESIGN.md:931-935）。

### 2.3 内存对象的计数口径

- **帧对象**：`kmalloc(struct_size(frame, data, len))`（`tbtun/tbt_tx.c:76-80`）。8 KiB 负载加上帧头会跨到 16 KiB slab，所以**每 8 KiB 负载占 16 KiB 物理内存**（README.zh.md:364-368）。
- **retain 对象**：同样是 `struct_size(retain, data, len)`（`tbtun/tbt_tx.c:105`），8 KiB 负载占 16 KiB slab。
- **retain 的上界不是 8 MiB**：`cfg.replay` 只裁剪已 ack 的帧；未 ack 的帧永不淘汰（`tbtun/tbt_tx.c:389-391`；DESIGN.md:2196-2198）。所以单流上界是 `W + 8 MiB` 负载，不是 8 MiB。
- **`replay` 名为 tunnel 级，实际按流比较**：`cap = sess->tunnel->cfg.replay`，GC 里用 `st->retain_bytes > cap`（`tbtun/tbt_tx.c:376-387`），所以每条流各有一份 8 MiB 的已 ack 预算，不是全会话共享。
- **重排树是全会话共享**：每流 `st->rx_ooo_bytes > st->rx_limit`，同时 `sess->rx_ooo_total > cfg.rx_reorder_bytes` 也会 kill（`tbtun/tbt_rx.c:270-279`）。8 条流共享一个 `R`，这是 §7 风险 2 的根因。
- **就绪队列的 64 MiB 是硬上限，2 MiB 只是 FBK 的 pause 标记**：`rx_queued >= 2 MiB` 时 FBK 报 `rx_state=2`（`tbtun/tbtun.h:181`；`tbtun/tbt_tx.c:2720-2728`），发送端据此暂停重传，避免把预算烧在应用不读的流上；新 DATA 仍可继续，直到接收端在 64 MiB 时停止读载体（`tbtun/tbt_rx.c:971-973`）。这个暂停是会话级的：任一条流 `st->paused` 为真，所有载体停止读取（`tbtun/tbt_carrier.c:552-554`）。WAN 矩阵的 far `rxq` 为 3.15-3.71 MiB（summary.md:165），所以稳态按 4 MiB/流估，硬上限才是 64 MiB。

> 文档漂移：README.zh.md:171-173 写 `hole_budget_ms` 默认 500、`0` 会被拒绝，`carrier_stall_ms` 默认 3000；代码实际是两者 `0 = auto`（`tbtun/tbt_parse.c:375-379, 476-477, 499-500`）。本文件以代码为准；配置行都写显式值，不受影响。

## 3. 三档 profile 配置行

### 3.1 档位

| 档 | N | M | n+m | `tx_window_bytes` | 帧 | 用途 |
|---|---:|---:|---:|---:|---:|---|
| P4 | 4 | 1 | 5 | 65536 | 8 | 最小生产档 |
| P8 | 8 | 2 | 10 | 131072 | 16 | 中等 |
| P12 | 12 | 4 | 16 | 196608 | 24 | N=16/M=4 的替代，保留 4 spare |
| P16 | 16 | 0 | 16 | 262144 | 32 | 最大数据载体，无 spare |

第三档（请求的 N=16/M=4）给两个可用替代，所以下面有四组配置行；前两档各一组。

公共键（`portblock` 和 `nonce` 两端写法不同）：`dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000`。

`dup=0` 的理由：`dup=1` 的唯一无歧义效应是 wire 翻倍（summary.md:168, 181）；冗余由 M 提供，不靠复制。

### 3.2 P4：N=4/M=1

far（HND0）：

```text
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=4 m=1 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=65536 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

near（JYSZ）：

```text
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=1 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=65536 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff
```

### 3.3 P8：N=8/M=2

far（HND0）：

```text
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=8 m=2 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=131072 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

near（JYSZ）：

```text
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=8 m=2 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=131072 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff
```

### 3.4 P12：N=12/M=4（替代 N=16/M=4）

far（HND0）：

```text
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=12 m=4 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=196608 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

near（JYSZ）：

```text
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=12 m=4 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=196608 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff
```

### 3.5 P16：N=16/M=0（替代 N=16/M=4）

far（HND0）：

```text
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=16 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
```

near（JYSZ）：

```text
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=16 m=0 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_window_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff
```

### 3.6 写入与回读

- 行宽 329-333 B，远小于 1023 B 的行上限（DESIGN.md:1607-1608）；`echo` 追加换行后立即应用（DESIGN.md:1598-1605）。
- 每行都可以直接 `echo '<整行>' > /proc/net/tbtun/config`；第 5 节用 `cat <<'EOF'` 只是为了避免 shell 引号错误，效果相同。
- **先 far，等 3 s，再 near**。反过来的话，near 会在 far 开始监听前拨号失败。
- `nonce` 是占位值 `00112233445566778899aabbccddeeff`；生产时两端换成同一个随机 32 位 hex。nonce 明文传输，是会话标识，不是密钥（DESIGN.md:1483-1485）。
- `portblock` 只被解析、存储和渲染；真正分配端口的是全局 `/proc/net/tbtun/ports`（DESIGN.md:1472, 1276-1280）。默认 40000-40063 正好 16 个；P12/P16 会占满，第二个隧道会 `port_exhaust`。
- 回读的规范行会带上默认值并按固定顺序重排（`tbt_parse.c:850-863`）；用 `cat /proc/net/tbtun/config` 核对 `n=`、`m=`、`tx_window_bytes=`、`rx_reorder_bytes=` 四项即可。

## 4. 内存与 CPU 预算

### 4.1 单档内存

固定堆的 69 KiB 这样来（`tbtun/tbt_carrier.c:946-970`）：`parser` 8224 B、`part` 8224 B、`http_tx` 8244 B、`http_rx` >= 8224 B，四个都跨到 16 KiB slab；`http_hdr` 4096 B 占 4 KiB；`struct tbt_carrier` 约 0.6 KiB 占 1 KiB。合计 `4×16 + 4 + 1 = 69 KiB`（[估算]，最后一个 1 KiB 是结构体大小取整）。

| 档 | 载体 | 固定堆 | W/帧 | 会话 tx 队列 slab | `txq_max` | retain 上限（负载/slab） | 单流稳态* | 8 流稳态 | 8 流硬上限 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| P4 | 5 | 345 KiB | 64 KiB / 8 | 0.25 MiB | 131360 B | 8.06 / 16.1 MiB | 9.4 MiB | 95 MiB | 1174 MiB |
| P8 | 10 | 690 KiB | 128 KiB / 16 | 0.38 MiB | 196896 B | 8.13 / 16.25 MiB | 9.6 MiB | 118 MiB | 1196 MiB |
| P12 | 16 | 1.08 MiB | 192 KiB / 24 | 0.50 MiB | 262432 B | 8.19 / 16.4 MiB | 9.9 MiB | 144 MiB | 1221 MiB |
| P16 | 16 | 1.08 MiB | 256 KiB / 32 | 0.63 MiB | 327968 B | 8.25 / 16.5 MiB | 10.2 MiB | 146 MiB | 1222 MiB |

\* 单流稳态不含会话 tx 队列；会话 tx 队列在 8 流列里只计一次。

表中的“单流稳态 / 8 流稳态 / 8 流硬上限”是 [估算]，按下面的口径和 4 MiB/流就绪队列计算；固定堆、W、`txq_max`、retain 上限来自代码常量。

口径：

- **W 帧缓冲** = `2W`：`2N` 个帧 × 16 KiB slab；会话 tx 队列的实际上限是 `2×(W + 65824)`，比 `2W` 多约 128 KiB。
- **会话 tx 队列 slab** = `2 × txq_max` = `2 × (W + 65824)`：队列是全会话共享的一条 `sess->tx_q`，不是每流一份；每个 8 KiB 负载占 16 KiB slab。未提交的 retx 改动允许 hole filler 越过 `txq_max` 一帧（约 16 KiB slab），下表忽略这一帧。
- **retain 上限** = `W + 8 MiB` 负载（未 ack 的 W 永不淘汰 + 已 ack 的 8 MiB 预算），slab 再乘 2。
- **单流稳态** = retain（按每流 4.25 Mbit/s、1 s 保留 ≈ 0.5 MiB 估算）+ 就绪队列（按 4 MiB + W，slab 2×；WAN 矩阵实测 3.15-3.71 MiB）+ 重排树份额（R/8，slab 2×）+ 固定堆/8。
- **8 流稳态** = 8 × 单流 + 会话 tx 队列 + 载体 socket 缓冲计费上限（每载体 4 MiB）；固定堆已经含在“单流”里。socket 缓冲是计费上限，不是预分配（DESIGN.md:992-993）。
- **8 流硬上限** = 8 ×（retain 上限 + 64 MiB 就绪队列 × 2 + 重排树份额 + 固定堆/8）+ 会话 tx 队列 + socket 缓冲。就绪队列一项就是 8 × 128 MiB = 1024 MiB。

### 4.2 与 955 MiB 对照

- **稳态 8 流：95-146 MiB**，加内核、iperf3、sshd 后仍远低于 955 MiB，余量约 800 MiB。
- **硬上限 8 流：1174-1222 MiB，超过 955 MiB**。超出的部分几乎全部是 8 × 128 MiB 的就绪队列；`retain` 上限只占 16 MiB/流。
- 就绪队列的 64 MiB 是硬上限（`tbtun/tbt_rx.c:971-973`）；稳态占用取决于应用读取速度（WAN 矩阵实测 3.15-3.71 MiB，summary.md:165）。只有应用停止读取时才会逼近 64 MiB。
- **建议上限**（[估算]，按 955 MiB 预留 150 MiB 给内核/应用）：
  - 若应用可能无限期停止读取：**最多 4 条并发流**。4 × 145 MiB + 固定堆 + socket 缓冲 ≈ 600-645 MiB，留 300 MiB 以上。
  - 若要跑 8 条流：**每条流的未读积压不超过 32 MiB 负载（64 MiB 物理）**。8 × 64 MiB + 其余 ≈ 660-710 MiB，留 240 MiB 以上。
  - `TBT_RX_RING_MAX` 是编译期常量（`tbtun/tbtun.h:179`），不能按隧道配置；上面是运行/运维约束，不是配置键。

### 4.3 CPU

`benches/` 的实测（QEMU，2 vCPU 客户机，10 Mbit/载体）：

| N | app Mbit/s | sys+softirq % | kworker % | 合计 % of 1 vCPU | 2 vCPU 占用 |
|---:|---:|---:|---:|---:|---:|
| 4 | 9.20 | 17.7 | 2.7 | 20.4 | 10.2% |
| 8 | 18.47 | 20.8 | 4.3 | 25.1 | 12.6% |
| 16 | 34.18 | 22.1 | 10.3 | 32.4 | 16.2% |

依据：N=4/8 行在 `benches/README.md:234-235`；N=16 行在 `benches/results/20261105T050727-scale-rec.csv:2`（`app_mbit=34.18`、`cpu_sys_pct=13.6`、`cpu_softirq_pct=7.5`、`cpu_total_pct=22.1`、`kworker_pct=10.3`）。注意 README 的 N=16 缩放表（`benches/README.md:356`）只有吞吐列，没有 CPU 列；CPU 要从 CSV 读。

- 按 17-34 Mbit/s 线性折算：**24-32% of one vCPU**（[估算]），即 2 vCPU 机器的 12-16%，余量约 1.7 vCPU。
- 该 N=16 行产生于窗口重写前（`txq_max=65536`，旧固定队列），只作量级参考；新树窗口更大，但每字节拷贝次数不变。
- 模块无加密、无隧道校验和；模块内每字节拷贝 2 次（DESIGN.md:192-195, 2202-2206），加上公共 socket 与载体 TCP 的入队/出队，整条路径约 3-4 次（[估算]）。
- HND0 在 WAN 矩阵期间的 far load 为 0.19-0.73（summary.md:102），iperf3 进程 CPU 0.17-0.56%（summary.md:133）；2 vCPU 在 17-34 Mbit/s 不是瓶颈。

## 5. HND0 构建与部署（`wan-deploy` 执行）

**执行者：`wan-deploy`。本文件不执行以下任何命令。**

前置条件：

- HND0 root SSH 可用；`/lib/modules/$(uname -r)/build` 存在（WAN 部署已验证，deploy/commands.md:30-45）。
- near 与 far 使用同一 commit；两端 `rx_reorder_bytes` 必须相同（DESIGN.md:931-935），否则发送端会用自己的 `R` 给自己封顶。
- 若 HND0 已加载旧模块，先 `rmmod tbtun`（会清掉现有隧道）。

```sh
# 1) 同步源码到 /tmp/tbtun-exp（在 JYSZ 执行）
rsync -a --exclude=.git --exclude='*.o' --exclude='*.ko' --exclude=review \
      --exclude='*.cmd' --exclude=.tmp_versions \
      /home/kunweiz/tbtun-worktrees/window/ root@45.11.1.62:/tmp/tbtun-exp/

# 2) 构建（HND0；-j1 避免 955 MiB 上并发编译 OOM）
ssh root@45.11.1.62 'cd /tmp/tbtun-exp && rm -f Module.symvers && \
  make -C /lib/modules/$(uname -r)/build M=$PWD PAHOLE=false -j1 modules'

# 3) 加载并确认
ssh root@45.11.1.62 'cd /tmp/tbtun-exp && insmod ./tbtun.ko && \
  lsmod | grep tbtun && ls /proc/net/tbtun/ && \
  cat /sys/module/tbtun/parameters/max_carriers'
```

写入配置（far 先，near 后；把 §3 对应档的整行原样放入）：

```sh
# far（HND0）
ssh root@45.11.1.62 'cat > /proc/net/tbtun/config' <<'EOF'
<§3 的 far 行>
EOF

sleep 3

# near（JYSZ，需 root）
cat > /proc/net/tbtun/config <<'EOF'
<§3 的 near 行>
EOF
```

回读与验证：

```sh
# 两端规范配置
ssh root@45.11.1.62 'cat /proc/net/tbtun/config; cat /proc/net/tbtun/ports'
cat /proc/net/tbtun/config
cat /proc/net/tbtun/ports

# 两端统计：c3 有 tx_window/lost/reorder_overflow，c6 有 holes_unrecoverable
ssh root@45.11.1.62 "grep ' c3 ' /proc/net/tbtun/stats; grep ' c6 ' /proc/net/tbtun/stats; cat /proc/net/tbtun/peers"
grep ' c3 ' /proc/net/tbtun/stats
grep ' c6 ' /proc/net/tbtun/stats
cat /proc/net/tbtun/peers

# 模块日志与端口
ssh root@45.11.1.62 'dmesg | tail -20'
dmesg | tail -20
```

检查点：

- `config` 回读里 `n`、`m`、`tx_window_bytes`、`rx_reorder_bytes` 与所选档一致。
- `c3` 的 `tx_window` 等于 `16384×N`，`txq_max` 等于 `tx_window + 65824`，`up` 等于 `n+m`。
- `c3` 的 `lost=0`、`replaced=0`、`reorder_overflow=0`；`c6` 的 `holes_unrecoverable=0`。
- `ports` 至少有 `n+m` 个可用端口；P12/P16 用默认 40000-40063 时正好占满。

流量与收尾（验收运行见 §6）：

```sh
# HND0 target（transient unit，SSH 退出后仍存活）
# 若 unit 已存在，先 systemctl stop tbtun-iperf3-5201
systemd-run --unit=tbtun-iperf3-5201 \
  --property=StandardOutput=file:/tmp/tbtun-exp/iperf3-server.log \
  --property=StandardError=file:/tmp/tbtun-exp/iperf3-server.log \
  /usr/bin/iperf3 -s -B 127.0.0.1 -p 5201

# JYSZ 客户端：8 并发流，60 s
/tmp/iperf3-local/bin/iperf3 -c 127.0.0.1 -p 9001 -t 60 -P 8 --json > /tmp/iperf-p8.json

# 收尾
printf 'del 1\n' | tee /proc/net/tbtun/config
ssh root@45.11.1.62 'printf "del 1\n" | tee /proc/net/tbtun/config'
rmmod tbtun
ssh root@45.11.1.62 'rmmod tbtun'
```

## 6. 验收

| 项 | 读法 | 目标 |
|---|---|---|
| app 吞吐 | HND0 `iperf3-server.log` 的本次切片，或 client JSON 的 `end.sum_received` | **8 流聚合 17-34 Mbit/s**；单流上限见 §1.3 |
| `rc` | iperf3 客户端退出码 | 0；无 `Broken pipe`/EOF |
| `holes_unrecoverable` | `stats` 的 `c6` 行 | 0 |
| raw `lost` | `stats` 的 `c3` 行，运行前后差值 | 0 |
| `W` | `stats` 的 `c3` 行 `tx_window=` | 精确等于 `16384×N` |
| `txq_max` | `stats` 的 `c3` 行 | 等于 `16384×N + 65824` |
| `reorder_overflow` | `stats` 的 `c3` 行 | 0（否则见 §7 风险 2） |

注意：

- **17-34 Mbit/s 是 8 流聚合目标**。P4 的 8 流上限约 19.2 Mbit/s，刚过 17；P8/P12/P16 的上限高于 34，实际由链路封顶。单流验收会失败，原因见 §1.3。
- `raw lost=0` 与数据完整性是两件事。WAN 矩阵里 raw 每轮都有 `lost`/`replaced`，但 `holes_unrecoverable=0`（summary.md:166）。如果 `lost>0` 而 `holes_unrecoverable=0`，数据没丢，但载体稳定性目标没达到；不要把它记成通过。
- 若要字节精确，另跑 `tbt_burst` 的 strict 相位；iperf3 只测吞吐，不做字节比对。

### 6.1 hole 深度门槛与 N<2

几何 hole 测试的声明门槛是 `g >= 4`：`tbt_late_tab[3]=776152`（0.919³），`tbt_late_tab[4]=713283`，条件 `tbt_late_tab[g] >= tbt_late_tab[TBT_K_MAX_HOLE]` 在 `g=4` 时为假，才继续走声明（`tbtun/tbt_tx.c:1011-1014, 1367`；`TBT_K_MAX_HOLE=3`，`tbtun/tbtun.h:250`）。

`W=2N` 帧时，hole 的最大深度就是 `2N`。因此：

- **N=1（W=2 帧）永远达不到 g=4**，`hole_depth_fail` 会关闭 hole 声明，只剩重传计时器和载体丢失路径修复。不要用 N=1 验证 hole 修复。
- N=2 恰好到 g=4；生产档 N>=4，不受影响。

## 7. 已知风险与建议上限

| # | 风险 | 证据 | 建议 |
|---|---|---|---|
| 1 | 8 流硬上限超过 955 MiB | §4.1：8 × 128 MiB 就绪队列 = 1024 MiB，总计 1174-1222 MiB | 应用可能停读时最多 4 流；8 流时未读积压 <= 32 MiB/流 |
| 2 | `R=262144` 是全会话共享，8 流 × W 会超 | `tbtun/tbt_rx.c:270-279`；README.zh.md:172 | 8 流验收把 `rx_reorder_bytes` 提到 `8W`：P4 524288、P8 1048576、P12 1572864、P16 2097152；两端一致 |
| 3 | raw 载体每轮 `lost`/`replaced` | summary.md:166, 177：1/5/9/8；`carrier_stall_ms=15000` 与 15 s 窗口重合 | 跑 >= 60 s；观察 `una_stall_us`/`stall_declared`/`stall_idle_skips`；必要时把 `carrier_stall_ms` 提到 60000 做对照 |
| 4 | `W=2N` 与单流 17-34 Mbit/s 冲突 | §1.3：P16 单流上限 9.6 Mbit/s | 单流目标改 `tx_window_bytes>=524288`（218 ms；300 ms 需 >=655360）、`rx_reorder_bytes>=W`（建议 1048576 留余量）；同时跑 8 流时 R>=4 MiB；否则只按 8 流聚合验收 |
| 5 | `retain` 的 8 MiB 不是硬上限 | 未 ack 帧永不淘汰（`tbtun/tbt_tx.c:389-391`） | 按 `W + 8 MiB`/流估算；8 流约 132 MiB slab |
| 6 | 文档默认值与代码不一致 | README.zh.md:171-173 vs `tbtun/tbt_parse.c:375-379` | 以代码为准；配置行写显式值，回读核对 |
| 7 | `hole_budget_ms=30000` 是 137 个 RTT | DESIGN.md:939-947 建议 218 ms 路径用 1000-1500 ms；30000/218 ≈ 137 | 它只决定重置上限，不决定修复速度；保持 30000 可以，但验收必须看 `rx_max_stall_us` 和 `holes_unrecoverable` |

风险 2 的补充：`R` 提高后，`tx_window_bytes` 不变，`W <= R` 仍成立；hole 字节预算变成 `max(64 KiB, R)`（`tbtun/tbt_rx.c:944-950`），会随 `R` 提高，这是预期行为。`R` 的最大值是 268435456（`tbtun/tbt_parse.c:481-483`），上面的值都远低于上限。

风险 1 的补充：接收端的 pause 是会话级的——任一条流 `st->paused` 为真，所有载体停止读取（`tbtun/tbt_carrier.c:552-554`）。8 流里一条应用停读会拖住其余 7 条，所以“最多 4 流”不只是内存建议，也是避免队头阻塞的建议。

风险 3 的补充：`carrier_stall_ms=15000` 是生产档的显式值，不是默认值。WAN 矩阵的假设是 raw 不发 PAD（`pad_tx=0`），15 s 测试窗口与 stall 计时重合，计时可能没有被数据/ACK 重置（summary.md:177）。生产跑 60 s 以上可以区分“计时器从未重置”和“真的丢载体”；如果 60 s 仍有 `lost`，再查 `carrier_stall_ms` 与 PAD/keepalive 的交互。

## 8. 证据索引

- 代码：`tbtun/tbt_uapi.h`、`tbtun/tbt_parse.c`、`tbtun/tbt_session.c`、`tbtun/tbt_tx.c`、`tbtun/tbt_rx.c`、`tbtun/tbt_carrier.c`、`tbtun/tbtun.h`、`tbtun/tbt_core.c`。
- 设计：`DESIGN.md` §4.3（hole 测试）、§5.4（窗口）、§8.4.1（echo 规则）、§9.6（replay ring）、§R16（内存）。
- 实测：`review/wan-experiment/summary.md`（直连基线、矩阵、结论、限制）、`review/wan-experiment/deploy/commands.md`（构建/加载命令）。
- 基准：`benches/README.md:225-255, 351-362`；`benches/results/20261105T050727-scale-rec.csv:2`。
