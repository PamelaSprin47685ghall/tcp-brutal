# Changelog

All notable changes to tbtun are recorded here. The wire revision is `TBT_CONV`
and moves independently of the module version.

## [0.0.1] - 2026-11-06

First tracked release. The module version is `0.0.1`; the wire version is
`TBT_WIRE_VERSION=1` and the wire revision is `TBT_CONV=0x0005`. The data plane
is a fully in-kernel near/far TCP tunnel: `N` active carriers plus `M` spares,
three carrier profiles, per-path feedback, replay-ring retransmission, and a
`/proc/net/tbtun` control surface. The host tests, the sanitizer build,
`review/lint.sh`, the final-tree bench rows and the final adversarial results
are recorded in Verification.

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
- Far-side session anchoring: `nonce=` pins the session nonce and `peer=` pins the
  near source address before the first HELLO; `join_src_reject` counts a HELLO from
  a source other than the anchor. Without either, the first HELLO is adopted
  trust-on-first-use and the module warns once.
- Control-frame lifetime and carrier accounting in `/proc/net/tbtun/stats`:
  `ctl_lost`, `ctl_retry`, `ctl_requeue`, `ctl_obsolete`, `ctl_queued`;
  `port_exhaust`, `port_alloc_fail`; `stall_paused_skips`, `win_block`,
  `win_resume`. Retransmits are split into `retx_hole`, `retx_slow`,
  `retx_pathloss` and `retx_other`, with `data_tx`, `retx_data_ppm` and
  `dup_bytes_ppm` as the session-wide rates.
- A `reconcile` stats line publishes the live and retired halves of the session
  totals, so `wire`, `data_tx` and `retx_frames` equal the live `peers` rows plus
  the carriers that have retired.

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

All 39 review findings and the adversarial F1–F8 items are closed in this
tree. The bullets record the fixes that changed observable behavior, then the
grouped entries close the remaining lifetime, wire and input-handling findings.
`review/findings.md` keeps the per-ID detail and the final status table.

- **Shared statistics counters:** every session counter with more than one writer
  context is `atomic64_t`, incremented with `atomic64_inc/add` and read through
  `tbt_stat_read()`, so concurrent carriers and stream workers can no longer lose
  an update. Counters written only by the pump or by one carrier's rx work stay
  `u64`, with the writer named in `struct tbt_session`; the hole gate reads the
  same values with the same comparison. A carrier that leaves the live array now
  moves its `tx_bytes`/`data_frames`/`retx_data` into session-wide retired totals,
  and the new `reconcile` stats line publishes both halves so the aggregates stay
  equal to the live `peers` rows plus the retired carriers. `review/lint.sh`
  refuses a regression to a plain field access.
- **Zero-length control frames and the carrier pump:** `tbt_pump_work()` only ran the
  pump when `tx_queued` was non-zero, but `tx_queued` counts payload bytes and a pinned
  HELLO/OPEN/FIN/RST has `body_len=0`. After a stream was killed its DATA frames were
  purged and a lone RST no longer woke the 2 ms timer; it waited for an unrelated
  `sk_write_space` that a quiet carrier never produces. The timer now also tests
  `ctl_queued`, the pinned-frame count (paired on enqueue, write, failure and obsolete
  drop; zeroed with the queue). Measured: before the fix the RST sometimes never left
  the module queue (`wire` lacked the 32-byte record); after it the record is in `wire`
  with `ctl_lost=0`, `ctl_queued=0`. The peer-window gate below removes the remaining
  zero-window delay for the tail.
- **Carrier stall vs peer pause:** `tbt_car_poll_work()` now defers the stall/loss
  decision while `peer_paused` (session or stream) or our own delivery pause is set,
  restarts the window, and counts `stall_paused_skips`. The final near group passes
  `near.fin_eagain` and `near.rst_eagain`: `peer_paused=1`, the FIN/RST reaches
  `wire` (536,418 and 98,880 bytes), `holes_unrecoverable=0`, and
  `stall_paused_skips` is 42/9. The earlier failure was a carrier declared lost
  while paused.
- **Peer-window admission:** DATA is now admitted only while
  `write_seq - snd_una < snd_wnd` (one chunk when `snd_wnd` is 0). Filling the carrier's
  TCP sndbuf past the advertised window parked the tail in zero-window persist, where
  the last ~282 bytes of `near.rst_eagain` did not move for 60 s. `snd_nxt` was the
  wrong cursor (it counts sent bytes); `write_seq` includes the queued tail. Control
  frames bypass the gate, so a pinned RST leaves after the in-window bytes. Counters
  `win_block`/`win_resume` are on the `c3` stats line; a full peer window counts as the
  pump's hiwat proof. Measured: the final near group is 14/14; `near.rst_eagain`
  shows `win_block=235`, `win_resume=1`, the RST in `wire` at 98,880 bytes,
  `ctl_lost=0`, `ctl_queued=0`; `near.fin_eagain` shows `win_block=6720`,
  `win_resume=1`, the FIN in `wire` at 536,418 bytes.
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
  `retx_data_ppm` (the bench `retx_ppm` column) is DATA copies over unique DATA
  records and `dup_bytes_ppm` is copy payload over application bytes, so FBK no
  longer dilutes the rate. The slow path's grace moved from 2xRTO to 3xRTO: on a
  clean 40 MiB n=4 run the old grace copied 310 frames that the peer had queued, not
  lost (whole-run wire/app 1.047 -> 1.024, honest retx 30k -> 8k; probes J/K passed
  3/3 in the retransmit-fix run).
- **Retransmit scheduling onto a dead path:** a retransmit copy now skips a carrier
  whose TCP has unacked bytes and has not moved `snd_una` for an RTO. A blackholed
  carrier stops being written, so the lowest-inflight scheduler
  (`payload_bytes_sent - tx_ack`) used to pick it exactly when it could deliver
  nothing; probe K failed 1/3 with `TXSTALLKILL` at the 500 ms budget. Idle paths stay
  eligible and new data is unaffected. J/K passed 3/3 with `holes_unrecoverable=0`
  in that run.
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
- **Review 29–31 (config bounds):** the renderer fits `TBT_LINE_MAX` (1024); a
  config read has no fixed 32 KiB cap; the write drain refuses an over-long tail
  instead of truncating it.
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
- **Review 1–10 and 36–39 (lifetime and teardown):** a replaced carrier is parked
  on `sess->dead` and freed after RETIRE; the retention trim runs only under
  `pump_lock`; the retransmit mark is gated on `snd_una`; a failed create destroys
  the new session and the old tunnel; every `sk_user_data` reader takes
  `sk_callback_lock`, and stop clears the callback before `cancel_work_sync`; the
  stream insert is rechecked under `sess->lock`; `w_tx`/`w_pump` are cancelled
  before `pump_lock`; `st->sk` is NULL before `sock_release`; `accepting` is set
  after the session exists; a short carrier write keeps its tail in `c->part`;
  `peers` holds the socket; teardown stops carriers and frees the TX queue before
  the unlink; the `http_rx` allocation is checked, `tbt_carrier_free()` covers the
  failure paths, and `c->node` is initialized before them.
- **Review 11–13 and 27 (wire):** `TBT_CONV=0x0005`; RETIRE is `0x24`; HELLO
  carries the profile in bits 1:0 and the generation in bits 5:2; PREDICT and
  CANCEL are removed; a zero-length DATA frame is refused.
- **Review 15 and 24 (accounting details):** the unused `fbk_mask` parameter is
  gone; the hole age is computed before `hole_since_ns` is cleared and printed
  as `age_us`.
- **Review 16 (session trust):** a far `nonce=` pins the nonce, a far `peer=`
  pins the anchor address, and a HELLO from another source is refused and counted
  in `join_src_reject`. Without either, the first HELLO still fixes the session
  (trust-on-first-use) and the module warns once.
- **Review 17, 28 and 33 (proc and HTTP buffers):** the `http` proc entry is
  removed with the subtree; `http_hdr` is allocated with `TBT_HTTP_HDR_MAX` and
  `http_rx` with `sizeof(*c->http_rx)`, both checked; the `http` snapshot is
  copied under `pump_lock`.
- **Review 22 (pinned control frames):** a control frame that meets `-EAGAIN` is
  retried and requeued, not freed. `ctl_retry`, `ctl_requeue`, `ctl_obsolete` and
  `ctl_lost` account for the lifetime; `ctl_queued` is the pinned-frame count.
- **Review 35 (dead code):** the five inert keys and the two dead declarations
  are gone.
- **Adversarial F1–F8:** the close-flush NULL dereference is closed by capturing
  `init_net` in `tbt_cfg_open()`; `rmmod` unregisters `/proc`, shuts every tunnel
  down, restores the core callbacks, releases the sockets and waits with
  `synchronize_rcu()`/`rcu_barrier()`; a completed FIN handshake detaches the
  stream, so sequential churn reuses the session; half-close delivers EOF;
  far-side input is read and delivered (the probe's 40-bit sid truncation is
  fixed); the feedback tick runs on `system_dfl_wq`, so the deprecated-workqueue
  warning is gone; and `src=` is not claimed. F9 (nonce replay from another
  source) is closed by the anchor; F10 and F11 by the enforced limits; F12 by
  `port_exhaust`/`port_alloc_fail`.
- **P0–P3:** P0 (rmmod and the data-plane regression) is the unload order plus
  the retransmit accounting, 3xRTO grace, stalled-path avoidance and short-write
  fix above; P1 (multi-connection, half-close, flush, port exhaustion, limits) is
  the stream detach, EOF delivery, control-frame lifetime, port counters and
  limit checks; P2 (per-stream ack/SACK and the workqueue warning) is the
  per-stream `tx_ack`/frame reference and `system_dfl_wq`; P3 (grammar and config
  read) is the key removal and the exact-size config snapshot.

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

- **No IPv6 and no DNS.** Addresses are literal IPv4 only; there is no name
  resolution.
- **No encryption and no authentication.** The tunnel is plaintext on the wire and
  the nonce is an identifier, not a secret; wrap it in WireGuard if the path is
  untrusted. A far side with neither `nonce=` nor `peer=` is trust-on-first-use:
  the first HELLO fixes the session and the module warns once. A single `peer=`
  anchor rejects a multi-homed or NAT-rebound near source.
- **No FEC.** Loss is repaired by retransmission from the replay ring and by
  duplication. Reordering is absorbed by a bounded tree (`rx_reorder_bytes`); a hole
  older than `hole_budget_ms` or a reorder overflow resets the stream and is counted
  in `holes_unrecoverable`/`gaps_total`.
- **No inbound address or interface allow list.** `src=` is not a key and there is
  no `dev=`/`uid=` filter. The final `pub.srcfilter` case passes under the redefined
  contract: a connection from 127.0.0.2 is usable, and the tunnel identity is the
  nonce. Do not read that as source filtering.
- **`pub.writer` covers two cases: an application that never reads and one that
  drains.** The final contract requires the write to finish (`sent==total`) or to
  still be blocked inside `write()` when the 10 s observation ends (`blocked=1`,
  `child_state=running`); the probe sets no `SO_SNDTIMEO`, so `EAGAIN` would be
  a failure. With no reader, the 100 MiB and 1 GiB offers stop at
  `sent=73400320/104857600` and `sent=74448896/1073741824`, both with
  `errno=0 eagain=0` and still blocked. With a draining reader,
  `pub.writer_drain` moves 100 MiB (104,857,600/104,857,600 sent and received,
  `errno=0/0 eagain=0 blocked=0`) in 1.40 s at 71.5 MiB/s (`mbps` in the
  result line), and
  `pub.writer_drain_1g` moves 1 GiB (1,073,741,824/1,073,741,824 sent and
  received, `errno=0/0 eagain=0 blocked=0`) in 14.64 s at 69.9 MiB/s. Closing
  the socket and issuing `flush` returns `MemAvailable`
  from a low of 218–223 MB to 396–397 MB in 0.03 s, with `oom=0->0` and no RST;
  both draining runs read `oom=0->0` and flush in 0.01 s. Where the unread
  write stops depends on the send window at that moment; it is neither a fixed
  ceiling nor a leak, and repeated cases do not grow without bound.
- **The peer-window gate is a heuristic.** DATA is admitted while
  `write_seq - snd_una < snd_wnd`; a full peer window counts as the pump's hiwat
  proof. `win_block`/`win_resume` report the gate, and control frames bypass it.
- **Throughput and run-to-run variance.** The final matrix delivered 3.11 /
  10.74 / 22.36 / 32.75 Mbit/s at n=1/4/8/16, with whole-run `wire/app`
  1.0198 / 1.0138 / 1.0123 / 1.0268 and `retx_ppm` 0 / 2,333 / 2,041 /
  12,719; every row is `strict=1 rc=0` and `holes_unrecoverable=0`. One
  earlier n=4 run of the same module measured 8.38 Mbit/s (`wire/app` 1.0146,
  `retx_ppm` 1,842), another 11.37 Mbit/s; that spread is real. The guest's
  `kworker` is about 99% of one vCPU during a transfer, and the 64 MiB rx ring
  costs about 2x physical memory because the strict send-then-read pattern
  holds the echo. The full matrix is `review/final-bench/summary.md`.
- **Benchmark numbers are lab numbers.** Every throughput, latency and `retx_ppm`
  figure comes from the QEMU bench: one 10 Mbit htb class per source port and netem
  `delay 20ms 10ms`. No measurement was taken on a real ISP path. Do not read these
  figures as expected Internet performance.
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
  HELLO `body[0]` uses `TBT_WIRE_VERSION`, not the module major. `make dkms-tarball`
  builds the tarball from this tree. A `.ko` built before the version unification
  still reports `1.0.0`; rebuild the module for `modinfo` to show `0.0.1`.

### Verification

Status on 2026-11-06. The final code tree is `f1282df`; `af3b9ba` adds only the
evidence files under `review/final/`, `review/final-qemu/`, `review/final-bench/`
and `review/dkms-qemu/`. The host, QEMU, adversarial and bench rows below were
measured with module sha256 `0b8f0006…`, the module the final tree builds.
Evidence lives in `review/final/` (host, QEMU A–K, adversarial),
`review/final-qemu/` (QEMU and adversarial run logs), `review/final-bench/`
(throughput, flush and writer runs) and `review/dkms-qemu/` (DKMS install, load
and unload on a clean guest kernel). The counter-reconciliation row names its own
module.

Host tests:

- `make check` (module build plus the host suites): `test_parse` **357 checks /
  0 failures**, `test_fuzz` **7,550,734 / 0**, `test_http` **249 / 0**,
  `test_reorder` **52,792 / 0**, `test_ack` **10,026 / 0**, `test_control`
  **181 / 0**, `test_multistream` **68,580 / 0**, `test_anchor` **124 / 0**; the
  h11 cross-check accepts a POST and a 200 response.
- `make host-test-san`: `test_parse` **357 / 0**, `test_fuzz` **7,550,734 / 0**,
  `test_http` **249 / 0**, `test_http_fuzz` **4,078,276 / 0**; the sanitizer
  binaries report 0 failures.
- `bash review/lint.sh`: **16/16 checks**, **14/14 self-tests**, exit 0. The
  atomic-stats, proc-entries-removed and retain-trim checks are part of the 16.

`test_parse` now reports **357 checks**; 365 was the older snapshot taken before
the five dead keys `framing`/`chunk`/`max_retry`/`dev`/`uid` and their parse
cases were deleted. What was deleted is the tests for keys already removed from
the grammar, not behavioural coverage; `make check` still reports 0 failures.

Parser and `/proc` checks:

- `test_parse` 357/0 is the grammar check; `test_fuzz` and `test_http_fuzz` are
  the hostile-input and HTTP-container checks above.
- Counter reconciliation (`tests/qemu/run.sh --probes C,E`) was accepted on the
  counters tree (module sha256 `cdaf9ad8…`): ten samples per probe, every
  aggregate monotone, then `wire == live_bytes + retired_bytes`,
  `data_tx == live_data + retired_data`, `retx_frames == live_retx +
  retired_retx` and `live_bytes == sum(peers tx_bytes)`; every delta was 0.
  Probe C kept `retired_bytes=197463 retired_data=24 retired_retx=15`; probe E
  `329047/40/25`. The final tree keeps the same `atomic64_t`/`tbt_stat_read()`
  path and the same `reconcile` line.

QEMU probes:

- `tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900`: **11 PASS /
  0 FAIL / 0 SKIP** in 165.7 s on module sha256 `0b8f0006…`
  (`review/final/qemu-AK-summary.txt`,
  `review/final-qemu/qemu-AK-console-f1282df.log`).

Adversarial:

- `TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build` on the final
  module (sha256 `0b8f0006…`, 2026-11-06): **91 PASS / 0 FAIL / 0 SKIP** in
  284.4 s, all 18 default groups plus the isolated close-flush crash repro. The
  green run prints 91 `ADV_RESULT` PASS lines; `console.klog.*` prints only on
  failure, so there is no 92nd line. Archive:
  `review/final/adversarial-results.txt`; run logs in `review/final-qemu/`.
- `pub.churn` (10,000 connect/send/close cycles) is judged with a 2 s timeout
  per cycle and one retry only after a timeout (`retried=` in the result line).
  The contract is still `usable=10000 refused=0 other=0`. Two final-criterion
  runs read 10000/10000 (`retried=1` and `retried=0`); the archived run is
  `usable=10000 refused=0 other=0 retried=0`.
- `pub.writer` (100 MiB and 1 GiB) must either finish the write
  (`sent==total`) or still be blocked in `write()` when the 10 s window ends
  (`blocked=1`, `child_state=running`). The probe sets no `SO_SNDTIMEO`;
  `EAGAIN` would be the probe's own timeout, not module backpressure, and is a
  failure. Both final runs read `errno=0 eagain=0 blocked=1
  child_state=running`: 100 MiB `sent=73400320/104857600`, 1 GiB
  `sent=74448896/1073741824`. With a reader that drains, `pub.writer_drain`
  moves 100 MiB, `sent=104857600/104857600 got=104857600/104857600
  errno=0/0 eagain=0 blocked=0` in 1.40 s at 71.5 MiB/s (`mbps` in the result
  line), and
  `pub.writer_drain_1g` moves 1 GiB, `sent=1073741824/1073741824
  got=1073741824/1073741824 errno=0/0 eagain=0 blocked=0`, in 14.64 s at
  69.9 MiB/s; both flush in 0.01 s with `oom=0->0`. After the child is killed
  and the socket closed, `flush` takes 0.03 s, `MemAvailable` rises from a low
  of 223,296/218,072 kB to 397,228/396,388 kB against 396,852/396,992 kB
  before, and `oom=0->0`.

Bench (QEMU lab emulation, one 10 Mbit htb class per source port, netem
`delay 20ms 10ms`, 40 MiB each way; not an ISP path). Every row is `rc=0`,
`strict=1` and byte-exact; the whole-run `wire/app` is the acceptance number,
not the CSV's 2 s window column.

| n | control | app | app/control | wire/app | retx_ppm | holes_unrecoverable |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 2.43 | 3.11 | 1.28x | 1.0198 | 0 | 0 |
| 4 | 2.73 | 10.74 | 3.93x | 1.0138 | 2,333 | 0 |
| 8 | 2.58 | 22.36 | 8.67x | 1.0123 | 2,041 | 0 |
| 16 | 2.63 | 32.75 | 12.45x | 1.0268 | 12,719 | 0 |

- Stamps and CSVs: `review/final-bench/csv/20261106T003712-…` (n=1),
  `…004314-…` (n=4), `…004635-…` (n=8), `…004925-…` (n=16). The full matrix is
  `review/final-bench/summary.md`.
- The whole-run `wire/app` range is 1.0123–1.0268 and `retx_ppm` is 0–12,719;
  every row clears `wire/app < 1.2` and `retx_ppm < 100k`. At n=4 the CSV's
  2 s window reads 1.0333 while the whole run reads 1.0138.
- One earlier n=4 run of the same module measured 8.38 Mbit/s (`wire/app`
  1.0146, `retx_ppm` 1,842) and another 11.37 Mbit/s; that spread is the
  recorded run-to-run variance, not a configuration change. n=16 is 10.5x the
  one-carrier app rate.

Version and packaging:

- `scripts/mkdkmsconf.sh --version` prints `0.0.1`; the authority is
  `tbtun/tbt_uapi.h` (`TBT_VERSION_MAJOR/MINOR/PATCH = 0/0/1`).
  `TBT_WIRE_VERSION=1`; `TBT_CONV=0x0005`.
- `make dkms.conf` generates `PACKAGE_VERSION="0.0.1"`. `make dkms-tarball`
  produces `dkms.tar.gz` with 23 entries under `./dkms_source_tree/`:
  `dkms.conf`, `Makefile`, `tbtun/*.c`, `tbtun/*.h` and `scripts/`.
- `scripts/install_dkms.sh --dry-run` exits 0 and prints the
  `dkms add/build/install` sequence for `tbtun-0.0.1`. `--stage-only` with a
  writable `DKMS_ROOT` writes the staged tree and a `dkms.conf` whose
  `PACKAGE_VERSION` is `0.0.1`.

DKMS: the full install/load/unload flow was verified in a QEMU guest on the
clean distribution kernel `7.2.9-1-MANJARO` (KVM, real `dkms 3.4.4`, the
`linux72` headers and module tree). `scripts/install_dkms.sh` staged, built and
installed `tbtun/0.0.1`; `modprobe tbtun` published `/proc/net/tbtun`; a tunnel
line was written and read back canonically; `rmmod tbtun` unloaded it; a second
`modprobe`/`rmmod` pair passed; and `--uninstall` removed every DKMS entry,
source tree and module file. 38 steps, 39 checks, 0 failures; console and
per-step logs are in `review/dkms-qemu/`.

The host was not used directly: `sudo` needs a password, `dkms` is not
installed, and there is no matching header tree under
`/lib/modules/$(uname -r)/build`. The earlier redirected-`/tmp` checks (the same
real `dkms 3.4.4`, `--sourcetree`/`--dkmstree`/`--installtree` under `/tmp`, and
the header payload from `docs/build-env.md`) remain historical support:

- `make dkms-tarball` on the final tree exits 0 (`dkms.tar.gz` sha256
  `4aa0b69a…`). Extracting it and running the `dkms.conf` make command in
  `/tmp` rebuilds a module that reports `version: 0.0.1` and
  `vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload`; its sha256 differs from
  the in-tree build because the build-id/BTF paths differ.
- In the `/tmp` tree, `dkms add`, `dkms build -k 7.2.9-1-MANJARO` and `dkms
  install` exit 0; `dkms status` reports `tbtun/0.0.1, 7.2.9-1-MANJARO, x86_64:
  installed`. This is the `/tmp` install tree, not the real `/lib/modules`.
- `modinfo` on the built and on the installed module reports `version: 0.0.1`
  and `vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload`.
- `dkms ldtarball dkms.tar.gz` into a fresh tree, then `dkms build` and
  `dkms install`: exit 0, `installed`.
- `dkms remove -m tbtun -v 0.0.1 --all` (the step behind `--uninstall`) exits 0,
  deletes `updates/dkms/tbtun.ko` and leaves `dkms status` empty. The source tree
  stays under `--sourcetree`; `--uninstall` removes it with
  `rm -rf "$DKMS_ROOT"/tbtun-*`.
- No second kernel release is installed (`/lib/modules` holds only
  `7.2.9-1-MANJARO`), and the guest uses that same release, so the cross-kernel
  DKMS path was not exercised.
- **Not executed on the host:** the system install
  (`sudo scripts/install_dkms.sh`), `modprobe`, `rmmod` and `/proc/net/tbtun`
  were exercised in the guest, not on this host. `sudo -n true` fails with "a
  password is required"; `dkms` and the kernel headers are not installed, and
  `/lib/modules/7.2.9-1-MANJARO/build` does not exist, so `install_dkms.sh`
  would stop at its headers check even with root. `insmod` of the built module
  fails with `EPERM` (no `CAP_SYS_MODULE`). The guest run is the clean-kernel
  evidence; the host remains unverified.

### Acceptance

The release acceptance commands:

```sh
# build, host suites, sanitizer build and the static checks
make check
make host-test-san
bash review/lint.sh

# QEMU probes and the adversarial suite
tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900
TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build

# bench rows quoted above
benches/run.sh throughput --n 4 --mib 40
benches/run.sh throughput --n 1 --mib 40

# packaging
scripts/mkdkmsconf.sh --version          # 0.0.1
make dkms.conf
make dkms-tarball                        # dkms.tar.gz
scripts/install_dkms.sh --dry-run
```

The measured host, QEMU, adversarial, writer, bench and DKMS results are in
Verification. Raw evidence: `review/final/` (`git-sha.txt`, `module.sha256`,
`qemu-AK-summary.txt`, `adversarial-results.txt`), `review/final-qemu/` (run
logs and consoles), `review/final-bench/` (CSVs, consoles, `summary.md`) and
`review/dkms-qemu/` (DKMS console and per-step logs).
The code tree is `f1282df`; `af3b9ba` only adds the evidence directories.
The operator install and unload procedure was executed end to end in the clean
QEMU guest (see the DKMS block); it was not executed directly on this host:

```sh
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

`n=8`, `n=16` and the full bench matrix are the final regression in
`review/final-bench/`.
