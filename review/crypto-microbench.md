# 微基准：ChaCha8/12/20（Bernstein 参考实现）与 AES-128-CBC

基线 `0db5166`，分支 `w-microbench`。实现来自 **Bernstein eSTREAM 参考源码**
（`chacha-ref.c version 20080118`，public domain），逐字节原样放在
`review/microbench/chacha-ref/`；测试向量与 `review/chacha8-reference.md`
（commit `040304a`）一致。原始日志 `review/microbench/logs/chacha-ref-*.log`。
2026-11-06 测得。

## 0. 结论

1. **参考实现下 ChaCha8 仍明显快于软件 AES，但倍数从手写版的 10× 降到 4.8×。**
   HND0 上 ChaCha8 最好 648.6 MB/s，AES-128-CBC 136.2 MB/s；1 Gbit/s（125 MB/s）
   只占单核 **19.3%**，AES 要占 **91.8%**。
2. **.74 上 ChaCha8 413.4 MB/s，AES 308.6 MB/s**，只快 1.34 倍；ChaCha20 只有
   193.0 MB/s，反而比 AES 慢。该机的 AES 数字偏低（5.8 c/B），但仍比 ChaCha20 快。
3. **.65 本轮跳过**：跑前 load 7.32、检测到其他 worker 进程，按避让规则不在
   RK3588 上跑。上一轮在 A76 上测的是另一份手写实现，不能与本表混用。
4. **两个测试向量逐字节通过**：TC1（128-bit 全零 key/IV，8 轮）与 256-bit 生成向量
   （key `00..1f`、IV `a0..a7`、counter 0）在三台机器的每次运行前都打印
   `SELFTEST tc1 PASS` / `SELFTEST v256 PASS`，失败即中止。

## 1. 实现与向量

| 项 | 内容 |
|---|---|
| 源码 | Bernstein `chacha8/ref`、`chacha12/ref`、`chacha20/ref`，`chacha-ref.c version 20080118` |
| 许可 | public domain（文件头保留） |
| 改动 | 只加了 `chacha-ref/ecrypt-sync.h`（类型/端序 shim，20 行），源码未改 |
| 链接 | 三个源文件分别用 `-DECRYPT_*=refN_*` 重命名符号，链进同一个 benchmark |
| TC1 | 128-bit 全零 key、8 字节全零 IV、8 轮，首块 `e28a5fa4...`，逐字节比对 |
| 256-bit | key `00..1f`、IV `a0..a7`、256-bit key、8 轮，首块 `9a1e949f...`，逐字节比对 |

自检在计时之前执行；任何一字节不符，程序打印 FAIL 并以非零状态退出，不产生性能数字。

## 2. 方法

- 4 MiB 输入、4 MiB 输出，`ECRYPT_encrypt_bytes` 一次调用；每次 15 个样本，
  程序打印中位数和最大值；整程序跑 3 遍，文档取 3 遍中的**最大值**（共享机上
  干扰只会让样本变慢，最大值最接近真实能力）。
- 计时用 `CLOCK_PROCESS_CPUTIME_ID`（进程 CPU 时间），抢占不会被算成吞吐下降。
- 频率：HND0 2200 MHz、.74 1792 MHz；`cycles/byte = 频率 / 字节率`，用 `--mhz`
  固定传入，避免 cpufreq 读数漂移。
- 全部 `nice -n 19` + `taskset` 单核；HND0 只跑 cpu0；不装包、不加载模块、不改网络。

## 3. 结果（4 MiB，15 样本 × 3 轮，单核）

### 3.1 HND0（QEMU vCPU，SSE2-only，2.2 GHz）

三次运行的单轮最大值波动较大（load 1.3–1.7，vCPU 有 steal），下表取三遍中的最好值：

| 实现 | MB/s | ns/byte | cycles/byte | 三次最大值 |
|---|---:|---:|---:|---|
| **ChaCha8** | **648.6** | 1.54 | 3.39 | 489.6 / 550.0 / 648.6 |
| ChaCha12 | 502.5 | 1.99 | 4.38 | 260.6 / 404.2 / 502.5 |
| ChaCha20 | 328.4 | 3.05 | 6.70 | 248.0 / 294.6 / 328.4 |
| AES-128-CBC | 136.2 | 7.34 | 16.2 | 124.9 / 136.2 |

### 3.2 .74（Athlon X4 760K，1.792 GHz）

三次运行几乎不抖（±0.5%），取中位：

| 实现 | MB/s | ns/byte | cycles/byte |
|---|---:|---:|---:|
| **ChaCha8** | **413.4** | 2.42 | 4.34 |
| ChaCha12 | 307.7 | 3.25 | 5.82 |
| ChaCha20 | 193.0 | 5.18 | 9.29 |
| AES-128-CBC | 308.6 | 3.24 | 5.81 |

### 3.3 .65（RK3588）

**跳过。** 跑前 `uptime` load 7.32（1 分钟）、`ps` 显示其他 worker 进程在跑；
按避让规则不在共享生产机上增加负载。日志中没有 .65 的 ChaCha 数字。

## 4. 1 Gbit/s（125 MB/s）下单核占用

| 机 | ChaCha8 | 占比 | ChaCha20 | 占比 | AES-CBC | 占比 |
|---|---|---:|---|---:|---:|---:|
| HND0 | 648.6 MB/s | **19.3%** | 328.4 | 38.1% | 136.2 | **91.8%** |
| .74 | 413.4 MB/s | 30.2% | 193.0 | 64.8% | 308.6 | 40.5% |
| .65 | — | — | — | — | — | — |

## 5. 是否值得替代 AES

- **HND0：值得。** 参考实现 ChaCha8 是 AES 的 4.8 倍；1G 下 AES 要 92% 的单核，
  ChaCha8 只要 19%。若将来在 HND0 上开加密，ChaCha8 是唯一不挤占 FEC/网络预算的档位。
- **.74：ChaCha8 仍快 1.34 倍**，但 ChaCha20 比 AES 慢；要用就用 8 轮，不要用 20 轮。
- **.65：没有本轮数据**，不能下结论；上一轮 A76 的 AES 硬件路径（1.81 GB/s）远快于
  任何软件 ChaCha，若该机要加密应优先用 ARMv8 crypto。
- **安全边界**：ChaCha8 是减轮版本，强度低于 ChaCha20；只适合「不需要很强安全」的
  混淆场景。需要抗主动攻击时用 ChaCha20 或标准 AEAD。
- **实现选择**：参考实现是标量 C89，未做 SIMD；HND0 上 3.4–6.7 c/B 是**下界**，
  Linux 内核/手写 SIMD 版本会更快。本轮数字适合做跨机对比，不适合当上限。

## 6. 局限与未跑项

- 参考实现逐字节循环、`x[16]` 数组落内存，未向量化；与手写 SIMD 版不可直接比。
- HND0 的 vCPU 有 steal，三次运行最好值与中位数差 20–40%；表里给的是最好值。
- .65 按避让规则跳过；没有在 A76 上重跑参考实现。
- AES 只测 `aes-128-cbc`（8192 B 块），未测 GCM/CTR；CBC 有链式依赖，偏保守。
- 三机均未安装软件、未改服务/网络、未加载模块；`/tmp` 已清空。

## 7. 复现

```sh
cd review/microbench
./run_chacha_ref.sh --bytes 4 --reps 15 --mhz 2200   # HND0
./run_chacha_ref.sh --bytes 4 --reps 15 --mhz 1792   # .74
# AES 锚点
nice -n 19 taskset -c 0 openssl speed -evp aes-128-cbc -bytes 8192 -seconds 1
```

脚本会先编译三个参考源文件（符号重命名），再跑三遍；日志见
`logs/chacha-ref-hnd0.log`、`logs/chacha-ref-x86-74.log`。
