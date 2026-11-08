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

1. **`tbt_fec_cfg_unpack()` 的 `w >= k` 检查会拒绝 SC-LDPC 的 w=256/k=500**
   （`tbtun/tbt_fec.c:790`：`if (!c->k || c->w < c->k || ...) return -EPROTO;`）。
   数据面 owner 必须按 codec 放宽该校验，或改用另一种 k 编码；不能让
   FEC_CFG 在接收端先于 codec 建立就被静默拒绝。
2. **连续 codeword-major 映射未实现、未测。** 当前只支持交错 bit-plane；
   原型 `--corr` 的 118.99 MB/s 只属于交错口径，不要拿它当连续映射的数字，
   也不要混用两种布局的恢复率。

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
