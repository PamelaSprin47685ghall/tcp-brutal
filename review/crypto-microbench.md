# 微基准：ChaCha20/12/8 与 AES-128-CBC 在三台候选机上的单核开销

基线 `0db5166`，分支 `w-microbench`。源码 `review/microbench/crypto.c`，运行脚本
`review/microbench/run_crypto.sh`，原始日志 `review/microbench/logs/crypto-{hnd0,arm65,x86-74}.log`。
2026-11-06 测得。**tbtun 当前没有加密**，AES 只是 CPU 锚点；本文回答「若将来要加密，
在 HND0 这类没有 AES 指令的机器上，快速流密码是否值得替代 AES」。

## 0. 结论

1. **HND0 上 ChaCha8 明显值得替代 AES。** 4 路 SIMD 版 1317 MB/s，是软件 AES-128-CBC
   （131 MB/s）的 **10.1 倍**；1 Gbit/s（125 MB/s）只占单核 **9.5%**，而 AES 要占 **95%**，
   几乎打满一个 vCPU。ChaCha20 也有 4.9 倍余量，但 ChaCha8 才是「1G 下几乎免费」的档位。
2. **ARM（.65）上不要换。** A76 有 ARMv8 crypto，AES-128-CBC 1809.9 MB/s，比 ChaCha8x4
   的 1433.2 MB/s 还快 26%；两者在 1G 下都只占单核 7–9%。
3. **老 x86（.74）上 ChaCha8 仍快 4.4 倍**（1373.6 vs 309.4 MB/s）；该机虽有 AES-NI，
   但 1.8 GHz 的 Piledriver 实测 AES 只有 5.8 cycles/byte，硬件路径并不快。
4. **ChaCha8 是「不需要很强安全」场景的正确选择**：8 轮比 20 轮少 60% 的轮函数，实测
   吞吐约为 ChaCha20 的 2.1 倍；但它不是抗密码分析的强度，只适合混淆/防轻量窥探。

## 1. 方法

- 单一 C 源文件，两个实现：
  - `chachaN`：标量 C，16 个状态字用局部变量，`-O3` 下由编译器自行 SLP 向量化；
  - `chachaNx4`：GCC vector extension 的 4 lane（16 字节）并行，一条指令处理 4 个块，
    x86-64 上是 SSE2、AArch64 上是 NEON。
- 每项在 4 MiB 缓冲上做 keystream 生成 + XOR，9 次取中位数；整个程序跑 3 遍，
  文档取 3 遍的中位数。计时用 `CLOCK_PROCESS_CPUTIME_ID`（进程 CPU 时间），
  共享机上的抢占不会伪装成吞吐骤降。
- 频率：HND0 2200 MHz、.65 大核 2304 MHz、.74 1792 MHz；`cycles/byte = 频率 / 字节率`。
- 全部 `nice -n 19` + `taskset` 单核；HND0 只跑 cpu0；不装包、不加载模块、不改网络。

## 2. 三机环境

| | HND0（生产） | .65（ARM） | .74（x86 桌面） |
|---|---|---|---|
| CPU | QEMU Virtual CPU 2.5+ | RK3588 Cortex-A76 | Athlon X4 760K |
| 频率 | 2200 MHz | 2304 MHz | 1792 MHz（固定） |
| SIMD | `sse2`（无 AVX/AES） | `asimd`（NEON，无 SVE） | `sse2 ssse3 sse4.1/4.2 avx fma aes`（无 AVX2） |
| 加密指令 | 无 | `aes pmull sha2` | `aes pclmulqdq` |
| AES 实测 | 131.0 MB/s（软件） | 1809.9 MB/s（硬件） | 309.4 MB/s（硬件路径，但很慢） |

## 3. 结果（4 MiB，9 次中位，单核）

### 3.1 HND0（2.2 GHz，SSE2）

| 变体 | MB/s | ns/byte | cycles/byte |
|---|---:|---:|---:|
| ChaCha20 标量 | 330.0 | 3.03 | 6.66 |
| ChaCha20 x4 | 647.4 | 1.54 | 3.40 |
| ChaCha12 标量 | 548.1 | 1.82 | 4.01 |
| ChaCha12 x4 | 996.6 | 1.00 | 2.21 |
| ChaCha8 标量 | 770.0 | 1.30 | 2.86 |
| **ChaCha8 x4** | **1316.8** | **0.76** | **1.67** |
| AES-128-CBC | 131.0 | 7.63 | 16.8 |

HND0 的 gcc 15.2 没有把标量 ChaCha 的四个独立 quarter-round 做 SLP 向量化
（标量只有 2.9–6.7 c/B，4 路版快 1.7–2.0 倍）；x4 是这台机器的正确实现。

### 3.2 .65（A76 2.304 GHz，NEON + AES）

| 变体 | MB/s | ns/byte | cycles/byte |
|---|---:|---:|---:|
| ChaCha20 标量 | 396.6 | 2.52 | 5.81 |
| ChaCha20 x4 | 651.1 | 1.54 | 3.54 |
| ChaCha12 标量 | 634.1 | 1.58 | 3.63 |
| ChaCha12 x4 | 1029.5 | 0.97 | 2.24 |
| ChaCha8 标量 | 905.0 | 1.11 | 2.55 |
| **ChaCha8 x4** | **1433.2** | **0.69** | **1.60** |
| AES-128-CBC | 1809.9 | 0.55 | 1.27 |

NEON 没有单指令 32 位循环移位，x4 的 `vrotl` 由移位+或合成，仍比标量快 1.6 倍。
AES 走 ARMv8 crypto（`OPENSSL_armcap=0xbd`，AES+PMULL），仍是全场最快。

### 3.3 .74（Athlon 1.792 GHz，128 位 SSE/AVX1）

| 变体 | MB/s | ns/byte | cycles/byte |
|---|---:|---:|---:|
| ChaCha20 标量 | 697.1 | 1.43 | 2.58 |
| ChaCha20 x4 | 618.4 | 1.62 | 2.90 |
| ChaCha12 标量 | 1038.4 | 0.96 | 1.73 |
| ChaCha12 x4 | 935.2 | 1.07 | 1.92 |
| **ChaCha8 标量** | **1373.6** | **0.73** | **1.31** |
| ChaCha8 x4 | 1258.8 | 0.79 | 1.43 |
| AES-128-CBC | 309.4 | 3.23 | 5.81 |

这台机器上 gcc 16 的 SLP 把标量版向量化得很好（SSE2/AVX1 128 位），
而 x4 的合成循环移位反而多花指令，所以**标量更快**。同一份源码在不同编译器/ISA 上
的最优实现不同，这一点必须在选型时实测，不能只看「有没有 SIMD」。

## 4. 1 Gbit/s（125 MB/s）下单核占用

| 机 | 最佳 ChaCha8 | 单核占比 | ChaCha20（x4 或标量） | 占比 | AES-CBC | 占比 |
|---|---|---:|---|---:|---:|---:|
| HND0 | 1316.8 MB/s | **9.5%** | 647.4 | 19.3% | 131.0 | **95.4%** |
| .65 | 1433.2 MB/s | 8.7% | 651.1 | 19.2% | 1809.9 | 6.9% |
| .74 | 1373.6 MB/s | 9.1% | 697.1 | 17.9% | 309.4 | 40.4% |

换算：`占用 = 125e6 / 实测 MB/s × 1e6`。HND0 的 AES 在 1G 下要 95% 的单核，
这还没有算 tunnel 本身的收发与 FEC；换句话说，**在 HND0 上用软件 AES 做 1G 加密
等于把两个 vCPU 中的一个整个吃掉**。ChaCha8 只占约 10%，FEC 与网络仍有空间。

## 5. 是否值得替代 AES

- **HND0：值得，且几乎是唯一可行解。** QEMU vCPU 不暴露 AES 指令，软件 AES 只有
  131 MB/s；ChaCha8x4 有 1.32 GB/s，10 倍差距。若未来在 HND0 上开加密，应选 ChaCha8
  （或 ChaCha12 作为保守档），而不是 AES-128-CBC。
- **.65：不值得。** ARMv8 crypto 的 AES 比 ChaCha8 快 26%，且 1G 下只占 7%；
  换算法只会增加代码量，不会省 CPU。
- **.74：值得。** 4.4 倍差距，且该机是「老 x86 候选」，没有 AVX2、AES 单元也慢。
- **安全边界**：ChaCha8 是减轮版本，密码学强度低于 ChaCha20，只适合「不需要很强安全」
  的混淆场景（用户原话）。如果只是防止运营商/中间设备识别流量，ChaCha8 足够；
  如果要求抗主动攻击，至少用 ChaCha20 或标准 AEAD。
- **MIPS 标量**：本次没有 MIPS 机可测。ChaCha 的标量实现只需 32 位加、异或、循环移位，
  没有查表、没有乘法，MIPS32r2 也能跑；AES 软件实现通常依赖 32 位 T-table 或位切片，
  在无 AES 指令的 MIPS 上更吃亏。按本轮 HND0（同样无 AES 指令、2.2 GHz）的 1.67 c/B
  推算，MIPS 若为 500 MHz 标量，ChaCha8 约 100–150 MB/s，仍可能高于软件 AES。

## 6. 局限与未跑项

- 只测了单核、4 MiB 热缓冲；没有测多核聚合、也没有测与 FEC/网络的叠加。
- ChaCha 实现是 C + 编译器向量化，不是手写汇编；Linux 内核的 AVX2/AVX-512 版会更快，
  因此本表对 HND0 是**保守下界**，对 .74 已接近编译器上限。
- AES 只测了 `openssl speed -evp aes-128-cbc -bytes 8192`，没有测 AES-GCM/CTR；
  CBC 有链式依赖，数字偏保守。
- .65 的 `cpu_mhz` 头行读的是 cpu0（A55 1.8 GHz），实际线程在 cpu4（2.304 GHz）；
  表中 cycles/byte 已按 2304 MHz 重算。
- 三机均未安装软件、未改服务/网络、未加载模块；`/tmp` 已清空。

## 7. 复现

```sh
cc -O3 -march=native -o crypto crypto.c    # 失败则去掉 -march=native
nice -n 19 taskset -c 0 ./crypto --bytes 4 --reps 9
# AES 锚点
nice -n 19 taskset -c 0 openssl speed -evp aes-128-cbc -bytes 8192 -seconds 1
```

原始日志：`logs/crypto-hnd0.log`、`logs/crypto-arm65.log`、`logs/crypto-x86-74.log`，
每份含三次完整运行与 AES 行。
