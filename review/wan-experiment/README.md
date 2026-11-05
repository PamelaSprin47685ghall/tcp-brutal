# review/wan-experiment

2026-11-06 的真实 WAN 实验：JYSZ（Manjaro 7.2.9，near）↔ HND0（45.11.1.62，Ubuntu 7.0.0，far），RTT 约 218 ms。只新增本目录，未改代码。

## 目录

| 路径 | 内容 |
|---|---|
| `summary.md` | 中文总报告：目的、拓扑、直连基线、tbtun 矩阵、结论、复现、限制、下一步。 |
| `baseline-direct/` | 绕过 mihomo 的直连 TCP 基线（P1/P4/P8，10 s × 2 次），有效基线。 |
| `baseline/` | 走 mihomo 的 proxy-path 结果，**无效**；只留作对照，不要引用其吞吐。 |
| `matrix/` | tbtun 5 配置 × 2 次 × 15 s 矩阵；`summary.md` 为英文详细版，`results.jsonl` 为逐次记录，`run_matrix.py`/`gen_summary.py` 为脚本。 |
| `deploy/` | 构建/加载、`ip rule` 绕行、far/near 配置、smoke、stats、dmesg、临时 sudoers 记录。 |

## 复现

1. 按 `deploy/commands.md` 在两端构建并 `insmod tbtun.ko`；near sha256 `0b8f0006…`，far sha256 `85426787…`。
2. JYSZ 加 `ip rule add to 45.11.1.62/32 lookup main pref 100`，确认 `ip route get 45.11.1.62` 走 `wlp4s0`，不是 `198.18.0.2`。
3. HND0 起 `iperf3 -s -B 127.0.0.1 -p 5201`；按 far→near 顺序写 `/proc/net/tbtun/config`（模板见 `deploy/config-far.txt`、`deploy/config-near.txt`）。
4. 跑 `matrix/run_matrix.py`，或手工 `/tmp/iperf3-local/bin/iperf3 -c 127.0.0.1 -p 9001 -t 15 -P 1 --json`；采集两端 `/proc/net/tbtun/{config,stats,peers,tunnels,ports}`。
5. 收尾：两端 `del 1`、`rmmod tbtun`、删 `ip rule pref 100`、停 iperf3、删临时 sudoers。

注意：`matrix/run_matrix.py` 依赖 `sudo -n` 访问 `/proc/net/tbtun/*`；临时 sudoers 规则已删除，复现时用 root 或按 `deploy/sudoers-90-tbtun-exp.txt` 重装并在结束后删除。

## 结果一句话

tbtun 单流 3.14-4.55 Mbit/s，只有直连 P1（17.08 Mbit/s）的 18-27%；瓶颈是 120-131 KB 的 in-flight 窗口和 raw 载体每轮 lost/rebuild，不是 ISP 五元组限速的验证成功。
