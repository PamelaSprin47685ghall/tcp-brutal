#!/bin/sh
# run.sh - boot the kernel in QEMU, run tests/qemu/init, print and judge it.
#
#   tests/qemu/run.sh [--ko PATH] [--kernel PATH] [--timeout SECONDS]
#                     [--init PATH] [--bin PATH]... [--log PATH]
#                     [--probes A,B,C] [--no-net] [--allow-skip]
#
# Exit codes:
#   0  the guest printed TBT_TEST_PASS
#   1  the guest printed TBT_TEST_FAIL (the reason is on the console),
#      exited non-zero, or timed out
#   2  a required input is missing (no kernel image, no initramfs built)
#
# The kernel and everything the guest runs come from ~/.cache/tbtun-build;
# nothing here writes outside it or the repository. README.md explains where
# each piece comes from and why it matches the module's vermagic.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cache=${TBTUN_BUILD:-$HOME/.cache/tbtun-build}
rel=$(uname -r)

ko=$root/tbtun.ko
kernel=$cache/dl/vmlinuz-$rel
init_src=
bins=
assets=
log=
initrd=$cache/qemu/initramfs.cpio.gz
timeout_s=120
smp=${TBT_TEST_SMP:-2}
probes=${TBT_PROBES:-A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R}
net=1
# Guest carrier port that the near side reaches through the qemu gateway.
car_port=${TBT_CAR_PORT:-19101}
hostfwd_port=${TBT_HOSTFWD_PORT:-$car_port}

while [ $# -gt 0 ]; do
    case $1 in
    --ko) ko=$2; shift 2 ;;
    --kernel) kernel=$2; shift 2 ;;
    --init) init_src=$2; shift 2 ;;
    --bin) bins="$bins --bin $2"; shift 2 ;;
    --asset) assets="$assets --asset $2"; shift 2 ;;
    --log) log=$2; shift 2 ;;
    --timeout) timeout_s=$2; shift 2 ;;
    --probes) probes=$2; shift 2 ;;
    --no-net) net=0; shift ;;
    --allow-skip) allow_skip=1; shift ;;
    -h | --help)
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
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
[ -f "$ko" ] || die "no module at $ko (run make check first)"

# virtio_net is a module in this kernel, and so are its two dependencies, so
# the guest gets them decompressed from the running kernel's module tree.
# Without them the NIC exists but no driver binds, and eth0 never appears.
net_assets=
if [ "$net" -eq 1 ]; then
    netdir=$cache/qemu/net
    mkdir -p "$netdir"
    # The qdiscs and the u32 classifier are modules too; tc loads them by name
    # through /lib/modules, which the guest does not have.
    # tcp_bbr is not part of the data path's framing, but the module sets
    # TCP_CONGESTION=bbr on every carrier. Without the congestion module the
    # guest silently keeps cubic ("failed: -22 actual=cubic"), which is not
    # the transport the production tunnel and probe I measure.
    for m in failover net_failover virtio_net sch_htb sch_netem cls_u32 sch_tbf tcp_bbr; do
        src=$(find "/usr/lib/modules/$rel/kernel" -name "$m.ko.zst" 2> /dev/null | head -1)
        [ -n "$src" ] || die "no $m module for $rel under /usr/lib/modules"
        dst=$netdir/$m.ko
        [ -f "$dst" ] || zstd -dc "$src" > "$dst" || die "cannot decompress $src"
        net_assets="$net_assets --asset $dst"
    done
fi

# iproute2's tc drives the per-port qdisc inside the guest. Busybox carries a
# tc applet, but it has no u32 filter support, so the real binary is copied
# with its libraries the same way the endpoint is.
if [ "$net" -eq 1 ] && [ -x /usr/bin/tc ]; then
    bins="$bins --bin /usr/bin/tc"
fi

# The small-message helper: one write, one reply, socket still open. Probe P
# needs it because tbt_burst cannot express "37 bytes and no more".
if [ -f "$root/host/tbt_ping.c" ]; then
    # Rebuild when the source is newer: a stale binary is a wrong-size message,
    # not a tunnel failure (it reported "bad usage" once MAX_MSG grew).
    if [ ! -x "$root/host/tbt_ping" ] || [ "$root/host/tbt_ping.c" -nt "$root/host/tbt_ping" ]; then
        "${HOSTCC:-cc}" -std=gnu99 -O2 -Wall -Wextra -Werror \
            -o "$root/host/tbt_ping" "$root/host/tbt_ping.c" ||
            die "cannot build $root/host/tbt_ping"
    fi
    bins="$bins --bin $root/host/tbt_ping"
fi

# Probe Q's connect flood: connections that never complete HELLO.
if [ -f "$root/host/tbt_flood.c" ]; then
    if [ ! -x "$root/host/tbt_flood" ] || [ "$root/host/tbt_flood.c" -nt "$root/host/tbt_flood" ]; then
        "${HOSTCC:-cc}" -std=gnu99 -O2 -Wall -Wextra -Werror \
            -o "$root/host/tbt_flood" "$root/host/tbt_flood.c" ||
            die "cannot build $root/host/tbt_flood"
    fi
    bins="$bins --bin $root/host/tbt_flood"
fi

# Probe T's near endpoint: the 090 deployment is the userspace client in front
# of the kernel far, and only that pair can drive an OPEN the far refuses for
# want of a free flow.
if [ -f "$root/host/tbt_client_main.c" ]; then
    if [ ! -x "$root/host/tbt_client" ] ||
        [ "$root/host/tbt_client_engine.c" -nt "$root/host/tbt_client" ] ||
        [ "$root/host/tbt_client_main.c" -nt "$root/host/tbt_client" ]; then
        make -C "$root" host/tbt_client >&2 ||
            die "cannot build $root/host/tbt_client"
    fi
    bins="$bins --bin $root/host/tbt_client"
fi

# The probe list travels as a file in the image: /proc/cmdline is empty in this
# guest (the kernel accepts tbt_probes= but does not publish it), and a file is
# the same channel on every kernel.
printf '%s\n' "$probes" > "$cache/qemu/probes.txt"
printf '%s\n' "$hostfwd_port" > "$cache/qemu/hostfwd.txt"
# The 256-carrier data volume is a host knob too, and the guest cannot inherit
# the caller's environment: it travels as a file, like the probe list. Empty
# means "leave the guest's own default".
printf '%s\n' "${TBT_N_MIB:-}" > "$cache/qemu/n_mib.txt"

# shellcheck disable=SC2086
"$root/tests/qemu/mkinitramfs.sh" --ko "$ko" --out "$initrd" \
    --asset "$cache/qemu/probes.txt" --asset "$cache/qemu/hostfwd.txt" \
    --asset "$cache/qemu/n_mib.txt" \
    ${init_src:+--init "$init_src"} $bins $assets $net_assets ||
    die "initramfs build failed"

# A probe run must not share a log with another run in flight: tests/qemu and
# tests/probe2 are routinely started by different people at the same time.
console=${log:-$cache/qemu/console.log}
mkdir -p "$(dirname "$console")"

accel=tcg
if [ -w /dev/kvm ]; then
    accel=kvm
fi

# The guest CPU model and vCPU count are tunable for A/B runs.  The default
# pair (max/2) is the historical baseline.  `-cpu host` is KVM-only, so a TCG
# run falls back to max/2 instead of failing to boot.
cpu=${TBT_TEST_CPU:-max}
if [ "$accel" != kvm ] && [ "$cpu" = host ]; then
    cpu=max
    smp=2
fi

# A user-mode NIC gives the guest eth0 with 10.0.2.15/24 and gateway 10.0.2.2.
# hostfwd sends the gateway's carrier port back to the guest's loopback listener,
# so carriers leave through eth0, cross the qdisc, and come back in: per-5-tuple
# shaping is exercised without a tap device or root.
netdev=
if [ "$net" -eq 1 ]; then
    # A previous qemu still holding the forward port makes the new one fail to
    # bind, and the guest then cannot reach the far listener at all.
    if command -v ss > /dev/null 2>&1 && ss -ltn 2> /dev/null | grep -q ":$hostfwd_port "; then
        die "host port $hostfwd_port is already in use; set TBT_HOSTFWD_PORT to a free port"
    fi
    netdev="-netdev user,id=n0,hostfwd=tcp::$hostfwd_port-:$car_port -device virtio-net-pci,netdev=n0"
fi

echo "run.sh: qemu accel=$accel kernel=$(basename "$kernel") timeout=${timeout_s}s probes=$probes net=$net"
set +e
# shellcheck disable=SC2086
timeout --foreground "$timeout_s" qemu-system-x86_64 \
    -accel "$accel" -cpu "$cpu" -smp "$smp" -m 512 \
    -kernel "$kernel" \
    -initrd "$initrd" \
    -append "console=ttyS0 panic=-1 loglevel=7 tsc=unstable tbt_probes=$probes" \
    -nographic -no-reboot -nodefaults \
    $netdev \
    -serial mon:stdio \
    -display none \
    > "$console" 2>&1
rc=$?
set -e

if [ $rc -eq 124 ]; then
    echo "run.sh: TIMEOUT after ${timeout_s}s"
fi
echo "run.sh: qemu exited $rc; console tail ($console):"
tail -n 40 "$console" | sed 's/^/    /'

# The guest reports one line per probe; the runner owns the verdict, so a
# missing marker or a crashed guest cannot be mistaken for a pass.
results=$(grep -a '^TBT_PROBE_RESULT ' "$console" || true)
if [ -z "$results" ]; then
    echo "run.sh: FAIL: no TBT_PROBE_RESULT lines on the console (rc=$rc)"
    exit 1
fi
passes=$(printf '%s\n' "$results" | grep -c ' PASS' || true)
fails=$(printf '%s\n' "$results" | grep -c ' FAIL' || true)
skips=$(printf '%s\n' "$results" | grep -c ' SKIP' || true)

echo "run.sh: probe summary"
printf '%s\n' "$results" | awk '{ printf "  %-3s %-5s", $2, $3; if (NF > 3) printf " %s", substr($0, index($0, $4)); print "" }'

if [ "$fails" -gt 0 ]; then
    echo "run.sh: FAIL: $fails probe(s) failed, $passes passed, $skips skipped"
    exit 1
fi
if [ "$skips" -gt 0 ] && [ "$allow_skip" -eq 0 ]; then
    echo "run.sh: FAIL: $skips probe(s) skipped; pass --allow-skip to accept a partial run"
    exit 1
fi
echo "run.sh: PASS: $passes probe(s) passed, $skips skipped"
exit 0
