#!/bin/sh
# mkimage.sh - build the ext4 disk image the guest mounts as /dev/vda.
#
# The image holds the four pieces a real install writes to, so the guest can
# write them without a writable host mount:
#
#   /modules/<rel>   the kernel's own module tree (kernel/*.ko.zst, modules.*)
#   /headers         the kernel build tree (the payload of linux72-headers)
#   /src             empty; bound to /usr/src, where DKMS stages the sources
#   /dkms            empty; bound to /var/lib/dkms, the DKMS tree
#
# The tree is populated with `mke2fs -d`, which needs no root: the files are
# copied from ~/.cache/tbtun-build (see docs/build-env.md) into a staging
# directory and handed to libext2fs. Rebuilt only when missing or --force.
#
# Exit codes: 0 built or already present, 2 a required input is missing.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=${TBTUN_KERNEL_REL:-$(uname -r)}
out=${TBTUN_DKMS_IMAGE:-$cache/dkms-qemu/disk.ext4}
size=${TBTUN_DKMS_IMAGE_SIZE:-1200M}
force=0

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    --force) force=1; shift ;;
    -h | --help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "mkimage.sh: unknown option $1" >&2
        exit 2
        ;;
    esac
done

die() {
    echo "mkimage.sh: $*" >&2
    exit 2
}

modsrc=$cache/linux72-full/usr/lib/modules/$rel
hdrsrc=$cache/linux72-headers/usr/lib/modules/$rel/build

[ -d "$modsrc/kernel" ] || die "no kernel module tree at $modsrc (see tests/qemu/README.md)"
[ -f "$hdrsrc/Makefile" ] || die "no kernel build tree at $hdrsrc (see docs/build-env.md)"

if [ -f "$out" ] && [ "$force" -eq 0 ]; then
    echo "mkimage.sh: $out exists ($(du -h "$out" | cut -f1)); use --force to rebuild"
    exit 0
fi

stage=$cache/dkms-qemu/imgroot
echo "mkimage.sh: staging $modsrc and $hdrsrc"
rm -rf "$stage"
mkdir -p "$stage/modules/$rel" "$stage/headers" "$stage/src" "$stage/dkms"

# The module tree is the whole point: depmod then sees what a real install
# sees, and dkms picks .ko.zst as the compression suffix because the tree
# carries compressed modules. A symlink to a missing /boot/vmlinuz would only
# be noise, so drop the package's convenience links.
cp -a "$modsrc"/. "$stage/modules/$rel"/
rm -f "$stage/modules/$rel/vmlinuz" "$stage/modules/$rel/kernelbase"

cp -a "$hdrsrc"/. "$stage/headers"/

mkdir -p "$(dirname "$out")"
rm -f "$out"
# -d fills the filesystem from the directory in one pass. Ownership is the
# invoking user's; the guest runs as root and does not care.
truncate -s "$size" "$out"
mke2fs -q -t ext4 -F -d "$stage" "$out" ||
    die "mke2fs failed for $out"

rm -rf "$stage"
echo "mkimage.sh: $out ($(du -h "$out" | cut -f1) allocated)"
