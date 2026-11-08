# SC-LDPC 标量解码器的结构优化（非 SIMD）

日期 2026-11-06。代码 `review/fec-prototype/sc_ldpc/`。数据
`review/sc-ldpc-algo.csv`。目标：HND0（QEMU 2 vCPU，qemu64，只有
SSE2）decode ≥4.215 MB/s（app 33.72 Mbit/s 的 payload 口径）。
本文只记非 SIMD 的结构改动；64 码字位并行在 `w-sclc-simd` 分支。

## 0. 结论

1. **标量结构优化把单核 decode 从 12.9 提到 19.2 MB/s**（本机
   Ryzen 9 7940HS，同批次 5 次中位，+48%）。HND0 2 线程从 2.67
   提到 3.69（fast 版，20 轮同批次中位，+38%）；opt 版见 §3。
2. **HND0 仍未稳定过 4.215。** 宿主是共享 QEMU，同一二进制 20 次
   测量的中位随宿主负载在 2.7–4.1 之间漂移；最好一批 4.12，另一批
   3.30。标量路径把差距从 2.2x 缩到 1.0–1.1x，不能单独保证过线。
   同一码字的 2 线程合作解码（check 分区 + inbox 同步）没在时限内
   修完跨区计数 bug，未给出可用结果；2 线程只用独立码字方案。
3. **bit-exact 成立。** 9 组构造（rate 0.50/0.60/0.70/0.40/0.25、
   acc/native/causal、`target_data_only` 0/1、n=131072 的 int32
   回退路径）各 40 mask，`out[]` 的 FNV64 与基线逐行一致；harness
   是 `dump.c`，它把实现 .c 直接 include 进来。
4. **有效候选**：固定度/固定 stride 邻接（uint16）、XOR 累加器、
   payload-only 写回、`known[]` 按需生成、FIFO 连续处理、
   `--bench-sec` 长窗口 + 线程绑核测量。
5. **失败候选**：窗口 4（recovery 0.993–0.9975）、block 128
   （0.9965–0.9985）、L=12（吞吐相近）、SoA ucnt/xorv（-10%）、
   按 check 重排初始化（-11%）、单独 early stop（-2%）、
   PGO（本机 +16%，HND0 反向）。
6. **与 bit-slicing 叠加**：改动都在标量控制流、邻接布局和写回上，
   bit-sliced 解码器可以照搬。它不是 4.215 的独立解，而是 SIMD
   路径的乘数。

## 1. 方法

- 测量：`--bench-sec 1.0`（固定 1 秒窗口，而不是按单次 decode 估
  reps），`taskset` 绑核，base/new 交替各 5 次取中位。HND0 用
  `taskset -c 0,1 nice -n 10`，20 轮交替。
- 本机噪声主要来自 CPU powersave 频率和同机其它 worker；交替测量
  可以把漂移摊到两边。HND0 是共享 QEMU，宿主负载决定绝对值，
  所以只看同批次相对提升。
- 恢复率口径不变：payload 全部恢复且值正确。`symmetric` 模型擦除
  payload 与 parity，`systematic` 只擦 payload。

## 2. 候选与数字（本机，block 256 / L 16，5 次中位）

| 配置 | base dec | opt dec | 提升 | recovery |
|---|---|---|---|---|
| rate 0.50 / p 0.30 / sym | 12.95 | 19.17 | +48% | 1.000000 |
| rate 0.50 / p 0.40 / sym | 10.68 | 15.74 | +47% | 1.000000 |
| rate 0.60 / p 0.30 / sym | 14.23 | 21.58 | +52% | 1.000000 |
| rate 0.70 / p 0.20 / sym | 19.87 | 27.72 | +39% | 1.000000 |
| rate 0.50 / p 0.30 / sys | 12.81 | 18.72 | +46% | 1.000000 |
| rate 0.60 / p 0.30 / sys | 14.58 | 21.96 | +51% | 1.000000 |
| rate 0.70 / p 0.20 / sys | 19.88 | 27.96 | +41% | 1.000000 |

HND0 同批次三方（rate 0.50 / p 0.30 / sym，2 线程，20 轮中位）：

| 版本 | t2 中位 | 相对 base |
|---|---|---|
| base（原 `sc_decode`） | 2.672 | — |
| fast（固定度 + uint16 + XOR 累加器 + early stop + known 写回） | 3.691 | +38% |
| lazy（只维护 ucnt，出队算 XOR，不写 known） | 3.206 | +20% |

**HND0 上 XOR 累加器优于 lazy XOR。** 本机相反（lazy 18.9 对
fast 17.4）。原因是 QEMU 的随机访问延迟高：lazy 在 check 出队时
集中读 7 个变量，fast 把 XOR 分摊到变量更新的顺序访问上。所以
HND0 走 fast 路线，lazy 只作为本机备选。

### 2.1 各部分贡献（本机，rate 0.50 / p 0.30 / sym）

用编译开关逐项回退 fast 版，5 次中位：

| 变体 | dec MB/s | 结论 |
|---|---|---|
| fast 全量 | 17.17 | — |
| 去掉 early stop | 17.54 | early stop 反而 -2%，删 |
| 写回全量（不裁剪 parity） | 15.70 | 裁剪 +9.4% |
| 变量侧用 CSR `voff/vadj` | 15.28 | 固定度 +12.4% |
| check 侧用 CSR `coff/cadj` | 16.21 | 固定 stride +5.9% |
| 初始化遍历所有变量 | 语义错（高字节计数） | 已知变量遍历保留 |

opt = fast - early stop - `known[]` 写回，本机 18.8–19.2。

## 3. HND0 绝对数字

**验收口径只看这一节。** HND0 是共享宿主上的 QEMU，绝对值随宿主
负载漂移；同一二进制两次 20 轮中位可以差 20% 以上。表中给同批次
base 与 new 的中位，不把不同批次拼在一起。

| 批次 | base t2 中位 | new t2 中位 | 说明 |
|---|---|---|---|
| fast，20 轮 | 1.881 | 4.118 | 宿主空闲；11/20 样本 ≥4.2 |
| fast，20 轮（另一批） | 1.881 | 2.641 | 宿主负载高 |
| 三方同批次，20 轮 | 2.672 | fast 3.691 / lazy 3.206 | 相对提升可信 |
| opt，20 轮 | 1.747 | 2.883 | 宿主负载高；后 6 轮 opt 3.2–4.5 |

**判定**：opt 的同批次提升是 +65%（1.747 → 2.883）；fast 是
+38%（2.672 → 3.691）。绝对值随宿主负载：宿主空闲批次 opt 单次
3.2–4.5、fast 中位 4.12；宿主忙批次中位 2.9。**没有一批能稳定
≥4.215**，单次最高 4.53。低值来自宿主争用，不是解码器本身。
要稳定过线，必须叠加位并行（sclc-simd）或换非共享宿主。

## 4. 失败候选

| 候选 | 数字 | 失败原因 |
|---|---|---|
| payload 窗口 4 | 本机 dec 14.0（+24%），recovery 0.995（0.50/p0.30）、0.993（0.60/p0.30）、0.9975（0.70/p0.20） | 恢复率低于 0.999 |
| block 128 / L 16 | dec 13.0，recovery 0.9965–0.9985 | 小 lifting 的有限长度损失 |
| L=12 | dec 12.7–17.9，recovery 0.9995–1.0 | 吞吐与 L=16 同档，无净收益 |
| SoA `ucnt[]`+`xorv[]` | dec 11.45（base 12.7） | 两次 RMW 抵消了更密的计数数组 |
| 初始化按 check 重排 | dec 10.9（base 12.3） | 多读一遍 val，写连续收益不够 |
| 单独 early stop | 17.54 对 17.17 | 热路径多一个计数分支 |
| PGO | 本机 14.4 对 12.4；HND0 1.73 对 2.11 | HND0 上反向；不采用 |
| 2 线程独立码字（base） | HND0 扩展 1.0–1.7x | 已知失败；fast 改变单核后重测见 §3 |
| 同码字 2 线程合作 | 未完成 | check 分区 + 跨区 inbox，修复初始化屏障后仍有计数停滞；未在时限内给出可用结果 |

## 5. 与 bit-slicing 的叠加

bit-sliced 解码器（`w-sclc-simd`）换的是数据布局，不换 peeling
语义。可以照搬的部分：

1. **固定度**：payload 5、parity 2，变量侧不再查 `voff`。
2. **payload-only 写回**：`target_data_only` 下只写 `out[0..n_data)`；
   `known[]` 按需生成。
3. **XOR 累加器**：每 check 一个 `xorv/ucnt` 位平面，变量解决时
   顺序更新；比出队时集中 gather 更适合 QEMU。
4. **测量**：`--bench-sec` 长窗口、绑核、同批次交替，否则 HND0
   的数字没有可比性。

## 6. 复现

```sh
review/fec-prototype/sc_ldpc/build.sh
./review/fec-prototype/sc_ldpc/sc_ldpc_bench --block 256 --L 16 \
  --rate 0.50 --p 0.30 --trials 2000 --model symmetric --codes 4 \
  --seed 1 --threads 1 --bench-sec 1.0 --no-header
```

HND0：

```sh
scp review/fec-prototype/sc_ldpc/{sc_ldpc.c,sc_ldpc.h,sc_ldpc_bench.c} \
  root@45.11.1.62:/tmp/sc_ldpc_algo/
ssh root@45.11.1.62 'cd /tmp/sc_ldpc_algo && \
  cc -O3 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c \
  -lm -pthread -o bench_new && \
  taskset -c 0,1 nice -n 10 ./bench_new --block 256 --L 16 --rate 0.50 \
  --p 0.30 --trials 100 --model symmetric --codes 4 --seed 1 \
  --threads 2 --bench-sec 1.0 --no-header'
```

bit-exact dump：`dump.c` 把实现 .c 直接 include 进来，编译两份
（基线 `orig/sc_ldpc.c` 与候选 `sc_ldpc.c`），逐行 diff：

```sh
cc -O2 -std=gnu99 -w -DSC_IMPL='"orig/sc_ldpc.c"' dump.c -lm -o d_base
cc -O2 -std=gnu99 -w -DSC_IMPL='"sc_ldpc.c"'      dump.c -lm -o d_new
for args in "256 16 50 30 2" "256 16 50 40 2" "256 16 60 30 2" \
            "256 16 70 20 2" "256 16 40 30 2" "256 16 25 30 2" \
            "256 16 50 30 0" "256 16 50 30 1"; do
  ./d_base $args > /tmp/b.txt; ./d_new $args > /tmp/n.txt
  diff -q /tmp/b.txt /tmp/n.txt >/dev/null && echo "MATCH $args" || echo "DIFF $args"
done
./d_base 8192 16 50 30 2 8 > /tmp/b.txt; ./d_new 8192 16 50 30 2 8 > /tmp/n.txt
diff -q /tmp/b.txt /tmp/n.txt
```
