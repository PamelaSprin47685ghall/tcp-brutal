# tbtun 0.2.0 合并设计（S2 输出，供 S3 拆分）

> 只读设计。本文合并四份 S2 设计并裁决其冲突：
> `review/s2-lifecycle-design.md`、`review/s2-fec-design.md`、
> `review/s2-tx-design.md`、`review/s2-release-design.md`。
> 行号以当前工作树 `2a8d509` 为准；与设计文本冲突处，以本文件引用的代码事实为准。
> 交付物：最终 API、锁序图、阶段表、文件所有权、验收映射、结案表。

---

## 0. 裁决摘要

| # | 冲突 | 裁决 | 一句话理由 |
|---|---|---|---|
| 1 | carrier 复用 vs `kfree_rcu` | **不复用，`refcount + kfree_rcu`** | 对象身份稳定；`users` 只能保护"从容器取得"的窗口，不能保护已持有指针的读者 |
| 2 | helper 命名 | **`tbt_carrier_get/put/gen` + `tbt_carrier_sk_get/put`** | 一套名字、一套语义：`sk_get` 取 `sock` 引用，可在返回后睡眠 |
| 3 | 锁序 | **`pump_lock → sess->lock → tx_lock`；`rx->lock`/`sk_callback_lock`/RCU 为叶子** | 合并三份锁序，消除 `tx_lock → sess->lock` 的反向可能 |
| 4 | session 引用计数 | **7 个 session work + tunnel + stream + carrier** | `w_fec_rx`/`w_fec_tx` 纳入 `tbt_session_arm`，destroy 用 completion 等归零 |
| 5 | wire 版本 | **`TBT_CONV 0x0006` + `TBT_FEC_CONV 0x1002`；FIN body 16；PARITY marker 是 40 字节头内的字段组合** | 纠正"4 字节 marker"的误解；marker 与现网 `hdr_check` 冲突，必须同版本改 |
| 6 | 迁移顺序 | **M0–M9，十个可独立编译的阶段** | 每阶段一个 owner；`tbt_tx.c`/`tbt_rx.c` 不在同一阶段被两条轨道同时改 |
| 7 | 验收映射 | **每项修复 → host / 10 s 真机 / 1 GiB / DKMS 四列判定** | 见 §8 |
| 8 | 遗漏检查 | **A/B/C/D 四类 + lifecycle 附 7 项 + FEC PAD 新发现** | 未覆盖项在 §9.6 明确列出并给结案理由 |

---

## 1. 最终对象模型与 API

### 1.1 `struct tbt_carrier` 的最终字段（`tbtun/tbtun.h:997-1089`）

```c
struct tbt_carrier
{
    struct list_head node;         /* sess->prov；不再用于 sess->dead */
    struct list_head release_node; /* sess->dead_release；自链表示不在队列 */
    struct socket *sock;           /* 安装者与 close() 私有；不参与发布 */
    struct sock __rcu *sk;         /* 发布给所有读路径；NULL 后必须过 grace period */
    struct tbt_session *sess;      /* 对象持有 session 引用，见 §5 */
    refcount_t refs;               /* 1 = 容器/安装者引用；读者各持一份 */
    struct rcu_head rcu;           /* 最后一个 put → kfree_rcu */
    struct work_struct w_connect;
    struct work_struct w_rx;
    struct work_struct w_tx;
    struct delayed_work w_pump;
    atomic_t connected;
    atomic_t up;
    atomic_t stopping;
    atomic_t rx_again;
    /* 删除：atomic_t pollers。poll 改用 tbt_carrier_get/put。 */
    bool released;
    bool joined;                   /* join() 已装入 sess->car[]；见 §7.3 */
    bool hello_sent;
    ...
};
```

变更清单：

| 字段 | 动作 | 理由 |
|---|---|---|
| `sk` | 加 `__rcu` | sparse 把"漏改的裸解引用"变成编译错误 |
| `refs` | 新增 | carrier 对象引用；容器引用 + 读者引用 |
| `rcu` | 新增 | `kfree_rcu` 的头部 |
| `joined` | 新增 | `adopt`/`prov` 顺序修复；见 §7.3 |
| `pollers` | 删除 | 被 `refs` 取代；poll 用 `tbt_carrier_get/put` |
| `users` | **不引入** | lifecycle 的 `users` 方案随复用一起撤销 |

### 1.2 carrier 生命周期 API

```c
/* 取得一份 carrier 引用；NULL 表示槽为空或对象正在释放。
 * 任意上下文；内部只用 RCU + refcount，不取 sess->lock，不睡眠。 */
struct tbt_carrier *tbt_carrier_get(struct tbt_session *sess, u8 index);

/* 释放一份引用；最后一个引用触发 kfree_rcu()。任意上下文。 */
void tbt_carrier_put(struct tbt_carrier *c);

/* 只取 gen：等价于 get + READ_ONCE(c->gen) + put。任意上下文。 */
u32 tbt_carrier_gen(struct tbt_session *sess, u8 index);

/* 取得一份 socket 引用。返回后可以睡眠、lock_sock、tcp_sendmsg_locked。
 * 内部：rcu_read_lock → rcu_dereference(c->sk) → refcount_inc_not_zero(sk_refcnt)。
 * NULL 表示未发布/正在 release/引用已归零。 */
struct sock *tbt_carrier_sk_get(struct tbt_carrier *c);
void tbt_carrier_sk_put(struct sock *sk);          /* sock_put(sk) */

/* 只判存在，不取引用，不睡眠。用于 lost/action/connect_work 的存在性检查。 */
static inline bool tbt_carrier_sk_present(struct tbt_carrier *c)
{
    return rcu_access_pointer(c->sk) != NULL;
}

/* carrier work 的 arm：在 sess->lock 下检查 stopping/released，与 release 互斥。
 * 任意上下文（softirq 可用）；不睡眠。返回 false 表示没有排队。 */
bool tbt_carrier_arm_tx(struct tbt_carrier *c);
bool tbt_carrier_arm_pump(struct tbt_carrier *c, unsigned int ms);
bool tbt_carrier_arm_rx(struct tbt_carrier *c);

/* 停止一个 carrier 的所有 work。可睡眠，不得持 pump_lock/tx_lock。
 * 顺序：sess->lock 下置 stopping → 锁外 cancel w_connect/w_rx/w_tx/w_pump。 */
void tbt_carrier_quiesce(struct tbt_carrier *c);
```

语义对照（解决命名冲突）：

| 旧名 | 出处 | 新名 | 语义变化 |
|---|---|---|---|
| `tbt_carrier_sk_get` | lifecycle | `tbt_carrier_sk_get` | 不变：取 `sock` 引用 |
| `tbt_carrier_sk_put` | lifecycle | `tbt_carrier_sk_put` | 不变 |
| `tbt_carrier_get/put` | tx | `tbt_carrier_get/put` | 不变：carrier 引用 |
| `tbt_car_sk_get` | tx | **删除** | tx 的 `sk_get` 只是 RCU 短读、不取引用；与 lifecycle 同名不同义。统一为取引用的 `tbt_carrier_sk_get` |
| `tbt_car_sk_put` | tx | **删除** | 同上 |
| `tbt_car_sk_hold` | tx | **删除** | `tbt_carrier_sk_get` 已经取引用，`hold` 冗余 |
| `tbt_car_sk_drop` | tx | **删除** | 用 `tbt_carrier_sk_put` |
| `tbt_carrier_arm_tx/pump` | tx | `tbt_carrier_arm_tx/pump` | 不变；新增 `arm_rx` |
| `tbt_carrier_quiesce` | tx | `tbt_carrier_quiesce` | 不变 |

为什么统一为"取引用"而不是"RCU 短读"：短读版本要求调用者在 `rcu_read_lock` 内完成所有解引用，而 TX 的多数读点（`tbt_tx_rto_ns`、`tbt_tx_bdp_window`、`tbt_tx_fbk`）需要多次读 `tcp_sock` 字段并在其间做分支；取引用版本允许离开 RCU 段后继续使用，代价是一对 `refcount_inc/dec`。这些路径都在每 tick/每 pass 的 carrier 循环里，不在逐包热路径，开销可忽略。

### 1.3 session 生命周期 API

```c
/* 准入闸 + 寿命闸。closing 后拒绝新引用。 */
static bool tbt_session_get(struct tbt_session *s);   /* 进程上下文；取 sess->lock */
static void tbt_session_put(struct tbt_session *s);   /* 归零时 complete(&s->drained) */

/* 7 个 delayed work 的统一入口。bit 从 0 变 1 时取引用。 */
enum tbt_sess_work {
    TBT_SW_POLL, TBT_SW_REPLACE, TBT_SW_PENDING, TBT_SW_REAP,
    TBT_SW_FBK, TBT_SW_FEC_TX, TBT_SW_FEC_RX,
};
static bool tbt_session_arm(struct tbt_session *s, enum tbt_sess_work w,
                            unsigned long delay);
static void tbt_session_cancel(struct tbt_session *s, enum tbt_sess_work w);
```

`tbt_session_arm()` 的快路径（softirq 可用）：`READ_ONCE(closing)` + `refcount_inc_not_zero()`，不取 `sess->lock`。权威的 `tbt_session_get()` 只在进程上下文用。

### 1.4 stream work API

```c
/* 每个 work 一个 bit；bit==1 ⇔ 存在一个尚未进入入口的实例，且它持有一个引用。 */
#define TBT_STREAM_F_SCHED_READ   4
#define TBT_STREAM_F_SCHED_DRAIN  5
#define TBT_STREAM_F_SCHED_CTRL   6
#define TBT_STREAM_F_SCHED_KILL   7
#define TBT_STREAM_F_SCHED_TARGET 8

static bool tbt_stream_schedule(struct tbt_stream *st, struct work_struct *w,
                                unsigned long bit);
static bool tbt_stream_cancel(struct tbt_stream *st, struct work_struct *w,
                              unsigned long bit);
```

---

## 2. 锁序总图

### 2.1 全局顺序（从外到内）

```
tbt_lock (mutex)                          /* tunnel/session 全局；/proc 配置 */
  │
  ├─→ sess->pump_lock (mutex)             /* 所有 carrier 字节写、slot 交换、批量 release */
  │     │
  │     ├─→ socket lock (lock_sock)       /* send_raw/part_flush/send_whole/xmit；此路径不取 sess->lock */
  │     │
  │     ├─→ sess->lock (spinlock)         /* car[]/prov/dead_release、streams hash、n_prov */
  │     │     ├─→ sess->tx_lock (spinlock)
  │     │     └─→ st->rx_lock / st->tx_lock
  │     │
  │     ├─→ sess->tx_lock (spinlock)      /* pump 的 tx_q 批处理 */
  │     ├─→ sess->fec_out_lock (spinlock) /* fec_out 队列 */
  │     └─→ c->http_lock (mutex)          /* HTTP 头块一次性写 */
  │
  ├─→ socket lock (lock_sock)             /* w_rx 的 tbt_session_join()；此路径不取 pump_lock */
  │     └─→ sess->lock (spinlock)
  │
  └─→ sess->lock (spinlock)               /* destroy、join、peers */
        └─→ st->rx_lock / st->tx_lock
```

三条路径不构成环：写路径 `pump_lock → socket lock` 不取 `sess->lock`；join 路径
`socket lock → sess->lock` 不取 `pump_lock`；replace 路径 `pump_lock → sess->lock`
不取 socket lock。因此 `socket lock` 与 `sess->lock` 之间不存在"哪个在外"的全局
答案——它们只在各自的路径里嵌套。任何新增代码必须落在其中一条路径里，不能同时
取 `pump_lock`、`socket lock`、`sess->lock` 三者。

### 2.2 叶子锁（持锁期间不得再取任何锁）

| 锁 | 类型 | 规则 |
|---|---|---|
| `rx->lock`（FEC RX） | spinlock_bh | 只做窗口检查、槽位放置、快照/提交；不睡眠、不分配、不交付、不取 `sess->lock`/`st->rx_lock`/`tx_lock`/`pump_lock` |
| `sk_callback_lock` | rwlock_bh | softirq 叶子；不得再取任何锁；`tbt_sk_ops_restore()` 的 `write_lock_bh` 等完在途 callback |
| RCU 读段 | — | 不睡眠、不取锁；`tbt_carrier_sk_get()` 的 `rcu_read_lock` 只覆盖"读指针 + 取引用" |
| `sess->tx_lock` | spinlock | 在 `pump_lock`/`sess->lock` 之下；不在其内取 `sess->lock`/`pump_lock`/`lock_sock` |
| `sess->fec_out_lock` | spinlock | 在 `pump_lock` 之下；不在其内取 `pump_lock`/`tx_lock`/`sess->lock` |
| `c->http_lock` | mutex | 在 `pump_lock` 之下；不在其内取 `pump_lock` |

### 2.3 `w_fec_rx` 与 `w_fec_tx` 的交叉检查

| | `w_fec_tx` | `w_fec_rx` |
|---|---|---|
| 入口 | `tbt_fec_tx_work()` | `tbt_fec_rx_work()` |
| 取锁顺序 | `pump_lock → fec_out_lock`；`tbt_carrier_send_fec()` 在 `pump_lock` 下用局部 `sk` | `rx->lock`（快照/提交）→ 释放 → 解码 → `sess->lock`（查 stream）→ `st->rx_lock`（交付）→ `tx_lock`（ACK 入队） |
| 是否取 `rx->lock` | **否** | 是 |
| 是否取 `pump_lock` | 是（`tbt_fec_out_flush`） | **否** |
| 交叉点 | 两者都取 `sess->lock`/`tx_lock`，但方向一致 | 同左 |

结论：**无环**。`rx->lock` 只在 `w_fec_rx` 内出现，且不跨到任何其他锁；`w_fec_tx` 完全不碰 `rx->lock`。唯一必须守住的是：`tbt_fec_ack()` 的组包与 `tbt_tx_enqueue()` 必须在 `rx->lock` 外执行（fec 设计 §1.2.2 已规定；本文重申为硬规则）。

`tbt_fec_rx_record()`（`w_rx` 内）只取 `rx->lock` 做放置，不取 `sess->lock`。它与 `w_fec_rx` 在 `rx->lock` 上串行，方向相同。

### 2.4 已知例外与断言

- `tbt_carrier_new_locked()` 在 `pump_lock` 下 `kzalloc(GFP_KERNEL)`：这是现有的睡眠点，不是新引入。删除复用后分配次数增加，但仍在 work 上下文。
- `tbt_carrier_release()` 不得在 `tx_lock` 下调用；它 `cancel_work_sync` + `synchronize_rcu`，会睡眠。
- `tbt_session_arm()` 的快路径不取 `sess->lock`，可在任意上下文调用（包括 softirq 与持 `sess->lock` 时）。它不保证 `closing` 之后一定失败：检查与置位可能竞争。destroy 的 cancel 与 `wait_for_completion` 是最终屏障。
- `wait_for_completion()` 不得在 spinlock 内；destroy 在 `tbt_lock` 下调用，与现有 `cancel_work_sync` 同构。
- `tbt_carrier_sk_get()` 不得在 `sk_callback_lock` 内调用；callback 已有 `c`，直接 `rcu_dereference` 更快。

---

## 3. 裁决 1：carrier 复用 vs `kfree_rcu`

### 3.1 两案对照

| | lifecycle 案：保留 dead 池 + `reset` + `users` | tx 案：删除复用 + `refcount` + `kfree_rcu` |
|---|---|---|
| 对象身份 | 复用后 `gen/profile/index` 全变 | 永不变；旧读者只看到 `released` |
| 读者保护 | `users` 必须在 `sess->lock` 内取得 | `refcount_inc_not_zero` 在 RCU 读段内 |
| work 竞争 | `work_busy()` 检查 + `arm` 的 `sess->lock` 互斥 | `refcount` + `arm` 的 `sess->lock` 互斥 |
| 复位 | `memset` + 重建 `http_lock` + 4 个 work + parser/http_rx | 无复位；`kzalloc` 出生即零 |
| 内存 | dead 池上限 `2*n_car` × 37 KiB ≈ 19 MiB 常驻 | 活跃 9.5 MiB + RCU 积压（替换率 × grace） |
| 分配 | 复用省分配 | 每次替换 5 次 `kmalloc`（可合并为 1 次） |

### 3.2 裁决：不复用

三条理由，按权重排序：

1. **`users` 保护不了已经持有指针的读者。** lifecycle §5.2.1 自己列出：`tbt_tx_fbk()` 的 carrier 遍历会写已复用对象的 `rx_state`；`/proc` 的 `carrier <n> down` 会把新生命标记为 lost。这些读者不是"从容器取得"，而是在锁外继续使用旧指针。`users` 只能在取得点自增，无法追溯一个已经离开容器的指针。`refcount` 让对象在最后一个读者放手前不消失，身份也不变。
2. **`work_busy()` 是 TOCTOU。** `take_dead()` 在 `sess->lock` 下检查四个 work，然后 `memset`。一个已经通过 `stopping` 检查、尚未 `schedule_work()` 的 callback 可以在检查与 `memset` 之间排队。lifecycle 用 `users` 锁内取得堵这个洞，但要求每一个读者都遵守；漏一个就是 UAF。`refcount + arm 的 sess->lock 互斥` 是结构性的：`arm` 在 `sess->lock` 下检查 `stopping/released`，`release` 在同一把锁下置 `stopping`，两者不可能交错。
3. **复位逻辑是静默错误的温床。** `memset(c, 0, sizeof(*c))` 会清掉 `http_lock`、四个 `work_struct`、`release_node`、`users`、`joined`；任何新增字段都必须记得在 `reset` 里重建。`BUILD_BUG_ON` 只能检查五个缓冲的指针类型，检查不了"哪个字段忘了重建"。

### 3.3 成本量化

| 项 | 数值 | 说明 |
|---|---|---|
| `struct tbt_carrier` | ≈ 1 KiB | 不含五个堆缓冲 |
| 五个缓冲 | `8244 + 4096 + 8252 + 8232 + ≈8232` ≈ **37 KiB** | `TBT_HTTP_FRAME_MAX`/`TBT_HTTP_HDR_MAX`/`TBT_CAR_PART_MAX`/`parser`/`http_rx.rec` |
| 256 carriers 活跃 | ≈ **9.5 MiB** | 两案相同 |
| dead 池上限 | `2*n_car = 512` × 37 KiB ≈ **19 MiB 常驻** | 只在复用案存在；不复用案没有这笔 |
| 稳态替换率 | `256 / carrier_stall_ms` | 15 s → 17/s；HND0 风暴 1 s → 256/s |
| 分配吞吐 | 17/s × 37 KiB ≈ 630 KiB/s；256/s ≈ 9.5 MiB/s | 在 `w_connect`/`w_replace`/`w_poll` 上下文，不在数据面 |
| RCU 积压 | 替换率 × grace period | 256/s × 1 s × 37 KiB ≈ 9.5 MiB；与 dead 池同量级 |
| 分配器 | 37 KiB 走 buddy（order-3/4） | 不在逐包热路径；若在意，见下 |

**可选优化（M5 内单独子步）**：把五个缓冲合并为一次 `kvzalloc`（一个 37 KiB 块，五个指针指向块内偏移）。这把 5 次分配/释放降为 1 次，减少碎片与失败点。不改变任何语义；`BUILD_BUG_ON` 的指针类型检查保留。

### 3.4 迁移步骤与删除范围

**删除**：

| 符号 | 位置 | 替代 |
|---|---|---|
| `tbt_carrier_take_dead()` | `tbt_carrier.c:1281-1300` | 无；`new_locked` 总是 `kzalloc` |
| 复用分支（保存五指针 + `memset` + 重建） | `tbt_carrier.c:1312-1337` | 删除 |
| `atomic_t pollers` | `tbtun.h:1013` | `refcount_t refs` |
| `sess->dead`、`n_dead`、`dead_peak` | `tbtun.h:693,705,706` | `dead_release`（`:698`）保留；`/proc` 字段改名见下 |
| `n_dead >= 2*n_car` 的 `-ENOSPC` | `tbt_session.c:1385-1389` | 删除 |
| `tbt_carrier_retire()` 的挂 `dead` | `tbt_carrier.c:1556-1561` | `stats_retire + release + put` |
| `tbt_carriers_stop()` 的 `dead` 遍历 | `tbt_carrier.c:2055-2065` | 删除；批量 `unpublish/sync/close/put` |
| `tbt_carrier_reset()`（lifecycle 提案） | 未落地 | 不新增 |

**保留但改语义**：

| 符号 | 新语义 |
|---|---|
| `sess->dead_release` | 唯一批量队列：join 替换的旧槽、replace 的 victims |
| `/proc` 的 `dead_now` | 改为 `dead_release` 链表长度（待 release 数） |
| `/proc` 的 `dead_peak` | 改为累计 `carrier_retired`（单调递增） |
| `tbt_carrier_birth_checks()` | 保留 `BUILD_BUG_ON`（五指针类型 + 缓冲大小），防止有人改内联数组 |

**新增**：

```c
static void tbt_carrier_free_rcu(struct rcu_head *head)
{
    struct tbt_carrier *c = container_of(head, struct tbt_carrier, rcu);

    tbt_carrier_free(c);
}

void tbt_carrier_put(struct tbt_carrier *c)
{
    if (c && refcount_dec_and_test(&c->refs))
        call_rcu(&c->rcu, tbt_carrier_free_rcu);
}
```

`tbt_carrier_free()` 增加 `list_del_init(&c->release_node)` 与 `tbt_session_put(c->sess)`（见 §5）。`tbt_carrier_new_locked()` 在 `kzalloc` 成功后 `refcount_set(&c->refs, 1)`。

**批量形态（`tbt_replace_work()` 与 `tbt_carriers_stop()` 共用）**：

```c
for each c in batch:  tbt_carrier_quiesce(c);        /* 可睡眠，不持 pump_lock */
for each c in batch:  tbt_carrier_unpublish(sess, c); /* restore callbacks、清 c->sk */
synchronize_rcu();                                    /* 整批一个 grace period */
for each c in batch:  tbt_carrier_close(c);           /* sock_release */
for each c in batch:  tbt_carrier_put(c);             /* 最后一个 put → kfree_rcu */
```

`tbt_carrier_release(sess, c)` 保留为单 carrier 的薄封装：
`quiesce + unpublish + synchronize_rcu + close`。它只给"无法批量"的调用者
（迁移完成后没有）。`tbt_carrier_retire()` 改为 `stats_retire + release + put`，
迁移完成后若没有调用者则删除。`tbt_carrier_discard()`（从未安装的 socket）直接
`free`，不需要 grace period：`c->sk` 从未发布给任何读者。

**迁移顺序（M5）**：

1. `tbtun.h`：加 `refs`/`rcu`；删 `pollers`；删 `sess->dead`/`n_dead`/`dead_peak`，加 `carrier_retired`。
2. `tbt_carrier.c`：实现 `put/free_rcu`；`new_locked` 删除复用分支；`retire` 改 `release + put`；`carriers_stop` 改批量；`free` 加 `release_node` 清理与 `session_put`。
3. `tbt_session.c`：`join` 删除 `-ENOSPC` 检查；旧槽只挂 `dead_release`；`create` 不再 `INIT_LIST_HEAD(&s->dead)`。
4. `tbt_procfs.c`：`dead_now`/`dead_peak` 改名与语义调整。
5. 全仓 `grep pollers`、`grep take_dead`、`grep sess->dead` 应为零。

---

## 4. 裁决 2：helper 命名与读点迁移

### 4.1 最终 API（见 §1.2）

一套名字，一套语义。`tbt_carrier_get/put` 管 carrier 对象；`tbt_carrier_sk_get/put` 管 socket 引用；`tbt_carrier_sk_present` 管存在性。删除 `tbt_car_sk_get/put/hold/drop`。

### 4.2 读点迁移映射

**写路径（`pump_lock` 拥有 `c->sk`，只用局部 `sk`）**

| 函数 | 位置 | 动作 |
|---|---|---|
| `tbt_carrier_send_raw` | `tbt_carrier.c:808-810` | `lockdep_assert_held(&sess->pump_lock)`；一次 `rcu_dereference_protected`；后续只用局部 `sk` |
| `tbt_carrier_part_flush` | `:834-836` | 签名加 `struct sock *sk` |
| `tbt_carrier_send_whole` | `:862-878` | 签名加 `struct sock *sk` |
| `tbt_carrier_send_fec` | `:1018` | 同上；`!sk` → `-ENOTCONN` |
| `tbt_carrier_xmit` | `:1067-1264` | 同上；20 余处读点收敛为一次 |
| `tbt_carrier_http_begin` | 同文件 | 签名加 `struct sock *sk` |
| `tbt_car_poll_one` 的 drip | `:500` | 改 `mutex_trylock(&sess->pump_lock)`，失败则下一个 50 ms pass |

**短读/长用（`tbt_carrier_get` + `tbt_carrier_sk_get/put`）**

| 文件 | 函数 | 位置 | 动作 |
|---|---|---|---|
| `tbt_tx.c` | `tbt_path_rtt_us` | `:534-541` | `sk_get/put`；`c` 由调用者持有 |
| `tbt_tx.c` | `tbt_tx_bdp_window` | `:594-612` | `get` → `sk_get` → 读 `srtt_us/mdev_us` → `sk_put` → `put` |
| `tbt_tx.c` | `tbt_tx_rto_ns` | `:727-772` | 同上；`pump_lock` 与 `w_fbk` 两种上下文都安全 |
| `tbt_tx.c` | `tbt_retain_tcp_done` | `:1165-1173` | 在 `tx_lock` 内 `get` → `sk_get` → 读 `snd_una` → `sk_put` → `put` |
| `tbt_tx.c` | `tbt_path_stalled_mask` | `:1819-1825` | `get` → `sk_get` → 读 `snd_una/snd_nxt` → `sk_put` → `put` |
| `tbt_tx.c` | `tbt_tx_pump` 尾部 | `:2797-2801` | 局部 `sk`（已在 `pump_lock` 下） |
| `tbt_tx.c` | `tbt_tx_fbk` | `:2997-3092` | `get` → `sk_get` → 读 `srtt_us` → `sk_put` → 写 `body`/`rx_state` → `put` |
| `tbt_tx.c` | `tbt_tx_ack` | `:1744-1749` | `get(sess, c->index)` 校验 `got == c`；不匹配则丢 RTT 样本；末尾 `put` |
| `tbt_carrier.c` | `tbt_car_poll_one` | `:285/303/350` | `get` 整个 pass；`sk_get/put` 读 `sk_state` |
| `tbt_carrier.c` | `tbt_car_rx_work` | `:686/770` | `sk_get` 整个读循环；`sk_put` 在返回前 |
| `tbt_carrier.c` | `tbt_carrier_dead` | `:2085` | `sk_get` → 读 `sk_state` → `sk_put` |
| `tbt_carrier.c` | `tbt_carrier_resume` | `:2378/2385` | `sk_present` 或 `sk_get` |
| `tbt_carrier.c` | `tbt_carrier_snapshot` | `:2404-2416` | `get` → `sk_get` → 读 → `sk_put` → `put` |
| `tbt_carrier.c` | `tbt_carrier_lost` | `:1418-1423` | `sk_present` |
| `tbt_carrier.c` | `tbt_tx_hello` | `:2337-2353` | 一次 `get`，读 `profile/gen`，`put` |
| `tbt_session.c` | `tbt_peers_show` | `:383-403` | `get` → `sk_get` → 读 → `sk_put` → `put` |
| `tbt_session.c` | `tbt_action_apply` | `:531-547` | `get` → `tbt_carrier_lost` → `put`；存在性用 `sk_present` |
| `tbt_session.c` | `tbt_session_join` | `:1331` | socket lock 下 `sk_get` → `inet_sk` → `sk_put` |
| `tbt_rx.c` | `tbt_rx_hello` | `:689/721-722` | socket lock 下 `sk_get` → 读 `inet_daddr` → `sk_put` |
| `tbt_rx.c` | `tbt_rx_fbk` | `:387-390` | `get` → 校验 `gen` → 写 `uack/...` → `put` |
| `tbt_fec_data.c` | `tbt_fec_out_try` | `:153/174/207` | `get` → `send_fec` → `put` |
| `tbt_procfs.c` | carrier 统计行 | `:720/980` | `get` → 读标量 → `put` |

**保留裸读（加注释说明为什么安全）**

| 位置 | 理由 |
|---|---|
| `tbt_carriers_start` `:1663` | 会话创建期、`w_poll` 未 arm、`tbt_lock` 下 |
| `tbt_carriers_stop` `:2023-2042` | teardown，所有 work 已 cancel，之后 `synchronize_rcu` |
| `tbt_replace_work` 的换槽读 `:1780-1908` | 已在 `sess->lock` 下；写入改 `rcu_assign_pointer` |
| `tbt_session_retire` `:1317-1319`、`join` `:1367` | 已在 `sess->lock` 下，只读 `gen` |

**迁移 gate**：`make C=1` 对 `__rcu` 的检查；`grep -n "READ_ONCE(c->sk)\|tcp_sk(c->sk)"` 在 TX/RX 文件应为零（除注释与写路径的 `rcu_dereference_protected`）。

---

## 5. 裁决 3：session 引用计数

### 5.1 持有者清单

| 持有者 | 取引用 | 放引用 | 备注 |
|---|---|---|---|
| tunnel（`t->sess`） | `tbt_session_create()` 的 `refcount_set(..., 1)` | `tbt_session_destroy()` 的最后一次 `put` | 权威引用 |
| 每个已发布 stream | `tbt_stream_get()` 发布路径，`sess->lock` 内、`hlist_add_head_rcu()` 前 | `tbt_stream_free_rcu()` | 最后一个 stream 引用在 RCU 回调里归还 |
| 每个 carrier 对象 | `tbt_carrier_new_locked()` 成功分配 | `tbt_carrier_free_rcu()` → `tbt_carrier_free()` | 出生到 `kfree_rcu` 恰好一个，不复用则无需"是否取过"标志 |
| `w_poll` | `tbt_session_arm(TBT_SW_POLL)` | work 入口末尾 / `tbt_session_cancel` | |
| `w_replace` | `TBT_SW_REPLACE` | 同上 | |
| `w_pending` | `TBT_SW_PENDING` | 同上 | |
| `w_reap` | `TBT_SW_REAP` | 同上 | |
| `w_fbk` | `TBT_SW_FBK` | 同上 | |
| `w_fec_tx` | `TBT_SW_FEC_TX` | 同上 | 新增到枚举 |
| `w_fec_rx` | `TBT_SW_FEC_RX` | 同上 | 新增到枚举 |

`struct tbt_session` 新增：

```c
    struct completion drained;   /* refs 归零时 complete；destroy 在此等待 */
    unsigned long sched;         /* 7 个 work 的 bit */
```

`tbt_session_put()` 不释放 session；释放只发生在 `tbt_session_destroy()`，它在 `wait_for_completion(&s->drained)` 之后 `kfree`。最后一个引用只负责唤醒 destroy，不负责决定释放者。

### 5.2 `tbt_session_destroy()` 的精确序列

```c
void tbt_session_destroy(struct tbt_tunnel *t)
{
    struct tbt_session *s = t->sess;
    struct tbt_stream *st;
    struct hlist_node *tmp, *pos, *next;
    int i;

    if (!s)
        return;
    t->sess = NULL;

    /* 1. 关门。此后新引用一律拒绝。 */
    spin_lock(&s->lock);
    s->closing = true;
    spin_unlock(&s->lock);

    /* 2. 停 FEC 的两个入口。stopping 让 record/arm 不再排队。
     *    需要新增 bool stopping 到 struct tbt_fec_tx（rx 已有）。 */
    if (s->fec_rx) {
        spin_lock_bh(&s->fec_rx->lock);
        s->fec_rx->stopping = true;
        spin_unlock_bh(&s->fec_rx->lock);
    }
    if (s->fec_tx)
        WRITE_ONCE(s->fec_tx->stopping, true);

    /* 3. 停 session 自己的定时器。每个 cancel 归还 pending 实例的引用。
     *    w_fec_rx/w_fec_tx 在这里取消，而不是 stream 循环之后：它们是唯一
     *    会在第 6 步之后重新 schedule(&st->w_read) 的 session work。
     *    tbt_fec_tx_flush()/tbt_fec_tx_work()/tbt_fec_rx_record() 的
     *    stopping 检查保证第 6 步的 w_kill/w_target 不会把它们重新 arm。 */
    tbt_session_cancel(s, TBT_SW_POLL);
    tbt_session_cancel(s, TBT_SW_REPLACE);
    tbt_session_cancel(s, TBT_SW_PENDING);
    tbt_session_cancel(s, TBT_SW_REAP);
    tbt_session_cancel(s, TBT_SW_FBK);
    tbt_session_cancel(s, TBT_SW_FEC_TX);
    tbt_session_cancel(s, TBT_SW_FEC_RX);

    /* 4. 停 carrier：cancel w_tx/w_pump、restore callbacks、unpublish、
     *    一个 grace period、close、put（每个 put 归还 session 引用）。 */
    tbt_carriers_stop(s);

    /* 5. 第一遍 stream：先停唯一会重新 attach 的 w_target，再停 w_kill，
     *    最后 detach（内部按 ctrl→read→drain 取消）。 */
    for (i = 0; i < TBT_STREAM_HT; i++) {
        spin_lock(&s->lock);
        pos = s->streams[i].first;
        spin_unlock(&s->lock);
        while (pos) {
            struct tbt_stream *v = hlist_entry(pos, struct tbt_stream, node);

            next = READ_ONCE(pos->next);
            if (refcount_inc_not_zero(&v->refs)) {
                tbt_stream_cancel(v, &v->w_target, TBT_STREAM_F_SCHED_TARGET);
                tbt_stream_cancel(v, &v->w_kill,   TBT_STREAM_F_SCHED_KILL);
                tbt_sock_detach(v);
                tbt_stream_put(v);
            }
            pos = next;
        }
    }

    tbt_tx_free_queue(s);

    /* 6. 第二遍：unlink，释放 socket，丢 hash 引用。最后一个 stream 引用
     *    在 RCU 回调里归还 session 引用。 */
    for (i = 0; i < TBT_STREAM_HT; i++) {
        for (;;) {
            struct tbt_stream *victim = NULL;

            spin_lock(&s->lock);
            hlist_for_each_entry_safe(st, tmp, &s->streams[i], node) {
                hlist_del_rcu(&st->node);
                victim = st;
                break;
            }
            spin_unlock(&s->lock);
            if (!victim)
                break;
            tbt_stream_cancel(victim, &victim->w_kill, TBT_STREAM_F_SCHED_KILL);
            tbt_stream_release_socket(victim);
            tbt_stream_put(victim);
        }
    }

    /* 7. 还没变成 stream 的 accepted socket。 */
    tbt_pending_drain(s);

    /* 8. 断言性第二遍 cancel：closing + stopping 之后不应再有新的 arm。
     *    canceled 为 true 说明有 arm 点漏改，WARN 并继续。 */
    WARN_ON_ONCE(tbt_session_cancel(s, TBT_SW_FBK));
    WARN_ON_ONCE(tbt_session_cancel(s, TBT_SW_REPLACE));
    WARN_ON_ONCE(tbt_session_cancel(s, TBT_SW_FEC_TX));
    WARN_ON_ONCE(tbt_session_cancel(s, TBT_SW_FEC_RX));

    /* 9. FEC 状态。 */
    tbt_fec_data_free(s);

    /* 10. 丢掉 tunnel 的引用，等所有 stream/work/carrier 引用归还。 */
    tbt_session_put(s);
    wait_for_completion(&s->drained);

    /* 11. 释放。 */
    tbt_crypto_pair_wipe(&s->crypto_ctx);
    put_net(s->net);
    kfree(s);
}
```

### 5.3 wait 点与终止性

`wait_for_completion(&s->drained)` 在第 10 步。它一定返回，因为：

| 引用来源 | 归还点 |
|---|---|
| 7 个 session work | 第 3 步 cancel；正在运行的实例由 `cancel_delayed_work_sync` 等完并归还 |
| carrier | 第 4 步 `put`；最后一个 `put` 走 `kfree_rcu`，`free` 里 `tbt_session_put` |
| stream（hash 引用） | 第 6 步 `tbt_stream_put` |
| stream（work 引用） | 第 5/6 步 `tbt_stream_cancel` 归还；运行中的实例在入口末尾归还 |
| stream（最后一个） | `tbt_stream_free_rcu()` 里 `tbt_session_put` |

第 6 步之后没有新的调度器能取引用：`closing` 已置位，所有 session work 已 cancel，`fec_rx/fec_tx` 的 `stopping` 已置位，carrier callbacks 已 restore，pump 已停。`tbt_stream_free_rcu()` 只做 `tbt_rx_free_queue/tbt_tx_free_stream/kfree` 和 `tbt_session_put`，不取任何锁，不会与 destroy 持有的 `tbt_lock` 形成环。

**调试字段**：`/proc/net/tbtun/stats` 增加 `sess_refs`（`refcount_read(&s->refs)`），destroy 后必须为 0。迁移期加 `WARN_ON_ONCE` 定位泄漏。

**`tbt_session_create()` 的失败路径**：`t->sess = s` 必须在
`tbt_fec_data_init()` **之前**设置；任何后续失败都走调用者的
`tbt_session_destroy()`，不再 `kfree(s)`。这样 `get_net()` 与
`init_completion()` 的配对唯一。`tbt_carriers_stop()` 必须容忍空的
`car[]`/`prov`/`dead_release`，`tbt_fec_data_free()` 必须容忍 NULL。

### 5.4 与 FEC 设计的冲突裁决

fec 设计说 `w_fec_tx` 在 stream teardown **之后**取消（因为 `w_kill/w_target` 可能经 `tbt_tx_fin()` 重新 arm）。lifecycle 说在 stream 循环**之前**取消（因为它是唯一会在第 6 步之后重新 `schedule(&st->w_read)` 的 work）。

裁决：**之前取消**，并补两条闸门：

1. `tbt_fec_tx_flush()` 在 `fec_tx->stopping` 时返回 `-ESHUTDOWN`；`tbt_tx_fin()` 看到它后返回 0（不推进，不 arm）。
2. `tbt_fec_rx_record()` 在 `rx->stopping` 时返回 `-ESHUTDOWN`；`w_rx` 已 cancel，不会再有新记录。

这样第 5 步的 `w_kill/w_target` 调 `tbt_tx_fin()` 时不会重新 arm `w_fec_tx`；第 8 步的第二次 cancel 只作为断言。fec 的"之后取消"被这条闸门取代。

---

## 6. wire 版本与格式

### 6.1 常量

| 常量 | 当前值 | 0.2.0 | 位置 |
|---|---|---|---|
| `TBT_CONV` | `0x0005u` | **`0x0006u`** | `tbtun/tbt_uapi.h:61` |
| `TBT_FEC_CONV` | `0x1001u` | **`0x1002u`** | `tbtun/tbt_uapi.h:107` |
| `TBT_FIN_BODY_LEN` | 未定义 | **`16`** | 新增，`tbt_uapi.h` |
| `TBT_WIRE_VERSION` | `1` | 不变 | `tbt_uapi.h:66` |
| `TBT_WIRE_VERSION_V6` | `2` | 不变 | `tbt_uapi.h:67` |

注意：FEC 会话的 HELLO/OPEN/FIN/RST 仍是 32 字节旧格式记录，用 `TBT_CONV`；只有 40 字节 FEC 记录用 `TBT_FEC_CONV`。两种 conv 在同一 carrier 上并存，**必须同版本升级**。

### 6.2 FIN body

当前：`tbt_tx_fin()` 以 `body = NULL, len = 0` 调用 `tbt_tx_control_pinned()`（`tbt_tx.c:357-358`），`tbt_body_bounds()` 对 FIN 是 `min=0, max=0`（`tbt_uapi.h:262-268`）。线上 32 字节。

0.2.0：body 恰 16 字节，小端：

| 偏移 | 类型 | 字段 | 含义 |
|---|---|---|---|
| +0 | u64 | `pkt_end` | 本 stream 最后一个 DATA 包之后的**绝对会话包 seq** |
| +8 | u64 | `pkt_base` | 本 stream 第一个 DATA 包的**绝对会话包 seq**，按 super-block 对齐 |

校验：

- `tbt_body_bounds()` 的 `TBT_F_FIN` 改为 `*min = *max = TBT_FIN_BODY_LEN`。
- `tbt_hdr_check()`（`tbt_proto.c:81`）的 `conv != TBT_CONV` 与 body bounds 自动拒绝旧版本。
- 接收端 `tbt_rx.c:445-448` 的 FIN 分支解析 body；`pkt_end < pkt_base` → `fec_fin_bad++` + `-EPROTO`；`pkt_base % (groups*n) != 0` → `fec_fin_bad++` + 忽略。
- 非 FEC session 也发送 16 字节零 body（`pkt_end = pkt_base = 0`），保持单一长度规则。

### 6.3 PARITY marker（纠正"4 字节 marker"）

**不存在 4 字节 marker 或魔数。** 代码中 `marker`/`magic` 零命中。FEC 记录没有独立的标记字段；"marker" 是 **40 字节 `struct tbt_fec_hdr` 内的一组字段值**，用来表示"本块为空"。

| 偏移 | 类型 | 字段 | marker 值 | 字节序 |
|---|---|---|---|---|
| +0 | u8 | `type` | `TBT_FEC_PARITY`（`0x31`） | — |
| +1 | u8 | `flags` | `0` | — |
| +2 | u16 | `share_index` | `n`（parity 0） | 小端 |
| +4 | u8 | `n` | 块宽 | — |
| +5 | u8 | `m` | 校验数 | — |
| +6 | u16 | `epoch` | `0` | 小端 |
| +8 | u64 | `block_id` | 目标块 | 小端 |
| +16 | u64 | `seq` | 该块第一个包的绝对 seq | 小端 |
| +24 | u16 | `win_len` | `0` | 小端 |
| +26 | u16 | `payload_len` | `0` | 小端 |
| +28 | u16 | `conv` | `0x1002` | 小端 |
| +30 | u16 | `reserved` | `0` | 小端 |
| +32 | u32 | `payload_crc32c` | 空 payload 的 CRC32C，即 `crc32c(~0, NULL, 0) ^ ~0 = 0` | 小端 |
| +36 | u32 | `header_crc32c` | 覆盖 +0..+35 | 小端 |

校验规则（`tbt_fec.c:hdr_check` + `tbt_fec_data.c:rx_record`）：

1. `header_crc32c` 必须匹配；失败 → `fec_hdr_crc_fail++` + `-EPROTO`。
2. `payload_crc32c` 必须匹配；失败 → `fec_payload_crc_fail++`，只丢该记录。
3. `win_len == 0` 时：`payload_len == 0 && epoch == 0`，否则 `-EPROTO`。
4. 块内已经有 DATA/parity/`win_len_set` 时收到 marker → `-EPROTO`（"混合来源拒绝"）。
5. marker 之后收到该块的 DATA → `-EPROTO`（`b->empty` 已置位）。
6. `win_len > n` → `-EPROTO`；DATA 的 `epoch != 0` → `-EPROTO`。

**与现网的冲突**：当前 `tbt_fec.c:241` 要求 PARITY `payload_len == max_payload`，`:255-258` 要求 `win_len != 0`。marker 必须放宽这两条；这是 `TBT_FEC_CONV 0x1002` 的核心变化。

### 6.4 0.1.0 ↔ 0.2.0 不互操作失败路径

| 方向 | 失败点 | 结果 |
|---|---|---|
| 0.1.0 → 0.2.0 | 首个 HELLO 的 `conv=0x0005`，`tbt_hdr_check()`（`tbt_proto.c:81`） | `-EPROTO` → `tbt_car_rx_error()` → `rx_errs++`、`stopping=1`，carrier 关闭 |
| 0.2.0 → 0.1.0 | 0.1.0 的 `tbt_hdr_check()` 要求 `0x0005` | 同上 |
| 绕过 conv | 0.1.0 的 FIN `max=0` 收到 16 字节 body | `-EPROTO` |
| FEC 记录 | `0x1002` 对 0.1.0 的 `0x1001` | `tbt_fec_hdr_check()` `-EPROTO`（`tbt_fec.c:200`） |
| FEC 会话的控制帧 | 0.1.0 的 `TBT_CONV 0x0005` 对 0.2.0 的 `0x0006` | 首个控制帧 `-EPROTO` |

**统计口径**：conv 错同时落 `fec_proto_fail`（FEC 记录）与 `rx_parse_proto`（旧记录），各加一次 `rx_errs`。`/proc/net/tbtun/stats` 的 `rx_parse_proto` 非零即版本不匹配。

**host 测试同步**：`host/test_fuzz.c:219` 的 `TBT_CONV == 0x0005` 与 `host/test_fec.c:215` 的 `TBT_FEC_CONV == 0x1001` 必须改为新值，否则 `make check` 失败。

---

## 7. 阶段表、文件所有权、每阶段 gate

### 7.1 阶段表

| 阶段 | 内容 | 依赖 | 可独立编译 | host | ≤10 s 真机短测 |
|---|---|---|---|---|---|
| **M0** | 0.1.0 发布（release 第 1-2 节，CHANGELOG 改法按 §9.3 C-06 修正） | 无 | 是 | `make check`、lint | — |
| **M1** | TX 记账 + 16 槽边界（tx L1+L3） | M0 | 是 | 新增 `test_tx_publish`、`test_tx_index`；`make check` | `prod-smoke-10s.sh`，`dup=3`，`dup_pending` 回 0 |
| **M2** | FEC wire（fec S1：conv、FIN body、marker、`tbt_fec_ack_bytes`、FEC PAD 修正） | M0 | 是 | `test_fec` 新用例；`test_fuzz`/`test_http` FIN 16 | 无需真机 |
| **M3** | stream work 引用（lifecycle §2） | M1 | 是 | lint 新 grep；`make check` | `prod-smoke-10s.sh`，`pub.churn` |
| **M4** | session 引用计数 + destroy 重排（lifecycle §4） | M3 | 是 | `make W=1`；`sess_refs` 调试字段 | `tunnel add/del` + `rmmod` 10 s |
| **M5** | carrier 身份：`refcount + kfree_rcu + 删除复用` | M4 | 是 | `make C=1`（先不加 `__rcu`） | 替换风暴 10 s + `/proc` 循环读 |
| **M6** | `c->sk` RCU + 所有读点（lifecycle §1 + tx L2/L4） | M5 | 是（原子提交） | sparse `__rcu`；`make C=1` | 替换风暴 10 s + `rmmod` |
| **M7** | adopt/prov 顺序（lifecycle §3） | M6 | 是 | `make check` | 连接洪水 + 替换 10 s |
| **M8** | FEC 数据面（fec S2-S4） | M2、M6 | 是 | `test_fec` 全量 | 8 MiB FEC 流 + `drop_ppm` 10 s |
| **M9** | 文档 + 0.2.0 版本 + 验收（含 §8.5 的 `.65` watchdog 环境准备） | M8 | 是 | `make check`、lint、`dkms-srcs-check` | 1 GiB + DKMS 七步；长稳前启用 watchdog |

**M6 是唯一的原子提交**：字段 `__rcu`、所有读点、`unpublish → sync → close` 顺序必须同时落地。M5 只做 refcount 与删除复用，不加 `__rcu`，避免 sparse 在中间树上报错。M5 与 M6 应紧邻合入，缩短"对象不复用但 `c->sk` 顺序未改"的窗口。

### 7.2 文件所有权矩阵

| 文件 | M1 | M2 | M3 | M4 | M5 | M6 | M7 | M8 | M9 |
|---|---|---|---|---|---|---|---|---|---|
| `tbtun/tbt_tx.c` | **L1/L3** | — | `w_read` 排队点 | session arm | — | **sk 读点** | — | FIN 门 | — |
| `tbtun/tbt_rx.c` | — | — | `w_drain` 排队点 | — | — | **sk 读点** | — | **FIN 解析/交付** | — |
| `tbtun/tbt_carrier.c` | — | — | `w_drain`/poll | arm | **refcount/删除复用** | **unpublish/close/读点** | **adopt/prov** | — | — |
| `tbtun/tbt_session.c` | purge 记账 | — | `w_reap` 等 | **destroy/arm** | dead 删除 | — | join | — | — |
| `tbtun/tbt_fec_data.c` | — | wire | `w_read` 排队点 | arm | — | — | — | **RX work/交付/FIN** | — |
| `tbtun/tbt_sock.c` | — | — | **stream work** | session arm | — | — | — | — | — |
| `tbtun/tbt_uapi.h` | `TBT_DUP_MAX` | **conv/FIN** | — | — | — | — | — | — | 版本 |
| `tbtun/tbt_proto.c` | — | **hdr_check/bounds** | — | — | — | — | — | — | — |
| `tbtun/tbt_fec.c/h` | — | **hdr_check/ack_bytes** | — | — | — | — | — | — | — |
| `tbtun/tbt_procfs.c` | — | — | — | `sess_refs` | **dead 字段** | — | — | 新计数器 | — |
| `host/test_*.c` | 新增 | **FIN/marker** | — | — | — | — | — | FEC 用例 | — |
| `review/lint.sh` | — | — | **新 grep** | — | — | — | — | — | — |

规则：**同一阶段，`tbt_tx.c`/`tbt_rx.c` 只允许一个 owner。** M1 与 M2 可并行（不同文件，`tbt_uapi.h` 的不同区域），但合并时由一个 owner 统一处理头文件冲突。

### 7.3 每阶段 gate

| 阶段 | 编译 | host | 真机 10 s | 回滚 |
|---|---|---|---|---|
| M0 | 是 | `make check` 357/2566；lint 16/16+14/14 | — | `git revert` 发布 commit |
| M1 | `make W=1` | 新增两个 host 测试；`make check` 全绿 | `prod-smoke-10s.sh` 无 `dup_pending` 卡正数 | 独立 revert |
| M2 | 是 | `test_fec`/`test_fuzz`/`test_http` 全绿；`TBT_CONV`/`TBT_FEC_CONV` 断言同步 | — | 独立 revert（wire 未上线） |
| M3 | 是 | lint grep 零命中 | `pub.churn` 无 UAF/`refcount_t:` | 独立 revert |
| M4 | 是 | `make W=1` | `rmmod` 有界返回；`sess_refs=0` | 依赖 M3，先回滚 M4 |
| M5 | `make C=1`（无 `__rcu`） | `slabinfo` 不增长 | 替换风暴无 `WARN`；`dead_now` 有界 | 独立 revert |
| M6 | **sparse `__rcu` 零警告** | `make C=1` 全绿 | 替换风暴 + `rmmod` 无 `KASAN` | 依赖 M5，先回滚 M6 |
| M7 | 是 | `make check` | 连接洪水无 `WARN_ON_ONCE(c->joined)` | 独立 revert |
| M8 | 是 | `test_fec` 全量 | 8 MiB FEC 流完成；`fec_deliver_nomem=0`（无注入） | 依赖 M2/M6 |
| M9 | 是 | `make check`、lint、`dkms-srcs-check` | 1 GiB + DKMS 七步 | 分支 `release/0.2.0` 丢弃 |

---

## 8. 验收映射

### 8.1 每项修复 → 四列判定

| 修复 | host（`make check`/`test_fec`/lint） | 10 s 真机短测 | 1 GiB 验收 | DKMS |
|---|---|---|---|---|
| **carrier 不复用 + `kfree_rcu`** | `make C=1` 无 `__rcu` 警告；`slabinfo` 断言（host 无） | 替换风暴：`carrier_replaced` 增长，`tbt_carrier` slab 不增长 | 全程 256 替换，`dead_now` 有界，无 Oops | 七步 PASS |
| **`c->sk` RCU** | sparse `__rcu`；`grep READ_ONCE(c->sk)` 零命中 | 替换 + `rmmod`：无 `KASAN: use-after-free in tcp_sk` | 无 `BUG:`/`Oops`；`rx_errs` 不异常 | 七步 PASS |
| **helper 统一** | `make C=1`；`grep tbt_car_sk_` 零命中 | 同左 | 同左 | 同左 |
| **stream work 引用** | lint：裸 `schedule_work(&st->` 零命中 | `pub.churn` 10000 次连接/关闭，无 `refcount_t:` | 长流无 `WARNING:` | 七步 PASS |
| **session 引用计数** | `make W=1`；`sess_refs` 调试字段 | `tunnel add/del` 循环 + `rmmod`，`sess_refs=0` | 全程无 `refcount_t:`；`del 1` 后 `sess_refs=0` | 七步 PASS |
| **adopt/prov 顺序** | `make check` | 连接洪水 + 替换：`prov_peak ≤ prov_cap`，`join_dup` 不增长 | `join_bad=0` | 七步 PASS |
| **TX dup 记账** | 新增 `test_tx_publish`：`tx_queued/dup_bytes/dup_pending/retain_bytes` 四等式 | `dup=3`，流结束后 `dup_pending=0` | `dup_bytes_ppm` 合理；`dup_pending=0` | 七步 PASS |
| **TX-6 16 槽边界** | 新增 `test_tx_index`：`idx=15` 通过、`idx=16` WARN | `n=16` 大流量 + `dup=2`，无 WARN | 无 WARN；`path_mask` 统计与 0.1.x 一致 | 七步 PASS |
| **FEC-A 解码移出锁** | `test_fec` 混合指针用例；STUCK 不写 `rec` | 8–16 MiB `drop_ppm`，`fec_decode_recovered>0`，无 `parser stuck` | 无 `sleeping function called from invalid context` | 七步 PASS |
| **FEC-B `-ENOMEM`** | `tbt_rx_fec_deliver` 的 `-EPROTO` 边界 | `fec_deliver_fail_ppm=10000`，流完成，`fec_deliver_nomem>0` | 正常路径 `fec_deliver_nomem=0`；无缺字节 | 七步 PASS |
| **FEC-C FIN 基数** | `test_fuzz`/`test_http` FIN 16；`tbt_fec_fin_valid` 边界 | 双 stream；`fec_fin_bad=0`，`CLOSETIMEOUT=0` | 1 GiB 单流 + 控制连接；`rx_delivered == tx_seq` | 七步 PASS |
| **FEC-D 空 group/marker** | `test_fec` marker 合法/非法；`win_len > n` 拒绝 | `groups=32,n=4,m=4`，37 B 写；`fec_short_tx=1`，`fec_short_unknown=0` | HTTP 小请求 + 8 MiB 响应；`fec_empty_block` 增长 | 七步 PASS |
| **FEC-F ACK 钳位** | `tbt_fec_ack_bytes` 边界（0、`U64_MAX`、`tx_seq` 上下） | 注入 `next_deliver=U64_MAX`；`fec_ack_bad>0`，`tx_ack ≤ tx_seq` | `fec_ack_bad=0`；`tx_ack` 单调 | 七步 PASS |
| **short_len 一致** | `test_fec` epoch=0/S/S+1；PARITY 不一致 | 37 B 短包 + 末 share 丢失；交付字节逐字节一致 | 每 1 MiB 注入 1 B 短包；`fec_short_unknown=0` | 七步 PASS |
| **PARSER_BUF 不变量** | `_Static_assert` + `BUILD_BUG_ON`；临时改大 `TBT_FEC_PAYLOAD_MAX` 必须编译失败 | 最大记录 + marker 各一次；`http_parse_err=0` | 无 `parser stuck` | 七步 PASS |
| **FEC PAD 死定义清理** | `TBT_FEC_PAD` 删除后 `make check` 全绿；host 用例断言 40 B 0x23 不再被 FEC 解析器接受 | 无发送方；0x23 只走 32 B 旧扫描器 | 同左 | 七步 PASS |

### 8.2 host 判定细节

```sh
source ~/.cache/tbtun-build/env.sh && make check      # test_parse 357, test_fec 2566, 0 failures（0.1.0 基线）
make host-test-san                                    # ASan/UBSan
make C=1                                              # sparse __rcu 零警告
bash review/lint.sh                                   # 16/16 + 14/14 + 新增 grep
make dkms-srcs-check
```

0.2.0 新增的 host 测试：`host/test_tx_publish.c`、`host/test_tx_index.c`、`host/test_carrier_life.c`、`host/test_fbk_body.c`、`host/test_fec.c` 的 marker/FIN/ACK 用例。

### 8.3 1 GiB 验收判定

沿用 release 设计 §3：far `45.11.1.62` 用 `busybox httpd` 提供 1 GiB，near `.65` 用 `curl` 下载；两端 256 carriers，`n=4 m=4 fec=on mode=grouped w=8 crypto=on`。

必须同时成立：

1. `size_download == 1073741824`。
2. `sha256sum -c` 通过。
3. 两端 `c9` 的 `fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、`gaps_total` 增量为零。
4. `rx_errs`、`connect_fail`、`alloc_fail`、`join_bad` 为零。
5. 任一端 `dmesg` 无 `BUG:`/`Oops`/`WARNING:`/`general protection fault`。
6. `sess_refs` 在 `rmmod` 前回到 0。
7. `dead_now`/`dead_release` 有界（≤ `TBT_REPLACE_BATCH` 的常数倍）。

**前置条件**：`.65` 的硬件喂狗者（§8.5）必须**保持运行**；不得停止喂狗。
1 GiB 传输期间不做故障注入。

### 8.4 DKMS 判定

沿用 release 设计 §4 的七步：安装 dkms 3.2.2 → 源码入机 → 干净前置 → `install_dkms.sh` → 配置回读 → 卸载/重载 → `--uninstall`。判定标记：

```
DKMS_STEP <name> rc=<n> secs=<n>
DKMS_CHECK <name> PASS|FAIL (<detail>)
DKMS_RESULT PASS|FAIL
```

`DKMS_RESULT` 必须为 PASS；`DKMS_STEP` 无 `rc=[1-9]`；`DKMS_CHECK` 无 ` FAIL`；`--uninstall` 后残留为零。

**前置条件**：`.65` 的硬件喂狗者（§8.5）必须**保持运行**；`rmmod`/`modprobe`
期间不得停止喂狗。

### 8.5 验收环境准备：`.65` 内核硬件 watchdog

**唯一安装源**：`review/wan-experiment/arm64/netwatch-65/`
（`tbtun-netwatch.py`、`tbtun-netwatch.service`、`install-65.sh`、
`rollback-65.sh`、`README.md`）。**设计说明**：`review/dot65-watchdog-design.md`。
早期草案 `review/tbtun-watchdog*` 已删除，不要使用。本设计不对 `.65` 做任何
写操作。

关键裁决（与旧草案不同，按此执行）：

- **硬件 watchdog**：`/dev/watchdog0`，固定 **44 s** 超时，`SETTIMEOUT=0`；
  `MAGICCLOSE=0` 保证关闭 fd 不停狗。`nowayout` 运行时不可设；
  `dw_wdt.nowayout=1` 只能在重启后通过内核命令行生效，当前方案不依赖它。
- **唯一喂狗者**：当前是 `tbtun-wdt-hold.service`。在正式服务确认持有
  `/dev/watchdog0` fd 之前，**不停止 hold、不重启 `.65`**。
- **正式服务**：`tbtun-netwatch.service`，`Type=simple`、`Restart=no`；
  脚本每 **5 s** 喂狗；**uptime < 600 s 时只喂狗、不探测、不累计失败**；
  uptime ≥ 600 s 后每 **30 s** 探测路由器 80，连续 **3 次**失败停止喂狗并退出，
  硬件在 44 s 内复位。进程被 kill 或 panic 同样复位。
- **systemd 不喂狗**：`RuntimeWatchdogUSec=0`。
- **切换**：启动新服务（EBUSY 重试）→ 读 `/proc/<pid>/fd` 确认指向
  `/dev/watchdog0` → 停 hold → 观察 ≥10 s → 失败立即重启 hold。
- **回滚**：`MAGICCLOSE=0` 下无法优雅停止；唯一安全回滚是
  `sync; systemctl reboot`（60 s 未完成用 `reboot -f`；`CONFIG_MAGIC_SYSRQ`
  未编译，不用 sysrq）。**不要写“systemctl stop 即可”。**
- **宽限期**：`BOOT_GRACE=600`。**按 `/proc/uptime` 计算，不是服务启动
  时间**；uptime < 600 s 时只喂狗，根本不调用探测，因此也不产生失败计数。
  目的是防止“开机网络还没起来 → 探测失败 → 复位 → 再开机”的重启循环。
  读不到 `/proc/uptime` 时留在宽限期继续喂狗（方向是不复位）。
- **reset budget**：`5 次 / 30 分钟`。budget 统计**武装启动次数**：`main()`
  打开设备前 +1，与探测结果无关；连续 30 分钟探测健康才清零。窗口内已满
  5 次时，喂狗者**继续喂狗、不再停止**，只写告警，等待人工介入。宽限期内的
  HTTP 失败不产生失败计数，因此也不进入 budget。
- **判据**：`WATCHDOG_EXPECT_STATUS` / `WATCHDOG_EXPECT_BODY_REGEX` 待路由器
  实测回填，不猜。
- **验收期间**：1 GiB 与 DKMS 都**保持喂狗者运行**；停喂狗者等于启动 44 s
  复位倒计时。只有 24 h 长稳做故障注入，且在窗口外。
- **范围**：0.2.0 长稳验收环境准备，不进入 0.1.0 发布内容。

**验收时序（从上电开始，宽限按 `/proc/uptime`）**：

| 时间 | 行为 | 失败计数 | 复位 |
|---|---|---|---|
| uptime 0–600 s | 宽限期：每 5 s 喂狗，不探测 | 不累计 | 不触发 |
| uptime 600 s | 第一次探测 | 0 | — |
| uptime 600–690 s | 每 30 s 探测；连续失败累计 | 1 → 2 → 3 | — |
| uptime 690 s | 第 3 次连续失败，停止喂狗 | 3 | 倒计时开始 |
| uptime 734 s | 硬件 44 s 超时 | — | **复位** |

总容忍 ≈ **734 s = 600 + 90 + 44**。任何一次探测成功都把连续失败计数清零，
时钟从下一次探测重新计。reset budget 在每次武装启动时 +1（`main()` 打开
设备前），与探测结果无关；连续 30 分钟探测健康才清零。因此宽限期内的 HTTP
失败不会额外消耗 budget。窗口内第 5 次武装后，喂狗者不再停止喂狗。

---

## 9. 结案表

### 9.1 来源 A：本次审计高危/中危

主审计编号不在仓库文本中。S2 已覆盖的已知高危按主题列出；主审计填表时按此映射，未覆盖项见 §9.6。

| 主题 | 覆盖阶段 | 状态 | 证据 |
|---|---|---|---|
| `c->sk` 先 `sock_release` 后清空 | M6 | `FIXED` | `tbt_carrier_unpublish/close`；sparse `__rcu`；QEMU probe R |
| carrier 复用导致身份错配 | M5 | `FIXED` | 删除 `take_dead`；`refcount + kfree_rcu`；`slabinfo` |
| stream work 无引用 | M3 | `FIXED` | 5 个 bit + `tbt_stream_schedule/cancel`；lint grep |
| session `refs` 死字段 | M4 | `FIXED` | `tbt_session_arm/cancel` + `drained` |
| adopt 先装 callback 后挂 prov | M7 | `FIXED` | `joined` + 顺序重排；`WARN_ON_ONCE` |
| TX dup 帧 UAF | M1 | `FIXED` | 发布前构造 + 一次发布；`test_tx_publish` |
| TX-6 16 槽越界 | M1 | `FIXED` | `tbt_retain_path_ok`；`test_tx_index` |
| FEC 解码在 `rx->lock` 内睡眠 | M8 | `FIXED` | `w_fec_rx`；`test_fec` 混合指针 |
| FEC 交付 `-ENOMEM` 丢包 | M8 | `FIXED` | 逐包提交 + 游标；注入器 |
| FEC FIN 基数错误 | M8 | `FIXED` | `pkt_end/pkt_base`；双 stream |
| FEC 空 group 静默损坏 | M8 | `FIXED` | `gtx.blk_id` + marker；37 B 用例 |
| FEC ACK 溢出 | M8 | `FIXED` | `tbt_fec_ack_bytes`；边界测试 |
| `short_len` last-writer-wins | M8 | `FIXED` | 首条 PARITY 固定 `(win_len, epoch)` |

### 9.2 来源 B：prod-code 未修 7 项

| ID | 项 | 状态 | 理由 / 证据 |
|---|---|---|---|
| B-01 | FEC 无 NACK/选择性重传 | `记录` | 本版只修交付与解码；NACK 是协议扩展，不在 0.2.0 范围。写入 `CHANGELOG.md` 的 Known limitations |
| B-02 | rate epoch 未实现 | `记录` | 同上；`TBT_FEC_CONV 0x1002` 不引入 rate epoch |
| B-03 | bench CSV header 缺 `retx_other`/`data_tx`/`retx_data_ppm` | `待办（非阻塞）` | 不在 S2 设计范围；单独 commit，不阻塞 0.2.0 |
| B-04 | 真机 DKMS/`modprobe`/`rmmod` 未验证 | `FIXED` | release §4 七步；证据目录 `dkms-65-<UTC>/`；`DKMS_RESULT PASS` |
| B-05 | `n=4` 吞吐运行间方差 8.38–11.37 Mbit/s | `记录` | 1 GiB 验收只判"完成 + 校验"，不判吞吐 |
| B-06 | `n=8`/`n=16` 仅单次 40 MiB 记录 | `记录` | 0.2.0 的 1 GiB 用 256 carriers；n=8/n=16 长流留待后续 |
| B-07 | 透明空洞推进未实现 | `FIXED (contract)` | 只按精确 seq 前进是有意设计；`交接.md` 决策记录 |

### 9.3 来源 C：文档漂移

| ID | 漂移 | 状态 | 处置 |
|---|---|---|---|
| C-01 | 版本引用不一致 | `FIXED` | M9：`tbt_uapi.h` + README/README.zh/DESIGN/CHANGELOG/交接 |
| C-02 | `DESIGN.md:2458-2460` "Real-machine DKMS install is not validated" | `FIXED` | M9：改写为已验真机，引用证据目录 |
| C-03 | `交接.md:593` "不阻塞 0.0.1" | `FIXED` | M0（0.1.0 发布时已改） |
| C-04 | `review/findings.md:752` 的 spec-drift 14 行 | `FIXED`/`撤销` | M9：逐行复核，`grep -c '^| .*| RESOLVED'` |
| C-05 | `README.md:548`、`README.zh.md:421` 的 0.0.1 DKMS 记录 | `记录` | 历史事实；0.2.0 验收后补新段 |
| C-06 | `CHANGELOG.md` 的 `[Unreleased]` 当前是 **0.2.0 内容**（FEC v6 grouped 等），不是 release 设计假设的 0.1.0 内容；`[0.0.1]` 已于 2026-11-06 发布（`:272`） | `FIXED` | M0 不得把 `[Unreleased]` 整体改名为 `[0.1.0]`。改法：在 `[Unreleased]` 之下插入 `[0.1.0] - 2026-11-07`，只收录属于 0.1.0 的 parser/长度上界/`fr->dropped`/`retx_copy`/`tbt_tx_fbk` 单次读/FEC epoch 拒绝/`TBT_DIAG` 七条；FEC v6 grouped 与 0.2.0 修复留在 `[Unreleased]` |
| C-07 | `review/final-checklist.md` 头部已标 `SUPERSEDED`，C40/C49 仍写 `TBT_CONV=0x0004`（实际 `0x0005`） | `FIXED` | M9：在头部加一行“行内常量以 `tbt_uapi.h` 为准”；不重写 60 条历史门禁（Chesterton's Fence） |

### 9.4 来源 D：`review/findings.md` 的 OPEN

当前快照无 `| OPEN` 行（`grep -n '| OPEN' review/findings.md` 为空）。台账现状：39 个代码发现（36 拆 a–d）+ F1–F12（F3 缺号）+ P0–P3 工作项，全部为 `FIXED` 或其变体。M9 复核：

```sh
grep -n '| OPEN' review/findings.md                 # 期望：空
grep -nE '\| (critical|high|medium|low|nit) \| (OPEN|FIXED)' review/findings.md | wc -l
bash review/lint.sh                                 # 16/16 + 14/14
```

若出现 `OPEN`，一律进表，不得留在正文。

### 9.5 lifecycle 附 7 项

| # | 缺陷 | 覆盖阶段 | 状态 |
|---|---|---|---|
| 1 | `tbt_car_poll_one()` 的 drip 写与 pump 共享 `c->part` 而无锁 | M6 | `FIXED`：`mutex_trylock(&pump_lock)` |
| 2 | `tbt_tx_fbk()` 的 carrier 遍历写已复用对象的 `rx_state` | M5/M6 | `FIXED`：不复用 + `get/put` |
| 3 | `tbt_fec_data_init()` 失败路径泄漏 `get_net()` | M4 | `FIXED`：`t->sess = s` 提前，失败走 destroy |
| 4 | `tbt_stream_get()` 发布路径不检查 `closing` | M4 | `FIXED`：锁内检查 |
| 5 | `cancel_work_sync(&st->w_kill)` 取消 pending 实例时泄漏引用 | M3 | `FIXED`：`tbt_stream_cancel` 归还 |
| 6 | `tbt_fec_tx_work` 在 stream 拆除之后才 cancel | M4 | `FIXED`：提前 cancel + `stopping` 闸门 |
| 7 | `tbt_carrier_free()` 不清 `release_node` | M5 | `FIXED`：`list_del_init(&c->release_node)` |

### 9.6 未覆盖项与结案理由

| 项 | 来源 | 状态 | 理由 |
|---|---|---|---|
| `TBT_FEC_PAD 0x23` 与 `TBT_F_PAD 0x23` 同值，且不在 `tbt_fec_record_type()` | WireFacts 新发现（`tbt_uapi.h:116/183`、`tbt_fec_data.c:707-710`） | **M2 `FIXED`** | 无发送方构造 40 B PAD；`tbt_fec_hdr_check()` 接受它但接收路径永远到不了。M2 删除 `TBT_FEC_PAD` 定义与 `hdr_check` 的接受分支；0x23 只按 32 B 旧 PAD 解析。不能把 0x23 加入 `tbt_fec_record_type()`：那会让旧 PAD 被当作 40 B FEC 记录 |
| `tbt_hdr_check()` 行号引用错位（`:77` vs `:81`） | WireFacts | `FIXED (doc)` | 本文统一用 `tbt_proto.c:81`；fec 设计文本的 `:77` 不再引用 |
| `tbt_body_bounds()` 行号引用错位（`:252` vs `:262-268`） | WireFacts | `FIXED (doc)` | 本文统一用 `tbt_uapi.h:262-268` |
| `TBT_CONV`/`TBT_FEC_CONV` 的 host 断言 | WireFacts | M2 `FIXED` | `host/test_fuzz.c:219`、`host/test_fec.c:215` 同步改 |
| FEC 无 NACK（B-01） | release B | `记录` | 协议扩展，不在 0.2.0 |
| rate epoch（B-02） | release B | `记录` | 同上 |
| bench CSV header（B-03） | release B | `待办（非阻塞）` | 单独 commit |
| 吞吐方差/单次记录（B-05/B-06） | release B | `记录` | 验收只判完成与校验 |
| `README` 的 0.0.1 DKMS 历史（C-05） | release C | `记录` | 历史事实，不伪造 |
| 主审计 A 类的具体编号 | release A | 待主审计填表 | 仓库无该清单；模板与门槛见 release §5.6/§5.7 |
| “prod-code 7 项”的权威编号 | release B | 候选清单 | 仓库无该编号；`review/s2-release-design.md:904-921` 的 B-01…B-07 是**候选**，主审计编号不同时以其覆盖，表结构与门槛不变 |
| `review/findings.md` 的 F3 缺号 | FindingsLedger | `记录` | 台账瑕疵，不影响状态；39 个代码发现 + F1–F12（缺 F3）+ P0–P3 全部为 `FIXED` 或其变体，无 `OPEN` |

---

## 10. 风险与回滚

| 风险 | 触发 | 缓解 | 回滚 |
|---|---|---|---|
| `kfree_rcu` 积压 | 替换风暴 256/s | RCU 积压 ≈ 9.5 MiB，与 dead 池同量级；可选合并缓冲为一次分配 | 回滚 M5 |
| `pump_lock` 下 37 KiB `kzalloc` | 每次替换 | 分配在 work 上下文，不在数据面；可选移到锁外预分配 | 同左 |
| `__rcu` 迁移漏点 | M6 | sparse 零警告是硬 gate；`grep` 零命中 | 回滚 M6（依赖 M5） |
| `synchronize_rcu` 在 `sess->lock` 内 | 误用 | 断言：`unpublish` 的 sync 在锁外；`tbt_carrier_release` 不得在 `tx_lock` 下 | — |
| `w_fec_rx` 自重试活锁 | `-ENOMEM` 持续 | 2 ms 退避；`rounds < 4`；`stopping` 后返回 | 回滚 M8 |
| `wait_for_completion` 不返回 | 引用泄漏 | `sess_refs` 调试字段；迁移期 `WARN_ON_ONCE` | 回滚 M4 |
| 0.1.0 与 0.2.0 混跑 | 灰度 | conv 硬拒绝；首个 HELLO 即断；`rx_parse_proto` 可见 | 回退到 0.1.0 二进制 |
| DKMS 3.2.2 与 QEMU 3.4.4 差异 | `.65` 真机 | 七步全流程；失败按 release §4.5 清理 | `--uninstall` + 手工清理 |
| watchdog 误复位 | 路由器瞬时抖动、判据过严、开机网络未就绪 | 600 s 宽限期（只喂狗不探测）；5 s 喂狗、30 s 探测、连续 3 次才停喂；5 次/30 分钟 reset budget；先 dry-run；1 GiB/DKMS 期间不做故障注入 | 无法优雅停止；唯一回滚是 `sync; systemctl reboot`（`review/dot65-watchdog-design.md` §5） |
| watchdog 判据猜错 | 未等实测就填状态码 | 实测结果回来前 `WATCHDOG_EXPECT_STATUS`/`WATCHDOG_EXPECT_BODY_REGEX` 留空；结果写入设计文档 §3.3 与证据目录 | 改 `/etc/tbtun-watchdog.conf`，不改脚本 |
| 切换时 hold 与正式服务都 open 失败 | 先停 hold 再启动新服务 | 新服务先启动并 EBUSY 重试；确认 `/proc/<pid>/fd` 指向 `/dev/watchdog0` 后再停 hold；失败立即重启 hold | 重启 `.65` |

---

## 11. 交付清单

| 项 | 内容 |
|---|---|
| 最终 API | `tbt_carrier_get/put/gen`、`tbt_carrier_sk_get/put`、`tbt_carrier_sk_present`、`tbt_session_get/put/arm/cancel`、`tbt_stream_schedule/cancel` |
| 锁序 | §2 总图；`rx->lock`/`sk_callback_lock`/RCU 为叶子；`pump_lock → sess->lock → tx_lock` |
| 阶段表 | M0–M9；每阶段可独立编译、host 测试、≤10 s 真机短测 |
| 文件所有权 | §7.2 矩阵；`tbt_tx.c`/`tbt_rx.c` 同阶段单 owner |
| 验收映射 | §8 四列判定 |
| 验收环境准备 | §8.5 与 `review/dot65-watchdog-design.md`：44 s 硬件 watchdog、5 s 喂狗、30 s 探测、hold 切换、回滚=重启；草案 `review/tbtun-watchdog{,.service,.conf}` 未安装 |
| 结案表 | §9 A/B/C/D + lifecycle 附 7 项 + 新发现 |
| 未覆盖项 | §9.6，含理由与状态 |
