# 0.4.x `.85` 引用清单与迁移步骤

调查时间：2026-11-08 14:15 UTC。基线：`e723dff`（`release: 0.3.2`）。
本文只记录，不修改仓库文件。

## 1. 现场判定

| 地址 | 本地 ARP | SSH 主机键（known_hosts） | 判定 |
|---|---|---|---|
| `192.168.123.85` | `02:51:22:e5:67:53` REACHABLE/DELAY | `SHA256:MbBBsT5vgc7yzPIYndTWY6PnNwxGnMu1Dw9Bf4Xd82w` | 当前 near |
| `192.168.123.82` | FAILED | 同一主机键 | 旧租约，历史 |
| `192.168.123.84` | FAILED | 同一主机键 | 旧租约，历史 |
| `192.168.123.74` | `94:de:80:be:00:22` REACHABLE | `SHA256:FC60BmjSUvV2UIsINSyjnrtn18iN5eaBdYilyCcmLpU` | 760K 备用 near |

`.82`、`.84`、`.85` 的主机键相同，说明是同一块 LubanCat 换了 DHCP 租约；
`.74` 是另一台机器。`scripts/find-near.sh --no-sweep --no-key-check` 已在本机
返回 `192.168.123.85`（只读邻居表，未登录板子）。

## 2. 版本控制事实（重要）

- `review/wan-experiment/arm64/netwatch-84/` 的 7 个文件在 `e723dff` 中
  **不存在**；它们在 master 工作树里是未跟踪文件：
  `git ls-files review/wan-experiment/arm64/netwatch-84` 输出为空。
  共 1598 行，`git clean -fdx` 会直接删掉。
- `review/wan-experiment/arm64/netwatch-65/` 的 5 个文件是已跟踪文件
  （`4c3623a review: add the 0.2.0 watchdog, 1 GiB and DKMS harnesses`），
  属于 `.65` 历史，不应改写。
- `review/0.3.2-scope-audit.md` 和 `review/0.3.2-release-draft.md` 引用的
  `netwatch-82/install-82.sh` 在全部 git 历史中从未存在
  （`git log --all -- review/wan-experiment/arm64/netwatch-82` 为空）。
  这是过时文档，不是可执行路径。
- `review/0.3.2-release-draft.md` 本身也不在 `e723dff` 中（本地未跟踪），
  迁移时不要把它当发布基线。

## 3. 需要更新的引用

| 文件 | 当前内容 | 处理 |
|---|---|---|
| `review/wan-experiment/arm64/netwatch-84/install-84.sh` | 文件名、`.84`、payload 文件名 | 改名 `install-85.sh`，注释与 payload 名改 `.85` |
| `review/wan-experiment/arm64/netwatch-84/rollback-84.sh` | 文件名、`.84` | 改名 `rollback-85.sh`，注释改 `.85` |
| `review/wan-experiment/arm64/netwatch-84/nowayout-84.sh` | 文件名、`.84` | 改名 `nowayout-85.sh`，注释改 `.85` |
| `review/wan-experiment/arm64/netwatch-84/netwatch-84.conf` | 文件名 | 改名 `netwatch-85.conf`；安装目标 `/etc/tbtun-netwatch.conf` 不变 |
| `review/wan-experiment/arm64/netwatch-84/tbtun-netwatch.py` | docstring/Handover 写 `.84` | 只改注释；设备路径与 44 s 契约不变 |
| `review/wan-experiment/arm64/netwatch-84/tbtun-netwatch.service` | unit 名与路径 | **不改**；改了会孤儿化正在运行的喂狗者 |
| `review/wan-experiment/arm64/netwatch-84/tbtun-netwatch.logrotate` | 无 IP | 不改 |
| `benches/adaptive/lib.sh:37-39` | `TBT_NEAR=root@...82`、`TBT_NEAR_IP=...82` | 默认改 `.85` |
| `benches/short.sh:4-5` | 示例 `.82` | 示例改 `.85` |
| `benches/adaptive/README.md:46-47` | 示例 `.82` | 示例改 `.85` |
| `benches/adaptive/drop_ppm.sh:9-10` | 示例 `.82` | 示例改 `.85` |
| `benches/adaptive/netem.sh:13-14` | 示例 `.82` | 示例改 `.85` |
| `benches/adaptive/matrix.sh:12-13` | 示例 `.82` | 示例改 `.85` |
| `benches/MICROBENCH.md:201-202,257` | `.82` 历史命令与历史测量 | **保留历史行**；只把“可重跑”的示例改 `.85`，并注明年份 |
| `review/wan-experiment/arm64/sclc-verify/verify-0.3.0.sh:26,65` | `NEAR_HOST=...82` | 默认改 `.85` |
| `review/wan-experiment/arm64/prod-1g/run-1g.sh:64,106` | `NEAR_HOST=...65` | 默认改 `.85`（比 `.82` 更旧，容易漏） |
| `review/0.3.2-scope-audit.md:43,147,166,264` | `.82`、`netwatch-82` | 改为 `.85`/`netwatch-85`，或标注“历史/不存在” |
| `review/0.3.2-release-draft.md`（本地未跟踪） | `.82`、`netwatch-82` | 同左；先决定是否入库 |
| `~/.ssh/config` | 只有 `Host 45.11.1.62` 和 `192.168.123.84` | 增加/改成 `192.168.123.85`（仓库外） |
| `review/dot65-watchdog-design.md`、`review/s2-merged-design.md`、`netwatch-65/*` | `.65` 历史设计 | **不改**；只在需要时加“历史”标记 |
| `review/wan-experiment/arm64/*/SUMMARY.md`、`results/**` | `.82`/`.65` 证据 | **不改**；证据不可重写 |
| `review/wan-experiment/arm64/fix44-*/SUMMARY.md`、`sclc-verify-*/SUMMARY.md` | `.82` 运行记录 | **不改**；历史证据 |

## 4. `netwatch-84` → `netwatch-85` 迁移步骤

1. 冻结并记录：
   `git status --short`；`scripts/find-near.sh --no-sweep --no-key-check`；
   `ssh-keygen -F 192.168.123.85 -l`。确认输出是 `.85` 和
   `SHA256:MbBB...`，不是同名 MAC 的其他机器。
2. 先把未跟踪套件入库，否则改名/提交会丢文件：
   `git add review/wan-experiment/arm64/netwatch-84`
3. 目录与文件改名（`git mv`，保留历史）：
   ```
   git mv review/wan-experiment/arm64/netwatch-84 \
          review/wan-experiment/arm64/netwatch-85
   git mv .../netwatch-85/install-84.sh  .../netwatch-85/install-85.sh
   git mv .../netwatch-85/rollback-84.sh .../netwatch-85/rollback-85.sh
   git mv .../netwatch-85/nowayout-84.sh .../netwatch-85/nowayout-85.sh
   git mv .../netwatch-85/netwatch-84.conf .../netwatch-85/netwatch-85.conf
   ```
4. 只改 `.84` 字面量与 payload 文件名；unit 名、安装路径、日志路径、
   `WATCHDOG_*` 默认值保持不变。用
   `grep -n '\.84\|192\.168\.123\.84'` 逐条确认，禁止批量 sed 到历史目录。
5. 更新第 3 节列出的活动默认值；历史证据与 `.65` 文档不动。
6. 在 `.85` 上先只读检查：
   ```
   sh install-85.sh --dry-run          # 只跑 selftest，不 open 设备
   rollback-85.sh --status             # holder/unit/uptime/budget
   systemctl list-unit-files 'tbtun*'
   ```
   如果 `tbtun-wdt-hold.service` 存在或已 enabled，先记录它的状态；
   `install-84.sh` 的“无前任喂狗者”假设对这个板子可能不成立。
7. 安装顺序：`--install-only` → `--enable` →（批准后）`--arm`。
   `--arm` 在另一个进程持有 `/dev/watchdog0` 时会拒绝；不要先 stop 旧
   feeder 再 arm，停旧 feeder 就是 44 s 倒计时。
8. 回滚：`rollback-85.sh --status` → `--next-boot-only` 或 `now`。
   `now` 会重启；重启是唯一退场方式。若 `nowayout=1`，`'V'+close` 和
   `WDIOS_DISABLECARD` 都失败，风险更高。

## 5. 风险

- `netwatch-84` 未入库，任何清理/切分支都可能丢套件；先 `git add`。
- `.82`/`.84` 已是 `FAILED` 租约；脚本默认值不改会在 SSH 阶段失败，
  或更糟：租约被别的设备复用后打到错误主机。IP 不能单独作为身份。
- 批量替换会污染 `results/**` 与历史 SUMMARY；这些是证据，不是配置。
- `netwatch-82` 文档引用是幽灵路径；照着它会找不到安装脚本。
- `.85` 与 `.82/.84` 主机键相同只能证明“同一块板子换租约”，不能证明
  当前 IP 一定是它；先 MAC，后主机键。
- 旧喂狗者/`tbtun-wdt-hold` 可能与新 unit 并存；设备只允许一个打开者，
  但“第二个 feeder 一直 EBUSY 重试”会掩盖交接失败。
- 改名 unit 或安装路径会让 `systemctl` 找不到正在运行的喂狗者；不要做。
