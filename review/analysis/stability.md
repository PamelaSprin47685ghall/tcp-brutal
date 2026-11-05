# 0.0.1 发布前稳定性与资源问题分析

> 范围：只做静态分析与方案。未修改源文件，未编译，未运行 QEMU。
> 行号取自 2026-11-05 当前工作树；若并行改动导致漂移，按“函数名 + 语句”重新定位。
> 证据快照：`adversarial/results.txt` 正被并行运行重写（当前 `sha=c18ff6c4`，public/final 组已落盘；
> `init.port_exhaust` 行来自上一轮 `sha=9c9ddb03`）。本文引用行号与数字时同时给出用例名。

## 0. 结论摘要

| # | 问题 | 最小根因 | 方案 | 发布阻断 |
|---|---|---|---|---|
| 1 | `pub.writer` 在 ~75 MB 后 `EAGAIN`，MemAvailable 412→134 MB | 重传路径把 64 MiB 交付环常量当作发送队列上限（`tbt_tx.c:548`）；每个 retain 每 RTO 再加一份副本；64 MiB rx_q 按 8 KiB 帧分配，`8296 > 8192` 落入页分配器 16 KiB 块（2×）；`tx_retain_bytes` 默认 0 | 每个 retain 至多一个待写副本；重传超发上限改为 ~320 KiB；retain 硬上限 8 MiB；模块级 `mem_max_mb=128` 预算 + 入口暂停；帧/retain 专用 kmem_cache | 是 |
| 2 | `seq < rx_next` 的重叠帧永不淘汰，`rx_ooo_bytes` 只增 | `tbt_rx_pull_locked()` 只拉 `seq == rx_next`（`tbt_rx.c:34-67`），插入允许重叠（`:70-103`），入口对 `seq < rx_next` 整帧丢弃（`:176`） | pull 分三种情况：全旧→释放；跨接→用 `off` 裁剪前缀后入队；精确→入队。入口同样裁剪 | 是（廉价） |
| 3 | `-EAGAIN` 时 pinned 控制帧被释放，FIN 可能丢 | `tbt_tx.c:1433-1436`、`:1452-1455` 直接 `kfree(fr)`；FIN 只由 `tbt_ctrl_work` 调一次（`tbt_sock.c:295`） | `-EAGAIN` 时重新入队并 arm 2 ms `w_pump`；死路径错误时 FIN/RST 重挂到新 generation；加 `tx_fin_sent`/`tx_rst_sent` | 是 |
| 4 | 普通自增在多载体并发下丢更新 | 会话级计数多写者；`tx_ack`/`uack`/`recv_cnt` 是 check-then-write，可回退 | 数据路径计数改为 per-carrier `u64_stats_t` 分片，读取点求和；单调高水位用 atomic64/atomic 的 max-CAS；低频事件计数用 atomic64 | 是（`tx_ack` 回退是正确性问题） |
| 5 | 端口耗尽后载体无限重试，操作员看不到 | 隧道创建不预留端口（`tbt_carrier.c:875-895`）；`tbt_port_alloc()` 返回 0 只计数重试（`:1099-1110`）；`tunnels` 只有 listening/idle | 创建时一次性预留 `n_car` 个端口，失败返回 `-EADDRNOTAVAIL` 并回滚；替换失败带退避重试；`tunnels` 加 `degraded`，`stats`/`ports` 暴露空闲数；一次 ratelimited 警告 | 是 |
| 6 | `rx_reorder_bytes=0`/`hole_budget_ms=0` 被接受 | 解析器允许 0（`tbt_parse.c:541-549`），`tbt_cfg_sane()` 不检查（`tbt_session.c:106-140`） | 对三个致命键拒绝 0：`rx_reorder_bytes`、`hole_budget_ms`、`carrier_stall_ms`；`tx_retain_bytes`/`tunnel_rate`/`replay`/`drip_ms`/`keepalive_ms` 的 0 语义保留 | 是 |

---

## 1. adversarial `pub.writer`：100 MiB / 1 GiB 写入在 ~75 MB 后阻塞

### 1.1 现象与构造

`adversarial/results.txt`（当前 `sha=c18ff6c4`，第 8–9 行）：

```
pub.writer FAIL attempted 100 MiB, accepted 75306298 bytes then blocked (errno=11),
           MemAvailable 412508 -> 133980 kB
pub.writer FAIL attempted 1024 MiB, accepted 74990705 bytes then blocked (errno=11),
           MemAvailable 411312 -> 180312 kB
```

构造：`adversarial/init:66-75` 的 `setup_raw` 建的是 **n=1、m=0、profile=raw** 的近/远隧道；
`adversarial/init:161,165` 跑 `adv_probe writer <PUB> 100|1024 0`，`do_read=0`；
`adv_probe.c` 的 `mode_writer()` 只 `send(1 MiB)`、从不 `read`，目标端是 `adv_probe target`
（`adversarial/init:117-120`）的回显进程。近端和远端在同一个 512 MB 客户机里。

因此这不是“单向灌 100 MiB”而是**回显死锁**：

1. 应用只写不读 → 近端交付环 `rx_q` 的回显字节堆到 `TBT_RX_RING_MAX`（64 MiB），`st->paused` 置位，
   `tbt_car_rx_work()` 停止读载体（`tbt_carrier.c:389-425`，暂停判断在 `:405-420`）。
2. 近端不读 → 载体回程窗口关闭 → 远端发送队列/retain 堵住 → 远端 `tbt_tx_submit()` 命中
   `TBT_SQ_MAX`（64 KiB）置 `tx_paused`，停止从 target socket 捕获回显。
3. target 的回显 `write()` 阻塞 → target 停止读应用数据 → 远端 `rx_q` 堆积 → 远端也暂停读载体。
4. 近端发送方向堵住 → 近端 `tx_q`/窗口满 → 应用 `send()` 返回 `EAGAIN`（errno 11）。

**这个用例的通过条件不是“传完 100 MiB”**，而是 `sent < total && after > 200000`
（`adv_probe.c:1611`）：阻塞是预期行为，内存必须仍有 >200 MB 可用。所以正确答案是**有界背压**，
不是提高吞吐。

### 1.2 内存去向：逐段预算

先给结论：这不是“某个队列漏了 free”，而是**三份有意的缓冲叠加，且分配器把每份放大 2×**。

| 持有者 | 代码路径 | 当前上限 | 说明 |
|---|---|---|---|
| 公共 socket 接收队列 | `tbt_sock.c:367-370` 设 SO_RCVBUF=1 MiB，但受 `net.core.rmem_max`（默认 212992）限制 | ~0.4 MB | 已捕获但尚未进帧的应用字节 |
| 会话发送队列 `tx_q` | 捕获侧 `tbt_tx.c:1616,1653` 限 64 KiB；**重传侧 `tbt_tx.c:548` 允许 `TBT_SQ_MAX + TBT_RX_RING_MAX` = 64 MiB + 64 KiB** | 64 MiB + 64 KiB | 本用例的主要异常项 |
| retain 环 | `tbt_tx.c:249-299` 只在 `acked && (old || over)` 时回收；`cap = cfg.replay`（默认 8 MiB），硬上限 `tx_retain_bytes` 默认 0（`tbt_parse.c:429`） | 8 MiB（条件）/ 1 s 内不限字节 | acked 帧保留 1 s（`TBT_REPLAY_MS`） |
| 近端交付环 `rx_q` | `tbt_rx.c:895-908` 在 `rx_queued >= TBT_RX_RING_MAX` 时暂停 | 64 MiB 载荷 | 应用不读时的回显 |
| 远端交付环 `rx_q` | 同上 | 64 MiB 载荷 | 与近端 `rx_q` 的**字节之和 ≤ 应用已接受字节**（回显是副本，不是新数据） |
| 重排树 | `tbt_rx.c:70-103`，`rx_reorder_bytes` 默认 256 KiB | 256 KiB | 正常 |
| 载体 sndbuf/rcvbuf | `tbt_carrier.c:1140-1141,1305-1306` 设 1 MiB，受 `wmem_max/rmem_max` 限制 | ~0.4 MB × 2 | n=1，量小 |
| parser/http 缓冲 | `tbt_carrier.c:701-720` | ~17 KiB/载体 | n=1，量小 |
| 应用自身 sndbuf/rcvbuf | 用户 socket，不在模块账本 | 自动调优，数 MB | 不计入模块内存 |

**分配器放大 2×**：`pahole` 实测 `struct tbt_frame` 的 `data` 偏移 104，`struct tbt_retain` 的
`data` 偏移 296。捕获块 `TBT_PAYLOAD_MAX=8192`（`tbt_uapi.h:90`）时：

- `struct_size(fr, data, 8192) = 8296`
- `struct_size(r, data, 8192) = 8488`

而本内核 `KMALLOC_SHIFT_HIGH = PAGE_SHIFT + 1`（`.../build/include/linux/slab.h:663`），
`KMALLOC_MAX_CACHE_SIZE = 8192`。**8296/8488 都超过 8192，`kmalloc()` 走 `kmalloc_large()` →
`alloc_pages(order=2)`，每个 8 KiB 载荷实际占一个 16 KiB 块。** 它们不会出现在 `kmalloc-16k`
的 slab 计数里（该缓存不存在），所以 `slabinfo` 看不到，必须看 `MemAvailable`/`vmstat`。

**算术**（100 MiB 那轮，MemAvailable 下降 278,528 kB ≈ 272 MiB）：

- 交付环：应用已接受 75 MB，按上面的不变量，近端 `rx_q` + 远端 `rx_q` + socket 缓冲合计约 75 MB 载荷。
- 重传副本：`tx_q` 允许到 64 MiB 载荷，本用例下大概率已积累数十 MiB。
- retain：≤8 MiB acked + 未 acked（窗口 128 KiB + `tx_q` 64 KiB）。
- 75 MB + 48～64 MB ≈ 123～139 MB 载荷；按 2× 放大 ≈ 246～278 MiB。
- 实测 272 MiB（100 MiB 轮）与 225.6 MiB（1 GiB 轮，重传队列少一些）正好落在这个区间。

以上拆分是静态推断；1.5 的 `txq=`/`retain_bytes=`/`rxq=` 三个统计量会把它变成确定结论：
哪个接近上限，哪个就是主项。不要把本节的比例当成已测数据。

### 1.3 根因：哪个上限缺失或失效

1. **`TBT_SQ_MAX` 64 KiB 被重传路径绕过**（`tbt_tx.c:548`）。
   `tbt_tx_retx()` 每 RTO 为“卡在队列里的 pending 副本”再排一份新副本：
   `:478` 的 `r->pending` 只保证“一个 RTO 内不重复”，过了 RTO 就继续 `cand[n++]`（`:522-525`），
   并不检查旧副本是否还在 `tx_q` 里。旧副本没写出去（载体 `sk_stream_memory_free()` 为假），
   新副本照样入队。`r->pending` 是 bool，不是计数，也没有“每个 retain 至多 N 份”的约束。
   唯一的闸门就是 `:548` 的 64 MiB + 64 KiB。按“每 20 ms 最多 8 个候选、每份 8 KiB”粗估，
   `tx_q` 可以以数 MB/s 增长到 64 MiB；每份 8296 B → 16 KiB 页块，即 ~128 MiB。
   这是本用例最直接、最可修的漏洞。

2. **`TBT_RX_RING_MAX` 是“每流 64 MiB 载荷”，不是“模块内存上限”**（`tbtun.h:127`）。
   它按载荷计，不按分配成本计；两个方向在同一个客户机里就是两份；没有全局账本。
   在 512 MB 客户机里，仅两个 64 MiB 环按 2× 就是 256 MiB。

3. **`tx_retain_bytes` 默认 0 = 无限**（`tbt_parse.c:429`）。
   `tbt_tx_retain_trim()`（`tbt_tx.c:305-338`）确实会在超过 `retain_max` 时回收，但它**连未 acked 的帧也回收**，
   等于静默丢数据（接收端随后只能等空洞预算重置）。所以它不能作为默认安全阀；默认必须是非零硬上限，
   且只回收“已由全局 ack 确认”的前缀。

4. **`replay` 8 MiB 上限是条件性的**（`tbt_tx.c:267`）。
   `acked` 要求 `tbt_retain_all_received()`：帧用过的**每一条**路径的 `uack` 都 ≥ 帧尾。
   如果 FBK 被堵在回程数据后面（本用例正是如此），`uack` 会停住；帧在 `uack` 之下的部分仍能回收，
   但在停住处之后的 retain 会保留。它不是本用例 272 MiB 的主项，但属于同一类“上限失效”，
   修 1.3.3 时应一起处理。

5. **`data_frames++` 在写之前**（`tbt_carrier.c:521`，review 14）。
   它把 `-EAGAIN`/`-EIO` 也计成“已发出”，使 `recv_lag`（`tbt_tx.c:886`）永久为正；
   `recv_lag` 又让 `tbt_retain_received()`（`:611`）对所有帧返回 false。
   这条链主要污染空洞门控和 `retx_ppm`，也会让“按路径已收”判据失效；应一并把自增移到写成功之后。

### 1.4 方案

#### A. 重传队列：每个 retain 至多一个待写副本

改动点：

- `struct tbt_retain`（`tbtun.h`）把 `bool pending` 换成：
  ```c
  u8  pending_copies;      /* 已入队、尚未写出的重传副本数，0..1 */
  u64 pending_since_ns;    /* 最早一份 pending 副本的入队时间 */
  ```
  `struct tbt_frame` 增加 `bool retx_copy;`（`path` 后正好有 3 字节空洞，不增大结构）。
- `tbt_tx_retx()`（`tbt_tx.c:430-585`）：
  - 候选帧若 `pending_copies` 非零且 `now - pending_since_ns < rto`，跳过；
  - 否则**先删掉队列里那份卡住的副本**（在 `tx_lock` 下扫 `sess->tx_q` 找 `fr->ret == r && fr->retx_copy`，
    `list_del`、`sess->tx_queued -= fr->len`、`pending_copies--`、`kfree(fr)`），再排一份新的；
  - 新副本 `fr->retx_copy = true; r->pending_copies = 1; r->pending_since_ns = now;`。
- 写成功（`tbt_tx.c:1506-1535` 的 DATA 分支）与 `drop:`（`:1385-1405`）都要对 `retx_copy` 副本减
  `pending_copies`；GC/trim 把 `fr->ret = NULL` 时同时清 `fr->retx_copy`。
- `:548` 的上限改为：
  ```c
  u32 retx_over = 2u * TBT_PAYLOAD_MAX * (sess->n_car ? sess->n_car : 1);
  if (READ_ONCE(sess->tx_queued) > TBT_SQ_MAX + retx_over) { ... }
  ```
  16 条载体时约 64 KiB + 256 KiB。**永远不要用 `TBT_RX_RING_MAX` 作为发送队列上限。**

锁/内存序：`tx_q`、`tx_queued`、`pending_copies`、`retx_copy` 都在 `sess->tx_lock` 下；
pump 是唯一把帧移出 `tx_q` 的上下文（`pump_lock` 串行），所以“删旧副本再排新副本”不会与写并发。
`pending_since_ns` 用 `WRITE_ONCE`/`READ_ONCE` 即可，因为它只在 `tx_lock` 下读写。

热路径：正常无阻塞时 `pending_copies == 0`，只多一次判断；阻塞时不再每 RTO 复制一份，净收益。

#### B. retain 硬上限

- `cfg.tx_retain_bytes` 默认从 0 改为 8 MiB（`tbt_parse.c:429`），0 仍保留“显式无限”语义。
- `tbt_tx_retain_gc()`：超过硬上限时，**按全局 `sess->tx_ack` 回收 acked 前缀**，不再依赖
  `tbt_retain_all_received()` 的逐路径证明；逐路径证明继续用于 `replay_ms` 的保留策略和丢包统计。
  即：
  ```c
  bool globally_acked = r->seq + r->len <= ack;
  bool over = (st->retain_max && st->retain_bytes > st->retain_max) ||
              (cap && st->retain_bytes > cap);   /* 硬上限或 replay 软上限 */
  if (over && globally_acked) { free; }
  else if (!acked) break;
  else if (!old && !over) break;
  ```
  这样即使 FBK 被堵、`uack` 停住，内存也有界；代价是极端情况下可能把“还在接收端 parser 里的副本”
  算成已收，产生少量伪重传——这是内存安全换来的可接受代价，且 review 14 修好后 `recv_lag` 不再误报。
- `tbt_tx_retain_trim()` 不再静默回收未 acked 帧。若硬上限被未 acked 数据顶穿（只可能是窗口/计数 bug），
  用新原因 `TBT_ST_RETAIN` 杀流并计数，而不是丢字节。未 acked 部分本来就被发送窗口（128 KiB）
  和 `tx_q`（64 KiB）约束，正常不会触发。

锁：与现有一致，`retain_bytes`/链表在 `sess->tx_lock` 下；GC 只在 pump（`pump_lock`）里跑，
`fr->ret` 的清理与 `tbt_tx_retx` 的候选指针不并发。

#### C. 分配器放大

三个选项，按 0.0.1 的侵入性排序：

1. **专用 kmem_cache（推荐，线协议不变）**：
   - `tbt_frame_cache`：对象 `sizeof(struct tbt_frame) + TBT_PAYLOAD_MAX`（8296）；
     `tbt_frame_ctl_cache`：对象 512 B，供 len ≤ 512 的控制帧/FBK/PREDICT/CANCEL/PAD/HELLO/OPEN/FIN/RST。
   - `tbt_retain_cache`：对象 `sizeof(struct tbt_retain) + TBT_PAYLOAD_MAX`（8488）。
   - SLUB 会按 `slub_min_objects`/`slub_max_order` 打包（8296 B 通常是 order-3 装 3 个对象
     或 order-4 装 7 个，每对象约 9.4–10.9 KiB，约 1.1–1.3×），而不是页分配器的 2×。
   - 新增 `tbt_frame_free(fr)` / `tbt_retain_free(r)`，按 `fr->len > 512` 选 cache；
     替换 `tbt_tx.c` 的 10 处 `kfree(fr)`、`tbt_rx.c:162` 的分配、`tbt_rx_free_queue()`、
     `tbt_tx_free_queue()`、`tbt_tx_free_stream()`、GC/trim 的 `kfree(r)`。
   - 模块 init 创建、exit 销毁；`kmem_cache_alloc(..., GFP_ATOMIC)` 在软中断/原子上下文安全。
2. **把 `TBT_PAYLOAD_MAX` 降到 7896 以下**，使 `struct_size` ≤ 8192，落回 kmalloc-8k（1×）。
   这是线协议变更：HELLO 会校验 `payload_max`（`tbt_rx.c:426-427`），必须同步 `TBT_CONV`、
   `DESIGN.md §3.3`、`host/test_fuzz.c` 的 conv 钉子和所有对端。0.0.1 不推荐。
3. **拆分配置**：`struct tbt_frame` 只放头部，`data` 改成 `u8 *`，载荷用 `kmalloc(len)`（8192 正好 1×）。
   每次分配变两次，热路径分配次数翻倍；作为备选。

无论选哪个，`/proc/net/tbtun/stats` 都应新增 `mem_bytes`（见 D），让“按载荷计”和“按分配计”的差异可见。

#### D. 模块级内存预算与背压

这是“背压设计与上限”的核心。现有背压是分层的，但缺最后一层全局闸门：

| 层 | 机制 | 当前值 | 0.0.1 改动 |
|---|---|---|---|
| 1 | 公共/目标 socket 的 TCP 窗口 | rcvbuf ~0.4 MB | 不变 |
| 2 | 捕获队列 `TBT_SQ_MAX` → `tx_paused` | 64 KiB | 不变 |
| 3 | 发送窗口 `tx_ack + rx_reorder_bytes/2` | 128 KiB（默认） | 不变 |
| 4 | 重传队列 | **64 MiB + 64 KiB** | 64 KiB + 2·PAYLOAD·n_car |
| 5 | retain | 8 MiB（条件）/ 0（硬） | 8 MiB 硬上限 |
| 6 | 交付环 `TBT_RX_RING_MAX` → `paused` | 64 MiB/流 | 保持（严格 40 MiB 用例需要） |
| 7 | **模块级内存预算** | **无** | `mem_max_mb=128`，分配即计费，入口暂停 |

实现：

```c
static atomic64_t tbt_mem_bytes;
static unsigned int tbt_mem_max_mb = 128;   /* module_param, 0444 */
static u64 tbt_mem_max_bytes;               /* init: mb << 20 */

/* 分配成本：当前分配器 16384；换专用 cache 后按 cache 的每对象足迹（约 9472）。 */
static u32 tbt_frame_cost(u16 len) { ... }
static u32 tbt_retain_cost(void)   { ... }
```

- `tbt_frame_alloc()`（`tbt_tx.c:50-64`）和 `tbt_retain_alloc()`（`:70-81`）在分配**前**检查预算，
  超了返回 NULL；成功分配后 `atomic64_add_return(cost, &tbt_mem_bytes)`，统一的 free helper 里减。
  这样重传路径（`tbt_tx_retx()`/`tbt_tx_inject_locked()`）也不会绕过预算。
- 控制帧（`len <= 512`，HELLO/OPEN/FIN/RST/RETIRE/PAD/FBK）**不计入数据预算**，或单独预留
  1 MiB 控制预算；否则预算满时 FIN/HELLO 分配失败，正好触发第 3 节的丢 FIN 问题。
- 入口闸门：
  - `tbt_car_rx_work()`（`tbt_carrier.c:389-425`）在 `tcp_read_sock()` 前检查预算，超了就
    `sess->mem_pauses++` 并直接返回；载体的 TCP 接收窗口随后关闭。
  - `tbt_tx_submit()`（`tbt_tx.c:1601-1660`）在分配前检查，超了置 `st->tx_paused` 并 break。
  - `tbt_rx_data()`（`tbt_rx.c:131-170`）在**任何计数/`rx_high`/`fbk_kick` 之前**检查，
    超了返回 `-EAGAIN`，记录留在 parser 里；重试时不会重复计数。这是关键：当前 `rx_frames++`
    在 `kmalloc` 之前（`:146-150`），若在分配处返回 `-EAGAIN` 会双计。
    若分配返回 NULL，先判 `tbt_mem_over_budget()`：为真则返回 `-EAGAIN`（记录留在 parser），
    否则维持 `-ENOMEM`（真 OOM，载体停止），不要把预算当成协议错误。
  - 预算检查是 `atomic64_read` 的软上限，允许竞态超出几帧；超过后不再分配新帧。
- 释放点：`tbt_rx_free_queue()`（`tbt_rx.c:910-931`）、`tbt_tx_free_queue()`、
  `tbt_tx_free_stream()`、pump 的 `drop:`、GC/trim、`tbt_carrier_xmit` 失败路径，
  全部走统一 free helper。
- 可见性：`stats` 增加 `mem_bytes=`、`mem_pauses=`；`peers` 不加列（16 列契约不变）。

为什么是 128 MiB：两个方向的严格 40 MiB 回显各需 ~40 MiB 载荷；按 2× 是 160 MiB，超过 128 MiB 会暂停。
这是**正确的背压**：暂停后应用读走数据，预算释放，继续。若担心 benches 的 40 MiB 严格用例，
把默认设 192 MiB 并先在 QEMU 里量；但 `pub.writer` 的 200 MB 门槛要求上限不能太大。
建议：先 128 MiB，跑 `benches/run.sh throughput --n 4 --mib 40` 与 adversarial public 两组，再定稿。

#### E. 热路径与锁

- 每帧多一次 `atomic64_add_return`/`sub`（x86 上是 `lock add`/`lock sub`）。以 8 KiB 帧、30 Mbit/s 计
  约 500 帧/s/载体，可忽略；若在意，可改成 per-CPU 计数器求和，但 0.0.1 不必。
- 预算计数器是叶子锁，不在任何 `tx_lock`/`rx_lock`/`pump_lock` 之内取其他锁，无锁序问题。
- 重传修复减少队列长度和 pump 扫描成本，是净收益。
- 专用 cache 的 `kmem_cache_alloc(GFP_ATOMIC)` 与 `kmalloc(GFP_ATOMIC)` 同一上下文约束。

### 1.5 判别方法（先测后改）

阻塞瞬间在另一个 shell：

```sh
grep -E 'txq=|rxq=|retain_bytes=|ooo_bytes=|allocf=|hiwat=|mem_bytes=' /proc/net/tbtun/stats
awk '/MemAvailable|SUnreclaim|Slab:/' /proc/meminfo
grep -E 'nr_free_pages|nr_slab_unreclaimable' /proc/vmstat
cat /proc/net/sockstat
```

判读：

- `txq=` 远大于 65536（例如数千万）→ 1.3.1 的重传超发是主项；修 A 后应 < ~320 KiB。
- `rxq=` 接近 64 MiB（67108864）→ 交付环是基座；这是 6 层背压的正常结果，不是泄漏。
- `retain_bytes=` 接近 accepted（~75 MB）→ 1.3.3/1.3.4 的 retain 上限失效；修 B 后应 ≤ 8 MiB。
- `ooo_bytes=` 持续非零且不回落 → 第 2 节的 review 20 也在发生（重叠帧），先修 2。
- `slabinfo` 里看不到这些对象是正常的：8296/8488 > 8192，走页分配器；用 `MemAvailable` 和
  `/proc/vmstat` 的 `nr_free_pages` 观察。
- 定向实验：用 `adv_probe fakefar` 的“暂停读载体”变体（或 `drop_ppm` + 不读）制造阻塞，
  只让应用写 16 MiB，观察 `txq` 增长率；当前应约每 RTO 增加 8 份副本，修 A 后应停在 1 份/retain。

### 1.6 验收命令

```sh
# 只跑 public 组（含 pub.writer 100 MiB 与 1 GiB）
TBT_ADV_GROUPS=public adversarial/run.sh --no-build

# 期望：两条 pub.writer 都 PASS；detail 里 MemAvailable 仍 > 200000 kB
# 运行中另开 shell：
watch -n1 "grep -E 'txq=|rxq=|retain_bytes=|mem_bytes=' /proc/net/tbtun/stats"

# 回归：严格 40 MiB 与多载体吞吐
benches/run.sh throughput --n 4 --mib 40
make check
```

通过标准：

- `pub.writer` 100 MiB 与 1 GiB 均 PASS；
- 阻塞时 `txq < 1 MiB`、`retain_bytes ≤ 8 MiB`、`mem_bytes ≤ 128 MiB`、`MemAvailable > 200 MB`；
- `benches` 的 n=4 40 MiB `strict=1` 不回归；
- `make check` 全绿。

### 1.7 风险

- 默认 `tx_retain_bytes=8 MiB` 会改变 replay 行为：超过 8 MiB 的已确认数据不再可重放。
  这是有意的内存换重放深度；文档要写明，且 0 仍表示无限。
- 全局预算可能在合法的双向 40 MiB 严格用例上触发暂停。暂停不是失败（应用读后会继续），
  但会让 benches 的完成时间变长；必须用 benches 实测定 128/192 MiB。
- 专用 cache 的 free 路径改造面大（10+ 处），漏一处 `kfree` 会把专用 cache 对象交给错误的释放器 →
  `kfree` 的 cache 检查告警/oops 或 slab 损坏。用单一 `tbt_frame_free()`/`tbt_retain_free()` 并加 `review/lint.sh` 检查
  `tbt_tx.c`/`tbt_rx.c` 里不再出现裸 `kfree(fr)`/`kfree(r)`。
- `tbt_rx_data()` 的 `-EAGAIN` 预算是新控制流；HTTP 的 `http_pending` 与 raw 的 parser 重试都要覆盖测试。
- `atomic64` 预算在 32 位上是锁前缀操作；本仓库目标 x86_64，可接受。

---

## 2. review 20：`seq < rx_next` 的重叠重排帧永不淘汰

### 2.1 根因

`tbt_rx_pull_locked()`（`tbt_rx.c:34-67`）只做一件事：

```c
if (fr->hdr.seq != st->rx_next) break;
rb_erase_cached(...);
st->rx_ooo_bytes -= fr->len;
st->rx_ooo_frames--;
list_add_tail(&fr->list, &st->rx_q);
st->rx_next = fr->hdr.seq + fr->len;
```

而 `tbt_rx_tree_insert_locked()`（`:70-103`）只拒绝**完全相同**的 `seq`，允许任意重叠：
一个节点 `seq=8192,len=8192` 先入树，随后一个 `seq=0,len=12288` 的直接帧把 `rx_next` 推到 12288；
树的左端是 8192，不等于 `rx_next`，`pull` 永远拉不到它。`rx_ooo_bytes` 只在 pull（`:47`）和
`tbt_rx_free_queue()`（`:928`）里减，于是这个节点永久占着 8192 字节，直到
`rx_ooo_bytes > rx_limit` 在 `:208` 触发 `TBT_ST_REORDER`（`:222-224`）。
`tbt_rx_check_hole()` 的空洞预算（`:873-878`）也会在 500 ms 后杀流。

还有第二个入口：`tbt_rx_data()` 的 `if (hdr->seq < st->rx_next)`（`:176`）对**跨接**的新帧
（`seq < rx_next < seq+len`）整帧丢弃；发送端重传同一帧会再次跨接、再次丢弃，
直到空洞预算重置。合法发送端不会产生重叠（chunk 边界固定、重传 len 不变），
所以这是敌意输入/异常对端的硬化，但代价很低。

### 2.2 方案

把 `tbt_rx_pull_locked()` 改成三分支循环，并让入口的跨接帧也走同一裁剪逻辑：

```c
for (;;) {
    node = rb_first_cached(&st->rx_tree);
    if (!node) break;
    fr = rb_entry(node, struct tbt_frame, rb);
    if (fr->hdr.seq > st->rx_next) break;

    if (fr->hdr.seq + fr->len <= st->rx_next) {
        /* 全部已交付：陈旧副本 */
        rb_erase_cached(&fr->rb, &st->rx_tree);
        st->rx_ooo_bytes -= fr->len;
        st->rx_ooo_frames--;
        list_add_tail(&fr->list, &free);   /* 解锁后 kfree，或直接 kfree */
        continue;
    }
    /* 跨接：丢掉已交付的前缀，off 记录裁剪量 */
    u32 dup = st->rx_next - fr->hdr.seq;
    rb_erase_cached(&fr->rb, &st->rx_tree);
    st->rx_ooo_bytes -= fr->len;   /* 整个节点离开树，不只是前缀 */
    st->rx_ooo_frames--;
    fr->off += dup;
    fr->hdr.seq = st->rx_next;
    list_add_tail(&fr->list, &st->rx_q);
    st->rx_queued += fr->len - fr->off;
    st->rx_next = fr->hdr.seq + fr->len - fr->off;
}
```

关键点：`off` 已经是“已交付的前缀”语义（`tbt_drain_work()` 用 `fr->data + fr->off`、
`left = fr->len - fr->off`，`tbt_sock.c:191-270`）。裁剪时**保持 `len` 不变**、把 `off` 增加 `dup`、
把 `hdr.seq` 前移到 `rx_next`；新的流末偏移是 `seq + len - off`。`rx_ooo_bytes` 减去**整帧 `len`**
（节点离开树；`dup` 已交付、`len - dup` 转入交付环），入队时 `rx_queued` 加 `len - off`
（只加尚未交付的部分）。
这样 `rx_queued`、`rx_next`、drain 的 `left` 三者一致。

入口 `tbt_rx_data()` 的 `hdr->seq < st->rx_next` 分支同样处理：

```c
if (hdr->seq < st->rx_next) {
    u32 dup = st->rx_next - hdr->seq;
    if (hdr->seq + len <= st->rx_next) { drop; }      /* 全旧 */
    else { fr->off = dup; hdr->seq = st->rx_next; /* 入 rx_q，rx_queued += len - dup */ }
}
```

入口分支要单独记账：裁剪后 `rx_queued += len - dup`、`rx_next += len - dup`，
不能直接落到现有 `hdr->seq == rx_next` 分支的 `rx_queued += len` / `rx_next = seq + len`
（那会把已交付的 `dup` 再算一遍）。

锁/内存序：树、`rx_next`、`rx_ooo_bytes`、`off` 都在 `st->rx_lock` 下；`tbt_drain_work()` 读 `off`
也拿同一把锁（`tbt_sock.c:191-270`），所以裁剪后发布是安全的。`kfree` 可以在 `spin_lock_bh` 下做
（现有 `tbt_rx_free_queue()` 就是），但更好的是把陈旧帧挂到局部 `free` 链表，解锁后统一释放，
减少持锁时间。

热路径：每个被 pull 的节点多一次 `<=` 比较；正常路径（`seq == rx_next`）走原分支，零额外分配。
裁剪只发生在敌意重叠，不影响合法发送端。

### 2.3 判别方法

用 raw 载体（`far` 组的注入器）发：

1. `DATA seq=8192 len=8192`（先到，进树）；
2. `DATA seq=0 len=12288`（后到，直接交付并把 `rx_next` 推到 12288）；
3. 再发 `DATA seq=12288 len=4096` 补齐尾部；
4. 读 `stats` 的 `ooo_bytes`/`ooo_frames` 与应用收到的字节。

当前：`ooo_bytes` 停在 8192（树节点按整帧 `len` 计入；实际重叠部分是 [8192,12288) 的 4096 字节），
`ooo_frames=1`，永不回落；
反复重叠最终 `reorder_overflow` 增加、流被 `TBT_ST_REORDER` 杀死。
修复后：`ooo_bytes=0`、`ooo_frames=0`、`reorder_overflow` 不变，应用收到完整的 16384 字节。

### 2.4 验收命令

```sh
# 在 adversarial/adv_probe.c 的 far 注入器里加 far.overlap 用例（HELLO 后发上述三条 DATA）
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
# 期望：far.overlap PASS；stats 里 ooo_bytes=0、reorder_overflow=0、holes_unrecoverable=0

make check        # host fuzz 覆盖记录/帧边界；树逻辑在内核，靠上面的 QEMU 用例
```

### 2.5 风险

- `off` 复用为“裁剪前缀”后，`fr->len` 仍是原始长度；任何还在用 `fr->hdr.seq + fr->len` 当流末的地方
  都要改成 `seq + len - off`。当前只有 pull 和 drain 用这个等式，改动面小，但必须全局 grep。
- 裁剪后 `fr->hdr.body_len` 仍是原始值；它不再用于线上，但调试日志会显示旧长度，加注释说明。
- 这是敌意输入路径；正常多载体乱序不会产生重叠，所以性能/行为回归风险低。
- 如果未来实现 PREDICT 的“部分推进”，`off` 的语义要与 token 匹配统一，避免两套裁剪。

---

## 3. review 22：`-EAGAIN` 时 pinned 控制帧被释放，可能丢 FIN

### 3.1 根因

pump 写完一批帧后，`tbt_carrier_xmit()` 返回 `-EAGAIN`（载体 TCP 发送缓冲满）时：

```c
if (fr->pinned) {
    kfree(fr);
    continue;
}
```

见 `tbt_tx.c:1433-1436`；死路径错误分支同样在 `:1452-1455` 释放 pinned 帧。
pinned 帧包括 HELLO、OPEN、FIN、RST、RETIRE、PAD（`tbt_tx_control_pinned()`，`:119-146`）。
后果：

- **FIN**：`tbt_ctrl_work()` 只在 `TCP_CLOSE_WAIT/LAST_ACK/CLOSING` 时调一次
  `tbt_tx_fin()`（`tbt_sock.c:277-296`），`tbt_tx_fin()` 为每条 up 载体排一份（`tbt_tx.c:174-197`）。
  如果所有副本都遇到 `-EAGAIN`，它们全被释放；没有重试路径。接收端永远看不到 FIN，
  只能等 `hole_budget_ms`（默认 500 ms）把流重置成 RST。半关闭语义丢失。
- **RST**：`tbt_tx_rst()` 同样一次性；丢失后只能等对端超时。
- **HELLO**：`tbt_tx_hello()` 只在 connect/join 时调一次；丢失后载体永远不 `up`，
  健康检查只重连 socket，不会重发 HELLO。
- **OPEN**：丢失后该载体永远 `open_sent=false`，调度器不会给它 DATA；`tbt_tx_announce()` 只在
  join/OPEN 事件再调，可能再也不来。
- **RETIRE**：丢失后旧 (index, gen) 不释放，替换载体的 HELLO 收 `EALREADY`，`join_dup` 风暴。

这不是“低概率的 FIN 丢失”，而是**任何一次载体写缓冲满都可能永久废掉一条控制路径**。

### 3.2 方案

**A. `-EAGAIN`：pinned 帧重新入队，不释放。**

```c
if (err == -EAGAIN) {
    if (fr->ret) { /* 释放路径预留，保持原逻辑 */ }
    if (fr->pinned) {
        tbt_tx_enqueue(sess, fr, true);          /* 队头，保持控制帧顺序 */
        if (c->sk && c->sk->sk_socket)
            set_bit(SOCK_NOSPACE, &c->sk->sk_socket->flags);
        mod_delayed_work(system_unbound_wq, &c->w_pump, msecs_to_jiffies(2));
        sess->dbg_pump_hiwat++;
        continue;
    }
    ...
}
```

`tbt_tx_enqueue()` 内部拿 `sess->tx_lock`；此时我们在锁外（batch 已从 `tx_q` 移出），安全。
`fr->pinned/path/gen` 由 pump 独占，重新入队后由 `tx_lock` 发布。
不要每次重排都 `tbt_tx_wake()`（会调度所有载体的 `w_tx`）；arm 该载体的 2 ms `w_pump` 即可，
它已经检查 `sess->tx_queued` 并调 `tbt_tx_pump()`（`tbt_carrier.c:161-175`）。

**B. 死路径错误：FIN/RST 重挂到新 generation。**

pump 的 pinned 分支在 `fr->gen != c->gen` 时 `goto drop`（`tbt_tx.c:1290-1296`）。
对 HELLO/OPEN/RETIRE，丢掉是对的（新载体有自己的握手）。对 FIN/RST，应改为：

```c
if (fr->gen && fr->gen != c->gen) {
    if (type == TBT_F_FIN || type == TBT_F_RST) {
        fr->gen = c->gen;        /* 重挂到同 index 的新载体 */
        /* 留在队列；等新载体 up（OPEN 写出）后由 pinned 分支写出 */
        continue;
    }
    goto drop;
}
```

pinned 分支对非 HELLO/OPEN 的帧要求 `atomic_read(&c->up)`（`:1310-1316`），所以 FIN 不会抢在
新载体的 OPEN 之前发出。新载体 `up` 后，`w_pump`/`w_tx` 会再次扫描队列并写出。

**C. 去重：`tx_fin_sent` / `tx_rst_sent`。**

`tbt_ctrl_work()` 可能因多次状态变化重复调 `tbt_tx_fin()`；`tbt_kill_work()` 与 `tbt_ctrl_work()`
都可能调 `tbt_tx_rst()`。加 `unsigned long tx_fin_sent, tx_rst_sent` 到 `struct tbt_stream`
（`test_and_set_bit` 需要 `unsigned long *`，不能用 `bool`），在 `tbt_tx_fin()`/`tbt_tx_rst()`
入口 `test_and_set_bit`，已发就直接返回。
这既避免重复帧，也给“重挂”一个稳定的语义：重挂的是同一批帧，不是新一批。

锁/内存序：`tx_fin_sent`/`tx_rst_sent` 用 `test_and_set_bit`（原子）；`fr->gen` 在 pump 独占期改，
重新入队时由 `tx_lock` 发布；`c->up` 是 `atomic_t`，已有 acquire/release 语义。

热路径：只影响控制帧，每流几次，可忽略。正常无 `-EAGAIN` 时行为不变。

### 3.3 判别方法

需要一个“载体发送缓冲满时应用半关闭”的用例：

1. 近端 n=1，raw；对端用一个假 far（`adv_probe fakefar` 变体）完成 HELLO，然后**停止读**载体 socket，
   让近端的载体 `sk_wmem_queued` 达到 `sk_sndbuf`。
2. 应用 `shutdown(SHUT_WR)`。
3. 假 far 恢复读；检查是否收到 `TBT_F_FIN`（`rx_fin_seen` / 应用侧 EOF）。

当前：FIN 被释放，假 far 永远收不到 FIN；500 ms 后流被 `TBT_ST_HOLE` 重置（应用看到 RST/ECONNRESET）。
修复后：假 far 收到 FIN，应用侧 `read()` 返回 0（EOF），`stats` 的 `holes_unrecoverable` 不增。
同一用例把 `shutdown` 换成 `close()` 可验证 RST。

### 3.4 验收命令

```sh
# 新增 near.fin_eagain（fake far 暂停读 → 应用 shutdown(SHUT_WR) → 恢复读）
TBT_ADV_GROUPS=near adversarial/run.sh --no-build
# 期望：near.fin_eagain PASS；detail 里 fin_seen=1、eof=1、reset=0

# 半关闭的总回归（F5 修好后）
TBT_ADV_GROUPS=public adversarial/run.sh --no-build   # pub.halfclose
```

### 3.5 风险

- 重新入队可能让一个永远满的载体在队头反复重试；每次 pump 只重试一次（4 个 pass 上限），
  且 `w_pump` 是 2 ms 一次，不会忙等。载体若真死，健康检查会在 `carrier_stall_ms` 后替换它。
- 重挂 FIN 到新 generation 依赖“新载体 up 后 pump 会再扫队列”；`tbt_tx_wake()` 在 HELLO/OPEN
  写成功后会调（`tbt_tx_pump()` 末尾的 `tbt_tx_wake` 路径），需要确认；否则在 `tbt_car_poll_work`
  的 50 ms 里补一次 `schedule_work(&c->w_tx)`。
- 重复 FIN/RST 本身对接收端幂等（`rx_fin_seen`/`tbt_stream_kill`），但会占队列；`tx_fin_sent`
  去重是必要的。
- 如果 `-EAGAIN` 的原因是 `drop_ppm` 注入器（它返回 0，不是 `-EAGAIN`），本修复不改变注入语义。

---

## 4. review 21：`rx_frames`/`data_frames` 等普通自增在多载体并发下丢更新

### 4.1 根因（先纠正评审的表述）

评审把 `c->rx_frames++`、`c->data_frames++` 也算作“多载体并发丢更新”。按当前代码：

- `c->rx_frames`/`c->rx_bytes` 只在**该载体的 `w_rx`** 里写（`tbt_rx.c:146-147`），
  一个 `work_struct` 不会自我并发 → **单写者，不会丢更新**；问题只是读者（`tbt_tx_fbk()`、
  `tbt_peers_show()`）可能看到撕裂的 64 位值（32 位平台）。
- `c->data_frames` 只在 `tbt_carrier_xmit()` 里写（`tbt_carrier.c:521`），而 `xmit` 由
  `pump_lock` 串行 → **单写者**。真正的 bug 是它在写之前自增（review 14），把 `-EAGAIN` 计成已发。
- 真正会丢更新的是**会话级多写者计数**：`sess->rx_data_frames++`、`sess->fbk_frames++`
  （`tbt_rx.c:150-152`，每条载体的 `w_rx` 都写）、`sess->rx_records++`（`:714`）、
  `sess->fbk_rx++`（`:285`）、`sess->rx_parse_proto++`、`sess->predict_used/cancelled`、
  `sess->dup_used`、`sess->stats_gap`、`sess->rx_reorder_overflow`、`sess->carrier_lost/replaced/
  alloc_fail/connect_fail`、`sess->rx_ready_calls`、`sess->tx_wake_calls` 等。
- 更严重的是**单调高水位的 check-then-write**：
  - `tbt_tx_ack()`：`if (uack <= READ_ONCE(sess->tx_ack)) return; WRITE_ONCE(sess->tx_ack, uack);`
    （`tbt_tx.c:1091-1096`）。两条载体的 `w_rx` 可交错：A 读 100，B 读 100，B 写 200，A 写 150 →
    `tx_ack` 回退到 150。这会破坏保留环 GC 和发送窗口。
  - `tbt_rx_fbk()`：`if (uack > READ_ONCE(pc->uack)) WRITE_ONCE(pc->uack, uack);`
    （`tbt_rx.c:262`）同样的回退；`recv_cnt` 也是 `WRITE_ONCE`（`:272`），旧 FBK 可覆盖新值。
  - `sess->join_retries` 是 `u8` 的 `++`（`tbt_carrier.c:782-784`），多载体并发丢更新，延迟 fail-fast。
  - `c->lost_counted` 的 `if (!c->lost_counted) { c->lost_counted = true; ... }` 不是原子的，
    两个上下文可同时计数。

所以第 4 项的方案要分两类：**计数**用分片求和；**单调高水位**用 max-CAS。不能只加原子自增。

### 4.2 方案：per-carrier 分片 + 读取点求和（选定）

选择 per-carrier 分片，而不是把所有 `u64` 换成 `atomic64_t`，理由：

1. 数据路径天然按载体分片：每条载体一个 `w_rx`，pump 由 `pump_lock` 串行。分片后每个计数器
   只有一个写者，`u64_stats_t` 的 seqcount 在 64 位上是空操作，**每帧零原子 RMW**。
2. 全 `atomic64_t` 会让 16 条载体的 `w_rx` 在同一缓存行上做 `lock inc`，正好打爆产品要卖的多路径吞吐。
3. `/proc` 不是热路径；读取时遍历 16 个分片求和，成本可忽略，且天然给出“每路径贡献”。

具体：

- 在 `struct tbt_carrier` 内加一个 `struct tbt_car_stats`：
  ```c
  struct tbt_car_stats {
      struct u64_stats_sync sync;   /* 单写者：该载体的 w_rx 或 pump */
      u64_stats_t rx_frames, rx_bytes, rx_records, fbk_rx, parse_proto;
      u64_stats_t tx_frames, tx_bytes, tx_data_bytes, lost_frames, dropped;
  };
  ```
  写入用 `u64_stats_update_begin(&c->stats.sync)` … `u64_stats_update_end(...)`；
  读取用 `u64_stats_read(&c->stats.rx_frames)`（内部处理 32 位撕裂）。
- 删除会话级的 `rx_data_frames`、`rx_records`、`fbk_rx`、`carrier_bytes`、`stats_tx` 等热计数，
  在 `tbt_show_stats()`/`tbt_tx_fbk()` 里遍历 `s->car[i]` 求和。`stats_tx` 用新的
  `c->tx_data_bytes`（只计 DATA 载荷）求和；`carrier_bytes` 用 `c->tx_bytes` 求和。
- 低频会话事件（`holes_declared/repaired/unrecoverable`、`carrier_lost/replaced/alloc_fail/
  connect_fail`、`session_join_*`、`profile_mismatch`、`http_*`、`dup_*`）用 `atomic64_t`；
  它们每 FBK/每事件一次，不在每帧路径上。
- 单调值：
  - `sess->tx_ack` → `atomic64_t` + `atomic64_try_cmpxchg` 的 max 循环；只有成功推进时更新
    `last_ack_ns` 并调 `tbt_tx_holes_repaired()`。
  - `pc->uack` → `atomic64_t` + max-CAS；`pc->recv_cnt` → `atomic_t` + max-CAS。
  - `sess->join_retries` → `atomic_t` + `atomic_inc_return`；`c->lost_counted` 改成
    `unsigned long`，用 `test_and_set_bit(0, &c->lost_counted)`。
  - `sess->rx_max_stall_us` 是 u32 max，两个上下文（w_fbk 的 `tbt_rx_check_hole()` 与 w_rx 的
    `tbt_rx_flush()` → `tbt_rx_check_hole()`），用 `cmpxchg` max 循环或 `atomic_t` max。
- `sess->fbk_frames` 的“每 32 帧立即 FBK”语义：改成 `atomic_inc_return(&sess->fbk_frames) >= 32`
  后 `atomic_xchg(&sess->fbk_frames, 0)`；允许竞态多触发一次 FBK，无害。
- `sess->peer_paused`/`pause_cleared_ns`：每个 FBK 覆盖所有路径，最后写入者赢；竞态窗口小。
  若要严格，给 FBK 加一个 `fbk_gen`，只应用更新的 FBK；0.0.1 可记为已知限制。

读取点改动：

- `tbt_peers_show()`（`tbt_session.c:382-394`）：`tx_frames`/`tx_bytes`/`recv_cnt` 等改用
  `u64_stats_read()`/`atomic64_read()`；列数不变（16 列契约）。
- `tbt_show_stats()`（`tbt_procfs.c:461-600`）：`rx_records`、`rx_data_frames`、`carrier_bytes`、
  `stats_tx` 等改成求和；键名保持不变，避免脚本漂移；新增 `mem_bytes`、`mem_pauses`（第 1 节）。
- `tbt_tx_fbk()`（`tbt_tx.c:1747-1810`）：读 `c->rx_frames` 用 `u64_stats_read`，读 `rx_high` 用
  `READ_ONCE`（单写者）。

内存序：`u64_stats_update_begin/end` 提供 seqcount；写者必须单写者（分片保证）；读者
`u64_stats_read` 自动重试。max-CAS 用 `atomic64_try_cmpxchg` 的循环，读到旧值就重试；
普通诊断值用 `READ_ONCE`/`WRITE_ONCE`。

热路径：每帧只在分片里做一次 seqcount（64 位为空）+ 普通加；没有 `lock` 前缀。
每 FBK 才做 2 次 CAS（tx_ack/uack/recv_cnt）。比现状多的是读取点的求和，可忽略。

### 4.3 判别方法

- 并发压力：n=4/8，40 MiB 双向，结束后比较：
  - `sum(c->rx_frames)` 与独立 DATA 帧计数（迁移前是 `sess->rx_data_frames`，迁移后由注入器/探针
    另行计数）相等；`sum(c->rx_records)` 与总记录数相等（`rx_records` 含 HELLO/FBK，不能和
    `rx_frames` 直接比）；
  - `sum(c->tx_bytes) == sess->carrier_bytes`；
  - `tx_ack` 在一次运行中单调不减（用 `stats` 采样或 ftrace/kprobe；静态分析已能证明回退路径）；
  - `recv_cnt` 在连续 `peers` 采样中不下降。
- 代码级：`grep -n "sess->.*++" tbtun/tbt_rx.c tbtun/tbt_carrier.c` 列出所有多写者；
  `grep -n "WRITE_ONCE(sess->tx_ack\|WRITE_ONCE(pc->uack"` 确认改成 CAS。
- KCSAN（若内核配置允许）跑 n=4 压力，当前会对这些字段报 data race；修复后应静默。

### 4.4 验收命令

```sh
make check                     # host 测试（stats 前缀不变）
bash review/lint.sh            # 新增 check_monotone_ack（见下）

# QEMU：n=4，双向 40 MiB，结束后采样
tests/qemu/run.sh --probes C,E --timeout 300
adversarial/run.sh --no-build   # 全量回归
# 期望：stats 的 tunnel 聚合 == peers 各行之和；tx_ack/recv_cnt 单调
```

`review/lint.sh` 建议新增一条机械检查：`tbt_tx_ack()` 中不得出现
`if (uack <= READ_ONCE(...)) { ... } WRITE_ONCE(sess->tx_ack, uack);` 的 check-then-write，
必须出现 `atomic64_try_cmpxchg` 或 `cmpxchg`。

### 4.5 风险

- 改字段类型会触及 `tbt_procfs.c` 的渲染和 `tbtun.h` 的结构布局；`tbt_session` 已经很大，
  把热计数移到 carrier 分片反而减小会话结构。
- `u64_stats_t` 要求写者唯一；如果未来一条载体有多个 `w_rx`（例如多队列），分片假设失效。
  在结构注释里写死“single writer = w_rx / pump”。
- `tx_ack` 的 CAS 循环在极端竞争下可能重试几次；每 FBK 一次，可接受。
- `fbk_frames` 的原子化会让“每 32 帧”略有抖动；FBK 是 10 ms 的软实时，不影响正确性。
- 32 位平台上 `u64_stats_read` 有重试；本仓库目标 x86_64。

---

## 5. 端口耗尽：`init.port_exhaust` 的 `alloc_fail=16` 与无限重试

### 5.1 根因

用例（`adversarial/init:250-262`）：

```sh
echo 'ports 40000-40015' > /proc/net/tbtun/ports       # 16 个端口
echo "tunnel 1 role=far ... n=1" > config
echo "tunnel 2 role=near ... n=16" > config            # 占满 16 个
echo "tunnel 3 role=near ... n=16" > config            # 没有端口了
sleep 2
_fail=$(awk '$1=="tunnel" && $2==3 { ... alloc_fail= }' /proc/net/tbtun/stats)
if [ -n "$_fail" ] && [ "$_fail" -gt 0 ]; then FAIL ...
```

也就是说，**用例要求隧道 3 在配置时被拒绝**（`alloc_fail` 为空/0），而不是“接受后报错”。
当前代码：

- `tbt_carriers_start()`（`tbt_carrier.c:875-895`）只 `tbt_carrier_new()` + `schedule_work(&c->w_connect)`，
  不分配端口，所以 `tbt_tunnel_add()` 成功，隧道 3 出现在 `tunnels` 里。
- `tbt_car_connect_work()`（`:1099-1110`）在 `c->local_port == 0` 时调 `tbt_port_alloc()`；
  返回 0 → `sess->carrier_alloc_fail++`、`retry_at_ns = now + 200 ms`、`schedule_delayed_work(&c->w_poll, 50 ms)`
  → 每 250 ms 重试一次，**永远重试**，没有日志、没有状态变化。
- `tbt_replace_work()`（`:945-958`）在替换时先分配新端口（旧载体还占着旧端口），失败则
  `sess->car[i] = NULL; carrier_alloc_fail++`，并且**没有重试**：`NULL` 槽没有 `w_poll`，
  这个 index 永久消失，`peers` 里连行都没有。
- `tbt_tunnel_show()`（`tbt_session.c:50-56,273-290`）的 `state` 只有 `listening`/`idle`；
  `tbt_peers_show()` 跳过 `!c` 的槽。操作员看到的是“少了一行”，不是错误。
- 相关 bug：`tbt_portmap_set()`（`tbt_session.c:60-78`）直接 `kvfree(tbt_ports.used)` 换新位图，
  不把现有载体的 `local_port` 标回新位图；之后 `tbt_port_alloc()` 可能把已占用的端口再发一次，
  `bind` 失败 → 又是无限重试。`tbt_port_free()` 只清当前范围内的位，范围外的端口静默泄漏。

### 5.2 方案

**A. 创建时准入（all-or-nothing）。**

- 新增 `int tbt_port_alloc_n(u16 n, u16 *out)`：在 `tbt_ports.lock` 下扫描位图，找到 n 个零位就
  全部置位；不够就一个都不置，返回 `-ENOSPC`。端口按升序返回。
- `tbt_carriers_start()`（近端）先 `tbt_port_alloc_n(sess->n_car, ports)`；失败时把 `-ENOSPC`
  映射为 `-EADDRNOTAVAIL` 返回（比 `-ENOSPC` 更贴近 bind 语义，操作员熟悉）。成功则把 `ports[i]` 直接赋给
  `c->local_port`，再 `schedule_work(&c->w_connect)`；`w_connect` 看到 `local_port` 已设置就不重分配。
- 任一步失败（`tbt_carrier_new` 返回 NULL）要把已预留的端口全部 `tbt_port_free()` 回滚。
- `tbt_tunnel_add()`（`tbt_session.c:167-235`）已经在 `tbt_session_create()` 失败时
  `tbt_listen_stop()` + `tbt_session_destroy()` + `tbt_tunnel_put()`，并把 `err` 返回给
  `tbt_config_apply()` → `write()`。所以隧道 3 不会出现在 `tunnels`，`alloc_fail` 也不会为 3 生成。
  这正是 `init.port_exhaust` 的 PASS 条件。
- 端口在隧道生命周期内保留（即使某条载体暂时没连上）；这是有意的：配置写入必须确定，
  端口占用也必须可解释。`tbt_carrier_release()` 释放端口时，替换路径按 B 的顺序拿回或换新。

**B. 替换失败要有界重试，不能孤儿化槽位。**

- 新增 `struct delayed_work w_replace_retry`（`tbt_session`），`tbt_session_create()` 初始化，
  `tbt_session_destroy()` 在两处 `cancel_work_sync(&s->w_replace)` 旁边一起 cancel。
- `tbt_replace_work()` 分配失败时：`sess->car[i] = NULL; sess->carrier_alloc_fail++;
  sess->port_retry++; queue_delayed_work(system_unbound_wq, &sess->w_replace_retry, 200ms)`。
  handler 直接调 `tbt_replace_work()`；每次失败把退避翻倍（200 ms → 400 → … → 2 s）。
  成功后清零。
- 端口块被 `ports` 写入扩大时，遍历所有隧道 kick 一次 `w_replace_retry`（在 `tbt_lock` 下收集
  需要 kick 的 session，锁外 schedule），让降级的槽位尽快恢复。
- 替换时的端口顺序：当前“先分配新端口、再释放旧载体”。在恰好 n 个端口的块里，替换必然需要
  n+1 个。建议：
  1. 先尝试分配一个**不同**的端口（保持现在的 TIME_WAIT 规避）；
  2. 若失败，`tbt_carrier_retire()` 释放旧端口后，再 `tbt_port_alloc()`；拿到旧端口也可以
     （SO_REUSEADDR 已开，`tbt_carrier.c:1117`）；
  3. 仍失败则走 B 的退避重试。
  这样不会把“块刚好 n 个”变成永久不可替换。

**C. 暴露（不改变线上协议）。**

- `tunnels` 的 `state` 列（9 列契约不变，`tbt_uapi.h:194`）增加 `degraded`：
  **仅近端** `t->cfg.role == TBT_ROLE_NEAR && t->sess` 且当前存在
  `car[i] == NULL` 或 `car[i]->stopping && !up` 的槽 → `degraded`；far 侧的空槽是
  “等待对端加入”，不能算降级。不要用累计 `carrier_alloc_fail` 判定，否则恢复后仍显示降级。
  否则保持 `listening`/`idle`。`TBT_TUNNELS_COLUMNS` 不变，host test 的 9 列检查不受影响。
- `stats` 的 `tunnel <id> c3 ...` 行增加：
  `alloc_fail=`（已有）、`port_retry=`、`degraded=<0|1>`、`ports_free=`（全局空闲数）、
  `ports_total=`。都是 per-tunnel 行，不在 `tbt_render_stats_prefix()` 的钉死前缀里。
- `/proc/net/tbtun/ports` 的只读视图增加 `# free <n> of <total>`；`ports` 写入语法不变。
  注意 `tbt_parse_ports()`（`tbt_parse.c:794-823`）目前不跳过 `#`，所以 `cat ports > ports`
  本来就不可回放；如果要求回放，顺手让 `tbt_parse_ports()` 对 `#` 返回 0（review 25 的 0-0
  问题一起处理）。
- 日志：每个 session 第一次耗尽时 `pr_warn_ratelimited("tbtun: tunnel %llu port block exhausted: "
  "need %u free %u; carriers will retry", ...)`，用一个 `bool port_warned` 只报一次；
  成功分配后清零。不要每 250 ms 打一条。

**D. `ports` 改范围时的位图重建。**

- 在 `tbt_portmap_set()` 里，先收集当前所有 session 的 `car[i]->local_port`（在 `tbt_lock` 下），
  再在 `tbt_ports.lock` 下分配新位图并把仍在范围内的端口置位；范围外的端口无法保留，
  记录 `port_orphan++` 并让相关载体走替换（或直接拒绝改范围，返回 `-EBUSY`）。
- 锁序：`tbt_lock` → `tbt_ports.lock` 是现有路径（`tbt_tunnel_add` 持 `tbt_lock` 调
  `tbt_port_alloc`）；不要在新代码里反向获取。收集和设置分两步，避免嵌套。

### 5.3 判别方法

```sh
echo 'ports 40000-40015' > /proc/net/tbtun/ports
echo "tunnel 1 role=far  listen=127.0.0.1:19101 target=127.0.0.1:19070 n=1  m=0" > /proc/net/tbtun/config
echo "tunnel 2 role=near listen=127.0.0.1:19100 peer=127.0.0.1:19101   n=16 m=0" > /proc/net/tbtun/config
echo "tunnel 3 role=near listen=127.0.0.1:19103 peer=127.0.0.1:19101   n=16 m=0" > /proc/net/tbtun/config
echo "tunnel3 write rc=$?"
cat /proc/net/tbtun/tunnels        # 只有 1、2 两行
cat /proc/net/tbtun/ports          # free 0 of 16
grep 'tunnel 3' /proc/net/tbtun/stats || echo "no tunnel 3 (correct)"
dmesg | tail                        # 一条 port block exhausted 警告
```

当前：`tunnel 3` 写入成功、`tunnels` 三行、`stats` 里 `alloc_fail=16`、`peers` 里 16 行 `connecting`。
修复后：`write` 返回 `-EADDRNOTAVAIL`（shell 里 `echo` 报错）、只有两行、`ports` free=0、一条警告。

### 5.4 验收命令

```sh
TBT_ADV_GROUPS=port adversarial/run.sh --no-build
# 期望：init.port_exhaust PASS（detail 里没有 alloc_fail，或 alloc_fail 为空）

# 替换路径：占满 16 个端口后，把 tunnel 2 的 carrier 0 down 掉，
# 观察 w_replace_retry 是否在端口释放后恢复（或 degraded 可见）
echo 'carrier 0 down' > /proc/net/tbtun/peers
sleep 1; cat /proc/net/tbtun/tunnels; grep 'tunnel 2' /proc/net/tbtun/stats
```

### 5.5 风险

- 准入会在端口暂时耗尽时拒绝一个“等一会儿就能起来”的隧道。这是有意的：配置写入必须给出
  确定的成功/失败，而不是接受一个永远 connecting 的对象。文档要写清“端口块大小 ≥ n + 替换余量”。
- 替换需要 n+1 个端口的瞬态；如果块刚好 n 个，B 的回退路径（释放旧端口再分配）必须可靠，
  否则降级会持续到有端口释放。
- `ports` 改范围的位图重建涉及全局状态；如果 0.0.1 不想做，至少要在改范围时拒绝（`-EBUSY`），
  不能继续静默丢位。
- `tunnels` 的 `state` 值变化可能影响只认 `listening` 的脚本；README/运维手册要更新。

---

## 6. review 23：`rx_reorder_bytes=0` / `hole_budget_ms=0` 被接受

### 6.1 根因

解析器（`tbt_parse.c:541-559`）：

```c
if (tbt_parse_u32(val, &cfg->hole_budget_ms) || cfg->hole_budget_ms > 60000u)
    return tbt_fail(e, key, "hole_budget_ms must be 0..60000");
...
if (tbt_parse_u32(val, &cfg->rx_reorder_bytes) || cfg->rx_reorder_bytes > TBT_REORDER_MAX)
    return tbt_fail(e, key, "rx_reorder_bytes must be 0..268435456 bytes");
...
if (tbt_parse_u32(val, &cfg->carrier_stall_ms) || cfg->carrier_stall_ms > TBT_STALL_MAX)
    return tbt_fail(e, key, "carrier_stall_ms must be 0..3600000 ms");
```

`tbt_cfg_sane()`（`tbt_session.c:106-140`）只检查 id/role/n/listen/peer/target，不检查这三个。

后果：

- `rx_reorder_bytes=0` → `st->rx_limit = 0`（`tbt_session.c:580`）。任何 `seq > rx_next` 的帧
  （多载体下必然出现）入树后 `rx_ooo_bytes > 0`，`tbt_rx_data()` 立即
  `tbt_stream_kill(st, TBT_ST_REORDER)`（`tbt_rx.c:208-224`）。n≥2 的隧道第一条乱序帧就死。
- `hole_budget_ms=0` → `tbt_rx_check_hole()` 的 `now - hole_since_ns > 0` 立即成立
  （`tbt_rx.c:876-891`），任何空洞立即 `TBT_ST_HOLE`；`tbt_tx_stall_check()` 的
  `now - start > 0`（`tbt_tx.c:1018-1076`，判断在 `:1061`）也会在下一个 10 ms tick 杀掉有未确认数据的流。
  单载体丢一个 TCP 段就足以触发。
- `carrier_stall_ms=0`（评审没点名，但同一类）→ `tbt_car_poll_work()` 的
  `now - last_una_ns > 0`（`tbt_carrier.c:275-284`）在第一次 50 ms 轮询、只要 `una != nxt`
  就判定载体失效 → 替换风暴。这个比前两个更隐蔽，必须一起拒绝。
- `keepalive_ms=0`/`drip_ms=0` 是**有定义的**：`http_elapsed()` 在 `ms==0` 时返回 false
  （`tbt_http.c:1250-1254`），即“关闭 keepalive/drip”。保留 0。
- `tx_retain_bytes=0`/`tunnel_rate=0`/`replay=0` 也是有定义的（无限/无字节上限），保留 0。

### 6.2 方案：拒绝，不取默认

现有语法约定是：**0 只在有明确语义时被接受**（`tx_retain_bytes` 的注释就写 “0 = unlimited”；
`tunnel_rate` 0 = 不限速；`replay` 0 = 不按字节限制；`drip_ms`/`keepalive_ms` 0 = 关闭）。
`rx_reorder_bytes`/`hole_budget_ms`/`carrier_stall_ms` 的 0 没有定义，且会让隧道失效。
因此：

- `tbt_parse.c`（0.0.1 的最小改动是“只拒绝 0”）：
  - `hole_budget_ms`：`1..60000`，错误串改为 `"hole_budget_ms must be 1..60000"`。
  - `rx_reorder_bytes`：`1..268435456`，错误串 `"rx_reorder_bytes must be 1..268435456 bytes"`。
  - `carrier_stall_ms`：`1..3600000`，错误串 `"carrier_stall_ms must be 1..3600000 ms"`。
  - 可选加固（单独提交）：`rx_reorder_bytes` 至少 `2 * TBT_PAYLOAD_MAX`（16 KiB），
    `carrier_stall_ms` 至少 100（大于两个 50 ms 轮询）。这两个下界会让
    `host/test_parse.c:982` 的 `rx_reorder_bytes=4096` 用例失效，必须同步改成 16384；
    0.0.1 可以先不做，避免一次改太多。
- `tbt_cfg_sane()` 镜像这三个下界（语义闸门），错误 `-EINVAL`。这样即使未来有 netlink/模块参数
  绕过解析器，也不会构造出失效隧道。
- 不取默认的理由：配置视图 `tbt_render_cfg()` 会把存储值原样渲染；如果解析时把 0 悄悄换成默认，
  `echo '... rx_reorder_bytes=0'` 会成功，但 `cat config` 显示 262144，违反
  “canonical render 解析回相同 struct”的契约，也掩盖操作员的错误。拒绝更符合现有语法。
- 更新 `host/test_parse.c:976-981`：它当前把 `rx_reorder_bytes=0` 当作“C3 keys at their bounds”
  接受。改成 `rx_reorder_bytes=1` 作为新的下界（若采纳 16 KiB 加固则改 16384），并新增三条 `refuse()`：
  `rx_reorder_bytes=0`、`hole_budget_ms=0`、`carrier_stall_ms=0`。
- 在 `adversarial/init` 的 `phase_param()` 里加 `param.zero`：写三条含 0 的隧道行，期望 `write`
  失败且 `tunnels` 不变；再写一条 `tx_retain_bytes=0`，期望成功（证明 0 的“无限”语义没被误伤）。

### 6.3 判别方法

```sh
# 当前：write 成功，隧道出现，随后第一次乱序/空洞就 reset
echo 'tunnel 9 role=near profile=raw listen=127.0.0.1:19109 peer=127.0.0.1:19101 n=2 m=0 rx_reorder_bytes=0' \
    > /proc/net/tbtun/config; echo rc=$?
cat /proc/net/tbtun/tunnels | grep '^9 '
grep 'tunnel 9' /proc/net/tbtun/stats   # reorder_overflow / holes_unrecoverable 增长

# 修复后：write 返回 -1 EINVAL，tunnels 无 9，dmesg 的 config rejected 带 key
```

### 6.4 验收命令

```sh
make check                       # test_parse 的新 refuse 用例
TBT_ADV_GROUPS=param adversarial/run.sh --no-build
# 期望：param.zero PASS；init.max_carriers 仍按原样

# 手动：
echo 'tunnel 9 role=near listen=127.0.0.1:19109 peer=127.0.0.1:19101 n=2 m=0 hole_budget_ms=0' \
    > /proc/net/tbtun/config; echo rc=$?   # 期望非 0（EINVAL）
echo 'tunnel 9 role=near listen=127.0.0.1:19109 peer=127.0.0.1:19101 n=2 m=0 tx_retain_bytes=0' \
    > /proc/net/tbtun/config; echo rc=$?   # 期望 0（0 = unlimited）
```

### 6.5 风险

- 现有配置文件若写了 0，会被拒绝；这是有意的破坏性收紧，需要在 changelog/README 里写清。
  默认渲染从不输出 0 给这三个键，所以 `cat config > config` 不受影响。
- 0.0.1 先按“拒绝 0”落地（范围 1..max），不引入 16 KiB/100 ms 的新下界；后者会让
  `host/test_parse.c:982` 的 `rx_reorder_bytes=4096` 和现有实验配置失效，作为后续加固单独提交。
- 如果采纳 16 KiB/100 ms 下界，必须同步更新 `host/test_parse.c:976-982` 与
  `tests/qemu/README.md` 里的 `rx_reorder_bytes=2097152`（不受影响，但要在文档里写明新下界）。
- `tbt_cfg_sane()` 的镜像检查是第二道门；保持与解析器错误串一致，避免两套说法。

---

## 7. 横切：发布门槛、测试矩阵与实施顺序

### 7.1 预算总表（0.0.1 建议值）

| 项 | 值 | 位置 |
|---|---|---|
| `TBT_SQ_MAX` | 64 KiB | `tbtun.h:116` |
| 重传超发 | `2 * TBT_PAYLOAD_MAX * n_car`（≤256 KiB @16） | `tbt_tx.c:548` 改 |
| `tx_retain_bytes` 默认 | 8 MiB（0 = 显式无限） | `tbt_parse.c:429` |
| `replay` | 8 MiB / 1 s | `tbt_parse.c:424` |
| `rx_reorder_bytes` | 256 KiB（0.0.1 最小 1；16 KiB 为可选加固） | `tbt_parse.c:428` |
| `TBT_RX_RING_MAX` | 64 MiB/流（保持，计入全局） | `tbtun.h:127` |
| 全局 `mem_max_mb` | 128 MiB（module_param） | 新增 |
| `hole_budget_ms` | 500（最小 1） | `tbt_parse.c:427` |
| `carrier_stall_ms` | 3000（0.0.1 最小 1；100 为可选加固） | `tbt_parse.c:430` |
| 端口块 | ≥ `n + 1`（替换余量） | 运维文档 |

### 7.2 实施顺序（按风险/依赖）

1. **第 6 项（零值拒绝）**：最小、纯解析，先把“配置即失效”关掉。
2. **第 1 项的 A/B（重传上限 + retain 硬上限）**：直接消除 `pub.writer` 的主内存项，改动集中。
3. **第 1 项的 D（全局预算）**：兜底；先量 128 MiB 对严格 40 MiB 的影响。
4. **第 1 项的 C（专用 cache）**：可选但推荐；如果 2+3 已让 MemAvailable > 200 MB，可放到 0.0.2。
5. **第 5 项（端口准入）**：改创建路径，影响配置语义，单独提交 + adversarial port。
6. **第 2/3 项（重叠帧、pinned 控制帧）**：独立的小修，各自加 QEMU 用例。
7. **第 4 项（计数分片 + max-CAS）**：面最大，放最后；先修 `tx_ack`/`uack` 的 CAS（正确性），
   再迁移统计分片（可观测性）。

### 7.3 测试矩阵

| 用例 | 覆盖 | 期望 |
|---|---|---|
| `TBT_ADV_GROUPS=public adversarial/run.sh` | pub.writer 100 MiB/1 GiB、halfclose、rst | writer PASS、MemAvailable > 200 MB |
| `TBT_ADV_GROUPS=port adversarial/run.sh` | 端口耗尽准入 | `init.port_exhaust PASS` |
| `TBT_ADV_GROUPS=far adversarial/run.sh` | 重叠帧、恶意容器 | 新增 `far.overlap PASS` |
| `TBT_ADV_GROUPS=near adversarial/run.sh` | 假 far、FIN 背压 | 新增 `near.fin_eagain PASS` |
| `TBT_ADV_GROUPS=param adversarial/run.sh` | 零值拒绝、max_carriers | 新增 `param.zero PASS` |
| `make check` | 解析/渲染/fuzz/http | 全绿 |
| `bash review/lint.sh` | 机械不变量 | 15/15 + 自测；新增 monotone-ack、无裸 kfree |
| `benches/run.sh throughput --n 4 --mib 40` | 严格 40 MiB、吞吐 | `strict=1`，吞吐不降 |

### 7.4 行号漂移与并行改动

当前工作树有多个工程师在改（`adversarial/results.txt` 已被重写）。本文的行号是 2026-11-05
的快照；落地前请用下面的命令重新定位，不要直接按行号打补丁：

```sh
grep -n "TBT_SQ_MAX + TBT_RX_RING_MAX" tbtun/tbt_tx.c
grep -n "if (fr->pinned)" tbtun/tbt_tx.c
grep -n "hdr->seq < st->rx_next\|static void tbt_rx_pull_locked" tbtun/tbt_rx.c
grep -n "carrier_alloc_fail\|tbt_port_alloc" tbtun/tbt_carrier.c tbtun/tbt_session.c
grep -n "hole_budget_ms must\|rx_reorder_bytes must\|carrier_stall_ms must" tbtun/tbt_parse.c
grep -n "WRITE_ONCE(sess->tx_ack\|WRITE_ONCE(pc->uack" tbtun/tbt_tx.c tbtun/tbt_rx.c
```

### 7.5 明确不在本文范围

- review 13（PREDICT 惰性）、review 14 的完整修复（本文只在第 1 节顺带要求把
  `data_frames++` 移到写成功之后）、review 16（首个 HELLO nonce 抢占）、F4/F5 的多流/半关闭、
  rmmod 竞态。这些是 0.0.1 的其他工作流，本文不重复设计，只标注与本文方案的交叉点。
- `init.max_tunnels`/`max_carriers` 未生效（review 18）、`config` 32 KiB 截断（review 30）、
  `ports 0-0`（review 25）等，属于配置面，另文处理。
