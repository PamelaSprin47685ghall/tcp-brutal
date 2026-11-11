#!/bin/sh
# build.sh - build the module against the header payload, in one command.
#
#   ./tools/build.sh        (or: make check)
#
# It sources the environment written by the header extraction
# (~/.cache/tbtun-build/env.sh, see docs/build-env.md) when KDIR is not
# already set, prints the exact make command it will run, then runs it.
#
# Override TBTUN_ENV_SH to point at another env.sh; set KDIR/PAHOLE yourself to
# build against a normally installed kernel tree.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
env_sh=${TBTUN_ENV_SH:-$HOME/.cache/tbtun-build/env.sh}

if [ -z "${KDIR:-}" ] && [ -r "$env_sh" ]; then
    . "$env_sh"
fi
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}

if [ ! -d "$KDIR" ]; then
    echo "build.sh: no kernel build tree at $KDIR" >&2
    echo "build.sh: install the matching headers, or set KDIR (docs/build-env.md)" >&2
    exit 1
fi

# PAHOLE must reach make as a command-line assignment: the kernel Makefile
# assigns PAHOLE itself, which overrides the environment.
mkdir -p "$root/build/kernel"
printf 'make -C %s M=%s/src/kernel MO=%s/build/kernel%s modules\n' "$KDIR" "$root" "$root" "${PAHOLE:+ PAHOLE=$PAHOLE}"

if [ -n "${PAHOLE:-}" ]; then
    exec make -C "$KDIR" M="$root/src/kernel" MO="$root/build/kernel" PAHOLE="$PAHOLE" modules
fi
exec make -C "$KDIR" M="$root/src/kernel" MO="$root/build/kernel" modules
