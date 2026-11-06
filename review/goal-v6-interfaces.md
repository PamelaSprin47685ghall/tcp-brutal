# tbtun v6 接口冻结（T0）

状态：**冻结，T0 已验收**（Main 裁决，2026-11-06；验收基线 `7105507`，745 行）。本文件后续只追加裁决/跟踪，ABI 未变。基线 `master 0db5166`。
权威来源：B（`fec-v6`）的 40B header / 控制帧提案，经用户批准为 T0 冻结基线；本文件是 T0 的唯一接口契约。
变更控制：任何 wire 字段、偏移、类型、CRC 顺序的变更，必须 B 与 integrator 书面同意，并 bump `TBT_FEC_CONV`（当前 `0x1001`）。只改文档、不改 ABI 的编辑不 bump。
范围：T0 只写文档，不改 `tbtun/` 产品代码。本文中的头文件签名是 B/C/D 的实现契约，不是已落地的代码。

---

## 0. 冻结摘要

| 项 | 冻结值 | 归属 |
|---|---|---|
| FEC wire 版本 | `TBT_FEC_CONV = 0x1001`；旧 ARQ 仍是 `TBT_CONV = 0x0005` | B |
| 记录头 | 40 字节，小端；`type`/`flags`/`share_index u16`/`n`/`m`/`epoch u16`/`block_id u64`/`seq u64`/`win_len u16`/`payload_len u16`/`conv u16`/`reserved u16`/`payload_crc32c u32`/`header_crc32c u32` | B |
| 控制帧 | ACK 16B；NACK 12B/项；FEC_CFG 32B；FEC_FBK 48B（含 32B bitmap）；PAD 0B | B |
| 载体位图 | 256 bit（32B），bit i = carrier i；新 FEC 不复用旧 `path_ack_mask` | B |
| 载体槽 | `TBT_MAX_CARRIERS_LIMIT = 256`；旧 ARQ 自己的 `TBT_LEGACY_MAX_CARRIERS = 16` | B |
| `max_carriers` | 默认 256，范围 1..256 | B |
| portblock | 默认 4096 个端口；256 carriers 的最小生产值 1024 | B |
| 缓冲预算 | 128 MiB/模块；每 carrier 实际 512 KiB 上沿；256 条推荐 64 MiB | B |
| poll | session 级 50 ms 合并；稳态不做每 carrier delayed poll | B |
| 1 share/carrier | `carrier = group*(n+m) + share_index`；`groups = carriers/(n+m)`，必须整除 | B |
| 加密 | ChaCha8，body-only；默认 `crypto=on`（`fec=on` 时）；`crypto=off` 可关 | C |
| 加密范围 | 只加密 FEC payload；40B FEC 头和 HTTP 头明文 | C |
| key 来源 | 每 session 随机 256-bit；HELLO v6 body 明文下发；`crypto_key=` 预共享覆盖 | C |
| 配置键 | `fec=` `mode=` `n=` `m=` `w=` `carriers=` `crypto=` `crypto_key=` | B/C |
| 流数 | v6 一隧道一 app stream；header 无 sid；多流留 v7 | B |
| 合并顺序 | A（w-window）→ B（w-fec-v6）→ C（w-crypto）→ D（w-sclc）→ E（w-hnd0）→ F（integration/T7） | F |

---

## 1. FEC v6 ABI

### 1.1 常量与类型集

```c
#define TBT_FEC_CONV            0x1001u
#define TBT_FEC_HDR_LEN         40
#define TBT_FEC_PAYLOAD_MAX     8192
#define TBT_FEC_REC_MAX         (TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX) /* 8232 */
#define TBT_FEC_MAX_SHARES      256
#define TBT_FEC_MAX_CARRIERS    256
#define TBT_FEC_MAX_GROUPS      256
#define TBT_FEC_MAX_W           1024
#define TBT_FEC_MAX_D           256
#define TBT_FEC_NACK_MAX_ITEMS  64   /* 64 * 12 = 768 B，单条 NACK 上限 */
```

记录类型：

| 值 | 名称 | 用途 |
|---:|---|---|
| `0x13` | OPEN | 旧控制帧，v6 沿用 |
| `0x14` | FIN | 旧控制帧，v6 沿用 |
| `0x15` | RST | 旧控制帧，v6 沿用 |
| `0x22` | HELLO | 旧控制帧，v6 body 扩展（见 §2.3） |
| `0x23` | PAD | 载体保活，`payload_len=0` |
| `0x30` | DATA | 数据 share |
| `0x31` | PARITY | 校验 share |
| `0x32` | ACK | 交付前沿 + credit |
| `0x33` | NACK | 缺失报告 |
| `0x34` | FEC_CFG | 码率参数 / seed / epoch |
| `0x35` | FEC_FBK | 丢包反馈 + 256-bit bitmap |

**不新增 `0x36`。** 新 FEC 类型集就是 `0x23` + `0x30..0x35`；HELLO/OPEN/FIN/RST 仍用旧值，靠 `conv=0x1001` 与新 body 语义区分。

### 1.2 40 字节 share header

所有多字节字段为**小端**。`struct tbt_fec_hdr` 是主机序结构；`tbt_fec_hdr_pack()` / `unpack()` 才是 wire 权威，禁止直接把结构体写到线上。

| 偏移 | 大小 | 字段 | 语义 |
|---:|---:|---|---|
| +0 | 1 | `type` | §1.1 的类型；未知类型 `-EPROTO` |
| +1 | 1 | `flags` | bit0 `LAST`；bit1..7 必须为 0 |
| +2 | 2 | `share_index` | `0..n+m-1`；数据 `0..n-1`，parity `n..n+m-1`；**按 u16 比较** |
| +4 | 1 | `n` | 每组数据 share 数，1..255 |
| +5 | 1 | `m` | 每组 parity share 数，1..255；`(u16)n+(u16)m <= 256` |
| +6 | 2 | `epoch` | 参数 epoch 的低 16 位；FEC_CFG 的 u32 为全值 |
| +8 | 8 | `block_id` | 组内 generation 号，从 0 单调；**不是 stream id，不编码 group** |
| +16 | 8 | `seq` | 全局数据包序号；各类型含义见 §1.4 |
| +24 | 2 | `win_len` | PARITY 覆盖的组内包数 L；DATA/控制帧为 0 |
| +26 | 2 | `payload_len` | DATA 实际字节数（末包可 < S）；PARITY 恒 S；控制帧 body 长度；PAD 0 |
| +28 | 2 | `conv` | 必须 `0x1001` |
| +30 | 2 | `reserved` | 必须 0 |
| +32 | 4 | `payload_crc32c` | 只覆盖 payload（`payload_len` 字节），CRC32C |
| +36 | 4 | `header_crc32c` | 覆盖 `+0..+35`（含 `payload_crc32c` 字段），CRC32C |

**为什么 header CRC 在 +36、payload CRC 在 +32。** 这是 B 已批准的布局：接收端先读 `conv/type/payload_len`，再校验 `header_crc32c`，最后校验 `payload_crc32c`。header CRC 覆盖 payload CRC 字段，防止攻击者改 CRC 字段本身。

### 1.3 CRC 与接收顺序

CRC32C（Castagnoli），语义与 Linux `crc32c()` 一致：

- `payload_crc32c = crc32c(0, payload, payload_len)`
- `header_crc32c  = crc32c(0, header_bytes_0_35, 36)`
- 自检向量：`crc32c(0, "123456789", 9) == 0xE3069283`

接收顺序：

1. `conv == 0x1001`；`type` 在已知集合；`payload_len` 在该类型的边界内（DATA 1..S；PARITY == S；控制帧 ≤ 8192；PAD 0）。
2. `header_crc32c` 校验 `+0..+35`。
3. `n/m/share_index/win_len/flags/reserved` 的语义校验；`(u16)n+(u16)m <= 256`；`share_index < (u16)n+(u16)m`。
4. `payload_crc32c` 校验 payload；DATA/PARITY 的 CRC 覆盖明文 payload（解密后校验，见 §2.4）。

失败处理分两类：

- **1–3 失败 = 字节流 framing 不可信**：`payload_len` 可能已损坏，无法安全跳到下一条记录；关闭该 carrier（`-EPROTO`），计数 `fec_proto_fail` / `fec_hdr_crc_fail`。绝不带着未知长度继续解析。
- **4 失败 = framing 可信、payload 损坏**：丢弃该 DATA/PARITY，计数 `fec_payload_crc_fail`，carrier 保持；不交付坏字节，等待重发/恢复。

任何情况下都不把坏字节交给应用。

### 1.4 `seq` / `block_id` / `win_len` / group 语义

**一隧道一 app stream。** v6 header 没有 sid；`block_id` 只标识 window/block，不做 stream demux。多流留 v7，不静默塞进 `block_id`。

```
groups G   = carriers / (n + m)          /* 必须整除；1 share/carrier */
group(c)   = c / (n + m)                 /* c = carrier 索引 */
share(c)   = c % (n + m)                 /* 必须等于 header.share_index */
```

- DATA：`seq` 是该包的全局序号；`block_id` 是该包所属组的 generation；`win_len=0`。接收端校验 `seq % G == group(carrier)`（条带化模型）。
- PARITY：`block_id` 是该组 generation；`seq` 是窗口**最后一个**全局 seq；`win_len=L` 是组内包数。覆盖的全局 seq 为
  `seq, seq-G, seq-2G, ..., seq-(L-1)G`。
- 单组模式（`mode=rlnc`，`G=1`）退化为连续窗口 `[seq-L+1, seq]`。
- ACK：`seq=next_deliver`；NACK：`seq=第一个缺失 seq`；FEC_CFG/FEC_FBK/PAD：`seq=0`。
- `block_id` 从 0 单调；epoch 切换不清零 `block_id` 与 `seq`。窗口外不可恢复时有界 reset，不重编号。

**末包。** DATA 的 `payload_len` 是实际应用字节数；最后一个 DATA 置 `flags.LAST`，`payload_len` 可为 1..S。PARITY 恒按 S 字节零填充后编码；接收端对末包按 `payload_len` 交付，剩余补零只参与解码。

### 1.5 控制帧 body

#### ACK（`0x32`，16 B）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 8 | `next_deliver`，已连续交付到的下一个包 |
| +8 | 4 | `credit_packets`，接收端还能收的包数 |
| +12 | 4 | `rtt_us`，0 表示未测 |

#### NACK（`0x33`，12 B/项）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 8 | `missing_seq` |
| +8 | 2 | `count`，从 `missing_seq` 起的连续缺失数 |
| +10 | 2 | `reserved=0` |

单条 NACK 最多 64 项（768 B）；更多缺失用多条 NACK。`payload_len` 必须是 12 的整数倍。

#### FEC_CFG（`0x34`，32 B）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 4 | `epoch`（全 32 位） |
| +4 | 2 | `n` |
| +6 | 2 | `m` |
| +8 | 2 | `w` |
| +10 | 2 | `k` |
| +12 | 1 | `d`（RLNC degree；SC-LDPC 可为 0） |
| +13 | 3 | `reserved[3]=0` |
| +16 | 8 | `seed`（与 session nonce 异或后的 PRNG seed） |
| +24 | 4 | `crc32c`，覆盖 `+0..+23` |
| +28 | 4 | `reserved=0` |

FEC_CFG 在 epoch 边界由发送端发出，重复 3 次，走任意 `part_len==0` 的 carrier。接收端按 `epoch` 建新窗口；旧 epoch 的窗口继续解码到滑出或超时。

#### FEC_FBK（`0x35`，48 B，含 32 B bitmap）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 2 | `epoch` |
| +2 | 1 | `m_current` |
| +3 | 1 | `m_target` |
| +4 | 4 | `p_ewma_ppm`（百万分之一） |
| +8 | 2 | `expected_shares`（本 epoch 已发 share 数） |
| +10 | 2 | `received_shares`（接收端已收 share 数） |
| +12 | 4 | `reserved=0` |
| +16 | 32 | `carrier_bitmap[32]`；bit i = carrier i 本 epoch 有丢包 |

**48 = 16 + 32，不是 80。** 旧 `path_ack_mask` 是 u16，新 FEC 用 256-bit bitmap，不复用旧字段。

### 1.6 控制帧不得排在 partial DATA 后面

**实测 bug 背景**：控制帧排进同一条 carrier 的写队列，而该 carrier 正在写一个 partial record；partial 写不完，控制帧永远发不出去（near `fbk_tx=253`、far `fbk_rx=0`，最终 EOF）。

冻结规则（三条同时成立）：

1. 每条 carrier 维护 `part_len` / `part_off`。**只有 `part_len == 0` 时**才允许把控制帧追加到该 carrier 的字节流。
2. 若选中的 carrier `part_len > 0`，调度器改选另一条 `part_len == 0` 的 carrier；若全部 carrier 都有 partial，控制帧进入 session 级 control queue，等 `sk_write_space` / 50 ms poll 重试，计数 `ctl_blocked_by_partial`。**绝不把控制帧插进 partial record 的中间。**
3. 控制帧在 `part_len == 0` 时优先于新的 DATA/PARITY；同一 carrier 上先控制、后数据。接收端 parser 仍按 `40 + payload_len` 识别记录边界，作为兜底。

host 测试必须覆盖：所有 carrier `part_len>0` 时注入 FEC_FBK，断言它等待而不是插队；任一条 carrier 完成 record 后，控制帧完整发出；接收端字节流无交错。

### 1.7 `tbt_fec.h` 接口（B 的实现契约）

```c
#ifndef _TBT_FEC_H
#define _TBT_FEC_H

#include "tbt_uapi.h"

#define TBT_FEC_CONV            0x1001u
#define TBT_FEC_HDR_LEN         40
#define TBT_FEC_PAYLOAD_MAX     8192
#define TBT_FEC_MAX_SHARES      256
#define TBT_FEC_MAX_CARRIERS    256

struct tbt_fec_hdr {
    u8  type;
    u8  flags;
    u16 share_index;
    u8  n;
    u8  m;
    u16 epoch;
    u64 block_id;
    u64 seq;
    u16 win_len;
    u16 payload_len;
    u16 conv;
    u16 reserved;
    u32 payload_crc32c;
    u32 header_crc32c;
};

/* wire <-> host；pack 写小端；unpack 不做语义校验。 */
int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN]);
int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h);

/* conv/type/长度/index/win_len/flags/reserved 的语义校验。
 * 返回 0 或 -EPROTO/-EINVAL；不读 payload。 */
int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload);

/* CRC32C，Linux crc32c() 语义；seed 通常为 0。 */
u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len);

/* 填 payload_crc32c 与 header_crc32c；payload 可为 NULL（PAD/控制帧无 body 时）。 */
int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload);
/* 校验两个 CRC；返回 0 / -EBADMSG。 */
int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload);

/* 256-bit bitmap；idx 0..255；越界返回 -ERANGE。 */
void tbt_fec_bitmap_set(u8 bm[32], u16 idx);
void tbt_fec_bitmap_clear(u8 bm[32], u16 idx);
bool tbt_fec_bitmap_test(const u8 bm[32], u16 idx);
u16  tbt_fec_bitmap_count(const u8 bm[32]);

/* 控制帧 pack/unpack；返回 body 长度或负 errno。 */
int tbt_fec_ack_pack(const struct tbt_fec_ack *a, u8 out[16]);
int tbt_fec_ack_unpack(const u8 in[16], struct tbt_fec_ack *a);
int tbt_fec_nack_pack(const struct tbt_fec_nack_item *it, u16 nr, u8 *out, size_t max);
int tbt_fec_nack_unpack(const u8 *in, size_t len, struct tbt_fec_nack_item *it, u16 max);
int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[32]);
int tbt_fec_cfg_unpack(const u8 in[32], struct tbt_fec_cfg *c);
int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[48]);
int tbt_fec_fbk_unpack(const u8 in[48], struct tbt_fec_fbk *f);

/* 中间码：RLNC / grouped。SC-LDPC 由 D 实现同一 ops 形状。 */
struct tbt_fec_rlnc *tbt_fec_rlnc_new(u16 n, u16 m, u16 k, u16 w,
                                      size_t s, u16 d, u64 nonce);
void tbt_fec_rlnc_free(struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_parity_count(const struct tbt_fec_rlnc *c, u32 L);
u32  tbt_fec_rlnc_coeffs(const struct tbt_fec_rlnc *c, u32 g, u32 r,
                         u32 *idx, u32 max);
int  tbt_fec_rlnc_encode(struct tbt_fec_rlnc *c, u8 *data[], u32 L, u8 *parity[]);
int  tbt_fec_rlnc_decode(struct tbt_fec_rlnc *c, u8 *data[], u8 *parity[],
                         const u8 *data_present, const u8 *parity_present, u32 L);
int  tbt_fec_rlnc_advance(struct tbt_fec_rlnc *c, int kind, u32 index);
u32  tbt_fec_rlnc_frontier(const struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_stuck_at(const struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_recovered(const struct tbt_fec_rlnc *c);
int  tbt_fec_rlnc_is_known(const struct tbt_fec_rlnc *c, u32 index);

/* codec 抽象：B 的 RLNC 与 D 的 SC-LDPC 共用；host 测试只认这张表。 */
struct tbt_fec_ops {
    const char *name;
    u8 mode; /* 1=rlnc, 2=grouped, 3=sc-ldpc */
    void *(*new)(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d, u64 nonce);
    void  (*free)(void *ctx);
    u32   (*parity_count)(const void *ctx, u32 L);
    u32   (*coeffs)(const void *ctx, u32 g, u32 r, u32 *idx, u32 max);
    int   (*encode)(void *ctx, u8 *data[], u32 L, u8 *parity[]);
    int   (*decode)(void *ctx, u8 *data[], u8 *parity[],
                    const u8 *data_present, const u8 *parity_present, u32 L);
    int   (*advance)(void *ctx, int kind, u32 index);
    u32   (*frontier)(const void *ctx);
    u32   (*stuck_at)(const void *ctx);
    u32   (*recovered)(const void *ctx);
    int   (*is_known)(const void *ctx, u32 index);
};

#endif /* _TBT_FEC_H */
```

契约：

- `data[]` 是 `L` 个 `S` 字节包；`parity[]` 是 `parity_count(L)` 个 `S` 字节行。
- `data_present` / `parity_present` 是位图（L 位 / parity 位数），1 = 已收到。
- `decode` 只填缺失包，返回恢复数；rank 不足返回 `-EAGAIN`，不静默输出错字节。
- `advance` 推进解码前沿；`kind` 语义由 codec 定义（RLNC：0=数据已知，1=数据缺失，2=parity 已知）；任何实现都不得让 frontier 越过未知包。
- `coeffs` 是 RLNC 专用；SC-LDPC 可返回 `-ENOSYS`。host 测试对 SC-LDPC 只断言 `decode` 恢复率。
- `tbt_fec_rlnc_decode` 与 `tbt_fec_rlnc_encode` 的 `L` 上限 `TBT_FEC_MAX_W`；`n+m <= 256` 必须按 u16 比较。

### 1.8 旧 ARQ 隔离：16 槽 vs 256 槽

| 常量 | 值 | 用途 |
|---|---:|---|
| `TBT_CONV` | `0x0005` | 旧 wire，**不变** |
| `TBT_MAX_CARRIERS_LIMIT` | **256** | 载体槽数组、新 FEC、256-bit bitmap |
| `TBT_LEGACY_MAX_CARRIERS` | **16** | 旧 ARQ：`path_ack_mask` u16、FBK 16 项、WINDOW 16 项、`path_gen[16]`、`path_end[16]`、FIN/RST mask |
| `TBT_FBK_MAX` / `TBT_WINDOW_BODY_LEN` | 仍按 16 算 | 旧 wire 的 body 边界，**不得**跟着 256 放大 |

规则：

- 旧会话 `n+m > 16` 在 FEC 数据面落地前拒绝：`tbt_cfg_sane()` 在 `fec=off` 时返回 `-EINVAL`，错误文本写明「legacy ARQ supports n+m<=16; set fec=on for 256 carriers」。
- `fec=on` 时 `n+m <= 256`，走新 FEC；旧 `path_ack_mask`、旧 FBK/WINDOW、旧 `path_gen/path_end` 不参与。
- `TBT_MAX_CARRIERS_LIMIT=256` 只给载体槽与新 FEC；旧 ARQ 的 16 槽结构必须改用 `TBT_LEGACY_MAX_CARRIERS`，否则 `path_ack_mask` 会被截断。
- `struct tbt_session.car[]` 扩到 256；旧 `path_lost_mask`（`unsigned long`，64 位）只服务旧 ARQ；新 FEC 用 `DECLARE_BITMAP(fec_lost_mask, 256)`。

---

## 2. ChaCha8 ABI（C 的实现契约）

### 2.1 `tbt_crypto.h`

```c
#ifndef _TBT_CRYPTO_H
#define _TBT_CRYPTO_H

#include "tbt_uapi.h"

#define TBT_CRYPTO_KEY_LEN    32   /* 256-bit */
#define TBT_CRYPTO_NONCE_LEN  8    /* Bernstein 布局，64-bit counter */
#define TBT_CRYPTO_BLOCK_LEN  64

struct tbt_crypto_ctx {
    u32 state[16];
    u64 counter;
    u8  nonce[TBT_CRYPTO_NONCE_LEN];
    bool ready;
};

/* key 可为 NULL（仅当 crypto=off）；nonce 8 字节；counter 初始块号。 */
int  tbt_crypto_init(struct tbt_crypto_ctx *c,
                     const u8 key[TBT_CRYPTO_KEY_LEN],
                     const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter);
void tbt_crypto_set_counter(struct tbt_crypto_ctx *c, u64 counter);
u64  tbt_crypto_get_counter(const struct tbt_crypto_ctx *c);

/* 原地 XOR，按块推进 counter；len 任意。 */
int tbt_crypto_xor(struct tbt_crypto_ctx *c, u8 *data, size_t len);

/* 指定 counter 的随机访问；不推进 c->counter。 */
int tbt_crypto_xor_at(struct tbt_crypto_ctx *c, u64 counter, u8 *data, size_t len);

/* body-only hook：只加密/解密 FEC payload；40B header 与 HTTP 头不动。
 * counter = (seq << 8) | (parity << 7) | (offset / 64)。 */
int tbt_crypto_body_xor(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                        u8 *payload, u16 len);

void tbt_crypto_wipe(struct tbt_crypto_ctx *c);

#endif /* _TBT_CRYPTO_H */
```

实现可在同一头文件增加 `struct tbt_crypto_pair { struct tbt_crypto_ctx tx, rx; }` 与 pair 初始化/擦除便利函数（C2 `33238ca` 已加）；这是 session 层便利结构，不改变上述函数契约、不改 wire。pair 必须保证 tx/rx nonce 不同；等 nonce 直接拒绝。

实现选择：**ChaCha8，64-bit counter + 64-bit nonce**（Bernstein eSTREAM 布局），public-domain 参考或 MIT tiny-chacha 改写均可；KAT 必须与 `review/chacha8-reference.md` §7 一致。若用 tiny-chacha 的 96-bit IV/32-bit counter，必须把 8B nonce 映射为 `00 00 00 00 || nonce` 并把 counter 扩到 64 位；32-bit counter 在 1G 下约 37 分钟回绕，**不允许**直接上线。

### 2.2 body-only 的确切范围

```
HTTP request/response headers  明文（QoS 需要）
HTTP chunk size line            明文
FEC 40B share header            明文（type/seq/share_index 供调度）
FEC payload（DATA/PARITY）      加密
```

- 加密顺序：TX 先算 `payload_crc32c`（明文），再 `tbt_crypto_body_xor()`；RX 先解密，再校验 `payload_crc32c`。
- 控制帧（ACK/NACK/FEC_CFG/FEC_FBK/PAD）**不加密**；它们不含应用字节，且需要在 FEC 头明文下调度。
- HTTP 头明文是硬要求：整条 TLS/全包加密会丢掉 HTTP 容器的 QoS 可见性。
- 职责边界（B 确认）：HELLO v6 body 的 key/nonce 由 C 的 session/crypto 层填写；FEC 层（B）只接收 tx/rx 两个 `tbt_crypto_ctx` 并调用 `tbt_crypto_body_xor`，不自己生成/交换 key/nonce。

### 2.3 key 来源与 HELLO v6 body

**已裁决（§9.1）**：目标合同写「FEC_CFG 明文下发 key」；B 的冻结基线把 key 放 HELLO 层，FEC_CFG 固定 32B，不新增 `0x36`。Main 维持 B 方案，记录为等价偏差（每会话明文 key、只混淆不认证）；B/C 不改 ABI。

HELLO v6 body（`type=0x22`，`conv=0x1001`，固定 66 B）：

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 1 | `ver=2`（旧 v6 之前是 1） |
| +1 | 1 | `role`（2 near / 3 far） |
| +2 | 16 | `nonce`（session nonce，旧字段） |
| +18 | 2 | `payload_max` |
| +20 | 2 | `chunk_max` |
| +22 | 1 | `profile` |
| +23 | 1 | `crypto`：bit0 enabled，bit1 key_in_hello |
| +24 | 1 | `key_id`：0=预共享/无 key，1=key 在 +34 |
| +25 | 1 | `reserved=0` |
| +26 | 8 | `crypto_nonce[8]` |
| +34 | 32 | `key[32]`（key_id=1 时有效；否则全 0） |

key 规则：

- `crypto_key=<64 hex>` 配置在两端：`key_id=0`，HELLO 不带 key；两端必须一致，否则 `-EPROTO` 关闭 carrier。
- 未配置 `crypto_key=`：near 在 session 创建时生成 32B 随机 key，`key_id=1`，HELLO 明文下发；far 采用（TOFU），若 far 自己配了 key 则拒绝。
- `crypto=off`：`crypto=0`，key 字段全 0；接收端不初始化 cipher。
- key 在 HELLO 中明文传输：**只混淆、不认证**；不提供对抗被动监听的机密性，也不防主动攻击。需要安全时用 WireGuard。

**每方向 nonce（必须，2026-11-06 补）**：`crypto_nonce[8]` 不是 session 共享值。每一端为自己的 **TX 方向**生成独立的 8B 随机 nonce，写进自己的 HELLO v6 body `+26`；**RX 方向使用对端 HELLO 的 nonce**。`crypto_key=` 预共享时 key 共享，nonce 仍各自独立。session 维护 **tx/rx 两个 `tbt_crypto_ctx`**；禁止一个 ctx 同时 seal/open 两个方向。这样 near/far 的 `seq` 都从 0 开始时，相同 `(seq,parity,block)` 也不会复用同一段 keystream。该语义不新增字段、不改 ABI；`crypto_nonce` 字段本来就在 `+26`。

### 2.4 默认与配置

- `fec=on` 时 `crypto` 默认 **on**；`fec=off` 时默认 off（旧路径没有解密点）。
- `crypto=off` 显式关闭；`crypto=on` 且 `fec=off` 拒绝（`-EINVAL`）。
- 每 session 独立 key；**每方向独立 8B nonce**（near TX / far TX 各一个），session 维护 tx/rx 两个 ctx；`crypto_nonce` 在 HELLO v6 body `+26` 交换。epoch 切换不换 key/nonce；carrier 重建不换 key/nonce（session 级）；新 session/方向重启必须换 nonce。
- counter 空间：`(seq << 8) | (parity << 7) | (offset / 64)`；S=8192 时 offset/64 ∈ 0..127，bit7 区分 parity；`seq < 2^56` 时无回绕。**counter 本身不带方向位**，方向隔离完全靠 nonce；禁止两端共用同一 nonce。

### 2.5 KAT 与性能门

- TC1：128-bit 全零 key/IV，8 轮，首块 `e28a5fa4a67f8c5defed3e6fb7303486...`。
- 256-bit 向量：key `00..1f`、IV `a0..a7`、counter 0，首块 `9a1e949f0d71a3a307da58730300dea7...`。
- `host/test_crypto` 必须逐字节比对；失败即 gate 失败。
- `host/test_crypto` 必须覆盖：同一 key、两个不同 nonce、相同 `(seq,parity,block)` 的 keystream 不同；tx/rx 两个 ctx 不串用。
- HND0 参考数字：ChaCha8 648.6 MB/s、3.39 cycles/byte；1G 单核 19.3%。app 33.72M 时约 4.2 MB/s，CPU 约 0.65%。

---

## 3. carrier 256 ABI

### 3.1 `max_carriers` 与旧 ARQ 的分界

- `max_carriers` 模块参数：默认 **256**，范围 1..256；`tbt_core.c` 的 `module_param` 描述同步改。
- `TBT_MAX_CARRIERS_LIMIT = 256`：载体槽数组、新 FEC、256-bit bitmap。
- `TBT_LEGACY_MAX_CARRIERS = 16`：旧 ARQ 的 `path_ack_mask`/FBK/WINDOW/path_gen/path_end/FIN/RST mask。
- `fec=off` 且 `n+m > 16`：`tbt_cfg_sane()` 返回 `-EINVAL`；`fec=on` 且 `n+m > 256`：`-EINVAL`。
- `struct tbt_session.car[256]`（2 KiB 指针）；`peers` 输出缓冲按 256 行分配。

### 3.2 portblock

- 默认 **4096** 个端口（建议 `30000-34095`）；256 carriers 的最小生产值 **1024**（256 × 4 次重建余量）。
- `ports lo-hi` 仍是全局唯一位图；`portblock=` 每隧道字段继续解析/渲染，但数据面用全局块（现状，不改）。
- 约束：`1 <= lo <= hi <= 65535`；`hi-lo+1 >= 256` 警告，`< 1024` 在生产 profile 中拒绝。
- 端口耗尽计数：`port_exhaust` / `port_alloc_fail`；重建时先释放再重试。

### 3.3 128 MiB 缓冲预算

| 项 | 值 |
|---|---:|
| 每 carrier `SO_SNDBUF`/`SO_RCVBUF` 设置值 | 64 KiB |
| 内核翻倍后每 carrier 实际（snd+rcv） | 256 KiB |
| 256 carriers 合计 | 64 MiB |
| 模块级 `fec_buffer_budget` 默认 | 128 MiB（`134217728`） |
| 固定开销（tcp_sock/HTTP/work item/peers） | 约 1.3 MiB |
| 生产 `max_tunnels` | 1（2 vCPU/955 MiB） |

- `max_tunnels > 1` 时，`fec_buffer_budget` 按 tunnel 数均分；创建时拒绝超限（`-ENOSPC`）。
- 每 carrier 512 KiB 是上沿，不是每流保证；实际按 64 KiB 设置值 + 内核翻倍计算。
- 256 carriers 的 socket 总数 = `max_tunnels × 256`；procfs `stats` 暴露 `fec_rx_mem_kb`、`fec_tx_mem_kb`、`carriers_up`。

### 3.4 session 级 poll 合并

- 稳态：**一个** `sess->w_poll`，每 50 ms 遍历全部 carrier；不再为每条 carrier 排 `w_poll`。
- 每 carrier 保留 `w_rx` / `w_tx`（数据面），但 `w_poll` 只在 connect/retry 阶段使用；连接建立后由 session poll 驱动。
- 效果：256 条 carrier 的 5120 wakeups/s 降到 20/s；单条故障发现延迟上限 50 ms。
- `sk_write_space` 回调只 `schedule_work(&sess->fec_sched_work)`，不直接发送；`tbt_carrier_send(c, buf, len)` 签名不变，FEC 调度器是唯一调用者。
- 256 carriers 下 `sess->pump_lock` 是串行点；B 改成每 carrier 自己的 `w_tx`，共享队列无锁取任务。旧 ARQ 的 `pump_lock` 语义保持不变，直到 `fec=on` 切换。

### 3.5 256-bit bitmap

- `DECLARE_BITMAP(carrier_mask, 256)`（32 B）；`test_bit`/`bitmap_set`/`bitmap_clear`/`bitmap_weight`。
- FEC_FBK 的 `carrier_bitmap[32]`：bit i = carrier i 本 epoch 有丢包；接收端按 carrier 索引置位，发送端按位调 `m`。
- 旧 `path_ack_mask`（u16）只服务旧 ARQ；新 FEC 任何路径都不得把 256 位截成 16 位。
- 边界测试：bit 0、bit 255、`n+m=256` 时 `share_index=255` 合法；`share_index` 比较先转 `u16`。

### 3.6 1 share/carrier 映射

```
carriers = groups * (n + m)          /* 必须整除 */
group    = carrier_index / (n + m)
share    = carrier_index % (n + m)   /* == header.share_index */
```

- 生产：`carriers=256`、`groups=32`、`n+m=8`（如 n=4,m=4）或 `groups=1`、`n+m=256`（single）。
- 一条 carrier 只承载一个 share；不允许 `carriers < n+m` 的降级映射进入 v6（旧 ARQ 仍可）。
- carrier 替换保持索引不变；新 carrier 用同一 `group/share`，否则接收端按 `-EPROTO` 关闭。
- `share_index` 是 u16；`n+m=256` 时 `share_index` 最大 255，比较必须 `(u16)share_index < (u16)n + (u16)m`。

---

## 4. `/proc/net/tbtun/config` 新键

### 4.1 键、默认值、边界

| 键 | 值 | 默认 | 边界 / 规则 |
|---|---|---|---|
| `fec=` | `off` \| `on` | `off` | `on` 启用 v6 数据面；`off` 走旧 ARQ |
| `mode=` | `rlnc` \| `grouped` \| `sc-ldpc` | `rlnc` | 仅 `fec=on` 有效；`grouped` 需 `carriers % (n+m)==0` |
| `n=` | 十进制 | `4` | 1..255；`n+m<=256` |
| `m=` | 十进制 | `1` | `fec=on` 时 1..255；`fec=off` 时可为 0；`n+m<=256` |
| `w=` | 十进制 | `0`（auto） | 0 或 1..1024；`fec=on` 时 `w=0` → `2*n`（rlnc/grouped）；`sc-ldpc` 的 0 → 256 |
| `carriers=` | 十进制 | `0`（auto = n+m） | 1..256；`carriers % (n+m)==0`；`fec=on` 时 `carriers >= n+m` |
| `crypto=` | `off` \| `on` | `fec=on` 时 `on`；`fec=off` 时 `off` | `crypto=on` 且 `fec=off` → `-EINVAL` |
| `crypto_key=` | 64 hex 或 `-` | `-`（未设置） | 64 hex = 256-bit；`-` 清除；空值拒绝 |

补充：

- `w` 与 `k`：v6 的 `k` 由 `n` 决定（`k=n`，每组数据包数）；不新增 `k=` 键。`w=0` 时渲染 effective `2*n`；`carriers=0` 时渲染 effective `n+m`。
- `mode=grouped`：`groups = carriers/(n+m)`；`n/m/w` 是**每组**参数；生产 `groups=32`。
- `mode=rlnc`：`groups=1`；`carriers=n+m`（single 256 时 `n+m=256`）。
- `mode=sc-ldpc`：`n/m` 是每块数据/parity share 数；`w=0` → 256，显式只接受 256/512（v6.0）；L 固定 8，D 若加 L 键必须先改 ABI。
- `fec=on` 且 `m=0`：拒绝（没有 parity 不是 FEC）；`n=1,m=1` 合法（最小对照）。
- `carriers=256` 时 `n+m` 必须是 8 的约数（32×8）或 256（single）；其他组合要求 `carriers % (n+m)==0`。
- `crypto_key=` 的 64 hex 只接受 `[0-9a-fA-F]`；奇数长度/非 hex 拒绝。

### 4.2 规范渲染顺序

`config` 读回仍是「每个标量键显式渲染 effective 值」。新键插在 `n m` 之后：

```
role listen [peer] [target] [peer] n m fec mode w carriers crypto crypto_key
dup profile keepalive drip_ms replay tunnel_rate max_inflate hole_budget_ms
rx_reorder_bytes tx_retain_bytes carrier_stall_ms portblock [nonce] [srcaddrs]
```

- 旧配置行不含新键，解析后按默认值渲染；`cat config > backup; cat backup > config` 仍逐字节回环。
- `crypto_key=-` 是「未设置」的规范拼写；实际 key 设置后渲染 64 hex（`/proc` 文件 0600 root，已是 root-only）。
- `fec=off` 时 `mode` 仍渲染（默认 `rlnc`），但数据面不读；`crypto` 必须 off。

### 4.3 校验顺序

`tbt_cfg_sane()` 在分配/建 session 之前：

1. `fec`/`mode` 值合法；`crypto` 与 `fec` 的组合合法。
2. `n>=1`；`fec=on` 时 `m>=1`；`(u16)n+(u16)m <= (fec?256:16)`。
3. `w==0 || (w>=n && w<=1024)`；`mode=sc-ldpc` 的显式 `w` ∈ {256,512}。
4. `carriers==0` → `n+m`；`carriers<=256`；`carriers % (n+m)==0`；`fec=on` 时 `carriers>=n+m`。
5. `crypto_key` 64 hex 或 `-`。
6. `max_carriers` 模块参数与 `max_tunnels` 预算检查。

任一失败返回 `-EINVAL`，整行不生效。

---

## 5. 分支、文件 ownership 与合并顺序

### 5.1 工作树与职责

| 工 | 分支 | 工作树 | 任务 | 文件/职责 |
|---|---|---|---|---|
| A `wan-overhaul2` | `w-window` | `/home/kunweiz/tbtun-worktrees/window` | T1 N=1 retx/控制面 | `tbt_tx.c`、`tbt_rx.c`、`tbt_carrier.c`（parser/poll/stall）、`tbt_procfs.c` 计数、QEMU A–K/adversarial |
| B `fec-v6` | `w-fec-v6` | `/home/kunweiz/tbtun-worktrees/fec-v6` | T2 FEC v6 + 256 carriers + 中间码 | `tbt_fec.h`、`tbt_fec.c`、`tbt_fec_tx.c`、`tbt_fec_rx.c`、`tbt_uapi.h`、`tbt_parse.*`、`tbt_procfs.c`、`tbt_session.c`、`tbt_carrier.c` 256 缩放、`tbt_tx.c`/`tbt_rx.c` 的 FEC 钩子 |
| C `crypto-v6` | `w-crypto` | `/home/kunweiz/tbtun-worktrees/crypto` | T3 ChaCha8 | `tbt_crypto.h/.c`、`host/test_crypto.c`、`tbt_fec.c` 的 body hook、HELLO key、`tbt_parse.*` 的 crypto 键、`tbt_procfs.c` |
| D `fec-core2` | `w-sclc`（当前 worktree `w-fec`，合并前统一命名） | `/home/kunweiz/tbtun-worktrees/fec` | T4 SC-LDPC | `review/fec-prototype/sc_ldpc/*`、`tbt_sclDPC.c`/`tbt_fec_ops` 实现；不先写内核 |
| E `wan-deploy` | `w-hnd0` | `/home/kunweiz/tbtun-worktrees/hnd0` | T5/T6 HND0 | 部署/回滚脚本、HND0 证据、`review/hnd0-v6/`；产品码只在必要时改 |
| F `goal-integrator` | `master` | `/home/kunweiz/Desktop/vibe/tcp-brutal` | T0/T7 | 本文档、合并顺序、冲突裁决、最终门禁 |

### 5.2 冲突文件与合并顺序

**合并列车（用户裁决）：`w-window` → `w-fec-v6` → `w-crypto` → `w-sclc` → `w-hnd0` → master（F/T7）。**
每一步只在 master 上合并前一等的 head；不允许跨序合并。D 的当前 worktree 仍是 `w-fec`，合并前改名为 `w-sclc` 或由 F 记录等价映射；不得因此改变 D 的提交内容。
**`w-window` 未过 G1（HND0 N=1 auto 根因已定位，待重跑），暂不进入合并列车**；A 的 `rx_reorder_bytes=1MiB` 重跑通过后再评估。

| 文件 | 冲突方 | 裁决 |
|---|---|---|
| `tbt_tx.c` | A（T1 修复） vs B（FEC packetizer/scheduler） | **A 先合**。B rebase 到 A 的 head；A 的旧 ARQ 行保留，B 只在 `fec=on` 分支替换数据面。冲突时 A 的旧路径行为优先，B 的 FEC 钩子行次之；integrator 裁决。 |
| `tbt_rx.c` | A（hole/retx） vs B（FEC rx） | 同上：A 先合；B 加 `tbt_fec_rx.c`，旧 `tbt_rx.c` 保留；`conv` 分流。 |
| `tbt_carrier.c` | A（parser/poll/stall） vs B（256 缩放/poll 合并） | A 先合；B 在其上做 256 缩放。B 必须保留 A 的 parser 自旋修复、raw 空闲 stall 重置、buffer 前置设置。 |
| `tbt_uapi.h` | B（v6 常量/类型） vs A（计数） | B 拥有 v6 常量；A 不新增 wire 常量。`TBT_MAX_CARRIERS_LIMIT=256` 与 `TBT_LEGACY_MAX_CARRIERS=16` 由 B 一次改完。 |
| `tbt_parse.c/h` | B（FEC 键） vs C（crypto 键） | B 先合；C rebase 后加 `crypto=`/`crypto_key=`。`struct tbt_cfg` 字段由 B 加 FEC 部分，C 加 crypto 部分。 |
| `tbt_procfs.c` | B vs C | B 先合 FEC 计数/配置；C 加 crypto 状态。 |
| `tbt_session.c` | B（car[]/预算） vs C（key 生成） | B 先合；C 在 session 创建处生成 key。 |
| `tbt_core.c` | B（max_carriers 默认） | B 独占。 |
| `Makefile` | B/C/D | B 加 `tbt_fec.o`；C 加 `tbt_crypto.o`；D 加 `tbt_sclDPC.o`；各自 rebase，SRCS/obj-m 同步。 |
| `DESIGN.md` | 所有人 | 各分支只加自己章节；F 在 T7 统一对齐，避免同段冲突。 |

规则：

- 每个工只提交自己的分支；**不合并、不 push**。F 负责 master 合并。
- 允许在自己分支 `git rebase`；禁止改写 master/他人分支。
- A 的 T1 必须先合，因为 B 的 256 缩放会碰同一批 carrier/tx/rx 行；A 未合前 B 只做 `tbt_fec.*`/parser/procfs，不动 `tbt_tx.c`/`tbt_rx.c`。
- B 的 v6 host 工作不被 A 阻塞：B 可以先写 `tbt_fec.c/h` + host 测试；数据面钩子等 A 合入后再接。
- C 依赖 B 的 `tbt_fec.c` body hook 与 HELLO body；B 的 ABI 冻结后 C 即可并行。
- D 依赖 B 的 `tbt_fec_ops`；D 的 host 原型与 B 的内核工作并行，不互相阻塞。

---

## 6. 门禁 G0–G7

所有门禁都要求：命令退出码 0、证据落盘、失败不静默。证据目录以主仓库 `review/` 为准；worktree 的证据在合并时带过来。

### G0 — T0 接口冻结（F）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`git status --short`；`git diff --stat` |
| 证据 | `review/goal-v6-interfaces.md`；`make check` 日志；`git show --stat HEAD` |
| 通过 | `make check` 0；`tbtun/` 无改动（docs only）；B 书面确认 40B/控制帧；本文档 commit 到 master |
| 失败 | 任何 `tbtun/` diff、接口未确认、`make check` 非 0 |

### G1 — T1 N=1 retx/控制面（A）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 480`；`bash adversarial/run.sh`；N=1 慢载体脚本 |
| 证据 | `review/final-qemu/`、`review/final-bench/`、`adversarial/results.txt`、`review/wan-experiment/t1-n1-auto/`、N=1 原始日志 |
| 通过 | N=1 慢载体 `rc=0`；far `holes_unrecoverable=0`；`fbk_rx>0`；`carrier_lost=0`；`raw lost=0`；**byte-exact = sink 读满 16MiB + EOF**（iperf3 只作吞吐/信息）；lab `W≈136–250 KB`，HND0 按 `W≈k×BDP`（203ms×17M→~862KB，1MiB cap 下 ~750KB）判，不用 lab 绝对值；A–K 11/11、0 skip；adversarial 91/91、0 failed/0 skipped；host/SAN/lint 全绿 |
| 失败 | 任一计数不符、skip、crash、hang |

**G1 现状（挂起，HND0 N=1 auto 根因已定位）**：A `w-window 3f4f1f0`（模块同 c672843）定位：① W 被 `rx_reorder_bytes=262144` 钳住，不是公式；near `bdp_bytes=375286`、`k=2` → 750572，实测 10.3 Mbit/s = 262144/192.7ms；far `W=121–125KB` 是反方向控制流。重跑：两端 `rx_reorder_bytes=1048576`、`tx_window_bytes=0`、`hole_budget_ms=0`、`carrier_stall_ms=15000`、`profile=raw`、`debug=2`，`W=min(750KB,1MiB)=750KB`，17 Mbit/s 可行。② near `holes_unrecoverable=1` 是 sender-side `tbt_tx_stall_check` 杀掉被 reaped 的 iperf3 控制流（`rx_max_stall_us=0`；数据流 `kill=5` 是 app close），不是 rx hole；控制流 `tx_ack` 为何停 >1s 待 debug 打印。RST 用例已修（near 15/15；测试 bug），lint 16/16+14/14，adversarial 全套仍在跑。T1 未绿，**不合并 `w-window`**。

**HND0 验收口径**：`W≈k×BDP`（203ms×17M→~862KB；1MiB cap 下 ~750KB），不用 lab 的 136–250KB 绝对值；通过条件仍是 `rc=0`、far `holes=0`、`fbk_rx>0`、`carrier_lost=0`、`raw lost=0`。

**重跑状态（1MiB，E `cb5771e`）**：rc=1；bulk 16MiB/3s（44.7 Mbit/s sender）、far `rx_ack=16777253`、`holes=0`、`lost=0`；`W=1MiB` cap、`bdp=1.17–1.26MB`、`k=2`；但 near `TXSTALLKILL sid=…760 ack=171 txseq=172 age=3.17s`，far 控制流 `tgt_state=7`、2.65MB 未交付；near c6 `gate_fail=314/depth_fail=67/time_fail=35`。**W 修复对 bulk 生效，剩余阻塞是控制流 stall**；A/`hnd0-n1b` 诊断中。T1 未绿，`w-window` 不合并。

**Adversarial 状态**：A 报告 91/91 cases pass；full suite 90/1，仅 `pub.writer_drain_1g` 在 host load 3.3–4.7 下超时；isolated writer 4/4 绿；A 正在安静主机重跑全套。RST 是 fake far parser 的 test bug（已修 3f4f1f0），lint 16/16+14/14。

**T1 后续**：`hnd0-n1b` 已 stand down，不重复 1MiB 跑；A 在 full adversarial 后诊断控制流 stall。T1 未绿，`w-window` 不合并。

**控制流 stall 机制（`hnd0-n1b` 诊断，A 修复中）**：`tbt_tx.c:tbt_tx_stall_check()` 的 `budget = hole_budget_ms ? hole_budget_ms : 1000ms`，再取 `max(budget, 2.5×RTO)`；本次 `hole_budget_ms=0` → 1s（RTO=200ms，floor=0.5s），`carrier_stall_ms=15000` 不参与。iperf3 控制流第 172 字节在 far 目标侧 3s 未被读走，`tx_ack` 停在 171，3.17s 后被杀；kill 发 RST → HND0 iperf3 server 关闭控制 socket、停止读数据 socket → far 数据流残留 `rxq=2.65MB`、`tgt_state=7`，target-close 未传回 near。A 修复方向：auto 用 `carrier_stall_ms`/5×RTO；target CLOSE 主动拆流/回传 close；修后重跑 HND0 N=1 auto。证据缺口：`t1-n1-auto/` 缺 `/proc/net/tbtun/config` dump；下次复跑补 config/ports 快照。

**控制流 stall 根因（A，`cb5771e`，覆盖上段的“修复方向”）**：不是 FBK/远端口 hole，是 per-carrier peer-window gate 的跨流饥饿。near 控制流 172B，far `uack=171`；kill 时 carrier window 满（`retx wndshort inflight=785496 wnd=785248`），pump gate `(queued-una) >= snd_wnd` 是 per-carrier FIFO；bulk 16.9MB backlog/1MiB window 每次 window 开启先吃满，控制流 1B frame 停在 session `tx_q` 尾部；`sends=0` 使 retx 跳过、无 HOLE copy；`tx_stall_check` 在 `max(1s,2.5×RTO)=1s` 触发 `TXSTALLKILL ack=171 txseq=172 age=3.17s`。后果：near 控制 RST 关 far iperf3 control target → iperf3 abort → far data `tgt_state=7`/2.65MB 未交付 → far 停 ack bulk → near data 也 stall（`kill=4`）。far rx hole 不是触发（`rx_max_stall_us=388ms<1s`、far `holes=0`）；bulk repair 正常（`retx_frames=26`、`hole=2`、far `rx_ack=16.7MB`）。修复：`tbt_tx_pump` 的 peer-window gate 对 `tx_seq-tx_ack <= 2*TBT_PAYLOAD_MAX` 的小 backlog DATA frame 豁免，保持流内顺序；更严格可两遍选择。A 未打补丁，先本地回归（bulk+control 同 carrier）再 HND0 auto 重跑；head 仍 `3f4f1f0`。

**代码路径确认（`hnd0-n1b`）**：`tbt_tx_pump()` 的 carrier admission 对 `TBT_F_DATA` 走 `sk_stream_memory_free(c->sk)`（只有 FBK 豁免）；控制流 1B DATA frame 与 bulk 共用同一 peer-window/sndbuf 门；`tbt_tx_window_ok()` 本身通过（`172 <= 171 + 1MiB`），饥饿点在 carrier send-memory 门，不在 tunnel 窗口公式。A 的小 backlog DATA 豁免正对此段。重跑证据放 `review/wan-experiment/t1-n1-auto-rx1m/`（不覆盖 E 的 `t1-n1-auto/`），补 `config`/`ports` 快照和两条 stream 的 per-stream 行。

**A 补丁（`w-window f8023a2`）**：① cross-stream 小 backlog DATA 豁免（`tx_seq-tx_ack <= 2*TBT_PAYLOAD_MAX`，peer-window 与 send-memory 两门）；② `hole_budget_ms=0` 的 sender stall budget 改为 `max(5s, 5×RTO, carrier_stall_ms)`（显式 `hole_budget_ms` 保持原义）；③ target close（EPIPE/ECONNRESET/ESHUTDOWN）主动 abort/传播，`-ENOTCONN` 仍 transient。本地 build/make check/near 15/15/A–K 11/11/lint 绿；完整 adversarial 待 HND0 rerun 后安静跑。**module sha 待确认**：worktree `tbtun.ko` sha256=`686797c5…`（07:22，早于 f8023a2 的 07:26），与用户报的 `3edf84e0` 不一致；`hnd0-n1b` 必须用 A 确认的 f8023a2 构建，不能用 stale ko。

**重建/模块确认（A + `hnd0-n1b`）**：`f8023a2` 干净重建确定性 sha256=`686797c5…`，路径 `/home/kunweiz/tbtun-worktrees/window/tbtun.ko`，vermagic `7.2.9-1-MANJARO SMP preempt mod_unload`，version 0.0.1；`hnd0-n1b` 从 `git archive f8023a2` 另路径重建得 `d5fcce40…`，`srcversion=475868A5` 与 686797c5 相同，证明源码同一。`3edf84e0` 是 A 的中间 cut（仍会在 `-ENOTCONN` 杀 delivery），**不用于 rerun**。**模块校验口径**：near 7.2.9 用 `686797c5`（或同源重建）；far HND0 7.0.0-42 用**同一份 f8023a2 源码**在 HND0 构建，记录 far ko sha256；两端 `srcversion` 必须一致（源码同一性），ko sha 因 vermagic/构建路径不同而不同。provenance：near 15/15 与两次 A–K 都在 `3edf84e0` 上；最终 `686797c5` 的 A–K 尚未跑，等 HND0 rx1m 后安静跑。

**用户裁决（模块 invariant）**：ko sha 依赖 build-path/kernel；`3edf84e0` 不是 f8023a2 的确定性 sha。Invariant = **same source `f8023a2` + per-end build + `srcversion=475868A5` + per-end sha 记录**；`hnd0-n1b` 按此跑 N=1 auto。`window-verify` 负责本机 A–K/完整 adversarial。

**f8023a2 rx1m 结果（`hnd0-n1b a2a37ed`）**：控制流 starvation 修复生效——`rc=0`、无 `TXSTALLKILL`、far `fbk_rx=1089`、`holes=0`、`carrier_lost=0`、`lost=0`；near `tx_window=1MiB` cap、`bdp=1321474`、`k=200`；far `tx_window=210880`、`bdp=99902`；CPU near 9.66%、far 21.71%。**但 16MiB 未逐字节**：sender 16,777,216 B，receiver 13,500,416 B（80.47%）；near `submit_bytes=15,996,575`（780,641 B 留在 public socket 接收队列未 capture）、`tx=14,007,990`、`wire=14,118,309`、流结束 `txq=1,113,317`；far `rx_delivered=13,614,558`；数据流 sid …905 close 时 `state=5 kill=5`、`rxq=24576`。机制：public socket close 直接 kill stream，未先排空 capture/txq，FIN/RST 越过队列数据。**close-drain 是新的/遗留阻塞，G1 不能判绿**；A 诊断修复。

**close-drain 修复归属**：已开 `window-close` 修 public close 的 drain 顺序（先排空 capture/txq，再 FIN/RST）；T1 未绿，不合并。

**close-drain 修复（`w-window ddef988`，srcversion `62E75E1C`）**：FIN 只在 capture 全部 submit 且 `tx_ack` 覆盖 `tx_seq` 后发出；`close_since_ns` + `tbt_tx_close_check()` 对无进展的 FIN 等待有界 reset；FIN 已写出但对端 FIN 缺失 → `max(60s, 2×budget)` 后 detach（不 reset）；对端 FIN 已到但 delivery 队列不缩 → `tbt_rx_deliver_stall_check()` 有界 reset；FIN order gate 改为 `state != DEAD`。回归：adversarial `closedrain` PASS、`closedrain_timeout`；make check/lint PASS；A–K/near/full adversarial/SAN 由 `window-verify` 补跑。T1 等 sink HND0 重跑。

**验收口径修正（2026-11-06）**：iperf3 server 在 `TEST_END` 主动 close 数据 socket，receiver 13,500,416 B **不是 tunnel 丢数据**；T1 的 byte-exact 改用 **sink 读满 16MiB + EOF**，iperf3 只作吞吐/信息。close-drain 仍是真 bug（near FIN 早于 capture/txq 排空），`window-close`/A3 继续修；T1 仍未绿。

**window-verify 重建（ddef988）**：clean rebuild HEAD `ddef988f…`，ko `/home/kunweiz/tbtun-worktrees/window/tbtun.ko` sha256 `c476bd48…`，size 6,071,368，`srcversion=62E75E1CE05E5C4887C9AAA`，vermagic `7.2.9-1-MANJARO`。A–K 运行中（`bg_670`）；随后 near 15/15、full adversarial（期望 91/91）、`host-test-san`；`writer_1g` 超时隔离复跑并分别报告。不合并/push，不碰 HND0。

**T1 sink 门禁 PASS（`hnd0-n1b b3b33b2`）**：`SINK_BULK bytes=16777216 eof=1`；sender `SENDCLOSE_OK bytes=16777216 txq_after=1113052`（close 时队列非空）；near `fin_seq=16777216 fin_w=1 rst_w=0 rxf=1/1`、live `txq=0`；far `rx_delivered=16777216`、`deliver_timeouts=0`、`close_timeouts=0`、`close_detaches=0`、live `txq=0`。FIN 时序：near `fin_q=1 fin_w=0 txq=1036242` → drain 到 `txq=0` → `fin_w=1` → `rxf=1/1` detach；全程无 RST。两端同源 ddef988、`srcversion=62E75E1C`；CPU near 10.57%、far 27.93%。iperf3 `rc=0`，receiver 11,796,480 B 是 TEST_END close，不作 byte-exact。**T1 sink 门禁 PASS；G1 仍等 `window-verify` 的 A–K/near/full adversarial/SAN 才能判绿。** `hnd0-sink` 不要重复跑/覆盖该证据目录。

**残余 accept wake race（`hnd0-sink` 独立复跑，ddef988）**：near HELLO 55B 写出、far 内核已收，但 far `rx_work_runs` 33s 不跑、sink 从未 accept；near `TXSTALLKILL ack=0 txseq=1114059 age=15.0s`、`fbk_rx=0`、`tx_ack=0`、`lost=1 replaced=1`；far `rx_read_bytes=55`、`c3 up=0 lost=2`、`fbk_tx=0`。同源 ddef988 的 hnd0-n1b 成功是**非必现**；符合 accept 侧 `sk_data_ready`/HELLO wake 竞态。A3 修 accept 侧唤醒；T1 需 **N=1 连续 5 次 5/5 sink 16MiB** 才算绿。

**5x 计划（`hnd0-n1b`）**：同一 A3 源码 per-end build、两端 srcversion 一致；5 轮独立 sink/sender；每轮 sink `bytes=16777216 eof=1`、far `rx_delivered=16777216`、near `fin_seq=16777216 fin_w=1`、`rst_w=0`、`deliver/close timeouts=0`、`close_detaches=0`、near/far `live txq=0`、无 `TXSTALLKILL`；证据 `review/wan-experiment/t1-n1-auto-rx1m-sink-5x/`，保留 b3b33b2 单次证据。失败也记录并继续，不掩盖。

**sink 测试就绪（`hnd0-n1b`）**：`sink.py`（accept 一次、读到 EOF、记录 bytes/expected/ok/secs）与 `send16m.py`（写满 16MiB、`shutdown(SHUT_WR)`、等 EOF）本地 loopback 验证 `16,777,216 B ok=1`；HND0 `/tmp/tbtun-sink/sink.py` `py_compile` 通过。下一轮证据目录 `review/wan-experiment/t1-n1-auto-rx1m-sink/`，保留 `a2a37ed` 的 iperf3 失败证据；等 `window-close`/A3 新 head。

### G2 — T2 FEC v6 + 256 carriers + 中间码（B）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`make host-test`（含 `host/test_fec`）；`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 480`；256-carrier host 资源测试 |
| 证据 | `review/fec-v6/`、`host/test_fec` 输出、QEMU 日志、`/proc/net/tbtun/peers` 256 行快照 |
| 通过 | `conv=0x1001`；40B 偏移/CRC 与本文一致；`share_index/epoch/win_len` u16；256-bit bitmap；`n+m=256` 不截断；1 share/carrier；RLNC/grouped host 向量 p=1–50%；控制帧 partial 测试通过；A–K 11/11；adversarial 91/91 |
| 失败 | 任何字段偏移不符、16 槽泄漏、`n+m=256` 截断、QEMU 回归 |

**G2 现状（T2.1–T2.4 完成，T2 parent 进行）**：B `w-fec-v6 a767b63` 落地 40B header/ABI、`tbt_fec`、256 carrier slots/portmap（4096；host test 30000–34095）、grouped RLNC host codec；`test_fec 1430/0`、`test_portmap 33999/0`、`parse 412/0`、host/SAN/lint、QEMU A–K 11/11。`fec=on` 仍是 parser/procfs，`tbt_session_create()` 返回 `-EOPNOTSUPP`；数据面等 A/G1 后接。T2.1–T2.4 完成，T2 parent 进行；不合并。

### G3 — T3 ChaCha8（C）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`./host/test_crypto`；HND0 `review/microbench/run_chacha_ref.sh --bytes 4 --reps 15 --mhz 2200`（或模块内 benchmark） |
| 证据 | `review/crypto/`、`host/test_crypto` 输出、HND0 日志 |
| 通过 | TC1 + 256-bit KAT 逐字节；body-only（HTTP 头/FEC 头明文）；默认开、`crypto=off` 可关；HND0 MB/s/cycles/byte 记录；1G 单核占用 ≤25% |
| 失败 | KAT 不符、头被加密、无法关闭、CPU 超预算 |

**T3 现状（G3 通过，待 B 后 rebase）**：C2 `w-crypto 33238ca` 加 `tbt_crypto_pair {tx,rx}`、per-direction nonce、等 nonce 拒绝、HELLO 双向；KAT 55/55，`make check`/`host-test`/`host-test-san` 绿；HND0 ChaCha8 8KiB max **635.0 MB/s / 3.46 c/B / 19.7% 单核**（median 24.7%），body-only/per-direction/default/可关。T3 标完成；合并前等 B 后 rebase；不合并。

### G4 — T4 SC-LDPC BEC 容量门（D）

| 项 | 内容 |
|---|---|
| 命令 | `cc -O2 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm -o sc_ldpc_bench`；`./sc_ldpc_bench --model symmetric --trials 500`，点 `(block,L,rate,p)=(256,8,0.70,0.30),(256,8,0.60,0.40),(512,16,0.50,0.50)`（或 D 证明的等价点） |
| 证据 | `review/sc-ldpc/`、`review/microbench/logs/sc-ldpc-*`、原始 CSV |
| 通过 | p=30/40/50 对应 rate ≤0.70/0.60/0.50；恢复 ≥99.9%（500 trials 的 0 失败或置信区间下界达标）；codec ≥125 MB/s（encode/decode，HND0 单/双核）；块延迟/内存记录 |
| 失败 | recovery 0、systematic-only 作弊、<125 MB/s、无置信区间 |

**G4 现状（`sclc2`，HND0 无可行点，2026-11-06）**：HND0（QEMU 2 vCPU，taskset 0）实测 decode **0.32–1.99 MB/s**，全部低于 app 4.215 MB/s（最好 47%）；2 线程最好 1.96；encode **2.65–9.54**，低于 wire 需求 8.4–14.1。恢复率不是障碍：p30 rate0.50、p40 rate0.40 为 1.0000；p50 rate0.25 block256/L16 为 0.9995、L32 为 1.0000，但 decode 0.43–0.46。本地 Ryzen 是 HND0 的 6–12×，不能当验收；extra_dc 6/8 与 5 相同、3/4 更差，已无旋钮。证据：`review/sc-ldpc-hnd0.md`/`.csv`（head `ef5f956`）、`review/sc-ldpc-sim.md`/`.csv`；`sclc2` 已停手，`w-fec` clean。**HND0 上 SC-LDPC 无可行点，G4 不修订，T6 阻塞；等用户 B（RLNC/grouped 降级）/C（SC-LDPC 硬门阻塞）裁决；不合并。**

### G5 — T5 HND0 256-flow 中间 FEC + ChaCha8（E）

| 项 | 内容 |
|---|---|
| 命令 | 部署脚本 `dry-run`；维护窗口内 `iperf3` 单流 256 carriers；`cat /proc/net/tbtun/{tunnels,peers,stats}`；`mpstat -P ALL`、`/proc/softirqs`、`ss -ti`、`free -m` |
| 证据 | `review/hnd0-v6/`（原始日志、CSV、procfs 快照、服务恢复检查） |
| 通过 | 256/256 carriers；app ≥33.72 Mbit/s；记录 p、near/far CPU/RAM、wire/app、holes、raw lost；frps/Caddy/hysteria/xray 全部 active；无持久改动 |
| 失败 | <33.72M、carrier 掉线、服务未恢复、OOM、无回滚 |

**T5.1 已接受 `c435ba9`**：全局 `/proc/net/tbtun/ports` 写入/verify、v6 行去 legacy 键、端口保留告警反转、dry-run 提交；T5.2 等 G5。

### G6 — T6 SC-LDPC 最终 + HND0 256-flow（D+E）

| 项 | 内容 |
|---|---|
| 命令 | G4 的 SC-LDPC 门 + G5 的 HND0 测试，但 `mode=sc-ldpc`，无中间 FEC |
| 证据 | `review/hnd0-v6/sc-ldpc/`、`review/sc-ldpc/` |
| 通过 | G4 全绿；G5 全绿；app ≥33.72M；无 RLNC/grouped 兜底；服务恢复；最终门 |
| 失败 | 任何中间码回退、G4 未过、HND0 不达标 |

**T6 状态：阻塞。** HND0 SC-LDPC decode 0.32–1.99 MB/s < app 4.215、encode 2.65–9.54 < wire 8.4–14.1；G4 不修订，等用户 B/C 裁决，不合并 SC-LDPC。

### G7 — T7 文档/0.0.2/合并 push（F）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；A–K 11/11；`bash adversarial/run.sh`；`grep TBT_VERSION tbtun/tbt_uapi.h`；`git merge --no-ff` 各分支；`git push` |
| 证据 | `review/final/`、`git log --oneline`、`git status`、release 说明 |
| 通过 | `0.0.2`；A–K 11/11；adversarial 91/91；host/SAN/lint；master 干净；push 成功；文档与 ABI 一致 |
| 失败 | 版本不符、回归、脏树、push 失败 |

---

## 7. 机器锁

三台机器的串行化用 `flock`，锁文件固定：

| 机器 | 锁文件 | 用途 |
|---|---|---|
| QEMU（本机） | `/tmp/tbtun-qemu.lock` | 所有 `tests/qemu/run.sh`、`review/dkms-qemu/run.sh`、adversarial QEMU 组 |
| HND0 `45.11.1.62` | `/tmp/tbtun-hnd0.lock` | 维护窗口、部署、iperf3、procfs 采集 |
| RK3588 `.65` | `/tmp/tbtun-rk3588.lock` | 所有 `.65` 构建/bench；跑前 `uptime`，load >4 先避让 |

协议：

```sh
lock=/tmp/tbtun-qemu.lock          # 或 hnd0 / rk3588
exec 9>"$lock"
flock -w 1800 9 || { echo "lock busy: $lock" >&2; exit 1; }
echo "$$ $(date -Is) $(whoami) <purpose>" >&9
trap 'flock -u 9' EXIT
# ... run ...
```

规则：

- 没有锁**不得**跑 QEMU/HND0/RK3588；`flock` 释放靠进程退出/close，锁文件不删除。
- 不杀持锁进程；等锁超时后报告 Main，不绕过。
- HND0 锁同时代表维护窗口；拿锁前先在任务/消息里声明目的与预计时长，拿锁后立即记录 holder 行。
- `.65` 跑前检查 load 与其他 worker；load >4 或检测到他人进程，跳过并记录，不抢跑。
- QEMU 锁覆盖 `make check` 里的 QEMU 部分；纯 host 测试不需要锁。

---

## 8. 风险

| # | 风险 | 证据/量级 | 处理 |
|---|---|---|---|
| R1 | **SC-LDPC 最终门：HND0 无可行点** | HND0 decode 0.32–1.99 MB/s < app 4.215；2 线程 1.96；encode 2.65–9.54 < wire 8.4–14.1；恢复率可 0.9995–1.0（p50 rate0.25），但 CPU 不过；本地 Ryzen 6–12× 不作验收；extra_dc 无改善 | G4 不修订；T6 阻塞；用户 B/C 裁决；不合并 SC-LDPC；不静默降级 |
| R2 | **app ≥33.72M 依赖 256-flow 聚合** | 33.72M 是直连 P8 单流；FEC 后 app = 聚合 × 码率；若路径本身只有 ~34M，FEC 开销可能达不到 | 先测 256-flow 聚合 > 33.72/码率；记录 p；不静默改目标；达不到按 blocker 规则停问 |
| R3 | **HND0 2 vCPU/955 MiB** | TCP/HTTP 22–31% 单核；256 flows 的 softirq/slab/端口；FEC grouped 0.75–1.5 GB/s（5–12× headroom） | session poll 合并；每 carrier `w_tx`；RPS/RFS 不可用（只有 2 vCPU），靠预算与实测；`max_tunnels=1`；64 MiB socket 缓冲 |
| R4 | **v6 回归** | 旧 conv 0x0005/16 槽仍在；`TBT_MAX_CARRIERS_LIMIT` 改 256 容易把旧 mask 截断 | `TBT_LEGACY_MAX_CARRIERS=16` 分离；`fec=off` 默认；A–K/adversarial 每步回归；`n+m>16` 旧路径拒绝 |
| R5 | **ChaCha8 CPU** | HND0 648.6 MB/s、3.39 c/B、1G 19.3%；app 33.72M 约 0.65% | 默认 on 但只在 `fec=on`；可关；body-only 不加包头/HTTP 头成本；KAT 与实测门 |
| R6 | **控制帧被 partial DATA 堵死** | 实测 near `fbk_tx=253`、far `fbk_rx=0` 最终 EOF | §1.6 三条规则；host 测试覆盖全 carrier partial；`ctl_blocked_by_partial` 计数 |
| R7 | **256 条 carrier 的资源/端口** | 64–128 MiB 缓冲；portblock 4096；TIME_WAIT；`max_tunnels×256` socket | 预算上限、portblock 4096、创建时拒绝超限；HND0 实测 RSS/slab/端口 |
| R8 | **p>20% 的 single sparse RLNC 不可行** | p=30–50% 的 XOR 32–39 GB/s，超 HND0 7.8–9.1 GB/s | 中间码用 grouped；最终必须 SC-LDPC；不把 single 写成高 p 生产 |
| R9 | **key 明文/无认证** | HELLO 明文下发 key；CRC 不是 MAC | 文档写明只混淆；需要安全用 WireGuard；不做强安全承诺 |
| R10 | **多流** | v6 一隧道一 stream；第二 stream 无 FEC 状态 | 第二 stream RST/拒绝并计数；多流留 v7；不静默塞 `block_id` |

---

## 9. 接口裁决与跟踪

### 9.1 已裁决：ChaCha8 key 载体（原 Q1）

**维持 B 的 HELLO v6 body 方案。** FEC_CFG 保持 32B，不新增 `0x36`；goal 文本里的「FEC_CFG 明文下发」按「HELLO v6 body 明文下发」记录为**等价偏差**（都是每会话明文 key、只混淆不认证）。B/C 不改 ABI。

### 9.2 跟踪项（负责人 / 截止门 / 验收）

| # | 项 | 负责人 | 截止门 | 验收 |
|---:|---|---|---|---|
| 1 | grouped 条带化与 `block_id` 映射：T0 冻结 `group=carrier/(n+m)`、`group=seq%G`、`block_id` 为组内 generation | B 确认 | G2 前 | B 在 `tbt_fec.c`/host 测试中按此实现；若改连续块，只改映射、不改 wire 字段，并书面回复 F |
| 2 | 旧 16 槽拆分：`path_ack_mask`/FBK/WINDOW/path_gen/path_end 改用 `TBT_LEGACY_MAX_CARRIERS=16`；`TBT_MAX_CARRIERS_LIMIT=256` 只给载体槽/新 FEC | B 实现；A 合入时检查；F 裁决 | G2 / A 合并时 | `n+m=256` 不截断；`fec=off` 的 `n+m>16` 被拒；旧 wire 常量不变 |
| 3 | SC-LDPC `w`/L 配置面：T0 冻结 `w=0→256`、显式 256/512、L=8；不新增键 | D 与 B/F 对齐 | G4 前 | D 的 `tbt_fec_ops` 实现接受该配置；若要 `L=`/`block=` 新键，先书面改 ABI |
| 4 | 末包 `payload_len < S`：T0 冻结 `flags.LAST` + 实际 `payload_len`；RLNC 内部按 S 零填充 | B 定稿 | G2 前 | B 的 host 测试覆盖末包 1..S；若改 FIN body 方案，先改控制帧 ABI |
| 5 | `w=`/`carriers=` auto 渲染：0 解析为 auto，读回渲染 effective 值 | B 更新 golden | G2 前 | `host/test_parse.c` 的 golden 行同步；`cat config > backup; cat backup > config` 仍逐字节回环 |
| 6 | 合并列车：`w-window` → `w-fec-v6` → `w-crypto` → `w-sclc` → `w-hnd0` → master | F | 全程 | 不合并直到门禁；D 的 `w-fec` worktree 在合并前映射到 `w-sclc` |
| 7 | `fec=on` 时 legacy 键（`dup`/`replay`/`max_inflate`/`hole_budget_ms`/`rx_reorder_bytes`/`tx_retain_bytes`）的行为 | B 定 | G2 前 | 建议「解析/渲染但数据面不读」；`dup` 绝不能在 FEC 路径复制数据；host 测试 `fec=on`+`dup=1` 无重复发送 |
| 8 | 全局端口块：部署脚本写 `/proc/net/tbtun/ports`（`ports 30000-34095`）；B 补 4096 端口分配 host 测试 | E 已修 `b9adb43`；B 补测试；F 检查 | G2 / G5 | `verify` 比对全局块；`tbt_portmap_set` 4096 端口无截断；256 次 alloc/free；256 carriers 不在第 17 条起 bind 失败 |

---

## 附录 A：40B header 速查

```
+0  type        +1  flags       +2  share_index u16
+4  n           +5  m           +6  epoch u16
+8  block_id u64
+16 seq u64
+24 win_len u16 +26 payload_len u16
+28 conv u16=0x1001             +30 reserved u16=0
+32 payload_crc32c u32          +36 header_crc32c u32
```

## 附录 B：控制帧速查

```
ACK 16:   next_deliver u64 | credit u32 | rtt_us u32
NACK 12:  missing_seq u64 | count u16 | reserved u16
CFG 32:   epoch u32 | n u16 | m u16 | w u16 | k u16 | d u8 | rsv[3]
          | seed u64 | crc32c u32 | rsv u32
FBK 48:   epoch u16 | m_cur u8 | m_tgt u8 | p_ppm u32
          | expected u16 | received u16 | rsv u32 | bitmap[32]
```

## 附录 C：冻结的证据路径

- `review/fec-design.md`（w-fec）：流式滑窗 RLNC 设计、`d=W` 退化修正、HND0 profile。
- `review/chacha8-reference.md`（w-fec-survey）：ChaCha8 参考实现、KAT、内核边界。
- `review/microbench.md`（w-microbench）：HND0/RK3588/Athlon 的 XOR/RLNC 实测。
- `review/sc-ldpc-microbench.md`（w-microbench）：SC-LDPC 原型当前状态（symmetric 未过）。
- `review/crypto-microbench.md`（w-microbench）：ChaCha8 648.6 MB/s、3.39 c/B。
- `review/hnd0-sim.md`（w-fec）：grouped 32×8 恢复率、`d=W/2` 修正。
- `review/final-checklist.md`：A–K/adversarial 的命令与证据格式。
