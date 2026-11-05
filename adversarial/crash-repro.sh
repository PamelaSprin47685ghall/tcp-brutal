#!/bin/sh
# crash-repro.sh - boot the minimal close-flush repro and report one result.
#
#   adversarial/crash-repro.sh
#
# Prints exactly one `ADV_RESULT ctl.close_flush PASS|FAIL <detail>` line on
# stdout; diagnostics go to stderr.  Exit 0 for PASS, 1 for FAIL, 2 for a
# missing input.  The guest is expected to hang after the oops, so the QEMU
# run is bounded by a short timeout and the verdict comes from the console.
set -eu

adv=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$adv/.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=$(uname -r)
kernel=$cache/dl/vmlinuz-$rel
ko=$root/tbtun.ko
console=${TBT_CRASH_LOG:-$adv/crash-console.log}
timeout_s=${TBT_CRASH_TIMEOUT:-45}

[ -f "$kernel" ] || { echo "crash-repro: no kernel at $kernel" >&2; exit 2; }
[ -f "$ko" ] || { echo "crash-repro: no module at $ko" >&2; exit 2; }
[ -x "$adv/adv_probe" ] || {
    cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror -I "$root/tbtun" \
        -o "$adv/adv_probe" "$adv/adv_probe.c" || exit 2
}

initrd=$cache/qemu/adv-crash-initramfs.cpio.gz
"$adv/qemu/mkinitramfs.sh" --ko "$ko" --out "$initrd" \
    --init "$adv/crash-init" --bin "$adv/adv_probe" --endpoint "$adv/adv_probe" > /dev/null ||
    { echo "crash-repro: initramfs build failed" >&2; exit 2; }

accel=tcg
[ -w /dev/kvm ] && accel=kvm
set +e
timeout --foreground "$timeout_s" qemu-system-x86_64 \
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

if grep -q 'REPRO_READY' "$console"; then
    :
else
    echo "ADV_RESULT ctl.close_flush SKIP the repro guest never reached REPRO_READY (console: $console)"
    exit 1
fi

if grep -qE 'BUG:|Oops|general protection fault|tbt_tunnel_add' "$console"; then
    detail=$(grep -E 'BUG:|Oops|RIP: 0010:tbt_tunnel_add|tbt_cfg_release|tbt_config_apply' "$console" |
        head -n 4 | tr '\n' ' ')
    echo "ADV_RESULT ctl.close_flush FAIL unterminated tunnel line at close: $detail (console: $console)"
    exit 1
fi

if grep -q 'TBT_REPRO_DONE' "$console"; then
    echo "ADV_RESULT ctl.close_flush PASS the guest survived the close flush (console: $console)"
    exit 0
fi

echo "ADV_RESULT ctl.close_flush FAIL the guest hung without a REPRO_SURVIVED marker (qemu rc=$rc, console: $console)"
exit 1
