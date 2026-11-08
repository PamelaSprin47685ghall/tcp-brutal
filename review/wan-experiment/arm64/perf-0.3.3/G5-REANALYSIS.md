# G5 perf 复核（2026-11-08T154006Z 原始 runs.csv）

本文件只复核已有原始数据，不产生新的真机流量。

- 原始数据：`review/wan-experiment/arm64/release-0.3.3-20261108T154006Z/perf-ab/runs.csv`
- 旧分析：同目录 `perf-ab/analysis/`（12/12 行 `valid=0`，所有版本 INSUFFICIENT）
- 复核命令：

```sh
./perf-stats.py .../perf-ab/runs.csv \
    --recompute-valid --secs 60 \
    --out .../perf-ab/analysis-fixed
```

## 1. 为什么旧分析全 FAIL

旧 `perf-ab.sh` 的 valid 规则是 `58..65 s`。真实窗口是 **66-73 s**：

| version | run | window_s |
|---|---:|---:|
| 0.3.0 | 1 | 73 |
| 0.3.0 | 2 | 67 |
| 0.3.0 | 3 | 67 |
| 0.3.1 | 1 | 67 |
| 0.3.1 | 2 | 67 |
| 0.3.1 | 3 | 68 |
| 0.3.2 | 1 | 67 |
| 0.3.2 | 2 | 66 |
| 0.3.2 | 3 | 67 |
| 0.3.3 | 1 | 66 |
| 0.3.3 | 2 | 67 |
| 0.3.3 | 3 | 66 |

多出的 6-13 s 是 A/B 快照与 sampler 停止的 SSH 开销，不是空闲。
吞吐一直按实际 `window_s`/`far_window_s` 计算，所以窗口偏长不改变速率。
修正后的规则：`60..80 s`，且 `app>0`、`wchar>0`、`stream_alive=1`。

## 2. 缺失 run

`--recompute-valid` 后 12 行中 10 行有效，2 行仍无效，原因都是
**curl 未启动**（`app_bytes=0`、`wchar_bytes=0`、`stream_alive=0`）：

| version | run | app_bytes | wchar | stream_alive | 备注 |
|---|---:|---:|---:|---:|---|
| 0.3.0 | 3 | 0 | 0 | 0 | curl 未启动；`drop_sid=8469` |
| 0.3.1 | 2 | 0 | 0 | 0 | curl 未启动；`drop_sid=9657` |

这两个 run 不重试，按缺失记录。它们不是窗口规则造成的。

## 3. 修正后统计（单 block，每版本 3 个 60 s 窗口）

| version | clean/valid | invalid | noisy | app 中位数 | 范围 | rel range | MAD | rel MAD | 判定 |
|---|---:|---:|---:|---:|---|---:|---:|---:|---|
| 0.3.0 | 0/2 | 1 | 3 | 31.721（all-valid，2 行） | 31.694..31.749 | 0.17% | 0.028 | 0.09% | INSUFFICIENT (no data) |
| 0.3.1 | 2/2 | 1 | 1 | 35.527（clean，2 行） | 35.417..35.637 | 0.62% | 0.110 | 0.31% | INSUFFICIENT (n<3) |
| 0.3.2 | 3/3 | 0 | 0 | 31.069（clean，3 行） | 30.015..31.927 | 6.15% | 0.858 | 2.76% | 基线 |
| 0.3.3 | 3/3 | 0 | 0 | 35.793（clean，3 行） | 35.577..35.933 | 0.99% | 0.140 | 0.39% | 对 0.3.2 BETTER |

0.3.0 的两行有效窗口都不是 clean：

- run 1：app 31.749，wchar 28.958；`|app-wchar|/app = 7.5% > 5%` → `app_wchar`。
- run 2：app 31.694，`drop_sid=71` → `drop_sid`。

0.3.1 的两行 clean 是 run 1 与 run 3；run 2 是上面的缺失行。

## 4. 版本间 verdict

| A | B | A 中位数 | B 中位数 | delta | dispersion | verdict |
|---|---|---:|---:|---:|---:|---|
| 0.3.0 | 0.3.1 | - | 35.527 | - | - | INSUFFICIENT (no data) |
| 0.3.0 | 0.3.2 | - | 31.069 | - | - | INSUFFICIENT (no data) |
| 0.3.0 | 0.3.3 | - | 35.793 | - | - | INSUFFICIENT (no data) |
| 0.3.1 | 0.3.2 | 35.527 | 31.069 | - | - | INSUFFICIENT (n<3) |
| 0.3.2 | 0.3.3 | 31.069 | 35.793 | **+15.20%** | 6.15% | **BETTER** |

`0.3.2 -> 0.3.3` 是唯一可判定的对：差值 15.20% 大于 5%，也大于两侧
离散度的较大值 6.15%。0.3.2 自身范围 6.15%，比 0.3.3 的 0.99% 宽得多；
这是 0.3.2 三窗里 30.015/31.069/31.927 的分布，不是 0.3.3 的抖动。

## 5. 边界

- 只有 **block 1**，没有反向 block，因此没有 drift 估计；0.3.2 的 6.15%
  范围也可能包含时间漂移。
- 0.3.0/0.3.1 的 n<3 不是“性能相同”，是“样本不足”。不要把 0.3.0 的
  all-valid 31.721 或 0.3.1 的 2 行 35.527 当成正式中位数。
- 0.3.2 -> 0.3.3 的 +15.20% 来自 n=3 对 n=3、单 block；修正后的脚本
  应补跑 G5 的 2 个 block（每版本 6 窗），确认该差值在反向 block 中仍
  大于离散度。
- 修正后的 `perf-ab.sh`/`perf-direct.sh` 已把 valid 规则改为 60..80 s；
  旧 CSV 用 `perf-stats.py --recompute-valid --secs 60` 复算。
