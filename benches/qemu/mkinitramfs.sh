#!/bin/sh
# mkinitramfs.sh - assemble the initramfs that benches/run.sh boots.
#
#   tests/qemu/mkinitramfs.sh [--ko PATH] [--out FILE] [--endpoint PATH]
#
# The image is a cpio.gz holding:
#   /init            the test itself (tests/qemu/init)
#   /<module>.ko     the module under test, under its own file name
#   /bin/tbt_burst   either the static helper (host/tbt_burst) or the busybox
#                    applet symlink when the helper is absent
#   /bin/nc /bin/sha256sum /bin/dd ...   busybox applets
#   /usr/lib/...     the dynamic loader and libraries tbt_burst needs when it is
#                    not static (glibc crt files are not installed on the host,
#                    so a static build is only possible with a musl toolchain)
#
# Everything is resolved from unprivileged sources: the guest binaries come from
# packages downloaded into ~/.cache/tbtun-build (see README.md). Nothing is
# installed on the host and nothing needs root here.
#
# Exit codes: 0 built, 2 a required input is missing (message says which).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}

ko=$root/tbtun.ko
out=
endpoint=
init_src=$root/benches/qemu/init
binaries=
assets=

while [ $# -gt 0 ]; do
    case $1 in
    --ko) ko=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --endpoint) endpoint=$2; shift 2 ;;
    --init) init_src=$2; shift 2 ;;
    --bin)
        binaries="$binaries $2"
        shift 2
        ;;
    --asset) assets="$assets $2"; shift 2 ;;
    -h | --help)
        sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "mkinitramfs.sh: unknown option $1" >&2
        exit 2
        ;;
    esac
done
out=${out:-$cache/bench/initramfs.cpio.gz}
endpoint=${endpoint:-$root/benches/bin/tbt_burst}

die() {
    echo "mkinitramfs: $*" >&2
    exit 2
}

[ -f "$ko" ] || die "no module at $ko (build it first: make check)"
[ -f "$init_src" ] || die "no init script at $init_src"

# --- endpoint, built here so run.sh never needs a second toolchain step ----
# The guest shares the host's glibc, so the plain host compiler is enough. A
# static build would be nicer; glibc's crt files are not installed, so only a
# musl toolchain could produce one, and mkinitramfs bundles the loader and the
# libraries instead.
if [ ! -x "$endpoint" ] && [ -f "$root/host/tbt_burst.c" ]; then
    "${HOSTCC:-cc}" -std=gnu99 -O2 -Wall -Wextra -Werror \
        -o "$endpoint" "$root/host/tbt_burst.c" ||
        die "cannot build $root/host/tbt_burst"
    echo "mkinitramfs: built $endpoint with ${HOSTCC:-cc}"
fi

# --- busybox ---------------------------------------------------------------
# The guest runs on the host's glibc, so system busybox is fine when present;
# otherwise take the -static package from the mirror into the cache.
bb=
if command -v busybox > /dev/null 2>&1; then
    bb=$(command -v busybox)
elif [ -x "$cache/busybox/usr/bin/busybox" ]; then
    bb=$cache/busybox/usr/bin/busybox
elif [ -x "$cache/busybox/bin/busybox" ]; then
    bb=$cache/busybox/bin/busybox
else
    die "busybox not found. Install it, or cache the static package:
  mkdir -p $cache/busybox
  curl -Lo /tmp/busybox.pkg \\
    https://mirror.sjtu.edu.cn/manjaro/stable/extra/x86_64/busybox-1.36.1-4-x86_64.pkg.tar.zst
  tar --zstd -xf /tmp/busybox.pkg -C $cache/busybox"
fi

# --- test endpoint ---------------------------------------------------------
if [ -x "$endpoint" ]; then
    echo "mkinitramfs: endpoint $endpoint"
else
    echo "mkinitramfs: no $endpoint; /bin/tbt_burst becomes a busybox applet symlink" >&2
    endpoint=
fi

# --- staging ---------------------------------------------------------------
work=$cache/bench/stage
rm -rf "$work"
mkdir -p "$work"/bin "$work"/proc "$work"/sys "$work"/dev "$work"/tmp

cp "$bb" "$work/bin/busybox"
# Every applet the binary carries, as symlinks. A hand-picked list is a trap:
# busybox in this build does not fall back to its internal applet table, so a
# command missing a symlink fails with "not found" *after* the point where it
# was needed, and the failure then looks like a module defect.
for applet in $("$bb" --list); do
    [ "$applet" = busybox ] && continue
    ln -sf busybox "$work/bin/$applet"
done
for applet in sh mount insmod poweroff; do
    [ -e "$work/bin/$applet" ] ||
        die "busybox has no '$applet' applet; the guest cannot boot"
done

# Keep the module's own name: a probe init looks for its own file.
cp "$ko" "$work/$(basename "$ko")"
cp "$init_src" "$work/init"
chmod +x "$work/init"

if [ -n "$endpoint" ]; then
    cp "$endpoint" "$work/bin/tbt_burst"
    chmod +x "$work/bin/tbt_burst"
else
    ln -s busybox "$work/bin/tbt_burst"
fi

# Extra files for a probe init, copied to / under their own name.
for asset in $assets; do
    [ -f "$asset" ] || die "no asset at $asset"
    # rm first: a busybox applet symlink of the same name would otherwise be
    # written through, replacing /bin/busybox with the new file.
    rm -f "$work/$(basename "$asset")"
    cp "$asset" "$work/$(basename "$asset")"
done

# Extra binaries for a probe init, copied to /bin under their own name.
for bin in $binaries; do
    [ -f "$bin" ] || die "no binary at $bin"
    dst=$work/bin/$(basename "$bin")
    # The applet loop above made bin/tc a symlink to busybox; cp would follow
    # it and overwrite busybox with tc. Replace the link instead.
    rm -f "$dst"
    cp "$bin" "$dst"
    chmod +x "$dst"
done

# Shared objects the endpoint needs. A static endpoint reports no ldd output
# and needs none of this; a dynamically linked one gets its interpreter and
# libraries copied to the same absolute path they came from.
copy_libs() {
    _bin=$1
    command -v ldd > /dev/null 2>&1 || return 0
    _libs=$cache/bench/libs.txt
    ldd "$_bin" 2> /dev/null |
        awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}' > "$_libs"
    # A file, not a pipe into `while read`: the loop body would run in a
    # subshell and its copies would race cpio.
    while read -r _lib; do
        [ -f "$_lib" ] || continue
        mkdir -p "$work$(dirname "$_lib")"
        cp -L "$_lib" "$work$_lib"
    done < "$_libs"
    rm -f "$_libs"
    for _interp in /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2; do
        [ -f "$_interp" ] || continue
        mkdir -p "$work$(dirname "$_interp")"
        cp -L "$_interp" "$work$_interp"
    done
}

if [ -n "$endpoint" ]; then
    copy_libs "$endpoint"
fi
for bin in $binaries; do
    copy_libs "$bin"
done

# --- packing ---------------------------------------------------------------
mkdir -p "$(dirname "$out")"
( cd "$work" && find . -print0 | cpio --null -o -H newc 2> /dev/null | gzip -9 ) > "$out"
echo "mkinitramfs: $out ($(du -h "$out" | cut -f1))"
