# 载体接收队列补齐设计（rcvq-design）

状态：设计稿，未改内核源码。基线 `6fd2191`，仓库 `tcp-brutal`。
范围：只讨论**载体（carrier）TCP socket 自身的接收队列**，不讨论 public/target socket 的交付路径。
前提：两端均加载本模块（双端可控）；FEC 已恢复出缺失 share 的明文 payload 与块元数据。

---

## 0. 结论先行

1. 历史否决 `tcp_send_rcvq()` 的结论对 **public socket + 任意填充字节** 成立，对 **carrier socket + FEC 精确还原的原始 wire 字节** 不成立。前者方向错、读者错、字节也错；后者方向对（peer→module）、读者就是模块、字节逐位相同。
2. 同序号重复段安全：只要注入段的 `[seq, end_seq)` 与在途真实段完全相同，先注入者推进 `tp->rcv_nxt`，后到者命中 `tcp_data_queue()` 的 `if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt))`，按 `TCP_OLD_DATA` 丢弃并回 D-SACK/quick ACK。**不需要**修改内核的重复段判定。
3. 接收端不需要发送端告知**洞的** TCP seq：洞的起点就是本端载体 socket 的 `tp->rcv_nxt`。若洞恰好等于整条记录（大 MSS/测试环境），`tcp_seq = rcv_nxt`，只需 `(carrier_id, wire_len, wire_bytes)`；若洞只是记录的子区间（真实 MSS 分段），manifest 还需携带记录起始 `record_start_seq`，接收端用 `delta = rcv_nxt - record_start_seq` 定位，见 §3.4。
4. `wire_bytes` 可以逐字节重建：48B FEC 头（由块元数据重算 CRC）+ 恢复出的明文 payload 经 `tbt_crypto_body_xor_flow()` 重加密 + 规范 HTTP chunk 头尾。全量 share 的 wire 长度固定为 `6 + 48 + 8192 + 2 = 8248`；短 share 由块内 `short_len`/`dlen` 决定，chunk 头长度随之变化。
5. `carrier_id` 的最小来源是**同块已收 share 的载体集合差**：块内 `(group, share) → carrier` 在正常调度下是双射（`carrier = group_map[group]*(n+m)+share`，`tbt_fec_data.c:1883`）；缺一份 share 时，缺的 carrier 就是补集。重放/回退到别的载体时，需要一份轻量 `FEC_MAP` 清单（或每 share 头携带原始 carrier_id）才能无歧义。
6. 最小注入路径：在载体 RX work 已持有的 `lock_sock(sk)` 下，**直接操作公开的 `struct tcp_sock` 字段**（`rcv_nxt`/`rcv_wup`/`out_of_order_queue`/`ooo_last_skb`/`selective_acks`），用导出的 `__sk_mem_schedule()` 预留内存、`__tcp_send_ack()` 立即回累计 ACK。不依赖 `tcp_send_rcvq()`/`tcp_rcv_established()`/kprobe；QEMU/KVM 7.2.9 原型已实测 wire 字节 memcmp 100% 匹配、洞合并、迟到段 D-SACK。
7. 首版只做**单洞、单记录、DATA share**；多记录洞、PARITY、重放/manifest 作为第二阶段。控制记录（HELLO/ACK/NACK/FBK/FIN）不受 FEC 保护，其丢失仍只能等 TCP 重传，这是本设计的明确边界。

---

## 1. 历史：`tcp_send_rcvq()` 当年为什么被放弃

### 1.1 事实

- `tcp_send_rcvq()` 是内核 `net/ipv4/tcp_input.c` 的内部函数（7.2.9 约 `:5549`，本地 7.0.14 为 `:5575`）：
  ```c
  int tcp_send_rcvq(struct sock *sk, struct msghdr *msg, size_t size)
  {
      ...
      skb = alloc_skb_with_frags(...);
      ...
      if (tcp_try_rmem_schedule(sk, skb, skb->truesize)) goto err_free;
      skb_copy_datagram_from_iter(skb, 0, &msg->msg_iter, size);
      TCP_SKB_CB(skb)->seq     = tcp_sk(sk)->rcv_nxt;
      TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(skb)->seq + size;
      TCP_SKB_CB(skb)->ack_seq = tcp_sk(sk)->snd_una - 1;
      if (tcp_queue_rcv(sk, skb, &fragstolen)) __kfree_skb(skb);
      return size;
  }
  ```
  它不检查 `tp->repair`、不检查连接状态、不检查连续性；唯一调用点是 `tcp_sendmsg_locked()` 在 `tp->repair && tp->repair_queue == TCP_RECV_QUEUE` 时（`net/ipv4/tcp.c:1202-1210`）。`tcp_send_rcvq` 与 `tcp_queue_rcv` 均**无 `EXPORT_SYMBOL`**（7.2.9 `Module.symvers` 中零命中）。
- 仓库历史里**从未提交过** `tcp_send_rcvq()` 的调用或包装；`tbtun/` 只有 `tbt_core.c:132-139` 的注释说明“曾用 one-shot kprobe 解析它，后来 kprobe 被删除”。唯一可执行用户是测试模块 `tests/probe2/tbt_probe2.c:482-490`（`register_kprobe("tcp_send_rcvq")`），在 `lock_sock()` 下调用（`:184-186`）。
- 两次测量否决了它：
  - `docs/recvmsg-probe.md`：应用的 fd 是 **client socket**，不是模块持有的 accepted socket；替换 accepted socket 的 `ops` 或往其接收队列写，应用都读不到。
  - `docs/inject-probe.md`：在双向连接上注入任意填充字节，注入推进了 `rcv_nxt`，随后到达的对端真实字节被判旧数据；实测 20520 字节总读取中丢了 36 字节（`36/40` 对端字节），见 `docs/inject-probe.md:206-215`。
- 直接引文：
  - `tbtun/tbt_sock.c:28-31`：“appending to the accepted socket's receive queue does reach the app, but is unsafe on a duplex connection: bytes the peer sends while we inject land behind an `rcv_nxt` we advanced and are dropped as `TCP_OLD_DATA`.”
  - `DESIGN.md:3453-3459`：同一结论；`docs/prior-art.md:845-848` 进一步指出，接收队列 `[copied_seq, rcv_nxt)` 内出现不连续 skb 会在 `tcp_collapse()` 命中 `BUG_ON(offset < 0)`。

### 1.2 当年的竞态，精确到序号

设 `S` 为模块持有的 accepted socket，`C` 为应用 client socket，二者是同一条 TCP 连接的两端；`S.rcv_nxt` 与 `C.snd_nxt` 描述 C→S 的同一字节流。

1. `t0`：`S.rcv_nxt = x`，`S.copied_seq = x`，`C.snd_nxt = x`。C 的 `[x, x+1)` 在途；C 的后续段 `[y, …)`（`y > x`）可能已进 `S.out_of_order_queue`。
2. `t1`：模块在 `lock_sock(S)` 下调用 `tcp_send_rcvq(S, …, L)`：skb `seq = x`、`end_seq = x+L` 入 `S.sk_receive_queue`，`tcp_rcv_nxt_update()` 把 `S.rcv_nxt` 改成 `x+L`；`C.snd_nxt` 仍是 `x`。
3. `t2`：C 的 `[x, x+1)` 到达，`end_seq = x+1 ≤ rcv_nxt = x+L`，`tcp_data_queue()` 判 `TCP_OLD_DATA`，D-SACK 后丢弃。
4. `t3`：C 重传 `x`，`tcp_sequence()` 以 `rcv_nxt` 比较仍判旧，继续丢。
5. `t4`：S 对 `x+L` 的累计 ACK 超过 C 的 `snd_nxt`；C 要么把 ACK 当非法忽略并继续重传，要么把 `snd_una` 推过从未发送的数据。随后 `copied_seq` 读到伪造的 `[x, x+L)`，把它当应用数据送进隧道；far 端收到应用从未写过的字节，应用真实字节永久丢失。

三种交错：
- **推进 `rcv_nxt` 越过在途真实字节**：实测路径，真实字节被“跳过”并以旧数据丢弃。
- **只塞接收队列、不推进 `rcv_nxt`**：队列与 `copied_seq` 不连续，`tcp_recvmsg_locked()` 的 `WARN(before(*seq, TCP_SKB_CB(skb)->seq))` 或 `tcp_collapse()` 的 `BUG_ON(offset < 0)` 会触发。
- **ooo 队头缺口**：若注入把 `rcv_nxt` 推到 `x+L < y`，缺口 `[x+L, y)` 只能由 C 在 `x+L` 发送，但 C 的下一个字节在 `x`（已被判旧），缺口永远等不到；若 `x+L ≥ y`，队头段被当旧/重叠丢掉。

### 1.3 为什么 carrier 场景不同

| 维度 | public socket（当年） | carrier socket（本设计） |
| --- | --- | --- |
| 读者 | 应用（实际 fd 是 client socket） | 模块自己（`tcp_read_sock`，`tbt_carrier.c:1302`） |
| 注入字节 | 任意填充，不是对端真实字节 | FEC 恢复的原始 wire 字节，逐位相同 |
| 序号 | 伪造 `rcv_nxt` 之后的数据 | 洞的起点就是本端 `rcv_nxt`，真实缺失段 |
| 方向 | 隧道→应用（写接收队列方向错） | peer→module（正是载体接收方向） |
| 结论 | 否决 | 可做，但必须精确填洞并排空 ooo |

关键区别：carrier socket 没有应用 fd，模块是唯一读者；FEC 恢复的 share 本来就是 peer 写在 carrier 上的字节。只要重建的 wire 字节与 peer 写出的完全一致，注入就不是“伪造”，而是“替 peer 重放一段它已经发送、但本端没收到的字节”。

### 1.4 当前 FEC 恢复字节的实际去向

当前路径（行号为本仓库）：

```
tbt_fec_rx_decode_one()                  tbt_fec_data.c:4825
  -> tbt_fec_deliver_ready()             tbt_fec_data.c:3702
  -> tbt_rx_fec_deliver(st, off, ...)    tbt_rx.c:332
  -> st->rx_q                            tbt_rx.c:357-363
  -> w_drain / tbt_drain_body            tbt_sock.c:455 / 341
  -> tbt_sock_deliver(st)                tbt_sock.c:289
  -> tcp_sendmsg_locked(st->sk)          tbt_sock.c:309
```

`st->sk` 是 accepted public socket（near）或 target connection（far）（`tbtun.h:568-571`）。也就是说，FEC 恢复的是 **share 的 payload**，按流字节偏移 `off = rx->deliver_bytes`（`tbt_fec_data.c:3725`）写进**隧道→应用/目标**方向的发送队列；carrier socket 只被 `tcp_read_sock()` 读，模块从不写它的 `rcv_nxt`/`copied_seq`/`sk_receive_queue`/`out_of_order_queue`（`tbt_carrier.c:641,921,1239` 只读诊断）。因此 carrier 的 TCP 洞仍在，后续已到 share 留在 `out_of_order_queue`，只能等 TCP 重传——这正是要补的缺口。

这个结构选择对“隧道→应用”方向是对的（§1.3）；缺的是“peer→module”方向 carrier 字节流的修复。两者不是同一个队列，也不是同一个方向，不能因为历史否决了前者就否定后者。

---

## 2. 核心原理：同序号重复段为什么安全

内核 `tcp_data_queue()`（7.2.9 约 `:5698`，本地 7.0.14 `:5724`）对旧数据的判定是：

```c
if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt)) {
    tcp_rcv_spurious_retrans(sk, skb);
    reason = SKB_DROP_REASON_TCP_OLD_DATA;
    tcp_dsack_set(sk, TCP_SKB_CB(skb)->seq, TCP_SKB_CB(skb)->end_seq);
    tcp_enter_quickack_mode(sk, TCP_MAX_QUICKACKS);
    inet_csk_schedule_ack(sk);
    tcp_drop_reason(sk, skb, reason);
    return;
}
```

因此，只要注入段满足：

```
TCP_SKB_CB(injected)->seq     == tp->rcv_nxt          （注入时，持 lock_sock）
TCP_SKB_CB(injected)->end_seq == seq + wire_len
wire_len                      == 洞长（或洞的前缀）
injected bytes                == peer 原始 wire bytes（逐字节）
```

则：
- 注入先到：本实现把 skb 追加到 `sk_receive_queue`，推进 `rcv_nxt`；随后**显式**排空 `out_of_order_queue` 中 `seq == rcv_nxt` 的整段（`tcp_ofo_queue()` 是 static，不能直接调用）。`tcp_send_rcvq()` 只做入队、不排空 ooo，也不是本实现使用的路径。
- 原始段后到：若 `end_seq < rcv_wup`，先在 `tcp_sequence()` 以 `TCP_OLD_SEQUENCE` 丢弃并发 D-SACK；否则通过 validate，在 `tcp_data_queue()` 命中上面的旧数据分支，`TCP_OLD_DATA` + D-SACK + quick ACK + 丢弃。两条路都不破坏流同步；注入后立即 `__tcp_send_ack()` 会把 `rcv_wup` 设为 `rcv_nxt`，所以 `rcv_wup <= rcv_nxt` 恒成立。
- 原始段先到：注入前在 `lock_sock` 下重读 `rcv_nxt`，若洞已被真实段填上，放弃注入（或把恢复字节当重复丢弃）。`lock_sock` 期间新输入走 `sk_backlog`，不会与检查并发。

**唯一不能妥协的是字节相同。** 如果注入的是任意填充，后到的真实字节被丢弃就是永久数据丢失——这正是 `inject-probe` 的 36/40。FEC 恢复的 share 是原始字节，所以这条前提成立；但必须保证重建逐位一致，且洞的位置/长度精确匹配。

**关键区别：注入后的 `rcv_nxt` 不超过 peer 的 `snd_nxt`。** peer 确实发送过这段字节（否则 carrier 不会出现这个洞），FEC 只是替它重放；因此累计 ACK 覆盖的是 peer 已发送的数据，`tcp_ack()` 不会把它当 `TCP_ACK_UNSENT_DATA`。历史 public-socket 注入的 `rcv_nxt` 超过了 `C.snd_nxt`（peer 从未发送注入的字节），才产生非法 ACK/黑洞（§1.2 t4）。这是 carrier 场景与历史场景最根本的差别。

### 2.1 不变式清单

| 编号 | 不变式 | 违反后果 |
| --- | --- | --- |
| I1 | 注入 skb 的 `seq == tp->rcv_nxt`（注入瞬间，持 `lock_sock`），`end_seq == seq + wire_len` | 洞错位/重复/永久损坏 |
| I2 | 注入字节与 peer 原始 wire 字节逐位相同；`wire_len` 等于洞长（或洞的前缀） | 真实字节被当旧数据丢弃，永久丢失 |
| I3 | 注入后 `[copied_seq, rcv_nxt)` 内是连续的 skb 序列，不能出现“无 skb 的字节” | `tcp_collapse()` 的 `BUG_ON(offset < 0)`（`docs/prior-art.md:845-848`） |
| I4 | ooo 排空只移动 `seq == rcv_nxt` 的整段；重叠/部分段/带 FIN 的段停手，交给内核 | 破坏 ooo 树、重复计费或丢失 FIN |
| I5 | 新 skb 必须 `sk_rmem_schedule()` 成功后 `skb_set_owner_r()`；`bytes_received` 增加 `wire_len`；`rcv_wup <= rcv_nxt` | 内存记账错误、窗口/ACK 异常 |
| I6 | 只在 carrier RX work 的 `lock_sock` 下操作；FEC 解码侧只入队，不取 `lock_sock`；不持 `rx->lock` 取 `lock_sock` | 死锁/ABBA、`sock_owned_by_me` 断言 |
| I7 | 注入记录被解析器读到时，FEC 层必须判重（`dpresent`/late），不能二次交付 | 应用收到重复字节 |
| I8 | intent 的 `carrier gen` 与当前 `c->gen` 一致，`sk_state == TCP_ESTABLISHED` | 往已死/已替换的 socket 注入 |
| I9 | 推进 `rcv_nxt` 后删除 `end_seq <= rcv_nxt` 的 SACK block（复制 `tcp_sack_remove()`）；排空 ooo 时维护 `tp->ooo_last_skb` | 对端继续 SACK/DSACK 已确认区间，`sacked_out` 抖动；`ooo_last_skb` UAF |

---

## 3. 接收端如何精确知道 `(carrier_id, tcp_seq, wire_bytes)`

### 3.1 `tcp_seq`：不需要发送端告知

- 注入点就是本端 carrier socket 的 `tp->rcv_nxt`（洞的起点），在 `lock_sock` 下读取。
- 洞的终点是 `tp->out_of_order_queue` 队头的 `TCP_SKB_CB(skb)->seq`；洞长为 `ooo_head_seq - rcv_nxt`。
- 发送端的 FEC `seq` 是**包索引**，不是 TCP seq；不要用 `seq * S` 去推 TCP seq（`tbt_rx.c:331` 的注释已过时，短包使 `seq*S` 不成立）。FEC `seq` 只用于确定 share 在块内的位置和原始发送顺序。
- 整条记录洞不需要发送端写 `write_seq`：接收端用本地 `rcv_nxt` 作为锚点，要求恢复记录的 `wire_len` 与洞长匹配。
- 子区间洞（真实 MSS 下丢失段只是记录的一部分）需要记录起始 seq/offset，见 §3.4；这是阶段 2 的扩展，首版在“洞 == 整条记录”时工作。

### 3.2 `wire_bytes`：逐字节重建

一条 FEC 记录在 carrier 字节流中的 wire 形式（HTTP profile）是：

```
<hex body_len>\r\n  +  [48B tbt_fec_hdr]  +  [payload_len B ChaCha8 ciphertext]  +  \r\n
```

其中 `body_len = 48 + payload_len`，`tbt_http_chunk_hdr()` 输出小写十六进制、无前导零（`tbt_http.c:875-898`），`tbt_http_fec_chunk_encode()` 正是发送端使用的函数（`tbt_http.c:1241-1255`）。全量 share：`body_len = 8240 = 0x2030`，chunk 头 `"2030\r\n"` 6 字节，wire 长度 `6 + 8240 + 2 = 8248`。短 share：`payload_len < 8192`，chunk 头长度随 body_len 变化（本协议 body_len≥49，实际 2..4 个 hex 位 + CRLF），wire 长度 `chunk_hdr_len + 48 + payload_len + 2`。

重建步骤（全部在 `w_fec_rx` 的块解码路径中完成，纯函数可抽到 host 测试）：

1. **48B 头**：从 `tbt_fec_blk` 的块元数据重算，字段如下表。`payload_crc32c` 对**明文** payload 重算；`header_crc32c` 用 `tbt_fec_hdr_seal()` 重算（它同时填 payload CRC）。
2. **密文**：恢复出的 `b->data[slot]` 是**明文**（RX 在放置前已解密并校验 payload CRC，`tbt_fec_data.c:6306-6325`）。对明文调用 `tbt_crypto_body_xor_flow(&sess->crypto_ctx.rx, sid, seq, parity, share_index, buf, len)`（`tbt_crypto.c:791-824`）得到与发送端完全相同的密文；该函数每次按 `(sid, seq, parity, share_index)` 重建计数器，RX 上下文只读，不依赖流式位置。crypto 关闭时是 no-op。
3. **chunk 头尾**：`tbt_http_chunk_hdr(buf, cap, 48 + payload_len)` + 记录 + `"\r\n"`。发送端用的同一函数，字节确定。
4. **LAST 标志**：`h.flags` 只在流最后一个 DATA share 上置 `TBT_FEC_FF_LAST`。首版只注入非 LAST share；若块状态能无歧义地给出 LAST（FIN 路径已记录），再支持 LAST。

| 偏移 | 字段 | 重建来源 |
| --- | --- | --- |
| 0 | `type` | slot `< n` → DATA；否则 PARITY |
| 1 | `flags` | 非 LAST 为 0；LAST 由块/FIN 状态给出 |
| 2 | `share_index` | slot 索引 |
| 4 | `n` | `b->n` |
| 5 | `m` | `b->m` |
| 6 | `epoch` | DATA 为 0；PARITY 为块内最后实 share 长度（`b->short_len`） |
| 8 | `block_id` | `b->block_id` |
| 16 | `seq` | DATA：`tbt_fec_data_seq(b->seq0, group, share)`；PARITY：`b->seq0` |
| 24 | `win_len` | DATA 为 0；PARITY 为块的窗口长度（`b->win_len_set` 时已知） |
| 26 | `payload_len` | present：`b->dlen[slot]`；recovered：最后一个实 share 用 `b->short_len`，否则 `S` |
| 28 | `conv` | `TBT_FEC_CONV` |
| 30 | `reserved` | group 索引 |
| 32 | `sid` | `b->sid` |
| 40 | `payload_crc32c` | 对明文 payload 重算 |
| 44 | `header_crc32c` | `tbt_fec_hdr_seal()` 重算 |

**注意**：被恢复 share 的 48B 头本身**不参与 FEC 编码**（FEC 只编码 payload），所以头不能从 parity 恢复，只能从块元数据重算。这也是为什么 `carrier_id` 不能只放在丢失 share 的头里——它必须由同块已收 share 推断，或由一份独立清单携带。

### 3.3 `carrier_id`：集合差、仿射映射与 manifest

正常调度下，块内 share 到 carrier 的映射是确定的：

```
carrier = group_map[group] * (n + m) + share
```

（`tbt_fec_data.c:1883`；`tx->group_map` 是本地到物理 group 的映射。）接收端在放置每个已收 share 时知道它来自哪个 carrier（`tbt_fec_rx_record(c, ...)` 的 `c`）。因此：

- **单缺失（首版）**：块内缺一个 share，且该块映射范围 `group_map[group]*(n+m)+share` 内的 carrier 中，已收 share 覆盖了除一个以外的所有；缺的 carrier 就是补集。若块内只有一个 group、映射是双射，这唯一确定。
- **同 group 至少一份已收**：`group_map[group] = (carrier - share) / (n + m)`；若整除且落在当前 carrier 集合内，直接算出缺失 share 的 carrier。
- **多缺失/重放/回退**：一个 share 可能被重放到另一条 carrier（`tbt_fec_data.c:1426-1472`），集合差不再唯一。此时需要一份轻量 `FEC_MAP`：
  - 新控制记录（建议在 `tbt_rec` 控制类型空间新增 `TBT_F_FEC_MAP`；若改走 FEC 记录空间，必须同步扩展 `tbt_fec_record_type()` 与解析门），body：`block_id (u64) | n (u8) | m (u8) | groups (u8) | gen (u8) | per-share: carrier_id (u8), record_start_seq (u32), wire_len (u16)`；由发送端在块建立时或收到 NACK 时发出，走 TCP 可靠的控制记录路径。
  - 接收端把 map 存进块槽；恢复 share 时查 `map[share]`。若 map 缺失/`gen` 不匹配/该 carrier 已死，放弃注入，回退 TCP 重传。
  - 开销：`12 + 7*(n+m)` 字节/块（每 share 1B carrier + 4B seq + 2B len）；`n+m=256` 时约 1.8 KB，相对块数据（`n*8KB`）仍可忽略。也可以只在 NACK 时按需请求，避免每块都发。
- **每 share 头携带 `carrier_id`** 是另一种选择，但会改 48B 头/`TBT_FEC_CONV`，且丢失 share 的头不可恢复；它只能帮助“已收 share 的原始 carrier”在重放时被识别，仍需集合差推断缺失者。首版不做。


### 3.4 子区间洞（真实 MSS 分段）

真实网络的 MSS 通常 1460 B，而一条 FEC 记录 8248 B（全量），所以一个丢失的 TCP 段往往只是记录的**子区间**，洞长不等于 `wire_len`。这时接收端必须知道记录在 carrier 字节流中的起点，才能把洞映射到重建记录的 `wire[delta .. delta+inject)`：

- 发送端在写记录前读 `tp->write_seq`，记录起始 TCP seq `S_r = write_seq + 批内前缀`；批写短写时尾部仍在同一记录内，`S_r` 不变。把 `(carrier_id, S_r, wire_len)` 放进 `FEC_MAP`（§3.3）或每 share 的冗余清单。
- 接收端：`delta = (s32)(rcv_nxt - S_r)`（32 位回绕安全）。`delta < 0` 表示洞在记录之前，等更早的记录；`delta >= wire_len` 表示记录已被收到，跳过；否则注入 `wire[delta .. delta + min(hole_len, wire_len - delta))`。
- 整条记录洞是 `delta == 0 && hole_len == wire_len` 的特例；首版先做这个特例，阶段 2 加 `FEC_MAP` 后支持任意 `delta`。
- 迟到原段的处理不变：它的 `end_seq <= rcv_nxt` 时被旧数据丢弃；若跨过 `rcv_nxt`，内核按部分重叠排队并跳过前缀（`tcp_data_queue():5719`、`tcp_recvmsg_locked()` 的 offset），不会重复投递。首版仍要求注入段与洞精确对齐，避免部分重叠。
- 没有 `S_r` 时，接收端也可以用已收的记录前缀/后缀内容匹配来定位 `delta`，但这是启发式；双端可控下 manifest 更简单、更可靠。

**洞长校验**：恢复记录的 `wire_len` 必须等于 `ooo_head_seq - rcv_nxt`（整条记录），或覆盖洞的前缀（多记录/子区间）。若 `wire_len > 洞长` 且记录起点就是 `rcv_nxt`，只注入洞长这一段前缀（记录剩余部分已在洞后，或等下一段）；若记录起点在 `rcv_nxt` 之前/之后，需要 `record_start_seq` 定位，见 §3.4。若 `wire_len < 洞长`，只注入这段前缀，`rcv_nxt` 前进后洞变小，等下一段恢复；若没有洞（`ooo` 空或 `ooo_head_seq == rcv_nxt`），不注入。

---

## 4. 发送端：记录边界与元数据

### 4.1 记录在 carrier 上的边界

- 发送端按记录写 carrier：`tbt_carrier_send_segs()` 一次最多 3 条记录（`TBT_BATCH_REC_MAX=3`），记录之间不交错；短写把尾部留在 `c->part`，下一次写前先 flush（`tbt_carrier.c:1449-1493`）。因此 carrier TCP 字节流就是记录的连续拼接，模块自己知道每条记录的 `[start, start+len)`，但**没有把 TCP seq 回填到记录**（`tbt_fec_out` 只有 `u16 carrier`，`tbtun.h:1208`）。
- 整条记录洞不需要回填 TCP seq：接收端用本地 `rcv_nxt`。子区间洞需要记录起始 seq（§3.4）；发送端在写记录前读 `tp->write_seq` 即可，批内前缀由记录顺序给出。发送端还需要保留**原始 carrier/gen** 和**记录顺序**，供 manifest/集合差使用。
- 发送端在 `tbt_fec_out_push()`/`tbt_fec_tx_try()` 时知道 `o->carrier`；在 `tbt_carrier_send_fec()` 成功后知道这条记录确实进了该 carrier 的 TCP 发送队列。此时可把 `(block_id, group, share, carrier, gen, record_start_seq, wire_len)` 记入 manifest 或块侧表。若记录在写 TCP 前被 `drop_ppm` 丢弃，则从未获得 TCP seq，也不会有洞；接收端“无洞不注入”自然排除。
- 重放（NACK/replay）可能把同一记录写到另一条 carrier；原始 carrier 仍是注入目标。manifest 必须保留**首次发送**的 carrier/gen，而不是重放时的 carrier。

### 4.2 发送端控制记录与数据记录的字节边界

- 控制记录（HELLO/OPEN/ACK/NACK/FBK/FIN）和 FEC share 共用 carrier 字节流，但控制记录不是 FEC 编码对象。它们丢失时没有 FEC 恢复，洞只能由 TCP 重传填；本设计只对 FEC share 的洞注入。
- FEC share 的边界由 48B 头和 `payload_len` 定界；HTTP profile 下再由 chunk 头尾包裹。接收端在解析时已经知道每条已收 share 的 wire 长度；丢失 share 的长度由块元数据（`dlen`/`short_len`/`S`）给出。整条记录洞时，“洞长 = 丢失 share 的 wire_len 之和”是可验证的；子区间洞时，洞长是某条记录 wire 的一个子区间，由 `record_start_seq` 定位（§3.4）。
- 发送端不需要额外写“记录长度”字段：`payload_len` 已在头里，chunk 头由 `tbt_http_chunk_hdr()` 确定性生成。

---

## 5. 注入路径：两条路线的对比与选择

### 5.1 路径 A：直接操作公开字段 + `__sk_mem_schedule` + `__tcp_send_ack`（推荐，已实测）

QEMU/KVM 7.2.9 原型（`/tmp/rcvq-probe/tbt_rcvq.c`）已验证：在 `lock_sock(sk)` 下填 `[rcv_nxt, ooo_seq)`，合并 ooo、推进 `rcv_nxt`/`rcv_wup`、发累计 ACK；随后到达的 100B 全重叠段被 D-SACK 丢弃，350B 部分重叠段只交付新增的 50B。生产路径不依赖 `tcp_send_rcvq()`、`tcp_rcv_established()` 或 kprobe：`struct tcp_sock` 的 `rcv_nxt`/`rcv_wup`/`out_of_order_queue`/`ooo_last_skb`/`rx_opt` 都是公开字段，`__sk_mem_schedule()` 与 `__tcp_send_ack()` 都已导出。实现见 `tbtun/tbt_rcvq.c` 的 `tbt_car_rcvq_apply()`。

在 carrier RX work（`tbt_car_rx_body`）已持有 `lock_sock(sk)` 的上下文中，按顺序做：

```c
/* 1. 重读并校验洞；inject_len <= gap */
tp = tcp_sk(sk);
if (READ_ONCE(sk->sk_state) != TCP_ESTABLISHED) return -ENOTCONN;
if (RB_EMPTY_ROOT(&tp->out_of_order_queue)) return -ENOENT;
gap = TCP_SKB_CB(rb_to_skb(rb_first(&tp->out_of_order_queue)))->seq -
      tp->rcv_nxt;
if (!gap || inject_len > gap) return -E2BIG;

/* 2. 构造接收队列 skb；不带 TCP header（不走 tcp_rcv_established） */
skb = alloc_skb(inject_len, GFP_KERNEL);
skb_put_data(skb, wire + delta, inject_len);
memset(TCP_SKB_CB(skb), 0, sizeof(struct tcp_skb_cb));
TCP_SKB_CB(skb)->seq = tp->rcv_nxt;
TCP_SKB_CB(skb)->end_seq = tp->rcv_nxt + inject_len;
TCP_SKB_CB(skb)->ack_seq = tp->snd_una;      /* 旧 ACK，不确认未发送数据 */
TCP_SKB_CB(skb)->tcp_flags = TCPHDR_ACK;
skb->ip_summed = CHECKSUM_UNNECESSARY;
if (atomic_read(&sk->sk_rmem_alloc) + skb->truesize > sk->sk_rcvbuf ||
    !sk_rmem_schedule(sk, skb, skb->truesize)) { kfree_skb(skb); return -ENOMEM; }
__skb_queue_tail(&sk->sk_receive_queue, skb);
skb_set_owner_r(skb, sk);
tp->bytes_received += inject_len;
WRITE_ONCE(tp->rcv_nxt, seq + inject_len);

/* 3. 精确排空 ooo 队头（只处理 seq == rcv_nxt 的整段） */
while ((skb = rb_entry_safe(rb_first(&tp->out_of_order_queue), ...))) {
    end = TCP_SKB_CB(skb)->end_seq;
    if (TCP_SKB_CB(skb)->seq != tp->rcv_nxt) break;
    if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) break;
    rb_erase(&skb->rbnode, &tp->out_of_order_queue);
    if (tp->ooo_last_skb == skb)
        tp->ooo_last_skb = rb_entry_safe(rb_last(&tp->out_of_order_queue),
                                         struct sk_buff, rbnode);
    __skb_queue_tail(&sk->sk_receive_queue, skb);   /* 已 charge，不重复 set_owner */
    tp->bytes_received += end - tp->rcv_nxt;
    WRITE_ONCE(tp->rcv_nxt, end);
}

/* 4. 删除已被累计 ACK 覆盖的 SACK block（复制 tcp_sack_remove()；static） */
if (tp->rx_opt.num_sacks)
    tbt_rcvq_sack_remove(tp);

/* 5. 立即累计 ACK，取消对端重传 */
tp->rcv_wup = tp->rcv_nxt;
tbt_rcvq_send_ack(sk, tp->rcv_nxt);   /* 7.2: (sk,nxt,0)；6.1..7.0: (sk,nxt) */
```

要点：

- **不依赖未导出符号**：`tcp_send_rcvq()`/`tcp_rcv_established()` 在部分内核未导出；`__sk_mem_schedule`（`EXPORT_SYMBOL`）与 `__tcp_send_ack`（`EXPORT_SYMBOL_GPL`）已导出。`tbt_rcvq.c` 用 `LINUX_VERSION_CODE` 适配 `__tcp_send_ack` 的 2/3 参数签名，覆盖 6.1/6.6/6.12/7.0/7.2/7.3。
- **内存记账**：`tcp_try_rmem_schedule()` 先检查 `sk_rmem_alloc + truesize <= sk_rcvbuf`，再 `sk_rmem_schedule()`（内部调导出的 `__sk_mem_schedule`）。这里显式做同样的检查；失败就丢弃修复，TCP 重传兜底。
- **不要用 `tcp_queue_rcv()` 搬 ooo skb**：它在 `tcp_data_queue_ofo()` 里已经 `skb_set_owner_r()` 计过 rmem；再用会重复计费。排空用 `__skb_queue_tail()`/`tcp_add_receive_queue()`，只推进 `rcv_nxt`/`bytes_received`。
- **排空最后一个 ooo 节点时必须更新 `tp->ooo_last_skb`**：否则 `tcp_data_queue_ofo()` 的精确相邻合并会解引用已释放的 skb（UAF）。擦除后取 `rb_last()`，空树则为 NULL。
- **推进 `rcv_nxt` 后要清 SACK**：`tcp_sack_remove()` 是 static，模块复制其逻辑（只删 `end_seq <= rcv_nxt` 的 `selective_acks`）；否则对端会继续 SACK/DSACK 已累计确认的区间，干扰 `sacked_out`/undo。
- **不要创建 `[copied_seq, rcv_nxt)` 内的洞**：`tcp_prune_queue()` → `tcp_collapse()` 对窗口内的不连续 skb 有 `BUG_ON(offset < 0)`（`docs/prior-art.md:845-848`）。注入只追加在 `rcv_nxt`，排空只移动 `seq == rcv_nxt` 的整段，窗口始终连续。
- **ACK 语义**：`ack_seq = tp->snd_una` 是旧 ACK，不确认本端未发送数据；`tp->rcv_wup = tp->rcv_nxt` 后再 `__tcp_send_ack()`，满足 `rcv_wup <= rcv_nxt`。迟到原段的 `end_seq <= rcv_nxt`，由 `tcp_data_queue()` 的 `TCP_OLD_DATA`/D-SACK 丢弃；若 `end_seq < rcv_wup`，先由 `tcp_sequence()` 的 `TCP_OLD_SEQUENCE` 丢弃。

### 5.2 路径 B：在 `tbt_car_rx` 层直接消费并推进/对齐 `rcv_nxt`

思路：不构造 skb，恢复的 wire 字节直接喂给模块解析器；同时把 `tp->rcv_nxt`、`tp->copied_seq`、`tp->bytes_received` 一起推进，再把 ooo 队头并入接收队列，最后 ACK。

优点：
- 不分配 skb、不 charge rmem、不经过 `tcp_send_rcvq()`，对内核内部函数的依赖最小。
- 恢复字节本来就是模块要解析的，直接进解析器可以少一次拷贝/入队。

缺点/约束：
- `copied_seq` 是 TCP 的读游标；把它推进到没有 skb 的字节上，要求模块是**唯一读者**，且解析器能一次消费完整条记录。若解析器因交付环满返回 `-EAGAIN`，只能不推进/回滚，否则 `[copied_seq, rcv_nxt)` 出现“没有 skb 的字节”，`tcp_collapse()` 可能 BUG。
- 需要新增 `tbt_rx_bytes(c, buf, len)` 直接喂解析器的入口，并把 `tbt_rx_skb()` 的解析逻辑抽出来复用。
- 注入与解析耦合，背压时恢复延迟；路径 A 把字节放进接收队列后，解析器可以按自己的节奏消费，TCP 状态已经前进。

**选择**：首版/产品化用**路径 A**。它把“TCP 状态推进”和“模块解析消费”解耦，符合内核已有的接收队列不变量；路径 B 作为后续优化，只在确认解析器能原子消费整条记录、且交付环有空间时启用。

### 5.3 `bh_lock_sock` 还是 `lock_sock`

- carrier RX work 是进程上下文（workqueue），已经在 `tbt_car_rx_body()` 里 `lock_sock(sk)`（`tbt_carrier.c:1277`），随后才 `tcp_read_sock()`（`:1302`）。注入应放在同一段 `lock_sock` 内、`tcp_read_sock` 之前，避免二次加锁。
- 入站软中断 `tcp_v4_rcv()` 用 `bh_lock_sock()`；当 socket 被 `lock_sock()` 持有时，新段进入 `sk_backlog`，不会并发执行 `tcp_data_queue()`。因此持 `lock_sock` 检查并修改 `rcv_nxt`/`out_of_order_queue` 是安全的。
- **不要**在 `bh_lock_sock`/softirq 下做这套手术：`alloc_skb(GFP_KERNEL)` 可能睡眠，`lock_sock` 也不能在 softirq 取。也不要在持 `rx->lock`（FEC 的 leaf spinlock，`tbtun.h:1028` 注释）时取 `lock_sock`。
- FEC 解码在 `w_fec_rx` 里运行，不能直接拿 carrier 的 `lock_sock`（会与 `rx->lock` 形成反向顺序）。正确做法：`w_fec_rx` 重建 wire 字节，推入 per-carrier 的 repair 队列（spinlock `irqsave`），然后 `tbt_carrier_arm_rx(c)` 唤醒 carrier RX work；由后者在 `lock_sock` 下应用。
- repair 队列要有界（建议 8 条/载体）；满了就丢弃最旧或拒绝新 intent，回退 TCP 重传。wire 缓冲 ≤8248 B，可用每载体预分配池（GFP_ATOMIC 只做指针入队）。
- 载体代次：intent 记录 `c->gen`；应用前检查 `c->gen` 未变、`sk_state == TCP_ESTABLISHED`、`c->sk` 仍指向同一 socket。载体被替换/关闭则丢弃 intent。

---

## 6. 最小实现方案（分阶段）

### 阶段 0：元数据与纯函数（不改 TCP 行为）

1. `tbt_fec_blk` 增加 `u8 src_carrier[TBT_FEC_MAX_SHARES]`、`u8 src_gen[TBT_FEC_MAX_SHARES]`（或按需的侧表），在 `tbt_fec_rx_record_body()` 放置 share 时记录 `c->index`/`c->gen`。
2. 新增纯函数（建议 `tbtun/tbt_rcvq.c`，host 可编译）：
   - `tbt_rcvq_rebuild_record(const struct tbt_fec_blk *, slot, group, out, cap, &len)`：按 §3.2 重建 48B 头 + 密文 + chunk 头尾。
   - `tbt_rcvq_plan(rcv_nxt, ooo_head, recovered[], n, carrier_map[], ...)`：输出 `{inject_len, carrier, seq}` 或 `skip_reason`。
   - `tbt_rcvq_ooo_drain_plan(...)`：纯逻辑，供 host 测试；内核侧执行。
3. host 测试先行：wire 重建与发送端 `tbt_fec_make_record()` + `tbt_http_fec_chunk_encode()` 逐字节比对。

### 阶段 1：单洞、单记录、DATA share（MVP）

1. `tbt_fec_rx_decode_one()` 在 `b->ready` 后，若块内恰好一个 DATA share 是恢复的、且集合差唯一确定 carrier：
   - 调 `tbt_rcvq_rebuild_record()` 重建 wire；
   - 推入该 carrier 的 repair 队列，记录 `(seq = 当前 rcv_nxt? 不能在这里读；只记 wire_len 和 carrier)`。**seq 在应用时读**，因为 `w_fec_rx` 与 carrier RX work 之间 `rcv_nxt` 可能已变。
   - `tbt_carrier_arm_rx(c)`。
2. `tbt_car_rx_body()` 在 `lock_sock` 后、`tcp_read_sock` 前调用 `tbt_carrier_repair_apply(c)`：
   - 取 intent；校验 `c->gen`、`sk_state`、`rcv_nxt`、ooo 洞长；
   - 调 `tbt_car_rcvq_apply()`：直接操作公开字段 + `__sk_mem_schedule()` + `__tcp_send_ack()`；
   - 精确排空 ooo 队头；
   - `__tcp_send_ack()`；
   - 计数器：`repair_injected/skipped_nohole/skipped_short/skipped_gen/rmem_fail`。
3. 解析器随后通过 `tcp_read_sock` 读到注入记录。`tbt_fec_rx_record()` 会把它判为重复（`dpresent`/`late`）并丢弃，不会二次交付；若想省掉重复解密/CRC，可在 carrier 上维护一个小的“已注入区间”ledger，解析到该区间时只消费、不喂 FEC（首版可不做，依赖现有重复门）。
4. 测试：单段丢失（netem/drop_ppm）、短 share、反向并发流量、RTO 前注入、迟到原段被 `TCP_OLD_DATA`/DSACK 丢弃、应用字节逐位一致。

### 阶段 2：PARITY、多记录洞、manifest、FEC_MAP 子区间

1. PARITY share 的 `win_len`/`epoch` 从块状态取；同样重建。
2. 多记录洞：用 `FEC_MAP`（§3.3）或按 `seq` 顺序 + wire_len 匹配；只有顺序/长度都无歧义才注入，否则回退。
3. 按内核矩阵（6.1/6.6/6.12/7.0/7.2/7.3）验证公开字段与 `__tcp_send_ack` 签名；加 `BUILD_BUG_ON` 固定结构布局/常量。
4. 可选：每 share 头携带 `carrier_id`（改 `TBT_FEC_CONV`），用于重放场景的原始 carrier 识别；若做，必须同时保留 `FEC_MAP` 或集合差作为丢失 share 的兜底。

### 原型骨架（只写进设计，不改内核）

```c
struct tbt_rcvq_intent {
    u8  carrier;                       /* c->index */
    u8  gen;                           /* c->gen at build time */
    u16 wire_len;                      /* full record wire length */
    u32 record_seq;                    /* TCP seq of wire[0]; 0 = unknown */
    u8  wire[TBT_HTTP_FEC_FRAME_MAX];  /* rebuilt record + chunk framing */
};

/* w_fec_rx: block ready, exactly one DATA share was recovered. */
int tbt_rcvq_build(struct tbt_session *sess, struct tbt_fec_blk *b,
                   u16 group, u16 slot, struct tbt_rcvq_intent *it)
{
    u16 plen = tbt_rcvq_slot_len(b, slot);      /* dlen / short_len / S */
    u8  rec[TBT_FEC_REC_MAX];
    struct tbt_fec_hdr h = {};                  /* fill per §3.2 table */
    int n;

    if (slot >= b->n) return -EOPNOTSUPP;       /* MVP: DATA only */
    h.type = TBT_FEC_DATA;
    h.share_index = slot; h.n = b->n; h.m = b->m;
    h.block_id = b->block_id; h.sid = b->sid; h.conv = TBT_FEC_CONV;
    h.reserved = group; h.payload_len = plen;
    h.seq = tbt_fec_data_seq(b->seq0, group, slot);
    /* DATA: h.epoch = h.win_len = 0. PARITY 阶段 2 再加。 */
    if (tbt_fec_hdr_seal(&h, b->data + (size_t)slot * TBT_FEC_S))
        return -EINVAL;
    tbt_fec_hdr_pack(&h, rec);
    memcpy(rec + TBT_FEC_HDR_LEN, b->data + (size_t)slot * TBT_FEC_S, plen);
    if (sess->crypto &&
        tbt_crypto_body_xor_flow(&sess->crypto_ctx.rx, h.sid, h.seq,
                                 h.type == TBT_FEC_PARITY, slot,
                                 rec + TBT_FEC_HDR_LEN, plen))
        return -EINVAL;
    n = tbt_http_fec_chunk_encode((char *)it->wire, sizeof(it->wire),
                                  rec, TBT_FEC_HDR_LEN + plen);
    if (n <= 0) return -EINVAL;
    it->wire_len = (u16)n; it->carrier = ...; it->gen = ...;
    it->record_seq = ...;              /* from FEC_MAP; 0 if unknown */
    return 0;
}

/* w_rx: already under lock_sock(sk). */
int tbt_rcvq_apply(struct tbt_carrier *c, struct sock *sk,
                   const struct tbt_rcvq_intent *it)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct rb_node *n = rb_first(&tp->out_of_order_queue);
    u32 seq = READ_ONCE(tp->rcv_nxt), hole_end;
    s32 delta = it->record_seq ? (s32)(seq - it->record_seq) : 0;
    u16 inject;

    if (!n) return -ENOENT;
    hole_end = TCP_SKB_CB(rb_to_skb(n))->seq;
    if (it->record_seq && (delta < 0 || delta >= it->wire_len))
        return -EAGAIN;                /* earlier record / already received */
    inject = min_t(u16, hole_end - seq, it->wire_len - delta);
    if (!inject) return -ENOENT;
    /* tbt_car_rcvq_apply(): alloc skb, queue at rcv_nxt, advance rcv_nxt,
     * merge ooo, clear SACK, __tcp_send_ack() */
    /* then exact-match ooo drain, ooo_last_skb, tcp_sack_remove, ACK, wake */
    return 0;
}
```

原型只实现 `DATA`、`slot < n`、`it->carrier == c->index`、`it->gen == c->gen` 的路径；`record_seq == 0` 时只接受 `delta == 0 && inject == hole_len` 的整条记录洞，`record_seq != 0` 时按 `delta` 注入子区间。其余返回 `-EOPNOTSUPP`，由 TCP 重传兜底。`record_seq == 0` 是“未知”哨兵；若真实 TCP seq 可能为 0，改用 `bool has_record_seq`。

### 代码触点

| 文件 | 改动 |
| --- | --- |
| `tbtun/tbtun.h` | `tbt_fec_blk` 加 `src_carrier/src_gen`；`tbt_carrier` 加 repair 队列、spinlock、计数器 |
| `tbtun/tbt_fec_data.c` | `tbt_fec_rx_record_body()` 记录来源 carrier/gen；`tbt_fec_rx_decode_one()` 生成 repair intent |
| `tbtun/tbt_carrier.c` | `tbt_car_rx_body()` 应用 repair；`tbt_carrier_free()` 清队列 |
| `tbtun/tbt_rcvq.c`（新） | wire 重建、planner、注入 helper、ooo 排空、ACK |
| `tbtun/tbt_uapi.h`/`tbt_fec.h` | 可选 `TBT_FEC_MAP` 类型/字段；`TBT_FEC_CONV` 升版 |
| `Makefile` | 新源文件；host 测试目标 |
| `host/test_rcvq.c`（新） | 纯函数/重建/属性测试 |

---

## 7. 单元测试设计

### 7.1 纯函数（host）

1. **wire 重建 golden**：
   - 全量 DATA（8192）：发送端 `tbt_fec_make_record()` + `tbt_http_fec_chunk_encode()` 得到 wire；模拟丢失，用块元数据 + 恢复 payload 重建，`memcmp` 必须为 0。
   - 短 share（`payload_len < S`，块内最后一个实 share）：`short_len`/`epoch` 路径；chunk 头长度变化。
   - PARITY：`win_len`/`epoch` 路径。
   - crypto 开/关：密文必须逐位一致；crypto 关时明文直通。
   - LAST：首版期望 `skip`；若块状态给出 LAST，则与发送端一致。
2. **planner 表驱动**：
   - 精确单洞单记录 → inject；
   - 洞比记录长 → 只注入前缀 / 或按阶段策略 skip；
   - 洞比记录短 → skip（重叠）；
   - `ooo` 空 / `rcv_nxt` 已前进 → skip；
   - 记录与已收字节重叠 → skip；
   - 子区间：`record_seq` 给出、`delta > 0`、`hole_len < wire_len` → 注入 `wire[delta .. delta+hole_len)`；
   - `record_seq` 未知 → 只接受整条记录洞（`delta == 0 && hole_len == wire_len`）；
   - 多记录、顺序无歧义 → 拼接注入；有歧义 → skip；
   - carrier 集合差唯一/不唯一；`gen` 不匹配；rmem 不足。
3. **属性/模糊测试**：
   - 随机生成 share 序列（含短包、PARITY、重放、丢失、乱序），模拟 carrier TCP 流和 repair planner；断言：最终交付流与发送流逐字节相同；没有字节交付两次；没有“无 skb 的 `[copied_seq, rcv_nxt)` 洞”；注入计数与丢失计数守恒。
   - 随机 `rcv_nxt`/`ooo_head` 的 32 位回绕边界。

### 7.2 内核集成（QEMU / 两模块）

1. 单 carrier 整条记录丢失（丢该记录的 6 个 MSS 段，或大 MSS 路径，使洞 == `wire_len`）：FEC 恢复后，注入必须在 carrier RTO 前完成；`tp->rcv_nxt` 前进、`tp->ooo` 排空、应用收到正确字节；迟到原段触发 `TCP_OLD_DATA`/DSACK；无重传风暴；`snd_una` 正常前进。
2. 短 share 丢失、PARITY 丢失、连续两条丢失（MVP 应 skip 或走 manifest）、重放到另一 carrier（manifest/集合差）、载体替换（skip）、接收缓冲满（rmem fail → skip，TCP 重传兜底）。
3. 真实 MSS 子区间洞（阶段 2）：丢一个 1460 B 段，`record_seq` 给出，`delta > 0`，注入 `wire[delta..]`；迟到原段按部分重叠/旧数据丢弃，应用字节正确。
3. 双向并发：反向流量持续时注入，确认反向字节不丢（这是历史 36/40 的回归测试；carrier 场景应 0 丢失）。
4. 锁/内存：`CONFIG_PROVE_LOCKING`、KASAN、`CONFIG_DEBUG_NET`；检查 `sock_owned_by_me`、`sk_rmem_alloc`、`tcp_collapse` 无 BUG。
5. 统计：`repair_injected/skipped_*`、`fec_dup_share`（注入记录被 FEC 判重）、`TCP_OLD_DATA`/DSACK 计数、RTO 次数。

---

## 8. 风险、边界与不做什么

1. **控制记录不受 FEC 保护**：HELLO/ACK/NACK/FBK/FIN 丢失没有可注入的恢复字节；它们的洞只能等 TCP 重传。本设计消除的是 FEC share 的队头阻塞，不是所有 carrier 字节的阻塞。
2. **`drop_ppm` 在写 TCP 前丢记录**：没有 TCP 洞，不注入；接收端“无洞不注入”自然覆盖。`docs/inject-probe.md` 的旧路径没有这个检查，是它丢字节的原因之一。
3. **载体替换/代次变化**：旧 carrier 的 TCP seq 空间已不存在；intent 的 `gen` 不匹配就丢弃。
4. **多记录洞/重放/子区间**：首版只做“洞 == 整条记录、记录起点 == rcv_nxt”的特例；真实 MSS 下丢失段通常只是记录的子区间，必须等阶段 2 的 `FEC_MAP` 提供 `record_start_seq` 后才能覆盖。不要为了“全都能补”而在歧义时猜 `delta`/顺序——猜错就是永久流损坏。
5. **不要碰 `tp->repair`**：注入不需要 repair；`repair=1` 会让应用非 PEEK 读 `-EPERM`、close 不发 FIN（`docs/inject-probe.md:151-164`）。carrier socket 没有应用读，但仍不应引入 repair。
6. **不要创建接收队列洞**：只在 `rcv_nxt` 追加，只排空 `seq == rcv_nxt` 的整段；重叠/部分段留给内核，不要自己 trim（`tcp_trim_head` 未导出，且容易破坏 `tcp_collapse` 不变式）。
7. **内核版本漂移**：公开字段（`rcv_nxt`/`rcv_wup`/`ooo_last_skb`/`selective_acks`）跨 6.1..7.3 稳定；`__tcp_send_ack` 在 7.2 增加 `flags` 参数，`tbt_rcvq.c` 用 `LINUX_VERSION_CODE` 适配。仍需在支持矩阵上跑集成测试，并用 `BUILD_BUG_ON` 固定结构布局/常量。
8. **安全边界**：FEC 头/payload CRC 不是密码学认证；双端可控是前提。不要在不可信 peer 上把“恢复字节”当可信输入。
9. **拥塞控制副作用**：迟到原段被 D-SACK 丢弃会让发送端看到一次伪重传；`__tcp_send_ack` 的累计 ACK 会尽快清除。对 carrier CC 是可接受的代价；若观测到明显退化，可在注入后标记/统计，而不是抑制 DSACK。
10. **不改变 FEC 交付路径**：恢复 share 的 payload 仍按现有 `tbt_rx_fec_deliver()` 交付；注入只修 carrier TCP 流，不改变隧道层的字节序。注入记录被 FEC 判重是预期行为，不要让它再次交付。

### 8.11 对“不要碰 `rcv_nxt`”保守结论的回应

一种保守评审意见是“只补诊断元数据，不碰载体接收队列”。它对 public socket 和“无精确字节”的注入是对的；对 carrier + FEC 精确字节，有三点需要分开看：

1. **方向不是“隧道→应用”**：`tbt_rx_fec_deliver()` 交付的是 share 的 **payload**，方向是隧道→应用；本设计注入的是 share 的 **原始 wire 记录**，方向是 peer→module。前者已经发生且只发生一次；后者只是把 peer 已经写出的 carrier 字节补回 carrier 的 TCP 序号空间。两者不是同一份字节，也不是同一个队列。
2. **洞在 TCP 序号空间，不在接收队列**：接收队列 `[copied_seq, rcv_nxt)` 确实无洞；洞是 `[rcv_nxt, ooo_head_seq)`，后续已到 share 在 `tp->out_of_order_queue`。注入填的是这个序号洞；填完后 ooo 队头才可能并入接收队列。没有这一步，`tcp_read_sock()` 永远走不到 ooo 里的字节。
3. **风险由前置条件兜底**：只有“单洞、单记录、wire 字节逐位重建、洞长匹配、`gen` 匹配、`rmem` 成功”同时成立才注入；任一不成立就 skip，回退 TCP 重传。这不是“无条件改写 `rcv_nxt`”，而是“在可证明的洞上重放可证明的字节”。保守方案（现状 + 诊断）仍是本设计在条件不成立时的行为。

如果评审认为连这个受控手术也不可接受，那么本设计应退化为“诊断 + NACK 归因”，载体 HOL 只能由 TCP 重传消除；这是产品取舍，不是原理上的不可能。

---

## 9. 验收标准

- 单 carrier 单 FEC share 丢失：注入在 RTO 前完成；`rcv_nxt` 前进、`ooo` 排空、应用字节逐位正确；迟到原段被 `TCP_OLD_DATA`/DSACK 丢弃；carrier 无多余重传。
- 双向并发下反向字节零丢失（回归历史 36/40）。
- host 测试覆盖 wire 重建（全量/短/PARITY/crypto/LAST）、planner 边界、随机属性；全部通过。
- 内核集成在 QEMU 上通过，KASAN/lockdep 无报告；`tcp_collapse` 无 BUG。
- 统计可解释：每次 skip 有明确原因（无洞/重叠/长度不符/gen 变化/rmem 失败/歧义）。

---

## 10. 实现与验证（本分支 w-rcvq-design）

- 模块：`tbtun/tbt_rcvq.h`（接口）、`tbtun/tbt_rcvq_wire.c`（wire 重建）、`tbtun/tbt_rcvq_plan.c`（carrier 集合差 + 洞计划）、`tbtun/tbt_rcvq.c`（repair 队列 + `tbt_car_rcvq_apply()`）。
- 集成：`tbt_fec_blk.src_carrier` 记录来源 carrier；`tbt_fec_rx_decode_one` 在块解码后重建并排队；`tbt_car_rx_body` 在 `lock_sock` 下先 `tbt_rcvq_apply_pending()` 再 `tcp_read_sock()`；`tbt_carrier` 持有有界 repair 队列，`tbt_carrier_free()` 清空。
- 测试：`host/test_rcvq.c` + `host/test_rcvq_wire.c` + `host/test_rcvq_plan.c`，253 checks/0 failures；`make host-test`、`make host-test-san`、`review/lint.sh` 全过；7.2.9 模块 `make` 通过（modpost/BTF）。
- MVP 边界：单缺失 DATA、整条记录洞（`hole == wire_len`）、非 LAST；PARITY、多记录、MSS 子区间（`record_seq`/`FEC_MAP`）为阶段 2；控制记录洞仍等 TCP 重传。

## 11. 一句话总结

历史否决的是“往 public socket 的接收队列写任意字节”；本设计做的是“往 carrier socket 的接收队列写 FEC 逐位还原的、序号正好等于本端 `rcv_nxt` 的原始 wire 字节”，并用精确的 ooo 排空和立即累计 ACK 把“同序号后到即丢弃”的内核语义变成消除载体队头阻塞与重传等待的机制。首版只做单洞单记录 DATA，其余用 manifest/回退逐步扩展。
