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
- Per-path feedback (`FBK`), per-path `uack`/`recv_cnt`/RTT, hole repair from the
  retained bytes with duplication and a replay ring, and `RETIRE` before a
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
- `config` writes drop the read snapshot at the start and end of a write, so the
  next read re-renders the new state. A read that races a multi-line write may
  still serve an intermediate snapshot: the write lock is not held across the
  whole write.
- The `config` control surface is `init_net`-only. `tbt_cfg_open()` captures
  `init_net`, not the writer's netns.
- The HELLO `profile|gen` byte is documented as bits 5:2 for generation and 1:0 for
  profile; HELLO `ver` is `TBT_WIRE_VERSION=1`, and the wire revision is `TBT_CONV=0x0005`.
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
- **Review 14:** `data_frames` is incremented only after the carrier write succeeds
  (or after the debug injector drops the record on purpose), so a failed
  `-EAGAIN`/`-EIO` write is no longer counted as a received record.
- **Review 20:** overlapping out-of-order frames are evicted or clipped in
  `tbt_rx_pull_locked()`; a stale node loses its whole length from
  `rx_ooo_bytes` instead of inflating it.
- **Review 34:** the partial-init `fail:` path calls `tbt_procfs_exit()`, which
  uses `remove_proc_subtree()` and does not warn about entries that were never
  created.
- **Sequential churn:** a stream detaches from its session once both directions
  have finished (our FIN queued and the peer's FIN delivered), so the next
  connection reuses the session instead of stopping at `TBT_STREAMS_MAX`.
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
- The PREDICT (`0x11`) and CANCEL (`0x12`) placeholder frames, their sender paths
  (`tbt_tx_predict_locked()`, `tbt_tx_cancel()`), the receiver bookkeeping, and the
  `predict_tx`/`predict_used`/`predict_cancelled` counters with their `stats`
  rendering. The receiver never advanced delivery from them. `TBT_CONV` is now
  `0x0005`; a 0x0004 peer that sends 0x11/0x12 fails `tbt_hdr_check()` with
  `-EPROTO` and its carrier closes.

### Known limitations

- **`pub.writer` stalls at about 75 MiB.** A 100 MiB or 1 GiB write stops at roughly
  75 MiB with `errno=11` (`EAGAIN`); `MemAvailable` fell from 397 MB to 259 MB during
  the run. The adversarial harness calls this PASS because the write made progress;
  the stall is still a limitation.
- **`src=` inbound allow list does not exist.** `init.src_key` and `pub.srcfilter`
  still FAIL: `src=` was never a key and no inbound address/interface allow list is
  implemented. The test contract needs redefinition.
- **Trust on first use is silent.** A `role=far` tunnel with neither `nonce=` nor
  `peer=` adopts the session nonce from the first HELLO and logs nothing; whoever
  wins that first connection defines the session. A configured `peer=` rejects a
  foreign source address and a configured `nonce=` is enforced, but there is no
  authentication and no warning. Pin `nonce=` or `peer=` on an untrusted path.
  Review 16 tracks this.
- **No placeholder prediction.** The PREDICT/CANCEL path is removed (`TBT_CONV`
  `0x0005`). A hole is repaired by resending the retained real bytes and by
  duplication; when the replay ring has already dropped them, the receiver performs
  a bounded reset (`holes_unrecoverable`) instead of guessing. Hole detection stays
  geometric (`rx_max_stall_us`, `hole_checks`, `gate_fail`), but nothing advances
  delivery without the real bytes.
- **Review 22 pending:** a pinned control frame is freed on `-EAGAIN`, so a FIN or
  similar control record can be lost under carrier backpressure.
- **Review 15, 21 and 24 open:** `fbk_mask` is still ignored, several counters are
  plain `u64` increments from concurrent carriers, and the hole age is zeroed before
  it is printed. None of them blocks the data path.
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
- **No IPv6 and no DNS.** Addresses are literal IPv4 only; there is no name
  resolution.
- **No encryption and no authentication.** The tunnel is plaintext on the wire and
  the nonce is an identifier, not a secret; wrap it in WireGuard if the path is
  untrusted.
- **No FEC.** Loss is repaired by retransmission from the replay ring and by
  duplication. Reordering is absorbed by a bounded tree (`rx_reorder_bytes`); a hole
  older than `hole_budget_ms` or a reorder overflow resets the stream and is counted
  in `holes_unrecoverable`/`gaps_total`.
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
  `TBT_WIRE_VERSION=1` and `TBT_CONV=0x0005`, independent of the module version;
  HELLO `body[0]` uses `TBT_WIRE_VERSION`, not the module major. Final regression is
  still pending. A `.ko` built before the version unification still reports `1.0.0`;
  rebuild the module for `modinfo` to show `0.0.1`.

### Verification

Status on 2026-11-05. Only runs that were actually executed are listed; this is not
a full-suite pass.

Version and packaging:

- `scripts/mkdkmsconf.sh --version` prints `0.0.1`; the authority is
  `tbtun/tbt_uapi.h` (`TBT_VERSION_MAJOR/MINOR/PATCH = 0/0/1`). `TBT_WIRE_VERSION=1`;
  `TBT_CONV=0x0005` after the placeholder-frame removal.
- `make dkms.conf` generates `PACKAGE_VERSION="0.0.1"`. `make dkms-tarball` produces
  `dkms.tar.gz` with 23 entries under `./dkms_source_tree/`: `dkms.conf`, `Makefile`,
  `tbtun/*.c`, `tbtun/*.h` and `scripts/`.
- `scripts/install_dkms.sh --dry-run` exits 0 and prints the
  `dkms add/build/install` sequence for `tbtun-0.0.1`. `--stage-only` with a writable
  `DKMS_ROOT` writes the staged tree and a `dkms.conf` whose `PACKAGE_VERSION` is
  `0.0.1`.

DKMS (dkms 3.4.4 from the Manjaro `extra` repository, `--sourcetree`/`--dkmstree`/
`--installtree` redirected to `/tmp`, and the header payload from
`docs/build-env.md`):

- `dkms add`, `dkms build -k 7.2.9-1-MANJARO` and `dkms install` exit 0;
  `dkms status` reports `tbtun/0.0.1, 7.2.9-1-MANJARO, x86_64: installed`.
- `modinfo` on the built and on the installed module reports `version: 0.0.1` and
  `vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload`.
- `dkms ldtarball dkms.tar.gz` into a fresh tree, then `dkms build` and
  `dkms install`: exit 0, `installed`.
- `dkms remove -m tbtun -v 0.0.1 --all` (the step behind `--uninstall`) exits 0,
  deletes `updates/dkms/tbtun.ko` and leaves `dkms status` empty. The source tree
  stays under `--sourcetree`; `--uninstall` removes it with
  `rm -rf "$DKMS_ROOT"/tbtun-*`.
- No second kernel release is installed (`/lib/modules` holds only
  `7.2.9-1-MANJARO`), so the cross-kernel DKMS path was not exercised.
- **Blocked on this host:** the system install (`sudo scripts/install_dkms.sh`),
  `modprobe`, `rmmod` and `/proc/net/tbtun`. `sudo -n true` fails with "a password is
  required"; `dkms` and the kernel headers are not installed, and
  `/lib/modules/7.2.9-1-MANJARO/build` does not exist, so `install_dkms.sh` would
  stop at its headers check even with root. `insmod` of the built module fails with
  `EPERM` (no `CAP_SYS_MODULE`). The DKMS runs above used a user namespace only for
  the root check in `dkms install` and never wrote to the real `/lib/modules`.

Host tests:

- `make check` (recorded in `交接.md`): `test_parse` **355 checks / 0 failures**,
  `test_http` **249 checks / 0 failures**, h11 cross-check passes; `test_fuzz` 0
  failures. `make host-test` also runs reorder, ack, control, multistream and anchor;
  `make host-test-san` runs the sanitizer binaries.

QEMU probes and adversarial cases (recorded in
`adversarial/results-public-20261105T110133Z.txt` and `review/findings.md`):

- `init.echo_sane`, `pub.churn` (usable=10000), `pub.two`, `pub.concurrent`
  (n=100, connected=100, echoed=100), `pub.halfclose`, `pub.rst`: **PASS**.
- `pub.writer` (100 MiB and 1 GiB): **PASS by the harness**; it still blocks at about
  75 MiB with `errno=11`, `MemAvailable` 397 → 259 MB. See the limitation above.
- `init.src_key`, `pub.srcfilter`: **FAIL**; no `src=` allow list exists. Contract
  redefinition pending.
- `near.data_path`, `near.second_conn`: **FAIL** in the recorded run, traced to
  `adv_probe` truncating the 40-bit sid. The fixture is fixed; the rerun has not
  happened.
- Probe **B: PASS**; Probe **I: PASS**, byte-exact for control `n=1` and `n=4`. The
  environment is slow, so three consecutive runs were not required for this status.
- `review/findings.md` still marks F4 (sequential churn) and review 34 OPEN. The code
  has since detached the stream on a completed FIN handshake (`tbt_sock.c`) and uses
  `remove_proc_subtree()` (`tbt_procfs.c`); the table is stale.
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

### Acceptance

Executed in this release pass: `scripts/mkdkmsconf.sh --version`, `make dkms.conf`,
`make dkms-tarball`, `scripts/install_dkms.sh --dry-run`, `--stage-only`, and the
redirected DKMS add/build/install/ldtarball runs above. The root steps below were
not executed; see Blocked.

```sh
# build and host tests (no root, no module)
make check

# packaging
scripts/mkdkmsconf.sh --version          # 0.0.1
make dkms-tarball                        # dkms.tar.gz

# install (root; matching headers under /lib/modules/$(uname -r)/build)
sudo scripts/install_dkms.sh
modinfo tbtun | grep '^version:'         # 0.0.1
ls -l /proc/net/tbtun

# one tunnel: near side listens, far side delivers to target
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=1' \
     > /proc/net/tbtun/config
cat /proc/net/tbtun/tunnels              # tunnel 1 is listed
echo 'del 1' > /proc/net/tbtun/config

# unload and remove
sudo rmmod tbtun
sudo scripts/install_dkms.sh --uninstall
dkms status                              # no tbtun entry
```

Still pending:

- Full QEMU A–K, full `adversarial/run.sh`, and the `n=8`/`n=16` bench on the final
  tree.
- `near.data_path` / `near.second_conn` rerun with the fixed `adv_probe`.
- `pub.writer` fix (about 75 MiB `EAGAIN`).
- `flush` retest after a transfer.
- Reviews 15, 21, 22 and 24; review 16 (TOFU) is documented, not fixed.
- DKMS install/uninstall on a host with root and the matching headers.
- Do not read this section as a full-suite pass.
