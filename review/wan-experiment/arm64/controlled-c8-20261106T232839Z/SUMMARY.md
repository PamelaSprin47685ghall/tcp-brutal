# controlled-c8-once 实验报告

- 实验 UTC：20261106T232839Z
- 退出码：0
- 判定：修复有效：join 成功，单次 1 MiB burst 通过，near 存活。
- near：reachable；far：reachable；证据：complete

## 实验配置

```
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=8 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30015 nonce=00112233445566778899aabbccddeeff
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped w=8 carriers=8 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31015 nonce=00112233445566778899aabbccddeeff
```

- near 端口块：30000-30015；far 端口块：31000-31015；carriers=8；join 超时 60s；burst 超时 120s
- 触发：恰好一次 tbt_burst 127.0.0.1 9001 1 5 interleaved（不重试）
- 选择理由：与事故的 1 MiB / 5 s 触发逐字一致；tbt_ping 37B 只验证 join，覆盖不足。

## 模块身份

| 项 | near | far |
|---|---|---|
| staged tree | `/root/tbtun-fixed-20261106T231212Z` | `/home/tbtun-t52-arm64` |
| tree commit | `unknown` | — |
| ko sha256 | `1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92` | `49d632d37a34660e906d9f5b8919d9f7895c4912a8af9f36a7b55873ddfbc0f4` |
| ko srcversion | `72168C1CE3E3378A758DAFF` | `846D47A623A8198946CBA64` |
| loaded srcversion | `72168C1CE3E3378A758DAFF` | `332759AF31AB0DD875E0EE7` |
| vermagic | `6.1.172 SMP mod_unload modversions aarch64` | `7.0.0-42-generic SMP preempt mod_unload modversions ` |
| expected srcversion | `72168C1CE3E3378A758DAFF` | `332759AF31AB0DD875E0EE7` |
| expected sha256 | `1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92` | — |
| expected vermagic | `6.1.172 SMP mod_unload modversions aarch64` | kernel `7.0.0-42-generic` |
| identity match | `yes` | `yes` |
| ko mtime (epoch) | — | `1794004781` |
| insmod | `max_carriers=256 max_tunnels=1 debug=0` | 已由 deploy_hnd0.sh 加载 |

## 前置检查与可见性

| 项 | 结果 |
|---|---|
| 两端 ssh | pass |
| .65 kernel.panic_on_oops=1 | 1 |
| .65 kernel.panic=10 | 10 |
| .65 额外 sysctl | oops_limit=2 panic_on_rcu_stall=1 oops_all_cpu_backtrace=1 |
| .65 /sys/fs/pstore 为空 | yes |
| .65 ramoops 已注册 | yes |
| .65 tbtun-kmsg-relay | active |
| .65 tbtun-sampler | active（baseline lines=20917） |
| .80 UDP 6666 接收端 | pid=599898 log=/home/kunweiz/tbtun-forensics/netconsole-6666.log |
| .80 tbtun-netconsole-rx | active-user（日志行基线=5520，结束=5773） |
| kmsg 探针 | 1 |
| far tbtun 已加载 | pass |
| far fixed srcversion | 332759AF31AB0DD875E0EE7（expected 332759AF31AB0DD875E0EE7，match=yes） |
| far c8 config / listeners | pass / pass |
| near tbtun 未加载 | pass |

## 步骤、预期结果与失败分支

| # | 步骤 | 预期结果 | 失败分支 | 实际 |
|---|---|---|---|---|
| 1 | 前置检查 | 两端 ssh、near panic/ramoops、可见性服务、far 已加载且 srcversion 为修复版、near 未加载、near 模块身份逐项匹配 | 任一失败：退出 3，不加载模块、不写配置；far 不匹配时停止、不跑 near | pass |
| 2 | 锁与负载 | 两端 flock 获取，load1 <= 4 | 锁忙/负载高：退出 4，不跑 | locks_held=yes |
| 3 | 可见性检查 | .65 relay/sampler active，.80 接收端 active，kmsg 探针在接收文件落盘 | 任一服务 inactive 或探针丢失：退出 3，不跑 | probe=1 relay=active sampler=active rx=active-user |
| 4 | far 配置 | far fixed srcversion 已匹配；ports 31000-31015、c8 config（carriers=8/n=4/m=4/fec=on/grouped/w=8/crypto=on/http-kal）逐字读回，9000/5202 监听 | 写失败/读回不一致/监听未起：退出 10，near 不加载 | pass |
| 5 | near 配置 | ip rule、rmem/wmem、insmod、加载后 srcversion=72168C1CE3E3378A758DAFF、ports 30000-30015、config 逐字读回 | 任一步失败：退出 10，清理 | pass |
| 6 | join gate | 60s 内两端 up=8；parse/mismatch/FEC 计数器记录但不阻塞 | 超时或快照 up 不一致：退出 5，不触发 burst，收集证据 | JOIN_OK=1 near_up=8 far_up=8 counter_warn=0 |
| 7 | 触发 | 恰好一次 1 MiB interleaved burst，120s 内返回 | 非 0/超时：记录，不重试；near 失联则转证据收集 | attempted=1 ssh_rc=0 burst_rc=0 wall_ms=1498 |
| 8 | 失联判定 | 记录 ping/ARP/ssh、失联与恢复时刻、是否重启 | 无法判定：记录 unknown，继续 far/netconsole 证据 | near_lost=0 lost_at=none returned_at=none rebooted=0 |
| 9 | 证据收集 | netconsole、pstore、journal -b -1 --since、sampler、dmesg、stats、far dmesg/ss 全部落盘 | 单项失败：记录原因，不阻塞其他项；无更优先判定时退出 8 | complete |
| 10 | 清理 | near rmmod/rule/sysctl、far rmmod/echo、锁释放 | 失联跳过并记录；清理失败退出 9 | clean_fail=0 |

## 崩溃证据计数

- netconsole 窗口内 `Internal error: Oops`：0
- netconsole 窗口内 `Kernel panic`：0
- near journal 内 Oops/panic：0
- netconsole 窗口：行 5520 -> 5773（451213 -> 500530 bytes）
- sampler：行 20917 -> 22866
- pstore：见 `near-pstore.txt`（同时收集 /sys/fs/pstore 与 systemd-pstore 归档）
- 注意：kmsg-relay/sampler 是用户态进程；panic 后可能停止，pstore 是主证据。

## 时间线

```
2026-11-06T23:28:39Z	1794007719	start | single-shot c8; evidence=/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/controlled-c8-20261106T232839Z
2026-11-06T23:28:40Z	1794007720	precheck_ssh | near=aarch64/6.1.172 far=7.0.0-42-generic boot_id=0a91d2eb-3122-41eb-913c-024a4e1fe724
2026-11-06T23:28:41Z	1794007721	load | near=0.15 far=0.29 limit=4
2026-11-06T23:28:41Z	1794007721	precheck_visibility | panic_on_oops=1 panic=10 oops_limit=2 panic_on_rcu_stall=1 oops_all_cpu_backtrace=1 pstore_empty=yes ramoops=yes
2026-11-06T23:28:42Z	1794007722	precheck_far_early | srcversion=332759AF31AB0DD875E0EE7 expected=332759AF31AB0DD875E0EE7
2026-11-06T23:28:42Z	1794007722	precheck_receiver | pid=599898 log=/home/kunweiz/tbtun-forensics/netconsole-6666.log lines=5520 relay=active sampler=active rx=active-user
2026-11-06T23:28:50Z	1794007730	precheck_far | srcversion=332759AF31AB0DD875E0EE7 expected=332759AF31AB0DD875E0EE7 ko_sha=49d632d37a34660e906d9f5b8919d9f7895c4912a8af9f36a7b55873ddfbc0f4 ko_mtime=1794004781
2026-11-06T23:28:51Z	1794007731	precheck_module | dir=/root/tbtun-fixed-20261106T231212Z srcversion=72168C1CE3E3378A758DAFF sha=1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92 identity=match
2026-11-06T23:28:51Z	1794007731	visibility_ready | pid=599898 log=/home/kunweiz/tbtun-forensics/netconsole-6666.log lines=5520 relay=active sampler=active rx=active-user
2026-11-06T23:28:53Z	1794007733	precheck_done | burst_sha=afa13883aa79f587e7bd667cddd8637e6eb7dab1747ccdb66fbd7d707d49a740
2026-11-06T23:28:53Z	1794007733	far_config_start
2026-11-06T23:28:58Z	1794007738	far_config_done | ports=31000-31015 config=exact echo=5202
2026-11-06T23:28:58Z	1794007738	near_config_start
2026-11-06T23:28:58Z	1794007738	route | dev=eth0
2026-11-06T23:28:58Z	1794007738	near_config_done | ports=30000-30015 config=exact
2026-11-06T23:28:58Z	1794007738	join_start | timeout=60s target=up=8
2026-11-06T23:29:01Z	1794007741	join_ok | near_up=8 far_up=8
2026-11-06T23:29:03Z	1794007743	burst_start | tbt_burst 127.0.0.1 9001 1 5 interleaved timeout=120s
2026-11-06T23:29:04Z	1794007744	burst_end | ssh_rc=0 burst_rc=0 wall_ms=1498 at=2026-11-06T23:29:04Z
2026-11-06T23:29:04Z	1794007744	netconsole_snapshot | start=451213 end=500530
2026-11-06T23:29:11Z	1794007751	far_evidence_done
2026-11-06T23:29:17Z	1794007757	evidence_done | near_lost=0 rebooted=0
2026-11-06T23:29:17Z	1794007757	end | final_rc=0
2026-11-06T23:29:17Z	1794007757	cleanup_start | exit_rc=0
2026-11-06T23:29:20Z	1794007760	cleanup_done | clean_fail=0
```

## 证据清单

| 文件 | 字节 |
|---|---|
| `burst-build.log` | 0 |
| `burst.txt` | 223 |
| `cleanup.txt` | 192 |
| `config-far.txt` | 386 |
| `config-near.txt` | 388 |
| `far-dmesg.log` | 333368 |
| `far-journal.log` | 513772 |
| `far-meta.txt` | 175 |
| `far-pre-config.txt` | 386 |
| `far-ss.txt` | 7559 |
| `far-stats.txt` | 4785 |
| `joined-far.txt` | 284748 |
| `joined-near.txt` | 156370 |
| `join-probes.txt` | 80 |
| `module.txt` | 979 |
| `near-dmesg.log` | 142409 |
| `near-journal-b0.log` | 41826 |
| `near-journal-b1.log` | 17 |
| `near-journal.log` | 41826 |
| `near-meta.txt` | 1174 |
| `near-probes.txt` | 218 |
| `near-pstore.txt` | 243 |
| `near-ss.txt` | 3867 |
| `near-stats.txt` | 4756 |
| `near-tbtun-last.log` | 1558325 |
| `netconsole-crash-lines.txt` | 49091 |
| `netconsole-fds.txt` | 219 |
| `netconsole.log` | 500530 |
| `netconsole-window.log` | 49317 |
| `post-far.txt` | 285310 |
| `post-near.txt` | 157003 |
| `pre-far.txt` | 283389 |
| `pre-near.txt` | 117436 |
| `route.txt` | 77 |
| `run.log` | 590 |
| `stats.txt` | 631 |
| `SUMMARY.md` | 8297 |
| `timeline.txt` | 2547 |

## 清理

```
=== cleanup 2026-11-06T23:29:17Z ===
near_reachable=yes near_rebooted=0 far_reachable=yes
near_rmmod=ok
near_rule=deleted
near_sysctl=restored
far_echo=stopped
far_rmmod=ok
locks_released=yes
```

## 结论与限制

- 修复有效：join 成功，单次 1 MiB burst 通过，near 存活。
- 本次实验只触发一次；无论结果如何都不应重跑同一构建。
- 若复现崩溃：先保存本目录，再按 `stop-65-down-*/forensics-65/README.md` 的只读取证流程分析；不要重新加载模块。
- 若 near 失联但无 Oops：netconsole 可能未送达 panic 输出，pstore/journal 也可能为空；结论只能写“未定”。
- netconsole 日志的窗口从本次布防之后开始；完整文件同时保存在 `netconsole.log`。
