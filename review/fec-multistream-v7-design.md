# FEC 多流 v7 设计：按 carrier group 分区的每流 FEC 平面

状态：只读设计，未改代码。基线 HEAD `00d34cf`（0.2.3 + 自适应 FEC phase 1b）。
目标：一个 tunnel/session 内允许多条 live app stream，共享 256 条 carrier；每条流的 FEC 块、窗口、交付、NACK、credit、FIN 相互隔离；单流时保持现有 256-carrier 行为不变。
范围：FEC v6/v7 数据面、会话流生命周期、wire 版本、SC-LDPC 接口缝。不在此范围：SC-LDPC 实现（仓库内不存在）、真机测试。
标记：`[事实]` 代码或文档；`[计算]` 由常量推导；`[设计]` 本方案的选择；`[待测]` 需要 host/QEMU 验证。

---

## 0. 结论先行

1. **单流限制不在线格式，而在会话状态。** 0.2.2 起 FEC 48 字节头在 +32 已有 `u64 sid`（`tbt_uapi.h:181-184`、`tbt_fec.h:59-61`），DATA/PARITY/ACK/NACK 实际都写 sid（`tbt_fec_data.c:1493,1256,1309,2522,2549`）；主路径 OPEN/FIN/RST 也带 40 位 sid（`tbt_proto.h:14-35`）。真正串行的是 `sess->far_sid`、`tx->seq/cur_block/cur_block_sid`、`rx->next_super/next_deliver/deliver_bytes`、单份 replay/credit/block 窗口（`tbtun.h:658-893`、`tbt_fec_data.c:1639,1653-1703,2906-3350`）。**v7 不需要新头，只需要每流状态。**

2. **组映射选 per-carrier（group 分区），不选 per-stream 全几何时分复用。** 每条流拥有一个**物理 group 位图**（1 group = n+m 条 carrier，256 carriers / 8 = 32 groups）；流内的 `groups=G_i`、seq/block 从 0 开始，carrier = 物理 group × (n+m) + share。会话的 16 MiB RX arena、14 MiB replay ring、2 MiB `g[]` 缓冲保持**单份**，按物理 group 切片，ΣG_i = 32 时内存与单流相同。若让每条流都用全部 32 groups（per-stream 时分），每条流各需一份 replay 的 2 个 super-block 松弛：15 MiB / 2.01 MiB ≈ 7 个 super-block，两条流各得 K=3、credit=1 个 super-block（1 MiB）；四条流无法满足 K≥3，NACK 修复退化。**这是选择 group 分区的决定性理由**（§2.2）。

3. **每流独立 seq/block 空间。** DATA/PARITY 的 `seq`、`block_id` 改为流内序号；`block_id` 不再由会话全局 seq 推导。sid 是唯一 demux 键；`(sid, block_id)` 唯一标识一个块。NACK/ACK 头里的 sid 从“只做入口过滤”升级为“路由键”。

4. **交付隔离靠每流游标 + 按物理 group 切片的共享 arena。** 每条流有自己的 `next_super/next_deliver/deliver_bytes/total_*`、NACK 状态和 credit；一条流的 socket 阻塞只关闭它自己的 credit，不阻塞别的流。`w_fec_rx` 轮转各流，遇到阻塞的流跳过并稍后重试。

5. **流生命周期：OPEN 携带初始 group 位图；新记录 `TBT_FEC_ALLOC`（0x36）做再平衡。** 近端是唯一分配者；远端按 OPEN/ALLOC 执行。再平衡用两阶段 drain/commit：旧 group 范围在流的 frontier 越过切换点之前不得复用。单流仍拿全部 32 groups；第二条流到来时，近端把现有流缩到公平份额（32/N），把释放的 group 给新流。

6. **wire 升版：`TBT_FEC_CONV 0x1007`、`TBT_CONV 0x0007`、`TBT_WIRE_VERSION_V6 4`。** 48 字节 FEC 头布局不变；OPEN body 从 0 变 32 字节 group 位图；FIN body 仍是 16 字节，但 `pkt_end/pkt_base` 改为流内包序号；新增 ALLOC 0x36。旧端在 HELLO 版本处被拒绝，两端必须同升。

7. **SC-LDPC 按每流 codec 接入。** 仓库内只有保留枚举与拒绝路径（`tbt_fec.h:1085-1091`、`tbt_fec_data.c:5710-5715`），没有实现。v7 只定义缝：每条流一个 codec 对象、G_i 个 group；codec ops 形状不变；share↔codeword 映射仍是 D 的开放契约。

8. **最小路径分五阶段**（§6）：wire/host → 单流 flow 对象 → 两流静态分配（解锁 iperf3 control+data）→ ALLOC 再平衡 → 自适应/SC-LDPC/文档。`tbt_fec_data.c` 只有一个所有者，阶段串行；wire/host 与会话/生命周期可并行。

---

## 1. 现状：单流闸门与数据面路径

### 1.1 准入闸门

`[事实]` 近端 `tbt_stream_start_now()` 在 FEC 会话下调用 `tbt_session_fec_stream_live()`；只要 `far_sid` 指向的流未 DEAD 且未发 FIN，就返回 `-EOPNOTSUPP` 并计 `fec_stream_refused`（`tbt_sock.c:704-718`；`tbt_session.c:913-932`）。`tbt_pending_body()` 对这个错误立即 `SO_LINGER 0` + `sock_release`，不排队（`tbt_sock.c:795-813`）。注释明说原因：FEC 记录没有 stream id，第二条流的字节会交付给当前流（`tbt_sock.c:704-707`）。**这条注释已被 0.2.2 的 sid 取代，但数据面仍只维护一份游标。**

`[事实]` 远端 `tbt_session_bind_sid()` 对不同 sid 的新 OPEN 无条件写 `sess->far_sid = sid`（`tbt_session.c:987`），旧流对象留在哈希里但再也收不到 FEC 记录（`tbt_fec_sid_accept()` 只认 `rec == far_sid`，`tbt_fec.h:148-153`）。即：近端拒绝第二条流，远端则静默截断旧流。

`[事实]` `n_active` 的硬界是 `TBT_STREAMS_MAX=128`（`tbtun.h:232`），`tbt_stream_get_created()` 在锁内检查并递增（`tbt_session.c:820-833`）。FEC 的单流限制是额外的、数据面专属的闸门。

### 1.2 sid 过滤

`[事实]` `tbt_fec_sid_accept(cur, barrier, rec)`：`!cur` 或 `cur == barrier` 时拒绝，否则要求 `rec == cur`（`tbt_fec.h:148-153`）。唯一调用点在 `tbt_fec_rx_record()`：对 DATA/PARITY/ACK/NACK 且 `h.sid != 0` 生效；`h.sid == 0` 绕过过滤（`tbt_fec_data.c:4601-4609`）。这是一个隐式契约缺口：sid 为 0 的数据记录会直接进入窗口。

`[事实]` ACK 的 `h.sid = st->sid`（`tbt_fec_data.c:2522`），NACK 的 `h.sid = 当前流 sid`（`:2549`），与头注释“control records carry 0”（`tbt_fec.h:59-61`）不一致；接收端对 ACK/NACK 也做 sid 过滤。**v7 应把这个既成事实写成正式契约：控制记录的 sid 是路由键，0 只保留给会话级记录。**

### 1.3 TX 路径

`[事实]` 会话只有一份 `struct tbt_fec_tx`（`tbtun.h:658-768`）：

- `tx->seq` 是会话全局包序号；`group = (seq-base_pkt)%groups`、`share = ((seq-base_pkt)/groups)%n`、`block_id = base_block + ((seq-base_pkt)/groups)/n`（`tbt_fec.h:746-792`）。
- `carrier = group*(n+m)+share`（`tbt_fec_data.c:1450`）；carrier 身份不写入 FEC 头，接收端只按 seq/share 反推。
- `tx->cur_block_sid` 给 DATA/PARITY 打 sid（`:1443,1493`）；换流时关闭旧块、丢弃旧流未打包的 ring 前缀、清空旧流的队列并 `peer_pkts = tx->seq`（`:1653-1703`）。
- `tbt_fec_tx_submit()` 把应用字节写进单一 ring，并记录 `switch_to/switch_sid/last_submit_sid`（`:2224-2271`）。
- 打包器每轮从 `tbt_fec_stream()` 取“当前流”（`:1639`）；该函数先看 `far_sid`，没有就取哈希里第一个流（`:169-186`）。

`[事实]` replay ring 按 `(block_id % K, group, share)` 索引，不含 sid（`tbt_fec.c:569-582`）；K 由形状决定，256 carriers 时 K=7（`tbt_fec.c:493-510`）。credit 是会话单值 `tx->peer_pkts/tx->credit`（`tbtun.h:677-678`），`tbt_fec_tx_credit_ok()` 比较全局 `tx->seq`（`tbt_fec_data.c:1536-1554`）。

### 1.4 RX 交付 / NACK / credit

`[事实]` 会话只有一份 `struct tbt_fec_rx`（`tbtun.h:770-893`）：

- 一个 `next_super`/`next_deliver`/`deliver_bytes`；交付按全局 super-block 顺序，要求每组 block ready（`tbt_fec_data.c:2806-2843,2906-3350`）。
- 块槽 `rx->blk[(group * slots) + (block_id % slots)]`（`:2649-2653`）；`slots = 16 MiB / (groups*shares*S)`，256 carriers 时 slots=8（`:5753-5759`）。
- `b->sid` 在放置时写入 `far_sid`（`:5273`），但全仓没有读取点；注释声称的“按 sid 清块”不存在。
- NACK 是每 super-block 一个：header `block_id` + 32 字节 group bitmap + 32 字节 share bitmap（`tbt_fec.h:212-216`）；扫描窗口 `[next_super, highest_block]`，受 `replay_k` 限制（`tbt_fec_data.c:3471-3920`）。
- ACK body 24 字节：`next_deliver`、`credit_packets`、`rtt_us`、`deliver_bytes`（`tbt_fec.h:197-204`）。credit 由 `tbt_fec_credit_packets(groups,n,K,slots,1024)` 推出，256 carriers/n=4 时 = 5 个 super-block = 640 包 = 5 MiB 数据（`tbt_fec.h:332-355`、`tbt_fec_data.c:2505-2511`）。
- ACK 处理用 `tbt_fec_stream(sess)` 取流，不看 `h.sid`（`:4719-4750`）；只靠“acked 不超过本流 tx_seq”兜底。

### 1.5 FIN / close / 生命周期

`[事实]` FIN 是主路径控制帧，不是 FEC 记录（`tbt_rx.c:458-490`）。FEC 发送端先 `tbt_fec_tx_finish()` 冻结 `st->fec_pkt_end = tx->seq`（`tbt_fec_data.c:1402-1424`），再在 FIN body 写 `(pkt_end, pkt_base)`（`tbt_tx.c:347-427`）；两者都是**会话全局**包序号。接收端 `tbt_fec_rx_fin()` 把 `pkt_end` 当作 `total_pkts`（`tbt_fec_data.c:5461-5579`）；FIN 先于流绑定到达时停在单槽 `next_fin_*`（`tbtun.h:843-847`）。

`[事实]` 优雅关闭要 `tx_fin_written && rx_fin_done` 才 detach（`tbt_session.c:1055-1067`）；detach 清 `far_sid`、盖 2 s 墓碑（`:1017-1021`）。`tbt_session_fec_rx_barrier()` 取消整个 `w_fec_rx` 并调用数据面的 `stream_reset`，清空所有游标与块（`tbt_session.c:1316-1323`、`tbt_fec_data.c:2733-2799`）。这是“换流”语义，不是“多流”语义。

### 1.6 单流假设清单

| # | 假设 | 证据 | v7 处理 |
|---:|---|---|---|
| 1 | 一个会话一个 `far_sid` | `tbtun.h:1157`；`tbt_session.c:987` | FEC 用 flow 表；`far_sid` 只留 ARQ/proc |
| 2 | 一个全局包序号 | `tx->seq` `tbtun.h:672`；映射 `tbt_fec.h:746-792` | 每流 seq/block |
| 3 | 一个块构建游标 | `tx->cur_block/cur_block_sid` `tbtun.h:690-694` | 每流 `cur_block`；`g[]` 按物理 group 共享 |
| 4 | 一个 RX 窗口/交付游标 | `rx->next_super/next_deliver/deliver_bytes` `tbtun.h:783-790` | 每流游标 |
| 5 | 一个 block arena 窗口 | `rx->blk`、`slots` `tbtun.h:848,783` | 共享 arena 按物理 group 切片 |
| 6 | 一个 replay ring | `tbt_fec.c:459-555` | 共享 ring 按物理 group 切片；key 加 sid |
| 7 | 一个 credit | `tx->peer_pkts/credit` `tbtun.h:677-678` | 每流 credit |
| 8 | 一个 FIN 单槽 | `next_fin_*` `tbtun.h:843-847` | 每流 FIN 状态 |
| 9 | 换流即清空/屏障 | `tbt_session.c:1316-1323` | 流创建/销毁独立；不再全清 |
| 10 | 停滞杀整个会话 | `TBT_FEC_STALL_MS` `tbt_fec_data.c:3038-3080` | 每流停滞只杀该流 |

---

## 2. 方案选择：per-carrier group 分区

### 2.1 两个候选

**候选 A：per-stream 全几何时分复用。** 每条流都有自己的 `tbt_fec_tx/rx` 全几何（32 groups、16 MiB arena、14 MiB replay），会话 packetizer 在流之间轮转；每条流使用全部 256 carriers，只是时间上交错。

**候选 B：per-carrier group 分区（本方案）。** 每条流拥有一个物理 group 位图；流内几何是 `G_i = popcount(bitmap)` 个 group。会话的 arena/replay/`g[]` 保持单份，按物理 group 切片。不同流的 group 集合不相交，因此它们的块槽、replay 槽、codec 组互不冲突。

### 2.2 内存与窗口计算

`[计算]` 取 256 carriers、n+m=8、groups=32、S=8192、slots=8、K=7、shares=8：

| 量 | 单 super-block | 会话总量 |
|---|---:|---:|
| 数据字节（n=4） | 32×4×8192 = 1 MiB | credit 5 块 = 5 MiB |
| RX arena（数据+parity） | 32×8×8192 = 2 MiB | 8 槽 = 16 MiB |
| replay（全部记录） | 32×8×8240 = 2,109,440 B | K=7 = 14,766,080 B |
| `g[]` 缓冲 | 32×8×8192 = 2 MiB | 2 MiB |

候选 B 下，流 i 的资源与 `G_i` 成正比：

| 资源 | 流 i | ΣG_i=32 |
|---|---:|---:|
| arena | `G_i × 512 KiB` | 16 MiB |
| replay | `G_i × 461,440 B` | 14.77 MB |
| credit | `5 × G_i × n` 包（n=4 时 20G_i 包 = 160G_i KiB） | 5 MiB |
| `g[]` | `G_i × 64 KiB` | 2 MiB |

`[计算]` 100 ms RTT 下的窗口上限（credit 字节 / RTT）：

| 流数 N | 每流 G_i | 每流 credit | 每流吞吐 | 聚合 |
|---:|---:|---:|---:|---:|
| 1 | 32 | 5 MiB | 400 Mbit/s | 400 Mbit/s |
| 2 | 16 | 2.5 MiB | 200 Mbit/s | 400 Mbit/s |
| 4 | 8 | 1.25 MiB | 100 Mbit/s | 400 Mbit/s |
| 8 | 4 | 640 KiB | 50 Mbit/s | 400 Mbit/s |
| 32 | 1 | 160 KiB | 12.5 Mbit/s | 400 Mbit/s |

聚合吞吐由总 in-flight 字节 / RTT 决定，与怎么分无关；group 分区只是把固定预算按 carrier 份额分给各流。

`[计算]` 候选 A 的代价：每条流都要一份 replay 的 K 个 super-block。15 MiB / 2.01 MiB ≈ 7 个 super-block 总量；两条流各 K=3 才能满足 `credit = K-2 = 1` 个 super-block（1 MiB），100 ms RTT 下每流 80 Mbit/s、聚合 160 Mbit/s——比候选 B 的 400 Mbit/s 低 2.5 倍。四条流需要 K≥3 × 4 = 12 个 super-block = 24 MiB，超出预算，只能把 K 压到 1~2，NACK 修复退化。**候选 A 在现有 15 MiB replay 预算下不可扩展。**

### 2.3 决策

`[设计]` **采用候选 B。** 理由：

1. 单流拿全部 32 groups，行为与 HEAD 完全一致（G=32 时所有公式退化为现状）。
2. 内存不随流数乘；ΣG_i=32 时 arena/replay/`g[]` 与单流相同。
3. 每条流的窗口按 carrier 份额缩放，聚合吞吐守恒；没有“小流占着大窗口”的浪费（小流的 G_i 小，credit 自然小）。
4. 交付隔离是结构性的：不同流的 group 集合不相交，块槽/replay 槽/codec 组不相交。
5. 不需要在 256 条 carrier 上做全局包排序；每条流在自己的 seq 空间内连续。
6. 与自适应 shape、SC-LDPC 的接口都是“每流一个 codec + G_i 个 group”，缝很小。

代价：需要**组分配/再平衡**协议（§4.4）。这是本方案的主要新增复杂度，也是候选 A 没有的；但候选 A 的每流内存乘数在现有预算下不可行。

---

## 3. 数据面设计

### 3.1 flow 对象与会话共享数组

`[设计]` 会话新增一个 flow 表（按 sid 哈希，RCU 查找）和每流对象；现有 `tbt_fec_tx/rx` 拆成“会话共享部分”与“每流部分”：

```c
struct tbt_fec_flow {
    struct hlist_node node;        /* session->fec_flows[], key = sid */
    refcount_t refs;               /* stream + RX work */
    u64 sid;                       /* 40-bit domain, same as main wire */
    u8  group_mask[TBT_FEC_BITMAP_BYTES]; /* physical groups, 256 bits */
    u16 group_map[TBT_FEC_MAX_GROUPS];    /* local g -> physical group */
    u16 groups;                    /* popcount(group_mask) */
    u8  state;                     /* ALLOC / ACTIVE / DRAINING / DEAD */
    u32 epoch;

    /* TX：每流 */
    u8 *ring; u32 ring_size, head, tail;  /* capture ring, sized by credit */
    u16 fill;
    u64 seq;                       /* flow-local next packet */
    u64 cur_block;                 /* flow-local block being built */
    u64 peer_pkts; u32 credit;     /* per-flow ACK frontier/credit */
    u64 base_pkt, base_block;      /* flow-local mapping origin */
    u64 fec_pkt_base; bool fec_pkt_base_set;
    u64 fec_pkt_end; bool finished;
    u8  switch_state; u32 switch_epoch;
    u64 switch_pkt, switch_block;  /* P0/B0 for shape or group switch */
    struct tbt_fec_gtx *g;         /* slice into session g[32], or per-flow */
    struct tbt_fec_grouped *codec; /* G_i groups */
    /* RX：每流 */
    u64 next_super, next_deliver, deliver_bytes;
    bool rebase_pending;
    u64 stream_pkt_base, stream_span, stream_base_pkt;
    u64 total_pkts, total_bytes; bool total_known;
    bool deliver_partial; u16 deliver_p, deliver_g;
    u64 ack_deliver;
    u16 replay_k;
    /* NACK pacing/probe/gap, per flow */
    ...
};
```

`[设计]` 会话保留：

```c
struct tbt_fec_plane {
    struct tbt_fec_tx tx;   /* session scratch: pkt/rec/replay/outq/drain */
    struct tbt_fec_rx rx;   /* session scratch: blk[]/req_ns/req_block/rec */
    struct hlist_head flows[TBT_STREAM_HT]; /* key = sid; same bucket count as streams */
    u16 n_flows;
    struct tbt_fec_flow **group_owner; /* total_groups entries; NULL = free */
    struct tbt_fec_alloc alloc; /* arbiter state */
};
```

`[设计]` flow 表用 `TBT_STREAM_HT=64` 个桶（`tbtun.h:228`），与 stream 哈希同构；
`group_owner[]` 的长度是 `total_groups = carriers/(n+m)`（256 carriers/n+m=8 时为 32），
不是固定 32。`TBT_FEC_MAX_FLOWS` 的上界就是 `total_groups`（每个 flow 至少 1 个 group）。

`[设计]` `struct tbt_stream` 增加一个 RCU 保护的 `struct tbt_fec_flow *flow`；capture
（`tbt_fec_tx_submit()`）、FIN 冻结、deliver 都用它直接找到本流状态，不再经过
`far_sid`。流对象的生命周期由 stream 引用 + flow 表引用共同保证。

`[设计]` **共享数组按物理 group 切片**：

- RX arena：`rx->blk[(phys_group * slots) + (block_id % slots)]`。两条流的 `block_id` 可以相同，但 `phys_group` 不同，槽不冲突。
- replay ring：`slot = ((block_id % K) * total_groups + phys_group) * shares + share`（`tbt_fec.c:569-582` 的 `groups` 用会话总组数）。同理不冲突。
- `tx->g[phys_group]`：`struct tbt_fec_gtx` 的 `blk_id/nreal/last_len` 按物理 group 保存；流的 packetizer 只访问自己的 group 切片。
- `rx->req_ns/req_block[(phys_group * slots) + (block_id % slots)]`：按物理 group 切片。
- codec：每流一个 `tbt_fec_grouped`，`groups = G_i`；局部 group 0..G_i-1。nonce 用局部 group 索引，两端从同一 bitmap 推导，天然一致。
- 队列/drain：保持会话级、按 carrier 索引（`tbtun.h:1096`）。稳态下不同流的 group 集合不相交，因此 carrier 集合也不相交：一条 carrier 队列只属于一条流的 group。只有再平衡过渡期，旧/新集合的记录可能短暂共用同一条 carrier；接收端按 sid 分派，队列本身不区分流。

`[设计]` 这样 `ΣG_i = 32` 时，arena/replay/`g[]` 的分配与 HEAD 的单流分配完全相同，只是切片边界可变。**没有每流乘数。**

### 3.2 group bitmap 与映射公式

`[设计]` 每条流的 `group_mask` 是 32 字节位图（与 `TBT_FEC_BITMAP_BYTES` 一致），bit `p` 表示物理 group `p`。`group_map[g]` 是第 g 个置位的物理 group（局部索引 0..G_i-1）。映射：

```
local_g   = (seq - base_pkt) % G_i
share     = ((seq - base_pkt) / G_i) % n
block_id  = base_block + ((seq - base_pkt) / G_i) / n
phys_g    = group_map[local_g]
carrier   = phys_g * (n + m) + share
```

`[设计]` 与 HEAD 的 `tbt_fec_group_of_seq_base()` 等 helper 同构，只是 `groups` 换成 `G_i`、末尾加 `group_map[]`。`base_pkt == base_block == 0` 时，单流（bitmap 全 1、G=32）逐字节等价于现状。

`[设计]` 校验：

- `popcount(group_mask) >= 1`，且 `group_mask` 的 bit 必须落在会话的 `total_groups = carriers/(n+m)` 内（`tbt_fec_data.c:5716-5725`）。
- 两条 live flow 的 `group_mask` 不得相交；OPEN/ALLOC 在 `sess->lock` 下检查并拒绝重叠（`-EPROTO` + 计数）。
- `G_i * n` 是流的 super-block 包数；`slots` 仍是会话的 8（每 group 每流的槽数）。

### 3.3 TX：每流 packetizer，共享 scratch

`[设计]` 会话仍只有一个 `w_fec_tx`（保证 `pkt/rec` 单写者），但每轮按轮转/DRR 选择一条有数据且有 credit 的流：

```
for each flow in round_robin:
    if flow->ring empty or !flow_credit_ok(flow): continue
    build up to Q packets (Q = 8 packets or 64 KiB quantum)
    if flow->fill == S: packetize(flow)
    if flow->ring dry and short: arm short timer per flow
```

`[设计]` 每条流的 `packetize()` 与 HEAD 的 `tbt_fec_packetize()` 同构，差异：

- `seq = flow->seq++`，映射用 `G_i/group_map`（§3.2）。
- `h.sid = flow->sid`，`h.seq/h.block_id` 是流内序号。
- `carrier = group_map[local_g]*(n+m)+share`。
- `flow->g[local_g]` 指向会话 `tx->g[phys_g]`；块状态按物理 group 保存。
- parity/empty marker 同 HEAD（`tbt_fec_data.c:1219-1345`），sid 用 `flow->sid`。
- `tbt_fec_tx_finish(flow)` 冻结 `flow->fec_pkt_end = flow->seq`。

`[设计]` 每流的 capture ring：`tbt_fec_tx_submit(st)` 按 `st->flow` 写入该流的 ring；ring 大小 = `clamp(credit_packets * S, 256 KiB, 1 MiB)`（HEAD 的公式 `tbt_fec_data.c:5890-5900` 按流套用）。Σ credit 字节 = 5 MiB（n=4）；加上 256 KiB 下限后，ring 总和最多 `max(5 MiB, 256 KiB × N)` = 8 MiB（N=32）。它仍是常数，不随流长增长。

`[设计]` 换流清队的逻辑（`tbt_fec_data.c:1705-1716` 按 sid 清空 `fec_out_ctl/
fec_out_replay/fec_outq`）删除；队列里同时存在多条流的记录，只在**某条 flow 销毁**
时清它自己的 sid。`struct tbt_fec_out.sid`（`tbtun.h:945-951`）保留，用于这个目的。

`[设计]` replay flush 的“已交付”判定（`tbt_fec_out_flush()` 用 `o->block_id` 与
会话 frontier 比较）改为用 `o->sid` 找到 flow，再与**该 flow** 的
`peer_pkts`/delivered frontier 比较；否则一条流的前进会提前释放另一条流的修复记录。

`[设计]` `tbt_tx_announce_carrier()` 现在 `atomic_set(&c->up,0)` 再置 1
（`tbt_tx.c:325-328`），一条新流的 OPEN 会短暂把整条 carrier 对所有流标成 down。
多流下 `c->up` 必须只反映 carrier 本身；每条流的宣告状态用已有的
`st->announced_mask/announced_gen`（`tbtun.h:485-486`）判定，DATA 路由也按
`(stream, carrier, generation)` 检查（`tbt_tx.c:2303-2307`）。

`[设计]` 流的 packetizer 可以在**任意包边界**让出：因为 seq/block 是流内的，别的流的包不会插进这条流的块。HEAD 必须整 super-block 切换的限制（`tx->switch_to` 丢前缀）消失。这是每流 seq 的直接收益：调度 quantum 可以小，公平性好，不需要“流切换只在 super-block 边界”。

`[设计]` `tbt_fec_tx_idle(flow)`、`tbt_fec_tx_flush(flow)`、`tbt_fec_tx_finish(flow)` 都改为按流判定；A 流的 FIN 不再被 B 流的积压拖住。

`[设计]` `sess->fec_closing` 现在是会话级 replay 准入上限（`tbt_fec_data.c:1720`）；
多流下改为 `flow->closing`，否则一条流关闭会把所有流的 replay 准入都压到
`TBT_FEC_REPLAY_CLOSE_BYTES_MAX`。

### 3.4 RX：每流窗口，共享 arena，交付隔离

`[设计]` `tbt_fec_rx_record()` 收到 DATA/PARITY：

1. `flow = tbt_fec_flow_lookup(sess, h.sid)`；未知 sid → 丢弃并计 `fec_drop_unknown_sid`。`h.sid == 0` 的 DATA/PARITY 一律拒绝（修掉现状的绕过）。
2. 用 `flow->G_i/group_map` 校验 `h.seq/share/block_id`（HEAD 的 `tbt_fec_data.c:5178-5190` 同构）。
3. 窗口判定用 `flow->next_super` 和会话的 `slots=8`，`block_id` 是流内的。
4. 槽 = `(phys_g * slots) + (block_id % slots)`；`b->sid = flow->sid`。
5. `b->ready` 后置 `flow->work_pending`，arm `w_fec_rx`。

`[设计]` 放置仍用一把 `rx->lock`（块槽是共享数组，carrier RX 可能并发）；交付游标
只由 `w_fec_rx` 单写者更新，不需要每流锁。若后续 profile 显示锁竞争，再把
`rx->lock` 按 group 切片；v7.0 不做。

`[设计]` `w_fec_rx` 轮转各流：

- 每条流有独立的 `next_super/next_deliver/deliver_bytes`。
- 对一条流，按 HEAD 的 `tbt_fec_super_ready_locked()`/`tbt_fec_deliver_ready()` 逻辑交付（`tbt_fec_data.c:2806-2843,2906-3350`），只是所有游标换成 `flow->`。
- `tbt_rx_fec_deliver(st, off - flow->stream_base, ...)` 的偏移是流内字节偏移；`stream_base` 每条流从 0 开始。
- 如果一条流的输出 socket 阻塞（`-ENOMEM`/`paused`），**跳过该流，继续下一条**；该流的 credit 会自然关闭，发送端停发这条流，别的流不受影响。
- 一条流超过 `TBT_FEC_STALL_MS` 无进展，只 `tbt_stream_kill(该流)`；其他流继续。

`[设计]` 块解码：会话的 `tbt_fec_rx_pass()` 仍可扫描全部 `32*slots` 个块；每个块带 `b->sid`，解码用该流的 codec/`G_i`。或者按流扫描自己的 group 切片；两者等价，前者改动小。

`[设计]` 交付隔离的边界：

- 不同流的 arena 切片不相交 → 一条流阻塞时它的块原地保留，不会被别的流覆盖。
- 一条流的 `next_super` 落后不会阻止别的流推进，因为窗口是每流的，不共享 `next_super`。
- 唯一的共享是 carrier 队列和 `w_fec_tx/w_fec_rx` 工作项；队列满只影响该 carrier 上的记录，工作项轮转保证不饿死其他流。

### 3.5 NACK

`[设计]` 每流一个 NACK 扫描，状态在 `flow` 内：

- `block_id` 是流内的；group bitmap 用**局部 group 索引** 0..G_i-1（不是物理索引），share bitmap 用 0..n+m-1。
- 记录头 `h.sid = flow->sid`；接收端按 sid 路由，发送端按 sid 找到 flow，再 `phys_g = group_map[local_g]`。
- 发送端 replay 查询：`(block_id % K) * total_groups + phys_g`，与 HEAD 的 ring 布局兼容，只是 groups 用会话总组数。
- `req_ns/req_block` 按物理 group 切片；group 范围被回收/重分配时，清除该范围的 stamp（否则旧 stamp 会抑制新流的请求）。
- 每流的 `replay_k` 仍是 7（或由会话 slots/K 决定）；NACK 窗口是 `[flow->next_super, flow->highest_block]`，按流裁剪。

`[设计]` `tbt_fec_nack_due_at()` 的去重键加 sid（`tbt_fec.c:683-707` 现在只有 block/bitmap）。

`[设计]` replay **发送队列**仍是会话级、共享 `TBT_FEC_REPLAY_BYTES_MAX=8 MiB`
（`tbtun.h:978`）。一条流的 NACK 风暴可以占满它，延迟其他流的修复。v7.0 给每条流
一个按 `G_i` 比例的入队配额（`flow->replay_queued_bytes`），超过就跳过该流的本轮
replay；总量仍受会话 8 MiB 约束。否则把“NACK 风暴隔离”列为已知限制。

### 3.6 credit

`[设计]` 每流 ACK：

- ACK 头 `h.sid = flow->sid`。
- body 的 `next_deliver`/`deliver_bytes` 是流内值；`credit_packets` 由 `tbt_fec_credit_packets(flow->G_i, n, K, slots, TBT_FEC_CREDIT_MAX)` 计算。
- 发送端 ACK 处理按 `h.sid` 找 flow，更新 `flow->peer_pkts/credit` 和该流的 `st->tx_ack`（`deliver_bytes` 是流内字节，clamp 到该流 `tx_seq`）。
- 不再有“换流时 `peer_pkts = tx->seq`”的硬重置；每条流从 0 开始自己的 frontier。

`[设计]` 聚合 credit = Σ `5*G_i*n` 包 = `5*32*n` = 与单流相同。一个流不消费 credit 不会阻塞别的流；它只浪费自己的 group 份额。

`[设计]` flow 创建时 `flow->credit = credit_cap(G_i)`，与 HEAD 的
`tx->credit = tx->credit_cap`（`tbt_fec_data.c:5886-5888`）同构：第一飞行不需要
等 ACK。之后的 ACK 只更新该 flow 的 `peer_pkts/credit`。

### 3.7 FIN / close

`[设计]` FIN 仍走主路径控制帧，逐流：

- `flow->fec_pkt_end = flow->seq`；`st->fec_pkt_end` 保存流内值。
- FIN body 的 `pkt_end/pkt_base` 改为**流内**包序号；`pkt_base` 是流的第一包（通常 0；跨 shape/group 切换时可能低于当前 `base_pkt`，沿用 `tbt_fec_fin_valid_base()` 的 prev-shape 逻辑，`tbt_fec.h:105-113`）。
- 接收端 `flow->total_pkts = pkt_end`；final block 用 `tbt_fec_final_block_of_total_base(total_pkts, flow->base_pkt, flow->base_block, flow->G_i, n)` 计算。
- FIN 先于流创建到达：按每 carrier 顺序，OPEN 先于 FIN 写出（§4.2）；如果仍然找不到 flow，丢弃并计数，不再用单槽 `next_fin_*`。v7 可以保留每流 `next_fin` 以容忍跨 carrier 乱序，但槽按 sid 索引。
- 关闭握手不变：`tx_fin_written && rx_fin_done` 才 detach；detach 后销毁 flow、归还 group、清该范围的 arena/replay stamp。墓碑仍按流（`tbtun.h:430`）。
- RST/kill：立即销毁 flow；其他流不受影响。迟到的记录因 flow 查找失败被丢弃。

### 3.8 自适应 shape 与 SC-LDPC 接口

`[设计]` 自适应 shape 仍是会话级决策（n/m/groups 的 n+m=8 族，`tbt_fec_adapt.h:47-49`），但切换状态移到每流：

- v7.0：**N>1 时冻结 shape**（不发起新切换）；N==1 时沿用 HEAD 的两阶段 CFG/FBK。这样多流首版不引入每流 CFG 的复杂度。
- v7.1：FEC_CFG/FBK 带 `h.sid`，每流独立两阶段提交；会话控制器把同一目标 shape 发给所有流，各流在自己的 P0 提交。FBK 的 `carrier_bitmap` 仍是物理 carrier，流的 bitmap 只在自己的 group 范围置位；发送端聚合各流的 expected/received。
- 组再平衡（ALLOC）与 shape 切换是两种独立的“移动 base”操作；每条流同一时间只允许一个在途切换，用 `flow->switch_state` 串行化。

`[设计]` SC-LDPC 缝：

- 每流一个 codec 对象，`groups = G_i`；`tbt_fec_grouped_new(G_i, n, m, k, w, s, d, nonce)` 的调用点从会话级改为流级（HEAD 在 `tbt_fec_data.c:5832,5913`）。
- codec ops 形状（`tbt_fec.h:1129-1148`）不变：`encode/decode` 接收 L 个数据 share 和 parity 行；流内 group 的局部索引 0..G_i-1。
- 数据面不走 ops 表（HEAD 直接调 `tbt_fec_grouped_*`，`tbt_fec_data.c:1224,1919-1922,2422,5832,5913`）；v7 在这些点改为 `flow->codec`。SC-LDPC 接入时在同一缝替换 `tbt_fec_grouped_*`。
- share↔codeword 映射、L、block 参数仍是 D 的开放契约（`review/sc-ldpc-0.3.0-design.md:252-268`）；v7 不定义它，只保证每流有独立的 G_i 和 codec 实例。
- `mode=3` 在实现落地前仍返回 `-EOPNOTSUPP`（`tbt_fec_data.c:5710-5715`）。

---

## 4. 生命周期与组分配

### 4.1 近端准入

`[设计]` 删除 FEC 单流闸门（`tbt_sock.c:704-718`、`tbt_session.c:913-932` 的调用点），保留 `closing` 和 `n_active < TBT_STREAMS_MAX`。新增 FEC 专属准入：

```
tbt_stream_start_now(sess, sock):
    sid = alloc_sid()
    if sess->fec:
        rc = tbt_fec_flow_admit(sess, sid, &flow)
        if rc == -EAGAIN:      # 需要再平衡
            queue the pending entry in sess->fec_alloc_wait
            arm w_fec_alloc
            return -EBUSY      # pending 队列保留 socket，稍后重试
        if rc == -ENOSPC:      # total_groups 条流已满（256 carriers 时 32）
            atomic64_inc(&sess->fec_flow_refused)
            return -EOPNOTSUPP
    create stream, set sk, state OPEN
    if sess->fec:
        queue OPEN on every carrier (body = flow->group_mask)
    attach socket, schedule w_read
```

`[设计]` 等待再平衡的流不立即 RST：它留在 `pending` 队列，`w_fec_alloc` 完成后再走 `tbt_stream_start_now`。等待有上界（再平衡 drain + commit），超时（例如 2 s）才 RST 并计 `fec_alloc_timeout`。这修掉 iperf3 双连接被立即 reset 的问题。

`[设计]` 同时删除 `tbt_stream_start_now()` 里对旧 `far_sid` 的 barrier
（`tbt_sock.c:728-733`）：新流不再需要清空旧流的 RX 游标；每条 flow 有自己的游标。
`tbt_tx.c:3261-3269` 的“`!far_sid && n_pending` 才重触发 pending”改为“flow 表或
group 池还有空位就重触发”，否则非 FEC 之外的多流等待会被 `far_sid` 卡住。

`[设计]` `fec_stream_refused` 计数保留但语义改为“FEC flow 满/分配失败”；新增 `fec_alloc_wait`、`fec_rebalance`、`fec_alloc_timeout`、`fec_drop_unknown_sid`、`fec_group_overlap`。

### 4.2 远端 OPEN

`[设计]` OPEN body = 32 字节 `group_mask`（§5.2）。`tbt_session_bind_sid(open=true)`：

1. 校验 body 长度/bitmap：置位数 ≥1，bit < total_groups，且与所有 live flow 的 bitmap 不相交；否则 `-EPROTO` + `fec_group_overlap`，关闭 carrier。
2. 创建/查找流；不同 sid 不再覆盖 `far_sid`。`far_sid` 只更新为“最近/主”用于 ARQ/proc。若 `n_active >= TBT_STREAMS_MAX` 或 flow 表满，返回 `-ENOSPC` 并计数；不再像 HEAD 那样 `tbt_stream_get_created()` 返回 NULL 后仍回 `TBT_BIND_OK`（`tbt_session.c:1000-1012`）。
3. 创建 flow 元数据：从 bitmap 推导 `group_map/G_i`，初始化 `seq=0/cur_block=0/base=0`、RX `next_super=0`（首块丢失时 NACK 0..，而不是 rebase 到首见块）。ring/codec 不在 OPEN 上下文分配，见下面的 softirq 说明。
4. 重复 OPEN（同 sid、同 bitmap）幂等；同 sid 不同 bitmap 视为错误。
5. `tbt_far_open_seen()` 拨目标连接（`tbt_sock.c:905-917`）。

`[设计]` 远端回显的 OPEN 也带同一 bitmap。近端收到回显时校验它与本地 flow
一致；不一致丢弃并计数（`fec_open_mismatch`），不改变本地分配。

`[设计]` **每 carrier 顺序不变量**：对每条 (flow, carrier)，OPEN（以及后续 ALLOC/CFG）必须先于该流该形状的任何 DATA/PARITY 写出。近端在 attach 之前 announce（`tbt_sock.c:752-757`），控制队列优先于数据队列；FEC 数据只在 `announced_mask` 命中的 carrier 上发送（`tbt_tx.c:2303-2307` 的等价逻辑）。接收端据此可以在 OPEN 时创建 flow，不需要缓存未知 sid 的原始记录。若仍然收到未知 sid 的 DATA/PARITY，丢弃并计数，依赖发送端的 NACK/重传；这是防御路径，不是正常路径。

`[设计]` **OPEN 可能运行在 carrier RX（softirq）**：flow 的 ring/codec 是 `GFP_KERNEL`
分配，不能原地做。OPEN 只做两件轻量事：校验 bitmap、创建/取一个 flow 元数据
（`GFP_ATOMIC`，或从会话预分配的小对象池取），然后 arm `w_fec_alloc` 在进程上下文
分配 ring/codec 并发布。发布前到达的 DATA/PARITY 仍可放置（窗口和 arena 已就绪），
decode 等 codec 就绪，TX capture 等 ring 就绪；若分配失败，销毁 flow 并关闭该
carrier，不静默丢流。

### 4.3 初始分配

`[设计]` 近端 `tbt_fec_flow_admit()`：

- `free_bits = total_groups - ΣG_i`。
- 若 `free_bits >= G_min`（G_min=1）：按公平份额 `G_target = total_groups / (n_flows + 1)`（至少 1）分配；如果 free_bits 足够，直接取 `G_target` 个 free bit，不打扰现有流。若 free_bits 不够，触发再平衡（§4.4）。
- 若 `n_flows == total_groups`（每组一条流）或无法凑出 1 个 group，返回 `-ENOSPC`。
- 分配结果写进 flow，OPEN body 携带。

`[设计]` 公平份额的取整：`G_i = total_groups / N`，余数按 sid 升序给前几个流；N 变化时重算。v7.0 等权；v7.1 可按流需求加权（§9）。

### 4.4 再平衡 ALLOC

`[设计]` 当新流需要 group 但 free_bits 不足时，近端作为唯一仲裁者：

1. 计算目标 `G_i`（等权，ΣG_i=32）。
2. 对每个需要缩小的现有流发送 `TBT_FEC_ALLOC`（0x36）：`group_mask`=新集合，`switch_pkt`=该流的下一个 super-block 边界（流内 seq），`block_base`=该流的下一个 block id，`epoch++`。
3. 接收端校验后回 `ALLOC(flags=ACK)`；发送端把该流的 packetizer 停在 `switch_pkt`（沿用 HEAD 的 DRAIN 逻辑，`tbt_fec_data.c:1439-1441,1737-1740`）。
4. 接收端在自己的 `next_deliver >= switch_pkt` 时应用新 bitmap：清空被释放 group 的 arena 槽/req stamp/replay 槽/`g[phys]` 状态（这些槽在 frontier 之前已经交付完），安装新 `group_map/G_i`，回 `ALLOC(flags=DONE)`（或复用 FBK 的 SWITCH_DONE 标志）。
5. 发送端收到 DONE 后 swap：`base_pkt=switch_pkt`、`base_block=switch_block`、`G_i=新值`、重建 codec（或复用 `old_codec` 直到下个 swap）、重切 `g[]` 指针、清被释放 group 的 replay 槽、重算 `credit_cap`。
6. 仲裁者等到该流的 ACK frontier ≥ `switch_pkt`，才把释放的 group 分配给新流；新流的 OPEN 在分配完成后发出。

`[设计]` 关键不变量：

- 被释放的 group 在 donor 的 `frontier >= switch_pkt` 之前不得出现在任何新流的 bitmap 中。
- 同一流的 ALLOC 与 shape CFG 串行（`flow->switch_state` 单飞）。
- 新流的 OPEN 在该流自己的 group 集合上；与 donor 的新 bitmap 不相交。
- 再平衡期间 donor 仍可用旧 group 发送/接收；接收端保留 `prev_bitmap/prev_G` 直到 frontier 越过 P0（与 shape 的 prev_shape 同构）。

`[设计]` 失败/超时：ALLOC 500 ms 未 ACK → abort，保留旧 bitmap，新流继续等待或返回 `-ENOSPC`；DRAIN 2 s → abort；与 HEAD 的 CFG 超时一致（`tbt_fec_data.c:2041-2100`）。

`[设计]` 新 `G_i` 的 codec 在 `w_fec_rx`（进程上下文）预分配，失败则 ALLOC abort；
不在 carrier RX 的 softirq 里分配。沿用 HEAD 的 `pending_codec` 预分配模式
（`tbt_fec_data.c:4374-4386`）。

### 4.5 销毁 / 墓碑 / 迟到记录

`[设计]` 流 detach（`tbt_session.c:1026-1046`）或 kill 时：

1. 标记 flow DEAD，停止该流的 TX/RX 工作（不取消会话工作项）。
2. 从 flow 表移除；迟到的 DATA/PARITY/ACK/NACK 查找失败 → 丢弃 + 计数。
3. 归还 `group_mask` 的 bit；清该范围的 arena/replay/req stamp；唤醒 `w_fec_alloc`（可增长其他流或接纳等待的流）。
4. 释放 ring/codec/flow；流对象按现有墓碑/reaper 回收（`tbt_session.c:1142-1197`）。
5. `far_sid` 若等于该 sid，清 0（保持 ARQ/proc 行为）。

`[设计]` group 复用是安全的：新流的记录 sid 不同，接收端按 sid 分派；旧流的迟到记录在查找阶段被丢弃，不会进入新流的 arena。replay 槽按物理 group 清空后，旧 block 的 NACK 不可能再被合法接收（流已死）。

`[设计]` session destroy 的顺序：先停止并销毁所有 flow（释放 ring/codec、归还
bitmap），再释放共享 arena/replay/`g[]`。现有 `tbt_fec_data_free()` 只释放共享部分
（`tbt_fec_data.c:5625-5680`），v7 需要在它之前加一个 flow 销毁循环；否则 flow
持有指向已释放 arena 的指针。

---

## 5. wire

### 5.1 版本

| 常量 | HEAD | v7 | 变化 |
|---|---:|---:|---|
| `TBT_FEC_CONV` | 0x1006 | **0x1007** | sid 成为路由键；seq/block 流内；新增 ALLOC 0x36；CFG/FBK 带 sid |
| `TBT_CONV` | 0x0006 | **0x0007** | OPEN body 32 B group bitmap；FIN body 语义改为流内包序号 |
| `TBT_WIRE_VERSION_V6` | 3 | **4** | HELLO body[0]；旧端在 `tbt_rx_hello()` 精确比较处被拒 |
| FEC 头 | 48 B | 48 B | 布局不变 |

`[事实]` 现有版本规则：任何偏移/长度/类型字节变化都升 `TBT_FEC_CONV` 并在同一提交更新 host 向量（`tbt_fec.h:11-13`）；HELLO 版本精确比较，两端必须同升（`tbt_uapi.h:69-75`）。v7 沿用硬切换，不做兼容解析。

`TBT_WIRE_VERSION`（旧 ARQ 的 HELLO 字节）保持 1；只有 FEC 的 `TBT_WIRE_VERSION_V6`
从 3 升到 4。`TBT_CONV` 升到 0x0007 对 ARQ 也生效（OPEN/FIN 长度与语义变化），所以
ARQ 两端也必须同升。

### 5.2 OPEN body（32 B）

`[设计]` `TBT_F_OPEN` 的 body 从 0 改为 32 字节：

```
+0  u8 group_mask[32]   bit p = 物理 group p；bit >= total_groups 必须为 0
```

- FEC 会话：近端总是发 32 字节；远端要求 `body_len == 32` 且 bitmap 合法。
- 非 FEC 会话：为保持一条长度规则，发 32 个零字节（与 FIN 的“非 FEC 也发 16 个零字节”先例一致，`tbt_uapi.h:345-354`）；接收端忽略。
- `tbt_body_bounds()` 的 `TBT_F_OPEN` 从 `min=max=0` 改为 `min=max=32`（`tbt_uapi.h:398-403`）。旧 ARQ 的 OPEN 也随之变长；这是 `TBT_CONV` 升版的一部分。
- 远端在创建 flow 前完成 bitmap 校验；非法 bitmap 返回 `-EPROTO`，关闭该 carrier（与现有 framing 错误一致）。

### 5.3 FIN body（16 B，语义变化）

`[设计]` 布局不变：

```
+0  u64 pkt_end   流内包序号：该流最后一个 DATA 包之后
+8  u64 pkt_base  流内包序号：该流第一个 DATA 包（通常 0）
```

- 接收端 `flow->total_pkts = pkt_end`，final block 用流内 `G_i/n/base_*` 计算。
- `pkt_base` 低于当前 `base_pkt` 时表示流跨过 shape/group 切换，沿用 `tbt_fec_fin_valid_base()` 的 prev-shape 分支（`tbt_fec.h:105-113`、`tbt_fec_data.c:5489-5496`）。
- 非 FEC 会话仍发 16 个零字节。

### 5.4 FEC 头语义（48 B 不变）

| 偏移 | 字段 | v7 语义 |
|---:|---|---|
| +8 | `block_id` | **流内**块号；`(sid, block_id)` 唯一 |
| +16 | `seq` | **流内**包序号；PARITY 仍为窗口末端 |
| +32 | `sid` | DATA/PARITY/ACK/NACK/CFG/FBK/ALLOC 必填；0 保留给会话级记录；必须落在 40 位主 sid 域 |
| +4/+5 | `n/m` | 会话形状；流的每个 group 仍是 n DATA + m PARITY |
| +24/+26 | `win_len/payload_len` | 不变（流内块） |

`[设计]` 校验收紧：

- DATA/PARITY 的 `sid != 0`；`sid` 必须能在 flow 表找到，否则丢弃。
- ACK/NACK 的 `sid != 0` 且必须找到 flow；找不到丢弃并计数。
- ALLOC 的 `sid != 0`、`n=m=0`、`share_index=0`、`win_len=0`、`block_id=0`、`payload_len=64`。
- CFG/FBK 的 `sid` 在 v7.0 单流时等于该流 sid；N>1 时 shape 冻结，FBK 仍按流发送/聚合。

### 5.5 ALLOC（0x36，64 B body）

`[设计]` 新记录类型 `TBT_FEC_ALLOC = 0x36`，用于组再平衡（初始分配走 OPEN，不需要 ALLOC）：

```
struct tbt_fec_alloc {
    u32 epoch;                    /* +0  每次分配递增 */
    u8  group_mask[32];           /* +4  新的物理 group 集合 */
    u8  flags;                    /* +36 bit0 ACK, bit1 DONE */
    u8  reserved[3];              /* +37 */
    u64 switch_pkt;               /* +40 流内 seq：第一条新集合记录 */
    u64 block_base;               /* +48 流内 block id：切换后第一块 */
    u32 crc32c;                   /* +56 覆盖 +0..+55 */
    u32 reserved2;                /* +60 */
};                                /* 64 B */
```

- header：`type=0x36`、`sid=flow sid`、`conv=0x1007`、`payload_len=64`、`block_id=0`、`seq=0`。
- 方向：发送端（近端仲裁者）→ 接收端 REQUEST；接收端 → 发送端 ACK/DONE。
- `switch_pkt` 必须对齐旧集合的 `G_old*n`（与 CFG 的 P0 校验同构，`tbt_fec_data.c:4455-4476`）；`block_base = base_block + (switch_pkt-base_pkt)/(G_old*n)`。
- 解析器：`tbt_fec_record_type()` 的范围 0x30..0x35 扩到 0x36（`tbt_fec_data.c:2305-2308`）；`tbt_fec_hdr_check()` 增加 ALLOC 的字段约束（`tbt_fec.c:199-317`）。
- 不新增 FEC_OPEN/FIN/RST：主路径已有，且每 carrier 顺序可保证先 OPEN 后 DATA。

### 5.6 CFG / FBK

`[设计]` v7.0 保持 48 B body 不变，但 `h.sid` 从 0 改为该流 sid（单流时就是唯一流；N>1 时 shape 冻结，FBK 按流报告）。v7.1 再扩展为每流独立切换：

- CFG 的 `groups` 字段是流内 `G_i`；`switch_pkt/block_base` 是流内值。
- FBK 的 `carrier_bitmap` 是物理 carrier；流的 bitmap 只在自己的 group 范围置位。
- 发送端聚合各流的 `expected/received`，控制器仍按会话决策。

### 5.7 兼容与测试向量

- 旧 0.2.x 对端在第一个 HELLO 失败（version 3 vs 4）；FEC conv 0x1006 vs 0x1007 也在 `tbt_fec_hdr_check()` 失败。两端必须同升。
- 同一提交更新：`host/test_fec.c` 的 conv/header/CFG/ACK/NACK 向量；新增 ALLOC pack/unpack、bitmap 映射、每流 credit/replay/FIN 向量；`host/test_parse.c` 的 OPEN body 与 FIN 语义。
- `DESIGN.md` 的 wire 段、`CHANGELOG.md`、`README*` 的版本/限制同步；`review/lint.sh` 要求新帧类型名出现在 `DESIGN.md`（`review/lint.sh:329-330`）。

---

## 6. 最小实现路径与文件所有权

### 6.1 阶段

**Phase 0：wire + host（不改内核行为）**

- 升 `TBT_FEC_CONV/TBT_CONV/TBT_WIRE_VERSION_V6`；改 OPEN body 边界；改 FIN 语义注释；加 ALLOC 0x36 的 pack/unpack 与 header 校验；改 `tbt_fec_hdr_check`。
- 加每流映射 helper（`G_i/group_map`）、每流 credit 公式、replay 切片 helper；host 单元测试覆盖单流（G=32）与两流（G=16/16）的映射、槽不冲突、FIN。
- 验收：host 全绿；单流 QEMU 只改版本号，行为不变。

**Phase 1：flow 对象，单流等价**

- 把 `tbt_fec_tx/rx` 拆成会话 scratch + `tbt_fec_flow`；flow 表按 sid；`w_fec_tx/w_fec_rx` 每轮取一条流。
- 单流时 `G=32`、bitmap 全 1，所有公式退化为 HEAD；QEMU 单流吞吐/时延/丢包与基线一致。
- 删除 `tbt_fec_sid_accept()` 的 `cur/barrier` 逻辑，改为 flow 查找；`b->sid` 接入 ready/deliver 校验。
- 验收：host + QEMU 单流回归；KASAN 无 UAF。

**Phase 2：两流静态分配（解锁 iperf3）**

- 删除近端单流闸门；OPEN body 携带 bitmap；远端按 bitmap 创建 flow。
- 每流 ACK/NACK/FIN/credit；交付隔离（一条流阻塞不影响另一条）。
- 临时准入策略：第一条流最多 16 groups（测试配置 `fec_max_groups_per_flow=16`），留 16 给第二条；第二条直接拿 free bits。此阶段不做再平衡。
- 验收：iperf3 control+data 两条并发，双向字节精确；两条 `tbt_burst` 各自校验；一条流 RST/关闭不影响另一条；`fec_drop_unknown_sid=0`。

**Phase 3：ALLOC 再平衡**

- 加 ALLOC 0x36、`w_fec_alloc`、仲裁者、drain/commit、group 归还；删除 Phase 2 的临时上限。
- 公平份额 `32/N`；N=1..32；等待准入不再立即 RST。
- 验收：3/4/8 条流；一条关闭后 group 归还；新流等待上界；donor 的字节流在缩容后完整；无 group 重叠。

**Phase 4：自适应/SC-LDPC/文档**

- v7.0 冻结 N>1 的 shape；v7.1 每流 CFG/FBK；FBK 聚合。
- SC-LDPC 缝：每流 codec；`mode=3` 仍拒绝，直到 D 的实现落地。
- stats/proc 每流行；CHANGELOG/DESIGN/README 同步；`review/lint.sh`。

### 6.2 文件所有权

| 所有者 | 文件 | 内容 |
|---|---|---|
| W（wire/host） | `tbtun/tbt_uapi.h`、`tbtun/tbt_fec.h`（wire 结构/helper）、`tbtun/tbt_fec.c`（replay/credit）、`host/test_fec.c`、`host/test_parse.c` | 版本、OPEN/FIN/ALLOC、映射/credit/replay helper、向量 |
| F（FEC 数据面，单所有者） | `tbtun/tbt_fec_data.c`、`tbtun/tbt_fec.h`（内部）、`tbtun/tbt_fec_adapt.*` | flow 对象、TX/RX、NACK、交付、分配仲裁的 data-plane 半边 |
| T（主路径） | `tbtun/tbt_tx.c`、`tbtun/tbt_rx.c`、`tbtun/tbt_carrier.c` | OPEN body、FIN body、announce/路由、ACK/FIN 按 sid |
| S（会话/生命周期） | `tbtun/tbt_session.c`、`tbtun/tbt_sock.c`、`tbtun/tbtun.h`、`tbtun/tbt_procfs.c` | 准入、flow 表/流绑定、detach/kill、计数 |
| Q（验收） | `tests/`、`review/`、`benches/` | host/QEMU 脚本、基线、文档 |

`[设计]` `tbt_fec_data.c` 是 5990 行的单文件，F 必须唯一所有者；W/S 可在 Phase 0/1 并行，T 在 wire 冻结后并行。`tbt_fec.h` 的 wire 部分归 W、内部结构归 F，先由 W 冻结接口再交给 F。**不要让两个代理同时编辑 `tbt_fec_data.c`。**

`[设计]` `w_fec_alloc` 是新的会话工作项；`enum tbt_sess_work` 的顺序是
destroy cancel 顺序的一部分（`tbtun.h:986-1000` 的注释、`tbt_session.c:1298-1314`
的取消逻辑）。新增项必须同时更新枚举、`tbt_sess_dw()` 和 destroy 的取消顺序，
否则会话销毁会漏取消一个持引用的 work。

`[设计]` Phase 4 清理陈旧注释：`tbt_fec.h:67,71` 的 `out[40]/in[40]`（实际 48）、
`tbt_session.c:914-916` 与 `tbt_sock.c:704-707` 的“FEC 记录无 stream id”、
`review/goal-v6-interfaces.md:120,207,1028` 的旧结论。v7 文档引用新事实，不复制旧句。

### 6.3 每阶段验收门

- Phase 0：`host/test_fec`、`host/test_parse` 全绿；单流 QEMU 版本升级后无回归。
- Phase 1：单流 16 MiB 传输字节精确；吞吐/时延与 HEAD 基线差 <5%；KASAN/refcount 干净。
- Phase 2：两流并发字节精确；一条流 stall 时另一条吞吐不降；iperf3 通过。
- Phase 3：3/4 流公平；再平衡后无丢字节；group 无重叠；等待准入 <2 s。
- Phase 4：自适应单流回归；多流冻结 shape 有明确日志；文档/lint 通过。

---

## 7. 风险清单

| # | 风险 | 证据 | 影响 | 缓解 | 验证 |
|---:|---|---|---|---|---|
| 1 | 再平衡时旧 group 被提前复用 | 旧块/旧记录仍在途 | 串流、块槽冲突 | 两阶段 drain/commit；frontier ≥ P0 才归还；prev_bitmap 保留到 P0 | QEMU 缩容时双向校验；`fec_group_overlap=0` |
| 2 | 两条流 bitmap 重叠 | OPEN/ALLOC 伪造或竞态 | arena/replay 槽冲突 | `sess->lock` 下检查不相交；重叠即 `-EPROTO` | fuzz + 恶意 OPEN 测试 |
| 3 | 单流回归 | 拆分 flow 对象 | 256-carrier 性能下降 | G=32/全 1 bitmap 走原公式；Phase 1 基线门 | QEMU 16 MiB 吞吐/时延对比 |
| 4 | 旧端不兼容 | conv/HELLO 升版 | 混版隧道断 | 硬升级；HELLO v4 精确比较；CHANGELOG 声明 | 混版 QEMU 预期失败 |
| 5 | FIN 语义变化 | pkt_end/pkt_base 改流内 | 关闭挂起/丢尾 | 每流 total；host 向量；FIN 先于 OPEN 的丢弃计数 | 关闭/half-close/churn |
| 6 | ACK/NACK 路由错误 | 现状不看 h.sid | 信用/修复错流 | 按 sid 查 flow；未知 sid 丢弃；clamp 到该流 tx_seq | 两流交叉 ACK/NACK 注入 |
| 7 | 每流 ring/codec 内存 | ring 1 MiB/流、codec G_i 组 | 多流内存上升 | ring 按 credit 定尺；codec 总和 = total_groups 组；flow 上限 total_groups | `slabinfo`/KASAN；32 流压力 |
| 8 | 锁序/RCU | flow 表在 carrier RX（softirq）查找 | UAF/死锁 | flow 表 RCU + refcount；`sess->lock` → flow lock；不在自旋锁内分配 | KASAN + lockdep |
| 9 | 自适应与再平衡互踩 | 两者都移动 base_pkt/block | 映射错乱 | 每流 `switch_state` 单飞；v7.0 N>1 冻结 shape | 切换/再平衡并发测试 |
| 10 | SC-LDPC 契约未定 | 仓库无实现 | 接缝返工 | 只定义每流 codec 缝；`mode=3` 继续拒绝 | 文档评审 |
| 11 | 公平性/饿死 | 等权分配 + 轮转调度 | 小流延迟 | 最小 1 group；DRR quantum；每流 credit 独立 | 32 流 ping/bulk 混合 |
| 12 | `far_sid` 语义残留 | ARQ/proc 仍读 | 误判当前流 | FEC 只读 flow 表；`far_sid` 标为“最近/主”；proc 加每流视图 | 代码审计 + proc 对比 |

---

## 8. host / QEMU 验收

### 8.1 host

- `host/test_fec`：
  - 单流 G=32：与 HEAD 向量逐字节一致（除 conv）。
  - 两流 G=16/16：同一 `block_id`、不同物理 group 的块/记录不冲突；replay 查询互不干扰。
  - bitmap：任意置位组合的 `group_map` 推导、carrier 计算、NACK 局部/物理索引转换。
  - credit：`tbt_fec_credit_packets(G_i,n,K,slots,max)` 对 G=1..32 的单调性与上界。
  - FIN：流内 `pkt_end/pkt_base`、跨 shape 的 prev-base、final block 计算。
  - ALLOC：pack/unpack、CRC、bitmap 校验、switch_pkt 对齐。
- `host/test_parse`：OPEN 32 B body、FIN 16 B 语义、conv 0x0007、HELLO v4。
- fuzz：FEC 记录/ALLOC/OPEN 的随机字段；不得越界、不得把非法 bitmap 当合法。

### 8.2 QEMU

| 用例 | 配置 | 通过标准 |
|---|---|---|
| 单流回归 | 256 carriers, n=4,m=4 | 16 MiB 双向字节精确；吞吐/时延与 HEAD 差 <5% |
| 两流 iperf3 | 256 carriers；control+data | 两条连接都完成；无 reset；`fec_stream_refused=0` |
| 两流独立校验 | 两条 `tbt_burst`，不同 payload pattern | 每条流的字节只出现在自己的 socket；无交叉 |
| 三/四流 | 256 carriers；混合 ping/bulk | 每流完成；group 份额符合 32/N；聚合吞吐守恒 |
| 缩容再平衡 | 3 流 → 关闭 1 条 → 新开 1 条 | donor 字节流完整；新流拿到释放的 group；无重叠 |
| 一条流 stall | 一条 socket 不读，另一条 bulk | stall 流的 credit 关闭；另一条吞吐不降；stall 流恢复后继续 |
| RST/close | 中途 RST；half-close | 只有目标流死亡；其他流继续；墓碑/回收正确 |
| churn | 1000 次 connect/send/close | 无 UAF/refcount；无 fd/内存泄漏 |
| 64 carriers | n=4,m=4 → groups=8 | 1..8 流；G_i 公平；单流 8 groups 正常 |
| 恶意 bitmap | OPEN/ALLOC 重叠/越界 | `-EPROTO`；carrier 关闭；计数；无内存破坏 |
| 混版 | v7 near + v6 far | 第一个 HELLO 失败；不解析错布局 |

### 8.3 观测

- 每流 `/proc` 行：sid、group bitmap/G_i、seq/frontier/credit、deliver_bytes、NACK 数、state。
- 计数器：`fec_drop_unknown_sid`、`fec_group_overlap`、`fec_alloc_wait`、`fec_alloc_timeout`、`fec_rebalance`、`fec_flow_refused`。
- 判定：live 流的 `fec_drop_sid` 应为 0（旧 sid 的迟到记录除外）；两流测试中不得出现跨 socket 的字节。

---

## 9. 开放问题

1. **公平份额是否等权？** v7.0 等权 `32/N`；交互流只需要很小的窗口，等权会让 bulk 流在小流存在时降到 1/N。v7.1 可按流积压/需求加权（发送端可请求更多 credit，或接收端按占用率增长份额）。在需求信号确定前，不做动态权重。
2. **流关闭后是否立即增长其他流？** v7.0 可以只把 group 放回 free 池，等下一次准入再分配；v7.1 在空闲时主动 grow，减少 bulk 流在短连接关闭后的窗口抖动。
3. **v7.0 是否冻结 shape？** 本方案建议 N>1 冻结，避免每流 CFG 的首版复杂度；如果自适应必须在多流下持续，需把 CFG/FBK 的每流提交提前到 Phase 2。
4. **ALLOC 的 ACK/DONE 是否复用 FBK 标志？** 本方案用 ALLOC 自身的 flags；如果控制记录数量成为问题，可复用 FBK 的 SWITCH_DONE 语义，但需要把 ALLOC epoch 与 FBK epoch 对齐。
5. **slots/K 是否每流可调？** 本方案固定会话 slots=8、K=7，按 group 切片；如果某条流需要更深窗口，可在 group 份额内提高 slots，但会改变 arena 切片算术，列为 v7.2。
6. **SC-LDPC 的 share↔codeword 映射**：仍是 D 的开放契约（`review/sc-ldpc-0.3.0-design.md:252-268`）；v7 只保证每流独立 G_i/codec，不定义码字打包。
7. **40 位 vs 64 位 sid**：FEC 头是 u64，主路径是 40 位；v7 要求 FEC sid 落在 40 位域（近端分配即满足），超出则丢弃。是否升主路径 sid 域，不在 v7 范围。

---

## 附：关键行号索引

| 主题 | 位置 |
|---|---|
| 单流闸门 | `tbtun/tbt_session.c:913-932`；`tbtun/tbt_sock.c:692-718` |
| 近端 pending/reset | `tbtun/tbt_sock.c:762-836` |
| 远端 bind/far_sid | `tbtun/tbt_session.c:934-1014` |
| 屏障/复位 | `tbtun/tbt_session.c:1316-1323`；`tbtun/tbt_fec_data.c:2733-2799` |
| sid 过滤 | `tbtun/tbt_fec.h:148-153`；`tbtun/tbt_fec_data.c:4601-4609` |
| FEC 48 B 头 | `tbtun/tbt_uapi.h:157-190`；`tbtun/tbt_fec.h:45-65` |
| conv 历史 | `tbtun/tbt_uapi.h:117-155` |
| TX packetize/换流 | `tbtun/tbt_fec_data.c:1426-1533,1639,1653-1703` |
| TX submit | `tbtun/tbt_fec_data.c:2224-2271` |
| RX 放置/窗口 | `tbtun/tbt_fec_data.c:5140-5300` |
| RX 交付 | `tbtun/tbt_fec_data.c:2806-2843,2906-3350` |
| NACK | `tbtun/tbt_fec_data.c:3471-3920`；`tbtun/tbt_fec.h:212-216` |
| credit | `tbtun/tbt_fec.h:299-355`；`tbtun/tbt_fec_data.c:2488-2530,4719-4795` |
| FIN | `tbtun/tbt_tx.c:347-427`；`tbtun/tbt_fec_data.c:1402-1424,5461-5579` |
| 流生命周期 | `tbtun/tbt_session.c:1017-1067,1142-1197`；`tbtun/tbtun.h:413-530` |
| FEC TX/RX 结构 | `tbtun/tbtun.h:584-893` |
| 内存预算 | `tbtun/tbt_fec_data.c:82,5753-5759,5890-5900`；`tbtun/tbt_fec.h:299-304` |
| replay ring | `tbtun/tbt_fec.c:459-555` |
| OPEN/FIN/RST | `tbtun/tbt_rx.c:448-499`；`tbtun/tbt_uapi.h:388-403` |
| SC-LDPC 保留 | `tbtun/tbt_fec.h:1085-1091`；`tbtun/tbt_fec_data.c:5710-5715` |
| 自适应切换 | `tbtun/tbt_fec_data.c:1912-2100,4241-4476` |
