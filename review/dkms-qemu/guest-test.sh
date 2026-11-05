#!/bin/bash
# guest-test.sh - the DKMS acceptance flow, run as root inside the guest chroot.
#
# Everything here happens on the guest kernel: the module is compiled by the
# real dkms script against /lib/modules/$(uname -r)/build, installed into the
# guest's module tree, loaded with modprobe, exercised through /proc/net/tbtun,
# unloaded with rmmod, and removed again by `install_dkms.sh --uninstall`.
#
# Every step prints its command line and `DKMS_STEP <name> rc=<n>`; every
# assertion prints `DKMS_CHECK <name> PASS|FAIL`. The final line is
# `DKMS_RESULT PASS|FAIL ...`, which is what the host runner judges.
#
# Output is both the console and /out/<name>.log (a read-write 9p export of
# review/dkms-qemu/artifacts on the host).
set -u
set -o pipefail

REL=$(uname -r)
REPO=/home/kunweiz/tbtun-worktrees/dkms-qemu
CACHE=/home/kunweiz/.cache/tbtun-build
OUT=/out
FAILED=0
STEP_RC=0

export PATH="/opt/dkms/bin:$CACHE/pahole/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$CACHE/pahole/usr/lib"
export HOME=/root
export TERM=dumb
# The guest is root and the repository belongs to uid 1000.
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=safe.directory
export GIT_CONFIG_VALUE_0="$REPO"

if ! : > "$OUT/.write-test" 2> /dev/null; then
    OUT=/tmp
    echo "guest-test: warning: /out is not writable; evidence stays on the console"
fi
rm -f "$OUT/.write-test"

say() {
    printf '%s\n' "$*"
}

ok() {
    printf 'DKMS_CHECK %s PASS%s\n' "$1" "${2:+ ($2)}"
}

bad() {
    printf 'DKMS_CHECK %s FAIL%s\n' "$1" "${2:+ ($2)}"
    FAILED=$((FAILED + 1))
}

# capture <count-failure> <name> <command...>
capture() {
    local count=$1 name=$2
    shift 2
    local t0 rc=0
    t0=$(date +%s)
    printf '\n=== STEP %s ===\n$ %s\n' "$name" "$(printf '%q ' "$@")"
    "$@" > /tmp/step.out 2>&1 || rc=$?
    cat /tmp/step.out
    cp /tmp/step.out "$OUT/$name.log" 2> /dev/null || true
    printf 'DKMS_STEP %s rc=%d secs=%d\n' "$name" "$rc" \
        "$(( $(date +%s) - t0 ))"
    STEP_RC=$rc
    if [ "$rc" -ne 0 ] && [ "$count" -ne 0 ]; then
        FAILED=$((FAILED + 1))
    fi
    return 0
}

step() {
    capture 1 "$@"
}

snapshot() {
    capture 0 "$@"
}

check_rc0() {
    local name=$1 rc=0
    shift
    "$@" > /tmp/check.out 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        ok "$name"
    else
        bad "$name" "rc=$rc"
    fi
    sed 's/^/    /' /tmp/check.out
    cp /tmp/check.out "$OUT/check-$name.log" 2> /dev/null || true
}

check_nonzero() {
    local name=$1 rc=0
    shift
    "$@" > /tmp/check.out 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        ok "$name" "rc=$rc as expected"
    else
        bad "$name" "rc=0, expected failure"
    fi
    sed 's/^/    /' /tmp/check.out
    cp /tmp/check.out "$OUT/check-$name.log" 2> /dev/null || true
}

check_absent() {
    if [ -e "$2" ]; then
        bad "$1" "exists: $2"
    else
        ok "$1" "absent: $2"
    fi
}

check_present() {
    if [ -e "$2" ]; then
        ok "$1" "present: $2"
    else
        bad "$1" "missing: $2"
    fi
}

check_nonempty() {
    if [ -s "$2" ]; then
        ok "$1" "non-empty: $2"
    else
        bad "$1" "empty or missing: $2"
    fi
}

check_glob_absent() {
    local hit
    # shellcheck disable=SC2086
    hit=$(compgen -G "$2" 2> /dev/null | head -3)
    if [ -n "$hit" ]; then
        bad "$1" "leftovers: $(printf '%s ' $hit)"
    else
        ok "$1" "no match for $2"
    fi
}

check_grep() {
    if grep -q -F -e "$2" "$3" 2> /dev/null; then
        ok "$1" "matched in $3"
    else
        bad "$1" "no '$2' in $3"
    fi
}

check_not_grep() {
    if grep -q -F -e "$2" "$3" 2> /dev/null; then
        bad "$1" "'$2' still in $3"
    else
        ok "$1" "no '$2' in $3"
    fi
}

check_grep_re() {
    if grep -q -E -e "$2" "$3" 2> /dev/null; then
        ok "$1" "matched in $3"
    else
        bad "$1" "no /$2/ in $3"
    fi
}

check_not_grep_re() {
    if grep -q -E -e "$2" "$3" 2> /dev/null; then
        bad "$1" "/$2/ still in $3"
    else
        ok "$1" "no /$2/ in $3"
    fi
}

say "guest-test: kernel $REL, dkms $(dkms --version 2>&1), $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "guest-test: repo $REPO, evidence $OUT"

# --- environment -----------------------------------------------------------
step env-uname uname -a
step env-proc-version cat /proc/version
step env-cmdline cat /proc/cmdline
step env-os-release cat /etc/os-release
step env-toolchain bash -c 'gcc --version | head -1; make --version | head -1;
    pahole --version; zstd --version; depmod --version | head -1;
    dkms --version; sha256sum /opt/dkms/bin/dkms'
step env-build-link ls -l "/lib/modules/$REL/build"
step env-build-makefile test -f "/lib/modules/$REL/build/Makefile"
step env-modules-tree bash -c "ls '/lib/modules/$REL' | head -20;
    echo kernel modules: \$(find '/lib/modules/$REL/kernel' -name '*.ko*' | wc -l)"
step env-writable-dirs bash -c 'ls -ld /usr/src /var/lib/dkms;
    touch /usr/src/.w && rm /usr/src/.w && echo "/usr/src writable"'
step env-repo bash -c "git -C '$REPO' rev-parse HEAD;
    git -C '$REPO' status --porcelain | head -20"
step env-meminfo bash -c 'head -3 /proc/meminfo; nproc'
# Nothing in the tree is a prebuilt module: whatever the guest loads, dkms
# compiled it here.
check_absent repo-no-prebuilt-module "$REPO/tbtun.ko"

# The state before anything is installed. dkms status may exit 0 with no
# output, so the absence of a tbtun line is checked separately.
snapshot pre-dkms-status dkms status
check_not_grep pre-dkms-status-empty tbtun "$OUT/pre-dkms-status.log"
check_absent pre-no-proc /proc/net/tbtun
check_glob_absent pre-no-module-file "/lib/modules/$REL/updates/dkms/tbtun.ko*"
check_glob_absent pre-no-src "/usr/src/tbtun-*"
check_not_grep pre-no-dep-entry tbtun "/lib/modules/$REL/modules.dep"

# --- install ---------------------------------------------------------------
say
say "### phase 1: scripts/install_dkms.sh --kernel $REL"
step install "$REPO/scripts/install_dkms.sh" --kernel "$REL"
INSTALL_RC=$STEP_RC

snapshot dkms-status-installed dkms status
check_grep dkms-entry-installed "tbtun/0.0.1" "$OUT/dkms-status-installed.log"
step installed-module-files bash -c "ls -l '/lib/modules/$REL/updates/dkms/';
    sha256sum '/lib/modules/$REL/updates/dkms/tbtun.ko'*"
snapshot installed-modinfo modinfo tbtun
check_grep_re modinfo-name '^name:[[:space:]]+tbtun$' "$OUT/installed-modinfo.log"
check_grep_re modinfo-version '^version:[[:space:]]+0\.0\.1$' "$OUT/installed-modinfo.log"
check_grep_re modinfo-vermagic "^vermagic:[[:space:]]+$REL" "$OUT/installed-modinfo.log"
check_grep_re modinfo-filename \
    "/lib/modules/$REL/updates/dkms/tbtun\\.ko\\.zst$" "$OUT/installed-modinfo.log"
snapshot installed-lsmod bash -c 'grep "^tbtun " /proc/modules; lsmod | head -5'
check_grep_re loaded-in-proc-modules '^tbtun ' "$OUT/installed-lsmod.log"
check_present proc-dir /proc/net/tbtun
check_present proc-config /proc/net/tbtun/config
check_present proc-tunnels /proc/net/tbtun/tunnels
check_present proc-ports /proc/net/tbtun/ports
snapshot proc-ports-empty cat /proc/net/tbtun/ports

# The module was built here, not copied in: keep the installed artifact and
# the dkms build log as evidence, before the uninstall removes them.
step artifact-module bash -c "cp -v '/lib/modules/$REL/updates/dkms/'tbtun.ko* '$OUT/tbtun.ko.zst'"
step artifact-make-log bash -c "cp -v /var/lib/dkms/tbtun/0.0.1/$REL/*/log/make.log '$OUT/make.log'"
check_grep make-log-compiled 'CC [M]  tbtun/tbt_core.o' "$OUT/make.log"
check_grep make-log-btf 'BTF [M] tbtun.ko' "$OUT/make.log"
check_grep make-log-exit0 '# exit code: 0' "$OUT/make.log"
snapshot dkms-tree bash -c "find /var/lib/dkms -maxdepth 4 | sort | head -40;
    ls -l /usr/src"

# --- configuration round-trip ---------------------------------------------
say
say "### phase 2: minimal tunnel configuration through /proc/net/tbtun"
step config-write bash -c "printf '%s\n' 'tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443' > /proc/net/tbtun/config"
snapshot config-read cat /proc/net/tbtun/config
check_grep config-roundtrip \
    "tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443" \
    /proc/net/tbtun/config
snapshot tunnels-read cat /proc/net/tbtun/tunnels
check_grep tunnels-listen "127.0.0.1:9000" "$OUT/tunnels-read.log"
check_grep tunnels-peer "203.0.113.9:443" "$OUT/tunnels-read.log"
check_grep_re tcp-listener ':2328 [0-9A-F]+:0000 0A' /proc/net/tcp
snapshot ports-read cat /proc/net/tbtun/ports
check_grep ports-readback "ports " "$OUT/ports-read.log"
snapshot stats-read head -20 /proc/net/tbtun/stats

# --- rmmod / modprobe ------------------------------------------------------
say
say "### phase 3: del, rmmod, modprobe"
step config-del bash -c "printf 'del 1\n' > /proc/net/tbtun/config"
step config-empty bash -c 'test -z "$(cat /proc/net/tbtun/config)"'
check_not_grep_re tcp-listener-gone ':2328 [0-9A-F]+:0000 0A' /proc/net/tcp
step rmmod rmmod tbtun
check_absent proc-dir-gone /proc/net/tbtun
check_not_grep_re modules-gone '^tbtun ' /proc/modules
step modprobe modprobe tbtun
check_present proc-dir-back /proc/net/tbtun
snapshot modprobe-lsmod bash -c 'grep "^tbtun " /proc/modules; modinfo tbtun'
check_grep_re modprobe-vermagic "^vermagic:[[:space:]]+$REL" "$OUT/modprobe-lsmod.log"
step config-write-again bash -c "printf '%s\n' 'tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443' > /proc/net/tbtun/config"
check_grep config-roundtrip-again \
    "tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443" \
    /proc/net/tbtun/config
step config-del-again bash -c "printf 'del 1\n' > /proc/net/tbtun/config"
step rmmod-again rmmod tbtun
check_absent proc-dir-gone-2 /proc/net/tbtun

# --- uninstall -------------------------------------------------------------
say
say "### phase 4: scripts/install_dkms.sh --uninstall"
step uninstall "$REPO/scripts/install_dkms.sh" --uninstall
UNINSTALL_RC=$STEP_RC

snapshot dkms-status-after-uninstall dkms status
check_not_grep dkms-entry-gone tbtun "$OUT/dkms-status-after-uninstall.log"
check_glob_absent src-gone "/usr/src/tbtun-*"
check_glob_absent module-file-gone "/lib/modules/$REL/updates/dkms/tbtun.ko*"
check_glob_absent module-file-extra-gone "/lib/modules/$REL/extra/tbtun.ko*"
check_absent proc-dir-gone-3 /proc/net/tbtun
check_not_grep_re modules-gone-2 '^tbtun ' /proc/modules
check_not_grep dep-entry-gone tbtun "/lib/modules/$REL/modules.dep"
check_nonzero modinfo-gone modinfo tbtun
snapshot leftovers bash -c "ls -la '/lib/modules/$REL/updates/dkms';
    ls -la /usr/src; find /var/lib/dkms -maxdepth 3; ls /lib/modules/$REL | head"
snapshot dmesg-scan bash -c "dmesg | grep -Ei 'tbtun|taint|BUG:|Oops|WARNING:' | tail -40"

# --- verdict ---------------------------------------------------------------
say
if [ "$INSTALL_RC" -eq 0 ] && [ "$UNINSTALL_RC" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    say "DKMS_RESULT PASS kernel=$REL install_rc=$INSTALL_RC uninstall_rc=$UNINSTALL_RC"
else
    say "DKMS_RESULT FAIL kernel=$REL install_rc=$INSTALL_RC uninstall_rc=$UNINSTALL_RC failed=$FAILED"
fi
say "guest-test: done at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
exit 0
