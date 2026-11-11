# Appendix: coexistence with ordinary TCP (optional)

**Status: optional appendix. Design only, never measured, not a release
gate.** The target deployment is metered bandwidth, where coexistence with
third-party TCP flows that share a bottleneck is not a requirement. Nothing
in this repository is accepted or rejected on the numbers below; the release
does not depend on them.

This appendix exists for two reasons. First, if someone later wants to know
how the uplink behaves beside a plain TCP flow, the method and the units are
written down once, so two runs are comparable. Second, the replay has a real
correctness boundary — the unprovable LAST bit — and that boundary is recorded
here. The boundary applies; the measurement does not gate anything.

## What this appendix is not

- It is not an acceptance criterion, a threshold, or a release checklist.
- It makes no throughput claim. No run has been executed, and no number in
  this file is a result.
- It does not constrain the data plane. A measurement that looked bad would
  be a reason to investigate, not a reason to change the protocol.

## Optional method, if someone chooses to measure

### Topology

```
sender ns ── veth ── [ tc: one bottleneck ] ── veth ── receiver ns
   │                                                        │
   ├── tbtun near (n carriers, FEC on)                      ├── tbtun far
   └── iperf3 client (plain TCP)                            └── iperf3 server
```

One `veth` pair, both ends in their own netns. The bottleneck is a `tbf`
qdisc (or `netem rate`) on the sender's egress. Both directions' carrier
bytes go through it; the far side's downlink is idle except for ACKs, so the
interesting share is the uplink.

Record what changes the per-segment cost, because it changes any number:
queue depth (`limit`), added delay (`netem delay`), TSO/GSO state, CPU
affinity and whether `irqbalance` is running.

### Optional load matrix

| Run | tbtun uplink | plain TCP |
| --- | --- | --- |
| B0 | off | 1 flow, unlimited |
| B1 | off | 2 flows, unlimited |
| A1 | 1 stream, 8 KiB records, FEC `8/8` | 1 flow, unlimited |
| A2 | 1 stream, FEC `8/24` | 1 flow, unlimited |
| A3 | 1 stream, FEC `8/8`, `debug_loss=1 drop_ppm` at 1 % | 1 flow, unlimited |
| A4 | 4 streams, FEC `8/8` | 1 flow, unlimited |

`B0` is the plain flow's own baseline. `B1` separates "tbtun is unfair" from
"two flows are unfair", which matters because the uplink is `n` TCP subflows
and multipath aggregation is unfair by construction.

If a run is made, 60 s after a 10 s warm-up with the first 5 s of the steady
state discarded is a reasonable window; that choice is a convention, not a
rule.

### Optional observations

- `T_b`: plain flow goodput, bytes/s.
- `T_u`: tbtun goodput (application bytes at the far side), bytes/s.
- `W_u`: tbtun bytes written to the carriers, bytes/s. `W_u / T_u - 1` is the
  uplink's wire overhead.
- `RTT_b` p50/p99 of the plain flow.
- `retrans_b`: the plain flow's retransmissions per second.
- `J`: Jain index over per-subflow delivered bytes, reported separately for
  (all tbtun carriers + plain) and (aggregate tbtun + plain).
- `ACK_lat_u`: far-side `fec_ack_lat_ns` and `ul_pin_inject`, so a change in
  acknowledgment timing is visible next to the other numbers.

Derived quantities, if a report is written: `loss_b = 1 - T_b(A)/T_b(B0)`,
`overhead = W_u/T_u - 1`, `share_u = W_u/(W_u + W_b)`. They are descriptive
only; this appendix defines no pass or fail value for any of them.

## Uncovered cases

**A lost last packet with no stream end is not replayed.** The sender sets the
LAST bit on the stream's final DATA record only. Neither the parity shares nor
the block state carry that bit, and the field is inside the header CRC, so a
guessed value would produce a self-consistent record that is not the bytes the
peer sent. A share recovered from parity is therefore replayed only once the
stream end is known (`rx->total_known`, set by this stream's LAST or its FIN),
which makes the bit exact. Until then the share waits for the carrier's own
TCP retransmission. This is a deliberate coverage hole, not an oversight: the
uplink's repair is best-effort for the final packet of a stream, and correct
by construction for every packet before it.

**The end belongs to one stream.** `total_known`/`total_pkts` are written once
per RX plane by that plane's own LAST or FIN, and no runtime path resets them.
A plane serves exactly one stream: detach/abort unlinks the flow and shrinks
its arena, and the next stream allocates a new flow with a new plane. An
arriving LAST therefore never changes what an earlier stream meant, and a
later stream on the same sid cannot inherit the previous end. A LAST that
arrives while an earlier gap is still open does not change the flag's meaning
either: the gap delays `next_deliver`, while `total_pkts` still names this
stream's final packet, so every replayed `seq < total_pkts` keeps the right
bit.

The one intra-flow switch path (`rx->rebase_pending`) has no writer today.
If it is enabled later, the arena reset must also clear `total_known` and
`total_pkts`; `tbt_fec_rx_shrink(reset = true)` does not do that.

**Not covered by the replay at all:** the raw profile (`profile == 3`, no
chunk positions), ARQ sessions (`fec=off`), and the near side's downlink. The
near side keeps its adaptive dispatch; only the far side's uplink receive
path is pinned and replayed.

**Not measured.** No coexistence run has been made for this release. The
method above is a suggestion for whoever needs the number, not a gate that
this or any later release must clear.

## Why a single number would mislead

The interesting failures are queueing and aggregation effects: a control
record on a data carrier delays the next share by a full socket buffer, and
`n` subflows grow their windows independently. Neither shows up in one flow's
throughput. Any optional run should therefore report the other flow and the
aggregate share separately, rather than one "fairness" figure.
