#!/bin/sh
# mkinitramfs.sh - assemble the initramfs for the DKMS acceptance guest.
#
#   review/dkms-qemu/mkinitramfs.sh [--out FILE]
#
# The image holds only what the guest needs before it can chroot into the
# host's userland over 9p:
#
#   /init           PID 1: mounts, then chroots into the writable view
#   /guest-test.sh  the acceptance flow, run as root inside the chroot
#   /dkms           the real dkms script (Manjaro package, version recorded)
#   /mods/*.ko      9p and virtio-fs dependencies, decompressed from the
#                   kernel's own module tree so vermagic matches by
#                   construction
#   /bin/busybox    static busybox plus every applet symlink
#
# Exit codes: 0 built, 2 a required input is missing.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=${TBTUN_KERNEL_REL:-$(uname -r)}
out=${TBTUN_DKMS_INITRD:-$cache/dkms-qemu/initramfs.cpio.gz}
dkms_src=${TBTUN_DKMS_BIN:-/tmp/rel-dkms/usr/bin/dkms}

while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift 2 ;;
    -h | --help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "mkinitramfs.sh: unknown option $1" >&2
        exit 2
        ;;
    esac
done

die() {
    echo "mkinitramfs.sh: $*" >&2
    exit 2
}

[ -f "$dkms_src" ] || die "no dkms script at $dkms_src"
[ -f "$root/review/dkms-qemu/init" ] || die "no review/dkms-qemu/init"
[ -f "$root/review/dkms-qemu/guest-test.sh" ] || die "no review/dkms-qemu/guest-test.sh"

bb=
for cand in "$cache/busybox/usr/bin/busybox" "$cache/busybox/bin/busybox"; do
    [ -x "$cand" ] && bb=$cand && break
done
[ -n "$bb" ] || die "no busybox in $cache/busybox"

work=$cache/dkms-qemu/initrd
rm -rf "$work"
mkdir -p "$work"/bin "$work"/proc "$work"/sys "$work"/dev "$work"/mods \
    "$work"/host "$work"/newroot "$work"/tmp

cp "$bb" "$work/bin/busybox"
for applet in $("$bb" --list); do
    [ "$applet" = busybox ] && continue
    ln -sf busybox "$work/bin/$applet"
done
for applet in sh mount insmod poweroff chroot; do
    [ -e "$work/bin/$applet" ] || die "busybox has no '$applet' applet"
done

cp "$root/review/dkms-qemu/init" "$work/init"
cp "$root/review/dkms-qemu/guest-test.sh" "$work/guest-test.sh"
cp "$dkms_src" "$work/dkms"
chmod +x "$work/init" "$work/guest-test.sh" "$work/dkms"

# 9p over virtio: netfs is a dependency of both 9pnet and 9p. The guest mounts
# the host root read-only and the artifacts directory read-write.
moddir=$cache/linux72-full/usr/lib/modules/$rel/kernel
for m in fs/netfs/netfs net/9p/9pnet net/9p/9pnet_virtio fs/9p/9p; do
    src=$moddir/$m.ko.zst
    [ -f "$src" ] || die "no $src"
    zstd -dc "$src" > "$work/mods/$(basename "$m").ko" ||
        die "cannot decompress $src"
done

mkdir -p "$(dirname "$out")"
( cd "$work" && find . -print0 | cpio --null -o -H newc 2> /dev/null | gzip -9 ) > "$out"
echo "mkinitramfs.sh: $out ($(du -h "$out" | cut -f1)), $("$dkms_src" --version 2> /dev/null | head -1)"
