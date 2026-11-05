# tbtun FEC 数据面设计：流式滑窗 (N+M, N) 纠删码字节流

状态：设计稿，待评审。目标实现版本 `TBT_CONV 0x0006`。
读者：内核工程师。所有结论都能落到一个函数或一个字段上；没有落到实处的句子都标了「待定」。
基线：`master 0db5166`；载体层修复参考 `w-window` 分支 head `a33aeb8`（评审时复核）。

## 0. 结论摘要（先读这一页）

| 问题 | 结论 |
|---|---|
| 块式还是流式 | **流式滑窗**。块式的交付条件是「凑够 N 份 share」，这是 `max of N` 的尾延迟，发生在正常路径；流式逐包交付，没有块 barrier。§4 |
| 编码方案 | **唯一的码型：流式 GF(2) 稀疏随机线性码（sliding-window RLNC）**。逐包、无块 barrier、XOR 成本 `(M/K)×d`；可行性取决于 `d` 与 `W` 的比例，见 §3.5。 |
| 生产参数（1G） | **数学上不成立**：`W=2K=448` 时 `d` 需 100–358（接近稠密），编码 25–89 XOR/byte，1G 下 3–11 GB/s。§3.5 |
| **1G + 256 五元组** | **grouped 滑窗 RLNC：32 组 × 8 carriers，每组 M=3/N=5/W=10/d=10**：1.5 GB/s 编码，arm64 5–10% 单核；码率 62.5%。§3.8 |
| 可行参数（1G，arm64，小 N/M） | `N=16, M=4, K=16, W=32, d=16`：4 XOR/byte，1G 下 500 MB/s。§3.5 |
| 可行参数（低速率） | `W<=32, d<=16`：2–4 XOR/byte，所有架构可行（17M/300M）。§3.5 |
| 小参数（原型 / 17M WAN） | `N=4, M=1, carriers=5, K=8, W=16, d=8, fec=rlnc`：wire 1.129，最坏恢复 46 ms。§8.7 |
| 动态码率 | `M = clamp(ceil(256×(p+0.09)), M_min, M_max)`，`N=256-M`；epoch 1 s、滞回 8；epoch 只改 parity 参数，不改数据 seq。§3.7 |
| wire format | **独立的 FEC 格式（conv 0x1001）**：40 字节头、`share_index` u16、`epoch` u16、`n/m` u8、256 位反馈 bitmap。**不复用旧 16-carrier ARQ 的帧头/FBK/WINDOW**；改常量解决不了 16 槽限制。§2.2、§11.2 |
| 窗口 | `W_send = k × BDP_wire / (S × (K+M)/K)`，`k = clamp(RTO/mean_srtt, 2, 5)`；上限由接收端缓冲与 credit 决定。§7 |
| 失败模型 | 1 share/carrier 时 carrier 级故障 = 1 个 share erasure，MDS 直接适用；`M=32` 容忍 32 条 carrier 同时断。若改成 k shares/carrier，必须 `M >= k × 目标故障数`。§11.6 |
| 迁移 | 保留 carrier/HTTP/procfs/sock；替换 per-frame seq/retain/retx/dup/hole。§9 |
| 参考实现 | Kodo 的 sliding-window / sparse RLNC（算法与测试向量参考，不复制代码）；其他码型不采用。§12 |
| 第一交付 | 本文档；原型在 `review/fec-prototype/`，先 host，不写内核。§9.5 |

**一句话**：用 N 条数据载体连续发 packet，M 条 parity 载体发覆盖滑动窗口的 GF(2) 随机线性组合；接收端逐包交付，缺包用 GF(2) 消元恢复；N+M 固定 256，M 随实测丢包率动态调整；没有块边界，没有 GF(2^8) 乘加。


**一句话**：应用字节流切成固定大小的 packet；N 条数据载体连续发送数据包，M 条 parity 载体连续发送覆盖滑动窗口的校验包；接收端按 seq 维护窗口，缺包用覆盖它的 parity 方程精确恢复；**没有「块」这个交付边界**。

---

## 1. 目标、非目标，以及和旧实现的区别

### 1.1 这个项目的核心贡献

核心是 **N+M 份字节流里的「选 N」**：任意 N 份可以高概率重构，M 份是校验份额，不是备用连接，也不是重传副本。N+M 条载体全部承载数据；每条载体上的字节都是同一段数据的线性组合。

三条硬性质：

1. **没有块 barrier。** 数据包到达即可交付；只有真正缺失的包才进入方程恢复。不存在「等一块凑够 N 份」的 `max of N` 尾延迟。
2. **不需要猜字节。** 缺失的包由 GF(2) 线性方程组精确解出（高概率，iid 擦除模型）；解出的字节和原始字节逐位相同。
3. **失败是份额丢失，不是字节流空洞。** 载体断开 = 它承载的 packet 丢失。没有 per-frame 序列空间、没有空洞几何检测、没有跨载体的全局有序约束。

### 1.2 旧实现错在哪里

旧实现把 N+M 读成「N 条数据载体 + M 条待机 spare」，数据面是 per-frame 的：每帧分配全局 seq，接收端用 `rb_root_cached` 按 seq 重排，发送端用 retain ring + dup + retx 修补空洞，接收端用 `hole_budget_ms` 兜底 reset。三处结构性代价：

| 旧机制 | 代价 | 在滑窗 FEC 下 |
|---|---|---|
| 全局 seq 有序交付 | 任一载体慢/丢，整条流停在最小 seq 上；n 从 1 增到 8 吞吐只涨 10% | 消失：数据包按 seq 独立到达，缺包用 parity 补 |
| retain + retx + dup | 每字节至少两次拷贝；dup=1 让 wire 翻倍（WAN 实测 `wire/submit=2.018`） | 消失：parity 是精确的 1/(N+M) 冗余 |
| hole 几何检测（`0.919^g`、generation、gate） | 大量状态、误判即注入无用副本 | 消失：没有「洞」这个概念 |

WAN 实测（`review/wan-experiment/summary.md`）：直连 P1/P4/P8 为 17.08/30.03/33.72 Mbit/s；旧 tbtun 全部配置 3.14–4.55 Mbit/s，总 in-flight 被钳在 120–131 KB，而该路径 BDP 约 465 KB。

**窗口钳制的确切来源**（`0db5166`）：`tbt_tx.c:459-464` 的 `tbt_tx_window()` 返回 `cfg.rx_reorder_bytes / TBT_TX_WINDOW_DIV`，`TBT_TX_WINDOW_DIV = 2`（`tbtun.h:204`），默认 `rx_reorder_bytes = 262144`（`tbt_parse.c:375`），窗口恒为 131,072 B。它是**每流**的字节上限，不是每载体配额，所以 n=1/4/8 的 in-flight 都是 128 KiB 量级。第二个独立天花板是 `TBT_TXQ_HARD_MAX = TBT_SQ_MAX + 4 × TBT_CHUNK_MAX = 98,448 B`（`tbt_tx.c:47`）。

**注意**：FEC 不是吞吐的万能药。它解决跨载体队头阻塞和份额丢失；它不改变单条载体 TCP 的拥塞控制，也不改变五元组限速的物理事实。若 `N+M` 条载体各自只能跑 3.4 Mbit/s，聚合上限就是 `(N+M) × 3.4`。FEC 的收益是让这个聚合真正可加。

### 1.3 目标

- 双向、字节精确、有序的应用字节流，语义与普通 TCP 连接一致。
- 数据面全内核；无用户态进程、无字符设备、无每流 syscall 对。
- **流式滑窗 (N+M, N) 纠删码，N/M/W 全部运行时配置**：N 条数据载体 + M 条 parity 载体；parity 覆盖滑动窗口 W；接收端任意足够方程可重构；没有块交付边界。
- **通用 N/M**：总 share 数 `N+M <= 255`；GF 约束是编码矩阵 `M+W <= 256`。`n/m` 是 FEC 参数，`carriers=` 是 TCP 连接数；目标部署是 **1 share/carrier**（`carriers = n+m`），也允许 `carriers < n+m` 的降级映射。不写死 N=4/M=1。
- **单一码型**：GF(2) 滑窗 RLNC；无 GF(2^8) 乘加、无块 barrier、mips 可行。`fec=rlnc` 是唯一值。§3。
- 解码前沿 `next_deliver` 推进；窗口滑动；窗口外不可恢复时有界 reset。
- 窗口 `W_send = k × BDP / wire_rate`；硬上限由接收端包缓冲决定；给出内存上界与 credit。
- 载体仍是 N+M 条独立 TCP（五元组分离）；HTTP 容器只是 QoS profile，FEC 在容器之上。
- **1 Gbit/s（125 MB/s）单核预算**：RLNC 的编码成本必须落在 arm64/mips 的可接受区间；mips 超预算时降 `d` 或降 `M`，并在 procfs 显示当前档位。

### 1.4 非目标

- **不做块式 FEC 作为主路径。** 块式（每 B 字节独立编码、等 N 份 share）只作为对照与极端 fallback；它的 `max of N` 尾延迟是本设计要消除的对象。§4 给出完整对比。
- **不做 ARQ 产品。** NACK 是可选的修复通道，不是主路径；主路径是「parity 方程一到就解」。
- **不做加密。** 与 `http-kal` 的明文可见性冲突，沿用旧设计：要机密性就整体套 WireGuard。
- **不做 MDS。** 恢复保证是 iid 擦除下的高概率，不是「任意 M 个」的确定性保证；§3.5 给出 p 曲线。
- **不复用旧 16-carrier wire。** 旧 ARQ 的 `path_ack_mask`/FBK/WINDOW/`path_gen[16]` 是结构性限制；FEC 用独立格式，两者靠 `conv` 区分。§2.2、§11.2。
- **不做跨流交织。** 每流独立的 packet 空间。
- **不做 IPv6、不做 DNS。** 沿用 v0 约束。
- **不保证不可恢复时无损。** 窗口外仍有缺失 → 有界 reset；绝不静默交付坏字节。

### 1.5 术语

| 词 | 含义 |
|---|---|
| packet | 一段连续应用字节，长度 `1..S`；不足 `S` 时零填充 |
| `S` | packet 的应用字节容量（默认 8 KiB） |
| `N` | 数据载体数；数据包按 seq 轮转分配到 N 条载体 |
| `M` | parity 载体数；每条 parity 载体承载一个独立的校验行 |
| `K` | 每组数据包数；每 K 个数据包生成 M 个 parity 包 |
| `W` | parity 覆盖窗口长度（数据包数），`W ≥ K` |
| 重叠度 | `(W-K)/K`；W=K 时退化为块式 |
| `next_deliver` | 解码前沿：所有 `seq < next_deliver` 的数据包已交付 |
| 方程 | 一个 parity 包给出的 GF(2) 线性方程 |
| credit | 接收端通告的「还能收多少 packet」 |

---

## 2. packet 格式、线上布局、HTTP 容器映射

### 2.1 为什么 packet 自带头

每个 packet 独立走一条载体，接收端必须能在不依赖其他 packet 的前提下识别它：属于哪条流、什么 seq、是数据还是 parity、覆盖哪个窗口。因此每个 packet 都带一个 32 字节头。开销 `32/S`：`S=8 KiB` 时 0.39%。

### 2.2 packet 记录（FEC wire v1，独立于旧 ARQ）

**这是新的 wire format，不复用旧 16-carrier ARQ 的任何字段。** 旧格式的 `path_ack_mask` 是 `u16`、FBK body 最多 16 项、`path_gen[16]`/`path_end[16]` 都是 16 槽；把常量改成 256 会牵动帧头、FBK、WINDOW、HELLO、FIN/RST mask 的整条链，且旧 ARQ 的语义（per-frame seq、retain、retx）与 FEC 无关。FEC 用独立的新格式，旧格式保留给旧的 per-frame 数据面，两者靠 `conv` 区分。

```
+0   u8   type          0x30 DATA；0x31 PARITY；0x32 ACK；0x33 NACK
                        0x34 FEC_CFG；0x35 FEC_FBK；0x23 PAD
+1   u8   flags         bit0 LAST；bit1..7 保留为 0
+2   u16  share_index   0..N+M-1；u16 为 256 carriers 留出余量
+4   u8   n             数据 share 数（n+m <= 256，u8 够用 0..255）
+5   u8   m             parity share 数
+8   u64  seq           DATA：数据包序号；PARITY：窗口最后一个序号
+16  u16  epoch         码率 epoch；PARITY/CFG/FBK 有效（1 s 一个，65536 个 ≈ 18 h）
+20  u32  sid_lo        40 位流 id 的低 32 位
+24  u8   sid_hi        高 8 位
+25  u8   d             PARITY 的 degree；其他类型为 0
+26  u16  win_len       PARITY 覆盖的包数（裁剪后 ≤ W）；DATA 为 0
+28  u16  payload_len   本记录载荷字节数；DATA/PARITY 恒为 S
+30  u16  conv          0x1001（FEC wire v1，与旧 0x0006 不同）
+32  u32  crc32c        覆盖本 packet 载荷（解码后校验用）
+36  u32  reserved      0
```

总计 40 字节。开销 `40/S`：`S=8 KiB` 时 0.49%，可接受。

字段约束（接收端必须全部检查，任一失败即 `-EPROTO` 并关闭该载体）：

- `type` 必须是本版本定义的值；未知类型拒绝。
- `conv == 0x1001`；旧 `0x0006` 的 carrier 直接拒绝（`conv_mismatch++`）。
- `n + m <= 256`；`share_index < n + m`，比较时必须先转 `u16`（`n+m=256` 时 `u8` 会截断成 0）。
- `payload_len == S`（DATA/PARITY）；`win_len <= W`（PARITY）；`d <= win_len`。
- `reserved == 0`；`flags` 只有 bit0 允许。
- `seq` 在 `[0, 2^63)`；`epoch` 在 `[0, 2^32)`。

**为什么用 40 字节而不是复用 32 字节。** 旧头没有 `epoch`，`share_index` 只有 8 位且被 `row`/`n`/`m` 挤占；`win_len` 与 `payload_len` 共用位置。256 carriers + 动态码率需要 `u16 index`、`u16 epoch`、`u8 n/m`、`u16 win_len`、`u16 payload_len` 同时存在。多 8 字节换取格式清晰，比在旧头上打补丁安全。

**为什么带 CRC。** TCP 校验和保证单跳无损坏，但解码是一次矩阵运算，实现 bug 会静默产出错误字节。CRC32C 覆盖 payload，在恢复后校验一次；失败就丢弃该包并计数 `fec_crc_fail`，绝不交付。成本：每包一次 `crc32c()`，8 KiB 约 0.13 µs（硬件加速约 10 GB/s），相对包周期可忽略。

### 2.3 数据包与 parity 包的索引

```
数据包:  d_0, d_1, d_2, ...        每包 S 字节（末包可短）
parity:  p_g^m                      g = 组号, m = 行号
```

第 `g` 组覆盖的数据包区间：

```
win_start(g) = max(0, g*K - (W-K))
win_end(g)   = g*K + K - 1
```

`win_len(g) = win_end - win_start + 1`，裁剪后 `≤ W`。启动阶段（`g=0`）窗口只有前 K 个包；稳态每个窗口满 W 个包。

**为什么是后向窗口。** parity 只能覆盖已经发出的数据；`win_end(g) = g*K + K - 1` 表示第 g 组的 parity 在该组最后一个数据包之后立即发出。这样 parity 的恢复能力在数据包发出后立刻生效，不需要等 W 个包。

### 2.4 线上布局（每条载体）

一条载体上的字节流是连续的 packet 记录。记录之间没有额外分隔符；接收端按头里的 `payload_len` 前进。

```
data carrier i:    [DATA seq=i] [DATA seq=i+N] [DATA seq=i+2N] ...
parity carrier m:  [PARITY g=0 row=m] [PARITY g=1 row=m] ...
```

数据包按 `seq % N` 分配到 N 条数据载体；parity 组按 `g` 顺序分配到 M 条 parity 载体，第 m 条只承载 row=m。

`raw` profile：记录直接连写。
`http-kal` / `http-kal-drip`：一条记录正好是一个 HTTP chunk 的 chunk-data。

```
chunk = hex(32 + S) "\r\n" <packet record> "\r\n"
```

这与旧 `DESIGN.md §3.3` 的「一个 chunk 恰好一条记录」一致，parser 不需要改语义，只需要接受新的 `type` 和 `payload_len`。

`PAD` 保持 32 字节记录、`payload_len=0`，用于载体保活；它不属于任何 packet。

### 2.5 常量与配置校验

FEC 记录头是 40 字节（§2.2），不是旧的 32 字节，所以**不复用旧记录的尺寸常量**：

| 常量 | 旧值 | FEC 值 | 说明 |
|---|---:|---:|---|
| `TBT_FEC_HDR` | — | **40** | FEC 记录头 |
| `TBT_FEC_PAYLOAD_MAX` | — | **8192** | `S` 的上限 |
| `TBT_FEC_REC_MAX` | — | **8232** | `40 + S` |
| `TBT_FEC_CHUNK_MAX` | — | **8236** | `40 + S + 4`（hex 长度行 + CRLF 的余量） |
| `TBT_FEC_MAX_CARRIERS` | — | **256** | FEC 自己的载体上限，不改旧的 `TBT_MAX_CARRIERS_LIMIT` |
| `TBT_FEC_MAX_SHARES` | — | **256** | `N+M` 的上限 |

`c->part` 残尾缓冲、`TBT_HTTP_FRAME_MAX` 在 FEC 侧按 `TBT_FEC_REC_MAX`/`TBT_FEC_CHUNK_MAX` 重新分配；旧的 8224/8228 常量留给旧 ARQ。

配置校验：

- `S ∈ [1024, 8192]`；`N >= 1`、`M >= 1`；`N+M <= 256`。
- `K >= 1`、`W >= K`、`W <= 1024`、`d <= W`。
- `carriers == N+M`（v1 强制 1 share/carrier）；`carriers <= 256`。
- `n+m=256` 时，`share_index` 的比较必须先转 `u16`（`u8` 会截断成 0）。

失败即拒绝该 `tunnel` 行，不进入运行期。

### 2.6 控制记录

`ACK`（0x32），载荷 16 字节：

```
+0  u64 next_deliver     已连续交付到的数据包序号（不含）
+8  u32 credit_packets   接收端还能接收的 packet 数
+12 u32 rtt_us           接收端观测到的恢复 RTT（可选，0 表示未测）
```

`NACK`（0x33），载荷变长，每个条目 12 字节：

```
+0  u64 missing_seq      缺失的数据包序号
+8  u16 count            连续缺失的包数（从 missing_seq 起）
+10 u16 reserved         0
```

`FEC_CFG`（0x34），载荷 32 字节：

```
+0  u32 epoch            新 epoch
+4  u16 n
+6  u16 m
+8  u16 w
+10 u16 k
+12 u8  d
+13 u8  reserved[3]
+16 u64 seed             PRNG seed（与 session_nonce 异或）
+24 u32 crc32c           覆盖前 24 字节
+28 u32 reserved         0
```

`FEC_FBK`（0x35），载荷 48 字节：

```
+0  u32 epoch            本反馈对应的 epoch
+4  u32 p_ewma_ppm       丢包率 EWMA，百万分之一
+8  u16 m_current
+10 u16 m_target
+12 u32 expected_shares  本 epoch 发送端已发 share 数
+16 u32 received_shares  接收端已收 share 数
+20 u8  carrier_bitmap[32]   256 位：哪些 carrier 在本 epoch 有丢包
+52 u32 reserved         0
```

**256 位位图**：`carrier_bitmap` 是 32 字节，覆盖 256 条 carrier；bit i = 1 表示 carrier i 在本 epoch 有丢失。它替代旧的 `path_ack_mask`（u16），不需要分块。

**注意区分两个位图**：

- **carrier 位图**（反馈用）：256 位，覆盖 256 条 carrier。
- **系数位图**（接收端本地）：`W` 位；`W=448/480` 时需要 **512 位（8×u64）**，不是 256 位。badrlnc 的 256-bit mask 不够。若用显式系数，每行 `d × 2 B`（d=16 时 32 B）；若用 seed 模式，只需 `(lower, symbols)` 两个整数。

控制记录走任意一条可用载体；不绑定 packet，不计入发送窗口。

---

## 3. 滑窗 RLNC：编码结构、系数、PRNG

本设计只采用一种码型：**GF(2) 稀疏随机线性码（streaming RLNC）**。其他码型不采用，理由在 §3.6 一句话说明。

### 3.1 通用参数与约束

| 参数 | 范围 | 约束 |
|---|---|---|
| `N` | 1..255 | 数据 carrier 数（也是数据 share 数） |
| `M` | 1..255 | parity carrier 数（也是 parity share 数） |
| `N+M` | 2..256 | 总 share 数；目标部署 96+32=128 |
| `carriers` | 1..256 | 实际 TCP 连接数；目标 = `N+M`（1 share/carrier） |
| `S` | 1024..8192 | packet 载荷；默认 8192 |
| `K` | 1..1024 | 每组数据包数；决定 parity 速率 `M/K` |
| `W` | K..1024 | 覆盖窗口；约束 `W >= K` |
| `d` | 1..W | 每个 parity 覆盖的数据包数（degree） |
| `fec` | `rlnc`（唯一） | 保留 `auto` 作为别名，永远解析为 `rlnc` |

配置校验：`carriers <= 256`、`carriers == N+M`（v1 强制 1 share/carrier）、`S` 在范围内、`K <= W`、`d <= W`、`N+M <= 256`。任一失败拒绝该 `tunnel` 行。

**为什么 `N+M <= 256` 而不是 255。** GF(2) 没有域大小限制；256 是 carrier 位图与 `u8` 索引的自然上限。`share_index` 用 `u8` 时最多 255，所以实际上限是 255；v1 取 128。

### 3.2 编码结构

```
数据:   d_0, d_1, ..., d_{L-1}          每包 S = 8 KiB
parity: p_{g,r}                          g = 组号, r ∈ [0, M)
```

第 g 组覆盖的数据包：

```
win_start(g) = max(0, g*K + K - W)
win_end(g)   = g*K + K - 1
```

`K` 是每组数据包数，`W` 是窗口长度（`W >= K`）。每 `K` 个数据包生成 `M` 个 parity 包。

parity 的系数是一个 `W` 位的 0/1 向量，其中恰好 `d` 位为 1：

```
p_{g,r} = XOR { d_i : i ∈ window(g), coef(g, r, i) == 1 }
```

- 系统化：`d_i` 本身是明文；无丢包时零解码。
- `d` 是 degree（每个 parity 覆盖多少个数据包）。
- 稀疏：`d << W`，编码只做 `d` 次 XOR。

**与块式的区别**：块式要求「凑够 N 份 share」才交付；RLNC 逐包交付，缺包用方程恢复。`K=W` 时窗口无重叠，但交付仍然是逐包的，不是块式的 `max of N`。

### 3.3 系数生成（PRNG）

系数不上 wire。收发两端用同一 PRNG、同一 seed 复现：

```
seed = session_nonce ^ (g * M + r)          /* 64 位 */
state = splitmix64(seed)
for j in 0..d-1:
    /* 从 [0, W) 选不重复的位置 */
    pos = j + bounded(&state, W - j)
    swap(pool[j], pool[pos])
    selected[j] = pool[j]
```

- `pool` 初始化为 `[0, 1, ..., W-1]`，用部分 Fisher-Yates 选 `d` 个不重复位置。
- `splitmix64` 是确定性、跨架构一致的 64 位 PRNG；不要用 `prandom`（内核的 `prandom` 状态不可控）。
- `session_nonce` 是 128 位，取低 64 位参与 seed；高 64 位混入。
- 接收端只需要 `(nonce, g, r, W, d)`，不需要额外的系数头。

**参考实现**：Kodo 的 sliding-window RLNC 用同一思路（确定性 PRNG + 收发共享 seed）。本设计不复制 Kodo 代码，只用它的算法结构与测试向量；详见 §12。

**安全边界**：系数是公开的（可复现），不提供机密性；只提供「随机性」以避免对抗性擦除模式。若担心对手构造擦除，可以在 nonce 里加一个随机段。

### 3.4 编码成本

每个数据包摊到 `(M/K) × d` 次 XOR/byte。

对 `N=224, M=32, K=224`：`M/K = 1/7`；对 `N=192, M=64`：`M/K = 1/3`。

`q = W/K = 2`：每个数据包被两组 parity 覆盖，所以编码成本是 q=1 的两倍：

```
编码字节量 = 125 × (M/256) × d × 2 MB/s
```

| M | d | XOR/byte | 1G 下编码量 | SIMD 32B 指令/s |
|---:|---:|---:|---:|---:|
| 32 | 16 | 4.0 | **500 MB/s** | 15.6 M |
| 64 | 16 | 8.0 | **1000 MB/s** | 31.3 M |
| 80 | 16 | 10.0 | **1250 MB/s** | 39.1 M |
| 96 | 16 | 12.0 | **1500 MB/s** | 46.9 M |
| 128 | 16 | 16.0 | **2000 MB/s** | 62.5 M |

**这张表是 d=16 的成本，但 d=16 在 W=448 时恢复率为 0**（§3.5：每方程只命中 1.6 个丢失包）。要真正恢复，`d >= W²p/M`：p=10%、M=64、W=384 时 d>=230，成本 `(64/256)×230×2 = 115 XOR/byte`，1G 下 14 GB/s。**所以 1G + 256 + W=2K 在 arm64 上也不成立。** survey 的 500–2000 MB/s 是 d=16 的下界，不是可恢复配置的成本。

**`d` 的硬约束（这一条推翻了很多直觉）**：稀疏 RLNC 的每个方程只覆盖 `d` 个位置，命中丢失包的概率是 `d/W`。要让每个丢失包被 `k` 个方程覆盖，需要：

```
M × (d/W) >= k = W × p        =>        d >= W² × p / M
```

对 `W=448, M=56, p=0.1`：`d >= 448² × 0.1 / 56 = 358`，接近稠密（W=448）。编码成本 `(M/K)×d = (56/224)×358 = 89 XOR/byte`，1G 下 11 GB/s，不可行。

**结论**：`W >> d` 时稀疏 RLNC 不成立。可行区间是 `d ≈ W/4..W/2`，或者 `W` 本身很小（`W<=32`）。`d=16` 只适用于 `W<=64` 的小窗口。

### 3.5 恢复概率与 iid 模型

设窗口内丢失 `k` 个数据包，收到 `n` 个独立 parity 方程。恢复概率 = 随机 `n×k` GF(2) 矩阵满秩的概率：

| n - k | 满秩概率（近似） |
|---:|---:|
| 0 | 0.288 |
| 1 | 0.577 |
| 2 | 0.770 |
| 3 | 0.885 |
| 4 | 0.942 |
| 5 | 0.971 |

**iid 擦除的容量阈值**：`p < 1 - R = 0.25`（R = N/(N+M) = 0.75）。

**host 模拟结果（正确模型：方程从 W 个位置选 d 个，命中丢失包才算非零）**：

| W | M | d | p=0.1 恢复率 | 说明 |
|---:|---:|---:|---:|---|
| 448 | 32 | 16..448 | **0.000** | `k=44.8 > M=32`，方程数不够，d 再大也无解 |
| 224 | 32 | 16 | 0.050 | d 太小，命不中 |
| 224 | 32 | 32 | 0.780 | 边缘 |
| 224 | 32 | 64 | **1.000** | `d=W/3.5` |
| 32 | 8 | 16 | 0.925 | 小窗口 |
| 32 | 8 | 32 | 0.970 | 小窗口稠密 |

**两条硬约束**：

1. **`M >= W×p`**：方程数必须不少于丢失数。`W=448, p=0.1` → `k=44.8`，`M=32` 直接无解。
2. **`d >= W²p/M`**：每个丢失包要被足够多的方程覆盖。`W=224, M=32, p=0.1` → `d >= 157`（接近稠密），但模拟显示 `d=64` 已经 1.000，因为方程命中的是"丢失包集合"而不是全窗口。

**对 1G/256 的直接含义**：`W=2K=448` 时，`M` 至少 56、`d` 至少 100–358，编码成本 25–89 XOR/byte。1G 下 3–11 GB/s，**arm64 SIMD 也做不到**。所以 1G + 256 carriers + 流式 RLNC（W=2K）不成立。

**可行的组合**：

| 目标 | 配置 | 编码成本 | 1G 可行性 |
|---|---|---|---|
| 1G + 256 carriers | **不成立**（W=2K 时 d 接近稠密） | 25–89 XOR/byte | 不可行 |
| 1G + 小 N/M | `N=16, M=4, K=16, W=32, d=16` | 4 XOR/byte | arm64 可行（500 MB/s） |
| 1G + 块式退化 | `W=K=224, M=56, d=64` | 16 XOR/byte | arm64 边缘（2 GB/s） |
| 17M/300M | `W<=32, d<=16` | 2–4 XOR/byte | 所有架构可行 |

**这一条必须让用户知道**：1G + 256 carriers + 流式 RLNC 在数学上不成立；要么降 N/M，要么降带宽，要么接受块式（W=K）。

| M | N=K | W=2K | p=5% | 10% | 15% | 20% | 25% | 30% |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 40 | 216 | 432 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| 56 | 200 | 400 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| 64 | 192 | 384 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| 80 | 176 | 352 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| 88 | 168 | 336 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| 104 | 152 | 304 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |

**W=K（块式）对照**：`p=0.15` 时 0.998，`p=0.25` 时 0.995；W=2K 把它们拉到 1.000。这就是「流式」的实际收益：**后续组的 parity 能补前面组的丢包**。

**为什么 W>K 不会让恢复变差**：窗口内丢失数 `W×p` 增加，但覆盖方程数 `(W/K)×M` 也按同样比例增加；净 overhead 不变，而「跨组修复」多了一层。模拟显示 W=2K 在所有 p 下都是 1.000。

### 3.6 不采用的码型（一句话）

RS 需要 `M` 次 GF 乘加/byte，mips 不可行；RaptorQ/LT 是块式 barrier；product/staircase 非 MDS 且恢复模式难验证；SC-LDPC 复杂度高一个量级。它们都不满足「流式 + 大 N/M + mips 可行」的组合，不进实现。


### 3.7 动态码率：N+M=256，M 随 p 调整

生产配置固定 `N+M=256`，`M` 由接收端测得的 iid 丢包率动态决定，`N=256-M`。这样带宽浪费跟随链路质量，而不是一直按最坏情况付费。

#### 3.7.1 控制器

接收端按 epoch（默认 1 s）统计 share 丢失率：

```
p_epoch = 1 - received_shares / expected_shares
p_ewma  = (1 - alpha) * p_ewma + alpha * p_epoch      alpha = 1/8
```

`expected_shares` 由发送端的 `FEC_FBK` 携带的「本 epoch 已发 share 数」给出；接收端用 `max_seq - first_seq + 1` 也能估，但发送端的计数更准。

目标 `M`：

```
M_target = clamp(ceil(1.3 * W * p_ewma) + 8, M_min, M_max)
M_target = round_up_to_8(M_target)
N_target = 256 - M_target
W = 2 * N_target                 /* 流式：W > K，跨组覆盖 */
```

- 公式来自 fec-survey 的 `M = clamp(ceil(1.3×W×p) + 8, 16, 128)`（W=2N）。`W` 依赖 `M`，所以迭代 2–3 次收敛。它比 `256(p+0.09)` 更保守：p=10% 时给 M=64（旧式给 56），p=20% 时给 96（旧式给 80）。
- `M_min = 16`、`M_max = 128`：低于 16 时抗突发能力太弱；高于 128 时 goodput 掉到 50% 以下。
- 对齐到 8：减少 epoch 切换频率，且 `M` 是 8 的倍数时 `M/8` 是整数，位图操作简单。

**为什么 `W = 2K = 2N`。** 1 share/carrier 要求 `K = N`（否则 parity carrier 与数据 carrier 的负载不均衡）。`W = K` 时 parity 只覆盖本组，丢包要等本组 parity——这是块式 barrier。`W = 2K` 让每个数据包被两组 parity 覆盖，后续组可以补前面组的丢包；模拟显示 W=2K 在 p<=30% 时恢复率 1.000。

**为什么是 `256 × p` 而不是 `256 × p/(1-p)`。** 推导：每 epoch 发 `N` 个数据包、`M` 个 parity 包；接收端收到 `(1-p)N` 个数据、`(1-p)M` 个 parity。需要 `(1-p)M >= pN`，即 `M >= pN/(1-p) = p(256-M)/(1-p)`，解得 `M >= 256p`。所以 `M/256` 就是 iid 阈值。

#### 3.7.2 码率表

| M | N | 码率 `N/256` | overhead `M/N` | goodput @1G | iid 阈值 `M/256` |
|---:|---:|---:|---:|---:|---:|
| 16 | 240 | 0.9375 | 6.7% | 937.5 Mbit/s | 6.25% |
| 32 | 224 | 0.8750 | 14.3% | 875.0 | 12.50% |
| 40 | 216 | 0.8438 | 18.5% | 843.8 | 15.63% |
| 48 | 208 | 0.8125 | 23.1% | 812.5 | 18.75% |
| 64 | 192 | 0.7500 | 33.3% | 750.0 | 25.00% |
| 96 | 160 | 0.6250 | 60.0% | 625.0 | 37.50% |
| 128 | 128 | 0.5000 | 100.0% | 500.0 | 50.00% |

**推荐 M 与 W**（`margin=0.09`，`W=2K=2N`）：

| p | M | N=K | W=2K | d | goodput @1G | 编码 MB/s | 恢复率 |
|---:|---:|---:|---:|---:|---:|---:|---|
| 5% | 32 | 224 | 448 | 16 | 875.0 Mbit/s | 500 | 99.9% |
| 10% | 64 | 192 | 384 | 16 | 750.0 | 1000 | 99.9% |
| 15% | 80 | 176 | 352 | 16 | 687.5 | 1250 | 99.9% |
| 20% | 96 | 160 | 320 | 16 | 625.0 | 1500 | 99.9% |
| 30% | 128 | 128 | 256 | 16 | 500.0 | 2000 | 99.9% |

**`M` 的下界**：`M >= 256p` 是渐近阈值；有限长需要 `M ≈ 256p + 24`（即 `margin=0.09`）。低于这个值，overhead 不足，恢复率快速下降。

#### 3.7.3 epoch 与切换

- **epoch 长度**：默认 1 s。1G 下是 125 MB、约 16000 个 8 KiB packet、约 125 个窗口；统计样本足够。
- **滞回**：`|M_target - M_current| >= 8` 才切换。避免 `M` 在阈值附近抖动。
- **切换时机**：只在 epoch 边界切换，不在窗口中间。
- **切换流程**：
  1. 接收端算出 `M_target`，发 `FEC_FBK{epoch, p_ewma, M_current, M_target}`。
  2. 发送端在下一个 epoch 边界应用新 `M`，并把 `FEC_CFG{epoch+1, N, M, W, d, seed}` 插入 parity 流（走任意 carrier，重复 3 次）。
  3. 接收端收到 `FEC_CFG` 后，为新 epoch 建新的窗口状态；旧 epoch 的窗口继续用旧参数解码，直到滑出或超时。

#### 3.7.4 跨 epoch 的窗口

**数据 seq 全局单调，不因 epoch 重置。** 这是「不引入块式 barrier」的关键：epoch 只改 parity 的参数，不改数据的编号与交付顺序。

接收端为每个活跃 epoch 维护一组方程：

```c
struct tbt_rlnc_epoch {
    u32 id;
    u8  n, m, w, d;
    u64 seed;
    struct tbt_rlnc_win *win;   /* 该 epoch 的窗口与方程 */
    u64 first_seq, last_seq;    /* 该 epoch 覆盖的数据 seq 范围 */
};
```

解码 `next_deliver` 时：

1. 找出覆盖它的所有 epoch（通常 1 个，切换瞬间 2 个）。
2. 每个 epoch 内做 GF(2) 消元；**不跨 epoch 混合方程**（系数由不同 PRNG 生成，混合没有意义）。
3. 任一 epoch 能解出 → 交付。
4. 所有 epoch 都 rank 不足 → 等新 parity。
5. `next_deliver < max_seen - W` → reset。

**epoch 重叠期**：切换后的 `W` 个数据包内，两个 epoch 的 parity 都覆盖它们。这是冗余，不是浪费——它让切换瞬间的恢复率不下降。

**epoch 切换不会引入块式 barrier**：数据包仍然逐包交付；只是「用哪组方程恢复」多了一个选择。

#### 3.7.5 256 carriers 的资源

`N+M=256`、1 share/carrier → 256 条 TCP。

| 项 | 值 |
|---|---|
| 每 carrier 速率 @1G | 3.91 Mbit/s |
| BDP @218 ms | 106 KiB |
| 推荐 `SO_SNDBUF`/`SO_RCVBUF` 设置值 | 64–128 KiB（内核翻倍后 128–256 KiB each） |
| 每 carrier 实际 snd+rcv | 256–512 KiB |
| 256 carriers 缓冲合计 | 64–128 MiB |
| 固定开销（tcp_sock/socket/HTTP/work item） | 约 1.4 MiB |
| 总内存 | 65–129 MiB |

CPU（1G，MSS 1460 → 85.6k pps，每 carrier 334 pps）：

| 项 | 估算 | 单核占比 |
|---|---:|---:|
| TCP 收发（含 ACK/窗口/重传） | 2–3 µs/pkt × 85.6k | **17–26%** |
| 50 ms poll × 256（合并后） | 20 wakeups/s × 256 × 1 µs | 0.5% |
| 未合并的每 carrier poll | 5,120 wakeups/s × 10 µs | 5.1%（必须避免） |
| HTTP chunk 解析 | 85.6k × 0.5 µs | **4.3%** |
| **合计（不含 FEC）** | | **22–31%** |

**50 ms poll 的合并**：256 条 carrier 各自 poll 会产生 5120 wakeups/s。改成「每 50 ms 一个 session 级 poll，遍历 256 条」，把 wakeups 降到 20/s，代价是单条 carrier 的故障发现延迟最多 50 ms。这是必须做的优化，否则 poll 本身就吃掉 5% 单核。

**中断**：256 条 TCP 的 softirq 会集中在一个或几个 CPU。需要 RPS/RFS 把流分散，或至少让 `sk_data_ready` 的 work item 走多队列。模块内的 pump 仍是单线程（`sess->pump_lock`），256 carriers 下必须改成「每 carrier 自己的 `w_tx`」。

#### 3.7.6 推荐默认值

**x86_64 / arm64**：

```
n=16 m=4 carriers=20 packet=8192 k=16 w=32 d=16 fec=rlnc
fec_m_min=4 fec_m_max=16 fec_margin=0.09 fec_epoch_ms=1000
fec_hysteresis=8
```

**mips**：

```
n=8 m=2 carriers=10 packet=8192 k=8 w=16 d=8 fec=rlnc
fec_m_min=2 fec_m_max=8 fec_margin=0.09 fec_epoch_ms=1000
fec_hysteresis=8
```

启动时 `M=16`（最省），控制器在 2–3 个 epoch 内收敛到链路实际需要的 `M`。若链路质量未知，可以从 `M=32` 起步，避免前几秒的 reset。

**1G 下参数的依据**：

- `epoch=1 s` → 125 MB、约 16000 个 8 KiB packet；`p` 的样本误差约 `sqrt(p(1-p)/16000)`，p=0.1 时约 0.24%，远小于 margin 5%。
- `margin=0.09` 对应 `M ≈ 256p + 24`；主要覆盖「有限长 gap + 短时突发」，不是统计误差。
- `M_max=128` 给 p=30% 留余量（阈值 50%）；`M_max=48` 是 mips 的 CPU 上限（编码 15–30% 单核）。
- `hysteresis=8` 对应 3.1% 的码率变化，避免 `M` 在阈值附近抖动。

### 3.8 Grouped 滑窗 RLNC：256 五元组 + 1G 的唯一现实路径

单 256-symbol 稀疏 RLNC 在 1G 不可行（§3.5：`d >= W²p/M`，W=448 时 d 接近稠密）。把 256 carriers 分成 32 组 × 8，每组独立滑窗 RLNC，可以把 `W` 从 448 降到 10–14，`d` 从 230 降到 8–10，编码成本降一个数量级。

#### 3.8.1 结构

```
256 carriers = 32 groups × 8 carriers
每组: N = 8 - M, K = N, W = 2N, d = min(W, 8..10)
应用字节流按 group = seq % 32 条带化（striping）
```

- 每组独立编码、独立窗口、独立解码；组间不共享方程。
- 应用字节流按 `seq % 32` 分到各组；接收端按 `seq` 重组。
- 一组丢包不阻塞其他组：这是 grouped 的核心优势。
- 组内仍是流式：`W=2N > K`，每个包被两组 parity 覆盖。

#### 3.8.2 每组参数与恢复率

host 模拟（GF(2) 随机稀疏，800 次/点，`K=N`, `W=2N`）：

| M | N | W | d | p=1% | 5% | 10% | 20% | 30% |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 7 | 14 | 8 | 1.000 | 0.697 | 0.710 | 0.000 | 0.000 |
| 2 | 6 | 12 | 8 | 1.000 | 0.938 | 0.946 | 0.839 | 0.349 |
| 2 | 6 | 12 | 12 | 1.000 | 0.986 | 0.989 | 0.897 | 0.325 |
| 3 | 5 | 10 | 8 | 1.000 | 0.996 | 0.996 | 0.974 | 0.917 |
| **3** | **5** | **10** | **10** | **1.000** | **1.000** | **1.000** | **0.980** | **0.925** |
| **4** | **4** | **8** | **8** | **1.000** | **1.000** | **1.000** | **0.996** | **0.996** |

**读数**：

- `M=1` 在 p>=5% 就崩（`d=8 < W²p/M = 19.6`）。
- `M=3, d=10`（稠密）在 p<=10% 是 1.000，p=20% 是 0.980。
- `M=4, d=8`（稠密）在 p<=30% 是 0.996，接近 99.9%。
- **要 99.9%+，M 必须随 p 增大**；`M=4` 是 p<=20% 的安全档。

#### 3.8.3 总码率、goodput、编码成本

| 每组 M | 每组 N | 总 N（32 组） | 总 parity | 码率 | goodput @1G | 每组 d | 编码 XOR/byte | 1G 编码量 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 7 | 224 | 32 | 87.5% | 875 Mbit/s | 8（不够） | — | — |
| 2 | 6 | 192 | 64 | 75.0% | 750 | 8 | 5.33 | 667 MB/s |
| **3** | **5** | **160** | **96** | **62.5%** | **625** | **10** | **12.0** | **1.5 GB/s** |
| 4 | 4 | 128 | 128 | 50.0% | 500 | 8 | 16.0 | 2.0 GB/s |

编码 XOR/byte = `(M/N) × d × q`，q=2。

**arm64 SIMD 32B**：1.5 GB/s = 46.9 M 指令/s，约 5–10% 单核；2.0 GB/s = 62.5 M，约 8–15%。**可行。**
**mips**：DRAM 271 MiB/s，1.5 GB/s 是它的 5.5 倍；不可行。

#### 3.8.4 延迟与内存

| 项 | 32×8 分组（M=3） |
|---|---|
| 每组速率 @1G | 125/32 = 3.9 MB/s |
| 1 包（8 KiB） | 2.1 ms |
| 窗口 W=10 | 21 ms |
| 恢复延迟 | 1–2 个窗口 = 21–42 ms |
| 每组缓冲 | W×S = 10×8 KiB = 80 KiB |
| 32 组缓冲 | 2.5 MiB |
| 解码 k²×S | k=W×p=1（p=10%），可忽略 |

#### 3.8.5 跨组重组

- 应用字节流按 `seq % 32` 条带化；接收端按 `seq` 重组。
- 组间独立：一组缺包只影响它自己的 seq，不影响其他组。
- 跨组 reorder：接收端维护全局 `next_deliver`；某组卡住时，其他组的数据包在 reorder 缓冲等待，直到该组恢复或超时。
- **一组 reset 不等于全局 reset**：该组的 seq 空洞由 NACK 或窗口外判据处理；若该组永久不可恢复，只有该组的 seq 被丢弃（应用层看到的是流中断，不是整条隧道 reset）。这是 grouped 相对单码的另一个优势。

#### 3.8.6 动态 M

全局控制器按 p 设每组 M：

```
M_group = clamp(ceil(1.3 × W × p) + 1, 1, 4)   /* W = 2(8 - M_group) */
总 N = 32 × (8 - M_group)
总 parity = 32 × M_group
```

| p | M_group | 总 N | 总 parity | 码率 | 恢复率 |
|---:|---:|---:|---:|---:|---|
| 1% | 1 | 224 | 32 | 87.5% | 100% |
| 5% | 2 | 192 | 64 | 75.0% | 93.8%（d=8）/ 98.6%（d=12） |
| 10% | 3 | 160 | 96 | 62.5% | 100%（d=10） |
| 20% | 4 | 128 | 128 | 50.0% | 99.6% |
| 30% | 4 | 128 | 128 | 50.0% | 99.6% |

#### 3.8.7 结论

**Grouped 滑窗 RLNC 是 256 五元组 + 1G + 流式的唯一现实路径**：

- 编码成本从单码的 14 GB/s 降到 1.5–2.0 GB/s（arm64 SIMD 5–15% 单核）。
- 码率代价：p=10% 时 62.5%（goodput 625 Mbit/s），p=20% 时 50%（500 Mbit/s）。
- 延迟：21–42 ms 窗口，可接受。
- 内存：2.5 MiB（32 组）。
- **mips 仍不可行**（1.5 GB/s > DRAM 271 MiB/s）。

**如果 grouped 也不可接受**（例如必须 87.5% 码率），只剩两个选项：

1. **降 N/M 到 ~20 carriers**（例如 4 组 × 5），单组成本更低，但五元组分离收益下降。
2. **降吞吐到 300–500 Mbps**，让单码或 grouped 都落在 mips/arm64 的预算内。

**需要用户决策**：接受 62.5–50% 的码率换 1G + 256 五元组，还是降 carrier 数/吞吐。

## 4. 块式 vs 流式：尾延迟是决定性的

这一节是设计的选择依据，不是可选项。**结论：流式滑窗是首选；块式只保留为对照与极端 fallback。**

### 4.1 块式的尾延迟从哪来

块式：每 `B` 字节一块，编码成 `N+M` 份 share，交付条件是「凑够 N 份」。

```
块完成时刻 = 第 N 小的 share 到达时刻 = max of N
```

即使无丢失、所有载体健康，块完成也要等 **N 条载体中最慢的那一份**。这是 `max of N` 的统计量：

- N=1：单包延迟。
- N=4：P99 约为单包的 1.5–2.5 倍（取决于速率分布与抖动）。
- 若某条载体慢，且慢载体数 > M，第 N 份落在慢载体上，块被直接拖慢。

**关键**：这个等待发生在**正常路径**，不是异常路径。只要块式用「N 份 share 凑齐」作为交付条件，它就必须付 `max of N`。

**N=224 时这个代价急剧放大。** 224 个独立同分布到达时间的最大值，期望约为单包的 `H_224 ≈ 6.3` 倍（调和数），P99 更高。也就是说，块式在 256 五元组部署下，即使所有载体健康、零丢失，交付延迟也是单包的 6 倍以上；而流式的正常路径是单包。这不是参数调优能消除的，是「交付条件」本身决定的。

### 4.2 流式的尾延迟

流式：数据包按 seq 独立到达、独立交付。

```
无丢失：交付延迟 = 单包到达时间（没有 max）
有丢失：交付延迟 = 覆盖该包的 parity 到达时间
```

没有 `max of N`。慢载体只影响它自己承载的包；若它承载的数据包迟到，由 parity 恢复，而 parity 走另外 M 条载体。

**代价**：parity 的 wire 开销与编码成本（§8）。但这是**异常路径**的成本，正常路径只付 `(K+M)/K` 的 wire。

### 4.3 定量：同一组载体，两种方案的 P99

设 N=4、M=1、S=8 KiB、RTT 218 ms、四条数据载体各 4.25 Mbit/s（合计 17 Mbit/s）、一条 parity 载体 4.25 Mbit/s。

块式（B=32 KiB，每块 4 份数据 share + 1 份 parity share）：

```
块完成 = 第 4 份 share 到达
parity share 走第 5 条载体，排第 5，不影响
第 4 份 ≈ 四条数据载体中最慢的一条 + 抖动
```

单份 share 传输 = `8 KiB / 4.25 Mbit/s` = 15.4 ms。四条并行，块完成 ≈ 15.4 ms + jitter；P99 由 `max of 4` 的尾部决定，约 1.5–2.5× 中位数。

流式（K=8、W=16、每 8 个数据包 1 个 parity）：

```
无丢失：交付 = 单包到达，P99 由单包 jitter 决定
有丢失：交付 = 覆盖它的 parity 到达
```

parity 每 8 个数据包发一次，覆盖最近 16 个包。丢失 `d_k` 后，覆盖它的最近 parity 在组尾之后发出。8 个数据包在 4 条载体上并行 = 2 轮 = 30.8 ms；加 parity 自身 1 轮传输 = 46 ms 最坏；平均约为一半。

**这里的对比不是「流式永远更快」**：无丢失时流式明显更快（单包 vs max of 4）；有丢失时流式的恢复延迟取决于 K，可能比块式的单块完成更慢。所以 K 的选择是「恢复延迟 vs wire」的权衡。

### 4.4 尾延迟对比表

| 方案 | 无丢失 P99 | 丢失时恢复延迟（最坏） | wire | 编码成本 |
|---|---|---|---|---|
| 块式 B=32 KiB, N=4, M=1 | `max of 4` ≈ 1.5–2.5× 单包 | 等该块凑够 4 份（≤ 1 轮） | 1.25 | 1× 数据量 |
| 块式 N=224, M=32 | `max of 224` ≈ 6.3× 单包 | 等 224 份到齐 | 1.143 | 32× 数据量 |
| **流式 RLNC N=224, M=32** | **单包** | **一个 parity 周期（K/M=7 包）** | **1.143** | **2.29 XOR** |
| 块式 B=32 KiB, N=4, M=2 | `max of 4` | 同上 | 1.50 | 2× 数据量 |
| **流式 K=4, W=8, M=1** | **单包** | **2 轮 = 31 ms** | **1.25** | **2× 数据量** |
| **流式 K=8, W=16, M=1** | **单包** | **3 轮 = 46 ms** | **1.125** | **2× 数据量** |
| 流式 K=8, W=16, M=2 | 单包 | 3 轮 = 46 ms | 1.25 | 4× 数据量 |
| 流式 K=16, W=32, M=2 | 单包 | 5 轮 = 77 ms | 1.125 | 4× 数据量 |

延迟按每条数据载体 4.25 Mbit/s（N=4 时总 17 Mbit/s）、S=8 KiB 估算：1 包 = 15.4 ms，1 轮 = N 条载体并行发 N 个包 = 15.4 ms。恢复延迟 = `(ceil(K/N) + 1) × 15.4 ms`：等待组尾发出 `ceil(K/N)` 轮，加 parity 自身传输 1 轮。这是最坏情况（丢失包恰在组首）；平均是它的一半左右。

**读数**：
- 无丢失时，流式的 P99 是单包；块式是 `max of N`。这是流式的结构性优势，不依赖参数。
- 有丢失时，流式的恢复延迟 `≤ K 包`；K 越小越快，但 wire 越高（`(K+M)/K`）。
- K=8、W=16、M=1 是甜点：wire 1.125（比块式 1.25 更低），最坏恢复 46 ms，编码量 2×。
- M=2 时 K=16、W=32：wire 1.125，最坏恢复 77 ms，编码量 4×。

### 4.5 为什么块式仍然保留

三个用途：

1. **对照基线**：P0/P1 的 host 原型同时实现块式与流式，用同一组 trace 比较 P50/P99、CPU、wire。这是「流式更好」的证据，不是断言。
2. **极端 fallback**：若实测发现流式的方程恢复在某些载体故障模式下不如块式（例如所有 parity 载体同时断，只剩 N 条数据载体），可以按配置切到块式（`fec=block`），不需要改 wire 格式。
3. **退化测试**：`K=W` 时流式退化为块式；这条路径必须正确，它是流式实现的自检。

**切换条件**（配置项 `fec=block`，默认 `auto`）：不自动切换；由运维在实测后决定。自动切换会让故障归因变难。`fec=block` 时 `K=W`，编码退化为块式，wire 格式不变。

### 4.6 流式不是银弹：它在哪里输

- **所有 parity 载体同时断**：只剩 N 条数据载体，丢失的包无法恢复，只能 NACK 或 reset。块式同样，但它至少还有 M 份 share 分散在数据载体上（若块式把 parity 也放数据载体）。
- **丢失集中在同一窗口**：M=1 时窗口内缺 2 个包，需要等下一个窗口的 parity；若下一个窗口也缺，恢复延迟叠加。K 越小，窗口重叠越多，这个风险越低。
- **窗口外**：`next_deliver` 落后超过 `W+K` 后不可恢复，必须 reset。块式的 reset 边界是「一块」，流式是「一个窗口」；两者量级相同。

---

## 5. 发送端状态机

### 5.1 对象

```c
struct tbt_fec_tx_pkt {
    u64 seq;
    u16 len;
    u8  *data;             /* S 字节，含零填充 */
    u64 enqueue_ns;
    struct list_head node;
};

struct tbt_fec_tx_group {
    u64 group;             /* g */
    u64 win_start, win_end;
    u8  parity_sent;       /* 已发出的行位图 */
    u8  *parity[M];        /* M × S */
    struct list_head node;
};

struct tbt_fec_tx {
    u64 next_seq;          /* 下一个数据包序号 */
    u64 next_group;        /* 下一个 parity 组号 */
    u32 inflight_pkts;     /* 已发未确认的数据包数 */
    u32 window_pkts;       /* 发送窗口，packet 数 */
    struct list_head data_q;    /* 待发的数据包 */
    struct list_head group_q;   /* 待生成/待发 parity 的组 */
    u8  *filling;          /* 正在填的 packet，S 字节 */
    u32 fill_bytes;
    u64 fill_start_ns;
};
```

每方向一个（near→far、far→near 各一份）。

### 5.2 状态

```
IDLE
  │ 公共 socket 有字节
  ▼
FILL ──填满 S──► QUEUE ──carrier 可写──► SEND_DATA
  │                                        │
  │ flush 定时器 / EOF                     │ 每 K 个数据包
  ▼                                        ▼
QUEUE（短包）                          ENCODE_GROUP ──M 份 parity──► SEND_PARITY
                                                                      │
                                                                      ▼
                                                                   WAIT_ACK
```

- `FILL`：`tbt_capture_one()` 把 skb 字节 `skb_copy_bits()` 进 `filling`。不做 per-frame 切分。
- 封包条件（三者之一）：
  1. `fill_bytes == S`；
  2. flush 定时器到期（默认 `max(2 × SRTT, 5 ms)`）；
  3. 上游读空且 `fill_bytes > 0` 且距上次写入超过 flush 间隔。
- `QUEUE`：数据包按 `seq % N'` 选目标数据载体（`N'` 是承载数据包的 carrier 数）；该载体满则换下一条（降级，计数 `fec_pkt_offpath`）。
- `ENCODE_GROUP`：每 `K` 个数据包封一组，计算 M 份 parity。窗口 `[win_start(g), win_end(g)]` 从数据包环形缓冲取。
- `SEND_PARITY`：第 m 行走 parity 载体 `N' + (m % M')`（`M'` 是承载 parity 的 carrier 数）。
- `WAIT_ACK`：`ACK.next_deliver` 推进后，释放 `seq < next_deliver` 的数据包与 `win_end < next_deliver` 的组。

### 5.3 调度

```
on carrier_writable(c):
    while (c->budget > 0):
        p = pick_packet(c)
        if (!p) break
        write_record(c, p)
        c->budget -= record_len(p)

pick_packet(c):
    /* 1. parity 载体只发映射到自己的行 */
    if (c->idx >= N_prime)
        return parity_head_matching(c->idx - N_prime)

    /* 2. 数据载体：取 seq 最老、且当前 inflight 最小的数据包 */
    return data_q_head_lowest_inflight(c)
```

规则：

- **数据包不绑定载体。** 按 seq 升序从 `data_q` 取，选当前 `inflight` 最小的数据载体。慢载体自然少分到包，不需要动态角色交换。
- **parity 行绑定 carrier 组。** 第 m 行走 `N' + (m % M')`；M 行必须分散到 `M'` 条 carrier，否则同一载体的丢失会同时打掉多行。
- **`N'` 与 `M'` 的划分**：`N' = max(1, round(carriers × N/(N+M)))`，`M' = carriers - N'`。carriers = N+M 时退化为 `N'=N, M'=M`（一 share 一 carrier）。
- 数据包优先于 parity：parity 迟到只影响恢复，数据迟到影响交付。
- 同一组的 M 份 parity 连续发出，让接收端尽快拿到完整方程组。
- 若目标载体 `-EAGAIN`，不等待：数据包换下一条载体（`fec_pkt_offpath++`）。这不影响正确性——接收端只认 seq。

### 5.3.1 控制帧不能被 partial record 堵住

**实测 bug**：N=1 慢载体上，FBK/控制帧被一个未写完的 DATA record 堵在同一条 64 KiB 载体的字节流后面（near `fbk_tx=253`、far `fbk_rx=0`，最终 EOF）。原因：控制帧排进了同一条 carrier 的写队列，而该 carrier 正在写一个 partial record；partial 写不完，控制帧永远发不出去。

**FEC 的规则**（三条，至少满足一条）：

1. **控制帧走独立 carrier。** 预留一条（或一组）carrier 只承载 `FEC_FBK`/`FEC_CFG`/`ACK`/`NACK`，不与 data/parity 共享写队列。代价是多一条 TCP，收益是控制面永远不被数据面堵住。
2. **同一 carrier 上，控制帧只在「无 partial record」时插入。** 发送端维护 `partial_len`；`partial_len > 0` 时控制帧不能插队，只能等当前记录写完。这要求发送端能感知「当前记录是否完整写出」。
3. **接收端区分控制通道。** 若控制帧与数据混在同一条 carrier，接收端 parser 必须能从字节流中识别记录边界（40 字节头 + `payload_len`），并且发送端保证控制帧不插进 partial record 的中间。

**推荐 1 + 2 组合**：控制帧有独立 carrier；若独立 carrier 不可用，退到规则 2，并在 procfs 计数 `ctl_blocked_by_partial`。规则 3 是接收端的兜底，不是发送端的借口。

**host 模拟要覆盖**：`partial record + control frame` 场景——模拟一条 carrier 写到一半（`partial_len > 0`），此时插入 `FEC_FBK`，验证它要么走独立 carrier，要么等记录写完，绝不进入 partial 的中间。

### 5.4 发送窗口

```
W_send = clamp(ceil(k × BDP_wire / (S × (K+M)/K)), W_min, W_max)
k      = clamp(RTO / mean_srtt, 2, 5)
BDP_wire = (mean_srtt + fb_delay) × Σ_i rate_i
W_max  = min(floor(fec_tx_mem_max / (S × (K+M)/K)), fec_window_pkts_max)
```

- `rate_i`、`srtt_i` 取自每条载体自己的 TCP BBR 估计，与 `w-window` 的 `tbt_tx_bdp_window()` 同源。
- 分母是「一个数据包在链路上占的 wire 字节」：`S × (K+M)/K`。
- `W_min = 2 × K`（至少两组，保证 parity 有数据可覆盖）。
- `W_max` 默认 512 包（4 MiB 应用数据）。
- 显式配置 `fec_window_pkts=N` 覆盖 auto。

**数值例**（N=4, M=1, K=8, W=16, S=8 KiB, BDP_wire=465 KiB, k=2）：
`W_send = ceil(2 × 465 / (8 × 9/8)) = ceil(930 / 9) = 104` 包。
应用 in-flight = 104 × 8 KiB = 832 KiB；wire in-flight = 936 KiB = 2 × 465。对。

### 5.5 发送端内存

```
tx_mem = W_send × S × (1 + M/K) + W × S × M/K   /* 数据包 + parity 组 */
```

`N=4, M=1, K=8, W=16, S=8 KiB, W_send=104`：
- 数据包 104 × 8 KiB = 832 KiB；
- 组缓冲：`W/K = 2` 个组 × 16 × 8 KiB × 1 = 256 KiB（每组覆盖 W 个数据包，但只存 parity M 份 + 引用）；
- 合计约 1.1 MiB。

### 5.6 失败处理

| 事件 | 处理 |
|---|---|
| 载体写返回 `-EAGAIN` | 数据包换载体（`fec_pkt_offpath++`）；parity 留队等 `sk_write_space` |
| 载体 `sk_state != ESTABLISHED` | 标记 lost；该载体队列里未写出的 packet 重新入全局队列 |
| 载体已写出但连接断 | packet 视为「可能丢失」；不主动重发，等接收端 NACK |
| 接收端 NACK 某 seq | 若包仍在窗口内：重新入队；若已释放：忽略（接收端会超时 reset） |
| 窗口满 | 停止封新包，继续发已有 packet；`fec_win_block++` |
| 流结束 | 封最后一个短包，发完所有 parity，发 `FIN` |

**为什么载体断了不主动重发已写 packet。** 载体 TCP 的 FIN/RST 不保证对端没收到数据；主动重发会让 wire 翻倍，而接收端 NACK 是精确的。

---

## 6. 接收端状态机

### 6.1 对象

本节的结构就是 RLNC 的；`win` 的方程表示见 §3.2，`epoch` 的划分见 §3.7.4。

```c
struct tbt_fec_rx_pkt {
    u64 seq;
    u8  *data;             /* S 字节；未收到为 NULL */
    bool crc_ok;
};

struct tbt_rlnc_win {
    u64 win_start, win_end;
    u8  *eq_rows;          /* 收到的 parity 方程，每行 W 位 */
    u8  have;              /* 已收到哪些 parity 行（M 位） */
    u16 missing_total;     /* 窗口内缺失的数据包数 */
    u64 *missing_seq;      /* 缺失的 seq，最多 W 个 */
};

struct tbt_fec_rx {
    u64 next_deliver;
    u32 buffered;
    u32 credit;
    struct tbt_fec_rx_pkt *pkt;     /* 环形缓冲，覆盖 [next_deliver, next_deliver+W_send) */
    struct tbt_rlnc_win    *win;    /* 环形缓冲，覆盖 [g, g+W/K) */
    u32 mem_used;
};
```

### 6.2 状态

```
WAIT_FIRST
  │ 任一 packet 到达
  ▼
COLLECT ──next_deliver 已收到──► DELIVER ──► 推进 ──► 窗口滑动
  │
  ├── next_deliver 缺失，且有可解方程 ──► RECOVER ──CRC ok──► DELIVER
  │
  └── 无方程且超过 fec_timeout ──► NACK ──仍不足──► RESET
```

### 6.3 收 packet

```
on_record(hdr, payload):
    validate(hdr)                       /* §2.2 全部检查 */
    if (hdr.seq < rx->next_deliver):
        late_pkt++; return              /* 迟到，丢弃 */
    if (hdr.seq >= rx->next_deliver + W_send):
        out_of_window++; return         /* 对端越界或反馈丢失 */
    switch (hdr.type):
    case DATA:
        if (rx->pkt[hdr.seq].data) { dup_pkt++; return }
        rx->pkt[hdr.seq].data = copy(payload, S)
        rx->pkt[hdr.seq].crc_ok = crc32c(payload) == hdr.crc32c
        update_windows_on_data(hdr.seq)
        try_deliver()
    case PARITY:
        g = group_of(hdr.seq, hdr.win_len)
        update_window_on_parity(g, hdr.row, payload)
        try_recover()
```

### 6.4 增量累加

```
update_windows_on_data(i):
    for g in groups_covering(i):        /* 最多 ceil(W/K) 个 */
        w = &rx->win[g]
        if (!w->active) continue
        for each parity row m covering i:
            if (coef(m, i - w->win_start))
                xor_into(w->rhs[m], d_i, S)
        w->missing_total--
        remove_from_missing(w, i)

update_window_on_parity(g, m, p):
    w = &rx->win[g]
    w->have_parity |= BIT(m)
    xor_into(w->rhs[m], p, S)
```

### 6.5 恢复与交付

```
try_recover():
    if (rx->pkt[next_deliver].data) { try_deliver(); return }
    for each group g covering next_deliver:
        w = &rx->win[g]
        k = w->missing_total
        if (k == 0 || k > M) continue
        if (popcount(w->have_parity) < k) continue
        rows = select k rows from have_parity
        if (!submatrix_invertible(rows, w->missing_seq, k)) continue
        solve k×k system -> recovered[k] of S bytes
        for each recovered j:
            if (crc32c(recovered[j], S) != hdr.crc32c) { crc_fail++; return }
            rx->pkt[seq_j].data = recovered[j]
        try_deliver(); return
    /* 无方程可解，等待 */
```

`try_deliver()` 从 `next_deliver` 起逐包交付，直到遇到缺失或 `-EAGAIN`。

### 6.6 顺序交付

```
deliver():
    while (rx->pkt[rx->next_deliver].data):
        p = &rx->pkt[rx->next_deliver]
        off = 0
        while (off < p->len):
            n = tbt_sock_deliver(stream, p->data + off, p->len - off)
            if (n <= 0) { /* -EAGAIN：保留 off，等 sk_write_space */ return }
            off += n
        free(p->data); rx->next_deliver++
        rx->buffered--; rx->credit++
        advance_windows()               /* 滑出不再需要的组 */
    send_ack()                          /* 合并：每 10 ms 或每 8 包一次 */
```

交付用 `tcp_sendmsg_locked()` + `MSG_DONTWAIT`，短写保留偏移，`-EAGAIN` 等 `sk_write_space`。与旧 `tbt_sock_deliver()` 完全一致，接口不变。

### 6.7 窗口滑动

组 `g` 在 `win_end(g) < next_deliver` 时不再需要，释放其 `rhs` 与缺失表。新组在 `win_start(g) <= next_deliver + W_send` 时创建。

窗口数量上界：`ceil(W_send/K) + 2`。

### 6.8 迟到、乱序、重复

- 乱序：数据包按 seq 存放在环形缓冲，不要求到达顺序。
- 迟到：`seq < next_deliver` 直接丢弃（`fec_pkt_late++`）。
- 重复：`pkt[seq].data != NULL` 时丢弃（`fec_pkt_dup++`）。
- parity 迟到：组已释放则丢弃（`fec_parity_late++`）。

### 6.9 超时、NACK、reset

```
sweep（每 10 ms）:
    if (rx->pkt[next_deliver].data) return
    age = now - rx->first_missing_ns
    if (age > fec_timeout_ms && !nack_sent):
        send_nack(next_deliver, contiguous_missing_count)
        nack_sent = true
    if (age > fec_reset_ms):
        stream_reset(ECONNRESET)
```

默认值：

- `fec_timeout_ms = 500`（大于 2×RTT + 反馈延迟；也是「等下一个窗口 parity」的上界）。
- `fec_reset_ms = 4 × fec_timeout_ms = 2000`。

**窗口外不可恢复的精确判据**：若 `next_deliver < max_received_seq - (W + K)`，则覆盖 `next_deliver` 的所有组都已滑出（`win_end(g) < next_deliver`），没有任何在途方程能覆盖它。此时立即 reset，不必等 `fec_reset_ms`。这是比超时更早、更确定的判据。

### 6.10 接收端内存

```
rx_mem = W_send × S                      /* 数据包缓冲 */
       + ceil(W_send/K) × (M × S + W × 8) /* 组缓冲：M 份 rhs + 缺失 seq 表 */
```

`N=4, M=1, K=8, W=16, S=8 KiB, W_send=104`：
- 数据包 104 × 8 KiB = 832 KiB；
- 组 13 × (8 KiB + 32 B) ≈ 106 KiB；
- 合计约 940 KiB。

硬上限 `fec_rx_mem_max`（默认 32 MiB）决定 `W_max`：

```
W_max = min(floor(fec_rx_mem_max / (S + M×S/K + W×2/K)), fec_window_pkts_max)
```

### 6.11 接收端背压

- `credit` 随交付递增；`ACK` 携带。
- 若 `buffered >= W_max`：停止从载体读（`tbt_car_rx_actor` 返回 0），载体接收窗口关闭，发送端 TCP 自然受阻。
- 若公共 socket 交付 `-EAGAIN` 且 ready 队列达上限：同样停止读载体。

---

## 7. 流控、窗口、credit

### 7.1 三层约束

1. **载体 TCP 自己的窗口/拥塞控制**：每条 carrier 独立，模块不干预。
2. **发送窗口 `W_send`**：限制「已发未确认」的数据包数。
3. **credit**：接收端显式通告的剩余容量，是 `W_send` 的上界来源之一。

### 7.2 公式

```
BDP_i    = (srtt_i + fb_delay) × rate_i
BDP_wire = Σ_i BDP_i
k        = clamp(RTO / mean_srtt, 2, 5)
W_send   = clamp(ceil(k × BDP_wire / (S × (K+M)/K)), 2K, min(W_max, credit))
W_max    = min(floor(fec_rx_mem_max / per_pkt_mem), fec_window_pkts_max)
```

`RTO = max(rtt_max + 4 × rttvar_max, 200 ms)`，与 `w-window` 的 `tbt_tx_rto_ns()` 同形。

**为什么除以 wire 膨胀 `(K+M)/K`。** `rate_i` 是载体 TCP 的 wire 速率，`BDP_wire` 是链路能容纳的 wire 字节；而 `W_send × S` 是应用字节。一个数据包在链路上占 `S × (K+M)/K` 字节。不除以膨胀系数，实际 wire in-flight 会多出 `(K+M)/K` 倍，导致队列堆积。

### 7.3 为什么是 `k × BDP`

WAN 实测：218 ms、17 Mbit/s 的路径 BDP 约 465 KB；旧实现把 in-flight 钳在 120–131 KB，理论上限 3.3–4.1 Mbit/s，实测 3.45–4.27，完全吻合。`k` 覆盖一个丢包修复周期：`k = RTO/mean_srtt ∈ [2,5]`，保证窗口内有足够多的包让接收端在等待某包时仍有活干。

### 7.4 credit 协议

- 接收端初始 `credit = W_max`。
- 每交付一包 `credit++`；每接收一个包 `credit--`。
- `ACK` 每 10 ms 或每 8 包发一次，携带 `next_deliver` 与当前 `credit`。
- 发送端：`W_send = min(W_auto, credit)`。收到 ACK 时更新 `credit` 并释放 `seq < next_deliver` 的包。
- 反馈丢失：若 200 ms 未收到 ACK 且窗口满，发一个 `ACK_REQ`（复用 0x32，`credit_packets = 0xffffffff`）；连续 3 次无响应（约 600 ms）才 reset。

### 7.5 上界汇总

| 项 | 公式 | 默认值（N=4,M=1,K=8,W=16,S=8K,W_send=104） |
|---|---|---|
| 发送端数据包缓冲 | `W_send × S` | 832 KiB |
| 发送端组缓冲 | `ceil(W/K) × (M×S + W×8)` | 约 18 KiB |
| 接收端数据包缓冲 | `W_send × S` | 832 KiB |
| 接收端组缓冲 | `ceil(W/K) × (M×S + W×8)` | 约 108 KiB |
| 每载体 socket 缓冲 | 128 KiB–4 MiB，握手前设置 | 5 × 512 KiB = 2.5 MiB |
| 控制记录 | 每 10 ms 一条 48 B | ~5 KB/s |

---

## 8. 1G 预算与参数建议

### 8.1 目标线

1 Gbit/s = 125 MB/s 应用数据。这是单核预算的分母。所有「每字节运算量」都乘以 125 MB/s 得到绝对操作数。

### 8.2 每字节成本

RLNC 的编码成本 = `(M/N) × d` XOR/byte。生产配置 `N+M=256`，`M` 动态。

| 配置 | ops/byte | 1G 下绝对量 | SIMD 32B 指令/s | 恢复保证 |
|---|---:|---:|---:|---|
| M=16/N=240, d=16 | 1.07 | 133 M XOR/s | 4.2 M | 窗口内高概率 |
| **M=32/N=224, d=16** | **2.29** | **286 M XOR/s** | **8.9 M** | **窗口内高概率** |
| M=32/N=224, d=32 | 4.57 | 571 M XOR/s | 17.8 M | 窗口内高概率 |
| M=64/N=192, d=16 | 5.33 | 667 M XOR/s | 20.8 M | 窗口内高概率 |
| M=96/N=160, d=16 | 9.60 | 1.20 G XOR/s | 37.5 M | 窗口内高概率 |
| M=128/N=128, d=16 | 16.0 | 2.00 G XOR/s | 62.5 M | 窗口内高概率 |

**关键区别在「op」是什么**：

- **XOR** 可以 SIMD：x86 一条 AVX2 指令 32 字节，ARM NEON 16 字节；`286 M XOR/s ÷ 32 B` ≈ 8.9 M 指令/s，单核 < 1%。
- **mips 标量**：1 指令/字；`286 M XOR/s` 约 15–30% 单核（考虑内存与循环开销），仍远低于 GF 查表。

### 8.3 架构可行性（1G 单核）

| 架构 | M=16 d=16 | M=32 d=16 | M=64 d=16 | M=128 d=16 |
|---|---|---|---|---|
| x86_64 (AVX2) | <1% | <1% | 1–2% | 2–4% |
| arm64 (NEON) | <1% | 1–2% | 2–3% | 4–8% |
| mips（标量） | 5–10% | **15–30%** | 30–60% | **超预算 60–120%** |

数字是 op-count 模型 + 典型 IPC 估算，**不是实测**。`review/fec-prototype` 的 bench 会给出 x86 实测；arm64/mips 若有交叉工具链则补实测，否则文档标注「未实测」。

**mips 的结论**：1G 下 `M=32/N=224, d=16` 约 15–30% 单核，可行；`M=64` 约 30–60%，边缘；`M=128` 不可行。mips 的 `M_max` 应限制在 48–64，`d` 降到 8（成本减半）。


### 8.4 恢复延迟

数据包在 N 条载体上并行；`K=N` 个数据包的墙钟时间是 `K × S / 数据总率`，不是 `K / rate_per_carrier` 的串行和。

| 场景 | 每 carrier | 1 包（8 KiB） | 最坏恢复（K=N 个包 + 1 个 parity） |
|---|---:|---:|---:|
| 17M WAN, N=4 | 3.42 Mbit/s | 19.2 ms | 4 包 = 77 ms |
| **1G, N=224, W=2K=448** | **3.91 Mbit/s** | **16.8 ms** | **29.4 ms + 16.8 ms = 46.2 ms** |
| 1G, N=176, W=2K=352 | 3.91 Mbit/s | 16.8 ms | 23.1 ms + 16.8 ms = 39.9 ms |

**恢复延迟由 `W=2K` 和数据总率决定**：`448 × 8 KiB / 125 MB/s = 29.4 ms`（丢失包恰在窗口最老端）。加一个 parity 包的传输时间（16.8 ms），最坏约 46.2 ms。平均是它的一半。这比块式的 `max of 224`（约 6.3 倍单包）更可预测，因为 parity 的发出时刻是确定的。


### 8.5 wire 开销

```
overhead = (K+M)/K × (1 + 32/S)
```

| K | M | S | overhead | 最坏恢复（17M，N=4） | 最坏恢复（1G，256 carriers） |
|---:|---:|---:|---:|---:|---:|
| 4 | 1 | 8 KiB | 1.254 | 31 ms | 1.0 ms |
| 8 | 1 | 8 KiB | 1.129 | 46 ms | 1.6 ms |
| 16 | 1 | 8 KiB | 1.065 | 77 ms | 2.6 ms |
| 8 | 2 | 8 KiB | 1.254 | 46 ms | 1.6 ms |
| 16 | 2 | 8 KiB | 1.129 | 77 ms | 2.6 ms |

对照旧实现：`dup=1` 的 `wire/submit = 2.018`；`K=8, M=1` 的 1.129 明显更省，且容错是结构性的。

### 8.6 packet 大小 tradeoff

| S | 头开销 | 包化延迟（1G，每载体 3.91M） | 适用 |
|---:|---:|---:|---|
| 1 KiB | 3.1% | 2.1 ms | 低延迟交互（头开销高） |
| 2 KiB | 1.6% | 4.2 ms | 低延迟 |
| 4 KiB | 0.78% | 8.4 ms | 折中 |
| **8 KiB** | **0.39%** | **16.8 ms** | **默认（零常量变更）** |

S 超过 8192 需要同步放大 `TBT_PAYLOAD_MAX`/`TBT_REC_MAX`/`TBT_CHUNK_MAX`/`c->part`。v1 固定 8 KiB。

### 8.7 参数建议（按场景）

| 场景 | 配置 | 理由 |
|---|---|---|
| 原型 / 17M WAN | `N=4, M=1, carriers=5, K=8, W=16, S=8K, d=8, fec=rlnc` | wire 1.129；最坏恢复 46 ms；XOR 成本 <1% |
| **1G + 256 carriers** | **不成立**（W=2K 时 d 需接近稠密，成本 25–115 XOR/byte） | 见 §3.5 |
| **1G 可行（arm64）** | `N=16, M=4, K=16, W=32, d=16, S=8K` | 4 XOR/byte；500 MB/s；goodput 800 Mbit/s |
| 1G mips | 同上，但 `M_max=48`, `d=8` | 编码 5–15% 单核；goodput ≤ 812 Mbit/s |
| 中等规模对照 | `N=96, M=32, carriers=128, K=96, W=96, S=8K, d=16` | wire 1.333；2.29 XOR/byte |
| 对照 | `N=4, M=1, carriers=5, K=W=16`（块式退化） | 分离「滑窗」与「块式」的贡献 |

**q 的选择规则**：

- 小 M（1–2）：`q=2`（`K=W/2`）。重叠提供跨组修复：一个 parity 组丢失，下一组仍能覆盖该数据包。
- 大 M（≥8）：`q=1`（`K=W`）。编码成本已经很高，翻倍不可接受；parity 载体故障由 M 的冗余覆盖（M=32 时断 1 条仍有 31 个方程）。
- `q=1` 不是块式：数据包仍然逐包交付，只是每个包只被一个组覆盖。块式的 `max of N` 来自「凑够 N 份 share」，与 q 无关。

不推荐 `M ≥ N`：wire 超过 2×，收益递减。
不推荐 `K < 4`：parity 太频繁，wire 超过 1.5。
不推荐 `W > 4K`：组缓冲与编码量上升，恢复延迟不再改善（延迟由 K 决定）。

### 8.8 host 实测（x86_64，块式对照）

`review/fec-prototype` 的块式编解码已在 x86_64 上跑通（`-O2 -std=c99`，2026-11-06）：

```
test_fec: PASS — 2260 个解码组合 + 1000 轮 fuzz
(4,1) 20/20, (4,2) 60/60, (2,1) 12/12, (8,2) 180/180, (8,4) 1980/1980, (1,1) 8/8
```

| N,M | share_len | encode MB/s | decode MB/s | encode XOR/byte | decode XOR/byte |
|---|---:|---:|---:|---:|---:|
| 4,1 | 1024 | 76,636 | 6,107 | 1 | 4 |
| 4,1 | 16384 | 64,772 | 14,147 | 1 | 4 |
| 4,1 | 65536 | 66,276 | 15,701 | 1 | 4 |
| 4,2 | 1024 | 2,302 | 328 | 2 | 4 |
| 4,2 | 16384 | 2,318 | 559 | 2 | 4 |
| 4,2 | 65536 | 2,308 | 574 | 2 | 4 |
| 8,2 | 16384 | 2,271 | 251 | 2 | 8 |

读数（折算 1G = 125 MB/s）：

- `M=1` 编码 65 GB/s，1G 下 **0.2%** 单核；这是内存带宽限制，不是 CPU。
- `M=2` 编码 2.3 GB/s，1G 下 **5.4%**；标量 GF 的成本已经可见，但可接受。
- `M=2` 解码 0.56 GB/s，若每个包都恢复，1G 下 **22%**；但恢复只在丢失时发生。
- `M=2` 的 encode 比 `M=1` 慢 28 倍，这个比值就是「GF 乘加 vs XOR」的价格。

**注意**：这是块式原型，不是流式。流式的增量累加成本结构不同（每包更新 `ceil(W/K)` 个组），实测数字等 P0 的 `bench_sliding` 出来再补。上表的用途是给「GF 乘加 vs XOR」一个硬锚点。

### 8.9 与旧实现的 wire 对照

| 配置 | wire/app | 容错 |
|---|---:|---|
| 旧 dup=0, n=4, m=1 | 1.038–1.041 | 无结构容错（spare 待机） |
| 旧 dup=1, n=4, m=1 | 2.016–2.018 | 概率性 |
| 新 K=8, M=1 | 1.129 | 窗口内任意 1 个丢失 |
| 新 K=16, M=2 | 1.129 | 窗口内任意 2 个丢失（MDS） |


## 9. 迁移计划与可复用代码

### 9.1 保留（不改或小改）

| 组件 | 文件 | 保留理由 | 需要的改动 |
|---|---|---|---|
| 内核 listener / accept | `tbt_listen.c` | 与数据面无关 | 无 |
| 载体生命周期、`sk_prot` 覆盖、替换循环 | `tbt_carrier.c` | 载体是 FEC 的传输层 | 队列元素从 frame 改为 packet；`carrier_mask` 记账 |
| 载体 socket 缓冲前置设置 | `tbt_carrier.c`（`tbt_car_setsockbuf`） | 握手前设置 sndbuf/rcvbuf，避免首包被小缓冲卡住 | 直接复用；`n_pri` 改为 `N+M` |
| parser 自旋修复 | `tbt_carrier.c` | 持锁 parser 记录时空转 | 直接复用 |
| raw 空闲 stall 修复 | `tbt_carrier.c` | 无未确认字节时重置 stall 计时 | 直接复用；判据改为「该载体无未确认 packet」 |
| HTTP 请求/响应生成、chunk 编解码、PAD | `tbt_http.c` | 容器只是 QoS profile | 接受新 type；`S` 不变 |
| 公共 socket 捕获与交付 | `tbt_sock.c` | `tcp_read_sock` / `tcp_sendmsg_locked` 的形状不变 | 捕获侧写 packet 缓冲；交付侧写解码后的包 |
| procfs 控制面与 parser | `tbt_procfs.c`、`tbt_parse.c` | 操作面不变 | 新增 `packet=`、`fec=`、`k=`、`w=`、`fec_timeout_ms=`；删除 `dup=`、`max_inflate=` |
| session 生命周期、nonce、portblock | `tbt_session.c` | 与数据面无关 | HELLO 增加 FEC 参数 |

### 9.2 替换

| 旧机制 | 文件 | 新归宿 |
|---|---|---|
| per-frame seq 分配、发送队列 | `tbt_tx.c` | packet 切分 + 滑窗编码 + 调度（`tbt_fec_tx.c`） |
| retain ring、`tx_retain_bytes` | `tbt_rec.c`、`tbt_tx.c` | 发送窗口缓冲（`struct tbt_fec_tx_pkt`） |
| retx（hole/slow/pathloss）、dup、`max_inflate` | `tbt_tx.c` | 删除；只保留接收端 NACK 触发的选择性重发 |
| hole 几何检测、`0.919^g` 表、generation gate | `tbt_tx.c` | 删除 |
| 接收端 seq reorder、`next_expected`、`hole_budget` | `tbt_rx.c` | packet 环形缓冲 + 窗口方程 + 解码前沿（`tbt_fec_rx.c`） |
| `TBT_F_DATA` / `FBK` / `SACK` 的 per-frame 语义 | `tbt_proto.h`、`tbt_uapi.h` | `DATA` / `PARITY` / `ACK` / `NACK`；`conv 0x0006` |

### 9.3 新增文件

| 文件 | 职责 | 依赖 |
|---|---|---|
| `tbtun/tbt_rlnc.c` / `.h` | 流式 RLNC：PRNG 系数、GF(2) 消元、rank 维护、CRC | 无（纯 GF(2)） |
| `tbtun/tbt_fec_tx.c` | packet 切分、组编码、调度、窗口、NACK 响应 | `tbt_fec`、`tbt_carrier` |
| `tbtun/tbt_fec_rx.c` | packet 缓冲、窗口方程、恢复、交付、credit、超时 | `tbt_fec`、`tbt_sock` |

依赖是平的：`tbt_fec` 不知道 carrier，`tbt_fec_tx` 不知道 HTTP。

### 9.4 与 overhaul（`w-window`）的接口与复用

**前提：旧 per-frame ARQ 不作为 256 carriers 的底座。** 它的 16-carrier 限制是结构性的（§11.2），FEC 数据面用独立的新 wire format；能复用的只有载体生命周期、HTTP 容器、sock 交付、procfs 与 portblock。

`w-window` 分支继续修 per-frame ARQ 的 bug，它不会停；FEC 不假设旧数据面已冻结。head 是 `a33aeb8`（2026-11-06 05:22）。

**直接复用（与 FEC 无关，属于载体/窗口层）：**

| 修复 | 位置 | 为什么 FEC 需要 |
|---|---|---|
| 握手前设置 carrier `sndbuf`/`rcvbuf` | `tbt_carrier.c` 的 `tbt_car_setsockbuf`（`:1353-1382`） | FEC 的 packet 突发更集中；`n_pri` 改为 `N+M`；`SO_SNDBUFFORCE` 失败回落 `SO_SNDBUF`；内核会翻倍，故传入值除以 2 |
| parser 自旋修复 | `tbt_carrier.c:600-618` | 只有 `copied_seq != rcv_nxt` 才重排 `w_rx`；`parser->have` 交给 50 ms poll 与 `sk_data_ready` |
| raw 空闲 stall 修复 | `tbt_carrier.c:307-399` | 三个重置分支：`una == nxt`、progress、`paused || !snd_wnd`；阈值 `max(3×RTO, 3 s)` |
| `W = k × BDP` 窗口模型 | `tbt_tx.c:517-649` | FEC 的 `W_send` 用同一套 BDP/RTT 采样，只换单位（字节 → packet） |
| procfs 诊断（`bdp_bytes`、`k_x100`、`rtt_mean_us`、`snd_wnd`、`window_clamp`、`wscale`） | `tbt_procfs.c`、`tbt_session.c` | FEC 需要同样的输入来证明窗口不是拍脑袋 |

**需要改造（同一概念，不同粒度）：**

| 项 | 旧 | 新 |
|---|---|---|
| 窗口判定 | `seq + len <= tx_ack + tx_window` | `inflight_pkts < W_send` |
| 队列上限 | `txq_max = tx_window + 8 × TBT_CHUNK_MAX` | `fec_txq_max = W_send + 8 × (K+M)/K` |
| 反馈 | `FBK` per-path + `SACK` per-stream | `ACK` per-stream + credit |
| 载体健康 | `snd_una` 静止 `carrier_stall_ms` | 不变；但 stall 只影响 packet 调度，不触发重传 |
| 窗口单位 | 字节 | packet；`W_send × S` 是应用字节 |

**整段废弃（per-frame ARQ 专用）：**

- retain ring、`replay` 预算、`tx_retain_bytes`
- `retx_hole` / `retx_slow` / `retx_pathloss` 三分支
- `dup`、`dup_degree`、`max_inflate`、`inflate_ppm`
- hole 几何检测、`late_decay` 表、`gate_fail`/`depth_fail`/`time_fail`
- 接收端 `next_expected` 的逐帧推进、`hole_budget_ms` 的逐帧语义
- `TBT_F_DATA`、`TBT_F_FBK`、`TBT_F_SACK` 的帧类型与校验

**接口约定：**

1. `tbt_carrier_send(c, buf, len)` 的签名不变；FEC 调度器是它唯一的调用者。
2. carrier 的 `sk_write_space` 回调只做 `schedule_work(&sess->fec_sched_work)`，不直接发。
3. carrier 的 `sk_state_change` 在 `ESTABLISHED` 之外的状态下，把该 carrier 的未发 packet 归还全局队列；由 `tbt_fec_tx.c` 的 `tbt_fec_tx_carrier_down(c)` 完成。
4. 载体层不解释 packet 头；它只保证字节流顺序。
5. 窗口公式以函数形式复用：`tbt_tx_bdp_window(sess, &mean, &var, &max)` 返回字节 BDP；FEC 侧 `W_send = DIV_ROUND_UP(k × bdp, S × (K+M)/K)`。

**三条会撞上的边界事实**（`0db5166` 实测，FEC 实现时必须处理）：

| # | 事实 | 位置 | FEC 的处理 |
|---|---|---|---|
| 1 | `c->part` 是每载体唯一的写缓冲，大小 `TBT_REC_MAX` | `tbtun.h:686`；`tbt_carrier.c:763-783` | `S=8 KiB` 时 packet 记录正好 8224 B，装得下；若加大 S，必须同步放大 |
| 2 | HTTP 容器 far 侧首写有闸门：`c->http_req_done` 为假时 `tbt_carrier_xmit` 返回 `-EAGAIN` | `tbt_carrier.c:687-691` | FEC 首包也会撞上；调度器必须把 `-EAGAIN` 当作「该载体暂不可用」而不是「packet 丢失」 |
| 3 | `spare` 现在**不参与 DATA**：`tbt_sched_rebuild` 跳过 `c->spare && !c->active` | `tbt_tx.c:1489-1491` | 这行必须删掉。FEC 的 M 条 parity 载体是冗余分量，不是备用；不删则 M 条永远拿不到 packet |

**对照数据（必须诚实读）。** `w-window` 的 QEMU bench 不是真实 WAN：

- 链路模型是 `benches/qemu/init` 的 htb（每源端口 17 Mbit/s、burst 32k）+ netem（双向 109 ms），RTT ≈ 218 ms；
- 但 slirp 4.9.5 不解析 `TCPOPT_WINDOW`，`snd_wscale=0`，`snd_wnd` 恒为 65535。`65535 × 8 / 0.218 s = 2.40 Mbit/s`，这是该 bench 的硬顶，与 `tx_window=1 MiB` 无关；
- 最新 CSV（`20261106T050520`）只跑了 1.18 MB；唯一完整的 16 MiB 双向行是 `045856`，`git_dirty=1`、`app 2.17 Mbit/s`、`wire/app 1.082`、`retx 55705 ppm`；
- `a33aeb8`（05:22）之后没有新数据；真实 WAN 的 `review/wan-experiment/` 是窗口修复前的 8d676dd 时代，只证明问题存在，不证明修复有效。

因此本设计的对照基线只有一条是硬的：`review/wan-experiment/summary.md` 的直连 P1/P4/P8 = 17.08/30.03/33.72 Mbit/s。FEC 的 P5 验收必须自己跑真实 WAN，不能引用 QEMU 的 2.4 Mbit/s 作为「已达标」或「未达标」。

### 9.5 分阶段计划

| 阶段 | 内容 | 验收 | 依赖 |
|---|---|---|---|
| P0 | host 纯函数库：RLNC 编码/解码、PRNG、GF(2) 消元；小 N/M 全组合 + iid 模拟 | `review/fec-prototype` 的 `make test` 全绿；`make sim` 给出 p 曲线；`make bench` 给出 x86 实测 MB/s | 无 |
| P0.5 | Kodo 测试向量交叉验证；1G 基准；arm64/mips 交叉编译（若有工具链） | 与参考实现逐字节对齐；给出 ops/byte 与实测/估算 | P0 |
| P1 | 内核内 `tbt_rlnc.c`，无 I/O；任意 N/M 参数化；host 单测复用同一份头文件 | 与 P0 相同的测试在内核编译选项下通过 | P0 |
| P2 | packet 切分 + packet 缓冲 + 顺序交付，`N=1, M=0` 退化路径 | QEMU 两 guest 字节精确传输 4 MiB；无 reset | P1 |
| P3 | `N=4, M=1, K=8, W=16` RLNC；调度；发送窗口；credit | QEMU + netem（1% 丢包、20 ms 抖动、乱序）下 4 MiB 字节精确；单载体 kill 后不 reset | P2 |
| P4 | procfs 配置/统计；`NACK`；载体故障恢复 | 配置可写可回读；任意 N/M 参数校验；NACK 后包恢复 | P3 |
| P5 | 真实 WAN `N=4, M=1, K=8, W=16` | 单流 ≥ 17.08 Mbit/s（直连 P1），`wire/app ≤ 1.2`，无 reset | P4 |
| P6 | `N+M=256, carriers=256, K=N, W=2K, d=16` | 窗口恢复率 ≥ 99.9%；256 carriers 资源符合 §11 | P5 |
| P7 | 1G 本地/局域网实测；arm64/mips 实测（若有硬件） | 1G 下 CPU ≤ 20%（mips）；≤ 5%（arm64） | P6 |

每个阶段独立可回退；P2 的 `N=1,M=0` 退化路径保证「FEC 框架本身不引入回归」。P0/P0.5 不写内核，先证明编码与预算。

---

## 10. 测试矩阵与验收标准

### 10.1 host 纯函数测试（最高优先级）

| 用例 | 输入 | 期望 |
|---|---|---|
| 任意 N/M 参数化 | `(N,M) ∈ {(1,1),(2,1),(4,1),(4,2),(8,2),(8,4),(16,8),(32,8),(96,32)}`；`(K,W) ∈ {(4,8),(8,16),(16,32)}` | 配置被接受，矩阵自检通过；`96/32` 只做参数校验与矩阵自检，不做全量恢复 |
| carrier 映射 | `carriers ∈ {4,8,16}`、`n+m ∈ {6,16,32,128}` | share `s` 走 `s % carriers`；一条 carrier 故障的 share 集合符合交错规则 |
| RLNC 恢复 | 窗口内 1..M 个丢失 | 逐字节等于原始 |
| rank 不足 | 窗口内丢失数 > 收到的方程数 | 返回不可恢复；继续等 parity；不崩溃 |
| 跨窗口丢失 | M+1 个丢失分布在不同窗口 | 仍可恢复（滑窗相对块式的关键优势） |
| 同窗口超限 | M+1 个丢失集中在同一窗口 | 返回不可恢复；不静默输出错字节 |
| 乱序 | parity 先于其覆盖的数据包到达；数据包乱序 | 结果与顺序无关 |
| 迟到 | 数据包在 parity 已用于恢复后才到 | 丢弃重复；不破坏前沿 |
| 窗口滑动 | `next_deliver` 从 0 推进到 L-1 | 每个位置的前沿推进条件正确；组释放正确 |
| 窗口外 | `next_deliver < max_seq - (W+K)` | 立即 reset 判据触发 |
| 边界 | `K > W`、`W > L`、`M=0`、`S=0`、`K=W`（块式退化）、`d > W`、`d == 0` | 非法组合拒绝；`K=W` 仍正确 |
| fuzz | 1000 轮随机 `(N,M,K,W)`、丢失集合、数据、乱序 | ASAN 无越界；CRC 全通过 |
| 1G 基准 | RLNC M=16..128，125 MB/s 数据，`S=8K`，`d=8/16/32` | MB/s 与 CPU%；M=32/d=16 ≤ 5% 单核（x86） |

### 10.2 尾延迟模拟（host，P0 的关键交付）

用同一组载体速率向量与抖动分布，比较块式与流式：

| 速率向量 | 块式 P99 | 流式 P99 | 结论 |
|---|---|---|---|
| 5 条均衡 3.4 Mbit/s | `max of 4` | 单包 | 流式应明显更低 |
| 4 快 + 1 慢 1.7 Mbit/s | `max of 4`（慢的是 parity，不影响） | 单包 | 块式接近流式 |
| 3 快 + 2 慢（N=4, M=1） | `max of 4` 落在慢载体 | 单包（慢的是 parity） | 流式明显更低 |
| 3 快 + 2 慢（N=4, M=2） | 同上 | 单包 | 流式明显更低 |

模拟必须输出 P50/P90/P99/P999 与「交付被卡住的总时长」。若某组向量下流式不占优，如实记录并解释。

### 10.3 QEMU 两 guest

沿用 `benches/qemu` 与 `review/dkms-qemu` 的拓扑：两个 guest，中间 `netem`。

| 用例 | netem | 期望 |
|---|---|---|
| 基线 | 无 | 4 MiB 字节精确；`wire/app ≈ 1.13`（K=8,M=1） |
| 丢包 | `loss 1%` | 字节精确；`fec_pkt_recovered > 0`；无 reset |
| 突发丢包 | `loss 5% burst` | 字节精确；`fec_crc_fail = 0` |
| 乱序 | `delay 20ms 10ms distribution normal` + `reorder 25% 50%` | 字节精确；`fec_pkt_late` 有计数但无 reset |
| 单载体 kill | 运行中 `ip link set down` 一条 carrier | 包恢复率 100%；`carriers_up` 恢复后继续 |
| 两载体 kill（M=1） | 同时 down 两条 | 短暂等待后 reset；`fec_timeout` 计数正确 |
| 窗口 | `fec_window_pkts` 显式设为 1 BDP / 2 BDP | `inflight_pkts` 跟随；吞吐随窗口单调 |
| 内存 | 持续 60 s 满速 | `fec_rx_mem_kb` 不超过 `fec_rx_mem_max` |
| 128 carriers（资源） | 创建 128 条 carrier，空闲 60 s | 总内存 ≤ 固定 0.7 MiB + 32 MiB 缓冲；无 OOM；`/proc/net/tbtun/peers` 128 行 |
| 128 carriers（吞吐） | 真实 WAN 或两台物理机 | 单流 ≥ 目标带宽；CPU 符合 §11.4 模型；`carriers_up=128/128` |
| 128 carriers（故障） | 断 1/8/32 条 | 分别恢复；断 33 条时 reset 且有明确计数 |

### 10.4 真实 WAN

拓扑与 `review/wan-experiment` 相同：JYSZ（near，NAT）→ HND0（far），RTT 约 218 ms，直连 P1 17.08 Mbit/s。

| 配置 | 目标 |
|---|---|
| `N=4, M=1, K=8, W=16, S=8 KiB` | 单流 ≥ 17.08 Mbit/s；`wire/app ≤ 1.2`；`fec_reset = 0` |
| `N=4, M=2, K=16, W=32` | ≥ P5 的 90%；窗口内任 2 丢失可恢复 |
| `N=4, M=1, K=W=16`（块式退化对照） | 与流式同链路对比 P99 延迟与吞吐 |
| `N=8, M=2, K=16, W=32, S=4 KiB` | 探索；不设硬指标 |

每次运行必须记录：`tunnel_mbit`、`wire_per_app`、`inflight_pkts`、`bdp_bytes`、`k_x100`、`carriers_up`、`fec_pkt_recovered`、`fec_pkt_late`、`fec_nack_tx`、`fec_reset`、`fec_rx_mem_kb`、`fec_block_latency_p50/p99`、near/far CPU。少于 30 s 的运行不作为结论。

### 10.5 验收标准（总）

1. **正确性**：host 全组合测试 100% 通过；QEMU 所有用例字节精确；CRC 失败数为 0（除非注入故障）。
2. **容错**：`N=4, M=1` 下任一载体断开，4 MiB 传输不 reset；`N=4, M=2` 下任两载体断开不 reset。
3. **尾延迟**：真实 WAN 上 `fec_block_latency_p99` 不高于块式退化配置的 70%。
4. **吞吐**：真实 WAN 单流 ≥ 17.08 Mbit/s（直连 P1 基线），且不低于 `w-window` 同链路最好值。
5. **开销**：`wire/app ≤ 1.2`（K=8/M=1）；`wire/app ≤ 1.2`（K=16/M=2）。
6. **内存**：`fec_rx_mem_kb ≤ fec_rx_mem_max`；发送端缓冲 ≤ `W_send × S × (1 + M/K)`。
7. **CPU**：1 Gbit/s 下 `cpu_total_pct ≤ 50%`（单核，含 TCP 22–31% + FEC ≤ 8% + 余量）；mips ≤ 70%。
8. **无回归**：`N=1, M=0` 退化路径的吞吐不低于旧实现的同配置。
9. **可观测**：`/proc/net/tbtun/stats` 能解释每一次 reset（`fec_timeout`、`fec_crc_fail`、`fec_window_exceeded` 至少一项非零）。

---

## 11. 256 五元组：N+M=256 的生产形态

这一节按生产配置写：**256 条独立 TCP，1 share/carrier，N+M=256，M 动态**。所有数字按 256 条算。

### 11.1 结论

| 问题 | 结论 |
|---|---|
| 256 条 carrier 可行吗 | 可行，但资源与 CPU 都是线性成本：约 64–128 MiB 缓冲、22–31% 单核的 TCP/HTTP 处理、768 个 work item |
| RLNC 在 1G 可行吗 | 可行。M=32 时 2.29 XOR/byte（arm64 1–2%）；M=128 时 16 XOR/byte（arm64 5–8%） |
| mips 可行吗 | 可行但紧：M=32 时 15–30%；M=64 时 30–60%；M=128 不可行。mips 的 `M_max` 限制在 48–64，`d=8` |
| 失败模型 | 1 share/carrier 让 carrier 级故障 = 1 个 share erasure；M=32 容忍 32 条 carrier 同时断（概率意义） |
| 解码成本 | 缺 k 个包只解 `k×k`（k ≤ M）的 GF(2) 系统；M=64 时 262k 次 XOR（微秒级） |
| 编码成本 | `(M/N)×d` XOR/byte；M=32/N=224/d=16 → 2.29；M=128/N=128 → 16；1G 下分别是 286 M / 2.0 G XOR/s |

### 11.2 载体层改动清单

**这不是「把常量改到 256」。** 旧 wire 的 16-carrier 限制分布在整条链上：`TBT_MAX_CARRIERS_LIMIT=16`、`path_ack_mask` 是 `u16`、FBK body 最多 16 项、WINDOW body 16 项、`announced_mask`/`path_gen[16]`/`path_end[16]`/FIN/RST mask 都是 16 槽。改常量会同时改帧头布局、FBK/WINDOW 编码、HELLO 协商与所有 host 测试。**FEC 用独立的新 wire format（§2.2，conv 0x1001），旧格式保留给旧 ARQ。**

| 层 | 旧 ARQ（保持原样） | FEC（新格式） |
|---|---|---|
| 帧头 | 32 字节，`path_ack_mask` u16 | 40 字节，`share_index` u16、`epoch` u16、`n/m` u8（§2.2） |
| 载体上限 | `TBT_MAX_CARRIERS_LIMIT=16` | FEC 自己的 `TBT_FEC_MAX_CARRIERS=256` |
| 反馈 | FBK 16 项 + SACK | `FEC_FBK` 48 字节，含 256 位 bitmap（§2.6） |
| 窗口 | WINDOW body 138 字节（16 项） | `FEC_CFG` 32 字节 + 接收端本地状态 |
| 数据面 | per-frame seq/retain/retx/dup | RLNC packet + 窗口方程 |
| 复用 | — | `portblock`（4096）、procfs 目录、carrier 生命周期、HTTP 容器、sock 交付 |

| 位置 | 现状 | FEC 需要 |
|---|---|---|
| `portblock` | 默认 0–0 / 100 个端口 | **4096**（256 carriers × 16 次重建余量） |
| procfs `peers` | 16 行/tunnel | 256 行/tunnel；输出缓冲按 `max_carriers` 分配 |
| carrier 数组 | `struct tbt_carrier *car[16]` | FEC 侧 `car[256]`（2 KiB 指针） |
| poll | 每 carrier 50 ms | **session 级 50 ms，一次遍历 256 条** |
| 内存预算 | 无全局上限 | `fec_buffer_budget=128 MiB`（§11.3） |

隐性成本：

1. **768 个 work item**（`w_rx`/`w_tx`/`w_drain` × 256）：每个约 64 B，合计 48 KiB；workqueue 的并发度与唤醒风暴要实测。
2. **256 个 HTTP 容器**：请求/响应头约 200 KiB；每 carrier 一个 `tbt_http_rx`，状态约 256 KiB。
3. **256 个 BBR 实例**：状态在 `tcp_sock` 内，per-ACK 的带宽滤波是 CPU 成本。

### 11.3 每 carrier 资源与总预算

`1G / 256 = 3.91 Mbit/s` per carrier；RTT 218 ms → **per-carrier BDP = 106 KiB**，**aggregate BDP = 27 MiB**。

**窗口与缓冲的推导**：

```
aggregate BDP = 256 × 106 KiB = 27 MiB
W_send (字节) = k × aggregate BDP，k = 2..3 -> 54..81 MiB
rx_reorder_bytes = 64..128 MiB（覆盖 W_send + 一个 RTT 的乱序）
per-carrier buffer = 512 KiB（实际值，含 snd+rcv）
256 × 512 KiB = 128 MiB（总预算上沿）
```

`SO_SNDBUF`/`SO_RCVBUF` 设置值被内核翻倍，所以每 carrier 实际占用是 `4 × 设置值`（snd + rcv 各 2×）：

| 档 | 设置值 | 实际 snd+rcv/carrier | 256 carriers 合计 | 相对 BDP | 评价 |
|---|---:|---:|---:|---:|---|
| 最小 | 32 KiB | 128 KiB | **32 MiB** | 1.21× | 勉强；丢包时恢复慢 |
| **推荐** | **64 KiB** | **256 KiB** | **64 MiB** | **2.42×** | 覆盖 BDP + 一个 RTT 突发 |
| 宽松 | 128 KiB | 512 KiB | **128 MiB** | 4.83× | 高抖动链路 |
| 过度 | 256 KiB | 1 MiB | **256 MiB** | 9.7× | 不推荐；排队延迟吃掉收益 |

固定开销：`tcp_sock`+`sock` 约 3.3 KiB × 256 = 845 KiB；socket+file 128 KiB；HTTP 256 KiB；work item 48 KiB；peers 行 51 KiB。合计约 **1.3 MiB**。

总内存 = 1.3 MiB + 32/64/128 MiB。**64 MiB 档是甜点**（用户给的 64–128 MB 区间下沿）。

全局上限：`max_tunnels × 256 × 256 KiB`。`max_tunnels=2` 就是 128 MiB。模块参数必须给 `max_carriers_total`，创建时拒绝超限。

### 11.4 256 条 TCP 的 CPU 账

1G、MSS 1460 → 85,616 pps 总，每 carrier 334 pps。

| 项 | 估算 | 单核占比 |
|---|---:|---:|
| TCP 收发（含 ACK、窗口、重传） | 2.0–3.0 µs/pkt × 85.6k | **17–26%** |
| session 级 50 ms poll（合并后） | 20 wakeups/s × 256 条 × 1 µs | **0.5%** |
| 未合并的每 carrier poll | 5,120 wakeups/s × 10 µs | 5.1%（必须避免） |
| HTTP chunk 解析 | 85.6k × 0.5 µs | **4.3%** |
| BBR per-ACK | 已含在 TCP 收发 | — |
| **合计（不含 FEC）** | | **22–31%** |

**必须做的两件事**：

1. **poll 合并**：session 级 50 ms 一次，遍历 256 条；把 5120 wakeups/s 降到 20/s。
2. **pump 拆分**：`sess->pump_lock` 在 256 carriers 下是串行点；改成每 carrier 自己的 `w_tx`，共享队列用无锁取任务。

**IRQ/softirq**：85.6k pps 的收发如果集中在一个 CPU，单核会被打满。需要 RPS/RFS 把 256 条流分散到至少 4 个核；否则 TCP 的 17–26% 会变成 70–100%。

### 11.5 FEC：滑窗 RLNC 的成本

编码成本 = `(M/N) × d` XOR/byte。`K=N`，`d=16`。

| M | N | XOR/byte | 1G 绝对量 | SIMD 32B 指令/s | x86/arm64 | mips 标量 |
|---:|---:|---:|---:|---:|---|---|
| 16 | 240 | 1.07 | 133 M | 4.2 M | <1% | 5–10% |
| 32 | 224 | 2.29 | 286 M | 8.9 M | 1–2% | 15–30% |
| 64 | 192 | 5.33 | 667 M | 20.8 M | 2–3% | 30–60% |
| 96 | 160 | 9.60 | 1.20 G | 37.5 M | 3–5% | 60–100% |
| 128 | 128 | 16.0 | 2.00 G | 62.5 M | 5–8% | **超预算** |

解码：缺 `k` 个包，`k <= M`。GF(2) 消元 `O(k³)`；`k=64` → 262k 次 XOR，约 0.1 ms。应用阶段 `k × d × S`；`k=64, d=16, S=8 KiB` → 8 MiB，@10 GB/s ≈ 0.8 ms。

**1G 数据路径的两次拷贝**：`tcp_read_sock` 的 `skb_copy_bits` 进 packet 缓冲，`tcp_sendmsg_locked` 从解码缓冲写出。125 MB/s × 2 = 250 MB/s 的内存流量，占 DDR 带宽的 1–2%，可忽略；但 cache 压力会体现在 mips 上，需要实测。


### 11.6 失败模型：1 share/carrier 的红利

| 模型 | carrier 故障的含义 | 码的要求 |
|---|---|---|
| **1 share/carrier** | 一条 carrier 断 = **恰好 1 个 share erasure** | RLNC 直接适用；M 容忍 M 条 carrier 同时断（概率意义） |
| k shares/carrier | 一条 carrier 断 = k 个 share erasure | 需要 `M >= k × 目标故障数` |

**1 share/carrier 让 carrier 级失败与 share 级失败同构**。这是这个部署形态最干净的地方，也是 256 条 TCP 的唯一理由。

三种非理想情况：

1. **carrier 慢而不是断**：share 迟到。窗口内可等，窗口外 reset。
2. **NAT/运营商按五元组限速**：256 条各自 3.91 Mbit/s，正好是设计目标。
3. **同时断 > M 条**：不可恢复，有界 reset。

### 11.7 推荐参数

**生产默认**：

```
n=16 m=4 carriers=20 packet=8192 k=16 w=32 d=16 fec=rlnc
fec_m_min=4 fec_m_max=16 fec_margin=0.09 fec_epoch_ms=1000
fec_hysteresis=8
```

启动 `M=16`，控制器 2–3 个 epoch 内收敛。若链路质量未知，从 `M=32` 起步。mips 的 `M_max=48`、`d=8`。

**mips 降档**：`d=8` 把 M=64 的成本从 5.33 降到 2.67 XOR/byte（5–10% 单核）；恢复率与 d=16 差异 < 5%。

### 11.8 原型与验证顺序

| 阶段 | 内容 | 验收 |
|---|---|---|
| H0 | host 纯函数：RLNC 编解码、PRNG、`(M=16..128, N=256-M)` 的矩阵自检 | 全组合恢复；p 曲线符合 §3.5 |
| H1 | host 动态码率模拟：p → M、epoch 切换、恢复率 | 控制器在 3 个 epoch 内收敛；切换期恢复率下降 < 5% |
| H2 | host 资源模型：256 carriers × 三档缓冲、CPU 账 | 与 §11.3/§11.4 一致 |
| H3 | QEMU 小规模：`n=8, m=2, carriers=10` | 字节精确；256 条不在 slirp 上跑 |
| H4 | QEMU 中规模：`n=32, m=8, carriers=40` | 字节精确；CPU/内存符合模型 |
| H5 | 真实 WAN：`n=224, m=32, carriers=256` | 1G 线速下的单流吞吐、`wire/app ≈ 1.14`、CPU 实测、256 条稳定性 |

**不要假设 slirp 能跑 256 条**：slirp 的 `snd_wscale=0` 把每条载体压到 2.4 Mbit/s，256 条的总上限是 614 Mbit/s，但用户态转发是单线程，256 条会先打爆 QEMU 进程。H5 必须用真实 WAN 或两台物理机。

### 11.9 开放问题（256 五元组专属）

1. **pump 的串行点**：256 carriers 下 `sess->pump_lock` 是瓶颈；改成每 carrier 独立 `w_tx`。
2. **索引边界**：`share_index` 用 `u8` 时能表示 0..255，正好覆盖 256 个 share。但校验必须写成 `(u16)idx < (u16)(n+m)`，不能写 `idx < n+m`（`n+m=256` 会被截断成 0）。这是一个实现陷阱，host 测试要覆盖 `n+m=256` 的边界。
3. **NEON/AVX 的内核上下文成本**：若用 SIMD 做 XOR，`kernel_neon_begin/end` 约 1–2 µs；每 8 KiB 包调用一次，1G 下是 8–16% 额外开销。XOR 可以先不用 SIMD，靠编译器自动向量化。
4. **256 条流的慢启动**：同时 connect 会产生 ACK 风暴；需要错开 10–50 ms。
5. **端口耗尽**：1024 个端口在多次重建后是否够？TIME_WAIT 回收策略？
6. **全局上限**：`max_tunnels × 256` 的 socket 总数需要模块参数与 procfs 暴露。
7. **epoch 切换的 parity 重复**：切换瞬间两个 epoch 的 parity 都发，wire 短暂上升；是否需要抑制旧 epoch 的 parity？

---


## 12. MIPS 加速与可行性（基于 192.168.123.254 实测）

数据来源：`review/mips-target.md`（只读探测，2026-11-06）。**不再假设 MT7621；以下是实测。**

### 12.1 目标机实测

| 项 | 实测值 |
|---|---|
| SoC | MediaTek MT7621 SoC（`system type`） |
| 核心 | MIPS 1004Kc V2.15；4 VPE（core 0/0/1/1）= 2 物理核 × 2 线程 |
| 主频 | BogoMIPS 579.58 → 约 880 MHz（推断；无 cpufreq/nvram clkfreq） |
| ISA | `mips16 dsp mt`；**有 DSP ASE，无 MSA**；无 FPU |
| 内存 | 440 MiB；DRAM 实测 271 MiB/s（`dd` 1M×512）；L2 1384 MiB/s |
| 内核 | 3.4.113 SMP，MIPS32_R2 32BIT，无 preempt、无 modversions |
| 编译器 | 设备上无；交叉工具链在构建机（`mipsel-linux-uclibc-gcc -march=mips32r2 -mtune=1004kc`） |
| 硬件加速 | `/proc/crypto` 只有 `*-generic`；无硬件 crypto；HNAT/PPE 只对转发流生效 |
| 部署模式 | AP/桥接（`br0={ra0,rai0,eth2,eth3}`）；客户端流量 L2 交换，不进 IP 栈 |

### 12.2 可行性判定：这台设备不是 1G 平台

**结论：1G / 256 carriers / RLNC 在 MT7621 上不可行。1G 目标只能放在 ARM64（NEON/多核）。**

**先分清两种「不够」**：

| 资源 | 实测能力 | 1G/256 的需求 | 判定 |
|---|---|---|---|
| ALU（XOR） | 双核 880 MHz ≈ 1.7 G 字/s | 286 MB/s ÷ 4 B = **71 M 字/s** | **4%，不紧张** |
| 内存（累加器布局） | DRAM 271 MiB/s | FEC 143 MB/s + 两次 skb 拷贝 250 MB/s = **393 MB/s** | **1.45×，超预算** |
| 内存（逐 parity 循环） | 同上 | generation 313 KB > L2 64 KB，32 倍重读 ≈ **4 GB/s** | **不可行** |
| TCP 终结 | 约 300–500 Mbps | 1 Gbps | **先撞墙** |
| 内存容量 | 440 MiB | 128 MiB carrier buffer | 29% |
| 协议 | `TBT_MAX_CARRIERS_LIMIT=16` + `BUILD_BUG_ON` | 256 | 必须换 wire format（已做） |
| 内核 | 3.4.113，无 preempt | tbtun 需要 6.1+ | **另一个大工程：实质 backport** |
| 部署 | AP/桥接 | tbtun 需要 IP 终结 | 必须先改路由模式 |

**排序很重要**：1 Gbps 下先撞墙的是 **TCP 终结（300–500 Mbps 上限）与两次 skb 拷贝**，不是 XOR。稀疏 RLNC 的真正风险是「工作集被推出 L2」，不是「算不动」。

**唯一可行的编码布局**：源包外循环、parity 内循环（累加器布局）。累加器约 45 KB，常驻 L2；访存 125 × (1 + 32/224) ≈ 143 MB/s。逐 parity 循环（parity 外、源包内）会让每个 parity 重读整代 313 KB，放大 32 倍到 4 GB/s，直接爆掉。

**这台设备的能力量级**：

- **CPU 端点（非 FEC）约 300–500 Mbps**。
- **RLNC W≤8 约 30–90 Mbps**。
- **1G 口只是 L2 交换的 1G**：桥接模式下客户端流量走硬件交换，不进 CPU。

**不要把 1G/256 的估算套到这台设备上。** §8、§11 的 1G 预算是 ARM64/x86 的目标。

### 12.2.1 如果一定要在这台设备上跑

两个独立的大工程，按顺序：

1. **内核 backport**：3.4.113 → 6.1+ 的实质 backport（`tcp_read_sock`、`tcp_sendmsg_locked`、`sk_prot` 覆盖、procfs 接口、workqueue 语义）。这不是改几个头文件；是整条 socket 路径的移植。
2. **低目标 RLNC**：`W≤8`、`M≤4`、`S=1–2 KiB`、30–90 Mbps；即使这样，编码仍占 30–90 MiB/s 的 30–100%。

在这两个工程完成前，**MIPS 端点不作为 FEC 的目标平台**。

### 12.3 如果仍要在 MIPS 上跑：优化清单

1. **XOR-only**：RLNC 已经是纯 XOR，无 GF 查表。
2. **32-bit word XOR、对齐、unroll 4–8**：1004Kc 是双发射，unroll 能填满流水线。
3. **per-VPE 并行**：2 物理核 × 2 线程；把 `M` 行分到两个 VPE。
4. **减少拷贝**：数据路径的两次 memcpy 占 250 MiB/s（1G 时）；MIPS 上必须零拷贝或复用缓冲。
5. **DSP ASE**：1004Kc 有 DSP ASE，但 packed XOR 不是它的强项；不要指望它加速 XOR。
6. **cache**：`S=8 KiB` 的 packet 放不进 L1（1004Kc 的 L1 通常 32 KiB），按包遍历、每包 unroll。
7. **GSO/TSO/checksum**：降 per-packet CPU；但这台机的驱动无 ethtool，offload 状态未知。

### 12.4 结论

- **1G 的 FEC 编码必须放在 x86/ARM64 侧**；MIPS 端点只做低速率（30–90 Mbps），且需要 3.4.113 → 6.1+ 的实质 backport。
- **MT7621 无 MSA**，`xor.v` 不可用；DSP ASE 不解决 XOR 吞吐。
- **这台机是桥接模式**，tbtun 需要路由模式才能生效；桥接下的 1G 是硬件交换，与 FEC 无关。
- **未实测**：GF(2) RLNC 在 1004Kc 上的实际吞吐；需要交叉编译后推到 /tmp 跑。当前数字由 AES/内存带宽夹逼，不是直接测量。

## 13. 参考实现、测试向量与移植边界

这一节回答三个问题：**算法从哪来**、**怎么证明对**、**哪些代码能进内核**。原则是先站在成熟实现上，不自己发明矩阵或解码器。

### 13.1 唯一的参考：Kodo 的 sliding-window RLNC

Kodo（Steinwurf）是网络编码的参考实现，包含 full RLNC、sparse RLNC、sliding-window RLNC。

**借鉴的部分**：

| 项 | Kodo 的做法 | 本设计 |
|---|---|---|
| 窗口结构 | generation 是窗口的锚点，相邻 generation 重叠 | `win_start(g) = g*K+K-W`，重叠 `W-K` |
| 稀疏度 | sparse RLNC 用固定平均度，不是二项分布 | 固定 `d`，`d=16` |
| 系数生成 | 确定性 PRNG，收发共享 seed | `splitmix64(nonce, g, r)` |
| 解码 | 增量高斯消元、rank 维护、early stop | 同结构 |
| 系统化 | 数据包直通，parity 只用于恢复 | 同 |

**不照搬的部分**：

- Kodo 是 C++ 模板库，依赖 STL 与异常；内核态只能移植算法，不能移植代码。
- Kodo 的 coefficient buffer 按 generation 分块；本设计是「每 parity 一行稀疏索引」。测试向量要按「同一组系数下的编码结果」对齐，不是按内存布局对齐。
- 本设计的 PRNG 是自己写的 `splitmix64`，不是 Kodo 的；跨架构一致性与内核可控性优先。

### 13.2 license：没有可直接用的 permissive 实现

调研结论（`review/fec-open-source.md`）：

| 库 | license | 与 GPLv2 内核模块 | 结论 |
|---|---|---|---|
| **Kodo**（Steinwurf） | **Steinwurf Research License** | **不可复制、不可链接** | 只读论文与 API 形状，不复制代码 |
| **badrlnc / tetrys** | **Apache-2.0** | **不兼容**（专利条款） | 只做 benchmark |
| 其他 RLNC C 实现 | 无 permissive 的 | — | — |

**结论：只能 clean-room。** 没有 BSD/MIT 的 sliding-window RLNC C 实现可用；Kodo 的 license 明确限制复制与链接，Apache-2.0 与 GPLv2 不兼容。算法与数学不受版权保护，但代码结构、注释、测试向量受。

### 13.2.1 没有 golden vectors：自己冻结

调研没有找到 Kodo 的可运行测试向量（license 也不允许直接取用）。所以：

1. **冻结自己的向量**：固定 `(W, d, seed)` 三元组，生成 parity 与恢复结果的 SHA-256，写进 `test_rlnc.c` 的常量表。
2. **至少三组**：`W=16, d=16`；`W=64, d=16`；`W=256, d=16`。
3. **覆盖两种路径**：编码输出（parity 字节）与解码恢复（丢 k 个包后的字节）。
4. **跨架构一致**：向量只依赖 `splitmix64` 与 GF(2) 运算，不依赖字节序或编译器。

这样即使将来换 PRNG 或优化内层循环，向量能立刻发现回归。

### 13.3 内核移植边界

| 约束 | 具体要求 |
|---|---|
| 语言 | C99/C11，无 C++、无 STL、无异常、无 RTTI |
| 浮点 | 禁止（内核态默认不保存 FPU 状态） |
| 内存 | 热路径无 `kmalloc`；预分配或 `GFP_ATOMIC` 池 |
| 依赖 | 只用内核导出符号；无外部库链接 |
| SIMD | 若用，必须走 `kernel_fpu_begin/end`（x86）或 `kernel_neon_begin/end`（arm64） |
| 栈 | 单帧 < 1 KiB（`-Wframe-larger-than=1024`）；大缓冲用 `kmalloc` |
| 并发 | 无锁热路径或每 CPU 缓冲；不用 `mutex` 在 softirq |
| 代码量 | `tbt_rlnc.c` < 1000 行；纯函数与内核胶水分离 |
| 可测试 | host 测试用同一份头文件 |

**移植粒度**：

- `tbt_rlnc.c`：PRNG、系数生成、编码、GF(2) 消元，约 800 行。
- 无 GF(2^8) 表、无 Cauchy 矩阵、无 SIMD 依赖。

### 13.4 测试向量交叉验证

| 项 | 参考 | 向量形式 | 验收 |
|---|---|---|---|
| RLNC 编码 | Kodo（若 license 允许跑） | 固定系数下的 parity | 逐字节相等 |
| PRNG | 无第三方 | 固定 `(nonce,g,r,W,d)` → 固定位置序列 | 两次运行一致；跨架构一致 |
| 解码 | 无第三方 | 固定方程 + 固定丢失集合 | 恢复结果逐字节相等 |
| 替代方案 | 两套独立实现（C 与 Python） | 同一组系数 | 逐字节相等 |

**最低要求**：若拿不到 Kodo 的可运行测试向量，用「C 实现与 Python 实现对同一组系数产出相同结果」作为替代；`review/fec-prototype` 已用 Python 验证过 PRNG 的确定性、无重复与均匀性（§附录 D.1）。




## 14. 开放问题

1. **K 与 W 的最终取值。** 本文档推荐 K=8/W=16（M=1）与 K=16/W=32（M=2）。若实测恢复延迟仍偏高，降到 K=4/W=8；若 wire 压力大，升到 K=16/W=32。P3 用 K=4 起步（最容易调试），P5 再定。
2. **恢复延迟是否真的比块式小。** 这是本设计最核心的断言，必须由 P0 的尾延迟模拟证明，而不是靠推理。若模拟显示在真实速率分布下流式不占优，应重新评估。
3. **NACK 是否进 v1。** 本文档列为 P4；若 P3 的 QEMU 结果显示「parity 已覆盖绝大多数丢失」，可以推迟到 P6。
4. **跨窗口方程组合。** 当前设计只在同一窗口内组合方程。若允许跨窗口组合（例如两个窗口各缺 1 个包，用两个 parity 交叉解），恢复能力更强，但复杂度上升。v1 不做。
5. **`W` 与内存的乘积。** 窗口内的组数是 `ceil(W_send/K) + ceil(W/K)`。`W_send=512, K=16, W=32` → 34 组；每组 `rhs` 是 `M × S`（M=2、S=8K → 16 KiB），加 `missing_seq` 表 32 × 8 B；合计约 553 KiB。可接受，但要在 `fec_rx_mem_max` 里计入。
6. **CRC 是否默认开启。** 默认开启（每包一次）；若实测 CPU 占比超过 1%，改为 debug 模式。
7. **多流共享载体。** 当前设计是每 tunnel 一组载体、每 stream 独立 packet 空间。若多条 stream 共享同一组载体，调度需要跨流公平；v1 先假设单流或少量流。
8. **与 `w-window` 的合并顺序。** 载体层修复（buffer/parser/stall）先 cherry-pick；窗口公式以函数复用；per-frame ARQ 整段删除。合并时以本文档 §9.4 的接口约定为准。
9. **块式 fallback 的触发条件。** 当前是手动配置；是否需要在「所有 parity 载体同时断」时自动切块式？自动切换会让故障归因变难，v1 不做。
10. **256 carriers 的专属问题**（pump 串行点、索引边界、NEON 上下文成本、慢启动、端口耗尽、全局上限）见 §11.9，不在这里重复。

---

## 附录 A：配置语法（提案）

```
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 \
  n=4 m=1 carriers=5 packet=8192 k=8 w=16 fec=rlnc fec_timeout_ms=500 fec_reset_ms=2000 \
  fec_nack=1 fec_window_pkts=0 fec_rx_mem_max=33554432 \
  profile=raw keepalive=2000 drip_ms=1000 \
  carrier_stall_ms=0 portblock=30000-30099 nonce=...
```

新增关键字：`carriers=`、`packet=`、`k=`、`w=`、`d=`、`fec=rlnc`、`fec_timeout_ms=`、`fec_reset_ms=`、`fec_nack=0|1`、`fec_window_pkts=`、`fec_rx_mem_max=`。

`n`/`m` 是 FEC 的 share 数；`carriers` 是实际 TCP 连接数，默认 `n+m`（1 share/carrier）。映射规则：share `s` 走 carrier `s % carriers`。约束：`carriers <= 256`、`carriers <= n+m`。目标部署是 `n=96, m=32, carriers=128`；资源账见 §11。
`fec=block` 只用于对照/fallback（§4.5）；默认 `auto`，按 §3.11 的规则选择。
128 五元组部署：

```
tunnel 2 role=near listen=0.0.0.0:9001 peer=45.11.1.62:9000 \
  n=96 m=32 carriers=128 packet=8192 k=96 w=128 d=16 fec=rlnc \
  fec_timeout_ms=500 fec_reset_ms=2000 fec_nack=1 \
  fec_window_pkts=0 fec_rx_mem_max=67108864 \
  profile=raw keepalive=2000 drip_ms=1000 \
  carrier_stall_ms=0 portblock=30000-31023 nonce=...
```

注意 `portblock` 要覆盖 128 个端口（示例给 1024 个）。

删除关键字：`dup=`、`max_inflate=`、`hole_budget_ms=`、`tx_retain_bytes=`、`rx_reorder_bytes=`。

## 附录 B：统计项（提案）

```
fec_pkts_tx fec_pkts_rx fec_pkts_recovered fec_pkts_direct
fec_parity_tx fec_parity_rx fec_parity_late
fec_pkt_dup fec_pkt_late fec_pkt_offpath fec_pkt_out_of_window
fec_crc_fail fec_recover_fail fec_timeout fec_window_exceeded
fec_nack_tx fec_nack_rx fec_nack_repaired fec_reset
fec_window_pkts fec_inflight_pkts fec_credit_pkts
fec_rx_mem_kb fec_tx_mem_kb fec_group_count
fec_latency_p50_us fec_latency_p99_us
```

## 附录 C：与旧实现的字段对照

| 旧 | 新 | 说明 |
|---|---|---|
| `TBT_F_DATA` (0x10) | `DATA` (0x30) | 语义从「一段应用字节 + 全局 seq」变为「固定大小 packet」 |
| `TBT_F_FBK` (0x20) | `ACK` (0x32) | 从 per-path 反馈变为 per-stream 前沿 + credit |
| `TBT_F_SACK` (0x25) | 删除 | ACK 已包含交付进度 |
| `TBT_F_PAD` (0x23) | 保留 | 载体保活 |
| `TBT_F_HELLO` (0x22) | 保留，body +6 B | 协商 `n/m/S/K/W` |
| `TBT_CONV 0x0005` | `TBT_CONV 0x0006` | 不兼容升级；旧 peer 在 `hdr_check` 处被拒 |
| `seq` (u64, 字节偏移) | `seq` (u64, packet 序号) | 单位从字节变为 packet |
| `tx_ack` (u64, 字节) | `next_deliver` (u64, packet) | 单位从字节变为 packet |
| `hole_budget_ms` | `fec_timeout_ms` / `fec_reset_ms` | 从逐帧空洞变为 packet 级超时 |
| `dup` / `max_inflate` | 删除 | parity 取代复制 |
| `retain` / `replay` | 发送窗口 | 只为 NACK 保留，不再为 ARQ |

## 附录 D：RLNC 系数生成与自检

### D.1 系数生成

```c
static inline u64 splitmix64(u64 *state)
{
    u64 z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* 从 [0, n) 取一个整数；n <= W <= 1024，偏差可忽略 */
static inline u32 bounded(u64 *state, u32 n)
{
    u64 r = splitmix64(state);
    return (u32)((r & 0xffffffffULL) % n);
}

/* 第 g 组第 r 行：选 d 个不重复的位置 */
void rlnc_coef(u64 nonce, u32 g, u32 r, u32 w, u32 d, u16 out[])
{
    u64 state = nonce ^ ((u64)g * 0x9e3779b97f4a7c15ULL) ^ r;
    u32 pool[1024];
    u32 j;

    for (j = 0; j < w; j++)
        pool[j] = j;
    for (j = 0; j < d; j++) {
        u32 pos = j + bounded(&state, w - j);
        u32 tmp = pool[j];
        pool[j] = pool[pos];
        pool[pos] = tmp;
        out[j] = (u16)pool[j];
    }
}
```

### D.2 自检

1. 同一 `(nonce, g, r, w, d)` 两次调用必须产生相同序列。
2. 序列内无重复；所有 `out[j] < w`。
3. 跨架构一致：只用 `u64` 算术，无字节序依赖。
4. `g` 与 `r` 的不同组合必须产生不同的行（碰撞率与随机预期一致）。
5. `W >= K`；`d <= W`。

### D.3 已验证的事实

用 Python 实现同一算法，对 10,000 个 seed 检查第一个位置：分布均匀（min 55 / max 100 / mean 63.67，期望 63.5）；96 个 `(g,r)` 组合产生 96 个不同的行；同一 seed 两次输出完全一致。这些事实在 `review/fec-prototype` 的 `test_rlnc.c` 里也要成立。
