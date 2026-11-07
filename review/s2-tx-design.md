# 0.2.0 TX 路径修复设计

本文只读设计，不改代码。目标是给出彻底、优雅的 TX 修复方案：具体到函数、锁、结构字段、失败分支与迁移顺序。所有行号以当前工作树 `2a8d509` 为准。

## 0. 总览与总不变量

修复分四层。依赖只有一条：**L4 依赖 L2 的 `c->sk` helper**；L1、L2、L3 可独立提交、独立验证。

| 层 | 缺陷 | 根因 | 主要文件 |
|---|---|---|---|
| L1 | `tbt_tx_submit()` dup 帧 UAF | dup 帧在主帧/retain 发布之后才读 `fr->hdr`、`r->data`；`dup_pending` 尚未递增，trim/gc 可释放 `r` | `tbt_tx.c`、`tbt_session.c`（purge 记账） |
| L2 | `sess->car[]` 无锁读 | 无引用计数、无 RCU；`c->sk` 先 `sock_release()` 后清空；对象被 `memset` 复用 | `tbtun.h`、`tbt_carrier.c`、全部读点 |
| L3 | TX-6 16 槽数组 | `path_end[]`/`una_observed_ns[]` 写点无索引检查；retain 循环上界是 `n_car`（可达 256）；FBK 循环上界 256 而 body 只有 16 项 | `tbt_tx.c` |
| L4 | `tbt_tx_fbk()`/`rto`/`bdp` 裸解引用 `c->sk` | 与 `tbt_carrier_release()` 的 `sock_release()` 无互斥 | `tbt_tx.c` |

贯穿全部四层、必须写进代码注释的不变量：

> 从 `sess->car[]` 取得的 `struct tbt_carrier *`，在离开取得它的 RCU 临界区前必须已经 `tbt_carrier_get()`；从 `c->sk` 取得的 `struct sock *`，必须已经通过 `tbt_car_sk_get()` 或 `tbt_car_sk_hold()` 取得。发布到 `sess->tx_q` 的帧，其构造者发布后不得再读该帧或它的 `ret`。

仓库历史（`review/findings.md` finding 2 / 36b）已修复“trim 在 `tx_lock` 外 free retain、pump 的 `batch` 仍持 `fr->ret`”的 UAF；当前代码另有一条**未被记录**的同族竞态：`tbt_tx_submit()` 放锁后仍读 `fr->hdr` 与 `r->data`。L1 修的就是这一条。

---

# 1. L1：`tbt_tx_submit()` 的 dup 帧 UAF

## 1.1 现状（文件:行）

`tbt_tx_submit()`：`tbt_tx.c:2844-2983`。主帧发布原文：

```c
/* tbt_tx.c:2917-2924 */
        fr->hdr = r->hdr;
        fr->hdr.seq = seq;
        fr->open_pending = st->near && !READ_ONCE(st->tx_open_sent);
        fr->ret = r;
        fr->st = st;
        refcount_inc(&st->refs);
        sess->tx_queued += chunk;
        list_add_tail(&fr->list, &sess->tx_q);
        list_add_tail(&r->list, &st->retain);
        st->retain_bytes += chunk;
        spin_unlock(&sess->tx_lock);
```

dup 构造原文：

```c
/* tbt_tx.c:2930-2966 */
        for (u32 k = 0; k < sess->tunnel->cfg.dup; k++)
        {
            ...
            dup = tbt_frame_alloc(chunk, GFP_ATOMIC);
            if (!dup)
            {
                atomic64_inc(&sess->dbg_alloc_fail);
                break;
            }
            dup->hdr = fr->hdr;
            dup->hdr.flags |= TBT_FF_LOW_PRIO;
            dup->dup_copy = true;
            dup->ret = r;
            memcpy(dup->data, r->data, chunk);
            spin_lock(&sess->tx_lock);
            if (sess->tx_queued + chunk > sess->txq_max)
            {
                spin_unlock(&sess->tx_lock);
                kfree(dup);
                break;
            }
            sess->tx_queued += chunk;
            atomic64_add(chunk, &sess->dup_bytes);
            if (r->dup_pending < U8_MAX)
                r->dup_pending++;
            dup->st = st;
            refcount_inc(&st->refs);
            list_add_tail(&dup->list, &sess->tx_q);
            spin_unlock(&sess->tx_lock);
        }
```

三条独立缺陷：

1. **`fr` 已发布，pump 可立即 free。** `fr` 在 `2921` 进入 `tx_q`；pump 在另一 CPU 取 `tx_lock`，把它移入本地 `batch`，写出后 `tbt_frame_free(fr)`（`tbt_tx.c:2801` 附近；`tbt_frame_free` 就是 `kfree(fr)`，`tbt_tx.c:85-93`）。submit 随后在 `2948` 读 `fr->hdr` → UAF read。
2. **`r` 已进入 retain，但 `dup_pending` 仍是 0。** `r` 在 `2922` 进入 `st->retain`；`dup_pending++` 在 `2962-2963`，晚于 `2952` 的 `memcpy(dup->data, r->data, chunk)`。`tbt_tx_retain_trim()`（`tbt_tx.c:495-532`）按 `retain_max` 从最老条目开始释放，**不检查 `dup_pending`**；`tbt_tx_retain_gc()`（`439-493`）同理。它们可以在 `2952` 前后 `kfree(r)` → UAF read；`2962` 还会对已释放的 `r` 写 `dup_pending` → UAF write。
3. **hdr 与 data 可能来自不同记录。** `fr` 被 free/复用后，`dup->hdr` 来自另一条记录，而 `dup->data` 来自 `r`；dup 帧头部与载荷不一致。

`retain_trim()` 的注释已写明它假设“pump 是 `tx_q` 之外帧的唯一持有者”，但 submit 在 `tx_lock` 外仍持有 `fr` 和 `r`，这个假设不成立：

```c
/* tbt_tx.c:489-493 */
 * This runs in the pump, under pump_lock, and nowhere else. The pump is the
 * only owner of frames outside sess->tx_q (its local batch and its retransmit
 * candidates hold fr->ret across a dropped tx_lock), so freeing an entry
 * anywhere else would let one of them dereference it after the free. */
```

## 1.2 设计：发布前完整构造，一次发布

### 1.2.1 新增常量与函数签名

```c
/* tbt_uapi.h；与 tbt_parse.c 的 dup 上界共用 */
#define TBT_DUP_MAX 3

/* tbt_tx.c：锁外构造，只碰本函数私有对象 */
static unsigned int tbt_tx_build_dups(struct tbt_session *sess,
                                       struct tbt_retain *r,
                                       const struct tbt_hdr *hdr,
                                       u16 chunk,
                                       struct tbt_frame *dup[TBT_DUP_MAX]);

/* tbt_tx.c：一次 tx_lock 临界区内发布主帧、retain 条目和全部 dup 帧。
 * 返回 false 表示容量不足，调用者仍拥有 fr/r/dup[]；返回 true 表示
 * 所有权已转移给 tx_q / retain。 */
static bool tbt_tx_publish_data(struct tbt_session *sess,
                                struct tbt_stream *st,
                                struct tbt_frame *fr,
                                struct tbt_frame *const dup[],
                                unsigned int ndup,
                                struct tbt_retain *r,
                                u16 chunk,
                                const struct tbt_hdr *hdr);

/* tbt_tx.c：所有从 tx_q / batch 移除 DATA 帧并 free 的路径共用。
 * 调用者必须持 sess->tx_lock。 */
void tbt_tx_frame_drop_account(struct tbt_session *sess,
                               struct tbt_frame *fr);
```

`tbt_tx_build_dups()` 的 `hdr` 是 submit 栈上的局部副本，`r` 是尚未加入 retain 的私有对象；返回实际构造成功的 dup 数（0..`TBT_DUP_MAX`），失败只减少返回值，不产生部分发布。

### 1.2.2 `tbt_tx_submit()` 改动后的顺序（伪代码）

```c
size_t tbt_tx_submit(struct tbt_stream *st, struct sk_buff *skb,
                     unsigned int off, size_t len)
{
    struct tbt_session *sess = st->sess;
    size_t done = 0;

    atomic64_inc(&sess->tx_submit_calls);
    if (sess->fec)
        return tbt_fec_tx_submit(st, skb, off, len);

    while (done < len)
    {
        u16 chunk = min_t(size_t, len - done, (size_t)TBT_PAYLOAD_MAX);
        struct tbt_retain *r;
        struct tbt_frame *fr;
        struct tbt_frame *dup[TBT_DUP_MAX];
        struct tbt_hdr hdr;
        unsigned int ndup;
        u64 seq;

        /* ---- A. 锁外：只使用本函数的私有对象 ---- */
        r = tbt_retain_alloc(chunk, GFP_ATOMIC);
        fr = tbt_frame_alloc(chunk, GFP_ATOMIC);
        if (!r || !fr)
        {
            kfree(r);
            kfree(fr);
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        if (skb_copy_bits(skb, off + done, r->data, chunk))
        {
            kfree(r);
            kfree(fr);
            atomic64_inc(&sess->dbg_copy_fail);
            break;
        }
        memcpy(fr->data, r->data, chunk);

        /* hdr 是栈上副本：seq 在锁内补，其余字段此时已完整。 */
        hdr = (struct tbt_hdr){
            .type = TBT_F_DATA,
            .body_len = chunk,
            .sid_lo = (u32)st->sid,
            .conv = TBT_CONV,
        };
        hdr.sid_hi = (u8)(st->sid >> 32);

        /* dup 的 hdr/data 全部来自 hdr 与 r->data；r 尚未发布，pump 不可见。 */
        ndup = tbt_tx_build_dups(sess, r, &hdr, chunk, dup);

        /* ---- B. 锁内：一次发布，失败全部回滚 ---- */
        if (!tbt_tx_publish_data(sess, st, fr, dup, ndup, r, chunk, &hdr))
        {
            /* publish 失败时 fr/r/dup[] 仍归本函数所有。 */
            for (u32 k = 0; k < ndup; k++)
                kfree(dup[k]);
            kfree(fr);
            kfree(r);
            WRITE_ONCE(st->tx_paused, true);
            atomic64_inc(&sess->dbg_over_qmax);
            break;
        }

        /* 发布成功后，本函数不再读 fr、dup[]、r。 */
        done += chunk;
        atomic64_add(chunk, &sess->tx_submit_bytes);
    }

    tbt_tx_wake(sess);
    return done;
}
```

`tbt_tx_build_dups()`：

```c
static unsigned int tbt_tx_build_dups(struct tbt_session *sess,
                                       struct tbt_retain *r,
                                       const struct tbt_hdr *hdr,
                                       u16 chunk,
                                       struct tbt_frame *dup[TBT_DUP_MAX])
{
    unsigned int n = 0;

    for (u32 k = 0; k < sess->tunnel->cfg.dup && n < TBT_DUP_MAX; k++)
    {
        u64 goodput = tbt_stat_read(&sess->tx_submit_bytes) + chunk;
        u64 dup_after = tbt_stat_read(&sess->dup_bytes) + chunk;
        struct tbt_frame *d;

        if ((goodput + dup_after) * 1000 >
            goodput * sess->tunnel->cfg.max_inflate_milli)
        {
            atomic64_inc(&sess->dup_suppressed);
            break;
        }
        d = tbt_frame_alloc(chunk, GFP_ATOMIC);
        if (!d)
        {
            atomic64_inc(&sess->dbg_alloc_fail);
            break;
        }
        d->hdr = *hdr;
        d->hdr.flags |= TBT_FF_LOW_PRIO;
        d->dup_copy = true;
        d->ret = r;
        memcpy(d->data, r->data, chunk);
        dup[n++] = d;
    }
    return n;
}
```

`tbt_tx_publish_data()`：

```c
static bool tbt_tx_publish_data(struct tbt_session *sess,
                                struct tbt_stream *st,
                                struct tbt_frame *fr,
                                struct tbt_frame *const dup[],
                                unsigned int ndup,
                                struct tbt_retain *r,
                                u16 chunk,
                                const struct tbt_hdr *hdr)
{
    u64 seq;
    u32 total = (u32)chunk * (1u + ndup);

    if (WARN_ON_ONCE(ndup > U8_MAX))
        return false;

    spin_lock(&sess->tx_lock);
    if (sess->tx_queued + total > sess->txq_max)
    {
        spin_unlock(&sess->tx_lock);
        return false;
    }

    if (!st->tx_start_ns)
        st->tx_start_ns = tbt_now_ns();
    seq = st->tx_seq;
    st->tx_seq = seq + chunk;

    r->seq = seq;
    r->hdr = *hdr;
    r->hdr.seq = seq;
    r->dup_pending = (u8)ndup;

    fr->hdr = *hdr;
    fr->hdr.seq = seq;
    fr->open_pending = st->near && !READ_ONCE(st->tx_open_sent);
    fr->ret = r;
    fr->st = st;
    refcount_inc(&st->refs);

    for (unsigned int k = 0; k < ndup; k++)
    {
        dup[k]->hdr.seq = seq;
        dup[k]->st = st;
        refcount_inc(&st->refs);
    }

    sess->tx_queued += total;
    atomic64_add((u64)chunk * ndup, &sess->dup_bytes);
    st->retain_bytes += chunk;

    list_add_tail(&fr->list, &sess->tx_q);
    list_add_tail(&r->list, &st->retain);
    for (unsigned int k = 0; k < ndup; k++)
        list_add_tail(&dup[k]->list, &sess->tx_q);

    spin_unlock(&sess->tx_lock);
    return true;
}
```

### 1.2.3 失败分支表

| 分支 | 位置 | 已分配对象 | 处理 | 记账 |
|---|---|---|---|---|
| `r`/`fr` 分配失败 | 锁外 | 可能一个成功 | `kfree` 两者，`dbg_alloc_fail++`，`break` | 不变 |
| `skb_copy_bits()` 失败 | 锁外 | `r`、`fr` | `kfree` 两者，`dbg_copy_fail++`，`break` | 不变 |
| 某个 dup 分配失败 | `build_dups` | 已成功的 `dup[0..n)` | 返回 `n`，主帧照发 | 只按 `n` 记账 |
| inflate 超限 | `build_dups` | 已成功的 `dup[0..n)` | `dup_suppressed++`，返回 `n` | 只按 `n` 记账 |
| 锁内容量不足 | `publish_data` | `fr`、`r`、全部 `dup[]` | 返回 `false`；submit 全部 `kfree`，`tx_paused=true`，`dbg_over_qmax++`，`break` | 不变；`seq` 未推进 |
| 发布成功 | `publish_data` | 所有权转移 | 无 | `tx_queued += chunk*(1+n)`；`dup_bytes += chunk*n`；`dup_pending = n`；`retain_bytes += chunk` |

容量检查只做一次，且是权威检查：`tx_queued + chunk*(1+ndup) > txq_max`。锁外不预检查，避免“预检查通过、锁内失败”时已经消耗 `seq`。

### 1.2.4 记账不变量与三处失衡修复

发布后必须同时满足：

```
tx_queued    = Σ 主帧 len + Σ dup 帧 len
dup_bytes    = Σ 已创建 dup len - Σ 已丢弃 dup len
dup_pending  = 该 retain 条目已入队、尚未写出/丢弃的 dup 数
retain_bytes = Σ retain 条目 len（每个 chunk 一份，与 dup 无关）
```

新增 helper：

```c
void tbt_tx_frame_drop_account(struct tbt_session *sess,
                               struct tbt_frame *fr)
{
    if (fr->hdr.type != TBT_F_DATA || !fr->dup_copy)
        return;
    if (tbt_stat_read(&sess->dup_bytes) >= fr->len)
        atomic64_sub(fr->len, &sess->dup_bytes);
    if (fr->ret && fr->ret->dup_pending)
        fr->ret->dup_pending--;
}
```

调用点（全部在 `sess->tx_lock` 内，`tx_queued` 由调用者减）：

1. **pump 的 `drop:` 分支**（`tbt_tx.c:2493-2496`）：现有内联逻辑改为调用 helper。
2. **`tbt_tx.c:2529-2542`**：载体槽在选中与写之间消失，非 pinned 帧直接 `tbt_frame_free(fr)`；当前**不减 `dup_pending`、不减 `dup_bytes`**。修复：free 前 `spin_lock(&sess->tx_lock); tbt_tx_frame_drop_account(sess, fr); spin_unlock(...)`。
3. **`tbt_tx_purge_stream()`**（`tbt_session.c:901-927`）：只对 dead sid 的 DATA 帧 `list_del` 后 free，当前**不减计数**。修复：在 `tx_lock` 内、`list_del` 后调用 `tbt_tx_frame_drop_account(sess, fr)`。`dup` 帧的 `fr->st` 也是该 stream，因此会被正确计入。
4. **`tbt_tx_free_queue()`**（`tbt_tx.c:3233-3251`）：会话销毁时清空队列，当前不减计数。修复：在 `tx_lock` 内对每个帧调用 account；随后 `dup_bytes`/`dup_pending` 归零，再 free。

这些路径的失衡表现是**卡在正数**（减计数有非零守卫、加计数有 `U8_MAX` 上限），后果是 `tbt_hole_copy_inflight()` 在 `tbt_tx.c:1223-1224` 看到 `r->dup_pending != 0` 永远返回 true，抑制空洞修复，直到 stream 被 stall timer 杀死。

`tbt_tx_retain_trim()` / `tbt_tx_retain_gc()` 不需要新增 `dup_pending` 检查：它们在 `tx_lock` 下遍历 `tx_q`，把 `fr->ret == r` 的帧置 NULL（`tbt_tx.c:470-475`、`513-518`），然后才释放 `r`。修复后 submit 在发布后不再读 `r`，所以“r 被 trim 释放”不再构成 UAF；pump 对 `fr->ret == NULL` 的 dup 帧跳过 `dup_pending--`（现有 `if (r)` / `if (fr->ret && ...)` 已保证）。**把这条契约写进 trim 的注释**：submit 不得在发布后持有 `r`。

`tbt_tx_free_stream()`（`tbt_tx.c:3254-3267`）直接 `kfree(r)`：必须保证它运行前 `tbt_tx_free_queue()` 已清空 `tx_q`（现有 `tbt_session_destroy()` 顺序满足：先 `tbt_tx_free_queue(s)`，流 unlink 后才进入 RCU 回调）。在 `free_stream` 加 `WARN_ON_ONCE(r->dup_pending)` 作为契约断言。

### 1.2.5 锁顺序

- `tbt_tx_submit()` 只取 `sess->tx_lock`；`build_dups()` 不取锁；`publish_data()` 只取 `tx_lock`。
- `tbt_tx_wake()` 在锁外调用，它自身走第 2 节的 carrier 快照 helper。
- 禁止在 `tx_lock` 下调用 `tbt_tx_wake()`、`tbt_frame_free()`（后者会 `tbt_stream_put()`，可能睡眠/递归）。
- 与 pump 的顺序：pump 先取 `pump_lock`，再取 `tx_lock`；submit 只取 `tx_lock`，不取 `pump_lock`，不引入新反转。
- `tbt_tx_frame_drop_account()` 的调用者必须已持 `tx_lock`；`dup_bytes` 的 atomic 操作在同一临界区内，保证与 `dup_pending` 同步。

## 1.3 迁移步骤

1. `tbt_uapi.h` 增加 `TBT_DUP_MAX 3`；`tbt_parse.c:487` 的 `v > 3` 改为 `v > TBT_DUP_MAX`。
2. `tbt_tx.c` 增加 `tbt_tx_build_dups()`、`tbt_tx_publish_data()`、`tbt_tx_frame_drop_account()`；`tbtun.h` 声明 account。
3. 重写 `tbt_tx_submit()` 的 while 循环体，删除 `2917-2922` 的旧发布段与 `2924-2966` 的旧 dup 循环。
4. pump 的 `drop:` 分支改用 helper；`2529-2542` 补 account。
5. `tbt_session.c` 的 `tbt_tx_purge_stream()` 补 account；`tbt_tx_free_queue()` 补 account；`tbt_tx_free_stream()` 加 `WARN_ON_ONCE(r->dup_pending)`。
6. trim/gc 注释补“submit 发布后不得再持有 r/fr”。
7. 单独提交，便于回滚；不依赖第 2/3/4 层。

## 1.4 验证

**host（无需 root）**
- `make host-test`：确保 `tbt_uapi.h` 的 `TBT_DUP_MAX` 不破坏 host 解析器。
- 新增 `host/test_tx_publish.c`（建议）：把 `tbt_tx_publish_data()` 抽成不依赖内核分配器的纯记账函数（fake `tx_q`、fake `retain`），断言：
  - 成功发布后 `tx_queued == chunk*(1+n)`、`dup_pending == n`、`dup_bytes == chunk*n`、`retain_bytes == chunk`；
  - 容量不足时四个计数器全部不变、`seq` 不变；
  - `n=0..3` 边界；
  - `tbt_tx_frame_drop_account()` 对 dup/主帧/控制帧的三种行为。
- `sparse`/`make C=1` 检查锁注释与指针类型。

**QEMU**
- `tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R`。重点：
  - probe G/H/I/J：dup 与 hole 修复；
  - probe R：link-flap-then-replace，验证 dup 帧在 carrier 替换时不被 trim 提前释放。
- guest 内 `dup=3`、`max_inflate=2000` 大流量；`debug=2` 观察流结束后 `dup_pending` 归零。
- 人为触发 `tbt_tx_purge_stream()`（kill 一个有大 dup 队列的 stream），确认 `dup_pending` 不卡正数、后续 hole 修复正常。

**真机**
- `review/wan-experiment/arm64/prod-smoke-10s.sh`、`controlled-c8-once.sh`。
- `/proc/net/tbtun/stats`：`dup_bytes_ppm` 与 `dup_tx` 比例合理；`dup_pending` 无在途时回 0；`tx_queued <= txq_max`；`dbg_alloc_fail` 不异常增长。
- KASAN/KFENCE 内核跑 `dup=3` + 强制替换；UAF 消失。
- 回归对照：`dup=0` 时行为与 0.1.x 一致（`dup_suppressed` 不增加，`tx_queued` 曲线不变）。

---

# 2. L2：`sess->car[]` 无锁读取

## 2.1 现状（文件:行）

全仓读取点 48 处（`tbt_tx.c` 30、`tbt_carrier.c` 8、`tbt_rx.c` 1、`tbt_session.c` 4、`tbt_fec_data.c` 3、`tbt_procfs.c` 2）。除 `tbt_carrier.c:580` 的 poll 用 `pollers++` 之外，**没有任何读取点持有 carrier 引用**。

### 2.1.1 `READ_ONCE(sess->car[i])` 后解引用 `c->sk` 的点

| 文件:行 | 函数 | 解引用 | 上下文/锁 | 风险 |
|---|---|---|---|---|
| `tbt_tx.c:596-603` | `tbt_tx_bdp_window` | `c->sk` → `tcp_sk(sk)->srtt_us/mdev_us/rate_*` | pump（`pump_lock`）或 `window_init` | release 先 `sock_release` 后清 `c->sk` → 悬垂 |
| `tbt_tx.c:734-740` | `tbt_tx_rto_ns` | `c->sk` 两次 → `tcp_sk` | pump **或** fbk work / rx 交付检查（无 `pump_lock`） | 同上，且两次读可撕裂 |
| `tbt_tx.c:1165-1173` | `tbt_retain_tcp_done` | `c->sk` → `tcp_sk(sk)->snd_una` | `tx_lock` 下 | 同上 |
| `tbt_tx.c:1819-1825` | `tbt_path_stalled_mask` | `c->sk` → `tcp_sk(sk)->snd_una/snd_nxt` | pump（`pump_lock`） | join 换槽不取 `pump_lock` |
| `tbt_tx.c:2797-2801` | `tbt_tx_pump` 尾部 | `c->sk` → `sk_stream_memory_free`、`sk_socket` | pump（`pump_lock`） | 同上 |
| `tbt_tx.c:3010-3019` | `tbt_tx_fbk` | `c->sk` → `tcp_sk(sk)->srtt_us` | fbk work，**无锁** | 最直接的 UAF |
| `tbt_carrier.c:2377-2380` | `tbt_carrier_resume` | `c->sk` 判空后 `tbt_car_schedule_rx` | `sess->lock` 下 | 锁内不释放，但 join 可在锁外 release |
| `tbt_carrier.c:2401-2412` | `tbt_carrier_snapshot` | `c->sk` → `sk_state/sk_user_data/sk_data_ready/sk_write_space` | 无锁（procfs） | 悬垂 |
| `tbt_session.c:383-403` | `tbt_peers_show` | `c->sk` → `sock_hold(sk)` 后读 tcp 字段 | `tbt_lock` | `sock_hold` 与 release 有 TOCTOU |
| `tbt_fec_data.c:153/174/207` | `tbt_fec_out_try` | 经 `tbt_carrier_send_fec()`（`tbt_carrier.c:1018/1067`）读 `c->sk` | `pump_lock`（`tbt_fec_out_flush`） | join 换槽不取 `pump_lock` |

### 2.1.2 解引用 `c->gen` 的点

| 文件:行 | 函数 | 说明 |
|---|---|---|
| `tbt_tx.c:229-230` | `tbt_tx_control_pinned` | 同一槽读两次，第二次直接 `->gen`；槽可为 NULL |
| `tbt_tx.c:427-433` | `tbt_retain_all_received` | `c->gen` + `c->uack`，`tx_lock`/`pump_lock` |
| `tbt_tx.c:1129-1135` | `tbt_retain_received` | `c->gen` + `c->uack`，`tx_lock` |
| `tbt_tx.c:1165-1169` | `tbt_retain_tcp_done` | `c->gen` + `c->sk`，`tx_lock` |
| `tbt_tx.c:1197-1201` | `tbt_retain_paths_stalled` | `c->gen` + `last_una_move_ns`，`tx_lock` |
| `tbt_tx.c:1227-1231` | `tbt_hole_copy_inflight` | `c->gen` + `c->uack`，`tx_lock` |
| `tbt_tx.c:1339-1341` | `tbt_tx_hole_check` | `c->up`、`c->rtt_us`（`tbt_path_rtt_us` 内再读 `c->sk`） |
| `tbt_tx.c:1375-1390` | `tbt_tx_hole_check` | `c->state/gen/data_frames/recv_cnt/uack`，写 `recv_lag/loss_ppm` |
| `tbt_tx.c:1744-1745` | `tbt_tx_ack` | `c->gen` + `r->path_gen[c->index]`；`c->index` 无 `<16` 检查 |
| `tbt_tx.c:2229/2271` | `tbt_tx_pump` RETIRE 改道 | `READ_ONCE(sess->car[alt])->gen`，同一表达式二次读 |
| `tbt_carrier.c:2337/2342` | `tbt_tx_hello` | `hc->gen/profile`（诊断） |
| `tbt_carrier.c:2351-2353` | `tbt_tx_hello` | **裸读 4 次** `sess->car[index]` 取 `profile/gen`；判空与解引用之间槽可变 NULL |
| `tbt_rx.c:387-390` | `tbt_rx_fbk` | `pc->gen` 后写 `uack/uack_ns/recv_cnt/rtt_*` |
| `tbt_session.c:1317-1319` | `tbt_session_retire` | `sess->lock` 下，只读 `gen`/写 `stopping` |
| `tbt_session.c:1367` | `tbt_session_join` | `sess->lock` 下比较 `peer_gen` |

### 2.1.3 解引用 `c->profile` 的点

| 文件:行 | 函数 | 说明 |
|---|---|---|
| `tbt_carrier.c:2337/2342` | `tbt_tx_hello` | `hc ? hc->profile : cfg.profile`（诊断） |
| `tbt_carrier.c:2351-2353` | `tbt_tx_hello` | body[22] 的 `profile`，裸读 |
| `tbt_carrier.c:489/494/1021/1097` | `tbt_car_poll_one` / `send_fec` / `send_whole` | `c` 是参数，不是 `car[]` 读点；但同样需要生命周期契约 |
| `tbt_rx.c:1140`、`tbt_session.c:475` | `tbt_rx_*` / `peers_show` | `c` 是参数 |

### 2.1.4 三类真实缺陷

1. **同槽多次读**：`tbt_tx.c:229-230`、`tbt_carrier.c:2351/2353` 判空后再次解引用；槽可在两次读之间变 NULL 或被替换。
2. **`c->sk` 悬垂**：`tbt_carrier_release()`（`tbt_carrier.c:1476-1527`）先 `sock_release(s)`，再 `c->sk = NULL`（`1507-1509`），普通 store、无 `synchronize_rcu`。所有无 `sock_hold` 的读者都能拿到已释放的 `struct sock`。
3. **对象复用造成身份错配**：`tbt_carrier_take_dead()`（`tbt_carrier.c:1281-1300`）在 `released + pollers==0 + 四个 work 不忙` 时把对象 `memset` 后复用；读者拿到的旧指针会突然变成另一个 carrier（`gen/profile/index` 全变）。`pollers` 只保护 poll，不保护 TX/RX/procfs。

关键事实：**`pump_lock` 不是 `sess->car[]` 的独占锁**。`tbt_session_join()`（`tbt_session.c:1390`）只取 `sess->lock`，不取 `pump_lock`；`tbt_replace_work()` 的 `dead_release` 批次在 `pump_lock` 释放之后才 `tbt_carrier_release()`（`tbt_carrier.c:1940` 解锁 → `1975-1990`）。因此“pump 内读 `car[]`”仍可与 join 换槽、旧 socket 释放并发。

## 2.2 设计：refcount + RCU，统一一套生命周期

### 2.2.1 结构字段（`tbtun.h:997`）

```c
struct tbt_carrier
{
    ...
    refcount_t refs;              /* 1 = slot/dead owner；读者各持一份 */
    struct rcu_head rcu;          /* 最后一个 put 之后 kfree_rcu */
    struct sock __rcu *sk;        /* RCU 发布；release 先清空再 sock_release */
    ...
};
```

`sess->car[]`：

```c
struct tbt_carrier __rcu *car[TBT_MAX_CARRIERS_LIMIT];
```

`refs` 语义：

- `tbt_carrier_new()` 成功时 `refcount_set(&c->refs, 1)`：这是“安装者引用”。
- 对象进入 `sess->car[]` 时，slot **不额外**持引用；安装者引用代表“对象在 slot 或 dead 列表”。
- 读者通过 `tbt_carrier_get()` 取一份临时引用。
- slot 替换：`rcu_assign_pointer()` 换指针，旧对象的安装者引用在 release 完成后由 `tbt_carrier_put()` 释放；`refs` 归零时 `kfree_rcu()`。
- 删除 `tbt_carrier_take_dead()` 的复用：`tbt_carrier_new_locked()` 总是 `kzalloc()`。这样 `gen/profile/index` 永远不会被 `memset` 改写，旧读者即使持有旧对象也只看到 `released` 状态，不会看到“另一个 carrier”。
- `n_dead` / `dead_peak` / `2*n_car` 的 `-ENOSPC` 限制可以删除：`sess->dead` 只保留“已从 slot 摘下、等待 release”的对象，release 完成后立即 `put`，不再累积。

### 2.2.2 统一 helper（`tbt_carrier.c` 实现，`tbtun.h` 声明）

```c
/* 取得一份 carrier 引用；返回 NULL 表示槽为空或对象正在释放。
 * 不取 sess->lock；内部只用 RCU + refcount，可在 tx_lock 下调用。 */
struct tbt_carrier *tbt_carrier_get(struct tbt_session *sess, u8 index);

/* 释放一份引用；最后一个引用触发 kfree_rcu()。 */
void tbt_carrier_put(struct tbt_carrier *c);

/* 只取 gen 的快捷方式；等价于 get + READ_ONCE(c->gen) + put。 */
u32 tbt_carrier_gen(struct tbt_session *sess, u8 index);

/* 短读 c->sk：进入 RCU 读临界区，返回的 sk 在 sk_put 前有效。
 * 临界区内不得睡眠、不得 lock_sock。 */
struct sock *tbt_carrier_sk_get(struct tbt_carrier *c);
void tbt_carrier_sk_put(struct sock *sk);

/* 长用 c->sk：在 RCU 临界区内 sock_hold()，返回后可以睡眠/加锁；
 * 用完调用 sk_drop()。 */
struct sock *tbt_carrier_sk_hold(struct tbt_carrier *c);
void tbt_carrier_sk_drop(struct sock *sk);
```

实现：

```c
struct tbt_carrier *tbt_carrier_get(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c;

    if (!sess || index >= READ_ONCE(sess->n_car))
        return NULL;
    rcu_read_lock();
    c = rcu_dereference(sess->car[index]);
    if (c && !refcount_inc_not_zero(&c->refs))
        c = NULL;
    rcu_read_unlock();
    return c;
}

void tbt_carrier_put(struct tbt_carrier *c)
{
    if (c && refcount_dec_and_test(&c->refs))
        call_rcu(&c->rcu, tbt_carrier_free_rcu);
}

u32 tbt_carrier_gen(struct tbt_session *sess, u8 index)
{
    struct tbt_carrier *c = tbt_carrier_get(sess, index);
    u32 gen = c ? READ_ONCE(c->gen) : 0;

    tbt_carrier_put(c);
    return gen;
}

struct sock *tbt_carrier_sk_get(struct tbt_carrier *c)
{
    rcu_read_lock();
    return c ? rcu_dereference(c->sk) : NULL;
}

void tbt_carrier_sk_put(struct sock *sk)
{
    (void)sk;
    rcu_read_unlock();
}

struct sock *tbt_carrier_sk_hold(struct tbt_carrier *c)
{
    struct sock *sk;

    rcu_read_lock();
    sk = c ? rcu_dereference(c->sk) : NULL;
    if (sk)
        sock_hold(sk);
    rcu_read_unlock();
    return sk;
}

void tbt_carrier_sk_drop(struct sock *sk)
{
    if (sk)
        sock_put(sk);
}
```

**与生命周期组对齐**：TX 侧不定义自己的 `READ_ONCE(c->sk)`，只调用上述四个 API。若生命周期组最终命名不同（例如 `tbt_car_sk_get`），只改这四个名字，语义与调用点不变。`sess->car[]` 的 RCU 与 `c->sk` 的 RCU 使用同一个 `rcu_read_lock` 域，可以嵌套；不存在两套机制。

### 2.2.3 `tbt_carrier_release()` 的新顺序

```c
static void tbt_carrier_release(struct tbt_session *sess, struct tbt_carrier *c)
{
    struct socket *s;

    if (!c)
        return;

    /* 与 tbt_carrier_arm_tx()/arm_pump() 的 schedule 互斥：
     * 锁内设置 stopping，之后任何 arm 都不会再排队；
     * 锁外 cancel 能捕获设置之前排队的 work。 */
    spin_lock(&sess->lock);
    atomic_set(&c->stopping, 1);
    spin_unlock(&sess->lock);

    cancel_work_sync(&c->w_connect);
    cancel_work_sync(&c->w_rx);

    s = c->sock;
    if (s)
    {
        if (s->sk)
        {
            /* 先撤掉读者的入口，再等所有已进入的读者退出。 */
            rcu_assign_pointer(c->sk, NULL);
            synchronize_rcu();

            tbt_sk_ops_restore(s->sk, &sess->def_ops);
            sock_replace_proto(s->sk, &tcp_prot);
            cancel_work_sync(&c->w_rx);
        }
        tbt_sock_abort_on_close(s);
        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        c->sock = NULL;
    }
    atomic_set(&c->connected, 0);
    atomic_set(&c->up, 0);
    ...
    c->released = true;
}
```

`w_tx` / `w_pump` 仍由调用者在 `release` 之前取消（`tbt_carriers_stop` 与 `tbt_replace_work` 已这样做），因为它们在运行时可能等待 `pump_lock`，而 `release` 可能被持 `pump_lock` 的调用者调用。新增 `tbt_carrier_quiesce(c)` 封装“设 stopping + cancel 四个 work”，调用者顺序固定为：

```
tbt_carrier_quiesce(c);       /* 可睡眠，不持 tx_lock */
tbt_carrier_release(sess, c); /* socket/parser，可睡眠 */
tbt_carrier_put(c);           /* 释放安装者引用 */
```

### 2.2.4 消除 work 调度 race

仅靠 `cancel_work_sync()` 不够：`cancel` 返回后，一个已经通过 `stopping` 检查的读者仍可能 `schedule_work(&c->w_tx)`。把“检查 + schedule”放进 `sess->lock`，与 release 的“设置 stopping”互斥：

```c
bool tbt_carrier_arm_tx(struct tbt_carrier *c)
{
    bool queued = false;

    if (!c)
        return false;
    spin_lock(&c->sess->lock);
    if (!atomic_read(&c->stopping) && !c->released)
        queued = schedule_work(&c->w_tx);
    spin_unlock(&c->sess->lock);
    return queued;
}

bool tbt_carrier_arm_pump(struct tbt_carrier *c, unsigned int ms)
{
    if (!c)
        return false;
    spin_lock(&c->sess->lock);
    if (!atomic_read(&c->stopping) && !c->released)
        mod_delayed_work(TBT_WQ, &c->w_pump, msecs_to_jiffies(ms));
    spin_unlock(&c->sess->lock);
    return true;
}
```

证明：若 arm 在 release 的 `spin_lock` 之前进入，它先 schedule，随后 release 设置 stopping 并 `cancel_work_sync()`，能取消它；若 arm 在 release 之后进入，它看到 `stopping=1` 或 `released=true`，不 schedule。因此 `cancel` 之后不会再有新的 work 排队，对象可以安全 `kfree_rcu`。

所有 `schedule_work(&c->w_tx)` / `schedule_delayed_work(&c->w_pump)` / `mod_delayed_work(..., &c->w_pump, ...)` 点必须改走这两个 helper：`tbt_tx.c:149`、`tbt_tx.c:1957`、`tbt_tx.c:2441`、`tbt_carrier.c:154/535`；`w_rx` 用 `tbt_carrier_arm_rx`（同样在 `sess->lock` 下检查）。

### 2.2.5 读点迁移映射

**只取 `gen` 的点（`tbt_carrier_gen()` 或一次 get/put）**

- `tbt_tx.c:229-230`：`fr->gen = tbt_carrier_gen(sess, path);`，删掉两次 `READ_ONCE`。
- `tbt_tx.c:2229/2271`：`fr->gen = tbt_carrier_gen(sess, alt);`。
- `tbt_carrier.c:2351-2353`：一次 `tbt_carrier_get(sess, index)`，读 `profile` 与 `gen`，再 `put`；无 carrier 时用 `sess->tunnel->cfg.profile`、`gen=0`。
- `tbt_session.c:1317-1319`、`1367`：已在 `sess->lock` 下，保留裸读。

**必须持 carrier 引用（get/put 跨整个使用段）**

- `tbt_tx_wake`（`145`）：get → `tbt_carrier_arm_tx` → put。
- `tbt_tx_announce`（`304`）/ `tbt_tx_announce_carrier`：调用者 get，函数内使用 `connected/hello_sent/stopping/index/gen/up`，返回前 put。
- `tbt_tx_fin`（`343`）/ `tbt_tx_rst`（`385`）：get 每个槽，读 `hello_sent/stopping`，入队 pinned 帧，put。
- `tbt_frame_carrier`（`178`）：改为返回 **get 后的引用**，函数名 `tbt_frame_carrier_get`，调用者负责 put。
- `tbt_tx_pump` 的 pinned 分支与 `tbt_sched_pick`：`tbt_sched_pick` 返回 get 后的引用，pump 在写入完成 / `tbt_frame_free(fr)` 后 put。`tbt_sched_rebuild` / `tbt_sched_better` 的临时读用 get/put。
- `tbt_tx_ctl_arm`（`1950`）：get 候选，`tbt_carrier_arm_pump`，put。
- `tbt_tx_retire_alt`（`2060`）：get 每个候选，读 `stopping/up`，put；返回索引。
- `tbt_tx_fbk`（`3010`、`3054`）：见第 4 节。
- `tbt_tx_bdp_window`（`596`）/ `tbt_tx_rto_ns`（`734`）/ `tbt_path_stalled_mask`（`1819`）/ `tbt_path_rtt_us`（`534`）：get + `sk_get`/`sk_put`，见第 4 节。
- `tbt_retain_all_received`（`427`）、`tbt_retain_received`（`1129`）、`tbt_retain_tcp_done`（`1165`）、`tbt_retain_paths_stalled`（`1197`）、`tbt_hole_copy_inflight`（`1227`）、`tbt_tx_hole_check`（`1339/1375/1501`）：在 `tx_lock` 内循环 get/put；`tbt_path_rtt_us` 用 `sk_get`/`sk_put`。这些函数只在 legacy ARQ 路径，`n_car<=16`，锁内 16 次 refcount 可接受。
- `tbt_tx_ack`（`1744`）：入口用 `tbt_carrier_get(sess, c->index)` 校验 `got == c`；不匹配则把 `c` 置 NULL（只丢 RTT 样本，不影响 ack 推进），函数末尾 put。
- `tbt_rx_fbk`（`387`）：get `pc`，校验 `gen`，写 `uack/uack_ns/recv_cnt/rtt_*`，put。
- `tbt_fec_out_try`（`153/174/207`）：get `c`，`tbt_carrier_send_fec` 使用期间持引用，put。
- `tbt_carrier_resume`（`2377`）：`sess->lock` 下 get，锁外 `tbt_car_schedule_rx`，put；或把 `tbt_car_schedule_rx` 改为内部 `arm_rx` helper。
- `tbt_carrier_snapshot`（`2401`）：get，`sk_hold`（可睡眠路径用 hold/drop），读完 put。
- `tbt_session.c:383`（peers）：get，`sk_hold` 后读，`sk_drop`，put。
- `tbt_session.c:531`（action）：get，调用 `tbt_carrier_lost`，put。
- `tbt_procfs.c:720/980`：get，读 `up/tx_bytes/...`，put；纯统计可接受短暂不一致，但不能悬垂。
- `tbt_tx_hello`（`2337/2351`）：见“只取 gen”。

**可以保留裸读的点**

- `tbt_carriers_start`（`1663`）：会话创建期、`w_poll` 未 arm、`tbt_lock` 下，无并发。
- `tbt_carriers_stop`（`2023/2040/2042`）：teardown，`w_poll`/`w_fbk`/`w_replace` 已取消，且之后 `synchronize_rcu`。
- `tbt_session_poll_work`（`580`）：`sess->lock` 下读；把 `pollers++` 改成 `tbt_carrier_get`（锁内 refcount_inc 安全），锁外使用后 put；删除 `pollers` 或保留为诊断。
- `tbt_replace_work` 的换槽读（`1780/1814/1840/1884/1908`）：已在 `sess->lock` 下，保留；写入改为 `rcu_assign_pointer`。

### 2.2.6 锁顺序

| 操作 | 允许的锁 | 禁止 |
|---|---|---|
| `tbt_carrier_get` | 任意（RCU + refcount，不睡眠） | 无 |
| `tbt_carrier_put` | 任意（`call_rcu` 不睡眠） | put 后继续使用 `c` |
| `tbt_carrier_sk_get/put` | 任意，但临界区内不得睡眠 | `lock_sock`、`sock_release`、`mutex_lock` |
| `tbt_carrier_sk_hold/drop` | hold 内不睡眠；drop 任意 | 无 |
| `tbt_carrier_arm_tx/pump` | 内部取 `sess->lock`（自旋） | 不得在 `tx_lock` 下调用（锁序 `sess->lock` 在外层） |
| `tbt_carrier_release` | 可睡眠；先 `sess->lock` 设 stopping，锁外 cancel + `synchronize_rcu` | 不得在 `tx_lock` 下调用 |
| slot 写入 | `sess->lock` 内 `rcu_assign_pointer` | 不得在 `tx_lock` 下取 `sess->lock` |

锁序固定为：`pump_lock` → `sess->lock` → （释放）→ `cancel_work_sync` / `synchronize_rcu`。`tx_lock` 与 `sess->lock` 不嵌套：需要 car 的 tx 路径先 get，再取 `tx_lock`。

### 2.2.7 失败路径

- `tbt_carrier_get()` 返回 NULL：调用者跳过该槽；不得把 NULL 传给除 `tbt_carrier_put` 之外的对象。
- `refcount_inc_not_zero()` 失败：对象正在释放，返回 NULL；正常竞态，不计数、不 WARN。
- `tbt_carrier_sk_get()` 返回 NULL：跳过 sk 相关计算；carrier 的标量字段仍可用。
- `tbt_carrier_sk_hold()` 返回 NULL：调用者放弃本次 socket 操作（poll/send 返回 `-ENOTCONN` 或跳过）。
- `tbt_carrier_arm_tx()` 返回 false：work 已在运行或 carrier 已 stopping；调用者依赖 fbk tick / replacement，不重试。
- `tbt_carrier_release()` 中 `synchronize_rcu()` 无失败；若 `s->sk` 已为 NULL，跳过 RCU 清空。
- `kfree_rcu` 回调只释放内存，不取任何锁；`tbt_carrier_free()` 必须幂等（`list_del_init` 已是幂等）。

## 2.3 迁移步骤

**S2.1 基础设施（不改调用者）**
1. `tbtun.h`：加 `refcount_t refs`、`struct rcu_head rcu`、`struct sock __rcu *sk`；`car[]` 加 `__rcu`。
2. `tbt_carrier.c`：实现 `tbt_carrier_get/put/gen/sk_get/sk_put/sk_hold/sk_drop`；`tbt_carrier_free_rcu()` 调用现有 `tbt_carrier_free()`。
3. `tbt_carrier_new_locked()`：`refcount_set(&c->refs, 1)`；删除 `tbt_carrier_take_dead()` 的调用，改为总是 `kzalloc`。
4. `tbt_carrier_release()`：按 2.2.3 改顺序；`c->sk` 用 `rcu_assign_pointer`。
5. 所有 slot 写入改 `rcu_assign_pointer`；所有 slot 读取在基础设施内部用 `rcu_dereference`。
6. 删除 `n_dead >= 2*n_car` 的 `-ENOSPC`（`tbt_session.c:1386-1389`）或改为仅诊断。

此步完成后，旧调用者仍用 `READ_ONCE(sess->car[i])`，行为不变；但对象不再被复用，`c->sk` 的释放顺序已安全。

**S2.2 TX 高风险点**
1. `tbt_tx_fbk`、`tbt_tx_rto_ns`、`tbt_tx_bdp_window`、`tbt_path_rtt_us`、`tbt_path_stalled_mask`（第 4 节）。
2. `tbt_tx_wake`、`tbt_tx_ctl_arm`、pump 尾部定时器（`2797`）。
3. `tbt_tx_pump` 的 `tbt_sched_pick` / `tbt_frame_carrier` 契约改为返回引用。
4. `tbt_tx_submit` 不直接读 `car[]`，无需改（第 1 节）。

**S2.3 RX / FEC / procfs / session**
1. `tbt_rx_fbk`（`387`）。
2. `tbt_fec_out_try`（`153/174/207`）。
3. `tbt_carrier_resume`、`tbt_carrier_snapshot`。
4. `tbt_session_peers_show`、`tbt_action_apply`。
5. `tbt_procfs.c:720/980`。

**S2.4 清理**
1. 删除 `atomic_t pollers` 或只保留诊断；poll 改用 get/put。
2. 删除 `tbt_carrier_take_dead()`、`n_dead` 的复用语义；`dead` 列表只用于待 release。
3. 全仓 `grep 'READ_ONCE(.*car\['` 应为 0（teardown 例外加注释）。
4. 更新 `tbtun.h:701-706` 的注释：不再复用，refcount 归零后 `kfree_rcu`。

## 2.4 验证

**host**
- `make host-test`：确保头文件改动不破坏 host 编译。
- `make C=1` / `sparse`：检查 `__rcu` 注解与 `rcu_dereference` 配对；裸读 `sess->car[]` 应报 `incorrect type`。
- 新增 `host/test_carrier_life.c`（建议）：用用户态 stub 模拟 `refcount + call_rcu`，断言 get/put 配对、最后一个 put 只触发一次 free、`refcount_inc_not_zero` 在 0 时返回 NULL。

**QEMU**
- `tests/qemu/run.sh --probes R`（link-flap-then-replace）是核心：替换期间 pump/fbk/rx 同时运行，KASAN 下不应出现 `struct sock` UAF。
- 全量 `A..R` 回归；`--allow-skip` 只用于已知环境缺失。
- guest 内并发读 `/proc/net/tbtun/peers`、`/proc/net/tbtun/stats`，同时用 `echo down > /proc/.../carrier` 触发替换；KCSAN 内核跑 10 分钟。

**真机**
- `review/wan-experiment/arm64/prod-smoke-10s.sh`；`controlled-c8-once.sh`。
- `/proc/net/tbtun/stats`：`carrier_replaced` 增加时 `join_dup=0`，`carrier_lost` 不异常；无 `BUG: KASAN: use-after-free in tcp_sk` / `tbt_tx_fbk`。
- `perf top` 确认 refcount/RCU 没有进入 TX 热路径前 10；若 `tbt_carrier_get` 出现在 `tbt_retain_*` 热点，考虑把 16 个 carrier 快照到 pump 局部数组，一次 get 整个 pass。
- 长稳 24h：`n=4,m=2`，每 30s 强制替换一个 carrier；`slabinfo` 中 `tbt_carrier` 不增长。

---

# 3. L3：TX-6 16 槽数组

## 3.1 现状（文件:行）

数组定义在 `tbtun.h:355-363`：

```c
u8 path_gen[16];     /* generation of the carrier that wrote each bit */
u64 path_end[16];    /* end offset in that path's payload-byte space */
u32 first_sent_seq[16];
u64 una_observed_ns[16];
```

写点：

| 位置 | 代码 | 检查 |
|---|---|---|
| `tbt_tx.c:2721-2726` | `path_mask/reserve_mask/path_gen[c->index]` | 有 `c->index < 16` |
| `tbt_tx.c:2734-2737` | `first_sent_seq[c->index]` | 有 `c->index < 16` |
| `tbt_tx.c:2738` | `r->una_observed_ns[c->index] = 0;` | **无** |
| `tbt_tx.c:2775-2776` | `r->path_end[c->index] = c->payload_bytes_sent;` | **只有 `if (r)`，无索引检查，且在 `tx_lock` 外** |
| `tbt_tx.c:1175-1176` | `r->una_observed_ns[i]` | 循环 `i < sess->n_car`，无 `<16` |

读点（全部以 `sess->n_car` 为上界，无 `<16`）：`tbt_retain_all_received`（`425-434`）、`tbt_retain_received`（`1127-1143`）、`tbt_retain_tcp_done`（`1163-1179`）、`tbt_retain_paths_stalled`（`1195-1201`）、`tbt_hole_copy_inflight`（`1225-1233`）、`tbt_tx_hole_check`（`1373-1394`、`1412/1420`）、`tbt_tx_ack`（`1744-1749`）。

当前不可达越界的唯一原因是配置链：`tbt_parse.c:663-668` 与 `tbt_session.c:128-141` 保证 `!fec ⇒ n_car<=16`；`sess->fec` 为真时 `tbt_tx_fbk()` 直接返回，legacy retain 也不会创建。**但写点自身没有守卫**：`path_end[16]` 会落进 `first_sent_seq[0]`，`una_observed_ns[16]` 会落进 `last_path/sends/dup_pending/pending/retx_now/declared`。历史上前一次同类越界（`announced_gen[c->index]` 覆盖 `tx_paused`）已经造成过真机 UBSAN trace（HND0 far journal），所以不能依赖配置链。

FBK 侧：`tbt_tx_fbk()` 的循环上界是 `TBT_MAX_CARRIERS_LIMIT`（256），而 `body[TBT_FBK_MAX]` 只有 16 项（288 字节）。`nr < TBT_MAX_CARRIERS_LIMIT` 恒真；若 `n_car>16` 且 `!fec`（配置链失效），会直接写穿栈。`mask` 是 `u16`，`BIT(i)` 在 `i>=16` 时静默别名。

## 3.2 设计：统一索引 helper

```c
/* tbt_tx.c 顶部；语义与 TBT_LEGACY_MAX_CARRIERS 绑定，不新造第二个上限。 */
#define TBT_ARQ_MAX_PATHS TBT_LEGACY_MAX_CARRIERS

/* 纯边界判断，不 WARN：调用者可能合法地遍历 FEC 会话的 256 个槽。 */
static inline bool tbt_arq_path_ok(const struct tbt_session *sess,
                                   unsigned int idx)
{
    return idx < TBT_ARQ_MAX_PATHS && idx < READ_ONCE(sess->n_car);
}

/* 16 槽 retain 数组的索引。任何越界都是代码错误，WARN 一次并拒绝写入。 */
static inline bool tbt_retain_path_ok(struct tbt_session *sess,
                                      unsigned int idx)
{
    if (WARN_ON_ONCE(idx >= TBT_ARQ_MAX_PATHS))
        return false;
    return idx < READ_ONCE(sess->n_car);
}
```

`WARN_ON_ONCE` 放在 `tbt_retain_path_ok()`，而不是每个写点；只有真正进入 16 槽数组的路径才会触发，FEC 的 256 槽路径不会被误报。

**写点**

`tbt_tx.c:2738`：

```c
if (tbt_retain_path_ok(sess, c->index))
    r->una_observed_ns[c->index] = 0;
```

`tbt_tx.c:2775-2776`：移入 `tx_lock` 临界区，与 `path_mask` 更新同锁；`path_end` 的读者在 `tx_lock` 下：

```c
spin_lock(&sess->tx_lock);
if (r && tbt_retain_path_ok(sess, c->index))
    r->path_end[c->index] = c->payload_bytes_sent;
spin_unlock(&sess->tx_lock);
```

`tbt_tx.c:1175`：循环上界改为 `min(sess->n_car, TBT_ARQ_MAX_PATHS)`，写前 `tbt_retain_path_ok`。

`tbt_tx.c:1744-1749`（`tbt_tx_ack`）：

```c
if (c && tbt_retain_path_ok(sess, c->index) &&
    (r->path_mask & BIT(c->index)) &&
    (c->gen & 0xf) == r->path_gen[c->index])
    i = c->index;
else if (r->last_path < TBT_ARQ_MAX_PATHS &&
         r->last_path < sess->n_car)
    i = r->last_path;
```

**读点**：所有访问 `r->path_*[i]` 的循环统一改为：

```c
for (i = 0; i < sess->n_car && i < TBT_ARQ_MAX_PATHS; i++)
```

涉及 `tbt_retain_all_received`、`tbt_retain_received`、`tbt_retain_tcp_done`、`tbt_retain_paths_stalled`、`tbt_hole_copy_inflight`、`tbt_tx_hole_check`（两处）、`tbt_tx_ack`（`last_path` 分支）。这些函数只在 legacy ARQ 路径，`n_car<=16`，改动不改变行为。

**FBK 循环**：

```c
for (i = 0; i < sess->n_car && i < TBT_LEGACY_MAX_CARRIERS &&
            nr < TBT_LEGACY_MAX_CARRIERS; i++)
```

- `body` 仍是 `u8 body[TBT_FBK_MAX]`；`nr*TBT_FBK_ENTRY_LEN <= 288`。
- `mask` 只在 `i < 16` 时 `mask |= BIT(i)`。
- 入口加配置断言：

```c
if (WARN_ON_ONCE(!sess->fec && sess->n_car > TBT_LEGACY_MAX_CARRIERS))
    return -EINVAL;
```

- 发布前 `WARN_ON_ONCE(nr > U8_MAX)`（`path_ack_count` 是 u8）。

**断言位置**

- `tbt_retain_path_ok()`：所有 16 槽写点。
- `tbt_tx_fbk()` 入口：配置链断言。
- `tbt_tx_pump()` 的 DATA 写成功分支：在 `if (r)` 内先 `WARN_ON_ONCE(!tbt_arq_path_ok(sess, c->index))`；若触发，跳过所有 `r->path_*` 写入，但仍 `c->payload_bytes_sent += fr->len`。
- `tbt_tx_ack()` 入口：`WARN_ON_ONCE(c->index >= TBT_MAX_CARRIERS_LIMIT)`（`c->index` 是 u8，最大 255，不会越界；这个断言只防 `index` 字段损坏）。

## 3.3 迁移步骤

1. 加 `TBT_ARQ_MAX_PATHS` 与两个 helper；不改调用点，编译通过。
2. 改 `tbt_tx.c:2738` 与 `2775-2776` 两个写点；`2775` 移入 `tx_lock`。这是唯一的行为变化（`path_end` 现在与 `path_mask` 同锁），单独验证。
3. 改所有 retain 读循环上界；机械替换，逐函数 review。
4. 改 `tbt_tx_fbk` 循环上界与 `mask` 条件。
5. 加 `WARN_ON_ONCE`；`debug=0` 下 WARN 仍会打印一次，确认没有正常路径触发。
6. 保留 `path_gen`/`first_sent_seq` 已有检查，与新 helper 不冲突。

## 3.4 验证

**host**
- `make host-test`；加 `BUILD_BUG_ON(TBT_FBK_MAX != TBT_FBK_ENTRY_LEN * TBT_LEGACY_MAX_CARRIERS)`。
- 新增 `host/test_tx_index.c`（建议）：把 `tbt_arq_path_ok` / `tbt_retain_path_ok` 抽成纯函数，断言 `idx=15` 通过、`idx=16` 在 `WARN_ON_ONCE` 下返回 false、`idx>=n_car` 返回 false。

**QEMU**
- `n=16,m=0,fec=off`：跑满 16 槽，`debug=2` 确认无 WARN。
- `n=16,m=16,fec=off` 应被配置拒绝（`tbt_parse.c:663-668`），确认错误信息。
- `fec=on,n=256`：确认 `tbt_tx_fbk()` 直接返回 0，不进入 legacy 循环；KASAN 无栈越界。
- probe I/J：`path_end` 更新在 `tx_lock` 内，确认 `retx_evident`/`retx_frames` 不退化。

**真机**
- `n=16` 大流量 + `dup=2`，`debug=2`；`WARN_ON_ONCE` 应为 0。
- 加 `BUILD_BUG_ON(offsetof(struct tbt_retain, first_sent_seq) - offsetof(struct tbt_retain, path_end) != sizeof(((struct tbt_retain *)0)->path_end))` 静态断言。
- 回归：`n=1..16` 的 `path_mask`/`path_end` 统计与 0.1.x 一致。

---

# 4. L4：`tbt_tx_fbk()` 与 `c->sk` RCU helper 的配合

## 4.1 现状（文件:行）

`tbt_tx_fbk()`：`tbt_tx.c:2997-3092`。

```c
/* tbt_tx.c:3008-3036 */
for (i = 0; i < sess->n_car && nr < TBT_MAX_CARRIERS_LIMIT; i++)
{
    struct tbt_carrier *c = READ_ONCE(sess->car[i]);
    struct sock *sk;
    u8 *e;
    u32 rtt = 0;

    if (!c || !atomic_read(&c->up))
        continue;
    /* One snapshot: tbt_carrier_release() clears c->sk with a plain
     * store, so a second read after the check can be NULL. */
    sk = READ_ONCE(c->sk);
    if (sk)
        rtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
    WRITE_ONCE(c->rx_state, state);
    e = body + nr * TBT_FBK_ENTRY_LEN;
    e[TBT_FBK_PATH] = (u8)i | ((c->peer_gen & 0xf) << 4);
    ...
    mask |= BIT(i);
    nr++;
}
```

钉 spare（`3052-3062`）：

```c
for (i = 0; i < sess->n_car; i++)
{
    struct tbt_carrier *sp = READ_ONCE(sess->car[i]);

    if (sp && sp->spare && atomic_read(&sp->up) &&
        !atomic_read(&sp->stopping))
    {
        fr->pinned = true;
        fr->path = (u16)i;
        fr->gen = sp->gen;
        break;
    }
}
```

`tbt_tx_rto_ns()`（`727-772`）：

```c
for (i = 0; i < sess->n_car; i++)
{
    struct tbt_carrier *c = READ_ONCE(sess->car[i]);
    u32 srtt, mdev, r;

    if (!c || !c->sk)
        continue;
    srtt = READ_ONCE(tcp_sk(c->sk)->srtt_us) >> 3;
    mdev = READ_ONCE(tcp_sk(c->sk)->mdev_us);
    ...
}
```

`tbt_tx_bdp_window()`（`594-612`）：

```c
struct tbt_carrier *c = READ_ONCE(sess->car[i]);
...
if (!c || !c->sk)
    continue;
tp = tcp_sk(c->sk);
srtt = READ_ONCE(tp->srtt_us) >> 3;
mdev = READ_ONCE(tp->mdev_us);
```

`tbt_path_rtt_us()`（`534-541`）：

```c
u32 rtt = READ_ONCE(c->rtt_us);
u32 srtt = 0;

if (c->sk)
    srtt = READ_ONCE(tcp_sk(c->sk)->srtt_us) >> 3;
```

共同问题：

- 无 carrier 引用；`sess->car[i]` 可被 join 换槽，旧对象可被 `release`。
- `c->sk` 先判空、后解引用；`release` 先 `sock_release()` 再清 `c->sk`，窗口内 `tcp_sk(sk)` 是 UAF。
- `rto`/`bdp` 多次读 `c->sk`，每次普通读，可能一次非空一次 NULL。
- `tbt_tx_fbk()` 是 `TBT_WQ` 的 delayed work（`tbt_fbk_work`，`3135`），进程上下文、`GFP_KERNEL`，不持 `pump_lock`；与 `w_replace` 的 release 完全并发。
- `tbt_tx_rto_ns()` 还会从 `tbt_tx_wait_budget_ns()`（`1545`）被 `tbt_tx_stall_check()`/`tbt_tx_close_check()` 以及 `tbt_rx_deliver_stall_check()`（`tbt_rx.c:1352`）调用，这些路径也不持 `pump_lock`。

## 4.2 设计

### 4.2.1 统一快照模板

```c
for (i = 0; i < sess->n_car; i++)
{
    struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
    struct sock *sk;

    if (!c)
        continue;

    sk = tbt_carrier_sk_get(c);      /* rcu_read_lock */
    if (sk)
    {
        /* 只读 tcp_sock 的标量；不 lock_sock、不睡眠。 */
        ...
    }
    tbt_carrier_sk_put(sk);          /* rcu_read_unlock */

    /* 继续读 c 的标量字段（gen/up/peer_gen/rx_bytes/...） */
    ...
    tbt_carrier_put(c);
}
```

规则：

- **短读**（只读 `tcp_sock` 字段、`sk_state`、`sk_user_data`）：`sk_get`/`sk_put`。
- **长用**（`lock_sock`、`tcp_sendmsg`、`sock_hold`、`kernel_sock_shutdown`）：`sk_hold`/`sk_drop`。
- **不在 RCU 临界区内睡眠**：`WRITE_ONCE(c->rx_state, state)` 与 `body` 写入放在 `sk_put` 之后。
- **不在 RCU 临界区内 `tbt_carrier_put`**：先 `sk_put`，再 `c_put`。

### 4.2.2 `tbt_tx_fbk()` 新伪代码

```c
int tbt_tx_fbk(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
    if (sess->fec)
        return 0;
    if (WARN_ON_ONCE(!sess->fec &&
                     sess->n_car > TBT_LEGACY_MAX_CARRIERS))
        return -EINVAL;

    u8 body[TBT_FBK_MAX];
    u32 nr = 0;
    u16 mask = 0;
    u8 state = tbt_rx_state(st);
    struct tbt_frame *fr;
    int i;

    for (i = 0; i < sess->n_car && i < TBT_LEGACY_MAX_CARRIERS; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        u8 *e;
        u32 rtt = 0;

        if (!c)
            continue;
        if (!atomic_read(&c->up))
        {
            tbt_carrier_put(c);
            continue;
        }

        sk = tbt_carrier_sk_get(c);
        if (sk)
            rtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
        tbt_carrier_sk_put(sk);

        WRITE_ONCE(c->rx_state, state);
        e = body + nr * TBT_FBK_ENTRY_LEN;
        e[TBT_FBK_PATH] = (u8)i | ((READ_ONCE(c->peer_gen) & 0xf) << 4);
        e[TBT_FBK_RXSTATE] = state;
        tbt_put_le64(e + TBT_FBK_UACK, READ_ONCE(c->rx_bytes));
        tbt_put_le32(e + TBT_FBK_RECVCNT, (u32)READ_ONCE(c->rx_frames));
        tbt_put_le32(e + TBT_FBK_RTT, rtt);
        mask |= BIT(i);
        nr++;
        tbt_carrier_put(c);
    }
    if (!nr)
        return 0;

    fr = tbt_frame_alloc(nr * TBT_FBK_ENTRY_LEN, gfp);
    if (!fr)
        return -ENOMEM;
    memcpy(fr->data, body, nr * TBT_FBK_ENTRY_LEN);

    for (i = 0; i < sess->n_car && i < TBT_LEGACY_MAX_CARRIERS; i++)
    {
        struct tbt_carrier *sp = tbt_carrier_get(sess, (u8)i);
        bool pin = sp && sp->spare && atomic_read(&sp->up) &&
                   !atomic_read(&sp->stopping);

        if (pin)
        {
            fr->pinned = true;
            fr->path = (u16)i;
            fr->gen = sp->gen;
        }
        tbt_carrier_put(sp);
        if (pin)
            break;
    }

    fr->hdr = (struct tbt_hdr){
        .type = TBT_F_FBK,
        .body_len = nr * TBT_FBK_ENTRY_LEN,
        .path_ack_count = (u8)nr,
        .path_ack_mask = mask,
        .sid_lo = (u32)st->sid,
        .seq = 0,
        .conv = TBT_CONV,
    };
    fr->hdr.sid_hi = (u8)(st->sid >> 32);
    fr->st = st;
    refcount_inc(&st->refs);
    tbt_tx_enqueue(sess, fr, true);
    sess->fbk_tx++;
    tbt_tx_wake(sess);
    return 0;
}
```

失败路径：

- `tbt_carrier_get` NULL → 跳过。
- `sk_get` NULL → `rtt=0`，entry 仍然发布（0 表示无样本）。
- `!nr` → 返回 0，不分配；调用方仍更新签名（现有行为）。
- `alloc` 失败 → `-ENOMEM`，不更新签名，下个 tick 重试。
- `tbt_tx_enqueue` 不检查容量（现有行为）；FBK 很小，保持。
- `fr->path` 的索引在 put 后仍有效（槽位号，不是指针）；pump 会用 `tbt_frame_carrier_get` 重新取。

### 4.2.3 `tbt_tx_rto_ns()` / `tbt_tx_bdp_window()`

```c
static u64 tbt_tx_rto_ns(struct tbt_session *sess, struct tbt_stream *st)
{
    u32 rto_us = 0;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        u32 srtt = 0, mdev = 0, r;

        if (!c)
            continue;
        sk = tbt_carrier_sk_get(c);
        if (sk)
        {
            srtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
            mdev = READ_ONCE(tcp_sk(sk)->mdev_us);
        }
        tbt_carrier_sk_put(sk);
        if (!srtt)
        {
            srtt = READ_ONCE(c->rtt_us);
            mdev = 0;
        }
        tbt_carrier_put(c);

        r = srtt + 4 * mdev;
        if (r > rto_us)
            rto_us = r;
    }
    ...
}
```

`tbt_tx_bdp_window()` 同样：`get` → `sk_get` → 读 `srtt_us/mdev_us` 与 `tbt_car_delivery_bps(tp)` → `sk_put` → `put`。`tbt_car_delivery_bps()` 接收 `const struct tcp_sock *tp`，在 RCU 临界区内调用，安全。

`tbt_path_rtt_us()`：

```c
static u32 tbt_path_rtt_us(struct tbt_carrier *c)
{
    struct sock *sk;
    u32 rtt = READ_ONCE(c->rtt_us);
    u32 srtt = 0;

    sk = tbt_carrier_sk_get(c);
    if (sk)
        srtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
    tbt_carrier_sk_put(sk);
    return srtt > rtt ? srtt : rtt;
}
```

`tbt_path_stalled_mask()` 同样：`get` → `sk_get` → 读 `snd_una/snd_nxt` → `sk_put` → `put`。

### 4.2.4 锁顺序

- `tbt_tx_fbk`：不取 `tx_lock`（只有 `tbt_tx_enqueue` 内部取）；`get/sk_get/put` 只用 RCU/refcount；`GFP_KERNEL` 分配在锁外。
- `tbt_tx_rto_ns`：被 pump（`pump_lock`）与 fbk work（无锁）调用；内部 get/sk_get 不取 `pump_lock`，两种上下文都安全。`tbt_carrier_put` 在 `pump_lock` 下调用时只 `call_rcu`，不睡眠。
- `tbt_path_stalled_mask`：pump 内；get/put 不改变锁序。
- `tbt_path_rtt_us`：在 `tx_lock` 下被 `tbt_tx_hole_check` 调用；`sk_get` 进入 RCU 读临界区（可嵌套在 `tx_lock` 下），不睡眠；`sk_put` 退出。不要在 `tx_lock` 下 `tbt_carrier_release` 或 `synchronize_rcu`。
- 禁止：在 `sk_get` 临界区内 `tbt_carrier_put`（先 `sk_put`，再 `c_put`）；在 `tx_lock` 下 `tbt_carrier_sk_hold`（hold 只做 `sock_hold`，不睡眠，实际可以，但保持短读更简单）。

### 4.2.5 与生命周期组的接口契约

TX 侧只依赖四个符号：

```c
struct sock *tbt_car_sk_get(struct tbt_carrier *c);   /* RCU 短读 */
void        tbt_car_sk_put(struct sock *sk);
struct sock *tbt_car_sk_hold(struct tbt_carrier *c);  /* sock_hold 长用 */
void        tbt_car_sk_drop(struct sock *sk);
```

生命周期组负责：

- `tbt_carrier_release()` 在 `sock_release()` 之前 `rcu_assign_pointer(c->sk, NULL)` 并 `synchronize_rcu()`。
- `tbt_carrier_get/put` 与 `kfree_rcu`。
- `tbt_carrier_arm_tx/pump` 的 `sess->lock` 互斥。

TX 侧不直接读 `c->sk`，也不定义第二个 `READ_ONCE(c->sk)` 包装。若生命周期组最终把 helper 命名为 `tbt_carrier_sk_get`，TX 侧只改调用名，不改语义。

## 4.3 迁移步骤

1. 先落地第 2 节的 `tbt_carrier_get/put` 与 `tbt_car_sk_get/put`；没有它们，本节无法编译。
2. 改 `tbt_path_rtt_us()`（最小，单点）。
3. 改 `tbt_tx_rto_ns()` 与 `tbt_tx_bdp_window()`；这两处是 `tcp_sk` 读的主要来源。
4. 改 `tbt_path_stalled_mask()`。
5. 改 `tbt_tx_fbk()`：循环上界、get/put、sk_get/put、pin spare。
6. 改 `tbt_tx_pump` 尾部的 `c->sk` 读（`2797-2801`）与 `tbt_retain_tcp_done`（`1165-1173`）。
7. 全仓 `grep 'tcp_sk(c->sk)'` / `grep 'READ_ONCE(c->sk)'`，TX 文件应为 0（除注释）。

## 4.4 验证

**host**
- `make host-test`；`sparse` 检查 `__rcu` 与 `rcu_dereference`。
- 新增 `host/test_fbk_body.c`（建议）：用 fake carrier 数组验证 `nr<=16`、`mask` 只有低 16 位、`path_ack_count` 不超过 16；`n_car=17` 时 `WARN_ON_ONCE` 触发并返回 `-EINVAL`（host 上把 WARN 替换为断言）。

**QEMU**
- `tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R`；重点 probe J（FBK 与 hole 修复）与 probe R（替换）。
- KASAN 内核跑 `n=4,m=2` + 每 50ms 强制 `carrier down`；`tbt_tx_fbk`/`tbt_tx_rto_ns` 不应出现 `use-after-free in tcp_sk`。
- KCSAN 内核跑同一场景，检查 `c->sk` 的读写是否有 data race 报告（RCU 注解应消除）。

**真机**
- `review/wan-experiment/arm64/prod-smoke-10s.sh`；观察 `/proc/net/tbtun/stats` 的 `fbk_tx`、`rto_us`、`bdp_bytes`、`rtt_mean_us` 在 carrier 替换前后不跳变。
- 替换期间 `rto_us` 不应突然变成 200ms（说明 `sk_get` 失败被当作无样本，而不是读到悬垂内存）；`fbk_tx` 持续增长。
- 24h 长稳：`carrier_replaced` 增长时无 oops；`slabinfo` 中 `sock`/`tbt_carrier` 不泄漏。

---

# 5. 总迁移顺序与 gate

| 阶段 | 内容 | 依赖 | 独立验证 gate |
|---|---|---|---|
| P0 | 第 3 节 TX-6 边界（helper + 写点 + 循环上界 + FBK 上界） | 无 | `make host-test`；QEMU `n=16` 与 `fec=on 256`；`debug=2` 无 WARN |
| P1 | 第 1 节 submit 发布前构造 + purge/free_queue/2529 记账 | 无 | host 记账单测；QEMU probe G/H/I/J/R；`dup_pending` 归零 |
| P2 | 第 2 节 carrier refcount + RCU + sk helper + work arm | 无 | `sparse`；QEMU probe R + KASAN；24h `slabinfo` 不增长 |
| P3 | 第 2 节 TX 读点迁移（wake/sched/pump/retain/ack） | P2 | QEMU 全量；`perf top` 无 refcount 热点 |
| P4 | 第 4 节 fbk/rto/bdp 使用 sk helper | P2 | QEMU probe J/R + KASAN/KCSAN；真机替换压测 |
| P5 | 清理：删除 `take_dead` 复用、`pollers`、裸 `READ_ONCE(car[])`、更新注释 | P3/P4 | `grep` 零命中；`make check`；全量 QEMU |

**回滚点**：P0、P1、P2 各自独立提交，均可单独 revert。P3/P4 依赖 P2，回滚 P2 前必须先回滚 P3/P4。P1 与 P0 不依赖 P2，可以在 P2 之前合入，缩短数据面 UAF 的暴露时间。

**合入前必须全部为真的不变量**：

1. `tbt_tx_submit()` 在 `tbt_tx_publish_data()` 返回后不再读 `fr`、`dup[]`、`r`。
2. 发布后的 `tx_queued` 等于 `tx_q` 中所有帧的 `len` 之和；`dup_pending` 等于该 retain 条目在 `tx_q` 中未写出的 dup 数。
3. 任何 `sess->car[]` 的读者在解引用字段前已 `tbt_carrier_get()`；任何 `c->sk` 的读者在解引用前已 `tbt_car_sk_get()` 或 `tbt_car_sk_hold()`。
4. `tbt_carrier_release()` 在 `sock_release()` 之前 `rcu_assign_pointer(c->sk, NULL)` 并 `synchronize_rcu()`。
5. 所有 16 槽数组的索引都经过 `tbt_retain_path_ok()`；所有 retain 循环上界为 `min(n_car, 16)`。
6. `tbt_tx_fbk()` 的 `nr` 不超过 16，`body` 写入不超过 `TBT_FBK_MAX`。
7. `tbt_tx_purge_stream()`、`tbt_tx_free_queue()`、载体消失的 batch 路径都调用 `tbt_tx_frame_drop_account()`，`dup_pending`/`dup_bytes` 不卡正数。
