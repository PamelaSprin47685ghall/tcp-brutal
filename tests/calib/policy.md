# SC-LDPC 连续码率负反馈控制（policy.md 草稿）

- 仓库：`/home/kunweiz/Desktop/vibe/tcp-brutal`，master `66ecd19`
- 范围：`src/kernel/tbt_fec_data.c` 的 RX 解码/放置路径、`src/kernel/tbt_fec_adapt_sclc.c/.h` 的估计器与控制律、`src/kernel/tbt_fec_late.h` 账本、`src/kernel/tbt_fec.h` 的每块 n/m 路径、`tests/unit/test_fec_adapt_sclc.c`
- 状态：只读设计，不改代码。`tests/calib/sclc-down.md`、`sclc-up.md` 的解析结果到达后再补数值；本文先给完整控制结构、解析式、稳定性条件和执行机构结论。
- 方向：**取消码率阶梯**。控制量是每块的校验份数 `m`（等价地 `n = 256 − m`），由接收端测得的 `E_b`/`T_b` 驱动，经 FBK 的 `m_target` 字段送回发送端，发送端把 n/m 放进下一块的报文头。没有档位、没有 `p_up/p_down` 表、没有固定余量。

---

## 0. 结论先行

1. **观测量可行，但当前 `T_b` 有 frontier 偏差。** 解码不是“只在 real+8 时试一次”：`tbt_fec_sclc_decode_wake()` 在每来一份、且 `ndata+nparity ≥ real+extra`（下行 extra=8，上行 extra=2）时唤醒；`tbt_fec_rx_decode_one()` 只在 `b->block_id == rx->next_super` 时真正尝试，失败后 50 ms 起指数退避，新 share 清零退避。所以“解码尝试”发生在到达路径，但被 frontier 和退避推迟。`T_b` 能在**首次唤醒**时精确快照；需要在放置路径加 `wake_ns/wake_ndata/wake_nparity/wake_real`，否则非 frontier 块的 `T_b` 会被排队时间抬高，把信道看得过好。
2. **控制律用随机逼近的分位数，不用 P² 估极尾。** `q_{k+1} = q_k + γ_q(1{E_k > q_k} − ε)` 直接估 `Q_{1−ε}(E)`；P² 的 5 个 marker 对 ε≈1e-3 的尾部是插值外推，误差不可控，只适合估尺度（IQR）和诊断。`m* = round(q + Δ)`，`Δ` 先取标定解析值，再由 `T_b − n` 的盈余慢速校正。
3. **ε 由边际权衡推导，不拍 1e-3。** 一条校验份的边际码率代价是 `1/(n+m)`；一次首次失败的边际代价是 `f_E(m−Δ)·(1+W/T_b)`。令二者相等得 `f_E = (1−ε)(1+εw)/((n+m)(1+w))`；对尾尺度 σ 近似 `ε ≈ σ/((n+m)(1+w))`，并夹在 `[1/(n+m), 5%]`。下行 n+m=256 时 ε 约 0.2–0.8%，上行每 group n+m=32 时 ε 约 1–3%——块越小，可达到的失败率越粗，这是块长决定的下限，不是调参。
4. **执行机构推荐 A（每块 n/m + LRU 码图），但有门槛。** 控制器在接收端，接收端可以在发 FBK 的同时预建目标 m 的码图；报文头已有 h.n/h.m，n+m 保持 256（上行每 group 32）时缓冲区大小不变，只有 DATA/parity 分界变化。码图内存下行 0.17–0.34 MB/张、上行 8 组约 1–4 MB/张；LRU 4–8 张是 2–16 MB，far 955 MiB、实测 used 385 MB，余量足够。门槛是码图构建 p99 ≤2 ms、缓存命中 ≥95%；达不到就退回 B（CFG + 死区 + 速率限制），但 B 的 drain/握手会把回路延迟推到 2–4 个 block，并重置估计器窗口。
5. **突发不需要单独的 `burst` 标志做稳态判断。** `E_b` 的分位数本身包含信道的突发分布；需要的是**突发前馈** `B_k = max(0, EWMA_fast(E) − q)`，在突发刚起、分位数还没抬起来时先加几份校验，衰减后由 q 接管。整条 carrier 失效：alive=0 冻结；部分失效由 E 自然覆盖，另可用 `(1−A)·n` 做前馈。
6. **删除阶梯和它的一整套防抖。** ladder、`MARGIN_PPM`、`HYST_PPM`、`UP_RUN/DOWN_RUN`、probe、`refused_mask`、`rate_mask`、`p_up/p_down`、`boundary/target` 全部退场；保留 pause 标志、alive/abandoned 冻结、r_fail 作为安全备份。`rate_pct` 只保留为兼容标签，由 n/m 派生，不参与决策。

---

## 1. 观测量：E_b、T_b 与解码时机

### 1.1 代码事实（master 66ecd19）

- `tbt_fec_sclc_decode_wake(real, m, groups, ndata, nparity)`（`tbt_fec.h:1790-1802`）：
  - `ndata >= real` → 可解（无需 parity）；
  - 否则要求 `ndata + nparity >= real + extra`，下行 `extra=8`，上行 `extra=2`。
- 放置路径（`tbt_fec_data.c:7620-7655`）在每一份 fresh share 后调用这个判定；满足时 `b->decode_next_ns=0`、`b->decode_tries=0`，并 arm RX work。
- 解码路径（`tbt_fec_data.c:5077-5106`）只在 `b->used && !b->ready && !b->decoding && now >= b->decode_next_ns && b->block_id == rx->next_super` 时尝试。失败后退避 `50 ms << tries`，最大 400 ms；新 share 清零退避。
- 结论：**不是每到一份都试，也不是只在 real+8 试一次**；是“跨过唤醒线就 arm，frontier 轮到才试，失败退避，新证据立即重试”。`T_b` 在唤醒线处可测；在真正尝试处测会被 frontier/退避污染。

### 1.2 定义

对每个 group block b（下行 1 个/块，上行 8 个/块）：

```
real_b   = 该块 DATA 位置数（尾块可小于 n）
d_b      = 首次唤醒时已到的 DATA 份数
p_b      = 首次唤醒时已到的 parity 份数
E_b      = real_b − d_b          // 首次唤醒时缺的 DATA 份数（需要 parity 补的份数）
T_b      = d_b + p_b             // 首次唤醒时已收到的总份数
S_b      = T_b − real_b          // 首次唤醒时的 parity 盈余（唤醒线要求 ≥ extra）
T_ok     = 首次成功时的总份数（仅 first_ok=1 时有效）
first_ok = 首次尝试是否成功
```

- `E_b` 是“解码时缺的 DATA 份数”，不是总份数；只有 DATA 缺失才需要 parity 补。把 parity 缺失也算进 E 会把“parity 晚到但不需要”误判成需要更多 parity。
- `S_b` 是唤醒时的实际盈余；控制律用它校正 Δ。全块用 `T_b − n`，尾块用 `T_b − real_b`。
- 首次尝试失败时，`E_b`/`T_b` 仍然有效（它们是唤醒瞬间的样本）；`T_ok` 更大，只用于诊断。失败样本进入首次失败率/安全备份，不用于 Δ 的盈余校正（否则会把“失败后多等的份数”误当成“m 过多”）。

### 1.3 需要的改动

1. `struct tbt_fec_blk` 增加：
   ```c
   u64 wake_ns;      /* 首次跨过唤醒线的时间，0=未跨 */
   u16 wake_ndata;   /* 唤醒时的 d_b */
   u16 wake_nparity; /* 唤醒时的 p_b */
   u16 wake_real;    /* 唤醒时的 real_b */
   u8  wake_first_ok;/* 首次尝试结果，0=未试/失败，1=成功 */
   u8  wake_attempts;/* 首次成功前的尝试次数 */
   ```
2. 放置路径在 `tbt_fec_sclc_decode_wake()` 首次为真且 `wake_ns==0` 时，取一次 `ktime_get_ns()` 并快照 `d_b/p_b/real_b`。每块最多一次时钟调用，成本可忽略；不要每份 share 取时间。
3. `tbt_fec_rx_decode_one()` 在首次尝试后写 `wake_first_ok`/`wake_attempts`；成功时把 `E_b/T_b/S_b/T_ok` 送进新的分位数估计器。
4. 尾块：`real_b < n` 时，用 `real_b` 归一化或直接排除（`real_b < n/2` 的块不进入分位数），避免把短块当成低丢失。
5. 多 flow/多 group：每个 group block 一个样本；上行一个 super-block 出 8 个样本，下行 1 个。控制律按方向聚合，`n` 用该方向的每组 n。

### 1.4 为什么不用 p 作为控制量

`p = miss/sent` 是 4 s 窗口的聚合比例，已经把“晚到但最终到了”的 share 修掉；它回答“这一段时间丢了多少”，不回答“这个块在解码时刻还缺几份”。连续控制要的是后者：`E_b` 直接是“需要多少 parity 才能在这个时刻解出来”。`p` 仍可保留为诊断和 pause 判据，但不再进入控制律。

---

## 2. 控制律

### 2.1 目标

每个方向、每个 group：

```
m* = clamp( round( Q_{1−ε}(E) + Δ(n) ), m_min, m_max )
n  = shares − m*          // 下行 shares=256；上行每 group shares=32
```

- `Q_{1−ε}(E)`：在线分位数，表示“ε 比例的块在唤醒时缺的 DATA 份数超过它”。
- `Δ(n)`：解析余量（标定给出）+ 在线盈余校正，覆盖“分位数估的是 E，而 codec 需要 m 份 parity 才能解”的差、以及估计误差。
- `m_min/m_max`：codec 可构建的范围（下行约 16..192，上行约 4..28），由 `tbt_fec_sclc_shape()`/codec 构建校验给出；不是档位。
- `shares` 恒定是 A 路线的前提；若 n+m 变化，块缓冲区、late 位图、估计器 base 都要跟着变，复杂度不值得。

### 2.2 分位数估计：Robbins–Monro

```
q_{k+1} = q_k + γ_q · ( 1{E_k > q_k} − ε )
```

- `q` 用 1/256 share 的定点（u32），`γ_q` 也按定点；`ε` 用 ppm。
- 期望更新为 0 当 `P(E>q)=ε`，所以 q 收敛到 `Q_{1−ε}(E)`。线性化：`dq/dk = γ_q (F(q) − (1−ε))`，在分位点附近斜率是密度 `f`，时间常数 `1/(γ_q f)` 个 block。
- 取 `γ_q = 1/16 share/block`（即每 16 个块移动约 1 份），低通 `q_act += (q − q_act)/4`。对下行 2 block/s，时间常数约 8 s；对上行 1 block/s，约 16 s。这个速度足够跟 RTT 尺度的变化，不会对单块噪声反应。
- 初值：`q_0 = max(extra, m_current − Δ0)`；冷启动前 16 个块只更新 q、不执行，避免第一块把 m 拉飞。
- 为什么不用 P²：P² 的 marker 在 0/25/50/75/100% 分位，ε=1e-3 的尾部只能靠 Q3→max 的抛物线外推；样本一抖，外推就失真。P² 可以用来估 IQR/σ（见 §2.3），但极尾用 RM。
- 为什么不用固定步长的“加一/减一”：`γ_q` 是概率步长，不是每块 ±1；否则在 ε=1e-3 时几乎永不更新，在 ε=5% 时每块跳 1 份，噪声大。RM 的步长与目标 ε 匹配。

### 2.3 ε 的边际推导

设：

- `R(m) = n/(n+m)`：payload 码率；
- `w = W/T_b`：一次失败多等的时间 / 一个 block 的时间；
- `G = R(1−ε)/(1+εw)`：长跑有效吞吐；
- `ε(m) = P(E > m−Δ)`，`dε/dm = −f_E(m−Δ)`。

对 m 求导并令 `dG/dm=0`：

```
f_E(m−Δ) · (1 + w) = (1−ε)(1+εw) / (n+m)
```

这就是“多一份 parity 的码率代价 `1/(n+m)`”与“少一次失败的等待代价 `f_E(1+w)`”的边际相等。对尾尺度 σ，取 `f_E ≈ ε/σ`：

```
ε ≈ σ(1−ε)(1+εw) / ((n+m)(1+w))
```

小 ε 时：

```
ε ≈ σ / ((n+m)(1+w))
```

- `σ` 由 P² 的 IQR 估：`σ ≈ (Q_{0.75}−Q_{0.25})/1.349`；没有 P² 时用 `max(1, EWMA(|E−q|))`。σ 至少 1 份。
- `w` 由实测：`W` 取失败首次尝试到成功的等待（或 NACK RTT），`T_b` 取相邻 block 的到达间隔；没有数据时 `w=1`，并在头文件标注假设。
- 夹紧：`ε ∈ [1/(n+m), 0.05]`。`1/(n+m)` 是一份 share 的分辨率：下行 256 份 → 0.39%，上行 32 份 → 3.1%。想要更低的 ε，只能加块长或加 share 数，不能靠控制律。
- 这条公式解释了为什么下行能到 1e-3 量级、上行只能到 1–3%：不是算法差，是每 group 只有 32 份，一份就是 3.1% 的粒度。
- 不做“人为压低 p”：ε 是失败率目标，不是对 p 的折减；q 和 Δ 都直接作用于 m。

### 2.4 Δ(n)：解析值 + 在线盈余校正

**解析值。** 标定曲线给出每个 n 下 `F(n,m)=ε` 的 `m_codec(n,ε)`；则

```
Δ0(n) = m_codec(n, ε) − Q_{1−ε}(E)
```

`Δ0` 包含 codec 的有限长缺口和唤醒线的 `extra`。若标定只给固定 m 的 `P(fail)–p`，先在 `(n,m)` 网格上拟合 `F`，再反演。`Δ0` 夹在 `[extra, m_max − q]`。

**在线校正。** 首次成功（`first_ok=1`）时用唤醒盈余 `S_b = T_b − real_b`。目标盈余 `S* = extra + 1`（唤醒线之上留一份余量）：

```
Δ_{k+1} = Δ_k + γ_Δ · (S* − S_b)
```

- `γ_Δ = 1/64 share/block`，比 q 慢 4 倍；只对 `first_ok=1` 的块更新，失败块走安全备份。
- 夹紧 `Δ ∈ [Δ0/2, Δ0+8]`；anti-windup：当 `m*` 被 `m_max` 或 `m_min` 饱和时停止积分。
- 若 `S_b` 长期等于 `extra`（唤醒线就是约束），说明 m 不够，Δ 增加；若 `S_b` 明显大于 `S*`，说明 m 过多，Δ 减小。这个回路把“解析值”拉到“这台机器、这条链路的实际 codec 行为”。

### 2.5 增益、收敛与反馈延迟

回路有三个时间尺度：

| 回路 | 增益 | 时间常数 | 作用 |
|---|---|---|---|
| 分位数 q | `γ_q=1/16` share/block | 8–16 block | 跟踪 E 分布 |
| 执行 m | q 的低通 `α=1/4` | 4 block | 平滑单块噪声 |
| 盈余 Δ | `γ_Δ=1/64` share/block | 64 block | 校正解析余量 |

延迟 `d` 由 FBK 间隔（≥100 ms）、RTT/2、执行（A 下一块；B 的 CFG drain）组成，按 block 数计：

```
d = ceil( (T_fbk + RTT/2 + T_apply) / T_b )
```

`T_b` 是实测的相邻 block 间隔，不是固定值。低速链路（下行 2 block/s、RTT 50–200 ms）`d≈1–2`；高速链路（T_b=10 ms、RTT=100 ms）`d≈10`，此时必须按下面的公式降增益，否则同样的 γ 在高速链路上会振荡。

一阶回路加纯延迟的相位裕度约 `90° − γ d`（弧度）。要求 45° 裕度：

```
γ_q · d ≤ π/4   →   γ_q ≤ π/(4d)
```

- d=2 → γ≤0.39；d=3 → γ≤0.26；d=4 → γ≤0.20；d=10 → γ≤0.079。运行时取 `γ_q = min(1/16, π/(8d))`：d≤8 时 1/16 有 2× 裕度，d>8 时自动降到 `π/(8d)`。`γ_q` 的单位是 share/block；wall-clock 时间常数是 `1/(γ_q f) · T_b`，高速链路 block 多、每块步长小，但墙钟时间不一定慢。
- 如果实测 `d` 抖动很大，用 `d` 的 95 分位而不是瞬时值；否则一次 RTT 尖峰会把增益压得过低。`T_b`/`W` 各用 EWMA，窗口至少 32 个 block。
- Δ 回路的增益是 q 的 1/4，且时间常数 4 倍，两个回路不会互相激发。
- 执行死区（B 路线）会引入 ±deadband/2 的极限环；低通和 Δ 的慢积分把极限环压到 1–2 份。A 路线没有死区，只有整数 m 的 1 份量化。
- 收敛性：RM 在恒定 γ 下收敛到分位点附近，稳态方差 `≈ γ ε(1−ε)/f²`；`γ=1/16`、`f≈0.1`、`ε=0.01` 时 σ_q≈0.8 份。对 m 的整数化来说足够。
- 低样本：前 16 个 block 只用解析 `Δ0` 和当前 m，q 不执行；`q_valid=false` 时 `m` 保持当前值，不升不降。`pause_*` 时冻结 q/Δ/m，恢复后重新积累。

### 2.6 启动与安全备份

- 启动：`m = m_init`（配置的初始值，默认 128/128 或 8/24），`q = max(extra, m−Δ0)`，`Δ=Δ0`。
- 首次失败备份：连续 `K=8` 个块 `first_ok=0`，或最近 32 个块的首次失败率 > `2ε`，把 `m` 加 `max(2, n/16)` 份，冻结 1 个 RTT；然后恢复正常。这不是档位，是安全覆盖。
- `r_fail`（TX 本地 NACK 窗口）仍可作为独立备份：若 `r_fail > 2%` 连续 2 个 tick，临时把 m 加 2 份并禁降 1 s。它不修改 q/Δ，只加一个偏移，偏移衰减后回到控制律。
- 若 codec 构建失败/内存不足：保持上一张可用码图，FBK 回 `m_current`，不把失败形状放上线；这取代旧的 `refused_mask`。

---

## 3. 执行机构：A 与 B

### 3.1 路线 A：每块 n/m + LRU 码图

**数据面改动。**

- 报文头 `h.n/h.m` 已经是 u8，且 `tbt_fec_hdr_check()` 已校验 `n,m≥1, n+m≤256`。当前 RX 在 `tbt_fec_data.c:6462/6570/6591/7283` 把 `h.n/m` 与 `rx->n/m` 比较并拒绝不匹配；A 改为接受 `h.n+h.m == rx->shares` 的任意拆分。
- `struct tbt_fec_blk` 存 `b->n/b->m`；`b->parity = b->buf + b->n*s`；`tbt_fec_blk_slice()`、`dpresent/ppresent` 的 DATA/parity 分界、`real_len`、NACK 扫描都改用 `b->n/m`。缓冲区大小 `(n+m)*s` 不变，因为 n+m 恒定。
- TX packetizer 用当前块的 `n/m` 生成 share_index；replay ring 的每条记录存 `n/m`（或按块 id 查形状表），NACK 重放用记录里的 n/m。
- RX 的 `tbt_fec_late` 账本：`struct tbt_fec_late_rec` 增加 `u16 n,m`；`tbt_fec_late_credit/note_miss/take/finalize` 用记录里的 n/m，而不是 `ctx->n/m`。位图索引 `group*(n+m)+share` 在 n+m 恒定时不变；若 n+m 变化，256 位图仍够，但 group 映射要按记录重算——第一版只允许 n+m 恒定。
- 估计器：`est_block` 的 `base=group*shares` 用恒定的 n+m；`real` 由块头给出；`est_init` 只在 session reset/INIT 调用，**不再在每次换码率时清零**。这是 A 相对 B 的关键收益：4 s 窗口/分位数样本不会被 CFG 打断。
- codec：TX/RX 各一个 LRU，key 为 `(n,m,block,L,seed)`；命中直接 decode/encode，未命中在工作队列构建（不在 softirq）。RX 在发 FBK `m_target` 后就可以预建目标图；TX 在收到 `m_target` 后预建。下一块到达时大概率已命中。

**内存与构建成本。**

- 下行 block256/L16：codec 状态约 167 KB（r50）到 339 KB（r30），6 个点 0.17–0.34 MB；LRU 8 张约 1.4–2.7 MB。
- 上行 8×32、block128/L16：每 group 的图更小，但 8 组合计约 1–4 MB/张（**估算，需用 `tbt_sclc_uplink_mem()` 实测**）；LRU 4 张约 4–16 MB。far 955 MiB、实测 used 385 MB，余量足够；仍建议上行 LRU 4 张、下行 8 张。
- 构建是 O(n_chk+edges)（约 10^4 条边）+ 若干 `zalloc`；估计 <1 ms，但必须在目标机上用 `clock_gettime` 实测 p50/p99。门槛：p99 ≤2 ms、缓存命中 ≥95%。达不到就退回 B。
- `tbt_fec_sclc.c` 的构建路径有 `zmalloc/zalloc`，可能睡眠；不能在 `rx->lock`/softirq 里构建。工作队列预建是必须的。

**为什么 A 适合本设计。** 控制器在 RX，RX 知道下一块的目标 m，可以在 FBK 发出后立即预建；TX 收到 FBK 后也能预建。回路延迟从“CFG drain + ACK”降到“FBK + 下一块”，且估计器窗口不被清零。报文头已经支持 n/m，不需要新线格式。

### 3.2 路线 B：CFG 握手 + 死区 + 速率限制

- 控制律仍输出 `m_target`，但只有 `|m_target − m_current| ≥ deadband`（建议 4 份，或 `max(2, n/64)`）且距上次 CFG ≥1 s 时才发 CFG；一次只有一个 CFG 在飞。
- CFG 携带 n/m/epoch，走现有两阶段提交；drain 到 P0、ACK、SWITCH_DONE。延迟约 `FBK 100 ms + RTT + drain 0.5–2 s`，即 2–4 个 block。
- 优点：复用现有 codec/ledger/estimator/切换测试；不需要 per-block n/m；风险最小。
- 缺点：drain 期间旧码率继续发，回路延迟大；每次 CFG 都 `est_init` 清窗口，连续控制的分位数被反复冷启动；多 flow 时 CFG 是 session 级，不能按 flow 调。
- 若选 B，控制律的 γ_q 要降到 1/32，deadband 4 份，且 `m_target` 在 CFG 在飞时冻结（anti-windup）。

### 3.3 推荐

**推荐 A，B 作为门槛未达时的回退。** 理由：

1. 控制器在 RX，A 允许 RX 预建目标图，构建不在关键路径；B 的 CFG 必须等 drain，回路延迟和估计器清零都与连续控制冲突。
2. 报文头已有 h.n/h.m，n+m 恒定时缓冲区大小不变，改动集中在“每块 n/m 的解析/账本/估计器 base”，是机械的、可测试的。
3. 内存 2–16 MB 对 955 MiB/385 MB used 是可接受的；构建 p99 的门槛可以用一个 host microbench 先量。
4. B 的 CFG 是 session 级，无法支持多 flow 的独立码率；A 的每块 n/m 天然按 flow/block。

**A 的落地顺序：** 先做 host microbench 量 `tbt_fec_sclc_new()` 的 p50/p99 和 `tbt_sclc_uplink_mem()`；再改 block/late/estimator 的 per-block n/m；最后接控制律。若 p99 >2 ms 或上行 LRU 内存 >32 MB，转 B。

---

## 4. 突发与 carrier 失效

### 4.1 突发

`E_b` 的分位数本身覆盖稳态的突发分布：如果信道长期是“每 10 个块来一次 20 份缺失”，`Q_{0.99}` 就会抬到 20，m 自然够。**不需要 `burst` 标志做稳态判断。**

但分位数是慢变量，突发刚起时它还没抬起来。需要前馈：

```
B_k = max(0, EWMA_fast(E) − q_k)
m* = clamp(round(q_k + Δ + B_k), m_min, m_max)
```

- `EWMA_fast` 用 α=1/4，时间常数 4 个块；突发 1–2 个块就能看到。
- `B_k` 上限 `n/4`，并在突发结束后按 `B ← B·7/8` 衰减；衰减到 0 后完全由 q 接管。
- 这取代旧的 `BURST_PPM/GAP`；不再比较 p_fast 和 p_ewma，因为 p 不是控制量。

### 4.2 carrier 失效

- 整条 carrier 失效：`alive=0` 或 `pause_carrier` → 冻结 q/Δ/m，保持当前 m；恢复后重新积累。此时任何码率都发不出去，调 m 没有意义。
- 部分 carrier 失效：`A = alive/carriers < 1`。死载波上的 share 不会到达，`E_b` 会上升，分位数/前馈会加 m。为加快响应，可加物理前馈 `B_carrier = ceil((1−A)·n)`（这是“少了一条 carrier 就少 n/carriers 份”的物理估计，不是人为压低 p）；它与 `B_k` 取 max，不叠加。
- `stalled > 10%`：同整条失效，冻结；`P_VALID` 清除，TX 看到后不再应用新 m。
- `abandoned > 1%`：冻结，避免把“块被放弃”误当成“需要更多 parity”。

---

## 5. 删除、保留与新增

### 5.1 删除

| 对象 | 位置 | 原因 |
|---|---|---|
| `tbt_fec_adapt_sclc_ladder`、`rate_at/rate_index/rate_ppm/rate_index_for_shape` | `.c:20-60` | 控制不再用档位；`rate_pct` 只做兼容标签 |
| `tbt_fec_adapt_sclc_init_plan`、`max_side`、`profile_of` 的档位分支 | `.c:66-130` | 由 n/m 直接构建；profile 固定 block/L |
| `tbt_fec_adapt_sclc_boundary`、`target` | `.c:637-707` | 由 q/Δ 取代 |
| `MARGIN_PPM`、`HYST_PPM` | `.h:363-364` | 固定余量/滞回退场 |
| `UP_RUN`、`DOWN_RUN`、`UP/DOWN_DWELL`、`MIN_GAP`、`REVERSE`、`RATE_MAX`、`HOLD`、`OSC` | `.h:337-350` | 连续控制不需要档位防抖；安全备份用失败计数 |
| `PROBE_*`（LOW/HIGH/MAX/STABLE/OBSERVE/BAN） | `.h:352-357` | probe 是档位探索，删除 |
| `refused_mask`、`no_down`、`reject_*`、`rate_mask` | `.h:374-443` | 每块 n/m 没有 session 级拒绝掩码；构建失败回退当前 m |
| `FAIL_DOWN`/`commit_bad`/`rollback` 的档位逻辑 | `.c:1084-1295` | 用 §2.6 的失败偏移/安全备份取代 |
| `burst` 标志、`BURST_PPM/GAP` | `.h:191-192`、`.c:627-631`、`tbt_fec_data.c:2990-2993` | 用 E 的 EWMA 前馈取代 |

### 5.2 保留

- `pause_idle/pause_carrier/pause_abandoned`、`est_carriers`、`est_abandoned`：冻结条件。
- `tbt_fec_late` 账本：改为 per-block n/m。
- `r_fail` 本地 NACK 窗口：安全备份（§2.6），不进入主控制律。
- `tbt_fec_sclc_new`/codec、`tbt_fec_hdr` 的 h.n/h.m、CFG 的 INIT/epoch：A 路线保留 CFG 只做 INIT/会话建立，不做换码率。
- `rate_pct`：由 `n*100/(n+m)` 派生，写进 CFG/FBK 兼容字段；控制律不读。

### 5.3 新增

1. `struct tbt_fec_blk` 的 wake 快照字段（§1.3）。
2. `tbt_fec_adapt_sclc_q` 模块（或同文件内的纯函数）：
   - `q_init(q0, eps)`;
   - `q_update(q, E, eps, gamma)`;
   - `eps_from_marginal(sigma, n, w)`;
   - `delta_update(delta, S, S_star, gamma)`;
   - `m_target(q, delta, burst, m_min, m_max)`。
   全部整数/定点，无浮点，可双编译。
3. FBK 的 `m_target` 语义：从“接收端建议的档位 m”改为“接收端控制律的 m_target”；TX 在 FBK 落地处存 `peer_m_target`，下一块用它（A）或触发 CFG（B）。FBK 64 B 不变。
4. `tbt_fec_late_rec` 增加 `u16 n,m`；`tbt_fec_late_*` 改用记录里的 n/m。
5. `struct tbt_fec_blk` 增加 `b->n/b->m`；`tbt_fec_blk_slice`、`real_len`、NACK 扫描、parity 偏移改用 `b->n/m`。
6. 标定头文件 `src/kernel/tbt_fec_adapt_sclc_cal.h`（新语义）：
   ```c
   #define TBT_FEC_ADAPT_SCLC_CAL_VERSION 2
   #define TBT_FEC_ADAPT_SCLC_CAL_PROVENANCE "..."
   struct tbt_fec_adapt_sclc_cal {
       u16 shares;          /* n+m，下行 256，上行 32 */
       u16 m_min, m_max;
       u16 delta0[/*n 或 m 的桶*/]; /* 解析余量，份 */
       u16 delta_len;
       u32 rho_ppm;         /* 若用簇方差 */
       u32 w_ppm;           /* 标定假设的 W/T_b，运行时可覆盖 */
   };
   static const struct ... tbt_fec_adapt_sclc_cal_down;
   static const struct ... tbt_fec_adapt_sclc_cal_up;
   ```
   `delta0` 由 `m_codec(n,ε) − Q_{1−ε}(E)` 生成；桶按 n 或 m 的等距网格，运行时线性插值。没有 `p_up/p_down`，没有 `rate_pct` 表。
7. 标定工具 `tests/bench/calib_slc_policy`（或沿用 `bench_sclc_recovery` 的输出）：
   - 输入 CSV：`dir,mode,n,m,p,trials,first_fail,block_fail,shares,misses,supers`；
   - 对每个 `(n,m,p)` 估 `F`，在 `(n,m)` 上单调拟合，反演 `m_codec(n,ε)`；
   - 输出 `delta0` 表和 `w` 假设；`--check` 比对已提交头文件。
   - 曲线到达前，头文件用解析占位：`delta0 = extra + 2`，`m_min/m_max` 由 codec 范围给出；**明确标注 provisional**。

---

## 6. 必须新增的单元测试

测试文件 `tests/unit/test_fec_adapt_sclc.c`（`ok/eq_u32`、`case_*`、`main` 注册）。新增：

### 6.1 收敛与阶跃

- `case_q_convergence`：合成 E ~ 已知分布（如负二项/泊松），RM 在 200 个块内收敛到 `Q_{1−ε}` 的 ±1 份；不同 ε（1e-3/1e-2/5e-2）分别验证。
- `case_q_step`：E 的均值从 2 跳到 8，q 在 32 个块内跟上，超调 ≤2 份；m 的低通不产生反向。
- `case_eps_marginal`：给定 σ、n、w，`eps_from_marginal` 与数值求导的 `dG/dm=0` 误差 <5%；ε 随 σ 线性、随 n 反比、随 w 反比。
- `case_delta_online`：构造 `S_b` 恒为 `S*+2`，Δ 在 256 个块内下降；`S_b=S*−2` 时上升；饱和时 anti-windup 不积分。

### 6.2 延迟与稳定性

- `case_delay_stability`：把测量延迟 d=0..6 个块注入，`γ_q` 取 1/8、1/16、1/32；断言 d≤4 时 1/16 无持续振荡，d=6 时 1/32 仍稳定；记录超调/收敛时间。
- `case_gain_margin`：把 `γ_q` 逐步加倍到振荡，验证理论边界 `γ_q d ≈ π/4` 的 2× 裕度。
- `case_actuator_deadband`（B）：deadband=4、CFG 在飞时冻结；断言无连续 CFG、无积分饱和。

### 6.3 突发、低样本、carrier

- `case_burst_onset`：前 50 块 E~2，突然 2 块 E=20；断言 m 在前馈下 1–2 块内加够，突发结束后 16 块内回落，无永久抬升。
- `case_low_sample`：<16 个块时 q 不执行，m 保持；第 17 块开始更新；`p_valid=false` 时冻结。
- `case_carrier_loss`：alive 从 256 降到 200，E 上升，m 增加；alive=0 冻结；恢复后 q/Δ 不被清零。
- `case_no_artificial_deflation`：任何路径都不出现 `p−δ`/`q−δ`；`E` 原样进入 RM。

### 6.4 执行机构 A

- `case_per_block_nm`：同一 session 交替 h.n/m = 128/128、120/136、136/120；断言块缓冲区大小不变、parity 偏移正确、NACK 重放用记录 n/m。
- `case_late_ledger_nm`：晚到 share 在 per-block n/m 下 credit/note/finalize；跨块 n/m 不同不串位。
- `case_codec_cache`：LRU 命中/未命中/淘汰；构建失败回退当前 m；缓存命中率统计。
- `case_est_not_reset`：换 m 不清 4 s 窗口/分位数；只有 INIT/reset 清。
- `case_header_accept`：h.n+h.m=shares 的任意拆分被接受；n+m≠shares 被拒绝；m=0/n=0 被拒绝。

### 6.5 安全备份

- `case_first_fail_backup`：连续 8 块 first_ok=0 → m 加 `max(2,n/16)`，冻结 1 RTT；恢复后回控制律。
- `case_r_fail_backup`：TX 本地 r_fail>2% 连续 2 tick → m 临时 +2，禁降 1 s；偏移衰减。
- `case_codec_build_fail`：构建失败 → 保持上一张图，FBK 回 m_current；不把失败形状放上线。

**旧测试处置：** 所有 `case_ctl_targets`、`case_ctl_hysteresis_*`、`case_ctl_down_*`、`case_ctl_burst`、`case_ctl_probe_*`、`case_convergence`、`case_no_oscillation`、`case_ctl_rejected`、`case_rate_mask`、`case_ctl_reverse`、`case_ctl_epoch_shape` 等档位/滞回断言全部删除或重写；`case_rate_ladder`/`case_shape_*` 改成 n/m 范围测试。`tests/unit/test_fec_sclc_ladder.c` 的 `boundary_ppm` 表删除。`host/sim_sclc_adaptive.c` 改成连续轨迹（m 随时间），不再断言 40/50/70 档。

---

## 7. 数值：待 calib 解析结果

曲线到达前，先用解析占位跑通结构：

| 量 | 占位 | 来源/替换 |
|---|---|---|
| `extra` | 下行 8，上行 2 | `tbt_fec.h:1786-1787` |
| `m_min/m_max` | 下行 16/192，上行 4/28 | codec 构建范围；`shape()` 校验 |
| `Δ0` | `extra + 2` 份 | 标定 `m_codec(n,ε)−Q` 替换 |
| `σ` | `max(1, IQR/1.349)`，冷启动 2 | E 样本/P² |
| `w=W/T_b` | 1.0 | 实测 RTT/block 周期替换 |
| `γ_q` | 1/16 share/block | 延迟测试后定 |
| `γ_Δ` | 1/64 share/block | 盈余测试后定 |
| `ε` | 运行时公式，夹 `[1/(n+m), 5%]` | §2.3 |

calib-down/up 到达后：

1. 每个 `(n,m,p)` 的 `first_fail/trials` 做 Wilson 区间和单调化；
2. 反演 `m_codec(n,ε)`，生成 `delta0` 桶；
3. 用 `host/sim_sclc_adaptive` 的连续轨迹验证：阶跃/突发/延迟下无持续振荡，wire/app 比与 m 的理论值一致；
4. 用 `tests/bench/bench_sclc_recovery` 或新 microbench 量 codec 构建 p50/p99，决定 A/B。

---

## 8. 风险与未决

1. **frontier 偏差**：不加 wake 快照，`T_b` 会被 frontier/退避抬高，m 会偏小；这是必须先改的观测点。
2. **A 的数据面范围**：per-block n/m 会触及 `tbt_fec_blk`、`late`、`est_block`、NACK 扫描、replay ring、TX packetizer；必须限定 n+m 恒定，否则 256 位图和 group 映射都要重做。
3. **codec 构建 p99**：`tbt_fec_sclc_new` 有多次 `zalloc`，可能在内存压力下变慢；A 的门槛是 p99≤2 ms、命中≥95%。达不到就 B。
4. **上行粒度**：每 group n+m=32，一份 parity 就是 3.1% 的码率；ε 下限 1/32，控制律不能承诺 1e-3。若产品需要更低失败率，必须加 block/share 数，不是调参。
5. **`m_target` 的 FBK 语义**：当前 `m_target` 是接收端 pending CFG 的 m；改成控制目标后，TX 必须存 `peer_m_target` 并在下一块使用。FBK 64 B 不变，但旧 peer 会把 `m_target` 当 CFG 建议；需要版本/能力位区分。
6. **多 flow**：A 的 per-block n/m 天然按 flow；B 的 CFG 是 session 级。若保留多 flow，A 更合适，但 `tbt_fec_late`/估计器要按 flow 聚合。
7. **block 512 profile**：旧 `profile_of` 在 rate 40 用 block 512；连续 m 会穿过这个点。第一版固定 block/L（下行 256/L16，上行 128/L16），不把 block 作为第二个执行机构；若要 512，另做 block 级滞回。
8. **`rate_pct` 兼容**：CFG/FBK 的 `rate_pct` 由 n/m 派生（最近 5% 或 0），控制律不读；旧 peer 可能按 rate_pct 建图，需要能力协商。
