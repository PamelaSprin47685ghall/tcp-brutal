# Changelog

All notable changes to tbtun are recorded here. The wire revision is `TBT_CONV`
and moves independently of the module version.

## [Unreleased]

### Added

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

- The 30 s FEC hole timer no longer resets a stream by itself. It resets only
  when no carrier TCP is ESTABLISHED below the default `tcp_retries2`, or on a
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
- FEC wire: `TBT_FEC_CONV` moves from `0x1002` to `0x1004`. `0x1003` grows the
  ACK body from 16 to 24 bytes and carries `deliver_bytes`, the stream-relative
  byte count the receiver actually handed to its output socket. The first 16
  bytes keep their `0x1002` offsets. The sender's `tx_ack` is now that value
  instead of `next_deliver * S`: a short share or a block closed early makes
  the packet frontier name bytes that were never delivered, and the
  multiplication acknowledged them, so retention and the transmit window
  treated undelivered bytes as delivered. `0x1004` makes NACK a 64-byte bitmap
  repair request (Added above). 0.2.0 and this revision do not interoperate.

### Fixed

- FEC whole-block loss at the delivery frontier: the receiver only NACKed up
  to `highest_block`, so a super-block whose every share was lost was
  invisible and the sender's TX stall fired. The scan now probes the next
  super-block (parity 0 per group) even when no share of it was seen; the
  sender's replay ring states the shape. A stale final-block tail of 0 with a
  present DATA share no longer marks the block empty: the share is kept and
  parity/NACK states the real cut.
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
- FEC receive, bounded reset: the 30 s FEC timer no longer kills a stream by
  itself. A super-block that is still not ready after `TBT_FEC_STALL_MS`
  (30 s) resets only when every carrier TCP has failed; while a carrier TCP
  is alive the receiver keeps NACKing/replaying and the stream waits (see
  Changed above).
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

## [0.2.0] - 2026-11-07

### Changed

- 0.2.0 has flipped `TBT_CONV` to `0x0006` and `TBT_FEC_CONV` to `0x1002`
  (from 0x0005/0x1001 in 0.1.0). FIN carries an exactly 16-byte body
  (`pkt_end`, `pkt_base`); a PARITY record may be an empty-block marker
  (`win_len=0`, `payload_len=0`, `epoch=0`). 0.1.0 and 0.2.0 do not
  interoperate: the first HELLO is rejected by the conv check
  (`review/s2-merged-design.md` §6).
- Carrier lifetime: carriers are no longer reused from a dead pool.
  `tbt_carrier_get/put/gen` with a refcount and `kfree_rcu` replace
  `take_dead`, and `tbt_carrier_unpublish/close` release `c->sk` before the
  pointer is cleared (`review/s2-merged-design.md` §3).
- Session and stream lifetime: `tbt_session_get/put/arm/cancel` with
  the `drained` completion replace the dead `refs` field, and stream work
  holds one of five bits and is cancelled through `tbt_stream_schedule/cancel`
  (`review/s2-merged-design.md` §1, §5).

### Fixed

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
