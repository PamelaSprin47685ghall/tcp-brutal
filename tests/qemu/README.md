# Running tbtun in QEMU

The module can only be exercised with kernel privileges. On this machine the
operator has no passwordless `sudo`, so `insmod` is unavailable, and a network
namespace cannot substitute for it: `CAP_SYS_MODULE` is needed in the initial
user namespace, which nothing unprivileged grants. Booting a kernel in QEMU is
the one way to load and test `tbtun.ko` without root on the host.

Nothing here installs a package or writes outside `~/.cache/tbtun-build` and the
repository.

## What the run does

`tests/qemu/run.sh` builds an initramfs, boots the kernel with it, and reads the
guest console. `tests/qemu/init` is PID 1 in the guest and runs the selected
probes, all through the operator surface only (`echo` into `/proc/net/tbtun/*`):

| Probe | What it establishes |
| --- | --- |
| A | control surface: `ports`, canonical read-back, rejected lines change nothing, `del`, the `debug_loss` gate |
| B | data path on loopback carriers: 4 MiB each way, byte for byte |
| C | carrier 0 taken down mid-transfer; a replacement is dialled and the stream still completes |
| D | the deterministic loss injector (`drop_ppm`) and reorder accounting |
| E | a warm spare takes over when an active carrier is taken down |
| F | duplication: `dup=2` under injected loss, no duplicate lands on the same path |
| G | hole repair on one lossy path, bounded reorder stall |
| H | sustained loss, recovery, and the bounded reset |
| I | **the thesis**: one shaped carrier (control) versus four on the same path; four must be at least 1.8x one |
| J | 1% loss and 2% reorder on every carrier; byte-exactness |
| K | one carrier's class forced to 100% loss mid-transfer, then restored; byte-exactness |
| L | FEC v6 over the raw container: `n=4 m=2 carriers=6`, 1 MiB byte-exact |
| M | FEC v6 over HTTP: `n=4 m=2 carriers=6`, 1 MiB byte-exact |
| N | FEC v6 grouped over HTTP: `n=4 m=4 carriers=16`, 1 MiB byte-exact, all 16 carriers up |
| O | the T5.2 smoke's shape (`profile=http-kal`, `n=1 m=1`, `carriers=2`, `fec=on`, `mode=grouped`) with `crypto=off`, with `crypto=on`, with carrier 0 taken down mid-transfer, and with a 700 ms loopback delay that makes a dial outlive one `kernel_connect` wait window |

G retries its whole window up to three times when the injector happened to drop
nothing, and skips with `no loss observed in 3 windows` rather than failing for
a correct reason. J and K judge byte-exactness under carrier-TCP loss and need
no placeholder frame: the sender repairs a hole from the replay ring, and the
receiver's bounded reset is the fallback.

The guest prints one `TBT_PROBE_RESULT <probe> PASS|FAIL|SKIP <detail>` line per
probe, then `TBT_TEST_PASS` and `poweroff -f`. `run.sh` judges from the
per-probe lines, not from QEMU's exit status (0 for an orderly poweroff even
after a failed probe), and prints a summary table:

```
run.sh: probe summary
  A   PASS
  B   PASS
  ...
run.sh: PASS: 9 probe(s) passed, 2 skipped
```

A run passes only when no probe failed and none was skipped; `--allow-skip`
accepts a partial run (for a module where C6 is not landed yet).

## The network

The guest gets one user-mode NIC, so carriers can leave through a real qdisc
instead of loopback:

```sh
qemu-system-x86_64 ... \
  -netdev user,id=n0,hostfwd=tcp::19101-127.0.0.1:19101 \
  -device virtio-net-pci,netdev=n0
```

QEMU's user-mode network gives the guest `10.0.2.15/24` with gateway `10.0.2.2`.
`hostfwd` sends the gateway's port 19101 back to the guest's loopback listener,
so the topology is:

```
near tunnel (listen 10.0.2.15:19100)
   carrier: 10.0.2.15:<portblock> -> 10.0.2.2:19101
      eth0 egress, root qdisc shapes by source port
         slirp -> hostfwd -> guest 10.0.2.15:19101
            far tunnel (listen 10.0.2.15:19101) -> target 127.0.0.1:19072
```

The far listener binds the guest's eth0 address, not loopback: slirp forwards to
the guest's default address, and a listener on `127.0.0.1` never sees the
forwarded connection. The host port is `TBT_HOSTFWD_PORT` (default 19101);
`run.sh` writes the value it gives qemu into the image, so the guest dials the
same port even when the default is overridden.

The near side is the only side that binds a portblock source port, so only the
near-to-far direction is shaped; the far side's replies leave with the far
listener's port and fall into the unshaped default class. That is deliberate:
it isolates the per-5-tuple limit under test.

The NIC driver and the qdiscs are modules in this kernel, so the image carries
them, decompressed from the running kernel's module tree:
`failover`, `net_failover`, `virtio_net`, `sch_htb`, `sch_netem`, `cls_u32`.

## Per-5-tuple shaping

Probe I sets this up inside the guest with the host's `tc` (busybox's `tc`
applet has no `u32` filter support):

```sh
tc qdisc add dev eth0 root handle 1: htb default 99
tc class add dev eth0 parent 1:  classid 1:1  htb rate 1000mbit ceil 1000mbit
tc class add dev eth0 parent 1:1 classid 1:99 htb rate 1000mbit ceil 1000mbit
i=0
while [ $i -lt 24 ]; do
    port=$((41000 + i)); cls=$((10 + i))
    tc class add dev eth0 parent 1:1 classid "1:$cls" htb rate 10mbit ceil 10mbit
    tc qdisc add dev eth0 parent "1:$cls" handle "$((100 + i)):" netem delay 20ms 10ms
    tc filter add dev eth0 parent 1: protocol ip prio 1 u32 \
        match ip sport "$port" 0xffff flowid "1:$cls"
    i=$((i + 1))
done
```

One class per portblock port, 10 Mbit each; 24 classes cover three disjoint
eight-port ranges, one per shaped probe (`I_PORTS=41000-41007`,
`J_PORTS=41008-41015`, `K_PORTS=41016-41023`). Disjoint ranges matter because a
carrier socket that closed sits in TIME_WAIT for a minute, and reusing its port
at once fails with `EADDRNOTAVAIL`. The thesis is measured as a pair on
that path, because the sum of the class rates is not what a single tunnel can
deliver: the harness's own echo path shares the qdisc, and the 20 ms jitter
dominates at these rates.

1. **Control, `n=1`**: one shaped carrier, `TBT_I_CONTROL_MIB` MiB (default 4).
   Its one-way rate must stay at or below `TBT_I_CONTROL_MAX` (4 Mbit/s; the
   measured range is 2.7-3.2, so the ceiling leaves room for run-to-run noise).
2. **Main, `n=4`**: four shaped carriers, `TBT_I_MIB` MiB (default 40). Its
   one-way rate must be at least `TBT_I_MBIT_MIN` (6 Mbit/s) and at least
   `TBT_I_RATIO_MIN` (1.8x) the control.

Both tunnel lines carry `rx_reorder_bytes=2097152 hole_budget_ms=5000`, which
is what makes the strict phase survive the shaped path. The rate is the sum of
`peers` `tx_bytes` over a two-second window during the strict phase, i.e. the
one-way goodput the qdisc lets through.

Measured on this box (2026-11-05):

| Run | One-way rate | Strict | Interleaved |
| --- | --- | --- | --- |
| `n=1` control | 2.7-3.2 Mbit/s | pass | pass |
| `n=4` main | 11.7-12.0 Mbit/s | pass | pass |

That is 3.9-4.4x, so the per-5-tuple claim holds on a path where every carrier is
individually capped at 10 Mbit.

`TBT_I_N=1..4` runs a single carrier count instead of the pair, asserting only
that count's absolute bound (4: >= 6 Mbit/s, 1: <= 4 Mbit/s).

`TBT_I_PACE_MBIT` (default 0) additionally paces the client's direction on
loopback with an htb class matching `dport 19100`. The current module does not
need it; a single tbf on `lo` starves the echo return and deadlocks the strict
phase, which is why the pacer uses two classes. `TBT_I_NETEM` sets the netem
arguments for the carrier classes (default `delay 20ms 10ms`; empty gives a
bare htb). `TBT_I_RX_REORDER` and `TBT_I_HOLE_BUDGET` are the tuned keys written
on both tunnel lines.

The interleaved phase is reported separately. If the strict phase passes and the
interleaved one resets, the probe fails with
`probe I: strict PASS, interleaved FAIL` and dumps `peers`, `stats` and the tc
classes, so the counters reach the module owner instead of being averaged away.

Observed once in three tuned runs, at 11.5 Mbit/s one-way:

| Counter | Value |
| --- | --- |
| `holes_declared` / `holes_repaired` | 1294 / 1294 |
| `holes_unrecoverable` | 1 |
| `rx_max_stall_us` | 17126 |
| `reorder_max_depth` | 83 |
| `txq` / `txp` | 0 / 0 (no queue pressure at the end) |
| per-carrier `loss_ppm` | 160k-179k on a link with no configured loss |

One unrecoverable hole ends the stream, and the interleaved phase is where it
happens: the strict phase is byte-exact every time.

## Inputs

| Piece | Where it comes from | Cache path |
| --- | --- | --- |
| kernel image | `linux72` package, `usr/lib/modules/<rel>/vmlinuz` | `~/.cache/tbtun-build/dl/vmlinuz-<rel>` |
| busybox | `busybox` package (static) | `~/.cache/tbtun-build/busybox/usr/bin/busybox` |
| module | `make check` | `./tbtun.ko` |
| checker | built from `host/tbt_burst.c` | `./host/tbt_burst` |
| NIC and qdisc modules | `/usr/lib/modules/<rel>/kernel`, decompressed | `~/.cache/tbtun-build/qemu/net/` |
| `tc` | the host's `/usr/bin/tc`, with its libraries | copied into the image |

The kernel **must** be the one the module was built against: vermagic is
`7.2.9-1-MANJARO` and the module is rejected by any other build. Both the kernel
and the header payload come from the same `linux72` 7.2.9-1 package, so they
match by construction.

Fetch the two guest pieces once, unprivileged:

```sh
cache=~/.cache/tbtun-build
base=https://mirror.sjtu.edu.cn/manjaro/stable

mkdir -p "$cache/dl" "$cache/busybox"
curl -Lo "$cache/dl/linux72.pkg.tar.zst" \
    "$base/core/x86_64/linux72-7.2.9-1-x86_64.pkg.tar.zst"
tar --zstd -xOf "$cache/dl/linux72.pkg.tar.zst" \
    usr/lib/modules/7.2.9-1-MANJARO/vmlinuz > "$cache/dl/vmlinuz-$(uname -r)"

curl -Lo "$cache/dl/busybox.pkg.tar.zst" \
    "$base/extra/x86_64/busybox-1.36.1-4-x86_64.pkg.tar.zst"
tar --zstd -xf "$cache/dl/busybox.pkg.tar.zst" -C "$cache/busybox"
```

Busybox is taken from `PATH` when it is installed; the cached package is the
fallback, not a requirement.

## Running

```sh
make check        # builds tbtun.ko (see docs/build-env.md)
make qemu-test    # builds, then boots and tests in QEMU
```

Individual probes, which is what a failure is debugged with:

```sh
tests/qemu/run.sh --probes A            # control surface only
tests/qemu/run.sh --probes I --timeout 300
tests/qemu/run.sh --probes A,B,I --log /tmp/i.log
```

`--probes` defaults to `A,B,C,D,E,F,G,H,I,J,K`. Probe I runs for about two
minutes (control plus main), so give it a `--timeout` of at least 300. `--allow-skip` accepts a run in
which some probes skipped. `--no-net` boots without the NIC, which turns I, J
and K into skips and leaves A–H on loopback. `--ko`,
`--kernel`, `--init`, `--bin`, `--asset`, `--log` and `--timeout` work as
before.

`-accel kvm` is used when `/dev/kvm` is writable, `tcg` otherwise (roughly an
order of magnitude slower, still correct). Memory is 512 MiB and the console log
is kept at `~/.cache/tbtun-build/qemu/console.log`; `run.sh` prints its tail.

## Reading the result

| Exit | Meaning |
| --- | --- |
| 0 | the guest printed `TBT_TEST_PASS` |
| 1 | the guest printed `TBT_TEST_FAIL`; the reason follows the marker |
| 2 | an input is missing: no kernel image, no module, no QEMU |

The console is the artifact. A kernel oops appears above the marker with a call
trace naming the module's function, and that trace — not the marker alone — is
what to read when a probe fails.

Failure text and its meaning:

- `mount /proc` / `/proc is not a mounted procfs`: the guest itself is broken,
  not the module. The initramfs mounts are the first thing to check.
- `/proc/net/tbtun/config missing after load`: the module loaded but published
  nothing at the documented path. `diagnose_proc` prints the alternatives it
  looked at.
- `ports directive not read back`, `config read-back ...`: the write path or the
  canonical rendering is wrong.
- `a rejected line changed the configuration`: a parse error was partially
  applied, which §8.4.1 forbids.
- `tunnel accepted the connection but carried no data`: the public socket closed
  without a byte, i.e. the data path is a stub or is broken. A real failure.
- `probe I: tc setup failed`: `tc` could not install htb/u32/netem. The image is
  missing a qdisc module or the host binary's libraries; the console shows
  which.
- `probe I: measured N Mbit/s, expected > 30 Mbit/s`: four shaped carriers did
  not reach 3/4 of their aggregate ceiling. Either per-5-tuple shaping is not
  working (the qdisc counters in `tc -s class show dev eth0` show which classes
  carried bytes), or the tunnel cannot fill four 10 Mbit pipes (the `peers`
  dump shows per-carrier bytes and stall state).
- `probe I: carrier N carried M bytes, expected >= 1 MiB`: the tunnel used fewer
  carriers than configured; `peers` shows which slots are `up`.
- `probe J/K` start their own `nc -lk` echo (on `TARGET_PORT3` and
  `TARGET_PORT4`) and tear it down; they no longer depend on probe I's target.
  Probe G retries its window when the injector dropped nothing and skips after
  three clean windows; it does not fail for a correct reason.
- `probe I: transfer finished before the rate window` or a client `ECONNRESET`:
  the tunnel's 64 KB send queue filled because the carriers drain slower than
  the client sends, and C2 kills the stream at the cap (`txq=65506 txp=1` in
  `stats`, `holes_unrecoverable=1` on the far side). This is the module's
  documented C2 boundary, not the qdisc: the tc class counters show the shaped
  carriers carried what they were given.
- `INFO: task kworker/N blocked for more than 122 seconds` with
  `tbt_kill_work -> tbt_sock_detach -> cancel_work_sync`: a module deadlock in
  the stream-kill path. The guest hangs rather than powering off, so the run
  ends at `--timeout`; the stack is the artifact.

## Limits

- Only `127.0.0.1` and the qemu user-mode network are used. There is no tap
  device, because that needs root on the host; user-mode networking is the
  unprivileged substitute and it is enough to put carriers behind a qdisc.
- The shaping is egress-only and per source port. An ingress policer would need
  `ifb`, which this kernel does not provide as a module here.
- No netns tests here. `netns_optin` visibility needs several namespaces with
  real interfaces, which belongs in a later test, not in this harness.
- The guest runs a Manjaro kernel without secure boot; module signature
  enforcement is off (`CONFIG_MODULE_SIG_FORCE` unset), so an unsigned build
  loads and taints the kernel.
