# 上行 grouped GF(16) SC-IRA codec：实现与实测

状态：实现完成，host 测试/ASan/内核编译通过；恢复率未达 p=0.40 block 0.999，
实测最低 0.99（多 seed 中位 0.9967）。本文只记录事实与接口，不把 0.85 当通过。
基线 `00d34cf`，分支 `w-sclc-uplink`，worktree `/home/kunweiz/tbtun-worktrees/sclc-uplink`。

## 0. 结论

1. **per-group 8×32 的擦除是“share 级二项分布”，不是 i.i.d. 符号擦除。**
   同一 group 内一个 codeword 只看到 n 个 data share 和 m 个 parity share；
   p=0.40 时丢失 share 数 K~Bin(n,p)，K/n 的尾部远宽于 i.i.d.。
   固定擦除比例 q=0.40/0.45 时码字恢复 1.0；q=0.50 时 7/16 码崩到 0.18。
   所以 p=0.40 的瓶颈是 K 的方差，不是 interleave 的周期性。
2. **`(j+c) mod n` 周期 interleave 不是问题。** seeded 随机置换在 p=0.40
   的恢复率与周期映射相同或更低（7/16：0.864 vs 0.858；3/8：0.952 vs
   0.951；1/3：0.9998 vs 1.0）。随机置换还多 0.4–0.5 MiB 标签表。
3. **per-group 8×32 的实测（40–300 blocks，5 seed）：**

| rate (n+m) | p=0.30 cw/block | p=0.40 cw/block | p=0.50 cw/block |
|---|---|---|---|
| 7/16 (14+18) | 1.000/1.000 | 0.858–0.865 / 0.79–0.85 | 0.29 / 0.20 |
| 3/8 (12+20) | 1.000/1.000 | 0.951–0.974 / 0.91–0.95 | 0.71 / 0.58 |
| 1/3 (10+22) | 1.000/1.000 | 0.980–0.987 / 0.955–0.98 | 0.85 / 0.72 |
| **1/4 (8+24)** | **1.000/1.000** | **0.9985–1.000 / 0.990–0.997** | 0.977 / 0.965 |

   1/4 的 5 seed × 300 block p=0.40：block 0.9967 / 1.0 / 0.9967 / 0.9967 /
   1.0，codeword 0.99974–1.0。**满足“至少 >0.99”，未稳定达到 0.999。**
   7/16、3/8、1/3 都不满足 >0.99（1/3 在 200 block 时 0.955）。
   原始多 seed（1/4，p=0.40，300 block/seed，codeword/block）：

   | seed | codeword | block |
   |---:|---:|---:|
   | 1 | 0.999740 | 0.9967 |
   | 2 | 1.000000 | 1.0000 |
   | 3 | 0.999870 | 0.9967 |
   | 4 | 0.999870 | 0.9967 |
   | 5 | 1.000000 | 1.0000 |

   **block 0.99–0.997 与目标 0.999 的差距是二项尾部，不是实现误差。**
   host test 的硬断言只覆盖用户给的“至少 >0.99”下限，并在输出中打印
   `TARGET p=0.40 block 0.999: NOT MET`；没有把 0.999 写成通过。
4. **0.999 需要更小的二项尾部。** 两个候选：
   - per-group 再降码率（7/32）没有改善（0.993–1.0，仍然 block <0.999），
     因为 n 仍只有 7–8；
   - **全局 256-share 交错**（8×32 只作为 carrier 分组，codeword 跨全部
     256 share）：host 原型 `n=64,m=192,rate 1/4` 在 p=0.30/0.40/0.50
     各 60 block 全部 1.0；`n=96,m=160,rate 3/8` p=0.40 block 0.983、
     codeword 0.999989；`n=86,m=170,rate 1/3` p=0.40 block 0.967。
     全局 1/4 的 60 block 明细：p=0.30 cw 1.0/block 1.0；p=0.40
     cw 1.0/block 1.0；p=0.50 cw 1.0/block 1.0。全局 3/8 与 1/3 的
     codeword 已接近 1.0，但 block 仍被 1536/1376 个 codeword 的独立
     失败率压到 0.983/0.967；全局 1/4 才把两者同时推到 1.0。
     全局交错的方差是 Bin(256,p)/256（std 0.031 vs per-group 0.09–0.13），
     这是 0.999 的唯一实测路径。代价是单个 2 MiB FEC block（8 个 decoder
     上下文仍可并行解 128 codeword/组），不是 8 个独立 share 组。
     这些全局数字是 host 原型 `groups=1`，尚未接入 per-group API，也没有
     数据面 owner 对 flat 256 指针接口的同意；不要把它当已实现。
5. **CPU 不是主要风险，但 1/4 的 payload 吞吐更低。** Ryzen 单线程：
   1/4 encode 30.5 MB/s、decode 20.8（p=0）/ 6.05（p=0.30）/ 5.69
   （p=0.40）MB/s payload；7/16 encode 57、decode 37（p=0）/ 11–18
   （p=0.30/0.40）。HND0 2t 按码率折算 1/4 约 5 MB/s payload，接近
   4.215，但未上机验证；不能用本机数字当验收。

## 1. 码型与参数

- 码型：SC-IRA `SC_TERM_ACC`，GF(16) 符号，系统化前向编码，BEC peeling
  + 有界 GF(16) core solve（`max_core=256`，矩阵/GF(2) 伴随矩阵各
  512 KiB 上限）。
- 每 codeword：`block=128` 变量/耦合块，`L=16`，`n_data=L*block/2=1024`
  符号（512 B payload）。码率由 `n_par` 决定；per-group 形状取
  `n_par = n_data*(den-num)/num`（向下取整）。
- 每 group block：`C = n*16384/1024 = n*16` codeword（share=8192 B=16384
  GF(16) 符号）。
- 256 carriers：`groups*(n+m)=256`；per-group 8×32 或 32×8。
- 全部 Phase 1 形状都满足 `n_par*C <= m*16384`；1/4 是唯一精确填满
  data/parity 的形状（8/32=1/4）。

## 2. share↔symbol 映射

- 单位是 GF(16) symbol（4 bit），每字节两个 symbol，低 nibble 在前。
- 生产映射 `(j+c) mod n`（payload）/ `(j+c) mod m`（parity），offset 按
  `(codeword, j)` 顺序的 per-share 计数器递增；收发两侧用同一循环推导，
  不传表。丢一个 data share 擦掉每个 codeword 的 1/n（周期 n），
  丢 parity share 擦掉 1/m。
- `TBT_SCLC_MAP_CONTIGUOUS`：codeword c 占 payload `[c*n_data,(c+1)*n_data)`；
  host A/B 用。p=0.30 时 interleaved block 0.95 vs contiguous 0.0
  （丢一个 data share 就整段 codeword 全擦，peeling 无法启动）。
- `TBT_SCLC_MAP_RANDOM`：每 codeword 用 seeded Fisher-Yates 置换 share
  标签，计数精确（base+rotating remainder）；p=0.40 与周期映射持平或更差，
  不作为生产默认。

## 3. 接口

### 3.1 直接 API（`tbtun/tbt_sclc_uplink.h`）

```c
struct tbt_sclc_params {
    u16 n, m;          /* data/parity shares per group */
    u16 block, L;      /* 128, 16 */
    u16 rate_num, rate_den; /* code rate; 1/4 or 7/16 */
    u16 max_core;      /* 256 */
    u8  mapping;       /* INTERLEAVED / CONTIGUOUS / RANDOM */
    u64 seed;
};
struct tbt_sclc_uplink *tbt_sclc_uplink_new(u16 groups, const struct tbt_sclc_params *);
int tbt_sclc_uplink_encode(u, group, u8 *data[], u8 *parity[]);
int tbt_sclc_uplink_decode(u, group, u8 *data[], u8 *parity[],
                           const u8 *data_present, const u8 *parity_present);
```

`decode` 返回 data share 中“完整”的个数；`tbt_sclc_uplink_share_known()`
给出逐 share 完整性；未完整的 share 必须 NACK，绝不交付半知字节。
`tbt_sclc_uplink_stat()` 给出 codeword ok/failed、core solve、缺失/恢复
share 数。

### 3.2 `tbt_fec_ops`

`tbt_fec_ops_sclc_gf16`（`mode=3`，name `sc-ldpc-gf16`）：
`new(n,m,k=block,w=L,s=S,d=rate*256,nonce=seed)`；`coeffs` 返回
`-ENOSYS`；`encode/decode` 作用于一个 group block。**`d=rate*256` 是
ops 内部约定，不是 wire 字段。**

### 3.3 wire / config 裁决

- rate 由 `n/(n+m)` 推出；不在 FEC_CFG 增加 rate 字段。1/4 = 8/32、
  7/16 = 14/32、3/8 = 12/32、1/3 对应 10+22 时实际 share 比是 10/32，
  若要求严格 `rate=n/(n+m)` 应选 10+22 的 5/16 或 8+24 的 1/4。
- `block`/`L` 走 session config（`mode=sc-ldpc` 下的配置），不改
  0x1006 FEC_CFG 的 reserved 字节，不偷用 `w/k`；0x1007 再考虑显式布局。
- 40 B header / 48 B FEC_CFG 不变。

## 4. 测试与性能

- `host/test_sclc_uplink.c`：三种 mapping、5 个形状、全 share 往返、
  逐个 share 丢失、p=0.30/0.40/0.50、8 group 独立擦除、replay
  byte-exact、ops、参数拒绝。`make host-test` 与 `make host-test-san`
  全绿；`make KERNEL_DIR=<cached headers> all` 通过（本机缺 pahole 时
  BTF 步骤可能被环境挡住，代码本身无 warning）。
- `host/bench_sclc_uplink.c`：`make host-bench-sclc`。
- 实测（Ryzen 9 7940HS，单线程，payload MB/s）：

| 形状 | encode | decode p=0 | decode p=0.30 | decode p=0.40 |
|---|---:|---:|---:|---:|
| 8×32 1/4 (8+24) | 30.5 | 20.8 | 6.05 | 5.69 |
| 8×32 7/16 (14+18) | 57 | 37 | 11.1 | 11.6 |

## 5. 风险与下一步

| # | 风险 | 证据 | 缓解 |
|---|---|---|---|
| R1 | p=0.40 block 0.99–0.997，未稳定 0.999 | §0 表 | NACK/replay 后 byte-exact；若要 0.999 用全局 256-share 交错 |
| R2 | per-group 1/4 的 HND0 吞吐未测 | 本机 5.7 MB/s payload | HND0 2t 短跑；按码率折算约 5 MB/s，边界 |
| R3 | 全局交错需要 2 MiB block 与新的数据面接口 | host 原型 groups=1 | Phase 2；8 个 decoder 上下文仍可并行 |
| R4 | core solve 上限 256 变量 | 原型 | 超限 `skipped`，不写错字节；统计暴露 |
| R5 | 1/4 的 parity 占 75% wire | 参数 | 只在上行 p≥0.3 使用；低 p 用 7/16 或 32×8 |

## 6. 证据

- 代码：`tbtun/tbt_sc_ldpc.[ch]`、`tbtun/tbt_sc_ldpc_gf16.[ch]`、
  `tbtun/tbt_sclc_uplink.[ch]`。
- 测试：`host/test_sclc_uplink.c`、`host/bench_sclc_uplink.c`。
- 原型：`/home/kunweiz/tbtun-worktrees/sclc-gf16/review/fec-prototype/sc_ldpc/`。
- 本机多 seed 数字：本文 §0.3/§0.4；原始 CSV 未落盘（host 输出可复现，
  seed/block 数见测试代码）。
