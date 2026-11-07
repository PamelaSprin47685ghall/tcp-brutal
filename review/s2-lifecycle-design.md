# tbtun 0.2.0 生命周期修复设计

> 范围：只读设计，不改代码、不构建、不部署。行号以当前工作树为准；引用现有代码时保持原文。
> 结论先行：0.2.0 的生命周期只依赖三条机械不变量——
> 1. **`c->sk` 只由 `pump_lock` 写、由 RCU 读、由 `sock_hold` 续命**；清指针、过 grace period、再 `sock_release()`。
> 2. **每一次 `queue_work` 都带一个引用，work 入口取走并归还；取消一个未运行的实例，取消者归还它的引用。**
> 3. **每个对象只有一个出生函数、一个死亡函数**；出生即全零，死亡即等引用归零。

---

## 0. 总则：所有权、锁序、三条规则

### 0.1 对象与所有者

| 对象 | 今天的寿命依据 | 0.2.0 的寿命依据 |
|---|---|---|
| `struct tbt_carrier` | `released` + `pollers` + `work_busy()` + “session 死前不 free” | 同上，但 `pollers` 泛化为 `users`，并**只在 `sess->lock` 下获取**；仍不早于 session 释放 |
| `struct sock`（carrier 的） | `sock_release()` 后立即 `c->sk = NULL` | `c->sk` 为 `__rcu`；`rcu_assign_pointer(NULL)` → `synchronize_rcu()` → `sock_release()`；读者 `sock_hold()` |
| `struct tbt_stream` | hash 引用 + 调用者引用；work 不持引用 | 增加“每个排队 work 一个引用”；work 入口 `put` |
| `struct tbt_session` | 仅 `t->sess` 指针；`refs` 字段已存在但从未使用 | `refs` 真正生效：tunnel 1 个 + 每个已发布 stream 1 个 + 每个 carrier 对象 1 个 + 每个已排队 session work 1 个；destroy 等归零 |

### 0.2 全局锁序（0.2.0 必须成立）

```
tbt_lock (mutex)
  → sess->pump_lock (mutex)            /* 所有 carrier 字节写、slot 交换 */
      → sess->lock (spinlock)          /* car[]/prov/dead、streams hash、n_prov */
          → sess->tx_lock (spinlock)
          → sess->fec_out_lock (spinlock)
      → c->http_lock (mutex)
  → sess->lock
      → st->rx_lock / st->tx_lock (spinlock)

sk_callback_lock (spinlock, softirq)：叶子锁，持锁期间不得再取任何锁。
socket lock (lock_sock)：在 pump_lock 之下；持 socket lock 时不得取 pump_lock。
RCU 读段：叶子，不得睡眠、不得取锁。
```

现有代码已经遵守 `pump_lock → sess->lock`（`tbt_replace_work()` 1748-2006）和 `socket lock → sess->lock`（`tbt_session_join()` 从 rx work 的 `lock_sock()` 内调用）。0.2.0 新增的 `users` 是 `atomic_t`，不引入锁；但**获取 `users` 必须在 `sess->lock` 内完成**（见 §5），这是它与 `take_dead()` 的握手点。

### 0.3 三条规则（评审时逐条对照）

- **R1**：任何在 `pump_lock` 之外读 `c->sk` 的代码，必须 `tbt_carrier_sk_get()` / `tbt_carrier_sk_put()`。
- **R2**：任何 `schedule_work()` / `queue_work()` / `mod_delayed_work()` 的目标 work，必须在同一个原子操作里“置位 + 取引用”；work 入口第一件事清位，最后一件事放引用。
- **R3**：`tbt_carrier_take_dead()` 的复用条件是 `released && users == 0 && !work_busy(×4)`，且 `users` 只能在 `sess->lock` 下从 `car[]`/`prov`/`dead` 取得。

---

## 1. `c->sk` 生命周期

### 1.1 现状（文件:行）

字段（`tbtun/tbtun.h:1122-1124` 附近）：

```c
struct tbt_carrier
{
    struct list_head node;    /* sess->prov or sess->dead */
    struct list_head release_node; /* sess->dead_release while a release is
                                    * pending; self-linked otherwise */
    struct socket *sock;
    struct sock *sk;
    struct tbt_session *sess;
```

**写点**

| 文件:行 | 代码 | 上下文 |
|---|---|---|
| `tbt_carrier.c:2257-2258` | `c->sock = s; c->sk = s->sk;` | `w_connect`，`kernel_connect` 返回后、安装 callbacks 前 |
| `tbt_carrier.c:2485-2486` | `c->sock = newsock; c->sk = newsock->sk;` | `tbt_carrier_adopt()`，安装 callbacks 前 |
| `tbt_carrier.c:1508-1509` | `c->sock = NULL; c->sk = NULL;` | `tbt_carrier_release()`，**在 `sock_release(s)` 之后** |

**清除顺序就是缺陷本身**：

```c
        tbt_sock_abort_on_close(s);
        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        c->sock = NULL;
        c->sk = NULL;
```

`sock_release()` 会走到 `tcp_close()` → `sock_put()`，最后一个引用一落，`struct sock` 即可被 `sk_free()` 回收；而 `c->sk` 仍是旧指针。任何在 `sock_release()` 之后、`c->sk = NULL` 之前读到它的上下文，都会拿到一个已释放（或正在释放）的 `struct sock`。更糟的是，读到之后还可以在 `c->sk = NULL` 之前再次读取——两次读取之间对象就可能被释放。

**读点全表**（`c->sk` / `c->sock`；★ = 任务点名函数）

| 文件:行 | 函数 | 读取内容 | 现有保护 | 风险 |
|---|---|---|---|---|
| `tbt_carrier.c:100` | `tbt_car_ready()` ★ | `sk->sk_user_data` | `sk_callback_lock` 读锁 | 安全；`c` 由 callback lock 保活 |
| `tbt_carrier.c:121` | `tbt_car_write_space()` ★ | 同上 | 同上 | 同上 |
| `tbt_carrier.c:168` | `tbt_car_state_change()` | 同上 | 同上 | 同上 |
| `tbt_carrier.c:285` | `tbt_car_poll_one()` ★ | `dsk = READ_ONCE(c->sk)`，诊断用 | `pollers` | 指针可能已释放 |
| `tbt_carrier.c:303` | `tbt_car_poll_one()` ★ | `psk`，provisional 分支 | `pollers` + `sess->lock` | 同上 |
| `tbt_carrier.c:350` | `tbt_car_poll_one()` ★ | `sk`，健康检查/停滞判定 | `pollers` | 同上，且后续多处解引用 |
| `tbt_carrier.c:686` | `tbt_car_rx_work()` ★ | `sk`，整个读循环 | `w_rx` 自身 | release 可并发清指针 |
| `tbt_carrier.c:770` | `tbt_car_rx_work()` ★ | `READ_ONCE(c->sk) == sk` 再确认 | 无 | 同上 |
| `tbt_carrier.c:808-810` | `tbt_carrier_send_raw()` | `lock_sock/tcp_sendmsg_locked/release_sock(c->sk)`，**三次读** | 无（poll 调用） | 三次读可得到不同指针 |
| `tbt_carrier.c:834-836` | `tbt_carrier_part_flush()` | 同上三次读 | 名义上 `pump_lock` | 无断言 |
| `tbt_carrier.c:862-878` | `tbt_carrier_send_whole()` | `!c->sk`、`sk_wmem_queued`、`sk_sndbuf`、三次 socket 调用 | 名义上 `pump_lock` | 同上 |
| `tbt_carrier.c:1018` | `tbt_carrier_send_fec()` | `!c->sk` 预检 | `pump_lock`（经 `tbt_fec_out_flush()`） | 无断言 |
| `tbt_carrier.c:1067` | `tbt_carrier_xmit()` ★ | `!c->sk` 预检 | `pump_lock` | 无断言 |
| `tbt_carrier.c:1082` | `tbt_carrier_xmit()` ★ | `tcp_sk(c->sk)->snd_nxt` | `pump_lock` | 无断言 |
| `tbt_carrier.c:1130-1133` | `tbt_carrier_xmit()` ★ | 诊断四读 | `pump_lock` | 无断言 |
| `tbt_carrier.c:1175-1180` | `tbt_carrier_xmit()` ★ | DATA 准入预检 | `pump_lock` | 无断言 |
| `tbt_carrier.c:1219-1221` | `tbt_carrier_xmit()` ★ | 写记录三次 socket 调用 | `pump_lock` | 无断言 |
| `tbt_carrier.c:1263-1264` | `tbt_carrier_xmit()` ★ | HELLO 诊断 | `pump_lock` | 无断言 |
| `tbt_carrier.c:1418-1423` | `tbt_carrier_lost()` | 仅 `!!READ_ONCE(c->sk)` | 无 | 只读存在性，可保留 |
| `tbt_carrier.c:1489-1509` | `tbt_carrier_release()` ★ | `s = c->sock`，末尾清 `c->sk` | 独占 | **清除顺序错误** |
| `tbt_carrier.c:2085` | `tbt_carrier_dead()` | `sk->sk_state` | `sess->lock`（调用者） | 与 release 并发时 UAF |
| `tbt_carrier.c:2199-2205` | `tbt_car_connect_work()` | `!!READ_ONCE(c->sock)` | `w_connect` 自身 | 只读存在性 |
| `tbt_carrier.c:2378,2385` | `tbt_carrier_resume()` | `READ_ONCE(c->sk)` 存在性 | `sess->lock` | 只读存在性 |
| `tbt_carrier.c:2404-2416` | `tbt_carrier_snapshot()` ★ | 多次读 `c->sk`，第二次才 `sock_hold` | `/proc`（`tbt_lock`） | 两次读之间可释放 |
| `tbt_carrier.c:2485-2486` | `tbt_carrier_adopt()` | 写点 | accept work | 安装顺序问题（§3） |
| `tbt_tx.c:539-540` | `tbt_path_rtt_us()` | `c->sk->srtt_us` | 无 | UAF |
| `tbt_tx.c:601-603` | `tbt_tx_bdp_window()` | `tcp_sk(c->sk)` | `pump_lock`（经 `tbt_tx_window_update`） | 无断言 |
| `tbt_tx.c:737-740` | `tbt_tx_rto_ns()` | `srtt_us/mdev_us` | 无（`w_fbk` 也调用） | UAF |
| `tbt_tx.c:1171-1173` | `tbt_retain_tcp_done()` | `snd_una` | `pump_lock` | 无断言 |
| `tbt_tx.c:1825-1827` | `tbt_path_stalled_mask()` | `snd_una/snd_nxt` | `pump_lock` | 无断言 |
| `tbt_tx.c:2385-2440` | `tbt_tx_pump()` ★ | 准入、`sk_socket->flags` | `pump_lock` | 无断言 |
| `tbt_tx.c:2737` | `tbt_tx_pump()` ★ | `write_seq` | `pump_lock` | 无断言 |
| `tbt_tx.c:2801-2804` | `tbt_tx_pump()` ★ | `sk_stream_memory_free`、`sk_socket->flags` | `pump_lock` | 无断言 |
| `tbt_tx.c:3017-3019` | `tbt_tx_fbk()` ★ | `srtt_us` | 无 | 已有“单次快照”注释，但无引用 |
| `tbt_session.c:397,414` | `tbt_peers_show()` | 两次 `READ_ONCE(c->sk)` + `sock_hold` | `tbt_lock` | 第一次读与第二次读之间可释放 |
| `tbt_session.c:547` | `tbt_action_apply()` | `if (c->sock)` | `tbt_lock` | 只读存在性 |
| `tbt_session.c:1331` | `tbt_session_join()` | `csk = READ_ONCE(c->sk)`，随后 `inet_sk(csk)` | socket lock（rx work） | release 可并发 |
| `tbt_rx.c:689,721-722` | `tbt_rx_hello()` | `csk` / `c->sk`，读 `inet_daddr` | socket lock（rx work） | release 可并发 |

### 1.2 设计

#### 1.2.1 字段与所有权

```c
struct tbt_carrier
{
    ...
    struct socket *sock;      /* 只有安装者和 tbt_carrier_close() 读；不参与发布 */
    struct sock __rcu *sk;    /* 发布给所有读路径；清除后必须过 grace period
                               * 才能 sock_release() */
    ...
    atomic_t users;           /* 见 §5：非 pump_lock、非 carrier work 的读者 */
};
```

- `sock` 与 `sk` 的职责分开：`sock` 是“谁拥有这个 socket”的私有句柄，`sk` 是“谁可以使用这个 socket”的公开指针。
- `c->sock` 的读者在迁移后只剩两处：安装点（connect/adopt）和 `tbt_carrier_close()`。`tbt_action_apply()` 的 `if (c->sock)`、`tbt_car_connect_work()` 的 `READ_ONCE(c->sock)`、`tbt_carrier_lost()` 的诊断全部改为 `rcu_access_pointer(c->sk)`。
- `__rcu` 注解让 sparse 成为迁移的机械检查：任何漏改的裸解引用都会在 `make C=1` 时报错。

#### 1.2.2 读 helper

```c
/* 取一个可用引用；NULL 表示该 carrier 当前没有可用的 socket（尚未发布、
 * 正在 release，或 socket 引用已归零）。
 *
 * RCU 段只覆盖“读指针 + 取引用”。离开 RCU 段后，sock 的寿命由 sk_refcnt
 * 保证，调用者可以睡眠、取 socket lock、做 tcp_sendmsg_locked()，直到
 * tbt_carrier_sk_put()。 */
static inline struct sock *tbt_carrier_sk_get(struct tbt_carrier *c)
{
    struct sock *sk;

    rcu_read_lock();
    sk = rcu_dereference(c->sk);
    if (sk && !refcount_inc_not_zero(&sk->sk_refcnt))
        sk = NULL;
    rcu_read_unlock();
    return sk;
}

static inline void tbt_carrier_sk_put(struct sock *sk)
{
    if (sk)
        sock_put(sk);
}
```

为什么是 `refcount_inc_not_zero()` 而不是裸 `sock_hold()`：`sock_hold()` 的文档前提是“对象已在 hash/list 中被锁找到”；这里只有 RCU 指针。`inc_not_zero` 在引用为 0 时返回 false，比 `refcount_inc` 的 `refcount_t: saturated` 警告更早、更明确地拒绝。正常情况下它不会失败（`sock_release()` 之前 socket 自己持有一个引用），失败只意味着“已经 unpublish”。

#### 1.2.3 发布与清除

发布点（两处，语义相同）：

```c
    c->sock = s;
    rcu_assign_pointer(c->sk, s->sk);
```

位置必须在 `sk_user_data = c` 之前：callback 一旦能拿到 `c`，就必须能拿到 `c->sk`。adopt 的顺序见 §3。

清除点拆成两阶段，供批量调用者摊薄 grace period：

```c
/* 阶段 1：让 c->sk 不可达，停掉所有 callback 和 rx work。
 * 不关闭 socket——调用者必须对整批 carrier 调用一次 synchronize_rcu()，
 * 再逐个 tbt_carrier_close()。 */
static void tbt_carrier_unpublish(struct tbt_session *sess,
                                  struct tbt_carrier *c)
{
    struct socket *s = c->sock;

    if (!s)
        return;
    atomic_set(&c->stopping, 1);
    cancel_work_sync(&c->w_connect);
    cancel_work_sync(&c->w_rx);
    if (s->sk) {
        /* 先恢复 core callbacks：write_lock_bh 会等待所有正在读
         * sk_user_data 的 softirq 退出，此后不会再有 callback 进入我们。 */
        tbt_sk_ops_restore(s->sk, &sess->def_ops);
        sock_replace_proto(s->sk, &tcp_prot);
        /* restore 之前已经排队的 w_rx 在这里收掉。 */
        cancel_work_sync(&c->w_rx);
    }
    rcu_assign_pointer(c->sk, NULL);
}

/* 阶段 2：关闭并释放。调用前必须已经过 synchronize_rcu()。 */
static void tbt_carrier_close(struct tbt_carrier *c)
{
    struct socket *s = c->sock;

    if (!s)
        return;
    tbt_sock_abort_on_close(s);
    kernel_sock_shutdown(s, SHUT_RDWR);
    sock_release(s);
    c->sock = NULL;
    atomic_set(&c->connected, 0);
    atomic_set(&c->up, 0);
    if (c->parser->have)
        c->sess->recv_partial_discard++;
    c->parser->have = 0;
    tbt_http_rx_new_message(c->http_rx);
    if (c->part_len || c->part_off)
        c->sess->recv_partial_discard++;   /* 未写完的尾字节，见 §5 */
    c->part_len = c->part_off = 0;
    if (c->local_port) {
        tbt_port_free(c->local_port);
        c->local_port = 0;
    }
    c->released = true;
}
```

批量形态（`tbt_replace_work()`、`tbt_carriers_stop()`）：

```
for each c in batch:  tbt_carrier_unpublish(sess, c);
synchronize_rcu();                       /* 整批一个 grace period */
for each c in batch:  tbt_carrier_close(c); tbt_carrier_stats_retire(sess, c);
```

单 carrier 的 `tbt_carrier_release()` 保留为 `unpublish + synchronize_rcu() + close` 的薄封装，只给“无法批量”的调用者（今天没有；迁移完成后可删）。

**正确性论证**：读者在 `rcu_read_lock()` 内完成 `rcu_dereference` + `refcount_inc_not_zero`；`synchronize_rcu()` 返回时，所有在 `rcu_assign_pointer(NULL)` 之前开始读段的人都已离开读段——他们要么拿到 NULL，要么已经持有引用。因此 `sock_release()` 之后，不可能再有人“拿到指针但还没取引用”。持有引用者让 `sk_refcnt` 保持非零，`sk_free()` 被推迟到最后一个 `sock_put()`。

#### 1.2.4 写路径：`pump_lock` 拥有 `c->sk`

0.2.0 把“所有 carrier 字节写都在 `sess->pump_lock` 下”从注释变成断言：

| 函数 | 新签名 | 断言 |
|---|---|---|
| `tbt_carrier_xmit` | 不变 | `lockdep_assert_held(&sess->pump_lock);` |
| `tbt_carrier_send_fec` | 不变 | 同上 |
| `tbt_carrier_send_raw` | 不变 | 同上（**新增**：poll 的 drip 也必须持锁） |
| `tbt_carrier_part_flush` | `(struct tbt_carrier *c, struct sock *sk)` | 同上 |
| `tbt_carrier_send_whole` | `(struct tbt_carrier *c, struct sock *sk, const void *data, size_t len)` | 同上 |
| `tbt_carrier_http_begin` | `(struct tbt_carrier *c, struct sock *sk)` | 同上 |

函数体内统一：

```c
    struct sock *sk;

    lockdep_assert_held(&sess->pump_lock);
    sk = rcu_dereference_protected(c->sk,
                                   lockdep_is_held(&sess->pump_lock));
    if (!sk)
        return -ENOTCONN;
```

然后**整个函数只使用局部 `sk`**，不再出现 `c->sk`。这把 `tbt_carrier_xmit()` 里 20 余处读点收敛为一次。

`tbt_car_poll_one()` 的 drip 写（`tbt_carrier.c:500`）今天不持 `pump_lock`，而 `tbt_carrier_part_flush()` 的注释写的是“caller holds sess->pump_lock (or is the only writer)”。它既不是前者也不是后者：pump 可以在同一 carrier 上写 `c->part`。这是与生命周期同源的锁缺陷，0.2.0 一并修：

```c
            if (n && mutex_trylock(&sess->pump_lock)) {
                int err = tbt_carrier_send_raw(c, b, n);

                mutex_unlock(&sess->pump_lock);
                if (!err) {
                    c->last_tx_ns = ktime_get_ns();
                    atomic64_inc(&sess->drip_tx);
                }
            }
            /* trylock 失败：pump 正拥有这条字节流，下一个 50 ms pass 再 drip。
             * keepalive 允许晚一个周期，健康检查不允许被 pump 拖住。 */
```

`trylock` 而不是 `mutex_lock`：poll 是 liveness 网，不能被一个正在等 socket lock 的 pump 卡住。

#### 1.2.5 `sk_user_data` / callbacks 与 RCU 的交互

两套机制必须分开看，混用会得出错误的结论：

| 机制 | 保护对象 | 同步原语 | 谁等谁 |
|---|---|---|---|
| `sk_user_data` + 四个 callback | `struct tbt_carrier *c` | `sk_callback_lock`（`read_lock_bh` / `write_lock_bh`） | `tbt_sk_ops_restore()` 的 `write_lock_bh` 等待所有在途 callback |
| `c->sk` | `struct sock *` | RCU + `sock_hold` | `synchronize_rcu()` 等待所有在途 `rcu_dereference` |

因此 release 的顺序是**先 callback、后指针、再 socket**：

```
1. atomic_set(stopping, 1)                 /* 新 callback 看到后立即返回 */
2. cancel_work_sync(w_connect)
3. cancel_work_sync(w_rx)                  /* 停掉正在读 socket 的 work */
4. tbt_sk_ops_restore()                    /* write_lock_bh：等完在途 callback */
5. sock_replace_proto(&tcp_prot)           /* sk_prot->destroy 不再指向模块 */
6. cancel_work_sync(w_rx)                  /* 收掉 4 之前排队的 w_rx */
7. rcu_assign_pointer(c->sk, NULL)
8. synchronize_rcu()                       /* 等完在途 c->sk 读者 */
9. abort / shutdown / sock_release()
```

为什么第 4 步在第 7 步之前：`tbt_car_write_space()` 在 callback lock 内会 `schedule_work(&c->w_tx)` 和 `schedule_work(&c->sess->w_fec_tx.work)`；先 restore 才能保证“restore 返回后不会再有新的调度”。如果先清 `c->sk`，callback 仍可进入并对 `c->sess` 做计数（session 可能正在销毁），只是不会 UAF；先 restore 更干净。

为什么第 8 步在第 9 步之前：见 1.2.3 的论证。第 8 步之后，`c->sk` 的读者要么已经持有引用，要么永远拿不到。

softirq 不会进入已释放对象的三重保证：

1. `sk_user_data` 在 `sk_callback_lock` 写锁下清空，`write_lock_bh` 等完所有读者；此后 callback 看到 NULL 直接返回。
2. `sock_replace_proto(&tcp_prot)` 保证即使 socket 在 FIN_WAIT/TIME_WAIT 里被内核异步销毁，`sk_prot->destroy()` 也是 `tcp_prot`，不是模块代码。
3. `synchronize_rcu()` + `sock_hold` 保证 `c->sk` 的读者在 `sock_release()` 之后仍持有有效 `struct sock`，直到自己 `sock_put()`；模块卸载前 `tbt_core.c` 的 `synchronize_rcu(); rcu_barrier();` 收尾。

### 1.3 迁移步骤

**这一节是一个原子提交**：字段类型、所有读点、release 顺序必须同时落地。任何中间状态都留下 UAF 窗口（旧顺序下读者 `inc_not_zero` 可能作用于已释放对象；新顺序下未改的裸读者可能解引用已释放对象）。提交内部按以下顺序改，便于 review：

1. `tbtun.h`：`sk` → `struct sock __rcu *sk`；加 helper 声明；`pollers` → `users`（§5 先做则此处只是改名）。
2. `tbt_carrier.c:2175-2330`、`:2464-2520`：两处安装改为 `rcu_assign_pointer()`；adopt 的发布顺序按 §3 调整。
3. `tbt_carrier.c:800-1279`：写路径改局部 `sk` + `lockdep_assert_held`；`tbt_car_poll_one()` 的 drip 改 `mutex_trylock`。
4. `tbt_carrier.c:1476-1533`：拆 `unpublish/close`；`tbt_replace_work()` 与 `tbt_carriers_stop()` 改批量形态。
5. 读路径逐点改：`tbt_car_poll_one`（285/303/350）、`tbt_car_rx_work`（686/770）、`tbt_carrier_dead`（2085）、`tbt_carrier_resume`（2378/2385 改 `rcu_access_pointer`）、`tbt_carrier_snapshot`（2404-2416 单次 get/put）、`tbt_carrier_lost`（1423 改 `rcu_access_pointer`）、`tbt_session.c:397/414/547/1331`、`tbt_rx.c:689/721`、`tbt_tx.c:539/601/737/1171/1825/2385/2737/2801/3017`。
6. `tbt_session.c:547` 与 `tbt_carrier.c:2199-2205` 的 `c->sock` 存在性改 `rcu_access_pointer(c->sk)`；此后 `c->sock` 只剩安装者与 `close` 两个读者。
7. 删除 `tbt_carrier_release()` 的单体实现（若无调用者）或保留薄封装。

### 1.4 失败路径

| 失败 | 行为 |
|---|---|
| `tbt_carrier_sk_get()` 返回 NULL | 写路径 `-ENOTCONN`；rx work 直接返回；poll 走 `tbt_carrier_lost()`；FBK 跳过该 entry；snapshot 保留上一次快照 |
| `sock->sk == NULL`（socket 半安装） | `unpublish` 跳过 callback restore，仍清 `c->sk`；`close` 只做 `sock_release` |
| release 被调用两次 | 第二次 `c->sock == NULL`，直接返回；`released` 已置位 |
| 读者持引用时 `sock_release()` | `tcp_close()` 取 socket lock，与读者的 `tcp_sendmsg_locked()` 串行；读者随后看到 `TCP_CLOSE`，返回 `-EPIPE`，无 UAF |
| `synchronize_rcu()` 期间有读者 | 读者不睡眠、不取锁，grace period 有界；不会死锁 |
| `queue_work`/callback 在 restore 后重排 w_rx | `work_busy` 阻止复用（§5）；w_rx 入口检查 `stopping || !sk` 后返回 |

### 1.5 回归风险

- **替换延迟**：每个 carrier 一个 `synchronize_rcu()` 会让 16-carrier 的替换 pass 多出数十毫秒。批量形态把它降为每批一个 grace period；必须按批量落地，否则 HND0 的替换风暴会回归。
- **`lockdep_assert_held` 误报**：任何遗漏的调用路径（例如未来从 softirq 调 `send_raw`）会立刻炸出来，这是有意的；不要用 `lockdep_off()` 绕过。
- **`__rcu` 与 sparse**：`READ_ONCE(c->sk)` 在 `__rcu` 字段上会被 sparse 警告；迁移必须清干净，否则 `make C=1` 失败。
- **`refcount_inc_not_zero` 的 NULL 语义**：旧代码把 `!c->sk` 当作“未连接”；新 helper 的 NULL 还包含“正在 release”。每个调用点要确认 NULL 分支的行为与旧代码一致（尤其 `tbt_carrier_send_fec` 的 `-ENOTCONN` 会让 FEC 记录留在队列里重试，这是期望行为）。
- **poll drip 改 `trylock`**：drip 可能晚一个 50 ms 周期；`drip_tx` 计数会略降。若中间盒对 keepalive 间隔敏感，把 `drip_ms` 的下限提高，而不是回到无锁写。

### 1.6 验证

- **host（能力有限，如实说明）**：这些路径无法在 host 单测覆盖。host 侧只有 `make W=1`、`make C=1`（sparse 的 `__rcu` 检查）、`review/lint.sh`。`BUILD_BUG_ON` 不覆盖指针生命周期。
- **QEMU（`adversarial/`，KASAN + lockdep + refcount 全开）**：
  - `far.garbage`、`far.hello_garbage`、`far.split_record`、`far.chunk_huge`：每个都触发 carrier 关闭，覆盖 unpublish 与 rx work 的竞争。
  - `near.fin_eagain`、`near.rst_eagain`：写路径与 release 并发。
  - `init.rmmod`（8 MiB 传输中 `rmmod`）：最强的一条，覆盖 callback/RCU/sock 三层。
  - `init.flush`、`create.replace`、`create.failure`：会话销毁路径。
  - `init.slab_leak`：100 个未认证连接后 `slabinfo` 不增长。
  - 全量 `ADV_KLOG` 扫描不得出现 `refcount_t:`、`KASAN`、`WARNING:`。
- **真机（HND0）**：替换风暴（`carrier_stall_ms` 调小逼出连续替换）+ 同时 `cat /proc/net/tbtun/peers` 循环；10 s 传输后 `rmmod`；`dmesg` 无 `refcount_t:`/`KASAN`/`Call Trace`。

### 1.7 交付清单

| 项 | 内容 |
|---|---|
| 改动文件:行 | `tbtun.h:1122-1124`；`tbt_carrier.c:100/121/168/285/303/350/500/686/770/808-810/834-836/862-878/1018/1067-1264/1418-1423/1476-1533/2085/2199-2205/2257-2258/2378-2385/2404-2416/2485-2486`；`tbt_tx.c:539-540/601-603/737-740/1171-1173/1825-1827/2385-2440/2737/2801-2804/3017-3019`；`tbt_session.c:397/414/547/1331`；`tbt_rx.c:689/721-722` |
| 函数签名 | `static inline struct sock *tbt_carrier_sk_get(struct tbt_carrier *c);` / `static inline void tbt_carrier_sk_put(struct sock *sk);` / `static void tbt_carrier_unpublish(struct tbt_session *, struct tbt_carrier *);` / `static void tbt_carrier_close(struct tbt_carrier *);` / `static int tbt_carrier_part_flush(struct tbt_carrier *, struct sock *);` / `static int tbt_carrier_send_whole(struct tbt_carrier *, struct sock *, const void *, size_t);` / `static int tbt_carrier_http_begin(struct tbt_carrier *, struct sock *);` |
| 锁顺序 | `pump_lock → socket lock`（写）；`sk_callback_lock` 叶子；RCU 读段叶子；`synchronize_rcu()` 不得在 `sess->lock` 内 |
| 失败路径 | 见 1.4 |
| 回归风险 | 见 1.5 |
| 验证 | 见 1.6 |

---

## 2. stream work 引用

### 2.1 现状（所有排队点）

`struct tbt_stream` 的 work：`w_read`、`w_drain`、`w_ctrl`、`w_kill`、`w_target`（`tbtun.h:295-301`）。

| 文件:行 | 目标 | 上下文 | 今天是否持引用 |
|---|---|---|---|
| `tbt_sock.c:154` | `st->w_read` | `tbt_pub_ready()` softirq | 否 |
| `tbt_sock.c:165` | `st->w_drain` | `tbt_pub_write_space()` softirq | 否 |
| `tbt_sock.c:173` | `st->w_ctrl` | `tbt_pub_state_change()` softirq | 否 |
| `tbt_sock.c:214` | `st->w_drain` | `tbt_sock_kick()` | 否 |
| `tbt_sock.c:371` | `st->w_read` | `tbt_ctrl_work()` | 否（自身无引用） |
| `tbt_sock.c:376` | `st->w_drain` | `tbt_ctrl_work()` | 否 |
| `tbt_sock.c:462-463` | `w_read`/`w_drain` | `tbt_sock_attach()` | 否 |
| `tbt_sock.c:627` | `st->w_read` | `tbt_stream_start_now()` | 否 |
| `tbt_rx.c:362` | `st->w_drain` | `tbt_rx_deliver()`（调用者持 ref） | 否 |
| `tbt_carrier.c:543` | `st->w_drain` | `tbt_car_poll_one()` RCU 遍历 | 否 |
| `tbt_tx.c:2822` | `it->w_read` | `tbt_tx_pump()` RCU 遍历 | 否 |
| `tbt_fec_data.c:651` | `st->w_read` | `tbt_fec_tx_work()`（持 ref） | 否 |
| `tbt_carrier.c:2591` | `st->w_target` | `tbt_carrier_open_target()` | **是**（`refcount_inc` + 入口 `put`） |
| `tbt_session.c:953` | `st->w_kill` | `tbt_stream_kill()` | **是**（同上） |

**已有的两个引用点也不完整**：

- `tbt_stream_kill()` 用 `DYING` 位保证只排队一次，但 `tbt_stream_release_socket()` 的 `cancel_work_sync(&st->w_kill)` 取消一个尚未运行的实例时，`tbt_stream_kill()` 取的引用永远不会归还 → stream 泄漏，session 引用（§4）永远不归零。
- `tbt_carrier_open_target()` 用 `queue_work()` 的返回值回滚，但 `target_started` 已经置位，失败后不会重试。
- `w_read`/`w_drain`/`w_ctrl` 完全没有引用：`tbt_tx_pump()` 的 RCU 遍历可以在 reaper 的 `cancel_work_sync()` 之后 `schedule_work(&it->w_read)`；reaper 随后丢掉 hash 引用，work 在已释放的 stream 上运行。这是真实的 UAF 窗口，不是理论问题。

### 2.2 设计

#### 2.2.1 位家族

一个 bit 无法区分“哪个 work 持有引用”，两个 work 同时排队会共用一份引用、先结束的放掉它、后结束的 UAF。因此每个 work 一个 bit：

```c
/* attached 的 bit 4..8：每个 bit 表示“该 work 有一个实例已排队或正在运行，
 * 且该实例恰好持有一个 stream 引用”。bit 在入口第一行清除，因此运行期间
 * 到达的唤醒会重新置位并取第二个引用，两次运行各还各的。 */
#define TBT_STREAM_F_SCHED_READ   4
#define TBT_STREAM_F_SCHED_DRAIN  5
#define TBT_STREAM_F_SCHED_CTRL   6
#define TBT_STREAM_F_SCHED_KILL   7
#define TBT_STREAM_F_SCHED_TARGET 8
```

`BUILD_BUG_ON(TBT_STREAM_F_SCHED_TARGET >= BITS_PER_LONG);` 放进 `tbt_carrier_init()`。

#### 2.2.2 排队 helper

```c
/* 返回 false 表示这次唤醒没有排上队，调用者必须把它当作“丢失一次唤醒”。
 * 50 ms session poll 会重新武装每个有工作的队列，所以丢失唤醒只意味着延迟。 */
static bool tbt_stream_schedule(struct tbt_stream *st, struct work_struct *w,
                                unsigned long bit)
{
    if (test_and_set_bit(bit, &st->attached))
        return true;                    /* 已有一个实例持有这次唤醒 */
    if (!refcount_inc_not_zero(&st->refs)) {
        clear_bit(bit, &st->attached);
        return false;                   /* stream 已在退出路径上 */
    }
    if (likely(queue_work(TBT_WQ, w)))
        return true;
    /* 位是清的而 work 已 pending：只可能是漏改的直接 queue_work()。
     * 回滚引用，避免双重排队泄漏。 */
    WARN_ON_ONCE(1);
    clear_bit(bit, &st->attached);
    tbt_stream_put(st);
    return false;
}
```

`refcount_inc_not_zero()` 而不是 `refcount_inc()`：`tbt_tx_pump()` 和 `tbt_car_poll_one()` 的 RCU 遍历不持引用，reaper 可能已经丢掉最后一个引用；RCU 保证内存仍可读，`inc_not_zero` 保证不会把 0 加回 1。

#### 2.2.3 入口与取消

入口统一为“清位 → 原 body → 放引用”。为了不改动大段函数体，把现有函数改名为 `_body`，外面套一层：

```c
static void tbt_pub_capture_work(struct work_struct *w)
{
    struct tbt_stream *st = container_of(w, struct tbt_stream, w_read);

    clear_bit(TBT_STREAM_F_SCHED_READ, &st->attached);
    tbt_pub_capture_body(st);
    tbt_stream_put(st);
}
```

五个 work 同形。规则：**body 内不得再 `tbt_stream_put()`**（今天 `tbt_target_work_fn()` 和 `tbt_kill_work()` 各有一个手动 `put`，迁移时删除）。

取消：

```c
/* cancel_work_sync() 返回 true ⇔ 它取消了一个 pending 实例；该实例的入口
 * 永远不会运行，所以它的引用由取消者归还。返回 false 时，实例要么已经跑完
 * （入口已 put），要么在 cancel 返回后重新排队（新实例自己 put）。
 * 因此不能无条件 clear+put：那会抢走新实例的引用。 */
static bool tbt_stream_cancel(struct tbt_stream *st, struct work_struct *w,
                              unsigned long bit)
{
    bool canceled = cancel_work_sync(w);

    if (canceled && test_and_clear_bit(bit, &st->attached))
        tbt_stream_put(st);
    return canceled;
}
```

**schedule 失败回滚**：`tbt_stream_schedule()` 内部完成；调用者只看返回值。`tbt_stream_kill()` 若返回 false，清掉 `DYING` 让后续 kill 能重试；`tbt_carrier_open_target()` 若返回 false，在 `sess->lock` 下清 `target_started`。

#### 2.2.4 状态机（谁置位/清位）

| 事件 | bit | 引用 | 说明 |
|---|---|---|---|
| `tbt_stream_schedule()` | `test_and_set` | 成功置位后 `inc_not_zero` | 位已置位则不加引用、不排队 |
| work 入口第一行 | `clear_bit` | — | 运行期间的新唤醒可以重新置位 |
| work 入口最后一行 | — | `tbt_stream_put` | 与入口的清位配对 |
| `tbt_stream_cancel()` | 仅在 `canceled` 时 `test_and_clear` | 仅在 `canceled` 时 `put` | 取消 pending 实例 |
| `tbt_stream_put()` 归零 | — | `call_rcu` 释放 | 所有 work 引用都已归还 |

不变量：**`bit == 1` ⇔ 存在一个尚未进入入口的实例，且它持有一个引用。** 入口清位后、`put` 前，实例正在运行，不计入 bit；此时新的 schedule 会取第二个引用。

### 2.3 `tbt_sock_detach()` 的取消顺序

detach 本身只负责 `w_ctrl/w_read/w_drain`；`w_target` 和 `w_kill` 由调用者在 detach 之前取消。完整全序：

```
w_target → w_ctrl → w_read → w_drain → w_kill
```

| 顺序 | work | 原因 |
|---|---|---|
| 1 | `w_target` | 唯一会 `tbt_sock_attach()` 的 work：它会重新安装 callbacks 并 `schedule_work(w_read/w_drain)`。不先停它，detach 之后会被重新 attach。 |
| 2 | `w_ctrl` | 唯一会在内部 `schedule_work(&st->w_read)` / `w_drain` 的 work（`tbt_sock.c:371,376`）。先停它，后面的取消才不会被它重新排队。 |
| 3 | `w_read` | TX 生产者：向 session 队列提交 DATA/FIN，并可能 `tbt_tx_fin()`。先停它，避免 detach 之后还有新帧进入队列。 |
| 4 | `w_drain` | RX 消费者：持 `rx_q`，错误路径会 `tbt_stream_kill()`（排队 `w_kill`）。必须在 `w_kill` 之前停，否则它会重新排队一个刚被取消的 kill。 |
| 5 | `w_kill` | 终结动作，可能由 `w_drain` 排队；必须最后取消。**不能由自己取消自己**：`tbt_kill_work()` 走 `tbt_stream_release_socket_now()`，它不 cancel `w_kill`。 |

`tbt_sock_detach()` 新形态：

```c
void tbt_sock_detach(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (!sk)
        return;
    if (!test_and_clear_bit(TBT_STREAM_F_ATTACHED, &st->attached))
        return;
    tbt_sk_ops_restore(sk, &st->sess->def_ops);

    tbt_stream_cancel(st, &st->w_ctrl,  TBT_STREAM_F_SCHED_CTRL);
    tbt_stream_cancel(st, &st->w_read,  TBT_STREAM_F_SCHED_READ);
    tbt_stream_cancel(st, &st->w_drain, TBT_STREAM_F_SCHED_DRAIN);
}
```

调用者：

- `tbt_stream_release_socket()`：`tbt_stream_cancel(w_kill)` → `tbt_sock_detach()`。
- `tbt_kill_work()`：`tbt_stream_cancel(w_target)` → `tbt_stream_release_socket_now()`（不 cancel `w_kill`）。
- `tbt_session_destroy()` 的 stream 循环：`tbt_stream_cancel(w_target)` → `tbt_stream_cancel(w_kill)` → `tbt_sock_detach()`。

### 2.4 迁移步骤

1. `tbtun.h`：加 5 个 bit；加 `tbt_stream_schedule/cancel` 原型（或 static inline）。
2. `tbt_sock.c`：实现 helper；`w_read/w_drain/w_ctrl` 改 `_body` + wrapper；`w_kill/w_target` 改用 helper 并删掉 body 内的手动 `put`。
3. 逐个替换 2.1 表中的 13 个排队点。`tbt_tx_pump()` 与 `tbt_car_poll_one()` 的 RCU 遍历保持“清 `tx_paused` → schedule”，schedule 失败即 stream 将死，不再处理。
4. `tbt_sock_detach()` 改取消顺序；`tbt_stream_release_socket()`、`tbt_kill_work()`、`tbt_session_destroy()` 按 2.3 调整。
5. 删除 `tbt_stream_kill()` 里的裸 `refcount_inc/queue_work` 与 `tbt_carrier_open_target()` 的裸 `queue_work`。
6. `review/lint.sh` 增加一条：`grep -rn 'schedule_work(&st->\|queue_work(TBT_WQ, &st->' tbtun/` 必须为空。

### 2.5 失败路径与回归风险

| 场景 | 行为 |
|---|---|
| `queue_work` 返回 false | `WARN_ON_ONCE` + 清位 + put；唤醒丢失，由 50 ms poll 兜底 |
| `refcount_inc_not_zero` 失败 | stream 已在退出路径；不排队，不写 `tx_paused` |
| `cancel_work_sync` 取消 pending 实例 | 取消者 put；不会泄漏 |
| `cancel_work_sync` 与 schedule 竞争 | schedule 要么看到位已置位（丢失一次唤醒），要么在 cancel 返回后重新置位（新引用）；两种都不会双 put |
| work 在 detach 之后被重新排队 | 持引用，stream 不释放；入口看到 `st->sk == NULL` 或 `stopping` 后返回；session 引用（§4）保证 `st->sess` 有效 |

回归风险：

- **唤醒丢失**：位去重天然会丢一次并发唤醒。`w_read` 丢一次会晚到下一个包/50 ms poll；`w_drain` 丢一次会晚到 `sk_write_space`/poll。两者都有兜底，但必须确认 `tbt_car_poll_one()` 的 rx_q 检查仍在（`tbt_carrier.c:540-548`）。
- **引用计数开销**：每条 DATA 帧的捕获路径多一次 `refcount_inc/dec`。相对 `tcp_read_sock()` 可忽略；若在意，可只在“可能并发 detach”的路径（softirq、pump、poll）使用，`tbt_stream_start_now()` 的首次排队可以保留直接 `schedule_work`——但不建议，统一才有机械可查性。
- **`canceled` 语义**：`cancel_work_sync()` 的返回值是“是否取消了一个 pending work”；不要用它判断“work 是否运行过”。代码里只有 `canceled && test_and_clear_bit` 这一种用法。

### 2.6 验证

- **host**：`tbt_stream` 的 work 逻辑不在 host 单测内；host 只能跑 `make W=1` 与 lint 的新 grep 规则。
- **QEMU（adversarial）**：`pub.churn`（10000 次连接/关闭，覆盖 reaper 与 `w_read` 的竞争）、`pub.concurrent`（100 并发）、`pub.rst`（RST 中途）、`init.flush`、`far.open_other_sid`、`near.second_conn`。KASAN + `CONFIG_REFCOUNT_FULL` 下不得出现 `refcount_t:` 或 UAF。
- **真机**：`pub.churn` 长跑 + 同时反复 `tunnel del/add`；结束后 `slabinfo` 中 `tbt_stream` 相关 slab 回到基线。

### 2.7 交付清单

| 项 | 内容 |
|---|---|
| 改动文件:行 | `tbtun.h:295-301,310-320`（bit 定义）；`tbt_sock.c:146-176/212-216/344-382/394-401/409-430/432-464/466-484/526-568/579-631`；`tbt_rx.c:362`；`tbt_carrier.c:543/2591`；`tbt_tx.c:2822`；`tbt_fec_data.c:651`；`tbt_session.c:953` |
| 函数签名 | `static bool tbt_stream_schedule(struct tbt_stream *, struct work_struct *, unsigned long);` / `static bool tbt_stream_cancel(struct tbt_stream *, struct work_struct *, unsigned long);` |
| 锁顺序 | helper 可在任意上下文调用（`refcount` + `queue_work` 均原子）；`cancel_work_sync` 必须在无 `sess->lock`/`pump_lock` 时调用 |
| 失败路径 | 见 2.5 |
| 回归风险 | 见 2.5 |
| 验证 | 见 2.6 |

---

## 3. carrier adopt / prov

### 3.1 现状（文件:行）

`tbt_carrier_adopt()`（`tbt_carrier.c:2464-2520`）的当前顺序：

```c
    c = tbt_carrier_new(sess, 0xff);
    if (!c)
        return -ENOMEM;
    c->provisional = true;
    tcp_sk(newsock->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(newsock->sk);
    c->sock = newsock;
    c->sk = newsock->sk;

    write_lock_bh(&newsock->sk->sk_callback_lock);
    newsock->sk->sk_user_data = c;
    newsock->sk->sk_data_ready = tbt_car_ready;
    newsock->sk->sk_write_space = tbt_car_write_space;
    newsock->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&newsock->sk->sk_callback_lock);

    tbt_car_setsockbuf(newsock, &sess->tunnel->cfg, sess->n_pri);
    atomic_set(&c->connected, 1);
    spin_lock(&sess->lock);
    {
        int n = atomic_inc_return(&sess->n_prov);

        if ((u32)n > sess->prov_peak)
            sess->prov_peak = (u32)n;
    }
    list_add_tail(&c->node, &sess->prov);
    spin_unlock(&sess->lock);
    sess->rx_adopt_events++;
    WARN_ON_ONCE(newsock->sk->sk_data_ready != tbt_car_ready);
    WARN_ON_ONCE(newsock->sk->sk_prot == &tcp_prot);
    tbt_car_schedule_rx(c);
```

缺陷链条：

1. callbacks 在挂上 `prov` **之前**安装。softirq 可以立刻进入 `tbt_car_ready()` → `tbt_car_schedule_rx()` → `w_rx` → `tbt_rx_hello()` → `tbt_session_join()`。
2. join 在 `sess->lock` 下把 `c` 放进 `sess->car[index]`，执行 `list_del_init(&c->node)`、`c->provisional = false`、`n_prov--`。
3. adopt 恢复执行，`list_add_tail(&c->node, &sess->prov)` 把一个**已经 joined、已经在 `car[]` 里**的 carrier 重新挂回 `prov`，并 `n_prov++`。
4. `tbt_replace_work()` 的 prov 清扫（`tbt_carrier.c:1932-1955`）：

```c
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        if (atomic_read(&c->stopping) || now > c->join_deadline_ns)
        {
            atomic_set(&c->stopping, 1);
            list_move_tail(&c->node, &victims);
            atomic_dec(&sess->n_prov);
            ...
        }
    }
```

`join_deadline_ns` 在出生时固定为 `now + 5 s`。5 秒后，这个**在用的** carrier 被当成过期 provisional 移入 `victims`，随后 `tbt_carrier_release()` + `tbt_carrier_free()` —— 而它仍在 `sess->car[]` 里，pump 下一帧就会写已释放内存。这是 0.2.0 必须关闭的 UAF。

`tbt_session_join()` 的 prov 段（`tbt_session.c:1385-1393`）：

```c
    if (c->provisional)
    {
        /* Leaves the bounded provisional list; the cap in
         * tbt_carrier_adopt() counts it until now. */
        c->provisional = false;
        atomic_dec(&sess->n_prov);
    }
    list_del_init(&c->node);
```

它假设 `c` 一定在 `prov` 上；在 adopt 的旧顺序下这个假设不成立。

`n_prov` 的算术今天没有错，错的是**顺序**：join 的 `dec` 可能发生在 adopt 的 `inc` 之前（计数瞬时为 -1），并且 `prov_peak` 会记下错误的高水位。

### 3.2 设计

#### 3.2.1 adopt 的新顺序

原则：**先让对象在 `prov` 上可见，再让任何 callback 能拿到它。**

```c
int tbt_carrier_adopt(struct tbt_tunnel *t, struct socket *newsock)
{
    struct tbt_session *sess = t->sess;
    struct tbt_carrier *c;

    if (!sess)
        return -ENOTCONN;
    /* 便宜的预检：不分配 37 KiB 就能拒绝洪水。权威检查在锁内。 */
    if (atomic_read(&sess->n_prov) >= (int)tbt_prov_cap(sess)) {
        sess->prov_reject++;
        return -ENOSPC;
    }
    c = tbt_carrier_new(sess, 0xff);
    if (!c)
        return -ENOMEM;

    c->provisional = true;
    tcp_sk(newsock->sk)->nonagle = TCP_NAGLE_OFF;
    tbt_carrier_install_proto(newsock->sk);
    c->sock = newsock;
    rcu_assign_pointer(c->sk, newsock->sk);

    /* 1) 先发布到 prov。此刻还没有任何 callback 指向 c，所以不会有
     *    w_rx 与 join 并发。 */
    spin_lock(&sess->lock);
    if (sess->closing ||
        atomic_read(&sess->n_prov) >= (int)tbt_prov_cap(sess)) {
        spin_unlock(&sess->lock);
        sess->prov_reject++;
        tbt_carrier_unpublish(sess, c);   /* 恢复继承来的 listener callbacks */
        synchronize_rcu();
        tbt_carrier_close(c);
        tbt_carrier_free(c);
        return -ENOSPC;
    }
    {
        int n = atomic_inc_return(&sess->n_prov);

        if ((u32)n > sess->prov_peak)
            sess->prov_peak = (u32)n;
    }
    list_add_tail(&c->node, &sess->prov);
    spin_unlock(&sess->lock);

    /* 2) 只有现在，softirq 才能通过 sk_user_data 找到 c。 */
    write_lock_bh(&newsock->sk->sk_callback_lock);
    newsock->sk->sk_user_data = c;
    newsock->sk->sk_data_ready = tbt_car_ready;
    newsock->sk->sk_write_space = tbt_car_write_space;
    newsock->sk->sk_state_change = tbt_car_state_change;
    write_unlock_bh(&newsock->sk->sk_callback_lock);

    tbt_car_setsockbuf(newsock, &sess->tunnel->cfg, sess->n_pri);
    atomic_set(&c->connected, 1);
    sess->rx_adopt_events++;
    tbt_car_schedule_rx(c);
    ...
}
```

第 1) 与第 2) 之间，accepted socket 仍带着从 listener 继承的 `sk_user_data = t` 和 `tbt_ready()`。一个到达的包会调用 `tbt_ready()`，它把 socket 当作 listener，`schedule_work(&t->w_accept)`——无害（accept 循环会再 accept 一次或拿到 `-EAGAIN`），且**不会触碰 `c`**。这是可接受的过渡窗口；它比“callback 先看到 `c` 但 `c` 不在 `prov`”安全得多。

拒绝路径复用 `unpublish + synchronize_rcu + close + free`：`unpublish` 的 `tbt_sk_ops_restore(s->sk, &sess->def_ops)` 会把继承来的 `tbt_ready()` 换回 core callbacks，并清掉 `sk_user_data = t`。不能只 `sock_release()`，否则 listener 的模块回调会留在 socket 上。

#### 3.2.2 `joined` 标志

- **位置**：`struct tbt_carrier`，紧挨 `provisional`：

```c
    bool provisional;             /* accepted, not yet joined to a slot */
    bool joined;                  /* join() installed it in sess->car[] */
```

- **谁置位**：`tbt_session_join()`，在 `sess->lock` 内，与 `sess->car[index] = c` 同一个临界区。
- **谁清位**：没有人。对象复用时由 `memset` 归零（§5）。
- **语义**：`joined ⇒ 在 car[] 且不在 prov`；`provisional ⇒ 在 prov 且不在 car[]`；两者皆 false ⇒ 已退休。
- **`w_replace` 如何跳过**：在 prov 清扫的循环体最前面：

```c
    list_for_each_entry_safe(c, tmp, &sess->prov, node)
    {
        if (c->joined) {
            /* join() 已经把它装进 car[]，而 adopt() 的旧顺序把它又挂回了
             * prov。它不是 victim：只摘链，不置 stopping、不释放。 */
            WARN_ON_ONCE(1);      /* 新顺序下不可达；命中就是顺序回归 */
            list_del_init(&c->node);
            continue;
        }
        if (atomic_read(&c->stopping) || now > c->join_deadline_ns) {
            ...
        }
    }
```

`tbt_carriers_stop()` 的 prov 循环同样加这个 skip（防御性；新顺序下不可达）。

#### 3.2.3 `n_prov` 一致性规则

1. `n_prov` **恰好**等于 `sess->prov` 上的元素个数。
2. 每一次链表变更和它的计数更新在**同一个 `sess->lock` 临界区**内完成：
   - adopt：`list_add_tail` + `atomic_inc_return`（同一个临界区）；
   - join：`list_del_init` + `atomic_dec`（同一个临界区）；
   - w_replace：`list_move_tail` + `atomic_dec`（同一个临界区）；
   - carriers_stop：全部 `list_move_tail` 后 `atomic_set(0)`（同一个临界区）。
3. 锁外只允许“advisory”读取（adopt 的预检）；权威检查在锁内。
4. `prov_peak` 与 `n_prov` 的增量在同一临界区内更新。
5. `tbt_prov_cap()` 只读 `sess->n_car`（create 后不变），可在锁外调用。

### 3.3 迁移步骤

1. `tbtun.h`：加 `bool joined;`；`prov_peak` 注释改为“受 sess->lock 保护”。
2. `tbt_carrier.c:2464-2520`：按 3.2.1 重排；拒绝路径用 `unpublish/sync/close/free`。
3. `tbt_session.c:1385-1393`：`c->provisional = false; c->joined = true;` + `list_del_init` + `atomic_dec` 同一个临界区。
4. `tbt_carrier.c:1932-1955`：加 `joined` skip + `WARN_ON_ONCE`。
5. `tbt_carrier.c:2008-2075`：prov 循环加同样的 skip。
6. `tbt_carrier.c:1303-1390`：`tbt_carrier_reset()` 的 memset 覆盖 `joined`（§5 先做则自动成立）。

### 3.4 失败路径与回归风险

| 场景 | 行为 |
|---|---|
| 预检通过、锁内检查失败（并发 adopt） | `prov_reject++`；`unpublish/sync/close/free`；socket 被 RST 关闭 |
| `tbt_carrier_new()` 失败 | `-ENOMEM`，socket 由 listener 的拒绝路径关闭（`tbt_listen.c:88-97`） |
| HELLO 在 adopt 返回前到达 | 现在一定在 `prov` 上；join 摘链、置 `joined`、装 `car[]`；adopt 的后续代码不再触碰 `node` |
| HELLO 与 w_replace 清扫竞争 | 两者都在 `sess->lock` 下；join 检查 `stopping`，清扫先置 `stopping` 则 join 拒绝；join 先摘链则清扫看不到 |
| 5 秒后 joined carrier 仍在 prov | 新顺序下不可能；`WARN_ON_ONCE` 命中即顺序回归 |

回归风险：

- **adopt 的过渡窗口**：callbacks 安装前，包会走 listener 的 `tbt_ready()`。这会让 `w_accept` 多跑一次；在 accept 洪水下可能多一次唤醒。可接受，但要在 `far.dup_index` 下确认没有 accept 饥饿。
- **`unpublish` 在拒绝路径**：它 `cancel_work_sync(w_rx)`（从未排队，开销可忽略）并 `synchronize_rcu()`；拒绝洪水下每个被拒连接多一个 grace period。`far.chunk_huge`/`init.slab_leak` 会压这条路径；若成为瓶颈，拒绝路径可以只 `tbt_sk_ops_restore + sock_release`（没有 `c->sk` 发布给任何读者，不需要 grace period）——但前提是 `c->sk` 从未被 `rcu_assign_pointer` 发布。设计上建议：**拒绝路径先 restore、再发布 `c->sk`**，这样拒绝时无需 sync。上面 3.2.1 的代码为了统一先发布了；落地时可把 `rcu_assign_pointer` 移到锁内检查通过之后。
- **`n_prov` 计数**：`atomic_t` 改成锁内普通 `u16` 也可以，但 `/proc` 的 advisory 读需要 `READ_ONCE`；保留 `atomic_t` 更省事。

### 3.5 验证

- **host**：`test_anchor`（124 项）覆盖 nonce/anchor 的纯逻辑；adopt 顺序不在 host 内。
- **QEMU（adversarial）**：`far.dup_index`、`far.srcaddr_free`、`far.nonce_pinned`、`far.src_anchor`、`far.second_carrier`、`init.slab_leak`（100 个未认证连接，验证 prov 上限与拒绝路径）、probe O（carrier replacement）。检查 `prov_reject`/`prov_peak` 与 `dmesg` 无 `WARN_ON_ONCE`。
- **真机**：连接洪水 + 替换风暴同时进行；`/proc/net/tbtun/stats` 的 `prov_peak` 不超过 `tbt_prov_cap()`；`join_dup` 不增长。

### 3.6 交付清单

| 项 | 内容 |
|---|---|
| 改动文件:行 | `tbtun.h`（`struct tbt_carrier` 的 `provisional` 旁）；`tbt_carrier.c:2464-2520/1932-1955/2008-2075`；`tbt_session.c:1385-1393` |
| 函数签名 | `int tbt_carrier_adopt(struct tbt_tunnel *, struct socket *);`（不变）；`int tbt_session_join(struct tbt_session *, struct tbt_carrier *, u8);`（不变） |
| 锁顺序 | adopt：`sess->lock`（发布）→ `sk_callback_lock`（安装）；join：socket lock → `sess->lock`；w_replace：`pump_lock` → `sess->lock` |
| 失败路径 | 见 3.4 |
| 回归风险 | 见 3.4 |
| 验证 | 见 3.5 |

---

## 4. session 引用计数

### 4.1 现状（文件:行）

`struct tbt_session` 有 `refcount_t refs;`（`tbtun.h:701`），`tbt_session_create()` 里 `refcount_set(&s->refs, 1);`（`tbt_session.c:1046`），**此外没有任何 `get/put`**。它是死字段。

`tbt_session_destroy()`（`tbt_session.c:1189-1301`）今天的顺序：

1. `t->sess = NULL; s->closing = true;`
2. `cancel_delayed_work_sync(&s->w_fbk); ... &s->w_replace; ... &s->w_pending; ... &s->w_reap;`
3. `cancel_delayed_work_sync(&s->w_poll);`
4. `tbt_carriers_stop(s);`
5. 再次 `cancel_delayed_work_sync(&s->w_poll);`
6. 第一遍 stream：`cancel_work_sync(&v->w_target); cancel_work_sync(&v->w_kill); tbt_sock_detach(v);`
7. `tbt_tx_free_queue(s);`
8. 第二遍 stream：unlink、`tbt_stream_release_socket(victim)`、`tbt_stream_put(victim);`
9. `tbt_pending_drain(s);`
10. 再次 cancel `w_fbk`/`w_replace`；`if (s->fec) cancel_delayed_work_sync(&s->w_fec_tx);`
11. `tbt_fec_data_free(s); tbt_crypto_pair_wipe; put_net; kfree(s);`

**排队点**（session 自己拥有的 delayed work）：

| work | 排队点 |
|---|---|
| `w_reap` | `tbt_session.c:878`（detach）、`:1024`（自续）、`tbt_sock.c:423`（kill work） |
| `w_pending` | `tbt_sock.c:426`（kill work）、`:691`（自续）、`:761`（`tbt_stream_start`）、`tbt_tx.c:2702`（FIN 写出） |
| `w_fbk` | `tbt_tx.c:3102/3104`（`tbt_fbk_kick`）、`:3224`（自续） |
| `w_replace` | `tbt_carrier.c:298/308`（poll）、`:1469`（lost）、`:1769/2003`（自续）、`tbt_session.c:1430`（join） |
| `w_fec_tx` | `tbt_fec_data.c:128/528/536/634/655/681/700/1165`、`tbt_carrier.c:156`（write_space） |
| `w_poll` | `tbt_session.c:1173`（create）、`tbt_carrier.c:180`（state_change）、`:615`（自续） |

**今天靠什么保证安全**：`cancel_delayed_work_sync()` 等正在运行的实例结束；对 `w_poll` 有两次 cancel（第一次在 `carriers_stop` 前，第二次在之后），因为 `tbt_car_state_change()` 的 softirq 可以在两次之间重新 `mod_delayed_work`。这个“双 cancel”是手工的屏障，不是结构性的。

**已发现的漏洞**：

- `tbt_fec_tx_work()` 在第 10 步才被 cancel，而它在第 6-8 步之后仍可能运行；它会 `tbt_fec_stream()` 取 stream 引用、`schedule_work(&st->w_read)`、并访问 `sess`。destroy 在第 8 步之后已经把 stream unlink 并可能释放，`st->sess` 仍指向 session，但 session 在第 11 步 `kfree`。一个在第 8 步之后排队的 `w_read` 会在 session 释放后访问 `st->sess`。
- `tbt_fec_data_init()` 失败路径 `kfree(s); return err;`（`tbt_session.c:1155-1159` 附近）没有 `put_net(s->net)`，且 `t->sess` 尚未设置，调用者的 `tbt_session_destroy()` 直接返回 → net 引用泄漏。
- `tbt_stream_get()` 的发布路径不检查 `sess->closing`。`tbt_stream_start_now()` 在锁外检查一次，真正的 `hlist_add_head_rcu()` 在 `tbt_stream_get()` 内、可能发生在 closing 置位之后 → 一个在 destroy 的 stream 清扫之后才发布的 stream 永远不会被 detach。

### 4.2 设计

#### 4.2.1 持有者

| 持有者 | 取引用 | 放引用 |
|---|---|---|
| tunnel（`t->sess`） | `tbt_session_create()` 的 `refcount_set(..., 1)` | `tbt_session_destroy()` 的最后一次 `tbt_session_put()` |
| 每个已发布 stream | `tbt_stream_get()` 发布路径，在 `sess->lock` 内、`hlist_add_head_rcu()` 之前 | `tbt_stream_free_rcu()`（RCU 回调） |
| 每个 carrier 对象 | `tbt_carrier_new_locked()` 成功分配/复用时一次；复用沿用上一次的引用 | `tbt_carrier_free()` |
| 每个已排队的 session work | `tbt_session_arm()` | work 入口末尾 / `tbt_session_cancel()` |

关键点：**carrier 对象从出生到 `tbt_carrier_free()` 恰好持有一个 session 引用，跨复用不变**。这样 `tbt_carrier_free()` 无条件 `tbt_session_put()` 即可，不需要“是否取过”的标志。

#### 4.2.2 get/put 与完成量

```c
struct tbt_session
{
    ...
    refcount_t refs;
    struct completion drained;   /* refs 归零时 complete；destroy 在此等待 */
    bool closing;
    ...
};

static bool tbt_session_get(struct tbt_session *s)
{
    bool ok;

    spin_lock(&s->lock);
    ok = !s->closing && refcount_inc_not_zero(&s->refs);
    spin_unlock(&s->lock);
    return ok;
}

static void tbt_session_put(struct tbt_session *s)
{
    if (refcount_dec_and_test(&s->refs))
        complete(&s->drained);
}
```

`tbt_session_put()` **不释放** session；释放只发生在 `tbt_session_destroy()`，它在 `wait_for_completion(&s->drained)` 之后 `kfree`。这样“最后一个引用”只负责唤醒 destroy，不负责决定释放者——释放者唯一，销毁顺序可读。

#### 4.2.3 work 自持

与 §2 同构，但对象是 session 和 `delayed_work`。用一个枚举把 `dw/wq/bit` 绑在一起：

```c
enum tbt_sess_work {
    TBT_SW_POLL, TBT_SW_REPLACE, TBT_SW_PENDING,
    TBT_SW_REAP, TBT_SW_FBK, TBT_SW_FEC_TX,
};

static bool tbt_session_arm(struct tbt_session *s, enum tbt_sess_work w,
                            unsigned long delay);
static void tbt_session_cancel(struct tbt_session *s, enum tbt_sess_work w);
```

`tbt_session_arm()`：

```c
    if (test_and_set_bit(w, &s->sched))
        return mod_delayed_work(wq, dw, delay);   /* 沿用已有实例的引用 */
    if (!refcount_inc_not_zero(&s->refs))
        goto rollback;
    if (!mod_delayed_work(wq, dw, delay)) {
        /* 已 pending：位已置位，不能走到这里（与 stream 同理） */
        WARN_ON_ONCE(1);
        goto rollback;
    }
    return true;
rollback:
    clear_bit(w, &s->sched);
    tbt_session_put(s);
    return false;
```

`mod_delayed_work()` 对已 pending 的 work 返回 false，这是**正常**的“提前重定时”，不是失败。因此 arm 的实现要区分：

```c
    if (test_and_set_bit(w, &s->sched))
        return mod_delayed_work(wq, dw, delay);   /* 沿用已有实例的引用 */
```

只有“位从 0 变 1”时才取引用。

入口：

```c
static void tbt_fbk_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fbk);

    clear_bit(TBT_SW_FBK, &sess->sched);
    tbt_fbk_body(sess);
    tbt_session_put(sess);
}
```

自续规则：**先 arm（取新引用），再 put（还自己的）**。顺序反过来，session 可能在 arm 之前被释放。

`tbt_session_cancel()`：

```c
    bool canceled = cancel_delayed_work_sync(dw);

    if (canceled && test_and_clear_bit(w, &s->sched))
        tbt_session_put(s);
```

#### 4.2.4 `closing` 与引用计数的关系

- `closing` 是**准入闸**：置位后，`tbt_session_get()`、`tbt_session_arm()` 的“位从 0 变 1”路径、`tbt_stream_get()` 的发布路径都拒绝新引用。
- 引用计数是**寿命闸**：`closing` 之前已经取得的引用全部被兑现；destroy 等它们全部归还。
- 二者缺一不可：只有 `closing`，一个在置位前一刻取到的引用可能在 `kfree` 之后才归还；只有引用计数，新的引用可以无限续命。
- `closing` 的置位必须在 destroy 的任何 cancel 之前，且在 `sess->lock` 下完成（与 `tbt_session_get()` 的检查同一把锁）。读侧用 `READ_ONCE()`（arm 的快路径、work 入口）即可；权威判定在 `tbt_session_get()` 的锁内。
- **锁序注意**：`tbt_car_write_space()` 是 softirq，`tbt_car_state_change()` 也是。它们调用 `tbt_session_arm()` 时不能取 `sess->lock`（进程上下文持 `sess->lock` 时被 softirq 抢占会自死锁）。因此 arm 的快路径用 `READ_ONCE(closing)` + `refcount_inc_not_zero()`，不加锁；权威的 `tbt_session_get()` 只给进程上下文用。arm 在 closing 之后仍可能成功（检查与置位竞争），这没关系：destroy 的 cancel 会收掉它，或者 wait 会等它跑完。

### 4.3 `tbt_session_destroy()` 的精确步骤

```c
void tbt_session_destroy(struct tbt_tunnel *t)
{
    struct tbt_session *s = t->sess;
    struct tbt_stream *st;
    struct hlist_node *tmp;
    struct hlist_node *pos, *next;
    int i;

    if (!s)
        return;
    t->sess = NULL;

    /* 1. 关门。此后新引用一律拒绝。 */
    spin_lock(&s->lock);
    s->closing = true;
    spin_unlock(&s->lock);

    /* 2. 停 session 自己的定时器。每个 cancel 归还 pending 实例的引用。
     *    w_fec_tx 必须在 stream 循环之前：它是唯一会在第 6 步之后
     *    重新 schedule(&st->w_read) 的 session work。 */
    tbt_session_cancel(s, TBT_SW_POLL);
    tbt_session_cancel(s, TBT_SW_REPLACE);
    tbt_session_cancel(s, TBT_SW_PENDING);
    tbt_session_cancel(s, TBT_SW_REAP);
    tbt_session_cancel(s, TBT_SW_FBK);
    if (s->fec)
        tbt_session_cancel(s, TBT_SW_FEC_TX);

    /* 3. 停 carrier：cancel w_tx/w_pump、restore callbacks、unpublish、
     *    一个 grace period、close、free（每个 free 归还一个 session 引用）。 */
    tbt_carriers_stop(s);

    /* 4. 第一遍 stream：先停唯一会重新 attach 的 w_target，再停 w_kill，
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

    /* 5. 第二遍：unlink，释放 socket，丢 hash 引用。最后一个 stream 引用
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
            tbt_stream_cancel(victim, &victim->w_kill,
                              TBT_STREAM_F_SCHED_KILL);
            tbt_stream_release_socket(victim);
            tbt_stream_put(victim);
        }
    }

    /* 6. 还没变成 stream 的 accepted socket。 */
    tbt_pending_drain(s);

    /* 7. FEC 状态。 */
    tbt_fec_data_free(s);

    /* 8. 丢掉 tunnel 的引用，等所有 stream/work/carrier 引用归还。
     *    此后没有任何调度器能再取引用：closing 已置位，所有 work 已取消，
     *    callbacks 已恢复，carriers 已 free。 */
    tbt_session_put(s);
    wait_for_completion(&s->drained);

    /* 9. 释放。 */
    tbt_crypto_pair_wipe(&s->crypto_ctx);
    put_net(s->net);
    kfree(s);
}
```

为什么第 8 步的 wait 一定会返回：

- session work 的引用：第 2 步全部 cancel 归还；若某个实例正在运行，`cancel_delayed_work_sync` 等它结束并归还。
- carrier 的引用：第 3 步 free 归还；`carriers_stop` 覆盖 `car[]`、`prov`、`dead`、`dead_release`（后者与 `dead` 重叠）。
- stream 的引用：第 4/5 步丢 hash 引用；每个 stream 的 work 引用在第 4/5 步 cancel 归还；最后一个 `tbt_stream_put` 走 RCU 回调，在 `tbt_stream_free_rcu()` 里归还 session 引用。
- 第 5 步之后，没有任何调度器能再 `tbt_stream_schedule()`（socket callbacks 已 restore，carrier works 已 cancel，session works 已 cancel，pump 已停）。

**注意**：`tbt_stream_free_rcu()` 在 RCU 回调里执行，`wait_for_completion()` 会等它。`tbt_stream_free_rcu()` 只做 `tbt_rx_free_queue/tbt_tx_free_stream/kfree` 和 `tbt_session_put`，不取任何锁，不会与 destroy 持有的 `tbt_lock` 形成环。

### 4.4 迁移步骤

1. `tbtun.h`：`struct tbt_session` 加 `struct completion drained;` 和 `unsigned long sched;`；`refs` 注释改为“tunnel + streams + carriers + works”。
2. `tbt_session.c:1031-1187`：`kzalloc` 后立刻 `init_completion(&s->drained)`；**在 `tbt_fec_data_init()` 之前** `t->sess = s;`，失败路径统一走调用者的 `tbt_session_destroy()`（删掉 `kfree(s)`）。
3. `tbt_session.c:615-633`：`tbt_stream_free_rcu()` 加 `tbt_session_put(st->sess)`；`tbt_stream_put()` 不变。
4. `tbt_session.c:635-720`：发布路径在 `sess->lock` 内检查 `closing`、`refcount_inc_not_zero(&sess->refs)`，失败则 `kfree(st)`。
5. `tbt_carrier.c:1303-1390`：`tbt_carrier_new_locked()` 在 `kzalloc` 成功后 `refcount_inc(&sess->refs)`；复用路径不重复取。`tbt_carrier_free()` 无条件 `tbt_session_put(c->sess)`。
6. `tbt_session.c:1189-1301`：按 4.3 重写；`w_fec_tx` 的 cancel 移到 stream 循环之前；删除第二次 `w_poll` cancel（由引用计数替代；迁移期可保留并断言返回 false）。
7. 所有 session work 的排队点改 `tbt_session_arm()`：`tbt_carrier.c:180/298/308/615/1469/1769/2003`、`tbt_session.c:878/1024/1173/1430`、`tbt_sock.c:423/426/691/761`、`tbt_tx.c:2702/3102/3104/3224`、`tbt_fec_data.c:128/528/536/634/655/681/700/1165`。
8. 所有 `cancel_delayed_work_sync` 改 `tbt_session_cancel()`（destroy 与 `tbt_carriers_stop`）。
9. `tbt_session_join()` 末尾的 `tbt_fbk_kick()` 改 arm（`tbt_fbk_kick` 内部已经是 arm 的薄封装，保留它）。

### 4.5 失败路径与回归风险

| 场景 | 行为 |
|---|---|
| `tbt_session_create()` 中途失败 | `t->sess` 已设置，调用者 `tbt_session_destroy()` 统一回收；`tbt_carriers_stop()` 处理空的 `car[]`/`prov`；`tbt_fec_data_free()` 处理 NULL |
| `tbt_fec_data_init()` 失败 | 不再 `kfree(s)`；走 destroy，`put_net` 正确配对 |
| work 在 closing 之后 arm | 快路径可能成功；destroy 的 cancel 收掉，或 wait 等它跑完；work 入口看到 closing 后立即返回 |
| work 自续在 cancel 之后 | 自续先 arm 再 put；closing 已置位时 arm 拒绝；若 arm 在 closing 前成功，wait 等它 |
| stream 在 closing 之后发布 | `tbt_stream_get()` 的锁内检查拒绝，`kfree(st)` |
| `wait_for_completion` 不返回 | 说明有引用泄漏（work 取消未归还、carrier 未 free、stream 未 unlink）。迁移期加 `WARN_ON_ONCE` 与 `/proc` 的 `sess_refs` 调试字段定位 |

回归风险：

- **destroy 变慢**：等 RCU 回调归还 stream 引用，最长一个 grace period。`init.rmmod` 的 8 MiB 传输场景会压这条路径；若超过 1 s，说明有引用未归还，不是“等 RCU”。
- **`tbt_lock` 下 sleep**：`wait_for_completion()` 在 `tbt_lock` 内，和现有 `cancel_work_sync()` 一样是睡眠点；不要在任何 atomic 上下文调用 `tbt_session_destroy()`（今天也没有）。
- **`w_fec_tx` 提前 cancel**：`tbt_fec_tx_work` 可能在 `tbt_carriers_stop` 之前仍在跑，`tbt_fec_out_flush` 会取 `pump_lock`；destroy 此时还没取 `pump_lock`（`carriers_stop` 里才取），所以不冲突。
- **`tbt_session_get()` 的锁**：只在进程上下文用；softirq 走 `READ_ONCE + inc_not_zero`。不要把 `tbt_session_get()` 塞进 `tbt_car_write_space()`。

### 4.6 验证

- **host**：`tbt_session` 不在 host 单测内；host 侧只验证 `make W=1` 与 lint。
- **QEMU（adversarial）**：`init.rmmod`（传输中 `rmmod`，最强）、`init.flush`、`ctl.cfg_replay`、`create.replace`、`create.failure`、`pub.two`、`pub.churn`。KASAN + lockdep 下不得出现 `refcount_t:`/`WARNING:`；`rmmod` 必须在有界时间内返回。
- **真机**：`tunnel add/del` 循环（每次 create/destroy 一个 session）与流量并发；`rmmod`；`/proc/net/tbtun/stats` 的调试字段 `sess_refs` 在 destroy 后回到 0。

### 4.7 交付清单

| 项 | 内容 |
|---|---|
| 改动文件:行 | `tbtun.h:695-705`（session 字段）；`tbt_session.c:615-633/635-720/878/969-1029/1031-1187/1189-1301/1323-1445`；`tbt_carrier.c:180/298/308/615/1303-1390/1469/1568-1583/1769/2003`；`tbt_sock.c:423/426/691/761`；`tbt_tx.c:2702/3102/3104/3224`；`tbt_fec_data.c:128/528/536/634/655/681/700/1165/1319-1343` |
| 函数签名 | `static bool tbt_session_get(struct tbt_session *);` / `static void tbt_session_put(struct tbt_session *);` / `static bool tbt_session_arm(struct tbt_session *, enum tbt_sess_work, unsigned long);` / `static void tbt_session_cancel(struct tbt_session *, enum tbt_sess_work);` |
| 锁顺序 | `tbt_lock → sess->lock`（destroy 的 closing）；`tbt_session_arm` 快路径无锁；`wait_for_completion` 不得在 spinlock 内 |
| 失败路径 | 见 4.5 |
| 回归风险 | 见 4.5 |
| 验证 | 见 4.6 |

---

## 5. dead-pool 不变量

### 5.1 现状（文件:行）

`tbt_carrier_take_dead()`（`tbt_carrier.c:1281-1301`）：

```c
    spin_lock(&sess->lock);
    list_for_each_entry_safe(c, tmp, &sess->dead, node)
    {
        if (!c->released || atomic_read(&c->pollers) ||
            work_busy(&c->w_connect) || work_busy(&c->w_rx) ||
            work_busy(&c->w_tx) || work_busy(&c->w_pump.work))
            continue;
        list_del_init(&c->node);
        if (sess->n_dead)
            sess->n_dead--;
        spin_unlock(&sess->lock);
        return c;
    }
    spin_unlock(&sess->lock);
    return NULL;
```

`tbt_carrier_new_locked()`（`tbt_carrier.c:1303-1390`）的复用路径：

```c
    c = tbt_carrier_take_dead(sess);
    reused = c != NULL;
    if (c)
    {
        /* Reset every scalar but keep the buffers. */
        http_tx = c->http_tx;
        http_hdr = c->http_hdr;
        part = c->part;
        parser = c->parser;
        http_rx = c->http_rx;
        memset(c, 0, sizeof(*c));
        c->http_tx = http_tx;
        c->http_hdr = http_hdr;
        c->part = part;
        c->parser = parser;
        c->http_rx = http_rx;
        INIT_LIST_HEAD(&c->node);
        INIT_LIST_HEAD(&c->release_node);
        memset(parser, 0, sizeof(*parser));
        tbt_http_rx_new_message(http_rx);
    }
```

`tbt_carrier_free()`（`tbt_carrier.c:1568-1583`）：

```c
void tbt_carrier_free(struct tbt_carrier *c)
{
    if (!c)
        return;
    list_del_init(&c->node);
    kfree(c->http_tx);
    kfree(c->http_hdr);
    kfree(c->part);
    kfree(c->parser);
    kfree(c->http_rx);
    /* retire-drain */
    kfree(c);
}
```

**不变量今天靠三样东西**：`released`、`pollers`、`work_busy()`。它们不完整：

1. `pollers` 只保护 poll；`w_fbk` 的 carrier 遍历（`tbt_tx_fbk()` 的循环）不持任何保护，会写 `c->rx_state`。如果该 carrier 在此期间被 `take_dead()` 复用并 `memset`，写落在新生命上；更糟的是 `/proc` 的 `carrier <n> down` 会调用 `tbt_carrier_lost()`，把新生命标记为 lost。
2. `tbt_carrier_snapshot()` 与 `tbt_peers_show()` 在 `tbt_lock` 下读 `c->sk`/计数器，但 `w_replace` 不持 `tbt_lock`，可以在两次读之间复用对象。
3. 复用时的 `memset(c, 0, sizeof(*c))` 会重置 `http_lock` 和四个 `work_struct`；这只有在“没有 work 在跑、没有人持有 `http_lock`”时才安全。`take_dead()` 检查了四个 work，但**没有检查 `http_lock`**。`http_lock` 的持有者是 `tbt_carrier_http_begin()`，它只在 `pump_lock` 下被调用（xmit / fec_out_flush），而 `take_dead()` 也在 `pump_lock` 下——所以今天是安全的，但这是隐式论证，没有写下来，也没有断言。

### 5.2 设计

#### 5.2.1 `pollers` → `users`，并规定获取点

```c
    atomic_t users;   /* 非 pump_lock、非 carrier work、非 sess->lock 跨越的
                       * 读者。只在 sess->lock 下从 car[]/prov/dead 取得，
                       * 在任意上下文释放。tbt_carrier_take_dead() 在
                       * sess->lock 下检查它，因此“取得”与“复用”互斥。 */
```

helper：

```c
static inline void tbt_carrier_user_get(struct tbt_carrier *c)
{
    atomic_inc(&c->users);
}

static inline void tbt_carrier_user_put(struct tbt_carrier *c)
{
    atomic_dec(&c->users);
}
```

获取模式（**必须**在 `sess->lock` 内从容器取指针并自增）：

```c
    spin_lock(&sess->lock);
    c = sess->car[i];
    if (c)
        tbt_carrier_user_get(c);
    spin_unlock(&sess->lock);
    if (c) {
        ... use c ...
        tbt_carrier_user_put(c);
    }
```

为什么“在锁内取得”是必需的：`take_dead()` 也在 `sess->lock` 下检查 `users`。如果读者在锁外自增，`take_dead()` 可能在“读者拿到指针”和“读者自增”之间看到 `users == 0` 并复用；读者随后自增的是新生命的计数，读写的是新生命的字段。锁内取得把这两步变成原子的，`take_dead()` 要么看到 `users > 0`，要么读者拿到指针时对象已不在任何容器里（不可能，因为容器读取与 `take_dead()` 的摘链同一把锁）。

谁需要 `users`：

| 读者 | 今天 | 0.2.0 |
|---|---|---|
| `tbt_session_poll_work()` | `pollers` | `users`（已是锁内取得） |
| `tbt_tx_fbk()` / `tbt_fbk_work()` 的 carrier 遍历 | 无 | `users`（每个 carrier 锁内取得） |
| `tbt_carrier_snapshot()` | 无 | `users`（锁内取得） |
| `tbt_peers_show()` | 无 | `users`（每个 carrier 锁内取得） |
| `tbt_action_apply()` 的 `carrier down` | 无 | `users`（锁内取得） |
| `tbt_carrier_resume()` | 无 | `users`（锁内取得；它已持 `sess->lock`） |
| `tbt_carrier_lost()` 从 pump/rx work/poll 调用 | 各自已有保护 | 不变 |

只读的采样器（`tbt_path_rtt_us()`、`tbt_tx_rto_ns()`）不需要 `users`：它们只读、不写，最坏情况是读到一个新生命的 RTT 样本；`tbt_carrier_sk_get/put` 已经保证不 UAF。

`take_dead()` 的检查改为：

```c
        if (!c->released || atomic_read(&c->users) ||
            work_busy(&c->w_connect) || work_busy(&c->w_rx) ||
            work_busy(&c->w_tx) || work_busy(&c->w_pump.work))
            continue;
```

并加注释说明 `users` 的锁内取得规则。

#### 5.2.2 出生函数

把“保存五个指针 + memset + 恢复 + 重置两个有状态缓冲”收进一个函数：

```c
/* 每个 carrier 对象的唯一出生点。调用者持有 pump_lock。
 *
 * 出生不变量：
 *   - 所有标量归零（memset）。这覆盖 released/stopping/connected/up/
 *     users/joined/provisional/stats_retired/lost_counted/
 *     http_started/http_hdr_done/http_req_done/http_pending/part_len/
 *     part_off/gen/index/spare/active/profile/state/join_deadline_ns。
 *   - 五个堆缓冲跨生命保留；它们只是存储，不是状态：
 *       parser    : memset(0)，have 会被当作 memmove 长度；
 *       http_rx   : tbt_http_rx_new_message()，chunk 状态 + 记录装配器；
 *       part      : part_len = part_off = 0，旧字节不可达；
 *       http_tx   : 纯输出，每次写前由 encoder 填满；
 *       http_hdr  : 纯输出，每次写前由 builder 填满。
 *   - http_lock 重新初始化是安全的：唯一持有者是 http_begin()，它的
 *     所有调用者都持 pump_lock，而本函数要求 pump_lock。
 */
static void tbt_carrier_reset(struct tbt_session *sess, struct tbt_carrier *c)
{
    char *http_tx = c->http_tx;
    char *http_hdr = c->http_hdr;
    u8 *part = c->part;
    struct tbt_parser *parser = c->parser;
    struct tbt_http_rx *http_rx = c->http_rx;

    lockdep_assert_held(&sess->pump_lock);
    memset(c, 0, sizeof(*c));
    c->http_tx = http_tx;
    c->http_hdr = http_hdr;
    c->part = part;
    c->parser = parser;
    c->http_rx = http_rx;
    INIT_LIST_HEAD(&c->node);
    INIT_LIST_HEAD(&c->release_node);
    memset(parser, 0, sizeof(*parser));
    tbt_http_rx_new_message(http_rx);
    mutex_init(&c->http_lock);
    INIT_WORK(&c->w_connect, tbt_car_connect_work);
    INIT_WORK(&c->w_rx, tbt_car_rx_work);
    INIT_WORK(&c->w_tx, tbt_car_tx_work);
    INIT_DELAYED_WORK(&c->w_pump, tbt_pump_work);
    c->sess = sess;
}
```

`tbt_carrier_new_locked()` 变成：

```c
    c = tbt_carrier_take_dead(sess);
    if (c) {
        tbt_carrier_reset(sess, c);
    } else {
        c = kzalloc(sizeof(*c), GFP_KERNEL);
        if (!c)
            return NULL;
        INIT_LIST_HEAD(&c->node);
        INIT_LIST_HEAD(&c->release_node);
        c->http_tx = kmalloc(TBT_HTTP_FRAME_MAX, GFP_KERNEL);
        c->http_hdr = kmalloc(TBT_HTTP_HDR_MAX, GFP_KERNEL);
        c->part = kmalloc(TBT_CAR_PART_MAX, GFP_KERNEL);
        c->parser = kzalloc(sizeof(*c->parser), GFP_KERNEL);
        c->http_rx = kzalloc(sizeof(*c->http_rx), GFP_KERNEL);
        if (!c->http_tx || !c->http_hdr || !c->part || !c->parser ||
            !c->http_rx) {
            tbt_carrier_free(c);
            return NULL;
        }
        tbt_carrier_reset(sess, c);
    }
```

注意：`tbt_carrier_reset()` 在 `kzalloc` 路径上重复了一次 memset（无害），但让“两条路径都经过同一个出生函数”成为结构事实。`c->gen = ++sess->carrier_gen;`、`c->index`、`c->spare` 等仍由 `new_locked` 在 reset 之后设置。

#### 5.2.3 用 BUILD_BUG_ON 钉死“指针而非内联数组”

```c
static void tbt_carrier_birth_checks(void)
{
    /* memset(c, 0, sizeof(*c)) 只清指针，不清指针指向的字节。若有人把
     * 这五个字段改成内联数组，memset 会变成“清零整个 8 KiB 缓冲”，
     * 而下面的 reset 逻辑会静默失效。让编译器拒绝这种改动。 */
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->part), u8 *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->parser), struct tbt_parser *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_rx), struct tbt_http_rx *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_tx), char *));
    BUILD_BUG_ON(!__builtin_types_compatible_p(
        typeof(((struct tbt_carrier *)0)->http_hdr), char *));
    /* 缓冲必须覆盖最大的记录/帧。 */
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_HTTP_FEC_FRAME_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
}
```

在 `tbt_carrier_init()` 里调用。

#### 5.2.4 `tbt_carrier_free()` 的清理

```c
void tbt_carrier_free(struct tbt_carrier *c)
{
    if (!c)
        return;
    list_del_init(&c->node);
    list_del_init(&c->release_node);   /* 可能仍在 dead_release 上 */
    kfree(c->http_tx);
    kfree(c->http_hdr);
    kfree(c->part);
    kfree(c->parser);
    kfree(c->http_rx);
    tbt_session_put(c->sess);          /* §4：出生时取的那一个 */
    kfree(c);
}
```

`list_del_init(&c->release_node)` 是防御性的：`tbt_session_join()` 会把 old 同时挂到 `dead` 和 `dead_release`；如果 `dead_release` 的 drain 还没跑，`dead` 的 free 会留下一个悬空 `release_node`。session 即将释放，链表头也会消失，但保持节点自链是零成本的卫生。

### 5.3 迁移步骤

1. `tbtun.h`：`pollers` → `users`；加 `tbt_carrier_user_get/put` 与 `tbt_carrier_reset` 声明。
2. `tbt_carrier.c:1281-1301`：`take_dead()` 改 `users`；注释写明锁内取得规则。
3. `tbt_carrier.c:1303-1390`：抽出 `tbt_carrier_reset()`；两条路径都调用。
4. `tbt_carrier.c:2429-2462`：加 `tbt_carrier_birth_checks()`。
5. `tbt_carrier.c:1568-1583`：`free()` 加 `release_node` 清理与 `tbt_session_put`。
6. 把 5.2.1 表中列出的读者改成锁内 `user_get/put`：poll（已是）、`tbt_tx_fbk()`、`tbt_carrier_snapshot()`、`tbt_peers_show()`、`tbt_action_apply()`、`tbt_carrier_resume()`。
7. `tbt_carrier_close()` 里把 `part_len/part_off` 的残留计入 `recv_partial_discard` 并清零（§1 的 close 已包含）。

### 5.4 失败路径与回归风险

| 场景 | 行为 |
|---|---|
| `take_dead()` 找不到可用对象 | 走 `kzalloc` 路径；dead 池满时 join 返回 `-ENOSPC`（现有逻辑） |
| `users` 泄漏（读者忘 put） | carrier 永不复用，dead 池增长到 `2 * n_car` 后 join 拒绝；`/proc` 的 `dead_peak` 可见。迁移期加 `WARN_ON_ONCE(atomic_read(&c->users) > 1)` 辅助定位 |
| `http_lock` 被持有而 reset | 不可能：reset 与 `http_begin` 都要求 `pump_lock`；`lockdep_assert_held` 钉死 |
| 旧 `part_len` 残留 | `close()` 计入 `recv_partial_discard` 并清零；reset 的 memset 再兜一层 |
| `parser->have` 残留 | reset 的 `memset(parser, 0, ...)`；`far.split_record` 会压这条路径 |

回归风险：

- **`users` 的锁内取得**：这是最容易写错的一条。任何在锁外 `user_get` 的写法都会留下一个窄 UAF 窗口；评审时用 `grep -n "tbt_carrier_user_get"` 逐点确认它前面三行内有 `spin_lock(&sess->lock)`。
- **`memset(c, 0, sizeof(*c))` 的开销**：`struct tbt_carrier` 约 1 KiB（不含五个堆缓冲）；复用路径每次 memset 1 KiB，可忽略。
- **`BUILD_BUG_ON` 的 `typeof` 空指针解引用**：`typeof(((struct tbt_carrier *)0)->part)` 是未求值上下文，GCC/Clang 都接受；若某个编译器拒绝，改用 `__builtin_types_compatible_p(__typeof__(...))` 或 `offsetof` 方案。

### 5.5 验证

- **host**：`BUILD_BUG_ON` 在编译期生效；`make W=1` 与 `review/lint.sh` 必须通过。host 单测无法覆盖池复用。
- **QEMU（adversarial）**：替换风暴（probe O / HND0 场景）+ 同时循环读 `/proc/net/tbtun/peers` 与 `stats`；`far.split_record`、`far.chunk_huge`（让 parser 留下半条记录）后立刻触发替换；`init.port_exhaust`（验证 `local_port` 不会双 free）；KASAN 下不得出现 UAF。
- **真机**：HND0 的 259 次 workqueue hog 场景复跑；`dead_peak`/`prov_peak` 不超过 `2 * n_car`/`prov_cap`；`dmesg` 无 `WARN_ON_ONCE`。

### 5.6 交付清单

| 项 | 内容 |
|---|---|
| 改动文件:行 | `tbtun.h`（`struct tbt_carrier` 的 `pollers`）；`tbt_carrier.c:1281-1301/1303-1390/1568-1583/2429-2462`；`tbt_tx.c:2997-3070`；`tbt_carrier.c:2394-2427`；`tbt_session.c:390-420/530-550`；`tbt_carrier.c:2366-2392` |
| 函数签名 | `static inline void tbt_carrier_user_get(struct tbt_carrier *);` / `static inline void tbt_carrier_user_put(struct tbt_carrier *);` / `static void tbt_carrier_reset(struct tbt_session *, struct tbt_carrier *);` / `static void tbt_carrier_birth_checks(void);` |
| 锁顺序 | `pump_lock`（reset/复用）→ `sess->lock`（take_dead 检查）；`users` 只在 `sess->lock` 内取得 |
| 失败路径 | 见 5.4 |
| 回归风险 | 见 5.4 |
| 验证 | 见 5.5 |

---

## 6. 整体迁移顺序与验收矩阵

### 6.1 建议的提交序列

| 阶段 | 内容 | 依赖 | 可独立回滚 |
|---|---|---|---|
| P0 | §5 dead-pool：`users`、`reset`、`BUILD_BUG_ON` | 无 | 是 |
| P1 | §3 adopt/prov：发布顺序、`joined`、`n_prov` | P0（`reset` 覆盖 `joined`） | 是 |
| P2 | §2 stream work 引用 | 无（与 P0/P1 正交） | 是 |
| P3 | §4 session 引用计数与 destroy 重排 | P2（stream 引用是 session 引用的基础） | 是 |
| P4 | §1 `c->sk` RCU 原子提交 | P0（`users`）、P3（session 寿命） | 否（内部原子） |

P4 单独一个提交，评审时只盯三件事：字段 `__rcu`、所有读点 `sk_get/put`、release 的 `unpublish → sync → close` 顺序。P4 之前的任何中间树都不应上线。

### 6.2 验收矩阵

| 不变量 | 机械检查 | QEMU | 真机 |
|---|---|---|---|
| `c->sk` 只在 pump_lock 下写 | `lockdep_assert_held` + sparse `__rcu` | `near.fin_eagain`/`near.rst_eagain`/`init.rmmod` | 替换风暴 + rmmod |
| 每个 work 一个引用 | lint grep：裸 `schedule_work(&st->` 为空 | `pub.churn`/`pub.concurrent` | 长 churn |
| prov 与 car[] 互斥 | `WARN_ON_ONCE(c->joined)` | `far.dup_index`/`far.second_carrier` | 替换风暴 |
| session 等归零 | `/proc` 调试 `sess_refs`；`WARN_ON_ONCE` | `init.rmmod`/`create.replace` | tunnel add/del churn |
| 出生即全零 | `BUILD_BUG_ON` + `WARN_ON_ONCE(part_len)` | `far.split_record` 后替换 | 替换风暴 + `slabinfo` |

### 6.3 需要同步更新的文档

- `DESIGN.md` §8.2（对象模型）与 §14（不变量）：加入“`c->sk` 是 RCU 指针”“work 引用”“session 引用”“carrier 出生函数”。
- `CHANGELOG.md` 的 `[Unreleased]` → `Fixed`：按 P0-P4 各写一条，点名 `tbt_carrier_unpublish/close`、`tbt_stream_schedule/cancel`、`joined`、`tbt_session_arm/cancel`、`tbt_carrier_reset`。
- `review/lint.sh`：新增两条 grep（裸 stream work 排队、`c->sk` 裸解引用），并保留 `atomic-stats`。

---

### 附：本设计顺带关闭的、任务清单之外的缺陷

1. `tbt_car_poll_one()` 的 drip 写与 pump 共享 `c->part` 而无锁（§1.2.4）。
2. `tbt_tx_fbk()` 的 carrier 遍历会写已复用对象的 `rx_state`（§5.2.1）。
3. `tbt_fec_data_init()` 失败路径泄漏 `get_net()`（§4.1/§4.4）。
4. `tbt_stream_get()` 的发布路径不检查 `closing`（§4.1/§4.4）。
5. `cancel_work_sync(&st->w_kill)` 取消 pending 实例时泄漏引用（§2.1）。
6. `tbt_fec_tx_work` 在 stream 拆除之后才被 cancel（§4.1/§4.3）。
7. `tbt_carrier_free()` 不清 `release_node`（§5.2.4）。
