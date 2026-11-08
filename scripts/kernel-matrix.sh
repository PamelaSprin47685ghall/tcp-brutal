#!/usr/bin/env bash
# kernel-matrix.sh - build tbtun against a matrix of kernel series and
# architectures, run the host tests in every cell, and - where a QEMU binary
# for the architecture exists - load and unload the module twice in a guest.
#
#   scripts/kernel-matrix.sh                    # 5.15/6.6/6.12/7.0 x x86_64/aarch64
#   scripts/kernel-matrix.sh --no-load          # build + host tests only
#   scripts/kernel-matrix.sh --only 6.12.116:x86_64 --keep
#   scripts/kernel-matrix.sh --list
#
# One cell is one (kernel, architecture) pair.  The kernel tarball is the
# official kernel.org one, fetched from a mirror and cached, so the headers a
# cell builds against are exactly the tarball's own tree after defconfig +
# modules_prepare.  Every cell gets a private copy of this repository, so the
# module build and the host tests never touch the checkout or each other.
#
# Results live under ~/.cache/tbtun-kmatrix/work/<version>-<arch>/:
#   tbtun.ko      the built module
#   cell.log      every command and its output
#   module.log    the module build (undefined-symbol warnings included)
#   host.log      make host-test
#   image.log     the kernel image build
#   qemu.log      the guest console, when a load check ran
#   status.*      tiny KEY=VALUE files (module, host, load, verdict, reason,
#                 elapsed, compiler, headers, vermagic)
#   report.md     the whole matrix, written by the parent at the end
#
# Exit status: 0 when no cell failed (a build-only cell counts as a pass, with
# the report saying why it could not be loaded), 1 otherwise.
set -uo pipefail

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEFAULT_VERSIONS="5.15.225 6.6.162 6.12.116 7.0.14"
DEFAULT_ARCHES="x86_64 aarch64"
CACHE=${KM_CACHE:-$HOME/.cache/tbtun-kmatrix}
WORK=${KM_WORK:-$CACHE/work}
MIRROR=${KM_KERNEL_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/kernel}
JOBS=${KM_JOBS:-1}
NPROC=$(nproc 2>/dev/null || echo 4)
MAKE_JOBS=${KM_MAKE_JOBS:-$((NPROC / JOBS))}
[ "$MAKE_JOBS" -ge 1 ] || MAKE_JOBS=1
[ "$MAKE_JOBS" -le 12 ] || MAKE_JOBS=12
TIMEOUT=${KM_TIMEOUT:-300}
MIN_FREE_GB=${KM_MIN_FREE_GB:-5}
HOSTCC=${HOSTCC:-cc}
KEEP=${KM_KEEP:-0}
LOAD_MODE=auto
QEMU_AARCH64=${KM_QEMU_AARCH64:-}

versions=
arches=
only=

usage() {
    printf '%s\n' \
        "usage: scripts/kernel-matrix.sh [options]" \
        "" \
        "  --versions LIST     comma/space separated tarball versions" \
        "  --arches LIST       x86_64, aarch64" \
        "  --only VER:ARCH     run one cell (repeatable)" \
        "  --jobs N            cells in parallel (default 1)" \
        "  --make-jobs N       make -j inside a cell (default nproc/jobs, max 12)" \
        "  --cache DIR         default ~/.cache/tbtun-kmatrix" \
        "  --work DIR          default <cache>/work" \
        "  --timeout SEC       QEMU timeout (default 300)" \
        "  --qemu-aarch64 PATH override the aarch64 emulator" \
        "  --no-load           skip the QEMU load check" \
        "  --keep              keep the kernel tree and the source copy" \
        "  --list              print the cells and exit"
}

die() {
    echo "kernel-matrix: $*" >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
    --versions) versions=$2; shift 2 ;;
    --arches) arches=$2; shift 2 ;;
    --only) only="$only $2"; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --make-jobs) MAKE_JOBS=$2; shift 2 ;;
    --cache) CACHE=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    --qemu-aarch64) QEMU_AARCH64=$2; shift 2 ;;
    --no-load) LOAD_MODE=none; shift ;;
    --load) LOAD_MODE=auto; shift ;;
    --keep) KEEP=1; shift ;;
    --list) LIST=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown option $1 (try --help)" ;;
    esac
done

[ "$MAKE_JOBS" -ge 1 ] || die "--make-jobs must be >= 1"
[ "$JOBS" -ge 1 ] || die "--jobs must be >= 1"
[ "$TIMEOUT" -ge 30 ] || die "--timeout must be >= 30"

# A version string names one stable tarball; its series directory is the
# leading component (5.15.225 -> v5.x).  Keep the mapping in one place so the
# report and the URL can never disagree.
series_of() {
    printf 'v%s.x' "${1%%.*}"
}

cell_versions=$DEFAULT_VERSIONS
cell_arches=$DEFAULT_ARCHES
[ -n "$versions" ] && cell_versions=$(printf '%s' "$versions" | tr ',' ' ')
[ -n "$arches" ] && cell_arches=$(printf '%s' "$arches" | tr ',' ' ')

cells=()
add_cell() {
    local ver=$1 arch=$2
    case " $cell_versions " in *" $ver "*) ;; *) die "version $ver is not in --versions" ;; esac
    case " $cell_arches " in *" $arch "*) ;; *) die "arch $arch is not in --arches" ;; esac
    cells+=("$ver:$arch")
}

if [ -n "$only" ]; then
    for item in $only; do
        case $item in
        *:*) add_cell "${item%%:*}" "${item##*:}" ;;
        *) for arch in $cell_arches; do add_cell "$item" "$arch"; done ;;
        esac
    done
else
    for ver in $cell_versions; do
        for arch in $cell_arches; do
            add_cell "$ver" "$arch"
        done
    done
fi

if [ "${LIST:-0}" -eq 1 ]; then
    printf '%s\n' "${cells[@]}"
    exit 0
fi

mkdir -p "$CACHE/dl" "$WORK"

# ---------------------------------------------------------------------------
# Inputs shared by the cells: tarballs, the guest init, and qemu-system-aarch64.
# Doing this once in the parent avoids two cells writing the same cache entry.

fetch_one() {
    local ver=$1 url dest
    url="$MIRROR/$(series_of "$ver")/linux-$ver.tar.xz"
    dest=$CACHE/dl/linux-$ver.tar.xz
    if [ ! -s "$dest" ]; then
        echo "kernel-matrix: fetching $url" >&2
        curl -fsSL --retry 3 -o "$dest.part" "$url" || return 1
        mv "$dest.part" "$dest" || return 1
    fi
    [ -f "$dest.sha256" ] || sha256sum "$dest" | sed 's/ .*//' > "$dest.sha256"
    printf '%s  %s\n' "$(cat "$dest.sha256")" "$dest"
}

fetch_all() {
    local ver pids=() failed=0
    for ver in $(printf '%s\n' "${cells[@]}" | cut -d: -f1 | sort -u); do
        fetch_one "$ver" > "$CACHE/dl/$ver.fetch" 2>> "$CACHE/dl/fetch.log" &
        pids+=($!)
    done
    for pid in "${pids[@]}"; do
        wait "$pid" || failed=1
    done
    [ "$failed" -eq 0 ] || die "a kernel tarball failed to download (see $CACHE/dl/fetch.log)"
}

ensure_init() {
    local arch=$1 src=$REPO/tests/kernel-matrix/init.c
    local out=$CACHE/init/init_$arch
    local cc
    case $arch in
    x86_64) cc=$HOSTCC ;;
    aarch64)
        command -v aarch64-linux-gnu-gcc > /dev/null 2>&1 || return 1
        cc=aarch64-linux-gnu-gcc
        ;;
    *) return 1 ;;
    esac
    mkdir -p "$CACHE/init"
    if [ ! -x "$out" ] || [ "$src" -nt "$out" ]; then
        "$cc" -static -nostdlib -ffreestanding -O2 -fno-stack-protector \
            -fno-pie -no-pie -o "$out" "$src" || return 1
    fi
    printf '%s\n' "$out"
}

ensure_qemu_aarch64() {
    if [ -n "$QEMU_AARCH64" ] && [ -x "$QEMU_AARCH64" ]; then
        printf '%s\n' "$QEMU_AARCH64"
        return 0
    fi
    if command -v qemu-system-aarch64 > /dev/null 2>&1; then
        command -v qemu-system-aarch64
        return 0
    fi
    # Split these: with `set -u`, the expansion of $dir in the same `local`
    # statement is an unbound variable, and the function would silently fail.
    local dir bin
    dir=$CACHE/tool/qemu-aarch64
    bin=$dir/usr/bin/qemu-system-aarch64
    if [ -x "$bin" ]; then
        printf '%s\n' "$bin"
        return 0
    fi
    command -v pacman > /dev/null 2>&1 || return 1
    local url
    url=$(pacman -Sp qemu-system-aarch64 2> /dev/null | grep -m1 '^https\?://' || true)
    [ -n "$url" ] || return 1
    mkdir -p "$dir"
    curl -fsSL --retry 2 -o "$dir/qemu.pkg.tar.zst" "$url" || return 1
    tar --zstd -xf "$dir/qemu.pkg.tar.zst" -C "$dir" || return 1
    [ -x "$bin" ] || return 1
    printf '%s\n' "$bin"
}

fetch_all

# Do not use `printf ... | grep -q` here: grep -q exits as soon as it matches,
# printf then dies of SIGPIPE, and pipefail turns the whole test false.
need_arm=0
case " ${cells[*]} " in
*":aarch64 "*) need_arm=1 ;;
esac

qemu_aarch64_bin=
if [ "$need_arm" -eq 1 ]; then
    qemu_aarch64_bin=$(ensure_qemu_aarch64 2> /dev/null || true)
    if [ -z "$qemu_aarch64_bin" ]; then
        echo "kernel-matrix: qemu-system-aarch64 unavailable; aarch64 cells are build-only" >&2
    fi
fi
ensure_init x86_64 > /dev/null 2>&1 || true
if [ "$need_arm" -eq 1 ]; then
    ensure_init aarch64 > /dev/null 2>&1 || true
fi

# ---------------------------------------------------------------------------
# One cell.

status() {
    printf '%s\n' "$2" > "$d/status.$1"
}

record_error() {
    local kind=$1 log=$2 detail
    if [ ! -s "$log" ]; then
        status reason "$kind: log is empty"
        return
    fi
    detail=$(grep -a -m1 -E 'error:|Error [0-9]+|undefined!' "$log" | cut -c1-240)
    [ -n "$detail" ] || detail=$(grep -a -v '^[[:space:]]*$' "$log" | tail -n 1 | cut -c1-240)
    status reason "$kind: $detail"
}

classify_build_error() {
    local log=$1
    if grep -qa 'No space left on device' "$log"; then
        echo disk
    elif grep -qaE 'unrecognized command-line option|internal compiler error|cc1: |assembler|linker|execvp|cannot find -l|No such file or directory' "$log"; then
        echo toolchain
    elif grep -qaE 'no member named|too few arguments|too many arguments|implicit declaration|unknown type name|incompatible|redefinition|has no member|unknown field|undeclared|invalid use of|expression has changed|configured with' "$log"; then
        echo kernel-api
    elif grep -qaE 'Kconfig|invalid configuration|configuration is invalid' "$log"; then
        echo config
    else
        echo build
    fi
}

latest_error_line() {
    grep -a -E 'error:|Error [0-9]+|undefined!' "$1" | tail -n 1 | cut -c1-240
}

run_cell() {
    local ver=$1 arch=$2
    d=$WORK/$ver-$arch
    mkdir -p "$d"
    # Every early return skips the normal cleanup at the end of the function.
    # The EXIT trap covers all of them, so a failed cell does not leave a
    # 1-2 GiB kernel tree behind.
    trap 'if [ "$KEEP" -eq 0 ]; then rm -rf "$d/kernel" "$d/src"; fi' EXIT
    exec >> "$d/cell.log" 2>&1
    echo "=== cell $ver $arch start $(date -Is) ==="

    local start elapsed
    start=$(date +%s)
    status module fail
    status host na
    status load skip
    status load_reason ""
    status verdict FAIL
    status reason "incomplete"
    status kernel "$ver"
    status arch "$arch"

    local free_gb
    free_gb=$(df -Pk "$WORK" 2> /dev/null | awk 'NR == 2 { print int($4 / 1024 / 1024) }')
    status disk_free_gb "${free_gb:-unknown}"
    if [ -n "$free_gb" ] && [ "$free_gb" -lt "$MIN_FREE_GB" ]; then
        status reason "disk: ${free_gb}GB free, need ${MIN_FREE_GB}GB"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi

    local tarball=$CACHE/dl/linux-$ver.tar.xz
    if [ ! -s "$tarball" ]; then
        status reason "download: $tarball missing"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi
    status tarball_sha256 "$(cat "$tarball.sha256" 2> /dev/null || sha256sum "$tarball" | sed 's/ .*//')"
    status tarball_url "$MIRROR/$(series_of "$ver")/linux-$ver.tar.xz"
    status headers "kernel.org tarball $(basename "$tarball") (defconfig + modules_prepare)"

    local cross= karch=x86
    if [ "$arch" = aarch64 ]; then
        cross=aarch64-linux-gnu-
        karch=arm64
    fi

    local kd=$d/kernel
    local src=$d/src
    local ko=$src/tbtun.ko

    # 1. Kernel tree.
    if [ ! -f "$kd/Makefile" ]; then
        mkdir -p "$kd"
        echo "--- extract $tarball"
        tar -xJf "$tarball" -C "$kd" --strip-components=1 || {
            status reason "extract: tar failed"
            status elapsed "$(( $(date +%s) - start ))"
            return 0
        }
    fi
    status kernel_source "$kd"

    local ccdesc=
    case $arch in
    x86_64) ccdesc=$("$HOSTCC" --version | head -n 1) ;;
    aarch64) ccdesc=$(aarch64-linux-gnu-gcc --version | head -n 1) ;;
    esac
    status compiler "$ccdesc"

    local kbuild=(make -C "$kd" ARCH="$karch" CROSS_COMPILE="$cross" -j"$MAKE_JOBS")

    # 2. defconfig + the options the matrix needs.  DEBUG_INFO/BTF are off
    # because this is a load check, not a debug build: pahole is not installed,
    # and the BTF objects would dwarf the rest of the tree.  WERROR is off so a
    # new compiler warning is reported as a warning, not as a kernel build
    # failure; the module's own build is still judged on its first error.
    # LIBCRC32C is the one symbol defconfig leaves as a module while tbtun
    # needs it in the guest; the load check found it on 6.12.
    echo "--- defconfig"
    if ! "${kbuild[@]}" defconfig; then
        status reason "config: defconfig failed"
        record_error config "$d/cell.log"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi
    "$kd/scripts/config" --file "$kd/.config" \
        -d DEBUG_INFO -d DEBUG_INFO_BTF -d DEBUG_INFO_BTF_MODULES \
        -d GDB_SCRIPTS -d MODULE_SIG -d MODULE_SIG_ALL -d MODULE_COMPRESS \
        -d WERROR -e LIBCRC32C \
        --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
        > /dev/null
    if ! "${kbuild[@]}" olddefconfig; then
        status reason "config: olddefconfig failed"
        record_error config "$d/cell.log"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi
    if ! "${kbuild[@]}" modules_prepare; then
        local kind
        kind=$(classify_build_error "$d/cell.log")
        status reason "prepare/${kind}: $(latest_error_line "$d/cell.log")"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi
    status modules_prepare "$(grep -m1 '^VERSION\|^PATCHLEVEL\|^SUBLEVEL' "$kd/Makefile" | tr '\n' ' ')"
    status kernel_release "$(make -s -C "$kd" ARCH="$karch" CROSS_COMPILE="$cross" kernelrelease 2> /dev/null || true)"

    # 3. Module build against this tree, in a private copy of the repository.
    echo "--- module build"
    mkdir -p "$src"
    rsync -a --delete \
        --exclude '.git' --exclude 'review' --exclude 'benches/results' \
        --exclude '*.ko' --exclude '*.o' --exclude '*.mod' --exclude '*.mod.c' \
        --exclude 'modules.order' --exclude 'Module.symvers' \
        "$REPO"/ "$src"/ || {
        status reason "rsync: cannot stage the source tree"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    }
    # modules_prepare leaves no vmlinux and therefore no Module.symvers, so
    # modpost cannot resolve the module's external symbols.  KBUILD_MODPOST_WARN
    # turns that expected condition into warnings; the guest load check is what
    # proves the symbols actually exist in the built kernel.
    local module_ok=0
    if ! make -C "$kd" M="$src" ARCH="$karch" CROSS_COMPILE="$cross" \
        KBUILD_MODPOST_WARN=1 -j"$MAKE_JOBS" modules > "$d/module.log" 2>&1; then
        status reason "module/$(classify_build_error "$d/module.log"): $(latest_error_line "$d/module.log")"
    elif [ ! -f "$ko" ]; then
        status reason "module: make succeeded but $ko is missing"
    else
        cp -f "$ko" "$d/tbtun.ko"
        module_ok=1
        status module ok
        if command -v modinfo > /dev/null 2>&1; then
            status modinfo_name "$(modinfo -F name "$d/tbtun.ko" 2> /dev/null || true)"
            status vermagic "$(modinfo -F vermagic "$d/tbtun.ko" 2> /dev/null || true)"
            status modinfo_depends "$(modinfo -F depends "$d/tbtun.ko" 2> /dev/null || true)"
            status modinfo_version "$(modinfo -F version "$d/tbtun.ko" 2> /dev/null || true)"
            if [ "$(cat "$d/status.modinfo_name")" != tbtun ] ||
                [ -z "$(cat "$d/status.vermagic")" ]; then
                module_ok=0
                status module fail
                status reason "modinfo: name=$(cat "$d/status.modinfo_name") vermagic=$(cat "$d/status.vermagic")"
            fi
        fi
        modinfo "$d/tbtun.ko" > "$d/modinfo.txt" 2>&1 || true
    fi

    # 4. Host tests: the same parser sources the module uses, built for the
    # host.  They cannot touch the guest and need no kernel tree, so they run
    # even when the module build failed: a cell records both results.
    echo "--- host tests"
    if make -C "$src" host-test HOSTCC="$HOSTCC" > "$d/host.log" 2>&1; then
        status host ok
    else
        status host fail
        status reason "host-test: $(latest_error_line "$d/host.log")"
        status elapsed "$(( $(date +%s) - start ))"
        return 0
    fi

    # 5. Load check: build the matching kernel image, boot it with an initramfs
    # holding only the module and the static checker, and let the guest load
    # and unload the module twice.
    if [ "$module_ok" -eq 0 ]; then
        status load skip
        status load_reason "module not built"
    elif [ "$LOAD_MODE" = auto ]; then
        local qemu= qargs=() init=
        case $arch in
        x86_64)
            if command -v qemu-system-x86_64 > /dev/null 2>&1; then
                qemu=$(command -v qemu-system-x86_64)
                qargs=(-accel kvm -cpu host)
                if [ ! -w /dev/kvm ]; then qargs=(-accel tcg -cpu max); fi
                qargs+=(-m 512 -smp 2 -nic none -kernel "$d/bzImage" -initrd "$d/initramfs.cpio.gz"
                       -append "console=ttyS0 panic=-1 loglevel=7"
                       -display none -serial mon:stdio -no-reboot)
            fi
            ;;
        aarch64)
            if [ -n "$qemu_aarch64_bin" ]; then
                qemu=$qemu_aarch64_bin
                # -nic none: without it qemu's default virtio-net asks for
                # efi-virtio.rom, which an unpacked qemu-system-aarch64 cannot
                # find even when qemu-common ships it.  The guest has no
                # network anyway.
                qargs=(-machine virt -accel tcg -cpu cortex-a57 -m 512 -smp 2 -nic none
                       -kernel "$d/Image" -initrd "$d/initramfs.cpio.gz"
                       -append "console=ttyAMA0 panic=-1 loglevel=7"
                       -display none -serial mon:stdio -no-reboot)
            fi
            ;;
        esac

        if [ -z "$qemu" ]; then
            status load skip
            status load_reason "no qemu for $arch"
        else
            echo "--- kernel image for load check"
            local img_ok=0
            build_img() {
                if [ "$arch" = x86_64 ]; then
                    "${kbuild[@]}" bzImage > "$d/image.log" 2>&1 &&
                        cp -f "$kd/arch/x86/boot/bzImage" "$d/bzImage"
                else
                    "${kbuild[@]}" Image > "$d/image.log" 2>&1 &&
                        cp -f "$kd/arch/arm64/boot/Image" "$d/Image"
                fi
            }
            if build_img; then
                img_ok=1
            else
                local ikind
                ikind=$(classify_build_error "$d/image.log")
                if [ "$ikind" = disk ]; then
                    # Another process on the host can fill the filesystem
                    # between the free-space check and the link step.  Wait
                    # once for the space to come back, then retry the
                    # incremental build.
                    echo "--- image build hit a full disk; retrying after 60s"
                    sleep 60
                    free_gb=$(df -Pk "$WORK" 2> /dev/null | awk 'NR == 2 { print int($4 / 1024 / 1024) }')
                    if [ -n "$free_gb" ] && [ "$free_gb" -ge "$MIN_FREE_GB" ] &&
                        build_img; then
                        img_ok=1
                    fi
                fi
            fi
            if [ "$img_ok" -ne 1 ]; then
                status load fail
                status load_reason "image/$(classify_build_error "$d/image.log"): $(latest_error_line "$d/image.log")"
                status reason "load: kernel image build failed"
            else
                status kernel_image "$d/$([ "$arch" = x86_64 ] && echo bzImage || echo Image)"
                init=$CACHE/init/init_$arch
                if [ ! -x "$init" ]; then
                    status load skip
                    status load_reason "guest init for $arch was not built"
                elif ! python3 "$REPO/tests/kernel-matrix/mkinitramfs.py" \
                    --init "$init" --ko "$d/tbtun.ko" --out "$d/initramfs.cpio.gz" \
                    > "$d/initramfs.log" 2>&1; then
                    status load fail
                    status load_reason "initramfs: mkinitramfs.py failed"
                else
                    local qenv=()
                    if [ "$arch" = aarch64 ]; then
                        qenv=(env "LD_LIBRARY_PATH=$CACHE/tool/qemu-aarch64/usr/lib")
                    fi
                    local qrc=0
                    timeout --foreground "$TIMEOUT" "${qenv[@]}" "$qemu" "${qargs[@]}" \
                        > "$d/qemu.log" 2>&1 || qrc=$?
                    status qemu_rc "$qrc"
                    # The guest console uses CRLF, so do not anchor at $.
                    if grep -qa 'KMATRIX_LOAD_PASS' "$d/qemu.log"; then
                        status load PASS
                    else
                        status load fail
                        local why
                        why=$(grep -aE 'KMATRIX_LOAD_FAIL|Kernel panic|BUG:|Call Trace|insmod|invalid module format' "$d/qemu.log" | tail -n 1 | tr -d '\r' | cut -c1-240)
                        [ -n "$why" ] || why="qemu rc=$qrc, no KMATRIX_LOAD_PASS"
                        status load_reason "$why"
                        status reason "load: $why"
                    fi
                fi
            fi
        fi
    else
        status load skip
        status load_reason "not requested (--no-load)"
    fi

    # 6. Verdict.
    local load
    load=$(cat "$d/status.load" 2> /dev/null || echo skip)
    if [ "$(cat "$d/status.module" 2> /dev/null)" = ok ] &&
        [ "$(cat "$d/status.host" 2> /dev/null)" = ok ]; then
        case $load in
        PASS) status verdict PASS; status reason ""; status load_reason "" ;;
        skip) status verdict BUILD_ONLY; status reason "" ;;
        *) status verdict FAIL ;;
        esac
    fi

    elapsed=$(( $(date +%s) - start ))
    status elapsed "$elapsed"
    if [ "$KEEP" -eq 0 ]; then
        rm -rf "$kd" "$src"
    fi
    echo "=== cell $ver $arch done verdict=$(cat "$d/status.verdict") elapsed=${elapsed}s ==="
    return 0
}

# ---------------------------------------------------------------------------
# Run the cells with a bounded number in flight.

matrix_start=$(date +%s)
running=0
for cell in "${cells[@]}"; do
    while [ "$running" -ge "$JOBS" ]; do
        wait -n || true
        running=$((running - 1))
    done
    ver=${cell%%:*}
    arch=${cell##*:}
    echo "kernel-matrix: start $ver $arch"
    run_cell "$ver" "$arch" &
    running=$((running + 1))
done
wait
matrix_wall=$(( $(date +%s) - matrix_start ))

# ---------------------------------------------------------------------------
# Collect and report.

get() {
    local dir=$1 key=$2
    cat "$dir/status.$key" 2> /dev/null | tr '|' '/' || true
}

rows=()
for cell in "${cells[@]}"; do
    ver=${cell%%:*}
    arch=${cell##*:}
    dir=$WORK/$ver-$arch
    verdict=$(get "$dir" verdict)
    [ -n "$verdict" ] || verdict=FAIL
    rows+=("$ver|$arch|$(get "$dir" module)|$(get "$dir" host)|$(get "$dir" load)|$verdict|$(get "$dir" compiler)|$(get "$dir" tarball_sha256)|$(get "$dir" vermagic)|$(get "$dir" elapsed)|$(get "$dir" reason)|$(get "$dir" load_reason)")
done

printf '\n%-12s %-8s %-7s %-6s %-9s %-11s %-9s %s\n' kernel arch module host load verdict wall reason
for row in "${rows[@]}"; do
    IFS='|' read -r ver arch module host load verdict compiler sha vermagic elapsed reason load_reason <<< "$row"
    printf '%-12s %-8s %-7s %-6s %-9s %-11s %-9s %s\n' \
        "$ver" "$arch" "${module:-na}" "${host:-na}" "${load:-skip}" "$verdict" \
        "${elapsed:-?}s" \
        "${reason:-${load_reason:-}}"
done

report=$WORK/report.md
{
    echo "# tbtun kernel matrix"
    echo
    echo "Run: $(date -Is) on $(uname -srm), $NPROC CPUs, $JOBS cells in parallel,"
    echo "make -j$MAKE_JOBS per cell.  Cache: \`$CACHE\`."
    echo "Total wall clock: ${matrix_wall}s."
    echo
    echo "| kernel | arch | module | host tests | load check | verdict | headers | compiler | tarball sha256 | vermagic | wall | note |"
    echo "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"
    for row in "${rows[@]}"; do
        IFS='|' read -r ver arch module host load verdict compiler sha vermagic elapsed reason load_reason <<< "$row"
        note=${reason:-${load_reason:-}}
        url=$(get "$WORK/$ver-$arch" tarball_url)
        echo "| $ver | $arch | ${module:-na} | ${host:-na} | ${load:-skip} | $verdict | kernel.org $(basename "${url:-unknown}") | ${compiler:-unknown} | ${sha:-unknown} | ${vermagic:-unknown} | ${elapsed:-?}s | ${note:-} |"
    done
    echo
    echo "Headers: each cell builds against its own kernel.org tarball after"
    echo "\`defconfig\` + \`modules_prepare\`; the compiler column is the one that"
    echo "built the kernel and the module.  The host tests use \`$HOSTCC\`."
    echo
    echo "## Per-cell logs"
    echo
    for cell in "${cells[@]}"; do
        ver=${cell%%:*}
        arch=${cell##*:}
        echo "- \`$WORK/$ver-$arch/\` (cell.log, module.log, host.log, qemu.log, modinfo.txt)"
    done
} > "$report"

du -sh "$CACHE" > "$WORK/cache-size.txt" 2> /dev/null || true
du -sh "$WORK" > "$WORK/work-size.txt" 2> /dev/null || true
echo
echo "kernel-matrix: report at $report"
echo "kernel-matrix: cache $(cat "$WORK/cache-size.txt" 2> /dev/null || echo unknown)"

rc=0
for row in "${rows[@]}"; do
    IFS='|' read -r _ver _arch _module _host _load verdict _rest <<< "$row"
    [ "$verdict" = FAIL ] && rc=1
done
exit "$rc"
