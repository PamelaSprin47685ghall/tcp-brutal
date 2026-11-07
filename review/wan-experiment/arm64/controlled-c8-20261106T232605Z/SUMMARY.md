# controlled-c8-once 实验报告

- 实验 UTC：20261106T232605Z
- 退出码：4
- 判定：前置检查失败，未加载模块、未触发 burst；见步骤表与 run.log。
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
| ko sha256 | `unknown` | `unknown` |
| ko srcversion | `unknown` | `unknown` |
| loaded srcversion | `unknown` | `unknown` |
| vermagic | `unknown` | `unknown` |
| expected srcversion | `72168C1CE3E3378A758DAFF` | `332759AF31AB0DD875E0EE7` |
| expected sha256 | `1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92` | — |
| expected vermagic | `6.1.172 SMP mod_unload modversions aarch64` | kernel `7.0.0-42-generic` |
| identity match | `?` | `?` |
| ko mtime (epoch) | — | `0` |
| insmod | `max_carriers=256 max_tunnels=1 debug=0` | 已由 deploy_hnd0.sh 加载 |

## 前置检查与可见性

| 项 | 结果 |
|---|---|
| 两端 ssh | fail |
| .65 kernel.panic_on_oops=1 | ? |
| .65 kernel.panic=10 | ? |
| .65 额外 sysctl | ? |
| .65 /sys/fs/pstore 为空 | ? |
| .65 ramoops 已注册 | ? |
| .65 tbtun-kmsg-relay | ? |
| .65 tbtun-sampler | ?（baseline lines=0） |
| .80 UDP 6666 接收端 | pid=? log=? |
| .80 tbtun-netconsole-rx | ?（日志行基线=0，结束=0） |
| kmsg 探针 | 0 |
| far tbtun 已加载 | not-checked |
| far fixed srcversion | unknown（expected 332759AF31AB0DD875E0EE7，match=?） |
| far c8 config / listeners | fail / not-checked |
| near tbtun 未加载 | not-checked |

## 步骤、预期结果与失败分支

| # | 步骤 | 预期结果 | 失败分支 | 实际 |
|---|---|---|---|---|
| 1 | 前置检查 | 两端 ssh、near panic/ramoops、可见性服务、far 已加载且 srcversion 为修复版、near 未加载、near 模块身份逐项匹配 | 任一失败：退出 3，不加载模块、不写配置；far 不匹配时停止、不跑 near | fail |
| 2 | 锁与负载 | 两端 flock 获取，load1 <= 4 | 锁忙/负载高：退出 4，不跑 | locks_held=no |
| 3 | 可见性检查 | .65 relay/sampler active，.80 接收端 active，kmsg 探针在接收文件落盘 | 任一服务 inactive 或探针丢失：退出 3，不跑 | probe=0 relay=? sampler=? rx=? |
| 4 | far 配置 | far fixed srcversion 已匹配；ports 31000-31015、c8 config（carriers=8/n=4/m=4/fec=on/grouped/w=8/crypto=on/http-kal）逐字读回，9000/5202 监听 | 写失败/读回不一致/监听未起：退出 10，near 不加载 | fail |
| 5 | near 配置 | ip rule、rmem/wmem、insmod、加载后 srcversion=72168C1CE3E3378A758DAFF、ports 30000-30015、config 逐字读回 | 任一步失败：退出 10，清理 | fail |
| 6 | join gate | 60s 内两端 up=8；parse/mismatch/FEC 计数器记录但不阻塞 | 超时或快照 up 不一致：退出 5，不触发 burst，收集证据 | JOIN_OK=0 near_up=? far_up=? counter_warn=0 |
| 7 | 触发 | 恰好一次 1 MiB interleaved burst，120s 内返回 | 非 0/超时：记录，不重试；near 失联则转证据收集 | attempted=0 ssh_rc=未执行 burst_rc=未执行 wall_ms=? |
| 8 | 失联判定 | 记录 ping/ARP/ssh、失联与恢复时刻、是否重启 | 无法判定：记录 unknown，继续 far/netconsole 证据 | near_lost=0 lost_at=none returned_at=none rebooted=0 |
| 9 | 证据收集 | netconsole、pstore、journal -b -1 --since、sampler、dmesg、stats、far dmesg/ss 全部落盘 | 单项失败：记录原因，不阻塞其他项；无更优先判定时退出 8 | complete |
| 10 | 清理 | near rmmod/rule/sysctl、far rmmod/echo、锁释放 | 失联跳过并记录；清理失败退出 9 | clean_fail=0 |

## 崩溃证据计数

- netconsole 窗口内 `Internal error: Oops`：0
- netconsole 窗口内 `Kernel panic`：0
- near journal 内 Oops/panic：0
- netconsole 窗口：行 0 -> 0（0 -> 0 bytes）
- sampler：行 0 -> 0
- pstore：见 `near-pstore.txt`（同时收集 /sys/fs/pstore 与 systemd-pstore 归档）
- 注意：kmsg-relay/sampler 是用户态进程；panic 后可能停止，pstore 是主证据。

## 时间线

```
2026-11-06T23:26:05Z	1794007565	start | single-shot c8; evidence=/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/controlled-c8-20261106T232605Z
2026-11-06T23:26:07Z	1794007567	precheck_ssh | near=aarch64/6.1.172 far=7.0.0-42-generic boot_id=0a91d2eb-3122-41eb-913c-024a4e1fe724
2026-11-06T23:28:07Z	1794007687	cleanup_start | exit_rc=4
2026-11-06T23:28:08Z	1794007688	cleanup_done | clean_fail=0
```

## 证据清单

| 文件 | 字节 |
|---|---|
| `cleanup.txt` | 140 |
| `run.log` | 274 |
| `SUMMARY.md` | 5731 |
| `timeline.txt` | 414 |

## 清理

```
=== cleanup 2026-11-06T23:28:07Z ===
near_reachable=yes near_rebooted=0 far_reachable=yes
near_rmmod=skipped(not_loaded)
locks_released=yes
```

## 结论与限制

- 前置检查失败，未加载模块、未触发 burst；见步骤表与 run.log。
- 本次实验只触发一次；无论结果如何都不应重跑同一构建。
- 若复现崩溃：先保存本目录，再按 `stop-65-down-*/forensics-65/README.md` 的只读取证流程分析；不要重新加载模块。
- 若 near 失联但无 Oops：netconsole 可能未送达 panic 输出，pstore/journal 也可能为空；结论只能写“未定”。
- netconsole 日志的窗口从本次布防之后开始；完整文件同时保存在 `netconsole.log`。
