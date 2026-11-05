# tbtun 真实 WAN 实验报告（JYSZ → HND0）

**日期**：2026-11-06
**仓库**：`/home/kunweiz/Desktop/vibe/tcp-brutal`，HEAD `8d676dd`；实验期间工作树干净，`review/wan-experiment/` 之外未改代码。
**一句话**：在 RTT 约 218 ms 的真实链路上，tbtun 单流接收吞吐为 3.14-4.55 Mbit/s，仅为直连单流 17.08 Mbit/s 的 18-27%；直接限制是约 120-131 KB 的隧道 in-flight 窗口，以及 raw 载体每轮 lost/rebuild。这不是“ISP 五元组限速”的验证成功。

## 1. 目的、拓扑与环境

### 1.1 目的

- 在真实 WAN 上测 tbtun 的端到端吞吐、wire 开销、重传和载体稳定性。
- 与绕过 mihomo 的直连 TCP 基线对比，判断差距来自路径/ISP 策略，还是 tbtun 自身。
- 观察 `n`（载体数）、`m`（spare）、`dup`（近端复制）、`profile`（raw/http-kal）各自的影响。

### 1.2 拓扑

```text
JYSZ (near)                              HND0 (far)
wlp4s0 192.168.123.69                    eth0 45.11.1.62
  │  NAT 171.120.34.226                    │
  └──────────── WAN ~218 ms ───────────────┘
  tbtun near 127.0.0.1:9001  ────────►  tbtun far 0.0.0.0:9000
                                             │
                                             └─► iperf3 -s 127.0.0.1:5201
```

| 角色 | 主机 | 地址 | 内核 | tbtun |
|---|---|---|---|---|
| near | JYSZ | 192.168.123.69（wlp4s0），NAT 171.120.34.226 | `7.2.9-1-MANJARO` x86_64 | `0.0.1`，sha256 `0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63`，vermagic `7.2.9-1-MANJARO SMP preempt mod_unload` |
| far | HND0（hnd0.kw92.cyou） | 45.11.1.62（eth0） | `7.0.0-42-generic` x86_64 | `0.0.1`，sha256 `854267873bfcbbcd11cf6af0709d7814b902e934ff8898813091c52c24e27a4f`，vermagic `7.0.0-42-generic SMP preempt mod_unload modversions` |

- 模块源码与 `70031a8` 相同：`git diff --stat 70031a8 HEAD -- tbtun Makefile scripts` 为空；HEAD 只改了 `.gitignore` 和 `review/baseline/p1-tree/dkms.conf`。
- 两端构建都有 `-Wframe-larger-than=1024` 警告（`tbt_procfs`/`tbt_carrier`/`tbt_tx`），无错误；HND0 缺 `vmlinux`，构建时跳过 BTF。
- 构建、加载、配置和清理的完整命令见 `deploy/commands.md`。

### 1.3 mihomo 绕行（`ip rule pref 100`）

默认 `ip route get 45.11.1.62` 返回 `via 198.18.0.2 dev Meta table 2022`，TCP 会终止在本地 mihomo TUN，不能作为 WAN 基线。实验期间加临时规则：

```sh
ip rule add to 45.11.1.62/32 lookup main pref 100
```

验证结果：

```sh
ip route get 45.11.1.62
# 45.11.1.62 via 192.168.123.254 dev wlp4s0 src 192.168.123.69

curl --interface wlp4s0 -4 -sS https://ifconfig.me
# 171.120.34.226

ssh -o BatchMode=yes root@45.11.1.62 hostname
# hnd0.kw92.cyou
```

HND0 看到的载体源地址是 `171.120.34.226:<port>`，从未出现 `198.18.x.x`；`nft list ruleset` 没有 Meta/TPROXY 规则命中该路径。收尾删除：

```sh
ip rule del to 45.11.1.62/32 lookup main pref 100
```

当前 `ip rule` 已无 pref 100，`ip route get` 回到 Meta，说明绕行已撤。完整记录见 `deploy/route-bypass.txt`。

### 1.4 路径质量与工具

- `ping -c 20 45.11.1.62`：20/20 收到，**0% 丢包**，RTT min/avg/max/mdev = **185.5/217.9/256.2/18.0 ms**（`baseline-direct/ping-hnd0-direct.txt`）。
- iperf3 流内 mean RTT 为 225-284 ms（`baseline-direct/parsed.json`）。直连也有大量重传（P1 691-1252，P4 12.7-13.6k，P8 13.1-18.3k），不是干净链路。
- 工具为 **iperf3 3.20**：HND0 用 `/usr/bin/iperf3`；JYSZ 无系统 iperf3，使用从 HND0 复制的 `/tmp/iperf3-local/bin/iperf3`。smoke 阶段用 `/tmp/tbtun-iperf3/usr/bin/iperf3` + `LD_LIBRARY_PATH=/tmp/tbtun-iperf3/usr/lib/x86_64-linux-gnu`。
- HND0 的 iperf3 server 以 transient systemd unit 运行，避免 SSH 退出被杀：

```sh
systemd-run --unit=tbtun-iperf3-5201 \
  --property=StandardOutput=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  --property=StandardError=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  /usr/bin/iperf3 -s -B 127.0.0.1 -p 5201
```

直连基线另起一个 iperf3 server，端口 10000（`baseline-direct/hnd0-server.log`）。

## 2. 直连基线（绕过 mihomo）

方法：`baseline-direct/run_baseline_direct.py`，命令为 `iperf3 -c 45.11.1.62 -p 10000 -t 10 -P {1,4,8} --json`，每个并行度跑 2 次。receiver 取 HND0 server 日志（与 client JSON 的 `end.sum_received` 一致），sender 取 client JSON；retransmits 为各流之和。每次 10 s。

| 并行流 | receiver Mbit/s（r1 / r2 / 均值） | sender Mbit/s（r1 / r2 / 均值） | retransmits（r1 / r2 / 均值） | iperf 流均值 RTT（ms，r1 / r2） |
|---|---|---|---|---|
| P1 | 14.390 / 19.779 / **17.084** | 17.825 / 23.171 / **20.498** | 691 / 1,252 / **972** | 260.7 / 284.2 |
| P4 | 27.717 / 32.344 / **30.031** | 38.270 / 45.922 / **42.096** | 12,677 / 13,602 / **13,140** | 269.4 / 247.1 |
| P8 | 32.817 / 34.626 / **33.722** | 56.614 / 57.033 / **56.824** | 18,287 / 13,063 / **15,675** | 250.4 / 237.6 |

- 基线是在 near 机器有外部负载时测的（`local_busy_median` 28.0-99.8%，见 `parsed.json`），仍达到 17-34 Mbit/s；far 侧 load 0.4-0.75，iperf3 进程 CPU 0.08-0.44 s/10 s，不是 far 端瓶颈。
- 直连 P1 的 cwnd 达 1.9-4.7 MB，能填满约 460 KB 的 BDP；这是后面对比的关键。
- **不要引用 `baseline/` 目录**：那是走 mihomo 的 proxy-path 结果，sender 被代理缓冲夸大、RTT 只反映本地 TUN，`baseline/PROXY-PATH-NOTICE.txt` 已明确标注无效。有效基线只在 `baseline-direct/`。

## 3. tbtun 矩阵

### 3.1 方法

- 5 个配置 × 2 次 × 15 s。每次先两端 `del 1`，写 far、写 near、等 3 s，回读 canonical config，再跑 `iperf3 -c 127.0.0.1 -p 9001 -t 15 -P 1 --json`。
- receiver 取 HND0 iperf3 server 日志切片，不是 client JSON；每次使用独立 portblock（near `30100+100i`，far `31100+100i`）。
- 公共参数：`keepalive=2000 hole_budget_ms=30000 carrier_stall_ms=15000 replay=8388608 rx_reorder_bytes=262144 nonce=00112233445566778899aabbccddeeff`；far `listen=0.0.0.0:9000 target=127.0.0.1:5201`；near `listen=127.0.0.1:9001 peer=45.11.1.62:9000`。
- 10/10 成功，无 EOF、无 Broken pipe、无重试。HND0 load 0.19-0.73；矩阵前后 `ip route get 45.11.1.62` 都走 wlp4s0（无 Meta）。

### 3.2 配置

| 代号 | profile | n | m | dup | 目的 |
|---|---|---:|---:|---:|---|
| c1-raw-n1m0d0 | raw | 1 | 0 | 0 | 单载体基线 |
| c2-raw-n4m1d0 | raw | 4 | 1 | 0 | n=4 + spare |
| c3-raw-n8m1d0 | raw | 8 | 1 | 0 | n=8 + spare |
| c4-httpkal-n4m1d0 | http-kal | 4 | 1 | 0 | HTTP 容器 profile |
| c5-raw-n4m1d1 | raw | 4 | 1 | 1 | 近端复制 |

### 3.3 吞吐、wire 与资源（逐次）

| 配置 | run | receiver Mbit/s | wire/submit | retx_ppm | dup_ppm | holes_unrec | carrier_lost | near busy avg/max % | far busy avg/max % | 结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| c1-raw-n1m0d0 | 1 | 3.580 | 1.020 | 2,290 | 10 | 0 | 1 | 63.7/100.0 | 14.8/67.0 | OK |
| c1-raw-n1m0d0 | 2 | 3.993 | 0.998 | 1,039 | 4 | 0 | 1 | 30.8/100.0 | 12.4/43.9 | OK |
| c2-raw-n4m1d0 | 1 | 3.851 | 1.038 | 5,828 | 3,371 | 0 | 5 | 83.0/96.0 | 30.9/63.0 | OK |
| c2-raw-n4m1d0 | 2 | 3.447 | 1.041 | 6,161 | 4,424 | 0 | 5 | 53.6/69.1 | 27.6/67.7 | OK |
| c3-raw-n8m1d0 | 1 | 4.266 | 1.095 | 43,171 | 40,452 | 0 | 9 | 33.7/73.7 | 24.6/59.8 | OK |
| c3-raw-n8m1d0 | 2 | 4.062 | 1.086 | 26,655 | 24,507 | 0 | 9 | 85.0/99.9 | 32.7/63.0 | OK |
| c4-httpkal-n4m1d0 | 1 | 4.006 | 1.052 | 11,247 | 5,203 | 0 | 0 | 83.9/100.0 | 9.8/61.9 | OK |
| c4-httpkal-n4m1d0 | 2 | 2.265 | 1.118 | 55,258 | 43,078 | 0 | 0 | 40.9/70.9 | 9.5/43.3 | OK |
| c5-raw-n4m1d1 | 1 | 4.068 | 2.018 | 11,754 | 11,037 | 0 | 8 | 10.9/22.1 | 36.8/94.0 | OK |
| c5-raw-n4m1d1 | 2 | 5.040 | 2.016 | 1,650 | 37 | 0 | 7 | 34.5/74.8 | 14.5/61.9 | OK |

说明：

- `retx_ppm`、`dup_ppm` 是 near `retx_data_ppm` / `dup_bytes_ppm` 的窗口值，不是累计比例。
- `carrier_lost` 是 near `lost` 的增量（载体丢失/替换事件），不是丢数据；`holes_unrecoverable` 全 0。
- `near/far busy` 是机器级 CPU busy，不是 tbtun 进程 CPU；near 列被外部负载污染（见第 6 节）。far iperf3 进程 CPU 为 0.17-0.56%。

### 3.4 与直连基线的百分比

| 配置 | 均值 Mbit/s | vs P1 17.08 | vs P4 30.03 | vs P8 33.72 |
|---|---:|---:|---:|---:|
| c1-raw-n1m0d0 | 3.787 | 22.2% | 12.6% | 11.2% |
| c2-raw-n4m1d0 | 3.649 | 21.4% | 12.2% | 10.8% |
| c3-raw-n8m1d0 | 4.164 | 24.4% | 13.9% | 12.3% |
| c4-httpkal-n4m1d0 | 3.136 | 18.4% | 10.4% | 9.3% |
| c5-raw-n4m1d1 | 4.554 | 26.7% | 15.2% | 13.5% |

- 最好的 c5 也只有直连 P1 的 26.7%；c3（n=8）只有直连 P8 的 12.3%。
- n 从 1 到 8 只增加约 10%（3.787 → 4.164），而直连 P1→P8 增加约 97%。这是“n 不扩展”的核心事实。

### 3.5 in-flight、rxq 与载体

| 配置 | run | 总 in-flight B | far rxq MiB | far rx stall ms | near win_block | spare_activated |
|---|---:|---:|---:|---:|---:|---:|
| c1-raw-n1m0d0 | 1 | 122,880 | 0.00 | 0.0 | 1,330 | 0 |
| c1-raw-n1m0d0 | 2 | 122,880 | 0.00 | 0.0 | 1,227 | 0 |
| c2-raw-n4m1d0 | 1 | 122,880 | 3.43 | 322.4 | 583 | 1 |
| c2-raw-n4m1d0 | 2 | 130,807 | 3.71 | 601.9 | 2,016 | 0 |
| c3-raw-n8m1d0 | 1 | 130,860 | 3.27 | 263.9 | 4,197 | 1 |
| c3-raw-n8m1d0 | 2 | 131,072 | 3.57 | 297.2 | 0 | 0 |
| c4-httpkal-n4m1d0 | 1 | 114,688 | 0.00 | 0.0 | 0 | 0 |
| c4-httpkal-n4m1d0 | 2 | 81,920 | 0.00 | 0.0 | 0 | 0 |
| c5-raw-n4m1d1 | 1 | 189,175 | 3.15 | 393.7 | 2,882 | 1 |
| c5-raw-n4m1d1 | 2 | 139,264 | 0.00 | 0.0 | 2,457 | 1 |

- 总 in-flight 在 n=1/4/8 都停在 **120-131 KB**（122,880 B；122,880-130,807 B；130,860-131,072 B），不随 n 增长。
- peer 快照的 RTT 为 234-300 ms。按 120-131 KB 与约 240-300 ms 的有效 RTT 估算，理论上限约 **3.3-4.1 Mbit/s**；实测 3.45-4.27 与之一致。按 218 ms × 17.08 Mbit/s，直连 BDP 约 **465 kB**；隧道窗口只有它的约 26-28%。
- n≥4 的多数 raw 运行结束时 far `rxq` 仍有 **3.15-3.71 MiB**，`rx_max_stall_us` 为 264-602 ms；说明 far 侧有序交付/窗口释放也在拖后腿，15 s 内没有到稳态。
- raw 运行每轮都有载体 `lost`/`replaced`：r1 为 **1/5/9/8**（c1/c2/c3/c5），r2 为 **1/5/9/7**；`holes_unrecoverable` 全 0。c2/c3/c5-r1 的 far 接收停顿为 264-602 ms；c1 和 c5-r2 的末次采样为 0，说明停顿不是每次重建都必然被采到。
- `m=1`：c2-r1、c3-r1、c5-r1/r2 出现 `spare_activated=1`；spare 也承载流量（near peers `tx_bytes` 348 KB/592 KB），不是纯待机。m=1 没有提高吞吐，价值是冗余（raw 每轮丢载体但无不可恢复 hole）。c1（m=0）丢了唯一载体一次也恢复了。
- `dup=1`：c5 `wire/submit` 2.018/2.016，near `dup_bytes` 9.28/9.75 MB，`inflate_ppm` 约 1.96e6；唯一无歧义的效应是 wire 翻倍。c5 的 receiver 4.07/5.04（均值 4.55）高于 c2，但 c5 的 near 负载低得多（10.9/34.5% vs 83.0/53.6%），且 c5-r2 的 `rxq=0`、in-flight 139-189 KB；两个样本不足以证明 dup 带来吞吐增益。
- `http-kal`：c4 `lost=0`、`rxq=0`、far busy 9.5-9.9%，稳定性最好；但均值 3.14，比 raw n=4 的 3.65 低约 14%，c4-r2 只有 2.27 Mbit/s 且 retx 55,258 ppm。HTTP 容器可见（near `http_req=5`、far `http_resp=5`、`pad_tx`/`drip_tx` 非零），但这条路径上没有看到 HTTP QoS 增益。

## 4. 结论

1. **tbtun 在该高 RTT 链路上远慢于直连。** 单流接收吞吐 3.14-4.55 Mbit/s，是直连 P1（17.08）的 18-27%、P8（33.72）的 9-15%。即使最好的 c5（dup=1）也只有 P1 的 26.7%。

2. **n 不扩展的直接原因是 in-flight/窗口，不是载体数不够。** 所有配置的总 in-flight 都停在约 120-131 KB，n=1 和 n=4 都是 122,880 B，n=8 也只有 130,860-131,072 B。按 218 ms × 17.08 Mbit/s，直连 BDP 约 465 kB；隧道窗口只有约 26-28%。在 234-300 ms RTT 下，120-131 KB 只能支撑 3.3-4.1 Mbit/s，正好是实测区间。增加 n 只是把同一个窗口分给更多载体，并增加重排序工作；far 侧还积压 3+ MiB、停顿 0.26-0.60 s，说明接收侧的有序交付也没有跟上。

3. **raw 载体每轮 lost/rebuild。** near `lost` 增量为 1/5/9/8（r1）和 1/5/9/7（r2），对应 `replaced` 和源端口变化；数据没有不可恢复丢失（`holes_unrecoverable=0`），c2/c3/c5-r1 的 far 接收停顿为 264-602 ms（c1 和 c5-r2 的末次采样为 0）。工作假设：raw 载体不发 PAD（`pad_tx=0`），而 `carrier_stall_ms=15000` 与 15 s 测试窗口重合，stall 计时可能没有被数据/ACK 重置。本矩阵没有改公共参数，因此没有验证这个假设。

4. **http-kal 稳定但更慢。** `lost=0`、`rxq=0`、far CPU 更低，说明 HTTP 容器确实让载体更稳；但吞吐均值 3.14，比 raw n=4 低约 14%，r2 还出现 55,258 ppm 重传。在这条没有真实 QoS 的路径上，http-kal 没有带来吞吐优势。

5. **m=1 无吞吐增益，dup=1 只翻倍 wire。** m=1 的 spare 会被激活并承载流量，价值是冗余而不是吞吐；c1（m=0）也能从单次丢失中恢复。dup=1 的 `wire/submit` 为 2.016-2.018，near `dup_bytes` 9.28/9.75 MB；吞吐数字虽高，但被 near 负载、in-flight 差异和 rxq 状态混杂，两个样本不能证明增益。**唯一无歧义的 dup 效应是 wire 翻倍。**

6. **这不是“ISP 五元组限速”的验证成功，而是窗口/载体稳定性问题。** 直连 TCP 在同一对端、同一路径、同一 NAT 源地址上达到 17.08/30.03/33.72 Mbit/s；隧道只是换了目的端口（9000/9001）和源端口。若 ISP 按五元组限速，直连基线也应受限，但直连没有。当前证据指向 tbtun 自身的发送窗口（120-131 KB）与 raw 载体重建。要验证五元组/端口策略，必须做端口或协议对照、以及真实 QoS 实验；本实验没有做，不能把“隧道慢”当作限速证据。

## 5. 复现步骤

以下命令是实际使用的版本；脚本和原始记录在同目录。注意：`matrix/run_matrix.py` 依赖 `sudo -n` 读/写 `/proc/net/tbtun/*`，临时 sudoers 规则已在收尾删除；复现时需要 root，或按 `deploy/sudoers-90-tbtun-exp.txt` 重新安装临时规则并在结束后删除。

### 5.1 两端构建与加载

near（JYSZ）：

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal
source ~/.cache/tbtun-build/env.sh
make KERNEL_DIR="$KDIR" PAHOLE="$PAHOLE" -j2
sha256sum tbtun.ko
# 0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63
modinfo tbtun.ko | grep -E 'version|vermagic'
# version: 0.0.1
# vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload
insmod /home/kunweiz/Desktop/vibe/tcp-brutal/tbtun.ko
```

far（HND0）：

```sh
ssh root@45.11.1.62 'mkdir -p /tmp/tbtun-exp-70031a8'
rsync -a --exclude=.git --exclude='*.o' --exclude='*.ko' --exclude=review \
      --exclude='*.cmd' --exclude=.tmp_versions \
      /home/kunweiz/Desktop/vibe/tcp-brutal/ root@45.11.1.62:/tmp/tbtun-exp-70031a8/
ssh root@45.11.1.62 'cd /tmp/tbtun-exp-70031a8 && rm -f Module.symvers && \
  make -C /lib/modules/$(uname -r)/build M=$PWD PAHOLE=false -j1 modules'
# sha256 854267873bfcbbcd11cf6af0709d7814b902e934ff8898813091c52c24e27a4f
ssh root@45.11.1.62 'cd /tmp/tbtun-exp-70031a8 && insmod ./tbtun.ko'
```

### 5.2 直连绕行与 iperf3 target

```sh
# JYSZ：绕开 mihomo
ip rule add to 45.11.1.62/32 lookup main pref 100
ip route get 45.11.1.62        # 应显示 via 192.168.123.254 dev wlp4s0

# HND0：iperf3 server
systemd-run --unit=tbtun-iperf3-5201 \
  --property=StandardOutput=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  --property=StandardError=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  /usr/bin/iperf3 -s -B 127.0.0.1 -p 5201
```

直连基线用另一个 server（端口 10000）和 `baseline-direct/run_baseline_direct.py`；它同样要求 `ip rule pref 100` 生效。

### 5.3 far/near 配置

先 `del 1` 清掉旧隧道，再写 far、写 near，等 3 s，回读确认。far 在前：

```sh
# far (HND0)
ssh root@45.11.1.62 'tee /proc/net/tbtun/config' <<'EOF'
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=4 m=1 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31099 nonce=00112233445566778899aabbccddeeff
EOF

# near (JYSZ)
sudo -n tee /proc/net/tbtun/config <<'EOF'
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=1 dup=0 profile=raw keepalive=2000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=30000 rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30099 nonce=00112233445566778899aabbccddeeff
EOF
```

矩阵每个配置的精确文本在 `matrix/config-<tag>-{far,near}.txt`；部署模板在 `deploy/config-far.txt`、`deploy/config-near.txt`。

### 5.4 iperf3 与 stats

```sh
# JYSZ
/tmp/iperf3-local/bin/iperf3 -c 127.0.0.1 -p 9001 -t 15 -P 1 --json

# near stats
sudo -n cat /proc/net/tbtun/{config,stats,peers,tunnels,ports}

# far stats
ssh root@45.11.1.62 'cat /proc/net/tbtun/{config,stats,peers,tunnels,ports}'
```

receiver 从 HND0 `iperf3-server.log` 的本次切片读取；矩阵脚本按 offset 切片并解析 `receiver` 行。`matrix/run_matrix.py` 会自动完成配置、iperf3、两端 stats/peers/tunnels、CPU 采样和 JSON 落盘；`matrix/gen_summary.py` 从 `results.jsonl` 重新生成英文 `matrix/summary.md`。

### 5.5 清理

```sh
# 两端删隧道
printf 'del 1\n' | sudo -n tee /proc/net/tbtun/config
ssh root@45.11.1.62 'printf "del 1\n" | tee /proc/net/tbtun/config'

# 卸载模块
rmmod tbtun
ssh root@45.11.1.62 'rmmod tbtun'

# 删除绕行与临时 sudoers
ip rule del to 45.11.1.62/32 lookup main pref 100
sudo rm -f /etc/sudoers.d/90-tbtun-exp

# 停止 iperf3
systemctl stop tbtun-iperf3-5201
```

当前状态已核对：`lsmod` 无 tbtun，9000/9001/5201 无监听，`ip rule` 无 pref 100，`sudo -n` 需要密码（sudoers 规则已删）。

## 6. 限制

1. **near 并行负载污染。** 整个矩阵期间 near 在跑无关的 `cc1plus` / `test_multistream` / `test_nv12_pipeline`，1 分钟 load 3.5-9.0，机器 busy 10.9-85%。near CPU 列是机器级数据，不能当作 tbtun 开销。直连基线也在类似负载下测（`local_busy_median` 28.0-99.8%），仍达到 17-34 Mbit/s，所以负载不能单独解释 ~4 Mbit/s 的隧道结果。
2. **15 s 窗口，raw n≥4 未稳态。** c2/c3/c5-r1 结束时 far `rxq` 还有 3.1-3.7 MiB；near 提交 9.3-10.4 MB，far 只交付 6.6-8.2 MB。因此 receiver 数字是下界，不是该配置的稳态上限。
3. **短测试曾 EOF/Broken pipe。** 部署阶段的 smoke 短跑出现过 `Broken pipe`（`deploy/smoke-*`）；矩阵统一用 15 s 后 10/10 成功。不要把 smoke 的短跑数字与矩阵混用。
4. **没有做参数扫描。** `rx_reorder_bytes`、`carrier_stall_ms`、`hole_budget_ms`、发送窗口/队列上限都没有扫；矩阵只改了 n/m/dup/profile。
5. **near iperf3 进程 CPU 未被矩阵采样器捕获。** 采样器在 iperf3 子进程创建前初始化，`near_iperf_cpu` 为空。补充的 c2 15 s 运行（`matrix/supp-c2-near-iperf-cpu.txt`）测得 0.0% of one core（31 个样本），机器 busy 33.3%，该次 5.38 Mbit/s；far iperf3 为 0.17-0.56%。客户端不是瓶颈。
6. **样本少、有混杂。** 每个配置只有 2 次；c5 的 near 负载明显低于 c2，且 c5-r2 的 rxq=0，不能把 c5 的高值直接归因于 dup。http-kal 的 far `dup_bytes_ppm=1000000` 是窗口 stale 值，未用于结论。
7. **直连基线 10 s、矩阵 15 s**；两者都受 near 负载和 WAN 波动影响。RTT 本身在 185-256 ms（ping）和 225-284 ms（iperf 流）之间变化。

## 7. 建议下一步（按优先级）

1. **把发送窗口/in-flight 与 n、BDP 解耦并放大。** 当前总 in-flight 被钳在 120-131 KB；目标是至少一个 BDP（218 ms × 17 Mbit/s ≈ 465 kB），并让窗口随 n 和实测 RTT 增长。先定位这个上限是隧道级全局配额、每载体 40 KB 配额，还是 `rx_reorder_bytes`/队列上限导致的；同时观察 far `rxq` 和 `rx_max_stall_us` 是否随窗口放大而下降。验收：单流 receiver 明显超过 4.1 Mbit/s 的窗口上限，且 n=4/8 能随 n 提升。
2. **排查 raw 载体 lost/rebuild。** 重点看 `carrier_stall_ms` 计时是否被数据/ACK 重置，以及 `pad_tx=0` 时载体如何维持 up 判定；`carrier_stall_ms=15000` 与 15 s 测试窗口重合，可能是计时器从未被重置的线索。建议加调试计数（last_rx/last_tx/stall_deadline），再用 `carrier_stall_ms=60000`、强制 PAD、或更长 keepalive 做对照。验收：raw 运行 `lost` 降到 0 或接近 0，且没有 0.26-0.60 s 的重建停顿。
3. **用真实 QoS 对比 http-kal。** 当前路径没有施加限速/丢包，http-kal 只是更慢。下一步应在受控 `tc netem`/真实 ISP QoS 下比较 raw 与 http-kal 的吞吐、丢包和识别行为，验证 HTTP 容器是否真的能避开五元组/协议策略。验收：同一 QoS 条件下两者有可重复的差异，而不是单次波动。
4. **默认关 dup。** dup=1 的 wire 翻倍是确定的，吞吐增益未被证实；在窗口问题解决前，dup 只会放大拥塞和 wire 成本。若保留，应作为显式 opt-in，并在干净链路、长窗口下重测。
5. **在干净主机复测 P8。** 换一台没有 `cc1plus`/`test_multistream` 的 near 主机，跑 30-60 s 稳态；同时用 `-P 1` 和 `-P 4/8` 通过隧道，分离“单流窗口限制”与“隧道总容量”。验收：给出稳态 receiver、in-flight、rxq、stall 和 near/far 进程 CPU，排除负载混杂。

## 8. 证据索引

- **直连基线**：`baseline-direct/summary.json`（逐次原始）、`baseline-direct/parsed.json`（解析后的 sender/receiver/retransmits/RTT/CPU）、`baseline-direct/console.log`、`baseline-direct/hnd0-server.log`、`baseline-direct/server-bytes.txt`、`baseline-direct/ping-hnd0-direct.txt`、`baseline-direct/local-env-direct.txt`、`baseline-direct/hnd0-env-direct.txt`、`baseline-direct/run_baseline_direct.py`、`baseline-direct/parse_direct.py`、`baseline-direct/iperf3-p{1,4,8}-r{1,2}.json`。
- **无效的 proxy 基线**：`baseline/`（走 mihomo；见 `baseline/PROXY-PATH-NOTICE.txt`，不要引用其吞吐）。
- **tbtun 矩阵**：`matrix/summary.md`（英文详细版）、`matrix/results.jsonl`（逐次解析记录）、`matrix/run_matrix.py`、`matrix/gen_summary.py`、`matrix/config-*.txt`、`matrix/stats-*.txt`、`matrix/peers-*.txt`、`matrix/tunnels-*.txt`、`matrix/iperf-*.json` + `.stderr.txt`、`matrix/server-*.log`、`matrix/cpu-*-far.csv`、`matrix/env-before.txt`、`matrix/supp-c2-near-iperf-cpu.txt`。
- **部署与清理**：`deploy/commands.md`、`deploy/route-bypass.txt`、`deploy/config-far.txt`、`deploy/config-near.txt`、`deploy/final-{near,far}.txt`、`deploy/stats-*`、`deploy/peers-*`、`deploy/tunnels-*`、`deploy/smoke-*`、`deploy/dmesg-*`、`deploy/sudoers-90-tbtun-exp.txt`（临时规则记录；收尾已删）。
