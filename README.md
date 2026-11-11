# tbtun

A Linux kernel TCP tunnel that carries TCP streams over `N + M` independent
5-tuples, so a per-flow ISP limit applies `N + M` times instead of once, and
refills a stalled carrier's holes from retained bytes instead of killing the
stream.

## How it works

The near host listens on a port. Clients connect to it; the kernel accepts each
connection and never hands it to a userspace process. Each connection becomes a
stream, and concurrent streams share the same carriers. A stream's payload is
cut into frames, and those frames travel to the far host over `N` active carrier
TCP connections plus `M` spares, each with its own source port, so each gets its
own shaper bucket at the ISP. The far host reassembles the byte stream and
connects to `target=`, and the reply travels back the same way. Everything —
accept, connect, framing, ordering, delivery — happens in the kernel. There is
no character device, no proxy process, no helper program: the module is
configured by writing text to `/proc/net/tbtun/config`.

Two independent gains are in play. Per-5-tuple accounting is the certain one:
`N + M` flows, `N + M` budgets. The second is a bet: the `http-kal` profile
wraps carriers so they parse as ordinary HTTP/1.1 keep-alive traffic, on the
theory that the ISP's classifier gives HTTP priority. That theory is measured,
not assumed; see [QoS caveat](#qos-caveat). If it turns out false, `profile=raw`
turns the wrapping off and the per-flow gain remains.

FEC v6 (`fec=on`) is an optional data plane: the application byte stream is cut
into 8 KiB DATA shares plus GF(2) band-RLNC parity shares, one share per
carrier, and the receiver recovers missing shares from parity. A missing
share in the last
`K` super-blocks is requested with a NACK and replayed by the sender (`K` is
the largest that fits the 15 MiB replay ring, up to 64); an
older loss stays in the carrier's TCP send queue and TCP retransmits it. It
rides both `profile=raw` and the HTTP chunk container; in the HTTP profile one
chunk body is exactly one 48-byte FEC record (`TBT_FEC_CONV=0x100c`),
while the request line, headers and chunk framing stay
plaintext. `mode=grouped` splits the carriers into `groups` groups of `n` DATA
plus `m` parity shares, `carriers = groups * (n + m)`, up to 256; the production
shape is 32 groups of `n=4, m=4`. `crypto=on` (the default whenever `fec=on`)
ChaCha8-obfuscates the DATA/PARITY payload only; the 48-byte FEC header, the
HTTP headers and every control frame stay clear. The old ARQ path is unchanged
when `fec=off`.

The FEC code is selected by `mode`. `mode=sc-ldpc` is implemented: the
downlink is the binary SC-LDPC block (one group of 256 shares, `L=8`, rate
0.5); the uplink is the grouped GF(16) SC-LDPC code (8 groups of 32 shares,
`L=16`, rate 1/4). `w` is the codeword block (256 downlink, 128 GF(16)
uplink, 512 reserved) and the rate follows `n/(n+m)`. The per-direction
`FEC_CFG` INIT carries `L`, block and rate, and the receiver uses the peer's
shape. `mode=rlnc`/`grouped` remain as the GF(2) band RLNC fallback.

Adaptive FEC (`adaptive=`) defaults on when `fec=on`. The GF(2)
`mode=rlnc`/`grouped` family keeps Phase 1: the shape stays in the
`n + m = 8`, 32-group family and `m` moves in 1..6. `mode=sc-ldpc` uses the
per-flow SC-LDPC estimator/controller and moves the split along the shipped
rate ladder (downlink `n+m=256` with `n` in 64/76/102/128/151/178; uplink
`n+m=32` with `n` in 8/16/20/24). The switch rides `FEC_CFG`/`FEC_FBK`
(wire `0x100c`). A `mode=sc-ldpc` line whose split is not on that ladder
stays `adaptive=off`, so a fixed test shape keeps its configured `n/m`.
`adaptive=off` always wins; `session <id> adaptive=off` disables it at
runtime. Shapes with `n + m != 8` are not implemented for the GF(2) family.

One session carries one FEC flow (wire `0x100c`). There is no subframe mux:
a DATA or PARITY payload is plain application bytes. An 8 KiB write becomes one full DATA share as the
write completes (cut-through); `TBT_FEC_SHORT_MS` (2) covers only the
interactive tail. A 1x256 block is 128 DATA shares followed by its parity
shares. The 36-byte OPEN with `route_slot`/`target_port`, the 64-byte
`FEC_CFG`/`FEC_FBK` and the GF(16)
bit-plane64 mapping stay. The stream id still rides in the FEC header
(`0x1005`), so a queued old-stream record cannot reach a new stream.

The tree implements the FEC v6 grouped data plane, adaptive Phase 1 and the
256-carrier extension: the `/proc` surface, the
listener and the far side, spares and carrier replacement with the RETIRE
handshake, duplication with the replay ring, hole repair from the retained
bytes, and the `http-kal`/`http-kal-drip` containers. A gap in a carrier's
stream is filled from the replay ring instead of killing the stream.
`holes_unrecoverable` in `stats` counts holes that outlived `hole_budget_ms` or
overran `rx_reorder_bytes`; `gaps_total` counts reorder overflows. Either one
resets the stream.

Backpressure is not loss. The stall threshold is 5 × the carrier's RTO
(`SRTT + 4×RTTVAR`), clamped to 1..60 s; `carrier_stall_ms=` is a cap that can
only lower it. Under FEC a stall pauses the carrier: new shares avoid it, the
shares already in flight are treated as lost and rebuilt from parity, and the
carrier is replaced only after `max(4×stall, 30 s)`, capped at 300 s, with no wire progress
(`stall_dead`). Under the old ARQ a stalled carrier is still killed and its
retained frames are repathed immediately. An idle carrier, a delivery pause and
a closed peer window restart the clock (`stall_idle_skips`,
`stall_paused_skips`). Every carrier socket also enables TCP keepalive (30 s
idle, 10 s between probes, 3 probes), so a half-open path closes at the socket
before the `stall_dead` bound. DATA admission stops at the peer's advertised window, so
at most one window sits in the carrier's TCP send buffer
(`win_block`/`win_resume`); control frames bypass that gate. The window check
reads `snd_wnd`, so it is a heuristic, and an unknown window admits one chunk.
The tunnel's own sender window auto-sizes from the carriers' BBR rate samples
and their aggregate advertised window; the bootstrap floor and the auto cap
both scale with the carrier count, and `tx_window_bytes=` pins it without
being truncated. Pinned control frames are retried and requeued instead of being
dropped at the first `-EAGAIN`, and the pump wakes on `ctl_queued` even when no
payload bytes are queued.

## Requirements

- Linux 6.1 or later, with the matching kernel headers for building.
- `pahole` (dwarves) if the kernel sets `CONFIG_DEBUG_INFO_BTF_MODULES=y`;
  the module's BTF step fails without it.
- Root to load the module and to write `/proc/net/tbtun/*` (the files are `0600`;
  the debug-only `http` is `0400`).

## Layout

| Path | Contents |
| --- | --- |
| `src/kernel/` | kernel module sources and `Kbuild` |
| `src/client/` | userspace client |
| `tests/unit/` | host conformance tests |
| `tests/bench/` | codec benchmarks and calibration |
| `tests/tools/` | guest-side helpers (`tbt_burst`, `tbt_ping`, `tbt_flood`) |
| `tests/qemu/` | QEMU boot harness |
| `tests/throughput/` | throughput harness and recorded runs |
| `tests/adversarial/` | attack and crash suite |
| `tests/http/` | RFC 9112 generator and audit |
| `tests/calib/` | codec calibration data |
| `tests/manual/` | manual acceptance scripts |
| `tools/` | build, DKMS, lint and QA scripts |
| `deploy/` | packaging for the two-host deployment |
| `docs/` | design and build notes |
| `build/` | out-of-source build output (generated) |

Every target writes under `build/`: `build/kernel/tbtun.ko`, `build/bin/`,
`build/obj/` and `build/lib/`. The source tree stays clean.

## Build

```sh
make check
```

`check` builds `build/kernel/tbtun.ko` and then runs the host tests — grammar, fuzz, the
HTTP container, and the reorder/ack/control/multistream/anchor models — which
need no root, no module and no kernel headers.
`make host-test` runs only those tests.


If the distribution's headers are not installed, the build can use an extracted
header payload instead of `/lib/modules/$(uname -r)/build`; see
[docs/build-env.md](docs/build-env.md) for the one-time setup, the `env.sh` it
produces, and the pahole gotchas. `make check` sources that environment itself
when it is present.

The multi-kernel compatibility check (5.15, 6.6, 6.12 and 7.0 on x86_64 and
aarch64, including a QEMU load check) is `tools/kernel-matrix.sh`; see
[docs/kernel-matrix.md](docs/kernel-matrix.md) for the cells, the current
results and their cost.

## Packaging

The module version has one authority: `TBT_VERSION_MAJOR/MINOR/PATCH` in
`src/kernel/tbt_uapi.h` (currently `0.10.0`). The wire
version is `TBT_WIRE_VERSION=1`, with `TBT_CONV=0x000d` for the 32-byte frames
and `TBT_FEC_CONV=0x100c` for the 48-byte FEC header and the 64-byte
`FEC_CFG`/`FEC_FBK` records; all three are independent of
the module version. A peer whose conv does not match is refused at the first
header, and a FEC record with the wrong `TBT_FEC_CONV` closes its carrier, so
both ends must run the same build.

The 36-byte OPEN carries: the 32-byte FEC group bitmap, the
little-endian route slot (the near side's listen mapping), and the
little-endian target port for that slot. Zero keeps the far side's configured
port; any other value must equal it. `TBT_WIRE_VERSION_V6` is 9:
version 9 grows the HELLO body to 90 bytes with a `boot_epoch` and a 128-bit
SipHash tag over the session-naming bytes. A configured `crypto_key=` (the
256-bit PSK) proves the tag, so a restarted client may present a fresh crypto
nonce and a client whose address changed may re-anchor; without the key the
tag is absent and the HELLO stays unauthenticated.

The current FEC wire is `0x100c`. Its record layout, offsets and record types
are the contract in `src/kernel/tbt_uapi.h`; a peer with any other conv is
refused at the first FEC record.
`tools/mkdkmsconf.sh` derives
`PACKAGE_VERSION="<major>.<minor>.<patch>"`; `dkms.conf` is generated by
`make dkms.conf` / `make dkms-tarball`, so do not edit it by hand.
`tools/install_dkms.sh` reads the same value with
`mkdkmsconf.sh --version`. For development builds, `TBT_DKMS_GIT_SUFFIX=1`
adds `.r<commits>.<sha>`; the default is the pure semantic version.

## Load

```sh
sudo insmod tbtun.ko debug_loss=0
```

Module parameters, read-only after load:

| Parameter | Default | Meaning |
|---|---|---|
| `debug` | `0` | Verbosity, `0..3` |
| `max_tunnels` | `64` | Maximum configured tunnels; a new id over the cap fails with `ENOSPC` |
| `max_carriers` | `256` | Carrier sockets per session, `1..256`; `carriers` over the cap fails with `EINVAL` |
| `debug_loss` | `0` | Allow `drop_ppm` writes, the deterministic loss injector |
| `carrier_cc` | `bbr` | Congestion control every carrier asks for by name with `setsockopt(TCP_CONGESTION)`. `bbr` keeps the measured default; `tbtcc` selects this module's loss-tolerant controller for an A/B run. It is never written to `net.ipv4.tcp_congestion_control` |
| `hold_enable` | `1` | BBR hold: after 200 ms of carrier idleness the 50 ms poll snapshots cwnd/pacing, bounded by `snd_wnd/mss`, `snd_cwnd_clamp`, `sk_sndbuf/mss` and `sk_max_pacing_rate`, and restores both while the session stays idle and TCP has nothing in flight. The first non-PAD write ends the hold and installs both values before it queues the record. On by default; write `0` to opt out. It writes `tcp_sock` fields directly, which is not a stable kernel API |

The control surface is created in `init_net` only; there is no per-netns view.
Unload with `sudo rmmod tbtun`. Unloading flushes every tunnel and closes every
listener and carrier.

### Production deployment

Both ends must run the same fixed build. A stale build can corrupt kernel memory. The per-carrier `parser` and
`http_rx` must be zeroed: a reused slab object otherwise leaves
`parser->have` dirty, and `tbt_http_rx_parse()` uses that value as a
`memmove()` length. The symptom is an `Internal error: Oops` within seconds
of load, often in an unrelated process, because the damage is slab-wide. A
mixed pair does not degrade gracefully: the unfixed end corrupts memory on its
own, whatever the other end does.

`/sys/module/tbtun/srcversion` is the identity gate, because nothing on the
wire tells the two builds apart. Compare the loaded module with the artifact
you meant to load:

```sh
modinfo -F srcversion tbtun.ko      # before load
cat /sys/module/tbtun/srcversion    # after load
```

`srcversion` is a per-build hash, so the near and far values are expected to
differ when the hosts differ in architecture or kernel config (the field pair
was `72168C1C…` on aarch64 and `332759AF…` on x86_64, from the same source).
Each host must match its own artifact; check both, and recheck after every
redeploy.

`debug=0` is the production setting, and it is also why a crash leaves so
little behind: the per-packet and per-poll diagnostics are gated by `debug`,
and at most the one-shot `tbtun-diag:` bring-up lines remain (session and
carrier creation, the first poll rounds, the first receive window). Arrange
visibility before you need it:

- Persist journald (`Storage=persistent`, or at least a `/var/log/journal`
  directory), so the last boot survives the reboot.
- `sysctl -w kernel.panic_on_oops=1 kernel.panic=10`: an Oops from corrupted
  memory becomes a ten-second reboot, and the reboot is the signal that
  something broke. Left alone, the host keeps running on a corrupted slab and
  the evidence scatters across unrelated processes.
- Keep ramoops registered and `/sys/fs/pstore` empty before each load. It is
  the only channel that survives a panic.
- netconsole is best effort on `rk_gmac-dwmac`, not a primary channel:
  `netconsole` is a module, so it covers nothing before userspace loads it,
  and it detaches itself (`network logging stopped on interface eth0 as it
  unregistered`) whenever `eth0` re-registers. Forward `/dev/kmsg` from a
  userspace relay instead, and keep netconsole as a second copy.

One controlled check, run once, never in a loop:

```sh
# against a tunnel already configured to listen on 127.0.0.1:9001
timeout 10 ./tests/tools/tbt_burst 127.0.0.1 9001 1 5 interleaved   # 1 MiB over 5 s
cat /proc/net/tbtun/stats | head -3
dmesg | tail -20
```

## Two-host example

Near host `10.0.0.1`, far host `203.0.113.9`, service on the far host listening
on `127.0.0.1:5201`.

On the near host:

```sh
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=1' \
     > /proc/net/tbtun/config
```

On the far host:

```sh
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=1 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

Then, on either host:

```sh
cat /proc/net/tbtun/tunnels
```

The near side now listens on `10.0.0.1:9000`. A client connecting there is
carried to `127.0.0.1:5201` on the far host over four carriers. Test it with
`iperf3 -c 10.0.0.1 -p 9000 -t 60 -P 1` against an `iperf3 -s` bound to the
target port.

For FEC v6 over the HTTP container, one group with six carriers (four data,
two parity):

```sh
# near
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=2 fec=on carriers=6' \
     > /proc/net/tbtun/config
# far
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=2 fec=on carriers=6 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

A grouped session uses one share per carrier: `carriers = groups * (n + m)`,
with `groups = carriers / (n + m)`. The production shape is 32 groups of
`n=4, m=4` on 256 carriers:

```sh
# near
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=4 fec=on mode=grouped carriers=256' \
     > /proc/net/tbtun/config
# far
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=4 fec=on mode=grouped carriers=256 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

The global packet sequence is striped over the groups (`group = seq % groups`).
Each group has its own block window and parity, and the receiver delivers a
global span only when every group's block in that span has decoded. Carrier
`c` always carries group `c / (n + m)` share `c % (n + m)`; a replacement
carrier keeps the same index.

The default profile is `http-kal`, so each 48-byte FEC record is one HTTP chunk
body while the request line, headers and chunk framing stay plaintext.
`profile=raw` carries the same records without the HTTP wrapper.

Both ends must agree on the tunnel id, `n`, `m`, `w`, `mode`, `carriers`, and
the profile family. The far
side's `listen` is where carriers arrive; the near side's `peer` is where they
are sent. The far side's `peer` is the anchor: only HELLOs from `10.0.0.1` may
join, and the port in a far `peer=` is grammar filler. Add a shared
`nonce=<32 hex digits>` to both lines to pin the session nonce. Without
`nonce=`, the far side adopts the nonce from the first HELLO; if `peer=` is also
absent, that first HELLO fixes the anchor and the module logs one
trust-on-first-use warning. Addresses are literals — IPv4 `a.b.c.d:port` or
IPv6 `[addr]:port`; there is no DNS.

## What you can observe

Everything the module knows is under `/proc/net/tbtun`. The files are root-only:
`0600`, except the debug-only `http`, which is `0400`; there is no helper
program, and `echo`/`cat` are the whole interface.

| File | Read it for |
| --- | --- |
| `config` | the canonical configuration; write tunnel lines, `del`, `flush`, `carrier ...`, and `session ...` actions here |
| `tunnels` | one line per tunnel: role, state, listen/peer/target, n, m, streams |
| `peers` | one line per carrier slot, 19 columns (below) |
| `ports` | the global fallback source port block and whether it is configured |
| `stats` | `debug_loss=<0\|1>`, gap and stream totals, then one counter block per tunnel (below) |
| `http` | debug only (`debug>=2`): the last **emitted** HTTP request header block from the near side's carrier 0 (headers only), for checking the container against a parser; not a received request, a response, or a live stream |

`drop_ppm` is refused with `EPERM` unless the module was loaded with
`debug_loss=1`; `stats` starts with that gate's state so a script can check it
before trying.

### `config` (read/write)

One directive per line. A directive is applied only when its line is complete: a
newline, or `close()` for a final unterminated line. A bad line returns `EINVAL`
from `write()`, so `echo` fails visibly, and nothing from that line is applied.
A line longer than 1023 bytes is refused with `E2BIG`. A whole-line `#` comment is
ignored; a `#` after a token is not a comment and the line is refused.

`tunnel <id>` takes these keys:

| Key | Default | Meaning |
|---|---|---|
| `role` | — | `near` or `far`; required |
| `listen` | — | `a.b.c.d:port` or `[v6]:port`; near: public address (`[::]` binds the IPv6 wildcard; IPv4-mapped clients follow the host's `bindv6only` setting); far: carrier address |
| `peer` | — | `a.b.c.d:port` or `[v6]:port`; near: where carriers are sent; far: the only near source address allowed to join (the anchor; the port is grammar filler) |
| `target` | — | `a.b.c.d:port` or `[v6]:port`; far: required literal destination; slot 0's destination and the default for OPENs that name port 0. Near: optional slot-0 target port; the address is grammar filler (the far side always dials its own `target_addr`) |
| `map` | — | `listen_port:target_port[,listen_port:target_port...]`, up to 7 extra slots. Ports only; every mapped listener binds on `listen=`'s address. Near: bind each listen port and send its target in OPEN. Far: accept those target ports as destinations (the listen half is ignored). Slot 0 is the `listen=`/`target=` pair. A near line refuses duplicate listen ports or one equal to `listen=` |
| `n` | `4` | DATA shares per group, `1..255`; `n + m <= 256` |
| `m` | `1` | Parity shares per group, `0..255`; `fec=on` needs `m >= 1`; `n + m <= 256` |
| `dup` | `1` | Duplication degree, `0..3` |
| `keepalive` | `5000` | Idle PAD interval, ms. The carrier's TCP keepalive is fixed at 30 s / 10 s / 3 probes and is not this key |
| `drip_ms` | `1000` | `http-kal-drip` idle byte interval, ms |
| `replay` | `8388608` | Replay ring bytes |
| `tunnel_rate` | `0` | Bytes/s ceiling, `0` = unset |
| `max_inflate` | `2.000` | Duplication inflation ceiling |
| `hole_budget_ms` | `0` | How long a hole may stay open, `0..60000` ms; `0` = auto from the measured RTT (floor 2 s) |
| `rx_reorder_bytes` | `67108864` | Receiver reorder budget, bytes, `1..268435456`; bounds each stream's reorder tree and the session-wide sum; `0` is refused |
| `tx_window_bytes` | `67108864` | Sender window bytes; `0` = auto from the carriers' BBR samples and aggregate advertised window (the cap scales with the carrier count); an explicit value must not exceed `rx_reorder_bytes` and is never truncated |
| `tx_retain_bytes` | `0` | Sender retention bytes, `0` = unlimited |
| `carrier_stall_ms` | `0` | Cap on the stall threshold, `0..3600000` ms; `0` = auto: 5 × RTO (`SRTT + 4×RTTVAR`), clamped to 1..60 s. An explicit value only lowers the auto threshold |
| `profile` | `http-kal` | `http-kal`, `http-kal-drip`, or `raw` |
| `fec` | `off` | `off` (old ARQ) or `on` (FEC v6 data plane) |
| `adaptive` | `on` when `fec=on` | `on` lets the control law move `n/m`; `on` requires `fec=on`; with `mode=sc-ldpc` the default is `on` only for a split on the shipped ladder, otherwise `off`; `adaptive=off` always wins |
| `mode` | `rlnc` | `rlnc` (one group), `grouped` (GF(2) band RLNC), or `sc-ldpc` (downlink 1×256 binary, uplink grouped GF(16); `w` is the codeword block) |
| `w` | `0` | RLNC window, `0..1024`; `0` resolves to `2*n` |
| `carriers` | `0` | Carrier slots, `0..256`; `0` resolves to `n + m`; must be a multiple of `n + m` |
| `crypto` | follows `fec` | `on` ChaCha8-obfuscates the FEC DATA/PARITY payload; `on` requires `fec=on`; unset means the same as `fec` |
| `crypto_key` | — | 64 hex digits: pre-shared 256-bit key; `-` clears it. Unset: the near side generates the key and sends it in HELLO (`key_id=1`) |
| `portblock` | `0-0` | Near only: source-port window for this tunnel's carriers, `1..65535`; `0-0` = draw from the global block set through `/ports`. Must hold at least `n + m` ports (`carriers` with FEC). A far tunnel records it but never dials |
| `dev` | — | Near only: bind each dialling carrier socket to this interface (`SO_BINDTODEVICE`) before connect, `1..15` bytes. A bind failure is a dial failure: `connect_fail` climbs and the carrier retries after 200 ms. Forces a direct path around a multi-homed or proxy-TUN route |
| `srcaddrs` | — | Comma-separated local source addresses, IPv4 or IPv6 literals; only the first is used; a near `srcaddrs=` must match `peer=`'s address family |
| `nonce` | — | `16` or `32` hex digits: pin the session nonce; near uses it locally, far requires it from the near side |

Keys that were parsed but never acted on (`framing=`, `chunk=`, `max_retry=`,
`uid=`) were removed; an unknown key is refused with `EINVAL`.

`nonce=` pins the 128-bit session nonce. 32 hex digits are the full value;
16 hex digits pin the low 64 bits and render left-padded with zeros. On a
`role=near` tunnel it is the nonce this side puts in HELLO. On a `role=far`
tunnel it is the nonce the far side requires from the near side: every HELLO
must match it. Without `nonce=`, a far tunnel adopts the nonce from the first
HELLO; if `peer=` is also absent, that first HELLO fixes the anchor too, and the
module logs one trust-on-first-use warning. The source address of the first
HELLO that passes the nonce check becomes the session anchor: every later
unauthenticated HELLO from another address is refused before the nonce is
examined (`join_src_reject`). A HELLO that proves the configured `crypto_key=`
may move the anchor, and a far `peer=` fixes the anchor at creation instead.
The join counters separate the reasons: `join_bad` is a nonce mismatch,
`join_nonce_reject` is a first HELLO from a source other than the configured far
`peer=` when no nonce is pinned, and `join_dup` is an occupied slot. The nonce
travels in clear text in HELLO: it is a session identifier and an anti-confusion
token, not a cryptographic secret. Use WireGuard when the path is hostile. A
64-hex-digit (256-bit) value is refused: the HELLO nonce field is 128 bits.

Other directives: `del <id>`, `flush`, `carrier <index> down|up|drop_ppm <n>`,
and `session <id> nonce=<hex>` (pins the session nonce for reproducible tests).
`drop_ppm` requires `debug_loss=1` and is refused with `EPERM` otherwise.

Reading `config` returns the canonical form: every scalar key, defaults
included, in a fixed order. The text round-trips byte for byte, so
`cat /proc/net/tbtun/config > /etc/tbtun.conf` and
`cat /etc/tbtun.conf > /proc/net/tbtun/config` restore exactly what was saved.
Reads are served by offset from one snapshot, so the whole configuration is
returned even when it is large; the snapshot is rebuilt after each write.

### `tunnels` (read)

`# id role state listen peer target n m streams`, then one line per tunnel.

### `peers` (read/write)

One line per carrier slot, nineteen columns:

| Column | Meaning |
| --- | --- |
| `id` | tunnel id |
| `carrier` | carrier index within the session |
| `sport` | local source port, from the port block |
| `peer` | the far side's address:port |
| `uack` | highest byte offset the peer has acknowledged |
| `rtt_us` | smoothed round-trip time, microseconds |
| `inflight` | bytes written but not yet acknowledged |
| `state` | `connecting`, `up`, `spare`, `stalled`, or `down` |
| `class` | the container this carrier runs (`raw`, `http-kal`, `http-kal-drip`) |
| `recv_cnt` | records received on this carrier |
| `rx_lag_ppm` | records the receiver has not yet accounted for below the sender's write high-water, in parts per million; in-flight records are included, so this is not packet loss |
| `minrtt_us` | minimum RTT seen, microseconds |
| `tx_frames` | records written on this carrier |
| `tx_bytes` | carrier bytes written, headers included |
| `dropped` | records dropped by the `drop_ppm` injector (debug only) |
| `retx_ppm` | retransmitted DATA copies per million unique DATA records; this is the loss proxy |
| `delivery_rate_bps` | the carrier's BBR rate sample, bytes/s (the auto window's input) |
| `cwnd_bytes` | the carrier TCP's `snd_cwnd` in bytes (a cross-check, not an input) |
| `snd_wnd` | the peer's advertised window, bytes |

Writing `carrier <index> down|up|drop_ppm <n>` acts on every running tunnel
that has that carrier index.

### `stats` (read)

The first three lines are global: `debug_loss=<0|1>` (the `drop_ppm` gate),
`gaps_total` (reorder-overflow resets), and `streams_total`. Then one block per
tunnel. A counter with more than one writer context is an `atomic64_t` read
through one accessor, so each value is a single load; a single-writer counter is
a plain `u64` named for its writer. The block is grouped by the design step that
owns it: the first lines are capture and RX work, then a `dbg` line and a `live`
snapshot, `txwake`, `c3` (carriers, joins, retransmission, reorder, gates),
`ctl` (pinned control frames), `streams` (the stream census), `c6` (holes),
`c8` (HTTP), `c9` (the FEC receive verdict), `c10` (FEC loss reasons),
`c11` (NACK/replay repair), `c5` (duplication), `cc` (the carrier congestion
control), and
`reconcile`.

| Key | Meaning |
| --- | --- |
| `submit_bytes`, `wire` | application bytes submitted / carrier bytes written, headers included |
| `data_tx`, `retx_frames`, `retx_bytes` | DATA records written (copies included) / copies / copy payload |
| `retx_hole`, `retx_slow`, `retx_pathloss`, `retx_other` | copies by source: hole repair (a record at the ack stalled at least 2×RTO), slow-path grace (TCP delivered it, but the ack has not covered it after 3×RTO), lost path, other |
| `retx_data_ppm`, `dup_bytes_ppm` | copies per million unique DATA records / copy payload per million application bytes |
| `port_exhaust`, `port_alloc_fail` | the tunnel's source-port window exhausted / port bitmap allocation failed; the carrier retries, nothing is killed |
| `join_src_reject`, `join_nonce_reject`, `join_bad`, `join_dup` | HELLO refused by the anchor, by the first-HELLO source, by the nonce, or by an occupied slot |
| `ctl_lost`, `ctl_retry`, `ctl_requeue`, `ctl_obsolete`, `ctl_queued` | pinned control frames: explicit loss after the retry bound, retries, requeues, obsolete generation, live depth |
| `stall_paused_skips`, `win_block`, `win_resume` | stall decision deferred while paused / DATA held at the peer-window gate / released |
| `stall_idle_skips`, `stall_declared`, `stall_recovered`, `stall_drop`, `stall_dead` | stall clock restarted because nothing was unacked / a FEC carrier was paused / its ack moved again / frames dropped by the ARQ pause helper (no caller yet, stays 0) / a paused carrier outlived `max(4×stall, 30 s)`, capped at 300 s, and was replaced |
| `tx_window`, `rtt_mean_us`, `rttvar_us`, `rtt_max_us`, `txq_max` | effective sender window and its inputs / frame-queue bound |
| `holes_declared`, `holes_repaired`, `holes_unrecoverable`, `rx_max_stall_us` | hole accounting |
| `dup_tx`, `dup_used`, `dup_suppressed`, `inflate_ppm` | duplication |
| `cc` line (`cc_carriers`, `cc_max_bw_bps`, `cc_pacing_bps`, `cc_clamp_hits`, `cc_hold_enter`, `cc_hold_exit`, `p_ppm`) | Carriers currently running `tbtcc`; summed windowed-max delivery rate and current pacing; `ssthresh` clamp activations; idle-hold enter/exit edges; the loss the FEC feedback path reported for this sending direction |
| `c9` line (`fec_proto_fail`, `fec_hdr_crc_fail`, `fec_payload_crc_fail`, `fec_short_tx`, `fec_short_unknown`, `fec_drop_window`, `fec_drop_crypto`) | FEC receive verdict: header/CRC failures close the carrier, a payload CRC drops only that share; `fec_drop_crypto` counts shares that arrived before the key |
| `c10` line (`fec_decode_blocks`, `fec_decode_recovered`, `fec_decode_nomem`, `fec_deliver_nomem`, `fec_deliver_dup`, `fec_deliver_ooo`, `fec_deliver_bad`, `fec_dup_share`, `fec_late_share`, `fec_late_data`, `fec_pad_share`, `fec_epoch_conflict`, `fec_slot_past_cut`, `fec_fin_bad`, `fec_ack_bad`, `fec_tx_share_gap`, `fec_empty_block`) | every way a FEC share or block can be lost: decode/delivery failures, duplicate/late/padded shares, epoch conflicts, past-cut slots, bad FIN/ACK, sender share gaps, empty blocks. A stall with all of them zero means the loss is outside these paths |
| `c11` line (`fec_nack_tx`, `fec_nack_rx`, `fec_nack_retx`, `fec_nack_drop`, `fec_nack_bad`, `fec_replay_hit`, `fec_replay_miss`, `fec_nack_retx_bytes`, `fec_replace_retx_bytes`, `fec_nack_lat_ns`, `fec_nack_lat_max_ns`, `fec_nack_repair_ns`, `fec_nack_repair_max_ns`, `fec_reset_tcp_failed`, `fec_reset_user`, `fec_wait_carrier`, `fec_backpressure`) | NACK/replay repair: NACKs sent/received, shares replayed, replay hits/misses, replay bytes, repair latency, and reset causes. `fec_reset_tcp_failed` is the only FEC-owned reset and requires every carrier TCP to be gone; `fec_wait_carrier`/`fec_backpressure` show a producer waiting for a usable carrier or the source ring |
| `reconcile live_*`/`retired_*` | live + retired equals the session total, for bytes, DATA and retransmits |

The `reconcile` line is the check that no carrier's bytes vanished with a
replacement: `wire == live_bytes + retired_bytes`, `data_tx == live_data +
retired_data`, and `retx_frames == live_retx + retired_retx`.

### `ports` (read/write)

`ports <lo>-<hi>` sets the global source port block for carriers; the default is
`40000-44095` (4096 ports = 256 carriers × 16 rebuilds). A blank line, or
`ports 0-0`, is a no-op and leaves the block unchanged. Reading the file prints
the block, whether it is configured, and the directive.

The module keeps one bitmap over the whole 16-bit source-port space. A
tunnel's `portblock=` is a window over that bitmap; when it is `0-0`, the
tunnel draws from this global block instead. Windows may overlap: they share
capacity, and the allocator never hands the same live source port to two
carriers. A window is therefore a search range, not a hard reservation; use
disjoint windows for isolation. A near block must hold at least one port per
carrier slot (`n + m`, or `carriers` with FEC), or the tunnel line is refused.
Changing `/ports` affects new allocations immediately; carriers already up
keep their ports until they close. Changing a tunnel's `portblock=` means
re-sending its tunnel line, which replaces the tunnel and its session. Reserve
every block in use in `net.ipv4.ip_local_reserved_ports`, not only the global
one.

## Known limitations

- **No DNS.** Every address is a literal: `listen=`, `peer=`, and `target=`
  accept `a.b.c.d:port` or `[ipv6]:port`; `srcaddrs=` accepts `a.b.c.d` or an
  IPv6 literal. There is no name resolution. `listen=[::]:port` binds the IPv6
  wildcard; dual-stack acceptance is the host's `bindv6only` policy. `map=`
  maps ports only, not addresses.

  An IPv6 pair uses the same grammar; the near host is `2001:db8::2`, the far
  host `2001:db8::1`, and the service listens on `[::1]:5201`:

  ```sh
  # near
  echo 'tunnel 1 role=near listen=[::]:9000 peer=[2001:db8::1]:9000 n=4 m=1' \
       > /proc/net/tbtun/config
  # far
  echo 'tunnel 1 role=far listen=[2001:db8::1]:9000 target=[::1]:5201 n=4 m=1 peer=[2001:db8::2]:9000' \
       > /proc/net/tbtun/config
  ```

  `srcaddrs=` must use the same family as `peer=` on a near tunnel.
- **No encryption and no authentication by default; `crypto=on` is
  obfuscation.** `crypto=on` (the default whenever `fec=on`) ChaCha8-obfuscates
  the FEC DATA/PARITY payload only. The 48-byte FEC header, the HTTP headers and
  every control frame stay in clear text, and a HELLO is authenticated only
  when `crypto_key=` is configured. With `crypto_key=` unset, the near side
  generates a 32-byte session key and sends it in the HELLO body (`key_id=1`);
  the far side adopts it. With `crypto_key=` set on both ends, `key_id=0` and
  the HELLO key field is zero; the v9 tag authenticates the HELLO and lets a
  restarted client or a moved source address rejoin, while a mismatch surfaces
  as a payload CRC failure on every share. The nonce is a session identifier,
  not a secret. Run it inside WireGuard on an untrusted path; the `raw` profile
  is then the only sensible choice, because an HTTP parser cannot see inside
  TLS.
- **FEC is optional; grouped GF(2) is the production shape.** `fec=on` supports one
  group (`carriers == n + m`) and grouped sessions
  (`carriers = groups * (n + m)`, up to 256 carriers; 32 groups of `n=4, m=4`
  in production) in both `raw` and
  `http-kal` containers. The sender emits one block of `n` DATA shares plus
  `m` parity shares per group; the receiver decodes a block when its parity
  makes the missing shares solvable, then delivers whole super-blocks in global
  sequence order. Repair is bounded: the receiver NACKs (record type `0x33`) the oldest incomplete
  super-block within the last `K` (`K` is the largest that fits the 15 MiB
  ring, up to 64), and the sender replays it from that ring on any up,
  non-stalled carrier. Inside the window the repair is
  RTT-scale (one NACK round trip plus the replay write), not the 30 s timer.
  A loss outside that window is not NACKed;
  it stays in the carrier's TCP send queue and TCP retransmits it. For loss
  recovery, the tunnel resets the stream only when every carrier TCP has failed
  (no ESTABLISHED socket below `tcp_retries2=15`) or on user/peer teardown; the
  30 s FEC timer only re-times the wait. A protocol violation (out-of-order
  delivery, a bad header) still fails closed. The replay ring and the RX arena are constant per FEC
  session (15 MiB + 16 MiB); neither grows with the stream. The final block of a stream is zero-padded and its
  real length travels in the parity `win_len` (with the FIN offset as a second
  source), so a short last packet is never delivered as padding. When `fec=off`,
  loss is repaired by retransmission from the replay ring and by
  duplication; there is no parity or erasure code. When the ring no longer holds
  the bytes, the receiver resets the stream (`holes_unrecoverable`) instead of
  guessing.
- **SC-LDPC and its per-flow adaptive law are implemented.** `mode=sc-ldpc`
  runs the downlink binary block (1×256) or the uplink grouped GF(16) code
  (8×32, `L=16`); `adaptive=on` moves the split along the shipped rate ladder
  in both directions. `rlnc`/`grouped` remain as the GF(2) band RLNC fallback.
- **Adaptive FEC.** The GF(2) family keeps Phase 1: `adaptive=on` moves `m`
  in the fixed `n + m = 8`, 32-group family; `n + m != 8` is not implemented.
  `mode=sc-ldpc` moves the configured split along the ladder; a non-ladder
  split stays fixed. `adaptive=off` keeps the configured shape.
- **Single-stream FEC.** A session carries one FEC flow (`0x100c`). The
  DATA/PARITY payload is the application byte stream, and an 8 KiB write is
  one DATA share, so no per-stream demux or window state exists.
- **One anchor per far tunnel.** A single `peer=` (or the first HELLO) fixes the
  source address. A multi-homed near host, or a NAT that rebinds the source
  address, has its unauthenticated HELLOs refused (`join_src_reject`); a HELLO
  that proves the configured `crypto_key=` may move the anchor instead. To keep
  the anchor fixed, pin `peer=` or one source address with `srcaddrs=`.
- **Trust on first use (TOFU) unless pinned.** Without `nonce=` and `peer=`, the first
  HELLO fixes both the nonce and the anchor, and the module logs one warning.
  Pin both on an untrusted path.
 `k × mean BDP` and the carrier
  aggregate.** `tx_window_bytes=0` takes `sum(snd_wnd)` (or `n_car × per-carrier
  BDP` when no window sample exists) as a floor over the single-flow `k × mean
  BDP` target, with a bootstrap floor of `max(1 MiB, n_car × 8 × payload)`.
  `k = (RTO + one-way + feedback delay) / mean SRTT` (about 2–5 on measured
  paths). The auto cap and the receiver's reorder budget scale with the carrier
  count (at least 8 MiB, eight full frames per carrier, 256 MiB ceiling), so a
  256-carrier tunnel is not held at one path's 8 MiB. An explicit
  `tx_window_bytes=` is the operator's contract and is not truncated. The
  reorder budget must still cover the window, because the peak tree occupancy is
  the whole window. Both ends should use the same `rx_reorder_bytes`, because
  the receiver's budget is not on the wire: the sender caps itself with its own
  configured value. A future WINDOW credit would remove that configuration
  contract.
- **The reorder tree and the ready queue are different costs.** A frame that
  arrives out of order waits in the tunnel's reorder tree (`rx_reorder_bytes`,
  8 MiB default, shared by the session); bytes that arrived in order but that
  the application has not read wait in the 64 MiB ready queue. Only the first
  is what the sender window must fit; the second is ordinary backpressure.
- **Guest CPU is the bottleneck.** In the latest QEMU runs `kworker` used about
  99% of one vCPU. The absolute Mbit/s above are a floor set by this guest, not
  a property of the line.
- **The receive ring costs about twice its payload.** `TBT_RX_RING_MAX` is
  64 MiB per stream, but each queued frame is `kmalloc(struct_size(frame, data,
  len))`; the frame header pushes every record over the next slab size, so a
  4 KiB record occupies an 8 KiB object and a full 8 KiB record a 16 KiB one. A
  strict send-then-read transfer also holds the echo in the public socket's
  receive queue; the guest's used memory peaked at about twice the 40 MiB
  transferred (75 MB → 163 MB in the memory row). Size the guest accordingly.

## Operations

Changing a rate, a duplication degree, or any other key does not need a reload:
re-send the full `tunnel` line, which replaces the tunnel and resets its
streams. A tunnel line replaces the whole tunnel, so include every required key.
`flush` closes every tunnel; to change one tunnel, re-send its line instead.

Rules do not survive a reboot. Save them once and restore them at boot:

```sh
cat /proc/net/tbtun/config > /etc/tbtun.conf
# at boot, before clients arrive:
cat /etc/tbtun.conf > /proc/net/tbtun/config
```

A systemd unit with `ExecStart=/bin/sh -c 'cat /etc/tbtun.conf > /proc/net/tbtun/config'`
and `After=systemd-modules-load.service` is enough; so is an `rc.local` line.
The module itself must be loaded first, by `/etc/modules-load.d/tbtun.conf` or
`modprobe tbtun`.

## Troubleshooting

- `tunnels` shows `state` and `streams`. A tunnel stuck at `idle` on the near
  side means the listener never bound; check `dmesg` and the port.
- `peers` is where a stalled carrier shows up: `state` other than `up`, an
  `rx_lag_ppm` that keeps climbing, `rtt_us` far above `minrtt_us`, or `inflight`
  pinned high while `tx_bytes` does not move.
- `stats` carries `gaps_total`. A non-zero count means a stream was reset by a
  reorder overflow; `holes_unrecoverable` counts holes the replay ring could
  not fill.
- `port_exhaust` climbing means the global or per-tunnel source port block is
  too small for the carrier count; widen it with `ports`, or widen that
  tunnel's `portblock=`. `port_alloc_fail` means the bitmap allocation failed
  (`ENOMEM`); the carrier retries either way.
- `win_block` climbing while `wire` stalls means the peer's advertised window is
  the limit, not loss; `win_resume` follows when the window opens.
- `join_src_reject` climbing means an unauthenticated near source address other
  than the anchor is trying to join; a multi-homed near host or a NAT address
  rebind without `crypto_key=` is the usual cause.
- `debug_loss=1` plus `carrier <i> drop_ppm 10000` drops one record in a hundred
  at the carrier's write boundary, so loss handling can be exercised without a
  lossy line. The `dropped` column in `peers` counts it.
- An `Internal error: Oops` in `__memcpy` with
  `tbt_http_rx_parse → tbt_rx_parse_pending → tbt_car_rx_work` in the stack is
  the uninitialised-parser bug, not a network fault. Check
  `cat /sys/module/tbtun/srcversion` on both ends against the artifact you
  meant to load.
- Unloading the module closes every listener and carrier; connected clients see
  EOF, not a hang.

## QoS caveat

The `http-kal` profile is a bet that the ISP classifies HTTP/1.1 as priority
traffic. It is not a fact, and the module cannot make a classifier privilege
anything. The bet is cheap to test, and testing it is part of using this
software: a ten-minute run with four configurations — plain TCP, one
`http-kal` carrier, four carriers, four `raw` carriers — tells you what each
result means.

If the measurement shows no gain from the HTTP profile, set `profile=raw`: the
per-5-tuple gain is independent of it. If it shows no gain from `N + M` either,
the line has an aggregate policer and no tunnel of this kind will help; a
WireGuard link with a single flow is then the honest choice.

## Security

By default the tunnel neither encrypts nor authenticates. `crypto=on` (the
default whenever `fec=on`) ChaCha8-obfuscates the FEC DATA/PARITY payload: it
keeps application bytes out of naive DPI signatures, but it is not
confidentiality. With `crypto_key=` unset the session key travels in clear text
in the HELLO body (`key_id=1`), and there is no message authentication. With
`crypto_key=` set on both ends, `key_id=0` and the HELLO key field is zero; the
v9 tag then authenticates the HELLO and authorises a restarted client or a
moved source address, but the data plane still has only a CRC. A wrong key
surfaces as a payload CRC failure, never as a silent downgrade. The
48-byte FEC header, the HTTP headers, the chunk framing and every control frame
are always plaintext. Configuration is root-only (`0600`), and so is loading the
module. If the path between the hosts is untrusted, run the tunnel inside
WireGuard: carriers then travel over the encrypted link, and the `raw` profile
is the only sensible choice, because an HTTP parser cannot see inside TLS.

## Scope

This is not a general-purpose VPN. It forwards TCP only, to one literal target
per tunnel, with no routing, no NAT, and no authentication. FEC v6 is
optional (`fec=on`); `mode=sc-ldpc` is implemented (binary downlink, grouped
GF(16) uplink), and `mode=rlnc`/`grouped` remain as the GF(2) band RLNC
fallback with `carriers = groups * (n + m)` up to 256 carriers. Multiple
concurrent FEC streams are not supported: one session carries one FEC flow
(`0x100c`). `crypto=on` is ChaCha8 body
obfuscation, not encryption, and it does not authenticate. It exists to multiply
a per-flow rate limit and to test whether a QoS profile helps. For anything
else, use WireGuard.

---

