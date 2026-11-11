# tbtun 0.8.0 黄金向量规范（F# 独立实现对照）

状态：0.8.0 Step 1 的线格式冻结稿。实现以 `tbtun/` 下 C 代码为准；本文与
`tests/golden_v080/` 的二进制/JSON 逐字节对应。任何偏移、长度或字节序变化
都必须同时提升 `TBT_CONV`，并重新生成向量。

> 2026-11-12：`tests/golden_v080/` 与生成器已删除（无读取者，且生成器
> 链接活代码，已从 0.8.0 漂移到 0.10.0，无法再证明 0.8 线格式）。本文保留
> 为 0.8 线格式的历史参考；文中的目录路径仅在 0.8 发布 tag 上有效。

## 1. 版本与常量

| 名称 | 值 | 说明 |
|---|---|---|
| `TBT_VERSION` | 0.8.0 | 模块版本 |
| `TBT_CONV` | `0x000b` | 32 字节旧帧头 / HELLO 帧头的 conv；0.7.3 的第一条 HELLO 即被拒 |
| `TBT_FEC_CONV` | `0x100b` | 48 字节 FEC 记录头 |
| `TBT_WIRE_VERSION_V6` | 8 | HELLO body[0] |
| `TBT_FEC_S` / `TBT_FEC_PAYLOAD_MAX` | 1024 | 每个 DATA/PARITY 槽的线上字节数 |
| `TBT_FEC_REC_MAX` | 1072 | 48 + 1024，raw 模式的记录长度 |
| http-kal chunk | 1079 | `"430\r\n"` 5 + 1072 + `"\r\n"` 2 |

整数一律小端，除非明确写“大端”。CRC32C 为 Castagnoli 反射式：poly
`0x82f63b78`，init `~0`，final xor；新 CRC 的 seed 为 0。校验值
`"123456789"` = `0xE3069283`（见 `crc32c.json`）。

## 2. FEC 记录头（48 字节）

| 偏移 | 类型 | 字段 |
|---|---|---|
| +0 | u8 | type：0x30 DATA、0x31 PARITY、0x32 ACK、0x33 NACK、0x34 CFG、0x35 FBK |
| +1 | u8 | flags：bit0 LAST，bit1 PADDED；mask `0x03`，其他位必须 0 |
| +2 | u16 | share_index |
| +4 | u8 | n（DATA share 数） |
| +5 | u8 | m（PARITY share 数） |
| +6 | u16 | epoch（仅 PARITY：share win_len-1 的真实长度，0 表示满 S） |
| +8 | u64 | block_id |
| +16 | u64 | seq：DATA `block_seq0 + group*n + share`；PARITY `block_seq0` |
| +24 | u16 | win_len：PARITY 覆盖的 packet 数；PADDED DATA 的真实字节数 1..1024 |
| +26 | u16 | payload_len：本记录 body 的字节数 |
| +28 | u16 | conv = 0x100b |
| +30 | u16 | group（DATA/PARITY）；控制记录必须 0 |
| +32 | u64 | sid（流 ID） |
| +40 | u32 | payload_crc32c，覆盖 body 明文 |
| +44 | u32 | header_crc32c，覆盖 +0..+43 |

### PADDED DATA

短 DATA（真实长度 `L < 1024`）在线上仍占满一个槽：

- `flags |= 0x02`
- `payload_len = 1024`
- `win_len = L`（1..1024）
- body 的 `[L, 1024)` 全零，然后对完整 1024 字节计算 CRC 并做 ChaCha8

满 1024 字节的 DATA 不置 PADDED，`win_len = 0`。PARITY 永远不置 PADDED：
非空 PARITY 的 `payload_len = 1024`；空块标记仍是 48 字节、`win_len =
payload_len = 0` 的唯一例外，它不携带应用字节，F# 端按控制记录处理。

`header_crc32c` 在 `payload_crc32c` 之后计算，计算时 +44 置 0。接收顺序：
conv/type/长度 → header CRC → 解密 body（若启用）→ payload CRC。任何 CRC
失败都只丢该 share，不拆连接。

## 3. HELLO v8

FEC 会话的 HELLO body 固定 66 字节：

| 偏移 | 内容 |
|---|---|
| +0 | wire version = 8 |
| +1 | role（near=2，far=3） |
| +2..17 | 16 字节 session nonce |
| +18 | u16 payload_max = 8192（旧 ARQ 上限） |
| +20 | u16 chunk_max = 8228（旧 ARQ 上限） |
| +22 | `profile | (generation << 2)`；http-kal=1，raw=3 |
| +23 | crypto：bit0 enabled，bit1 key-in-HELLO |
| +24 | key_id：0 预共享，1 本 HELLO 携带密钥 |
| +25 | reserved = 0 |
| +26..33 | 本方向 8 字节 crypto nonce |
| +34..65 | 32 字节会话密钥（key_id=0 时全零） |

`hello_v8_frame.bin` 是 32 字节帧头 + 66 字节 body：帧头 type=0x22、
conv=0x000b、body_len=66、seq=1、sid=0x0102030405。`hello_v8.json` 列出
每个字段和 body CRC。

## 4. ChaCha8

Bernstein 布局，不是 RFC 8439：

- state[0..3] 常量 `"expand 32-byte k"`
- state[4..11] 32 字节 key，小端
- state[12..13] 64 位 block counter，小端
- state[14..15] 8 字节 nonce，小端
- 8 轮；每 64 字节输出一个 keystream block

FEC body 的 counter 与 nonce 折叠（见 `tbt_crypto_body_xor_flow`）：

```
nonce_word14 = nonce[0..3]  ^ (u32)sid
nonce_word15 = nonce[4..7]  ^ (u32)(sid >> 32) ^ (parity ? share_index : 0)
counter      = (seq << 8) | (parity ? 0x80 : 0)
```

然后从 `counter` 开始 XOR `payload_len` 字节。DATA 的 `share_index` 不参与
nonce；PARITY 必须参与，否则同一块的每个 parity share 会复用 keystream。
`sid == 0` 时退化为只折叠 parity/share（兼容单流）。`seq < 2^56`。

`chacha8.json` 给出 64 字节 raw KAT（counter=0）和一个 1024 字节 body
向量（sid=`0x0102030405`、seq=7、DATA、share=0）；`chacha8_body_cipher.bin`
是后者的密文。

## 5. Mux 子帧

单条 FEC 字节流内复用多条应用 TCP 连接。8 字节头，**大端**：

| 偏移 | 类型 | 字段 |
|---|---|---|
| +0 | u8 | cmd：OPEN=1、DATA=2、FIN=3、RST=4、WIN=5 |
| +1 | u8 | slot |
| +2 | u16 | len，大端 |
| +4 | u32 | stream_id，大端 |

- OPEN：`len=0`，`slot`=近端 mapping slot 0..7，`stream_id`=新流
- DATA：`len=1..1024`，`slot=0`，后跟 len 字节
- FIN/RST：`len=0`，`slot=0`
- WIN：`len`=授予对端的信用字节数（0..65535），无 body，`slot=0`

解析器只把 DATA 的 len 当 body 长度；WIN 的 len 是信用，不读 body。流控：
接收端累计 `rx_unacked`，超过 `rx_window/2` 时回一个 WIN（len 最多
65535），并从 `rx_unacked` 中扣除这一 WIN 已归还的字节；余额若仍超过
65535，在后续 WIN 中继续归还，不能清零丢弃。发送端 `tx_credit` 不足时
不能发 DATA。`mux_stream.bin/json` 是一条交错了两条流的确定性字节流，
含 OPEN/DATA/WIN/FIN。

## 6. 下行 SC-LDPC（binary，S=1024）

- `groups=1`，`n+m=256` 个 share，每 share 1024 字节
- `block=256`，`L=8`，`batches=16`（1024/64）
- share 字节 `64*b + c` 的 bit j 对应 codeword `b*64+c` 的 bit `8*s+j`；
  data share s 承载每个 codeword 的 payload bit 8s..8s+7，parity share s
  承载发送的 parity bit 8s..8s+7
- 每个 coupling block 有 n 个 payload 变量、m 个 parity 变量、m 个 check；
  payload 度 5（n<=m）或 4（n>m），check 度 ≤14
- 系统码：data shares 原样发送；parity 由编码器计算

`tests/golden_v080/sclc_downlink_*`：

- `sclc_downlink_data.bin`：128 个 data share（131072 字节），种子
  `0x10+i` 的确定性填充
- `sclc_downlink_parity.bin`：128 个 parity share（131072 字节）
- `sclc_downlink_recovered.bin`：擦除 data share 3 后解码恢复的结果，
  应与 data 文件逐字节相同
- `sclc_downlink.json`：n/m/block/L/S/seed、`data_present`/`parity_present`
  （每 share 一字节，不是位图）、恢复 share 数

编码/解码器构造参数：`new(n=128, m=128, rate_ppm=500, block=256, S=1024,
d=0, seed=17)`。

## 7. 上行 GF(16) SC-LDPC（S=1024）

- `groups=8`，每组 `n=8`、`m=24`，共 256 share
- `block=64`，`L=8`；`SCLC_SYM_PER_SHARE = 2048`（1024 字节 = 2048 个
  GF(16) 符号）
- codeword 数 `C = n*2S/(L*block/2) = n*8 = 64`
- 生产映射为 bitplane64：每 64 条 codeword 一个 batch，4 个 bit plane
  承载一个符号的 4 个 bit；一个 batch 内所有 codeword 共用 share 映射
- 系统码：8 个 data share 原样发送；24 个 parity share 由编码器计算

`tests/golden_v080/sclc_uplink_*`：

- `sclc_uplink_data.bin`：8*1024 字节
- `sclc_uplink_parity.bin`：24*1024 字节
- `sclc_uplink_recovered.bin`：擦除 data share 0 后解码恢复，应与 data
  文件相同
- `sclc_uplink.json`：参数、present 字节、恢复 share 数

构造参数：`new(n=8, m=24, block=64, L=8, S=1024, rate_256=64, seed=1)`。

### 已知限制

`S=1024, block=64, L=8` 下 `22/10` 的图需要 50% parity，超过 10 个
parity share 的 20480 个符号容量，构造器拒绝。0.8.0 的上行最高可用档为
`21/11`（65.6%）。这不是线格式变化；`tbt_fec_sclc_up_buildable_64_8()`
和 FEC_CFG 校验都会拒绝 22/10。若后续要恢复 70% 档，需改用 `L=16` 或重新
标定上行图，不能只改校验。

## 8. 文件清单与验证

`tests/golden_v080/manifest.json` 列出每个文件的字节数和 CRC32C。验证顺序：

1. 读 `manifest.json`，逐文件核对大小和 CRC32C。
2. `crc32c.json` 的 `check_123456789` 必须是 `0xE3069283`。
3. `hello_v8_frame.bin`：前 32 字节按第 3 节帧头解码，后 66 字节按
   HELLO body 解码；body CRC 与 JSON 一致。
4. `http_padded_data.chunk.bin` / `http_parity.chunk.bin`：长度 1079，
   首 5 字节 `"430\r\n"`，末 2 字节 `"\r\n"`，中间 1072 字节是 FEC 记录；
   记录头 CRC 与 body CRC 都要通过。
5. `chacha8.json`：raw KAT 与 body 向量逐字节一致；body 的 counter/nonce
   折叠按第 4 节。
6. `mux_stream.bin`：按第 5 节解析出 6 帧，字段与 JSON 一致。
7. `sclc_*`：编码输出与 `*_parity.bin` 一致；按 present 字节擦除后解码，
   恢复结果与 `*_recovered.bin` 一致。

生成器：`tests/golden_v080/gen_golden.c`。它链接真实的
`tbt_fec.c`、`tbt_crypto.c`、`tbt_http.c`、`tbt_mux.c`、
`tbt_fec_sclc.c`、`tbt_sclc_uplink.c`，不是手写常量。重新生成：

```
cc -std=gnu99 -O2 -I tbtun -o /tmp/gen_golden \
  tests/golden_v080/gen_golden.c tbtun/tbt_fec.c tbtun/tbt_crypto.c \
  tbtun/tbt_proto.c tbtun/tbt_http.c tbtun/tbt_rec.c \
  tbtun/tbt_fec_sclc.c tbtun/tbt_sclc_uplink.c tbtun/tbt_sc_ldpc.c \
  tbtun/tbt_sc_ldpc_gf16.c tbtun/tbt_mux.c
/tmp/gen_golden
```

F# 实现只需消费 `tests/golden_v080/`；C 侧测试见 `host/test_mux.c`、
`host/test_http.c`、`host/test_fec.c`、`host/test_fec_sclc*.c`。
