#!/usr/bin/env bash
# qemu-e2e-smoke.sh - boot a real kernel in QEMU, load tbtun.ko, and drive the
# 0.4.1 data plane through the operator surface before any real machine does.
#
#   scripts/qemu-e2e-smoke.sh                 # 7.0.14, cached under ~/.cache
#   scripts/qemu-e2e-smoke.sh --kernel 6.12.116
#   TBT_E2E_QEMU_TIMEOUT=90 scripts/qemu-e2e-smoke.sh
#
# What it proves, all inside one guest on 127.0.0.1:
#   * a 256-carrier SC-LDPC near+far pair comes up, and three sequential
#     streams each carry 128 KiB byte-exact with streams_active=0 in between;
#   * a concurrent second SC-LDPC stream is refused and the first one survives;
#   * a rebuilt n=4 m=4 grouped pair carries two concurrent streams (two sids);
#   * a 64-carrier http-kal FEC pair (8 x (4+4)) carries two concurrent
#     streams and both ends report fbk_tx and fbk_rx, so the 340 B control
#     encoder and the lazy replay ring run on both sides;
#   * del + delete_module return inside the deadline (no D state), the module
#     leaves /proc/modules, and the kernel log gained no UBSAN, BUG, Oops,
#     WARNING, lockdep or stall report.
#
# The guest prints QEMU_E2E_PASS (or QEMU_E2E_FAIL <reason>) as its last line;
# this runner judges that marker plus Kernel panic, not qemu's exit status.
#
# Build cache: <cache>/kernel/linux-<version> holds the kernel tree, image and
# config; <cache>/work/<version> holds the module copy, the static guest init,
# the initramfs and the console log.  A second run with the same version skips
# the kernel build and only rebuilds the module and the guest.
#
# Exit status: 0 pass, 1 guest failure or timeout, 2 missing input/tool.
set -euo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KVER=${TBT_E2E_KERNEL:-7.0.14}
CACHE=${TBT_E2E_CACHE:-$HOME/.cache/tbtun-e2e}
KM_CACHE=${KM_CACHE:-$HOME/.cache/tbtun-kmatrix}
MIRROR=${KM_KERNEL_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/kernel}
JOBS=${TBT_E2E_JOBS:-$(nproc 2>/dev/null || echo 4)}
QEMU_TIMEOUT=${TBT_E2E_QEMU_TIMEOUT:-75}
HOSTCC=${HOSTCC:-cc}

die() {
    echo "qemu-e2e-smoke: $*" >&2
    exit 2
}

usage() {
    sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case $1 in
    --kernel) KVER=$2; shift 2 ;;
    --cache) CACHE=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --timeout) QEMU_TIMEOUT=$2; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
done

[ "$JOBS" -ge 1 ] || die "--jobs must be >= 1"
[ "$QEMU_TIMEOUT" -ge 20 ] || die "--timeout must be >= 20"

command -v qemu-system-x86_64 > /dev/null 2>&1 || die "qemu-system-x86_64 is not installed"
command -v "$HOSTCC" > /dev/null 2>&1 || die "no C compiler ($HOSTCC)"
command -v python3 > /dev/null 2>&1 || die "python3 is required for the initramfs"
command -v rsync > /dev/null 2>&1 || die "rsync is required for the module source copy"
command -v timeout > /dev/null 2>&1 || die "timeout is required"

series=v${KVER%%.*}.x
TARBALL=$KM_CACHE/dl/linux-$KVER.tar.xz
KDIR=$CACHE/kernel/linux-$KVER
WORK=$CACHE/work/$KVER
SRC=$WORK/src
INIT=$WORK/init
KO=$WORK/tbtun.ko
INITRD=$WORK/initramfs.cpio.gz
LOG=$WORK/qemu.log
mkdir -p "$CACHE/kernel" "$WORK"

echo "qemu-e2e-smoke: repo=$REPO kernel=$KVER cache=$CACHE jobs=$JOBS"

# ---------------------------------------------------------------------------
# Kernel tarball and image.  The tarball is the one kernel-matrix.sh already
# caches; the tree is kept between runs so a second run pays only the module.

if [ ! -s "$TARBALL" ]; then
    mkdir -p "$(dirname "$TARBALL")"
    echo "qemu-e2e-smoke: fetching $MIRROR/$series/linux-$KVER.tar.xz"
    curl -fsSL --retry 3 -o "$TARBALL.part" "$MIRROR/$series/linux-$KVER.tar.xz" ||
        die "kernel tarball download failed"
    mv "$TARBALL.part" "$TARBALL"
fi

if [ ! -f "$KDIR/Makefile" ]; then
    echo "qemu-e2e-smoke: extracting $(basename "$TARBALL")"
    mkdir -p "$KDIR"
    tar -xJf "$TARBALL" -C "$KDIR" --strip-components=1 ||
        die "kernel tarball extraction failed"
fi

IMAGE=$KDIR/arch/x86/boot/bzImage
if [ ! -f "$IMAGE" ] || [ ! -f "$KDIR/.config" ]; then
    echo "qemu-e2e-smoke: building $KVER defconfig bzImage (-j$JOBS)"
    make -C "$KDIR" -j"$JOBS" defconfig > "$WORK/config.log" 2>&1 ||
        die "defconfig failed (see $WORK/config.log)"
    # A load/fault check wants the sanitizer and lockdep reports, not a small
    # build: UBSAN names the wrong shift/index, lockdep names a self-deadlock
    # before the data plane observes it as a stall.
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
    if ! grep -q '^CONFIG_UBSAN=y' "$KDIR/.config"; then
        die "the guest kernel would not carry UBSAN (check $WORK/config.log)"
    fi
    if ! grep -q '^CONFIG_MODULES=y' "$KDIR/.config" ||
        ! grep -q '^CONFIG_INET=y' "$KDIR/.config"; then
        die "the guest kernel lacks modules or INET (check $WORK/config.log)"
    fi
    make -C "$KDIR" -j"$JOBS" bzImage > "$WORK/image.log" 2>&1 ||
        die "kernel image build failed (see $WORK/image.log)"
    make -C "$KDIR" modules_prepare >> "$WORK/image.log" 2>&1 ||
        die "modules_prepare failed (see $WORK/image.log)"
else
    echo "qemu-e2e-smoke: image cache hit"
fi

# 7.0 writes vmlinux.symvers during the image build and the external-module
# modpost reads Module.symvers.  They share one format, and this kernel has no
# MODVERSIONS, so the copy is exact rather than a compatibility shim.
if [ ! -s "$KDIR/Module.symvers" ]; then
    if [ ! -s "$KDIR/vmlinux.symvers" ]; then
        make -C "$KDIR" -j"$JOBS" vmlinux.symvers >> "$WORK/image.log" 2>&1 ||
            die "cannot build vmlinux.symvers (see $WORK/image.log)"
    fi
    cp -f "$KDIR/vmlinux.symvers" "$KDIR/Module.symvers" ||
        die "cannot install Module.symvers"
fi

KRELEASE=$(make -s -C "$KDIR" kernelrelease 2> /dev/null || true)
[ -n "$KRELEASE" ] || KRELEASE=$KVER
echo "qemu-e2e-smoke: image $IMAGE ($KRELEASE)"

# ---------------------------------------------------------------------------
# Module against that exact tree.  The source copy keeps the worktree clean and
# the exclude list keeps the build small.

echo "qemu-e2e-smoke: building tbtun.ko against $KRELEASE"
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
    echo "qemu-e2e-smoke: vermagic $vermagic"
    case $vermagic in
    "$KRELEASE"*) ;;
    *) die "module vermagic '$vermagic' does not match '$KRELEASE'" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Guest PID 1, static so the image needs no libc payload.  The kernel-matrix
# packer already writes the newc image with /proc and /dev/console.

echo "qemu-e2e-smoke: building the static guest init"
"$HOSTCC" -static -O2 -Wall -Wextra -Werror \
    -o "$INIT" "$REPO/tests/qemu-e2e/guest.c" ||
    die "guest init build failed"
python3 "$REPO/tests/kernel-matrix/mkinitramfs.py" \
    --init "$INIT" --ko "$KO" --out "$INITRD" ||
    die "initramfs build failed"

# ---------------------------------------------------------------------------
# Boot and judge.

accel=tcg
cpu=max
smp=4
if [ -w /dev/kvm ]; then
    accel=kvm
    cpu=host
fi

echo "qemu-e2e-smoke: booting qemu accel=$accel timeout=${QEMU_TIMEOUT}s"
rm -f "$LOG"
set +e
timeout --foreground "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -accel "$accel" -cpu "$cpu" -smp "$smp" -m 1536 \
    -kernel "$IMAGE" -initrd "$INITRD" \
    -append "console=ttyS0 panic=-1 panic_on_oops=1 loglevel=7 tsc=unstable" \
    -nic none -display none -monitor none -serial "file:$LOG" -no-reboot \
    > "$WORK/qemu.stderr" 2>&1
qrc=$?
set -e

echo "qemu-e2e-smoke: qemu rc=$qrc; console tail:"
tail -n 25 "$LOG" 2> /dev/null | sed 's/^/    /' || true

if [ ! -s "$LOG" ]; then
    die "qemu wrote no console log (rc=$qrc, see $WORK/qemu.stderr)"
fi

evidence=(
    "QEMU_E2E_STREAM sclc-seq-1"
    "QEMU_E2E_STREAM sclc-seq-2"
    "QEMU_E2E_STREAM sclc-seq-3"
    "QEMU_E2E_REJECT sclc-second"
    "QEMU_E2E_STREAM sclc-after-reject"
    "QEMU_E2E_MULTISTREAM 2 streams ok"
    "QEMU_E2E_DEL grouped ok"
    "QEMU_E2E_UP kal64 up=64"
    "QEMU_E2E_STREAM kal64-bidir ok"
    "QEMU_E2E_FBK both-ends ok"
    "QEMU_E2E_DEL kal64 ok"
    "QEMU_E2E_UNLOAD ok"
)

bad_reason=
if grep -aq '^QEMU_E2E_PASS' "$LOG"; then
    # The guest owns the assertions; these markers prove every phase ran, so
    # an early poweroff cannot be mistaken for a pass.
    for want in "${evidence[@]}"; do
        grep -aqF "$want" "$LOG" ||
            bad_reason="missing evidence: $want"
    done
    if [ -z "$bad_reason" ] && grep -aq 'Kernel panic' "$LOG"; then
        bad_reason="Kernel panic in the console log"
    fi
elif grep -aq '^QEMU_E2E_FAIL' "$LOG"; then
    bad_reason=$(grep -a '^QEMU_E2E_FAIL' "$LOG" | tail -n 1)
else
    bad_reason=$(grep -aE 'Kernel panic|UBSAN|BUG:|Oops|Call Trace|general protection' "$LOG" |
        tail -n 1 || true)
    [ -n "$bad_reason" ] || bad_reason="qemu rc=$qrc, no QEMU_E2E_PASS marker"
fi

if [ -n "$bad_reason" ]; then
    echo "qemu-e2e-smoke: FAIL: $bad_reason"
    echo "qemu-e2e-smoke: full log $LOG"
    exit 1
fi

wall=$(awk '/^QEMU_E2E_PASS/ { gsub(/\r/, "", $2); print $2; exit }' "$LOG" 2> /dev/null || true)
echo "qemu-e2e-smoke: PASS: ${bad_reason:-guest reported QEMU_E2E_PASS}${wall:+ (guest wall $wall)}"
echo "qemu-e2e-smoke: full log $LOG"
exit 0
