# Coexistence with ordinary TCP: measurement design

0.8.0 Step 3 gives the near side's uplink a deterministic shape: one share per
block on a fixed carrier, control frames only on parity carriers, and a
parity-recovered share replayed into its carrier's own receive queue. None of
that adds bytes to the wire, but it does change *when* those bytes are sent
and how quickly the receiver acknowledges them. A bottleneck shared with
ordinary TCP therefore needs a measured answer to one question:

> How much throughput does a competing plain TCP flow lose while a tbtun
> uplink runs beside it?

This document fixes the topology, the load, the metrics and the pass/fail
thresholds, so the number is comparable across builds. It is a design, not a
benchmark result: no measurement is claimed here.

## Topology

```
sender ns ── veth ── [ tc: one bottleneck ] ── veth ── receiver ns
   │                                                        │
   ├── tbtun near (n carriers, FEC on)                      ├── tbtun far
   └── iperf3 client (plain TCP)                            └── iperf3 server
```

- One `veth` pair, both ends in their own netns. The bottleneck is a `tbf`
  qdisc (or `netem rate`) on the sender's egress, sized to the link under
  test: 10, 50 and 200 Mbit/s.
- Queue depth (`limit`) is fixed per run and reported: 1x and 4x the
  bandwidth-delay product. A deep queue hides unfairness behind loss; a
  shallow queue exposes it as retransmission.
- Delay from `netem delay` on the same egress: 5 ms and 50 ms, so the two
  regimes (window-limited, rate-limited) are both covered.
- Both directions' carrier bytes go through the same bottleneck. The far
  side's downlink is idle except for ACKs, so the measured share is the
  uplink.
- CPU: sender and receiver each pinned to distinct cores. `irqbalance`
  stopped. TSO/GSO left at the distribution default and recorded, because a
  change there moves the per-segment cost and the fairness number.

## Load

| Run | tbtun uplink | plain TCP |
| --- | --- | --- |
| B0 (baseline) | off | 1 flow, unlimited |
| B1 | off | 2 flows, unlimited |
| A1 | 1 stream, 8 KiB records, FEC `8/8` | 1 flow, unlimited |
| A2 | 1 stream, FEC `8/24` | 1 flow, unlimited |
| A3 | 1 stream, FEC `8/8`, `debug_loss=1 drop_ppm` at 1 % | 1 flow, unlimited |
| A4 | 4 streams, FEC `8/8` | 1 flow, unlimited |
| A5 | 1 stream, FEC `8/8`, carrier count 8 vs 64 | 1 flow, unlimited |

`B0` is the denominator for every ratio. `B1` separates "tbtun is unfair" from
"two flows are unfair", which matters because the uplink is `n` TCP
subflows and multipath aggregation is unfair by construction.

Each run lasts 60 s after a 10 s warm-up; the first 5 s of the steady state
are discarded so slow start and carrier bring-up do not enter the median.

## Metrics (sampled once per second)

- `T_b`: plain flow goodput, bytes/s, from the receiver's counters.
- `T_u`: tbtun goodput (application bytes at the far side), bytes/s.
- `W_u`: tbtun bytes written to the carriers, bytes/s. `W_u / T_u - 1` is the
  uplink's wire overhead: FEC redundancy plus framing plus control records.
- `RTT_b` p50/p99 of the plain flow (from `ss -tin` or the receiver's
  timestamps).
- `retrans_b`: the plain flow's retransmissions per second.
- `J`: Jain index over per-subflow delivered bytes, including the plain flow
  as one entry. Reported separately for (all tbtun carriers + plain) and
  (aggregate tbtun + plain).
- `ACK_lat_u`: far-side `fec_ack_lat_ns` and `ul_pin_inject`, so a change in
  acknowledgment timing is visible next to the fairness number.

Derived numbers, which are the ones the review reads:

```
loss_b   = 1 - T_b(A*) / T_b(B0)
overhead = W_u / T_u - 1
share_u  = W_u / (W_u + W_b)
```

## Pass/fail

The contract is about the competing flow, not about tbtun's own throughput:

1. `loss_b <= 0.15` for every A run at 50 Mbit/s, and `loss_b <= 0.25` at
   10 Mbit/s. A single-lane link is where multipath aggregation bites; the
   looser bound at 10 Mbit/s acknowledges that the bottleneck is only a few
   MSS deep.
2. `RTT_b` p99 must not exceed its `B0` value by more than one bottleneck
   queue's standing delay (the queue depth divided by the rate). A larger
   increase means tbtun's carrier writes are building a standing queue that
   the plain flow pays for.
3. `retrans_b` must not grow by more than 2 percentage points over `B0`.
4. `J` over (aggregate tbtun + plain) at or above 0.9. Below that the uplink
   is taking more than its share, whatever the absolute numbers say.
5. `overhead` is reported, not bounded: `8/24` is expected near 0.33. A value
   far above `m/(n+m)` points at control or keepalive traffic that should be
   on a parity carrier and is not.

A run that fails 1 or 4 blocks the release; 2 and 3 are investigated and
recorded.

## What each Step 3 change is expected to move

- **Deterministic pinning.** Data carriers no longer carry ACK/NACK/FBK/PAD
  or drip records, so the per-carrier byte stream has one writer and one
  record shape. Expect `RTT_b` p99 to drop relative to the rotating dispatch,
  and `T_u` to be flatter (no record hopping between sockets).
- **Control frames on parity carriers only.** Control bytes still exist; they
  are now concentrated. With `m = 8` of 16 slots, that is half the carriers
  carrying twice the control rate. Watch `W_u` and the parity carriers'
  `wmem_queued`; a parity carrier that is also the least-loaded replay target
  can queue control behind repairs.
- **Replay into rcv_nxt.** No extra bytes reach the wire (the segment was
  already sent once). The effect is timing: the far side ACKs a hole about
  one carrier RTT earlier than TCP retransmission would, which moves `T_u`
  up and leaves `T_b` unchanged in the rate-limited regime. In the
  window-limited regime it can briefly open the receive window sooner, so
  `RTT_b` is the metric to watch.

## Harness sketch

```
# one run: B0 or A*
ip netns exec snd tc qdisc replace dev veth0 root tbf rate $R burst 32k \
    limit $LIMIT
ip netns exec snd tc qdisc replace dev veth0 parent 1:1 handle 10: netem \
    delay $RTT
./scripts/build.sh && insmod tbtun.ko ...
tbtun_near ... fec=on carriers=$N n=$N m=$M &
iperf3 -c $FAR -t 60 -J > plain.json &
# tbtun payload: 60 s of 8 KiB records at line rate, one stream
sleep 10   # warm-up, then sample for 50 s
```

Every run records: kernel version, module commit, `tc -s` at the end,
`/proc/net/tbtun/stats` and `/proc/net/tbtun/peers`, and the iperf3 JSON.
The comparison script takes the median of the steady-state samples and prints
`loss_b`, `overhead`, `share_u`, `J`, `RTT_b` p50/p99 and `retrans_b` in one
row per run.

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

**Not measured yet.** This document is the design. No coexistence number is
claimed in this release; the run matrix above must be executed on the target
kernel before the thresholds mean anything.

## Why this is not a microbenchmark

The interesting failures are queueing and aggregation effects: a control
record on a data carrier delays the next share by a full socket buffer, and
`n` subflows grow their windows independently. Neither shows up in a single
flow's throughput. The design above therefore always measures the *other*
flow, at two queue depths and two RTTs, with and without FEC redundancy, and
reports the multipath share separately from the aggregate share.
