# wire v9 gap audit: fixed-cell framing, deterministic pinning, mux capacity

Tree: `w-wire-v9-gap` at `5b730dc` (0.8.0, `TBT_WIRE_VERSION_V6`).
Method: static reading only. Every claim below cites the file and line in this
tree. No runtime test was run, and no code was changed.

Two senders exist and they do not behave the same way:

| Sender | File | DATA routing |
| --- | --- | --- |
| userspace client, near or far role | `host/tbt_client_engine.c` | pinned to `group*(n+m)+share` |
| kernel module, near or far role | `tbtun/tbt_fec_data.c` | first carrier with send space, `async_rot` cursor |

So "the near side pins DATA" is true only when the near side is the userspace
client. The module's near role load-balances DATA across all carriers.

## Verdicts

| Claim | Verdict |
| --- | --- |
| A. Every DATA/PARITY record is 1079 B on the wire | **GAP** — DATA and full PARITY are 1079 B; the empty-block PARITY marker is 54 B |
| B. One 1079 B chunk is one skb | **BLOCKING** — no GSO/TSO control exists, and one record is not one segment |
| C. No control frame ever rides a data carrier | **BLOCKING** — the module's control/replay/PAD/HELLO/OPEN/FIN all reach data carriers; the client falls back to data carriers |
| D. Per-carrier block_id is dense, increasing, never reused | **GAP** — increasing within one flow, but short blocks and drops leave holes; replay appends old ids; a reconnect starts a new TCP stream; flows have independent id spaces |
| E. One FEC flow multiplexes 100+ app streams | **BLOCKING** — host caps at 8 flows, the module admits exactly 1 FEC flow per session, the mux is not wired |
| F. Nothing else invalidates the offset arithmetic | **GAP** for uplink (prefix only), **BLOCKING** for downlink (mid-stream PAD) |

## A. Constant wire size

**CONFIRMED for DATA, GAP for the claim as written.**

The arithmetic is right. `TBT_FEC_HDR_LEN` is 48 and `TBT_FEC_PAYLOAD_MAX` is
1024 (`tbtun/tbt_uapi.h:283-306`), so a full record is 1072 bytes. The chunk
size line is lowercase hex with no leading zeros followed by CRLF
(`tbtun/tbt_http.c:875-898`): 1072 = 0x430 gives `430\r\n`. One chunk is
`size line + record + CRLF` (`tbtun/tbt_http.c:1241-1253`,
`tbtun/tbt_fec_batch.c:47-67`), so 5 + 1072 + 2 = 1079 bytes. `S` is a
compile-time constant, `TBT_FEC_S = TBT_FEC_PAYLOAD_MAX` (`tbtun/tbt_fec.h:57`),
not a per-session value.

Both senders pad a short DATA slot and keep the real length in `win_len`:

- module: `tbtun/tbt_fec_data.c:2004-2010` sets `TBT_FEC_FF_PADDED`,
  `h.win_len = len`, `h.payload_len = TBT_FEC_S`, and zero-fills the tail;
- client: `host/tbt_client_engine.c:1023-1038` does the same.

`tbt_fec_make_record()` emits `48 + payload_len` bytes, and the PADDED rule
requires `payload_len == max_payload` (`tbtun/tbt_fec.c:414-422`), so every
DATA record is exactly 1072 bytes. A non-empty PARITY record also sets
`payload_len = TBT_FEC_S` (`tbtun/tbt_fec_data.c:1700-1705`).

The exception is the empty-block marker. It is a PARITY record with
`payload_len = 0`, `win_len = 0`, emitted when a group has no packet in a
block (`tbtun/tbt_fec_data.c:1706-1735`; client `host/tbt_client_engine.c:1126-1151`).
That record is 48 bytes, its chunk is `30\r\n` + 48 + CRLF = **54 bytes**, and
it is queued on the control path (`tbt_fec_send_record(..., true, ...)` at
`tbtun/tbt_fec_data.c:1727`), so it can ride any carrier. Therefore "every
DATA/PARITY FEC record is a constant size" is false. If the invariant is
narrowed to "DATA records on data carriers", it holds.

Other records that share the same carrier containers and have their own sizes:

| Record | Record bytes | Chunk bytes | Site |
| --- | --- | --- | --- |
| DATA, short or full | 1072 | 1079 | `tbt_fec_data.c:2004-2010` |
| PARITY, non-empty | 1072 | 1079 | `tbt_fec_data.c:1700-1705` |
| PARITY, empty marker | 48 | 54 | `tbt_fec_data.c:1706-1735` |
| HELLO v6 | 98 (32+66) | 104 | `tbt_uapi.h:552`, `tbt_carrier.c:3779` |
| OPEN | 68 (32+36) | 74 | `tbt_uapi.h:506`, `tbt_tx.c:381-383` |
| FIN | 48 (32+16) | 54 | `tbt_uapi.h:523`, `tbt_tx.c:519-520` |
| RST | 32 | 38 | `tbt_uapi.h:571-575`, `tbt_tx.c:553-555` |
| PAD | 32 | 38 | `tbt_http.h:99-101`, `tbt_carrier.c:1071-1079` |
| ACK | 72 (48+24) | 78 | `tbt_uapi.h:328`, `tbt_fec_data.c:3664` |
| NACK / CFG | 112 (48+64) | 118 | `tbt_uapi.h:333,359`, `tbt_fec_data.c:3695,3750` |
| FBK (FEC) | 112 (48+64) | 118 | `tbt_uapi.h:414`, `tbt_fec_data.c:5716` |

So the fixed-cell property is real for DATA, but the carriers are not a stream
of 1079-byte cells. The receiver accepts every size from 48 to 1072 and routes
by the record type byte (`tbtun/tbt_http.c:1290-1330`).

## B. Per-skb framing

**BLOCKING for any skb-level substitution. Irrelevant for stream-level
arithmetic, which is the only arithmetic claim (2) needs.**

There is no GSO/TSO/MSS control anywhere in the tree. A search for
`gso_max_segs`, `sk_gso`, `NETIF_F_GSO`, `gso_size`, `sk_route_caps`,
`TCP_MAXSEG` and `MSG_MORE` matches only `docs/prior-art.md`. The client's
`set_carrier_sockopts()` sets exactly `TCP_NODELAY`, `SO_SNDBUF`,
`SO_RCVBUF`, `TCP_NOTSENT_LOWAT` and `SO_KEEPALIVE`
(`host/tbt_client_engine.c:393-406`). The module sets `TCP_NAGLE_OFF` and
writes `sk_sndbuf`/`sk_rcvbuf` directly (`tbtun/tbt_carrier.c:3453-3455`,
`:3665`, `:3949`, `:4036`) and touches no GSO field.

One record is one socket write, but a write is not a segment:

- module: one record per `tcp_sendmsg_locked()` (`tbtun/tbt_carrier.c:1516-1585`,
  called from `tbt_carrier_send_fec()` at `:1719-1726`). The batch builder
  supports 3 records per write (`tbtun/tbt_fec_batch.h:32-35`), but no caller
  uses `count > 1`;
- client: one `write()` per 1079-byte frame, with a partial-write loop
  (`host/tbt_client_engine.c:443-465`, `:519-536`).

The kernel can merge two writes into one skb (autocork appends to the tail skb
while the retransmit queue is non-empty), and with TSO a single skb carries
many MSS-sized segments. A single 1079-byte record can also be split at an MSS
or page boundary inside `tcp_sendmsg_locked()`. `DESIGN.md:881-886` states the
same contract: "There is no `TBT_MSS` and no 'one chunk == one segment'
requirement ... GSO may merge chunks into larger skbs without changing
framing."

Consequence: a scheme that substitutes bytes at the skb level has no invariant
to stand on. A scheme that parses the reassembled TCP byte stream (claim (2)
as written) does not need one.

## C. Control-frame pinning

**BLOCKING. The claim is false in both senders, for different reasons.**

### C.1 Userspace client (near)

`ctl_carrier()` returns the first up, non-dead carrier in
`[e->tx_n, e->n_car)` — the parity range — and otherwise scans
`[0, e->n_car)` and returns the first up carrier, which can be a data carrier
(`host/tbt_client_engine.c:492-512`). The comment at `:496-498` says this is
deliberate: "The kernel accepts a control record on any carrier, so fall back
to any up carrier rather than block the session."

Every control record goes through `send_record(e, NULL, ...)`, which calls
`ctl_carrier()` (`host/tbt_client_engine.c:551-566`). Call sites include OPEN
(`:1231`), FIN (`:1256`), RST (`:1272`), CFG/FBK (`:653-675`, `:1299`,
`:1467`), ACK (`:1333`), NACK (`:1970`). If `send_record_to()` returns
`-EAGAIN`, control is parked in `e->pending` and `pending_flush()` resolves it
through `ctl_carrier()` again (`:568-583`).

When it triggers:

1. Startup. `flow_tick()` sends OPEN and CFG as soon as `e->up_count` is
   non-zero (`host/tbt_client_engine.c:2669-2673`). Carriers are dialled in
   index order (`engine_start_carriers()`, `:3126-3139`), and the uplink shape
   puts data carriers at indices `g*32 + 0..7`; the first parity carrier is
   index 8. The first flow therefore usually emits OPEN and CFG on a data
   carrier before any parity carrier is up.
2. Any window where no parity carrier is up. With 192 of 256 carriers in the
   parity range, this is rare in steady state but is exactly the loss case the
   design cares about.
3. `pending_flush()` uses the same fallback, so a parked ACK can land on a
   data carrier later.

The client never sends PAD (`host/tbt_client_engine.c:2819-2820` only discards
one on receive), but it sends HELLO on every carrier, including data carriers,
right after the request header (`carrier_connected()`,
`host/tbt_client_engine.c:754-769`; `carrier_send_hello()` `:681-710`).

DATA is not affected by the fallback in the production shape:
`carrier_at()` returns a non-NULL slot for every index below `n_car = 256`,
and `groups*(n+m) = 256`, so `send_record(e, carrier_at(...), ..., false)`
never passes NULL (`host/tbt_client_engine.c:1058,1064`).

### C.2 Kernel module

The module has no parity preference at all:

- control: `tbt_fec_out_try()` scans every carrier from `tx->ctl_rot`,
  wrapping over `sess->n_car`, and writes on the first up, non-stalled one with
  socket space (`tbtun/tbt_fec_data.c:775-828`). The comment at `:777-781`
  says "Control records (ACK and NACK) may ride any up carrier";
- replay: `tbt_fec_out_try_replay()` picks the least-backlogged carrier from a
  snapshot of every up carrier (`tbtun/tbt_fec_data.c:686-745`); the comment
  at `:690-691` says "The carrier is not part of the record's identity";
- PAD: pinned to each carrier's own index, so every idle carrier, data
  carriers included, receives a 38-byte PAD chunk every `keepalive_ms`
  (`tbtun/tbt_carrier.c:1071-1079`);
- HELLO: one per carrier, pinned by index (`tbtun/tbt_carrier.c:3779-3780`);
- OPEN: one per stream per carrier, `tbt_tx_announce()` loops over all
  carriers (`tbtun/tbt_tx.c:381-383`, `:392-398`);
- FIN/RST: one per carrier (`tbtun/tbt_tx.c:518-520`, `:553-555`);
- empty-block marker: queued as control, so it rides any carrier
  (`tbtun/tbt_fec_data.c:1727`).

Quantitatively, in the production downlink shape (`n=128, m=128, groups=1`,
half the 256 carriers are data), every control record has roughly a 50% chance
of landing on a data carrier. On the uplink shape (`n=8, m=24, groups=8`,
64 data carriers of 256) the share is 25%. This is the normal path, not an
edge case.

**Consequence for the arithmetic:** any byte the far side adds to a data
carrier's stream between two DATA chunks shifts every later offset. The
client's fallback and the module's round-robin both do exactly that.

## D. block_id monotonicity per carrier

**GAP. Increasing within a flow, but not dense, not unique per carrier, and
not preserved across replay or reconnect.**

`block_id` is `tx->cur_block`, one counter per FEC flow, shared by every group
(`tbtun/tbt_fec_data.c:1957`; `tbtun/tbtun.h:927`). It advances by exactly one
at a block boundary (`tbtun/tbt_fec_data.c:1946-1948`, `:2315-2317`) and on a
short cut (`:1799`). It is never reset. The client has the same counter per
flow, starting at 0 (`host/tbt_client_engine.c:1163-1165`, `:2473-2475`).

Three separate things break density on a carrier:

1. **Short blocks skip shares.** The client's idle seal sends only the partial
   share of the current group, parity for that group, and empty markers for
   the remaining groups (`host/tbt_client_engine.c:2442-2469`). Data carriers
   whose share index is at or above the group's `win_len` get no chunk at all
   for that block. The module behaves the same way (`tbt_fec_tx_emit_parity()`
   sends `count = tbt_fec_tx_parity_count(win_len, ...)` rows,
   `tbtun/tbt_fec_data.c:1585-1712`; a group with no packet emits the 48-byte
   marker at `:1727`).
2. **Drops.** The client drops a DATA record when the pinned carrier's 128 KiB
   write buffer is full: `send_record_to()` returns `-EAGAIN`, and
   `flow_send_data()` returns 0 (`host/tbt_client_engine.c:519-536`,
   `:1064-1065`). There is no per-block retry before the next block, so the
   slot is missing. The module drops a record only when no carrier has room,
   and keeps it queued otherwise, but it can also lose the flow's whole queue
   at teardown.
3. **Replay.** A NACK replay in the client re-sends the old record on the same
   scheduled carrier, appended after newer blocks because `carrier_queue()`
   writes to the tail (`host/tbt_client_engine.c:2361-2366`, `:462-483`). The
   module replays on any carrier (`tbtun/tbt_fec_data.c:690-745`). Either way
   an older `block_id` can follow a newer one on the same carrier.

Carrier death and reconnect:

- client: `carrier_close()` closes the fd, clears `wbuf`, the header state and
  the HELLO flags (`host/tbt_client_engine.c:826-837`); `carrier_connect()`
  dials again and `carrier_connected()` writes a fresh request header and
  HELLO (`:773-815`, `:754-769`). Unsent records in the old `wbuf` are lost.
  Block ids continue, but the byte stream starts over, so the old base is
  invalid. Worse, a DATA write to a carrier already marked `dead` returns
  `-ENOTCONN`, `flow_tx_feed()` returns an error, and the client sends RST and
  frees the flow (`host/tbt_client_engine.c:2599-2603`): one dead carrier can
  kill the application stream even though 255 carriers are healthy.
- module: `tbt_carrier_lost()` marks the carrier stopping, sends RETIRE on
  another carrier, and arms a replacement at the same index with a new
  generation (`tbtun/tbt_carrier.c:2555-2601`, `:2115`). Block ids and the TX
  plane continue; there is no epoch re-anchor. In-flight records are
  re-dispatched to other carriers by the FIFO drain, which is possible only
  because DATA is not pinned.

Finally, with more than one client flow on the same carriers, each flow has
its own `block_id` sequence starting at 0. On one carrier, chunks from flow A
block 5 and flow B block 5 are indistinguishable by block id; the offset
formula would have to be per `(sid, carrier)`. The module side cannot even
reach that state because it admits one FEC flow (section E).

## E. Mux capacity

**BLOCKING. 100+ concurrent connections is not supported by any current path.**

Client-side constants, `host/tbt_client_engine.c:52-58`:

```
TBT_FLOW_MAX       8
TBT_RX_SLOTS       8
TBT_REPLAY_BLOCKS  8
TBT_RX_CODECS      4
```

These are not just slot counts. `TBT_RX_SLOTS` fixes the receive window at
8 blocks, of which one is under delivery, so an ACK grants 7 blocks
(`host/tbt_client_engine.c:1306-1316`). `TBT_REPLAY_BLOCKS` fixes the sender's
replay ring at 8 blocks and caps unacknowledged blocks at 7
(`host/tbt_client_engine.c:2533-2548`). The module's equivalents are
`TBT_FEC_RX_SLOTS 66` (`tbtun/tbt_fec_data.c:233`), replay K=66
(`tbtun/tbt_fec.h:791`), and `TBT_STREAMS_MAX 128` / `TBT_PENDING_MAX 128`
(`tbtun/tbtun.h:349-350`); the FEC flow count is the binding limit, not those.

`flow_alloc()` returns NULL when all 8 slots are taken
(`host/tbt_client_engine.c:869-875`). The accept path then does
`close(fd); continue;` (`:2992-2998`): the connection is accepted and
immediately closed, with no log and no queueing. That is what the 9th
concurrent connection gets on the client. On the far role the same limit
applies: `handle_open()` returns `-ENOMEM` for a 9th flow
(`host/tbt_client_engine.c:2125-2127`).

The module's real limit is one FEC flow per session:
`tbt_fec_flow_admit_mask()` and `tbt_fec_flow_admit()` both contain
`if (sess->n_fec_flows) return ERR_PTR(-ENOSPC)` (`tbtun/tbt_fec_data.c:8764-8768`,
`:8813-8818`). `tbt_sock.c:993-1003` maps that `-ENOSPC` to `-EOPNOTSUPP`, and the
pending path aborts the socket (`tbt_sock.c:1090-1099`). So with a module far side the second
concurrent stream already fails; the 9th is never reached.

At the time of this audit the mux existed but was not wired (0.9 removed
`tbt_mux.c`, `tbt_mux.h` and its host test; this section is kept as history):

- `tbtun/tbt_mux.h:1-82` defines an 8-byte subframe (`cmd`, `slot`, big-endian
  `u16 len`, big-endian `u32 stream_id`), `TBT_MUX_STREAMS 64`
  (`tbt_mux.h:66`), slots 0..7 (`:70`), and a parser that never allocates;
- a search for `tbt_mux_encode`, `tbt_mux_parser_push`, `tbt_mux_flow_open`
  and `tbt_mux_tx_charge` finds definitions in `tbtun/tbt_mux.c` and
  declarations in `tbtun/tbt_mux.h` only. No production caller exists.

Per-flow cost, if `TBT_FLOW_MAX` is raised (client, from
`host/tbt_client_engine.c:885-890`, `:973-1000`, `:1546`, `:1881`,
`:1628-1640`):

- `struct flow` inline ~66 KiB (`replay[8]`, `rx_codec[4]`, `rx[8]`,
  `rbuf[65536]`);
- `tx_plane` = 256 × 1024 = 256 KiB;
- replay ring = 8 × (256 × 1072) ≈ 2.19 MiB, lazily allocated;
- RX block buffers up to 8 slots × 256 shares × 1024 ≈ 2 MiB;
- output buffer up to 64 MiB (`:1628-1640`).

That is roughly 4.5 MiB of steady buffers per flow, plus the output buffer.
The module's per-flow cost is similar: a TX ring of 256 KiB initial, up to
16 MiB (`tbtun/tbt_fec_data.c:212-213`, `:8335-8341`), group buffers of
`groups*(n+m)*S` = 256 KiB, a replay ring sized to K=66 (about 2.4 MiB in the
1x256 shape, `tbtun/tbt_fec.h:791`, `tbtun/tbt_fec.c:702-745`), RX descriptors
for 66 slots (`tbtun/tbt_fec_data.c:225-240`), and lazy block planes of up to
66 x 256 x 1024 = 16.5 MiB for the 1x256 shape
(`tbtun/tbt_fec_data.c:8264-8310`, `:8379-8385`, `:8397-8420`).

A mux-based design would pay those costs once and multiplex app streams in
subframes, but `TBT_MUX_STREAMS` would have to be raised above 100, and the
subframe's `u16 len` caps each DATA frame at 1024 bytes, so the sender must
fragment and the receiver must reassemble. None of that exists today.

## F. Other invalidators of the offset arithmetic

**GAP for the uplink prefix, BLOCKING for the downlink.**

1. **HTTP header on every carrier.** The client writes a full POST block on
   every carrier immediately after connect, including data carriers
   (`host/tbt_client_engine.c:724-752`, `:754-769`). The module writes the
   same block lazily before the first record on each carrier
   (`tbtun/tbt_carrier.c:1642-1712`, `:1774`, `:1857`). The block's length is
   fixed except for the `Host` value, which is the peer literal
   (`tbtun/tbt_http.c:281-303`; `tbt_http.h:53-55`; only the Host value
   varies, the 26-byte request id is fixed). The far side finds the body by
   scanning for CRLFCRLF (`tbtun/tbt_http.c:859-872`; module `tbtun/tbt_rx.c:1038-1058`;
   client `host/tbt_client_engine.c:2832-2872`). A per-carrier
   `rx_body_start_seq` must therefore be taken after this header, and it is
   not a compile-time constant across deployments.
2. **HELLO chunk before the first DATA chunk.** 104 bytes on every carrier
   (`host/tbt_client_engine.c:681-710`; module `tbtun/tbt_carrier.c:3779`).
   Also part of the prefix.
3. **OPEN/CFG on a data carrier at start.** The client sends OPEN and CFG as
   soon as one carrier is up (`host/tbt_client_engine.c:2669-2673`), and data
   is gated on the far side's OPEN echo (`flow_tx_ready()`,
   `host/tbt_client_engine.c:2495-2504`; `handle_open()` `:2110-2121`). So
   OPEN/CFG normally land in the prefix, before the first DATA chunk. They
   shift the base but do not break a base anchored at the first DATA body.
4. **Response header in the other direction.** The far side writes its 200
   block on every carrier before its first chunk (`tbtun/tbt_carrier.c:1620-1624`,
   `:1670-1675`; client `host/tbt_client_engine.c:744-747`), so the
   far→near stream has its own prefix.
5. **PAD mid-stream (downlink).** In FEC mode drip is disabled but PAD is not:
   the condition at `tbtun/tbt_carrier.c:1071-1078` does not test `sess->fec`.
   Every idle carrier gets a 38-byte PAD chunk on its own stream. A PAD can
   therefore appear between block b and block b+1, shifting every later
   offset by 38. For the far→near direction this is fatal, not a prefix
   problem.
6. **The receiver does not enforce the mapping.** The client's
   `handle_fec()` discards the carrier pointer (`(void)c;`,
   `host/tbt_client_engine.c:2177`) and stores a share by `sid/block/share`
   (`:2222`). The module's RX routes by `sid` and validates `n/m` and
   `share_index` but never checks that the record arrived on
   `group*(n+m)+share`. A control record on a data carrier is accepted
   silently. Nothing today can detect a pinning violation, let alone recover
   from one.

## Smallest changes that would make (1) and (2) true

Ordered by increasing risk. Items 1-3 are local to the client and do not
change the record format.

1. **Never let DATA fall back, and never kill the flow on a carrier error.**
   Keep `send_record(..., control=false)` from calling `ctl_carrier()` when the
   pinned carrier is missing or dead; queue that share for the carrier's
   reconnect instead. Stop returning `-EIO` from `flow_tx_feed()` on a
   per-carrier `-ENOTCONN` (`host/tbt_client_engine.c:551-566`, `:2599-2603`).
   Without this, one dead carrier breaks both the stride and the stream.
2. **Make control wait for a parity carrier.** Change `ctl_carrier()` to
   return only indices in `[tx_n, n_car)`, and let the pending queue hold
   control until one is up (`host/tbt_client_engine.c:492-512`, `:568-583`).
   This removes the fallback inserts without touching the wire. It does not
   fix the kernel sender (item 5) and does not remove the HELLO prefix.
3. **Define the base after the prefix and reset it on reconnect.** Track one
   `body_start` per carrier after the HTTP header and HELLO, and reset it
   when a new request block is parsed. This is receiver-side state only, and
   it makes the 1079 stride exact for a carrier that has no other inserts.
4. **Eliminate mid-stream inserts on data carriers.** Two sub-options:
   (a) never drop a DATA record: block or queue the share for its own carrier
   instead of dropping it at `send_record_to()`'s `-EAGAIN`
   (`host/tbt_client_engine.c:519-536`), and never replay a share onto the
   data-carrier stream (a replay must be a byte-for-byte refill of the same
   slot, or the block must be re-sent on a new stream);
   (b) keep drops and replay, but accept that `block_id*1079` is invalid and
   use a per-carrier chunk counter instead.
   (a) preserves the design claim; (b) gives it up.
5. **Make the module pin DATA and control.** This is the largest change: the
   module's DATA FIFO drain must stop choosing carriers by send space
   (`tbtun/tbt_fec_data.c:951-1112`) and write each share to its scheduled
   carrier, with the same no-drop/no-fallback rules; control and replay must
   be limited to parity carriers; PAD must not be sent on data carriers.
   Until this happens, claim (2) is true only for a userspace near side.
6. **Make the empty-block marker constant-size, or keep it off data
   carriers.** The 48-byte PARITY marker (`tbtun/tbt_fec_data.c:1706-1735`)
   can be sent on any carrier through the control queue. If it must stay
   legal on a data carrier, the protocol needs a full-size empty record
   (PADDED DATA with `win_len = 0` is currently rejected,
   `tbtun/tbt_fec.c:414-430`; the marker is the only defined empty form).
7. **One flow per carrier set, or per-(sid, carrier) offsets.** Today the
   client can run 8 flows whose independent block-id sequences interleave on
   the same carriers. A single muxed FEC flow (claim 3) is the cleaner fix;
   otherwise every offset must be keyed by `(sid, carrier)` and the receiver
   must keep per-flow state.
8. **Wire the mux and raise the limits.** `TBT_MUX_STREAMS` 64 → at least 128,
   a production caller in `tbt_rx.c`/`tbt_tx.c`, and a decision on whether the
   module admits more than one FEC flow. This is the only path to 100+
   concurrent connections, and it is a design change, not a bug fix.

## What this audit did not establish

- No packet capture was taken, so the 1079-byte and 54-byte sizes are derived
  from the encode paths, not observed on a wire.
- The exact probability of the client's control fallback depends on carrier
  bring-up order at the far side; the audit establishes the mechanism and the
  trigger conditions, not a measured rate.
- The module's `n=128, m=128` downlink shape was read from
  `host/tbt_client_engine.c:3199-3260` and the 0.8 shape helpers; it was not
  exercised.
