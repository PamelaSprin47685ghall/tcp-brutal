# 滑窗 RLNC 开源实现与 1G/MIPS 可行性

状态：调研稿。范围只限滑窗 RLNC；RS、RaptorQ、product/SC-LDPC 不在本文。
基线：`master 0db5166`；**生产目标是 HND0：256 流 / 物理 1G / 五元组限速严重，grouped 32×8，app 500–750M；HND0 只有 2 vCPU / 955 MiB，CPU/RAM 是风险**。`.65`（RK3588 ARM64）和 `.74`（x86_64）是 lab，不是生产。
证据标记：`[实测]` 官方公开数据；`[源码]` 直接读到的代码/接口；`[推算]` 由操作数模型或架构比例推出，未在目标机实测；`[待测]` 必须上机。

**一句话**：生产对端是 **HND0：256 流 / 物理 1G / 五元组限速严重，grouped 32×8，app 500–750M；2 vCPU / 955 MiB 是风险**。`.65`（RK3588 ARM64）与 `.74`（x86_64）是 lab。Kodo Slide 的 API 支持可变窗口/码率，但 N/M 不是 codec 参数；Kodo 代码因研究许可证不可用，只能 clean-room 重写；MIPS/ARM64 的 lab 结论保留。

## 0. 结论摘要

| 问题 | 结论 |
|---|---|
| **生产目标** | **HND0：256 流 / 物理 1G / 五元组限速严重；grouped 32×8，app 500–750M。** HND0 只有 **2 vCPU / 955 MiB**，CPU/RAM 是风险。`.65`/`.74` 是 lab。 |
| Kodo 的滑窗实现现在能拿到源码吗 | **不能**。`steinwurf/kodo`、`kodo-rlnc`、`kodo-slide`、`kodo-core` 均 404；公开的只有 `kodo-slide-c`/`kodo-slide-go` 绑定和 2015 年的旧 Kodo 快照。 |
| Kodo 许可证 | **Steinwurf Research License 1.0/1.2**：仅研究/教育，禁止商业使用、禁止内部业务使用、禁止再分发/再许可，1.2 版有效期 6 个月。不是开源许可证，不能进 tbtun，不能复制代码。 |
| 可变码率 | **支持，但在应用层**：Kodo Slide 是 rateless 编码器，发多少 coded packet 由应用决定；测试里的 `rate_controller(n,k)` 就是码率控制器。 |
| 可变窗口 | **支持**：`kslide_encoder_set_window(lower, symbols)` / 解码器同名接口逐包设置；窗口可每包不同。 |
| 可变 N/M | **codec 没有 N/M 概念**。N 是 tbtun 的 carrier 映射，M 是应用每窗口发几个 parity；Kodo 只看到 stream symbol 和 window。动态 N/M 必须在 tbtun 的 epoch 调度层实现。 |
| sparse degree | **Slide 的 C API 不暴露 density/degree**；可以用自定义系数（`write_symbol`/`read_symbol` 直接传 coefficient vector）。旧 Kodo 2015 有 `sparse_uniform_generator::set_density(double)`，但那是 block/generation，不是 Slide。 |
| Kodo 1G 单核（x86 参考） | 官方 block RLNC 数据：i7-4770、K=100 时 enc 40 / dec 26.4 Gbps；K=500 时 11.2 / 7.2 Gbps。插值 K=256 约 **19 / 12.4 Gbps**；1G 只占单核 **5–8%**。`[实测]` + `[推算]` |
| arm64 1G（lab） | `.65` 是 RK3588（4×A76 2.3GHz + 4×A55 1.8GHz，NEON，7.73 GiB，kernel 6.1.172，1G eth0 单队列/IRQ CPU0，生产负载）：ARM64 构建/移植目标，**不是干净 benchmark**。`.74` 是 x86_64（Athlon X4 760K），非最终。Kodo arm64 估 K=256 约 3.8–6.4/2.5–4.1 Gbps，需实测。 |
| MIPS（lab） | 生产 HND0 是 x86，MIPS 不是生产目标。`192.168.123.254` = JDC-1/MT7621：1004Kc V2.15，4 VPE（2 核），880 MHz 级，无 MSA、无 FPU、440 MiB、内核 3.4.113、无编译器/headers。**MT7621 上 1G/RLNC 不可行**；W≤8（K≤4）只有 30–90 Mbps；1G 口是 L2 交换。详见 §4。 |
| 256 carriers（生产） | **当前代码硬限 16**：`tbtun/tbt_core.c:82` 的 `BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16)`、`tbtun/tbt_uapi.h:113` 的 u16 `path_ack_mask`。256 需要 v6 线格式（path_id u16、256-bit ack、约 64 B 头），不是配置。内存侧 1G/256 约 32 MiB 缓冲可容纳；HND0 2 vCPU 的 CPU 是主要风险。 |
| 1G 平台 | **HND0 生产：256 carriers / 物理 1G / app 500–750M。** `.65` RK3588 ARM64 是构建/移植 lab 目标但非干净 benchmark；`.74` x86_64 非最终。Kodo x86 K=256 约 19/12.4 Gbps；ARM64 估 3.8–6.4/2.5–4.1 Gbps，需实测。 |
| 测试向量 | **没有可直接拿的 golden coding vectors**。Kodo 的测试是随机自洽；badrlnc 有手写 mask/payload 向量；tetrys-rs 有 RFC 9407 头字段字节向量。必须自己冻结向量。 |

## 1. 调研对象与版本

| 项目 | commit / 版本 | 日期 | 许可证 | 语言 | 结论 |
|---|---|---|---|---|---|
| `steinwurf/kodo-slide-c` | `7750183` | 2021-06-02 | Steinwurf Research 1.0 | C++/C 绑定 | API/行为参考；不可用代码 |
| `steinwurf/kodo-slide-go` | `cf69cca` | 2021-06-02 | Steinwurf Research 1.0 | Go/cgo | 测试场景参考；不可用代码 |
| `msherif4/kodo`（2015 快照） | `a858002` | 2013-06-26 | Steinwurf Research 1.0 | C++11 | sparse generator/decoder 算法参考；不可用代码 |
| `ihatemyfcklife/badrlnc` | `ccb38a8` | 2026-09-27 | Apache-2.0 | Go | GF(2) 滑窗、256-bit mask、增量 Gauss-Jordan；算法参考；Apache-2.0 与 GPLv2 不兼容 |
| `gdanezis/tetrys-rs` | `67dca71` | 2026-03-04 | Apache-2.0 | Rust | RFC 9407 elastic window、GF(2^4/2^8)；不是 GF(2) sparse；算法参考 |
| `steinwurf/kodo-dev` | `a5c7382` | 2021 | Steinwurf Research 1.0 | waf 构建壳 | `resolve.json` 指向已 404 的私有仓库 |

补充事实：

- `kodo-slide-c` 的 `README.rst` 明确写「A valid Kodo license is required if you wish to use this project」；`wscript` 链接 `kodo_slide`、`fifi`、`cpuid`，核心实现不在公开仓库。`[源码]`
- `kodo-slide-c` 的 C 文件只是 `kodo_slide::encoder/decoder` 的薄包装；`src/kodo_slide_c/kodo_slide_c.cpp` 里只有 opaque struct 和转发。`[源码]`
- Kodo 当前许可证页面是 `https://www.steinwurf.com/kodo-research-license-1-2`；1.2 版第 2 条禁止商业部署、内部业务使用、再分发；第 10 条有效期 6 个月。`[源码/网页]`

## 2. Kodo Slide 的滑窗模型

### 2.1 对象与 API

Kodo Slide 把数据看成一条连续 symbol stream，编码窗口是 stream 上的半开区间 `[lower_bound, lower_bound + symbols)`。`[源码]`

| 操作 | C API | 语义 |
|---|---|---|
| 加源包 | `kslide_encoder_push_front_symbol(enc, data)` | 在 stream 前端追加一个源 symbol，返回 stream index |
| 淘汰最老 | `kslide_encoder_pop_back_symbol(enc)` | 移除最老 symbol，lower bound 前进 |
| 设窗口 | `kslide_encoder_set_window(enc, lower, symbols)` | 指定下一次编码覆盖的窗口；`symbols` 可逐包变化 |
| 设种子 | `kslide_encoder_set_seed(enc, seed)` | 64-bit seed；收发同 seed 生成同一组系数 |
| 生成系数 | `kslide_encoder_generate(enc, coef)` | 写入当前窗口的 coefficient vector |
| 编码 | `kslide_encoder_write_symbol(enc, sym, coef)` | 按给定系数写 coded symbol |
| 源包直写 | `kslide_encoder_write_source_symbol(enc, sym, index)` | systematic 路径 |
| 解码 | `kslide_decoder_read_symbol(dec, sym, coef)` | 就地解码；symbol/coef 都可被修改 |
| 源包直读 | `kslide_decoder_read_source_symbol(dec, sym, index)` | systematic 路径 |
| 状态 | `rank`、`symbols_missing`、`symbols_partially_decoded`、`symbols_decoded`、`is_symbol_decoded` | 解码器 rank/交付状态 |

对应源码：`kodo-slide-c/src/kodo_slide_c/kodo_slide_c.h` 第 219–306 行（encoder）、357–474 行（decoder）。`[源码]`

窗口语义：

```
window = [lower_bound, lower_bound + symbols)
stream = [stream_lower_bound, stream_upper_bound)
window 必须落在 stream 内；stream_symbols 是解码器可用的 symbol 总数（容量）。
```

### 2.2 可变码率、N/M、窗口：逐项回答

**窗口：支持，逐包可变。** `set_window` 在每次 encode/decode 前调用；`coefficient_vector_size()` 随当前窗口变化。测试 `test_kodo_slide_c.cpp` 的 `slide_window` 用例把窗口固定为 5，但每轮都重新 `set_window`；`basic_api` 用例把窗口设为整个 stream。`[源码]`

**码率：支持，但不在 codec 参数里。** Kodo Slide 没有 `set_rate`、`set_m`、`set_n`。它是 rateless：应用想发多少 coded packet 就调用多少次 `generate`/`write_symbol`。测试里的 `rate_controller(n,k)` 是唯一的码率机制：`k` 次迭代 push 源包，`n-k` 次迭代发 coded packet。`slide_window` 用 `(6,3)`，`TestCodec` 用 `(8,3)`。`[源码]`

**N/M：不是 codec 概念。** Kodo 只看到 symbol stream 和 window；哪些 symbol 是数据、哪些是 parity，由应用调用 `write_source_symbol` 还是 `write_symbol` 决定。N 是 tbtun 的 carrier 映射；M 是应用每个窗口发几个 parity。动态 N/M 不需要重配 codec，只需要改变调度和每包的窗口/系数。`[源码] + [推论]`

**seed 模式的前提：** 接收端必须知道 `(lower_bound, symbols)` 才能生成同一组系数；Kodo 的 C API 不定义 wire header，窗口几何必须由 tbtun 头携带。Kodo 的内置 PRNG 是私有的（未公开），不要试图与 Kodo 的 seed 兼容；tbtun 用 `splitmix64 + Fisher-Yates`。`[源码/推断]`

**sparse：Slide 不暴露 density/degree。** C API 的 factory 只有 `symbol_size` 和 `field`（`kslide_binary`、`binary4`、`binary8`、`binary16`）；`generate` 用内置 PRNG，没有 density 参数。若要固定 degree `d=16`，必须自己生成 coefficient vector，再传给 `write_symbol`/`read_symbol`；API 明确允许「custom coding scheme can be implemented by generating the coding vector manually」。`[源码]`

旧 Kodo 2015 的 sparse 路径（只作算法参考，不是 Slide）：

```cpp
// src/kodo/sparse_uniform_generator.hpp
class sparse_uniform_generator {
    void set_density(double density);   // Bernoulli 概率，不是固定 degree
    // 每个 coefficient 以 density 概率非零；GF(2) 时置 1
};
```

`benchmark/throughput/codes.hpp` 用 `sparse_full_rlnc_encoder<fifi::binary>` 把 `sparse_uniform_generator` 叠进编码栈；`benchmark/throughput/main.cpp` 的 density 选项是 0.1–0.5，symbols 16–128，symbol_size 1600。`[源码]` 这证明 Kodo 的 sparse 参数是 **density**，不是固定 degree；`d ≈ density × W`。对 `W=256, d=16`，density=0.0625。

### 2.3 解码算法

旧 Kodo 的 `linear_block_decoder` 是增量高斯消元：`[源码]`

- `decode_symbol(data, coefficients)` 进入 `decode_coefficients`；
- `forward_substitute_to_pivot` 从低位扫 coefficient，遇到已有 pivot 就 XOR 该 pivot 的 payload 和 coefficient；
- 找到 pivot 后（GF(2) 不需要 normalize），`forward_substitute_from_pivot` 消去高位；
- `backward_substitute` 把新 pivot 代回已有行；
- `m_rank`、`m_maximum_pivot` 维护 rank；`is_complete()` 判断 rank == symbols。

这个算法的热路径是 **payload XOR**：每碰到一个 pivot，就要 XOR `symbol_size` 字节。`[源码]` 对滑窗 `W=256, S=8 KiB`，最坏一次消元可能触碰 256 个 pivot，即 2 MiB payload XOR；这是 1G 下的主要风险。设计里的「只解 k 个缺失、k≤M」比这个便宜，应该作为 C 实现的主路径，而不是照搬 Kodo 的全窗口消元。

### 2.4 C++→C 内核移植边界

**不能直接移植的部分：**

- Kodo Slide 的核心是 C++14 模板层（`kodo_slide::encoder/decoder`），依赖 `fifi`、`sak`、`boost`、`cpuid`；C API 只是薄包装，链接 C++ 静态库。`[源码]`
- 旧 Kodo 2015 同样是 C++ 模板 + Boost + `sak::storage`；`linear_block_decoder` 用 `boost::optional`、`std::vector`。`[源码]`
- 内核态没有 C++ 运行时、STL、异常、Boost；不能把 Kodo 编进 `.ko`。

**可以独立重写的部分（约 800–1200 行 C）：**

| 模块 | 建议实现 |
|---|---|
| stream | `struct { u64 lower, upper; u16 cap; u8 *slots; }` 环形缓冲；push/pop |
| window | `struct { u64 lower; u16 len; }`；每包携带，不设全局窗口 |
| coefficient | `u64 bits`（grouped W≤12）+ `d` 个 index；GF(2) 系数按位存；单组 stress 才用 8×u64 |
| PRNG | `splitmix64(state)`；`state = nonce ^ epoch ^ row`；Fisher-Yates 选 d 个不重复 index |
| 编码 | `memset(p,0,S)`；对 d 个 index 做 `xor_into(p, data[i], S)` |
| 解码 | 只对缺失集合建 `k×k` GF(2) 矩阵；高斯消元后 `k²` 次 payload XOR；不要全窗口消元 |
| 状态 | `rank`、`missing`、`partially_decoded`、`decoded`；用于 procfs |
| 内存 | 隧道创建时 `kvmalloc`；fast path 零分配 |

**内核约束：**

- 无浮点、无 STL/Boost、无异常、无 per-packet `kmalloc`。
- 不用 `prandom`（状态不可控）；用 `splitmix64`。
- NEON/MSA 必须批量调用（每 8 KiB 包一次），不能每行一次；MIPS 没有内核 MSA API，见 §4。
- `skb` 可能非线性；先 `skb_copy_bits` 到线性 packet buffer，再做 XOR。
- 1G 下 `S=8 KiB` 的包率是 15.26k/s；每包一次 `crc32c` 可接受。

## 3. 生产目标 HND0（256/1G）与 lab 候选

### 3.0 生产目标：HND0（256 carriers / 物理 1G / grouped 32×8 / app 500–750M）

HND0 是最终对端，**256 流 / 物理 1G / 五元组限速严重**；`256/1G` 是生产，不是 lab。`[用户确认]` HND0 的 CPU/RAM 是风险：**2 vCPU / 955 MiB**（`review/wan-experiment/summary.md` 的 far 侧，x86_64，kernel 7.0.0-42-generic）。

| 项 | 生产值 |
|---|---|
| carriers | **256**（32 groups × 8 carriers） |
| 每组 | `N+M=8`，`K=N`，`W=2N`，`q=2` |
| 每组 M/d | `M=2/d=12`（750M）、`M=3/d=10`（625M）、`M=4/d=8`（500M）；p=30% 要 99.9% 用 `M=5/d=6`（375M） |
| app | **500–750 Mbit/s** |
| 物理口 | 1G；parity 占满物理口是设计的一部分 |
| 窗口 | 每组 12/10/8 包，约 25/21/17 ms；恢复 1–2 个窗口 |
| 内存 | FEC 3.0/2.5/2.0 MiB；256 carriers 的 socket 缓冲 32–64 MiB；955 MiB 可容纳，但 256 socket/slab/work item 要监控 |
| CPU 风险 | **2 vCPU**；256 TCP ≈ 89k pps + grouped 编码 1.0–2.0 GB/s + 解码；这是生产的主要风险，必须实测，不能先验保证 500–750M |
| 五元组 | 256 条独立五元组是为了绕/摊薄限速；单流约 3.9 Mbit/s；实际限速策略需实测确认 |

**结论**：HND0 生产按 `256 carriers + grouped 32×8 + app 500–750M` 设计；2 vCPU/955 MiB 是硬约束，CPU 先于内存成为瓶颈。`.65`/`.74` 是 lab，不是生产。

## 3. HND0 256/1G 预算与 lab 候选

### 3.1 Kodo 官方 benchmark（x86 参考；HND0 1G 也用得上）

条件：Debian 10，Dell Optiplex / Intel i7-4770 @3.4GHz，单线程，block `K × 1280 B`，systematic，10% packet loss，100 次平均；吞吐 = 原始 block 字节 / 编解码时间。`[实测]`

| K | 编码 Gbps | 解码 Gbps |
|---:|---:|---:|
| 16 | 88 | 56 |
| 50 | 64 | 44 |
| 100 | 40 | 26.4 |
| 500 | 11.2 | 7.2 |
| 1000 | 5.68 | 3.68 |

来源：`https://www.steinwurf.com/blog/benchmark-kodo`。这是 **block RLNC**，不是 Kodo Slide；Kodo Slide 没有公开 benchmark。字段未在博客写明；Kodo 2015 的 throughput benchmark 同时有 Binary/Binary8/Binary16/Prime2325，博客很可能用默认 GF(2^8)。GF(2) 只做 XOR，通常更快；所以这组数对 GF(2) sparse 是保守锚点。`[实测] + [推论]`

K=256 的插值（log-log，K=100→500）：

```
enc: 40 * (256/100)^(-0.791) ≈ 19.0 Gbps
dec: 26.4 * (256/100)^(-0.807) ≈ 12.4 Gbps
```

1G 占单核：`1/19 = 5.3%`（编码），`1/12.4 = 8.1%`（解码）。`[推算]`

**注意**：这是 1280 B symbol；`S=8 KiB` 的工作集是 `256×8 KiB = 2 MiB`，L2/L3 压力更大，吞吐可能降 20–50%。`[推算]` 但 1G 仍有 2–10× 余量（x86）。

### 3.2 HND0 生产配置：grouped 32×8（1G 物理口，app 500–750M）

当前生产设计是 **grouped 滑窗 RLNC**：`256 carriers = 32 groups × 8 carriers`；每组 `N+M=8`，`K=N`，`W=2N`，`q=2`；应用字节流按 `seq % 32` 条带化。`[源码: review/fec-design.md §3.8]`

| 每组 M | 每组 N | W | d | 总 N（32 组） | 总 parity | 码率 | app @1G | 编码 XOR/byte | 编码 MB/s |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 6 | 12 | 12 | 192 | 64 | 75.0% | 750M | 8.00 | 1000 |
| **3** | **5** | **10** | **10** | **160** | **96** | **62.5%** | **625M** | **12.0** | **1500** |
| **4** | **4** | **8** | **8** | **128** | **128** | **50.0%** | **500M** | **16.0** | **2000** |
| 5 | 3 | 6 | 6 | 96 | 160 | 37.5% | 375M | 20.0 | 2500 |

公式：`编码 MB/s = 125 × (M/N) × d × q`（q=2）；表按 1G line 计算，app 500–750M 时按比例降低。

**每组窗口/延迟/内存**：

| M | W | 每组 W×S | 32 组缓冲 | 每组窗口（1G line 3.9 MB/s） |
|---:|---:|---:|---:|---:|
| 2 | 12 | 96 KiB | 3.0 MiB | 25.2 ms |
| 3 | 10 | 80 KiB | 2.5 MiB | 21.0 ms |
| 4 | 8 | 64 KiB | 2.0 MiB | 16.8 ms |
| 5 | 6 | 48 KiB | 1.5 MiB | 12.6 ms |

**读数：**

- 编码成本 8–20 XOR/byte；1G 下 1.0–2.5 GB/s。ARM64 NEON 可行；HND0 2 vCPU 需实测；MT7621 仍不可行。
- app 750/625/500M 对应码率 75%/62.5%/50%；`M=4/d=8` 是 p≤20% 的安全档，`M=5/d=6` 是 p=30% 要 99.9% 的档。
- 每组 `W≤12`，系数位图 1×u64 就够；不再需要 512-bit 单组位图。
- **单组 `N+M=256, W=2N=448` 已被否决**：`d≈100–358`、25–89 XOR/byte、3–11 GB/s，arm64 也做不到。grouped 32×8 是 1G/256 的唯一现实路径。`[源码: review/fec-design.md §3.5/§3.8]`
- 旧稿的单组 `W=2N` 表已 superseded；不要把它当生产配置。

### 3.3 HND0 256 carriers 的资源

| 项 | 数值 | 说明 |
|---|---:|---|
| 每 carrier 速率 | 3.91 Mbit/s | 1G / 256 |
| 每 carrier BDP（RTT 218 ms） | 106 KiB | 3.91 Mbit/s × 0.218 s；每组窗口 21 ms（M=3），约 1.25 包/carrier |
| `SO_SNDBUF/RCVBUF` 设置 32 KiB | 128 KiB 实际/carrier | 内核翻倍 ×2（snd+rcv） |
| 256 carriers 缓冲 | **32 MiB** | 32 KiB 档 |
| 设置 64 KiB | 64 MiB | 更安全，RAM 足够时 |
| 固定开销 | ~1.4 MiB | tcp_sock/socket/HTTP parser/work item |
| FEC 数据缓冲 | ~2.5 MiB | 32 组 × W=10 × 8 KiB（M=3 档） |
| FEC parity rhs | ~0.75–1.0 MiB | 32 组 × M=3/4 × 8 KiB |
| 总内存 | **~37 MiB**（32 KiB 档） | 不含内核其他部分；955 MiB 可容纳 |

CPU（HND0 2 vCPU，1G，89k IP pps，256 carriers）：`[推算，未实测]`

- TCP 收发、256 条 flow 的 poll、HTTP 解析、grouped 编码 1.5 GB/s、解码，全部压在 **2 vCPU** 上；这是 HND0 生产的主要风险。
- 不要用单核百分比外推：HND0 是共享 2 vCPU，实际可用算力、 steal、调度延迟都未知；必须实测 500–750M app。
- 如果 CPU 打满：先降 app/减少 carriers，或把 poll 合并为 session 级扫描；不要假设 2 vCPU 一定够。

**256 carriers 的结论：**

- 内存可行（32–64 MiB）；不是瓶颈。
- **当前代码硬限 16 carriers**（`tbt_core.c:82` 的 `BUILD_BUG_ON`、`tbt_uapi.h:113` 的 u16 `path_ack_mask`）。256 carriers 需要 v6 线格式，不是配置；这是 fec-core 的前置条件。`[源码]`
- HND0 2 vCPU 是 CPU 风险，需实测；MT7621 不可行（§4）。
- lab 备选：128 carriers + 2 shares/carrier 可把 socket/work item 数减半，缓冲仍是 32 MiB；但一条 carrier 断 = 2 个 erasure，M 必须按 2× 故障数设计。生产 HND0 仍按 256 carriers/grouped 32×8。
- 256 carriers 的 `pump_lock` 串行点是主要风险；必须改成 per-carrier `w_tx`，否则 256 条流会排队。

### 3.4 1G 瓶颈与优化

| 瓶颈 | 证据 | 优化 |
|---|---|---|
| 全窗口消元的 payload XOR | Kodo `linear_block_decoder` 每个 pivot XOR S 字节；W=256 最坏 2 MiB/方程 | 只解 k 个缺失（k≤M）；k² 而不是 W² |
| dense 系数 | Kodo `uniform_generator` 每个 coefficient 都非零；W=256 平均 128 XOR/byte | sparse d=16；density=16/W |
| 每包拷贝 | Kodo Slide `read_symbol` 就地操作，但 C API 仍可能拷贝 | 零拷贝 ring；`skb_copy_bits` 只一次 |
| C++ 抽象 | 模板层、`sak::storage`、`boost::optional` | C 直接数组/位图 |
| 256 TCP 流 | 89k pps、256 sockets | per-carrier workqueue；RPS/RSS；TSO/GSO |
| MIPS 无 SIMD | 见 §4；1004Kc 无 MSA，32-bit 标量 | 不作为 1G 目标；MT7621 只做低目标 |

### 3.5 lab 候选 192.168.123.74：x86_64，不是 ARM64

`review/arm64-target.md`（`w-arm64-recon`，commit `ccafdeb`）只读探测结论：**它不是 ARM64，是一台低端 x86_64 对照机**，不能替代 ARM64 的 1G 验收。`[实测]`

| 项 | 值 |
|---|---|
| 主机/架构 | `760K`，`x86_64`，Manjaro，内核 `7.3.0-rc1-2-MANJARO` |
| CPU | AMD Athlon X4 760K（Richland，2013），4 核，BIOS 锁 1.8 GHz（标称 3.8/4.1 GHz 未发挥） |
| ISA | AVX1、SSE4.2、AES、PCLMUL；**无 AVX2/AVX-512**；无 NEON/SVE（x86 不存在） |
| 内存 | 19.5 GiB；256 carriers 的窗口缓冲不是约束 |
| 构建 | 无 `/lib/modules/.../build`、无 `source`，未装 `linux73-headers`；当前不能产出 `tbtun.ko`；gcc 16.2.1/make 4.4.1 可用 |
| 网口 | `enp2s0`，r8169，**1000 Mbps full**，MTU **1492**，PCIe 1.0 x1，单 rx/tx 队列，RPS 关闭 |
| 中断 | IRQ 46 全部落在 CPU2（累计 2.65 亿次）；单队列 + 单核软中断 |
| 工具 | 无 ethtool；有 iperf3 但本次未运行；nft/iptables 空 |
| 1G goodput | **1G 物理口；app 500–750M 可接受。** `.74` 是候选，需实测：理论上限约 `900/wire` Mbps；单 IRQ 核/单队列是主要风险 |
| FEC 算力 | AVX1 `vpxor` + 4 核；1G 档约 0.5 GB/s XOR 有余量；瓶颈是网口/软中断，不是 XOR `[推算，未实测]` |
| 256 carriers | 内存/端口（32768–60999）够；1G 口 + 单队列可做 256 条候选，但需实测软中断/IRQ 是否撑得住 500–750M app |

**定位**：低端 x86 **非最终** 1G 物理候选/对照机；可以验证 256-carrier 会话、端口、内存路径，并实测 app 500–750M；但不能验证 ARM64 NEON/SVE、arm64 内存序。`.65` 侦察中，待其报告再决定 ARM64 候选。`[实测/推断]`

### 3.6 `.65` ARM64 构建/移植目标（lab）：RK3588，非干净 benchmark

`review/node65-target.md`（`w-node65`，commit `7450f99`）只读探测结论：`192.168.123.65` 是 **aarch64 RK3588（LubanCat-5 V2）**，适合做 ARM64 构建/移植目标，但**不是干净 benchmark 机**。`[实测]`

| 项 | 值 |
|---|---|
| SoC/架构 | Rockchip RK3588，aarch64（ARMv8-A，LE） |
| CPU | 4×Cortex-A76 r4p0（2.3 GHz）+ 4×Cortex-A55 r2p0（1.8 GHz）；L3 3 MiB |
| ISA | `asimd`（NEON）、AES、PMULL、CRC32、FP16、dot-product；**无 SVE/SVE2** |
| 内存 | 7.73 GiB（可用约 4.78 GiB）；swap 2 GiB |
| 内核/构建 | 6.1.172；`/lib/modules/.../build` 与 headers 齐全；gcc 15.2.0；`CONFIG_MODVERSIONS=y`，`MODULE_SIG` 未启用 |
| 网口 | `eth0` st_gmac **1000 Mb/s full**，MTU 1500，单 RX/单 TX 队列，IRQ 全在 CPU0，RPS 关闭 |
| offload | RX/TX checksum on、TSO/GSO/GRO on、SG on；LRO off；`ethtool` 可读 |
| 生产负载 | mihomo、mediamtx、5×ffmpeg、frpc、rclone；load ~0.35；eth0 已有 13.8 万 RX drop |
| 1G 可行性 | 1G 物理口；app 500–750M 在 grouped 32×8 下可行；编码 1.0–2.0 GB/s，A76 NEON/内存有余量；收包单队列/IRQ CPU0 是短板 `[推断，未实测]` |
| 移植阻塞 | 当前 master 在 6.1 编译失败：`system_dfl_wq` 在 6.1 不存在（16 处引用）；需映射到 `system_unbound_wq` 或加版本兼容宏 |
| 256 carriers | 当前 ABI 硬限 16（`TBT_MAX_CARRIERS_LIMIT`/u16 ack mask）；256 需 v6 数据面改动 |

**定位**：`.65` 是 ARM64 **构建/移植**目标；可以做功能、6.1 适配、低参数验证，但生产负载会污染吞吐/延迟测量，不能当干净 benchmark。`.74` 是 x86_64 低端对照，非最终；`.254` MIPS 不可行。

## 4. MIPS：lab 结论（1G 物理口；app 500–750M；生产目标 HND0，MIPS 不可行）

> 依据：`review/mips-target.md`（`w-mips-recon`，2026-11-06）和 `/tmp/mips-recon/01..19-*.txt` 的只读原始输出。目标机未跑 FEC 基准；没有编译器，无法在设备上实测 GF(2) XOR/RLNC。按用户决定，**停止 MIPS 1G 方向的性能调研**；本节只给结论和低目标边界。

### 4.1 实际目标：JDC-1 / MT7621

| 项 | 实测/结论 |
|---|---|
| 板卡/固件 | JDC-1（京东云无线宝），Padavan 3.4.3.9 |
| SoC | MediaTek MT7621 |
| CPU | MIPS 1004Kc V2.15，4 VPE（2 物理核 × 2 线程），BogoMIPS 579.58；MT7621 典型 880 MHz（cpuinfo 不报 MHz） |
| ISA/ASE | `mips16 dsp mt`；**无 MSA**；**无 FPU** |
| 内存 | 450,768 kB（440 MiB）；swap 512 MiB（eMMC） |
| 存储 | rootfs 13.5 MiB，100% 满；`/tmp` 39 MiB；`/opt` 19 MiB 可用 |
| 内核 | 3.4.113 SMP，32-bit，vermagic `3.4.113 SMP mod_unload MIPS32_R2 32BIT` |
| 构建 | 设备内**不可能**：无 gcc/make/ld/as/headers/build tree；只能厂商 `mipsel-linux-uclibc-gcc 4.4.7 -mips32r2 -mtune=1004kc` 交叉编译 |
| 模块 | `modules_disabled=0`；151 个 `.ko`；`tainted=4096`（已加载过树外模块）；无模块签名 |
| 网卡/链路 | `eth2`（交换机 CPU 口，IRQ 11 全在 CPU1）、`eth3`（WAN）；`br0 = {eth2, eth3, ra0, rai0}`；phy2 协商 1000BASE-T |
| 转发模式 | **AP/桥接**：过境帧由交换机/桥转发，不进 IP 栈；tbtun 是 L3/L4 端点，当前配置下看不到过境流量 |
| 工具 | 无 ethtool/tc/nft/iperf/python/perl；有 openssl、taskset、mii_mgr、brctl、iptables、insmod |
| 1G 口 | 是 **L2 交换意义上的 1G**，不是 CPU 端点意义上的 1G；CPU 端点上限约 **300–500 Mbps**，加 FEC 后更低 |

### 4.2 为什么 1G/滑窗 RLNC 在 MT7621 不成立

1. **CPU 不够。** 2 个 880 MHz 级 1004Kc 核、32-bit、无 MSA。`mips-target.md` 的实测锚点：L2 驻留 memset 1384 MiB/s（纯写上界）、AES-128-CBC 14.9 MiB/s、SHA-256 20.8 MiB/s（单核）。q=2 下 GF(2) sparse 的编码是 500–1500 MB/s XOR（§3.2，M=32–96），解码 123–1000 MB/s，再加 256 条 TCP 的约 89k pps；FEC 本身已占掉大部分标量吞吐，TCP 栈再叠加，两个核不够 125 MB/s 载荷。
2. **`mips-target.md` 的 GF(256) 模型给出更低的边界。** 该报告按 GF(256) 4-bit 表乘加估算 15–44 MiB/s/核、双核 30–88 MiB/s；W≤8 时约 **30–90 Mbps**。本设计是 GF(2) sparse，编码成本比 GF(256) dense 低，但没有实测；即便乐观，也到不了 1G。**不要用 MSA/DSP 假设去填这个差距。**
3. **1G 口不是端点 1G。** MT7530 交换机能 1G 转发；CPU 端点约 300–500 Mbps，tbtun 的 FEC 代理绕过 `hw_nat`/`sfe`，借不到硬件 NAT 的 1G。
4. **当前是桥接模式。** `br0` 把 `eth2/eth3/ra0/rai0` 桥在一起，WiFi 客户端与上游网关在同一桥域；过境流量不经过 IP 栈。要跑 tbtun，必须先切路由模式，或只承载本机流量。
5. **内核 3.4.113 是独立硬阻塞。** README 要求 Linux ≥6.1；`tbt_carrier.c` 依赖 `kernel_connect`、私有 `struct proto` + `tcp_sendmsg_locked`、`sk_data_ready` 等现代接口；设备没有 headers/build tree/编译器。这是实质性向后移植，不是改几个宏。
6. **256 carriers 当前代码不支持。** `tbtun/tbt_core.c:82` 的 `BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16)` 和 `tbtun/tbt_uapi.h:113` 的 `TBT_MAX_CARRIERS_LIMIT 16`（`path_ack_mask` 是 u16）把 carrier 数硬限在 16；256 需要 v6 线格式（path_id u16、ack 位图 256 bit、约 64 B 头），不是配置问题。`[源码]`

### 4.3 Linux/MIPS 的 xor/memcpy/csum：没有 MSA/DSP 快路径

- `arch/mips/lib/Makefile` 只有 `bitops`、`csum_partial`、`delay`、`memcpy`、`memset`、`mips-atomic`、`strncpy_user`、`strnlen_user`、`uncached`；**没有 xor**，没有 MSA/DSP 变体。`[源码]`
- `memcpy.S` 用 32-bit `lw/sw`（32-bit 内核）或 64-bit `ld/sd`（64-bit 内核）+ `pref`；`csum_partial.S` 同理；没有 MSA/DSP 指令。`[源码]`
- `arch/mips/include/asm/msa.h` 只有用户上下文 `enable_msa()`/`_save_msa`/`_restore_msa`；内核没有 `kernel_fpu_begin/end` 式保护 API。1004Kc 本身无 MSA，讨论 MSA 对本目标没有意义。`[源码]`
- 结论：不要假设 Linux 已经为 MIPS 优化了 XOR；1004Kc 上只能 32-bit GPR 标量 XOR。

### 4.4 低目标结论（给 fec-core）

- **1G 物理口；app 500–750M 可接受；ARM64 优先，但 x86_64 `.74` 是 1G 物理候选，需实测；`.65` 侦察中。** MT7621 不作为 1G 目标，Kodo/RLNC 在这块板上只能低目标。
- 若必须在 MT7621 上跑：**W≤8（即 K≤4），目标 30–90 Mbps**（`mips-target.md` 的估算；GF(2) sparse 可能略高，但没有实测）；先切路由模式、回移内核、把 carrier 数限制在 16 或做 v6；不要承诺 1G。
- 若必须 1G：换 ARM64（NEON）；x86 仅作参考。Kodo 的 x86 锚点是 K=256 约 19/12.4 Gbps，1G 占 5–8%；ARM64 需实测，估 3.8–6.4/2.5–4.1 Gbps。
- **不再调研 MSA/DMA/RAID XOR/NIC offload 作为 MIPS 1G 路径**；这些不是本目标的可行手段，继续投入没有意义。

## 5. 其他滑窗 RLNC 实现（只列最相关）

### 5.1 badrlnc（Apache-2.0，Go）

- URL：`https://github.com/ihatemyfcklife/badrlnc`，commit `ccb38a8`，2026-09-27，Apache-2.0。
- 结构：`SlidingWindowEncoder`（窗口 1..256）、`IncrementalDecoder`（256-bit mask、增量 Gauss-Jordan、cascade back-substitution）、`InOrderResequencer`、`SplitMix64`、`Bitset256`。
- Wire：compact 24 B（W≤64，64-bit mask）/ extended 48 B（W≤256，256-bit mask）；`BaseSeq + Mask + payload`。`[源码]`
- 可复用：**算法/数据结构参考**（256-bit mask、pivot ring、cascade、48 B header）；Apache-2.0 与 GPLv2 不兼容，不能复制进内核模块。
- 风险：parity mask 是 **dense**（`GenerateParity` 对 active window 的每个 bit 随机置位）；grouped 32×8 的 W≤12，256-bit mask 够用，但 dense mask 仍是 ~W/2 XOR/byte（W=10 约 5），且 Go/1G mips 不是生产路径；只作算法参考。其 README 的 `GenerateParity 54.6 ns/op`（1200 B × 平均 16 源包 ≈ 19 KB XOR）对应约 350 GB/s，超过单核内存带宽，疑似 benchmark 未计入 payload 或被优化；不要引用为 1G 证据。`[源码/推算]`

### 5.2 tetrys-rs（Apache-2.0，Rust）

- URL：`https://github.com/gdanezis/tetrys-rs`，commit `67dca71`，2026-03-04，Apache-2.0。
- RFC 9407 的 elastic-window 网络编码：encoder window 可 push/remove，decoder 增量高斯消元；GF(2^4)/GF(2^8)，Vandermonde 系数（`alpha^(src_id*coded_id)`），SIMD NEON/AVX2。`[源码]`
- 可复用：窗口/ACK/反馈状态机、增量消元、RFC 9407 头字段字节布局（`tests/conformance.rs` 有固定字节向量）；**不是 GF(2) sparse**，GF(2^8) Vandermonde 在 256 个源 symbol 时会有 `mod 255` 重复（256 个 ID 只有 255 个非零域元素），不能直接用于 N+M=256。`[源码/推算]`
- Apache-2.0 与 GPLv2 不兼容；Rust 不能进内核。只作算法参考。

### 5.3 没有找到的其他实现

- `yeliqseu/streamc`：源码已下架，只留 API 头；无法使用。
- `Roy-Hu/RLNC_Streaming`：无许可证，实验代码；不建议。
- Kodo 2015 的 `sparse_full_rlnc_encoder` 是 block/generation，不是滑窗；只作 sparse generator/decoder 参考。

## 6. 测试向量

### 6.1 公开项目里有什么

| 项目 | 测试向量 | 能否直接用 |
|---|---|---|
| Kodo Slide C | `basic_api`：100 symbols、750 B、全窗口；`slide_window`：5-symbol 窗口、rate 6/3、50% loss | 行为场景，不是字节向量；需要 Kodo 私有库才能跑 |
| Kodo Slide Go | `TestCodec`：capacity 15、window 5、symbol 16 B、rate 8/3、50% loss、4 个 field、100 decoded | 行为场景；可照着写 C 测试，但没有期望字节 |
| Kodo 2015 | `test_sparse_uniform_generator`：同 seed → 同 vector；`test_matrix`；`test_rlnc_full_vector_codes` | 自洽测试；没有 golden vector |
| badrlnc | 手写 `Shard{BaseSeq, Mask, Data}` + 恢复断言；`matrix_inversion_test.go` 用固定 seed `0xdeadbeefcafe1337` | 可提取简单 mask/payload 向量；Apache-2.0，不能复制测试代码 |
| tetrys-rs | `tests/conformance.rs`：RFC 9407 头/EV 的固定字节布局 | 只对 Tetrys wire 有用；不是 coding vector |

### 6.2 建议 fec-core 冻结自己的向量

没有开源项目发布「滑窗 GF(2) RLNC 的 golden parity 字节」。不要假装有。建议自己生成并提交：

```
HND0 生产 grouped: W=12/10/8/6（M=2/3/4/5，N=6/5/4/3，d=12/10/8/6）；lab 小参数 4/1、8/2、16/4 仅对照；单组 W=448/480 是 stress，不是生产
d = 16
seed = 0x0000000000000000 .. 0x000000000000000f
data[i][j] = (u8)(i*31 + j*17 + 0x5a)   // 固定模式
期望：每个 (W, seed, row) 的 d 个 index、parity payload 的 hex、解码恢复结果；HND0 生产 grouped 32×8 的 W=12/10/8/6 场景必须覆盖；lab 小参数 4/1、8/2、16/4 作为对照
```

最小自检：

1. `d=1` 退化为 systematic；parity == 对应源包。
2. `W=2, d=1, seed=0`：两个源包 A/B，parity=A；丢 B 不能恢复（rank 不足），丢 A 可恢复 B？不，parity=A 只能恢复 A；要测 `d=2` parity=A⊕B，丢 A 或 B 都能恢复。
3. `W=16, d=16`：parity 是所有源包 XOR；任意丢 1 个可恢复。
4. 同 seed 在编码器/解码器生成同一组 index；换 W 后 index 集合变化，但两端一致。
5. 乱序/重复/迟到/窗口滑动；Kodo 的 `slide_window` 场景作为行为 oracle。

## 7. 给 fec-core 的具体输入

### 7.1 HND0 生产配置（256 carriers / grouped 32×8 / app 500–750M）

```
carriers = 256
groups = 32                  # 32 groups × 8 carriers
per group: N + M = 8, K = N, W = 2N, q = 2
M_group = clamp(ceil(1.3 * W * p) + 2, 1, 6)
total N = 32 * (8 - M_group)
total parity = 32 * M_group
d: M=2 -> 12, M=3 -> 10, M=4 -> 8, M=5 -> 6
```

- p≤5% → M=2,N=6,W=12,d=12，app 750M；p≤10% → M=3,N=5,W=10,d=10，app 625M；p≤20% → M=4,N=4,W=8,d=8，app 500M；p=30% 要 99.9% → M=5,N=3,W=6,d=6，app 375M。
- 每组窗口 12/10/8/6 包，约 25/21/17/13 ms；32 组缓冲 3.0/2.5/2.0/1.5 MiB。
- 编码 8/12/16/20 XOR/byte，1G 下 1.0/1.5/2.0/2.5 GB/s；**HND0 2 vCPU** 是风险，必须实测。
- **HND0 生产**：256 carriers / 物理 1G / 五元组限速严重 / app 500–750M；CPU/RAM 2 vCPU/955 MiB 是硬约束。
- `.65`/`.74` 是 lab；MT7621 不可行。
- 前置条件：当前代码只支持 16 carriers；256 需要 v6 数据面（40 B FEC 头、share_index u16、epoch u16、n/m u8、256-bit 反馈 bitmap）。`[源码]`

### 7.2 数据结构和热路径

```c
struct fec_row {
    u64 bits;           /* grouped W <= 12；单组 stress 才需要更大位图 */
    u8  *payload;       /* S bytes */
    u16 len;
};

struct fec_group {
    u64 win_start, win_end;
    u8  m;              /* 本 epoch 的 M */
    u8  rows_have;      /* M <= 8 用位图；M>8 用 256-bit 位图 */
    u8  *rhs[FEC_M_MAX];
};
```

- 编码：`memset(p,0,S)`，对 `d` 个 index 做 `xor_into`；不要遍历整个窗口。
- 解码：只对缺失集合 `missing[k]` 建 `k×k` 矩阵；消元后 `k²` 次 payload XOR；不要 Kodo 的全窗口 forward/backward substitution。
- 位图：grouped `W≤12` 用 1×`u64`；`row`/`m` 用 `u8`。单组 stress 才需要 8×`u64`。
- PRNG：`splitmix64(nonce ^ epoch ^ row)`；Fisher-Yates 选 `d` 个 index；不要用 `prandom`。
- 校验：恢复后 `crc32c`；失败丢包，绝不交付坏字节。

### 7.3 头部/系数（grouped 生产）

当前设计是**独立的 40 B FEC 格式**（`conv 0x1001`）：`share_index u16`、`epoch u16`、`n/m u8`、256-bit 反馈 bitmap；**不复用旧 16-carrier 的帧头/FBK/WINDOW**，改常量解决不了 16 槽限制。`[源码: review/fec-design.md §2.2/§11.2]`

- grouped 每组 `W≤12`，`win_len` 用 u8 就够；单组 `W=448` 才需要 u16，但单组已被否决。
- 系数位图 grouped `W≤12` 用 1×`u64`；seed 模式不需要在 wire 上带系数；显式系数只需 2 B。
- 旧 32 B 头的 `win_len` u8 问题只对单组 W=448 有意义；生产 grouped 不要复用旧头。
- `n/m`：每组 N+M=8，u8 足够；`epoch` 用于动态 M 和 stale 拒绝。
- 反馈：256-bit bitmap 覆盖 256 carriers；不是旧的 16-bit `path_ack_mask`。

### 7.4 HND0 生产验收（256/1G）

- HND0 2 vCPU：app **500–750 Mbit/s**；`wire/app = 256/N = 1.33/1.6/2.0`（M=2/3/4），不是 1.15。
- CPU：2 vCPU 的 `steal`/`%idle` 必须留余量；256 TCP ≈ 89k pps + grouped 编码 1.0–2.0 GB/s；先测 500M，再试 750M。
- 256 carriers：`carrier_lost` 稳定；`rxq` 不积压；in-flight ≥ 1 BDP；五元组限速策略需实测确认。
- 内存：955 MiB 下 32–64 MiB socket 缓冲 + 3 MiB FEC 可容纳；监控 slab/socket 数。
- MT7621 是 lab/不可行，不是生产验收项。

## 8. 参考文献与源码位置

- Kodo 官方 benchmark：`https://www.steinwurf.com/blog/benchmark-kodo`
- Kodo 研究许可证 1.2：`https://www.steinwurf.com/kodo-research-license-1-2`
- Kodo Slide C API：`kodo-slide-c/src/kodo_slide_c/kodo_slide_c.h`（本地 `/tmp/fec-survey/kodo-slide-c`，commit `7750183`）
- Kodo Slide C 测试：`kodo-slide-c/test/src/test_kodo_slide_c.cpp`（rate controller 第 105–137 行；slide_window 第 240–380 行）
- Kodo Slide Go 测试：`kodo-slide-go/kslide_test.go`（capacity 15/window 5/rate 8/3/50% loss）
- Kodo 2015 sparse generator：`kodo-2015/src/kodo/sparse_uniform_generator.hpp`（`set_density` 第 125 行）
- Kodo 2015 decoder：`kodo-2015/src/kodo/linear_block_decoder.hpp`（forward/backward substitution）
- Kodo 2015 sparse benchmark：`kodo-2015/benchmark/throughput/codes.hpp`、`main.cpp`（density 0.1–0.5，symbols 16–128）
- badrlnc：`https://github.com/ihatemyfcklife/badrlnc`，commit `ccb38a8`；`rlnc.go`（48 B extended header）、`encoder.go`（SplitMix64/dense mask）、`gauss_jordan.go`
- tetrys-rs：`https://github.com/gdanezis/tetrys-rs`，commit `67dca71`；`src/encoder/window.rs`、`src/decoder/matrix.rs`、`tests/conformance.rs`
- Linux MIPS：`arch/mips/lib/Makefile`、`memcpy.S`、`csum_partial.S`、`memset.S`、`arch/mips/include/asm/msa.h`、`arch/mips/kernel/process.c`、`arch/mips/include/asm/dsp.h`
- 实际 MIPS 目标：`review/mips-target.md`（`w-mips-recon`，2026-11-06；JDC-1 / MT7621 / 1004Kc / 3.4.113）
- 1G lab 候选 `.74`：`review/arm64-target.md`（`w-arm64-recon`，commit `ccafdeb`；x86_64 / Athlon X4 760K / 1G r8169，不是 ARM64，非最终）
- ARM64 构建/移植目标 `.65`：`review/node65-target.md`（`w-node65`，commit `7450f99`；RK3588 / 4×A76+4×A55 / NEON / 7.73 GiB / kernel 6.1.172 / 1G eth0 单队列 / 生产负载）
- HND0 生产对端：`review/wan-experiment/summary.md`（2 vCPU / 955 MiB；256 流 / 物理 1G / 五元组限速严重；x86_64）
- grouped 32×8 设计：`review/fec-design.md` §3.8（`w-fec`）
