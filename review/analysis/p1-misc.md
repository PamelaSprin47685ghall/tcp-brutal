# P1 杂项：F5 半关闭、review 14 空洞门控、review 16 + F8 nonce/源绑定

本文只做静态分析与方案设计：不修改源文件、不编译、不跑 QEMU。行号取自
2026-11-05 07:30 的工作区；代码仍在被另一位工程师改动，定位以函数名为准，
行号只用于快速跳转。

## 0. 基线与证据

当前工作区的实测（`adversarial/results.txt`，模块 sha
`c18ff6c480cc1d88`，07:28；`adversarial/console-public.log` 07:27）：

| 用例 | 当前结果 | 本文对应 |
| --- | --- | --- |
| `pub.halfclose` | FAIL，`echo 10 bytes, EOF=0` | §1 |
| `far.srcaddr_free` | FAIL，同 nonce 从 127.0.0.2 在空闲 index 上 ACCEPTED | §3 |
| `far.anchor` | PASS，index 0 joined，gen 4 | §3 兼容性 |
| `far.dup_index` | PASS，同源同 index 被 `join_dup` 拒绝 | §3 兼容性 |
| `far.hello_garbage` | PASS，首个 HELLO 采纳 nonce 后垃圾字节关闭载体 | §3 采纳点 |
| `far.wrong_nonce` | PASS，错误 nonce 关闭载体 | §3 不变量 |

三项结论：

- **F5**：两个已观测根因——near 侧的 FIN 可能早于 `tbt_sock_attach()`
  被处理，`w_ctrl` 从未调度；far 侧把 `FIN_WAIT2 -> tcp_done()` 的
  `TCP_CLOSE` 当成 reset。FIN 发射点应移到 capture 工作项，判据改为
  `sk_err` + `RCV_SHUTDOWN`；复用 `TBT_F_FIN`，seq 取 drain 完成后的
  `tx_seq`。
- **review 14**：`data_frames++` 必须在写成功后（或 drop_ppm 的有意丢弃
  分支）自增，不能放在 `tbt_carrier_xmit()` 入口；门控的 `tail` 只做
  “已写未 ack”的扣除，不再依赖“已入队”。
- **review 16 + F8**：会话加源锚（配置的 far `peer=` 优先，否则首个通过
  nonce 的源），每条 HELLO 在 join 前查源；far 的 `peer=` 变成规范键，
  新增 `nonce=` 配置键用于预置钉住。TOFU 保留但告警并写清信任模型。

代码事实：`tbt_tx.c` 已有 `tbt_tx_fin()`（174 行）、`tbt_sock.c` 已有
`tbt_ctrl_work()`（277 行）和 `tbt_sock_shutdown_wr()`（181 行）、
`tbt_rx.c` 已有 FIN 的 `rx_fin_seen/rx_fin_seq`（328-345 行）。也就是说
F5 的“线缆格式和接收端 EOF 门”已经在代码里，缺的是**发送端的触发条件与
状态判别**，以及一个顺序竞态。

---

## 1. F5：应用 `shutdown(SHUT_WR)` 后对端收不到 EOF

### 1.1 应用半关闭在内核里的状态迁移

模块拥有的是公共连接的**被动端**（`kernel_accept()` 返回的 accepted
socket）；应用持有的是 loopback 连接的主动端（它自己的 client socket）。

1. 应用 `shutdown(SHUT_WR)`：应用的 client socket 发 FIN。
2. 模块的 accepted socket 收到 FIN：`tcp_fin()` 在
   `TCP_ESTABLISHED` 上执行 `tcp_set_state(TCP_CLOSE_WAIT)`，并在函数开头
   设置 `sk->sk_shutdown |= RCV_SHUTDOWN`，随后调用 `sk_state_change()`。
3. 反向（far 侧 target 连接）：模块在收到 near 的 FIN 后对 target 连接做
   `kernel_sock_shutdown(SHUT_WR)`，target socket 进入 `TCP_FIN_WAIT1`。
   target 读到 EOF 后关闭，其 FIN 到达：
   - 若 target 的 FIN 抢在本端 FIN 的 ACK 之前，状态 `FIN_WAIT1 -> CLOSING`；
   - 通常 ACK 先到，状态 `FIN_WAIT2`，`tcp_fin()` 调
     `tcp_time_wait(sk, TCP_TIME_WAIT, 0)`，后者最终 `tcp_done(sk)`：
     `tcp_set_state(TCP_CLOSE)`、`sk_shutdown = SHUTDOWN_MASK`、
     `sk_state_change()`。
4. 模块要做的“写方向结束”语义只有一件事：把 `RCV_SHUTDOWN` 翻译成一个
   `TBT_F_FIN`；它**不是** `TCP_CLOSE_WAIT` 这个状态名本身。

上述状态迁移来自 Linux 7.2.9 对应的 mainline 代码：
`net/ipv4/tcp_input.c` 的 `tcp_fin()`、`net/ipv4/tcp_minisocks.c` 的
`tcp_time_wait()`、`net/ipv4/tcp.c` 的 `tcp_done()` 与
`__tcp_read_sock()`。`tcp_fin()` 在 FIN_WAIT2 分支调
`tcp_time_wait(sk, TCP_TIME_WAIT, 0)` 后仍会走
`sk->sk_state_change(sk)`；`tcp_time_wait()` 末尾的 `tcp_done()` 先把状态
置为 `TCP_CLOSE`、`sk_shutdown = SHUTDOWN_MASK`，再调一次
`sk_state_change()`。因此回调看到的是 `TCP_CLOSE`，不是 `TIME_WAIT`。

当前代码有两个已观测的根因（a、b），各自都能独立造成 `EOF=0`；c、d、e
是同一路径上的潜在边界，必须一起设计，否则修好 a/b 后会在负载或异步
attach 下再次出现。

### 1.2 根因

#### F5-a：状态回调可能根本没装上，FIN 被静默吞掉（near 侧，先决）

`tbt_sock_attach()` 在 `tbt_sock.c:372-377` 安装
`sk->sk_state_change = tbt_pub_state_change`，而它只在 `kernel_accept()`
返回、`tbt_stream_start()` 运行后才执行。应用在 `connect()` 返回后立刻
`send()+shutdown()` 时，FIN 完全可能在 accept 工作项运行之前就被 TCP 栈
处理完：此时 accepted socket 已经 `CLOSE_WAIT`、`sk_shutdown` 已有
`RCV_SHUTDOWN`，但 `tcp_fin()` 调的是**默认回调**，`w_ctrl` 从未被调度。

`tbt_pub_capture_work()`（`tbt_sock.c:79-104`）随后会 `tcp_read_sock()`
把数据读走，但它只看数据，不看 `sk_shutdown`（95-103 行没有 FIN 检查）。
于是：near 不发 FIN → far 的 target 永远等不到 EOF → target 不关闭 →
far 不回 FIN → 应用读到 echo 的 10 字节后一直等到超时。这与
`pub.halfclose` 的观测完全一致；`tbt_stream_start()` 末尾虽然
`schedule_work(&st->w_read)`（`tbt_sock.c:443`），但 w_read 没有 FIN 分支。
在 1 vCPU 的 guest 上，应用在 `connect()` 返回后连续 `send()+shutdown()`
往往跑在 accept 工作项之前，这条路径很可能就是当前观测的起点。

#### F5-b：优雅的 `TCP_CLOSE` 被当成 reset（far 侧，FIN_WAIT2 路径必现）

`tbt_ctrl_work()`（`tbt_sock.c:277-296`）：

```
state = READ_ONCE(sk->sk_state);
if (READ_ONCE(sk->sk_err) || state == TCP_CLOSE) {
    tbt_tx_rst(...); tbt_stream_kill(...); return;
}
if (state == CLOSE_WAIT || LAST_ACK || CLOSING)
    tbt_tx_fin(...);
```

`FIN_WAIT2 -> tcp_time_wait() -> tcp_done()` 后状态就是 `TCP_CLOSE`，
`sk_err == 0`。于是 far 在 target 优雅关闭后发的是 `TBT_F_RST` 并杀流，
不是 FIN；near 的 accepted socket 被 `SHUT_RDWR` 重置，应用读到 echo 10
字节后拿到 `ECONNRESET`（测试只打印 `EOF=0`，看不出 errno）。即使
`FIN_WAIT1 -> CLOSING` 的分支碰巧命中（当前列表含 `CLOSING`），也只是
竞态下的偶然正确；loopback 上 target 的 ACK 通常先于它的 FIN，所以
FIN_WAIT2 是常见路径。修好 F5-a 之后，这条路径会成为下一个必现的 EOF
阻断点。

同一函数还有第二个问题：`FIN_WAIT1/FIN_WAIT2/TIME_WAIT` 都不在正列表里。
**状态枚举不是可靠的判据**；可靠判据是 `sk_err` 与
`sk_shutdown & RCV_SHUTDOWN` 的组合。

#### F5-c：FIN 的 seq 与抓取进度竞态（潜在，必须一起修）

`tbt_tx_fin()` 在 `tbt_tx.c:179-181` 取 `seq = st->tx_seq`；`tx_seq` 只在
`tbt_tx_submit()` 里推进（`tbt_tx.c:1664-1665`）。如果从状态回调里直接发
FIN，capture 工作项可能还没把最后几个字节读出来，FIN 的 seq 会**偏小**。
接收端 `tbt_drain_work()` 的 EOF 门是
`list_empty(rx_q) && rx_next >= rx_fin_seq`（`tbt_sock.c:252-258`）；
偏小的 FIN 会让 target 的写方向提前关闭，尾部字节被截断。当前 10 字节用例
可能因为 w_read 先跑而掩盖了它。

#### F5-d：FIN 是一次性 pinned 控制帧（潜在，和 review 22 交叉）

`tbt_tx_fin()` 只对调用时 `up` 的载体入队（`tbt_tx.c:182-192`）；一个载体
都没 up 时它返回成功但零拷贝。pump 在 `-EAGAIN` 时对 pinned 帧直接
`kfree(fr)`（`tbt_tx.c:1433-1437`）。单载体隧道在背压下可能丢 FIN，
EOF 永远不到。这不是本次 clean-link 验收的阻塞项，但 F5 不能宣称在背压下
完成；review 22（pinned 控制帧不丢）是它的前置。

#### F5-e：EOF 债在 socket 尚未 attach 时被标记为“已完成”（边界）

`tbt_drain_work()` 在门满足时先置 `rx_fin_done = true`，再调
`tbt_sock_shutdown_wr()`（`tbt_sock.c:252-274`）；后者在 `st->sk == NULL`
时直接返回（`tbt_sock.c:181-189`）。far 的 target 连接是异步 connect 的
（`tbt_target_work_fn`），如果 FIN 先到、target 后 attach，EOF 被记为
done 但从未施加。正常用例里 target 在会话建立时就已连上，所以不是本次
观测的主因，但必须在设计里堵住。

### 1.3 修复方案（函数级）

**单一 FIN 发射点：capture 工作项，而不是状态回调。**

1. `tbt_pub_capture_work()`：
   - 在 `lock_sock()` 内、`tcp_read_sock()` 之后读
     `sk->sk_shutdown` 与 `tcp_sk(sk)->copied_seq/rcv_nxt`；
   - 条件：`(sk_shutdown & RCV_SHUTDOWN) &&
     copied_seq == rcv_nxt && !tx_fin_sent && state != TBT_ST_DEAD`；
   - 满足时在 `release_sock()` 之后调 `tbt_tx_fin()`。`tcp_read_sock()`
     会把 FIN 所在的 skb 吃掉并把 `copied_seq` 推进过 FIN 的序号
     （内核 `__tcp_read_sock()` 的 `TCPHDR_FIN` 分支），所以
     `copied_seq == rcv_nxt` 正是“所有前序字节已提交、FIN 已消费”的判据。
   - 这个检查**不依赖** `sk_state_change` 是否装上，覆盖 F5-a；也覆盖
     正常 `CLOSE_WAIT` 路径。
2. `tbt_sock_attach()`：除 `w_drain` 外再 `schedule_work(&st->w_read)`。
   near 侧 `tbt_stream_start()` 已经调度 w_read；far 的
   `tbt_target_attach()` 没有，这一行让“attach 前已排队的 FIN/数据”
   也能被看到（顺带缓解 F6 的 far 输入停滞）。
   - 与 w_ctrl 的并发是安全的：workqueue 在开始执行时会清掉 pending 位，
     w_ctrl 在 w_read 运行期间再 `schedule_work()` 会排下一次执行；若
     w_read 只是“已排队未运行”，它稍后运行时也会看到已经设置的
     `RCV_SHUTDOWN`。不会出现“FIN 已到但没有任何一次 w_read 看到它”的
     丢失唤醒。
3. `tbt_ctrl_work()` 改为“只做 reset 判定和唤醒”：
   - `st->state == TBT_ST_DEAD` → 返回（死流不再发 FIN/RST）；
   - `sk_err != 0` → `tbt_tx_rst()` + `tbt_stream_kill()`；
   - 否则 `sk_shutdown & RCV_SHUTDOWN` → `schedule_work(&st->w_read)`，
     **不再在这里发 FIN**；
   - 否则 `state == TCP_CLOSE` → `tbt_tx_rst()` + kill（本端 abort 或对端
     没有 FIN 就消失）。
   - `sk_err` 必须排在 `RCV_SHUTDOWN` 之前：`tcp_reset()`/`tcp_done()` 也会
     把 `sk_shutdown` 置成 `SHUTDOWN_MASK`，reset 不能被误报成 EOF。
4. `tbt_tx_fin()`：
   - 调用者保证 capture 已 drain，函数内仍按 `sess->tx_lock` 读
     `st->tx_seq`；
   - 返回“实际入队的拷贝数”；0 拷贝时调用者不置 `tx_fin_sent`，留待载体
     up / FBK tick 重试；
   - 用每流 `tx_fin_mask`（bit = 载体 index）或每载体 `fin_sent` 标志避免
     重复；替换载体（新 gen）要允许再补一份；
   - `-EAGAIN` 时不要把 pinned FIN `kfree`：重新入队（有界）或清 mask 位，
     由下一次 pump/FBK 重试。这是 review 22 的交叉点，实施顺序见 §4。
5. `tbt_sock_attach()` 补“EOF 债”重放：若 `rx_fin_done` 且 `st->sk`
   存在，调一次 `tbt_sock_shutdown_wr()`；`kernel_sock_shutdown(SHUT_WR)`
   对已关闭写方向是幂等的。或者把 `rx_fin_done` 拆成
   “FIN 已收且字节已交”与“SHUT_WR 已施加”两个状态，attach 时补后者。
6. `tbt_drain_work()` 的 EOF 门保持原样；它是正确的：
   `list_empty(rx_q) && rx_next >= rx_fin_seq` 保证不截断。

### 1.4 不变量与边界

- **方向独立**：一条流有两个半关闭状态——本端 `tx_fin_*`（我方的写方向
  结束）和对端 `rx_fin_*`（对端的写方向结束）。FIN 只作用于所拥有 socket
  的**写**方向（`SHUT_WR`），读方向继续流动。两个 FIN 可以交叉。
- **远端 EOF 的施加**：接收端在 `tbt_drain_work()` 里先把 `rx_q` 中所有
  先序字节用 `tbt_sock_deliver()` 写完，门满足后调
  `tbt_sock_shutdown_wr()`，即 `kernel_sock_shutdown(sock, SHUT_WR)`。
  这是半关闭，不是 `close`：target 仍可继续发送剩余数据，near 应用仍可
  继续写；只有 `TBT_F_RST` 才双向中止。这条路径已经正确，F5 不需要改
  接收端，只需要保证发送端真的发出 FIN、且 seq 正确。
- **有序**：`FIN.seq == tx_seq`，且所有 `seq < FIN.seq` 的 DATA 已在 FIN
  之前入队/提交。接收端在 `rx_next >= rx_fin_seq` 之前不施加 EOF；空洞只会
  延迟 EOF，不会截断。
- **幂等**：每方向至多一个有效 FIN；重复 FIN 被 `rx_fin_seen` 忽略；发送端
  用 `tx_fin_sent`/mask 避免重复。`tbt_tx_fin()` 可被重试，但不得改变 seq。
- **reset 优先**：`sk_err != 0` 一律 RST，绝不 EOF；`RCV_SHUTDOWN` 且
  `sk_err == 0` 才是优雅 FIN。
- **死流沉默**：`TBT_ST_DEAD` 的流不再从 `w_ctrl` 发 FIN 或 RST。
- **完全关闭**：本端 FIN 已发且对端 FIN 已施加，才是双向结束；这是 F4
  （多公共连接/流回收）的清理点，不属于 F5。
- **不新造帧**：复用 `TBT_F_FIN`（0x14，空 body），不需要新 flag；RST 仍是
  唯一的 abort 语义。

### 1.5 协议影响

- 线上格式不变，`TBT_CONV` 不变；只是把已有的 FIN 语义补全。
- 需要写进 DESIGN 的规则：
  - FIN 的 seq 是“最后一个 DATA 之后的字节偏移”，由 capture 完成后取；
  - 接收端只在有序且队列空时施加 `SHUT_WR`；
  - FIN 与 RST 的判别是 `sk_err` vs `RCV_SHUTDOWN`，不是 TCP 状态名；
  - FIN 每方向幂等，允许重传/多载体拷贝。
- 对端（未来版本）若发 `DATA` 在 `FIN` 之后，属于协议违规；当前接收端
  以第一个 FIN 的 seq 为准，不做 `max()` 修补。

### 1.6 验收命令

```sh
# 公开端口组；当前 pub.halfclose FAIL，修好后应为 PASS
TBT_ADV_GROUPS=public adversarial/run.sh --no-build
# 期望：ADV_RESULT pub.halfclose PASS ... echo 10 bytes, EOF=1
```

guest 内直接跑：

```sh
timeout 30 /bin/adv_probe halfclose 19100
```

补充（建议新增的探针，当前不存在）：target 先发 10 字节再
`shutdown(SHUT_WR)`，应用读到 10 字节 + EOF 后继续写 10 字节，target 仍能
读到这 10 字节。它验证两个方向独立半关闭，且 far 侧不会把 `SHUT_WR` 升级成
`SHUT_RDWR`。

回归：

```sh
TBT_ADV_GROUPS=public adversarial/run.sh --no-build   # pub.rst 仍 PASS
# 控制台/dmesg 无 WARN/oops；clean close 不应再产生 TBT_F_RST
```

### 1.7 风险

- **`sk_err` 与 `RCV_SHUTDOWN` 的顺序**是正确性的关键；写反会把 reset 报成
  EOF。FIN 后紧接 RST 的极端序列，结果取决于两个 w_ctrl 的先后；可以接受，
  但要在文档里写明。
- **在 `TCP_CLOSE` 上 `tcp_read_sock()`**：F5-b 的路径会在 socket 已
  `tcp_done()` 后由 w_read 补读最后的数据。内核 `tcp_read_sock()` 只拒绝
  `TCP_LISTEN`，接收队列在 `tcp_done()` 时不被清空；但 `tcp_cleanup_rbuf()`
  可能尝试发 ACK。现有代码在 data_ready 与 FIN 的竞态里已经走到过这里，
  不是新增风险；仍需在 QEMU 里用 halfclose 反复跑确认无 WARN。
- **延迟 EOF**：capture 被 `tx_paused`（发送队列满）卡住时，FIN 会一直等到
  队列降到 `TBT_SQ_MAX/2` 后由 pump 重新调度 w_read（`tbt_tx.c:1567-1581`）。
  这是正确的背压，但对端应用可能等到自己的超时；需要在文档里说明。
- **pinned FIN 在 `-EAGAIN` 下丢失**：单载体 + 背压时 EOF 仍可能丢；F5 的
  clean-link 验收会过，但“背压下不缺 FIN”依赖 review 22。不要把它算进 F5
  的完成定义。
- **`tbt_sock_detach()` 恢复的是模块自己的回调**（`tbt_sock.c:390-394`），
  不是 stock 回调；socket 若比流活得久，回调仍会进模块但 `sk_user_data`
  为 NULL 会早退。F5 不改这一点，但新增的 `schedule_work(w_read)` 必须靠
  `sk_user_data != st` 的检查避免 detach 后重入（已有）。
- **target 已 `TIME_WAIT/CLOSE` 后 kill**：若后续又收到 RST，`tbt_stream_kill`
  会对已关闭 socket 调 `kernel_sock_shutdown`；无害，但可记录错误。

---

## 2. review 14：空洞门控把失败的写也计入

### 2.1 根因

`tbt_carrier_xmit()` 在函数入口就自增 `c->data_frames`
（`tbt_carrier.c:520-521`），位置在所有可能失败的写之前：

- drop_ppm 注入分支（528-544）；
- HTTP 容器：先写 header，再写 chunk（545-619）；
- raw：先冲刷上一帧的 `c->part` 尾巴，再 `tcp_sendmsg_locked()`
  （638-690）。

`-EAGAIN` 时 pump 会释放该路径的 reservation
（`fr->ret->reserve_mask &= ~BIT(c->index)`，`tbt_tx.c:1427-1432`），把帧
重新入队并设置 `avoid_mask`（1438-1442）；它**不会**设置
`r->path_mask`（path_mask 只在 xmit 成功后、`tbt_tx.c:1500-1510` 才置位）。
于是这条记录：

- 在 `data_frames` 里被算了一次；
- 不在 `path_mask` 里，因此也不在门控的 `tail` 里；
- 对端的 `recv_cnt` 没有它。

空洞门控（`tbt_tx.c:870-890`）算的是：

```
tail       = retain 中 seq+len > c->uack 且 path_mask 含本路径的记录数
sent_below = data_frames - tail
lag        = sent_below - recv_cnt
```

失败写让 `sent_below` 虚高，`lag > 0`，在干净链路上也会被误判为丢包，
进而 `holes_declared++`、注入重传，甚至触发有界重置。这就是 review 14。
HTTP 分支同样：header 写成功、chunk 写返回 `-EAGAIN`（612-614），记录已
在入口计数但没发出。

完整返回链：

```
tcp_sendmsg_locked() -> -EAGAIN
  -> tbt_carrier_send() 原样返回（HTTP 分支 462-463、612-614；
     raw 分支 653/655/665）
  -> tbt_carrier_xmit() 原样返回 -EAGAIN
  -> tbt_tx_pump() 1418-1442：释放 reserve_mask、dbg_pump_hiwat++、
     unpinned 帧 avoid_mask 后重新入队 / pinned 帧 kfree
  -> 下一轮 pump 在别的路径重试
```

入口的 `data_frames++` 就发生在这一整条链之前，所以失败写也被计了一次。

反例（必须保留的语义）：

- **短写**（`err != total`，671-690）返回 0，记录尾部存进 `c->part`，
  下一轮继续写。这条记录**应该**计数：它的字节已经承诺给该载体的 TCP
  字节流，`first_sent_seq` 也把 `c->part_len` 算进去了
  （`tbt_tx.c:1516-1518`），在 ack 之前它一直在 `tail` 里。
- **drop_ppm**（528-544）返回 0 但什么都没写。注入器的契约是“当作线上
  丢了”（注释 536-542），所以它**必须**计入 `data_frames`，否则探针 G
  的注入丢包对门控不可见，`holes_declared` 会假性为 0。

### 2.2 修复方案（精确修改点）

1. 删除 `tbt_carrier.c:520-521` 的入口自增。
2. 在三个“已提交”点自增（仅 `TBT_F_DATA`）：
   - drop_ppm 分支 `return 0` 之前（543 行前）；
   - HTTP 成功分支，`c->tx_frames++`（615 行）旁边；
   - raw 成功分支，`c->tx_frames++`（691 行）旁边。
3. `tx_frames` 语义保持“字节已交给 TCP 的记录数”；drop 分支不增加
   `tx_frames`（注入器没写）。`data_frames` 的字段注释
   （`tbtun.h:480`）改成：“已提交到本路径字节流的 DATA 记录，或注入器
   有意丢弃的记录；绝不包括排队中、EAGAIN 或报错的写。”
4. 在门控计算处（`tbt_tx.c:870` 附近）补一条不变量注释：
   凡计入 `data_frames` 的记录，要么已在 TCP 字节流里（含 `c->part` 的
   短写尾巴），要么是注入器的有意丢失；`tail` 负责减去尚未 ack 的部分。
   仍在 `sess->tx_q` 或被 EAGAIN 重排队的记录永远不计。
5. 计数器写入用 `WRITE_ONCE(c->data_frames, c->data_frames + 1)`，门控读用
   `READ_ONCE`；pump 由 `pump_lock` 串行化，只有一个写者。
6. 门控不再读取 `sess->tx_q`：它只读“已提交”的 `data_frames` 和
   “已写未 ack”的 `tail`。`tail` 是扣除项，不是入队项；这正是“已发出”
   与“已入队”的分界。

### 2.3 不变量与边界

- `data_frames(i)` 只统计“到达载体 i 字节流”的 DATA 记录（或注入器的
  有意丢弃）；排队、EAGAIN、错误一律不计。
- `recv_cnt(i)` 是接收端在路径 i 上收到的 DATA 记录数
  （`tbt_rx.c:171-176`）。
- 对一条计入 `data_frames(i)` 且 `seq+len <= c->uack` 的记录：要么它确实
  从 i 到达（计入 `recv_cnt`），要么 i 丢了它；差值才是确认丢失。
- 短写只计一次；尾部在 `c->part` 里仍属于这条记录的字节流。
- HTTP header 已写、chunk 未写：记录不计；下一次写 chunk 时 header 不会
  重发（`http_started` 已置位）。
- drop_ppm 是唯一“没写但计数”的例外；它是 debug-only，且门控依赖它。

### 2.4 协议影响

- 无线上变化。`/proc/net/tbtun/peers` 展示的是 `tx_frames` 而不是
  `data_frames`；`retx_ppm` 的分母不变。行为变化只在门控：干净链路上
  `holes_declared` 不再因 EAGAIN 增长，`rx_lag_ppm` 归零。
- 如果团队希望注入丢包也进入 `retx_ppm` 的分母，应在 drop 分支同时增加
  `tx_frames`；这超出 review 14，且会改变 peers 展示和测试预期，建议不做。

### 2.5 验收命令

```sh
# 干净链路 + 背压：先记录，再跑 pub.writer，再记录
grep -E 'holes_declared|hole_checks|gate_fail|dbg_pump_hiwat|carrier_short' \
    /proc/net/tbtun/stats
TBT_ADV_GROUPS=public adversarial/run.sh --no-build
# 期望：dbg_pump_hiwat/carrier_short 可以 >0（确有背压），
#       holes_declared 不增长，quiesce 后 peers 的 rx_lag_ppm 为 0。
```

注入丢包回归（证明 drop 分支仍然计数）：

```sh
# debug_loss=1 的模块
echo 'carrier 0 drop_ppm 100000' > /proc/net/tbtun/peers
# 跑一段传输；期望 holes_declared > 0，retx_frames 增长。
```

### 2.6 风险

- 漏掉 drop_ppm 分支的自增，会让探针 G 的注入丢包变成“假通过”
  （`drop_ppm>0` 但 `holes_declared=0`）。代码里要保留注释。
- 短写必须在 `c->part` 存好之后再计数；如果提前到 `tcp_sendmsg_locked()`
  返回处，尾部丢失时会计入一条没提交完的记录。
- HTTP 有两次写；只在 chunk 写成功后计数，不能因为 header 成功就计数。
- 门控读与 pump 写并发：pump 是唯一写者；读侧可能看到自增前后的值，`tail`
  的修正在“已 ack”方向保证不会误报，在“未 ack”方向最多让门控更保守。
- 不要顺手改 `recv_cnt` 或门控阈值；这不是 review 14 的范围。

---

## 3. review 16 + F8：首个 HELLO 的 nonce 抢占与源地址绑定

### 3.1 现状

字段（`tbtun.h:315-317`）：

- `nonce[16]`：会话 key，HELLO 明文携带；
- `nonce_set`：far 侧“已有一个固定 nonce”（采纳或钉住）；
- `nonce_pinned`：显式钉住。

产生与配置：

- 会话创建时 `get_random_bytes(s->nonce)`（`tbt_session.c:683`），
  `nonce_set = false`。near 直接用它发 HELLO（`tbt_carrier.c:1189`）。
- far 在第一条 HELLO 上采纳：`tbt_rx.c:463-479`。采纳分支只检查
  `t->cfg.peer_addr`（470-471）；但 far 的规范配置里没有 `peer=`
  （渲染器对 far 省略 peer，`tbt_parse.c:891-898`；host 测试明确断言
  “far line normalizes peer away”，`host/test_parse.c:497-505`、
  `830-840`）。因此 `cfg.peer_addr` 通常是 0，**任何源**的首条 HELLO
  都能定义 nonce（review 16）。
- `nonce_pinned` 目前只能由运行时动作 `session <id> nonce=<hex>` 设置
  （`tbt_parse.c:688-710`、`tbt_session.c:410-430`）；没有 tunnel 配置键。
  会话一创建载体就可能连上，运维来不及在下一条命令里钉住。
- 后续 HELLO 只比较 nonce（`tbt_rx.c:480-485`），**不比较源地址**；
  `tbt_session_join()` 只检查 index 占用/重复（`tbt_session.c:797-833`）。
  所以同一 nonce 从 127.0.0.2 来、落在空闲 index 上会被接纳（F8，
  `far.srcaddr_free` ACCEPTED）。`far.srcaddr_dup` 之所以 PASS，只是因为
  index 0 被占（`join_dup`），不是因为源检查。
- `cfg.srcaddrs` 对任何 role 都解析（`tbt_parse.c:323-357`），但只被
  near 用作本地 bind 地址 `[0]`（`tbt_carrier.c:1089`）；far 上是死配置。

### 3.2 修复方案

**主修复：会话级源锚（source anchor），配置优先、首连兜底。**

1. `struct tbt_session` 增加：
   - `__be32 anchor_addr;`
   - `bool anchor_set;`
   - `u64 join_src_reject;`
2. 会话创建（`tbt_session_create`）：
   - 若 `cfg.peer_addr` 非 0（far 允许配置），
     `anchor_addr = cfg.peer_addr; anchor_set = true;`
   - 若 far 且既没有 `cfg.nonce_set` 也没有 `cfg.peer_addr`，
     `pr_warn_once("far tunnel %llu uses trust-on-first-use; configure nonce= or peer= ...")`。
3. far 的 HELLO 准入（`tbt_rx_hello`，`tbt_rx.c:449-490`）在
   `sess->lock` 下按以下顺序：
   1. `if (!c->sk) reject;`
   2. `src = inet_sk(c->sk)->inet_daddr;`
   3. `if (anchor_set && src != anchor_addr)` → `join_src_reject++`，
      `-EPROTO`。**对所有 index 生效，包括空闲 index**；先于 nonce 检查，
      陌生源连 nonce 都探不到。
   4. `if (nonce_pinned)`：nonce 必须精确匹配，否则
      `session_join_bad++` / `-EPROTO`。
   5. `else if (!nonce_set)`：若 `cfg.peer_addr` 已配置，第 3 步已保证
      源匹配；采纳 nonce，`nonce_set = true`。若未配置，则 TOFU：采纳
      nonce，并设 `anchor_addr = src; anchor_set = true`。
   6. `else`：nonce 必须匹配，否则 `session_join_bad++` / `-EPROTO`。
   7. `if (!anchor_set) { anchor_addr = src; anchor_set = true; }`
      （只在 nonce 检查通过后设置）。
   8. 最后才 `tbt_session_join()`。
4. `tbt_session_join()` 加防御性断言：`anchor_set && src == anchor_addr`，
   否则 `-EPROTO`。当前只有 `tbt_rx_hello` 调用它；断言让未来调用者
   失败关闭（fail closed），也保证“检查 + 入座”不会漏。
5. 拒绝必须发生在 `tbt_session_join()` **之前**，避免副作用：
   不 `tbt_carrier_open_target()`、不回 far 的 HELLO、不安装载体。

**配置面：让锚可见、可预置。**

6. far 的 `peer=` 升级为规范键：
   - 运行时已经用它做采纳检查（`tbt_rx.c:470-471`），只是渲染器把它丢了；
   - 让 `tbt_render_cfg()` 对 `role=far` 也输出 `peer=addr:port`
     （`tbt_parse.c:891-898`），并在 DESIGN §8.3/§9.5 写明：far 的
     `peer=` 表示“唯一允许加入载体的 near 源地址”，端口被忽略；
   - 为什么不用 `srcaddrs=`：它已被文档定义为 near 的本地 bind 地址，
     far 示例里填的是 far 自己的地址；改成入站白名单会静默改变现有配置的
     含义，且 `srcaddrs[1..]` 在 near 也没被使用。v0 的 near 只用
     `srcaddrs[0]`（`tbt_carrier.c:1089`），所以单锚足够。
   - 为什么不用“纯首连地址”：纯 TOFU 仍让第一个连接（review 16）定义
     锚；配置优先 + 首连兜底才同时解决 review 16（配置了 peer/nonce 时）
     和 F8（未配置时也拒绝换源）。
7. 增加 tunnel 配置键 `nonce=<16|32 hex>`（复用 `tbt_parse_nonce`）：
   - 解析进 `struct tbt_cfg`（`u8 nonce[16]; bool nonce_set;`）；
   - `tbt_session_create()` 若配置了 nonce，拷贝并置
     `nonce_pinned = nonce_set = true`；否则保持随机/TOFU；
   - 规范渲染输出 `nonce=`（`cat config` 是 0600 root；nonce 在 HELLO 里
     本来就明文，不是密码，是会话标识/反注入 token）；
   - 运行时 `session <id> nonce=` 保留，供测试和轮换。
   - 是否强制：若要在 0.0.1 就“关闭” review 16 而不是只文档化，可在
     `tbt_cfg_sane()` 里要求 `role=far` 至少配置 `nonce=` 或 `peer=`，
     否则 `EINVAL`。代价是现有测试配置（`adversarial/init` 的 far 行、
     QEMU 脚本、`host/test_parse` 的 far 用例）都要补一行；而
     `far.hello_garbage` 的语义正是“首连采纳 nonce”，强制钉住后必须把
     测试改成先配置测试 nonce。建议 0.0.1 先做“告警 + 文档 + 可配置”，
     下一版再考虑强制。
8. 计数：新增 `join_src_reject`，加进 `/proc/net/tbtun/stats` 的
   `join_dup/join_bad/join_nonce_reject` 一行（`tbt_procfs.c:549-564`），
   并更新 DESIGN 的 stats 说明。
   - `far.srcaddr_dup` 之后会计入 `join_src_reject` 而不是 `join_dup`
     （测试只断言 close，不断言计数，仍 PASS）；
   - `far.dup_index` 同源，仍然 `join_dup`。

**兼容性核对：**

- `far.hello_garbage`：127.0.0.1 的首条 HELLO 采纳 nonce 并设锚；载体随后
  因垃圾字节被 stop，锚是会话属性，**不随载体死亡清除**。
- `far.anchor`：127.0.0.1 的 index 0 与锚一致，且原 occupant 已 dead，
  `tbt_carrier_dead()` 允许替换 → PASS。
- `far.dup_index`：127.0.0.1 同 index 0，锚通过，`join_dup` 拒绝 → PASS。
- `far.srcaddr_free`：127.0.0.2 在 index 1，锚不匹配，`join_src_reject`
  拒绝 → `read_hello` 拿不到 HELLO → `accepted=0` → PASS。
- `far.wrong_nonce`：同源、错 nonce，nonce 检查拒绝 → PASS。

### 3.3 不变量与边界

- nonce 每会话至多采纳一次；采纳要求源匹配已配置的 `peer=`（若有），
  否则是 TOFU。
- 锚一旦设置，**任何 index、任何后续 HELLO** 都必须来自该源地址；
  有效 nonce 也不能绕过。
- 只有通过 nonce 检查的 HELLO 才能设置锚；被拒绝的 HELLO 不改变锚。
- 锚是会话属性，跨载体死亡/替换存活；`join_dup` 不清锚。
- `nonce_pinned` 下 nonce 必须 128 位精确匹配；16 hex 形式是“低 64 位，
  高半零”，只在测试/复现时使用。
- 源检查在 `sess->lock` 下、`tbt_session_join()` 之前；拒绝无副作用。
- nonce 不是密码：HELLO 明文携带；真正的抗重放是源绑定，真正的机密性靠
  WireGuard。文档必须直说。

### 3.4 协议影响

- 无线上变化，`TBT_CONV` 不变；HELLO 已带 nonce/index。
- 配置语法变化：
  - 新增 `nonce=`；
  - far 的 `peer=` 从“解析但渲染丢弃”变成“规范、可回读、有语义”；
  - 规范 far 行会多出 `peer=`/`nonce=`，`TBT_LINE_MAX`（1024）预算要
    重新验证；最大行测试 `host/test_parse.c:383-391` 必须重跑。
- 行为变化：
  - 配了 `peer=` 的 far 对所有 join 都做源检查（以前只检查首条）；
  - 未配 `peer=` 的 far 以首个通过 nonce 的源为锚，换源被拒（F8 修复）；
  - 未配 `nonce=`/`peer=` 的 far 仍是 TOFU，但会告警；严格部署必须配
    至少一个。
- `far.srcaddr_dup` 的计数从 `join_dup` 变为 `join_src_reject`；
  `far.dup_index` 不变。

### 3.5 验收命令

```sh
# far 组；当前 far.srcaddr_free FAIL，修好后应为 PASS
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
# 期望：far.srcaddr_free PASS ... refused
#       far.anchor PASS / far.dup_index PASS / far.wrong_nonce PASS
#       far.hello_garbage PASS
```

配置面：

```sh
# 钉住 nonce：错 nonce 的首连必须被拒
echo 'tunnel 1 role=far listen=127.0.0.1:19101 target=127.0.0.1:19070 nonce=<32hex> n=4' \
    > /proc/net/tbtun/config
# 用 adv_probe 发一个不同 nonce 的 HELLO；载体关闭，join_bad 增长

# 钉住源：127.0.0.2 即使 nonce 正确也必须被拒
echo 'tunnel 1 role=far listen=127.0.0.1:19101 target=127.0.0.1:19070 peer=127.0.0.1:1 n=4' \
    > /proc/net/tbtun/config
# 从 127.0.0.2 发正确 nonce；载体关闭，join_src_reject 增长

# 规范配置回读：peer=/nonce= 必须保留
cat /proc/net/tbtun/config > /tmp/c
echo flush > /proc/net/tbtun/config
cat /tmp/c > /proc/net/tbtun/config
grep -E 'nonce=|peer=' /proc/net/tbtun/config
```

host 测试：

```sh
make check          # 更新 far peer 渲染断言，新增 nonce 解析/渲染用例
make host-test-san
```

### 3.6 风险

- **向后兼容**：没有 `peer=`/`nonce=` 的 far 仍 TOFU；首个通过 nonce 的
  源成为锚。near 换源（NAT rebinding、多宿主）会被拒；运维必须更新锚
  （重建 far 会话）或配置新地址。文档要写清。
- **旧备份**：修复前的 `cat config` 会把 far 的 `peer=` 丢掉（渲染器
  省略）；用旧备份恢复会失去锚。修复后的新备份才包含它。
- **`peer=` 的端口**：far 的锚只需要地址，但语法要求非 0 端口；端口被
  忽略。若觉得别扭，后续可加 `anchor=<ipv4>` 专用键；当前选 `peer=`
  是因为 DESIGN §9.5 已经这么写、代码也已经读它。
- **多宿主 near**：v0 的 near 只用 `srcaddrs[0]` 拨号，单锚足够；如果以后
  near 真的轮换源地址，单锚会拒掉合法载体，需要把锚扩展成列表。
- **nonce 明文**：on-path 攻击者能读到 nonce；源绑定只能挡不能伪造 TCP
  源的 off-path 重放。要机密性就套 WireGuard。
- **运行时 nonce 轮换**：`session <id> nonce=` 在已有载体加入后覆盖
  nonce，未来载体必须用新值；已有载体不受影响。文档要写明顺序，或考虑
  在 `nonce_set` 后拒绝该动作（除非显式轮换）。
- **stats/测试**：新增 `join_src_reject` 会改 stats 行；`far.srcaddr_dup`
  打印的 `join_dup` 增量会变成 0。更新 host 测试和 DESIGN 的 stats 列表。
- **行宽**：`nonce=`/far `peer=` 可能把最大规范行推过 1024；重跑
  `maximal render fits TBT_LINE_MAX`，必要时提高 `TBT_LINE_MAX`（同时改
  procfs linebuf 的边界）。
- **竞态**：两个不同源的首条 HELLO 并发；`sess->lock` 串行化，先到者设锚，
  后到者被拒。钉住 nonce 时只有正确者能通过；TOFU 时先到者赢，这是文档化
  的残余风险。
- **防御性断言**：`tbt_session_join()` 的 `anchor_set` 断言必须允许“首个
  join 前锚已由准入函数设置”；否则 `far.anchor` 会失败。实现时把设锚放在
  join 之前，并加测试。

---

## 4. 三项之间的关系与实施顺序

### 4.1 共享代码

| 项 | 主要文件 | 锁/状态 | 与其它项的交集 |
| --- | --- | --- | --- |
| F5 | `tbt_sock.c`、`tbt_tx.c`、`tbtun.h` | `sess->tx_lock`、socket lock、`w_read`/`w_ctrl` | 与 review 14 共享“pump 在 EAGAIN 下如何处理帧”的边界；与 nonce 只共享 `tbtun.h` 字段 |
| review 14 | `tbt_carrier.c`、`tbt_tx.c`（注释）、`tbtun.h`（注释） | pump 的 `pump_lock` 串行化 | 不改 FIN/HELLO；只改计数器位置 |
| nonce/F8 | `tbt_rx.c`、`tbt_session.c`、`tbt_parse.[ch]`、`tbtun.h`、`tbt_procfs.c` | `sess->lock` | 与 F5/14 无共享锁；只共享 `tbtun.h` 和 stats 行 |

结论：**没有真正的共享函数**。F5 的 FIN 与 seq 在 `tbt_tx.c`/`tbt_sock.c`；
nonce 与 session 锁在 `tbt_rx.c`/`tbt_session.c`。唯一需要协调的是
`struct tbt_session`/`struct tbt_stream` 的新字段和 stats 行，避免合并冲突。

### 4.2 顺序建议

1. **review 14 先做**。它最小（一个函数内的计数位置），没有协议/配置
   变化，而且能先消除“假空洞 → 误重传/有界重置”的干扰；否则 F5 的验收
   可能被门控误杀混淆。
2. **F5 第二**。它是用户可见的最高严重度（正常 half-close 挂起），但需要
   仔细处理 `sk_err`/`RCV_SHUTDOWN` 的顺序、capture 的 drain 条件、以及
   FIN 的幂等/重试。先做 14 再跑 `pub.halfclose`，结果更干净。
3. **nonce/F8 第三**。它改配置语法和 host 测试，独立于数据路径；放在最后
   可以让前两项先用现有配置跑完对抗组。若人手允许，它可以和 14 并行
   （文件不重叠），F5 也可并行，但 `tbtun.h` 由一个人统一合入。

F5 的“背压下 FIN 不丢”依赖 review 22（pinned 控制帧在 EAGAIN 下保留），
不在这三项里；如果 22 还没做，F5 的完成定义应限定为 clean link，并在
文档里标注依赖。

### 4.3 最终验证（由主 agent 统一跑一次）

```sh
source ~/.cache/tbtun-build/env.sh && make check
bash review/lint.sh
TBT_ADV_GROUPS=public adversarial/run.sh --no-build
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
# 期望：pub.halfclose PASS（EOF=1）、far.srcaddr_free PASS（refused）、
#       far.anchor/far.dup_index/far.wrong_nonce/far.hello_garbage PASS、
#       pub.rst PASS、无新增 WARN/oops。
```

`public` 组里的 `pub.churn`/`pub.writer`、`far` 组里的 `far.data_path`
分别属于 F4、稳定性/F6 的任务，不在本文三项的验收范围；比较结果时只看
本文列出的行，不要把它们的既有 FAIL 当成回归。

不要在每个小改动后重复构建/QEMU；构建和 QEMU 当前由另一位工程师占用。

---

## 5. 根因行号索引

| 位置 | 作用 | 问题 |
| --- | --- | --- |
| `tbt_sock.c:79-104` | capture：`tcp_read_sock` 后不检查 `RCV_SHUTDOWN` | F5-a：attach 前到的 FIN 永远不发 |
| `tbt_sock.c:277-296` | `tbt_ctrl_work` | F5-b：`TCP_CLOSE` 一律 RST；状态列表漏 FIN_WAIT/TIME_WAIT |
| `tbt_sock.c:181-189` | `tbt_sock_shutdown_wr` | F5-e：`st->sk == NULL` 时静默返回，EOF 债丢失 |
| `tbt_sock.c:252-274` | `tbt_drain_work` EOF 门 | 逻辑正确；依赖 FIN seq 正确 |
| `tbt_sock.c:372-379` | `tbt_sock_attach` | 只调度 w_drain；不调度 w_read；不重放 EOF 债 |
| `tbt_tx.c:174-195` | `tbt_tx_fin` | F5-c：seq 取 `tx_seq`；F5-d：无 up 载体时零拷贝 |
| `tbt_tx.c:1664-1665` | `tx_seq` 推进 | 只在 capture 中推进，FIN 必须等它 |
| `tbt_tx.c:1421-1442` | pump 的 EAGAIN | pinned 帧被 `kfree`；unpinned 帧释放 reservation |
| `tbt_tx.c:870-890` | 空洞门控 | review 14：`data_frames` 虚高导致 `lag` 假阳性 |
| `tbt_carrier.c:520-521` | 入口 `data_frames++` | review 14 根因 |
| `tbt_carrier.c:528-544` | drop_ppm 分支 | 必须保留 `data_frames++` |
| `tbt_carrier.c:612-619` | HTTP 成功点 | 应在此计 DATA |
| `tbt_carrier.c:691-695` | raw 成功点 | 应在此计 DATA |
| `tbt_rx.c:449-490` | far 的 HELLO 准入 | review 16/F8：采纳不绑源、后续不查源 |
| `tbt_session.c:797-850` | `tbt_session_join` | 只查 index，不查源 |
| `tbt_session.c:683` | 随机 nonce | far 未 pin 时会被首连覆盖 |
| `tbt_session.c:410-430` | 运行时 nonce pin | 唯一现有 pin 途径 |
| `tbt_parse.c:891-898` | far 渲染省略 peer | 锚不可见、备份丢失 |
| `host/test_parse.c:497-505,830-840` | far peer 断言 | 需要随配置面修改 |
| `tbtun.h:315-317` | nonce 标志 | 新增配置键时复用 |
| `tbtun.h:480` | `data_frames` 注释 | 语义需改写 |
| `tbt_procfs.c:549-564` | stats 行 | 新增 `join_src_reject` |
