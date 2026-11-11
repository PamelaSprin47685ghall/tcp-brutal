# SC-LDPC codec build cost (route A gate)

2026-11-11，worktree `/tmp/tbt-dp`，branch `w-dataplane-perblock`，base `66ecd19`。

门限：下行 binary codec 构建与上行 8-group GF(16) 构建，p99 ≤ 2000 µs。

## 1. 被测构造器

`tbt_fec_sclc_new()` 定义在 kernel-only 的 `tbt_fec_data.c`，host bench 不能链接它。
Bench 调用它唯一分派到的 `tbt_fec_grouped_new_ops()`，参数与 `tbt_fec_sclc_new()`
对同一 shape 推导的完全一致：

| 方向 | ops | groups | n+m | k | w | s | d |
|---|---|---|---:|---:|---:|---:|---:|
| 下行 | `tbt_fec_ops_sclc` | 1 | 256 | `rate_ppm = n*1000/256` | `block = 256` | 8192 | 0 |
| 上行 | `tbt_fec_ops_sclc_gf16` | 8 | 32 | `block = 128` | `L = 16` | 8192 | `rate_256 = n*256/32` |

下行 shape 的 L 不进入 binary codec：`tbt_fec_sclc_new()` 对 DOWN 传
`w = block`，graph 固定 `L = 8`（`tbt_fec_sclc.c` 的 `sclc_new()` 只接受
`w == 256`）。所以下行表用 L=8，上行表用 L=16。把下行 shape 的 L 写成 16
不会改变构造参数，codec 与 L=8 完全相同，因此没有单独的 L=16 行。

上行 8 组走的是数据面路径：`tbt_fec_grouped_new_ops()` 调 8 次
`tbt_sclc_uplink_new(1, ...)`，不是一个多组对象。表里的 `up8` 是另一个入口
`tbt_sclc_uplink_new(8, ...)` 的交叉检查，它不是 `tbt_fec_sclc_new()` 的路径。

## 2. 方法

- `clock_gettime(CLOCK_MONOTONIC)` 包住 `new`；`free` 在计时外。
- 每点 8 seeds × 256 builds = 2048 个样本，先 16 次 warmup。
- 样本排序；p50/p99 用 nearest-rank，max 取最大样本。
- 内存是持有一个 codec 时的 live heap 增量（glibc `mallinfo2` 的
  `uordblks + hblkhd`），取 8 个 seed 的最大值。它包含 grouped container
  和分配器 chunk 取整。
- 上行 per-codec 另列 `tbt_sclc_uplink_mem()`；该 helper 是请求字节数之和，
  与实测差 < 3%。
- 下行没有 `tbt_sclc_*` mem helper；`codec_B` 是实测值，并在 §5 按源码逐项核对。

## 3. 环境

- host: AMD Ryzen 9 7940HS，16 threads
- kernel: `Linux 7.2.9-1-MANJARO #1 SMP PREEMPT_DYNAMIC x86_64`
- compiler: `cc (GCC) 16.2.1 20260810`
- libc: glibc 2.44
- flags: `-std=gnu99 -O2 -Wall -Wextra -Werror`（Makefile `HOST_CFLAGS`）
- build: `make tests/bench/bench_sclc_build`
- run: `taskset -c 3 build/bin/bench_sclc_build`
- target: `make host-bench-sclc-build` 构建后直接运行，不绑核；FAIL 时以非零退出。

表中的数字来自上面绑核的一次运行。

## 4. 结果

### 4.1 下行 `tbt_fec_sclc_new`（DOWN，block=256，L=8）

| n | m | p50 µs | p99 µs | max µs | codec B |
|---:|---:|---:|---:|---:|---:|
| 64 | 192 | 59.1 | 81.7 | 103.4 | 261472 |
| 76 | 180 | 56.7 | 87.5 | 119.1 | 256672 |
| 102 | 154 | 47.0 | 71.6 | 146.5 | 246256 |
| 128 | 128 | 41.8 | 60.4 | 77.9 | 234832 |
| 151 | 105 | 48.3 | 66.5 | 91.5 | 221200 |
| 178 | 78 | 53.9 | 72.6 | 106.1 | 212560 |

下行全部通过：表中 p99 最差 87.5 µs。重复运行观察到 p99 最高约 158 µs
（机器有其它负载时），仍远低于 2000 µs。

### 4.2 上行 8 组，数据面路径（`tbt_fec_grouped_new_ops`，8 × `tbt_sclc_uplink_new(1)`）

n+m=32，block=128，L=16。

| n | m | p50 µs | p99 µs | max µs | codec B（单组） | 8-group B（实测） | helper B |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 | 24 | 2820.2 | 5112.1 | 5717.5 | 1158640 | 9181472 | 1162880 |
| 16 | 16 | 461.3 | 879.3 | 1149.9 | 525792 | 4222192 | 530536 |
| 20 | 12 | — | — | — | — | — | — |
| 24 | 8 | 347.4 | 671.8 | 927.1 | 412688 | 3328720 | 421264 |

n=20 被构造器拒绝，原因见 §6。

### 4.3 上行 8 组，直接多组 API（`tbt_sclc_uplink_new(8, ...)`，交叉检查）

| n | m | p50 µs | p99 µs | max µs | helper/8 B | 8-group B（实测） | helper B |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 8 | 24 | 2827.9 | 4900.6 | 5495.0 | 1150025 | 9180304 | 9200200 |
| 16 | 16 | 468.2 | 923.0 | 4931.9 | 529514 | 4216816 | 4236112 |
| 20 | 12 | — | — | — | — | — | — |
| 24 | 8 | 320.1 | 538.2 | 617.3 | 419787 | 3327520 | 3358296 |

两个上行入口都在 n=8 超过门限，所以 FAIL 不依赖入口选择。

重复运行（`taskset -c 5`）：数据面 n=8 p99 5980.1 µs，直接 API n=8
p99 5748.4 µs；未绑核的 `make host-bench-sclc-build`：数据面 6120.6 µs，
直接 API 5765.6 µs；另一次绑核运行：数据面 5718.1 µs，直接 API 5236.2 µs。
n=8 的 p99 在 4.8–6.1 ms 之间。直接 API 的 n=16 在负载下 p99 最高观察到
1731.8 µs，仍在 2000 µs 内，但余量不大。其余点低于 2000 µs。结论不变。

## 5. 内存核对

下行分配项（`N = 8*(n+m) = 2048`，`C = 8*m`，`E = 8*dv*n + 15*m`，
`dv = 5`（n≤m）或 4（n>m））：

    graph:   (N+1)*4 + (C+1)*4 + 2*E*4 + C*4 + C*dcs*2 + C
    scratch: 5*N*8 + C*4*8 + 2*C*8 + C*4 + N*4 + N + n + 128
    另加 struct tbt_sclc 与 grouped container（约 2.1 KiB）

`dcs` 是构建时算出的最大 check degree：n≤m 时为 7；n>m 时由 seed 决定，
上限 `2 + 4*(floor(n/m)+1)`。n=178 的 8-seed 实测最大值为 212560 B，对应
`dcs = 13`。以 n=64 为例，上式求和 259272 B，加 struct 与 container 约
261.5 KiB，实测 261472 B；逐项求和与实测在 0.1% 内一致。

上行 `tbt_sclc_uplink_mem()` 不计 `share_bad`/`share_known` 的
`2*(n+m)` 字节，也不计 `plab`/`qlab`（BITPLANE64 下为 NULL）。helper 与
实测的差异在 3% 内，表中的 `8-group B` 是实测 live heap。

内存量级：下行单 codec 212560–261472 B；上行单 codec 412688–1158640 B，
8 组 3328720–9181472 B。n=8（25% 档）的 8 组构建约 9.2 MB，是上行最贵的一点。

## 6. n=20 为什么不可构造

上行 n=20、m=12、block=128、L=16：

- `n_data = L*(block/2) = 1024`，`codes = n*16384/1024 = 320`；
- rate = 160/256，`n_par` 向上取整到 lift=64：`want = 1024*96/160 = 614`，
  `n_par = 10*64 = 640`；
- `codes*n_par = 320*640 = 204800 > m*16384 = 196608`。

`tbt_sclc_uplink_new()` 的预算检查拒绝这个组合，两个入口都返回 NULL。
n=24 可以构造（`384*320 = 122880 ≤ 131072`），但它是 75% 档，高于 wire
的 70% 上限，不在生产 ladder 上。生产 ladder 中本表覆盖的是 n=8（25%）
和 n=16（50%）。

## 7. 结论

**FAIL**：上行 25% 档（n=8，m=24）的 8 组构建 p99 为 5112.1 µs（数据面路径）
和 4900.6 µs（直接多组 API），均超过 2000 µs 门限；p50 也已约 2.8–2.9 ms。
下行全部通过（表中 p99 最差 87.5 µs；负载下观察到约 158 µs）。
n=16、n=24 的上行构建通过（本表 p99 最高 923.0 µs；另一次负载运行中
直接 API n=16 的 p99 到过 1731.8 µs）。

Route A 的“每块 n/m + LRU”在 25% 档不满足 2 ms 构建门限，因此按门限规则
停止：未开始任何 route-A data-plane 编辑。
