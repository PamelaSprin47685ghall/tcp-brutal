# 0.3.0 自适应 FEC 验收脚本

这四个脚本把 `review/adaptive-fec-0.3.0-design.md` §7 的验收方法落成可重复的运行：
双端 0.3.0、`n=4 m=4` 固定基线对 `adaptive=on`、10 s 预热 + 60 s 稳态、逐次字节校验。
它们只操作运行时接口（`/proc/net/tbtun/*`、`tc`），不碰内核源码，也不重启模块。

```
benches/adaptive/
  lib.sh          共享引擎：ssh、near 旁路规则、config、join、快照、注入、一轮运行、网格、CSV/JSON/中位数
  drop_ppm.sh     FEC 记录丢弃（隔离 FEC 层）
  netem.sh        tc netem 丢包（端到端）
  matrix.sh       0/1/3/5/10% × 32/64/256 carriers × fixed/adaptive × 3 次
  check-bytes.sh  sha256 / 字节数闸门（失败即停）
```

## 1. 前置条件

| 项 | 要求 |
|---|---|
| 双端 | 同一个 0.3.0 构建；`/proc/net/tbtun` 存在；`adaptive=` 能被 config 读回 |
| 模块参数 | `drop_ppm` 需要 `insmod tbtun debug_loss=1`；netem 不需要 |
| 权限 | netem 需要在注入端有 root 和 iproute2（`tc`、`prio`、`u32`、`netem`） |
| near | 到 far 的路由走 `--dev`（默认 `eth0`）；脚本会按需加 `ip rule ... pref 100 lookup main` |
| far 源 | `--source` 指向 far 上的源文件，`far-raw-serve.py` 在 `127.0.0.1:5210` 提供它 |
| 网络 | 控制端能 ssh 到两端（`BatchMode=yes`）；near 上装了 `curl sha256sum tee wc mkfifo stat` |

源文件大于一次 60 s 传输，才能保证窗口不被“下载完成”截短。约 14 MB/s 时 70 s ≈ 1 GB，
所以 1 GiB 文件在 0% 自适应下可能提前完成；脚本会记 `early=1` 并按实际窗口计算速率。
推荐 2 GiB：

```sh
scp review/wan-experiment/arm64/prod-1g/far-serve.sh root@45.11.1.62:/root/
ssh root@45.11.1.62 'SIZE_MIB=2048 FILE=accept-2g.bin sh /root/far-serve.sh --start'
# 然后所有脚本加 --source /root/accept-1g/accept-2g.bin
```

`far-serve.sh`、near 旁路规则、tunnel 配置串和 join 判定都沿用
`benches/short.sh` 与 `review/wan-experiment/arm64/prod-1g/run-1g.sh` 的既有约定；
本目录不另起一套主机配置。`short.sh` 仍然是 5 s 单形状工具，本目录是 60 s 自适应矩阵。

## 2. 快速开始

先看计划，不连主机、不写任何东西：

```sh
benches/adaptive/drop_ppm.sh --near root@192.168.123.82 --far root@45.11.1.62 \
    --near-ip 192.168.123.82 --far-ip 45.11.1.62 --dry-run

benches/adaptive/netem.sh  ... --dry-run --burst
benches/adaptive/matrix.sh ... --dry-run --quick
```

冒烟（2 个丢包点 × 3 个 carrier 数 × 2 种模式 × 1 次，约 30 分钟）：

```sh
benches/adaptive/matrix.sh --near root@... --far root@... \
    --near-ip ... --far-ip ... --quick --tag smoke
```

正式矩阵（约 5 小时；两个注入器各约 2.5 小时）：

```sh
benches/adaptive/matrix.sh --near root@... --far root@... \
    --near-ip ... --far-ip ... --repeats 3 --tag acceptance
```

单点调试：

```sh
benches/adaptive/drop_ppm.sh ... --carriers 256 --loss 3 --mode adaptive --repeats 1
benches/adaptive/netem.sh ... --carriers 256 --loss 5 --mode fixed,adaptive \
    --netem-where far --burst
```

## 3. 一轮运行做了什么

1. 重写两端 `tunnel 1`（`n=4 m=4 w=8 carriers=<C> adaptive=on|off`），等待两端 `c3 up=<C>`。
2. 拍 pre 快照（`stats`、`peers`、`config`、`/proc/stat`）。
3. 注入：
   - `drop_ppm`：对注入端每个 carrier 写一行 `carrier <i> drop_ppm <ppm>`（`loss% × 10000`）。
     写在 socket 之前，TCP 不重传，只有 parity/NACK 能修。
   - `netem`：在 `--netem-where` 指定的端加 `prio` 根 qdisc + `u32` 过滤 + `netem loss`，
     只命中 carrier 五元组（near 出口 `dport 9000`，far 出口 `sport 9000`），ssh 不受影响。
4. near 上启动 `curl -f --max-time <warmup+duration+2>`，body 经 `tee | wc -c` 计数、
   同时送 `sha256sum`；near 不落多 GB 临时文件。
5. 10 s 预热后拍 A 快照；再跑 60 s 稳态（每秒检查传输是否提前结束）；拍 B 快照。
6. 停传输、撤注入，计算 A→B 的增量，用 far 上 `head -c <N> <source> | sha256sum`
   与 near 的流式 sha256 比对；不一致立即停止整个网格。
7. 写一行 `runs.csv`、一行 `runs.jsonl`，并保存 `runs/<name>/{near,far}-{pre,a,b}.txt`。

`--kill-at SEC --kill-carrier N` 在稳态第 SEC 秒写 `carrier N down`（S12）。
`early=1` 表示传输在窗口结束前完成，此时窗口取实际传输时长。

## 4. 输出与字段

结果目录默认 `benches/results/<UTC>-adaptive-<script>[-tag]/`：

```
runs.csv        每次运行一行（49 列，见下）
runs.jsonl      同样的行，JSON 对象
medians.csv     按 inject,loss_pct,carriers,mode 分组的中位数/min/max
verdict.txt     S1..S12 的 PASS/FAIL/SKIP 与依据
summary.json    runs 数、medians、verdict 的机器可读汇总
run.log         时间戳日志
runs/<name>/    两端 pre/a/b 快照、传输的 size/sha/curl.rc/curl.err、bytes.txt
```

`runs.csv` 列：

| # | 列 | 含义 |
|---:|---|---|
| 1 | stamp | UTC |
| 2 | script | drop_ppm / netem / matrix |
| 3 | inject | ppm / netem |
| 4 | loss_pct | 配置丢包（%） |
| 5 | ppm | 同一值的 ppm |
| 6 | carriers | 32 / 64 / 256 |
| 7 | mode | fixed / adaptive |
| 8 | adaptive | on / off（写进 config 的值） |
| 9 | rep | 第几次 |
| 10 | order | 该组内 fixed/adaptive 的顺序（1/2，逐次翻转） |
| 11 | window_s | 稳态窗口实际秒数 |
| 12 | warmup_s | 预热秒数 |
| 13 | early | 传输提前结束 |
| 14 | app_bytes | near `rx_delivered` 增量（回退：far `submit_bytes`） |
| 15 | app_mbps | app_bytes / window_s / 1e6 |
| 16 | curl_bytes | near 实际收到的 body 字节（wc -c） |
| 17 | wire_bytes | far `peers` 的 `tx_bytes` 增量 |
| 18 | wire_per_app | wire/app |
| 19 | retx_bytes | far c11 `fec_nack_retx_bytes` + `fec_replace_retx_bytes` 增量 |
| 20 | retx_ppm | retx/app |
| 21 | p_ppm | c13 `p_ppm`（控制律用的 max(ewma,win)） |
| 22 | p_win_ppm | c13 1 s 窗口 |
| 23 | p_delta_pp | `(p_ppm - 配置 ppm)/10000`，即百分点误差 |
| 24 | sent | c13 本 epoch 的 DATA share |
| 25 | miss | c13 本 epoch 的缺失 share |
| 26 | switches | 切换次数增量 |
| 27 | aborts | 中止增量 |
| 28 | rollbacks | 回滚增量 |
| 29 | state | c13 状态名 |
| 30 | reason | c13 原因名 |
| 31 | near_cpu_pct | near 整机 CPU（`/proc/stat`，窗口内） |
| 32 | far_cpu_pct | far 整机 CPU |
| 33 | hol_max_us | near c6 `rx_max_stall_us`（会话内最大值，窗口上界） |
| 34 | stall_declared | c3 `stall_declared` 增量 |
| 35 | holes_unrecoverable | c6 增量 |
| 36 | fec_reset_hole | c11 增量 |
| 37 | lost | far c3 `lost` 增量（carrier 关闭） |
| 38 | has_c13 | 该构建是否导出 c13 |
| 39 | near_sha256 | near 流式 sha256 |
| 40 | bytes | 被哈希的字节数 |
| 41 | bytes_ok | 1 = far 前缀哈希一致且长度合法 |
| 42 | curl_rc | 0 完整 / 28 超时截断 / 143 被杀 / 其他为失败 |
| 43 | rc | 本行整体 0/1 |
| 44 | r_fail_ppm | c13 残差失败率 |
| 45 | pauses | 进入冻结状态的次数增量 |
| 46 | pause_ns | 冻结总时长增量 |
| 47 | sample_unknown | 估计器跳过的块增量 |
| 48 | abandoned | 未完成即丢弃的块增量 |
| 49 | note | `kill=N@S`、`burst` 等标记 |

读法要点：

- **p_est**：`p_ppm` 对 drop_ppm 的配置值应在 ±1.5 pp 内（S2）。netem 下 `p_ppm` 低于配置值是正常的，
  TCP 已经重传掉一部分。
- **wire/app**：FEC 的冗余成本；固定 n4m4 的稳态参考约 2.2（含 carrier 头、ACK、NACK）。
- **retx**：FEC 修复流量，不是 TCP 重传；`ss -ti` 的 TCP 重传另看。
- **switches/pauses**：60 s 内切换 ≤3 次（S8）；`pauses` 是冻结次数，`pause_ns/pauses` 是平均停顿。
- **holes_unrecoverable / fec_reset_hole / lost**：任何增量都不是“正常损耗”，S1/S8 直接判失败。
- **bytes_ok**：失败即整轮停止，不再继续跑后面的格子。

## 5. 成功标准

`verdict.txt` 会按已测到的数据判 S1..S12；缺数据的项写 SKIP，不伪装成 PASS。

| 标准 | 判据 | 由谁判 |
|---|---|---|
| S1 | 所有 run `bytes_ok=1`，`holes_unrecoverable=0`，`fec_reset_hole=0`，`rc=0` | 全部脚本 |
| S2 | drop_ppm 下 `|p_delta_pp| ≤ 1.5`（1/3/5/10%，且 c13 存在） | drop_ppm |
| S3 | 0%：adaptive app ≥ fixed × 1.60 | 矩阵 256 carriers |
| S4 | 1%：≥ × 1.40 | 同上 |
| S5 | 3%：≥ × 1.20 | 同上 |
| S6 | 5%：≥ fixed × 0.98（down-probe 稳定时可人工按 × 1.05 复核） | 同上 |
| S7 | 10%：≥ fixed × 0.97，恢复率不低于 fixed | 同上 |
| S8 | 每 60 s 切换 ≤3 次；`rx_max_stall_us ≤ 500000`；平均停顿 ≤500 ms；`lost=0` | 全部 |
| S9 | 256 carriers、5% 时 near CPU ≤ fixed × 1.05 | drop_ppm |
| S10 | 32/64 carriers 同样满足 S1（各自的中位数见 `medians.csv`） | 矩阵 |
| S11 | netem 5% 突发（`--burst`）：adaptive ≥ fixed × 0.95，字节一致 | netem --burst |
| S12 | `--kill-at` 行：字节一致、`rc=0`、无 stream reset | drop_ppm/netem |

S3–S7 的百分比是设计建议值，机器不同可调；`verdict.txt` 里是硬编码的同一组数，
修改时只改 `lib.sh` 的 `verdict_from` 一处。

## 6. 清理

正常退出时 trap 会撤掉当前注入；netem 还有一个按 marker token 工作的远端看门狗，
即使 ssh 断了也会在 `SAFETY_SECS`（默认 600 s）后删掉自己加的 qdisc。

显式清理：

```sh
benches/adaptive/matrix.sh ... --cleanup
# 或分别：
benches/adaptive/drop_ppm.sh ... --cleanup   # carrier <i> drop_ppm 0 + del 1
benches/adaptive/netem.sh  ... --cleanup     # 只删带 marker 的 qdisc + del 1
```

手动命令（只在确认是本工具加的规则时执行）：

```sh
tc qdisc del dev eth0 root                                  # 注入端
printf 'del 1\n' > /proc/net/tbtun/config                   # 两端
ip rule del pref 100 to 45.11.1.62/32 lookup main           # near，仅当 marker 存在
rm -f /root/.tbt-adaptive-bypass-pref100
```

`--cleanup` 不会动 mihomo 自己的 ip rule：只有 `/root/.tbt-adaptive-bypass-pref100`
记录了本次添加时才删除。

## 7. 已知限制与待确认

1. **action 文件**：工单写的是 `/proc/net/tbtun/carriers`；当前代码把运行中动作
   （`carrier <i> down|up|drop_ppm`）放在 `/proc/net/tbtun/peers`（0600，每次 write 只解析一行）。
   脚本自动探测：存在且可写就用 `carriers`，否则用 `peers`；`TBT_ACTION_FILE` 可强制。
2. **0.3.0 未落地时**：HEAD 没有 `adaptive=`/c13。脚本在 config 读回里看不到 `adaptive=` 会立即退出，
   c13 缺失时 S2 记 SKIP。不会把固定形状的数字冒充自适应。
3. **drop_ppm 无逐 carrier 读回**：当前 procfs 不显示每 carrier 的 ppm；注入是否生效由写入返回码、
   `p_est` 和 0% 对照共同确认。
4. **netem 方向**：tc 只能整形 egress。默认 `far`（下载方向）；`--netem-where both` 才两个方向都丢。
5. **netem 的 p_est 偏低**：TCP 会重传，FEC 只修 TCP 来不及的部分，属预期。
6. **切换停顿**：c13 只给 `pauses` 和总 `pause_ns`，没有单次最大值；S8 用平均值加
   `rx_max_stall_us` 作上界，若需要精确单次停顿，应在设计里增加 `pause_max_ns`。
7. **`rx_max_stall_us` 是会话内最大值**，不是窗口内最大值；跨 run 比较时只作为上界。
8. **CPU** 是整机 `/proc/stat`，含 ssh/curl 等噪声；S9 是相对比较，不追求绝对值。
9. **源文件大小**：1 GiB 在高吞吐下会提前完成，`early=1` 会标出来；正式验收建议 2 GiB。
10. **同一台主机同时只跑一个 harness**：`/tmp/tbt-adaptive*` 与 netem marker 按设备名共用。
11. **不自动重启模块**：`--near-module` 只在 `/proc/net/tbtun` 缺失时 insmod 一次，从不 rmmod。
12. **ssh 瞬断不算传输结束**：warm-up/稳态的存活探测只在 near 明确报告 `curl.rc` 存在时结束；
    ssh 失败或超时连续 `TBT_TRANSFER_FAIL_MAX`（默认 3）次才判定传输结束，单次失败不置 `early=1`。

## 0.4.0 SC-LDPC adaptive acceptance

`benches/adaptive/sclc-0.4.0.sh` is the 0.4.0 counterpart. It keeps the
same host/ssh/injection/transfer conventions, but configures the asymmetric
SC-LDPC pair: near TX is the fixed binary downlink (1x256, block 256, L 8,
rate 0.50), far TX is the GF(16) uplink (8x32, block 128, L 16) whose six
ladder rates {22/10, 18/14, 16/16, 12/20, 9/23, 8/24} are the adaptive
direction. It runs `adaptive=on`, explicit `adaptive=off`, and the omitted-key
default-off regression, injects `carrier <i> drop_ppm` on the far, samples the
near/far c13 every 0.25 s, and checks p_est convergence, the final rate,
transitions/reversals, six-rate coverage, bytes, holes, reset and up=256.
The downlink fixed rate is a known limitation and is asserted by config.

```sh
benches/adaptive/sclc-0.4.0.sh --near root@192.168.123.82 \
    --far root@45.11.1.62 --near-ip 192.168.123.82 --far-ip 45.11.1.62 \
    --source /root/accept-1g/accept-2g.bin --quick --tag smoke
```
