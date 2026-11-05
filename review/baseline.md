# tbtun 基线冻结 — 2026-11-05

只测量，未修改 `tbtun/`、`host/`、`tests/`、`benches/` 的源文件。
模块全程未重建：`make check` 命中已有对象，`tbtun.ko` 的 sha256 在第一条命令和最后一条命令时相同。

唯一写入：

- `review/baseline/*`（每条命令的原始 stdout+stderr、console、CSV）
- `review/baseline.md`（本文）
- 脚本自身的运行产物：`adversarial/results.txt`、`adversarial/console-*.log`、`adversarial/crash-console.log`、`benches/results/20261105T07*.csv`、`~/.cache/tbtun-build/bench/console-*.log`

所有 QEMU 运行串行，未与其它 guest 并发；两次 bench 的 `host_qemu_others=0`。

---

## 1. 环境与 sha

来源：`review/baseline/01-env-sha.txt`、`00-prequeue-state.txt`、`07-bench-preconditions.txt`。

| 项 | 值 | 来源 |
|---|---|---|
| 测量开始 | 2026-11-05T07:24:12+08:00 | 实测 |
| `git rev-parse HEAD` | `ee0e5e632c90704c77a4364182e185fe0aed61e1` | 实测 |
| `git status --porcelain` | 大量未提交/未跟踪：`tbtun/`、`host/`、`tests/`、`benches/`、`review/`、`adversarial/`、`docs/`、`DESIGN.md` 均 untracked；`brutal*.c` 等已删除但删除未提交 | 实测 |
| `uname -r` | `7.2.9-1-MANJARO` | 实测 |
| 模块大小 | 5,405,288 B | 实测 |
| 模块 sha256 | `c18ff6c480cc1d8886d09952b93f1d754adba4e10929a72a8e322ef814a056a6` | 实测 |
| 模块 mtime | 2026-11-05 06:28 | 实测 |
| `vermagic` | `7.2.9-1-MANJARO SMP preempt mod_unload` | 实测 |
| 构建环境 | `KDIR=~/.cache/tbtun-build/linux72-headers/.../build`，`PAHOLE=.../pahole` v1.32，`LD_LIBRARY_PATH` 已设 | 实测 |
| QEMU | 11.1.2，`/dev/kvm` 可写，accel=kvm | 实测 |
| 残留进程 / 端口 | 无 qemu 残留；19100/19101/29101 均无监听 | 实测 |

`make check` 没有重建模块（输出只有 5 行 make 递归 + 4 行测试），所以本次所有 QEMU/bench 数字都来自同一个 sha `c18ff6c4…`。

---

## 2. 命令结果总表

| # | 命令 | 结果 | 关键数字 | 原始输出 |
|---|---|---|---|---|
| 1 | `git rev-parse HEAD` / `git status` / `uname -r` / 模块大小 | 记录完成 | HEAD `ee0e5e6`，模块 5,405,288 B，sha `c18ff6c4…` | `01-env-sha.txt` |
| 2 | `make check` | **PASS**（RC=0，2.45 s） | 365 checks / 0 failures；`test_fuzz` 8,349,443 / 0；`test_http` 249 / 0；h11 交叉校验通过 | `02-make-check.txt` |
| 3 | `make host-test-san` | **PASS**（RC=0，7.29 s） | 365 / 0；8,349,443 / 0；249 / 0；`test_http_fuzz` 4,078,276 / 0 | `03-host-test-san.txt` |
| 4 | `bash review/lint.sh` | **PASS**（RC=0） | 15/15 checks，12/12 self-tests | `04-lint.txt` |
| 5 | `tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --allow-skip --timeout 480` | **FAIL**（RC=1，58.35 s） | A–H PASS；I FAIL；J/K 未执行；0 skip | `05-qemu-probes.txt`、`05-qemu-console.log` |
| 5b | `tests/qemu/run.sh --probes I --allow-skip --timeout 300`（定位复跑） | **FAIL**（RC=1，20.34 s），与 5 同因 | control n=1 4 MiB：7.6 Mbit/s，`rc=3 strict=0` | `05b-qemu-probeI.txt`、`05b-qemu-probeI.log` |
| 6 | `bash adversarial/run.sh --no-build --timeout 600` | **FAIL**（RC=1，208 s） | 53 PASS / 20 FAIL / 0 SKIP | `06-adversarial.txt`、`06-adversarial-results.txt`、`06-adversarial-klog.txt` |
| 7 | `benches/run.sh throughput --n 4 --mib 40` | 脚本 RC=0，但 n4 行 **FAIL** | control 2.88；tunnel 43.30；app 1.31；`wire/app=33.06`；`retx_ppm=1,171,952`；`rc=2 strict=0` | `08-bench-n4.txt`、`08-bench-n4.csv`、`08-bench-n4-console.log` |
| 8 | `benches/run.sh throughput --n 1 --mib 40` | 脚本 RC=0，但 n1 行 **FAIL** | control 2.66；tunnel 7.90；app 0.65；`wire/app=12.07`；`retx_ppm=2,352,439`；`rc=2 strict=0` | `09-bench-n1.txt`、`09-bench-n1.csv`、`09-bench-n1-console.log` |

`--no-build` 用于 adversarial：第 2 步的 `make check` 刚刚构建/确认过同一个 sha，避免再次调用 `make check` 改变基线。脚本仍然完成了它自己的 `adv_probe` 编译和 15 组 QEMU 启动。

按用户指示，n=8/16 未运行，构建环境已释放。

---

## 3. QEMU 探针（A–K）

判定行来自 `05-qemu-console.log`，逐项：

| 探针 | 结果 | 关键证据（本次实测） |
|---|---|---|
| A | **PASS** | `/proc` 语法、canonical read-back、拒绝行不改配置、`del`、`debug_loss` 门控 |
| B | **PASS** | 4 MiB 双向逐字节通过 |
| C | **PASS** | 传输中拿下 carrier 0；`carrier_lost=2 carrier_replaced=1 carrier0=up frames=268 bytes=2088035`；strict + interleaved 均通过 |
| D | **PASS** | `drop_ppm=20000` + reorder；hole declare/repair 发生，strict 通过 |
| E | **PASS** | `spare 4 took over: state=up bytes=2056287`；strict 通过 |
| F | **PASS** | `dup=2` + 注入丢包；无同路径重复 |
| G | **PASS** | `dropped=24 holes_declared=47 holes_repaired=47 holes_unrecoverable=0 rx_max_stall_us=32505 limit=500000` |
| H | **PASS** | 持续丢包后 `phase 2: reset in 0.58 s, holes_unrecoverable=1`（有界重置） |
| I | **FAIL** | `control n=1: one-way 7.7 Mbit/s, rc=3 strict=0 interleaved=-1`；`strict send-then-read failed: sent=4194304 recvd=1195555 first_mismatch=581042` |
| J | **未执行** | `probe I` 的 `fail()` 直接 `poweroff`，J/K 从未启动；`run.sh` 的 summary 只列到 I |
| K | **未执行** | 同上 |

探针 I 的失败细节（`05-qemu-console.log:864-1004`）：

- control 是 `n=1`、4 MiB、`profile=raw`、`rx_reorder_bytes=2097152 hole_budget_ms=5000`，单条 10 Mbit htb + `netem delay 20ms 10ms`。
- 只读回 1,195,555 B，`first_mismatch=581042`；两次 `TXSTALLKILL`，`age_us≈5.00 s`，即正好撞上 5 s 的 `hole_budget_ms`。
- `tunnel 1 c3 … retx_frames=8875 retx_bytes=67637762`，而 `submit_bytes=1195555`；`tunnel 2 … retx_frames=4315 retx_bytes=35292286`。
- `holes_unrecoverable=1`，`rx_max_stall_us=113203`。
- 7.7 Mbit/s 是 `peers` 的 `tx_bytes` 速率，包含重传；不是有效吞吐。

单独复跑 I（`05b-qemu-probeI.log`）复现同一模式：

- `control n=1: one-way 7.6 Mbit/s, rc=3 strict=0 interleaved=-1`
- `strict send-then-read failed: sent=4194304 recvd=1219866 first_mismatch=558069`
- `retx_frames=7989 retx_bytes=62868337`（对 1.22 MB 回程字节）
- `TXSTALLKILL … age_us=5002915`

结论：**单载体 control 在本次基线上可复现地失败**，不是一次性抖动。

`run.sh` 的 console 尾部还有 `net eth0: Unexpected TXQ (0) queue failure: -5`（关机阶段，`05-qemu-console.log:1201-1207`），不是模块 oops。

---

## 4. adversarial 全套

命令：`bash adversarial/run.sh --no-build --timeout 600`。
统计（实测）：**53 passed, 20 failed, 0 skipped**；退出码 1。

20 个 FAIL 中 5 个是 console 派生（4 条 `deprecated workqueue` + 1 条 rmmod 挂起），15 个是攻击本身的失败。

### 4.1 全部 ADV_RESULT 行（原样）

```text
ADV_RESULT init.echo_sane PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=2097152
ADV_RESULT pub.churn FAIL sha=c18ff6c480cc1d88 10000 connect/send/close cycles: usable=1 refused=9999 other=0 (a closed stream must free the session for the next connection)
ADV_RESULT pub.two PASS sha=c18ff6c480cc1d88 A usable=1, B refused=1, A still usable=1
ADV_RESULT pub.halfclose FAIL sha=c18ff6c480cc1d88 app half-close: echo 10 bytes, EOF=0
ADV_RESULT pub.rst PASS sha=c18ff6c480cc1d88 RST after 4194304 bytes: no warning, module still loaded
ADV_RESULT init.src_key FAIL sha=c18ff6c480cc1d88 src= is not a key; the documented inbound allow list does not exist
ADV_RESULT pub.srcfilter FAIL sha=c18ff6c480cc1d88 connection from 127.0.0.2 was accepted=1: no src= allow list exists
ADV_RESULT pub.writer FAIL sha=c18ff6c480cc1d88 attempted 100 MiB, accepted 75306298 bytes then blocked (errno=11), MemAvailable 412508 -> 133980 kB
ADV_RESULT pub.writer FAIL sha=c18ff6c480cc1d88 attempted 1024 MiB, accepted 74990705 bytes then blocked (errno=11), MemAvailable 411312 -> 180312 kB
ADV_RESULT ctl.nonroot PASS sha=c18ff6c480cc1d88 uid 1000 write to /proc/net/tbtun/config: open failed (the documented mode is 0600 root)
ADV_RESULT init.nonroot_unchanged PASS sha=c18ff6c480cc1d88 config unchanged after the uid-1000 attempt
ADV_RESULT ctl.line513 PASS sha=c18ff6c480cc1d88 513-byte line + newline: write=-1 errno=22 (refused or buffered; the current line buffer is 1024 bytes)
ADV_RESULT ctl.line512 PASS sha=c18ff6c480cc1d88 512-byte line + newline: write=-1 errno=22 (refused or buffered)
ADV_RESULT ctl.line512nonl PASS sha=c18ff6c480cc1d88 512 bytes with no newline: write=512 errno=0 (buffered or refused, never applied)
ADV_RESULT ctl.line1025 PASS sha=c18ff6c480cc1d88 1025-byte line + newline: write=-1 errno=7 (must be refused with the 1024-byte buffer)
ADV_RESULT ctl.line1024nonl PASS sha=c18ff6c480cc1d88 1024 bytes with no newline: write=-1 errno=7
ADV_RESULT init.cfg_unchanged PASS sha=c18ff6c480cc1d88 over-long lines changed nothing
ADV_RESULT init.cfg_roundtrip PASS sha=c18ff6c480cc1d88 the canonical config replays line by line
ADV_RESULT init.echo_after_roundtrip PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=1048576
ADV_RESULT init.max_tunnels FAIL sha=c18ff6c480cc1d88 80 tunnels accepted although max_tunnels=64 (documented limit not enforced)
ADV_RESULT init.scale PASS sha=c18ff6c480cc1d88 1024 carriers up of 1024 rows; MemAvailable 401248 -> 320704 kB
ADV_RESULT init.slab_leak PASS sha=c18ff6c480cc1d88 32k slabs grew by 0 objects over 100 connections
ADV_RESULT init.slab_after_flush PASS sha=c18ff6c480cc1d88 after flush: 32k objects 0 -> 0 (the session teardown is the only free point)
ADV_RESULT init.final_echo PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=4194304
ADV_RESULT init.dmesg FAIL sha=c18ff6c480cc1d88 1 kernel warning/BUG/oops lines; see the console
ADV_RESULT console.klog.final FAIL sha=c18ff6c480cc1d88 1 kernel oops/warning lines (console /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/console-final.log)
ADV_RESULT init.cfg_replay FAIL sha=c18ff6c480cc1d88 config replay rc=0, no clean transfer close: ECHO_TIMEOUT op=read bytes=1048576
ADV_RESULT init.echo_after_replay PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=1048576
ADV_RESULT console.hang.rmmod FAIL sha=c18ff6c480cc1d88 phase rmmod started but did not finish (group rmmod; console /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/console-rmmod.log)
ADV_RESULT console.klog.rmmod FAIL sha=c18ff6c480cc1d88 1 kernel oops/warning lines (console /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/console-rmmod.log)
ADV_RESULT init.flush_midstream FAIL sha=c18ff6c480cc1d88 flush mid-transfer: no clean close: ECHO_TIMEOUT op=read bytes=1048576
ADV_RESULT init.echo_after_flush PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=1048576
ADV_RESULT cat.cfg_self PASS sha=c18ff6c480cc1d88 cat config > config rc=0, 2 tunnels still present: 
ADV_RESULT init.echo_after_cat PASS sha=c18ff6c480cc1d88 ECHO_OK bytes=1048576
ADV_RESULT cat.cfg_mix PASS sha=c18ff6c480cc1d88 bad line not applied, far line applied (write rc=0)
ADV_RESULT init.max_carriers FAIL sha=c18ff6c480cc1d88 n=8 accepted with max_carriers=4 (parameter not enforced)
ADV_RESULT init.port_exhaust FAIL sha=c18ff6c480cc1d88 port block exhausted: tunnel 3 accepted, alloc_fail=16, carriers retry forever with no operator-visible error
ADV_RESULT near.resp_not_chunked PASS sha=c18ff6c480cc1d88 200 with Content-Length instead of chunked: carrier poisoned
ADV_RESULT near.resp_500 PASS sha=c18ff6c480cc1d88 500 response: carrier poisoned
ADV_RESULT near.resp_hdr_big PASS sha=c18ff6c480cc1d88 response header > carrier buffer: carrier poisoned
ADV_RESULT near.resp_close PASS sha=c18ff6c480cc1d88 Connection: close response: carrier poisoned
ADV_RESULT near.hello_wrong_nonce PASS sha=c18ff6c480cc1d88 response HELLO with a wrong nonce: carrier poisoned
ADV_RESULT near.chunk_huge PASS sha=c18ff6c480cc1d88 chunk size 0xFFFFFFFF in a response: carrier poisoned
ADV_RESULT near.split_record PASS sha=c18ff6c480cc1d88 chunk 32 with body_len 100 in a response: carrier poisoned
ADV_RESULT near.chunk_oversize PASS sha=c18ff6c480cc1d88 chunk 8229 in a response: carrier poisoned
ADV_RESULT near.drip_pad PASS sha=c18ff6c480cc1d88 a drip chunk and a PAD chunk between records accepted
ADV_RESULT near.fbk_forged PASS sha=c18ff6c480cc1d88 forged FBK gen+1 and path 15: uack 5000 -> 5000 (must stay 5000)
ADV_RESULT near.data_path PASS sha=c18ff6c480cc1d88 DATA delivered to the app after forged FBK (child rc=0)
ADV_RESULT near.second_conn FAIL sha=c18ff6c480cc1d88 second public connection after the first closed: rc=1 (0 = accepted and usable, 1 = refused)
ADV_RESULT far.garbage PASS sha=c18ff6c480cc1d88 9000 bytes of non-HTTP garbage; carrier must close, not crash
ADV_RESULT far.hello_garbage PASS sha=c18ff6c480cc1d88 HELLO joined (nonce adopted), then 64 garbage bytes; carrier must close
ADV_RESULT far.wrong_nonce PASS sha=c18ff6c480cc1d88 HELLO with the wrong 128-bit nonce: carrier closed; join_bad+nonce_reject 0 -> 0
ADV_RESULT far.anchor PASS sha=c18ff6c480cc1d88 carrier joined index 0, far generation 4
ADV_RESULT far.dup_index PASS sha=c18ff6c480cc1d88 duplicate (nonce,index=0): carrier closed; join_dup 0 -> 0; anchor must survive
ADV_RESULT far.srcaddr_dup PASS sha=c18ff6c480cc1d88 same nonce and index from 127.0.0.2: refused as a duplicate
ADV_RESULT far.srcaddr_free FAIL sha=c18ff6c480cc1d88 same nonce from 127.0.0.2 on a free index: ACCEPTED (no source binding; replayed nonce hijacks a slot)
ADV_RESULT far.chunk_huge PASS sha=c18ff6c480cc1d88 chunk size 0xFFFFFFFF must be PROTO
ADV_RESULT far.bodylen_tail PASS sha=c18ff6c480cc1d88 8224-byte chunk, 35 bytes sent, then FIN: no warning, no overread
ADV_RESULT far.split_record PASS sha=c18ff6c480cc1d88 chunk 32 with body_len 100 (record split across chunks) must be PROTO
ADV_RESULT far.drip_split PASS sha=c18ff6c480cc1d88 drip chunk between header and body must still be PROTO
ADV_RESULT far.chunk_oversize PASS sha=c18ff6c480cc1d88 chunk 8229 > TBT_CHUNK_MAX (8228) must be PROTO
ADV_RESULT far.no_host PASS sha=c18ff6c480cc1d88 request without Host must be PROTO
ADV_RESULT far.headers_big PASS sha=c18ff6c480cc1d88 9000-byte header block must be PROTO (effective cap: carrier buffer)
ADV_RESULT far.open_other_sid PASS sha=c18ff6c480cc1d88 OPEN for a second sid must be PROTO, not a second stream
ADV_RESULT far.fbk_forged PASS sha=c18ff6c480cc1d88 FBK uack 999999 with gen+1 and path 15: carrier 0 uack 5000 -> 5000 (must stay 5000)
ADV_RESULT far.hello_profile0 PASS sha=c18ff6c480cc1d88 HELLO with profile 0 must be refused
ADV_RESULT far.hello_profile9 PASS sha=c18ff6c480cc1d88 HELLO byte 9 = profile 1, generation 2: accepted as a generation, not a profile
ADV_RESULT far.data_path FAIL sha=c18ff6c480cc1d88 anchor echo: ping=0 pong=0 unexpected=0 (duplicate seq must not deliver); far stats rx_records=0 capture_runs=0 actor_calls=0 submit_bytes=0 tx_ack=0
ADV_RESULT console.klog.far FAIL sha=c18ff6c480cc1d88 1 kernel oops/warning lines (console /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/console-far.log)
ADV_RESULT stall.hello_chain PASS sha=c18ff6c480cc1d88 15 sequential joins: every HELLO answered=1, rx_adopt_events=15, join_dup=0, join_nonce_reject=0
ADV_RESULT stall.flush PASS sha=c18ff6c480cc1d88 flush completed after the carrier chain
ADV_RESULT console.klog.stall FAIL sha=c18ff6c480cc1d88 1 kernel oops/warning lines (console /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/console-stall.log)
ADV_RESULT ctl.close_flush PASS sha=c18ff6c480cc1d88 the guest survived the close flush (console: /home/kunweiz/Desktop/vibe/tcp-brutal/adversarial/crash-console.log)
```

### 4.2 分组结论

| 组 | 结果 | 说明 |
|---|---|---|
| public | 3 PASS / 5 FAIL | churn、halfclose、src_key、srcfilter、writer×2 失败 |
| control | 全 PASS | 非 root、行长、canonical round-trip 都成立 |
| resource | 1 PASS / 1 FAIL | 1024 载体可起；`max_tunnels=64` 不生效（80 条被接受） |
| slab | 全 PASS | 100 条垃圾连接后 32k slab 增长 0 |
| final | 1 PASS / 1 FAIL | 4 MiB echo 通过；dmesg 有 1 行 deprecated workqueue |
| replay | 1 PASS / 1 FAIL | 传输中重放 config 后应用读超时 |
| rmmod | **挂起 + oops** | `PHASE_START rmmod` 无 `PHASE_END`；int3 oops，panic |
| flush | 1 PASS / 1 FAIL | 传输中 flush 后应用读超时；flush 返回后 echo 仍可用 |
| catconfig | 全 PASS | `cat config > config` 与恢复 echo 通过 |
| cfg_mix | 全 PASS | 坏行不生效、后续 far 行生效 |
| param | FAIL | `max_carriers=4` 下 `n=8` 仍被接受 |
| port | FAIL | 端口段耗尽后 tunnel 3 仍被接受，`alloc_fail=16`，载体无限重试 |
| near | 11 PASS / 1 FAIL | 恶意响应全部被毒化；`second_conn` 失败 |
| far | 17 PASS / 2 FAIL | `srcaddr_free` 与 `data_path` 失败 |
| stall | 全 PASS | 15 次顺序 HELLO 全部应答；flush 完成 |
| crash repro | PASS | `ctl.close_flush PASS`，`REPRO_SURVIVED` |

`far.data_path` 的现场（`console-far.log:428-445`）：`rx_records=0 capture_runs=1 actor_calls=1 submit_bytes=8 tx=8`，`car_state=7`（CLOSE），`lost=3 join_dup=2 join_bad=1`，`rx_proto=12 rx_errs=14 rx_last_err=-71`，`http_parse_err=9`，`profile_mismatch=1`。合法 anchor 在前面的 HELLO 攻击之后已不在可用状态，DATA 无法投递。

`pub.writer` 的两个失败是新出现的资源观测：应用完全不读时，公共接收路径在阻塞前吃掉了 278 MB / 231 MB 的 `MemAvailable`，超过 harness 的 200 MB 阈值。

---

## 5. bench（本次实测 vs 历史文档）

### 5.1 n=4（40 MiB，raw，m=0）

| 指标 | 本次实测 | 历史（`benches/results/20261105T045320-throughput-rec.csv`，README 表 1） |
|---|---|---|
| control | 2.88 Mbit/s，`strict=1` | 2.61 Mbit/s，`strict=1` |
| tunnel（wire） | **43.30 Mbit/s** | 11.80 Mbit/s |
| app（有效） | **1.31 Mbit/s** | 9.20 Mbit/s |
| app/control | **0.45x** | 3.52x |
| 脚本打印的 `ratio` | 15.03（= wire/control） | 4.52（wire/control） |
| `wire_per_app` | **33.06** | 1.28 |
| `retx_ppm` | **1,171,952** | 177,105 |
| `holes_declared/repaired/unrecoverable` | 1 / 0 / 0 | 6 / 6 / 0 |
| `rx_max_stall_us` | 718,752 | 15,723 |
| `txq_max` | **13,796,424 B** | 65,536 B |
| `wall_s` | 32.2 | 79.2 |
| `rc` / `strict` | **2 / 0** | 0 / 1 |

`rc=2` 的含义（`host/tbt_burst.c:359-367`）：strict 阶段失败且 `recvd==0 && sent>0`，即 40 MiB 发完之后一个回程字节都没读到。

**这是本次最重要的基线事实**：`ratio 15.03x` 与 `tunnel 43.30 Mbit/s` 是重传放大的假象，不是吞吐。用应用字节算，n=4 只有 1.31 Mbit/s，低于 2.88 Mbit/s 的单流 control（0.45x），与交接文档声称的 9.20 Mbit/s / 3.52x 严重不符。

### 5.2 n=1（40 MiB，raw，m=0）

| 指标 | 本次实测 | 历史（`20261105T044430-throughput-rec.csv`，README 表 1） |
|---|---|---|
| control | 2.66 Mbit/s，`strict=1` | 2.73 Mbit/s，`strict=1` |
| tunnel（wire） | 7.90 Mbit/s | 6.22 Mbit/s |
| app | **0.65 Mbit/s** | 3.14 Mbit/s |
| app/control | **0.24x** | 1.15x |
| `wire_per_app` | **12.07** | 1.98 |
| `retx_ppm` | **2,352,439** | 172,077 |
| `holes_unrecoverable` | **1** | 0 |
| `rx_max_stall_us` | **5,004,741**（撞 5 s 预算） | 0 |
| `txq_max` | **26,976,338 B** | 82,064 B |
| `wall_s` | **7.75** | 207.9 |
| `rc` / `strict` | **2 / 0** | 3 / 0 |

历史 n=1 是“跑了 205 s、写 1.98 倍、最后 mismatch”；现在是“7.75 s 内重传 235%、发送队列涨到 27 MB、一个回程字节没读到、5 s 空洞预算触发”。失败模式相同（单载体不可用），但退化程度更重。

### 5.3 与历史对照的解读

- control 本身正常（2.66–2.88 Mbit/s，`strict=1`，`wire/app=1.000`），说明 qdisc、slirp、`tbt_burst` 和测量窗口都没有坏。
- 两条隧道行都在 `retx_ppm` 百万级以上、`txq_max` 从 64 KB 涨到 13.8/27 MB、`wire/app` 从 1.2–2.0 涨到 12–33，且 `rc=2/3 strict=0`。这不是噪声，是数据面回归。
- 历史 README 的“n=4 9.20 Mbit/s、3.52x、strict=1”在本次基线上不成立；不能再用它作为产品主张的当前证据。

---

## 6. 与 `交接.md` §2 声称状态的差异清单

| 交接 §2 的声称 | 本次实测 | 判定 |
|---|---|---|
| “最近一次已知全绿：A–K 11/11 PASS、0 skip” | A–H PASS，I FAIL，J/K 未执行 | **已漂移** |
| “控制 3.0 Mbit/s，n=4 11.7 Mbit/s，3.90×” | control 2.66–2.88；n=4 wire 43.30 / app 1.31；`rc=2 strict=0` | **已漂移** |
| “模块约 4.9 MB；最近 05:28，5,403,144 B” | 5,405,288 B，06:28，sha `c18ff6c4…` | 数值漂移（大小/时间） |
| “`make check` → 365 checks / 0 failures” | 365 / 0 | **仍成立** |
| “`test_fuzz` 8,349,443 / 0” | 8,349,443 / 0 | **仍成立** |
| “`test_http` 249 / 0；h11 通过” | 249 / 0；h11 通过 | **仍成立** |
| “`review/lint.sh` → 15/15，12/12” | 15/15，12/12 | **仍成立** |
| C1 骨架“已验证” | 探针 A PASS；`ctl.*` 全 PASS | 仍成立 |
| C2 “部分验证；n=1 的 40 MiB 严格用例失败” | 仍失败：bench n=1 `rc=2 strict=0`、`wire/app=12.07`、`txq=27 MB`；探针 I 的 4 MiB control 也 `rc=3 strict=0` | 成立但**显著恶化** |
| C3 “已验证；探针 B/C/D/E；benches 全部行 strict=1” | B/C/D/E PASS；bench n1/n4 均 `strict=0` | **“benches 全部行 strict=1”已不成立** |
| C4 “已验证；n=4/8/16 线性增长” | n=4 `strict=0`、app 0.45x；n=8/16 按指示未跑 | **无法复验，且 n=4 反证** |
| C5 “已验证；探针 F dup=2、dup_same_path=0” | 探针 F PASS | 仍成立 |
| C6 “部分验证；G/H 通过；PREDICT 接收端惰性” | G/H PASS；所有 stats `predict_tx=0 predict_used=0` | 仍成立（且 PREDICT 确实未参与） |
| C7 “已验证；探针 E/K/D” | E PASS；K 未执行；D PASS | 部分成立，K 缺测 |
| C8 “已验证；test_http 249 + h11；benches profile 行” | host 测试通过；本次未跑 profile bench；J/K 未跑 | 部分成立 |
| “`adversarial` F1 close-flush 未复跑” | `ctl.close_flush PASS`，`REPRO_SURVIVED` | **已复跑并通过** |
| “F2 rmmod 中途卸载必然 oops” | 实测 `Oops: int3` + `Kernel panic`，phase 挂起 | **仍成立，且已取得原文** |
| “F4 一个会话只服务一条公共连接” | `pub.churn` usable=1/10000；`pub.two` B refused；`near.second_conn` FAIL | **仍成立** |
| “F5 半关闭不传达 EOF” | `pub.halfclose` EOF=0 | **仍成立** |
| “F6 远端载体输入与投递停滞” | `far.data_path` `rx_records=0 car_state=7` | 有证据支持 |
| “F7 每次会话一条 deprecated workqueue 警告” | 4 个 console 各 1 行；`init.dmesg` FAIL | **仍成立** |
| “F10 `max_tunnels` 不生效” | 80 条隧道被接受（文档上限 64） | **仍成立** |
| “F11 `max_carriers` 不生效” | `max_carriers=4` 下 `n=8` 被接受 | **仍成立** |
| “`src=` 未实现” | `init.src_key` + `pub.srcfilter` FAIL | **仍成立** |
| “`flush` 在传输后可能阻塞” | `init.flush_midstream` 与 `init.cfg_replay` 均 ECHO_TIMEOUT | **仍成立** |
| “端口耗尽无操作员可见错误” | `init.port_exhaust FAIL`，`alloc_fail=16` | **仍成立** |
| “`retx_ppm` 115k–304k，`wire/app` 1.17–1.98” | 本次 1.17M（n4）、2.35M（n1）；`wire/app` 33.06/12.07 | **数量级漂移** |
| “`/proc/net/tbtun/http` 只输出原始请求块”等 P3 项 | 本次未逐项复测 | 未验证 |

---

## 7. 新出现的 oops / warning 原文

### 7.1 rmmod 中途卸载（必然崩溃，已复现）

日志：`adversarial/console-rmmod.log:405-500`。`PHASE_START rmmod` 之后没有 `PHASE_END`。

```text
[    5.649190] Oops: int3: 0000 [#1] SMP NOPTI
[    5.649209] CPU: 0 UID: 0 PID: 80 Comm: adv_probe Tainted: G           OE       7.2.9-1-MANJARO #1 PREEMPT(full)  a672d45b14e33932fcd987828575bb40d09db72c
[    5.649221] Tainted: [O]=OOT_MODULE, [E]=UNSIGNED_MODULE
[    5.649228] RIP: 0010:0xffffffffc0607181
[    5.649236] Code: cc cc cc cc ... <cc> cc cc ...
[    5.649284] Call Trace:
[    5.649291]  ? tcp_data_queue+0x9df/0xf50
[    5.649317]  ? tcp_rcv_established+0x2d3/0xd00
[    5.649339]  ? tcp_v4_do_rcv+0x189/0x3d0
[    5.649353]  ? tcp_v4_rcv+0xd08/0x15f0
[    5.649710] Unloaded tainted modules: tbtun(OE):1 [last unloaded: tbtun(OE)]
[    5.649734] Kernel panic - not syncing: Fatal exception in interrupt
```

`RIP` 落在全 `cc`（int3）的已释放模块文本；调用栈在软中断里从 `tcp_data_queue` 进入载体套接字的 `sk_data_ready` 回调。这与 F2 的描述一致：模块卸载后仍从已释放代码取指。

### 7.2 deprecated workqueue（已知 F7，仍然存在）

每个启动的 guest 在第一次 carrier 反馈时打印一次：

```text
[    3.705534] workqueue: work func tbt_fbk_work [tbtun] enqueued on deprecated workqueue. Use system_{percpu|dfl}_wq instead.
```

出现位置：

- `adversarial/console-far.log:411`
- `adversarial/console-final.log:407`
- `adversarial/console-stall.log:408`
- `review/baseline/05-qemu-console.log:442`（探针 A 之后）
- `review/baseline/05b-qemu-probeI.log:474`

它导致 `init.dmesg` 与 4 条 `console.klog.*` FAIL。不是新缺陷，但本次基线明确把它计为失败。

### 7.3 探针 I 的模块 debug 打印（不是 oops）

`05-qemu-console.log:870-960` 有大量 `hole gatefail` / `hole depthfail` / `hole timefail`，以及两条 `TXSTALLKILL`。它们是 `debug_loss=1` 下的 `printk`，不是内核 warning；但说明 4 MiB 的 control 已经进入空洞/重传/5 s 重置路径。

---

## 8. 数据来源标注

- **本次实测（2026-11-05 07:24–07:36）**：第 2–5 节的所有 PASS/FAIL、checks 数、探针结果、ADV_RESULT、bench n1/n4 的 Mbit/s、`retx_ppm`、`wire/app`、`rc/strict`、`txq_max`、内存数值、oops 原文。
- **历史文档（未在本次复测）**：交接 §2 的“11/11 全绿、3.90×”、§7 的 9.20/18.47/34.18 Mbit/s 表、`benches/README.md` 的 profile/loss/latency/memory/scale 数字、review/findings.md 的 05:19/05:27 状态表。
- 第 5 节的“历史”列逐行来自 `benches/results/20261105T045320-throughput-rec.csv` 与 `20261105T044430-throughput-rec.csv`，不是本次测量。

---

## 9. benches 工作树改动（未提交）

用户要求记录 `benches/run.sh` 与 `benches/qemu/init` 的当前改动。完整 diff 在 `review/baseline/10-benches-diff.txt`。

```text
 benches/qemu/init | 171 +++++++++++++++++++++++++++++++++++++++++++++++-------
 benches/run.sh    |  81 +++++++++++++++++++++++---
 2 files changed, 222 insertions(+), 30 deletions(-)
```

摘要：

- `benches/qemu/init`
  - 新增 `P_hole_iters=2000`。
  - `configure()` 增加 `BENCHCFG` 行：把 `ports` 行与 canonical config 以 `|`/`;` 转义后写入 console/CSV，记录产生该行的确切操作面状态。
  - 新增三个场景：`bench_flush_hang()`（5 s 有界 flush，挂起时记录 wchan 与内核栈）、`bench_second_conn()`（A 活跃 / B 并发 / C 在 A 关闭后）、`bench_hole_cost()`（carrier 1 在写边界丢弃每个 DATA，制造每请求一个空洞，记录延迟分布）。
  - `xfer()`、`lat_parse()`、`lat_ok()`、`bench_latency()`、`bench_scale()` 有配套改动；场景在子 shell 中运行，避免 harness 算术/语法错误杀掉 PID 1。
- `benches/run.sh`
  - 新增 `--hole-iters` 等选项与新场景分派；`scale` 的 netem 改为 1 ms；`free_port_triple()`、`build_bins()`、`run_one()` 有配套改动。
  - CSV 新增 `src_sha256` / `src_status`：因为 `tbtun/` 未被 git 跟踪，`git status` 无法标识模块源码，改用源码哈希。
- 同目录还有未跟踪的 `benches/src/tbt_conn.c` 与已修改的 `benches/src/tbt_lat.c`；不在本次要求范围，但属于同一批未提交工作。

这些改动解释了为什么 `benches/results/` 里出现 `second-connection-*`、`hole-cost-*`、`flush-hang-*` 等新 CSV；本次 n4/n1 使用的是工作树里的 `run.sh`/`init`，不是 HEAD 版本。

---

## 10. 结论与下一步

全绿：

- `make check`、`make host-test-san`、`review/lint.sh`。
- 探针 A–H（控制面、数据面、替换、丢包、备用、复制、预测、有界重置）。
- adversarial 的 53 项，包括全部 near/far HTTP 毒化用例、close-flush 复现、config 往返/混合行、slab、1024 载体规模。

失败（按优先级）：

1. **P0 rmmod 中途卸载**：int3 oops + panic，已取得原文。`adversarial/console-rmmod.log:405-500`。
2. **P0 数据面回归**：n=1 与 n=4 的 40 MiB 严格阶段都失败；`retx_ppm` 1.17M/2.35M，`wire/app` 33/12，`txq_max` 13.8/27 MB，`rc=2`。探针 I 的 4 MiB control 也失败，说明不是只有大流量才触发。这直接推翻交接 §2 的“benches 全部行 strict=1”。
3. **P1 多公共连接**：`pub.churn` 10000 次只有 1 次可用；`near.second_conn` 拒绝第二条连接。
4. **P1 半关闭 EOF**、**flush 中传输不干净关闭**、**端口耗尽无错误**、**max_tunnels/max_carriers/src= 不生效**。
5. **P2 deprecated workqueue**（噪声，但让每个 guest 的 dmesg 检查失败）。
6. **新观测**：应用不读时公共接收路径吃掉 278 MB 内存（`pub.writer`）；`far.data_path` 在 HELLO 攻击后 anchor 进入 CLOSE，`rx_records=0`。

最该先修的是 **P0 rmmod**（唯一必然 panic、与业务无关），紧接着是 **数据面回归**：先定位 `retx_ppm` 百万级与 `txq_max` 从 64 KB 涨到 MB 级的原因，再复跑探针 I 与 bench n=4；在这两个数字回到历史量级之前，交接 §7 的吞吐表不能作为当前主张。
