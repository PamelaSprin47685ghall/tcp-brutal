# SC-LDPC / RaptorQ 开源实现调研（fec-core2 输入）

状态：调研稿。范围只限 **SC-LDPC / streaming LDPC** 与 **RaptorQ（RFC 6330）**；不扩展到其他码型。
基线：`master 0db5166`；生产对端 HND0：256 流 / 物理 1G / 五元组限速严重 / grouped 32×8 / app 500–750M；HND0 只有 2 vCPU / 955 MiB。`[用户确认]`
证据标记：`[实测]` 本机或目标机直接输出；`[源码]` 直接读到的代码/接口；`[推算]` 由操作数/架构比例推出；`[待测]` 必须上机。

## 0. 结论摘要

| 问题 | 结论 |
|---|---|
| 有成熟的 C/C++ SC-LDPC 编解码器吗 | **没有找到**。aff3ct / IT++ 是 **block LDPC**，不是 SC-LDPC；`npminh12/Spatially-coupled-LDPC` 是 MIT 的 C++ **BEC 仿真**（506 行，非 packet codec）；`libcoding` 是 BSD-3 的 streaming LDPC，但源码 `ldpc.h`/`ldpc.cc` 缺失；SCA-LDPC 是 Rust 且无 license；其余是 MATLAB/Python。 |
| SC-LDPC 能直接给 fec-core2 用吗 | **不能直接复用**。只能拿 `npminh12` 的耦合构造/BP 思路和 aff3ct 的 BP/peeling 解码器做算法原型；内核移植需要自己写图构造、调度、窗口和测试向量，风险高。 |
| RaptorQ 有可用的 C 实现吗 | **有：nanorq（C11，MIT，RFC 6330）**。低层 API 不依赖 libc，调用方提供 arena；有 golden MD5 向量和 RFC 矩阵；但 RaptorQ 是 **block** 码，不是流式。 |
| RaptorQ 其他实现 | `foxsen/raptorQ`（C++，Apache-2.0，44 tests）、`libRaptorQ`（C++11/Eigen，LGPLv3/GPLv3）、`orq`（C++，MIT，2014）、`cberner/raptorq`（Rust，Apache-2.0）。Apache/LGPLv3/GPLv3 与 GPLv2 内核不兼容；C++/Eigen 不能进内核。 |
| 内核可移植性 | nanorq 低层 API 最接近：C11、无 STL、无 libc、caller arena；但 RaptorQ 需要整块 K×T 矩阵和求逆，block barrier/内存/CPU 是硬约束。SC-LDPC 没有可用 C 实现，BP 图/迭代调度要自己写。 |
| 对 fec-core2 的建议 | 先评估 **nanorq RaptorQ 作为 block fallback/对照**（K=64/128/256，T=1400/8192，overhead 2–5%）；SC-LDPC 只做算法原型，不作为 v1 主路径。RLNC 的流式低延迟仍是主设计。 |

## 1. 对象与版本

| 项目 | commit / 版本 | 日期 | 许可证 | 语言 | 结论 |
|---|---|---|---|---|---|
| `aff3ct/aff3ct` | `e1b5296` | 2026-11-03 | MIT | C++ | block LDPC（5G/DVB/QC），有 BP/peeling + BEC channel；**无 SC-LDPC** |
| IT++ | 4.3.1 | 2013-07-06 | GPL-2.0 | C++/BLAS | block `LDPC_Code`；**无 SC-LDPC**；老、重依赖 |
| `npminh12/Spatially-coupled-LDPC` | `3d403f4` | 2019-10-26 | MIT | C++ | 506 行 BEC BP 仿真；SC-LDPC 构造参考，不是 packet codec |
| `aarond10/libcoding` | `6ba5b66` | 2012-03-08 | BSD-3 | C++ | streaming LDPC for files；源码 `ldpc.h`/`ldpc.cc` **缺失**，只有 test |
| `atneit/SCA-LDPC` | `5eea992` | 2024-07-22 | 无 license | Rust | 不是 C/C++；不可用 |
| `hesfahanizadeh/Unified_SC_LDPCL` | `2224028` | 2021-09-14 | MIT | MATLAB | 构造/分析参考；不是 C/C++ |
| `nanorq` | `7d52fbe` | 2026-10-24 | **MIT** | **C11** | **RFC 6330 RaptorQ**；低层无 libc；golden MD5 向量；block |
| `foxsen/raptorQ` | `53727d2` | 2026-03-23 | Apache-2.0 | C++ | RFC 6330；44 tests；C++/STL；GPLv2 不兼容 |
| `libRaptorQ` | `a394b22` | 2022-12-13 | LGPLv3/GPLv3 | C++11/Eigen | RFC 6330；header-only；GPLv2 不兼容 |
| `olanmatt/orq` | `0e85e54` | 2014-10-30 | MIT | C++ | RFC 6330；老、未完成；C++ |
| `cberner/raptorq` | `73e4e97` | 2026-10-23 | Apache-2.0 | Rust | RFC 6330；active；非 C；GPLv2 不兼容 |

## 2. SC-LDPC / streaming LDPC

### 2.1 它是什么，和 RLNC 的差别

SC-LDPC 是空间耦合的 LDPC：把多个 LDPC 块沿时间/位置耦合，形成卷积/滑窗结构；接收端用 BP 或 peeling 逐窗解码。它的优点是阈值接近容量、码率固定、编码是 XOR/稀疏矩阵；缺点是码图设计/耦合、BP 迭代调度、窗口/尾比特复杂，且需要长码才能体现阈值优势。`[源码/通用]`

对 packet erasure：
- 变量节点=packet，校验节点=XOR 约束；BEC 上 BP 消息是“已知/未知”，可退化为 peeling；不需要浮点 LLR。
- 但 SC-LDPC 的图构造（protograph、coupling length L、lifting M）和窗口同步是核心工作；没有成熟 C 库可直接拿。

### 2.2 aff3ct（MIT，C++，block LDPC）

- 地址：`https://github.com/aff3ct/aff3ct`，commit `e1b5296`，MIT。
- 能力：LDPC encoder/decoder（BP flooding/layered/ONMS、bit-flipping、peeling），`Channel_binary_erasure`；支持 5G/DVB-S2/QC-LDPC/AList。`[源码]`
- **没有 SC-LDPC**：全仓 grep `SC-LDPC`/`spatially coupled` 零命中；它是 block LDPC 框架。`[源码]`
- 可复用：BP/peeling 解码器、AList/QC 矩阵、BEC 仿真；可作为 SC-LDPC 解码器的算法参考。
- 风险：C++/STL/模板、构建重；不能进内核；没有 packet-level streaming API；没有 SC-LDPC 测试向量。

### 2.3 IT++（GPL-2.0，C++，block LDPC）

- 官方：SourceForge `itpp`，4.3.1（2013-07-06），GPL-2.0。`[网页]`
- 能力：`itpp::LDPC_Code`（block LDPC），依赖 BLAS/LAPACK/FFTW；有 LDPC 编解码。
- **没有 SC-LDPC**；2013 年后基本不活跃；C++/重依赖；不能进内核。
- 许可证 GPLv2 与内核兼容，但代码老、C++/BLAS，实际不可移植；只作历史参考。

### 2.4 `npminh12/Spatially-coupled-LDPC`（MIT，C++，BEC 仿真）

- 地址：`https://github.com/npminh12/Spatially-coupled-LDPC`，commit `3d403f4`，MIT。
- 单文件 `spatiallycoupledLDPC.cpp`，506 行：`(l,r)` regular protograph、coupling `L`、lifting `M`、BEC BP 解码；常量 `L=500, M=100, l=5, r=10, MAX_BP_ITER=200`。`[源码]`
- 可复用：SC-LDPC 构造/耦合/BP 的算法参考；不是 packet codec，没有帧格式、流式窗口、测试向量；代码是仿真风格（`stdafx.h`、`malloc.h`、全局数组），不能直接进内核。
- 结论：**只作算法参考**；如果 fec-core2 要做 SC-LDPC，需要自己重写图/窗口/调度。

### 2.5 `libcoding`（BSD-3，C++，streaming LDPC）

- 地址：`https://github.com/aarond10/libcoding`，commit `6ba5b66`，BSD-3。
- README 描述 `StreamingLDPCEncoder/Decoder`：按窗口编码大文件，`storage factor 2.05`、`block size 32768`、`window 4096`；看起来是 packet/file erasure LDPC。`[源码]`
- **源码缺失**：仓库只有 `ldpc_test.cc`、`Makefile`、`README`；`ldpc.h`/`ldpc.cc` 不在 tree 里（`git ls-tree` 也没有），无法构建。`[源码]`
- 结论：**不可用**；只能从 README 看接口思路。

### 2.6 其他

- `atneit/SCA-LDPC`：Rust，无 license；不是 C/C++。
- `hesfahanizadeh/Unified_SC_LDPCL`：MATLAB，MIT；构造/分析，不是 C/C++。
- `a-yeyang/sc-ldpc`、`zrbzrb1106/ldpc_decoding`：Python/MATLAB，无 license；只作研究参考。

### 2.7 SC-LDPC 内核移植边界

- **没有可移植的 C 实现**；aff3ct/IT++ 是 C++/STL/重依赖，`npminh12` 是仿真，`libcoding` 源码缺失。
- 若自研：需要 protograph/coupling/lifting、窗口同步、BP/peeling 调度、尾比特/终止、测试向量；代码量远大于 RLNC 的 XOR 内核。
- BEC 上可只用 XOR/peeling（无浮点），但图存储和迭代调度仍是核心；HND0 2 vCPU/1G 下 BP 迭代 CPU 需实测。
- 结论：**v1 不建议**；只作 fec-core2 的算法原型/对照。

## 3. RaptorQ（RFC 6330）

### 3.1 nanorq（C11，MIT）——最现实的 C 参考

- 地址：`https://github.com/sleepybishop/nanorq`，commit `7d52fbe`，MIT。
- 能力：RFC 6330 RaptorQ；`K_max=56403`，`T=8..65536`；GF(256)；编码/解码/repair；高层 `nanorq.h` + 低层 `nanorq_core.h`。`[源码]`
- 低层 API：`nanorq_core_encoder_new`、`calculate_prepare_memory`、`prepare`、`calculate_work_memory`、`precalculate`、`get_memory_reqs`、`init_matrix`、`place_symbol`、`get_packet_mix`；**caller 提供 arena，无 libc 依赖**（NDEBUG）。`[源码]`
- 高层 API 用 `calloc`/`ioctx`/`mmap`；内核移植应只用低层。
- TSNC：README 有实验性 block-based recoding/sliding-window helpers（`nanorq_decoder_add_recoded_symbol` 等），**不是 RFC 6330 互操作面**，dense 恢复上限 256 MiB；不要当 SC-LDPC/流式标准。`[源码]`
- 测试向量：`t/60encode.t` 对 `examples/encode K 64 10 sample_data/raw` 的输出做 **MD5 比对**；`t/assets/examples/encode/K_{10,50,100,500,1000,5000,10000,50000,56403}.txt` 是 golden 输出；`t/assets/mats` 有 RFC 矩阵。我们本机验证 `K=10, T=64` 的 MD5 与 `K_10.txt` 一致。`[实测]`
- 内存（本机调用 `nanorq_core_get_memory_reqs`，overhead=2）：`[实测]`

| K | T | prepare | work | matrix | schedule | total |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 1400 | 19.6 KB | 2.9 KB | 81 KB | 27.4 KB | 131 KB |
| 64 | 1400 | 37.3 KB | 4.9 KB | 138 KB | 42.7 KB | 223 KB |
| 128 | 1400 | 69.3 KB | 11.1 KB | 249 KB | 73.4 KB | 403 KB |
| 256 | 1400 | 124 KB | 24 KB | 439 KB | 135 KB | 721 KB |
| 256 | 8192 | 124 KB | 24 KB | 2.46 MB | 135 KB | **2.74 MB** |
| 512 | 8192 | 249 KB | 67 KB | 4.78 MB | 258 KB | **5.35 MB** |
| 1024 | 8192 | 480 KB | 198 KB | 9.11 MB | 504 KB | **10.3 MB** |

- 本机解码示例（Ryzen 9 7940H，`examples/decode K T 10 sample_data/raw`，单次，非严格 benchmark）：K=256/T=8192 约 14 ms；K=512/T=8192 约 24 ms；K=1024/T=1400 约 33 ms。`[实测，host]` HND0 2 vCPU 必须重测。
- 内核边界：低层 C11/无 libc/caller arena 可行；但 `oblas_common.c` 仍用 `calloc/free`（高层/`obl_alloc`），内核要用 core API 或替换；`oblas_lite.c` 的 SIMD 是 **x86 AVX512/AVX2/GFNI/SSSE3**，没有 ARM NEON 路径；ARM64 走标量。`[源码]`
- RaptorQ 是 **block**：K×T 整块、矩阵求逆、等 K 个 symbol；不是流式，block barrier/延迟/内存是硬约束。`[源码/通用]`

### 3.2 foxsen/raptorQ（C++，Apache-2.0）

- 地址：`https://github.com/foxsen/raptorQ`，commit `53727d2`，Apache-2.0。
- C++ 实现，无外部依赖；`test.cpp` 44 tests（GF(256)、Deg、编码/解码、吞吐）；block RaptorQ。`[源码]`
- 没有看到 golden RFC 字节向量；测试是自洽/功能测试。C++/STL，不能进内核；Apache-2.0 与 GPLv2 不兼容。
- 结论：可作第二参考/交叉验证，不可直接移植。

### 3.3 libRaptorQ（C++11/Eigen，LGPLv3/GPLv3）

- 地址：`https://github.com/LucaFulchir/libRaptorQ`，commit `a394b22`，2022-12-13。
- header-only C++11，Eigen 矩阵；RFC6330 + RAW API；有 `test/test_c.c`、`test_cpp_rfc.cpp`、`test_cpp_raw.cpp`。`[源码]`
- 许可证 LGPLv3/GPLv3；与 GPLv2 内核不兼容；C++/Eigen/STL；block；不能进内核。
- 结论：只作 RFC 行为/测试参考。

### 3.4 orq（C++，MIT，2014）

- 地址：`https://github.com/olanmatt/orq`，commit `0e85e54`，MIT。
- RFC6330 C++，基于 OpenRQ；有 `test/`（tuple/row/partition/parameters/orq）；2014 年后不活跃，README 自述“heavy active development”，完成度有限。`[源码]`
- MIT 可进 GPLv2，但 C++/STL/老代码；block；不建议直接移植。

### 3.5 cberner/raptorq（Rust，Apache-2.0）

- 地址：`https://github.com/cberner/raptorq`，commit `73e4e97`，2026-10-23，Apache-2.0。
- Rust RFC6330，active；有 benches/fuzz/RFC6330_ERRATA；非 C；Apache-2.0 与 GPLv2 不兼容。`[源码]`
- 结论：可作 RFC 交叉验证/测试向量参考，不能进内核。

### 3.6 RaptorQ 测试向量

- **nanorq 是唯一有公开 golden 编码输出的 C 实现**：`t/assets/examples/encode/K_*.txt` + `t/60encode.t` 的 MD5 比对；`t/assets/mats` 有 RFC 预计算矩阵；`t/85api.t`/`api_regress` 有 API 回归。`[源码/实测]`
- foxsen/raptorQ：44 tests，但没有 golden RFC 字节向量；libRaptorQ：C/C++ tests，没有公开 golden 字节；orq：单元测试，没有 golden；RFC 6330 本身没有官方 golden 编码字节。`[源码]`
- 建议 fec-core2 用 nanorq 的 `K=10/100/1000` 向量做互操作/回归，再自己冻结 K=64/128/256 的 HND0 参数向量。

### 3.7 RaptorQ 内核移植边界

- 可用：nanorq 低层 API（C11、无 libc、caller arena、无 STL）；MIT 与 GPLv2 兼容。
- 需要改：`oblas_common` 的 `calloc/free`、`-march=native`、x86 SIMD 分支；ARM64 没有 NEON 路径；内核用 `kmalloc/kvmalloc` 提供 arena；`memcpy/memset` 可用。
- 硬约束：block K×T 矩阵 + 求逆；K=256/T=8192 约 2.74 MB/块；K=1024/T=8192 约 10.3 MB/块；block barrier 延迟 = K×T / app rate（K=256/T=8192 @750M ≈ 22 ms，加解码）。HND0 2 vCPU/955 MiB 内存够，CPU 需实测。
- 不建议把 RaptorQ 当流式主路径；它适合 block fallback/对照/大文件，不适合逐包低延迟。

## 4. 三者对照（给 fec-core2 的选型）

| 维度 | RLNC grouped 32×8（主设计） | SC-LDPC | RaptorQ（nanorq） |
|---|---|---|---|
| 流式 | **是**，逐包 | 是，卷积/窗口 | **否**，block |
| 延迟 | 窗口 21–25 ms | 窗口 + BP 迭代 | K×T / app + 解码；K=256/T=8192 @750M ≈ 22 ms |
| 编码 | GF(2) XOR，8–16 XOR/byte | XOR/稀疏矩阵，码图复杂 | GF(256) 矩阵，block 求逆 |
| 解码 | k×k GF(2) 消元 | BP/peeling 迭代 | GF(256) 消元/ inactivation |
| 成熟 C/C++ | 自研（本设计） | **无** | **nanorq C/MIT** |
| 内核 | 自研 C | 自研图/调度，风险高 | 低层 C 可移植，block 内存/CPU |
| 适用 | HND0 生产主路径 | 算法原型/研究 | block fallback/对照 |

## 5. 给 fec-core2 的输入

### 4.1 推荐顺序

1. **RaptorQ（nanorq）作为 block fallback/对照**：MIT/C11/无 libc，唯一有 golden 向量的 C 实现；先做 host 互操作与 HND0 小规模 K=64/128/256、T=1400/8192、overhead 2–5% 的解码 CPU/内存/延迟测量。
2. **SC-LDPC 只做算法原型**：用 `npminh12` 的构造/BP 和 aff3ct 的 BP/peeling 参考；没有可复用 C codec，需自研图/窗口/调度；不作为 v1 主路径。
3. **RLNC 仍是主设计**：流式、逐包、低延迟；RaptorQ/SC-LDPC 是对照/备选，不替代 grouped 32×8 的生产路径。

### 4.2 RaptorQ 具体参数建议

- K=64/128/256；T=1400（低延迟）或 8192（低开销）；overhead=2–5%（RaptorQ 的 repair 开销）。
- 内存按 §3.1 表；K=256/T=8192 约 2.74 MB/块，K=1024/T=8192 约 10.3 MB/块。
- block 延迟：K×T / app；K=256/T=8192 @750M ≈ 22 ms，@500M ≈ 33 ms；再加解码。
- 测试：nanorq `t/60encode.t` 的 K=10/100/1000 向量；自建 K=64/128/256 的 HND0 参数向量；解码丢包 2–10%。

### 4.3 SC-LDPC 具体输入

- 参考：`npminh12` 的 `(l,r,L,M)` 构造/BP；aff3ct 的 `Decoder_LDPC_BP_peeling` + `Channel_binary_erasure`。
- 需要自研：protograph/coupling/lifting、窗口同步、BP/peeling 调度、尾比特、packet header、测试向量。
- 风险：没有 C/C++ 生产实现；BP 迭代/图存储 CPU；内核无浮点（BEC 可硬判决，但码设计/调度仍复杂）；不建议 v1。

### 4.4 许可证结论

| 代码 | 许可证 | 与 GPLv2 内核 | 可用方式 |
|---|---|---|---|
| nanorq | MIT | **兼容** | 可直接移植低层 C（保留版权） |
| aff3ct | MIT | **兼容** | 算法/测试参考；C++ 不能进内核 |
| orq | MIT | **兼容** | C++ 老代码；只参考 |
| `npminh12` | MIT | **兼容** | C++ 仿真；只参考 |
| libcoding | BSD-3 | **兼容** | 源码缺失；不可用 |
| IT++ | GPL-2.0 | 兼容 | C++/BLAS 老库；无 SC-LDPC |
| foxsen/raptorQ | Apache-2.0 | **不兼容** | 只参考/独立实现 |
| cberner/raptorq | Apache-2.0 | **不兼容** | Rust；只参考 |
| libRaptorQ | LGPLv3/GPLv3 | **不兼容** | C++/Eigen；只参考 |
| SCA-LDPC | 无 license | 不可用 | 不采用 |

## 6. 参考文献

- aff3ct：`https://github.com/aff3ct/aff3ct`，commit `e1b5296`；`src/Module/Decoder/LDPC/BP/Peeling`、`src/Module/Channel/Binary_erasure`。
- IT++：`https://sourceforge.net/projects/itpp/`，4.3.1（2013-07-06），GPL-2.0。
- `npminh12/Spatially-coupled-LDPC`：`https://github.com/npminh12/Spatially-coupled-LDPC`，commit `3d403f4`；`spatiallycoupledLDPC.cpp`。
- `aarond10/libcoding`：`https://github.com/aarond10/libcoding`，commit `6ba5b66`；`ldpc_test.cc`（源码缺失）。
- nanorq：`https://github.com/sleepybishop/nanorq`，commit `7d52fbe`（`ursoid-tech/nanorq` 同 commit）；`include/nanorq_core.h`、`t/60encode.t`、`t/assets/examples/encode/K_*.txt`。
- foxsen/raptorQ：`https://github.com/foxsen/raptorQ`，commit `53727d2`；`test.cpp`。
- libRaptorQ：`https://github.com/LucaFulchir/libRaptorQ`，commit `a394b22`；`test/`。
- orq：`https://github.com/olanmatt/orq`，commit `0e85e54`；`test/`。
- cberner/raptorq：`https://github.com/cberner/raptorq`，commit `73e4e97`。
