# 候选目标 192.168.123.74 只读侦察

状态：只读侦察，未运行任何负载。
时间：2026-11-06 05:5x，目标机本地时间同一时区。
访问：`ssh root@192.168.123.74` 公钥直接通过（BatchMode，无密码交互）。本文不含任何凭据。

## 0. 结论

**它不是 ARM64。** 这是一台 x86_64 的老桌面机：AMD Athlon X4 760K（Richland，2013），
Gigabyte F2A85XM-D3H，4 核锁在 1.8 GHz，Realtek 千兆网口，Manjaro + 内核 7.3.0-rc1。

对"256 carriers / 1G 滑窗 RLNC"的判定：

| 维度 | 判定 | 依据 |
|---|---|---|
| ARM64 验收平台 | **不可用** | `uname -m` = `x86_64`；无 `asimd`/`neon`/`sve` 标志（x86 不存在这些概念） |
| 功能验证（跑通 N+M 数据面） | 可用，但有前置 | 需先装 `linux73-headers`；当前无内核构建目录 |
| 1 Gbit/s goodput 验收 | **不可用** | 网口本身只有 1 Gbps，且 FEC 的 `wire_per_app > 1`；上限约 0.8–0.9 Gbps 线速，扣 FEC 后更低 |
| FEC XOR 算力 | 有余量 | AVX1 `vpxor` + 4 核；1G 档 4 XOR/byte 远低于内存带宽 |
| 内存 | 充足 | 19.5 GiB，256 carriers 的窗口缓冲不构成约束 |
| 内核模块构建 | **当前不可行** | `/lib/modules/7.3.0-rc1-2-MANJARO/build` 与 `source` 均不存在 |

一句话：**它是 x86_64 的对照机，不是 ARM64 的目标机。** 可以当"低端 x86 下界"来跑功能与
相对性能，但不能拿它替代 arm64 的 1G 验收。

## 1. 平台事实

| 项 | 值 |
|---|---|
| hostname | `760K` |
| `uname -a` | `Linux 760K 7.3.0-rc1-2-MANJARO #1 SMP PREEMPT_DYNAMIC Fri, 04 Sep 2026 15:43:39 +0000 x86_64 GNU/Linux` |
| `uname -m` | `x86_64` |
| 发行版 | Manjaro Linux（rolling，`ID_LIKE=arch`） |
| 主板 / BIOS | Gigabyte F2A85XM-D3H / F4e，2014-01-07 |
| 内存 | 19.5 GiB（`MemTotal: 20417708 kB`），swap 21.4 GiB，THP `always` |
| 根分区 | `/dev/sda2` 933G，已用 480G，可用 453G |
| 温度 | k10temp `temp1_input` = 35.5 °C |
| 负载 | `load average: 1.09, 1.04, 1.27`，uptime 3 天 22 小时，26 个 running service |

## 2. 架构与 ISA

```
Architecture:  x86_64            Vendor ID: AuthenticAMD
Model name:    AMD Athlon(tm) X4 760K Quad Core Processor
CPU family 21, Model 19, Stepping 1, microcode 0x6001119
CPU(s): 4        Thread(s) per core: 1     Socket(s): 1     NUMA: 1 node
L1d 64 KiB（4 实例，16K/核）   L1i 64 KiB（2 实例）   L2 2 MiB（2 实例，共 4 MiB）   无 L3
```

- 频率：`cpuinfo_min_freq = cpuinfo_max_freq = bios_limit = 1800000`，实测 1796–1806 MHz。
  `scaling_driver = acpi-cpufreq`，governor `schedutil`，`boost=1` 但无可用上限。
  **1.8 GHz 是 BIOS 限制，不是热降频**（温度 35.5 °C）。760K 标称 3.8 GHz / 4.1 GHz boost，
  也就是说这台机器只发挥了约 47% 的标称主频。若要用它做任何性能结论，先在 BIOS 里确认
  Cool'n'Quiet / 倍频设置。
- 关键 flags（有）：`sse4_1 sse4_2 ssse3 popcnt aes pclmulqdq avx fma fma4 xop tbm bmi1 sse4a`
- 关键 flags（无）：`avx2`、`avx512*`、`neon`、`asimd`、`sve`、`crc32`（ARM 语义的）
- 结论：**x86_64，AVX1 上限**。任何依赖 AVX2/AVX-512 或 NEON 的优化路径在这里都要走标量/SSE 分支。
  仓库当前 `tbtun/` 与 `Makefile` 中没有任何 `__m256`/`neon`/`march=native` 代码，所以这一点
  不阻塞现状，只影响将来给 FEC 写 SIMD 时的分支选择。

## 3. 内核、模块与构建能力

| 项 | 值 |
|---|---|
| 内核 | `7.3.0-rc1-2-MANJARO`，`PREEMPT_DYNAMIC`，`HZ=1000`，`NR_CPUS=8192` |
| `/lib/modules/7.3.0-rc1-2-MANJARO/` | 存在，6442 个 `.ko` |
| `.../build`、`.../source` | **不存在** |
| `/proc/config.gz` | 可读（70 KB） |
| `modules_disabled` | `0` |
| `tainted` | `12288` = `O`（out-of-tree）+ `E`（unsigned） |
| `lsmod` | 64 行；`tun`（used by 2）、`sch_fq`、`tcp_diag`、`inet_diag` 已加载 |

内核配置要点：

```
CONFIG_MODULES=y            CONFIG_MODULE_SIG=y        CONFIG_MODULE_SIG_ALL=y
CONFIG_MODULE_SIG_SHA512=y  CONFIG_MODULE_SIG_FORCE 未设置   CONFIG_MODVERSIONS 未设置
CONFIG_TUN=m  CONFIG_VETH=m  CONFIG_NF_TABLES=m  CONFIG_NET_SCH_FQ=m  CONFIG_NET_SCH_CAKE=m
CONFIG_XDP_SOCKETS=y  CONFIG_BPF_SYSCALL=y  CONFIG_NET_SCH_FQ_CODEL=y  CONFIG_NET_SCH_FQ_PIE=m
```

即：签名不强制（`FORCE` 关闭，Secure Boot 下 `lockdown = [none]`），所以自编译的未签名模块
可以加载——`taint` 里的 `E` 证明已有第三方模块（`linux73-nvidia-open`）在跑。`MODVERSIONS`
未启用，减少了一类 ABI 摩擦。

工具链：

```
gcc 16.2.1 20260810      make 4.4.1        ld 2.47（binutils）     pkg-config 3.0.7
python3 3.14.7           git 2.55.0        openssl 3.6.4
无 clang；无 perf；无 ethtool；无 tcpdump；无 aarch64 交叉工具链；无 qemu-aarch64
有 nft、iperf3（本次未运行）
```

**阻塞点**：`Makefile` 的默认目标是 `$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules`，
`KERNEL_DIR = /lib/modules/$(uname -r)/build`。该目录不存在，`linux73-headers` 未安装
（`pacman -Q` 只有 `linux73`、`linux-api-headers`、`base-devel`）。所以在不改系统的前提下，
**`make` 不能产出 `tbtun.ko`**。本次按"只读"要求没有安装、没有构建。

用户态部分（`host/test_*`、`review/fec-prototype/`）不依赖 `KERNEL_DIR`，只要允许写盘就能编译；
本次同样没有执行。

## 4. 网络

| 项 | 值 |
|---|---|
| 接口 | `enp2s0`（唯一物理口），`Meta`（mihomo 的 TUN，198.18.0.1/30，mtu 9000，虚拟） |
| 驱动 / 设备 | `r8169`，`10ec:8168`，subsys `1458:e000`，RTL8111/8168/8411 rev 06 |
| 速率 / 双工 | `1000` Mbps / `full`（sysfs `speed`、`duplex`） |
| MTU | **1492**（不是 1500），`tx_queue_len = 1000` |
| PCIe | `LnkCap: Speed 2.5GT/s, Width x1`；`LnkSta: 2.5GT/s, x1`（PCIe 1.0 x1） |
| 队列 | 只有 `rx-0` / `tx-0`；`rps_cpus = 0`（RPS 关闭）；`napi_defer_hard_irqs=1`，`gro_flush_timeout=20000` |
| 中断 | IRQ 46（`enp2s0`）累计 265,681,269 次，**全部落在 CPU2**；`smp_affinity_list = 0-3` 但实际未分散 |
| 计数 | rx 168.8M 包 / 34.35 GB；tx 207.5M 包 / 260.16 GB；`rx_dropped=39505`；无 error/crc/fifo/miss |
| softnet | 四个 CPU 的 `dropped` 列全 0 |

地址与路由：

```
192.168.123.74/24 dev enp2s0            default via 192.168.123.254（DHCP，metric 100）
2408:8226:276:6ce0::/64                  default via fe80::1（RA）
qdisc: fq（enp2s0、Meta 都是 fq，无自定义 class/filter）
nft list ruleset: 空（0 行）             iptables: 三链全 ACCEPT
nf_conntrack: 未加载（/proc/sys/net/netfilter/nf_conntrack_count 不存在）
```

sysctl 相关：

```
net.core.default_qdisc = fq            net.ipv4.tcp_congestion_control = bbr
net.core.rmem_max = 16777216           net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 131072 33554432      net.ipv4.tcp_wmem = 4096 16384 4194304
net.ipv4.tcp_mtu_probing = 1           net.ipv4.tcp_fastopen = 3
net.ipv4.tcp_ecn = 2                   net.ipv4.tcp_timestamps = 1
net.core.somaxconn = 4096              net.ipv4.ip_local_port_range = 32768 60999
```

三点值得注意：

1. **MTU 1492**。上游大概率是 PPPoE，或有人在 NetworkManager 里手工设过。FEC 的包长预算、
   `W_send` 的字节换算都要按 1492 算，不要假设 1500。改 MTU 属于写操作，本次未动。
2. **单队列 + IRQ 全绑 CPU2 + RPS 关闭**。1 Gbit/s 的软中断已经压在一个核上（该 IRQ 计数
   2.65 亿）。在这台机器上跑 1G 级实验，CPU2 会先饱和，四核实际只剩三核给编码与协议栈。
3. **PCIe 1.0 x1（2.5 GT/s）**。单向有效带宽约 200 MB/s，对千兆（125 MB/s）够用但无余量；
   它不是 2.5G/10G 口，也没有第二条物理链路。

背景负载（会影响任何测量的噪声源）：`mihomo`（127.0.0.1:7890 等）、`frpc`（多个 127.0.0.1 端口）、
`ollama`（11434）、一个 python（5080）、一个 node（7676）。`load average ≈ 1.1` 说明已有一个核
的常驻占用。

## 5. 对 256 carriers / 1G 滑窗 RLNC 的逐项判断

参照 `review/fec-design.md`（w-fec 分支）的预算口径：可行档 `N=16, M=4, K=16, W=32, d=16`
是 4 XOR/byte；1 Gbit/s 下约 500 MB/s 的 XOR 吞吐；`M=32/N=224, d=16` 在 mips 上约 15–30% 单核。

| 需求 | 这台机器 | 判断 |
|---|---|---|
| 256 条 carrier 的会话状态与窗口缓冲 | 19.5 GiB 内存，4 核 | 充足。内存不是约束 |
| 1G 线速的 XOR 吞吐（约 0.5 GB/s） | AVX1 `vpxor`，4 核 × 1.8 GHz；DDR3 双通道约 25 GB/s 量级 | **有余量**。瓶颈不在算力，在网口与单核软中断 [推算，未实测] |
| 1G 应用层 goodput 验收 | 物理口 1000 Mbps；FEC `wire_per_app > 1` | **做不到**。理论天花板 ≈ 900/wire_per_app Mbps |
| ARM64 特有的 NEON/SVE 路径验证 | 无 | **无法验证** |
| 构建 `tbtun.ko` | 无 `build/`，无 headers | **当前不能**，需装 `linux73-headers` |
| 256 carriers 的端口/五元组规模 | `ip_local_port_range` 32768–60999（约 28k 端口） | 够 256 条，但同一目标 5-tuple 会受 conntrack/端口耗尽影响（本机 conntrack 未加载） |

补充一条：`M=32/N=224, d=16` 这一档在 mips 上是 15–30% 单核，x86 单核 IPC 通常高 2–3 倍，
所以在这台机器上大致是 5–15% 单核 [推算]。真正吃掉 CPU 的是 TCP 协议栈与那一个绑死的软中断核。

## 6. 与 ARM64 目标的关系

`w-window` 分支的结论是"256/1G 是 ARM64 目标，不是 MT7621 MIPS 端点"。这台机器两头都不是：

- 它比 MT7621 强得多（4 核 1.8 GHz、AVX、19 GB 内存），做功能验证和小参数实验很舒服；
- 它不是 arm64，不能替代 arm64 的 ISA 相关结论（NEON/SVE、arm64 内存序、arm64 内核路径）；
- 它只有 1G 口，连"1G goodput"这个验收项本身都放不下。

合适的定位：**低端 x86 对照机**。用来确认"同一份代码在慢机器上不会崩、比例关系是否成立"，
以及在 arm64 硬件到手前先把 256-carrier 的会话/端口/内存路径跑通。

## 7. 本次未做（按只读边界）

- 未跑任何 benchmark、iperf3、FEC、`make`、模块加载、安装、重启。
- 未改网络、防火墙、路由、MTU、qdisc、sysctl。
- 未测实际吞吐、延迟、offload 状态（`ethtool` 未安装，无法读 TSO/GSO/GRO/checksum/scatter-gather）。
  因此"r8169 支持哪些 offload"在本报告中**没有实测证据**，只有驱动与芯片型号。
- 未用密码登录；未在远端留下任何文件。

## 8. 原始输出摘要（关键行）

```
$ uname -a
Linux 760K 7.3.0-rc1-2-MANJARO #1 SMP PREEMPT_DYNAMIC Fri, 04 Sep 2026 15:43:39 +0000 x86_64 GNU/Linux

$ lscpu | head
Architecture: x86_64
Model name:   AMD Athlon(tm) X4 760K Quad Core Processor
CPU(s): 4     Core(s) per socket: 4     Socket(s): 1
CPU max MHz: 1800.0000    CPU min MHz: 1800.0000

$ cat /sys/devices/system/cpu/cpu0/cpufreq/bios_limit
1800000

$ free -m
               total        used        free      shared  buff/cache   available
Mem:           19939        1612        1218           4       17471       18326
Swap:          21934        1021       20912

$ ls -ld /lib/modules/$(uname -r)/build
ls: cannot access '/lib/modules/7.3.0-rc1-2-MANJARO/build': No such file or directory

$ ip -br addr
enp2s0  UP  192.168.123.74/24  2408:8226:276:6ce0:1d89:e19d:c11b:eff/64
Meta    UNKNOWN  198.18.0.1/30

$ cat /sys/class/net/enp2s0/{speed,duplex,mtu}
1000
full
1492

$ lspci -vv -s 02:00.0 | grep -E 'LnkCap|LnkSta|Kernel driver'
        LnkCap: Port #0, Speed 2.5GT/s, Width x1
        LnkSta: Speed 2.5GT/s, Width x1
        Kernel driver in use: r8169

$ tc qdisc show
qdisc fq 0: dev enp2s0 root refcnt 2 limit 10000p flow_limit 100p buckets 1024 ...

$ sysctl net.ipv4.tcp_congestion_control net.core.default_qdisc
net.ipv4.tcp_congestion_control = bbr
net.core.default_qdisc = fq

$ grep -c ^processor /proc/cpuinfo
4
```
