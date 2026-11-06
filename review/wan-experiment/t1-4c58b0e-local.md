# T1 本地套件 — 4c58b0e（2026-11-06）

## 结论

- A–K 跑了 3 次：run #2、#3 均 11/11 PASS、0 SKIP；run #1 只有 probe J
  偶发失败（详见下文），isolated J 4/4 PASS。J 的失败依赖 A–K 序列，
  不是 4c58b0e 的确定性回归。
- near 组 15/15 PASS，含 `near.fin_eagain` 与 `near.rst_eagain`。
- 完整 adversarial（18 组 + crash repro）93 PASS / 0 FAIL / 0 SKIP。
- `make host-test-san` rc=0。
- 残余风险 R11（per-round module reload 的 carrier-rebuild race）不阻塞
  loaded-once gate，见末节。

## 源码与模块

| 项 | 值 |
|---|---|
| head | `4c58b0eed002c1cd0c0fb15f9b04f97b44ff1beb`（`w-window`，parent `ddef988`） |
| ko | `/home/kunweiz/tbtun-worktrees/window/tbtun.ko` |
| ko sha256 | `c14bafee9a858a4b406edd580b91906734a6ebbbf27dc0e2bdd7d14c8aafb437` |
| size | 6,088,496 B |
| srcversion | `10C45DCB900C5C78F725596` |
| vermagic | `7.2.9-1-MANJARO SMP preempt mod_unload` |
| 构建 | `make clean` + 全量重编，08:52:25→08:52:33 +08:00，gcc 16.2.1 |

构建结果与 A3 自己的 `c14bafee` 逐字节一致。

## A–K（`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 600`）

| run | 结果 | 说明 |
|---|---|---|
| #1 | 10/11 | 仅 J FAIL：`byte-exact completion failed under 1% loss / 2% reorder (rc=1)`；失败流 `rst_w=1 live_bytes=2276044 retx=34 holes_unrecoverable=0` |
| #2 | 11/11 PASS，0 SKIP | 207.97 s |
| #3 | 11/11 PASS，0 SKIP | 201.07 s |
| J isolated | 4/4 PASS | 单跑 1 次 + 连跑 3 次，每次 17–45 s |

J 的判定是 `tbt_burst` 在 1% 丢包 / 2% 重排下两个方向逐字节完成。
失败发生在 A–K 序列中（I 探针之后），isolated 4/4 不复现；因此按
sequence-dependent flake 记录，而非确定性失败。run #1 的证据保留在
`~/.cache/tbtun-build/a2-evidence/4c58b0e/a2-AK-4c58b0e.log`。

## near 组（`TBT_ADV_GROUPS=near adversarial/run.sh --no-build`）

15 passed / 0 failed / 0 skipped，24.85 s。`near.fin_eagain` 与
`near.rst_eagain` 均 PASS；`ctl.close_flush` PASS。

## 完整 adversarial

`TBT_ADV_GROUPS="public control resource slab final replay rmmod flush
catconfig cfg_mix param port near far stall httptorn hole create"
adversarial/run.sh --no-build`

93 PASS / 0 FAIL / 0 SKIP，rc=0，282.48 s。关键行：

- `pub.writer_drain_1g` PASS：1024 MiB / 6.27 s，读写各 1073741824 B。
- `pub.closedrain`、`pub.closedrain_timeout` PASS。
- `pub.churn` PASS：10000 连接，usable=10000，refused=0。
- `ctl.close_flush` PASS（独立 crash repro guest 存活）。

逐条 `ADV_RESULT` 见同目录 `t1-4c58b0e-local-results.txt`（93 行，
sha 前缀 `c14bafee9a858a4b`）。

## host-test-san

`make host-test-san` rc=0：

- `test_parse` 385 checks / 0 failures
- `test_fuzz` 7,550,734 checks / 0 failures
- `test_http` 249 checks / 0 failures
- `test_http_fuzz` 4,078,276 checks / 0 failures

## 残余风险 R11

hnd0-5x 在 per-round module reload 下观察到 carrier-rebuild race：
attempt-1 3/5、attempt-2 1/5，far `lost=1`，near HELLO 超时后重拨；
数据仍 16 MiB / EOF 完整。该现象只在每轮重新加载模块时出现，
loaded-once 的 T1 gate（sendclose 5/5、shutdown-WR 5/5）不受影响，
记为独立 ticket R11。

## 证据

- 本目录 `t1-4c58b0e-local-results.txt`：完整套件的 93 条结果。
- 大日志不入库，保存在
  `~/.cache/tbtun-build/a2-evidence/4c58b0e/`：A–K 三次、J 单跑与三次
  连跑、near 组、完整 adversarial、SAN、构建日志。
- `ddef988` 的同名套件结果已 superseded，保存在
  `~/.cache/tbtun-build/a2-evidence/ddef988/`。
