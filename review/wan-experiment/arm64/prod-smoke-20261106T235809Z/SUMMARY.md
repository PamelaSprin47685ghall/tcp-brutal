# prod-smoke-10s 生产冒烟报告

- 实验 UTC：20261106T235809Z
- 退出码：0
- 判定：通过：256 carriers join 成功，唯一一轮 1 MiB / 5s burst 通过，near 存活，关键计数器为零。
- near：reachable；far：reachable；证据：complete

## 配置

```
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 nonce=00112233445566778899aabbccddeeff
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 nonce=00112233445566778899aabbccddeeff
```

- carriers=256，n=4，m=4，groups=32（派生量，不写入 config），w=8，fec=on mode=grouped
- near 端口块：30000-30511；far 端口块：31000-31511
- MIB=1；SECS=5（硬上限 10）；PROCS=1；join 超时 180s；burst 超时 135s
- 触发：恰好一轮 tbt_burst 127.0.0.1 9001 1 5 interleaved（不重试）
- 256 验收形状：n=4 m=4，groups=32，w=8，fec=on mode=grouped；内核 config 无 groups= 键。

## 模块身份与 max_carriers

| 项 | near | far |
|---|---|---|
| staged tree | `/root/tbtun-prod-20261106T235122Z` | `/home/tbtun-t52-arm64` |
| tree commit | `unknown` | — |
| ko sha256 | `9e028e1dd66a14a5202316b41f140a3bfa424ed4fa7bc4d4550b4170ca5d0ba2` | `cb366613307cffbc51d45bf128a074ce9a9adc5ec4ecd5532cdf04984c5c941f` |
| ko srcversion | `EAA11C59413AA48F66150DD` | `9B02B3348DE07FFF548D862` |
| loaded srcversion | `EAA11C59413AA48F66150DD` | `9B02B3348DE07FFF548D862` |
| expected srcversion | `EAA11C59413AA48F66150DD`（source=auto-ko） | `9B02B3348DE07FFF548D862`（source=auto-ko） |
| identity match | `yes` | `yes` |
| expected sha256 | `9e028e1dd66a14a5202316b41f140a3bfa424ed4fa7bc4d4550b4170ca5d0ba2` | — |
| sha256 gate | `pass` | — |
| far known bad srcversions | — | `846D47A623A8198946CBA64` |
| ko vermagic | `6.1.172 SMP mod_unload modversions aarch64` | `7.0.0-42-generic SMP preempt mod_unload modversions ` |
| loaded max_carriers | `256`（insmod 后） | `256`（预检） |
| max_tunnels / debug | `1` / `0` | `1` / `0` |
| insmod | `max_carriers=256 max_tunnels=1 debug=0` | 已加载（far 不重新 insmod） |

## 前置检查、可见性与配置读回闸门

| 项 | 结果 |
|---|---|
| 两端 ssh | pass |
| near 未加载 | pass |
| near /sys/fs/pstore 为空 | yes |
| near ramoops | yes |
| near panic_on_oops / panic | 1 / 10 |
| near 额外 sysctl | oops_limit=2 panic_on_rcu_stall=1 oops_all_cpu_backtrace=1 |
| near tbtun-kmsg-relay | active |
| near tbtun-sampler | active（baseline lines=49200，end=51727） |
| .80 UDP 6666 接收端 | pid=599898 log=/home/kunweiz/tbtun-forensics/netconsole-6666.log |
| .80 tbtun-netconsole-rx | active-user（行基线=5780，结束=5783） |
| kmsg 探针 | 1 |
| far tbtun 已加载 | pass |
| far srcversion | `9B02B3348DE07FFF548D862`（expected `9B02B3348DE07FFF548D862`） |
| far max_carriers | `256`（要求 256） |
| far 当前 config carriers | `256`（步骤 4 重写） |
| far config 读回 carriers | `256` |
| near config 读回 carriers | `256` |
| far config / listeners | pass / pass |

## 步骤、预期结果与失败分支

| # | 步骤 | 预期结果 | 失败分支 | 实际 |
|---|---|---|---|---|
| 1 | 参数校验 | CARRIERS 在 8/16/32/64/128/256；SECS<=10；PROCS<=32 | 非法：退出 2 | carriers=256 mib=1 secs=5 procs=1 |
| 2 | 前置检查 | 两端 ssh、near 未加载、far 已加载且 srcversion/max_carriers 匹配、可见性 active、pstore 空、模块身份匹配 | 任一失败：退出 3，不加载模块、不写配置 | pass |
| 3 | 锁与负载 | 两端 flock 获取；load1 只记录 | 锁忙：退出 4 | locks_held=1 near_load=0.05 far_load=0.15 |
| 4 | far 配置 | ports 31000-31511、config 逐字读回，carriers=256、n=4、m=4、w=8、fec=on、mode=grouped、http-kal，9000/5202 监听 | 写失败/读回不一致/监听未起：退出 10，near 不加载 | pass |
| 5 | near 配置 | ip rule、rmem/wmem、insmod max_carriers=256、加载后 max_carriers=256、srcversion=EAA11C59413AA48F66150DD、ports 30000-30511、config 逐字读回 carriers=256 | 任一步失败：退出 10，清理 | pass |
| 6 | join gate | 180s 内两端 c3 up=256；记录 c3 n、c9 错误计数、http_parse_err、rx_errs、reorder_overflow | 超时或快照 up 不一致：退出 5，不触发 burst，收集证据 | JOIN_OK=1 near_up=256 near_n=256 far_up=256 far_n=256 counter_warn=0 |
| 7 | 触发 | 恰好一轮 1 MiB / 5s interleaved，135s 内返回 | 非 0/超时：记录，不重试；near 失联则转证据收集 | attempted=1 procs=1 ssh_rc=0 burst_rc=0 wall_ms=743 |
| 8 | 失联判定 | 记录 ping/ARP/ssh、boot_id、是否重启 | 无法判定：记 unknown，继续 far/netconsole 证据 | near_lost=0 lost_at=none returned_at=none rebooted=0 |
| 9 | 证据收集 | netconsole、pstore、journal、dmesg、stats、far dmesg/stats、join 轨迹、c9 检查全部落盘 | 单项失败：记录原因，不阻塞其他项 | complete |
| 10 | 清理 | near rmmod/rule/sysctl、far echo/rmmod、锁释放；失联时跳过模块/网络清理 | 清理失败退出 9 | clean_fail=0 skipped=no |

## 关键计数器

| 项 | near | far |
|---|---|---|
| c3 up（join 末次） | 256 | 256 |
| c3 up（post） | 256 | 256 |
| c3 n（join 末次） | 256 | 256 |
| c3 n（post） | 256 | 256 |
| c3 data_tx | 0 | 0 |
| c3 rx_ack | 0 | 0 |
| c3 reorder_overflow | 0 | 0 |
| c8 http_parse_err | 0 | 0 |
| rx_errs | 0 | 0 |
| c9 错误计数全零 | c9_all_error_zero=yes | c9_all_error_zero=yes |

## 崩溃证据计数

- netconsole 窗口内 `Internal error: Oops`：0
- netconsole 窗口内 `Kernel panic`：0
- near journal 内 Oops/panic：0
- near pstore 内 Oops/panic：0
- sampler 内 Oops/panic：0
- netconsole 窗口：行 5780 -> 5783（502204 -> 502564 bytes）
- 注意：kmsg-relay/sampler 是用户态进程；panic 后可能停止，pstore 是主证据。

## join 轮询轨迹（末尾 60 行）

```
2026-11-06T23:58:29Z near_up=0 near_n=256 far_up=16 far_n=256
2026-11-06T23:58:32Z near_up=65 near_n=256 far_up=86 far_n=256
2026-11-06T23:58:35Z near_up=141 near_n=256 far_up=151 far_n=256
2026-11-06T23:58:37Z near_up=212 near_n=256 far_up=221 far_n=256
2026-11-06T23:58:40Z near_up=256 near_n=256 far_up=256 far_n=256
```

## 时间线（末尾 50 行）

```
2026-11-06T23:58:09Z	1794009489	start | prod-smoke carriers=256 mib=1 secs=5 procs=1; evidence=/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/prod-smoke-20261106T235809Z
2026-11-06T23:58:10Z	1794009490	precheck_ssh | near=aarch64/6.1.172 far=7.0.0-42-generic boot_id=0a91d2eb-3122-41eb-913c-024a4e1fe724
2026-11-06T23:58:11Z	1794009491	load | near=0.05 far=0.15 limit=4
2026-11-06T23:58:11Z	1794009491	precheck_visibility | panic_on_oops=1 panic=10 oops_limit=2 panic_on_rcu_stall=1 oops_all_cpu_backtrace=1 pstore_empty=yes ramoops=yes
2026-11-06T23:58:14Z	1794009494	precheck_far | srcversion=9B02B3348DE07FFF548D862 expected=9B02B3348DE07FFF548D862 source=auto-ko staged=9B02B3348DE07FFF548D862 max_carriers=256
2026-11-06T23:58:15Z	1794009495	precheck_receiver | pid=599898 log=/home/kunweiz/tbtun-forensics/netconsole-6666.log lines=5780 relay=active sampler=active rx=active-user
2026-11-06T23:58:18Z	1794009498	precheck_far_state | pre_carriers=256 echo_sha=4501aafe898f755ae01ac7075f6548974349be634993b129a0a94bc7a38343a7
2026-11-06T23:58:18Z	1794009498	precheck_module | ko=/root/tbtun-prod-20261106T235122Z/tbtun.ko srcversion=EAA11C59413AA48F66150DD expected=EAA11C59413AA48F66150DD source=auto-ko sha=9e028e1dd66a14a5202316b41f140a3bfa424ed4fa7bc4d4550b4170ca5d0ba2 sha_gate=pass identity=match
2026-11-06T23:58:20Z	1794009500	precheck_done | burst_sha=afa13883aa79f587e7bd667cddd8637e6eb7dab1747ccdb66fbd7d707d49a740
2026-11-06T23:58:20Z	1794009500	far_config_start
2026-11-06T23:58:28Z	1794009508	far_config_done | ports=31000-31511 config=exact carriers=256 echo=5202
2026-11-06T23:58:28Z	1794009508	near_config_start
2026-11-06T23:58:28Z	1794009508	route | dev=eth0
2026-11-06T23:58:28Z	1794009508	near_config_done | ports=30000-30511 config=exact carriers=256 max_carriers=256
2026-11-06T23:58:28Z	1794009508	join_start | timeout=180s target=up=256
2026-11-06T23:58:40Z	1794009520	join_ok | near_up=256 far_up=256
2026-11-06T23:58:42Z	1794009522	burst_start | procs=1 tbt_burst 127.0.0.1 9001 1 5 interleaved timeout=135s
2026-11-06T23:58:43Z	1794009523	burst_end | ssh_rc=0 rcs=0  burst_rc=0 wall_ms=743 at=2026-11-06T23:58:43Z
2026-11-06T23:58:43Z	1794009523	netconsole_snapshot | start=502204 end=502564
2026-11-06T23:58:51Z	1794009531	far_evidence_done
2026-11-06T23:58:57Z	1794009537	evidence_done | near_lost=0 rebooted=0
2026-11-06T23:58:57Z	1794009537	end | final_rc=0
2026-11-06T23:58:57Z	1794009537	cleanup_start | exit_rc=0
2026-11-06T23:59:04Z	1794009544	cleanup_done | clean_fail=0
```

## 证据清单

| 文件 | 字节 |
|---|---|
| `burst-1.log` | 199 |
| `burst-1.rc` | 23 |
| `burst-build.log` | 0 |
| `burst.txt` | 238 |
| `c9-far-joined.txt` | 342 |
| `c9-far-post.txt` | 342 |
| `c9-near-joined.txt` | 342 |
| `c9-near-post.txt` | 342 |
| `cleanup.txt` | 192 |
| `config-far.txt` | 388 |
| `config-near.txt` | 390 |
| `counters-joined.txt` | 504 |
| `counters-post.txt` | 504 |
| `far-dmesg.log` | 335663 |
| `far-journal.log` | 519786 |
| `far-meta.txt` | 180 |
| `far-pre-config.txt` | 388 |
| `far-ss.txt` | 49570 |
| `far-stats.txt` | 31920 |
| `joined-far.txt` | 339096 |
| `joined-near.txt` | 211309 |
| `join-probes.txt` | 320 |
| `module.txt` | 1462 |
| `near-dmesg.log` | 143821 |
| `near-journal-b0.log` | 258 |
| `near-journal-b1.log` | 17 |
| `near-journal.log` | 258 |
| `near-meta.txt` | 1200 |
| `near-probes.txt` | 218 |
| `near-pstore.txt` | 243 |
| `near-ss.txt` | 31286 |
| `near-stats.txt` | 30926 |
| `near-tbtun-last.log` | 3555676 |
| `netconsole-crash-lines.txt` | 129 |
| `netconsole-fds.txt` | 219 |
| `netconsole.log` | 502564 |
| `netconsole-window.log` | 360 |
| `post-far.txt` | 338974 |
| `post-near.txt` | 212054 |
| `pre-far.txt` | 283310 |
| `pre-near.txt` | 152539 |
| `route.txt` | 77 |
| `run.log` | 515 |
| `stats.txt` | 717 |
| `SUMMARY.md` | 9413 |
| `timeline.txt` | 2531 |

## 清理

```
=== cleanup 2026-11-06T23:58:57Z ===
near_reachable=yes near_rebooted=0 far_reachable=yes
near_rmmod=ok
near_rule=deleted
near_sysctl=restored
far_echo=stopped
far_rmmod=ok
locks_released=yes
```

## 结论与限制

- 通过：256 carriers join 成功，唯一一轮 1 MiB / 5s burst 通过，near 存活，关键计数器为零。
- 本次只触发一轮；无论结果如何都不重试。
- 若 near 失联：先保存本目录，再按只读取证流程分析；不要重新加载模块。
- 若 near 失联但无 Oops：netconsole 可能未送达 panic 输出，pstore/journal 也可能为空；结论只能写“未定”。
- groups=32 是 FEC 派生量（carriers/(n+m)），不是内核 config 键；config 中只有 n/m/w/carriers。
