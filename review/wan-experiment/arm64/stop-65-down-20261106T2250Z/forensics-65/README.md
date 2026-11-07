# forensics-65 — .65 near-side crash evidence

- 目标：`192.168.123.65`（lubancat，Embedfire LubanCat-5 V2，RK3588 aarch64，Linux 6.1.172 #1 SMP）
- 事故时刻：`2026-11-06 22:44:09–22:44:16Z` = `2026-11-07 06:44:09–06:44:16 CST`
- 重启：`2026-11-06 22:59:26Z` = `2026-11-07 06:59:26 CST`
- 取证方式：ssh 只读；未 rmmod/insmod/重启/跑测试/改配置
- 时区：远端 `Asia/Shanghai`（CST = UTC+8）。**所有 journalctl 相对时间参数必须带 `UTC` 或换算成 CST，否则窗口错误**（例如字面 `--since "2026-11-06 22:20"` 会被解释为 14:20Z，返回 `-- No entries --`）。

## 判定

**内核 Oops（tbtun 模块内存破坏）**，不是 NIC/链路故障、不是掉电、不是单纯 hang/软锁。

- 6 次 `Internal error: Oops`，全部在 `tbtun` 加载后 1–7 秒内；`Modules linked in: tbtun(O)`。
- Oops #2 的 `lr = tbt_http_rx_parse+0x1bc/0x620 [tbtun]`，栈为 `tbt_rx_parse → tbt_rx_parse_pending → tbt_car_rx_work`，崩溃在 `__memcpy`。
- Oops #1/#3/#4 发生在无关进程（`bash` / `cc` / `python3`）的 dcache 路径（`__d_lookup_rcu` / `__d_lookup`），说明破坏是全局 slab 级的。
- Oops #2 的 `pgd = 0x18000002ffff8003` 指向远超本机 RAM 的物理地址，页表项已被污染。
- 崩溃前 4 分钟 atop 采样正常（CPU idle 799%、MEM free 6.0G、`oomkill 0`），排除资源耗尽。
- `ethtool -S eth0` 无 CRC/length/over/dropped 错误；`Link is Down` 仅在 20:23/20:41 CST（事件前 6 小时）出现且随后恢复。
- `RuntimeWatchdogUSec=0`；本次启动 dmesg 无 `watchdog did not stop`；DW watchdog `cr` bit0=0。**无看门狗复位证据**。
- 15 分钟静默后的复位源**证据不足**（无 systemd shutdown 日志、无 panic、无 kdump、pstore 空）。

## far 端交叉验证（来自同目录 `../far-state.txt`）

far 端（45.11.1.62:9000）在 `55128.29` uptime 接受 8 条连接：

```
tunnel 1 carrier ACCEPTED lport=9000 rport=24287, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=24289, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=24291, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=30000, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=30001, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=30002, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=30004, awaiting HELLO
tunnel 1 carrier ACCEPTED lport=9000 rport=30006, awaiting HELLO
```

far stats：`up=0 lost=0 ... rx_read_bytes=0 rx_reads=8 rx_read_zero=8 rx_ready_calls=0 tx=0 wire=0 streams_total=8 streams_active=0`。

**结论：far 既没有收到 HELLO，也没有发送过任何应用层数据（`tx=0 wire=0`）。因此 near 的 `tbt_http_rx_parse` 的输入不是 far 的 HTTP 响应，而是 near 本地的未初始化/复用缓冲区。** 这与 Oops #1 的 X8 dump 中出现 netpbm man page 文件名碎片（`pamchannel.1.gz`、`pamcomp.1.gz`、`pamcrate.1.gz`、`pamcut.1.gz`、`pamdeinterlace.1`）一致：rx 缓冲区未清零，解析了 slab 残留垃圾。

另：8 条连接中有 3 条使用临时端口 `24287/24289/24291`，不在 near 配置的 `portblock=30000-30015` 内，提示端口分配竞争/回退，值得后续审计（非本次崩溃直接原因）。

## 加载前 30 秒窗口（`pre_load_30s.txt`）

`06:43:30–06:44:09` 只有 CRON（`compile-guard`）与 mihomo NTP 常规记录：

- 无旧 tbtun 实例（`06:00–06:44:10` 唯一的 tbtun 日志就是 `06:44:09 loaded`）
- 无旧端口占用/`carrier DIALED` 残留（最后一次 tbtun 活动在 19:30 CST）
- 无 userspace 写 `config`/`ports` 的日志（journal 不记录此类写入，且无相关程序输出）
- 模块加载前状态干净

## Oops #2 寄存器要点（详见 `oops2-regs.txt`）

```
CPU: 0  PID: 271981  Comm: kworker/0:3
pc : __memcpy+0x15c/0x260
lr : tbt_http_rx_parse+0x1bc/0x620 [tbtun]
x0 (dst) = 0xffff000195684000
x1 (src) = 0xffff0001ffffffc0   # 距 0xffff000200000000 边界仅 0x40
x2 (len) = 0x0000ffff2433bb80   # ≈256 TiB，垃圾长度，越界的直接原因
x3       = 0xffff0001ffffefc0
x4       = 0x000000012433bb90
x5       = 0x000000012433ab90
x8       = 0x0000000000000000   # 注意：Oops #2 的 x8 为 0
```

**纠正**：文件名碎片不在 Oops #2（其 x8=0），而在 **Oops #1** 的 `x8 = 0xffff000195f6e510` 内存 dump 中；该 dump 已作为附录附在 `oops2-regs.txt` 末尾。

## 文件清单

| 文件 | 内容 |
|---|---|
| `meta.txt` | date/tz、uptime、cmdline、`journalctl --list-boots`、panic sysctls、pstore、lsmod、接口、watchdog 设备 |
| `b1_oops_sections.txt` | `journalctl -b -1` 的 58700–62103 行原文（首次 fault 到日志终止，含 6 次 Oops），390 KB |
| `b1_markers.txt` | `-b -1` 中所有 Oops/BUG/WARNING/Call trace/link down 的行号索引 |
| `b1_tail400.txt` | `-b -1` 最后 400 行原文 |
| `oops2-regs.txt` | Oops #2 完整寄存器/栈/内存 dump（行 58924–59766）+ Oops #1 X8 dump 附录 + 寄存器解读 |
| `boot0.txt` | 本次启动 dmesg、ethtool/`-S`、`ip -s link`、`ip neigh`、`ss -tan`、`/var/log` 列表 |
| `visibility.txt` | 可见性设施现状：cmdline、pstore、kexec、panic、`/proc/config.gz`、journald、watchdog debugfs |
| `window_utc.txt` | 用户字面窗口 vs 正确 UTC 窗口对比；OOM/link down/watchdog 检查 |
| `tail_checks.txt` | systemd watchdog 配置、`-b -2` 尾部、静默窗口验证、rsyslog 状态 |
| `rsyslog_window.txt` | `kern.log`/`syslog` 的 06:4x 落盘检查、atop 文件列表、rsyslog 配置 |
| `atop_check.txt` | atop 06:40 崩溃前采样、syslog ISO 时间戳、rsyslog `imklog` 配置 |
| `pre_load_30s.txt` | 06:43:30–06:44:10 完整 journal、tbtun/port/carrier 关键词、加载历史 |

## 完整原始文件

**完整 `journalctl -b -1`（62103 行，9.6 MB）未入库**，保留在取证机：

```
/tmp/tbt-forensics-65/b1_full.txt
```

重建方法（在 .65 可达时）：

```bash
ssh root@192.168.123.65 'journalctl -b -1 --no-pager -o short-iso' > b1_full.txt
```

仓库内 `b1_oops_sections.txt` 即该文件第 58700–62103 行，覆盖全部 6 次 Oops 及崩溃触发序列；`b1_markers.txt` 提供行号索引，可按需回查。

## 关键行号索引（相对于 `b1_full.txt`）

| 行号 | 内容 |
|---|---|
| 58707 | `tbtun 0.0.1 loaded: 1 tunnels max, 256 carriers max` |
| 58708 | `tbtun: tunnel 1 carrier 3 DIALED lport=30003 peer=45.11.1.62:9000` |
| 58709 | 首次 fault `000000020047fffc` |
| 58736 | Oops #1 `[#1]`（bash, `__d_lookup_rcu`） |
| 58924 | Oops #2 fault `ffff000200000000` |
| 59352 | Oops #2 `[#2]`（kworker/0:3, `__memcpy` ← `tbt_http_rx_parse`） |
| 59767 | Oops #3 fault |
| 59779 | Oops #3 `[#3]`（cc） |
| 60325 | Oops #4 fault（NULL+0x19） |
| 60353 | Oops #4 `[#4]`（python3） |
| 60397 | Oops #5 fault（`000000000594a585`，写） |
| 60959 | Oops #5 `[#5]`（kworker/u16:4, `move_expired_inodes`） |
| 61501 | `WARNING ... kernel/exit.c:817 do_exit+0x90` |
| 62091 | Oops #6 fault `0000000200480058` |
| 62103 | **最后一行**：`Internal error: Oops: 0000000096000004 [#6] SMP` |

## 缺失证据

1. pstore/ramoops 为空：ramoops 已注册，但 `panic_on_oops=0`，Oops 不触发 `panic()`。
2. 无 kdump：`CONFIG_KEXEC`/`CONFIG_CRASH_DUMP` 未编译。
3. 无 SysRq：`CONFIG_MAGIC_SYSRQ` 未编译。
4. 无 soft lockup / hung task 检测：`CONFIG_SOFTLOCKUP_DETECTOR`/`CONFIG_DETECT_HUNG_TASK` 未编译。
5. Oops #6 无 Call trace：日志在 `Mem abort info` 后中断。
6. `06:44:16–06:59:26` 无任何日志，无法区分 hang 与立即死亡。
7. 复位源不明：无 shutdown 日志、无 watchdog 证据。
8. rsyslog 的 `kern.log`/`syslog` 未捕获 06:44 的 Oops（06:43:06 后停止落盘），journald 是唯一来源。
9. `/var/log/dmesg` 只保存启动前 12 秒。
10. 本次启动 tbtun 未加载，`/proc/net/tbtun/*` 不可用。
11. ethtool 计数已因重启清零。
12. far 端日志仅 `far-state.txt` 快照，未取完整 journal。
