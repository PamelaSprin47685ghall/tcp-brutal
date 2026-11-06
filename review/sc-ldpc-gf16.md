# GF(16) 4-bit SC-IRA：p=0.40 的 core solve 与 binary block512 对比

日期 2026-11-06。代码：`review/fec-prototype/sc_ldpc/sc_ldpc_gf16.{c,h}`、
`sc_ldpc_gf16_bench.c`、`build_gf16.sh`。数据：`review/sc-ldpc-gf16.csv`。
本文只回答一件事：**在 p=0.40、rate≤0.60 的上行场景，GF(16) 4-bit 码
能不能修 binary 的坏 seed/stopping set，并在恢复率、CPU、吞吐上给出
相对 binary block512 的实证优势。**

## 0. 结论

1. **“4-bit 码”在仓库里没有定义。** 五份 SC-LDPC 文档与
   `review/fec-prototype/sc_ldpc/` 源码中，`GF(16)`/`4-bit`/`q-ary`/
   `non-binary`/`quantiz` 全部零命中。仓库里唯一的 GF(16) 是 legacy
   滑动窗的 `SLIDING_L2_RS4`（GF(2⁴) 柯西矩阵 RS，`n+m≤15`），与
   SC-LDPC 无关。三种可能读法：GF(16) 符号码（本原型）、4-bit LLR
   量化（BEC peeling 是硬判决，不适用）、4-bit packing（wire 格式，
   不改变恢复）。本文按 GF(16) 符号码实现。
2. **仅换字母表、仍用 peeling，不能修 p=0.40。** GF(16) peeling 与
   binary peeling 在同图、同 seed、同擦除 mask 下的已解集合逐点相同
   （`--cross-check`，200 trial，0 mismatch）。要修 stopping set，必须
   在 peeling 停住后做 **GF(16) 代数 core solve**：对未解核心做高斯
   消元，再验完整码字。
3. **坏 seed 39596 / p=0.40 / r=0.35 / block256 L16**：peeling
   **0.9903**（99/10000 失败）→ core solve **1.000000**（20k，0 失败）；
   203 次 core 全部解出，**203/203 是 GF(2) 秩亏、GF(16) 秩满**。这就是
   “4-bit 修 p=0.40”的直接机制。
4. **同 bit block（GF(16) block128 = binary block512 = 4096 payload bit），
   p=0.40：**
   - r=0.40：GF(16) 160k 失败 7 次，**0.999956**（99% 失败率 CI
     [1.7e-5, 1.1e-4]）；binary block512 1M **0.999168**
     （[7.6e-4, 9.1e-4]）。失败率约低 19×。
   - r=0.45：GF(16) 160k 失败 9 次，**0.999944**（[2.4e-5, 1.3e-4]）；
     binary block512 1M **0.998084**（1916 失败，ci99 0.000113）。
     **同 bit block 下，GF(16) 能在 r=0.45 过 0.999，binary 不能。**
   - r=0.50：GF(16) 160k **0.994306**，core 大量 `skipped`（core 达
     700 变量），同 bit block 不可行。block256 的 r=0.50 是 2× 码长，
     不能当作同 bit block 的优势。
5. **本机 CPU（Ryzen 9 7940HS，单线程，`-O3`，无 `-march=native`，
   5 seed 中位）：** GF(16) b128 r=0.40 decode **29.4**、encode
   **94.2 MB/s**；r=0.45 decode **34.4**、encode **112.2 MB/s**。
   同 bit block binary scalar b512 r=0.40 decode 7.07、encode 22.9；
   r=0.45 decode 8.41、encode 27.5。GF(16) 约 **4.1–4.5× decode、
   4× encode**。2 线程本地 1.92–1.94×（b128 r=0.45 decode 65.9）。
6. **对 binary bit-sliced：本地 i.i.d. GF(16) 更快，相关擦除下
   bit-sliced 更快；HND0 未测（T5.2 由 t52-256 独占）。**
   - 本地 i.i.d. b512 L16 p=0.40：bit-sliced 1t 20.56/16.92、
     2t 35.24/31.59 MB/s（r=0.40/0.45）；GF(16) b128 1t 29.4/34.4、
     2t 57.0/65.9，GF(16) 快 **1.4–2.1×**。
   - 本地 share 级相关擦除（app mode3）：bit-sliced 1t 107.2/127.8，
     GF(16) 29–34，bit-sliced 快 **3–4×**。
   - HND0 已测 bit-sliced i.i.d. p0.40/r0.40 2t 5.91 MB/s；GF(16)
     HND0 未测。按 binary scalar 的 HND0/local 比 6–8× 投影，GF(16)
     b128 r=0.45 单线程约 4.3–5.7、双线程约 6.6–10.8 MB/s；可能过
     4.215，也可能不过。**必须等 T5.2 结束后的 HND0 短跑才能判定。**
7. **判定：** GF(16) 的**恢复率/码率优势是实证的**（同 bit block，
   r=0.45 仍 0.999944，binary 0.998）；**相对 binary scalar 的 CPU
   优势也是实证的**；**相对 binary bit-sliced 的 HND0 吞吐优势未证明**。
   如果上行用 scalar b512，GF(16) b128 r=0.45 是明确的升级候选；如果
   上行用 bit-sliced，binary 已满足 33.72，GF(16) 只有在 HND0 实测
   ≥4.215 且需要 r=0.45 的恢复 margin 时才值得换。**没有 HND0 数据前
   不要切换生产路径。**

## 1. “4-bit 码”的三种读法

| 读法 | 含义 | 对 BEC SC-LDPC 的影响 |
|---|---|---|
| GF(16) 符号码 | 每符号 4 bit，q-ary BP/peeling | 码长按符号算；同 block/L 下 bit block ×4；可用 GF(16) 代数 core solve 修 stopping set |
| 4-bit LLR 量化 | 软判决消息位宽 | BEC peeling 没有 LLR；不适用。5G/Wi-Fi/SSD 的 “4-bit” 多指这个 |
| 4-bit packing | 每字节/ nibble 打包 | 只改 wire 格式，不改码率或恢复 |

仓库没有给出定义；本文采用第一种。外部文献里 q-ary LDPC 的 CPU 吞吐
（如 ARM 0.132 Mbit/s、x86 GF(64) 约 3 Mbit/s）是 **AWGN 软判决
FFT-SPA/EMS** 的数字，不是 BEC peeling；不能拿来否决本原型。本原型的
q-ary peeling 每边只做一次 16×16 表乘，复杂度远低于软判决 q-ary BP。

另一个数字歧义：`review/sc-ldpc-sim.md:157` 的 “11.2 MB/s” 是本机
binary **scalar** b256 L8 r0.50 p0.40 的 decode；sclc-bench 的
“11.2 MB/s” 是 **HND0 bit-sliced 2 线程** b256 L16 r0.50 p0.30 的
decode（11.202）。两者数值巧合相同，含义不同。

## 2. 构造与解码器

- **图**：完全复用 `SC_TERM_ACC`。payload 度 5 的环形窗口、parity 度 2
  的累加器、check 度 6–7；rate>0.50 缩 `n_base`，rate<0.50 追加
  `extra_dc=5` 的 extra check。GF(16) 只把每个变量/校验符号从 1 bit
  换成 4 bit，拓扑不变。
- **系数**：每条边独立随机取 GF(16) 非零系数；每个 check 的 owned
  parity 系数固定为 1。check 方程
  `Σ h_v·x_v = 0`（加法是 XOR）。编码仍是前向扫描：
  `x_owned = Σ_{v≠owned} h_v·x_v`，因为 owned 系数为 1。
- **解码**：q-ary peeling（BEC 上的 BP 特化）。check 的已知项和存在
  `cx` 低 4 bit，未知数存在高字节；`ucnt==1` 时
  `x_v = syn / h_v`。每边一次 16×16 表乘。
- **core solve**：peeling 队列空后，收集仍未解的变量 `U` 和活跃 check
  `C`，构造 `|C|×|U|` 的 GF(16) 矩阵与 RHS `syn`，做带主元的高斯消元。
  只有 `rank=|U|`、行一致、且整码字 `gf16_verify()` 通过才接受；否则
  回滚。默认上限 256 变量；超过则 `skipped`。同时算同一 0/1 关联矩阵的
  GF(2) 秩，用来解释 binary 为什么修不了。
- **恢复集合关系**：peeling 阶段 GF(16) 与 binary 的已解集合完全相同；
  core solve 只会增加已解变量。`--cross-check` 在 core-max=256 下要求
  `binary_only=0`（GF(16) 不能丢 binary 已解变量），`gf16_extra` 是
  4-bit 多解出的变量。b128 r=0.45、2000 trial：`binary_only=0,
  gf16_extra=8`。

## 3. 正确性

- `--payload-check`：block 16/L4 与 block 256/L16，4 个码实例，
  `verify=0 errors=0`；编码/解码 bit-exact。
- `--cross-check` core-max=0：p=0.30/r=0.50 与 p=0.50/r=0.30 各 200
  trial，`mismatches=0`；GF(16) peeling 与 binary 同图同 mask 的已解
  集合逐点相同。
- `--cross-check` core-max=256：b128 r=0.45、2000 trial，
  `binary_only=0, gf16_extra=8`；core solve 不会破坏 binary 已解变量。
- 所有 trial 对每个已知变量与原始码字逐符号比较；`value_errors=0`。

## 4. p=0.40 结果

### 4.1 同 bit block：GF(16) b128 L16 vs binary b512 L16

两者 payload 都是 4096 bit（512 B），总码字都是 8192 bit；GF(16) 用
2048 个符号节点，binary 用 8192 个 bit 节点，边数约 1/4。

| 码 | rate | trials | 失败 | recovery | 99% 失败率 CI |
|---|---|---|---|---|---|
| GF(16) b128 | 0.40 | 160k（8 seed） | 7 | **0.999956** | [1.7e-5, 1.1e-4] |
| GF(16) b128 | 0.45 | 160k（8 seed） | 9 | **0.999944** | [2.4e-5, 1.3e-4] |
| GF(16) b128 | 0.50 | 160k（8 seed） | 911 | 0.994306 | [5.2e-3, 6.2e-3] |
| binary b512 | 0.40 | 1M（用户基线） | 832 | **0.999168** | [7.6e-4, 9.1e-4] |
| binary b512 | 0.45 | 1M（本机） | 1916 | **0.998084** | [1.8e-3, 2.0e-3] |

- r=0.40：GF(16) 的 99% 失败率上界 1.1e-4 低于 binary 的下界 7.6e-4，
  失败率约低 19×；这是同 bit block、同 p 的对比。
- r=0.45：GF(16) 仍 0.999944，binary 1M 只有 0.998084（1916 失败）。
  因此 **r=0.45 是 GF(16) 的净码率优势**（更高码率 + 更好恢复）。
- r=0.50：GF(16) 的 core 达 700 变量、`skipped` 100+，恢复掉到 0.994，
  同 bit block 不可行。block256 的 r=0.50（0.99966）是 2× 码长，
  binary 用 block1024 也能拿回一部分码长收益，不能直接算 GF(16) 优势。

### 4.2 2× bit block：GF(16) b256 L16

| rate | trials | 失败 | recovery | 备注 |
|---|---|---|---|---|
| 0.35 | 160k | 1 | 0.99999375 | 最稳，但码率低 |
| 0.40 | 160k | 4 | 0.999975 | core 4 次失败：3 欠定 + 1 秩亏 |
| 0.45 | 160k | 8 | 0.999950 | core 8 次失败：多为欠定/秩亏 |
| 0.50 | 160k | 54 | 0.9996625 | 2× 码长；core 大量 skipped，边界 |

b256 的 payload 是 binary b512 的 2×；它的恢复优势有一部分来自更长
的码，不应作为“同 bit block 优势”的主证据。它的价值是：如果 app 的
share 允许 1024 B/codeword，GF(16) b256 r=0.45 在 2× 码长下仍
0.99995，且本地 decode 33.1 MB/s。

### 4.3 坏 seed / stopping set

| 图 | rate | peeling | core solve | core 统计 |
|---|---|---|---|---|
| seed 39596 b256 L16 | 0.35 | 0.9903（99/10k） | **1.000000（0/20k）** | 203 calls，203 solved；203/203 GF(2) 秩亏、GF(16) 满秩；vars_max=7 |
| seed 23758 b256 L16 | 0.40 | 0.9989（22/20k） | 0.99995（1/20k） | 22 calls，21 solved；1 次 `nr<nu` 欠定 |
| seed 31677 b256 L16 | 0.40 | 0.9999（1/10k） | 0.99995（1/20k） | 1 call，1 次 `nr<nu` 欠定 |
| seed 39596 b256 L16 | 0.40 | 0.9998（2/10k） | 0.99995（1/20k） | 6 calls，5 solved；1 次 GF(16) 秩亏 |
| seed 47515 b256 L16 | 0.40 | 0.9995（5/10k） | 0.99995（1/20k） | 6 calls，5 solved；1 次 `nr<nu` 欠定 |
| seed 55434 b256 L16 | 0.35 | 0.99995（1/10k） | 0.99995（1/20k） | 3 calls，2 solved；1 次 GF(16) 秩亏 |

**残差失败分类**：
- `nr<nu`（活跃 check 少于未解变量）：结构性欠定，换字母表或换系数
  seed 都修不了；只能换图/seed 或加 check。
- `rank<nu`（GF(16) 秩亏）：同一图换系数 seed 可能修；本次没有做
  coefficient-seed 搜索。
- `bad`（行不一致或整码字 verify 失败）：本次 0 次。
- 同 bit block r=0.40 的 7 次失败里，3 次是 `rankdef`，4 次是
  `under`；r=0.45 的 9 次失败里，5 次是 `rankdef`，4 次是 `under`。
  残差率 4.4e-5–5.6e-5，仍远低于 0.999 门槛。

### 4.4 为什么 binary 修不了

binary 的 GF(2) 核心矩阵在坏 seed 上秩亏；GF(16) 给同一 0/1 关联矩阵
配上随机非零系数后，行列式/秩以高概率变为满秩。seed 39596/r=0.35 的
203 个 core 全部属于这一类：GF(2) 秩 < 变量数，GF(16) 秩 = 变量数。
binary 即使做 GF(2) 高斯消元也修不了；GF(16) core solve 可以。这就是
4-bit 的净收益，不是“换字母表让 peeling 更强”。

## 5. 吞吐与 CPU

本机 Ryzen 9 7940HS，`taskset -c 0`/`0,1`，`nice -n 10`，
`-O3 -std=c99 -march=x86-64 -mtune=generic`，无 `-march=native`。
GF(16) 是 5 seed × `--bench-sec 0.20 --runs 5` 的中位；binary scalar 是
同批 0.20 s 的单次（其 bench 没有 runs 选项），但 5 次重复的波动 <10%。

| 码 | block | rate | 1t decode | 1t encode | 2t decode |
|---|---|---|---|---|---|
| GF(16) | 128 | 0.40 | **29.4** | **94.2** | 57.0 |
| GF(16) | 128 | 0.45 | **34.4** | **112.2** | 65.9 |
| GF(16) | 256 | 0.40 | 28.4 | 93.5 | — |
| GF(16) | 256 | 0.45 | 33.1 | 109.7 | — |
| binary scalar | 512 | 0.40 | 7.07 | 22.9 | — |
| binary scalar | 512 | 0.45 | 8.41 | 27.5 | — |

单位 MB/s（payload）。GF(16) 相对 binary scalar 同 bit block：
decode 4.1–4.5×、encode 4.1×。2 线程本地 1.92–1.94×。

**注意固定 mask 方差**：同一 b128/r=0.40 在不同 mask 下见过 14.8 与
30.0 MB/s；14.8 是单 mask 离群值。本文用 5 seed 中位，binary 也应在
同一口径下比较。

**同机 bit-sliced 对照**（w-sclc-simd 的 `sc_ldpc_bs_bench`，b512 L16
p=0.40 batch512，本地 Ryzen 9 7940HS）：

| 模式 | rate | 1t | 2t |
|---|---|---|---|
| bit-sliced i.i.d. | 0.40 | 20.56 | 35.24 |
| bit-sliced i.i.d. | 0.45 | 16.92 | 31.59 |
| bit-sliced corr（share 级） | 0.40 | 107.22 | — |
| bit-sliced corr（share 级） | 0.45 | 127.80 | — |
| GF(16) b128 | 0.40 | 29.4 | 57.0 |
| GF(16) b128 | 0.45 | 34.4 | 65.9 |

i.i.d. 下 GF(16) 比 bit-sliced 快 1.4–2.1×；相关擦除下 bit-sliced 快
3–4×。**app mode3 的真实图样是 share 级相关擦除**，所以生产吞吐对比
不能只看 i.i.d.；GF(16) 目前没有相关批处理模式，这是它的吞吐短板。

**与 binary bit-sliced 的 HND0 数字**（sclc-bench 8c64ccf A/B）：

| 点 | 1t decode | 2t decode |
|---|---|---|
| p0.30/r0.50 b256 L16 | 8.47 | 11.202 |
| p0.40/r0.40 b256 L16 | 5.766 | 5.91 |
| p0.50/r0.30 b256 L16 | 4.062 | 5.803 |
| p0.50/r0.25 b256 L16 | 4.006 | 4.003 |

binary bit-sliced 的 HND0 encoder e2e 是 105–217 MB/s，远高于 GF(16)
scalar encode 的 HND0 投影（见下）。

**HND0 投影**：binary scalar 的 HND0/local 比约 6–8×（p0.40/r0.40：
local 7.07，HND0 0.911–1.059）。GF(16) b128 r=0.45 local 34.4：
- 1 线程：34.4 / 8…6 = **4.3–5.7 MB/s**；
- 2 线程（按 HND0 1.0–1.7× 扩展）：**4.3–9.7 MB/s**；若按本机 1.9×，
  8.2–10.8。
GF(16) b128 r=0.40 local 29.4：1t 3.7–4.9；2t 3.7–8.3。
这些是**投影，不是验收**；GF(16) 的 q-ary 表乘和随机内存访问可能让
HND0/local 比比 binary scalar 更差，也可能因为每 bit 边数只有 1/4 而
更好。必须实测。

## 6. HND0 计划（T5.2 已 GO，HND0 由 t52-256 独占；本轮不跑）

前置：T5.2 256 WAN 已 GO，HND0 2 vCPU 归 t52-256；本原型**不碰
HND0**，不跑 tbtun/256，不 push。下列命令留待 T5.2 结束、parent 明确
释放 HND0 后再执行。

```sh
# 本地打包
scp review/fec-prototype/sc_ldpc/{sc_ldpc.c,sc_ldpc.h,sc_ldpc_gf16.c,
    sc_ldpc_gf16.h,sc_ldpc_gf16_bench.c,build_gf16.sh} \
    root@45.11.1.62:/tmp/sc_gf16/
ssh root@45.11.1.62 'cd /tmp/sc_gf16 && sh build_gf16.sh'

# 关键点：同 bit block，p=0.40，20k trial，core-max 256
for R in 0.40 0.45; do
  taskset -c 0 nice -n 10 ./sc_ldpc_gf16_bench --block 128 --L 16 \
    --rate $R --p 0.40 --trials 20000 --model symmetric --codes 4 \
    --threads 1 --core-max 256 --bench-sec 0.20 --runs 5 --no-header
  taskset -c 0,1 nice -n 10 ./sc_ldpc_gf16_bench --block 128 --L 16 \
    --rate $R --p 0.40 --trials 20000 --model symmetric --codes 4 \
    --threads 2 --core-max 256 --bench-sec 0.20 --runs 5 --no-header
done
```

同时跑 b256 L16 r=0.40/0.45（2× 码长选项）和 binary bit-sliced
p0.40/r0.40 b256/b512 的同批基线，交替执行，记录 `/proc/loadavg`。
每点 5 轮取中位。

**验收门**：p=0.40、b128 L16、r=0.45、2 线程 decode **≥4.215 MB/s**
（留 20% 余量则 ≥5 MB/s）；1 线程也报，用于判断是否必须 2 线程。
若 2 线程仍 <4.215：给出最大可达值和瓶颈（q-ary peeling 的随机内存
访问 + 16×16 表乘；core solve 只在失败时跑，不是瓶颈），不硬推。
若 ≥4.215：GF(16) b128 r=0.45 同时具备更高码率、0.999944 恢复和
可用的 HND0 吞吐，可作为上行候选；仍需与 binary bit-sliced 的
恢复 margin/复杂度做产品取舍。

## 7. 判定

- **恢复率/码率：GF(16) 赢。** 同 bit block、p=0.40：
  r=0.40 0.999956 vs binary 1M 0.999168；r=0.45 0.999944 vs binary
  0.998084。坏 seed 39596 从 0.9903 修到 1.000000，203/203 core
  是 GF(2) 秩亏、GF(16) 满秩。这是 binary 即使做 GF(2) 消元也拿不到
  的恢复率。
- **CPU：相对 binary scalar 赢。** 同 bit block decode 4.1–4.5×、
  encode 4×；2 线程本地 1.92×。
- **吞吐：相对 binary bit-sliced 未证明。** bit-sliced HND0
  p0.40/r0.40 2t 5.91 MB/s；GF(16) HND0 投影 4.3–9.7 MB/s（2t），
  区间跨过 4.215。没有 HND0 实测就不能说 GF(16) 更快或更慢。
- **产品建议：** 如果上行用 scalar b512，或 i.i.d. bit-sliced，
  GF(16) b128 r=0.45 是明确升级候选（更高码率、更好恢复、
  1.4–4× CPU）。如果上行用 share 级相关 bit-sliced，binary 已满足
  33.72 且吞吐快 3–4×；GF(16) 的净优势只剩 r=0.45 的恢复 margin 和
  更低 wire 开销，代价是 HND0 吞吐未知、q-ary 实现更复杂。**先跑
  HND0 短跑（i.i.d. 与相关两种口径），再决定是否切换；当前不 push、
  不 merge、不碰生产。**
- **若 HND0 不可行：** 最大可达值按投影约 4–6 MB/s（2t），瓶颈是
  q-ary peeling 的随机内存访问与表乘，不是 core solve；下一步是
  64 码字批处理/SIMD q-ary 解码器，或换更强宿主。不要靠调 core-max
  或降码率硬推——r=0.40 的码率已经比 binary 低，降码率只会更慢。

## 8. 复现

```sh
cd review/fec-prototype/sc_ldpc
./build_gf16.sh

# 正确性
./sc_ldpc_gf16_bench --block 16 --L 4 --rate 0.50 --p 0.10 --payload-check
./sc_ldpc_gf16_bench --block 128 --L 16 --rate 0.45 --p 0.40 \
  --trials 2000 --model symmetric --core-max 256 --cross-check

# 同 bit block 恢复（8 seed × 20k；seed 1 7920 15839 23758 31677 39596 47515 55434）
for S in 1 7920 15839 23758 31677 39596 47515 55434; do
  ./sc_ldpc_gf16_bench --block 128 --L 16 --rate 0.45 --p 0.40 \
    --trials 20000 --model symmetric --codes 1 --seed $S --threads 1 \
    --core-max 256 --bench-sec 0.02 --runs 1 --no-header
done

# 吞吐（5 seed 中位）
for S in 1 2 3 4 5; do
  taskset -c 0 nice -n 10 ./sc_ldpc_gf16_bench --block 128 --L 16 \
    --rate 0.45 --p 0.40 --trials 2000 --model symmetric --codes 1 \
    --seed $S --threads 1 --core-max 256 --bench-sec 0.20 --runs 5 \
    --no-header
done
```

CSV 的 series：`gf16-b128-core-0.40`/`-0.45`/`-0.50`（同 bit block）、
`gf16-core-0.40`/`-0.35`/`-0.45`/`-0.50`（2× bit block）、
`gf16-seed40`（坏 seed 定位）、`gf16-thr5`（吞吐）。`# core ...` 行是
core solve 统计，不在 CSV 里。
