#!/bin/sh
# run.sh - build the module, boot the QEMU guests, run the adversarial suite,
# and print one table row per attack.
#
#   adversarial/run.sh [--no-build] [--timeout SECONDS] [--no-crash]
#
# Exit codes: 0 every attack passed, 1 an attack failed or a guest died,
# 2 an input (kernel image, module, busybox) is missing.
#
# The suite is split into groups, one QEMU boot each, because a defect can
# hang the kernel: a hang in the hostile-carrier group must not hide the
# public-port results.  A group lists its phases in /adv-phases.txt; the guest
# prints PHASE_START/PHASE_END, and a started phase without an end is reported
# as a hang.
#
# The kernel, busybox and headers come from ~/.cache/tbtun-build, exactly as
# tests/qemu/README.md documents.  The initramfs builder is a private copy
# under adversarial/qemu/ so a concurrent edit to tests/qemu/ cannot change
# what this suite boots.
set -eu

adv=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$adv/.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=$(uname -r)
kernel=$cache/dl/vmlinuz-$rel
ko=$root/tbtun.ko
timeout_s=${TBT_ADV_TIMEOUT:-600}
carrier_timeout=${TBT_ADV_CARRIER_TIMEOUT:-240}
public_timeout=${TBT_ADV_PUBLIC_TIMEOUT:-2400}
groups=${TBT_ADV_GROUPS:-"public control resource slab final replay rmmod flush catconfig cfg_mix param port near far stall"}
build=1
crash=1
results_file=$adv/results.txt

while [ $# -gt 0 ]; do
    case $1 in
    --no-build) build=0; shift ;;
    --no-crash) crash=0; shift ;;
    --timeout) timeout_s=$2; shift 2 ;;
    -h | --help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "run.sh: unknown option $1" >&2
        exit 2
        ;;
    esac
done

die() {
    echo "run.sh: $*" >&2
    exit 2
}

[ -x "$(command -v qemu-system-x86_64)" ] || die "qemu-system-x86_64 is not installed"
[ -f "$kernel" ] || die "no kernel image at $kernel (see tests/qemu/README.md)"
[ -f "$cache/env.sh" ] || die "no build environment at $cache/env.sh"
[ -x "$adv/qemu/mkinitramfs.sh" ] || die "adversarial/qemu/mkinitramfs.sh is missing"

if [ "$build" -eq 1 ]; then
    echo "run.sh: building tbtun.ko (source $cache/env.sh; make check)"
    ( . "$cache/env.sh"; make -C "$root" check ) || die "the module build failed"
fi
[ -f "$ko" ] || die "no module at $ko; run make check"
echo "run.sh: module sha256 $(sha256sum "$ko" | cut -d' ' -f1)"

echo "run.sh: building adv_probe"
cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror -I "$root/tbtun" \
    -o "$adv/adv_probe" "$adv/adv_probe.c" || die "adv_probe build failed"

accel=tcg
[ -w /dev/kvm ] && accel=kvm

: > "$results_file"

phase_list() {
    case $1 in
    stable) echo "public control resource slab final replay rmmod flush param" ;;
    near) echo "near" ;;
    far) echo "far" ;;
    *) echo "$1" ;;
    esac
}

run_group() {
    g=$1
    phases=$(phase_list "$g")
    tmo=$2
    initrd=$cache/qemu/adv-initramfs-$g.cpio.gz
    console=$adv/console-$g.log
    # The sha of the image this guest will load.  It is captured before the
    # initramfs is built and stamped on every row of this group, because the
    # module may be rebuilt between groups.
    gsha=$(sha256sum "$ko" | cut -c1-16)

    printf '%s\n' "$phases" > "$cache/qemu/adv-phases.txt"
    "$adv/qemu/mkinitramfs.sh" --ko "$ko" --out "$initrd" \
        --init "$adv/init" --bin "$adv/adv_probe" --endpoint "$adv/adv_probe" \
        --asset "$cache/qemu/adv-phases.txt" > /dev/null ||
        die "initramfs build failed for group $g"

    echo "run.sh: group $g: qemu accel=$accel timeout=${tmo}s sha=$gsha phases=$phases"
    set +e
    timeout --foreground "$tmo" qemu-system-x86_64 \
        -accel "$accel" -cpu max -smp 2 -m 512 \
        -kernel "$kernel" \
        -initrd "$initrd" \
        -append "console=ttyS0 panic=-1 loglevel=7 tsc=unstable" \
        -nographic -no-reboot -nodefaults \
        -serial mon:stdio \
        -display none \
        > "$console" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 124 ] && echo "run.sh: group $g TIMEOUT after ${tmo}s (console: $console)"

    grep '^ADV_RESULT ' "$console" 2> /dev/null |
        sed "s/^ADV_RESULT \([^ ]*\) \([^ ]*\) /ADV_RESULT \1 \2 sha=$gsha /" \
            >> "$results_file" || true

    # A phase that started but never ended is a hang; a phase that never
    # started is a group that died before it.  Only the first such phase is
    # reported: the later ones are consequences of the same stop.
    dead=0
    for p in $phases; do
        [ "$dead" -eq 1 ] && continue
        if ! grep -q "^PHASE_START $p" "$console"; then
            echo "ADV_RESULT console.missing.$p FAIL sha=$gsha phase $p never started (group $g died earlier; console $console)" >> "$results_file"
            dead=1
        elif ! grep -q "^PHASE_END $p" "$console"; then
            echo "ADV_RESULT console.hang.$p FAIL sha=$gsha phase $p started but did not finish (group $g; console $console)" >> "$results_file"
            dead=1
        fi
    done
    n=$(grep -cE 'BUG:|Oops|general protection fault|INFO: task .* blocked|deprecated workqueue' "$console" 2> /dev/null || true)
    if [ "${n:-0}" -gt 0 ]; then
        echo "ADV_RESULT console.klog.$g FAIL sha=$gsha $n kernel oops/warning lines (console $console)" >> "$results_file"
    fi
    if [ "$dead" -eq 0 ] && ! grep -q '^TBT_ADV_DONE' "$console"; then
        echo "ADV_RESULT console.done.$g FAIL sha=$gsha group $g did not reach TBT_ADV_DONE (qemu rc=$rc; console $console)" >> "$results_file"
    fi
}

for g in $groups; do
    case $g in
    public) run_group public "$public_timeout" ;;
    stable) run_group stable "$timeout_s" ;;
    *) run_group "$g" "$carrier_timeout" ;;
    esac
done

# The close-flush crash is reproduced in its own guest: the oops hangs the
# machine, which would hide every later result if it ran in a group.
if [ "$crash" -eq 1 ]; then
    echo "run.sh: booting the close-flush crash repro (isolated guest)"
    csha=$(sha256sum "$ko" | cut -c1-16)
    crash_line=$("$adv/crash-repro.sh" 2> /dev/null | grep '^ADV_RESULT ' || true)
    if [ -z "$crash_line" ]; then
        crash_line="ADV_RESULT ctl.close_flush FAIL sha=$csha the crash repro produced no verdict; see $adv/crash-console.log"
    else
        crash_line=$(printf '%s\n' "$crash_line" |
            sed "s/^ADV_RESULT \([^ ]*\) \([^ ]*\) /ADV_RESULT \1 \2 sha=$csha /")
    fi
    printf '%s\n' "$crash_line" >> "$results_file"
fi

results=$(cat "$results_file")
if [ -z "$results" ]; then
    echo "run.sh: FAIL: no ADV_RESULT lines"
    exit 1
fi

echo
printf '%-32s %-6s %-18s %s\n' "ATTACK" "RESULT" "BUILD" "DETAIL"
printf '%-32s %-6s %-18s %s\n' "--------------------------------" "------" "------------------" "------"
printf '%s\n' "$results" |
    awk '{
        name=$2; res=$3; sha=$4; detail="";
        for (i = 5; i <= NF; i++) detail = detail (i > 5 ? " " : "") $i;
        printf "%-32s %-6s %-18s %s\n", name, res, sha, detail;
    }'

passes=$(printf '%s\n' "$results" | awk '$3 == "PASS" {n++} END {print n + 0}')
fails=$(printf '%s\n' "$results" | awk '$3 == "FAIL" || $3 == "TIMEOUT" {n++} END {print n + 0}')
skips=$(printf '%s\n' "$results" | awk '$3 == "SKIP" {n++} END {print n + 0}')

echo
klogs=$(grep '^ADV_KLOG ' "$results_file" || true)
if [ -n "$klogs" ]; then
    echo "kernel-log events during attacks:"
    printf '%s\n' "$klogs" | sed 's/^/    /'
fi
echo "run.sh: $passes passed, $fails failed, $skips skipped"
for g in $groups; do
    [ -f "$adv/console-$g.log" ] && echo "run.sh: group $g console: $adv/console-$g.log"
done
[ -f "$adv/crash-console.log" ] && echo "run.sh: crash console: $adv/crash-console.log"

if [ "$fails" -gt 0 ] || [ "$skips" -gt 0 ]; then
    exit 1
fi
exit 0
