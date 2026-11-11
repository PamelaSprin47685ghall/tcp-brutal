#!/usr/bin/env bash
#
# mkdkmsconf.sh - emit dkms.conf with the version derived from tbt_uapi.h.
#
# The TBT_VERSION_MAJOR/MINOR/PATCH macros are the single version authority.
# The default PACKAGE_VERSION is the pure semantic version
# "major.minor.patch". A development suffix is opt-in:
#
#   TBT_DKMS_GIT_SUFFIX=1 ./tools/mkdkmsconf.sh
#       -> "major.minor.patch.r<commits>.<sha>"
#
# `--version` prints only the version (no dkms.conf) for install scripts.
# Parsing failures are fatal; this script never falls back to a stale value.
#
# DEST_MODULE_LOCATION is mandatory: dkms' read_conf rejects a dkms.conf that
# omits it. "/updates/dkms" is one of the accepted prefixes (/kernel, /updates,
# /extra) and is exactly the install tree that install_dkms.sh --uninstall
# cleans; the same directive makes dkms add and dkms ldtarball work.
set -euo pipefail

cd "$(dirname "$0")/.."

UAPI=src/kernel/tbt_uapi.h

die() {
    printf 'mkdkmsconf.sh: %s\n' "$*" >&2
    exit 1
}

[ -r "$UAPI" ] || die "cannot read $UAPI"

# Exactly one active, numeric #define for the requested macro.
version_field() {
    local macro=$1 value

    value=$(awk -v macro="$macro" '
        $1 == "#define" && $2 == macro {
            if ($3 ~ /^[0-9]+$/) {
                print $3
                n++
            } else {
                bad = 1
            }
        }
        END {
            if (bad || n != 1)
                exit 1
        }
    ' "$UAPI") || die "expected exactly one numeric #define ${macro} in ${UAPI}"

    printf '%s\n' "$value"
}

MAJOR=$(version_field TBT_VERSION_MAJOR)
MINOR=$(version_field TBT_VERSION_MINOR)
PATCH=$(version_field TBT_VERSION_PATCH)
VERSION="${MAJOR}.${MINOR}.${PATCH}"

if [ "${TBT_DKMS_GIT_SUFFIX:-0}" = "1" ]; then
    command -v git >/dev/null 2>&1 || die "TBT_DKMS_GIT_SUFFIX=1 needs git"
    git rev-parse --git-dir >/dev/null 2>&1 ||
        die "TBT_DKMS_GIT_SUFFIX=1 needs a git work tree"
    VERSION="${VERSION}.r$(git rev-list --count HEAD).$(git rev-parse --short HEAD)"
fi

case "${1:-}" in
--version)
    printf '%s\n' "$VERSION"
    exit 0
    ;;
"") ;;
*)
    die "unknown argument: $1"
    ;;
esac

cat <<EOF
PACKAGE_NAME="tbtun"
PACKAGE_VERSION="${VERSION}"

MAKE[0]="make KERNEL_DIR=\${kernel_source_dir} all"
CLEAN="make KERNEL_DIR=\${kernel_source_dir} clean"

BUILT_MODULE_NAME[0]="tbtun"
BUILT_MODULE_LOCATION[0]="build/kernel"
DEST_MODULE_LOCATION[0]="/updates/dkms"

AUTOINSTALL="yes"
EOF
