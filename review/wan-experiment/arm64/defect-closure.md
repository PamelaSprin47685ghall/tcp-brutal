# 0.2.0 缺陷结案表（模板）

本表按来源分四类：A 本次审计、B prod-code 未修 7 项、C 文档漂移、D
`review/findings.md` 的 `OPEN`。另附 A 类的 lifecycle 7 项与 §9.6 未覆盖项。

**使用规则**

1. 状态列按 `review/s2-merged-design.md` §9 预填；**证据列是结案前必须实际
   通过的检查**，不是引用。任何一行的命令不过，状态改回 `记录` 或 `待办`，
   不得口头结案。
2. 证据只接受两种形式：`文件:行`，或可复制执行并给出期望输出的命令。
   “已修”“已验”不算证据。
3. `复核人/日期` 必须独立于修改者；日期用 UTC `YYYY-MM-DD`。
4. 每次验收（host / 10 s 真机 / 1 GiB / DKMS）完成后，把证据目录写进对应
   行，不新建第二张表。

## 1. 字段与状态

| 字段 | 取值 |
|---|---|
| `ID` | `A-H-01`、`A-M-01`、`L-01`、`B-01`…`B-07`、`C-01`…`C-07`、`D-<findings 编号>` |
| `来源` | A=本次审计；B=prod-code 未修；C=文档漂移；D=findings OPEN；L=lifecycle 附项 |
| `严重度` | critical / high / medium / low / nit |
| `一句话` | 现象 + 触发条件，≤60 字 |
| `状态` | 见下表 |
| `处置理由` | 为什么这样结案；`记录` 必须写明“为什么这一版不修” |
| `证据` | `文件:行` 或命令 + 期望输出 |
| `复核人/日期` | 独立复核者与 UTC 日期 |

| 状态 | 含义（沿用 `review/findings.md:56-61`） |
|---|---|
| `FIXED` | 修复存在，且有命名检查覆盖 |
| `FIXED (static)` | 修复存在，但无运行时测试直达该单元 |
| `FIXED (contract)` | 代码有意不提供旧行为，测试契约已改写并通过 |
| `记录` | 缺陷存在，本版不修，理由成立，且 `CHANGELOG.md` 的 `### Known limitations` 有对应条目 |
| `待办` | 可修、不在本版范围、不阻塞发布；必须有单独 commit 计划 |
| `撤销` | 复核后不成立（设计允许的附加结果，不用于新提交） |

## 2. 来源 A：本次审计高危/中危

| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| A-H-01 | 本次审计 | high | `c->sk` 在 `sock_release` 之后仍被读，UAF | FIXED | 先 unpublish 并过 grace period，再 close；`sk` 加 `__rcu` | `grep -n 'tbt_carrier_unpublish\|tbt_carrier_close' tbtun/tbt_carrier.c` 命中两个定义；`grep -n 'struct sock __rcu \*sk' tbtun/tbtun.h` 命中；`make C=1` 无 `__rcu` 警告 | |
| A-H-02 | 本次审计 | high | dead carrier 被复用，身份错配 | FIXED | 删除复用；`refcount + kfree_rcu`，对象身份稳定 | `grep -c 'take_dead' tbtun/tbt_carrier.c` → `0`；`grep -n 'refcount_inc_not_zero(&c->refs)' tbtun/tbt_carrier.c` → `:122`；`grep -n 'kfree_rcu' tbtun/tbt_carrier.c` 命中（M5 落地后） | |
| A-H-03 | 本次审计 | high | stream work 无引用，拆除后 work 仍跑 | FIXED | 5 个 bit + `tbt_stream_schedule/cancel` 统一持有与归还 | `grep -n 'tbt_stream_schedule\|tbt_stream_cancel' tbtun/tbtun.h` 命中两条 `bool` 声明；`grep -rn 'schedule_work(&st->' tbtun/` 在 M3 落地后应为 0（当前树仍有 `tbt_fec_data.c:702`、`tbt_rx.c:362`，未结案） | |
| A-H-04 | 本次审计 | high | session `refs` 是死字段，destroy 不等归零 | FIXED | 7 个 work + tunnel/stream/carrier 持有；destroy 用 completion 等归零 | `grep -n 'tbt_session_arm\|tbt_session_cancel' tbtun/tbtun.h` 命中声明；`grep -rn 'tbt_session_arm(' tbtun/*.c | wc -l` ≥ 7；`grep -n 'sess_refs' tbtun/tbt_procfs.c` 命中（0.2.0 字段落地后） | |
| A-H-05 | 本次审计 | high | adopt 先装 callback 后挂 prov，读者看到半成品 | FIXED | `joined` 标记 + 顺序重排；`WARN_ON_ONCE` 兜底 | `grep -n 'joined' tbtun/tbt_carrier.c` 命中 adopt/prov 两处；`make check` PASS | |
| A-H-06 | 本次审计 | high | TX dup 帧发布后继续被写，UAF | FIXED | 发布前构造、一次发布；新增 `test_tx_publish` 四等式 | `test -f host/test_tx_publish.c`；`make check` 中 `test_tx_publish` PASS | |
| A-H-07 | 本次审计 | high | TX-6 第 16 槽越界 | FIXED | `tbt_retain_path_ok` 边界；新增 `test_tx_index` | `test -f host/test_tx_index.c`；`make check` 中 `test_tx_index` PASS | |
| A-H-08 | 本次审计 | high | FEC 解码在 `rx->lock` 内睡眠 | FIXED | 解码移入 `w_fec_rx` work；锁内只入队 | `grep -n 'w_fec_rx' tbtun/tbt_fec_data.c` 命中；`host/test_fec` 混合指针用例 PASS；10 s 真机无 `sleeping function called from invalid context` | |
| A-M-01 | 本次审计 | medium | FEC 交付 `-ENOMEM` 整段丢包 | FIXED | 逐包提交 + 游标，`-ENOMEM` 重试不前进 | `grep -n 'fec_deliver_nomem' tbtun/tbt_fec_data.c` 命中；注入 `fec_deliver_fail_ppm=10000` 时流完成 | |
| A-M-02 | 本次审计 | medium | FEC FIN 基数错误，末段被丢 | FIXED | `pkt_end/pkt_base` 统一基数；FIN body 16 字节 | `grep -n 'pkt_end\|pkt_base' tbtun/tbt_fec_data.c` 命中；1 GiB 单流 + 控制连接 `rx_delivered == tx_seq` | |
| A-M-03 | 本次审计 | medium | 空 group 静默损坏 | FIXED | `gtx.blk_id` + marker；37 B 写用例 | `grep -n 'blk_id' tbtun/tbt_fec_data.c` 命中 group 状态判断；`host/test_fec` 37 B 用例 PASS | |
| A-M-04 | 本次审计 | medium | FEC ACK 溢出回绕 | FIXED | `tbt_fec_ack_bytes` 钳位到 `[pkt_base, tx_seq]` | `grep -n 'u64 tbt_fec_ack_bytes' tbtun/tbt_fec.c` 命中；边界用例 0 / `U64_MAX` / 上下界 PASS | |
| A-M-05 | 本次审计 | medium | `short_len` last-writer-wins | FIXED | 首条 PARITY 固定 `(win_len, epoch)`，后续不一致即拒 | `grep -n 'win_len' tbtun/tbt_fec_data.c` 命中首条 PARITY 分支；`host/test_fec` epoch=0/S/S+1 用例 PASS | |

### 2.1 A 附：lifecycle 7 项（`review/s2-merged-design.md` §9.5）

| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| L-01 | lifecycle | medium | `tbt_car_poll_one()` 的 drip 写与 pump 共享 `c->part` 无锁 | FIXED | `mutex_trylock(&pump_lock)` 包住 drip | `grep -n 'pump_lock' tbtun/tbt_carrier.c` 命中 poll 路径 | |
| L-02 | lifecycle | high | `tbt_tx_fbk()` 遍历写已复用对象的 `rx_state` | FIXED | 不复用 + `get/put` | `grep -n 'tbt_carrier_get' tbtun/tbt_tx.c` 命中 fbk 路径 | |
| L-03 | lifecycle | medium | `tbt_fec_data_init()` 失败路径泄漏 `get_net()` | FIXED | `t->sess = s` 提前，失败统一走 destroy | `grep -n 'get_net\|put_net' tbtun/tbt_session.c` 成对出现（`:170`、`:223`） | |
| L-04 | lifecycle | high | `tbt_stream_get()` 发布路径不检查 `closing` | FIXED | 锁内检查 `closing` 后再取引用 | `grep -rn 'READ_ONCE(.*closing)' tbtun/*.c | wc -l` ≥ 2（当前 `tbt_carrier.c` 多处） | |
| L-05 | lifecycle | medium | `cancel_work_sync(&st->w_kill)` 取消 pending 时泄漏引用 | FIXED | `tbt_stream_cancel` 归还引用 | `grep -n 'bool tbt_stream_cancel' tbtun/tbt_sock.c` → `:86`；`grep -n 'tbt_stream_cancel(victim' tbtun/tbt_session.c` 命中 kill 路径 | |
| L-06 | lifecycle | high | `tbt_fec_tx_work` 在 stream 拆除后才 cancel | FIXED | 提前 cancel + `stopping` 闸门 | `grep -n 'stopping' tbtun/tbt_fec_data.c` 命中 tx work 入口 | |
| L-07 | lifecycle | low | `tbt_carrier_free()` 不清 `release_node` | FIXED | `list_del_init(&c->release_node)` | `grep -n 'list_del_init(&c->release_node)' tbtun/tbt_carrier.c` 命中 | |

## 3. 来源 B：prod-code 未修 7 项

仓库中没有权威编号；`review/s2-release-design.md:904-921` 的 B-01…B-07 是候选，
主审计编号不同时以其覆盖，字段与门槛不变。

| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| B-01 | prod-code | medium | FEC 无 NACK/选择性重传，块内丢够即停滞 | 记录 | 本版只修交付与解码；NACK 是协议扩展，不在 0.2.0 范围 | `grep -n 'NACK' CHANGELOG.md` 在 `### Known limitations` 下命中 | |
| B-02 | prod-code | medium | rate epoch 未实现 | 记录 | 同上；`TBT_FEC_CONV 0x1002` 不引入 rate epoch | `grep -n 'rate epoch' CHANGELOG.md` 命中 | |
| B-03 | prod-code | low | bench CSV header 缺 `retx_other`/`data_tx`/`retx_data_ppm` | 待办 | 不在 S2 范围；单独 commit，不阻塞 0.2.0 | `交接.md:603-604`；`grep -n 'retx_other' 交接.md` | |
| B-04 | prod-code | medium | 真机 DKMS/`modprobe`/`rmmod` 未验证 | FIXED | `dkms-65` 七步全 PASS，`DKMS_RESULT PASS` | `review/wan-experiment/arm64/dkms-65-<UTC>/run.log`；`dkms-65/judge-dkms-log.sh <log>` → `DKMS_JUDGE PASS` | |
| B-05 | prod-code | low | `n=4` 吞吐运行间方差 8.38–11.37 Mbit/s | 记录 | 1 GiB 只判完成与校验，不判吞吐 | `交接.md:599-600`；`prod-1g` 的 `perf.txt` 仅记录 | |
| B-06 | prod-code | low | `n=8`/`n=16` 仅单次 40 MiB 记录 | 记录 | 0.2.0 用 256 carriers；n=8/16 长流留待后续 | `交接.md:599-600`；`prod-1g` 只跑 256 | |
| B-07 | prod-code | low | 透明空洞推进未实现，只按精确 seq 前进 | FIXED (contract) | 有意设计：不猜测缺失字节；契约已改写并通过 | `交接.md` 决策记录表；`tbtun/tbt_rx.c` 的精确 seq 前进路径 | |

## 4. 来源 C：文档漂移

| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| C-01 | 文档漂移 | low | 版本引用不一致 | FIXED | M9：`tbt_uapi.h` + README/README.zh/DESIGN/CHANGELOG/交接 | `./scripts/mkdkmsconf.sh --version` → `0.2.0`；`grep -nE '0\.2\.0' README.md README.zh.md DESIGN.md` | |
| C-02 | 文档漂移 | low | `DESIGN.md` 仍写 “Real-machine DKMS install is not validated” | FIXED | 0.2.0 验收后改写为已验真机，引用证据目录 | `grep -n 'Real-machine DKMS' DESIGN.md` → 无“not validated” | |
| C-03 | 文档漂移 | low | `交接.md` 仍写 “不阻塞 0.0.1” | FIXED | 0.1.0 发布时已改 | `grep -n '不阻塞 0\.0\.1' 交接.md` → 空 | |
| C-04 | 文档漂移 | low | `review/findings.md` 的 spec-drift 14 行全标 RESOLVED，需逐行复核 | FIXED | M9 逐行复核代码后保留/撤销 | `grep -c '^| .*| RESOLVED' review/findings.md`；逐行对照 `review/final-checklist.md:801` | |
| C-05 | 文档漂移 | low | README 的 DKMS 记录仍是 0.0.1/x86_64 | 记录 | 历史事实，不伪造；0.2.0 验收后补新段 | `grep -n '0\.0\.1' README.md README.zh.md` 命中历史段；新段引用 `dkms-65-<UTC>` | |
| C-06 | 文档漂移 | medium | `CHANGELOG.md` 的 `[Unreleased]` 是 0.2.0 内容，不能整体改名 | FIXED | M0：在 `[Unreleased]` 之下插入 `[0.1.0] - 2026-11-07`，只收录 0.1.0 七条 | `grep -n '\[Unreleased\]\|\[0\.1\.0\]\|\[0\.2\.0\]' CHANGELOG.md` | |
| C-07 | 文档漂移 | low | `review/final-checklist.md` 头部已 SUPERSEDED，C40/C49 仍写 `TBT_CONV=0x0004` | FIXED | M9：头部加“行内常量以 `tbt_uapi.h` 为准”，不重写 60 条历史门禁 | `head -n 5 review/final-checklist.md`；`grep -n 'TBT_CONV' tbtun/tbt_uapi.h` → `0x0006` | |

## 5. 来源 D：`review/findings.md` 的 OPEN

当前快照没有 `| OPEN` 行（`review/findings.md:56-61` 的 Final status 表）；历史
说明在 `:200`。表是快照，不是实时状态，每次结案必须重跑：

```sh
grep -n '| OPEN' review/findings.md                 # 期望：空
grep -nE '\| (critical|high|medium|low|nit) \| (OPEN|FIXED)' review/findings.md | wc -l
bash review/lint.sh                                 # 16/16 + 14/14
source ~/.cache/tbtun-build/env.sh && make check     # test_parse 357, test_fec 2566, 0 failures
```

出现 `OPEN` 时逐条进表，行模板：

| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| D-01 | findings | `<critical\|high\|medium\|low\|nit>` | <现象> | `<FIXED\|记录\|撤销>` | <理由> | `review/findings.md:<line>` + <命令与期望> | |

## 6. 未覆盖项与结案理由（`review/s2-merged-design.md` §9.6）

| 项 | 来源 | 状态 | 理由 | 证据 |
|---|---|---|---|---|
| `TBT_FEC_PAD 0x23` 与 `TBT_F_PAD` 同值 | WireFacts | FIXED | M2 删除定义与 `hdr_check` 接受分支；0x23 只按 32 B 旧 PAD 解析 | `grep -rn 'TBT_FEC_PAD' tbtun/` → 空；`grep -n 'TBT_F_PAD' tbtun/tbt_uapi.h` → 仅旧 PAD |
| `tbt_hdr_check()` 行号引用错位 | WireFacts | FIXED | 文档修正：引用以实际定义为准，不照抄设计文本 | `grep -n 'int tbt_hdr_check' tbtun/tbt_proto.c`（当前树 `:102`） |
| `tbt_body_bounds()` 行号引用错位 | WireFacts | FIXED | 文档修正：引用以实际定义为准 | `grep -n 'static inline bool tbt_body_bounds' tbtun/tbt_uapi.h` → `:281` |
| `TBT_CONV`/`TBT_FEC_CONV` 的 host 断言 | WireFacts | FIXED | `host/test_fuzz.c:219`、`host/test_fec.c:215` 同步改 | `grep -n 'TBT_CONV\|TBT_FEC_CONV' host/test_fuzz.c host/test_fec.c` |
| FEC 无 NACK（B-01） | release B | 记录 | 协议扩展，不在 0.2.0 | `CHANGELOG.md` 的 Known limitations |
| rate epoch（B-02） | release B | 记录 | 同上 | 同上 |
| bench CSV header（B-03） | release B | 待办 | 单独 commit | `交接.md:603-604` |
| 吞吐方差/单次记录（B-05/B-06） | release B | 记录 | 验收只判完成与校验 | `交接.md:599-600` |
| README 的 0.0.1 DKMS 历史（C-05） | release C | 记录 | 历史事实，不伪造 | `README.md:548`、`README.zh.md:421` |
| 主审计 A 类具体编号 | release A | 待填 | 仓库无该清单；模板与门槛见本文 §1/§7 | — |
| prod-code 7 项权威编号 | release B | 候选 | 主审计编号不同时以其覆盖 | `review/s2-release-design.md:904-921` |
| `review/findings.md` 的 F3 缺号 | FindingsLedger | 记录 | 台账瑕疵，不影响状态 | `grep -n 'F3' review/findings.md` |

## 7. 结案门槛

| 时点 | 门槛 |
|---|---|
| 0.1.0 发布前 | A 类无 `记录`（除非主 agent 批准且写入 Known limitations）；B 类每项有状态；C-01/C-03 为 `FIXED`；D 类无 `OPEN` |
| 0.1.0 发布后 | 本表随 tag 归档到 `review/findings.md` 顶部或 `review/release-0.1.0.md`；不得只存在于对话中 |
| 0.2.0 验收前 | B-04 必须 `FIXED`；C-02 已改写；1 GiB 与 DKMS 各有独立证据目录与 `SUMMARY.md` |
| 0.2.0 发布前 | 1 GiB：sha256 一致、两端 c9 错误计数为零、无 Oops；DKMS：七步全 PASS；否则不发 |

## 8. 一行怎么填（示例）

```markdown
| A-H-02 | 本次审计 | high | dead carrier 被复用，身份错配 | FIXED | 删除复用；refcount + kfree_rcu | `grep -c 'take_dead' tbtun/tbt_carrier.c` → `0`；`grep -n 'kfree_rcu' tbtun/tbt_carrier.c` 命中；1 GiB `counters-near.txt` 的 `dead_now=12` | 张三 2026-11-20 |
```

注意最后一项：`dead_now` 的实测值来自证据目录，不是设计文本。
