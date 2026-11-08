# 0.3.0 SC-LDPC 落地设计：下行 256 单组，上行 grouped

状态：只读设计，未改代码。基线 HEAD `a6eb228`。SC-LDPC 原型与实测在
`/home/kunweiz/tbtun-worktrees/{fec,sclc-gf16,sclc-simd,sclc-algo,sclc-encode}`。
本文以 T0 `review/goal-v6-interfaces.md` 的 SC-LDPC 段、`review/fec-v6-wire.md`、
`review/fec-v6/G2.md`、`review/fec-v6/grouped.txt` 和上述原型证据为依据。
标记：`[事实]` 代码/文档；`[实测]` 原型原始数据；`[设计]` 本方案选择；`[待测]` 必须上机。

---

## 0. 结论先行

1. **0.3.0 的码型是 SC-IRA（SC-LDPC 的 BEC 特化），不是 GF(2) band RLNC。**
   构造：payload 度 5 的环形窗口、parity 度 2 的累加器、check 度 6–7；系统化；
   BEC peeling 解码；rate 0.50 精确，>0.50 缩 `n_base`，<0.50 追加 extra check
   （`review/sc-ldpc-sim.md:24-48`，`sc_ldpc.h:32-38`）。当前 GF(2) RLNC/band
   只作为过渡/回退模式，不再继续调 n/m 类表。
2. **下行 256 单组**：`carriers=256`、`n+m=256`、`groups=1`；SC-LDPC codeword
   `block=256`、`L=8`（p=0.40 用 `block=512,L=16` 的实测点），rate 按实测
   margin 取 **0.50/0.40/0.30** 对应 p=30/40/50%（不是 T0 的容量点
   0.70/0.60/0.50；容量点实测 recovery 0，`review/sc-ldpc-sim.md:184-210`）。
3. **上行 grouped**：`carriers=256`、`n+m=8`、`groups=32`（或 8×32，见 §3.3）；
   每组独立 SC-IRA codec；rate p30→0.50、p40→**0.45（GF16）**、p50→0.30。
   **GF(16) 4-bit 优先**：HND0 b128/L16/r0.45 2t **10.27 MB/s**、1t 6.43，
   recovery 0.999944；binary 同 bit block r0.45 只有 0.998084。GF16 的 0.45
   码率在 p=0.40 同时赢恢复率和带宽（`sc-ldpc-gf16.md:4.1`，
   `sc-ldpc-gf16-hnd0-thr.csv`）。
4. **CPU 已经不是否决项，前提是 bit-sliced + bit64 编码。**
   HND0 实测（`review/sc-ldpc-bitslice.md:6`，**i.i.d. symmetric** 口径）：
   - p=0.30/r=0.50：1t **8.47**、2t **11.20 MB/s**，recovery 0.999821（1M，下界 0.999783）；
   - p=0.40/r=0.40 block512/L16：recovery 0.999168（1M，下界 0.999090），2t 约 5.5–6.5（待复核）；
   - p=0.50/r=0.30：1t 4.06、2t **5.80 MB/s**，recovery 0.999862；
   - bit64 编码 HND0 e2e **105–217 MB/s**，wire 需求 8.4–14.1（`review/sc-ldpc-bit64.md:6`）。
   app 目标 4.215 MB/s（33.72 Mbit/s）在三个 p 点都有可行配置。
   share 级相关擦除在本机 158–260 MB/s（`sc-ldpc-bitslice.md:4`），HND0 未测；
   真实 app 是 share 级，HND0 相关擦除应不低于 i.i.d.，但需复核。
5. **1 Gbit/s / 125 MB/s 不是 HND0 的目标。** 本机 Ryzen 相关擦除 bit-sliced
   已达 158–260 MB/s（`sc-ldpc-bitslice.md:4`），但 HND0 2 vCPU 只有
   8.47–11.20；1G 需要更快宿主或硬件，0.3.0 明确按 app 33.72M 验收。
6. **自适应关系**：SC-LDPC 的自适应是**码率**自适应，不是 n/m 类表。
   `rate_target = clamp(1 - p_est - margin, 0.30, 0.70)`，margin 0.15–0.20；
   在 epoch 边界切换码率，旧 FEC block 解完再换，新 block 用新图。0.3.0
   可先固定每方向码率 + 手工配置，0.3.1 打开自动码率；epoch/CFG/FBK 框架
   与 0.2.x 的自适应设计共用。

---

## 1. 现有调研与原型事实

### 1.1 T0 的 SC-LDPC 约定

- `mode=sc-ldpc`；`n/m` 是每块 data/parity share 数；`w=0→256`，显式只接受
  256/512；**L 固定 8**；新增 L/block 键必须先改 ABI（`goal-v6-interfaces.md:605`）。
- FEC_CFG：`epoch/n/m/w/k/d/seed`；SC-LDPC 的 `d` 可为 0；CFG 在 epoch 边界
  发 3 次，接收端按 epoch 建新窗口，旧 epoch 继续解码到滑出/超时
  （`goal-v6-interfaces.md:250-262`）。
- G4 门：`(block,L,rate,p)=(256,8,0.70,0.30),(256,8,0.60,0.40),(512,16,0.50,0.50)`，
  recovery ≥99.9%，codec ≥125 MB/s（`goal-v6-interfaces.md:938-944`）。
- 现状：HND0 标量 decode 0.32–1.99 MB/s，encode 2.65–9.54；恢复率可 0.9995–1.0，
  CPU 不过；G4 不修订，T6 阻塞（`goal-v6-interfaces.md:947`）。
- 新指令：上行允许 grouped SC-LDPC 4-bit 码，原型 `w-sclc-gf16`
  （`goal-v6-interfaces.md:971`）。

### 1.2 原型构造（SC-IRA）

- 生产构造 `SC_TERM_ACC`：payload block i 连 check `(base..base+4) mod n_base`，
  `base=floor(i*n_base/L)`，度 5；parity block a 连 check a 和 a+1，度 2
  （最后一个度 1）。`n_base=L` → rate 0.50；rate 0.60/0.70 缩 `n_base`；
  rate<0.50 追加 `extra_dc=5` 的 extra check（`review/sc-ldpc-sim.md:32-48`）。
- 编码：一次正序扫描，parity 子矩阵下双对角，永不奇异（`sc-ldpc-sim.md:47`）。
- 解码：BEC peeling/迭代 BP；check 的 `xorv` 与 `ucnt` 合并成 u16；rank 不足
  不写任何字节（`sc-ldpc-sim.md:51-55`，`sc_ldpc.h:88-105`）。
- 小规模 bit-exact：block 16/32、L=2/4、rate 0.50、p=0.01/0.10，
  `payload_check` 4096/2048 码字 bit_errors=0（`sc-ldpc-sim.md:3`，表 3）。
- 码率-恢复：容量点 rate=1-p 的 recovery 0；要 0.999 需要余量 ≥0.15–0.20
  （`sc-ldpc-sim.md:3.2`）。实测可行点：
  - p=0.30/r=0.50：0.9990–1.0000；
  - p=0.40/r=0.40：0.9985–1.0000；r=0.45：0.9975–1.0000；
  - p=0.50/r=0.30：0.9960–1.0000；r=0.25：0.9995–1.0000。
- 延迟/内存：codeword 延迟 = L*block 的传输时间；codec 状态 59 KB–1.38 MB
  （block 256/512、L 8–32），不含数据窗口（`sc-ldpc-sim.md:4.2`）。

### 1.3 HND0 标量结果（为什么不能直接用）

- HND0（QEMU 2 vCPU）单核 decode **0.32–1.99 MB/s**，2 线程 **0.55–1.96**；
  app 需要 **4.215 MB/s**，最好只有 47%（`review/sc-ldpc-hnd0.md:0`）。
- encode 2.2–10.3 MB/s，wire 需求 8.4–14.1（rate 0.50/0.40/0.30），多数点不够。
- p=0.50 的 rate 0.25 恢复 0.9995–1.0，但 decode 0.43–0.56；没有同时满足
  0.999 与 4.2 的标量点（`sc-ldpc-hnd0.md:0`）。
- 本机 Ryzen 是 HND0 的 6–12 倍；本机数字不能当验收（`sc-ldpc-hnd0.md:5`）。

### 1.4 优化后的 HND0 结果（0.3.0 的基础）

- **bit-sliced 相关擦除**（64 码字共享同一 share 擦除 mask）：本机
  158–260 MB/s，比标量 12.5x（`sc-ldpc-bitslice.md:4`）。
- HND0 同批 A/B（`sc-ldpc-bitslice.md:6`）：

| p | rate | block/L | 1t MB/s | 2t MB/s | recovery |
|---|---|---|---:|---:|---|
| 0.30 | 0.50 | 256/16 | 8.47 | 11.20 | 0.999821（1M，下界 0.999783） |
| 0.40 | 0.40 | 512/16 | — | ~5.5–6.5 待复核 | 0.999168（1M，下界 0.999090） |
| 0.50 | 0.30 | 256/16 | 4.06 | 5.80 | 0.999862（160k，下界 0.999763） |
| 0.50 | 0.25 | 256/16 | 4.01 | 4.00 | 0.999950（但 2t < 4.215，不用） |

- **bit64 编码器**：64 码字位平面，一次前向扫描；HND0 e2e **105–217 MB/s**，
  wire 需求 8.4–14.1，编码不再是瓶颈（`sc-ldpc-bit64.md:6`）。
- **GF(16) 4-bit**：同 bit block（b128 GF16 = b512 binary），p=0.40：
  r=0.40 recovery **0.999956** vs binary 0.999168；r=0.45 **0.999944** vs
  binary 0.998084；本地 decode 29.4/34.4 MB/s，约 4x binary scalar
  （`sc-ldpc-gf16.md:4.1`）。HND0 b128/L16：r=0.45 **1t 6.43 / 2t 10.27 MB/s**，
  r=0.40 1t 4.93 / 2t 7.22；encode 24–39 MB/s，全部高于 wire 8.4–14.1
  （`sc-ldpc-gf16-hnd0-thr.csv`）。ARM64 b128 r=0.45 2t **15.2**、1t 7.7
  （`sc-ldpc-gf16-arm64.md`）。
- **相关擦除 vs i.i.d.**：share 级擦除下 bit-sliced 12.5x；i.i.d. 只有
  2.4–5.5x。tbtun 的真实擦除是 share 级，所以必须按 share 映射
  （`sc-ldpc-bitslice.md:0`）。

### 1.5 开源调研

- 没有成熟 C/C++ SC-LDPC codec：aff3ct/IT++ 是 block LDPC；npminh12 是
  506 行 BEC 仿真（MIT，构造参考）；libcoding 源码缺失；SCA-LDPC 是 Rust
  无 license（`review/fec-open-source-sclc.md:0`）。
- RaptorQ 有 nanorq（C11/MIT，RFC 6330），但是 block 码、K×T 矩阵/求逆，
  不是流式；可作为 block fallback/对照，不是 SC-LDPC（`fec-open-source-sclc.md:3`）。
- 结论：SC-LDPC 只能基于仓库原型自研/移植；原型是 clean-room，只取
  npminh12 的构造规则和 BEC 语义（`sc_ldpc.h:1-14`）。

---

## 2. 码型与参数

### 2.1 下行 256 单组

| 参数 | 值 | 依据 |
|---|---|---|
| 码型 | SC-IRA（`SC_TERM_ACC`），系统化 | `sc-ldpc-sim.md:1.2` |
| carriers | 256 | T0 |
| n+m（每 FEC block share 数） | 256 | 单组 |
| groups | 1 | `groups=carriers/(n+m)` |
| codeword block `w` | 256（p=0.40 的实测点用 512） | T0 `w=256/512` |
| coupling L | 8（p=0.40 用 16） | T0 L 固定 8；实测 block512/L16 |
| rate | p30→0.50，p40→0.40，p50→0.30 | 实测 margin，不是容量点 |
| 每 block data/parity share | R=0.50:128/128；R=0.40:≈102/154；R=0.30:≈77/179 | `n≈R*256`，整数化后由 codec 校验 |
| 解码 | BEC peeling + bit-sliced 64 码字 + 相关擦除 | `sc-ldpc-bitslice.md` |
| 编码 | bit64 前向扫描 | `sc-ldpc-bit64.md` |

**码率与 share 数的关系**：SC-IRA 的码率由 codeword 图（`n_base`/extra check）
决定，不是 `n/(n+m)` 的 share 比。`n/m` 是每个 FEC block 的 data/parity
share 数；codec 必须保证 `n` 个 data share 的 payload 位和 `m` 个 parity
share 的 parity 位正好装下。整数化后 `n/m` 与目标 rate 可能差 <1%，
FEC_CFG 同时携带 `n/m` 和码率参数（`k` 或 `d`），接收端按实际图解码。
**这是 0.3.0 接线前必须用 host 测试钉死的第一个契约。**

### 2.2 上行 grouped

| 参数 | 值 | 说明 |
|---|---|---|
| carriers | 256 | 不变 |
| n+m | 8（32 groups）或 32（8 groups） | 见 §3.3 |
| groups | 32 或 8 | 每组独立 codec/window |
| codeword block | 256 | T0 |
| L | 8 | T0 |
| rate | p30→0.50，p40→0.45（GF16），p50→0.30 | GF16 的 0.45 是净码率优势 |
| 码型 | **GF(16) 4-bit 优先**；binary bit-sliced 备选 | HND0 GF16 r0.45 2t 10.27 |
| 每 block data/parity share | R=0.50:4/4；R=0.40:3/5；R=0.30:2/6 | 8 shares 的整数化 |

**上行为什么 grouped**：256 carriers 的 FEC 状态/解码工作可以按组并行；
每组独立窗口/队列，单组故障不影响其他组；码率/码型可按上行链路单独选。
代价是每组 share 少（8），`n/m` 整数化粒度粗（37.5%/62.5%），且小块的
有限长度损失更大。若 host 仿真显示 block 8 的恢复率不达标，改用 8 groups
× 32 shares（每块 32 shares，`n/m` 更细），保持 256 carriers 不变。

### 2.3 码率表（实测 margin）

| p_est | 下行 rate | 上行 rate | 余量 1-p-rate | 备注 |
|---:|---:|---:|---:|---|
| 0–10% | 0.70–0.80 | 0.70–0.80 | ≥0.15 | 低损，码率越高越好；实测点有限，先 0.70 |
| 10–20% | 0.60–0.70 | 0.60–0.70 | ≥0.15 | |
| 20–30% | 0.50 | 0.50 | 0.20 | HND0 8.47/11.20 MB/s |
| 30–40% | 0.40 | 0.45（GF16） | 0.20/0.15 | HND0 GF16 r0.45 2t 10.27，rec 0.999944；下行 block512/L16 0.999168 |
| 40–50% | 0.30 | 0.30 | 0.20 | HND0 4.06/5.80 MB/s |
| >50% | 0.25 或停 | 0.25 或停 | ≤0.25 | 0.25 恢复 0.99995 但 2t 4.00 < 4.215；不宣称可行 |

`[设计]` 0.3.0 先固定 p30/40/50 三个码率点（下行 0.50/0.40/0.30；
上行 0.50/0.45/0.30），p 的连续插值在 0.3.1 打开；不要用 T0 的容量点
0.70/0.60/0.50 做验收，
它们在有限长度下 recovery 0。

---

## 3. 数据面架构

### 3.1 share 与 codeword 的映射

SC-LDPC codeword 是**比特级**的：一个 codeword 有 `block*L` 个变量
（block=256、L=8 → 2048 bit），系统化后前 `n_data` 位是 payload，其余是
parity。tbtun 的一条 carrier 承载一个 **share**（S=8192 B = 65536 bit）。
一个 share 可以装 `65536 / (block*L)` 个 codeword：

- block=256、L=8 → 2048 bit/codeword → 32 codeword/share（若 payload 全占满）；
- block=256、L=16 → 4096 bit/codeword → 16 codeword/share；
- block=512、L=16 → 8192 bit/codeword → 8 codeword/share；
- GF(16) b128/L16 → payload 512 B/codeword → **16 codeword/share**；
- GF(16) b256/L16 → payload 1024 B/codeword → **8 codeword/share**。

**注意**：`sc-ldpc-bitslice.md:6` 的 HND0 8.47/11.20 是 **i.i.d. symmetric**
口径；`--corr` 的 158–260 MB/s 是本机、且 batch 内 64 码字共享同一 mask。
真实 share 映射下一个 share 只有 8–16 个 codeword，64-codeword batch 跨
4–8 个 share，mask 不完全相同，`--corr` 的 12.5x 不能直接套 HND0。
**验收以 i.i.d. HND0 数字为下界**；相关擦除是额外收益，需按实际 share 映射
重测。

`[待测]` bit64 文档 §5.2 明确说“share ↔ codeword 区间、parity 行如何从
位平面切出”在原型文档里没有定义，接线前必须钉死一种读法。本文建议：

1. **一个 share = 整数个 codeword**，codeword 在 share 内按 LSB-first 连续排列；
2. data share 装 payload 位，parity share 装 parity 位；同一 codeword 的
   payload/parity 不混在同一个 share；
3. bit-sliced 批次 64 codeword；若一个 share 只有 32 codeword，则一批跨
   2 个 share，两个 share 的擦除 mask 可能不同，`--corr` 的 12.5x 会下降；
   要么把批次降到 32，要么选 `block*L=1024`（64 codeword/share）使一批正好
   一个 share。**推荐后者**：block=128/L=8 或 block=64/L=16，保持 64
   codeword/share，相关擦除的 HND0 吞吐 8.47–11.20 可直接复用；代价是
   codeword 更短，需要重新测 recovery。若坚持 T0 block=256/L=8，则先测
   32-codeword batch 的 HND0 吞吐和 recovery。

### 3.2 TX 流水线

1. 应用字节进 capture ring（现有 `tbt_fec_tx_submit`，softirq 安全）。
2. packetizer 把字节切成 share（8192 B）；一个 FEC block 有 `n+m` 个 share
   （下行 256，上行 8/32）。
3. 对每个 data share，按 §3.1 切成 codeword；用 bit64 编码器批量编码
   （64 codeword/批），得到 parity 位。
4. 把 parity 位按 codeword 顺序装进 parity share；data/parity share 按
   `carrier = group*(n+m)+share` 发到对应 carrier。
5. 一个 FEC block 的 data+parity share 全部发出后，进入下一个 block。
   没有跨 FEC block 的 look-ahead；L 是 codeword 内部的耦合长度，不是
   FEC block 的窗口。**这是与“流式 SC-LDPC 窗口”不同的关键简化**：
   每个 codeword 独立编码/解码，FEC block 只是 share 的集合。

### 3.3 RX 流水线

1. 每个 carrier 收到 share，按 header 的 `block_id/share_index` 放入 FEC block。
2. 一个 FEC block 的 share 齐/缺确定后，对每个 data share 的 codeword
   做 bit-sliced peeling；缺失 share 是擦除，parity share 提供方程。
3. 解码成功的 data share 按 seq 交付；缺失且解不出的 share 触发 NACK
   （现有 64 B bitmap，按 group/share 请求）；重放到达后再解码。
4. 一个 FEC block 的 data share 全部交付后，`next_deliver` 前进；ACK 携带
   credit。SC-LDPC 的 FEC block 是 2 MiB（下行）或 64 KiB（上行 8 shares），
   现有 super-block 交付逻辑可以复用。
5. 解码器按 64 codeword 批处理；每批的擦除 mask 来自 share 级 bitmap；
   相关擦除下 peeling 调度只算一次，吞吐 8.47–11.20 MB/s（HND0）。

### 3.4 内存

| 项 | 下行 256 单组 | 上行 32×8 | 上行 8×32 |
|---|---:|---:|---:|
| FEC block 数据 | 2 MiB | 64 KiB/组 ×32 = 2 MiB | 256 KiB/组 ×8 = 2 MiB |
| codec 状态 | 83 KB–231 KB | 每组 ~几十 KB | 每组 ~几十 KB |
| 解码 scratch | 64 codeword × 2048 bit 位平面 ≈ 16 KB/批 | 同 | 同 |
| 现有 RX arena | 16 MiB，可放 8 个 FEC block | 同 | 同 |
| 额外 | NACK/replay 队列 | 同 | 同 |

`[设计]` 不引入 L 个 FEC block 的大窗口；内存与现有 16 MiB arena 同量级。
若采用 block=256/L=8 的 32-codeword 批，scratch 更小。

---

## 4. 与 0.3.0 自适应的关系

### 4.1 自适应的是码率，不是 n/m 类表

SC-LDPC 的码率由 codeword 图决定；`n/m` 是每 block 的 share 数，必须与
码率一致。控制律：

```
p_est = 窗口内擦除 share / 发送 share
rate_target = clamp(1 - p_est - margin, 0.25, 0.80)
margin = 0.15 (p<30%) / 0.20 (p>=30%)
```

- 不用固定类表；`rate_target` 连续，按码率族量化到可用点
  {0.25,0.30,0.35,0.40,0.45,0.50,0.60,0.70}（原型 `rate_pct`）。
- 滞回：升码率需 `p_est < 1 - rate - margin - 0.03` 连续 3 次；
  降码率需 `p_est > 1 - rate - margin + 0.03` 连续 2 次。
- 驻留：码率切换后 ≥3 s 不再切；反向冷却 5 s；≤10 次/60 s。
- 码率切换在 **FEC block 边界**：当前 block 的 data/parity 发完、接收端
  解完/交付完，再换新图；旧 block 不跨 epoch。FEC_CFG 携带
  `epoch/n/m/rate/block/L/seed`；接收端按 epoch 建新 codec，旧 codec 在
  最后一个旧 block 交付后释放。字节不丢：share/seq 连续，block 边界切换。
- `p_est` 用接收端 share 级擦除统计（NACK/缺失/恢复），与 0.2.x 的
  miss 位图设计一致；FBK 上报 `p_ewma_ppm`/`expected/received`。
- 失败回退：新 codec 分配失败或新 block 解码失败率 >1%，回退到旧 epoch，
  保持旧图；不重置信道。

### 4.2 0.3.0 的分期

- **0.3.0**：SC-LDPC 固定码率（下行 0.50@p30、0.40@p40、0.30@p50；
  上行同），手工 `rate=` 配置；epoch/CFG 框架先打通，自动码率可关。
- **0.3.1**：打开 `adaptive=on`，按 §4.1 连续码率；复用 0.2.x 的 p_est/
  FBK/切换状态机，但目标从 n/m 类表换成码率。
- **回退**：`mode=rlnc|grouped` 保留为过渡/低 CPU 回退；不再调它的 n/m 表。

---

## 5. Wire 与 ABI

### 5.1 现有 48 B FEC header

- `+4 n`、`+5 m`：每 block 的 data/parity share 数；SC-LDPC 沿用。
- `+8 block_id`：FEC block 序号；SC-LDPC 的 epoch/窗口按 block 边界。
- `+2 share_index`：0..n+m-1；carrier 映射不变。
- `+24 win_len`：SC-LDPC 置 0（码率由 FEC_CFG 携带；不再用 per-block
  window 长度）。
- `+6 epoch`：仍是 PARITY 的 short-length，不是配置 epoch；配置 epoch 在
  FEC_CFG body。
- `conv`：0.3.0 升 `TBT_FEC_CONV`（SC-LDPC 语义 + CFG 扩长），两端一起升级。

### 5.2 FEC_CFG 扩为 48 B（建议）

```
+0  u32 epoch
+4  u16 n            // data shares per block
+6  u16 m            // parity shares per block
+8  u16 w            // codeword block (256/512)
+10 u16 k            // code rate in 1/1000, or n_data per codeword
+12 u8  d            // 0 for SC-LDPC
+13 u8  flags        // bit0 APPLY, bit1 ABORT
+14 u16 groups       // 256/(n+m)
+16 u64 switch_pkt   // first packet of new epoch
+24 u64 block_base   // first FEC block id of new epoch
+32 u64 seed
+40 u32 crc32c       // +0..+39
+44 u8  codec        // 1=rlnc, 2=grouped, 3=sc-ldpc
+45 u8  symbol       // 0=binary, 1=gf16
+46 u16 L            // 8 (T0 fixed; explicit for clarity)
```

`[设计]` 这样 FEC_CFG 同时表达码型/码率/块/组/L，接收端不依赖本地配置
猜测对端形状。**上下行可以不同**：每端在 session 开始时发自己的 TX CFG，
对端按收到的 CFG 建 RX codec；这是 SC-LDPC 非对称（下行 256 单组、
上行 grouped）的必要条件。现有 RLNC 数据面假设两端 n/m 相同；SC-LDPC
必须改为 per-direction CFG 协商。

### 5.3 FEC_FBK

- `p_ewma_ppm`、`expected_shares`、`received_shares`：share 级擦除率；
- `m_current/m_target`：当前/建议的 parity share 数（或码率档）；
- `flags`：CFG_ACK/CFG_NACK/SWITCH_DONE/ROLLBACK/P_VALID/P_CAPPED；
- `carrier_bitmap`：本窗口有记录的 carrier；
- `epoch`：当前 RX epoch（hi/lo 两段，避免 16 位回绕）。

---

## 6. CPU / 内存预算

### 6.1 HND0（验收机）

| 方向 | 配置 | decode | encode | 结论 |
|---|---|---:|---:|---|
| 下行 p30 | r0.50 block256/L16 | 8.47 (1t) / 11.20 (2t) | bit64 105–217 | 过 4.215 |
| 下行 p40 | r0.40 block512/L16 | ~5.5–6.5 (2t) 待复核 | 同 | 预计过 |
| 下行 p50 | r0.30 block256/L16 | 4.06 (1t) / 5.80 (2t) | 同 | 2t 过，1t 差 0.15 |
| 上行 p40 | GF16 b128/L16/r0.45 | 6.43 (1t) / **10.27 (2t)** | 24–39 | 过 4.215，且码率更高 |
| 上行 p40 | GF16 b128/L16/r0.40 | 4.93 (1t) / 7.22 (2t) | 同 | 过 |
| 上行 p50 | GF16/binary r0.30 | ~4–6 | 同 | 需实测 |

- 1t 不够时用 2t；HND0 只有 2 vCPU，2t 是上限。
- 1 Gbit/s/125 MB/s：本机相关擦除 158–260 MB/s，但 HND0 8.47–11.20；
  1G 不在 HND0 目标内。
- 编码 bit64 已远超 wire 需求；解码是主成本。
- 每 FEC block 的 codec 状态 83–231 KB；数据 2 MiB；scratch 16 KB/批；
  现有 16 MiB arena 可容纳。

### 6.2 为什么标量数字不是否决

标量 HND0 0.32–1.99 MB/s 是 bit-level i.i.d. 口径；tbtun 的真实擦除是
share 级相关擦除，bit-sliced 64 码字共享 mask，HND0 8.47–11.20。
**必须按 share 映射和 bit-sliced 相关擦除验收，不能再用标量 i.i.d. 数字
判死 SC-LDPC。**

---

## 7. 实现路径

### 7.1 阶段

1. **Host codec 定型**：把 `sc_ldpc.c`/`sc_ldpc_bs.c`/`sc_enc64.c` 合成
   一个 host-only codec，固定 share↔codeword 映射、data/parity 打包、
   batch=64、GF16/binary 选择；跑 BEC 曲线和 bit-exact。
2. **Codec ops**：实现 `tbt_fec_ops` 的 SC-LDPC 项（`mode=3`），
   `coeffs` 返回 `-ENOSYS`；`encode/decode` 用 host codec 的接口。
3. **数据面**：在 `tbt_fec_data.c` 增加 SC-LDPC 分支：share 打包/解包、
   FEC block 的 decode 调度、NACK/replay、ACK 前沿；不改变 carrier/queue。
4. **Per-direction CFG**：session 开始时交换 TX CFG；RX codec 按对端 CFG
   建；支持下行 256 单组、上行 grouped。
5. **自适应**：epoch/CFG 切换、码率控制、p_est/FBK；先固定码率，再打开。
6. **验收**：HND0 下行/上行、p30/40/50、byte-exact、CPU/RAM。

### 7.2 代码位置

- `tbtun/tbt_fec.h`：`tbt_fec_ops` 的 SC-LDPC 项；`tbt_fec_params` 的
  `codec=SC_LDPC`；`tbt_fec_cfg` 扩 48 B。
- `tbtun/tbt_fec.c`：CFG/FBK pack/unpack；`params_check` 接受 SC-LDPC；
  `tbt_fec_m_for_p` 不再用于 SC-LDPC。
- `tbtun/tbt_fec_data.c`：`tbt_fec_data_init` 按 `mode=3` 建 SC-LDPC
  codec；`tbt_fec_packetize`/`tbt_fec_blk_decode`/`tbt_fec_deliver_ready`
  加 SC-LDPC 分支；`tbt_fec_rx_record` 的 CFG/FBK；NACK/replay。
- `tbtun/tbt_session.c`：per-direction CFG 协商；adaptive work。
- `tbtun/tbt_parse.c/h`：`mode=sc-ldpc`、`rate=`、`block=`/`w=`、`L=`
  （若扩 ABI）；`carriers/n/m` 校验。
- `tbtun/tbt_procfs.c`：SC-LDPC 统计（epoch/rate/recovery/decode MB/s）。
- `Makefile`：host codec/bench 源文件；内核只加 codec 的纯 C 部分。
- `host/test_fec.c`：SC-LDPC BEC 向量、share 映射、bit-exact、CFG/FBK。

### 7.3 工作量/风险

- 码型构造/解码器：原型已有 C；内核化主要是接口/内存/无浮点/无 libc。
- 数据面：SC-LDPC 的 share↔codeword 映射是新增工作；不是简单替换 codec。
- Per-direction CFG：改变现有“两端同 n/m”的假设；需要协议/测试。
- 自适应：码率切换的 epoch/窗口语义；比 RLNC 的 n/m 切换复杂。
- 风险：GF16 HND0 未测；block=256/L=8 的 32-codeword batch 吞吐未测；
  T0 `w=256/512` 与 uplink grouped block 的 ABI；1G 目标不在 HND0。

---

## 8. 验收

### 8.1 BEC/host 门

- p=30/40/50，rate 0.50/0.40/0.30（不是容量点 0.70/0.60/0.50），
  recovery ≥99.9%（1M 码字或 Wilson 下界 ≥0.999）。
- bit-exact：编码/解码逐字节；share↔codeword 映射的 pack/unpack 往返。
- codec 吞吐：host 相关擦除 ≥125 MB/s（1G 目标）或 HND0 ≥4.215 MB/s
  （app 目标），两者分开记录。
- 内存：codec 状态 + 2 MiB FEC block + scratch；HND0 RSS 记录。

### 8.2 HND0 端到端

- 下行 256 单组：p=30/40/50，rate 0.50/0.40/0.30，1 GiB byte-exact；
  app ≥33.72 Mbit/s（4.215 MB/s）；decode ≥4.215、encode ≥8.4–14.1；
  CPU/RAM/延迟记录。
- 上行 grouped：32×8 或 8×32；GF16/binary 二选一；同 p/rate；byte-exact；
  app 目标同上。
- 自适应：p 30→40→30，epoch 切换，无字节丢失，无 stream reset；
  切换次数/停顿记录。
- 1G/125 MB/s：单独的快宿主验收；HND0 不宣称。

### 8.3 对比

- 基线：0.2.3 固定 n4m4 RLNC/grouped。
- 对比：SC-LDPC 下行/上行、p30/40/50、app/wire/CPU/RAM/recovery。
- 记录：`review/sc-ldpc-0.3.0/`、原始 CSV、dmesg、procfs、`ss -ti`。

---

## 9. 风险清单

| # | 风险 | 证据 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | 容量点 rate=1-p 的 recovery 0 | `sc-ldpc-sim.md:3.2` | 验收失败 | 用 margin 0.15–0.20；rate 0.50/0.40/0.30 |
| R2 | HND0 标量 decode 0.32–1.99 | `sc-ldpc-hnd0.md:0` | 不过 4.215 | bit-sliced 相关擦除 8.47–11.20；2t |
| R3 | i.i.d. bit-sliced 只有 2.4–5.5x | `sc-ldpc-bitslice.md:0` | 吞吐不稳 | 按 share 映射，利用相关擦除 12.5x |
| R4 | share↔codeword 映射未定义 | `sc-ldpc-bit64.md:5.2` | 接线错误/静默错 | host 测试钉死；pack/unpack 往返 |
| R5 | block256/L8 的 32-codeword batch 未测 | 同上 | 相关擦除加速下降 | 先测 32 batch；或改 block128/L8 使 64/share |
| R6 | 上行 grouped block 8 太短 | 原型只测 block 16/32 | 恢复率不达标 | 改 8×32；host 仿真 block 8/32 |
| R7 | GF16 HND0 已测 5 seed，未做最终 5 轮中位 | `sc-ldpc-gf16-hnd0-thr.csv` | 吞吐不确定 | 5 轮中位；与 bit-sliced A/B；ARM64 15.2 可作旁证 |
| R8 | 上下行形状不同，现有数据面假设同 n/m | `tbt_fec_data.c` init | RX codec 错 | per-direction CFG 协商 |
| R9 | T0 `w=256/512` 与 uplink block 冲突 | `goal-v6-interfaces.md:605` | ABI 变更 | 扩 `block=`/`groups=` 键；bump conv |
| R10 | 1G/125 MB/s 在 HND0 不可达 | 实测 8.47–11.20 | 目标错位 | 0.3.0 按 app 33.72M；1G 单独 |
| R11 | 码率切换的旧 block/窗口语义 | 现有 epoch 设计 | 字节丢失 | FEC block 边界切换；旧 block 解完再换 |
| R12 | 没有成熟 C SC-LDPC 库 | `fec-open-source-sclc.md:0` | 实现风险 | 基于仓库原型；clean-room；host 测试 |
| R13 | 内核无浮点/无 libc | 原型 host | 移植失败 | 只用整数/表；无 malloc；预分配 |
| R14 | 内存：2 MiB block + codec + scratch | 现有 16 MiB arena | OOM | 复用 arena；按块分配；记录 RSS |
| R15 | 解码失败时 NACK/replay 语义 | 现有 NACK 64 B | 流停 | 复用现有 bitmap/replay；SC-LDPC 解不出的 share 再 NACK |

---

## 10. 推荐

1. **0.3.0 用 SC-LDPC，不再继续 GF(2) band/n-m 类表。** 下行 256 单组
   SC-IRA，block=256/L=8（p40 用 512/16），rate 0.50/0.40/0.30；
   上行 grouped 32×8 或 8×32，**GF(16) b128/L16 优先**（p40 用 r0.45，
   HND0 2t 10.27 MB/s、rec 0.999944），binary bit-sliced 作为下行/回退。
2. **验收按 HND0 app 33.72 Mbit/s（4.215 MB/s）**，不按 1G/125 MB/s；
   1G 用快宿主单独验收。bit-sliced 相关擦除 + bit64 编码是过线的关键。
3. **先固定码率，后自适应。** 0.3.0 打通 SC-LDPC 数据面/CFG/epoch/
   固定 rate；0.3.1 打开连续码率控制（`rate=1-p-margin`），复用 p_est/FBK。
4. **保留 RLNC/grouped 作为回退模式**，但冻结其 n/m 表；不再投入调参。
5. **接线前先钉死三个契约**：share↔codeword 映射、FEC block 的 n/m 与
   码率一致性、per-direction CFG 协商。这三项任一错误都会静默解错/丢字节。

---

## 附：关键证据索引

- T0 SC-LDPC 约定：`review/goal-v6-interfaces.md:250-262,605,938-947,971`
- 构造/曲线：`/home/kunweiz/tbtun-worktrees/fec/review/sc-ldpc-sim.md`
- HND0 标量：`.../fec/review/sc-ldpc-hnd0.md`
- bit-sliced：`.../sclc-simd/review/sc-ldpc-bitslice.md`
- bit64 编码：`.../sclc-encode/review/sc-ldpc-bit64.md`
- GF16：`.../sclc-gf16/review/sc-ldpc-gf16.md`、`sc-ldpc-gf16-hnd0-5seed.csv`
- 开源调研：`.../fec-survey/review/fec-open-source-sclc.md`
- 原型源码：`.../fec/review/fec-prototype/sc_ldpc/{sc_ldpc.c,sc_ldpc.h}`、
  `.../sclc-gf16/review/fec-prototype/sc_ldpc/sc_ldpc_gf16.{c,h}`
- 现有 wire：`tbtun/tbt_uapi.h:85-201`；数据面 `tbtun/tbt_fec_data.c:4560-4815`
