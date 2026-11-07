# tcp-brutal 0.2.0 FEC 数据面修复设计（FEC-A..F + 解析缓冲不变量）

> 只读设计。行号取自当前工作树。两项 wire 版本升级必须同一版本落地：
> `TBT_CONV 0x0005 → 0x0006`、`TBT_FEC_CONV 0x1001 → 0x1002`。
> 目标：彻底、可验证、无静默数据损坏；0.1.0 与 0.2.0 硬不互操作。

---

## 0. 总览：工作模型、锁序、改动面

### 0.1 两个方向、两个 work

```
TX:  pump_lock → tx_lock → tx->lock / fec_out_lock
RX:  rx->lock（叶子锁，单独持有）
     sess->lock（stream 哈希）与 st->rx_lock 绝不在 rx->lock 内获取
```

| | `w_fec_tx`（现有） | `w_fec_rx`（新增） |
|---|---|---|
| 类型 | `struct delayed_work` | `struct delayed_work` |
| 状态 | `tx->lock`、`tx->codec`、`sess->fec_out` | `rx->lock`、`rx->codec`、`rx->blk[]` |
| 职责 | packetize / encode / 写 carrier | 解码 / 按 super-block 顺序交付 / 发 ACK |
| 共享 | 仅 `sess->lock`（查 stream）与 stream 对象 | 同左 |
| 取消点 | session destroy 末尾 | 同左，先置 `rx->stopping` |

`rx->lock` 内只允许：

1. 窗口检查 `next_super .. next_super+slots-1`；
2. 槽位查找 / `tbt_fec_blk_reset()`；
3. 一个 share 的 `memcpy`（≤ S）；
4. presence、`ndata/nparity`、`dlen`、`win_len/short_len`、`total_*` 更新；
5. 解码 claim / commit 的小数组快照；
6. 置 `rx->work_pending`。

锁外完成：header unpack / CRC / 解密、GF(2) 解码、`tbt_fec_stream()`、`tbt_rx_fec_deliver()`、ACK 组包与入队、`kvzalloc/kfree`、`schedule_delayed_work()`。

### 0.2 新增/修改的公共结构

`struct tbt_fec_rx`（`tbtun/tbtun.h:610`）新增：

```c
    /* w_fec_rx owns decode and ordered delivery; carriers only place shares. */
    bool work_pending;      /* a block may be decodable or deliverable */
    bool stopping;          /* session teardown: w_fec_rx returns */
    bool deliver_partial;   /* resume the walk at (deliver_p, deliver_g) */
    u16 deliver_p;          /* packet index inside next_super */
    u16 deliver_g;          /* group index inside that packet */
    u8 *rec;                /* m * s: recovered-slot scratch */
    u8 *zero;               /* s zero bytes: padding and absent parity */
    u8 dpresent_snap[TBT_FEC_MAX_SHARES];
    u8 ppresent_snap[TBT_FEC_MAX_SHARES];
    u64 stream_pkt_base;    /* absolute seq of the bound stream's first packet */
    u64 next_fin_sid;       /* FIN for the next stream, consumed on adoption */
    u64 next_fin_end;
    u64 next_fin_base;
    u64 next_fin_bytes;
    bool next_fin_valid;
```

`struct tbt_fec_blk`（`tbtun/tbtun.h:541`）新增：

```c
    u16 ndata;        /* present real DATA shares in [0, real) */
    u16 nparity;      /* present PARITY shares */
    u16 count_real;   /* real length the counts were computed for */
    bool decoding;    /* w_fec_rx owns this block */
    bool empty;       /* PARITY marker with win_len == 0 */
    bool win_len_set; /* a PARITY record fixed win_len/short_len */
```

`struct tbt_fec_gtx`（`tbtun/tbtun.h:564`）新增：

```c
    u64 blk_id;       /* block this group's nreal/last_len belong to */
```

`struct tbt_session`（`tbtun/tbtun.h:650`）新增：

```c
    struct delayed_work w_fec_rx; /* decode + ordered delivery + ACK */
```

`struct tbt_stream`（`tbtun/tbtun.h:390`）新增：

```c
    u64 fec_pkt_base;      /* first global FEC packet seq of this stream */
    bool fec_pkt_base_set;
    u64 fec_pkt_end;       /* actual end, frozen by tbt_fec_tx_finish() */
    bool fec_finished;     /* finish ran; block closed and advanced */
    u64 tx_fin_pkts;       /* frozen with tx_fin_seq */
    u64 tx_fin_pkt_base;
```

### 0.3 新增计数器（`tbtun/tbtun.h` 的 `struct tbt_session` 统计区，c9 输出）

`fec_decode_blocks`、`fec_decode_recovered`、`fec_decode_nomem`、
`fec_deliver_nomem`、`fec_deliver_bad`、`fec_dup_share`、`fec_late_share`、
`fec_pad_share`、`fec_epoch_conflict`、`fec_fin_bad`、`fec_ack_bad`、
`fec_tx_share_gap`、`fec_empty_block`。

### 0.4 全局落地顺序

| 步骤 | 文件 | 内容 | 可独立验证 |
|---|---|---|---|
| S1 | `tbt_uapi.h`, `tbt_proto.h/.c`, `tbt_fec.h/.c`, `host/test_fec.c`, `host/test_fuzz.c`, `host/test_http.c` | `TBT_CONV 0x0006`、FIN body 16、`TBT_FEC_CONV 0x1002`、marker/epoch 规则、`tbt_fec_ack_bytes` | `make host-test` |
| S2 | `tbtun.h`, `tbt_fec_data.c`（TX） | `gtx.blk_id`、块对齐跳转、`emit_empty`、`finish/fec_finished`、base/end | host 编译 + QEMU 短包 |
| S3 | `tbt_fec_data.c`（RX）、`tbt_rx.c`、`tbt_tx.c` | `w_fec_rx`、交付游标、`-ENOMEM`、FIN 绝对基数、pump 门 | QEMU 双 stream / 丢包 |
| S4 | `tbt_session.c`, `tbt_procfs.c` | init/cancel 顺序、`stopping`、计数器 | 卸载/重载无 warning |
| S5 | `DESIGN.md`, `CHANGELOG.md`, `交接.md` | wire 表、锁序、失败模式、迁移说明 | 文档评审 |

**落地后必须成立的不变量**

1. `rx->lock` 内不睡眠、不分配、不取其他锁、不交付。
2. 只有 `w_fec_rx` 推进 `next_super/deliver_bytes/next_deliver/ack_deliver`。
3. 已 `ready` 的块不可变；present 槽位不可变；迟到/重复 share 丢弃并计数。
4. 每个 stream 的 `pkt_base` 按 `groups*n` 对齐；FIN 的 `pkt_end` 是实际末尾。
5. `PARITY win_len=0` 只用于空块标记；`win_len>0` 的块所有 parity 的 `(win_len, epoch)` 一致。
6. 任何 wire 版本不匹配都在首个 HELLO 失败并关闭 carrier，绝不静默解析。

---

## 1. FEC-A：decode 移出 `rx->lock`

### 1.1 现状（文件:行）

- `tbt_fec_rx_record()`：`tbtun/tbt_fec_data.c:1052`；`spin_lock_bh(&rx->lock)` 在 `:1197`。
- 锁内调用链：
  - `tbt_fec_blk_decode()` `:772`（调用点 `:1250`）
  - `tbt_fec_rx_retry_all()` `:1032`（调用点 `:1249`）
  - `tbt_fec_deliver_ready()` `:903`（调用点 `:1253`）
- `tbt_fec_blk_decode()` → `tbt_fec_rlnc_decode()`（`tbtun/tbt_fec.c:1013`）→
  `tbt_fec_rlnc_bind()`（`:781`）→ `tbt_fec_alloc()`（`:28`）=
  `kvzalloc(GFP_KERNEL)`。**首次解码在自旋锁内做可睡眠分配**。
- `tbt_fec_rlnc_recover()`（`:887`）做 GF(2) 消元：rhs 每行 S 字节，
  `rank × parity_count × S` 次 XOR；单组 `n=192,m=64` 时可达数十 MB，
  全部持 session 级 `rx->lock`，所有 carrier 的 `w_rx` 被阻塞。
- `tbt_fec_deliver_ready()` 内调用 `tbt_fec_stream()`（`tbt_fec_data.c:75`，
  取 `sess->lock`）与 `tbt_rx_fec_deliver()`（`tbtun/tbt_rx.c:315`，取
  `st->rx_lock`、`kmalloc(GFP_ATOMIC)`），即
  `rx->lock → sess->lock → st->rx_lock` 的嵌套。
- 交付推进点：`tbt_fec_data.c:1014-1023`。

### 1.2 设计

#### 1.2.1 新签名

```c
void tbt_fec_rx_work(struct work_struct *w);

/* One pass over groups*slots. Called only by w_fec_rx. */
static void tbt_fec_rx_pass(struct tbt_session *sess);

/* Claim one block under rx->lock, decode outside it, commit under it. */
static bool tbt_fec_rx_decode_one(struct tbt_session *sess, u16 group, u16 slot);

/* Decode one block outside rx->lock. Present slots point into b->data;
 * missing slots point into rx->rec; padding points at rx->zero.
 * Returns 0 when ready, -EAGAIN when more shares are needed. */
static int tbt_fec_blk_decode(struct tbt_session *sess, u16 group,
                              struct tbt_fec_blk *b);

static void tbt_fec_deliver_ready(struct tbt_session *sess);

/* Lazy recount: ndata is the number of present shares in [0, real). */
static void tbt_fec_blk_counts(const struct tbt_fec_rx *rx, u16 group,
                               struct tbt_fec_blk *b);
```

#### 1.2.2 `tbt_fec_rx_record()` 锁内只放置

```c
    spin_lock_bh(&rx->lock);
    /* 现有窗口检查、槽位查找/重置（:1197-1215） */
    if (b->ready) { atomic64_inc(&sess->fec_late_share); unlock; return 0; }
    if (b->empty) return -EPROTO;              /* marker fixed the block */

    if (h.type == TBT_FEC_DATA) {
        if (b->dpresent[share]) { dup; }        /* present slots immutable */
        else { memcpy(...); b->dpresent[share] = 1; b->dlen[share] = ...; }
        if (h.flags & TBT_FEC_FF_LAST) { ...total...; }
    } else if (h.win_len == 0) {
        ...marker（§4）...
    } else {
        if (b->ppresent[idx]) { dup; }
        else { memcpy(...); b->ppresent[idx] = 1; b->nparity++; }
        ...cut agreement（§6）...
    }
    tbt_fec_blk_counts(rx, group, b);           /* O(n) only when real changes */
    if (b->empty || b->ndata >= real)
        b->ready = true;
    else if (b->nparity >= real - b->ndata)
        rx->work_pending = true;
    spin_unlock_bh(&rx->lock);
    if (wake)
        schedule_delayed_work(system_wq, &sess->w_fec_rx, 0);
    return 0;
```

`tbt_fec_blk_counts()`：

```c
static void tbt_fec_blk_counts(const struct tbt_fec_rx *rx, u16 group,
                               struct tbt_fec_blk *b)
{
    u16 real = tbt_fec_blk_real_len(rx, group, b);
    u16 i, nd = 0;

    if (b->count_real == real)
        return;
    for (i = 0; i < real; i++)
        nd = (u16)(nd + (b->dpresent[i] ? 1 : 0));
    b->ndata = nd;
    b->count_real = real;
}
```

`count_real` 在 `tbt_fec_blk_reset()` 中置 `0xffff`，保证首次扫描重算。

#### 1.2.3 `w_fec_rx`

```c
void tbt_fec_rx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx);
    struct tbt_fec_rx *rx = sess->fec_rx;
    bool again;
    int rounds = 0;

    if (!rx || READ_ONCE(sess->closing) || READ_ONCE(rx->stopping))
        return;
    do {
        again = false;
        tbt_fec_rx_pass(sess);        /* decode every candidate once */
        tbt_fec_deliver_ready(sess);  /* may re-arm on -ENOMEM（§2） */
        spin_lock_bh(&rx->lock);
        if (rx->work_pending) { rx->work_pending = false; again = true; }
        spin_unlock_bh(&rx->lock);
    } while (again && ++rounds < 4);
    if (again)
        mod_delayed_work(system_wq, &sess->w_fec_rx, 0);
}
```

`tbt_fec_rx_pass()` 对 `groups*slots`（≤ 1024）逐个检查候选：

```c
    if (!b->used || b->ready || b->decoding)
        continue;
    real = tbt_fec_blk_real_len(rx, group, b);
    tbt_fec_blk_counts(rx, group, b);
    if (b->empty || b->ndata >= real) {
        b->ready = true;             /* complete without codec */
        continue;
    }
    if (b->nparity < real - b->ndata)
        continue;                    /* not solvable yet */
    tbt_fec_rx_decode_one(sess, group, slot);
```

#### 1.2.4 解码数据通路（关键正确性点）

`tbt_fec_rlnc_recover()`（`tbt_fec.c:887`）只写 `ctx->data[idx]`，`idx` 仅来自
`unknown[]`；已知槽只读。因此可把缺失槽指向私有 `rec` 缓冲：

```c
    /* under rx->lock: snapshot */
    snap.real = tbt_fec_blk_real_len(rx, group, b);
    memcpy(snap.dp, b->dpresent, rx->n);
    memcpy(snap.pp, b->ppresent, rx->m);
    for (i = snap.real; i < rx->n; i++)
        snap.dp[i] = 1;                 /* padding is known zero */
    b->decoding = true;
    /* unlock */

    for (i = 0, k = 0; i < snap.real; i++)
        data_ptrs[i] = snap.dp[i] ? b->data + (size_t)i * S
                                  : rx->rec + (size_t)(k++) * S;
    for (i = snap.real; i < rx->n; i++)
        data_ptrs[i] = rx->zero;
    for (j = 0; j < rx->m; j++)
        parity_ptrs[j] = snap.pp[j] ? b->parity + (size_t)j * S : rx->zero;

    rc = tbt_fec_rlnc_decode(ctx, data_ptrs, parity_ptrs,
                             snap.dp, snap.pp, rx->n);
    /* relock: commit only slots still missing */
    if (rc >= 0) {
        for (i = 0, k = 0; i < snap.real; i++) {
            if (snap.dp[i]) continue;
            if (!b->dpresent[i]) {
                memcpy(b->data + (size_t)i * S, rx->rec + (size_t)k * S, S);
                b->dpresent[i] = 1;
                b->ndata++;
            }
            k++;
        }
        b->ready = true;
    }
    b->decoding = false;
```

- 已到达的 share 不被覆盖：`snap.dp[i]` 为真或 carrier 已填 `b->dpresent[i]`
  时跳过。
- padding 不写 `b->data`，指向 `rx->zero`，消除“解码器与 carrier 同时写槽位”
  的竞争。
- `b->ready` 的块此后不可变：重复 share 丢弃、迟到 share 丢弃；槽位重置只可能
  在 `next_super` 越过该块后发生（`:1198` 的窗口检查）。
- `tbt_fec_rlnc_bind()` 的首次 `kvzalloc` 现在发生在 `w_fec_rx`（进程上下文），
  不再触发 “sleeping function called from invalid context”。
- `rc < 0`（STUCK/NOMEM）时 `recover()` 不写任何 `rec` 槽；清 `decoding`，
  保持 `ready=false`，等新 share 或 `total_*` 变化触发下一轮。

#### 1.2.5 交付顺序

只有 `w_fec_rx` 写 `next_super/deliver_bytes/next_deliver/ack_deliver`。解码可以
乱序把块标 ready，但 `tbt_fec_deliver_ready()` 从 `rx->next_super` 开始，
`tbt_fec_super_ready()`（`:878`）不满足就停；`-ENOMEM` 时保留
`(deliver_p, deliver_g)` 游标，`next_super` 不前进。ACK 只在一个完整
super-block 交付后发送。

`tbt_fec_super_ready()` 在 `rx->lock` 内读 `used/ready/block_id`，避免与
`tbt_fec_blk_reset()` 竞争；`total_known` 的 `group_tail_len==0` 例外保留。

#### 1.2.6 锁顺序（新增规则）

```
rx->lock 是叶子锁：
  - 不在其内取 sess->lock / st->rx_lock / tx->lock / fec_out_lock
  - 不在其内调用 tbt_fec_stream / tbt_rx_fec_deliver / tbt_fec_ack
  - 不在其内做任何 kvzalloc/kfree
w_fec_rx: rx->lock（快照/提交）→ 释放 → codec/交付 → sess->lock / st->rx_lock
w_fec_tx: 与 rx->lock 无交集
```

#### 1.2.7 初始化 / 取消 / destroy 顺序

`tbt_fec_data_init()`（`tbt_fec_data.c:1345`）末尾：

```c
    INIT_DELAYED_WORK(&sess->w_fec_tx, tbt_fec_tx_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx, tbt_fec_rx_work);
    sess->fec = true;
```

`tbt_session_destroy()`（`tbtun/tbt_session.c:1189`）：

```c
    s->closing = true;
    if (s->fec_rx) {
        spin_lock_bh(&s->fec_rx->lock);
        s->fec_rx->stopping = true;
        spin_unlock_bh(&s->fec_rx->lock);
    }
    ... /* w_fbk/w_replace/w_pending/w_reap/w_poll, carriers_stop, streams */
    cancel_delayed_work_sync(&s->w_fbk);
    cancel_delayed_work_sync(&s->w_replace);
    if (s->fec) {
        cancel_delayed_work_sync(&s->w_fec_rx);
        cancel_delayed_work_sync(&s->w_fec_tx);
    }
    tbt_fec_data_free(s);
```

`w_fec_tx` 仍放在 stream teardown 之后取消：`w_kill/w_target` 可能经
`tbt_tx_fin()` 重新 `mod_delayed_work`；`w_fec_rx` 的自重试由
`cancel_delayed_work_sync` 的循环语义覆盖。`stopping` 保证正在运行的
`w_fec_rx` 不向正在拆除的 stream 交付。

`tbt_fec_rx_free()`（`:1290`）增加 `kvfree(rx->rec); kvfree(rx->zero);`。

### 1.3 迁移步骤

1. `tbtun.h`：加 §0.2 字段；`tbt_fec_data_init` 分配 `rec`（`m*S`）与
   `zero`（`S`）、初始化 `w_fec_rx`；`rx_free` 释放；destroy 加取消。
2. `tbt_fec_data.c`：`tbt_fec_rx_record()` 删除 `tbt_fec_blk_decode/
   tbt_fec_rx_retry_all/tbt_fec_deliver_ready` 调用，改为放置 + `work_pending`；
   删除 `tbt_fec_rx_retry_all()`。
3. 把 `tbt_fec_blk_decode()` 改造成“快照→锁外解码→提交”；新增
   `tbt_fec_rx_pass/decode_one` 与 `tbt_fec_rx_work`。
4. `tbt_fec_rx_fin()` 改为只更新 `total_*` 并置 `work_pending`，不再在锁内
   retry/deliver。
5. 新增计数器并接 `/proc` c9。

### 1.4 验证

- **Host**：`make host-test`、`make host-test-san`。`host/test_fec.c` 增加
  “混合指针解码”用例：已知槽指向块缓冲、缺失槽指向独立 `rec`、padding 指向
  零缓冲，验证恢复字节与全缓冲解码一致；STUCK 路径不得写任何 `rec` 槽。
- **QEMU**：`drop_ppm` 打掉单 carrier 的 share，8–16 MiB `tbt_burst`；确认
  `fec_decode_recovered > 0`、`fec_short_unknown=0`、无 `parser stuck`；
  `debug=2` 观察 `rx_work_runs` 不再因解码长时间停住；ftrace `workqueue`
  对比改前 `w_rx` 停滞时间。
- **真机**：32×8、p=30% 注入，`tbt_burst` 8 MiB；`CLOSETIMEOUT=0`，吞吐不低于
  改前；单组 256-share profile 复测 `n=192,m=64`，确认首次解码不再触发
  “sleeping function called from invalid context”。

---

## 2. FEC-B：`tbt_rx_fec_deliver()` 的 `-ENOMEM`

### 2.1 现状（文件:行）

- `tbt_rx_fec_deliver()`：`tbtun/tbt_rx.c:315`；`kmalloc(..., GFP_ATOMIC)`
  失败返回 `-ENOMEM`（`:326`）。
- `tbt_fec_deliver_ready()`：`tbt_fec_data.c:903`；`:1008` 调用交付，
  **返回值被丢弃**；随后 `:1014-1023` 无条件推进：

```c
rx->next_super++;
rx->deliver_bytes = off;
rx->next_deliver = (s + 1) * (u64)rx->groups * rx->n;
...
rx->ack_deliver = rx->next_deliver;
tbt_fec_ack(sess, rx->next_deliver);
```

一次原子分配失败 → 该包永久丢失，且 ACK 前沿越过它，发送端释放对应数据。

### 2.2 设计

交付循环改为逐包提交：

```c
        for (p = start_p; p < rx->n; p++) {
            for (group = start_g; group < rx->groups; group++) {
                ...
                err = tbt_rx_fec_deliver(st, off - rx->stream_base,
                                         b->data + (size_t)p * rx->s, len);
                if (err == -ENOMEM) {
                    /* commit only the successful prefix; keep the cursor */
                    spin_lock_bh(&rx->lock);
                    rx->deliver_bytes = off;
                    rx->deliver_p = p;
                    rx->deliver_g = group;
                    rx->deliver_partial = true;
                    spin_unlock_bh(&rx->lock);
                    atomic64_inc(&sess->fec_deliver_nomem);
                    mod_delayed_work(system_wq, &sess->w_fec_rx,
                                     msecs_to_jiffies(2));
                    tbt_stream_put(st);
                    return;             /* next_super/next_deliver/ack 不动 */
                }
                if (err == -EPROTO) {   /* u64 偏移溢出，实际不可达 */
                    atomic64_inc(&sess->fec_deliver_bad);
                }
                off += len;
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off;    /* per-packet commit */
                spin_unlock_bh(&rx->lock);
                delivered = true;
            }
            start_g = 0;
        }
        rx->deliver_partial = false;
        rx->next_super++;
        ...
```

**失败时不推进的字段**

| 字段 | 失败时 |
|---|---|
| `next_super` | 不推进 |
| `deliver_bytes` | 只到最后一个成功包的末尾 |
| `next_deliver` | 不推进 |
| `ack_deliver` | 不推进 |
| ACK | 不发送 |
| 块 `ready` | 保持（已解码，不重解） |

**credit 窗口如何停**：ACK 只在 `delivered && next_deliver > ack_deliver` 时
发送；失败路径不发 ACK。发送端 `tbt_fec_tx_credit_ok()`（`:479`）用
`seq - peer_pkts < credit`，其 `credit` 等于接收端 `slots*groups*n`，正好是
arena 容量；最后一个 ACK 的 credit 用尽后发送端自然停在窗口内，不会把 share
推到 `fec_drop_window`。

**重试**：`mod_delayed_work(..., 2ms)`；新 share 到达时 `rx_record` 也会
`schedule_delayed_work(..., 0)`，可提前唤醒。重试从
`(deliver_p, deliver_g)` 继续，已交付前缀不重放。

**可选加固**：`tbt_rx_fec_deliver()` 在 `kmalloc` 之前先做一次
`off + len <= READ_ONCE(st->rx_next)` 的快速检查（仍需在 `st->rx_lock` 下
复核），避免重试时对已交付包反复分配。

### 2.3 迁移步骤

1. 在 `tbt_fec_deliver_ready()` 中引入 `start_p/start_g` 与游标提交；
   `-ENOMEM` 分支按上表返回。
2. 加 `fec_deliver_nomem/fec_deliver_bad` 计数器。
3. （可选）加 debug-only `fec_deliver_fail_ppm` 注入器，仅用于验证失败路径，
   默认 0。

### 2.4 验证

- **Host**：数据面函数是 `static`，主机侧只验证 `tbt_rx_fec_deliver` 的
  `off > U64_MAX - len` 返回 `-EPROTO`；`-ENOMEM` 路径用注入器在 QEMU 验证。
- **QEMU**：打开 `fec_deliver_fail_ppm=10000`（1%），8 MiB 流；要求
  `fec_deliver_nomem>0` 且流最终完成、`tx_ack` 不回退、无 `CLOSETIMEOUT`。
  关闭注入后重跑，`fec_deliver_nomem=0`。
- **真机**：内存压力下（`stress-ng --vm`）跑 64 KiB/1 MiB 探测；确认没有
  “应用收到缺字节但 ACK 已过”的现象（对比发送端 `tx_seq` 与接收端
  `rx_delivered`）。

---

## 3. FEC-C：FIN 基数

### 3.1 现状（文件:行）

- FIN 接收：`tbtun/tbt_rx.c:445`；`:463` 调用
  `tbt_fec_rx_fin(sess, hdr->seq)`。
- `tbt_fec_rx_fin()`：`tbt_fec_data.c:1259`：

```c
        rx->total_pkts = total_bytes ?
            (total_bytes + rx->s - 1) / rx->s : 0;
```

这是**每 stream 的包数**；而 `total_pkts` 在别处是**会话绝对包 seq**：

- LAST 写入 `rx->total_pkts = h.seq + 1`（`:1230`）；
- 交付比较 `seq = base + p*groups + group` 与 `total_pkts`（`:976`、`:1017`）。

第二个 stream 的 `total_pkts` 会远小于 `next_super` 的基址 → `s > final`，
一个字节也不交付、ACK 不前进，最终 `CLOSETIMEOUT`。

- FIN 发送门：`tbtun/tbt_tx.c:2294-2299` 要求 `tx_ack == tx_seq` 才写出
  FIN。接收端若正等 FIN 才能确定末尾，就形成死锁（最后由 close timer 杀流）。
- `tbt_tx_fin()`：`tbtun/tbt_tx.c:312`；`:320-324` 先
  `tbt_fec_tx_flush()` 置 `flush_req`，紧接着 `tbt_fec_tx_idle()` 要求
  `!flush_req`（`tbt_fec_data.c:499`）——除非 work 恰好在两条指令之间跑完，
  否则永远返回 0，FIN 可能根本排不进队列。

### 3.2 设计

#### 3.2.1 wire（`tbtun/tbt_uapi.h`）

```c
#define TBT_CONV 0x0006u
#define TBT_FIN_BODY_LEN 16
```

FIN body（小端，`body_len` 必须恰为 16）：

| 偏移 | 类型 | 含义 |
|---|---|---|
| +0 | u64 | `pkt_end`：本 stream 最后一个 DATA 包之后的**绝对会话包 seq** |
| +8 | u64 | `pkt_base`：本 stream 第一个 DATA 包的**绝对会话包 seq**，按 super-block 对齐 |

`tbt_body_bounds()`（`tbt_uapi.h:252`）：

```c
    case TBT_F_FIN:
        *min = TBT_FIN_BODY_LEN;
        *max = TBT_FIN_BODY_LEN;
        return true;
```

`tbt_hdr_check()`（`tbt_proto.c:77`）的 `conv != TBT_CONV` 与 body bounds 自动
拒绝旧版本；`tbt_rec_scan()`（`tbt_rec.c:13`）无需改动，
`body_len=16 ≤ TBT_PAYLOAD_MAX`。

#### 3.2.2 发送端

`tbt_fec_packetize()`（`:414`）在首包记录 base：

```c
    if (!st->fec_pkt_base_set) {
        st->fec_pkt_base = seq;
        st->fec_pkt_base_set = true;
    }
```

`tbt_fec_tx_finish()`（`:407`）改为：

```c
static void tbt_fec_tx_finish(struct tbt_session *sess, struct tbt_stream *st)
{
    struct tbt_fec_tx *tx = sess->fec_tx;

    if (READ_ONCE(st->fec_finished))
        return;
    WRITE_ONCE(st->fec_pkt_end, tx->seq);   /* actual end, before the jump */
    if (st->fec_pkt_base_set)
        tbt_fec_tx_close_block(sess, true); /* §4: parity/markers, jump */
    smp_store_release(&st->fec_finished, true);
}
```

`tbt_tx_fin()`（`tbt_tx.c:312`）先请求 flush，再等 finish 完成，消除
`flush_req` 自锁：

```c
    if (sess->fec) {
        if (!smp_load_acquire(&st->fec_finished)) {
            tbt_fec_tx_flush(st);   /* first call: request, return 0 */
            return 0;
        }
        if (!tbt_fec_tx_idle(st))
            return 0;
    }
    ...
    spin_lock(&sess->tx_lock);
    if (!st->tx_fin_seq_set) {
        st->tx_fin_seq = st->tx_seq;
        if (sess->fec) {
            st->tx_fin_pkts = READ_ONCE(st->fec_pkt_end);
            st->tx_fin_pkt_base = st->fec_pkt_base_set ?
                                  READ_ONCE(st->fec_pkt_base) :
                                  READ_ONCE(st->fec_pkt_end);
        }
        st->tx_fin_seq_set = true;
    }
    ...
    tbt_put_le64(body, st->tx_fin_pkts);
    tbt_put_le64(body + 8, st->tx_fin_pkt_base);
    tbt_tx_control_pinned(..., TBT_F_FIN, st->sid, seq, body,
                          sizeof(body), ...);
```

非 FEC session 也发送 16 字节零 body（`pkt_end=pkt_base=0`），保持
`tbt_hdr_check` 的单一长度规则。

#### 3.2.3 发送端 FIN 门

`tbt_tx.c:2294` 改为：

```c
    if (type == TBT_F_FIN && fr->st && !sess->fec &&
        READ_ONCE(fr->st->state) != TBT_ST_DEAD &&
        READ_ONCE(fr->st->tx_ack) != READ_ONCE(fr->st->tx_seq))
        continue;
```

FEC 的 FIN 由 `tbt_tx_fin()` 的 `fec_finished + idle` 保证在所有 FEC 记录写完之后
入队；接收端不依赖 FIN 的字节顺序，只依赖 body 的 `pkt_end`。

#### 3.2.4 接收端

`tbt_rx.c` FIN 分支解析 body：

```c
        u64 pkt_end = tbt_le64(payload);
        u64 pkt_base = tbt_le64(payload + 8);

        if (pkt_end < pkt_base) {
            atomic64_inc(&sess->fec_fin_bad);
            return -EPROTO;
        }
        if (sess->fec)
            tbt_fec_rx_fin(sess, st, hdr->seq, pkt_end, pkt_base);
```

`tbt_fec_rx_fin()` 新签名：

```c
void tbt_fec_rx_fin(struct tbt_session *sess, struct tbt_stream *st,
                    u64 total_bytes, u64 pkt_end, u64 pkt_base);
```

逻辑（在 `rx->lock` 内）：

```c
    u64 span = (u64)rx->groups * rx->n;
    if (span && (pkt_base % span)) {
        atomic64_inc(&sess->fec_fin_bad);
        return;                                   /* must be block-aligned */
    }
    if (rx->stream_sid && st->sid != rx->stream_sid) {
        /* FIN for the next stream arrived before adoption: keep one slot */
        rx->next_fin_sid = st->sid;
        rx->next_fin_end = pkt_end;
        rx->next_fin_base = pkt_base;
        rx->next_fin_bytes = total_bytes;
        rx->next_fin_valid = true;
        rx->work_pending = true;
        return;
    }
    if (!rx->stream_sid) {
        rx->stream_sid = st->sid;
        rx->stream_base = rx->deliver_bytes;
        rx->stream_pkt_base = pkt_base;
    }
    if (!rx->total_known) {
        rx->total_pkts = pkt_end;                 /* absolute, not ceil(bytes/S) */
        rx->total_bytes = total_bytes;
        rx->total_known = true;
    } else {
        if (pkt_end != rx->total_pkts)
            atomic64_inc(&sess->fec_fin_bad);     /* keep LAST's monotone value */
        if (!rx->total_bytes)
            rx->total_bytes = total_bytes;
    }
    rx->work_pending = true;
```

`deliver_ready()` 的 stream 绑定改为“粘住当前交付对象”，避免新 OPEN 提前切走
旧 stream 的尾块：

```c
static struct tbt_stream *tbt_fec_rx_stream(struct tbt_session *sess,
                                            bool *is_new)
{
    struct tbt_fec_rx *rx = sess->fec_rx;
    struct tbt_stream *st;

    if (rx->stream_sid) {
        st = tbt_stream_get(sess, rx->stream_sid, false, false);
        if (st)
            return st;            /* keep delivering the bound stream */
    }
    st = tbt_fec_stream(sess);
    if (st && st->sid != rx->stream_sid)
        *is_new = true;
    return st;
}
```

`is_new` 时在 `rx->lock` 内重置
`stream_sid/stream_base/stream_pkt_base/total_*`，并消费匹配 sid 的
`next_fin_*`。

#### 3.2.5 ACK 字节估计

`:1128` 的 `a.next_deliver * TBT_FEC_S` 改为 stream 相对：

```c
    u64 base = READ_ONCE(st->fec_pkt_base_set ? st->fec_pkt_base : 0);
    u64 rel = a.next_deliver > base ? a.next_deliver - base : 0;
```

避免第二个 stream 一开始就被 `clamp(tx_seq)` 过度确认。

#### 3.2.6 0.1.0 ↔ 0.2.0 的失败方式

| 方向 | 失败点 | 结果 |
|---|---|---|
| 0.1.0 → 0.2.0 | 首个 HELLO 的 `conv=0x0005`，`tbt_hdr_check()`（`tbt_proto.c:77`） | `-EPROTO`，carrier 关闭，不解析后续字节 |
| 0.2.0 → 0.1.0 | 0.1.0 的 `tbt_hdr_check()` 要求 `0x0005` | 同上 |
| 即使绕过 conv | 0.1.0 的 FIN `max=0` 收到 16 字节 body | `-EPROTO` |
| FEC 记录 | `TBT_FEC_CONV 0x1002` 对 0.1.0 的 `0x1001` | `tbt_fec_hdr_check()` `-EPROTO` |

### 3.3 迁移步骤

1. `tbt_uapi.h`：`TBT_CONV 0x0006`、`TBT_FIN_BODY_LEN`、`tbt_body_bounds`
   FIN 16。
2. `tbtun.h`：stream 新字段；`tbt_fec_tx_finish/packetize` 签名；
   `tbt_fec_rx_fin` 原型。
3. `tbt_fec_data.c`：base/end/finish/close_block 推进；`rx_fin` 绝对
   `pkt_end`。
4. `tbt_tx.c`：FIN body、`fec_finished` 门、pump 门跳过 FEC。
5. `tbt_rx.c`：FIN body 解析与校验。
6. host 测试：`test_fuzz.c:205`、`test_http.c:2348` 的 FIN body 从 0 改 16；
   新增 `pkt_end < pkt_base` 拒绝用例。

### 3.4 验证

- **Host**：`test_fuzz`/`test_http` 的 FIN 编解码；新增纯函数
  `tbt_fec_fin_valid(pkt_end, pkt_base, span, prev_end)` 的边界测试（0、相等、
  非对齐、跨越前一 stream）。
- **QEMU**：两个顺序 stream（HTTP keep-alive 控制连接 → 新数据连接）；
  `debug=1` 检查 `fec_fin_bad=0`、`CLOSETIMEOUT=0`；用 `drop_ppm` 打掉最后一个
  DATA（含 LAST），确认 FIN 的 `pkt_end` 仍能交付尾块；打掉 FIN，确认 close
  timer 有界、无永久挂起。
- **真机**：iperf3 控制+数据双连接、HTTP 两轮 keep-alive；核对接收端
  `rx_delivered` 与发送端 `tx_seq` 逐字节一致；`/proc` 的 `tx_ack` 不回退。

---

## 4. FEC-D：短包 flush 与 groups>1 的空 group

### 4.1 现状（文件:行）

- flush 决策：`tbt_fec_data.c:595-650`：

```c
        bool completes = (u16)(share + 1) == tx->n;
        bool flushable = completes;
        ...
        if (!flushable) {
            flushable = true;
            for (g = 0; g < tx->groups; g++) {
                if (!tx->g[g].nreal) { flushable = false; break; }
            }
        }
```

两个缺陷：

1. 一个 group 的块**已满**时 `nreal` 被清零，和“该 group 在本块没有包”无法
   区分；`flushable` 因此可能永远为假，短包 flush 永不发生。
2. 空 group 无法表达；`tbt_fec_tx_close_block()`（`:378`）对 `nreal==0` 直接
   `continue`，接收端 `tbt_fec_super_ready()`（`:878`）会永远等这个 group 的块
   （除非 `total_known` 且是 final block）。

- 早关跳转：`:397-401`：

```c
        tx->seq = (block_id + 1) * (u64)tx->n * tx->groups +
                  (tx->seq % tx->groups);
```

落到轮转中间。下一块中，落点之前的 group 会跳过 share 0，但 `packetize()` 仍写
`nreal = share + 1`（`:459`），把跳过的槽位算成真实包；`blk[0]` 是上一块的残留，
parity 描述了一个“洞”，接收端把零填充当数据交付——**静默数据损坏**。

### 4.2 设计

#### 4.2.1 发送端

1. `tbt_fec_gtx.blk_id` 区分“空”与“已满”：
   - `blk_id == block_id && nreal == 0` → 本块已满（parity 已发），跳过；
   - `blk_id != block_id` → 本块没有该 group 的包，发空标记。

2. `tbt_fec_packetize()` 进入新块时重置：

```c
    if (tx->g[group].blk_id != block_id) {
        tx->g[group].blk_id = block_id;
        tx->g[group].nreal = 0;
        tx->g[group].last_len = 0;
    }
    if (share != tx->g[group].nreal)
        atomic64_inc(&sess->fec_tx_share_gap);   /* invariant guard */
```

3. `tbt_fec_tx_close_block()` 的 `advance` 改为块对齐：

```c
        tx->seq = (block_id + 1) * (u64)tx->n * tx->groups;
```

每个 group 在新块都从 share 0 开始，`nreal = share + 1` 重新成立，
`emit_parity()` 的 `last_seq = seq_of_block_pkt(block_id, group, win_len-1, ...)`
也重新成立。

4. 空块标记 `tbt_fec_tx_emit_empty()`：

```c
static void tbt_fec_tx_emit_empty(struct tbt_session *sess, u16 group,
                                  u64 block_id)
{
    struct tbt_fec_tx *tx = sess->fec_tx;
    struct tbt_fec_hdr h;
    u16 rec_len;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = tx->n;                 /* parity 0 */
    h.n = (u8)tx->n;
    h.m = (u8)tx->m;
    h.block_id = block_id;
    h.seq = tbt_fec_seq_of_block_pkt(block_id, group, 0, tx->groups, tx->n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    if (tbt_fec_make_record(sess, &h, NULL, tx->rec, &rec_len))
        return;
    tbt_fec_send_record(sess, (u16)(group * (tx->n + tx->m) + tx->n),
                        tx->rec, rec_len, false);
    atomic64_inc(&sess->fec_empty_block);
}
```

`tbt_fec_make_record()` 对 `payload_len=0` 不 memcpy；
`tbt_crypto_xor_at()` 对 `len=0` 直接返回 0（`tbt_crypto.c:376`），crypto on
也安全。

5. `close_block()` 循环：

```c
    for (group = 0; group < tx->groups; group++) {
        struct tbt_fec_gtx *g = &tx->g[group];

        if (g->blk_id != block_id) {
            tbt_fec_tx_emit_empty(sess, group, block_id);
            continue;
        }
        if (!g->nreal || g->nreal >= tx->n)
            continue;                       /* full: parity already sent */
        memset(g->blk + (size_t)g->nreal * TBT_FEC_S, 0,
               (size_t)(tx->n - g->nreal) * TBT_FEC_S);
        tbt_fec_tx_emit_parity(sess, group, block_id, g->nreal);
        g->nreal = 0;
    }
```

6. flush 决策简化为“超时即关”：

```c
        else if (now >= tx->short_at_ns && tbt_fec_tx_credit_ok(tx))
        {
            tbt_fec_packetize(sess, st, false);
            tbt_fec_tx_close_block(sess, true);   /* always safe with markers */
        }
```

删除 `completes/flushable` 及其循环。`tbt_fec_tx_finish()` 也调用
`close_block(true)`（§3）。

#### 4.2.2 接收端

`tbt_fec_rx_record()` 的 PARITY 分支：

```c
    if (h.win_len == 0) {
        if (b->ndata || b->nparity || b->win_len_set)
            return -EPROTO;                 /* marker vs data: mixed source */
        b->empty = true;
        b->win_len_set = true;
        b->ready = true;
        rx->work_pending = true;
    } else { ... }
```

- `tbt_fec_blk_real_len()` 开头 `if (b->empty) return 0;`。
- 交付循环 `p >= real` 自然跳过空块。
- `tbt_fec_super_ready()` 保留 `total_known` 的 `group_tail_len==0` 例外，
  作为“标记丢失 + FIN 已到”的兜底。

#### 4.2.3 `tbt_fec_hdr_check()`（`tbt_fec.c:194`）

```c
    if (h->type == TBT_FEC_DATA) {
        if (!h->payload_len || h->payload_len > max_payload) return -EPROTO;
        if (h->epoch) return -EPROTO;       /* §6 */
        if (h->win_len) return -EPROTO;
    } else { /* PARITY */
        if (h->win_len == 0) {
            if (h->payload_len || h->epoch) return -EPROTO;
        } else {
            if (h->win_len > h->n) return -EPROTO;
            if (h->payload_len != max_payload) return -EPROTO;
            if (h->epoch > max_payload) return -EPROTO;
        }
    }
```

**wire**：`TBT_FEC_CONV 0x1002`（与 §6 共用一次升级）。

### 4.3 迁移步骤

1. `tbt_fec.c`：`hdr_check` 允许 marker、收紧 `win_len ≤ n`、DATA epoch=0。
2. `tbt_fec_data.c`：`gtx.blk_id`、块对齐跳转、`emit_empty`、`close_block`
   标记、删除 `flushable`。
3. 接收端 marker 处理与 `empty` 语义。
4. `host/test_fec.c`：`PARITY win_len=0` 由 `-EPROTO` 改为合法 marker
   （`payload_len=0,epoch=0`）；`win_len=1024` 改为 `-EPROTO`；新增“marker 后到
   DATA”拒绝用例。

### 4.4 验证

- **Host**：`make host-test`（`test_fec`）覆盖 marker 合法/非法组合、
  `win_len > n`、DATA epoch 非零。
- **QEMU**：`groups=32, n=4, m=4`，写入 37 字节后静默；预期 1 个 DATA +
  31 个 marker + 4 个 parity，接收端一个 RTT 内交付；`fec_short_tx=1`、
  `fec_short_unknown=0`、`fec_proto_fail=0`。随后 256 KiB 突发，确认没有因每块
  marker 造成吞吐回退。
- **真机**：`host/tbt_ping` 37 B/8191 B/8192 B/8193 B；HTTP 小请求（cookie）
  与 8 MiB 响应；`fec_empty_block` 随短消息增长，`fec_tx_share_gap=0`。

---

## 5. FEC-F：ACK 溢出与上界

### 5.1 现状（文件:行）

- `tbt_fec_data.c:1128`：

```c
u64 acked = a.next_deliver * TBT_FEC_S;
```

`a.next_deliver` 是 peer 可控 u64，乘法可回绕。

- `:1157`：

```c
if (a.credit_packets)
    WRITE_ONCE(sess->fec_tx->credit, a.credit_packets);
```

peer 可给 `0xffffffff` 的 credit，发送端会跑出接收端 arena，share 被
`fec_drop_window` 丢弃，块永远不完整。

### 5.2 设计

在 ACK 分支入口先钳位，再做饱和运算：

```c
        struct tbt_fec_tx *tx = sess->fec_tx;
        u64 sent = READ_ONCE(tx->seq);

        /* 1. 上界：ACK 不可能超过已发送的包 */
        if (a.next_deliver > sent) {
            a.next_deliver = sent;
            atomic64_inc(&sess->fec_ack_bad);
        }
        /* 2. stream 相对前沿，饱和乘 S */
        u64 base = READ_ONCE(st->fec_pkt_base_set ? st->fec_pkt_base : 0);
        u64 rel = a.next_deliver > base ? a.next_deliver - base : 0;
        u64 txb = READ_ONCE(st->tx_seq);
        u64 limit = txb / TBT_FEC_S + 1;
        u64 acked = rel > limit ? txb : rel * TBT_FEC_S;

        if (acked > txb)
            acked = txb;
        /* 3. credit 上界与 0 拒绝 */
        if (!a.credit_packets) {
            atomic64_inc(&sess->fec_ack_bad);
        } else {
            WRITE_ONCE(tx->credit,
                       min_t(u32, a.credit_packets, TBT_FEC_CREDIT_MAX));
        }
```

具体拒绝/钳位条件：

| 条件 | 动作 |
|---|---|
| `a.next_deliver > tx->seq` | 钳到 `tx->seq`，`fec_ack_bad++` |
| `rel > tx_seq/S + 1` | `acked = tx_seq`（饱和，不乘） |
| `acked > tx_seq` | 钳到 `tx_seq` |
| `a.credit_packets == 0` | 忽略，保留旧 credit，`fec_ack_bad++` |
| `a.credit_packets > TBT_FEC_CREDIT_MAX` | 钳到 `TBT_FEC_CREDIT_MAX` |

`peer_pkts` 的 cmpxchg 使用钳位后的 `a.next_deliver`，保证单调且不超过
`tx->seq`。

**无 wire 变化**。

### 5.3 迁移步骤

1. 抽出纯函数到 `tbt_fec.c/h`，便于 host 测试：

```c
u64 tbt_fec_ack_bytes(u64 next_deliver, u64 pkt_base, u64 tx_seq);
```

2. ACK 分支改用该函数并加上 credit 钳位。
3. 加 `fec_ack_bad` 计数器。

### 5.4 验证

- **Host**：`host/test_fec.c` 对 `tbt_fec_ack_bytes` 做边界：
  `next_deliver=0`、`=pkt_base`、`=U64_MAX`、`tx_seq=0`、
  `tx_seq=U64_MAX/2`、`rel` 恰在 `limit` 上下；断言结果 ≤ `tx_seq` 且不回绕。
- **QEMU**：构造 `next_deliver=U64_MAX` 的 ACK（host 侧 raw socket 注入或
  debug 注入器），确认 `fec_ack_bad>0`、`tx_ack` 不超过 `tx_seq`、无
  `CLOSETIMEOUT`；credit=0/0xffffffff 的 ACK 不改变窗口。
- **真机**：正常 8 MiB 流，`fec_ack_bad=0`；`tx_ack` 单调。

---

## 6. `short_len` 一致性（epoch / cut）

### 6.1 现状（文件:行）

- 发送端 DATA：`:447` `h.epoch = (u16)(len < TBT_FEC_S ? len : 0);` —— 每个短
  share 都写自己的长度，包括块中间的短包。
- 发送端 PARITY：`:357`
  `h.epoch = (g->last_len && g->last_len < S) ? g->last_len : 0;`。
- 接收端 DATA：`:1224` `if (h.epoch) b->short_len = h.epoch;`。
- 接收端 PARITY：`:1242` 同样 last-writer-wins。
- `b->short_len` 在交付时只用于 `p + 1 == real`（`:989`）。
- `nreal` 取最小值（`:1244-1246`），容忍 parity 之间的 `win_len` 不一致。

后果：块中间一个短 share 的 epoch 被后到的记录覆盖，若最后一个真实 share 是从
parity 恢复的，交付会用它截断，可能少交付字节；`win_len` 不一致时行为取决于
到达顺序。

### 6.2 设计

#### 6.2.1 语义收紧（`TBT_FEC_CONV 0x1002`）

1. DATA 记录的 `epoch` 必须为 0（`hdr_check` 拒绝非零）；发送端 DATA 不再写
   epoch。
2. 只有 PARITY 携带块的 `epoch`，含义固定为“**share `win_len-1` 的
   payload_len，0 表示 S**”。
3. 一个块的第一个 PARITY 记录固定 `(win_len, epoch)`，后续 PARITY 必须完全
   一致；否则 `-EPROTO` + `fec_epoch_conflict`。
4. `win_len <= n`（`hdr_check`），`epoch <= S`。
5. 第一个 PARITY 到达时，若已有 DATA 槽位 `i >= win_len`，说明对端混用了不同的
   块形状 → `-EPROTO`（“混合来源拒绝”）。
6. `epoch=0` 且 `win_len<n` 合法：最后一个真实 share 是满的。

#### 6.2.2 接收端代码

```c
    } else { /* PARITY, win_len > 0 */
        if (!b->win_len_set) {
            u16 i;
            for (i = h.win_len; i < rx->n; i++) {
                if (b->dpresent[i]) {
                    atomic64_inc(&sess->fec_epoch_conflict);
                    return -EPROTO;
                }
            }
            b->nreal = h.win_len;
            b->short_len = h.epoch;
            b->win_len_set = true;
        } else if (b->nreal != h.win_len || b->short_len != h.epoch) {
            atomic64_inc(&sess->fec_epoch_conflict);
            return -EPROTO;
        }
        ...
    }
```

#### 6.2.3 `short_unknown` 的判定

`tbt_fec_blk_decode()`（`:772`）中：

```c
    if (missing) {
        if (!b->win_len_set) {
            /* 没有 parity 却要恢复：只有在 have_parity >= missing 时才可能
             * 进入这里，因此对一致的对端不可达；保留为协议守卫。 */
            for (i = 0; i < real; i++)
                if (b->dpresent[i] && b->dlen[i] && b->dlen[i] < rx->s) {
                    if (!b->short_unknown) {
                        b->short_unknown = true;
                        atomic64_inc(&sess->fec_short_unknown);
                    }
                    return -EAGAIN;
                }
        }
        /* win_len_set 时 short_len 权威：0=最后一个真实 share 满，
         * 否则为其长度 */
    }
```

- 交付：`b->dlen[p]` 优先（present share 自己的长度）；仅当
  `p+1 == real` 且 `dlen[p]==0` 时用 `b->short_len`。
- `epoch > S` 由 `hdr_check` 的 `h->epoch > max_payload` 拒绝；
  `win_len > n` 新增拒绝。
- 块的 `cut` 即 `(n, m, win_len, epoch)`；`n/m` 已在 `rx_record` 对
  `rx->n/m` 校验，`win_len/epoch` 由首条 PARITY 固定。

#### 6.2.4 发送端

```c
    /* DATA */
    h.epoch = 0;
    /* PARITY (emit_parity) */
    h.epoch = (u16)(g->last_len && g->last_len < TBT_FEC_S ? g->last_len : 0);
```

`g->last_len` 在新块由 `blk_id` 重置（§4），保证它属于当前块。

### 6.3 迁移步骤

1. `tbt_fec.c:hdr_check`：DATA epoch=0；PARITY `win_len ≤ n`、`win_len==0` 的
   marker 规则；`epoch ≤ S` 保留。
2. 发送端 DATA epoch=0；PARITY 不变（仍用 `last_len`）。
3. 接收端首条 PARITY 固定 `(win_len, epoch)`；删除
   `if (h.epoch) b->short_len = h.epoch;` 的两处 last-writer-wins。
4. host 测试：`test_fec.c:356-366` 的 DATA epoch=64 改为 `-EPROTO`；新增
   “两条 PARITY 的 win_len/epoch 不一致 → -EPROTO”“win_len=2 但已收到 share 3
   → -EPROTO”。
5. 计数器 `fec_epoch_conflict` 接 `/proc`。

### 6.4 验证

- **Host**：`make host-test`；`test_fec` 覆盖 `epoch=0/S/S+1`、
  `win_len=0/1/n/n+1`、PARITY 不一致、marker 后 DATA。
- **QEMU**：37 B 短包（块中间短 share）+ 最后一个 share 丢失，由 parity 恢复；
  核对应用收到的字节数等于写入字节数（不是 S 的倍数）；
  `fec_short_unknown=0`、`fec_epoch_conflict=0`。
- **真机**：8 MiB 流中每 1 MiB 注入一次 1 字节短包（`tbt_ping` 变长写），
  确认交付字节逐字节一致；`fec_short_unknown=0`。

---

## 7. `TBT_PARSER_BUF` 不变量

### 7.1 现状（文件:行）

- `tbtun/tbtun.h:511-519`：

```c
#define TBT_PARSER_BUF (TBT_FEC_REC_MAX > TBT_REC_MAX ? TBT_FEC_REC_MAX : TBT_REC_MAX)
#define TBT_CAR_PART_MAX (TBT_HTTP_FEC_FRAME_MAX > TBT_REC_MAX ? TBT_HTTP_FEC_FRAME_MAX : TBT_REC_MAX)
struct tbt_parser { u8 buf[TBT_PARSER_BUF]; size_t have; };
```

- `TBT_FEC_REC_MAX = TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX = 8232`
  （`tbt_uapi.h:142`）。
- `TBT_REC_MAX = TBT_HDR_LEN + TBT_PAYLOAD_MAX = 8224`（`:212`）。
- `tbt_fec_record_len()`（`tbt_fec_data.c:712`）接受
  `payload_len ≤ TBT_FEC_PAYLOAD_MAX`；`tbt_rec_scan()`（`tbt_rec.c:13`）依赖
  `tbt_hdr_check()` 的 `body_len ≤ TBT_PAYLOAD_MAX`。
- 两者与 `TBT_PARSER_BUF` 的关系没有任何编译期约束。若有人把
  `TBT_FEC_PAYLOAD_MAX` 调大而忘记调缓冲，`tbt_rx_skb()` 的
  `take = min(len - done, sizeof(p->buf) - p->have)` 会让记录永远拼不完整，
  parser 卡死（`p->have == sizeof(p->buf)` 时返回 `-EAGAIN`）。

### 7.2 设计

#### 7.2.1 不变量

```
TBT_PARSER_BUF      >= max(TBT_REC_MAX, TBT_FEC_REC_MAX)
TBT_FEC_REC_MAX     == TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX
TBT_REC_MAX         == TBT_HDR_LEN + TBT_PAYLOAD_MAX
TBT_CAR_PART_MAX    >= TBT_FEC_REC_MAX
sizeof(tbt_http_rx.rec) == TBT_FEC_REC_MAX
```

#### 7.2.2 编译期断言

在 `tbt_fec_data_init()`（`tbt_fec_data.c:1345`）入口：

```c
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_FEC_REC_MAX != TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX);
    BUILD_BUG_ON(TBT_REC_MAX != TBT_HDR_LEN + TBT_PAYLOAD_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(sizeof(((struct tbt_http_rx *)0)->rec) != TBT_FEC_REC_MAX);
```

`tbt_fec_data.c` 经 `tbtun.h:103` 包含 `tbt_http.h`，因此最后一个断言可放在
同一处；主机侧 `tbt_uapi.h` 也可加：

```c
_Static_assert(TBT_PARSER_BUF >= TBT_FEC_REC_MAX, "parser buffer < FEC record");
_Static_assert(TBT_PARSER_BUF >= TBT_REC_MAX, "parser buffer < old record");
```

#### 7.2.3 运行期上界

`tbt_fec_record_len()` 的 `len > TBT_FEC_PAYLOAD_MAX → TBT_REC_PROTO` 已是上界；
再加一条显式防御：

```c
    *need = TBT_FEC_HDR_LEN + len;
    if (*need > TBT_PARSER_BUF)
        return TBT_REC_PROTO;
```

`tbt_rec_scan()` 的 `need = TBT_HDR_LEN + hdr.body_len` 同理，由 `hdr_check` 的
`body_len ≤ TBT_PAYLOAD_MAX` 保证。

### 7.3 迁移步骤

1. 在 `tbt_uapi.h` / `tbt_http.h` 加 `_Static_assert`。
2. 在 `tbt_fec_data_init()` 加 `BUILD_BUG_ON`。
3. 在 `tbt_fec_record_len()` 加 `need > TBT_PARSER_BUF` 防御。
4. 不改任何长度常量。

### 7.4 验证

- **Host**：`make host-test`；`test_parse`/`test_fuzz`/`test_http` 全绿。
- **编译期反证**：临时把 `TBT_FEC_PAYLOAD_MAX` 改成 8193（仅本地试验），确认
  `BUILD_BUG_ON` 立即失败；恢复后重新编译通过。
- **真机**：最大记录（8192 payload）与 marker（0 payload）各跑一次，
  `http_parse_err=0`、`parser stuck` 日志为 0。

---

## 8. 附：完整 wire 变化清单

| 常量 | 旧值 | 新值 | 影响 |
|---|---|---|---|
| `TBT_CONV` | `0x0005` | `0x0006` | FIN body 16 字节；首个 HELLO 硬拒绝旧版 |
| `TBT_FIN_BODY_LEN` | — | `16` | `+0 u64 pkt_end`、`+8 u64 pkt_base`，小端 |
| `TBT_FEC_CONV` | `0x1001` | `0x1002` | PARITY marker（`win_len=0,payload_len=0,epoch=0`）；DATA epoch 必须 0；PARITY `win_len ≤ n`；`(win_len,epoch)` 块内一致 |
| `TBT_FEC_ACK_LEN` | `16` | `16` | 不变（ACK 不新增字段；stream 相对基址由发送端本地 `fec_pkt_base` 计算） |

**失败模式总表**

| 场景 | 结果 |
|---|---|
| 旧版 HELLO | 首个记录 `-EPROTO`，carrier 关闭 |
| FIN `body_len != 16` | `tbt_hdr_check` `-EPROTO` |
| FIN `pkt_end < pkt_base` | `fec_fin_bad++`，`-EPROTO` |
| FIN `pkt_base % (groups*n) != 0` | `fec_fin_bad++`，忽略该 FIN |
| LAST 与 FIN 的 `pkt_end` 不一致 | `fec_fin_bad++`，保留 LAST 的单调值 |
| PARITY marker 与 DATA 混用 | `-EPROTO` |
| PARITY `(win_len,epoch)` 不一致 | `fec_epoch_conflict++`，`-EPROTO` |
| DATA 槽位 `>= win_len` | `fec_epoch_conflict++`，`-EPROTO` |
| 重复 share | 丢弃，`fec_dup_share++` |
| 迟到 share（块已 ready） | 丢弃，`fec_late_share++` |
| 解码分配失败 | `fec_decode_nomem++`，块保持未 ready，重试 |
| 交付分配失败 | `fec_deliver_nomem++`，游标保留，2 ms 重试 |
| ACK 越界/credit 异常 | 钳位/忽略，`fec_ack_bad++` |
| 发送端 share 序不连续 | `fec_tx_share_gap++`（不应发生） |
