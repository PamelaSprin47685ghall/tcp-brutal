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
chunk body is exactly one 48-byte FEC record (`TBT_FEC_CONV=0x1007`; 0x1006 was
the adaptive cut, 0x1005 was
the 0.2.2 stream-id cut, 0.1.0 used `0x1001`), while the request line, headers
and chunk framing stay
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

Adaptive FEC (`adaptive=`, off by default) has two paths. The GF(2)
`mode=rlnc`/`grouped` family keeps Phase 1: the shape stays in the
`n + m = 8`, 32-group family and `m` moves in 1..6. `mode=sc-ldpc` uses the
per-flow SC-LDPC estimator/controller: the uplink GF(16) 8x32/L16 codec can
switch along the rate ladder; the downlink binary 1x256/L8 codec is fixed at
rate 50%, so its controller stays at 50 (known limitation). The switch rides
`FEC_CFG`/`FEC_FBK` (wire `0x1007`). `session <id> adaptive=off` disables it
at runtime. Shapes with `n + m != 8` are not implemented for the GF(2)
family.

A session can carry multiple concurrent FEC streams in 0.4.0 (v7, wire
`0x1007`). Each stream owns a per-flow FEC TX/RX context and an admission
reference. A sequential connection after the previous one closes still resets
the old FEC RX/TX state, rebases the window on its own first block, and closes
the old target socket. DATA/PARITY carry the stream id (`0x1005`; carried
through `0x1007`), so a queued old-stream record cannot reach the new stream.

This tree implements C1–C8 of [DESIGN.md](DESIGN.md), plus the FEC v6 grouped
data plane, adaptive Phase 1, and the 256-carrier extension: the `/proc`
surface, the
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
carrier is replaced only after `min(2×stall, 300 s)` with no wire progress
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

## Build

```sh
make check
```

`check` builds `tbtun.ko` and then runs the host tests — grammar, fuzz, the
HTTP container, and the reorder/ack/control/multistream/anchor models — which
need no root, no module and no kernel headers.
`make host-test` runs only those tests.

`test_parse` now reports 357 checks; 365 was the older snapshot taken before the
five dead keys `framing`/`chunk`/`max_retry`/`dev`/`uid` and their parse cases
were deleted. What was deleted is the tests for keys already removed from the
grammar, not behavioural coverage; `make check` still reports 0 failures.

If the distribution's headers are not installed, the build can use an extracted
header payload instead of `/lib/modules/$(uname -r)/build`; see
[docs/build-env.md](docs/build-env.md) for the one-time setup, the `env.sh` it
produces, and the pahole gotchas. `make check` sources that environment itself
when it is present.

The multi-kernel compatibility check (5.15, 6.6, 6.12 and 7.0 on x86_64 and
aarch64, including a QEMU load check) is `scripts/kernel-matrix.sh`; see
[docs/kernel-matrix.md](docs/kernel-matrix.md) for the cells, the current
results and their cost.

## Packaging

The module version has one authority: `TBT_VERSION_MAJOR/MINOR/PATCH` in
`tbtun/tbt_uapi.h` (currently `0.4.0`). The wire
version is `TBT_WIRE_VERSION=1`, with `TBT_CONV=0x0007` for the 32-byte frames
and `TBT_FEC_CONV=0x1007` for the 48-byte FEC records; all three are independent of
the module version. `TBT_CONV 0x0005` removed the unused PREDICT/CANCEL
placeholder frames: a peer that still sends one fails the header check and its
carrier closes, and a 0x0004 peer is refused at the first header because its
conv does not match.

The current FEC wire is `0x1007`: `0x1002` added FIN's 16-byte body
(`pkt_end`, `pkt_base`) and the PARITY block marker (`win_len=0`,
`payload_len=0`, `epoch=0`); `0x1003` grew the ACK to 24 bytes and added
`deliver_bytes`; `0x1004` made NACK a 64-byte bitmap repair request; `0x1005`
added the 64-bit stream id at header +32 and grew the header to 48 bytes;
`0x1006` grew `FEC_CFG` to 48 bytes, made `FEC_FBK` a 48-byte report, and moved
HELLO body[0] to version 3 for the adaptive shape switch. `0x1007` is the
multi-stream cut: stream-local FEC seq/block ids, a 32-byte OPEN group bitmap,
and ALLOC (0x36); HELLO body[0] moves to version 4. A 0.2.x peer is refused at
the first HELLO: both ends must be upgraded together.
`scripts/mkdkmsconf.sh` derives
`PACKAGE_VERSION="<major>.<minor>.<patch>"`; `dkms.conf` is generated by
`make dkms.conf` / `make dkms-tarball`, so do not edit it by hand.
`scripts/install_dkms.sh` reads the same value with
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
| `hold_enable` | `1` | BBR hold: the 50 ms carrier poll restores the last good cwnd/pacing across idle periods. On by default; write `0` to opt out. It writes `tcp_sock` fields directly, which is not a stable kernel API |

The control surface is created in `init_net` only; there is no per-netns view.
Unload with `sudo rmmod tbtun`. Unloading flushes every tunnel and closes every
listener and carrier.

### Production deployment

Both ends must run the same fixed build. The 2026-11-06 field crash was memory
corruption inside the module: `tbt_carrier_new_locked()` allocated the
per-carrier `parser` and `http_rx` with `kmalloc()`, so a reused slab object
could leave `parser->have` dirty, and `tbt_http_rx_parse()` uses that value as
a `memmove()` length. The symptom is an `Internal error: Oops` within seconds
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
timeout 10 ./host/tbt_burst 127.0.0.1 9001 1 5 interleaved   # 1 MiB over 5 s
cat /proc/net/tbtun/stats | head -3
dmesg | tail -20
```

A retry hides the bug it is meant to expose: the second load starts on a slab
the first one already damaged, so a clean second run proves nothing. The full
harness — identity, visibility preflight, join gate, one burst, evidence
collection — is `review/wan-experiment/arm64/controlled-c8-once.sh`.

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
trust-on-first-use warning. Addresses are literal IPv4 only — no DNS, no IPv6
in 0.2.x.

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
| `listen` | — | `a.b.c.d:port`; near: public address; far: carrier address |
| `peer` | — | near: where carriers are sent; far: the only near source address allowed to join (the anchor; the port is grammar filler) |
| `target` | — | far only: literal address the payload is delivered to |
| `n` | `4` | DATA shares per group, `1..255`; `n + m <= 256` |
| `m` | `1` | Parity shares per group, `0..255`; `fec=on` needs `m >= 1`; `n + m <= 256` |
| `dup` | `1` | Duplication degree, `0..3` |
| `keepalive` | `5000` | Idle PAD interval, ms. The carrier's TCP keepalive is fixed at 30 s / 10 s / 3 probes and is not this key |
| `drip_ms` | `1000` | `http-kal-drip` idle byte interval, ms |
| `replay` | `8388608` | Replay ring bytes |
| `tunnel_rate` | `0` | Bytes/s ceiling, `0` = unset |
| `max_inflate` | `2.000` | Duplication inflation ceiling |
| `hole_budget_ms` | `0` | How long a hole may stay open, `0..60000` ms; `0` = auto from the measured RTT (floor 2 s) |
| `rx_reorder_bytes` | `8388608` | Receiver reorder budget, bytes, `1..268435456`; bounds each stream's reorder tree and the session-wide sum; `0` is refused |
| `tx_window_bytes` | `0` | Sender window bytes; `0` = auto from the carriers' BBR samples and aggregate advertised window (the cap scales with the carrier count); an explicit value must not exceed `rx_reorder_bytes` and is never truncated |
| `tx_retain_bytes` | `0` | Sender retention bytes, `0` = unlimited |
| `carrier_stall_ms` | `0` | Cap on the stall threshold, `0..3600000` ms; `0` = auto: 5 × RTO (`SRTT + 4×RTTVAR`), clamped to 1..60 s. An explicit value only lowers the auto threshold |
| `profile` | `http-kal` | `http-kal`, `http-kal-drip`, or `raw` |
| `fec` | `off` | `off` (old ARQ) or `on` (FEC v6 data plane) |
| `adaptive` | follows `fec` | `on` lets the 0.3.0 control law choose `n/m` in the `n + m = 8` family; `on` requires `fec=on`; unset means the same as `fec` |
| `mode` | `rlnc` | `rlnc` (one group), `grouped` (GF(2) band RLNC), or `sc-ldpc` (downlink 1×256 binary, uplink grouped GF(16); `w` is the codeword block) |
| `w` | `0` | RLNC window, `0..1024`; `0` resolves to `2*n` |
| `carriers` | `0` | Carrier slots, `0..256`; `0` resolves to `n + m`; must be a multiple of `n + m` |
| `crypto` | follows `fec` | `on` ChaCha8-obfuscates the FEC DATA/PARITY payload; `on` requires `fec=on`; unset means the same as `fec` |
| `crypto_key` | — | 64 hex digits: pre-shared 256-bit key; `-` clears it. Unset: the near side generates the key and sends it in HELLO (`key_id=1`) |
| `portblock` | `0-0` | Near only: source-port window for this tunnel's carriers, `1..65535`; `0-0` = draw from the global block set through `/ports`. Must hold at least `n + m` ports (`carriers` with FEC). A far tunnel records it but never dials |
| `dev` | — | Near only: bind each dialling carrier socket to this interface (`SO_BINDTODEVICE`) before connect, `1..15` bytes. A bind failure is a dial failure: `connect_fail` climbs and the carrier retries after 200 ms. Forces a direct path around a multi-homed or proxy-TUN route |
| `srcaddrs` | — | Comma-separated local source addresses; only the first is used |
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
HELLO that passes the nonce check becomes the session anchor: every later HELLO
from another address is refused before the nonce is examined
(`join_src_reject`), and a far `peer=` fixes the anchor at creation instead.
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
`c11` (NACK/replay repair), `c5` (duplication), and
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
| `stall_idle_skips`, `stall_declared`, `stall_recovered`, `stall_drop`, `stall_dead` | stall clock restarted because nothing was unacked / a FEC carrier was paused / its ack moved again / frames dropped by the ARQ pause helper (no caller yet, stays 0) / a paused carrier outlived `min(2×stall, 300 s)` and was replaced |
| `tx_window`, `rtt_mean_us`, `rttvar_us`, `rtt_max_us`, `txq_max` | effective sender window and its inputs / frame-queue bound |
| `holes_declared`, `holes_repaired`, `holes_unrecoverable`, `rx_max_stall_us` | hole accounting |
| `dup_tx`, `dup_used`, `dup_suppressed`, `inflate_ppm` | duplication |
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

## Verified performance

Measured with [benches/](benches/), which drives the same operator surface from
a KVM guest: virtio-net, one 10 Mbit htb class per source port with
`delay 20ms 10ms` netem, 40 MiB sent through `tbt_burst`'s strict
send-then-read phase (byte-exact), one configuration per boot. The control is a
single `dd | nc` flow on one shaped 5-tuple. `app` is the near tunnel's
`submit_bytes` rate; `wire/app` is whole-run carrier bytes over application
bytes; `retx_ppm` is copies per million unique DATA records. The module under
test is sha256 `0b8f0006…`, the module the final tree `f1282df` builds; raw
CSVs, consoles and the acceptance table are in `review/final-bench/`.

| Carriers | Control (Mbit/s) | App (Mbit/s) | App/control | wire/app | retx_ppm | strict / rc | CSV |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `n=1` | 2.43 | 3.11 | 1.28x | 1.0198 | 0 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T003712`) |
| `n=4` | 2.73 | 10.74 | 3.93x | 1.0138 | 2,333 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004314`) |
| `n=8` | 2.58 | 22.36 | 8.67x | 1.0123 | 2,041 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004635`) |
| `n=16` | 2.63 | 32.75 | 12.45x | 1.0268 | 12,719 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004925`) |

Every row is byte-exact, with `holes_unrecoverable=0`; the whole-run `wire/app`
range is 1.0123–1.0268 and `retx_ppm` is 0–12,719, so every row clears the
current targets (`wire/app < 1.2`, `retx_ppm < 100k`). The `wire/app` column is
the whole-run ratio; the CSV's 2 s window reads 1.0333 at `n=4`. One earlier
`n=4` run of the same module (`0b8f0006…`) measured 8.38 Mbit/s (`wire/app`
1.0146, `retx_ppm` 1,842) and another 11.37 Mbit/s, so the spread is real; the
ratio is the claim. `n=16` is 10.5x the one-carrier app rate. The full matrix
and CSVs are in `review/final-bench/summary.md`.

Acceptance evidence for tree `f1282df` (module `0b8f0006…`, evidence commit
`af3b9ba`): host, QEMU A–K (11/11) and adversarial (18 groups plus the crash
repro, 91 PASS / 0 FAIL / 0 SKIP in 284.4 s) results are in `review/final/`
and `review/final-qemu/`; throughput, flush and writer results are in
`review/final-bench/`; the DKMS install/load/unload run on a clean guest kernel
is in `review/dkms-qemu/`.

The writer probes cover two contracts. `pub.writer`, where the application
never reads, stays blocked inside `write()`: the 100 MiB offer is still blocked
at `sent=73400320/104857600` and the 1 GiB offer at
`sent=74448896/1073741824`, both with `errno=0 eagain=0 blocked=1`, so there is
no `EAGAIN` (the probe sets no `SO_SNDTIMEO`); memory stays bounded
(`oom=0->0`), and `flush` returns the memory in 0.03 s. `pub.writer_drain` and
`pub.writer_drain_1g`, where the application drains, complete: the 100 MiB run
reports `sent=104857600/104857600`, `got=104857600/104857600`, 71.5 MiB/s in
1.40 s; the 1 GiB run reports `sent=1073741824/1073741824`,
`got=1073741824/1073741824`, 69.9 MiB/s in 14.64 s. Both report
`errno=0/0 eagain=0 blocked=0 oom=0->0`, and `flush` takes 0.01 s. See
`review/final/README.md`.

The ratio is still the claim: `N` five-tuples beat one. These are laboratory
numbers from QEMU/slirp with one 10 Mbit htb class per source port and netem
`delay 20ms 10ms`; they are not an ISP measurement. The absolute values depend on
this QEMU/slirp path, not on a 10 Mbit bucket.

The cost table below is from earlier bench runs; the latest `n=1`/`n=4`
throughput runs above did not re-measure it.

| Cost | Measured |
| --- | --- |
| `http-kal` container vs `raw` | about 1% more bytes (one chunk per ~4 KB record) and about 3.5 percentage points more system CPU per vCPU |
| Hole repair, per-path loss | the knee is at the first 1%: app throughput 9.85 → 6.91 Mbit/s, then flat to 5.40 at 10%; `holes_unrecoverable` stayed 0 at 1%, 5% and 10% |
| Latency, 1 KB echo | +156 µs at p50, +274 µs at p99 over the direct path; one repaired hole moves p99 to about 24 ms, with a 32 ms maximum |

These are laboratory numbers from one host. They say nothing about whether an
ISP prioritises HTTP; that is [DESIGN.md §17](DESIGN.md)'s measurement, on a
real line.

## Known limitations

- **No IPv6 and no DNS.** Every address is literal IPv4: `listen=`, `peer=`,
  `target=`, and `srcaddrs=` accept only `a.b.c.d`; there is no name resolution.
- **No encryption and no authentication by default; `crypto=on` is
  obfuscation.** `crypto=on` (the default whenever `fec=on`) ChaCha8-obfuscates
  the FEC DATA/PARITY payload only. The 48-byte FEC header, the HTTP headers and
  every control frame stay in clear text, and there is no authentication. With
  `crypto_key=` unset, the near side generates a 32-byte session key and sends
  it in the HELLO v6 body (`key_id=1`); the far side adopts it. With
  `crypto_key=` set on both ends, `key_id=0` and the HELLO key field is zero; a
  mismatch surfaces as a payload CRC failure on every share. The nonce is a
  session identifier, not a secret. Run it inside WireGuard on an untrusted
  path; the `raw` profile is then the only sensible choice, because an HTTP
  parser cannot see inside TLS.
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
  runs the downlink binary block (1×256, `L=8`, rate 0.5) or the uplink
  grouped GF(16) code (8×32, `L=16`, rate 1/4); `adaptive=on` (off by default)
  switches the uplink along the ladder and keeps the downlink at rate 50%.
  `rlnc`/`grouped` remain as the GF(2) band RLNC fallback.
- **Adaptive FEC.** The GF(2) family keeps Phase 1: `adaptive=on` moves `m`
  in the fixed `n + m = 8`, 32-group family; `n + m != 8` is not implemented.
  `mode=sc-ldpc` uses the per-flow SC-LDPC controller (off by default): the
  uplink GF(16) codec switches along the ladder; the downlink binary codec is
  fixed at rate 50%. `adaptive=off` keeps the configured shape.
- **Multi-stream FEC v7.** A session can carry concurrent streams (0.4.0,
  wire `0x1007`); each stream has its own FEC TX/RX flow and admission
  reference. A sequential connection still resets the old state and closes the
  old target.
- **One anchor per far tunnel.** A single `peer=` (or the first HELLO) fixes the
  source address. A multi-homed near host, or a NAT that rebinds the source
  address, will have later HELLOs refused (`join_src_reject`); keep one stable
  source address, for example with `srcaddrs=`.
- **Trust on first use (TOFU) unless pinned.** Without `nonce=` and `peer=`, the first
  HELLO fixes both the nonce and the anchor, and the module logs one warning.
  Pin both on an untrusted path.
- **Throughput varies run to run.** The final `n=4` run measured 10.74 Mbit/s
  (`retx_ppm` 2,333, whole-run `wire/app` 1.0138); one earlier run of the same
  module measured 8.38 Mbit/s (`retx_ppm` 1,842, `wire/app` 1.0146) and another
  11.37 Mbit/s. The final `n=1`/`n=8`/`n=16` rows are 3.11/22.36/32.75 Mbit/s;
  the full matrix is in `review/final-bench/`.
- **The sender window is the larger of `k × mean BDP` and the carrier
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
- **DKMS install was verified in a clean guest kernel, not on this host.** The
  host cannot run it: `sudo` needs a password, and neither `dkms` nor the
  matching kernel headers is installed. In a QEMU guest booting the clean
  `7.2.9-1-MANJARO` kernel with the real `dkms 3.4.4` and the `linux72` headers,
  `scripts/install_dkms.sh` staged, built and installed `tbtun/0.0.1`,
  `modprobe` published `/proc/net/tbtun`, a tunnel line round-tripped
  canonically, `rmmod` unloaded it, a second `modprobe`/`rmmod` passed, and
  `--uninstall` removed every entry and file: 38 steps, 39 checks, 0 failures
  (`review/dkms-qemu/`). Do not read that as a host install.
- **Builds older than the 2026-11-07 fix corrupt kernel memory.** The
  per-carrier `parser` and `http_rx` were allocated with `kmalloc()`, so a
  reused slab object could leave `parser->have` dirty and
  `tbt_http_rx_parse()` used it as a `memmove()` length; the field host
  produced six Oopses within seconds of load. Fixed builds zero both objects
  (`kzalloc()`, plus an explicit `memset()` on the reuse path), and
  `tbt_rx_parse_pending()` refuses a carrier whose `have` exceeds its buffer
  as a second line of defence. Nothing on the wire distinguishes the two
  builds; check `srcversion` on both ends.

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
- `join_src_reject` climbing means a near source address other than the anchor
  is trying to join; a multi-homed near host or a NAT address rebind is the usual cause.
- `debug_loss=1` plus `carrier <i> drop_ppm 10000` drops one record in a hundred
  at the carrier's write boundary, so loss handling can be exercised without a
  lossy line. The `dropped` column in `peers` counts it.
- An `Internal error: Oops` in `__memcpy` with
  `tbt_http_rx_parse → tbt_rx_parse_pending → tbt_car_rx_work` in the stack is
  the uninitialised-parser bug, not a network fault. Check
  `cat /sys/module/tbtun/srcversion` on both ends against the artifact you
  meant to load. The 2026-11-06 evidence and the collection procedure are in
  `review/wan-experiment/arm64/stop-65-down-20261106T2250Z/forensics-65/`.
- Unloading the module closes every listener and carrier; connected clients see
  EOF, not a hang.

## QoS caveat

The `http-kal` profile is a bet that the ISP classifies HTTP/1.1 as priority
traffic. It is not a fact, and the module cannot make a classifier privilege
anything. The bet is cheap to test, and testing it is part of using this
software: [DESIGN.md §17](DESIGN.md) gives a ten-minute procedure with four
configurations — plain TCP, one `http-kal` carrier, four carriers, four `raw`
carriers — and tells you what each result means.

If the measurement shows no gain from the HTTP profile, set `profile=raw`: the
per-5-tuple gain is independent of it. If it shows no gain from `N + M` either,
the line has an aggregate policer and no tunnel of this kind will help; a
WireGuard link with a single flow is then the honest choice.

## Security

By default the tunnel neither encrypts nor authenticates. `crypto=on` (the
default whenever `fec=on`) ChaCha8-obfuscates the FEC DATA/PARITY payload: it
keeps application bytes out of naive DPI signatures, but it is not
confidentiality. With `crypto_key=` unset the session key travels in clear text
in the HELLO v6 body (`key_id=1`), and there is no message authentication. With
`crypto_key=` set on both ends, `key_id=0` and the HELLO key field is zero; a
wrong key surfaces as a payload CRC failure, never as a silent downgrade. The
48-byte FEC header, the HTTP headers, the chunk framing and every control frame
are always plaintext. Configuration is root-only (`0600`), and so is loading the
module. If the path between the hosts is untrusted, run the tunnel inside
WireGuard: carriers then travel over the encrypted link, and the `raw` profile
is the only sensible choice, because an HTTP parser cannot see inside TLS.

## Scope

This is not a general-purpose VPN. It forwards TCP only, to one literal target
per tunnel, with no routing, no NAT, no authentication, and no IPv6. FEC v6 is
optional (`fec=on`); `mode=sc-ldpc` is implemented (binary downlink, grouped
GF(16) uplink), and `mode=rlnc`/`grouped` remain as the GF(2) band RLNC
fallback with `carriers = groups * (n + m)` up to 256 carriers. Multiple
concurrent FEC streams are supported in 0.4.0 (v7, `0x1007`). `crypto=on` is
ChaCha8 body
obfuscation, not encryption, and it does not authenticate. It exists to multiply
a per-flow rate limit and to test whether a QoS profile helps. For anything
else, use WireGuard.

---

`logo.png` is the legacy TCP Brutal mark, kept for the repository's history; the
manual does not use it.
