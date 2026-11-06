# node65 目标机只读侦察:Embedfire LubanCat-5 V2(RK3588)

- 目标:`root@192.168.123.65`,公钥直连成功(BatchMode,未使用密码)
- 时间:2026-11-06;方式:**纯只读**,未跑 benchmark / iperf / FEC / 未加载模块 / 未安装 / 未改网络
- 结论先行:**可以移植并构建 tbtun,但当前 master 需要一个明确的 6.1 适配补丁(`system_dfl_wq` 在 6.1 不存在,源码有 16 处引用);256-carrier 受 ABI 硬限 16 限制;1 Gbps 下的滑窗 RLNC 只在设计稿的 GF(2) 小参数下成立;且该机是带生产负载的共享设备。**

---

## 1. 平台身份

| 项 | 值 |
|---|---|
| 设备树 model | `Embedfire LubanCat-5 V2` |
| compatible | `embedfire,rk3588-lubancat-5-v2` / `rockchip,rk3588` |
| 主机名 | `lubancat` |
| 架构 | **aarch64**(ARMv8-A,64-bit,LE) |
| OS | Ubuntu 26.04.1 LTS(resolute) |
| 内核 | `6.1.172`(自编译,`#1 SMP Sat Oct 17 15:47:17 CST 2026`) |
| 编译内核的编译器 | `aarch64-linux-gnu-gcc 15.2.0`,`GNU ld 2.46` |
| uptime / load | 14 天 22 小时 / 0.35 0.37 0.24 |
| 温度 | 35–37 °C(全部 thermal zone) |

不是 x86,也不是 MIPS。RK3588 SoC,双 GMAC,无独立网卡(无 PCIe 设备可见,`lspci` 空)。

### 1.1 与同网段其他候选的关系

- `192.168.123.74` 已被 `w-arm64-recon` 判定为 **x86_64(AMD Athlon X4 760K)**,不是 ARM64,不能作本项目的 ARM64 验收平台。
- `192.168.123.65`(本机)是同网段目前唯一确认的 **真 aarch64** 目标。
- 本次侦察之前,主仓库、`docs/`、`review/` 与所有 worktree 都没有 RK3588 / LubanCat / aarch64 记录;本报告是首份。

## 2. CPU / ISA

big.LITTLE,8 核,全部在线:

| cluster | 核 | part | 频率 | capacity | cache |
|---|---|---|---|---|---|
| little | cpu0–3 | `0xd05` = Cortex-A55,r2p0 | 408–1800 MHz | 414 | L1d/L1i 32K each,L2 128K |
| big | cpu4–7 | `0xd0b` = Cortex-A76,r4p0 | 408–2304 MHz | 1024 | L1d/L1i 32K each,L2 128K |

- L3:3072 KiB(1 instance)
- `scaling_governor=performance`,当前大核 2304 MHz 满频,小核 1800 MHz
- ISA flags:`fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp`
  - **有 NEON(`asimd`)**、AES、PMULL、CRC32、FP16(`asimdhp`)、dot-product(`asimddp`)
  - **无 `sve` / `sve2`**
- `NR_CPUS=8`,`CONFIG_SMP=y`

算力粗算:A76 2.3 GHz × 4 + A55 1.8 GHz × 4。A76 是 ARMv8.2,有 128-bit NEON 与 `tbl` 16 路字节查表,适合做 GF(2^8) 表驱动乘法;但没有 SVE 的 256/512-bit 宽度。

## 3. 内存 / 存储

- `MemTotal` 8,104,768 kB ≈ **7.73 GiB**;`MemAvailable` 5,014,864 kB ≈ **4.78 GiB**
- Swap 2 GiB(几乎未用)
- HugePages:0(hugepage 2 MiB 可用但未预留)
- 根分区 `/dev/mmcblk0p2` 58 G,eMMC,**42 G 可用**
- `/tmp` 是 tmpfs,3.9 G(已用 2.4 G)
- `/boot/firmware` 488 M

## 4. 内核与可加载模块能力

| 检查 | 结果 |
|---|---|
| `/lib/modules/6.1.172/build` | → `/usr/src/linux-headers-6.1.172`(存在) |
| headers 完整性 | `Makefile`、`Module.symvers`、`arch/arm64`、`scripts/mod/modpost`、`include/config/auto.conf` 全在,66 M |
| `CONFIG_MODULES` | `=y` |
| `CONFIG_MODVERSIONS` | `=y`(外部模块须用本机 `Module.symvers` 构建 → 已具备) |
| `CONFIG_MODULE_SIG` | **not set**(无需签名) |
| `CONFIG_MODULE_COMPRESS_*` | `NONE=y`(直接 `.ko`) |
| `CONFIG_MODULE_UNLOAD` / `FORCE_LOAD` | `=y` / `=y` |
| `CONFIG_LOCALVERSION` | `""` |
| `/proc/sys/kernel/modules_disabled` | `0` |
| `CONFIG_KPROBES` | **not set**(本机不支持 kprobe) |
| `CONFIG_DEBUG_INFO_BTF` | 未启用(无 BTF) |
| `CONFIG_CC_IS_CLANG` | 未设置(内核用 gcc 构建) |
| DKMS | 未安装 |

已加载模块里有 `tun`(used by 2),`sch_fq_codel`,`ipv6`,`squashfs` 等。**没有** `tcp_brutal` / `tbtun` 痕迹(`modinfo tcp_brutal` 报 not found,无 tbtun 二进制)。

> `CONFIG_KPROBES not set` 对 tbtun **无影响**:`tbtun/tbt_core.c:93-101` 明确说明模块只调用 `EXPORT_SYMBOL(_GPL)` 的符号(`tcp_sendmsg_locked`、`tcp_read_sock`、`sock_create_kern`、`kernel_*` 等),早期的一发式 kprobe 已移除。

## 5. 构建工具链

| 工具 | 版本 |
|---|---|
| gcc | 15.2.0(Ubuntu 15.2.0-16ubuntu1)—— 与构建内核的版本一致 |
| make | GNU Make 4.4.1(aarch64) |
| ld | GNU ld 2.46 |
| pkg-config | 2.5.1 |
| clang | **未安装** |

开发包:
- 有:`build-essential`、`linux-headers-6.1.172`、`linux-libc-dev`、`libelf-dev`、`libssl-dev`、`libzstd-dev`、`zlib`、`libzstd`
- 无:`libbpf-dev`、`libnl-3-dev`、`libnl-genl-3-dev`、`/usr/include/bpf`、`/usr/include/libnl3`
- 只有运行时库:`libbpf1`、`libnl-3-200`、`libnl-genl-3-200`、`libllvm21`

## 6. 网卡

两块 SoC 内建 MAC,驱动 `st_gmac`(device tree 为 `rockchip,rk3588-gmac` + `snps,dwmac-4.20a`,OF 节点 `/ethernet@fe1c0000`):

| iface | 状态 | 速率 | MTU | 队列 | ring | 备注 |
|---|---|---|---|---|---|---|
| eth0 | UP,载波 | **1000 Mb/s full** | 1500 | 1 RX / 1 TX(硬件最多 2) | RX 512 / TX 512(最大 1024) | 主用口,192.168.123.65/24 |
| eth1 | DOWN,NO-CARRIER | 未知(能力 10/100/1000) | 1500 | 1/1 | 512/512 | 备用口,未接线 |
| usb0 | DOWN | — | 1500 | — | — | USB gadget |
| Meta | UP | 10 Mb/s(虚拟) | 9000 | — | — | mihomo TUN,198.18.0.1/30 |

eth0 offload:
- `rx-checksumming: on`,`tx-checksumming: on`(ipv4/ipv6 on,ip-generic fixed off)
- `tcp-segmentation-offload: on`,`generic-segmentation-offload: on`,`generic-receive-offload: on`
- `large-receive-offload: off [fixed]`
- `scatter-gather: on`;`tx-nocache-copy: off`
- `ntuple-filters: off [fixed]`

eth0 能力/计数:
- `ethtool -T`:支持硬/软收发时间戳,PTP Hardware Clock 0
- 驱动统计为 DesignWare MMC 计数器(`mmc_tx_octetcount_gb` 等)
- `/proc/net/dev` 累计:RX 15,069,723,197 B / 13,732,446 pkt,**drop 138,672**;TX 3,101,447,242 B / 6,498,145 pkt

## 7. 网络配置(只读观察)

- 路由:唯一默认路由 `default via 192.168.123.254 dev eth0 proto dhcp metric 100`
- `192.168.123.0/24 dev eth0`;`198.18.0.0/30 dev Meta`(fake-ip 段)
- `ip_forward = 0`
- qdisc:`eth0` = `mq` + `fq_codel`(limit 10240p, target 5ms, interval 100ms, ecn)
- **nftables / iptables 规则为空**(无 NAT/过滤规则)
- sysctl:`rmem_max = wmem_max = 212992`(208 KiB),`tcp_congestion_control = cubic`,可用 `reno cubic`
- 中断:`eth0` 两个 GICv3 中断(266/265),累计 5,921,802 次**全部落在 CPU0**;`smp_affinity_list=0-7`,但计数集中在 cpu0 → 实际单核收包
- RPS 关闭(`rps_cpus=00`,`rps_flow_cnt=0`)
- 邻居表中有同网段主机:`192.168.123.69`、`.72`、`.74`、`.253`、`.254`(网关 `dc:d8:7c:d6:82:0e`)

## 8. 环境噪声(重要,影响任何后续实验)

该机**不是空闲测试床**,正在跑:

- `mihomo`(Clash.Meta):监听 127.0.0.1:7890、UDP,持有 `Meta` TUN(198.18.0.0/30、MTU 9000)
- `mediamtx`:UDP 8000/8001/8189(RTSP/WebRTC 媒体服务器)
- `ffmpeg` × 5 个进程,各开两路 UDP(Recv-Q 达 410,112 B,约 400 KB 积压)→ 有持续视频推流
- `frpc`、`rclone`、`Xvfb`、`node`(106 MB RSS)、`atop`、`systemd-journald`
- 内存已用 3.0 G / 7.7 G,`/tmp` 已用 2.4 G

含义:
1. 带宽与 CPU 均被占用,任何吞吐/延迟测量都不可信,且实验流量可能与用户业务互相干扰。
2. `Meta` 接口 + fake-ip 段意味着默认路由之外的流量可能被 mihomo 接管;若后续实验要保证走物理 `eth0`,需先确认路由/策略,而不能靠默认路由推断。
3. eth0 已有 13.8 万 RX drop,基线本身不干净。

## 9. tbtun 可行性

**结论:可以移植,但不是零改动。当前 master 在 6.1.172 上会编译失败,阻断点唯一且集中:`system_dfl_wq`。**

### 9.1 已满足的条件

- `Makefile` 是标准 out-of-tree kbuild:`obj-m += tbtun.o`,`make -C /lib/modules/$(uname -r)/build M=$(PWD) modules`。目标机的 `build` 链接、`Module.symvers`、`modpost`、`auto.conf` 全部就绪。
- 内核与 headers 由同一 gcc 15.2.0 构建,目标机 gcc 也是 15.2.0;`CONFIG_MODVERSIONS=y` 下符号版本匹配可满足。
- `CONFIG_MODULE_SIG` 未启用,不需要签名;`CONFIG_MODULE_COMPRESS_NONE=y`,不需要 `zstd`。
- 目标内核**未启用 `CONFIG_DEBUG_INFO_BTF`**,因此不需要 `pahole`;这一点比仓库当前的 Manjaro 7.2.9 构建环境更简单(`docs/build-env.md` 里的 pahole 约束在此不适用)。
- 模块不依赖 kprobe(`CONFIG_KPROBES not set` 无碍),也不依赖 BPF、netlink、genl、tun/tap。
- 源码 `tbtun/*.c`、`tbtun/*.h` 中**没有**任何 `arm_neon.h` / `immintrin.h` / `__m256` / AVX / SSE / `__aarch64__` / `__x86_64__` 条件编译 → 纯可移植 C,到 arm64 无架构障碍。`tbt_http.h:41-43` 的 `x86_64` 只是 User-Agent 字符串。
- 所需导出符号在 6.1.172 的 `Module.symvers` 中全部存在:`tcp_sendmsg_locked`(`EXPORT_SYMBOL_GPL`)、`tcp_read_sock`、`tcp_prot`、`sock_create_kern`、`kernel_bind`、`kernel_connect`、`kernel_listen`、`kernel_accept`、`sock_release`、`kernel_sock_shutdown`、`kernel_sendmsg`(均 `EXPORT_SYMBOL`)。
- `sock_replace_proto` 在 6.1 存在:`include/net/sock.h:1983` 的 `static inline`(inline 不进 `Module.symvers`,所以符号表里查不到是正常的)。
- `MODULE_LICENSE("GPL")`;仓库里现有的 `tbtun.ko` 是 x86-64 产物(`ELF 64-bit LSB relocatable, x86-64`),**不能**用于本机,必须重编。

### 9.2 阻断点:`system_dfl_wq`(6.1 无此符号)

- 在 `/usr/src/linux-headers-6.1.172/include/` 中 grep `system_dfl_wq`:**零命中**。
- 源码实际引用 16 处:`tbt_carrier.c:1613`;`tbt_session.c:818,877,948`;`tbt_sock.c:412,415,650,720`;`tbt_tx.c:1578,1938,1970,2195,2578,2580,2659`。
- 全仓没有任何 `LINUX_VERSION_CODE` / `KERNEL_VERSION()` 条件编译 → 在 6.1 上会直接报未声明标识符。
- `DESIGN.md:1743-1744` 自己写明:`system_dfl_wq` 在 6.1 不存在,"7.2.9 is the build target and 6.1 is not supported"。
- 6.1 可用的 workqueue 有 `system_wq`、`system_highpri_wq`、`system_long_wq`、`system_unbound_wq`、`system_freezable_wq`、`system_power_efficient_wq`。7.x 对 `system_dfl_wq` 的定义是 **unbound、workers 不绑 CPU、非 concurrency-managed、未到 max_active 即执行**,语义最接近的是 `system_unbound_wq`;但 `max_active` 与优先级不完全相同,**替换前需核对排队属性,不能假定完全等价**。
- 最小补丁形态:一个版本兼容宏(6.1 下映射到 `system_unbound_wq`,新内核保留 `system_dfl_wq`),或按版本条件编译 16 处引用。

### 9.3 文档与版本门的矛盾(移植前必须修正)

- `README.md:49`、`README.zh.md:38`、`scripts/install_dkms.sh:21`(`MIN_KERNEL=6.1`)声称支持 6.1+。
- `DESIGN.md:1743-1744` 声明 6.1 不支持。
- `scripts/install_dkms.sh:78-95`、`:334-337` 的版本门只比较 major.minor,**拦不住 6.1 上的编译失败**——它会让目标机先通过检查,再在 `make` 阶段报错。

### 9.4 仍需构建时验证的风险(本次未编译)

- `tbt_carrier.c:1485` 的 `tbt_tcp_prot = tcp_prot;` 是整结构体拷贝;6.1 与 7.2.9 的 `struct proto` 字段集不同,字段名差异会在编译期暴露。
- `tcp_sendmsg_locked` 在 6.1 的签名与 7.2.9 是否一致,需实际编译确认。
- 这些属于适配工作量,不是架构障碍;arm64 本身没有额外问题。

## 10. 256 carriers / 1 Gbps 滑窗 RLNC 可行性

### 10.1 当前代码的硬边界(master `0db5166`)

- `tbtun/tbt_uapi.h:113`:`TBT_MAX_CARRIERS_LIMIT 16`,注释为 "path_ack_mask is 16 bits"
- `tbtun/tbt_core.c:36`:`max_carriers` 参数范围 `1..16`,默认 16
- `tbtun/tbt_uapi.h:137`:`TBT_WINDOW_BODY_LEN = 8 + 8*16 + 2 = 138`
- `tbtun/tbt_uapi.h:131`:`TBT_FBK_MAX = 18*16 = 288`
- master 上**没有** FEC/RLNC:无编码矩阵、无 `fec`/`rlnc`/`galois` 符号。`tbt_carrier.c:54` 的 "M spares" 是**复制冗余**,不是编码冗余;`交接.md:481-482` 也写明"无 FEC:只有复制与重放"。

### 10.2 既有设计已给出扩展路径(不是本报告的新提案)

`w-window` 分支的 `DESIGN.md:987-1025` 已把 16 槽定性为 ABI 硬限,并列出改动面:

- `BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16)`;ARQ 的 `avoid_mask`、`path_mask`、`reserve_mask`、`announced_mask`、`path_gen[]`、`path_end[]`、`first_sent_seq[]`、`una_observed_ns[]`、`sched.order[]` 与 FIN/RST 的 `unsigned long` 掩码全是 16 位宽
- 扩到 256 需要 256-bit 掩码,并把 FBK(288 B)与 WINDOW(138 B)分块或扩大;文档将其定性为数据面改动,推迟到 FEC 块模型
- 已按 256 设计好的部分:缓冲预算 `TBT_CAR_SOCKBUF_TOTAL = 128 MiB`,256 carriers 时每 carrier 512 KiB 有效,用 `SO_SNDBUFFORCE`/`SO_RCVBUFFORCE` 绕过默认 208 KiB 的 `wmem_max`/`rmem_max`;`TBT_PORTMAP_MAX = 4096`;per-stream 限制;procfs 渲染
- 资源估算:256 个 TCP socket × 5 个 work 结构(poll/pump/rx/tx/connect)、256 × ~24 KiB HTTP/parser ≈ 6 MiB、per-carrier 50 ms poll = **5120 wakes/s** → 必须先合并为 session 级扫描(20 wakes/s),keepalive 也要批处理

`w-fec` 分支的 `review/fec-design.md` 是设计稿(host 原型在 `review/fec-prototype/`,内核 `tbt_rlnc.c` 尚未写),关键数字:

- 码型是**流式 GF(2) 稀疏 RLNC,纯 XOR,没有 GF(2^8) 乘加**
- 生产参数 `W = 2K = 448` 时 `d` 需 100–358,25–89 XOR/byte,1G 下 3–11 GB/s → **数学上不成立**
- 1G arm64 可行参数:`N=16, M=4, K=16, W=32, d=16` → **4 XOR/byte,1G 下约 500 MB/s**
- 低速率:`W≤32, d≤16` → 2–4 XOR/byte,所有架构可行
- 动态码率 `M = clamp(ceil(256×(p+0.09)), M_min, M_max)`,`N = 256-M`,epoch 1 s、滞回 8
- wire format 是独立的 FEC 格式(40 B 头、`share_index` u16、`epoch` u16、`n/m` u8、256 位反馈 bitmap),不复用旧 16-carrier 的帧头/FBK/WINDOW
- **设计稿内部不一致**:开头写目标版本 `TBT_CONV 0x0006`,§0 表格写 `conv 0x1001`(`fec-design.md:14` 与 `:20`);实现前需作者澄清

### 10.3 本机能力对照

- 物理上限:1GbE,TCP 实测通常 ≤ 940 Mbps(1500 B MTU 约 78–83 kpps)
- 算力:4×A76 @2.3 GHz + 4×A55 @1.8 GHz;NEON 128-bit,**无 SVE**
- 500 MB/s 的 XOR 处理量:纯 C 的 XOR 在 A76 上受内存带宽限制约 4–8 GB/s,500 MB/s 占 6–12%;NEON 后更低 → **可行**
- 3–11 GB/s 的编码量超过 4×A76 的总和 → **不可行**(与 `fec-design.md` 的结论一致)
- 收包侧是短板:单 RX/TX 队列、eth0 两个中断计数全在 CPU0、RPS 关闭;1 Gbps 收包本身已占 A76 单核约 30–40%,编码必须分散到其它核才有余量
- 256 carriers 的 5120 wakes/s 若照现状每 carrier 独立 poll,8 核会被唤醒本身吃掉;合并扫描是启用 256 的前置条件

### 10.4 结论

1. **可移植但非零改动**:tbtun 本体是纯可移植 C,headers、工具链、签名策略都满足;唯一编译阻断是 6.1 缺少 `system_dfl_wq`,需一个集中补丁(见 §9.2),不改内核配置。
2. **256 carriers 不是 CPU 问题,是 ABI 与调度问题**:当前硬限 16,需要数据面改动;内存与 socket 数量在本机可承受(128 MiB 缓冲预算、8 GiB 内存),但 poll/keepalive 必须先批处理。
3. **1G + 滑窗 RLNC 在设计稿的可行参数下成立**:GF(2) XOR、`N=16/M=4/K=16/W=32/d=16`、约 500 MB/s,本机 CPU 有空间;生产参数 `W=448` 不成立。
4. **本机是合适的移植与低参数验证目标,不是现成的 1G×256 验证床**:瓶颈在单队列收包、CPU0 中断集中、208 KiB 默认 socket 缓冲上限,以及生产负载干扰。

## 11. 后续若要实测,需先解决的前置条件(本次未执行)

1. **先修 6.1 适配**:把 `system_dfl_wq` 换成 `system_unbound_wq`(或加版本兼容宏),并修正 `README.md:49` / `DESIGN.md:1743-1744` 的版本声明矛盾与 `install_dkms.sh` 只比较 major.minor 的版本门。
2. 目标机是生产共享机:mihomo / mediamtx / 5 路 ffmpeg 会污染测量,应协商停流或换空闲机。
3. 内核无 BTF、无 clang、无 libbpf-dev / libnl-3-dev;若引入 BPF 工具链需安装。
4. eth0 中断绑在 CPU0、RPS 关闭;要做 1 Gbps 级实验,应先评估 IRQ affinity / RPS(写操作,本次禁止)。
5. `rmem_max`/`wmem_max` 仅 208 KiB,同网段低 RTT 够用,跨 WAN 需调大(写操作)。
6. 若要 256 carriers,需先改协议位图与长度常量,并重估 `max_tunnels` 上限;同时把 per-carrier poll 合并为 session 级扫描。

## 12. 原始输出摘要(关键行)

```
uname -a: Linux lubancat 6.1.172 #1 SMP Sat Oct 17 15:47:17 CST 2026 aarch64 GNU/Linux
nproc: 8
lscpu: Cortex-A55 x4 (1800 MHz, cap 414) + Cortex-A76 x4 (2304 MHz, cap 1024)
flags: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp
free -m: total 7914, used 3019, available 4895
df: /dev/mmcblk0p2 58G 13G 42G 24% /
/lib/modules/6.1.172/build -> /usr/src/linux-headers-6.1.172
CONFIG_MODULES=y CONFIG_MODVERSIONS=y # CONFIG_MODULE_SIG is not set
# CONFIG_KPROBES is not set
gcc 15.2.0 / GNU Make 4.4.1 / GNU ld 2.46 / pkg-config 2.5.1 / clang 缺失
eth0: st_gmac, 1000Mb/s full, mtu 1500, queues rx-0/tx-0, ring 512/512 (max 1024)
offloads: tso on, gso on, gro on, lro off[fixed], rx/tx checksum on, sg on
irq 71 (eth0): 5,921,802 次,全部在 CPU0
default via 192.168.123.254 dev eth0 proto dhcp metric 100
qdisc: mq + fq_codel
nft/iptables: 空
net.core.rmem_max = 212992; net.ipv4.tcp_congestion_control = cubic
TBT_MAX_CARRIERS_LIMIT 16 (tbt_uapi.h:113); TBT_WINDOW_BODY_LEN 138; TBT_FBK_MAX 288
tbtun 源码: 无 kprobe 依赖、无 SIMD/架构条件编译;MODULE_LICENSE("GPL")
grep system_dfl_wq /usr/src/linux-headers-6.1.172/include/ -> 零命中(6.1 无此符号)
sock_replace_proto -> include/net/sock.h:1983 static inline(6.1 存在)
tcp_sendmsg_locked -> EXPORT_SYMBOL_GPL; tcp_read_sock/tcp_prot/sock_create_kern/kernel_*/sock_release/kernel_sendmsg -> EXPORT_SYMBOL
6.1 可用 workqueue: system_wq, system_highpri_wq, system_long_wq, system_unbound_wq, system_freezable_wq, system_power_efficient_wq
```

## 13. 本次明确未做的事

未跑 benchmark、iperf/iperf3、FEC、任何发包/测速;未加载/卸载模块;未安装软件;未改路由、防火墙、sysctl、qdisc、IRQ affinity;未重启;未写入目标机任何文件。除 SSH 会话本身外未产生实验流量。

---

## 14. 当前状态快照(2026-11-06 17:43 CST,验收口径 = arm64 端)

本次只做状态确认,未跑 benchmark、未做 256-carrier、未 push。额外产生的网络动作仅:3 个 ICMP 到 HND0、1 个 TCP/22 探测、1 次 DNS 解析。

### 14.1 主机与登录

| 项 | 值 |
|---|---|
| 地址 | `192.168.123.65`(私网,NAT 后;主表默认网关 `192.168.123.254`) |
| SSH | `root@192.168.123.65`,公钥直连 OK(BatchMode) |
| 当前登录 | `w` 仅 1 个 root 会话,来源 `192.168.123.69`(疑为主控机);`who` 为空 |
| 主机名 | `lubancat`;设备 `Embedfire LubanCat-5 V2`(RK3588) |
| uptime | 15 天 9 小时 |

### 14.2 架构 / 内核 / 内存

- `uname -m` = **aarch64**,`uname -r` = **6.1.172**
- 8 核:`Cortex-A55 ×4`(≤1800 MHz)+ `Cortex-A76 ×4`(≤2304 MHz),`nproc=8`
- 内存 7914 MiB total,**5404 MiB available**;swap 2047 MiB(已用 91 MiB)
- 无 WiFi:`/sys/class/net` 只有 `eth0`、`eth1`、`usb0`、`Meta`、`lo`;`iw` 未安装,`nmcli` 显示 `eth0 ethernet connected`、`eth1 ethernet unavailable`。**没有 wlan 设备**。

### 14.3 网络速率 / 双工

| iface | 状态 | 速率 | 双工 |
|---|---|---|---|
| eth0 | UP,载波 | **1000 Mb/s** | **full** |
| eth1 | DOWN,NO-CARRIER | -1(未知) | unknown |
| usb0 | DOWN | — | — |
| Meta | UP(mihomo TUN) | 10 Mb/s(虚拟) | full |

### 14.4 负载 / 进程 / 其他 worker

- load average:**0.00 / 0.02 / 0.00**(`1/331` 可运行/总任务),**当前空闲**
- 无 `iperf` / `iperf3` / `tbtun` / `bench` / `fec` 进程;无相关 systemd unit
- 常驻业务:`mediamtx`(CPU 3.8%)、`ffmpeg ×6`(各 0.3–0.4%,全部推 `rtsp://127.0.0.1:8554/camN`,本地回环,不占物理带宽)、`mihomo`、`frpc`、`rclone`、`atop`
- **其他 worker 的痕迹(均已退出,当前无活动)**:
  - `/tmp/tbtun-rk3588.lock`(root,0 字节,11-06 07:11)—— 说明曾有 rk3588/tbtun 任务在此机加锁运行
  - `/root/.bash_history` 记录了 `uname -a`、`ip addr`、`cat /proc/cpuinfo`,以及向 `authorized_keys` 注入 `hatch` 的 ed25519 公钥
  - `/root/.omp/natives/18.16.0`(11-06 06:09)
  - `omp-renice.service` 是 10-22 起就常驻的优先级服务,不是活动实验
- 结论:**没有正在跑的实验**;但有前人留下的锁文件与公钥,接手前应确认锁的语义(建议删除或确认无 flock 持有者;本次未删,只读)。

### 14.5 构建工具链 / 模块

| 项 | 值 |
|---|---|
| gcc | 15.2.0(Ubuntu 15.2.0-16ubuntu1) |
| make | GNU Make 4.4.1 |
| headers | `/lib/modules/6.1.172/build -> /usr/src/linux-headers-6.1.172`(存在) |
| 已装 tbtun | **未加载**;`lsmod` 无 `tbtun`,`modinfo tbtun` = not found,`/lib/modules/6.1.172` 下无 `tbtun*.ko` |
| 模块 arch | 该机没有任何 tbtun 模块产物;仓库里的 `tbtun.ko` 是 **x86-64**,不能用于本机 |
| 内核可加载模块目录 | 标准 `kernel/net/ipv4/*.ko` 齐全(含 `tcp_bbr.ko` 等),无第三方模块 |

编译阻断仍是 `system_dfl_wq`(§9.2):6.1.172 无此符号,源码 16 处引用。

### 14.6 HND0 可达性

| 检查 | 结果 |
|---|---|
| DNS `hnd0.kw92.cyou` | → `2a02:6ea0:d32f::f380:f495`(仅 IPv6 记录) |
| TCP `45.11.1.62:22` | **OK**(timeout 5 s 内建连) |
| ICMP `45.11.1.62` | 3 发 2 收,**33% 丢包**,RTT `302.4 / 308.9 / 315.3 ms` |
| 路由 | `ip route get 45.11.1.62` → **`via 198.18.0.2 dev Meta table 2022`**,即**经 mihomo 代理**,不是物理 eth0 |
| 主表 | `default via 192.168.123.254 dev eth0`(物理路径存在,但被 `ip rule 9002` 的策略路由覆盖) |
| 旁证 | `mihomo` 有一条 ESTAB:`[2408:...]:52730 → [2a02:6ea0:d32f::f380:f495]:7000` —— **HND0 同时是 mihomo 的代理节点**,所以本机到 HND0 的流量天然被它接管 |

### 14.7 判断:能否作为 near

**可以,但有三个前置条件。**

支持理由:
- 验收口径要求 arm64 端,本机是当前唯一确认的 aarch64 目标;8 核 A76/A55、7.7 GiB 内存、1 GbE 全双工,规模匹配。
- 当前 load 0.00,无实验进程;ffmpeg/mediamtx 走本地回环,对物理网卡吞吐影响小。
- near 主动连接 far,位于 NAT 后不构成障碍(与 JYSZ 实验的 near 角色一致)。
- HND0 的 TCP/22 可达。

必须先解决:
1. **构建补丁**:`system_dfl_wq` → `system_unbound_wq`(或版本兼容宏),否则 6.1.172 上编不过(§9.2)。
2. **绕开 mihomo**:当前到 `45.11.1.62` 的路由是 `table 2022 via 198.18.0.2 dev Meta`。若直接跑,载体 TCP 会被本机 mihomo 代理,HND0 看到的源地址与路径都不是物理 WAN。需要为 far 地址加 `ip rule`/`ip route` 绕行(与 JYSZ 实验的 `ip rule 绕行` 做法一致),或临时停 mihomo。**这是写操作,本次未做。**
3. **NAT 会话容量**:near 发起 N+M 条 TCP 连接。当前 16 carriers 无压力;若做 256,需评估出口 NAT 的会话表与端口映射上限(本次未测)。

不建议直接用当前状态跑 benchmark 的原因:load 虽低,但 HND0 路径经代理、RTT 302–315 ms 且 33% ICMP 丢包,测出的数字不代表物理 WAN;且本机有前人留下的锁与业务进程,需先确认归属。
