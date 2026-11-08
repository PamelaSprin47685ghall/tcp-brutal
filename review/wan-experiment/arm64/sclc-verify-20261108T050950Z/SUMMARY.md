# SC-LDPC 0.3.0 fix43 真机验收（真实模块，PASS）

- 模块: tbtun 0.2.3, HEAD `830c7f7`
  - near srcversion `B66D7B86A162F744837BFF3`
  - far  srcversion `2296207711D4F7FC38D809F`
- near `root@192.168.123.82` (aarch64, 6.1.172)
- far  `root@45.11.1.62` (x86_64, 7.0.0-42-generic)
- 首连/稳态证据: `review/wan-experiment/arm64/sclc-verify-20261108T050535Z/`
- 1 GiB 证据: 本目录 `review/wan-experiment/arm64/sclc-verify-20261108T050950Z/`

## 判定

| 阶段 | 结果 | 关键数字 |
|---|---|---|
| 配置/join | **PASS** | 两端 up=256；far `L=16`、near `L=8`；adaptive=off；无 WARNING |
| 首连 15 s | **PASS** | HTTP 200；53,477,376 B；TTFB 1.262 s；far decode +4；replay high 4,919,280 < 5 MiB；backpressure 0；hol_wait 19,031（只记录）；near ack_tx +43；far ack_rx +44；无 reset/holes |
| 256 稳态 20+60 s | **PASS** | app 155,500,544 B/60 s（2.59 MB/s）；wire far 374,431,233 / near 312,192,380；outq_drop 0；nack_backpressure 0；near ack_tx +143；far ack_rx +142；near/far credit 各自推进；hol_wait 0 / 1,834,304（加权，只记录）；无 reset/holes |
| 1 GiB 单流 | **PASS（byte-exact）** | rc=0；HTTP 200；size/file 1,073,741,824；near/far sha256 均 `e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393`；TTFB 1.968 s；737.60 s（11.65 Mbit/s） |

## 1 GiB 详细

```
http=200 size=1073741824 ttfb=1.967721 time=737.595406 speed=1455732
near sha256 e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393
far  sha256 e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393
```

| 计数器（窗口增量） | near | far |
|---|---|---|
| `c3 up` | 256 | 256 |
| `c10 fec_decode_blocks` | **+8372** | 0（上行 GET，info） |
| `c10 fec_decode_recovered` | +55,859 | 0 |
| `c11 fec_nack_tx / rx` | +893 / +3 | +3 / +901 |
| `c11 fec_nack_retx` | 0 | **6949** |
| `c11 fec_replay_hit / miss` | 0 / 24 | **6949 / 42,963**（never_stored 42,963） |
| `c11 fec_replay_queue_high` | 140,080 | **4,919,280**（< 5,242,880） |
| `c11 fec_nack_backpressure` | 0 | **0** |
| `c11 fec_outq_drop` | 0 | 0 |
| `c11 fec_outq_high_wait` | 0 | 31（pacing，info） |
| `c11 fec_hol_wait` | 0 | 23,430,162（按深度加权，info） |
| `c9 fec_ack_tx` | **+994** | +1 |
| `c9 fec_ack_rx` | +1 | **+994** |
| `c9 fec_credit_wait` | 0 | +10,044（near ack_tx +994，ACK 在推进） |
| reset_tcp / reset_hole / holes_unrec / deliver_bad | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| wire（far tx / near rx） | 2,303,697,107 | **2,828,190,196** |
| wire/app | 2.145 | **2.634** |
| CPU busy | **8.06%** | **42.87%** |

观察项（不判失败）：
- far `fec_replay_miss=42,963`（全部 `never_stored`）：far 的 replay 环没有这些被请求的
  share，接收端重试；1 GiB 仍 byte-exact 完成，replay 队列高水位 4.9 MiB、
  backpressure 0。属于修复效率/环覆盖的残余，不是本次判据的“打满”。
- far `fec_hol_wait=23,430,162` 是按 per-carrier 队列深度加权的量
  （`tbt_fec_data.c:753/897/1036` 在 `-EAGAIN` 时 `+= depth`），
  TX outq 深但持续排空；按用户要求只记录。
- 1 GiB 速度 1.46 MB/s 低于稳态 2.59 MB/s；完成时间 737.6 s，远低于 7200 s 上限。

## 首连与稳态（证据目录 050535Z）

首连：

```
curl: (28) Operation timed out after 15002 milliseconds with 53477376 out of 1073741824 bytes received
http=200 size=53477376 ttfb=1.261854 time=15.002101 speed=3564659
```

- near decode +269、ack_tx +43；far decode +4、ack_rx +44、credit_wait +274；
- far replay high 4,919,280、max_queued 582、backpressure 0；near replay high 140,080；
- far nack_retx 861 / 7,094,640 B（retx ratio 0.133）；
- hol_wait far 19,031 / near 0（只记录）；无 reset/holes；传输窗口无 tbtun hog。

稳态：

- app 155,500,544 B / 60 s；wire far 374,431,233 / near 312,192,380；CPU near 12.5% / far 57.2%；
- outq_drop 0、nack_backpressure 0、near ack_tx +143、far ack_rx +142、
  near credit 0、far credit +1451（peer near ack_tx +143）、replay high 4,919,280；
- hol_wait near 0 / far 1,834,304（加权，info）；无 reset/holes；up=256。

## Harness 修正记录

1. `steady_report` / `one_report` 的 `local end=$1 pre=...$end...` 在 bash 中
   用旧的全局 `end=far` 展开，导致 near/far 报告都读 far 的 stats。已改为逐条
   `local` 赋值；050535Z 的稳态检查因此 near/far 各自正确（near ack_tx 143、
   far ack_rx 142，credit 各自 PASS）。
2. `one_report` 原先没有输出 `ack_rx`，1 GiB 的 `far_one_g_ack_rx` 读到 `?`
   而误判 FAIL；真实 far `fec_ack_rx` 从 241 增至 1235（+994）。已补上。
3. `--one-g-only` 的 SUMMARY 引用了 steady 阶段才设置的 `near_cpu` 等变量，
   `set -u` 在写 SUMMARY 时崩溃（RUN_RC=1），cleanup 未执行。已加默认值；
   本次 cleanup 已单独执行并 PASS。

说明：首连/稳态在 050535Z 一次跑完并通过；该进程在稳态检查后、1 GiB 前被我
提前终止（误判了 crosscheck 风险），因此 1 GiB 在同一会话上用 `--one-g-only`
续跑一次。数据面只各跑一次，没有重试失败的测试。

## 清理（已完成）

`cleanup.txt`（本目录）：

- far raw sender 停止：`FAR_STOP PASS`，5210 释放；
- near `pref 100 to 45.11.1.62 lookup main` 删除（前后 `ip rule show` 在证据中）；
- near / far `rmmod tbtun`：两端 `tbtun not loaded`；
- watchdog：`tbtun-netwatch` inactive，未触碰；其他服务/隧道未动；
- far 1 GiB 源文件保留；near 1 GiB 文件保留在 `/root/sclc-verify/sclc-1g.bin`。

## 证据文件

首连/稳态（`sclc-verify-20261108T050535Z/`）：
`run.log`、`first-connect.txt`、`first-sampler-*.log`、`first-during-*.txt`、
`first-analysis.txt/json`、`steady-a/b-*-stats.txt`、`steady-sampler-*.log`、
`*-kernel-first.txt`、`console.log`。

1 GiB（本目录）：
`run.log`、`one-g-curl.txt`、`one-g-size.txt`、`one-g-near-sha.txt`、
`far-serve.log`（FAR_SHA256）、`one-g-{near,far}.txt`、
`one-g-*-pre/post.txt`、`one-g-*-procstat-*`、`one-g-*-netdev-*`、
`one-g-sampler-*.log`、`*-kernel-one-g.txt`、`near-post.txt`/`far-post.txt`、
`cleanup.txt`、`console.log`。
