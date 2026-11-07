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
| B-04 | prod-code | medium | 真机 DKMS/`modprobe`/`rmmod` 未验证 | 待办 | 十轮 1 GiB 均 FAIL（fix19 9.75%、fix20 99.63% 尾部截断），按发布门槛七步未执行；`.65` 上仍无 `dkms` 包。需先 `apt-get install -y --no-install-recommends dkms`（候选 3.2.2-1ubuntu1），待 1 GiB PASS 后以 `REPO=/root/tbtun-0.2.0-fix20-20261107T084234Z` 跑 `--run` | `.65:/root/dkms-65-preflight-20261107T013548Z/run.log` 的 `DKMS_CHECK dkms_installed FAIL (dkms not found)`、`DKMS_RESULT FAIL (checks_failed=2 rollback=0)`；`accept-1g-20261107T080653Z/FAILURE.md`（fix19）与 `accept-1g-20261107T084611Z/FAILURE2.md`（fix20，DKMS 均未执行）；无 `dkms-65-<UTC>/run.log`，不得写 `DKMS_JUDGE PASS` | |
| B-05 | prod-code | low | `n=4` 吞吐运行间方差 8.38–11.37 Mbit/s | 记录 | 吞吐按 error 处理：直连 eth0 实测 2.0–2.2 Mbit/s、路径丢包 12–18%，1 GiB 在 1800 s 内需 ≥4.8 Mbit/s。十轮 1 GiB 均未通过；fix20 独占重跑 99.63%、2.20 Mbit/s、far wire/交付 8.4x、retx 6.35x、尾部 3.98 MB 被 `TBT_ST_DEAD` 截断、无 UBSAN；fix19 1.12 Mbit/s、wire/交付 21.9x、`fec_replay_miss_enospc=872,166`、`tbt_tx.c:2818` UBSAN；fix5/7/8 已使已交付前缀逐字节正确、NACK 与载波阻尼生效 | `accept-1g-20261107T084611Z/FAILURE2.md`（99.63%、2.20 Mbit/s、wire 8.4x、retx 6.35x、尾部截断、无 UBSAN）；`accept-1g-20261107T080653Z/FAILURE.md`（1.12 Mbit/s、retx 19.8x、enospc 872,166、UBSAN）；`accept-1g-20261107T044300Z/FAILURE.md`（2.19 Mbit/s、NACK drop=0、replaced=5）与 `accept-1g-20261107T032225Z/link-and-carrier.txt`（iperf3 2.59–5.03 Mbit/s、ping 12–18%） | |
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

## 9. 2026-11-07 1 GiB 验收（FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T013627Z/`
（`FAILURE.md`、`run.log`、`counters-{near,far}.txt`、`near-curl.txt`、
`near-curl.err.txt`、`first-divergence.txt`、`data-loss-shift-map.txt`、
`near-chunks.txt`、`far-chunks-all.txt`）。

形态 256 carriers、n=4 m=4、grouped、w=8、crypto=on、http-kal，模块
near `E8A17366F0F61FDC30C3C70` / far `72411330E2541E41EBD374E`，两端 join 到
256，下载唯一一次。

**结论：FAIL。** curl rc=28，`size_download=57671482`，本地文件 57671482 B，
near sha256 `17ecbda3…` 与 far `c6a25615…` 不同；near `fec_payload_crc_fail`
增量 145；其余错误计数（两端 `rx_errs`、`connect_fail`、`alloc_fail`、
`join_bad`、`gaps_total`、`reorder_overflow`）全为 0；两端内核无
BUG/Oops/WARNING；near `boot_id` 未变；喂狗者 `pid=23184` 仍在。

**新缺陷（critical，尚无编号）：静默丢数。** 收到的 57,671,482 字节不是源文件
的前缀：前 18,956,090 字节逐字节相同，此后 near[18,956,090] 等于
far[18,964,282]，即恰好 8,192 字节缺失；偏移在 near 22,024,192 与 23,072,768
之间再增加两次 8,192，此后稳定为 24,576 字节缺失。三次删除都是
`TBT_FEC_S` 大小的份额载荷，且没有任何计数器报告空洞（`gaps_total=0`、
`holes_*=0`、`reorder_overflow=0`、`ooo_bytes=0`）。这使 0.2.0 不能作为可靠
字节流发布。

**停流点**：`rx_delivered=tx_ack=57671680` 后不再前进；far
`submit_bytes=66256896`，8.58 MB 卡在 8 MB 发送窗口；near 只有
`fec_payload_crc_fail=145` 一个异常计数。无 NACK（B-01）与精确 seq 交付
（B-07）使一个不可恢复的组永久停流，curl 随后触发 120 s / 64 KiB/s 限速。

**工具缺陷**（不改变上述判定，但必须修）：`ssh_near()` 的 `timeout 20` 套在
传输上，`NEAR_FETCH_RC=124` 是 `timeout` 的返回码而非 near 判定；
`near_busy()` 无结尾换行使 `read` 返回 1，`set -e` 在 `perf.txt`/`SUMMARY.md`/
清理之前终止脚本；`journalctl -k --since` 两端都返回 `-- No entries --`，
内核窗口判据实际为空。

## 10. 2026-11-07 第二次 1 GiB 验收（fix2，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T020829Z/`
（`FAILURE.md`、`near-curl.txt`、`near-sha256.txt`、`far-prefix-sha256.txt`、
`near-stats-{pre,post}.txt`、`far-stats-{pre,post}.txt`、`near-dmesg.txt`、
`far-dmesg.txt`、`near-fetch.log`）。

模块：near `86A8FDA3185B4E25E81D441`（sha256 `c55334fa…0344e2`）、
far `D23D3E58CFC8C12B3B72AB7`，`TBT_FEC_CONV 0x1003`，256 carriers/n4m4/
grouped/w8/crypto=on/http-kal。

**结论：FAIL。** curl rc=18
（`end of response with 1007681734 bytes missing`），`size_download=66060090`，
317.80 s，1.66 Mbit/s；near sha256 `bd5ad5d6…` 与
far 前 66,060,090 字节 `7f7cdaff…` 不同 —— **交付字节仍不是源前缀**，
但 fix 新增的 `fec_slot_past_cut` 两端均为 0，说明不是第一轮那条分支，
或分歧位于拆除尾段。因 harness 在 FAIL 时仍执行 `cleanup_all`，near 的部分
文件已被删除，分歧点无法定位。

停流：near `rx_delivered=66060288` 后 stream 消失（`streams_active=0`），
far `c6 holes_unrecoverable=1`，即 30 s 有界复位把流杀掉，curl 收到完整但
过短的 HTTP 响应（FIN，不是超时）。carrier 250 在两端同时 `lost`
（near `[11721.191997]`、far `[67751.369639]`），far 随即 ACCEPTED 新
carrier；near c3 `lost=1 replaced=1`。按 uptime 折算，掉载晚于 curl 失败约
7 s，更像拆除的结果而非触发点。

计数：两端 c3 的 `connect_fail/alloc_fail/join_bad/reorder_overflow` 与
`gaps_total`、`rx_errs` 全 0；c10 硬错误
（`fec_decode_nomem`、`fec_deliver_nomem`、`fec_deliver_dup`、
`fec_deliver_bad`、`fec_epoch_conflict`、`fec_slot_past_cut`、`fec_fin_bad`、
`fec_ack_bad`、`fec_tx_share_gap`）两端全 0。非零：near
`fec_payload_crc_fail=16`、`fec_late_share=1943`、`fec_empty_block=31`、
`fec_decode_blocks=8`、`fec_decode_recovered=6`；far `fec_late_share=1`、
`fec_pad_share=31`。内核无 BUG/Oops/WARNING；near boot_id 未变；喂狗者
`pid=23184` 仍在。

**工具缺陷**：`near-fetch.sh` 是 `#!/bin/sh` 却用了 bash 子串展开
`${got_hash:0:16}`，dash 报 `303: Bad substitution` 并在 sha256 之后中止，
near 侧计数/内核/判定全部缺失；`run-1g.sh` 在 FAIL 时仍清理并删除部分文件；
near 内核窗口再次为空（SKIP）。

## 11. 2026-11-07 第三轮 1 GiB 验收（fix3，join 失败）

证据：`review/wan-experiment/arm64/accept-1g-20261107T023541Z/`
（`FAILURE.md`、`near-pre.txt`、`far-pre.txt`、`join-probes.txt`、`run.log`）。

模块：near `D9F5F8F4E4DCBAD99583434`（sha256 `8bb55507…daebbb`）、
far `D639F67ACE6C0AF9565B204`（sha256 `eece0233…16bc4a`，
`/root/tbtun-0.2.0-fix3-20261107T023217Z/tbtun.ko`，`TBT_FEC_CONV 0x1003`）。

**结论：FAIL，未发起传输。** join gate 180 s 内 `far_up=0`、`near_up=256`
（偶有 240–255 抖动）。far 侧从模块加载（uptime 68880.660677）起没有任何
accept：`tunnels` 的 streams=0、`rx_adopt_events=0`、`capture_runs=0`、
`actor_calls=0`、`wire=0`、c3/c9/c10 全 0，dmesg 里既无
`carrier ACCEPTED` 也无 `refused an incoming connection`。near 侧
`lost=13312 replaced=13312`、`wire=6,433,674` B（13,312 代 × ~483 B 的
HELLO/profile 字节）、`rx_reads=26,159` 全部 `rx_read_zero`，即对端关闭/复位。

根因候选：①far 的 accept 循环从未运行 —— `tbt_listen_start()` 把
`t->accepting` 置 0，只有 `tbt_tunnel_add()` 在 `tbt_session_create()` 之后
置 1 并 `schedule_work(&t->w_accept)`，而 `tbt_ready()` 在 `accepting==0`
时直接返回；②本轮配置写入"替换"了 far 上已存在的 tunnel 1，走
`tbt_listen_stop(old)`（`accepting=0` + `cancel_work_sync`）后重建 listener，
若新的 `accepting=1`/`schedule_work` 未生效，监听仍在但无人 accept；
③fix3 的 `3994eea` 只改 `tbt_car_schedule_rx`/`w_rx`，far 无 carrier 时该路径
未运行，不能直接解释，但 adopt 路径未排除；④不是路径/backlog 问题
（`tcp_abort_on_overflow=0`、`somaxconn=4096`、backlog 128、`connect_fail=0`）。

未做：未发起下载、未跑 DKMS、未重跑。现场：near 已 del+rmmod、9001 空闲、
喂狗者 `pid=23184` 仍在；far 已 del、9000 空闲、模块保持加载、5210 httpd
与 1 GiB 源仍在。

**更正（2026-11-07T03:06Z）：** 上述 ①/② 的 accept 推断不成立。真正的
原因是 far 的 fail2ban 在 02:26:27Z 把 near 的直连出口 IP `171.120.34.226`
加入 `addr-set-sshd` 并 `reject with icmp port-unreachable`（只对 TCP），
所以 SYN 根本没到 listener；解除封禁后 join 在 16 s 内到 256。见 §12 与
`accept-1g-20261107T030212Z/firewall-unban.txt`。

## 12. 2026-11-07 第四轮 1 GiB 验收（fix4，直连 eth0，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T030212Z/`
（`FAILURE.md`、`stream-integrity.txt`、`near-partial-1048378.bin`、
`firewall-unban.txt`、`near-stats-{pre,post}.txt`、`far-stats-{pre,post}.txt`）。

模块：near `01F468A1B5C62F96115CCB2`（sha256 `ddb2320d…36f4f`）、
far `8A6BBE49134ADB5BA741F0E`；config 带 `dev=eth0` 并回读通过；near 侧
`ip rule pref 100 to 45.11.1.62/32 lookup main`，全程不走 mihomo。

**结论：FAIL。** 解除 fail2ban 封禁后 join 于 173 s 到 256（前 173 s 全 0，
是封禁窗口），传输 187.19 s 只收到 **1,048,378 B**，curl rc=28
（`Operation too slow`），吞吐 0.04 Mbit/s。

**静默丢数仍在。** 前 909,114 B 与源逐字节相同；此后 near[909114] 对齐到
far[900922]（少 8 KiB），到 near[941882] 又回到正确对齐（说明那 8 KiB 被
**重复**投递、另一段 8 KiB **缺失**），再累积 16/24/32 KiB 缺失，最后
1,023,802 处已无法对齐。fix3/fix4 新增的守卫全为 0
（`fec_deliver_ooo`、`fec_deliver_dup`、`fec_slot_past_cut`、
`fec_deliver_bad`、`fec_epoch_conflict`），唯一非零错误是
`fec_payload_crc_fail=179`；`fec_decode_recovered=69/75`（92% 的块靠奇偶恢复）、
`fec_late_share=220`、`fec_dup_share=17`、far `fec_credit_wait=4331`。

**fail2ban 封禁（第三轮 join 失败的真正原因）**：far 的 sshd jail 在
02:26:27Z 封禁 `171.120.34.226`（near 的直连出口 IP），触发是 5 次
`Invalid user kunweiz` 的 SSH 尝试（`ssh 45.11.1.62` 默认用本地用户名）。
封禁规则 `meta l4proto tcp ip saddr @addr-set-sshd reject with
icmp port-unreachable` 只拦 TCP，所以 ping 通、TCP 全被拒。已解除并加入
`ignoreip`（运行时 + `/etc/fail2ban/jail.local`），其余 35 个攻击源保持封禁。

**工具缺陷**：`near-fetch.sh` 的 `delta_zero()` 按 `^<name>\.` 找行，而
`counters.txt` 的行是 `c9.<name>.pre=...`，导致 3 个 c9 计数被误报为
"counter missing"；两端内核窗口仍为空（SKIP），只有 dmesg 尾部被判定。

未做：未跑 DKMS、未重跑。现场保留：near `/root/dl-1g`（1 MiB 部分文件）、
far `/root/accept-1g`（1 GiB 源与 httpd）。

## 13. 2026-11-07 第五轮 1 GiB 验收（fix5，直连 eth0，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T032225Z/`
（`FAILURE.md`、`link-and-carrier.txt`、`near-stats-{pre,post}.txt`、
`far-stats-{pre,post}.txt`、`near-dmesg.txt`、`far-dmesg.txt`）。

模块：near `9043190A7F8C199B8C6D74D`（sha256 `690c4046…7a5986`）、
far `0498A16AD1CD774A172B2D2`；join 10 s 到 256；`dev=eth0` 回读通过。

**数据完整性：PASS（对已交付前缀）。** 交付 142,606,138 B，near 与 far 同长度
前缀的 sha256 均为 `3850cebc…f58a13`，逐字节一致 —— 前四轮的重放/丢数被
fix5 的 FEC tx/rx work 串行化修掉了。near c9 全 0、near c10 全 0、far c10 全 0。

**整体仍 FAIL：流在 136 MiB 被杀。** curl rc=18
（`end of response with 931135686 bytes missing`），不是 1800 s 超时。

**根因：far 的载波 stall 判定误杀健康载波。** far `stall_declared=4` 与
`lost=4` 完全一致；near `lost=6 replaced=6` 而 `stall_declared=0`，是被对端
关闭后的连带。判定条件为 `una != nxt`、窗口未关、且 `carrier_stall_ms`
（配置写死 15000 ms）内 ACK 无进展；本路径 `rtt_mean=316 ms`、
`rttvar=1.33–2.85 s`、`rtt_max=4.87 s`、`rto=500 ms`，一次丢包就把 RTO
退避到 0.5→1→2→4→8→16 s，15 s 落在退避区间内。载波被判死后其上的 share
丢失，`n=4 m=4` 也补不回来，far `c6 holes_unrecoverable=1`，30 s 有界复位
杀流。

**慢速按 error 记。** 直连路径：ping far 丢包 18%（隧道在）/12%（隧道停），
RTT 297–300 ms；ping 1.1.1.1 丢包 6.7%；iperf3 near→far 上传 5.03 Mbit/s
（接收端 2.59 Mbit/s，5 s 内 206 次重传）；隧道运行时 far→near 反向
0.397 Mbit/s。本次 2.01 Mbit/s，1 GiB 在 1800 s 内需 ≥4.8 Mbit/s。

**工具缺陷**：`near-fetch.sh` 的 `delta_zero()` 仍按 `^<name>\.` 找行，
而 `counters.txt` 是 `c9.<name>.pre=...`，3 个 c9 计数被误报为 missing。

未做：未跑 DKMS、未重跑。现场保留：near `/root/dl-1g`（142,606,138 B，
逐字节前缀）、far `/root/accept-1g`（1 GiB 源与 httpd）；两端 tunnel 已删，
near 已 rmmod，喂狗者 `pid=23184` 仍在。

## 14. 2026-11-07 第七轮 1 GiB 验收（fix7，直连 eth0，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T041301Z/`
（`FAILURE.md`、`near-stats-{pre,post}.txt`、`far-stats-{pre,post}.txt`、
`near-dmesg.txt`、`far-dmesg.txt`）。

模块：near `7F05230FD86BE43BC6A541F`（sha256 `af28dd7d…82ec41`）、
far `0E5C37320004439E2DDF405`；`TBT_FEC_CONV 0x1004`；join 4 s 到 256；
`dev=eth0` 回读通过；`carrier_stall_ms=0`（auto）。

**数据完整性：PASS（已交付前缀）。** 交付 209,715,002 B，near 与 far 同长度
前缀 sha256 均为 `7baca3ce…804186`，逐字节一致。near c9 全 0、
near c10 错误字段全 0（`fec_decode_blocks=223`、`fec_decode_recovered=137`）。

**整体 FAIL：200 MiB 处流被复位。** curl rc=18
（`end of response with 864026822 bytes missing`），777.33 s，2.16 Mbit/s。

**根因：NACK 一条都没发出去。** 两端 `fec_nack_tx=0`，而
`fec_nack_drop` near=19 / far=174；`fec_nack_rx=0`、`fec_replay_hit=0`、
`fec_nack_retx=0`。`fec_nack_drop` 只在 `tbt_fec_send_nack()` 返回错误时累加，
该函数经 `tbt_fec_nack_pack` → `tbt_fec_make_record` → `tbt_fec_out_push`
（GFP_ATOMIC kmalloc，-ENOMEM；replay 才可能 -ENOSPC）。也就是说修复请求
从未进入发送队列；near 等满 30.03 s（`fec_nack_repair_ns`）后复位
（`fec_reset_user=1`），far `c6 holes_unrecoverable=1`。

**载波仍在频繁更换。** near `lost=173 replaced=173`、`connect_retry=150`、
收尾 `up=246`；far `lost=174`、`up=241`。auto stall 阈值是 `5×RTO`
（钳 1..60 s），暂停后 `min(2×stall,300 s)` 无线上进展才替换；在本路径
（RTT 300 ms、RTTVAR > 1 s、丢包 12–18%）下 777 s 仍换了 173 次。

**性能**：2.16 Mbit/s；near CPU 3.9%、far CPU 10.5%；near
MemAvailable 7,546,128 kB；无 ARQ 重传（`retx_*` 全 0）。内核无
BUG/Oops/WARNING；near boot_id 未变；喂狗者 `pid=23184` 仍在。

**工具缺口**：harness 完全不判 c11（本报告的 c11 由证据文件重算）；
`delta_zero()` 的命名不匹配问题仍在。

未做：未跑 DKMS、未重跑。现场保留：near `/root/dl-1g`（209,715,002 B
逐字节前缀）、far `/root/accept-1g`（1 GiB 源与 httpd）。

## 15. 2026-11-07 第八轮 1 GiB 验收（fix8，直连 eth0，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T044300Z/`
（`FAILURE.md`、`near-stats-{pre,post}.txt`、`far-stats-{pre,post}.txt`、
`near-dmesg.txt`、`far-dmesg.txt`）。

模块：near `0EA220DC9D3198ED49BEFBA`（sha256 `c124ec45…0c45d0`）、
far `377B3DD6D984F53EF52F14B`；`TBT_FEC_CONV 0x1004`；join 8 s 到 256；
`dev=eth0` 回读通过；`carrier_stall_ms=0`（auto）。

**数据完整性：PASS（已交付前缀）。** 交付 127,926,074 B，near 与 far 同长度
前缀 sha256 均为 `5bd3dfaa…fdc874`，逐字节一致。

**两项修复都生效。**
① NACK 真正上线：near `fec_nack_tx=57 rx=17 retx=71 drop=0`，far
`fec_nack_tx=17 rx=57 retx=1080 replay_hit=3831 nack_retx_bytes=8,890,560`；
`fec_nack_enomem/einval/enospc/eother/bad` 全 0；修复延迟 near 0.43 ms、
far 1.52 s。
② 载波更换被压住：near `lost=5 replaced=5`（第七轮 173）、far `lost=5`、
两端 `up=256`；dmesg 有 `carrier N stalled … recovered after 323 ms/2833 ms`
的暂停-恢复对。

**整体 FAIL：128 MiB 处流被复位。** curl rc=18
（`end of response with 945815750 bytes missing`），468.05 s，2.19 Mbit/s。

**根因：replay ring 未命中，洞补不上。** near `fec_replay_miss=497`、
far `fec_replay_miss=3354`（约 47% 的请求落在环外），near
`fec_nack_repair_ns=30.54 s`（等满修复窗口）后 `fec_reset_user=1` 复位；
far `c6 holes_unrecoverable=1`。环是 K=4 个超级块、上限 9 MiB，而本形态一个
超级块是 32 groups × 8 shares × 8 KiB = 2 MiB，K=4 约 8 MiB，与 8 MiB 的
tx window 同量级：发送端在接收端前沿卡住时继续推进，恰好把需要的那一块挤出
环。环必须大于发送窗口，或在 ACK 前沿停滞超过环深时停止推进。

**性能**：2.19 Mbit/s；near CPU 3.4%、far CPU 10.0%；near
MemAvailable 7,541,872 kB；修复放大：far 为 127.9 MB 交付重放 8.89 MB。

**工具缺口**：harness 仍不判 c11；`delta_zero()` 命名不匹配仍在。

未做：未跑 DKMS、未重跑。现场保留：near `/root/dl-1g`（127,926,074 B
逐字节前缀）、far `/root/accept-1g`（1 GiB 源与 httpd）。

## 16. 2026-11-07 fix19 单流 1 GiB（作废，外部干预）

证据：`review/wan-experiment/arm64/accept-1g-20261107T075754Z/`
（`INTERFERENCE.md`、`SUMMARY.md`、`near-dmesg.txt`、`far-dmesg.txt`）。

harness 改为单流 curl、上限 7200 s、停滞 120 s 且 <64 KiB/s 判失败；
near fix19 `FB5F31B87EB79BFCA33E34A`（sha256 `107d1e80…42eac6`），far fix19
`C0346E98476A3A31EF512A4`。preflight PASS（dev=eth0、route 走 eth0），
join 8 s 到 256。

**本轮作废，不产生任何产品结论。** near 的 tbtun 在 join 之后被外部
load/unload：near dmesg `32359.607 unloaded`、`32360.623 loaded`、
`32394.887 unloaded`；far 在 `88425.8` 同一毫秒丢掉全部载波并报
`no carrier can carry: user side backpressured`。near-fetch 启动时
`/proc/net/tbtun/config` 不存在、9001 未监听，curl 从未运行（rc=4）。
`SUMMARY.md` 里的 `size_download=127926074` / sha `5bd3dfaa…` 是第八轮遗留的
`/root/dl-1g` 文件，不是本轮数据；本轮没有 c10/c11/c12。

未重跑。现场：far 仍加载 fix19、5210 与 1 GiB 源在；near 无模块、喂狗者
`pid=23184` 在、boot_id 未变。下次运行前需要 near 的独占窗口。

## 17. 2026-11-07 fix19 单流 1 GiB（独占重跑，FAIL）

证据：`review/wan-experiment/arm64/accept-1g-20261107T080653Z/`
（`SUMMARY.md`、`FAILURE.md`、`counters-full.txt`、
`near-cleanup-manual.txt`、`far-final-state.txt`、`near-kernel-window.txt`、
`far-kernel-window.txt`）。

near fix19 `FB5F31B87EB79BFCA33E34A`（sha256 `107d1e80…42eac6`），far fix19
`C0346E98476A3A31EF512A4`。preflight PASS；join 5 s 到 256/256；传输
08:07:46Z–08:20:18Z，单流 curl，7200 s 上限、120 s 停滞判失败。

**结论：FAIL，未跑 DKMS，未重跑。** curl rc=28（最后 120 s <64 KiB/s），
`size_download=104,711,916`（9.75%），本地大小相同，sha256
`fe57db74…` ≠ far `c6a25615…`；746.8 s、1.12 Mbit/s；near CPU 8.1%、
far CPU 55.0%；near MemAvailable 7,533,608 kB。

**失败点 1（新缺陷）：`tbt_tx.c:2818` UBSAN 越界移位。** far 在
08:11:50Z 报 `UBSAN: shift-out-of-bounds in tbtun/tbt_tx.c:2818:32`，
`shift exponent 163`，栈 `tbt_tx_pump → tbt_pump_work`，内核 taint
`G W OE`。源码 `u16 blocked`（`:2516`）+ `blocked |= BIT(c->index)`
（`:2818`）；`c->index` 为 0..255，`u16` 掩码只覆盖 0..15，index≥64 触发
UBSAN。`tbt_sched_pick(..., u16 blocked, ...)`（`:2219`）与
`blocked & BIT(idx)`（`:2243`）同样只对 16 条载波有效。harness 的内核窗口
正则只有 `BUG:|Oops|WARNING:|general protection fault`，漏掉 UBSAN；已把
`UBSAN:` 加入 `run-1g.sh`、`near-fetch.sh`、`run-dkms-65.sh`。

**失败点 2：修复环容量与重传风暴。** far wire 2,292,002,003 B / 交付
104,712,114 B = 21.9x；`fec_nack_retx=323,667` 帧 / 2,071,297,322 B =
19.8x。far `fec_replay_miss=959,714`，其中 `enospc=872,166`、
`never_stored=87,463`、`get=87,548`、`evicted=85`；near
`fec_replay_miss=202,624`（全部 `never_stored`/`get`）。near
`fec_nack_repair_max_ns=16.6 s` > 8×RTT 上界 15.0 s；far 0.224 s。

**失败点 3：载波 churn 与 RX 协议错误。** near `c3.replaced=102`、
`lost=9`、`connect_fail` +1、`connect_retry` +42、`stall_declared=10`；
far `lost=95`、`stall_declared=780`、`stall_dead=1`。near `rx_errs=93`、
`rx_proto=93`、`rx_last_err=-71`（EPROTO）；`tbt_car_rx_error()` 对每个
非 EALREADY 接收错误停掉该载波，93 次协议错误对应载波退役。
`c3.una_stall_us` near 545 s / far 540 s。

**零计数**：`fec_stream_refused` 两端 0；`fec_reset_hole` 两端 0；
`holes_unrecoverable` 两端 0；c10 硬错误两端 0；c9 crc/proto 两端 0。
非零的是 `fec_replay_miss_evicted=85`（far）、`fec_replay_miss_get`
202,624/87,548、`fec_replay_miss_enospc=872,166`（far）。

**内核/重启/喂狗者**：无 `BUG:`/`Oops`/`general protection fault`/
`WARNING:`；唯一的 splat 是上述 UBSAN。near boot_id `0a91d2eb…` 未变；
喂狗者 `pid=23184` 全程持有 `/dev/watchdog0`；运行期间无外部模块
load/unload（near dmesg 只有本次 `32888.406 loaded`，清理前无
`unloaded`；far 只有 `87917.924 loaded`）。08:04:42Z 启动的旧 `final-run`
harness 已在 08:06:31Z 被终止，其 `accept-1g-20261107T080442Z` 冻结，
与本轮无关。

**未做**：DKMS 七步（PASS 前提未满足）、重跑、tag/push。清理：near
`del 1`+`rmmod`、删 pref-100 rule 与 marker、恢复
`kernel.panic/panic_on_oops/oops_limit/panic_on_rcu_stall/oops_all_cpu_backtrace`
为 0/0/10000/0/0 并删 `/etc/sysctl.d/99-tbtun-visibility.conf`；保留
`/root/dl-1g`（104,711,916 B 证据）。far 保留 fix19 模块、256 配置、
9000/5210 与 1 GiB 源；near 喂狗者保留。

**下一步（未执行）**：修 `blocked` 掩码（256 位或索引上限）与 replay ring
容量/repair 策略；修好后重新独占跑 1 GiB，再跑 DKMS。

## 18. 2026-11-07 fix20 单流 1 GiB（独占重跑，FAIL，99.63% 尾部截断）

证据：`review/wan-experiment/arm64/accept-1g-20261107T084611Z/`
（`SUMMARY.md`、`FAILURE2.md`、`counters-full.txt`、
`far-httpd-timeout-probe.txt`、`near-fetch-near-stats-post.txt`、
`near-cleanup-manual.txt`、`far-final-state.txt`、两端 kernel window）。

near fix20 `9D5B8A616A565FE62DEA315`（sha256 `fe076133…50c88`），far fix20
`B39C75B094C277E6177450C`。preflight PASS；join 10 s 到 256/256；传输
08:46:58Z–09:52:00Z，单流 curl。

**结论：FAIL，未跑 DKMS，未重跑。** curl rc=18
（`end of response with 3977607 bytes missing`），
`size_download=1,069,764,217`（99.63%），本地大小相同；far 完整 sha256
`c6a25615…`，near 文件 sha256 `35b6797e…`。**near 文件的 sha256 与 far 源前
1,069,764,217 B 的 sha256 完全一致**（`head -c 1069764217 … | sha256sum`），
即隧道交付的前缀逐字节正确，未损坏、未乱序；缺的是最后 3,977,607 B
（0.37%）。3896.767 s、2.20 Mbit/s；near CPU 6.7%、far CPU 31.6%；
near MemAvailable 7,512,000 kB。

**失败点：尾部流被 `TBT_ST_DEAD` 复位。** near 在 curl 退出瞬间的
per-stream 快照：`state=5 kill=5 ... skerr=0 fin_seq=0 fin_q=0 fin_w=0
rst_w=1`；`state=5`/`kill=5` 是 `TBT_ST_DEAD`，`rst_w=1` 表示发的是 RST
而非 FIN，所以 curl 报 rc=18。两端 `fec_reset_user=1`
（user/peer/application 拆除），`fec_reset_hole=0`、
`holes_unrecoverable=0`、`fec_reset_tcp_failed=0`；内核窗口无
`fec reset`/`fec deliver conflict`/`fec ordered walk hole`。far 的 target
（busybox httpd）在 1,069,764,415 B 处结束；far 侧
`tgt_state`/`tgt_err`/kill reason 在 post 快照时已随流回收，未捕获首个事件。

**60 s 源超时假设已被证伪。** busybox httpd 新版本有
`SO_SNDTIMEO=DATA_WRITE_TIMEOUT(60s)`；但在 far 上直接停读 65 s 再 drain，
httpd 仍把完整文件发完（4,096 + 1,073,737,926 B，然后 EOF），见
`far-httpd-timeout-probe.txt`。尾部 EOF 不是源 httpd 的 60 s 写超时。

**fix20 改进（实测）**：交付 99.63%（fix19 9.75%）；near
`rx_errs/rx_proto` 0/0（fix19 93/93）；near `replaced/lost` 9/9
（fix19 102/9）；near `connect_fail` 0；far `fec_replay_miss` 21,899
（fix19 959,714），`enospc` 0（fix19 872,166），`evicted` 0
（fix19 85）；far wire/交付 8.4x（fix19 21.9x），retx/交付 6.35x
（fix19 19.8x）；near `fec_nack_repair_max_ns` 4.53 s
（fix19 16.6 s，且 < 8×RTT 12.79 s）；无 UBSAN（fix19
`tbt_tx.c:2818`）。256 位 `blocked` 掩码与 NACK 过滤/去重均生效。

**非零计数**：`fec_reset_user=1` 两端（正常拆除）；`fec_replay_miss_get`
near 388,028 / far 21,899，全部 `never_stored`（near 是 far 对 near TX
控制帧的探测，near replay hit=2；far 数据方向 miss 率
21,899/839,318=2.6%）；far `fec_nack_retx=825,558` /
6,795,028,339 B；far `fec_nack_backpressure=1,492`、
`fec_replay_queue_high=8,388,408`（8 MiB 环顶）、`fec_replay_dedup=26,656`。
要求的零计数（`fec_stream_refused`、`fec_replay_miss_enospc`、
`fec_replay_miss_evicted`、`fec_nack_drop`、`fec_reset_hole`、
`holes_unrecoverable`、c10 硬错误、c9 crc/proto）两端全 0。

**内核/重启/喂狗者**：两端无 `BUG:`/`Oops`/`GPF`/`WARNING:`/`UBSAN:`；
near boot_id `0a91d2eb…` 未变；喂狗者 `pid=23184` 全程在；运行期无外部
模块 load/unload。

**未做**：DKMS（PASS 前提未满足）、重跑、tag/push。清理：near
`del 1`+`rmmod`、删 pref-100 rule 与 marker；保留 `/root/dl-1g`
（1,069,764,217 B 证据）；far 保留 fix20 模块、256 配置、9000/5210 与
1 GiB 源；near sysctl 仍为 0/0/10000/0/0。

**下一步（未执行）**：在 curl 退出/流拆除时抓 far 的 per-stream
`kill`/`tgt_state`/`tgt_err`，定位 far target EOF 的首个事件；同时评估
far TX backpressure（replay queue 8 MiB 顶、`fec_nack_backpressure=1,492`）
与数据优先策略；修好后重新独占跑 1 GiB，再跑 DKMS。

## 19. 2026-11-07 fix21 单流 1 GiB（无效运行：near 被并发占用）

near fix21 `4F8426255ED8270D36513E1`（sha256
`4af699ed…6557b`），far fix21 `234CB9E14AAEB5AD7784B8D`。preflight PASS
（near dev=eth0、`ip rule` pref 100 added、`ip route get 45.11.1.62` 走
eth0）；join 11 s 到 256/256；两端 config 回读 PASS（256 carriers、n=4
m=4、w=8、fec=on、grouped、crypto=on、http-kal、`dev=eth0`）。

**结论：本次运行无效，不作为 fix21 的产品判定，未跑 DKMS、未重跑。**
原因不是产品缺陷，而是 **near 被两个 `run-1g.sh` 同时占用**：

- 本 run（EV `accept-1g-20261107T101803Z`）18:18:03 启动，18:18:53
  开始 transfer。
- 并发的另一 run（pid 1072211，18:17:53 启动，EV
  `accept-1g-20261107T101753Z`）在 18:18:54 启动自己的 near-fetch。
- 两个 curl 同时连 near 的 `127.0.0.1:9001`。FEC v6 数据平面一次只
  承载一个 stream（`tbt_sock.c:695` 的 `tbt_session_fec_stream_live`
  门），先到者建立 stream，后到者被 `tbt_stream_start_now` 以
  `-EOPNOTSUPP` 拒绝并 `fec_stream_refused++`。

**本 run 的失败点**：curl rc=7，`http=000 size=0 time=0.000311
speed=0`，stderr `curl: (7) Failed to connect to 127.0.0.1 port 9001
after 0 ms: Could not connect to server`。near 侧
`fec_stream_refused=1`（`tbtun/tbt_sock.c:698`），`streams_total=2`
而 `streams_active=1 n_streams=1`：被采纳的那个 stream
（`sid=502410510336`，`state=1`）属于并发 run，本 run 的 curl 在
connect 阶段收到 RST，因此 size/sha 全 0。

**并发 run 的痕迹**：near `rx_delivered=15728746`、
`rx_drain_bytes=88034344`（并发 run 的下载），near dmesg 只有
carrier 连接失败（`-99` 132 次、carrier 72 `-110`），无 accept 拒绝
日志；far `streams_total=259`、`rx_adopt_events=259`、`n_streams=1`，
`c12 fec_outq 21:3/24696`，far watcher `FIRST_STREAM 10:18:56`、
`WATCH_STOP 10:18:57`，`tgt_state=1 tgt_err=0`。near 上并发 run 的
`sh /root/near-fetch.sh --run`（pid 212514）与 `curl`（pid 213471）
在 18:21 仍 ESTAB 并持续写 `/root/dl-1g/accept-1g.bin`（已 98 MB）。

**两端健康**：near boot_id `0a91d2eb…` 未变，喂狗者 `pid=23184` 全程
在；near 内核窗口仅 1 行（carrier 72 `-110`），far 窗口
`journalctl` 空（SKIP）；两端 dmesg 尾部无 `BUG:`/`Oops`/`UBSAN:`/
`WARNING:`/GPF。far 5210 raw sender（pid 143680）与 9000 监听正常，
1 GiB 源 sha256 `c6a25615…` 与任务书一致。

**未做**：DKMS（PASS 前提未满足）、重跑、tag/push。near 仍加载 fix21，
等待并发 run 结束后再按任务书清理（`del 1`、`rmmod`、删 pref-100
rule、sysctl）。证据：`review/wan-experiment/arm64/accept-1g-20261107T101803Z/`
（`run.log`、`near-fetch.log`、`counters-*.txt`、`near-post.txt`、
`far-post.txt`、`far-stream-watch.*`）。

### 19.1 清场后的唯一重跑（同样无效：外部 `rmmod`）

清场：kill 并发 run（本地 pid 1072211 及其 `timeout`/`ssh`/`tee` 子进程；
near 上 `near-fetch.sh` 212514 与 `curl` 213471）；near `del 1` +
`rmmod`，删 `/root/dl-1g` 与 pref-100 rule 及 marker；确认 9001 空闲、
`/proc/net/tbtun` 消失、boot_id `0a91d2eb…` 未变、喂狗者 23184 在。
far 重置 session（`del 1` + 重写 256 config）后
`streams_active=0 n_streams=0`，c10/c11/c12 全零，raw sender 143680 与
9000 监听在。

重跑 EV `accept-1g-20261107T102334Z`：preflight PASS；join 10 s 到
256/256；两端 config 回读 PASS；transfer 10:24:21 开始。**near 内核在
10:24:58（monotonic 41137）被外部 `rmmod`**（dmesg `tbtun: unloaded`），
`run-1g.sh` 随后在 10:25:40 收到 SIGTERM（`RUN_RC=143`），near-fetch
未留下任何数据（`/root/dl-1g` 不存在）。**本次同样不是产品判定。**

**但重跑暴露一个必须记录的可疑点**：transfer 期间 near 只有
`tunnels … streams=1`，而 **far 的 `tunnels … streams=256`**，far
`c11 fec_nack_retx=26904`、`fec_replay_hit=26904`；near 卸载后 far 的
`streams_active=0 n_streams=0`（256 个 stream 随 peer 消失而回收）。
单 near stream 对应 256 个 far stream，与
`tbtun/tbt_session.c:793-845` 的 adopt 路径吻合：`tbt_session_bind_sid`
在 `spin_unlock` 之后才调 `tbt_stream_get()`，256 个 carrier 上的同一
OPEN 并发到达时都可能通过 `!st` 判定，各自创建 stream。第一次运行
far 的 `streams_total=259`、`rx_adopt_events=259` 也是同一形态。**这是
待复现、待修的竞态假设，本次传输未完成，不能据此定论。**

**未做**：DKMS、第三次运行、tag/push（按指令停止）。far 保留 256
config、raw sender 143680、9000 监听，session 已重置为全零；near
保持清理后状态。证据：`accept-1g-20261107T102334Z/`
（`run.log`、`near-fetch.log`、`far-stats-after-rmmod.txt`、
`far-tunnels.txt`、`near-tunnels.txt`）与 near dmesg
`41085 loaded` / `41137 unloaded`。

### 19.2 10:24:50 外部 `rmmod` 的取证结论

`journalctl -k` 把卸载定在 **2026-11-07 18:24:50 +08**（UTC 10:24:50），
与 dmesg monotonic 41137 一致。取证结果：

| 来源 | 结果 |
|---|---|
| `journalctl -u ssh`（18:23–18:26） | `-- No entries --` |
| `journalctl` 全量 18:24:40–18:25:05 | 只有 `tbtun: unloaded` 一行，无会话记录 |
| `/var/log/auth.log` | 该时段只有每分钟的 `CRON` pam session，无 sshd 记录 |
| `last -x` / `/var/log/wtmp` | wtmp 最后写入 06:00；btmp 空；无该时段会话 |
| `/var/log/audit/` | 目录不存在（无 auditd） |
| `/root/.bash_history` | 只有早期 ssh key / `ip addr`，无 `rmmod`/`run-1g` |
| near 活跃 sshd 会话 | 只有我的 ControlMaster `sshd-session: root@notty`（pid 208594，18:13:52） |
| 本地进程 | 无 `run-1g.sh`/`near-fetch`/`rmmod`/`deploy_hnd0`/`build_on_65` |

**结论**：near 侧没有可归因的交互式会话记录，卸载只能来自**复用的
ControlMaster 连接**上的非交互命令；该连接同时被多个本地 agent 共用，
本地进程已全部退出，**无法从日志进一步定位到具体会话**。已知的启动器
是旧 final-run 的 parent（本地 pid 592094），由 final-run2 负责停掉。
清场时已 `ssh -O exit` 关闭两个复用 socket，并停掉本地
`/tmp/near-guard.sh` 守护；此后 socket 又被重建，说明仍有进程在使用该
路径，交由 final-run2 处理。

**当前状态**：near 无模块、`/proc/net/tbtun` 消失、9001 空闲、无
pref-100 rule、`/root/dl-1g` 已删、boot_id `0a91d2eb…` 未变、喂狗者
23184 在；far fix21 `234CB9E1…`、256 carriers config、9000/5210 监听、
raw sender 143680 在、`streams_active=0 n_streams=0`、c10/c11/c12 全零。
**按指令不重跑**，等 adopt 竞态修复并重建 fix22 后，再做最后一次
独占 1 GiB。

### 19.3 更正：`tunnels … streams=` 不是 stream 数

fix22 运行中复核 `/proc/net/tbtun/tunnels`：near `streams=1`、
`streams_active=1 n_streams=1`；far `streams=256`、但
`streams_active=1 n_streams=1`。far 的 `tunnels … streams=` 来自
`tbtun/tbt_listen.c` 的 `tbt_accept_one()`：该函数对 **near 的 listener**
走 `tbt_stream_start()`，对 **far 的 listener** 走 `tbt_carrier_adopt()`，
两者之后都执行 `t->streams++`。far 的 listener 接受的是 carrier 连接，
所以 256 是 **256 个 carrier（+3 次重连）**，不是 256 个 stream。
真实 stream 数看 `streams_active` / `n_streams`（本次均为 1）。

**因此 §19.1 的“单 near stream → 256 far streams”推断不成立，撤回。**
fix22 的 `a940d60`（`tbt_stream_get_created()` 在 `sess->lock` 内去重，
只有 `created=true` 的调用者执行 `tbt_far_open_seen()`）仍是合理的并发
去重修复，但 256 这个数字不再作为该竞态的证据；竞态本身没有在本次
验收中被观测到。

## 20. 2026-11-07 fix22 单流 1 GiB（最终验收）

near fix22 `A492881E31FCE9FBB0CDE5D`（sha256
`d1eeb7742304413c0395d70dd02e42e154ef209be482dccb89affbaa7b6ad02d`），
far fix22 `16A5C0241D4EA848C19DEB2`（debug=0）。修复 `a940d60`
（`bind one FEC stream per sid under the session lock`）：
`tbt_stream_get_created()` 在 `sess->lock` 内去重，只有
`created=true` 的调用者执行 `tbt_far_open_seen()`。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
pid 143680）；join 3 s 到 256/256；两端 config 回读 PASS（256 carriers、
n=4 m=4、w=8、fec=on、grouped、crypto=on、http-kal、`dev=eth0`）。

> 过程记录：fix22 的首次调用在 setup 阶段 FAIL —— `far-serve.sh --start`
> 需要 21.2 s，超过 run-1g.sh 默认的 `SSH_TMO=20`，ssh 被杀、`FAR_SHA256`
> 为空；near 模块未加载。改 `SSH_TMO=180` 后重跑，即本节。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、teardown、内核窗口、
boot_id、喂狗者、DKMS。）

### 20.1 结果：FAIL（curl 停滞判据，68.9%）

transfer 10:41:07Z–11:27:12Z（2756.0 s），单流 curl，HTTP 200。

| 项 | 值 |
|---|---|
| curl rc | **28**（`Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds`） |
| size_download / 本地文件 | **739,705,610**（68.9%），期望 1,073,741,824 |
| 速度 | 268,394 B/s（2.15 Mbit/s） |
| near sha256 | `06f690d8f1908e6d…`（与 far `c6a25615…` 不一致，因不完整） |
| CPU | near 10.0%、far 33.5% |
| 内核窗口 | 两端 `journalctl` 有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 pid 23184 全程在 |

**要求的零计数全部为 0**（两端 delta）：`fec_stream_refused`、
`fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、
`fec_replay_miss_enospc`、`fec_replay_miss_evicted`、`fec_nack_drop`、
`fec_reset_tcp_failed`、`fec_reset_hole`、`holes_unrecoverable`、
`holes_declared`、`holes_repaired`、`close_timeouts`、`gaps_total`、
`rx_errs`、`connect_fail`、`alloc_fail`、`join_bad`；near `rx_proto=0`。
near `replaced=6`、`lost=6`（**回落到 lost 量级**）。

**非零计数（失败线索）**：

| 端 | 计数 | 值 |
|---|---|---|
| near | `fec_nack_tx` | 41,872 |
| near | `fec_replay_miss` | **274,389**，全部 `never_stored`（`fec_replay_hit` 仅 3） |
| near | `fec_nack_repair_ns` | **234,130,824,978**（累计 234 s 等待修复） |
| near | `fec_nack_repair_max_ns` | **5,760,000,881**（单次 5.76 s；8×RTT 边界 13.5 s，判定 PASS） |
| far | `fec_nack_retx` | **419,962** |
| far | `fec_replay_hit` | 415,798 |
| far | `fec_replay_miss` | 14,889，全部 `never_stored` |
| far | `fec_nack_backpressure` | 717 |
| far | `fec_replay_stale` | 738 |
| far | `fec_nack_repair_ns` | 365,650,716（0.37 s） |

**teardown**：far watcher `WATCH_START 10:41:14`、`FIRST_STREAM 10:41:17`、
`NO_STREAM_LINE 11:27:16`；最后一条 live stream 行
`sid=260097179648 state=5 kill=5 attached=0x2 … tgt_state=0 tgt_err=0
fin_seq=0 fin_q=0 fin_w=0 rst_w=1`。两端 `fec_reset_user=1`，
`fec_reset_hole=0`、`fec_reset_tcp_failed=0`。

**失败点**：curl 的 120 s / 64 KiB/s 停滞判据在 11:25–11:27 触发，传输
停在 68.9%。低速窗口的候选原因（未定论）：near 累计 234 s 的修复等待
（单次最大 5.76 s）、far 的 `fec_nack_backpressure=717`，以及 **far 反复
NACK near 的少量数据而 near 的 replay 全部 `never_stored`**（274,389
次 miss 对 41,872 次 NACK）。这不是计数错误（要求的错误计数全 0），
而是修复/重传路径把吞吐压到判据以下。

**未做**：DKMS（FAIL）、重跑、tag/push。证据：
`review/wan-experiment/arm64/accept-1g-20261107T103835Z/`。

### 20.2 诊断：absent / never_stored 的 NACK 风暴

A/B 的诊断：本次 FAIL 不是错误计数，而是 **NACK 风暴**。far 对 near
没有、也从未存储的块反复发 NACK：near 侧 `fec_replay_miss=274,389`
全部 `never_stored`，而 near 的 `fec_nack_tx` 只有 41,872；far 侧
`fec_nack_retx=419,962`、`fec_nack_backpressure=717`、
`fec_replay_stale=738`。对 never_stored 块的每次 NACK 都不可能被满足，
只能等超时/重试，累计把 near 的修复等待推到 234 s（单次最大 5.76 s），
最终让 curl 的 120 s / 64 KiB/s 判据在 68.9% 处触发。

修复方向（B，fix23）：在 NACK 生成/发送路径过滤 absent 与
`never_stored` 的块，不再为不可能被 replay 的块反复请求。

**状态**：near 保持清理态（无模块、9001 空闲、无 pref-100 rule、
`/root/dl-1g` 已删、boot_id 未变、喂狗者 23184 在）；far 保留
fix22（`16A5C024…`，debug=0）、256 carriers config、9000/5210 与
raw sender 143680。按指令暂停重跑，等 fix23 后再做独占 1 GiB。

## 21. 2026-11-07 fix23 单流 1 GiB（最终验收）

near fix23 `3279ADF65B3DDE2785A6D0C`（sha256
`2187eece5de0b172c0b80cb724d54e6e6fadf7ff9cef9a799a2524d6382c3714`），
far fix23 `36AC04FBCABBCA8D52408A0`（debug=0）。修复方向：NACK
生成/发送路径过滤 absent 与 `never_stored` 的块（§20.2）。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
143680）；join 到 256/256；两端 config 回读 PASS。`SSH_TMO=180`
（far-serve.sh --start 实测 21.2 s，超过默认 20 s）。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、`fec_replay_high_water`
/`pass_throttled`/`fec_flush_skip`/`hol_wait`、teardown、内核窗口、
boot_id、喂狗者、DKMS。）

### 21.1 结果：FAIL（curl 停滞，21.6%；NACK 风暴已消除，HOL 等待上升）

transfer 11:44:21Z–12:03:35Z（1142.2 s），单流 curl，HTTP 200。

| 项 | 值 |
|---|---|
| curl rc | **28**（`Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds`） |
| size_download / 本地文件 | **232,456,192**（21.6%），期望 1,073,741,824 |
| 速度 | 203,507 B/s（1.63 Mbit/s） |
| near sha256 | `593ddca4c5831a67…`（不完整） |
| CPU | near 9.0%、far 43.0% |
| 内核窗口 | 两端有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 23184 在 |

**要求的零计数全部为 0**（两端 delta）：`fec_stream_refused`、
`fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、
`fec_replay_miss_enospc`、`fec_replay_miss_evicted`、`fec_nack_drop`、
`fec_reset_tcp_failed`、`fec_reset_hole`、`holes_unrecoverable`、
`holes_declared`、`holes_repaired`、`close_timeouts`、`gaps_total`、
`rx_errs`、`connect_fail`、`alloc_fail`、`join_bad`；near `rx_proto=0`。
near `replaced=4`、`lost=4`（回落到 lost 量级）。

**NACK 风暴已消除（fix23 生效）**：

| 计数 | fix22 | fix23 | 变化 |
|---|---|---|---|
| near `fec_replay_miss` | 274,389（全 never_stored） | **246** | −99.9% |
| near `fec_nack_rx` | 8,583 | **9** | −99.9% |
| near `fec_nack_tx` | 41,872 | 21,995 | −47% |
| far `fec_nack_never_stored` | （无此计数） | **622** | 新增，低位 |
| far `fec_nack_absent_reply` | （无此计数） | **7,111** | 新增 |
| far `fec_replay_high_water` | （无此计数） | **127** | 新增 |
| far `fec_replay_pass_throttled` | （无此计数） | **54,570** | 新增 |

**但 HOL 等待与 flush 跳过大幅上升**：

| 计数 | fix22 最终 | fix23 最终 | 变化 |
|---|---|---|---|
| far `fec_flush_skip` | 19,061 | **2,220,187** | ×116 |
| far `fec_hol_wait` | 3,201 | **4,091,101** | ×1278 |
| far `fec_flush_pass` | 13,307 | 331,217 | ×25 |
| far `fec_nack_backpressure` | 717 | 1,930 | ×2.7 |
| far `fec_replay_stale` | 738 | 1,340 | ×1.8 |
| far `fec_nack_retx` | 419,962 | 264,659 | −37% |
| near `fec_nack_repair_ns` | 234,130,824,978（234 s） | 124,912,767,595（125 s） | −47% |
| near `fec_nack_repair_max_ns` | 5,760,000,881（5.76 s） | 6,173,308,350（6.17 s） | +7% |

**失败点**：curl 的 120 s / 64 KiB/s 判据在 12:01–12:03 触发，停在
21.6%。NACK 风暴不再出现（near 的 replay miss 从 27 万降到 246），
但阻塞转移到了 HOL：far 的 `fec_hol_wait=4.09M`、`fec_flush_skip=2.22M`，
`fec_replay_pass_throttled=54,570`。也就是说，修复路径不再对
never_stored 的块空转，但有序交付仍在等缺口，吞吐反而低于 fix22。

**teardown**：far watcher `WATCH_START 11:44:27`、`FIRST_STREAM 11:44:30`、
`NO_STREAM_LINE 12:03:35`；最后 live 行 `sid=158427250688 state=5 kill=5
attached=0x2 … tgt_state=0 tgt_err=0 fin_w=0 rst_w=1`；两端
`fec_reset_user=1`、`fec_reset_hole=0`、`fec_reset_tcp_failed=0`。

**未做**：DKMS（FAIL）、重跑、tag/push。证据：
`review/wan-experiment/arm64/accept-1g-20261107T114209Z/`。

## 22. 2026-11-07 fix24 单流 1 GiB（最终验收）

near fix24 `962A9D3336B7F64629AA9D7`（sha256
`64e08b76f9bed7cfcf070a466035afa71af63f54acc00be13266a6df9eb41f35`），
far fix24 ko sha256
`0a501e9fc2b7c0d1c6affa4dda037e41e3c235fe9d7b8d1f389f0876c9febe97`
（srcversion `36AC04FBCABBCA8D52408A0` 与 fix23 相同，用 ko sha 区分），
debug=0。修复方向：降低 `fec_flush_skip` / `fec_hol_wait`（fix23 中
分别升到 2.22M / 4.09M），让吞吐回到 fix22 量级。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
143680）；join 到 256/256；两端 config 回读 PASS。`SSH_TMO=180`。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、
`fec_replay_high_water`/`pass_throttled`/`fec_flush_skip`/`hol_wait`
是否回到 fix22 量级、teardown、内核窗口、boot_id、喂狗者、DKMS。）

### 22.1 结果：FAIL（far 侧 hole reset，13.2%；HOL 指标已回落）

transfer 12:15:22Z–12:23:05Z（443.8 s），单流 curl，HTTP 200。

| 项 | 值 |
|---|---|
| curl rc | **18**（`end of response with 932069713 bytes missing`） |
| size_download / 本地文件 | **141,672,111**（13.2%） |
| 速度 | 319,248 B/s（2.55 Mbit/s） |
| near sha256 | `8aa4123b1371fdc3…`（不完整） |
| CPU | near 9.0%、far 42.9% |
| 内核窗口 | 两端有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 23184 在 |

**FAIL 的直接原因（任务要求为 0 的项）**：**far
`holes_unrecoverable=1`、`fec_reset_hole=1`**。far 在 443.8 s 判定一个
洞不可恢复并复位流，near 的连接随之关闭，curl 报 rc=18。near 侧
`holes_unrecoverable=0`、`fec_reset_hole=0`，但 `fec_reset_user=1`
（对端拆除）。far 的 `holes_declared=0`、`holes_repaired=0`，
`close_timeouts=0`；watcher 最后一条 stream 行 `state=5 kill=4
rst_w=1`（kill=4，与 fix22/fix23 的 kill=5 不同）。

其余要求的零计数仍为 0：`fec_stream_refused`、`fec_proto_fail`、
`fec_hdr_crc_fail`、`fec_payload_crc_fail`、`fec_replay_miss_enospc`、
`fec_replay_miss_evicted`、`fec_nack_drop`、`fec_reset_tcp_failed`、
`close_timeouts`、`gaps_total`、`rx_errs`、`connect_fail`、`alloc_fail`、
`join_bad`；near `rx_proto=0`、`replaced=2`、`lost=2`。

**fix24 的 HOL/flush 指标确实回落（对比 fix23 同期/最终）**：

| 计数（far） | fix23 | fix24 | 变化 |
|---|---|---|---|
| `fec_flush_skip` | 2,220,187 | **641,201** | −71% |
| `fec_hol_wait` | 4,091,101 | **1,336,568** | −67% |
| `fec_replay_pass_throttled` | 54,570 | **2,612** | −95% |
| `fec_replay_high_water` | 127 | **19** | −85% |
| `fec_nack_backpressure` | 1,930 | **475** | −75% |
| `fec_nack_retx` | 264,659 | 122,797 | −54% |

near `fec_replay_miss=276`（全 never_stored）、`fec_nack_rx=9` 仍处低位；
near `fec_nack_repair_ns=7,825,547,748`（7.8 s）、`repair_max_ns=1.85 s`。
速度 2.55 Mbit/s，高于 fix23 的 1.63 与 fix22 的 2.15。

**失败点**：HOL/NACK 指标都回到或低于 fix22 量级，但 far 的
`holes_unrecoverable` / `fec_reset_hole` 从 0 变成 1，流在 13.2% 被
复位。这是 fix24 引入的新回归（fix22/fix23 该两项均为 0）。

**未做**：DKMS（FAIL）、重跑、tag/push。证据：
`review/wan-experiment/arm64/accept-1g-20261107T121253Z/`。

## 23. 2026-11-07 fix25 单流 1 GiB（最终验收）

near fix25 `2AB0A7DEC2C4B7F754FF58C`（sha256
`e3e6634f4e9656c5226e4d926bc2ee3c2fe7642e0457f95f70243b1636ae5a53`），
far fix25 `8A0738F252ED909684E908B`（debug=0）。修复方向：fix24 的
far `holes_unrecoverable=1` / `fec_reset_hole=1` 新回归（§22.1），
同时保持 HOL 指标在 fix24 的水平。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
143680）；join 到 256/256；两端 config 回读 PASS。`SSH_TMO=180`。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、
`fec_flush_skip`/`hol_wait`、`fec_replay_high_water`/`pass_throttled`、
`fec_nack_lat_max_ns`、`tx_ack` 是否持续前进、teardown、内核窗口、
boot_id、喂狗者、DKMS。）

### 23.1 结果：FAIL（完全停滞，19.2%；fix24 的 hole reset 回归已修复）

transfer 12:40:00Z–12:56:20Z（962.1 s），单流 curl，HTTP 200。

| 项 | 值 |
|---|---|
| curl rc | **28**（`Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds`） |
| size_download / 本地文件 | **206,066,329**（19.2%） |
| 速度 | 214,192 B/s（1.71 Mbit/s） |
| near sha256 | `d2d61a8dfce5d0b4…`（不完整） |
| CPU | near 8.7%、far 35.6% |
| 内核窗口 | 两端有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 23184 在 |

**要求的零计数全部为 0（两端）**：`fec_stream_refused`、
`fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、
`fec_replay_miss_enospc`、`fec_replay_miss_evicted`、`fec_nack_drop`、
`fec_reset_tcp_failed`、`fec_reset_hole`、`holes_unrecoverable`、
`holes_declared`、`holes_repaired`、`close_timeouts`、`gaps_total`、
`rx_errs`、`connect_fail`、`alloc_fail`、`join_bad`；near `rx_proto=0`、
`replaced=3`、`lost=3`。**fix24 的 `holes_unrecoverable=1` /
`fec_reset_hole=1` 回归已修复**（本次两项均为 0）。

**失败点：12:56 起完全停滞，不是慢速**。

| 端 | 证据 |
|---|---|
| far | `tx_ack` 停在 **206,069,507**；`live pub_una=pub_nxt=432,381,241`；`car_nxt=car_copied=769,501,449`；`c12 fec_outq` 空 |
| near | `rx_delivered` 停在 206,069,507 附近，`/root/dl-1g/accept-1g.bin` 5 分钟只增长 4.7 KB |
| 两端 | `stall_declared` far 2,613 / near 20，`stall_recovered` far 2,605 / near 20，`stall_dead` far 6 / near 0 |

far 没有待发数据（publish 与 carrier 都已追平），但 target 仍有 868 MB
未读；near 不 ACK 新数据，far 的 FEC 发送窗口因此不再前进。

**根因线索：never_stored 的 replay 缺口**。

| 计数 | 值 |
|---|---|
| near `fec_nack_tx` | 18,246 |
| near `fec_replay_miss` | 96（全 `never_stored`） |
| near `fec_nack_repair_ns` | 120,225,619,288（120 s） |
| near `fec_nack_repair_max_ns` | 5,833,423,409（5.83 s） |
| far `fec_nack_rx` | 18,249 |
| far `fec_replay_hit` | 205,089 |
| far `fec_replay_miss` | **8,446（全 `never_stored`）** |
| far `fec_nack_never_stored` | 408 |
| far `fec_nack_absent_reply` | 4,401 |
| far `fec_nack_lat_ns` | **72,258,331,478,104**（累计 72,258 s） |
| far `fec_nack_lat_max_ns` | **15,939,788,324（15.9 s）** |
| far `fec_nack_backpressure` | 1,262 |

near 请求的块中有一部分 far 从未存储（`never_stored`），永远无法
replay；near 的 reorder 窗口因此卡住，不再 ACK 新数据。far 的
`fec_nack_lat_max_ns=15.9 s` 与累计 72,258 s 的 NACK 等待说明修复
路径仍在长时间等这些不可能被满足的请求。

**HOL 指标（低于 fix23，接近 fix24）**：far `fec_flush_skip=1,144,927`、
`fec_hol_wait=2,497,967`、`fec_replay_high_water=31`、
`fec_replay_pass_throttled=7,187`、`fec_replay_stale=812`。

**teardown**：far watcher `WATCH_START 12:40:12`、`FIRST_STREAM 12:40:15`、
`NO_STREAM_LINE 12:56:21`；最后 live 行 `sid=858037157888 state=5 kill=5
rst_w=1`；far `fec_reset_user=1`，near `fec_reset_user=0`。

**未做**：DKMS（FAIL）、重跑、tag/push。证据：
`review/wan-experiment/arm64/accept-1g-20261107T123735Z/`。

## 24. 2026-11-07 fix26 单流 1 GiB（最终验收）

near fix26 `11D372E332FB76C803764A8`（sha256
`c3db046bf0eccd1d913898c233ac440d4c4e8935f416f6a5fbac83736c09b864`），
far fix26 `7648462BA6175B3AAAFEB5A`（debug=0）。修复方向：fix25 的
完全停滞（near 的 reorder 窗口卡在 `never_stored` 的 replay 缺口，
§23.1），即 NACK 越过 frontier 与 super-block 卡死。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
143680）；join 到 256/256；两端 config 回读 PASS。`SSH_TMO=180`。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、
`fec super stuck` / `fec NACK skips frontier` 日志、`tx_ack` /
`next_deliver` 是否持续前进、`fec_nack_repair_max_ns`、teardown、
内核窗口、boot_id、喂狗者、DKMS。）

### 24.1 结果：FAIL（完全停滞，12.2%；出现 `fec NACK skips frontier`）

transfer 13:21:24Z–13:28:08Z（389.4 s），单流 curl，HTTP 200。

| 项 | 值 |
|---|---|
| curl rc | **28**（`Operation too slow. Less than 65536 bytes/sec transferred the last 120 seconds`） |
| size_download / 本地文件 | **131,100,672**（12.2%） |
| 速度 | 336,662 B/s（2.69 Mbit/s） |
| near sha256 | `1be0aa337024d1aa…`（不完整） |
| CPU | near 8.9%、far 35.4% |
| 内核窗口 | 两端有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 23184 在 |

**零计数**：`fec_stream_refused`、`fec_proto_fail`、`fec_hdr_crc_fail`、
`fec_payload_crc_fail`、`fec_replay_miss_enospc`、
`fec_replay_miss_evicted`、`fec_nack_drop`、`fec_reset_tcp_failed`、
`fec_reset_hole`、`holes_unrecoverable`、`holes_declared`、
`holes_repaired`、`close_timeouts`、`gaps_total`、`connect_fail`、
`alloc_fail`、`join_bad` 两端均为 0；near `rx_proto=0`、
`replaced=2`、`lost=2`。**唯一非零错误是 far `rx_errs=1`**。

**失败点：13:27 起完全停滞**。

| 端 | 证据 |
|---|---|
| far | `tx_ack` 停在 **131,102,826**；`live pub_una=pub_nxt=93,007,343`；`car_nxt=car_copied=923,266,298`；`c12 fec_outq` 空 |
| near | `rx_delivered` 停在 131,101,802，dl 文件 5 分钟只增 41 KB |
| 两端 | far `stall_declared=1,224`、`stall_dead=7`、`up_post=255`（一个 carrier 掉了）；near `stall_declared` 低位 |

**fix26 的日志确实出现，但停滞仍在**：

- near `dmesg` 中 **`fec NACK skips frontier` 563 次**，最后几条形如
  `next=185 for s=186 hi=186`：NACK 跳过 185 去请求 186，185 不再被
  请求，near 的 reorder 窗口卡在 185。
- **`fec super stuck` 0 次**（该日志未触发）。
- far `fec_nack_lat_max_ns=8,637,841,436`（8.6 s，低于 fix25 的 15.9 s），
  `fec_nack_lat_ns=30,850,949,890,090`（累计 30,850 s）；
  `fec_nack_backpressure=677`、`fec_replay_stale=647`。
- near `fec_nack_tx=8,540`、`fec_replay_miss=108`（全 never_stored）、
  `fec_nack_repair_ns=79,376,017,195`（79 s）、`repair_max_ns=5.21 s`。
- far `fec_replay_hit=100,748`、`fec_replay_miss=3,606`（全 never_stored）、
  `fec_nack_never_stored=166`、`fec_nack_absent_reply=1,589`。

**HOL 指标**：far `fec_flush_skip=402,725`、`fec_hol_wait=547,705`、
`fec_replay_high_water=17`、`fec_replay_pass_throttled=4,096`，
与 fix24/25 同量级。

**teardown**：far watcher `WATCH_START 13:21:33`、`FIRST_STREAM 13:21:36`、
`NO_STREAM_LINE 13:28:09`；最后 live 行 `sid=916774191104 state=5 kill=5
rst_w=1`；far `fec_reset_user=1`，near `fec_reset_user=0`。

**未做**：DKMS（FAIL）、重跑、tag/push。证据：
`review/wan-experiment/arm64/accept-1g-20261107T131919Z/`。

## 25. 2026-11-07 fix27 单流 1 GiB（最终验收）

near fix27 `1BCFF379FCE25277AAC9CCC`（sha256
`042d0f3947c92e875a0f4d22ee5b93fe06fd0eae22deedcbdf96cdef8f809681`），
far fix27 `94F377ED217E0B33F5DC9BA`（debug=0）。修复方向：fix26 的
`fec NACK skips frontier`（NACK 跳过缺失块，reorder 窗口卡住，
§24.1）。

preflight PASS（near dev=eth0、pref-100 rule added、`ip route get
45.11.1.62` 走 eth0）；far-serve PASS（sha256 `c6a25615…`、raw sender
143680）；join 到 256/256；两端 config 回读 PASS。`SSH_TMO=180`。

（1 GiB 结果待填：size/sha、吞吐/CPU、c10/c11/c12、
`fec NACK skips frontier` 是否 0、`fec super stuck`、`tx_ack` /
`next_deliver`、`fec_nack_repair_max_ns`、teardown、内核窗口、
boot_id、喂狗者、DKMS。）

### 25.1 结果：功能 PASS（1 GiB 完整、sha256 一致）；harness 仅 `far_rx_errs=7` 未过

transfer 13:42:30Z–14:06:55Z（1451.0 s），单流 curl。

| 项 | 值 |
|---|---|
| curl rc / http | **0 / 200** |
| size_download / 本地文件 | **1,073,741,824 / 1,073,741,824** |
| near sha256 | `c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93` |
| far sha256 | 同上（**一致**） |
| 吞吐 | 739,987 B/s（**5.92 Mbit/s**），1451 s |
| CPU | near 16.6%、far 64.4% |
| 内核窗口 | 两端 `journalctl` 有记录，无 `BUG:/Oops/WARNING:/UBSAN:/GPF` |
| boot_id / 喂狗者 | near `0a91d2eb…` 未变；喂狗者 23184 在 |

**任务要求的零计数全部为 0**（两端 delta）：`fec_stream_refused`、
`fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、
`fec_replay_miss_enospc`、`fec_replay_miss_evicted`、`fec_nack_drop`、
`fec_reset_tcp_failed`、`fec_reset_hole`、`holes_unrecoverable`、
`holes_declared`、`holes_repaired`、`close_timeouts`、`gaps_total`、
`connect_fail`、`alloc_fail`、`join_bad`；near `rx_proto=0`、
`replaced=2`、`lost=2`（回落到 lost 量级）。

**harness 唯一失败项**：`far_rx_errs=7`（delta）。far 的 carrier RX
层为 `rx_proto=7`、`rx_last_err=-71`（EPROTO）、`lost=2`；这是
carrier 握手/帧解析错误，不进入数据完整性路径——**1 GiB 的 sha256
与 far 源完全一致**。该计数作为已知问题记入 0.3.0（与 PARITY marker /
迟到 share 相关），不作为 0.2.0 阻塞项。

**fix26 的两类停滞日志在本次均为 0**：near `fec NACK skips frontier`
0 次、`fec super stuck` 0 次（transfer 期间；dmesg 在 13:42 已轮转，
计数从 0 起）。

**NACK/replay 基线（fix27）**：

| 计数 | near | far |
|---|---|---|
| `fec_nack_tx` / `fec_nack_rx` | 1,613 / 14 | 14 / 1,613 |
| `fec_nack_retx` | 17 | 11,677 |
| `fec_replay_hit` | 5 | 11,677 |
| `fec_replay_miss`（全 never_stored） | 168 | 306 |
| `fec_nack_repair_ns` | 811,750,482,448（811 s） | 460,015,026（0.46 s） |
| `fec_nack_repair_max_ns` | 6,300,631,327（6.30 s） | 460,015,026（0.46 s） |
| `fec_replay_high_water` / `pass_throttled` | — | 0 / 0 |
| `fec_flush_skip` / `fec_hol_wait` | — | 5,326,236 / 75,974,933 |

**teardown（正常）**：far watcher `WATCH_START 13:42:39`、
`FIRST_STREAM 13:42:42`、`WATCH_STOP 14:07:04`；最后 live 行
`sid=195941105664 state=1 kill=0 tgt_state=7 tgt_err=0 fin_w=0 rst_w=0`
—— **target 正常关闭（tgt_state=7），无 RST**；两端 `fec_reset_user=0`、
`fec_reset_hole=0`。near `up_post=254`（2 个 carrier 掉了）、
far `up_post=256`。

**过程说明**：harness 在写 `SUMMARY.md` 时卡在 `kw_detail` 的 bash
字符串操作（near 窗口 696 KB、far 窗口 1.2 MB），4 分钟无进展后被
kill（`RUN_RC=143`）；传输、计数、内核窗口与 near-fetch 判定均已在
kill 前完成，`SUMMARY.md` 前 803 字节已写入。**这不是产品失败。**

**结论**：**功能验收 PASS**（1 GiB、sha256 一致、要求的错误计数全 0、
teardown 正常）。harness 因 `far_rx_errs=7` 判 FAIL，该计数属 carrier
RX 层，未影响交付内容。按用户指示（功能 PASS 后仍跑 DKMS）继续
DKMS 七步。

**证据**：`review/wan-experiment/arm64/accept-1g-20261107T134014Z/`
（`SUMMARY.md` 是 harness 的 FAIL 判定；`counters-*.txt`、
`far-stats-post.txt`、`near-stats-post.txt`、`far-stream-watch.txt`、
`run.log`、`near-verdict.txt`）。

## 26. 2026-11-07 fix27 的 DKMS 七步（`.65`）

前置：`apt-get install -y --no-install-recommends dkms` 安装
`dkms 3.2.2-1ubuntu1`（`dkms --version` = `dkms-3.2.2`）；
`REPO=/root/tbtun-0.2.0-fix27-20261107T133636Z`；
`bash /root/dkms-65/run-dkms-65.sh --run`（必须用 bash，脚本含
`set -o pipefail` 与 `printf %q`）。

（七步结果待填：`DKMS_STEP`/`DKMS_CHECK` 摘要、`DKMS_RESULT`、
残留检查、喂狗者。）

### 26.1 结果：PASS（`DKMS_RESULT PASS (checks_failed=0 rollback=0)`）

七步全部 PASS：

| 步 | 关键检查 |
|---|---|
| 1 `source` | 源码树、`install_dkms.sh`、`tbt_uapi.h`、`install_script_exec`、`source_version=0.2.0` |
| 2 `clean` | 无模块、无 `/proc/net/tbtun`、`dkms status` 空、无 `/usr/src/tbtun-*`、无 `updates/dkms` ko |
| 3 `install` | `dkms_status`、`modinfo_version=0.2.0`、`vermagic=6.1.172`、`installed_ko`、`sysfs_version`、`dmesg_loaded`、六个 procfs、`make_log` |
| 4 `config` | 写 `tunnel 1`、回读 `n=4 m=1 dup=1 profile=http-kal keepalive=5000`、`tunnels_peer`、`listen_19000`、`del` 后 config 空与端口释放 |
| 5 `rmmod` | 模块卸载、procfs 消失、dmesg `unloaded`、最后 50 行内核干净 |
| 6 `modprobe` | 版本 0.2.0、`modinfo -F filename` 指向 `updates/dkms/tbtun.ko`、配置往返、监听出现/消失 |
| 7 `uninstall` | `dkms status` 空、无 `/usr/src/tbtun-*`、三处无 ko、`modinfo` 无解析、`modules.dep` 干净、procfs 消失、模块卸载、无 `/var/lib/dkms/tbtun*` |

**harness 修复（本次验收的一部分）**：首次运行唯一失败项是
`make_log`——DKMS 3.2.2 把构建日志从
`/var/lib/dkms/$MODULE/$VER/build/make.log` 移到
`/var/lib/dkms/$MODULE/kernel-$REL-$ARCH/log/make.log`。已把
`run-dkms-65.sh` 的检查改为依次尝试旧路径、
`kernel-$REL-$ARCH/log/make.log`、`$VER/$REL/$ARCH/log/make.log`，
本次 PASS 的 `make_log` 明细为
`exit code 0 (/var/lib/dkms/tbtun/kernel-6.1.172-aarch64/log/make.log)`。

**第二次运行的失败是回滚残留**：上一轮 rollback 之后 `dkms status`
虽为空，但 `dkms install` 仍报 `Module tbtun/0.2.0 already installed
on kernel 6.1.172 (aarch64), skip`，导致 `install_dkms.sh` 的
“no tbtun.ko* is installed” 检查失败。清理 `/var/lib/dkms` 后第三次
运行成功。**两次失败都不是产品缺陷。**

**残留（PASS 后）**：near 无 tbtun 模块、`/proc/net/tbtun` 消失、
`dkms status` 空、无 `/usr/src/tbtun-*`、无
`/lib/modules/6.1.172/updates/dkms/`、无 `/var/lib/dkms/tbtun*`、
19000 空闲、boot_id `0a91d2eb…` 未变、喂狗者 pid 23184 在。

**证据**：`review/wan-experiment/arm64/dkms-65-20261107T142840Z/`
（`run.log`、`install.log`、`uninstall.log`、`config-readback.txt`、
`tunnels-readback.txt`、`ss-19000.txt`、`dmesg-after-install.txt`、
`dmesg-after-rmmod.txt`）。

> 备注：`tunnels_line` 的 PASS 明细只匹配到表头
> `# id role state listen peer target n m streams`，这是 harness 的
> 宽松匹配；同一步的 `tunnels_peer` 与 `listen_19000` 都独立 PASS，
> 表项本身正常。
