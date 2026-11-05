# 微基准：ChaCha8/12/20（Bernstein 参考实现）与 AES-128-CBC

基线 `0db5166`，分支 `w-microbench`。实现来自 **Bernstein eSTREAM 参考源码**
（`chacha-ref.c version 20080118`，public domain），逐字节原样放在
`review/microbench/chacha-ref/`；测试向量与 `review/chacha8-reference.md`
（commit `040304a`）一致。原始日志 `review/microbench/logs/chacha-ref-*.log`。
2026-11-06 测得。

§8 是 crypto-v6 移植的跨机读数，§9 是 w-crypto 33238ca 优化版的 HND0 复核。

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
5. **优化版（w-crypto 33238ca）在 HND0 的 8 KiB 记录上达到 ≤25%。** 8 遍干净测量：
   最好 **635.0 MB/s**（3.46 c/B）→ 1G 单核 **19.7%**，中位 505.6 MB/s → **24.7%**；
   旧 crypto-v6 是 458.9 MB/s / 27.2%。64 KiB 两口径 19.5%/21.6%；1 KiB 中位
   25.4%，略超线。KAT 55/55。详见 §9。

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

## 8. crypto-v6 实现的 KAT 与吞吐（w-fec 0615b3d）

fec-core2/crypto-v6 的 host 移植（`review/fec-prototype/crypto/`，C99，64-bit counter +
8-byte nonce）已提交。构建 `cc -std=c99 -O2 -Wall -Wextra -Werror`（KAT）与
`-O3`（bench），KAT 16 项全部 PASS（含 TC1、256-bit 向量、RFC 8439、counter carry、
streaming、in-place、counter wrap、invalid args、wipe）。bench 自带 TC1 自检，
失败拒绝出数。日志：`logs/chacha8-impl-{hnd0,x86-74}.log`。

### HND0（2.2 GHz，SSE2；`--seconds 0.3 --mhz 2200`）

| rounds | 1 KiB MB/s (c/B) | 8 KiB MB/s (c/B) | 64 KiB MB/s (c/B) |
|---:|---:|---:|---:|
| 8 | 373.4 (5.89) | **458.9 (4.79)** | 419.0 (5.25) |
| 12 | 199.1 (11.05) | 300.9 (7.31) | 343.7 (6.40) |
| 20 | 203.2 (10.83) | 270.3 (8.14) | 275.5 (7.99) |

### .74（1.792 GHz；`--seconds 0.3 --mhz 1792`）

| rounds | 1 KiB MB/s (c/B) | 8 KiB MB/s (c/B) | 64 KiB MB/s (c/B) |
|---:|---:|---:|---:|
| 8 | 210.1 (8.53) | **217.5 (8.24)** | 217.3 (8.25) |
| 12 | 183.8 (9.75) | 181.7 (9.86) | 179.7 (9.97) |
| 20 | 132.0 (13.58) | 135.1 (13.26) | 140.3 (12.78) |

### 1 Gbit/s 单核占比

| 机 | ChaCha8 | 占比 | ChaCha20 | 占比 |
|---|---|---:|---|---:|
| HND0 | 458.9 MB/s | **27.2%** | 275.5 | 45.4% |
| .74 | 217.5 MB/s | 57.5% | 140.3 | 89.1% |
| .65 | — | — | — | — |


### 8.1 .65（RK3588 A55 1.8 GHz，w-crypto 33238ca）

按避让规则在 load 0.15 的窗口、`flock /tmp/tbtun-rk3588.lock` + `nice -n 19 taskset -c 0`
上跑；KAT 全绿（含 per-direction nonce 与 HELLO gap 用例），三次 bench 各 `timeout` 包住。
日志 `logs/chacha8-v6-arm65.log`。

| rounds | 1 KiB MB/s (c/B) | 8 KiB MB/s (c/B) | 64 KiB MB/s (c/B) | 1G 单核 |
|---:|---:|---:|---:|---:|
| 8 | 294.6 (6.11) | **314.0 (5.73)** | 313.3 (5.75) | **39.8–42.4%** |
| 12 | 238.0 (7.56) | 250.5 (7.19) | 249.9 (7.20) | 49.9–52.5% |
| 20 | 165.2 (10.90) | 171.2 (10.51) | 170.9 (10.53) | 73.0–75.7% |

### 8.2 .74（Athlon 1.792 GHz）

**本轮排除，没有可用数字。** 两次尝试时该机 load 分别为 82.6 和 16.8–43.2，
`bun`/`tq_tp5_dw_alloc`/`python3` 在跑；在 load 82 时测到 ChaCha8 200.0/179.3/146.7 MB/s
（1G 62.5–85.2%），只能说明机器被占满，不能代表 33238ca。等负载回落到 <2 再补测，
**不要引用这一行**。

### 8.3 HND0

HND0 的权威数字见 §9（microbench2 的 8 遍干净复核，w-crypto 33238ca）：
ChaCha8 8 KiB max 635.0 MB/s（3.46 c/B，1G 单核 19.7%），中位 505.6 MB/s（24.7%）。
我早前与 microbench2 并发时测到的一组数字已作废，不在此引用。

### 读数

- 同一台 HND0 上，crypto-v6 实现比 Bernstein 参考慢约 30%（458.9 vs 648.6 MB/s）；
  .74 上慢约 47%（217.5 vs 413.4）。差异来自实现结构（64-bit counter、in-place、
  无展开）与 bench 口径（64 KiB 热缓存、0.3 s），不是测量错误；两套数字都保留。
- ChaCha8 仍是 HND0 上最省的软件档：1G 下 27% 单核，ChaCha20 要 45%。
- .74 的 ChaCha8 占 57%，ChaCha20 占 89%：这台 1.8 GHz 老 x86 上只有 8 轮才划算。
- **.65 仍然跳过**：跑前 load 5.25、`coreyolo` 占 341% CPU（cpu7），按避让规则不跑。

## 9. 优化版 ChaCha8（w-crypto 33238ca）HND0 复核

2026-11-06 在 HND0 上复核 `w-crypto` 的优化版。源码用
`git archive 33238ca -- Makefile tbtun host` 取出；`tbt_crypto.c`、`tbt_crypto.h`、
`bench_crypto.c`、`test_crypto.c` 的 sha256 与本地 `git show 33238ca:...` 逐字节
一致。`tbt_crypto.c` 自 43bbbff 起只加了 `tbt_crypto_pair_init/wipe`，热路径未变。
原始日志 `logs/chacha8-33238ca-hnd0.log`。

### 9.1 KAT

`cc -std=gnu99 -O2 -Wall -Wextra -Werror` 构建 `host/test_crypto`，
`nice -n 19 taskset -c 0` 运行：**55/55 PASS**，`all checks passed`。覆盖 TC1、
256-bit 生成向量、RFC 8439、counter carry/wrap、streaming、xor_at、in-place、
invalid args、wipe、body counter `(seq<<8)|(parity<<7)`、HELLO v6 字段/截断/off、
key 解析、per-direction nonce、pair init/wipe、session 双向。失败即非零退出，
不出性能数字。

### 9.2 方法

- `cc -std=gnu99 -O3 -Wall -Wextra -Werror` 构建 `host/bench_crypto`；
  1/8/64 KiB 热缓冲、in-place XOR、每次调用 counter 0；
  `--seconds 0.3 --mhz 2200`，每格是程序内 3 个样本的中位数。
- `nice -n 19 taskset -c 0`，完整矩阵跑 8 遍；下表跨 8 遍取最大值与中位数。
- HND0 是 KVM 客户机（QEMU Virtual CPU 2.5+，2 vCPU，SSE2-only，2200 MHz 标签）。
  8 遍期间 `ps` 没有竞争进程，cpu0 的 steal 计数从 7272 只走到 7273（客户机侧
  steal ≈ 0），但 MB/s 仍有约 2 倍波动；这是宿主机共享物理核/频率造成的，不是
  客户机调度。8 KiB 的最大值在三遍里是 634.7/634.9/635.0，可重复，所以用最大值
  作为“无争用能力”，中位数作为“典型争用值”。
- 07:02–07:09 的 75dcf9f/43bbbff 试跑与另一 worker 的 `bench_old`/`bench_new`
  （cpu0）和内核编译（cpu1）重叠，数字作废；§8.3 已记录。

### 9.3 结果（MB/s，max / 8 遍中位）

| rounds | 1 KiB | 8 KiB | 64 KiB |
|---:|---:|---:|---:|
| 8 | 609.0 / 492.9 | **635.0 / 505.6** | 642.0 / 577.7 |
| 12 | 474.4 / 394.5 | 498.7 / 425.9 | 497.9 / 443.6 |
| 20 | 320.5 / 293.0 | 320.5 / 309.5 | 320.6 / 258.6 |

8 轮最大值的 c/B（2200 MHz 标签折算）：1 KiB 3.61、8 KiB 3.46、64 KiB 3.43；
12 轮 4.64/4.41/4.42；20 轮 6.86。标签频率不等于宿主机实际频率，所以 c/B 只作
标尺，1G 占比与频率无关。8 轮 8 KiB 的 8 遍范围是 311.1–635.0 MB/s。

### 9.4 1 Gbit/s（125 MB/s）单核占比

| 机 | 口径 | ChaCha8 8 KiB | 占比 | ChaCha8 64 KiB | 占比 |
|---|---|---:|---:|---:|---:|
| HND0 | 8 遍最大 | 635.0 MB/s | **19.7%** | 642.0 | 19.5% |
| HND0 | 8 遍中位 | 505.6 MB/s | **24.7%** | 577.7 | 21.6% |
| HND0 | 旧 crypto-v6（0615b3d，单遍） | 458.9 MB/s | 27.2% | 419.0 | 29.8% |

**8 KiB 记录口径：是，≤25%。** 最好 19.7%，8 遍中位 24.7%（只剩 0.3 个百分点
余量）；64 KiB 两个口径都低于线；1 KiB 中位 492.9 MB/s = 25.4%，略高于线，
最大值 20.5%。旧版 458.9/27.2% → 新版 635.0/19.7%：吞吐 +38%，单核占比 −7.5
个百分点；按中位算是 +10% / −2.5 个百分点。

### 9.5 读数

- 优化版与 Bernstein 参考实现（4 MiB，648.6 MB/s，19.3%）只差 2%。两者口径
  不同（4 MiB vs 8 KiB 热缓冲、每次调用 init+wipe），只能作量级对照。
- 优化点（显式展开 4 个 double round、8 字节 XOR、64-bit counter）在 HND0 的
  SSE2-only vCPU 上有效：同机同 bench 口径，旧 crypto-v6 最好 458.9，新 635.0。
- HND0 的宿主机争用无法从客户机内消除：8 KiB 的 max 比中位高 26%，1 KiB 高
  24%，64 KiB 高 11%。只看单遍容易得到 300–450 MB/s 的假低值；报告“是否 ≤25%”
  时必须同时给最大值与中位。
- 与 Ryzen 的 1207–1221 MB/s / 4.01–4.06 c/B / 10.2–10.4% 比：那是同一份代码在
  安静主机上的数字；HND0 的 3.46 c/B 看似更低，是因为 2200 MHz 只是标签频率，
  不能跨机比 c/B，只能比 1G 占比。
- .65 的 33238ca 数字见 §8.1（07:11 的安静窗口，load 0.15，flock + taskset cpu0）。
  microbench2 没有在 .65 上跑；07:12 的避让检查（load 0.71、mediamtx/ffmpeg、
  3 个登录会话）记在 `logs/chacha8-33238ca-hnd0.log`。本节只复核 HND0。
- 全程未装包、未加载模块、未改网络/服务；HND0 的 `/tmp` 临时目录已删，本地未产生
  临时文件。
