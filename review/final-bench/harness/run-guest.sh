#!/bin/sh
# run-guest.sh - boot one final-bench measurement guest.
#
#   run-guest.sh flush    two 40 MiB strict transfers, then a timed flush
#   run-guest.sh writer   100 MiB and 1 GiB pub.writer cases with a memory ledger
#
# The caller holds /tmp/tbtun-qemu.lock; this script does no locking itself.
set -eu

root=/home/kunweiz/tbtun-worktrees/final-bench
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
kernel=$cache/dl/vmlinuz-$(uname -r)

case ${1:-} in
flush)
    name=flush-two-transfer
    init=flush-two-transfer-init.sh
    tmo=300
    ;;
writer)
    name=writer-memory
    init=writer-memory-init.sh
    tmo=420
    ;;
idle)
    name=idle-cpu
    init=idle-cpu-init.sh
    tmo=180
    ;;
*)
    echo "usage: $0 flush|writer" >&2
    exit 2
    ;;
esac

initrd=$cache/qemu/final-bench-$name.cpio.gz
console=$root/review/final-bench/$name-console.log

"$root/adversarial/qemu/mkinitramfs.sh" --ko "$root/tbtun.ko" --out "$initrd" \
    --init "$root/review/final-bench/harness/$init" \
    --endpoint "$root/benches/bin/tbt_burst" \
    --bin "$root/adversarial/adv_probe" > /dev/null

set +e
timeout --foreground "$tmo" qemu-system-x86_64 \
    -accel kvm -cpu max -smp 2 -m 512 \
    -kernel "$kernel" -initrd "$initrd" \
    -append "console=ttyS0 panic=-1 loglevel=7 tsc=unstable" \
    -nographic -no-reboot -nodefaults \
    -serial mon:stdio -display none \
    > "$console" 2>&1
rc=$?
set -e
echo "QEMU_RC $rc console=$console"
grep -E '^(XFER|FLUSH_|MEM_|WRITER_|STATS|IDLE_|TBT_ADV_DONE)' "$console" || true
[ "$rc" -eq 0 ] || exit "$rc"
