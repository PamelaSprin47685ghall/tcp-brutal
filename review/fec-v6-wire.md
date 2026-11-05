# FEC v6 wire / 256 carriers / grouped RLNC：T0 冻结实现说明

状态：T0 冻结基线已由用户批准；本文件记录 `w-fec-v6` 的实现口径。
基线：`master 0db5166`。实现分支：`w-fec-v6`。
范围：FEC v6 wire、256 载体基础设施、host RLNC/grouped codec、动态码率。
不在此范围：SC-LDPC（D 负责）、内核 FEC 数据面（等 A 的 N=1 修复与 T0 冻结后接）。

## 1. 40 字节 share header（小端）

| 偏移 | 类型 | 字段 | 约束 |
|---:|---|---|---|
| +0 | u8 | type | 0x30 DATA / 0x31 PARITY / 0x32 ACK / 0x33 NACK / 0x34 FEC_CFG / 0x35 FEC_FBK / 0x23 PAD |
| +1 | u8 | flags | bit0 LAST；其余位必须为 0 |
| +2 | u16 | share_index | DATA/PARITY: `(u16)share_index < (u16)n + (u16)m`；控制帧为 0 |
| +4 | u8 | n | 数据 share 数；控制帧为 0 |
| +5 | u8 | m | parity share 数；控制帧为 0 |
| +6 | u16 | epoch | 码率 epoch |
| +8 | u64 | block_id | 发送端选择的 block/window 标识；控制帧为 0 |
| +16 | u64 | seq | DATA: 包序号；PARITY: 窗口末序号；`seq < 2^63` |
| +24 | u16 | win_len | PARITY: 覆盖包数 1..1024；DATA/控制帧为 0 |
| +26 | u16 | payload_len | DATA 实际字节数 1..S（末包可短）；PARITY 恒 S；控制帧 body 长度；PAD 0 |
| +28 | u16 | conv | 0x1001 |
| +30 | u16 | reserved | 必须为 0 |
| +32 | u32 | payload_crc32c | 仅覆盖 payload |
| +36 | u32 | header_crc32c | 覆盖 header +0..+35（含 payload_crc32c 字段） |

接收端顺序：`conv/type/长度` → `header_crc32c` → 字段语义 →（DATA/PARITY：ChaCha8 `body_xor` 解密）→ `payload_crc32c`。framing/header CRC/字段语义失败关闭该 carrier；payload CRC 失败只丢该记录并计数，不交付坏字节。TX 顺序相反：先对明文算 `payload_crc32c` 并封 header，再 `body_xor` 加密 payload；控制帧不加密。

`n + m = 256` 时比较必须先转 `u16`：`(u16)n + (u16)m`，否则 `u8` 截断为 0。`share_index` 用 `u16`，0..255 全部可表示；256 被拒绝。

CRC32C 使用标准 Castagnoli 反射形式：初值 `~0`、末尾取反；主机实现与内核 `crc32c()` 用 `crc32c(~seed, data, len) ^ ~0` 对齐。向量：`CRC32C("123456789") = 0xe3069283`。

## 2. 控制帧

控制帧是完整记录，不是 share；header 的 `n/m/share_index/win_len/block_id` 必须为 0。ACK 的 `seq=next_deliver`，NACK 的 `seq=首个缺失 seq`，FEC_CFG/FEC_FBK/PAD 的 `seq=0`。控制帧不得排在 partial DATA 记录中间：载体写入以“整记录”为单位，部分写失败就丢弃/重试整条记录，绝不在一条记录的字节之间插入控制帧。

### ACK（16 B）

| 偏移 | 类型 | 字段 |
|---:|---|---|
| +0 | u64 | next_deliver |
| +8 | u32 | credit_packets |
| +12 | u32 | rtt_us |

### NACK（12 B/项）

| 偏移 | 类型 | 字段 |
|---:|---|---|
| +0 | u64 | missing_seq |
| +8 | u16 | count（>=1） |
| +10 | u16 | reserved = 0 |

body 长度必须是 12 的倍数；最多 `TBT_FEC_PAYLOAD_MAX / 12` 项。

### FEC_CFG（32 B）

| 偏移 | 类型 | 字段 |
|---:|---|---|
| +0 | u32 | epoch |
| +4 | u16 | n |
| +6 | u16 | m |
| +8 | u16 | w |
| +10 | u16 | k |
| +12 | u8 | d |
| +13 | u8[3] | reserved = 0 |
| +16 | u64 | seed |
| +24 | u32 | crc32c，覆盖 +0..+23 |
| +28 | u32 | reserved = 0 |

### FEC_FBK（48 B 总长，含 32 B bitmap）

| 偏移 | 类型 | 字段 |
|---:|---|---|
| +0 | u16 | epoch |
| +2 | u8 | m_current |
| +3 | u8 | m_target |
| +4 | u32 | p_ewma_ppm |
| +8 | u16 | expected_shares |
| +10 | u16 | received_shares |
| +12 | u32 | reserved = 0 |
| +16 | u8[32] | carrier_bitmap，bit i = carrier i 本 epoch 有丢包 |

`expected_shares/received_shares` 用 u16：1 s epoch、8 KiB 包、1 G 物理口约 15k share/s，不会溢出。若未来 epoch 或包率变化使上界超过 65535，必须升版本，不得静默回绕。

256 位 bitmap 独立于旧 ARQ 的 `path_ack_mask`（u16）。旧 ARQ 仍只认 16 槽；FEC 不读也不写旧字段。

## 3. 载体层：256 slots，旧 ARQ 仍 16 槽

- `TBT_MAX_CARRIERS_LIMIT = 256`：只用于载体槽、`sess->car[]`、procfs 遍历、模块参数上限。
- `TBT_LEGACY_MAX_CARRIERS = 16`：旧 wire 的 `path_id/path_ack_count/path_ack_mask`、FBK 16 项、WINDOW 16 项继续用它。旧会话 `n+m>16` 在 FEC 数据面落地前拒绝。
- `max_carriers` 默认 256；`carrier_sockbuf_kib` 默认 64（内核会把设置值翻倍，实际 snd+rcv 约 256 KiB/carrier）；`carrier_buffer_budget_mib` 默认 128。创建会话前检查 `(n+m) × 4 × sockbuf <= budget`。
- `fec=off` 且 `n+m>16`：解析/会话拒绝，错误文本 "legacy ARQ supports n+m<=16; set fec=on for 256 carriers"。`fec=on` 在 FEC 数据面落地前由 `tbt_session_create()` 返回 `-EOPNOTSUPP`；parser/procfs 仍接受并渲染 256 行。
- `fec=on` 时旧键 `dup`/`replay`/`max_inflate`/`hole_budget_ms`/`rx_reorder_bytes`/`tx_retain_bytes` 继续解析/渲染（保证旧配置回环），但 FEC 数据面不读。尤其 `dup` 在 FEC 路径绝不复制 DATA/PARITY；重复由 parity 方程承担。
- 默认 portblock 40000–44095（4096 端口）。配置的 block 至少要有 `n+m` 个端口；QEMU 小 N 仍可用 41000–41015。
- poll 合并：session 级 50 ms 一次遍历所有 carrier；不再每 carrier 一个 delayed work。保留 provisional join deadline、connect 重试、stall/lost 检测、queue re-arm 和 `poll_rescues` 计数。
- procfs `peers` 每 carrier 一行，256 行由 seq_file 动态增长；不分配固定 16 行缓冲。

## 4. Host RLNC / grouped codec

`tbtun/tbt_fec.c` 是 host/kernel 双编译的纯函数：

- `tbt_fec_rlnc_*`：单组 GF(2) 滑窗 RLNC。`seed = nonce ^ ((g*m + r) × golden)`，splitmix64 选 `d` 个互异位置；系数不上线。
- `tbt_fec_grouped_*`：32 组 × 8 share（或小 N 的 1 组），每组独立 codec。
- 1 share/carrier 映射（T0 §3.6）：`carriers = groups × (n+m)`；`group = carrier / (n+m)`；`share = carrier % (n+m)`，且 `share == header.share_index`。
- 条带化：`group = seq % G`；`block_id = (seq / G) / k`（组内 generation，不编码 group、不做 stream demux）；PARITY 覆盖 `seq, seq-G, ..., seq-(L-1)G`。
- `d = w` 在 `w>1` 时被 `tbt_fec_rlnc_new()` 拒绝：所有 parity 行都是全 1 行，秩 1，M 无效。生产一律 `d = w/2`。
- 解码是 GF(2) 消元；`STUCK` 时不写任何恢复字节，绝不交付坏数据。

动态参数（p 为组内 share 丢包率，ppm）：

| p | 每组 M | N | W | d | 码率 | 形态 |
|---:|---:|---:|---:|---:|---:|---|
| ≤1% | 3 | 5 | 10 | 5 | 62.5% | grouped 32×8 |
| ≤20% | 4 | 4 | 8 | 4 | 50% | grouped 32×8 |
| ≤30% | 5 | 3 | 6 | 3 | 37.5% | grouped 32×8 |
| >30% | 6 | 2 | 4 | 2 | 25% | grouped 32×8 fallback |

single 256：仅 `carriers=256` 且 `p<=20%`，M=64/N=192/K=192/W=384/d=64。p>20% 的 single 参数在 CPU 上不可行，`tbt_fec_params_single()` 返回 `-ERANGE`。

SC-LDPC：`TBT_FEC_CODEC_SC_LDPC` 是保留值；`tbt_fec_params_check()` 目前拒绝它，`tbt_fec_params_dynamic()` 不会返回它。D 接入时补实现和 wire 映射，不改 40 B header。

ChaCha8 密钥：按用户裁决走 **HELLO v6 body**（T0 §2.3），FEC_CFG 保持 32 B，不新增 0x36。B 不在 FEC 头/控制帧里携带 key；C 负责 HELLO body 扩展。

## 5. 与 A/C/D 的交接

- **A（w-window / tbt_tx.c、tbt_rx.c）**：本分支不改 `tbt_tx.c`/`tbt_rx.c`。载体层 poll 合并和 256 槽数组会改变 `tbtun.h` 的 `TBT_MAX_CARRIERS_LIMIT`，合并时以 A 的 N=1 修复为准；旧 ARQ 的 16 槽语义不变。
- **C（载体/HTTP/控制面/crypto）**：新 FEC 记录走现有 `tbt_carrier_send()` 的整记录写入；部分写返回 `-EIO`，调用方必须整记录重试或丢弃，不能在中间插入控制帧。`tbt_parse.c` 新增 `fec=off|on`、`mode=`、`w=`、`carriers=`；`fec=on` 在 FEC 数据面落地前返回 `-EOPNOTSUPP`。
- **C 的 ChaCha8 钩子**：TX 用 `tbt_fec_hdr_seal(h, plaintext)` 后 `tbt_crypto_body_xor(ctx, h->seq, h->type==PARITY, payload, h->payload_len)`；RX 先 `body_xor` 再 `tbt_fec_hdr_verify`。只对 DATA/PARITY；控制帧跳过。key 走 HELLO v6 body（T0 §2.3），FEC_CFG/40B header 不携带 key。
- **方向隔离**：每端为自己的 TX 生成独立 `crypto_nonce[8]`，填进自己的 HELLO v6 body +26；RX 用对端 HELLO 的 nonce。session 维护 tx/rx 两个 `tbt_crypto_ctx`：FEC 层用本地 nonce 初始化 tx ctx、用对端 nonce 初始化 rx ctx，绝不复用一个 ctx 同时 seal/open。B 不实现 HELLO 或 crypto ctx；B 的 `tbt_fec.c` 只接收 C 传入的两个 ctx，不改 wire/ABI。
- **D（SC-LDPC）**：保留 `TBT_FEC_CODEC_SC_LDPC` 和 `tbt_fec_params` 的 codec 字段；D 提供 encode/decode 与参数选择，复用同一 40 B header 和控制帧。不要在 `FEC_CFG` 里加 key；ChaCha8 密钥属于 session/HELLO 层，需另行冻结。

## 6. 验证

- `make host-test`：header/CRC/bitmap/控制帧/RLNC/grouped/动态参数/partial-DATA framing。
- `make host-test-san`：ASan/UBSan。
- `make check`：模块构建（`tbt_fec.c` 进入 `tbtun-objs` 后）。
- `review/lint.sh`：机械不变量。
- QEMU 小 N：`n=4,m=1,carriers=5` 旧 ARQ 回归；slirp 不用于吞吐判定，256 carrier 不在 slirp 上跑。
