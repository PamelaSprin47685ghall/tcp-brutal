# tbtun 0.0.1 最终 QEMU 验收（final-qemu）

日期：2026-11-06
工作树：`/home/kunweiz/tbtun-worktrees/final-qemu`
分支：`w-final-qemu`
基线：`master 68bfbaa`，HEAD `68bfbaa52c366f998527b4eb1050746283302757`
模块：`tbtun.ko` 5,950,104 B
模块 sha256：`0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63`
（`adversarial/results.txt` 每行的 `sha=0b8f0006a7a10fe3` 是该 sha 的前 16 位。）

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
`make` 未重新编译），因此 sha 对应 HEAD 源码。

## 2. QEMU 探针 A–K（命令 1）

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

## 3. 探针 C/E 的 reconcile（命令 2）

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

## 4. adversarial 全套（命令 3）

```sh
flock -x /tmp/tbtun-qemu.lock -c \
  'TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build'
```

默认 15 组 + 独立 crash repro：
`public control resource slab final replay rmmod flush catconfig cfg_mix param port near far stall`。

- 退出码 1；**78 PASS / 1 FAIL / 0 SKIP**（79 行 ADV_RESULT）。
- public 组 timeout=3000 s；`pub.churn` 10000/10000 usable，约 23 分钟。
- 所有 15 组都出现 `PHASE_END` 与 `TBT_ADV_DONE`，无 `console.hang.*`/
  `console.missing.*`/`console.done.*` 行。
- crash repro：`ADV_RESULT ctl.close_flush PASS`；console 有 `REPRO_READY`、
  `REPRO_SURVIVED`、`TBT_REPRO_DONE`，无 oops。

### 唯一的 FAIL 行

```
ADV_RESULT console.klog.final FAIL sha=0b8f0006a7a10fe3 1 kernel oops/warning lines (console /home/kunweiz/tbtun-worktrees/final-qemu/adversarial/console-final.log)
```

根因（`klog-scan.txt` 有完整表）：`run.sh` 用
`grep -cE 'BUG:|Oops|general protection fault|INFO: task .* blocked|deprecated workqueue'`
扫描整个 console，唯一命中是 guest 自己的成功行：

```
406:ADV_RESULT init.dmesg PASS no WARNING/BUG/Oops/INFO: task in the guest log
```

该行文本含字面词 `Oops`，于是扫描器把测试自己的 PASS 说明当成内核告警。
同一 console 的 `init.dmesg` 是 PASS；所有 console 都没有 `WARNING:`、
`Call Trace:`、`refcount_t:`、`KASAN`、`UBSAN` 或真实 oops。这是测试脚本的
自匹配假阳性，不是模块缺陷。按“不得改判据”的要求，未修改 `adversarial/run.sh`。

### 其他值得记录的结果

- `init.port_exhaust PASS tunnel 3 accepted but the exhaustion is operator-visible:
  port_exhaust=15 port_exhaust=16 (alloc_fail=16, log lines=10)`：按当前契约
  （操作员可见的耗尽原因）PASS，但隧道本身仍被接受。
- `pub.writer PASS` 两次都在约 75 MiB 后 `EAGAIN` 阻塞，`MemAvailable`
  397476→254416 kB 与 398188→252768 kB；高于 200000 kB 判据。
- `near.fin_eagain`/`near.rst_eagain` PASS，`pump_hiwat` 分别 0→4956、0→219。
- `far.srcaddr_free`、`far.data_path`、`near.data_path`、`near.second_conn`、
  `init.src_key`、`pub.srcfilter` 等旧 findings 中的项本次均 PASS。

## 5. 重跑记录

无重跑。A–K 首跑 11/11 PASS，没有出现 K 类时序 flake；adversarial 的 FAIL
是确定性的扫描器自匹配（同一行文本必然命中），重跑只会复现同一行，因此没有
用重跑掩盖或改变判据。

## 6. 耗时

- QEMU A–K：182.63 s（约 3 分 03 秒）。
- adversarial 全套：1567.81 s（约 26 分 08 秒，含 public churn）。
- 两项合计：约 29 分 10 秒。构建与 host 测试另计（`make check` host 部分 2.41 s）。

## 7. 剩余风险与未覆盖项

1. **发布门禁“0 FAIL”未满足**：`console.klog.final` 是假阳性。要让它变绿只能改
   扫描器（排除 `ADV_RESULT` 行）或改 `init.dmesg` 的 detail 文本；两者都属于
   测试判据，超出本工权限。
2. **J/K 的单次通过**：本票要求一次完整 A–K；`review/final-checklist.md` 的
   C07/C08 还要求 J、K 各连续 3 次，C09 要求 E、K 各 3 次，本次未执行。
3. **retx_ppm 观测值偏高**：探针 C/E 的 peers 行在环回丢包注入下出现
   303k–570k 的 `retx_ppm`。这不是本票判据，但和 P2 的 `<100k` 目标相关；
   该数字来自环回注入，不是整形链路，不能直接与 benches 的 `retx_ppm` 比较。
4. **pub.writer 余量**：`MemAvailable` 终点约 253–254 MB，高于 200 MB 判据但
   余量不大；1024 MiB 用例同样在约 75 MiB 后阻塞。
5. **port_exhaust 的语义**：第三条隧道仍被接受，PASS 依赖操作员可见的计数器与
   日志行。若未来契约要求直接拒绝，该项需要重新判定。
6. **模块 taint**：`loading out-of-tree module taints kernel` 与
   `module verification failed` 是未签名树外模块的预期信息，不是告警。
7. `adversarial/console-{halfclose,srcfilter,stable,writer}.log` 是更早运行的
   遗留文件，不属于本次 15 组；本次 public 组内部的 srcfilter 阶段记录在
   `console-public.log`。

## 8. 证据文件

| 文件 | 内容 |
|---|---|
| `qemu-AK-summary.txt` | A–K 结果、reconcile 算术、命令 |
| `qemu-AK-run.log` | run.sh 完整输出 |
| `qemu-AK-console.log` | 探针 guest 完整 console（95 KB） |
| `adversarial-results.txt` | `adversarial/results.txt` 原样副本（79 行） |
| `adversarial-run.log` | adversarial/run.sh 完整输出与表格 |
| `adversarial-console-final.log` | FAIL 所在组的完整 console |
| `adversarial-console-public.log` | public 组（含 churn）完整 console |
| `adversarial-crash-console.log` | close-flush crash repro console |
| `klog-scan.txt` | 所有 console 的 klog/oops/WARN 扫描与命中解释 |
| `module.sha256` | 模块 sha256 |
| `head.txt` | `git rev-parse HEAD` |
| `make-check.log` | 构建与 host 测试输出 |
