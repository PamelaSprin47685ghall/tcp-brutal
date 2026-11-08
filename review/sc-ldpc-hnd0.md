# SC-LDPC 在 HND0 上的实测：恢复率与 app 吞吐

日期 2026-11-06。代码与构造见 `review/sc-ldpc-sim.md` 与
`review/fec-prototype/sc_ldpc/`。原始数据：`review/sc-ldpc-hnd0.csv`
（21 行，含 host 与 threads 列）。本文只回答一个问题：**在 HND0 上，
SC-LDPC 能不能达到 33.72 Mbit/s 的 app 目标。**

## 0. 结论

1. **不能。** HND0 单核 decode 是 **0.32–1.99 MB/s**（payload 口径），
   2 线程是 **0.55–1.96 MB/s**；app 需要 **4.215 MB/s**（33.72 Mbit/s），
   留 20% 余量是 5 MB/s。最好的观测点也只有目标的 47%。
2. **恢复率不是障碍。** p=0.30/0.40 的点 recovery 1.0000；p=0.50 的
   rate 0.30/0.25 在 block 256/L=16 是 0.9995–1.0000。也就是说，
   “降码率换 margin”在恢复率上成立，**卡住的是 HND0 的 CPU**。
3. **p=0.50 的最低 rate 0.25 仍不满足吞吐。** rate 0.25、block 256/L=16：
   recovery 0.9995，decode 0.455（单核）/0.555（双线程）；block 256/L=32：
   recovery 1.0000，decode 0.460。**不要宣称 p=0.50 可行。**
4. **2 线程不够。** 2 vCPU 上实测扩展 1.0–1.7 倍，最好 1.96 MB/s，
   仍不到 4.2。位并行（64 码字）才能过，但那是解码器重写，不在本轮范围。
5. **encode 也紧。** app 的 wire 需求是 `4.215/R` MB/s：rate 0.50 要
   8.4、rate 0.40 要 10.5、rate 0.30 要 14.1。HND0 单核 encode 是
   2.2–10.3 MB/s，多数点低于 wire 需求。
6. **本机 Ryzen 的数字不是验收。** 同一二进制在 Ryzen 9 7940HS 上
   decode 是 6.5–12 MB/s（rate 0.40/0.50），HND0 只有 0.9–2.0。
   比例 6–12 倍，和早先 HND0 快照的 1.6 MB/s 一致。

## 1. 机器与测量方法

| 项 | 值 |
|---|---|
| 主机 | `hnd0.kw92.cyou`，`root@45.11.1.62` |
| CPU | QEMU Virtual CPU version 2.5+，2 vCPU |
| 内核 | `7.0.0-42-generic` x86_64 |
| 编译器 | gcc 15.2.0（`-O3 -std=c99 -Wall -Wextra -Werror`） |
| 内存 | 955 MB（测时 used 385 MB） |
| 负载 | 1 分钟 load average 0.54 |
| 单线程 | `taskset -c 0 nice -n 10` |
| 双线程 | `taskset -c 0,1 nice -n 10`，`--threads 2` |

源码用 `scp` 放到 HND0 的 `/tmp/sc_ldpc_margin/`，在 HND0 本机编译。
每点 2000 trial，4 个码实例，symmetric 模型：payload 与 parity 的每个
码比特独立以 p 擦除；recovery 要求全部变量恢复**且值正确**（逐字节
比较，不是 systematic-only 上界）。吞吐是 payload MB/s：重复 encode /
decode 0.20 s，按 `n_data/8` 折算；decode 用固定擦除 mask。

HND0 是共享宿主上的 QEMU，吞吐有噪声。同一个 p=0.30/rate 0.50 点重复
5 次，decode 是 0.85、1.06、1.19、1.31、1.55、1.99 MB/s。表里给一次
观测值，结论按整个范围判断；**没有一次接近 4.2**。

## 2. 基础点（单线程）

表 1（HND0，2000 trial，symmetric，`--threads 1`）

| p | rate | block | L | recovery | ci99 | iter | enc MB/s | dec MB/s | mem B |
|---|---|---|---|---|---|---|---|---|---|
| 0.30 | 0.50 | 256 | 16 | 1.000000 | 0.001653 | 7.56 | 5.67 | 1.057 | 167088 |
| 0.40 | 0.40 | 256 | 16 | 1.000000 | 0.001653 | 9.39 | 7.11 | 0.911 | 231600 |
| 0.40 | 0.40 | 512 | 16 | 1.000000 | 0.001653 | 10.07 | 9.54 | 1.059 | 463024 |
| 0.50 | 0.30 | 256 | 16 | 1.000000 | 0.001653 | 11.39 | 4.00 | 0.345 | 339124 |
| 0.50 | 0.30 | 512 | 16 | 0.998000 | 0.003051 | 12.19 | 6.20 | 0.711 | 678028 |

对照 app 目标：payload 4.215 MB/s。**五个点的 decode 全部低于目标，
最好的是 1.059（目标的 25%）。** 恢复率 1.0 只说明码在 BEC 上工作，
不说明 HND0 跑得动。

## 3. p=0.50 的缺口：rate 0.25 与更大 L

表 2（HND0，p=0.50，单线程）

| rate | block | L | recovery | ci99 | iter | enc MB/s | dec MB/s | mem B |
|---|---|---|---|---|---|---|---|---|
| 0.25 | 256 | 16 | 0.999500 | 0.002093 | 10.31 | 3.36 | 0.455 | 425136 |
| 0.25 | 512 | 16 | 1.000000 | 0.001653 | 11.18 | 5.21 | 0.431 | 850096 |
| 0.25 | 256 | 32 | 1.000000 | 0.001653 | 10.51 | 3.01 | 0.460 | 871600 |
| 0.30 | 256 | 32 | 1.000000 | 0.001653 | 11.72 | 6.27 | 0.744 | 692708 |
| 0.30 | 512 | 32 | 0.999000 | 0.002455 | 12.56 | 2.65 | 0.322 | 1385308 |

恢复率在 rate 0.25 达到 0.9995–1.0000，但 decode 0.43–0.46；L=32 把
延迟翻倍（2.1 ms），吞吐没有改善。rate 0.30 的 256/L=32 是 0.744，
仍只有目标的 18%。**p=0.50 没有同时满足 0.999 与 4.2 MB/s 的点。**

## 4. 2 线程 decode

表 3（HND0，`--threads 1|2`，同一批次）

| p | rate | block | L | dec 1 线程 | dec 2 线程 | 扩展 |
|---|---|---|---|---|---|---|
| 0.30 | 0.50 | 256 | 16 | 1.988 | 1.964 | 0.99x |
| 0.40 | 0.40 | 512 | 16 | 0.755 | 1.277 | 1.69x |
| 0.50 | 0.30 | 256 | 16 | 0.738 | 1.022 | 1.38x |
| 0.50 | 0.25 | 256 | 16 | 0.515 | 0.555 | 1.08x |

2 线程用两个独立码字与两套 scratch，聚合吞吐按总 payload 折算。
QEMU 2 vCPU 的实际扩展远低于 2 倍，最好的点 1.96 MB/s，仍是目标的
47%。按这个上界，即使把线程加到 4 个（HND0 只有 2 vCPU，会超售），
也过不了 4.2。

## 5. 为什么本机数字不能当验收

同一份源码、同一组参数：

| 点 | Ryzen 9 7940HS dec | HND0 dec | 比例 |
|---|---|---|---|
| p=0.30, rate 0.50, b256 L16 | 11.81 | 1.06–1.99 | 6–11x |
| p=0.40, rate 0.40, b512 L16 | 6.48 | 1.06 | 6x |
| p=0.50, rate 0.30, b256 L16 | 4.18 | 0.35–1.02 | 4–12x |

HND0 是 QEMU 虚拟 CPU，单核性能约为本机的 1/6–1/12；早先 HND0 快照的
1.6 MB/s 也落在这个比例内。**G4 的吞吐验收只能看 HND0 列。**

## 6. 判定

按“只认 HND0、decode ≥4.215 MB/s（5 MB/s 留余量）”的口径：

- **SC-LDPC 在 HND0 上没有可行工作点。** 恢复率可以靠降码率做到
  0.999–1.0，但 decode 最高 1.99 MB/s，encode 多数点也低于 wire 需求。
- **p=0.50 的最低测试 rate 是 0.25**：recovery 0.9995–1.0000，
  decode 0.43–0.56 MB/s。不宣称可行。
- 要继续只有两条路：位并行（64 码字，需要重写 peeling 的唯一性判断）
  或多线程 + 更高主频的宿主；2 vCPU 的 HND0 上多线程不够。
- 因此本轮不应按“SC-LDPC margin 工作点”修订 G4；应在 RLNC/grouped
  降级（B）与 SC-LDPC 硬门阻塞（C）之间由 Main 决定。

## 7. 复现

```sh
# 本机交叉检查
review/fec-prototype/sc_ldpc/build.sh
./review/fec-prototype/sc_ldpc/sc_ldpc_bench --block 256 --L 16 \
  --rate 0.50 --p 0.30 --trials 2000 --model symmetric --codes 4 \
  --seed 1 --threads 1 --no-header

# HND0
scp review/fec-prototype/sc_ldpc/{sc_ldpc.c,sc_ldpc.h,sc_ldpc_bench.c,build.sh} \
  root@45.11.1.62:/tmp/sc_ldpc_margin/
ssh root@45.11.1.62 'cd /tmp/sc_ldpc_margin && sh build.sh && \
  taskset -c 0 nice -n 10 ./sc_ldpc_bench --block 256 --L 16 \
  --rate 0.50 --p 0.30 --trials 2000 --model symmetric --codes 4 \
  --seed 1 --threads 1 --no-header'
```

原始 CSV：`review/sc-ldpc-hnd0.csv`。本机低速率矩阵：
`review/sc-ldpc-sim.csv` 的 `series=margin|probe` 行。
