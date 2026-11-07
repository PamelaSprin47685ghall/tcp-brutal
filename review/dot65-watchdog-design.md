# `.65` 硬件 watchdog 设计（0.2.0 长稳验收环境准备）

> 只读设计说明。**唯一安装源**：`review/wan-experiment/arm64/netwatch-65/`
> （`tbtun-netwatch.py`、`tbtun-netwatch.service`、`install-65.sh`、
> `rollback-65.sh`、`README.md`）。本文保留为设计说明；实现细节与实测数字
> 以该目录的 README 为准。**不安装、不对 `.65` 做任何写操作。**
> 当前 `.65` 的唯一喂狗者是 `tbtun-wdt-hold.service`；在正式服务验证能
> `open("/dev/watchdog0")` 成功之前，**不停止、不重启 `.65`**。
> 本文不进入 0.1.0 发布内容，不修改 tbtun 源码。

---

## 0. 生命线与硬约束

| 约束 | 内容 |
|---|---|
| 唯一喂狗者 | 当前是 `tbtun-wdt-hold.service`。它是 `.65` 的生命线。 |
| 禁止动作 | 在正式服务未确认持有 `/dev/watchdog0` fd 之前，停止 hold、重启 `.65`、或让任何进程短暂独占失败。 |
| 超时 | **固定 44 s**。`SETTIMEOUT=0`，不尝试改。 |
| 喂狗间隔 | **5 s**。必须 `< 44/3 ≈ 14.67 s`。 |
| 宽限期 | **600 s**。开机后只喂狗，不探测、不累计失败。防止重启循环。 |
| 探测间隔 | **30 s**。失败只计数，不立即停止喂狗。 |
| 连续失败阈值 | **3 次**（90 s 确认窗口）。达到后停止喂狗，让硬件在 44 s 内复位。 |
| 总容忍 | **≈734 s = 600 + 90 + 44**（从上电到硬件复位）。 |
| `nowayout` | 运行时不可设。`MAGICCLOSE=0` 已提供等效保证：关闭 fd 不停狗。 |
| `dw_wdt.nowayout=1` | 只能在**重启后**通过内核命令行生效；当前方案**不依赖**它。 |
| 回滚 | `MAGICCLOSE=0` 下无法优雅停止 watchdog；**唯一回滚是重启 `.65`**。 |
| 重启路径 | `sync; systemctl reboot`；60 s 未重启用 `reboot -f`。`CONFIG_MAGIC_SYSRQ` 未编译，**不要用 sysrq**。 |
| reset budget | **5 次 / 30 分钟**。窗口内已满 5 次时继续喂狗、不再停止；宽限期内失败不计入。 |

---

## 1. 为什么必须是硬件 watchdog

纯 systemd 服务（`Restart=always` + `systemctl reboot`）有三个漏洞：

1. 服务被 `kill -9` 后，systemd 会重启它；如果重启逻辑本身坏了，没人复位机器。
2. 内核 panic/hang 时，用户态服务根本不运行，无法执行 `systemctl reboot`。
3. systemd 自己可以喂 `/dev/watchdog0`（`RuntimeWatchdogUSec`），掩盖用户态故障。

硬件 watchdog 把“复位”交给 SoC：只要 `/dev/watchdog0` 在 44 s 内没有被
`WDIOC_KEEPALIVE`，硬件就拉复位。它与内核是否调度、systemd 是否存活无关。

---

## 2. 硬件事实（待实测确认）

| 项 | 取值 | 确认方式 |
|---|---|---|
| 设备节点 | `/dev/watchdog0` | `ls -l /dev/watchdog*` |
| 平台 | RK3588（`uboot-evb_rk3588`），aarch64 | `/proc/device-tree/model` |
| 驱动 | 待实测（预期 `dw_wdt`） | `readlink -f /sys/class/watchdog/watchdog0/device/driver` |
| timeout | **44 s**（固定） | `wdctl /dev/watchdog0` 或 `ioctl WDIOC_GETTIMEOUT` |
| `MAGICCLOSE` | **0** | `wdctl /dev/watchdog0`；`ioctl WDIOC_GETSUPPORT` |
| `nowayout` | 运行时不可设 | `cat /sys/module/dw_wdt/parameters/nowayout`（若模块存在） |
| `WDIOC_GETBOOTSTATUS` | 待实测是否支持 `WDIOF_CARDRESET` | 启动时读一次并写日志 |
| sysrq | **未编译** | `CONFIG_MAGIC_SYSRQ` 不存在；不要写 `/proc/sysrq-trigger` |

**`nowayout` 的准确表述**：当前运行时不能把 `nowayout` 从 0 改成 1。等效保证来自
`MAGICCLOSE=0`：即使进程正常 `close(fd)`，驱动也不会停狗。若要显式 `nowayout=1`，
只能在**下次重启**时给内核命令行加 `dw_wdt.nowayout=1`；当前方案不依赖它，也不
要求改内核命令行。

---

## 3. 正式服务

### 3.1 unit

```ini
# /etc/systemd/system/tbtun-netwatch.service
[Unit]
Description=tbtun acceptance hardware watchdog feeder
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
EnvironmentFile=-/etc/tbtun-netwatch.conf
ExecStart=/usr/local/sbin/tbtun-netwatch.py
Restart=no

[Install]
WantedBy=multi-user.target
```

- `Restart=no` 是硬要求：喂狗者崩溃后不重启，让硬件复位。
- **不要** `WatchdogSec=`：那是 systemd 的软件 watchdog，会重启服务，不是硬件复位。
- **不要** `Restart=always`：那会在喂狗者崩溃后重新喂狗，掩盖故障。

### 3.2 脚本行为

`/usr/local/sbin/tbtun-netwatch.py`（实现：
`review/wan-experiment/arm64/netwatch-65/tbtun-netwatch.py`，Python 3）：

```
1. open("/dev/watchdog0", O_WRONLY)
   - EBUSY → 每 2 s 重试（hold 还持有 fd）
   - 其他错误 → 写日志，exit 2
2. 读 WDIOC_GETBOOTSTATUS，写日志（上次是否 watchdog 复位）
3. 主循环，0.5 s tick：
   - 每 5 s：ioctl(fd, WDIOC_KEEPALIVE, 0)
   - 开机后 600 s 内：只喂狗，不探测、不累计失败（宽限期）
   - 600 s 后每 30 s：启动一次非阻塞探测（curl，超时 10 s）
   - 探测结束后：解析状态码/正文，更新连续失败计数
   - 连续失败 >= 3：检查 reset budget（5 次 / 30 分钟）
       - budget 未满：写日志 + sync + 写 last-trip；close(fd)；exit 3
       - budget 已满：写告警，继续喂狗，不再停止
4. 退出即复位：MAGICCLOSE=0 保证 close 不停狗；硬件在 44 s 内复位。
```

关键点：

- **喂狗与探测解耦**：探测用 `subprocess.Popen` 非阻塞启动，主循环每 0.5 s
  检查一次；探测跑 10 s 也不会错过 5 s 喂狗。单线程、无锁、无共享状态。
- **宽限期**：开机后 600 s 内只喂狗。此时网络可能还没起来，探测必然失败；
  如果此时咬，就会形成“开机 → 探测失败 → 复位”的重启循环。宽限期内的
  任何失败都不累计、不进入 reset budget。
- **HTTP 失败只计数**：单次失败不停止喂狗，只有连续 3 次才停止。
- **reset budget**：30 分钟内第 5 次 watchdog 复位后，继续喂狗、不再停止，
  只写告警。硬件 watchdog 不能“半咬”，所以 budget 的实现是“不再停止喂狗”。
- **停止喂狗 = 退出**：不调用 `systemctl reboot`，不写 sysrq。硬件复位是设计
  目标。`close(fd)` 后 `MAGICCLOSE=0` 让驱动继续计时。
- **panic/被 kill**：fd 随进程消失；没人喂；硬件复位。这是设计目标。

**实现状态**：完整实现是 `review/wan-experiment/arm64/netwatch-65/`，包含
600 s 宽限期与 reset budget。早期草案 `review/tbtun-watchdog*` 已删除。
安装前逐条核对本文 §3.2、§6、§9.1 与该目录 README 的时序是否一致。

### 3.3 探测判据（待实测填入）

| 变量 | 默认 | 含义 | 待实测确认 |
|---|---|---|---|
| `WATCHDOG_PROBE_URL` | `http://192.168.123.254:80/` | 探测目标 | 是否监听 80、是否需要 `Host` 头 |
| `WATCHDOG_METHOD` | `GET` | HTTP 方法 | `HEAD` 是否被接受 |
| `WATCHDOG_PROBE_TIMEOUT_S` | `10` | 连接 + 读取超时 | 路由器正常响应时间 |
| `WATCHDOG_EXPECT_STATUS` | 空 | 正常状态码集合（如 `200-399`）；空 = 任意 2xx/3xx | 实测状态码；401/403 是否也算“活着” |
| `WATCHDOG_EXPECT_BODY_REGEX` | 空 | 正文匹配；空 = 不检查正文 | 正文前 200 字节；是否返回重定向页 |
| `WATCHDOG_GRACE_S` | `600` | 开机宽限期；只喂狗、不探测、不累计失败 | — |
| `WATCHDOG_RESET_BUDGET` | `5` | 30 分钟窗口内允许的 watchdog 复位次数 | — |
| `WATCHDOG_RESET_WINDOW_S` | `1800` | reset budget 的滑动窗口 | — |

**判据语义**（三条同时成立才算“正常”）：

1. TCP 连接在 `WATCHDOG_PROBE_TIMEOUT_S` 内建立；
2. HTTP 状态码落在 `WATCHDOG_EXPECT_STATUS`（空则 2xx/3xx）；
3. `WATCHDOG_EXPECT_BODY_REGEX` 非空时，正文匹配。

任一条不成立记一次失败。**不用 `curl -f`**：它把 4xx/5xx 一律当失败，而路由器
返回 401/403 仍可能说明本机网络栈正常。必须显式解析状态码。

**实测回填前不写死任何状态码或正文。** 实测结果写入
`review/dot65-watchdog-probe.txt`，再把结论填进上表与
`review/wan-experiment/arm64/netwatch-65/README.md` 的判据常量。

---

## 4. 切换顺序（hold → 正式服务）

**前提**：hold 正在运行并持有 `/dev/watchdog0`。正式服务启动后会 `EBUSY` 重试，
这是预期行为，不是失败。

```sh
# 0. 记录当前 hold 的状态与 fd
systemctl status tbtun-wdt-hold --no-pager
HOLD_PID=$(systemctl show -p MainPID --value tbtun-wdt-hold)
ls -l /proc/$HOLD_PID/fd | grep watchdog

# 1. 启动正式服务；它会 EBUSY 重试，不会抢走 hold 的 fd
systemctl start tbtun-netwatch

# 2. 确认正式服务已持有 /dev/watchdog0 的 fd
NEW_PID=$(systemctl show -p MainPID --value tbtun-netwatch)
test -n "$NEW_PID" && test "$NEW_PID" != 0
ls -l /proc/$NEW_PID/fd | grep -F '/dev/watchdog0'
# 期望：一行指向 /dev/watchdog0；没有就停止，不要动 hold

# 3. 确认新服务已经喂过一次狗（日志有 "feed" 且无 EBUSY）
journalctl -u tbtun-netwatch -n 20 --no-pager

# 4. 停 hold；此时 fd 释放，正式服务在下一次重试时 open 成功
systemctl stop tbtun-wdt-hold

# 5. 观察 ≥10 s，确认正式服务日志持续有 feed
sleep 10
journalctl -u tbtun-netwatch --since '-15s' --no-pager

# 6. 失败则立即重启 hold（hold 会重新 open，继续续命）
#    systemctl start tbtun-wdt-hold
```

**禁止**：先停 hold 再启动正式服务。两步之间最多 44 s，任何 open 失败都会复位。

**验证 fd 的替代方法**（不依赖 `/proc`）：

```sh
# 正式服务日志应打印 "opened /dev/watchdog0, timeout=44s, magicclose=0"
journalctl -u tbtun-netwatch -n 5 --no-pager
```

脚本在 open 成功后必须打印这一行；没有这一行就不算持有 fd。

---

## 5. 回滚

`MAGICCLOSE=0` 下**无法优雅停止 watchdog**。`systemctl stop tbtun-netwatch`
只会关闭 fd；驱动继续计时；44 s 后硬件复位。因此：

| 场景 | 动作 |
|---|---|
| 正式服务配置错误，但 hold 仍在 | 不要停正式服务；先修配置，再按 §4 重新切换。若必须停，先启动 hold（它会 EBUSY 重试），确认 hold 持有 fd 后再停正式服务。 |
| 正式服务已接管，需要回到 hold | 先 `systemctl start tbtun-wdt-hold`（EBUSY 重试）→ 确认 hold 的 fd → 再停正式服务。两步之间不能超过 44 s。 |
| 需要彻底移除 watchdog | **唯一安全方式：`sync; systemctl reboot`**。重启后由 bootloader/内核处理；不要在重启前停喂狗者。 |
| 重启 60 s 未完成 | `reboot -f`。**不要用 sysrq**（`CONFIG_MAGIC_SYSRQ` 未编译）。 |

**绝对不要**写“`systemctl stop tbtun-netwatch` 即可停用”。那是错的：它会启动
44 s 复位倒计时。

---

## 6. 宽限期、reset budget 与复位归因

### 6.1 宽限期 600 s

- 从上电/服务启动起算，`WATCHDOG_GRACE_S=600` 内只喂狗：
  **不探测、不累计失败、不触发复位**。
- 理由：开机后网络、路由、上游路由器都可能还没就绪；此时探测必然失败。
  如果立即咬，就会形成“开机 → 探测失败 → 复位 → 再开机”的循环。
- 宽限期内的任何失败**不进入 reset budget**。
- 宽限期结束后的第一次探测从 `t=600 s` 开始；连续 3 次失败最早在
  `t=690 s` 停止喂狗，硬件在 `t≈734 s` 复位。

### 6.2 reset budget（5 次 / 30 分钟）

- 实现统计**武装启动次数**：`main()` 打开设备前把
  `/var/lib/tbtun-netwatch/boot-count` 加一；连续 30 分钟探测健康才清零。
  这与探测结果无关，宽限期内的 HTTP 失败不会额外消耗 budget。
- 启动时读取最近 `WATCHDOG_RESET_WINDOW_S=1800` 秒内的记录。
- 若窗口内已有 `WATCHDOG_RESET_BUDGET=5` 次：
  **继续喂狗，不再停止**；只写告警，等待人工介入。
- 硬件 watchdog 不能“半咬”：budget 的实现就是“不再停止喂狗”，而不是
  “停止喂狗但期望不复位”。
- 宽限期内的失败不计入 budget；budget 只统计武装启动次数。
- budget 是防重启循环的最后一道闸；它不阻止人工 `sync; systemctl reboot`。

### 6.3 复位归因

- netwatch 在停止喂狗前写 `GIVING UP: gateway ... hardware watchdog will
  reset ...`，并 `sync`。
- `boot-count` 记录武装启动次数，用于 budget 与“刚复位过”的归因；它不改变
  宽限期、不阻止复位。

| 证据 | 来源 | 说明 |
|---|---|---|
| `WDIOC_GETBOOTSTATUS` 的 `WDIOF_CARDRESET` | 硬件 | 最可靠；启动时读一次并写日志 |
| `/var/lib/tbtun-netwatch/boot-count` | 文件 | 武装启动次数；budget 依据 |
| `journalctl -u tbtun-netwatch` | journal | 最后一次 `GIVING UP` |
| `/dev/kmsg` 最后一行 | netconsole/串口 | `GIVING UP: gateway ... hardware watchdog will reset` |

---

## 7. 告警与日志

| 通道 | 写法 | 用途 |
|---|---|---|
| journal | `SyslogIdentifier=tbtun-netwatch`（脚本 `print` 到 journal） | 常规取证 |
| kmsg | `printf '<4>tbtun-netwatch: %s\n' "<msg>" > /dev/kmsg` | journal 不可用时的串口/netconsole 兜底 |
| 文件 | `/var/log/tbtun-watchdog.log`，append，UTC 时间戳 | 长稳后分析；`logrotate` 每日轮转、保留 7 份、`maxsize 10M` |
| 复位前 | `GIVING UP` 日志 + `sync` | 硬件复位后第一手证据 |
| 复位计数 | `/var/lib/tbtun-netwatch/boot-count` | budget 与归因 |

告警去重：只在**首次失败**、**达到阈值**、**停止喂狗前**各写一条；中间失败只写
文件日志，避免 24 h 内刷屏。

---

## 8. 验证（真实复位，实验窗口外）

| 检查 | 操作 | 期望 |
|---|---|---|
| open 重试 | 正式服务启动时 hold 仍持有 fd | 日志每 2 s 一条 `EBUSY, retrying`；hold 停止后 open 成功 |
| 宽限期 | 启动后立即用坏 URL | 600 s 内只有 `feed` 与 `grace` 日志；失败计数保持 0；不复位 |
| 干跑 | `WATCHDOG_DRY_RUN=1` + 坏 URL，等过宽限期 | 3 次失败后写告警与 `pre-reset.log`，**不停止喂狗、不复位** |
| 正常路径 | 指向路由器 | 连续失败计数保持 0；日志每 30 s 一行 |
| 停止喂狗复位 | `WATCHDOG_DRY_RUN=0` + 坏 URL，等过宽限期 | 600 s 后 90 s 内停止喂狗；硬件在 44 s 内复位；总时间 ≈734 s |
| `kill -9` 喂狗者 | `kill -9 <pid>` | `Restart=no` 不重启；硬件在 44 s 内复位 |
| panic | 内核 panic（若可注入） | 硬件在 44 s 内复位 |
| 复位归因 | 启动后读 `WDIOC_GETBOOTSTATUS` | `WDIOF_CARDRESET` 置位；日志写明 |
| reset budget | 30 分钟内制造 5 次复位，再坏 URL | 第 5 次后继续喂狗、不再停止；日志写明 budget exhausted |
| systemd 不喂狗 | `systemctl show -p RuntimeWatchdogUSec` | `0` |

**干跑语义**：`WATCHDOG_DRY_RUN=1` 时，连续失败 3 次只写日志与
`pre-reset.log`，**继续喂狗**；用于在不复位的前提下验证计数与告警。
干跑仍然遵守 600 s 宽限期。

---

## 9. 与验收流程的接口

`MAGICCLOSE=0` 改变了一个关键前提：**“停止喂狗者”等于“启动 44 s 复位倒计时”**。

### 9.1 时序（从上电/服务启动开始）

| 时间 | 行为 | 失败计数 | 复位 |
|---|---|---|---|
| 0–600 s | 宽限期：每 5 s 喂狗，不探测 | 不累计 | 不触发 |
| 600 s | 第一次探测 | 0 | — |
| 600–690 s | 每 30 s 探测；连续失败累计 | 1 → 2 → 3 | — |
| 690 s | 第 3 次连续失败，停止喂狗 | 3 | 倒计时开始 |
| 734 s | 硬件 44 s 超时 | — | **复位** |

总容忍 ≈ **734 s = 600 + 90 + 44**。任何一次探测成功都把连续失败计数清零。
reset budget（5 次 / 30 分钟）在 600 s 之后才可能被消耗；窗口内第 5 次
复位后，喂狗者继续喂狗、不再停止。

### 9.2 验收规则

- **1 GiB 验收**：喂狗者**保持运行**；不做坏 URL 测试，不停止喂狗。
- **DKMS 验收**：喂狗者**保持运行**；`rmmod`/`modprobe` 期间不允许停止喂狗。
- **24 h 长稳**：喂狗者运行；启用前把 `systemctl status`、
  `netwatch-65/README.md` 的判据常量、`wdctl`、`MAGICCLOSE` 复制进证据目录。
- **证据收集**：**不要停喂狗者**；先收集 `dmesg`/pstore/netconsole，再决定是否
  回滚。停喂狗者会启动复位倒计时。
- **判定**：长稳的失败判据不是“复位了几次”，而是“复位后能否继续实验”。若
  硬件 watchdog 在 24 h 内触发超过 2 次，视为 near 侧不稳定，按验收失败处理，
  不重试。

---

## 10. 待实测结果回填

实测项（只读，不在 `.65` 上写）：

1. `/dev/watchdog0` 的驱动名、timeout、`MAGICCLOSE`、`nowayout` 当前值。
2. `WDIOC_GETBOOTSTATUS` 是否支持 `WDIOF_CARDRESET`。
3. 路由器 `192.168.123.254:80` 的：是否监听、`GET /` 状态码、`HEAD` 是否支持、
   正文前 200 字节、响应时间、是否需要 `Host` 头、是否 3xx。

回填位置：

- 本文 §2 与 §3.3 的“待实测确认”列。
- `review/wan-experiment/arm64/netwatch-65/README.md` 与
  `tbtun-netwatch.py` 的探测判据常量（以该目录为准）。
- 证据文件 `review/dot65-watchdog-probe.txt`（`wdctl`、`nowayout`、
  `WDIOC_GETBOOTSTATUS`）与 `review/dot65-router-probe.txt`（`curl -v` 原始输出）。

**在实测结果回来前，本文不写死任何状态码、正文或 timeout。**
