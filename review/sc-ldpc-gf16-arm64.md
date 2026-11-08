# GF(16) SC-IRA ARM64 CPU Codec: Bench Harness, x86 Baseline, and .65 Measurements

Date: 2026-11-06. Branch: `w-sclc-gf16`. Worktree: `/home/kunweiz/tbtun-worktrees/sclc-gf16`.
Scope: CPU codec only (bit-sliced/GF16), no network, no HND0, no push.

## 0. Deliverables

- `review/fec-prototype/sc_ldpc_arm64/` — ARM64-targeted copy of the GF(16) SC-IRA prototype.
- `build_arm64.sh` — builds `sc_ldpc_gf16_bench_arm64` for `aarch64-linux-gnu` with `-march=armv8-a+crypto`.
- `sc_ldpc_gf16_bench_x86` — native x86-64 binary for local baseline comparison.
- Scalar GF(16) benchmark matrix: 20k trials, 5 seeds, 0.20 s bench window, 5 runs median.
- `.65` (lubancat, Cortex-A55/A76) native GCC build and the measured GF(16)
  1t/2t matrix, bit-sliced and binary-scalar b512 baselines; data in
  `review/sc-ldpc-gf16-arm64.csv`, `sc-ldpc-gf16-arm64-core.log`,
  `sc-ldpc-bitslice-arm64.csv`, `sc-ldpc-binary-arm64.csv`. See §6.

## 1. x86-64 Scalar Baseline (Ryzen 9 7940HS, 1t/2t)

All numbers are payload MB/s (n_data symbols × 4 bits / 2 = bytes per codeword).
`enc` = encode throughput; `dec` = decode throughput (peeling + optional core solve).

### Symmetric model (all variables erased with prob p=0.40)

| block | L | rate | threads | enc MB/s | dec MB/s | recovery (20k, 5 seeds) |
|---|---|---|---|---|---|---|
| 128 | 16 | 0.40 | 1 | 95.3 | 30.5 | 0.9990–1.0000 |
| 128 | 16 | 0.40 | 2 | 93.6 | 57.5 | 0.9974–1.0000 |
| 128 | 16 | 0.45 | 1 | 114.2 | 35.5 | 0.9946–1.0000 |
| 128 | 16 | 0.45 | 2 | 111.8 | 66.8 | 0.9934–1.0000 |
| 128 | 16 | 0.50 | 1 | 133.6 | 40.4 | 0.9898–0.9953 |
| 128 | 16 | 0.50 | 2 | 132.6 | 78.8 | 0.9709–0.9932 |
| 256 | 16 | 0.40 | 1 | 94.6 | 29.9 | 0.9969–1.0000 |
| 256 | 16 | 0.40 | 2 | 94.0 | 56.2 | 0.9981–0.9999 |
| 256 | 16 | 0.45 | 1 | 115.4 | 35.5 | 0.9976–1.0000 |
| 256 | 16 | 0.45 | 2 | 113.0 | 67.4 | 0.9985–0.9999 |
| 256 | 16 | 0.50 | 1 | 134.7 | 39.9 | 0.9964–0.9989 |
| 256 | 16 | 0.50 | 2 | 134.6 | 79.3 | 0.9971–0.9981 |

### Systematic model (only payload erased, parity known)

| block | L | rate | threads | enc MB/s | dec MB/s | recovery (20k, 5 seeds) |
|---|---|---|---|---|---|---|
| 128 | 16 | 0.40 | 1 | 95.7 | 37.3 | 1.0000 |
| 128 | 16 | 0.40 | 2 | 95.2 | 72.0 | 1.0000 |
| 128 | 16 | 0.45 | 1 | 112.9 | 43.5 | 1.0000 |
| 128 | 16 | 0.45 | 2 | 114.4 | 86.7 | 1.0000 |
| 128 | 16 | 0.50 | 1 | 135.5 | 51.0 | 1.0000 |
| 128 | 16 | 0.50 | 2 | 134.5 | 101.5 | 1.0000 |
| 256 | 16 | 0.40 | 1 | 95.6 | 37.5 | 1.0000 |
| 256 | 16 | 0.40 | 2 | 95.1 | 72.2 | 1.0000 |
| 256 | 16 | 0.45 | 1 | 113.4 | 43.5 | 1.0000 |
| 256 | 16 | 0.45 | 2 | 113.9 | 85.1 | 1.0000 |
| 256 | 16 | 0.50 | 1 | 135.5 | 50.6 | 1.0000 |
| 256 | 16 | 0.50 | 2 | 135.8 | 96.6 | 1.0000 |

## 2. Key Observations

1. **Systematic decode is 20–30% faster than symmetric** because it only resolves payload variables; parity bits are known and skipped.
2. **Encode is stable at 94–136 MB/s** across all configurations; it is a single forward sweep with no data parallelism.
3. **Decode is the bottleneck**: 30–101 MB/s depending on rate/model/threads. The q-ary peeling inner loop does random memory access through `vadj`/`cadj` and a 256-entry GF(16) multiply table.
4. **2-thread scaling is ~1.9×** for decode, consistent with the binary scalar decoder.
5. **Core solve is negligible** in throughput; it only runs on the rare stopping-set cases and does not affect the median decode speed.

## 3. NEON Analysis

- `vqtbl1q_u8` can look up 16 bytes at a time from a 16-byte table. The GF(16) multiply table is 256 bytes; splitting it into 4 tables of 64 entries each requires high-bit selection, which adds overhead.
- The peeling decoder's bottleneck is **random memory access** through `vadj`/`cadj`, not arithmetic. NEON vectorization of the multiply table will not help much.
- More promising directions:
  - **CSR reordering** of `vadj`/`cadj` for cache locality.
  - **Prefetching** the next check's adjacency.
  - **Bit-sliced GF(16)**: 64 codewords in parallel, each nibble as a bit-plane. This is a larger rewrite but could give 64× on the peeling loop.

## 4. Next Steps

- Deliver bench harness + scalar baseline (this document).
- NEON rewrite deferred to a separate ticket; the scalar baseline is sufficient for ARM64/HND0 comparison.
- ARM64 `.65` measurements are done (§6); `.65` builds natively, so `CROSS=1`
  is only needed for a non-`.65` aarch64 target.

## 5. Reproduce

```sh
cd review/fec-prototype/sc_ldpc_arm64
./build_arm64.sh            # native x86-64
./build_arm64.sh CROSS=1    # aarch64-linux-gnu

# x86 baseline (example: b128 r0.45 sym 1t)
./sc_ldpc_gf16_bench_x86 --block 128 --L 16 --rate 0.45 --p 0.40 \
    --trials 20000 --model symmetric --codes 1 --threads 1 \
    --core-max 256 --bench-sec 0.20 --runs 5 --no-header
```

## 6. ARM64 `.65` 实测（2026-11-06 UTC）

Host `.65`（lubancat）：4×Cortex-A55 @1.8 GHz + 4×Cortex-A76 @2.3 GHz，
Ubuntu gcc 15.2.0，`-O3 -std=c99 -Wall -Wextra -Werror -march=armv8-a`，
`scaling_governor=performance`。运行 `taskset -c 4`（1 线程）或
`taskset -c 4,5`（2 线程）、`nice -n 10`；GF(16) 每点 `--trials 20000`、
`--bench-sec 0.20 --runs 5`，5 个 seed 取中位，recovery 是 5×20k
独立 trial 的 pooled。纯用户态：无 tbtun、无网络、无 tunnel。数据：
`review/sc-ldpc-gf16-arm64.csv`（GF16）、`sc-ldpc-gf16-arm64-core.log`
（core solve）、`sc-ldpc-bitslice-arm64.csv`（bit-sliced i.i.d.）、
`sc-ldpc-bitslice-corr-arm64.csv`（bit-sliced 相关擦除）、
`sc-ldpc-binary-arm64.csv`（binary 标量）、
`sc-ldpc-gf16-arm64-env.log`（环境、构建、正确性）。

### 6.1 GF(16) scalar

单位 payload MB/s。`enc` 单线程（编码没有数据并行）；`dec` 是
`--threads 1|2` 的聚合吞吐。

| model | block | rate | threads | enc | dec | recovery (100k) | min–max |
|---|---|---|---|---|---|---|---|
| symmetric | 128 | 0.40 | 1 | 30.3 | 6.2 | 0.999940 | 0.99985–1.0 |
| symmetric | 128 | 0.40 | 2 | 30.3 | **12.3** | 0.999900 | 0.99975–1.0 |
| symmetric | 128 | 0.45 | 1 | 36.1 | 7.7 | 0.999940 | 0.99985–1.0 |
| symmetric | 128 | 0.45 | 2 | 36.1 | **15.2** | 0.999920 | 0.99980–1.0 |
| symmetric | 256 | 0.40 | 1 | 29.5 | 5.1 | 0.999980 | 0.99995–1.0 |
| symmetric | 256 | 0.40 | 2 | 29.6 | 10.0 | 0.999980 | 0.99995–1.0 |
| symmetric | 256 | 0.45 | 1 | 35.4 | 6.2 | 0.999980 | 0.99995–1.0 |
| symmetric | 256 | 0.45 | 2 | 35.5 | 12.2 | 0.999970 | 0.99990–1.0 |
| systematic | 128 | 0.40 | 1 | 30.3 | 9.2 | 1.000000 | 1.0 |
| systematic | 128 | 0.40 | 2 | 30.2 | 17.9 | 1.000000 | 1.0 |
| systematic | 128 | 0.45 | 1 | 36.2 | 10.9 | 1.000000 | 1.0 |
| systematic | 128 | 0.45 | 2 | 36.1 | **21.4** | 1.000000 | 1.0 |
| systematic | 256 | 0.40 | 1 | 29.5 | 7.4 | 1.000000 | 1.0 |
| systematic | 256 | 0.40 | 2 | 29.6 | 14.4 | 1.000000 | 1.0 |
| systematic | 256 | 0.45 | 1 | 35.5 | 8.7 | 1.000000 | 1.0 |
| systematic | 256 | 0.45 | 2 | 35.5 | 17.1 | 1.000000 | 1.0 |

- 2 线程扩展 1.95–1.98×，和 x86 的 1.9× 一致。
- core solve 在 20k 点全部 `skipped=0`、`vars_max≤18`；core-max 256
  从未成为限制。symmetric 的残余失败就是 core 的 `failed`（b128
  r0.40：6/100k；r0.45：6/100k；b256 r0.40：2/100k；r0.45：2/100k），
  `under`/`rankdef` 都有，`bad=0`；systematic 0 失败。
- 与 x86（Ryzen 9 7940HS）同点比：decode 慢 4.4–4.9×，encode 慢
  3.1×；A76 2.3 GHz 对 Ryzen 的 5 GHz + 更宽核，比例合理。

### 6.2 binary bit-sliced b512 L16 p0.40（i.i.d.）

`sc_ldpc_bs_bench --block 512 --L 16 --p 0.40 --trials 20000 --batch 512
--threads 2`，5 seed 中位；`dec scalar` 是同批标量解码，`dec bs 1t` 是
bit-sliced 1 线程，`dec bs 2t` 是 2 线程。

| rate | dec scalar | dec bs 1t | dec bs 2t | recovery bs | 1t speedup | 2t speedup |
|---|---|---|---|---|---|---|
| 0.40 | 1.29 | 4.62 | 6.43 | 0.999609 | 3.5× | 2.5× |
| 0.45 | 1.53 | 4.23 | 5.86 | 0.997266 | 2.8× | 1.9× |

- **recovery 样本只有 2560 个唯一码字。** `sc_ldpc_bs_bench` 的
  `--trials` 是 `trials/batch` 次重复解码同一个 batch，不是重新抽
  擦除图样：每 seed 512 个码字，5 seed 合计 2560。r0.40 是 1/2560
  失败，r0.45 是 7/2560。这个精度不足以单独证明 0.999，只能作为
  x86 1M 基线（0.999168/0.998084）的同向复核。
- `unres_mismatch=0`、`bit_mismatch=0`：bit-sliced 与同批标量解码
  逐字节一致；`recovery_bs == recovery_scalar`。
- 2 线程只有 1.38–1.39× 扩展（GF(16) 是 1.97×）：bit-sliced 内层是
  64 位 XOR + 位平面计数，A76 上先撞内存带宽。
- 与 x86 同点比：scalar 5.5×、sliced 1t 4.5×、2t 5.5× 慢。

### 6.3 binary bit-sliced b512 L16 p0.40（share 级相关擦除）

`--corr`：同一 batch 的 512 个码字共享一个擦除 mask，这是 app mode3
（一个 share 的 65536 个 bit-plane 看到同一缺失 share 集合）的映射。

| rate | dec scalar | dec bs 1t | dec bs 2t | recovery bs | 2t speedup |
|---|---|---|---|---|---|
| 0.40 | 1.35 | 21.06 | 33.19 | 1.000000 (0/2560) | 13.2× |
| 0.45 | 1.61 | 28.76 | 36.88 | 0.800000 (512/2560) | 11.8× |

- 相关擦除下 bit-sliced 快一个数量级：2t 33–37 MB/s，是 i.i.d. 的
  5–6×，与 x86 的 158–260 MB/s 同方向（绝对值受 A76 主频/内存限制）。
- **但共享 mask 的恢复是全有或全无**：r0.45 的 5 个 mask 里有 1 个
  是 stopping set，整批 512 个码字全部失败 → pooled 0.8；r0.40 的
  5 个 mask 全部解出。相关擦除不能只看吞吐，mask 级的失败会整组放大。
  这也是 x86 表只报 corr 吞吐、没报 corr 恢复的原因。

### 6.4 binary scalar b512 L16 p0.40（encode 参考）

bit-sliced bench 不测 encode；用 `sc_ldpc_bench` 同一台机器、同一码，
取 encode MB/s 和标量 decode 参考（`--term` 默认 ACC，与 GF(16) 图
同规则）。

| rate | threads | enc | dec | recovery (100k) |
|---|---|---|---|---|
| 0.40 | 1 | 8.5 | 1.31 | 0.999410 |
| 0.40 | 2 | 8.5 | 2.63 | 0.999570 |
| 0.45 | 1 | 10.0 | 1.57 | 0.998090 |
| 0.45 | 2 | 10.0 | 3.09 | 0.998200 |

- 标量 encode 8.5–10.0 MB/s；x86 同点 22.9–27.5，慢 2.7×。GF(16)
  encode 30.3–36.1，反而比 binary 标量快 3.6×：q-ary 码的边数只有
  binary 的 1/4，前向扫描的随机访问次数随之减少。
- 恢复率与 x86 的 1M 基线一致：r0.40 0.99941（x86 0.999168），
  r0.45 0.99809（x86 0.998084）。2 线程只改吞吐，不改恢复。
- 这里测的是 binary **标量** encoder。64 码字 bit-sliced encoder
  （`w-sclc-encode` 的 `sc_enc64`）是另一个 worktree，不在本基线内。

### 6.5 判定

- **arm64 2 线程验收点通过。** 上行目标 33.72 Mbit/s = 4.215 MB/s。
  `.65` 上 GF(16) b128 r0.45 2t decode **15.2 MB/s（3.6×）**，1t 也有
  7.7（1.8×）；b128 r0.40 2t 12.3（2.9×）；b256 r0.45 2t 12.2
  （2.9×）；b256 r0.40 2t 10.0（2.4×）。全部过门，2 线程不是必需。
- **recovery 不随 ISA 变。** 同 seed/图/擦除模型下 arm64 只是整数
  运算；r0.45 ≥0.99992，r0.40 ≥0.99990，与 x86 一致。
- **i.i.d. 下 GF(16) 在 `.65` 上也快于 bit-sliced：** 1t 1.3–1.8×、
  2t 1.9–2.6×；同 bit block 的恢复 margin 也更大（r0.45 0.99992 对
  0.99727，注意 bit-sliced 只有 2560 唯一码字）。
- **相关擦除下 bit-sliced 仍然最快：** `.65` 2t 33–37 MB/s，是
  GF(16) b128 2t 的 2.2–2.4×。代价是共享 mask 的整组失败风险
  （r0.45 5 个 mask 坏 1 个）。产品取舍与 x86 结论一致：i.i.d./独立
  擦除用 GF(16)，share 级相关擦除用 bit-sliced；不能在 corr 模式下
  只看吞吐、不看 mask 级恢复。
- **与 HND0 的关系：** HND0 是 QEMU 2 vCPU（x86 qemu64），`.65` 是
  ARM64 验收平台；本文数字不能外推到 HND0，反之亦然。`.65` 的
  2t 15.2 MB/s 回答了 `sc-ldpc-gf16.md` §5 留下的“GF(16) 在 ARM64
  上够不够 4.215”的问题：够，且余量 3.6×。

### 6.6 复现

```sh
# .65 原生构建（无 cross）
gcc -O3 -std=c99 -Wall -Wextra -Werror -march=armv8-a \
    sc_ldpc.c sc_ldpc_gf16.c sc_ldpc_gf16_bench.c -lm -pthread \
    -o gf16_bench

# GF16：4 点 × 2 model × 1t/2t × 5 seed
for model in symmetric systematic; do for block in 128 256; do
  for rate in 0.40 0.45; do for t in 1 2; do
    cpus=4; [ "$t" = 2 ] && cpus=4,5
    for seed in 1 2 3 4 5; do
      taskset -c "$cpus" nice -n 10 ./gf16_bench --block "$block" --L 16 \
        --rate "$rate" --p 0.40 --trials 20000 --codes "$t" \
        --model "$model" --threads "$t" --seed "$seed" --core-max 256 \
        --bench-sec 0.20 --runs 5 --no-header \
        --series "gf16-arm64-b${block}-r${rate}-${model}-t${t}"
    done
  done; done
done; done

# bit-sliced i.i.d. / corr（每 seed 512 个唯一码字）
taskset -c 4,5 nice -n 10 ./bs_bench --block 512 --L 16 --rate 0.40 \
    --p 0.40 --trials 20000 --batch 512 --threads 2 --model sym --seed 1
taskset -c 4,5 nice -n 10 ./bs_bench --block 512 --L 16 --rate 0.45 \
    --p 0.40 --trials 20000 --batch 512 --threads 2 --model sym --corr --seed 1

# binary 标量 encode 参考
taskset -c 4 nice -n 10 ./bin_bench --block 512 --L 16 --rate 0.45 \
    --p 0.40 --trials 20000 --codes 1 --model symmetric --extra-dc 5 \
    --threads 1 --seed 1 --no-header
```
