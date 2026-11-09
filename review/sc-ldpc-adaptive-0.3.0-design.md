# SC-LDPC 自适应设计：0.3.0 固定 L8/rate0.5，0.3.1 L16/block512 + 码率自适应

状态：只读设计；本文件提交为文档 commit，不改代码。基线 HEAD `10ef014`
（phase 1a/1b：shape-invariant 存储、RX 估计器、super-block 边界切换）。
本文只新增/修改本文件，不改 `tbt_fec_data.c`、`tbt_fec_adapt.c` 或任何
热点代码。

标记：`[事实]` 代码/文档；`[实测]` 原型原始数据；`[设计]` 本文选择；
`[计算]` 概率/带宽推导；`[待测]` 必须上机或 host 仿真确认。

依据：`review/sc-ldpc-0.3.0-design.md`、`review/adaptive-fec-0.3.0-design.md`、
T0 `review/goal-v6-interfaces.md` 的 SC-LDPC 段、`tbtun/tbt_fec_adapt.{c,h}`、
`tbtun/tbt_fec_data.c` 的切换/估计器路径、原型 worktree
`/home/kunweiz/tbtun-worktrees/{fec,sclc-simd,sclc-gf16,sclc-encode}`。

---

## 0. 版本关系（已裁决）

| 版本 | 码型/profile | 码率 | 自适应 | NACK | 验收范围 | 已知限制 |
|---|---|---|---|---|---|---|
| **0.3.0 第一版** | SC-IRA `block256/L8`，P=1024 | **固定 0.50** | 无 | 必须 | 0/1/3/5/10% × 32/64/256 × 60 s | L8 p30 0.9990、p40 **0.9985**、p50 容量点≈0；20/30% 只作压力观察，不作发布门 |
| **0.3.1** | L16：A=`256/16`，B=`512/16` | **自适应 0.25–0.70** | 有 | 必须 | 0/1/3/5/10/20/30% × 32/64/256 × 60 s | share mapping/q_s、B profile 吞吐、GF16 待测 |

**0.3.0 已裁决基线**：

- 下行：`carriers=256`、`groups=1`、`n+m=256`、`block256/L8`、
  `P=1024`、`rate=0.50`、`n=128/m=128`。
- 上行：`carriers=256`、`groups=8`、`shares=32`、`block256/L8`、
  `P=1024`、`rate=0.50`、`n=16/m=16`。
- 恢复靠 NACK/replay；**不启用**码率控制、L16/block512、group 自适应。
- **已知限制（必须记录）**：T0 L8 在 p=0.40 的每码字 recovery 只有
  **0.9985**（p=0.30 0.9990），达不到 0.999；p=0.50 在 rate=0.50 是
  容量点，recovery≈0。0.3.0 不宣称这些点达到 0.999，NACK/replay 是
  兜底；0.3.1 用 L16/block512 + 码率自适应解决。

**0.3.1 目标**：L16/block512 的实测点 p30/r50 0.999821、
p40/r40 b512 0.999168、p50/r30 0.999862；按 §3–§5 的码率/控制律
覆盖 0–30% 验收矩阵。

## 0.1 结论先行

1. **SC-LDPC 的最终自适应是码率自适应，不是 GF(2) 的 n/m 类表。**
   0.3.0 先固定 `L8/rate0.5 + NACK` 落地；0.3.1 再打开码率自适应。
   码率由 SC-IRA 图决定；`n/m` 是每个 FEC block 的 data/parity share 数，
   由码率和几何派生。0.2.x 的 C0..C5 类表作废，不再接线。
2. **下行 0.3.0：1×256/L8/rate0.5 固定。** `carriers=256`、`groups=1`、
   `n+m=256`、`block256/L8`、`P=1024`、`n=128/m=128`；NACK/replay 兜底。
   **0.3.1**：几何不变，升级 L16/block512 + 码率
   `r=clamp(1-p_est-0.20, 0.25, 0.70)`（量化到 codec 支持的离散点）、
   `n=floor(r*256)`、`m=256-n`。没有跨 FEC block 的滑动窗口；`L` 是
   码字内部耦合长度，切换在 FEC block 边界安全。
3. **上行 0.3.0：8×32/L8/rate0.5 固定**，每 group `n=16/m=16`。
   **0.3.1**：8×32 码率自适应；可选 G=4（s=64）降低量化损失；
   G=32 被否（burst 容忍 m<8、block512 codec 状态 14.8 MiB 超预算）。
4. **0.3.1 码率阶梯只用实测强的点**：
   `{0.25, 0.30, 0.40, 0.50, 0.60, 0.70}`。0.35/0.45 在 p=0.40 的实测弱
   （0.998481 / 0.998084），列为 `[待测]` 候选，不进入 0.3.1 基线。
   `rate=1-p-margin` 的 margin 取 **0.20**；p=10/20/30/40/50 正好落到
   0.70/0.60/0.50/0.40/0.30。
5. **0.3.1 下行两个 codec profile**：A=`block256/L16`（P=2048 bit，
   用于 p≤30% 与 p>40%）；B=`block512/L16`（P=4096 bit，
   用于 30%<p≤40%，即码率 0.40 的区间）。L=8 在 p30/r0.50 只有 0.9990、
   p40/r0.40 只有 **0.9985**，是 0.3.0 的已知限制；L=16 的实测点是
   0.999821 / 0.999168。**T0 的“L 固定 8”在 0.3.1 修订为 L=16，
   并把 L 放进 FEC_CFG**。
6. **0.3.1 p_est 用 256 位 miss 位图，窗口改为 8 个 super-block
   （2 MiB 块约 4 s）。** 现有估计器 1 s 双桶 + 1024-share 门槛在 2 MiB
   块上永远不满足（n=76 时 1 s 只有 ~152 share）；必须扩窗口/扩位图。
   0.3.0 不启用估计器控制，只用 NACK/replay。
7. **0.3.1 切换复用现有两阶段提交**：`FEC_CFG(epoch, shape, P0, B0)` →
   `CFG_ACK` → 停在 P0 → `SWITCH_DONE` → swap；旧 codec 保留到提交，
   abort/rollback 不丢字节。SC-IRA 的码字自包含，FEC block 边界切换
   不会截断卷积链；这一点与 RLNC 的块局部假设一致。
8. **per-direction CFG 是 0.3.0 和 0.3.1 的共同硬前提**：现有 RX 强制
   `n+m`/`groups` 与本端配置相等（`tbt_fec_data.c:4418-4423`），session
   起始不交换形状。下行 1×256 与上行 8×32 不同，必须新增每端 TX 的
   初始 CFG，RX 按 peer shape 建 codec。否则 SC-LDPC 无法接线。
9. **目标**：0.3.0 以 NACK 后残余 ≤1e-6、字节精确、无 reset 为主；
   0.3.1 加 codec 级 per-superblock 失败率 ≤5%、wire/app ≤
   `1/r_wire × 1.05 + 0.01`、HND0 2t decode ≥4.215 MB/s（p=30/40/50）。
10. **验收矩阵**：`0/1/3/5/10/20/30%` × `32/64/256` carriers × 60 s，
    drop_ppm（FEC 层）与 netem（端到端）双轨。**0.3.0 发布门是 p≤10%**；
    20/30% 只作压力观察（L8 已知限制）；**0.3.1 覆盖全矩阵**，另加
    host BEC 1e6 super-block 门，因为 60 s 端到端测不出 1e-6 残余失败。

---

## 1. 事实基线

### 1.1 SC-IRA 码与 rate 参数化

- `[事实]` 生产构造 `SC_TERM_ACC`：payload block i 连 check
  `(base..base+4) mod n_base`，`base=floor(i*n_base/L)`，度 5；
  parity block a 连 check a 和 a+1，度 2（最后一个度 1）；
  `m=block/2`，`n_data=L*m`，`n_base=L` 时 rate 0.50。
  `sc_ldpc.c:26-34,326-354,367-388`。
- `[事实]` rate 精确公式：
  ```
  n_data = L*m
  R>50: n_base = round(L*(100-R)/R / m) * m / m   // 实际 = L/(L+n_base)
  R<50: n_base = L; n_extra = llround(n_data*(100-R)/R) - n_data
  n_par = n_base*m + n_extra; n = n_data + n_par
  rate_actual = n_data/n
  ```
  `rate_pct` 只接受 `{25,30,35,40,45,50,60,70}`；没有 0.55/0.65/0.80。
  `sc_ldpc.c:290-310`。
- `[实测]` block=256、L=16、m=128 的实际参数：

| rate_pct | n_data | n_base | n_extra | n_chk | n | rate_actual |
|---:|---:|---:|---:|---:|---:|---:|
| 70 | 2048 | 7 | 0 | 896 | 2944 | 0.695652 |
| 60 | 2048 | 11 | 0 | 1408 | 3456 | 0.592593 |
| 50 | 2048 | 16 | 0 | 2048 | 4096 | 0.500000 |
| 45 | 2048 | 16 | 455 | 2503 | 4551 | 0.450011 |
| 40 | 2048 | 16 | 1024 | 3072 | 5120 | 0.400000 |
| 35 | 2048 | 16 | 1755 | 3803 | 5851 | 0.350026 |
| 30 | 2048 | 16 | 2731 | 4779 | 6827 | 0.299985 |
| 25 | 2048 | 16 | 4096 | 6144 | 8192 | 0.250000 |

- `[实测]` 恢复率（symmetric BEC，2000 trial，除非另注）：
  - p30/r50 b256 L16：0.999821（1M，Wilson99 [0.999783,0.999852]）；
  - p40/r40 b512 L16：0.999168（1M，[0.999090,0.999239]）；
  - p40/r40 b256 L16：0.998988（160k，未过 0.999）；
  - p50/r30 b256 L16：0.999862（160k，[0.999763,0.999920]）；
  - p20/r70 b256 L16：0.9995（2000）；b512 L16：1.0；
  - p30/r60 b256 L16：0.9995（2000）；b512 L16：1.0；
  - p40/r35：0.998481（160k），**不是解**；p40/r45 binary b512 0.998084
    （1M），GF16 0.999944；0.35/0.45 不进入基线。
  - 容量点 rate=1-p 的 recovery 全 0；要稳定 0.999 需要余量
    ≥0.15–0.20。`sc-ldpc-sim.md:184-228`、`sc-ldpc-bitslice.md:125-144`。
- `[实测]` HND0 2t decode（binary bit-sliced，i.i.d.）：
  p30/r50 11.20 MB/s；p40/r40 5.91（b256/L16；b512/L16 5.5–6.5
  待复核）；p50/r30 5.80；p50/r25 4.00。
  `sc-ldpc-bitslice.md:125-135`。编码 bit64 e2e 105–217 MB/s，不是瓶颈。
- `[事实-推导]` 码字状态内存：`mem=6n+14n_chk+8E+176` 字节；
  b256/L16/r50 167,088；b256/L16/r40 约 231,600；
  b256/L16/r30 约 339,100；b512/L16/r50 334,000；
  b512/L16/r40 463,024；b512/L16/r30 约 678,000。
- `[事实]` 生产仓库 `TBT_FEC_CODEC_SC_LDPC=2` 只是保留值；
  `tbt_fec_params_check()` 拒绝，数据面 `mode=3` 返回 `-EOPNOTSUPP`。
  `tbt_fec.h:1089-1091`、`tbt_fec.c:1580-1581`、
  `tbt_fec_data.c:5710-5713`。

### 1.2 现有自适应框架：可复用与必须替换

- `[事实]` 估计器：500 ms 双桶（1 s 窗）+ 每 super-block EWMA α=1/8，
  `p_used=max(EWMA,win)`，门槛 1024 shares/4 supers，alive 过滤，
  idle 200 ms，alive<90%/stalled>10%/abandoned>1% 暂停。
  `tbt_fec_adapt.c:37-230`、`tbt_fec_adapt.h:49-56,202-213`。
- `[事实]` 控制律：6 类 `{7,1}..{2,6}`，250 ms 评估，up_run 3/down_run 6，
  dwell 3 s/10 s（振荡×2，cap 30 s），reverse 5 s，≤10 次/60 s，
  30 s 内 3 次冻结 30 s，down-probe（r_fail<0.5%、稳定 15 s），
  fail-up（r_fail>2% 连续 2 次），probe 失败禁 60 s。
  `tbt_fec_adapt.c:236-608`、`tbt_fec_adapt.h:218-242`。
- `[事实]` 切换：发送端 `FEC_CFG(APPLY)` 两阶段提交；CFG 50 ms 重试
  最多 3 次、500 ms 无 ACK abort、DRAIN 2 s 超时、COOLDOWN 5 s；
  接收端 PREP→ARMED→APPLIED，APPLIED 后见旧形状记录回滚。
  `tbt_fec_data.c:1834-2100,4241-4500`。
- `[事实]` **当前切换硬性要求 `groups` 与 `n+m` 不变**
  （`tbt_fec_data.c:1995-1998,4418-4423`）。因此：
  - 下行 1×256 的 `n/m` 在 256 内变化 → 允许（0.3.1 码率切换）；
  - 上行 8×32 的 `n/m` 在 32 内变化 → 允许（0.3.1 码率切换）；
  - group 数变化（8↔4/16/32）→ 0.3.0 不做；0.3.1 需 Phase 2。
- `[事实]` **估计器只在 `adaptive && groups<=32 && n+m==8` 时折桶**
  （`tbt_fec_data.c:3257-3259`）；`est_block` 把 `real` 截到 32、
  `miss_mask` 是 `u32`（`tbt_fec_adapt.c:85-110`）。下行 n=178、
  上行 s=32 都超出，必须扩。
- `[事实]` 发送端 `r_fail = ΔNACK 点名的 DATA share / ΔDATA 包`，
  1 s 差分，clamp 1e6（`tbt_fec_data.c:2133-2156`）；接收端 FBK
  `p_ewma_ppm` 字段实际装 `p_used`，`expected/received` 是 1 s 窗
  （`tbt_fec_data.c:3990-4036`）。
- `[事实]` **可复用**：双桶/EWMA/p_used/p_valid、三类暂停、250 ms
  评估节奏、dwell/reverse/rate/hold、probe 骨架、epoch 两阶段提交、
  P0/B0 重定位、replay reset、credit 重算、FBK 通道、r_fail 定义。
  **必须替换**：类表、`class_of_p` 阈值、`adapt_shape` 的
  `groups=32/w=2n/k=n/d=n`、`SHARES=8` 闸门、32 位 miss 掩码、
  RLNC codec 构造。

### 1.3 wire/ABI 事实

- `[事实]` 48 B FEC header：`+4 n`、`+5 m`（u8）、`+6 epoch` 是 PARITY
  短 share 长度（不是配置 epoch）、`+8 block_id`、`+16 seq`、
  `+24 win_len`（PARITY 1..n；**0=空块标记**，要求
  `share_index==n`、`flags==0`、`payload_len==0`、`epoch==0`）、
  `+26 payload_len`、`+28 conv=0x1006`、`+32 sid`、`+40 payload_crc32c`、
  `+44 header_crc32c`。`tbt_uapi.h:79-185`。
  **SC-LDPC 真 parity 必须 `win_len=n`，不能写 0。**
- `[事实]` FEC_CFG 实际 48 B：`epoch u32`、`n/m u16`、`w u16`、`k u16`、
  `d u8`、`flags u8`、`groups u16`、`switch_pkt u64`、`block_base u64`、
  `seed u64`、`crc32c u32`、`reserved u32`（必须 0）。
  `tbt_uapi.h:212-243`、`tbt_fec.c:732-799`。**没有 codec/symbol/L/rate
  字段**；`+44` reserved 非 0 直接 `-EPROTO`。
- `[事实]` CFG 校验 `groups == 256/(n+m)` 且 `256%(n+m)==0`
  （`tbt_fec.c:790-794`）。这只对 256 carriers 成立；32/64 carriers
  的 `groups=carriers/(n+m)`，必须改为按实际 carrier 数校验。
- `[事实]` FEC_FBK 实际 48 B：`epoch_lo u16`、`m_current u8`、
  `m_target u8`、`p_ewma_ppm u32`、`expected_shares u16`、
  `received_shares u16`、`epoch_hi u16`、`flags u8`、`n_current u8`、
  `carrier_bitmap[32]`。`tbt_uapi.h:249-281`。
  `m_current/m_target` 现在是 parity share 数，SC-LDPC 要改成
  当前/目标码率（u8，rate_pct）。
- `[事实]` per-direction CFG 未实现：每端 TX 各自发 CFG，但 RX 只接受
  与本端配置相同的 `n+m`/`groups`；session 起始不交换 CFG
  （`tbt_fec_data.c:4418-4423,5167-5181`）。
- `[事实]` T0 约束：`mode=sc-ldpc`；`w=0→256`，显式只接受 256/512；
  **L 固定 8**；`d` 可为 0；改 wire 字段必须 bump conv/HELLO。
  `goal-v6-interfaces.md:592-630,1046`。T0 G4 的容量点
  `(256,8,0.70,0.30)` 等 recovery=0，门本身需要修订。

### 1.4 几何事实

- `[事实]` `carriers = groups*(n+m)`，`carrier = group*(n+m)+share`，
  super-block = `groups*(n+m)*S = carriers*S`，`S=8192`。
  `tbt_fec_data.c:4-9,5716-5724`。
- `[事实]` 256 carriers、`groups*(n+m)=256` 时：super-block=2 MiB、
  `slots=8`、`K=7`、`credit=640`、replay≈14.8 MiB。下行 1×256 与
  上行 8×32 都是这个几何，arena/ring 不用改。
  `tbt_fec_data.c:5750-5758`、`tbt_fec.h:303-328`。
- `[事实]` 32 carriers 的 super-block 是 256 KiB、slots=64、K=59、
  credit=912；64 carriers 是 512 KiB、slots=32、K=29、credit=864。
- `[事实]` 交付边界是 super-block：所有 group 的同号 block 完成才交付；
  `next_deliver = base + (s+1)*groups*n`。`tbt_fec_data.c:2806-2838,
  3302-3306`。
- `[事实]` `rx->rec` scratch 现在按 `max(m,6)*S` 分配
  （`tbt_fec_data.c:5822-5828`）；SC-LDPC 下行 m 可达 192，
  但 SC codec 不用 RLNC 系数，`rec` 可缩到 0 或小缓冲。

### 1.5 必须先钉死的契约缺口

1. `[事实]` **share↔codeword 映射未定义**。`sc-ldpc-bit64.md:206-215`
   按 payload-only：b256/L16 每个 data share 32 个码字（payload 256 B），
   一批 64 码字覆盖 2 个 share；parity 如何跨 share 切分没有定义。
   `sc-ldpc-0.3.0-design.md:180-193` 又按完整码字口径，二者矛盾。
2. `[事实]` **低损码率点不足**：p<0.20 的生产尺寸实测只有 p=0.20/r0.70；
   0.70 是 `rate_pct` 上限，没有 0.80。
3. `[事实]` **L=8 与实测点冲突**：p30/r50 b256 L8 recovery 0.9990
   （2000 trial），L16 1.0；p40/r40 b256 L8 0.9985，L16 0.9995
   （sim）/0.999168（1M b512）。生产要用 L16。
4. `[事实]` **估计器窗口不匹配 2 MiB 块**：1 s/1024 share 在 n=76 时
   只有 ~152 share；p_valid 永不成立。
5. `[事实]` **bit-sliced scratch 不是 16 KB**：`unk+val` 就是
   2*8*n=32–64 KB，加 check 计数平面，b256/L16 约 150–180 KB/批。
6. `[事实]` **per-direction CFG 缺失**，上下行不同形状无法建会话。
7. `[事实]` **CFG 的 `groups=256/(n+m)` 校验**对 32/64 carriers 不成立。
8. `[事实]` **`groups==1` 时数据面把 `w` 推成 `n`**
   （`tbt_fec_data.c:5740-5749`）；SC-LDPC 的 `w` 是 codeword block
   256/512，不能沿用这条 RLNC 规则。

---

## 2. 设计公理与目标

### 2.1 公理

1. SC-LDPC 是最终码；GF(2) RLNC 只作过渡/回退，不再调它的 n/m 表。
2. 自适应只改**码率**和由码率派生的 `(n,m)`；不改 carrier 数、
   share 大小、super-block 大小、交付语义。
3. 每个方向独立：下行 1×256、上行 8×32 各有自己的估计器、FBK、
   控制律、epoch 和形状。
4. 切换只在 FEC block/super-block 边界；不丢字节、不 reset stream。
5. 控制律用连续公式 + 实测数值表；码率点是 codec 属性，不是
   丢包率分类。**不恢复 C0..C5 类表。**
6. 估计器用 share 级缺失（BEC 擦除率），不用 bit 级误码或 TCP 重传率。

### 2.2 目标（按版本）

| 目标 | 0.3.0 第一版（L8/rate0.5） | 0.3.1（L16/block512 + 码率自适应） |
|---|---|---|
| codec 级 per-superblock 失败率 | 不设门；L8 已知限制（p40 0.9985） | ≤5% |
| NACK 后残余失败率 | ≤1e-6 | ≤1e-6 |
| wire/app | 固定 2.0（r0.5） | ≤ `1/r_wire × 1.05 + 0.01` |
| HND0 2t decode | L8 bit-sliced 待测；标量 p30 1.057 MB/s；4.215 目标需补测 | ≥4.215 MB/s（p30/40/50） |
| 字节完整性 | 100% | 100% |
| 切换 | 无形状切换；per-direction INIT | ≤3 次/60 s；最大停顿 ≤500 ms |
| CPU | 固定基线 | ≤ 固定 SC-LDPC r0.50 +5% |
| 内存 | arena 16 MiB + codec + replay 15 MiB | 同左；codec ≤4 MiB |

---

## 3. 下行 256 单组

### 3.1 固定什么、调什么

`[已裁决]` 0.3.0 第一版固定 `block256/L8`、`P=1024`、`rate=0.50`、
`n=128/m=128`；不启用码率控制。下面的 A/B profile 与码率自适应是
0.3.1 的设计。

| 参数 | 0.3.0 第一版 | 0.3.1 |
|---|---|---|
| `carriers/groups/n+m` | 256/1/256 | 同左 |
| super-block | 2 MiB；slots=8、K=7、credit=640 | 同左 |
| share `S` | 8192 B | 同左 |
| codec 族 | SC-IRA `SC_TERM_ACC`，系统化，BEC peeling | 同左 |
| codec profile | `block256/L8`，P=1024 | A=`256/16`；B=`512/16` |
| 码率 `r` | **固定 0.50** | `r=clamp(1-p-0.20,0.25,0.70)`，量化 |
| `n/m` | **128/128** | `n=floor(r_actual*256)`、`m=256-n` |
| 冗余 | **50%** | `1-n/256` |
| 窗口 | 一个 FEC block；`L` 是码字内部耦合，无滑动窗口 | 同左 |
| 形状切换 | 无（只有 per-direction INIT） | 两阶段提交 |

**回答“窗口/码率/冗余如何随丢包调整”**：
- 窗口不调（两个版本都不调）。SC-IRA 的每个码字是自包含的
  （长度 `n=block*L` bit），FEC block 只是码字的集合；没有跨 FEC block
  的 look-ahead 或滑动窗。调窗口只会改延迟/内存，不会改善 BEC 恢复率。
- 0.3.0 码率固定 0.50，冗余固定 50%；丢包靠 NACK/replay 兜底。
- 0.3.1 码率调：丢包率升高 → `r_target` 降低 → `n` 降低、`m` 升高，
  冗余 `1-n/256` 升高。码率切换在 FEC block 边界，旧 block 的码字
  已完整包含在旧 share 里，不需要链式状态。
- 冗余是码率的派生量，不是独立旋钮；`n/m` 必须与 codec 的实际
  parity 位数一致（见 §3.3）。

### 3.2 码率阶梯与 `n/m` 表（0.3.1）

`[已裁决]` 0.3.0 固定 `rate=0.50`、`n=128/m=128`，不量化、不切换；
下面的码率阶梯是 0.3.1。

`[设计]` 码率阶梯（codec 实际 rate，不是 nominal rate_pct）：

```
R = {0.25, 0.299985, 0.40, 0.50, 0.592593, 0.695652}
```

0.35/0.45 是 `[待测]` 候选，不进 0.3.1 基线。理由：p40/r35 实测
0.998481、p40/r45 binary 0.998084，均低于 0.999；GF16 的 0.45 好，
但 0.3.1 下行先用 binary bit-sliced，GF16 作为上行备选。
0% 丢包时 0.70 仍发 30% parity，这是当前 `rate_pct` 的上限；
0.75/0.80 是后续 host 实验，不在 0.3.1 基线。

控制边界（`r_target=1-p-0.20`，向下量化到 R）：

| p_est | `r_target` | 量化码率 | profile | `n` | `m` | `n/256` | wire/app |
|---:|---:|---:|---|---:|---:|---:|---:|
| p≤10.43% | ≥0.70 | 0.695652 | A | 178 | 78 | 0.695313 | 1.438 |
| 10.43%<p≤20.74% | 0.60–0.70 | 0.592593 | A | 151 | 105 | 0.589844 | 1.695 |
| 20.74%<p≤30.00% | 0.50–0.60 | 0.500000 | A | 128 | 128 | 0.500000 | 2.000 |
| 30.00%<p≤40.00% | 0.40–0.50 | 0.400000 | B | 102 | 154 | 0.398438 | 2.510 |
| 40.00%<p≤50.00% | 0.30–0.40 | 0.299985 | A | 76 | 180 | 0.296875 | 3.368 |
| p>50.00% | <0.30 | 0.250000 | A | 64 | 192 | 0.250000 | 4.000 |

`[计算]` `n=floor(r_actual*256)` 保证 wire rate ≤ codec rate，parity
容量足够；余数（<1 share）在 parity 流末尾补零。例：
- r0.40：`n=102`，profile B（P=4096）时 `C=102*16=1632` 码字，
  `n_chk=6144`，parity 需 `1632*6144=10,027,008` bit，
  容量 `154*65536=10,092,544`，余 65,536 bit；若用 P=2048，
  `C=3264`、`n_chk=3072`，需求与余量相同（`C*n_chk` 与 P 无关）；
- r0.695652：`n=178`，`C=5696`，`n_chk=896`，
  parity 需 `5,103,616`，容量 `78*65536=5,111,808`，余 8,192 bit。
- r0.50：`n=128`，`C=4096`，`n_chk=2048`，正好 `8,388,608` bit。

`[设计]` `n/m` 的权威是 codec 的 `rate_pct` + `w/L`；`n/m` 只保证
容量。接收端按 CFG 的 `rate_pct/w/L/k/n/m` 重建 codec，并校验
`C=n*65536/P` 为整数、`C*n_chk ≤ m*65536`；不满足则 `CFG_NACK`。

### 3.3 codec profile 与 share 映射契约

`[设计]` 0.3.0 一个 profile；0.3.1 两个 profile：

| profile | `w` | `L` | `P` (payload bit/cw) | `C_s=65536/P` (cw/share) | 版本/用途 |
|---|---:|---:|---:|---:|---|
| 0.3.0 | 256 | 8 | 1024 | 64 | 固定 r0.5；L8 已知限制 |
| A | 256 | 16 | 2048 | 32 | 0.3.1；p≤30% 或 p>40% |
| B | 512 | 16 | 4096 | 16 | 0.3.1；30%<p≤40%（码率 0.40） |

`[实测]` 0.3.0 L8：p30/r50 0.9990、p40/r40 0.9985、p50/r50≈0
（容量点）——**已知限制**。0.3.1 A 在 p30/r50 0.999821；B 在
p40/r40 0.999168；A 在 p50/r30 0.999862。B 在 p50/r30 未测，所以
p50 用 A。A 在 p40/r40 0.998988 未过，所以 p40 用 B。

`[设计]` **share↔codeword 映射必须先钉死**。推荐：

1. 一个 data share 装 `B` 个码字的 payload 位平面：share j 含
   码字 `0..B-1` 的变量区间 `[j*65536/B, (j+1)*65536/B)`；
   一个码字跨 `P*B/65536` 个 share。丢失一个 share 擦除每个码字的
   `65536/(B*P)` 比例。
2. 一个 parity share 同理装 `B` 个码字的 parity 位平面；parity 位数
   `n_chk` 不整除时末尾补零。
3. `B` 取 64 的倍数（bit-sliced 解码器要求）。推荐 `B=64`；
   若 host BEC 在 p30/r50 的 50% 单 share 擦除下不达标，升到
   `B=128`（25%）或改用 profile B（25%）。
4. 映射的 pack/unpack 必须 host bit-exact 往返；`sc_pack64`/
   `sc_unpack64` 是 codeword-major，需要新增 share 级 interleaver。
5. 每个 FEC block 的码字数 `C=n*65536/P`（或每 group `C_g`）不必是
   64 的倍数；bit-sliced 解码器要求 batch 是 64 的倍数，最后一批用
   零/擦除码字补齐，解码后丢弃。补齐不改变有效码字的语义。

`[计算]` 单 share 擦除比例 `f=65536/(B*P)`：

| profile | P | B=64 | B=128 | B=256 |
|---|---:|---:|---:|---:|
| 0.3.0 L8 | 1024 | **100%** | 50% | 25% |
| A | 2048 | 50% | 25% | 12.5% |
| B | 4096 | 25% | 12.5% | 6.25% |

**0.3.0 的额外已知限制**：L8/P=1024 且 `B=64` 时，一个丢失的 data
share 擦除每个码字的全部 payload（100%）；peeling 无法启动，
**parity 不能恢复这个 share，NACK/replay 是唯一恢复路径**。若希望
parity 参与恢复，0.3.0 至少用 `B=128`（50%）或 `B=256`（25%），
代价是 bit-sliced batch/scratch 翻倍/四倍。0.3.1 的 L16/P=2048 在
`B=64` 时是 50%、`B=128` 时 25%，这是升级 L16 的另一个理由。

这是 SC-LDPC 能否在 share 级擦除下工作的关键；`[待测]` host 必须按
实际映射重测 q_s（每丢失 share 的恢复概率），不能直接用 bit 级
i.i.d. 的 0.999821。

### 3.4 失败率、wire、CPU 数值表

`[已裁决]` 0.3.0 固定 r0.5/L8 的已知限制（`[实测]`，2000 trial；
`P_sb` 用 §3.4 的一阶模型，N=128）：

| p | q_c | `r_nack=p(1-q_c)` | `P_sb` | 备注 |
|---:|---:|---:|---:|---|
| 20% | ≈1.0 | ≈0 | ≈0 | 0.3.0 发布门内 |
| 30% | 0.9990 | 3.0e-4 | 3.8% | 压力观察，不作发布门 |
| 40% | **0.9985** | 6.0e-4 | **7.4%** | **已知限制**，不作发布门 |
| 50% | ≈0 | ≈0.5 | ≈1 | 容量点；NACK 兜底，不可发布 |

0.3.0 的发布门是 p≤10%；p=20/30% 只作压力观察；p=40/50% 的
L8 数字记录为已知限制，由 0.3.1 的 L16/block512 + 码率自适应解决。

`[待测]` 0.3.0 L8 的 HND0 bit-sliced 2t 吞吐尚未测；标量 p30/r50
是 1.057 MB/s。4.215 MB/s 目标需要 L8 bit-sliced 补测；若不过，
0.3.0 的 CPU 目标降级为“记录值”，0.3.1 的 L16 已实测
11.20/5.91/5.80（p30/40/50）。

`[设计]` 下面是 **0.3.1** 的一阶模型：

```
N_data = n                         // 每 super-block DATA share 数
q_s    = 每个丢失 DATA share 被 codec 恢复的概率（host 实测）
P_sb   = 1 - (1 - p*(1-q_s))^N_data
r_nack = p*(1-q_s)                 // 每 DATA share 的 NACK 率
```

`[计算]` 先用 `q_s≈q_c`（每码字恢复率，相关擦除口径）做占位，
`[待测]` 用实际映射的 q_s 替换：

| p | rate | profile | n | m | q_c | `P_sb` | `r_nack` |
|---:|---:|---|---:|---:|---:|---:|---:|
| 0–5% | 0.695652 | A | 178 | 78 | ≈1.0 | <0.1% | <1e-5 |
| 10% | 0.695652 | A | 178 | 78 | 0.9999 | 0.18% | 1.0e-5 |
| 20% | 0.592593 | A | 151 | 105 | 0.9995 | 1.50% | 1.0e-4 |
| 30% | 0.500000 | A | 128 | 128 | 0.999821 | 0.69% | 5.4e-5 |
| 40% | 0.400000 | B | 102 | 154 | 0.999168 | 3.34% | 3.3e-4 |
| 50% | 0.299985 | A | 76 | 180 | 0.999862 | 0.52% | 6.9e-5 |

`[计算]` `P_sb` 最大值在 p40 的 3.34%，低于 5% 目标；`r_nack` 最大
0.033%，NACK 开销可忽略。p20 的 1.5% 是保守占位（p20/r60 比
p30/r60 容易，实际应更低）。

`[实测/待测]` HND0 2t decode 与目标：

| p | rate | profile | 2t MB/s | 目标 4.215 | 备注 |
|---:|---:|---|---:|---|---|
| 10% | 0.695652 | A | `[待测]` | 预计过 | rate0.70 的 checks 少 |
| 20% | 0.592593 | A | `[待测]` | 预计过 | 参考 p30/r50 11.20 |
| 30% | 0.500000 | A | 11.20 | 过 | 实测 |
| 40% | 0.400000 | B | 5.91（b256）；B 5.5–6.5 待复核 | 预计过 | b256 实测，b512 待测 |
| 50% | 0.299985 | A | 5.80 | 过 | 实测 |

wire/app 目标按 `1/r_wire × 1.05 + 0.01`：p≤10% ≤1.52；
p20 ≤1.79；p30 ≤2.11；p40 ≤2.65；p50 ≤3.55。对比固定 r0.50 的 2.0：
p≤10% 省 ~25%，p20 省 ~11%，p30 持平。

---

## 4. 上行 grouped

### 4.1 几何候选与代价模型

`[事实]` `carriers=256`，`groups*shares=256`。候选：

| G | s=shares/group | 每 group FEC block | super-block | codec 份数 | 备注 |
|---:|---:|---:|---:|---:|---|
| 32 | 8 | 64 KiB | 2 MiB | 32 | 旧 RLNC 32×8 家族；SC 不推荐 |
| 16 | 16 | 128 KiB | 2 MiB | 16 | |
| 8 | 32 | 256 KiB | 2 MiB | 8 | 本文基线 |
| 4 | 64 | 512 KiB | 2 MiB | 4 | 高 p 线效优先 |
| 2 | 128 | 1 MiB | 2 MiB | 2 | 接近单组 |
| 1 | 256 | 2 MiB | 2 MiB | 1 | 下行同构 |

`[已裁决]` 0.3.0：`G=8`、`s=32`、`block256/L8`、`rate=0.50`，
每 group `n=16/m=16`；不启用 group 切换、不启用码率自适应。
下面的代价模型/表是 0.3.1。

`[设计]` 选择代价：

```
r_c      = codec 实际码率
n(G)     = floor(s*r_c), m(G)=s-n(G)
r_w(G)   = n/s                         // wire rate
wire_cost= 1/r_w
mem(G)   = G * codec_state(P,L)
burst(G) = m(G)                        // 单组可容忍的连续丢 share 数
P_sb(G)  = 1-(1-p*(1-q_s))^(G*n)       // 与 G 近似无关
```

约束：
1. `mem(G) ≤ 4 MiB`；
2. 每 group 至少 64 个码字（一个 bit-sliced batch）：`n*65536/P ≥ 64`；
3. `burst(G) ≥ B_burst`（建议 8，覆盖 8 条连续 carrier 的局部故障）；
4. `P_sb(G) ≤ 5%`。

在这些约束下，`r_w(G)` 越大越好；同分时 G 大一点（隔离好），
但 `mem` 和 `burst` 会否决。

### 4.2 码率量化表（G=32/16/8/4/2）

`[计算]` `r_w=n/s`，括号内是相对 codec 实际码率的量化损失：

| 码率 `r_c` | G=32 (s=8) | G=16 (s=16) | G=8 (s=32) | G=4 (s=64) | G=2 (s=128) |
|---:|---|---|---|---|---|
| 0.695652 | 0.625 (10.2%) | 0.6875 (1.2%) | 0.6875 (1.2%) | 0.6875 (1.2%) | 0.6953 (0.05%) |
| 0.592593 | 0.500 (15.6%) | 0.5625 (5.1%) | 0.5625 (5.1%) | 0.5781 (2.4%) | 0.5859 (1.1%) |
| 0.500000 | 0.500 (0%) | 0.500 (0%) | 0.500 (0%) | 0.500 (0%) | 0.500 (0%) |
| 0.400000 | 0.375 (6.25%) | 0.375 (6.25%) | 0.375 (6.25%) | 0.3906 (2.3%) | 0.3984 (0.4%) |
| 0.299985 | 0.250 (16.7%) | 0.250 (16.7%) | 0.28125 (6.25%) | 0.2969 (1.0%) | 0.2969 (1.0%) |
| 0.250000 | 0.250 (0%) | 0.250 (0%) | 0.250 (0%) | 0.250 (0%) | 0.250 (0%) |

`[事实]` G=32 的 `burst=m`：r0.70 时 s=8,n=5,m=3 <8，不满足局部
故障约束；r0.60/r0.50 时 m=4 <8；r0.40 时 m=5 <8；r0.30 时 m=6 <8。
**32×8 在 256 carriers 下对 8 条连续 carrier 的局部故障不够**。
G=8 的 m：r0.70 10、r0.60 14、r0.50 16、r0.40 20、r0.30 23，
全部 ≥8。G=4 更大。这是推荐 G≤8 的硬理由。

### 4.3 group 选择律

`[设计]` 0.3.0 固定 `G=8`（32 shares/group）+ `rate=0.50`，不调
`n/m`。0.3.1 打开码率自适应，并可选 group 自适应；规则：

```
for G in {4, 8, 16, 32}:           // 按 2 的幂，s=256/G
    n = floor(s*r_c); m = s-n
    if n < 1 or m < 1: continue
    if G*codec_state(P,L) > 4 MiB: continue
    if n*65536/P < 64: continue
    if m < B_burst: continue
    if P_sb(G) > 5%: continue
    score = n/s                    // 越大越好
    pick max score; tie -> smaller G? 或 larger G?
```

`[设计]` 结果表（`B_burst=8`，`mem≤4 MiB`）：

| p_est | 码率 | 推荐 G | `n/m` per group | `r_w` | wire/app | 理由 |
|---:|---:|---:|---|---:|---:|---|
| p≤10.43% | 0.695652 | 8 | 22/10 | 0.6875 | 1.455 | G=4/2 只多 0–1%，G=8 隔离更好 |
| 10.43%<p≤20.74% | 0.592593 | 8 | 18/14 | 0.5625 | 1.778 | G=4 可到 0.578，按 wire 目标可选 |
| 20.74%<p≤30.00% | 0.500000 | 8 | 16/16 | 0.500 | 2.000 | 所有 G 相同，取隔离 |
| 30.00%<p≤40.00% | 0.400000 | 4 | 25/39 | 0.3906 | 2.560 | G=8 0.375；G=4 省 4.2% wire |
| 40.00%<p≤50.00% | 0.299985 | 4 | 19/45 | 0.2969 | 3.368 | G=8 0.28125；G=4 省 5.3% |
| p>50.00% | 0.250000 | 4 | 16/48 | 0.250 | 4.000 | 相同；G=4 的 m 更大 |

这张表是上述代价模型在给定 `r_c`、`mem`、`burst` 约束下的解，不是
0.2.x 的 C0..C5 类表：p 只通过 `r_t=1-p-0.20` 和码率阶梯进入，
`G` 的选择由量化损失、codec 状态、burst 容忍度共同决定。

`[设计]` 0.3.0 不做 group 切换，因为现有 `tbt_fec_tx_switch_begin`
拒绝 `groups`/`n+m` 变化（`tbt_fec_data.c:1995-1998`）。0.3.1 需要：
- 放宽几何门；
- `tx->g[]`/`rx->blk[]`/`req_ns`/`req_block`/replay ring 按新 G 重建；
- 旧 codec/旧数组保留到提交，或采用 stop-and-swap 放弃回滚；
- `rx->rec` scratch 按最大 m 预分配。

0.3.0 的“固定 G=8”指 256 carriers；32/64 carriers 按
`G=carriers/32` 缩放（64→2、32→1），`s=32` 不变。

### 4.4 上行数值表（0.3.1 G=8 基线）

`[已裁决]` 0.3.0：G=8，每 group `block256/L8`、`rate=0.50`、
`n=16/m=16`，不随 p 变。下面的表是 0.3.1。

| p | 码率 | profile | `n/m` | `r_w` | wire/app | `N_data` | `P_sb` |
|---:|---:|---|---|---:|---:|---:|---:|
| p≤10.43% | 0.695652 | A | 22/10 | 0.6875 | 1.455 | 176 | ≤0.2% |
| 10.43%<p≤20.74% | 0.592593 | A | 18/14 | 0.5625 | 1.778 | 144 | 1.4% |
| 20.74%<p≤30.00% | 0.500000 | A | 16/16 | 0.500 | 2.000 | 128 | 0.69% |
| 30.00%<p≤40.00% | 0.400000 | B | 12/20 | 0.375 | 2.667 | 96 | 3.15% |
| 40.00%<p≤50.00% | 0.299985 | A | 9/23 | 0.28125 | 3.556 | 72 | 0.49% |
| p>50.00% | 0.250000 | A | 8/24 | 0.250 | 4.000 | 64 | ~0 |

`[计算]` G=8 的 codec 状态：A r0.50 8×167 KB=1.34 MiB；
B r0.40 8×463 KB=3.70 MiB；A r0.30 8×339 KB=2.71 MiB。
4 MiB 预算内。G=32 的 B r0.40 是 14.8 MiB，放不下；G=32 的 A r0.50
是 5.3 MiB，也超 4 MiB。**这是 32×8 不能配 L16 codec 的硬约束。**

`[计算]` CPU：每 group 码字数 `C_g=n*32`（P=2048）或 `n*16`
（P=4096），G=8 时 r0.50 是 512、r0.30 是 288、r0.40 是 192，
都 ≥64（一个 bit-sliced batch）。总码字数 `G*C_g` 与 G 无关；
8 个 codec 串行解码，HND0 2t 吞吐同 §3.4（11.20 / 5.91 / 5.80
MB/s，p=30/40/50），CPU 与 G=1 同量级；额外成本是 8 份 codec
状态的 cache 占用（1.34–3.70 MiB），已计入 `mem` 约束。

---

## 5. 控制律（0.3.1；0.3.0 不启用）

`[已裁决]` 0.3.0 固定 `rate=0.50`，不运行本节的控制律；p_est/FBK/
码率切换都是 0.3.1 的设计。0.3.0 只用 NACK/replay 兜底。

### 5.1 p_est：接收端 miss 位图

`[设计]` 定义 `p = 缺失 DATA share / 存活过滤后的 DATA share`：

- 每个 group block 解码后，构建 **256 位** `miss_mask`（32 B），
  位 i=1 表示 DATA share i 缺失（`dpresent==0`）；只统计 `i<real`。
- `alive` 过滤：`carrier = group*(n+m)+i`；若 carrier 不 alive，
  分子分母都不计。alive 定义沿用
  `up && !stopping && ESTABLISHED && now-last_rx < max(4*RTT,1s)`
  （`tbt_fec_adapt.h:190-200`）。
- 每 super-block 聚合 `S_b=Σ real_live`、`M_b=popcount(miss_mask)`；
  `p_b=M_b/S_b`。
- **窗口**：8 个 500 ms 桶（4 s 环），`p_win` 覆盖最近 4 s；
  `p_ewma += (p_b-p_ewma)/8`（α=1/8）。`p_used=max(p_ewma,p_win)`。
- **p_valid**：`supers_win≥8 && sent_win≥512`，且 alive≥90%、
  stalled≤10%、abandoned≤1%。`[计算]` 2 MiB 块在 4.215 MB/s 下
  约 0.5 s/块，4 s≈8 块；n=76 时 8 块=608 share，满足 512。
  现有 1 s/1024 share 门槛在 n=76 时永不满足，必须改。
- **快路径**：最近 2 个桶（1 s）的 `p_fast`；若
  `p_fast > max(p_used,10%)` 且 `p_fast > p_ewma+5pp`，标记 burst，
  允许在 min-gap 后立即降码率。
- **未知/放弃**：`real` 未知的块计 `unknown`，不进 p；abandoned
  >1% 冻结。沿用现有语义。
- **码率诊断**：另计 `codec_fail_shares`（codec 未恢复的缺失 share）
  和 `nack_req_shares`（NACK 点名），只做 procfs/FBK 诊断，
  不进 p 的分子。

`[设计]` API 变化：
```
void tbt_fec_est_block(struct tbt_fec_est *e, u64 now_ns, u16 group,
                       u16 real,
                       const u8 miss_mask[TBT_FEC_BITMAP_BYTES],
                       const u8 alive[TBT_FEC_BITMAP_BYTES]);
```
`TBT_FEC_BITMAP_BYTES=32`；`shares` 可达 256；不再把 `real` 截到 32。
桶数组从 2 扩到 8（或保留 2 个 500 ms 桶但把窗口计到 8 个
super-block；实现上任选，语义必须 8 块/4 s）。

### 5.2 r_fail 与残差反馈

`[设计]` 发送端本地计算：

```
r_fail = ΔNACK 点名的 DATA share / ΔDATA 包发送数   // 1 s 差分
```

沿用 `tbt_fec_data.c:2133-2156` 的口径。`r_fail` 是控制律的硬约束：
- `r_fail > 2%` 连续 2 次评估 → 降一档码率，`no_raise_until=now+30s`；
- `r_fail < 0.3%` 且稳定 15 s → 允许 probe-up 一档；
- 新 epoch 的前 1 s 不计 `r_fail`（避免旧 epoch NACK 污染）。

### 5.3 码率目标与量化

```
p    = p_used_ppm / 1e6
r_t  = 1 - p - 0.20
r_t  = clamp(r_t, 0.25, 0.70)
r_nom= max{ r in R : r <= r_t }      // 向下量化，保 margin
n    = floor(r_nom_actual * s)
m    = s - n
```

- `margin=0.20` 对所有 p；低 p 时 0.70 上限绑定。
- 不恢复类表；`r_t` 连续，量化只到 codec 支持的码率点。
- 若 `n<1` 或 `m<1` 或 parity 容量不足，降一档再算；仍不足则保持
  当前形状并告警。

### 5.4 滞回、驻留、probe

| 项 | 值 | 说明 |
|---|---|---|
| 评估周期 | 250 ms | 沿用 |
| 降码率（加冗余） | 连续 2 次（0.5 s） | 安全方向，快 |
| 降码率 dwell | 3 s | |
| 升码率（减冗余） | 连续 6 次（1.5 s） | 风险方向，慢 |
| 升码率 dwell | 10 s | |
| 迟滞 | 2 pp | SC 曲线陡，0.5 pp 太小 |
| 最小间隔 | 3 s | 沿用 |
| 反向冷却 | 5 s | 沿用 |
| 速率帽 | 10 次/60 s | 沿用 |
| 振荡冻结 | 30 s 内 3 次 → 30 s | 沿用 |
| probe-up | `r_fail<0.3%` 稳定 15 s；观察 10 s；`r_fail>1.5%` 连续 2 次回退；禁 60 s | 只在 p≤30% 开 |
| fail-down | `r_fail>2%` 连续 2 次 | 优先于 p |

`[设计]` probe-up 是“少发冗余”的探测，和旧 down-probe 方向相反。
旧 down-probe 是降类（加冗余）拿 wire 增益；SC-LDPC 的 wire 增益来自
**升码率**，所以 probe 方向要反过来。只在 p≤30%、`r_fail<0.3%` 时开；
p>30% 不开，避免高损下抖动。

### 5.5 切换边界、epoch、回滚

`[事实]` SC-IRA 码字自包含：一个码字是 `block*L` bit 的独立码，
FEC block 是码字集合；没有跨 FEC block 的卷积状态。因此
**FEC block/super-block 边界切换是安全的**，不会截断链式状态。
`review/sc-ldpc-0.3.0-design.md:212-220`。

`[设计]` 复用现有两阶段提交：

```
TX STEADY --decision--> CFG_SENT
CFG_SENT --CFG_ACK--> DRAIN
CFG_SENT --timeout 500ms / CFG_NACK--> STEADY (abort)
DRAIN: packetizer 停在 seq>=P0
DRAIN --SWITCH_DONE && peer_pkts>=P0--> SWAP --> STEADY(new)
DRAIN --timeout 2s / carrier 全死--> ABORT --> COOLDOWN(5s) --> STEADY(old)
RX STEADY --CFG--> PREP (allocate codec)
PREP --ok--> ARMED (CFG_ACK)
PREP --alloc fail--> STEADY (CFG_NACK)
ARMED --frontier>=P0--> APPLIED (swap shape, SWITCH_DONE)
ARMED --timeout 2s--> ROLLBACK
APPLIED --first new-shape record--> STEADY(new), free old codec
APPLIED --old-shape record seq>=P0--> ROLLBACK
```

- `P0` 是旧形状 super-block 边界；`B0=cur_block+1`；映射用
  `base_pkt/base_block` 相对坐标。
- `epoch` 每方向独立，单调 +1；CFG 携带 `epoch`；RX 只接受更大
  epoch。`sess->adapt.epoch` 是本地 TX epoch，`rx->epoch` 是 peer
  TX epoch，二者不能混。
- 旧 codec 保留到第一条新形状记录；abort 不 swap，旧状态天然保留。
- **post-commit 回滚**：新 epoch 的前 4 个 super-block 若
  `r_fail>2%` 或 `codec_fail>10%`，发送端在下一个边界发
  `CFG(APPLY, 旧 shape, 新 epoch)` 回滚；旧 shape 的 codec 可在
  swap 时重建，或把 `old_codec` 保留到第二次 swap。`[设计]` 建议
  保留 `old_codec` 到提交后 2 s，代价是一份 codec 内存；若内存紧，
  回滚时重建。
- **速率切换粒度**：降码率可跨多档（p 突增时直接到目标）；
  升码率一次一档（probe 例外，一次一档）。跨多档降码率避免多次
  stall。

### 5.6 初始、per-direction CFG、快启动

`[设计]` session 起始：
1. 每端用配置的初始形状（默认 `rate=0.50`，A profile）建 TX codec。
2. 每端发 `FEC_CFG(INIT, epoch=1, shape)` 给对端；对端 RX 按 peer
   shape 建 codec。`INIT` 用 flags 的新 bit 或 epoch=1 区分；
   旧端不支持则回退到“两端同形状”的旧行为。
3. `p_valid` 之前不切；`p_valid` 后 **fast start**：若 `r_fail<0.3%`，
   允许一次直接跳到 `r_nom`，不受升码率 6 次/dwell 限制；若 p 高，
   直接降到目标。初始 0.50 只是安全占位，不是测量最优。
4. `adaptive=off` 时按配置的固定 `rate`/`n/m` 跑，不发 CFG。

### 5.7 控制伪代码

```
every 250ms:
  if (!adaptive || switching || !stream_active) return
  if (now - last_eval < 250ms) return
  o = snapshot(estimator)
  if (!o.p_valid || o.pause_idle || o.pause_carrier || o.pause_abandoned)
      return
  p = o.p_ppm; r = o.r_fail_ppm
  if (probe_active) { ... observe/revert/commit ...; return }
  if (r_valid && r > 20000) {
      if (++fail_run >= 2) lower_rate(one_step, FAIL_DOWN); return
  } else fail_run = 0
  r_t = clamp(1 - p - 0.20, 0.25, 0.70)
  target = quantize_down(r_t)
  if (target > cur) {
      if (p <= threshold_up(cur) + 2000) { up_run=0; return }
      if (++up_run < 6) return
      if (!dwell_ok(UP) || !rate_ok()) return
      switch_begin(target, UP)
  } else if (target < cur) {
      if (p >= threshold_down(cur) - 2000) { down_run=0; return }
      if (++down_run < 2) return
      if (!dwell_ok(DOWN) || !rate_ok()) return
      switch_begin(target, DOWN)
  } else { up_run=down_run=0 }
```

`threshold_up(cur)`/`threshold_down(cur)` 由码率阶梯反推：
`p_up = 1 - r_cur - 0.20`，`p_down = 1 - r_prev - 0.20`；
2 pp 迟滞。

### 5.8 失败/回滚矩阵

| 失败 | 检测 | 动作 |
|---|---|---|
| CFG 丢失 | 500 ms 无 ACK | 重发 3×50 ms；仍无 → abort，旧形状 |
| RX 分配失败 | CFG_NACK | abort；降一档目标，不重试同形状 |
| frontier 不到 P0 | DRAIN 2 s | abort；旧形状从 P0 继续 |
| SWITCH_DONE 丢失 | RX 每 100 ms 重发 | TX 收到任一份推进 |
| APPLIED 后 TX abort | 旧形状记录 seq≥P0 | RX 回滚 |
| 新 epoch r_fail 高 | 前 4 个 super-block 的 r_fail >2% | 下一边界回滚/降档，禁 30 s |
| codec 分配内存不足 | ENOMEM | 保持旧形状，降档，告警 |
| carrier 全死 | any_tcp_alive 假 | abort；走既有 stall/reset |
| epoch 回绕 | u32 | 会话内不现实；忽略 ≤cur |

---

## 6. Wire/ABI 变更

### 6.1 Header

- `[设计]` 48 B header 不变；SC-LDPC 真 parity 的 `win_len=n`（不是 0），
  `epoch` 字段=0（满 share），`share_index∈[n,n+m)`。
- `[事实]` 空块标记 `win_len=0` 仍保留给“无数据”的 group；SC-LDPC
  不用它表示 parity。

### 6.2 FEC_CFG v2（48 B 最小扩展）

`[设计]` 复用现有 48 B，`+44` reserved 拆为 codec/symbol/L/rate_pct；
bump `TBT_FEC_CONV 0x1006 → 0x1007`、HELLO v3→v4：

```
+0  u32 epoch
+4  u16 n
+6  u16 m
+8  u16 w              // SC: codeword block 256/512
+10 u16 k              // SC: payload bits per codeword (1024/2048/4096)
+12 u8  d              // SC: 0
+13 u8  flags          // bit0 APPLY, bit1 ABORT, bit2 INIT
+14 u16 groups
+16 u64 switch_pkt
+24 u64 block_base
+32 u64 seed
+40 u32 crc32c
+44 u8  codec          // 1=rlnc, 2=grouped, 3=sc-ldpc
+45 u8  symbol         // 0=binary, 1=gf16
+46 u8  L              // 8 (0.3.0) / 16 (0.3.1)
+47 u8  rate_pct       // 25/30/40/50/60/70
```

校验按 codec 分支：
- RLNC：`w>=k`、`d<=w`、`d!=w>1` 等原规则；
- SC-LDPC：`w∈{256,512}`、`L∈{8,16}`、`rate_pct∈R`、
  `k=P`、`d=0`、`n+m=256/G`、`C=n*65536/P` 为整数、
  `C*n_chk<=m*65536`；`groups=carriers/(n+m)`（不是固定 256）。
- `rate_pct` 是 nominal；实际 rate 由 codec 构造推导；RX 用
  `rate_pct+w+L` 重建并校验 `k`。

### 6.3 FEC_FBK（0.3.1 语义）

`[设计]` 48 B 不变。0.3.0 可继续用现有 `m_current/m_target` 或不用
FBK；0.3.1 把语义改为：

```
+0  u16 epoch_lo
+2  u8  rate_current    // 当前 RX 码率，rate_pct
+3  u8  rate_target     // 建议码率，rate_pct
+4  u32 p_used_ppm      // max(EWMA, window)
+8  u16 expected_shares // 窗口内 alive DATA share
+10 u16 received_shares // expected - miss
+12 u16 epoch_hi
+14 u8  flags            // ACK/NACK/DONE/ROLLBACK/P_VALID
+15 u8  n_current
+16 u8[32] carrier_bitmap
```

`m_current/m_target` 的字段名保留（wire 兼容），语义改为
`rate_current/rate_target`；两端一起升级。`codec_fail`/`r_fail`
不塞进 FBK：`r_fail` 由发送端本地 NACK 计数；`codec_fail` 进 procfs。

### 6.4 per-direction 初始 CFG

`[设计]` session 起始每端发一次 `FEC_CFG(INIT, epoch=1)`：
- `tbt_fec_tx` 保存 `local_shape`（本端 TX）。
- `tbt_fec_rx` 保存 `peer_shape`（对端 TX），估计器/FBK 用 peer_shape。
- `rx_on_cfg` 的 `n+m/groups == 本端配置` 门改为
  “`cfg` 属于 peer 方向”；本地 TX 形状不参与校验。
- 若对端不发 INIT（旧端），回退到本端配置的同形状行为。

### 6.5 配置键

`[设计]` 新增/明确：

| 键 | 值 | 默认 | 说明 |
|---|---|---|---|
| `mode=sc-ldpc` | 已有 | — | 数据面从拒绝改为建 SC codec |
| `rate=` | 25/30/40/50/60/70 | 50 | 0.3.0 固定 50；0.3.1 初始/自适应起点 |
| `block=` 或 `w=` | 256/512 | 256 | 0.3.0 固定 256；0.3.1 A/B |
| `L=` | 8/16 | 8（0.3.0）/16（0.3.1） | 耦合长度；T0 修订 |
| `symbol=` | binary/gf16 | binary | 0.3.0/0.3.1 下行 binary；GF16 0.3.1 上行可选 |
| `groups=` | 1..32 | `carriers/(n+m)` | 上行 8；下行 1 |
| `adaptive=` | on/off | 0.3.0 off；0.3.1 on | 已有 |

`n/m` 不再要求用户手填；由 `rate`+`groups`+`carriers` 派生。
调试可覆盖 `n/m`，但控制器会重新派生。

---

## 7. 实现分期

| 阶段 | 版本 | 内容 | 依赖 |
|---|---|---|---|
| P0 | 0.3.0 | 钉死 L8/rate0.5 的 share↔codeword 映射、payload/parity 打包、NACK 语义 | 无 |
| P1 | 0.3.0 | host codec：`sc_ldpc` L8 + bit-sliced decode + bit64 encode + share interleaver；bit-exact | P0 |
| P2 | 0.3.0 | `tbt_fec_ops` SC-LDPC；CFG v2（codec/L/rate=50）、per-direction INIT、header `win_len=n` | P1 |
| P3 | 0.3.0 | 数据面：下行 1×256、上行 8×32；NACK/replay；已知限制测试 p≤10 | P2 |
| P4 | 0.3.1 | L16/block512 profile A/B；host BEC 校准 q_s/映射 | P3 |
| P5 | 0.3.1 | 估计器：256 位 miss、8 块窗口、快/慢路径、p_valid | P4 |
| P6 | 0.3.1 | 控制律：码率表、目标公式、滞回/dwell/probe/fail-down；替换类表 | P5 |
| P7 | 0.3.1 | 切换：codec 工厂、post-commit 回滚、per-direction epoch | P6 |
| P8 | 0.3.1 | 验收 harness：20/30%、SC-LDPC 配置、恢复率/RSS、方向注入 | P7 |
| P9 | 0.3.1 | group 自适应（可选）：放宽几何门、G∈{4,8,16,32}、重建数组 | P8 |

**热点文件所有权**：`tbt_fec_data.c` 是唯一集成点；P2–P7 由一名
owner 串行落地，其他 agent 只提交 host 测试/小 patch。估计器/
控制律纯逻辑放 `tbt_fec_adapt.c`（host 可测），`tbt_fec_data.c`
只留 hook。

---

## 8. 验收矩阵

### 8.1 环境与方法

- 双端同版本，`fec=on crypto=on profile=http-kal`。**0.3.0**：
  `rate=50`、`adaptive=off`（L8/rate0.5 固定）；**0.3.1**：初始
  `rate=50`、`adaptive=on`（L16/block512 + 码率自适应）。
- **drop_ppm**（FEC 层）：`debug_loss=1`，对 carrier 写
  `carrier <i> drop_ppm <ppm>`；FEC record 在写 socket 前丢弃，
  TCP 不重传，只由 parity/NACK 修复。i.i.d. 均匀。
- **netem**（端到端）：`tc qdisc ... netem loss X% [25%]`；TCP 会重传，
  p_fec 低于配置值，正常。
- 矩阵：loss `{0,1,3,5,10,20,30}%` × carriers `{32,64,256}` ×
  60 s 稳态（10 s warm-up）× 3 次；drop_ppm 与 netem 各一轮；
  另加 5% burst（25% correlation）、reorder 1%、RTT 20/100 ms。
  **0.3.0 发布门只取 p≤10%**；p=20/30% 只作压力观察；p=40/50%
  记录 L8 已知限制。**0.3.1 覆盖全矩阵**。
- 方向：drop_ppm 默认 far（下载发送端）；上行验收在 near 注入。
- 字节完整性：1 GiB 文件 sha256；或 60 s 流式 sha256。
- 记录：`/proc/net/tbtun/{stats,peers,tunnels}`、dmesg、`ss -ti`、
  `/proc/<pid>/status`（RSS）、`perf stat`/`top`。

### 8.2 每 carrier 数的几何

| carriers | 下行 | 上行 0.3.0 | 上行 0.3.1 |
|---:|---|---|---|
| 256 | 1×256 | 8×32 | 8×32 或 4×64 |
| 64 | 1×64 | 2×32 | 2×32 或 1×64 |
| 32 | 1×32 | 1×32 | 1×32 |

32/64 carriers 的 `n/m` 用同一公式：下行 `n=floor(r*carriers)`、
`m=carriers-n`；上行 `n=floor(r*s)`、`m=s-n`，`s=32`（或 0.3.1
的 64）。codec 的 `w/L/P` 不随 carrier 数变；只变 `n/m/C`。

`[已裁决]` 0.3.0 固定 `rate=0.50`：256 下行 `n=128/m=128`；上行
每 group `n=16/m=16`；32/64 carriers 按比例缩放。下面的期望形状表
是 **0.3.1**（稳态目标，不是硬编码；实际由 p_est 量化，允许 ±1 档
滞回）：

| loss | 256 下行 | 64 下行 | 32 下行 | 256 上行 (G=8) | 64 上行 (G=2) | 32 上行 (G=1) |
|---:|---|---|---|---|---|---|
| 0/1/3/5/10% | 178/78 A | 44/20 A | 22/10 A | 22/10 A | 22/10 A | 22/10 A |
| 20% | 151/105 A | 37/27 A | 18/14 A | 18/14 A | 18/14 A | 18/14 A |
| 30% | 128/128 A | 32/32 A | 16/16 A | 16/16 A | 16/16 A | 16/16 A |

`[事实]` 现有 harness 固定 `n=4 m=4 carriers=C`，两端同形状；SC-LDPC
需要 per-direction 配置和 `rate/block/L/groups` 键。`[设计]` 新增
`down_rate`/`up_rate`（或 near/far 分侧）配置。

### 8.3 指标

| 指标 | 来源 | 用途 |
|---|---|---|
| app MB/s | near `rx_delivered` 增量 / 窗口 | 主指标 |
| wire/app | far `live_bytes+retired_bytes` / app | 冗余成本 |
| p_est 误差 | c13 `p_ppm` - 注入 ppm | 估计器 |
| NACK 率 | c11 `fec_nack_req_shares` / ΔDATA | r_fail |
| codec 失败率 | c10 `fec_decode_recovered` / `fec_decode_blocks` | 恢复率 |
| 切换/停顿 | c13 switches/aborts/rollbacks；c6 `rx_max_stall_us` | 稳定性 |
| 洞/reset | c6 `holes_unrecoverable`、c11 `fec_reset_hole` | 正确性 |
| CPU | `/proc/stat` 窗口 | 开销 |
| RSS | `/proc/<pid>/status` | 内存 |
| 字节校验 | sha256 | 正确性 |

`[事实]` 现有 harness 未采集 RSS/恢复率；`[设计]` P8 补上。

### 8.4 成功标准

#### 0.3.0 发布门（L8/rate0.5 + NACK）

| # | 标准 | 值 |
|---|---|---|
| S0.1 | 字节一致、无洞、无 reset | p=0/1/3/5/10%，32/64/256 carriers，60 s |
| S0.2 | NACK 后残余 | ≤1e-6；NACK 率记录 |
| S0.3 | p=20/30% | 只作压力观察，不作发布门；记录 `P_sb`/NACK |
| S0.4 | p=40/50% | 记录 L8 已知限制（p40 0.9985；p50 容量点≈0），不作发布门 |
| S0.5 | per-direction INIT | 下行 1×256 / 上行 8×32 建会话成功，无形状错 |

#### 0.3.1 发布门

| # | 标准 | 值 |
|---|---|---|
| S1 | 字节一致、无洞、无 reset | 所有 run |
| S2 | p_est 误差 | p≤10% ≤±1.5 pp；p=20% ≤±2.5 pp；p=30% ≤±3.0 pp |
| S3 | app vs 固定 SC-LDPC r0.50 | p0–10 ≥1.30×（或 wire/app ≤0.80×）；p20 ≥1.10×（或 ≤0.85×）；p30 ≥0.98× |
| S4 | wire/app | ≤ `1/r_wire × 1.05 + 0.01` |
| S5 | codec 失败/super-block | ≤5%；残余 ≤1e-6（host） |
| S6 | 切换/停顿 | ≤3 次/60 s；max stall ≤500 ms；A→B→A ≤1 次/10 s |
| S7 | CPU | ≤ 固定 r0.50 +5% |
| S8 | RSS | arena 16 MiB + codec ≤4 MiB + replay 15 MiB |
| S9 | 32/64 carriers | S1/S2/S6 过；app ≥ 固定 ×0.95 |
| S10 | netem 5% burst | 字节一致；app ≥ 固定 ×0.95 |
| S11 | 切换中 kill carrier | 字节一致；abort/rollback；无 reset |
| S12 | 20/30% | 字节一致；app ≥ 固定 ×0.95；无洞 |
| S13 | host BEC | 1e6 super-block：`P_sb≤5%`、`P_res≤1e-6`；HND0 2t ≥4.215 MB/s（p30/40/50）；pack/unpack bit-exact |

`[设计]` S3 的“或 wire/app”分支覆盖 CPU-limited 场景：当 wire 不是
瓶颈时，升码率不会提高 app，但会降低 wire/app，仍算达标。

### 8.5 host BEC 门（60 s 测不出的部分）

- **0.3.0**：用 L8/rate0.5 的实际映射跑 1e6 super-block，量化
  p=20/30/40% 的 q_c、`P_sb`、NACK 率；p40 的 0.9985 作为已知限制
  记录，**不作为 ≤5% 发布门**。p50 记录容量点行为。
- **0.3.1**：L16/A/B 全矩阵跑 1e6 super-block，`P_sb≤5%`、
  `P_res≤1e-6`；Wilson 上界记录。
- 用实际 share↔codeword 映射和 erasure 图样，模拟 1e6 super-block
  每 `(p, rate, profile, G)`。
- 统计：codec 失败 super-block 比例、NACK 修复、2 轮后残余洞。
- 采样量：1e6 super-block；`P_res=1e-6` 期望 ~1 事件，用 Wilson
  上界判断，不用点估计。
- 吞吐：HND0 2t ≥4.215 MB/s；本机相关擦除 ≥125 MB/s（1G 目标，
  单独记录，不混 HND0）。

---

## 9. 风险清单

| # | 风险 | 证据 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | share↔codeword 映射未定义 | `sc-ldpc-bit64.md:206-215` | 静默错/无增益 | P0 钉死；host bit-exact；按实际映射测 q_s |
| R2 | L8 是 0.3.0 的已知限制 | p30/r50 0.9990；p40/r40 0.9985；p50/r50≈0 | 0.3.0 p40/p50 NACK 重，不作发布门 | 0.3.0 固定 L8/rate0.5 + NACK，发布门 p≤10%；0.3.1 升 L16/block512 + 码率自适应 |
| R3 | per-direction CFG 缺失 | `tbt_fec_data.c:4418-4423` | 上下行不同形状无法建会话 | INIT CFG；peer_shape |
| R4 | 估计器 1 s/1024 share 在 2 MiB 块永不 valid | `tbt_fec_adapt.h:51-54` | 自适应不生效 | 8 块/4 s 窗；256 位 mask |
| R5 | 0.35/0.45 弱 | p40/r35 0.998481；binary r45 0.998084 | 选错码率 | 不进基线；host 门后再开 |
| R6 | 0.3.1 p40/r40 的 P_sb≈3.3% | 本文计算 | NACK 频繁 | 目标 5%；NACK/replay；r_fail 反馈 |
| R7 | 切换截断卷积链 | 误解 | 错误设计 | 码字自包含；FEC block 边界安全 |
| R8 | group 切换需重建数组 | `tbt_fec_data.c:1995-1998` | 0.3.0 不能做 | 0.3.0 固定 G=8；0.3.1 Phase 2 |
| R9 | G=32 + L16 codec 内存超预算 | 5.3–14.8 MiB | OOM | G≤8；4 MiB 预算 |
| R10 | 32/64 carriers 的 CFG groups 校验错 | `tbt_fec.c:790-794` | 会话建不起来 | 按 carriers 校验 |
| R11 | T0 G4 容量点 recovery=0 | `sc-ldpc-sim.md:184-228` | 验收门错 | 改用 margin 点 0.50/0.40/0.30 |
| R12 | 1G/125 MB/s 不是 HND0 目标 | HND0 5.8–11.2 | 目标错位 | 按 4.215 MB/s 验收；1G 单独 |
| R13 | 旧类表 dead code 被接线 | `tbt_fec_adapt.c:236-608` | 错误切换 | 替换，不接线；host 单测 |
| R14 | 默认 n=4,m=1 + adaptive 触发 begin 拒绝/epoch 空转 | `tbt_parse.c:407-408` | 自适应不生效 | SC 默认 rate=50；begin 门按新几何 |
| R15 | 20/30% verdict 未实现 | `lib.sh:1339` | 矩阵不判定 | P8 加 S12 |
| R16 | FBK 语义变更 | `m_current/m_target` | 两端不兼容 | conv/HELLO bump；一起升级 |
| R17 | `win_len=0` 空块标记冲突 | `tbt_fec.c:196-334` | parity 被当空块 | SC parity 写 `win_len=n` |
| R18 | `rx->rec` 按 max m=6 分配 | `tbt_fec_data.c:5822-5828` | m 大时越界 | SC 不用 rec；或按 max m 预分配 |
| R19 | bit-sliced scratch 16 KB 估计偏小 | 实际 150–180 KB/批 | 内存不足 | 按 180 KB/批预算 |
| R20 | GF16 未进下行基线 | 只有 p40 数据 | 高损恢复率 | binary bit-sliced 先；GF16 0.3.1 |

---

## 10. 决策记录

### 已定

0. **版本已裁决**：0.3.0 第一版固定 `L8/rate0.5 + NACK`；
   0.3.1 升 `L16/block512` + 码率自适应。T0 L8 在 p40 只有 0.9985，
   记为 0.3.0 已知限制，不作发布门。
1. 下行 0.3.0 固定 1×256/L8/rate0.5；0.3.1 几何不变，只调码率/`n/m`；
   窗口两版都不调。
2. 0.3.1 码率阶梯 `{0.25,0.30,0.40,0.50,0.60,0.70}`；0.35/0.45 待测。
3. 0.3.1 margin=0.20；`r=1-p-0.20`，向下量化。
4. 0.3.1 下行 profile A=256/16、B=512/16；B 只用于 30%<p≤40%。
5. 上行 0.3.0 固定 8×32/L8/rate0.5；0.3.1 允许 G∈{4,8}，按代价表。
6. 0.3.1 p_est 用 256 位缺失位图；8 块/4 s 窗；快/慢两路径。
7. 0.3.1 降码率快（2 次/0.5 s），升码率慢（6 次/1.5 s、10 s dwell）；
   迟滞 2 pp；probe-up 只在 p≤30%。
8. 0.3.1 切换复用两阶段提交；epoch 每方向独立；post-commit 回滚用新 CFG。
9. wire（两版共用）：CFG v2 48 B（codec/symbol/L/rate_pct）；FBK 语义改码率；
   conv 0x1007/HELLO v4；header `win_len=n`。
10. 验收：0.3.0 发布门 p≤10%（0/1/3/5/10%），20/30% 压力观察；
    0.3.1 覆盖 0/1/3/5/10/20/30% × 32/64/256 × 60 s + host BEC 1e6。

### 未决（需要后续确认）

1. `q_s`（每丢失 share 的恢复概率）的实际映射实测值；本文用
   `q_s≈q_c` 占位，必须 host 校准。
2. share 映射的 `B` 取 64/128/256；取决于 p30/r50 的 50%/25%/12.5%
   单 share 擦除 host 结果。
3. 0.3.1 group 自适应的隔离收益是否值得重建数组的复杂度；
   如果 G=4/8 的差异 <2%，可长期固定 G=8。
4. GF16 上行是否在 0.3.1 打开；binary bit-sliced 的 HND0 2t 在
   p50/r30 是 5.80，已过 4.215；GF16 的 0.45 码率可省 wire，
   但 HND0 5-seed 恢复率 0.99972 需复核。
5. T0 G4 门的正式修订文本；本文建议改为 margin 点
   `(256,16,0.50,0.30)`、`(512,16,0.40,0.40)`、`(256,16,0.30,0.50)`。
6. 控制帧是否加 MAC/认证；当前威胁模型没有，伪造 CFG 可 DoS，
   不在本设计范围。

---

## 附：关键证据索引

- T0 SC-LDPC：`review/goal-v6-interfaces.md:244-274,570-630,938-947,1046`
- 现有 SC 设计：`review/sc-ldpc-0.3.0-design.md`
- 现有自适应设计：`review/adaptive-fec-0.3.0-design.md`
- 构造/曲线：`/home/kunweiz/tbtun-worktrees/fec/review/sc-ldpc-sim.md`、`.csv`
- bit-sliced：`.../sclc-simd/review/sc-ldpc-bitslice.md`
- bit64 编码：`.../sclc-encode/review/sc-ldpc-bit64.md`
- GF16：`.../sclc-gf16/review/sc-ldpc-gf16.md`、`sc-ldpc-gf16-hnd0-5seed.csv`
- 估计器/控制律：`tbtun/tbt_fec_adapt.c:37-608`、`tbt_fec_adapt.h:22-250`
- 切换/数据面：`tbtun/tbt_fec_data.c:1834-2100,3257-3298,3990-4182,4241-4500,5710-5760`
- wire：`tbtun/tbt_uapi.h:79-281`、`tbt_fec.c:140-334,732-840`
- harness：`benches/adaptive/{lib.sh,matrix.sh,README.md}`
- 256-carrier 基线：`review/wan-experiment/arm64/steady-256-20261108/summary.txt`

---

**本文为设计文档提交（`review/sc-ldpc-adaptive-0.3.0-design.md`）；
未改任何代码。**

---

## 0.5.0 实现补记（2026-11）

下行二进制 1×256/L8 的码率阶梯已经落地，wire 仍为 `TBT_FEC_CONV 0x1007`：

- 六个 n/m 拆分：64/192、76/180、102/154、128/128、151/105、178/78
  （25/30/40/50/60/70%），`n+m=256` 不变。
- 图结构取 `c=max(n,m)` 个 parity 变量和 check 每个耦合块，payload 度 5，
  check 度 ≤7。`n>m` 时每块打孔后 `n-m` 个 parity 变量：编码器照算，
  解码器按擦除起手，只发送 m 个 parity share。这样高速率点仍是强
  rate-1/2 图加打孔，避免把 check 度抬到 12~17 后阈值反而下降。
- `sclc_mask` 下行打开全部六点；`FEC_CFG` 接受任意阶梯 n/m
  （`block=256`、`groups=1`、`k=n*1000/256`）。旧 0.4.0 端会 NACK
  该 CFG，控制器回退，失败安全；自适应下行切档要求两端均为 0.5.0。
- host 实测（`host/test_fec_sclc_ladder`，8/16 个块的共享级恢复）：
  25%@p=55%、30%@p=50%、40%@p=40%、50%@p=30%、60%@p=20.74% 均为
  1.000；70%@p=10.43% 为 0.95（门限 0.80）。控制器在
  0/10/20/30/35/45% 的 suggestion 分别落到 70/70/60/50/40/30，
  建议形状均可构建并修复。
- `host/bench_sclc_downlink` 用进程序 CPU 时间的单次最优值计时，六点
  wire 吞吐均 ≥250 MB/s；payload 吞吐随码率下降，低码率点不以它作门限。
