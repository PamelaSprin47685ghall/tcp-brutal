# tbtun 0.3.3 完整发布门 — 执行计划（待部署，先不跑）

- 状态：**计划 + 前置检查完成；未跑任何 gate，未改远端状态，未 push。**
- 触发条件：sclc-031-core 部署 0.3.3 并报告 near/far srcversion 后，pin 身份再执行。
- 规则：单一真机门、串行不并发；每 gate 单次、失败取证不重试；不 push；
  netwatch/喂狗者全程只观察、不停止/不重启。

## 0. 工具与分支（前置检查结果）

| 分支 | commit | 文件 | 状态 |
|---|---|---|---|
| `w-0.3.3-uplink` | `18fd717` | `review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh`、`UPLINK-1G.md`、`far-uplink-recv.py`、`uplink-sampler.sh` | 分支存在，worktree 未建 |
| `w-0.3.3-fault` | `539bf24` | `scripts/qa/fault-inject.sh`、`soak.sh`、`lib.sh`、`stream-server.py` | 分支存在，worktree 未建 |
| `w-0.3.3-session` | `c6d863b` | 0.3.3 产品修复（session/sock + host test） | 待部署 |
| `w-0.3.3-perf` | `a49f49d` | `review/wan-experiment/arm64/perf-0.3.3/`（ab/curl/direct/sampler/stats/versions） | worktree 已存在 `/tmp/tbt-033-perf` |

前置检查（2026-11-08T15:1xZ，只读）：

| 项 | near `.85` | far `.62` |
|---|---|---|
| 磁盘 `/root` | 58G，**37G avail** | 20G，**6.2G avail** |
| 工具 | curl/openssl/sha256sum/ss(6.19)/pgrep/pkill/timeout/awk/stat | python3/curl/sha256sum/ss(6.19)/iptables(1.8.11)/timeout/pgrep/pkill/stat |
| tbtun | unloaded（0.3.3 待部署） | unloaded |
| netwatch | active，MainPID 3926 | — |
| 端口 | 9001 空闲 | **5210/5211 空闲** |
| 1 GiB 源 | `/root/sclc-verify/sclc-1g.bin` 1 GiB（0.3.2 G2 结果） | `/root/sclc-verify-1g/accept-1g.bin` 1 GiB + `far-raw-serve.py` |
| 0.3.2 `.ko` | `/root/tbtun-0.3.2-release-20261108T140023Z/tbtun.ko` | 同路径 |
| perf 0.3.0/0.3.1 `.ko` | **缺**（需 staging） | **缺**（需 staging） |
| near ip rule pref 100 | **当前无**（0.3.3 deploy 需补；uplink/fault/soak 前置要求） | — |

结论：主机工具/磁盘/端口就绪；待 0.3.3 部署、perf `.ko` staging、near bypass rule 恢复。

## 1. G0 部署身份与 setup（sclc-031-core 完成后）

1. 记录 0.3.3 身份：tag/commit、tar sha256、near/far srcversion、module sha256、
   modinfo version（0.3.3）、vermagic、build dir；写入 `release-0.3.3-<UTC>/identity.txt`。
2. 确认配置/join：`mode=sc-ldpc`、256 carriers、`adaptive=off`、near 8×32 L16、
   far 1×256 L8、`c3 up=256 replacing=0 empty=0`、near ip rule
   `100: to 45.11.1.62 lookup main`、`ip route get 45.11.1.62` dev=eth0。
3. 脚本准备：
   - `git worktree add /tmp/tbt-033-uplink w-0.3.3-uplink`、
     `git worktree add /tmp/tbt-033-fault w-0.3.3-fault`（只读 checkout）；
   - 或使用 0.3.3 release tree 中已合并的脚本（以实际 tree 为准）；
   - fault/soak 必须 `NEAR_HOST=root@192.168.123.85`（脚本默认是 `.82`）。
4. perf 版本编组（staging，无流量）：
   - 0.3.0/0.3.1：按 `perf-0.3.3/README.md` 在两端 `git archive tag → tar →
     make M=<dir> modules`，落到 `/root/tbtun-perf/<ver>/tbtun.ko`；
   - 0.3.2：已有 `/root/tbtun-0.3.2-release-.../tbtun.ko`；
   - 0.3.3：0.3.3 build dir 的 `tbtun.ko`；
   - 更新 `perf-versions.conf` 增加 0.3.3 行（near/far 路径）。
5. 各脚本 `--preflight`（只读）逐个跑；全 PASS 才进入 gate。

## 2. Gate 顺序（严格串行；5210/5211 独占）

```
G1 下行 1 GiB byte-exact  (far-raw-serve on 5210)  -> cleanup 停源
G2 上行 1 GiB byte-exact  (far-uplink-recv on 5210) -> cleanup
G3 fault-inject 60 s      (stream-server on 5210)   -> cleanup
G4 soak 60×60 s           (stream-server on 5210)   -> cleanup
G5 perf direct(before) -> A/B -> direct(after)      -> cleanup
G6 恢复 0.3.3 session -> 最终 up=256/内核窗口 -> DKMS -> watchdog
G7 cleanup + SUMMARY
```

### G1 下行 1 GiB byte-exact（复用 0.3.2 的 fix48 harness）

- 命令：`EV=<gate>/downlink SHAPE_EVIDENCE=<G0 preflight far-shape> SSH_TMO=120
  EXPECT_NEAR_SRCVERSION=<0.3.3 near> EXPECT_FAR_SRCVERSION=<0.3.3 far>
  verify-0.3.2-fix48.sh --one-g-only`。
- 判据（沿用 0.3.2 G2）：rc=0、http=200、size=1,073,741,824、两端 sha256 相等；
  c3 up=256/0/0；G2 的 error 计数窗口全 0；replay high <5 MiB；
  never_stored≤256；short_parity>0；pad_skip=0；ack>0；内核窗口无
  BUG/Oops/WARNING（hog 仅记录）。
- 预计 20–30 min（far 丢包时 1.0–1.3 MB/s）。cleanup 停 far-raw-serve，释放 5210。

### G2 上行 1 GiB byte-exact（`verify-uplink-1g.sh`）

- 前置：5210 空闲；near/far `/root` ≥1.5 GiB；near 生成 AES-CTR 1 GiB
  （`EXPECT_SHA256=369d49c2…`）；far receiver 落盘。
- 命令：`EXPECT_NEAR_SRCVERSION=<0.3.3 near> EXPECT_FAR_SRCVERSION=<0.3.3 far>
  EXPECT_MODULE_VERSION=0.3.3 SSH_TMO=40 UPLINK_MAX=7200
  verify-uplink-1g.sh --preflight`，PASS 后 `--run`（单次 POST，不重试）。
- 判据：curl rc=0、http=200、size_upload=1 GiB、两端文件 1 GiB、两端 sha 相同、
  far recv 日志 `recv=1073741824 result=ok`、两端 up min/max=256、
  replacing/empty=0、reset/holes_unrec/deliver_bad 增量 0、内核窗口 clean、
  boot_id 不变。
- 预计 20–35 min（上行 8×32/L16 约 1 MB/s + 生成/sha）。PASS 后脚本清理
  两端 1 GiB 文件；FAIL 保留。

### G3 fault-inject（`scripts/qa/fault-inject.sh`）

- 前置：session up=256；5210 空闲；far iptables（nf_tables）；near `ss -K`
  （iproute2 6.19）；`NEAR_HOST=root@192.168.123.85`。
- 命令：`NEAR_HOST=root@192.168.123.85 CYCLES=3 GAP=0.2 fault-inject.sh --run`
  （默认 RST 2 s；`--keep` 保留模块；`--no-rst`/`--rmmod` 备用）。
- 判据：60 s near 流；carrier kill + far RST 窗口后恢复；up 回到 256；
  注入期允许 stream reset（脚本按注入语义判定）；无残留 RST rule；
  内核窗口无 BUG/Oops/WARNING（D-state 按 whitelist）。
- 预计 3–5 min。cleanup 停 far source、删 RST rule；模块保留给 soak。

### G4 soak 60×60 s（`scripts/qa/soak.sh`，>1 h）

- 前置：5210 空闲；session up=256；`NEAR_HOST=root@192.168.123.85`。
- 命令：`NEAR_HOST=root@192.168.123.85 CYCLES=60 WIN_SECONDS=60
  SAMPLE_SECONDS=5 soak.sh --run`。
- 判据：每 cycle 的 carrier 不变量、零增量计数（reset/holes/deliver_bad/
  outq_drop/replay_drop/drop_sid 等）、内核窗口、内存/ slab / far RSS /
  D-state；`up_min` 不跌。
- 时长：约 65–80 min。**若被进程重启/环境中断**：允许分两段
  `--cycles 30` + `--cycles 30`（同一 session/config），在 SUMMARY 记录
  “连续中断”及两段边界；leak/slab 对比按段分别报告，不冒充单段 60 cycle。
- cleanup 停 far source；模块保留。

### G5 perf A/B 3×60 s + 直连基线（`perf-0.3.3/`）

- 顺序：`perf-direct.sh --run --tag before` → `perf-ab.sh --run --tag 033`
  → `perf-direct.sh --run --tag after`；分析 `perf-stats.py`。
- A/B：版本 `0.3.0 0.3.1 0.3.2 0.3.3`（manifest 已加 0.3.3）；
  `--blocks 1` 即每版本 3×60 s（任务最小要求）；若时间允许用默认 2 blocks
  （ABC+CBA）抵消漂移。`SETTLE` 用 60 s（与 fix48 160 s 的差异记录）。
- 直连：far `0.0.0.0:5211` + `far-raw-serve.py`，near
  `curl http://45.11.1.62:5211/accept-1g.bin`，3×60 s + 前后各 100 次 HEAD；
  near 用 marker 文件管理自建 pref-100 rule。
- 注意：A/B 会卸/装模块，netwatch 只观察；A/B 前若 0.3.3 deploy 已加
  pref-100 rule，按 `perf-ab.sh` 的 marker 逻辑处理（不要留重复/冲突规则）；
  A/B 后按 deploy 配置恢复 0.3.3 session。
- 预计 40–60 min（含 staging 后的 4 版本 join/窗口）。

### G6 最终 up=256 + DKMS + watchdog

- 恢复 0.3.3 session（deploy 的 build/config；pin srcversion），确认
  `c3 up=256 replacing=0 empty=0 up_rescued=0`、内核窗口 clean。
- DKMS 七步（near `.85`，脚本 `/root/dkms-65/`）：
  `REPO=<0.3.3 build dir> VER=0.3.3 OUT=/root/dkms-85-<UTC> KEEP=0
  bash run-dkms-65.sh --run`；`judge-dkms-log.sh` 必须 `DKMS_JUDGE PASS`
  （无 `DKMS_STEP rc=[1-9]`、无 `DKMS_CHECK FAIL`、无 `DKMS_ROLLBACK`、
  `DKMS_RESULT PASS`）；`dkms status` 空、`/usr/src/tbtun-*` 与
  `updates/dkms/tbtun.ko*` 清理。
- watchdog：near unit active、MainPID 持 `/dev/watchdog0`、`first feed ok`；
  DKMS 期间不停止喂狗者。
- 预计 5–10 min。

### G7 cleanup + 报告

- 停 far source、删 RST/ip rule（只删自建）、停 samplers/receiver；
  两端模块按 deploy/测试状态处理；netwatch active；boot_id 不变；不 push。
- 证据目录：`release-0.3.3-<UTC>/`（identity、preflight、downlink、uplink、
  fault、soak、perf-direct-before/after、perf-ab、dkms、watchdog、SUMMARY）。

## 3. 时间预算（串行）

| 段 | 预计 |
|---|---|
| G0 deploy/setup/staging | 15–30 min（perf 0.3.0/0.3.1 构建另计） |
| G1 下行 1 GiB | 20–30 min |
| G2 上行 1 GiB | 20–35 min |
| G3 fault | 3–5 min |
| G4 soak | 65–80 min（或两段） |
| G5 perf | 40–60 min |
| G6 up/DKMS/watchdog | 10–20 min |
| **合计** | **约 3–4 h**（不含 staging 构建） |

## 4. 风险与归因规则

- **far 环境**：0.3.2 gate 期间出现 ICMP 66% loss、carrier stall 0.4–14 s、
  dmesg 刷屏；0.3.3 若吞吐低于基线或 SSH 变慢，先归因环境（记录吞吐/丢包/
  超时），不判产品失败；正确性以 sha/计数/内核窗口为准。
- **5210/5211 独占**：任何两个 gate 不并发；前一个 cleanup 确认端口空闲
  才启动下一个。
- **fault 注入**：kill/RST 可能 reset 流（脚本按注入语义判定）；
  RST rule 由 trap 清理；`--rmmod` 默认关闭。
- **soak 中断**：允许两段，但必须记录连续中断与段边界；不把两段拼成
  “单段 60 cycle”。
- **perf**：模块卸/装期间 netwatch 只观察；A/B 结束恢复 0.3.3；
  `.ko` 身份以 srcversion 为准（不用版本字符串）。
- **near ip rule**：deploy 的 pref-100 与 perf 的 marker rule 不得冲突；
  uplink/fault/soak 前置要求 `ip route get 45.11.1.62` dev=eth0。
- **失败**：每 gate 单次，失败取证保留，不重试；后续依赖 gate 停止；
  release 负责人决定是否重跑。

## 5. 待确认/待部署

1. 0.3.3 的 tag/commit、两端 srcversion、build dir（sclc-031-core）。
2. 0.3.3 release tree 是否包含 uplink/fault/perf 脚本；否则用上述
   worktree/branch 脚本（需建 `/tmp/tbt-033-uplink`、`/tmp/tbt-033-fault`）。
3. perf 0.3.0/0.3.1 的 staging 构建（两端）与 manifest 更新。
4. near ip rule pref 100 的恢复（deploy）与 perf marker rule 的处理。
5. soak 单段 60 cycle 还是两段 30+30（默认单段；中断则两段并记录）。
6. perf A/B 用 1 block（3×60 s/版本）还是默认 2 blocks（时间允许时）。
