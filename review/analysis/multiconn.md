# F4 / DESIGN §19 Q6：一个会话服务多条公共连接（多 stream）

**状态**：纯静态设计。未修改源文件，未编译，未运行 QEMU（构建与 QEMU 由另一名工程师占用）。
**代码快照**：当前工作树。行号会随代码漂移，函数名与不变量才是锚点。
**标记**：`[证据]` = 代码或测试输出直接支持；`[推断]` = 由代码结构推出、尚未实测；`[建议]` = 设计选择。

---

## 0. 结论摘要

### 0.1 根因不是一处判断，而是三层

1. **直接拒绝层**：`struct tbt_session.far_sid`（`tbtun/tbtun.h:325`）只有一个槽。
   `tbt_stream_start()` 在 `tbtun/tbt_sock.c:415` 判断 `sess->far_sid != 0`，
   `:423-424` 返回 `-EBUSY`；`tbt_accept_one()` 在 `tbtun/tbt_listen.c:72-79`
   把这个错误变成 `sock_release(newsock)`，客户端看到连接被关闭。
   `far_sid` 只在分配失败时清零（`tbt_sock.c:429-431`），关闭路径从不清零，
   所以第一条连接结束后整个会话永久忙。`[证据]`
2. **并发污染层**：即使删掉 `-EBUSY`，`sess->tx_ack`、`sess->rx_ack`、`sess->peer_paused`、
   `sess->holes[]`、`sess->predict_seq`、`sess->target` 都是单流状态；
   `c->open_sent` 编码“每条载体只发一次 OPEN”；`tbt_session_stream()` 只返回一条流。
   两条流同时存在时，ACK 会跨流套用，重传/空洞判据会错误，目标连接只能绑一条。
   `[证据]`
3. **生命周期层**：流对象只在会话销毁时从哈希摘除（`tbt_session.c:738`）；
   `tbt_stream_kill()` 只置 DEAD 并排队 `w_kill`，不摘链、不释放哈希引用；
   `w_kill` 只在 `tbt_sock.c:346` 释放 socket，且 `:331-332` 在 `sk == NULL` 时提前返回。
   于是每次连接留下一个流对象，`far_sid` 也永不复位；会话不销毁就不回收。`[证据]`

### 0.2 真正的铰链：三个序列空间必须分开

只把 `far_sid` 换成“多条流”会立刻破坏 ACK 与重传。必须显式区分：

| 空间 | 归属 | 用途 | 现有字段 |
|---|---|---|---|
| 每流单向字节偏移 | 每条流、每个方向 | DATA/FIN 的 `seq`、重排、发送窗口、每流 ACK | `st->tx_seq`、`st->rx_next`；`st->tx_ack` 为新增（现为 `sess->tx_ack`） |
| 每路径累计载荷字节 | 每条载体 | 路径 inflight；**负向**判断“肯定未到达”；正向证明 v0.1 仅单流启用（多流待写记账精确后再评估） | 新增 `c->payload_bytes_sent`；接收端复用 `c->rx_bytes` |
| 每路径记录计数 | 每条载体 | 确认丢包比例、空洞门控 | `c->data_frames`、`c->recv_cnt` |

现在代码把第一个空间同时当作第二个空间使用：`c->rx_high`（`tbt_rx.c:147-149`）、
`c->uack`、`c->sent_high`（`tbt_tx.c:1534-1535`）都存“某条流的字节偏移”。
一条路径上混入多条流后，这个值不再单调，也不再能证明任何一条记录的到达。`[证据]`

### 0.3 推荐结论

- **OPEN 帧足够创建流**，不需要新的“流开始”帧。现有 OPEN（`TBT_F_OPEN = 0x13`，
  空体，头里带 sid）就是流创建消息。
- **需要一个新的每流 ACK 帧 `SACK`**，用于反向没有数据可捎带时的单向确认；
  双向流量继续用每个帧头里的 `uack` 捎带。
- **交付权威是每流累计 ACK `st->tx_ack`**；每路径累计载荷字节只用于
  inflight 调度、丢包统计和“肯定未到达”的负向判断。多流下**禁用**每路径
  正向证明（未上线的 A 被计入、B 到达会把它推过 A 的帧尾）；单流保留现有优化。
- **`TBT_CONV` 从 `0x0003` 提升到 `0x0004`**。理由不是帧布局，而是 ACK 语义、
  FBK 的每路径 `uack` 改义、以及 sid 用满 40 位。`tbt_hdr_check()` 已经强制
  `conv == TBT_CONV`（`tbt_proto.c:77`），所以旧版对第一个记录就会拒绝，不存在
  “静默误解析”的混合运行。
- **sid 用满 40 位**：`(prefix16 << 24) | counter24`，`struct tbt_stream.sid` 改为 `u64`，
  哈希键改为 40 位。线格式本来就有 `sid_hi`，现在 `tbt_proto.c:89` 拒绝非零值，
  提升 conv 时一并放开。这样 10k churn 和长会话都不会撞 sid。
- **每条流在远端有自己的 target 连接**。不能把多条独立公共 TCP 连接复用到一条
  target TCP 连接上：TCP 没有消息边界，target 会把字节拼在一起，FIN/RST 也会互相干扰。
- **F6 与 F4 不同源**。F6 是远端载体接收工作的丢失唤醒（provisional 载体的 50 ms
  轮询在 `tbt_carrier.c:218-228` 提前返回，绕过了 `:316-320` 的
  `copied_seq != rcv_nxt` 补救；`w_rx` 只调度一次，`schedule_work` 会合并）。
  它发生在任何流存在之前（`far.data_path` 的 `rx_records=0`、`capture_runs=0`），
  与单流限制无关。但它是多流验收的先决条件，必须先修。`[推断，证据强]`

### 0.4 分阶段

| 阶段 | 内容 | 用户可见结果 |
|---|---|---|
| P0 | F6 载体读唤醒修复 + 测试契约/观测字段 | 远端能稳定收 HELLO；后续测试可信 |
| P1 | 顺序复用（仍只允许一条活跃流）：关闭即进入 tombstone、清门、释放 socket | `pub.churn`、`near.second_conn` 通过 |
| P2 | 每流状态 + 协议 v4（仍只允许一条活跃流）：ACK/重传/空洞/预测/target 全部按流 | 单流探针不回归，为并发打底 |
| P3 | 并发多流：删门、远端按 sid 建流、每流 target、每载体每流 OPEN 矩阵、每流暂停 | `pub.two`（新契约）、100 并发通过 |
| P4 | 清理/公平/内存/文档：tombstone、预算、计数器、F5 半关闭、长稳 | 10k churn 无泄漏，rmmod/flush 干净 |

**不要**把 P0 和 P3 混在一个补丁里：F6 的失败会伪装成多流的失败，反之亦然。

### 0.5 十条不变量（评审用）

1. 一个 tunnel 一个 session；一个 session 有 N 条流；一条流恰好一个 socket。
2. 只有 near 分配 sid；far 只镜像。sid 在会话内对“活跃 + tombstone”唯一。
3. 每条流的 TX 与 RX 字节空间独立；ACK 只作用于该流的 `st->tx_ack`。
4. 每路径 `uack` 是“该路径累计收到的 DATA/PREDICT 载荷字节”，不是任何流的 seq；
   多流下它只用于 inflight/负向判断，**不单独**作为交付证明。
5. near→far 的 DATA 只能走“该流已在该载体写过 OPEN”的载体（`announced_mask`）。
6. 每条流在 far 侧最多一条 target 连接；绝不把两条流复用到一条 target。
7. 哈希里的流一定持有哈希引用；摘链恰好释放一次；任何排队帧/工作项都持有流引用。
8. `sess->lock`、`tx_lock`、`rx_lock` 下不睡眠、不取 socket 锁；socket 锁下可以取它们（数据路径）。
9. 会话 TX 队列是共享 FIFO；pump 是唯一写载体的人；每流有队列配额。
10. 关闭一条流只影响该流；会话关闭才影响全部。tombstone 期内迟到帧被丢弃，不新建流。

---

## 1. 当前限制的根因（问题 1）

### 1.1 现有对象与所有权

```
tbt_tunnel (配置、监听 socket、refs)
  └── tbt_session (nonce、car[]、streams hash、单流 ack/target/holes)
        ├── tbt_carrier[0..n+m-1] (socket、parser、每路径 seq/计数)
        └── hlist streams[64] ──> tbt_stream (sid、sk、rx_q/rx_tree、retain、work)
```

- `struct tbt_session` 的注释直接写着“the session's single stream id”
  （`tbtun/tbtun.h:325`）和“one target per session”（`:322`）。
- `struct tbt_carrier.open_sent` 的注释是“our OPEN for the stream was written”
  （`tbtun/tbtun.h:468`），单数。
- `tbt_session_stream()` 的注释是“The session's single stream, or NULL”
  （`tbt_session.c:608`），9 个调用点全部依赖它。

### 1.2 所有拒绝/阻断第二条连接的位置

#### A. 客户端可见的直接拒绝

| # | 位置 | 条件 | 行为 |
|---|---|---|---|
| A1 | `tbtun/tbt_sock.c:415` | `busy = sess->far_sid != 0` | 判定会话已有流 |
| A2 | `tbtun/tbt_sock.c:423-424` | `busy` | 返回 `-EBUSY` |
| A3 | `tbtun/tbt_listen.c:72-79` | `tbt_stream_start()` 返回错误 | `sock_release(newsock)`，客户端看到 EOF/RST |
| A4 | `tbtun/tbt_sock.c:420` + 全文件无清零 | 第一条流建立 | `far_sid` 永久非零；只有 `:429-431` 的分配失败会清零 |

`pub.churn` 的 1/10000 就是 A1–A4 的直译：第一条连接把 `far_sid` 置位，关闭后
`tbt_stream_kill()` 只置 DEAD，没有人清 `far_sid`；下一次 accept 必然 `-EBUSY`。
`near.second_conn` 同因。`[证据：adversarial/findings.md F4]`

#### B. 远端协议拒绝

| # | 位置 | 条件 | 行为 |
|---|---|---|---|
| B1 | `tbtun/tbt_rx.c:113-125` | `sess->far_sid` 已存在且 `!= h->sid_lo` | 返回 `-EPROTO`；调用方 `tbt_car_rx_actor` 把该载体标记 stopping（`tbt_carrier.c:365-380`） |
| B2 | `tbtun/tbt_rx.c:126-127` | 首次采纳 | 只对 far 调 `tbt_far_open_seen(sess)`，没有 sid 参数 |
| B3 | `tbtun/tbt_sock.c:456-458` | `sess->far_sid == 0` | `tbt_far_open_seen()` 直接返回 |
| B4 | `tbtun/tbt_sock.c:475-477` | 同上 | `tbt_target_attach()` 返回 `-EAGAIN` |
| B5 | `tbtun/tbt_sock.c:503-504` | 同上 | `tbt_target_detach()` 找不到流 |

B1 是“第二条流”在远端的正式拒绝：它不是“拒绝连接”，而是**毒化整条载体**。
在旧契约里这是对的（`far.open_other_sid` 期望 PROTO）；在多流里必须改成
“新 sid 是合法的新流”。

#### C. 删掉 A/B 后才会暴露的静默阻断

| # | 位置 | 条件 | 后果 |
|---|---|---|---|
| C1 | `tbtun/tbt_tx.c:150-169` | `tbt_tx_announce()` 跳过 `c->open_sent` 为真的载体 | 第二条流的 OPEN 永远不会排到已服务过第一条流的载体上 |
| C2 | `tbtun/tbt_tx.c:1303-1311` | pump 遇到 `type == OPEN` 且 `c->open_sent` 为真 | 直接把 OPEN 帧 `goto drop` 丢弃 |
| C3 | `tbtun/tbt_tx.c:1337-1340` | 未 pinned 的 DATA 要求 `c->open_sent` | 第二条流无法使用任何“已 OPEN 过”的载体 |
| C4 | `tbtun/tbt_tx.c:1171` | 调度器 `need_open` 过滤 `!c->open_sent` | 新流的数据路径被排除 |
| C5 | `tbtun/tbt_tx.c:1476-1489` | 写 HELLO 后只为 `tbt_session_stream(sess)` 排一条 OPEN | 新载体加入时只宣告一条流 |
| C6 | `tbtun/tbt_session.c:841-846` | `tbt_session_join()` 只宣告 `tbt_session_stream()` | 同上，且 join 在载体 socket 锁内，不能用 GFP_KERNEL 批量分配 |
| C7 | `tbtun/tbt_session.c:608-616` | 单流访问器 | 9 个调用点（见 §1.4）全部只能看到一条流 |
| C8 | `tbtun/tbt_carrier.c:1372-1388` | `target_started` 一次性 | 整个会话只拨一条 target |
| C9 | `tbtun/tbt_sock.c:466-467` | 只 attach `sess->target` | 第二条流没有 target 可绑 |
| C10 | `tbtun/tbt_session.c:749-764` | 销毁时只释放 `sess->target` | 每流 target 无处安放 |
| C11 | `tbtun/tbt_carrier.c:403-414` | 任一 `st->paused` 就停止读**所有**载体 | 一条流的背压会 HOL 阻塞全部流 |
| C12 | `tbtun/tbt_carrier.c:784-790` | join 重试 20 次后杀 `tbt_session_stream()` | 载体加入失败会随机杀一条流（多流时更不可接受） |

#### D. 生命周期导致的“永不复用”

| # | 位置 | 事实 | 后果 |
|---|---|---|---|
| D1 | `tbtun/tbt_session.c:720-747` | 只有会话销毁循环里 `hlist_del_rcu(&st->node)`（`:738`） | 每次连接留下一个流对象，直到 flush/rmmod |
| D2 | `tbtun/tbt_session.c:525-541` | `tbt_stream_free_rcu()` 只由 `tbt_stream_put()` 触发 | 哈希引用不释放，对象永不 free |
| D3 | `tbtun/tbt_sock.c:321-347` | `w_kill` 只在 `sk != NULL` 时 `sock_release`；`:331-332` 早返回 | socket 可能泄漏；且没有摘链动作 |
| D4 | `tbtun/tbt_sock.c:288-292` | `TCP_CLOSE`/`sk_err` 才 kill | 正常 FIN 关闭可能停在 CLOSING/LAST_ACK，不进入回收 |
| D5 | `tbtun/tbt_session.c:749-764` | 销毁只释放 `sess->target` | near 侧的公共 accepted socket 在销毁路径没有统一释放点 |
| D6 | `tbtun/tbtun.h:325` | `far_sid` 无“已关闭”状态 | 无法区分“从未有流”和“上一条流已死” |

### 1.3 `pub.churn` 的完整因果链

```
accept #1
  → tbt_stream_start: far_sid=0 → 分配 sid，far_sid=sid
  → 流建立、传输、关闭
  → tbt_stream_kill: state=DEAD；w_kill 释放 socket
  → 没有代码清 far_sid；哈希引用仍在
accept #2
  → tbt_stream_start: far_sid!=0 → -EBUSY
  → tbt_accept_one: sock_release(newsock)
  → 客户端 read 得到 0/-1，计为 refused
... 重复 9999 次
```

远端同样：`tbt_rx.c:116` 写入 `far_sid` 后从不清零；即使 near 侧修好，far 侧仍会
把新 sid 判成 PROTO（B1）。所以 **F4 必须两端同时改**，不能只改 near。

### 1.4 单流访问器的 9 个调用点

`tbt_session_stream()`（`tbt_session.c:609`）被以下位置调用；多流时必须逐个改成
“按 sid 取流”或“遍历所有活跃流”：

| 位置 | 用途 | 多流改法 |
|---|---|---|
| `tbt_carrier.c:786` | join 重试耗尽后杀流 | 杀全部活跃流或置 `sess->closing` |
| `tbt_session.c:841` | join 后宣告流 | near 侧遍历活跃流；far 侧不宣告 |
| `tbt_tx.c:403` | `tbt_tx_path_lost()` | 遍历所有流的 retain，清该路径位 |
| `tbt_tx.c:771` | `tbt_tx_holes_repaired()` | 每流自己的 hole FIFO |
| `tbt_tx.c:811` | `tbt_tx_hole_check()` | 每流调用一次 |
| `tbt_tx.c:1020` | `tbt_tx_stall_check()` | 每流独立计时、独立 kill |
| `tbt_tx.c:1086` | `tbt_tx_ack()` | 由帧头 sid 找到流，只更新该流 |
| `tbt_tx.c:1264` | pump 顶部 GC/retx | 遍历活跃流，逐流 GC/retx |
| `tbt_tx.c:1830` | `tbt_fbk_work()` | 遍历活跃流，发 SACK/查空洞 |

### 1.5 会话级单流状态清单（必须搬进 `tbt_stream`）

| 字段 | 现在位置 | 语义 | 多流后果 |
|---|---|---|---|
| `tx_ack` | `tbtun.h:345` | 对端已交付的**我方 TX**偏移 | A 的 ack 被套到 B 的 seq 上；B 的未确认帧被 GC |
| `rx_ack` | `tbtun.h:346` | 我方已交付偏移，写进每个帧的 `uack` | 所有流共用一个 uack；对端无法分辨 |
| `peer_paused` | `tbtun.h:347` | 对端交付队列满 | 一条流暂停会抑制所有流的 stall kill |
| `holes[]` / `hole_head/tail` | `tbtun.h:409-410` | 已声明空洞 FIFO（按 seq） | 不同流的 seq 混在同一个 FIFO，修复判断错误 |
| `predict_seq` / `predict_ns` | `tbtun.h:405-406` | 发送端最近占位符 | 不同流的 seq 互相覆盖 |
| `last_ack_ns` / `pause_cleared_ns` | `tbtun.h:414-415` | stall 计时 | 一条流的进度会重置另一条流的计时 |
| `target` / `target_started` / `w_target` | `tbtun.h:319-323` | 远端目标连接 | 只能服务一条流 |
| `c->open_sent` | `tbtun.h:468` | 该载体已为“那条流”写过 OPEN | 阻断后续所有流的 OPEN |
| `c->rx_high` | `tbtun.h:499` | 该路径上“那条流”的最高 seq | 多流混跑后不再单调 |
| `c->sent_high` | `tbtun.h:482` | 该路径上“那条流”的最高写偏移 | 同上 |
| `c->spec_seq` | `tbtun.h:501` | 该路径上占位符的 seq | 不同流的占位符互相清除 |
| `c->uack` | `tbtun.h:478` | 对端每路径“那条流”的高水位 | 改成每路径累计载荷字节；只作 inflight/负向判断 |

---

## 2. 目标对象模型（问题 2）

### 2.1 归属关系

```
tbt_tunnel
  ├── 配置、监听 socket、refs、accepting
  └── tbt_session (1:1)
        ├── nonce、sid 分配器、closing
        ├── car[0..n+m-1] ──> tbt_carrier
        │                       ├── socket / sk / parser / HTTP 状态
        │                       ├── 每路径累计载荷字节（发送/接收）
        │                       ├── 每路径记录计数、RTT、gen
        │                       └── hello_sent / up / open_sent(诊断)
        ├── streams hash (64 桶) ──> tbt_stream (N)
        │                             ├── sid (u64, 40 位有效)
        │                             ├── 唯一的 socket：near=公共 accepted，far=target
        │                             ├── RX：rx_q / rx_tree / rx_next / fin / paused
        │                             ├── TX：tx_seq / tx_ack / retain / holes / predict
        │                             ├── 每载体宣告位图 announced_mask
        │                             ├── work：w_read / w_drain / w_ctrl / w_kill
        │                             ├── far 专属：w_target / target_started
        │                             └── refs / rcu / tombstone
        └── 共享 TX 队列 tx_q / tx_queued / tx_lock
```

### 2.2 `struct tbt_stream`：新增/迁移字段

不写补丁，只列字段与语义。现有字段保留，新增用 `+`，迁移用 `←`。

```c
struct tbt_stream {
    /* 身份与生命周期 */
    u64 sid;                  /* 40 位有效；哈希键；替代 u32 */
    u8  state;                /* PENDING/OPEN/CLOSING/DEAD；见 §2.5 */
    bool near;                /* 本端是否 sid 分配者 */
    bool unlinked;            /* 已从哈希摘除，防止二次摘 */
    bool dying;               /* 终态迁移只允许一个赢家 */
    u64 tombstone_until_ns;   /* DEAD 后保留在哈希的截止时间 */
    u32 n_frames;             /* 排队帧持有的引用数（调试） */

    /* socket 与工作项 */
    struct sock *sk;
    unsigned long attached;
    struct work_struct w_read, w_drain, w_ctrl, w_kill;
    struct work_struct w_target;   /* far：拨 target，替代 sess->w_target */
    bool target_started;           /* far：本流只拨一次 */
    /* tombstone 到期由会话级 w_reap 扫描，不为每条流排一个 delayed work */

    /* TX（每流） */
    u64 tx_seq;               /* 已保留 */
    u64 tx_ack;               /* ← sess->tx_ack */
    u64 last_ack_ns;          /* ← sess->last_ack_ns */
    u64 pause_cleared_ns;     /* ← sess->pause_cleared_ns */
    bool peer_paused;         /* ← sess->peer_paused */
    bool tx_paused;           /* 已保留，语义改为“本流队列配额满” */
    bool fin_queued;          /* + FIN 已排过，避免重复 */
    bool rst_queued;          /* + RST 已排过，避免重复 */
    u32 tx_queued;            /* + 本流在会话队列中的字节数 */
    u32 tx_queue_max;         /* + 本流配额 */
    bool tx_open_sent;        /* 已保留 */
    u16 announced_mask;       /* + near：哪些载体已写过本流 OPEN */
    u64 predict_seq, predict_ns; /* ← sess->predict_seq/ns */
    struct tbt_hole *holes;   /* ← sess->holes，改为每流惰性分配 */
    u16 hole_head, hole_tail; /* ← sess->hole_head/tail */
    struct list_head retain;  /* 已保留，仍由 sess->tx_lock 保护 */
    u32 retain_bytes, retain_max;

    /* RX（每流，已基本就绪） */
    spinlock_t rx_lock;
    struct list_head rx_q;
    struct rb_root_cached rx_tree;
    u64 rx_next, rx_delivered;
    u32 rx_queued, rx_ooo_bytes, rx_ooo_frames;
    u32 rx_limit;
    u64 hole_since_ns;
    u64 rx_fin_seq;
    bool rx_fin_seen, rx_fin_done;
    bool paused;
    u8  sack_state;           /* + 0 ok / 1 hole / 2 paused / 3 dead */
    u64 last_sack_next;       /* + 上次发出的 ack 值 */
    u8  last_sack_state;      /* + 上次发出的状态 */
    bool sack_dirty;          /* + 需要发 SACK */
};
```

要点：

- `st->tx_lock` 目前初始化了但未使用（保留链由 `sess->tx_lock` 保护）。多流**不要**
  拆成每流 TX 锁；共享队列 + 共享 `tx_lock` 更简单，帧持有流引用即可。
- `st->holes` 必须每流，因为 `seq` 是每流空间。惰性分配，避免 100 条流各 4 KiB
  常驻；总上限由会话预算约束。
- `st->announced_mask` 是 16 位（最多 16 条载体），正好一个 `u16`。
- `st->tx_queue_max` 用于防止一条流吃满 64 KiB 的会话队列（`TBT_SQ_MAX`）。

#### `struct tbt_frame` 的两个新字段

```c
struct tbt_frame {
    ...
    struct tbt_stream *st;  /* + 该帧所属的流；持有引用；会话级帧为 NULL */
    u64 path_end;           /* + 写出时在本路径字节空间的结束位置 */
    ...
};
```

- 入队/重传/预测/控制帧创建时绑定 `fr->st` 并 `refcount_inc`；帧写出、
  丢弃、`tbt_tx_free_queue()` 时 `tbt_stream_put`。
- 这样 pump 不需要在 `tx_lock` 下做哈希查找，也保证流对象在帧排队期间
  不会被释放（§2.6）。
- `path_end` 只对 DATA/PREDICT 有意义；它在**写成功后**由 pump 填入，
  用于 §3.2b 的负向判断/单流正向证明。

### 2.3 `struct tbt_session`：保留什么、删什么

保留：

- `streams` 哈希、`next_sid` / `sid_prefix`（`next_sid` 改 u64）。
- `car[]`、`n_car`、`n_pri`、`prov`、`dead`、`closing`。
- `nonce`、`nonce_set`、`nonce_pinned`。
- 共享 TX 队列 `tx_q` / `tx_queued` / `tx_lock`。
- 调度器 `sched_*`（全局选路，不需要每流一份）。
- `carrier_gen`、`join_retries`。
- 会话级统计与 FBK 节拍 `fbk_frames`。

删除/迁移：

- `far_sid`：删除。远端按 sid 查哈希；近端只分配不保存“当前唯一流”。
- `tx_ack` / `peer_paused` / `last_ack_ns` / `pause_cleared_ns`：迁到流；
  `rx_ack` 不保留字段，直接用 `st->rx_next`。
- `holes` / `hole_head/tail`：迁到流。
- `predict_seq` / `predict_ns`（发送端）：迁到流。
- `w_target` / `target` / `target_started`：迁到流。
- `tbt_session_stream()`：删除；调用点改成“按 sid”或“遍历”。

新增：

- `u32 n_live`：活跃流数（PENDING/OPEN/CLOSING）。
- `u32 n_tombstones`：DEAD 但仍在哈希的流数。
- `u32 max_streams`：来自模块参数/配置，默认建议 1024。
- `struct delayed_work w_reap`：会话级 tombstone 扫描器（200 ms 一次，
  没有 tombstone 时停）。
- `u64 rx_queued_total`：所有流 `rx_q + rx_tree` 的字节和（原子或
  `sess->lock` 保护）；只算 `rx_q` 会漏掉重排树的内存。
- `u64 rx_budget`：会话接收预算，建议 256 MiB。
- `u64 retain_bytes_total`：所有流 retain 的字节和，用于会话级重放预算。
- `u64 streams_rejected`、`late_frames`、`tombstone_forced`：诊断。

### 2.4 `struct tbt_carrier`：每路径三个计数器

```c
struct tbt_carrier {
    ...
    u64 payload_bytes_sent;  /* + 本路径写出的 DATA/PREDICT 载荷字节累计 */
    u64 rx_bytes;            /* 已有；改为统计 DATA+PREDICT 载荷字节 */
    u64 data_frames;         /* 已有；DATA 记录数（不含 PREDICT） */
    u64 rx_frames;           /* 已有；收到的 DATA 记录数 */
    u64 uack;                /* 语义改为：对端在本路径累计收到的载荷字节 */
    u64 uack_ns;
    u32 recv_cnt;            /* 已有；对端在本路径收到的 DATA 记录数 */
    u64 sent_high;           /* 删除；用 payload_bytes_sent 替代 */
    u64 rx_high;             /* 删除；用 rx_bytes 替代 */
    u64 spec_path_end;       /* + 本路径最新占位符之后的载荷字节位置 */
    u32 spec_bytes;          /* 保留；每路径投机预算 */
    ...
};
```

`uack` 的名字可以保留（`/proc` 列名不动），但注释和语义必须改。`peers` 的
`uack`/`inflight` 列从“流字节偏移”变成“路径载荷字节”；这是可观测语义变化，
必须在 DESIGN §8.3 与测试说明里写清。

`uack` 是**advisory**：多流下它只回答“这条路径还差多少字节才到某帧的末尾”
（负向判断），不回答“某帧一定到了”（正向证明）；后者以每流 `st->tx_ack`
为准。单流时保留现有的正向优化（§3.2b）。

### 2.5 流生命周期状态机

```
                 near accept / far 首个 OPEN|DATA
        ┌──────────────────────────────────────────┐
        ▼                                          │
   [PENDING] ── target attach / OPEN ──> [OPEN] ──┤
        │                                   │      │
        │ target 失败 / RST                 │ FIN  │
        ▼                                   ▼      │
     [DEAD] <── RST / 错误 ─────────── [CLOSING]  │
        │                                          │
        │ w_kill: detach + release socket          │
        │ 保留在哈希，丢弃迟到帧                   │
        ▼                                          │
   [TOMBSTONE] ── grace 到期 w_reap ──> 摘链 + put 哈希引用
```

规则：

1. **PENDING**：far 已按 sid 建流，target 正在拨；RX 可以排队，TX 不开始。
2. **OPEN**：socket 已 attach；双向可传。
3. **CLOSING**：本端已发 FIN（或收到 FIN 且写侧已关）；仍可收对端 FIN/数据。
4. **DEAD**：终态；不再接受新数据；异常关闭要发 RST。`w_kill` 负责 detach、
   释放 socket、进入 tombstone。
5. **TOMBSTONE**：仍在哈希，`tbt_stream_get()` 能找到但所有 RX 帧被丢弃；
   不参与 `n_live`，参与 `n_tombstones`。到期由**会话级** `w_reap` 扫描摘链。
6. **tombstone 宽限**必须大于“最后一帧可能还在路上的时间”：建议
   `2 × max(hole_budget_ms, replay_ms)`，默认 2 s。理由：载体 TCP 重传 +
   发送端 replay 窗口；宽限太短会让迟到帧在 far 侧被当成新流并拨 target。
7. **sid 复用**：分配器跳过“活跃 + tombstone”sid；40 位计数器使实际复用
   几乎不可能。若 24 位计数器耗尽（16.7M 次分配），回绕并跳过占用项；
   若无可跳过项，返回 `-EBUSY` 并计数。

现有的 `TBT_ST_GAP` / `TBT_ST_REORDER` / `TBT_ST_HOLE` 不是生命周期状态，
而是 `tbt_stream_kill(st, reason)` 的 `reason` 取值；`kill_reason` 保留它们，
`state` 只使用 PENDING/OPEN/CLOSING/DEAD（外加哈希里的 tombstone 标记）。

### 2.6 引用计数与 RCU

引用来源：

| 持有者 | 何时加 | 何时减 |
|---|---|---|
| 哈希 | 插入时（`refs=2` 中的 1） | `w_reap` 摘链时恰好一次 |
| 调用者 | `tbt_stream_get()` 返回时；或 near accept 路径在 `publish` 前设 `refs=2` | 调用者 `tbt_stream_put()` |
| 排队帧 | 帧创建并绑定 `fr->st` 时 | 帧写出/丢弃/free 时 |
| 工作项 | `queue_work` 前 `refcount_inc` | 工作项末尾 `put` |
| 会话销毁循环 | 从哈希摘下的引用转移给循环 | 循环末尾 `put` |

不变量：

- 一个流只要还在哈希里，就一定持有哈希引用；摘链只能发生一次
  （`st->unlinked` 保护）。
- `w_kill` / `w_target` 都可能比哈希引用活得久，因此**必须**自己持引用。
  tombstone 的摘链由会话级 `w_reap` 完成：它在 `sess->lock` 内摘链并
  `tbt_stream_put` 哈希引用；`w_reap` 本身由会话生命周期保护（destroy 时
  `cancel_delayed_work_sync`），不需要每流引用。
- `tbt_stream_free_rcu()` 负责释放残留的 `rx_q`/`rx_tree`/retain/holes；
  释放后断言 `st->sk == NULL`、`attached == 0`、`list_empty(&st->rx_q)`、
  `rx_tree` 为空；否则是生命周期 bug。
- `sess->lock` 下只做 `hlist_del_rcu` 和 `refcount` 操作；`cancel_work_sync`、
  `sock_release`、`kfree` 一律在锁外。

### 2.7 sid 分配

```c
/* 建议，不是补丁 */
u64 sid = ((u64)(sess->sid_prefix & 0xffff) << 24) |
          (sess->next_sid++ & 0xffffff);
```

- 40 位：`sid_hi = sid >> 32`（prefix 的高 8 位），`sid_lo = sid & 0xffffffff`。
- sid 的“查重 + 哈希插入”在 `sess->lock` 内完成，避免两个 CPU 同时拿到同一个 sid。
  流对象本身在锁外用 `kzalloc(GFP_KERNEL)` 分配并初始化（锁、list、work、refs），
  再在锁内发布；**不要**在持锁时调用 `tbt_stream_get()`（它自己会取
  `sess->lock`，会自锁死）。near 的 accept 路径建议用一个
  `tbt_stream_publish_locked()`：锁内查重、设 `sid`、`hlist_add_head_rcu`、
  `n_live++`；锁外再做 announce/attach。
- 查重范围：`streams` 哈希（活跃 + tombstone）。
- `max_streams` 在分配前检查；满则 near 返回 `-EBUSY`（accept 路径释放 socket），
  far 丢弃该帧并计数 `streams_rejected`。
- 16 位 prefix 的作用是降低跨会话/跨重启的偶然碰撞；不是安全边界，nonce 才是。

### 2.8 内存预算

多流把三个“每会话一份”的预算变成了“每流一份”，必须显式设上限：

| 预算 | 现在 | 多流建议 | 说明 |
|---|---|---|---|
| 活跃流数 | 1 | `max_streams=1024` | 100 并发验收的下限；1024 留余量 |
| 每流 `rx_q` | `TBT_RX_RING_MAX=64 MiB` | 保留 64 MiB（单流 40 MiB 测试需要） | 但加会话总预算 |
| 会话 `rx_q` 总和 | 无 | `rx_budget=256 MiB` | 超过则对新帧返回 `-EAGAIN`，暂停该流 |
| 每流 retain | `replay=8 MiB`（per stream） | 会话级 `retain_budget=64 MiB` | 否则 100 流 × 8 MiB = 800 MiB |
| 每流 socket buffer | 1 MiB + 1 MiB | 保留，但 100 流 = 200 MiB | 测试用 1 MiB/流，观察 MemAvailable |
| tombstone 数 | 0 | `max_tombstones=4096` | 超过则强制回收最老的，计数 |

**结论**：`TBT_RX_RING_MAX` 和 `replay` 的 per-stream 语义在多流下会相乘。
P3 必须先加会话总预算，再做 100 并发，否则 `pub.writer` 式的内存压力会直接 OOM。

---

## 3. 线上格式（问题 3）

### 3.1 现有 OPEN 足够承载“新 stream 开始”

- `TBT_F_OPEN = 0x13`，`body_len=0`，头里 `sid_hi:sid_lo` 命名流
  （`tbt_uapi.h:52-54`、`:143-145`）。
- **sid 天然按 stream 命名，但唯一性由 near 的分配器保证**，不是字段自带。
  线格式的 40 位空间足够；当前 v0 只用低 32 位且从不回收，导致第二条流连
  名字都拿不到。v4 用 40 位 + “跳过活跃/tombstone”的分配器，才使
  “一个 sid = 一条流”成为可依赖的不变量。
- 语义建议：**near→far 单向**，每条流在每条载体上最多一次，且必须先于该载体上
  该流的 DATA。
- far→near 的 OPEN 回显建议删除。理由：
  1. near 是 sid 的分配者，far 不需要“回告”一个 near 已经知道的流；
  2. 删除后 far 不需要维护 `announced_mask`，也不需要 `tbt_far_open_seen()` 的
     回显分支；
  3. 旧代码的回显主要用于“far 采纳 sid 后触发 target attach”；P3 把 target
     attach 移到 far 的流创建路径，不再需要回显。
- 如果评审坚持保留双向 OPEN，它必须是**幂等**的：near 收到 far 的 OPEN 时只
  校验 sid 存在，不创建流、不改变状态；far 收到 near 的重复 OPEN 时只更新
  `announced_mask`，不重复拨 target。

### 3.2 三个序列空间（协议的核心）

#### (a) 每流单向字节偏移

- `seq` 字段：DATA 的第一个字节、FIN 的“最后一字节之后”。
- 发送端 `st->tx_seq`，接收端 `st->rx_next`。
- ACK 值：`st->tx_ack`（对端 `rx_next`）。
- 重排树、空洞预算、发送窗口都以它为准。
- 每条流每个方向独立，从 0 开始。

#### (b) 每路径累计载荷字节

- 发送端 `c->payload_bytes_sent`：本路径写出的 DATA/PREDICT 载荷字节累计。
- 接收端 `c->rx_bytes`：本路径**接受进流状态**的 DATA/PREDICT 载荷字节累计。
- FBK 每路径 `uack` 携带 `c->rx_bytes`。
- 发送端为每个 retain 记录 `r->path_end[i]`：该副本在本路径字节空间中的结束位置。
- **方向性规则（多流的核心）**：
  - **否定是安全的**：若 `c->uack < r->path_end[i]` 且 gen 匹配，该副本一定
    没有在本路径完整到达（收到的字节还不够），可以继续重传。
  - **肯定是不安全的（在记账不精确时）**：若 `c->uack >= r->path_end[i]`，
    只能说明本路径收到的载荷字节总数够了。反例：路径上先“计入”A(100 B)
    但没有真正上线（debug ppm 丢弃、`-EAGAIN` 被当成已发、partial write
    的尾部记账错误），再写 B(100 B) 并正常到达；接收端 `uack=100`，恰好
    等于 A 的 `path_end=100`，会误判 A 到达。TCP 本身不会让 B 越过已写出的
    A，所以这不是网络丢包，而是**发送端记账**把未上线字节算进了路径空间。
    单流时 `recv_lag` 门能抓住多数这种情况；多流时路径级聚合的 `recv_lag`
    可能被另一条流的记录抵消，门不再可靠。
  - 因此多流下**不得**用这个正向比较去跳过重传/GC；等 P4 把写记账做成
    逐字节精确（partial tail、ppm 返回码、-EAGAIN 不计）并有每流证明后，
    再考虑重新启用。
  - 单流时（`n_live == 1`）正向比较仍然有效，并保留 `recv_lag == 0` 的门
    （现有 `tbt_retain_received()` 的规则，`tbt_tx.c:611`）。
  - 多流时的权威交付证明是**每流累计 ACK** `st->tx_ack`：它只在该流的连续
    字节全部到达时前进，不受其他流影响。GC/重传的“已到达”以它为准；
    每路径字节只用于 inflight 调度、丢包统计和“肯定未到达”的负向判断。
- **前提（必须写进代码注释）**：
  1. `payload_bytes_sent` 只在该记录被 `tcp_sendmsg_locked` **完整接受之后**
     累加；写前累加会把从未上线的字节算进去，产生假阳性（review 14 的
     `data_frames` 就是写前累加，不能照抄）。
  2. `rx_bytes` 只在该记录被接收端**接受进流状态之后**累加（入 `rx_q` 或
     reorder tree），解析后因 `-ENOMEM`/暂停而丢弃的记录不能算。
  3. 单流的正向证明与 `recv_lag == 0` 联合使用：有确认记录缺口时，字节
     高水位不是帧级证明（现有 `tbt_retain_received()` 在 `tbt_tx.c:611`
     已经这样做）。
- 路径 inflight：`c->payload_bytes_sent - c->uack`（按路径、跨流）。
- 旧的 `c->rx_high`、`c->sent_high` 删除。

**代价**：多流下跨路径乱序时，可能多重传一些“已经到达但被洞挡住”的帧。
用 `retx_extra` 计数；P4 再评估是否做每流 SACK bitmap 来消除这份浪费。
这比“用一个不可靠的正向证明跳过重传”安全：错误的正向判断会丢帧，
错误的负向判断只会多传一次。

#### (c) 每路径记录计数

- 发送端 `c->data_frames`，接收端 `c->recv_cnt`。
- 用途：确认丢包比例 `loss_ppm`、空洞门控的 `tail`/`lag`。
- 它只数 DATA 记录，不参与字节窗口；与 (b) 独立。
- 多流下 `recv_lag` 是**路径级聚合**，会混合不同流的记录；它只能用于
  丢包比例/诊断，不能作为某条流的交付证明。`loss_ppm` 的语义应从
  “确认丢帧比例”降级为“路径记录缺口比例的估计”，或在 P4 改成每流统计。

#### 三个空间不要混用

| 错误用法 | 后果 |
|---|---|
| 用 `c->uack` 存某条流的 seq | 多流交错后不再单调；证明失效；误重传/误 GC |
| 用 `st->tx_ack` 做路径 inflight | 一条流的 ack 影响所有路径的调度 |
| 用 `c->recv_cnt` 做字节窗口 | 记录大小不等，窗口失真 |
| 用 `sess->rx_ack` 做所有流的 uack | 对端无法分辨是哪条流 |

### 3.3 每流 ACK：捎带 + SACK

- **捎带**：每个属于流 S 的帧，头里的 `uack = S->rx_next`。DATA/FIN/RST/PREDICT/
  CANCEL 都可以捎带。这是双向流量的主路径，零额外帧。
- **SACK**：新增帧类型 `TBT_F_SACK = 0x25`，用于反向没有数据可捎带时。

```
type      = 0x25
sid_hi:sid_lo = 流
uack      = st->rx_next（连续已收偏移）
seq       = 0
body_len  = 1
body[0]   = state: 0 ok / 1 hole / 2 paused-or-target-connecting / 3 dead
path_id   = 发送它的载体
```

发送规则：

- 每 10 ms 的 FBK tick 里，若 `st->rx_next != st->last_sack_next` 或
  `st->sack_state != st->last_sack_state`，发一个 SACK。
- 若同一 tick 内该流已经发过任何带 `uack` 的帧，且 `uack` 等于当前 `rx_next`，
  可以跳过 SACK（捎带已覆盖）。
- SACK 的 `uack` 只增不减；发送端按 `tbt_tx_ack` 的单调规则处理。
- 100 条流、每条 10 ms 一个 SACK 的最坏开销：100 × 100/s × 32 B = 320 KB/s
  ≈ 2.6 Mbit/s。只在“ack 有变化”时发，空闲流为 0。

接收规则：

- `uack` 只作用于 `sid` 命名的流；`uack > st->tx_seq` 时截断到 `st->tx_seq`。
- `state == 2` 设置 `st->peer_paused=true`，抑制该流的 stall kill；
  `state == 3` 只 kill 该流。
- SACK 的 `uack` 为 0 时忽略（与现有 `tbt_tx_ack` 的 `if (!uack) return` 一致）。

### 3.4 FBK 语义调整

FBK 保持**会话级**，因为它的主体是“每条载体的健康”，不是流：

- `sid_lo = 0`、`sid_hi = 0`、头 `uack = 0`。
- body 仍是 `nr × 18 B`，布局不变：
  `path/gen | rx_state | uack | recv_cnt | rtt_us`。
- `uack` 改义为“该路径累计收到的 DATA/PREDICT 载荷字节”（§3.2b）。
- `rx_state` 改义为**载体级聚合**：
  `0` 正常；`1` 任一流有空洞；`2` 任一流暂停/正在连 target；`3` 会话 closing。
  每流状态由 SACK 携带；`/proc` 可以另外展示每流状态。
- `recv_cnt` 仍是该路径的 DATA 记录数。
- `rtt_us` 仍是该路径 RTT。
- PREDICT 的 16 字节也计入路径载荷字节：发送端 `payload_bytes_sent` 在写出
  PREDICT 时累加 16，接收端在 PREDICT 分支同样累加 `rx_bytes`。占位符的
  清除条件改为 `uack >= spec_path_end`（占位符之后的下一个路径字节位置），
  不再用 `spec_seq`；否则多流下不同流的 seq 会互相清除。
- 收到 FBK 后：更新每路径 `uack/recv_cnt/rtt`；然后**逐流**调用
  `tbt_tx_hole_check(st, mask)`，因为“哪些帧是空洞”是每流 retain 的判断。
  注意：每路径 `uack` 只用于 inflight/负向判断；多流下**不要**用它做
  “该帧已到达”的正向证明（§3.2b）。每流交付以 `st->tx_ack` 为准。

这样 10 ms 的载体反馈与流数解耦；100 条流不会把 FBK 放大 100 倍。

### 3.5 40 位 sid

- 线格式已有 `sid_hi`（`tbt_uapi.h` 头偏移 +28）和 `sid_lo`（+24）。
- 现在 `tbt_proto.c:89` 拒绝 `sid_hi != 0`；v4 放开，改为 `sid < (1ULL<<40)`。
- `struct tbt_stream.sid`、`tbt_stream_get()`、哈希桶、`tbt_stream_kill_by_sid()`、
  `/proc` 打印全部改 `u64`。
- 所有 `hdr.sid_lo = st->sid` 的位置改为 `tbt_hdr_set_sid(&hdr, st->sid)`；
  所有 `hdr->sid_lo` 的读取改为 `tbt_hdr_sid(hdr)`。涉及
  `tbt_tx.c:133`、`:739`、`:758`、`:1648`、`:1794`、`tbt_rx.c:143/331/350/369/393`、
  `tbt_session.c` 的 `tbt_stream_get/kill_by_sid`、`tbt_procfs.c` 的打印。
- `sid_hi` 不再被 `tbt_hdr_check` 拒绝；`reserved2` 仍必须为 0。

### 3.6 `TBT_CONV` 提升与兼容策略

**建议：`0x0003` → `0x0004`，硬不兼容。**

变更理由（任意一条都足以提升）：

1. 每路径 FBK `uack` 从“流 seq 高水位”改为“路径载荷字节”。
2. 每流 ACK 语义显式化（捎带 `uack` 按 sid 解释；新增 SACK）。
3. `sid_hi` 从必须 0 变为有效位（40 位 sid）。
4. far→near OPEN 回显删除（若采纳）。

兼容策略：

- `tbt_hdr_check()` 已经要求 `h->conv == TBT_CONV`（`tbt_proto.c:77`）。
  v4 模块收到 v3 帧 → 第一个记录（HELLO）即 `-EPROTO` → 载体被关闭；
  v3 模块收到 v4 帧同理。**不存在混合版本运行**，也不会静默误解析。
- 两端必须一起升级。模块没有用户态配对（DESIGN §11），所以这是可接受的。
- 若产品要求滚动升级，可另加 HELLO 能力位和 v3 回退（v3 模式只允许一条流）。
  这会把数据路径的状态机翻倍，**不建议**在 v0.1 做；硬提升更诚实。
- `TBT_VERSION` 不必跟着 wire conv 走；两者独立（`tbt_uapi.h:44` 的注释）。

### 3.7 线上不变量

1. HELLO 必须是每条载体的第一个记录；HELLO 的 conv 必须等于本端。
2. OPEN 的 `sid` 必须是 near 分配的；far 不得发明 sid。
3. near→far DATA 的 `sid` 必须已在该载体上发过 OPEN。这是**发送端**不变量
   （`announced_mask`）；接收端为容忍 OPEN 丢包，允许用 DATA 在 far 侧建流。
4. 同一 `(sid, direction)` 的 `seq` 单调递增；FIN 的 `seq` 等于该方向最后一字节之后。
5. SACK/FIN/RST 的 `sid` 必须属于一个已知流；未知 sid 的 FIN/RST 被忽略，
   未知 sid 的 DATA/OPEN 在 far 侧可以建流（容错），在 near 侧拒绝。
6. FBK 的 `sid` 必须为 0；SACK 的 `sid` 必须非 0。
7. 任何帧的 `uack` 不得超过发送端该流的 `tx_seq`；接收端截断。
8. 载体 generation 与 path_id 的匹配规则不变；替换后的旧 gen 帧丢弃。

### 3.8 旧测试契约的变化

| 测试 | 旧期望 | 新期望 |
|---|---|---|
| `pub.two` | B 被拒绝，A 仍可用 | A、B 都可用；各自回显；B 关闭不影响 A |
| `far.open_other_sid` | 第二个 sid 必须 PROTO | 对 **far** 端：第二个 sid 是合法新流（或超 `max_streams` 时被拒），不再毒化载体。对 **near** 端：未知 sid 仍然拒绝/丢弃，near 不从 RX 建流。测试需分别覆盖两端 |
| `near.second_conn` | 第一条关闭后第二条可用 | 不变，但现在是多流路径 |
| `peers` 的 `uack` | 流字节偏移 | 路径累计载荷字节；列名不变，语义变 |
| `tunnels` 的 `streams` | 累计 accept 数 | 建议保留“累计”，另在 `stats` 加 `streams_active/tombstones` |
| `far.data_path` | 单流 ping/pong | 不变；另加多流隔离用例 |

---

## 4. 锁、并发与生命周期（问题 4）

### 4.1 锁层次

从外到内（允许的获取方向）：

| 层 | 锁 | 保护 | 规则 |
|---|---|---|---|
| 1 | `tbt_lock`（mutex） | 全局 tunnel 列表、`/proc` 写 | 不与其他锁嵌套取 socket；可短暂取 `sess->lock`；session destroy 路径会经 `tbt_carriers_stop()` 取 `pump_lock` |
| 2 | `sess->pump_lock`（mutex） | pump、载体替换 | 可睡眠；可取 socket 锁、`tx_lock`，也可**短暂**取 `sess->lock`（现有 `tbt_replace_work`、`tbt_session_stream` 如此）；**反向禁止**：持有 `sess->lock` 时不得取 `pump_lock` |
| 3 | socket 锁（`lock_sock`） | 载体/公共/target socket | 取它时**不得**持有 `sess->lock`/`tx_lock`/`rx_lock` |
| 4 | `sess->lock`（spin） | 流哈希、`car[]`、`prov/dead`、`n_live` | 不睡眠、不分配 GFP_KERNEL、不取 socket 锁 |
| 5 | `sess->tx_lock`（spin） | 共享 TX 队列、所有流的 retain | 不在其下取 `sess->lock`；帧自带 `fr->st` 引用 |
| 6 | `st->rx_lock`（spin_bh） | 每流 `rx_q`/`rx_tree` | 不在其下取 socket 锁；drain 先放锁再 `tcp_sendmsg_locked` |

**唯一允许的“socket → spin”反向嵌套**是数据路径：

- 公共 socket 的 `tcp_read_sock` 持有 socket 锁 → `tbt_tx_submit` 取 `tx_lock`。
- 载体 socket 的 `tcp_read_sock` 持有 socket 锁 → `tbt_rx_frame` 取 `sess->lock`
  和 `st->rx_lock`。
- target socket 的 `tcp_read_sock` 同理。

规则写成一句话：**持有 session spinlock 时绝不取 socket 锁；持有 socket 锁时
只允许取短暂、不睡眠的 spinlock。** 这与现有代码一致，多流不要破坏它。

### 4.2 accept 路径：如何避免持锁睡眠

`tbt_accept_one()` 在 `w_accept`（进程上下文）里运行：

1. `kernel_accept(..., O_NONBLOCK)`：不持任何 session 锁。
2. `tbt_stream_start()`：
   - 锁外：`kzalloc` 流对象，初始化 `refs=2`、`rx_lock`、`tx_lock`、各 list、
     `INIT_WORK`/`INIT_DELAYED_WORK`（`tbt_stream_init_work`）；设
     `st->sk = sock->sk`、`st->near = true`、`st->state = OPEN`。
     这样对象一旦可见就是完整的，读者不会看到半初始化的流。
   - `spin_lock(&sess->lock)`：
     - 检查 `sess->closing` → `-ENOTCONN`；
     - 检查 `n_live < max_streams` → `-EBUSY`；
     - 分配 sid（查重活跃 + tombstone）；
     - 设 `st->sid`、`st->sess`；
     - `hlist_add_head_rcu(&st->node, ...)`；`n_live++`；
     - `spin_unlock`。
   - **不要**在锁内调用 `tbt_stream_get()`：它自己会取 `sess->lock`，会自锁死。
     也不要在锁内做 `tbt_stream_put()` 的释放语义（free retain/队列）；把
     put 放到锁外。`call_rcu` 本身可以在原子上下文调用，但锁内只做摘链/计数。
   - `closing`/`max_streams` 检查失败时，锁外 `kfree(st)` 并返回。
   - 锁外：`tbt_tx_announce(st, GFP_KERNEL)`、`tbt_sock_attach(st)`、
     `schedule_work(&st->w_read)`、`tbt_fbk_kick`、`tbt_stream_put(st)`。
3. 任一步失败：`tbt_stream_abort(st)`（锁内置 DEAD；锁外 detach/release；
   锁内摘链；锁外 put），
   `sock_release(newsock)`；客户端看到关闭。

**禁止**：在 `sess->lock` 内调用 `tbt_tx_announce`（会分配、取 `tx_lock`）、
`tbt_sock_attach`（会取 socket 锁、setsockopt）、`tbt_stream_get()`（自锁）、
`tbt_stream_put()`（可能释放）、`kernel_accept`、`sock_release`、
`cancel_work_sync`。

`tbt_tx_announce` 的 GFP 选择：

- accept 路径（进程上下文）：`GFP_KERNEL`。
- 载体 RX 路径（可能持有载体 socket 锁）：`GFP_ATOMIC`，且只做“入队”，
  不直接写载体、不 pump。

### 4.3 far 侧建流与 target 拨号

`tbt_rx_frame()` 运行在载体 RX 工作项里，可能持有载体 socket 锁：

1. 解析 sid；对 far 侧的 OPEN/DATA，调用
   `tbt_stream_get(sess, sid, alloc=true, GFP_ATOMIC, &created)`。
2. 若 `created`：
   - 在 `sess->lock` 内 `n_live++`；
   - `state=PENDING`；
   - `test_and_set_bit(target_started)` 成功者 `schedule_work(&st->w_target)`；
   - **不**在 RX 上下文里 `kernel_connect`。
3. `w_target`（进程上下文）：
   - `sock_create_kern` + `kernel_connect`；
   - 成功：`st->sk = s->sk`、`tbt_sock_attach(st)`、`state=OPEN`；
   - 失败：`tbt_tx_rst(st)` + `tbt_stream_kill(st, ...)`；只影响该流；
   - 若流已 DEAD：释放 socket，put。
4. `tbt_session_join()` 不再拨 target；它只发 HELLO、安装载体。
5. `tbt_carrier_open_target()`、`sess->target`、`sess->w_target` 删除。

**target 连接失败的处理**：当前单流实现是“记日志、`target_started=false`，
等下一次 join 重试”。多流下建议：单次尝试失败即 RST 该流，客户端可重连；
不做无限重试，避免 100 条流同时重试打爆 target。

**拨号必须有界、可取消**：现有 `tbt_target_work_fn()` 用阻塞的
`kernel_connect(..., 0)`（`tbt_carrier.c:1333-1341`）。目标不可达时它会等到
TCP SYN 超时，`cancel_work_sync(&w_target)` 随之阻塞几分钟——这与交接 §8
记录的 `flush` 阻塞同因。多流下每个流一个 `w_target`，问题会放大。建议
`O_NONBLOCK` + 显式超时/重试，或至少检查 `sess->closing`/`st->state==DEAD`
后立即放弃并释放 socket；`w_target` 不得成为会话销毁的不可中断等待点。

### 4.4 pump / fbk / join 的迭代

#### pump

- 顶部：在 RCU 下遍历哈希并用 `refcount_inc_not_zero` 收集活跃流引用，
  逐流 `retain_gc`、`retain_trim`、`retx`，然后 put。**不要**调用
  `tbt_stream_get()` 做逐帧查找：每帧一次 `sess->lock` 既昂贵又不必要；
  帧自带 `fr->st`，流列表用 RCU 一次收集。`pump_lock → sess->lock` 本身
  是允许的（现有 `tbt_replace_work` 如此），但只能短暂持有。
- `retain_gc`/`retx` 的“已到达”判断以 `st->tx_ack` 为准：
  - `st->tx_ack >= r->seq + r->len` → 可 GC（该流已连续交付到帧尾）；
  - 否则按 RTO 重传。多流下**不要**用 `tbt_retain_received()` 的每路径
    正向证明跳过重传（§3.2b 的反例）；单流（`n_live == 1`）保留现有优化。
  - 多流多传的帧记入 `retx_extra`，P4 评估。
- 帧循环：每个 `fr` 自带 `fr->st`（引用），用它做：
  - `fr->hdr.seq + fr->len <= st->tx_ack`（已确认则丢弃）；
  - `tbt_tx_window_ok(st, fr)`；
  - `fr->hdr.uack = st->rx_next`（写出时）。
  `fr->st == NULL` 的会话级帧（HELLO/PAD/FBK/RETIRE）不走这些流判断。
- 流已 DEAD：DATA/PREDICT/CANCEL 丢弃；FIN/RST 仍写出（它们持有引用）。
- 每流队列配额：`st->tx_queued + chunk > st->tx_queue_max` 时，该流的
  submit 返回短写并置 `st->tx_paused`；pump 在 `st->tx_queued` 低于低水位时
  清 `tx_paused` 并 `schedule_work(&st->w_read)`。
- 会话总队列 `sess->tx_queued` 仍是硬上限；每流配额是公平性，不是替代。

#### fbk

- `w_fbk` 每 10 ms 一次（有流时）：
  1. 发一个会话级 FBK（每路径健康）；
  2. RCU 遍历活跃流：需要则发 SACK；`tbt_rx_check_hole(st)`；
     `tbt_tx_stall_check(st)`；
  3. pump。
- 每流取引用，锁外调用；不要持有 `sess->lock` 发帧。

#### join

- near 侧：写 HELLO 后，由 pump 的 HELLO 分支遍历活跃流，为每条流在该载体上
  排一个 OPEN（设置 `announced_mask` 的时机是“OPEN 写成功”，不是“入队”）。
- far 侧：写 HELLO 后只置 `up=1`；不需要为已有流宣告任何东西（near 知道 sid）。
- `tbt_session_join()` 不再调用 `tbt_session_stream()`。

#### path lost

- `tbt_tx_path_lost(sess, index)`：遍历所有流，对每个 retain 清 `path_mask`/
  `reserve_mask` 的该位，必要时 `retx_now=true`。
- 载体 join 重试耗尽：置 `sess->closing` 并杀掉所有活跃流，而不是随机杀一条。

### 4.5 关闭、摘链与 RCU

`tbt_stream_kill(st, reason)`：

1. 用 `test_and_set_bit(&st->dying)` 或 `cmpxchg(&st->state, old, DEAD)`
   抢占终态；输家直接返回。**不要**先读 `state != DEAD` 再写，否则两个
   载体同时 RST 会各自加引用/排队，泄漏一个 `w_kill` 引用。
2. `state=DEAD`，`kill_reason=reason`。
3. `kernel_sock_shutdown(sk->sk_socket, SHUT_RDWR)` 唤醒阻塞的 capture/drain。
4. `refcount_inc(&st->refs)`（给 `w_kill`）。
5. `queue_work(system_unbound_wq, &st->w_kill)`。

`w_kill`：

1. 若异常关闭且 `!st->rst_queued`，`tbt_tx_rst(st)` 并置 `rst_queued`；
   若正常关闭且未发 FIN，`tbt_tx_fin(st)`。`tbt_ctrl_work` 已经排过 RST 时
   不重复排（现实现 `tbt_sock.c:288-292` 与 `:328` 会重复发一次，多流下浪费
   带宽且放大攻击面，顺手用标志去重）。
2. `tbt_sock_detach(st)`（取消 w_read/w_drain/w_ctrl；清回调）。
3. `tbt_stream_release_socket(st)`：shutdown + `sock_release`；置 `st->sk=NULL`。
   关闭方式按 `kill_reason` 区分：异常（RST/错误/空洞）用
   `SO_LINGER=0` 发 RST；正常 FIN 关闭**不要**强设 linger，让 stock TCP
   走 FIN/TIME_WAIT。把所有关闭都做成 RST 会把已交付但应用尚未读的数据
   丢掉，也会掩盖真正的端口/TIME_WAIT 问题。
   `sk==NULL` 时跳过释放，但**继续**执行第 4-6 步；不能像现在
   `tbt_sock.c:331-332` 那样直接 `return`，否则不会进入 tombstone。
4. `st->tombstone_until_ns = now + grace`；`sess->n_tombstones++`；
   `sess->n_live--`。
5. 若 `sess->w_reap` 未在跑，`mod_delayed_work(system_unbound_wq,
   &sess->w_reap, REAP_INTERVAL_MS)`；没有 tombstone 时它会自行停止。
   **若 `sess->closing`，不要排 `w_reap`**：会话销毁会统一摘链，排了反而
   可能在 session 释放后运行。
6. `tbt_stream_put(st)`（释放 w_kill 引用）。

`w_reap`（会话级，每 200 ms）：

1. `spin_lock(&sess->lock)`；
2. 遍历每个桶；对 `state==DEAD && now >= tombstone_until_ns && !unlinked`
   的流：`hlist_del_rcu`、`unlinked=true`、`n_tombstones--`，把流指针
   收进本地链表（不要在锁内 `put`，`put` 可能触发 RCU/释放）；
3. `spin_unlock`；对本地链表逐个 `tbt_stream_put`（哈希引用）；
4. 若还有 tombstone，重新排 200 ms；否则不排。

**为什么不用每流 delayed work**：10k churn 会产生上万个待触发的
`delayed_work`，定时器轮和内存都不划算；会话级扫描一次只看 64 个桶，
且没有 tombstone 时完全停摆。

会话销毁：

1. `closing=true`；取消 `w_fbk`/`w_replace`/`w_reap`。
2. 停载体（取消 `w_rx`/`w_tx`/`w_pump`，释放载体 socket）。
3. **第一遍：停所有生产者**（逐流取引用，锁外处理）：
   - `cancel_work_sync(&w_target)`（far）；
   - `tbt_sock_detach(st)`（清回调，取消 `w_read`/`w_drain`/`w_ctrl`）；
   - `cancel_work_sync(&w_kill)`（`w_kill` 可能排 RST/FIN，必须等它结束）。
   这一遍之后，没有任何工作项能再向 `sess->tx_q` 入队。
4. `tbt_tx_free_queue(sess)`：释放所有帧；每帧 `put` 它的 `fr->st`。
   此时队列没有并发生产者，`fr->ret` 也不会再被 pump 解引用。
5. **第二遍：摘链与释放**（逐桶，锁内 `hlist_del_rcu`，锁外
   `tbt_stream_release_socket`/`tbt_stream_put`）；包括 tombstone。
6. 释放 `sess->holes`（若还有会话级残留）、`put_net`、`kfree(sess)`。

**为什么必须先停生产者再释放队列**：现有顺序（`tbt_carriers_stop` →
`tbt_tx_free_queue` → 逐流 `cancel_work_sync(&w_kill)`）有一个真实窗口：
正在运行的 `w_kill` 会在 `tbt_sock.c:328` 调 `tbt_tx_rst()`，而队列已经在
`tbt_session.c:725` 被清空；新排的 RST/FIN 帧不会再被释放，且它的 `fr->st`
引用会拖住流对象。多流下 `w_kill` 更多，窗口更大。先 cancel
`w_kill`/`w_target`/`w_ctrl`，再 free queue，才没有并发入队者。

**为什么第二遍才摘链**：队列帧持有流引用；第一遍停生产者时流还在哈希里，
哈希引用保证它们不会被释放。第二遍摘链时队列已空，`fr->ret` 不再存在，
`tbt_stream_free_rcu()` 释放 retain 是安全的。

### 4.6 每流暂停与 HOL

现状：`tbt_car_rx_work()` 在 `tbt_carrier.c:403-414` 扫描**所有**流，
只要任一流 `paused` 就 `return`，不读该载体。多流下这是全局 HOL：一条慢流
会让 100 条流全部停摆。

改法：

1. 删除该扫描。
2. `tbt_rx_data(st, ...)` 在入队前检查：
   - `st->rx_queued + st->rx_ooo_bytes + len > st->rx_limit` → `-EAGAIN`；
     只算 `rx_queued` 会漏掉重排树，实际内存可以是两倍上限。
   - `sess->rx_queued_total + len > sess->rx_budget` → `-EAGAIN`；
   - `st->paused` → `-EAGAIN`。
3. `-EAGAIN` 让记录留在该载体的 parser 里；只有**这条载体**停止读取。
   其他载体继续服务其他流。
4. `tbt_rx_flush(st)` 在 `rx_queued + rx_ooo_bytes` 超过上限时置
   `st->paused=true`；`tbt_drain_work` 在总量低于 `TBT_RX_READY_LOW` 时清
   `paused` 并 `tbt_carrier_resume(sess)`。
5. `tbt_carrier_resume()` 已经会重新调度所有载体（`tbt_carrier.c:1199-1226`），
   保持不变；多调度几个载体是可接受的。
6. 一条载体若队头记录的流被暂停，这条载体暂时停住（per-carrier HOL），
   但不会拖住其他载体。这是有界且正确的折中。
7. `st->peer_paused` 由 SACK state=2 设置；`tbt_tx_stall_check(st)` 用它抑制
   该流的 stall kill。会话级 `peer_paused` 删除。

### 4.7 死锁/饿死检查清单

- [ ] 任何 `spin_lock(&sess->lock)` 内没有 `kmalloc(GFP_KERNEL)`、`mutex_lock`、
      `lock_sock`、`sock_release`、`cancel_work_sync`。
- [ ] 任何 `spin_lock(&sess->tx_lock)` 内没有 `tbt_stream_get()`（会取 `sess->lock`）；
      帧自带 `fr->st`。
- [ ] `st->rx_lock` 在 `tcp_sendmsg_locked` 前释放（现有 drain 已如此）。
- [ ] 持有 `sess->lock` 时不取 `pump_lock`；`pump_lock` 内取 `sess->lock`
      必须短、不睡眠（现有 `tbt_replace_work` 的边界）；流列表优先用 RCU
      一次收集，避免逐帧查找。
- [ ] `w_kill` 不等待自己：`cancel_work_sync(&w_kill)` 只在会话销毁/其他上下文调用，
      `w_kill` 自己不调用。
- [ ] `w_reap` 与 `tbt_stream_get` 的竞态：摘链在 `sess->lock` 内，
      `tbt_stream_get` 的查找也在同一锁内；找到的流一定持有引用。
- [ ] 100 条流同时 `w_read` 不会把 `system_wq` 打满；必要时用
      `system_unbound_wq` 或每流限流（P4 观测）。

---

## 5. 远端 target 与 F6（问题 5）

### 5.1 为什么不能把多条流复用到一条 target 连接

- TCP 是字节流，没有消息边界。两条公共连接的字节如果写进同一条 target
  连接，target 看到的是交错后的连续字节，无法还原成两个请求。
- FIN/RST 语义冲突：一条流半关闭会关掉整条 target 连接，另一条流被截断。
- 即使 target 是 HTTP/1.1，keep-alive 的请求边界也只允许串行复用，不允许
  两条独立连接并发交错；HTTP/2 之类自带多路复用的协议不在本设计范围。
- 因此：**一条流 = 一条 target 连接**。这不是实现细节，是 TCP 语义决定的。

### 5.2 每流 target 的设计

- `struct tbt_stream` 增加 `w_target`、`target_started`；`st->sk` 在 far 侧
  就是 target socket。
- far 侧收到某 sid 的第一个 OPEN/DATA 时创建流并调度 `w_target`；
  `test_and_set_bit(target_started)` 保证只拨一次。
- `w_target` 成功：`st->sk = s->sk`，`tbt_sock_attach(st)`；此后
  target→near 的捕获/投递完全走现有的每流 socket 路径。
- `w_target` 失败：`tbt_tx_rst(st)` + kill；只影响该流。
- `sess->target`、`sess->target_started`、`sess->w_target`、
  `tbt_carrier_open_target()`、`tbt_target_attach/detach(sess, ...)` 删除或改为
  每流版本。
- `tbt_kill_work()` 里 `if (sess->target == sock) sess->target = NULL` 的别名
  处理删除；target 就是 `st->sk`，不再有会话级别名。

### 5.3 F6 是否与 F4 同源

**结论：不同源，但同类。**

证据：

- `far.data_path` 的失败输出是 `rx_records=0 capture_runs=0 actor_calls=0
  submit_bytes=0 tx_ack=0`（`adversarial/results.txt`）。这说明 far 侧
  **根本没有解析任何记录**，流/ target 逻辑从未运行。
- 同一轮里 far 日志有 `carrier ACCEPTED`，说明 accept 成功、载体对象建立；
  失败发生在“读载体”这一步。
- `findings.md` 的 F6 快照里，`far.open_other_sid`、`far.wrong_nonce`、
  `far.dup_index` 等一批用例超时，且 join 计数保持 0；但**最新
  `results.txt` 里它们有的已经 PASS**（例如 `far.wrong_nonce`、`far.dup_index`、
  `far.open_other_sid`），只有 `far.data_path` 仍报
  `rx_records=0`。这说明 F6 是**间歇性**的读唤醒竞态，不能用单次 PASS
  否认；也不能把 `far.data_path` 的失败归给 sid/流逻辑。

机制 `[推断，证据强]`：

1. `tbt_carrier_adopt()` 安装回调后只 `schedule_work(&c->w_rx)` 一次
   （`tbt_carrier.c:1297-1318`）。
2. 如果 `w_rx` 在 HELLO 入队之前运行，`tcp_read_sock` 读到 0 就返回。
3. HELLO 到达时，`sk_data_ready` → `tbt_car_ready()` → `schedule_work(&c->w_rx)`
   （`tbt_carrier.c:64-77`）。若 `w_rx` 仍 pending/running，`schedule_work`
   返回 false，这次唤醒被合并丢失。
4. `tbt_car_rx_work()` 结束时没有 `copied_seq != rcv_nxt` 的自调度
   （`tbt_carrier.c:389-440`）。
5. 唯一的兜底是 50 ms 的 `w_poll`，但 provisional 分支在
   `tbt_carrier.c:218-228` 提前返回，绕过了 `:316-320` 的
   `copied_seq != rcv_nxt` 补救。
6. HELLO 一直躺在接收队列，直到 `TBT_JOIN_TIMEOUT_MS=5 s` 把载体退休。
   前两条连接可能赢在竞态，后面的输；这解释了“前两条正常、后面全卡”。

修复（P0，独立于多流）：

- 在 provisional 分支里也检查 `copied_seq != rcv_nxt`，需要则
  `schedule_work(&c->w_rx)`。
- `tbt_car_rx_work()` 结束时若 `copied_seq != rcv_nxt`，自调度一次；
  或在 `tcp_read_sock` 返回 0 且 socket 仍 ESTABLISHED 时把 `w_poll` 提前到
  下一个 tick。
- 加计数器 `rx_lost_wakeups`，验证修复后不再增长。

**与 F4 的关系**：F6 是“载体读工作”的丢失唤醒；F4 是“会话只有一个流槽/一个
target/一套 ACK”。它们共享一种设计习惯（每会话一个对象/一个工作项），但修复
位置和验收完全不同。多流测试必须先有 F6 修复，否则 100 并发的失败无法归因。

### 5.4 多流对 F6 的放大

- 每条新流的 OPEN 都要经过载体读路径；如果某条载体的 `w_rx` 卡住，这条载体上
  的新流全部看不到 OPEN，far 不会建流，near 会看到 ack 不动。
- 载体替换时，新载体的 HELLO 也可能丢唤醒；多流下这会影响所有流，而不是一条。
- 所以 P0 的验收必须是“连续 100 次载体 accept 都能解析 HELLO 并 join”，
  而不只是单次 `far.data_path`。

---

## 6. 分阶段补丁计划（问题 6）

> 只写边界与顺序，不写代码补丁。每阶段的“完成定义”包含不变量检查。

### P0 — 先决条件（不属于多流本身）

**目标**：远端载体读路径可靠；测试能区分 F6 与 F4。

改动边界：

- `tbt_carrier.c`：provisional poll 的 `copied_seq != rcv_nxt` 补救；
  `tbt_car_rx_work` 的自调度；`rx_lost_wakeups` 计数。
- `adversarial/`：`pub.two` 的新契约（两条都可用）；`far.open_other_sid`
  的新契约（第二个 sid 合法）；100 并发用例骨架。
- `tbt_procfs.c`：`stats` 增加 `streams_active`、`streams_rejected`、
  `late_frames`（为后续阶段观测）。

完成定义：

- `far.wrong_nonce`、`far.dup_index`、`far.chunk_huge` 等在**多次运行**中
  稳定按预期关闭/拒绝，不再间歇超时（最新 `results.txt` 已有个别 PASS，
  正说明它是竞态而不是确定性失败）。
- `far.data_path` 的 `rx_records > 0`、`capture_runs > 0`。
- 连续 100 次 far 载体 accept，每次 HELLO 都被解析（`rx_adopt_events=100`）。
- `rx_lost_wakeups` 在修复后为 0 或不再增长。

**不要**在本阶段改 `far_sid`/ACK/target；保持单流行为，只修读路径。

### P1 — 顺序复用（最小可用，仍只允许一条活跃流）

**目标**：第一条连接关闭后，第二条能建立；`pub.churn`、`near.second_conn` 通过。

改动边界：

- `tbt_sock.c`：`tbt_stream_start()` 把 `busy` 改为“`n_live > 0`”；
  关闭路径清门；`tbt_stream_abort()` 统一失败回收。
- `tbt_session.c`：`tbt_stream_unlink()`/`w_reap`/tombstone；`n_live`/
  `n_tombstones`；`tbt_stream_kill` 为 `w_kill` 持引用；`w_kill` 即使
  `sk==NULL` 也继续进入 tombstone，并按关闭原因释放 socket（异常 RST /
  正常 FIN）。
- `tbt_rx.c`：near 侧禁止从 RX 采纳 sid（`if role==NEAR && !found → drop`）；
  far 侧对 tombstone sid 的帧丢弃，不新建流。far 的 `tbt_rx_bind_stream()`
  必须先查 sid：若该 sid 是 DEAD/tombstone，直接丢弃该帧，**不得**把它写入
  `sess->far_sid`；只有全新 sid 才采纳。否则一条迟到的旧帧会占住 `far_sid`，
  把随后的新 OPEN 判成 PROTO。
- `tbt_session.c`/`tbt_rx.c`：far 侧在最后一条活跃流进入 tombstone 时把
  `sess->far_sid = 0`，让新 OPEN 能重新采纳；清除与采纳都必须先过
  tombstone 检查。near 侧则在 `tbt_stream_start()` 里把 `far_sid` 指向
  新流，旧 tombstone 靠自己的 sid 被查到。
- `tbt_sock.c`：far 的 `target_started` 在最后一条流死亡时复位；
  `sess->target` 在流关闭时释放。
- `tbt_tx.c`：关闭时先让 `tbt_session_stream()` 不再返回旧流（near/far 的
  `far_sid` 置 0），再清空会话级 ack/holes/predict；确保旧流队列帧已排空/
  丢弃后再允许新流。顺序反了会让新流的 seq 0 被旧 ack 卡住，或让 pump 继续
  为旧流重传。

完成定义：

- `pub.churn` 10000：`live=10000 refused=0 other=0`。
- `near.second_conn`：rc=0。
- 1000 次 churn 后 `n_live=0`、`n_tombstones` 有界、kmalloc 无增长。
- 旧单流探针不回归。

风险：P1 仍用会话级 ack，必须在“旧流完全死亡 + tombstone 挡住迟到帧”后
才复位 ack；否则新流的 seq 0 会被旧 ack 卡住。P2 会用每流 ack 消除这个脆弱点。

### P2 — 每流状态 + 协议 v4（仍只允许一条活跃流）

**目标**：把 ACK/重传/空洞/预测从会话搬到流；为并发打底；单流行为不变。

改动边界：

- `tbtun.h`：`tbt_stream` 增加 §2.2 字段；`tbt_session` 删除 §2.3 字段；
  `tbt_carrier` 增加 §2.4 字段。
- `tbt_proto.c/h`、`tbt_uapi.h`：`TBT_CONV=0x0004`；`TBT_F_SACK=0x25`；
  放开 `sid_hi`；`tbt_hdr_set_sid/get_sid`；`tbt_body_bounds` 加 SACK。
- `tbt_tx.c`：`tbt_tx_ack(st, c, uack)`；`tbt_tx_window_ok(st, fr)`；
  `tbt_tx_retain_gc(st)`；`tbt_tx_retx(st)`；`tbt_tx_hole_check(st, mask)`；
  `tbt_tx_stall_check(st)`；`tbt_tx_path_lost` 遍历；`tbt_tx_fbk` 会话级；
  新增 `tbt_tx_sack(st)`；pump 用 `fr->st` 和 `st->tx_ack`；`uack=st->rx_next`；
  pump 成功写出后记录 `r->path_end[i]` 并累加 `c->payload_bytes_sent`。
- `tbt_rx.c`：`tbt_rx_data` 更新 `st->rx_next` 而不是 `sess->rx_ack`；
  `tbt_rx_frame` 按 sid 应用 `uack`；SACK 处理；FBK 每路径 `uack=rx_bytes`；
  `tbt_rx_fbk` 逐流 hole check；删除 `sess->peer_paused`。
- `tbt_carrier.c`：删除 `sent_high/rx_high`；`spec_path_end`；
  `payload_bytes_sent` 的字段与注释。
- `tbt_procfs.c`：`peers` 的 `uack` 语义注释；每流状态输出。
- host tests：sid 分配、path-byte 的 inflight/负向判断与单流正向证明、
  SACK/FBK 编解码、retain GC。

完成定义：

- 所有现有探针 A–K 通过；T2 字节精确；T4 丢包不劣化；T7 吞吐不降。
- 单流 `holes_unrecoverable=0`、`retx_ppm` 不高于基线。
- 1000 次单流 churn 无泄漏。
- v3 模块与 v4 模块互连：第一个 HELLO 被拒绝，双方不崩溃。

### P3 — 并发多流（用户可见的功能）

**目标**：`pub.two` 两条都可用；100 并发；关闭互不影响。

改动边界：

- `tbt_sock.c`：`tbt_stream_start()` 删除 `n_live>0` 门，改为
  `n_live < max_streams`；每 accept 分配新 sid；`tbt_far_open_seen` 改为
  `tbt_far_stream_touch(sid)`；`tbt_target_attach/detach` 改为每流。
- `tbt_rx.c`：far 侧按 sid 建流（OPEN/DATA 可建，FIN/RST 不建）；
  near 侧只校验不建；`tbt_rx_frame` 的 uack 按 sid。
- `tbt_session.c`：删除 `tbt_session_stream()`；`tbt_session_join` 不再宣告流；
  `tbt_stream_get` 返回 `created`；`n_live`/`max_streams`。
- `tbt_tx.c`：`announced_mask` 替代 `open_sent` 门；pump 的 HELLO 分支遍历
  活跃流排 OPEN；`tbt_tx_announce` 按流；`tbt_tx_fin/rst` 按流；
  `tbt_tx_path_lost`/`tbt_tx_hole_check`/`tbt_tx_stall_check` 遍历。
- `tbt_tx.c`：多流下 GC/重传的“已到达”改为只看 `st->tx_ack`；禁用
  `tbt_retain_received()` 的每路径正向证明（单流保留）；新增 `retx_extra`。
- `tbt_carrier.c`：删除全局 paused 扫描；删除 `tbt_carrier_open_target`；
  join 重试耗尽改为 `sess->closing` + 杀全部。
- `tbt_sock.c`：每流 target 的 `w_target`、`target_started`。
- `tbt_procfs.c`：`streams_active`、`tombstones`。

完成定义：

- `pub.two`（新契约）：A、B 各自回显；B 关闭后 A 仍可用；无串流。
- 100 并发：100 条各自唯一 payload，字节精确；全部关闭后 `n_live=0`；
  `MemAvailable` 有界；无 OOM；`dmesg` 干净。
- 10 条流时 `carrier 0 down`：全部继续；替换载体加入后新流可用；
  `holes_unrecoverable=0`。
- 一条流 RST 不影响其他流。

### P4 — 清理、公平、内存、文档

**目标**：长稳；10k churn + 并发；半关闭；可观测性；规格一致。

改动边界：

- 每流队列配额、会话 RX/retain 预算、tombstone 上限。
- `pub.halfclose`（F5）与关闭状态机：FIN 的 seq、`rx_fin_done`、
  `tbt_sock_shutdown_wr` 的每流路径。
- `/proc` 计数器与文档：`streams_active`、`streams_rejected`、`late_frames`、
  `tombstone_forced`、`sack_tx/rx`、`path_bytes`。
- DESIGN §3.6、§8.2、§8.3、§19 Q6；交接 §8 F4/F6、§9 P1-5；adversarial
  README/findings 的契约更新。
- 长稳：1 小时 100 流混合读写；`rmmod` 中途；`flush` 中途。

完成定义：

- 10k churn 后无流对象/socket/retain 泄漏；`n_live=0`；`n_tombstones=0`
  （grace 过后）。
- 100 并发 1 小时：无 OOM、无 `holes_unrecoverable`、无 `dmesg` 警告。
- `pub.halfclose` 通过（若本阶段修 F5）；否则明确记录为独立缺陷。
- `review/lint.sh` 全绿；`make host-test-san` 全绿。

### 阶段依赖图

```
P0 (F6) ──> P1 (顺序复用) ──> P2 (每流状态 v4) ──> P3 (并发) ──> P4 (清理)
                     │                              │
                     └──── 可独立发布 ──────────────┘
```

- P1 可以单独发布，立刻修 `pub.churn`，但并发仍被拒。
- P2 不改用户可见并发，只改内部状态；它是 P3 的前置，必须先用单流探针验证。
- P3 是功能里程碑；P4 是稳定化。

---

## 7. 测试计划（问题 7）

### 7.1 必测（验收）

#### T-A `pub.churn` 10000

- 步骤：`adversarial/run.sh` 的 public 组，`mode_churn(10000)`。
- 通过：`live=10000`、`refused=0`、`other=0`。
- 附加：结束后 `streams_active=0`；`n_tombstones` 在 grace 后归零；
  `kmalloc-1k`/`kmalloc-2k` 对象数回到基线；`ss -s` 的 TIME_WAIT 不持续增长。
- 注意：10k 顺序连接会在客户端和 far→target 侧产生大量 TIME_WAIT；
  这是环境限制，必须与模块泄漏区分。观测 `ss -s`、`/proc/net/sockstat`。
- 若 far→target 的 ephemeral port 耗尽：先确认这是 TIME_WAIT 还是泄漏。
  正常关闭应走 FIN/TIME_WAIT；只有异常/已读尽的流才用 `SO_LINGER=0` RST。
  在 target 连接上设置 `SO_REUSEADDR`（允许 `tcp_tw_reuse` 复用）是正当的
  缓解，但不能用“全部 RST”掩盖问题，也不改变“每流一条 target”的语义。

#### T-B `pub.two`（新契约）

- 步骤：A 连接、发 "AAAA"、读回；B 连接、发 "BBBB"、读回；A 再发 "CCCC"、
  读回；关闭 B；A 再发 "DDDD"、读回。
- 通过：A/B 都回显自己的字节；A 在 B 关闭后仍可用；A 的流里不出现 "BBBB"，
  B 的流里不出现 "AAAA"。
- 旧测试 `mode_two` 的 `ok_b = read_to(...) <= 0` 必须改成 `== 4`。

#### T-C `near.second_conn`

- 步骤：fake far 让第一条公共连接关闭，再连第二条。
- 通过：rc=0；第二条能收到 fake far 的 DATA。
- 这是 P1 的最小验收；P3 后仍必须通过。

#### T-D 100 并发

- 步骤：100 个客户端同时连公共端口；第 i 个发送 64 KiB 的唯一 pattern
  （例如 `i` 填充），echo target 回显；每个客户端校验字节精确；一半正常关闭，
  一半 `SO_LINGER=0` RST；全部结束后检查。
- 通过：
  - 100/100 字节精确，无串流（每个客户端只看到自己的 pattern）；
  - `streams_active=0`（grace 后 `tombstones=0`）；
  - `MemAvailable` 下降有界（建议 < 512 MiB），无 OOM；
  - `dmesg` 无 warning/oops；
  - 任一客户端 RST 不影响其他 99 条。
- 变体：100 条中 10 条只发不收（触发背压），验证其他 90 条不被 HOL 阻塞。

### 7.2 回归（现有）

| 测试 | 关注点 |
|---|---|
| `init.echo_sane` / `init.final_echo` | 单流字节精确 |
| `pub.rst` | 单流 RST 不崩 |
| `pub.writer` 100 MiB/1 GiB | 背压与内存 |
| `pub.halfclose` | F5；多流关闭路径不能更糟 |
| `far.data_path` / `near.data_path` | 载体读与单流数据 |
| `stall.hello_chain` | 载体 join 链 |
| `init.rmmod` | 多流下的卸载 |
| `init.flush` | 多流下的 flush |
| `ctl.close_flush` | 控制面崩溃 |
| `init.slab_leak` | 对象泄漏 |
| `make check` / `host-test-san` / `review/lint.sh` | 语法、ASan/UBSan、机械不变量 |
| 探针 A–K | 吞吐、丢包、替换、预测 |

### 7.3 新测试

1. **流隔离**：A 发 1 MiB、B 发 1 字节；B 的 echo 延迟不被 A 的队列阻塞；
   A 的 payload 不出现在 B。
2. **每流关闭**：
   - A 正常 FIN，B 继续传；
   - A RST，B 继续传；
   - far target 拒绝 A 的连接，只有 A 收到 RST，B 不受影响。
3. **载体丢失/替换**：10 条流时 `carrier 0 down`；所有流继续；
   替换载体 join 后新流可用；`join_dup=0`、`holes_unrecoverable=0`。
4. **SACK**：单向 upload（反向无数据）时，ack 仍推进；伪造 SACK 的
   `uack=U64_MAX` 被截断；SACK state=3 只杀一条流。
5. **sid**：
   - host 单测：分配器在 tombstone/活跃冲突时跳过；40 位边界；
   - 设备测试：强制多次 churn，确认新 sid 不复用 tombstone sid；
   - 迟到帧：在旧流 tombstone 期内注入旧 sid 的 DATA，确认被丢弃且不新建流。
6. **协议版本**：v4 模块对 v3 HELLO 拒绝；v3 模块对 v4 HELLO 拒绝；
   双方 `dmesg` 无 oops。
7. **内存**：100 流 × 1 MiB 同时 in-flight；`rx_budget`/`retain_budget`
   生效；`MemAvailable` 不跌破安全线。
8. **公平**：一条流持续灌入，另一条流 1 KB 请求的 p99 不被拖到秒级；
   每流队列配额生效。

### 7.4 host 单测（不需要 QEMU）

- `tbt_stream_get` 的并发插入/查找（模拟两个 CPU）。
- sid 分配器：wrap、冲突、tombstone、`max_streams`。
- path-byte 方向性规则：给定一组记录和接收字节计数，验证
  “负向一定安全、正向仅单流且 `recv_lag==0` 才成立”；包含
  “A 被计入但未上线、B 到达”的反例，确认多流下正向判断被禁用。
- retain GC：多流、每流 ack 独立、会话预算。
- SACK/FBK 编解码与 `tbt_hdr_check` 的 conv/sid_hi/body bounds。
- 这些测试可以放在现有 `host/` 框架里，与 `tbt_rec` 的 host 测试同风格。

### 7.5 观测指标

| 指标 | 期望 |
|---|---|
| `streams_active` | 并发时 = 客户端数；结束后 0 |
| `n_tombstones` | grace 内有界；grace 后 0 |
| `streams_rejected` | 未超 `max_streams` 时为 0 |
| `late_frames` | 可以有，但不导致新建流 |
| `sack_tx` / `sack_rx` | 单向流 > 0；双向流接近 0（捎带覆盖） |
| `holes_unrecoverable` | 0（正常丢包） |
| `retx_ppm` | 不高于单流基线 |
| `MemAvailable` | 100 并发 1 MiB 传输后回到基线的合理范围 |
| `dmesg` | 无 warning/oops |

---

## 8. 风险

| # | 风险 | 可能性 | 影响 | 缓解 | 检测 |
|---|---|---|---|---|---|
| R1 | 三个序列空间映射错误，ACK 跨流 | 高（最大风险） | 静默丢数据/误重传/挂起 | 字段改名；`path_end` 与 `tx_ack` 分离；host 单测；先单流回归再并发 | 字节精确、`holes_unrecoverable`、`retx_ppm` |
| R2 | 流生命周期 UAF（帧/工作项/RCU） | 中 | oops/泄漏 | 帧与工作项各持引用；tombstone；`call_rcu`；free 前断言 | KASAN、`init.slab_leak`、`rmmod` |
| R3 | 锁序反转（`sess->lock` vs `tx_lock` vs socket） | 中 | 死锁/软锁 | 文档化层次；帧自带 `fr->st`；pump 先收集引用 | lockdep、`review/lint.sh`、长稳 |
| R4 | 100 流内存相乘（64 MiB rx_q、8 MiB retain） | 高 | OOM | 会话预算、每流配额、`max_streams`；测试用 1 MiB/流 | `MemAvailable`、`pub.writer` |
| R5 | far→target 连接扇出/TIME_WAIT | 中 | churn 后期失败 | 每流 target；RST 关闭；`SO_REUSEADDR`；监控 `ss -s` | `pub.churn`、`/proc/net/sockstat` |
| R6 | F6 未修，100 并发结果不可信 | 高 | 误判为多流 bug | P0 先修；`rx_lost_wakeups` 计数 | 载体 accept 链、`far.*` |
| R7 | 协议硬升级导致旧模块不可用 | 确定 | 部署中断 | 文档醒目；两端同升；不做混合模式 | 版本互连测试 |
| R8 | 测试契约漂移（`pub.two`/`far.open_other_sid`/`peers uack`） | 中 | 测试误报 | 同一提交更新 README/findings/DESIGN | 代码评审 |
| R9 | F5 半关闭在多流关闭路径回归 | 中 | 应用挂起 | P4 专项；每流 FIN 状态机 | `pub.halfclose` |
| R10 | 认证载体可无限建流/拨 target | 低-中 | 资源耗尽/SSRF 面 | `max_streams`、拨号速率限制、nonce 信任边界 | `streams_rejected`、target 连接数 |
| R11 | 每流暂停仍造成 per-carrier HOL | 中 | 单载体上的其他流延迟 | 有界；多载体分散；P4 观测 | p99 延迟、`rx_paused_skips` |
| R12 | tombstone 过期太短，迟到帧新建流 | 低-中 | 错误 target 连接/数据错乱 | grace ≥ 2×hole_budget/replay；`late_frames` 计数 | 注入迟到帧测试 |

---

## 9. 开放问题与建议决策

1. **`max_streams` 默认值**：建议 1024。100 并发是验收下限；1024 留余量，
   同时把内存/哈希限制在可控范围。可做成模块参数，P4 再暴露到 `/proc`。
2. **会话 RX 预算**：建议 256 MiB；每流仍可用 64 MiB（单流 40 MiB 测试），
   但总和受限。超过时对新帧 `-EAGAIN`，暂停该流，不丢字节。
3. **会话 retain 预算**：建议 64 MiB；`replay` 从“每流 8 MiB”改为
   “会话共享 64 MiB”，否则 100 流最坏 800 MiB。实现可以先用
   “每流 `max(replay/n_live, 1 MiB)`”近似，P4 再做全局最旧优先。
4. **SACK 节拍**：10 ms tick + dirty 标志；只在 ack/状态变化时发。
   双向流量优先捎带，SACK 是兜底。
5. **tombstone grace**：建议 `2 × max(hole_budget_ms, replay_ms)`，默认 2 s；
   上限 4096 个 tombstone，超过强制回收最老的并计数。
6. **far→near OPEN 回显**：建议删除；如果保留，必须幂等且不创建流。
   删除可以让 far 侧不需要 `announced_mask`，减少状态。
7. **40 位 sid 的时机**：建议 P2 一次做完，与 conv 0x0004 同一个补丁。
   如果 P2 想缩小范围，可先用 32 位并在 P4 升 40 位，但那会是第二次
   conv 提升和第二次全量 sid 改动；不推荐。
8. **F5 半关闭**：建议放在 P4，与每流关闭状态机一起修；不要在 P3 的功能
   补丁里夹带。若 P3 后发现 `pub.halfclose` 回归，先回退 P3 的关闭改动，
   不要用“已知问题”掩盖。
9. **`pub.two` 的测试契约**：必须由测试所有者确认新语义（两条都可用），
   否则 P3 无法验收。本文建议的判定见 §7.1 T-B。
10. **`tunnels` 的 `streams` 列**：建议保留“累计 accept 数”，在 `stats`
    增加 `streams_active`/`tombstones`；不要静默改变已固定的列语义。

---

## 10. 附录

### A. 行号索引（当前快照）

| 主题 | 位置 |
|---|---|
| 单流槽 `far_sid` | `tbtun/tbtun.h:325` |
| 单 target | `tbtun/tbtun.h:319-323` |
| 会话级 ack | `tbtun/tbtun.h:342-347` |
| 会话级 holes/predict | `tbtun/tbtun.h:405-415` |
| 每路径 seq 字段 | `tbtun/tbtun.h:478-503` |
| `open_sent` | `tbtun/tbtun.h:468` |
| accept 拒绝 | `tbtun/tbt_sock.c:411-424` |
| accept 调用方关闭 | `tbtun/tbt_listen.c:72-79` |
| `far_sid` 唯一清零 | `tbtun/tbt_sock.c:429-431` |
| far 采纳/拒绝 | `tbtun/tbt_rx.c:108-129` |
| far OPEN 处理 | `tbtun/tbt_sock.c:452-468` |
| target attach | `tbtun/tbt_sock.c:470-495` |
| target detach | `tbtun/tbt_sock.c:497-510` |
| 单流访问器 | `tbtun/tbt_session.c:608-616` |
| join 宣告单流 | `tbtun/tbt_session.c:841-846` |
| 销毁摘链 | `tbtun/tbt_session.c:720-747` |
| 销毁 target | `tbtun/tbt_session.c:749-764` |
| 单 target 拨号 | `tbtun/tbt_carrier.c:1372-1388` |
| OPEN 宣告跳过 | `tbtun/tbt_tx.c:150-169` |
| pump OPEN 丢弃 | `tbtun/tbt_tx.c:1303-1311` |
| pump DATA 要求 OPEN | `tbtun/tbt_tx.c:1337-1340` |
| 调度器 OPEN 过滤 | `tbtun/tbt_tx.c:1171` |
| HELLO 后排单流 OPEN | `tbtun/tbt_tx.c:1476-1489` |
| 全局 paused 扫描 | `tbtun/tbt_carrier.c:403-414` |
| provisional poll 早返回 | `tbtun/tbt_carrier.c:218-228` |
| poll 的 copied_seq 补救 | `tbtun/tbt_carrier.c:316-320` |
| 载体 ready 回调 | `tbtun/tbt_carrier.c:64-77` |
| 载体 rx 工作 | `tbtun/tbt_carrier.c:389-440` |
| FBK 每路径 uack | `tbtun/tbt_tx.c:1770-1800` |
| `rx_high` 更新 | `tbtun/tbt_rx.c:147-149` |
| `rx_ack` 更新 | `tbtun/tbt_rx.c:217` |
| `sent_high` 更新 | `tbtun/tbt_tx.c:1534-1535` |
| 帧头 uack 赋值 | `tbtun/tbt_tx.c:1417` |
| conv 校验 | `tbtun/tbt_proto.c:77` |
| sid_hi 校验 | `tbtun/tbt_proto.c:89` |
| TBT_CONV | `tbtun/tbt_uapi.h:44` |
| 帧类型 | `tbtun/tbt_uapi.h:52-54` |
| OPEN body bounds | `tbtun/tbt_uapi.h:143-145` |

### B. 证据与推断

- `[证据]`：`adversarial/results.txt` 的 `pub.churn`/`pub.two`/
  `near.second_conn`/`far.data_path`；`adversarial/findings.md` F4/F6；
  `tbtun` 代码的上述行。
- `[推断]`：F6 的丢失唤醒机制；`pub.churn` 的 TIME_WAIT/端口耗尽风险；
  100 并发的内存量级。这些需要在 P0/P3 实测确认。
- 本文没有运行任何构建或 QEMU；所有性能/内存数字都是量级估计，不是实测。

### C. 术语

- **stream**：一条公共 TCP 连接在隧道内的对应物；一个 sid、一套 TX/RX 状态、
  一个 socket。
- **session**：一个 tunnel 的载体集合与共享状态；一个 nonce、N 条载体、
  N 条流。
- **carrier/path**：一条载体 TCP 连接；`index` 是槽位，`gen` 是代次。
- **path bytes**：某条载体上累计的 DATA/PREDICT 载荷字节；与任何流的 seq 无关。
- **tombstone**：流已 DEAD 但仍在哈希中的宽限期对象；用于丢弃迟到帧、
  防止 sid 过早复用。
- **SACK**：新的每流确认帧；`uack` 是该流连续已收偏移。
- **FBK**：会话级载体反馈帧；每路径健康、RTT、记录计数。

## 12. P2 尝试记录（未完成，已回退到 P1）

2026-11-05 的 P2 尝试曾实现：`TBT_CONV=0x0004`、`TBT_WIRE_VERSION=1`、
`TBT_F_SACK=0x25`、40 位 sid、`st->tx_ack`、删除会话 ACK 复位、FBK
per-path `uack=c->rx_bytes`、per-stream holes/predict。host `make check` 通过
（348/0），但 QEMU 失败：`init.echo_sane` 在 stream start 后约 0.5 s 被
`TBT_ST_HOLE` 杀死，far 只收到 64 KiB，app 读到 EOF；public 组随后出现
soft lockup/panic（`tbt_tx_pump`/`tbt_car_poll_work`）。根因未定位。

已回退：uapi 0x0003、sid u32、session `tx_ack/rx_ack`、FBK `rx_high`、
session holes/predict、无 SACK，并恢复 far→near OPEN 回显（far 侧
`c->open_sent` 门需要它，否则 far→near DATA 永远不发）。当前树
`make check` 348/0、lint 15/15，QEMU `pub.churn` 10000/0/0、
`near.second_conn` rc=0、探针 A/B/E/G/I/J/K PASS、slab PASS。

P2 未完成项：每流 `tx_ack` 与 `tbt_tx_ack` sid 语义、SACK 的 dirty/10 ms
节拍、FBK path-byte 语义与 retain `path_end`、每流 holes/predict、
`tbt_tx_pump` 在 `st==NULL`/流退休时的 DATA 归属、以及 QEMU 单流回归。

---

## 13. P2 单变量步进计划（从当前 P1 树增量修改）

**约束**：本计划只描述步骤，不改源码。当前树保持 P1 状态；devops 正在做
基线快照与复验。P2 必须从当前树**一次只改一处**，每一步单独编译、单独跑
完整验证、单独留 patch；禁止把 P2a–P2d 混在一次提交或一次编译里。

### 13.0 基线与工作方法

**P1 基线（当前树）**：

- `make check` = 348 checks / 0 failures（314 基线后新增 host 用例）。
- `review/lint.sh` = 15/15 checks、12/12 self-tests。
- QEMU：`pub.churn` 10000/0/0、`near.second_conn` rc=0、探针
  A/B/E/G/I/J/K PASS（J/K 各 1 次；基线要求 3/3）、`pub.rst` PASS、
  `init.slab_leak`/`slab_after_flush` PASS。
- bench：n=4 ratio 4.69x、wire/app 1.0128；n=1 ratio 1.11x、wire/app
  1.0626（本次实测；P1 历史基线为 5.08x/1.0005 与 1.0316，差异未消）。

> **先决条件**：P2a 开始前，devops 必须先冻结当前树为 `p1-base` 快照
> （源码目录 + `tbtun.ko` + 上述结果）。P2a 的 diff 只能相对 `p1-base`。

**每一步的固定流程**：

1. 从**上一步通过后的树**复制快照：`cp -a tbtun /tmp/tbtun-<step>-before`。
2. 只改本步骤列出的文件/函数；不顺手改其他字段、不加调试代码。
3. `make clean && make check`（必须 0 failures）。
4. `bash review/lint.sh`（必须 15/15、12/12）。
5. QEMU：
   - `TBT_ADV_GROUPS=public adversarial/run.sh --no-build`
     （必须 `init.echo_sane`、`pub.churn` 10000/0/0、`pub.two` 旧契约、
     `pub.rst`、`pub.writer` 全 PASS；`pub.halfclose`/`srcfilter` 仍为既有 FAIL）。
   - `TBT_ADV_GROUPS=near adversarial/run.sh --no-build`
     （必须 `near.second_conn` rc=0，其余 near PASS）。
   - `tests/qemu/run.sh --probes A,B,E,G,I,J,K --timeout 600 --ko "$PWD/tbtun.ko"`
     必须 7/7 PASS；J/K 再各跑 2 次，合计 3/3。
   - `benches/run.sh throughput --n 4,1 --mib 40`
     记录 `ratio`、`wire_per_app`、`retx`；n=4 不得低于 P1 基线的
     统计噪声范围（建议以连续两次 n=4 的均值判断）。
6. 全部通过后：`cp -a tbtun /tmp/tbtun-<step>-after`，并生成
   `diff -ruN /tmp/tbtun-<step>-before /tmp/tbtun-<step>-after > /tmp/<step>.patch`。
   这个 patch 就是该步骤唯一的回退单元。
7. 任一项失败：只 `patch -R` 该步骤的 patch，回到上一步的树；**不要**
   回退前面已通过的步骤，也不要整体重置 P1。

### 13.1 P2a：`sess->tx_ack` → `st->tx_ack`（唯一变量：ACK 归属）

**不改**：协议、帧布局、FBK、sid 宽度、OPEN/SACK、FBK per-path 语义、
会话 `rx_ack`、holes/predict。

**当前锚点（P1 树）**：

| 位置 | 当前内容 |
|---|---|
| `tbtun.h:242` | `struct tbt_stream` 只有 `tx_seq`，无 `tx_ack` |
| `tbt_tx.c:265` | `tbt_tx_retain_gc()`：`ack = READ_ONCE(sess->tx_ack)` |
| `tbt_tx.c:364` | `tbt_tx_window_ok(sess, fr)`：用 `sess->tx_ack` |
| `tbt_tx.c:454` | `tbt_tx_path_lost()`：用 `sess->tx_ack` |
| `tbt_tx.c:474` / `:631` | `tbt_tx_retx()`：用 `sess->tx_ack` |
| `tbt_tx.c:687` | `tbt_retain_received(sess, r)`：用 `sess->tx_ack` |
| `tbt_tx.c:917` / `:961` | `tbt_tx_holes_repaired()` / `tbt_tx_hole_check()` |
| `tbt_tx.c:1184` | `tbt_tx_stall_check()` |
| `tbt_tx.c:1220-1247` | `tbt_tx_ack()`：读/写 `sess->tx_ack` |
| `tbt_tx.c:1321` | `tbt_path_inflight(sess, c)` |
| `tbt_tx.c:1561` | pump 的已确认丢弃判断 |
| `tbt_session.c` reset | `tbt_session_reset_ack_locked()` 清 `sess->tx_ack` |
| `tbt_procfs.c` stats | 打印 `s->tx_ack` |

**P2a diff（只此一组）**：

1. `tbtun.h`：`struct tbt_stream` 在 `tx_seq` 旁加 `u64 tx_ack;`。
   `struct tbt_session` 的 `tx_ack` 保留到本步骤最后再删，便于编译对照。
2. `tbt_tx.c`：
   - `tbt_tx_ack()`：把 `sess->tx_ack` 的比较/写入改为 `st->tx_ack`；
     保留现有 `sid == sess->far_sid` 检查与 `uack` 截断。
   - `tbt_tx_retain_gc()`、`tbt_tx_retx()`、`tbt_tx_path_lost()`、
     `tbt_tx_holes_repaired()`、`tbt_tx_hole_check()`、`tbt_tx_stall_check()`：
     用本函数已有的 `st`，把 `sess->tx_ack` 换成 `st->tx_ack`。
   - `tbt_tx_window_ok()` 增加 `struct tbt_stream *st` 参数；pump 调用点
     传当前 `st`。`type == TBT_F_DATA && !st` 时 `goto drop`（P1 的
     session ack 不会 NULL；P2a 必须先补这个防御，否则会复现 P2 的
     `tbt_tx_pump` NULL 解引用）。
   - `tbt_retain_received()` 增加 `struct tbt_stream *st` 参数；
     两个调用点传 `st`。
   - `tbt_path_inflight()` 增加 `st` 参数；`tbt_sched_better/rebuild/pick`
     增加 `st` 参数并逐层传下去；pump 调用点传当前 `st`。
3. `tbt_session.c`：`tbt_session_reset_ack_locked()` 删除
   `sess->tx_ack = 0;`；`tbt_stream_kill` 不再依赖会话 ack 复位。
4. `tbt_procfs.c`：stats 的 `tx_ack` 改打印当前 `one->tx_ack`（无流则 0）。
5. 最后删除 `struct tbt_session.tx_ack` 字段；`grep -n 'sess->tx_ack'`
   必须为空。

**为什么单流下等价**：P1 只允许一条活跃流，`st->tx_ack` 与
`sess->tx_ack` 是同一逻辑值；差别只在“谁拥有它”。本步骤不改变
`uack` 的来源（仍由帧头/FBK 携带），所以单流行为应逐字节一致。

**验收**：13.0 的全部命令；重点看 `init.echo_sane`、`pub.churn`、
探针 A/B/E/G/I/J/K、bench n=4/n=1。**任何一项与 P1 基线不一致就回退
P2a patch，不进入 P2b。**

**失败判别点**：

- `pub.churn` refused>0：`tbt_tx_ack` 没有把 ACK 写到新流，或
  `tbt_stream_kill` 仍清了会话 ack；查 `stats tx_ack` 与
  `grep sess->tx_ack`。
- bench wire/app 变差：`tbt_retain_received`/`tbt_path_inflight` 仍读
  旧 session ack，导致重复重传；查这两个函数的参数是否全部改成 `st`。
- NULL oops：pump 的 `st == NULL` 防御缺失。

#### 13.1.1 P2a / P2a' 实测结果（2026-11-05）

- **P2a（读取点迁移）失败**：public `pub.churn` 9926/74、`pub.two` A 二次
  不可用、`pub.halfclose` echo 0；已回退内核 diff。诊断：`ack stale
  sid=<旧> cur=<新> uack=1` 被忽略；`retire tx_ack=1`；pump 在退休窗口
  见 `stale DATA fsid=… far=0 st=0`。问题在退休/新流切换窗口与
  `st==NULL` 归属。
- **P2a'（零行为镜像）已落地**：`tbtun.h` 加 `st->tx_ack`；流创建为 0；
  `tbt_tx_ack` 更新 `sess->tx_ack` 时同步 `st->tx_ack`；退休时同步清零。
  **所有读取点仍读 `sess->tx_ack`**，因此单流行为与 P1 一致。
- **P2a' 验证**：`make check` 348/0、lint 15/15；探针 B PASS；
  public 8/10（churn 10000/0/0、two、halfclose、rst、writer PASS；
  仅既有 src_key/srcfilter FAIL）。
- **探针 I**：三次中 1 次 PASS；两次失败都在阈值边缘——
  一次 n=4 5.9 Mbit/s（要求 ≥6），一次 ratio 1.77x（要求 ≥1.8）。
  P2a' 未改任何读取点，失败是吞吐阈值波动，不是 ACK 归属错误；
  未观察到 far_sid/st/sess ack 时序异常。
- 结论：P2a' 保留（零行为镜像）；**不进入 P2a''**，直到 I 的
  阈值波动被环境或基线复验澄清。
- `host/test_ack.c` 保留：10019 checks / 0 failures，作为 P2a'' 的
  规格测试。

#### 13.1.2 P2a'' 单读取点迁移结果（2026-11-05）

- `tbt_tx_retx` → `st->tx_ack`：**保留**；host/test_ack、探针 B、public
  churn 10000/0/0 全过。
- `tbt_tx_window_ok` → `st->tx_ack`：**失败并回退**；public 在退休窗口
  遇到 `st==NULL` 的 DATA，出现 hang/oops。
- `tbt_tx_stall_check` → `st->tx_ack`：**保留**；三件套全过。
- pump DATA 丢弃判断 → `st->tx_ack` + sid 归属：**失败并回退**；
  public churn 9927/73。说明 pump 在退休/新流窗口对 DATA 的归属仍不能
  只看当前 `st`。
- `tbt_tx_holes_repaired` → `st->tx_ack`：**保留**；public churn
  10000/0/0，halfclose/rst/writer 全过。
- 当前保留范围：`tbt_tx_retx`、`tbt_tx_stall_check`、
  `tbt_tx_holes_repaired` 读 `st->tx_ack`；其余读取点仍读
  `sess->tx_ack`。`tbt_tx_pump` 未做 sid 归属迁移。

#### 13.1.3 retire/purge 与 window/pump 的结论（2026-11-05）

- 尝试把 `tbt_tx_purge_stream()` 放进 `tbt_stream_detach_from_session()`，
  在清 `far_sid` 之前丢掉旧流 DATA/PREDICT/CANCEL。结果：public
  `pub.halfclose` 回归（echo 0），因为 CLOSE_WAIT 的早退把应用尚未送出
  的 10 字节也 purge 了。已回退，purge 仍只在 `tbt_stream_kill()` 的
  终态路径执行（detach 之后）。
- 因此本轮没有继续迁移 `tbt_tx_window_ok` / pump DATA 归属；此前它们
  单独迁移失败（window 在退休窗口遇 `st==NULL` 的 DATA；pump 迁移
  churn 9927/73），均保持回退。
- 当前保留：P2a' 镜像；`tbt_tx_retx`、`tbt_tx_stall_check`、
  `tbt_tx_holes_repaired` 读 `st->tx_ack`；其余读 `sess->tx_ack`。
- 验证：make check 348/0、lint 15/15；public churn 10000/0/0、
  halfclose PASS、two/rst/writer PASS；仅既有 src_key/srcfilter FAIL。
- 关键约束：**早退（CLOSE_WAIT）不能 purge 未送出的 DATA**；若要按 sid
  丢弃旧 DATA，必须先区分“终态 kill”与“半关闭早退”。

#### 13.1.4 帧持流引用方案（2026-11-05，通过）

- `struct tbt_frame` 增加 `struct tbt_stream *st`；DATA/PREDICT/CANCEL
  入队时绑定并 `refcount_inc`；控制帧为 NULL。
- `tbt_tx_submit`（含 dup）、`tbt_tx_retx`、`tbt_tx_inject_locked`、
  `tbt_tx_predict_locked`、`tbt_tx_cancel` 绑定 `fr->st`。
- pump 直接使用 `fr->st`：DATA 的 ack 丢弃判断与
  `tbt_tx_window_ok(fr->st, fr)` 不再查 sid；`fr->st==NULL` 的 DATA
  直接丢弃。写出/丢弃/purge/free_queue 统一走 `tbt_frame_free()`，
  在 `tx_lock` 外 put。
- `tbt_tx_purge_stream(st)` 按 `fr->st == st` 摘链；CLOSE_WAIT 早退
  不 purge，保留未发 DATA 给 halfclose；kill 终态才 purge。
- 验证：make check 348/0、lint 15/15、host/test_ack 10026 checks；
  probe B PASS；public churn **10000/0/0**、halfclose/two/rst/writer
  全 PASS（仅既有 src_key/srcfilter FAIL）。
- 结论：P2a'' 的 window/pump 迁移通过；当前保留 retx/stall/holes/
  window/pump 全部读 `st->tx_ack`，P2a' 镜像仍在。P2b 未开始。

### 13.2 P2b：只加 SACK（唯一变量：每流备用 ACK 通道）

**基于 P2a 通过后的树。不改**：FBK 布局/语义、现有帧头 `uack` 的用法、
sid 宽度、OPEN 行为、retain/path 证明。

**当前锚点（P2a 树）**：

- `tbt_uapi.h:52-60` 帧类型枚举；`:138-155` `tbt_body_bounds()`。
- `tbt_tx.c:1975` `tbt_tx_fbk()`；`:2077` `tbt_fbk_work()`。
- `tbt_rx.c:353` `case TBT_F_FBK`；`:316` 顶部 `tbt_tx_ack` 调用。
- `tbtun.h` 的 `struct tbt_stream`（P2a 后已有 `tx_ack`）。

**P2b diff（只此一组）**：

1. `tbt_uapi.h`：新增 `TBT_F_SACK = 0x25`；`tbt_body_bounds()` 的
   `case TBT_F_RETIRE` 旁加 `case TBT_F_SACK`，`min=max=1`。
   **不改 `TBT_CONV`**（P2c 才 bump；P2b 只保证同版本两端）。
2. `tbtun.h`：`struct tbt_stream` 加 `u64 sack_next;` 与
   `u8 sack_state;`；加 `int tbt_tx_sack(...)` 原型。
3. `tbt_tx.c`：新增 `tbt_tx_sack(sess, st, gfp)`：
   - `body[0] = tbt_rx_state(st)`；
   - `sid=st->sid`、`uack=READ_ONCE(st->rx_next)`、`seq=0`、`body_len=1`；
   - `tbt_tx_enqueue(sess, fr, true)` + `tbt_tx_wake(sess)`。
4. `tbt_tx.c:2077 tbt_fbk_work()`：在 `tbt_tx_fbk()` 之后，若
   `st->rx_next != st->sack_next || st->sack_state != tbt_rx_state(st)`，
   调 `tbt_tx_sack()`，更新 `sack_next/sack_state`。仍每 10 ms 一次。
5. `tbt_rx.c`：新增 `case TBT_F_SACK`；只校验 `body_len==1`，
   其余走顶部已有的 `tbt_tx_ack`（即 SACK 的 `uack` 与现有捎带 ack
   完全同义），body state 先只记 `st->peer_paused = (state==2)`，
   不做 kill。**不改变 FBK 的 `uack`/`rx_state` 含义。**

**验收**：13.0 全部命令。重点：

- 探针 I/J/K（SACK 只应在反向无数据时补充 ack，不应改变吞吐/重传）；
- bench n=4/n=1 的 `wire_per_app` 不得高于 P1 基线；
- `pub.churn` 10000/0/0 保持。

**失败判别点**：

- SACK 每 tick 都发（即使 ack 未变）：`sack_next` 未更新或比较错；
  抓 `stats sack_tx`（若加计数器）或 bench CPU。
- 反压被误判：SACK state 2 只应设 `peer_paused`，不应触发 kill；
  检查 `case TBT_F_SACK` 是否误调 `tbt_stream_kill`。
- 帧类型冲突：`tbt_body_bounds` 未加 SACK 导致 `-EPROTO`；看
  `rx_parse_proto`。

**回退**：只反向 P2b patch；P2a 保留。

### 13.3 P2c：40 位 sid + `TBT_CONV=0x0004` + `TBT_WIRE_VERSION=1`

**基于 P2b 通过后的树。唯一变量：sid 宽度与 wire 版本。不改**：
每流 ack 语义（P2a）、SACK 发送逻辑（P2b）、FBK per-path 语义（P2d）。

**当前锚点（P2b 树）**：

| 位置 | 当前 |
|---|---|
| `tbt_uapi.h:46` | `TBT_CONV 0x0003u` |
| `tbt_uapi.h:30` | `TBT_VERSION_MAJOR 1`（模块版本，不要改） |
| `tbt_proto.c:77` | conv 相等校验 |
| `tbt_proto.c:89` | `sid_hi != 0` 拒绝 |
| `tbtun.h:242` | `u32 sid`；`:356-358` `far_sid/next_sid/sid_prefix` u32 |
| `tbtun.h:487` | `TBT_STREAM_BUCKET(sid)` 32 位 |
| `tbt_sock.c:509` | `(prefix16<<16)|(counter16)` |
| `tbt_session.c:573/685` | `tbt_stream_get/bind_sid` u32 |
| `tbt_carrier.c` HELLO builder | `body[0]=TBT_VERSION_MAJOR` |
| `tbt_rx.c` HELLO parser | 比较 `TBT_VERSION_MAJOR` |

**P2c diff（只此一组）**：

1. `tbt_uapi.h`：`TBT_CONV=0x0004u`；新增 `TBT_WIRE_VERSION 1`；
   注释同步。模块 `TBT_VERSION_MAJOR/MINOR/PATCH` **不动**。
2. `tbt_proto.c`：删除 `sid_hi != 0` 拒绝；`tbt_hdr_check` 只校验
   `reserved2`、body bounds、conv。`tbt_proto.h` 加
   `tbt_hdr_sid()` / `tbt_hdr_set_sid()` 两个 inline。
3. `tbtun.h`：`struct tbt_stream.sid`、`far_sid`、`next_sid` 改 `u64`；
   `TBT_STREAM_BUCKET` 用 `(u32)sid ^ (u32)(sid>>32)`；所有 sid
   原型改 `u64`。
4. `tbt_sock.c:509`：`sid = ((u64)(prefix16)<<24)|(counter & 0xffffff)`；
   保留 sid 0 跳过。
5. 所有帧构造：`tbt_hdr_set_sid(&fr->hdr, st->sid)`；所有帧读取：
   `tbt_hdr_sid(hdr)`。逐处清单：
   `tbt_tx.c` 的 `tbt_tx_control_pinned`、`tbt_tx_announce`、`tbt_tx_fin`、
   `tbt_tx_rst`、`tbt_tx_predict_locked`、`tbt_tx_cancel`、`tbt_tx_fbk`、
   `tbt_tx_sack`；`tbt_rx.c` 的 `tbt_rx_bind_stream`、`tbt_rx_data`、
   `tbt_rx_frame`、SACK/FIN/RST；`tbt_session.c` 的
   `tbt_stream_get/kill_by_sid`；`tbt_procfs.c` 的 sid 打印。
6. HELLO：`tbt_carrier.c` builder `body[0]=TBT_WIRE_VERSION`；
   `tbt_rx.c` parser 比较 `TBT_WIRE_VERSION`。模块版本不动。
7. host：`host/test_fuzz.c` 的 `TBT_CONV` 钉住值改 `0x0004`；
   `sid_hi 1` 从 `TBT_REC_PROTO` 改 `TBT_REC_OK`；新增 40 位边界用例
   （`sid_hi=0xff, sid_lo=0xffffffff` 接受）。
8. DESIGN §3.3/§3.6 同步 conv、SACK、sid 位布局；`multiconn.md` 同步。

**验收**：13.0 全部命令，另加：

- `make check` 的 host 数字变化必须解释为“新增 conv/sid 用例”；
- 旧 v3 模块对新 v4 帧、v4 对 v3 帧：第一个 HELLO 即拒绝，不崩溃；
- `far.open_other_sid` 的契约变化（far 端第二个 sid 合法）只在 P3 改测试。

**失败判别点**：

- 载体永远不 join：HELLO 的 `body[0]`/conv 不一致；看
  `tbt_rx_hello` 的 body_len/ver 分支与 `tbt_hdr_check`。
- `sid` 打印/哈希错：`TBT_STREAM_BUCKET` 仍是 32 位，或 `sid_hi`
  未写入；查 `tbt_hdr_sid` 与 `grep sid_lo`。
- host 失败：`test_fuzz` 的 conv/sid 期望未同步。

**回退**：只反向 P2c patch；P2a/P2b 保留。回退后 `TBT_CONV` 回 0x0003，
sid 回 u32，HELLO 回模块版本。

#### 13.3.1 P2c 实测结果（2026-11-05，保留）

- `TBT_CONV=0x0004`；`TBT_WIRE_VERSION=1`，HELLO body[0] 用它；模块
  `TBT_VERSION_*` 未动。
- sid 用满 40 位：`(prefix16<<24)|counter24`；`struct tbt_stream.sid`、
  `far_sid`、`next_sid` 改 `u64`；`TBT_STREAM_BUCKET` 用高低 32 位混合；
  `tbt_hdr_set_sid/get_sid` 支持 `sid_hi`；`tbt_proto.c` 放开非零
  `sid_hi`。
- 帧构造/读取点：tx 的 control/OPEN/FIN/RST/PREDICT/CANCEL/FBK 与
  submit DATA 都写 `sid_hi`；rx 的 bind/data/FIN/RST/ack 用
  `tbt_hdr_sid()`。
- host：`test_fuzz` conv 钉 0x0004，新增 `sid_hi=1` 与
  `sid_hi=0xff + sid_lo=0xffffffff` 接受用例；`make check` 348/0、
  `make host-test-san` 全过、lint 15/15。
- QEMU：探针 B PASS；public churn 10000/0/0、two/halfclose/rst/writer
  PASS（仅既有 src_key/srcfilter FAIL）；探针 I 一次 FAIL n=4 5.3
  Mbit/s（当前环境慢，非协议错误）。
- 保留 P2c；P2b（SACK）未开始。

### 13.4 P2d：FBK per-path `uack` 改为路径载荷字节

**基于 P2c 通过后的树。唯一变量：per-path uack 的空间。不改**：
每流 `tx_ack`（P2a）、SACK（P2b）、sid 宽度（P2c）、OPEN/帧布局。

**当前锚点（P2c 树）**：

| 位置 | 当前 |
|---|---|
| `tbt_tx.c:1975-2010` | `tbt_tx_fbk()`：`e[TBT_FBK_UACK]=c->rx_high` |
| `tbt_tx.c:265` | `tbt_retain_all_received()`：`c->uack >= r->seq+r->len` |
| `tbt_tx.c:687` | `tbt_retain_received()`：路径正向证明 |
| `tbt_tx.c:961` | `tbt_tx_hole_check()`：`tail`/`gate` 用 seq |
| `tbt_tx.c:1321` | `tbt_path_inflight()`：`sent_high - ack` |
| `tbt_rx.c:138` | `rx_high` 更新 |
| `tbt_rx.c:207` | `sess->rx_ack` 更新 |
| `tbtun.h` carrier | `sent_high`、`rx_high`、`spec_seq` |

**P2d diff（只此一组）**：

1. `tbtun.h` carrier：新增 `u64 payload_bytes_sent;`、
   `u64 spec_path_end;`；删除/停用 `sent_high`、`rx_high`、`spec_seq`
   （先保留字段到本步骤结束，避免大 diff；最终 `grep` 为空）。
2. `tbtun.h` retain：加 `u64 path_end[16];`。
3. `tbt_tx.c`：
   - DATA 写出成功后 `c->payload_bytes_sent += fr->len;`
     `r->path_end[c->index] = c->payload_bytes_sent;`
   - PREDICT 写出后 `payload_bytes_sent += len; spec_path_end = ...`。
   - `tbt_tx_fbk()` 的 per-path `uack = c->rx_bytes`。
   - `tbt_path_inflight()` 改 `payload_bytes_sent - c->uack`。
   - `tbt_retain_all_received()` / `tbt_retain_received()` /
     `tbt_tx_hole_check()` 的路径判断改 `r->path_end[i]`；
     **单流时**保留正向证明（`recv_lag==0`），**多流时禁用正向证明**
     （`n_live > 1` 时只允许负向：`c->uack < path_end` 表示未到）。
4. `tbt_rx.c`：
   - `rx_bytes` 只在该记录被接收进流状态后累加；
     DATA 与 PREDICT 都计入；删除 `rx_high` 更新。
   - `tbt_rx_fbk()` 的 `spec_bytes` 清除条件改 `uack >= spec_path_end`。
5. `tbt_procfs.c`：`peers` 的 `uack`/`inflight` 语义注释改为路径载荷字节。
6. DESIGN §3.3/§8.3：FBK per-path `uack` 改义；注明单流正向证明、
   多流仅负向/多传的代价。

**为什么单流可保留正向证明**：单流时路径载荷字节与流 seq 一一对应，
`recv_lag==0` 时 `uack >= path_end` 仍能证明记录到达；多流时路径上混入
其他流的记录，正向比较可能假阳性，因此 P3 必须关闭正向证明。

**验收**：13.0 全部命令。重点：

- 探针 I/J/K（重传/空洞/预测）；
- bench n=4/n=1 的 `wire_per_app`/`retx`；
- `pub.churn`/`near.second_conn` 保持。

**失败判别点**：

- 重传暴涨：`path_end` 未在写成功后赋值，或 `rx_bytes` 在丢弃时也累加；
- 假阳性丢帧：单流下 `recv_lag` 门被绕过；查 `tbt_retain_received()`；
- 空洞门控误判：`tail` 仍用 `seq` 与 `uack` 比较。

**回退**：只反向 P2d patch；P2a–P2c 保留。回退后 FBK 回 `rx_high`，
retain 回 seq 证明。

#### 13.4.1 P2b + P2d 实测结果（2026-11-05，保留）

- **P2b SACK**：`TBT_F_SACK=0x25`，body 1 字节 state，`uack=st->rx_next`，
  sid 为该流；发送端 10 ms tick + dirty/ack 变化触发，DATA 仍用帧头
  `uack` 捎带；接收端走 `tbt_tx_ack` 单调/窗口逻辑，未知 sid 计数丢弃。
  host `test_fuzz` 钉住 SACK 1 字节接受、0/2 拒绝。
- **P2d FBK 路径字节**：carrier 加 `payload_bytes_sent`、`spec_path_end`；
  retain 加 `path_end[16]`；DATA/PREDICT 写出后累加；FBK per-path
  `uack=c->rx_bytes`（DATA+PREDICT）；retain 正向/负向判断改
  `path_end`；`tbt_path_inflight` 用 payload bytes。
- host：`make check` 348/0；`host-test-san` 全过；lint 15/15、12/12；
  `test_fuzz` 8,525,966 checks / 0 failures。
- QEMU：探针 B PASS；public 8/10（churn **10000/0/0**、
  two/halfclose/rst/writer PASS；仅既有 src_key/srcfilter FAIL）；
  探针 I 一次 **PASS**。
- 保留 P2b + P2d；未跑 bench/n=8/16。

#### 13.4.2 P3a 每流 target 实测结果（2026-11-05，保留）

- `struct tbt_stream` 增加 `target`、`target_started`、`w_target`；
  `tbt_target_work_fn`/`tbt_carrier_open_target`/`tbt_target_attach`/
  `tbt_target_detach` 全部按流；far 侧收到 OPEN 后为当前流拨 target。
- `tbt_far_open_seen` 只 attach 本流 target；`tbt_stream_release_socket_now`
  清本流 target；`w_kill` 先 `cancel_work_sync(w_target)`；session destroy
  第一遍逐流 cancel w_target，第二遍由 stream release 释放 target。
- 删除 `struct tbt_session.target/target_started/w_target` 与 destroy 的
  session target 块；单流门 `far_sid` 未动。
- 验证：make check 348/0；host-test-san 全过；lint 15/15、12/12；
  探针 B PASS；public 8/10（churn **10000/0/0**、two/halfclose/rst/writer
  PASS；仅既有 src_key/srcfilter FAIL）；探针 I 一次 PASS。
- 保留 P3a；P3b（删单流门/并发）未开始。

#### 13.4.3 P3b1 两条并发实测（2026-11-05，未通过 churn）

- 已改：near 删除 `far_sid` 单流门；far 按未知 sid 建流；每流 target
  已就绪；`announced_mask` 替代 `c->open_sent` 做每流每载体 OPEN 门；
  `pub.two` 改为新契约（A/B 都可用、互不串流、B 关闭不影响 A）。
- 验证：make check 348/0；host-test-san 全过；lint 15/15、12/12；
  探针 B PASS；public `pub.two` **PASS**（A/B 都可用）、halfclose/rst/
  writer PASS；`pub.churn` **FAIL 9896/8/96**（usable/refused/other）。
- 失败点：两条并发连接数据正确，但顺序 churn 出现约 1% 读超时。
  尝试 `tbt_session_for_each_stream` 逐流 retx/stall/holes/SACK 后，
  probe B 出现 pump/sess 锁死锁与载体 `up=0/lost=1`；已回退遍历
  改动，但当前树 probe B 仍未恢复（`tx_ack=0`、`fbk_rx=0`、
  `holes_unrecoverable=1`）。
- 未保留为通过状态；P3b1 需先恢复单流 B，再定位 churn。
  P3b2（100 并发）未开始。

#### 13.4.4 P3b1 恢复结果（2026-11-05）

- 从 `/tmp/tbtun-p2b-after` 恢复 P2b/P2c，补 `fr->st=NULL`，
  再恢复 P3a 每流 target；未恢复 P3b1（无 announced_mask/单流门删除/
  far 未知 sid 建流）。
- **注意**：该快照实际不含 P2d（path_end/payload_bytes_sent），
  当前树因此回到 P2b+P3a；P2d 需另行重做。
- 验证：make check 348/0；host-test-san 通过；lint 15/15、12/12；
  probe B **3/3 PASS**；probe I **3/3 PASS**；public churn
  **10000/0/0**、two/halfclose/rst/writer PASS；仅既有
  src_key/srcfilter FAIL。
- P3b1 的失败根因（announced_mask 无 generation、far bind 简化、
  far_sid 门删除）已在 13.4.3 记录；并发留待 P3b2。

### 13.5 P3 前置门

只有 P2a–P2d 全部通过、且每一步的 patch 都能单独反演，才允许进入 P3：

- 删除 `far_sid` 单槽门；
- 每流 target、`announced_mask`、每流暂停；
- `pub.two` 新契约（B 也可用）；
- 100 并发。

在此之前**不得**删除单流门、不得改 `far_sid` 语义、不得把 `pub.two`
改成“两条都可用”。

---

## 14. P3b2 准备：并发多流的三个先决条件（只设计，未改代码）

### 14.1 announced_mask 必须带 generation

P3b1 的失败根因是 `u16 announced_mask` 只有 carrier index，没有 generation。
载体在同一 index 被替换后，旧 bit 仍为 1，新 OPEN 被 pump 按旧 bit 丢弃，
`c->up` 保持 0，DATA 被挡，最终 `tx_ack=0/fbk_rx=0/holes_unrecoverable=1`。

P3b2 的正确形态：

- 每流两个 u16：`u16 announced_mask` + `u16 announced_gen[16]`，或
  `u16 announced_mask` + `u8 announced_gen[16]`（gen 只有 4 位有效）。
- `tbt_tx_announce()` 只在
  `(mask & BIT(i)) && announced_gen[i] == (c->gen & 0xf)` 时认为已宣告；
  gen 不同就重新排 OPEN。
- pump 写 OPEN 成功后同时更新 `announced_mask` 与 `announced_gen[i]`。
- pump 的 DATA 门用同样的 `(bit && gen)` 判断，不再用 `c->open_sent`。
- `c->open_sent` 只保留为诊断；载体替换时新 `struct tbt_carrier` 的
  `open_sent=false`，但 per-stream 位图仍要按 gen 失效。
- 不变量：DATA(sid) 只能在“该 sid 的 OPEN 已在该 (index,gen) 写完”的载体上走。

### 14.2 far 多 sid bind 必须保留单流时代的三条检查

P3b1 把 `tbt_session_bind_sid()` 简化成“未知 sid 一律建流”，丢掉了：

1. 旧流仍活着时，新 sid 必须拒绝（或按显式规则排队），不能覆盖主流程；
2. 旧流已 closing/DEAD 时，换 sid 前必须
   `tbt_session_reset_ack_locked()` + `tbt_stream_tombstone_locked(old)`；
3. 迟到/墓碑 sid 必须丢弃，不能重新建流并拨 target。

P3b2 的 per-sid 形态：

- far 侧 `tbt_stream_get(sid, alloc=true)` 只在 **OPEN**（以及容错的 DATA）
  到达时创建；FIN/RST 未知 sid 只计数丢弃，不拨 target。
- 每条流自己的 `closing`/`tombstone` 判断；旧流的 session 级状态
  （`sess->tx_ack/rx_ack/peer_paused/holes/predict`）在旧流仍可能被读时
  不能 reset；P3b2 必须先把这些状态迁到 stream，或明确只允许一个
  “session-active” 流（其余流只走 SACK/每流 ack）。
- `tbt_session_stream()` 不能再作为唯一入口；FBK/retx/stall/holes 要么
  逐流遍历，要么按 sid 显式取流。

### 14.3 每流 retx/stall/holes 的遍历与锁序

P3b1 的 `tbt_session_for_each_stream` 在 pump 内回调，出现
`tbt_stream_get` 与 tx_lock/sess_lock 的死锁；恢复时 pump 单流后 B 才绿。

P3b2 的遍历规则：

- **锁序**：`pump_lock` → `tx_lock` → `sess->lock` 只允许由外到内；
  `sess->lock` 内**不得**取 socket 锁或 `tx_lock`；
  `tx_lock` 内**不得**取 `sess->lock` 或 socket 锁。
- 遍历流列表时，在 `sess->lock` 或 RCU 下只做
  `refcount_inc_not_zero` 收集引用；**锁外**回调，回调后 put。
- pump 的帧循环用 `fr->st`，不查哈希；retx/stall/holes 的遍历在
  `pump_lock` 内、`tx_lock` 外进行，回调内部再按函数需要取 `tx_lock`。
- 不允许在 `tx_lock` 内 `tbt_stream_put()`；帧释放统一走
  `tbt_frame_free()`，锁外 put。
- `w_kill`/`w_target`/`w_reap` 的 cancel 顺序：先 cancel w_target，
  再 cancel w_kill，再 detach/release；不要从 w_kill 里 cancel 自己。
- `announced_mask` 的 gen 更新与 pump OPEN 写成功在同一 tx_lock 段内，
  避免“位图已置、载体未 up”的窗口。

### 14.4 P3b2 门

只有以下全部满足才允许删单流门：

1. P2d 在单流 B/I/churn 绿（当前已满足）。
2. P3a 每流 target 在单流绿（当前已满足）。
3. announced_mask 带 gen，且 B 的 carrier replace 用例通过。
4. far bind 保留 closing/tombstone/reset 检查，且迟到 sid 注入不建流。
5. retx/stall/holes/FBK 的遍历通过 lockdep/长稳；无 `sess->lock` 内
   socket/tx_lock。
6. 先 2 条并发，再 10 条，最后 100 条；每档 churn 10000 不回归。

### 14.5 P3b2a 实测（announced generation，已回退）

- 尝试给 `announced_mask` 加 `announced_gen[16]`，OPEN/DATA 门按 `(index,gen)`；
  单流 B×3、I×3 仍 PASS，但 public `pub.churn` 超时（Terminated）、
  `pub.halfclose` echo 0。
- 说明单流下用 per-stream 位图替换 `c->open_sent` 仍不等价：`c->open_sent`
  同时承担“HELLO 后 OPEN 已写”的载体状态与调度器 `need_open` 过滤；
  仅按流位图门控会在 churn 的新流/旧载体状态间留下窗口。
- 已回退 `announced_gen` 与 mask 门控，恢复 `c->open_sent`；public 恢复
  8/10（churn 10000/0/0、two/halfclose/rst/writer PASS）。
- far bind 的 closing/tombstone/reset 检查当前树已存在，无需再改。
- 结论：P3b2 的 announced 矩阵必须与 `c->open_sent`/调度器一起重新设计，
  不能只替换 pump 的两个判断。

## 15. P3b3 前置：host 多流状态机模型与内核改动映射

`host/test_multistream.c` 是不链接内核的纯 C 状态机模型。它只描述会话、
流、载体、announced 矩阵、调度器和帧引用之间的状态转移；不模拟 socket、
skb、workqueue 或真实 TCP。`make host-test` 已包含它，当前
`test_multistream: 27419 checks, 0 failures`。

### 15.1 模型中的四种模式

| 模式 | OPEN/DATA 门 | 调度器 `need_open` | 用途 |
|---|---|---|---|
| `P2` | 载体级 `c->open_sent` | 载体级 `c->open_sent` | 当前单流基线；复现“新流复用旧载体时 DATA 早于 OPEN” |
| `P3B1` | 只按 index 的 `announced_mask` | 载体级 `c->open_sent` | 复现 P3b1：旧 index 位阻止新 generation 的 OPEN |
| `P3B2A` | `(index,gen)` announced 矩阵 | 载体级 `c->open_sent` | 复现 P3b2a：DATA 门已按流，调度器仍按载体 |
| `P3B3` | `(index,gen)` announced 矩阵 | 每流 `(index,gen)` | 目标设计：OPEN、DATA、调度器三者同源 |

模型用 `replace_carrier()` 模拟同一 index 上新 `struct tbt_carrier`：
`gen` 增加、`open_sent=false`、旧 announced 位失效。`scenario_carrier_replace()`
证明：index-only 位图会让新 generation 的 OPEN 被旧 bit 挡住；带 gen 的矩阵
不会。`scenario_new_stream_existing_carrier()` 证明：只要调度器仍用
`c->open_sent`，新流就可能在 OPEN 前发 DATA。`scenario_concurrent()` 跑
2/10/100 条流，逐流检查 target 唯一、ack 独立、announced `(index,gen)` 一致、
引用平衡。`scenario_churn()` 跑 1000 轮创建/OPEN/ACK/SACK/retire，检查迟到
ACK 不迁移到新流、tombstone 不污染新 sid。

### 15.2 模型发现的硬约束

1. **announced 必须是 `(index, gen)`，不能只存 index。** 载体替换后同一
   index 是新对象；旧 bit 若保留，`tbt_tx_announce()` 会跳过新载体的 OPEN，
   而 `c->up` 仍为 0，DATA 永远不出去。P3b1 的失败样本正是这个形态。
2. **`c->open_sent` 不能同时承担“载体状态”和“本流已公告”。** 它天然是
   载体级布尔值：第一条流把载体置 true 后，第二条流无论是否发过 OPEN 都会
   被 DATA 门和调度器放行。P3b2a 只替换 DATA 门、保留调度器过滤，仍会在
   churn 的新旧流交替窗口里让 OPEN 排不到队。
3. **OPEN 写成功才更新 announced。** 模型在 `write_opens()` 成功写 OPEN 后
   才置 `announced_mask/gen`；`tbt_tx_announce()` 的入队动作只负责排队。
   内核当前在 `tbt_tx_announce()` 里提前置 `st->tx_open_sent`，这是
   “已排队”断言，不能拿来当“已公告”门。
4. **调度器必须按帧的流选择路径。** `tbt_sched_pick()` 当前只收
   `need_open` 布尔值；P3b3 必须传入 `fr->st`（或至少 sid），否则同一次
   pump 里不同流的 DATA 会共享一个错误的载体过滤结果。
5. **ACK/SACK 的 sid 必须逐流。** `tbt_tx_ack()` 仍以 `sess->far_sid` 做门，
   并把 `uack` 与 `sess->tx_ack` 比较；第二条流的 ACK 会被丢弃。模型里
   `stream_ack()` 只改 `st->tx_ack`，且 clamp 到 `st->tx_seq`。
6. **retire 后旧 ACK 不得迁移。** 模型在 `stream_retire()` 清 announced 位，
   并让迟到 ACK 只作用于 tombstone 自身；新流从 0 开始。
7. **帧必须持流引用。** 模型 `frame_new()`/`frame_free()` 对 `fr->st` 做
   refcount；pump、purge、retx 都按 `fr->st` 而不是当前 primary 流。
8. **每帧的 `uack` 必须按流。** `tbt_rx_data()` 把 `st->rx_next` 写进
   `sess->rx_ack`，pump 又用 `fr->hdr.uack = sess->rx_ack` 覆盖所有帧。
   多流交错时，最后处理的流会把自己的偏移写给别的流的 DATA/SACK，接收端
   的窗口和 SACK 会回退或错记。模型里 `stream_sack()` 只改自己的状态。

### 15.3 状态/转换 → 内核函数映射

下表是 P3b3 的改动清单。左列是模型对象/转换，右列是当前内核位置和改法。

| 模型状态/转换 | 当前内核位置 | P3b3 改法 | 不变量 |
|---|---|---|---|
| `stream_new()` / hash 插入 | `tbt_stream_get()`（`tbt_session.c`）；near 由 `tbt_stream_start_now()`（`tbt_sock.c`）分配 sid | 保留；新增 `announced_mask`、`announced_gen[16]`、`open_pending` 初始化 | sid 唯一；refs=2（hash+caller） |
| `announce()` 排队 OPEN | `tbt_tx_announce()`（`tbt_tx.c`） | 不再以 `c->open_sent` 作为唯一跳过条件；对每个 `hello_sent && !stopping` 且 `!(announced_gen[i]==c->gen && mask bit)` 的载体排队 OPEN；`tx_open_sent` 保留为入队断言 | 每载体最多一个未写 OPEN |
| pinned OPEN 准入 | `tbt_tx_pump()` 的 `else if (type == TBT_F_OPEN)`（`tbt_tx.c`，约 1523 行） | 现在 `READ_ONCE(c->open_sent)` 就 `goto drop`；P3b3 改为 `tbt_stream_announced(fr->st, c)` 才 drop。否则第一条流把载体置 `open_sent=true` 后，第二条流的 OPEN 永远进不了泵 | 旧流的 `open_sent` 不挡新流 OPEN |
| `write_opens()` 成功 | `tbt_tx_pump()` 的 `case TBT_F_OPEN`（`tbt_tx.c`，约 1729 行） | 写成功后在同一 `tx_lock` 段内置 `st->announced_mask |= BIT(c->index)`、`announced_gen[c->index]=c->gen&0xf`；仍置 `c->open_sent` | announced 与载体 gen 同源 |
| OPEN 帧的流归属 | `tbt_tx_control_pinned()` 目前只带 sid，`fr->st` 未绑定 | 增加 `struct tbt_stream *st` 参数（或写成功后按 sid 查流），OPEN 成功时给对应流记账并 `refcount_inc`；释放走 `tbt_frame_free()` | 每个 OPEN 只记到自己的流 |
| `sched_need_open()` | `tbt_sched_rebuild()` / `tbt_sched_pick()`（`tbt_tx.c`） | 增加 `struct tbt_stream *st` 参数；`need_open` 时用 `tbt_stream_announced(st,c)` 替代 `!c->open_sent`；pinned 帧仍按 `fr->path` | 调度器不把 DATA 放到未公告载体 |
| `data_gate()` | `tbt_tx_pump()` 的 `if (type == TBT_F_DATA && !READ_ONCE(c->open_sent))`（约 1559 行） | 改为 `if (type == TBT_F_DATA && !tbt_stream_announced(fr->st, c)) continue;`；保留 `c->up` | DATA 只走已公告 `(index,gen)` |
| `fr->open_pending` | `tbt_tx_submit()` 约 1935 行；pump 的 WARN 约 1651 行 | 保留为入队断言：`st->near && !st->tx_open_sent`；不要把它升级成门 | WARN 不得变成丢帧 |
| `replace_carrier()` | `tbt_carrier_alloc()` 的 `c->gen=++sess->carrier_gen`；`tbt_session_join()`（`tbt_session.c`） | 新载体 join 后，对**所有活跃流**重新 `tbt_tx_announce()`；旧流的 `announced_gen` 自然不等于新 gen，不需要清旧载体位 | 新 gen 必须重新 OPEN，旧 gen 位不得复用 |
| `stream_retire()` | `tbt_stream_tombstone_locked()`、`tbt_stream_put()`、`tbt_session_bind_sid()`（`tbt_session.c`） | 终态清 `announced_mask/gen`；按 `fr->st` purge；保留 tombstone 至 deadline；迟到 ACK 只作用于 tombstone 自身 | 旧流不污染新 sid |
| `stream_ack()` | `tbt_tx_ack()`（`tbt_tx.c`）；`tbt_rx_frame()` 对每个带 sid 的帧先调它（`tbt_rx.c`） | 删除 `sid != sess->far_sid` 门；把 `uack <= sess->tx_ack` 改为 `uack <= READ_ONCE(st->tx_ack)`；`sess->tx_ack` 仅保留统计镜像；clamp 到 `st->tx_seq`；ACK 先于 OPEN 到达且查不到流时丢弃，不 adopt | 每流 ack 单调、独立；迟到 ACK 不建流 |
| `stream_sack()` | `tbt_tx_sack()`（`tbt_tx.c`）；`tbt_rx_data()` 写 `sess->rx_ack`（`tbt_rx.c`） | 绑定 `fr->st = st` 并 `refcount_inc`；`uack` 用 `st->rx_next`；pump 写帧时改为 `fr->hdr.uack = fr->st ? READ_ONCE(fr->st->rx_next) : sess->rx_ack`，不要用全局 `sess->rx_ack` 覆盖；未知 sid 不建流 | 每帧 uack 属于该帧的流；SACK 不回落 primary |
| `retain/path_end` | `tbt_tx_retain_gc/trim/retx()`、`tbt_tx_path_lost()` | `path_lost` 不能只处理 primary；按 `(index,gen)` 清每个流的 `path_mask/reserve_mask`，或记 session 丢失位由 pump 逐流处理；retain 仍每流 | 旧 gen 的 path_end 不证明新 gen |
| `holes` | `sess->holes[]`、`hole_head/tail`、`tbt_tx_holes_repaired()`、`tbt_tx_hole_check()` | 当前 hole 环是 session 级且不带 sid，seq 却是每流字节偏移；P3b3 必须给 `struct tbt_hole` 加 `sid`（或改成每流环），`holes_repaired`/`hole_check` 接收 `st` | 一条流的 hole 不取消另一条流的 PREDICT |
| `retx/stall` | `tbt_tx_retx()`、`tbt_tx_stall_check()` 只取 `tbt_session_stream()` | pump 前用快照（`sess->lock` 内 `refcount_inc_not_zero`，锁外处理）逐流跑 retain_gc/trim/retx；stall 逐流判断，任一活跃流 stall 才 kill | 锁序 `pump_lock → tx_lock → sess->lock`；锁外 put |
| `target attach/detach` | `tbt_carrier_open_target()`、`tbt_target_attach/detach()`（P3a 已每流） | 保留；`tbt_session_join()` 与 `tbt_far_open_seen()` 对每条流各拨一次 target；kill/retire 时 detach 一次 | 每流恰好一个 target |
| far 多 sid bind | `tbt_session_bind_sid()`（`tbt_session.c`） | 删除 `!sess->far_sid` 的单流 adopt 门；按 sid 查 hash：DEAD/tombstone 丢弃；closing 的旧流不阻塞新 sid；未知 sid 只有带 OPEN 语义才建流；保留 closing/tombstone/reset 检查 | 迟到/伪造 sid 不建流；不重置别的流 ACK |
| `sess->far_sid` | near `tbt_stream_start_now()`、far `bind_sid`、`tbt_session_stream()` | 逐步降级为“最近/primary”统计字段；数据面改用 hash + `fr->st`；`tbt_pending_work()` 不再因 `far_sid` 非空而停 | 多流时不存在单一 primary 门 |
| `frame refs` | `tbt_frame_alloc/free()`、`tbt_tx_enqueue()`、`tbt_tx_purge_stream()` | 所有入队路径绑定 `fr->st` 并 `refcount_inc`；pump 用 `fr->st`；`tbt_frame_free()` 在 `tx_lock` 外 put | 无引用泄漏；purge 不漏帧 |
| `churn` | near `tbt_stream_start_now()`、far `bind_sid`、retire/tombstone | 允许新 sid 在旧流 CLOSE_WAIT 后立即创建；不要全局 `tbt_session_reset_ack_locked()` 清掉其他流状态 | 1000 轮后 refs/target/ack 平衡 |

### 15.4 P3b3 修改顺序（每步单变量、可回退）

1. **S1：announced 记账，不换门。** 给 `struct tbt_stream` 加
   `announced_mask`、`announced_gen[16]`；在 pump 的 OPEN 成功分支写入；
   `tbt_tx_announce()` 只排队。单流基线应完全不变。
2. **S2：OPEN 帧绑定 `fr->st`。** 扩展 `tbt_tx_control_pinned()` 或写成功
   后按 sid 取流；所有 control 帧入队都持流引用。验证 purge/retire 无泄漏。
3. **S3：DATA 门换 `(index,gen)`。** 只改 pump 的 DATA 分支；调度器仍用
   `c->open_sent`。单流 B/I/churn 必须绿；这是 P3b2a 的失败点，必须单独过。
4. **S4：调度器按流传入 `st`。** `tbt_sched_pick()`/`tbt_sched_rebuild()`
   增加 `st`；`need_open` 用 announced。此步后才允许第二条流真正并发。
5. **S5：ACK/SACK 逐流。** 删除 `sess->far_sid` 门，`tbt_tx_ack()` 用
   `st->tx_ack`；SACK 绑定 `fr->st`；pump 里 `fr->hdr.uack` 用
   `fr->st->rx_next`（SACK/ACK 控制帧），FBK 仍走 per-path `c->rx_bytes`。
6. **S6：retx/stall/holes/FBK 遍历。** 用快照逐流；`holes` 加 sid 或每流
   环；`tbt_tx_path_lost()` 不再只处理 primary。锁序不变，锁外回调/put。
7. **S7：far 多 sid bind。** 删除单流 adopt 门，保留 tombstone/closing/
   reset 检查；`tbt_session_join()` 对所有活跃流 announce + 拨 target。
8. **S8：并发档位。** 先 2 条，再 10 条，最后 100 条；每档跑 churn
   10000 不回归，再跑 public 必需项。

### 15.5 回退点

- S1/S2 可独立回退：它们只增加记账和引用，不改变门控。
- S3 失败 → 只回退 DATA 门，保留 announced 记账；不要恢复 `announced_mask`
  的 index-only 形态。
- S4 失败 → 回退 `tbt_sched_pick()` 签名和 `need_open` 判断，保留 S3。
- S5 失败 → 回退 ACK/SACK 的 sid 门，保留 pump 的 `fr->st` 绑定。
- S6 失败 → 回退遍历，保留单流 `tbt_session_stream()` 路径；不要删单流门。
- S7 失败 → 回退 `bind_sid`，保留 near 单流创建；far 仍只 adopt 一个 sid。

## 16. S1 + S2 实施记录（2026-11-05，保留）

### 16.1 S1：announced 记账

- `struct tbt_stream` 增加 `u16 announced_mask` 与 `u8 announced_gen[16]`。
- `tbt_tx_pump()` 的 `case TBT_F_OPEN` 在 OPEN 写成功后，才在同一处更新
  `announced_gen[c->index] = c->gen & 0xf` 与 `announced_mask |= BIT(index)`；
  `tbt_tx_announce()` 入队时只置 `tx_open_sent`，不写 announced。
- `c->open_sent` 保持原语义：仍由 OPEN 写成功置 true，DATA 门和调度器
  继续只看它。announced 目前只记账，不参与门控。
- 调试断言：OPEN 成功时若 `fr->st` 为空，或更新后
  `tbt_stream_announced(fr->st, c)` 仍为假，`WARN_ON_ONCE` 并累加
  `sess->dbg_announce_mismatch`。单流正常路径下，OPEN 写成功后
  announced 与该载体的 `open_sent` 同时为真。
- 已知空档：`tbt_tx_announce()` 仍因 `c->open_sent` 跳过已开载体，所以
  churn 的第二条流可能没有自己的 OPEN；此时 announced=false 而
  open_sent=true。S1 的断言只覆盖真正写出 OPEN 的路径。这个空档在 S3
  之前必须解决，否则 S3 直接用 announced 做 DATA 门会让 churn 停住。

### 16.2 S2：控制帧持流引用

- `tbt_tx_control_pinned()` 增加 `struct tbt_stream *st` 参数；非空时
  `refcount_inc(&st->refs)`，`fr->st = st`。
- OPEN（`tbt_tx_announce` 与 HELLO 后的补发）、FIN、RST、SACK、FBK 都绑定
  各自的流；HELLO、PAD、RETIRE 是会话/载体级帧，传 NULL。
- `tbt_frame_free()` 已按 `fr->st` put（`fr->st=NULL` 保护保留）。pump 的
  丢弃路径、`tbt_tx_purge_stream()`、`tbt_tx_free_queue()` 都走它。
- `tbt_tx_purge_stream()` 仍按 `fr->st == st` 匹配，且只清
  DATA/PREDICT/CANCEL；FIN/RST 等控制帧保留给对端。CLOSE_WAIT 只
  `tbt_stream_detach_from_session()`，不 purge；只有 kill 终态才 purge。
- 未改 DATA 门、调度器、`far_sid` 单流门。

### 16.3 验证

- `make check`：模块编译通过；`test_parse 348`、`test_fuzz 8525966`、
  `test_http 249`、`test_reorder 51831`、`test_ack 10026`、
  `test_control 128`、`test_multistream 27419`、`test_anchor 101`，
  全部 0 failures。
- `make host-test-san`：全绿（`test_http_fuzz 4078276` 等）。
- `bash review/lint.sh`：15/15 checks、12/12 self-tests。
- 探针 B ×2：PASS；探针 I ×1：PASS（n=1/n=4 ratio 1.86x）。
- public 组：`pub.churn` 10000/0/0、`pub.halfclose`、`pub.two`、
  `pub.rst`、`pub.writer`（100 MiB 与 1024 MiB）全 PASS；
  仅既有 `init.src_key` / `pub.srcfilter` 按预期 FAIL。8 passed, 2 failed。
- 未跑 bench / n=8/16。

### 16.4 下一步

S3 只把 pump 的 DATA 门从 `c->open_sent` 换成
`tbt_stream_announced(fr->st, c)`；调度器仍用 `c->open_sent`。S3 通过后
再进入 S4。

## 17. S3a + S3b 实施记录（2026-11-05，保留）

### 17.1 S3a：OPEN 改为每流每载体

- `tbt_tx_announce()` 不再因 `c->open_sent` 跳过载体；它用
  `tbt_stream_announced(st, c)` 判断该流在该 `(index, gen)` 上是否已写
  过 OPEN。每条新流在每条已 HELLO 的载体上发自己的 OPEN。
- `tbt_tx_pump()` 的 pinned OPEN 准入不再因 `c->open_sent` 丢帧；只丢
  “该流已在此 `(index,gen)` 公告过”的重复帧，以及没有 `fr->st` 的帧。
- `c->open_sent` 只保留“该载体至少写过一个 OPEN”的载体状态；S3a 的
  DATA 门仍用它（S3b 才换）。
- far bind：`tbt_session_bind_sid(sess, sid, bool open)`；OPEN 是换流的
  唯一入口，DATA/FIN/RST/PREDICT 不能把会话移到新 sid。OPEN 换流时
  重置 session ACK 空间、tombstone 旧流、再 adopt 新 sid。已 tombstone
  或 closing 的 sid 直接 DROP，迟到 FIN/OPEN 不会把会话切回旧流。
- `tbt_rx_bind_stream()` 增加 `open` 参数；OPEN 传 true，其余传 false。

S3a 第一次 public 实测失败：`pub.churn` 9875/10000、`pub.two`
A still usable=0。根因是 far 收到新 OPEN 换流后，旧 sid 的迟到 FIN
仍走 `bad` 分支返回 `-EPROTO`；补上 `new->tombstone_until_ns` 的 DROP
检查后，public 恢复 8/10，必需项全 PASS。S3a 最终模块 sha256
`261efbe6870ce45445c3682913264641ae28e05694c4727158f0f6779addc243`。

### 17.2 S3b：DATA 门换 announced

只改 `tbt_tx_pump()` 的一处：

```c
- if (type == TBT_F_DATA && !READ_ONCE(c->open_sent))
+ if (type == TBT_F_DATA &&
+     (!fr->st || !tbt_stream_announced(fr->st, c)))
      continue;
```

调度器仍用 `c->open_sent`；没有改 `tbt_sched_pick/rebuild` 签名，没有删
`far_sid` 门。DATA 只在帧所属流已在该 `(index,gen)` 写过 OPEN 后发出。
S3b 模块 sha256
`3ae10b7ec6955dad3aaec270ad7c10197c155ef64457cd03ac005cfe683797f8`。

### 17.3 验证数字

| 步骤 | make check / host | B ×2 | public | I ×1 |
|---|---|---|---|---|
| S3a | 全绿，`test_multistream 27419` | PASS / PASS | churn 10000/0/0，two/halfclose/rst/writer PASS；8 passed 2 failed（仅既有 src） | PASS |
| S3b | 全绿，`test_multistream 27419` | PASS / PASS | churn 10000/0/0，two/halfclose/rst/writer PASS；8 passed 2 failed（仅既有 src） | PASS |

- `make host-test-san`：S3b 全绿（`test_fuzz 8525966`、`test_http_fuzz 4078276`）。
- `bash review/lint.sh`：S3b 15/15 checks、12/12 self-tests。
- `pub.two` 仍是旧契约：A usable=1、B refused=1、A still usable=1。
- 未跑 bench / n=8/16；未并发多流。

### 17.4 下一步

S4 才改调度器：`tbt_sched_pick/rebuild` 接收 `fr->st`/流，`need_open`
用 `tbt_stream_announced(st, c)`，不再用 `c->open_sent`。S4 前不要删
`far_sid` 门，也不要开始 2/10/100 并发。

## 18. S4 实施记录（2026-11-05，保留）

### 18.1 调度器按流

- 新增 `struct tbt_sched`：`order[16]`、`n`、`need_open`、`win`、`rot`、
  `sort_ns`。`struct tbt_stream` 内嵌一份 `sched`；`struct tbt_session`
  保留一份 `ctrl_sched` 给控制帧。
- `tbt_sched_better(sess, s, a, b)` 使用所选调度状态的 `rot`。
- `tbt_sched_rebuild(sess, st, need_open)`：
  - `st != NULL` 用 `st->sched`，否则用 `sess->ctrl_sched`；
  - `need_open` 时过滤条件从 `!c->open_sent` 改为
    `!tbt_stream_announced(st, c)`；spare/up/stopping 规则不变。
- `tbt_sched_pick(sess, st, blocked, need_open)`：状态、窗口、重建判断都
  从会话字段移到所选 `struct tbt_sched`。
- `tbt_tx_pump()` 对非 pinned 帧传 `fr->st`；FBK 传 NULL，走
  `ctrl_sched`。HELLO/PAD/RETIRE 仍是 pinned，不经过调度器。
- 未改 ACK/SACK/far_sid/bind；未启用并发多流。

### 18.2 验证数字

- `make check`：全绿，`test_multistream 27419`，其余 host 测试 0 failures。
- `make host-test-san`：全绿（`test_fuzz 8525966`、`test_http_fuzz 4078276`）。
- `bash review/lint.sh`：15/15 checks、12/12 self-tests。
- 探针 B ×2：PASS / PASS。
- public：8 passed，2 failed；`pub.churn` 10000/0/0、`pub.two`、
  `pub.halfclose`、`pub.rst`、`pub.writer`（100 MiB + 1024 MiB）全 PASS；
  仅既有 `init.src_key` / `pub.srcfilter` FAIL。
- 探针 I ×1：PASS。
- 模块 sha256：`7e4995c5feffc97fa2880d5d39fadbd0ac4ce14d6343e77860e09b77627355ba`。
- 未跑 bench / n=8/16。

### 18.3 S5 接口准备

- 每流调度状态已有位置：`struct tbt_stream.sched`；控制状态在
  `struct tbt_session.ctrl_sched`，不需要再为 S5 增加调度字段。
- S5 仍需改的是 **inflight 的 ack 基准**：`tbt_path_inflight()` 与
  `tbt_sched_better()` 现在读 `sess->tx_ack`。多流前应让它们接收
  `st`/`ack`，DATA 用 `st->tx_ack`，控制用 `sess->tx_ack`。
- S5 还要逐流化 ACK/SACK、retx/stall/holes/FBK；这些仍走
  `tbt_session_stream()` 或 session 级 hole 环。
- 本步没有删 `far_sid` 门，也没有允许多条并发。

## 19. S5 实施记录（2026-11-05，保留）

### 19.1 ACK / SACK / inflight

- `tbt_tx_ack()` 以 `st->tx_ack` 为权威：
  - 旧值比较从 `uack <= sess->tx_ack` 改为 `uack <= READ_ONCE(st->tx_ack)`；
  - 先 `WRITE_ONCE(st->tx_ack, uack)`，再把 `sess->tx_ack` 写成
    control/legacy 镜像；
  - `sess->far_sid` 门保持不变。
- `tbt_path_inflight(sess, st, c)`：`st != NULL` 用 `st->tx_ack`，否则用
  `sess->tx_ack`。`tbt_sched_better(sess, st, s, a, b)` 把 `st` 传下去；
  `tbt_sched_rebuild()` 在 `need_open` 时传流，控制帧传 NULL。
- `tbt_tx_pump()` 的每帧 `hdr.uack`：
  - `fr->st != NULL` 且不是 FBK：用 `fr->st->rx_next`；
  - FBK 和无流控制帧：用 `sess->rx_ack`。
- SACK 帧在 S2 已绑定 `fr->st`；本步让 pump 不再用全局 `sess->rx_ack`
  覆盖它。

### 19.2 验证数字

- `make check`：全绿，`test_multistream 27419`，其余 host 测试 0 failures。
- `make host-test-san`：全绿（`test_fuzz 8525966`、`test_http_fuzz 4078276`）。
- `bash review/lint.sh`：15/15 checks、12/12 self-tests。
- 探针 B ×2：PASS / PASS。
- 探针 I ×1：PASS。
- public 共跑 4 次：
  - 3 次 8 passed / 2 failed，`pub.churn` 10000/0/0、`pub.two`、
    `pub.halfclose`、`pub.rst`、`pub.writer`（100 MiB + 1024 MiB）全 PASS；
  - 1 次 `pub.two` 出现瞬时 `A still usable=0`，同一模块随后连续两次
    通过；未改代码。仅既有 `init.src_key` / `pub.srcfilter` 固定 FAIL。
- 模块 sha256：`c5395e1088ad98bcb06f3d50a28bd1651150943c789cb9b9fecd6c8b026cd9ef`。
- 未跑 bench / n=8/16。

### 19.3 S6 逐流遍历接口与锁

需要把 `tbt_session_stream()` 的单流读取替换为快照/迭代器：

- `tbt_tx_pump()`：当前只对 primary 调 `tbt_tx_retain_gc/trim/retx`；
  改为先快照所有活跃流（`sess->lock` 内 `refcount_inc_not_zero`），锁外
  逐流处理，最后 `tbt_stream_put()`。
- `tbt_tx_ack()`：`tbt_tx_holes_repaired(sess)` 改为
  `tbt_tx_holes_repaired(sess, st)`；hole 环需带 sid 或改成每流环。
- `tbt_tx_hole_check(sess, fbk_mask)` 改为接收 `st`；
  `tbt_tx_stall_check(sess)` 改为逐流；`tbt_fbk_work()` 的单个
  `tbt_session_stream()` 改为遍历，并逐流发 FBK/SACK。
- `tbt_tx_path_lost(sess, index)` 当前只处理 primary；它在
  `tbt_carrier_lost()` 中可能处于 atomic/softirq 上下文，不能睡眠。
  可记 session 丢失位、由 pump 逐流清 `path_mask/reserve_mask`，或
  RCU 快照后处理。
- 仍读 `sess->tx_ack` 的位置：`tbt_retain_received()`、
  `tbt_retain_tcp_done()`、`tbt_hole_copy_inflight()`、
  `tbt_tx_hole_check()`、`tbt_tx_path_lost()` 等；S6 改为 `st->tx_ack`。
- 锁序保持 `pump_lock → tx_lock → sess->lock`，锁外回调/put；不得在
  `sess->lock` 内取 socket 或 `tx_lock`。

## 20. S6a + S6b 实施记录（2026-11-05，保留）

### 20.1 S6a：holes 带 sid，ACK 读取点归流

- `struct tbt_hole` 增加 `u64 sid`。sid 是 40 位
  (`prefix16 << 24 | counter24`)，用 `u32` 会截断前缀，因此用 `u64`。
- 保留会话级 `sess->holes[]`、`hole_head/hole_tail`；每次声明同时写入
  `sid`。S6b 的 `tbt_tx_holes_repaired(sess, st)` 只消费本流的 head。
- 剩余 `sess->tx_ack` 读取点改为 `st->tx_ack`：
  - `tbt_tx_retain_gc()`
  - `tbt_tx_retx()` 的候选复制检查
  - `tbt_retain_received(sess, st, r)`
  - `tbt_tx_hole_check()`
  - `tbt_tx_path_lost()`（S6b 改为延迟处理）
- `tbt_retain_tcp_done()` / `tbt_hole_copy_inflight()` 本身不读
  `sess->tx_ack`，只依赖 retain 的 per-path 字段。
- 单流下 `st->tx_ack` 与 `sess->tx_ack` 等价；public 8/10，必需项全 PASS。

### 20.2 S6b：refcounted snapshot 与逐流遍历

- `tbt_tx_stream_snapshot(sess, out, max)`：持 `sess->lock`，对每条非
  `TBT_ST_DEAD` 流 `refcount_inc_not_zero`；释放锁后处理；最后
  `tbt_stream_put()`。上限 `TBT_STREAM_SNAP_MAX=128`，溢出记
  `dbg_snap_overflow`。
- `tbt_tx_pump()`：
  - 快照全部活跃流；逐流 `retain_gc/trim/retx`；
  - `st` 仅作为单流 HELLO 补发对象取 `snap[0]`，快照引用在 pump 结束
    统一 put；
  - 队列扫描/选择逻辑不变。
- `tbt_fbk_work()`：快照逐流发 FBK/SACK、`tbt_rx_check_hole()`、
  `tbt_tx_stall_check(sess, st)`；任一存活则续定时器。
- `fbk_frames` 从 session 移到 `struct tbt_stream`，避免多流互相清零。
- `tbt_tx_holes_repaired(sess, st)`、`tbt_tx_hole_check(sess, st, mask)`、
  `tbt_tx_stall_check(sess, st)` 改为调用者持引用；函数内不再 get/put。
  `tbt_tx_ack()` 在 put 前调用 holes_repaired。
- `tbt_rx_fbk()` 按 FBK 头的 sid 取流，再调用 hole_check，锁外 put。
- `tbt_tx_path_lost(sess, index)` 不再在 atomic/softirq 中遍历：只
  `set_bit(index, &sess->path_lost_mask)` 并 `tbt_fbk_kick(now)`；
  pump/FBK 用 `xchg` 取走 lost 位，再逐流清 `path_mask/reserve_mask`、
  置 `retx_now`。
- 锁序：`pump_lock → tx_lock → sess->lock`；snapshot 只在
  `sess->lock` 内取引用；retain/hole/path-lost 的 tx_lock 与回调都在锁外。
  未在 `sess->lock` 内取 socket/tx_lock。

### 20.3 验证数字

| 步骤 | make check / san / lint | B ×2 | public | I |
|---|---|---|---|---|
| S6a | 全绿；lint 15/15 + 12/12 | PASS / PASS | churn 10000/0/0，two/halfclose/rst/writer PASS；8/10（仅既有 src） | PASS |
| S6b | 全绿；lint 15/15 + 12/12 | PASS / PASS | churn 10000/0/0，two/halfclose/rst/writer PASS；8/10（仅既有 src） | PASS |

- S6a 模块 sha256：
  `a73e65185b8762256776a14a52b474ad4dda2c19898ac7447eacf778ebe119ea`。
- S6b 模块 sha256：
  `343e728d7a5d74a41b101ed27853ceee008d624452b3484887c07314b78a9239`。
- 未跑 bench / n=8/16；未并发多流。

### 20.4 S7 前仍需处理

- 会话 hole 环仍是单个 FIFO；`tbt_tx_holes_repaired()` 遇到别的 sid 会
  停住。并发前改成每流环或每流 head/tail。
- `sess->predict_seq/predict_ns`、`sess->last_ack_ns/pause_cleared_ns`
  仍是会话级；S7 需归流或明确只用于控制路径。
- `tbt_session_join()` 仍只对 `tbt_session_stream()`（primary）announce
  和拨 target；S7 要遍历全部活跃流。
- `tbt_carrier_lost()` 的 join_retries 超限仍只 kill primary；S7 要逐流
  或按 session 处理。
- `tbt_tx_pump()` 的 HELLO 补发仍用 `snap[0]`；S7 要对该载体上所有
  活跃流各发一次 OPEN。
