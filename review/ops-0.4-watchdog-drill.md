# watchdog 回滚与喂狗退场演练（本地沙箱）
日期 2026-11-09；仓库 /home/kunweiz/Desktop/vibe/tcp-brutal；沙箱 /tmp/wd-drill-sim。
安全边界：.85 只做只读 SSH；未执行任何 systemctl 变更，未触碰其运行中的 feeder。本机真实
/dev/watchdog0（crw 246,0）从未被打开：沙箱里的 /dev/watchdog0 是 /dev/null 的 bind。

## 方法
- unshare -rmp --fork + 私有 mount ns + mock rootfs + mock systemctl（逐条记录调用）；
  假 /proc/cmdline、/proc/uptime、boot_id 与 /sys/class/watchdog。
- 喂狗用 FIFO 真跑 tbtun-netwatch.py（WATCHDOG_DEV 指向 FIFO），逐字节记录写入。
- 驱动：fifo-test.sh、ns-enter.sh、root/opt/sandbox/scenario-*.sh；证据在 run-fifo-*/ 与 root/evidence/。

## A. netwatch 喂狗行为（真实进程）
1) 首次 + 周期：FEED_S=1、grace=0、探测 127.0.0.1:18080（HTTP 200）。
   bytes.log：0.523/1.524/2.524/3.524/4.524/5.525 s，6 字节全为 0x31('1')；
   日志 "first feed ok (watchdog kept alive)"，此后每 2 s "feed ok"。
2) 探测失败达阈值：probe=1 s、threshold=3、URL=127.0.0.1:1。
   日志：failed 1/3、2/3、3/3 → "VERDICT ... stopping the watchdog feed, hardware reset in <=44 s"；
   pre-reset.log reason=reset。
   bytes.log：0.018/1.018/2.019 后不再写入；进程保持 fd 打开直到测试工具 8 s 后发 SIGTERM
   （feeder 的 give_up 分支只停写、不 close），即停止喂狗后 44 s 倒计时开始。
3) nowayout=1 下 SIGTERM：假 cmdline 含 dw_wdt.nowayout=1。
   bytes=4，全为 0x31，has_V=False；日志 "signal 15, exiting; feed stops, hardware resets in <=44 s"，
   随后 "feeder stopped"；device-info.log 记录 nowayout=1 (cmdline, unverified)。
   源码核对：唯一写入是 os.write(fd, b"1")，无 'V'、无 WDIOS_DISABLECARD。

## B. rollback-85.sh 与 drill（mock rootfs 实跑）
- --status：holders pid=14(python3)，active=active，RuntimeWatchdogUSec=0。
- --next-boot-only --dry-run：只打印 "+ systemctl disable ..."；disable 调用 0 次，unit 仍 enabled。
- --next-boot-only：disable 1 次；unit=disabled；feeder 存活；stop/start/restart 调用 0 次。
- drill 默认审计：feeder_holder PASS(pid=16)、restart_no PASS、RuntimeWatchdogUSec=0 PASS、
  nowayout_expected PASS(1)、magicclose PASS(options=0x8180)；DRILL_RESULT AUDIT_OK；
  审计阶段 disable 0 次、unit 仍 enabled、feeder 存活。
- --execute-next-boot --yes：DRILL_RESULT AUDIT_OK；disable 1 次；unit=disabled；feeder 存活；
  stop/start/restart 0 次 → “不立即 stop 活跃喂狗者”的防呆成立。
- nowayout-85.sh --prepare/--rollback：uEnv 从无 token → 追加 dw_wdt.nowayout=1 → 还原备份后
  token_after_rollback=0，且与备份逐字节相同。
- 模拟重启后：cmdline=absent、sysfs nowayout=0；drill --verify 输出 fresh_boot/unit_disabled/
  unit_inactive/no_holder 全 PASS，DRILL_RESULT PASS。

## C. .85 只读快照（演练期间）
CMDLINE 含 dw_wdt.nowayout=1；IS_ACTIVE=active；IS_ENABLED=enabled（重启仍会自动 arm）；MAINPID=3926；
RUNTIME_WDOG=0；OPTIONS/TIMEOUT=absent（内置驱动未导出该 sysfs，沙箱按 0x8180 建模）；
UPTIME=75655.85；BOOTID=c7eff3d2-09ca-4e72-b263-0150dfbeb79d。
只执行了 cat 与 systemctl is-active/is-enabled/show；无 stop/start/restart/reboot。

## 结论
- 首次喂狗、周期喂狗、阈值停喂、SIGTERM 不写 'V' 均在真实进程与真实字节流中观察到。
- rollback 的 next-boot-only 只 disable、不 stop；drill 的审计与执行路径同样不触碰活跃 feeder，
  避免提前触发 44 s 硬重启。
- nowayout cmdline 的移除由 nowayout-85.sh --rollback 完成（rollback-85.sh 本身不改 cmdline），
  模拟重启后的验证全部 PASS。

## 复现
  sh /tmp/wd-drill-sim/fifo-test.sh success|fail|term
  unshare -rmp --fork --propagation private sh /tmp/wd-drill-sim/ns-enter.sh /opt/sandbox/scenario-rollback.sh
  unshare -rmp --fork --propagation private sh /tmp/wd-drill-sim/ns-enter.sh /opt/sandbox/scenario-drill.sh
  CMDLINE_SRC=/tmp/wd-drill-sim/proc/cmdline.nowayout0 BOOTID_SRC=/tmp/wd-drill-sim/proc/boot_id.after \
    unshare -rmp --fork --propagation private sh /tmp/wd-drill-sim/ns-enter.sh /opt/sandbox/scenario-verify.sh
备注：watchdog-rollback-drill.sh 只有 audit/verify/execute-* 模式，没有 --sandbox 选项；
沙箱由上述 namespace + mock rootfs 提供，脚本本身未改动。
