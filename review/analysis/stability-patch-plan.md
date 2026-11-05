# stability patch plan — review 20 / 22 / 21

> 本文件只描述补丁，不修改源码、不构建、不跑 QEMU。
> 锚点取自 2026-11-05 当前工作树。`perf` 正在改 `tbt_tx.c`/`tbt_rx.c`，`eng-contract` 在改
> `tbt_parse.c`/`tbt_procfs.c`；行号会漂移，落地前必须按 §5 的 grep 重新定位。
> 依赖与合并门禁见 §0。

## 0. 合并门禁与所有权

| 补丁 | 文件 | 依赖 | 本波能否独立落地 |
|---|---|---|---|
| P20-1/2 重排树三分支 + 入口裁剪 | `tbt_rx.c` | perf 冻结 `tbt_rx.c` | 否，等 perf |
| P22-1 瞬态错误重新入队 | `tbt_tx.c` | perf 冻结 `tbt_tx.c` | 否，等 perf |
| P22-2 FIN/RST 跨替换保留 | `tbt_tx.c` | perf 冻结 `tbt_tx.c` | 否，等 perf |
| P22-3 FIN/RST 按 index 排队 + 去重 | `tbt_tx.c`、`tbtun.h` | perf；`tbtun.h` 与其他人协调 | 否，等 perf |
| P21-1 max-CAS | `tbtun.h`、`tbt_tx.c`、`tbt_rx.c`、`tbt_carrier.c`、`tbt_session.c` | perf 冻结 tx/rx；类型改动要一次编译通过 | 否，等 perf |
| P21-2 per-carrier 分片 | `tbtun.h`、`tbt_rx.c`、`tbt_carrier.c`、`tbt_tx.c`、`tbt_session.c`、`tbt_procfs.c` | perf + eng-contract | 否，等两者 |
| 测试 `far.overlap` | `adversarial/adv_probe.c`、`adversarial/init` | 无（补丁前会失败，属预期） | 是 |
| 测试 `near.fin_eagain` | `adversarial/adv_probe.c`、`adversarial/init` | 无（补丁前会失败） | 是 |
| lint `check_monotone_ack` | `review/lint.sh` | 无（补丁前会失败；可与 P21-1 同交） | 是（建议同交） |
| 文档/README | `DESIGN.md`、`README*` | 无 | 是 |

原则：**同一文件同一时间只有一个合并者**。P20 与 P21-1/2 都改 `tbt_rx.c`，必须串行：
先 P20（逻辑独立、验收明确），再 P21；P22 与 P21 都改 `tbt_tx.c`，先 P22（控制面正确性），
再 P21（计数/原子性）。P21-2 的 `tbt_procfs.c` 读取点最后做，避免与 eng-contract 冲突。

---

## 1. review 20：`seq < rx_next` 的重叠重排帧

### 1.1 不变量

修复后，重排树只保存“尚未交付”的帧：

1. 树中任意节点满足 `seq + len > rx_next`；`seq < rx_next` 的节点只可能短暂存在，
   在下一次 `tbt_rx_pull_locked()` 被清除或裁剪。
2. `rx_ooo_bytes` 只计树中帧的完整 `len`；节点离开树时减**整帧 len**，不是重叠前缀。
3. `rx_queued` 只计进入 `rx_q` 的**尚未交付**字节：`len - off`。
4. `rx_next` 是 `rx_q` 队尾的流偏移；`off` 是该帧已交付的前缀长度。
5. 入口的跨接帧（`seq < rx_next < seq+len`）不再整帧丢弃，而是裁剪前缀后入队。

### 1.2 P20-1：`tbt_rx_pull_locked()` 三分支

- 文件/函数：`tbtun/tbt_rx.c:tbt_rx_pull_locked()`
- 当前锚点：函数 34-67；调用点 `tbt_rx_data()` 189（以 re-grep 为准）
- 依赖：只改 `tbt_rx.c`；等 perf 冻结
- 变更：函数签名加 `struct list_head *free`，把陈旧帧挂到调用者的释放链表，
  避免在 `st->rx_lock` 下 `kfree`

**前**（当前代码，省略末尾 hole 计时）：

```c
static void tbt_rx_pull_locked(struct tbt_stream *st)
{
    for (;;)
    {
        struct rb_node *node = rb_first_cached(&st->rx_tree);
        struct tbt_frame *fr;

        if (!node)
            break;
        fr = rb_entry(node, struct tbt_frame, rb);
        if (fr->hdr.seq != st->rx_next)
            break;
        rb_erase_cached(&fr->rb, &st->rx_tree);
        st->rx_ooo_bytes -= fr->len;
        st->rx_ooo_frames--;
        list_add_tail(&fr->list, &st->rx_q);
        st->rx_queued += fr->len;
        st->rx_next = fr->hdr.seq + fr->len;
    }
    /* ... hole_since_ns 计时保持不变 ... */
}
```

**后**：

```c
static void tbt_rx_pull_locked(struct tbt_stream *st, struct list_head *free)
{
    for (;;)
    {
        struct rb_node *node = rb_first_cached(&st->rx_tree);
        struct tbt_frame *fr;

        if (!node)
            break;
        fr = rb_entry(node, struct tbt_frame, rb);
        if (fr->hdr.seq > st->rx_next)
            break;

        if (fr->hdr.seq + fr->len <= st->rx_next)
        {
            /* 全旧：整个节点已经被别的副本交付 */
            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;
            st->rx_ooo_frames--;
            list_add_tail(&fr->list, free);
            continue;
        }

        if (fr->hdr.seq == st->rx_next)
        {
            /* 精确：正常前进 */
            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;
            st->rx_ooo_frames--;
            list_add_tail(&fr->list, &st->rx_q);
            st->rx_queued += fr->len;
            st->rx_next = fr->hdr.seq + fr->len;
            continue;
        }

        /* 跨接：seq < rx_next < seq+len，丢掉已交付前缀 */
        {
            u32 dup = st->rx_next - fr->hdr.seq;

            rb_erase_cached(&fr->rb, &st->rx_tree);
            st->rx_ooo_bytes -= fr->len;      /* 节点离开树，减整帧 */
            st->rx_ooo_frames--;
            fr->off += dup;                    /* off 记录已交付前缀 */
            fr->hdr.seq = st->rx_next;
            list_add_tail(&fr->list, &st->rx_q);
            st->rx_queued += fr->len - fr->off;
            st->rx_next = fr->hdr.seq + fr->len - fr->off;
        }
    }
    /* ... hole_since_ns 计时保持不变 ... */
}
```

调用者 `tbt_rx_data()` 的配套改动：

```c
struct tbt_frame *fr;
struct tbt_frame *victim, *vtmp;
LIST_HEAD(free);
/* ... */
spin_lock_bh(&st->rx_lock);
/* ... 各分支 ... */
tbt_rx_pull_locked(st, &free);
/* ... */
spin_unlock_bh(&st->rx_lock);

list_for_each_entry_safe(victim, vtmp, &free, list)
{
    list_del(&victim->list);
    kfree(victim);
}
```

注意：`tbt_rx_pull_locked()` 只有这一个调用点；签名变更不影响其他文件。若不想改签名，
也可以直接在锁内 `kfree(fr)`（现有 `tbt_rx_free_queue()` 就在锁内 `kfree`），但会拉长持锁时间。

### 1.3 P20-2：入口 `tbt_rx_data()` 对 `seq < rx_next` 的裁剪

- 文件/函数：`tbtun/tbt_rx.c:tbt_rx_data()`
- 当前锚点：131-229；`seq < rx_next` 分支 176-183；`seq == rx_next` 分支 184-193
- 依赖：与 P20-1 同一个提交（共用 `free` 链表与 `off` 语义）

**前**：

```c
if (hdr->seq < st->rx_next)
{
    /* Already delivered (a retransmit that lost the race). */
    spin_unlock_bh(&st->rx_lock);
    kfree(fr);
    tbt_stream_put(st);
    return 0;
}
if (hdr->seq == st->rx_next)
{
    list_add_tail(&fr->list, &st->rx_q);
    st->rx_queued += len;
    st->rx_next = hdr->seq + len;
    tbt_rx_pull_locked(st);
    if (hdr->flags & TBT_FF_LOW_PRIO)
        sess->dup_used++;
}
```

**后**：

```c
if (hdr->seq < st->rx_next)
{
    u32 dup = st->rx_next - hdr->seq;

    if (hdr->seq + len <= st->rx_next)
    {
        /* 全旧：重传输给了已交付的副本 */
        spin_unlock_bh(&st->rx_lock);
        kfree(fr);
        tbt_stream_put(st);
        return 0;
    }

    /* 跨接：前缀 [seq, rx_next) 已由别的副本交付，只保留尾部 */
    fr->off = dup;
    fr->hdr.seq = st->rx_next;
    list_add_tail(&fr->list, &st->rx_q);
    st->rx_queued += len - dup;
    st->rx_next = fr->hdr.seq + (len - dup);
    tbt_rx_pull_locked(st, &free);
    if (hdr->flags & TBT_FF_LOW_PRIO)
        sess->dup_used++;
}
else if (hdr->seq == st->rx_next)
{
    list_add_tail(&fr->list, &st->rx_q);
    st->rx_queued += len;
    st->rx_next = hdr->seq + len;
    tbt_rx_pull_locked(st, &free);
    if (hdr->flags & TBT_FF_LOW_PRIO)
        sess->dup_used++;
}
```

关键：入口分支要**单独记账**，不能落到 `hdr->seq == rx_next` 分支的 `rx_queued += len` /
`rx_next = seq + len`，否则把已交付的 `dup` 再算一遍。`fr->hdr.seq` 改成裁剪后的值；
`hdr->seq` 保持原值，供后面的 `predict_seq` 判断使用（见 1.5）。

### 1.4 记账表

| 场景 | `rx_ooo_bytes` | `rx_ooo_frames` | `rx_queued` | `rx_next` | `off` |
|---|---|---|---|---|---|
| 树·全旧 `seq+len<=rx_next` | `-=len` | `--` | 不变 | 不变 | 不变 |
| 树·精确 `seq==rx_next` | `-=len` | `--` | `+=len` | `+=len` | 不变（0） |
| 树·跨接 `seq<rx_next<seq+len` | `-=len` | `--` | `+=len-off` | `+=len-dup` | `+=dup` |
| 入口·全旧 | 不变（不在树） | 不变 | 不变 | 不变 | 不变 |
| 入口·跨接 | 不变（不在树） | 不变 | `+=len-dup` | `+=len-dup` | `=dup` |
| 入口·精确 | 不变 | 不变 | `+=len` | `+=len` | 0 |

`tbt_rx_free_queue()` 仍然在释放整棵树后把 `rx_ooo_bytes/rx_ooo_frames` 清零；无需改动。

### 1.5 与 dup / PREDICT 的交互

- **dup 精确副本**：`tbt_rx_tree_insert_locked()` 仍对完全相同的 `seq` 返回 false，直接丢弃。
- **dup 陈旧副本**：入口全旧分支丢弃；若已在树中，pull 的全旧分支释放。不会重复交付。
- **dup 跨接副本**：入口/pull 裁剪后只交付尾部；`dup_used` 只在 LOW_PRIO 帧**入队**时递增
  （精确或跨接），全旧不递增，保持现有语义。
- **`rx_ooo_bytes` 双重计数**：两个不同 `seq` 的重叠节点会各计一次 `len`，所以上限仍是
  “存储字节”而不是“唯一字节”。这是有意的内存界；敌意对端会更快触发 `rx_limit`，可接受。
- **PREDICT 的 `tbt_rx_tree_has_locked()`**：修复后树中不再有已交付范围，`tree_has` 的语义变干净；
  `tbt_rx_frame()` 的 `hdr->seq < st->rx_next` 仍覆盖已交付范围。
- **`st->predict_seq` 清除**：现有 `if (st->predict_seq == hdr->seq) st->predict_seq = 0;`
  用原始 `hdr->seq`，入口裁剪后仍然正确（占位符在原始 seq 上，真字节已到达）。
  可选加固：在 pull 的全旧/跨接分支里，如果 `predict_seq` 落在
  `[fr->hdr.seq, rx_next)`，也清零；PREDICT 当前是惰性的（review 13），这不影响验收。
- **`predict_used/predict_cancelled`**：计数路径不变；树中陈旧节点消失后，重复的 PREDICT
  会走 `hdr->seq < rx_next` 的 `predict_cancelled`，与“已交付”语义一致。

### 1.6 验收：`far.overlap`

构造（在 `adversarial/adv_probe.c` 的 far raw 注入器里，HELLO 之后、同一载体）：

1. `DATA seq=8192 len=8192` —— 先到，进树；`rx_next=0`。
2. `DATA seq=0 len=12288` —— 直接交付 `[0,12288)`，`rx_next=12288`；树节点 `[8192,16384)`
   变成跨接。
3. `DATA seq=12288 len=4096` —— 与裁剪后的尾部重复；入口全旧丢弃。

期望：

- 应用收到完整的 16384 字节，顺序正确；
- `stats`：`ooo_bytes=0`、`ooo_frames=0`、`reorder_overflow` 不增、`holes_unrecoverable` 不增；
- 当前（未修复）：`ooo_bytes` 停在 8192（树节点整帧计入），`ooo_frames=1`，重复多次后
  `reorder_overflow` 增长、流被 `TBT_ST_REORDER` 杀死。

命令：

```sh
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
# 期望 far.overlap PASS；运行中可 cat /proc/net/tbtun/stats | grep -E 'ooo_bytes|reorder_overflow'
```

`far.overlap` 是测试设施，独立于 perf；补丁前它会失败，这是预期的红。

### 1.7 风险

- `off` 的语义从“交付进度”扩展为“裁剪前缀 + 交付进度”；`tbt_drain_work()` 的
  `left = len - off`、`data + off` 已经兼容，但任何新的读者必须用 `seq + len - off` 当流末。
- 裁剪只应发生在敌意/异常重叠；合法发送端的 chunk 边界固定、重传 len 不变，不会进入跨接分支。
- `kfree` 延迟到锁外需要保证所有早退路径都释放 `free` 链表；本补丁只在 pull 之后统一释放，
  早退路径（入口全旧、insert 重复）在 pull 之前，`free` 为空。
- `rx_ooo_bytes` 的双重计数没有消除；这是内存界，不是正确性问题。

---

## 2. review 22：`-EAGAIN` 时 pinned 控制帧被释放

### 2.1 P22-1：瞬态错误时 pinned 帧重新入队

- 文件/函数：`tbtun/tbt_tx.c:tbt_tx_pump()`，batch 写入循环
- 当前锚点：`if (err == -EAGAIN || err == -ENOMEM)` 约 1483；pinned `kfree` 约 1493
- 依赖：只改 `tbt_tx.c`；等 perf 冻结
- 注意：当前代码已把 `-ENOMEM`（HTTP header kmalloc 失败）与 `-EAGAIN` 合并为瞬态错误；
  两者都应重新入队，不能释放 pinned 帧

**前**：

```c
if (err == -EAGAIN || err == -ENOMEM)
{
    if (fr->ret)
    {
        spin_lock(&sess->tx_lock);
        fr->ret->reserve_mask &= ~BIT(c->index);
        spin_unlock(&sess->tx_lock);
    }
    if (fr->pinned)
    {
        kfree(fr);
        continue;
    }
    fr->path = 0xff;
    fr->avoid_mask = BIT(c->index);
    sess->dbg_pump_hiwat++;
    tbt_tx_enqueue(sess, fr, false);
    continue;
}
```

**后**：

```c
if (err == -EAGAIN || err == -ENOMEM)
{
    if (fr->ret)
    {
        spin_lock(&sess->tx_lock);
        fr->ret->reserve_mask &= ~BIT(c->index);
        spin_unlock(&sess->tx_lock);
    }
    if (fr->pinned)
    {
        /* 控制帧不能丢：HELLO/OPEN 丢了载体永远不 up，FIN/RST 丢了
         * 对端永远看不到关闭。队头重排，2 ms w_pump 兜底。 */
        tbt_tx_enqueue(sess, fr, true);
        if (c->sk && c->sk->sk_socket)
            set_bit(SOCK_NOSPACE, &c->sk->sk_socket->flags);
        mod_delayed_work(system_unbound_wq, &c->w_pump,
                         msecs_to_jiffies(2));
        sess->dbg_pump_hiwat++;
        continue;
    }
    fr->path = 0xff;
    fr->avoid_mask = BIT(c->index);
    sess->dbg_pump_hiwat++;
    tbt_tx_enqueue(sess, fr, false);
    continue;
}
```

锁/上下文：`tbt_tx_enqueue()` 内部拿 `sess->tx_lock`；此时在 batch 写循环里、锁外，安全。
`mod_delayed_work()` 在进程上下文、`pump_lock` 下调用，安全（`w_pump` 会等 `pump_lock`）。
`fr->path/gen` 不变；pinned 帧的 `ret` 为 NULL，预留释放块是 no-op。

### 2.2 P22-2：FIN/RST 跨载体替换保留

- 文件/函数：`tbtun/tbt_tx.c:tbt_tx_pump()`，pinned 选择分支
- 当前锚点：`if (fr->pinned)` 约 1341；`!c`/`stopping`/`gen` 检查约 1343-1355
- 依赖：只改 `tbt_tx.c`；等 perf 冻结
- 语义：HELLO/OPEN/RETIRE 属于某个 generation，替换后应丢弃；FIN/RST 属于**流**，
  只要同 index 上有新载体，就应该在新载体上重发

**前**：

```c
if (fr->pinned)
{
    if (fr->path >= sess->n_car)
        goto drop;
    c = READ_ONCE(sess->car[fr->path]);
    if (!c)
        goto drop;
    if (atomic_read(&c->stopping))
        goto drop;
    /* A pinned frame from a carrier that has since been replaced
     * must never be written on the replacement. */
    if (fr->gen && fr->gen != c->gen)
        goto drop;
    if (type == TBT_F_HELLO)
    {
        ...
    }
    else if (type == TBT_F_OPEN)
    {
        ...
    }
    else if (!atomic_read(&c->up))
    {
        continue;
    }
}
```

**后**：

```c
if (fr->pinned)
{
    bool stream_ctl = (type == TBT_F_FIN || type == TBT_F_RST);

    if (fr->path >= sess->n_car)
        goto drop;
    c = READ_ONCE(sess->car[fr->path]);
    if (!c)
    {
        /* 槽位正在替换：FIN/RST 等新载体，其它 pinned 帧丢弃 */
        if (stream_ctl)
            continue;
        goto drop;
    }
    if (atomic_read(&c->stopping))
    {
        if (stream_ctl)
            continue;
        goto drop;
    }
    /* HELLO/OPEN/RETIRE 绑定 generation；FIN/RST 是流级的，允许跨代 */
    if (fr->gen && fr->gen != c->gen && !stream_ctl)
        goto drop;
    if (type == TBT_F_HELLO)
    {
        ...
    }
    else if (type == TBT_F_OPEN)
    {
        ...
    }
    else if (!atomic_read(&c->up))
    {
        continue;
    }
}
```

`continue` 在 `list_for_each_entry_safe` 里只把帧留在 `tx_q`，不会释放；下一次 pump 再看。
新载体的 HELLO/OPEN 写入会 `tbt_tx_wake()`（`tbt_tx_control_pinned()` 尾部），
`tbt_car_poll_work()` 的 50 ms 也会 `schedule_work(&c->w_tx)`，所以 FIN/RST 不会无限等。

### 2.3 P22-3：FIN/RST 按 index 排队 + 去重

- 文件/函数：`tbtun/tbt_tx.c:tbt_tx_fin()`（183-206）、`tbt_tx_rst()`（208-225）；
  `struct tbt_stream` 增加两个标志
- 依赖：`tbt_tx.c` + `tbtun.h`；等 perf，字段改动与其他人协调
- 目的：FIN/RST 在“调用时槽位为空/未 up”时也不能丢；每个 index 一份，等该 index 的载体

**前**（`tbt_tx_fin` 的核心循环）：

```c
for (i = 0; i < sess->n_car; i++)
{
    struct tbt_carrier *c = READ_ONCE(sess->car[i]);

    if (!c || !atomic_read(&c->up) || atomic_read(&c->stopping))
        continue;
    err = tbt_tx_control_pinned(sess, (u8)i, TBT_F_FIN, st->sid, seq, NULL, 0,
                                false, gfp);
    if (err)
        break;
}
```

**后**：

```c
/* 原函数头的 int i, err = 0; 改成： */
int i;
bool queued = false;

if (test_and_set_bit(0, &st->tx_fin_sent))
    return 0;

for (i = 0; i < sess->n_car; i++)
{
    /* 不要求 c/up：帧按 index 排队，pinned 选择会等该 index 的载体。
     * tbt_tx_control_pinned() 在槽位为空时把 gen 置 0。 */
    if (!tbt_tx_control_pinned(sess, (u8)i, TBT_F_FIN, st->sid, seq, NULL, 0,
                               false, gfp))
        queued = true;
    else
        sess->dbg_alloc_fail++;
}
if (!queued)
    clear_bit(0, &st->tx_fin_sent);   /* 分配全失败：允许后续重试 */
tbt_tx_wake(sess);
return queued ? 0 : -ENOMEM;
```

`tbt_tx_rst()` 同样处理，标志换成 `tx_rst_sent`。

`struct tbt_stream`（`tbtun.h`，`tx_open_sent` 附近）：

```c
unsigned long tx_fin_sent;   /* test_and_set_bit; bool 不能用于 bitop */
unsigned long tx_rst_sent;
```

`tbt_ctrl_work()`（`tbt_sock.c:277-296`）可能因多次状态变化重复调 `tbt_tx_fin()`；
`tbt_kill_work()` 与 `tbt_ctrl_work()` 都可能调 `tbt_tx_rst()`。`test_and_set_bit` 让第二次调用
直接返回；`tbt_tx_fin()` 的 `seq` 仍然在 `tx_lock` 下读取，语义不变。

注意：如果 `tbt_tx_fin()` 在 `tbt_tx_rst()` 之后才被调用（流已 DEAD），仍会排队 FIN；
接收端对已死流忽略 FIN，无害。若在意，可在入口检查 `st->state == TBT_ST_DEAD` 直接返回。

### 2.4 与 `tbt_tx_stall_check()` 的交互

- FIN/RST 不在 `st->retain` 里，不推进 `st->tx_seq`，也不影响 `sess->tx_ack`。
- 如果应用在 FIN 之前的数据已经全部 acked（`tx_ack == tx_seq`），`tbt_tx_stall_check()`
  的 `ack < READ_ONCE(st->tx_seq)` 为假，不会杀流；FIN 只需等载体可写。
- 如果还有未 acked 数据，`hole_budget_ms` 到期后 `tbt_tx_stall_check()` 会杀流，
  `tbt_kill_work()` 发 RST；这是正确的终态，pending FIN 变为无关。**不要**为了让 FIN
  通过而延长 `hole_budget_ms`，也不要在 stall check 里排除控制帧。
- `tbt_tx_stall_check()` 在 `peer_paused`/`st->paused` 时直接返回；FIN 的载体背压不是
  数据 stall，不改变该逻辑。
- 测试要先把应用数据 acked（`tx_ack == tx_seq`）再触发 FIN，避免把“数据空洞重置”误判为
  “FIN 丢失”。

### 2.5 验收：`near.fin_eagain`

两个变体，按可控性排序：

**变体 A（确定性，路径死亡重挂）**：

1. 近端 `n=1 m=1`（或 `m=0` 让替换重拨），raw；假 far 接受 `n+m` 条载体并完成 HELLO/OPEN。
   `m=1` 时 per-index FIN 会同时排在 spare 的 index 上，spare 本身是 `up` 的，所以即使
   主载体立刻死掉，spare 上那份 FIN 也能到达。
2. 应用写 64 KiB，等 `stats` 的 `tx_ack == tx_seq`（或 200 ms）。
3. 应用 `shutdown(SHUT_WR)`；等 100 ms 让 `tbt_ctrl_work()` 把 FIN 排到每个 index。
4. 假 far 关闭该载体 socket（或 `echo 'carrier 0 down' > /proc/net/tbtun/peers`）；
   近端 `tbt_carrier_lost()` 标记 stopping 并替换/启用备用。
5. 新载体（或 spare）加入/激活后，pump 写出该 index 的 FIN；假 far 在任意一条连接上读到
   `TBT_F_FIN` 即可。
6. 断言：`fin_seen=1`、应用 `read()` 返回 0（EOF）、`holes_unrecoverable` 不增、无 RST。

**变体 B（精确命中 -EAGAIN）**：

`sk_stream_memory_free()` 为真但 `tcp_sendmsg_locked()` 返回 `-EAGAIN` 的窗口很窄，
纯用户态很难稳定触发。建议加一个 debug-only 注入：

```c
/* tbt_carrier_xmit() 入口，仅测试用；module_param(debug_xmit_eagain, uint, 0644) */
if (unlikely(debug_xmit_eagain && fr->pinned &&
             (fr->hdr.type == TBT_F_FIN || fr->hdr.type == TBT_F_RST))) {
    debug_xmit_eagain--;
    return -EAGAIN;
}
```

测试置 `debug_xmit_eagain=1`，应用 `shutdown(SHUT_WR)`，等 100 ms，确认 FIN 仍在队列
（`stats` 的 `txq` 非零、`tx_frames` 未增），下一次 pump 写出；假 far 收到 FIN。
这个 hook 只用于验收，不改变正常路径；如果不想加模块参数，变体 A 已覆盖“路径死亡重挂”，
变体 B 可作为后续。

命令：

```sh
TBT_ADV_GROUPS=near adversarial/run.sh --no-build
# 期望 near.fin_eagain PASS；detail: fin_seen=1 eof=1 reset=0
# 运行中：grep -E 'txq=|tx_ack=|tx_seq=' /proc/net/tbtun/stats
```

### 2.6 风险

- 重新入队会让一个永远满的载体在队头反复重试；每次 pump 最多 4 个 pass，`w_pump` 2 ms，
  不会忙等；载体真死时 `carrier_stall_ms` 后替换。
- 按 index 排队会让每条载体都收到 FIN/RST（最多 16 份）；接收端 `rx_fin_seen`/
  `tbt_stream_kill` 幂等，带宽代价可忽略（32 字节/份）。
- `tx_fin_sent`/`tx_rst_sent` 是“已排队”而不是“已写出”；如果队列在会话销毁前没写出去，
  标志不会阻止销毁（`tbt_tx_free_queue` 释放所有帧）。这是可接受的终态。
- 变体 B 的 debug hook 是新模块参数；发布前可保留（默认 0，无行为影响），或只放在测试分支。

---

## 3. review 21：per-carrier 分片与 max-CAS

### 3.1 最小落地范围（两个提交）

**提交 1（正确性优先，必须做）**：单调高水位的 max-CAS。

- `sess->tx_ack`：`u64` → `atomic64_t`
- `pc->uack`：`u64` → `atomic64_t`
- `pc->recv_cnt`：`u32` → `atomic_t`
- `sess->join_retries`：`u8` → `atomic_t`
- `c->lost_counted`：`bool` → `unsigned long`

这五个值不是“统计”，是决策输入：`tx_ack` 回退会破坏保留环 GC/发送窗口，`uack` 回退会让
`tbt_retain_received()` 误判，`recv_cnt` 回退会污染空洞门控，`join_retries` 丢更新会延迟
fail-fast，`lost_counted` 竞态会重复计数。先修它们，再谈分片。

**提交 2（可观测性，按需）**：per-carrier `u64_stats_t` 分片。

先迁**每帧/每记录**的热计数：

| 旧字段 | 新分片 | 写者 |
|---|---|---|
| `c->rx_frames` | `c->stats.rx_frames` | 该载体 `w_rx` |
| `c->rx_bytes` | `c->stats.rx_bytes` | 该载体 `w_rx` |
| `sess->rx_records` | `c->stats.rx_records` | 该载体 `w_rx`（raw parser） |
| `sess->fbk_rx` | `c->stats.fbk_rx` | 该载体 `w_rx` |
| `sess->rx_parse_proto` | `c->stats.parse_proto` | 该载体 `w_rx` |
| `c->tx_frames` | `c->stats.tx_frames` | pump（`pump_lock`） |
| `c->tx_bytes` | `c->stats.tx_bytes` | pump |
| `sess->stats_tx` | `c->stats.tx_data_bytes` | pump（DATA 载荷） |
| `sess->carrier_bytes` | 求和 `c->stats.tx_bytes` | 不新增 |
| `sess->rx_data_frames` | 删除（当前只写不读） | 无 |

低频事件（`holes_*`、`carrier_lost/replaced/alloc_fail/connect_fail`、`join_*`、
`predict_*`、`dup_*`、`http_*`、`pad_tx`、`drip_tx`、`rx_ready_calls`、`tx_wake_calls`、
`poll_rescues`）**不在最小范围**；它们每事件一次，先用 `atomic64_t` 或保持现状，
等热路径分片稳定后再迁。

### 3.2 P21-1：max-CAS 的精确改动

- 文件/函数：`tbtun.h`（字段）、`tbt_tx.c:tbt_tx_ack()`、`tbt_rx.c:tbt_rx_fbk()`、
  `tbt_carrier.c:tbt_carrier_lost()`、`tbt_session.c:tbt_session_join()`
- 当前锚点：`tbt_tx_ack()` 1134；`tbt_rx_fbk()` 235；`tbt_carrier_lost()` 约 735-785；
  `tbt_session_join()` 的 `join_retries=0` 约 499；字段 `tbtun.h:359/367/487/493/503`
- 依赖：等 perf 冻结 `tbt_tx.c`/`tbt_rx.c`；类型改动必须与所有读点一次编译通过

辅助函数（放在 `tbtun.h`，`#include <linux/atomic.h>`）：

```c
static inline bool tbt_u64_max(atomic64_t *v, u64 x)
{
    u64 old = atomic64_read(v);

    while (x > old) {
        if (atomic64_try_cmpxchg(v, &old, x))
            return true;
    }
    return false;
}

static inline bool tbt_u32_max(atomic_t *v, u32 x)
{
    u32 old = atomic_read(v);

    while (x > old) {
        if (atomic_try_cmpxchg(v, &old, x))
            return true;
    }
    return false;
}
```

`tbt_tx_ack()` **前**：

```c
if (uack <= READ_ONCE(sess->tx_ack))
{
    tbt_stream_put(st);
    return;
}
WRITE_ONCE(sess->tx_ack, uack);
sess->last_ack_ns = tbt_now_ns();
```

**后**：

```c
if (!tbt_u64_max(&sess->tx_ack, uack))
{
    tbt_stream_put(st);
    return;
}
WRITE_ONCE(sess->last_ack_ns, tbt_now_ns());
```

`tbt_rx_fbk()` **前**：

```c
if (uack > READ_ONCE(pc->uack))
{
    WRITE_ONCE(pc->uack, uack);
    WRITE_ONCE(pc->uack_ns, ktime_get_ns());
}
...
WRITE_ONCE(pc->recv_cnt, cnt);
```

**后**：

```c
if (tbt_u64_max(&pc->uack, uack))
    WRITE_ONCE(pc->uack_ns, ktime_get_ns());
...
tbt_u32_max(&pc->recv_cnt, cnt);
```

`tbt_carrier_lost()` **前**：

```c
if (!c->lost_counted)
{
    c->lost_counted = true;
    sess->carrier_lost++;
}
...
if (sess->join_retries < U8_MAX)
    sess->join_retries++;
if (sess->join_retries > 20)
{
    ...
}
```

**后**：

```c
if (!test_and_set_bit(0, &c->lost_counted))
    sess->carrier_lost++;
...
if (atomic_inc_return(&sess->join_retries) > 20)
{
    ...
}
```

`tbt_session_join()` 的 `sess->join_retries = 0;` → `atomic_set(&sess->join_retries, 0);`。

读点替换（`READ_ONCE(sess->tx_ack)` → `atomic64_read(&sess->tx_ack)`）：

- `tbt_tx.c:262, 361, 426, 446, 562, 828, 876, 1098, 1147, 1190, 1415`
- `tbt_tx_stall_check()` 的 `start = sess->last_ack_ns` 不变（时间戳，不是高水位）

读点替换（`READ_ONCE(c->uack)` → `atomic64_read(&c->uack)`）：
`tbt_tx.c:252, 636, 726, 930, 952`；`tbt_session.c:366`。
读点替换（`READ_ONCE(c->recv_cnt)` → `atomic_read(&c->recv_cnt)`）：
`tbt_tx.c:939-940`；`tbt_session.c:394`。

内存序：这些值都是单调高水位，读写只需要 relaxed；CAS 保证不回退。`last_ack_ns` 是
“最近一次推进”的时间戳，CAS 成功后 `WRITE_ONCE` 即可；两个 CPU 同时推进不同值时，
后写的时间戳更晚，单调性足够。

热路径：`tbt_tx_ack()` 每收到一帧调用一次；绝大多数调用是 `atomic64_read` + 比较，
只有推进时才 CAS。`uack/recv_cnt` 每 FBK（10 ms）一次 CAS。可接受。

### 3.3 P21-2：per-carrier `u64_stats_t` 分片

`struct tbt_carrier` 内新增（`tbtun.h`）：

```c
struct tbt_car_stats {
    struct u64_stats_sync rx_sync;   /* 写者：该载体的 w_rx */
    u64_stats_t rx_frames, rx_bytes, rx_records, fbk_rx, parse_proto;
    struct u64_stats_sync tx_sync;   /* 写者：pump（pump_lock） */
    u64_stats_t tx_frames, tx_bytes, tx_data_bytes;
};
```

**必须用两个 `u64_stats_sync`**：`rx_sync` 的写者是 `w_rx`，`tx_sync` 的写者是 pump；
共用一个 sync 就是两个写者，seqcount 会坏。`u64_stats_t` 本身不带 seqcount；
读取时必须用 `u64_stats_fetch_begin()/fetch_retry()` 包住同一 sync 下的一组字段。

写者改动：

- `tbt_rx_data()`（`tbt_rx.c:146-147`）：
  ```c
  u64_stats_update_begin(&c->stats.rx_sync);
  u64_stats_inc(&c->stats.rx_frames);
  u64_stats_add(&c->stats.rx_bytes, len);
  u64_stats_update_end(&c->stats.rx_sync);
  ```
- `tbt_rx_parse_raw()`（`tbt_rx.c:714`）：`u64_stats_inc(&c->stats.rx_records)`。
- `tbt_rx_fbk()`（`tbt_rx.c:285`）：`u64_stats_inc(&c->stats.fbk_rx)`。
- `tbt_carrier.c:371` 与 `tbt_rx.c:710` 的 `rx_parse_proto++`：
  `u64_stats_inc(&c->stats.parse_proto)`。
- `tbt_carrier_xmit()` 的成功路径（`tbt_carrier.c:585-586, 608-613, 686-691`）：
  ```c
  u64_stats_update_begin(&c->stats.tx_sync);
  u64_stats_inc(&c->stats.tx_frames);
  u64_stats_add(&c->stats.tx_bytes, n_or_total);
  if (fr->hdr.type == TBT_F_DATA)
      u64_stats_add(&c->stats.tx_data_bytes, fr->len);
  u64_stats_update_end(&c->stats.tx_sync);
  ```
  `tbt_tx.c:1596` 的 `sess->stats_tx += fr->len;` 删除（改由 `tx_data_bytes` 求和）。
  `sess->carrier_bytes += ...` 三处删除（改由 `tx_bytes` 求和）。

读取点（`tbt_procfs.c:tbt_show_stats()`、`tbt_session.c:tbt_peers_show()`、
`tbt_tx.c:tbt_tx_fbk()`）：

```c
unsigned int start;
u64 rx_frames = 0, rx_records = 0, fbk_rx = 0, parse_proto = 0;
u64 tx_frames = 0, tx_bytes = 0, tx_data = 0;

for (k = 0; k < s->n_car; k++) {
    struct tbt_carrier *c = READ_ONCE(s->car[k]);
    if (!c) continue;
    do {
        start = u64_stats_fetch_begin(&c->stats.rx_sync);
        rx_frames  += u64_stats_read(&c->stats.rx_frames);
        rx_records += u64_stats_read(&c->stats.rx_records);
        fbk_rx     += u64_stats_read(&c->stats.fbk_rx);
        parse_proto+= u64_stats_read(&c->stats.parse_proto);
    } while (u64_stats_fetch_retry(&c->stats.rx_sync, start));
    do {
        start = u64_stats_fetch_begin(&c->stats.tx_sync);
        tx_frames += u64_stats_read(&c->stats.tx_frames);
        tx_bytes  += u64_stats_read(&c->stats.tx_bytes);
        tx_data   += u64_stats_read(&c->stats.tx_data_bytes);
    } while (u64_stats_fetch_retry(&c->stats.tx_sync, start));
}
```

`tbt_show_stats()` 的 `c3` 块已经遍历 `s->car[k]` 统计 `up`，把上面的求和放进同一个循环；
`stats_tx`/`carrier_bytes`/`rx_records`/`fbk_rx`/`rx_parse_proto` 的 `seq_printf` 参数改成局部和。
`tbt_peers_show()` 的 `frames`/`bytes` 改成 `u64_stats_read(&c->stats.tx_frames/tx_bytes)`。
但 `u64_stats_read()` 在 32 位上是普通读，**单字段也要**用 fetch/retry 包住：

```c
unsigned int start;
u32 cnt;
do {
    start = u64_stats_fetch_begin(&c->stats.rx_sync);
    cnt = (u32)u64_stats_read(&c->stats.rx_frames);
} while (u64_stats_fetch_retry(&c->stats.rx_sync, start));
tbt_put_le32(e + TBT_FBK_RECVCNT, cnt);
```

`tbt_peers_show()` 的 `frames`/`bytes` 同理，把两个字段放在同一个 tx_sync 的 fetch/retry 里，
避免 32 位撕裂。`tbt_tx_fbk()` 的 `tbt_put_le32(e + TBT_FBK_RECVCNT, ...)` 改成上面的 `cnt`；
FBK 的 `u32` 截断语义不变。

删除/保留：

- `sess->rx_data_frames` 只写不读，直接删除（`tbt_rx.c:150` 与 `tbtun.h:438`）。
- `sess->rx_records`、`sess->fbk_rx`、`sess->stats_tx`、`sess->carrier_bytes`、
  `sess->rx_parse_proto` 的字段删除，所有读写点改为分片/求和。
- `sess->fbk_frames` 保留，但改 `atomic_t`（`atomic_inc_return` + `atomic_xchg`），
  它不是分片，是会话级 cadence。
- `sess->rx_delivered_bytes` 是单流 drain 写的，保留。
- `c->rx_frames`/`rx_bytes`/`tx_frames`/`tx_bytes` 旧字段删除，避免两份真相。

### 3.4 暂不做的

- 低频事件计数的分片/原子化：`holes_*`、`carrier_*`、`join_*`、`predict_*`、`dup_*`、
  `http_*`、`pad_tx`、`drip_tx`、`rx_ready_calls`、`tx_wake_calls`、`poll_rescues`。
  它们每事件一次，丢更新只影响诊断；等热路径稳定后再统一迁 `atomic64_t`。
- `c->loss_ppm`/`rtt_us` 的 EWMA 竞态：良性，保留。
- `sess->peer_paused` 的 FBK 乱序覆盖：加 `fbk_gen` 是后续加固，不在本次。

### 3.5 验收

```sh
make check                         # 编译 + host 测试（无内核）
bash review/lint.sh                # 新增 check_monotone_ack（见 §4.3）

# QEMU：n=4，双向 40 MiB，结束后采样
tests/qemu/run.sh --probes C,E --timeout 300
# 断言：
#   sum(peers 各行 tx_frames) == stats 的 tx 帧总和
#   sum(peers 各行 tx_bytes)  == stats 的 wire 总和
#   stats 的 rx_records 在运行中单调不减
#   tx_ack/recv_cnt 在连续采样中不回退
```

若内核配置允许，跑一次 KCSAN 的 n=4 压力；当前这些字段会报 data race，P21-1 后应静默。

### 3.6 风险

- `atomic64_t` 改字段类型会让所有 `READ_ONCE` 读点编译失败；必须一次改完，不能分文件半合并。
- `u64_stats_t` 的写者唯一性是硬约束；`rx_sync`/`tx_sync` 必须分开，且不能有第三个写者
  （例如未来在 softirq 里更新 tx 计数）。
- 读取点的 `fetch_begin/retry` 循环不能跨 `seq_printf` 或睡眠；先在循环里求和，再打印。
- 删除 `sess->carrier_bytes` 等字段会改变 `stats` 的聚合来源；键名保持不变，值应相等。
- `tbt_procfs.c` 是 eng-contract 的文件；P21-2 的读取点改动必须与其协调，避免同时改同一段。

---

## 4. 独立项（不依赖 perf/contract，可先做）

### 4.1 `far.overlap` 测试

- 文件：`adversarial/adv_probe.c`（far raw 注入器）、`adversarial/init`（phase_far 已有 far 组）
- 不依赖源码补丁；补丁前红、补丁后绿
- 序列与断言见 §1.6

### 4.2 `near.fin_eagain` 测试

- 文件：`adversarial/adv_probe.c`（fakefar 的暂停/恢复读、FIN 解析）、`adversarial/init`
- 变体 A 不依赖 debug hook；变体 B 需要 §2.5 的 `debug_xmit_eagain`（测试设施）
- 序列与断言见 §2.5

### 4.3 `review/lint.sh` 新检查

```sh
check_monotone_ack() {
    # tbt_tx_ack() 不得保留 check-then-write；必须走 tbt_u64_max/try_cmpxchg
    body=$(func_body "$1/tbtun/tbt_tx.c" tbt_tx_ack)
    printf '%s\n' "$body" | grep -qE 'tbt_u64_max|try_cmpxchg' && return 0
    printf 'tbt_tx_ack still uses check-then-write'
    return 1
}
```

以及 `check_no_bare_frame_free`：`tbt_tx.c`/`tbt_rx.c` 里不再出现裸 `kfree(fr)`/`kfree(r)`
（配合第 1 节的统一 free helper；如果 0.0.1 不做专用 cache，这条可跳过）。

### 4.4 文档

- `DESIGN.md`：review 20 的“重叠帧裁剪”、review 22 的“FIN/RST 跨代保留”、review 21 的
  “每载体分片 + 单调高水位 CAS”。
- `README*`：`stats` 新增字段（如果 P21-2 落地）与 `peers` 语义不变。

---

## 5. 落地前重新定位（perf 正在改 tx/rx）

```sh
grep -n "static void tbt_rx_pull_locked\|hdr->seq < st->rx_next\|hdr->seq == st->rx_next" tbtun/tbt_rx.c
grep -n "if (fr->pinned)\|if (err == -EAGAIN\|if (fr->gen && fr->gen != c->gen)" tbtun/tbt_tx.c
grep -n "int tbt_tx_fin\|int tbt_tx_rst\|static void tbt_tx_stall_check\|void tbt_tx_ack" tbtun/tbt_tx.c
grep -n "u64 tx_ack\|u64 uack\|u32 recv_cnt\|bool lost_counted\|u8 join_retries" tbtun/tbtun.h
grep -n "sess->rx_data_frames\|sess->rx_records\|sess->fbk_rx\|sess->stats_tx\|sess->carrier_bytes" tbtun/*.c
```

如果 perf 已经改了 `tbt_tx.c` 的 `-ENOMEM`/`-EAGAIN` 分支或 `data_frames` 位置（当前工作树
已把 `data_frames++` 移到成功路径，review 14 已修），以当前代码为准，不要按旧行号硬套。

## 6. 建议提交顺序

1. **P20**（tbt_rx.c，独立逻辑）→ `far.overlap` 绿。
2. **P22**（tbt_tx.c + tbtun.h，控制面）→ `near.fin_eagain` 绿。
3. **P21-1**（max-CAS，正确性）→ `make check` + n=4 单调性。
4. **P21-2**（分片，可观测性）→ `stats`/`peers` 求和一致。
5. 低频事件计数与文档，按余量。

每一步都单独编译、单独 QEMU 组；不要在一个提交里混 P20/P22/P21，否则回滚粒度太粗。
