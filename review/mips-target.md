# MIPS 目标机侦察：192.168.123.254（JDC-1 / MT7621 / Padavan）

- 日期：2026-11-06
- 目标：`192.168.123.254`，用户确认为实际 MIPS 目标
- 方式：SSH 只读命令 + 本机只读网络探测
- 边界：未安装任何包，未重启，未改网络/防火墙/闪存，未加载 tbtun；唯一的写操作是在 `/tmp`（tmpfs）写 32 MiB 基准文件并立即删除
- 凭据：不记录、不落盘、不进仓库。设备侧账号口令只用于本次会话

标注约定：**【实测】** = 设备上命令的直接输出；**【推断】** = 由实测数据或公开规格推导，未在设备上直接验证。

---

## 0. 结论摘要

| 项目 | 结论 | 依据 |
| --- | --- | --- |
| SoC | MediaTek MT7621（`system type` 直接给出） | 实测 |
| 核心 | MIPS 1004Kc V2.15，2 物理核 × 2 VPE = 4 个逻辑 CPU，880 MHz | 实测 + 推断 |
| ISA | MIPS32r2 + `mips16` + **DSP ASE** + MT ASE；**无 MSA**；cpuinfo 无 FPU 行 | 实测 |
| 内存 | MemTotal 450768 kB（440 MiB）；512 MiB swap 在 eMMC | 实测 |
| 存储 | rootfs 13.5 MiB squashfs 只读且 100% 满；/tmp 39 MiB；/opt 97.6 MiB（剩 19 MiB） | 实测 |
| 内核 | 3.4.113 SMP，Padavan，2026-03-03 构建，gcc 4.4.7 | 实测 |
| 设备内编译 | **不可能**：无 gcc/make/头文件/内核构建树，rootfs 只读 | 实测 |
| 模块加载 | 可用：`modules_disabled=0`，151 个 `.ko`，`tainted=4096`（已加载过树外模块） | 实测 |
| 网卡 | `eth2`（交换机 CPU 口）、`eth3`（WAN）；无 ethtool，驱动不报 speed/duplex（EINVAL） | 实测 |
| 1G 链路 | 唯一插线的 PHY（phy2）显示 1000BASE-T 状态字；本机在 WiFi 上，未做有线端到端实测 | 推断（中置信度） |
| 转发模式 | **AP/桥接**：`br0 = {eth2, eth3, ra0, rai0}`，FDB 显示 WiFi 客户端与上游网关在同一个桥里 | 实测 |
| 1 Gbps 隧道 | **不可行**（CPU 端点上限约 300–500 Mbps，加 FEC 后更低） | 推断（有实测锚点） |
| 256 carriers | 协议**硬上限 16**（`BUILD_BUG_ON(... > 16)`）；OS/内存侧可行；吞吐无收益 | 实测（代码）+ 推断 |
| 滑窗 RLNC | 1 Gbps 下**不可行**（差 1–2 个数量级）；W≤8 时约 30–90 Mbps 量级 | 推断（有实测锚点） |
| tbtun 上机 | 需实质性向后移植（README 要求 Linux ≥ 6.1） | 实测（README）+ 推断 |

一句话：**这台机器是一台 880 MHz 双核 MIPS 桥接式路由器。它的 1G 口是 L2 交换意义上的 1G，不是 CPU 端点意义上的 1G；滑窗 RLNC 与 256 carriers 在这颗 CPU 上都不成立。**

---

## 1. 访问方式

- 公钥：本机 `~/.ssh/id_ed25519` 被拒（`Offering public key ... Authentications that can continue: publickey,password`）。
- 用户先前提供的 root 口令：dropbear 通告 `publickey,password`，客户端确实发出了口令包，随后被拒（`Permission denied`）。**未做任何重试或猜测。**
- 用户随后补充的 `admin` 账号：SSH 与 HTTP 均成功。口令不在此记录。
- 服务指纹：SSH 为 `dropbear_2025.88`；HTTP 为 `httpd`，`WWW-Authenticate: Basic realm="JDC-1"`，页面标题 `JDC-1 Wireless Router`。
- 设备自述：`nvram get productid` = `JDC-1`，`firmver` = `3.4.3.9`（Padavan 分支），主机名 `JDC-1`。

```
debug1: Remote protocol version 2.0, remote software version dropbear_2025.88
HTTP/1.0 401 Unauthorized
Server: httpd
WWW-Authenticate: Basic realm="JDC-1"
```

---

## 2. SoC / 核心 / ISA 【实测】

```
system type		: MediaTek MT7621 SoC
processor		: 0
cpu model		: MIPS 1004Kc V2.15
BogoMIPS		: 579.58
wait instruction	: yes
microsecond timers	: yes
tlb_entries		: 32
extra interrupt vector	: yes
hardware watchpoint	: yes, count: 4, address/irw mask: [0x0ffc, 0x0ffc, 0x0ffb, 0x0ffb]
ASEs implemented	: mips16 dsp mt
shadow register sets	: 1
kscratch registers	: 0
core			: 0
VPE			: 0
```

`processor` 0..3，`core` 依次为 0/0/1/1，`VPE` 依次为 0/1/0/1：

```
processor 0: core 0, VPE 0
processor 1: core 0, VPE 1
processor 2: core 1, VPE 0
processor 3: core 1, VPE 1
```

- **4 个逻辑 CPU，2 个物理核，每核 2 个 VPE（SMT）**。`/sys/devices/system/cpu/online` = `0-3`。
- 主频：cpuinfo 不报 MHz；`BogoMIPS 579.58` 是 MT7621A 在 880 MHz 下的典型值【推断】。设备不暴露 cpufreq（`/sys/devices/system/cpu/cpu0/cpufreq` 不存在），nvram 也没有 `clkfreq`。
- **ISA 扩展**：`mips16`、`dsp`、`mt`。**没有 `msa`**（1004Kc 本身不支持 MSA，与预期一致）。
  - `dsp` = MIPS DSP ASE，有整数 SIMD 指令（`addu.qb`、`dpau.h.qbr` 等），但 GCC 4.4.7 的自动向量化基本用不上，需要手写内联汇编或 intrinsics。
  - 没有 MSA，就没有 128-bit 向量寄存器；RLNC 的批量 XOR 只能走 32-bit GPR。
- **FPU**：cpuinfo 没有 FPU 行。MT7621 的 1004Kc 无 FPU【推断；设备没有 `/proc/config.gz`，无法确认 `CONFIG_CPU_HAS_FPU`】。
- 缓存：cpuinfo 不报。MT7621 规格为每核 32 KiB L1 I/D + 64 KiB L2【推断】。

---

## 3. 内存与存储 【实测】

```
MemTotal:         450768 kB
MemFree:          401808 kB
Buffers:            2152 kB
Cached:             8552 kB
SwapTotal:        524284 kB
SwapFree:         524284 kB
```

```
Filesystem                Size      Used Available Use% Mounted on
rootfs                   13.5M     13.5M         0 100% /
/dev/root                13.5M     13.5M         0 100% /
tmpfs                    39.0M      1.1M     37.9M   3% /tmp
tmpfs                    16.1M      2.2M     13.9M  13% /etc
/dev/loop0               97.6M     73.7M     19.0M  80% /opt
/dev/mmcblk0p1           98.5M     98.5M         0 100% /media/AiCard_01
```

```
Filename				Type		Size	Used	Priority
/dev/mmcblk0p5                          partition	524284	0	-1
```

- MemTotal 450768 kB = **440 MiB**（512 MiB 物理减去内核保留）。
- swap 512 MiB 在 `/dev/mmcblk0p5`；eMMC 为 `mmcblk0`，60416000 块 ≈ **57.6 GiB**。
- rootfs 是只读 squashfs，**100% 占满**；`/tmp` 是 39 MiB tmpfs（剩 37.9 MiB）；`/opt` 是 97.6 MiB 的 loop 镜像（剩 19 MiB）。
- 结论：放一个几十到几百 KiB 的 `.ko` 没问题（`/tmp` 或 `/opt`），但没有可写的持久分区承载内核构建产物。

---

## 4. 内核 / 模块 / 构建能力 【实测】

```
Linux JDC-1 3.4.113 #1 SMP Tue Mar 3 12:36:19 CST 2026 mips GNU/Linux
Linux version 3.4.113 (root@hiboy-VirtualBox) (gcc version 4.4.7 (GCC) ) #1 SMP Tue Mar 3 12:36:19 CST 2026
```

```
vermagic=3.4.113 SMP mod_unload MIPS32_R2 32BIT
vermagic=3.4.113 SMP mod_unload MIPS32_R2 32BIT
```

```
ls: /lib/modules/3.4.113/build: No such file or directory
drwxr-xr-x    3 admin    root            48 Mar  4  2026 /lib/modules/3.4.113
kernel
modules.dep
```

- 无 `/boot`，无 `/proc/config.gz`，无 `/proc/kallsyms`，无 `/lib/modules/3.4.113/build`，无内核头文件。
- 151 个 `.ko`，`modules.dep` 存在；`insmod`/`modprobe` 是 busybox applet；`/proc/sys/kernel/modules_disabled` = `0`。
- `/proc/sys/kernel/tainted` = `4096`（`TAINT_OOT_MODULE`）→ 设备上已经加载过树外模块，且未被拒绝。
- 编译器与工具：`gcc`、`cc`、`make`、`ld`、`as`、`ldconfig` 全部缺失；`ldd` 只是一个脚本；无 `python`/`perl`/`strace`/`gdb`/`ethtool`/`tc`/`nft`/`swconfig`/`iperf`。有 `openssl 3.0.20-dev`、`awk`、`jq`、`busybox`。
- 可用工具（节选）：`/sbin` 有 `mii_mgr`、`mii_mgr_cl45`、`brctl`、`iptables`、`insmod`、`modprobe`、`lsmod`；`/usr/sbin` 有 `dropbear`、`httpd`、`nvram`、`dnsmasq`；`/usr/bin` 有 `openssl`、`taskset`、`strings`。

**构建能力判断**

1. 设备内构建：**不可能**。没有编译器、没有内核头文件、没有构建树，rootfs 只读且已满。
2. 外部交叉编译：**原理上可行**。设备上的 openssl 就是 Padavan 工具链编出来的，编译参数直接可读：

```
compiler: /opt/rt-n56u/toolchain-mipsel/toolchain-3.4.x/bin/mipsel-linux-uclibc-gcc
  -fPIC -mabi=32 -Wa,--noexecstack -mips32r2 -march=mips32r2 -mtune=1004kc -O3
  -fomit-frame-pointer -pipe -DEMBED -Dlinux ...
```

   即 `mipsel-linux-uclibc-gcc 4.4.7`，小端，`-march=mips32r2 -mtune=1004kc`。
   vermagic 必须精确匹配 `3.4.113 SMP mod_unload MIPS32_R2 32BIT`：**无 `preempt`、无 `modversions`**；3.4 内核没有模块签名机制，所以不存在签名拦截。
3. **真正的障碍不是工具链，是 API**。README 写明 `Linux 6.1 or later`；`tbt_carrier.c` 依赖 `kernel_connect`、私有 `struct proto` + `tcp_sendmsg_locked`、`sk_data_ready` 上下文约束等。3.4.113 与 6.1 之间这些内部接口差异很大，需要一次实质性向后移植，而不是改几个宏。

---

## 5. 网卡 / 交换 / 链路

接口清单（`ifconfig -a` / `/proc/net/dev`）【实测】：

```
eth2  Link encap:Ethernet  HWaddr DC:D8:7C:D6:82:0E   Interrupt:11
      RX bytes:261182373875 (243.2 GiB)  TX bytes:44355078648 (41.3 GiB)
eth3  Link encap:Ethernet  HWaddr DC:D8:7C:D6:82:0F
      inet addr:192.168.1.254  Bcast:192.168.1.255  Mask:255.255.255.0
      RX bytes:92321130498 (85.9 GiB)  TX bytes:291478583622 (271.4 GiB)
ra0   (2.4G) Interrupt:12      rai0 (5G) Interrupt:32
wds0..3 / wdsi0..3 / apcli0 / apclii0：全为 0
```

中断分布（`/proc/interrupts`）【实测】：

```
           CPU0       CPU1       CPU2       CPU3
 11:         71  357258772          0          0  MIPS GIC  eth2
 12:         82          0   26745861          0  MIPS GIC  ra0
 31:          7          0          0          0  MIPS GIC  ralink_esw
 32:        288          0          0   50274021  MIPS GIC  rai0
 64:   77103856          0          0          0  MIPS GIC  ipi_resched
```

- 没有 `eth0`/`eth1`：`eth2` 是交换机 CPU 口（IRQ 11，全部落在 CPU1），`eth3` 是 WAN（无独立 IRQ 行）。
- 软中断与 IRQ 都挤在单核上（`eth2` → CPU1，`rai0` → CPU3），这对单流吞吐不利【推断】。
- **没有 ethtool**；`/sys/class/net/{eth2,eth3}/speed` 和 `duplex` 都返回 `Invalid argument`（3.4 的 MTK 驱动没实现 `get_settings`）；`link_mode=0`，`carrier=1`。
- 没有 `tc`，qdisc 不可查；`txqueuelen` = 1000。
- **offload 状态无法读取**（无 ethtool）：TSO/GSO/GRO/checksum 只能靠驱动默认值【未验证】。

链路速率只能从 PHY 寄存器间接判断（`mii_mgr -g -p N -r R`，只读）【实测】：

```
phy0.reg0=1040 phy0.reg1=7969 phy0.reg10=7800
phy1.reg0=1040 phy1.reg1=7969 phy1.reg10=7800
phy2.reg0=1040 phy2.reg1=7969 phy2.reg10=7cff
phy3.reg0=1040 phy3.reg1=7949 phy3.reg10=4000
phy4.reg0=1040 phy4.reg1=7949 phy4.reg10=4000
```

- 只有 `phy2` 的 1000BASE-T 状态字（reg 0x0A）与其它端口不同（`0x7cff` vs `0x7800`/`0x4000`），说明 **phy2 是唯一插线的端口**，且协商到 1000BASE-T【推断，中置信度】。
- 本机（`wlp4s0`）通过 WiFi 接入：`rx bitrate 162.0 MBit/s VHT-MCS 4 40MHz NSS 2`，`tx bitrate 260.0 MBit/s VHT-MCS 3 80MHz NSS 2`，信号 -65 dBm。**因此本次没有做有线 1G 端到端吞吐测试。**

---

## 6. 转发模式：这是一台桥，不是路由器 【实测】

```
bridge name	bridge id		STP enabled	interfaces
br0		8000.dcd87cd6820e		yes		ra0
							eth2
							eth3
							rai0
```

```
port no	mac addr		is local?	ageing timer
  1	02:51:22:e5:67:53	no		   0.64
  4	14:ab:02:53:c0:ef	no		   0.38
  3	60:a3:e3:4e:04:00	no		  10.84
  1	94:de:80:be:00:22	no		   0.96
  2	c8:8a:9a:b3:c7:ed	no		   0.00
  2	ce:81:da:35:00:30	no		  11.78
  3	da:7f:f1:ed:ce:70	no		   4.67
  3	dc:d8:7c:56:82:0e	yes		   0.00
  4	dc:d8:7c:d6:82:0f	yes		   0.00
```

端口号 1=`eth2`，2=`rai0`，3=`ra0`，4=`eth3`。关键两行：

- `port 2  c8:8a:9a:b3:c7:ed` = 本机 `wlp4s0` 的 MAC，学在 **rai0**（5G）上；
- `port 4  14:ab:02:53:c0:ef` = 上游网关 `192.168.1.1` 的 MAC，学在 **eth3**（WAN）上。

即：**WiFi 客户端与上游网关在同一个桥域里，帧由交换机/桥直接转发，不经过 IP 栈。**

旁证：

```
sw_mode=1
wan0_ifname=eth3
wan0_proto=static
wan0_ipaddr=192.168.1.254
lan_ipaddr=192.168.123.254
```

```
Chain PREROUTING (policy ACCEPT)
GATEWAY_MAP_PRE  all  --  0.0.0.0/0  0.0.0.0/0
```

`/opt/bin` 下有 `arp-proxy`、`arp-proxy.gateway-map.bak`、`relay-service.disabled`、`update_iptables.sh`，iptables 里有 `GATEWAY_MAP_PRE` 链——这是 Padavan 的「AP + 网关映射」玩法。`eth3` 同时持有 `192.168.1.254/24` 并被 ping 通（设备自己 ping 上游 `192.168.1.1` 为 0.94 ms），但内核事实是：**eth3 是 br0 的桥端口**。

**后果（对 tbtun 最关键）**：客户端流量在 L2 被交换掉，CPU 只处理发往设备自身的流量。tbtun 是 L3/L4 端点；在当前配置下它看不到过境流量。要让它有意义，设备必须切到路由模式，或者只承载本机流量。

---

## 7. 实测基准

方法：全部在设备上跑；单核测试用 `taskset -c 0` 绑定；没有编译器，所以没有 GF(256)/XOR 微基准（见 §10）。

| 测试 | 命令 | 结果 | 换算 |
| --- | --- | --- | --- |
| DRAM 清零/拷贝 | `dd if=/dev/zero of=/dev/null bs=1M count=512` | 1.89 s | **271 MiB/s** |
| L2 驻留清零 | `dd if=/dev/zero of=/dev/null bs=64k count=8192` | 0.37 s | **1384 MiB/s** |
| tmpfs 写 32 MiB | `dd if=/dev/zero of=/tmp/bench.bin bs=1M count=32` | 0.37 s | **86 MiB/s** |
| AES-128-CBC（单核） | `taskset -c 0 openssl speed -evp aes-128-cbc` | 16 KiB: 15275.35k B/s | **14.9 MiB/s** ≈ 59 cycles/B @880 MHz |
| SHA-256（单核） | `taskset -c 0 openssl speed sha256` | 16 KiB: 21796.18k B/s | **20.8 MiB/s** ≈ 42 cycles/B |
| SSH 通道 | `ssh ... 'dd if=/dev/zero bs=1M count=16'` | 2.92 s | **5.5 MiB/s = 44 Mbit/s** |

原始片段：

```
type             16 bytes     64 bytes    256 bytes   1024 bytes   8192 bytes  16384 bytes
AES-128-CBC      10864.97k    14013.27k    14982.83k    15203.49k    15521.11k    15275.35k
sha256            2154.82k     6261.10k    13601.54k    18926.59k    21359.48k    21796.18k
```

补充事实：

- 系统空闲：`load average: 0.11, 0.04, 0.01`，`uptime` 4 天 4 小时。
- `/proc/crypto` 只有 generic 实现（`aes-generic`、`sha1-generic`、`md5-generic`、`des-generic`…），**没有硬件 crypto 引擎**。
- 两个 `dd` 数据点的差异说明：1 MiB 块超出 L2，跑的是 DRAM 写；64 KiB 块驻留 L2，跑的是缓存写。**RLNC 的窗口数据（几十 KiB）驻留 L2，所以编码循环应参照 1384 MiB/s 的上界，而不是 271 MiB/s。**

---

## 8. 可行性判断

### 8.1 1 Gbps

- **链路层**：phy2 协商到 1000BASE-T【推断】，有线交换机路径是 1G。这一层由 MT7530 硬件转发，CPU 不参与，所以「1G 链路」本身是真的。
- **端点层**：tbtun 的流量是本地终结的 TCP，不走 HNAT/PPE 旁路。这台机器在软件 TCP 终结下的上限约 300–500 Mbps【推断，依据：271 MiB/s 的 DRAM 带宽、44 Mbit/s 的 SSH 加密吞吐、4 个 VPE 挤在 2 个核上】。
- 再加上 tbtun 的 per-record 头解析、`skb_copy_bits`、重放环、每 carrier 的 token bucket，以及 FEC，**1 Gbps 不现实**。
- 结论：**不可行**。合理目标是几十到一两百 Mbps【推断】。

### 8.2 256 carriers

**协议层（硬约束）**：

```
tbtun/tbt_core.c:82:    BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16); // path_ack_mask is u16
tbtun/tbt_uapi.h:95:    #define TBT_MAX_CARRIERS_LIMIT 16 /* path_ack_mask is 16 bits */
```

- `path_id` 是 u8 且限定 `0..15`；`path_ack_mask` 是 u16；`path_ack_count` 上限 16。
- **256 carriers 需要 v6 线格式**：`path_id` 变 u16、ack 位图变 256 bit（32 B）。32 B 的头会长到约 64 B，1500 B 包上多 2% 开销。
- 这不是配置问题，是协议问题。

**OS 层（可行）**：

```
ulimit -n 1024
/proc/sys/fs/file-max 65536
/proc/sys/fs/nr_open 1048576
tcp_rmem: 4096 87380 3641344
tcp_wmem: 4096 16384 3641344
sockets: used 54   TCP: inuse 5 orphan 0 tw 0 alloc 9 mem 3
```

- 256 个 socket 对 fd 上限没有压力。
- 内存：每条 carrier 若 64 KiB 在途 ≈ 16 MiB；若不设上限（tcp_wmem/rmem 上限 3.5 MiB），最坏 900 MiB → OOM。**必须给每条 carrier 设 `SO_SNDBUF`/`SO_RCVBUF` 上限。**
- CPU：1 Gbps / 256 = 4 Mbps/carrier ≈ 350 pps/carrier，合计约 90k pps。瓶颈仍是 §8.1 的端点 CPU，增加 carrier 数不提高上限。
- 结论：**内存与 fd 可行，协议被 16 卡住，吞吐无收益。**

### 8.3 滑窗 RLNC

模型：编码一条报文要把它与窗口内 W 条源报文线性组合，成本 = W × 载荷。可用编码吞吐 ≈ 单核 GF(256) 吞吐 × 核数。

单核 GF(256) 4-bit 表 mul-add 的吞吐**没有实测**（设备无编译器）。用两个实测锚点夹逼：

- 下界：AES-128-CBC 的查表路径 **14.9 MiB/s**（59 cycles/B，每字节 4 次查表）。
- 上界：L2 驻留 memset **1384 MiB/s**（0.64 cycles/B，纯写）。
- GF mul-add 每字节 2 次查表 + 2 次 XOR + 移位，取 **20–60 cycles/B** → **15–44 MiB/s/核**，两核合计 **30–88 MiB/s**。

对照 1 Gbps = 125 MiB/s 载荷：

| 窗口 W | 需要的编码吞吐 | 双核能力 30–88 MiB/s | 结论 |
| --- | --- | --- | --- |
| 32 | 4000 MiB/s | 差 45–130 倍 | 不可行 |
| 16 | 2000 MiB/s | 差 23–66 倍 | 不可行 |
| 8 | 1000 MiB/s | 差 11–33 倍 | 不可行 |
| 4 | 500 MiB/s | 差 6–17 倍 | 不可行 |
| 2 | 250 MiB/s | 差 3–8 倍 | 不可行 |
| 1（纯 XOR，已不是 RLNC） | 125 MiB/s | 余量 1.4–4 倍 | 勉强 |

反过来，给定 30–88 MiB/s 的编码能力：

| 窗口 W | 有效载荷吞吐 |
| --- | --- |
| 4 | 7.5–22 MiB/s ≈ 60–176 Mbps |
| 8 | 3.8–11 MiB/s ≈ 30–88 Mbps |
| 16 | 1.9–5.5 MiB/s ≈ 15–44 Mbps |
| 32 | 0.9–2.8 MiB/s ≈ 7–22 Mbps |

- 解码比编码更贵：G×G 矩阵求逆是 O(G³)，再乘 G² 次数据搬运；G=32 时解码成本约为编码的 1.5–3 倍【推断】。
- 系统化编码（多数报文原样发送、只在丢包时发组合包）能压低平均成本，但接收端一旦要解码，仍要付 G² 的代价。
- **结论**：在 MT7621 上，1 Gbps 与任何 W≥2 的滑窗 RLNC 不相容；可行量级是 **W≤8 时几十 Mbps**。要做 1 Gbps，编码必须放到 x86 侧，MIPS 只做转发；或者放弃 RLNC 退回重放/重传。

---

## 9. 对 tbtun 落地路径的建议

1. **先定模式**：当前是 AP/桥接，tbtun 看不到过境流量。要路由模式，或把设备当纯 L2 中继。
2. **内核**：3.4.113 与现代 API 差距大（README 要求 ≥6.1）。这是向后移植工作量，不是构建配置问题。评估时应先看 `tbt_carrier.c` 用到的 `struct proto` 私有拷贝、`tcp_sendmsg_locked`、`kernel_connect` 在 3.4 上的替代方案。
3. **模块存放**：`/tmp`（39 MiB tmpfs）或 `/opt`（19 MiB 可用）。rootfs 只读且 100% 满，不能放。
4. **HNAT 与 tbtun 互斥**：HNAT 会把已建立的流从 CPU 旁路，tbtun 的 per-packet 处理会失效。做实验时要先关 `hw_nat`（本次未改）。
5. **验证顺序**：先用 Padavan 工具链交叉编译一个最小 `.ko`（只打印 vermagic 与加载成功），确认 `insmod` 路径；再谈移植。

---

## 10. 未覆盖与限制

- **不是 root**：`admin` 账号拿不到 `dmesg` 的早期启动日志（环形缓冲只剩 264 行无线日志）、`/proc/kallsyms`、内核 config。
- **没有编译器**：GF(256)/XOR/RLNC 只能推算，不能实测。§8.3 的区间来自 AES 与 memset 两个锚点，不是直接测量。
- **没有 ethtool/tc**：链路速率、offload、队列只能从 PHY 寄存器与 sysfs 间接判断。
- **本机在 WiFi 上**：没有做有线 1G 端到端吞吐测试。
- **未测**：256 条真实 TCP 连接的建立/维持成本（需要写代码或装工具，超出只读范围）。
- **未验证**：`CONFIG_MODVERSIONS`、`CONFIG_MODULE_SIG`、`CONFIG_PREEMPT` 只能从 vermagic 反推（无 `modversions`、无 `preempt` 字样）；`CONFIG_MIPS_*`/`CONFIG_CPU_*` 无从读取。

---

## 附录 A：复现命令

```sh
# 访问（口令不入库，交互输入）
ssh admin@192.168.123.254

# 系统
uname -a; cat /proc/version; cat /proc/cpuinfo; cat /proc/meminfo; free; df -h
cat /proc/cmdline; cat /proc/mtd; cat /proc/partitions

# 内核与模块
uname -r; ls -ld /lib/modules/$(uname -r) /lib/modules/$(uname -r)/build
cat /proc/sys/kernel/modules_disabled; cat /proc/sys/kernel/tainted
strings /lib/modules/3.4.113/kernel/arch/mips/rt2880/rt_timer_wdg.ko | grep vermagic

# 网络
ifconfig -a; ip route; cat /proc/net/dev; cat /proc/interrupts
cat /sys/class/net/eth3/{speed,duplex,link_mode,carrier}
brctl show; brctl showmacs br0
mii_mgr -g -p 2 -r 10

# 转发模式
iptables -t nat -L -n; cat /proc/sys/net/ipv4/ip_forward
nvram get sw_mode; nvram get wan0_ifname; nvram get wan0_ipaddr

# 基准
taskset -c 0 openssl speed -evp aes-128-cbc
taskset -c 0 openssl speed sha256
time dd if=/dev/zero of=/dev/null bs=1M count=512
time dd if=/dev/zero of=/dev/null bs=64k count=8192
```

## 附录 B：设备关键输出汇总

```
# uname -a
Linux JDC-1 3.4.113 #1 SMP Tue Mar 3 12:36:19 CST 2026 mips GNU/Linux

# /proc/version
Linux version 3.4.113 (root@hiboy-VirtualBox) (gcc version 4.4.7 (GCC) ) #1 SMP Tue Mar 3 12:36:19 CST 2026

# cpuinfo 摘要
system type : MediaTek MT7621 SoC
cpu model   : MIPS 1004Kc V2.15   (×4 VPE)
BogoMIPS    : 579.58
ASEs        : mips16 dsp mt

# 内存
MemTotal: 450768 kB    SwapTotal: 524284 kB (/dev/mmcblk0p5)

# 模块
modules_disabled=0   tainted=4096   .ko 数量=151
vermagic=3.4.113 SMP mod_unload MIPS32_R2 32BIT

# 接口
eth2 192.168.123.254/24 所在的 br0 成员；eth3 192.168.1.254/24（WAN）
br0 = {ra0, rai0, eth2, eth3}

# 工具
gcc/make/ld/as/ethtool/tc/nft/iperf/python/perl：全部缺失
openssl 3.0.20-dev、taskset、mii_mgr、brctl、iptables、insmod：存在

# 基准
AES-128-CBC 15275.35k B/s (单核)
SHA-256     21796.18k B/s (单核)
dd 1M×512   1.89 s (271 MiB/s)
dd 64k×8192 0.37 s (1384 MiB/s)
ssh 16 MiB  2.92 s (5.5 MiB/s)
```
