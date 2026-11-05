# `retx_ppm` 与 `wire/app` 偏高：静态分析与实验设计

范围：DESIGN §19 Q7、§4.3、§4.6、§9.7；`交接.md` §7/§8/§9 P2-9。
只做静态分析与实验设计，**不改源码、不编译、不跑 QEMU**。行号取自当前工作区
（2026-11-05），`review/findings.md` 的状态表是 05:19 快照，行号已漂移。
`benches/` 正被并行修改，其中的行号只作定位提示；`tbtun/` 的行号以本文写作时为准。

---

## 0. 结论

1. **`retx_ppm` 的分子不是"丢包帧"，是"定时器排队的重传副本"；分母不是"数据帧"，是
   "该载体上写出的全部记录（含 FBK）"。** 两者口径不同，所以它既高估也低估，方向不固定。
2. 干净链路（`drop_ppm=0`、`holes_unrecoverable=0`、`strict=1`）上，**真实的数据帧重传率
   是 21%–52%**（n=8→n=2），n=1 的极端点是 97%（该行 `rc=3`，严格相位已失败）。
   而报告值是 12%–19%。报告值偏低的唯一原因是分母里塞进了 30%–51% 的 FBK 记录。
   **这不是"分母偏小"，恰恰是分母偏大掩盖了问题。**
3. 这些重传里**几乎全部是误重传**：字节最终都到了（strict 逐字节通过），只是到得比
   `rto` 晚。判据是"越拥塞越差"：n=1 → 97%，n=2 → 52%，n=4 → 29%，n=8 → 21%；
   而注入 1% 丢包反而让比例**下降**（198k→168k ppm）。真实丢包不会这样。
4. 门控之后仍有 **7 条**重发同一帧的路径（§3）。其中 R1（定时器）不受 `snd_una` 约束：
   `tbt_tx.c:468-469` 在"没有任何副本被 ack"时回退到本地写时间，这就是被回退掉的那条
   规则的残骸，它今天仍在线上运行。
5. 自激环有三个，主环是 **L1：`recv_lag` 一旦非零就永不清零**，从而永久废掉
   `tbt_retain_received()` 的"已收到"证明，让定时器对**每一条**写在该路径上的帧都
   可以开火。`recv_lag>0` 在干净链路上已由 `holes_declared=505`（n=8）反证成立。
6. 替换环的误杀点不在 stall 判据（它比 `交接.md` 写的更严），在**写错误分类**：
   `tbt_carrier_send()` 把正常背压的**短写**当 `-EIO`（`tbt_carrier.c:466`），
   pump 把任何非 `-EAGAIN` 当载体死亡（`tbt_tx.c:1451`）。`-ENOMEM` 同理。

---

## 1. 计数器语义

### 1.1 分子与分母

| 计数器 | 位置 | 什么时候加 | 统计对象 |
|---|---|---|---|
| `c->lost_frames` | `tbt_tx.c:576` | `tbt_tx_retx()` 把副本放进队列之后 | **仅定时器重传**，按 `r->last_path` 归因 |
| `c->tx_frames` | `tbt_carrier.c:691`（raw）、`:615`（http） | 记录写成功后，**无类型判断** | 该载体上写出的**全部**记录：DATA + HELLO/OPEN/FBK/PAD/FIN/RETIRE |
| `sess->retx_frames` / `retx_bytes` | `tbt_tx.c:569-570`、`:712-713` | 定时器副本 + 空洞注入副本 | 排队数，不是写出数 |
| `c->data_frames` | `tbt_carrier.c:521` | **`tbt_carrier_xmit()` 入口**，任何 `-EAGAIN` 之前 | DATA **写尝试**，不是写成功 |
| `c->recv_cnt` | `tbt_rx.c:272` | FBK 收到时由对端发布 | 对端**解析到的** DATA 记录数 |
| `c->recv_lag` | `tbt_tx.c:886` | 每次空洞检查重算 | `data_frames - tail - recv_cnt`，见 §4.1 |

`retx_ppm` 的渲染在 `tbt_session.c:390`：

```c
retx_ppm = frames ? (u32)((u64)READ_ONCE(c->lost_frames) * 1000000 / frames) : 0;
```

`frames` 是 `c->tx_frames`。bench 取的是 `peer_retx_max 2`（`benches/qemu/init:456`），
即**各载体 ppm 的最大值**，不是聚合值。所以它既不是"数据帧重传率"，也不是"全局重传率"，
而是"最差路径上，定时器重传数 / 该路径全部记录数"。

### 1.2 口径造成的两个偏差

**偏差 A：分母被控制帧稀释。** FBK 每 10 ms 一条，落在某一条载体上；数据记录受
128 KiB 发送窗口限制。用三条独立数据反解（§2），FBK 实测 **91.0 条/秒**，在分母中占
30%（n=8）到 51%（n=2）。所以报告 ppm **低于**真实数据帧重传率 1.7–3.1 倍。

**偏差 B：分子只认定时器。** `tbt_tx_inject_locked()`（`tbt_tx.c:688`）的重传副本加了
`sess->retx_frames`（`:712`）却**不碰** `lost_frames`。`dup` 副本同理。所以空洞修复
在 `retx_ppm` 里完全隐形。当前 `holes_declared` ≤ 512，注入副本 < 2% 的字节量，
暂不影响结论，但一旦按 §6 提高 dup 或修好 gate，这个口径就会误导。

**偏差 C：分子是"排队数"不是"写出数"。** 副本入队后可能被 pump 在
`tbt_tx.c:1360`（`fr->hdr.seq + fr->len <= tx_ack`）直接丢弃，从未上线，
但 `lost_frames` 已经加过。高 n 时这个虚增可达 17%–35%（§2 的 n=4/n=8 行）。

---

## 2. 定量对账：报告值 vs 真实值

数据来自 `benches/results/*-rec.csv` 与 `*-rec-carriers.csv`。三个量互相独立：

- `app_bytes` = 提交给隧道的应用字节（80 MiB，两个阶段各 40 MiB）；
- 唯一 DATA 帧数 `U = app_bytes / 8192`（`TBT_PAYLOAD_MAX`=8192，`tbt_uapi.h:90`；
  40 MiB 恰好整除）；
- `wire_bytes` = Σ`c->tx_bytes`，即每条记录的 `TBT_HDR_LEN(32) + len` 之和，
  **不含 TCP 层重传，不含反方向**。

设 `R` = 重传记录数、`C` = 控制记录数，则
`frames = U + R + C`、`excess = wire - app = R·8192 + C·(32+18n)`。
两个未知量、两个方程，可解。

| 行 | U | frames | 报告 ppm | 由字节反解 R | 由 ppm 反解 R | 差 | 反解 C | C/s | **真实数据帧重传率 R/U** | wire/app |
|---|---|---|---|---|---|---|---|---|---|---|
| n=1 | 5,120 | 28,802 | 172,071 | 4,803 | 4,956 | −3.1% | 18,876 | 90.8 | **96.8%**（该行 rc=3） | 1.98 |
| n=2 | 10,240 | 31,631 | 168,031 | 5,350 | 5,315 | +0.7% | 16,039 | 91.0 | **51.9%** | 1.57 |
| n=4 | 10,240 | 20,463 | 145,384 | 3,015 | 2,975 | +1.3% | 7,204 | 91.0 | **29.1%** | 1.28 |
| n=8 | 10,240 | 17,782 | 122,258 | 2,213 | 2,174 | +1.8% | 5,321 | 91.1 | **21.2%** | 1.21 |

（n=4/n=8 的"由 ppm 反解"用逐载体 `Σ ppm_i·frames_i`，不是 max×Σ；用 max 会虚高
17%/35%，那正是偏差 C。）

四条行反解出的 `C/s` 全部是 **91.0 ± 0.1**，与 `TBT_FBK_INTERVAL_MS=10` 的
100/s 减去调度损耗吻合。这同时证明了：记录大小就是 8192、`wire_bytes` 只含前向、
超额字节**全部**是重传载荷。三条独立量的误差 ≤ 3.1%。

### 2.1 读数

- **比例随载体数单调下降**：96.8% → 52% → 29% → 21%。若是真实丢包，链路条件相同，
  比例应与 n 无关。它随 n 下降只说明一件事：**每路径的排队深度**决定重传率。
- **干净链路比注入丢包更差**：`loss-0`（`drop_ppm=0`）是 197,660 ppm / wire-app 1.352；
  `loss-10000` 是 168,129 / 1.200。真实丢包**降低**了假重传，因为它降低了发送速率、
  削浅了队列。这是"超时短于排队时延"的教科书签名。
- **htb 溢出不在账上**：`wire_bytes` 在记录层计数（`c->tx_bytes`），发生在
  `tcp_sendmsg_locked` 返回之后；载体 TCP 自己的丢包/重传既不进 `wire_bytes`，
  也不进 `recv_lag`（一次写尝试对应一次最终到达，差值回零）。所以"也许是 htb 丢了包"
  解释不了这 25 MB 超额——它只可能来自模块自己多写的记录。
- **n=1 的 1.98 不是另一个 bug**：单载体时全部 40 MiB 挤一条 10 Mbit 流，队列最深，
  97% 的帧在 RTO 到期时还没被 ack，于是几乎每帧重发一次，`wire/app` 逼近 2。
  `交接.md` 把它记成"单载体不是可用配置"，其实它是同一机制的极端点。
  （该行 `rc=3`：严格相位在逐字节比对失败处结束，第二阶段的 40 MiB 从未提交，
  所以 `app_bytes` 是 40 MiB 而不是 80 MiB。用字节反解的 R 是 4,803，用 ppm 反解
  是 4,956，差 3.1%——行本身不完整，但两个独立量仍然互相印证。）

---

## 3. 重发同一帧的全部路径

按"是否会重复发送同一份载荷"穷举。R1–R7 之外的写路径（HELLO/OPEN/FIN/RST/PAD/CANCEL）
不重发载荷。

### R1 定时器重传 —— `tbt_tx_retx()`，`tbt_tx.c:430-578`

触发条件（`tbt_tx.c:452-527`）：

```
seq+len > tx_ack                      (:446)  仍在窗口内
sends > 0                             (:448)  写过至少一次
!tbt_retain_received(r)               (:454)  没有任何路径能证明收到
wire_ns = tbt_retain_wire_ns(...)     (:457)  某副本 snd_una 越过 first_sent_seq 的首次观测时刻
if (!wire_ns) wire_ns = last_sent_ns  (:468-469)  ★ 回退到本地写时间
!wire_ns && !retx_now → 跳过           (:470)
first = r                             (:472)  ★ 在超时判断之前锁存
pending 且 age < rto → 跳过            (:478)
!retx_now 且 age < rto → 跳过          (:480)
→ last_sent_ns = now; pending = true; cand[n++]   (:523-525)
```

★ 这一行就是被回退的 `snd_una` 门控的残骸：只要**没有任何副本**被 TCP ack，
RTO 就从本地写时刻起算。1 MiB 载体缓冲在 10 Mbit 上是 840 ms，而
`tbt_tx_rto_ns()`（`tbt_tx.c:371`）上限 250 ms。所以只要队列深于 250 ms，
**还没上线的帧就会被判超时**。

另外两点：
- 每轮最多把 8 个**不同**的帧重新入队（`:527`），不是"只发洞"；只有 `r == first`
  的那一个会宣告（`:489`），其余静默重传。
- `lost_frames` 按 `r->last_path` 归因（`:576`），而副本实际写在别的路径上。
  所以"路径 0 的 ppm"里混着"路径 0 写出的帧被重传到路径 1"的计数。

**计入 `retx_ppm`：是。受 `snd_una` 约束：否（有回退）。**

### R2 空洞注入 —— `tbt_tx_inject_locked()`，`tbt_tx.c:688-717`

触发：`tbt_tx_hole_check()`（`:809`）几何门控全部通过后，在 `:1002` 调用。
副本数 `d2 = dup + (g==1)`，至少 1，避开已用路径。

**计入 `retx_ppm`：否**（只加 `sess->retx_frames`，`:712`）。受门控：是。

### R3 路径死亡批量标记 —— `tbt_tx_path_lost()`，`tbt_tx.c:396-426`

`tbt_carrier_lost()` 调用它；对每个 `last_path == index`、未 ack 的保留帧置
`retx_now = true`（`:414-417`），并清掉该路径的 `path_mask` 位。这些帧随后走 R1，
**跳过 RTO 判断**（`:480` 的 `!r->retx_now` 短路）。所以一条路径死亡会立刻把它的全部
在途帧（≤128 KiB）倾泻到幸存路径上。

**计入 `retx_ppm`：是（经 R1）。受门控：否（`retx_now` 直通）。**

### R4 写错误后改路 —— pump，`tbt_tx.c:1444-1464`

`tbt_carrier_xmit()` 返回非 `-EAGAIN` 错误 → `tbt_carrier_lost()`（`:1451`）→
同一帧 `avoid_mask = BIT(index)` 重新入队（`:1462`）。**帧对象被复用，不是副本**，
但如果 `tcp_sendmsg_locked` 已写出前缀，那段字节已经上线，接收端丢弃半个记录
（parser 重置）——所以是浪费，不是重复投递。

**计入 `retx_ppm`：否。** 这是 `wire/app` 的一个未计量来源。

### R5 `-EAGAIN` 后改路 —— pump，`tbt_tx.c:1427-1441`

无字节写出（`part` 缓冲属于上一条记录），重新入队并 `avoid_mask = BIT(index)`。
严格说不是重发，但它让"同一 seq 的第一次写"发生在第二条路径上，而
`c->data_frames++`（`tbt_carrier.c:521`）已经在第一条路径上记过一次。
**这是 L1 自激环的燃料**（§4.1）。

**计入 `retx_ppm`：否。**

### R6 `dup` 副本 —— `tbt_tx_submit()`，`tbt_tx.c:1680-1712`

按 `cfg.dup` 复制，每份避开主副本路径。当前所有 bench 行 `dup_tx=0`、`dup=0`。

**计入 `retx_ppm`：否。** 计入 `wire/app`：是。

### R7 接收端重复帧去重之前的到达

同一帧的两份副本到达同一路径时，`tbt_rx_data()` 在 `tbt_rx.c:146` 先加
`rx_frames++`，去重发生在投递树里。所以 `recv_cnt` 计的是**到达**数，不是**去重**数。
这让 `recv_lag` 的公式在"同路径重复"时仍然平衡（见 §4.1），但也意味着
`recv_cnt` 不能当作"应用收到的帧数"。

**计入 `retx_ppm`：否。** 但它决定了 `recv_lag` 的语义。

---

## 4. 自激环

### 4.1 L1（主环）：`recv_lag` 永不清零 → "已收到"证明永久失效

`recv_lag` 的计算在 `tbt_tx.c:868-891`：

```c
tail        = 保留环里 end > uack、且 path_mask 含 i、gen 匹配的帧数;
sent_below  = c->data_frames - tail;
lag         = sent_below > c->recv_cnt ? sent_below - c->recv_cnt : 0;
WRITE_ONCE(c->recv_lag, lag);
```

`data_frames` 在 `tbt_carrier_xmit()` **入口**自增（`tbt_carrier.c:521`），
在 `drop_ppm` 分支之前、在任何 `tcp_sendmsg_locked` 之前。所以它数的是**写尝试**。
`recv_cnt` 数的是对端**解析到的记录**。

一次 `-EAGAIN`（或 `part` 冲刷失败，`tbt_carrier.c:653/655/664`）造成：

- 该路径 `data_frames += 1`，但对端永远收不到这条记录；
- 帧改走别的路径，`path_mask` 里**没有**这条路径的位，所以 `tail` 也不会把它减掉；
- 于是 `sent_below` 永远比 `recv_cnt` 大 1。

**这个差值不会自愈。** 后续即使同一 seq 再写回该路径，`data_frames` 和 `recv_cnt`
同时 +1，差值不变。只有换载体（新 `tbt_carrier` 对象，计数归零）才会清掉。

后果链：

1. `tbt_retain_received()`（`tbt_tx.c:590-618`）第一件事就是
   `if (READ_ONCE(c->recv_lag)) continue;`——**该路径的高水位从此不再能证明任何帧**。
2. 于是 R1 的 `!received` 条件对写在该路径上的每一帧都成立。
3. 同时 `tbt_tx_hole_check()` 的 `if (lag) lost = true;`（`:906-907`）让几何门控
   在干净链路上也判定"该路径确认丢包"，于是宣告空洞、注入副本。
4. 注入副本和定时器副本继续占用队列和窗口，加深排队，产生更多 `-EAGAIN`。

**已在数据中反证**：n=8 的干净链路 `holes_declared=505/512`。门控要宣告，
必须至少有一条已用路径 `lag > 0`（`:906`）。所以 `recv_lag>0` 在干净链路上**确实存在**。
n=4 的 6/13 次同理。n=2 的 `holes_declared=0` 不代表 lag=0，只代表门控的另一个
条件（其它路径 `uack >= hole->seq`）没过——定时器不依赖门控，照样重传 52%。

### 4.2 L2：`first` 锁存 → 只重传、不宣告

`tbt_tx.c:472-473` 在**任何**超时判断之前锁存 `first`：

```c
if (!first) first = r;      // 第一个"未收到"的帧，不一定超时
```

而宣告只在 `r == first` 时发生（`:489-505`）。所以：

- `first` 是"刚写过、还在 RTO 内"的帧 → 它 `continue`；
- 它后面的帧（seq 更大，但上次写时刻可能比 `first` 更早——例如 `first` 刚被
  重传过、时钟被刷新）走 `r != first` 分支，**只入队、不宣告、不记 `holes_declared`**；
- 即使 `r == first`，只要 `tbt_hole_copy_inflight()` 为真（还有副本在途），
  也会落到 `retx_queued` 只发不宣告（`:489-505`）。

这是 n=2 的 `holes_declared=0` 与 169k ppm 并存的两个候选解释之一（另一个是
门控本身没过，§4.1）。两者指向同一个操作结论：**"没有宣告"不能当作"没有重传"**。
这也解释了为什么 Q7 说"去掉误重传后暴露了替换循环"：36a 的 `snd_una` 只被用作
R1 的**时钟起点**，而且被 `:468-469` 的回退架空；它从来没有对 R1 构成硬门控。
真正被门控的是 R2（几何测试）。所以"门控上线"从来没有压住重传，只是改变了重传的
计时方式。

### 4.3 L3：替换环

先把 stall 判据本身说清楚，因为它**不是**主要嫌疑：

`tbt_car_poll_work()`（`tbt_carrier.c:202`）每 50 ms 跑一次：

```c
bool blocked = nxt == c->last_nxt && wmem > 0;        (:263)
if (una != c->last_una) { last_una = una; last_una_ns = now; }   (:265-268)
if (blocked) last_una_ns = now;                        (:271-273)
else if (last_una_ns && una != nxt &&
         now - last_una_ns > carrier_stall_ms) → tbt_carrier_lost()   (:275-284)
```

未 ack 的字节留在 `sk_wmem_queued` 里，所以 `wmem == 0` 基本蕴含 `una == nxt`。
因此 stall 真正能触发的形状只有一种：**`nxt` 在推进（我们的 TCP 有窗口、还在发）
而 `una` 连续 3 s 不动**——那是真黑洞，不是背压。反过来说，只要 `una` 每 3 s 动一次，
这条规则不会误杀。它比 `交接.md` 描述的"backpressure vs dead path"更严，
不太可能是 191 次替换的来源。**真正的误杀在写错误分类上。**

自激链：

1. pump 把**任何**非 `-EAGAIN` 的写错误升级成"载体死亡"
   （`tbt_tx.c:1444-1451`）。可达的非致命错误有两个：
   - `tbt_carrier_send()` 对**任何短写**返回 `-EIO`（`tbt_carrier.c:466`）。
     而 `tcp_sendmsg_locked` 写出前缀是**正常背压**，不是错误。http-kal 的
     头块和整帧都走这条路径。
   - HTTP 头 `kmalloc` 失败返回 `-ENOMEM`（`tbt_carrier.c:565`）。
     内存压力下这就是一条"载体死亡"。
2. `tbt_carrier_lost()`（`:745`）置 `stopping`、发 RETIRE（`:771`，只在还有别的
   活载体时发）、`tbt_tx_path_lost()` 把该路径**全部**在途帧标 `retx_now`、
   `join_retries++`、`schedule_work(&sess->w_replace)`。
3. `tbt_replace_work()`（`:903`）在 `pump_lock` 下换入新载体（同 index，gen+1），
   `carrier_replaced++`（`:948`），并在**释放旧 socket 之后**才拨号。
4. 新载体的 `HELLO(gen+1)` 到达对端时，若对端还持有旧的 `(index, gen)`
   （RETIRE 没到，或旧 socket 的关闭还没被对端看到），`tbt_session_join()`
   返回 `-EALREADY`，`session_join_dup++`（`tbt_session.c:813`）。RX 路径把这个
   当作"关掉这条载体，不关会话"（`tbt_carrier.c:375-381`），随后由 poll 再排一次
   `w_replace`，形成 50 ms 一轮的重试。

**为什么它会自激**：R3 的 `retx_now` 是**无条件**的——一条路径被判死，它全部在途帧
立刻倾泻到幸存路径，不经 RTO、不看窗口、没有 `sends` 上限。幸存路径的队列随即变深，
短写和内存压力更容易出现，于是又有一条路径"死亡"。同时 `recv_lag` 被 `-EAGAIN`
污染（L1），重传量进一步上升。

**收尾条件**：`join_retries` 只在 `tbt_carrier_lost` 里递增（`:782-784`），
只在对端为当前代回 HELLO 时清零（`tbt_rx.c:499`）。注意 `-EALREADY` 本身**不**
增加这个计数——它是 RX 路径的事件。所以计数衡量的是"连续多少次载体死亡没能换来
一次成功 join"，超过 20 就 `tbt_stream_kill(TBT_ST_DEAD)`。这正是"32 s 接收端空洞"
的收尾方式：不是某一帧丢了，是替换环把流自己杀掉了。

**当前代码里会自激的具体条件**（按嫌疑排序）：

1. 短写被当成 `-EIO`（`tbt_carrier.c:466`）→ pump 判死（`tbt_tx.c:1451`）；
2. `-ENOMEM` 被当成载体死亡（`tbt_carrier.c:565` + `tbt_tx.c:1451`）；
3. `tbt_tx_path_lost()` 的 `retx_now` 没有 `sends` 上限（`:414-417`）；
4. `join_retries` 的语义是"连续失败轮数"，但计数点是载体死亡而非 join 失败，
   与它想表达的东西差一层（`:782-784`）。

---

## 5. 判别实验矩阵

所有实验都用现有计数器，不改代码。判据写在"排除"列：读数落在哪一侧就排除哪个假设。
每条至少 3 次，取中位数；每次只改一个参数。

记号：`T` = `/proc/net/tbtun/tunnels` 行（`retx_frames`/`retx_bytes`/`holes_*`/
`gate_fail`/`depth_fail`/`time_fail`/`hole_checks`/`carrier_lost`/`carrier_replaced`/
`join_dup`/`carrier_short`/`carrier_err`）；`P` = `/proc/net/tbtun/peers`
（每路径 `uack`/`rx_lag_ppm`/`frames`/`bytes`/`retx_ppm`/`inflight`）。

| # | 改什么 | 观察 | 排除的假设 | 判据 |
|---|---|---|---|---|
| E1 | `rx_reorder_bytes` 262144 → 32768（窗口 128K→16K） | `retx_ppm`、`wire/app`、`rx_max_stall_us` | **H-queue**：重传由"队列深于 RTO"驱动 | 若 ppm 掉到 <50k 且吞吐不崩 → H-queue 成立；若 ppm 不变 → 定时器与排队无关，转 E3 |
| E2 | `rx_reorder_bytes` 262144 → 2097152（窗口 1 MiB） | 同上 | 反向验证 H-queue | 若 ppm 显著上升且 `carrier_short` 上升 → 队列深度是自变量，H-queue 成立 |
| E3 | `carrier_stall_ms` 3000 → 30000 | `carrier_lost`、`carrier_replaced`、`join_dup` | **H-replace**：替换环在驱动重传 | 若替换归零而 `retx_ppm` 不变 → 替换是症状不是病因（预期如此）；若 ppm 跟着掉 → 替换是主因 |
| E4 | `tunnel_rate` 0 → 4 Mbit（低于 4×10 Mbit 容量） | `retx_ppm`、`carrier_short`、`inflight` | **H-queue**（不靠窗口，靠限速削浅队列） | 若 ppm→~0 且 `carrier_short`→0 → 队列深度是唯一自变量 |
| E5 | `drop_ppm` 0 / 10000 / 50000（单载体） | `retx_ppm`、`holes_declared`、`holes_unrecoverable` | **H-loss**：高 ppm 是真实丢包 | 干净链路若已有 21%–52% 真实重传率，注入 1% 只能加 ~1%；实测 ppm 反而下降 → H-loss 排除（已由 CSV 支持，需复现确认） |
| E6 | n = 1/2/4/8，其余不变 | 真实重传率 `R/U`（§2 反解）、`carrier_short` | **H-queue** vs **H-ackpath** | 比例随 n 单调降（96.8→52→29→21%）→ 每路径排队深度；若平坦 → ack 路径（FBK 延迟） |
| E7 | `profile=raw` vs `http-kal` | `wire/app`、`retx_ppm` | **H-framing**：HTTP 容器开销解释 1.2–1.35 | 已有数据：http-kal 1.356 vs raw 1.404，ppm 177k vs 177k → 容器只值 ~1%，排除 |
| E8 | 不改模块；在 `benches/qemu/init` 里对 `tbt_burst` 的两个阶段各加一个 2 s 采样窗口（严格相位只发不读，交错相位边发边读），读 `T.retx_frames`、`P.rx_lag_ppm`、`peer_paused` 的增量 | 两阶段的 ppm 增量 | **H-backpressure**：交付背压被当作丢包 | 严格相位增量显著高于交错相位 → 背压是触发条件；直接指向 §6 的 F2 |

**必须一起读的三个量**（单独看会误判）：

- `Σ retx_ppm_i·frames_i`（聚合重传数）与 `T.retx_frames`：两者差 >10% 说明
  偏差 C（排队未写出）显著，`retx_ppm` 不可直接当重传率。
- `P.rx_lag_ppm > 0` 且 `T.carrier_short > 0` 且 `drop_ppm=0`：**L1 成立**。
  这是唯一能直接证实主环的读数组合。当前 CSV 没有这两列，`benches/qemu/init:474`
  只导出了 7/8/9/13/14/15/16 列，需要加列（见 §6 F5）。
- `T.gate_fail / hole_checks`：门控失败率。若接近 1，说明几何测试基本没工作，
  所有修复都来自定时器（R1），与 §4.2 一致。

---

## 6. 最小修复方案

目标：`retx_ppm < 50k`、`wire/app < 1.1`、n=4 ≥ 9.20 Mbit/s、不引入替换风暴。
顺序即依赖顺序；F1 单独就能拿掉一半以上的假重传。

### F1 修正 `data_frames` 的记账（1 行 + 1 处补偿）

- `tbt_carrier.c:521` 的 `c->data_frames++` 移到写成功处（`:691` / `:615`，与
  `tx_frames++` 同处）。
- 在 `drop_ppm` 分支（`tbt_carrier.c:525-545`）显式补一次 `c->data_frames++`，
  否则注入丢包失去 gate 证据（那正是它存在的目的）。
- 效果：`-EAGAIN` 不再永久污染 `recv_lag`；`tbt_retain_received()` 恢复；
  干净链路的 `holes_declared` 归零；R1 的 `!received` 条件恢复选择性。
- 风险：注入实验的 `holes_declared` 依赖这条补偿，漏了会把 1% 丢包实验变成"零空洞"，
  看起来像修好了，其实是把检测关掉了。

### F2 定时器时钟改成"路径 ack 推进"，删除写时间回退

- 删 `tbt_tx.c:468-469` 的 `wire_ns = r->last_sent_ns`。
- 新判据：
  - `acked_late = ∃i: snd_una 已越过 first_sent_seq[i] 且 now - una_observed_ns[i] >= rto`
    （保持现状，`:630-664`）；
  - `path_stuck = 该帧写过的每条路径都满足 now - c->last_una_move_ns >= rto`；
  - `overdue = acked_late || path_stuck || r->retx_now`。
- 需要新增 `c->last_una_move_ns`：**只在 `snd_una` 真正变化时**更新。现有的
  `last_una_ns`（`tbt_carrier.c:266-269`）在 `blocked` 时也刷新，不能复用。
- 效果：深队列但 ack 在推进 → 不重传；路径真的停住 → 一个 RTO 后重传。
  这正是 DESIGN §4.3 说的 "only the carrier TCP's own acknowledgement may start the RTO"，
  但把"路径级停滞"作为唯一例外，而不是"帧级写时间"。
- 风险：严格相位里若对端 parser 也停（rx 队列满），`path_stuck` 会成立并重传——
  这是想要的；真正的风险是**接收端空洞预算（§19 Q8）成为唯一兜底**，
  `rx_max_stall_us` 可能上升。必须看 E8 的 `rx_max_stall_us` 与 `rc/strict`。

### F3 让宣告与重传对齐

- `tbt_tx.c:472-473` 的 `if (!first) first = r;` 移到超时判断**之后**。
- 定时器每轮最多重传 **1** 帧（`:527` 的 `ARRAY_SIZE(cand)` 对 `retx_now` 保留 8，
  对定时器路径改成 1）。
- 效果：`holes_declared` 与 R1 一一对应，运维指标可信；一个洞不再变成 8 份副本。
- 风险：多洞时修复变慢一个 RTT 量级。配合 F2 后洞本来就少，可接受。

### F4 替换路径防风暴

主因不是 stall 判据，是**写错误分类**（§4.3）。四条，按重要性排序：

1. **短写不是错误。** `tbt_carrier_send()` 在 `(size_t)err != len` 时返回 `-EIO`
   （`tbt_carrier.c:466`）；改成 `-EAGAIN`，让 pump 走"改路重试"（`tbt_tx.c:1427`），
   而不是 `tbt_carrier_lost()`。http-kal 的头块和整帧都走这条路径，这是最可能的
   替换源。
2. **`-ENOMEM` 不是载体死亡。** pump 只应对 `-EIO/-EPIPE/-ECONNRESET/-ENOTCONN`
   判死；`-ENOMEM`（`tbt_carrier.c:565` 的头 kmalloc）应退避重试
   （`tbt_tx.c:1444` 的分支拆分）。
3. **`retx_now` 加上限。** `tbt_tx_path_lost()` 的标记加 `r->sends < 4`
   （`:414-417`）：一份帧最多被"路径死亡"触发 4 次，之后只等接收端预算。
4. **`join_retries` 计数点对齐。** 上限 20 → 4，并在对端 `-EALREADY` 拒绝时也
   +1（`tbt_carrier.c:375-381`），让它真的衡量"连续失败轮数"而不是"载体死亡数"。

（`blocked` 缺 `rcv_wnd > 0` 是防御性修补，不是主因；加上无害，但不该被当成
Q7 的解。）

- 效果：替换风暴的放大系数被钳住；真死路径更早干净失败（bounded failure
  好过风暴）。
- 风险：真实的连续路径死亡会更早杀流（上限 4 而非 20）。用 `adversarial` 的
  carrier-down 用例定标；若 4 太紧，取 8。

### F5 指标口径（不做就无法验收）

- `tbt_session.c:390`：分子改为 `lost_frames + injected_frames`（新增每路径注入计数），
  分母改为写成功的 DATA 记录数。这样 `retx_ppm` 直接就是数据帧重传率，
  `<50k` 才有意义。
- `benches/qemu/init:474` 的 `BENCHCAR` 增加 `rx_lag_ppm`、`carrier_short`、
  `retx_frames`、`retx_bytes` 四列；`run.sh` 的 `carrier_header` 同步。
- 风险：历史 CSV 不可比。在 `benches/README.md` 里注明断代点。

### 验收映射

| 要求 | 由谁满足 | 残余风险 |
|---|---|---|
| `retx_ppm < 50k` | F1（去掉污染）+ F2（去掉回退）+ F5（口径） | 1% 注入下真丢包约 5% 全路径 → 50k 边缘；需 E5 定标 |
| `wire/app < 1.1` | F1+F2 去掉 21%–52% 的重复载荷 | 32 B/8192 B=0.4% + FBK ~1.3% + HTTP chunk 1% ≈ 1.03–1.05 |
| n=4 ≥ 9.20 Mbit/s | 省下的 per-flow 额度变成有效载荷 | F2 让个别洞晚一个 RTO；若 `carrier TCP` 因少了"假重传"而 cwnd 增长变慢，吞吐可能持平而非上升——这是唯一真实的性能风险 |
| 不引入替换风暴 | F4（短写→`-EAGAIN`、`-ENOMEM` 退避、`retx_now` 上限） | 真死路径更早杀流（上限 4） |

---

## 7. 必须靠 bench 才能确认的（静态不可证）

1. **`carrier_short > 0` 且 `rx_lag_ppm > 0` 在干净链路上同时成立。**
   静态只能证明"一次 `-EAGAIN` 会永久留下 lag"，不能证明它真的发生了。
   `holes_declared=505` 已间接证明 lag>0，但没证明来源是 `-EAGAIN` 而不是注入器。
2. **R1 与 R2 的精确拆分。** `T.retx_frames` 与 `Σ lost_frames` 的差值。
   当前 CSV 两者都没有原始值，只有派生 ppm。
3. **F2 之后严格相位是否还能通过。** 这是整个方案的单点风险：假重传今天在
   给交付背压"垫背"，拿掉后接收端空洞预算（§19 Q8）成为唯一兜底。
   必须看 `holes_unrecoverable`、`rx_max_stall_us`、`rc`、`strict`。
4. **n=4 吞吐是否 ≥9.20。** 可能更高（省下 25% 线上字节），也可能持平
   （carrier TCP 的 cwnd 少了假重传的"额外 ack 时钟"）。两种都不能静态推出。
5. **替换的真实来源**：`carrier_lost` 之前，`dbg_pump_dead`、`carrier_err`、
   `carrier_short`、`rx_parse_proto` 哪一个先涨。若 `carrier_short` 与 `carrier_lost`
   同步上涨，§4.3 的短写假设成立；若 `carrier_short` 为 0 而 `carrier_lost` 上涨，
   则回到 stall 判据，E3 会把它分离出来。
6. **`carrier_lost`/`carrier_replaced`/`join_dup` 在 F4 后是否归零**，
   以及 RETIRE 与 replacement HELLO 的竞态是否只剩 1–2 次。
7. **1% 注入下 `holes_unrecoverable` 是否仍为 0**（F1 改变了 gate 的证据链）。
8. **`wire/app` 的剩余超额构成**：32 B 头 + FBK + HTTP chunk 的账要闭合到 ±2%，
   否则说明还有第八条重发路径没找到。

---

## 8. 可被静态证伪的断言（供复核）

以下每条都能用一次读代码/一次读计数器推翻；写在这里是为了让评审有靶子。

- A1：`lost_frames` 只在 `tbt_tx.c:576` 自增，因此 `retx_ppm` 不含注入副本。
  （`grep -n lost_frames tbtun/*.c` → 两处：`tbt_tx.c:576`、`tbtun.h:491`。）
- A2：`tx_frames` 在 `tbt_carrier.c:691` 无类型判断地自增，因此分母含 FBK。
- A3：`data_frames` 在 `tbt_carrier.c:521` 于任何 send 之前自增，因此计的是尝试。
- A4：`recv_lag` 只在换载体（新对象，计数归零）时才会回到 0，因为
  `data_frames - recv_cnt` 的差值对每次 `-EAGAIN` 单调 +1 且后续写不抵消。
- A5：`first` 在 `tbt_tx.c:472` 于超时判断之前锁存，因此存在"重传但不宣告"的窗口。
- A6：`tbt_tx_retx()` 每轮最多入队 8 帧（`tbt_tx.c:527`），不是 1 帧。
- A7：`snd_una` 门控的写时间回退仍在线上（`tbt_tx.c:468-469`）。
- A8：`blocked` 判据缺 `rcv_wnd > 0`（`tbt_carrier.c:263`）。
- A9：`join_retries` 只在对端回 HELLO 时清零（`tbt_rx.c:499`），
  上限 20 是按"累计"而非"连续"计的。
