# Changelog

All notable changes to tbtun are recorded here. This project is pre-release; the
wire revision is `TBT_CONV` and moves independently of the module version.

## [0.0.1] - 2026-11-05 (Unreleased)

First tracked release. It is **not released yet**: the full QEMU, adversarial and
bench suites still have to be re-run on the final tree. Current data-plane probe and
bench results are in Verification; they are not a full-suite pass.

### Added

- A fully in-kernel near/far TCP tunnel: the module owns the public accepted socket
  and the far-side target socket; there is no userspace helper, proxy process or
  character device.
- `N` active carriers plus `M` spares, each on its own 5-tuple, with a per-session
  carrier array, spares that can be promoted, and a replacement worker that dials a
  fresh carrier at the same index.
- Carrier profiles `raw`, `http-kal` and `http-kal-drip`; the HTTP profiles use a
  chunked keep-alive container, the raw profile has no HTTP bytes.
- Per-path feedback (`FBK`), per-path `uack`/`recv_cnt`/RTT, sender-side
  PREDICT/CANCEL placeholders, duplication with a replay ring, and `RETIRE` before a
  replacement joins.
- A `/proc/net/tbtun` control surface: `config` (canonical write/read), `tunnels`,
  `peers`, `ports`, `stats`, and the debug-only `http` snapshot.
- `max_tunnels` and `max_carriers` module parameters are enforced: a new tunnel id
  over `max_tunnels` fails with `ENOSPC`; `n + m > max_carriers` fails with `EINVAL`.
  Both checks run before allocation, so a refused write changes nothing.
- `ports 0-0` and a blank line written to `/ports` are documented no-ops; they leave
  the current port block unchanged.

### Changed

- `config` reads use an exact-size snapshot held per open file description and served
  by offset. A read no longer stops after the first short buffer, and a config that
  does not fit is refused with `E2BIG` instead of being silently truncated.
- `config` writes invalidate the read snapshot at the start and end of a write, so a
  read that races a multi-line write sees a complete pre- or post-write view.
- The `config` control surface is `init_net`-only. `tbt_cfg_open()` captures
  `init_net`, not the writer's netns.
- The HELLO `profile|gen` byte is documented as bits 5:2 for generation and 1:0 for
  profile; the wire revision remains `TBT_CONV=0x0003`.
- DESIGN now records that carrier and listener binds use `SO_REUSEADDR`, and why:
  a replacement rebinds a just-freed reserved port while the old connection sits in
  `TIME_WAIT`; a failed `connect()` is covered by the port-block retry.

### Fixed

- **Module unload:** `tbtun_exit()` unregisters `/proc`, shuts every tunnel down
  (stops listeners, cancels work, clears socket callbacks and releases sockets), then
  waits with `synchronize_rcu()` and `rcu_barrier()` before the module text can go
  away. The exit path no longer relies on `flush_workqueue()` as a barrier.
- **Review 14:** `data_frames` is incremented only after a successful carrier write;
  a failed `-EAGAIN`/`-EIO` write no longer counts as a received record in the hole
  gate.
- **FBK queue head:** feedback is inserted at the head of the session queue, so a
  data/retransmit backlog cannot delay the `recv_cnt`/`uack` that the retransmit
  state machine depends on.
- **Receive proof:** a frame is delivered if the global `tx_ack` has passed its end;
  otherwise it is received on a path whose generation matches and whose `uack` has
  passed the frame end. Only the hole frame itself (`seq == tx_ack`) does not trust
  any per-path high-water; it is repaired by the retransmit timer and the geometric
  test. `recv_lag` remains loss evidence for the hole gate, but it no longer disables
  the receipt proof.
- **Short-write tail offset:** after a partial carrier write, the retained tail is
  copied from `fr->data + (done - TBT_HDR_LEN)` instead of `fr->data`, keeping the
  header/payload split aligned. This fixes the strict `n=4` payload corruption at
  about 1.2 MB.
- **TX queue bound:** the session TX queue has a hard byte cap (`TBT_TXQ_HARD_MAX`);
  retransmits may use a bounded overage so a hole filler can enter a queue full of
  window-blocked new data.
- **`-ENOMEM` as backpressure:** a transient HTTP-header allocation failure is treated
  like `-EAGAIN`; the frame is retried on another path and the carrier is not declared
  dead.
- **Config read:** exact-size rendering with `kvmalloc`, offset service, EOF at the
  end, `E2BIG` for a line/total that does not fit, and no fixed 32 KiB cap.
- **Ports:** `ports 0-0` and blank lines are no-ops; `ports 0-1` and reversed ranges
  are still refused. A too-long `/ports` line returns `E2BIG`.
- **Limits:** `max_tunnels` is checked in `tbt_tunnel_add()` under `tbt_lock`, before
  `kzalloc` and before detaching an existing tunnel; re-sending an existing id still
  replaces it at the cap. `max_carriers` is checked in `tbt_cfg_sane()` before any
  allocation or session creation.

### Removed

- `framing=` (including `framing=ext`), `chunk=`, `max_retry=`, `dev=` and `uid=`:
  these were parsed and rendered but never read by the data path. They are now
  rejected as unknown keys with `EINVAL`.
- `netns_optin`: the module parameter, the `tbt_netns_optin()` accessor and its
  declaration were removed. The control surface is `init_net`-only.
- The `src=` claim: `src=` was never a key and no inbound address/interface allow
  list is implemented. `srcaddrs` selects the local source address for near-side
  carriers (only the first entry is used).

### Known limitations

- **One active stream per session.** A second public connection is not served by the
  same session; multi-stream support is pending.
- **Half-close is not propagated as EOF.** `shutdown(SHUT_WR)` on the public side does
  not reliably deliver EOF to the far target yet.
- **PREDICT is sender-side only.** The receiver records `predict_seq`/`predict_token`
  but does not advance delivery from them; the working recovery path is the sender's
  immediate retransmit and the replay ring.
- **Review 20 pending:** overlapping out-of-order frames are not evicted from the
  reorder tree; a peer sending overlapping records can inflate `rx_ooo_bytes` until
  the reorder bound resets the stream.
- **Review 22 pending:** a pinned control frame is freed on `-EAGAIN`, so a FIN or
  similar control record can be lost under carrier backpressure.
- **Throughput and `retx_ppm` on shaped paths.** Probe I passes control `n=1` and
  `n=4` strict + interleaved byte-exact. Latest bench: `n=4` 11.39 Mbit/s,
  `wire/app` 1.0005, `retx_ppm` 96,095; `n=1` 3.46 Mbit/s, 1.0316, 1,090.
  `n=8`/`n=16` and the full bench matrix are pending.
- **`flush` can block after a transfer.** Observed historically; it was not
  re-tested in the latest data-plane run, so treat it as pending verification.
  Prefer re-sending the full tunnel line between measurements.
- **No IPv6.** Addresses are literal IPv4 only.
- **No encryption.** The tunnel is plaintext on the wire; wrap it in WireGuard if the
  path is untrusted.
- **No per-netns control surface.** `/proc/net/tbtun` is created in `init_net` only.
- **Only the first `srcaddrs` entry is used.** Multi-address carriers and
  per-`srcaddr` listeners are not implemented.

### Packaging

- `MODULE_VERSION` is derived from `TBT_VERSION_MAJOR/MINOR/PATCH` in
  `tbtun/tbt_uapi.h`.
- `dkms.conf` is generated by `scripts/mkdkmsconf.sh`, which reads those three macros
  from `tbt_uapi.h`; do not hand-edit `dkms.conf`. The default `PACKAGE_VERSION` is the
  pure semantic version `<major>.<minor>.<patch>`; a development suffix
  `.r<commits>.<sha>` is opt-in with `TBT_DKMS_GIT_SUFFIX=1`. Parsing failure is fatal.
  `make dkms-tarball` builds `dkms.tar.gz` from the generated file and the module
  sources.
- `scripts/install_dkms.sh` reads the same value with `mkdkmsconf.sh --version`; it
  installs the module only (nothing under `/usr/bin`).
- The module version is not unified to `0.0.1` yet. The current tree reports the
  pre-release `1.0.0`; the generated `dkms.conf` now says `PACKAGE_VERSION="1.0.0"`.
  The planned decoupling (`TBT_WIRE_VERSION=1`, HELLO `body[0]` using it, module
  version `0.0.1`) is recorded in `review/analysis/p3-contract.md` §6.5 and has not
  been implemented.

### Verification

Host-side evidence on the current tree (2026-11-05):

- `make check`: **314 checks, 0 failures**; `test_fuzz` seed=1 **8,349,443 checks,
  0 failures**; `test_http` **249 checks, 0 failures**; h11 cross-check accepted the
  emitted request/response.
- `make host-test-san`: **314 checks, 0 failures**; `test_fuzz` 8,349,443;
  `test_http` 249; `test_http_fuzz` 4,078,276; all 0 failures.
- `bash review/lint.sh`: **15/15 checks passed, 12/12 self-tests passed**.

Data-plane QEMU probes (measured 2026-11-05):

- **A, B, E, G, I, J, K: PASS.** J and K each passed 3 of 3 consecutive runs.
  I passed byte-exact for control `n=1` and for `n=4` in both the strict and the
  interleaved phase.
- This is not a full-suite green: only the probes above were run.

Bench (QEMU, one 10 Mbit htb class per source port, netem `delay 20ms 10ms`,
40 MiB each way):

- `n=4`: control 2.24 Mbit/s, app 11.39 Mbit/s, app/control 5.08x, `wire/app`
  1.0005, `retx_ppm` 96,095, `strict=1`, `rc=0`
  (`benches/results/20261105T112245-throughput.csv`).
- `n=1`: control 2.48 Mbit/s, app 3.46 Mbit/s, `wire/app` 1.0316, `retx_ppm`
  1,090, `strict=1`, `rc=0`
  (`benches/results/20261105T112725-throughput.csv`).
- Current targets are `wire/app < 1.2` and `retx_ppm < 100k`; the `n=4` run is
  within both, and `n=1` is 1.0316 / 1,090.
- `n=8` and `n=16`: **pending final regression**; no multiplier claim is made.

Still to run on the final 0.0.1 tree before release:

- `n=8`/`n=16` bench runs.
- Remaining QEMU probes and the full `adversarial/run.sh` suite, including
  `init.max_tunnels`, `init.max_carriers`, `init.rmmod`, `cat.cfg_self` and
  `init.cfg_roundtrip`.
- A final `modinfo -p tbtun.ko` / `modinfo -F version tbtun.ko` check after the
  version unification described under Packaging.
