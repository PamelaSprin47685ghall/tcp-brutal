# Changelog

All notable changes to tbtun are recorded here. This project is pre-release; the
wire revision is `TBT_CONV` and moves independently of the module version.

## [0.0.1] - 2026-11-05 (Release candidate)

First tracked release, currently a release candidate. It is **not released yet**: the
full QEMU, adversarial and bench suites still have to be re-run on the final tree.
Current data-plane probe and bench results are in Verification; they are not a
full-suite pass.

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
  profile; HELLO `ver` is `TBT_WIRE_VERSION=1`, and the wire revision is `TBT_CONV=0x0004`.
- DESIGN now records that carrier and listener binds use `SO_REUSEADDR`, and why:
  a replacement rebinds a just-freed reserved port while the old connection sits in
  `TIME_WAIT`; a failed `connect()` is covered by the port-block retry.

### Fixed

- **Review 23:** `hole_budget_ms=0` and `rx_reorder_bytes=0` are refused with `EINVAL`
  instead of creating a tunnel that dies on its first hole or reordered frame. The
  accepted ranges are now `1..60000` and `1..268435456`; omitted keys keep their
  defaults. Zero remains valid where it has a defined meaning (`tx_retain_bytes`,
  `tunnel_rate`, `replay`, `keepalive`, `drip_ms`).
- **Module unload:** `tbtun_exit()` unregisters `/proc`, shuts every tunnel down
  (stops listeners, cancels work, clears socket callbacks and releases sockets), then
  waits with `synchronize_rcu()` and `rcu_barrier()` before the module text can go
  away. The exit path no longer relies on `flush_workqueue()` as a barrier.
- **FBK queue head:** feedback is inserted at the head of the session queue, so a
  data/retransmit backlog cannot delay the `recv_cnt`/`uack` that the retransmit
  state machine depends on.
- **Receive proof:** a frame is delivered if the global `tx_ack` has passed its end;
  otherwise it is received on a path whose generation matches and whose `uack` has
  passed the frame end. Only the hole frame itself (`seq == tx_ack`) does not trust
  any per-path high-water; it is repaired by the retransmit timer and the geometric
  test. `recv_lag` remains loss evidence for the hole gate, but it no longer disables
  the receipt proof.
- **Retransmit accounting and the slow path:** each copy is tagged where it is queued
  (hole / slow / carrier loss) and counted where the carrier accepts the write;
  `retx_ppm` is DATA copies over unique DATA records and `dup_bytes_ppm` is copy
  payload over application bytes, so FBK no longer dilutes the rate. The slow path's
  grace moved from 2xRTO to 3xRTO: on a clean 40 MiB n=4 run the old grace copied 310
  frames that the peer had queued, not lost (whole-run wire/app 1.047 -> 1.024, honest
  retx 30k -> 8k; probes J/K still 3/3).
- **Retransmit scheduling onto a dead path:** a retransmit copy now skips a carrier
  whose TCP has unacked bytes and has not moved `snd_una` for an RTO. A blackholed
  carrier stops being written, so the lowest-inflight scheduler
  (`payload_bytes_sent - tx_ack`) used to pick it exactly when it could deliver
  nothing; probe K failed 1/3 with `TXSTALLKILL` at the 500 ms budget. Idle paths stay
  eligible and new data is unaffected. J/K pass 3/3 with `holes_unrecoverable=0`.
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

- **Sequential churn: far FIN does not detach.** When a far stream ends with a normal
  FIN, the session keeps its stream slot. Usable connections on one tunnel therefore
  stop at `TBT_STREAMS_MAX` (128); the 129th connection fails silently. The RST path
  is clean (200/200). A fix is in progress and is **not** in this tree.
- **Multi-stream is partial.** Two concurrent public connections on one tunnel work:
  `pub.two` reports A usable, B usable, and A usable again after B closes. Sequential
  reuse is limited by the FIN-detach bug above; 100 concurrent connections are not
  verified.
- **`pub.writer` stalls at about 75 MiB.** A 100 MiB or 1 GiB write stops at roughly
  75 MiB with `errno=11` (`EAGAIN`); `MemAvailable` fell from 410 MB to 269 MB during
  the run. Not fixed.
- **`src=` inbound allow list does not exist.** `init.src_key` and `pub.srcfilter`
  still FAIL: `src=` was never a key and no inbound address/interface allow list is
  implemented. The test contract needs redefinition.
- **Nonce/F8 pending.** The first-HELLO nonce adoption and session/rx application are
  still pending.
- **Predictive injection is experimental.** PREDICT is sender-side only. The sender
  emits a placeholder when the replay ring underflowed; the receiver records
  `predict_seq`/`predict_token` and does not advance delivery from them. The working
  recovery path is the sender's immediate retransmit and the replay ring. Do not rely
  on prediction for correctness.
- **Review 14 pending:** `data_frames` is not gated on the carrier write result in this
  tree, so a failed `-EAGAIN`/`-EIO` write can still be counted as a received record in
  the hole gate.
- **Review 20 pending:** overlapping out-of-order frames are not evicted from the
  reorder tree; a peer sending overlapping records can inflate `rx_ooo_bytes` until
  the reorder bound resets the stream.
- **Review 22 pending:** a pinned control frame is freed on `-EAGAIN`, so a FIN or
  similar control record can be lost under carrier backpressure.
- **Open reviews:** 13, 16, 21, 24 and 34 are also unfixed; together with 14, 20 and
  22 above, that is the full open set from the review pass.
- **Throughput and `retx_ppm` on shaped paths.** Probe I passes control `n=1` and
  `n=4` strict + interleaved byte-exact. Latest bench: `n=4` 11.39 Mbit/s,
  `wire/app` 1.0005, `retx_ppm` 96,095; `n=1` 3.46 Mbit/s, 1.0316, 1,090.
  `n=8`/`n=16` and the full bench matrix are pending.
- **Benchmark numbers are lab numbers.** Every throughput, latency and `retx_ppm`
  figure comes from the QEMU bench: one 10 Mbit htb class per source port and netem
  `delay 20ms 10ms`. No measurement was taken on a real ISP path. Do not read these
  figures as expected Internet performance.
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
  `tbtun/tbt_uapi.h`, so a rebuilt module reports `0.0.1` from `modinfo`.
- `dkms.conf` is generated by `scripts/mkdkmsconf.sh`, which reads those three macros
  from `tbt_uapi.h`; do not hand-edit `dkms.conf`. The default `PACKAGE_VERSION` is the
  pure semantic version `<major>.<minor>.<patch>`; a development suffix
  `.r<commits>.<sha>` is opt-in with `TBT_DKMS_GIT_SUFFIX=1`. Parsing failure is fatal.
  `make dkms-tarball` builds `dkms.tar.gz` from the generated file and the module
  sources.
- `scripts/install_dkms.sh` reads the same value with `mkdkmsconf.sh --version`; it
  installs the module only (nothing under `/usr/bin`).
- The module version is `0.0.1` (`TBT_VERSION_MAJOR/MINOR/PATCH = 0/0/1`), so
  `make dkms.conf` generates `PACKAGE_VERSION="0.0.1"`. The wire version is
  `TBT_WIRE_VERSION=1` and `TBT_CONV=0x0004`, independent of the module version;
  HELLO `body[0]` uses `TBT_WIRE_VERSION`, not the module major. Final regression is
  still pending. A `.ko` built before the version unification still reports `1.0.0`;
  rebuild the module for `modinfo` to show `0.0.1`.

### Verification

Status on 2026-11-05. Only runs that were actually executed are listed; this is not
a full-suite pass.

Version and packaging:

- `scripts/mkdkmsconf.sh --version` prints `0.0.1`; the authority is
  `tbtun/tbt_uapi.h` (`TBT_VERSION_MAJOR/MINOR/PATCH = 0/0/1`).
  `TBT_WIRE_VERSION=1` and `TBT_CONV=0x0004` are unchanged.
- `make dkms.conf` generates `PACKAGE_VERSION="0.0.1"`; `install_dkms.sh` stages
  `/usr/src/tbtun-0.0.1`.
- No `dkms.tar.gz` is present in the tree; regenerate it after the final regression.

Host tests:

- `make check`: **355 checks, 0 failures**.
- `make host-test-san`: **355 checks, 0 failures**.

QEMU probes and adversarial cases:

- Probe **B: PASS**.
- `pub.two`: **PASS**. A usable, B usable, and A usable again after B closes.
- `pub.halfclose`: **PASS**.
- `ctl.close_flush`: **PASS**.
- `init.rmmod`: **PASS**.
- `pub.churn`: **usable=128** (`TBT_STREAMS_MAX`) on the sequential path; the RST
  path is 200/200. This is the FIN-detach limitation above, not a pass.
- `pub.writer` (100 MiB and 1 GiB): **FAIL** at about 75 MiB with `errno=11`;
  `MemAvailable` 410 → 269 MB.
- `near.data_path`, `near.second_conn`: **FAIL**, traced to `adv_probe` truncating the
  40-bit sid. The fixture is fixed; the rerun has not happened.
- `init.src_key`, `pub.srcfilter`: **FAIL**; no `src=` allow list exists. Contract
  redefinition pending.
- Probe **I: PASS**, byte-exact for control `n=1` and `n=4`. The environment is slow,
  so three consecutive runs were not required for this status.
- This is **not** a full A–K/adversarial green.

Bench (QEMU lab emulation, one 10 Mbit htb class per source port, netem
`delay 20ms 10ms`, 40 MiB each way; not an ISP path):

- `n=4`: control 2.24 Mbit/s, app 11.39 Mbit/s, app/control 5.08x, `wire/app`
  1.0005, `retx_ppm` 96,095, `strict=1`, `rc=0`
  (`benches/results/20261105T112245-throughput.csv`).
- `n=1`: control 2.48 Mbit/s, app 3.46 Mbit/s, `wire/app` 1.0316, `retx_ppm`
  1,090, `strict=1`, `rc=0`
  (`benches/results/20261105T112725-throughput.csv`).
- Targets: `wire/app < 1.2`, `retx_ppm < 100k`; `n=4` is within both.
- `n=8` and `n=16`: not measured.

Still pending:

- Sequential churn FIN-detach fix and rerun.
- `near.data_path` / `near.second_conn` rerun with the fixed `adv_probe`.
- `pub.writer` fix (about 75 MiB `EAGAIN`).
- `flush` retest after a transfer.
- Reviews 13, 14, 16, 20, 21, 22, 24 and 34.
- Nonce/F8 session and rx application.
- PREDICT receiver-side progress.
- `n=8`/`n=16` bench, full `adversarial/run.sh`, and the full QEMU A–K suite.
- Regenerate `dkms.tar.gz`; run DKMS install/uninstall.
- 100 concurrent connections.
- Do not read this section as a full-suite pass.
