#!/usr/bin/env bash
# prod-smoke-10s.sh - tbtun 生产冒烟（最终验收 256 carriers，测试窗口 <= 10 s）。
#
# 目的
#   在工作站（.80）上通过 ssh 驱动 .65（near）和 45.11.1.62（far），
#   用修复版 tbtun 建立指定 carriers 的隧道，等待两端 c3 up 达到
#   CARRIERS，然后只触发一轮 tbt_burst，最后收集两端证据并清理。
#   默认 CARRIERS=256，MIB=1，SECS=5，PROCS=1。
#
#   这是生产冒烟，不是压测：SECS 是 tbt_burst 的 socket 超时，不是
#   持续压测时长；脚本硬拒绝 SECS>10。join gate 默认 180 s，足够
#   256 条载体按 token bucket max(8, 256/4)=64/s 完成加入（约 4-20 s）。
#
# 运行位置
#   工作站 192.168.123.80（本地 netconsole 接收端、.65 的 kmsg-relay /
#   sampler 可见性都在这一侧检查）。
#
# 参数（环境变量）
#   CARRIERS       默认 256；允许 8/16/32/64/128/256。
#   MIB            默认 1；tbt_burst 每个方向发送/校验的 MiB。
#   SECS           默认 5；tbt_burst 的 socket 超时，硬上限 10。
#                  脚本拒绝 SECS>10（用法错误，退出 2）。
#   PROCS          默认 1；同一轮 burst 内并发的 tbt_burst 进程数。
#                  默认 1 时只执行一次 tbt_burst 命令。PROCS>1 时在同一轮
#                  并发启动 PROCS 个客户端，仍然只触发一轮，不重试、不循环。
#   NEAR_HOST      默认 root@192.168.123.65。
#   FAR_HOST       默认 root@45.11.1.62。
#   NEAR_IP        默认 192.168.123.65；用于 ping/ARP 失联探测。
#   NEAR_KO        默认 /root/tbtun-fixed-20261106T231212Z/tbtun.ko。
#   NEAR_SRCVERSION
#                  默认空；为空时从 $NEAR_KO 用 modinfo -F srcversion 读取，
#                  并与加载后的 /sys/module/tbtun/srcversion 比较。
#   FAR_SRCVERSION 默认空；为空时从 $FAR_STAGE/tbtun.ko 读取 srcversion，
#                  并与 far 加载后的值比较。
#   NEAR_SHA256    默认空；为空只校验 srcversion；非空时同时校验
#                  $NEAR_KO 的 sha256。
#   FAR_KNOWN_BAD_SRCVERSIONS
#                  默认 846D47A623A8198946CBA64；far loaded 或 staged ko
#                  命中该列表即拒绝。
#   FAR_STAGE      默认 /home/tbtun-t52-arm64；far 上 tbt_echo 与日志目录。
#   JOIN_TIMEOUT   默认 180 秒；两端 c3 up=CARRIERS 的等待上限。
#   NEAR_RETURN_WAIT
#                  默认 240 秒；burst 后 near 失联时等待恢复的时间。
#   LOCK_WAIT      默认 120 秒；两端 flock 等待时间。
#   LOCK_HOLD_SECS 默认 1200 秒；lock holder 的安全超时。
#   LOAD_LIMIT     默认 4；load1 超过只记录警告，不阻塞（本脚本的硬闸门
#                  是锁、配置、join 和单次 burst）。
#   EVIDENCE_DIR   证据根目录；默认 <repo>/review/wan-experiment/arm64。
#
# 配置形状（脚本按 CARRIERS 计算；groups 是 FEC 派生量）
#   carriers   n   m   groups   near portblock   far portblock
#   8          4   4   1        30000-30015      31000-31015
#   16         8   8   1        30000-30031      31000-31031
#   32        16  16   1        30000-30063      31000-31063
#   64         4   4   8        30000-30127      31000-31127
#   128        4   4  16        30000-30255      31000-31255
#   256        4   4  32        30000-30511      31000-31511
#
#   256 的验收形状：n=4 m=4，groups=32，w=8，fec=on mode=grouped。
#   注意：内核 config 解析器没有 groups= 键（见 tbtun/tbt_parse.c）；
#   groups 由 carriers/(n+m) 派生，只用于校验和报告，不能写进 config，
#   否则内核会以 unknown key 拒绝。config 里实际写的是
#   n=4 m=4 fec=on mode=grouped w=8 carriers=256。
#
#   两端模块都用 max_carriers=256 加载（far 已加载；near 由本脚本 insmod）。
#   near 未加载时无法读回 max_carriers，所以 near 的 256 读回放在 insmod
#   之后做硬闸门；far 的 256 读回在预检阶段做硬闸门。
#
# 单次约束
#   BURST_ATTEMPTED 是硬闸门：任何路径都不可能触发第二轮。join 循环只
#   重读 stats，不重跑 burst；burst 失败、超时、near 失联都不重试。
#   burst 外层超时为 SECS*3+120；远端每个 tbt_burst 进程另有同一超时。
#
# 证据目录
#   review/wan-experiment/arm64/prod-smoke-<UTC>/
#     SUMMARY.md              中文报告：判定、配置、步骤、计数器、清理
#     timeline.txt            带 UTC 时间戳的阶段时间线
#     run.log                 脚本自身运行日志
#     netconsole.log          本地接收端完整日志快照
#     netconsole-window.log   从本次布防行号基线之后开始的窗口
#     netconsole-crash-lines.txt
#     join-probes.txt         两端 c3 up / c3 n 的逐次轮询轨迹
#     counters-joined.txt     join 成功时的关键计数器
#     counters-post.txt       burst 后的关键计数器
#     c9-near-*.txt / c9-far-*.txt
#                             c9 行与错误/丢弃计数全零检查
#     near-pstore.txt         /sys/fs/pstore 与 systemd-pstore 归档
#     near-journal-b0.log     当前 boot 的 journal
#     near-journal-b1.log     上一 boot 的 journal（重启后取）
#     near-journal.log        按是否重启选出的主 journal
#     near-dmesg.log
#     near-stats.txt          config/ports/tunnels/peers/stats
#     near-tbtun-last.log     sampler 的 250ms 状态记录
#     near-tbtun-last-old.log sampler 轮转后的上一份（若存在）
#     near-ss.txt / near-meta.txt / near-probes.txt
#     far-dmesg.log / far-journal.log / far-stats.txt / far-ss.txt
#     pre-near.txt / pre-far.txt / joined-*.txt / post-*.txt
#     burst.txt               唯一一轮 burst 的原始输出
#     burst-<i>.log / burst-<i>.rc
#     burst-build.log
#     module.txt              两端模块身份与 max_carriers
#     cleanup.txt             清理结果
#
# 退出码
#   0   全流程完成，near 存活，唯一一轮 burst 全部 rc=0
#   2   用法错误（CARRIERS/MIB/SECS/PROCS 非法，或 SECS>10）
#   3   前置检查失败（ssh、near 未加载、far 身份、max_carriers、可见性、
#       pstore、模块身份、工具）
#   4   锁忙
#   5   join gate 失败（未触发 burst）
#   6   near 失联或重启（证据已收集，清理已跳过）
#   7   burst 返回非 0，但 near 存活
#   8   关键证据缺失
#   9   清理不完整
#   10  设置失败（far/near 配置、insmod、ports、config、burst 构建）
#
# 步骤、预期结果与失败分支
#   1 参数校验   预期：CARRIERS/MIB/SECS/PROCS 合法，SECS<=10。
#                失败：退出 2，不创建证据目录。
#   2 前置检查   预期：两端 ssh、near 未加载、far 已加载且 srcversion 匹配、
#                far max_carriers=256、可见性服务 active、pstore 空、
#                near 模块身份匹配、far tbt_echo 存在、near tbt_burst 可用。
#                失败：退出 3，不加载模块、不写配置。
#   3 锁         预期：两端 flock 成功。
#                失败：退出 4，不跑。
#   4 far 配置   预期：ports 31000-31511、far config 逐字读回，
#                carriers=256、n=4、m=4、w=8、fec=on、mode=grouped、
#                profile=http-kal、9000/5202 监听。
#                失败：退出 10，near 不加载，far 清理。
#   5 near 配置  预期：ip rule、rmem/wmem、insmod max_carriers=256、
#                加载后 max_carriers=256、srcversion 匹配、ports
#                30000-30511、near config 逐字读回且 carriers=256。
#                失败：退出 10，清理。
#   6 join gate  预期：180 秒内两端 c3 up=256；同时记录 c3 n、c9 错误计数、
#                http_parse_err、rx_errs、reorder_overflow，非零只告警。
#                失败：退出 5，不触发 burst，收集证据。
#   7 触发       预期：恰好一轮 tbt_burst 127.0.0.1 9001 1 5 interleaved，
#                SECS*3+120 秒内返回。
#                失败：记录 rc，不重试；near 失联则转证据收集。
#   8 失联判定   预期：记录 ping/ARP/ssh、boot_id、是否重启。
#                失败：记 unknown，继续 far 与 netconsole 证据。
#   9 证据收集   预期：netconsole、pstore、journal、dmesg、stats、
#                far dmesg/stats、join 轨迹、c9 检查全部落盘。
#                失败：单项记录原因，不阻塞其他项。
#   10 清理      预期：near rmmod/rule/sysctl、far echo/rmmod、锁释放。
#                失联时跳过模块/网络清理并保留现场；锁仍然释放。
#
# 注意
#   - 本脚本不重试、不降级、不换工具。
#   - far 的当前 config 可能还是旧 carriers；脚本会在步骤 4 重写为
#     CARRIERS。预检只记录 far 当前 config 的 carriers，不把它当作
#     前置条件；真正的 256 闸门是步骤 4 的逐字读回。
#   - near 的 max_carriers 只能在 insmod 后读回；预检记录计划值，
#     步骤 5 对加载后的 /sys/module/tbtun/parameters/max_carriers 做
#     硬闸门。
#   - near/far 的期望 srcversion 默认从各自 ko 读取；显式设置则作为期望
#     值。far staged ko 命中 FAR_KNOWN_BAD_SRCVERSIONS 时拒绝，避免下一次
#     redeploy 回退到已知事故构建。NEAR_SHA256 为空只校验 srcversion；
#     非空时同时校验 near ko 的 sha256。
#   - .65 失联时所有 ssh 调用都带 timeout；清理跳过，证据优先。
#   - netconsole 是尽力而为；panic 的权威证据是 pstore。

set -euo pipefail

PROG=${0##*/}

usage() {
    local rc=${1:-2}
    if (( rc == 0 )); then
        cat <<'EOF'
用法: prod-smoke-10s.sh

环境变量:
  CARRIERS=256      允许 8/16/32/64/128/256
  MIB=1             tbt_burst 每个方向的 MiB
  SECS=5            tbt_burst socket 超时；硬上限 10
  PROCS=1           同一轮并发 tbt_burst 进程数；只触发一轮
  NEAR_HOST=root@192.168.123.65
  FAR_HOST=root@45.11.1.62
  NEAR_KO=/root/tbtun-fixed-20261106T231212Z/tbtun.ko
  NEAR_SRCVERSION=      空则从 NEAR_KO 读取
  FAR_SRCVERSION=       空则从 FAR_STAGE/tbtun.ko 读取
  NEAR_SHA256=          空则只校验 srcversion
  FAR_KNOWN_BAD_SRCVERSIONS=846D47A623A8198946CBA64
  FAR_STAGE=/home/tbtun-t52-arm64
  JOIN_TIMEOUT=180  NEAR_RETURN_WAIT=240  EVIDENCE_DIR=<repo>/review/wan-experiment/arm64

示例:
  CARRIERS=256 MIB=1 SECS=5 PROCS=1 review/wan-experiment/arm64/prod-smoke-10s.sh
EOF
    else
        cat >&2 <<'EOF'
用法: prod-smoke-10s.sh

环境变量:
  CARRIERS=256      允许 8/16/32/64/128/256
  MIB=1             tbt_burst 每个方向的 MiB
  SECS=5            tbt_burst socket 超时；硬上限 10
  PROCS=1           同一轮并发 tbt_burst 进程数；只触发一轮
  NEAR_HOST=root@192.168.123.65
  FAR_HOST=root@45.11.1.62
  NEAR_KO=/root/tbtun-fixed-20261106T231212Z/tbtun.ko
  NEAR_SRCVERSION=      空则从 NEAR_KO 读取
  FAR_SRCVERSION=       空则从 FAR_STAGE/tbtun.ko 读取
  NEAR_SHA256=          空则只校验 srcversion
  FAR_KNOWN_BAD_SRCVERSIONS=846D47A623A8198946CBA64
  FAR_STAGE=/home/tbtun-t52-arm64
  JOIN_TIMEOUT=180  NEAR_RETURN_WAIT=240  EVIDENCE_DIR=<repo>/review/wan-experiment/arm64

示例:
  CARRIERS=256 MIB=1 SECS=5 PROCS=1 review/wan-experiment/arm64/prod-smoke-10s.sh
EOF
    fi
    exit "$rc"
}

case ${1:-} in
    -h | --help) usage 0 ;;
esac
[[ $# -eq 0 ]] || usage 2

# ---------------------------------------------------------------------------
# 参数与派生配置。

CARRIERS=${CARRIERS:-256}
MIB=${MIB:-1}
SECS=${SECS:-5}
PROCS=${PROCS:-1}
NEAR_HOST=${NEAR_HOST:-root@192.168.123.65}
FAR_HOST=${FAR_HOST:-root@45.11.1.62}
NEAR_IP=${NEAR_IP:-192.168.123.65}
NEAR_KO=${NEAR_KO:-/root/tbtun-fixed-20261106T231212Z/tbtun.ko}
NEAR_SRCVERSION=${NEAR_SRCVERSION:-}
FAR_SRCVERSION=${FAR_SRCVERSION:-}
NEAR_SHA256=${NEAR_SHA256:-}
FAR_KNOWN_BAD_SRCVERSIONS=${FAR_KNOWN_BAD_SRCVERSIONS:-846D47A623A8198946CBA64}
FAR_STAGE=${FAR_STAGE:-/home/tbtun-t52-arm64}
JOIN_TIMEOUT=${JOIN_TIMEOUT:-180}
NEAR_RETURN_WAIT=${NEAR_RETURN_WAIT:-240}
LOCK_WAIT=${LOCK_WAIT:-120}
LOCK_HOLD_SECS=${LOCK_HOLD_SECS:-1200}
LOAD_LIMIT=${LOAD_LIMIT:-4}
EVIDENCE_ROOT=${EVIDENCE_DIR:-}

case $CARRIERS in
    8) N=4; M=4 ;;
    16) N=8; M=8 ;;
    32) N=16; M=16 ;;
    64 | 128 | 256) N=4; M=4 ;;
    *) usage 2 ;;
esac
# 不要用 GROUPS：bash 把它当作用户组 ID 的特殊数组，赋值会被忽略/覆盖。
FEC_GROUPS=$((CARRIERS / (N + M)))
W=8

[[ $MIB =~ ^[1-9][0-9]*$ ]] || usage 2
[[ $SECS =~ ^[1-9][0-9]*$ ]] || usage 2
(( SECS <= 10 )) || usage 2
[[ $PROCS =~ ^[1-9][0-9]*$ ]] || usage 2
(( PROCS <= 32 )) || usage 2
(( FEC_GROUPS > 0 )) && (( FEC_GROUPS * (N + M) == CARRIERS )) || usage 2
[[ $NEAR_KO == /* ]] || usage 2

NEAR_PORT=9001
FAR_PORT=9000
ECHO_PORT=5202
NEAR_PORTBASE=30000
FAR_PORTBASE=31000
NEAR_PORTBLOCK="$NEAR_PORTBASE-$((NEAR_PORTBASE + 2 * CARRIERS - 1))"
FAR_PORTBLOCK="$FAR_PORTBASE-$((FAR_PORTBASE + 2 * CARRIERS - 1))"
NEAR_DIR=$(dirname "$NEAR_KO")
SYSCTL_MAX=67108864
INSMOD_PARAMS='max_carriers=256 max_tunnels=1 debug=0'
BURST_TIMEOUT=$((SECS * 3 + 120))
NONCE=00112233445566778899aabbccddeeff

NEAR_CFG="tunnel 1 role=near listen=127.0.0.1:$NEAR_PORT peer=45.11.1.62:$FAR_PORT n=$N m=$M fec=on mode=grouped w=$W carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=$NEAR_PORTBLOCK nonce=$NONCE"
FAR_CFG="tunnel 1 role=far listen=0.0.0.0:$FAR_PORT target=127.0.0.1:$ECHO_PORT n=$N m=$M fec=on mode=grouped w=$W carriers=$CARRIERS crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=$FAR_PORTBLOCK nonce=$NONCE"

SSH_BASE=(-o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 -o TCPKeepAlive=yes)
SSH_TMO=30

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
LOCKS_HELD=0
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
COUNTER_WARN=0
NUP_LAST="?"
NN_LAST="?"
FUP_LAST="?"
FN_LAST="?"
BURST_ATTEMPTED=0
BURST_SSH_RC=""
BURST_RCS=""
BURST_RC=""
BURST_WALL_MS=""
BURST_END_UTC=""
PRECHECK_DONE=0
FAR_CFG_OK=0
NEAR_CFG_OK=0
EVIDENCE_INCOMPLETE=0
CLEAN_FAIL=0
FINAL_RC=$EX_OK
NEAR_ARCH=""
NEAR_KREL=""
FAR_KREL=""
NEAR_LOAD=""
FAR_LOAD=""
NEAR_KO_SHA=""
NEAR_KO_SRCVERSION=""
NEAR_KO_VERMAGIC=""
NEAR_KO_MTIME=0
NEAR_TREE_COMMIT=""
NEAR_LOADED_SRCVERSION=""
NEAR_SRCVERSION_SOURCE="unset"
if [[ -n $NEAR_SRCVERSION ]]; then NEAR_SRCVERSION_SOURCE="env"; fi
NEAR_SHA256_GATE="unchecked"
NEAR_IDENTITY_MATCH="?"
NEAR_MAX_CARRIERS=""
NEAR_MAX_TUNNELS=""
NEAR_DEBUG=""
FAR_LOADED_SRCVERSION=""
FAR_SRCVERSION_SOURCE="unset"
if [[ -n $FAR_SRCVERSION ]]; then FAR_SRCVERSION_SOURCE="env"; fi
FAR_IDENTITY_MATCH="?"
FAR_MAX_CARRIERS=""
FAR_MAX_TUNNELS=""
FAR_DEBUG=""
FAR_KO_SHA=""
FAR_KO_SRCVERSION=""
FAR_KO_VERMAGIC=""
FAR_ECHO_SHA=""
FAR_PRE_CFG=""
FAR_PRE_CARRIERS="?"
FAR_CFG_CARRIERS="?"
NEAR_CFG_CARRIERS="?"
FAR_GOT_PORTS=""
NEAR_GOT_PORTS=""
FAR_GOT_CFG=""
NEAR_GOT_CFG=""
BURST_SHA=""
PRE_NEAR_DMESG=0
PRE_FAR_DMESG=0
POST_NEAR_DMESG=0
POST_FAR_DMESG=0
PROBE_PING_RC=0
PROBE_SSH_RC=0
NEAR_PANIC_ON_OOPS="?"
NEAR_PANIC="?"
NEAR_EXTRA_SYSCTLS="?"
NEAR_PSTORE_EMPTY="?"
NEAR_RAMOOPS="?"
NEAR_CLEAN_STATE="not-checked"
FAR_MODULE_STATE="not-checked"
FAR_LISTEN_STATE="not-checked"

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
[[ -f $REPO/host/tbt_burst.c ]] || { echo "$PROG: 不是仓库 checkout: $REPO" >&2; exit "$EX_USAGE"; }
EVIDENCE_ROOT=${EVIDENCE_ROOT:-$REPO/review/wan-experiment/arm64}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
EV=$EVIDENCE_ROOT/prod-smoke-$STAMP
if ! mkdir "$EV" 2>/dev/null; then
    EV=$EVIDENCE_ROOT/prod-smoke-$STAMP-$$
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

near_boot_id() {
    timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null
}

load1_on() {
    timeout 12 ssh "${SSH_BASE[@]}" "$1" "cut -d' ' -f1 /proc/loadavg" 2>/dev/null
}

# ---------------------------------------------------------------------------
# config / stats 解析。

cfg_token() {
    local cfg=$1 key=$2 tok
    for tok in $cfg; do
        case $tok in
            "$key="*) printf '%s\n' "${tok#*=}"; return 0 ;;
        esac
    done
    return 1
}

# 快照文件里的 stats 段有两种标题：snapshot() 用 ===stats===，
# near-stats.txt 用 === stats ===。只在这段里找计数器，避免把
# tunnels 段的 n=4 误当成 c3 的 n=256。
stats_text() {
    awk '
        /^=== ?stats ?===$/ { f = 1; next }
        f && /^===/ { exit }
        f { print }' "$1" 2>/dev/null
}

stat_field() {
    local file=$1 field=$2
    stat_field_text "$(stats_text "$file")" "$field"
}

sec_number() {
    awk -v s="===$2===" '
        $0 == s { f = 1; next }
        f && /^===/ { exit }
        f && !done {
            v = $0
            gsub(/[^0-9]/, "", v)
            if (v != "") { print v; done = 1 }
        }' "$1" 2>/dev/null
}

stat_field_text() {
    printf '%s\n' "$1" | awk -v f="$2=" '
        $1 == "tunnel" && $2 == "1" {
            for (i = 1; i <= NF; i++) {
                if (index($i, f) == 1) {
                    print substr($i, length(f) + 1)
                    exit
                }
            }
        }'
}

live_c3() {
    local host=$1 text
    text=$(timeout 12 ssh "${SSH_BASE[@]}" "$host" 'cat /proc/net/tbtun/stats 2>/dev/null' 2>/dev/null) || text=""
    printf '%s\n' "$text" | awk '
        $1 == "tunnel" && $2 == "1" && $3 == "c3" {
            for (i = 1; i <= NF; i++) {
                if (index($i, "up=") == 1) u = substr($i, 4)
                if (index($i, "n=") == 1) n = substr($i, 3)
            }
            print (u == "" ? "?" : u) " " (n == "" ? "?" : n)
            found = 1
            exit
        }
        END { if (!found) print "? ?" }'
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

# c9 全零检查：只覆盖错误/丢弃计数。fec_ack_tx / fec_ack_rx 是流量计数，
# fec_credit_wait 是等待计数，单独记录，不参与“全零”判定。
c9_report() {
    local file=$1 out=$2 line tok key val nonzero="" found=0
    line=$(awk '$1 == "tunnel" && $2 == "1" && $3 == "c9" { print; exit }' "$file" 2>/dev/null) || line=""
    {
        printf 'c9_line=%s\n' "$line"
        if [[ -z $line ]]; then
            printf 'c9_present=no\n'
            printf 'c9_all_error_zero=unknown\n'
            printf 'c9_error_nonzero=unknown\n'
            return 0
        fi
        printf 'c9_present=yes\n'
        for tok in $line; do
            case $tok in
                *=*) ;;
                *) continue ;;
            esac
            key=${tok%%=*}
            val=${tok#*=}
            case $key in
                fec_proto_fail | fec_hdr_crc_fail | fec_payload_crc_fail | \
                    fec_short_tx | fec_short_unknown | fec_stream_refused | \
                    fec_drop_window | fec_drop_crypto)
                    found=1
                    if [[ $val != 0 ]]; then
                        nonzero+="$key=$val "
                    fi
                    ;;
                fec_ack_tx | fec_ack_rx | fec_credit_wait)
                    printf 'c9_info.%s=%s\n' "$key" "$val"
                    ;;
            esac
        done
        if (( ! found )); then
            printf 'c9_all_error_zero=unknown\n'
            printf 'c9_error_nonzero=unknown\n'
        elif [[ -n $nonzero ]]; then
            printf 'c9_all_error_zero=no\n'
            printf 'c9_error_nonzero=%s\n' "$nonzero"
        else
            printf 'c9_all_error_zero=yes\n'
            printf 'c9_error_nonzero=none\n'
        fi
    } >"$out"
}

write_counters() {
    local tag=$1 near_file=$2 far_file=$3 out=$4 end file
    {
        for end in near far; do
            if [[ $end == near ]]; then file=$near_file; else file=$far_file; fi
            printf '=== %s ===\n' "$end"
            if [[ ! -f $file ]]; then
                printf '%s=unavailable\n' "$end"
                continue
            fi
            printf '%s.c3.up=%s\n' "$end" "$(stat_field "$file" up)"
            printf '%s.c3.n=%s\n' "$end" "$(stat_field "$file" n)"
            printf '%s.c3.data_tx=%s\n' "$end" "$(stat_field "$file" data_tx)"
            printf '%s.c3.rx_ack=%s\n' "$end" "$(stat_field "$file" rx_ack)"
            printf '%s.c3.reorder_overflow=%s\n' "$end" "$(stat_field "$file" reorder_overflow)"
            printf '%s.c8.http_parse_err=%s\n' "$end" "$(stat_field "$file" http_parse_err)"
            printf '%s.rx_errs=%s\n' "$end" "$(stat_field "$file" rx_errs)"
            c9_report "$file" "$EV/c9-$end-$tag.txt"
            grep '^c9_all_error_zero=' "$EV/c9-$end-$tag.txt" 2>/dev/null || true
            grep '^c9_error_nonzero=' "$EV/c9-$end-$tag.txt" 2>/dev/null || true
            grep '^c9_info\.' "$EV/c9-$end-$tag.txt" 2>/dev/null || true
        done
    } >"$out"
}

counter_warn_from() {
    local file=$1 tag=$2 end=$3 val
    if [[ ! -f $file ]]; then
        return 0
    fi
    val=$(stat_field "$file" up)
    if [[ -z $val ]]; then
        note "counter: $end c3 up unavailable"
        COUNTER_WARN=1
    elif [[ $val != "$CARRIERS" ]]; then
        note "counter: $end c3 up=$val != $CARRIERS"
        COUNTER_WARN=1
    fi
    val=$(stat_field "$file" n)
    if [[ -z $val ]]; then
        note "counter: $end c3 n unavailable"
        COUNTER_WARN=1
    elif [[ $val != "$CARRIERS" ]]; then
        note "counter: $end c3 n=$val != $CARRIERS"
        COUNTER_WARN=1
    fi
    val=$(stat_field "$file" http_parse_err)
    if [[ -z $val ]]; then
        note "counter: $end http_parse_err unavailable"
        COUNTER_WARN=1
    elif [[ $val != 0 ]]; then
        note "counter: $end http_parse_err=$val"
        COUNTER_WARN=1
    fi
    val=$(stat_field "$file" rx_errs)
    if [[ -z $val ]]; then
        note "counter: $end rx_errs unavailable"
        COUNTER_WARN=1
    elif [[ $val != 0 ]]; then
        note "counter: $end rx_errs=$val"
        COUNTER_WARN=1
    fi
    val=$(stat_field "$file" reorder_overflow)
    if [[ -z $val ]]; then
        note "counter: $end reorder_overflow unavailable"
        COUNTER_WARN=1
    elif [[ $val != 0 ]]; then
        note "counter: $end reorder_overflow=$val"
        COUNTER_WARN=1
    fi
    c9_report "$file" "$EV/c9-$end-$tag.txt"
    if grep -q '^c9_all_error_zero=no' "$EV/c9-$end-$tag.txt" 2>/dev/null; then
        note "counter: $end c9 错误计数非零: $(grep '^c9_error_nonzero=' "$EV/c9-$end-$tag.txt" | head -n 1)"
        COUNTER_WARN=1
    elif ! grep -q '^c9_all_error_zero=yes' "$EV/c9-$end-$tag.txt" 2>/dev/null; then
        note "counter: $end c9 检查不可用（c9 行缺失）"
        COUNTER_WARN=1
    fi
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
run max_carriers sh -c 'cat /sys/module/tbtun/parameters/max_carriers 2>&1 || true'
run pstore sh -c 'ls -la /sys/fs/pstore 2>&1 || true'
EOS
}

# ---------------------------------------------------------------------------
# 远端 flock。持有者是后台 ssh + sleep；脚本退出时先从锁文件读出远端
# holder pid 并 kill，再 kill 本地 ssh。只杀本地 ssh 时远端 bash 可能继续
# 持有 flock(9)，这是之前实验前遇到的锁泄漏。

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
    line=$(ss -ulnp 2>/dev/null | grep -E "(:6666[[:space:]])" | head -n 1) || true
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
    local marker="tbt-prod-smoke-probe-$(date +%s)-$RANDOM" i
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
    else
        RX_SERVICE_STATE="inactive"
        note "visibility: tbtun-netconsole-rx.service 不是 active"
        return 1
    fi
    note "visibility: relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE sampler_lines=$SAMPLER_BASELINE_LINES"
    return 0
}

# far 身份闸门：期望 srcversion 默认从 far staged ko 读取；显式
# FAR_SRCVERSION 优先。srcversion 或 max_carriers 不匹配时必须在任何
# near 写入之前停止；loaded/staged 命中已知事故构建列表也拒绝。
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

    # 期望值来源：显式 FAR_SRCVERSION 优先；为空时从 far staged ko 读取。
    # staged ko 同时用于已知事故构建拒绝门，避免下一次 redeploy 回退。
    FAR_KO_SRCVERSION=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        "modinfo -F srcversion '$FAR_STAGE/tbtun.ko' 2>/dev/null" 2>/dev/null) || FAR_KO_SRCVERSION=""
    if [[ -n $FAR_SRCVERSION ]]; then
        FAR_SRCVERSION_SOURCE="env"
        if [[ -n $FAR_KO_SRCVERSION && $FAR_KO_SRCVERSION != "$FAR_SRCVERSION" ]]; then
            note "warn: far staged ko srcversion=$FAR_KO_SRCVERSION != 显式 FAR_SRCVERSION=$FAR_SRCVERSION（以显式值为准）"
        fi
    else
        [[ -n $FAR_KO_SRCVERSION ]] || {
            note "far identity: FAR_SRCVERSION 为空且读不到 $FAR_STAGE/tbtun.ko 的 srcversion"
            return 1
        }
        FAR_SRCVERSION=$FAR_KO_SRCVERSION
        FAR_SRCVERSION_SOURCE="auto-ko"
    fi

    case " $FAR_KNOWN_BAD_SRCVERSIONS " in
        *" $FAR_LOADED_SRCVERSION "*)
            note "far identity: 已加载 srcversion=$FAR_LOADED_SRCVERSION 是已知事故构建；必须部署修复版"
            return 1
            ;;
    esac
    if [[ -n $FAR_KO_SRCVERSION ]]; then
        case " $FAR_KNOWN_BAD_SRCVERSIONS " in
            *" $FAR_KO_SRCVERSION "*)
                note "far identity: staged ko srcversion=$FAR_KO_SRCVERSION 是已知事故构建；下一次 redeploy 会回退"
                return 1
                ;;
        esac
    fi
    [[ $FAR_LOADED_SRCVERSION == "$FAR_SRCVERSION" ]] || {
        note "far identity: 已加载 srcversion=$FAR_LOADED_SRCVERSION != expected=$FAR_SRCVERSION（source=$FAR_SRCVERSION_SOURCE）"
        return 1
    }
    FAR_MAX_CARRIERS=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'cat /sys/module/tbtun/parameters/max_carriers' 2>/dev/null) || FAR_MAX_CARRIERS=""
    [[ $FAR_MAX_CARRIERS == 256 ]] || {
        note "far identity: max_carriers=${FAR_MAX_CARRIERS:-unknown} != 256；先 redeploy"
        return 1
    }
    FAR_MAX_TUNNELS=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'cat /sys/module/tbtun/parameters/max_tunnels' 2>/dev/null) || FAR_MAX_TUNNELS=""
    FAR_DEBUG=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        'cat /sys/module/tbtun/parameters/debug' 2>/dev/null) || FAR_DEBUG=""
    FAR_IDENTITY_MATCH="yes"
    return 0
}

# ---------------------------------------------------------------------------
# far 的 echo 目标。

far_stop_echo() {
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl stop tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "systemctl reset-failed tbt-echo-$ECHO_PORT.service" >/dev/null 2>&1 || true
}

far_restart_echo() {
    local i
    far_stop_echo
    timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" \
        "systemd-run --no-block --unit=tbt-echo-$ECHO_PORT --collect \
         --property=StandardOutput=file:$FAR_STAGE/tbt-echo-$ECHO_PORT.log \
         --property=StandardError=file:$FAR_STAGE/tbt-echo-$ECHO_PORT.log \
         '$FAR_STAGE/tbt_echo' $ECHO_PORT echo" >/dev/null 2>&1 || return 1
    for i in $(seq 1 20); do
        if listening_on "$FAR_HOST" "$ECHO_PORT"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# ---------------------------------------------------------------------------
# 模块身份。

record_near_module_identity() {
    NEAR_KO_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "sha256sum '$NEAR_KO'" 2>/dev/null | awk '{print $1}') || NEAR_KO_SHA=""
    NEAR_KO_SRCVERSION=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "modinfo -F srcversion '$NEAR_KO' 2>/dev/null" 2>/dev/null) || NEAR_KO_SRCVERSION=""
    NEAR_KO_VERMAGIC=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "modinfo -F vermagic '$NEAR_KO' 2>/dev/null" 2>/dev/null) || NEAR_KO_VERMAGIC=""
    NEAR_KO_MTIME=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "stat -c %Y '$NEAR_KO' 2>/dev/null" 2>/dev/null) || NEAR_KO_MTIME=0
    NEAR_TREE_COMMIT=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "git -C '$NEAR_DIR' rev-parse HEAD 2>/dev/null" 2>/dev/null) || NEAR_TREE_COMMIT=""
}

near_sha256_gate() {
    if [[ -z $NEAR_SHA256 ]]; then
        NEAR_SHA256_GATE="not-set"
        return 0
    fi
    if [[ $NEAR_KO_SHA == "$NEAR_SHA256" ]]; then
        NEAR_SHA256_GATE="pass"
        return 0
    fi
    NEAR_SHA256_GATE="fail"
    note "near identity: ko sha256=${NEAR_KO_SHA:-unknown} != expected=$NEAR_SHA256"
    return 1
}

# ---------------------------------------------------------------------------
# 失联探测。

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
        'echo "=== meta ==="; date -u; uptime; cat /proc/sys/kernel/random/boot_id; lsmod | grep -E "^tbtun" || true; cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null || true' \
        >"$EV/far-meta.txt" 2>&1 || true
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
        'echo "=== date ==="; date -u; echo "=== uptime ==="; uptime; echo "=== boot_id ==="; cat /proc/sys/kernel/random/boot_id; echo "=== panic ==="; cat /proc/sys/kernel/panic_on_oops /proc/sys/kernel/panic /proc/sys/kernel/oops_limit /proc/sys/kernel/panic_on_rcu_stall /proc/sys/kernel/oops_all_cpu_backtrace 2>/dev/null; echo "=== cmdline ==="; cat /proc/cmdline; echo "=== max_carriers ==="; cat /sys/module/tbtun/parameters/max_carriers 2>/dev/null; echo "=== boots ==="; journalctl --list-boots --no-pager 2>&1 | tail -n 6' \
        >"$EV/near-meta.txt" 2>&1 || true
    return 0
}

write_stats_summary() {
    POST_NEAR_DMESG=$(sec_number "$EV/post-near.txt" dmesg_count)
    POST_FAR_DMESG=$(sec_number "$EV/post-far.txt" dmesg_count)
    POST_NEAR_DMESG=${POST_NEAR_DMESG:-0}
    POST_FAR_DMESG=${POST_FAR_DMESG:-0}
    write_counters post "$EV/post-near.txt" "$EV/post-far.txt" "$EV/counters-post.txt"
    counter_warn_from "$EV/post-near.txt" post near
    counter_warn_from "$EV/post-far.txt" post far
    {
        printf 'join_ok=%s near_up=%s near_n=%s far_up=%s far_n=%s counter_warn=%s\n' \
            "$JOIN_OK" "$NUP_LAST" "$NN_LAST" "$FUP_LAST" "$FN_LAST" "$COUNTER_WARN"
        printf 'burst_attempted=%s burst_ssh_rc=%s burst_rc=%s wall_ms=%s\n' \
            "$BURST_ATTEMPTED" "${BURST_SSH_RC:-未执行}" "${BURST_RC:-未执行}" "${BURST_WALL_MS:-?}"
        printf 'near_lost=%s near_rebooted=%s\n' "$NEAR_LOST" "$NEAR_REBOOTED"
        printf 'dmesg_delta_near=%s dmesg_delta_far=%s\n' \
            "$((POST_NEAR_DMESG - PRE_NEAR_DMESG))" "$((POST_FAR_DMESG - PRE_FAR_DMESG))"
        printf '\n=== counters-post ===\n'
        cat "$EV/counters-post.txt" 2>/dev/null || true
    } >"$EV/stats.txt" 2>&1 || true
}

# ---------------------------------------------------------------------------
# SUMMARY.md。cleanup 在最后调用它，所以清理结果也在里面。

write_summary() {
    local rc=$1
    local oops_nc=0 panic_nc=0 oops_jr=0 oops_ps=0 oops_sm=0
    local verdict join_actual burst_actual lost_actual clean_actual
    local near_state far_state evidence_state c9_near c9_far

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
    elif (( NEAR_CFG_OK == 0 || FAR_CFG_OK == 0 )); then
        verdict="设置阶段中止（far/near 配置未完成），未触发 burst；见步骤表与 run.log。"
    elif (( JOIN_OK == 0 )); then
        verdict="join gate 失败，未触发 burst；未复现崩溃。"
    elif (( BURST_ATTEMPTED == 0 )); then
        verdict="触发前中止，未跑 burst；见 run.log。"
    elif (( oops_nc + panic_nc + oops_jr + oops_ps + oops_sm > 0 )); then
        verdict="证据记录到 Oops/panic，但 near 当前可达且未检测到重启；需人工核对，不能判定通过。"
    elif [[ ${BURST_RC:-} == 0 && ${BURST_SSH_RC:-} == 0 ]]; then
        if (( COUNTER_WARN )); then
            verdict="burst 通过，near 存活；但计数器有非零项或不可用（c9 错误计数、http_parse_err、rx_errs 或 reorder_overflow），需人工复核。"
        else
            verdict="通过：${CARRIERS} carriers join 成功，唯一一轮 ${MIB} MiB / ${SECS}s burst 通过，near 存活，关键计数器为零。"
        fi
    elif [[ ${BURST_SSH_RC:-} != 0 ]]; then
        verdict="join 成功，burst ssh_rc=${BURST_SSH_RC}（burst rc=${BURST_RC:-unknown}）；结果未知或中断，见 burst.txt。"
    elif [[ -n ${BURST_RC:-} ]]; then
        verdict="join 成功，burst rc=${BURST_RC}，near 存活；未崩溃但 burst 未通过。"
    else
        verdict="join 成功，burst 结果未知（near 存活或 ssh 中断）；见 burst.txt。"
    fi

    join_actual="JOIN_OK=${JOIN_OK} near_up=${NUP_LAST:-?} near_n=${NN_LAST:-?} far_up=${FUP_LAST:-?} far_n=${FN_LAST:-?} counter_warn=${COUNTER_WARN}"
    burst_actual="attempted=${BURST_ATTEMPTED} procs=${PROCS} ssh_rc=${BURST_SSH_RC:-未执行} burst_rc=${BURST_RC:-未执行} wall_ms=${BURST_WALL_MS:-?}"
    lost_actual="near_lost=${NEAR_LOST} lost_at=${NEAR_LOST_AT:-none} returned_at=${NEAR_RETURNED_AT:-none} rebooted=${NEAR_REBOOTED}"
    clean_actual="clean_fail=${CLEAN_FAIL} skipped=$([[ $NEAR_LOST == 1 || $NEAR_REBOOTED == 1 ]] && echo yes || echo no)"
    near_state=$(if near_ssh_ok; then echo reachable; else echo unreachable; fi)
    far_state=$(if timeout 10 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1; then echo reachable; else echo unreachable; fi)
    evidence_state=$(if (( EVIDENCE_INCOMPLETE )); then echo incomplete; else echo complete; fi)
    c9_near=$(grep '^c9_all_error_zero=' "$EV/c9-near-post.txt" 2>/dev/null | tail -n 1 || true)
    c9_far=$(grep '^c9_all_error_zero=' "$EV/c9-far-post.txt" 2>/dev/null | tail -n 1 || true)

    {
        printf '# prod-smoke-10s 生产冒烟报告\n\n'
        printf -- '- 实验 UTC：%s\n' "$STAMP"
        printf -- '- 退出码：%s\n' "$rc"
        printf -- '- 判定：%s\n' "$verdict"
        printf -- '- near：%s；far：%s；证据：%s\n\n' "$near_state" "$far_state" "$evidence_state"

        printf '## 配置\n\n'
        printf '```\n%s\n%s\n```\n\n' "$NEAR_CFG" "$FAR_CFG"
        printf -- '- carriers=%s，n=%s，m=%s，groups=%s（派生量，不写入 config），w=%s，fec=on mode=grouped\n' \
            "$CARRIERS" "$N" "$M" "$FEC_GROUPS" "$W"
        printf -- '- near 端口块：%s；far 端口块：%s\n' "$NEAR_PORTBLOCK" "$FAR_PORTBLOCK"
        printf -- '- MIB=%s；SECS=%s（硬上限 10）；PROCS=%s；join 超时 %ss；burst 超时 %ss\n' \
            "$MIB" "$SECS" "$PROCS" "$JOIN_TIMEOUT" "$BURST_TIMEOUT"
        printf -- '- 触发：恰好一轮 tbt_burst 127.0.0.1 %s %s %s interleaved（不重试）\n' \
            "$NEAR_PORT" "$MIB" "$SECS"
        if (( CARRIERS == 256 )); then
            printf -- '- 256 验收形状：n=4 m=4，groups=32，w=8，fec=on mode=grouped；内核 config 无 groups= 键。\n\n'
        else
            printf -- '- 本次形状：n=%s m=%s，groups=%s，w=%s，fec=on mode=grouped；内核 config 无 groups= 键。\n\n' \
                "$N" "$M" "$FEC_GROUPS" "$W"
        fi

        printf '## 模块身份与 max_carriers\n\n'
        printf '| 项 | near | far |\n|---|---|---|\n'
        printf '| staged tree | `%s` | `%s` |\n' "$NEAR_DIR" "$FAR_STAGE"
        printf '| tree commit | `%s` | — |\n' "${NEAR_TREE_COMMIT:-unknown}"
        printf '| ko sha256 | `%s` | `%s` |\n' "${NEAR_KO_SHA:-unknown}" "${FAR_KO_SHA:-unknown}"
        printf '| ko srcversion | `%s` | `%s` |\n' "${NEAR_KO_SRCVERSION:-unknown}" "${FAR_KO_SRCVERSION:-unknown}"
        printf '| loaded srcversion | `%s` | `%s` |\n' "${NEAR_LOADED_SRCVERSION:-unknown}" "${FAR_LOADED_SRCVERSION:-unknown}"
        printf '| expected srcversion | `%s`（source=%s） | `%s`（source=%s） |\n' \
            "$NEAR_SRCVERSION" "$NEAR_SRCVERSION_SOURCE" "$FAR_SRCVERSION" "$FAR_SRCVERSION_SOURCE"
        printf '| identity match | `%s` | `%s` |\n' "$NEAR_IDENTITY_MATCH" "$FAR_IDENTITY_MATCH"
        printf '| expected sha256 | `%s` | — |\n' "${NEAR_SHA256:-not-set}"
        printf '| sha256 gate | `%s` | — |\n' "$NEAR_SHA256_GATE"
        printf '| far known bad srcversions | — | `%s` |\n' "$FAR_KNOWN_BAD_SRCVERSIONS"
        printf '| ko vermagic | `%s` | `%s` |\n' "${NEAR_KO_VERMAGIC:-unknown}" "${FAR_KO_VERMAGIC:-unknown}"
        printf '| loaded max_carriers | `%s`（insmod 后） | `%s`（预检） |\n' "${NEAR_MAX_CARRIERS:-unknown}" "${FAR_MAX_CARRIERS:-unknown}"
        printf '| max_tunnels / debug | `%s` / `%s` | `%s` / `%s` |\n' \
            "${NEAR_MAX_TUNNELS:-unknown}" "${NEAR_DEBUG:-unknown}" "${FAR_MAX_TUNNELS:-unknown}" "${FAR_DEBUG:-unknown}"
        printf '| insmod | `%s` | 已加载（far 不重新 insmod） |\n\n' "$INSMOD_PARAMS"

        printf '## 前置检查、可见性与配置读回闸门\n\n'
        printf '| 项 | 结果 |\n|---|---|\n'
        printf '| 两端 ssh | %s |\n' "$([[ $PRECHECK_DONE == 1 ]] && echo pass || echo fail)"
        printf '| near 未加载 | %s |\n' "$NEAR_CLEAN_STATE"
        printf '| near /sys/fs/pstore 为空 | %s |\n' "$NEAR_PSTORE_EMPTY"
        printf '| near ramoops | %s |\n' "$NEAR_RAMOOPS"
        printf '| near panic_on_oops / panic | %s / %s |\n' "$NEAR_PANIC_ON_OOPS" "$NEAR_PANIC"
        printf '| near 额外 sysctl | %s |\n' "$NEAR_EXTRA_SYSCTLS"
        printf '| near tbtun-kmsg-relay | %s |\n' "$KMSG_RELAY_STATE"
        printf '| near tbtun-sampler | %s（baseline lines=%s，end=%s） |\n' \
            "$SAMPLER_STATE" "$SAMPLER_BASELINE_LINES" "$SAMPLER_END_LINES"
        printf '| .80 UDP 6666 接收端 | pid=%s log=%s |\n' "${NC_PID:-?}" "${NC_LOG:-?}"
        printf '| .80 tbtun-netconsole-rx | %s（行基线=%s，结束=%s） |\n' \
            "$RX_SERVICE_STATE" "$NC_START_LINES" "$NC_END_LINES"
        printf '| kmsg 探针 | %s |\n' "$NC_PROBE_OK"
        printf '| far tbtun 已加载 | %s |\n' "$FAR_MODULE_STATE"
        printf '| far srcversion | `%s`（expected `%s`） |\n' "${FAR_LOADED_SRCVERSION:-unknown}" "$FAR_SRCVERSION"
        printf '| far max_carriers | `%s`（要求 256） |\n' "${FAR_MAX_CARRIERS:-unknown}"
        printf '| far 当前 config carriers | `%s`（步骤 4 重写） |\n' "$FAR_PRE_CARRIERS"
        printf '| far config 读回 carriers | `%s` |\n' "$FAR_CFG_CARRIERS"
        printf '| near config 读回 carriers | `%s` |\n' "$NEAR_CFG_CARRIERS"
        printf '| far config / listeners | %s / %s |\n' \
            "$([[ $FAR_CFG_OK == 1 ]] && echo pass || echo fail)" "$FAR_LISTEN_STATE"
        printf '\n'

        printf '## 步骤、预期结果与失败分支\n\n'
        printf '| # | 步骤 | 预期结果 | 失败分支 | 实际 |\n|---|---|---|---|---|\n'
        printf '| 1 | 参数校验 | CARRIERS 在 8/16/32/64/128/256；SECS<=10；PROCS<=32 | 非法：退出 2 | carriers=%s mib=%s secs=%s procs=%s |\n' \
            "$CARRIERS" "$MIB" "$SECS" "$PROCS"
        printf '| 2 | 前置检查 | 两端 ssh、near 未加载、far 已加载且 srcversion/max_carriers 匹配、可见性 active、pstore 空、模块身份匹配 | 任一失败：退出 3，不加载模块、不写配置 | %s |\n' \
            "$([[ $PRECHECK_DONE == 1 ]] && echo pass || echo fail)"
        printf '| 3 | 锁与负载 | 两端 flock 获取；load1 只记录 | 锁忙：退出 4 | locks_held=%s near_load=%s far_load=%s |\n' \
            "$LOCKS_HELD" "${NEAR_LOAD:-?}" "${FAR_LOAD:-?}"
        printf '| 4 | far 配置 | ports %s、config 逐字读回，carriers=%s、n=%s、m=%s、w=%s、fec=on、mode=grouped、http-kal，9000/5202 监听 | 写失败/读回不一致/监听未起：退出 10，near 不加载 | %s |\n' \
            "$FAR_PORTBLOCK" "$CARRIERS" "$N" "$M" "$W" "$([[ $FAR_CFG_OK == 1 ]] && echo pass || echo fail)"
        printf '| 5 | near 配置 | ip rule、rmem/wmem、insmod max_carriers=256、加载后 max_carriers=%s、srcversion=%s、ports %s、config 逐字读回 carriers=%s | 任一步失败：退出 10，清理 | %s |\n' \
            "${NEAR_MAX_CARRIERS:-?}" "$NEAR_SRCVERSION" "$NEAR_PORTBLOCK" "$NEAR_CFG_CARRIERS" \
            "$([[ $NEAR_CFG_OK == 1 ]] && echo pass || echo fail)"
        printf '| 6 | join gate | %ss 内两端 c3 up=%s；记录 c3 n、c9 错误计数、http_parse_err、rx_errs、reorder_overflow | 超时或快照 up 不一致：退出 5，不触发 burst，收集证据 | %s |\n' \
            "$JOIN_TIMEOUT" "$CARRIERS" "$join_actual"
        printf '| 7 | 触发 | 恰好一轮 %s MiB / %ss interleaved，%ss 内返回 | 非 0/超时：记录，不重试；near 失联则转证据收集 | %s |\n' \
            "$MIB" "$SECS" "$BURST_TIMEOUT" "$burst_actual"
        printf '| 8 | 失联判定 | 记录 ping/ARP/ssh、boot_id、是否重启 | 无法判定：记 unknown，继续 far/netconsole 证据 | %s |\n' \
            "$lost_actual"
        printf '| 9 | 证据收集 | netconsole、pstore、journal、dmesg、stats、far dmesg/stats、join 轨迹、c9 检查全部落盘 | 单项失败：记录原因，不阻塞其他项 | %s |\n' \
            "$evidence_state"
        printf '| 10 | 清理 | near rmmod/rule/sysctl、far echo/rmmod、锁释放；失联时跳过模块/网络清理 | 清理失败退出 9 | %s |\n\n' \
            "$clean_actual"

        printf '## 关键计数器\n\n'
        printf '| 项 | near | far |\n|---|---|---|\n'
        printf '| c3 up（join 末次） | %s | %s |\n' "$NUP_LAST" "$FUP_LAST"
        printf '| c3 up（post） | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" up 2>/dev/null)" "$(stat_field "$EV/post-far.txt" up 2>/dev/null)"
        printf '| c3 n（join 末次） | %s | %s |\n' "$NN_LAST" "$FN_LAST"
        printf '| c3 n（post） | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" n 2>/dev/null)" "$(stat_field "$EV/post-far.txt" n 2>/dev/null)"
        printf '| c3 data_tx | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" data_tx 2>/dev/null)" "$(stat_field "$EV/post-far.txt" data_tx 2>/dev/null)"
        printf '| c3 rx_ack | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" rx_ack 2>/dev/null)" "$(stat_field "$EV/post-far.txt" rx_ack 2>/dev/null)"
        printf '| c3 reorder_overflow | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" reorder_overflow 2>/dev/null)" "$(stat_field "$EV/post-far.txt" reorder_overflow 2>/dev/null)"
        printf '| c8 http_parse_err | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" http_parse_err 2>/dev/null)" "$(stat_field "$EV/post-far.txt" http_parse_err 2>/dev/null)"
        printf '| rx_errs | %s | %s |\n' \
            "$(stat_field "$EV/post-near.txt" rx_errs 2>/dev/null)" "$(stat_field "$EV/post-far.txt" rx_errs 2>/dev/null)"
        printf '| c9 错误计数全零 | %s | %s |\n\n' "${c9_near:-not-collected}" "${c9_far:-not-collected}"

        printf '## 崩溃证据计数\n\n'
        printf -- '- netconsole 窗口内 `Internal error: Oops`：%s\n' "$oops_nc"
        printf -- '- netconsole 窗口内 `Kernel panic`：%s\n' "$panic_nc"
        printf -- '- near journal 内 Oops/panic：%s\n' "$oops_jr"
        printf -- '- near pstore 内 Oops/panic：%s\n' "$oops_ps"
        printf -- '- sampler 内 Oops/panic：%s\n' "$oops_sm"
        printf -- '- netconsole 窗口：行 %s -> %s（%s -> %s bytes）\n' \
            "$NC_START_LINES" "$NC_END_LINES" "$NC_START_SIZE" "$NC_END_SIZE"
        printf -- '- 注意：kmsg-relay/sampler 是用户态进程；panic 后可能停止，pstore 是主证据。\n\n'

        printf '## join 轮询轨迹（末尾 60 行）\n\n'
        printf '```\n'
        tail -n 60 "$EV/join-probes.txt" 2>/dev/null || true
        printf '```\n\n'

        printf '## 时间线（末尾 50 行）\n\n'
        printf '```\n'
        tail -n 50 "$EV/timeline.txt" 2>/dev/null || true
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
        printf -- '- 本次只触发一轮；无论结果如何都不重试。\n'
        printf -- '- 若 near 失联：先保存本目录，再按只读取证流程分析；不要重新加载模块。\n'
        printf -- '- 若 near 失联但无 Oops：netconsole 可能未送达 panic 输出，pstore/journal 也可能为空；结论只能写“未定”。\n'
        printf -- '- groups=%s 是 FEC 派生量（carriers/(n+m)），不是内核 config 键；config 中只有 n/m/w/carriers。\n' "$FEC_GROUPS"
    } >"$EV/SUMMARY.md" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# 清理。任何一步都不能挂住；near 失联时跳过模块/网络清理并保留现场。

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

    if (( NEAR_LOST || NEAR_REBOOTED )); then
        printf 'cleanup=skipped(near_lost=%s near_rebooted=%s); 保留现场\n' \
            "$NEAR_LOST" "$NEAR_REBOOTED" >>"$EV/cleanup.txt"
        note "cleanup: near 失联/重启，跳过模块/网络清理，保留现场"
    else
        if near_ssh_ok; then
            if (( NEAR_MODULE_LOADED )); then
                if timeout 25 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'rmmod tbtun' >>"$EV/cleanup.txt" 2>&1; then
                    printf 'near_rmmod=ok\n' >>"$EV/cleanup.txt"
                else
                    printf 'near_rmmod=failed\n' >>"$EV/cleanup.txt"
                    note "cleanup: near rmmod 失败"
                    CLEAN_FAIL=1
                fi
            else
                printf 'near_rmmod=skipped(not_loaded)\n' >>"$EV/cleanup.txt"
            fi
            if (( NEAR_RULE_ADDED )); then
                if timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
                    'ip rule del to 45.11.1.62/32 lookup main pref 100' >>"$EV/cleanup.txt" 2>&1; then
                    printf 'near_rule=deleted\n' >>"$EV/cleanup.txt"
                else
                    printf 'near_rule=delete_failed_or_absent\n' >>"$EV/cleanup.txt"
                fi
            else
                printf 'near_rule=not_added_by_script\n' >>"$EV/cleanup.txt"
            fi
            if (( NEAR_SYSCTL_SAVED )) && [[ -n $OLD_RMEM && -n $OLD_WMEM ]]; then
                if timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
                    "sysctl -q -w net.core.rmem_max=$OLD_RMEM net.core.wmem_max=$OLD_WMEM" >>"$EV/cleanup.txt" 2>&1; then
                    printf 'near_sysctl=restored\n' >>"$EV/cleanup.txt"
                else
                    printf 'near_sysctl=restore_failed\n' >>"$EV/cleanup.txt"
                    CLEAN_FAIL=1
                fi
            fi
            if (( NEAR_MODULE_LOADED )) && listening_on "$NEAR_HOST" "$NEAR_PORT"; then
                printf 'near_listen_%s=still_listening\n' "$NEAR_PORT" >>"$EV/cleanup.txt"
                CLEAN_FAIL=1
            fi
        else
            printf 'near_cleanup=skipped(unreachable)\n' >>"$EV/cleanup.txt"
            note "cleanup: near 不可达，跳过 rmmod/rule/sysctl；可见性服务保持运行"
            CLEAN_FAIL=1
        fi

        if (( FAR_TOUCHED )); then
            if timeout 10 ssh "${SSH_BASE[@]}" "$FAR_HOST" true >/dev/null 2>&1; then
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
            else
                printf 'far_cleanup=skipped(unreachable)\n' >>"$EV/cleanup.txt"
                CLEAN_FAIL=1
            fi
        else
            printf 'far_cleanup=skipped(not_touched)\n' >>"$EV/cleanup.txt"
        fi
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

mark start "prod-smoke carriers=$CARRIERS mib=$MIB secs=$SECS procs=$PROCS; evidence=$EV"
note "evidence: $EV"
note "config: carriers=$CARRIERS n=$N m=$M groups=$FEC_GROUPS w=$W near=$NEAR_PORTBLOCK far=$FAR_PORTBLOCK"

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
lock_acquire "$NEAR_HOST" /tmp/tbtun-rk3588.lock "prod-smoke-10s $STAMP" NEAR || die "$EX_BUSY" "near 锁忙: /tmp/tbtun-rk3588.lock"
lock_acquire "$FAR_HOST" /tmp/tbtun-hnd0.lock "prod-smoke-10s $STAMP" FAR || die "$EX_BUSY" "far 锁忙: /tmp/tbtun-hnd0.lock"
LOCKS_HELD=1

NEAR_LOAD=$(load1_on "$NEAR_HOST") || NEAR_LOAD=""
FAR_LOAD=$(load1_on "$FAR_HOST") || FAR_LOAD=""
[[ -n $NEAR_LOAD && -n $FAR_LOAD ]] || die "$EX_PRECOND" "读不到两端 load1"
if ! awk -v a="$NEAR_LOAD" -v b="$LOAD_LIMIT" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
    note "warn: $NEAR_HOST load1=$NEAR_LOAD > $LOAD_LIMIT（只记录，不阻塞）"
fi
if ! awk -v a="$FAR_LOAD" -v b="$LOAD_LIMIT" 'BEGIN { exit !(a + 0 <= b + 0) }'; then
    note "warn: $FAR_HOST load1=$FAR_LOAD > $LOAD_LIMIT（只记录，不阻塞）"
fi
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

# near：panic/ramoops 可见性。panic_on_oops/panic 只记录；pstore 空是硬闸门。
NEAR_PANIC_ON_OOPS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/panic_on_oops' 2>/dev/null) || NEAR_PANIC_ON_OOPS="?"
NEAR_PANIC=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/sys/kernel/panic' 2>/dev/null) || NEAR_PANIC="?"
NEAR_EXTRA_SYSCTLS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    'printf "oops_limit=%s panic_on_rcu_stall=%s oops_all_cpu_backtrace=%s\n" \
        "$(cat /proc/sys/kernel/oops_limit 2>/dev/null || echo ?)" \
        "$(cat /proc/sys/kernel/panic_on_rcu_stall 2>/dev/null || echo ?)" \
        "$(cat /proc/sys/kernel/oops_all_cpu_backtrace 2>/dev/null || echo ?)"' 2>/dev/null) \
    || NEAR_EXTRA_SYSCTLS="unknown"
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
    note "warn: $NEAR_HOST 未注册 ramoops 后端（记录，不阻塞）"
fi
mark precheck_visibility "panic_on_oops=$NEAR_PANIC_ON_OOPS panic=$NEAR_PANIC $NEAR_EXTRA_SYSCTLS pstore_empty=$NEAR_PSTORE_EMPTY ramoops=$NEAR_RAMOOPS"

# near：工具。
MISSING=$(timeout 15 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    'for t in bash cc timeout ss ip sysctl modinfo sha256sum systemctl wc awk sed grep date seq head tail journalctl; do command -v "$t" >/dev/null 2>&1 || printf "%s " "$t"; done' 2>/dev/null) || MISSING="ssh"
[[ -z $MISSING ]] || die "$EX_PRECOND" "$NEAR_HOST 缺少工具: $MISSING"

# far 修复版闸门：先于任何 near 写入。
far_identity_gate || die "$EX_PRECOND" "far 修复版身份闸门失败；停止，不跑 near（见上一条）"
FAR_MODULE_STATE="pass"
mark precheck_far "srcversion=$FAR_LOADED_SRCVERSION expected=$FAR_SRCVERSION source=$FAR_SRCVERSION_SOURCE staged=$FAR_KO_SRCVERSION max_carriers=$FAR_MAX_CARRIERS"

# 可见性：.65 relay/sampler active，.80 接收端 active。
visibility_precheck || die "$EX_PRECOND" "可见性服务未就绪（relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE）"
NC_PID=$(nc_find_pid) || die "$EX_PRECOND" "工作站 UDP 6666 没有接收端（ss -ulnp 看不到 pid）"
NC_LOG=$(nc_find_log "$NC_PID") || NC_LOG="$HOME/tbtun-forensics/netconsole-6666.log"
[[ -f $NC_LOG ]] || die "$EX_PRECOND" "UDP 6666 接收端（pid $NC_PID）没有文件输出，无法保存 netconsole 证据"
[[ -s $NC_LOG ]] || note "netconsole: 接收文件目前为空: $NC_LOG"
NC_START_LINES=$(wc -l <"$NC_LOG" 2>/dev/null) || NC_START_LINES=0
NC_START_SIZE=$(stat -c %s "$NC_LOG" 2>/dev/null) || NC_START_SIZE=0
if ! nc_probe; then
    die "$EX_PRECOND" "kmsg 探针没有到达 $NC_LOG；relay/receiver 路径不通，拒绝开跑"
fi
NC_PROBE_OK=1
mark precheck_receiver "pid=$NC_PID log=$NC_LOG lines=$NC_START_LINES relay=$KMSG_RELAY_STATE sampler=$SAMPLER_STATE rx=$RX_SERVICE_STATE"

# far：已由部署脚本加载；记录当前 config 与 echo 工具。
FAR_PRE_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || FAR_PRE_CFG=""
printf '%s\n' "$FAR_PRE_CFG" >"$EV/far-pre-config.txt"
if [[ -n $FAR_PRE_CFG ]]; then
    FAR_PRE_CARRIERS=$(cfg_token "$FAR_PRE_CFG" carriers || true)
    FAR_PRE_CARRIERS=${FAR_PRE_CARRIERS:-?}
fi
require_tokens "far pre" "$FAR_PRE_CFG" role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 \
    || note "warn: far 当前 config 不是 far 形状（将在步骤 4 重写）：'$FAR_PRE_CFG'"
timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "test -x '$FAR_STAGE/tbt_echo'" >/dev/null 2>&1 \
    || die "$EX_PRECOND" "far 缺少 $FAR_STAGE/tbt_echo"
FAR_ECHO_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "sha256sum '$FAR_STAGE/tbt_echo'" 2>/dev/null | awk '{print $1}') || FAR_ECHO_SHA=""
FAR_KO_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "sha256sum '$FAR_STAGE/tbtun.ko'" 2>/dev/null | awk '{print $1}') || FAR_KO_SHA=""
if [[ -z $FAR_KO_SRCVERSION ]]; then
    FAR_KO_SRCVERSION=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "modinfo -F srcversion '$FAR_STAGE/tbtun.ko' 2>/dev/null" 2>/dev/null) || FAR_KO_SRCVERSION=""
fi
FAR_KO_VERMAGIC=$(timeout 20 ssh "${SSH_BASE[@]}" "$FAR_HOST" "modinfo -F vermagic '$FAR_STAGE/tbtun.ko' 2>/dev/null" 2>/dev/null) || FAR_KO_VERMAGIC=""
FAR_PRE_LISTEN=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "ss -lntH 'sport = :$FAR_PORT'" 2>/dev/null | wc -l) || FAR_PRE_LISTEN=0
if (( FAR_PRE_LISTEN > 0 )); then
    FAR_LISTEN_STATE="precheck: listening"
else
    FAR_LISTEN_STATE="precheck: not-listening"
fi
mark precheck_far_state "pre_carriers=$FAR_PRE_CARRIERS echo_sha=${FAR_ECHO_SHA:-unknown}"

# near：固定 staged tree，校验模块身份。
[[ -n $NEAR_KO ]] || die "$EX_PRECOND" "NEAR_KO 为空"
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -s '$NEAR_KO'" >/dev/null 2>&1 \
    || die "$EX_PRECOND" "$NEAR_KO 不存在或为空"
record_near_module_identity
[[ ${NEAR_KO_VERMAGIC%% *} == "$NEAR_KREL" ]] \
    || die "$EX_PRECOND" "near 模块 vermagic '$NEAR_KO_VERMAGIC' != kernel '$NEAR_KREL'"
[[ -n $NEAR_KO_SRCVERSION ]] || die "$EX_PRECOND" "near 模块没有 srcversion"
if [[ -n $NEAR_SRCVERSION ]]; then
    NEAR_SRCVERSION_SOURCE="env"
else
    NEAR_SRCVERSION=$NEAR_KO_SRCVERSION
    NEAR_SRCVERSION_SOURCE="auto-ko"
fi
[[ $NEAR_KO_SRCVERSION == "$NEAR_SRCVERSION" ]] \
    || die "$EX_PRECOND" "near ko srcversion '$NEAR_KO_SRCVERSION' != expected '$NEAR_SRCVERSION'（source=$NEAR_SRCVERSION_SOURCE）"
near_sha256_gate || die "$EX_PRECOND" "near sha256 闸门失败；见上一条"
NEAR_IDENTITY_MATCH="yes"
mark precheck_module "ko=$NEAR_KO srcversion=$NEAR_KO_SRCVERSION expected=$NEAR_SRCVERSION source=$NEAR_SRCVERSION_SOURCE sha=${NEAR_KO_SHA:-unknown} sha_gate=$NEAR_SHA256_GATE identity=match"

# 预快照。
note "snapshot: pre"
snapshot "$NEAR_HOST" "$EV/pre-near.txt" || die "$EX_PRECOND" "near 预快照失败"
snapshot "$FAR_HOST" "$EV/pre-far.txt" || die "$EX_PRECOND" "far 预快照失败"
PRE_NEAR_DMESG=$(sec_number "$EV/pre-near.txt" dmesg_count)
PRE_FAR_DMESG=$(sec_number "$EV/pre-far.txt" dmesg_count)
PRE_NEAR_DMESG=${PRE_NEAR_DMESG:-0}
PRE_FAR_DMESG=${PRE_FAR_DMESG:-0}

# burst 工具：优先使用 staged tree 里已有的 tbt_burst；缺失时只构建这个
# 测试工具，不触碰模块状态。
if ! timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -x '$NEAR_DIR/tbt_burst'" >/dev/null 2>&1; then
    if ! timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "test -f '$NEAR_DIR/host/tbt_burst.c'" >/dev/null 2>&1; then
        rscp_near "$REPO/host/tbt_burst.c" "$NEAR_HOST:$NEAR_DIR/host/tbt_burst.c" \
            || die "$EX_SETUP" "拷贝 tbt_burst.c 到 near 失败"
    fi
    timeout 60 ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
        "cd '$NEAR_DIR' && cc -O2 -Wall -Wextra -o tbt_burst host/tbt_burst.c" \
        >"$EV/burst-build.log" 2>&1 \
        || die "$EX_SETUP" "near 上构建 tbt_burst 失败；日志 $EV/burst-build.log"
fi
BURST_SHA=$(timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "sha256sum '$NEAR_DIR/tbt_burst' '$NEAR_DIR/host/tbt_burst.c'" 2>/dev/null) || BURST_SHA=""
{
    printf 'near_src_dir=%s\n' "$NEAR_DIR"
    printf 'near_ko=%s\n' "$NEAR_KO"
    printf 'near_tree_commit=%s\n' "${NEAR_TREE_COMMIT:-unknown}"
    printf 'near_ko_sha256=%s\n' "${NEAR_KO_SHA:-unknown}"
    printf 'near_ko_srcversion=%s\n' "${NEAR_KO_SRCVERSION:-unknown}"
    printf 'near_ko_vermagic=%s\n' "${NEAR_KO_VERMAGIC:-unknown}"
    printf 'near_ko_mtime=%s\n' "$NEAR_KO_MTIME"
    printf 'expected_near_srcversion=%s\n' "$NEAR_SRCVERSION"
    printf 'near_srcversion_source=%s\n' "$NEAR_SRCVERSION_SOURCE"
    printf 'near_sha256_expected=%s\n' "${NEAR_SHA256:-not-set}"
    printf 'near_sha256_gate=%s\n' "$NEAR_SHA256_GATE"
    printf 'near_identity_match=%s\n' "$NEAR_IDENTITY_MATCH"
    printf 'near_insmod_params=%s\n' "$INSMOD_PARAMS"
    printf 'near_loaded_srcversion=%s\n' "${NEAR_LOADED_SRCVERSION:-not-loaded-yet}"
    printf 'near_max_carriers=%s\n' "${NEAR_MAX_CARRIERS:-not-loaded-yet}"
    printf 'far_stage=%s\n' "$FAR_STAGE"
    printf 'far_ko_sha256=%s\n' "${FAR_KO_SHA:-unknown}"
    printf 'far_ko_srcversion=%s\n' "${FAR_KO_SRCVERSION:-unknown}"
    printf 'far_ko_vermagic=%s\n' "${FAR_KO_VERMAGIC:-unknown}"
    printf 'far_loaded_srcversion=%s\n' "${FAR_LOADED_SRCVERSION:-unknown}"
    printf 'far_max_carriers=%s\n' "${FAR_MAX_CARRIERS:-unknown}"
    printf 'far_expected_srcversion=%s\n' "$FAR_SRCVERSION"
    printf 'far_srcversion_source=%s\n' "$FAR_SRCVERSION_SOURCE"
    printf 'far_identity_match=%s\n' "$FAR_IDENTITY_MATCH"
    printf 'far_known_bad_srcversions=%s\n' "$FAR_KNOWN_BAD_SRCVERSIONS"
    printf 'far_echo_sha256=%s\n' "${FAR_ECHO_SHA:-unknown}"
    printf 'far_pre_config_carriers=%s\n' "$FAR_PRE_CARRIERS"
    printf 'burst_sha256:\n%s\n' "${BURST_SHA:-unknown}"
} >"$EV/module.txt"
mark precheck_done "burst_sha=${BURST_SHA%% *}"
PRECHECK_DONE=1

# --- 2. far 配置 ------------------------------------------------------------

mark far_config_start
FAR_TOUCHED=1
far_stop_echo
timeout 15 ssh "${SSH_BASE[@]}" "$FAR_HOST" "printf 'ports $FAR_PORTBLOCK\n' > /proc/net/tbtun/ports" \
    || die "$EX_SETUP" "far ports 写入失败"
FAR_GOT_PORTS=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" "grep '^ports ' /proc/net/tbtun/ports" 2>/dev/null) || FAR_GOT_PORTS=""
[[ $FAR_GOT_PORTS == "ports $FAR_PORTBLOCK" ]] \
    || die "$EX_SETUP" "far ports 读回 '$FAR_GOT_PORTS' != 'ports $FAR_PORTBLOCK'"

timeout 15 ssh "${SSH_BASE[@]}" "$FAR_HOST" "printf '%s\n' '$FAR_CFG' > /proc/net/tbtun/config" \
    || die "$EX_SETUP" "far config 写入失败"
FAR_GOT_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$FAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || FAR_GOT_CFG=""
FAR_CFG_CARRIERS=$(cfg_token "$FAR_GOT_CFG" carriers || true)
FAR_CFG_CARRIERS=${FAR_CFG_CARRIERS:-?}
require_tokens "far prod-smoke" "$FAR_GOT_CFG" \
    role=far listen=0.0.0.0:9000 target=127.0.0.1:5202 n="$N" m="$M" fec=on mode=grouped w="$W" \
    carriers="$CARRIERS" crypto=on profile=http-kal portblock="$FAR_PORTBLOCK" nonce="$NONCE" \
    || die "$EX_SETUP" "far config 不是预期形状：'$FAR_GOT_CFG'"
[[ $FAR_GOT_CFG == "$FAR_CFG" ]] \
    || die "$EX_SETUP" "far config 读回不一致：'$FAR_GOT_CFG'"
[[ $FAR_CFG_CARRIERS == "$CARRIERS" ]] \
    || die "$EX_SETUP" "far config carriers=$FAR_CFG_CARRIERS != $CARRIERS"
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
FAR_LISTEN_STATE="pass"
FAR_CFG_OK=1
mark far_config_done "ports=$FAR_PORTBLOCK config=exact carriers=$FAR_CFG_CARRIERS echo=$ECHO_PORT"

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

timeout 20 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "insmod '$NEAR_KO' $INSMOD_PARAMS" \
    || die "$EX_SETUP" "near insmod 失败"
NEAR_MODULE_LOADED=1
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'test -d /proc/net/tbtun' >/dev/null 2>&1 \
    || die "$EX_SETUP" "insmod 后 /proc/net/tbtun 不存在"
NEAR_LOADED_SRCVERSION=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /sys/module/tbtun/srcversion' 2>/dev/null) || NEAR_LOADED_SRCVERSION=""
[[ $NEAR_LOADED_SRCVERSION == "$NEAR_SRCVERSION" ]] \
    || die "$EX_SETUP" "加载的 srcversion '$NEAR_LOADED_SRCVERSION' != expected '$NEAR_SRCVERSION'"
NEAR_MAX_CARRIERS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /sys/module/tbtun/parameters/max_carriers' 2>/dev/null) || NEAR_MAX_CARRIERS=""
[[ $NEAR_MAX_CARRIERS == 256 ]] \
    || die "$EX_SETUP" "near max_carriers='$NEAR_MAX_CARRIERS' != 256"
NEAR_MAX_TUNNELS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /sys/module/tbtun/parameters/max_tunnels' 2>/dev/null) || NEAR_MAX_TUNNELS=""
NEAR_DEBUG=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /sys/module/tbtun/parameters/debug' 2>/dev/null) || NEAR_DEBUG=""
record_near_module_identity
near_sha256_gate || die "$EX_SETUP" "insmod 后 near sha256 闸门失败；ko 可能在预检后被替换"

timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "printf 'ports $NEAR_PORTBLOCK\n' > /proc/net/tbtun/ports" \
    || die "$EX_SETUP" "near ports 写入失败"
NEAR_GOT_PORTS=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "grep '^ports ' /proc/net/tbtun/ports" 2>/dev/null) || NEAR_GOT_PORTS=""
[[ $NEAR_GOT_PORTS == "ports $NEAR_PORTBLOCK" ]] \
    || die "$EX_SETUP" "near ports 读回 '$NEAR_GOT_PORTS' != 'ports $NEAR_PORTBLOCK'"

# 写 config 即开始 dial；join 计时从这一刻起。
timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" "printf '%s\n' '$NEAR_CFG' > /proc/net/tbtun/config" \
    || die "$EX_SETUP" "near config 写入失败"
NEAR_GOT_CFG=$(timeout 12 ssh "${SSH_BASE[@]}" "$NEAR_HOST" 'cat /proc/net/tbtun/config' 2>/dev/null) || NEAR_GOT_CFG=""
NEAR_CFG_CARRIERS=$(cfg_token "$NEAR_GOT_CFG" carriers || true)
NEAR_CFG_CARRIERS=${NEAR_CFG_CARRIERS:-?}
require_tokens "near prod-smoke" "$NEAR_GOT_CFG" \
    role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n="$N" m="$M" fec=on mode=grouped w="$W" \
    carriers="$CARRIERS" crypto=on profile=http-kal portblock="$NEAR_PORTBLOCK" nonce="$NONCE" \
    || die "$EX_SETUP" "near config 不是预期形状：'$NEAR_GOT_CFG'"
[[ $NEAR_GOT_CFG == "$NEAR_CFG" ]] \
    || die "$EX_SETUP" "near config 读回不一致：'$NEAR_GOT_CFG'"
[[ $NEAR_CFG_CARRIERS == "$CARRIERS" ]] \
    || die "$EX_SETUP" "near config carriers=$NEAR_CFG_CARRIERS != $CARRIERS"
printf '%s\n' "$NEAR_GOT_CFG" >"$EV/config-near.txt"
NEAR_CFG_OK=1
mark near_config_done "ports=$NEAR_PORTBLOCK config=exact carriers=$NEAR_CFG_CARRIERS max_carriers=$NEAR_MAX_CARRIERS"

# --- 4. join gate -----------------------------------------------------------

mark join_start "timeout=${JOIN_TIMEOUT}s target=up=$CARRIERS"
JOIN_DEADLINE=$((SECONDS + JOIN_TIMEOUT))
NEAR_FAILS=0
while (( SECONDS < JOIN_DEADLINE )); do
    NSTAT=$(live_c3 "$NEAR_HOST") || NSTAT=""
    FSTAT=$(live_c3 "$FAR_HOST") || FSTAT=""
    NUP=${NSTAT%% *}
    NN=${NSTAT#* }
    FUP=${FSTAT%% *}
    FN=${FSTAT#* }
    [[ -n $NUP ]] || NUP="?"
    [[ -n $NN ]] || NN="?"
    [[ -n $FUP ]] || FUP="?"
    [[ -n $FN ]] || FN="?"
    NUP_LAST=$NUP
    NN_LAST=$NN
    FUP_LAST=$FUP
    FN_LAST=$FN
    printf '%s near_up=%s near_n=%s far_up=%s far_n=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$NUP" "$NN" "$FUP" "$FN" >>"$EV/join-probes.txt"
    if [[ $NUP == "$CARRIERS" && $FUP == "$CARRIERS" ]]; then
        JOIN_OK=1
        break
    fi
    if [[ $NUP == "?" ]]; then
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
    for _end in near far; do
        _f=$EV/joined-$_end.txt
        [[ $(stat_field "$_f" up) == "$CARRIERS" ]] || { note "gate: $_end up=$(stat_field "$_f" up)"; GATE_FAIL=1; }
        _n=$(stat_field "$_f" n)
        [[ $_n == "$CARRIERS" ]] || { note "warn: $_end c3 n=${_n:-missing} != $CARRIERS（记录，不阻塞）"; COUNTER_WARN=1; }
        _v=$(stat_field "$_f" http_parse_err)
        [[ -z $_v || $_v == 0 ]] || { note "warn: $_end http_parse_err=$_v（记录，不阻塞）"; COUNTER_WARN=1; }
        _v=$(stat_field "$_f" rx_errs)
        [[ -z $_v || $_v == 0 ]] || { note "warn: $_end rx_errs=$_v（记录，不阻塞）"; COUNTER_WARN=1; }
        _v=$(stat_field "$_f" reorder_overflow)
        [[ -z $_v || $_v == 0 ]] || { note "warn: $_end reorder_overflow=$_v（记录，不阻塞）"; COUNTER_WARN=1; }
    done
    write_counters joined "$EV/joined-near.txt" "$EV/joined-far.txt" "$EV/counters-joined.txt"
    counter_warn_from "$EV/joined-near.txt" joined near
    counter_warn_from "$EV/joined-far.txt" joined far
    if (( GATE_FAIL )); then
        JOIN_OK=0
        mark join_gate_failed "up mismatch in joined snapshot"
    elif (( COUNTER_WARN )); then
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

# --- 5. 唯一一轮 burst ------------------------------------------------------

if (( BURST_ATTEMPTED )); then
    die "$EX_USAGE" "内部错误：burst 已经尝试过，拒绝第二轮"
fi
# 可见性服务可能被外部 agent 改动；触发前再探一次。
if ! nc_probe; then
    EVIDENCE_INCOMPLETE=1
    die "$EX_PRECOND" "触发前 kmsg 探针丢失；拒绝开跑（崩溃证据无法保证）"
fi
BURST_ATTEMPTED=1
mark burst_start "procs=$PROCS tbt_burst 127.0.0.1 $NEAR_PORT $MIB $SECS interleaved timeout=${BURST_TIMEOUT}s"
set +e
timeout $((BURST_TIMEOUT + 30)) ssh "${SSH_BASE[@]}" "$NEAR_HOST" \
    "bash -s -- '$NEAR_DIR' '$PROCS' '$BURST_TIMEOUT' '$MIB' '$SECS' '$NEAR_PORT'" >"$EV/burst.txt" 2>&1 <<'EOS'
set -u
dir=$1
procs=$2
btimeout=$3
mib=$4
secs=$5
port=$6
cd "$dir" || exit 3
rm -f prod-smoke-burst-*.log prod-smoke-burst-*.rc
pids=""
i=1
while [ "$i" -le "$procs" ]; do
    (
        start=$(date +%s%N)
        timeout "$btimeout" ./tbt_burst 127.0.0.1 "$port" "$mib" "$secs" interleaved > "prod-smoke-burst-$i.log" 2>&1
        rc=$?
        end=$(date +%s%N)
        printf 'burst_rc=%d wall_ms=%d\n' "$rc" "$(( (end - start) / 1000000 ))" > "prod-smoke-burst-$i.rc"
    ) &
    pids="$pids $!"
    i=$((i + 1))
done
wait $pids
for i in $(seq 1 "$procs"); do
    printf '=== burst %s ===\n' "$i"
    cat "prod-smoke-burst-$i.rc" 2>/dev/null || echo 'burst_rc=missing'
    cat "prod-smoke-burst-$i.log" 2>/dev/null || true
done
exit 0
EOS
BURST_SSH_RC=$?
set -e
BURST_RCS=$(sed -n 's/^burst_rc=\([0-9-]*\) wall_ms=.*/\1/p' "$EV/burst.txt" 2>/dev/null | tr '\n' ' ') || BURST_RCS=""
BURST_RC_COUNT=$(printf '%s\n' "$BURST_RCS" | wc -w) || BURST_RC_COUNT=0
BURST_RC=""
if grep -q '^burst_rc=missing$' "$EV/burst.txt" 2>/dev/null; then
    # 某个进程没写出 rc（被杀、磁盘/权限、ssh 中断）：不能当作 0。
    BURST_RC="missing"
elif (( BURST_RC_COUNT != PROCS )); then
    BURST_RC="incomplete"
elif (( BURST_RC_COUNT > 0 )); then
    BURST_RC=0
    for _rc in $BURST_RCS; do
        if [[ $_rc != 0 ]]; then
            BURST_RC=$_rc
            break
        fi
    done
fi
BURST_WALL_MS=$(sed -n 's/^burst_rc=[0-9-]* wall_ms=\([0-9]*\)$/\1/p' "$EV/burst.txt" 2>/dev/null | sort -n | tail -n 1) || BURST_WALL_MS=""
BURST_END_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
mark burst_end "ssh_rc=$BURST_SSH_RC rcs=${BURST_RCS:-unknown} burst_rc=${BURST_RC:-unknown} wall_ms=${BURST_WALL_MS:-?} at=$BURST_END_UTC"

# 拉取每个进程的日志和 rc；失联时失败，不阻塞。
if near_ssh_ok; then
    for _i in $(seq 1 "$PROCS"); do
        rscp_near "$NEAR_HOST:$NEAR_DIR/prod-smoke-burst-$_i.log" "$EV/burst-$_i.log" 2>/dev/null || true
        rscp_near "$NEAR_HOST:$NEAR_DIR/prod-smoke-burst-$_i.rc" "$EV/burst-$_i.rc" 2>/dev/null || true
    done
fi

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
