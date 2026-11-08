# 0.3.3 同链路性能 A/B 与直连基线

本目录只有测量方法：驱动脚本、直连基线脚本、统计判据和一份历史数据复核。
**脚本没有在真机执行过**；本文件引用的数字全部来自仓库里已有的记录，并逐条
标注来源与可比性。0.3.3 的 A/B 结果要等真机跑完后另建
`results/<UTC>-ab/`，不在本目录里预填。

## 1. 为什么需要这套脚本

仓库已有的 0.3.x 数字不能直接横比：

- 0.3.0 / 0.3.1 在 near `.82`，0.3.2 fix54 已换到 `.85`；
- 0.3.0 tag 已包含 fix43，0.3.1 tag 已包含 fix44，0.3.2 tag 已包含 fix54；
  但三次是不同时间、不同 near 主机的单次运行，不是同一次对跑；
- 0.3.1 的稳态数字在落盘 SUMMARY 里是 `?`，`0.3.2-scope-audit.md:75` 引用的
  22.9 Mbit/s 实际来自一个 identity 为 **fix43/0.3.0** 的目录；
- 每次 60 s 稳态只有一次，没有中位数、没有区间；同构建两次 60 s 的差可达
  10%（20.7 对 22.9 Mbit/s）。

所以本套方法只做一件事：在**同一台 near、同一台 far、同一时段、同一形状、
同一 far 源**上，按相反顺序重复三个版本，把每次 60 s 原始数据全部留下，
再用固定规则判断差异是否超过噪声。

## 2. 拓扑与测量点

```
near 192.168.123.85                                     far 45.11.1.62
┌───────────────────────────┐                     ┌──────────────────────────┐
│ curl 127.0.0.1:9001       │  256 carriers       │ tbtun far                │
│   -> near tbtun           │ ==================> │   -> 127.0.0.1:5210      │
│      (8x32/L16 uplink TX) │      真实 WAN        │      (1x256/L8 downlink) │
└───────────────────────────┘                     │ far-raw-serve.py         │
                                                   │   -> accept-1g.bin       │
直连基线: curl -> http://45.11.1.62:5211/accept-1g.bin （不经过 tbtun）        │
                                                   └──────────────────────────┘
```

形状在三个版本上逐字相同：

| 端 | 配置片段 |
|---|---|
| far | `n=128 m=128 fec=on adaptive=off mode=sc-ldpc w=256 carriers=256`，`L=8`，`target=127.0.0.1:5210` |
| near | `n=8 m=24 ... mode=sc-ldpc w=128 carriers=256`，`L=16`，`peer=45.11.1.62:9000`，`dev=eth0` |

每个 60 s 窗口记录：

| 指标 | 来源 | 含义 |
|---|---|---|
| app bytes | far `/proc/net/tbtun/stats` 的 `capture_runs ... submit_bytes` 窗口差 | far 目标实际交给隧道的数据 |
| app Mbit/s | `app_bytes * 8 / far_window_s / 1e6` | 主判据；用 far 本机窗口 |
| wchar | near `curl` 的 `/proc/<pid>/io` `wchar` 窗口差 | 客户端实际写出的字节，交叉校验 |
| wire | far `/proc/net/dev` 物理网卡 tx 窗口差 | 含 TCP/IP 头、重传、重复 |
| wire/app | `wire_bytes / app_bytes` | 隧道放大率 |
| ack | near `c9 fec_ack_tx`、far `c9 fec_ack_rx` 窗口差 | FEC ACK 帧数 |
| CPU | 两端 `/proc/stat` 首行 busy 差 / 窗口 | busy 百分比（全机，0-100） |
| link | `c3 up/lost/replaced/stall_*`、`c6 holes/rx_max_stall_us`、`c11 NACK/replay`、`c9 drop_sid` | 载波与修复状态 |
| RTT | `c3 rtt_mean_us` 窗口末值 | 载波 TCP SRTT，不是隧道 FBK 估计 |
| 环境 | `/proc/loadavg`、thermal zone、`scaling_cur_freq` | 负载、温度、频率 |

**不测**：正确性、1 GiB byte-exact、首连、上行 POST、内存。这些由
`sclc-verify` 与 `prod-1g` 负责，A/B 不重复判。

## 3. 文件

| 文件 | 作用 |
|---|---|
| `perf-ab.sh` | 三版本轮换：卸模块、装模块、校验 srcversion、写配置、等 join、3×60 s |
| `perf-direct.sh` | near→far 单 TCP 直连（不经过 tbtun）：3×60 s 吞吐 + 前后各 100 次 HEAD 延迟（每次新 TCP，分别给 connect/TTFB/total）；1 GiB 在窗口内跑完时自动接上下一个 GET |
| `perf-sampler.sh` | 远端 1 s 采样器：c3/c6/c9/c11、load1；给窗口内 `up_min` 与 stall 时序 |
| `perf-curl.sh` | 一次有界 GET，写 pidfile；`exec curl` 保证 wchar 属于这条流 |
| `perf-curl-loop.sh` | 直连用：一个 GET 完成后立刻接下一个，窗口内始终有流量 |
| `perf-stats.py` | 读 `runs.csv`，出中位数、范围、MAD、block drift、5% 判据、延迟分位 |
| `perf-versions.conf` | 版本名到两端 `.ko` 路径的映射 |

## 4. 版本编组（必须先做）

`perf-ab.sh` 不构建模块。它要求每个版本在两端各有一个 `.ko`，路径写在
`perf-versions.conf`：

```
0.3.0   /root/tbtun-perf/0.3.0/tbtun.ko                        /root/tbtun-perf/0.3.0/tbtun.ko
0.3.1   /root/tbtun-perf/0.3.1/tbtun.ko                        /root/tbtun-perf/0.3.1/tbtun.ko
0.3.2   /root/tbtun-0.3.2-release-20261108T140023Z/tbtun.ko    /root/tbtun-0.3.2-release-20261108T140023Z/tbtun.ko
```

0.3.0 / 0.3.1 可以按 tag 在两端各建一次：

```sh
for v in 0.3.0 0.3.1; do
    git archive --format=tar "v$v" | gzip -9 >"/tmp/tbtun-$v.tar.gz"
    for h in root@192.168.123.85 root@45.11.1.62; do
        ssh "$h" "mkdir -p /root/tbtun-perf/$v && tar -xzf - -C /root/tbtun-perf/$v"
    done <"/tmp/tbtun-$v.tar.gz"
    # 每台主机上：
    # make -C /lib/modules/$(uname -r)/build M=/root/tbtun-perf/$v modules -j2
done
```

**身份门只用 srcversion，不用版本字符串。** 发布前的 fix 构建常常把
`TBT_VERSION_PATCH` 留在旧值（fix44 报 0.3.0）；脚本在 `insmod` 后读取
`/sys/module/tbtun/srcversion`，与 `.ko` 的 `modinfo -F srcversion` 比较，
不等就中止。sha256、version、srcversion 全部写进 `identity.txt`。

前置条件：

1. near 的硬件喂狗者必须活着；脚本只观察，不停止、不重启 netwatch。
2. far 的 1 GiB 源已在跑：`far-serve.sh --start`（127.0.0.1:5210）。
3. near 的 mihomo 隔离：脚本以 marker 文件管理
   `ip rule add to 45.11.1.62/32 lookup main pref 100`，只删自己加的规则；
   并断言 `ip route get 45.11.1.62` 的 dev 是 `eth0`。
4. 两端 `/root` 空间足够，far 的 `/tmp` 是 478 MB tmpfs，源文件不放 `/tmp`。

## 5. 用法

```sh
cd review/wan-experiment/arm64/perf-0.3.3

# 1) 只读预检：ssh、工具、三个 .ko、far 源、near 路由
#    规则不存在时只报告，不加；--run 才加
./perf-ab.sh --preflight

# 2) 完整 A/B：默认 2 个 block × 每版本 3 个 60 s 窗口
./perf-ab.sh --run --tag 033

# 3) 只想跑一轮 3×60 s（不做反向 block）
./perf-ab.sh --run --blocks 1 --tag 033-one-block

# 4) 直连基线（建议在 A/B 前后各跑一次，判断链路本身有没有漂）
./perf-direct.sh --run --tag before
./perf-ab.sh --run --tag ab
./perf-direct.sh --run --tag after

# 5) 统一分析
#    A/B：可把多份 runs.csv 一起给，版本中位数是这些文件的合并中位数；
#    drift 仍按文件分别计算
./perf-stats.py results/*-ab/runs.csv --out results/analysis

# 旧 runs.csv 的 valid 列按 58..65 s 写成 0 时，用实际窗口重新判定：
./perf-stats.py <old>/runs.csv --recompute-valid --secs 60 --out <old>/analysis-fixed

#    直连：每次 direct 运行有自己的 latency.csv，逐个分析（不要用 glob
#    把多个 latency.csv 塞给一个 --latency）
for d in results/*-direct; do
    [ -f "$d/latency.csv" ] || continue
    ./perf-stats.py "$d/runs.csv" --latency "$d/latency.csv" --out "$d/analysis"
done

# 其它
./perf-ab.sh --status
./perf-ab.sh --cleanup          # del 1、rmmod、删自建 ip rule
./perf-ab.sh --dry-run          # 打印计划，不 ssh、不写
./perf-direct.sh --cleanup      # 停 far 直连监听、删自建 ip rule
```

默认时间：每个版本一次 join + 60 s settle + 3 个窗口，每窗口 20 s 预热 +
60 s 测量 + 10 s 间隔；2 个 block 共 6 次 join、18 个窗口。settle 用来
避开 join 期的修复/替换瞬态（`verify-0.3.2-fix48.sh` 用 160 s；可用
`--settle 160` 对齐）。反向 block 是默认值：第一
block 按 `0.3.0 0.3.1 0.3.2`，第二 block 按 `0.3.2 0.3.1 0.3.0`，用于抵消
随时间单调漂移（温度、链路、邻居流量）。`--blocks 1` 就是任务要求的
“同一版本 3×60 s”，但统计功效弱一半。

`--keep` 保留最后一个 session 的模块与配置；偶数 block 的最后一臂是 0.3.0，
所以 `--keep` 之后加载的通常是 0.3.0，不是 0.3.2。

退出码：0 完成，2 用法，3 预检，4 装载/配置，5 join，6 行字段/内部错误。
**失败不重试**；证据保留，模块可能留在已装载状态，用 `--cleanup` 收尾。

## 6. 统计判据

### 6.1 样本与中心

- 分析单位是一个 60 s 窗口，不是一次 SSH 或一个 10 s 滑窗。
- 每个版本先取 `valid=1` 且 `noisy=0` 的窗口作为主样本；`valid` 要求
  `app>0`、`wchar>0`、窗口在 **60..80 s**、curl 在窗口末仍存活。真实
  窗口是 66-73 s：20 s 预热之后的 A/B 快照和 sampler 停止有 6-13 s
  开销；吞吐按实际 `window_s` 计算，所以窗口偏长不影响速率。
- 中心用**中位数**，不用均值：一次 carrier stall 可以把均值拉走，中位数
  不动。
- 离散度同时给两个：
  - `rel_range = (max - min) / median × 100%`
  - `rel_mad = median(|x_i - median|) / median × 100%`
- 少于 3 个 clean 窗口：判 `INSUFFICIENT (n<3)`；一个 clean 窗口都没有时
  判 `INSUFFICIENT (no data)`，不判优劣。
  单个带 carrier stall 的窗口会被剔除；`--blocks 1` 时 3 窗只剩 2 个
  clean，就会得到 INSUFFICIENT。默认 2 个 block（6 窗）正是为了避免
  一次 stall 把整轮判成“样本不足”。

### 6.2 版本间判据

对 A→B，`delta = (median_B - median_A) / median_A × 100%`，
`disp = max(rel_range_A, rel_range_B)`：

| 条件 | 判定 |
|---|---|
| `|delta| < 5%` | **SAME**：无提升，也无可判定的退步 |
| `|delta| >= 5%` 且 `|delta| > disp` | **BETTER / WORSE** |
| `|delta| >= 5%` 但 `|delta| <= disp` | **INCONCLUSIVE**：差值在观测范围内 |

`5%` 是任务给定的“视为无提升”阈值，不是统计显著性。`disp` 是本次样本
自己报的噪声尺度；0.3.x 历史同构建 60 s 的跨度约 10%，所以一个 6% 的
“提升”如果样本跨度也是 10%，只能判 INCONCLUSIVE。

### 6.3 block drift

两个 block 只抵消单调漂移，不能完全平衡位置：`0.3.1` 在 ABC/CBA 里
总是中间。若怀疑“第二个加载的版本”本身有位置效应，用
`--versions "0.3.1 0.3.0 0.3.2"` 再跑一轮，把 0.3.1 换到第一臂；不要
直接把两轮中位数混在一起，先看两轮的方向是否一致。

同一版本、同一 session 的两个 block 的中位数如果相差超过 **固定 5%**
（`(max-min)/min`，以较小的 block 中位数为分母），`perf-stats.py` 标
`DRIFT`。多个 `runs.csv` 一起给时，drift 按文件（session）分别算，不会
把两次运行的 block 1 混在一起。此时整轮 A/B 的版本间差值不可信，应先查环境（见 §7），再决定
是否重跑；不要用“反正在两个 block 里都跑了”来掩盖漂移。

### 6.4 主指标与辅指标

- 主判据只用 **app Mbit/s**（far submit_bytes）。
- `wchar` 是交叉校验：`|app - wchar| / app > 5%` 的行标 `noisy`，因为
  窗口两端有在途/套接字缓冲差；沿固定形状比较时这个偏差对三版相同。
- `wire/app`、CPU、RTT、NACK/replay 只作解释，不参与“提升/退步”判定。
  例如 CPU 降低但 app 不变，只能写“CPU 降了”，不能写“性能提升”。

## 7. 噪声源与对应记录

| 来源 | 已知证据 | 脚本记录 | 处理 |
|---|---|---|---|
| CPU 热/降频 | 仓库没有温度-性能记录，只有 `tbt_car_rx_work` hog 的日志 | 两端 thermal zone、`scaling_cur_freq`、load1、CPU busy | 窗口末温度/频率进 CSV；两 block 温漂由反向顺序抵消 |
| 载波 stall / 替换 | fix54 记录 far 600-826 ms stall；0.3.1 长跑末 `up=252/254` | 1 s sampler 的 `up_min`，c3 `lost/replaced/stall_*`，c6 `rx_max_stall_us` | `up_min<256`、`lost>0`、`stall>0` 标 `noisy`，主样本剔除；原始行仍保留 |
| near/far 时钟偏差 | 两端不要求 NTP 同步 | 每个 sampler 用**本机** epoch 切片；near/far 各自算窗口 | 不用跨主机时间戳配对 |
| 链路 RTT / 抖动 | 历史 RTT 166-370 ms，ping 185-256 ms；直连也大量重传 | 窗口末 `rtt_mean_us`；直连用 `ss` 的 `rtt:` | 看 block drift；直连前后两跑作对照 |
| near 无关负载 | `summary.md:290` 记录过 `cc1plus` 等，1 min load 3.5-9.0 | near load1、CPU busy | 运行前人工确认；load1 异常高时整轮作废 |
| far workqueue hog | 0.3.1 `tbt_car_rx_work` >10 ms，7 次 | CPU busy、NACK/replay | 只解释，不剔除；若 CPU busy 接近 100%，该行标 `noisy` 可人工复核 |
| join / 流切换瞬态 | fix29 A/B 明确首连瞬态 -6.66%；fix54 的 stream switch 修复 | 每窗口前等 `streams_active=0`；20 s 预热 | 瞬态不进 60 s 窗口 |
| 版本混装 | fix44 报 0.3.0；`hnd0-sclc256` 被误归为 0.3.1 | srcversion + sha256 + boot_id | 身份门失败即中止 |

## 8. 输出

### 8.1 A/B 目录

```
results/<UTC>-ab[-tag]/
  meta.txt                 主机、版本、runs/blocks/secs、git HEAD、脚本 sha256
  identity.txt             每版本每端 version/srcversion/sha256/ko/config 回读
  run.log                  时间戳日志
  runs.csv                 一行一个窗口（52 列）
  runs/b<block>-o<order>-<version>-r<run>/
      near-a.txt far-a.txt         窗口起点快照
      near-b.txt far-b.txt         窗口终点快照
      near.sampler far.sampler     1 s 采样原始日志
      curl.pid curl.out
      result.txt                   该窗口的 key=value 摘要
  analysis/
      summary.md summary.csv verdict.txt summary.json
```

### 8.2 runs.csv 列（52）

```
stamp,dataset,block,order,version,run,window_s,warmup_s,
app_bytes,app_source,wchar_bytes,curl_requests,stream_alive,
app_mbps,wchar_mbps,wire_mbps,wire_over_app,
ack_tx,ack_rx,nack_tx,nack_rx,retx_bytes,c3_retx_bytes,
cpu_near_pct,cpu_far_pct,load1_near,load1_far,
temp_near_c,temp_far_c,freq_near_mhz,freq_far_mhz,
rtt_near_us,rtt_far_us,
up_min_near,up_min_far,up_end_near,up_end_far,
c3_lost,c3_replaced,c3_stall_declared,c3_stall_recovered,c3_stall_dead,
c6_holes,c6_stall_us,c11_nack_drop,c11_replay_miss,
c9_drop_sid,c9_stream_refused,valid,noisy,rc,note
```

`valid=1` 才是可用窗口；`noisy=1` 的原因写在 `note`（`up_near`、`lost`、
`stall`、`holes`、`reset`、`drop_sid`、`app_wchar` 的组合）。`invalid`
也会写进 `note`，但它单独计入 `invalid` 列，不把 `noisy` 置 1。

### 8.3 直连目录

```
results/<UTC>-direct[-tag]/
  meta.txt run.log runs.csv         与 A/B 同一个 52 列 schema
  latency-pre.csv latency-post.csv latency.csv
  runs/direct-r<N>/                 near-a/b、far-a/b、curl.out、curl.pid、curl.pid.loop、result.txt
  analysis/                         若 perf-stats.py 可执行
```

直连期间 far 在 `0.0.0.0:5211` 临时提供同一个 1 GiB 文件；窗口跑完即停
（`--keep-server` 保留）。该监听只在测量期间存在，不是常驻服务。

延迟是串行 HEAD，每次新建 TCP，`connect_s` 是握手，`ttfb_s` 是到响应头
首字节，`total_s` 是连接关闭；吞吐窗口内的 TCP RTT 另由 `ss -tin` 的
`rtt:` 记录到 `rtt_near_us`。

直连行的 `dataset=direct`、`version=direct`、`app_source=curl_wchar`。
`app` 是窗口内已完成请求的 `size=` 之和加上当前请求的 wchar 差值；
`curl_requests` 可能大于 1；`stream_alive` 只表示窗口末是否正有一个
请求在跑，不再作为 `valid` 的必要条件。
`ack_*`、`c3_*`、`c6_*` 留空；`rtt_near_us` 来自 `ss -tin` 的 `rtt:`（毫秒
转微秒）。`app_mbps == wchar_mbps` 是口径决定的，不是巧合。

### 8.4 分析输出

- `summary.csv`：每个 `(dataset, version, metric)` 的
  `n/median/min/max/p25/p75/mad/rel_range_pct/rel_mad_pct/n_all/median_all/...`
- `summary.md`：稳态表、pairwise 表、block drift、ACK/链路计数汇总、直连延迟分位
- `verdict.txt`：`PAIR ... delta_pct=... dispersion_pct=... verdict=...`
- `summary.json`：同样内容的机器可读版本

## 9. 历史数据复核（不能直接当基线）

### 9.1 1 GiB 单流

| 版本 | 构建 | near | 时间 | 吞吐 | wire/app | CPU near/far | 备注 |
|---|---|---|---:|---:|---:|---|---|
| 0.3.0 | fix43 `830c7f7` | .82 | 737.60 s | **11.65 Mbit/s** | 2.634 | 8.06% / 42.87% | retx 6,949；`never_stored` 42,963 |
| 0.3.1 | fix44 | .82 | 889.20 s | **9.66 Mbit/s** | 2.858 | 8.7% / 40.1% | retx 30,386；末 `up=252/254` |
| 0.3.2 | fix54 `1a5bf63` | .85 | 792.70 s | **10.84 Mbit/s** | 2.617 | 3.9% / 45.2% | byte-exact PASS；`never_stored` 32/97 |

### 9.2 60 s 稳态

| 版本 | 证据 | app | wire/app | CPU near/far | 备注 |
|---|---|---:|---:|---|---|
| 0.3.0 fix43 | `sclc-verify-20261108T050950Z` | 20.7 Mbit/s | 2.408 | 12.5% / 57.2% | far `hol_wait` 1,834,304 |
| 0.3.0 fix39 | `steady-256-20261108` | 23.9 Mbit/s | 2.027 | 9.7% / 65.7% | 10 s max 27.9，p50 23.2 |
| 0.3.0 fix43 | `hnd0-sclc256` | 22.93 Mbit/s | 2.055 | 机器 busy 95.7/96.8 | **不是 0.3.1**；口径不同 |
| 0.3.1 fix44 | 落盘 SUMMARY | `?` | — | — | 引用的 22.9 无法从落盘复算 |
| 0.3.2 fix54 | `fix54g-run` | 22.2 Mbit/s | 2.405 | 6.9% / 82.1% | `up=256`，`drop_sid=0` |

同构建（fix43）两次 60 s 的 20.7 对 22.9 差 **10.6%**，这就是为什么本套
判据要求 `|delta| > max(rel_range_A, rel_range_B)`，而不是只看 5%。

### 9.3 直连

| 来源 | 并行 | 数值 | 说明 |
|---|---|---:|---|
| iperf3 → far:10000 | P1 | 17.08 Mbit/s | 单流，真实 WAN，RTT 约 218 ms |
| iperf3 → far:10000 | P4 | 30.03 Mbit/s | 少流 |
| iperf3 → far:10000 | P8 | 33.72 Mbit/s | 少流 |
| 32 条 curl → far:5212 | P32 | 224-254 Mbit/s | **聚合**，不能与单流/隧道单流横比 |

本目录的 `perf-direct.sh` 测的是**单 TCP 流** near→far，对应 17-34 Mbit/s
这一栏。256-carrier 隧道的 app 高于单流直连是正常的，不要把“隧道/直连”
当成加速比；它只说明链路在这段时间能给单流多少。

## 10. 预期输出（格式示意，不是实测）

`perf-ab.sh --run` 的正常日志片段：

```
== preflight
   0.3.0 near=SRC SHA VER far=SRC SHA VER
   ...
join ok: up=256 on both ends after 37s
run 0.3.0 block=1 order=1 run=1
run 0.3.0: app=<A> Mbit/s wchar=<W> wire=<C> valid=1 noisy=0
...
analysis: .../analysis/summary.md
done: .../runs.csv
```

`analysis/summary.md` 的表格形状：

```
| dataset | version | n clean/valid | invalid | noisy | app median (clean) | all-valid median | range (clean) | rel range | ... |
| ab | 0.3.0 | 6/6 | 0 | 0 | <值> | <值> | <min>..<max> | <d>% | ... |
| ab | 0.3.1 | 6/6 | 0 | 0 | <值> | <值> | <min>..<max> | <d>% | ... |
| ab | 0.3.2 | 6/6 | 0 | 0 | <值> | <值> | <min>..<max> | <d>% | ... |
| direct | direct | 3/3 | 0 | 0 | <值> | <值> | <min>..<max> | <d>% | ... |

| dataset | A | B | median A | median B | delta | dispersion | verdict |
| ab | 0.3.0 | 0.3.1 | <值> | <值> | <d>% | <d>% | SAME / BETTER / WORSE / INCONCLUSIVE |
```

`summary.md` 另有 ACK/链路计数表（clean 窗口求和）：`ack_tx/s`、`ack_rx/s`、
`nack_tx`、`retx ppm`、`lost`、`stalls`、`holes`、`drop_sid`。直连行这些字段
为 `-`，因为直连没有 tbtun 计数器。

`verdict.txt` 的 `COUNTERS` 行按 `has_ack`/`has_link`/`has_retx` 输出；
直连或旧版缺字段时写 `-`，不写 0。

`verdict.txt` 一行一对：

```
PAIR ab 0.3.0 0.3.2 delta_pct=<d> dispersion_pct=<d> verdict=<...>
DRIFT ab 0.3.0 span_pct=<d> state=stable|DRIFT
```

直连延迟表（HEAD，毫秒）：

```
| phase | n | connect p50 | ttfb p50 | ttfb p90 | ttfb p99 | total p99 | non-200 |
| pre  | 100 | <值> | <值> | <值> | <值> | <值> | 0 |
| post | 100 | <值> | <值> | <值> | <值> | <值> | 0 |
```

## 11. 边界

- 脚本不构建、不 scp `.ko`、不生成 1 GiB 文件，也不碰喂狗者。
- 不判正确性；`valid` 只说明窗口可测量，不说明字节对。
- 不做 1 GiB 体积模式。任务允许 3×60 s 或 3×1 GiB，本实现取 60 s；
  1 GiB 的 byte-exact 与吞吐仍由 `sclc-verify` 负责。
- 不把 `INCONCLUSIVE` 改写成“趋势向好”。样本不够就加 block 或等链路安静。
- 不重试失败窗口。失败行留在 `runs.csv`，`valid=0`，原因在 `note`。
- 1 GiB 源在 80 s 内被读完时，窗口会提前结束，该行 `valid=0`。按历史
  10-25 Mbit/s 不会发生；若链路已超过约 100 Mbit/s，先换更大的源，不要
  把短窗口当吞吐。
