# 探针 J：far 侧 parser 停在 98,304 的根因分析

分析对象：工作树 2026-11-05 10:30 快照
（`tbtun/tbt_rx.c` 09:48、`tbtun/tbt_carrier.c` 10:30、`tbtun/tbt_tx.c` 10:28、`tbtun/tbt_sock.c` 08:57）。
只读分析：未改源码、未构建、未跑 QEMU。

故障数据（2026-11-05，J，debug=0，1% 丢包 + 2% 重排，逐字节严格用例）：

- far：`fbk_rx=0`、`tx_ack=0`、`rx_ack=98,304`、`ooo_frames=3`、`holes_declared=0`、
  `hole_checks=0`、`holes_unrecoverable=1`
- near：`fbk_tx=53`、`fbk_rx=52`、`tx_ack=98,304`、`retx_frames=155`、
  `hole_checks=52`、`gate_fail=23`、`depth_fail=28`
- 内核日志：`TXSTALLKILL ack=0 txseq=98304 age=5.1s`

---

## 0. 结论摘要

1. **far 的 parser 停在一条“已经离开 socket、但仍留在 `c->parser->buf`”的记录上。**
   `tbt_rx_skb()` 返回 `-EAGAIN` 时，`*consumed` 已经把这条记录的字节从 TCP 接收队列取走，
   于是 `tp->copied_seq == tp->rcv_nxt`（`tbt_rx.c:762-825`、`tbt_carrier.c:347-394`）。
   此后：`sk_data_ready` 不会再来（没有新字节）；`tbt_car_poll_work()` 的
   `copied_seq != rcv_nxt` 检查不会触发（`tbt_carrier.c:309-313`）；
   `tbt_carrier_resume()` 只在 `st->paused` 清除时调用（`tbt_sock.c:239-243`、`:263-267`）。
   **所有唤醒源同时消失。**
2. 记录本身、以及同一载体字节流里排在它后面的每一条 FBK，都永远不再被解析。
   不同载体的 parser 各自独立（`struct tbt_carrier.parser`，`tbtun.h:544`），
   但如果停的是**流级**拒绝（交付环满、等待 OPEN），四条载体可以在同一批里全停，
   于是 far 出现全局 `fbk_rx=0`。
3. 当前工作树已经在 `tbt_carrier.c:314-325` 加了 `c->parser->have` 的 poll 重试，
   但它只 `schedule_work(&c->w_rx)`。`w_rx` 唯一的解析入口是 `tcp_read_sock()` 的 actor
   （`tbt_carrier.c:434`）；没有 socket 字节时 actor 根本不会被调用，
   **这次重试不会让 parser 再跑一遍**。这是当前修复不完整的地方。
4. 最小修复：让 `tbt_car_rx_work()` 在入口（pause 检查之后、`tcp_read_sock()` 之前）
   直接调用一次 `tbt_rx_parse()`（导出为 `tbt_rx_parse_pending()`）。
   这样 poll 的 `schedule_work` 才有意义，并且对所有 `-EAGAIN` 生产者
   （交付环、等待 OPEN、parser 缓冲、http pending）都成立。
5. 发送侧的 `gate_fail/depth_fail` 是第二个独立问题：窗口被钉在 98,304 后，
   “其它每条活路径的 `uack >= hole->seq`”这个门永远不满足（`tbt_tx.c:1008-1013`）。
   J 的修复只能靠 RTO 定时器（`tbt_tx.c:466-646`），而它的副本又被同一个 parser 停住所挡。
   parser 唤醒修好后，RTO 副本（约 200 ms）能在 far 的 500 ms 预算内补上。

---

## 1. 失败时间线

| 时刻 | 事件 | 代码/证据 |
|---|---|---|
| t≈0 | near 捕获应用字节，按 8192 B 分帧，`seq` 从 0 递增。 | `tbt_tx.c:1763-1870`（`tbt_tx_submit` 的 chunk 与 seq 分配） |
| t≈20–50 ms | far 连续收到 12 帧 = 98,304 B，`rx_ack=98304`。 | far `rx_ack` 由 `tbt_rx_data` 写入：`tbt_rx.c:213-218` |
| t≈20–50 ms | 某条载体上的下一条记录在 `tbt_rx_skb()` 里返回 `-EAGAIN`；该记录的字节已全部取进 `p->buf`，`copied_seq == rcv_nxt`。 | `tbt_rx.c:817-822`；`*consumed = done` |
| t≈20–50 ms | `tbt_car_rx_work()` 正常返回。没有新字节，`sk_data_ready` 不再触发；`w_poll` 的 `copied_seq != rcv_nxt` 为假。 | `tbt_carrier.c:396-446`、`:309-313` |
| t≈50 ms 起 | far 的 `rx_next` 冻结在 98,304；3 条更晚的帧进了重排树（其它载体），`ooo_frames=3`。 | `tbt_rx.c:176-210` |
| t≈50 ms 起 | far 的交付在洞处停住，target（`nc -e /bin/cat`）只收到 98,304 B，只回显 98,304 B；far 的 `tx_seq=98304`。 | `tests/qemu/init:895-900`；far `txseq=98304` |
| t≈10 ms 起 | near 每 10 ms 生成一条 FBK，`fbk_tx` 在**入队**时自增。 | `tbt_tx.c:1909-1970`、`:1967-1968` |
| t≈50–500 ms | near 收到 52 条 far 的 FBK；FBK 头的 `uack` 在 pump 写入时填成 far 的 `rx_ack`（98,304），near 的 `tx_ack` 因此到 98,304。far 的 `tx_ack=0` 说明 near 的 FBK 一条也没到达 far。 | `tbt_tx.c:1575`、`:1192-1210`；far `fbk_rx=0` |
| 同期 | near 的 `tbt_tx_hole_check()` 随每条 FBK 跑一次：`hole_checks=52`；门控/深度拒绝 51 次：`gate_fail=23`、`depth_fail=28`；`holes_declared=0`。 | `tbt_tx.c:923-1130` |
| 同期 | RTO 定时器仍在重传：`retx_frames=155`。副本被写到其它载体，但 far 的 parser 不再读，副本停在载体 TCP 缓冲/解析缓冲里。 | `tbt_tx.c:466-646`；`tbt_carrier.c:517-712` |
| t≈5.1 s | far 的 `tbt_tx_stall_check()` 以 `ack=0 txseq=98304` 杀流，`holes_unrecoverable++`。 | `tbt_tx.c:1132-1186`（日志在 `:1180`） |
| 采样时 | far `fbk_rx=0`、`tx_ack=0`；near `fbk_tx=53`、`fbk_rx=52`、`tx_ack=98304`。 | 工单数据 |

关于 `holes_unrecoverable=1` 的归属：接收侧 `tbt_rx_check_hole()`（`tbt_rx.c:842-897`）
和发送侧 `tbt_tx_stall_check()`（`tbt_tx.c:1132-1186`）**共用同一个计数器**。
日志 `ack=0 txseq=98304` 只能属于 far 的发送侧停顿：far 的 `tx_ack=0`、far 的回显
`tx_seq=98304`；near 的 `tx_ack=98304` 排除了 near。
若 far 的接收洞计时器先到 500 ms，`tbt_rx_check_hole()` 会先杀流，统计里的
`rx_max_stall_us≈500000` 是判据；若它是 0，则先到的是 5.1 s 的发送侧停顿。

---

## 2. 所有能让 `tbt_car_rx_work` 停止、并依赖外部重新调度的路径

| # | 位置 | 条件 | 唤醒源 | 会不会永久丢 |
|---|---|---|---|---|
| 1 | `tbt_carrier.c:64-76` `tbt_car_ready` | 载体收到新字节（`sk_data_ready`） | 只 `schedule_work(&c->w_rx)` | 不丢；这是正常入口 |
| 2 | `tbt_carrier.c:406-411` | `stopping` 或 `sk == NULL` | 无 | 按设计永久；载体已被替换/释放 |
| 3 | `tbt_carrier.c:417-429` | 会话里任一 stream `paused && sk` | `tbt_drain_work` 把 `rx_queued` 降到 `TBT_RX_READY_LOW` 以下、清 `paused`、调 `tbt_carrier_resume`（`tbt_sock.c:239-243`、`:263-267`；`tbt_carrier.c:1243-1270`） | 只有 `paused` 真的被置位才不丢；若 `-EAGAIN` 没置 `paused`，这里没有唤醒 |
| 4 | `tbt_carrier.c:347-394` actor，`-EAGAIN` 分支 | `tbt_rx_skb()` 返回 `-EAGAIN`；actor 返回 `used` | 新字节 / 第 3 条的 resume / `w_poll`（仅当 `copied_seq != rcv_nxt`） | **会丢**：`used == len` 且 `copied_seq == rcv_nxt` 且未 paused 时，三个源都不成立 |
| 5 | `tbt_carrier.c:377-393` actor，其它错误 | `-EPROTO`/`-ENOMEM`/`-EFAULT`/`-EALREADY` | 无；`stopping=1` | 按设计永久；但 `-ENOMEM` 是瞬态的，不该走这里 |
| 6 | `tbt_carrier.c:434-446` | `tcp_read_sock()` 返回 0，socket 仍 `ESTABLISHED` | 新字节 / `w_poll` 的 `copied_seq != rcv_nxt` / resume | **会丢**：若字节已全部取进 `p->buf`（`copied_seq == rcv_nxt`），`w_poll` 的第一条不触发 |
| 7 | `tbt_rx.c:762-825` `tbt_rx_skb` | parser 持有未投递的记录（`p->have > 0`） | 只有 actor 再次被调用才会重试 | **会丢**：actor 只在 `tcp_read_sock` 有 skb 时被调用；没有新字节就不重试 |
| 8 | `tbt_rx.c:539-548`、`:652-658` `http_pending` | 解码出的记录等待处理（`-EAGAIN`） | 同 7 | **会丢**；J 是 raw，不走这里 |
| 9 | `tbt_sock.c:191-275` `tbt_drain_work` | 目标/public socket 发送缓冲满，`tbt_sock_deliver()` 返回 `-EAGAIN` | `tbt_pub_write_space` → `schedule_work(w_drain)`（`tbt_sock.c:176`） | 不丢；socket 自己的 `sk_write_space` 是源 |
| 10 | `tbt_sock.c:239-243`、`:263-267` | `paused` 且 `rx_queued < TBT_RX_READY_LOW` | 本次 drain 自己调 `tbt_carrier_resume` | 不丢 |
| 11 | `tbt_carrier.c:1243-1270` `tbt_carrier_resume` | 被 drain/替换路径调用 | 给所有载体 `schedule_work(w_rx)` | 不丢；但只在 `paused` 清除或替换时调用 |
| 12 | `tbt_carrier.c:309-326` `w_poll` rescue | 每 50 ms：`copied_seq != rcv_nxt` → 排 w_rx；否则 `c->parser->have` → 排 w_rx | 自己 | **当前不完整**：`parser->have` 只排 w_rx，而 w_rx 不会在没有 socket 字节时解析（见 7） |
| 13 | `tbt_carrier.c:1196`、`:1365` | 载体拨号/接受后首次排 w_rx | 一次 | 一次性 |
| 14 | `tbt_tx.c:1991-2013` `tbt_fbk_work` | 每 10 ms 重排自己 | 自己 | 驱动 `tbt_rx_check_hole` 和 `tbt_tx_stall_check`；不直接唤醒 w_rx |

**共同结构缺陷**：parser 的待处理字节只在 `tbt_rx_skb()` 内部被重试，
而 `tbt_rx_skb()` 只由 `tcp_read_sock()` 的 actor 调用。任何 `-EAGAIN`
只要把 socket 字节读空、又把记录留在 `p->buf`，就没有任何路径会再次进入 parser。

---

## 3. 哪个 `-EAGAIN` 分支在 J 的时序下永久失去唤醒

### 3.1 精确条件

同时满足以下五条，唤醒就永久丢失：

1. `tbt_rx_skb()` 在 `tbt_rx.c:817-822`（拷贝后解析返回 `-EAGAIN`）
   或 `tbt_rx.c:791-806`（缓冲满分支）返回 `-EAGAIN`；
2. 这次调用把 `tcp_read_sock()` 交给 actor 的最后一个字节也拷进了 `p->buf`
   （`done == len`），所以 `tp->copied_seq == tp->rcv_nxt`；
3. `c->parser->have > 0`，被拒绝的记录仍在缓冲里；
4. `st->paused == false`，drain 不会调 `tbt_carrier_resume`；
5. 该载体不再有新字节到达（对端窗口被这个洞本身钉住）。

此时：`sk_data_ready` 无事件；`w_poll` 的第一条（`copied_seq != rcv_nxt`）为假；
`w_poll` 的第二条（`parser->have`）只排 w_rx，而 w_rx 没有 socket 字节就不会调 actor；
`tbt_carrier_resume` 不被调用。**记录和它后面的所有 FBK 永久停住。**

### 3.2 候选生产者逐条核对

| 候选 | 代码 | 在 J 里成立吗 | 说明 |
|---|---|---|---|
| stream 不存在 | `tbt_rx.c:145-146` 返回 `-EPROTO`；actor `:386-393` 置 `stopping` | 不是 `-EAGAIN` | 若旧构建在这里返回 `-EAGAIN`，唤醒应来自 OPEN/attach；`tbt_far_open_seen`/`tbt_target_attach` 只排 `w_drain`（`tbt_sock.c:374`），不唤醒载体 → 同样丢 |
| rx_q 满 | 当前 `tbt_rx_data` 无条件入队（`tbt_rx.c:186-226`）；上限是 64 MiB 的 pause（`tbt_rx.c:899-911`、`tbtun.h:141-143`） | 当前代码不产生 `-EAGAIN` | 但 `tbt_rx.c:506` 的注释和 actor `:366-370` 的注释仍写着“delivery ring full / record stays buffered”。若故障构建仍有这个返回，它就是 J 的生产者：环满 → 记录留在 parser → 全载体停 |
| parser 缓冲不足 | `tbt_rx.c:791-806`，要求 `p->have == TBT_PARSER_BUF`（8224） | 间接 | raw 的合法记录总长 ≤ 8224（`tbt_uapi.h:90-92`、`tbt_proto.c:87`）。缓冲满时，第一条记录一定是完整的；它不被消费只能是因为**处理函数拒绝了它**。所以这是“症状”，不是独立生产者 |
| http 记录未完成 | `tbt_rx.c:539-548`、`:652-658` | 否 | J 的 `profile=raw`（`tests/qemu/init:898-901`） |
| `tbt_rx_data` 等待 OPEN | 当前 `tbt_rx_data` 没有 state 检查；`tbt_rx_bind_stream` 先建流（`:110-128`），再由 `tbt_far_open_seen` 置 OPEN（`tbt_sock.c:515-531`） | 当前代码不产生 `-EAGAIN` | 若故障构建等待 OPEN/target attach，唤醒应来自 `tbt_far_open_seen`/`tbt_target_attach`；它们只排 `w_drain`（`tbt_sock.c:374`），不唤醒载体 → 同样丢 |

### 3.3 从统计判定是哪一个

看故障运行的 `rx_errs` / `rx_last_err`（`tbt_procfs.c:712-724` 的 stats 行）：

- `rx_errs=0`：非致命 `-EAGAIN` 路径（环满/OPEN/缓冲）。本文 §6.1 的修复直接覆盖。
- `rx_last_err=-12`（`-ENOMEM`）：`tbt_rx_data` 的分配失败（`tbt_rx.c:162-166`）被 actor
  当成协议错误杀了载体（`tbt_carrier.c:386-393`）。此时 §6.1 的重试不会跑，
  还要把 `-ENOMEM` 改成可重试；注意计数器位置（§6.3）。
- `rx_last_err=-71`（`-EPROTO`）：记录头损坏或 stream 不存在，是另一个 bug；
  载体被毒化是当前设计，先查 `tbt_hdr_check`（`tbt_proto.c:84-118`）。

---

## 4. FBK 与 DATA 同一条载体字节流：parser 停住是否必然挡住 FBK

- **同一条载体：必然。** `tbt_rx_skb()` 是顺序字节流解析，不能跳过一条记录；
  `tbt_rx_parse_raw()` 在第一个 `-EAGAIN` 直接返回（`tbt_rx.c:738-740`）。
  排在该记录之后的所有记录（包括 FBK）都留在 `p->buf` 或 socket 里。
- **不同载体：不必然。** 每条载体有自己的 `parser`（`tbtun.h:544`，分配在
  `tbt_carrier.c:735`）。载体 0 的半条记录不挡载体 1。
- **全局 `fbk_rx=0` 因此只有三种解释**：
  1. 四条载体的 parser 都被**流级**拒绝停住（交付环/等待 OPEN）；
  2. 四条载体都被置了 `stopping`（致命错误）；
  3. near 根本没把 FBK 写上线。
- near 的 `fbk_tx=53` 是**入队计数**，不是上线计数：`tbt_tx_fbk()` 在
  `tbt_tx_enqueue()` 之后自增（`tbt_tx.c:1967-1968`）。FBK 的 `uack` 也不是在
  `tbt_tx_fbk()` 里填的，而是在 pump 写之前由 `fr->hdr.uack = sess->rx_ack`
  填（`tbt_tx.c:1575`）。
- FBK 的“豁免”只豁免了 `sk_stream_memory_free()` 预检查（`tbt_tx.c:1504-1513`）。
  真正的写仍可能返回 `-EAGAIN`：`c->part` 的续写（`tbt_carrier.c:650-668`）和
  `tcp_sendmsg_locked()`（`:674-676`）。pump 会把 FBK 重新入队（`tbt_tx.c:1594-1625`）。
  所以 FBK 可能排在满载体的 `tx_q` 里——但这是 far 不读、窗口关闭的**后果**，
  不是独立的永久阻塞。far 一旦恢复读，`tbt_car_write_space`（`tbt_carrier.c:85-125`）
  和 2 ms 的 `w_pump` 会让 FBK 出去。
- **结论**：修 parser 的唤醒是必要且充分的；不要在 `tcp_sendmsg_locked` 之前
  给 FBK 开更大的旁路，TCP 的发送缓冲就是真实门。

---

## 5. 为什么 debug=3 能过

打印没有修好任何东西，它只是把 §3.1 的竞态窗口撑开：

1. **EAGAIN 打印本身**：`tbt_rx_parse_raw()` 在 `debug >= 2` 时于返回前
   `pr_info`（`tbt_rx.c:734-737`）。它拉长了“记录留在 `p->buf`”到
   “下一次 `tbt_rx_skb()`”的间隔。`tbt_rx_data()` 每次入队后都会
   `tbt_rx_flush()` → `tbt_sock_kick()` 排 `w_drain`（`tbt_rx.c:226`、`:899-911`；
   `tbt_sock.c:173-177`），于是 drain 在这个窗口里跑完，环/目标 socket 腾出空间，
   下一次 parse 就能消费。
2. **`copied_seq != rcv_nxt` 的概率变大**：解析变慢后，`tcp_read_sock()` 结束时
   socket 里常常还有字节，`w_poll` 的第一条 rescue（`tbt_carrier.c:309-313`）
   在 50 ms 内把 w_rx 重新排上。无打印时 parser 往往一次把 socket 读空，
   正好落进 `copied_seq == rcv_nxt` 的死角。
3. **`schedule_work` 合并**：慢解析让 `sk_data_ready` 在 w_rx 仍在运行时到达。
   Linux workqueue 在 `process_one_work()` 调用函数前清掉 pending 位，
   所以运行期间的 `schedule_work` 会再排一次；w_rx 得到第二轮。
   快解析时第二轮所需的新字节还没到，work 已经返回，pending 位空着，没有第二轮。
4. **`tbt_rx_flush` 与 `w_drain` 的先后**：`tbt_rx_data()` 先入 `rx_q`，再
   `tbt_rx_flush()`（`tbt_rx.c:186-226`）；flush 只做 pause 判定和 `tbt_sock_kick`。
   打印把这条路径拉长，drain 更容易在 parser 的下一次尝试前完成
   “消费 → 减 `rx_queued` → 清 `paused` → `tbt_carrier_resume`”。
   无打印时 parser 先回到 socket，看到 `-EAGAIN` 并永久睡下。

因此 J 在 debug=3 下逐字节完成，而 `holes_repaired=0` 触发另一条断言：
空洞不是被发送端补上的，而是时序变慢后接收端自己走过去了。

---

## 6. 最小修复

### 6.1 直接重试 parser（主修复）

**`tbtun/tbt_rx.c`**：把 `tbt_rx_parse()`（`:749-754`）导出为一个只处理“已缓冲字节”的入口：

```c
/* Parse what this carrier already holds, without touching the socket.
 * Sleepable; called from w_rx, the same single writer that runs the actor. */
int tbt_rx_parse_pending(struct tbt_carrier *c)
{
    if (!c->parser->have)
        return 0;
    return tbt_rx_parse(c);
}
```

**`tbtun/tbtun.h`**：加声明 `int tbt_rx_parse_pending(struct tbt_carrier *c);`
（放在 `tbt_rx_skb` 附近）。

**`tbtun/tbt_carrier.c` `tbt_car_rx_work()`**（`:396-446`）：在 pause 检查之后、
`tcp_read_sock()` 之前：

```c
lock_sock(sk);
err = tbt_rx_parse_pending(c);
if (err == -EAGAIN) {
    release_sock(sk);
    return;                 /* 仍被拒绝：50 ms poll / drain resume 会再试 */
}
if (err) {
    release_sock(sk);
    /* 复用 actor 的非 EAGAIN 分支：rx_errs++、rx_last_err、stopping=1 */
    tbt_car_rx_error(c, err);
    return;
}
drained = tcp_read_sock(sk, &desc, tbt_car_rx_actor);
release_sock(sk);
```

（把 actor `:377-393` 的错误处理提成一个 `static void tbt_car_rx_error()`，
两处共用，避免两份 `stopping` 逻辑漂移。）

**精确条件**：
- 只在 `c->parser->have != 0` 时进入 parser；`have == 0` 时是一个空分支。
- 必须在 pause 检查之后：paused 的流不应该继续往满环里塞记录。
- 必须在 `tcp_read_sock()` 之前：这样 `copied_seq == rcv_nxt`、没有 skb 时也能重试。
- 返回 `-EAGAIN` 时直接返回，不再读 socket；唤醒由 50 ms poll（现在有效）
  或 drain 的 `tbt_carrier_resume` 提供。

**锁/上下文**：
- `w_rx` 是可睡眠的 work；`tbt_rx_parse()` 与 actor 在同一个上下文里运行，
  每条载体只有一个写者。
- 放在 `lock_sock(sk)` 之内是最保守的选择：它与 actor 完全同上下文，
  且排除并发的 socket 操作。`tbt_rx_parse()` 不读载体 socket，只碰
  `c->parser`、stream 的 `rx_lock`、`sess->lock`、`tx_lock`，不会引入新的锁序。
- `tbt_rx_parse()` 可能调 `tbt_stream_kill()`（`queue_work`）和
  `tbt_tx_wake()`（`schedule_work`），都不阻塞；不会在 socket 锁下睡眠。
- 不需要新的 work/timer；`w_poll` 的 `parser->have` 分支（`:314-325`）保留，
  修好后它才真正有效。

### 6.2 为什么不会在 bench n=4 上重新引入重传或延迟

- **门槛是 `parser->have`**。稳态下它只在一次读的尾部留下半条记录时非零；
  retry 只做一次 `tbt_rec_scan()` 的头检查（`tbt_rec.c:17-38`），
  返回 `TBT_REC_NEED_MORE`，O(1)，不分配、不拷贝、不上线。
- **它只补投已经收到的记录**。不产生新字节，不改 `tx_seq`/`tx_ack`/retain，
  不触发重传；唯一可能的副作用是和正常 parse 一样调用 `tbt_tx_ack()`/
  `tbt_fbk_kick()`，这些在正常路径本来就会发生。
- **不新增 work/timer**。`w_poll` 本来每 50 ms 就在跑；retry 让它的
  `schedule_work` 有实际效果，不增加调度频率。
- **bench n=4 的瓶颈是 4×10 Mbit 整形和载体 TCP**，不是解析器。
  每 `w_rx` 多一次头检查在噪声内；吞吐/重传的验收见 §7。

### 6.3 如果 `rx_last_err=-12`（-ENOMEM）才需要的附加修复

`tbt_rx_data()` 在分配失败时返回 `-ENOMEM`（`tbt_rx.c:162-166`），
actor 把它当协议错误杀了载体（`tbt_carrier.c:386-393`）。这时 §6.1 的重试
根本不会跑（`stopping` 已置位）。需要：

1. `tbt_rx_data()` 在 `!fr` 时返回 `-EAGAIN`，不是 `-ENOMEM`；
2. actor 把 `-ENOMEM` 当 `-EAGAIN`（返回 `used`，不置 `stopping`）；
3. **计数器位置**：`c->rx_frames++`、`c->rx_bytes`、`rx_high`、
   `sess->rx_data_frames++` 和 `fbk_kick` 都在分配之前（`tbt_rx.c:148-165`）。
   重试会重复计数。把它们移到分配成功之后，或加一个 `counted` 标志。
   这是独立的小改动，只在 `rx_last_err=-12` 时做。

---

## 7. 验证命令（只 J + G + bench n=4）

```sh
# J 是时序敏感的；debug=0（不要带 A，A 会重载 debug=2 把 bug 藏起来）
for i in 1 2 3; do tests/qemu/run.sh --probes J --timeout 180; done

# 预测/空洞修复不回归
tests/qemu/run.sh --probes G --timeout 180

# n=4 吞吐、严格性、重传
benches/run.sh throughput --n 4 --mib 40
```

判据：

- **J**：`rc=0`、逐字节、`holes_repaired >= 1`；far `fbk_rx > 0`；
  far `tx_ack` 前进；无 `TXSTALLKILL`/`RXHOLEKILL`；far `rx_ack` 能越过 98,304。
  三次都过才算过（§5 的竞态窗口）。
- **G**：`holes_unrecoverable=0`、`holes_repaired >= 1`；p99 不劣于
  `review/baseline.md` 的记录。
- **bench n=4**：`strict=1`、`rc=0`；goodput 与最近一份
  `benches/results/*-throughput*.csv` 在噪声内；`retx_ppm` 不高于同基线；
  `txq_max` 不增大。

---

## 8. 风险与未决

1. **生产者的最终确认**：工单没有给 `rx_errs`/`rx_last_err`。按 §3.3 先看这两个值；
   本文的主修复覆盖 `rx_errs=0` 的非致命 `-EAGAIN`。若是 `-12`，还要做 §6.3；
   若是 `-71`，是记录头损坏的另一条线（`tbt_proto.c:84-118`）。
2. **`holes_unrecoverable=1` 的归属**：接收侧（`tbt_rx.c:895`）和发送侧
   （`tbt_tx.c:1184`）共用一个计数器。日志 `ack=0 txseq=98304` 只能属于 far 的
   发送侧停顿；但若 `rx_max_stall_us≈500000`，先到的是接收侧的 500 ms 预算。
   两者对本文的根因结论没有影响，但会影响“修好后 J 的哪条断言先过”。
3. **门控不可满足**：`tbt_tx.c:1008-1013` 要求每条其它活路径的 `uack >= hole->seq`。
   窗口被钉在洞上后，其它路径不再被喂，`uack` 永远停在洞前；门控永远关着。
   J 只能靠 RTO 定时器（`:466-646`）。如果 2×RTO（`:537-548`）加 far 的 500 ms
   预算在 2% 重排下仍太紧，需要第二个修复（例如门控忽略“没有 retained 帧在洞之后”
   的路径，或对第一个洞缩短 2×RTO）。**不要和 §6.1 一起改**；先单独量 J。
4. **poll 的 50 ms 重试**：`tbt_carrier.c:314-325` 会对任何尾部有半条记录的载体
   每 50 ms 排一次 w_rx。加了 §6.1 后它是一次廉价的头检查；不加则是空转。
   不是正确性风险，但值得在 stats 里观察 `rx_work_runs`/`rx_read_zero`。
5. **`-ENOMEM` 重试的计数语义**：见 §6.3 第 3 条；不改计数器会虚增
   `rx_frames`/`recv_cnt`，进而影响 loss_ppm 和门控。

---

## 9. 代码索引（按结论）

| 结论 | 代码 |
|---|---|
| parser 停在已离开 socket 的记录上 | `tbt_rx.c:762-825`（`*consumed = done`）、`tbt_carrier.c:347-394` |
| 没有新字节时 actor 不会被调用 | `tbt_carrier.c:434`（`tcp_read_sock`）、`:64-76`（`sk_data_ready`） |
| poll 只排 w_rx、不直接解析 | `tbt_carrier.c:309-326` |
| drain 只在 `paused` 时唤醒载体 | `tbt_sock.c:239-243`、`:263-267`；`tbt_carrier.c:1243-1270` |
| pause 规则与 64 MiB 环 | `tbt_rx.c:899-911`；`tbtun.h:141-143` |
| FBK 入队计数与 uack 填写 | `tbt_tx.c:1967-1968`、`:1575` |
| FBK 豁免不是绝对 | `tbt_tx.c:1504-1513`；`tbt_carrier.c:650-676` |
| 门控要求其它路径 uack 越过洞 | `tbt_tx.c:1008-1013` |
| RTO 定时器是唯一旁路 | `tbt_tx.c:466-646` |
| 接收洞预算 500 ms | `tbt_rx.c:842-897`；`tbt_parse.h:68` 默认值 |
| 发送侧停顿杀流 | `tbt_tx.c:1132-1186` |
| J 的构造（raw、n=4、1%/2%） | `tests/qemu/init:885-915` |
