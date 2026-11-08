# SC-LDPC 下行 codec 交接（c8d5457）

状态：codec/ops/host 已完成，`tbtun/tbt_fec_sclc.c` 与 `tbtun/tbt_fec_sclc.h`
提供 `tbt_fec_ops_sclc`（`mode=3`、`coeffs=-ENOSYS`）。数据面接线未做，本文件
只记录集成 owner 必须知道的契约与缺口。

## 1. 参数与映射

- `block=256`、`L=8`、`N=2048`、`rate=0.50`；`n_data=1024`、`n_par=1024`、
  `n_chk=1024`。
- 一个 FEC block：`n+m=256` shares = 128 data + 128 parity；8192 codewords；
  128 个 batch，每 batch 64 codeword。
- 映射是**交错 bit-plane**：data share `s` 的 byte `64*b+c`、bit `j` 是
  codeword `64*b+c` 的 payload bit `8*s+j`；parity share `s` 用同一布局装
  parity bit `8*s+j`。batch `b` 的段是 share 的 `64*b .. 64*b+63` 字节。
- 一个 share 丢失 = 每个 batch 的 64 codeword 同一 8-bit 段擦除（共享 mask），
  每个 codeword 只丢 `8/1024` payload 位，peeling 仍有种子。
- `ops->new` 只接受 `n=m=128, w=256, s=8192, d=0, k=0|500`；`L` 固定 8，
  wire 不新增字段。

## 2. 集成 gap（必须由数据面 owner 处理）

状态（0.3.0 接线，commit e029909 / fbcc21a / b7f570f）：gap 1 已解决；
数据面 `mode=3` 已接线（下行二进制单组、上行 GF(16) grouped），
per-direction INIT 已实现，adaptive 在 mode=3 强制 off。

1. ~~**`tbt_fec_cfg_unpack()` 的 `w >= k` 检查会拒绝 SC-LDPC 的 w=256/k=500**~~
   已解决：unpack 按 (d, k vs w) 区分三种编码——RLNC `d>=1, k<=w`；
   下行二进制 `d=0, k∈100..1000`（k 是千分码率）；上行 GF(16)
   `k>w`（k=block, w=L, d=rate/256）。codec 工厂仍是精确参数集的权威。
2. **连续 codeword-major 映射未实现、未测。** 当前只支持交错 bit-plane；
   原型 `--corr` 的 118.99 MB/s 只属于交错口径，不要拿它当连续映射的数字，
   也不要混用两种布局的恢复率。

### 2.1 接线后新增的残余风险

- **L 必须来自对端 INIT，不能用本地 L**：0x1006 的 FEC_CFG 复用字段携带
  图参数——下行 `w=block, k=rate_ppm, d=0`；上行 `k=block, w=L,
  d=rate/256`。首连失败的直接原因就是接收端用本地 L=8 安装 near 的
  上行 INIT（应为 L=16），图错、share 全解不出。`tbt_fec_sclc_shape_of_cfg()`
  现在只从 INIT 取参数，本地 L 只用于本端 TX。
- **短块放大**：2 ms 静默关闭一个块时，SC-LDPC 仍发满 m 个 parity
  （下行 128 条 ≈ 1 MiB，上行 24 条）。码率 0.5 只在满块成立。
- **NACK 修复粒度**：下行 256-share 块在 p=0.5 时一次 NACK 最多 64 share，
  修复需要多轮；30 s stall 预算可能先到。
- **0.3.0 发布状态（fix43，2026-11-08）**：1 GiB 单流 737.6 s、
  11.65 Mbit/s、sha256 一致；replay 高水位 4.9 MB（< 5 MiB）、
  backpressure 0、无 reset/holes、无 workqueue hog。
- **0.3.1 优化项：`fec_replay_miss_never_stored=42,963`**（1 GiB run）。
  NACK 点名了 replay 环里从未存在的 share。最可能的来源是短块：接收端
  在 parity 的 `win_len` 到达前把全部 n 个 DATA share 都当作缺失并 NACK，
  其中 `[win_len, n)` 是发送端从未写的 padding 位置，环里自然没有记录。
  每次这类 NACK 都浪费一个往返。0.3.1 可做：(a) 接收端在 real 未知时
  只请求已看到缺口的 share，或先等 parity 定 cut；(b) 发送端对 padding
  位置的请求回一个带 `win_len` 的 marker，让接收端立刻收敛。
  不阻塞 0.3.0。
- **修复突发与 replay 环**：fix42 首连里，接收端在一次初始突发中请求
  1348 个 share（11 MB），把 8 MiB replay 环打满并触发 8 次背压。
  现在 replay 队列到 50% 时 packetizer 让路（不再产生新 DATA），
  入队也按 4 条/NACK 节流；环的 75% 高水位仍用于 flush 节流。
  重发去重靠 `tbt_fec_replay_dup()`（一个 RTT 内）与
  `tbt_fec_nack_share_skip()`（请求窗口内不重复点名）。
- **解码成本**：下行一次块解码 128 batch；本机 109–119 MB/s，HND0
  8–11 MB/s。失败后退避 20→200 ms（`fec_decode_backoff`）。块码的部分
  恢复现在会提交：早先"整块全恢复才提交"把 parity 已恢复的 share 丢掉，
  一个块的重修实测 102 s。
- **INIT epoch=1**：0.3.0 adaptive off，无冲突；0.3.1 的切换 epoch 必须
  从 2 起，或把 INIT 与切换 epoch 分开。

## 3. 性能口径

- 本机 Ryzen：encode 131–138 MB/s，decode 109–119 MB/s（单 share 缺失，
  128 batch 全解）；ASan 下 62/46 MB/s。payload 口径是 1 MiB/block。
- HND0 老 CPU 的原型实测是 **8–11 MB/s**（2 vCPU，bit-sliced 标量）。
  109–119 不是 HND0 数字，不能据此说“G 口已满”；HND0 端到端仍需实测。

## 4. 测试与恢复率

`host/test_fec_sclc.c`（`make host-test`、`make host-test-san`）覆盖：
参数/ops 表、roundtrip、byte-exact 映射（单 bit 只落在 codeword 0）、单 share
恢复、短末块、stopping set（全擦除返回 `-EAGAIN` 且不写缓冲区）、p=0.3/0.4/0.5
share 级擦除；每个恢复的 share 都与原始数据 `memcmp`。

share 级恢复率（8192 codeword 全恢复）：p=0.3 **1.000**、p=0.4 **0.821**、
p=0.5 **0.037**。这是 NACK/replay 的输入，不是 codeword 级 0.999；
p=0.5 靠 NACK 兜底。

## 5. 范围

未改 `tbtun/tbt_fec_data.c`、`tbtun/tbt_fec_adapt.*`；未改 wire
（`TBT_FEC_CONV` 仍是 `0x1006`，FEC_CFG 48 B 不变）。内核对象未在本机编译：
`/lib/modules/7.2.9-1-MANJARO/build` 不存在；代码只用 `kvzalloc/kvfree` 与
`linux/string.h`，与 `tbt_fec.c` 同依赖。
