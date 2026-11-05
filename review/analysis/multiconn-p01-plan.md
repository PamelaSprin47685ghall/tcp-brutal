# P0 / P1 可执行补丁计划：F6 载体读唤醒 + 顺序复用

**状态**：只读核对 + 计划更新。本波不改源码、不构建、不跑 QEMU。
**代码快照**：当前工作树（已含 P0-rmmod 的 `tbt_sk_ops` / `tbt_stream_release_socket` /
`tbt_sock_abort_on_close`；perf 已加 `tbt_car_poll_work` 的 `parser->have` 重试和
`tbt_tx_ack` 的 `fb_sample`）。S1 只动 `tbt_tx.c` / `tbt_carrier.c`；P0 改
`tbt_carrier.c`，P1-core 只碰 `tbt_tx_ack` 一处（加 sid），**必须等 S1 冻结后按函数名
重新锚定再应用**。
**行号**：当前快照；S1 合入后以函数名和代码片段为准。
**片段约定**：代码块里的 `...` 表示“该函数原有代码保持不变”，不是省略的补丁；
每个改动的前后片段都给出可直接对照的边界。

---

## 0. 范围与硬约束

### 0.1 本波做什么

- **P0**：修远端载体读路径，使 provisional 载体的 HELLO 不被“别的流的背压”或
  “一次性的 `w_rx`”挡住。验收 `far.data_path` 的 `rx_records>0`、`far.hello_garbage`
  仍 PASS、探针 A/B 不回归。
- **P1**：顺序复用。仍只允许**一条活跃 stream**；第一条关闭后，下一条能建立。
  验收 `pub.churn` 10000（`live=10000/refused=0/other=0`）、`near.second_conn rc=0`、
  探针 B 不回归。

### 0.2 本波不做什么

- 不做协议 v4：不改 `TBT_CONV`、不加 SACK、不改 40 位 sid、不改 OPEN 语义、
  不做每流 ACK / 每流 target / 每流暂停。这些属于 P2/P3。
- 不把 `pub.two` 改成“两条都可用”。P1 的 `pub.two` 仍是旧契约：B 不拒绝，但
  被排队；B 的读会超时，A 继续可用。P3 才改契约。
- 不修 F5 半关闭；P1 的关闭仍沿用当前的 `SO_LINGER=0` RST 语义（P4 再区分
  graceful FIN / abnormal RST）。

### 0.3 与 S1 / P0-rmmod 的衔接

| 现有代码 | 位置 | P0/P1 如何使用 |
|---|---|---|
| `tbt_sk_ops_save/restore` | `tbt_sock.c:397-427` | 释放任何 socket 前先 `tbt_sock_detach()`；它调用 `tbt_sk_ops_restore()`。不要写回 `tbt_pub_*` 函数。 |
| `tbt_sock_detach()` | `tbt_sock.c:377-396` | 清回调 + `cancel_work_sync(w_read/w_drain/w_ctrl)`；P1 的 release 路径复用。 |
| `tbt_stream_release_socket()` | `tbt_sock.c:438-466` | 已做 detach + cancel `w_kill` + 清 `sess->target` 别名 + abort + `sock_release`。P1 只加 `target_started=false` / target kick，并拆出 `_now` 变体避免自 cancel。 |
| `tbt_sock_abort_on_close()` | `tbt_sock.c:429-436` | P1 继续用它做 RST 关闭；P4 再按关闭原因区分。 |
| `tbt_session_destroy()` | `tbt_session.c:726-804` | P1 调整顺序：先停所有 `w_kill` 生产者，再 `tbt_tx_free_queue()`，再摘链/释放。 |

S1 冲突点：

- P0 改 `tbt_carrier.c` 的 `tbt_car_poll_work()` / `tbt_car_rx_work()`。
- P1-core 的 `tbt_tx_ack()` sid 检查改 `tbt_tx.c`；`tbt_rx.c` 的
  `tbt_rx_bind_stream()` / `tbt_rx_data()` 墓碑检查改 `tbt_rx.c`。
- `tbt_tx_purge_stream()` 可以放进 `tbt_session.c`（直接走 `sess->tx_q`），
  不碰 `tbt_tx.c`；target 重启可以只改 `tbt_sock.c`（重置
  `target_started` + release 后 kick），不必改 `tbt_carrier.c`。
- 这些改动都不改 wire；等 S1 合入后按函数名 rebase，不要与 S1 并行改同一段。

## 0.4 当前代码 rebase（本次只读核对）

### 0.4.1 当前锚点（不是旧稿行号）

| 函数 | 当前行号 | 备注 |
|---|---|---|
| `tbt_stream_free_rcu()` | `tbt_session.c:552` | 释放 rx_q/retain，`kfree(st)` |
| `tbt_stream_put()` | `tbt_session.c:563` | `refcount_dec_and_test` + `call_rcu` |
| `tbt_stream_get()` | `tbt_session.c:571` | 哈希查找/创建，GFP_ATOMIC |
| `tbt_session_stream()` | `tbt_session.c:634` | 单流访问器，仍按 `far_sid` |
| `tbt_stream_kill()` | `tbt_session.c:643` | 当前无单赢家、无 w_kill 引用 |
| `tbt_stream_kill_by_sid()` | `tbt_session.c:663` | RST/FIN 路径 |
| `tbt_session_destroy()` | `tbt_session.c:726` | `tbt_tx_free_queue()` 在 `:750`；stream 循环 `:755-781`；target 块 `:783-794` |
| `tbt_kill_work()` | `tbt_sock.c:321` | 当前自行 detach/release，`sk==NULL` 早返回 |
| `tbt_sock_attach()` | `tbt_sock.c:344` | bit 0 仍用字面量 `test_and_set_bit(0, ...)` |
| `tbt_sock_detach()` | `tbt_sock.c:377` | `tbt_sk_ops_restore` + cancel w_read/w_drain/w_ctrl |
| `tbt_sk_ops_save/restore()` | `tbt_sock.c:397-427` | P0-rmmod 新增 |
| `tbt_sock_abort_on_close()` | `tbt_sock.c:429` | RST 关闭 |
| `tbt_stream_release_socket()` | `tbt_sock.c:438` | detach + cancel w_kill + release；**不能从 w_kill 调用** |
| `tbt_stream_start()` | `tbt_sock.c:468` | `far_sid != 0` → `-EBUSY` |
| `tbt_far_open_seen()` | `tbt_sock.c:515` | far 采纳后 announce + attach `sess->target` |
| `tbt_target_attach()` | `tbt_sock.c:533` | 按 `far_sid` 找流 |
| `tbt_target_detach()` | `tbt_sock.c:560` | 按 `far_sid` 找流 |
| `tbt_tx_ack()` | `tbt_tx.c:1203` | 当前无 sid 参数，新增了 `fb_sample` |
| `tbt_tx_free_queue()` | `tbt_tx.c:2027` | 队列释放 |
| `tbt_tx_free_stream()` | `tbt_tx.c:2041` | retain 释放 |
| `tbt_rx_bind_stream()` | `tbt_rx.c:108` | 单 `far_sid` 采纳 |
| `tbt_rx_data()` | `tbt_rx.c:131` | 无 DEAD 检查 |
| `tbt_rx_frame()` 的 uack | `tbt_rx.c:316` | `tbt_tx_ack(sess,c,hdr->uack)` |
| `tbt_car_poll_work()` | `tbt_carrier.c:190` | provisional 分支 `:208`；新增 `parser->have` 重试 `:314-327` |
| `tbt_car_rx_work()` | `tbt_carrier.c:403` | 全局 pause 扫描 `:419-436`；结尾 `:473-482` |
| `tbt_carrier_adopt()` | `tbt_carrier.c:1352` | 调度 w_rx 在 `:1394` |
| `tbt_target_work_fn()` | `tbt_carrier.c:1401` | 失败置 `target_started=false` |
| `tbt_carrier_open_target()` | `tbt_carrier.c:1446` | 只看 `target_started`，不看 `sess->target` |

### 0.4.2 P0 rebase 结论

- P0-rmmod 已落地：`tbt_sk_ops_save/restore`（`tbt_sock.c:397-427`）、
  `tbt_stream_release_socket`（`:438`）、`tbt_sock_abort_on_close`（`:429`）；
  `tbt_session_destroy` 的 stream 循环已调用 release。
- 仍缺/仍有风险：`tbt_kill_work`（`:321`）自己 detach/release 且
  `sk==NULL` 早返回；`tbt_stream_release_socket` 会 `cancel_work_sync(w_kill)`，
  不能从 `w_kill` 调用；destroy 仍是 `tbt_tx_free_queue()`（`:750`）先于
  `cancel_work_sync(w_kill)`（`:774`）；`tbt_stream_kill` 无单赢家/无 w_kill 引用；
  `tbtun.h` 无墓碑/pending 字段。
- perf 已在 `tbt_car_poll_work()` 里加了 `parser->have` 重试（`:314-327`），
  但 provisional 分支仍在 `:208-228` 提前返回，**看不到**这段重试；
  P0-B 仍然必要，并且要把 `c->parser->have` 也纳入 provisional 的补救条件
  （一个只到一半的 HELLO 会让 `copied_seq == rcv_nxt`，只看字节计数会漏）。
- `tbt_car_rx_work()` 结尾（`:473-482`）仍只有 `tbt_carrier_lost()`，
  没有 `copied_seq != rcv_nxt` 自调度；P0-C 仍然必要。
- 全局 pause 扫描仍在 `:419-436`；P0-A 的 provisional 跳过仍然必要。
- P0 行号按上表替换：`:391-406` → `:419-436`，`:379` → `:403`，
  `:1320-1321` → `:1394`。P0 不改 `tbtun.h`。

### 0.4.3 P1-core 最小改动集（逐项）

| # | 改动 | 当前锚点 | 只改 session/sock？ | 需改 tx/rx？ | tbtun.h |
|---|---|---|---|---|---|
| 1 | `tbt_stream_kill` 单赢家 + w_kill 引用 + 清 `far_sid`/ACK/墓碑 | `tbt_session.c:643` | 是 | 否 | `tombstone_until_ns` |
| 2 | `tbt_stream_release_socket_now`（不自 cancel） | `tbt_sock.c:438` | 是 | 否 | 否 |
| 3 | `tbt_kill_work` 无条件进墓碑 + schedule reaper/pending | `tbt_sock.c:321` | 是 | 否 | 否 |
| 4 | TX 队列 purge（旧 sid 的 DATA/PREDICT/CANCEL） | `tbt_session.c` 新增 | 是（直接走 `sess->tx_q`） | 否 | 否 |
| 5 | destroy 顺序：先停 `w_kill`/`w_ctrl`，再 free queue | `tbt_session.c:726` | 是 | 否 | 否 |
| 6 | pending 队列（churn 0 refused） | `tbt_sock.c:468` + `tbt_session.c` | 是 | 否 | `pending`/`n_pending`/`w_pending` |
| 7 | target 重启（旧 target unpublish + kick） | `tbt_sock.c:438`/`:515` | 是（重置 `target_started`） | 否 | 否 |
| 8 | ACK 的 sid 检查 | `tbt_tx.c:1203` + `tbt_rx.c:316` | **否** | **是（tx+rx 各一处）** | 否 |
| 9 | DEAD/tombstone 的 RX 丢弃与 far 采纳保护 | `tbt_rx.c:108`/`:131` | **否** | **是（rx 两处）** | 否 |
| 10 | 会话级墓碑 reaper | `tbt_session.c` 新增 | 是 | 否 | `w_reap`（或全局静态 work） |

**与 P0-rmmod 新代码的衔接点**：

- 所有 socket release 前先 `tbt_sock_detach()`；它内部
  `tbt_sk_ops_restore(sk, &st->sess->def_ops)`，不要写回 `tbt_pub_*`。
- `tbt_stream_release_socket_now()` 复用现有 `tbt_stream_release_socket()`
  的 detach/abort/release body，但**去掉** `cancel_work_sync(&w_kill)`；
  公共版本保留 cancel，供 destroy/错误路径调用。
- destroy first pass 用 `tbt_sock_detach()` 停 `w_read/w_drain/w_ctrl`
  并恢复核心回调；second pass 再用公共 `tbt_stream_release_socket()`。
- pending socket 失败/关闭/销毁路径必须先
  `tbt_sk_ops_restore(sock->sk, &t->def_ops)`，再
  `tbt_sock_abort_on_close()` / `sock_release()`。
- target 的 orphan/旧 socket 释放同样走 `tbt_sk_ops_restore` +
  `tbt_sock_abort_on_close`，不要只依赖 `tbt_target_detach(s, ts)`（它按
  `far_sid` 找流，`far_sid` 可能已为 0）。

**结论**：

- **P1-core 安全版** = 1-10。tx 1 处（`tbt_tx_ack` 加 sid 参数），rx 3 处
  （`tbt_rx_frame` 传 sid、`tbt_rx_data` 的 DEAD 检查、`tbt_rx_bind_stream`
  改成 `tbt_session_bind_sid()` 薄封装），不改 wire、不改帧布局。perf 正在改
  `tbt_tx.c`，tx 那处必须等 S1 冻结后 rebase；rx 三处冲突低。
- **P1-S 实验版** = 1-7 + 10（不碰 tx/rx）。它能修 `near.second_conn`，
  也可能让 `pub.churn` 通过，但**不能保证正确**：
  - 旧流迟到的 `uack` 会被 `tbt_tx_ack` 当成新流的 ACK（没有 sid 检查）；
  - 旧 sid 的迟到 DATA 在 near 侧会被判 PROTO/毒化载体；在 far 侧可能被
    当成新流采纳（没有墓碑检查）。
  因此 P1-S 只能作为“先让 churn 跑起来”的调试步骤，不应作为合并版本。
- **target 重启不需要改 `tbt_carrier.c`**：`tbt_carrier_open_target()` 只看
  `target_started`。在 `tbt_stream_release_socket_now()` 里先清
  `sess->target`、置 `target_started=false`，release 后调用
  `tbt_carrier_open_target()` 即可；`tbt_far_open_seen()` 只负责“orphan target
  绑定”，不再把旧 target 绑给新流。

### 0.4.4 P1-S：只改 `tbt_session.c` / `tbt_sock.c` 的实验版

**目标**：不改 `tbt_tx.c` / `tbt_rx.c` / `tbt_carrier.c`，先把
`near.second_conn` 和（可能）`pub.churn` 跑起来。**不是最终合并版本。**

改动集：

1. `tbt_stream_kill()`（`tbt_session.c:643`）：
   - 用 `test_and_set_bit(TBT_STREAM_F_DYING, &st->attached)` 单赢家；
   - `state=DEAD`；在 `sess->lock` 下清 `far_sid`、复位
     `tx_ack/rx_ack/peer_paused/holes/predict`；打墓碑截止时间；
   - 调 `tbt_tx_purge_stream()`（**在 `tbt_session.c` 内实现**，直接走
     `sess->tx_q`，不碰 `tbt_tx.c`）；
   - `refcount_inc` 后 queue `w_kill`。
2. `tbt_kill_work()`（`tbt_sock.c:321`）：
   - 调 `tbt_stream_release_socket_now()`（新增 static，不 cancel 自己）；
   - 非 closing 时 schedule 墓碑 reaper / pending；
   - `tbt_stream_put()`。
3. `tbt_stream_release_socket()`（`tbt_sock.c:438`）：
   - 公共入口先 `cancel_work_sync(&w_kill)`，再 `_now`；
   - `_now` 先清 `sess->target`/`target_started`，再 detach/release，
     最后 `tbt_carrier_open_target()`（现有函数只检查 `target_started`）。
4. `tbt_stream_start()`（`tbt_sock.c:468`）：
   - 总是把 accepted socket 放进 pending；`w_pending` 串行启动一条；
   - `tbt_stream_start_now()` 保留现有函数体，去掉 `busy` 分支。
5. `tbt_far_open_seen()`（`tbt_sock.c:515`）：
   - 只 attach “ours（`sk_user_data == st`）”或 orphan
     （`sk_user_data == NULL`）的 target；旧 target 等旧 `w_kill` 释放后 kick。
6. 墓碑 reaper：每 200 ms 扫 `sess->streams`，DEAD 且到期则 `hlist_del_rcu`
   + `tbt_stream_put`；`w_reap` 用 `system_unbound_wq`。

**P1-S 不能修的两个正确性缺口**：

- `tbt_tx_ack()`（`tbt_tx.c:1203`）没有 sid 参数：旧流的迟到 `uack` 可能被
  当成新流的 ACK。P1-S 只能靠“旧流已 close、far 不再发旧帧”的概率；
  10k churn 可能过，但不是证明。
- `tbt_rx_data()` / `tbt_rx_bind_stream()`（`tbt_rx.c:131`/`:108`）没有
  DEAD/墓碑检查：旧 sid 的迟到 DATA 在 near 侧会被判 PROTO/毒化载体，
  在 far 侧可能被当成新流采纳。P1-S 不解决。

**结论**：P1-S 只能作为“先跑 churn 看窗口”的调试分支；验收通过也不能
作为合并依据。真正可合并的 P1-core 必须加 0.4.3 的第 8、9 项（tx/rx 各一处）。

#### P1-S 的“零 tbtun.h 字段”做法（可行但复杂）

如果 perf 完全不允许改 `tbtun.h`，P1-S 可以用 `tbt_session.c` 里的全局静态
结构，而不是往 `struct tbt_session` 加字段：

```c
/* tbt_session.c */
static LIST_HEAD(tbt_pending_all);
static DEFINE_MUTEX(tbt_pending_mu);
static struct work_struct tbt_pending_work;

struct tbt_pending {
    struct list_head node;
    struct tbt_tunnel *t;   /* holds a tunnel ref, so sess lifetime is safe */
    struct socket *sock;
};

static LIST_HEAD(tbt_tomb_all);
static DEFINE_MUTEX(tbt_tomb_mu);
static struct delayed_work tbt_reap_work;

struct tbt_tomb {
    struct list_head node;
    struct tbt_tunnel *t;
    u32 sid;
    u64 until_ns;
};
```

- `tbt_stream_start()` 调 `tbt_pending_enqueue(sess, sock)`；wrapper 里
  `refcount_inc(&sess->tunnel->refs)`；`w_pending` 弹出后检查 `t->sess`
  与 `closing`，再启动。
- `tbt_stream_kill()` 调 `tbt_tomb_add(sess, sid)`；全局 reaper 每 200 ms
  扫 `tbt_tunnels`（持 `tbt_lock`）或扫 tomb 表，按 `(t,sid)` 找流并摘链。
- `tbt_session_destroy()` 调 `tbt_pending_drain(t)` / `tbt_tomb_drop(t)`。

这确实可以做到**不改 `tbtun.h`**，但代价是：

- 全局 mutex/work，所有 session 串行；
- wrapper 必须持 tunnel ref，销毁路径要 drain；
- 墓碑表与 `sess->streams` 双份索引，容易出现“表里有、哈希里没有”的竞态。

**建议**：如果 perf 能接受 5 个字段（见 0.4.5），用 per-session 结构；
零字段方案只作为“绝对不允许改头文件”的最后手段。

### 0.4.5 tbtun.h 最小 diff

如果允许改头文件，P1 的最小字段集是：

```c
struct tbt_stream {
    ...
    u64 tombstone_until_ns;   /* +8 bytes */
};

struct tbt_session {
    ...
    struct delayed_work w_reap;   /* +~56 bytes */
    struct list_head pending;     /* +16 */
    u16 n_pending;                /* +2 */
    struct work_struct w_pending; /* +~32 */
};
```

`struct tbt_pending { struct list_head node; struct socket *sock; }` 可以只在
`tbt_session.c` 里定义，不进 `tbtun.h`。

**如果只允许加 1 个字段**：只加 `tombstone_until_ns`；`w_reap` 改成
`tbt_session.c` 里的全局 `static struct delayed_work`，在 `tbt_lock` 下遍历
`tbt_tunnels` 扫描（会话销毁也在 `tbt_lock` 下，生命周期可串行化）。
pending 队列改成 `tbt_session.c` 里的全局 list + 全局 mutex + 全局 work，
wrapper 持 `struct tbt_tunnel *` 的 ref。**不推荐**：全局锁/生命周期复杂，
但确实可以做到不改 `tbtun.h`。

**如果完全不允许改 `tbtun.h`**：用 0.4.4 的全局静态结构——全局 pending
list + 全局 tomb 表 + 全局 reaper，wrapper 持 tunnel ref。这能避免改头文件，
但全局锁/双索引/销毁 drain 的复杂度明显更高。不要用 `st->hole_since_ns`
重载墓碑截止时间：DEAD 后它确实空闲，但把两个语义塞进一个字段，评审应拒绝。

**另外**：2.9.1 的落点需要 `tbtun.h` 加 `enum tbt_bind_result` 和
`tbt_session_bind_sid()` 原型；这不是结构体字段，但同样改头文件。若连原型
都不想加，可以把 `tbt_session_bind_sid()` 的结果编码成 `int` 返回值
（0/1/2/3），但 enum 更可读。

### 0.4.6 验收顺序（P1）

1. **P1-core 先过 `pub.churn` 10000**：`live=10000/refused=0/other=0`；
   结束后 `far_sid=0`、墓碑 grace 后归零、无 kmalloc/socket 泄漏。
2. **`near.second_conn` rc=0**。
3. **探针 B 不回归**。
4. **`pub.two` 保持旧契约**：当前 adversarial 期望 B refused；P1 的 pending
   队列会让 B 静默等待/超时，不能把它改成“两条都可用”。P3 才改
   `mode_two` 的期望和 README/findings。

P3 的验收顺序才是：churn/second_conn 仍绿 → `pub.two` 新契约（B 也回显）
→ 100 并发。P1 不把 `pub.two` 新契约作为门槛，避免测试契约和实现同时变。

---

## 1. P0：F6 载体读唤醒

### 1.1 现象与根因（当前代码）

`far.data_path` 的失败输出是 `rx_records=0 capture_runs=0 actor_calls=0
submit_bytes=0 tx_ack=0`，但 far 日志有 `carrier ACCEPTED`。失败在“读载体”，
不在流/ target。当前代码有三处会挡住 provisional 载体的 HELLO：

1. **全局 pause 扫描**（`tbt_carrier.c:419-436`）：只要会话里任一条 stream
   `paused && sk`，`tbt_car_rx_work()` 直接 `return`，不读**这条**载体。
   provisional 载体还没有 stream，却也被这条规则挡住。
2. **provisional 轮询盲区**（`tbt_carrier.c:208-217`）：`c->provisional` 分支
   只检查 join deadline，然后重排 50 ms 轮询；它**不检查** `copied_seq != rcv_nxt`，
   而 `:309-313` 的补救只在 provisional 分支之后。
3. **`w_rx` 一次性**：`tbt_carrier_adopt()` 在 `tbt_carrier.c:1394` 只
   `schedule_work(&c->w_rx)` 一次；`tbt_car_rx_work()` 结束时没有“还有未读字节
   就再调度自己”的兜底。若第一次读发生在 HELLO 入队之前，而后续
   `sk_data_ready` 被合并/丢失，HELLO 会一直躺在接收队列，直到 5 s join deadline。

> 结论：P0 不是协议问题，是读调度问题。三处都要修，缺一处仍可能间歇失败。

### 1.2 P0-A：provisional 载体跳过全局 pause 扫描

**文件/函数**：`tbt_carrier.c`，`tbt_car_rx_work()`（当前 `:403`）。

**当前片段**（`:419-436`）：

```c
    rcu_read_lock();
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry_rcu(st, &c->sess->streams[i], node)
        {
            if (READ_ONCE(st->paused) && READ_ONCE(st->sk))
            {
                c->sess->rx_paused_skips++;
                rcu_read_unlock();
                return;
            }
        }
    }
    rcu_read_unlock();
    desc.arg.data = c;
```

**替换为**：

```c
    /* A provisional carrier has no stream data yet; its only job is to read
     * HELLO.  Another stream's delivery backpressure must not hide it. */
    if (!c->provisional)
    {
        rcu_read_lock();
        for (i = 0; i < TBT_STREAM_HT; i++)
        {
            hlist_for_each_entry_rcu(st, &c->sess->streams[i], node)
            {
                if (READ_ONCE(st->paused) && READ_ONCE(st->sk))
                {
                    c->sess->rx_paused_skips++;
                    rcu_read_unlock();
                    return;
                }
            }
        }
        rcu_read_unlock();
    }
    desc.arg.data = c;
```

**边界**：

- 只对 `c->provisional` 跳过；已 join 的载体保持原行为，避免改变 P1 之前的背压语义。
- P3 会把 pause 判断移进 `tbt_rx_data()`（每流），届时删除这段全局扫描；P0 不做。

### 1.3 P0-B：provisional 轮询补救未读字节

**文件/函数**：`tbt_carrier.c`，`tbt_car_poll_work()`（当前 `:190`）。

**当前片段**（`:208-217`）：

```c
    if (c->provisional)
    {
        if (now > c->join_deadline_ns)
        {
            atomic_set(&c->stopping, 1);
            schedule_work(&sess->w_replace);
            return;
        }
        schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
        return;
    }
```

**替换为**：

```c
    if (c->provisional)
    {
        struct sock *psk = READ_ONCE(c->sk);

        if (now > c->join_deadline_ns)
        {
            atomic_set(&c->stopping, 1);
            schedule_work(&sess->w_replace);
            return;
        }
        /* The one-shot w_rx may have run before HELLO was queued.  The poll
         * is the only rescue while the carrier is provisional; without this
         * check a lost wake leaves HELLO unread until the join deadline. */
        if (psk && READ_ONCE(psk->sk_state) == TCP_ESTABLISHED &&
            (READ_ONCE(tcp_sk(psk)->copied_seq) !=
                 READ_ONCE(tcp_sk(psk)->rcv_nxt) ||
             c->parser->have))
        {
            schedule_work(&c->w_rx);
        }
        schedule_delayed_work(&c->w_poll, msecs_to_jiffies(50));
        return;
    }
```

**边界**：

- 不 `lock_sock`；这是快照读取，真正读在 `w_rx` 里加锁。误判最多多调度一次，安全。
- `psk` 可能被 retire 清空；`READ_ONCE(c->sk)` + NULL 检查。
- `tcp_sk(psk)` 只在 `sk_state == ESTABLISHED` 时解引用；关闭中的 socket 交给 join deadline。

### 1.4 P0-C：`tbt_car_rx_work()` 结束自调度

**文件/函数**：`tbt_carrier.c`，`tbt_car_rx_work()`（当前 `:403-482`）。

**当前结尾**（`:473-482`）：

```c
    /* The peer closed the connection: a carrier never sends FIN, so a zero
     * read with a non-established socket is the end of this path. */
    if (drained <= 0 && READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
        tbt_carrier_lost(c->sess, c);

    /* Parsing may have enqueued a control frame (OPEN, FBK). ... */
}
```

**替换为**：

```c
    /* The peer closed the connection: a carrier never sends FIN, so a zero
     * read with a non-established socket is the end of this path. */
    if (drained <= 0 && READ_ONCE(sk->sk_state) != TCP_ESTABLISHED)
    {
        tbt_carrier_lost(c->sess, c);
    }
    else if (READ_ONCE(c->sk) == sk &&
             (READ_ONCE(tcp_sk(sk)->copied_seq) !=
                  READ_ONCE(tcp_sk(sk)->rcv_nxt) ||
              c->parser->have))
    {
        /* Bytes arrived after tcp_read_sock() stopped but before this work
         * returned.  Re-arm; sk_data_ready may have been coalesced. */
        schedule_work(&c->w_rx);
    }
```

**边界**：

- `READ_ONCE(c->sk) == sk` 防止 retire 已经把 `c->sk` 清空后解引用旧 sk。
- 只重排 `w_rx`，不在 work 内循环 `tcp_read_sock`，避免长时间占用工作队列。
- 若 `drained > 0` 且仍有未读字节，也重排；这对大 backlog 是必要的。

### 1.5 P0-D：观测（可选，避免 tbtun.h 冲突）

**推荐**：在 `tbt_session` 加 `u64 rx_self_reschedules;`，在两处 `schedule_work(&c->w_rx)`
前自增。若 perf 不想动 `tbtun.h`，退化为：

```c
if (debug >= 2)
    pr_info_ratelimited("tbtun: carrier %u self-reschedule copied=%u rcv_nxt=%u\n",
                        c->index, tcp_sk(sk)->copied_seq, tcp_sk(sk)->rcv_nxt);
```

并在 `tbt_procfs.c` 的 stats 里复用现有 `poll_rescues`（语义近似：轮询/自调度救回）。
**P0 验收不依赖这个计数器**；它只用于证明修复生效。

### 1.6 P0 验收

1. `far.data_path`：`rx_records > 0`、`capture_runs > 0`、`actor_calls > 0`；
   ping/pong 字节精确，重复 seq 不投递。
2. `far.hello_garbage` 仍 PASS：有效 HELLO 后 64 字节垃圾必须关闭载体，不崩。
3. `far.wrong_nonce` / `far.dup_index` / `far.chunk_huge` / `far.split_record` /
   `far.chunk_oversize` / `far.no_host` / `far.headers_big` 在**多次运行**中稳定
   按预期关闭/拒绝，不再间歇超时。
4. 探针 A/B 不回归：`make qemu-test --probes A,B`（或当前等价命令）通过，
   吞吐/丢包指标不低于基线。
5. 连续 100 次 far 载体 accept，每次 HELLO 都被解析（`rx_adopt_events=100`，
   若计数器存在）；`dmesg` 无新增 warning/oops。

### 1.7 P0 与 S1 的关系

- P0 只改 `tbt_carrier.c` 的三个函数；S1 也改 `tbt_carrier.c`。**S1 合入后再应用**，
  按函数名/片段 rebase，不要按当前行号硬打。
- P0 不改 `tbt_tx.c`；若 S1 已改 `tbt_car_rx_work` 的 pause 逻辑，以 S1 为准，
  只补“provisional 跳过”和“自调度”两点。
- P0 不涉及 `tbtun.h`；可选计数器除外。

---

## 2. P1：顺序复用（仍一条活跃 stream）

### 2.1 P1 的不变量

1. 一个 session 最多一条**活跃** stream（`sess->far_sid != 0`）。
   `sid == 0` 保留为“无流”哨兵；分配器必须跳过 0。
2. 一条 stream 进入终态后，它的 sid 在 `TBT_TOMBSTONE_MS` 内仍是“墓碑”：
   迟到帧被丢弃，不得被当成新流。
3. `sess->far_sid` 只在“当前流进入终态”或“新流建立”时改变；改变与
   `tx_ack/rx_ack` 复位在同一把 `sess->lock` 下。
4. 新流的 `tx_seq`/`rx_next` 从 0 开始；旧流的迟到 ACK 必须被 sid 检查拒绝。
5. 哈希引用由墓碑 reaper 释放；`w_kill` 自己持一个引用；socket 在 `w_kill`
   里释放；`tbt_stream_kill` 不释放哈希引用。
6. 任何可能入队 `sess->tx_q` 的工作项（`w_ctrl`/`w_kill`/`w_read`/`w_drain`）
   必须在 `tbt_tx_free_queue()` 之前停住。
7. 关闭一条流只影响该流；P1 不允许第二条活跃流，但**允许把新 accept 排队**，
   不向客户端返回 `-EBUSY`。

### 2.2 需要改的字段（tbtun.h）

perf 现在尽量不动 `tbtun.h`。P1 的**最小**字段集如下；先列出来，合并时协调。

#### 必需（P1 的墓碑/队列）

| 结构 | 字段 | 类型 | 用途 |
|---|---|---|---|
| `tbt_stream` | `tombstone_until_ns` | `u64` | 该流作为墓碑保留到何时；reaper 据此摘链 |
| `tbt_session` | `w_reap` | `struct delayed_work` | 会话级墓碑扫描器（200 ms） |
| `tbt_session` | `pending` | `struct list_head` | 已 accept、等待唯一活跃流让出的公共 socket |
| `tbt_session` | `n_pending` | `u16` | 队列长度，上限 128 |
| `tbt_session` | `w_pending` | `struct work_struct` | 串行启动 pending 队列（可睡眠） |

#### 可选（观测/调试）

| 结构 | 字段 | 用途 |
|---|---|---|
| `tbt_session` | `u64 rx_self_reschedules` | P0 自调度计数；不加就用 `poll_rescues`/debug |
| `tbt_session` | `u32 tombstone_forced` | 墓碑上限触发强制回收计数 |

#### 不需要新字段的位

`struct tbt_stream.attached` 是 `unsigned long`，当前只用 bit 0。P1 复用：

```c
#define TBT_STREAM_F_ATTACHED 0  /* existing tbt_sock_attach */
#define TBT_STREAM_F_DYING    1  /* tbt_stream_kill 的单赢家 */
#define TBT_STREAM_F_UNLINKED 2  /* reaper 已摘链 */
```

`tbt_sock_attach` 里的字面量 `test_and_set_bit(0, ...)` 改成
`TBT_STREAM_F_ATTACHED`；`tbt_sock_detach` 的 `test_and_clear_bit(0, ...)` 同步。
这样 `dying`/`unlinked` 不占结构体字段。

**注意**：除上述结构体字段外，`tbtun.h` 还需要 `TBT_STREAM_F_*` 宏
（`tbt_sock.c` / `tbt_session.c` 共用）；`tbt_tx_purge_stream()` 放在
`tbt_session.c` 里 static，不需要头文件原型。

**如果 perf 完全不允许改 tbtun.h**：

- P0 可以零字段（用 `poll_rescues`/debug）。
- P1 **不能**零字段：至少需要 `tombstone_until_ns` + `w_reap`；否则只能把死流
  永久留在哈希（10k churn 泄漏）或不做墓碑（迟到帧误建流）。pending 队列需要
  `pending`/`n_pending`/`w_pending`。请把这 5 个字段作为 P1 的合并前置。

### 2.3 终态迁移：`tbt_stream_kill()` 单赢家

**文件/函数**：`tbt_session.c`，`tbt_stream_kill()`（当前 `:643`）。

**当前片段**：

```c
void tbt_stream_kill(struct tbt_stream *st, u8 reason)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        return;
    st->kill_reason = reason;
    WRITE_ONCE(st->state, TBT_ST_DEAD);
    if (sk && sk->sk_socket)
        kernel_sock_shutdown(sk->sk_socket, SHUT_RDWR);
    queue_work(system_dfl_wq, &st->w_kill);
}
```

**替换为**：

```c
void tbt_stream_kill(struct tbt_stream *st, u8 reason)
{
    struct sock *sk = READ_ONCE(st->sk);

    /* One winner.  A read-then-write let two carriers both queue w_kill and
     * leak one reference. */
    if (test_and_set_bit(TBT_STREAM_F_DYING, &st->attached))
        return;
    st->kill_reason = reason;
    WRITE_ONCE(st->state, TBT_ST_DEAD);

    /* Detach the sid from the session *now*: this is the gate that lets the
     * next queued public connection start.  It also resets the session ack
     * space under sess->lock and stamps the tombstone deadline. */
    tbt_stream_detach_from_session(st);

    /* Drop queued DATA/PREDICT/CANCEL for the dead sid.  Keep FIN/RST: the
     * far side still needs the close control frame. */
    tbt_tx_purge_stream(st->sess, st->sid);

    if (sk && sk->sk_socket)
        kernel_sock_shutdown(sk->sk_socket, SHUT_RDWR);

    /* w_kill can outlive the hash reference; it owns this reference. */
    refcount_inc(&st->refs);
    if (!queue_work(system_dfl_wq, &st->w_kill))
        tbt_stream_put(st);
}
```

**边界**：

- `test_and_set_bit` 是单赢家；输家直接返回，不加引用、不排队。
- `tbt_stream_detach_from_session` 只做短 spinlock 操作，不睡眠；可以在
  carrier RX 的原子上下文调用。
- `tbt_tx_purge_stream` 只取 `sess->tx_lock`，不睡眠；也允许在 RX 上下文调用。
- `refcount_inc` 后 `queue_work`；若队列已 pending（理论上不会，因为单赢家），
  `queue_work` 返回 false，必须 `put`，否则泄漏引用。
- 不在 `tbt_stream_kill` 里释放 socket/哈希引用；那是 `w_kill`/reaper 的职责。

#### 2.3.1 `tbt_tx_purge_stream()` 片段

**文件/函数**：`tbt_session.c`，新增 `static` 函数；**不碰 `tbt_tx.c`**，
也不需要 `tbtun.h` 原型（只在本文件调用）。

```c
static void tbt_tx_purge_stream(struct tbt_session *sess, u32 sid)
{
    struct tbt_frame *fr, *tmp;

    spin_lock(&sess->tx_lock);
    list_for_each_entry_safe(fr, tmp, &sess->tx_q, list)
    {
        u8 type = fr->hdr.type;

        if (fr->hdr.sid_lo != sid)
            continue;
        /* Keep OPEN/FIN/RST: the far side still needs the close control.
         * Drop only payload-bearing and speculative frames. */
        if (type != TBT_F_DATA && type != TBT_F_PREDICT &&
            type != TBT_F_CANCEL)
            continue;
        list_del(&fr->list);
        sess->tx_queued -= fr->len;
        kfree(fr);
    }
    spin_unlock(&sess->tx_lock);
}
```

**边界**：

- P1 的 `struct tbt_frame` 还没有 `fr->st` 引用（P2 才加），所以这里只
  `kfree(fr)`，不 put 流引用。
- `fr->ret` 指向旧流的 retain；帧被释放后不再解引用，retain 随旧流 RCU 释放。
- 若 S1 已经给帧加了字段，按 S1 的释放 helper 替换 `kfree(fr)`。

### 2.4 清 `far_sid`、复位会话 ACK、打墓碑

**文件**：`tbt_session.c`，新增 `tbt_stream_detach_from_session()`；
在 `tbt_stream_kill` 里调用。

**建议片段**：

```c
static void tbt_stream_detach_from_session(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->far_sid) == st->sid)
    {
        WRITE_ONCE(sess->far_sid, 0);

        /* P1 is still single-stream: the next stream restarts at seq 0, so
         * the session ack space must restart with it.  P2 moves these into
         * tbt_stream and deletes this reset. */
        sess->tx_ack = 0;
        sess->rx_ack = 0;
        sess->peer_paused = false;
        sess->last_ack_ns = 0;
        sess->pause_cleared_ns = 0;
        sess->predict_seq = 0;
        sess->predict_ns = 0;
        sess->fbk_frames = 0;
        sess->hole_head = sess->hole_tail = 0;
    }
    st->tombstone_until_ns = ktime_get_ns() +
        (u64)TBT_TOMBSTONE_MS * NSEC_PER_MSEC;
    spin_unlock(&sess->lock);
}
```

**要点**：

- 只在 `far_sid == st->sid` 时复位；如果新流已经先占了 `far_sid`（例如旧流
  的 RST 迟到），不要动新流的状态。
- `sess->holes` 数组不 `memset`；`hole_head=hole_tail=0` 即清空 FIFO。
- `tombstone_until_ns` 无论是否当前流都要打；墓碑用于挡迟到帧。
- **不要**在这里清 `sess->target`/`target_started`；那是 socket release 的职责
  （见 2.6）。否则新 target 可能在旧 target 释放前启动。

### 2.5 ACK 串行化：旧 ACK 不得污染新流

P1 仍用 `sess->tx_ack/rx_ack`，必须解决“旧流 ACK 在新流复位后到达”的竞态。

#### 2.5.1 `tbt_tx_ack` 增加 sid 参数

**文件/函数**：`tbt_tx.c`，`tbt_tx_ack()`（当前 `:1203`）；调用点在
`tbt_rx.c:316`。

> **S1 衔接**：当前 `tbt_tx_ack()` 已含 perf 的 `fb_sample` 逻辑
> （`:1203` 之后）。本次只加 `sid` 参数和开头的 sid 校验，**保留**
> `fb_sample`/RTT 采样 body；不要用旧稿的整段函数覆盖。

**当前签名**：

```c
void tbt_tx_ack(struct tbt_session *sess, struct tbt_carrier *c, u64 uack)
```

**改为**：

```c
void tbt_tx_ack(struct tbt_session *sess, struct tbt_carrier *c,
                u64 uack, u32 sid)
```

**当前开头**：

```c
    if (!uack)
        return;
    st = tbt_session_stream(sess);
    if (!st)
        return;
    if (uack > READ_ONCE(st->tx_seq))
        uack = READ_ONCE(st->tx_seq);
    if (uack <= READ_ONCE(sess->tx_ack))
    {
        tbt_stream_put(st);
        return;
    }
    WRITE_ONCE(sess->tx_ack, uack);
    sess->last_ack_ns = tbt_now_ns();
```

**替换为**：

```c
    if (!uack || !sid)
        return;
    st = tbt_stream_get(sess, sid, false, false);
    if (!st)
        return;

    /* Serialize with tbt_stream_detach_from_session(): if the sid is no
     * longer current, this is a late ACK for a tombstoned stream.  Dropping
     * it prevents an old high uack from acknowledging the new stream. */
    spin_lock(&sess->lock);
    if (sid != READ_ONCE(sess->far_sid))
    {
        spin_unlock(&sess->lock);
        tbt_stream_put(st);
        return;
    }
    if (uack > READ_ONCE(st->tx_seq))
        uack = READ_ONCE(st->tx_seq);
    if (uack <= sess->tx_ack)
    {
        spin_unlock(&sess->lock);
        tbt_stream_put(st);
        return;
    }
    sess->tx_ack = uack;
    sess->last_ack_ns = tbt_now_ns();
    spin_unlock(&sess->lock);
```

**调用点**（`tbt_rx.c:316`）：

```c
    /* Every frame carries the sender's receive offset for the opposite
     * direction; only the current stream may consume it. */
    if (hdr->sid_lo)
        tbt_tx_ack(sess, c, hdr->uack, hdr->sid_lo);
```

**边界**：

- `FBK`/`HELLO`/`PAD`/`RETIRE` 的 `sid_lo == 0`，不调用。
- 旧流的 ACK 即使 `tbt_rx_frame` 在 `far_sid` 清零前读到旧 sid，`tbt_tx_ack`
  在 `sess->lock` 下重新检查 sid，会丢弃。
- `tbt_tx_holes_repaired(sess)` 仍在函数末尾；它按当前 `far_sid` 处理，sid 已
  校验为当前，安全。
- 这是 P1 唯一需要改 `tbt_tx.c` 的 ACK 逻辑；S1 合入后 rebase。

#### 2.5.2 `tbt_rx_data` 的 `rx_ack` 更新

**文件/函数**：`tbt_rx.c`，`tbt_rx_data()`（当前 `:131`）。

**当前片段**（在 `st->rx_lock` 内，约 `:217`）：

```c
    WRITE_ONCE(sess->rx_ack, st->rx_next);
    spin_unlock_bh(&st->rx_lock);
```

**替换为**：先解锁，再在 `sess->lock` 下按 sid 更新：

```c
    spin_unlock_bh(&st->rx_lock);

    /* The session ack is reset when the current stream dies.  Publish this
     * stream's rx_next only if it is still the current stream, or a late
     * frame can move the new stream's uack backwards/forwards. */
    spin_lock(&sess->lock);
    if (READ_ONCE(sess->far_sid) == st->sid)
        WRITE_ONCE(sess->rx_ack, READ_ONCE(st->rx_next));
    spin_unlock(&sess->lock);
```

**边界**：

- 不要 `spin_lock(&sess->lock)` 嵌套在 `st->rx_lock` 里；先放 `rx_lock`。
- `st->rx_next` 是 u64，`READ_ONCE` 在 64 位平台原子；即便另一个载体刚推进，
  取到更大的值也是单调安全的。
- 这个 sid 检查是 P1 的临时补丁；P2 把 ack 搬进 `tbt_stream` 后删除。

### 2.6 墓碑与 reaper

#### 2.6.1 结构

- `struct tbt_stream` 增加 `u64 tombstone_until_ns`。
- `struct tbt_session` 增加 `struct delayed_work w_reap`。
- 不需要 `n_tombstones`：reaper 扫 64 个桶即可；只有 64 个桶头，成本可忽略。

#### 2.6.2 池

**`w_reap` 用 `system_unbound_wq`**。理由：

- 它每 200 ms 扫一次，可能短暂睡眠/被调度延迟；unbound 不占 per-CPU 默认池。
- `w_kill` 保持当前 `system_dfl_wq`，不改 S1/P0-rmmod 的池选择。
- `w_pending` 也用 `system_unbound_wq`（它会 `tbt_tx_announce`/`tbt_sock_attach`，
  可能睡眠）。

#### 2.6.3 reaper 片段

**文件**：`tbt_session.c`，新增：

```c
#define TBT_TOMBSTONE_MS 2000
#define TBT_REAP_INTERVAL_MS 200

static void tbt_reap_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_reap);
    struct tbt_stream *st, *tmp;
    u64 now = ktime_get_ns();
    bool more = false;
    int i;

    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry_safe(st, tmp, &sess->streams[i], node)
        {
            if (READ_ONCE(st->state) != TBT_ST_DEAD)
                continue;
            if (!st->tombstone_until_ns || now < st->tombstone_until_ns)
            {
                more = true;
                continue;
            }
            if (test_and_set_bit(TBT_STREAM_F_UNLINKED, &st->attached))
                continue;
            hlist_del_rcu(&st->node);
            /* tbt_stream_put() only does refcount_dec + call_rcu; it does not
             * free inline, so it is safe under sess->lock. */
            tbt_stream_put(st);
        }
    }
    spin_unlock(&sess->lock);

    if (more && !READ_ONCE(sess->closing))
        mod_delayed_work(system_unbound_wq, &sess->w_reap,
                         msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
}
```

**要点**：

- **哈希引用何时释放**：reaper 的 `hlist_del_rcu` 之后立刻 `tbt_stream_put`，
  释放哈希引用；如果还有 `w_kill` 引用/调用者引用，对象不 free，RCU 回调等
  最后一个引用。
- **socket 何时 release**：在 `w_kill` 的 `tbt_stream_release_socket_now()`，
  早于墓碑。墓碑期间对象没有 socket，只有队列/retain，迟到帧被丢弃。
- **迟到帧**：`tbt_rx_data` 看到 `state == DEAD` 直接丢弃；`tbt_rx_bind_stream`
  看到 tombstone sid 直接丢弃，不采纳。
- **墓碑期限**：2 s。`TBT_REPLAY_MS=1000`，载体 RTO 通常 < 1 s；2 s 覆盖
  最后一个 FIN/RST/重传。P1 的 16 位 sid 在 10k churn 内不会回绕（65k），
  所以不需要 40 位 sid。
- **墓碑上限**：P1 10k churn、每条 1 字节，2 s 内最多约 2000 条；每条
  `struct tbt_stream` 约 1 KiB，约 2 MiB，可接受。若 churn 更快，可在 reaper
  里加 `TBT_TOMBSTONE_MAX`（4096）强制回收最老的；P1 先不加，观测 `slab`。
- **reaper 与 `w_kill` 的竞争**：reaper 只摘哈希引用；`w_kill` 持有自己的
  引用，仍可安全 release/put。`test_and_set_bit(UNLINKED)` 防止二次摘。
- **reaper 与 `tbt_stream_get`**：查找在 `sess->lock` 下；摘链也在同一锁下；
  找到的流一定带引用。安全。

#### 2.6.4 调度 reaper

在 `tbt_kill_work()` 末尾（见 2.7）：

```c
    if (!READ_ONCE(sess->closing))
        mod_delayed_work(system_unbound_wq, &sess->w_reap,
                         msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
```

不要在 `tbt_stream_kill()` 里调度：它可能在原子上下文；`w_kill` 是进程上下文，
更适合。墓碑期限已在 `tbt_stream_kill()` 打好，reaper 晚 200 ms 无妨。

### 2.7 socket release 与 `w_kill` 竞争

#### 2.7.1 拆出不自 cancel 的 release

**文件/函数**：`tbt_sock.c`，`tbt_stream_release_socket()`（当前 `:438`）。

**当前片段**：

```c
void tbt_stream_release_socket(struct tbt_stream *st)
{
    struct sock *sk;
    struct socket *sock;

    tbt_sock_detach(st);
    cancel_work_sync(&st->w_kill);

    sk = READ_ONCE(st->sk);
    if (!sk)
        return;
    sock = sk->sk_socket;
    WRITE_ONCE(st->sk, NULL);
    if (!sock)
        return;
    if (READ_ONCE(st->sess->target) == sock)
        WRITE_ONCE(st->sess->target, NULL);
    tbt_sock_abort_on_close(sock);
    sock_release(sock);
}
```

**改为**：

```c
static void tbt_stream_release_socket_now(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct sock *sk = READ_ONCE(st->sk);
    struct socket *sock;
    bool kick = false;

    if (!sk)
        return;
    sock = sk->sk_socket;

    /* Unpublish the target *before* detach: a concurrent far OPEN must not
     * see the old target with sk_user_data already cleared and mistake it
     * for an orphan target. */
    spin_lock(&sess->lock);
    if (sess->target == sock)
    {
        sess->target = NULL;
        sess->target_started = false;
        kick = true;
    }
    spin_unlock(&sess->lock);

    tbt_sock_detach(st); /* idempotent: attached bit 0 */
    /* Publish NULL before the release: a concurrent kill reads st->sk. */
    WRITE_ONCE(st->sk, NULL);
    if (!sock)
        return;

    tbt_sock_abort_on_close(sock);
    sock_release(sock);

    /* The old target is gone; a new far-side stream may be waiting for one. */
    if (kick && READ_ONCE(sess->far_sid) && !READ_ONCE(sess->closing))
        tbt_carrier_open_target(sess->tunnel);
}

void tbt_stream_release_socket(struct tbt_stream *st)
{
    /* Public entry: safe from session destroy / error paths.  It must not be
     * called from w_kill itself (self cancel_work_sync deadlock). */
    cancel_work_sync(&st->w_kill);
    tbt_stream_release_socket_now(st);
}
```

**要点**：

- `tbt_kill_work()` 调 `tbt_stream_release_socket_now()`，**不**调
  `tbt_stream_release_socket()`；后者会 `cancel_work_sync(&st->w_kill)`，
  在 `w_kill` 自己里调用会自死锁。
- `tbt_session_destroy()` 仍调公共版本；它会等 `w_kill` 结束，然后 release。
- `sess->target=NULL` 与 `target_started=false` 在 `tbt_sock_detach()` **之前**
  一起设置（同一个 `sess->lock`）：这是“旧 target 不再发布”的时点。旧 socket
  的 detach/release 在其后完成；新 target 可以在旧 socket 释放期间开始，
  因为旧 target 已经无法被新的 OPEN 绑定。
- `tbt_carrier_open_target()` 需要加 `!sess->target` 检查（见 2.8）。

#### 2.7.2 `w_kill` 改造

**文件/函数**：`tbt_sock.c`，`tbt_kill_work()`（当前 `:321`）。

**当前片段**：

```c
static void tbt_kill_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_kill);
    struct sock *sk;
    struct socket *sock;

    might_sleep();
    tbt_tx_rst(st->sess, st, GFP_KERNEL);
    tbt_sock_detach(st);
    sk = READ_ONCE(st->sk);
    if (!sk || !(sock = sk->sk_socket))
        return;
    tbt_sock_abort_on_close(sock);
    if (READ_ONCE(st->sess->target) == sock)
        WRITE_ONCE(st->sess->target, NULL);
    WRITE_ONCE(st->sk, NULL);
    sock_release(sock);
}
```

**替换为**：

```c
static void tbt_kill_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_kill);
    struct tbt_session *sess = st->sess;

    might_sleep();
    tbt_tx_rst(sess, st, GFP_KERNEL);

    /* No self cancel: this is the w_kill work item. */
    tbt_stream_release_socket_now(st);

    if (!READ_ONCE(sess->closing))
    {
        mod_delayed_work(system_unbound_wq, &sess->w_reap,
                         msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
        if (sess->tunnel && sess->tunnel->cfg.role == TBT_ROLE_NEAR)
            queue_work(system_unbound_wq, &sess->w_pending);
    }

    tbt_stream_put(st); /* the reference taken in tbt_stream_kill() */
}
```

**要点**：

- 不再 `return` 早退；即使 `sk==NULL` 也继续 schedule reaper/pending、put 引用。
- `tbt_tx_rst` 仍保留当前“异常/正常都 RST”的语义；P4 再区分 graceful FIN。
- `tbt_stream_release_socket_now` 内部 `tbt_sock_detach` 会取消
  `w_read/w_drain/w_ctrl`；`w_kill` 不取消自己。
- `w_kill` 的引用在最后 put；哈希引用由 reaper 释放；两者独立。

### 2.8 远端 target 重启

P1 仍是一个 `sess->target`，但旧流关闭后，下一条流需要新的 target。当前
`tbt_carrier_open_target()` 只在 carrier join 时调用，顺序复用后不会再有 join，
所以必须在“新流 OPEN 到达”和“旧 target 释放”两处 kick。

#### 2.8.1 `tbt_carrier_open_target()` 加保护

**文件/函数**：`tbt_carrier.c`，`tbt_carrier_open_target()`（当前 `:1446`）。

> **Rebase 结论：本节可跳过。** `tbt_stream_release_socket_now()` 只要在清
> `sess->target` 之后把 `target_started` 置 false，现有的
> `if (!sess->target_started)` 就足以防止双 target；这样 P1-core 完全不碰
> `tbt_carrier.c`，避开 S1。下面的 `!sess->target` 检查只是 defense-in-depth，
> 若保留则等 S1 冻结后再加。

**当前片段**：

```c
    if (!sess)
        return -ENOTCONN;
    spin_lock(&sess->lock);
    if (!sess->target_started)
    {
        sess->target_started = true;
        start = true;
    }
    spin_unlock(&sess->lock);
```

**替换为**：

```c
    if (!sess || READ_ONCE(sess->closing))
        return -ENOTCONN;
    spin_lock(&sess->lock);
    /* Do not start a second target while the old stream's target socket is
     * still published; its release will kick us again. */
    if (!sess->target_started && !READ_ONCE(sess->target))
    {
        sess->target_started = true;
        start = true;
    }
    spin_unlock(&sess->lock);
```

#### 2.8.2 `tbt_far_open_seen()`：不要绑旧 target

**文件/函数**：`tbt_sock.c`，`tbt_far_open_seen()`（当前 `:515`）。

**当前结尾**：

```c
    tbt_stream_put(st);
    if (READ_ONCE(sess->target) && READ_ONCE(sess->target)->sk)
        tbt_target_attach(sess, READ_ONCE(sess->target));
```

**替换为**：

```c
    {
        struct sock *cur_sk = READ_ONCE(st->sk);
        struct socket *tgt;
        struct sock *tgt_sk;
        void *owner;
        bool ours;

        /* Classify under sess->lock.  release_socket_now() unpublishes
         * sess->target under the same lock *before* tbt_sock_detach(), so a
         * reader cannot see an old target whose sk_user_data is already NULL
         * and mistake it for an orphan. */
        spin_lock(&sess->lock);
        tgt = sess->target;
        tgt_sk = tgt ? tgt->sk : NULL;
        owner = tgt_sk ? READ_ONCE(tgt_sk->sk_user_data) : NULL;
        ours = (owner == st);
        spin_unlock(&sess->lock);

        tbt_stream_put(st);

        /* Attach only if this target is already ours, or it was dialled
         * before any stream existed (orphan, sk_user_data == NULL).  A target
         * still owned by the previous stream must be released by that
         * stream's w_kill; attaching it here would splice two public
         * connections onto one target. */
        if (tgt && tgt_sk && (tgt_sk == cur_sk || ours || owner == NULL))
            tbt_target_attach(sess, tgt);
        else
            tbt_carrier_open_target(sess->tunnel);
    }
```

**行为**：

- 新流的 `st->sk` 为 NULL；旧 target 的 `sk_user_data` 指向旧流 → 不 attach；
  调用 `tbt_carrier_open_target`；它看到 `sess->target` 非 NULL，不启动，等旧
  `w_kill` 的 release 再 kick。
- target 在第一条流之前就拨好（`sk_user_data == NULL`）时，作为 orphan 绑给
  新流；否则 P1 的第一个 target 永远绑不上。
- 如果 `sess->target` 为空，`open_target` 启动新 target。
- 旧 `w_kill` 的 `tbt_stream_release_socket_now` 清空 `sess->target` 后调用
  `tbt_carrier_open_target`；此时新流的 `far_sid` 已设置，启动新 target。

#### 2.8.3 `tbt_target_work_fn()` 失败/取消

- 失败路径已 `sess->target_started = false`；保留。
- `tbt_target_attach` 返回 `-ECANCELED` 时，当前代码释放 socket 并 `return`；
  它没有把 `target_started=false`，下一轮可能不再重试。P1 在 `-ECANCELED`
  分支加 `WRITE_ONCE(sess->target_started, false);`，让新流能重新 kick。
- 不在 P1 改成每流 target；P3 再做。

### 2.9 迟到帧与 sid 采纳

#### 2.9.1 `tbt_rx_bind_stream()`：near 永不采纳，far 先查墓碑

**文件/函数**：`tbt_rx.c`，`tbt_rx_bind_stream()`（当前 `:108`）。

**当前片段**：

```c
    spin_lock(&sess->lock);
    if (!sess->far_sid)
    {
        sess->far_sid = h->sid_lo;
        adopt = true;
    }
    else if (sess->far_sid != h->sid_lo)
    {
        bad = true;
    }
    spin_unlock(&sess->lock);
    if (bad)
        return -EPROTO;
    if (adopt && sess->tunnel->cfg.role == TBT_ROLE_FAR)
        tbt_far_open_seen(sess);
    return 0;
```

> **Rebase 落点（先读）**：下面的逻辑**不要**展开在 `tbt_rx.c` 里。
> 它必须作为 `tbt_session_bind_sid(sess, sid)` 放在 `tbt_session.c`，
> 这样 `tbt_stream_find_locked()` / `tbt_stream_is_closing()` 可以保持
> `static`，不需要跨文件暴露“调用者必须持 `sess->lock`”的函数。
> 片段里的 `h->sid_lo` 在搬过去后换成参数 `sid`，返回值改成
> `TBT_BIND_OK/ADOPT/DROP/BAD`。

**逻辑参考（放进 `tbt_session_bind_sid()`）**：

```c
    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing))
    {
        drop = true;
    }
    else if (sess->tunnel->cfg.role == TBT_ROLE_NEAR)
    {
        /* Only the near side allocates sids.  It never adopts from RX. */
        if (!sess->far_sid)
            drop = true;
        else if (sess->far_sid != h->sid_lo)
        {
            struct tbt_stream *old =
                tbt_stream_find_locked(sess, h->sid_lo);

            if (old && READ_ONCE(old->state) == TBT_ST_DEAD)
                drop = true;
            else
                bad = true;
        }
    }
    else
    {
        struct tbt_stream *cur = sess->far_sid ?
            tbt_stream_find_locked(sess, sess->far_sid) : NULL;
        struct tbt_stream *new =
            tbt_stream_find_locked(sess, h->sid_lo);

        if (new && READ_ONCE(new->state) == TBT_ST_DEAD)
            drop = true; /* late frame for a tombstone */
        else if (!sess->far_sid)
        {
            sess->far_sid = h->sid_lo;
            adopt = true;
        }
        else if (sess->far_sid != h->sid_lo)
        {
            if (cur && tbt_stream_is_closing(cur))
            {
                sess->far_sid = h->sid_lo;
                adopt = true;
            }
            else
                bad = true;
        }
    }
    spin_unlock(&sess->lock);

    if (drop)
        return 0;
    if (bad)
        return -EPROTO;
    if (adopt && sess->tunnel->cfg.role == TBT_ROLE_FAR)
        tbt_far_open_seen(sess);
    return 0;
```

**新增 helper**（`tbt_session.c`，`static`，调用者持 `sess->lock`）：

```c
static struct tbt_stream *tbt_stream_find_locked(struct tbt_session *sess,
                                                 u32 sid)
{
    struct tbt_stream *st;

    hlist_for_each_entry(st, &sess->streams[TBT_STREAM_BUCKET(sid)], node)
        if (st->sid == sid)
            return st; /* hash reference keeps it alive; no extra ref */
    return NULL;
}

/* The old stream has started its close handshake.  P1 may hand the session
 * to a new sid once the old stream can no longer carry application data:
 * either it is already DEAD, or it has seen the peer's FIN, or its socket is
 * in a closing TCP state.  The near side only starts the next stream after
 * its own TCP_CLOSE, so this is a safety net for the far side's lag. */
static bool tbt_stream_is_closing(const struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        return true;
    if (READ_ONCE(st->rx_fin_seen))
        return true;
    if (sk)
    {
        u8 s = READ_ONCE(sk->sk_state);

        if (s == TCP_CLOSE_WAIT || s == TCP_LAST_ACK ||
            s == TCP_CLOSING || s == TCP_CLOSE)
            return true;
    }
    return false;
}
```

**要点**：

- near 侧 `far_sid==0` 时，任何 RX 帧都 drop，不再“采纳旧 sid”。这是 P1
  清 `far_sid` 后必须的，否则一条迟到旧帧会占住门。
- far 侧只有在“旧当前流已 DEAD 或已进入关闭半程”时才允许新 sid；新 sid
  若是墓碑，drop。
- `tbt_stream_find_locked` 不增引用；调用者持 `sess->lock`，哈希引用保证
  对象不会被 free；解锁后只用 `adopt/bad/drop` 布尔，不保留指针。
- `drop` 返回 0（不毒化载体）；`bad` 返回 `-EPROTO`。

**实际落点（`tbt_rx.c` 只留薄封装）**：

`tbtun.h` 增加（不是字段，是 enum + 原型）：

```c
enum tbt_bind_result {
    TBT_BIND_OK = 0,
    TBT_BIND_ADOPT,
    TBT_BIND_DROP,
    TBT_BIND_BAD,
};

int tbt_session_bind_sid(struct tbt_session *sess, u32 sid);
```

`tbt_session.c`：

```c
int tbt_session_bind_sid(struct tbt_session *sess, u32 sid)
{
    bool adopt = false, drop = false, bad = false;

    /* ... 2.9.1 的逻辑，h->sid_lo 换成 sid ... */

    if (drop)
        return TBT_BIND_DROP;
    if (bad)
        return TBT_BIND_BAD;
    if (adopt && sess->tunnel->cfg.role == TBT_ROLE_FAR)
        tbt_far_open_seen(sess);
    return TBT_BIND_OK;
}
```

`tbt_rx.c`（整个 `tbt_rx_bind_stream()` 只剩这些）：

```c
static int tbt_rx_bind_stream(struct tbt_carrier *c, const struct tbt_hdr *h)
{
    int r = tbt_session_bind_sid(c->sess, h->sid_lo);

    return r == TBT_BIND_BAD ? -EPROTO : 0;
}
```

这样 `tbt_rx.c` 的 diff 只有一个函数体；采纳/墓碑/role 判断都在
`tbt_session.c`，`tbt_stream_find_locked()` 可以保持 static。

#### 2.9.2 `tbt_rx_data()`：DEAD 流直接丢

**文件/函数**：`tbt_rx.c`，`tbt_rx_data()`（当前 `:131`）。

**当前开头**：

```c
    st = tbt_stream_get(sess, hdr->sid_lo, false, false);
    if (!st)
        return -EPROTO;
    c->rx_frames++;
```

**替换为**：

```c
    st = tbt_stream_get(sess, hdr->sid_lo, false, false);
    if (!st)
        return -EPROTO;
    if (READ_ONCE(st->state) == TBT_ST_DEAD)
    {
        /* Tombstone: drop late DATA without growing its rx_q. */
        tbt_stream_put(st);
        return 0;
    }
    c->rx_frames++;
```

**要点**：

- 放在 `c->rx_frames++/rx_bytes` 之前，避免墓碑把丢包统计污染。
- `tbt_rx_frame` 的 FIN/RST 对墓碑流：`tbt_stream_kill_by_sid` 会找到 DEAD 流，
  `tbt_stream_kill` 的 DYING 位直接返回；FIN 只设置 `rx_fin_seen`，无副作用。
  可不改；若要更干净，在 FIN/RST 分支前加同样的 DEAD drop。

### 2.10 pending 队列：`pub.churn` 0 refused 的确定性机制

#### 2.10.1 为什么需要它

P1 仍是“一条活跃 stream”。`tbt_stream_kill` 清 `far_sid` 后，下一次 accept 才能
建立新流。但 `pub.churn` 是“客户端 close 后立刻 connect”：

- 客户端 close 只发出 FIN；near 侧的 accepted socket 还要经过
  `CLOSE_WAIT → w_ctrl 发 FIN → far target 关闭 → far FIN 回来 → TCP_CLOSE`
  才进入 `tbt_stream_kill`。这中间有几毫秒到几十毫秒。
- 下一次 `connect` 的 accept 可能落在这个窗口里；如果 `tbt_stream_start` 直接
  返回 `-EBUSY`，`tbt_accept_one` 会 `sock_release`，客户端立即 EOF，
  `pub.churn` 记为 `refused`。10k 次里只要撞上一次就失败。

**结论**：只清门/墓碑不够，必须把“旧流未完全让出”期间的新 accept **排队**，
而不是拒绝。这就是 pending 队列。它不引入第二条活跃流，符合 P1 的单流约束。

#### 2.10.2 结构

```c
struct tbt_pending
{
    struct list_head node;
    struct socket *sock;
};
```

- 私有于 `tbt_session.c`；`tbtun.h` 只需要 `sess->pending`/`n_pending`/`w_pending`。
- 上限 `TBT_PENDING_MAX 128`（等于 listener backlog）。
- 每个 pending socket 继承 listener 的 `tbt_ready()` 和 `sk_user_data=t`；
  在启动前，它的数据只留在自己的 receive queue，不会被 capture。启动时
  `tbt_sock_attach` 会替换回调并 `schedule_work(w_read)`，把已排队数据读走。

#### 2.10.3 `tbt_stream_start()`：总是入队

**文件/函数**：`tbt_sock.c`，`tbt_stream_start()`（当前 `:468`）。

**当前**：

```c
int tbt_stream_start(struct tbt_session *sess, struct socket *sock)
{
    ...
    spin_lock(&sess->lock);
    busy = sess->far_sid != 0;
    if (!busy) { sid = ...; sess->far_sid = sid; }
    spin_unlock(&sess->lock);
    if (busy)
        return -EBUSY;
    ...
}
```

**改为**：拆成 `tbt_stream_start_now()`（现有函数体，假定 `far_sid==0`）和
`tbt_stream_start()`（入队）：

```c
int tbt_stream_start(struct tbt_session *sess, struct socket *sock)
{
    struct tbt_pending *p;

    p = kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p)
        return -ENOMEM;
    p->sock = sock;

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing))
    {
        spin_unlock(&sess->lock);
        kfree(p);
        return -ENOTCONN;
    }
    if (sess->n_pending >= TBT_PENDING_MAX)
    {
        spin_unlock(&sess->lock);
        kfree(p);
        return -EBUSY;
    }
    list_add_tail(&p->node, &sess->pending);
    sess->n_pending++;
    spin_unlock(&sess->lock);

    queue_work(system_unbound_wq, &sess->w_pending);
    return 0; /* ownership transferred; accept loop must not release */
}
```

**`tbt_stream_start_now()`**：现有函数体，去掉 `busy` 分支，保留：

```c
static int tbt_stream_start_now(struct tbt_session *sess, struct socket *sock)
{
    ...
    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing) || READ_ONCE(sess->far_sid))
    {
        spin_unlock(&sess->lock);
        return -EBUSY;
    }
    sid = ((sess->sid_prefix & 0xffff) << 16) | (sess->next_sid & 0xffff);
    sess->next_sid++;
    /* sid 0 is the "no stream" sentinel in sess->far_sid.  A random prefix
     * of 0 plus counter 0 would collide with it; skip. */
    if (!sid)
        sid = 1;
    sess->far_sid = sid;
    spin_unlock(&sess->lock);

    st = tbt_stream_get(sess, sid, true, true);
    ...
}
```

**边界**：`tbt_stream_get()` 若返回的是同 sid 的墓碑（只有 16 位 counter
回绕才会发生；10k churn < 65k，P1 不会），必须 `tbt_stream_put` 并清
`far_sid`，不能把墓碑当新流用。P2 的 40 位 sid 从根本上消除这个回绕。

#### 2.10.4 `w_pending`：串行启动一条

**文件**：`tbt_session.c`，新增；`system_unbound_wq`。

```c
static void tbt_pending_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(w, struct tbt_session, w_pending);

    for (;;)
    {
        struct tbt_pending *p;
        struct socket *sock;
        int err;

        spin_lock(&sess->lock);
        if (READ_ONCE(sess->closing) || READ_ONCE(sess->far_sid) ||
            list_empty(&sess->pending))
        {
            spin_unlock(&sess->lock);
            return;
        }
        p = list_first_entry(&sess->pending, struct tbt_pending, node);
        list_del(&p->node);
        sess->n_pending--;
        spin_unlock(&sess->lock);

        sock = p->sock;
        kfree(p);

        /* The client may have closed while it was queued. */
        if (!sock->sk || READ_ONCE(sock->sk->sk_state) == TCP_CLOSE)
        {
            if (sock->sk)
                tbt_sk_ops_restore(sock->sk, &sess->tunnel->def_ops);
            tbt_sock_abort_on_close(sock);
            sock_release(sock);
            continue;
        }

        err = tbt_stream_start_now(sess, sock);
        if (err)
        {
            if (sock->sk)
                tbt_sk_ops_restore(sock->sk, &sess->tunnel->def_ops);
            tbt_sock_abort_on_close(sock);
            sock_release(sock);
            continue;
        }
        return; /* one live stream only */
    }
}
```

**要点**：

- 只有 `w_pending` 会启动 pending；workqueue 保证同一个 work 不会并发执行。
- `tbt_stream_kill()` 清 `far_sid` 后，`w_kill` 末尾
  `queue_work(system_unbound_wq, &sess->w_pending)`；
  启动下一条。
- `tbt_stream_start()` 总是入队；第一条连接也走一次 work，延迟可忽略。
- 失败/客户端已关闭时，必须 `tbt_sk_ops_restore` 再 `sock_release`，否则
  pending socket 还带着 `tbt_ready()` 和 `sk_user_data=t`，rmmod 后会取指。
- `pub.two`：B 入队；`w_pending` 看到 `far_sid != 0` 返回；B 的读超时，A 正常。
  旧测试的 `ok_b = read_to(...) <= 0` 仍成立。P3 才把 B 变成真正的第二条流。

#### 2.10.5 初始化/销毁

`tbt_session_create()` 加：

```c
INIT_LIST_HEAD(&s->pending);
s->n_pending = 0;
INIT_WORK(&s->w_pending, tbt_pending_work);
INIT_DELAYED_WORK(&s->w_reap, tbt_reap_work);
```

`tbt_session_destroy()` 取消：

```c
cancel_work_sync(&s->w_pending);
cancel_delayed_work_sync(&s->w_reap);
```

并 drain pending（在 streams 释放之后、target 释放之前）：

```c
while (!list_empty(&s->pending))
{
    struct tbt_pending *p =
        list_first_entry(&s->pending, struct tbt_pending, node);

    list_del(&p->node);
    s->n_pending--;
    if (p->sock->sk)
        tbt_sk_ops_restore(p->sock->sk, &t->def_ops);
    tbt_sock_abort_on_close(p->sock);
    sock_release(p->sock);
    kfree(p);
}
```

### 2.11 `tbt_session_destroy()` 顺序修正

当前顺序（`tbt_session.c:726-804`）：

```
closing=true
cancel w_fbk/w_replace/w_target
tbt_carriers_stop()
tbt_tx_free_queue()          <-- 队列先清
for each stream: unlink -> cancel w_kill -> release -> put
if (s->target) release
free
```

**问题**：`w_kill` 可能在 `tbt_tx_free_queue()` 之后才运行，并在
`tbt_kill_work()` 里 `tbt_tx_rst()` 入队一个新 RST/FIN。这个帧不会再被释放，
且它的引用会拖住流对象；多流下窗口更大。当前 `tbt_stream_release_socket()`
自己会 `cancel_work_sync(&w_kill)`，但它在 unlink 循环里、已经晚于 free queue。

**修正顺序**：

```
1. closing=true; cancel w_fbk/w_replace/w_target/w_reap/w_pending
2. tbt_carriers_stop()                 // 不再有载体 RX/TX
3. first pass: 对每个 stream
      cancel_work_sync(&st->w_kill)
      tbt_sock_detach(st)              // 停 w_read/w_drain/w_ctrl
   （不 unlink、不 release；临时 ref 后 put，哈希 ref 仍在）
4. tbt_tx_free_queue()                 // 现在没有生产者能再入队
5. second pass: unlink -> tbt_stream_release_socket() -> tbt_stream_put()
6. drain pending sockets
7. release sess->target（若还有 target-without-stream）
8. free
```

**first pass 片段**（哈希稳定：carriers 已停、w_reap/w_pending 已 cancel、
`closing` 阻止新流）：

```c
for (i = 0; i < TBT_STREAM_HT; i++)
{
    struct hlist_node *pos;

    spin_lock(&s->lock);
    pos = s->streams[i].first;
    spin_unlock(&s->lock);

    while (pos)
    {
        struct tbt_stream *v = hlist_entry(pos, struct tbt_stream, node);
        struct hlist_node *next = READ_ONCE(pos->next);

        if (refcount_inc_not_zero(&v->refs))
        {
            cancel_work_sync(&v->w_kill);
            tbt_sock_detach(v);
            tbt_stream_put(v);
        }
        pos = next;
    }
}
```

**边界**：

- `hlist_del_rcu` 只在 second pass；first pass 不改变哈希链，`next` 稳定。
- `cancel_work_sync(&w_kill)` 可能运行 `w_kill`，它会 release socket、schedule
  reaper/pending；此时 `closing=true`，`w_kill` 不 schedule reaper/pending；
  它的 `tbt_tx_rst` 入队仍发生在 free queue 之前，安全。
- `tbt_sock_detach` 可能取消正在跑的 `w_ctrl`；`w_ctrl` 可能入队 FIN/RST；
  也在 free queue 之前，安全。
- second pass 的 `tbt_stream_release_socket()` 会再 cancel 一次 `w_kill`
  （已经结束，立即返回），然后 detach/release；幂等。
- `s->target` 块保留：处理“target 已拨但还没有 stream”的情况；如果 stream
  release 已清空 `sess->target`，块看到 NULL，跳过。
- `s->target` 块不要只靠 `tbt_target_detach(s, ts)`：它按 `far_sid` 找流，
  而 `far_sid` 此时可能已经是 0。直接对 `ts->sk` 调
  `tbt_sk_ops_restore(ts->sk, &s->def_ops)`，再
  `tbt_sock_abort_on_close(ts)` / `sock_release(ts)`；否则 target 上的
  `sk_user_data` 可能还指向已释放的 stream，rmmod 后取指。

### 2.12 P1 的验收

#### 必测

1. **`pub.churn` 10000**
   - 通过：`live=10000`、`refused=0`、`other=0`。
   - 附加：结束后 `far_sid=0`、哈希里无活跃流、墓碑在 2 s 后清零；
     `kmalloc-1k/2k` 回到基线；`ss -s` 的 TIME_WAIT 不持续增长；
     `dmesg` 无新增 warning/oops。
   - 若仍有 `refused>0`：先看是不是 pending 队列满（`n_pending=128`）或
     `w_pending` 延迟；不要回到 `-EBUSY`。

2. **`near.second_conn`**
   - 通过：rc=0；第二条能收到 fake far 的 DATA。

3. **探针 B 不回归**
   - `make qemu-test --probes B`（或当前等价命令）通过；吞吐/丢包/替换指标
     不低于 P1 前基线。

#### 回归

| 测试 | 关注点 |
|---|---|
| `pub.two`（旧契约） | B 超时/不崩，A 仍可用；P1 不改契约 |
| `pub.rst` | RST 流 kill/tombstone，不崩 |
| `init.slab_leak` | 100 次连接后对象回收（墓碑 grace 后） |
| `init.rmmod` | destroy first pass + pending drain，无 oops |
| `init.flush` | 单流 flush，不因 pending/reaper 死锁 |
| `far.data_path` / `near.data_path` | P0 + P1 后仍 PASS |
| `stall.hello_chain` | 载体 join 链不受影响 |
| `make check` / `host-test-san` / `review/lint.sh` | 机械不变量 |

### 2.13 P1 的风险与回退

| 风险 | 缓解 |
|---|---|
| 旧 ACK 污染新流 | `tbt_tx_ack` 的 sid 检查 + `tbt_rx_data` 的 sid 检查；P1 临时，P2 删除 |
| `w_kill` 自 cancel 死锁 | `_now` 变体；公共 release 只从 destroy/错误路径调用 |
| 墓碑期间迟到帧 | 2 s grace + far 侧 `tbt_stream_find_locked` 检查；不采纳 DEAD sid |
| target 串流 | 在 `sess->lock` 下按 `sk_user_data` 分类：ours/orphan 才 attach；旧 target 由旧 `w_kill` 先 unpublish 再 release，然后 kick |
| pending socket 带模块回调 | 失败/关闭/销毁路径一律 `tbt_sk_ops_restore` 再 release |
| destroy 队列泄漏 | first pass 停 `w_kill`/`w_ctrl` 后再 free queue |
| 10k churn 墓碑内存 | 2 s grace、约 2000 条；观测 slab；超限再加 `TBT_TOMBSTONE_MAX` |
| 10k churn 的 target 端口/TIME_WAIT | far 侧 `SO_LINGER=0` RST 关闭避免 far TIME_WAIT；target 侧可能积累，监控 `ss -s`/`sockstat`；必要时 `SO_REUSEADDR`，不要用“全部 RST”掩盖 |
| RST 关闭后立刻新连接 | 新 OPEN 可能先于旧 RST 到达 far，旧 `far_sid` 仍非 closing，far 会 EPROTO。P1 的 `pub.churn` 是 graceful FIN，`rx_fin_seen` 已覆盖；RST+立即重连留到 P3 的显式流代次/流关闭 ack。不要把 P1 的 10k churn 当成 RST 压力测试。 |

**回退**：P1 的改动集中在 `tbt_sock.c` / `tbt_session.c` / `tbt_rx.c` /
`tbt_tx.c` 的 ACK 与 purge；若 churn 仍失败，先回退 pending 队列（恢复 `-EBUSY`），
保留墓碑/清门，确认 `near.second_conn` 仍通过，再定位 `w_pending`/target。

---

## 3. 与 P2/P3 协议 v4 的独立性

| 改动 | 属于 | 是否依赖 v4 | 何时可做 |
|---|---|---|---|
| P0-A provisional 跳过 pause 扫描 | P0 | 否 | S1 合入后立即 |
| P0-B provisional poll 补救 | P0 | 否 | 同上 |
| P0-C rx work 自调度 | P0 | 否 | 同上 |
| P1 终态单赢家 + w_kill 引用 | P1 | 否 | S1 合入后 |
| P1 清 `far_sid` + 会话 ACK 复位 | P1 | 否（同 wire） | S1 合入后；P2 会删除复位 |
| P1 墓碑/reaper | P1 | 否 | S1 合入后 |
| P1 socket release `_now` | P1 | 否 | S1 合入后 |
| P1 target 重启 | P1 | 否 | S1 合入后；只改 `tbt_sock.c`，不碰 `tbt_carrier.c` |
| P1 迟到帧/sid 检查 | P1 | 否（同 wire） | S1 合入后 |
| P1 pending 队列 | P1 | 否 | S1 合入后 |
| P1 destroy 顺序修正 | P1 | 否 | S1 合入后 |
| `tbt_tx_ack` sid 参数 | P1 | 否（同 wire） | 与 S1 冲突，S1 后 |
| `tbt_tx_purge_stream` | P1 | 否 | 放 `tbt_session.c` static，不碰 `tbt_tx.c` |
| P1-S（只改 session/sock） | P1 实验 | 否 | 可在 S1 前做；**不可作为合并版本**（缺 tx/rx 的 sid/墓碑检查） |
| SACK / 40 位 sid / 每流 ACK | P2 | **是** | 等 conv 0x0004 |
| OPEN 单向/announced_mask | P3 | **是** | 等 P2 |
| 每流 target | P3 | 是（流身份/生命周期） | 等 P2 |
| 每流暂停（删除全局扫描） | P3 | 是（每流 ack/队列） | 等 P2 |

**结论**：P0/P1 都可以在 perf 验证完 S1 后立即实施；它们不改 wire、不改
`TBT_CONV`、不需要 SACK/40 位 sid。P1 的会话 ACK 复位是临时补丁，P2 用
`st->tx_ack` 取代后删除。

---

## 4. 需要 perf 协调的 tbtun.h 字段（最小集）

| 字段 | 必需性 | 说明 |
|---|---|---|
| `tbt_stream.tombstone_until_ns` | **必需** | 墓碑期限；否则只能永久保留死流或误建流 |
| `tbt_session.w_reap` | **必需** | 会话级 200 ms 墓碑扫描 |
| `tbt_session.pending` / `n_pending` | **必需（churn 0 refused）** | accept 排队，避免 `-EBUSY` |
| `tbt_session.w_pending` | **必需（churn 0 refused）** | 串行启动 pending，进程上下文可睡眠 |
| `tbt_session.rx_self_reschedules` | 可选 | P0 观测；不加用 `poll_rescues`/debug |
| `tbt_session.tombstone_forced` | 可选 | 墓碑上限诊断 |
| `attached` 的 bit 1/2 | 不需要新字段 | `DYING`/`UNLINKED` 复用 `unsigned long` |

如果 perf 只能接受一个字段：优先 `tombstone_until_ns`；`w_reap` 可以用
`w_fbk` 的 10 ms tick 兼职扫描（但 `w_fbk` 在没有活跃流时会停，需要额外
“有墓碑就继续”的条件）。**不推荐**零字段方案：10k churn 会泄漏或误建流。

---

## 5. 当前行号索引（S1 后按函数名重锚）

| 主题 | 位置 |
|---|---|
| `tbt_car_poll_work` | `tbt_carrier.c:190` |
| provisional 分支 | `tbt_carrier.c:208-217` |
| 全局 pause 扫描 | `tbt_carrier.c:419-436` |
| `tbt_car_rx_work` | `tbt_carrier.c:403` |
| `tbt_carrier_adopt` 调度 w_rx | `tbt_carrier.c:1394` |
| `tbt_carrier_open_target` | `tbt_carrier.c:1446` |
| `tbt_target_work_fn` | `tbt_carrier.c:1401` |
| `tbt_stream_kill` | `tbt_session.c:643` |
| `tbt_stream_get` / `put` / free | `tbt_session.c:552-671` |
| `tbt_session_destroy` | `tbt_session.c:726` |
| `tbt_tx_free_queue` | `tbt_session.c:750`（调用）/ `tbt_tx.c:2027` |
| `tbt_stream_release_socket` | `tbt_sock.c:438` |
| `tbt_kill_work` | `tbt_sock.c:321` |
| `tbt_stream_start` | `tbt_sock.c:468` |
| `tbt_far_open_seen` | `tbt_sock.c:515` |
| `tbt_target_attach/detach` | `tbt_sock.c:533/560` |
| `tbt_rx_bind_stream` | `tbt_rx.c:108` |
| `tbt_rx_data` | `tbt_rx.c:131` |
| `tbt_rx_frame` 的 uack 调用 | `tbt_rx.c:316` |
| `tbt_tx_ack` | `tbt_tx.c:1203` |
| `tbt_stream_start` 调用方 | `tbt_listen.c:73` |
| `tbt_sk_ops_save/restore` | `tbt_sock.c:397-427` |
| `tbt_sock_abort_on_close` | `tbt_sock.c:429-436` |
| `tbt_sock_detach` | `tbt_sock.c:377-396` |
| `tbt_sock_attach` | `tbt_sock.c:344-375` |

---

## 6. 实施记录（P0/P1 已落地）

### 6.1 改动文件

- `tbtun/tbt_carrier.c`：P0-A provisional 跳过全局 pause 扫描；P0-B provisional
  poll 补救（`copied_seq != rcv_nxt || parser->have`）；P0-C rx work 结束自调度。
- `tbtun/tbtun.h`：`tombstone_until_ns`、`w_reap`、`w_pending`（delayed）、
  `pending`、`n_pending`、`TBT_STREAM_F_*`、`tbt_session_bind_sid` 原型。
- `tbtun/tbt_session.c`：单赢家 kill + w_kill 引用；`tbt_stream_detach_from_session`
  清 `far_sid`/ACK/墓碑；`tbt_tx_purge_stream`（static，走 `tx_q`）；会话级
  `w_reap`；`tbt_session_bind_sid`；destroy first pass 停 w_kill/w_ctrl 再 free queue；
  pending drain；target 回调直接 `tbt_sk_ops_restore`。
- `tbtun/tbt_sock.c`：`tbt_stream_release_socket_now`（不自 cancel）；
  `tbt_kill_work` 用它并 put w_kill 引用；pending 队列 + `w_pending`；
  `tbt_stream_start` 总是入队；near CLOSE_WAIT 时 `tbt_tx_fin` 后 retire；
  `tbt_far_open_seen` 只 attach ours/orphan target。
- `tbtun/tbt_tx.c`：`tbt_tx_ack` 加 sid 参数和 `far_sid` 校验（保留 `fb_sample`）；
  FIN/RST 写出后 kick pending（1 ms）。
- `tbtun/tbt_rx.c`：`tbt_rx_bind_stream` 改为 `tbt_session_bind_sid` 薄封装；
  `tbt_rx_data` 丢弃 DEAD/tombstone DATA；`rx_ack` 更新按 sid 串行化。
- `adversarial/adv_probe.c`：`near.second_conn` 的 fake far 现在回显第二条流的
  DATA。原测试从不回显，rc=0 在旧代码上也不可能；这是测试缺陷修正，不是放宽
  模块验收。
- `review/lint.sh`：`sk-cleared-before-release` 检查实际 release 函数
  `tbt_stream_release_socket_now`，语义不变。

### 6.2 实测结果（QEMU，KVM）

| 项 | 结果 |
|---|---|
| `pub.churn` 10000 | **PASS** `live=10000 refused=0 other=0`，约 66 s（public 组） |
| `near.second_conn` | **PASS** `rc=0 echoed=1` |
| `pub.two` 旧契约 | **PASS** `A usable=1, B refused=1, A still usable=1` |
| 探针 B | **PASS** |
| `pub.rst` | **PASS** |
| `init.slab_leak` / `init.slab_after_flush` | **PASS** |
| `init.rmmod` / `init.reload` / `init.dmesg_after_rmmod` | rmmod **FAIL**（`ECHO_ABORT`，不再 hang）；reload/dmesg **PASS**。基线 `console.hang.rmmod` 为 hang，属既有缺陷，不是本波回归。 |
| `init.flush_midstream` | **FAIL**（`ECHO_ABORT`）；基线为 `ECHO_TIMEOUT`，属既有缺陷。 |
| `pub.halfclose` | **FAIL**（F5 既有缺陷；本波未修，且 near CLOSE_WAIT retire 也不改善） |
| `init.src_key` / `pub.srcfilter` | **FAIL**（既有 `src=` 未实现） |
| `make check` | 314 checks / 0 failures |
| `review/lint.sh` | 15/15 checks, 12/12 self-tests |

### 6.3 本波的有意取舍

- 仍只允许一条**活跃** stream；pending 队列只排队，不并发。
- near 侧在 `TCP_CLOSE_WAIT`（应用半关闭/关闭）就 retire 出会话，以便下一条
  连接能建立；旧 stream 继续 drain，直到 `TCP_CLOSE`/RST 才 kill。这会让
  `pub.halfclose` 仍失败，F5 留给 P4。
- target 在旧 stream 未完全释放时允许“重叠一条”：`target_started` 置 false，
  新 stream 拨新 target；旧 target 由旧 `w_kill` 释放，且不会清掉新 target。
  这是 P1 的临时方案，P3 改成每流 target。
- FIN/RST 写出后延迟 1 ms 才启动 pending，避免新 OPEN 抢在旧 FIN 前到达 far。
  10k churn 约 66 s，低于 150 s 超时。
- `w_reap`/`w_pending` 实际用 `system_dfl_wq`。计划里的
  `system_unbound_wq` 在 7.2 上触发 `deprecated workqueue` 警告（F7），
  首轮 near 组因此 `console.klog.near FAIL`；改用 `system_dfl_wq` 后消失。
- 会话 ACK 复位仍是 P1 临时补丁；P2 的每流 ACK 会删除它。
