#!/usr/bin/env bash
#
# install_dkms.sh - build and install the tbtun kernel module through DKMS.
#
# The module is the only artifact. Nothing is written to /usr/bin, /usr/local/bin
# or any other PATH directory; configuration happens through /proc/net/tbtun.
#
#   sudo ./tools/install_dkms.sh
#   sudo ./tools/install_dkms.sh --kernel 6.12.1-arch1-1
#   sudo ./tools/install_dkms.sh --uninstall
#   ./tools/install_dkms.sh --dry-run        # no root, prints every step
#
# The script never assumes the installed file is an uncompressed tbtun.ko: a
# kernel built with CONFIG_MODULE_COMPRESS_* stores tbtun.ko.zst (or .xz/.gz),
# so every lookup goes through the tbtun.ko* glob.
#
set -euo pipefail

PROGRAM=${0##*/}
MODULE=tbtun
MIN_KERNEL=6.1
DKMS_ROOT=${DKMS_ROOT:-/usr/src}
RUNNING_KERNEL=$(uname -r)

DRY_RUN=0
STAGE_ONLY=0
UNINSTALL=0
LOAD=1
FAILED=0
KERNEL=""
SRC_DIR=$(cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    cat <<EOF
Usage: $PROGRAM [options]

Build and install $MODULE through DKMS. Requires root unless --dry-run.

Options:
  -k, --kernel VER   build for this kernel release (default: the running one)
  -n, --dry-run      print the commands instead of running them; no root needed
  -u, --uninstall    unload $MODULE, remove every DKMS entry, the staged
                     source trees and the installed module files
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
    [ -n "$major" ] && [ -n "$minor" ] || return 1
    [ "$major" -gt "$want_major" ] && return 0
    [ "$major" -eq "$want_major" ] && [ "$minor" -ge "$want_minor" ]
}

module_loaded() {
    [ -r /proc/modules ] && grep -q "^$MODULE " /proc/modules
}

dkms_present() {
    command -v dkms >/dev/null 2>&1
}

# Read the whole status output: grep -q would close the pipe early and, under
# pipefail, turn a match into a failure. dkms may exit non-zero when no module
# matches, so its status is ignored and only grep decides. An installed entry
# ends the version with a comma ("tbtun/0.0.1, 6.12..."), an added or broken
# entry ends it with a colon ("tbtun/0.0.1: added"), so accept both.
dkms_has_version() {
    local version=$1
    { dkms status 2>/dev/null || true; } |
        grep -F -e "$MODULE/$version," -e "$MODULE/$version:" >/dev/null 2>&1
}

dkms_has_other_version() {
    local version=$1
    { dkms status 2>/dev/null || true; } |
        grep -F "$MODULE/" |
        grep -vF -e "$MODULE/$version," -e "$MODULE/$version:" >/dev/null 2>&1
}

# dkms remove/build/install all call module_is_broken_and_die, so a broken
# entry must be removed by deleting its tree directory instead.
dkms_is_broken() {
    local version=$1
    { dkms status 2>/dev/null || true; } |
        grep -F "$MODULE/$version: broken" >/dev/null 2>&1
}

# The DKMS tree defaults to /var/lib/dkms; /etc/dkms/framework.conf may
# override it. Read the last assignment, strip quotes and trailing space.
dkms_tree_path() {
    local tree=""
    if [ -r /etc/dkms/framework.conf ]; then
        tree=$(sed -n 's/^[[:space:]]*dkms_tree[[:space:]]*=[[:space:]]*"\{0,1\}\([^"#]*\)"\{0,1\}.*/\1/p' \
            /etc/dkms/framework.conf | tail -n 1 | sed 's/[[:space:]]*$//')
    fi
    printf '%s\n' "${tree:-/var/lib/dkms}"
}

# True when DIR exists and is writable, or when its nearest existing parent is.
writable_dir() {
    local dir=$1
    while [ ! -e "$dir" ]; do
        dir=$(dirname -- "$dir")
    done
    [ -d "$dir" ] && [ -w "$dir" ]
}

unload_module() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  + rmmod %s  # only if loaded\n' "$MODULE"
        return 0
    fi
    module_loaded || return 0
    say "Unloading the running $MODULE module"
    if ! rmmod "$MODULE"; then
        die "cannot unload $MODULE: it is still in use; delete its tunnels (echo 'del <id>' > /proc/net/tbtun/config) and retry"
    fi
}

# Remove every DKMS entry for the module. --all means "every kernel for this
# version", not "every version", so enumerate the versions. Broken entries
# cannot go through dkms remove; delete their tree directory instead. A
# failure is recorded in FAILED and the caller continues with the file
# cleanup, so an uninstall never stops half-way.
remove_all_dkms_entries() {
    local versions current
    if ! dkms_present; then
        say "note: dkms is not installed; skipping dkms remove"
        return 0
    fi
    versions=$({ dkms status 2>/dev/null || true; } |
        sed -n "s#^$MODULE/\([^,:]*\)[,:].*#\1#p" | sort -u) || true
    if [ "$DRY_RUN" -eq 1 ]; then
        if [ -n "$versions" ]; then
            for current in $versions; do
                if dkms_is_broken "$current"; then
                    printf '  + rm -rf %s/%s/%s  # broken DKMS entry\n' \
                        "$DKMS_TREE" "$MODULE" "$current"
                else
                    printf '  + dkms remove -m %s -v %s --all\n' \
                        "$MODULE" "$current"
                fi
            done
        else
            printf '  # no DKMS entries found for %s\n' "$MODULE"
        fi
        return 0
    fi
    [ -n "$versions" ] || return 0
    for current in $versions; do
        if dkms_is_broken "$current"; then
            say "Removing broken DKMS entry $MODULE/$current"
            if ! rm -rf "$DKMS_TREE/$MODULE/$current"; then
                printf '%s: warning: cannot remove %s/%s/%s; continuing\n' \
                    "$PROGRAM" "$DKMS_TREE" "$MODULE" "$current" >&2
                FAILED=1
            fi
            continue
        fi
        if ! run dkms remove -m "$MODULE" -v "$current" --all; then
            printf '%s: warning: dkms remove %s/%s failed; continuing cleanup\n' \
                "$PROGRAM" "$MODULE" "$current" >&2
            FAILED=1
        fi
    done
}

# The module file may be compressed by the kernel build, so match tbtun.ko*.
module_file_present() {
    local file
    for file in "/lib/modules/$TARGET_KERNEL/updates/dkms/$MODULE.ko"* \
        "/lib/modules/$TARGET_KERNEL/updates/$MODULE.ko"* \
        "/lib/modules/$TARGET_KERNEL/extra/$MODULE.ko"*; do
        [ -e "$file" ] && return 0
    done
    return 1
}

depmod_kernels() {
    local dir
    for dir in /lib/modules/*; do
        [ -d "$dir" ] || continue
        if ! run depmod -a "${dir##*/}"; then
            printf '%s: warning: depmod failed for %s; continuing\n' \
                "$PROGRAM" "${dir##*/}" >&2
            FAILED=1
        fi
    done
}

DKMS_TREE=$(dkms_tree_path)

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
        SRC_DIR=$(cd -- "$2" && pwd) || die "no such source directory: $2"
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

if [ "$UNINSTALL" -eq 1 ]; then
    if [ -n "$KERNEL" ] || [ "$STAGE_ONLY" -eq 1 ]; then
        printf '%s: note: --kernel and --stage-only do not apply to --uninstall; ignoring\n' \
            "$PROGRAM" >&2
    fi
    if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
        die "uninstalling needs root; re-run with sudo"
    fi
    say "Removing $MODULE"
    unload_module
    remove_all_dkms_entries

    say "Removing staged sources under $DKMS_ROOT"
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  + rm -rf %s/%s-*\n' "$DKMS_ROOT" "$MODULE"
    else
        for dir in "$DKMS_ROOT/$MODULE"-*; do
            [ -e "$dir" ] || continue
            if ! rm -rf "$dir"; then
                printf '%s: warning: cannot remove %s; continuing\n' \
                    "$PROGRAM" "$dir" >&2
                FAILED=1
            fi
        done
    fi

    say "Removing installed module files"
    for path in updates/dkms updates extra; do
        if ! run rm -f /lib/modules/*/"$path"/"$MODULE".ko*; then
            printf '%s: warning: cannot remove %s files under /lib/modules/*/%s; continuing\n' \
                "$PROGRAM" "$MODULE" "$path" >&2
            FAILED=1
        fi
    done

    depmod_kernels
    if [ "$FAILED" -ne 0 ]; then
        printf '%s: uninstall finished with errors; see the warnings above\n' \
            "$PROGRAM" >&2
        exit 1
    fi
    say "Done."
    exit 0
fi

[ -f "$SRC_DIR/Makefile" ] || die "no Makefile in $SRC_DIR"
[ -d "$SRC_DIR/tbtun" ] || die "no tbtun/ directory in $SRC_DIR"

if [ "$DRY_RUN" -eq 0 ] && [ "$STAGE_ONLY" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    die "installing needs root; re-run with sudo (or use --dry-run)"
fi

if [ "$DRY_RUN" -eq 0 ] && [ "$STAGE_ONLY" -eq 1 ] &&
    ! writable_dir "$DKMS_ROOT"; then
    die "$DKMS_ROOT is not writable; set DKMS_ROOT to a writable directory or run with sudo"
fi

[ -x "$SRC_DIR/tools/mkdkmsconf.sh" ] || die "missing tools/mkdkmsconf.sh"

if ! VERSION=$("$SRC_DIR/tools/mkdkmsconf.sh" --version); then
    die "cannot derive PACKAGE_VERSION from $SRC_DIR/src/kernel/tbt_uapi.h"
fi
[ -n "$VERSION" ] || die "tools/mkdkmsconf.sh returned an empty version"

TARGET_KERNEL=${KERNEL:-$RUNNING_KERNEL}
if ! kernel_supported "$TARGET_KERNEL"; then
    die "$MODULE needs Linux $MIN_KERNEL or later; $TARGET_KERNEL is not"
fi

if [ "$STAGE_ONLY" -eq 0 ]; then
    if ! dkms_present; then
        if [ "$DRY_RUN" -eq 1 ]; then
            say "warning: dkms is not installed; the steps below would fail"
        else
            die "dkms is not installed; install it first (e.g. pacman -S dkms)"
        fi
    fi
    if [ "$DRY_RUN" -eq 0 ] && [ ! -e "/lib/modules/$TARGET_KERNEL/build" ]; then
        die "kernel headers for $TARGET_KERNEL are missing (/lib/modules/$TARGET_KERNEL/build); install the matching linux-headers package"
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
    printf '  + install -d %s/src/kernel %s/tools\n' "$STAGE" "$STAGE"
    printf '  + cp -a %s/src/kernel/*.c %s/src/kernel/*.h %s/src/kernel/Kbuild %s/src/kernel/\n' "$SRC_DIR" "$SRC_DIR" "$SRC_DIR" "$STAGE"
    printf '  + cp -a %s/tools/mkdkmsconf.sh %s/tools/install_dkms.sh %s/tools/\n' "$SRC_DIR" "$SRC_DIR" "$STAGE"
    printf '  + %s/tools/mkdkmsconf.sh > %s/dkms.conf\n' "$SRC_DIR" "$STAGE"
else
    rm -rf "$STAGE" || die "cannot remove stale stage directory $STAGE"
    install -d "$STAGE/src/kernel" "$STAGE/tools"
    cp -a "$SRC_DIR/Makefile" "$SRC_DIR/.clang-format" "$STAGE/"
    cp -a "$SRC_DIR"/src/kernel/*.c "$SRC_DIR"/src/kernel/*.h \
        "$SRC_DIR"/src/kernel/Kbuild "$STAGE/src/kernel/"
    cp -a "$SRC_DIR/tools/mkdkmsconf.sh" "$SRC_DIR/tools/install_dkms.sh" "$STAGE/tools/"
    chmod +x "$STAGE/tools/mkdkmsconf.sh" "$STAGE/tools/install_dkms.sh"
    # Generate in the source tree: TBT_DKMS_GIT_SUFFIX=1 needs its git history,
    # which the stage under /usr/src does not have. The same environment then
    # decides both VERSION and dkms.conf, so the two cannot disagree.
    "$SRC_DIR/tools/mkdkmsconf.sh" >"$STAGE/dkms.conf" ||
        die "cannot generate $STAGE/dkms.conf"
    grep -F "PACKAGE_VERSION=\"$VERSION\"" "$STAGE/dkms.conf" >/dev/null ||
        die "generated $STAGE/dkms.conf does not declare PACKAGE_VERSION=$VERSION"
fi

if [ "$STAGE_ONLY" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        say "Dry run: would stage $MODULE $VERSION at $STAGE."
    else
        say "Staged $MODULE $VERSION at $STAGE (dkms.conf written, nothing installed)."
    fi
    exit 0
fi

# Only the running kernel can have the module loaded. Unloading it while
# installing for another kernel would disrupt a live tunnel.
if [ "$TARGET_KERNEL" = "$RUNNING_KERNEL" ]; then
    unload_module
fi

# A DKMS build uses its own copy of the sources. If this version is already
# registered, unbuild only the target kernel and refresh that copy; other
# kernels keep their built and installed modules. A broken entry cannot go
# through dkms unbuild (module_is_broken_and_die), so delete it and add again.
REGISTERED=0
if dkms_present && dkms_has_version "$VERSION"; then
    REGISTERED=1
fi
if [ "$REGISTERED" -eq 1 ] && dkms_is_broken "$VERSION"; then
    say "Removing broken DKMS entry $MODULE/$VERSION"
    run rm -rf "$DKMS_TREE/$MODULE/$VERSION" ||
        die "cannot remove broken DKMS entry $DKMS_TREE/$MODULE/$VERSION"
    REGISTERED=0
fi
if [ "$REGISTERED" -eq 1 ]; then
    say "Refreshing $MODULE/$VERSION for kernel $TARGET_KERNEL only"
    run dkms unbuild -m "$MODULE" -v "$VERSION" -k "$TARGET_KERNEL" ||
        die "dkms unbuild failed for $MODULE/$VERSION on $TARGET_KERNEL; see 'dkms status'"
    BUILD_TREE="$DKMS_TREE/$MODULE/$VERSION/build"
    run rm -rf "$BUILD_TREE" ||
        die "cannot remove $BUILD_TREE"
    if [ "$DRY_RUN" -eq 0 ]; then
        install -d "$BUILD_TREE" ||
            die "cannot create $BUILD_TREE"
        cp -a "$STAGE"/. "$BUILD_TREE"/ ||
            die "cannot copy $STAGE into $BUILD_TREE"
    else
        printf '  + install -d %s\n' "$BUILD_TREE"
        printf '  + cp -a %s/. %s/\n' "$STAGE" "$BUILD_TREE"
    fi
fi
if [ "$REGISTERED" -eq 0 ]; then
    run dkms add "$STAGE" ||
        die "dkms add failed for $STAGE; check 'dkms status' for a leftover $MODULE/$VERSION entry"
fi
if [ "$DRY_RUN" -eq 0 ] && dkms_present && dkms_has_other_version "$VERSION"; then
    say "note: another $MODULE DKMS version is still registered; this install replaces the module file for $TARGET_KERNEL"
fi

run dkms build -m "$MODULE" -v "$VERSION" -k "$TARGET_KERNEL" ||
    die "dkms build failed for $TARGET_KERNEL; see /var/lib/dkms/$MODULE/$VERSION/build/make.log"
run dkms install -m "$MODULE" -v "$VERSION" -k "$TARGET_KERNEL" ||
    die "dkms install failed for $TARGET_KERNEL; see 'dkms status' and the build log above"

run depmod -a "$TARGET_KERNEL" ||
    die "depmod failed for $TARGET_KERNEL"

if [ "$DRY_RUN" -eq 0 ] && ! module_file_present; then
    die "dkms install reported success, but no $MODULE.ko* is installed for $TARGET_KERNEL; check 'dkms status'"
fi

if [ "$LOAD" -eq 1 ]; then
    if [ "$TARGET_KERNEL" != "$RUNNING_KERNEL" ]; then
        say "note: built for $TARGET_KERNEL, not the running $RUNNING_KERNEL; skipping modprobe"
    elif ! run modprobe "$MODULE"; then
        printf '%s: modprobe %s failed for kernel %s\n' \
            "$PROGRAM" "$MODULE" "$TARGET_KERNEL" >&2
        printf '  installed module files:\n' >&2
        for file in "/lib/modules/$TARGET_KERNEL/updates/dkms/$MODULE.ko"* \
            "/lib/modules/$TARGET_KERNEL/updates/$MODULE.ko"* \
            "/lib/modules/$TARGET_KERNEL/extra/$MODULE.ko"*; do
            [ -e "$file" ] && printf '    %s\n' "$file" >&2
        done
        printf '  check dmesg for the load error\n' >&2
        exit 1
    fi
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
