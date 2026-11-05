# P3b1-B：探针 B `tx_ack=0 / fbk_rx=0 / holes_unrecoverable=1 / up=0 / lost=1` 的独立根因

> 只读分析。未修改源码、未构建、未跑 QEMU。
> 对比基准：`/tmp/tbtun-p2b-after`（P2b+P2d 完成快照）与当前树（P3a + P3b1）。
> 行号取自 2026-11-05 当前树；P3b1 正在恢复，落地前按 §6 重新 grep。

## 0. 结论

1. **直接原因不是 HELLO 未 join，而是 OPEN 未写成功。** P3b1 用每流 `announced_mask` 替代每载体
   `c->open_sent`，但 `announced_mask` 只有“index”维度，没有 generation 维度。载体在同一 index
   被替换后，旧 bit 仍在；新载体的 OPEN 被 `tbt_tx_pump()` 的 OPEN 选择分支按旧 bit 丢掉，
   `c->up` 保持 0，DATA 被 `!up` 门挡住，FBK 无流可发 → `tx_ack=0`、`fbk_rx=0`，最终
   `holes_unrecoverable=1`，健康检查把载体记为 `lost=1`。
2. **第二个独立问题是 far 的 `tbt_session_bind_sid()` 被 P3b1 简化成“未知 sid 一律建流”。**
   它不再检查旧流是否 closing、不再 tombstone 旧 sid、不再 reset session ACK；一个迟到/旧 sid
   可以覆盖 `sess->far_sid`，让 session 级 FBK/pump/ACK 指向错误的流。P3a 的每流 target 是对的，
   但 session 级状态仍是单流，二者不匹配。
3. **`tbt_session_for_each_stream` 已不在当前树中**；回退没有留下函数级引用计数不平衡。
   `tbt_session_bind_sid`/`tbt_session_join`/`tbt_session_destroy`/`w_target`/`w_kill` 的
   get/put 是配对的。真正需要立刻修的是 `tbt_frame_alloc()` 没有初始化 `fr->st`：
   HELLO/PAD/RETIRE 的 `st == NULL`，`tbt_frame_free()` 会对未初始化指针做 `tbt_stream_put()`，
   可能误减一个活流的引用（或崩）。这是 P2 就存在的潜在 bug，P3b1 的控制帧churn 把它暴露出来。
4. **最小恢复点**：保留 P3a 的每流 target，把 P3b1 的“并发使能”四处回退到 P2 单流形态：
   近端 `far_sid` 门、far 单流 bind、`c->open_sent` OPEN/DATA 门、`tbt_pending_work` 的单流返回。
   `announced_mask` 可以留字段但不再参与门控；`fr->st = NULL` 必须补。P3b1 的两条并发/churn
   留到 P3b2，等 session 级 ACK/FBK/holes/pump 全部 per-stream 化并定义锁序后再开。

---

## 1. 证据范围

当前树与 P2 快照的差异中，P3a 与 P3b1 混在一起。按 `review/analysis/multiconn.md` §13.4.2/§13.4.3：

- **P3a（保留，已实测探针 B PASS）**：`struct tbt_stream` 的 `target/target_started/w_target`；
  `tbt_target_work_fn`/`tbt_carrier_open_target`/`tbt_target_attach`/`tbt_target_detach`/
  `tbt_far_open_seen` 全部按流；session 级 target 字段删除；`w_kill`/destroy 逐流 cancel。
- **P3b1（未通过，待回退）**：near 删除 `far_sid` 单流门；far 按未知 sid 建流；
  `announced_mask` 替代 `c->open_sent`；`pub.two` 改为 A/B 都可用；
  尝试 `tbt_session_for_each_stream` 逐流 retx/stall/holes/SACK（已回退遍历，但 P3b1 其余改动仍在）。

P2 快照的 `tbt_session_bind_sid()`、`tbt_stream_start_now()`、`tbt_pending_work()`、
`tbt_tx_announce()` 与 pump 的 OPEN/DATA 门，是 P3a 之前已经通过探针 B 的形态。
P3a 只把 target 改成每流；它没有改这些门。因此恢复点应回到“P2 门 + P3a target”。

---

## 2. 根因 A：`announced_mask` 缺少 generation 维度，OPEN 被丢，`up` 永远为 0

### 2.1 当前代码

`tbt_tx_announce()`（`tbt_tx.c:177-196`）：

```c
if (!c || !atomic_read(&c->connected) || !READ_ONCE(c->hello_sent) ||
    (READ_ONCE(st->announced_mask) & BIT(i)) ||
    atomic_read(&c->stopping))
    continue;
tbt_tx_control_pinned(sess, st, (u8)i, TBT_F_OPEN, st->sid, 0,
                      NULL, 0, true, gfp);
```

pump 的 OPEN 选择（`tbt_tx.c:1519-1524`）：

```c
else if (type == TBT_F_OPEN)
{
    if (!READ_ONCE(c->hello_sent) ||
        (fr->st && (READ_ONCE(fr->st->announced_mask) & BIT(c->index))))
        goto drop;
}
```

pump 的 OPEN 完成（`tbt_tx.c:1726-1733`）：

```c
case TBT_F_OPEN:
    if (fr->st)
        WRITE_ONCE(fr->st->announced_mask,
                   READ_ONCE(fr->st->announced_mask) | BIT(c->index));
    WRITE_ONCE(c->open_sent, true);
    atomic_set(&c->up, 1);
    ...
```

DATA 门（`tbt_tx.c:1556-1558`）：

```c
if (type == TBT_F_DATA &&
    (!fr->st || !(READ_ONCE(fr->st->announced_mask) & BIT(c->index))))
    continue;
```

### 2.2 失败序列

1. 流 S 在载体 index i 上写 HELLO；`case TBT_F_HELLO` 先把 `c->up = 0`，再排 OPEN。
2. OPEN 写成功：`announced_mask |= BIT(i)`，`c->open_sent = true`，`c->up = 1`。数据开始流动。
3. 载体 i 因任一原因失效并被替换（同 index，新 `c->gen`）。新载体的 HELLO 写成功：
   `c->up = 0`，HELLO 分支排一份新 OPEN（`fr->gen = 新 gen`）。
4. pump 处理新 OPEN：`fr->st->announced_mask & BIT(i)` 仍是**旧载体**写下的 1 →
   `goto drop`。新 OPEN 被释放，`c->up` 保持 0。
5. 新载体的 DATA 选择：先过 `if (!atomic_read(&c->up)) continue;` → 永远不选。
   没有 DATA，far 没有流/没有 FBK；near 的 `tx_ack` 保持 0、`fbk_rx` 保持 0。
6. 500 ms 后 `tbt_tx_stall_check()`/`tbt_rx_check_hole()` 杀流：
   `holes_unrecoverable=1`。载体健康检查看到 `snd_nxt`/`snd_una` 静止，记 `lost=1`。

这与探针 B 的 `tx_ack=0 / fbk_rx=0 / holes_unrecoverable=1 / up=0 / lost=1` 完全一致。
`up=0` 的直接来源就是第 4 步：**OPEN 被 per-stream 的陈旧 bit 丢掉**；不是 HELLO 未 join，
也不是 far 没有 target。

### 2.3 为什么 P2 的 `c->open_sent` 没有这个问题

`c->open_sent` 在 `struct tbt_carrier` 上。载体被替换时创建新的 `struct tbt_carrier`，
`open_sent` 从 false 开始，新 OPEN 不会被旧状态挡住。P2 的 `tbt_tx_announce()` 还会在排 OPEN
前 `atomic_set(&c->up, 0)`，排失败时恢复 `up=1`，状态与帧一致。P3b1 去掉了这段 `up` 维护，
只留下 per-stream 位图，导致“位图说写过、载体说没 up”的不一致。

### 2.4 判别方法

在探针 B 的失败 guest 里看：

```sh
cat /proc/net/tbtun/peers      # state=down/stalled, up 列 0
cat /proc/net/tbtun/stats      # tx_ack=0 fbk_rx=0 holes_unrecoverable=1
dmesg | grep -E "DIALED|ACCEPTED|carrier stall|join_dup|hole"
```

若日志显示“同一 index 先 DIALED/OPEN，再 DIALED（新 gen），但之后没有 DATA/FBK”，
且 `peers` 的 `up=0`，就是本条。若日志显示 HELLO 被 far 拒绝/`join_dup`，先查第 3 节。

---

## 3. 根因 B：far 的 `tbt_session_bind_sid()` 简化破坏单流状态

### 3.1 当前代码（`tbt_session.c:664-701`）

```c
if (role == TBT_ROLE_NEAR) { ... find, OK/DROP ... }

spin_lock(&sess->lock);
st = tbt_stream_find_locked(sess, sid);
if (st) { dead = state == DEAD; unlock; return dead ? DROP : OK; }
spin_unlock(&sess->lock);

st = tbt_stream_get(sess, sid, true, false);
if (!st) return BAD;
spin_lock(&sess->lock);
WRITE_ONCE(sess->far_sid, sid);
spin_unlock(&sess->lock);
tbt_far_open_seen(st);
tbt_stream_put(st);
return TBT_BIND_ADOPT;
```

它不再：

- 检查旧流 `tbt_stream_is_closing()`；
- 对旧 sid 做 `tbt_stream_tombstone_locked()`；
- 调用 `tbt_session_reset_ack_locked()`；
- 拒绝“旧流还活着时的新 sid”。

### 3.2 后果

P3b1 同时删除了近端 `tbt_stream_start_now()` 的 `far_sid` 单流门（`tbt_sock.c:529-570`），
所以近端可以连续/并发启动多个流。far 侧收到一个新 sid 就建流并覆盖 `sess->far_sid`：

- session 级 `sess->rx_ack`、`peer_paused`、`fbk_frames`、`holes`、`predict`、`pause_cleared_ns`、
  `last_ack_ns` 仍是一份；P3a 的 target 是每流，但 FBK/pump 的 `tbt_session_stream(sess)` 取
  `sess->far_sid`，现在可能指向一个迟到/旧的流。
- 旧流的 `st->tx_ack`（P2a 已每流）不会自动 reset；新流的 `tx_ack` 从 0 开始，但 far 的 FBK
  可能用旧流的 `rx_next`/sid 回给 near，near 的 `tbt_tx_ack(sess,c,uack,sid)` 找不到对应流或
  应用到错误流 → `tx_ack=0`。
- `tbt_fbk_work()`/`tbt_tx_stall_check()`/`tbt_tx_pump()` 都通过 `tbt_session_stream(sess)`
  取一个“主”流；旧流的 retain/holes 不再被处理，新流的 session 级状态又被旧值污染。
  这与 P3b1 churn 的“约 1% 读超时”是同一类问题：**session 级状态没有 per-stream 化，
  但单流门已经拆掉**。

P2 的 `tbt_session_bind_sid()` 用 `tbt_stream_is_closing()` 决定是否允许换 sid，并在换 sid 时
reset ACK + tombstone 旧流。P3a 的每流 target 不需要删掉这段；P3b1 删掉它是错误的。

### 3.3 判别方法

在失败 guest 的 `dmesg`/`stats` 里找：

```sh
grep -E "sid|adopt|join|tombstone|tx_ack|fbk" dmesg
cat /proc/net/tbtun/stats | grep -E "tx_ack|fbk_rx|holes"
```

如果 `sess->far_sid` 在一条流结束前变过，或 `peers`/`stats` 的 `tx_ack` 在 0 与非 0 之间跳，
就是本条的指纹。单流探针 B 也可能因为“setup 流 + 主流程”触发：setup 流还在 closing 时，
主流程的 sid 被 far 直接 adopt，旧 session ACK 未 reset。

---

## 4. 根因 C：`tbt_session_for_each_stream` 回退后的引用计数

### 4.1 当前树没有这个函数

`grep -rn "for_each_stream" tbtun` 无结果；它只出现在 `review/analysis/multiconn.md:1876` 的
P3b1 失败记录里。当前树是“回退遍历之后”的形态，所以不存在“函数还在但引用没配平”的直接问题。

### 4.2 现有引用路径是配对的

- `tbt_session_bind_sid()`：`tbt_stream_get(..., true, ...)` 返回 hash + caller 两引用；
  `tbt_far_open_seen(st)` 不取引用；`tbt_stream_put(st)` 释放 caller；hash 引用留给 reap。
- `tbt_session_join()`（`tbt_session.c:1036-1093`）：两次 `tbt_session_stream(sess)` 各取一引用，
  各自 `tbt_stream_put`；`tbt_carrier_open_target(st)` 在成功 queue 前 `refcount_inc`，
  `tbt_target_work_fn()` 末尾 `tbt_stream_put`；queue 失败时函数自己 put。
- `tbt_stream_kill()`：`refcount_inc` 给 `w_kill`；`tbt_kill_work()` 末尾 put。
- `tbt_session_destroy()` 第一遍：`refcount_inc_not_zero` + `cancel_work_sync(w_target/w_kill)` +
  `tbt_sock_detach` + put；第二遍 unlink 后 put hash 引用。配对。
- `tbt_reap_work()`：unlink 后 put hash 引用。配对。

### 4.3 真正的引用 bug：`fr->st` 未初始化

`tbt_frame_alloc()`（`tbt_tx.c:71-88`）用 `kmalloc`，初始化了 `len/off/open_pending/pinned/
dup_copy/retx_copy/path/avoid_mask/ret/gen`，**没有初始化 `fr->st`**。而
`tbt_frame_free()`（`tbt_tx.c:61-69`）会：

```c
struct tbt_stream *st = fr->st;
fr->st = NULL;
if (st)
    tbt_stream_put(st);
```

HELLO/PAD/RETIRE 走 `tbt_tx_control_pinned(sess, NULL, ...)`，`fr->st` 保持未初始化。
如果这块 `kmalloc` 复用了之前 DATA 帧的内存，`fr->st` 就是旧流的指针；HELLO/PAD 写完后
`tbt_frame_free()` 会对旧流做一次多余的 `tbt_stream_put()`。这会：

- 提前释放一个仍然挂在 hash/retain/队列上的流 → `tx_ack=0`、`fbk_rx=0`、后续 UAF；
- 或对已释放对象减引用 → refcount underflow/oops。

P2 快照同样缺这一行，所以它不是 P3b1 引入的；但 P3b1 增加了控制帧/每流 target 的分配-释放
churn，更容易让 `kmalloc` 复用到带旧 `fr->st` 的对象。**这是必须先修的 P0，独立于 P3b1 回退。**

修复：`tbt_frame_alloc()` 里 `fr->st = NULL;`（或改用 `kzalloc`）。这不会改变任何门控语义，
只消除悬空 put。

---

## 5. `up=0 / lost=1` 的直接来源

按时间顺序：

1. `c->up` 只在 pump 的 `case TBT_F_OPEN` 里置 1（`tbt_tx.c:1731`）。
2. P3b1 的 `tbt_tx_announce()` 不再在排队 OPEN 前清 `up`；HELLO 分支会清 `up` 再排 OPEN。
3. OPEN 被 `announced_mask` 的陈旧 bit（同 index 旧 gen）或 `fr->gen != c->gen` 丢掉。
4. `up` 保持 0；pump 的 DATA 分支在 `if (!atomic_read(&c->up)) continue;` 处永远跳过。
5. 没有 DATA → far 不建流/不回 FBK → `tx_ack=0`、`fbk_rx=0`。
6. 500 ms 后 hole/stall 预算杀流 → `holes_unrecoverable=1`；载体 `snd_nxt/una` 静止 →
   `tbt_car_poll_work()` 的 stall 判定 → `lost=1`。

因此 `lost=1` 是结果，不是原因；`up=0` 是 OPEN 未写成功的结果；OPEN 未写成功是
`announced_mask` 缺 generation 维度的结果。HELLO 本身通常已经 join 成功（否则 `peers`
的 `state` 会更早是 down，且 `join_dup`/`session_join_bad` 会增长）。

---

## 6. 最小恢复点（保留 P3a）

原则：**只回退 P3b1 的“并发使能”，保留 P3a 的每流 target 和 P2b/P2d 的 per-path 字节/SACK。**
不要整文件回退 `tbt_session.c`/`tbt_sock.c`/`tbt_tx.c` 到 P2，否则会丢 P3a/P2d。

### 6.1 `tbt_sock.c`

| 函数 | 当前锚点 | 回到 P2 形态 |
|---|---|---|
| `tbt_stream_start_now()` | 529-570 | 恢复 `if (closing \|\| far_sid) return -EBUSY;`；在锁内 `sess->far_sid = sid;`；`tbt_stream_get` 失败时回滚 `far_sid=0`。删除 P3b1 的“latest stream”二次加锁覆盖。 |
| `tbt_pending_work()` | 576-640 | 锁内条件恢复 `closing \|\| far_sid \|\| list_empty(&pending)`；末尾恢复 `return; /* one live stream only */`。删除 `continue; /* start every queued connection */`。 |

保留：`tbt_far_open_seen(st)`、`tbt_target_attach(st)`、`tbt_target_detach(st)`（P3a）。

### 6.2 `tbt_session.c`

| 函数 | 当前锚点 | 回到 P2 形态 |
|---|---|---|
| `tbt_session_bind_sid()` | 664-701 | 恢复 P2 的 `tbt_stream_is_closing()` + `drop/bad/adopt` 逻辑：near 只认已存在 sid；far 只在 `!far_sid` 或旧流 closing 时 adopt；adopt 时 `tbt_session_reset_ack_locked()` + `tbt_stream_tombstone_locked(cur)`；对 tombstone 的迟到帧返回 DROP；其他新 sid 返回 BAD。 |
| 新增/恢复 `tbt_stream_is_closing()` | 当前已删 | 从 P2 快照原样加回（`state==DEAD` / `rx_fin_seen` / `sk_state` 为 CLOSE_WAIT/LAST_ACK/CLOSING/CLOSE）。 |
| `tbt_session_join()` | 1036-1093 | 保留 P3a 的 `tbt_carrier_open_target(st)`；不需要回退。 |

**P2 bind 与 P3a `tbt_far_open_seen(st)` 的适配**：P2 在 `adopt` 分支调用
`tbt_far_open_seen(sess)`；P3a 的函数接收 stream。adopt 后按 sid 取一个引用再调用：

```c
if (adopt && sess->tunnel->cfg.role == TBT_ROLE_FAR) {
    struct tbt_stream *ast = tbt_stream_get(sess, sid, false, false);

    if (ast) {
        tbt_far_open_seen(ast);
        tbt_stream_put(ast);
    }
}
```

不要恢复 P2 的 `sess->target_started = false;`：P3a 的 target 是每流，新流的
`st->target_started` 默认 false，`tbt_far_open_seen(st)` 会为它排 `w_target`。

### 6.3 `tbt_tx.c`

| 位置 | 当前锚点 | 回到 P2 形态 |
|---|---|---|
| `tbt_tx_announce()` | 177-196 | 门恢复 `READ_ONCE(c->open_sent)`；排 OPEN 前 `atomic_set(&c->up, 0);`；`tbt_tx_control_pinned(...)` 失败时 `atomic_set(&c->up, 1);`。删除 `announced_mask` 判断。 |
| pump OPEN 选择 | 1519-1524 | 恢复 `if (!hello_sent \|\| READ_ONCE(c->open_sent)) goto drop;`。 |
| pump DATA 门 | 1556-1558 | 恢复 `if (type == TBT_F_DATA && !READ_ONCE(c->open_sent)) continue;`。 |
| pump OPEN 完成 | 1726-1733 | 保留 `c->open_sent=true; up=1;`；`announced_mask` 的写可以删除（字段保留但不再参与门控）。 |
| `tbt_frame_alloc()` | 71-88 | **新增 `fr->st = NULL;`（P0，必须做）**。 |

保留：`tbt_tx_control_pinned(sess, st, ...)` 的 `st` 参数/引用（P3b1 引入，但与门控解耦后
只是多一个平衡引用；不必为恢复 B 而改所有调用点）。如果后续要彻底回退 P3b1，可再把签名改回
P2 的 `(sess, path, ...)`；不是恢复 B 的必要条件。

### 6.4 `tbtun.h`

- `announced_mask`（273）可以保留为未使用字段，或后续删除；不要让它继续参与门控。
- `fr->st`（202）保留；`tbt_frame_alloc` 初始化它。
- P3a 的 `target/target_started/w_target` 保留。
- P2b/P2d 的 `path_end`/`payload_bytes_sent`/`spec_path_end` 保留。

### 6.5 测试契约

- `adversarial` 的 `pub.two` 必须回到 P2 契约（A 可用、B 被拒），直到 P3b2。
  P3b1 的“A/B 都可用”是并发目标，不是当前恢复目标。
- `pub.churn` 预期回到 P2b 的 10000/0/0；如果仍失败，按 §3 的 far bind 状态继续查。
- 探针 B 恢复后再跑探针 I；`announced_mask` 回退不会影响探针 I 的 per-path 字节/SACK。

### 6.6 不需要回退的

- P3a 的每流 target：`tbt_target_work_fn`/`tbt_carrier_open_target`/`tbt_target_attach`/
  `tbt_target_detach`/`tbt_far_open_seen(st)`/`w_target` 的 cancel/put。
- P2b 的 SACK、P2d 的 `payload_bytes_sent`/`path_end`/`spec_path_end`。
- `tbt_rx.c` 的 `tbt_rx_data`/`tbt_rx_bind_stream` sid 分派（P2a/P2d 形态；P3b1 没有改这里）。
- `tbt_session_destroy()` 的逐流 `cancel_work_sync(&v->w_target)`（P3a）。

---

## 7. 验证计划（等恢复补丁落地后）

1. `make check` + `review/lint.sh`：host 全绿；`test_reorder` 51,831 / 0；lint 15/15+12/12。
2. 探针 B 三次：`tests/qemu/run.sh --probes B --timeout 180`；期望 4 MiB 双向 strict=1、
   interleaved=1，`tx_ack`/`fbk_rx` 非 0，`up>=1`，`lost=0`。
3. 探针 I 三次：`n=4` ≥6 Mbit/s、ratio ≥1.8；对比 P3a 基线。
4. `pub.churn`：期望 10000/0/0；`pub.two` 按 P2 契约（B refused）。
5. 若 B 仍失败：用 §2.4/§3.3 的判别命令区分“OPEN 被丢”与“far bind 状态错”；
   若 `fr->st` 修复后 B 恢复但 I 仍低，按探针 I 的 per-path 字节路径继续查 P2d。

## 8. 风险

- 回退 `announced_mask` 会让 P3b1 的 `pub.two` 新契约失效；这是有意的：并发留到 P3b2，
  先把单流 B 恢复到 P3a 的通过状态。
- `fr->st = NULL` 是独立 P0；即使 P3b1 全部回退，也必须保留，否则控制帧的悬空 put 仍在。
- P2 bind 的 `tbt_stream_is_closing()` 依赖 `st->sk`；P3a 的每流 target 在 attach 前 `st->sk`
  为 NULL，`tbt_stream_is_closing` 仍能靠 `state`/`rx_fin_seen` 判断；不要把它改成依赖 target。
- `tbt_session_for_each_stream` 的遍历不要在恢复补丁里重新引入；它需要先定义
  `pump_lock → tx_lock → sess->lock` 的锁序和每流状态的所有权，否则会再次死锁。

---

## 9. 追加（multiconn 最小恢复后）：当前树已整体回退到 P2b，over-revert 丢了 P3a 和 P0

### 9.1 当前 diff 的事实

对下列七个文件执行：

```sh
for f in tbtun.h tbt_tx.c tbt_session.c tbt_sock.c tbt_rx.c tbt_carrier.c tbt_listen.c; do
    diff -u /tmp/tbtun-p2b-after/$f tbtun/$f
done > /tmp/p3b1-current.diff
wc -l /tmp/p3b1-current.diff
```

结果：**0 行**。当前树与 `/tmp/tbtun-p2b-after` 在七个文件上逐字节相同。

这意味着“最小恢复”不是选择性回退，而是**整树回到了 P2b 快照**：

- P3b1（near `far_sid` 门、far 单流 bind、`announced_mask`、`open_sent` 门、sid 分派、
  `for_each_stream` 残留）已全部消失——这部分是对的。
- **P3a 的每流 target 也一起消失了**（`struct tbt_session` 又有 `target/target_started/w_target`，
  `tbt_far_open_seen(sess)`/`tbt_target_attach(sess)`/`tbt_carrier_open_target(t)` 回到 session 级）。
  这与“保留 P3a”的目标冲突。
- **P0 的 `fr->st = NULL` 也消失了**。P2b 的 `tbt_frame_alloc()` 不初始化 `fr->st`；
  当前树逐字节等于 P2b，所以也没有这一行。

### 9.2 逐条分类（对“恢复前的 P3a+P3b1 树”，当前已不存在）

| 差异 | 归属 | 处理 |
|---|---|---|
| `struct tbt_stream` 的 `target/target_started/w_target`；`tbt_far_open_seen(st)`、`tbt_target_attach(st)`、`tbt_target_detach(st)`、`tbt_carrier_open_target(st)`、`tbt_target_work_fn(st)`、destroy 逐流 cancel | P3a | **保留**（当前被误删） |
| near `tbt_stream_start_now()`/`tbt_pending_work()` 删除 `far_sid` 门 | P3b1 | 回退（当前已回退） |
| far `tbt_session_bind_sid()` 的“未知 sid 一律建流” | P3b1 | 回退（当前已回退） |
| `announced_mask` 替代 `c->open_sent`（announce/pump OPEN/DATA 三处） | P3b1 | 回退（当前已回退） |
| `tbt_tx_control_pinned(sess, st, ...)` 的 `st` 参数/引用 | P3b1 | 可回退（当前已回退；但 `fr->st=NULL` 必须保留） |
| `tbt_session_for_each_stream` 逐流 retx/stall/holes/SACK 的签名/调用 | P3b1 残留 | 回退（当前已回退） |
| `path_end`/`payload_bytes_sent`/`spec_path_end`/SACK/`st->tx_ack` | P2a/P2d | 保留（当前在 P2b 中保留） |
| `fr->st = NULL` | P0 修复 | **必须补**（当前缺失） |

### 9.3 最可能的残留：`fr->st` 未初始化

当前 `tbt_tx.c:tbt_frame_alloc()`（约 71-88 行）用 `kmalloc`，初始化了
`len/off/open_pending/pinned/dup_copy/retx_copy/path/avoid_mask/ret/gen`，**没有 `fr->st = NULL`**。
当前 `tbt_tx.c:tbt_frame_free()`（约 61-69 行）却：

```c
struct tbt_stream *st = fr->st;
fr->st = NULL;
if (st)
    tbt_stream_put(st);
```

P2b 的 `tbt_tx_control_pinned()` 不接收 `st`，所以 **HELLO/OPEN/FIN/RST/PAD/RETIRE 全部控制帧**
的 `fr->st` 都是未初始化的。只要这块 `kmalloc` 复用了之前 DATA 帧的内存，`fr->st` 就是旧流指针；
控制帧写完后 `tbt_frame_free()` 会对旧流做一次多余的 `tbt_stream_put()`：

- 旧流的引用被提前减到 0 → 流被 RCU 释放，但 hash/retain/tx_q 仍可能指向它；
- 后续 pump/retain 使用已释放流 → `tx_ack=0`、`fbk_rx=0`、`txq` 卡在硬上限
  （观测 98304 ≈ 96 KiB）、`holes_unrecoverable=1`、`lost=1`；
- 若对已释放对象减引用 → refcount underflow/oops；pump 可能在同一对象上空转/软锁死。

P2b 之前探针 B 能过，是因为 `kmalloc` 恰好给到零页/零对象；一旦分配-释放模式变化（P3b1 的
每流 target/控制帧 churn，或只是重跑），就会复现。**这不是 P3b1 的门控问题，而是 P2b 就有的
潜在 UAF/refcount bug；当前 over-revert 把它留了下来。**

**修复（唯一必须补的行）**：

```c
/* tbtun/tbt_tx.c:tbt_frame_alloc(), after fr->gen = 0; */
fr->st = NULL;
```

（或把 `kmalloc` 改成 `kzalloc`，但只补一行更精确。）

### 9.4 精确回退/恢复清单

**当前树已经等于 P2b，所以七个文件没有“还要回退”的 P3b1 行。** 需要做的是两件不同的事：

1. **必须先补 P0**：
   - `tbtun/tbt_tx.c` / `tbt_frame_alloc()` / 约 71-88 行：加 `fr->st = NULL;`。
   - 保留 `tbt_frame_free()` 的 put；这样每个控制帧的 `fr->st` 是 NULL，不再误减流引用。
2. **若目标是“保留 P3a”**（当前已丢）：在 P0 修好、B 绿之后，按 §6.6 重新只加 P3a：
   - `tbtun.h`：`struct tbt_stream` 的 `w_target/target/target_started`；`struct tbt_session`
     删除 `target/target_started/w_target`；原型改 `tbt_far_open_seen(st)`/`tbt_target_attach(st)`/
     `tbt_target_detach(st)`/`tbt_carrier_open_target(st)`。
   - `tbt_sock.c`：`tbt_stream_init_work` 加 `w_target`；`tbt_kill_work` cancel `w_target`；
     `tbt_far_open_seen(st)`、`tbt_target_attach(st)`、`tbt_target_detach(st)`；`release_socket_now`
     清 `st->target`。
   - `tbt_carrier.c`：`tbt_target_work_fn(st)`、`tbt_carrier_open_target(st)`、`tbt_session_join`
     的 target 调用。
   - `tbt_session.c`：destroy 第一遍逐流 cancel `w_target`；bind 的 adopt 分支按 §6.2 的
     `tbt_stream_get(sid)+tbt_far_open_seen(st)+put` 适配。
   - **不要**重新引入 `announced_mask`、near `far_sid` 门删除、far 未知 sid 建流。

**若目标只是先让 B 绿**：只做第 1 条（P0），不要先恢复 P3a；P3a 的每流 target 与单流门不冲突，
但一次只改一个变量。

### 9.5 回退后 B 仍失败时的最可能残留

按概率排序：

1. **`fr->st` 未初始化**（§9.3）：先补，再重跑 B。若 B 恢复，`txq=98304`/soft lockup 是
   控制帧误 put 释放流后的次生现象。
2. **`.ko` 与源码不一致**：当前树已逐字节回到 P2b；如果失败的 B 用的是回退前混合构建的
   `tbtun.ko`，那组数字描述的不是当前树。先 `make check` 重建，确认 `sha256sum tbtun.ko`
   与 guest 的 `insmod` 一致，再跑 B。
3. **P2b 的 pump/replace 锁序**：`txq=98304` 达到硬上限、`tx_ack=0` 时，`tbt_tx_retx()`/
   `tbt_tx_hole_check()` 仍可能注入副本；pump 软锁死优先查
   `tbt_tx_pump()`（`pump_lock`）与 `tbt_replace_work()`/`tbt_carriers_stop()` 的
   `cancel_work_sync(&w_tx)` 是否在 `pump_lock` 外（review 6/7 的回归面）。
4. **far 的 HELLO/join**：P2b 的 `tbt_rx_hello()`/`tbt_session_join()` 若因 profile/gen/nonce
   拒绝，near 会看到载体被关闭 → `lost=1`；看 `dmesg` 的 `join_dup`/`session_join_bad`/
   `carrier stall`。这与 §2/§3 的判别命令相同。

### 9.6 结论

- 当前 `diff -u` 为空：**P3b1 已全部回退，但 P3a 和 `fr->st=NULL` 也被一起回退掉了**。
- 先补 `fr->st = NULL`（P0），重建，跑 B；这是最可能让 `txq=98304`/`tx_ack=0`/soft lockup
  消失的一行。
- 若 B 绿且要保留 P3a，再按 §9.4 第 2 条单独恢复每流 target；P3b1 的并发门控继续留在 P3b2。
