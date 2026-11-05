#!/bin/sh
# run.sh - build and run the injection probe in QEMU.
#
#   tests/probe2/run.sh [--timeout SECONDS]
#
# Env: TBT_ARM_REPAIR=1 arms tp->repair before injecting (reproduces the -EPERM
#      symptom); default 0, which is what the results in docs/inject-probe.md
#      were measured with.
set -eu
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$dir/../.." && pwd)
timeout_s=120
[ "${1:-}" = "--timeout" ] && { timeout_s=$2; shift 2; }
# Never let a probe run compile the module under test: build only this dir.
make -C "$dir" > /tmp/tbt_probe2_make.log 2>&1 || { cat /tmp/tbt_probe2_make.log >&2; exit 1; }
# The module parameter is read from a file in the initramfs: an environment
# variable on the host does not cross into the guest.
# The file lands in the guest root as /arm_repair (mkinitramfs copies --bin
# targets into /bin under their own name, so it is passed as an init asset).
printf '%s' "${TBT_ARM_REPAIR:-0}" > "$dir/arm_repair"
# The guest runs /probe2_user: mkinitramfs --bin resolves the target relative
# to the initramfs root, so both the asset and the binary land there.

exec "$root/tests/qemu/run.sh" \
    --asset "$dir/arm_repair" \
    --bin "$dir/probe2_user" \
    --ko "$dir/tbt_probe2.ko" \
    --init "$dir/init" \
    --log "${TBT_PROBE2_LOG:-/tmp/tbt_probe2_console.log}" \
    --timeout "$timeout_s"
