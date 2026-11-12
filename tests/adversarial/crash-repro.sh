#!/bin/sh
# crash-repro.sh - boot the minimal close-flush repro and report one result.
#
#   adversarial/crash-repro.sh [--ko PATH] [--kernel PATH]
#
# Prints exactly one `ADV_RESULT ctl.close_flush PASS|FAIL <detail>` line on
# stdout; diagnostics go to stderr.  Exit 0 for PASS, 1 for FAIL, 2 for a
# missing input.  The guest is expected to hang after the oops, so the QEMU
# run is bounded by a short timeout and the verdict comes from the console.
set -eu

adv=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$adv/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=$(uname -r)
kernel=${TBT_ADV_KERNEL:-}
ko=$root/build/kernel/tbtun.ko
console=${TBT_CRASH_LOG:-$adv/crash-console.log}
timeout_s=${TBT_CRASH_TIMEOUT:-45}

usage() {
    cat <<EOF
Usage: adversarial/crash-repro.sh [options]

  --ko PATH       module image to boot (default $ko)
  --kernel PATH   kernel image (default: dl/vmlinuz-$rel under the cache)
  -h, --help      print this help
EOF
}

while [ $# -gt 0 ]; do
    case $1 in
    --ko)
        [ $# -ge 2 ] || { echo "crash-repro: --ko needs a path" >&2; exit 2; }
        ko=$2
        shift 2
        ;;
    --kernel)
        [ $# -ge 2 ] || { echo "crash-repro: --kernel needs a path" >&2; exit 2; }
        kernel=$2
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "crash-repro: unknown option $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

# The documented env.sh may point at a payload cache ($T52) that holds dl/
# while the entry cache only carries env.sh.
cache_dirs=$cache
if [ -f "$cache/env.sh" ]; then
    _t52=$(
        . "$cache/env.sh" > /dev/null 2>&1 || exit 1
        printf '%s' "${T52:-}"
    ) || _t52=
    if [ -n "$_t52" ]; then
        cache_dirs="$cache_dirs $_t52"
    fi
fi
case " $cache_dirs " in
*" $HOME/.cache/tbtun-build-t52 "*) ;;
*) cache_dirs="$cache_dirs $HOME/.cache/tbtun-build-t52" ;;
esac
if [ -z "$kernel" ]; then
    for _d in $cache_dirs; do
        if [ -f "$_d/dl/vmlinuz-$rel" ]; then
            kernel=$_d/dl/vmlinuz-$rel
            break
        fi
    done
fi

[ -f "$kernel" ] || {
    echo "crash-repro: no kernel at ${kernel:-dl/vmlinuz-$rel under $cache_dirs}" >&2
    exit 2
}
[ -f "$ko" ] || { echo "crash-repro: no module at $ko" >&2; exit 2; }
[ -x "$(command -v qemu-system-x86_64)" ] || {
    echo "crash-repro: qemu-system-x86_64 is not installed" >&2
    exit 2
}
[ -x "$adv/adv_probe" ] || {
    cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror -I "$root/src/kernel" \
        -o "$adv/adv_probe" "$adv/adv_probe.c" || exit 2
}

mkdir -p "$cache/qemu" || { echo "crash-repro: cannot create $cache/qemu" >&2; exit 2; }
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
