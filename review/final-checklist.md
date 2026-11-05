> **SUPERSEDED（历史快照）**
>
> 本文件记录的是 2026-11-05 旧树上的门禁状态，**已被 f1282df 的最终证据取代**
> （f1282df = 3ba0e6a + w-docs5 + w-evidence2；模块与 b57b438 逐字节相同）。
> 当前最终结果见 `review/final/README.md` 与
> `review/final-qemu/adversarial-results-f1282df.txt`。
> 旧内容（包括 FAIL、未跑项和 365/314 口径冲突）保留仅作历史；
> 不要按本文件的结论判断 0.0.1 是否可发布。

# tbtun 0.0.1 发布前最终回归清单

本文只列门禁：每条给出**验收命令**、**期望输出/数字**、**当前状态**、**证据路径**。
不修改源码，不构建，不启动 QEMU。

## 0. 基线与状态约定

### 0.1 两个基线，不要混用

| 基线 | 模块 | 用途 | 位置 |
|---|---|---|---|
| 旧实测基线 | 5,405,288 B，sha `c18ff6c4…`，2026-11-05 06:28 | 证明“修复前确实坏”，作为回归对照 | `review/baseline.md`、`review/baseline/` |
| 发布候选树 | 当前工作树（2026-11-05 11:20 构建，5,423,560 B） | 0.0.1 的验收对象；每次跑完必须记录新 sha | `tbtun.ko`、`benches/results/*.csv` |

旧基线中 `n=4 app 1.31 Mbit/s、wire/app 33.06、retx_ppm 1.17M` 是**修复前**的数字，不能作为当前状态。
当前吞吐证据来自 `CHANGELOG.md` 的 Verification 与 README 的 Verified performance：
`n=4 app 11.39 Mbit/s、wire/app 1.0005、retx_ppm 96,095`（CSV `20261105T112245`），
`n=1 app 3.46 Mbit/s、wire/app 1.0316、retx_ppm 1,090`（CSV `20261105T112725`）。

### 0.2 状态取值

- **PASS**：在当前发布候选树上有本次或修复者归档的实测输出。
- **FAIL**：有实测输出不满足期望。
- **未跑**：没有当前树的实测输出。
- **待多流完成**：依赖 P1 多流落地，当前实现按旧契约必然失败或按旧契约通过。
- **待复跑归档**：修复者报告已通过，但当前树没有可引用的原始输出。
- **待决策**：需要发布负责人决定“修”还是“写入已知限制”。

### 0.3 通用前置

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal
source ~/.cache/tbtun-build/env.sh    # KDIR / PAHOLE / LD_LIBRARY_PATH
```

QEMU 与 root 操作使用显式 `sudo`；**禁止 `pkexec`**（本机 polkit 配置错误）。
每次发布候选构建后先记录：

```sh
sha256sum tbtun.ko
git rev-parse HEAD
git status --porcelain
```

期望：sha 与 `benches/results/*.csv` 的 `ko_sha256` 列一致；工作树状态被记录，不要求干净。

### 0.4 当前状态快照（2026-11-05，P2/P3 后）

| 项 | 当前状态 | 证据 / 待验 |
|---|---|---|
| 版本 | **已统一 0.0.1**：`TBT_VERSION` 0/0/1、`TBT_WIRE_VERSION=1`、`TBT_CONV=0x0004`、`dkms.conf` 0.0.1、`modinfo` 0.0.1、install dry-run 0.0.1 | `tbt_uapi.h:30-49`、`dkms.conf:2`、`modinfo -F version tbtun.ko`、`install_dkms.sh --dry-run`；`dkms.tar.gz` 仍是旧版本，待最终回归后重新生成 |
| P2 | **完成**：每流 `tx_ack` + 帧引用、40 位 sid/conv v4、SACK、FBK 路径字节 | host `test_ack 10,026`、`test_fuzz 8,525,966`、`test_reorder 51,831` |
| P3a | **完成**：每流 `target` | 代码与 probe 行为；待最终归档 |
| P3b1 | **部分完成**：两条并发正确（`pub.two` 新契约 PASS）；`pub.churn` 9896/8/96，约 1% 读超时 | 多流 retx/FBK 遍历正在修 |
| 已验证探针 | `B` PASS；`public` 的 `churn`/`two`/`halfclose`/`rst`/`writer` 已验证；`I` 最近一次 PASS（环境变慢，不要求 3/3） | 不是全套 A–K 全绿 |
| 待验 | P3b 的 100 并发、nonce/F8 的 session/rx 应用、review 22、PREDICT 接收端、n=8/16、完整 adversarial/bench 回归 | 这些仍未完成，不能写“全绿” |

---

## A. 构建与静态检查（goal 成功标准 1–2）

### C01 `make check`

```sh
make check
```

期望：

- 退出码 0。
- `test_parse`：**365 checks, 0 failures**（goal 原文）。
- `test_fuzz`：`checks=8525966 failures=0`（P2 后）。
- `test_ack`：`10,026 checks, 0 failures`；`test_reorder`：`51,831 checks, 0 failures`。
- `test_http`：`249 checks, 0 failures`。
- h11 交叉校验打印 accepted，不出现 skipped。

当前状态：**部分/待复跑归档**。P2 host 结果：`test_ack 10,026`、`test_fuzz 8,525,966`、`test_reorder 51,831`。`test_parse` 的 314/365 口径冲突仍未解决；旧 314/0 不能代表 P2 后的全量。发布前必须二选一：恢复 365，或更新 goal/CHANGELOG/README 并写明减少项。

证据：`review/baseline/02-make-check.txt`（旧树 365/0）；P2 host 输出（test_ack/test_fuzz/test_reorder）；`CHANGELOG.md` Verification。

### C02 `make host-test-san`

```sh
make host-test-san
```

期望：退出码 0；`test_parse`、`test_fuzz`、`test_ack`、`test_reorder`、`test_http`、`test_http_fuzz` 全部 `failures=0`；ASan/UBSan 无报告。

当前状态：**待复跑归档**。P2 host 已报告 test_ack 10,026、test_fuzz 8,525,966、test_reorder 51,831；ASan 全套尚未在新树上复跑，不能用旧 314/0 代替。

证据：`review/baseline/03-host-test-san.txt`（旧树）；`CHANGELOG.md` Verification。

### C03 `review/lint.sh`

```sh
bash review/lint.sh
```

期望：`15/15 checks passed, 0 failed`；`12/12 self-tests passed, 0 failed`；退出码 0。

当前状态：**PASS**（旧树与当前树记录一致）。

证据：`review/baseline/04-lint.txt`。

### C04 发布候选的构建可复现性

```sh
sha256sum tbtun.ko
modinfo tbtun.ko | grep -E 'vermagic|version'
```

期望：`vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload`；版本在 C39 完成后为 `0.0.1`；同一源码两次构建的 sha 一致（或差异被解释为构建时间戳）。

当前状态：**未跑**（当前树 11:20 构建，未记录与源码哈希的对应关系）。

证据：`review/baseline/01-env-sha.txt`（旧树）。

---

## B. QEMU 探针 A–K（goal 成功标准 3）

### C05 全套 A–K

```sh
tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 480
```

发布门禁**不要**加 `--allow-skip`：要求 `11/11 PASS、0 skip`，退出码 0。

当前状态：**未跑全套**。当前树已报告 A、B、E、G、I、J、K PASS；C、D、F、H 未在新树复跑。旧基线是 A–H PASS、I FAIL、J/K 未执行。

证据：`review/baseline/05-qemu-probes.txt`、`05-qemu-console.log`（旧树）；`CHANGELOG.md` Verification（当前树部分探针）。

### C06 探针 I（配置已变更）

```sh
tests/qemu/run.sh --probes I --timeout 300
```

配置（`tests/qemu/init`）：

- control：`n=1`、4 MiB、`profile=raw`、`rx_reorder_bytes=2097152 hole_budget_ms=5000`。
- main：`n=4`、40 MiB、同一组 reorder/hole 预算。
- 整形：每个源端口 10 Mbit htb + `netem delay 20ms 10ms`。

期望：

- control `rc=0 strict=1`，单向速率 `<= 8 Mbit/s`（`I_CONTROL_MAX` 默认 8，不是旧 README 的 4）。
- main `rc=0 strict=1`、interleaved 也通过；单向 `>= 6 Mbit/s` 且 `>= 1.8x` control。
- 两次 `TBT_PROBE_RESULT I PASS`，无 skip。

当前状态：**PASS（修复者报告，待归档）**。当前树记录 control 3.7–4.2 Mbit/s、`rc=0 strict=1`；n=4 的 strict 与 interleaved 都逐字节通过。旧基线两次都 `rc=3 strict=0`，是修复前对照。

证据：`CHANGELOG.md` Verification；旧失败见 `review/baseline/05-qemu-console.log:864-1004`、`05b-qemu-probeI.log`。

### C07 探针 J（1% 丢包 + 2% 重排，3/3）

```sh
for i in 1 2 3; do tests/qemu/run.sh --probes J --timeout 180 || break; done
```

配置：`tc_setup delay 20ms 10ms loss 1% reorder 2%`；`n=4 m=1 dup=0`；4 MiB（`C_MIB=4`）；far/near 都用 `profile=raw`。

期望：**连续 3 次** `TBT_PROBE_RESULT J PASS`；两个阶段都逐字节通过；不要求 `holes_repaired>0`（netem 丢包由载体 TCP 自己修复，隧道不应误报空洞）。

当前状态：**PASS 3/3（修复者报告，待归档）**。

证据：`CHANGELOG.md` Verification；旧基线 J 未执行。

### C08 探针 K（载体黑洞 2 秒，3/3）

```sh
for i in 1 2 3; do tests/qemu/run.sh --probes K --timeout 180 || break; done
```

配置：`n=4 m=1`；4 MiB；传输开始后把 carrier 0 的 htb 子类改为 `netem ... loss 100%`，2 秒后恢复为 `loss 0%`。

期望：**连续 3 次** PASS；`rc=0` 且逐字节通过；`holes_repaired` 断言已移除（原因同 J）。

当前状态：**PASS 3/3（修复者报告，待归档）**。

证据：`CHANGELOG.md` Verification；旧基线 K 未执行。

### C09 替换/退休锁序：E 与 K 连续 3 次

```sh
for i in 1 2 3; do tests/qemu/run.sh --probes E,K --timeout 240 || break; done
```

期望：3/3 全 PASS；替换风暴计数 `carrier_replaced`、`join_dup` 与基线同量级（不出现每会话数十次替换）。

当前状态：**部分**。K 已 3/3；E 只有单次通过记录，没有 3/3 归档。

证据：`CHANGELOG.md` Verification；`review/baseline/05-qemu-console.log` 的 E 段（旧树）。

### C10 新树复跑 C、D、F、H

```sh
tests/qemu/run.sh --probes C,D,F,H --timeout 300
```

期望：4/4 PASS；C 的 `carrier_lost`/`carrier_replaced` 非零且 strict 通过；D 的 `drop_ppm=20000` 下 strict 通过；F 的 `dup_same_path=0`；H 的有界重置时间可解释。

当前状态：**未跑**（旧树全 PASS，但当前树没有输出）。

证据：旧树 `review/baseline/05-qemu-console.log:523-862`。

---

## C. adversarial 全套（goal 成功标准 4）

### C11 全套入口

```sh
bash adversarial/run.sh
```

发布门禁要求：退出码 0；`0 failed, 0 skipped`；每个组都出现 `PHASE_END` 与 `TBT_ADV_DONE`；没有 `console.hang.*`、`console.klog.*`。

当前状态：**未跑全套**。旧基线是 53 PASS / 20 FAIL / 0 SKIP。修复者报告 P0 rmmod 与 P3 部分已通过，但没有当前树的全套结果。

证据：`adversarial/results.txt`、`adversarial/console-*.log`；旧结果归档在 `review/baseline/06-adversarial-results.txt`。

### C12 public 组

```sh
TBT_ADV_GROUPS=public bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.echo_sane` | PASS，`ECHO_OK bytes=2097152` | 待复跑（旧树 PASS） |
| `pub.churn` | PASS，10000 次 connect/send/close 全部 `usable` | **部分：9896/8/96**（约 1% 读超时）；多流 retx/FBK 遍历正在修 |
| `pub.two` | **新契约**：A 与 B 两条并发连接都可用，A 仍可用 | **PASS（新契约）**：两条并发正确；100 并发仍未验 |
| `pub.halfclose` | PASS，`EOF=1` | **PASS**（当前 public 组已验证） |
| `pub.rst` | PASS，无 warning、模块仍加载 | **PASS**（当前 public 组已验证） |
| `init.src_key` | PASS：`src=` 被拒绝，且文档不再声称支持 | **测试需改**。`adversarial/init` 的两个分支都 `res ... FAIL`，当前实现无论怎样都 FAIL |
| `pub.srcfilter` | PASS：来自 127.0.0.2 的连接被拒，或明确记录“不支持 src 过滤” | **测试/契约需改**（旧树 accepted=1 → FAIL） |
| `pub.writer`（100 MiB） | PASS：完成写入，或按可解释流控拒绝且 `MemAvailable > 200000` | **PASS**（当前 public 组已验证） |
| `pub.writer`（1024 MiB） | 同上 | **PASS/待归档**（当前 public 组已验证 writer；原始输出待归档） |

证据：当前 public 组输出（待归档）；旧树 `adversarial/console-public.log:405-413` 仅作对照。`pub.two` 必须用新契约断言。

### C13 control 组

```sh
TBT_ADV_GROUPS=control bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `ctl.nonroot` | PASS，uid 1000 打开 `config` 失败 | 待复跑（旧树 PASS） |
| `init.nonroot_unchanged` | PASS | 待复跑 |
| `ctl.line513` | PASS，513 字节行被拒 | 待复跑 |
| `ctl.line512` | PASS，512 字节行被拒 | 待复跑 |
| `ctl.line512nonl` | PASS，无换行不生效 | 待复跑 |
| `ctl.line1025` | PASS，errno 7 | 待复跑 |
| `ctl.line1024nonl` | PASS，errno 7 | 待复跑 |
| `init.cfg_unchanged` | PASS | 待复跑 |
| `init.cfg_roundtrip` | PASS，canonical config 逐行重放 | 待复跑 |
| `init.echo_after_roundtrip` | PASS，`ECHO_OK bytes=1048576` | 待复跑 |

证据：旧树 `adversarial/console-control.log`；归档 `review/baseline/06-adversarial-results.txt`。

### C14 resource 组

```sh
TBT_ADV_GROUPS=resource bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.max_tunnels` | PASS：80 条写入中，超过 64 的隧道被 `ENOSPC` 拒绝 | **代码已修，待复跑归档**（旧树 80 条全接受） |
| `init.scale` | PASS：1024 个载体 `up` | 待复跑（旧树 PASS） |

证据：`CHANGELOG.md` Added/Limits；旧树 `adversarial/console-resource.log`。

### C15 slab 组

```sh
TBT_ADV_GROUPS=slab bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.slab_leak` | PASS：100 条垃圾连接后 32k slab 增长 < 50 | 待复跑（旧树 0） |
| `init.slab_after_flush` | PASS：flush 后 32k 对象回落到基线 | 待复跑（旧树 0→0） |

证据：旧树 `adversarial/console-slab.log`。

### C16 final 组

```sh
TBT_ADV_GROUPS=final bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.final_echo` | PASS，`ECHO_OK bytes=4194304` | 待复跑（旧树 PASS） |
| `init.dmesg` | PASS：guest dmesg 中 0 条 WARNING/BUG/Oops/INFO: task/deprecated workqueue | **待修**：`tbt_fbk_work` 仍入队到 deprecated workqueue |
| `console.klog.final` | PASS：console 无同类行 | **待修**（同上） |

证据：旧树 `adversarial/console-final.log:407`；`review/baseline/06-adversarial-klog.txt`。

### C17 replay 组

```sh
TBT_ADV_GROUPS=replay bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.cfg_replay` | PASS：传输中逐行重放 config，应用干净关闭 | **待修**（旧树 ECHO_TIMEOUT） |
| `init.echo_after_replay` | PASS | 待复跑（旧树 PASS） |

证据：旧树 `adversarial/console-replay.log`。

### C18 rmmod 组

```sh
TBT_ADV_GROUPS=rmmod bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.rmmod` | PASS：`rmmod rc=0`、应用干净退出 | **PASS（修复者报告，待复跑归档）**；旧树是 int3 oops + panic |
| `init.reload` | PASS：rmmod 后 `insmod` 成功、`/proc/net/tbtun/config` 存在 | 待复跑归档 |
| `init.dmesg_after_rmmod` | PASS：0 条 warning/oops | 待复跑归档 |
| `console.hang.rmmod` | **不得出现** | 旧树出现；修复后应消失 |
| `console.klog.rmmod` | **不得出现** | 旧树出现；修复后应消失 |

证据：旧树 `adversarial/console-rmmod.log:405-500`；修复说明见 `CHANGELOG.md` 的 Fixed “Module unload”。

### C19 flush 组

```sh
TBT_ADV_GROUPS=flush bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.flush_midstream` | PASS：传输中 flush 后应用干净关闭 | **待修**（旧树 ECHO_TIMEOUT） |
| `init.echo_after_flush` | PASS | 待复跑（旧树 PASS） |

证据：旧树 `adversarial/console-flush.log`。

### C20 catconfig 组

```sh
TBT_ADV_GROUPS=catconfig bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `cat.cfg_self` | PASS：`cat config > config` 返回 0，两条隧道仍在 | 待复跑（旧树 PASS） |
| `init.echo_after_cat` | PASS | 待复跑（旧树 PASS） |

证据：旧树 `adversarial/console-catconfig.log`。

### C21 cfg_mix 组

```sh
TBT_ADV_GROUPS=cfg_mix bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `cat.cfg_mix` | PASS：坏行不生效、后续 far 行生效 | 待复跑（旧树 PASS） |

证据：旧树 `adversarial/console-cfg_mix.log`。

### C22 param 组

```sh
TBT_ADV_GROUPS=param bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.max_carriers` | PASS：`max_carriers=4` 下 `n=8` 写入被拒 | **代码已修，待复跑归档**（旧树 n=8 被接受） |

证据：`CHANGELOG.md` Added/Limits；旧树 `adversarial/console-param.log`。

### C23 port 组

```sh
TBT_ADV_GROUPS=port bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `init.port_exhaust` | PASS：端口段耗尽时 `alloc_fail=0` 或第三条隧道被明确拒绝，并给出操作员可见错误 | **待修**（旧树 tunnel 3 被接受，`alloc_fail=16`，载体无限重试） |

证据：旧树 `adversarial/console-port.log`；`review/baseline/06-adversarial-results.txt`。

### C24 near 组

```sh
TBT_ADV_GROUPS=near bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `near.resp_not_chunked` | PASS：载体被毒化 | 待复跑（旧树 PASS） |
| `near.resp_500` | PASS | 待复跑 |
| `near.resp_hdr_big` | PASS | 待复跑 |
| `near.resp_close` | PASS | 待复跑 |
| `near.hello_wrong_nonce` | PASS | 待复跑 |
| `near.chunk_huge` | PASS | 待复跑 |
| `near.split_record` | PASS | 待复跑 |
| `near.chunk_oversize` | PASS | 待复跑 |
| `near.drip_pad` | PASS | 待复跑 |
| `near.fbk_forged` | PASS：uack 保持 5000 | 待复跑 |
| `near.data_path` | PASS：伪造 FBK 后 DATA 仍投递 | 待复跑 |
| `near.second_conn` | PASS：第一条关闭后第二条连接 `rc=0` 且可用 | **PASS/待归档**（`pub.two` 新契约两条并发正确；100 并发未验） |

证据：旧树 `adversarial/console-near.log`。

### C25 far 组

```sh
TBT_ADV_GROUPS=far bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `far.garbage` | PASS | 待复跑（旧树 PASS） |
| `far.hello_garbage` | PASS | 待复跑 |
| `far.wrong_nonce` | PASS | 待复跑 |
| `far.anchor` | PASS：合法载体加入 index 0 | 待复跑 |
| `far.dup_index` | PASS：重复 `(nonce,index)` 被拒，anchor 存活 | 待复跑 |
| `far.srcaddr_dup` | PASS | 待复跑 |
| `far.srcaddr_free` | PASS：来自 127.0.0.2 的 nonce 重放被拒 | **未做**（旧树 ACCEPTED，nonce/源绑定未落地） |
| `far.chunk_huge` | PASS | 待复跑 |
| `far.bodylen_tail` | PASS | 待复跑 |
| `far.split_record` | PASS | 待复跑 |
| `far.drip_split` | PASS | 待复跑 |
| `far.chunk_oversize` | PASS | 待复跑 |
| `far.no_host` | PASS | 待复跑 |
| `far.headers_big` | PASS | 待复跑 |
| `far.open_other_sid` | PASS | 待复跑 |
| `far.fbk_forged` | PASS：uack 保持 5000 | 待复跑 |
| `far.hello_profile0` | PASS：profile 0 被拒 | 待复跑 |
| `far.hello_profile9` | PASS：HELLO 字节 9 被解释为 generation，不是 profile | 待复跑（旧树 PASS，detail 已说明） |
| `far.data_path` | PASS：anchor echo ping/pong 都收到，`rx_records>0` | **待复跑确认**；旧树 `rx_records=0`、`car_state=7`、`lost=3`、`join_dup=2` |
| `console.klog.far` | **不得出现** | 旧树出现 deprecated workqueue；待 F7 修复 |

证据：旧树 `adversarial/console-far.log:411-445`。

### C26 stall 组

```sh
TBT_ADV_GROUPS=stall bash adversarial/run.sh --no-build
```

| ADV_RESULT | 期望 | 当前状态 |
|---|---|---|
| `stall.hello_chain` | PASS：15 次顺序 HELLO 全部应答，`rx_adopt_events=15` | 待复跑（旧树 PASS） |
| `stall.flush` | PASS：载体链之后 flush 完成 | 待复跑（旧树 PASS） |
| `console.klog.stall` | **不得出现** | 旧树出现 deprecated workqueue；待 F7 修复 |

证据：旧树 `adversarial/console-stall.log:408`。

### C27 close-flush 崩溃复现

```sh
bash adversarial/crash-repro.sh
```

期望：恰好一行 `ADV_RESULT ctl.close_flush PASS`；console 出现 `REPRO_SURVIVED` 与 `TBT_REPRO_DONE`；无 oops。

当前状态：**PASS**（旧树与当前树都通过）。

证据：`adversarial/crash-console.log:402-410`；旧归档 `review/baseline/06-adversarial-results.txt`。

---

## D. bench（goal 成功标准 5）

### C28 n=4

```sh
benches/run.sh throughput --n 4 --mib 40
```

期望（goal 原文）：`rc=0`、`strict=1`、app `>= 9.20 Mbit/s`、`retx_ppm < 50k`、`wire/app < 1.1`。
README/CHANGELOG 把当前目标放宽为 `retx_ppm < 100k`、`wire/app < 1.2`；发布门禁必须明确采用哪一组。

当前状态：**部分达标**。

| 指标 | 当前值（CSV `20261105T112245`） | goal 标准 | README 目标 | 判定 |
|---|---|---|---|---|
| rc / strict | 0 / 1 | 0 / 1 | 同 | PASS |
| app | 11.39 Mbit/s | >= 9.20 | 同 | PASS |
| control | 2.24 Mbit/s | — | — | 记录 |
| app/control | 5.08x | — | — | 记录 |
| `wire/app` | 1.0005 | < 1.1 | < 1.2 | PASS |
| `retx_ppm` | 96,095 | **< 50k** | < 100k | **按 goal FAIL；按 README PASS** |
| `txq_max` | 98 KB | 有界 | — | PASS |

证据：`benches/results/20261105T112245-throughput.csv`；`CHANGELOG.md` Verification；README Verified performance。

### C29 n=1

```sh
benches/run.sh throughput --n 1 --mib 40
```

期望（goal P2-11）：`rc=0`、`strict=1`；README 目标 `wire/app < 1.2`、`retx_ppm < 100k`；app 不低于单流 control 的合理范围。

当前状态：**PASS**。当前值：control 2.48、app 3.46 Mbit/s、1.40x、`wire/app 1.0316`、`retx_ppm 1,090`、`rc=0 strict=1`。

证据：`benches/results/20261105T112725-throughput.csv`；README Verified performance。

### C30 n=8

```sh
benches/run.sh throughput --n 8 --mib 40
```

期望：`rc=0 strict=1`；app 明显高于 n=4（历史 18.47 Mbit/s、7.08x）；`wire/app < 1.2`；`retx_ppm < 100k`（或 goal 的 50k）。

当前状态：**未跑**（README 明确 pending final regression）。

证据：历史 `benches/results/20261105T045445-throughput-rec.csv`；当前无 CSV。

### C31 n=16

```sh
benches/run.sh throughput --n 16 --mib 40
```

期望：`rc=0 strict=1`；`carriers_up=16/16`、`alloc_fail=0`、`connect_fail=0`；app 高于 n=8（历史 34.18 Mbit/s、12.5x）。

当前状态：**未跑**。

证据：历史 `benches/results/` 的 scale 行；当前无 CSV。

---

## E. 稳定性与资源（goal 成功标准 6–7）

### C32 flush 在 1 秒内返回

```sh
TBT_ADV_GROUPS=flush bash adversarial/run.sh --no-build
benches/run.sh flush-hang
```

期望：`init.flush_midstream` PASS；`stall.flush` PASS；bench 的 `flush_s < 1.0` 且 `hung=0`；两次 40 MiB 传输后 `echo flush > /proc/net/tbtun/config` 在 1 s 内返回。

当前状态：**未验证**。README/CHANGELOG 仍把 “flush can block after a transfer” 列为已知限制。

证据：旧树 `adversarial/console-flush.log`；`benches/results/*-flush-hang-*.csv`（旧树 `flush_s` 与 `hung` 列）。

### C33 100 MiB 写入不阻塞

```sh
TBT_ADV_GROUPS=public bash adversarial/run.sh --no-build
```

期望：`pub.writer`（100 MiB）PASS：完成写入，或按可解释流控拒绝且 `MemAvailable > 200000`；不再出现旧树的 412,508 → 133,980 kB。

当前状态：**未做**（`st-backpressure` pending）。

证据：旧树 `adversarial/console-public.log:412`。

### C34 1 GiB 写入不阻塞

同上，`pub.writer`（1024 MiB）期望相同。

当前状态：**未做**（旧树 411,312 → 180,312 kB）。

证据：旧树 `adversarial/console-public.log:413`。

### C35 review 20：重排帧淘汰

```sh
# 在 guest 内构造重叠重排帧后读取
cat /proc/net/tbtun/stats | grep -E 'rx_ooo_bytes|reorder'
```

期望：`rx_ooo_bytes` 不随重叠帧单调增长；超过 `rx_reorder_bytes` 前有淘汰；有界重置计数可解释。

当前状态：**未做**（CHANGELOG Known limitations 明确 review 20 pending）。

证据：`CHANGELOG.md` Known limitations；`review/findings.md` 的 review 20 行。

### C36 review 22：`-EAGAIN` 下控制帧不丢

```sh
# 在载体背压下注入 FIN/RST，观察对端是否收到
TBT_ADV_GROUPS=public bash adversarial/run.sh --no-build
```

期望：pinned 控制帧（FIN 等）在 `-EAGAIN` 下不丢；`pub.rst` 与 halfclose 路径均无丢帧。

当前状态：**未做**（review 22 pending）。

证据：`review/findings.md` 的 review 22 行；`CHANGELOG.md` Known limitations。

### C37 端口耗尽有操作员可见错误

```sh
TBT_ADV_GROUPS=port bash adversarial/run.sh --no-build
```

期望：`init.port_exhaust` PASS：`alloc_fail=0` 或第三条隧道被明确拒绝，且 `/proc/net/tbtun/stats` 有可见错误。

当前状态：**待修**（旧树 FAIL）。

证据：旧树 `adversarial/console-port.log`。

### C38 零值预算配置被拒

```sh
# 期望 write 返回 EINVAL，而不是接受后隧道失效
echo 'tunnel 9 role=near profile=raw listen=127.0.0.1:19099 peer=127.0.0.1:19101 n=1 rx_reorder_bytes=0' > /proc/net/tbtun/config
echo 'tunnel 9 role=near profile=raw listen=127.0.0.1:19099 peer=127.0.0.1:19101 n=1 hole_budget_ms=0' > /proc/net/tbtun/config
```

期望：两条写入都失败（`EINVAL`）；配置不变。

当前状态：**未验证**（review 23；旧基线未单测）。

证据：`review/findings.md` 的 review 23 行。

### C39 计数器在多载体下不丢更新

```sh
# 跑 n=16 传输后对比 stats 与 peers 的帧/字节汇总
cat /proc/net/tbtun/stats
cat /proc/net/tbtun/peers
```

期望：`rx_frames`/`data_frames` 等为原子或加锁更新；汇总与每载体之和一致（允许统计窗口误差）。

当前状态：**未验证**（review 21）。

证据：`review/findings.md` 的 review 21 行。

---

## F. 版本与 DKMS（goal 成功标准 8）

### C40 版本统一为 0.0.1

```sh
grep -nE 'TBT_VERSION_(MAJOR|MINOR|PATCH)' tbtun/tbt_uapi.h
./scripts/mkdkmsconf.sh | grep PACKAGE_VERSION
grep -n '0.0.1\|1.0.0' README.md README.zh.md DESIGN.md 交接.md
```

期望：`TBT_VERSION_MAJOR/MINOR/PATCH = 0/0/1`；`PACKAGE_VERSION="0.0.1"`（发布 tag 上不带 `.rN.hash`）；README/README.zh 出现 0.0.1；DESIGN/交接 不再声称当前是 1.0.0。

当前状态：**PASS（版本/文档），tarball 待生成**。`tbt_uapi.h` 为 0/0/1，`TBT_WIRE_VERSION=1`，`TBT_CONV=0x0004`；`dkms.conf` 为 `PACKAGE_VERSION="0.0.1"`；`dkms.tar.gz` 仍是旧版本，待最终回归后 `make dkms-tarball`。

证据：`tbtun/tbt_uapi.h:30-49`、`dkms.conf:2`、`CHANGELOG.md` Packaging。

### C41 CHANGELOG 存在且与实测一致

```sh
sed -n '1,60p' CHANGELOG.md
grep -n '314\|365\|96,095\|11.39\|3.46' CHANGELOG.md README.md
```

期望：存在 `## [0.0.1]`；包含 Added/Changed/Fixed/Removed/Known limitations/Packaging/Verification；Verification 的数字与 CSV 一致；发布时把 `(Unreleased)` 改为发布日期。

当前状态：**部分**。CHANGELOG 已标 0.0.1 release candidate，Packaging 已写 0.0.1；Verification 仍明确不是全套全绿，host 数字已改为 P2 的 test_ack/test_fuzz/test_reorder。发布前仍需最终回归归档。

证据：`CHANGELOG.md`。

### C42 `modinfo -F version`

```sh
modinfo -F version tbtun.ko
```

期望：`0.0.1`。

当前状态：**PASS**（`modinfo -F version tbtun.ko` 报 `0.0.1`）。

证据：当前 `tbtun.ko`；源码 `tbt_uapi.h:30-32`。

### C43 `modinfo -p` 参数表

```sh
modinfo -p tbtun.ko
```

期望恰好 4 个参数：`debug`、`max_tunnels`、`max_carriers`、`debug_loss`；无 `netns_optin`。

当前状态：**PASS**（`modinfo -p tbtun.ko` 只有 `debug`、`max_tunnels`、`max_carriers`、`debug_loss`；无 `netns_optin`）。

证据：`tbtun/tbt_core.c:27-44`。

### C44 DKMS dry-run

```sh
./scripts/install_dkms.sh --dry-run
```

期望：退出码 0；打印安装步骤；不写 `/usr/src`；不需要 root。

当前状态：**PASS**（`--dry-run --no-load` 打印 `Installing tbtun 0.0.1`，stage `/usr/src/tbtun-0.0.1`）。

证据：当前 `install_dkms.sh` dry-run 输出。

### C45 DKMS 安装并加载

```sh
sudo ./scripts/install_dkms.sh
modinfo -F version tbtun
sudo modprobe tbtun
lsmod | grep '^tbtun'
cat /proc/net/tbtun/tunnels
```

期望：`dkms add/build/install` 全部成功；`modinfo` 报 0.0.1；`modprobe` 后 `/proc/net/tbtun/*` 出现；`dmesg` 无 oops。

当前状态：**未跑**。注意本机没有标准内核头文件（`docs/build-env.md`），DKMS 需要 `/lib/modules/$(uname -r)/build`；要么在干净内核上跑，要么用 `--kernel` 指定一个有头文件的 release。

证据：`docs/build-env.md` 的 Header coverage / Root；`scripts/install_dkms.sh`。

### C46 DKMS 卸载

```sh
sudo rmmod tbtun
sudo ./scripts/install_dkms.sh --uninstall
modinfo tbtun          # 期望失败
ls /usr/src/tbtun-*    # 期望无残留
```

期望：卸载返回 0；模块不在 `lsmod`；`modinfo` 找不到；`/usr/src` 无 `tbtun-*`。

当前状态：**未跑**。

证据：`scripts/install_dkms.sh` 的 `--uninstall` 分支。

### C47 干净内核验证

期望：在未安装本仓库、未加载旧模块的干净内核上完成 C45/C46；记录内核 release 与 `vermagic`。

当前状态：**未跑**。

---

## G. 文档一致性（goal 成功标准 9）

### C48 README / README.zh

```sh
grep -nE '0\.0\.1|1\.0\.0|Verified performance|Known limitations' README.md README.zh.md
```

期望：版本 0.0.1；性能表只引用当前 CSV；`n=8/16` 明确 pending；flush、多流、halfclose、PREDICT、review 20/22 的限制与代码一致。

当前状态：**部分**。README/README.zh 的 Packaging 已写 0.0.1、性能表已用当前 CSV、`n=8/16` 标 pending；但 README 没有多流/churn/halfclose 的当前状态，需要在 Known limitations 或 Scope 补一段：两条并发 PASS、`pub.churn` 9896/8/96（约 1% 读超时）、100 并发未验、`pub.halfclose` PASS。本轮只改清单与 CHANGELOG Verification，README 待同步。

证据：`README.md:57-70`、`:213-270`；`README.zh.md:45-58`、`:194-240`。

### C49 DESIGN.md

```sh
grep -nE 'current tree reports|not implemented|1\.0\.0|0\.0\.1|TBT_WIRE_VERSION' DESIGN.md
```

期望：版本段不再写 “current tree reports 1.0.0 / not implemented”；`TBT_CONV=0x0004` 与代码一致；SO_REUSEADDR、HELLO 位布局、`srcaddrs` 语义与代码一致。

当前状态：**PASS（版本/wire）**。DESIGN §11 已写当前 0.0.1 release candidate、`TBT_WIRE_VERSION=1`、`TBT_CONV=0x0004`，Packaging 写 `dkms.conf` 0.0.1；其余漂移（review 15/20/22、n=8/16）仍按各自条目跟踪。

证据：`DESIGN.md:1804-1820`。

### C50 交接.md

```sh
grep -nE '§2|11/11|9\.20|3\.52|n=1|retx_ppm|已知全绿' 交接.md
```

期望：§2 状态表、§7 性能表、§8 缺陷表、§9 未完成清单都刷新到 0.0.1 最终状态；不再声称 “benches 全部行 strict=1” 或 “最近一次全绿 11/11” 而没有新证据。

当前状态：**FAIL**。交接仍停留在修复前快照；`review/baseline.md` 已记录其与实测的差异。

证据：`交接.md` §2/§7/§8/§9；`review/baseline.md` §6。

### C51 review/findings.md 状态表

```sh
grep -nE '^\| (1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39) ' review/findings.md
```

期望：每条 finding 的 Status 与当前代码一致；review 20/22 若未修必须仍标 open，不能提前标 fixed；行号按当前代码刷新。

当前状态：**待刷新**（状态表是 05:19/05:27 快照，代码已多次变动）。

证据：`review/findings.md:1-60`。

### C52 验收标准本身一致

```sh
grep -nE '365|314|50k|100k|1\.1|1\.2' .pi/goals/*.md README.md CHANGELOG.md
```

期望：同一组数字只出现一种口径。当前有三处冲突：

- `make check`：goal 365 vs 当前 `test_parse` 最后记录 314（P2 新增 test_ack 10,026 / test_reorder 51,831 未解决总数口径）。
- `retx_ppm`：goal `< 50k` vs README/CHANGELOG `< 100k`。
- `wire/app`：goal `< 1.1` vs README/CHANGELOG `< 1.2`。

当前状态：**FAIL（文档冲突）**。发布前必须选定口径并同步三处。

证据：`.pi/goals/active_goal_2026110507234084_mw2qtfvz-pyz8w0.md` 成功标准 1/5；`README.md:213-240`；`CHANGELOG.md` Verification。

---

## H. 已知限制与发布边界

### C53 多流（P3a/P3b1 部分完成）

状态：**部分完成（P3a/P3b1）**。每流 `target` 已完成；`pub.two` 新契约两条并发 PASS；`pub.churn` 9896/8/96（约 1% 读超时），100 并发未验。发布声明必须写清“两条并发可用、churn 约 1% 读超时、100 并发未验”，不能写单流或全多流。

证据：`CHANGELOG.md` Known limitations；`adversarial/console-public.log:406`；`adversarial/console-near.log`。

### C54 half-close EOF（当前 public 组已通过）

状态：**PASS（当前 public 组）**。`pub.halfclose` 已验证 `EOF=1`；不再作为已知限制。

证据：当前 public 组 `pub.halfclose EOF=1`（待归档）；旧树 `adversarial/console-public.log:408` 仅作对照。

### C55 nonce 抢占 / 源绑定

状态：**未完成**。nonce/F8 的 session/rx 应用仍在待办；`far.srcaddr_free` 的旧树 FAIL 仍作为风险记录。

证据：旧树 `adversarial/console-far.log` 的 `far.srcaddr_free`。

### C56 PREDICT 接收端惰性

状态：**未完成**。PREDICT 接收端仍是惰性；发布声明必须写“PREDICT 只是发送端占位，实际恢复靠立即重传与重放环”，或按 review 13 删除占位路径与计数器。

证据：旧树 `review/baseline/05-qemu-console.log` 的 `c6` 行；`CHANGELOG.md` Known limitations。

### C57 review 20 / 22

状态：**review 20/22 仍未完成**（见 C35/C36）。发布声明必须写“重叠重排帧不淘汰”“`-EAGAIN` 下控制帧可能丢”，或修复后关闭。

证据：`CHANGELOG.md` Known limitations；`review/findings.md`。

### C58 IPv6 与加密 out of scope

状态：**明确 out of scope**。README/DESIGN/CHANGELOG 必须一致：只接受字面 IPv4；隧道不加密，需要保密时套 WireGuard。

证据：`CHANGELOG.md` Known limitations；README Security/Scope。

### C59 单 `srcaddrs` / init_net-only

状态：**明确限制**。只使用 `srcaddrs` 第一个地址；`/proc/net/tbtun` 只在 `init_net` 创建；`netns_optin` 已删除。

证据：`CHANGELOG.md` Removed/Known limitations；`tbtun/tbt_core.c`。

### C60 发布声明

期望：`CHANGELOG.md` 的 0.0.1 段从 `(Unreleased)` 改为发布日期；列出全部已知限制；引用最终回归的 CSV 与 console 路径；声明“实验室数字，不是 ISP 实测”。

当前状态：**部分**。标题已是 `0.0.1 (Release candidate)`，Packaging 已写 0.0.1；最终回归未完成，尚未改成正式发布日期，Verification 也未宣称全绿。

---

## I. 发布门禁汇总

| 门禁 | 条目 | 当前状态 | 是否阻塞 0.0.1 |
|---|---|---|---|
| 构建与静态检查 | C01–C04 | C03 PASS；P2 host 已报 test_ack 10,026、test_fuzz 8,525,966、test_reorder 51,831；C01/C02 的 ASan/全量仍待归档，314/365 口径冲突未决 | C01/C02 阻塞（数字对齐后可通过） |
| QEMU 探针 | C05–C10 | 当前已验证 B、public（churn/two/halfclose/rst/writer）、I 最近一次 PASS（不要求 3/3）；A/E/G/J/K 未在本轮重验，C/D/F/H 未跑 | 阻塞（全套 11/11） |
| adversarial public | C12 | churn 9896/8/96（约 1% 读超时）；two/halfclose/rst/writer PASS；src_key/srcfilter 测试/契约待改 | churn 与 src 组阻塞 |
| adversarial control/resource/slab/catconfig/cfg_mix/param/stall/crash | C13/C14/C15/C20/C21/C22/C26/C27 | 旧树全 PASS；max_* 待复跑 | 不阻塞（复跑归档即可） |
| adversarial final/rmmod/replay/flush/port | C16/C17/C18/C19/C23 | rmmod 已修；final/replay/flush/port 未过 | 阻塞 |
| adversarial near/far | C24/C25 | second_conn 两条并发 PASS/待归档；srcaddr_free/data_path 未过 | srcaddr_free/data_path 阻塞 |
| bench | C28–C31 | n=1/n=4 当前 CSV：n=4 11.39 Mbit/s、wire/app 1.0005、retx 96,095；n=1 3.46、1.0316、1,090；n=8/16 未跑 | n=8/16 阻塞；n=4 已进入 100k/1.2 目标 |
| 稳定性/资源 | C32–C39 | 全部未验证或未做 | 阻塞（goal 6–7） |
| 版本/DKMS | C40–C47 | 版本 0.0.1、wire 1/conv 0x0004、dkms.conf 0.0.1、modinfo 0.0.1、dry-run 0.0.1；dkms.tar.gz 旧；DKMS 安装/卸载未跑 | 仅 tarball/安装卸载待验 |
| 文档 | C48–C52 | README 版本/性能已更新，但缺少多流/churn/halfclose 当前状态；DESIGN 版本段 PASS；交接/findings 仍部分旧 | README 与交接待同步 |
| 已知限制 | C53–C60 | 多流部分完成（two PASS、churn ~1% 超时）；halfclose PASS；nonce/PREDICT/review20/22 未完成 | P3b/nonce/PREDICT/review 阻塞 |

**结论**：P2 与 P3a 已完成，P3b1 两条并发可用但 churn 仍有约 1% 读超时；版本已统一 0.0.1（除 dkms.tar.gz）。0.0.1 仍不能发布。最先要清的三组是：

1. **多流与 churn**（C12/C24/C53）：`pub.two` PASS，`pub.churn` 9896/8/96；先修多流 retx/FBK 遍历，再验 100 并发。
2. **剩余 adversarial/探针**（C12/C16/C19/C23/C25）：src_key/srcfilter、final/replay/flush/port、far.srcaddr_free/data_path。
3. **版本/DKMS 收尾**（C40–C47）：dkms.tar.gz 重新生成、安装/卸载复跑。

多流（C53）仍是最大功能缺口：two 已通、churn 约 1% 超时、100 并发未验。nonce/PREDICT/review 20/22 必须要么修，要么在 CHANGELOG 与 README 里以同样的措辞声明为已知限制。
