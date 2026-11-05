# ChaCha8 核心：移植、KAT 与吞吐

状态：实现完成。范围只限 **payload body 的流加密核心**：不做认证、不做头部保护、
不做密钥与 nonce 管理。基线 `w-fec c84c8dc`；语义参考
`review/chacha8-reference.md`（commit `040304a`）。

证据标记：`[实测]` 本机编译/运行；`[源码]` 直接读到的代码；`[推算]` 由算术或比例推出；
`[待测]` 需要上机（HND0 / RK3588）。

## 0. 结论

| 问题 | 结论 |
|---|---|
| 实现 | C99，`chacha8.c` 257 行；无分配、无 I/O；8/12/20 rounds，128/256-bit key，**64-bit counter + 8-byte nonce** |
| KAT | 7 组向量逐字节一致；由 Bernstein ref、tiny-chacha、Linux generic 三实现交叉验证 `[实测]` |
| 吞吐 | ChaCha8 **1130–1173 MB/s**（1/8/64 KiB，热缓存）；ChaCha20 622–634 MB/s `[实测]` |
| 对照 | 与 Ryzen 参考 1060–1125（ChaCha8）/ 663（ChaCha20）同量级；本机 AES-128-CBC（AES-NI）1880 MB/s `[实测]` |
| 1G | 125 MB/s 只占 ChaCha8 单核约 **11%**，约 9 倍余量 `[推算]` |
| 计数器 | 32-bit counter 在 1G 下 256 GiB ≈ **37 min** 回绕；本实现用 64-bit counter，取消该悬崖 `[推算]` |
| 判定 | 1G 的 body 加密不是瓶颈；ChaCha8 可以继续用，剩余工作只在认证与密钥管理 |

## 1. 来源与许可

| 来源 | 版本/commit | 许可证 | 用到的部分 |
|---|---|---|---|
| `0xNullll/tiny-chacha` | `890a5f0` | **MIT** | quarter-round、double-round 置换、流式 init/block/xor 结构 |
| Bernstein eSTREAM `chacha8/ref/chacha.c` | `20080118` | **public domain** | 128-bit key 的 `tau` 常量、64-bit counter + 8-byte IV 布局、测试语义 |
| `secworks/chacha_testvectors` | `a56fff0` | **BSD-2** | TC1（128-bit 零 key）与 256-bit 零 key 的 8/12/20 rounds 向量 |
| RFC 8439 | — | IETF | ChaCha20 block 与加密向量（§2.3.2、§2.4.2） |

MIT 全文保留在 `chacha8.c` 文件头，`chacha8.h` 指向该处。相对 tiny-chacha 的改动：

1. 64-bit block counter（`state[12..13]`）与 8-byte nonce（`state[14..15]`），对齐 Bernstein
   的 `chacha/ref` 布局；tiny 原版是 32-bit counter + 96-bit IV。
2. 128-bit key 使用 `tau`（`"expand 16-byte k"`）并把 key 重复成 8 个字；tiny 原版始终用
   `sigma`，因此无法复现官方 128-bit 向量。
3. counter 回绕保护：2^64 块之后拒绝再出块，而不是静默复用 counter 0。
4. `chacha8_wipe` 用 volatile 写清零上下文。
5. 显式 little-endian load/store；tiny 的大端分支引用了未定义的 `SECURE_MEMCPY`。
6. 去掉 bool / XChaCha / feature flag，只留本原型需要的路径。

## 2. API 与状态布局

```c
int  chacha8_init(chacha8_ctx *ctx, const uint8_t *key, size_t key_len,
                  const uint8_t nonce[8], uint64_t counter, int rounds);
int  chacha8_crypt(chacha8_ctx *ctx, const uint8_t *in, size_t len, uint8_t *out);
int  chacha8_xor(const uint8_t *key, size_t key_len, const uint8_t nonce[8],
                 uint64_t counter, int rounds,
                 const uint8_t *in, size_t len, uint8_t *out);
void chacha8_wipe(chacha8_ctx *ctx);
```

| 项 | 语义 |
|---|---|
| `key_len` | 16 或 32；其他值返回 `-1` |
| `rounds` | 8、12 或 20；其他值返回 `-1` |
| `nonce` | 8 字节，little-endian 装入 `state[14..15]` |
| `counter` | 64-bit 块计数器，`state[12]` 低字、`state[13]` 高字 |
| `chacha8_crypt` | 增量流式；`in == out` 允许；`len == 0` 返回 0 |
| 回绕 | 出完 `0xffffffffffffffff` 号块后 `wrapped=1`；下一次需要新块时返回 `-1`，本次已写出的部分输出由调用方丢弃 |
| `chacha8_wipe` | 清零整个上下文（含 padding） |

状态布局（Bernstein，不是 RFC 8439）：

| 字 | 内容 |
|---|---|
| `state[0..3]` | 常量：256-bit key 用 `sigma`，128-bit key 用 `tau` |
| `state[4..11]` | key words，little-endian；16-byte key 重复两次 |
| `state[12..13]` | 64-bit block counter，低字在前 |
| `state[14..15]` | 8-byte nonce，little-endian |

每块 64 字节；`pos` 记录当前块已消费的字节数。`chacha8_init` 不预生成块，第一次
`chacha8_crypt` 才生成 counter 指定的块。

## 3. 计数器、nonce 与 rekey

**32-bit counter 的悬崖。** tiny-chacha / RFC 8439 布局的 counter 只有 32 位：

```
2^32 blocks × 64 B = 2^38 B = 256 GiB
256 GiB ÷ 125 MB/s = 2199 s ≈ 36.6 min
```

即 1 Gbit/s 持续打流约 37 分钟就必须换 key 或 nonce，否则 counter 回到 0，同一
key/nonce 下重复使用 keystream。参考文档 §9.1 已指出这一点 `[推算]`。

**64-bit counter 的上限。** 本实现每块递增 `state[12..13]`：

```
2^64 blocks × 64 B = 2^70 B ≈ 1.18 ZB
1.18 ZB ÷ 125 MB/s ≈ 2.99 × 10^5 年
```

因此 counter 回绕不再是工程约束；`wrapped` 保护只是防止 API 被误用后静默复用。
`[推算]`

**8-byte nonce 的生日界。** nonce 空间只有 64 位。若每包随机取 nonce、同一 key 下长期
使用，则碰撞概率约为 `N^2 / 2^65`；`N = 2^32` 包时约 39%。按 1G、1200 B 包计算，
`2^32` 包约 11.5 小时。这不是本 core 的 bug，而是 wire 设计的约束 `[推算]`。可选做法：

- 每会话独立 key，nonce 用包序号（counter 型），不做随机；
- 或把 nonce 扩到 96-bit（需改 API 与状态布局，回到 RFC 8439 形状）；
- 或按会话/按 `2^31` 包主动 rekey。

**rekey 策略**不属于 core：core 只保证同一 key/nonce 下 counter 不回绕。建议在会话边界
或每 `2^32` 块强制换 key；两者都远早于 64-bit 上限。

## 4. KAT

测试文件 `test_chacha8.c`，逐字节比较。向量与来源：

| # | key | nonce | counter | rounds | 字节 | 期望（首 4 … 末 4） | 来源 / 交叉验证 |
|---|---|---|---:|---:|---:|---|---|
| 1 | 16×`00` | 8×`00` | 0 | 8 | 128 | `e28a5fa4…338e430d` | secworks TC1 block 0–1；Bernstein ref 复现 `[实测]` |
| 2 | 32×`00` | 8×`00` | 0 | 8 | 64 | `3e00ef2f…0e0cfe42` | secworks 256-bit 零 key；Bernstein/tiny/Linux 复现 `[实测]` |
| 3 | 32×`00` | 8×`00` | 0 | 12 | 64 | `9bf49a6a…411326be` | secworks 256-bit 零 key；tiny/Linux 复现 `[实测]` |
| 4 | 32×`00` | 8×`00` | 0 | 20 | 64 | `76b8e0ad…b2ee6586` | secworks 256-bit 零 key；Bernstein/tiny/Linux 复现 `[实测]` |
| 5 | `00..1f` | `a0..a7` | 0 | 8 | 128 | `9a1e949f…e415e88b` | Bernstein ref + tiny + Linux `chacha_block_generic` `[实测]` |
| 6 | `00..1f` | `4a…`（映射） | 1 | 20 | 64 | `10f1e7e4…a2503c4e` | RFC 8439 §2.3.2 |
| 7 | `00..1f` | `4a…`（映射） | 1 | 20 | 114 | `6e2e359a…5e42874d` | RFC 8439 §2.4.2 |

第 5 组即参考文档 §7.2 的向量；该文档只列了前 64 字节，后 64 字节
（从 `09d530f4` 开始，以 `b6266796e415e88b` 结束）本次用 Bernstein ref 补验，完全一致 `[实测]`。第 6/7 组把
RFC 的 96-bit nonce 映射到本布局：counter 取 `0x09000000_00000001`，nonce 取
`00 00 00 4a 00 00 00 00`，见 `test_chacha8.c` 注释。

第 2–4 组与第 5 组的前 64 字节另用 Linux `chacha_block_generic`（7.4.0-rc1，仅作对照，不进入代码）在
用户态 shim 下复现，三份独立实现（Bernstein ref、tiny-chacha、Linux generic）输出一致
`[实测]`。

结构性检查：

- counter 1 的块等于 counter 0 流中的第 2 块；
- `0xffffffff + 1` 进位到 `0x100000000`，两块输出连续；
- 7 段任意切分的增量调用等于一次性调用；
- `in == out` 原地加密等于一次性调用；
- 1..64 字节的每个长度都等于整块的前缀；
- 回绕后调用返回 `-1`，65 字节跨回绕调用也返回 `-1`；
- 19 项非法参数（key 长度、rounds、空指针）全部拒绝；
- wipe 后上下文每个字节为 0。

关键区分：同一份 secworks 文件里，**128-bit key 段用 `tau` 常量，256-bit key 段用
`sigma`**；两者输出不同，不能混用。本实现按 `key_len` 选择常量；tiny-chacha 原版固定
`sigma`，正是它复现不了 128-bit TC1 的原因。

## 5. 基准

方法：`cc -std=c99 -O3`，单核 `taskset`，热缓存，原地 XOR，每趟从 counter 0 重新
init（含每包初始化成本），每 case 采样 3 次取中位数，单次 ≥0.25 s（代表运行用 0.3 s）。
MB/s 按 10^6 B/s。
cycles/B 用 `/proc/cpuinfo` 报告的最高核心 MHz 折算；该值是瞬时采样，只能作量级参考。

本机：AMD Ryzen 9 7940H，16 线程，最高 5263 MHz，Linux 7.2.9-1-MANJARO，
GCC 16.2.1。代表运行（`cpu_mhz: 4848`）：

| rounds | 1 KiB MB/s | cycles/B | 8 KiB MB/s | cycles/B | 64 KiB MB/s | cycles/B |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 1130.4 | 4.29 | 1164.8 | 4.16 | 1172.5 | 4.13 |
| 12 | 884.2 | 5.48 | 901.5 | 5.38 | 918.8 | 5.28 |
| 20 | 621.5 | 7.80 | 631.7 | 7.67 | 634.0 | 7.65 |

五次独立运行（不同核心）的 MB/s 波动约 ±12%；首次运行频率未爬升，整体偏低。
cycles/B 随采样频率漂移，ChaCha8 在 4.1–4.6 之间。

对照：

| 项 | rounds | 本机 MB/s | 参考 MB/s | 说明 |
|---|---:|---:|---:|---|
| 本实现 | 8 | 1130–1173 | 1060–1125 | Bernstein ref 1060，tiny-chacha 1125 `[实测]` |
| 本实现 | 20 | 622–634 | 663 | Bernstein ref `[实测]` |
| AES-128-CBC（AES-NI） | — | 1880 | — | `openssl speed -evp aes-128-cbc -bytes 8192`，本机 `[实测]` |
| AES-128-CBC（HND0） | — | — | 144.5 | 无 `aes` flag，软件 AES，`review/microbench.md` §3.4 |
| AES-128-CBC（.65 A76） | — | — | 1796.3 | ARMv8 crypto 扩展 |
| AES-128-CBC（.74 Athlon） | — | — | 309.3 | AES-NI 但每 cycle 仅约 0.17 B |

观察：

- ChaCha8 比 ChaCha20 快约 **1.85×**（1130/622 到 1173/634）。rounds 比是 2.5×，
  差值来自每块固定的 feedforward、序列化和 XOR 成本。
- 1 KiB 与 64 KiB 差距很小（ChaCha8 约 4%），说明每包 init 的成本可忽略；真正的
  成本是每字节的 8 个 double-round。
- 与 Ryzen 参考同量级：ChaCha8 与 tiny-chacha（1125）相当、比 Bernstein ref（1060）
  高约 7–10%；ChaCha20 比 Bernstein ref（663）低约 4–6%。参考实现同样是无 SIMD 的
  纯 C，差异在编译器与测量口径。
- 本机 AES-NI 比 ChaCha8 快 1.6×；但 HND0 没有 AES 指令，软件 AES 只有 144.5 MB/s，
  这正是选 ChaCha 的理由。

## 6. 与 1G 和 HND0 的关系

1 Gbit/s 物理口 = 125 MB/s。本机单核 ChaCha8 约 1.15 GB/s，占用约 11% `[推算]`。
HND0 是 QEMU vCPU、只有 SSE2、2 vCPU，本次未测；按同代码在受限 x86 上 1/3–1/2 的
吞吐推算，约 380–590 MB/s，仍有 3–4.7 倍余量 `[推算][待测]`。即使按最保守的 1/4，
也有 2.3 倍。

与 FEC 的 XOR 预算比较：HND0 单核 XOR 约 8.9 GB/s（`review/microbench.md` §4）。
ChaCha8 每字节约 4.3 cycles，XOR 每字节约 0.25–0.6 cycles，加密成本比 XOR 高约一个
数量级（7–17 倍）。但 1G 下两者分别是 125 MB/s 和 667 MB/s–2 GB/s 的预算，各自都有
数倍余量。加密不是新的瓶颈。

注意口径：1 KiB 行已包含每包 `init`（64 字节 memset + 16 个字写入）；真实包处理还要
加上 FEC 编解码与协议栈，本表只回答“密码学核心是否够快”。

## 7. 局限与未做

- **纯标量 C。** 没有手写 SSE2/AVX2/NEON；`-O3` 也没有自动向量化（`objdump` 里只有
  1 条 `pxor`）。若未来需要更高吞吐，SIMD 是明确的剩余空间，但 1G 不需要。
- **无认证。** 没有 Poly1305/AEAD，不能防篡改；FEC 也不是完整性保护。需要认证时必须在
  外层加 MAC，或换成 ChaCha20-Poly1305。
- **无头部保护。** 只加密 body；协议头要么明文，要么由别的机制保护。
- **无密钥管理。** nonce 生成、会话密钥、rekey 触发都不在本文件。
- **无恒定时间审计。** 实现没有查表、没有数据相关分支，但未做形式化或侧信道测量。
- **cycles/B 是估计。** 频率来自 `/proc/cpuinfo` 瞬时值，误差约 ±10%。
- **热缓存。** 64 KiB 在 L1/L2 内；冷内存或跨 NUMA 的数字会低一些。
- **未上 HND0/.65。** ARM NEON 与 QEMU SSE2 的实际吞吐仍需实测。

## 8. 构建与运行

```sh
# KAT
cc -std=c99 -O2 -Wall -Wextra -Werror -o test_chacha8 test_chacha8.c chacha8.c
./test_chacha8

# 吞吐
cc -std=c99 -O3 -Wall -Wextra -Werror -o bench_chacha8 bench_chacha8.c chacha8.c
./bench_chacha8                 # 默认 0.25 s/case
./bench_chacha8 --seconds 0.3 --mhz 4848
```

两条命令都是单行 `cc`；无 C++、无 STL、无外部库。
