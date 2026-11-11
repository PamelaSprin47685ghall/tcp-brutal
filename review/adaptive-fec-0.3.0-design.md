# 0.3.0 自适应 FEC n/m 设计方案

状态：只读设计，未改代码。基线 HEAD `a6eb228`（0.2.3 准备，hold 默认开启）。
目标：256 carriers 不变，按方向测量丢包率，运行时在 block 边界切换 `(n,m)`；两端同步；不丢字节、不中断用户流。
标记约定：`[事实]` 来自代码；`[设计]` 是本方案的选择；`[计算]` 是概率/带宽推导，不是实测。

---

## 0. 结论先行

1. **0.3.0 的形状族固定 `n+m=8`，`groups=32`，`m∈1..6`。** 协议字段支持 `n+m∈{8,16,32,64,128,256}`，但 `n+m≠8` 会改变 `groups`、`span`、RX arena 每块切片、replay ring 尺寸和 `req_ns` 数组，需要热重建，列为 Phase 2。`n+m=8` 时 `groups*(n+m)=256`，每 super-block 恒 2 MiB、`slots=8`、`K=7`、RX arena 恒 16 MiB、TX ring 恒 1 MiB；切换只改“切片指针 + codec + 映射基址”，不重建 carrier，不重新分配大块内存。
2. **复用已存在但未接线的 `FEC_CFG(0x34)` / `FEC_FBK(0x35)`**。`TBT_FEC_CONV` 从 `0x1005` 升到 `0x1006`，`TBT_WIRE_VERSION_V6` 从 2 升到 3。两端必须同为 0.3.0；旧端在 HELLO 版本检查处被拒绝，这是仓库既有的硬切换策略。
3. **丢包率用接收端逐块 miss 位图**：`miss = parity 恢复的 DATA ∪ NACK 请求的 DATA`，按 1 s 窗口 + 每 super-block EWMA 平滑；发送端用 NACK 请求率 `r_fail` 做残差反馈。这样不依赖 `fec_late_share`（它是迟到/重复，不是丢包），也不受 `decode_blocks` 只统计“尝试解码”的偏差影响。
4. **切换用两阶段提交**：`CFG(epoch,shape,switch_pkt,block_base)` → 接收端 `CFG_ACK` → 发送端停在 `P0` 等 frontier → 接收端 `SWITCH_DONE` → 发送端换 TX、发第一条新形状记录。接收端保留旧 codec 到第一条新形状记录，支持发送端 abort 后的回滚。
5. **映射改为相对坐标** `base_pkt/base_block`：`group=(seq-base_pkt)%groups`，`block_id=base_block+((seq-base_pkt)/groups)/n`。`base=0` 时与现行为完全一致，先做无行为变化的机械替换，再加切换。
6. **shape 字段的并发保护**：RX 侧把 `group/share/block_id` 计算移进 `rx->lock`（放置本来就在锁内），切换在同一把锁内换 `n/m/groups/slots/base_*/data/parity` 指针；TX 侧新增 `tx->shape_lock` 保护 NACK 处理器读到的 `n/m/groups`，ACK 处理器改用缓存 `tx->credit_cap`，不再读形状。
7. **验收双轨**：`drop_ppm`（FEC 记录在写 socket 前丢弃，TCP 不重传，只由 parity/NACK 修复）验证估计器和控制律；`netem`（TCP 段丢失，TCP 会重传）验证端到端。矩阵 0/1/3/5/10% × 32/64/256 carriers × 60 s，字节校验、吞吐、切换次数/停顿、CPU。

---

## 1. 现状硬约束（代码事实）

### 1.1 形状在 session 创建时定型

- `n/m` 来自 `tunnel->cfg`，在 `tbt_fec_data_init()` 一次性读取：`tbt_fec_data.c:4564-4565`。`groups=carriers/(n+m)`：`:4591-4600`。`rx->n/m/groups/slots`：`:4657-4660`。`tx->groups/n/m`：`:4714,4748-4749`。
- codec、RX arena、replay ring、credit 都在 init 分配：`rx->codec` `:4686`，`tx->codec` `:4759`，`tx->replay` `:4703`，`rx->blk` `:4671,4791-4812`，`credit` `:4733-4734`。
- 唯一参数变更路径是重写 `/proc/net/tbtun/config` 同 id tunnel → 重建 session/carriers（`tbt_session.c:237-316`）。`carrier down|up|drop_ppm` 是唯一运行中动作（`tbt_session.c:550-627`）。
- FEC 模式单流：`tbt_session_fec_stream_live()` 拒绝第二条流（`tbt_session.c:865-879`，`tbt_sock.c:713-718`）。

### 1.2 每条 FEC 记录自描述 n/m，但接收端强制相等

- 48 B 头：`+4 n`、`+5 m`（`tbt_uapi.h:158-176`，`tbt_fec.h:45-64`）。DATA/PARITY/marker 都写 `h.n/h.m`（`tbt_fec_data.c:1463-1464,1240-1241,1292-1293`）。
- 接收端：`group=h.seq%rx->groups`、`h.n==rx->n && h.m==rx->m`、`h.block_id==(h.seq/rx->groups)/rx->n`，任一不满足返回 `-EPROTO`（`tbt_fec_data.c:4097-4104`）。
- `-EPROTO` 会关闭 carrier（`tbt_rx.c:1108-1110,1018-1023`，`tbt_carrier.c:1002-1005`）。**切换时形状不一致绝不能走 `-EPROTO`，必须改为丢弃 + 计数。**

### 1.3 FEC 模式当前没有任何丢包率输入

- `tbt_tx_fbk()` 在 FEC 下直接返回 0（`tbt_tx.c:3712-3715`）；旧 ARQ 的 `c->loss_ppm` 只在 `tbt_tx_hole_check()` 更新（`tbt_tx.c:1641-1647`），FEC 下恒为 0。
- `FEC_CFG/FBK` 只有 pack/unpack 和 host 测试；内核接收端明确忽略：`tbt_fec_data.c:4085-4086`（`/* FBK/CFG are accepted and ignored by this cut. */`）。
- `fec_decode_blocks` 是“进入 codec 的尝试次数”，`fec_decode_recovered` 是“成功恢复的 share 数”，`fec_late_share` 是“迟到/重复”，都不是 p（`tbt_fec_data.c:2054-2060,3662,4163,4210,4225`）。

### 1.4 几何、span、credit 与 FIN 都绑在 `groups*n`

- `span = groups*n`：delivery `base = s*groups*n`（`tbt_fec_data.c:2535`），frontier `next_deliver=(s+1)*groups*n`（`:2818`），FIN `pkt_base%(groups*n)==0`（`tbt_fec.h:105-113`，`tbt_fec_data.c:4388-4389`）。
- credit：`tbt_fec_credit_packets(groups,n,replay_k,slots,1024)`（`tbt_fec.h:303-328`；ACK 发送 `tbt_fec_data.c:2088-2089`；发送端更新 `:3774`）。
- replay ring 的 `(block_id,group,share)` 索引与 `n/m/groups` 绑定（`tbt_fec.c:459-473,512-560`）。
- NACK 位图 `(group,share)` 语义与形状绑定（`tbt_uapi.h:195-199`，`tbt_fec_data.c:3796-4083`）。

### 1.5 形状并发读点

| 读者 | 读什么 | 上下文 | 现状保护 |
|---|---|---|---|
| carrier `w_rx` → `tbt_fec_rx_record()` | `rx->n/m/groups/slots`、`b->data/parity` | 可能是 softirq/工作项 | `rx->lock` 只护放置；形状字段在锁外读（`tbt_fec_data.c:4094-4104`） |
| `w_fec_rx` decode/deliver/NACK | `rx->n/m/groups/codec/blk` | 进程上下文 | 单写者 `rx->run` |
| `w_fec_tx` packetizer | `tx->n/m/groups/codec/g[]` | 进程上下文 | 单写者 `tx->run` |
| NACK 处理器（carrier RX） | `tx->n/m/groups/replay` | 可能是 softirq | `replay_lock` 只护 ring 存取；形状在锁外读 |
| ACK 处理器（carrier RX） | `tx->groups/n` 算 credit cap | 可能是 softirq | 无 |
| `tbt_fec_rx_fin()` | `rx->groups/n` | carrier RX | `rx->lock` 内（`tbt_fec_data.c:4388`） |

### 1.6 其他

- 没有 1 s 定时器；有 50 ms `w_poll`（`tbt_carrier.c:921-978`）和 10 ms 旧 `w_fbk`（`tbt_tx.c:3870`）。
- `rx->lock` 是 `spinlock_bh` 叶锁：不做分配/decode/交付/取别的锁（`tbtun.h:725` 注释）。
- `TBT_FEC_STALL_MS=30000`（`tbt_fec_data.c:71`）；切换超时必须远小于它。
- `drop_ppm` 对 FEC 记录也生效，且在写 socket 前丢弃（`tbt_fec_data.c:551-557`），是测试 FEC 层的理想注入点。
- 现有动态策略 `tbt_fec_m_for_p()` / `tbt_fec_params_dynamic()` 未被内核调用（`tbt_fec.c:1446-1528`；仅 `host/test_fec.c:1680-1790`）。`params_dynamic` 在 256 carriers、p≤20% 时返回 SINGLE（`w=384,d=64`），与 init 对 `groups==1` 的 `w=n,d=w/2` 推导不一致（`tbt_fec_data.c:4613-4623`）。**不要直接接线；用新表替换 `m_for_p` 的阈值。**

---

## 2. 几何与不变量

### 2.1 256 carriers、`n+m=8`

`carriers=256`，`shares=8`，`groups=32`；每 super-block `groups*shares*S = 256*8192 = 2 MiB`；`slots=16MiB/2MiB=8`；`K=7`；RX arena 16 MiB；replay ring `7*256*8258 ≈ 14.8 MiB`；TX ring `min(credit*S,1MiB)=1MiB`。

| 类 | n | m | DATA carriers | PARITY carriers | span=G*n | credit=min(5*span,1024) | 冗余 m/(n+m) | 理想块失败率* |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| C0 | 7 | 1 | 224 | 32 | 224 | 1024 | 12.5% | 0.269% @1% |
| C1 | 6 | 2 | 192 | 64 | 192 | 960 | 25.0% | 0.135% @3% |
| C2 | 5 | 3 | 160 | 96 | 160 | 800 | 37.5% | 0.037% @5% |
| C3 | 4 | 4 | 128 | 128 | 128 | 640 | 50.0% | 0.043% @10% |
| C4 | 3 | 5 | 96 | 160 | 96 | 480 | 62.5% | 0.002% @10%, 0.123% @20% |
| C5 | 2 | 6 | 64 | 192 | 64 | 320 | 75.0% | 0.008% @20% |

\* `[计算]` 二项分布 `P(>m losses)`，iid 假设，仅作下界；实际受 codec band 矩阵非 MDS、突发丢失影响，会更高。控制律必须用实测 `r_fail` 修正，不能把表当保证。

### 2.2 `n+m=8` 时 carrier 映射不变

`carrier = group*(n+m)+share`（`tbt_fec_data.c:1430`）。`n+m=8` 时 `group=carrier/8`、`share=carrier%8` 与 n/m 无关；改 n 只改变 `share < n` 的 DATA/PARITY 解释，carrier 对象、socket、`sess->car[]`、`fec_outq[]`、`drain[]` 全部不动。**这是 0.3.0 选 `n+m=8` 的核心理由。**

### 2.3 Phase 2：`n+m` 变化

| n+m | groups | span | 说明 |
|---:|---:|---:|---|
| 16 | 16 | 16n | 低损效率类 `n=15,m=1`（6.25%）；或 `8/8` |
| 32 | 8 | 32n | 高损大块 `16/16`（50%） |
| 64 | 4 | 64n | `32/32` |
| 128 | 2 | 128n | `64/64` |
| 256 | 1 | 256n | 单 group；`w/d` 语义与 init 不一致，先统一再启用 |

`n+m` 变化会改变 `groups`，从而改变 `group=seq%groups`、`carrier→(group,share)`、`slots`、`K`、`req_ns` 数组和每块 `data/parity` 切片。Phase 2 需要预分配/重建，协议不变。

---

## 3. 丢包率估计

### 3.1 信号与用途

| 信号 | 来源 | 用途 | 注意 |
|---|---|---|---|
| 逐块 miss 位图 | 本设计新增；parity 恢复 ∪ NACK 请求 | **主 p** | 只统计需要修复的 DATA share，正是 FEC 尺寸控制需要的量 |
| `fec_decode_recovered` | `tbt_fec_data.c:2059-2060` | miss 的子集 | 只覆盖“需要且能解码”的块 |
| NACK 请求 share 数 | `fec_nack_req_shares`（`tbt_fec_data.c:3823`） | 残差 `r_fail`、miss 的另一半 | 含重复请求；NACK 扫描已有 in-flight 过滤 |
| `fec_late_share` | `:3662,4163,4210,4225` | 诊断 | 迟到/重复，不是丢包 |
| `lost_out` / `icsk_retransmits` | `tcp_sock`（`tbt_carrier.c:440`） | TCP 层快速提示/无 FBK 时回退 | 含伪丢失；只作 hint |
| `delivery_rate` / `app_limited` | `tp->rate_delivered/rate_interval_us`（`tbt_session.c:508-510`） | 判断 app-limited | 不直接当 p |
| RTT/RTO | `c->rtt_us`、`rx->rtt_us`（`tbt_fec_data.c:2886`） | 判定窗口 J、NACK hold | 不直接当 p |
| replay miss | `fec_replay_miss_evicted` 等（`tbtun.h:1270-1276`） | 健康度 | ring 太小，不是路径丢包 |
| carrier alive/stalled | `c->up/stopping/stalled/last_rx_ns` | 排除 | 一个死 carrier 占 1/256 share，但 10% 死 carrier 会严重偏置 |

### 3.2 逐块观测（核心）

对每个 group block `(g,b)`，定义：

- `real`：本块真实 DATA share 数，来自 PARITY 的 `win_len`、FIN tail，或 `ndata==n` 时取 n。`win_len_set==false && !total_known && ndata<n` 时 `real` 未知，**跳过该块**（计 `sample_unknown`）。
- `sent = real`（只统计 DATA；parity 丢失单独计 `fec_parity_miss`，不参与 n/m 控制）。
- `miss_mask`：`real` 位的位图，只置位“该 DATA share 需要修复”：
  1. **parity 恢复**：`tbt_fec_blk_decode()` 成功时，`dpresent_snap[i]==0` 且 `i<real` 的槽被 codec 写回；在 `tbt_fec_rx_decode_one()` 的锁内写回阶段置位（`tbt_fec_data.c:3458-3477`）。
  2. **NACK 请求**：`tbt_fec_nack_scan()` 实际命名一个 DATA share 时置位（`:3316-3332`，已有 `req_ns/req_block` 去重）。
  3. 两者取并集；同一位只算一次。
- 不统计“晚到但没触发修复”的 share：它没有消耗 parity/NACK，不是尺寸控制的成本。
- 不统计 `late`/`dup`：它们不是丢失。

聚合点：`tbt_fec_deliver_ready()` 在 `next_super++` 前，持 `rx->lock` 读取每个 group 的 `real` 和 `miss_mask`，求和后释放；`next_super++` 后块槽可能被 carrier RX 复用（`tbt_fec_data.c:2817-2841`）。

分母的 carrier 存活过滤：对 `i<real`，`carrier = g*(n+m)+i`；若该 carrier 不 alive，则 `sent` 和 `miss` 都不计。alive 定义（接收端视角）：

```
up && !stopping && sk_state==TCP_ESTABLISHED &&
(now - c->last_rx_ns) < max(4*c->rtt_us, 1s)
```

若 alive carrier 比例 <90%，冻结自适应（`fec_adapt_pause_carrier`），不切换。

被放弃的未 ready 块（槽被复用/流复位）：在 `tbt_fec_blk_reset()`（`tbt_fec_data.c:1927-1946`）里 `atomic64_inc(&fec_est_abandoned)`；若 `abandoned/sent > 1%`，冻结并保持当前形状。不要用它们估计 p（会低估）。

### 3.3 平滑、窗口与公式

每个 super-block 完成后（`w_fec_rx` 单写者）：

```
S_b = Σ_g real_g              // alive-filtered
M_b = Σ_g popcount(miss_mask_g & live_mask_g)
p_b = M_b / S_b               // S_b>0
p_ewma += (p_b - p_ewma) / 8  // 定点 ppm；α=1/8
```

1 s 双桶窗口：`S_win`、`M_win`；`p_win = M_win/S_win`。上报 `p_used = max(p_ewma, p_win)`。

样本充分性：`S_win ≥ 1024` 且完成块数 ≥4。`[计算]` 二项 2σ：N=1024 时 p=1% → 0.62 pp，3% → 1.07 pp，5% → 1.36 pp，10% → 1.88 pp。分类阈值间隔 1.5–2 pp，配合 0.5 pp 滞回与连续计数，噪声不会直接触发切换。

`p_ewma` 的整数形式（ppm）：

```
delta = p_b_ppm - p_ewma_ppm;
p_ewma_ppm += delta / 8;      // 向零截断即可
```

空闲：`tx->seq` 200 ms 未动 → 冻结（不更新、不切换）。空闲 >2 s → 以 τ=10 s 向 0 缓慢衰减；恢复后要求 4 块/1024 share 的 warm-up。发送端用 `Δtx->seq` 判断；接收端用“最近一次放置 share 的时间”。

### 3.4 区分路径丢包 / 应用空闲 / carrier stall

| 情况 | 检测 | 动作 |
|---|---|---|
| 应用空闲 | 发送端 `tx->seq` 不变 >200 ms；`tp->app_limited` 全 carrier | 冻结 p；不切换；不做 down-probe |
| carrier stall（发送端） | `c->stalled`，或 `icsk_retransmits` 高且 `snd_una` 不动 | 该 carrier 的 share 从 p 分子分母剔除；>10% carrier stall → 冻结 |
| carrier 死（接收端） | `last_rx_ns` 老化 >max(4×RTT,1 s)，或 `!up/stopping`/非 ESTABLISHED | 剔除；alive<90% → 冻结 |
| TCP 隐藏的丢失 | `Δlost_out/Δsegs_out` >3% 持续 1 s，而 p_fec 低 | 快速提示：允许提前升一类；等 p_fec 确认 |
| replay ring 不足 | `fec_replay_miss_evicted` 速率 >1% | 不是 p；保持形状，检查 K/credit |
| 协议/形状错误 | `fec_proto_fail`、`fec_epoch_bad` 持续 | 冻结自适应，告警 |

### 3.5 FBK 上报

接收端 `w_fec_rx` 每 100 ms（有数据活动时）发一条 `FEC_FBK`，走控制队列（`tbt_fec_send_record(..., control=true)`，`tbt_fec_data.c:1055-1058`）。字段见 §5.1。发送端收到后更新 `tx->peer_p`、`tx->peer_valid`，唤醒 adapt work。

发送端自己的回退：50 ms poll 里按 carrier 采样 `Δlost_out/Δsegs_out` 与 `Δicsk_retransmits`，1 s 聚合为 `p_tcp`；仅当 `peer_valid==false`（>500 ms 无 FBK）时用 `p_tcp` 做开环建议，且不发起切换（没有 CFG_ACK 就无法确认）。

---

## 4. n/m 控制律

### 4.1 候选与示例表

候选形状：`n+m ∈ {8,16,32,64,128,256}`，`groups=256/(n+m)` 必须整除 256，`m≥1`、`n≥1`。0.3.0 启用 `n+m=8`；Phase 2 启用其余。

| 丢包 p（DATA share） | 类 | n | m | groups | 冗余 | 说明 |
|---|---:|---:|---:|---:|---:|---|
| 0–1% | C0 | 7 | 1 | 32 | 12.5% | 低损，最大化 app |
| 1–3% | C1 | 6 | 2 | 32 | 25% | |
| 3–5% | C2 | 5 | 3 | 32 | 37.5% | |
| 5–10% | C3 | 4 | 4 | 32 | 50% | 与固定 n4m4 相同 |
| >10% | C4 | 3 | 5 | 32 | 62.5% | |
| >20% | C5 | 2 | 6 | 32 | 75% | 可选；再高应怀疑路径不可用 |

Phase 2 可选行：`p<0.2% → 15/1, n+m=16, groups=16, 6.25%`；`p>20% → 16/16, n+m=32, groups=8, 50%`（大块降低尾部失败率）。

`[计算]` 相对固定 n4m4 的 wire-limited app 上限：C0 1.75×、C1 1.50×、C2 1.25×、C3 1.00×、C4 0.75×、C5 0.50×。实际要扣掉修复开销和 TCP 行为；验收看端到端 goodput。

### 4.2 滞回、冷却、驻留

- 评估周期 250 ms。
- 升类：`p > 上边界 + 0.5 pp` 连续 3 次（0.75 s），且距上次切换 ≥3 s。
- 降类：`p < 下边界 - 0.5 pp` 连续 6 次（1.5 s），且距上次切换 ≥10 s。
- 反向冷却：切换后 5 s 内不允许反向。
- 速率限制：≤1 次/3 s，≤10 次/60 s；30 s 内 3 次 → 冻结 30 s（`fec_adapt_hold`）。
- 抖动惩罚：10 s 内 A→B→A 发生一次，下一次驻留时间 ×2（上限 30 s）。
- 切换期间（`state != STEADY`）不评估。

### 4.3 残差反馈与 down-probe

静态表是“上限”；codec 非 MDS、突发丢失会让实际块失败率高于二项下界。用实测残差修正：

- `r_fail = Δfec_nack_req_shares / Δtx->seq`（1 s；发送端收到的 NACK 命名 share 数 / 发出的 DATA packet 数；重复请求会高估，NACK 扫描的 in-flight 过滤已降低重复）。
- `r_fail > 1.5%` 连续 2 次 → 升一类（m+1），并设 `no_probe_until = now+30 s`。
- `r_fail < 0.3%` 且当前类稳定 15 s、`no_probe_until` 已过 → 允许 down-probe 一类（m-1），观察 10 s；若 `r_fail > 1.5%`，立即回退并设该类的 `no_probe_until = now+60 s`。
- down-probe 是 5% 丢包时拿到 >5% app 增益的关键：静态表在 5% 给 C3（与固定 n4m4 相同），probe 到 C2（37.5%）可多 8.3% 的 wire 预算；如果实测 `r_fail` 超限则回退，保证不回归。

### 4.4 控制伪代码

```
every 250ms:
  if (!sess->adaptive || switching || !stream_active) return
  if (now - last_eval < 250ms) return
  if (p_valid == false || samples < 4 || sent_win < 1024) return
  p = max(p_ewma_ppm, p_win_ppm)
  r = r_fail_1s
  target = class_of(p)                 // 名义表
  if (r > 15000) target = min(target+1, 5)
  if (r < 3000 && stable > 15s && now > no_probe_until)
      target = max(target-1, 0)        // probe down
  if (target > cur) {
      if (p <= up_threshold(cur) + 5000) return
      if (++up_count < 3) return
  } else if (target < cur) {
      if (p >= down_threshold(cur) - 5000) return
      if (++down_count < 6) return
  } else { up_count=down_count=0; return }
  if (!dwell_ok() || !cooldown_ok() || !rate_ok()) return
  switch_begin(target)
```

`class_of` 用名义边界 1/3/5/10/20%；`up_threshold(cur)` 是当前类的上边界，`down_threshold(cur)` 是下边界。所有比较用 ppm 整数。

### 4.5 初始、配置与覆盖

- 配置新增 `adaptive=on|off`（默认 `on` 当 `fec=on`）。初始类由配置的 `n/m` 决定；若不在表内，用 C3（n4m4）作安全起点。
- `adaptive=on` 时，`cfg.w` 被忽略，每个形状用 `w=2n,d=n`（`tbt_fec_data.c:4613-4623` 的规则），避免用户把 `w=8` 固定后 n 变化导致 codec 窗口不一致。
- 运行期动作：`session <id> adaptive=on|off`（复用 `tbt_parse.c:792-813` 的 session 动作解析和 `tbt_session.c:550-627` 的 apply），不重建 session。调试可选：`session <id> fec n=<n> m=<m>` 强制一次切换（需 `debug>=1`），用于验收脚本，不进入 canonical config。
- `/proc/net/tbtun/stats` 新增 `c13 adaptive` 行（插在 `tbt_procfs.c:1229-1230`，c12 之后、c8 之前），不改 `TBT_TUNNELS_COLUMNS`/`TBT_PEERS_COLUMNS`（host 测试钉住格式）。字段：
  `epoch n m groups class p_ppm p_win_ppm sent miss switches aborts rollbacks state reason`。

---

## 5. 两端同步切换

### 5.1 Wire 变更

- `TBT_FEC_CONV 0x1005 → 0x1006`（`tbt_uapi.h:113`）；`TBT_WIRE_VERSION_V6 2 → 3`（`tbt_uapi.h:73`）。旧端在 HELLO 版本检查被拒（`tbt_rx.c:680-684`），这是仓库既有的“两端一起升级”策略。
- 不新增帧类型；复用 `FEC_CFG(0x34)`/`FEC_FBK(0x35)`。`TBT_FEC_CFG_LEN 32 → 48`（`tbt_uapi.h:200`）；`FBK` 保持 48。
- CFG body（48 B，little-endian）：

```
+0  u32 epoch          // 配置代次，单调 +1，从 1 开始
+4  u16 n
+6  u16 m
+8  u16 w              // 接收端按 w=2n,d=n 校验/忽略
+10 u16 k              // = n
+12 u8  d
+13 u8  flags          // bit0 APPLY, bit1 ABORT
+14 u16 groups         // 256/(n+m)
+16 u64 switch_pkt     // 第一条新形状记录的绝对 packet seq = P0
+24 u64 block_base     // 第一条新形状 block 的 block_id = B0
+32 u64 seed           // codec 种子；0.3.0 固定 0x5a5a...，接收端校验相等
+40 u32 crc32c         // 覆盖 +0..+39
+44 u32 reserved       // 0
```

- FBK body（48 B）：

```
+0  u16 epoch_lo
+2  u8  m_current      // 接收端当前 RX m
+3  u8  m_target       // 接收端建议（advisory）
+4  u32 p_ewma_ppm
+8  u16 expected_shares// 窗口内 alive-filtered DATA share
+10 u16 received_shares// expected - miss
+12 u16 epoch_hi
+14 u8  flags          // bit0 CFG_ACK, bit1 CFG_NACK, bit2 SWITCH_DONE,
                       // bit3 ROLLBACK, bit4 P_VALID
+15 u8  n_current
+16 u8  carrier_bitmap[32] // 窗口内至少送过一条记录的 carrier
```

- 控制记录头：`seq=0`、`block_id=0`、`n=m=0`、`share_index=0`、`win_len=0`；`tbt_fec_hdr_check()` 的 CFG 分支改为 48 B（`tbt_fec.c:304-306`），FBK 仍 48（`:307`）。
- 控制帧仍明文（与所有 FEC 控制帧一致，`tbt_crypto.h:5-20`），只有 header CRC32C。这是既有威胁模型；伪造 CFG 可强制切换（DoS），但没有新增 MAC 机制。风险清单中记录。
- HELLO 不新增字段；版本 3 本身即“支持自适应”的能力信号。`body[25]` 保留字节仍必须 0（`tbt_crypto.c:529-530`），不动 `tbt_crypto.c`。

### 5.2 边界与映射

发送端在决策时先关闭当前 block（对每个 group 发 parity/marker，`tbt_fec_tx_close_block()`，`tbt_fec_data.c:1347-1379`），然后：

```
P0 = base_pkt + (cur_block + 1 - base_block) * span_old   // span_old=groups_old*n_old
B0 = cur_block + 1
```

`P0` 是旧形状最后一个 super-block 的结束 packet seq，也是新形状第一条记录的 seq；`B0` 是新形状第一个 block_id。

新映射（发送端和接收端一致）：

```
rel        = seq - base_pkt
group      = rel % groups
local      = rel / groups
share      = local % n
block_id   = base_block + local / n
span       = groups * n
block_start= base_pkt + (block_id - base_block) * span
carrier    = group * (n + m) + share          // DATA
parity car = group * (n + m) + n + i          // PARITY
```

`base_pkt=0, base_block=0` 时与现有公式完全一致（`tbt_fec_data.c:1425-1430,4097-4104`）。新增 helper（`tbt_fec.h:693-735` 旁）：

```
tbt_fec_group_of_seq_base(seq, base_pkt, groups)
tbt_fec_block_of_seq_base(seq, base_pkt, base_block, groups, n)
tbt_fec_seq_of_block_pkt_base(block_id, group, p, base_pkt, base_block, groups, n)
tbt_fec_final_block_of_total_base(total_pkts, base_pkt, base_block, groups, n)
```

替换点（全部列在 §6.1）：`tbt_fec_data.c:1425-1430, 2535, 2818, 4097-4104, 4153-4156, 4388-4426, 2255/2295/2391/2549/3082/3276`。

### 5.3 状态机

发送端（TX controller）：

```
STEADY --decision--> CFG_SENT
CFG_SENT --CFG_ACK--> DRAIN
CFG_SENT --timeout 500ms / CFG_NACK--> STEADY (abort, no state change)
DRAIN: packetizer 停在 seq>=P0；控制/重放继续
DRAIN --SWITCH_DONE && peer_pkts>=P0--> SWAP --> STEADY(new)
DRAIN --timeout 2s / carrier 全死--> ABORT --> COOLDOWN(5s) --> STEADY(old)
```

接收端（RX）：

```
STEADY --CFG--> PREP（w_fec_rx 分配新 codec）
PREP --ok--> ARMED（发 CFG_ACK）
PREP --alloc fail--> STEADY（发 CFG_NACK）
ARMED --frontier>=P0--> APPLIED（锁内换形状，发 SWITCH_DONE + 新 credit ACK）
APPLIED --第一条新形状记录--> STEADY(new)（释放旧 codec）
APPLIED --旧形状记录 seq>=P0 且未 commit--> ROLLBACK --> STEADY(old)
ARMED --ABORT--> STEADY(old)
```

`P0` 必须是旧形状 super-block 边界；`frontier` 用 `next_deliver`（绝对 packet 数）。接收端在 `tbt_fec_deliver_ready()` 完成 `next_super++`、`next_deliver` 更新后检查 `next_deliver >= pending.switch_pkt`，在 `w_fec_rx` 里执行 APPLIED（进程上下文，可分配/释放）。

### 5.4 切换时重置的状态

TX（`tbt_fec_tx`，`tbtun.h:645-719`）：

| 项 | 动作 | 位置 |
|---|---|---|
| `codec` | 换新 `tbt_fec_grouped_new(G,n',m',n',2n',S,n',nonce)`；旧 codec 延迟释放 | `tbt_fec_data.c:4759` |
| `n/m/groups` | 更新 | `tbtun.h:676` |
| `base_pkt/base_block` | `P0/B0` | 新增 |
| `cur_block/seq/fill` | `B0/P0/0` | `tbt_fec_data.c:1376-1378,1405` |
| `cur_block_sid/switch_*` | 清 | `tbt_fec_data.c:1632-1700` 模板 |
| `credit/credit_cap` | 重算 `tbt_fec_credit_packets`；ACK 用 cap | `:4733-4734,3774` |
| `replay` | 清 `block_gen`、更新 `r->n/m`；K 不变 | `tbt_fec.c:459-473` |
| `fec_outq[]/fec_out_replay` | 清空数据/重放；保留 ctl（CFG/FBK） | `tbt_fec_data.c:1683-1699` |
| `drain[]` | swap 前 cancel，swap 后重新 arm | `:4717-4726,1679-1680` |
| `peer_pkts` | 保持（绝对 frontier，≥P0） | `tbtun.h:665` |
| `st->tx_paused` | swap 后清，恢复 capture | `tbt_fec_data.c:1783-1788` |

RX（`tbt_fec_rx`，`tbtun.h:723-841`）：

| 项 | 动作 | 位置 |
|---|---|---|
| `codec` | 换新；旧 codec 保留到 commit | `tbt_fec_data.c:4686` |
| `n/m/groups/slots` | 更新（`slots` 在 256 carriers 下恒 8） | `tbtun.h:734-736` |
| `blk[].data/parity` | 同一 `(n+m)*S` 缓冲内重切：`data=base`，`parity=base+n'*S` | `tbtun.h:600-601` |
| `blk[]` 元数据 | 全部 reset（used/ready/block_id/sid/nreal/ndata/nparity/count_real/decoding/empty/win_len_set/short_len/first_gap_ns/miss_mask） | `tbt_fec_data.c:1927-1946` |
| `next_super/highest_block` | `B0` | `tbtun.h:737,807` |
| `next_deliver/deliver_bytes` | 不变（=P0 / 已交付字节） | `tbtun.h:738,743` |
| `base_pkt/base_block` | `P0/B0` | 新增 |
| `stream_sid/base/total_*` | 不变（不是流切换） | `tbtun.h:745-757` |
| `req_ns/req_block/nack` | 清 | `tbt_fec_data.c:4675-4680,4670` |
| `replay_k` | 256 carriers 下恒 7；用 helper 重算 | `:4667` |
| `progress_*/gap_*/probe_*/repair_*` | 清 | `tbtun.h:760-838` |
| `rec` scratch | 按最大 m（6）分配，切换不重分配 | `:4682` |
| `stream_span/prev shape` | 保留，供 FIN 校验 | 新增 |

### 5.5 不丢字节论证

- I1：应用字节按序分配 packet seq；`tx->seq` 单调；ring 持有未打包字节（`tbt_fec_data.c:1424,1824-1871`）。
- I2：接收端按 seq 递增交付；`next_deliver` 是已交付 packet 数，`deliver_bytes` 是精确字节数（`tbt_fec_data.c:2478-2860`）。
- I3：切换点 `P0` 是旧形状 super-block 边界；发送端在 `seq>=P0` 不再发旧形状 DATA，在 `seq<P0` 不发新形状 DATA。
- I4：接收端只在 `next_deliver==P0` 时应用新形状，所以 `<P0` 的每个 packet 都已按旧形状交付；第一条新形状记录是 `P0`。
- I5：切换后到达的 `seq<P0` 记录只能是已交付块的 replay/duplicate，丢弃；切换前不可能出现 `seq>=P0` 的新形状记录（发送端等 `SWITCH_DONE`）。
- I6：`base_pkt/base_block` 把 `seq>=P0` 双射到 `(block_id>=B0,group,share)`；两端用同一组基址和形状。
- 因此没有 seq 被跳过、重复或映射到两个块；切换只暂停“新 packet 的生成”，不暂停已缓冲数据的交付。暂停上界 = 一个 RTT + 修复时间 + 分配时间（<2 s），应用侧由 `st->tx_paused` 背压，不重置连接。

### 5.6 FIN、credit、NACK 的细节

- **FIN span**：FIN 体仍 16 B（`tbt_uapi.h:265-274`），`pkt_base` 是流首 packet。流可能跨多次切换，所以校验必须用“流起始时的 span”，不是当前 `groups*n`。发送端在 `tbt_stream` 增 `u32 fec_pkt_span`（首 packet 时记 `groups*n`）；接收端在 `tbt_fec_rx` 增 `u32 stream_span`（首次绑定流时记当前 span），并保留一份 `prev_base_pkt/prev_span` 处理 FIN 先于 DATA 到达且流起始在上一形状的情况。`tbt_fec_rx_fin()` 的 `span`（`:4388`）和 `base=pkt_base/span`（`:4420-4426`）改用 `stream_span`/prev。
- **credit**：ACK 的 `credit_packets` 用新形状重算（`tbt_fec_data.c:2088-2089`）；APPLIED 后立即发一条 ACK，不等 cadence。发送端在 SWAP 时重算 `tx->credit`/`tx->credit_cap`；因为停在 `P0` 且无新在途，credit 突变安全。
- **NACK**：接收端 APPLIED 后清 NACK 状态，只扫描 `block_id>=B0`；发送端 NACK 处理器若收到 `block_id<B0` 的请求，丢弃并计 `fec_nack_stale_epoch`（旧 ring 已在 SWAP 清空）。切换前（DRAIN）旧 NACK 仍由旧 ring 服务，不能提前清。
- **stall**：发送端 `tbt_tx` 的 stall 检查（`tbt_tx.c:1906-1981`）在 `switch_pending` 时抑制；接收端 30 s stall 远大于切换 3 s 超时，APPLIED 时重置 `progress_ns`。
- **FIN/close 期间不切换**：若 `st->fec_finished` 或 `sess->closing`，推迟决策；避免 FIN 冻结的 `pkt_end` 跨形状。

### 5.7 失败与回退

| 失败 | 检测 | 动作 |
|---|---|---|
| CFG 丢失 | 500 ms 无 CFG_ACK | 重发 3 次（每 50 ms，3 个 carrier）；仍无 → abort，状态未变 |
| 接收端分配失败 | FBK `CFG_NACK` | 发送端 abort；接收端保持旧形状 |
| frontier 不到 P0 | DRAIN 2 s 超时 | 发送端发 ABORT，恢复旧形状从 P0 继续；接收端未 APPLIED 则清 pending |
| SWITCH_DONE 丢失 | 接收端每 100 ms 重发，直到首条新形状记录或 2 s | 发送端收到任一份即推进；若已 abort，接收端见旧形状记录回滚 |
| APPLIED 后发送端 abort | 旧形状记录 `seq>=P0` 且未 commit | 回滚到旧形状，计数 `fec_switch_rollback` |
| carrier 全死 | `tbt_fec_any_tcp_alive()` 假 | abort；走既有 stall/reset 路径 |
| epoch 回绕 | u32，会话生命周期内不现实 | 比较用无符号；忽略 `epoch<=cur` |

---

## 6. 代码位置与最小实现路径

### 6.1 逐文件改动清单

**wire/UAPI**
- `tbtun/tbt_uapi.h:73` `TBT_WIRE_VERSION_V6=3`；`:113` `TBT_FEC_CONV=0x1006`；`:200` `TBT_FEC_CFG_LEN=48`；注释更新 `:85-143`（CFG/FBK 语义）。不新增帧类型（`:146-151` 不变）。
- `tbtun/tbt_fec.h:210-234` CFG/FBK 结构体字段；`:593-597` pack/unpack 声明；`:693-735` 新增 base-aware helper；`:105-113` FIN 注释改为“流起始 span”。
- `tbtun/tbt_fec.c:716-799` pack/unpack 偏移/CRC；`:304-306` CFG 长度；`:1446-1528` 用新类表替换 `m_for_p` 阈值（保留 `params_*` 给 Phase 2/host 测试，不接线）。
- `tbtun/tbt_proto.c:71-76` 只改注释（FIN 体不变）；`TBT_CONV` 不变。

**FEC 数据面**
- `tbt_fec_data.c:1411-1505` packetizer：映射改 base；`seq>=P0` 暂停；header `n/m` 用当前形状；块缓冲切片。
- `:1212-1279` parity：`share_index=n+i`、`seq` helper base-aware、carrier 公式。
- `:1276-1328` empty marker：`seq` helper。
- `:1347-1379` close block：`tx->seq` 跳变改 `base_pkt + (block_id+1-base_block)*span`。
- `:1391-1408` finish：`block_start` base-aware。
- `:1592-1795` tx work：DRAIN 检查、SWAP 执行、队列清理、`st->tx_paused`。
- `:1515-1533` credit ok：加 `switch_pending` 暂停。
- `:3554-4370` rx record：CFG/FBK 分支（替换 `:4085-4086` 的 ignore）；形状计算移进 `rx->lock`（`:4094-4104`）；不匹配丢弃而非 `-EPROTO`；NACK/ACK 分支。
- `:1998-2066` blk decode：恢复槽位图。
- `:3426-3478` decode one：写回恢复槽时置 miss 位。
- `:2974-3423` NACK scan：请求 DATA 时置 miss 位；APPLIED 后只扫 `>=B0`。
- `:2478-2860` deliver ready：聚合 miss/real；`next_deliver>=P0` 触发 APPLIED；`base`/`next_deliver` 公式。
- `:4373-4458` rx fin：`stream_span`/prev。
- `:2309-2377` stream reset：保留 `base_*`（形状是 session 级，不是流级）。
- `:4499-4558` free：释放新/旧 codec、max_m scratch。
- `:4560-4815` init：shape-invariant 块缓冲、max_m scratch、`credit_cap`、`base_*=0`、adaptive 初始类。
- `:239-343, 893-1062` out queue/flush：CFG/FBK 发送、SWAP 清队列。
- `:2068-2110` ack：新 credit。

**session/stream**
- `tbtun/tbtun.h:908-917` 加 `TBT_SW_FEC_ADAPT`；`:645-719` tx 加 `base_pkt/base_block/shape_lock/credit_cap/switch_*`；`:723-841` rx 加 `base_pkt/base_block/stream_span/prev_*/est_*`；`:582-619` blk 加 `miss_mask`/`judged`；`:1218-1366` 新计数。
- `tbt_session.c:1160-1180` 分发新 work；`:1209-1258` arm/cancel；`:1306-1307` 不变（carrier 数不变）；`:1412-1426` init；`:1445-1566` destroy 取消新 work；`:550-627` `adaptive=` 动作。
- `tbt_tx.c:1906-1981` stall 抑制；`:357-430` FIN 期间推迟切换；`:3712-3715` 保持旧 FBK 禁用（FEC FBK 是另一条路径）。
- `tbt_sock.c:120-183` 背压：切换暂停时 ring 满会置 `tx_paused`，swap 后清除；`:692-733` 流启动/barrier 不变。
- `tbt_carrier.c:3361-3390` HELLO 版本 3；`:1002-1005` 确保形状不匹配不再走 `-EPROTO`；`:921-978` poll 采样 `lost_out/retrans`、alive 掩码；`:1922-1943` 速率 EWMA 复用。

**配置/procfs**
- `tbt_parse.h:46-103` 加 `u8 adaptive`；`tbt_parse.c:401-422` 默认；`:540-580` 解析 `adaptive`；`:636-683` 校验（`adaptive` 需 `fec=on`）；`:1029-1041` 渲染；`:792-813` session 动作加 `adaptive=`。
- `tbt_procfs.c:1229-1230` 加 c13 行；`:1050-1080` c10 可加 `fec_parity_miss`；`:1107-1142` c11 可加 switch 计数。不改列名常量（`tbt_uapi.h:370-377`）。
- `Makefile:16` 若新增 `tbt_fec_adapt.c` 则加源文件；`:69-73` host 测试加新向量。

### 6.2 Phase 1（0.3.0 可交付）

只启用 `n+m=8`；TX/RX 块缓冲改为 `(n+m)*S` 单缓冲切片；`rec` 按 max m=6 预分配；replay ring 不重分配，只清空/改 n/m；codec 每次切换分配（32 个 group，小）；不改变 carrier 数量/对象。

### 6.3 Phase 2（后续）

`n+m∈{16,32,64,128,256}`：重建 `tx->g[]`、`rx->blk[]`、`req_ns/req_block`、replay ring、`rec`、`slots/K/credit`；旧上下文保留到 commit 以支持回滚（内存约翻倍），或改成“frontier 后 stop-and-swap”放弃回滚。先统一 `n+m=256, groups=1` 的 `w/d` 语义（`tbt_fec_data.c:4613-4623` vs `tbt_fec.c:1467-1478`）。

### 6.4 实施顺序（最小风险）

1. Wire + host 测试：CFG/FBK 48 B、conv/version、pack/unpack、CRC、HELLO 版本拒绝；`host/test_fec.c:765-860` 更新。
2. base 映射机械替换：`base_pkt=base_block=0`，行为不变；host 测试加 `base!=0` 的 helper 向量。
3. shape-invariant 存储：`(n+m)*S` 切片、max_m scratch、codec swap、replay 清空；不接控制律。
4. 接收端估计器 + FBK：miss 位图、窗口/EWMA、alive 过滤、100 ms 上报；用 `drop_ppm` 验证 p 精度。
5. 发送端控制律 + CFG 发送/重试：adapt work、类表、滞回/dwell/probe；先不真正 swap（只发 CFG 并记录），验证协议。
6. 接收端 APPLIED/回滚 + 发送端 SWAP：打开真实切换；先 idle/单流短测，再 60 s。
7. 鲁棒性：形状不匹配丢弃、stale NACK、FIN span、credit、stall 抑制、队列清理。
8. 控制面：`adaptive=`、c13、运行时开关。
9. 验收矩阵。

### 6.5 接口契约

```
struct tbt_fec_shape { u16 n, m, groups, w, k, d; };
struct tbt_fec_switch {
    u32 epoch;
    u64 switch_pkt, block_base;
    struct tbt_fec_shape shape;
    u8 state, flags;
};

int  tbt_fec_switch_begin(struct tbt_session *sess, struct tbt_fec_shape shape);
void tbt_fec_switch_on_cfg(struct tbt_session *sess, const struct tbt_fec_cfg *cfg);
void tbt_fec_switch_on_fbk(struct tbt_session *sess, const struct tbt_fec_fbk *fbk);
void tbt_fec_switch_on_frontier(struct tbt_session *sess, u64 next_deliver);

/* estimator, called only from w_fec_rx */
void tbt_fec_est_recovered(struct tbt_session *sess, u16 group, struct tbt_fec_blk *b);
void tbt_fec_est_requested(struct tbt_session *sess, u16 group, u64 block_id, u16 share);
void tbt_fec_est_super_done(struct tbt_session *sess, u64 super);
```

---

## 7. 验收

### 7.1 环境与方法

- 双端 0.3.0，`fec=on crypto=on profile=http-kal`，`n=4 m=4 w=8 carriers=256` 作为初始/基线。
- **drop_ppm**（首选，隔离 FEC）：`debug_loss=1`，对所有 carrier 写 `carrier <i> drop_ppm <ppm>`（`tbt_fec_data.c:551-557`）；记录在写 socket 前丢弃，TCP 不重传，只有 parity/NACK 修复。用于验证 p 估计和控制律。
- **netem**（端到端）：`tc qdisc add dev <if> root netem loss 5% 25%`（均匀/突发）、`reorder 1% 50%`、`delay 20ms`；两个方向分别注入。TCP 会重传，p_fec 会低于 netem 配置值，这是正常的（FEC 只修 TCP 来不及的 share）；netem 用来看 goodput/字节完整性。
- 矩阵：loss {0,1,3,5,10}% × carriers {32,64,256} × 60 s 稳态（10 s warm-up）× 3 次；另加 5% 突发、reorder、RTT 20/100 ms。
- 基线：固定 n4m4；上限参考：固定 n7m1（0/1%）。
- 字节完整性：1 GiB 文件 `sha256sum`；或 iperf3 + 应用层校验。
- 记录：`/proc/net/tbtun/{stats,peers,tunnels}`、`dmesg`、`ss -ti`、`/sys/class/net/<if>/statistics/tx_bytes`、`perf stat`/`top`。

### 7.2 指标

| 指标 | 来源 | 用途 |
|---|---|---|
| app goodput | iperf3 | 主指标 |
| wire/app | tx_bytes / app bytes | 冗余/重传成本 |
| TCP retrans | `ss -ti` / `icsk_retransmits` | 路径行为 |
| FEC recovered | `fec_decode_recovered` | 恢复率 |
| NACK req/sent | `fec_nack_req_shares` / `Δtx->seq` | `r_fail` |
| replay hit/miss | c11 | 修复健康度 |
| p_est vs 注入 | c13 `p_ppm` | 估计精度 |
| switch count/pause | c13 + 日志 | 抖动/中断 |
| resets | `fec_reset_*`、`holes_unrecoverable` | 稳定性 |
| CPU | `perf stat`/`top` | 开销 |
| 字节校验 | sha256 | 正确性 |

### 7.3 成功标准（建议值，验收时可按机器调整）

- S1：所有 run 字节校验一致，无 `fec_reset_hole`/`holes_unrecoverable`/stream kill。
- S2：drop_ppm 下 p_est 误差 ≤±1.5 pp（1/3/5/10%，N≥1024）；netem 下 p_est ≤ 配置 loss（TCP 隐藏部分）。
- S3：0% loss，app ≥ 固定 n4m4 +60%。
- S4：1% loss，app ≥ +40%。
- S5：3% loss，app ≥ +20%。
- S6：5% loss，app ≥ 固定 -2%；若 down-probe 稳定（`r_fail<1.5%`），目标 ≥ +5%。
- S7：10% loss，app ≥ 固定 -3%；恢复率 ≥ 固定。
- S8：60 s 稳态切换 ≤3 次；10 s 内 A→B→A 不超过 1 次；切换最大应用停顿 ≤500 ms；无 carrier 关闭。
- S9：256 carriers CPU ≤ 固定 +5%。
- S10：32/64 carriers 同样通过，无回归（注意 K/slots/credit 不同，表按 helper 重算）。
- S11：netem 5% 突发，app ≥ 固定 -5%，无字节丢失。
- S12：切换过程中 kill 一个 carrier：无字节丢失；切换 abort 或完成；无 stream reset。

### 7.4 对比与记录

- 每组 3 次，交替 fixed/adaptive 运行，报中位数与 min/max；沿用 `benches/results/<UTC>/` 与 `benches/short.sh` 的约定。
- 单独记录一次“强制切换”序列（0%→5%→0%，或调试动作 `fec n=.. m=..`）的 epoch/state 日志与最大停顿，作为切换正确性的证据。

---

## 8. 风险清单

| # | 风险 | 证据 | 影响 | 缓解 | 验证 |
|---|---|---|---|---|---|
| R1 | codec band 矩阵非 MDS，m 个 parity 不保证恢复 m 个丢包 | `tbt_fec.c:850-959`；`tbt_fec_rlnc_recover()` rank 不足即 STUCK | 表偏低，r_fail 升高 | 残差反馈升类；NACK 兜底；不宣称 MDS | drop_ppm 下测每类 r_fail |
| R2 | 估计器只统计“需要修复”的 share，可能低估 | `miss=recovered∪requested` | 低损时 m 偏小 | 用 netem 对照；`sample_unknown`/`abandoned` 比例 >1% 冻结 | S2 + 日志 |
| R3 | carrier stall 被当路径丢包 | 发送端 `c->stalled`；接收端 `last_rx_ns` | m 偏高 | alive 过滤；>10% stall 冻结 | 注入 stall/down |
| R4 | 应用空闲时 p 无样本 | `tx->seq` 不动 | 误切换 | 200 ms 冻结；warm-up | 交互式流量测试 |
| R5 | 切换竞态：接收端 APPLIED、发送端 abort | §5.7 | 形状不一致 | `SWITCH_DONE` 门 + 旧 codec 保留 + 回滚规则 | 丢 CFG/FBK 注入 |
| R6 | CFG/FBK 丢失或承载 carrier 死亡 | 控制队列 | 切换卡住 | 50 ms 重发 ×3；2 s abort；控制轮转 | kill 承载 carrier |
| R7 | 形状不匹配走 `-EPROTO` 打死 carrier | `tbt_fec_data.c:4101-4104`，`tbt_carrier.c:1002-1005` | 连接抖动 | 改丢弃 + `fec_epoch_bad`；只对畸形记录用 `-EPROTO` | 伪造旧形状记录 |
| R8 | FIN `pkt_base` 跨形状对齐失败 | `tbt_fec.h:105-113`，`tbt_fec_data.c:4388` | FIN 被拒/流不复位 | `stream_span`/prev；FIN/close 期间不切换 | 长流跨多次切换 |
| R9 | credit 语义变化 | `tbt_fec.h:303-328`；`tbt_fec_data.c:2088,3774` | 发送超窗/停顿 | 停在 P0；APPLIED 立即 ACK；`credit_cap` 缓存 | 小 credit（C5）高 BDP |
| R10 | NACK 处理器与 TX swap 竞态 | `tbt_fec_data.c:3796-4083` | 误读位图/旧 ring | `tx->shape_lock`；`block_id<B0` 丢弃 | 切换时并发 NACK |
| R11 | 内存：Phase 2 双上下文 | arena 16 MiB + replay 15 MiB | OOM/延迟 | Phase 1 不重建；Phase 2 预分配/stop-and-swap；限制会话 | fault injection |
| R12 | CPU：每 3 s 一次 codec 分配/32 group | `tbt_fec_grouped_new` | 抖动 | 最小驻留；down-probe 有 no-probe 计时 | perf |
| R13 | `cfg.w` 固定导致 n 变化时 codec 窗口不一致 | `tbt_fec_data.c:4613-4618` | rank 变差 | adaptive 时忽略 `cfg.w`，用 2n/d=n | host 向量 |
| R14 | `params_dynamic` 与 init 的 w/d 不一致 | `tbt_fec.c:1467-1478` vs `tbt_fec_data.c:4613-4623` | Phase 2 直接接线会错 | 不接线；先统一语义 | host 测试 |
| R15 | 统计跨形状不可比 | 累计 atomic64 | 误判回归 | c13 按 epoch 重置窗口；保留累计但标注 | 验收脚本 |
| R16 | 控制帧明文、仅 CRC | `tbt_crypto.h:5-20` | 伪造 CFG 强制切换（DoS） | 既有威胁模型；限速；后续可加 MAC | 安全评审 |
| R17 | 旧端不兼容 | conv/version bump | 混部失败 | 明确“两端一起升级”；HELLO 拒绝 | 0.2.2 互操作测试（应拒绝） |
| R18 | 单流切换时 FIN 与暂停交互 | `tbt_fec_tx_finish` `:1391-1408` | FIN 丢/流不复位 | FIN/close 期间不切换；swap 后清 `tx_paused` | 短流/长流 |
| R19 | 32/64 carriers 的 K/slots/credit 不同 | `tbt_fec.h:260-264` 注释 | 表值不适用 | 全部用 helper 重算；验收覆盖 | S10 |
| R20 | `n+m=256` 单 group 的 `w=n` 语义 | `tbt_fec_data.c:4613-4618` | codec 行为不同 | Phase 2 先统一 | host 向量 |

---

## 9. 任务分解与并行计划

| WP | 范围 | 文件 | 依赖 | 可并行 |
|---|---|---|---|---|
| WP0 | 接口冻结：`tbt_fec_shape`、`tbt_fec_switch`、CFG/FBK 布局、estimator API | `tbt_fec.h`、`tbt_uapi.h`、本文件 | — | 先做，1 人 |
| WP1 | wire + host 测试：conv/version、CFG 48 B、FBK 布局、pack/unpack、CRC、HELLO 拒绝 | `tbt_uapi.h`、`tbt_fec.h/c`、`host/test_fec.c` | WP0 | 是 |
| WP2 | base 映射：helper + 全部公式点，`base=0` 行为不变；host 向量 | `tbt_fec.h`、`tbt_fec_data.c` | WP0 | 与 WP1 并行（不同文件为主） |
| WP3 | shape-invariant 存储：`(n+m)*S` 切片、max_m scratch、codec swap、replay 清空 | `tbtun.h`、`tbt_fec_data.c` init/free | WP0 | 与 WP1/2 并行，但 `tbt_fec_data.c` 需串行 |
| WP4 | 接收端估计器：miss 位图、窗口/EWMA、alive 过滤、FBK 生成 | `tbt_fec_data.c` rx 路径、`tbtun.h` | WP2/3 | 与 WP5 可并行（不同函数） |
| WP5 | 发送端控制律：adapt work、类表、滞回/probe、CFG 发送/重试 | `tbt_session.c`、`tbt_fec_data.c` tx 路径 | WP4 接口 | 与 WP4 并行 |
| WP6 | 接收端切换：CFG/FBK 处理、PREP/ARMED/APPLIED/ROLLBACK、锁内换形状 | `tbt_fec_data.c` rx 路径 | WP4/5 | 与 WP7 并行（需接口） |
| WP7 | 发送端切换：DRAIN/SWAP、credit/queue、ACK 门 | `tbt_fec_data.c` tx 路径 | WP5 | 与 WP6 并行 |
| WP8 | 鲁棒性：形状不匹配丢弃、stale NACK、FIN span、stall 抑制 | `tbt_fec_data.c`、`tbt_tx.c` | WP6/7 | 后置 |
| WP9 | 控制面：`adaptive=`、c13、运行时开关 | `tbt_parse.c/h`、`tbt_procfs.c`、`tbt_session.c` | WP5 | 独立 |
| WP10 | 验收：host 测试、drop_ppm/netem 脚本、矩阵 | `host/`、`benches/` | WP1-9 | 最后 |

并行约束：`tbt_fec_data.c` 是热点文件，WP2/3/4/5/6/7/8 都碰它。建议：
- WP1（`tbt_fec.c/h` + host）与 WP9（parse/procfs）独立并行；
- `tbt_fec_data.c` 设一个集成 owner，按 §6.4 顺序落地；其他 agent 只提交小 patch/测试，不并发改同一文件；
- 或者把估计器/控制律纯逻辑放进新 `tbt_fec_adapt.c`（host 可测），`tbt_fec_data.c` 只留 hook，减少冲突；Makefile:16 加源文件。
- 接口在 WP0 冻结后，WP4/5 可先写 host 单测（类表、滞回、窗口、base 映射），不阻塞内核数据面。

关键路径：WP0 → WP1/WP2 → WP3 → WP4 → WP5 → WP6/WP7 → WP8 → WP10。WP9 可全程并行。

---

## 10. 决策记录与未决问题

### 已定

1. **Phase 1 固定 `n+m=8`**：carrier 映射不变、内存不重建、风险最小；`n+m` 变化进 Phase 2。
2. **接收端测量、发送端决策/提交**：发送端拥有 block 边界和 TX 状态；接收端通过 FBK 报告 p 和建议 m。
3. **p 只统计 DATA share**：parity 丢失单独诊断；控制目标是 DATA 修复率。
4. **p = recovered ∪ NACK-requested**：不依赖 late/dup；两个事件都在 `w_fec_rx` 内，单写者。
5. **`base_pkt/base_block` 相对坐标**：`base=0` 保持现行为；切换只改基址。
6. **两阶段提交 + 旧 codec 保留到 commit**：支持 abort 回滚；Phase 1 内存代价小。
7. **RX 形状字段用 `rx->lock` 保护**（把计算移进锁内），不引入 seqlock；TX 形状用新 `shape_lock`，ACK 用 `credit_cap` 缓存。
8. **`adaptive` 默认 on（fec=on）**，运行时可关；`cfg.w` 在 adaptive 下被 2n 覆盖。
9. **不接 `tbt_fec_params_dynamic()`**：其 SINGLE 分支与 init 的 w/d 不一致；用新类表。

### 未决（需要后续确认）

1. `adaptive` 默认是否应为 off（0.3.0 首版保守）？本设计建议 on，但保留配置/运行时开关，验收可先 off 跑基线。
2. Phase 2 的 `n+m=16/32` 是否进入 0.3.0 的验收范围？本设计建议不进，先把 `n+m=8` 的切换/回滚/字节完整性跑稳。
3. down-probe 的 `r_fail` 阈值（1.5%/0.3%）需要 drop_ppm 实测校准；首版可先用 2%/0.5%，避免过早 probe。
4. 是否给控制帧加 MAC/认证？当前威胁模型没有；若 0.3.0 要防伪造 CFG，需要单独设计，不在本方案范围。
5. `n+m=256` 单 group 的 `w/d` 语义统一（`tbt_fec_data.c:4613-4623` vs `tbt_fec.c:1467-1478`）是 Phase 2 的前置。

---

## 附：关键行号索引

- 形状定型：`tbt_fec_data.c:4564-4600,4657-4660,4686,4703,4714,4733-4749,4759,4791-4812`
- 映射：`tbt_fec_data.c:1425-1430,4097-4104`；helper `tbt_fec.h:693-735`
- 形状拒绝：`tbt_fec_data.c:4101-4104`；carrier 关闭 `tbt_carrier.c:1002-1005`
- CFG/FBK 忽略：`tbt_fec_data.c:4085-4086`；pack/unpack `tbt_fec.c:716-799`
- 动态参数：`tbt_fec.c:1446-1528`；host `host/test_fec.c:1680-1790`
- FIN/span：`tbt_fec.h:105-113`；`tbt_fec_data.c:4388-4426`
- credit：`tbt_fec.h:303-328`；`tbt_fec_data.c:2088-2089,3774`
- 交付/聚合：`tbt_fec_data.c:2478-2860`（`next_super++`/ACK `:2817-2857`）
- 解码/恢复：`tbt_fec_data.c:1998-2066,3426-3478`
- NACK：`tbt_fec_data.c:2974-3423,3796-4083`
- 流重置模板：`tbt_fec_data.c:1632-1700,2309-2377`
- 初始化/释放：`tbt_fec_data.c:4499-4558,4560-4815`
- 50 ms poll：`tbt_carrier.c:921-978`；速率 EWMA `:1922-1943`
- 旧 FBK 禁用：`tbt_tx.c:3712-3715`；`loss_ppm` `:1641-1647`
- 配置：`tbt_parse.h:46-103`；`tbt_parse.c:401-422,540-580,636-683,1029-1041`
- 统计：`tbtun.h:1218-1366`；`tbt_procfs.c:1050-1080,1107-1142,1229-1230`
- HELLO：`tbt_uapi.h:295-303`；`tbt_rx.c:674-712`；`tbt_carrier.c:3361-3390`
- `drop_ppm` FEC：`tbt_fec_data.c:551-557`
