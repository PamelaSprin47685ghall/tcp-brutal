# SC-LDPC 原型微基准（symmetric 删除信道）

基线 `0db5166`，分支 `w-microbench`。被测代码是 fec-core2 的 SC-LDPC 原型
（`w-fec` head `c84c8dc`，`review/fec-prototype/sc_ldpc/`），本文只做构建、运行与记录，
没有另写实现。原始日志 `review/microbench/logs/sc-ldpc-{hnd0,x86-74}.log`。
2026-11-06 测得。

## 0. 状态与结论

1. **原型还没有通过 symmetric 解码。** 四个测试点的 `recovery` 全部是 `0.000000`
   （500 trials，99% 置信区间半宽 0.0065）。这不是可行性结论，是「原型未完成」。
2. **因此本文只报告成本面**：编码/解码 MB/s、peeling 迭代数、每块延迟、内存。
   恢复率要等作者修好 symmetric 解码后重跑；不要用本表的 recovery 判断 SC-LDPC 行不行。
3. **成本量级**：编码 2.9–5.4 MB/s、解码 0.4–1.6 MB/s（payload），迭代数 10–34，
   延迟 0.52–2.10 ms/块（按 1 Gbit/s 的块到达时间算），内存 91–667 KB。
   这是逐 bit 的 C 原型、没有 SIMD、每块只有 2048–11703 个变量节点，和
   `review/microbench.md` 里 1200 B 包级的 RLNC 数字不可直接比较。
4. **.65 按避让规则跳过**：跑前 load 7.32，检测到其他 worker 进程；不在 RK3588 上增加负载。

## 1. 容量前提（用户给定，本文不改）

- p=50% 时 1 Gbit/s 物理口的应用上限由容量界决定：码率 ≤0.5，app ≤500 Mbit/s。
- 有限长、高概率（99%+）需要码率 0.35–0.45，对应 app 350–450 Mbit/s。
- 500–750 Mbit/s 不再是 50% 丢包下的目标；本文的 rate=0.35/0.40/0.50 三档就是围绕这个前提选的。

## 2. 方法

- 构建（三机同一命令）：
  `cc -O2 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm -o sc_ldpc_bench`
- 运行：`--model symmetric`（数据与 parity 都按 p 删除，对应 256 条 carrier 的实际信道），
  `--trials 500`，每点 `timeout 15`，`nice -n 19` + `taskset` 单核。
- 四个点：`(block,L,rate,p)` = `(256,8,0.50,0.30)`、`(256,8,0.40,0.40)`、
  `(512,16,0.35,0.50)`、`(256,32,0.35,0.50)`。
- `block` 是每个耦合块的变量节点数；n、payload、edges 由 harness 打印。
- `enc_mbps`/`dec_mbps` 是 payload 的 MB/s；`iter_mean` 是 peeling 的波数；
  `latency_ms` 是 `L × 8192 B × 8 / 1e9` 的块到达时间。

## 3. 结果

### 3.1 HND0（2 vCPU，2.2 GHz，SSE2）

| block | L | rate | p | recovery | iter_mean | enc MB/s | dec MB/s | latency ms | mem B |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 256 | 8 | 0.50 | 0.30 | 0.000000 | 10.10 | 5.125 | 1.558 | 0.524 | 91,312 |
| 256 | 8 | 0.40 | 0.40 | 0.000000 | 11.49 | 3.555 | 1.022 | 0.524 | 122,544 |
| 512 | 16 | 0.35 | 0.50 | 0.000000 | 22.93 | 2.862 | 0.402 | 1.049 | 637,393 |
| 256 | 32 | 0.35 | 0.50 | 0.000000 | 33.71 | 4.085 | 0.638 | 2.097 | 666,649 |

### 3.2 .74（Athlon X4 760K，1.792 GHz）

| block | L | rate | p | recovery | iter_mean | enc MB/s | dec MB/s | latency ms | mem B |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 256 | 8 | 0.50 | 0.30 | 0.000000 | 10.10 | 5.436 | 1.364 | 0.524 | 91,312 |
| 256 | 8 | 0.40 | 0.40 | 0.000000 | 11.49 | 3.895 | 0.833 | 0.524 | 122,544 |
| 512 | 16 | 0.35 | 0.50 | 0.000000 | 22.93 | 2.989 | 0.514 | 1.049 | 637,393 |
| 256 | 32 | 0.35 | 0.50 | 0.000000 | 33.71 | 2.850 | 0.490 | 2.097 | 666,649 |

### 3.3 .65（RK3588）

**跳过。** 跑前 `uptime` load 7.32（1 分钟）、`ps` 显示其他 worker 进程；按避让规则
不在共享生产机上跑，日志中没有 .65 的 SC-LDPC 数字。

## 4. 读数

- **迭代数只随 L 增长**：L=8 → 10.1，L=16 → 22.9，L=32 → 33.7；同一 L 下 rate/p 的影响
  在 10–11.5 之间。这与「每帧窗口数 = L，peeling 波沿耦合链推进」的预期一致。
- **吞吐随 n 和 edges 下降**：256/L8 的 n=2048、edges=7680，enc 5.1 MB/s；
  512/L16 与 256/L32 的 n 都是 11703，但后者 edges 更多（58k vs 55k），enc 反而略高、
  dec 更低，说明当前实现受逐 bit 循环和缓存访问支配，不是干净的带宽曲线。
- **两机差异不大**：.74 的 enc 略高（5.44 vs 5.13），HND0 的 dec 在第一个点更高
  （1.56 vs 1.36），其余点 .74 更好。差异在 10–30% 量级，和两台机的共享负载同量级。
- **延迟**：块到达时间 0.52–2.10 ms；原型单块编码/解码的实际耗时远小于此
  （500 trials 在 15 s 内完成），所以延迟不是当前瓶颈，恢复率才是。

## 5. 与 RLNC 的关系

- `review/microbench.md` 的 RLNC 是 **1200 B 包级**、W=8–32、d=8–16 的实现，编码
  0.46–1.03 GB/s、解码 0.72–0.84 GB/s（HND0 单核）。
- 本原型是 **逐 bit** 的 SC-LDPC，块只有 2048–11703 个变量节点，且没有 SIMD；
  两者相差 2–3 个数量级，**不能**据此说「SC-LDPC 比 RLNC 慢 1000 倍」。
- 可比的只有结构性成本：SC-LDPC 的编码是 O(edges)、解码是 O(edges + 恢复的 bit 数)；
  RLNC 的编码是 O(d × parity 数)、解码是 O(k²) payload XOR。等原型的 symmetric 解码
  修好、并且改成包级/字级实现后，才有同口径的对比。

## 6. 未跑/失败项

- **recovery 全 0**：原型 symmetric 解码未通过，作者正在修；本文不把它当可行性结论。
- **.65 跳过**：load 7.32 + 其他 worker，按避让规则不跑。
- 没有跑完整 54 点矩阵，也没有跑 systematic/native 模型；按 fec-core2 的要求只跑
  少量 symmetric 点确认失败。
- 三机均未安装软件、未改服务/网络、未加载模块；`/tmp` 已清空；未提交产品代码。

## 7. 复现

```sh
cd review/fec-prototype/sc_ldpc          # w-fec c84c8dc
cc -O2 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm -o sc_ldpc_bench
nice -n 19 taskset -c 0 ./sc_ldpc_bench --block 256 --L 8 --rate 0.50 --p 0.30 \
     --trials 500 --model symmetric
```

批量脚本 `review/microbench/run_sc_ldpc.sh` 保留了 54 点矩阵的入口，等原型修好后使用。
