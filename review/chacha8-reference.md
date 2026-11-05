# ChaCha8 参考实现与内核移植调研

状态：调研稿。范围只限 **ChaCha8 / ChaCha20 对照**；不扩展到其他密码。
基线：`master 0db5166`；生产对端 HND0：256 流 / 物理 1G / grouped 32×8 / app 500–750M；HND0 2 vCPU / 955 MiB。
证据标记：`[实测]` 本机直接编译/运行；`[源码]` 直接读到的代码/接口；`[推算]` 由操作数/架构比例推出；`[待测]` 必须上机。

## 0. 结论摘要

| 问题 | 结论 |
|---|---|
| 第一参考 | **Bernstein eSTREAM `chacha8/ref/chacha.c`**：public domain，`chacha-ref.c version 20080118`；128/256-bit key、8-byte IV、64-bit counter；8 rounds 写死在循环里；无依赖，最适合做语义参考。 |
| 第二参考 | **`0xNullll/tiny-chacha`**：MIT，C，277 行；ChaCha8/12/20，128/256-bit key，**96-bit IV + 32-bit counter**，增量 API，无 malloc，大小端都支持；最适合内核移植。 |
| 内核现状 | `chacha_crypt(..., nrounds)` / `chacha_block_generic(..., nrounds)` 接受 8，但 `chacha_permute` 有 `WARN_ON_ONCE(nrounds != 20 && nrounds != 12)`；crypto API 只注册 `chacha20`/`xchacha20`/`xchacha12`，**没有 `chacha8`**。模块可以调 `EXPORT_SYMBOL_GPL`，但会触发 WARN/不受支持；不要依赖。 |
| 当前内核 arch 代码 | 当前 master（7.4.0-rc1）**没有** `arch/x86/crypto/chacha*`、`arch/arm64/crypto/chacha*`；只有 `lib/crypto/chacha.c` + `chacha-block-generic.c`。v6.6/v5.15 曾有 arch glue（传 `nrounds`），但也没有 8-round crypto API。 |
| libsodium / monocypher / libgcrypt / OpenSSL | **都只提供 ChaCha20**（固定 20 rounds），没有 rounds 参数；不能直接拿 ChaCha8。 |
| 测试向量 | `secworks/chacha_testvectors`（BSD-2）有 ChaCha8 官方 draft 向量；TC1（128-bit all-zero key/IV，8 rounds）首块 `e28a...`。我们另生成 256-bit key/IV/counter 向量，Bernstein ref、tiny-chacha、Linux generic 三份实现一致。 |
| 内核移植建议 | 用 **tiny-chacha（MIT）** 或 Bernstein ref（public domain）做本地 C 实现；不要改内核 whitelist，也不要依赖 `chacha_crypt(...,8)` 的 WARN 路径。tiny 的 32-bit counter 在 1G 下约 37 分钟回绕，必须 rekey 或换 64-bit counter。 |

## 1. 参考实现与版本

| 项目 | 版本/commit | 日期 | 许可证 | 语言 |  rounds | 结论 |
|---|---|---|---|---|---|---|
| Bernstein eSTREAM `chacha8/ref/chacha.c` | `chacha-ref.c version 20080118` | 2008-01-18 | **public domain** | C89 | 8 | 第一语义参考；无依赖；64-bit counter |
| Bernstein eSTREAM `chacha20/ref/chacha.c` | 同上 | 2008-01-18 | public domain | C89 | 20 | 对照；证明 8/20 只差循环次数 |
| `0xNullll/tiny-chacha` | `890a5f0` | 2026-02-17 | **MIT** | C | 8/12/20 | 第二参考；内核友好；96-bit IV/32-bit counter |
| `glycerine/chacha8c` | `3a9ac61` | 2026-05-26 | BSD-3 | C/C++/Go | 8 | Go stdlib ChaCha8 的 C 移植；seed-only CSPRNG，无 nonce/counter |
| `secworks/chacha_testvectors` | `a56fff0` | 2016-04-21 | **BSD-2** | 文本/生成器 | 8/12/20 | 官方 draft 测试向量；不是实现 |
| Linux `lib/crypto/chacha.c` + `chacha-block-generic.c` | `d68ce4e`（7.4.0-rc1） | 2026-11-05 | GPL-2.0-or-later | C | 20/12（8 仅未支持） | `nrounds` 可变，但 WARN/无 chacha8 API |
| libsodium | master | — | ISC | C | 20 | 只有 ChaCha20 |
| Monocypher | master | — | BSD-2/CC0 | C | 20 | 只有 ChaCha20 |
| libgcrypt | master | — | LGPL-2.1+ | C | 20 | 只有 ChaCha20 |
| OpenSSL | master | — | Apache-2.0 | C | 20 | 只有 `EVP_chacha20` |
| `joostrijneveld/chacha-arm-cortex-m` | 2017 | 2017-07-10 | CC0 | ARM asm | 8/12/20 | Cortex-M3 汇编；不是内核 |
| `database64128/chacha8-go` | 2025 | 2025-08-15 | BSD-3 | Go | 8 | Go；不是 C |

## 2. Bernstein eSTREAM ref（第一参考）

- URL：`https://cr.yp.to/streamciphers/timings/estreambench/submissions/salsa20/chacha8/ref/chacha.c`
- 头文件：同目录 `ecrypt-sync.h`；`ecrypt-portable.h` 需从 eSTREAM 包或自写最小 shim（我们测试时自写了 15 行类型/端序宏，仅测试用）。
- 许可证：文件头写 `D. J. Bernstein / Public domain`。
- 结构：`ECRYPT_keysetup(ctx,key,kbits,ivbits)`、`ECRYPT_ivsetup(ctx,iv)`、`ECRYPT_keystream_bytes(ctx,out,bytes)`；`input[16]` 状态：constants + 8 key words + 2 counter words + 2 IV words。
- rounds：`salsa20_wordtobyte` 里 `for (i = 8; i > 0; i -= 2)`，即 8 rounds；`chacha20/ref` 同文件结构，循环 20。没有运行时 rounds 参数；8/20 是两份源码。
- 语义：128-bit key 用 `tau` 常量并把 key 重复；256-bit key 用 `sigma`；IV 8 bytes，counter 64-bit；`ECRYPT_encrypt_bytes` 每块递增 counter。
- 测试：TC1（128-bit all-zero key/IV，8 rounds）首块与我们编译输出一致：`e28a5fa4a67f8c5defed3e6fb7303486aa8427d31419a729572d777953491120...` `[实测]`
- 内核边界：纯 C、无 malloc、无 libc（除 memcpy 语义）；可提取 `salsa20_wordtobyte` + `keysetup/ivsetup`；64-bit counter 适合长流；8-byte IV 与 RFC7539 的 96-bit nonce 不同，tbtun 要自己定 wire 布局。`[源码]`

## 3. tiny-chacha（第二参考，推荐移植）

- URL：`https://github.com/0xNullll/tiny-chacha`，commit `890a5f0`，2026-02-17，MIT。
- 代码量：`src/tiny_chacha.c` 277 行 + `src/tiny_chacha.h` 176 行；无 malloc/stdio；只用 `stdint.h/stddef.h/string.h/stdbool.h`。`[源码]`
- API：`ChaChaInit(ctx,key,key_len,iv[12],counter,rounds)`、`ChaChaCipher(ctx,in,len,out)`；支持 8/12/20、128/256-bit key；96-bit IV + 32-bit counter（RFC7539 布局）；增量流式。`[源码]`
- 端序：显式处理 little/big endian；MIPS/ARM 可用。`[源码]`
- 测试：我们用 256-bit key `00..1f`、8-byte IV `a0..a7`、counter 0、rounds=8，映射到 tiny 的 12-byte IV（前 4 字节 0，后 8 字节 `a0..a7`），输出与 Bernstein ref、Linux generic 完全一致：`9a1e949f0d71a3a3...` `[实测]`
- 性能（本机 Ryzen 9 7940H，单核，64 MiB）：tiny ChaCha8 约 **1125 MB/s**；Bernstein ref ChaCha8 约 1060 MB/s，ChaCha20 约 663 MB/s。`[实测，host]`
- 内核边界：MIT 与 GPLv2 兼容；C、无 malloc、无 STL；把 `<string.h>` 换成内核头即可；`FORCE_INLINE` 用 `__attribute__`，内核 GCC 可用；**32-bit counter** 在 1G 下 `2^32×64 B = 256 GiB`，约 37 分钟回绕；必须 rekey/换 nonce，或改用 64-bit counter 变体。`[源码/推算]`

## 4. Linux kernel 现状

当前 master：`d68ce4e`，`7.4.0-rc1`（2026-11-05）。`[实测/源码]`

### 4.1 generic 代码

- `lib/crypto/chacha-block-generic.c`：`chacha_block_generic(state, out, nrounds)`；`chacha_permute` 用 `nrounds` 循环，但开头有：
  ```c
  WARN_ON_ONCE(nrounds != 20 && nrounds != 12);
  ```
  传 8 会计算 8 rounds，但触发 WARN；不是支持的路径。`[源码]`
- `lib/crypto/chacha.c`：`chacha_crypt_generic` 是 static；公开的是 `chacha_crypt(state,dst,src,bytes,nrounds)`，`EXPORT_SYMBOL_GPL(chacha_crypt)`；`chacha_block_generic` 也 `EXPORT_SYMBOL`。`[源码]`
- `include/crypto/chacha.h`：注释明确「generic ChaCha code currently allows only the 20 and 12-round variants」。`[源码]`

### 4.2 crypto API

- `crypto/chacha.c` 只注册 `chacha20`、`xchacha20`、`xchacha12`；没有 `chacha8`。`crypto_alloc_skcipher("chacha8")` 不会成功。`[源码]`
- 因此「ChaCha8 只是传 8」在 **generic 函数层**成立，在 **crypto API 层**不成立；没有官方 `chacha8` 算法。

### 4.3 arch 代码

- 当前 master **没有** `arch/x86/crypto/chacha*`、`arch/arm64/crypto/chacha*`；tree 里只有 `arch/*/kernel/vdso/vgetrandom-chacha.S`。`[源码]`
- v6.6/v5.15 曾有 `arch/x86/crypto/chacha_glue.c`、`arch/arm64/crypto/chacha-neon-glue.c`；它们把 `nrounds` 传给 asm，但同样只由 crypto API 以 20/12 调用，没有 8-round 注册。`[源码]`
- 结论：不要假设 arch 路径会给 8 rounds 加速；当前 master 只有 generic。

### 4.4 模块可用性

- `tbtun` 是 `MODULE_LICENSE("GPL")`，可以调用 `EXPORT_SYMBOL_GPL(chacha_crypt)`/`EXPORT_SYMBOL(chacha_block_generic)`。`[源码]`
- 但传 `nrounds=8` 会触发 `WARN_ON_ONCE`（内核 taint/日志），且不受支持；生产不应依赖。
- 推荐：把 tiny-chacha（MIT）或 Bernstein ref（public domain）作为模块内 C 实现，不碰内核 whitelist。

## 5. libsodium / Monocypher / libgcrypt / OpenSSL

| 库 | API | rounds | 结论 |
|---|---|---|---|
| libsodium | `crypto_stream_chacha20` / `_ietf` | 20 | 无 rounds 参数；不能直接 ChaCha8 |
| Monocypher | `crypto_chacha20_djb` / `_ietf` | 20 | 源码 `FOR(i,0,10)` = 20 rounds；无参数 |
| libgcrypt | `GCRY_CIPHER_CHACHA20` | 20 | 只有 ChaCha20 |
| OpenSSL | `EVP_chacha20` | 20 | 只有 ChaCha20；无 rounds 参数 |

这些都是 ChaCha20-only；要 ChaCha8 只能 fork/改循环，不如直接用 tiny-chacha。`[源码/文档]`

## 6. GitHub 上的 ChaCha8 实现

| 项目 | 许可证 | 语言 | 特点 | 可用性 |
|---|---|---|---|---|
| `0xNullll/tiny-chacha` | MIT | C | ChaCha8/12/20、96-bit IV/32-bit counter、增量、无 malloc | **推荐移植** |
| `glycerine/chacha8c` | BSD-3 | C/C++/Go | Go stdlib ChaCha8 移植；seed-only CSPRNG；无 nonce/counter | 可作第二实现/交叉验证，但不适合 packet nonce |
| `joostrijneveld/chacha-arm-cortex-m` | CC0 | ARM asm | Cortex-M3 的 ChaCha8/12/20  permutation | 只作汇编参考 |
| `secworks/chacha_testvectors` | BSD-2 | 文本/生成器 | 官方 draft 向量（8/12/20） | **测试向量** |
| `database64128/chacha8-go` | BSD-3 | Go | ChaCha8/ChaCha8-Poly1305 | Go，不是 C |
| `abeaumont/ocaml-chacha` | BSD-2 | OCaml | ChaCha8/12/20 | 不是 C |
| `chocolatkey/chacha8` | AGPL-3.0 | Go | — | 许可证不适合 |
| `hanna-kruppe/chacha8rand` | 无 license | Rust | ChaCha8 CSPRNG | 不可用 |
| `nixberg/chacha8rand-rs` | MIT | Rust | ChaCha8 CSPRNG | 不是 C |

## 7. 测试向量

### 7.1 官方 TC1（128-bit key，8 rounds）

来源：`secworks/chacha_testvectors`，`src/chacha_testvectors.txt`，TC1；BSD-2；commit `a56fff0`。`[源码/实测]`

```
Key: 16×0x00
IV:  8×0x00
Rounds: 8
Keystream block 0:
e2 8a 5f a4 a6 7f 8c 5d ef ed 3e 6f b7 30 34 86
aa 84 27 d3 14 19 a7 29 57 2d 77 79 53 49 11 20
b6 4a b8 e7 2b 8d eb 85 cd 6a ea 7c b6 08 9a 10
18 24 be eb 08 81 4a 42 8a ab 1f a2 c8 16 08 1b
```

我们用 Bernstein `chacha8/ref` 以 128-bit key 复现，完全一致。`[实测]`

### 7.2 生成的 256-bit 向量（三实现交叉校验）

```
key = 00 01 02 ... 1f (32 bytes)
IV  = a0 a1 ... a7 (8 bytes)
counter = 0
rounds = 8
Keystream block 0 (64 bytes):
9a1e949f0d71a3a307da58730300dea725342ee45cf066d8350095e3c443cee2
68db83e2ce8d899d90e96aef644b1c8493c228cdaa420c5603d745f14127b4ba
```

- Bernstein ref、tiny-chacha（IV 映射为 `00 00 00 00 a0..a7`，counter 0）、Linux `chacha_block_generic(...,8)` 三者 128 字节输出完全一致。`[实测]`
- Linux generic 传 8 会触发 `WARN_ON_ONCE`，我们在用户态 shim 里屏蔽 WARN 后验证；这证明算法一致，不代表内核支持 8。`[实测/源码]`

## 8. 性能

本机 Ryzen 9 7940H，单核，64 MiB，`-O3`：`[实测，host]`

| 实现 | rounds | 吞吐 |
|---|---:|---:|
| Bernstein ref C | 8 | 1060 MB/s |
| Bernstein ref C | 20 | 663 MB/s |
| tiny-chacha C | 8 | 1125 MB/s |

- ChaCha8 比 ChaCha20 约快 1.6×（ref C；SIMD 实现比例可能不同）。
- 1G 物理口 = 125 MB/s；ChaCha8 只占单核约 11%（按 1125 MB/s），不是瓶颈。
- HND0 2 vCPU/500–750M app = 62.5–93.75 MB/s；更不是瓶颈。
- MIPS 1004Kc 无 MSA，估计 50–150 MB/s；34 Mbit/s 生产足够，1G 不够；需实测。`[推算]`

## 9. 内核移植边界

### 9.1 推荐：tiny-chacha

- MIT；C；277 行；无 malloc/stdio；只依赖 `stdint/stddef/string/stdbool`；内核可用。`[源码]`
- 96-bit IV + 32-bit counter：与 RFC7539 一致；每 key/nonce 最多 256 GiB，1G 下约 37 分钟；必须 rekey 或换 64-bit counter 变体。`[推算]`
- 需要改：`#include <string.h>` → 内核 `<linux/string.h>`；`SECURE_MEMCPY` 保持 `memcpy`；`FORCE_INLINE` 的 `__attribute__((always_inline))` 内核 GCC 可用；无浮点、无分配。
- 优点：支持 8/12/20，测试方便；MIT 与 GPLv2 兼容。

### 9.2 备选：Bernstein ref

- public domain；C89；无依赖；64-bit counter + 8-byte IV；适合长流/自定义 wire。
- 需要 `ecrypt-sync.h`/`ecrypt-portable.h`（eSTREAM 包或最小 shim）；`ECRYPT_encrypt_bytes` 每块递增 64-bit counter；无 malloc。
- 若 tbtun 要 96-bit nonce/32-bit counter，需自己改 IV/counter 布局；tiny 更贴近 RFC7539。

### 9.3 不推荐：内核 `chacha_crypt(...,8)`

- `WARN_ON_ONCE(nrounds != 20 && nrounds != 12)`；会 taint/日志；不是支持路径。
- crypto API 没有 `chacha8`；`crypto_alloc_skcipher("chacha8")` 失败。
- 当前 master 无 arch ChaCha 文件；不要假设 AVX2/NEON 加速。

## 10. 给 fec-core2 的输入

1. **首选 tiny-chacha**：MIT/C/277 行/无 malloc，直接作为模块内 ChaCha8 实现；固定 key/nonce/counter 向量见 §7；32-bit counter 必须 rekey（1G 约 37 分钟）。
2. **备选 Bernstein ref**：public domain，64-bit counter/8-byte IV；适合长流或自定义 nonce 布局；需要 eSTREAM 头 shim。
3. **测试**：用 §7.1 官方 TC1 和 §7.2 生成的 256-bit 向量做双实现交叉校验；再用 tiny 的 8/12/20 输出确认 rounds 参数。
4. **不要**用 libsodium/Monocypher/libgcrypt/OpenSSL 直接拿 ChaCha8（只有 20）；不要依赖内核 `chacha_crypt(...,8)` 的 WARN 路径。
5. **性能**：ChaCha8 在 HND0 1G 下不是瓶颈；MIPS 1004Kc 只适合 34 Mbit/s 生产，不适合 1G。

## 11. 参考文献

- Bernstein eSTREAM ChaCha8 ref：`https://cr.yp.to/streamciphers/timings/estreambench/submissions/salsa20/chacha8/ref/chacha.c`（version 20080118，public domain）。
- Bernstein eSTREAM ChaCha20 ref：同目录 `chacha20/ref/chacha.c`。
- tiny-chacha：`https://github.com/0xNullll/tiny-chacha`，commit `890a5f0`，MIT。
- chacha8c：`https://github.com/glycerine/chacha8c`，commit `3a9ac61`，BSD-3。
- secworks 测试向量：`https://github.com/secworks/chacha_testvectors`，commit `a56fff0`，BSD-2；`src/chacha_testvectors.txt`。
- Linux：`https://github.com/torvalds/linux`，commit `d68ce4e`（7.4.0-rc1）；`lib/crypto/chacha.c`、`lib/crypto/chacha-block-generic.c`、`include/crypto/chacha.h`、`crypto/chacha.c`。
- v6.6/v5.15 arch 参考：`arch/x86/crypto/chacha_glue.c`、`arch/arm64/crypto/chacha-neon-glue.c`（当前 master 已无这些文件）。
- libsodium / Monocypher / libgcrypt / OpenSSL：各自 ChaCha20 实现，固定 20 rounds。
