# wire v9 staging plan

Tree: `w-wire-v9-gap` at `5b730dc` (0.8.0). Companion to
`docs/audit/wire-v9-gaps.md`. This document is a plan only: no protocol or
data-plane code is changed, and no runtime test is run.

Labels used below:

- **[verified]** read in this tree at the cited line.
- **[upstream]** Linux TCP behaviour that is not visible in this tree; the
  target kernel is not vendored here, so it carries a verification step.
- **[estimate]** arithmetic from the code constants, not a measurement.

The three premises of the expert proposal, restated:

1. every DATA/PARITY FEC record is a constant 1079 B on the wire;
2. DATA share `(g,s)` is pinned to carrier `g*(n+m)+s`, controls never touch a
   data carrier, so an offset is `base + block_id*1079`;
3. one FEC flow multiplexes 100+ application streams.

S1 targets (1) at the skb layer, S2 targets (2), S3 targets (1) for every
record type. Premise (3) is not staged here; its blockers are in the audit
(`E` section) and only the mux/limit work can move it.

## 0. Decision summary

| Step | What it buys | Necessary for throughput? | Necessary for injection? | Risk | Wire change |
| --- | --- | --- | --- | --- | --- |
| S1 | one 1079 B record per skb | no | only if substitution is per-skb | kernel-dependent, possibly impossible without a TCP patch | none |
| S2 | control/data carrier separation | no (removes a real failure mode) | yes | low, local scheduling | none |
| S3 | constant size for every FEC record | no | only if parity carriers are in scope | wire bump | yes |

Recommended order for the injection program: **S2, then the S3 decision, then
S1**. S1 is the lowest-risk experiment but the least useful result; it can run
in parallel with S2 because it changes no wire byte. None of the three should
gate the throughput work, which is orthogonal (section 5).

## 1. S1: one record equals one skb

### 1.1 What is already true [verified]

- One record per send call. Client: `send_record_to()` encodes one chunk and
  calls `carrier_queue()` once (`host/tbt_client_engine.c:519-536`). Module:
  `tbt_carrier_send_fec()` always calls the batch path with `count = 1`
  (`tbtun/tbt_carrier.c:1719-1726`); the batch builder would accept 3 records
  but no caller uses more (`tbtun/tbt_fec_batch.h:32-35`).
- Nagle is off on every carrier. Client `TCP_NODELAY`
  (`host/tbt_client_engine.c:399`); module `TCP_NAGLE_OFF`
  (`tbtun/tbt_carrier.c:3665`, `:3949`, `:4036`).
- There is no GSO/TSO/MSS control anywhere in the tree. A search for
  `gso_max_segs`, `sk_gso`, `NETIF_F_GSO`, `gso_size`, `sk_route_caps`,
  `TCP_MAXSEG`, `MSG_MORE` matches only `docs/prior-art.md`.

So the write pattern is already "one 1079-byte send per record, pushed
immediately". The open question is whether TCP puts that send in its own skb.

### 1.2 Options, one by one

| # | Knob | Layer / API | Per socket? | One record == one skb? | Other semantics | Throughput cost [estimate] | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| O1 | `TCP_MAXSEG = 1079` | socket option, userspace; kernel field `tp->rx_opt.user_mss` | yes | no, not alone: caps the segment size, not the skb size | advertised MSS, cwnd growth in segments, PMTU clamp | ~1 pp header, +35% packets | usable, insufficient alone |
| O2 | `sk_gso_max_size = mss`, `sk_gso_max_segs = 1` | `struct sock` fields, module-only | yes | likely yes, but route-owned and can be overwritten | disables TSO for this socket; more skbs per byte | pps +35%, TSO off CPU | usable, fragile, must be verified |
| O3 | `sk_route_caps &= ~NETIF_F_GSO` | `struct sock` field, module-only | yes | same as O2, weaker | route-owned; reset on dst change | as O2 | not recommended |
| O4 | `ethtool -K <dev> tso off gso off` | netdev, root | no | yes for that device | affects all traffic on the interface | potentially large at high rates | usable only for a lab proof |
| O5 | close the skb per record inside TCP | kernel patch | n/a | yes | none | none | **unavailable** |

Reasons and exact limits:

- **O1 / `TCP_MAXSEG`.** This is the supported per-socket way to bound the
  segment. It lowers the send MSS to 1079 and advertises 1079 to the peer.
  It does not by itself bound `size_goal`: with TSO on, TCP may still build a
  larger skb that contains several 1079-byte segments. It also cannot raise
  the MSS above the path MTU, so on a path with MTU 1400 it only lowers
  1360 → 1079.
  Client: add `setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, ...)` in
  `set_carrier_sockopts()` (`host/tbt_client_engine.c:393-406`).
  Module: the `tcp_sock_set_*` family is already used for keepalive
  (`tbtun/tbt_carrier.c:3423-3428`); `tcp_sock_set_mss()` is the natural call
  if the target tree exports it (check `Module.symvers`). If it is not
  exported, the module can write `tcp_sk(sk)->rx_opt.user_mss` directly and
  let the next `tcp_sync_mss()` apply it. That is an internal field, not an
  API contract; the module already writes `nonagle` and `sk_sndbuf` this way
  (`tbtun/tbt_carrier.c:3453-3455`, `:3665`).

- **O2 / GSO caps.** `sk_gso_max_size` and `sk_gso_max_segs` are plain fields
  of `struct sock`, so a module needs no exported symbol, but they are derived
  from the route by `sk_setup_caps()` and can be overwritten on a route or
  PMTU change. Setting them once after connect is therefore a convention, not
  a guarantee. `sk_gso_max_size = mss` is the field that bounds
  `tcp_xmit_size_goal()`; `sk_gso_max_segs = 1` bounds the TSO segment count
  of an skb. Both are worth setting, then verifying.

- **O3 / `sk_route_caps`.** The same fragility as O2 with a broader effect:
  clearing `NETIF_F_GSO` changes the socket's view of the route. It is reset
  whenever the destination is replaced (`sk_dst_set`, reconnect, PMTU event).

- **O4 / netdev features.** `ethtool -K` is a supported interface but it is
  not per socket and not per route. The client dials many WAN addresses; the
  carriers may use different devices; disabling TSO on all of them affects
  every other socket on the box. Use it only to prove the mechanism in a lab,
  not as a product setting.

- **O5 / kernel patch.** There is no per-write "close the skb" flag in TCP.
  Forcing one skb per record from inside the stack means changing
  `tcp_sendmsg_locked()` to start a new skb at each record boundary, or
  introducing a socket option that does so. That is a kernel patch; a stock
  module cannot do it, and an out-of-tree patch would have to be carried
  against every kernel the product supports. Mark **unavailable**.

### 1.3 The residual hole

Even O1+O2 do not give an API guarantee, for three independent reasons:

1. With GSO still enabled, `size_goal` can exceed 1079 and TCP may put
   several records into one skb; the prefix (HTTP header, 317 + len(Host)
   bytes, then HELLO, 104 bytes) does not stop that. Under `TCP_NODELAY` the
   header and HELLO are pushed before the first DATA write, so the prefix
   does not by itself split the first record; it only means the absolute
   stream position of the first record is not 1079-aligned.
2. A record can straddle the tail skb when the tail is only partly filled and
   still unsent: autocork holds it while the retransmit queue is non-empty,
   and qdisc backpressure can leave bytes queued. The next write fills the
   remaining room before a new skb is allocated.
3. `size_goal` has a kernel-version-dependent floor and is derived from the
   route (`sk_gso_max_size`), so a route or PMTU change can silently undo O2.

The invariant that *is* defensible with O1 alone is **one record == one TCP
segment**: with MSS 1079 each segment carries exactly one record, and the
byte offset arithmetic only needs segment boundaries to be irrelevant (it
works on the reassembled stream). "One record == one skb" is a stronger
property that only O4 or O5 can promise with certainty.

### 1.4 How to prove what actually happens [procedure, not run]

- **Segment count.** `getsockopt(TCP_INFO)` exposes `tcpi_segs_out` on modern
  kernels. Record the value before and after N records on one carrier: a
  ratio of exactly 1.0 (no retransmits, no control chunks on that carrier)
  proves one segment per record. `ss -ti` prints the same counter as
  `segs_out`.
- **Segment sizes.** `tcpdump -i <dev> -s 0 -w s1.pcap` on the carrier's
  device; every DATA segment payload must be 1079 B after the prefix. This
  proves segmentation, not skb identity: a GSO super-skb is split into
  segments before the capture point.
- **skb identity.** Use bpftrace on the transmit path, for example a kprobe
  on `__tcp_transmit_skb` or `tcp_set_skb_tso_segs` that reads `skb->len`,
  `skb_shinfo(skb)->gso_size` and `gso_segs`. Pass condition: every skb that
  carries DATA has `len == 1079` and `gso_segs == 1`; fail as soon as one skb
  has `len > 1079` or `gso_segs > 1`. A per-skb count over N records must
  equal N.
- **Write-queue shape.** A kretprobe on `tcp_sendmsg_locked` counts send
  calls; dividing by the skb count gives records per skb directly.
- Run the probe with data-plane control frames disabled on the carrier under
  test, so a HELLO or PAD chunk cannot be mistaken for a record. This is the
  same isolation S2 provides.

### 1.5 Throughput cost of S1 [estimate]

Assume IPv4 + TCP, 40 B of headers per segment, and a 1460 B path MSS.

- Header share: 40/1079 = 3.71% vs 40/1460 = 2.74%, i.e. the goodput ceiling
  drops by about 1 percentage point.
- Segments per payload byte: 1460/1079 = 1.353, so +35% packets for the same
  bytes. On a 100 Mbit/s carrier: 11.6 kpps instead of 8.6 kpps; on the
  uplink's 64 data carriers: 742 kpps instead of 548 kpps.
- Slow start: cwnd is counted in segments, so reaching the same byte window
  needs 35% more RTTs. On a 20 ms RTT this is a few extra RTTs, not a steady
  state cost.
- TSO off (O4) or a one-segment GSO cap (O2): the per-byte CPU cost rises with
  the segment count. At 742 kpps a modern x86 core is at roughly 0.5-1.5
  cores of softirq work depending on the driver; this is an estimate, not a
  measurement.

S1 does not reduce throughput through the FEC layer; it only makes the same
bytes arrive in smaller, more numerous packets.

### 1.6 Exit gate

- If the injection scheme can substitute bytes at stream offsets, **skip S1**:
  it is not needed and it costs ~1 pp plus CPU.
- If substitution is per skb, S1's probe decides between O1+O2 (works on this
  kernel) and O5 (needs a patch, i.e. the program is blocked). Do not start
  S2/S3 work on the assumption that O1+O2 works; run the probe first.

## 2. S2: control and data carriers separated

### 2.1 Inventory of every path that can put a non-DATA record on a data carrier

| # | Path | Site | Current behaviour | Minimal fix | Timing change |
| --- | --- | --- | --- | --- | --- |
| 1 | client `ctl_carrier()` fallback | `host/tbt_client_engine.c:492-512` | parity first, then any up carrier | return NULL when no parity carrier is up; keep the record in `e->pending` | OPEN/CFG/ACK wait for a parity carrier |
| 2 | client `send_record()` with `c == NULL` | `host/tbt_client_engine.c:551-566` | routes every control record through `ctl_carrier()`; DATA only if `carrier_at()` returned NULL | forbid the fallback when `control == false` | none (DATA never passes NULL today) |
| 3 | client OPEN/CFG trigger | `host/tbt_client_engine.c:2669-2673` | fires as soon as `up_count >= 1` | fire when a parity carrier is up (`ctl_up`) | + time for the first parity carrier |
| 4 | client HELLO per carrier | `host/tbt_client_engine.c:681-710`, `:754-769` | one HELLO on every carrier | cannot be moved without replacing per-carrier identity/profile checks; treat as a prefix | none |
| 5 | client empty marker | `host/tbt_client_engine.c:1149` | already pinned to the group's parity-0 carrier | keep | none |
| 6 | client PAD | none (`:2819-2820` only discards) | no PAD sender | keep | none |
| 7 | module control scan | `tbtun/tbt_fec_data.c:775-828` | every carrier from `ctl_rot` | build a parity-only candidate bitmap; park the record if empty | ACK/NACK wait for a parity carrier |
| 8 | module replay | `tbtun/tbt_fec_data.c:686-745` | least-backlogged carrier, any up carrier | replay only on the share's own carrier; if it is down, defer to the replacement epoch | block repair waits for the replacement |
| 9 | module empty marker | `tbtun/tbt_fec_data.c:1727` | control queue, any carrier | pin to the group's parity-0 carrier, like the client | none |
| 10 | module PAD | `tbtun/tbt_carrier.c:1071-1079` | one PAD per idle carrier | skip PAD on data carriers; rely on TCP keepalive (`TBT_KEEPIDLE_S 30`) | idle middleboxes may drop a data carrier after their own timeout |
| 11 | module HELLO | `tbtun/tbt_carrier.c:3779-3780` | one per carrier | prefix carve-out, same as #4 | none |
| 12 | module OPEN | `tbtun/tbt_tx.c:381-383`, `:392-398` | one per stream per carrier | either pin to a control carrier, or keep as a prefix before the first DATA | OPEN echo delayed |
| 13 | module FIN/RST | `tbtun/tbt_tx.c:518-520`, `:553-555` | one per carrier | same choice as OPEN; FIN is the more important one because it follows data | close latency |
| 14 | module response header | `tbtun/tbt_carrier.c:1620-1624`, `:1670-1675` | every carrier, before its first chunk | prefix carve-out | none |

Two paths do not fit the "no control on data carriers" sentence as literally
written, and the plan should not pretend otherwise:

- HELLO is per-carrier by design. Removing it means redesigning how the far
  side learns a carrier's index, generation and profile. The workable
  invariant is **no control frame after the first DATA chunk on a data
  carrier**; the HTTP header and HELLO form a prefix that the base offset
  accounts for.
- OPEN/FIN are per-stream per-carrier for a reason: a carrier that joins after
  the stream was announced must still learn the stream. Restricting them to a
  control carrier is possible only if the control carrier's failure triggers a
  re-announce on the replacement. This is a design decision, not a one-line
  fix. The cheap version is to keep them in the prefix for carriers that are
  already up, and to move them after the first DATA only for late joiners
  (which then need a fresh base anyway).

### 2.2 Startup delay and stall analysis

Client [verified constants]:

- Carriers are dialled in index order at startup
  (`engine_start_carriers()`, `host/tbt_client_engine.c:3126-3139`). The
  uplink shape places data carriers at `g*32 + 0..7`; the first parity
  carrier is index 8.
- The flow has a fixed 50 ms start delay (`TBT_OPEN_DELAY_NS`,
  `host/tbt_client_engine.c:63`) and then waits for the far side's OPEN echo
  (`flow_tx_ready()`, `:2495-2504`). The app's first data byte is gated on
  both.
- All 256 dials are issued concurrently in one loop, so the first parity
  carrier's handshake overlaps the first data carrier's. On a LAN the
  difference is sub-millisecond; on a WAN it is bounded by the connect plus
  HELLO round trip, i.e. roughly one RTT.
- Marginal first-byte delay of S2: `max(0, t_parity_up - 50 ms)` for the first
  flow. On a 20 ms RTT this is usually zero; on a 100 ms RTT it is on the
  order of 50-150 ms for the first flow. Later flows see zero: the parity
  carriers are already up.

Module [verified constants]:

- There is no 50 ms start delay. The receiver's ACK is what opens the
  sender's credit; an ACK cannot be sent if no parity carrier is up.
- A carrier that never presents HELLO is retired after
  `TBT_JOIN_TIMEOUT_MS = 5000` (`tbtun/tbtun.h:455`); one dial attempt is
  bounded by `TBT_CONNECT_TIMEOUT_MS = 1000` with several waits
  (`tbtun/tbt_carrier.c:50-53`).
- If no parity carrier comes up, the flow stalls at the credit window with no
  error. That is worse than a clean refusal.

Policy:

1. Add a session flag `ctl_up` set when at least one carrier whose index maps
   to a parity share (`index % (n+m) >= n`) is up.
2. Open and CFG are sent only when `ctl_up`. Data already waits for the OPEN
   echo, so no separate gate is needed.
3. If `ctl_up` is false, the flow must not fall back. Wait up to
   `T_ctl = TBT_JOIN_TIMEOUT_MS` (5 s). If it expires, reset the flow with a
   counted reason (`ctl_no_carrier`) and log once per session.
4. If a parity carrier dies mid-session and no other parity carrier is up,
   pause the TX and ACK paths (do not reroute) and start `T_ctl`. A pause is
   safe because the peer's credit cannot advance without ACKs; a reset after
   `T_ctl` is honest.
5. The 192-of-256 parity population makes a total parity outage a catastrophic
   event, not a normal one. The realistic trigger is the startup window and a
   middlebox that blocks the parity port range. Both are better handled by a
   clean failure than by a silent data-carrier insert.

### 2.3 What S2 does not fix

- The HTTP header and HELLO prefix remain on every data carrier. The offset
  base must be taken after them (audit section F). S2's invariant is about the
  steady-state stream after the prefix.
- The far side's response header remains on every carrier in the reverse
  direction.
- The kernel's `fc->slow` detector and socket-space check are unchanged; they
  still decide which parity carrier gets a control record.

### 2.4 Exit criteria

- A stream capture of a data carrier, after the prefix, contains only 1079 B
  DATA chunks with strictly increasing `block_id` and the expected group/share
  for that carrier index.
- No FEC control type (0x32-0x35) and no PAD (0x23) appears after the first
  DATA chunk on a data carrier.
- The startup probe with only data carriers up must not emit OPEN/CFG on a
  data carrier; it must hold them until `ctl_up`.

## 3. S3: constant size for every record type

### 3.1 The current marker and why it matters [verified]

The empty-block marker is a PARITY record with `payload_len = 0`, `win_len =
0`, 48 bytes, chunk 54 bytes (`tbtun/tbt_fec_data.c:1706-1735`; client
`host/tbt_client_engine.c:1126-1151`). It exists because a group with no
packet in a block must say so, otherwise the receiver waits for a block that
will never arrive.

On the client it is already sent to the group's parity-0 carrier
(`tbt_client_engine.c:1149`). In the module it is queued as control, so it can
land on a data carrier (`tbt_fec_data.c:1727`). **The cheapest way to keep the
1079-byte invariant on data carriers is S2 path #9: pin the marker to the
parity-0 carrier. No wire change is needed for that.**

A full-size marker costs 1025 extra bytes each. A short block with `g` groups
and one real share emits `g-1` markers (client seal,
`host/tbt_client_engine.c:2468-2469`). Uplink `g=8`: seven full-size markers
are 7,553 B instead of 378 B, i.e. +7,175 B per idle seal. At the 25 ms
idle-seal cadence (`TBT_TX_IDLE_SEAL_NS`, `host/tbt_client_engine.c:66`), a
1-byte-per-25 ms application would generate about 287 KB/s of extra padding.
That is the cost of option A/B below.

### 3.2 Protocol options

**Option A: a new FEC type, e.g. `TBT_FEC_EMPTY 0x36`.**

- Layout: 48-byte header + 1024 zero bytes, `win_len = 0`, `payload_len =
  S`, `share_index = n`, `epoch = 0`, valid header and payload CRC.
- Receiver: on this type, mark the group `marker` for `block_id`, as the
  current 48-byte marker does.
- Old receiver: `tbt_fec_record_type()` only accepts 0x30..0x35
  (`tbtun/tbt_uapi.h:272-275`), and `tbt_fec_hdr_check()` requires the
  current `TBT_FEC_CONV` (`tbtun/tbt_fec.c:368`). An old peer either rejects
  the type or the conv and poisons the carrier. This is a hard break.

**Option B: reuse PARITY with a new flag bit, e.g. `TBT_FEC_FF_EMPTY 0x04`.**

- Layout: PARITY, `flags = 0x04`, `payload_len = S`, `win_len = 0`, body
  zeros.
- Receiver: the flag means "empty, full-size"; the existing `win_len == 0`
  marker path learns a second spelling.
- Old receiver: `tbt_fec_hdr_check()` rejects any flag outside
  `TBT_FEC_FF_MASK` (`tbtun/tbt_fec.c:370`). Hard break, same as A. The
  0.8.0 PADDED cut is the precedent: the new flag is exactly what an old
  receiver refuses (`tbtun/tbt_uapi.h:122-127`).

**Option C: keep the 54-byte marker, forbid it on data carriers.**

- No wire change. The marker stays on a parity carrier, where its size does
  not affect any data-carrier offset.
- This satisfies claim (1) as "every DATA record on a data carrier is 1079 B"
  and leaves parity carriers with a small variable record. If the injection
  scheme also indexes parity carriers by `block_id*1079`, C is not enough.

Recommendation: **C**. It is part of S2 (#9), costs no wire change and no
padding. Choose A or B only if parity carriers must also be fixed-cell.

### 3.3 Wire version bump

The repository's own rule is explicit:

- `tbtun/tbt_uapi.h:7-9`: "Any change to a wire value is a protocol change:
  bump `TBT_CONV` with it."
- `tbtun/tbt_fec.h:11-13`: "Any change to an offset, a length, or a type byte
  is an ABI change: bump `TBT_FEC_CONV`."
- `tbtun/tbt_uapi.h:107-109`: on a wire cut, `TBT_CONV`, `TBT_FEC_CONV` and
  HELLO `body[0]` move together.
- The HELLO gate compares `body[0]` exactly (`tbtun/tbt_proto.h:95-112`), and
  the first record's conv is checked before any other field
  (`tbtun/tbt_proto.c:77`), so an old peer fails cleanly at HELLO rather than
  misparsing.

Therefore:

- **Option A (new type):** bump `TBT_CONV 0x000b -> 0x000c`,
  `TBT_FEC_CONV 0x100b -> 0x100c`, and `TBT_WIRE_VERSION_V6 8 -> 9`. All
  three, because a type byte is an ABI change and the session gate must refuse
  old peers at the first HELLO.
- **Option B (new flag):** bump `TBT_CONV 0x000b -> 0x000c` (the session
  gate). `TBT_FEC_CONV` may stay `0x100b` if the 48-byte layout and every
  offset are unchanged, which is the 0.8.0 PADDED precedent. Bumping the
  HELLO version byte to 9 is optional but gives a cleaner refusal; without it
  an old peer dies on the first FEC record's flags instead of at HELLO.
- **Option C:** no bump. It changes scheduling, not bytes.

### 3.4 Exit criteria

- Every FEC record a data carrier can see is exactly 1079 B, and the first
  non-DATA byte after the prefix never appears.
- If A or B is chosen, a mixed-version pair fails at the first HELLO with a
  counted reason, and the golden vectors are regenerated from the frozen
  release tag. (`tests/golden_v080/` was dropped on 2026-11-12: it had no
  reader, the generator ran against the live tree, and the tree had moved
  from 0.8.0 to 0.10.0, so it could no longer certify the 0.8 wire.)

## 4. Deterministic pinning versus dynamic dispatch: the quantified trade

### 4.1 The two designs

- **Dynamic (today, module).** DATA records wait in one session FIFO and the
  drain writes each record on the first carrier with socket space, starting
  from `async_rot` (`tbtun/tbt_fec_data.c:951-1112`). The `carrier` computed
  by the packetizer is kept only for the debug loss injector
  (`tbtun/tbtun.h:1229-1232`).
- **Strict (today, client near).** `carrier = group*(n+m)+share`
  (`host/tbt_client_engine.c:1058`, parity at `:1112`), one DATA chunk per
  data carrier per block.

### 4.2 Model

Let `N_d` be the number of data carriers per block (uplink 64 = 8 groups x 8
shares; downlink 128 = 1 group x 128 shares), `S = 1024` payload bytes,
`K = 7` blocks of credit (client ACK: `(TBT_RX_SLOTS-1)` blocks,
`host/tbt_client_engine.c:1306-1316`), `R_i` the RTT and `B_i` the bandwidth
of carrier `i`.

Strict pinning: every data carrier must deliver exactly one chunk per block.
With `K` blocks in flight, the per-block interval is

```
T_block = max_i max( R_i / K , S / B_i )
throughput = N_d * S / T_block
latency floor = max_i R_i
```

Dynamic: the same global window of `N_d * K` data chunks is placed on whatever
carriers have room. The interval is set by the carriers actually used:

```
T_block ≈ max( R_eff / K , N_d * S / sum_i B_i )
throughput ≈ N_d * S / T_block
latency floor ≈ R_eff
```

`R_eff` is the effective RTT of the used set; with the module's slow/stall
skip it tends towards the fast carriers, but only after a refusal marks a
carrier slow (`TBT_SKIP_MS = 20`, `tbt_fec_data.c:249`). A slow carrier with a
large socket buffer can still absorb records for a while, so the real gain is
between "no gain" and the full `R_max/R_eff` ratio.

### 4.3 Numbers for the production shapes [estimate]

Homogeneous, `R = 20 ms`, `B = 100 Mbit/s` per carrier:

- uplink: `T_block = 20/7 = 2.86 ms`, throughput = 64 x 1024 / 2.86 ms =
  22.9 MB/s (183 Mbit/s), same for both designs;
- downlink: 128 x 1024 / 2.86 ms = 45.8 MB/s (367 Mbit/s), same for both.

One RTT outlier, `R = 200 ms`, other 63 carriers at 20 ms, bandwidth ample:

- strict: `T_block = 28.6 ms`, uplink 2.29 MB/s (18.3 Mbit/s), downlink
  4.58 MB/s (36.6 Mbit/s);
- dynamic with the slow carrier skipped: 22.9 / 45.8 MB/s, i.e. about 10x
  better;
- tail latency: strict's block latency floor is 200 ms every block; dynamic's
  is 20 ms once the outlier is skipped.

One bandwidth outlier, 1 Mbit/s among 63 x 100 Mbit/s:

- strict: `T_block = S/B = 8.2 ms`, uplink 8.0 MB/s (64 Mbit/s);
- dynamic: window-limited at 22.9 MB/s (183 Mbit/s), about 2.9x better with
  the current K=7. If `K` were raised to 66, the gap would widen to roughly
  27x, because strict remains `N_d * min B_i` while dynamic approaches the
  window or the sum of the fast carriers.

The statistical point matters as much as the outliers: strict takes the
maximum over 64 (or 128) carriers every block, so the p99 of the maximum is
paid on every block even when no carrier is persistently bad. Dynamic takes
the maximum over the carriers that actually carry the block, and can avoid a
persistently bad one.

### 4.4 Alternatives that keep determinism and recover load balance

**A. Strict mapping + carrier replacement (recommended).**
Keep `c = g*(n+m)+s`; monitor each carrier's RTT/loss/delivery from the
existing ACK/FBK path; if a carrier is persistently below the median, RETIRE
it and join a replacement at the same index (the mechanism already exists:
`tbt_carrier_lost()`, `tbtun/tbt_carrier.c:2555-2601`). Determinism holds
inside an epoch; the replacement starts a new TCP stream and a new base.
Throughput returns to the homogeneous case; cost is replacement churn and one
block of repair while the replacement joins. This is the only option that
keeps the closed-form offset and fixes a persistent outlier.

**B. Block-rotated permutation.**
`c = g*(n+m) + ((s + f(block_id)) % (n+m))`, with `f` fixed and known to both
sides (for example `block_id % (n+m)`). Each carrier still receives exactly
one chunk per block, so `base + block_id*1079` survives unchanged: only which
share sits on which carrier moves. It spreads transient loss/congestion
across carriers, but a persistently slow carrier still gets `1/(n+m)` of the
shares, so it does not recover the min-rate bound.

**C. Weighted epoch map announced in CFG.**
Allow carrier `i` to own `w_i` shares per block, with `sum w_i = n`, and
announce the map at block-boundary epochs. This is deterministic and can
achieve sum-of-rates, but the offset is no longer a closed form: the receiver
needs `offset(c, block) = base + block*1079 + extra_chunks(c, block)`, i.e. a
per-epoch map plus cumulative extras. It is the honest option if both exact
offsets and full load balance are mandatory, at the cost of a new wire
contract and more receiver state.

**D. Raise the window.**
The client's `TBT_RX_SLOTS` fixes `K = 7`. Raising it scales both designs
equally in the homogeneous case, but it scales dynamic's advantage in the
heterogeneous case because strict is still `min B_i`-bound. Memory cost is
about 2 MiB of RX block buffers per 8 slots (`host/tbt_client_engine.c:1881`,
`:1546`), plus the replay ring. This is an orthogonal throughput lever, not a
substitute for pinning.

Recommendation: keep the module's dynamic dispatch for throughput. If the
injection program needs determinism, use **A** (strict + replacement) and
accept the epoch boundary, or **B** if only transient outliers matter. Choose
**C** only if the program cannot tolerate a min-rate carrier at all.

## 5. Final ordering

For **throughput**, none of S1-S3 is on the critical path. The throughput
levers are the window (`K`), the credit/pacing loop, and the slow-carrier
handling; they are orthogonal to pinning and should not wait for any stage
here. Strict pinning would *reduce* throughput whenever the carrier set is
heterogeneous, so do not adopt it as a throughput measure.

For the **injection program**:

1. **S2 first.** It removes a real failure mode (control frames on data
   carriers), fixes the marker/PAD placement, and is the only stage that can
   be done without a wire break. Its startup delay is bounded by one carrier
   handshake, usually absorbed by the client's 50 ms start delay; with a 5 s
   join timeout it cannot hang forever.
2. **Decide S3 vs "S2 only".** If parity carriers are out of scope, S2 path
   #9 is enough and no wire bump is needed. If they are in scope, choose
   option B (flag) over A (type) and bump `TBT_CONV` at least; budget the
   padding cost in section 3.1.
3. **S1 last, and only if substitution is per skb.** Run the probe before
   writing any code. If the probe shows one skb per record with O1+O2, the
   stage is a small socket-option change. If it does not, the stage requires
   a TCP patch and must be escalated as a program-level blocker; it is not a
   reason to delay S2.

If the user's original S1 -> S2 -> S3 order is kept for scheduling reasons,
run S1 as a parallel experiment: it changes no wire byte and its outcome does
not affect S2 or S3. Do not let its possible "needs a kernel patch" result
stop S2, which is independently valuable.

## 6. What this plan does not decide

- Whether the product's near side is the userspace client or the module. The
  pinning and the control-carrier logic differ between them (audit section C);
  S2 lists both.
- Whether 100+ streams are handled by the mux or by raising the flow limit.
  The audit's E section shows both paths are blocked today; that is a separate
  design decision and is not part of S1-S3.
- The exact `sk_gso_*` behaviour of the target kernel. O2's "likely" verdict is
  the one item that must be checked against the target tree before S1 is
  scheduled as a code change; the check is `Module.symvers` for the
  `tcp_sock_set_*` family plus the S1.4 probe for the skb shape.
