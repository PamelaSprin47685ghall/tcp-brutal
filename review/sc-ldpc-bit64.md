# SC-LDPC 64 码字位平面布局与 bit-sliced 编码器

日期 2026-11-06。代码：`review/fec-prototype/sc_ldpc/{sc_enc64.h,sc_enc64.c,
sc_enc64_pack.c,sc_enc64_sse2.c,sc_enc64_test.c,sc_enc64_bench.c}`。
图访问接口：`sc_ldpc.h` 的 `sc_get_graph()`（`sc_graph` 带 `owned`）。

本文只回答三件事：位平面怎么摆；编码器为什么一次前向扫描就正确；
解码器合并时哪些状态必须按码字保留。实测数字见 §6。

## 0. 结论

1. 64 个码字共享一套位平面：每个变量一个 `uint64_t`，bit b 是码字 b 的
   该变量值。一次批编码的 XOR 次数是 `edges - n_chk`，与码字数无关。
2. 编码仍是 SC-IRA 的前向代入：check ck 只 XOR 除自己拥有的 parity 变量
   之外的邻居，再写回该 parity 变量。构造保证 parity 变量只被更晚的
   check 读取，所以一遍扫描精确等于逐码字 `sc_encode`。
3. 与标量 `sc_encode` 逐码字 bit-exact：12 组参数 × 5 种 payload ×
   64 码字 = **3840/3840**；SSE2 2×64 两条 lane 在 12 组参数上各
   64/64，见 `sc_enc64_test`。
4. SSE2 只做 128-bit XOR（2×64），不用 AVX；HND0 的 qemu64 有 SSE2。

## 1. 位平面布局

一批是 64 个互相独立的码字。变量编号与 `sc_ldpc.h` 完全一致：

| 区域 | 变量编号 | 字数 |
|---|---|---|
| payload | `0 .. n_data-1` | `n_data = L*m` |
| base parity | `n_data .. n_data+n_base*m-1` | `n_base*m` |
| extra parity (rate<0.50) | `n_data+n_base*m .. n-1` | `n_extra` |

check 拥有哪个 parity 变量由 lifting 置换决定：块 a 的 parity 变量
`n_data + a*m + b` 只属于块 a 内的一条对角边，归属记在
`sc_graph.owned[ck]`，**不是** `n_data + ck`。额外 check
`ck = n_base*m+j` 拥有 `n_data + n_base*m + j`。位平面数组 `v[n]` 中，
`v[x]` 的 bit b 就是变量 x 在码字 b 的值。内存 `8*n` 字节；payload 区在
入口由调用者填好，parity 区在出口有效，payload 区不被改写。

按位看，这把 64×n 的比特矩阵转置成 n 个 64-bit 列。所有编码运算都是
`v[p] = XOR v[a_i]`，没有进位、移位、掩码或按码字分支。

### 1.1 字节位序与 wire 打包

wire 上的 payload 是每码字 `n_data/8` 字节、字节内 LSB first：码字 b 的
bit k 在 `in[b*(n_data/8) + k/8]` 的 bit `k%8`。`sc_pack64()` 把 64 个
码字的这种字节流转成 `v[0..n_data-1]`，`sc_unpack64()` 把 parity 平面
转回同样的字节流。两者都用 8×8 位矩阵转置（三次掩码交换），代价约
3 条指令/字节，见 §4。

### 1.2 SSE2 2×64

`sc_enc128_run_sse2()` 用 `n` 个 16 字节对齐的 128-bit 字：低 64 位是
码字 0..63 的平面，高 64 位是码字 64..127 的平面。同一份 check 计划、
同一遍扫描，每条指令处理 128 位。两条 lane 是两批独立码字，因此不改变
任何语义；调用者把两批 payload 分别装进两条 lane 即可。

## 2. 编码器

### 2.1 计划

`sc_enc64_build()` 把图编译成与 `sclc-algo` 345682d 解码器 fast 布局
同形的数组：固定 stride、uint16 变量编号、check 顺序与变量顺序不变。

```
dcs                      最大 check 度
cadj[ck*dcs + k]         check ck 的完整邻接；owned parity 放在行尾
cdeg[ck]                 check 度；XOR 表是 [0, cdeg-1)
owned[ck]                check ck 要解的 parity 变量
pair[ck]                 ck 与 ck+1 互相不读对方的 parity 时置 1
```

每一行是解码器 `cadj_f[ck*dcs + k]` 行的一个置换（owned 挪到行尾），
所以解码器可以直接吃这个数组；stride、索引宽度、check/变量编号都与
345682d 一致。`sc_enc64_build()` 会验证 owned 恰好出现在邻接表里一次、
每个 parity 变量恰好被一个 check 拥有，否则拒绝。`n >= 65536` 的码
超出 uint16，直接拒绝。计划只依赖图，不依赖 payload，可以跨批复用；
计划本身不引用 `sc_code`，码对象可以先释放。

### 2.2 一遍扫描为什么正确

构造（`SC_TERM_ACC`）里，块 a 的 base parity 变量 `p` 恰好连两个 check：
块 a 内拥有它的那个 check（由 lifting 置换决定，记在 `owned[]`），以及
块 a+1 的对应 check。拥有它的 check 的 XOR 表不含 `p` 本身，所以它读到
的 parity 只有块 `a-1` 的（以及更早的），这些在扫描到块 `a` 时已经写好。
最后一个 base parity 只连自己的 check。额外 check 的 parity 变量度数为
1，只被自己读取。因此 check 编号 `0 .. n_chk-1` 的升序就是拓扑序，
一遍前向扫描精确。`sc_enc64_build()` 在计划编译时验证这个性质：每个
parity 读取必须来自更早的 check，每个 parity 变量必须恰好被一个 check
拥有；不满足就拒绝，不会静默算错。

`pair` 只是调度优化：相邻两个 check 若互不读取对方的 parity 变量，就可以
把两个 64-bit 累加器同时保持在寄存器里，隐藏 XOR 依赖链；写回顺序仍是
先两个累加、后两个 store，语义不变。计划编译时用邻接表判定这个条件，
不假设块结构，所以 rate 0.60/0.70 的 `n_base < L` 环形窗口也适用。

### 2.3 代价

每批 XOR 次数 `xors = edges - n_chk`，每批 payload `8*n_data` 字节。
rate 0.50/0.40/0.30、block 256/512、L=16 的实测计划规模：

| block | L | rate | n_data | n_chk | xors | XOR/byte |
|---|---|---|---|---|---|---|
| 256 | 16 | 0.50 | 2048 | 2048 | 12160 | 0.74 |
| 256 | 16 | 0.40 | 2048 | 3072 | 16640 | 1.02 |
| 256 | 16 | 0.30 | 2048 | 4779 | 24106 | 1.47 |
| 512 | 16 | 0.50 | 4096 | 4096 | 24320 | 0.74 |
| 512 | 16 | 0.40 | 4096 | 6144 | 33280 | 1.02 |
| 512 | 16 | 0.30 | 4096 | 9557 | 48210 | 1.47 |

`xors` 与批大小（64 或 128 码字）无关；码字越多，摊到每比特的指令越少。
这就是相对逐码字 `sc_encode` 的 64×/128× 来源。

## 3. API

```c
sc_enc64 *e = sc_enc64_build(c, err, sizeof err);  /* 一次 */
sc_enc64_run(e, v);                                /* 每批，v[n] */
sc_enc128_run_sse2(e, w);                          /* 每批 128 码字，w[n] __m128i */
sc_pack64(v, wire_bytes, n_data);                  /* 入口 */
sc_unpack64(wire_bytes, v + n_data, n_par);        /* 出口，注意 offset */
```

`sc_enc128_run_sse2()` 在无 SSE2 的构建里返回 -1；调用者应回退到标量。
`v` 必须是 16 字节对齐（SSE2 路径用对齐 load/store）。`n < 2^31`、
`n_data % 8 == 0` 是当前实现的隐含前提（`sc_enc64_build()` 不检查后者，
由构造保证：`n_data = L*m`，`m = block/2 >= 8`）。

计划编译只消费 `sc_ldpc.h` 的只读图视图 `sc_get_graph()`：

```c
typedef struct {
    int n, n_data, n_par, n_chk;
    long edges;
    int dc_max;
    const int *voff, *coff, *vadj, *cadj;
    const int *owned;   /* n_chk：每个 check 解的 parity 变量，-1 表示无 */
} sc_graph;
void sc_get_graph(const sc_code *c, sc_graph *out);
```

`owned` 是本编码器唯一比解码器多要的字段；合并 `sc_ldpc_bs` 时把它加进
`sc_graph` 与 `sc_get_graph()` 即可（解码器不读它，不影响 `sc_decode_bs`）。

## 4. 打包代价

`sc_pack64()`/`sc_unpack64()` 每个 64 字节列做 8 次 8×8 转置加 8 次字节
拼接。`sc_enc64_bench --` 的 `e2e_mbps` 指标把 pack + 编码 + unpack 全部
计入；只测内核的 `enc64_mbps` 是编码器本身。两者都报，因为 app 的 wire
吞吐必须付打包成本。

## 5. 与解码器合并

编码器不约束解码器内部状态，但下面几项是共享契约：

1. **值平面**：64 码字一批，变量编号同上，bit b = 码字 b。解码器解出的
   值也放回同一方向，才能直接复用 `sc_unpack64()`。
2. **擦除与已知**：每个变量两个 64-bit 平面——`unk[v]`（bit b = 码字 b
   仍未知）和 `val[v]`（已知值；未知 lane 的内容在被解出前无意义）。
   check 侧维护 `x[ck]`（已知邻居值的 64-bit XOR）。
3. **每 check 的未决计数必须按码字保留**：BEC peeling 只在"该 check 在
   该码字只剩一个未知邻居"时推进。`x[ck]` 和 `unk[v]` 都是 64-bit 平面，
   但计数不是；两个可选表示：
   - 3 个饱和 bit-plane（0/1/>1 足够），每条边增删各约 6–8 条位运算；
   - 64 个 8-bit lane 计数（每个 uint64 装 8 个码字），按 8 码字一组
     推进，XOR 仍用 64-bit 值平面。
4. **单未知的识别**：对 check ck 先算 `eq1[ck]`（该 check 恰好一个未知
   邻居的码字掩码），然后对每条边 (v,ck) 测 `unk[v] & eq1[ck]`；命中的
   码字里 v 就是那个唯一未知。解出后把 `val[v]` 的对应位置为
   `x[ck]`，清 `unk[v]`，再对 ck 的其它邻居减计数。
5. **累加器结构不变**：系统模型里 parity 全部已知，不需要解 parity；
   symmetric 模型里 parity 与 payload 一样参与 peeling，布局相同。
   rate<0.50 的额外 check 度数为 `extra_dc+1`，rate>0.50 的
   `n_base < L` 环形窗口在解码侧也只是邻接表，不改变位平面。

### 5.1 与 `sc_ldpc_bs` 的桥

兄弟分支的位切片解码器 `sc_ldpc_bs` 用 **codeword-major、位打包、LSB
first** 的线上布局（码字 j 起于 `bits[j*stride]`，变量 v 在字节 `v/8`、
位 `v%8`），与本编码器的 **variable-major 位平面**正好互为转置：

```
sc_unpack64(planes, bytes, n_bits)   →  sc_ldpc_bs 的 bits/erased/out 布局
sc_pack64(planes, bytes, n_bits)     ←  接收到的 share 字节
```

两者不能直接互喂：把 `v[]` 当 `bits` 传进 `sc_decode_bs` 会静默解错。
`sc_enc64_test.c` 的 `pack_ref` 和 `sc_enc128_test.c` 是可执行的布局契约。

### 5.1.1 check 邻接与 345682d 对齐

编码器的计划数组就是 345682d fast 布局的 check 侧：
`cadj[ck*dcs + k]`（uint16、固定 stride `dcs`、check 与变量编号不变）加
`cdeg[ck]`。唯一差别是每行把 owned parity 放在行尾；解码器只按行扫描，
行内顺序无关，所以合并时可以直接把 `sc_enc64` 的 `cadj`/`cdeg` 当作
`cadj_f`/`cdeg_f` 使用（或反之）。`pay_adj`/`par_adj`/`par_deg` 是变量
侧，编码器不需要；`payload-only` 写回与 `known` 按需生成同样是解码器
自己的事。

已做逐 check 的交叉验证：在 `345682d` 与本计划上分别构建
block 256/L16、seed 7 的码，把每行邻接排序后比较，rate 0.50/0.60/0.70
的 `dcs`、`cdeg` 和每行变量集合**逐 check 完全相同**（只有 owned 的
行内位置不同）。rate 0.30 在 `345682d` 里 `fast=0`（payload 度不再是
5，走 int32 CSR 回退），所以没有可共享的 fast 数组；编码器仍用固定
stride，节点编号与 check 顺序一致，但合并时那一段只能共用 CSR。

### 5.2 线上映射（未定义，合并前必须钉死）

app 的一个 8 KiB share（`S = 8192`）在 block 256/L16 下等于 **32 个
码字**（每个码字 payload 256 B）；一批 64 码字正好覆盖 **2 个 share**，
SSE2 的 128 码字覆盖 4 个。`sc_unpack64()` 的输出是「码字连续、LSB
first」的字节，可以按 share 切 8192 B 装进现有 PARITY 记录；但
「share ↔ 码字区间」和「parity 行如何从位平面切出」在
`review/fec-design.md`、`review/sc-ldpc-sim.md`、`review/sc-ldpc-hnd0.md`
里都没有定义。编码器本身不依赖这个映射，但接线前必须定死一种读法
（推荐码字连续：一个 share = 32 个码字的 parity 字节）。

## 6. HND0 实测

本机 Ryzen 9 7940HS（同二进制、同口径，仅供对照；测量时机器上有其他
任务，绝对值有噪声，比例关系稳定）：

| block | L | rate | scalar 64 码字 | enc64 内核 | e2e | SSE2 128 码字 |
|---|---|---|---|---|---|---|
| 256 | 16 | 0.50 | 22.5 | 3979 | 741 | 7314 |
| 256 | 16 | 0.40 | 23.4 | 2815 | 592 | 5179 |
| 256 | 16 | 0.30 | 15.4 | 1898 | 439 | 3515 |

单位 MB/s（payload），`--runs 5` 中位数，单线程。逐码字 `sc_encode`
在同批次上只有 15–23 MB/s；位平面把它放大到 1.9–4.0 GB/s（内核），
打包后仍有 0.44–0.74 GB/s。2 线程（`taskset -c 0,1 --threads 2`）聚合
是 7874 / 5508 / 3701 MB/s，为单线程的 1.95–1.98 倍。SSE2 一列是
128 码字批的 payload 吞吐，和 `enc64` 比每码字要除以 2（约 1.8–1.9×）。

瓶颈（Ryzen 9 7940HS，`perf_event_open`，block 256/L16，单线程、
taskset 单核、每点约 2 s）：

| rate | 内核 | cycles/edge | IPC | L1D miss/edge | 指令/edge |
|---|---|---|---|---|---|
| 0.50 | 标量 | 1.662 | 4.78 | 0.098 | 7.9 |
| 0.50 | SSE2 | 1.833 | 5.07 | 0.152 | 9.3 |
| 0.30 | 标量 | 1.818 | 4.66 | 0.113 | 8.5 |
| 0.30 | SSE2 | 1.918 | 5.15 | 0.168 | 9.9 |

**不是 XOR 链**：链的理论下限是 1.0 cycle/edge，实测 1.66–1.92，且
IPC 4.7–5.2，没有卡在累加器上。**不是 LLC/DRAM**：generic
cache-miss 只有 1e-5–2e-5/edge，L1D miss 的 1e-4；工作集
（计划 67–139 KB 加 `v` 33–109 KB）全部落在 1 MiB L2 内。

真正的成本是 `v` 字 load 流的 **L1D miss（10–17%/edge）由 L2 服务**
的延迟，加上每 edge 的索引/地址/循环指令流（7.9–9.9 条/edge，约为
6-wide Zen 4 发射的 66–82%）。L1D miss 按 64 B/fill 折合 6.3–10.8
B/edge，和 `v` 的 8/16 B/edge 同量级，说明 miss 流由 `v` 主导；L2
带宽远未饱和，是延迟不是带宽。SSE2 的 `v` 大 2 倍、miss 率更高，
cycles/edge 反而略高，但一条指令处理 16 B，折合每码字 1.8–1.9×。
`pair[]` 的两条链在源码里是两段顺序归约，GCC 不交错，只靠乱序在边界
重叠；手工交错、展开和地址预算可以再压指令/edge，但当前余量已足够。

2 线程近线性（1.95–1.98×）说明两个实例之间没有共享写，瓶颈是每核的
L2 延迟与发射，不是共享缓存。

打包是 e2e 的大头：8×8 转置约 3 条指令/字节，把 4.0 GB/s 的内核压到
0.74 GB/s。因此 wire 口径必须用 `e2e_mbps`，不能用 `enc64_mbps`。

HND0（QEMU qemu64，2 vCPU，只有 SSE2，共享宿主）：

- bit-exact：`sc_enc64_test` 在 HND0 上 `RESULT: PASS`，**3840/3840**。
- 预跑（rate 0.50，1 线程，2 次 × 0.2 s，sclc-bench 同批编译）：
  `scalar_ref` 9.405、`enc64` 1211.96、`e2e` 182.26、`sse2` 1967.95
  MB/s。
- smoke 全 rate：`e2e` **105–217 MB/s**；wire 需求 8.4–14.1 MB/s，
  余量 7.5–25×。
- 完整 5 轮 × 1/2 线程矩阵由 sclc-bench 的 A/B harness 补跑，
  `taskset 0`/`0,1`、`nice -n 10`、逐次记录 load；原始行写进
  `review/sc-ldpc-hnd0-enc64.csv`。

结论：HND0 上编码不再是瓶颈。同一批 64 码字的 `e2e` 已比逐码字
`sc_encode` 快一个数量级以上（预跑 182.26 vs 9.405），瓶颈转到解码。

## 7. 复现

```sh
cd review/fec-prototype/sc_ldpc && ./build.sh
./sc_enc64_test                       # bit-exact 与 SSE2 lane 检查
./sc_enc64_bench --block 256 --L 16 --rate 0.50 --runs 5
```

HND0（QEMU qemu64，2 vCPU，只有 SSE2）：

```sh
ssh root@45.11.1.62 'mkdir -p /tmp/sclc_enc64'
scp sc_ldpc.c sc_ldpc.h sc_enc64.h sc_enc64_internal.h sc_enc64.c \
    sc_enc64_pack.c sc_enc64_sse2.c sc_enc64_test.c sc_enc64_bench.c \
    root@45.11.1.62:/tmp/sclc_enc64/
ssh root@45.11.1.62 'cd /tmp/sclc_enc64 && \
  cc -O3 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_enc64.c \
     sc_enc64_pack.c sc_enc64_sse2.c sc_enc64_test.c -lm -pthread \
     -o sc_enc64_test && \
  cc -O3 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_enc64.c \
     sc_enc64_pack.c sc_enc64_sse2.c sc_enc64_bench.c -lm -pthread \
     -o sc_enc64_bench'
ssh root@45.11.1.62 'cd /tmp/sclc_enc64 && \
  taskset -c 0 nice -n 10 ./sc_enc64_test'
ssh root@45.11.1.62 'cd /tmp/sclc_enc64 && \
  taskset -c 0 nice -n 10 ./sc_enc64_bench --block 256 --L 16 \
    --rate 0.50 --seconds 0.20 --runs 5'          # 单线程
ssh root@45.11.1.62 'cd /tmp/sclc_enc64 && \
  taskset -c 0,1 nice -n 10 ./sc_enc64_bench --block 256 --L 16 \
    --rate 0.50 --seconds 0.20 --runs 5 --threads 2'   # 2 线程
```

三个 rate 各跑一遍；`--runs 5` 取中位数。HND0 是共享宿主上的 QEMU，
同一点的绝对值可漂 ±40%，所以结论按整个 rate 范围看，不按单点。

## 8. 限制

- 支持前向可解的构造（`SC_TERM_ACC` 与 `SC_TERM_CAUSAL`）；native
  拒绝。计划编译时会验证前向顺序，不满足的图直接报错。
- 计划用 `uint16_t` 变量编号（与 345682d 的 fast 布局一致）；
  `n >= 65536` 或 check 度 > 255 的码直接拒绝，需要 int32 CSR 回退。
- SSE2 路径要求 16 字节对齐；不提供 AVX/AVX-512 版本（HND0 没有）。
- 打包只定义了 LSB-first 的字节位序；app 若用别的位序，只需替换
  `sc_pack64()`/`sc_unpack64()`，编码器与布局不变。
