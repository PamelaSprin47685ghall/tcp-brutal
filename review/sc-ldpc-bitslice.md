# SC-LDPC 位并行解码器：64 码字 bit-slicing 的实测

日期 2026-11-06。代码：`review/fec-prototype/sc_ldpc/`
（`sc_ldpc_bs.c`、`sc_ldpc_bs.h`、`sc_ldpc_bs_test.c`、`sc_ldpc_bs_bench.c`）。
本文回答一个问题：**在 HND0（qemu64，只有 SSE2）上，64 码字 bit-slicing
能不能把 decode 从 0.32–1.99 MB/s 提到 app 目标 4.215 MB/s。**
本地数据：`review/fec-prototype/sc_ldpc/local-bs.csv`；HND0 数据见第 6 节。

## 0. 结论

1. **bit-sliced 解码器与标量解码器逐位一致。** `sc_ldpc_bs_test` 覆盖
   小码与生产尺寸、sys/sym 两种信道、batch 64/128/256/512、rate
   0.50/0.60/0.70、check degree ≤16；输出字节与 unresolved 计数全部
   相等。bench 对每个码字再逐字节比较，`unres_mismatch=0`、
   `bit_mismatch=0`。
2. **i.i.d. 比特擦除（原型口径）的加速是 2.4–5.5 倍，不是 8–16 倍。**
   本机同批 scalar 对 bit-sliced（batch 1024，5 次中位）：
   rate 0.50 5.00→27.71 MB/s（5.48x）、rate 0.60 5.94→14.81（2.49x）、
   rate 0.70 8.82→23.76（2.69x）。原因是 64 个码字的擦除图样互相独立，
   peeling 调度不同步：一个 64 位 mask 事件平均只携带约 2 个有效比特，
   稀疏因子约 30。批越大，调度并集增长越慢，所以 batch 512/1024 比
   batch 64 好 2–3 倍，但仍消不掉稀疏。
3. **相关擦除（share 级擦除，app mode3 的真实图样）的加速是 12.5–12.8 倍，
   绝对吞吐 158–260 MB/s。** 64 个码字共享同一个擦除 mask 时，peeling
   调度只算一次，每个 mask 事件都是满的；`--corr` 模式实测
   rate 0.50 158.4、0.60 179.9、0.70 260.3 MB/s（同批 scalar
   12.3/14.3/20.4）。**这是 app 应当采用的映射**：一个 share 的 65536
   个 bit-plane 看到完全相同的缺失 share 集合。
4. **HND0 验收见第 6 节。** rate 0.50/p0.30 是 1t 8.47、2t 11.20；
   rate 0.30/p0.50 是 1t 4.06、2t 5.80；rate 0.40/p0.40 用 block512
   L16 过恢复率，吞吐待 T5.2 后复核。本机是 Ryzen 9 7940HS，数字
   不能直接当 HND0；HND0 只认同批次 A/B。
5. **瓶颈是调度稀疏，不是 XOR 吞吐，也不是 AVX。** 位平面计数器
   （3/4/5 plane）与 wave 批处理已把每事件成本压到约 12–16 个 64 位 op；
   剩余成本是独立码字调度并集带来的事件数。相关擦除下事件数降到
   单码字水平，吞吐随之上一个数量级。

## 1. 设计

- 一个 batch 放 64×G 个码字；bit j 属于码字 j（每组 64 个）。
- 每个 check 维护：未知数位平面计数器（degree ≤7 用 3 plane，≤15 用
  4 plane，≤31 用 5 plane）、已知值 XOR、`count==1` mask。
- 变量被 check 解出时，`res = act & unk[v]`，值 = check 的已知值 XOR；
  同一 wave 内多个 check 解出的同一变量先累积成一个 mask，再向它的
  所有 check 传播一次。这就是标量解码器的 wave 语义，只是 64 路并行。
- 输入/输出是 packed、codeword-major、LSB-first 的位矩阵；64×64 位
  转置桥接变量主序状态。转置是 Hacker's Delight 的原地算法。
- 只用标量 `uint64_t`。HND0 是 qemu64，只有 SSE2，没有 AVX/AVX2/
  AVX-512；G 个 group 的内层循环由编译器用 SSE2 展开，不依赖 AVX。
- 已并入 sclc-algo 的可迁移项：每 check 的 XOR 累加器（本来就有）、
  `target_data_only` 下只转置 `out[0..n_data)`、ACC 固定度变量邻接
  （payload 5、parity 2/1，构造时校验，rate<0.50 的 extra edge 回退
  到 `voff`）。`known[]` 按需生成不适用：本解码器不维护 known 数组，
  unresolved 直接从 `unk` 位平面算。
- 预分配所有 scratch；每次 decode 只 memset 和写输出，不 malloc。

## 2. 正确性

`sc_ldpc_bs_test` 对每个配置构造一个码，随机生成 batch 个码字与擦除
mask，标量 `sc_decode` 逐码字解，bit-sliced 整批解，要求：

- 每个码字的 `out[]` 逐字节相等；
- 每个码字的 unresolved 计数相等；
- 全零/随机码字、sys/sym 两种信道都覆盖。

覆盖：block 16/32/64/256/512，L 2/4/8/16，rate 0.50/0.60/0.70，
p 0.001–0.5，batch 64/128/256/512，dc_max 7–16。结果 `RESULT: PASS`。
bench 的 recovery 路径再做一次逐码字比较，所有点
`unres_mismatch=0`、`bit_mismatch=0`。

## 3. 本机 i.i.d. 吞吐（Ryzen 9 7940HS，单线程）

`--bench-sec 1.0`，每点 3 次中位；scalar 是同一 batch 的标量解码
（不是原 bench 的固定 mask 热缓存口径），payload MB/s。

| block | L | rate | p | batch | scalar | sliced | 加速 |
|---|---|---|---|---|---|---|---|
| 256 | 16 | 0.50 | 0.30 | 1024 | 5.15 | 36.69 | 7.19x |
| 256 | 16 | 0.60 | 0.30 | 1024 | 6.02 | 19.59 | 3.25x |
| 256 | 16 | 0.70 | 0.20 | 1024 | 9.05 | 30.02 | 3.32x |
| 512 | 16 | 0.50 | 0.30 | 512 | 4.97 | 25.59 | 5.17x |

（`local-bs.csv` 的 5 次 0.2 s 窗口在较重负载下是 27.7/14.8/23.8，
是本表的保守下界；本机共享，单次样本可漂 1.5–2x。batch 1024 对
block 512 因工作集超过 L2 反而略慢，故 block 512 用 batch 512。
HND0 验收只看第 6 节同批 A/B。）

## 4. 相关擦除（share 级，app mode3）

`--corr`：batch 内所有码字共享同一个擦除 mask，payload 仍各不相同。
这正是 share 级擦除在 bit-plane 视角下的图样：一个 share 丢失，
该 share 的 65536 个 bit-plane 在同一位置同时丢失。

| block | L | rate | p | batch | scalar | sliced | 加速 |
|---|---|---|---|---|---|---|---|
| 256 | 16 | 0.50 | 0.30 | 1024 | 12.32 | 158.35 | 12.57x |
| 256 | 16 | 0.60 | 0.30 | 1024 | 14.33 | 179.87 | 12.49x |
| 256 | 16 | 0.70 | 0.20 | 1024 | 20.35 | 260.25 | 12.77x |

这里 scalar 也比 i.i.d. 快，因为所有码字共用 mask，缓存/分支预测更友好；
bit-sliced 仍快 12.5 倍，绝对吞吐 158–260 MB/s。app 的 payload 口径与
原型一致（一个 bit-plane 的 n_data 比特就是 n_data 个 share 的一位；
65536 个 bit-plane 合起来是 n_data 个 8192 B share），所以这些 MB/s
可直接与 4.215 比较。

## 5. 瓶颈

- 本机 gprof：标量 `sc_decode` 占 88.6%，10.18 cycles/edge，L1 miss
  7894/decode，LLC miss 约 3.6/decode；热点是随机间接寻址，不是 XOR。
- bit-sliced 的 wave 循环占 80–90%；init/转置/输出合计 10–20%。
- i.i.d. 下每个 mask 事件平均约 2 个有效比特（batch 1024），所以
  64 位并行的理论 64x 被稀疏因子吃掉，只剩 2.4–5.5x。
- 相关擦除下事件携带 64 个有效比特，吞吐 158–260 MB/s，说明限制
  确实是调度稀疏，不是位宽或内存带宽。
- 3/4/5-plane 特化、去掉 pending 分支、touched flag 三项合计把本机
  rate 0.50 从 22.5 提到 27.7 MB/s（+23%）。

## 6. HND0 实测

以下数字来自 sclc-bench 在 HND0 上的同批 A/B（候选 8c64ccf，base 为
原版标量 bench，block 256/L16，symmetric，taskset 0/1，nice 10）。
T5.2 256 WAN 随后占用了 HND0，2 线程 5 轮复核被暂停；表里的 2t 是
暂停前的观测值，不是最终 5 轮中位。

| p | rate | 1t MB/s | 2t MB/s | base 1t/2t | recovery（强证据） |
|---|---|---|---|---|---|
| 0.30 | 0.50 | 8.47 | 11.20 | 1.65 / 2.59 | 0.999821（1M 码字，Wilson99 [0.999783, 0.999852]） |
| 0.30 | 0.50 (b512) | — | 10.58 | — | 同上 |
| 0.40 | 0.40 | 5.77 | 5.91 | — | block256 L16 0.998988（160k，未过）；**block512 L16 0.999168（1M，[0.999090, 0.999239]）** |
| 0.50 | 0.30 | 4.06 | 5.80 | — | 0.999862（160k，[0.999763, 0.999920]） |
| 0.50 | 0.25 | 4.01 | 4.00 | — | 0.999950（160k），但 2t 4.00 < 4.215，改用 r0.30 |

要点：

1. **rate 0.50 / p0.30：1t 8.47、2t 11.20，过 4.215；恢复率 1M 码字
   下界 0.999783 > 0.999。** 这是最稳的点。
2. **rate 0.40 / p0.40：block256 L16 的恢复率 0.998988 差 0.000012；
   block512 L16 用 1M 码字测得 0.999168，Wilson 下界 0.999090 > 0.999。**
   block512 的 bit-sliced 本机吞吐 17.6–20.1 MB/s（batch 512），按
   rate 0.50 的 HND0/本机比例外推 1t 约 4.4、2t 约 5.5–6.5；需 T5.2
   结束后在 HND0 复核。
3. **rate 0.35 / p0.40 不是解**：160k 码字 0.998481，seed 2 单点
   0.9888，比 rate 0.40 更差；不要用。
4. **rate 0.30 / p0.50：2t 5.80 过线；1t 4.06 差 0.15。** 恢复率
   0.999862 明确过。
5. **rate 0.25 / p0.50：恢复率 0.999950，但 2t 4.00 略低于 4.215；
   用 rate 0.30。**
6. HND0 的绝对值随共享宿主负载漂移；这些是同批 base/cand 的相对值，
   不是跨批拼接。block512 L16 rate 0.50 p0.30 的 2t 是 10.58 MB/s。
7. **三个 p 都已有恢复率可行点**：p0.30/r0.50 0.999821（1M，下界
   0.999783）、p0.40/r0.40 block512 L16 0.999168（1M，下界 0.999090）、
   p0.50/r0.30 0.999862（160k，下界 0.999763）。
8. T5.2 256 WAN 修好 announce mask 后，第一件事是跑 p0.40/r0.40
   block512 L16 的 2 线程 5 轮 A/B；其余点的 2t 5 轮复核随后。

## 7. 判定

- **i.i.d. 比特擦除**：bit-slicing 是真实加速（2.4–5.5x），但 HND0 上
  能否过 4.215 取决于第 6 节；rate 0.60 是最紧的点。
- **share 级擦除**：相关 mask 让 bit-slicing 回到 12.5x、158–260 MB/s，
  远超 4.215；但需要 (a) 码字按 share 映射（n=256/512），
  (b) 重新测 n=256/512 的恢复率（原型没有这些点），
  (c) codec 内部做 64×n_data 位转置，输出仍是字节序 share。
- **不要宣称 i.i.d. 有 8–16x**：实测是 2.4–5.5x；8–16x 只在相关擦除下成立。
