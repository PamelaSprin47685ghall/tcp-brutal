# Changelog

All notable changes to tbtun are recorded here. The wire revision is `TBT_CONV`
and moves independently of the module version.

## [0.8.0] - 2026-11-11

### Added

- Constant-frame FEC cells at `S = 1024`. A short DATA record is zero-padded to
  the full slot (`payload_len = 1024`), and the real length 1..1024 travels in
  the header word at `+24` under `TBT_FEC_FF_PADDED (0x02)`. Every data-bearing
  DATA/PARITY record is 1072 bytes raw, or a 1079-byte http-kal chunk
  (`"430\r\n" + 48 + 1024 + "\r\n"`). The empty-block marker stays 48 bytes:
  it carries no share.
- Single-flow mux subframes (`tbtun/tbt_mux.h`, `tbtun/tbt_mux.c`): an 8-byte
  big-endian header with OPEN/DATA/FIN/RST/WIN, a stateless encoder, a
  zero-allocation streaming parser and a per-`stream_id` credit window. The
  module and a Linux userspace client compile the same C99 source.
- `tests/golden_v080/`: deterministic binary/JSON vectors for an independent
  F# 0.9.x client (HELLO v8, padded DATA/PARITY chunks, ChaCha8, CRC32C, mux
  subframes, binary and GF(16) SC-LDPC encode/decode), plus
  `review/spec-080-fsharp.md`.

### Changed

- `TBT_VERSION` is 0.8.0 and `TBT_CONV` is `0x000b`. `TBT_FEC_CONV` stays
  `0x100b` and HELLO `body[0]` stays 8 because the 48-byte FEC header layout is
  unchanged; the `TBT_CONV` bump refuses a 0.7.3 peer at its first HELLO.
- `TBT_FEC_PAYLOAD_MAX` / `TBT_FEC_S` fall to 1024, so `TBT_FEC_REC_MAX` is
  1072. The downlink SC-LDPC share is 1024 bytes and has 16 batches; the
  uplink GF(16) codec defaults to `block=64, L=8` (`C = n*8`).
- The FEC window and out-queue byte marks are scaled to the 1 KiB cell
  (16 MiB / 4 MiB), and the AArch64 hardware CRC32C path is enabled for Clang.
- Every DATA/PARITY block names its own `n`/`m` in the header word at `+30`,
  together with the group; a block's sequence is
  `block_seq0 + group * n + share_index` for DATA and `block_seq0` for
  PARITY, so a single session may carry blocks of different shapes.
- Continuous SC-LDPC rate control (`tbt_fec_adapt_sclc.c`): the controller
  works in Q16.16 shares and moves the uplink split without a CFG switch
  stall. Codecs are kept in a small LRU per TX/RX plane
  (`tbt_fec_codec_cache.h`), so a returning rate reuses its built graph.
- FIN and half-close fixes: a FIN's `header.seq` is the stream's packet end,
  TX freezes `last_real_end` before the block jump, and RX keeps
  `rx_fin_seq` at `U64_MAX` until the delivery frontier reaches `pkt_end`.
  `fec_fin_with_undelivered` counts a FIN that arrived with missing packets.
- Replay ring slots are reclaimed by their own block span, not by an
  approximate packet span, and the c9 counters expose the reclaim path
  (`tbt_fec_replay_reclaimable`).
- FEC credit is counted in DATA packets end to end: the ACK grants a packet
  count, `tbt_fec_credit_packets()` converts the byte window once, and the
  TX gate compares absolute packet frontiers.

### Fixed

- A userspace receiver holds its first NACK for one carrier round trip and
  its retry for another, instead of a fixed 30 ms and 20 ms. Its ACK now
  carries the measured carrier RTT instead of a 1 ms placeholder, so the
  sender's replay dedup window covers the same trip. On a 200-300 ms WAN the
  old hold named shares still in flight; field evidence: 342,721 late shares
  for 537 delivered blocks (2.5x the shares those blocks carry), and the TX
  flush then stalled behind the replay queue for 2.4 s at a time.

### Known limitations

- At `S=1024, block=64, L=8` the uplink `22/10` split needs more parity symbols
  than its ten shares carry; the codec and wire reject it. The highest
  buildable uplink split is `21/11` (65.6%). Restoring the 70% point needs
  `L=16` or a new uplink graph, not a validation-only change.

## [Unreleased]

### Added

- IPv6 endpoints. `listen=`, `peer=`, `target=` and `srcaddrs=` accept an IPv4
  literal or a bracketed IPv6 literal (`[2001:db8::1]:9000`), and one tunnel
  line may mix families. The listener, carrier dial and target dial create the
  socket in the configured address family; a carrier's `portblock` source port
  is bound with `sockaddr_in6`; the far-side anchor comparison folds an
  IPv4-mapped IPv6 source back to IPv4; the HTTP `Host` header uses
  `[v6]:port`. `/proc/net/tbtun/config` and the `tunnels`/`peers` views render
  IPv6 in RFC 5952 compressed form. IPv4 configuration and output are
  byte-for-byte unchanged. The `sk_prot` override now copies `tcpv6_prot` for
  an IPv6 carrier, so the socket is released through the cache it was
  allocated from.
- A userspace Linux client shares the FEC code with the module: the SC-LDPC
  engine in `host/tbt_client_engine.c` and the `host/tbt_client_main.c` entry
  point drive the near and far roles over the same wire, and
  `host/test_client_loopback.c`, `host/test_client_wire.c` and
  `host/test_client_rx.c` cover the echo path, the wire encoding and the RX
  credit accounting. `host-test` builds and runs them, and the sanitizer suite
  runs their `_san` variants.

### Changed

- `struct tbt_cfg` stores each endpoint as `struct tbt_addr` (family plus
  network-order bytes) instead of `__be32`. This structure is internal to the
  module and the host tests; the `/proc` grammar is the operator ABI and gains
  the bracketed IPv6 form. The unused `TBT_PARSE_EAFNOSUPPORT` error code is
  gone.
- The uplink FEC window rises from 4 MiB to 16 MiB: both directions now cap
  the credit at 64 x 256 x `S`, which is 16 MiB at `S=1024`.

### Fixed

- The userspace client admits a HELLO in the right order: the session nonce
  and the carrier index are checked before `hello_crypto()` commits the peer
  key and RX nonce, and an unset far adopts the nonce only after every check
  passes. One wrong-nonce connection used to leave a poisoned key and RX nonce
  behind, so every later HELLO from the real near was refused until the
  process restarted. The timerfd read now checks its result instead of
  discarding it with a `void` cast, which the target AArch64 GCC 15 build
  refuses under `-Werror=unused-result`. `host/test_client_rx` adds the
  wrong-nonce poison case, a directed CFG_ACK under `adaptive=off` with a live
  control law, eight old-split blocks held in the reorder window across a
  split change, and the nonce length, adoption and HELLO-offset rules.
- Far-side carrier buffers no longer leak. Every accepted connection is
  tracked in the engine's carrier list; `carrier_close()` detaches the slot,
  and the tick after the epoll batch frees every non-retained dead carrier.
  A handshake that never completes and a HELLO that replaces an occupied index
  both release the ~192 KiB object (`rbuf` + `wbuf`), while a near carrier
  stays retained for its reconnect. `host/test_client_rx` runs 64 failed
  handshakes and a slot replacement, and the sanitizer build reports no leak.
- `adaptive=off` decides "directed" from the CFG ACK/NACK/SWITCH_DONE/
  ROLLBACK mask instead of "any nonzero flag", so a periodic FBK that carries
  `P_VALID` cannot reopen the control law. `host/test_client_rx` pins it.
- A paused carrier is no longer declared dead before TCP's own recovery can
  finish. The backstop was `4 x stall_ns`, which is 4 s once the 1 s
  detection floor applies: inside the RTO ladder of one lost segment
  (0.2+0.4+0.8+1.6+3.2 = 6.2 s for five failed attempts) and a seventh of
  the 30 s TCP keepalive the design names as its first line of
  defence. On the far side, which cannot dial, every such call spent a
  near-side redial: a 256-carrier run logged 9 `stall-dead` losses in 305 s
  with 61,525 shares, all byte-exact. The bound is now
  `max(4 x stall_ns, 30 s)`, capped at 300 s, and the predicate lives in
  `tbt_carrier_policy.h` where `host/test_carrier_policy.c` compiles it.
- A single dead carrier no longer terminates a userspace client flow. A
  DATA/PARITY write that the mapped carrier refuses is a dropped share, not a
  flow-fatal error: the handoff is counted in `tx_carrier_dropped`, the copy
  is retried on another up carrier, and if no carrier takes it the replay
  ring and the peer's NACK own the repair. Control records retry across the
  up carriers and fall back to `pending[]` on any carrier fault, and a HELLO
  stays on its own carrier instead of being parked for another one. Local
  encode and replay-allocation failures still fail the flow.
- The ACK credit is now counted in shares (`groups*(n+m)` per block), not
  DATA packets; it supersedes the 0.8.0 packet-credit model. The cap is
  16384 shares (64 blocks x 256); the host formula is
  `(TBT_RX_SLOTS-1)*groups*(n+m)`. `fec_credit_unit_bad` counts only a grant
  below one whole block.
- A write that ends on a non-1024-byte boundary no longer loses its tail. The
  idle cut counted the buffered partial packet as a real share, so every
  grace pass closed an empty block, re-armed the 25 ms block deadline, and
  never reached the 100 ms tail grace; 8191 bytes delivered 7168 and 8193
  delivered 8192. `tbt_fec_cut_pass()` now derives the block state from real
  shares only, and `host/test_cell_tail` pins the cut for 1, 37, 1023, 1024,
  1025, 8191, 8192 and 8193 bytes on both the idle cut and a half-close.
- The peripheral BBR hold saves and restores `snd_cwnd` and `sk_pacing_rate`
  again. After 200 ms of carrier idleness the 50 ms poll snapshots both fields,
  bounded by `snd_wnd/mss`, `snd_cwnd_clamp`, `sk_sndbuf/mss` and
  `sk_max_pacing_rate`, and writes them back while the session stays idle and
  TCP has nothing in flight. A non-PAD write cancels the hold before it queues
  bytes; a PAD in flight only skips that poll. 0.7.0 removed the restore, so
  the first burst after an idle period started from BBR's collapsed window.
  `host/test_hold` pins the save bounds, the apply gate and the save/restore
  loop.

## [0.7.3] - 2026-11-11

### Fixed

- SC-LDPC adaptive rate is enabled by default when `fec=on` and the configured
  split is on the shipped rate ladder (`128/128`, `154/102`, `178/78`,
  `8/24`, ...); `adaptive=off` still wins. Previously mode=3 defaulted off.
- The SC-LDPC estimator no longer counts DATA shares recovered by parity at
  the 0 ms early decode as network loss. A late DATA share cancels one such
  in-flight credit (`rx->late_data_credit`), so a real ~9% loss no longer
  reads 36-42% and parks the controller in the 50% dead zone.
- A shape switch increments `c13 switches`; an RX-idle side no longer
  overwrites the live TX view, and the TX tick publishes the peer FBK shape
  and p into `sess->adapt`.
- `est_sclc` is reinitialised on INIT, switch apply/rollback and stream reset;
  the late-data credit is cleared with the block arena.

## [0.7.2] - 2026-11-10

### Fixed

- `tbt_car_cc_actual()` tested the address of `icsk_ca_ops->name`, a
  `char[TCP_CA_NAME_MAX]` that is never NULL; an empty name was reported as
  a valid algorithm. It now tests `name[0] == '\0'` and the module builds
  without that warning.
- Carrier subflows get their 0.8 MiB per-direction buffer back and the
  `SOCK_SNDBUF_LOCK`/`SOCK_RCVBUF_LOCK` bits stay clear. 0.7.1 split one
  64 MiB session budget over 256 carriers (128 KiB per direction) and locked
  both fields, so TCP/BBR could not grow a subflow to its BDP. At 400 ms RTT
  one subflow at 0.5 MB/s needs 200 KiB of payload plus skb truesize; with
  the locks set the WAN 1 GiB run fell from 95.22 s (11.28 MB/s) to 350.12 s
  (3.07 MB/s). `tbt_car_sockbuf_of()` now returns 838,861 B for every
  subflow, and `tbt_car_setsockbuf()` clears both locks.

- A stream reset no longer frees an RX block plane while the decoder reads it.
  `tbt_fec_rx_shrink()` skips `b->decoding` blocks; `tbt_fec_rx_decode_one()`
  clears `decoding` under the lock after the decode and frees its own plane
  when `rx->stopping` or `!b->used`; slot placement refuses a decoding slot.
  The TX replay shrink was already under `tx->replay_lock` for every
  `blk_rec` access.

### Changed

- The ChaCha8 bulk path uses four-block interleaved SSE2 on x86_64, entered
  through `kernel_fpu_begin()`/`kernel_fpu_end()`, for 2.1x the previous
  `-mno-sse` throughput (370 -> 774 MB/s per 8 KiB share on the x86_64 test
  host). A tail shorter than four blocks stays on the portable scalar body;
  non-x86 targets are unchanged.
- `tbt_fec_sclc.o` is built at `-O3` like the cipher object. The SC-LDPC
  encode/decode loops are the largest TX/RX cost after the cipher.
- `seg_gather()` uses a 64-bit SWAR movemask: `(x & 0x8080...) *
  0x0002040810204081 >> 56` folds the eight row bytes into one plane byte.
  Measured 213 -> 175 ns per 64-byte segment on the x86_64 test host (18%);
  the gather runs once per share per batch in both encode and decode.
  `seg_scatter()` keeps the byte-extract plus transpose8 form: the SWAR
  mirror measured equal, so the simpler existing code stays.
- The FEC sliding window grows to 64 super-blocks. `TBT_FEC_CREDIT_MAX` is
  8192 packets (64 MiB of source at S=8192), `TBT_FEC_REPLAY_K_MAX` is 66
  (64 blocks of credit plus two of slack), and both FEC windows are 64 MiB.
  At 400 ms RTT the old 2048-packet/16-block cap held the protocol to
  35 MiB/s; 64 blocks cover the 51.2 MB BDP of 128 MB/s. The parent loopback
  socket asks for 8 MiB (16 MiB after doubling; the BDP is on the carriers),
  and the packetizer's
  unencoded capture FIFO is capped at 16 MiB; the 64-block flight window
  lives in the lazy replay planes, not in that FIFO. `rx_reorder_bytes` and
  `tx_window_bytes` now default to 64 MiB; a line that lowers the reorder
  budget without naming a sender window clamps the sender window to it.
- The TX replay ring is block-granular. It keeps K=66 descriptors but only
  allocates a block's record plane when its first record is stored; an ACK
  that passes the block returns the plane to an 8-block hot pool (4 below
  the 220 MiB memory floor) or frees it. The static ring is about 17 MiB
  rather than the 139 MiB an eager 66-block plane would need; active
  unacknowledged blocks are allocated only while they are in flight.
- The RX arena has 66 logical slots but no eager block planes. A slot takes
  its `(n+m)*S` buffer on the first valid share and returns it when the
  super-block is delivered or the slot is reused; the hot pool holds at most
  two blocks (4 MiB at 2 MiB/block), and memory pressure stops new
  buffers only below the hard floor. A receive-light side commits only its
  live reassembly blocks.
- FEC dispatch uses two passes instead of hard-rejecting on notsent. Pass 1
  prefers a carrier with socket space and `write_seq - snd_nxt <= 64 KiB`;
  if none exists, the first carrier with socket space is used anyway, with
  no notsent test. `notsent_lowat` stays at the kernel default. This keeps
  BBR from going app-limited (the 32/256 KiB hard gates measured 171/100 s
  against 90.9 s for no gate) while still preferring an empty-queue subflow
  when a stream switches.
- The session output FIFO keeps 4096 queued records (8192 pointer slots)
  and its byte mark rises to 64 MiB for the 64-block window; records stay
  per-share allocations, not a static byte array. A slow carrier is evicted
  after 15 s of frozen `snd_una` (was 8 s), so 310–400 ms RTT jitter does
  not start a replacement storm.
- A FEC stream switch no longer clears `c->up` on the persistent carriers.
  The legacy ARQ OPEN completion owned that flag; in FEC mode clearing it
  stalled all 256 carriers and the next stream sat at 0 B until timeout.
- The memory floor no longer blocks a new stream at 0 B. Below 180 MiB the
  packetizer first frees hot/ACKed planes but keeps every unacknowledged
  block; it opens no new block while eight are unacked and waits 5 ms for the
  ACK frontier, and pauses only below 80 MiB with two or more active blocks.
  Evicting the frontier block would turn its NACK into -ESTALE and kill the
  stream after 30 s, so shrink never touches `block_start >= ack_floor`. A new stream is refused below
  64 MiB. RX and replay resets keep at most two hot planes and free the
  rest immediately; flow abort/detach calls both resets before the delayed
  flow_put, and clears stale `req_block`/NACK state for the next sid.
- Stream abort/detach retires the old sid's queued DATA/replay records before
  the final flow put, so a FIFO record can no longer hold a flow reference
  and strand its 16 MiB TX ring (three 16 MiB orphans after three streams).
  `host/test_fec.c` now asserts the replay allocator returns to its baseline
  after create -> ensure/store -> reset -> free.

### Tests

- `host/test_carrier_policy.c` asserts 838,861 B for every carrier count
  and that both buffer locks are clear.
- `host/test_fec.c`, `host/test_fec_rx.c` and `host/test_mem_budget.c`
  assert K=66, the 8192-packet credit, the block-granular 20 MiB replay
  ledger and the 66-slot lazy RX arena.

- A local application close (`SEND_SHUTDOWN` or `TCP_CLOSE` while
  `rx_fin_done` is false) now sends the tunnel RST immediately instead of
  waiting for `rx_queued` to become non-empty. The old stream no longer
  survives for seconds and collides with the next one after a curl timeout.

- Late DATA settlement is bounded and attributed: a 16-record ledger keyed
  by current epoch, block id, group and share deduplicates repeats. At fold
  it records only the exact misses that `est_block` counts (alive/limit,
  not `codec_fail`); pending bits clear only those bits, and a post-fold
  share corrects only its own bit when the 500 ms bucket generation still
  matches. p is recomputed from the window sent/miss totals. Old-epoch,
  evicted or unattributable tails increment `fec_late_unknown` and never
  credit. The fixed 65,000 ppm p offset and 7,800 ppm EWMA subtraction are
  removed. `p_ewma` is a 64-super sent/miss ring with an anchor; correcting
  a committed super replays the ring, so `p_win` and `p_ewma` both reflect
  the corrected history. All estimator mutation and `alive_bm` updates are
  serialized by `rx->lock`. The ledger lives in `tbt_fec_late.h`; host tests
  cover six deterministic cases (in-flight cancellation, duplicate, block
  isolation, old epoch, eviction, share 255) plus exact EWMA replay.

### Verification

- On the 7.3.0-rc6 host at 400 ms RTT (200 ms netem on lo), three 20 s
  streams measured 5.770, 5.770 and 5.793 MB/s: the second and third did not
  decay behind the first stream's unsent tail. `del 1`, `del 2` and `rmmod`
  then left 0 tbt vmalloc entries.
- On the 7.3.0-rc6 host, five 8 MB/s streams aborted after 2 s each left
  `/proc/vmallocinfo` tbt entries at 2 / 139,264 B after every stream (no
  16 MiB TX-ring accumulation); `del 1`, `del 2` and `rmmod tbtun` left
  0 tbt entries and 0 `0xffffffffc0` orphan blocks.

### Compatibility

- Wire unchanged: `TBT_CONV` 0x000a, `TBT_FEC_CONV` 0x100a, HELLO v7.
  0.7.1 and 0.7.2 peers interoperate; the replay and arena changes are
  local memory policies.

## [0.7.1] - 2026-11-10

### Fixed

- An unrecoverable hole or a vanished peer now reaches the application as a
  reset, not a FIN. The kill path called `kernel_sock_shutdown(SHUT_RDWR)`,
  which sends a real FIN before the `SO_LINGER=0` close sends its RST; the
  1 GiB run whose far host disappeared at 522,190,848 B ended as `curl
  rc=18` (clean EOF), so the application could not tell a truncated stream
  from a complete one. The wake now uses `SHUT_RD` plus `ECONNRESET`.
  `stream_reset_user` and `stream_reset_tcp_failed` now count the FEC reset
  path that they documented but never incremented.
- `cc_bbr_ok`/`cc_bbr_fail` count the congestion control the carrier
  actually runs, read from `inet_csk(sk)->icsk_ca_ops->name` after the
  request. A kernel that refuses `TCP_CONGESTION=bbr` on a socket that
  already uses BBR (the 6.1.172 near host returns EINVAL while
  `tcp_congestion_control` is bbr) is no longer counted as a failure. The
  refusal and the actual algorithm are logged instead.
- Diagnostics name the stream close edge: FIN requests carry their caller
  (`pub`, `starved`, `closecheck`), and the public socket's state change,
  ctrl error/peer EOF, `shutdown_wr`, peer FIN, `maybe_detach` and
  `stream_kill` paths print their stream state. Every carrier retirement
  prints its reason, and one unconditional per-generation stall snapshot
  (`una/nxt/wnd/wmem/packets_out/retrans/srtt/mdev`) records why a carrier
  stopped being acknowledged.
- Probe N's 1x256 grouped 16 MiB run no longer reports `fec_short_tx` or
  `fec_credit_wait`. A block sealed by the 25 ms idle timer used to flush the
  partial packet in front of it as a short DATA share; that share forced one
  extra share, an unaligned final tail, and one extra super-block of credit,
  so a 16 MiB transfer (exactly the 16-block window) stalled until the ACK
  frontier caught up. The partial packet now starts the next block, with a
  100 ms seam deadline after a mid-write cut, so it coalesces into a full
  share. The per-record FEC traces move to `debug=3`; at `debug=2` the nine
  lines per share slowed the guest enough that the 25 ms timer fired on its
  own and turned a healthy transfer into a cut-block storm.
- The 1 GiB downlink no longer collapses after its first minute. On the
  300 ms WAN the 10 s samples showed repair traffic rising from 47 MB to
  270 MB per two minutes while payload fell from 3.1 MB/s to 0.65-1.0 MB/s.
  Replay K rises from 7 to 31 (64 MiB ring, 64 MiB RX arena, 16 MiB credit),
  so a frontier repairing behind a full in-flight window never falls outside
  the NACK window.
- A frontier block that survives several NACKs now doubles its retry interval
  up to 2 s, and the per-share hold follows that backoff. A duplicate replay
  no longer queues ahead of DATA on every repair round.
- A closed credit window no longer ages the block-cut clock. Reopening credit
  cannot seal a short block that was only waiting for the next round; the
  new `fec_short_blocks` counter separates warm-up cuts from a steady leak.
- A carrier that the FEC drain kept marking slow with `snd_una` frozen for
  8 s is retired and redialled, at most once per second. It no longer holds
  the credit frontier behind a path that is not draining.
- `c3` exports `cc_bbr_ok` / `cc_bbr_fail` and `slow_evicted`, so a field run
  can prove every carrier accepted `TCP_CONGESTION=bbr`.
- `scripts/qa/stats-sampler-070.sh` keeps c3/c6/c9/c10/c11/c12 on disk once
  per interval for long-run attribution.
- A 256-carrier bring-up no longer overflows the pinned control queue. At
  `TBT_CTL_QUEUE_MAX=256` the second half of a simultaneous HELLO/OPEN wave
  was refused with `-ENOSPC`, the carrier waited out its 5 s join deadline
  and the peer redialled it, so one overflow became a replacement storm that
  doubled the socket-buffer footprint on the 955 MB far host. The queue is
  now 1024 frames, and the poll requeues a HELLO that was never enqueued
  (paced at 100 ms while the queue has room) instead of waiting for the
  deadline. Field evidence: 622,081 `ctl queue full` diagnostics in two days,
  a global OOM at 07:02, and a full-system freeze at 07:21 where both
  `/var/log/syslog` and `/var/log/kern.log` stop together.
- Stall, recovery and per-generation stall-snapshot lines are silent at the
  production default and at `debug=1`; they print only at `debug>=2`, still
  rate limited. The per-frame `ctl queue full` warning moves to `debug>=2`.
  At 256 carriers the stall/recover pair fired thousands of times per hour
  even though 99% of the pauses recovered inside 200 ms, and the lines were
  the last kernel output before the freeze.
- The session poll no longer calls `tbt_car_poll_one()` under `sess->lock`
  for provisional carriers. That branch prints at `debug>=1` and can
  allocate; a printk inside the session spinlock stops every other CPU on
  the same lock while the printk ring buffer or console makes progress. The
  provisional list now takes a carrier reference, drops the lock, and polls
  like the joined slots. The peer-pause stream-hash scan also runs once per
  poll pass instead of once per carrier.
- `tbt_stream_kill()` prints only the kill winner. A RST is queued once per
  carrier, and the losing calls arrive up to 255 times for one stream; the
  diagnostic stood before the `TBT_STREAM_F_DYING` test, so closing one
  application turned into a 256-line burst on both ends. The byte counters
  are unchanged by the move; only the duplicate lines are gone.
- Carrier socket buffers come from one 64 MiB session budget and keep the
  SNDBUF/RCVBUF userlocks. 0.7.0 cleared the locks so autotuning could grow
  every socket; a 256-carrier session could then reach several GiB of socket
  memory, and the 955 MB far host froze in direct reclaim during a 1 GiB
  transfer (07:02 global OOM, 07:21 both logs stop together). A 256-carrier
  session now holds at most 128 KiB per direction per carrier, while a
  narrow stripe keeps the 0.8 MiB floor. The shared rule is
  `tbt_car_sockbuf_of()` in `tbt_carrier_policy.h`, covered by
  `host/test_carrier_policy.c`.
- The FEC plane fits in 256 MiB. Replay `K` falls from 31 to 16
  (`TBT_FEC_REPLAY_BUDGET` 33 MiB, ~32.2 MiB used), the RX arena from 64 to
  32 MiB (`TBT_FEC_RX_BUDGET`), and the output FIFO from 4096 to 2048
  slots; credit follows K-2 blocks, so the 16-round downlink / 8-round
  uplink parent window stays the cap. `tbt_mem_avail_kb()` now guards the
  data plane: below 180 MiB the packetizer opens no new block and re-arms in
  2 s; below 100 MiB a new stream is refused. This is the memory that
  actually froze the 955 MB far host: at 09:59 on 2026-11-10 journald
  reported memory pressure right after the last tbtun line, and the kernel
  died in `skb_page_frag_refill` (reported `python3 invoked oom-killer`).
  The acceptance source is now a static C `sendfile()` binary with no
  application buffer.

## [0.7.0] - 2026-11-10

0.7.0 removes the 0.6.0 kernel stream multiplexer and the shared FEC plane
that carried it. One session carries one FEC flow again, and the DATA/PARITY
payload is the application byte stream itself. Carrier transport is now
BBR-owned and carrier-count independent, and the parent stream and its FEC
window are sized for the whole tunnel.

### Removed

- The SC-LDPC stream mux: the 0.6.0 multiplexer module and its header, the
  32-byte subframes tagged `[sid, offset, length, type]`, the per-stream
  receive windows and capture rings, and the per-stream receive-window record
  (0x37). The shared plane admission ledger and the subframe FIN/RST path go
  with it.
- The `FEC_ALLOC` record (0x36), its multi-stream group rebalancing, and the
  `tbt_fec_alloc_*` control path behind it. Record types are only 0x30..0x35
  again.

### Changed

- Single-stream 8 KiB cut-through. One DATA share leaves per full 8 KiB
  write and is sent as that write completes, not at a block boundary.
  `TBT_FEC_SHORT_MS` (2) applies only to the interactive tail. A 1x256 block
  is 128 DATA shares followed by its parity shares.
- Wire v10. `TBT_CONV` moves to `0x000a`, `TBT_FEC_CONV` to `0x100a`, and
  FEC HELLO `body[0]` to version 7; the three constants move together. The
  48-byte FEC header and every offset stay unchanged. A 0.6.0 peer is refused
  at the first HELLO and the first FEC conv check; there is no compatibility
  branch.
- FEC receive decodes on the share that can cover the block, not after a
  50 ms quiet window. The downlink 1x256 block wakes on complete DATA or at
  present shares >= n + 8 (missing <= 120 < m); the uplink 8x32 group wakes
  at present shares >= n + 2. A failed attempt keeps its 50..400 ms backoff,
  and a fresh share clears it; a share for a block behind the delivery
  frontier is dropped as `fec_late_share` before decryption or placement.
- Lock-free FEC dispatch. A DATA record is no longer bound to carrier i's
  private queue: it waits in one session FIFO and the drain sends it on the
  first carrier from an atomic rotation cursor whose send space holds it. A
  full socket costs the scan one skipped index, not the record a place
  behind it, so `fec_hol_wait` and `fec_flush_skip` stay at 0. When every
  socket is full the head stays in the FIFO and the work retries in 2 ms. A
  carrier without room for `TBT_SKIP_MS` (20 ms) is marked slow and tried
  last; room again clears the mark. The receiver is unchanged: it places a
  share by its header, not by arrival order.
- The parent stream socket (public accept and far target) asks for an 8 MiB
  `SO_SNDBUF`/`SO_RCVBUF` through `SO_SNDBUFFORCE`/`SO_RCVBUFFORCE`, so the
  kernel holds 16 MiB per direction; the near listener reserves the same
  buffer before the handshake, and an unprivileged context falls back to the
  capped `SO_*BUF`. The parent receive pause mark rises from 2 to 8 MiB.
- The parent FEC window is eight rounds: 8 MiB downlink (eight 1 MiB blocks)
  and 4 MiB uplink, not 256 carriers x 1 MiB. `TBT_FEC_CREDIT_MAX` stays at
  1024 packets (8 MiB at S=8192) downlink; the uplink credit is capped at
  512 packets in the same byte unit.
- `TBT_FEC_TX_RING_MAX` rises from 1 to 16 MiB. The ring is allocated with
  `tbt_fec_vzalloc()` (`vzalloc`/`kvzalloc`) and released with `kvfree`, so
  the ceiling never asks for a high-order page.
- Carrier transport is BBR-owned and carrier-count independent. Every
  carrier socket gets a direct `sk_sndbuf`/`sk_rcvbuf` floor of 838,861 B
  (0.8 MiB) with `SOCK_SNDBUF_LOCK`/`SOCK_RCVBUF_LOCK` cleared, so TCP
  autotuning may grow both buffers. The per-`n_pri` split, the 128 MiB
  `TBT_CAR_SOCKBUF_TOTAL` budget and the `SO_*BUFFORCE` fallback are gone.
  Each dialled and adopted carrier asks for `TCP_CONGESTION=bbr` explicitly
  and keeps the system default only when the kernel refuses it.
- The peripheral idle hold no longer snapshots or restores `snd_cwnd` and
  `sk_pacing_rate`; BBR keeps ownership. While FEC data sits in the out
  queues no carrier is marked held and no PAD is queued.
- A FEC carrier is paused only after `snd_una` has been frozen for at least
  `TBT_STALL_MS` (1 s) with `packets_out > 0`, so a BBR probe cycle no
  longer looks like a stall.
- Module version is `0.7.0`.

### Fixed

- A sustained 8 KiB cut-through stream no longer seals a one-share block per
  write. The 2 ms timer now flushes a partial packet only on the stream's
  first packet; a block that holds real shares waits `TBT_FEC_BLOCK_IDLE_MS`
  (25 ms) without new bytes, and FIN still closes it at once. The idle cut
  lives on its own `TBT_SW_FEC_CUT` work so its timer can never hold the next
  write's 0 ms submit wake behind it. A sealed short block sends
  `min(m, win_len)` parity rows instead of row 0 alone; a full block keeps
  all m rows.
- The FEC DATA outq counters no longer underflow when a replay record is
  freed. `tbt_fec_out_free()` decremented the DATA counters for replay
  objects that never entered the DATA FIFO; the unsigned high-water test
  then read the negative value as a huge positive and stopped the packetizer
  (0 ms 64-carrier run: block 6, outq=-2/-16480, full TX ring). The release
  path now charges each record to its own queue, and the high-water and hard
  cap comparisons are signed.
- A sequential SC-LDPC stream now completes its FEC INIT/CFG handshake.
  Every carrier that receives the same OPEN resolves the one flow, but only
  the carrier that creates the stream may attach it; a carrier that lost
  that race now drops its reference instead of unlinking the plane the
  winner is about to use. The removed shared-plane admission ledger made
  abort the loser's duty, so the 0.7.0 second stream's flow was unlinked
  before its INIT arrived: the peer dropped the INIT as an unknown sid, sent
  no CFG ACK, and the stream stalled at 0 bytes. The mux remnants
  `tbt_fec_flow_claim_slot()` and `tbt_fec_flow_shared()` are gone.
- FEC credit is counted in whole super-blocks, not packet sequence numbers.
  A block closed before it filled advances `seq` to the next block boundary
  without sending the skipped slots, so the packet gate spent a full block
  of credit for a one-share block and could close in the middle of the next
  block. The field run cut a one-share 2 KB block per ACK and stalled for
  118 s at 383 MB (`fec super 380 stuck: ... nreal=1 short=2048`,
  `fec_replay_miss_never_stored` +84, gap 28.81 s). The window now matches
  what the receiver buffers (K-2 super-blocks, `slots` blocks) and the
  replay ring holds, and the block id does not change while a block fills,
  so a started block can always be finished.
- The FEC packetizer resumes capture at half the source ring instead of only
  when it is completely empty. The parent socket's receive window is what
  the application sees; holding it shut until the last byte drained let the
  far loopback reach `rwnd_limited` with BBR at 22 kbps, after which the
  application delivered 2 KB per RTO and every block was cut at one share.
  `tbt_sock.c` already documented the half contract; the pump now honours it.
- A paused capture no longer drops the socket's data-ready wake. The pump's
  resume was the only path back, and it can be missed while the ring never
  falls to its resume mark, so the application's window stayed shut and the
  stream froze for 200 s. The read attempt is
  bounded: the actor returns 0 when the ring takes nothing and
  `tcp_read_sock()` stops on that return.
- A NACK probe for a super-block this side has not built yet is counted as
  `fec_nack_req_invalid`, not as `fec_replay_miss_never_stored`. The receiver
  at the packet frontier probes the next block while the application is
  stalled; the block does not exist yet and will when bytes arrive. Counting
  it as a storage miss made a 200 s application stall look like a replay-ring
  defect (+124 on the 1 GiB rerun); the probe gets no answer and retries.
- A NACK replay starts on the usable carrier with the smallest TCP send
  backlog, not on the first empty per-pass counter. The repair is appended to
  that socket's FIFO, so a carrier holding megabytes of DATA delays it even
  though `fec_replay_queued` reads 0. The field run measured
  `fec_nack_repair_max_ns` 120.8 s with an empty replay queue; the same
  frontier block then waited on carrier TCP retransmission.

### Tests

- `host/test_fec_sclc_data` pins `min(m, win_len)` parity for short blocks
  and recovers a lost GF(16) share from those rows; `host/test_fec_rx` pins
  the short-block reorder-window and late-share rules; `host/test_fec_out`
  asserts that freeing a replay record leaves the DATA outq unchanged.
- `host/test_carrier_policy` asserts the 0.8 MiB floor, the cleared buffer
  userlocks and the BBR request path (including `-ENOENT` tolerance);
  `host/test_hold` follows the snapshot-free hold state machine.
- `host/test_fec_switch` models the first flow's detach, the second OPEN's
  stream-creation race and its INIT/CFG ACK: the losing carrier must keep
  the winner's plane.
- `host/test_fec_rx` pins the block credit window: a short cut spends one
  block, five blocks may be open, a started block admits every share, and a
  stale frontier is clamped instead of wrapping.

## [0.6.0] - 2026-11-09

0.6.0 is the merge cut. Two parallel 0.5.0 lines had each taken the same
`TBT_CONV`, `TBT_FEC_CONV` and HELLO version with different meanings; this
release makes their union one wire revision. A 0.5.0 peer is refused at the
first HELLO (version 6), the first FEC conv check (a 0.5.0 record carries
`0x1008`) and the first OPEN length check, so both ends must be upgraded
together; there is no compatibility branch.

### Added

- SC-LDPC single-plane stream multiplexing. One 1x256 downlink plane (and
  the 8x32 GF(16) uplink) carries up to 16 concurrent streams as 32-byte
  subframes tagged `[sid, offset, length, type]` from the subframe layer,
  with per-stream receive windows, FIN and RST. The per-stream
  receive-window record (0x37) reports one stream's receive window in a
  16-byte body named by the record header's `sid`. The shared admission
  ledger counts claims, aborts, attaches and detaches and keeps its pending
  admission slots and attached legs balanced, so a losing carrier cannot
  tear down a plane that its winner still needs.
- The 36-byte OPEN with `route_slot` and `target_port`, the 64-byte
  `FEC_CFG`/`FEC_FBK` bodies, and the GF(16) bit-plane64 share mapping are
  all retained under the merged revision; every length and offset from the
  v8 cut is the v9 contract.
- Multi-port mapping rides the same session: one `tunnel` line serves its
  `listen=`/`target=` pair plus every `map=listen:target` slot, and the far
  side authorizes only a slot/port pair from its own table.
- The v0.4.4 carrier-policy, bounded-peeling and FEC_ALLOC validation
  fixes are folded in, including `tbt_carrier_policy.h`, the three
  carrier decisions that `host/test_carrier_policy` pins, and the bounded
  ALLOC state exits.

### Changed

- Wire v9. `TBT_CONV` moves to `0x0009`, `TBT_FEC_CONV` to `0x1009`, and
  FEC HELLO `body[0]` to version 6; all three constants move together.
  `TBT_OPEN_BODY_LEN` is 36, `TBT_FEC_CFG_LEN` and `TBT_FEC_FBK_LEN` are 64,
  the per-stream receive-window record is 0x37, and the SC-LDPC DATA/PARITY
  payload is the 32-byte subframe stream. The 48-byte FEC header and every
  offset stay unchanged.
- Module version is `0.6.0`.

### Verification

- `scripts/qemu-e2e-smoke.sh` boots 7.0.14 under KVM with lockdep, UBSAN and
  DEBUG_ATOMIC_SLEEP and drives the merged data plane over
  `profile=http-kal`: three sequential streams, then six concurrent streams
  on the one 1x256 plane (four bulk streams moving 6 MiB per direction,
  one 50 KiB stream closing through a forced tail receive-window record, one
  mid-stream RST), the multiport mapping and GF(16) uplink stages, a grouped
  pair, and `del` + `delete_module` with a clean kernel log.
- `host/test_fec` covers the 36-byte OPEN, the 64-byte CFG/FBK bodies and
  the receive-window record; the mux host test covers the ring lifecycle and
  the admission ledger under concurrent claim/attach/abort.

## [0.5.0] - unreleased

### Added

- Multi-port mapping over one N+M carrier session. A `tunnel` line keeps its
  original `listen=`/`target=` pair as slot 0 and adds `map=listen:target`
  entries (comma-separated, up to `TBT_MAP_MAX - 1` extra slots) for the
  others. A near tunnel binds every map listen port on `listen=`'s address and
  accepts one stream per local port; a far tunnel ignores the listen half and
  uses the map target ports as its authorized destination set. One session per
  tunnel therefore serves every port instead of one 5-carrier (default) session
  per port.
- Far-side target authorization. The far tunnel accepts an OPEN only when the
  slot exists in its own mapping table and the wire port matches that slot
  (see OPEN under Changed). An unauthorized slot or port is dropped and
  logged, never dialed and never allowed to close the carrier.
  `tbtun/tbt_map.{c,h}` holds the pure slot-to-port resolver
  `tbt_map_slot_target()`, and `host/test_map.c` pins it.
- `FEC_ALLOC` keeps its 64-byte body; the flags mask widens to `0x0f` so
  `COMMITTED` (0x04) and `ABORT` (0x08) join `ACK`/`DONE`, and the reserved
  bytes are validated.
- SC-LDPC single-plane stream multiplexing: one SC-LDPC FEC plane carries
  32-byte subframes tagged by stream id, with per-stream receive windows,
  FIN and RST.
- The per-stream receive-window record (0x37) carries one stream's receive
  window: delivered byte offset, credit, and a reserved word. The 16-byte
  body names the target stream in the record header's `sid`.
- `host/test_fec` covers the receive-window body and its header rules.
- `scripts/qemu-e2e-smoke.sh` boots 7.0.14 under KVM with lockdep, UBSAN and
  DEBUG_ATOMIC_SLEEP and drives the shipped 1x256 downlink / 8x32 uplink pair
  over `profile=http-kal`: three sequential streams, then six concurrent
  streams on the one plane (four bulk streams moving 6 MiB per direction,
  one 50 KiB stream closing through a forced tail receive-window record, one
  mid-stream RST), a grouped pair, and `del` + `delete_module` with a clean
  kernel log.

### Changed

- Wire v8 base. `TBT_CONV` moves to `0x0008`, `TBT_FEC_CONV` to `0x1008`,
  and FEC HELLO `body[0]` to version 5. A 0.4.0 peer is refused at the first
  HELLO, the first FEC conv check and the first OPEN length check; there is
  no compatibility branch. The 32-byte main header and the 48-byte FEC
  header keep every offset; the DATA/PARITY payload interpretation and the
  new receive-window record are the changes.
- OPEN carries 36 bytes: the 32-byte physical group bitmap, then `route_slot`
  and `target_port` (both little-endian). The near side always sends its own
  mapping slot and that slot's target port; the port is 0 only when the slot
  has no `target=` configured. The far side resolves the slot in its own
  `target=`/`map=` table and requires the wire port to equal that slot's port
  (0 means "the configured one"). Validation order is mask, route slot,
  target port.
- `FEC_CFG` grows to 64 bytes. The v7 48-byte prefix is unchanged; the
  extension names `codec_variant`, `L`, `block` and `rate_ppm`, protected by
  a second CRC over `+48..+55`, with `reserved2` at `+60`.
- `FEC_FBK` grows to 64 bytes and the full 256-bit `carrier_bitmap[32]` is
  restored (the 0.4.0 SC overlay truncated it to 224 carriers). `p_fast_ppm`
  moves to `+48`; the extension also carries the receiver's variant/L/block
  and current/target rates. `codec_fail` has no v8 wire field and stays a
  local procfs diagnostic.
- A switch normalizes an RLNC shape's `codec_variant` from its group count
  (single 0, grouped 1) before storing it, and the FBK check normalizes the
  committed value again. A grouped flow therefore keeps accepting the peer's
  variant-1 reports after its first rate switch. The uplink GF(16)
  `block=128` is documented as a legal variant-2 combination.
- Canonical config renders `map=` once, comma-separated, after the role's
  peer/target keys. A repeated `map=` on one line replaces the earlier list.
  A near line refuses a map listen port equal to `listen=` or to another map;
  a far line accepts the same numbers because it does not bind them.
- Uplink GF(16) SC-IRA moves to the Wire v8 bit-plane share mapping
  (`TBT_SCLC_MAP_BITPLANE64`). The C codewords split into 64-lane batches,
  every codeword of a batch uses the same symbol->share map, and one symbol
  occupies four little-endian bit planes of lanes/8 bytes. A lost share
  therefore erases one identical variable set in all 64 lanes and one
  shared-control peel serves the batch. Per-share symbol counts are solved at
  build time so every data share still fills S bytes exactly; parity shares
  may keep a zero tail. The bytes inside a share change, so the legacy
  symbol-interleaved layout is not wire-compatible: both peers must take the
  Wire v8 cut.
- Module version is `0.5.0`.

### Performance

- Uplink GF(16) SC-IRA: the encoder keeps the 64-lane bit-sliced sweep
  (four uint64 planes per variable, 4x4 GF(16) multiply masks) and the
  gather/scatter become four plane loads and stores per symbol. The
  decoder runs one peel and one dense GF(16) elimination for all 64 lanes
  of a batch; scratch is preallocated with kvzalloc and no hot path
  allocates. Host, pinned core, p=0.30: rate 1/4 encode 43.5 -> 400 MB/s,
  decode 6.5 -> 373 MB/s; rate 1/2 encode 57 -> 622 MB/s, decode 16 ->
  666 MB/s. At p=0.13 rate 11/16 encodes 121 -> 540 MB/s and decodes
  32 -> 826 MB/s; at p=0.40 rate 3/8 decodes 7.8 -> 409 MB/s. The
  eight-group rate-1/4 footprint is 8.75 MiB.
- Paired BEC comparison (same block and share-loss mask, six rates,
  p=0.10..0.45): the bit-plane block recovery is never below the legacy
  mapping and is higher at the high-rate operating points (22/10 p=0.20
  0.69 -> 0.73; 18/14 p=0.30 0.68 -> 0.72; 16/16 p=0.35 0.77 -> 0.79).
  The codec now uses `sc_build`'s quantized parity count, so 18/14 and
  22/10 do not map the 28/17 padding slots that the graph never solves.
- Speedups over the legacy per-codeword mapping, derived from the pinned-core
  numbers above: decode 6.5 -> 373 MB/s at rate 1/4 (~57x) and 16 -> 666 MB/s
  at rate 1/2 (~42x); encode 43.5 -> 400 MB/s at rate 1/4 (~9x) and
  57 -> 622 MB/s at rate 1/2 (~11x). `make host-bench-sclc` /
  `host/bench_sclc_uplink` reproduce the pairs. A 300-block paired BEC sweep
  (same block, same share-loss mask, six rates x p=0.10..0.45) puts the
  bit-plane block recovery at or above the legacy mapping at every point,
  with the gains concentrated at high rate: 22/10 p=0.30 0.170 -> 0.217
  (+4.7 pp), 12/20 p=0.45 0.780 -> 0.823 (+4.3 pp), 9/23 p=0.45 0.937 ->
  0.973 (+3.7 pp). Reproduce with `make host-bench-recovery`.

### Fixed

- The SC-LDPC TX mux lock was never initialised, so the first use turned the
  locking validator off for the rest of the boot. A stream ring is now
  allocated and freed outside that spinlock; only the pointer moves under it.
- Duplicate OPENs leaked their admission reference, and a fresh flow did not
  count its own admission at all: a losing carrier could abort a plane whose
  winner had not attached yet. A carrier that found its flow by lookup also
  consumed a slot it did not own. Pending admission slots and attached legs
  are now tracked separately and both are balanced, and a lookup-attached
  stream claims its own slot (or is refused with `-ESTALE` if the plane is
  already dying), so the plane outlives every admission and attached leg.
- An unlink path that lost the race to another unlink no longer drops the
  table reference a second time, so abort/detach cannot underflow the flow's
  refcount.
- Binding the first OPEN compared the peer's TX bitmap with the local TX
  bitmap, so the shipped asymmetric 1x256 / 8x32 pair never came up.
- Detach unlinked the flow after it cleared `streams_active`, so a sequential
  successor could share a plane that was already leaving.
- Kill shut a socket down at the call site while a carrier RX actor already
  held another socket's lock, which lockdep reports as recursive
  `k-sk_lock-AF_INET` nesting; `w_kill` owns the shutdown now.
- The HTTP container did not dispatch the receive-window record (0x37), the
  plane-level ACK stopped updating with no single stream, and a sub-window
  tail never sent a final receive-window record; bulk multi-stream over
  `http-kal` stalled or left the sender's FIN gate closed.
  `tbt_fec_record_type()`, the plane frontier and a forced tail report cover
  all three.
- A carrier marked itself OPEN-ready when the pinned OPEN *failed* to queue
  (the success test was inverted), letting FEC data run before its OPEN. The
  flag now stays clear until the pump writes the OPEN; the direct FEC data
  path must not overtake the queued control frame.
- The mux FIN tail report held a borrowed plane pointer across an unlock;
  it now takes a referenced primary. The receiver's closed-sid table was dead
  state once feed stopped creating legs, and is gone.
- The mux host test covers the ring alloc/install/remove/free lifecycle, a
  four-thread install/remove/pack stress, and the admission ledger under
  concurrent claim/attach/abort (the kernel contract is one lock, as in the
  module).

### Verification

- Host vectors cover the 64-byte CFG/FBK bodies, the 36-byte OPEN, the v8
  CFG extension CRC and the explicit rejection of v7 (`0x1007`, 48-byte
  CFG/FBK, 32-byte OPEN) records.
- `make check`, `make host-test-san` and `./review/lint.sh` pass. The host
  tests cover parse/render round-trips, slot-to-target routing, unauthorized
  slot/port refusal, near listen conflict detection, the maximal-line limit,
  and both network-order port helpers.

## [0.4.0] - 2026-11-09

### Added

- Multi-stream FEC v7: each stream owns a per-flow FEC TX/RX context, so
  one session can carry concurrent streams. `TBT_FEC_CONV` moves to
  `0x1007` and `TBT_WIRE_VERSION_V6` to 4. Per-flow admission references,
  flow lifetime/teardown, a late far OPEN publisher's live flow, and a
  peer-FIN stream whose target never closes are handled; the single-stream
  path remains available.
- SC-LDPC adaptive rate control Phase 2 as host-testable pure logic
  (`tbtun/tbt_fec_adapt_sclc.{c,h}`). The estimator folds per-group
  256-bit miss masks into eight 500 ms buckets (4 s window) and a 1/8
  EWMA; `p_used = max(EWMA, window)` becomes valid after 8 super-blocks
  and 512 alive-filtered shares. The controller maps p to
  `r_t = clamp(1 - p - 0.20, 0.25, 0.70)`, quantizes down onto the codec
  ladder `{25,30,40,50,60,70}`, and derives `n = floor(r_actual * (n+m))`.
  It is off by default, never suggests a switch while off, and adds fast
  start, 2 pp hysteresis, two-sample down / six-sample up, 3 s / 10 s
  dwell, 5 s reverse cooldown, 10-in-60 s cap, 3-in-30 s hold, burst,
  residual fail-down, probe up/commit/revert/abort, post-commit rollback,
  and CFG-reject fallback.
- Phase 3 wiring: `adaptive=on` is accepted for `mode=sc-ldpc` when the
  initial shape is a shipped profile on the rate ladder. The receiver folds
  the real 256-bit miss bitmap into the SC estimator (eight 500 ms buckets,
  4 s window, 1/8 EWMA, 1 s p_fast, codec-failure mask). FEC_FBK carries
  p_used and the switch handshake; for SC adaptive, the last four bytes of
  the carrier bitmap carry p_fast and codec_fail in 16 ppm units, so the
  48-byte body and every offset stay unchanged. The sender evaluates the
  controller every 250 ms and drives the existing two-phase FEC_CFG
  transaction; CFG_NACK, abort, swap, rollback and post-commit failure feed
  `rejected`, `abort`, `applied` and `commit_failed`. INIT validates the
  peer profile and NACKs an unsupported one; the first switch epoch is 2
  because INIT owns generation 1; the recovered-slot scratch is sized for
  the largest supported split so a rate switch cannot overrun it.
  The downlink binary codec is still the fixed 1x256/L8/r0.5 profile, so its
  controller stays at rate 50; the uplink GF(16) 8x32/L16 codec can build
  every ladder split (8/24 through 22/10) and switches rates.
- `host/test_fec_adapt_sclc` covers the rate ladder and n/m for the
  256-share downlink and 32-share uplink, estimator windows and masks,
  target boundaries, convergence at 0/1/3/5/10/15/25/35/45/55/100% loss,
  non-oscillation, all boundary paths, parameter validation, the CFG wire
  profile conversion, the FBK overlay, rate masks, and the epoch base.
- `host/test_parse` accepts the downlink and uplink adaptive lines and
  rejects off-ladder n/m, a wrong block, and a wrong L. `host/test_fec`
  round-trips the two shipped SC profiles and the SC FBK overlay;
  `host/test_sclc_uplink` pins all six uplink ladder splits.
- Per-tunnel `portblock=lo-hi` is consumed by the near-side carrier
  allocator instead of being recorded only. A near tunnel's carriers draw
  from its own window; `0-0` keeps drawing from the global block set through
  `/proc/net/tbtun/ports`. The module now keeps one absolute bitmap over the
  whole 16-bit source-port space, so overlapping windows share capacity but
  can never be handed the same live source port. A near window must hold at
  least one port per carrier slot (`n + m`, or `carriers` with FEC); a
  shorter block is refused with `EINVAL` and the `portblock` key is named.
  A far tunnel records the key but never dials.
- `host/test_portmap` pins the window rules, the overlap-conflict rule, the
  ports 1 and 65535 boundaries, lowest-first reuse after free, and the parse
  refusals (`0-0`, reversed, out of range, too small for the carrier count).

### Fixed

- FEC flow admission no longer self-deadlocks at init: the group
  reservation and publish take `sess->lock` briefly, while the flow and
  plane allocations run outside it. The SC-LDPC estimator's 32-entry arrays
  are guarded by `rx->groups <= TBT_FEC_ADAPT_GROUPS`, closing the
  bool-corrupting overrun. (e9e85bb)
- TX path masks cover all 256 carriers: `struct tbt_frame.avoid_mask` is a
  256-bit bitmap, the scheduler folds it without a u16 shift, and the retain
  bookkeeping is guarded to its legacy 16 slots. The RX frame initializer
  uses the same bitmap form. This removes the UBSAN index/shift reports seen
  on a 256-carrier 0.4.0 deployment. (17b8b49)
- `/proc` flag reads go through `tbt_read_bool()`: a compile-time check
  refuses a non-bool field, and the load is a byte load normalized to
  true/false. A flag byte left neither 0 nor 1 by a neighbouring
  carrier-indexed write no longer trips UBSAN's `invalid-load` in
  `tbt_show_stats()` or the stream/config readers. All stream, session,
  adaptive and portmap flags are covered. (eb8d242)
- The adaptive SC-LDPC estimator fold is skipped when `rx->groups` exceeds
  its 32-entry stack scratch; `BUILD_BUG_ON(TBT_FEC_ADAPT_GROUPS <
  TBT_SCLC_MAX_GROUPS)` and the mirrored host check keep that scratch tied to
  the codec limit. (eb8d242)
- `tbt_carrier_init()` asserts that `fec_drop_ppm` covers
  `TBT_MAX_CARRIERS_LIMIT`; it is the array immediately before the session's
  `adaptive` flag. (eb8d242)
- Near-side generated crypto keys are tunnel-level: the first near session
  stores the generated key and TX nonce in the tunnel, and every later
  session copies them (`source=inherited`) instead of generating a fresh
  key. This keeps the far's pinned key valid across session/stream churn and
  removes the G1 `crypto key changed` HELLO rejects. (0407b40)
- SC-LDPC OPEN admission validates the group bitmap against the peer's RX
  group count (the 256-bit wire map), not the local TX group count. A
  1x256/L8 downlink (`groups=1`) now accepts the 8x32/L16 uplink's 8-group
  OPEN instead of returning `-EPROTO`; the peer bitmap is installed into the
  RX plane, and FEC_CFG INIT checks that its group count matches. (702e980)
- A second SC-LDPC flow is refused cleanly before the group-count check:
  SC-LDPC with a live flow returns `-EOPNOTSUPP` (mapped to `TBT_BIND_DROP`
  and `fec_stream_refused`), so the carrier stays up instead of being
  stopped by the peer's 8-group OPEN bitmap against local `groups=1`.
  (62cb504)
- A sequential SC-LDPC flow is admitted with the peer RX mask: after the
  previous flow is released, the new OPEN's 8-group peer bitmap is installed
  into the RX plane while the TX mask stays local (`groups=1`), so the 60 s
  uplink → 1 GiB downlink sequence no longer returns `-EPROTO`. A concurrent
  second flow is still refused cleanly (`-EOPNOTSUPP`,
  `fec_stream_refused`). (7f2e180)
- Stream teardown releases the FEC flow with the stream:
  `tbt_stream_detach_from_session()` calls `tbt_fec_flow_detach()` immediately,
  so the flow count and group reservation drop at detach instead of at the
  2.2 s tombstone/reap. `tbt_tx_close_check()` computes its bound once via
  `tbt_tx_close_bound()` (an unwritten peer FIN gets 3 s, a written FIN keeps
  60 s / two wait budgets, and `rx_queued` keeps the longer bound), and
  `tbt_stream_start_now()` retries one admission after retiring a closing old
  stream while a concurrent live stream stays refused. Host `test_close_bound`
  and `test_stream_peer_eof` cover the 3 s/60 s bounds and
  RCV_SHUTDOWN/TCP_CLOSE EOF. (63ba0a1)
- G4 close-path wakeups: a new far stream, a finished FEC block, a received
  FIN and a delivered peer FIN now kick the FBK tick. `w_fbk` stops when the
  session has no stream, and the FIN retry / `tbt_tx_close_check()` otherwise
  slept until an unrelated carrier loss. `tbt_tx_fin()` stamps
  `close_since_ns` before its FEC early returns, and a duplicate sid is
  refused with `-EEXIST` before the SC-LDPC admission check. (587792d)

### Changed

- Writing `/ports` no longer replaces the bitmap. The absolute bitmap keeps
  the bits of live carriers, so changing the global window cannot hand a
  port still held by a carrier to a new one; carriers already up keep their
  ports until they close.
- The `ports` view labels its block `# global block ...`, because a tunnel
  with `portblock=` no longer uses it.
- TX P0/P1: a carrier that refuses a FEC batch with `-EAGAIN` now arms
  `SOCK_NOSPACE`, so the next ACK calls `tbt_car_write_space()` and kicks
  that carrier's drain immediately. The drain keeps its 2 ms timer as the
  bounded fallback. `/proc/net/tbtun/stats` reports `nospace_armed` next to
  `ws_calls`.
- TX P0/P1: the ARQ/control pump has one session work item. `tbt_tx_wake()`
  used to schedule one `w_tx` per carrier, and all of them then serialised
  on `pump_lock`; the session work clears its bit at entry, so a wake that
  races a running pump re-arms it instead of being lost.
- TX P0/P1: FEC carrier drains share one delayed work with a ready bitmap
  and a round-robin cursor. A carrier that refuses a batch keeps only its
  own bit and the 2 ms fallback; the other ready carriers are served in the
  same run. This is the single-thread TX step for 0.4.0.

### Verification

- Kernel matrix (2026-11-08): 6.6.162, 6.12.116 and 7.0.14 pass the module
  build, host tests and QEMU load check on x86_64 and aarch64. 5.15.225
  fails the module build as intended: the source requires Linux 6.1 or
  newer. See `docs/kernel-matrix.md` and `scripts/kernel-matrix.sh`.
- `host/test_fec_out` covers the write-space wake, the timeout fallback,
  and the shared ready-list (a full carrier cannot hold a ready carrier
  behind it).

### Known limitations

- Adaptive rate control is off by default. When enabled, the uplink GF(16)
  8x32/L16 codec can switch along the rate ladder; the downlink binary
  1x256/L8 codec is fixed at rate 50%, so its controller stays at 50.

## [0.3.3] - 2026-11-08

### Fixed

- A far-side OPEN for a new sid retires the old stream instead of leaving
  it live until an unrelated timeout: the old target, ACTIVE slot and hash
  entry are released, the old FEC RX cursor is barriered, and a live object
  that is no longer `far_sid` drops its late DATA/FIN/RST and is killed.
  The near allocator skips every sid still in the hash (including
  tombstones), so a 24-bit wrap cannot hand out a sid the far still
  protects. (e0312f6)
- A late FEC RX barrier no longer cancels a newer stream's pending
  decode/delivery. `tbt_session_bind_sid()` barriers only while `far_sid`
  still names the stream that adopted the OPEN, and re-arms the RX work if
  a later adoption wins the race during the cancel. (c6d863b)

### Added

- Uplink track: 0.3.3 uplink 1 GiB byte-exact acceptance harness and
  documentation under `review/wan-experiment/arm64/sclc-verify/`
  (`verify-uplink-1g.sh`, `far-uplink-recv.py`, `UPLINK-1G.md`). (18fd717)
- Fault track: `host/test_fault.c` models carrier kill, RST, FIN, a fast
  A->B->C redial and FEC block loss with the real RLNC codec;
  `scripts/qa/` adds the fault-injection and soak tools. (539bf24)
- Perf track: same-link A/B and direct-baseline tooling under
  `review/wan-experiment/arm64/perf-0.3.3/`. (a49f49d)

### Changed

- The FEC carrier batch is capped at three records (24,780 B) instead of
  eight (66,080 B). Eight frames crossed the 64 KiB boundary and needed
  order-5 pages; on the 955 MB far host the allocation failed in
  `tbt_carrier_send_fec_batch()` and direct reclaim stalled the FEC drain
  for 0.2-2.4 s. Three frames stay in kmalloc-32k (order-3), and
  `TBT_CAR_PART_MAX` follows the same cap. `tbt_core.c` now fails the build
  if the part buffer grows past 32 KiB.
  This is the t040-tx batch P0 (8 -> 3) stability/performance fix. (4df9a5c)
- The uplink GF(16) decoder returns immediately when every DATA share is
  already present, instead of gathering and re-solving the whole block.
  Missing parity shares stay unknown for the NACK path; `codes_ok` still
  reports every codeword as complete.

### Tests

- `host/test_fec_out` and `host/test_http` pin the three-record batch
  boundary: one, three and eight records, short records, and the
  part/short-write split. `host/test_sclc_uplink` covers the parity-only
  early return and its `share_known`/stat results.

## [0.3.2] - 2026-11-08

### Fixed

- The per-stream announce mark is cleared when a carrier is lost and when
  `join()` installs a new generation. It stored four bits of the carrier
  generation while `sess->carrier_gen` is a full counter, so a replacement
  whose generation was 16, 32, ... greater looked already announced, its
  OPEN was skipped, and the slot stayed `up=0` forever (observed: carrier 3
  gen 4 replaced by gen 292). The 50 ms session poll also re-announces a
  connected carrier whose HELLO was written but whose OPEN never queued,
  counted as `up_rescued`. `tbt_session_retire()` compares the peer's
  generation carried in RETIRE with `peer_gen`, not the local counter. c3
  reports `replacing=` and `empty=`, so `up + replacing + empty == n` holds
  at every instant. (78d5a90)
- The NACK scan no longer probes a block while its 50 ms decode settle
  window is open. The scan and the window raced on first connect: each
  replay restarted the window (fix45 measured 482 NACKs, 27,465 replayed
  shares and 226 MB in one connect, with far NACK->wire 286.7 s). A block
  with no parity is never gated, and a failed decode still asks for repair.
  (2f074e5)
- The FEC data plane is isolated across a stream switch: a new sid resets
  the RX cursor and blocks even when the session barrier was skipped; a
  block carries the sid of the record that placed it, and a non-current
  block is refused at delivery; only the current stream may append to the
  shared TX ring; a stale target read is consumed and discarded; and the
  old stream is killed with its replay ring reset. (859ddd4)
- A stream switch flushes the whole FEC TX ring. The new stream's first
  submit is refused while the packetizer owns the old block; `switch_sid`
  stays set through the cleanup; `[tail, head)` is dropped
  unconditionally; every replay record is freed; and an old stream's late
  submit is discarded while the gate is set. (7751dc9)
- The packetizer also triggers the ring switch, because it can run before
  the new stream's first submit when `cur_block_sid` and `switch_sid` are
  both 0. A submit from a different stream (including the no-block case)
  flushes the ring; the old stream is killed even if it never built a
  block; and every queued data/replay record is validated against the
  current `far_sid` at send time. (a275dea)
- An empty marker built for a NACK now carries the NACK's sid and is
  dropped if that sid is no longer current. `tbt_fec_stream()` rejects a
  dead, detached or tombstoned stream, the packetizer switch decision and
  the whole-ring flush are re-evaluated in one `tx->lock` section,
  `old_sid` is the block owner or last submitter (never the current
  stream), and the per-carrier fallback flush validates the record sid.
  (1a5bf63)

### Added

- FEC block decode runs once, at the end of its share burst, instead of
  as soon as one parity share arrives. Every new share pushes
  `decode_next_ns` out by 50 ms; a failed decode backs off 50..400 ms; a
  duplicate share does not postpone the decode; and only the delivery
  frontier is decoded. On HND0 a 1 MiB block took 47.8 ms and was decoded
  7.7 times per delivered block; the single `w_fec_rx` worker spent
  0.96 core to deliver 2.86 MB/s. The 64x8 bit plane gather/scatter is
  replaced by an 8x8 SWAR transpose: host -O2 encode 135 -> 528 MB/s,
  decode with one share erased 112 -> 356 MB/s, and a block-end decode at
  0.5% share loss costs 3.8 ms instead of 8.9 ms. (78db715)

### Documentation

- `review/0.3.2-scope-audit.md` records the 0.2.x content against 0.3.1,
  the release-evidence gaps, and the P0/P1/P2 work list. (a1cbae1)

### Verification

- fix54 substantive acceptance PASS. The tag-exact 0.3.2 release gate
  (tag archive build on both hosts, 1 GiB, DKMS, watchdog, up=256, windowed
  NACK->wire) remains pending; see `review/0.3.2-release-draft.md`.
- Correction to 0.3.1: `fec_nack_lat_ns` is a session cumulative sum
  (far 502.20 s in the 1 GiB run), not a NACK->wire latency. The tail is
  `fec_nack_lat_max_ns` 603.9 ms (far), so the 600 s cumulative bound does
  not establish a 600 s latency bound. 0.3.2 gates use the windowed delta
  and record `fec_nack_lat_max_ns` / `fec_nack_repair_max_ns`.

## [0.3.1] - 2026-11-08

### Fixed

- A short final block no longer NACKs its padding shares: the receiver asks
  only for shares the sender produced (`fec_nack_pad_skip` counts a skipped
  request), so the last block does not wait for records that never existed.
- A short block keeps at most 8 KiB of parity instead of a full 1 MiB block
  (`fec_short_parity_saved`).
- `fec_replay_miss_never_stored` falls from 42,963 in the 0.3.0 1 GiB run to
  24 (near) / 21 (far): the replay ring now holds the shares the receiver
  asks for.

### Verification

- 1 GiB single stream: rc=0, HTTP 200, size 1,073,741,824, near/far sha256
  equal (`e926b854…ad7393`), 889.20 s (9.66 Mbit/s), TTFB 0.540 s; no reset,
  holes or drops.
- NACK→wire: `fec_nack_lat_ns` is a session cumulative sum (far 502.20 s),
  not a latency. The tail is `fec_nack_lat_max_ns` 603.9 ms (far); the
  600 s cumulative bound does not establish a 600 s latency bound. Replay
  high-water 2.94 MB (< 5 MiB); `fec_nack_backpressure=0`;
  `fec_nack_pad_skip=0`; `fec_short_parity_saved` 23 near / 889 far.
- Wire: `TBT_FEC_CONV 0x1006` / `TBT_WIRE_VERSION_V6 3` unchanged; 0.3.0 and
  0.3.1 interoperate.

### Known observations

- At the end of the 1 GiB run `up=252` (near) and `up=254` (far): 2-4
  carriers became unschedulable. The transfer stayed byte-exact with no
  reset/hole/drop; a run that requires 256 up for the whole transfer should
  judge this separately.
- far logged a `tbt_car_rx_work` workqueue hog (>10 ms, 7 times) during the
  steady window. It is a CPU hotspot on the 256-carrier RX path, not a
  reset/hole/drop.

### Deferred to 0.3.2 and later

- Concurrent multi-stream v7 (branch `w-fec-v7`).
- SC-LDPC rate adaptation and Adaptive Phase 2.
- The end-of-run `up=252/254` carrier availability.
- HND0 CPU cost (the far `tbt_car_rx_work` hotspot).
- The fix29 XOR/fec_frame bisection (word-wise XOR 751715b, per-carrier FEC
  frame buffer 28e3de2); measurements stay under `benches/results/`.

## [0.2.3] - 2026-11-08

### Changed

- The peripheral BBR hold is enabled by default (`hold_enable=1`). The 50 ms
  carrier poll restores the last good `snd_cwnd`/pacing across idle periods.
  Set `hold_enable=0` at load, or write
  `/sys/module/tbtun/parameters/hold_enable`, to opt out. The hold writes
  `tcp_sock` fields directly, which is not a stable kernel API; `hold_enable=0`
  is the rollback.
- Wire: `TBT_FEC_CONV` stays `0x1005`; 0.2.3 changes only the default
  parameter, so 0.2.2 and 0.2.3 interoperate.

### Verification

- 256 carriers steady state: 10 s window max 27.9 Mbit/s, 60 s average
  23.9 Mbit/s; wire/app 2.03; far CPU 65.7%; retx 37k ppm. The tunnel stayed
  at 256 carriers for the whole run; `bind_drop_no_stream=0`,
  `fec_drop_window=0`; no WARNING in the kernel window.

## [0.2.2] - 2026-11-08

### Added

- Opt-in BBR hold (`hold_enable=0`, module parameter, default off): the 50 ms
  carrier poll restores the last good cwnd and pacing across an idle period
  instead of letting the stack restart from the initial window. It is off by
  default because it changes carrier behaviour; `host/test_hold` covers the
  hold/release rules.

### Fixed

- FEC DATA no longer leaves on a carrier whose OPEN for the current stream is
  not on the wire. The far drops DATA that arrives before its stream exists,
  and the sender has already consumed the share; the first connection could
  lose its request and wait for the application timeout. The far counts that
  drop as `bind_drop_no_stream`.
- A new stream resets the FEC RX delivery state (frontier, reorder blocks,
  partial cursor, FIN bounds) before it is bound, and rebases the window on
  its own first block: `next_super`, `highest_block` and `next_deliver` follow
  the new sid, while a lower block may only move the window before the new
  stream's first delivery. The previous stream's reordered tail can no longer
  be delivered to the new stream or leave it stuck behind a stale window.
- FEC DATA/PARITY carry the 64-bit stream id in the record header
  (`TBT_FEC_CONV` 0x1005, 48-byte header). The receiver drops a share whose
  sid is not its current stream (or whose sid is the old barrier sid); the
  sender closes the old block and tags its records with the old sid, so a
  queued old-stream record is never delivered to the new stream.
- Switching streams clears the old stream's FEC TX state before the new one
  publishes, so no old-stream record is queued or sent under the new sid.
- The old target socket is closed when its stream dies, so a sequential
  connection cannot leave the previous target open or answer with HTTP/0.9.
- A tunnel overwrite keeps the session crypto state. Each side adopts the
  peer's key and TX nonce from the first HELLO and refuses a different one on
  every later carrier, but the replaced session's last HELLOs and the
  replacement's are indistinguishable to it. Regenerating the key or nonce on
  every overwrite therefore rejected every carrier of the replacement
  (measured: 256 carriers, near up=18, 25 far "crypto key changed"). A
  replacement now inherits the TX nonce when the role is unchanged, and a
  near side also inherits the key it generated and announced. Only a new
  tunnel, or a delete and re-add, generates fresh crypto state. The
  `crypto_key=` path is unchanged.

### Verification

- 32 carriers: join and first connect reached 78/72 Mbit/s; a sequential
  stream aborted and replaced delivered 92.7 MB in 15 s with no HTTP/0.9
  response, `fec_drop_window=0` and no stuck stream. The 32-carrier short test
  ran at 32.1 Mbit/s with no regression.
- Wire: `TBT_FEC_CONV` 0x1005 and the 48-byte header are not compatible with
  0.2.1 (0x1004, 40-byte header); both ends must be upgraded together.
  `hold_enable` is opt-in and off by default.

## [0.3.0] - 2026-11-08

### Added

- Downlink SC-LDPC (binary, one group of 256 shares, L=8, rate 0.5) is wired
  into the FEC data plane; the shape alone names the variant (`mode=sc-ldpc`,
  `w` is the codeword block: 256 downlink, 128 GF(16) uplink, 512 reserved).
- Uplink grouped GF(16) SC-LDPC (8 groups of 32 shares, L=16, rate 1/4) is
  wired; the per-direction INIT (`FEC_CFG`) carries L, block and rate, and the
  receiver uses the peer's shape.
- FEC outq backpressure instead of dropping records; the packetizer stops at
  the replay high-water mark; a NACK reply is capped at 4 records and
  deduplicated.
- Adaptive FEC Phase 1 (GF(2) data plane). The receiver estimates the
  DATA-share loss rate from per-block miss bitmaps (1 s window, EWMA, at
  least 1024 alive-filtered shares and 4 super-blocks); the sender runs the
  control law over the fixed `n + m = 8`, `groups = 32` family (`m` 1..6) and
  switches shape through `FEC_CFG`/`FEC_FBK` at a super-block boundary.
  `adaptive=` defaults to `fec`, `session <id> adaptive=on|off` flips it at
  runtime, and the `c13` line reports epoch, shape, class, `p_ppm`,
  `p_win_ppm`, `r_fail_ppm`, switches, aborts, rollbacks and pauses.
  (02f2517, 00d34cf)
- Wire `TBT_FEC_CONV 0x1006`: `FEC_CFG` grows from 32 to 48 bytes and names
  the shape switch, `FEC_FBK` becomes a 48-byte report, and HELLO body[0]
  moves to `TBT_WIRE_VERSION_V6 3`. A 0.2.x peer (0x1005) is refused at the
  first HELLO; both ends must be upgraded together.
- `host/test_fec_adapt`, `host/test_fec_sclc` and `host/test_fec_sclc_data`
  cover the estimator, the control law and the SC-LDPC codec.

### Fixed

- A session work reference is no longer lost when `tbt_session_arm()` races a
  `cancel_delayed_work_sync()`. `cancel_work_sync()` disables the work while it
  runs (kernel 6.13+), so a queue attempted in that window is dropped. The arm
  now takes its reference before publishing the sched bit and returns it when
  `work_pending()` shows that no instance took the queue. The same protocol
  fixes `tbt_stream_schedule()`, and the two raw `schedule_work()` calls for
  `w_fec_tx` go through the session bit. Without this, one lost reference kept
  `refs` above zero, `wait_for_completion(&drained)` never returned, and a
  `del 1` config write plus `rmmod` stayed in D state forever (observed on
  0.2.1: `tbt_session_destroy+0x40b` waiting on the FEC RX stuck path).
- Session destroy waits for `drained` with a bound (`TBT_SESSION_DRAIN_MS`,
  10 s). A leak is reported with the remaining reference count, sched bits,
  streams and carriers, and the session is leaked instead of blocking the
  caller and every later `rmmod`. `host/test_lifetime` pins the ownership
  interleavings and the bounded drain.
- A block decode that cannot finish the block backs off instead of decoding
  the same block 128 times before its NACK repairs arrive.
- The peer's graph is taken from the initial `FEC_CFG`; a repair burst is
  paced instead of filling the replay ring.

### Verification

- First connect 15 s: 53,477,376 B, HTTP 200, TTFB 1.262 s.
- Steady state 60 s: 2.59 MB/s app (155,500,544 B), wire 374 MB far /
  312 MB near, outq_drop 0, NACK backpressure 0, up=256.
- 1 GiB single stream: rc=0, HTTP 200, size 1,073,741,824, near/far sha256
  equal (`e926b854…ad7393`), 737.60 s (11.65 Mbit/s); no reset, holes or
  drops.
- Wire: `TBT_FEC_CONV 0x1006` is not compatible with 0.2.x (`0x1005`); both
  ends must be upgraded together.

### Known limitations

- `fec_replay_miss=42,963` (all `never_stored`) on the far side of the 1 GiB
  run: the requested shares were not in the replay ring, the receiver
  retried, and the transfer still finished byte-exact; replay high-water
  4.9 MiB and backpressure 0. This is a repair-efficiency residual, not a
  failed acceptance criterion.
- The last block of a stream carries full parity; there is no partial-parity
  saving for a short block.
- SC-LDPC rate adaptation, Adaptive Phase 2 and concurrent multi-stream are
  not in 0.3.0; see [0.3.1].

## [0.2.1] - 2026-11-08

### Fixed

- TX window auto-sizing takes the carrier aggregate as its floor: `sum(snd_wnd)`
  (or `n_car × per-carrier BDP`), with a bootstrap floor of
  `max(1 MiB, n_car × 8 × TBT_PAYLOAD_MAX)`. The auto cap and the receiver's
  reorder budget scale with the carrier count (at least 8 MiB, eight full
  frames per carrier, 256 MiB ceiling), so a 256-carrier tunnel is no longer
  held at the single-flow 8 MiB cap. An explicit `tx_window_bytes=` is never
  truncated. (ba938c6, b4ca269)
- The BDP estimate covers every live carrier: a carrier without a delivery-rate
  sample falls back to its advertised window divided by SRTT, so the aggregate
  is not estimated from the few carriers that happened to produce a sample.
  (cdae2a4)
- FEC replay and credit are sized from the aggregate BDP: the ring keeps the
  largest K up to `TBT_FEC_REPLAY_K_MAX` that fits 15 MiB (the 256-carrier shape
  lands on K=7, smaller shapes get a larger window), and the ACK credit is
  `K_eff - 2` super-blocks capped at `TBT_FEC_CREDIT_MAX` (1024 packets, 8 MiB
  at S=8192). (f651bb4)
- Each FEC carrier drains its own queue in its own work; writes are batched in
  8-frame parts and the part buffer covers a full batch (66 KB, u32 counts).
  (d0953f8, 20c1511, af0ee9a, d03b817, 04b1298)
- The TX pump does not run in FEC mode when `tx_q`/`ctl_q` are empty. (e155fb7)

### Performance

- 32/64/256 carriers: app 107.6/118.2/97.3 Mbit/s, which is 96.0%/105.4%/86.8%
  of direct/2 (112.1 Mbit/s). 256 is +70.7% over fix30 (57.0 Mbit/s), its wire
  rate is 96.3% of the direct P32 run, wire/app falls from 2.44 to 2.22 and retx
  from 13.5% to 4.9%. 32 and 64 meet the target; 256 is 3.2 points short of
  90% and stays on the 0.3.0 list (`txq_max` 16.8 MB still full, far CPU 1.47
  cores).
- Raw data: `benches/results/20261107T231300Z-fix31-compare/` and
  `benches/results/20261107T231242Z-remote-fix31/`; fix30 comparison kept under
  `benches/results/20261107T223300Z-fix30-direct32-vs-tunnel/` and
  `benches/baseline/remote-fix30.csv`.

## [0.2.0] - 2026-11-08

### Added

- A host microbenchmark base (`host/bench_micro`, `make bench`/`bench-full`,
  `scripts/microbench.sh run|baseline|compare`) that links the same
  `tbt_fec.c`/`tbt_crypto.c` objects and `tbt_tx_acct.h` the module uses and
  mirrors the kernel-only scheduler and NACK scan loops. It writes a stable
  five-column CSV (`name,iterations,ns_per_op,mb_per_s,checksum`), targets
  <= 2 s with `--quick` and <= 10 s with `--full`, and stores every run under
  `benches/results/<UTC stamp>/`. `benches/short.sh` is the matching real-host
  short sweep (32/64/256 carriers, fec/profile/crypto toggles, 5 s single
  stream); `benches/MICROBENCH.md` records the first host baseline and the
  noise rules. No kernel source changed.
- FEC NACK repair (`TBT_FEC_CONV` 0x1004): a NACK names its super-block in
  the record header and carries a 32-byte group bitmap plus a 32-byte share
  bitmap. The sender keeps up to the last `K = 7` super-blocks of DATA+PARITY
  records in a replay ring capped at 15 MiB (about 14.08 MiB for the
  production 32-group shape; the effective K shrinks until the cap fits) and
  re-queues the requested shares on any up, non-stalled carrier. The ACK
  credit is capped to `K - 2` super-blocks, so the ring covers the sender
  window and the oldest NACKable block is never evicted. The ring and the
  16 MiB RX arena are constant; the total FEC memory is at most 31 MiB and
  neither grows with the stream. The c11 line exports `fec_nack_tx/rx/retx`,
  `fec_nack_drop`, `fec_nack_bad`, `fec_nack_retx_bytes`,
  `fec_replace_retx_bytes`, `fec_replay_hit/miss`,
  `fec_replay_miss_no_ring/alloc/get`, `fec_nack_lat_ns`,
  `fec_nack_lat_max_ns`, `fec_nack_repair_ns`, `fec_nack_repair_max_ns`,
  `fec_reset_tcp_failed`, `fec_reset_user`, `fec_wait_carrier` and
  `fec_backpressure`.
- `dev=<ifname>` on a tunnel line binds the near side's dialling carrier
  sockets to one interface with `SO_BINDTODEVICE` before `bind`/`connect`, so
  a TUN or proxy policy route cannot capture the dial. The name is 1..15
  bytes; an empty value, whitespace, control characters and `/` are refused.
  The key is near-only and empty by default; a bind failure is a dial failure
  (`connect_fail++`, retry after 200 ms, ratelimited log naming the interface
  and the error).
- The c3 line gains `stall_recovered`, `stall_drop` and `stall_dead`.
  `stall_declared` now counts a FEC carrier paused by the poll, not a carrier
  killed; `stall_recovered` counts a paused carrier whose `snd_una` moved
  again; `stall_dead` counts a paused carrier that outlived
  `min(2×stall, 300 s)` without wire progress and was replaced. `stall_drop`
  is the ARQ pause helper's frame counter; it has no caller yet and stays 0.

### Changed

- The 30 s FEC hole timer no longer resets a stream by itself. A super-block
  that is still not ready after `TBT_FEC_STALL_MS` (30 s) resets only when no
  carrier TCP is ESTABLISHED below the default `tcp_retries2`, or on a
  user/peer teardown; while a carrier TCP is alive the receiver keeps NACKing
  and replaying, and out-of-window bytes stay in the carrier's TCP send queue.
  Under the documented assumptions this gives `E_tunnel ⊆ E_direct` and
  `P(user stall) ≤ P(direct stall)`; it is not a promise that the tunnel never
  stalls.
- `carrier_stall_ms` is now a cap, not the threshold. The threshold is
  `5 × RTO` (`RTO = SRTT + 4×RTTVAR`, floored at 200 ms), clamped to 1..60 s;
  an explicit `carrier_stall_ms=` can only lower it. Under FEC a stall pauses
  the carrier: new shares avoid it, the shares already in flight are treated
  as lost and recovered by parity, and the carrier is replaced only after
  `min(2×stall, 300 s)` without wire progress. Under ARQ a stall still kills
  the carrier and repaths its retained frames immediately.
- Every carrier socket enables TCP keepalive with `TCP_KEEPIDLE=30 s`,
  `TCP_KEEPINTVL=10 s` and `TCP_KEEPCNT=3`, so a half-open path closes at the
  socket before the `stall_dead` bound. A failure to set the options is logged
  and not fatal; the poll's bound remains the fallback.
- The FEC sender skips stalled carriers for new shares. When every usable
  carrier is stalled, an ACK stays queued and the flush returns `-EAGAIN`
  instead of dropping it, so a recovered carrier can reopen the credit window
  instead of deadlocking on an ACK that was never sent.
- Carrier lifetime: carriers are no longer reused from a dead pool.
  `tbt_carrier_get/put/gen` with a refcount and `kfree_rcu` replace
  `take_dead`, and `tbt_carrier_unpublish/close` release `c->sk` before the
  pointer is cleared (`review/s2-merged-design.md` §3).
- Session and stream lifetime: `tbt_session_get/put/arm/cancel` with
  the `drained` completion replace the dead `refs` field, and stream work
  holds one of five bits and is cancelled through `tbt_stream_schedule/cancel`
  (`review/s2-merged-design.md` §1, §5).
- Wire revision: `TBT_CONV` is `0x0006` and `TBT_FEC_CONV` is `0x1004`
  (0.1.0 used `0x0005`/`0x1001`). FIN carries an exactly 16-byte body
  (`pkt_end`, `pkt_base`); a 0.1.0 peer is refused at the first HELLO by the
  conv check (`review/s2-merged-design.md` §6). On the FEC wire, `0x1002`
  made a PARITY record usable as an empty-block marker (`win_len=0`,
  `payload_len=0`, `epoch=0`); `0x1003` grew the ACK body from 16 to 24 bytes
  and added `deliver_bytes`, the stream-relative byte count the receiver
  actually handed to its output socket (the first 16 bytes keep their
  `0x1002` offsets; the sender's `tx_ack` is that value instead of
  `next_deliver * S`, so a short share or an early-closed block no longer
  acknowledges bytes that were never delivered); `0x1004` makes NACK a
  64-byte bitmap repair request (see Added). 0.1.0 and 0.2.0 do not
  interoperate.

### Fixed

- FEC parity send at block close: `tbt_fec_tx_emit_parity()` discarded
  `tbt_fec_send_record()`'s return, so a failed parity send was silent and the
  receiver waited for a shape it never got. It now logs
  block/group/parity/win_len/epoch/store/send at debug>=2 and re-arms the TX
  work on failure; the replay ring keeps the parity for the peer's NACK probe.
  The close path also logs each group's `g->blk_id`/`nreal` and whether it
  took the parity, empty-marker or skip branch.
- FEC empty-marker delivery: `tbt_fec_tx_emit_empty()` sent the marker on the
  parity-0 carrier and discarded `tbt_fec_send_record()`'s error, so a stalled
  or full carrier silently lost it. The marker is now a control record (any up
  carrier), its store/send result is logged, a failed send re-arms the TX
  work, and the replay ring keeps it for the peer's NACK probe. The NACK scan
  probes every empty group of the blocked super-block, not only the frontier
  group, so the markers are requested.
- FEC whole-block loss at the delivery frontier: the receiver only NACKed up
  to `highest_block`, so a super-block whose every share was lost was
  invisible and the sender's TX stall fired. The scan now probes the next
  super-block (parity 0 per group) even when no share of it was seen; the
  sender's replay ring states the shape. A stale final-block tail of 0 with a
  present DATA share no longer marks the block empty: the share is kept and
  parity/NACK states the real cut. The parity reply starts at row `s % m`
  (which always covers DATA share `s` under the band matrix) and falls back
  to the other covering rows; a fixed row 0 misses every share with
  `s % m >= band` and repeats a duplicate that cannot decode the block.
- FEC sparse-block NACK storm: a started super-block's unstarted groups were
  requested for their whole `n+m` range before the stream end was known. A
  100-byte short stream on 32 carriers produced full-range NACKs for shares
  the sender never generated, the block never became ready, and the stream
  was reset. The receiver now probes only parity 0 for an unstarted group
  until `total_pkts` is known; the empty marker, the parity's `win_len` or FIN
  gives the real shape. The sender answers a never-stored share in a closed
  empty group with its empty marker and stores it in the ring. A closed short
  block (`win_len < n`) has no records for its padding DATA shares; a NACK for
  one of those is answered with a parity row carrying the real
  `win_len`/`epoch`, so the receiver learns the cut instead of waiting.
  `fec_replay_miss_get` is split into `fec_replay_miss_evicted` and
  `fec_replay_miss_never_stored`; `fec_nack_sparse_probe` and
  `fec_nack_empty_reply` count the new paths.
- FEC NACK never reached the wire: `tbt_fec_send_nack()` tested the positive
  64-byte body length from `tbt_fec_nack_pack()` as a boolean and returned
  `-EINVAL`, so every repair request was dropped (`fec_nack_tx=0`,
  `fec_nack_drop` climbing, `fec_replay_hit=0`). The sender now compares the
  return to `TBT_FEC_NACK_LEN`; a failed NACK is split into
  `fec_nack_enomem/einval/enospc/eother` and logged with its block and masks.
- FEC work re-entrancy: `w_fec_tx` and `w_fec_rx` can be re-queued while
  running, and workqueue then executes the new instance on another worker.
  Two packetizers shared `tx->pkt`/`tx->rec` (a header CRC computed for one
  record and a payload written by the other produced the field's
  `fec_payload_crc_fail=179` with `fec_hdr_crc_fail=0`); two delivery walks
  shared `next_super`/`deliver_bytes` (one 8 KiB slot duplicated and another
  skipped). Each work now owns an `atomic_t run` word: a second entry only
  raises the `again` bit and returns, and the owner re-arms once when its body
  ends. The raw parser also consumes a FEC record on every return, because
  `tbt_fec_rx_record()` may have decrypted it in place; a retry would XOR the
  plaintext twice. The `stopping` check moved ahead of decryption for the same
  reason.
- FEC receive, silent slot loss: the delivery walk skipped a slot whose index
  was past the block's stated cut (`p >= real`) and, separately, a slot past
  the stream's last packet (`seq >= total_pkts`) without a counter, while the
  packet frontier still advanced by `groups * n` and the ACK followed it. A
  present share in such a slot means the sender's cut and its own DATA records
  disagree; the block is now refused and `fec_slot_past_cut` names it, so the
  frontier cannot run past a payload that was never delivered.
- FEC receive, undiagnosable stall: `fec_decode_blocks`, `fec_decode_recovered`,
  `fec_decode_nomem`, `fec_deliver_nomem`, `fec_deliver_bad`, `fec_dup_share`,
  `fec_late_share`, `fec_pad_share`, `fec_epoch_conflict`, `fec_slot_past_cut`,
  `fec_fin_bad`, `fec_ack_bad`, `fec_tx_share_gap` and `fec_empty_block` are
  exported in a new `/proc` line `c10`. The 1 GiB acceptance run reported
  "every error counter is zero" while the stream lost three 8192-byte slots,
  because the counters that name those losses were not exported.
- FEC receive, recovered length: `tbt_fec_rx_share_len()` is now the one
  place that decides a slot's delivery length, and it lives in `tbt_fec.h` so
  the host tests exercise it. A recovered share uses the parity epoch only at
  the block's last real position; every earlier position is `S`.
  `tbt_fec_blk_decode()` refuses a block whose short share sits anywhere but
  that position, or whose epoch disagrees with the share it describes, rather
  than delivering zero padding as data.
- FEC receive, duplicate share: `tbt_rx_fec_deliver()` now reports a share
  whose bytes the stream socket already accepted as `1` instead of `0`. The
  delivery walk counts `fec_deliver_dup`, realigns `deliver_bytes` to
  `rx_next`, and does not advance the cursor a second time. The old path
  dropped the share and still added `len`, so `deliver_bytes` (and the ACK)
  could name bytes the application never received. The branch is defensive:
  the ordered walk only reaches it when the cursor and the socket disagree.
- FEC receive, unordered delivery: `tbt_rx_fec_deliver()` no longer inserts a
  share into the reorder tree when the ordered walk's offset is past
  `rx_next`. That path delivered bytes out of order, so the first divergence
  corrupted the rest of the stream. A hole now returns `2`, the walk counts
  `fec_deliver_ooo`, stops the FEC receiver and resets the stream; a partial
  overlap returns `1` and is treated as a duplicate. The tree was the last
  way for an FEC stream to deliver a byte that was not the next source byte.
- FEC receive, empty final group: the final super-block may leave a group with
  no packet. `tbt_fec_super_ready_locked()` skips such a group only when no
  block exists for it; a block that does exist must still be ready. The
  delivery walk now checks `tbt_fec_blk_deliverable()` (used && ready &&
  block_id == s) before reading a slot, so a stale buffer can never be
  delivered as stream bytes. `fec_late_data` names a late DATA share whose
  real slot is still absent (the ready rule makes it impossible; the counter
  exists so a field run can prove it).
- TX: a duplicate frame is fully built before publication, and the
  retention index is bounded so a 16-slot overflow cannot write past the array
  (`review/s2-merged-design.md` §9.1).
- FEC receive: decoding moves out of `rx->lock` into `w_fec_rx`,
  delivery commits per packet with a cursor, FIN uses `pkt_end`/`pkt_base`,
  an empty group uses the block marker, the FEC ACK byte count is clamped, and
  `short_len` is fixed by the first PARITY record
  (`review/s2-merged-design.md` §9.1).
- Adoption: the far side installs the stream callback only after the
  provisional carrier is published, with `joined` and a `WARN_ON_ONCE` on the
  ordering (`review/s2-merged-design.md` §9.1).

## [0.1.0] - 2026-11-07

### Added

- FEC v6 grouped data plane. One share rides each carrier:
  `carriers = groups * (n + m)`, `group(carrier) = carrier / (n + m)`,
  `share(carrier) = carrier % (n + m)`. The production shape is 32 groups of
  `n=4, m=4` on 256 carriers.
- The global packet sequence is striped over the groups (`group = seq % groups`).
  Each group has its own block window, codec context and parity. The receiver
  delivers a super-block (one block from every group) in global sequence order,
  so the stream has no hole and the ACK frontier is global.
- A short final block is zero-padded and carries its real length in the parity
  `win_len`. If every parity record of that block is lost, the FIN byte offset
  gives the total packet count. Padding is never delivered.
- Host coverage for the grouped mapping, the final-block length and a 32x8
  encode/decode/reassembly run, including a short final super-block. QEMU probe
  N carries 1 MiB each way over two groups of eight carriers in the HTTP chunk
  container, byte-exact.
- `c9 fec_proto_fail fec_hdr_crc_fail fec_payload_crc_fail` in
  `/proc/net/tbtun/stats`, the receive verdict T0 §1.3 specifies. With
  `crypto=on` a wrong key is a payload CRC failure on every share, so the
  counter separates "the key is wrong" from "the peer is silent".
- `fec_short_tx`, `fec_short_unknown`, `fec_drop_window` and `fec_drop_crypto`
  in the c9 line: partial packets the packetizer flushed, blocks the receiver
  refused because they mixed a recovered share with a short one and stated no
  length, shares that arrived more than a block window ahead, and shares that
  arrived before the session key.
- The parity record's `epoch` field carries the block's last real share length
  (0 = every real share is full). A recovered share has no `payload_len` of its
  own, and the parity that recovers it can arrive before the share does, so
  without this a recovered short share would be delivered as S bytes and the
  block's zero padding would reach the application. This cut does not implement
  rate epochs; the deviation is recorded next to the HELLO conv one.
- QEMU probe P and `host/tbt_ping`: one small write and one reply with the
  socket still open, over the smoke's own tunnel shape. Sizes 37, 1, 8191,
  8192, 8193, 20000 and 65536 bytes, two connections in one session, and a real
  HTTP request/response handshake against busybox httpd. A 1 MiB burst cannot
  see any of this.
- QEMU probe O: `profile=http-kal`, `n=1 m=1`, `carriers=2`, `fec=on`,
  `mode=grouped`, with the T5.2 smoke's own knobs. It runs the same tunnel
  with `crypto=off`, with `crypto=on`, with carrier 0 taken down mid-transfer,
  and with a 700 ms loopback delay that forces a dial past one connect window.
  Every phase asserts both carriers up, `rx_proto=0`, `http_parse_err=0` and
  the FEC CRC counters at zero.

### Changed

- The HTTP container's `Content-Type` is `application/gzip` on both the request
  and the response block. The body is still a tbtun FEC record (optionally
  ChaCha8-obfuscated), not a gzip member: nothing is compressed and no
  `Content-Encoding` is sent. The request parser accepts exactly this value, so
  both ends must run this revision. `application/gzip` is the media type a
  classifier expects on a long upload and one a middlebox will not try to
  decode.
- QEMU probe volumes: L/M carry 256 KiB and O 64 KiB (256 KiB for the
  carrier-replacement phase) through `host/tbt_ping`; N keeps the single 1 MiB
  transfer. Small messages are the case that regressed, so they are what the
  suite exercises most.
- `fec=on` now owns `carriers` session slots and marks all of them active; the
  old ARQ still uses `n` DATA carriers plus `m` spares.
- `tbt_fec_tx_submit()` updates the stream byte offset, so the FIN names the
  real end of the stream and the FEC ACK can clamp to it.
- `mode=sc-ldpc` with `fec=on` returns `-EOPNOTSUPP` instead of silently using
  the RLNC data plane.

### Fixed

- The first carrier wave was a connect storm: `tbt_carriers_start()`
  scheduled `w_connect` for all `n_car` slots at once, so a 256-carrier
  session fired 256 dials in one instant at the far listener. Every carrier
  is now installed unconnected and the 50 ms session poll drives the first
  dials through the same `max(8, n/4)`-per-second token bucket that paces
  replacements and failed-connect retries; the bucket starts with a
  16-token burst so small sessions are not delayed.
- A 256-carrier session plus a connect flood could starve the far host until it
  stopped answering (HND0: `tbt_accept_work` and `tbt_replace_work` hogged a
  CPU, 10138 accepted connections, journal stops, unclean reboot 62 min later).
  The far's provisional list had no bound, `tbt_replace_work()` walked the
  whole graveyard of every carrier it had ever replaced on every pass (O(n²)
  total, and the graveyard was never freed before teardown), the accept loop
  had no per-run budget, and every accepted connection and every failed dial
  wrote an unratelimited console line. Provisional carriers are now capped at
  `n + n/4`, freed as soon as they expire or are refused, the poll holds
  `sess->lock` across its non-sleeping provisional branch so that free cannot
  race it, replacement passes retire at most 16 carriers and release at most
  16 join-replaced carriers before re-arming 20 ms later, replacement dials are
  token-paced at `n/4` per second, accepts are
  batched at 32, the poll's state-change wake is 10 ms instead of 0, and the
  per-connection/per-dial lines are ratelimited. QEMU probe Q floods the far
  with connections that never send HELLO while taking carrier slots down in
  waves: the provisional peak stays at the cap, the list drains, userspace keeps
  running, and a legitimate replacement still joins.
- A carrier that never completed its first connect was retried every 200 ms
  per slot, outside the replacement pacing (up to 1280 attempts/s at 256
  carriers). The retry now spends a token from the same `max(8, n/4)`-per-
  second dial bucket. Join-replaced carrier objects are now a bounded reuse
  pool: an object is reused, buffers included, only after
  `tbt_carrier_release()` has run, the 50 ms poll holds no reference
  (`pollers`), and `w_connect`/`w_rx`/`w_tx`/`w_pump` are idle. `dead` is
  capped at `2 * n_car`; a join that would exceed it is refused with
  `-ENOSPC` until a new carrier reuses an entry. The poll takes its
  reference under `sess->lock`, the same lock the reuse path takes, and the
  deferred-release drain cancels `w_tx`/`w_pump` first.
  `dead_now`/`dead_peak` are in the c3 line, and probe Q asserts both caps.
- A `fec=off` session warned `!work->func` on teardown: `w_fec_tx` is
  initialised only by `tbt_fec_data_init()`, and the unconditional
  `cancel_delayed_work_sync()` ran on a zeroed work. The cancel now follows
  `s->fec`.
- The grouped RLNC coefficient schedule could leave a data share in no parity
  row. `tbt_fec_rlnc_coeffs()` picked `d` of `w` positions with splitmix64;
  for the production `n=4 m=4 w=8 d=4` shape a given share was absent from all
  four rows in 1/16 of blocks, so a block that lost one DATA share in flight
  (3/4 DATA + 4/4 PARITY) stayed undecodable, the super-block walk stopped at
  0, and `tx_ack` froze at one credit window. Coefficients are now a
  deterministic band: column `c` belongs to row `r` when
  `(c - r) mod m < band`, with the widest band that fits `d` terms and keeps
  rank `min(m, k)`. Every column is in at least one row; the `n4m4` matrix is
  the complement of the identity, so any subset of up to four data erasures is
  recoverable when all parity arrives, and a single lost share is covered by
  three of the four parity rows. The nonce no longer selects coefficients; no
  wire byte changed, but both ends must run this revision. QEMU probe N now
  drops every FEC record scheduled for carrier 0 (debug `drop_ppm`) before
  taking that carrier down, so the 16 MiB interleaved gate exercises the exact
  3 DATA + 4 PARITY block: byte-exact, `tx_ack` reaches 16 MiB on both ends,
  every c9 counter is zero, and the old codec stalls at
  `fec super 0 stuck ... shares=3/4 parity=4`.
- The same gate exposed a second loss path. A present short DATA share was
  copied into its S-byte slot without zeroing the tail, while the sender
  zero-pads that slot before encoding parity. The decoder XORs the whole slot
  into the right-hand side, so stale bytes from the slot's previous block
  corrupted every share recovered from a block that also held a short share;
  the transfer completed but `tbt_burst` reported a byte mismatch. The
  receiver now zero-pads the tail to S. The delivery length still comes from
  `dlen`/`short_len`, so no padding reaches the application.
- Per-carrier stream bookkeeping was 16 slots wide while a FEC v6 session has
  256 carriers, so every carrier from 16 up wrote past the end of its arrays:
  `announced_gen[c->index]` overwrote `tx_paused` with a carrier generation,
  `tx_fin_mask`/`tx_rst_mask`/`*_inflight` were one `unsigned long` and
  `set_bit()` on bit 16..255 wrote 30+ bytes past them, and `BIT(index)` on a
  u16 mask was truncated so a carrier's OPEN could never be recorded. The
  corruption showed up as an invalid `_Bool` load of `tx_paused` under UBSAN
  (`tbt_tx.c:2695`, `tbt_sock.c:96`, `tbt_fec_data.c:459`,
  `tbt_procfs.c:618/892`), the `WARN_ON_ONCE(!tbt_stream_announced())` at
  `tbt_tx.c:2570` firing on every carrier from 16 up, a stream that never
  delivered and a close that waited out its 60 s bound. All six are now
  `DECLARE_BITMAP(..., TBT_MAX_CARRIERS_LIMIT)` plus `announced_gen[256]`, with
  an index guard at each write and read. Probe N asserts that a 256-carrier
  transfer adds no kernel warning.
- A session with more than 16 carriers could not bring them up. `tbt_hdr_check()`
  capped the old header's `path_id` at `TBT_LEGACY_MAX_CARRIERS` (16), but
  `path_id` is a carrier index and the carrier layer has 256 slots, so the
  HELLO/OPEN/FIN/RST of carriers 16..255 failed their own encoding inside the
  HTTP container. The sender counted `http_parse_err` and the carrier was
  poisoned before a byte reached the wire, which looked exactly like a peer
  whose HTTP response could not be parsed: measured with `carriers=256`, `up=16`
  and `http_parse_err=240` (one per broken carrier, no HELLO rejection and no
  wire error). The 16-carrier limit now applies only to the old ARQ's per-path
  ack fields, whose mask really is 16 bits. Probe N runs the production shape
  (256 carriers, `n=4 m=4`, 32 groups): `up=256` on both ends, `http_req=256`,
  `http_resp=256`, `http_parse_err=0`, 1 MiB byte-exact.
- A second live stream in a `fec=on` session is refused instead of corrupting
  the first. The FEC record carries no stream id and the plane names one stream
  (`far_sid`), so two live streams had their bytes delivered to whichever was
  current: on the WAN smoke the near's control connection sent 167 bytes, the
  far acknowledged 37, iperf3's server failed its own cookie read with "Bad
  file descriptor" and the client idled out after 114 s. The near now resets the
  second connection (`fec_stream_refused`) and the first keeps its bytes. A
  two-connection protocol (iperf3) therefore cannot run over `fec=on`; a
  single-connection load tool is unaffected. A multi-stream FEC plane needs a
  stream tag in the record, which the frozen header does not have.
- The FEC v6 packetizer held a partial packet until 8192 bytes arrived, so an
  application that writes a small request and then waits for the answer waited
  forever: iperf3's 37-byte control cookie, an HTTP request, any
  request/response protocol. The bytes sat in the ring until the application
  closed the connection, and the close-time flush then delivered them to a
  peer whose application had already given up (measured: the T5.2 smoke,
  `submit_bytes=37`, far `rx_delivered=0`, target connected but silent, iperf3
  timed out after 180 s). The packetizer now flushes a partial packet after
  2 ms of silence with a dry ring, and closes its block with parity
  `win_len` = the block's real packet count, so the receiver can place a
  non-final short block instead of waiting for shares that are not coming. A
  short DATA packet is legal anywhere and only `LAST` marks the end of the
  stream. The receiver places shares by the sum of the payload lengths before
  them instead of the packet index times S, so a flushed packet leaves no
  hole, and it measures the offset from the byte at which the current stream
  began, so a second connection in the same session (iperf3's data connection
  after its control connection) starts at byte 0 again.
- Every carrier after the first one was refused with `-EPROTO` when
  `crypto=on` generated the session key. The far side adopted the key from the
  near's HELLO and then passed "we have a key" to `tbt_crypto_resolve_key()`,
  which read it as "we own the key" and demanded `key_id 0` from a peer that
  keeps announcing `key_id 1`. A session therefore ran on one carrier while
  the replacement loop burned the rest (measured: probe N, one carrier up and
  315 HELLO rejections in 4 s; the WAN smoke showed the same as
  `rx_proto=27` and one carrier up on each side). The session now records
  where the key came from (`crypto_key_peer`) and only a key this side
  configured or generated is treated as authoritative.
- A carrier dial that outlived one `TBT_CONNECT_TIMEOUT_MS` window was treated
  as a failure. A blocking `kernel_connect()` reports an expired
  `sk_sndtimeo` as `-EINPROGRESS`, not `-ETIMEDOUT`, and the socket is still
  handshaking; releasing it (RST) and dialling a new 5-tuple left the peer
  with an accepted socket waiting for a HELLO that could never arrive
  (measured: `connect_fail=41` with `-115`, one carrier replacing itself every
  second while the far logged `awaiting HELLO`). `tbt_connect_bounded()` waits
  again inside a 4 s budget and counts the dials that needed it
  (`connect_retry`).
- A fresh carrier allocated `parser` and `http_rx` with `kmalloc()` and never
  zeroed them; only the dead-pool reuse path cleared `parser->have`, and the
  chunk assembler was reset lazily on the first parsed chunk. A slab that
  came back dirty therefore made `tbt_rx_parse_pending()` enter
  `tbt_http_rx_parse()` with a huge `have`, and the `memmove()` that drains
  the parse buffer ran past it (8-carrier near: Oops in `__memcpy` from
  `tbt_http_rx_parse`, then dcache corruption in unrelated tasks). Both
  objects are now reset on every birth, new or reused; `tbt_carrier_release()`
  resets the assembler too; and `tbt_http_rx_parse()` refuses a carrier whose
  `have`/`http_have` exceed their buffers with `-EPROTO` instead of copying.
- The bring-up trace is now behind the module's `debug` parameter. `TBT_DIAG`
  and `TBT_DIAG_WARN` printed unconditionally, which at 256 carriers would
  emit a line per poll pass, per carrier and per HELLO on a production host.
  Both macros now expand to `if (debug >= 1)`: `debug=0` is silent and the
  arguments are not evaluated, while `debug>=1` still prints with
  `pr_info`/`pr_warn` and the `tbtun-diag:` prefix. Failures an operator must
  see at `debug=0` keep their own `pr_warn_ratelimited()` line: HTTP header
  alloc/build failure, carrier and control-frame allocation failure, FEC
  configuration rejection and arena failure, HELLO crypto parse/resolve
  rejection, HELLO write failure or abandonment, and a lost carrier. The one
  unconditional per-dial `DIALED` success line moved under the same gate.
- `tbt_frame_alloc()` left `fr->dropped` uninitialized. Only the `drop_ppm`
  injector sets it, but the pump reads it on every write to choose between
  `drop_snd_nxt` and `write_seq` for `first_sent_seq`; a `kmalloc()`ed frame
  could record the injector's mark for a frame that was never dropped, and
  the retention entry then looked acknowledged too early.
- A retransmit copy set `retx_copy` after `tbt_tx_enqueue()`, so the frame
  became visible with its state incomplete. The pump is the only consumer
  today and holds `pump_lock` across both calls, so the window is not
  reachable; the flag is now set before the enqueue, so the object is
  published initialized and a second consumer cannot read the wrong path mask
  or skip the `retx_frames` accounting.
- `tbt_tx_fbk()` read `c->sk` twice (`if (c->sk)`, then `tcp_sk(c->sk)`).
  `tbt_carrier_release()` clears the pointer with a plain store, so the
  second read could be NULL after the check. The function now takes one
  `READ_ONCE()` snapshot.
- A FEC share's `epoch` (the block's last real share length) was not bounded
  by S. The receiver stores it in `short_len` and uses it as a delivery
  length, so a peer could name up to 65535 and make
  `tbt_fec_deliver_ready()` read past the block's S-byte slot.
  `tbt_fec_hdr_check()` now rejects `epoch > max_payload`, and the delivery
  path clamps `len` to `rx->s` as a second gate.
- `tbt_rx_skb()` computed `sizeof(p->buf) - p->have` without the bound
  `tbt_rx_parse_pending()` applies. A dirty `have` left by a missed
  initialization made the subtraction underflow to a huge `take`, the copy
  failed with `-EFAULT`, and the carrier stopped reading. It now returns
  `-EPROTO` when `have > sizeof(p->buf)`.

### Known limitations

- The FEC path still has no NACK or selective retransmit. The codec now
  guarantees every single lost DATA share is covered (three of the four parity
  rows for `n4m4`), and the `n4m4` band recovers any subset of up to four data
  erasures when all parity arrives, but a block that loses a data share and
  enough parity rows still stalls until the stream closes. The designed safety
  net is a bounded NACK over the existing `TBT_FEC_NACK` body plus a
  per-session replay ring holding the last two credit windows; it is not part
  of this cut.

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

- **Sender window and the raw false-stall kill:** the send window was
  `rx_reorder_bytes / 2`, 128 KiB with the old 262144 default. On a 218 ms /
  17 Mbit path (BDP ~465 KiB) that held in-flight at 120–131 KB and throughput at
  3.79–4.55 Mbit/s while direct TCP on the same 5-tuple reached 17.08 Mbit/s;
  the window was the cause, not ISP shaping. The window is now a flow-control
  bound against the peer's contiguous delivered offset, sized once per pump pass
  as `k × mean BDP`, where `mean_BDP = mean_srtt × Σ BBR rate` and
  `k = (RTO + one_way + feedback_delay) / mean_srtt` (2–5 on the measured paths),
  with a 1 MiB bootstrap value (no sample yet), a two-frame liveness floor, a
  cap at `rx_reorder_bytes`, immediate growth and a 25% shrink hysteresis. The
  1 MiB value must not override `k × B` once a sample exists: a 70 KB BDP at
  k=2 would become a 15×-B window whose 3.4 s drain at 2.4 Mbit outlived the
  1.5 s hole budget and aborted the strict phase with EPIPE. `RTTVAR` is the largest per-carrier `mdev_us`,
  `one_way = mean_srtt / 2`, and the feedback delay is the measured uack lag,
  floored at the 10 ms FBK cadence. A sum of per-carrier BDPs, a max-RTT product
  and a cwnd sum are not bounds: the sender splits deterministically, so jitter,
  not a persistently slow carrier, sets the spread the reorder tree must absorb.
  The same SRTT + 4 × RTTVAR shape sets the sender RTO (200 ms floor, 2 ×
  feedback delay, 500 ms cap), and `hole_budget_ms` is sized at 2–3 × that RTO
  plus one one-way delay. The capacity identity is `R ≥ W = k × BDP`: the peak
  reorder occupancy is the whole window, and the 8 MiB default covers `k = 5` at
  218 ms / 17 Mbit. `tx_window_bytes=` (0 = auto, the default) pins it and must
  not exceed `rx_reorder_bytes`. `rx_reorder_bytes` now defaults to
  8388608 (was 262144), bounded per stream and by a session-wide aggregate of the
  same value. Carrier `SO_SNDBUF`/`SO_RCVBUF` and the frame queue
  (`txq_max = tx_window + 8 chunks`) scale with the window, and the buffers use
  `SO_SNDBUFFORCE`/`SO_RCVBUFFORCE` because the plain option is clamped to
  `net.core.wmem_max`/`rmem_max`. The raw-carrier stall clock now runs only
  while `snd_una != snd_nxt` and `snd_una` has not moved; an idle carrier, a
  delivery pause or a closed peer window resets it. The old code refreshed only
  on `snd_una` progress, so a long idle gap counted as stall time and the first
  DATA after it killed every raw carrier (WAN: `lost = n + m`,
  `replaced = n + m - 1`, `holes_unrecoverable = 0`; `http-kal` `lost = 0`), and
  the old `sk_wmem_queued > 0` test masked real stalls. `/proc/net/tbtun/peers`
  gains `delivery_rate_bps`, `cwnd_bytes` and `snd_wnd`; the `c3` line gains
  `tx_window`, `rtt_mean_us`, `rttvar_us`, `rtt_max_us`, `txq_max`,
  `stall_idle_skips`, `stall_declared`, `pump_hw_armed` and `pump_hw_fired`; the
  `live` line gains `car_sndbuf`. Local harness note: the single-guest QEMU
  bench terminates each carrier at libslirp 4.9.5, which does not parse
  `TCPOPT_WINDOW`, so its SYN-ACK carries no window scale and the local
  carrier is capped at 64 KiB / RTT = 2.40 Mbit/s at 218 ms (`snd_wnd=65535`,
  `wscale=0`, `rmem_alloc=0`). The tunnel delivered 2.35 Mbit/s, 98% of that
  cap: a harness artifact, not a module limit. A non-slirp link or the real
  WAN is required for a >1 MiB carrier window. The 256-carrier/1G
  target is ARM64 (1G physical port, 500-750 Mbit/s at the application).
  The `.74` host (x86_64, 4 x 1.8 GHz, AVX1, one queue) is the functional/1G
  candidate but unproven. The MT7621 MIPS endpoint (1004Kc, 880 MHz, no MSA,
  440 MiB, kernel 3.4.113, 300-500 Mbit/s CPU) is not that platform and needs
  a 6.1+ backport.
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
- **Review 23:** `rx_reorder_bytes=0` is refused with `EINVAL` instead of creating a
  tunnel that dies on its first reordered frame. The accepted range is
  `1..268435456`. `hole_budget_ms=0` now means auto (see above), and
  `carrier_stall_ms=0` likewise; both derive from the measured RTO. Zero remains valid where it has a defined meaning (`tx_retain_bytes`,
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
  `delay 20ms 10ms`. The one real-WAN measurement is the baseline the sender-window
  fix targets (218 ms / 17 Mbit: tunnel 3.79–4.55 Mbit/s at 120–131 KB in flight,
  direct TCP 17.08 Mbit/s); it is not a throughput expectation. Do not read these
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
