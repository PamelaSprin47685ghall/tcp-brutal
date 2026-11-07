#!/usr/bin/env bash
# controlled-c8-once.sh - .65 near x HND0 far, c8 配置的单次受控实验。
#
# 目的
#   在 2026-11-06 22:44Z 的 c8 事故配置下，只触发一次 1 MiB burst。无论
#   结果如何都不重试。实验前确认崩溃可见性（panic_on_oops、ramoops、
#   tbtun-kmsg-relay、tbtun-sampler、.80 接收端）；实验后即使 .65 失联，
#   也把 netconsole、pstore、journal、sampler、dmesg、stats 和时间线收进
#   证据目录。
#
#   注意：tbtun-kmsg-relay 和 tbtun-sampler 都是用户态进程，panic 之后
#   不能保证继续运行。panic 的权威证据是 ramoops/pstore；kmsg-relay 的
#   netconsole 日志是尽力而为，sampler 记录的是崩溃前的最后状态。
#
# 单次约束
#   本脚本只在内联的 burst 段调用一次 tbt_burst，且用 BURST_ATTEMPTED
#   做硬闸门：任何路径都不可能第二次触发。join 循环只重读 stats，不重跑
#   burst。burst 失败、超时、near 失联都不重试。
#
# 用法
#   EXPECTED_FAR_SRCVERSION=<far 修复版 srcversion> \
#       review/wan-experiment/arm64/controlled-c8-once.sh
#
#   脚本在工作站运行，通过 ssh 驱动 .65（near）和 45.11.1.62（far）。
#   运行前由操作者完成（脚本只检查，不代做）：
#     - .65: sysctl -w kernel.panic_on_oops=1 kernel.panic=10
#     - .65: /sys/fs/pstore 为空，ramoops 已注册
#     - .65: tbtun-kmsg-relay.service、tbtun-sampler.service 为 active
#     - .80: tbtun-netconsole-rx.service（user unit）为 active，UDP 6666
#            有文件输出
#     - far: 已用 deploy_hnd0.sh 部署**修复版** tbtun（far 与 near 同代码，
#            parser->have 未初始化 bug 同样存在），9000/5202 在监听；
#            操作者设置 EXPECTED_FAR_SRCVERSION=<far 修复版 srcversion>
#     - near: tbtun 未加载，9001 无监听
#     - 修复后的模块在 .65:/root/tbtun-fixed-20261106T231212Z/tbtun.ko
#
# 环境变量
#   NEAR_HOST            默认 root@192.168.123.65
#   FAR_HOST             默认 root@45.11.1.62
#   TBT_SRC_DIR          .65 上的 staged tree，默认
#                        /root/tbtun-fixed-20261106T231212Z
#   EXPECTED_NEAR_SRCVERSION
#                        期望的 near srcversion，默认 72168C1CE3E3378A758DAFF
#   EXPECTED_NEAR_SHA256 期望的 near ko sha256，默认
#                        1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92
#   EXPECTED_NEAR_VERMAGIC
#                        期望的 near vermagic，默认
#                        "6.1.172 SMP mod_unload modversions aarch64"
#   EXPECTED_FAR_SRCVERSION
#                        far 修复后构建的 srcversion，默认
#                        332759AF31AB0DD875E0EE7（2026-11-06T23:28Z 单次实验
#                        已验证的 far loaded 值）。far 是 x86_64，srcversion
#                        与 near 不同；可用环境变量覆盖。
#   FAR_STAGE            far 上的 staged tree，默认 /home/tbtun-t52-arm64
#   EVIDENCE_DIR         证据根目录，默认 <repo>/review/wan-experiment/arm64
#   JOIN_TIMEOUT         join gate 超时，默认 60 秒
#   BURST_TIMEOUT        burst 外层超时，默认 120 秒
#   NEAR_RETURN_WAIT     near 失联后等待恢复的秒数，默认 240
#   LOCK_WAIT            两端 flock 等待秒数，默认 120
#   LOCK_HOLD_SECS       lock holder 的安全超时，默认 1200 秒
#   LOAD_LIMIT          两端 load1 上限，默认 4
#   ALLOW_PRE_CRASH_MODULE=1
#                        允许使用 mtime 早于已知崩溃时刻的 tbtun.ko（危险）
#   ALLOW_RX_PROCESS_FALLBACK=1
#                        允许 systemd user manager 不可见时用 UDP 进程检查
#                        代替 tbtun-netconsole-rx active（默认不允许）
#   ALLOW_BAD_FAR_STAGE=1
#                        允许 far staged tbtun.ko 是已知事故构建（默认拒绝；
#                        当前 /home/tbtun-t52-arm64/tbtun.ko 就是 846D47A6，
#                        下一次 far redeploy 会回退到它）
#
# 证据目录
#   review/wan-experiment/arm64/controlled-c8-<UTC>/
#     SUMMARY.md              中文报告：判定、步骤、预期/失败分支、实际
#     timeline.txt            带 UTC 时间戳的阶段时间线
#     run.log                 脚本自身的运行日志
#     netconsole.log          本地接收端完整日志快照
#     netconsole-window.log   从日志行号基线之后开始的窗口
#     netconsole-crash-lines.txt
#     near-journal.log        崩溃那一 boot 的 journal（重启后取 -b -1）
#     near-journal-b0.log     当前 boot
#     near-journal-b1.log     上一 boot
#     near-dmesg.log
#     near-pstore.txt         /sys/fs/pstore 与 /var/lib/systemd/pstore
#     near-stats.txt          /proc/net/tbtun/{config,ports,tunnels,peers,stats}
#     near-tbtun-last.log     .65 sampler 的 250ms 状态记录
#     near-tbtun-last-old.log sampler 轮转后的上一份（若存在）
#     near-ss.txt             ss -tan / ss -s
#     near-meta.txt           uptime、boot_id、panic sysctl、cmdline、boots
#     far-dmesg.log
#     far-journal.log
#     far-stats.txt
#     far-ss.txt
#     stats.txt               两端关键计数器
#     pre-near.txt / pre-far.txt / joined-*.txt / post-*.txt
#     burst.txt               唯一一次 burst 的原始输出
#     burst-build.log
#     module.txt              两端模块身份
#     near-probes.txt         burst 后的 ping/ARP/ssh 探测
#     cleanup.txt             清理结果
#
# 退出码
#   0   全流程完成，near 存活，burst rc=0
#   2   用法错误
#   3   前置检查失败（ssh、panic、ramoops、可见性服务、far 未部署、near 已加载、
#       模块身份、模块过旧）
#   4   锁忙或负载过高
#   5   join gate 失败（未触发 burst）
#   6   near 失联或重启（证据已收集）
#   7   burst 返回非 0，但 near 存活
#   8   关键证据缺失
#   9   清理不完整
#  10   设置失败（far/near 配置、insmod、ports、config、burst 构建）
#
# 步骤、预期结果与失败分支（SUMMARY.md 会带实际值重写同一张表）
#   1 前置检查   预期：两端 ssh、near panic_on_oops=1/panic=10、ramoops 已注册、
#                /sys/fs/pstore 为空、.80 UDP 6666 接收端有文件输出、far 已加载
#                且 srcversion 为修复版、far staged .ko 不是已知事故构建、
#                9000/5202 在听、near 未加载、模块身份可记录。
#                失败：退出 3，不加载模块、不写配置；far 不匹配或 stage 是旧
#                构建时停止，不跑 near。
#   2 锁与负载   预期：两端 flock 成功，load1 <= 4。
#                失败：退出 4，不跑。
#   3 可见性检查  预期：.65 kmsg-relay/sampler active，.80 接收端 active，
#                kmsg 探针出现在 .80 的接收文件。
#                失败：退出 3，不跑；否则崩溃时没有日志。
#   4 far 配置   预期：ports 31000-31015 与 far config 写入并逐字读回，
#                9000/5202 重新监听。
#                失败：退出 10，near 不加载，far 清理。
#   5 near 配置  预期：ip rule、rmem/wmem、insmod、srcversion 一致、
#                ports 30000-30015、near config 逐字读回。
#                失败：退出 10，清理。
#   6 join gate  预期：60 秒内两端 up=8；parse/mismatch/FEC 计数器记录但不阻塞。
#                失败：up 超时或快照 up 不一致：退出 5，不触发 burst，收集证据。
#   7 触发       预期：恰好一次 1 MiB interleaved burst，120 秒内返回。
#                失败：记录 rc，不重试；near 失联则转证据收集。
#   8 失联判定   预期：记录 ping/ARP/ssh、失联时刻、恢复时刻、是否重启。
#                失败：记 unknown，继续 far 与 netconsole 证据。
#   9 证据收集   预期：netconsole、pstore、journal -b -1 --since、sampler、
#                dmesg、stats、far dmesg/ss 全部落盘。
#                失败：单项记录原因，不阻塞其他项；无更优先判定时退出 8。
#  10 清理       预期：near rmmod/rule/sysctl、far rmmod/echo、锁释放。
#                失败：失联跳过并记录；清理失败退出 9。
#
# 注意
#   - 本脚本不重试、不降级、不换工具。
#   - 已知崩溃时刻 2026-11-06T22:44:09Z 之前构建的 tbtun.ko 默认拒绝加载。
#   - .65 失联时所有 ssh 调用都带 timeout，清理会跳过并记录。
#   - far staged /home/tbtun-t52-arm64/tbtun.ko 当前是已知事故构建
#     846D47A623A8198946CBA64；脚本默认拒绝（ALLOW_BAD_FAR_STAGE=1 可放行），
#     因为下一次 far redeploy 会回退。2026-11-06T23:28Z 实测的 loaded 修复版
#     是 332759AF31AB0DD875E0EE7。
#
# 触发选择
#   选择 `tbt_burst 127.0.0.1 9001 1 5 interleaved`，不用 tbt_ping 37B：
#   - 1 MiB / 5 s 与事故的 c8 触发逐字一致，双向字节校验会持续走
#     HTTP RX 解析路径；tbt_ping 37B 只验证 join，覆盖不足。
#   - 本次只允许再挂一次，必须用最接近事故的负载，而不是最小验证。

set -euo pipefail

PROG=${0##*/}
[[ $# -eq 0 ]] || { echo "usage: $PROG" >&2; exit 2; }
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_IP=192.168.123.65
LOCAL_IP=192.168.123.80
NC_PORT=6666
NEAR_LOCK=/tmp/tbtun-rk3588.lock
FAR_LOCK=/tmp/tbtun-hnd0.lock
LOCK_WAIT=${LOCK_WAIT:-120}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-1200}
LOAD_LIMIT=${LOAD_LIMIT:-4}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-60}
BURST_TIMEOUT=${BURST_TIMEOUT:-120}
NEAR_RETURN_WAIT=${NEAR_RETURN_WAIT:-240}
TBT_SRC_DIR=${TBT_SRC_DIR:-/root/tbtun-fixed-20261106T231212Z}
FAR_STAGE=${FAR_STAGE:-/home/tbtun-t52-arm64}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}
ALLOW_PRE_CRASH_MODULE=${ALLOW_PRE_CRASH_MODULE:-0}
EXPECTED_NEAR_SRCVERSION=${EXPECTED_NEAR_SRCVERSION:-72168C1CE3E3378A758DAFF}
EXPECTED_NEAR_SHA256=${EXPECTED_NEAR_SHA256:-1637ca0fd3d6371595102f34e187242ef44c0d73d717a7d77e039c793ba2ab92}
EXPECTED_NEAR_VERMAGIC=${EXPECTED_NEAR_VERMAGIC:-"6.1.172 SMP mod_unload modversions aarch64"}
EXPECTED_FAR_SRCVERSION=${EXPECTED_FAR_SRCVERSION:-332759AF31AB0DD875E0EE7}
FAR_KNOWN_BAD_SRCVERSIONS=${FAR_KNOWN_BAD_SRCVERSIONS:-846D47A623A8198946CBA64}
ALLOW_BAD_FAR_STAGE=${ALLOW_BAD_FAR_STAGE:-0}

NEAR_PORT=9001
FAR_PORT=9000
ECHO_PORT=5202
NEAR_PORTBLOCK=30000-30015
FAR_PORTBLOCK=31000-31015
CARRIERS=8
INSMOD_PARAMS='max_carriers=256 max_tunnels=1 debug=0'
SYSCTL_MAX=67108864
CRASH_UTC=2026-11-06T22:44:09Z

# 与事故逐字一致；far 只替换 role/listen/target/portblock。
NEAR_CFG='tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=8 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30015 nonce=00112233445566778899aabbccddeeff'
FAR_CFG='tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped w=8 carriers=8 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31015 nonce=00112233445566778899aabbccddeeff'

SSH_BASE=(-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 -o TCPKeepAlive=yes)

EX_OK=0
EX_USAGE=2
EX_PRECOND=3
EX_BUSY=4
EX_JOIN=5
EX_LOST=6
EX_RUN=7
EX_EVIDENCE=8
EX_CLEAN=9
EX_SETUP=10

# 状态位。cleanup 和 SUMMARY 依赖这些值，所以先给默认值。
NEAR_MODULE_LOADED=0
NEAR_RULE_ADDED=0
NEAR_SYSCTL_SAVED=0
OLD_RMEM=""
OLD_WMEM=""
FAR_TOUCHED=0
KMSG_RELAY_STATE="?"
SAMPLER_STATE="?"
RX_SERVICE_STATE="?"
NC_PROBE_OK=0
NC_LOG=""
NC_PID=""
NC_START_SIZE=0
NC_END_SIZE=0
NC_START_LINES=0
NC_END_LINES=0
SAMPLER_BASELINE_LINES=0
SAMPLER_END_LINES=0
START_UTC_J=""
NEAR_LOST=0
NEAR_LOST_AT=""
NEAR_RETURNED_AT=""
NEAR_REBOOTED=0
NEAR_BOOT_ID_BEFORE=""
NEAR_BOOT_ID_AFTER=""
JOIN_OK=0
GATE_WARN=0
NUP_LAST=""
FUP_LAST=""
BURST_ATTEMPTED=0
BURST_SSH_RC=""
BURST_RC=""
WALL_MS=""
BURST_END_UTC=""
PRECHECK_DONE=0
FAR_CFG_OK=0
NEAR_CFG_OK=0
EVIDENCE_INCOMPLETE=0
CLEAN_FAIL=0
NEAR_KO_SHA=""
NEAR_KO_SRCVERSION=""
NEAR_KO_VERMAGIC=""
NEAR_LOADED_SRCVERSION=""
NEAR_TREE_COMMIT=""
FAR_KO_SHA=""
FAR_KO_SRCVERSION=""
FAR_LOADED_SRCVERSION=""
FAR_KO_VERMAGIC=""
FAR_KO_MTIME=0
FAR_IDENTITY_MATCH="?"
NEAR_PANIC_ON_OOPS="?"
NEAR_PANIC="?"
NEAR_EXTRA_SYSCTLS="?"
NEAR_PSTORE_EMPTY="?"
NEAR_RAMOOPS="?"
NEAR_IDENTITY_MATCH="?"
NEAR_CLEAN_STATE="not-checked"
FAR_MODULE_STATE="not-checked"
FAR_LISTEN_STATE="not-checked"
LOCKS_WERE_HELD="no"
PRE_NEAR_DMESG=0
PRE_FAR_DMESG=0
POST_NEAR_DMESG=0
POST_FAR_DMESG=0
FINAL_RC=$EX_OK

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
[[ -f $REPO/host/tbt_burst.c ]] || { echo "$PROG: 不是仓库 checkout: $REPO" >&2; exit "$EX_USAGE"; }
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
EV=$EVIDENCE_ROOT/controlled-c8-$STAMP
if ! mkdir "$EV" 2>/dev/null; then
    EV=$EVIDENCE_ROOT/controlled-c8-$STAMP-$$
    mkdir -p "$EV"
fi
START_UTC_J=$(date -u +"%Y-%m-%d %H:%M:%S UTC")

note() {
    printf '%s %s\n' "$(date -u +%H:%M:%SZ)" "$*" | tee -a "$EV/run.log" >&2
}

die() {
    local rc=$1
    shift
    note "FATAL: $*"
    exit "$rc"
}

mark() {
    local phase=$1
    shift
    printf '%s\t%s\t%s%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        "$(date +%s)" \
        "$phase" \
        "${*:+ | $*}" | tee -a "$EV/timeline.txt" >&2
}

# ---------------------------------------------------------------------------
# ssh / scp

rrun_near() {
    timeout "${SSH_TMO:-30}" ssh "${SSH_BASE[@]}" "$NEAR_HOST" "$@"
}

rrun_far() {
    timeout "${SSH_TMO:-30}" ssh "${SSH_BASE[@]}" "$FAR_HOST" "$@"
}

rscp_near() {
    timeout "${SSH_TMO:-30}" scp -q "${SSH_BASE[@]}" "$@"
}

module_loaded_on() {
    timeout 12 ssh "${SSH_BASE[@]}" "$1" 'lsmod | grep -q "^tbtun "' >/dev/null 2>&1
}

listening_on() {
    [[ -n $(timeout 12 ssh "${SSH_BASE[@]}" "$1" "ss -lntH 'sport = :$2'" 2>/dev/null) ]]
}

near_ssh_ok() {
    timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" true >/dev/null 2>&1
}

near_alive() {
    ping -c 3 -W 1 "$NEAR_IP" >/dev/null 2>&1 || return 1
    near_ssh_ok || return 1
    return 0
}

near_boot_id() {
    timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null
}

load1_on() {
    timeout 12 ssh "${SSH_BASE[@]}" "$1" "cut -d' ' -f1 /proc/loadavg" 2>/dev/null
}

# ---------------------------------------------------------------------------
# stats 解析：每个查找一次 awk，不拼 grep|head，避免 SIGPIPE 影响 pipefail。

sec() {
    awk -v s="===$2===" '$0 == s { f = 1; next } f && /^===/ { exit } f' "$1"
}

sec_number() {
    awk -v s="===$2===" '
        $0 == s { f = 1; next }
        f && /^===/ { exit }
        f && !done {
            v = $0; gsub(/[^0-9]/, "", v)
            if (v != "") { print v; done = 1 }
        }' "$1"
}

stat_field_text() {
    printf '%s\n' "$1" | awk -v k="tunnel 1 $2 " -v f="$3=" '
        index($0, k) == 1 {
            for (i = 1; i <= NF; i++) {
                if (index($i, f) == 1) {
                    v = substr($i, length(f) + 1)
                    if (v ~ /^[0-9]+$/) { print v; exit }
                }
            }
        }'
}

field() {
    stat_field_text "$(sec "$1" stats)" "$2" "$3"
}

live_field() {
    local host=$1 key=$2 fld=$3 text=""
    text=$(timeout 12 ssh "${SSH_BASE[@]}" "$host" 'cat /proc/net/tbtun/stats' 2>/dev/null) || text=""
    stat_field_text "$text" "$key" "$fld"
}

# 逐 token 检查 config，给出缺失项；精确比较仍由调用方负责。
require_tokens() {
    local label=$1 cfg=$2 tok
    shift 2
    for tok in "$@"; do
        case " $cfg " in
            *" $tok "*) ;;
            *)
                note "config: $label 缺少 token: $tok"
                return 1
                ;;
        esac
    done
    return 0
}

# ---------------------------------------------------------------------------
# 快照：两端通用，输出按 ===section=== 分段。

snapshot() {
    local host=$1 out=$2
    timeout 60 ssh "${SSH_BASE[@]}" "$host" 'bash -s' >"$out" 2>&1 <<'EOS'
set -u
say() { printf '\n===%s===\n' "$1"; }
run() { say "$1"; shift; "$@" 2>&1; printf '[rc=%d]\n' "$?"; }
run uname uname -a
run date date -u +%Y-%m-%dT%H:%M:%SZ
run uptime uptime
run boot_id cat /proc/sys/kernel/random/boot_id
run loadavg cat /proc/loadavg
run nproc nproc
run lsmod sh -c 'lsmod | grep -E "^tbtun" || true'
run proc_ls sh -c 'ls -l /proc/net/tbtun 2>&1 || true'
run config sh -c 'cat /proc/net/tbtun/config 2>&1 || true'
run ports sh -c 'cat /proc/net/tbtun/ports 2>&1 || true'
run tunnels sh -c 'cat /proc/net/tbtun/tunnels 2>&1 || true'
run peers sh -c 'cat /proc/net/tbtun/peers 2>&1; printf "[lines] "; wc -l < /proc/net/tbtun/peers 2>/dev/null || echo 0'
run stats sh -c 'cat /proc/net/tbtun/stats 2>&1 || true'
run dmesg sh -c 'dmesg 2>&1 | tail -n 2000'
run dmesg_count sh -c 'dmesg 2>/dev/null | wc -l'
run procstat cat /proc/stat
run freem free -m
run netdev cat /proc/net/dev
run ss_listen sh -c 'ss -ltnp 2>&1 || true'
run ss_summary sh -c 'ss -s 2>&1 || true'
run ss_tan sh -c 'ss -tan 2>&1 | head -n 400 || true'
run iprule sh -c 'ip rule show 2>&1 || true'
run iproute sh -c 'ip route get 45.11.1.62 2>&1 || true'
run ipaddr sh -c 'ip -4 addr show 2>&1 || true'
run sysctl sh -c 'sysctl net.core.rmem_max net.core.wmem_max kernel.panic_on_oops kernel.panic 2>&1 || true'
run pstore sh -c 'ls -la /sys/fs/pstore 2>&1 || true'
EOS
}

# ---------------------------------------------------------------------------
# 远端 flock。持有者是后台 ssh + sleep；脚本退出时先从锁文件读出远端
# holder pid 并 kill，再 kill 本地 ssh。只杀本地 ssh 时远端 bash 可能继续
# 持有 flock(9)，这是 2026-11-06 单次实验前遇到的锁泄漏。

NEAR_LOCK_PID=""
NEAR_LOCK_TMP=""
NEAR_LOCK_HOST=""
NEAR_LOCK_PATH=""
NEAR_LOCK_PURPOSE=""
FAR_LOCK_PID=""
FAR_LOCK_TMP=""
FAR_LOCK_HOST=""
FAR_LOCK_PATH=""
FAR_LOCK_PURPOSE=""

lock_acquire() {
    local host=$1 lock=$2 purpose=$3 prefix=$4 reply tmp pid
    case $purpose in *"'"*) die "$EX_USAGE" "lock purpose 不能含单引号" ;; esac
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/tbtun-lock.XXXXXX")
    mkfifo "$tmp/fifo"
    timeout $((LOCK_HOLD_SECS + 60)) ssh "${SSH_BASE[@]}" "$host" \
        "exec 9>>'$lock' || { echo 'cannot open lock file' >&2; exit 3; }
         flock -w $LOCK_WAIT 9 || { echo 'lock busy (another holder)' >&2; exit 4; }
         printf '%s %s %s %s\n' \$\$ \"\$(date -Is)\" \"\$(id -un)\" '$purpose' > '$lock'
         echo LOCKED
         sleep $LOCK_HOLD_SECS 9>&-" >"$tmp/fifo" 2>"$tmp/err" &
    pid=$!
    if ! read -r reply <"$tmp/fifo"; then
        note "lock: 无法取得 $lock on $host: $(tr '\n' ' ' <"$tmp/err" | cut -c1-300)"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -rf "$tmp"
        return 1
    fi
    if [[ $reply != LOCKED ]]; then
        note "lock: $host 返回异常: $reply"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -rf "$tmp"
        return 1
    fi
    printf -v "${prefix}_LOCK_PID" '%s' "$pid"
    printf -v "${prefix}_LOCK_TMP" '%s' "$tmp"
    printf -v "${prefix}_LOCK_HOST" '%s' "$host"
    printf -v "${prefix}_LOCK_PATH" '%s' "$lock"
    printf -v "${prefix}_LOCK_PURPOSE" '%s' "$purpose"
    note "lock: $lock 已持有"
}

lock_release() {
    local prefix=$1 pid tmp host lock purpose line rpid
    eval "pid=\${${prefix}_LOCK_PID:-}"
    eval "tmp=\${${prefix}_LOCK_TMP:-}"
    eval "host=\${${prefix}_LOCK_HOST:-}"
    eval "lock=\${${prefix}_LOCK_PATH:-}"
    eval "purpose=\${${prefix}_LOCK_PURPOSE:-}"
    # 先杀远端 holder（锁文件第一行第一列），否则远端 bash 会继续持有
    # flock(9) 直到 LOCK_HOLD_SECS。只有锁文件仍是本脚本的 purpose 才 kill，
    # 避免误杀在我们 holder 已死后抢到锁的另一个 agent。
    if [[ -n $host && -n $lock && -n $purpose ]]; then
        line=$(timeout 10 ssh "${SSH_BASE[@]}" "$host" \
            "head -n 1 '$lock' 2>/dev/null" 2>/dev/null) || line=""
        if [[ -n $line && $line == *"$purpose"* ]]; then
            rpid=$(awk '{print $1}' <<<"$line")
            if [[ $rpid =~ ^[0-9]+$ ]]; then
                timeout 10 ssh "${SSH_BASE[@]}" "$host" \
                    "kill $rpid 2>/dev/null || true" >/dev/null 2>&1 \
                    || note "lock: 远端 holder $rpid on $host 未能 kill"
            fi
        fi
    fi
    if [[ -n $pid ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    [[ -n $tmp ]] && rm -rf "$tmp"
    printf -v "${prefix}_LOCK_PID" '%s' ""
    printf -v "${prefix}_LOCK_TMP" '%s' ""
    printf -v "${prefix}_LOCK_HOST" '%s' ""
    printf -v "${prefix}_LOCK_PATH" '%s' ""
    printf -v "${prefix}_LOCK_PURPOSE" '%s' ""
}

# ---------------------------------------------------------------------------
# netconsole 接收端（工作站）与发送端（.65）。

nc_find_pid() {
    local line pid
    line=$(ss -ulnp 2>/dev/null | grep -E "(:$NC_PORT[[:space:]])" | head -n 1) || true
    [[ -n $line ]] || return 1
    pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    [[ -n $pid ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    printf '%s\n' "$pid"
}

nc_find_log() {
    local pid=$1 fd target best="" size best_size=0
    : >"$EV/netconsole-fds.txt"
    for fd in /proc/"$pid"/fd/*; do
        target=$(readlink "$fd" 2>/dev/null) || continue
        printf '%s -> %s\n' "$fd" "$target" >>"$EV/netconsole-fds.txt"
        case $target in
            /*)
                [[ -f $target ]] || continue
                size=$(stat -c %s "$target" 2>/dev/null) || size=0
                if [[ -z $best ]] || (( size > best_size )); then
                    best=$target
                    best_size=$size
                fi
                ;;
        esac
    done
    [[ -n $best ]] || return 1
    printf '%s\n' "$best"
}

nc_snapshot() {
    [[ -n $NC_LOG && -f $NC_LOG ]] || { EVIDENCE_INCOMPLETE=1; return 1; }
    cp -f "$NC_LOG" "$EV/netconsole.log" 2>/dev/null || { EVIDENCE_INCOMPLETE=1; return 1; }
    NC_END_SIZE=$(stat -c %s "$NC_LOG" 2>/dev/null) || NC_END_SIZE=0
    NC_END_LINES=$(wc -l <"$NC_LOG" 2>/dev/null) || NC_END_LINES=0
    if (( NC_END_LINES >= NC_START_LINES )); then
        tail -n +$((NC_START_LINES + 1)) "$NC_LOG" >"$EV/netconsole-window.log" 2>/dev/null || true
    else
        # 接收文件被截断/轮转：行号基线失效，退化为整份拷贝并记录。
        cp -f "$NC_LOG" "$EV/netconsole-window.log" 2>/dev/null || true
        note "netconsole: 行数变小（$NC_START_LINES -> $NC_END_LINES），窗口退化为整份"
    fi
    grep -aE 'Internal error: Oops|Kernel panic|Call trace|tbtun|tbt_http|tbt_rx_parse' \
        "$EV/netconsole-window.log" >"$EV/netconsole-crash-lines.txt" 2>/dev/null || true
    return 0
}

nc_probe() {
    local marker="tbt-c8-once-probe-$(date +%s)-$RANDOM" i
    [[ -n $NC_LOG && -f $NC_LOG ]] || return 1
    timeout 10 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "printf '%s\n' '$marker' > /dev/kmsg" >/dev/null 2>&1 || return 1
    for i in $(seq 1 30); do
        if grep -aqF "$marker" "$NC_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

visibility_precheck() {
    local xdg
    KMSG_RELAY_STATE=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'systemctl is-active tbtun-kmsg-relay.service' 2>/dev/null) || KMSG_RELAY_STATE="unknown"
    SAMPLER_STATE=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'systemctl is-active tbtun-sampler.service' 2>/dev/null) || SAMPLER_STATE="unknown"
    [[ $KMSG_RELAY_STATE == active ]] || { note "visibility: tbtun-kmsg-relay=$KMSG_RELAY_STATE"; return 1; }
    [[ $SAMPLER_STATE == active ]] || { note "visibility: tbtun-sampler=$SAMPLER_STATE"; return 1; }
    SAMPLER_BASELINE_LINES=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'wc -l < /var/log/tbtun-last.log 2>/dev/null || echo 0' 2>/dev/null) || SAMPLER_BASELINE_LINES=0
    [[ $SAMPLER_BASELINE_LINES =~ ^[0-9]+$ ]] || SAMPLER_BASELINE_LINES=0

    xdg=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
    if XDG_RUNTIME_DIR=$xdg systemctl --user is-active --quiet tbtun-netconsole-rx.service; then
        RX_SERVICE_STATE="active-user"
    elif systemctl is-active --quiet tbtun-netconsole-rx.service; then
        RX_SERVICE_STATE="active-system"
    elif [[ ${ALLOW_RX_PROCESS_FALLBACK:-0} == 1 ]] && nc_find_pid >/dev/null 2>&1; then
        RX_SERVICE_STATE="udp-process-only"
        note "visibility: user/system manager 看不到 tbtun-netconsole-rx；ALLOW_RX_PROCESS_FALLBACK=1，UDP $NC_PORT 进程存在，继续并记录"
    else
        RX_SERVICE_STATE="inactive"
        note "visibility: tbtun-netconsole-rx.service 不是 active，UDP $NC_PORT 也没有接收端"
        return 1
    fi
    note "visibility: relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE sampler_lines=$SAMPLER_BASELINE_LINES"
    return 0
}

# far 修复版身份闸门：far 与 near 同代码，旧 far 同样有 parser->have 未初始化
# bug。srcversion 不匹配时必须在任何 near 写入之前停止。
far_identity_gate() {
    if ! module_loaded_on "$FAR_HOST"; then
        note "far identity: tbtun 未加载；先部署修复版"
        return 1
    fi
    FAR_LOADED_SRCVERSION=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'cat /sys/module/tbtun/srcversion' 2>/dev/null) || FAR_LOADED_SRCVERSION=""
    [[ -n $FAR_LOADED_SRCVERSION ]] || {
        note "far identity: 读不到 /sys/module/tbtun/srcversion"
        return 1
    }
    case " $FAR_KNOWN_BAD_SRCVERSIONS " in
        *" $FAR_LOADED_SRCVERSION "*)
            note "far identity: 加载的是已知事故构建 srcversion=$FAR_LOADED_SRCVERSION；必须部署修复版"
            return 1
            ;;
    esac
    [[ -n $EXPECTED_FAR_SRCVERSION ]] || {
        note "far identity: 未设置 EXPECTED_FAR_SRCVERSION；far 修复版 srcversion 尚未提供"
        return 1
    }
    [[ $FAR_LOADED_SRCVERSION == "$EXPECTED_FAR_SRCVERSION" ]] || {
        note "far identity: 已加载 srcversion=$FAR_LOADED_SRCVERSION != expected=$EXPECTED_FAR_SRCVERSION"
        return 1
    }
    FAR_IDENTITY_MATCH="yes"
    return 0
}

# ---------------------------------------------------------------------------
# far 的 echo 目标。

far_restart_echo() {
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        "systemd-run --no-block --unit=tbt-echo-$ECHO_PORT --collect \
         --property=StandardOutput=file:$FAR_STAGE/tbt-echo-$ECHO_PORT.log \
         --property=StandardError=file:$FAR_STAGE/tbt-echo-$ECHO_PORT.log \
         '$FAR_STAGE/tbt_echo' $ECHO_PORT echo" >/dev/null 2>&1 || return 1
    local i
    for i in $(seq 1 20); do
        if listening_on "$FAR_HOST" "$ECHO_PORT"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# ---------------------------------------------------------------------------
# 证据收集。

collect_far_evidence() {
    note "evidence: far"
    snapshot "$FAR_HOST" "$EV/post-far.txt" || note "evidence: post-far 快照失败"
    timeout 45 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'bash -s' >"$EV/far-dmesg.log" 2>&1 <<'EOS' || EVIDENCE_INCOMPLETE=1
dmesg | tail -n 5000
echo ===
dmesg | grep -E 'tbtun|carrier|HELLO|Oops|panic' | tail -n 800
EOS
    timeout 30 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'for f in config ports tunnels peers stats; do echo "=== $f ==="; cat /proc/net/tbtun/$f 2>&1; done' \
        >"$EV/far-stats.txt" 2>&1 || EVIDENCE_INCOMPLETE=1
    timeout 30 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'echo "=== ss -tan ==="; ss -tan 2>&1; echo "=== ss -s ==="; ss -s 2>&1; echo "=== ss :9000 ==="; ss -tan "( sport = :9000 or dport = :9000 )" 2>&1' \
        >"$EV/far-ss.txt" 2>&1 || EVIDENCE_INCOMPLETE=1
    timeout 45 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'journalctl -k -n 5000 --no-pager -o short-iso 2>&1' >"$EV/far-journal.log" 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'echo "=== meta ==="; date -u; uptime; cat /proc/sys/kernel/random/boot_id; lsmod | grep -E "^tbtun" || true' \
        >"$EV/far-meta.txt" 2>&1 || true
}

near_probe() {
    local ts neigh
    PROBE_PING_RC=0
    PROBE_SSH_RC=0
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    ping -c 3 -W 1 "$NEAR_IP" >/dev/null 2>&1 || PROBE_PING_RC=$?
    neigh=$(ip neigh show "$NEAR_IP" 2>/dev/null | tr '\n' ';') || neigh=""
    timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" true >/dev/null 2>&1 || PROBE_SSH_RC=$?
    printf '%s ping_rc=%s ssh_rc=%s neigh=%s\n' \
        "$ts" "$PROBE_PING_RC" "$PROBE_SSH_RC" "${neigh:-none}" >>"$EV/near-probes.txt"
}

collect_near_evidence() {
    note "evidence: near"
    if ! near_ssh_ok; then
        printf 'near 不可达，跳过 ssh 证据；见 near-probes.txt、netconsole.log\n' >"$EV/near-unreachable.txt"
        printf 'near 不可达；journal 未采集\n' >"$EV/near-journal.log"
        printf 'near 不可达；dmesg 未采集\n' >"$EV/near-dmesg.log"
        printf 'near 不可达；stats 未采集\n' >"$EV/near-stats.txt"
        printf 'near 不可达；pstore 未采集\n' >"$EV/near-pstore.txt"
        printf 'near 不可达；ss 未采集\n' >"$EV/near-ss.txt"
        printf 'near 不可达；sampler 未采集\n' >"$EV/near-tbtun-last.log"
        EVIDENCE_INCOMPLETE=1
        return 1
    fi

    # boot_id 读取失败时，用 uptime 兜底判断本次重启。
    if (( ! NEAR_REBOOTED )) && [[ -z $NEAR_BOOT_ID_AFTER ]]; then
        local near_up
        near_up=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cut -d. -f1 /proc/uptime' 2>/dev/null) || near_up=""
        if [[ $near_up =~ ^[0-9]+$ ]] && (( near_up < 600 )); then
            NEAR_REBOOTED=1
            note "near uptime=${near_up}s 且 boot_id 未取到；按本次重启处理"
        fi
    fi

    snapshot "$NEAR_HOST" "$EV/post-near.txt" || note "evidence: post-near 快照失败"

    timeout 60 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'bash -s' >"$EV/near-pstore.txt" 2>&1 <<'EOS' || true
set -u
echo "=== /sys/fs/pstore ==="
ls -la /sys/fs/pstore 2>&1
for f in /sys/fs/pstore/*; do
    [ -f "$f" ] || continue
    echo "--- $f ---"
    cat "$f" 2>&1
done
echo "=== /var/lib/systemd/pstore ==="
ls -la /var/lib/systemd/pstore 2>&1
for d in /var/lib/systemd/pstore/*; do
    [ -d "$d" ] || continue
    echo "--- $d ---"
    ls -la "$d" 2>&1
    for f in "$d"/*; do
        [ -f "$f" ] || continue
        echo "--- $f ---"
        cat "$f" 2>&1
    done
done
EOS

    timeout 90 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        "journalctl -b -1 --since '$START_UTC_J' -n 30000 --no-pager -o short-iso 2>&1" \
        >"$EV/near-journal-b1.log" 2>&1 || true
    timeout 90 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        "journalctl -b --since '$START_UTC_J' -n 30000 --no-pager -o short-iso 2>&1" \
        >"$EV/near-journal-b0.log" 2>&1 || true
    if (( NEAR_REBOOTED )); then
        cp -f "$EV/near-journal-b1.log" "$EV/near-journal.log" 2>/dev/null || true
    else
        cp -f "$EV/near-journal-b0.log" "$EV/near-journal.log" 2>/dev/null || true
    fi
    [[ -s $EV/near-journal.log ]] || EVIDENCE_INCOMPLETE=1

    timeout 30 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'dmesg 2>&1' >"$EV/near-dmesg.log" 2>&1 || true
    timeout 30 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /var/log/tbtun-last.log 2>&1' \
        >"$EV/near-tbtun-last.log" 2>&1 || true
    if timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -f /var/log/tbtun-last.log.old' >/dev/null 2>&1; then
        timeout 30 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /var/log/tbtun-last.log.old 2>&1' \
            >"$EV/near-tbtun-last-old.log" 2>&1 || true
    fi
    SAMPLER_END_LINES=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'wc -l < /var/log/tbtun-last.log 2>/dev/null || echo 0' 2>/dev/null) || SAMPLER_END_LINES=0
    [[ $SAMPLER_END_LINES =~ ^[0-9]+$ ]] || SAMPLER_END_LINES=0
    timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'for f in config ports tunnels peers stats; do echo "=== $f ==="; cat /proc/net/tbtun/$f 2>&1; done' \
        >"$EV/near-stats.txt" 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'echo "=== ss -tan ==="; ss -tan 2>&1; echo "=== ss -s ==="; ss -s 2>&1' \
        >"$EV/near-ss.txt" 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        'echo "=== date ==="; date -u; echo "=== uptime ==="; uptime; echo "=== boot_id ==="; cat /proc/sys/kernel/random/boot_id; echo "=== panic ==="; cat /proc/sys/kernel/panic_on_oops /proc/sys/kernel/panic /proc/sys/kernel/oops_limit /proc/sys/kernel/panic_on_rcu_stall /proc/sys/kernel/oops_all_cpu_backtrace; echo "=== cmdline ==="; cat /proc/cmdline; echo "=== boots ==="; journalctl --list-boots --no-pager 2>&1 | tail -n 6' \
        >"$EV/near-meta.txt" 2>&1 || true
    return 0
}

wait_near_return() {
    local deadline=$((SECONDS + NEAR_RETURN_WAIT))
    while (( SECONDS < deadline )); do
        sleep 5
        near_probe
        if (( PROBE_SSH_RC == 0 )); then
            NEAR_RETURNED_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
            return 0
        fi
    done
    return 1
}

write_stats_summary() {
    POST_NEAR_DMESG=$(sec_number "$EV/post-near.txt" dmesg_count 2>/dev/null)
    POST_FAR_DMESG=$(sec_number "$EV/post-far.txt" dmesg_count 2>/dev/null)
    POST_NEAR_DMESG=${POST_NEAR_DMESG:-0}
    POST_FAR_DMESG=${POST_FAR_DMESG:-0}
    {
        for _end in near far; do
            _f=$EV/post-$_end.txt
            if [[ -f $_f ]]; then
                for _pair in 'c3 up' 'c3 data_tx' 'c3 rx_ack' 'c3 reorder_overflow' \
                             'c8 http_parse_err' 'c8 http_req' 'c8 http_resp' 'c8 profile_mismatch' \
                             'c9 fec_drop_window' 'c9 fec_drop_crypto' 'c9 fec_short_unknown' \
                             'streams streams_active' 'streams n_streams'; do
                    read -r _lk _fld <<<"$_pair"
                    printf '%s.%s.%s=%s\n' "$_end" "$_lk" "$_fld" "$(field "$_f" "$_lk" "$_fld")"
                done
            else
                printf '%s=unavailable\n' "$_end"
            fi
        done
        printf 'dmesg_delta_near=%s dmesg_delta_far=%s\n' \
            "$((POST_NEAR_DMESG - PRE_NEAR_DMESG))" "$((POST_FAR_DMESG - PRE_FAR_DMESG))"
    } >"$EV/stats.txt" 2>&1 || true
}

# ---------------------------------------------------------------------------
# 模块身份。

record_near_module_identity() {
    NEAR_KO_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "sha256sum '$TBT_SRC_DIR/tbtun.ko'" 2>/dev/null | awk '{print $1}') || NEAR_KO_SHA=""
    NEAR_KO_SRCVERSION=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "modinfo -F srcversion '$TBT_SRC_DIR/tbtun.ko' 2>/dev/null" 2>/dev/null) || NEAR_KO_SRCVERSION=""
    NEAR_KO_VERMAGIC=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "modinfo -F vermagic '$TBT_SRC_DIR/tbtun.ko' 2>/dev/null" 2>/dev/null) || NEAR_KO_VERMAGIC=""
    NEAR_TREE_COMMIT=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "git -C '$TBT_SRC_DIR' rev-parse HEAD 2>/dev/null" 2>/dev/null) || NEAR_TREE_COMMIT=""
    {
        printf 'near_src_dir=%s\n' "$TBT_SRC_DIR"
        printf 'near_tree_commit=%s\n' "${NEAR_TREE_COMMIT:-unknown}"
        printf 'near_ko_sha256=%s\n' "${NEAR_KO_SHA:-unknown}"
        printf 'near_ko_srcversion=%s\n' "${NEAR_KO_SRCVERSION:-unknown}"
        printf 'near_ko_vermagic=%s\n' "${NEAR_KO_VERMAGIC:-unknown}"
        printf 'near_loaded_srcversion=%s\n' "${NEAR_LOADED_SRCVERSION:-not-loaded-yet}"
        printf 'expected_srcversion=%s\n' "$EXPECTED_NEAR_SRCVERSION"
        printf 'expected_sha256=%s\n' "$EXPECTED_NEAR_SHA256"
        printf 'expected_vermagic=%s\n' "$EXPECTED_NEAR_VERMAGIC"
        printf 'identity_match=%s\n' "$NEAR_IDENTITY_MATCH"
        printf 'near_insmod_params=%s\n' "$INSMOD_PARAMS"
        printf 'far_stage=%s\n' "$FAR_STAGE"
        printf 'far_ko_sha256=%s\n' "${FAR_KO_SHA:-unknown}"
        printf 'far_ko_srcversion=%s\n' "${FAR_KO_SRCVERSION:-unknown}"
        printf 'far_loaded_srcversion=%s\n' "${FAR_LOADED_SRCVERSION:-unknown}"
        printf 'far_ko_vermagic=%s\n' "${FAR_KO_VERMAGIC:-unknown}"
        printf 'far_expected_srcversion=%s\n' "${EXPECTED_FAR_SRCVERSION:-unset}"
        printf 'far_identity_match=%s\n' "$FAR_IDENTITY_MATCH"
        printf 'far_ko_mtime=%s\n' "$FAR_KO_MTIME"
        printf 'far_known_bad_srcversions=%s\n' "$FAR_KNOWN_BAD_SRCVERSIONS"
    } >"$EV/module.txt"
}

# ---------------------------------------------------------------------------
# SUMMARY.md。cleanup 在最后调用它，所以清理结果也在里面。

write_summary() {
    local rc=$1
    local oops_nc=0 panic_nc=0 oops_jr=0 oops_ps=0 oops_sm=0
    local verdict join_actual burst_actual lost_actual clean_actual
    local near_state far_state evidence_state

    if [[ -f $EV/netconsole-window.log ]]; then
        oops_nc=$(grep -acE 'Internal error: Oops' "$EV/netconsole-window.log" 2>/dev/null || true)
        panic_nc=$(grep -acE 'Kernel panic' "$EV/netconsole-window.log" 2>/dev/null || true)
    fi
    if [[ -f $EV/near-journal.log ]]; then
        oops_jr=$(grep -acE 'Internal error: Oops|Kernel panic' "$EV/near-journal.log" 2>/dev/null || true)
    fi
    if [[ -f $EV/near-pstore.txt ]]; then
        oops_ps=$(grep -acE 'Internal error: Oops|Kernel panic' "$EV/near-pstore.txt" 2>/dev/null || true)
    fi
    if [[ -f $EV/near-tbtun-last.log ]]; then
        oops_sm=$(grep -acE 'Internal error: Oops|Kernel panic' "$EV/near-tbtun-last.log" 2>/dev/null || true)
    fi
    oops_nc=${oops_nc:-0}
    panic_nc=${panic_nc:-0}
    oops_jr=${oops_jr:-0}
    oops_ps=${oops_ps:-0}
    oops_sm=${oops_sm:-0}

    if (( NEAR_LOST )); then
        if (( oops_nc + panic_nc + oops_jr + oops_ps + oops_sm > 0 )); then
            verdict="崩溃复现：near 失联，netconsole/journal/pstore/sampler 有 Oops/panic 证据；禁止重试。"
        else
            verdict="near 失联，但现有证据没有 Oops/panic；原因未定（hang、NIC、掉电或证据未送达）。"
        fi
    elif (( NEAR_REBOOTED )); then
        verdict="near 重启（panic_on_oops 路径），证据已收集；按崩溃复现处理，禁止重试。"
    elif (( PRECHECK_DONE == 0 )); then
        verdict="前置检查失败，未加载模块、未触发 burst；见步骤表与 run.log。"
    elif (( NEAR_CFG_OK == 0 )); then
        verdict="设置阶段中止（far/near 配置未完成），未触发 burst；见步骤表与 run.log。"
    elif (( JOIN_OK == 0 )); then
        verdict="join gate 失败，未触发 burst；未复现崩溃。"
    elif (( BURST_ATTEMPTED == 0 )); then
        verdict="触发前中止（可见性探针或设置失败），未跑 burst；见 run.log。"
    elif (( oops_nc + panic_nc + oops_ps + oops_sm > 0 )); then
        verdict="证据记录到 Oops/panic，但 near 当前可达且未检测到重启；需人工核对，不能判定修复有效。"
    elif [[ ${BURST_RC:-} == 0 ]]; then
        verdict="修复有效：join 成功，单次 1 MiB burst 通过，near 存活。"
    elif [[ -n ${BURST_RC:-} ]]; then
        verdict="join 成功，burst rc=${BURST_RC}，near 存活；未崩溃但 burst 未通过。"
    else
        verdict="join 成功，burst 结果未知（near 存活或 ssh 中断）；见 burst.txt。"
    fi

    join_actual="JOIN_OK=${JOIN_OK} near_up=${NUP_LAST:-?} far_up=${FUP_LAST:-?} counter_warn=${GATE_WARN}"
    burst_actual="attempted=${BURST_ATTEMPTED} ssh_rc=${BURST_SSH_RC:-未执行} burst_rc=${BURST_RC:-未执行} wall_ms=${WALL_MS:-?}"
    lost_actual="near_lost=${NEAR_LOST} lost_at=${NEAR_LOST_AT:-none} returned_at=${NEAR_RETURNED_AT:-none} rebooted=${NEAR_REBOOTED}"
    clean_actual="clean_fail=${CLEAN_FAIL}"
    near_state=$(if near_ssh_ok; then echo reachable; else echo unreachable; fi)
    far_state=$(if timeout 10 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1; then echo reachable; else echo unreachable; fi)
    evidence_state=$(if (( EVIDENCE_INCOMPLETE )); then echo incomplete; else echo complete; fi)

    {
        printf '# controlled-c8-once 实验报告\n\n'
        printf -- '- 实验 UTC：%s\n' "$STAMP"
        printf -- '- 退出码：%s\n' "$rc"
        printf -- '- 判定：%s\n' "$verdict"
        printf -- '- near：%s；far：%s；证据：%s\n\n' "$near_state" "$far_state" "$evidence_state"

        printf '## 实验配置\n\n'
        printf '```\n%s\n%s\n```\n\n' "$NEAR_CFG" "$FAR_CFG"
        printf -- '- near 端口块：%s；far 端口块：%s；carriers=%s；join 超时 %ss；burst 超时 %ss\n' \
            "$NEAR_PORTBLOCK" "$FAR_PORTBLOCK" "$CARRIERS" "$JOIN_TIMEOUT" "$BURST_TIMEOUT"
        printf -- '- 触发：恰好一次 tbt_burst 127.0.0.1 %s 1 5 interleaved（不重试）\n' "$NEAR_PORT"
        printf -- '- 选择理由：与事故的 1 MiB / 5 s 触发逐字一致；tbt_ping 37B 只验证 join，覆盖不足。\n\n'

        printf '## 模块身份\n\n'
        printf '| 项 | near | far |\n|---|---|---|\n'
        printf '| staged tree | `%s` | `%s` |\n' "$TBT_SRC_DIR" "$FAR_STAGE"
        printf '| tree commit | `%s` | — |\n' "${NEAR_TREE_COMMIT:-unknown}"
        printf '| ko sha256 | `%s` | `%s` |\n' "${NEAR_KO_SHA:-unknown}" "${FAR_KO_SHA:-unknown}"
        printf '| ko srcversion | `%s` | `%s` |\n' "${NEAR_KO_SRCVERSION:-unknown}" "${FAR_KO_SRCVERSION:-unknown}"
        printf '| loaded srcversion | `%s` | `%s` |\n' "${NEAR_LOADED_SRCVERSION:-unknown}" "${FAR_LOADED_SRCVERSION:-unknown}"
        printf '| vermagic | `%s` | `%s` |\n' "${NEAR_KO_VERMAGIC:-unknown}" "${FAR_KO_VERMAGIC:-unknown}"
        printf '| expected srcversion | `%s` | `%s` |\n' \
            "$EXPECTED_NEAR_SRCVERSION" "${EXPECTED_FAR_SRCVERSION:-未设置}"
        printf '| expected sha256 | `%s` | — |\n' "$EXPECTED_NEAR_SHA256"
        printf '| expected vermagic | `%s` | kernel `%s` |\n' \
            "$EXPECTED_NEAR_VERMAGIC" "$FAR_KREL"
        printf '| identity match | `%s` | `%s` |\n' "$NEAR_IDENTITY_MATCH" "$FAR_IDENTITY_MATCH"
        printf '| ko mtime (epoch) | — | `%s` |\n' "$FAR_KO_MTIME"
        printf '| staged ko hazard | — | `%s` |\n' \
            "$(if [[ -f $EV/far-stage-hazard.txt ]]; then tr '\n' ' ' <"$EV/far-stage-hazard.txt"; else echo none; fi)"
        printf '| insmod | `%s` | 已由 deploy_hnd0.sh 加载 |\n\n' "$INSMOD_PARAMS"

        printf '## 前置检查与可见性\n\n'
        printf '| 项 | 结果 |\n|---|---|\n'
        printf '| 两端 ssh | %s |\n' "$([[ $PRECHECK_DONE == 1 ]] && echo pass || echo fail)"
        printf '| .65 kernel.panic_on_oops=1 | %s |\n' "$NEAR_PANIC_ON_OOPS"
        printf '| .65 kernel.panic=10 | %s |\n' "$NEAR_PANIC"
        printf '| .65 额外 sysctl | %s |\n' "$NEAR_EXTRA_SYSCTLS"
        printf '| .65 /sys/fs/pstore 为空 | %s |\n' "$NEAR_PSTORE_EMPTY"
        printf '| .65 ramoops 已注册 | %s |\n' "$NEAR_RAMOOPS"
        printf '| .65 tbtun-kmsg-relay | %s |\n' "$KMSG_RELAY_STATE"
        printf '| .65 tbtun-sampler | %s（baseline lines=%s） |\n' "$SAMPLER_STATE" "$SAMPLER_BASELINE_LINES"
        printf '| .80 UDP %s 接收端 | pid=%s log=%s |\n' "$NC_PORT" "${NC_PID:-?}" "${NC_LOG:-?}"
        printf '| .80 tbtun-netconsole-rx | %s（日志行基线=%s，结束=%s） |\n' \
            "$RX_SERVICE_STATE" "$NC_START_LINES" "$NC_END_LINES"
        printf '| kmsg 探针 | %s |\n' "$NC_PROBE_OK"
        printf '| far tbtun 已加载 | %s |\n' "$FAR_MODULE_STATE"
        printf '| far fixed srcversion | %s（expected %s，match=%s） |\n' \
            "${FAR_LOADED_SRCVERSION:-unknown}" "${EXPECTED_FAR_SRCVERSION:-未设置}" "$FAR_IDENTITY_MATCH"
        printf '| far c8 config / listeners | %s / %s |\n' \
            "$([[ $FAR_CFG_OK == 1 ]] && echo pass || echo fail)" "$FAR_LISTEN_STATE"
        printf '| near tbtun 未加载 | %s |\n' "$NEAR_CLEAN_STATE"
        printf '\n'

        printf '## 步骤、预期结果与失败分支\n\n'
        printf '| # | 步骤 | 预期结果 | 失败分支 | 实际 |\n|---|---|---|---|---|\n'
        printf '| 1 | 前置检查 | 两端 ssh、near panic/ramoops、可见性服务、far 已加载且 srcversion 为修复版、far staged .ko 不是已知事故构建、near 未加载、near 模块身份逐项匹配 | 任一失败：退出 3，不加载模块、不写配置；far 不匹配或 stage 是旧构建时停止、不跑 near | %s |\n' \
            "$([[ $PRECHECK_DONE == 1 ]] && echo pass || echo fail)"
        printf '| 2 | 锁与负载 | 两端 flock 获取，load1 <= %s | 锁忙/负载高：退出 4，不跑 | locks_held=%s |\n' \
            "$LOAD_LIMIT" "$LOCKS_WERE_HELD"
        printf '| 3 | 可见性检查 | .65 relay/sampler active，.80 接收端 active，kmsg 探针在接收文件落盘 | 任一服务 inactive 或探针丢失：退出 3，不跑 | probe=%s relay=%s sampler=%s rx=%s |\n' \
            "$NC_PROBE_OK" "$KMSG_RELAY_STATE" "$SAMPLER_STATE" "$RX_SERVICE_STATE"
        printf '| 4 | far 配置 | far fixed srcversion 已匹配、staged .ko 检查通过；ports %s、c8 config（carriers=8/n=4/m=4/fec=on/grouped/w=8/crypto=on/http-kal）逐字读回，9000/5202 监听 | 写失败/读回不一致/监听未起：退出 10，near 不加载 | %s |\n' \
            "$FAR_PORTBLOCK" "$([[ $FAR_CFG_OK == 1 ]] && echo pass || echo fail)"
        printf '| 5 | near 配置 | ip rule、rmem/wmem、insmod、加载后 srcversion=%s、ports %s、config 逐字读回 | 任一步失败：退出 10，清理 | %s |\n' \
            "$EXPECTED_NEAR_SRCVERSION" "$NEAR_PORTBLOCK" "$([[ $NEAR_CFG_OK == 1 ]] && echo pass || echo fail)"
        printf '| 6 | join gate | %ss 内两端 up=%s；parse/mismatch/FEC 计数器记录但不阻塞 | 超时或快照 up 不一致：退出 5，不触发 burst，收集证据 | %s |\n' \
            "$JOIN_TIMEOUT" "$CARRIERS" "$join_actual"
        printf '| 7 | 触发 | 恰好一次 1 MiB interleaved burst，%ss 内返回 | 非 0/超时：记录，不重试；near 失联则转证据收集 | %s |\n' \
            "$BURST_TIMEOUT" "$burst_actual"
        printf '| 8 | 失联判定 | 记录 ping/ARP/ssh、失联与恢复时刻、是否重启 | 无法判定：记录 unknown，继续 far/netconsole 证据 | %s |\n' \
            "$lost_actual"
        printf '| 9 | 证据收集 | netconsole、pstore、journal -b -1 --since、sampler、dmesg、stats、far dmesg/ss 全部落盘 | 单项失败：记录原因，不阻塞其他项；无更优先判定时退出 8 | %s |\n' \
            "$evidence_state"
        printf '| 10 | 清理 | near rmmod/rule/sysctl、far rmmod/echo、锁释放 | 失联跳过并记录；清理失败退出 9 | %s |\n\n' \
            "$clean_actual"

        printf '## 崩溃证据计数\n\n'
        printf -- '- netconsole 窗口内 `Internal error: Oops`：%s\n' "$oops_nc"
        printf -- '- netconsole 窗口内 `Kernel panic`：%s\n' "$panic_nc"
        printf -- '- near journal 内 Oops/panic：%s\n' "$oops_jr"
        printf -- '- netconsole 窗口：行 %s -> %s（%s -> %s bytes）\n' \
            "$NC_START_LINES" "$NC_END_LINES" "$NC_START_SIZE" "$NC_END_SIZE"
        printf -- '- sampler：行 %s -> %s\n' "$SAMPLER_BASELINE_LINES" "$SAMPLER_END_LINES"
        printf -- '- pstore：见 `near-pstore.txt`（同时收集 /sys/fs/pstore 与 systemd-pstore 归档）\n'
        printf -- '- 注意：kmsg-relay/sampler 是用户态进程；panic 后可能停止，pstore 是主证据。\n\n'

        printf '## 时间线\n\n'
        printf '```\n'
        tail -n 40 "$EV/timeline.txt" 2>/dev/null || true
        printf '```\n\n'

        printf '## 证据清单\n\n'
        printf '| 文件 | 字节 |\n|---|---|\n'
        find "$EV" -maxdepth 1 -type f -printf '| `%f` | %s |\n' 2>/dev/null | sort || true
        printf '\n'

        printf '## 清理\n\n'
        printf '```\n'
        cat "$EV/cleanup.txt" 2>/dev/null || printf 'cleanup.txt 缺失\n'
        printf '```\n\n'

        printf '## 结论与限制\n\n'
        printf -- '- %s\n' "$verdict"
        printf -- '- 本次实验只触发一次；无论结果如何都不应重跑同一构建。\n'
        printf -- '- 若复现崩溃：先保存本目录，再按 `stop-65-down-*/forensics-65/README.md` 的只读取证流程分析；不要重新加载模块。\n'
        printf -- '- 若 near 失联但无 Oops：netconsole 可能未送达 panic 输出，pstore/journal 也可能为空；结论只能写“未定”。\n'
        printf -- '- netconsole 日志的窗口从本次布防之后开始；完整文件同时保存在 `netconsole.log`。\n'
    } >"$EV/SUMMARY.md" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# 清理。任何一步都不能挂住；near 失联时跳过并记录。

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    set +e
    mark cleanup_start "exit_rc=$rc"

    {
        printf '=== cleanup %s ===\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'near_reachable=%s near_rebooted=%s far_reachable=%s\n' \
            "$(near_ssh_ok && echo yes || echo no)" "$NEAR_REBOOTED" \
            "$(timeout 10 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1 && echo yes || echo no)"
    } >"$EV/cleanup.txt"

    if near_ssh_ok; then
        if (( NEAR_MODULE_LOADED && ! NEAR_REBOOTED )); then
            if timeout 25 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'rmmod tbtun' >>"$EV/cleanup.txt" 2>&1; then
                printf 'near_rmmod=ok\n' >>"$EV/cleanup.txt"
            else
                printf 'near_rmmod=failed\n' >>"$EV/cleanup.txt"
                note "cleanup: near rmmod 失败"
                CLEAN_FAIL=1
            fi
        elif (( NEAR_REBOOTED )); then
            printf 'near_rmmod=skipped(rebooted)\n' >>"$EV/cleanup.txt"
        else
            printf 'near_rmmod=skipped(not_loaded)\n' >>"$EV/cleanup.txt"
        fi
        if (( NEAR_RULE_ADDED && ! NEAR_REBOOTED )); then
            if timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
                'ip rule del to 45.11.1.62/32 lookup main pref 100' >>"$EV/cleanup.txt" 2>&1; then
                printf 'near_rule=deleted\n' >>"$EV/cleanup.txt"
            else
                printf 'near_rule=delete_failed_or_absent\n' >>"$EV/cleanup.txt"
            fi
        fi
        if (( NEAR_SYSCTL_SAVED && ! NEAR_REBOOTED )) && [[ -n $OLD_RMEM && -n $OLD_WMEM ]]; then
            if timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
                "sysctl -q -w net.core.rmem_max=$OLD_RMEM net.core.wmem_max=$OLD_WMEM" >>"$EV/cleanup.txt" 2>&1; then
                printf 'near_sysctl=restored\n' >>"$EV/cleanup.txt"
            else
                printf 'near_sysctl=restore_failed\n' >>"$EV/cleanup.txt"
                CLEAN_FAIL=1
            fi
        fi
        if (( NEAR_MODULE_LOADED && ! NEAR_REBOOTED )) && listening_on "$NEAR_HOST" "$NEAR_PORT"; then
            printf 'near_listen_%s=still_listening\n' "$NEAR_PORT" >>"$EV/cleanup.txt"
            CLEAN_FAIL=1
        fi
    else
        printf 'near_cleanup=skipped(unreachable)\n' >>"$EV/cleanup.txt"
        note "cleanup: near 不可达，跳过 rmmod/rule/sysctl；可见性服务保持运行"
    fi

    if timeout 10 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1; then
        if (( FAR_TOUCHED )); then
            timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
            timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
            printf 'far_echo=stopped\n' >>"$EV/cleanup.txt"
            if listening_on "$FAR_HOST" "$ECHO_PORT"; then
                printf 'far_echo=still_listening\n' >>"$EV/cleanup.txt"
                CLEAN_FAIL=1
            fi
            local i
            for i in 1 2 3; do
                if timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'rmmod tbtun' >>"$EV/cleanup.txt" 2>&1; then
                    printf 'far_rmmod=ok\n' >>"$EV/cleanup.txt"
                    break
                fi
                sleep 2
            done
            if module_loaded_on "$FAR_HOST"; then
                printf 'far_rmmod=still_loaded\n' >>"$EV/cleanup.txt"
                note "cleanup: far rmmod 后模块仍在"
                CLEAN_FAIL=1
            fi
        fi
    else
        printf 'far_cleanup=skipped(unreachable)\n' >>"$EV/cleanup.txt"
        (( FAR_TOUCHED )) && CLEAN_FAIL=1
    fi

    lock_release NEAR
    lock_release FAR
    printf 'locks_released=yes\n' >>"$EV/cleanup.txt"

    mark cleanup_done "clean_fail=$CLEAN_FAIL"

    if (( rc == 0 && CLEAN_FAIL == 1 )); then
        rc=$EX_CLEAN
    fi
    if (( rc == 0 && EVIDENCE_INCOMPLETE == 1 )); then
        rc=$EX_EVIDENCE
    fi
    write_summary "$rc"
    set -e
    exit "$rc"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# ---------------------------------------------------------------------------
# 主流程。

mark start "single-shot c8; evidence=$EV"
note "evidence: $EV"

# --- 1. 前置检查 ------------------------------------------------------------

if ! timeout 15 ssh "${SSH_BASE[@]}" "$NEAR_HOST" true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $NEAR_HOST 失败（需要 BatchMode root key）"
fi
if ! timeout 15 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1; then
    die "$EX_PRECOND" "ssh $FAR_HOST 失败（需要 BatchMode root key）"
fi
NEAR_ARCH=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'uname -m' 2>/dev/null) || NEAR_ARCH=""
[[ $NEAR_ARCH == aarch64 ]] || die "$EX_PRECOND" "$NEAR_HOST 是 $NEAR_ARCH，不是 aarch64"
NEAR_KREL=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'uname -r' 2>/dev/null) || NEAR_KREL=""
FAR_KREL=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'uname -r' 2>/dev/null) || FAR_KREL=""
NEAR_BOOT_ID_BEFORE=$(near_boot_id) || NEAR_BOOT_ID_BEFORE=""
[[ -n $NEAR_BOOT_ID_BEFORE ]] || die "$EX_PRECOND" "读不到 $NEAR_HOST 的 boot_id"
mark precheck_ssh "near=$NEAR_ARCH/$NEAR_KREL far=$FAR_KREL boot_id=$NEAR_BOOT_ID_BEFORE"

# 锁先于所有状态检查和写入，避免兄弟 agent 在 check 与 act 之间改状态。
lock_acquire "$NEAR_HOST" "$NEAR_LOCK" "controlled-c8-once $STAMP" NEAR || die "$EX_BUSY" "near 锁忙: $NEAR_LOCK"
lock_acquire "$FAR_HOST" "$FAR_LOCK" "controlled-c8-once $STAMP" FAR || die "$EX_BUSY" "far 锁忙: $FAR_LOCK"
LOCKS_WERE_HELD="yes"

NEAR_LOAD=$(load1_on "$NEAR_HOST") || NEAR_LOAD=""
FAR_LOAD=$(load1_on "$FAR_HOST") || FAR_LOAD=""
[[ -n $NEAR_LOAD && -n $FAR_LOAD ]] || die "$EX_PRECOND" "读不到两端 load1"
awk -v a="$NEAR_LOAD" -v b="$LOAD_LIMIT" 'BEGIN { exit !(a + 0 <= b + 0) }' \
    || die "$EX_BUSY" "$NEAR_HOST load1=$NEAR_LOAD > $LOAD_LIMIT"
awk -v a="$FAR_LOAD" -v b="$LOAD_LIMIT" 'BEGIN { exit !(a + 0 <= b + 0) }' \
    || die "$EX_BUSY" "$FAR_HOST load1=$FAR_LOAD > $LOAD_LIMIT"
mark load "near=$NEAR_LOAD far=$FAR_LOAD limit=$LOAD_LIMIT"

# near：必须未加载、9001 无监听。
NEAR_CLEAN_STATE="pass"
if module_loaded_on "$NEAR_HOST"; then
    NEAR_CLEAN_STATE="fail: tbtun loaded"
fi
if timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -d /proc/net/tbtun' >/dev/null 2>&1; then
    NEAR_CLEAN_STATE="fail: /proc/net/tbtun present"
fi
if listening_on "$NEAR_HOST" "$NEAR_PORT"; then
    NEAR_CLEAN_STATE="fail: $NEAR_PORT listening"
fi
[[ $NEAR_CLEAN_STATE == pass ]] || die "$EX_PRECOND" "near 不干净：$NEAR_CLEAN_STATE；先 rmmod tbtun 并清理"

# near：panic/ramoops 可见性。
NEAR_PANIC_ON_OOPS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/panic_on_oops' 2>/dev/null) || NEAR_PANIC_ON_OOPS="?"
NEAR_PANIC=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/panic' 2>/dev/null) || NEAR_PANIC="?"
NEAR_EXTRA_SYSCTLS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    'printf "oops_limit=%s panic_on_rcu_stall=%s oops_all_cpu_backtrace=%s\n" \
        "$(cat /proc/sys/kernel/oops_limit 2>/dev/null || echo ?)" \
        "$(cat /proc/sys/kernel/panic_on_rcu_stall 2>/dev/null || echo ?)" \
        "$(cat /proc/sys/kernel/oops_all_cpu_backtrace 2>/dev/null || echo ?)"' 2>/dev/null) \
    || NEAR_EXTRA_SYSCTLS="unknown"
[[ $NEAR_PANIC_ON_OOPS == 1 ]] || die "$EX_PRECOND" "$NEAR_HOST kernel.panic_on_oops=$NEAR_PANIC_ON_OOPS，先 sysctl -w kernel.panic_on_oops=1"
[[ $NEAR_PANIC == 10 ]] || die "$EX_PRECOND" "$NEAR_HOST kernel.panic=$NEAR_PANIC，先 sysctl -w kernel.panic=10"
NEAR_PSTORE_EMPTY="no"
if timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -d /sys/fs/pstore && [ -z "$(ls -A /sys/fs/pstore 2>/dev/null)" ]' >/dev/null 2>&1; then
    NEAR_PSTORE_EMPTY="yes"
else
    die "$EX_PRECOND" "$NEAR_HOST /sys/fs/pstore 非空或不存在；先保存旧 dump 再清空"
fi
NEAR_RAMOOPS="no"
if timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -d /sys/module/ramoops || dmesg 2>/dev/null | grep -q "pstore: Registered ramoops"' >/dev/null 2>&1; then
    NEAR_RAMOOPS="yes"
else
    die "$EX_PRECOND" "$NEAR_HOST 未注册 ramoops 后端"
fi
mark precheck_visibility "panic_on_oops=$NEAR_PANIC_ON_OOPS panic=$NEAR_PANIC $NEAR_EXTRA_SYSCTLS pstore_empty=$NEAR_PSTORE_EMPTY ramoops=$NEAR_RAMOOPS"

# near：工具。
MISSING=$(timeout 15 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    'for t in cc timeout ss ip sysctl modinfo sha256sum systemctl wc; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done' 2>/dev/null) || MISSING="ssh"
[[ -z $MISSING ]] || die "$EX_PRECOND" "$NEAR_HOST 缺少工具: $MISSING"

# far 修复版闸门：先于任何 near 写入。
far_identity_gate || die "$EX_PRECOND" "far 修复版身份闸门失败；停止，不跑 near（见上一条）"
mark precheck_far_early "srcversion=$FAR_LOADED_SRCVERSION expected=$EXPECTED_FAR_SRCVERSION"

# 可见性：.65 relay/sampler active，.80 接收端 active。
visibility_precheck || die "$EX_PRECOND" "可见性服务未就绪（relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE）"
NC_PID=$(nc_find_pid) || die "$EX_PRECOND" "工作站 UDP $NC_PORT 没有接收端（ss -ulnp 看不到 pid）"
NC_LOG=$(nc_find_log "$NC_PID") || NC_LOG="$HOME/tbtun-forensics/netconsole-6666.log"
[[ -f $NC_LOG ]] || die "$EX_PRECOND" "UDP $NC_PORT 接收端（pid $NC_PID）没有文件输出，无法保存 netconsole 证据"
[[ -s $NC_LOG ]] || note "netconsole: 接收文件目前为空: $NC_LOG"
NC_START_LINES=$(wc -l <"$NC_LOG" 2>/dev/null) || NC_START_LINES=0
NC_START_SIZE=$(stat -c %s "$NC_LOG" 2>/dev/null) || NC_START_SIZE=0
if ! nc_probe; then
    die "$EX_PRECOND" "kmsg 探针没有到达 $NC_LOG；relay/receiver 路径不通，拒绝开跑"
fi
NC_PROBE_OK=1
mark precheck_receiver "pid=$NC_PID log=$NC_LOG lines=$NC_START_LINES relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE"

# far：必须已由 deploy_hnd0.sh 部署，且 9000/5202 在听，up=0。
FAR_MODULE_STATE="pass"
if ! module_loaded_on "$FAR_HOST"; then
    FAR_MODULE_STATE="fail: tbtun not loaded"
fi
if ! timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'test -d /proc/net/tbtun' >/dev/null 2>&1; then
    FAR_MODULE_STATE="fail: /proc/net/tbtun absent"
fi
[[ $FAR_MODULE_STATE == pass ]] || die "$EX_PRECOND" "far 未加载：$FAR_MODULE_STATE；先跑 deploy_hnd0.sh"
FAR_LISTEN_STATE="pass"
listening_on "$FAR_HOST" "$FAR_PORT" || FAR_LISTEN_STATE="fail: $FAR_PORT not listening"
listening_on "$FAR_HOST" "$ECHO_PORT" || FAR_LISTEN_STATE="fail: $ECHO_PORT not listening"
[[ $FAR_LISTEN_STATE == pass ]] || die "$EX_PRECOND" "far 监听异常：$FAR_LISTEN_STATE"
FAR_UP=$(live_field "$FAR_HOST" c3 up || true)
[[ ${FAR_UP:-0} == 0 ]] || die "$EX_PRECOND" "far 已有 up=$FAR_UP 的活跃载体；先 teardown/redeploy"
timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "test -x '$FAR_STAGE/tbt_echo'" >/dev/null 2>&1 \
    || die "$EX_PRECOND" "far 缺少 $FAR_STAGE/tbt_echo"
far_identity_gate || die "$EX_PRECOND" "far 修复版身份闸门失败；停止，不跑 near（见上一条）"
FAR_KO_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "sha256sum '$FAR_STAGE/tbtun.ko'" 2>/dev/null | awk '{print $1}') || FAR_KO_SHA=""
FAR_KO_SRCVERSION=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "modinfo -F srcversion '$FAR_STAGE/tbtun.ko' 2>/dev/null" 2>/dev/null) || FAR_KO_SRCVERSION=""
FAR_KO_VERMAGIC=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "modinfo -F vermagic '$FAR_STAGE/tbtun.ko' 2>/dev/null" 2>/dev/null) || FAR_KO_VERMAGIC=""
FAR_KO_MTIME=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "stat -c %Y '$FAR_STAGE/tbtun.ko'" 2>/dev/null) || FAR_KO_MTIME=0
if [[ -n $FAR_KO_VERMAGIC ]]; then
    [[ ${FAR_KO_VERMAGIC%% *} == "$FAR_KREL" ]] \
        || die "$EX_PRECOND" "far 模块 vermagic '$FAR_KO_VERMAGIC' != kernel '$FAR_KREL'"
else
    note "warn: 读不到 $FAR_STAGE/tbtun.ko vermagic；以 loaded srcversion 为准（记录）"
fi
if [[ -n $FAR_KO_SRCVERSION ]]; then
    case " $FAR_KNOWN_BAD_SRCVERSIONS " in
        *" $FAR_KO_SRCVERSION "*)
            printf 'far_stage_hazard=known_bad_srcversion:%s\n' "$FAR_KO_SRCVERSION" >"$EV/far-stage-hazard.txt"
            if [[ $ALLOW_BAD_FAR_STAGE != 1 ]]; then
                die "$EX_PRECOND" "far staged $FAR_STAGE/tbtun.ko 是已知事故构建 srcversion=$FAR_KO_SRCVERSION；当前 loaded=$FAR_LOADED_SRCVERSION 是修复版，但下一次 far redeploy 会回退。先替换 stage，或显式设置 ALLOW_BAD_FAR_STAGE=1"
            fi
            note "warn: far staged .ko 是已知事故构建；ALLOW_BAD_FAR_STAGE=1，继续（loaded 仍按 expected 校验）"
            ;;
    esac
fi
if [[ -n $FAR_KO_SRCVERSION && $FAR_KO_SRCVERSION != "$FAR_LOADED_SRCVERSION" ]]; then
    note "warn: far 已加载 srcversion=$FAR_LOADED_SRCVERSION != $FAR_STAGE/tbtun.ko srcversion=$FAR_KO_SRCVERSION（记录，不阻塞）"
fi
FAR_PRE_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || FAR_PRE_CFG=""
require_tokens "far pre" "$FAR_PRE_CFG" \
    role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped crypto=on profile=http-kal \
    nonce=00112233445566778899aabbccddeeff \
    || die "$EX_PRECOND" "far 现有 config 不是 far 形状：'$FAR_PRE_CFG'"
printf '%s\n' "$FAR_PRE_CFG" >"$EV/far-pre-config.txt"
mark precheck_far "srcversion=$FAR_LOADED_SRCVERSION expected=$EXPECTED_FAR_SRCVERSION ko_sha=${FAR_KO_SHA:-unknown} ko_mtime=$FAR_KO_MTIME"

# near：固定 staged tree，校验模块身份与构建时间。
[[ -n $TBT_SRC_DIR ]] || die "$EX_PRECOND" "找不到 staged tbtun.ko；先构建修复后的模块并设置 TBT_SRC_DIR"
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -s '$TBT_SRC_DIR/tbtun.ko'" >/dev/null 2>&1 \
    || die "$EX_PRECOND" "$TBT_SRC_DIR/tbtun.ko 不存在或为空"
NEAR_KO_MTIME=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "stat -c %Y '$TBT_SRC_DIR/tbtun.ko'" 2>/dev/null) || NEAR_KO_MTIME=0
[[ $NEAR_KO_MTIME =~ ^[0-9]+$ ]] || die "$EX_PRECOND" "读不到 $TBT_SRC_DIR/tbtun.ko 的 mtime"
CRASH_EPOCH=$(date -u -d "$CRASH_UTC" +%s)
if (( NEAR_KO_MTIME <= CRASH_EPOCH )) && [[ $ALLOW_PRE_CRASH_MODULE != 1 ]]; then
    die "$EX_PRECOND" "tbtun.ko mtime $(date -u -d "@$NEAR_KO_MTIME" +%Y-%m-%dT%H:%M:%SZ) 早于已知崩溃 $CRASH_UTC；这可能是已知会崩的旧构建。构建修复版，或显式设置 ALLOW_PRE_CRASH_MODULE=1"
fi
record_near_module_identity
[[ ${NEAR_KO_VERMAGIC%% *} == "$NEAR_KREL" ]] \
    || die "$EX_PRECOND" "near 模块 vermagic '$NEAR_KO_VERMAGIC' != kernel '$NEAR_KREL'"
[[ -n $NEAR_KO_SRCVERSION ]] || die "$EX_PRECOND" "near 模块没有 srcversion"
[[ $NEAR_KO_SHA == "$EXPECTED_NEAR_SHA256" ]] \
    || die "$EX_PRECOND" "near ko sha256 '$NEAR_KO_SHA' != expected '$EXPECTED_NEAR_SHA256'"
[[ $NEAR_KO_SRCVERSION == "$EXPECTED_NEAR_SRCVERSION" ]] \
    || die "$EX_PRECOND" "near ko srcversion '$NEAR_KO_SRCVERSION' != expected '$EXPECTED_NEAR_SRCVERSION'"
[[ $NEAR_KO_VERMAGIC == "$EXPECTED_NEAR_VERMAGIC" ]] \
    || die "$EX_PRECOND" "near ko vermagic '$NEAR_KO_VERMAGIC' != expected '$EXPECTED_NEAR_VERMAGIC'"
NEAR_IDENTITY_MATCH="yes"
mark precheck_module "dir=$TBT_SRC_DIR srcversion=$NEAR_KO_SRCVERSION sha=${NEAR_KO_SHA:-unknown} identity=match"

mark visibility_ready "pid=$NC_PID log=$NC_LOG lines=$NC_START_LINES relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE"

# 预快照。
note "snapshot: pre"
snapshot "$NEAR_HOST" "$EV/pre-near.txt" || die "$EX_PRECOND" "near 预快照失败"
snapshot "$FAR_HOST" "$EV/pre-far.txt" || die "$EX_PRECOND" "far 预快照失败"
PRE_NEAR_DMESG=$(sec_number "$EV/pre-near.txt" dmesg_count)
PRE_FAR_DMESG=$(sec_number "$EV/pre-far.txt" dmesg_count)
PRE_NEAR_DMESG=${PRE_NEAR_DMESG:-0}
PRE_FAR_DMESG=${PRE_FAR_DMESG:-0}

# burst 工具：只构建，不运行。
if ! timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -x '$TBT_SRC_DIR/tbt_burst'" >/dev/null 2>&1; then
    if ! timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -f '$TBT_SRC_DIR/host/tbt_burst.c'" >/dev/null 2>&1; then
        rscp_near "$REPO/host/tbt_burst.c" "$NEAR_HOST:$TBT_SRC_DIR/host/tbt_burst.c" \
            || die "$EX_SETUP" "拷贝 tbt_burst.c 到 near 失败"
    fi
    timeout 60 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        "cd '$TBT_SRC_DIR' && cc -O2 -Wall -Wextra -o tbt_burst host/tbt_burst.c" \
        >"$EV/burst-build.log" 2>&1 \
        || die "$EX_SETUP" "near 上构建 tbt_burst 失败；日志 $EV/burst-build.log"
fi
BURST_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "sha256sum '$TBT_SRC_DIR/tbt_burst' '$TBT_SRC_DIR/host/tbt_burst.c'" 2>/dev/null) || BURST_SHA=""
printf 'burst_sha256:\n%s\n' "${BURST_SHA:-unknown}" >>"$EV/module.txt"
mark precheck_done "burst_sha=${BURST_SHA%% *}"
PRECHECK_DONE=1

# --- 2. far 配置 ------------------------------------------------------------

mark far_config_start
FAR_TOUCHED=1
timeout 15 ssh "${SSH_BASE[@]}" "$FAR_HOST" "printf 'ports $FAR_PORTBLOCK\n' > /proc/net/tbtun/ports" \
    || die "$EX_SETUP" "far ports 写入失败"
FAR_GOT_PORTS=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "grep '^ports ' /proc/net/tbtun/ports" 2>/dev/null) || FAR_GOT_PORTS=""
[[ $FAR_GOT_PORTS == "ports $FAR_PORTBLOCK" ]] \
    || die "$EX_SETUP" "far ports 读回 '$FAR_GOT_PORTS' != 'ports $FAR_PORTBLOCK'"

timeout 15 ssh "${SSH_BASE[@]}" "$FAR_HOST" "printf '%s\n' '$FAR_CFG' > /proc/net/tbtun/config" \
    || die "$EX_SETUP" "far config 写入失败"
FAR_GOT_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || FAR_GOT_CFG=""
require_tokens "far c8" "$FAR_GOT_CFG" \
    role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n=4 m=4 fec=on mode=grouped w=8 carriers=8 \
    crypto=on profile=http-kal portblock=31000-31015 nonce=00112233445566778899aabbccddeeff \
    || die "$EX_SETUP" "far config 不是 c8 形状：'$FAR_GOT_CFG'"
[[ $FAR_GOT_CFG == "$FAR_CFG" ]] \
    || die "$EX_SETUP" "far config 读回不一致：'$FAR_GOT_CFG'"
printf '%s\n' "$FAR_GOT_CFG" >"$EV/config-far.txt"

# config 写入会重建监听；等 9000 回来，再重启 echo。
FAR_LISTEN_OK=0
for _i in $(seq 1 20); do
    if listening_on "$FAR_HOST" "$FAR_PORT"; then
        FAR_LISTEN_OK=1
        break
    fi
    sleep 0.5
done
(( FAR_LISTEN_OK == 1 )) || die "$EX_SETUP" "far $FAR_PORT 在 config 写入后未重新监听"
far_restart_echo || die "$EX_SETUP" "far echo $ECHO_PORT 重启失败"
FAR_CFG_OK=1
mark far_config_done "ports=$FAR_PORTBLOCK config=exact echo=$ECHO_PORT"

# --- 3. near 配置 -----------------------------------------------------------

mark near_config_start
NEAR_RULES=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'ip rule show' 2>/dev/null) || NEAR_RULES=""
case $NEAR_RULES in
    *'45.11.1.62 lookup main'*)
        note "rule: 45.11.1.62 via main 已存在，保留"
        ;;
    *)
        timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'ip rule add to 45.11.1.62/32 lookup main pref 100' \
            || die "$EX_SETUP" "near ip rule 添加失败"
        NEAR_RULE_ADDED=1
        ;;
esac
ROUTE=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'ip route get 45.11.1.62' 2>/dev/null) || ROUTE=""
printf '%s\n' "$ROUTE" >"$EV/route.txt"
DEV=$(printf '%s\n' "$ROUTE" | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -n 1)
case $DEV in
    Meta | tun[0-9]* | utun[0-9]* | wg[0-9]* | tailscale[0-9]*)
        die "$EX_PRECOND" "到 45.11.1.62 仍走 $DEV：$ROUTE" ;;
    '')
        die "$EX_PRECOND" "到 45.11.1.62 没有路由：$ROUTE" ;;
esac
mark route "dev=$DEV"

OLD_RMEM=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'sysctl -n net.core.rmem_max' 2>/dev/null) || OLD_RMEM=""
OLD_WMEM=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'sysctl -n net.core.wmem_max' 2>/dev/null) || OLD_WMEM=""
[[ -n $OLD_RMEM && -n $OLD_WMEM ]] || die "$EX_SETUP" "读不到 near rmem/wmem"
NEAR_SYSCTL_SAVED=1
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    "sysctl -q -w net.core.rmem_max=$SYSCTL_MAX net.core.wmem_max=$SYSCTL_MAX" \
    || die "$EX_SETUP" "near rmem/wmem 写入失败"

timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "insmod '$TBT_SRC_DIR/tbtun.ko' $INSMOD_PARAMS" \
    || die "$EX_SETUP" "near insmod 失败"
NEAR_MODULE_LOADED=1
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -d /proc/net/tbtun' >/dev/null 2>&1 \
    || die "$EX_SETUP" "insmod 后 /proc/net/tbtun 不存在"
NEAR_LOADED_SRCVERSION=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /sys/module/tbtun/srcversion' 2>/dev/null) || NEAR_LOADED_SRCVERSION=""
[[ $NEAR_LOADED_SRCVERSION == "$EXPECTED_NEAR_SRCVERSION" ]] \
    || die "$EX_SETUP" "加载的 srcversion '$NEAR_LOADED_SRCVERSION' != expected '$EXPECTED_NEAR_SRCVERSION'"
record_near_module_identity

timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "printf 'ports $NEAR_PORTBLOCK\n' > /proc/net/tbtun/ports" \
    || die "$EX_SETUP" "near ports 写入失败"
NEAR_GOT_PORTS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "grep '^ports ' /proc/net/tbtun/ports" 2>/dev/null) || NEAR_GOT_PORTS=""
[[ $NEAR_GOT_PORTS == "ports $NEAR_PORTBLOCK" ]] \
    || die "$EX_SETUP" "near ports 读回 '$NEAR_GOT_PORTS' != 'ports $NEAR_PORTBLOCK'"

# 写 config 即开始 dial；join 计时从这一刻起。
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "printf '%s\n' '$NEAR_CFG' > /proc/net/tbtun/config" \
    || die "$EX_SETUP" "near config 写入失败"
NEAR_GOT_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || NEAR_GOT_CFG=""
require_tokens "near c8" "$NEAR_GOT_CFG" \
    role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=8 \
    crypto=on profile=http-kal portblock=30000-30015 nonce=00112233445566778899aabbccddeeff \
    || die "$EX_SETUP" "near config 不是 c8 形状：'$NEAR_GOT_CFG'"
[[ $NEAR_GOT_CFG == "$NEAR_CFG" ]] \
    || die "$EX_SETUP" "near config 读回不一致：'$NEAR_GOT_CFG'"
printf '%s\n' "$NEAR_GOT_CFG" >"$EV/config-near.txt"
NEAR_CFG_OK=1
mark near_config_done "ports=$NEAR_PORTBLOCK config=exact"

# --- 4. join gate -----------------------------------------------------------

mark join_start "timeout=${JOIN_TIMEOUT}s target=up=$CARRIERS"
JOIN_DEADLINE=$((SECONDS + JOIN_TIMEOUT))
NEAR_FAILS=0
while (( SECONDS < JOIN_DEADLINE )); do
    NUP=$(live_field "$NEAR_HOST" c3 up || true)
    FUP=$(live_field "$FAR_HOST" c3 up || true)
    NUP_LAST=${NUP:-?}
    FUP_LAST=${FUP:-?}
    printf '%s near_up=%s far_up=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${NUP:-?}" "${FUP:-?}" >>"$EV/join-probes.txt"
    if [[ $NUP == "$CARRIERS" && $FUP == "$CARRIERS" ]]; then
        JOIN_OK=1
        break
    fi
    if [[ -z $NUP ]]; then
        NEAR_FAILS=$((NEAR_FAILS + 1))
    else
        NEAR_FAILS=0
    fi
    if (( NEAR_FAILS >= 3 )); then
        near_probe
        if (( PROBE_SSH_RC != 0 )); then
            NEAR_LOST=1
            NEAR_LOST_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
            mark near_lost "during join; ping_rc=$PROBE_PING_RC ssh_rc=$PROBE_SSH_RC"
            break
        fi
    fi
    sleep 2
done
if (( JOIN_OK )); then
    mark join_ok "near_up=$CARRIERS far_up=$CARRIERS"
    snapshot "$NEAR_HOST" "$EV/joined-near.txt" || true
    snapshot "$FAR_HOST" "$EV/joined-far.txt" || true
    GATE_FAIL=0
    GATE_WARN=0
    for _end in near far; do
        _f=$EV/joined-$_end.txt
        [[ $(field "$_f" c3 up) == "$CARRIERS" ]] || { note "gate: $_end up=$(field "$_f" c3 up)"; GATE_FAIL=1; }
        for _pair in 'c8 http_parse_err' 'c8 profile_mismatch' 'c9 fec_drop_window' 'c9 fec_drop_crypto' 'c9 fec_short_unknown'; do
            read -r _lk _fld <<<"$_pair"
            _v=$(field "$_f" "$_lk" "$_fld")
            [[ $_v == 0 ]] || { note "warn: $_end $_fld=${_v:-missing}（记录，不阻塞）"; GATE_WARN=1; }
        done
    done
    if (( GATE_FAIL )); then
        JOIN_OK=0
        mark join_gate_failed "up mismatch in joined snapshot"
    elif (( GATE_WARN )); then
        mark join_counters "nonzero counters recorded, not gated"
    fi
fi
if (( JOIN_OK == 0 )); then
    note "join: 失败 near_up=${NUP_LAST:-?} far_up=${FUP_LAST:-?} near_lost=$NEAR_LOST"
    nc_snapshot || true
    collect_far_evidence || true
    if (( NEAR_LOST )); then
        wait_near_return || true
        if [[ -n $NEAR_RETURNED_AT ]]; then
            NEAR_BOOT_ID_AFTER=$(near_boot_id) || NEAR_BOOT_ID_AFTER=""
            [[ -n $NEAR_BOOT_ID_AFTER && $NEAR_BOOT_ID_AFTER != "$NEAR_BOOT_ID_BEFORE" ]] && NEAR_REBOOTED=1
        fi
    fi
    collect_near_evidence || true
    write_stats_summary
    nc_snapshot || true
    near_probe
    if (( PROBE_SSH_RC != 0 )) && (( ! NEAR_LOST )); then
        NEAR_LOST=1
        NEAR_LOST_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
        mark near_lost "final probe after join failure; ping_rc=$PROBE_PING_RC ssh_rc=$PROBE_SSH_RC"
        nc_snapshot || true
    fi
    mark evidence_done "join_failed near_lost=$NEAR_LOST rebooted=$NEAR_REBOOTED"
    if (( NEAR_LOST || NEAR_REBOOTED )); then
        exit "$EX_LOST"
    fi
    exit "$EX_JOIN"
fi

# --- 5. 唯一一次 burst ------------------------------------------------------

if (( BURST_ATTEMPTED )); then
    die "$EX_USAGE" "内部错误：burst 已经尝试过，拒绝第二次"
fi
# 可见性服务可能被外部 agent 改动；触发前再探一次。
if ! nc_probe; then
    EVIDENCE_INCOMPLETE=1
    die "$EX_PRECOND" "触发前 kmsg 探针丢失；拒绝开跑（崩溃证据无法保证）"
fi
BURST_ATTEMPTED=1
mark burst_start "tbt_burst 127.0.0.1 $NEAR_PORT 1 5 interleaved timeout=${BURST_TIMEOUT}s"
set +e
timeout $((BURST_TIMEOUT + 30)) ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    "cd '$TBT_SRC_DIR' && start=\$(date +%s%N); timeout $BURST_TIMEOUT ./tbt_burst 127.0.0.1 $NEAR_PORT 1 5 interleaved; rc=\$?; end=\$(date +%s%N); printf 'burst_rc=%d wall_ms=%d\n' \"\$rc\" \"\$(( (end - start) / 1000000 ))\"; exit \$rc" \
    >"$EV/burst.txt" 2>&1
BURST_SSH_RC=$?
set -e
BURST_RC=$(sed -n 's/^burst_rc=\([0-9-]*\) wall_ms=.*/\1/p' "$EV/burst.txt" | tail -n 1)
WALL_MS=$(sed -n 's/^burst_rc=[0-9-]* wall_ms=\([0-9]*\)$/\1/p' "$EV/burst.txt" | tail -n 1)
BURST_END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark burst_end "ssh_rc=$BURST_SSH_RC burst_rc=${BURST_RC:-unknown} wall_ms=${WALL_MS:-?} at=$BURST_END_UTC"

# --- 6. 失联判定与证据收集 --------------------------------------------------

note "evidence: netconsole（burst 后第一份快照）"
nc_snapshot || true
mark netconsole_snapshot "start=$NC_START_SIZE end=$NC_END_SIZE"

collect_far_evidence || true
mark far_evidence_done

near_probe
if (( PROBE_SSH_RC != 0 )); then
    NEAR_LOST=1
    NEAR_LOST_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    mark near_lost "after burst; ping_rc=$PROBE_PING_RC ssh_rc=$PROBE_SSH_RC"
    note "near 失联，等待最多 ${NEAR_RETURN_WAIT}s 以便取 -b -1 与 pstore"
    if wait_near_return; then
        mark near_returned "at=$NEAR_RETURNED_AT"
        NEAR_BOOT_ID_AFTER=$(near_boot_id) || NEAR_BOOT_ID_AFTER=""
        if [[ -n $NEAR_BOOT_ID_AFTER && $NEAR_BOOT_ID_AFTER != "$NEAR_BOOT_ID_BEFORE" ]]; then
            NEAR_REBOOTED=1
            mark near_rebooted "boot_id $NEAR_BOOT_ID_BEFORE -> $NEAR_BOOT_ID_AFTER"
        fi
    else
        note "near 在 ${NEAR_RETURN_WAIT}s 内没有恢复"
        mark near_not_returned "waited=${NEAR_RETURN_WAIT}s"
    fi
else
    NEAR_BOOT_ID_AFTER=$(near_boot_id) || NEAR_BOOT_ID_AFTER=""
    if [[ -n $NEAR_BOOT_ID_AFTER && $NEAR_BOOT_ID_AFTER != "$NEAR_BOOT_ID_BEFORE" ]]; then
        NEAR_REBOOTED=1
        mark near_rebooted "boot_id $NEAR_BOOT_ID_BEFORE -> $NEAR_BOOT_ID_AFTER"
    fi
fi

collect_near_evidence || true
write_stats_summary
nc_snapshot || true

# 最后一次 liveness 探测：防止崩溃发生在证据采集之后。
near_probe
if (( PROBE_SSH_RC != 0 )) && (( ! NEAR_LOST )); then
    NEAR_LOST=1
    NEAR_LOST_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    mark near_lost "final probe; ping_rc=$PROBE_PING_RC ssh_rc=$PROBE_SSH_RC"
    nc_snapshot || true
fi
if (( NEAR_REBOOTED )) && [[ -z $NEAR_LOST_AT ]]; then
    NEAR_LOST_AT="${BURST_END_UTC:-unknown}（burst 结束时刻；精确 Oops 见 netconsole）"
fi
mark evidence_done "near_lost=$NEAR_LOST rebooted=$NEAR_REBOOTED"

# --- 7. 收尾判定 ------------------------------------------------------------

if (( NEAR_LOST || NEAR_REBOOTED )); then
    FINAL_RC=$EX_LOST
elif (( BURST_ATTEMPTED == 0 )); then
    FINAL_RC=$EX_JOIN
elif [[ ${BURST_RC:-} == 0 && $BURST_SSH_RC == 0 ]]; then
    FINAL_RC=$EX_OK
else
    FINAL_RC=$EX_RUN
fi
mark end "final_rc=$FINAL_RC"
exit "$FINAL_RC"
