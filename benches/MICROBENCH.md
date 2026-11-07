# 微基准：tbtun 的性能反馈闭环

1 GiB 长测回答的是"功能是否端到端正确"，一次要几分钟到几十分钟，不能作为
调优的反馈信号。本目录的微基准把数据路径拆成一组 1 秒级的操作，用同一台
机器、同一个二进制、同一份 CSV 回答"这次改动让哪一段变快或变慢了"。

两类输入：

- **host 微基准**（`host/bench_micro`）：不需要真机、不需要 root、不需要
  加载模块。链接模块使用的同一批纯函数源文件，覆盖 FEC、crypto、replay、
  NACK、scheduler 和 capture 记账。一次 `--quick` ≤ 2 s，`--full` ≤ 10 s。
- **真机短测**（`benches/short.sh`）：在 near/far 上跑 5 s 的单流，覆盖
  app/wire 吞吐、retx、NACK/replay、HOL、CPU 和 carrier stall。只在 near
  空闲时运行；1 GiB 验收期间不要碰它。

两者输出同一种五列 CSV：

```
name,iterations,ns_per_op,mb_per_s,checksum
```

## 1. 快速开始

```sh
make bench                 # host --quick，<= 2 s，结果在 benches/results/<stamp>/
make bench-full            # host --full，<= 10 s，用于记录基线

scripts/microbench.sh run --quick --tag my-change
scripts/microbench.sh baseline --full          # 写入 benches/baseline/host.csv
scripts/microbench.sh compare                  # baseline vs 最新一次结果
scripts/microbench.sh compare A.csv B.csv --threshold 5
```

`run` 每次写一个新的 UTC 时间戳目录：

```
benches/results/20261107T142936Z-base1/
  host.csv     五列结果，可直接 diff
  host.log     人读表格和运行时长
```

`compare` 按 `name` 连接两份 CSV，输出每行的 ns/op 与 MB/s 变化：

```
name,base_ns,new_ns,delta_ns_pct,base_mb_s,new_mb_s,delta_mb_pct,checksum,verdict
fec.encode.n4m4.w8.k4.s1500,4499.132,4570.660,+1.6,1333.590,1312.721,-1.6,same,stable
```

`verdict` 的规则：ns/op 上涨超过阈值（默认 10%）为 `REGRESS`，下降超过
阈值为 `IMPROVE`，只有一侧有该行为 `NEW`/`MISSING`，其余 `stable`。
`checksum` 为 `CHANGED` 说明输入或输出语义变了，此时先看正确性，再看速度。
报告同时保存到新结果目录的 `compare-<stamp>.txt`。

脚本不重试。构建或运行失败就退出并留下半成品，重试会把方差藏起来。

## 2. 指标含义

| 列 | 含义 |
|---|---|
| `name` | 基准名。`model.` 前缀表示内核 static 函数的 host 镜像，其余是链接真实源文件或真实 inline 头文件。 |
| `iterations` | 所有测量轮的总操作数。用于判断样本量，不用于跨次比较。 |
| `ns_per_op` | 每轮 `总时间 / 操作数` 的中位数（quick 5 轮，full 7 轮）。比较时只看这一列。 |
| `mb_per_s` | 每次操作搬运的载荷字节 / 中位耗时，10^6 字节/秒。不搬载荷的操作（调度、扫描、记账）为 0。 |
| `checksum` | 输入或初始状态的 64 位 FNV-1a。与迭代次数无关，因此同一二进制两次运行的该列必须相同；变化即语义变化。 |

`ns_per_op` 是每轮均值的**中位数**，不是最小值的平均：一次中断、一次频率
切换只会污染一轮，不会污染整行。

## 3. 基准清单

### 链接真实源文件（模块同一对象）

| 行 | 被测函数 | 说明 |
|---|---|---|
| `fec.encode.n4m4.w8.k4.s1500` | `tbt_fec_rlnc_encode` | 4 DATA + 4 PARITY，w=8，S=1500，L=4 |
| `fec.encode.n4m4.w8.k8.s1500` | 同上 | L=8 的整组编码 |
| `fec.decode.n4m4.w8.k4.erase1.s1500` | `tbt_fec_rlnc_decode` | 擦 1 个 DATA，恢复 |
| `fec.decode.n4m4.w8.k8.erase1.s1500` | 同上 | L=8 擦 1 |
| `fec.grouped64.encode_one_group.n4m4.w8.s1500` | `tbt_fec_grouped_ctx` + encode | 生产形状 64 组（256 carriers）下的单组编码 |
| `fec.hdr.seal_verify.s1500` | `tbt_fec_hdr_seal` + `verify` | 40 B 头 + 1500 B 载荷的两次 CRC32C |
| `fec.replay.store_get.s1500` | `tbt_fec_replay_store/get` | 1540 B 记录入环并取回 |
| `fec.replay.get_miss.never_stored` | `tbt_fec_replay_get` | 常见 NACK miss（块在环内、该 share 未存） |
| `fec.bitmap.count.256` | `tbt_fec_bitmap_count` | 256 bit 位图计数 |
| `fec.nack.due.new` | `tbt_fec_nack_due_at` | 每个新缺口的状态机与两次 32 B mask 拷贝 |
| `crypto.body_xor.1500` | `tbt_crypto_body_xor` | DATA body XOR，1500 B |
| `crypto.body_xor.8192` | 同上 | 8192 B，FEC 载荷上限 |
| `crypto.body_xor_share.parity.1500` | `tbt_crypto_body_xor_share` | PARITY share，share_index 混入计数器 |

### 真实 inline（`tbt_tx_acct.h`）

| 行 | 说明 |
|---|---|
| `capture.account.8k` | 一次 8 KiB chunk 的 `fits` + `publish` + bitmap set/test。**不含** skb 分配与 copy_from_user，那两项只能在 guest/真机测。 |

### 内核 static 函数的 host 镜像（`model.`）

| 行 | 镜像的内核位置 | 说明 |
|---|---|---|
| `model.sched.pick.256` | `tbt_tx.c:tbt_sched_pick` | 256 carriers、64 帧窗口的 hot pick，含每 64 次一次 rebuild 的摊销 |
| `model.sched.rebuild.256` | `tbt_tx.c:tbt_sched_rebuild` | 256 条路径的插入排序，含 `tbt_sched_better` 的 inflight 比较与 tie 轮转 |
| `model.sched.stalled_mask.16` | `tbt_tx.c:tbt_path_stalled_mask` | 遍历 16 个 legacy ARQ 槽，判 `snd_una != snd_nxt` 且 RTO 未动 |
| `model.nack.scan.64groups` | `tbt_fec_data.c:tbt_fec_nack_scan` 的位图段 | 64 组、每组 8 share、每组缺 1 个，找第一个缺口 |

模型行不是链接内核代码，而是逐行镜像；`host/test_reorder.c` 与
`host/test_fec_out.c` 已经用同一方法。内核分支改了而模型没改，是缺陷：
`model.` 行的绝对值可以讨论，但同一份镜像内的回归是有意义的。

## 4. 首版基线（2026-11-07，当前 HEAD）

`--full` 两次运行，机器空闲。完整数据在
`benches/results/20261107T143222Z-base-a/host.csv` 与
`benches/results/20261107T143231Z-base-b/host.csv`，基线文件为
`benches/baseline/host.csv`（取自 base-a）。

| name | ns/op | MB/s |
|---|---:|---:|
| `fec.encode.n4m4.w8.k4.s1500` | 4537.832 | 1322.217 |
| `fec.encode.n4m4.w8.k8.s1500` | 2994.970 | 4006.718 |
| `fec.decode.n4m4.w8.k4.erase1.s1500` | 4199.761 | 1428.653 |
| `fec.decode.n4m4.w8.k8.erase1.s1500` | 2710.520 | 4427.194 |
| `fec.grouped64.encode_one_group.n4m4.w8.s1500` | 4495.243 | 1334.744 |
| `fec.hdr.seal_verify.s1500` | 4841.374 | 309.829 |
| `fec.replay.store_get.s1500` | 42.523 | 72431.797 |
| `fec.replay.get_miss.never_stored` | 2.247 | 0 |
| `fec.bitmap.count.256` | 65.820 | 0 |
| `fec.nack.due.new` | 5.610 | 0 |
| `crypto.body_xor.1500` | 1148.869 | 1305.632 |
| `crypto.body_xor.8192` | 5999.479 | 1365.452 |
| `crypto.body_xor_share.parity.1500` | 1146.094 | 1308.794 |
| `model.sched.pick.256` | 179.539 | 0 |
| `model.sched.rebuild.256` | 6249.527 | 0 |
| `model.sched.stalled_mask.16` | 8.004 | 0 |
| `model.nack.scan.64groups` | 1241.001 | 0 |
| `capture.account.8k` | 1.282 | 0 |

两次 full 的同名行差异 ≤ 3.0%，多数 ≤ 1.4%。读法：

- **FEC 编码/解码约 1.3-4.4 GB/s，但这是 `-O2` 的值。** 模块里
  `tbt_fec.c` 走 Kbuild 的 `-O2`；同一份代码用 `-O3` 编译时编码约 15 GB/s
  （`tbt_xor_into` 的逐字节循环被向量化）。内核通常关闭自动向量化，所以
  1.3 GB/s 更接近真机；它也是当前最值得看的数字：256 carriers 的每个
  super-block 都要过这条路径。
- **`fec.hdr.seal_verify` 只有 310 MB/s。** 两次 CRC32C 都是逐字节查表，
  1500 B 载荷约 4.8 µs。每 share 都做一次，是 FEC 路径上的固定税。
- **crypto 约 1.3 GB/s。** 与 1500 B / 8192 B 的尺寸几乎无关，说明开销在
  逐块 keystream 生成，不在调用边界。模块用 `-O3` 编译该文件，这里用 `-O2`；
  `host/bench_crypto` 的 `-O3` 数字是它的上界。
- **replay 命中 42.5 ns、miss 2.2 ns。** 环是 O(1) 槽位，不是查找结构；NACK
  路径的 miss 主要花在后续分配与重发，不在这里。
- **bitmap 计数 65.8 ns/次。** 32 字节逐位循环，是 NACK scan 每次都要付的。
- **scheduler rebuild 6.25 µs / 256 路径，pick 摊销 180 ns。** rebuild 每
  64 帧或 10 ms 一次；按 256 carriers 的条纹频率，它是 pick 的主要成本。
- **NACK scan 模型 1.24 µs / 64 组。** 每次扫描只走位图与缺口，不含后续
  `send_nack` 的分配与写队列。

## 5. 用微基准驱动调优

1. **先记录基线。** 空闲机上 `scripts/microbench.sh baseline --full`。基线
   文件是固定路径，后续 compare 默认用它。
2. **一次只改一个热点。** 例如只改 `tbt_xor_into` 的向量化/分块，或只改
   NACK scan 的位图遍历。
3. **quick 循环。** `make bench` 或 `scripts/microbench.sh run --quick`，
   2 秒内看到方向。`compare` 里只有超过阈值（默认 10%）的行才值得追。
4. **full 确认。** 抖动通常 1-5%；quick 的 ±10% 可能是噪声。用 `--full`
   跑 2-3 次，比较中位数，不看单次。
5. **真机短测确认端到端。** host 微基准变快不等于 wire/app 变好；例如
   减少一次内存拷贝可能在真机上被 carrier TCP 掩盖。等 near 空闲后用
   `benches/short.sh` 验证 32/64/256 carriers 的 app/wire 与 retx。
6. **把改进写回基线。** 确认无回归后重跑 `baseline --full`，并在
   CHANGELOG 记录改动与数字。

反馈闭环的判据不是"某个函数快了"，而是"host 微基准的对应行变快，且真机
短测的 app 吞吐/retx 不退化"。两者矛盾时，以真机为准，并检查模型是否漂移。

## 6. 噪声控制

- **固定 CPU。** `MICROBENCH_CPU=2 scripts/microbench.sh run --full` 会用
  `taskset -c 2` 绑定。绑核减少迁移，但不消除同核中断；基线机器上不要同时
  跑 QEMU、编译或另一个基准。
- **预热。** 每行先自适应校准迭代数（约 8 ms/40 ms），校准本身就是预热；
  测量轮从热缓存开始。
- **中位数。** quick 5 轮、full 7 轮，取每轮均值的中位数。不要用最小值
  （会挑到频率最高的瞬间），也不要用平均值（会被一次中断拉高）。
- **重复运行。** 跨进程的抖动比进程内大。正式基线跑 2-3 次 full；两次
  同名行差异超过 10% 时，先查机器负载，再信数字。
- **频率与功耗。** 笔记本的 boost 会漂移；有条件时用 `cpupower frequency-set
  -g performance`，并在同一电源状态下对比。
- **编译选项。** 微基准按模块的 `-O2` 编译 `tbt_fec.c`；不要用 `-O3` 的
  数字和模块对比。`crypto` 在模块里是 `-O3`，这里的行是下界。
- **不要跨机器比较绝对值。** 只比较同一台机器、同一内核、同一编译选项下
  的两份 CSV。

## 7. 真机短测

`benches/short.sh` 通过 ssh 驱动 near/far，每个形状跑一次 5 s 单流下载。
far 的 `far-raw-serve.py` 在 127.0.0.1:5210 提供文件，near 用一条 `curl`
连接公共监听，`--max-time 5` 到点结束。没有 iperf3，没有第二条连接。

```sh
benches/short.sh \
  --near root@192.168.123.82 --far root@45.11.1.62 \
  --near-ip 192.168.123.82 --far-ip 45.11.1.62 \
  --carriers 32,64,256 --duration 5 \
  --near-module /root/tbtun-0.2.0-fix27-20261107T133636Z/tbtun.ko \
  --url http://127.0.0.1:9001/accept-1g.bin \
  --baseline
```

它做的是：

1. near 未加载模块时 `insmod` 指定的 .ko（只加载，不卸载），确认
   `ip route get <far>` 走 eth0（缺规则时加 `pref 100 lookup main`）。
2. 两端 `echo flush`，按 `n=4 m=4 fec=on mode=grouped w=8 carriers=N
   crypto=on profile=http-kal` 写 `/proc/net/tbtun/config`；near 额外写
   `dev=eth0` 与 portblock，far 保留 `target=127.0.0.1:5210` 与 raw sender。
3. 等两端 `c3 up=N`（最多 180 s）。
4. 前后读两端 `stats`、`peers`、`/proc/stat`，跑一次 curl，算差值。

输出：

- `remote.csv`：五列格式。`iterations` 是 1 MiB 块数，
  `ns_per_op` 是每块耗时，`mb_per_s` 是 curl 的 `size_download / time_total`
  （应用吞吐），`checksum` 是应用字节数的十六进制。
- `remote-detail.csv`：carriers、app/wire 字节、wire/app、FEC replay 字节与
  ppm、NACK tx/rx、replay hit/miss、HOL 最大值、`fec_hol_wait`、
  `fec_flush_skip`、`stall_declared`、near/far CPU、两端 `fec_proto_fail`、
  实际耗时与 rc。
- `<name>.curl`：curl 的 `size_download time_total rc`。

CPU 列是**单核百分比**：`(busy_ticks 差值) / (elapsed × CLK_TCK) × 100`。
超过 100 表示用了不止一个核；4 核机器上 220% 约为整机的 55%。

`--baseline` 把本次 `remote.csv` 复制到 `benches/baseline/remote.csv`；
`--compare` 用 `scripts/microbench.sh compare` 与它比较，规则与 host 基线
相同。失败形状记 `rc != 0` 后继续，最后以非零退出；不重试。

安全边界：

- near 的地址由 `--near`/`--near-ip` 传入，不写死：重启后 DHCP 可能换
  IP，先用扫描确认新地址，再把它同时传给这两个参数（`--near-ip` 是 far
  的 peer 锚点，必须与 near 实际地址一致）。
- near 的硬件喂狗者不碰；脚本只读 `/dev/watchdog0` 的持有者。
- **1 GiB 验收期间不要运行**：near 被验收进程占用时，carrier 与 CPU 都是
  共享的，数字没有意义。
- 首次真机基线见 `benches/baseline/remote.csv`（本文件下一节记录）。

### 真机基线（32/64/256 carriers，5 s 单流下载）

**0.2.0 的最终代码是 fix28；它的基线保留在
`benches/baseline/remote-fix28.csv`。当前基线已切到 fix31（见下文
0.2.1 节）。** fix29 是 0.3.0 实验：
32/64 有改善，但 256 的 retx/replay/NACK 中位一致偏高 20-35%，不进入
0.2.0。fix29 的数据保留在 `benches/results/` 与
`benches/baseline/remote-fix29.csv`，供 0.3.0 二分。

形状：n=4 m=4 grouped w=8 fec=on crypto=on http-kal，near RK3588
（aarch64 6.1.172，192.168.123.82）、far x86_64，每次 5 s 单流下载。

| 版本 | 状态 | near srcversion | far srcversion |
|---|---|---|---|
| fix28 | **0.2.0 基线** | `449268BA3066E72B3F0A282` | `2B4947A8AD7902C3CFAF760` |
| fix29 | 0.3.0 实验（256 retx 回归） | `9F0A26F960F994479F36DEC` | `1BBF6F9F8D32936E5A80B68` |

| carriers | fix28 Mbit/s | fix28 wire/app | fix28 retx% | fix28 far CPU | fix29 Mbit/s | fix29 wire/app | fix29 retx% | fix29 far CPU | Δ app |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | **26.81** | 2.0689 | 3.3% | 114.6% | 25.58 | 2.2057 | 4.3% | 115.0% | -4.5% |
| 64 | **40.68** | 2.2326 | 7.0% | 132.2% | 43.62 | 2.1863 | 5.7% | 129.2% | +7.2% |
| 256 | **60.39** | 2.1913 | 13.3% | 167.0% | 57.03（中位） | 2.494（中位） | 18.8%（中位） | 166.9% | -5.6% |

fix28 的其余基线指标：

| carriers | app MB/s | NACK tx/rx | replay hit/miss | HOL us | hol_wait | flush_skip | stall | near CPU | proto near/far |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 32 | 3.351 | 20/20 | 67/2 | 0 | 0 | 0 | 0 | 47.8% | 0/0 |
| 64 | 5.085 | 21/21 | 216/0 | 0 | 0 | 0 | 0 | 77.6% | 0/0 |
| 256 | 7.549 | 18/19 | 612/26 | 0 | 0 | 0 | 0 | 90.6% | 0/0 |

CPU 是单核百分比；far 167% = 1.67 核。

**procfs 死锁修复从 fix28 起有效，fix29 也保持。** 64 与 256 各跑完 5 s
后：无 D 状态进程、`refcnt=0`、`rmmod` 返回 0、重新 `insmod` 后 srcversion
不变。旧版 64 carriers 的死锁证据保留在
`benches/results/20261107T145223Z-near-deadlock/`，作为回归样本。

**fix29 的 256 二分（0.3.0 参考）。** 4+4 次重复，原始数据
`benches/results/20261107T220100Z-remote-256-bisect/bisect-256.csv`：

- app 中位 fix29 57.03（55.34-60.39）vs fix28 60.39（55.36-60.39），
  -5.6%，范围重叠；fix29 方差更大。
- wire/app 中位 2.494 vs 2.378（+4.9%），retx 18.8% vs 15.0%（+26%），
  replay 826 vs 686（+20%），NACK 27 vs 20（+35%）；far CPU 166.9% vs
  165.2%。
- c9 的 `fec_payload_crc_fail`/`fec_hdr_crc_fail`/`fec_proto_fail` 全 0，
  排除数据损坏与 fec_frame 缓冲覆盖；更可能是时序效应（word-wise XOR 与
  fec_frame 复用让发送更集中，触发更多 NACK/replay）。
- 0.3.0 要定位到单项：分别构建 "fix28 + XOR only" 与
  "fix28 + fec_frame only" 的 ko，在 256 下 A/B。

重启前 32 carriers 的对照（near 运行 15 小时 + 64 死锁残留 + far fix27）：
app 0.472 MB/s、wire/app 3.9512、replay 81.6%、near/far CPU 5.2/6.4 核。
重启 + fix28 后同一形状为 3.351 MB/s、2.07、3.3%、0.48/1.15 核；**6-7 倍
的差异不能全部归因于单一变量**，但长跑后的 near 状态本身就是主要成本。

near 重启确认：boot_id `0a91d2eb-…` → `d6999c60-…`；重启后模块未加载、
`/proc/net/tbtun` 不存在、9001 空闲、`tbtun-wdt-hold` inactive（transient
服务不自动恢复，硬件狗未武装，保持不武装）。DHCP 把地址从 .65 换到 .82；
脚本用 `--near`/`--near-ip` 传入新地址。

### 0.2.1（fix30）：直连 P32 对照（历史，已被 fix31 取代）

fix30 把 auto window 的上限放开：32 carriers 的 `tx_window` 从 fix28 的
378 KB 提升到 8 MB，wire 吞吐随之接近直连。near srcversion
`AE36B83CF5304ECAB6A3CB2`，far `E4BE181A81D3825E7EA88AC`。原始数据
`benches/results/20261107T223300Z-fix30-direct32-vs-tunnel/fix30-compare.csv`。

对照方法：far 临时在 `0.0.0.0:5212` 用 Python 并发 HTTP sender 提供同一个
1 GiB 源；near 用 32 条并发 curl 直连 5 s。隧道用 32 carriers 单流下载
5 s。两者同一时段、同一源。

| 路径 | app MB/s | app Mbit/s | wire Mbit/s | wire/app | near CPU | far CPU | near 核/Mbit | far 核/Mbit |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 直连 P32 | 28.637 | 229.1 | 229.1 | 1.000 | 76.4% | 147.6% | 0.00334 | 0.00644 |
| 隧道 32 | 12.289 | 98.3 | 223.6 | 2.275 | 86.0% | 142.0% | 0.00875 | 0.01445 |
| 隧道 64 | 11.953 | 95.6 | 215.8 | 2.256 | 81.4% | 141.0% | 0.00851 | 0.01475 |
| 隧道 256 | 7.130 | 57.0 | 139.4 | 2.443 | 85.8% | 155.0% | 0.01505 | 0.02719 |

32 carriers 的比值：wire 是直连的 **97.6%**，app 是直连的 42.9%（即
直连/2 的 **85.8%**）；每 Mbit 的 near/far CPU 是直连的 2.62/2.24 倍。
隧道 32 的 `tx_window=8,388,608 B`、`bdp=132,709 B`、`k=3`、
`rtt_mean=176,468 us`；far 侧 32 carriers 的 `snd_wnd` 合计 25.6 MB。
NACK 29/29、replay 148/0、retx 2.0%、HOL/flush/stall/proto 全 0。

判定：

- **32 达标**：wire 接近直连 P32（97.6%），app 达到直连/2 的 85.8%；
  剩余的 14% 是 FEC parity 之外的 framing/CPU 成本，量级合理。
- **64 达标**：app 95.6 Mbit/s，wire 215.8 Mbit/s（直连的 94.2%）；
  与 32 持平，说明窗口 8 MB 已足够覆盖 64 carriers 的 BDP。
- **256 不达标**：app 57.0 Mbit/s（直连/2 的 49.8%），wire 139.4
  Mbit/s（直连的 60.8%），比 64 低 40%。`tx_window` 仍被 8 MB 上限卡住，
  而 256 carriers 的 `snd_wnd` 合计 15.3 MB、`cwnd` 31.6 MB；retx 13.5%、
  far CPU 155%、`txq_max` 8.45 MB（队列打满）。窗口上限与 256 的每
  carrier 开销共同限制了扩展。
- **0.2.1 结论**：32/64 达标；256 未达标。若 0.2.1 的验收包含 256，
  需要继续放大 `tx_window`（至少覆盖 256 carriers 的 BDP）并降低
  256 下的 retx；否则 256 只能按“可用但低于 32/64”记录。

### 0.2.1（fix31）：batch/per-carrier 并行后的三档（当前基线）

**当前基线文件 `benches/baseline/remote.csv` 与 `remote-detail.csv` 是
fix31 的三行。** fix30 对照保留在 `benches/baseline/remote-fix30.csv`，
fix28（0.2.0）在 `remote-fix28.csv`，fix29（0.3.0 实验）在
`remote-fix29.csv`。

fix31（near `B52199BAC7DF4D9A206AA5F`，far `8898C4C2C05DFABAD2D47C4`）
在 256 上大幅改善。原始数据
`benches/results/20261107T231300Z-fix31-compare/fix31-compare.csv`。

| 路径 | app Mbit/s | wire Mbit/s | wire/app | retx% | near CPU | far CPU | near 核/Mbit | far 核/Mbit |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 直连 P32 | 224.2 | 224.2 | 1.000 | - | 76.1% | 132.5% | 0.00339 | 0.00591 |
| 隧道 32 | 107.6 | 242.5 | 2.254 | 1.9% | 82.0% | 152.2% | 0.00762 | 0.01414 |
| 隧道 64 | 118.2 | 265.0 | 2.242 | 3.8% | 90.4% | 159.0% | 0.00765 | 0.01345 |
| 隧道 256 | 97.3 | 215.8 | 2.218 | 4.9% | 90.6% | 146.6% | 0.00931 | 0.01507 |

对比 fix30：32 98.3→107.6（+9.5%），64 95.6→118.2（+23.6%），
256 57.0→97.3（+70.7%）。相对直连/2（112.1 Mbit/s）：32 为 96.0%，
64 为 105.4%，256 为 86.8%。256 的 wire/app 从 2.44 降到 2.22，retx 从
13.5% 降到 4.9%；`tx_window=16 MB`、`txq_max=16.8 MB`（仍满）、
`rtt_mean=190,968 us`、far `snd_wnd` 合计 27.1 MB。

稳定性：32/64/256 跑完后无 D 状态进程、`refcnt=0`、`rmmod` 返回 0、
重新 `insmod` 后 srcversion 不变。

**0.2.1 判定**：32 达标（直连/2 的 96.0%），64 达标（105.4%），
256 达到直连/2 的 86.8%，比 fix30 的 49.8% 大幅改善，可视为基本达标；
若验收要求 ≥90%，256 还差 3.2 个百分点。

**0.3.0 剩余（256）**：`txq_max=16.8 MB` 仍打满，说明发送侧写 carrier
的速度仍是限制；far CPU 1.47 核（每 Mbit 0.0151 核，是直连的 2.5 倍）；
FEC parity 让 wire/app 固定在 2.22-2.25。下一步优先查 TX 队列的
batch/per-carrier 并行是否还能提高 drain 速率，其次才是 FEC/HTTP 的
每字节成本。fix29 的 word-wise XOR 与 fec_frame 复用仍留 0.3.0 二分，
不要在 256 直接叠加。

## 8. 已知限制

- `model.` 行是镜像，不是链接内核代码。内核改了分支而模型没改，数字会
  骗人；改内核调度/NACK 逻辑时同步改 `host/bench_micro.c` 的模型。
- `capture.account.8k` 只测记账纯函数，不含 skb 分配、`copy_from_user`、
  公共 socket 唤醒。这些只能由 guest/真机短测覆盖。
- 微基准测的是稳态热路径，不含建连、HELLO、carrier 拨号、join、慢启动。
  端到端的第一秒由真机短测负责。64/256 的 join 在 fix28/fix29 下约 10 s
  内完成，旧版的 procfs 死锁已由 fix28 修复（证据见 near-deadlock 目录）。
- 0.2.0 的基线文件 `benches/baseline/remote.csv` 是 fix28；fix29 的数据
  保留在 `benches/baseline/remote-fix29.csv` 与
  `benches/results/20261107T220100Z-remote-256-bisect/bisect-256.csv`，
  仅作 0.3.0 参考，不代表 0.2.0 行为。
- 真机 `checksum` 是字节计数，不是内容哈希；内容正确性由 1 GiB 验收与
  host 功能测试负责。
- 基线是单次 full 运行（内部已取 7 轮中位数）。跨进程方差用两次 full
  的对比衡量；正式调优前建议再跑一次 `baseline --full` 确认机器状态。
