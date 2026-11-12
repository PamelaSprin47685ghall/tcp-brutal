#!/bin/sh
# run.sh - build the module, boot the QEMU guests, run the adversarial suite,
# and print one table row per attack.
#
#   adversarial/run.sh [--no-build] [--timeout SECONDS] [--no-crash]
#                      [--ko PATH] [--kernel PATH] [--self-test]
#
# Exit codes: 0 every attack passed, 1 an attack failed or a guest died,
# 2 an input (kernel image, module, busybox) is missing.
#
# The suite is split into groups, one QEMU boot each, because a defect can
# hang the kernel: a hang in the hostile-carrier group must not hide the
# public-port results.  A group lists its phases in /adv-phases.txt; the guest
# prints PHASE_START/PHASE_END, and a started phase without an end is reported
# as a hang.
#
# Each phase declares the attacks it must produce.  A probe crash or an exec
# failure produces no ADV_RESULT line, so the missing rows are synthesized as
# FAIL with the QEMU return code instead of letting the group look clean.
#
# The kernel, busybox and headers come from ~/.cache/tbtun-build/env.sh or
# ~/.cache/tbtun-build-t52, exactly as tests/qemu/README.md documents.  The
# initramfs builder is a private copy under adversarial/qemu/ so a concurrent
# edit to tests/qemu/ cannot change what this suite boots.
set -eu

adv=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$adv/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=$(uname -r)
ko=$root/build/kernel/tbtun.ko
kernel=${TBT_ADV_KERNEL:-}
carrier_timeout=${TBT_ADV_CARRIER_TIMEOUT:-${TBT_ADV_TIMEOUT:-240}}
public_timeout=${TBT_ADV_PUBLIC_TIMEOUT:-2400}
groups=${TBT_ADV_GROUPS:-"public control resource slab final replay rmmod flush catconfig cfg_mix param port near far stall httptorn hole create"}
build=1
crash=1
self_test=0
results_file=$adv/results.txt
tmp_files=

cleanup() {
    if [ -n "${tmp_files:-}" ]; then
        rm -f $tmp_files
    fi
    return 0
}
trap cleanup EXIT

usage() {
    cat <<EOF
Usage: adversarial/run.sh [options]

  --no-build        use build/kernel/tbtun.ko without running make check
  --no-crash        skip the isolated close-flush crash repro
  --timeout SEC     timeout for non-public groups (default $carrier_timeout)
  --ko PATH         module image to boot (default $ko)
  --kernel PATH     kernel image (default: dl/vmlinuz-$rel under the cache)
  --self-test       exercise the expected-result manifest without QEMU
  -h, --help        print this help

Groups are one QEMU boot each; set TBT_ADV_GROUPS to a space-separated list
(default: $groups).  TBT_ADV_PUBLIC_TIMEOUT bounds the public group and the
stable group, which contains the 10k-cycle public phase.
EOF
}

die() {
    echo "run.sh: $*" >&2
    exit 2
}

# The documented env.sh may point at a payload cache ($T52) that holds dl/ and
# busybox/ while the entry cache only carries env.sh.
cache_dirs=$cache
if [ -f "$cache/env.sh" ]; then
    _t52=$(
        . "$cache/env.sh" > /dev/null 2>&1 || exit 1
        printf '%s' "${T52:-}"
    ) || _t52=
    if [ -n "$_t52" ]; then
        cache_dirs="$cache_dirs $_t52"
    fi
fi
case " $cache_dirs " in
*" $HOME/.cache/tbtun-build-t52 "*) ;;
*) cache_dirs="$cache_dirs $HOME/.cache/tbtun-build-t52" ;;
esac

while [ $# -gt 0 ]; do
    case $1 in
    --no-build) build=0; shift ;;
    --no-crash) crash=0; shift ;;
    --timeout)
        [ $# -ge 2 ] || die "--timeout needs a value"
        carrier_timeout=$2
        shift 2
        ;;
    --ko)
        [ $# -ge 2 ] || die "--ko needs a path"
        ko=$2
        shift 2
        ;;
    --kernel)
        [ $# -ge 2 ] || die "--kernel needs a path"
        kernel=$2
        shift 2
        ;;
    --self-test) self_test=1; shift ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "run.sh: unknown option $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

phase_list() {
    case $1 in
    stable) echo "public control resource slab final replay rmmod flush param" ;;
    public) echo "public srcfilter closedrain closedrain_timeout" ;;
    near) echo "near nearx" ;;
    far) echo "far farx" ;;
    catconfig) echo "catconfig bigcfg" ;;
    writer) echo "writer writerdrain" ;;
    closedrain) echo "closedrain closedrain_timeout" ;;
    *) echo "$1" ;;
    esac
}

# The attacks each phase must report.  Only rows the phase prints on every
# non-hanging path are listed: an optional diagnostic (init.*_setup on a
# rejected config, console.*) is not an attack result.  Probe modes that emit
# several rows list all of them, so a crash halfway through a probe is still
# visible as the missing tail.
expected_names() {
    case $1 in
    public)
        echo "init.echo_sane pub.churn pub.two pub.concurrent pub.halfclose
              pub.rst init.src_key pub.writer pub.writer_drain
              pub.writer_drain_1g"
        ;;
    control)
        echo "init.nonroot_unchanged init.cfg_unchanged init.cfg_roundtrip
              init.echo_after_roundtrip ctl.nonroot ctl.line513 ctl.line512
              ctl.line512nonl ctl.line1025 ctl.line1024nonl
              ctl.release_overflow"
        ;;
    resource) echo "init.max_tunnels init.scale" ;;
    port) echo "init.port_exhaust" ;;
    slab) echo "init.slab_leak init.slab_after_flush" ;;
    final) echo "init.final_echo init.dmesg" ;;
    replay) echo "init.cfg_replay init.echo_after_replay" ;;
    rmmod) echo "init.rmmod init.reload init.dmesg_after_rmmod" ;;
    flush) echo "init.flush_midstream init.echo_after_flush" ;;
    param) echo "init.max_carriers" ;;
    near)
        echo "near.resp_not_chunked near.resp_500 near.resp_hdr_big
              near.resp_close near.hello_wrong_nonce near.chunk_huge
              near.split_record near.chunk_oversize near.drip_pad
              near.fbk_forged near.data_path near.second_conn"
        ;;
    nearx) echo "near.fin_eagain near.rst_eagain" ;;
    far)
        echo "far.garbage far.hello_garbage far.wrong_nonce far.anchor
              far.dup_index far.srcaddr_dup far.srcaddr_free far.chunk_huge
              far.bodylen_tail far.split_record far.drip_split
              far.chunk_oversize far.no_host far.headers_big
              far.open_other_sid far.fbk_forged far.hello_profile0
              far.hello_profile9 far.data_path"
        ;;
    farx) echo "far.nonce_pinned far.src_anchor far.overlap far.second_carrier" ;;
    stall) echo "stall.hello_chain stall.flush" ;;
    httptorn) echo "cat.http_torn" ;;
    hole) echo "hole.created hole.age" ;;
    create) echo "create.baseline create.failure create.replace" ;;
    catconfig) echo "cat.cfg_self" ;;
    bigcfg) echo "cfg.big_write cfg.big_roundtrip cfg.big_replay cfg.echo_after_big" ;;
    cfg_mix) echo "cat.cfg_mix" ;;
    writer) echo "pub.writer" ;;
    writerdrain) echo "pub.writer_drain pub.writer_drain_1g" ;;
    halfclose) echo "pub.halfclose" ;;
    esac
}

has_result() { # results-file name
    awk -v n="$2" '$1 == "ADV_RESULT" && $2 == n { found = 1 } END { exit !found }' "$1"
}

# Print a FAIL row for every expected attack the guest did not report.  A
# crash (SIGSEGV, rc 139), a missing /bin/adv_probe, or a group timeout leaves
# no row at all; the group must not be able to pass by omission.
missing_results() { # results-file console sha rc phase...
    _rf=$1
    _console=$2
    _sha=$3
    _rc=$4
    shift 4
    for _phase in "$@"; do
        if grep -q "^PHASE_START $_phase" "$_console" 2> /dev/null; then
            _state="phase ran"
        else
            _state="phase never started"
        fi
        for _name in $(expected_names "$_phase"); do
            has_result "$_rf" "$_name" && continue
            echo "ADV_RESULT $_name FAIL sha=$_sha phase $_phase: no ADV_RESULT was printed ($_state; qemu rc=$_rc, so a probe crash, exec failure, or timeout; console $_console)"
        done
    done
}

self_test() {
    _dir=$(mktemp -d "${TMPDIR:-/tmp}/tbt-adv-selftest.XXXXXX") ||
        die "self-test: cannot create a temporary directory"
    trap 'rm -rf "$_dir"' EXIT
    _console=$_dir/console
    _rf=$_dir/results
    {
        echo "PHASE_START far"
        echo "ADV_RESULT far.garbage PASS sha=selftest 9000 bytes of garbage"
        echo "PHASE_END far"
        echo "TBT_ADV_DONE"
    } > "$_console"
    grep '^ADV_RESULT ' "$_console" > "$_rf"
    missing_results "$_rf" "$_console" selftest 139 far >> "$_rf"
    _dup=$(grep -c '^ADV_RESULT far.garbage ' "$_rf" || true)
    if [ "$_dup" -eq 1 ] &&
        grep -q '^ADV_RESULT far.anchor FAIL sha=selftest ' "$_rf" &&
        grep -q '^ADV_RESULT far.data_path FAIL sha=selftest ' "$_rf"; then
        echo "run.sh: self-test PASS: present row kept once, missing far.* rows synthesized as FAIL"
        _rc=0
    else
        echo "run.sh: self-test FAIL: expected-result synthesis is wrong" >&2
        cat "$_rf" >&2
        _rc=1
    fi
    rm -rf "$_dir"
    trap - EXIT
    exit "$_rc"
}

if [ "$self_test" -eq 1 ]; then
    self_test
fi

[ -x "$(command -v qemu-system-x86_64)" ] || die "qemu-system-x86_64 is not installed"
if [ -z "$kernel" ]; then
    for _d in $cache_dirs; do
        if [ -f "$_d/dl/vmlinuz-$rel" ]; then
            kernel=$_d/dl/vmlinuz-$rel
            break
        fi
    done
fi
[ -n "$kernel" ] ||
    die "no kernel image vmlinuz-$rel under: $cache_dirs (use --kernel PATH or TBT_ADV_KERNEL)"
[ -f "$kernel" ] || die "no kernel image at $kernel (see tests/qemu/README.md)"
[ -x "$adv/qemu/mkinitramfs.sh" ] || die "adversarial/qemu/mkinitramfs.sh is missing"

if [ "$build" -eq 1 ]; then
    [ -f "$cache/env.sh" ] || die "no build environment at $cache/env.sh (set TBTUN_BUILD or use --no-build)"
    echo "run.sh: building tbtun.ko (source $cache/env.sh; make check)"
    ( . "$cache/env.sh"; make -C "$root" check ) || die "the module build failed"
fi
[ -f "$ko" ] || die "no module at $ko; run make check"
echo "run.sh: module sha256 $(sha256sum "$ko" | cut -d' ' -f1)"

echo "run.sh: building adv_probe"
cc -D_GNU_SOURCE -std=gnu99 -O2 -Wall -Wextra -Werror -I "$root/src/kernel" \
    -o "$adv/adv_probe" "$adv/adv_probe.c" || die "adv_probe build failed"

qemu_dir=$cache/qemu
mkdir -p "$qemu_dir" || die "cannot create $qemu_dir"

accel=tcg
[ -w /dev/kvm ] && accel=kvm

: > "$results_file"

# Count kernel oops/warning lines on a guest console.  Harness output shares
# the serial console with printk, and a PASS detail can name the very patterns
# this scan looks for ("no WARNING/BUG/Oops/..."), so skip harness lines before
# matching.  Only timestamped kernel lines or lines that start with a kernel
# marker are considered; the rest of the console is userspace chatter.
klog_count() {
    awk '
        /^ADV_/ || /^PHASE_/ || /^TBT_/ || /^NEARX_/ || /^FAR_/ ||
        /^CONC_/ || /^REPRO_/ || /^PARTIAL_WROTE/ || /^burst:/ ||
        /^== / || /^# / { next }
        /^\[[[:space:]]*[0-9]+\.[0-9]+\]/ {
            if ($0 ~ /WARNING:|BUG:|Oops|general protection fault|Call Trace:|INFO: task .* blocked|refcount_t:|KASAN|UBSAN|deprecated workqueue/) n++
            next
        }
        /^(WARNING:|BUG:|Oops|general protection fault|Call Trace:|INFO: task|refcount_t:|KASAN|UBSAN|deprecated workqueue)/ {
            if ($0 ~ /WARNING:|BUG:|Oops|general protection fault|Call Trace:|INFO: task .* blocked|refcount_t:|KASAN|UBSAN|deprecated workqueue/) n++
            next
        }
        END { print n + 0 }
    ' "$1"
}

run_group() {
    g=$1
    phases=$(phase_list "$g")
    tmo=$2
    initrd=$qemu_dir/adv-initramfs-$g.cpio.gz
    console=$adv/console-$g.log
    # The sha of the image this guest will load.  It is captured before the
    # initramfs is built and stamped on every row of this group, because the
    # module may be rebuilt between groups.
    gsha=$(sha256sum "$ko" | cut -c1-16)

    printf '%s\n' "$phases" > "$qemu_dir/adv-phases.txt"
    "$adv/qemu/mkinitramfs.sh" --ko "$ko" --out "$initrd" \
        --init "$adv/init" --bin "$adv/adv_probe" --endpoint "$adv/adv_probe" \
        --asset "$qemu_dir/adv-phases.txt" > /dev/null ||
        die "initramfs build failed for group $g"

    echo "run.sh: group $g: qemu accel=$accel timeout=${tmo}s sha=$gsha phases=$phases"
    set +e
    timeout --foreground "$tmo" qemu-system-x86_64 \
        -accel "$accel" -cpu max -smp 2 -m 512 \
        -kernel "$kernel" \
        -initrd "$initrd" \
        -append "console=ttyS0 panic=-1 loglevel=7 tsc=unstable" \
        -nographic -no-reboot -nodefaults \
        -serial mon:stdio \
        -display none \
        > "$console" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 124 ]; then
        echo "run.sh: group $g TIMEOUT after ${tmo}s (console: $console)"
    fi

    gr=$(mktemp "${TMPDIR:-/tmp}/tbt-adv-$g-results.XXXXXX") ||
        die "cannot create a result file for group $g"
    tmp_files="$tmp_files $gr"

    grep '^ADV_RESULT ' "$console" 2> /dev/null |
        sed "s/^ADV_RESULT \([^ ]*\) \([^ ]*\) /ADV_RESULT \1 \2 sha=$gsha /" \
            > "$gr" || true

    if [ "$rc" -eq 124 ]; then
        echo "ADV_RESULT console.timeout.$g FAIL sha=$gsha group $g hit the ${tmo}s timeout; rows the guest did not print are synthesized below (console $console)" >> "$gr"
    fi
    missing_results "$gr" "$console" "$gsha" "$rc" $phases >> "$gr"

    # A phase that started but never ended is a hang; a phase that never
    # started is a group that died before it.  Only the first such phase is
    # reported: the later ones are consequences of the same stop.
    dead=0
    for p in $phases; do
        [ "$dead" -eq 1 ] && continue
        if ! grep -q "^PHASE_START $p" "$console"; then
            echo "ADV_RESULT console.missing.$p FAIL sha=$gsha phase $p never started (group $g died earlier; qemu rc=$rc; console $console)" >> "$gr"
            dead=1
        elif ! grep -q "^PHASE_END $p" "$console"; then
            echo "ADV_RESULT console.hang.$p FAIL sha=$gsha phase $p started but did not finish (group $g; qemu rc=$rc; console $console)" >> "$gr"
            dead=1
        fi
    done
    n=$(klog_count "$console")
    if [ "${n:-0}" -gt 0 ]; then
        echo "ADV_RESULT console.klog.$g FAIL sha=$gsha $n kernel oops/warning lines (console $console)" >> "$gr"
    fi
    if [ "$dead" -eq 0 ] && ! grep -q '^TBT_ADV_DONE' "$console"; then
        echo "ADV_RESULT console.done.$g FAIL sha=$gsha group $g did not reach TBT_ADV_DONE (qemu rc=$rc; console $console)" >> "$gr"
    fi

    cat "$gr" >> "$results_file"
    rm -f "$gr"
}

for g in $groups; do
    case $g in
    public) run_group public "$public_timeout" ;;
    stable) run_group stable "$public_timeout" ;;
    writer) run_group writer 600 ;;
    *) run_group "$g" "$carrier_timeout" ;;
    esac
done

# The close-flush crash is reproduced in its own guest: the oops hangs the
# machine, which would hide every later result if it ran in a group.
if [ "$crash" -eq 1 ]; then
    echo "run.sh: booting the close-flush crash repro (isolated guest)"
    csha=$(sha256sum "$ko" | cut -c1-16)
    crash_line=$("$adv/crash-repro.sh" 2> /dev/null | grep '^ADV_RESULT ' || true)
    if [ -z "$crash_line" ]; then
        crash_line="ADV_RESULT ctl.close_flush FAIL sha=$csha the crash repro produced no verdict; see $adv/crash-console.log"
    else
        crash_line=$(printf '%s\n' "$crash_line" |
            sed "s/^ADV_RESULT \([^ ]*\) \([^ ]*\) /ADV_RESULT \1 \2 sha=$csha /")
    fi
    printf '%s\n' "$crash_line" >> "$results_file"
fi

results=$(cat "$results_file")
if [ -z "$results" ]; then
    echo "run.sh: FAIL: no ADV_RESULT lines"
    exit 1
fi

echo
printf '%-32s %-6s %-18s %s\n' "ATTACK" "RESULT" "BUILD" "DETAIL"
printf '%-32s %-6s %-18s %s\n' "--------------------------------" "------" "------------------" "------"
printf '%s\n' "$results" |
    awk '{
        name=$2; res=$3; sha=$4; detail="";
        for (i = 5; i <= NF; i++) detail = detail (i > 5 ? " " : "") $i;
        printf "%-32s %-6s %-18s %s\n", name, res, sha, detail;
    }'

passes=$(printf '%s\n' "$results" | awk '$3 == "PASS" {n++} END {print n + 0}')
fails=$(printf '%s\n' "$results" | awk '$3 == "FAIL" || $3 == "TIMEOUT" {n++} END {print n + 0}')
skips=$(printf '%s\n' "$results" | awk '$3 == "SKIP" {n++} END {print n + 0}')

echo
klogs=$(grep '^ADV_KLOG ' "$results_file" || true)
if [ -n "$klogs" ]; then
    echo "kernel-log events during attacks:"
    printf '%s\n' "$klogs" | sed 's/^/    /'
fi
echo "run.sh: $passes passed, $fails failed, $skips skipped"
for g in $groups; do
    [ -f "$adv/console-$g.log" ] && echo "run.sh: group $g console: $adv/console-$g.log"
done
[ -f "$adv/crash-console.log" ] && echo "run.sh: crash console: $adv/crash-console.log"

if [ "$fails" -gt 0 ] || [ "$skips" -gt 0 ]; then
    exit 1
fi
exit 0
