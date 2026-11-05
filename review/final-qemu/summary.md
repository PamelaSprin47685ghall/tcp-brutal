> **SUPERSEDED（历史快照）**
>
> 本文件基于 master 68bfbaa 的旧 QEMU 运行，**已被 f1282df 的最终证据取代**
> （f1282df = 3ba0e6a + w-docs5 + w-evidence2；模块与 b57b438 逐字节相同）。
> 当前最终结果见 `review/final/README.md` 与
> `review/final-qemu/adversarial-results-f1282df.txt`。
> 旧内容（包括“adversarial 未达到 79 PASS / 0 FAIL”和 68bfbaa 的旧失败）保留仅作历史；
> 不要按本文件的结论判断 0.0.1 是否可发布。

# tbtun 0.0.1 最终 QEMU 验收（final-qemu）

日期：2026-11-06
工作树：`/home/kunweiz/tbtun-worktrees/final-qemu`
分支：`w-final-qemu`
基线：`master 68bfbaa`，源码 HEAD `68bfbaa52c366f998527b4eb1050746283302757`
模块：`tbtun.ko` 5,950,104 B
模块 sha256：`0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63`
（`adversarial/results.txt` 每行的 `sha=0b8f0006a7a10fe3` 是该 sha 的前 16 位。）

## 0. 结论

- **QEMU 探针 A–K：11/11 PASS，0 SKIP。**
- **adversarial 的 klog 扫描器已修复并验证**：修前 `console-final.log` 命中 1 条
  （guest 自己的 PASS 说明），修后 0 条；注入真实 `[ 1.0] WARNING:` 仍被抓到。
- **adversarial 全套未达到 79 PASS / 0 FAIL / 0 SKIP。** 修后跑了两次，两次各有一个
  不同的真实间歇失败：`pub.churn`（2/10000 读超时）和 `pub.writer`（100 MiB 未阻塞、
  1024 MiB 的 MemAvailable 降到 176 MB）。两者都不是 klog 自匹配，也没有修改判据。
- 按“不要重跑掩盖”的要求，第二次失败后停止重跑；三次运行的原始结果全部保留。

## 1. 构建

```sh
source ~/.cache/tbtun-build/env.sh && make check
```

退出码 0。`make check` 输出（完整日志 `make-check.log`）：

- `test_parse`: 357 checks, 0 failures
- `test_fuzz`: seed=1 checks=7550734 failures=0
- `test_http`: seed=0x9e3779b97f4a7c15 checks=249 failures=0
- `test_reorder`: 52792 checks, 0 failures
- `test_ack`: 10026 checks, 0 failures
- `test_control`: 181 checks, 0 failures
- `test_multistream`: 68580 checks, 0 failures
- 124 checks, 0 failures
- h11 交叉校验通过
- Wall time: 2.41 seconds（host 测试部分）

模块在本次构建前已由同一工作树构建（`tbtun.ko` mtime 23:59:51，晚于全部源文件；
`make` 未重新编译），因此 sha 对应 HEAD 源码。扫描器修复只改 `adversarial/run.sh`，
不改模块，模块 sha 在三次 adversarial 运行中保持一致。

## 2. QEMU 探针 A–K

```sh
flock -x /tmp/tbtun-qemu.lock -c \
  'tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900 \
     --log review/final-qemu/qemu-AK-console.log'
```

- 退出码 0；**11/11 PASS，0 SKIP**；耗时 182.63 s。
- 未加 `--allow-skip`，未修改任何探针或判据。
- 控制台无 `BUG:`/`Oops`/`WARNING:`/`Call Trace:`/`INFO: task ... blocked`/
  `deprecated workqueue`/`refcount_t:`/`KASAN`/`UBSAN` 行。

| 探针 | 结果 | 关键证据（console） |
|---|---|---|
| A | PASS | 控制面、canonical 回读、拒绝行不改变配置 |
| B | PASS | 4 MiB 双向逐字节 |
| C | PASS | `carrier_lost=2 carrier_replaced=1 carrier0=up`；reconcile 见下 |
| D | PASS | `drop_ppm` 注入与重排计数 |
| E | PASS | `spare 4 took over: state=up bytes=2766274`；`spare_activated=1 replaced=0 lost=2`；reconcile 见下 |
| F | PASS | `dup=2`，无同路径重复 |
| G | PASS | 空洞修复，有界重排 |
| H | PASS | 持续丢包、恢复与有界重置 |
| I | PASS | control n=1 3.2 Mbit/s，main n=4 11.2 Mbit/s，**3.50x**（min 1.8），`strict=1 interleaved=1` |
| J | PASS | 每载体 1% 丢包 + 2% 重排，逐字节 |
| K | PASS | 单载体 100% 黑洞 2 s 后恢复，逐字节 |

## 3. 探针 C/E 的 reconcile

`tests/qemu/init:233-275` 的 `check_reconcile 2 10` 对每个探针采样 10 次，拒绝
`wire`/`data_tx`/`retx_frames` 回退，并在最后一次样本上要求精确相等。
console 只打印最后一次样本；C、E 都 PASS，且 console 没有
`reconcile: counter went backwards` 或 `reconcile: short sample` 行。

探针 C（tunnel 2，console 572-575）：

```
reconcile: wire=11636228 live=11496196 retired=140032 peers=11496196 d_wire=0
reconcile: data_tx=1421 live=1404 retired=17 d_data=0
reconcile: retx_frames=390 live=377 retired=13 d_retx=0
reconcile: live_bytes-peers_tx_bytes=0
```

- `11496196 + 140032 = 11636228`
- `1404 + 17 = 1421`
- `377 + 13 = 390`
- `live_bytes == Σ peers tx_bytes == 11496196`

探针 E（tunnel 2，console 686-689）：

```
reconcile: wire=12905466 live=12716227 retired=189239 peers=12716227 d_wire=0
reconcile: data_tx=1578 live=1555 retired=23 d_data=0
reconcile: retx_frames=544 live=530 retired=14 d_retx=0
reconcile: live_bytes-peers_tx_bytes=0
```

- `12716227 + 189239 = 12905466`
- `1555 + 23 = 1578`
- `530 + 14 = 544`
- `live_bytes == Σ peers tx_bytes == 12716227`

## 4. adversarial klog 扫描器修复

### 问题

旧扫描：

```sh
grep -cE 'BUG:|Oops|general protection fault|INFO: task .* blocked|deprecated workqueue' "$console"
```

它扫描整份 console，包括 harness 自己的输出。`console-final.log:406` 是 guest 的
成功行 `ADV_RESULT init.dmesg PASS no WARNING/BUG/Oops/INFO: task in the guest log`，
其中字面词 `Oops` 让扫描器把自己的 PASS 说明当成内核告警，产生
`console.klog.final FAIL`。

### 修复

`adversarial/run.sh` 新增 `klog_count()`：

- 先跳过 harness 前缀：`^ADV_`、`^PHASE_`、`^TBT_`、`^NEARX_`、`^FAR_`、
  `^CONC_`、`^REPRO_`、`^PARTIAL_WROTE`、`^burst:`、`^== `、`^# `；
- 只考虑带 `[    12.345]` 时间戳的行，或以 `WARNING:`、`BUG:`、`Oops`、
  `general protection fault`、`Call Trace:`、`INFO: task`、`refcount_t:`、
  `KASAN`、`UBSAN`、`deprecated workqueue` 开头的行；
- 计数模式保留并补全为：
  `WARNING:|BUG:|Oops|general protection fault|Call Trace:|INFO: task .* blocked|refcount_t:|KASAN|UBSAN|deprecated workqueue`。

完整 diff：`klog-scan-fix.diff`（42 行）。

### 验证

- 修前旧 grep 在首跑归档的 `adversarial-console-final.log` 上 = **1**。
- 修后 `klog_count` 在同一文件上 = **0**；首跑其余 console 也全部 = 0。
- 负测试（临时副本，已删除，未提交）：
  - 追加 `[    1.000000] WARNING: synthetic negative test` 和一行含
    `WARNING/BUG/Oops` 的 `ADV_RESULT` → 计数 **1**（真实内核行被计，harness 行被跳过）；
  - 再追加裸 `Call Trace:` → 计数 **2**；
  - 再追加 `[    2.000000] BUG: unable to handle page fault ...` → 计数 **3**。
- 修后两次全套运行的所有 console（含 `console-final.log`）`klog_count` 都是 0，
  没有任何真实内核告警。详见 `klog-scan-test.txt`、`klog-scan-run3.txt`。

## 5. adversarial 全套运行（三次）

命令（每次都在 `flock -x /tmp/tbtun-qemu.lock` 内）：

```sh
TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build
```

默认 15 组 + 独立 crash repro：
`public control resource slab final replay rmmod flush catconfig cfg_mix param port near far stall`。
三次运行的所有组都到达 `PHASE_END` 与 `TBT_ADV_DONE`，crash repro 都是 PASS
（`REPRO_READY`/`REPRO_SURVIVED`/`TBT_REPRO_DONE`）。

| 运行 | 时机 | PASS | FAIL | SKIP | 墙钟 | FAIL 行 |
|---|---|---|---|---|---|---|
| run 1 | 修前 | 78 | 1 | 0 | 1567.81 s（可能含等锁） | `console.klog.final` 自匹配 |
| run 2 | 修后 | 77 | 1 | 0 | 237.94 s | `pub.churn` |
| run 3 | 修后 | 76 | 2 | 0 | 227.97 s | `pub.writer` ×2 |

### run 1（修前）

```
ADV_RESULT console.klog.final FAIL sha=0b8f0006a7a10fe3 1 kernel oops/warning lines (console /home/kunweiz/tbtun-worktrees/final-qemu/adversarial/console-final.log)
```

已确认为扫描器自匹配，修复见第 4 节。

### run 2（修后，churn 失败）

```
ADV_RESULT pub.churn FAIL sha=0b8f0006a7a10fe3 10000 connect/send/close cycles: usable=9998 refused=0 other=2 (a closed stream must free the session for the next connection)
```

`adv_probe churn` 每轮 connect/send 1 字节/读 1 字节；`other=2` 是 2 轮在 800 ms 内
没有读到回显（`read_to` 返回 -2 超时，或 connect 失败）。同一轮其余 9998 轮可用，
`refused=0`。该组之后 `pub.two`、`pub.concurrent`（100/100）、`pub.halfclose`、
`pub.rst`、`pub.writer`、`pub.srcfilter` 都 PASS。

### run 3（修后，writer 失败）

```
ADV_RESULT pub.writer FAIL sha=0b8f0006a7a10fe3 attempted 100 MiB, accepted 104857600 bytes then blocked (errno=0), MemAvailable 397588 -> 186840 kB
ADV_RESULT pub.writer FAIL sha=0b8f0006a7a10fe3 attempted 1024 MiB, accepted 120435907 bytes then blocked (errno=11), MemAvailable 398204 -> 176028 kB
```

判据是 `sent < total && after > 200000`：

- 100 MiB 用例把 100 MiB 全部写入（`sent == total`），没有发生阻塞，`errno=0`；
- 1024 MiB 用例在 120,435,907 B 后阻塞，但 `MemAvailable` 降到 176,028 kB，
  低于 200,000 kB。

run 1/2 的同一用例在约 75 MiB 处阻塞，`MemAvailable` 终点为 253–257 MB，PASS。
run 3 的 churn 是 10000/10000 PASS。两次修后运行的失败项不同，说明这是
环境/缓冲阈值敏感的间歇行为，不是扫描器问题。

### 关于“79 PASS”

adversarial 的固定攻击判定是 **78 条**（far 23、near 14、public 10、control 10、
resource 2、slab 2、final 2、replay 2、rmmod 3、flush 2、catconfig 2、cfg_mix 1、
param 1、port 1、stall 2、crash 1）。第 79 行是 `console.klog.<group>` 条件检查，
只在失败时输出；扫描器修好后，全绿运行的期望是 **78 PASS / 0 FAIL / 0 SKIP**，
`run.sh` 会打印 `78 passed`。要得到字面 79 PASS，必须让 klog 检查在成功时也输出
一行 PASS，那是判据/输出契约变更，本次未做。

## 6. 重跑记录

- 修前 run 1：`console.klog.final` 自匹配失败。
- 修后 run 2：`pub.churn` 2/10000 失败；为区分“扫描器修复引入”还是既有 flake，
  原样重跑一次。
- 修后 run 3：`pub.writer` ×2 失败（与 run 2 不同的项）。此后停止重跑，避免用
  重跑掩盖间歇失败；三次结果全部保留。
- A–K 没有失败，未重跑。

## 7. 耗时

- QEMU A–K：182.63 s。
- adversarial run 1：1567.81 s（可能包含等待 QEMU 锁；当时另一 worker 的 bench 已排队，
  run 2/3 无此等待，约 230 s）。
- adversarial run 2：237.94 s。
- adversarial run 3：227.97 s。

## 8. 剩余风险与未覆盖项

1. **adversarial 0 FAIL 门禁未满足**：`pub.churn` 与 `pub.writer` 各出现一次真实
   间歇失败；不是 klog 扫描器问题，也没有改判据。
2. **churn 的 2/10000 超时**：属于已知的顺序流复用/FIN 释放面（F4 顺序部分）。
   本次 10000 轮里 9998 可用、0 refused；另一次运行 10000/10000。需要更多轮次或
   在安静主机上定位。
3. **writer 的阈值敏感**：100 MiB 可能在管道缓冲内全部写完而不阻塞；1024 MiB 的
   `MemAvailable` 可能低于 200 MB。判据要求 `sent < total && after > 200000`，
   在 512 MiB guest 上对系统状态敏感。是测试阈值问题还是模块缓冲行为，需要单独判定。
4. **“79 PASS”口径**：固定攻击是 78 条；第 79 行是条件 klog 检查，全绿时不存在。
5. **J/K 3/3、E/K 3/3**：`review/final-checklist.md` 的 C07/C08/C09 要求连续 3 次，
   本票只要求一次完整 A–K，未执行。
6. **retx_ppm 观测值**：探针 C/E 的 peers 行在环回丢包注入下出现 303k–570k 的
   `retx_ppm`；不是本票判据，但与 P2 `<100k` 目标相关，且来自环回注入而非整形链路。
7. **未签名模块 taint**：`loading out-of-tree module taints kernel` 与
   `module verification failed` 是预期信息。
8. `adversarial/console-{halfclose,srcfilter,stable,writer}.log` 是更早运行的遗留
   文件，不属于本次 15 组；本次 public 组内部的 srcfilter 阶段记录在
   `console-public.log`。

## 9. 证据文件

| 文件 | 内容 |
|---|---|
| `qemu-AK-summary.txt` | A–K 结果、reconcile 算术、命令 |
| `qemu-AK-run.log` | run.sh 完整输出 |
| `qemu-AK-console.log` | 探针 guest 完整 console（95 KB） |
| `adversarial-results.txt` | run 3 的 `adversarial/results.txt` 原样副本（78 行） |
| `adversarial-results-run2-churnfail.txt` | run 2 的原始 results（含 churn FAIL） |
| `adversarial-run.log` | run 1 的完整输出与表格（修前） |
| `adversarial-run-2.log` | run 2 的完整输出与表格（修后，churn FAIL） |
| `adversarial-run-3.log` | run 3 的完整输出与表格（修后，writer FAIL） |
| `adversarial-console-public.log` | run 3 public 组完整 console |
| `adversarial-console-public-run2-churnfail.log` | run 2 public 组完整 console |
| `adversarial-console-final.log` | run 3 final 组完整 console（klog 干净） |
| `adversarial-console-final-run2.log` | run 2 final 组完整 console |
| `adversarial-crash-console.log` | run 3 crash repro console |
| `klog-scan.txt` | 修前扫描与自匹配解释 |
| `klog-scan-test.txt` | 修前/修后对比与负测试 |
| `klog-scan-run3.txt` | run 3 全部 console 的 klog 计数（全 0） |
| `klog-scan-fix.diff` | `adversarial/run.sh` 的扫描器 diff |
| `module.sha256` | 模块 sha256 |
| `head.txt` | `git rev-parse HEAD` |
| `make-check.log` | 构建与 host 测试输出 |
