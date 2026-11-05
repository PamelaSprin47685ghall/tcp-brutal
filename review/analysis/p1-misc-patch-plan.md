# P1 杂项补丁计划：F5、review 14、review 16 + F8

本文是 `review/analysis/p1-misc.md` 的执行版：给出精确改动点、代码片段、
不变量、冲突矩阵与验收命令。**本波不改源码、不构建、不跑 QEMU**；构建与
QEMU 由 perf 工程师独占，`tbt_tx.c`/`tbt_rx.c`/`tbt_sock.c` 正在被他修改
（CRC 诊断与接收证明修复）。

行号取自 2026-11-05 08:50 的工作区快照；perf 的改动会让行号漂移，**按函数
名和锚点行应用**。所有代码片段都是“待应用”的，不是已改。

## 0. 当前状态（先看这张表）

| 项 | 状态 | 证据 |
| --- | --- | --- |
| review 14 | **已落地**（perf 在 `tbt_carrier.c`） | `data_frames++` 在 528（drop_ppm）、611（HTTP 成功）、689（raw 成功）；入口 520-521 已无自增 |
| F5 | 未落地 | `tbt_pub_capture_work`（tbt_sock.c:98-124）不检查 `RCV_SHUTDOWN`；`tbt_ctrl_work`（303-322）仍把 `TCP_CLOSE` 当 reset；`tbt_tx_fin`（tbt_tx.c:203-224）无幂等/掩码 |
| nonce/F8 | 未落地 | `struct tbt_cfg`（tbt_parse.h:45-73）无 `nonce`；`tbt_render_cfg`（tbt_parse.c:784-830）对 far 省略 peer；`tbt_rx_hello`（tbt_rx.c:419-505）只在采纳时看 `cfg.peer_addr`，后续不看源；`tbt_session_join`（tbt_session.c:802-855）只查 index |
| multiconn | 未落地 | `tbt_stream_start`（tbt_sock.c:494-530）仍 `far_sid != 0` 即 `-EBUSY`；`tbt_session_stream` 仍是单流 |
| perf 冻结面 | 进行中 | `tbt_tx.c` 08:34、`tbt_rx.c` 08:18、`tbt_sock.c` 07:37；**不要碰这三个文件** |

---

## 1. F5：半关闭传达 EOF

### 1.1 两个根因与目标

- **(a) near 侧 FIN 早于 attach**：应用 `connect→send→shutdown` 时，FIN 可能
  在 `kernel_accept()`/`tbt_sock_attach()` 之前就被 TCP 栈处理完；
  `tcp_fin()` 调的是默认 `sk_state_change`，`w_ctrl` 从未调度。
  `tbt_pub_capture_work()` 只读数据，不看 `RCV_SHUTDOWN`，于是隧道不发 FIN。
- **(b) far 侧优雅 `TCP_CLOSE` 被当 reset**：near FIN 让 far 对 target 做
  `SHUT_WR`；target 关闭后 `FIN_WAIT2 → tcp_time_wait → tcp_done` 的状态是
  `TCP_CLOSE`、`sk_err == 0`。`tbt_ctrl_work()` 的
  `if (sk_err || state == TCP_CLOSE)` 发 `TBT_F_RST` 并杀流，near 应用拿到
  `ECONNRESET` 而不是 EOF。

目标：**FIN 的唯一发射点是 capture 工作项**，判据是
`(sk_shutdown & RCV_SHUTDOWN) && copied_seq == rcv_nxt && !sk_err`；`w_ctrl`
只负责 reset 判定和唤醒。FIN 的 seq 取 drain 完成后的 `tx_seq`；每方向幂等；
多载体每路径一份；失败可重试。

### 1.2 数据结构（`tbtun.h`，`struct tbt_stream`）

在 TX 段、`tx_start_ns` 之后加：

```c
	/* TX half-close. w_read is the only emitter; w_ctrl and the FBK tick
	 * only set tx_fin_wanted. tx_fin_seq is frozen on first emission and
	 * equals tx_seq at that moment. */
	bool tx_fin_wanted;       /* peer FIN seen; wait for capture drain */
	bool tx_fin_sent;         /* at least one FIN copy queued */
	bool tx_fin_seq_set;      /* tx_fin_seq frozen */
	u16 tx_fin_mask;          /* carriers with a queued FIN copy */
	u64 tx_fin_seq;           /* byte offset after the last DATA */
```

`tx_fin_mask` 的 bit 是载体 index（`n_car <= 16`）。`tx_fin_seq` 与
`tx_fin_seq_set` 在 `sess->tx_lock` 下读写；`tx_fin_mask`/`tx_fin_sent` 允许
`READ_ONCE`/`WRITE_ONCE`，重复拷贝由接收端 `rx_fin_seen` 吸收。

### 1.3 `tbt_pub_capture_work()`（`tbt_sock.c:98-124`）

**改动点**：在 `lock_sock()` 内、`tcp_read_sock()` 之后判断 FIN；在
`release_sock()` 之后发 FIN。

```c
static void tbt_pub_capture_work(struct work_struct *w)
{
	struct tbt_stream *st = container_of(w, struct tbt_stream, w_read);
	struct sock *sk = READ_ONCE(st->sk);
	read_descriptor_t desc = {};
	bool fin = false;

	might_sleep();
	if (!sk)
		return;
	/* ... sk_user_data check unchanged ... */

	desc.arg.data = st;
	st->sess->pub_capture_runs++;
	lock_sock(sk);
	st->sess->pub_last_rcv_nxt = tcp_sk(sk)->rcv_nxt;
	st->sess->pub_last_copied = tcp_sk(sk)->copied_seq;
	st->sess->pub_last_qlen = sk->sk_receive_queue.qlen;
	tcp_read_sock(sk, &desc, tbt_pub_actor);
	st->sess->pub_last_after = tcp_sk(sk)->copied_seq;
	if (!READ_ONCE(sk->sk_err) &&
	    (READ_ONCE(sk->sk_shutdown) & RCV_SHUTDOWN) &&
	    tcp_sk(sk)->copied_seq == tcp_sk(sk)->rcv_nxt &&
	    READ_ONCE(st->state) != TBT_ST_DEAD) {
		WRITE_ONCE(st->tx_fin_wanted, true);
		fin = true;
	}
	release_sock(sk);
	if (fin && READ_ONCE(st->state) != TBT_ST_DEAD &&
	    !READ_ONCE(st->tx_fin_sent)) {
		if (tbt_tx_fin(st->sess, st, GFP_KERNEL) > 0)
			WRITE_ONCE(st->tx_fin_sent, true);
	}
}
```

为什么 `copied_seq == rcv_nxt` 是正确判据：内核 `__tcp_read_sock()` 在遇到
带 `TCPHDR_FIN` 的 skb 时会 `tcp_eat_recv_skb(); ++seq; break;`，而
`rcv_nxt` 已经包含 FIN 的序号；所以 drain 完 FIN 后两者相等。若 capture 因
`TBT_SQ_MAX`/`tx_paused` 只读了一部分，`copied_seq < rcv_nxt`，不发 FIN；
pump 在队列降到 `TBT_SQ_MAX/2` 后会重新调度 `w_read`（`tbt_tx.c` 的
`tx_paused` 恢复段），届时再发。

**不要**在 `w_ctrl` 里发 FIN；它可能早于 capture 运行，seq 会偏小（见 1.1
的 (c) 竞态）。

### 1.4 `tbt_ctrl_work()`（`tbt_sock.c:303-322`）

**改动点**：删掉状态枚举；按 `DEAD → sk_err → RCV_SHUTDOWN → TCP_CLOSE` 的
顺序判定。

```c
static void tbt_ctrl_work(struct work_struct *w)
{
	struct tbt_stream *st = container_of(w, struct tbt_stream, w_ctrl);
	struct sock *sk = READ_ONCE(st->sk);

	might_sleep();
	if (!sk)
		return;
	if (READ_ONCE(st->state) == TBT_ST_DEAD)
		return;
	if (READ_ONCE(sk->sk_err)) {
		tbt_tx_rst(st->sess, st, GFP_KERNEL);
		tbt_stream_kill(st, TBT_ST_DEAD);
		return;
	}
	if (READ_ONCE(sk->sk_shutdown) & RCV_SHUTDOWN) {
		WRITE_ONCE(st->tx_fin_wanted, true);
		schedule_work(&st->w_read);
		return;
	}
	if (READ_ONCE(sk->sk_state) == TCP_CLOSE) {
		tbt_tx_rst(st->sess, st, GFP_KERNEL);
		tbt_stream_kill(st, TBT_ST_DEAD);
		return;
	}
}
```

- `sk_err` 必须在 `RCV_SHUTDOWN` 之前：`tcp_reset()`/`tcp_done()` 会把
  `sk_shutdown` 置成 `SHUTDOWN_MASK`，reset 不能被误报成 EOF。
- `FIN_WAIT1/FIN_WAIT2/TIME_WAIT/CLOSE_WAIT/LAST_ACK/CLOSING` 不再需要枚举；
  唯一判据是 `RCV_SHUTDOWN` + `sk_err`。
- `TBT_ST_DEAD` 早退避免 kill 与 w_ctrl 竞争时再发 FIN/RST。
- 与 `w_read` 的并发是安全的：workqueue 在开始执行时会清 pending 位，运行
  期间再 `schedule_work()` 会排下一次执行；若 `w_read` 只是“已排队未运行”，
  它稍后运行时会看到已经设置的 `RCV_SHUTDOWN`。不会出现“FIN 已到但没有
  任何一次 w_read 看到它”的丢失唤醒。

### 1.5 `tbt_sock_attach()`（`tbt_sock.c:370-401`）

**改动点**：attach 时补调度 `w_read`，并重放“EOF 债”。

```c
	write_lock_bh(&sk->sk_callback_lock);
	/* ... callbacks unchanged ... */
	write_unlock_bh(&sk->sk_callback_lock);

	schedule_work(&st->w_read);   /* FIN/data may predate attach */
	schedule_work(&st->w_drain);  /* frames may predate this socket */
	if (READ_ONCE(st->rx_fin_done))
		tbt_sock_shutdown_wr(st); /* EOF was owed before attach */
```

- near 侧 `tbt_stream_start()` 已经 `schedule_work(&st->w_read)`；这行对
  far 的 `tbt_target_attach()` 是关键（target 可能在 attach 前已发 FIN/数据）。
- `rx_fin_done` 由 `tbt_drain_work()` 在 `rx_lock` 下设置；这里 `READ_ONCE`
  足够，`kernel_sock_shutdown(SHUT_WR)` 对已关闭写方向幂等。
- `tbt_sock_shutdown_wr()` 在 `st->sk == NULL` 时早退；attach 时 `st->sk`
  已由调用者设置（`tbt_stream_start`/`tbt_target_attach`），所以能施加。

### 1.6 `tbt_tx_fin()`（`tbt_tx.c:203-224`）

**改动点**：冻结 seq；每路径一份；返回实际入队的拷贝数；可重试。

```c
int tbt_tx_fin(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp)
{
	u64 seq;
	int i, queued = 0;

	spin_lock(&sess->tx_lock);
	if (!st->tx_fin_seq_set) {
		st->tx_fin_seq = st->tx_seq;
		st->tx_fin_seq_set = true;
	} else if (st->tx_fin_seq != st->tx_seq) {
		/* No submit may follow the FIN decision. */
		WARN_ON_ONCE(1);
	}
	seq = st->tx_fin_seq;
	spin_unlock(&sess->tx_lock);

	for (i = 0; i < sess->n_car; i++) {
		struct tbt_carrier *c = READ_ONCE(sess->car[i]);

		if (!c || !atomic_read(&c->up) || atomic_read(&c->stopping))
			continue;
		if (st->tx_fin_mask & BIT(i))
			continue;
		if (tbt_tx_control_pinned(sess, (u8)i, TBT_F_FIN, st->sid,
					  seq, NULL, 0, false, gfp))
			continue; /* -ENOMEM: leave the bit clear, retry later */
		st->tx_fin_mask |= BIT(i);
		queued++;
	}
	tbt_tx_wake(sess);
	return queued;
}
```

- 返回 0 表示“当前没有可入队的载体”；调用者保持 `tx_fin_sent = false`，
  由 FBK tick 重试。返回 `> 0` 时调用者置 `tx_fin_sent = true`。
- `tx_fin_mask` 的 bit 在入队时置位；若 pinned 帧在 pump 的 `-EAGAIN` 路径
  被释放，必须清位（见 1.8），否则重试不会补发。
- 重复调用是幂等的：已置位的载体跳过；`tx_fin_seq` 不变。
- 多载体并发调用（w_read 与 FBK tick）可能同时看到同一个空 bit 而入队两份；
  接收端 `rx_fin_seen` 只取第一份，重复无害。若要严格去重，可在
  `sess->tx_lock` 下先占位；当前实现不要求。

### 1.7 迟到的载体：`tbt_tx_announce()` 与 FBK tick

**改动点 A**：`tbt_tx_announce()`（`tbt_tx.c:178-198`）在为一个载体排队 OPEN
时，清掉该载体的 FIN 位；循环结束后若 `tx_fin_sent`，补发一份。

```c
	for (i = 0; i < sess->n_car; i++) {
		struct tbt_carrier *c = READ_ONCE(sess->car[i]);

		if (!c || !atomic_read(&c->connected) || !READ_ONCE(c->hello_sent) ||
		    READ_ONCE(c->open_sent) || atomic_read(&c->stopping))
			continue;
		/* A (re)joined carrier has a new generation and no FIN copy. */
		if (READ_ONCE(st->tx_fin_sent))
			st->tx_fin_mask &= ~BIT(i);
		/* ... existing OPEN enqueue ... */
	}
	if (READ_ONCE(st->tx_fin_sent))
		tbt_tx_fin(sess, st, gfp);
	tbt_tx_wake(sess);
```

**改动点 B**：`tbt_fbk_work()`（`tbt_tx.c:1926-1949`）在 `if (st)` 块内、
`tbt_stream_put(st)` 之前重试：

```c
	if (READ_ONCE(st->tx_fin_wanted) && READ_ONCE(st->state) != TBT_ST_DEAD) {
		if (tbt_tx_fin(sess, st, GFP_KERNEL) > 0)
			WRITE_ONCE(st->tx_fin_sent, true);
	}
```

- 每 50 ms 一次；`tbt_tx_fin` 对已置位的载体返回 0，开销是一次 16 项扫描。
- 如果 `tbt_tx_fin` 因 `-ENOMEM` 或“无载体 up”返回 0，下一 tick 再试。
- 如果 pinned FIN 被 pump 释放且清位，下一 tick 会补发到该载体。
- 手工 `carrier <i> up` 复用同一 gen 时，若旧 FIN 拷贝已丢，mask 位仍在，
  tick 不会重发；这条运维路径应在 `up` 时清该位，或依赖替换（新 gen）时
  `tbt_tx_announce` 的补发。常规 EOF 不依赖手工 up。

### 1.8 与 review 22 的依赖（`-EAGAIN` pinned 帧）

当前 pump 在 `-EAGAIN`/`-ENOMEM` 和错误路径对 pinned 帧直接
`kfree(fr)`（`tbt_tx.c:1525-1529`、`1544-1548`）。F5 的 clean-link 验收
不需要它；但“背压下 FIN 不丢”需要它。两个选择：

**选择 B（本波推荐，最小改动，配合 1.7 的重试）**：保留 `kfree`，但在
释放前清位：

```c
	if (fr->pinned) {
		if (type == TBT_F_FIN && st) {
			st->tx_fin_mask &= ~BIT(c->index);
			WRITE_ONCE(st->tx_fin_sent, st->tx_fin_mask != 0);
		}
		kfree(fr);
		continue;
	}
```

同样的清位要放在所有会释放 FIN 的路径：`-EAGAIN`/`-ENOMEM`、错误路径、
以及 `goto drop` 的 `list_del` 前（用 `fr->hdr.type == TBT_F_FIN` 判断，
index 用 `fr->path`）。封装成：

```c
static void tbt_tx_fin_unmark(struct tbt_stream *st, u8 index)
{
	if (!st || index >= TBT_MAX_CARRIERS_LIMIT)
		return;
	st->tx_fin_mask &= ~BIT(index);
	WRITE_ONCE(st->tx_fin_sent, st->tx_fin_mask != 0);
}
```

- 本波如果 review 22 不做：F5 验收限定 clean link；文档明确“背压下 EOF
  仍可能丢”，不要把 `pub.writer` 的 EOF 纳入 F5 完成定义。
- 选择 B 的代价是 FIN 可能延迟一个 FBK tick（≤50 ms）再补发；对 EOF 语义
  可接受。选择 A（pinned FIN 不 `kfree`、重新入队，加 `u8 tries` 上限）适合
  以后把 pinned 控制帧统一改成“不丢、有限重试”，但它要改 `struct tbt_frame`
  和 pump 的释放路径，本波不做。
- 如果 multiconn 已落地，pump 的 `st` 可能不是帧所属的流；此时用
  `fr->hdr.sid_lo` 查流（`tbt_stream_get`）再清位，或让 `tbt_frame` 携带
  所属流的引用。单流阶段用 `st` 即可。

### 1.9 不变量与边界

- FIN 每方向一份；`FIN.seq == tx_seq`（drain 完成时）；所有 `seq < FIN.seq`
  的 DATA 已提交。
- 接收端 `tbt_drain_work()` 的 `list_empty(rx_q) && rx_next >= rx_fin_seq`
  不变；空洞只延迟 EOF，不截断。
- `sk_err != 0` → RST；`RCV_SHUTDOWN && sk_err == 0` → FIN；`TCP_CLOSE` 且
  两者都不成立 → RST。
- 死流（`TBT_ST_DEAD`）不再从 `w_ctrl` 发 FIN/RST。
- EOF 债：`rx_fin_done` 已置但 `st->sk` 尚未 attach 时，attach 必须补
  `SHUT_WR`。
- 重复 FIN 由接收端 `rx_fin_seen` 吸收；发送端 `tx_fin_mask` 只影响带宽。
- 半关闭只作用于写方向；`TBT_F_RST` 才是双向中止。

### 1.10 验收

```sh
# 等 perf 释放构建/QEMU 后
TBT_ADV_GROUPS=public adversarial/run.sh --no-build
# 期望：ADV_RESULT pub.halfclose PASS ... echo 10 bytes, EOF=1
#       pub.rst PASS；console/dmesg 无 WARN/oops
```

guest 内直跑：

```sh
timeout 30 /bin/adv_probe halfclose 19100
```

建议新增（不在本波实现）：target 先发 10 字节再 `shutdown(SHUT_WR)`，应用读到
10+EOF 后继续写 10 字节，target 仍能读到；验证双向半关闭不升级成
`SHUT_RDWR`。

### 1.11 风险

- `sk_err`/`RCV_SHUTDOWN` 顺序写反会把 reset 报成 EOF。
- 在 `tcp_done()` 之后由 w_read 补读数据：`tcp_read_sock()` 只拒绝
  `TCP_LISTEN`，`tcp_done()` 不清接收队列；`__tcp_send_ack()` 在
  `TCP_CLOSE` 直接返回，不会发 ACK。仍需 QEMU 反复跑确认无 WARN。
- capture 被 `tx_paused` 卡住时 FIN 延迟；这是背压，不是丢帧。
- review 22 未做时，单载体 + 背压仍可能丢 FIN；F5 的完成定义要写清。
- multiconn 落地后，`w_read`/`w_ctrl` 的每流状态必须保持；`tbt_tx_announce`
  的 FIN 补发要按流处理。

---

## 2. review 14：已落地，只验收

### 2.1 现状（不要改）

`tbt_carrier.c` 当前：

- 528：drop_ppm 分支，`c->data_frames++`（有意丢弃，必须保留）；
- 611：HTTP chunk 写成功后，`if (fr->hdr.type == TBT_F_DATA) c->data_frames++`；
- 689：raw 写成功后同样；
- 入口 520-521 已无自增；
- `tbt_tx.c:948-949` 的门控仍用 `data_frames - tail`。

### 2.2 验收方式

```sh
# guest 内，干净链路 + 背压
grep -E 'holes_declared|hole_checks|gate_fail|dbg_pump_hiwat|carrier_short' \
    /proc/net/tbtun/stats
# 跑 pub.writer；期望 dbg_pump_hiwat/carrier_short 可以 >0，
# 但 holes_declared 不增长；quiesce 后 peers 的 rx_lag_ppm 为 0。
```

注入丢包回归（证明 drop 分支仍计数）：

```sh
echo 'carrier 0 drop_ppm 100000' > /proc/net/tbtun/peers
# 期望 holes_declared > 0、retx_frames 增长；若为 0，说明 drop 分支的
# data_frames++ 被误删，探针 G 会假通过。
```

### 2.3 不要做的事

- 不要把 `data_frames++` 移回入口；
- 不要在 HTTP header 写成功后计数（必须等 chunk 写成功）；
- 不要改 `recv_cnt`、`tail` 或门控阈值；
- 不要把 drop_ppm 的计数删掉。

---

## 3. review 16 + F8：nonce 配置键与源锚

### 3.1 字段

`struct tbt_cfg`（`tbt_parse.h:45-73`）加：

```c
	u8 nonce[16];      /* tunnel nonce=; pins the session nonce */
	bool nonce_set;    /* nonce= was present */
```

`struct tbt_session`（`tbtun.h:329-331` 附近）加：

```c
	u8 nonce[16];
	bool nonce_set;
	bool nonce_pinned;
	__be32 anchor_addr;   /* far: the only accepted near source */
	bool anchor_set;
	u64 join_src_reject;
```

### 3.2 `tbt_parse.c`：语法与渲染

**（1）前置声明**：`tbt_parse_nonce()` 定义在 536 行，`tbt_parse_tunnel()`
在 359 行。在 `tbt_parse_tunnel()` 之前加：

```c
static int tbt_parse_nonce(const char *s, u8 out[16]);
```

**（2）tunnel 键**：在 `srcaddrs` 分支之后、最终 `else` 之前加：

```c
		else if (!strcmp(key, TBT_ACT_NONCE))
		{
			if (tbt_parse_nonce(val, cfg->nonce))
				return tbt_fail(e, key,
					"nonce must be 16 or 32 hex digits");
			cfg->nonce_set = true;
		}
```

`TBT_ACT_NONCE` 已在 `tbt_uapi.h` 定义为 `"nonce"`；复用，不新增 uapi。

**（3）far 的 `peer=` 渲染**：`tbt_render_cfg()`（`tbt_parse.c:784-830`）
当前只在 near 分支输出 peer。改为：

```c
	if (c->role == TBT_ROLE_NEAR)
	{
		rc = tbt_addf(buf, size, &off, " peer=%s:%u", pa,
			      tbt_port_load(c->peer_port));
		if (rc)
			return rc;
	}
	else
	{
		rc = tbt_addf(buf, size, &off, " target=%s:%u", ta,
			      tbt_port_load(c->target_port));
		if (rc)
			return rc;
		/* far: the near source address allowed to join carriers.
		 * The port is grammar filler; only the address is used. */
		if (c->peer_addr && c->peer_port)
		{
			rc = tbt_addf(buf, size, &off, " peer=%s:%u", pa,
				      tbt_port_load(c->peer_port));
			if (rc)
				return rc;
		}
	}
```

**（4）nonce 渲染**：在 `portblock` 块之后、`srcaddrs` 之前加：

```c
	if (c->nonce_set)
	{
		char nb[2 * sizeof(c->nonce) + 1];

		tbt_fmt_hex(c->nonce, sizeof(c->nonce), nb);
		rc = tbt_addf(buf, size, &off, " nonce=%s", nb);
		if (rc)
			return rc;
	}
```

`tbt_fmt_hex()` 定义在 `tbt_render_cfg()` 之后（约 846 行）；在
`tbt_render_cfg()` 之前加前置声明，或把函数上移：

```c
static void tbt_fmt_hex(const u8 *in, size_t n, char *out);
```

### 3.3 `tbt_session_create()`（`tbt_session.c:651-689`）

把 684 行的 `get_random_bytes(...)` 替换为：

```c
	if (t->cfg.nonce_set)
	{
		memcpy(s->nonce, t->cfg.nonce, sizeof(s->nonce));
		s->nonce_pinned = true;
		s->nonce_set = true;
	}
	else
	{
		get_random_bytes(s->nonce, sizeof(s->nonce));
	}
	if (t->cfg.role == TBT_ROLE_FAR && t->cfg.peer_addr)
	{
		s->anchor_addr = t->cfg.peer_addr;
		s->anchor_set = true;
	}
	if (t->cfg.role == TBT_ROLE_FAR && !t->cfg.nonce_set && !t->cfg.peer_addr)
		pr_warn_once("tbtun: tunnel %llu far side uses trust-on-first-use; "
			     "configure nonce= or peer= to pin it\n",
			     (unsigned long long)t->cfg.id);
```

- 配置了 `peer=` 的 far，锚在会话创建时就固定，首条 HELLO 也必须来自该源。
- 未配置 `peer=`/`nonce=` 的 far 仍是 TOFU，但会告警；`far.hello_garbage`
  的测试语义保持不变。

### 3.4 `tbt_rx_hello()`（`tbt_rx.c:449-490`）

**改动点**：在 `sess->lock` 内、nonce 检查之前查锚；所有检查通过后再设锚；
拒绝必须发生在 `tbt_session_join()` 之前。

```c
	if (t->cfg.role == TBT_ROLE_FAR)
	{
		struct sock *csk = READ_ONCE(c->sk);
		__be32 src;

		if (h->path_id >= sess->n_car || !csk)
			return -EPROTO;
		src = inet_sk(csk)->inet_daddr;
		spin_lock(&sess->lock);
		/* 1. Established anchor: every later HELLO, any index. */
		if (sess->anchor_set && src != sess->anchor_addr)
		{
			sess->join_src_reject++;
			spin_unlock(&sess->lock);
			return -EPROTO;
		}
		/* 2. Nonce: pinned, adopt-once, or match. */
		if (sess->nonce_pinned)
		{
			if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
			{
				sess->session_join_bad++;
				spin_unlock(&sess->lock);
				return -EPROTO;
			}
		}
		else if (!sess->nonce_set)
		{
			if (t->cfg.peer_addr && src != t->cfg.peer_addr)
			{
				sess->join_nonce_reject++;
				spin_unlock(&sess->lock);
				return -EPROTO;
			}
			memcpy(sess->nonce, body + 2, sizeof(sess->nonce));
			sess->nonce_set = true;
		}
		else if (memcmp(body + 2, sess->nonce, sizeof(sess->nonce)))
		{
			sess->session_join_bad++;
			spin_unlock(&sess->lock);
			return -EPROTO;
		}
		/* 3. First authenticated HELLO fixes the anchor (no peer=). */
		if (!sess->anchor_set)
		{
			sess->anchor_addr = src;
			sess->anchor_set = true;
		}
		spin_unlock(&sess->lock);
		err = tbt_session_join(sess, c, h->path_id);
		if (err)
			return err;
	}
```

- 源检查先于 nonce：陌生源连 nonce 都探不到。
- 只有通过 nonce 检查的 HELLO 才能设锚；被拒的 HELLO 不改锚。
- `join_src_reject` 只在 rx_hello 计数一次；join 的断言不重复计数。

### 3.5 `tbt_session_join()` 防御（`tbt_session.c:802-855`）

在 `index >= n_car` 检查之后、取 `sess->lock` 之后加：

```c
	struct sock *csk = READ_ONCE(c->sk);
	__be32 src;

	if (index >= sess->n_car || !csk)
		return -EINVAL;
	src = inet_sk(csk)->inet_daddr;
	spin_lock(&sess->lock);
	if (!sess->anchor_set || src != sess->anchor_addr)
	{
		/* tbt_rx_hello() is the only caller and already counted the
		 * rejection; fail closed for any future caller. */
		spin_unlock(&sess->lock);
		return -EPROTO;
	}
```

- 首个 join 前，rx_hello 已设锚，断言成立。
- 不要在这里 `join_src_reject++`，否则同一次拒绝会计两次。

### 3.6 `tbt_procfs.c`：`join_src_reject`

在 c3 行（`tbt_procfs.c:695-707`）加：

```c
			      "join_dup=%llu join_bad=%llu join_nonce_reject=%llu "
			      "join_src_reject=%llu "
			      "fbk_tx=%llu fbk_rx=%llu "
```

并在参数列表 `s->join_nonce_reject` 之后加 `s->join_src_reject`。

### 3.7 `tbt_uapi.h`：不改

- `TBT_CONV` 保持 `0x0003`；HELLO 体不变，nonce/源检查都是本地策略。
- `TBT_ACT_NONCE` 已经存在，tunnel 键复用它。
- `/proc` 的 `stats` 行不在 `tbt_uapi.h`；`TBT_PEERS_COLUMNS`/
  `TBT_TUNNELS_COLUMNS` 不受影响。
- 结论：**不改 `tbt_uapi.h`**。

### 3.8 `host/test_parse.c`：要改

1. `test_far_peer()`（471-490）：
   - “far with peer port 0” 仍然拒绝；
   - 把 `ok(strstr(buf, " peer=") == NULL, "far line normalizes peer away")`
     改成断言渲染出 ` peer=198.51.100.7:8443`。
2. `full_far_canon`（820-825）：在 `target=...` 之后加
   ` peer=198.51.100.7:8443`；`roundtrip("full far line", ...)` 随之通过。
3. 新增 nonce 用例：
   - far 行带 `nonce=00112233445566778899aabbccddeeff` 解析成功；
   - 渲染包含该 32 hex；
   - 16 hex 的 `nonce=0011223344556677` 规范化为
     `00000000000000000011223344556677`；
   - 短/非 hex 的 `nonce=` 返回 `TBT_PARSE_EINVAL`，错误键为 `nonce`；
   - `session` 动作的现有用例不变。
4. `maximal_cfg()`（337-368）：加 `c->nonce_set = true;` 并填 32 hex，
   让 `test_maximal_lines()`（370-410）覆盖 `nonce=` 的行宽预算；
   如果 `n < TBT_LINE_MAX` 失败，提高 `TBT_LINE_MAX` 或缩短其他键。
5. `review/lint.sh` 若检查 canonical 键列表，同步更新（先跑再说）。

### 3.9 兼容性

- `far.hello_garbage`：127.0.0.1 的首条 HELLO 采纳 nonce 并设锚；载体死亡
  不清锚。
- `far.anchor`：127.0.0.1 的 index 0 与锚一致，原 occupant 已 dead →
  替换成功。
- `far.dup_index`：127.0.0.1 同 index 0，锚通过，`join_dup` 拒绝。
- `far.srcaddr_dup`：127.0.0.2 在 index 0，锚拒绝，计入 `join_src_reject`
  而不是 `join_dup`；测试只断言 close，仍 PASS。
- `far.srcaddr_free`：127.0.0.2 在 index 1，锚拒绝 → `accepted=0` → PASS。
- `far.wrong_nonce`：同源、错 nonce → nonce 检查拒绝 → PASS。

### 3.10 验收

```sh
# host 解析/渲染（不跑 QEMU；但构建归 perf，等冻结后再跑）
make check
make host-test-san

# far 组（等 perf 释放 QEMU）
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
# 期望：far.srcaddr_free PASS ... refused
#       far.anchor/far.dup_index/far.wrong_nonce/far.hello_garbage PASS
```

配置面：

```sh
echo 'tunnel 1 role=far listen=127.0.0.1:19101 target=127.0.0.1:19070 nonce=<32hex> n=4' \
    > /proc/net/tbtun/config
# 发错 nonce 的 HELLO：载体关闭，join_bad 增长

echo 'tunnel 1 role=far listen=127.0.0.1:19101 target=127.0.0.1:19070 peer=127.0.0.1:1 n=4' \
    > /proc/net/tbtun/config
# 从 127.0.0.2 发正确 nonce：载体关闭，join_src_reject 增长

# 规范配置回读
cat /proc/net/tbtun/config > /tmp/c
echo flush > /proc/net/tbtun/config
cat /tmp/c > /proc/net/tbtun/config
grep -E 'nonce=|peer=' /proc/net/tbtun/config
```

### 3.11 风险

- 旧 `cat config` 备份不含 far `peer=`（旧渲染器丢弃）；恢复会失去锚。
- `peer=` 的端口是语法填充；far 只用地址。若嫌别扭，后续加 `anchor=`。
- near 换源（NAT rebinding/多宿主）会被单锚拒绝；v0 的 near 只用
  `srcaddrs[0]` 拨号，单锚足够；多源需要把锚扩成列表。
- nonce 明文；源绑定只挡 off-path 重放，机密性靠 WireGuard。
- 运行时 `session <id> nonce=` 在已有载体加入后覆盖 nonce；未来载体必须用
  新值；文档要写明。
- `join_src_reject` 改 stats 行；`far.srcaddr_dup` 的 `join_dup` 增量会变 0。
- `nonce=`/far `peer=` 增加规范行长度；`maximal_cfg` 必须覆盖，必要时提高
  `TBT_LINE_MAX`（同时改 procfs linebuf 边界）。

---

## 4. 实施顺序、文件冲突矩阵、冻结边界

### 4.1 文件冲突矩阵

| 文件 | F5 | review 14 | nonce/F8 | multiconn (F4) | perf (CRC/接收证明) | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| `tbt_sock.c` | w_read/w_ctrl/attach | 无 | 无 | `tbt_stream_start`/`far_open_seen`/`target_attach` 大改 | **是** | F5 必须等 perf 冻结；与 multiconn 同文件、逻辑相邻，需 rebase |
| `tbt_tx.c` | `tbt_tx_fin`/`announce`/`fbk`/pump | 只读门控 | 无 | `announce`/pump 的单流假设会被改 | **是** | F5 必须等 perf；与 multiconn 高冲突 |
| `tbt_rx.c` | 接收端已正确，不改 | 无 | `tbt_rx_hello` 源检查 | `tbt_rx_bind_stream`/adoption 会改 | **是** | nonce 的 hello hunk 等 perf；与 multiconn 中冲突 |
| `tbt_session.c` | 无 | 无 | `create`/`join`/anchor | `join`/`far_sid`/stream 表大改 | 否 | `create` 可先做；`join` 等 multiconn |
| `tbt_carrier.c` | 无 | **已落地** | 无 | 可能被 multiconn 碰 | **是** | 不改 |
| `tbt_parse.c/h` | 无 | 无 | `nonce=`/far `peer=` | 无 | 否 | **可独立做** |
| `tbt_procfs.c` | 无 | 无 | `join_src_reject` | stream 统计可能加 | 否 | 可独立做 |
| `tbtun.h` | +6 stream 字段 | 无 | +4 session 字段 | stream/session 字段大改 | 可能加计数器 | 字段分块加；最后统一合入 |
| `host/test_parse.c` | 无 | 无 | far peer/nonce/最大行 | 无 | 否 | 可独立做 |
| `tbt_uapi.h` | 无 | 无 | **不改** | 无 | 否 | 不改 |
| `DESIGN.md` | FIN 规则 | 记录已落地 | 语法/信任模型 | multiconn | 否 | 文档 owner |

### 4.2 哪些必须等 perf 冻结

**必须等 `tbt_tx.c`/`tbt_rx.c`/`tbt_sock.c` 冻结：**

- F5 的全部代码：`tbt_sock.c` 的 w_read/w_ctrl/attach；`tbt_tx.c` 的
  `tbt_tx_fin`/`announce`/`fbk`/pump（review 22）。
- nonce 的 `tbt_rx_hello` 源检查（`tbt_rx.c`）。
- 任何 `tbt_carrier.c` 改动（本波没有）。

**可以独立做（不碰 perf 文件）：**

- `tbt_parse.c/h`：`nonce=` 键、far `peer=` 渲染、前置声明。
- `tbt_session.c` 的 `tbt_session_create` 与 session 字段（不碰 `join`）。
- `tbt_procfs.c` 的 `join_src_reject`。
- `host/test_parse.c` 的 far peer/nonce/最大行测试。
- 这些先落地时是“惰性”的：`nonce=` 只进 cfg，`anchor` 只进 session；
  直到 `tbt_rx_hello` 的 hunk 应用才生效。

**必须与 multiconn 协调（不是 perf）：**

- `tbt_session.c` 的 `tbt_session_join`：multiconn 很可能重写。
- `tbt_sock.c` 的 `tbt_stream_start`/`tbt_far_open_seen`/`tbt_target_attach`：
  F5 的 w_read/w_ctrl 与它们同文件但不同函数；逻辑上 F5 的每流 FIN 必须
  在 multiconn 的流表上工作。
- `tbt_rx.c` 的 `tbt_rx_bind_stream`：nonce 的 hello 检查在其上游。

### 4.3 顺序

1. **review 14：无代码工作**。已落地；只按 §2.2 验收。
2. **Wave A（现在，独立）**：nonce 配置面——`tbt_parse.h/c`、`tbt_session.c`
   的 `create`/字段、`tbt_procfs.c`、`host/test_parse.c`。不碰 perf 文件。
3. **Wave B（等 perf 冻结 `tbt_rx.c`）**：`tbt_rx_hello` 源锚 +
   `tbt_session_join` 防御（后者等 multiconn 合并）。
4. **Wave C（等 perf 冻结 `tbt_sock.c`/`tbt_tx.c`）**：F5 的 (a)+(b)；
   先 `w_read`/`w_ctrl`/`attach`，再 `tbt_tx_fin`/`announce`/`fbk`。
5. **Wave D（可选，`tbt_tx.c`）**：review 22 的 pinned 帧保留/清位；不做则
   F5 的背压验收延后。

如果 multiconn 先落地，Wave C/B 在 multiconn 之上 rebase；如果 F5 先落地，
multiconn 必须保留每流 `tx_fin_*` 字段和 w_read/w_ctrl 逻辑。session 的
anchor 字段与 multiconn 的流表正交。

### 4.4 最终验收命令（等 perf 释放环境后一次跑）

```sh
source ~/.cache/tbtun-build/env.sh && make check
bash review/lint.sh
TBT_ADV_GROUPS=public adversarial/run.sh --no-build
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
```

期望：

- `pub.halfclose PASS`（`EOF=1`）、`pub.rst PASS`；
- `far.srcaddr_free PASS`（refused）、`far.anchor/far.dup_index/
  far.wrong_nonce/far.hello_garbage PASS`；
- `holes_declared` 不随 `-EAGAIN` 增长；`drop_ppm` 仍能制造 hole；
- 无新增 WARN/oops。

`public` 组的 `pub.churn`/`pub.writer`、`far` 组的 `far.data_path` 属于
F4/F6/稳定性任务，不在本计划三项的验收范围。

---

## 5. 当前行号索引（08:50 快照）

| 位置 | 作用 |
| --- | --- |
| `tbt_sock.c:98-124` | `tbt_pub_capture_work`：加 FIN 判据 |
| `tbt_sock.c:303-322` | `tbt_ctrl_work`：改 reset/唤醒 |
| `tbt_sock.c:370-401` | `tbt_sock_attach`：补 w_read + EOF 债 |
| `tbt_tx.c:203-224` | `tbt_tx_fin`：冻结 seq/掩码/返回拷贝数 |
| `tbt_tx.c:178-198` | `tbt_tx_announce`：迟到载体补 FIN |
| `tbt_tx.c:1926-1949` | `tbt_fbk_work`：FIN 重试 |
| `tbt_tx.c:1525-1529,1544-1548` | pump 释放 pinned 帧（review 22） |
| `tbt_tx.c:948-949` | 空洞门控的 `data_frames - tail`（不改） |
| `tbt_carrier.c:528,611,689` | review 14 已落地的三处计数 |
| `tbt_parse.h:45-73` | `struct tbt_cfg`：加 nonce |
| `tbt_parse.c:359` | `tbt_parse_tunnel`：加 nonce 键（需前置声明） |
| `tbt_parse.c:536` | `tbt_parse_nonce` 定义 |
| `tbt_parse.c:784-830` | `tbt_render_cfg`：far peer + nonce |
| `tbt_session.c:651-689` | `tbt_session_create`：nonce/anchor |
| `tbt_session.c:802-855` | `tbt_session_join`：源断言 |
| `tbt_rx.c:449-490` | `tbt_rx_hello`：源锚 + nonce |
| `tbtun.h:253-262` | stream TX 段：加 `tx_fin_*` |
| `tbtun.h:329-331` | session nonce 字段：加 anchor/计数 |
| `tbt_procfs.c:695-707` | c3 stats 行：加 `join_src_reject` |
| `host/test_parse.c:471-490` | far peer 渲染断言 |
| `host/test_parse.c:812-825` | `full_far`/`full_far_canon` |
| `host/test_parse.c:337-410` | `maximal_cfg`/最大行 |

---

## 6. session/rx 应用（nonce/F8，待 P2a 完成后实施）

本节只写计划，不改源码、不构建。§5 的 session/rx 行号是 08:50 快照，
已被本节 13:45 锚点取代。解析部分（`tbt_cfg.nonce/nonce_set`、`nonce=`
的语法与 canonical render）已经落地；本节只把值接到会话创建和 HELLO 准入。

### 6.1 当前锚点

| 位置 | 作用 |
| --- | --- |
| `tbt_session.c:917-959` | `tbt_session_create`；954 行仍是 `get_random_bytes(s->nonce)` |
| `tbt_rx.c:409-493` | `tbt_rx_hello`；445-476 是 nonce 分支，478 调 `tbt_session_join` |
| `tbt_session.c:1104-1157` | `tbt_session_join`（P2a 可能重写） |
| `tbtun.h:343-345` | `nonce[16] / nonce_set / nonce_pinned` |
| `tbtun.h:449` | `session_join_dup, session_join_bad, join_nonce_reject` |
| `tbt_procfs.c:711-720` | c3 stats 行（`join_dup/join_bad/join_nonce_reject`） |
| `tbt_parse.h:74-80` | 已落地的 `cfg.nonce[16] / nonce_set` |

### 6.2 `tbt_session_create`：应用配置 nonce 与锚

把 954 行的 `get_random_bytes(...)` 替换为：

```c
	if (t->cfg.nonce_set)
	{
		memcpy(s->nonce, t->cfg.nonce, sizeof(s->nonce));
		s->nonce_pinned = true;
		s->nonce_set = true;
	}
	else
	{
		get_random_bytes(s->nonce, sizeof(s->nonce));
	}
	/* Optional configured anchor.  A far `peer=` is accepted by the parser
	 * but not yet rendered canonically; the primary anchor is the first
	 * HELLO that passes the nonce check. */
	if (t->cfg.role == TBT_ROLE_FAR && t->cfg.peer_addr)
	{
		s->anchor_addr = t->cfg.peer_addr;
		s->anchor_set = true;
	}
	if (t->cfg.role == TBT_ROLE_FAR && !t->cfg.nonce_set && !t->cfg.peer_addr)
		pr_warn_once("tbtun: tunnel %llu far side uses trust-on-first-use; "
			     "configure nonce= to pin it\n",
			     (unsigned long long)t->cfg.id);
```

- `nonce_pinned = true` 让 `tbt_rx_hello` 走 pinned 分支；`nonce_set = true`
  防止后续采纳覆盖。
- 如果 P2a 在 `tbt_session_create` 里重排 per-stream/ack 初始化，把这段放在
  `s->n_pri` 之后、`tbt_carriers_start()` 之前；不要插进 P2a 的 ack 块。
- `cfg.peer_addr` 的锚是可选加固；本波解析只加了 `nonce=`，far `peer=` 仍
  不是规范键。最小验收不依赖它。

### 6.3 `tbt_rx_hello`：先查锚，再查 nonce

把 far 分支的开头改成下面的形状（现有 nonce 三分支原样保留，锁只取一次）：

```c
		struct sock *csk = READ_ONCE(c->sk);
		__be32 src;

		if (h->path_id >= sess->n_car || !csk)
			return -EPROTO;
		src = inet_sk(csk)->inet_daddr;
		spin_lock(&sess->lock);
		/* An established anchor rejects any other source, free index
		 * included, before the nonce is even examined. */
		if (sess->anchor_set && src != sess->anchor_addr)
		{
			sess->join_src_reject++;
			spin_unlock(&sess->lock);
			return -EPROTO;
		}
```

现有 `nonce_pinned` / 采纳 / 比对三分支保持不动；在它们之后、`spin_unlock`
之前加：

```c
		/* Only a HELLO that passed the nonce check may fix the anchor. */
		if (!sess->anchor_set)
		{
			sess->anchor_addr = src;
			sess->anchor_set = true;
		}
```

- 现有 461-462 行的 `t->cfg.peer_addr` 采纳检查保留，作为配置锚的兜底。
- 拒绝发生在 `tbt_session_join()` 之前，不会打开 target、不会回 HELLO、
  不会安装载体。
- `join_src_reject` 只在 rx_hello 计数一次；不要放在 `tbt_session_join`。

### 6.4 `tbt_session_join`：防御性断言（可选）

如果 P2a 没有重写 join，把开头改成下面的形状（锁只取一次）：

```c
		struct sock *csk = READ_ONCE(c->sk);
		__be32 src;

		if (index >= sess->n_car || !csk)
			return -EINVAL;
		src = inet_sk(csk)->inet_daddr;
		spin_lock(&sess->lock);
		if (!sess->anchor_set || src != sess->anchor_addr)
		{
			spin_unlock(&sess->lock);
			return -EPROTO; /* rx_hello already counted it */
		}
```

- 首个 join 前，rx_hello 已设锚；断言成立。
- 不要在 join 里 `join_src_reject++`，否则同一次拒绝会计两次。
- P2a 若把 join 改成 per-stream 的 announce/绑定，优先保留 rx_hello 的准入；
  join 断言可以删掉。

### 6.5 字段与 stats

`tbtun.h` 的 session nonce 块（343-345）之后加：

```c
	__be32 anchor_addr;   /* far: the only accepted near source */
	bool anchor_set;
	u64 join_src_reject;
```

`tbt_procfs.c` 的 c3 行（711-720）加：

```c
			      "join_dup=%llu join_bad=%llu join_nonce_reject=%llu "
			      "join_src_reject=%llu "
```

并在参数列表 `s->join_nonce_reject` 之后加 `s->join_src_reject`。

### 6.6 与 multiconn P2a 的冲突

| 文件 | P2a 正在改什么 | 本节要加什么 | 处理 |
| --- | --- | --- | --- |
| `tbt_session.c` | `tx_ack/rx_ack` 移到 stream、stream 生命周期、join/announce、tombstone | create 的 nonce/anchor；join 的防御断言 | **高冲突**；create 块插在 P2a 的 per-stream 初始化之后；join 断言等 P2a 定型后再加或删 |
| `tbt_rx.c` | 接收证明/ack、`tbt_rx_data`/bind | `tbt_rx_hello` 的源锚检查 | 中冲突；hello 在 bind/join 之前，检查本身稳定；按函数名 rebase |
| `tbtun.h` | 新 stream/session 字段 | `anchor_addr/anchor_set/join_src_reject` | 中冲突；单独一块加在 nonce 块后，不要混进 P2a 的 ack 字段 |
| `tbt_procfs.c` | 可能加 per-stream 统计 | `join_src_reject` | 低冲突；按 stats 行 rebase |
| `tbt_parse.*` | 不改 | 已落地 | 无 |

**合并顺序：P2a 先，本节后。** P2a 正在改 tbt_tx.c/tbt_session.c/tbtun.h
的 ack 与 stream 生命周期；本节只依赖 `tbt_rx_hello` 和 session 创建。
如果本节先落地，P2a 必须保留 `anchor_*` 字段和 rx_hello 的源检查。

### 6.7 最小验收（等 P2a 完成后）

```sh
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
```

期望：

- `far.hello_garbage PASS`：127.0.0.1 的首条 HELLO 采纳 nonce 并设锚；
- `far.anchor PASS`：127.0.0.1 的 index 0 与锚一致；
- `far.dup_index PASS`：同源同 index，锚通过，`join_dup` 拒绝；
- `far.wrong_nonce PASS`：同源错 nonce，nonce 检查拒绝；
- `far.srcaddr_free PASS`：127.0.0.2 的空闲 index 被锚拒绝（REFUSED）；
- `far.srcaddr_dup` 仍 close（现在由源锚拒绝，`join_dup` 增量可能为 0）。

可选配置面：给 far 行加 `nonce=<32hex>`，用 `adv_probe` 发错 nonce 的首连，
确认在设锚之前就被 `session_join_bad` 拒绝；再加 `peer=127.0.0.1:1`，从
127.0.0.2 发正确 nonce，确认被 `join_src_reject` 拒绝。

### 6.8 风险与边界

- 单锚会拒绝多宿主 near；v0 的 near 只用 `srcaddrs[0]` 拨号，单锚足够。
- far `peer=` 目前不是规范键（渲染器仍省略）；锚的配置形式是可选加固，
  最小验收走首连 TOFU 锚。
- nonce 在 HELLO 里明文；锚只挡 off-path 重放，机密性靠 WireGuard。
- 锚是会话属性，跨载体死亡/替换存活；`far.hello_garbage` 设锚后载体死亡，
  `far.anchor` 仍应通过。
- `join_src_reject` 会改 c3 stats 行；`far.srcaddr_dup` 的 `join_dup` 增量
  会变 0。更新 stats 测试/文档。
- 如果 P2a 把 `tbt_session_join`/`tbt_session_bind_sid` 改成多流绑定，源锚
  仍应在 `tbt_rx_hello` 入口做；不要把它下沉到 bind，否则旧 sid 的迟到
  帧会被误拒。

### 6.9 host 规格测试映射（`host/test_anchor.c`）

`host/test_anchor.c` 是纯 C 模型，95 checks；它把下面的顺序和计数固定下来，
后续 `tbt_rx_hello`/`tbt_session_join` 按这张表实现：

| 模型步骤 | 内核落点 |
| --- | --- |
| `anchor_init(peer, nonce, pinned)` | `tbt_session_create`：`cfg.nonce/nonce_set` → `sess->nonce`、`nonce_pinned/nonce_set`；`cfg.peer_addr` → `anchor_addr/anchor_set` |
| 源锚检查 | `tbt_rx_hello` far 分支、nonce 之前：`join_src_reject++`，返回 `-EPROTO` |
| pinned memcmp | `tbt_rx_hello` 的 `nonce_pinned` 分支：`session_join_bad++`，返回 `-EPROTO` |
| 采纳（`!nonce_set`） | `tbt_rx_hello`：`cfg.peer_addr` 不匹配 → `join_nonce_reject++`；否则 `memcpy` 并置 `nonce_set` |
| `nonce_set` memcmp | `tbt_rx_hello` 的 else 分支：`session_join_bad++`，返回 `-EPROTO` |
| 设锚 | `tbt_rx_hello` 在 nonce 检查通过之后：`anchor_addr = src; anchor_set = true` |
| occupant 检查 | `tbt_session_join`：live old → `session_join_dup++`，返回 `-EALREADY` |
| accept | `tbt_session_join`：`sess->car[index] = c` |

顺序不变量：**源 → nonce → 设锚 → join**；一次拒绝只增加一个计数，且绝不
设置/改变锚。`host/test_anchor.c` 的用例覆盖：TOFU 首连、同源重连、换源、
重放 nonce、错误 nonce、live/dead occupant、配置 peer、pinned nonce、
`join_nonce_reject` 兜底、计数分离。
