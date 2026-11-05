# P0-F2：传输中 `rmmod` 从已释放的模块 text 取指

状态：**只做静态分析与方案**。未修改任何源文件，未编译，未跑 QEMU（构建与 QEMU 由另一工程师占用）。
行号以当前工作树为准；内核侧引用以 `~/.cache/tbtun-build/src/linux-7.2.9` 为准。

---

## 0. 结论（先给判断）

1. **用户假设成立，但还差一环。** `tbt_sock_detach()` 把
   `sk_data_ready / sk_write_space / sk_state_change` 写回 `tbt_pub_ready /
   tbt_pub_write_space / tbt_pub_state_change`（`tbt_sock.c:392-394`），仍是模块 text。
   同时 `tbt_session_destroy()` 只 detach、**从不 release 公共 accepted socket**
   （`tbt_session.c:749-750`）。模块 text 释放后，下一个到达该 socket 的包在软中断里
   调用 `sk->sk_data_ready(sk)`（`tcp_data_queue()` → `tcp_data_ready()`，
   `tcp_input.c:5602-5605`），或 FIN 到达时调用 `sk->sk_state_change(sk)`
   （`tcp_fin()`，`tcp_input.c:4983`），跳到已释放地址。当前
   `console-rmmod.log` 是前者；用户先前读到的版本是后者。两个指针都在同一组
   被错误恢复的回调里。
2. **“在 attach 时保存原指针”对 accepted socket 不成立。** accepted socket 的
   回调是 `sock_copy()` 从 listener 继承的（`sock.c:2210-2229`，回调字段不在
   `sk_dontcopy` 区间内，`sock.h:214-232`）。listener 在
   `tbt_listen_start()` 里把 `sk_data_ready` 改成了 `tbt_ready`
   （`tbt_listen.c:130-131`），所以 accepted socket 继承到的
   **`sk_data_ready` 是模块 text**；`sk_write_space`/`sk_state_change` 目前恰好
   仍是 core 默认值（listener 没改它们），但这是偶然，不能当成规则。
   在 `tbt_sock_attach()` 时保存，至少 `data_ready` 会保存成 `tbt_ready`。
   正确的快照点是 `tbt_listen_start()` 中 `sock_create_kern()` 成功之后、覆盖之前；
   那一份四个指针全是 `sock_init_data()` 的默认值。
3. 同一缺陷还有四个变体：carrier 释放后留在 `FIN_WAIT` 且 `sk_prot` 指向模块
   `.ro_after_init` 数据；被拒绝的 accepted child 继承 `tbt_ready` + `sk_user_data=t`；
   target socket 无 linger 释放；teardown 里 `w_kill`/`w_tx` 可被重排队。
   只修“公共 socket 漏 release”不够。

`adversarial/findings.md` 的 F2 已经指出“detach 但未 release”。本报告确认该结论，
并补上“恢复源不能是 attach 时的指针”、carrier `sk_prot`、listen 错误路径和两个
work 重排队窗口。

---

## 1. 崩溃证据链（`adversarial/console-rmmod.log`）

当前工作树的 `adversarial/console-rmmod.log` 在 07:3x 被重新生成过；下表以**当前文件**为准
（早先一次运行是 `tcp_fin -> sk_state_change`，机制相同，见本节末尾的对照）。

| 行 | 内容 | 含义 |
|---|---|---|
| 403-404 | `PHASE_START rmmod` / `== rmmod mid-transfer` | `adversarial/init:334` 的 phase 开始 |
| 405 | `Oops: int3: 0000 [#1] SMP NOPTI` | 跳到被填充成 `0xcc`（int3）的已释放 text |
| 409-410 | `RIP: 0010:0xffffffffc0607181`；`Code: cc cc ... <cc> ...` | RIP 在模块 text 范围，内容已是 int3 填充 |
| 423 | `? tcp_data_queue+0x9df/0xf50` | TCP 输入软中断把数据排入 receive queue |
| 469 | `? tcp_sendmsg_locked+0x896/0x12b0`（TASK 侧） | 应用正在 `sendto()`，同时软中断收包 |
| 485 | `Modules linked in:`（空） | oops 时没有任何模块在册 |
| 486 | `Unloaded tainted modules: tbtun(OE):1 [last unloaded: tbtun(OE)]` | 模块已经卸载，text 已释放 |
| 488-489 | `RIP: 0010:0xffffffffc0607181`，同样的 `cc` 填充 | 同一地址的第二次打印 |
| 500 | `Kernel panic - not syncing: Fatal exception in interrupt` | 软中断里的致命异常直接 panic，guest 挂死 |

**早先一次运行（用户读到的版本）的对照：** 那次是
`BUG: unable to handle page fault ... supervisor instruction fetch`，
`RIP: 0xffffffffc0407060`，栈为
`<IRQ> tcp_fin+0x96 -> tcp_data_queue -> ...`，
`<TASK> tcp_send_fin -> __tcp_close -> inet_release -> sock_close -> __x64_sys_close`，
`Unloaded tainted modules: tbtun`。两次的差别只是“先到的是数据还是 FIN”：
数据走 `tcp_data_queue()` 的 `sk_data_ready`，FIN 走 `tcp_fin()` 的
`sk_state_change`；两个函数指针都在 `tbt_sock_detach()` 里被写回模块 text
（`tbt_sock.c:392-394`）。

**链路还原：**

1. `phase_rmmod`（`adversarial/init:334-360`）先 `setup_raw` 建 near+far 两条隧道，
   再跑 `adv_probe echo 8 20`（8 MiB，每 64 KiB 睡 20 ms），1 秒后 `rmmod tbtun`。
2. 应用连接落在 near tunnel 的公共监听端口；模块 `kernel_accept()` 得到 accepted
   socket，`tbt_stream_start()` 把 `st->sk` 指向它并在 `tbt_sock_attach()` 装入
   三个模块回调（`tbt_sock.c:373-376`）。
3. `rmmod` → `tbtun_exit()` → `tbt_tunnel_shutdown()` → `tbt_session_destroy()`。
   对每个 stream：`cancel_work_sync(&victim->w_kill)`、`tbt_sock_detach(victim)`、
   `tbt_stream_put(victim)`（`tbt_session.c:748-750`）。
   **没有 `sock_release()`。** near 侧又没有 `s->target`，所以 `tbt_session.c:754-763`
   的 target 释放块也不覆盖它。
4. `tbt_sock_detach()` 把回调写回模块自己的三个函数（`tbt_sock.c:392-394`），
   把 `sk_user_data` 清 NULL。socket 仍是 `TCP_ESTABLISHED`，仍在 ehash 里，
   仍能收包；只是没人读它。
5. `module_exit` 返回，模块 text 被 `free_module()` 释放。
6. 应用继续 `sendto()`（当前日志的 TASK 栈就是 `tcp_sendmsg_locked`）。数据到达
   那个 accepted socket，`tcp_data_queue()` 调用 `sk->sk_data_ready(sk)` →
   `tbt_pub_ready` → 已释放 text → oops。若先到的是应用 close 的 FIN，则走
   `tcp_fin()` 的 `sk->sk_state_change(sk)` → `tbt_pub_state_change`，结果相同
   （早先一次运行就是这样）。
7. oops 发生在软中断，`Fatal exception in interrupt` 直接 panic，于是
   `console.hang.rmmod` 与 `console.klog.rmmod` 同时 FAIL（`adversarial/results.txt:29-30`）。

**一个措辞纠正：** 应用并不“持有” accepted socket（它没有 fd）；应用持有的是
loopback 连接的另一半（客户端 socket）。accepted socket 在内核里由模块创建，
因为没有 fd，只有模块能释放它。模块不释放，它就一直在 ehash 里活着。
这正是“必须由模块自己 release”的原因。

---

## 2. 现状：`module_exit` 每一步做什么、漏什么

`tbt_core.c:146-158`：

```c
static void __exit tbtun_exit(void)
{
    tbt_procfs_exit();                 // 148
    mutex_lock(&tbt_lock);             // 149
    tbt_tunnel_shutdown();             // 150
    mutex_unlock(&tbt_lock);           // 152
    flush_workqueue(system_wq);        // 155
    synchronize_rcu();                 // 156
    rcu_barrier();                     // 157
    pr_info("tbtun: unloaded\n");
}
```

| 步骤 | 代码 | 真正停住的东西 | 没停住的东西 |
|---|---|---|---|
| 1 | `tbt_procfs_exit()` `tbt_procfs.c:684-695` | 新 `/proc` 调用；`remove_proc_entry()` 内部 `proc_entry_rundown()`（`fs/proc/inode.c:255-274`）等待所有在途 fop，包括 `release` 里的最后一行 | socket 回调、work、RCU |
| 2 | `mutex_lock(&tbt_lock)` | 新的 config 动作 | 同上 |
| 3a | `tbt_tunnel_shutdown()` 标记 `closing` `tbt_session.c:253-266` | 新的 session work 会早退 | 已在跑的 work |
| 3b | `tbt_listen_stop()` `tbt_listen.c:140-158` | 新 accept；`cancel_work_sync(&t->w_accept)`；listener `sock_release()`（TCP_LISTEN close 同步销毁，含 accept 队列里的 child） | 已经 accept 的 child 归 `tbt_session_destroy()`；`tbt_accept_one()` 拒绝路径的 child 仍是裸 `sock_release()`（W4） |
| 3c | `tbt_session_destroy()` `tbt_session.c:700-776` | session work、carrier work、stream work；target socket 的 release | **公共 accepted socket 的 release**；回调恢复成模块函数；carrier/target 无 linger |
| 4 | `mutex_unlock` | — | — |
| 5 | `flush_workqueue(system_wq)` | 只 flush 此刻已排队的 `system_wq` work | **不 flush delayed work**；**不阻止 flush 之后新排队**；**不覆盖 `system_unbound_wq`**（`w_kill`、`w_fbk` 在 unbound） |
| 6 | `synchronize_rcu()` | 已在途的 RCU 读临界区 | 不阻止新的回调/工作 |
| 7 | `rcu_barrier()` | 调用它之前已排队的 `call_rcu`/`kfree_rcu` | 之后才排队的回调 |

**“哪一步之后仍可能有代码在模块 text 里执行？”**
第 3 步返回之后（以及第 7 步之后）都可能有。具体是：

- 第 3c 步 detach 后的公共 accepted socket：**永久**（直到 socket 被别的路径释放，
  而当前没有任何路径会释放它）；
- 第 3c 步 carrier release 后留在 `FIN_WAIT1/2` 的 socket：最长约 60 秒
  （`TCP_FIN_TIMEOUT`），或直到对端关闭；
- 第 3c 步 target socket 无 linger release 后同样的 `FIN_WAIT`；
- 第 3c 步里被 `w_ctrl` 在 detach 期间重新排队的 `w_kill`（`system_unbound_wq`，
  第 5 步的 `flush_workqueue(system_wq)` 不管它）；
- 第 3c 步里被正在跑的 `w_poll` 重新排队的 `w_tx`（carrier 随后被 `kfree()`）。

所以 `tbt_core.c:153-154` 的注释“Nothing may still be running when the text
disappears”目前是**不成立的**。第 5-7 步只是“看起来像屏障”，并没有建立这个不变量。

---

## 3. 竞态窗口清单（每条可指到代码）

> 记号：`[已发生]` = 日志已证；`[同类]` = 同一机制的另一处；`[推断]` = 静态推演，
> 未在日志中出现。所有“窗口”都指“模块 text 已释放后仍可能被进入”。

### W1 `[已发生]` 公共 accepted socket 只 detach、不 release

- `tbt_session.c:748-750`：`cancel_work_sync(w_kill)` → `tbt_sock_detach()` → `tbt_stream_put()`。
- `tbt_sock.c:382-399`：detach 只改回调、取消三个 work，**没有 `sock_release`**。
- 对比 `tbt_sock.c:322-347` 的 `tbt_kill_work()`：只有它 release。
- 触发：应用继续 `sendto()` → 数据到达 → `tcp_data_queue` → `sk_data_ready`
  （`tcp_input.c:5602-5605`）；若先 close，则 FIN → `tcp_fin` → `sk_state_change`
  （`tcp_input.c:4983`）。两者都在同一组被错误恢复的指针里。
- 结果：当前日志 `console-rmmod.log:405-423`（`tcp_data_queue`）与 `:500`（panic）；
  早先日志 `:431`（`tcp_fin`）与 `:520`（unload 完成）。

### W2 `[同类]` detach 恢复的是模块 text，而不是内核默认回调

- `tbt_sock.c:392-394`。
- 即使将来某个路径补上了 `sock_release`，只要 release 之前还有包到达
  （detach 与 release 之间的窗口），也会进模块 text；模块已卸载时就是 W1。
- 这是“为什么必须在 detach 时就写回 core 指针”的原因。

### W3 `[同类]` carrier 释放后留在 `FIN_WAIT`，回调 + `sk_prot` 都指向模块

- 回调写成模块函数：`tbt_carrier.c:818-821`（注意 `sk_data_ready` 写的是
  `tbt_car_ready`，不是 `_off`，明显是笔误；两者都是模块 text，结果一样）。
- `sk_prot` 在 `tbt_carrier.c:59-62` 安装为 `&tbt_tcp_prot`，调用点
  `tbt_carrier.c:1126`（拨号）与 `1291`（接受）。`tbt_carrier_release()` 从不恢复。
- 释放只做 `kernel_sock_shutdown()` + `sock_release()`（`tbt_carrier.c:823-824`），
  **没有 `SO_LINGER=0`**。`kernel_sock_shutdown(SHUT_WR)` 在 ESTABLISHED 上
  走 `tcp_shutdown()` → `tcp_close_state()` → `FIN_WAIT1`
  （`tcp.c:3079-3084`；`tcp_close_state()` 定义在 `tcp.c:3054`）；随后
  `sock_release()` 的 `__tcp_close()` 对 `FIN_WAIT1` 走
  `sk_stream_wait_close()` + `adjudge_to_death`，socket 仍 hash，最长 60 秒。
- 释放后仍有包：`tcp_data_ready()` 在 `SOCK_DEAD` 时也会调用
  `sk->sk_data_ready(sk)`（`tcp_input.c:5602-5605`）→ `tbt_car_ready` → 已释放 text。
- 最终销毁时 `inet_csk_destroy_sock()` 调 `sk->sk_prot->destroy(sk)`
  （`inet_connection_sock.c:1282`），读的是模块 `.ro_after_init` 里的 `struct proto`
  → 数据 UAF。
- 触发条件：对端在 carrier close 之后还发数据（或 FIN 之后的重传/窗口探测）。
  在 loopback 上 FIN 交换很快，`FIN_WAIT2` 仍会停留；窗口真实存在。

### W4 `[同类]` 被拒绝的 accepted child 继承 `tbt_ready` + `sk_user_data=t`

- `tbt_listen.c:78`：`tbt_stream_start()` 返回 `-EBUSY`（第二条公共连接）或
  `tbt_carrier_adopt()` 返回 `-ENOMEM/-ENOTCONN` 时，直接 `sock_release(newsock)`。
- 该 child 的 `sk_data_ready` 是从 listener 继承的 `tbt_ready`，
  `sk_write_space`/`sk_state_change` 是 core 默认值；`sk_user_data` 是 `t`
  （`sock_copy()` 不复制 `sk_dontcopy` 区间，回调不在该区间：
  `sock.h:214-232`、`sock.c:2210-2229`；`inet_csk_clone_lock()` 也不重置回调，
  `inet_connection_sock.c:1204-1265`）。
- 无 linger release → `FIN_WAIT1`，仍 hash。之后到达的数据会调 `tbt_ready(child)`，
  它把 `sk_user_data` 当 `struct tbt_tunnel *`，检查 `accepting` 并
  `schedule_work(&t->w_accept)`（`tbt_listen.c:36-44`）。
- 如果隧道随后被删除/卸载，`t` 已被 `kfree_rcu`，这是**不需要 rmmod 就能触发的
  UAF**（只是窗口窄）。`init.rmmod` 里第二条连接没有出现，所以日志没显示它。

### W5 `[同类]` target socket 无 linger 释放

- `tbt_session.c:754-763`：`tbt_target_detach()`（内部 detach 会把回调写成模块函数）
  → `kernel_sock_shutdown()` → `sock_release()`，无 linger。
- `tbt_carrier.c:1357`：target connect 成功但 stream 已 `-ECANCELED` 时同样无 linger。
- 如果 stream 曾 attach，回调是模块函数；`FIN_WAIT` 期间的数据会进模块 text。
  如果从未 attach，回调是 `sock_create_kern()` 的默认值，安全，但 socket 仍会
  在 `FIN_WAIT` 停留。
- far 侧在 `init.rmmod` 中同样存在，只是崩溃先发生在 near 的公共 socket。

### W6 `[推断，窗口窄]` `w_kill` 在 detach 期间被 `w_ctrl` 重新排队

- `tbt_session.c:748` 只 cancel 一次 `w_kill`，然后 `tbt_sock_detach()`。
- detach 在 `tbt_sock.c:397-399` 才 `cancel_work_sync(&st->w_ctrl)`。
- 如果 `w_ctrl` 此刻正在运行，它可能在 `tbt_sock.c:287-289` 调
  `tbt_stream_kill()`，后者在 `tbt_session.c:635` `queue_work(system_unbound_wq, &st->w_kill)`。
- 这个 `w_kill` 在第一次 cancel 之后、detach 的 cancel 之内排队；detach 只等
  `w_ctrl` 结束，不会取消它。随后 `tbt_stream_put()` 释放 stream，`w_kill`
  在已释放对象上运行；若在 `module_exit` 返回后运行，就是模块 text。
- `flush_workqueue(system_wq)`（`tbt_core.c:155`）**不覆盖 unbound wq**，所以
  第 5 步也救不了。

### W7 `[推断，窗口窄]` `w_poll` 在 `carriers_stop()` 取消之后重新排队 `w_tx`

- `tbt_carriers_stop()` 在 `tbt_carrier.c:1011-1022` 先 `atomic_set(stopping,1)` +
  `cancel_work_sync(&c->w_tx)` + `cancel_delayed_work_sync(&c->w_pump)`。
- 一个已经越过入口 `stopping` 检查的 `w_poll`（`tbt_carrier.c:212-218`）可以继续走到
  rescue 段，在 `tbt_carrier.c:323` `schedule_work(&c->w_tx)`。
- 之后 `tbt_carrier_release()` 在 `tbt_carrier.c:807` 只 cancel `w_poll`，**不再
  cancel `w_tx`**；`tbt_carriers_stop()` 在 `tbt_carrier.c:1049` `kfree(c)`。
- 结果：`w_tx` 在已释放的 `struct tbt_carrier` 上运行（`c->sess->tx_work_runs++`，
  `tbt_carrier.c:179-187`）。即使模块还 loaded 也是 UAF；若排队跨过卸载，就是 text UAF。

### W8 `[同类]` `flush_workqueue(system_wq)` 不是屏障

- `tbt_core.c:155`。它只 flush 当前已排队的 work：
  - 不包含 `schedule_delayed_work()` 尚未到期的 delayed work（本模块的
    `w_poll`/`w_pump`/`w_fbk` 都是 delayed work，虽然各自有 `cancel_*_sync`，
    但“靠 flush 兜底”是错的）；
  - 不阻止 flush 返回后由软中断回调重新 `schedule_work()`；
  - 不覆盖 `system_unbound_wq`（`w_kill`、`w_fbk` 在这里）。
- 结论：不能把它当作“没有 work 了”的证明。真正的证明是“每个调度源都已被停掉，
  且每个 work 都被 `cancel_*_sync` 过”。

### W9 `[同类]` `rcu_barrier()` 只对“调用前已排队”的回调有效

- `tbt_core.c:156-157`。`tbt_stream_free_rcu`（`tbt_session.c:527-536`）和
  `kfree_rcu(t, rcu)`（`tbt_session.c:150`）都在模块 text。
- 如果 W6/W7 在 `rcu_barrier()` 之后才 `call_rcu`/`kfree_rcu`，barrier 不等待。
- 顺序必须是：先停掉所有能 `call_rcu` 的上下文，再 `synchronize_rcu()`，
  最后 `rcu_barrier()`。当前顺序对“已排队”的回调是对的，但前提不成立。

### W10 `[设计风险]` 持 `tbt_lock` 调用会睡眠的原语

- `tbt_core.c:149-152` 持有 `tbt_lock` 调 `tbt_tunnel_shutdown()`；
  后者进入 `tbt_listen_stop()` 的 `cancel_work_sync(&t->w_accept)`（`tbt_listen.c:154`）
  和 `tbt_session_destroy()` 的 `cancel_*_sync`、`sock_release`、
  `mutex_lock(&sess->pump_lock)`（`tbt_session.c:715-771`）。
- `mutex` 下睡眠本身合法，但它建立了锁序：**任何被 cancel 的 work 只要取
  `tbt_lock` 就会死锁**。当前 `tbt_lock` 的使用者只有
  `tbt_core.c`、`tbt_procfs.c`、`tbt_session.c` 的 show/action 路径
  （`grep -n tbt_lock tbtun/*.c`），没有 work 回调取它，所以今天不死锁；
  这是一步之遥的隐患。
- `tbt_carriers_stop()` 在 `tbt_carrier.c:1027-1045` 持 `pump_lock` 调
  `tbt_carrier_retire()` → `tbt_carrier_release()`（`tbt_carrier.c:800-826`），
  后者 `cancel_work_sync(&c->w_rx/w_connect)` 并 `sock_release()`。
  今天 `w_rx`/`w_connect` 不取 `pump_lock`（`w_rx` 明确不 pump，
  `tbt_carrier.c:443-445` 的注释），所以不死锁；一旦有人在 `w_rx` 里加
  `tbt_tx_pump()`，就是 ABBA 死锁。
- `sess->lock` 是 spinlock，destroy 已经做到“锁内只摘链、锁外 detach”，这点是对的。

### W11 `[已排除]` procfs 在途 syscall

- `tbt_procfs_exit()` 的 `remove_proc_entry()` 调 `proc_entry_rundown()`
  （`fs/proc/inode.c:255-274`），等待 `in_use` 归零并关闭所有 `pde_openers`，
  包括 `tbt_cfg_release()`（`tbt_procfs.c:195-206`）。
- 所以第 1 步之后没有在途的 `config` 写者/读者。procfs **不是** F2 的来源。
  但注意：`release` 里可能再 `tbt_tunnel_add()`，它发生在 shutdown 之前，
  会被随后的 shutdown 一并清掉，这是正确的。

### W12 `[已排除]` netdev notifier / raw timer / module param 回调

- `grep register_netdevice_notifier|register_pernet|timer_setup|mod_timer|hrtimer tbtun/*`
  无结果。模块只有 workqueue 和 delayed work，没有 raw timer、没有 notifier。
- module param 是 `0444` 只读，无回调。

---

## 4. 五个问题的直接回答

### Q1 `module_exit` 的顺序，以及“哪一步之后仍可能执行模块 text”

见第 2 节表格。简答：

```
procfs_exit        // 等在途 fop；停新 syscall
tbt_lock
  tunnel_shutdown
    listen_stop    // 停 accept + release listener
    session_destroy
      cancel w_fbk/w_replace/w_target
      carriers_stop // cancel w_tx/w_pump，retire/release/free，synchronize_rcu
      tx_free_queue
      streams: cancel w_kill ×1, detach, put   // ← 公共 socket 不 release
      target: detach, shutdown, release        // ← 无 linger
      cancel w_fbk/w_replace ×2
    tunnel_put
tbt_lock unlock
flush_workqueue(system_wq)
synchronize_rcu
rcu_barrier
```

**第 3 步返回之后**仍有五类执行上下文可能进模块 text（公共 socket 永久、
carrier/target 的 FIN_WAIT、w_kill、w_tx）。第 5-7 步都不能关闭它们。
正确的边界是：**在 `module_exit` 返回前，模块安装过的每一个函数指针都必须已被
core 函数覆盖，或者它所在的 socket 已被同步销毁（unhash + TCP_CLOSE），
并且每一个 work 都已被 `cancel_*_sync`。**

### Q2 具体有哪些上下文

| 上下文 | 代码位置 | 卸载后能否进入 | 触发 |
|---|---|---|---|
| 公共 accepted socket 的 `sk_data_ready/write_space/state_change` | 安装 `tbt_sock.c:373-376`；detach 写回模块函数 `392-394` | **能，永久** | 应用数据/FIN/窗口更新 |
| target socket 的同一组回调 | 经 `tbt_target_attach()` → `tbt_sock_attach()`（`tbt_sock.c:470-495`） | 能，`FIN_WAIT` 期间 | target 侧数据/FIN |
| carrier socket 的同一组回调 | 安装 `tbt_carrier.c:1131-1134`、`1296-1299`；release 写回 `818-821` | 能，`FIN_WAIT` 最长 60 s | 对端数据/ACK/FIN |
| listener 的 `sk_data_ready` | `tbt_listen.c:130-131`；stop 写回 `150-151` | **不能**（listener close 同步销毁、unhash） | 只在 release 过程中，模块仍 loaded |
| `sk_error_report` | 模块从未安装 | 不适用 | — |
| `w_accept` | `tbt_listen.c:44` | 不能（`cancel_work_sync`，且 `sk_user_data` 先清） | — |
| `w_read/w_drain/w_ctrl` | `tbt_sock.c:116/127/135` | 不能（detach 内 cancel） | 但 `w_ctrl` 可重排 `w_kill`，见 W6 |
| `w_kill` | `tbt_session.c:635`（`system_unbound_wq`） | **能**（W6） | `w_ctrl` 在 detach 期间 kill |
| `w_connect/w_rx` | `tbt_carrier.c:76/236` 等 | 不能（release 内 cancel） | — |
| `w_tx/w_pump` | `tbt_carrier.c:123/226` 等 | **能**（W7，carrier 已 free） | `w_poll` 在 stop 后重排 |
| `w_poll` | `tbt_carrier.c:147` 等 | 不能（release 内 `cancel_delayed_work_sync`） | — |
| `w_target/w_replace/w_fbk` | `tbt_session.c:675-677` | 不能（destroy 内 cancel；`closing` 阻止自排队） | — |
| `tbt_stream_free_rcu` | `tbt_session.c:527-541` | 能（若 barrier 之后才排队） | `tbt_stream_put` |
| tunnel `kfree_rcu` | `tbt_session.c:150` | 能（同上） | `tbt_tunnel_put` |
| procfs write/read/release | `tbt_procfs.c:135-206` | 不能（`proc_entry_rundown`） | — |
| netdev/notifier/timer | 无 | — | — |

### Q3 每个上下文的“停”原语、顺序、锁约束

| 上下文 | 原语 | 关键约束 |
|---|---|---|
| workqueue（普通） | `cancel_work_sync()` | 对象必须还活着；**不能**持有该 work 会取的锁；先断调度源（清回调 / `stopping` / `closing`）再 cancel |
| delayed work | `cancel_delayed_work_sync()` | 自排队 work 先置 `closing/stopping`；必要时 `disable_delayed_work_sync()`（7.2 可用）做更强保证 |
| softirq 回调 | 没有“cancel”；在 `write_lock_bh(&sk->sk_callback_lock)` 下把三个指针换成 core 函数、`sk_user_data=NULL`；然后 **同步销毁 socket** | 必须先换指针再 cancel work，否则 cancel 之后还能被软中断重新排队；换指针后不能再有模块 work 依赖该 socket |
| socket 生命周期 | `SO_LINGER={1,0}` + `sock_release()` | linger 0 让 `__tcp_close()` 走 `tcp_disconnect()` → `TCP_CLOSE` → `inet_csk_destroy_sock()`（`tcp.c:3186-3190`；销毁在 `tcp.c:3224-3298`），同步 unhash；只 `kernel_sock_shutdown()` 会留 `FIN_WAIT` |
| carrier `sk_prot` | `sock_replace_proto(sk, &tcp_prot)`（inline，`sock.h:1917-1922`） | `tcp_prot` 已导出（`tcp_ipv4.c:3399`）；必须在 `sock_release` 前恢复，否则最终 `sk->sk_prot->destroy()` 读模块 `.data` |
| RCU 读者 | `synchronize_rcu()` | 在停掉所有会 `call_rcu` 的上下文之后 |
| RCU 回调 | `rcu_barrier()` | 最后一步；等待 `tbt_stream_free_rcu` 与 `kfree_rcu` |
| procfs | `remove_proc_entry()` | 内部已等待在途 fop，无需额外原语 |
| netdev/notifier | 无 | — |

**推荐顺序（也是补丁的顺序）：**

```
1. tbt_procfs_exit()                      // 停新 syscall，等在途 fop
2. tbt_lock
3.   listen_stop                          // 停 accept，恢复 listener 回调，release
4.   session_destroy
4.1    cancel w_fbk / w_replace / w_target
4.2    carriers_stop
         cancel w_tx / w_pump（锁外）
         retire/release：cancel w_poll/w_connect/w_rx，
                         恢复 core 回调 + tcp_prot，
                         linger 0 + sock_release
         再次 cancel w_tx / w_pump（锁外，防 W7）
         free carrier
4.3    tx_free_queue
4.4    streams：cancel w_kill
                detach（恢复 core 回调；cancel w_read/w_drain/w_ctrl）
                cancel w_kill 第二次（防 W6）
                linger 0 + sock_release
                put
4.5    target（若仍存在）：detach、linger 0、shutdown、release
4.6    再次 cancel w_fbk / w_replace
5. tbt_lock unlock
6. synchronize_rcu()
7. rcu_barrier()
```

### Q4 现有 teardown 是否在持锁时调用会睡眠的原语？死锁风险

**是。** 具体：

1. `tbt_lock`（mutex）跨 `tbt_listen_stop()` 的 `cancel_work_sync`、
   `tbt_session_destroy()` 的 `cancel_*_sync`、`sock_release`、
   `mutex_lock(&sess->pump_lock)`（`tbt_core.c:149-152` →
   `tbt_listen.c:154`、`tbt_session.c:715-771`）。
   风险：任何被 cancel 的 work 若取 `tbt_lock` 就 ABBA 死锁。今天没有
   （`tbt_lock` 只在 core/procfs/session 的 show/action 里），但这是隐式契约，
   没有编译期或 lockdep 断言保护。副作用：整个 teardown 期间所有 `/proc` 读写
   被阻塞，放大了“flush 卡住”的表象。
2. `pump_lock`（mutex）跨 `tbt_carrier_retire()` → `tbt_carrier_release()` 的
   `cancel_work_sync(&w_rx/w_connect)` 和 `sock_release`
   （`tbt_carrier.c:1027-1045` → `800-826`）。
   风险：`w_rx`/`w_connect` 一旦取 `pump_lock` 就死锁。当前 `w_rx` 明确不 pump
   （`tbt_carrier.c:443-445`），所以今天安全，但注释已经承认这是“不能做”的约束。
3. `sess->lock`（spinlock）**没有**跨睡眠调用：destroy 在锁内只
   `hlist_del_rcu`，锁外 detach（`tbt_session.c:733-751`）。这是正确的。
4. `sk_callback_lock` 下只做指针写，不睡眠。正确。
5. `tbt_target_detach()` 的注释（`tbt_sock.c:501-502`）记录了一次历史死锁：
   在 `sess->lock` 外又取一次导致 `flush` 自旋。说明这块曾经踩过锁序。

**结论：** 当前没有可复现的 ABBA 死锁，但 `tbt_lock`/`pump_lock` 跨
`cancel_*_sync` + `sock_release` 是系统性风险。F2 的最小补丁可以保持现有锁序，
但应在注释里把它写成不变量；后续应把 `tbt_session_destroy()` 移到 `tbt_lock` 之外
（先在锁内摘链，再锁外销毁）。

### Q5 最小、可验证的补丁方案

见第 5 节。核心是四件事：

1. 在 `tbt_listen_start()` 里、覆盖之前，快照 listener 的 core 回调；
2. 所有 detach/release 写回快照，而不是写回模块函数；
3. 所有模块拥有的 socket 在 teardown 时 `SO_LINGER=0` + `sock_release()`；
4. 补上 `w_kill` 的第二次 cancel 和 carrier free 前的 `w_tx/w_pump` cancel。

---

## 5. 补丁方案（函数级伪代码，不含完整 diff）

### 5.0 实施顺序（每步都能单独编译/审查）

1. `tbtun.h`：加 `struct tbt_sk_ops` 与 `tbt_tunnel.def_ops` /
   `tbt_session.def_ops`。纯数据结构，行为不变。
2. `tbt_listen.c`：在 `sock_create_kern()` 后快照；`tbt_listen_stop()` 用快照恢复；
   拒绝路径恢复 + linger + release。
3. `tbt_sock.c`：实现 `tbt_sk_ops_save/restore`、`tbt_sock_abort_on_close`；
   `tbt_sock_detach()` 改用快照；新增 `tbt_stream_release_socket()`。
4. `tbt_session.c`：`tbt_session_create()` 复制快照；destroy 的 stream 循环调用
   `tbt_stream_release_socket()`；target 块加 linger。
   **第 2+3+4 步合起来是 F2 的闭合点**：公共 accepted socket 会被恢复 core 回调
   并同步销毁。建议这三步放在一个 commit，便于 bisect。
5. `tbt_carrier.c`：release 恢复 core 回调 + `tcp_prot` + linger + 二次 cancel
   `w_rx`；`tbt_carriers_stop()` 在 free 前二次 cancel `w_tx/w_pump`；
   删除两个 `_off` 函数。这一步关闭 W3/W7。
6. `tbt_core.c`：删除 `flush_workqueue(system_wq)`，改写注释。这一步不改变行为，
   只是把“安全边界”写对，防止下一个人再把 flush 当屏障。

第 2+3+4 步之后，`init.rmmod` 的 oops 应当消失；第 5 步之后，carrier 的
`FIN_WAIT` 窗口和 `w_tx` UAF 也关闭；第 6 步只影响可读性。

### 5.1 `tbtun.h`：新增回调快照类型与字段

放在 `struct tbt_tunnel` 之前：

```c
/* The four callbacks sock_init_data() installs (net/core/sock.c:3775-3778).
 *
 * Snapshot source: a socket straight out of sock_create_kern(), before this
 * module replaces anything.  An accepted socket is NOT a valid source: it
 * inherits the listener's already-replaced sk_data_ready through sock_copy()
 * (callbacks are outside sk_dontcopy_begin/end), and
 * inet_csk_clone_lock() does not reset it.  Saving in tbt_sock_attach()
 * would therefore save tbt_ready(), which is module text. */
struct tbt_sk_ops {
    void (*data_ready)(struct sock *sk);
    void (*write_space)(struct sock *sk);
    void (*state_change)(struct sock *sk);
    /* sk_error_report is never replaced by this module; listed so a future
     * attach that does replace it cannot silently skip the restore. */
    void (*error_report)(struct sock *sk);
};
```

`struct tbt_tunnel` 增加：

```c
    struct tbt_sk_ops def_ops;  /* core callbacks, captured before tbt_ready */
```

`struct tbt_session` 增加：

```c
    struct tbt_sk_ops def_ops;  /* copy of tunnel->def_ops; restore source */
```

**不需要**在 `struct tbt_stream` / `struct tbt_carrier` 里各存一份：
它们都通过 `st->sess` / `c->sess` 拿到同一份快照；每 socket 一份只会增加
“存的是继承来的 tbt_ready”这种错误的机会。

### 5.2 `tbt_sock.c`：保存/恢复/硬关闭三个 helper

```c
void tbt_sk_ops_save(struct tbt_sk_ops *ops, struct sock *sk)
{
    ops->data_ready   = READ_ONCE(sk->sk_data_ready);
    ops->write_space  = READ_ONCE(sk->sk_write_space);
    ops->state_change = READ_ONCE(sk->sk_state_change);
    ops->error_report = READ_ONCE(sk->sk_error_report);
}

/* Never install a NULL callback: a NULL sk_data_ready is a worse crash than
 * a stale one.  All four are non-NULL on a sock_init_data() socket. */
void tbt_sk_ops_restore(struct sock *sk, const struct tbt_sk_ops *ops)
{
    write_lock_bh(&sk->sk_callback_lock);
    sk->sk_user_data = NULL;
    if (ops->data_ready)
        WRITE_ONCE(sk->sk_data_ready, ops->data_ready);
    if (ops->write_space)
        WRITE_ONCE(sk->sk_write_space, ops->write_space);
    if (ops->state_change)
        WRITE_ONCE(sk->sk_state_change, ops->state_change);
    if (ops->error_report)
        WRITE_ONCE(sk->sk_error_report, ops->error_report);
    write_unlock_bh(&sk->sk_callback_lock);
}

/* Close with RST and make the destroy synchronous: __tcp_close() takes the
 * SOCK_LINGER/!sk_lingertime branch (net/ipv4/tcp.c:3186-3190) and reaches
 * TCP_CLOSE + inet_csk_destroy_sock() before sock_release() returns.  A plain
 * kernel_sock_shutdown()+sock_release() leaves FIN_WAIT1/2 for up to 60 s,
 * still hashed and still able to receive data. */
void tbt_sock_abort_on_close(struct socket *sock)
{
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };

    if (!sock || !sock->sk)
        return;
    sock_setsockopt(sock, SOL_SOCKET, SO_LINGER, KERNEL_SOCKPTR(&lg),
                    sizeof(lg));
}
```

`tbt_sock_detach()` 只改恢复源（`tbt_sock.c:382-399`）：

```c
void tbt_sock_detach(struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (!sk)
        return;
    if (!test_and_clear_bit(0, &st->attached))
        return;
    /* Swap the callbacks BEFORE cancelling: a packet that arrives after the
     * cancel but before the swap would schedule work on a stream that is
     * about to be freed. */
    tbt_sk_ops_restore(sk, &st->sess->def_ops);

    cancel_work_sync(&st->w_read);
    cancel_work_sync(&st->w_drain);
    cancel_work_sync(&st->w_ctrl);
}
```

新增“释放 stream 自己那个 socket”的 helper（放在 `tbt_sock_detach()` 之后）：

```c
/* A stream owns exactly one socket: the accepted public socket on the near
 * side, the target connection on the far side.  Detaching callbacks is not
 * enough; the socket must not outlive the module.
 *
 * The second cancel_work_sync(w_kill) closes the w_ctrl race: a w_ctrl that
 * was already running when detach() cleared the callbacks can have queued
 * w_kill (tbt_ctrl_work -> tbt_stream_kill -> queue_work on system_unbound_wq).
 * The first cancel in session_destroy() ran before detach and cannot see it. */
void tbt_stream_release_socket(struct tbt_stream *st)
{
    struct sock *sk;
    struct socket *sock;

    tbt_sock_detach(st);            // idempotent: attached bit
    cancel_work_sync(&st->w_kill);

    sk = READ_ONCE(st->sk);
    if (!sk)
        return;
    sock = sk->sk_socket;
    /* Publish NULL before release: a concurrent kill reads st->sk and would
     * otherwise shut down a released socket. */
    WRITE_ONCE(st->sk, NULL);
    if (!sock)
        return;
    /* On the far side this socket is also sess->target; clear the alias or
     * session destroy shuts down freed memory. */
    if (READ_ONCE(st->sess->target) == sock)
        WRITE_ONCE(st->sess->target, NULL);
    tbt_sock_abort_on_close(sock);
    sock_release(sock);
}
```

`tbt_kill_work()`（`tbt_sock.c:322-347`）里的内联 linger 块可以改为调用
`tbt_sock_abort_on_close(sock)`；行为不变，减少两份实现。它已经在 detach 之后
release，所以恢复源修好后它自动安全。

`tbt_sock_attach()`（`tbt_sock.c:349-379`）不需要保存任何东西；只建议加：

```c
    WARN_ON_ONCE(!st->sess->def_ops.data_ready ||
                 !st->sess->def_ops.write_space ||
                 !st->sess->def_ops.state_change);
```

如果快照缺失，说明 `tbt_listen_start()` 的初始化路径被改坏了；在 attach 时
早一点 WARN 比在 detach 时发现更好。

### 5.3 `tbt_listen.c`：快照点、stop 恢复、拒绝路径

`tbt_listen_start()`（`tbt_listen.c:100-137`）在 `sock_create_kern()` 成功后、
安装 `tbt_ready` 之前：

```c
    tbt_sk_ops_save(&t->def_ops, s->sk);
    /* sock_init_data() guarantees all four; a NULL here means the socket was
     * not created the way we think, and we could never restore it. */
    if (WARN_ON_ONCE(!t->def_ops.data_ready || !t->def_ops.write_space ||
                     !t->def_ops.state_change || !t->def_ops.error_report)) {
        sock_release(s);
        return -EINVAL;
    }
```

注意：必须在 `write_lock_bh()` 安装 `tbt_ready` **之前**快照。这是全模块唯一
一个“回调仍是 core 默认值”的 socket 创建点（near/far 的 listener 都走这里）。

`tbt_listen_stop()`（`tbt_listen.c:140-158`）把 `150-151` 改为：

```c
    tbt_sk_ops_restore(s->sk, &t->def_ops);  // also clears sk_user_data
    atomic_set(&t->accepting, 0);
    cancel_work_sync(&t->w_accept);
    kernel_sock_shutdown(s, SHUT_RDWR);
    sock_release(s);
```

listener 的 close 会走 `__tcp_close()` 的 `TCP_LISTEN` 分支
（`tcp.c:3145-3151`）→ `inet_csk_listen_stop()` → `TCP_CLOSE` →
`inet_csk_destroy_sock()`，同步 unhash。所以这里**不需要** linger；恢复 core
回调是防御性的（release 过程中模块还 loaded，本来也不会崩）。

`tbt_accept_one()` 的失败路径（`tbt_listen.c:71-79`）必须修：

```c
        if (err) {
            pr_warn_ratelimited(...);
            /* The child inherited the listener's tbt_ready() and
             * sk_user_data = t.  Release it as a core socket: otherwise it
             * sits in FIN_WAIT1 and a later packet calls tbt_ready() with a
             * tunnel pointer that may already be freed. */
            tbt_sk_ops_restore(newsock->sk, &t->def_ops);
            tbt_sock_abort_on_close(newsock);
            sock_release(newsock);
            continue;
        }
```

### 5.4 `tbt_carrier.c`：release 恢复 core 回调 + `tcp_prot` + linger

`tbt_carrier_release()`（`tbt_carrier.c:800-826`）当前 `807-809` 先 cancel
`w_poll/w_connect/w_rx`，`816` 之后才换回调。一个在 cancel 与换指针之间到达的
软中断仍能 `schedule_work(&c->w_rx)`，让 w_rx 在 cancel 之后重新排队。
**不能简单地把“换回调”提到 cancel 之前**：`w_connect` 可能正在运行，并在
cancel 返回之前才把新 socket 装进 `c->sock`；`s = c->sock` 必须在
`cancel_work_sync(&c->w_connect)` 之后读，才是最终值。
因此采用“cancel → 读 s → 换回调 → 再 cancel 一次 w_rx → release”：

```c
    atomic_set(&c->stopping, 1);
    cancel_delayed_work_sync(&c->w_poll);
    cancel_work_sync(&c->w_connect);
    cancel_work_sync(&c->w_rx);
    /* w_connect cannot start again (stopping=1), so this is the final socket. */
    s = c->sock;
    if (s) {
        /* Same restore rule as a stream: the accepted carrier inherited
         * tbt_ready() from the far listener, and the dialled carrier was
         * overwritten in tbt_car_connect_work().  Use the session snapshot. */
        tbt_sk_ops_restore(s->sk, &sess->def_ops);
        /* tbt_tcp_prot lives in module .ro_after_init.  A socket left in
         * FIN_WAIT keeps sk_prot pointing there; its final destroy calls
         * sk->sk_prot->destroy() after the module may be gone. */
        sock_replace_proto(s->sk, &tcp_prot);
        /* A callback that ran before the restore may have re-queued w_rx.
         * After the restore no callback can schedule it again. */
        cancel_work_sync(&c->w_rx);
        tbt_sock_abort_on_close(s);
        kernel_sock_shutdown(s, SHUT_RDWR);
        sock_release(s);
        c->sock = NULL;
        c->sk = NULL;
    }
```

删除 `tbt_car_write_space_off()` 与 `tbt_car_state_change_off()`
（`tbt_carrier.c:127-155` 附近）：唯一使用者就是这里，替换后成为死代码。
`tbt_car_ready` 仍用于活着的 carrier，保留。

`tbt_car_connect_work()` 的 `release:`（`tbt_carrier.c:1162-1163`）：
此时回调还是 `sock_create_kern()` 的 core 默认值、`sk_prot` 还是 `tcp_prot`，
只需加 `tbt_sock_abort_on_close(s);` 再 `sock_release(s);`，避免 SYN_SENT/ESTABLISHED
的失败 socket 在 `FIN_WAIT` 停留。

`tbt_carriers_stop()`（`tbt_carrier.c:1002-1053`）的 free 循环（`1045-1050`）
在 `tbt_carrier_free(c)` 之前补：

```c
    list_for_each_entry_safe(c, tmp, &sess->dead, node)
    {
        tbt_carrier_retire(sess, c);   // cancels w_poll/w_connect/w_rx
        /* A w_poll that had already passed its `stopping` check can have
         * queued w_tx after the pre-lock cancel.  Nothing can queue it now
         * (stopping=1 and every scheduling source is cancelled), so this
         * second cancel is the barrier before kfree(c). */
        cancel_work_sync(&c->w_tx);
        cancel_delayed_work_sync(&c->w_pump);
        tbt_carrier_free(c);
    }
```

必须在 `pump_lock` **之外**做这两次 cancel（`w_tx`/`w_pump` 会取 `pump_lock`）；
当前循环本来就在 `mutex_unlock(&sess->pump_lock)` 之后，位置正确。

### 5.5 `tbt_session.c`：复制快照、release stream、target linger、二次 cancel

`tbt_session_create()`（`tbt_session.c:651-684`）在 `s->tunnel = t;` 之后：

```c
    s->def_ops = t->def_ops;
```

`tbt_session_destroy()` 的 stream 循环（`tbt_session.c:733-751`）改为：

```c
            cancel_work_sync(&victim->w_kill);
            /* detach + second w_kill cancel + linger0 release, in one place */
            tbt_stream_release_socket(victim);
            tbt_stream_put(victim);
```

target 块（`tbt_session.c:754-763`）改为：

```c
    if (s->target)
    {
        struct socket *ts = s->target;

        tbt_target_detach(s, ts);       // restores core callbacks if attached
        tbt_sock_abort_on_close(ts);
        kernel_sock_shutdown(ts, SHUT_RDWR);
        sock_release(ts);
        s->target = NULL;
    }
```

如果 far stream 已经在 `tbt_stream_release_socket()` 里释放并清掉 `s->target`，
这个块就不会执行；如果 target 已连接但 OPEN 还没到（没有 stream），它照常释放。
两条路径互斥，不会 double release。

`tbt_target_work_fn()` 的 `-ECANCELED` 路径（`tbt_carrier.c:1355-1359`）加
`tbt_sock_abort_on_close(s);`（socket 未 attach，回调是 core 默认值，安全）。

### 5.6 `tbt_core.c`：去掉假的屏障，写明真正的边界

```c
static void __exit tbtun_exit(void)
{
    /* 1. Stop new /proc calls and wait for every in-flight fop.
     *    remove_proc_entry() -> proc_entry_rundown() waits for pde users
     *    and closes pde_openers (including tbt_cfg_release). */
    tbt_procfs_exit();

    mutex_lock(&tbt_lock);
    /* 2. Close listeners, cancel every work item, restore the core callbacks
     *    on every socket, and release every socket with SO_LINGER=0 so that
     *    each one reaches TCP_CLOSE and is unhashed before this returns. */
    tbt_tunnel_shutdown();
    mutex_unlock(&tbt_lock);

    /* 3. No module callback or work item can be reached any more.  Wait for
     *    readers that started before the teardown, then for the module-text
     *    RCU callbacks (tbt_stream_free_rcu, kfree_rcu of tunnels).
     *
     *    flush_workqueue(system_wq) is NOT a substitute: it neither stops
     *    new scheduling nor flushes delayed work, and w_kill/w_fbk live on
     *    system_unbound_wq. */
    synchronize_rcu();
    rcu_barrier();
}
```

是否保留 `flush_workqueue(system_wq)`？建议**删除**。它给人“已经 flush 干净”的
错觉，却不覆盖 delayed/unbound；真正的保证来自每个对象的 `cancel_*_sync`。
如果保留，只能当调试断言用，不能写进“安全边界”的论证。

**同一补丁里的 F7 小修（`init.dmesg_after_rmmod` 需要）：** 7.2 把
`system_unbound_wq` 标成 deprecated，`queue_*` 会打印
`workqueue: work func ... enqueued on deprecated workqueue`。`adversarial/init`
的 `init.dmesg_after_rmmod` 会把这条 warning 算成失败。把 `tbt_fbk_kick()`、
`tbt_fbk_work()`、`tbt_tx_pump()` 的 `w_pump` 和 `tbt_stream_kill()` 的 `w_kill`
从 `system_unbound_wq` 换成 `system_dfl_wq`。两者都是 `WQ_UNBOUND`、
`WQ_MAX_ACTIVE`，属性相同；`system_dfl_wq` 只是没有 `__WQ_DEPRECATED`
（`kernel/workqueue.c:8092-8094`）。这样既保留“独立 unbound 池”的语义，
又不产生 warning。

### 5.7 锁顺序与 NULL 保护（补丁必须遵守）

- `tbt_sk_ops_save()` 只在 `tbt_listen_start()` 的 `sock_create_kern()` 之后调用，
  不需要锁（socket 还没发布）。
- `tbt_sk_ops_restore()` 内部 `write_lock_bh(&sk->sk_callback_lock)`；
  **调用者不得再持有任何会被软中断获取的锁**，也不得在锁内调用它。
- `cancel_work_sync()` 必须在 `sk_callback_lock` 之外；本方案保持
  “先换指针 → 解锁 → cancel”。
- `sock_setsockopt()` / `sock_release()` 会睡眠，只能在进程上下文；
  当前所有调用点都是进程上下文（module_exit、work、proc write）。
- `sock_replace_proto()` 用 `WRITE_ONCE`，可与软中断并发；新旧 proto 的
  函数指针相同（`tbt_tcp_prot` 是 `tcp_prot` 的副本），切换期间语义不变。
- NULL 保护：
  - `tbt_sk_ops_save()` 后若任一指针为 NULL，`tbt_listen_start()` 返回 `-EINVAL`，
    不安装回调；
  - `tbt_sk_ops_restore()` 对 NULL 字段跳过并保留当前值（理论上不可达）；
  - `tbt_stream_release_socket()` 对 `sk == NULL`、`sk->sk_socket == NULL`
    都直接返回，保证重复调用安全（`w_kill` 可能已经释放过）。
- 锁序不变：`tbt_lock` → `sess->lock` / `sess->pump_lock` → `sk_callback_lock`。
  `sess->lock` 是 spinlock，绝不跨 `cancel_*_sync` / `sock_release`。
- 长期建议（不在 F2 最小补丁内）：把 `tbt_session_destroy()` 移到 `tbt_lock`
  之外，先在锁内 `list_del_rcu` 摘链，再锁外销毁；这样 `/proc` 不会被 teardown
  长时间阻塞，也消除“work 取 tbt_lock”的潜在 ABBA。

### 5.8 为什么不用另外两种“更省事”的方案

- **只在 `module_exit` 里 `synchronize_rcu()` 提前**（交接文档 §8 的建议）：
  无效。F2 的调用者是 TCP 软中断，不经过模块的 RCU 读临界区；
  `synchronize_rcu()` 不会让一个仍然活着的 socket 的回调指针变成 core 函数。
- **只把 `tbt_sock_detach()` 改成“不恢复回调”**：更糟。socket 仍指向模块 text，
  只是 `sk_user_data=NULL`；下一次包进来仍然取指到已释放 text。
- **只 release 公共 socket、不恢复回调**：如果 release 是 linger 0 且同步销毁，
  可以挡住 F2 的已知路径；但 W4（被拒绝 child）、W3（carrier FIN_WAIT）、
  W5（target）仍会漏。恢复 core 回调是“所有 socket 的统一不变量”，
  比“逐个记得 release”更可靠。

---

## 6. 为什么 `adversarial/run.sh init.rmmod` 会通过

`phase_rmmod`（`adversarial/init:334-360`）的判定是：

```sh
timeout 15 rmmod tbtun          # 必须 rc=0
wait_timeout "$EP" 8            # 应用必须自己退出，不能是 137
clean_abort /tmp/echo_rmmod.out # ECHO_EOF* 或 errno=104/32/103
dmesg | grep -cE 'WARNING:|BUG:|Oops|...'   # 必须为 0
```

补丁后的时序：

1. `rmmod` → `tbtun_exit()` → `tbt_procfs_exit()` 等在途 fop。
2. `tbt_tunnel_shutdown()`：
   - near listener：`tbt_listen_stop()` 恢复 core 回调、cancel `w_accept`、
     `sock_release`（listener 同步销毁）。
   - near session：`carriers_stop` 取消全部 carrier work，恢复 core 回调 +
     `tcp_prot`，`SO_LINGER=0` + `sock_release` 让每条 carrier 走
     `tcp_disconnect()` → `TCP_CLOSE` → `inet_csk_destroy_sock()`，同步 unhash。
   - near stream：`cancel w_kill` → `tbt_stream_release_socket()` 恢复
     `sock_def_*`、第二次 cancel `w_kill`、`SO_LINGER=0` + `sock_release`。
     公共 accepted socket 收到 RST 并同步销毁；应用的下一次 read/write
     得到 `ECONNRESET`（errno 104）或 `EPIPE`（32）。
   - far 侧同理；target socket 也以 linger 0 释放。
3. `module_exit` 返回时，内核里不再有任何指针指向模块 text 或模块 `.data`；
   `rcu_barrier()` 等完 `tbt_stream_free_rcu`/`kfree_rcu`。
4. 应用被 RST 唤醒后退出；`clean_abort` 接受 104/32/103，`wait_timeout`
   返回非 137；`dmesg` 无 BUG/WARNING。`init.rmmod` PASS。
5. 随后 `insmod /tbtun.ko debug=1` 成功，`/proc/net/tbtun/config` 存在，
   `init.reload` PASS；`init.dmesg_after_rmmod` PASS。

即使应用的 FIN 在 `sock_release` 之前已经到达，它命中的是已经恢复的
`sock_def_wakeup`（core），不会进模块 text；如果 FIN 在 release 之后到达，
socket 已 `TCP_CLOSE` 且 unhash，内核不会把它交给该 socket。两条路径都不会 oops。

---

## 7. 验收命令

按顺序（由占用构建/QEMU 的工程师执行）：

```sh
# 1. 静态不变量（不需要 QEMU）
bash review/lint.sh

# 2. 构建 + host 测试（确认没有编译/风格回归）
source ~/.cache/tbtun-build/env.sh
make check

# 3. F2 本体：必须三行 PASS，且没有 console.hang.rmmod
TBT_ADV_GROUPS=rmmod adversarial/run.sh
grep -E 'init\.(rmmod|reload|dmesg_after_rmmod)' adversarial/results.txt
# 期望：
#   ADV_RESULT init.rmmod PASS ...
#   ADV_RESULT init.reload PASS ...
#   ADV_RESULT init.dmesg_after_rmmod PASS ...
#   （没有 console.hang.rmmod / console.klog.rmmod FAIL）

# 4. 证据日志不得再有取指 oops
grep -nE 'BUG:|Oops|RIP:|Call Trace|freed module text' \
     adversarial/console-rmmod.log
# 期望：只匹配到 PHASE 文本或空；不得匹配 405-500 那一段

# 5. 同一缺陷的非卸载表现（F2 报告里提到的泄漏）：
#    flush 后应用应收到 EOF/RST，而不是 ECHO_TIMEOUT op=read
TBT_ADV_GROUPS=flush adversarial/run.sh
grep -E 'init\.(flush_midstream|echo_after_flush)' adversarial/results.txt
```

补充（可选，验证 carrier 释放路径没有回归）：

```sh
tests/qemu/run.sh --probes A,B,E,K --allow-skip --timeout 300
```

重点看探针 E/K：carrier 被替换/退休时现在会发 RST 而不是 FIN；`join_dup`、
`carrier_replaced`、`holes_repaired` 不应变差。

---

## 8. 残余风险

1. **RST 语义变化。** teardown 现在对公共 socket / carrier / target 都发 RST。
   应用侧 `clean_abort` 接受 104/32/103，但如果某个上层协议依赖“flush 时收到
   EOF”，行为会从 EOF 变成 ECONNRESET。F5（半关闭 EOF）本来就没实现，
   所以这不是新增的协议缺口；但需要在 `DESIGN.md §8.4`/§9.7 写清
   “teardown 是 reset，不是 half-close”。
2. **恢复 core 回调会让“漏 release”从崩溃退化成泄漏。** 这是有意的：
   崩溃比泄漏严重。但它也意味着不能只靠“恢复回调”就宣称 teardown 正确；
   release 的不变量仍必须保持。建议在 `review/lint.sh` 加一条机械检查：
   `tbt_session_destroy()` 的 stream 循环必须调用 `tbt_stream_release_socket()`。
3. **`sk_error_report` 未被替换。** 当前安全；如果未来有人安装它，必须加进
   `tbt_sk_ops` 的保存/恢复，否则同一类 bug 会以 `EPOLLERR` 路径回归。
4. **`tbt_pub_*` 用 `read_lock()` 而不是 `read_lock_bh()`**
   （`tbt_sock.c:114/125/133`）。当前 `sk_data_ready`/`sk_write_space` 只在软中断
   调用，`sk_state_change` 的进程上下文调用点（`inet_shutdown()`）持有
   `lock_sock()`（BH 已关），所以没有可复现死锁；但和 `write_lock_bh()` 配对时，
   这是一个“只要将来多一个 BH-enabled 调用点就死锁”的隐患。建议统一改成
   `read_lock_bh()`（carrier 侧已经是 `read_lock_bh`）。
5. **`pump_lock` 跨 `tbt_carrier_release()` 的睡眠。** 本补丁没有重构它；
   `w_rx` 现在明确不 pump（`tbt_carrier.c:443-445`），所以不死锁。若未来
   `w_rx` 需要 pump，必须先改锁结构（先锁外 cancel，再拿 `pump_lock` 换槽）。
6. **`tbt_lock` 跨整个 teardown。** 本补丁保持现状；它阻塞所有 `/proc` 读写，
   并把“work 不得取 `tbt_lock`”变成隐式契约。建议后续把 destroy 移到锁外。
7. **`synchronize_rcu()` 在 `tbt_carriers_stop()` 末尾（`tbt_carrier.c:1051-1053`）
   的注释不准确。** 它不能替代 `sk_callback_lock` 的屏障；真正保证“回调看不到
   已释放 carrier”的是 `write_lock_bh` + `sk_user_data=NULL` + `cancel_work_sync`。
   RCU 同步保护的是 `w_rx` 里对 `sess->streams` 的 `hlist_for_each_entry_rcu`。
   本补丁不删除它，但应改注释，避免下一个人误以为 RCU 能停 socket 回调。
8. **W6/W7 是窄窗口。** 静态上成立，但没有日志证据；修复后建议在
   `review/lint.sh` 或一个 KASAN/KCSAN 的 QEMU 变体里跑一次 `init.rmmod`
   多次循环（例如 20 次），观察是否还有 `workqueue` 相关 warning。
9. **`tbt_stream_release_socket()` 的第二次 `cancel_work_sync(&st->w_kill)`
   可能等待一个正在 `sock_release()` 的 `w_kill`。** 这是有界等待
   （`sock_release` 不阻塞在对端窗口上；`tbt_sock_deliver` 用 `MSG_DONTWAIT`），
   但它发生在 `tbt_lock` 下，最坏情况会把 `/proc` 阻塞一次 socket close 的时间。
   可接受；若要更干净，把 destroy 移到锁外。

---

## 9. 对用户假设的逐条确认

1. **“detach 把三个回调恢复成模块 text”**：**确认**，`tbt_sock.c:392-394`。
2. **“detach 后公共 socket 仍被应用持有”**：**部分确认，需纠正**。应用持有的是
   客户端 socket；accepted socket 在内核里，应用没有 fd。它之所以还活着，是因为
   `tbt_session_destroy()` 没有 `sock_release()`（`tbt_session.c:749-750`），
   不是应用持有。
3. **“模块卸载后 FIN 走 `tcp_fin -> sk_state_change` 跳到已释放地址”**：**确认**，
   `tcp_input.c:4983-4984`。当前日志先到的是数据，走
   `tcp_data_queue -> sk_data_ready`（`:423`）；早先日志是 FIN，走
   `tcp_fin+0x96`（`:431`）。两者都指向已释放 text，且都在
   `tbt_sock_detach()` 恢复的那组指针里。
4. **“Modules linked in 为空”**：确认，当前日志 485 行；486 行明确写了
   `last unloaded: tbtun`（早先日志是 519/520 行）。
5. **“sock_init_data 的默认回调非 NULL，可以在 attach 保存、detach 写回”**：
   **前半确认，后半不成立**。默认值确实非 NULL（`sock.c:3775-3778`），
   而且“把函数指针当数据保存/写回”确实不需要引用未导出符号（不产生符号重定位，
   只是间接调用一个合法的 core 地址）。但 accepted socket 在 attach 时，
   `sk_data_ready` 已经是继承来的 `tbt_ready`（`sock_copy()` +
   `inet_csk_clone_lock()`），不是默认值；另外两个指针目前是默认值，但不可依赖。
   必须从 `tbt_listen_start()` 里 `sock_create_kern()` 刚返回的 listener 快照，
   见 §5.3。
6. **“carrier/listener 是否同样有问题”**：
   - carrier：**是，而且 release 之后仍可能收到回调**（`FIN_WAIT1/2` 最长 60 s，
     `tcp_data_ready()` 在 `SOCK_DEAD` 时仍调用 `sk_data_ready`），并且
     `sk_prot` 还指向模块 `.data`。见 W3。
   - listener：**release 之后不会**（TCP_LISTEN close 同步销毁、unhash）；
     风险只在 release 过程中，而那时模块还 loaded。见 W4 的对比。
7. **“被拒绝的 accepted child”**：用户没有提，但这是同一继承机制的另一处，
   而且不需要 rmmod 就能 UAF（`tbt_listen.c:78`）。补丁必须覆盖。

---

## 10. 一句话交接

> F2 不是“RCU 没等够”，而是“模块把自己的函数指针留在了一个不会被释放的 socket 上”。
> 修法不是提前 `synchronize_rcu()`，而是：**从 fresh listener 快照 core 回调；
> 每个 detach/release 写回快照；每个模块拥有的 socket 用 `SO_LINGER=0` +
> `sock_release()` 同步销毁；再补两次 work 的二次 cancel。**

---

## 11. 实施记录（2026-11-05，本轮）

- 模块 sha256：`5ed71bbd65076a5fb6d1b380dedaa4d686a903300ac74823092cb03b3740fefb`。
- `make check`：365 checks / 0 failures；`test_fuzz` 8349443 / 0；
  `test_http` 249 / 0；h11 交叉校验通过。
- `bash review/lint.sh`：15/15 checks，12/12 self-tests（`listen-clear-before-cancel`
  现在接受 `tbt_sk_ops_restore` 作为“先清回调再 cancel”的等价写法）。
- `TBT_ADV_GROUPS=rmmod adversarial/run.sh`：
  `init.rmmod` PASS、`init.reload` PASS、`init.dmesg_after_rmmod` PASS、
  `ctl.close_flush` PASS；4 passed / 0 failed / 0 skipped；
  console 无 BUG/Oops/RIP/WARNING/panic。
- `tests/qemu/run.sh --probes A,B,E --timeout 240`：A PASS、B PASS、E PASS。
- `tests/qemu/run.sh --probes K --timeout 240`：**FAIL**，但同一台机器、同一环境
  下用**补丁前**的基线模块（`~/.cache/tbtun-build/bench/stage/tbtun.ko`，
  sha `c18ff6c4...`，与 `review/baseline` 的 adversarial 基线 sha 一致）重跑
  K 也 FAIL（`holes_repaired is 0`，`TXSTALLKILL age_us≈500ms`）。
  所以 K 的失败不是本补丁引入的；它是 hole-repair/PREDICT 的既有问题
  （review 13 的范围），需要单独修。
