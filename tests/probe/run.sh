#!/bin/sh
# run.sh - build and run the proto_ops probe in QEMU.
#
#   tests/probe/run.sh [--timeout SECONDS]
#
# Exists so nobody has to remember the three --init/--bin/--ko arguments and
# the order they must be in. Uses the same QEMU kernel cached by
# tests/qemu/README.md.
set -eu

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$dir/../.." && pwd)
timeout_s=90

[ "${1:-}" = "--timeout" ] && { timeout_s=$2; shift 2; }

make -C "$dir" > /dev/null
exec "$root/tests/qemu/run.sh" \
    --ko "$dir/tbt_probe.ko" \
    --init "$dir/init" \
    --bin "$dir/probe_user" \
    --timeout "$timeout_s"
