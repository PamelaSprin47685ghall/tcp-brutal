# T1 本地套件 — 0888ce4（2026-11-06）

## 结论

- A–K 连续 3 次：11/11 PASS、0 SKIP，三次 rc=0。
- J isolated 5/5 PASS。
- near 组 15/15 PASS，含 `near.fin_eagain` 与 `near.rst_eagain`。
- 完整 adversarial（18 组 + crash repro）93 PASS / 0 FAIL / 0 SKIP。
- `make host-test-san` rc=0。
- 本轮取代 4c58b0e 的本地套件：后者在 A–K 序列中 J 偶发 1/3 失败，
  0888ce4 修掉了该根因（见下节）。

## 源码与模块

| 项 | 值 |
|---|---|
| head | `0888ce40e439ea9e4f947b8694dcfc6d0785e027`（`w-window`） |
| ko | `/home/kunweiz/tbtun-worktrees/window/tbtun.ko` |
| ko sha256 | `a6e459f6d8790e2cb85f8710aefaa861ea3d556ed9d9029cd841a0c78129eea5` |
| size | 6,088,504 B |
| srcversion | `711422E4053E3F13B89BD4E` |
| vermagic | `7.2.9-1-MANJARO SMP preempt mod_unload` |
| 构建 | `make clean` + 全量重编，09:15:17→09:15:24 +08:00，gcc 16.2.1 |

构建结果与 window-close 自己的 `a6e459f6` 逐字节一致。

## J flake 的根因与修复

4c58b0e 的 full A–K run #1 失败时，far 侧证据是：

- 流 `kill=4`（`TBT_ST_HOLE`），`holes_unrecoverable=1`，
  `rx_max_stall_us=1,005,262`，`rst_w=1`。
- 不是 close-drain 或 stall 误判：`close_timeouts=0`、
  `deliver_timeouts=0`、`close_detaches=0`。
- near 侧 `una_stall_us=998,975`：carrier 的 `snd_una` 卡了约 1 s，
  处于 TCP RTO backoff（200 + 400 + 800 ms）中，`retx_slow=32`；
  far 侧 `ooo_frames=151`。

即 receiver 的 auto hole budget 下限 1 s 比它等待的 carrier 修复窗口更短，
而 sender 自己的 stall budget 是 `max(5 s, 5 × RTO)`。

0888ce4 的修复（`tbtun/tbt_rx.c`、`tbtun/tbt_tx.c`、`tests/qemu/init`、
`DESIGN.md`）：

- auto 预算改为 `max(2 s, 5 × SRTT)`，下限从 1 s 提到 2 s。
- 显式 `hole_budget_ms` 语义不变（probe H 仍断言 500 ms）。
- probe G 的 auto 上限同步改为 2000 ms。
- `DESIGN.md` §4.6 记录原因。

## A–K（`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 600`）

| run | 结果 | 备注 |
|---|---|---|
| #1 | 11/11 PASS，0 SKIP | rc=0 |
| #2 | 11/11 PASS，0 SKIP | rc=0 |
| #3 | 11/11 PASS，0 SKIP | rc=0 |

三次合计 611 s。J isolated 另跑 5 次，5/5 PASS。

## near 组（`TBT_ADV_GROUPS=near adversarial/run.sh --no-build`）

15 passed / 0 failed / 0 skipped。`near.fin_eagain`、`near.rst_eagain`
均 PASS；`ctl.close_flush` PASS。

## 完整 adversarial

`TBT_ADV_GROUPS="public control resource slab final replay rmmod flush
catconfig cfg_mix param port near far stall httptorn hole create"
adversarial/run.sh --no-build`

93 PASS / 0 FAIL / 0 SKIP，rc=0，300 s。关键行：

- `pub.writer_drain_1g` PASS：1024 MiB / 6 s 级，读写各 1073741824 B。
- `pub.closedrain`、`pub.closedrain_timeout` PASS。
- `pub.churn` PASS：10000 连接，usable=10000，refused=0。
- `ctl.close_flush` PASS（独立 crash repro guest 存活）。

逐条 `ADV_RESULT` 见同目录 `t1-0888ce4-local-results.txt`（93 行，
sha 前缀 `a6e459f6d8790e2c`）。

## host-test-san

`make host-test-san` rc=0：

- `test_parse` 385 checks / 0 failures
- `test_fuzz` 7,550,734 checks / 0 failures
- `test_http` 249 checks / 0 failures
- `test_http_fuzz` 4,078,276 checks / 0 failures

## QEMU 独占与并发事故

4c58b0e 的 A–K run #5、#6 曾因与其它 agent 共享
`~/.cache/tbtun-build/qemu/` 而失败（`libs.txt` 被并发删除导致 rc=2；
半成品 initramfs 导致 guest panic 于 `mount_root_generic`，rc=1）。
这两次是环境故障，不是产品判定，未计入上面的 tally。

0888ce4 的测试在独占下完成：A–K 3x 之后的每次 QEMU 调用都持有
`flock /tmp/tbtun-qemu.lock`，测试结束后释放，`pgrep qemu-system` 为空。

## 残余风险 R11

hnd0-5x 在 per-round module reload 下观察到 carrier-rebuild race：
attempt-1 3/5、attempt-2 1/5，far `lost=1`，near HELLO 超时后重拨；
数据仍 16 MiB / EOF 完整。该现象只在每轮重新加载模块时出现，
loaded-once 的 T1 gate 不受影响，记为独立 ticket R11。

## 证据

- 本目录 `t1-0888ce4-local-results.txt`：完整套件的 93 条结果。
- 大日志不入库，保存在 `~/.cache/tbtun-build/a2-evidence/0888ce4/`：
  A–K 三次、J 五次、near 组、完整 adversarial、SAN、构建日志。
- 4c58b0e 的旧套件保存在 `~/.cache/tbtun-build/a2-evidence/4c58b0e/`，
  已被本轮取代。
