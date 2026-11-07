# tbtun-netwatch — .65 硬件看门狗 + 网关 HTTP 门控

目标机：`root@192.168.123.65`（Embedfire LubanCat-5 V2，RK3588 aarch64，Ubuntu 26.04.1，systemd 259，kernel 6.1.172）。

本文所有数字来自 2026-11-07 08:31–08:33 CST 的只读实测。

## 1. 结论先行

| 问题 | 实测答案 |
|---|---|
| 有没有硬件看门狗 | 有，`Synopsys DesignWare Watchdog [version 0]`，`/dev/watchdog0`（243,0）与 `/dev/watchdog`（10,130） |
| 超时能不能设 30 s | **不能**。`wdctl` 报 `Timeout: 44 seconds`、`SETTIMEOUT 0`；驱动没有 `.set_options`，超时固定 44 s |
| `nowayout=1` 能不能设 | **运行时不能**。`CONFIG_WATCHDOG_NOWAYOUT` 未设置，`dw_wdt` 是 `CONFIG_DW_WATCHDOG=y` 内建，`/sys/module/dw_wdt/parameters/nowayout` 不存在（`module_param(..., perm=0)`） |
| 那还需要 nowayout 吗 | **不需要**。`MAGICCLOSE 0` 已经给出更强保证：一旦 open，用户态无法停止，写 `V`、`WDIOS_DISABLECARD`、close 都不行 |
| 有没有 sysfs 属性可读 | **没有**。`CONFIG_WATCHDOG_SYSFS` 未设置，`/sys/class/watchdog/watchdog0/` 下只有 `dev/device/power/subsystem/uevent` |
| systemd 是否在喂狗 | **否**。`RuntimeWatchdogUSec=0`、`WatchdogDevice=` 空、无 `watchdogd` |
| 开机时看门狗是否已武装 | **否**。`wdctl` 的 `BOOT-STATUS` 全 0，dmesg 无 boot 期 watchdog 记录 |

由此得到设计的第一原则：**唯一的复位开关就是"停止喂狗"**。这个开关一旦部署，直到下次重启都不能取消。

### 1.1 原始探测输出

`wdctl /dev/watchdog0`（设备空闲时，唯一一次成功读取）：

```
Device:        /dev/watchdog0
Identity:      Synopsys DesignWare Watchdog [version 0]
Timeout:       44 seconds
Timeleft:      44 seconds
Pre-timeout:    0 seconds
FLAG           DESCRIPTION               STATUS BOOT-STATUS
KEEPALIVEPING  Keep alive ping reply          1           0
MAGICCLOSE     Supports magic close char      0           0
PRETIMEOUT     Pretimeout (in seconds)        0           0
SETTIMEOUT     Set timeout (in seconds)       0           0
```

`zcat /proc/config.gz | grep -E 'CONFIG_WATCHDOG|CONFIG_DW_WATCHDOG'`：

```
CONFIG_WATCHDOG=y
CONFIG_WATCHDOG_CORE=y
# CONFIG_WATCHDOG_NOWAYOUT is not set
CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y
CONFIG_WATCHDOG_OPEN_TIMEOUT=0
# CONFIG_WATCHDOG_SYSFS is not set
# CONFIG_WATCHDOG_HRTIMER_PRETIMEOUT is not set
# CONFIG_WATCHDOG_PRETIMEOUT_GOV is not set
CONFIG_DW_WATCHDOG=y
```

`modinfo dw_wdt`：

```
filename:       (builtin)
parm:           nowayout:Watchdog cannot be stopped once started (default=0) (bool)
```

`dmesg | grep -i -E 'watchdog|wdt|feaf0000'`：

```
[    9.325692] rk_gmac-dwmac fe1c0000.ethernet: Enable RX Mitigation via HW Watchdog Timer
[    9.894203] rk_gmac-dwmac fe1b0000.ethernet: Enable RX Mitigation via HW Watchdog Timer
[ 5569.362919] watchdog: watchdog0: watchdog did not stop!
[ 5608.944941] watchdog: watchdog0: watchdog did not stop!
```

前两行是 GMAC 网卡的 RX mitigation，和 dw_wdt 无关，容易被误读成看门狗已启用。后两行是第 10 节事故的痕迹。

`cat /proc/cmdline` 里没有任何 `watchdog` / `dw_wdt` 参数。时钟：`tclk_wdt0` = 24 MHz、`pclk_wdt0` = 100 MHz，均指向 `feaf0000.watchdog`。

`systemctl show -p RuntimeWatchdogUSec -p RebootWatchdogUSec -p KExecWatchdogUSec -p WatchdogDevice`：

```
WatchdogDevice=
RuntimeWatchdogUSec=0
RebootWatchdogUSec=10min
KExecWatchdogUSec=0
```

`command -v wdctl watchdogd`：`/usr/bin/wdctl` 存在，`watchdogd` 不存在。`systemctl list-unit-files | grep -i watchdog` 为空。

44 s 这个数字有一处独立交叉验证：第 10 节里临时喂狗者被 kill 后，机器在**无喂狗状态下存活了约 36 秒**才被新进程接管，期间没有复位。所以真实超时明显大于 36 s，与 `wdctl` 报的 44 s 一致。

### 1.2 0.2.0 验收参数确认

| 参数 | 值 | 落在哪里 | 怎么确认 |
|---|---|---|---|
| 开机宽限 | 600 s | `WATCHDOG_GRACE_S` / `BOOT_GRACE` | `--selftest` 的 `SELFTEST timing` 与 `SELFTEST uptime` |
| 探测间隔 | 30 s | `WATCHDOG_PROBE_S` / `PROBE_SECS` | 同上；日志每 30 s 一次探测 |
| 连续失败阈值 | 3 次 | `WATCHDOG_FAIL_THRESHOLD` | 同上；日志 `gateway probe failed 1/3..3/3` |
| 硬件复位 | 44 s | `WATCHDOG_HARD_TIMEOUT_S` | `wdctl` 的 `Timeout: 44 seconds`；`SETTIMEOUT 0` |
| 总容忍 | 734 s | 派生值 `600 + 30×3 + 44` | `SELFTEST timing` 断言 `total=734`，不等即 FAIL |
| 喂狗周期 | 5 s | `WATCHDOG_FEED_S` | `SELFTEST timing` 断言 `feed×3 < 44` |
| reset budget | 5 次 / 30 min | `WATCHDOG_RESET_BUDGET` / `WATCHDOG_RESET_WINDOW_S` | `SELFTEST reset_budget` 的 `armed_in_window` 与 `boot_count` |
| 服务不自动重启 | `Restart=no` | unit 文件 | `systemctl show -p Restart --value tbtun-netwatch` → `no`；`SELFTEST unit_restart` |
| systemd 不喂狗 | `RuntimeWatchdogUSec=0` | systemd 全局配置 | `systemctl show -p RuntimeWatchdogUSec --value` → `0`；`SELFTEST systemd_watchdog` |

验收前把这两条命令的输出原样存进证据目录：

```sh
systemctl show -p Restart -p RuntimeWatchdogUSec -p MainPID tbtun-netwatch
/usr/local/sbin/tbtun-netwatch.py --selftest --no-probe
```

## 2. 路由器 80 端口实测

命令：

```sh
curl -sS -m 5 --connect-timeout 3 -o /tmp/gw-b.txt -D /tmp/gw-h.txt \
     -w 'code=%{http_code} total=%{time_total} size=%{size_download}\n' \
     http://192.168.123.254/
```

5 次结果，全部成功：

```
code=401 dns=0.000075 conn=0.000821 total=0.001722 size=140
code=401 dns=0.000076 conn=0.000748 total=0.001579 size=140
code=401 dns=0.000074 conn=0.000742 total=0.001599 size=140
code=401 dns=0.000076 conn=0.000762 total=0.001683 size=140
code=401 dns=0.000076 conn=0.000726 total=0.001624 size=140
```

成功率 5/5，总耗时 1.58–1.72 ms，抖动 < 0.15 ms。

响应头原文（`cat -A`，`^M$` 是 CRLF）：

```
HTTP/1.0 401 Unauthorized^M$
Server: httpd^M$
Date: Sat, 07 Nov 2026 00:33:24 GMT^M$
WWW-Authenticate: Basic realm="JDC-1"^M$
Content-Type: text/html^M$
Connection: close^M$
^M$
```

正文前 200 字节（实际全文 140 字节）：

```
<HTML><HEAD><TITLE>401 Unauthorized</TITLE>
<BODY BGCOLOR="#cc9999"><H4>401 Unauthorized</H4>
Authorization required.
</BODY></HTML>
```

没有 `Content-Length`（HTTP/1.0 + `Connection: close`）。`Content-Type: text/html`。

端口扫描：

| 端口 | 结果 |
|---|---|
| 80 | OPEN |
| 22 | OPEN |
| 53 | OPEN |
| 443 / 8080 / 8443 / 23 | closed/filtered |

`ping 192.168.123.254`：3/3，0% 丢包，0.376/0.429/0.470 ms。

## 3. "正常响应"的可执行判据

三条同时成立才算健康：

1. `curl` 退出码 0；
2. HTTP 状态码匹配 `^[1-4][0-9]{2}$`，即 100–499；
3. 响应头首行匹配 `^HTTP/1\.[01] ([1-5][0-9]{2})`，且该数字与 `curl -w %{http_code}` 一致。

**401 是健康答案**，不是故障。这是本设计最容易被写错的一点：任何"要求 200"或"要求 2xx/3xx"的判据在这台路由器上会 100% 误报。

明确排除：

- 5xx 算失败（路由器自带 httpd 不会返回 5xx，出现即异常）；
- `000` 算失败（连接失败、超时、DNS 失败）；
- 首行不是 HTTP 状态行算失败（防劫持页、防 captive portal）；
- **不匹配正文关键字**。正文里的 `Authorization required.` 会随固件升级变化，`Basic realm="JDC-1"` 同理；
- **不要求 `Content-Length > 0`**。实测没有这个头，写了就是永远失败。

## 4. 设计

### 4.1 时间预算

```
uptime 0–600 s ──► 只喂狗，不探测，不累计失败
uptime 600 s   ──► 宽限结束；再等一个探测周期（对齐到开机时间）
uptime 630 s   ──► 第一次探测，之后每 30 s 一次
uptime 690 s   ──► 第 3 次连续失败，停止喂狗
uptime 734 s   ──► 硬件 44 s 超时复位；总容忍 ≈ 734 s（12 分 14 秒）
```

宽限从**系统 uptime** 算，不是从服务启动算。服务在开机 30 s 时启动，宽限就剩 570 s；服务在 uptime 两小时时被手工拉起，宽限已经过去，立即开始探测。判定依据是 `/proc/uptime` 的第一个字段，每轮循环重读。

总容忍 734 s = 600 + 90 + 44：宽限结束（600）→ 等一个周期（30）→ 第 1 次探测（630）→ 第 2 次（660）→ 第 3 次失败（690）→ 停止喂狗 → 44 s 后复位（734）。第一次探测对齐到**开机时间**，不是对齐到"循环发现宽限结束"的那一刻；否则服务启动偏移会让 734 漂移。这个数字是"从开机到复位"的近似上界，前提是宽限一结束网络就是坏的。宽限期内网络坏多久都不复位。

喂狗周期 5 s。44 s 超时下余量 8.8 倍；若按建议的 10 s，余量只有 4.4 倍，一次 mmcblk0 写抖动就可能吃掉一半。所以取 5 s，`FEED_SECS` 可改。

### 4.1.1 宽限期做什么、不做什么

| 行为 | 宽限期内（uptime < 600 s） | 宽限期后 |
|---|---|---|
| 喂狗 | 是，每 5 s | 是，每 5 s |
| HTTP 探测 | **否** | 是，每 30 s |
| 失败计数 | **否**，计数保持 0 | 是 |
| 停止喂狗 / 复位 | **否** | 连续 3 次失败后 |
| 日志 | `boot grace start`、每 2 分钟一条 `boot grace: uptime=...`、`boot grace end: ...; first probe at uptime=630` | 失败才记 `gateway probe failed n/3` |

宽限期的实现是**根本不调用 `probe_once()`**，不是"探测了但不计入"。这样连"失败告警"都不会产生——宽限期内的 HTTP 失败既不复位，也不写 `gateway probe failed`。这是有意的：开机阶段网络本来就可能没起来，把它记成失败只会污染日志，让真正的故障难找。

如果服务启动时 uptime 已经超过 600（手工 `systemctl start`，或 unit 起得晚），日志只有一条 `uptime=... already past the 600s grace; probing begins`，没有 `boot grace start`。

`/proc/uptime` 读不出来时（权限、procfs 异常）服务**留在宽限期**继续喂狗。方向是"不复位"，因为读不到 uptime 不构成"网关坏了"的证据。

宽限期存在的理由：故障持续时，44 s 复位一次会让机器永远起不来，验收现场和人工干预窗口都没有。600 s 让"坏网络 + 重启"的循环周期拉长到 12 分钟以上，同时给 reset budget 留出累计时间。

### 4.2 为什么是"停止喂狗"而不是 `systemctl reboot`

`systemctl reboot` 走的是用户态 shutdown 路径，内核已经 hang 时它根本不会被执行；`reboot -f` 也要进程还能被调度。硬件看门狗不依赖任何用户态代码，内核死了它照样倒计时。所以复位动作交给硬件，脚本只负责"不再喂"。

停止喂狗前做三件事，顺序不能反：写 journal、写 `/var/log/tbtun-watchdog.log`（`fsync`）、`sync`。之后才 `give_up.set()`。

### 4.3 为什么 `Restart=no`

喂狗者自己崩溃、被 kill、被 OOM 时，fd 关闭，硬件继续倒计时，44 s 后复位——这正是我们要的。如果配了 `Restart=always`，systemd 会在 1 s 内把它拉起来重新 open、重新喂狗，故障被永久掩盖。所以 `Restart=no` 是功能要求，不是风格偏好。

同理 `OOMScoreAdjust=-1000`：喂狗者是最后一个该被 OOM 杀掉的进程；但真被杀了也必须复位，不能重启。

### 4.4 为什么 `RuntimeWatchdogUSec=0`

保持 0。如果 systemd 也喂狗，那么"我们的喂狗者停了"这件事会被 systemd 掩盖，门控失效。`RebootWatchdogUSec=10min` 保持默认，它只在 shutdown 期生效，与运行期无关。

### 4.5 告警通道

三层，全部复用 .65 已有设施：

| 层 | 落点 | 说明 |
|---|---|---|
| journal | `SyslogIdentifier=tbtun-netwatch` | `journalctl -u tbtun-netwatch` |
| 文件 | `/var/log/tbtun-watchdog.log` | 每条 `fsync`，复位后仍在 |
| kmsg | `/dev/kmsg`，格式 `<4>tbtun-netwatch: ...` | 被现有 `tbtun-kmsg-relay.service` 转发到 `192.168.123.80:6666` |

注意：网络已经坏掉时 kmsg 转发必然也发不出去（UDP fire-and-forget，不阻塞）。它的价值在于"路由器 httpd 挂了但 .65 网络正常"这一种情况。真正的复位证据要看复位后的 journal 和 `/var/log/tbtun-watchdog.log`。

### 4.6 复位预算

`/var/lib/tbtun-netwatch/armed-log` 每次武装启动追加一行 epoch；启动时丢弃
`WATCHDOG_RESET_WINDOW_S=1800` 之外的行，`boot-count` 保存窗口内的条数。一次
武装启动最多对应一次复位，所以窗口内已有超过 `RESET_BUDGET=5` 次武装启动时，
判定改为**继续喂狗、只写告警**——不是"不 open"。不 open 会让设备无人喂狗，
与"不要复位风暴"是两件事：前者把机器变成无保护，后者只是不再咬。

这是第二道防线，和宽限期职责不同：

| 防线 | 挡住什么 | 计数对象 |
|---|---|---|
| 开机宽限 600 s | 单次开机内的过早复位 | 无计数，只看 uptime |
| reset budget 5 次 | 跨多次开机的复位循环 | 启动次数 |

宽限期内的 HTTP 失败**不产生失败计数**，因此也**不进 budget**。budget 只统计"武装启动了几次"，由 `main()` 在打开设备前 `+1`，与探测结果无关。连续 30 分钟探测健康才清零。

### 4.7 切换窗口

`/dev/watchdog0` 只允许一个打开者：旧喂狗者持有 fd 时，新进程 `open()` 返回
`EBUSY`。所以**不存在**"新进程先拿到 fd、再停旧进程"的顺序；声称这么做的脚本
一定在两个进程之间留了无人喂狗的空窗。

`install-65.sh` 用的是设备允许的最强顺序：

1. 旧喂狗者继续持有 fd（`tbtun-wdt-hold`，脚本核对它的 `/proc/<pid>/fd`）；
2. 启动新服务；`open_watchdog()` 每 2 s 重试 `EBUSY`，窗口
   `WATCHDOG_OPEN_RETRY_S=120` s；
3. 等日志出现 `open /dev/watchdog0: busy ... retrying`——这是"新进程的 open
   路径已被执行、正在等待"的唯一证据；
4. 停旧喂狗者；旧 fd 释放后，新进程的下一次重试（≤2 s）成功；
5. 在 44 s 硬件窗口内确认新 MainPID 的 fd 指向 `/dev/watchdog0`，且日志出现
   `first feed ok`；
6. 任一步失败立即 `systemctl start tbtun-wdt-hold`，等它重新 open 并喂狗，再以
   非零退出；绝不停掉最后一个喂狗者。

切换不影响宽限判定：宽限看的是 `/proc/uptime`，不是服务启动时刻。在 uptime
200 s 时切换，新进程照样只喂狗到 600 s；在 uptime 两小时时切换，新进程立即
开始探测。

反方向是 fail-safe：新进程 120 s 内没拿到 fd 就退出，旧喂狗者仍在喂，机器不受
影响；脚本报告失败，人工重试。

### 4.8 运行参数与状态文件

常量全部可用环境变量覆盖（unit 没有 `EnvironmentFile`，需要时手工加一行
`EnvironmentFile=-/etc/tbtun-netwatch.conf`）：

| 变量 | 默认 | 含义 |
|---|---|---|
| `WATCHDOG_PROBE_URL` | `http://192.168.123.254/` | 探测目标 |
| `WATCHDOG_EXPECT_STATUS` | `100-499` | 允许的状态码集合，逗号与 `a-b` 区间 |
| `WATCHDOG_FEED_S` | `5` | 喂狗周期 |
| `WATCHDOG_PROBE_S` | `30` | 探测周期 |
| `WATCHDOG_PROBE_TIMEOUT_S` | `5` | 单次探测总超时 |
| `WATCHDOG_CONNECT_TIMEOUT_S` | `3` | 连接超时 |
| `WATCHDOG_FAIL_THRESHOLD` | `3` | 连续失败阈值 |
| `WATCHDOG_GRACE_S` | `600` | 开机宽限（按 `/proc/uptime`） |
| `WATCHDOG_RESET_BUDGET` | `5` | 30 分钟窗口内允许的武装启动次数 |
| `WATCHDOG_RESET_WINDOW_S` | `1800` | budget 窗口 |
| `WATCHDOG_HEALTHY_RESET_S` | `1800` | 连续健康多久清空 budget |
| `WATCHDOG_OPEN_RETRY_S` | `120` | `EBUSY` 重试总窗口 |
| `WATCHDOG_HEARTBEAT_S` | `300` | `feed ok` 心跳周期 |
| `WATCHDOG_DRY_RUN` | `0` | `1` = 判定只写证据、不停喂 |

状态文件：

| 文件 | 内容 |
|---|---|
| `armed-log` | 每次武装启动一行 epoch；窗口外的行在启动时丢弃 |
| `boot-count` | 窗口内的武装启动次数，便于 `cat` 与证据引用 |
| `pre-reset.log` | 每次判定的时间、原因、uptime、计数（dry-run 与 budget 耗尽也写） |

## 5. 文件

| 文件 | 部署位置 |
|---|---|
| `tbtun-netwatch.py` | `/usr/local/sbin/tbtun-netwatch.py` |
| `tbtun-netwatch.service` | `/etc/systemd/system/tbtun-netwatch.service` |
| `install-65.sh` | 不部署，在 .65 上就地执行 |
| `rollback-65.sh` | 不部署，在 .65 上就地执行 |
| `README.md` | `/usr/local/share/doc/tbtun-netwatch.md` |

## 6. 安装

```sh
# 在 .65 上，payload 与本脚本同目录
scp tbtun-netwatch.py tbtun-netwatch.service install-65.sh root@192.168.123.65:/root/netwatch/
ssh root@192.168.123.65 'sh /root/netwatch/install-65.sh'
```

脚本顺序：前置检查（`RuntimeWatchdogUSec=0`、hold 确实持有 fd）→ 装文件 →
`daemon-reload` → `--selftest`（只读，不碰设备）→ `enable` → 校验 unit 契约
（`Restart=no`）→ 切换（§4.7 的六步，失败自动回到 hold）→ 观察 10 s 并验证
fd。`--dry-run` 只打印动作并跑只读 selftest。

安装不会立即触发判定：如果 `cat /proc/uptime` 小于 600，新服务进入宽限，只喂狗。安装后立刻 `tail -f /var/log/tbtun-watchdog.log`，看到 `boot grace start` 就说明宽限生效；看不到且 uptime 已经大于 600，说明直接进入探测，也是对的。

`--selftest` 可以单独跑，完全安全（只读；`open()` 本身就是武装，所以它不
open 设备）：

```sh
/usr/local/sbin/tbtun-netwatch.py --selftest
/usr/local/sbin/tbtun-netwatch.py --selftest --no-probe   # 不碰网络
```

输出是机器可判定的：

```
SELFTEST device_node PASS (char 243:0)
SELFTEST device_holder PASS (pid=1234(tbtun-netwatch))
SELFTEST timing PASS (grace=600 probe=30x3 hard=44 total=734 feed=5)
SELFTEST systemd_watchdog PASS (RuntimeWatchdogUSec='0')
SELFTEST unit_restart PASS (Restart=no)
SELFTEST unit_fd PASS (pid=1234 holds /dev/watchdog0)
SELFTEST gateway PASS (5/5 code=401)
SELFTEST_RESULT PASS
```

`SKIP` 表示该项在当前状态不可判定（例如 unit 尚未安装），不算失败；任一
`FAIL` 时最后一行是 `SELFTEST_RESULT FAIL`，退出码 3。

## 7. 验证

```sh
systemctl status tbtun-netwatch
journalctl -u tbtun-netwatch -n 30 --no-pager
tail -n 20 /var/log/tbtun-watchdog.log

# 喂狗者确实持有设备
PID=$(systemctl show -p MainPID --value tbtun-netwatch)
ls -l /proc/$PID/fd | grep watchdog

# wdctl 报 busy 是健康状态，不是故障
wdctl /dev/watchdog0
# -> wdctl: cannot read information about /dev/watchdog0: Device or resource busy
```

判读：

- 开机后前 10 分钟：只有 `boot grace start: uptime=...`、每 2 分钟一条 `boot grace: uptime=..., Ns left`、以及 uptime 过 600 时的 `boot grace end: ...; first probe at uptime=630, then every 30s`。**这段时间不应出现任何 `gateway probe failed`**。出现就说明宽限没生效，立即查 `uptime_secs()`；
- 宽限期后：正常探测静默（只有每 5 分钟一条 `feed ok` 心跳），失败时是
  `gateway probe failed 1/3: ...`；
- 出现 `VERDICT:` 就是已经在停止喂狗，44 s 内会复位；
- `first feed ok` 只在新进程第一次写设备时出现一次；`feed ok, uptime=...`
  每 5 分钟一条心跳，用来证明长跑期间一直在喂；
- `open /dev/watchdog0: busy ... retrying` 是切换期间的正常行，不是故障；
- `reset budget exhausted: ... keeping the feed` 表示窗口内武装启动次数已超
  预算：服务**仍在喂狗**，需要人工介入，不会复位；
- `DRY-RUN: ...` 只在 `WATCHDOG_DRY_RUN=1` 时出现，永远不停喂；
- 复位后 `journalctl --list-boots` 多一条，`/var/log/tbtun-watchdog.log` 保留 `VERDICT` 行。

确认宽限是否按 uptime 走，而不是按进程启动时间：

```sh
cat /proc/uptime                       # 与日志里的 uptime 对得上
systemctl show -p ExecMainStartTimestamp tbtun-netwatch
journalctl -u tbtun-netwatch | grep -E "boot grace (start|end)"
```

如果服务是手工 `systemctl start` 且 `cat /proc/uptime` 已大于 600，日志会直接进入探测，没有 `boot grace start`——这是正确行为。

**不要**用 `systemctl stop` 做验证——那会真的复位。

## 8. 回滚

```sh
sh rollback-65.sh --status           # 只读：谁持有 fd、unit 状态、uptime、budget
sh rollback-65.sh --to-hold          # 交回 tbtun-wdt-hold，不复位
sh rollback-65.sh --next-boot-only   # 本次开机保持武装，下次开机不再武装，不复位
sh rollback-65.sh                    # 默认：disable + sync + 重启（唯一彻底退场方式）
```

`--to-hold` 用于正式服务已接管、需要回到临时喂狗者。这里同样不存在"先确认
hold 的 fd、再停正式服务"的顺序：hold 在正式服务持有 fd 时只能拿到 `EBUSY`。
实际顺序是：启动 hold（它在重试）→ 停正式服务 → 15 s 内确认 hold 拿到 fd →
确认失败立即重启正式服务并重新确认 fd。两步之间最多 15 s，小于 44 s 硬件窗口。
hold 是 transient unit，重启后不存在；脚本会检测并给出重建命令。

`--next-boot-only` 是"不付复位代价"的退场方式：`disable` 在前，`stop` 不做，
所以本次运行仍然受保护，下次开机干净。

默认模式先 `disable`，再 `sync`，再 `systemctl reboot --no-block`；60 s 未重启
则 `reboot -f`。**不要**把 `systemctl stop tbtun-netwatch` 当成关掉看门狗：它
只是启动 44 s 倒计时。`CONFIG_MAGIC_SYSRQ` 未编译，不要用 sysrq。

文件清理在复位之后做：

```sh
rm -f /etc/systemd/system/tbtun-netwatch.service \
      /usr/local/sbin/tbtun-netwatch.py \
      /usr/local/share/doc/tbtun-netwatch.md
rm -rf /var/lib/tbtun-netwatch /var/log/tbtun-watchdog.log
systemctl daemon-reload
```

## 9. 已知限制

1. **44 s 不可调**。驱动无 `.set_options`，`wdctl -s` 无效。要 30 s 只能改驱动或换内核。
2. **`nowayout=1` 需要重启**。若一定要编译级 nowayout，只能在内核命令行加 `dw_wdt.nowayout=1` 并重启；当前 `MAGICCLOSE=0` 已提供等效保证，不建议为此重启。
3. **探测只覆盖 192.168.123.254:80**。路由器重启期间会误判，600 s 开机宽限 + 90 s 判定窗口是唯一缓冲。开机 10 分钟后路由器固件升级超过 90 s，仍会触发一次复位。
4. **kmsg 转发依赖网络**。网络故障时告警只留在本地 journal 和日志文件里。
5. **`CONFIG_WATCHDOG_SYSFS` 未设置**，所以没有 `timeout`/`state`/`bootstatus` 可读，`bootstatus` 只能靠 `wdctl` 的 `BOOT-STATUS` 列。
6. **复位是硬复位**，不 unmount、不同步。脚本在停喂前 `sync` 一次，但 44 s 窗口内的写入仍可能丢失。
7. **宽限期不区分"刚开机"和"刚重启"**。每次开机都无条件等 600 s，包括 watchdog 复位后的那次。所以一次真故障的完整闭环是 734 s 复位 + 开机 600 s 宽限 ≈ 22 分钟才可能第二次复位。reset budget 的 5 次上限对应约 2 小时的持续故障。
8. **宽限期内网络故障不会被发现**。这是刻意的取舍：宁可 10 分钟不判定，也不要 44 s 一次的复位循环。需要更早发现时，把 `BOOT_GRACE` 调小，但要同时接受循环变短。

## 10. 事故记录（2026-11-07 08:31 CST）

只读探测时执行了 `os.open("/dev/watchdog0")` 后直接 `close`，武装了看门狗。内核打印：

```
[ 5569.362919] watchdog: watchdog0: watchdog did not stop!
```

当时误判为 `nowayout=1`，随后用 `wdctl` 才看清真相：`MAGICCLOSE 0`、`SETTIMEOUT 0`。写 `V` 无效，第二次 `did not stop`（`[ 5608.944941]`）。

临时处置：`systemd-run --unit=tbtun-wdt-hold --property=Restart=always`，每 5 s 写一字节，维持机器存活。该 unit 是 transient 的，不落盘，重启后消失。

教训，也是本设计存在的理由：**`/dev/watchdog0` 不是只读设备**。`open()` 本身就是一次写操作。
