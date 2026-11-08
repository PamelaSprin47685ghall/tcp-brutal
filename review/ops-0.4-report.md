# 0.4.x 运维与恢复能力报告

时间：2026-11-08 14:15 UTC。基线：`e723dff`（`release: 0.3.2`）。分支/worktree：`w-ops-0.4` / `/tmp/tbt-ops`。本轮只写脚本与报告，没有登录 `.74/.85`，没有改 master 工作树，没有执行任何回滚/重启。

## 交付物

| # | 内容 | 路径 |
|---|---|---|
| 1 | near IP 自动发现/恢复 | `scripts/find-near.sh` |
| 2 | 760K 备用 near 检查 + 步骤 | `scripts/backup-near-check.sh`、`review/ops-0.4-backup-near-760k.md` |
| 3 | watchdog 回滚演练 | `scripts/watchdog-rollback-drill.sh` |
| 4 | `.85` 引用清单/迁移 | `review/ops-0.4-ip85-migration.md` |
| 5 | 0.2.4 tag + 本机 D 态 | `scripts/local-dstate-check.sh`、`review/ops-0.4-tag024-and-dstate.md` |

## 现场结论

- 本机 ARP：`.85` = `02:51:22:e5:67:53` REACHABLE；`.82/.84` FAILED；`.74` = `94:de:80:be:00:22` REACHABLE。
- known_hosts：`.82/.84/.85` 同一 ed25519 主机键 `SHA256:MbBB...`；`.74` 是 `SHA256:FC60...`。`.85` 是当前 near。
- `find-near.sh --no-sweep --no-key-check` 已返回 `192.168.123.85`。

## 1. near 自动发现

`scripts/find-near.sh` 依次查：本地 `ip neigh`（MAC）→ dnsmasq/ISC lease 文件或只读路由器 SSH → 私网 `/24` ICMP 扫描 → `ssh-keyscan` 主机键。只接受私网 `/24`；不 sudo、不写路由/lease/known_hosts、不登录、不 open `/dev/watchdog0`；整体超时默认 60 s。stdout 只有一个 IP，诊断走 stderr。`--strict-key` 可要求主机键匹配；默认 MAC 匹配即可，键不匹配仅告警。

## 2. 760K 备用 near

- 只读检查：`scripts/backup-near-check.sh`；`--plan` 只打印步骤。
- root 免密**本轮未验证**：known_hosts 有 `.74` 的键，但没有 Host 配置；用 `ssh -o BatchMode=yes ... 'id -u'` 判定，失败再 `ssh-copy-id`。
- mihomo：在 `rules:` 顶部加 `IP-CIDR,45.11.1.62/32,DIRECT,no-resolve`，`mihomo -t` 后 reload/restart；验证 `ip route get 45.11.1.62` 不是 `dev Meta`。若仍是 Meta，用仓库验证过的 `ip rule add to 45.11.1.62/32 lookup main pref 100` 兜底并持久化。
- 回滚：`del 1` → `rmmod tbtun` → 删 pref-100 规则/marker → 恢复 mihomo 备份并重启 → 确认原路由；root key 只删对应公钥行。
- 风险：restart mihomo 会断所有代理连接；两个 near 不能同时在线；x86_64 模块不能复用 `.85` 的 aarch64 `.ko`。

## 3. watchdog 回滚演练

`scripts/watchdog-rollback-drill.sh` 默认只读：收集 unit/holder/uptime/budget/nowayout/cmdline/sysfs，跑 `rollback --status` 和两个 `--dry-run`。`--verify` 在重启后比较 boot_id、unit disabled、无 holder。`--execute-next-boot` 需 `--yes`；`--execute-now` 还需 `--i-know-this-reboots`，并交给 rollback 重启。

契约（脚本头部重复）：`MAGICCLOSE=1`，close 不带 `'V'` 不停表；停 unit 就是 44 s 倒计时；**重启是唯一退场**。`nowayout=1` 会让 `'V'+close` 和 `WDIOS_DISABLECARD` 都失败，喂狗者一死就是硬复位；它本身不武装设备，但 `RuntimeWatchdogUSec` 必须保持 0。sysrq 未编译，不要用。

关键发现：`netwatch-84/` 的 7 个文件在 `e723dff` 中**未跟踪**（1598 行），`git clean -fdx` 会删掉；`netwatch-82` 文档引用在全部 git 历史中不存在。先 `git add` 再迁移。

## 4. `.85` 引用清单

必须改的活动默认值：`benches/adaptive/lib.sh:37-39`、`benches/short.sh:4-5`、`benches/adaptive/{README,drop_ppm,netem,matrix}.sh`、`sclc-verify/verify-0.3.0.sh:26,65`、`prod-1g/run-1g.sh:64,106`（还是 `.65`）。`netwatch-84/*` 整个目录改名为 `netwatch-85`，文件名 `.84`→`.85`；unit 名、安装路径、日志路径不变。历史 `results/**`、SUMMARY、`.65` 文档不改。迁移前先 `git add` 未跟踪套件；改名后 `--dry-run`、`--install-only`、`--enable`、`--arm`，每步检查 holder，禁止先停旧 feeder。

## 5. 0.2.4 tag 与 D 态

- `v0.2.4` 是 annotated tag，指向 `1b64df0`，tag 消息 `tbtun 0.2.4`，版本头 0.2.4；`w-0.2.4` worktree 干净；origin **没有**该 tag（本地 tag）。重建步骤：确认远端为空 → `git tag -d` → 在 `1b64df0` 上 `git tag -a`；已有远端 tag 时禁止删除/force-push。
- 本机确实处于 D 态：`rmmod tbtun`（pid 1599742）、写 `/proc/net/tbtun/config` 的 bash（pid 1599330）持续 D；`/sys/module/tbtun/refcnt = -1`，holders 空，`/proc/net/tbtun` 已消失，uptime 617292 s。与旧证据 `near-deadlock.txt` 同型。
- 清理只有：保存证据 → `sync; sync; sync` → `systemctl reboot`；120 s 未 reboot 再从串口 `reboot -f`。不要二次 `rmmod`、不要再写 proc、不要 `kill -9`。重启后确认模块/proc/D 态都干净再继续。

## 风险摘要

1. `.82/.84` 是失效租约；IP 不能单独作为身份，先 MAC 后主机键。
2. `netwatch-84` 未入库；清理分支/工作树可能直接丢套件。
3. watchdog 停 unit 不是关闭；`nowayout=1` 把最后软件退路也去掉。
4. mihomo 的 DIRECT 只影响自身选路；TUN auto-route 抢先时仍需 pref-100。
5. 本机 tbtun refcnt 已损坏；在 0.4.x 修 teardown 前不要再次加载。

已验证：4 个脚本 `sh -n` 通过；`find-near.sh` 只读 ARP 返回 `.85`；`backup-near-check.sh --plan`、两个 `--help` 正常。未做真机验证。
