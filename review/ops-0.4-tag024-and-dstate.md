# 0.2.4 本地 tag 与 D 态重启检查

调查时间：2026-11-08 14:15 UTC。基线：`e723dff`。
本轮只做只读检查；没有删 tag、没有重启、没有动 `/proc/net/tbtun`。

## 1. `v0.2.4` 本地 tag

### 1.1 现状证据

```
$ git cat-file -t v0.2.4
tag
$ git cat-file -p v0.2.4
object 1b64df029b2023ed2487f2ca8701e43511427453
type commit
tag v0.2.4
tagger Kunwei Zhang <kunweiz92@gmail.com> 1794122374 +0800

tbtun 0.2.4
```
- 目标提交：`1b64df0 fix: backpressure the FEC outq instead of dropping records`
  （2026-11-08 12:26:05 +0800）。
- tagger 时间：`2026-11-08T15:19:34+0800`。
- `git show v0.2.4:tbtun/tbt_uapi.h`：`TBT_VERSION_MAJOR 0`、
  `MINOR 2`、`PATCH 4`，与 tag 名一致。
- `git branch --contains v0.2.4`：本地 `w-0.2.4`；worktree
  `~/Desktop/vibe/tcp-brutal-0.2.4` 在 `1b64df0`，`git status --porcelain`
  为空。
- `git ls-remote --tags origin 'refs/tags/v0.2.4*'` 输出为空（rc=0）：
  **origin 上没有这个 tag，当前是本地 tag**。
- tag 未签名：`git tag -v v0.2.4` 会走 GPG 校验并失败；这是 annotated
  tag，不是 signed tag。

### 1.2 检查步骤（只读）

```
git cat-file -t v0.2.4
git rev-parse v0.2.4^{}
git show v0.2.4:tbtun/tbt_uapi.h | grep -E 'TBT_VERSION_(MAJOR|MINOR|PATCH)'
git branch --contains v0.2.4
git -C ~/Desktop/vibe/tcp-brutal-0.2.4 status --porcelain
git ls-remote --tags origin 'refs/tags/v0.2.4*'
```

### 1.3 清理/重建步骤（不执行）

只在“tag 指向错误提交且 origin 没有该 tag”时重建；顺序不能反：
```
# 1) 先确认远端为空；有远端 tag 时停止，不要 force-push
git ls-remote --tags origin 'refs/tags/v0.2.4*'

# 2) 删本地 tag（annotated tag 必须先删）
git tag -d v0.2.4

# 3) 在正确提交上重建 annotated tag
git tag -a v0.2.4 1b64df029b2023ed2487f2ca8701e43511427453 -m 'tbtun 0.2.4'

# 4) 复核
git cat-file -t v0.2.4
git rev-parse v0.2.4^{}
git show v0.2.4:tbtun/tbt_uapi.h | grep -E 'TBT_VERSION_'

# 5) 只有在批准后才推
git push origin refs/tags/v0.2.4
```
约束：
- 远端已有 `v0.2.4` 且指向不同提交时，**不要**删除/移动/force-push；
  用新 tag（如 `v0.2.4-<fix>`）或发公告。
- 若发布政策要求签名 tag，`git tag -s` 需要 GPG key；这是另一条 tag，
  不能把已存在的 annotated tag 原地改成 signed。
- 本地 tag 不是发布证据；没有 push 就没有远端副本。

### 1.4 正式处置结论（2026-11-09）

- **保留本地 `v0.2.4` 不动**：不删除、不移动、不改指、不重签，也不推送。
  它是 0.2.4 的本地历史标注，不属于 0.4.x 发布线。
- 2026-11-09 复核：`git ls-remote --tags origin` 没有 `v0.2.4`；
  `1b64df0` 不在 `master` 历史中。`git push --follow-tags` 从 0.4.x
  分支不会带上它，但 `git push --tags` 会。
- 发布时只推明确指定的 tag：
  ```
  git push origin refs/tags/v0.4.1
  ```
  不要使用 `git push --tags`。
- 防误推工具：`scripts/pre-push-tag-guard.sh`。
  - 发布前手动检查：`scripts/pre-push-tag-guard.sh origin`。它列出
    `git push --tags` 会创建的 tag；`v0.2.4` 在列表内时退出 2。
  - 可选 pre-push hook（linked worktree 也适用）：
    ```
    hook=$(git rev-parse --git-path hooks/pre-push)
    mkdir -p "$(dirname "$hook")"
    cat > "$hook" <<'EOF'
    #!/bin/sh
    root=$(git rev-parse --show-toplevel) || exit 1
    exec "$root/scripts/pre-push-tag-guard.sh" --hook
    EOF
    chmod +x "$hook"
    ```
    hook 只在 `refs/tags/v0.2.4` 将被创建时拒绝推送，不改变任何 tag。
  - 远端查询失败时脚本退出 3；此时同样不要用 `--tags`。
- 如果将来需要 0.2.4 的修订，另打新 tag（如 `v0.2.4-<fix>`），不要复用
  `v0.2.4`。

## 2. 本机 D 态重启

本机是工作站（`linux 7.2.9-1-MANJARO`，不是 near）。2026-11-08 的检查
发现 tbtun 在 teardown 后留下 D 态任务和损坏的模块引用计数；重启是本机
唯一的干净退场路径。

### 2.1 现场证据

检查时的进程：
```
pid=1599330 ppid=1     stat=Ds wchan=0 comm=bash
  args=bash -c printf "del 1\n" > /proc/net/tbtun/config 2>/dev/null || true;
        rmmod tbtun 2>/dev/null || true; lsmod | grep tbtun || echo "tbtun not loaded";
        ip rule del to 45.11.1.62/32 lookup main pref 100 2>/dev/null || true; ...
pid=1599742 ppid=1599741 stat=D  wchan=0 comm=rmmod
  args=rmmod tbtun
pid=2308463 ppid=2     stat=D->I comm=kworker/u64:1-btrfs-endio
```
模块与 proc：
```
/sys/module/tbtun/refcnt          -> -1
/sys/module/tbtun/holders         -> 空
/proc/net/tbtun                   -> 不存在
lsmod                             -> tbtun 368640 -1
/proc/uptime                      -> 617292.96 s（约 7.1 天）
/proc/sys/kernel/hung_task_timeout_secs -> 120
```
`dmesg` 在本轮没有可读输出；`/proc/<pid>/stack` 返回 `EACCES`（非 root）。
两个 tbtun 相关任务从检查开始持续为 D；kworker 第一次快照为 D，第二次变成
I，属于 btrfs endio，不是 tbtun 主因。

这与仓库里的旧证据同型：
`review/wan-experiment/arm64/results/20261107T234902Z-firstconn/near-deadlock.txt`
记录 `tbt_session_destroy` 的 bash 和 `proc_entry_rundown` 的 rmmod 都为 D，
`refcnt=-1`。本机是同一类 teardown 死锁：proc entry 已消失、模块引用计数
损坏、rmmod 无法返回。

### 2.2 重启前核查（只读）

```
uname -r
ps -eo pid,ppid,stat,wchan:32,comm,args --sort=pid | awk '$3 ~ /^D/'
cat /sys/module/tbtun/refcnt
ls -A /sys/module/tbtun/holders
ls -ld /proc/net/tbtun
cat /proc/sys/kernel/hung_task_timeout_secs
dmesg -T | grep -E 'hung_task|tbtun|proc_entry_rundown' | tail -n 20
journalctl -k -b --no-pager | grep -E 'hung_task|tbtun|proc_entry_rundown' | tail -n 20
# root:
for p in <D pids>; do cat /proc/$p/stack; done
```
或直接：
```
scripts/local-dstate-check.sh --collect /var/tmp/tbtun-dstate-$(date -u +%Y%m%dT%H%M%SZ)
```
期望输出 `DSTATE_VERDICT REBOOT_REQUIRED`；退出码 2。

### 2.3 重启（不自动执行）

1. 停止所有新的 tbtun 测试。不要再次 `rmmod tbtun`，不要写
   `/proc/net/tbtun/config`，不要 `kill -9` D 态任务；这些只会增加新的
   D 态进程。
2. 尽可能保存现场：`scripts/local-dstate-check.sh --collect ...`；
   如果有 root，补 `/proc/<pid>/stack`、`dmesg -T`。
3. 同步：
   ```
   sync; sync; sync
   ```
4. 正常重启：
   ```
   systemctl reboot
   ```
   等待 120 s。若仍在本机，且你有串口/物理控制台，最后手段：
   ```
   reboot -f
   ```
   `reboot -f` 可能丢未落盘数据；只在正常重启无效时用。
5. SysRq 只在确认可用时用：
   ```
   cat /proc/sys/kernel/sysrq
   grep CONFIG_MAGIC_SYSRQ /boot/config-$(uname -r)
   ```
   两者都允许才：
   ```
   echo s > /proc/sysrq-trigger
   sleep 1
   echo b > /proc/sysrq-trigger
   ```
   仓库的 `.65` 设计明确写 `CONFIG_MAGIC_SYSRQ` 未编译；本机不要假设可用。
6. 重启后不要立即加载 tbtun；先完成 2.4 的全部核查。

### 2.4 重启后核查（只读）

**处置结论**：D 态任务不响应信号，`refcnt=-1` 没有安全的用户态卸载路径；
`systemctl reboot` 是唯一正常退场，`reboot -f` 只在正常重启无效且能接受
数据丢失时使用。重启前停止新测试，不要再次 `rmmod`、写
`/proc/net/tbtun/config` 或 `kill -9` D 态任务。

重启完成、能登录后先跑：
```
uname -r                                  # 期望 7.2.9-1-MANJARO
lsmod | grep '^tbtun' || echo tbtun-not-loaded
ls -ld /proc/net/tbtun 2>/dev/null || echo proc-gone
cat /sys/module/tbtun/refcnt 2>/dev/null || echo module-gone
ps -eo pid,ppid,stat,wchan:32,comm,args --sort=pid | awk '$3 ~ /^D/'
scripts/local-dstate-check.sh
dmesg -T | grep -E 'hung_task|tbtun|proc_entry_rundown' | tail -n 20
journalctl -k -b --no-pager | grep -E 'hung_task|tbtun|proc_entry_rundown' | tail -n 20
btrfs device stats / 2>/dev/null || true
```

判据：

- `uname -r` 为 `7.2.9-1-MANJARO`（或重启后的新内核）。
- `lsmod` 无 tbtun，`/proc/net/tbtun` 不存在，`refcnt` 读不到。
- D 态扫描为空，`local-dstate-check.sh` 退出 0、`DSTATE_VERDICT OK`。
- dmesg/journal 没有新的 `hung_task` 或 `proc_entry_rundown`。
- btrfs stats 无新增错误；有错误先查盘，不要在故障窗口跑 scrub。

以上全部满足，才重新构建并加载 0.4.x 模块。首次加载后先用一次 close
（`del 1` 加 `rmmod`）验证 teardown，再恢复测试。0.4.x 的 teardown 修复
已经落在 master（`aceca3a`、`5192dd0`）。

### 2.5 风险

- D 态任务不响应信号，`kill -9` 不能回收；等待不会自行恢复（已持续 7 天）。
- 第二次 `rmmod` 可能再挂一个进程；再次写 `/proc/net/tbtun/config` 也可能
  挂住新的 bash。
- 模块 refcnt `-1` 说明引用计数已经损坏；不存在“安全卸载”的用户态路径。
- 重启是唯一干净退场；`reboot -f` 有数据丢失风险。
- btrfs endio kworker 曾短暂 D，提示存储 I/O 也可能有问题；重启后先看
  `dmesg`/`btrfs device stats`，不要在故障窗口跑 scrub。
- 这是本机（工作站）的模块残留，不是 near；不要把它和 `.85` 上的喂狗者
  或 tbtun 运行状态混为一谈。
