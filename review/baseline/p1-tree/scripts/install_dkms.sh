#!/usr/bin/env bash
#
# install_dkms.sh - build and install the tbtun kernel module through DKMS.
#
# The module is the only artifact. Nothing is written to /usr/bin, /usr/local/bin
# or any other PATH directory; configuration happens through /proc/net/tbtun.
#
#   sudo ./scripts/install_dkms.sh
#   sudo ./scripts/install_dkms.sh --kernel 6.12.1-arch1-1
#   sudo ./scripts/install_dkms.sh --uninstall
#   ./scripts/install_dkms.sh --dry-run        # no root, prints every step
#
set -euo pipefail

PROGRAM=${0##*/}
MODULE=tbtun
MIN_KERNEL=6.1
DKMS_ROOT=${DKMS_ROOT:-/usr/src}

DRY_RUN=0
STAGE_ONLY=0
UNINSTALL=0
LOAD=1
KERNEL=""
SRC_DIR=$(cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    cat <<EOF
Usage: $PROGRAM [options]

Build and install $MODULE through DKMS. Requires root unless --dry-run.

Options:
  -k, --kernel VER   build for this kernel release (default: the running one)
  -n, --dry-run      print the commands instead of running them; no root needed
  -u, --uninstall    remove the module from DKMS and unload it
      --no-load      install but do not modprobe the module
      --stage-only   copy the sources and write dkms.conf, then stop; no root
                     needed when DKMS_ROOT is writable
      --source DIR   source tree to install (default: the parent of this script)
  -h, --help         show this help
EOF
}

die() {
    printf '%s: %s\n' "$PROGRAM" "$*" >&2
    exit 1
}

say() {
    printf '%s\n' "$*"
}

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  + %s\n' "$*"
        return 0
    fi
    "$@"
}

# "6.12.1-arch1-1" -> major 6, minor 12
kernel_major_minor() {
    local rel=$1 major rest minor
    major=${rel%%.*}
    rest=${rel#*.}
    minor=${rest%%.*}
    printf '%s %s\n' "$major" "$minor"
}

kernel_supported() {
    local rel=$1 major minor want_major want_minor
    major=$(kernel_major_minor "$rel" | cut -d' ' -f1)
    minor=$(kernel_major_minor "$rel" | cut -d' ' -f2)
    want_major=${MIN_KERNEL%%.*}
    want_minor=${MIN_KERNEL#*.}
    case "$major$minor" in
    '' | *[!0-9]*) return 1 ;;
    esac
    [ "$major" -gt "$want_major" ] && return 0
    [ "$major" -eq "$want_major" ] && [ "$minor" -ge "$want_minor" ]
}

while [ $# -gt 0 ]; do
    case "$1" in
    -k | --kernel)
        [ $# -ge 2 ] || die "--kernel needs a release"
        KERNEL=$2
        shift 2
        ;;
    -n | --dry-run)
        DRY_RUN=1
        shift
        ;;
    -u | --uninstall)
        UNINSTALL=1
        shift
        ;;
    --no-load)
        LOAD=0
        shift
        ;;
    --stage-only)
        STAGE_ONLY=1
        shift
        ;;
    --source)
        [ $# -ge 2 ] || die "--source needs a directory"
        SRC_DIR=$(cd -- "$2" && pwd)
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        die "unknown option: $1"
        ;;
    esac
done

[ -f "$SRC_DIR/Makefile" ] || die "no Makefile in $SRC_DIR"
[ -d "$SRC_DIR/tbtun" ] || die "no tbtun/ directory in $SRC_DIR"

if [ "$UNINSTALL" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
        die "uninstalling needs root; re-run with sudo"
    fi
    say "Removing $MODULE from DKMS"
    run dkms remove -m "$MODULE" --all
    run rm -rf "$DKMS_ROOT/$MODULE-"*
    run depmod -a
    if [ "$LOAD" -eq 1 ]; then
        run rmmod "$MODULE" || true
    fi
    say "Done."
    exit 0
fi

if [ "$DRY_RUN" -eq 0 ] && [ "$STAGE_ONLY" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    die "installing needs root; re-run with sudo (or use --dry-run)"
fi

[ -x "$SRC_DIR/scripts/mkdkmsconf.sh" ] || die "missing scripts/mkdkmsconf.sh"

if ! VERSION=$("$SRC_DIR/scripts/mkdkmsconf.sh" --version); then
    die "cannot derive PACKAGE_VERSION from $SRC_DIR/tbtun/tbt_uapi.h"
fi
[ -n "$VERSION" ] || die "scripts/mkdkmsconf.sh returned an empty version"

TARGET_KERNEL=${KERNEL:-$(uname -r)}
if ! kernel_supported "$TARGET_KERNEL"; then
    die "$MODULE needs Linux $MIN_KERNEL or later; $TARGET_KERNEL is not"
fi

if [ "$STAGE_ONLY" -eq 0 ] && ! command -v dkms >/dev/null 2>&1; then
    if [ "$DRY_RUN" -eq 1 ]; then
        say "warning: dkms is not installed; the steps below would fail"
    else
        die "dkms is not installed; install it first (e.g. pacman -S dkms)"
    fi
fi

STAGE="$DKMS_ROOT/$MODULE-$VERSION"

say "Installing $MODULE $VERSION for kernel $TARGET_KERNEL"
say "  source: $SRC_DIR"
say "  stage:  $STAGE"

if [ "$DRY_RUN" -eq 1 ]; then
    printf '  + rm -rf %s\n' "$STAGE"
    printf '  + install -d %s\n' "$STAGE"
    printf '  + cp -a %s/Makefile %s/.clang-format %s\n' "$SRC_DIR" "$SRC_DIR" "$STAGE"
    printf '  + install -d %s/tbtun %s/scripts\n' "$STAGE" "$STAGE"
    printf '  + cp -a %s/tbtun/*.c %s/tbtun/*.h %s/tbtun/\n' "$SRC_DIR" "$SRC_DIR" "$STAGE"
    printf '  + cp -a %s/scripts/mkdkmsconf.sh %s/scripts/install_dkms.sh %s/scripts/\n' "$SRC_DIR" "$SRC_DIR" "$STAGE"
    printf '  + (cd %s && ./scripts/mkdkmsconf.sh > dkms.conf)\n' "$STAGE"
else
    rm -rf "$STAGE"
    install -d "$STAGE/tbtun" "$STAGE/scripts"
    cp -a "$SRC_DIR/Makefile" "$SRC_DIR/.clang-format" "$STAGE/"
    cp -a "$SRC_DIR"/tbtun/*.c "$SRC_DIR"/tbtun/*.h "$STAGE/tbtun/"
    cp -a "$SRC_DIR/scripts/mkdkmsconf.sh" "$SRC_DIR/scripts/install_dkms.sh" "$STAGE/scripts/"
    chmod +x "$STAGE/scripts/mkdkmsconf.sh" "$STAGE/scripts/install_dkms.sh"
    (cd "$STAGE" && ./scripts/mkdkmsconf.sh >dkms.conf)
fi

if [ "$STAGE_ONLY" -eq 1 ]; then
    say "Staged $MODULE $VERSION at $STAGE (dkms.conf written, nothing installed)."
    exit 0
fi

run dkms add "$STAGE"
if [ -n "$KERNEL" ]; then
    run dkms build -m "$MODULE" -v "$VERSION" -k "$KERNEL"
    run dkms install -m "$MODULE" -v "$VERSION" -k "$KERNEL"
else
    run dkms build -m "$MODULE" -v "$VERSION"
    run dkms install -m "$MODULE" -v "$VERSION"
fi
run depmod -a

if [ "$LOAD" -eq 1 ]; then
    run modprobe "$MODULE"
fi

say
if [ "$DRY_RUN" -eq 1 ]; then
    say "Dry run: nothing was changed."
else
    say "Installed. The control surface is /proc/net/tbtun:"
fi
say "  echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443' \\"
say "       > /proc/net/tbtun/config"
say "  cat /proc/net/tbtun/tunnels"
