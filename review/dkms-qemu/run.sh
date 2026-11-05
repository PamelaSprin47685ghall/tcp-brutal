#!/bin/sh
# run.sh - boot the clean 7.2.9-1-MANJARO kernel in QEMU and run the DKMS
# acceptance flow inside it.
#
#   review/dkms-qemu/run.sh [--keep-image] [--timeout SECONDS] [--console FILE]
#
# The guest is a chroot built from read-only 9p exports of the host's /usr,
# /etc and /home plus a writable ext4 image for /usr/src, /var/lib/dkms and
# /usr/lib/modules/<rel>. Inside it, review/dkms-qemu/guest-test.sh runs the
# real scripts/install_dkms.sh, modprobe, rmmod and --uninstall as root on the
# guest kernel.
#
# The ext4 image is rebuilt for every run, so the guest always starts with the
# distro payload's module tree and an empty /usr/src and /var/lib/dkms; the
# guest's own pre-checks then prove that no tbtun exists anywhere yet.
#
# QEMU is exclusive: a run takes /tmp/tbtun-qemu.lock for its whole duration,
# so it never overlaps another QEMU or bench on this box.
#
# Exit codes:
#   0  the guest printed DKMS_RESULT PASS and no step failed
#   1  the guest printed DKMS_RESULT FAIL, a step failed, or it timed out
#   2  a required input is missing (kernel image, busybox, dkms, module tree)
set -eu

# One QEMU at a time on this host: benches and probe runs share the box. The
# re-exec must come before option parsing, or $@ would already be consumed.
if [ "${TBTUN_DKMS_LOCKED:-0}" != 1 ]; then
    exec env TBTUN_DKMS_LOCKED=1 flock -x /tmp/tbtun-qemu.lock "$0" "$@"
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=${TBTUN_KERNEL_REL:-$(uname -r)}
kernel=$cache/dl/vmlinuz-$rel
image=$cache/dkms-qemu/disk.ext4
initrd=$cache/dkms-qemu/initramfs.cpio.gz
artifacts=$here/artifacts
console=$here/console.log
timeout_s=${TBTUN_DKMS_TIMEOUT:-1800}
keep_image=0

while [ $# -gt 0 ]; do
    case $1 in
    --keep-image) keep_image=1; shift ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --console) console=$2; shift 2 ;;
    -h | --help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
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

[ -x "$(command -v qemu-system-x86_64)" ] || die "qemu-system-x86_64 not installed"
[ -f "$kernel" ] || die "no kernel image at $kernel (see tests/qemu/README.md)"
[ -f /tmp/rel-dkms/usr/bin/dkms ] ||
    die "no extracted dkms at /tmp/rel-dkms/usr/bin/dkms (Manjaro package)"
[ -d "$cache/linux72-full/usr/lib/modules/$rel/kernel" ] ||
    die "no module tree at $cache/linux72-full (see tests/qemu/README.md)"
[ -f "$cache/linux72-headers/usr/lib/modules/$rel/build/Makefile" ] ||
    die "no kernel build tree at $cache/linux72-headers (see docs/build-env.md)"

if [ "$keep_image" -eq 1 ]; then
    "$here/mkimage.sh"
else
    "$here/mkimage.sh" --force
fi
"$here/mkinitramfs.sh" --out "$initrd"

rm -rf "$artifacts"
mkdir -p "$artifacts"

accel=tcg
cpu=max
if [ -w /dev/kvm ]; then
    accel=kvm
    cpu=host
fi

# Built with `set --` so the kernel command line survives as one argument.
set -- qemu-system-x86_64 \
    -accel "$accel" -cpu "$cpu" -smp 2 -m 2048 \
    -kernel "$kernel" -initrd "$initrd" \
    -append "console=ttyS0 panic=-1 loglevel=7" \
    -nographic -no-reboot -nodefaults -display none -serial mon:stdio \
    -drive "file=$image,format=raw,if=virtio,cache=unsafe" \
    -virtfs "local,path=/,mount_tag=hostroot,security_model=none,readonly=on" \
    -virtfs "local,path=$artifacts,mount_tag=out,security_model=none"

{
    echo "# host environment"
    date -u +%Y-%m-%dT%H:%M:%SZ
    uname -a
    qemu-system-x86_64 --version | head -1
    echo "# why the guest: no root, no dkms, no matching headers on the host"
    echo "uid=$(id -u)"
    echo "dkms=$(command -v dkms || echo absent)"
    echo "host_headers=$(test -e "/lib/modules/$rel/build" && echo present || echo absent)"
    echo "host_module_tree=$(test -d "/usr/lib/modules/$rel/kernel" && echo present || echo absent)"
    sha256sum "$kernel" "$initrd" /tmp/rel-dkms/usr/bin/dkms
    echo "# qemu"
    printf '%s ' "$@"
    echo
} > "$artifacts/host-env.txt"

echo "run.sh: accel=$accel kernel=$(basename "$kernel") timeout=${timeout_s}s"
echo "run.sh: console $console"

rc=0
timeout --foreground "$timeout_s" "$@" > "$console" 2>&1 || rc=$?
if [ "$rc" -eq 124 ]; then
    echo "run.sh: TIMEOUT after ${timeout_s}s"
fi
echo "run.sh: qemu exited $rc"

# The built module is the guest's, not the host's: move it out of the evidence
# directory but keep its digest.
if [ -f "$artifacts/tbtun.ko.zst" ]; then
    mkdir -p "$cache/dkms-qemu"
    mv "$artifacts/tbtun.ko.zst" "$cache/dkms-qemu/guest-tbtun.ko.zst"
    sha256sum "$cache/dkms-qemu/guest-tbtun.ko.zst" > "$artifacts/guest-tbtun.ko.sha256"
    echo "run.sh: guest module kept at $cache/dkms-qemu/guest-tbtun.ko.zst"
fi

steps=$(grep '^DKMS_STEP ' "$console" || true)
checks=$(grep '^DKMS_CHECK ' "$console" || true)
result=$(grep '^DKMS_RESULT ' "$console" || true)

echo "run.sh: steps"
printf '%s\n' "$steps" | sed 's/^/    /'
echo "run.sh: checks"
printf '%s\n' "$checks" | sed 's/^/    /'
echo "run.sh: result"
printf '%s\n' "${result:-    (no DKMS_RESULT line)}" | sed 's/^/    /'

failed_steps=$(printf '%s\n' "$steps" | grep -cE '^DKMS_STEP [^ ]+ rc=[1-9]' || true)
failed_checks=$(printf '%s\n' "$checks" | grep -c ' FAIL' || true)

echo "run.sh: console tail ($console)"
tail -n 20 "$console" | sed 's/^/    /'

if [ -z "$result" ]; then
    echo "run.sh: FAIL: the guest printed no DKMS_RESULT (rc=$rc)"
    exit 1
fi
if ! printf '%s\n' "$result" | grep -q '^DKMS_RESULT PASS'; then
    echo "run.sh: FAIL: guest reported failure"
    exit 1
fi
if [ "$failed_steps" -ne 0 ] || [ "$failed_checks" -ne 0 ]; then
    echo "run.sh: FAIL: $failed_steps failed step(s), $failed_checks failed check(s)"
    exit 1
fi
echo "run.sh: PASS: DKMS install/load/unload/uninstall on $rel"
exit 0
