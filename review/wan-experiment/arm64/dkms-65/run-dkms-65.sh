#!/bin/bash
# run-dkms-65.sh - the DKMS acceptance flow on .65 (real machine, 6.1.172).
#
# Seven steps after the preflight, each judged separately:
#   1 source     0.2.0 tree present, version and commit as expected
#   2 clean      no module, no dkms entry, no stale sources or ko files
#   3 install    install_dkms.sh --kernel <rel>, module loaded by the script
#   4 config     tunnel write, canonical read-back, listen, delete
#   5 rmmod      unload, procfs gone, no kernel splat
#   6 modprobe   reload, version and filename from updates/dkms, config again
#   7 uninstall  --uninstall, every artifact gone
#
# Preflight checks the one system prerequisite: dkms 3.2.2 is installed. This
# script does NOT install packages. If dkms is missing, the only allowed
# command is:
#   apt-get install -y --no-install-recommends dkms
# Ubuntu 22.04 ships 3.2.2-1ubuntu1 for it (dkms --version 3.2.2). Do not add
# --install-recommends: apt would pull linux-headers for the wrong kernel and
# change the system state.
#
# The hardware watchdog feeder must already be running. This script only
# reads that fact; rmmod/modprobe must never be preceded by stopping it.
#
# Markers, same shape as review/dkms-qemu/guest-test.sh:
#   DKMS_STEP <name> rc=<n> secs=<n>
#   DKMS_CHECK <name> PASS|FAIL (detail)
#   DKMS_ROLLBACK class=<build|install|modprobe|rmmod-in-use|uninstall-partial>
#   DKMS_RESULT PASS|FAIL
#
# Environment:
#   REPO=/root/tbtun-0.2.0-fix-20261107T013251Z
#                               source tree (must contain scripts/install_dkms.sh)
#   VER=0.2.0                   expected package version
#   EXPECT_HEAD=                optional git commit the tree must be at
#   EXPECT_DKMS=3.2.2           expected dkms version
#   EXPECT_DKMS_PKG=3.2.2-1ubuntu1  expected dpkg version of dkms
#   OUT=/root/dkms-65-<UTC>     evidence directory
#   KEEP=0                      1: do not auto-rollback on failure (forensics)
#
# Usage: run-dkms-65.sh [--preflight|--run] [--keep]
set -u
set -o pipefail

MODE=run
REPO=${REPO:-/root/tbtun-0.2.0-fix-20261107T013251Z}
VER=${VER:-0.2.0}
EXPECT_HEAD=${EXPECT_HEAD:-}
EXPECT_DKMS=${EXPECT_DKMS:-3.2.2}
EXPECT_DKMS_PKG=${EXPECT_DKMS_PKG:-3.2.2-1ubuntu1}
KEEP=${KEEP:-0}
OUT=${OUT:-/root/dkms-65-$(date -u +%Y%m%dT%H%M%SZ)}
REL=$(uname -r)
ARCH=$(uname -m)
MODULE=tbtun
FAILED=0
STEP_RC=0
ROLLED=0

for arg in "$@"; do
    case "$arg" in
        --preflight) MODE=preflight ;;
        --run) MODE=run ;;
        --keep) KEEP=1 ;;
        -h | --help)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            printf 'run-dkms-65: unknown argument: %s\n' "$arg" >&2
            exit 2
            ;;
    esac
done

mkdir -p "$OUT"
: >"$OUT/run.log"

say() { printf '%s\n' "$*" | tee -a "$OUT/run.log"; }
ok() { printf 'DKMS_CHECK %s PASS%s\n' "$1" "${2:+ ($2)}" | tee -a "$OUT/run.log"; }
bad() {
    printf 'DKMS_CHECK %s FAIL%s\n' "$1" "${2:+ ($2)}" | tee -a "$OUT/run.log"
    FAILED=$((FAILED + 1))
}

capture() {
    local count=$1 name=$2
    shift 2
    local t0 rc=0
    t0=$(date +%s)
    printf '\n=== STEP %s ===\n$ %s\n' "$name" "$(printf '%q ' "$@")" | tee -a "$OUT/run.log"
    "$@" >"$OUT/$name.log" 2>&1 || rc=$?
    tee -a "$OUT/run.log" <"$OUT/$name.log"
    printf 'DKMS_STEP %s rc=%d secs=%d\n' "$name" "$rc" \
        "$(( $(date +%s) - t0 ))" | tee -a "$OUT/run.log"
    STEP_RC=$rc
    if [ "$rc" -ne 0 ] && [ "$count" -ne 0 ]; then
        FAILED=$((FAILED + 1))
    fi
    return 0
}
step() { capture 1 "$@"; }

check_true() {
    local name=$1 cond=$2 detail=${3:-}
    if eval "$cond"; then ok "$name" "$detail"; else bad "$name" "$detail"; fi
    return 0
}
check_absent() {
    if [ -e "$2" ]; then bad "$1" "exists: $2"; else ok "$1" "absent: $2"; fi
    return 0
}
check_present() {
    if [ -e "$2" ]; then ok "$1" "present: $2"; else bad "$1" "missing: $2"; fi
    return 0
}
klog() { dmesg 2>/dev/null || journalctl -k -n 400 --no-pager 2>/dev/null || true; }
dkms_status() { dkms status 2>/dev/null || true; }

rollback() {
    local cls=$1
    ROLLED=1
    say "DKMS_ROLLBACK class=$cls"
    if [ "$KEEP" -eq 1 ]; then
        say "KEEP=1: rollback skipped; artifacts stay for forensics"
        return 0
    fi
    case "$cls" in
        build)
            rm -rf "/var/lib/dkms/$MODULE/$VER" "/usr/src/$MODULE-$VER"
            depmod -a "$REL" 2>/dev/null || true
            ;;
        install)
            rm -rf "/var/lib/dkms/$MODULE/$VER" "/usr/src/$MODULE-$VER"
            rm -f /lib/modules/"$REL"/updates/dkms/"$MODULE".ko*
            depmod -a "$REL" 2>/dev/null || true
            ;;
        modprobe)
            "$REPO/scripts/install_dkms.sh" --uninstall >"$OUT/rollback-modprobe.log" 2>&1 || true
            ;;
        rmmod-in-use)
            printf 'del 1\n' >/proc/net/tbtun/config 2>/dev/null || true
            rmmod "$MODULE" 2>/dev/null || true
            fuser -v /proc/net/tbtun/* >"$OUT/rollback-fuser.log" 2>&1 || true
            ;;
        uninstall-partial)
            dkms remove -m "$MODULE" -v "$VER" --all >"$OUT/rollback-dkms-remove.log" 2>&1 || true
            rm -rf "/var/lib/dkms/$MODULE/$VER" "/usr/src/$MODULE-$VER"
            rm -f /lib/modules/"$REL"/updates/dkms/"$MODULE".ko*
            rm -f /lib/modules/"$REL"/updates/"$MODULE".ko*
            rm -f /lib/modules/"$REL"/extra/"$MODULE".ko*
            depmod -a "$REL" 2>/dev/null || true
            ;;
        *)
            say "unknown rollback class: $cls"
            ;;
    esac
    say "rollback class=$cls done; residuals:"
    dkms_status | sed 's/^/    /' | tee -a "$OUT/run.log"
    ls -d /usr/src/"$MODULE"-* 2>/dev/null | sed 's/^/    /' | tee -a "$OUT/run.log"
    ls /lib/modules/"$REL"/updates/dkms/"$MODULE".ko* 2>/dev/null | sed 's/^/    /' | tee -a "$OUT/run.log"
    return 0
}

finish() {
    local verdict=PASS
    if [ "$FAILED" -ne 0 ]; then verdict=FAIL; fi
    printf 'DKMS_RESULT %s (checks_failed=%d rollback=%d)\n' \
        "$verdict" "$FAILED" "$ROLLED" | tee -a "$OUT/run.log"
    printf 'evidence: %s\n' "$OUT" | tee -a "$OUT/run.log"
    if [ "$verdict" = PASS ]; then exit 0; fi
    if [ "$ROLLED" -eq 1 ]; then exit 4; fi
    exit 3
}

# ---------------------------------------------------------------------------
# preflight

say "== preflight"
say "host: $(uname -a)"
say "release=$REL arch=$ARCH"

if [ "$(id -u)" -eq 0 ]; then ok root; else bad root "must run as root"; finish; fi

got_dkms=""
if command -v dkms >/dev/null 2>&1; then
    got_dkms=$(dkms --version 2>/dev/null | grep -oE '[0-9]+(\.[0-9]+)+' | head -n 1)
    if [ "$got_dkms" = "$EXPECT_DKMS" ]; then
        ok dkms_version "dkms $got_dkms"
    else
        bad dkms_version "dkms=${got_dkms:-?} expected=$EXPECT_DKMS"
    fi
else
    bad dkms_installed "dkms not found"
fi
if [ "$got_dkms" != "$EXPECT_DKMS" ]; then
    # This script never installs packages: give the one allowed command.
    say "   the only allowed install command (run it yourself, then rerun):"
    say "     apt-get install -y --no-install-recommends dkms"
    say "   expected package: dkms $EXPECT_DKMS_PKG (dkms --version $EXPECT_DKMS)"
    say "   never add --install-recommends: it pulls linux-headers for the wrong kernel"
fi
if command -v dkms >/dev/null 2>&1 && command -v dpkg-query >/dev/null 2>&1; then
    got_dkms_pkg=$(dpkg-query -W -f '${Version}' dkms 2>/dev/null || true)
    if [ "$got_dkms_pkg" = "$EXPECT_DKMS_PKG" ]; then
        ok dkms_pkg_version "$got_dkms_pkg"
    else
        bad dkms_pkg_version "got=${got_dkms_pkg:-none} expected=$EXPECT_DKMS_PKG"
    fi
fi
if command -v dpkg >/dev/null 2>&1; then
    check_true dpkg_ii "dpkg -l dkms 2>/dev/null | grep -q '^ii'" "dkms package state ii"
fi
check_present kernel_headers "/lib/modules/$REL/build"
check_present kernel_symvers "/lib/modules/$REL/build/Module.symvers"
if command -v gcc >/dev/null 2>&1 && command -v make >/dev/null 2>&1; then
    ok toolchain "$(gcc -dumpversion 2>/dev/null) / make"
else
    bad toolchain "gcc or make missing"
fi
for t in modprobe insmod rmmod depmod modinfo; do
    if command -v "$t" >/dev/null 2>&1; then ok "kmod_$t" "$t"; else bad "kmod_$t" "missing"; fi
done

say "== watchdog feeder (must stay up; this script never stops it)"
holder=""
for fddir in /proc/[0-9]*/fd; do
    if ls -l "$fddir" 2>/dev/null | grep -qF /dev/watchdog0; then
        pid=${fddir#/proc/}
        pid=${pid%/fd}
        holder="pid=$pid($(cat "/proc/$pid/comm" 2>/dev/null || echo '?'))"
        break
    fi
done
if [ -n "$holder" ]; then
    ok watchdog_feeder "$holder holds /dev/watchdog0"
else
    bad watchdog_feeder "no holder; stopping the feeder is a 44 s reset"
fi
say "   RuntimeWatchdogUSec=$(systemctl show -p RuntimeWatchdogUSec --value 2>/dev/null || echo unknown)"

if [ "$MODE" = preflight ]; then
    finish
fi

if [ "$FAILED" -ne 0 ]; then
    say "preflight failed; no step executed"
    finish
fi

# ---------------------------------------------------------------------------
# step 1: source tree

say "== step 1: source"
step source test -d "$REPO"
check_present source_tree "$REPO"
check_present install_script "$REPO/scripts/install_dkms.sh"
check_present uapi "$REPO/tbtun/tbt_uapi.h"
if [ -x "$REPO/scripts/install_dkms.sh" ]; then ok install_script_exec; else bad install_script_exec "not executable"; fi
if [ -x "$REPO/scripts/mkdkmsconf.sh" ]; then
    got_ver=$("$REPO/scripts/mkdkmsconf.sh" --version 2>/dev/null | tr -d '[:space:]')
    if [ "$got_ver" = "$VER" ]; then ok source_version "$got_ver"; else bad source_version "got=$got_ver expected=$VER"; fi
else
    bad source_version "scripts/mkdkmsconf.sh missing or not executable"
fi
if [ -n "$EXPECT_HEAD" ]; then
    head=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || true)
    if [ "$head" = "$EXPECT_HEAD" ]; then ok source_head "$head"; else bad source_head "got=${head:-none} expected=$EXPECT_HEAD"; fi
fi
if [ "$STEP_RC" -ne 0 ] || [ "$FAILED" -ne 0 ]; then
    say "source check failed; nothing installed"
    finish
fi

# ---------------------------------------------------------------------------
# step 2: clean preconditions

say "== step 2: clean state"
step clean true
check_true clean_no_module "! lsmod | grep -q '^tbtun '" "no tbtun in lsmod"
check_absent clean_procfs /proc/net/tbtun
if [ -z "$(dkms_status)" ]; then ok clean_dkms_empty "dkms status empty"; else bad clean_dkms_empty "$(dkms_status | tr '\n' ';')"; fi
if ls -d /usr/src/"$MODULE"-* >/dev/null 2>&1; then
    bad clean_src "stale /usr/src/$MODULE-* present"
else
    ok clean_src "no /usr/src/$MODULE-*"
fi
if ls /lib/modules/"$REL"/updates/dkms/"$MODULE".ko* >/dev/null 2>&1; then
    bad clean_ko "stale updates/dkms ko present"
else
    ok clean_ko "no updates/dkms ko"
fi
if [ "$FAILED" -ne 0 ]; then
    say "clean state failed; see release section 4.5 before retrying"
    finish
fi

# ---------------------------------------------------------------------------
# step 3: install

say "== step 3: install"
step install "$REPO/scripts/install_dkms.sh" --kernel "$REL"
if [ "$STEP_RC" -ne 0 ]; then
    # install_dkms.sh installs and then modprobes; a non-zero rc after a
    # successful dkms install is a load failure, not a build failure.
    if dkms_status | grep -qF "$MODULE/$VER,"; then
        rollback modprobe
    else
        rollback build
    fi
    finish
fi

status=$(dkms_status)
if printf '%s\n' "$status" | grep -qF "$MODULE/$VER, $REL, $ARCH: installed"; then
    ok dkms_status "tbtun/$VER, $REL, $ARCH: installed"
else
    bad dkms_status "$(printf '%s' "$status" | tr '\n' ';')"
fi
got=$(modinfo -F version "$MODULE" 2>/dev/null || true)
if [ "$got" = "$VER" ]; then ok modinfo_version "$got"; else bad modinfo_version "got=${got:-none} expected=$VER"; fi
vm=$(modinfo -F vermagic "$MODULE" 2>/dev/null | awk '{print $1}')
if [ "$vm" = "$REL" ]; then ok modinfo_vermagic "$vm"; else bad modinfo_vermagic "got=${vm:-none} expected=$REL"; fi
if ls /lib/modules/"$REL"/updates/dkms/"$MODULE".ko* >/dev/null 2>&1; then
    ok installed_ko "$(ls /lib/modules/"$REL"/updates/dkms/"$MODULE".ko* | head -n 1)"
else
    bad installed_ko "no ko under /lib/modules/$REL/updates/dkms"
fi
sv=$(cat /sys/module/"$MODULE"/version 2>/dev/null || true)
if [ "$sv" = "$VER" ]; then ok sysfs_version "$sv"; else bad sysfs_version "got=${sv:-none} expected=$VER"; fi
klog >"$OUT/dmesg-after-install.txt"
if grep -qF "$MODULE $VER loaded" "$OUT/dmesg-after-install.txt"; then
    ok dmesg_loaded "$MODULE $VER loaded"
else
    bad dmesg_loaded "no 'tbtun $VER loaded' in kernel log"
fi
for f in config tunnels ports stats peers http; do
    check_present "procfs_$f" "/proc/net/tbtun/$f"
done
# DKMS 3.2.2 moves the build log from <module>/<ver>/build/make.log to
# <module>/kernel-<rel>-<arch>/log/make.log; accept either layout so the
# check does not fail on a successful build.
mklog=
for p in "/var/lib/dkms/$MODULE/$VER/build/make.log" \
    "/var/lib/dkms/$MODULE/kernel-$REL-$ARCH/log/make.log" \
    "/var/lib/dkms/$MODULE/$VER/$REL/$ARCH/log/make.log"; do
    if [ -f "$p" ]; then
        mklog=$p
        break
    fi
done
if [ -n "$mklog" ] && grep -qF '# exit code: 0' "$mklog"; then
    ok make_log "exit code 0 ($mklog)"
else
    bad make_log "make.log missing or non-zero exit"
fi
if [ "$FAILED" -ne 0 ]; then
    rollback install
    finish
fi

# ---------------------------------------------------------------------------
# step 4: config read-back

say "== step 4: config read-back"
step config sh -c "printf 'tunnel 1 role=near listen=127.0.0.1:19000 peer=203.0.113.9:443\n' > /proc/net/tbtun/config"
if [ "$STEP_RC" -ne 0 ]; then
    rollback modprobe
    finish
fi
cat /proc/net/tbtun/config >"$OUT/config-readback.txt" 2>/dev/null || true
cat /proc/net/tbtun/tunnels >"$OUT/tunnels-readback.txt" 2>/dev/null || true
ss -ltnH 'sport = :19000' >"$OUT/ss-19000.txt" 2>/dev/null || true
cfg=$(cat "$OUT/config-readback.txt" 2>/dev/null || true)
say "   config: $cfg"
for tok in "n=4" "m=1" "dup=1" "profile=http-kal" "keepalive=5000"; do
    case " $cfg " in
        *" $tok "*) ok "config_${tok%%=*}" "$tok" ;;
        *) bad "config_${tok%%=*}" "missing token $tok" ;;
    esac
done
if grep -qE '1 near listening 127\.0\.0\.1:19000' "$OUT/tunnels-readback.txt"; then
    ok tunnels_line "$(head -n 1 "$OUT/tunnels-readback.txt")"
else
    bad tunnels_line "no '1 near listening 127.0.0.1:19000'"
fi
if grep -qF '203.0.113.9:443' "$OUT/tunnels-readback.txt"; then
    ok tunnels_peer "203.0.113.9:443"
else
    bad tunnels_peer "peer missing"
fi
if grep -q . "$OUT/ss-19000.txt"; then ok listen_19000 "ss shows 127.0.0.1:19000"; else bad listen_19000 "no listener"; fi

step config-del sh -c "printf 'del 1\n' > /proc/net/tbtun/config"
if [ -z "$(cat /proc/net/tbtun/config 2>/dev/null)" ]; then ok config_empty "config empty after del"; else bad config_empty "config not empty"; fi
if ss -ltnH 'sport = :19000' 2>/dev/null | grep -q .; then bad listen_gone "19000 still listening"; else ok listen_gone "19000 free"; fi
if [ "$FAILED" -ne 0 ]; then
    rollback modprobe
    finish
fi

# ---------------------------------------------------------------------------
# step 5: rmmod

say "== step 5: rmmod"
step rmmod rmmod "$MODULE"
if [ "$STEP_RC" -ne 0 ]; then
    rollback rmmod-in-use
    finish
fi
check_true rmmod_lsmod "! lsmod | grep -q '^tbtun '" "module unloaded"
check_absent rmmod_procfs /proc/net/tbtun
klog >"$OUT/dmesg-after-rmmod.txt"
if grep -qF 'unloaded' "$OUT/dmesg-after-rmmod.txt"; then
    ok dmesg_unloaded "unloaded line present"
else
    bad dmesg_unloaded "no 'unloaded' line in kernel log"
fi
if tail -n 50 "$OUT/dmesg-after-rmmod.txt" |
    grep -qE 'BUG:|Oops|WARNING:|UBSAN:|general protection fault'; then
    bad rmmod_kernel "splat in the last 50 kernel lines"
else
    ok rmmod_kernel "clean"
fi
if [ "$FAILED" -ne 0 ]; then
    rollback rmmod-in-use
    finish
fi

# ---------------------------------------------------------------------------
# step 6: modprobe and read-back

say "== step 6: modprobe"
step modprobe modprobe "$MODULE"
if [ "$STEP_RC" -ne 0 ]; then
    rollback modprobe
    finish
fi
sv=$(cat /sys/module/"$MODULE"/version 2>/dev/null || true)
if [ "$sv" = "$VER" ]; then ok reload_version "$sv"; else bad reload_version "got=${sv:-none} expected=$VER"; fi
fn=$(modinfo -F filename "$MODULE" 2>/dev/null || true)
case "$fn" in
    */updates/dkms/"$MODULE".ko*) ok reload_filename "$fn" ;;
    *) bad reload_filename "got=${fn:-none}" ;;
esac
step config2 sh -c "printf 'tunnel 1 role=near listen=127.0.0.1:19000 peer=203.0.113.9:443\n' > /proc/net/tbtun/config"
cfg=$(cat /proc/net/tbtun/config 2>/dev/null || true)
say "   config: $cfg"
for tok in "n=4" "m=1" "dup=1" "profile=http-kal" "keepalive=5000"; do
    case " $cfg " in
        *" $tok "*) ok "reload_cfg_${tok%%=*}" "$tok" ;;
        *) bad "reload_cfg_${tok%%=*}" "missing token $tok" ;;
    esac
done
if ss -ltnH 'sport = :19000' 2>/dev/null | grep -q .; then ok reload_listen "listening"; else bad reload_listen "no listener"; fi
step config2-del sh -c "printf 'del 1\n' > /proc/net/tbtun/config"
if ss -ltnH 'sport = :19000' 2>/dev/null | grep -q .; then bad reload_listen_gone "still listening"; else ok reload_listen_gone "free"; fi
if [ "$FAILED" -ne 0 ]; then
    rollback modprobe
    finish
fi

# ---------------------------------------------------------------------------
# step 7: uninstall

say "== step 7: uninstall"
rmmod "$MODULE" 2>/dev/null || true
step uninstall "$REPO/scripts/install_dkms.sh" --uninstall
if [ "$STEP_RC" -ne 0 ]; then
    rollback uninstall-partial
    finish
fi

if [ -z "$(dkms_status)" ]; then ok uninstall_dkms_empty "dkms status empty"; else bad uninstall_dkms_empty "$(dkms_status | tr '\n' ';')"; fi
if ls -d /usr/src/"$MODULE"-* >/dev/null 2>&1; then
    bad uninstall_src "stale /usr/src/$MODULE-*"
else
    ok uninstall_src "no /usr/src/$MODULE-*"
fi
for p in updates/dkms updates extra; do
    if ls /lib/modules/"$REL"/"$p"/"$MODULE".ko* >/dev/null 2>&1; then
        bad "uninstall_ko_${p//\//_}" "stale ko under $p"
    else
        ok "uninstall_ko_${p//\//_}" "none under $p"
    fi
done
if modinfo "$MODULE" >/dev/null 2>&1; then
    bad uninstall_modinfo "modinfo still resolves"
else
    ok uninstall_modinfo "modinfo resolves nothing"
fi
if grep -qF "$MODULE" /lib/modules/"$REL"/modules.dep 2>/dev/null; then
    bad uninstall_modules_dep "modules.dep still mentions $MODULE"
else
    ok uninstall_modules_dep "modules.dep clean"
fi
check_absent uninstall_procfs /proc/net/tbtun
check_true uninstall_lsmod "! lsmod | grep -q '^tbtun '" "module unloaded"
residuals=$(find /var/lib/dkms -maxdepth 2 -name "$MODULE*" 2>/dev/null || true)
if [ -z "$residuals" ]; then ok uninstall_state_dir "no /var/lib/dkms/$MODULE*"; else bad uninstall_state_dir "$residuals"; fi

if [ "$FAILED" -ne 0 ]; then
    rollback uninstall-partial
fi
finish
