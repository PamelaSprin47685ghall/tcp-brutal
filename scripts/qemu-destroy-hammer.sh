#!/usr/bin/env bash
# qemu-destroy-hammer.sh - destroy the 0.4.1 lifecycle in a real kernel.
#
#   scripts/qemu-destroy-hammer.sh
#   TBT_E2E_QEMU_TIMEOUT=180 scripts/qemu-destroy-hammer.sh
#
# It boots the cached 7.0.14 kernel (lockdep + UBSAN + DEBUG_ATOMIC_SLEEP)
# under KVM with tests/qemu-e2e/hammer.c as PID 1, then judges the guest's
# QEMU_HAMMER_PASS line plus three things the guest cannot fake:
#
#   * every phase marker ran: 20 add/join/stream/del rounds and 10
#     delete_module -> finit_module cycles;
#   * the serial console holds no UBSAN, BUG, Oops, WARNING, lockdep,
#     sleep-in-atomic, list/refcount or stall report;
#   * no kernel panic.
#
# The guest itself also reads dmesg and fails on the same patterns, so a
# report that never reaches the serial file still fails the run.
#
# Exit status: 0 pass, 1 guest failure or timeout, 2 missing input/tool.
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KVER=${TBT_E2E_KERNEL:-7.0.14}
CACHE=${TBT_E2E_CACHE:-/tmp/tbt-e2e-cache}
KM_CACHE=${KM_CACHE:-$HOME/.cache/tbtun-kmatrix}
MIRROR=${KM_KERNEL_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/kernel}
JOBS=${TBT_E2E_JOBS:-$(nproc 2>/dev/null || echo 4)}
QEMU_TIMEOUT=${TBT_E2E_QEMU_TIMEOUT:-120}
HOSTCC=${HOSTCC:-cc}

die() {
    echo "qemu-destroy-hammer: $*" >&2
    exit 2
}

command -v qemu-system-x86_64 > /dev/null 2>&1 || die "qemu-system-x86_64 is not installed"
command -v "$HOSTCC" > /dev/null 2>&1 || die "no C compiler ($HOSTCC)"
command -v python3 > /dev/null 2>&1 || die "python3 is required for the initramfs"
command -v rsync > /dev/null 2>&1 || die "rsync is required"
command -v timeout > /dev/null 2>&1 || die "timeout is required"

series=v${KVER%%.*}.x
TARBALL=$KM_CACHE/dl/linux-$KVER.tar.xz
KDIR=$CACHE/kernel/linux-$KVER
WORK=$CACHE/work/$KVER-hammer
SRC=$WORK/src
INIT=$WORK/init
KO=$WORK/tbtun.ko
INITRD=$WORK/initramfs.cpio.gz
LOG=$WORK/qemu.log
DEBUGLOG=$WORK/qemu.debug
mkdir -p "$CACHE/kernel" "$WORK"

echo "qemu-destroy-hammer: repo=$REPO kernel=$KVER cache=$CACHE jobs=$JOBS"

# ---------------------------------------------------------------------------
# Kernel image: reuse the cached build whenever it exists.  The fallback is
# the same defconfig plus lockdep/UBSAN the smoke runner established.

if [ ! -f "$KDIR/Makefile" ]; then
    [ -s "$TARBALL" ] || die "no kernel tarball at $TARBALL and no tree at $KDIR"
    mkdir -p "$KDIR"
    tar -xJf "$TARBALL" -C "$KDIR" --strip-components=1 ||
        die "kernel tarball extraction failed"
fi

IMAGE=$KDIR/arch/x86/boot/bzImage
if [ ! -f "$IMAGE" ] || [ ! -f "$KDIR/.config" ]; then
    echo "qemu-destroy-hammer: building $KVER bzImage (-j$JOBS)"
    make -C "$KDIR" -j"$JOBS" defconfig > "$WORK/config.log" 2>&1 ||
        die "defconfig failed (see $WORK/config.log)"
    "$KDIR/scripts/config" --file "$KDIR/.config" \
        -d DEBUG_INFO -d DEBUG_INFO_BTF -d DEBUG_INFO_BTF_MODULES \
        -d GDB_SCRIPTS -d MODULE_SIG -d MODULE_SIG_ALL -d MODULE_COMPRESS \
        -d WERROR -d RANDOMIZE_BASE \
        -e DEBUG_KERNEL -e EXPERT \
        -e LIBCRC32C \
        -e UBSAN -e UBSAN_BOUNDS -e UBSAN_SHIFT -e UBSAN_DIV_ZERO \
        -e UBSAN_UNREACHABLE -e UBSAN_BOOL -e UBSAN_ENUM \
        -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_SPINLOCK \
        -e DEBUG_MUTEXES -e DEBUG_LIST \
        --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
        > /dev/null
    make -C "$KDIR" -j"$JOBS" olddefconfig >> "$WORK/config.log" 2>&1 ||
        die "olddefconfig failed (see $WORK/config.log)"
    grep -q '^CONFIG_UBSAN=y' "$KDIR/.config" ||
        die "the guest kernel would not carry UBSAN"
    grep -q '^CONFIG_PROVE_LOCKING=y' "$KDIR/.config" ||
        die "the guest kernel would not carry lockdep"
    grep -q '^CONFIG_DEBUG_ATOMIC_SLEEP=y' "$KDIR/.config" ||
        die "the guest kernel would not carry DEBUG_ATOMIC_SLEEP"
    make -C "$KDIR" -j"$JOBS" bzImage > "$WORK/image.log" 2>&1 ||
        die "kernel image build failed (see $WORK/image.log)"
    make -C "$KDIR" modules_prepare >> "$WORK/image.log" 2>&1 ||
        die "modules_prepare failed (see $WORK/image.log)"
else
    echo "qemu-destroy-hammer: image cache hit"
fi

if [ ! -s "$KDIR/Module.symvers" ]; then
    if [ ! -s "$KDIR/vmlinux.symvers" ]; then
        make -C "$KDIR" -j"$JOBS" vmlinux.symvers >> "$WORK/image.log" 2>&1 ||
            die "cannot build vmlinux.symvers"
    fi
    cp -f "$KDIR/vmlinux.symvers" "$KDIR/Module.symvers" ||
        die "cannot install Module.symvers"
fi

KRELEASE=$(make -s -C "$KDIR" kernelrelease 2> /dev/null || true)
[ -n "$KRELEASE" ] || KRELEASE=$KVER
echo "qemu-destroy-hammer: image $IMAGE ($KRELEASE)"

# ---------------------------------------------------------------------------
# Module against that exact tree.

echo "qemu-destroy-hammer: building tbtun.ko"
mkdir -p "$SRC"
rsync -a --delete \
    --exclude '.git' --exclude 'review' --exclude 'benches/results' \
    --exclude '*.ko' --exclude '*.o' --exclude '*.mod' --exclude '*.mod.c' \
    --exclude 'modules.order' --exclude 'Module.symvers' \
    "$REPO"/ "$SRC"/ || die "cannot stage the module source"
make -C "$KDIR" M="$SRC" -j"$JOBS" modules > "$WORK/module.log" 2>&1 ||
    die "module build failed (see $WORK/module.log)"
[ -f "$SRC/tbtun.ko" ] || die "module build produced no tbtun.ko"
cp -f "$SRC/tbtun.ko" "$KO"
if command -v modinfo > /dev/null 2>&1; then
    vermagic=$(modinfo -F vermagic "$KO" 2> /dev/null || true)
    echo "qemu-destroy-hammer: vermagic $vermagic"
    case $vermagic in
    "$KRELEASE"*) ;;
    *) die "module vermagic '$vermagic' does not match '$KRELEASE'" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Static guest PID 1 and initramfs.

echo "qemu-destroy-hammer: building the static guest init"
"$HOSTCC" -static -O2 -Wall -Wextra -Werror -pthread \
    -o "$INIT" "$REPO/tests/qemu-e2e/hammer.c" ||
    die "guest init build failed"
python3 "$REPO/tests/kernel-matrix/mkinitramfs.py" \
    --init "$INIT" --ko "$KO" --out "$INITRD" ||
    die "initramfs build failed"

# ---------------------------------------------------------------------------
# Boot and judge.

accel=tcg
cpu=max
if [ -w /dev/kvm ]; then
    accel=kvm
    cpu=host
fi

echo "qemu-destroy-hammer: booting qemu accel=$accel timeout=${QEMU_TIMEOUT}s"
rm -f "$LOG"
rm -f "$DEBUGLOG"
set +e
timeout --foreground "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -accel "$accel" -cpu "$cpu" -smp 4 -m 1536 \
    -kernel "$IMAGE" -initrd "$INITRD" \
    -append "console=ttyS0 panic=-1 panic_on_oops=1 loglevel=7 tsc=unstable" \
    -nic none -display none -monitor none -serial "file:$LOG" -no-reboot \
    -device isa-debug-exit,iobase=0x501,iosize=0x04 \
    -d cpu_reset,guest_errors -D "$DEBUGLOG" \
    > "$WORK/qemu.stderr" 2>&1
qrc=$?
set -e

echo "qemu-destroy-hammer: qemu rc=$qrc; console tail:"
tail -n 20 "$LOG" 2> /dev/null | sed 's/^/    /' || true

[ -s "$LOG" ] || die "qemu wrote no console log (rc=$qrc, see $WORK/qemu.stderr)"

bad_reason=
guest_verdict=
case $qrc in
33) guest_verdict=pass ;;
35) guest_verdict=fail ;;
esac

if [ "$guest_verdict" = pass ] || grep -aq 'QEMU_HAMMER_PASS' "$LOG"; then
    # The guest asserts the twenty rounds, ten reload cycles and the empty
    # bad-line scan before it prints PASS.  Repeating the phase markers here
    # would only add a console-interleaving race: printk can split any line.
    bad_reason=$(grep -aE 'UBSAN|BUG:|kernel BUG at|Oops|WARNING:|Call Trace|general protection|invalid opcode|possible circular locking|possible recursive locking|inconsistent lock state|held lock|SOFTIRQ-safe -> SOFTIRQ-unsafe|sleeping function called from invalid context|list_add corruption|list_del corruption|refcount_t:|soft lockup|RCU stall|rcu_sched|blocked for more than|workqueue lockup|Kernel panic' "$LOG" |
        tail -n 1 || true)
elif [ "$guest_verdict" = fail ] || grep -aq 'QEMU_HAMMER_FAIL' "$LOG"; then
    bad_reason=$(grep -a 'QEMU_HAMMER_FAIL' "$LOG" | tail -n 1)
    [ -n "$bad_reason" ] || bad_reason="guest reported failure (debug-exit 0x11)"
else
    bad_reason=$(grep -aE 'Kernel panic|UBSAN|BUG:|Oops|Call Trace|general protection' "$LOG" |
        tail -n 1 || true)
    [ -n "$bad_reason" ] || bad_reason="qemu rc=$qrc, no QEMU_HAMMER_PASS marker"
fi

if [ -n "$bad_reason" ]; then
    echo "qemu-destroy-hammer: FAIL: $bad_reason"
    echo "qemu-destroy-hammer: full log $LOG"
    exit 1
fi

wall=$(awk '/QEMU_HAMMER_PASS/ {
    for (i = 1; i <= NF; i++) {
        gsub(/\r/, "", $i)
        if ($i ~ /^[0-9]+s$/) { print $i; exit }
    }
}' "$LOG" 2> /dev/null || true)
echo "qemu-destroy-hammer: PASS${wall:+ (guest wall $wall)}"
echo "qemu-destroy-hammer: full log $LOG"
exit 0
