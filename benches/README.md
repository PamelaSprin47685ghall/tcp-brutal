# benches - the performance envelope of tbtun

This directory answers two questions with numbers, not with the design's
intentions:

1. Does a tunnel of `N` shaped carriers actually go faster than one flow, and
   how much does the tunnel itself cost?
2. What do the HTTP container, prediction, the jitter buffer, and the carrier
   count cost in throughput, CPU, latency, and memory?

Every number below was produced by a run of `benches/run.sh` on this machine
and is stored, row for row, in `benches/results/*.csv`. Anything not measured
is labelled as such.

## What the run does

`benches/run.sh <scenario>` boots one QEMU guest (KVM, kernel 7.2.9, 2 vCPUs,
512 MiB), loads `tbtun.ko`, and runs one scenario through the module's operator
surface only (`echo` into `/proc/net/tbtun/*`). The host has no passwordless
`sudo`, so a guest is the only way to load the module; the same reason
`tests/qemu/README.md` gives.

The topology is the probe-I topology:

```
near tunnel (listen 127.0.0.1:19100)
   carriers: 10.0.2.15:4100x -> 10.0.2.2:29101
      eth0 egress, root qdisc: 10 Mbit htb class per source port + netem
         slirp -> hostfwd -> guest 10.0.2.15:19101
            far tunnel -> target 127.0.0.1:19102 (echo)
```

Inside the guest, `tc` installs one htb class per source port in
41000..41031, each at 10 Mbit with a netem child (`delay 20ms 10ms` unless a
scenario says otherwise), and one u32 filter per port. The module's carrier
block is 41000..41015 (16 ports, `max_carriers`), so `n=16` uses the block
exactly. The extra ports 41016..41031 exist so the control flow and the direct
latency flow can be shaped exactly like a carrier without ever being picked by
the module.

Only the near-to-far direction is shaped: the far listener's replies leave
with port 19101, which falls into the unshaped default class. That isolates
the per-5-tuple limit, as probe I does.

### Measurement points

| What | Where it is read | What it includes |
| --- | --- | --- |
| tunnel rate | sum of the near tunnel's `peers` `tx_bytes` over a 2 s window | frame headers and HTTP container bytes, not TCP/IP headers |
| control rate | the control's htb class `Sent` bytes over a 2 s window | TCP/IP headers too (about +2.7% at 1500 B segments) |
| app rate | the near tunnel's `submit_bytes` over the same window | application payload captured from the public socket |
| overhead | `tx_bytes` delta / `submit_bytes` delta | container + frame + retransmit + duplicate bytes |
| frames/s | sum of `peers` `tx_frames` over the window | records written, control and PAD included |
| CPU | `/proc/stat` system+irq+softirq ticks over the whole transfer | percent of one vCPU (2 vCPUs are available, so 200% is the ceiling) |
| kworker CPU | utime+stime of every `kworker*` over the transfer | workqueue context, i.e. the module's deferred work |
| queue peaks | `/proc/net/tbtun/stats` and `peers` sampled every 250 ms | `txq`, `rxq`, out-of-order bytes, retained bytes, speculative bytes, in-flight bytes |
| guest memory | `MemTotal - MemAvailable`, process RSS sum, `/proc/slabinfo` | sampled at 1 s |
| host RSS | `/proc/<qemu-pid>/status` `VmRSS` sampled at 200 ms | the guest's resident memory as the host sees it |

The control is `dd if=/dev/zero bs=64k | nc -p <shaped port> 10.0.2.2:29103`,
with a busybox `nc -lk` sink behind hostfwd. It is one TCP flow on one shaped
5-tuple, measured at the qdisc, which is the same layer the carrier counters
sit above. The rate window starts after 256 KiB has crossed the class; the
transfer is complete when the class has carried the whole volume (not when
`dd` returns, because `close()` does not wait for the socket buffer to drain).

The tunnel transfer is `host/tbt_burst.c`'s strict phase: 40 MiB sent with
blocking `send()`, then read back and compared byte for byte. A row with
`strict=1` is therefore also a correctness result. The guest raises
`tcp_rmem`/`tcp_wmem` to 64 MiB first, exactly as probe I does, so the strict
phase can hold the echoed volume it has not read yet.

### What is not measured

- A real ISP classifier. The 10 Mbit htb classes are a stand-in for a
  per-5-tuple policer; they say nothing about whether an ISP prioritises HTTP.
  DESIGN.md §17 is the measurement for that, on a real line.
- TLS, `framing=ext`, `http-kal` on a middlebox that rewrites bodies.
- Hosts other than this one. The absolute Mbit/s numbers are a property of
  this QEMU/slirp path as much as of the module.
- More than 16 carriers: the port block and `max_carriers` are both 16.

## Running

```sh
benches/run.sh throughput --n 4 --mib 40      # one carrier count, one boot
benches/run.sh throughput --n 1 --mib 40      # the sweep is one boot per N
benches/run.sh profile --only http-kal        # raw | http-kal | http-kal-drip
benches/run.sh loss --loss 50000              # one drop_ppm level
benches/run.sh latency --iters 10000
benches/run.sh memory --mib 40
benches/run.sh scale --n 16
benches/run.sh throughput --quick             # 8 MiB, 1 s window, ~1 minute
```

**One configuration per boot is deliberate.** `echo flush` into
`/proc/net/tbtun/config` was observed to block after a completed 40 MiB
transfer (and after a latency stream closes), so a second measured row in the
same guest never finishes. Each invocation therefore measures exactly one
configuration; the sweep is the sequence of invocations, and each writes its
own CSV. `benches/results/` holds the recorded sweep; the tables below use,
for each `(scenario, row)`, the newest run of that row (older repeats are kept
as raw data).

The recorded sweep (2026-11-05) was:

```sh
benches/run.sh throughput --mib 40 --n 1 --tag rec
benches/run.sh throughput --mib 40 --n 2 --no-control --control-mbit 2.61 --tag rec
benches/run.sh throughput --mib 40 --n 4 --no-control --control-mbit 2.61 --tag rec
benches/run.sh throughput --mib 40 --n 8 --no-control --control-mbit 2.61 --tag rec
benches/run.sh profile --mib 40 --only raw --tag rec
benches/run.sh profile --mib 40 --only http-kal --tag rec
benches/run.sh profile --mib 40 --only http-kal-drip --tag rec
benches/run.sh loss --mib 40 --loss 0 --tag rec
benches/run.sh loss --mib 40 --loss 10000 --tag rec
benches/run.sh loss --mib 40 --loss 50000 --tag rec
benches/run.sh loss --mib 40 --loss 100000 --tag rec
benches/run.sh latency --iters 10000 --loss-iters 2000 --tag rec
benches/run.sh memory --mib 40 --tag rec
benches/run.sh scale --mib 40 --n 8 --tag rec
benches/run.sh scale --mib 40 --n 16 --tag rec
```

`--no-control` skips the control measurement in a later throughput boot and
`--control-mbit` carries the control value forward, so the ratio stays
defined without paying 130 s for another control.

Useful options: `--mib N`, `--n N`, `--only PROFILE`, `--loss PPM`,
`--iters N`, `--loss-iters N`, `--netem 'delay|5ms|2ms'`, `--window S`,
`--tag NAME`, `--timeout S`. `--quick` sets 8 MiB, a 1 s window, and 2000
latency iterations unless the option was given explicitly.

Each invocation writes `benches/results/<stamp>-<scenario>[-<tag>].csv` and
`...-carriers.csv`, prints a table, and leaves the console log in
`~/.cache/tbtun-build/bench/`. Runs are serialised with `flock`: a second KVM
guest on this box would perturb both. If a foreign `qemu-system` process is
already running, the run warns and records the count in
`host_qemu_others`; treat those rows as contended.

Requirements: `qemu-system-x86_64`, `/dev/kvm` writable, the kernel image and
busybox from `tests/qemu/README.md`'s cache, and `/usr/bin/tc` (the guest's
busybox `tc` has no u32 filters). `make check` must have produced
`./tbtun.ko`.

## CSV columns

One uniform header for every scenario; a scenario fills the columns it
measures and leaves the rest empty.

| Column | Meaning |
| --- | --- |
| `stamp`, `scenario`, `row` | run time, scenario, row name (`n4`, `profile-http-kal`, `loss-50000`, `lat-tunnel-n4`, ...) |
| `n`, `m`, `profile`, `mib` | tunnel configuration of the row |
| `drop_ppm`, `dropped_by_ppm` | injector setting and the records it actually dropped on carriers 1-2 (near side) |
| `iters`, `bytes` | latency iterations and request size |
| `control_mbit` | control flow's qdisc rate |
| `tunnel_mbit` | tunnel's carrier rate (same layer as probe I) |
| `ratio` | `tunnel_mbit / control_mbit` |
| `goodput_mbit` | the row's measured one-way rate (control or tunnel) |
| `app_mbit` | application payload rate over the same window |
| `wall_s` | wall time of the whole transfer |
| `wire_bytes`, `app_bytes`, `wire_per_app` | whole-transfer carrier bytes, captured app bytes, and their ratio |
| `frames`, `frames_s` | records written by the near side and their rate |
| `cpu_sys_pct`, `cpu_softirq_pct`, `cpu_total_pct` | kernel CPU over the transfer, percent of one vCPU |
| `kworker_pct` | `kworker*` CPU over the transfer, percent of one vCPU |
| `retx_ppm`, `dup_tx`, `inflate_ppm` | retransmitted frames per million, duplicates written, inflation |
| `holes_declared`, `holes_repaired`, `holes_unrecoverable` | prediction's hole accounting (near tunnel) |
| `rx_max_stall_us` | longest app-visible stall before a hole was filled or reset |
| `reorder_max_depth` | deepest out-of-order backlog, frames |
| `carriers_up` | carriers schedulable at the end of the row, `up/rows` |
| `txq_max`, `rxq_max` | peak module queues, bytes |
| `ooo_bytes_max`, `retain_bytes_max`, `spec_bytes_max` | peak reorder tree, replay retention, speculative bytes |
| `mem_before_kb`, `mem_peak_kb`, `mem_after_kb` | guest used memory (`MemTotal - MemAvailable`) |
| `proc_rss_before_kb`, `proc_rss_peak_kb` | sum of all guest process RSS |
| `slab_before_kb`, `slab_peak_kb`, `slab_after_kb` | `Slab:` from `/proc/meminfo` |
| `slab_top` | five largest positive per-cache slab deltas, `name:KiB;` |
| `lat_min_us` ... `lat_mean_us`, `lat_complete` | latency distribution, `complete=1` when every iteration finished |
| `connect_fail`, `alloc_fail`, `carrier_lost`, `carrier_replaced` | carrier engine failures for the row |
| `qemu_rss_peak_kb` | host-side peak RSS of the QEMU process |
| `host_qemu_others` | foreign qemu processes at run start (contention signal) |
| `rc`, `strict`, `notes` | transfer exit status, byte-exact strict phase passed, free-form note |

## Results

Recorded 2026-11-05, 04:44-05:14 local, on this host. Every row is one boot of
`benches/run.sh`; the raw rows are `benches/results/*-rec.csv` and
`*-rec-carriers.csv`. `rc` is `tbt_burst`'s exit status and `strict` is its
byte-exact strict phase. The provenance of each row (stamp, foreign QEMU
count) is in the CSV's `host_qemu_others` column. Only the throughput
n2/n4/n8 rows and the two HTTP profile rows ran with no foreign guest; every
other row saw two or four foreign `qemu-system` processes. The two control
measurements bracket the effect: 2.73 Mbit/s with two foreign guests, 2.61
with none (about 4%). The shapes (ratios, overheads, peaks) are the robust
part; the absolute Mbit/s and microsecond values are contended-run values.

### 1. Throughput vs carrier count (raw, 40 MiB strict send-then-read)

The control is one `dd | nc` flow on one shaped 5-tuple (10 Mbit class,
`delay 20ms 10ms`). `app` is the near tunnel's `submit_bytes` rate, `wire` its
carrier `tx_bytes` rate. `app/control` is the ratio that matters.

| N | control Mbit/s | wire Mbit/s | app Mbit/s | app/control | wire/app | frames/s | CPU sys+softirq % | kworker % | rc | strict |
|---|---|---|---|---|---|---|---|---|---|---|
| control | 2.73 | - | 2.73 | 1.00 | 1.000 | - | 5.9 | 0.0 | 0 | 1 |
| 1 | 2.73 | 6.22 | 3.14 | 1.15 | 1.978 | 185.5 | 3.6 | 1.5 | 3 | 0 |
| 2 | 2.61 | 8.27 | 5.27 | 2.02 | 1.568 | 216.0 | 15.3 | 1.7 | 0 | 1 |
| 4 | 2.61 | 11.80 | 9.20 | 3.52 | 1.283 | 270.0 | 17.7 | 2.7 | 0 | 1 |
| 8 | 2.61 | 22.30 | 18.47 | 7.08 | 1.207 | 432.0 | 20.8 | 4.3 | 0 | 1 |

Per-carrier bytes for n2: 64,438,470 / 64,368,691; n4: 27,469,625 /
27,672,193 / 27,312,238 / 26,883,643; n8: 12.7-13.1 MB each (full list in the
carriers CSV). The carriers split the 40 MiB evenly.

Reading:

- Spreading is what wins: 8 x 10 Mbit carriers deliver 7.08x the single-flow
  control, and the growth from 2 to 8 carriers is close to linear (2.02,
  3.52, 7.08).
- The tunnel is *not* free at one carrier. n1 delivered 3.14 Mbit against the
  control's 2.73 (1.15x) but wrote 1.98x the app bytes and **failed the
  byte-exact strict phase** (`rc=3`, 205 s). The failure is reproducible: the
  first 8 MiB quick run and the first 40 MiB sweep both ended the same way.
  Do not read the n1 wire rate (6.22) as goodput; it is mostly retransmission.
- `wire/app` falls as N grows (1.98, 1.57, 1.28, 1.21) because a retransmit
  spreads over more carriers and because the retransmit share per carrier
  falls. The residual 1.17-1.21 at n8 is the cost of the current prediction
  path on a clean link (see "Observed module behaviour").
- CPU rises with N (3.6% to 20.8% of one vCPU), and kworker is 1.5-4.3%: most
  of the work runs in the transferring process's syscall context, not in the
  workqueue.

### 2. Cost of the HTTP profile (N=4 M=1, 40 MiB)

`http-kal` puts one chunk per record and a POST header per carrier; with
`m=1` the spare drips. `http-kal-drip` makes every carrier drip when idle.
`app B/frame` is `app_bytes / frames`, i.e. the average record payload.

| profile | app Mbit/s | wire Mbit/s | wire/app | frames/s | app B/frame | CPU sys % | CPU softirq % | kworker % | rc | strict |
|---|---|---|---|---|---|---|---|---|---|---|
| raw | 8.78 | 12.33 | 1.4039 | 277.5 | 4050 | 11.9 | 3.9 | 2.5 | 0 | 1 |
| http-kal | 9.83 | 13.33 | 1.3558 | 292.5 | 3535 | 15.6 | 3.3 | 2.4 | 0 | 1 |
| http-kal-drip | 8.94 | 11.38 | 1.2731 | 263.0 | 3565 | 15.1 | 3.1 | 2.2 | 0 | 1 |

**Hypothesis: the HTTP container costs < 5% throughput at 10 Mbit per
carrier.** It holds: http-kal delivered 9.83 Mbit against raw's 8.78 (+12%)
and drip 8.94 (+2%); the spread between the three is within the run-to-run
noise of the retransmit-dominated wire path (the earlier raw run, taken under
contention, measured 9.45). The container's own cost is visible in the frame
overhead: one chunk per ~4 KB record adds ~41 bytes (1.0%), and `wire/app`
does not separate the profiles beyond that. What does change is **CPU**: the
HTTP encode raises system time from 11.9 to 15.1-15.6 percentage points of
one vCPU, about 25% more kernel CPU for the same transfer. The drip profile
is not cheaper than http-kal during a bulk transfer (the cadence only matters
when idle); its 11.38 Mbit wire is the lowest of the three because its
retransmit share was lowest in that run.

### 3. Cost of prediction (N=4, raw, `drop_ppm` on carriers 1 and 2)

| drop_ppm | app Mbit/s | wire/app | retx_ppm | holes d/r/u | max stall us | depth | carriers | rc | strict |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 9.85 | 1.352 | 197660 | 17/17/0 | 19004 | 6 | 4/4 | 0 | 1 |
| 10000 | 6.91 | 1.200 | 168129 | 214/214/0 | 1481503 | 123 | 4/4 | 0 | 1 |
| 50000 | 5.73 | 1.488 | 115930 | 689/689/0 | 1170477 | 126 | 4/4 | 0 | 1 |
| 100000 | 5.40 | 1.446 | 140020 | 1126/1126/0 | 878936 | 113 | 4/4 | 0 | 1 |

The knee is at the **first 1%**, not at the hole budget. Goodput falls 30%
from 9.85 to 6.91 Mbit at 1% per path on two of four carriers (5% of records
overall), then flattens: 5.73 at 5%, 5.40 at 10%. Every hole was repaired
(`holes_unrecoverable=0`, every transfer strict=1); the cost is time, not
correctness. The app-visible worst stall jumps from 19 ms clean to 1.48 s at
1%, 1.17 s at 5%, 0.88 s at 10%: the receiver holds a hole up to the 5 s
budget, and the retransmit path takes up to ~1.5 s to fill it. The reorder
depth rises from 6 to 113-126 frames, which is the same effect seen from the
other side. `dup_tx` is 0 and `inflate_ppm` 1.000 in every row: this is the
hole/retransmit path, not duplication.

### 4. Latency (1 KB echo, CLOCK_MONOTONIC, 10k iterations; 2k in the loss phase)

| path | iters | p50 us | p90 us | p95 us | p99 us | max us | mean us | complete | rc |
|---|---|---|---|---|---|---|---|---|---|
| direct | 10000 | 1139.8 | 1537.8 | 1590.1 | 1653.5 | 1840.5 | 1137.9 | 1 | 0 |
| tunnel, n=4 | 10000 | 1295.4 | 1716.7 | 1789.7 | 1927.1 | 2554.8 | 1302.0 | 1 | 0 |
| tunnel, n=4, 1% loss | 2000 | 1308.9 | 1728.8 | 1793.2 | 23762.5 | 32193.1 | 1604.6 | 1 | 0 |

The tunnel adds **155.6 us at p50** (1295.4 - 1139.8) and **273.6 us at p99**
over the direct path. Both paths pay one slirp round trip and one 1 ms netem
hop; the extra is the near capture/actor hop plus the far target connection.
The jitter buffer costs nothing in steady state: it only holds a frame when a
hole exists, so the clean p50/p99 are the transport's. Under 1% loss the
injector dropped 12 records in the 2k-iteration phase, and the tail moves by
one repaired hole: **p99 23.8 ms, max 32.2 ms** against 1.9/2.6 ms clean.
That 22-31 ms is the 20 ms retransmit floor plus the feedback round trip, and
it is the number to quote for "what one lost record costs an interactive
flow". The mean rises from 1302 to 1605 us, consistent with 12 stalls of
~20-30 ms over 2000 iterations.

### 5. Memory (N=4 M=1, 40 MiB, sampled at 1 s; host RSS at 200 ms)

| row | used before kB | used peak kB | used after kB | proc RSS peak kB | slab before kB | slab peak kB | slab after kB | txq max | rxq max | ooo max | retain max | spec max | qemu RSS peak kB | rc |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| mem | 75544 | 162548 | 67988 | 9056 | 15012 | 73000 | 20040 | 65536 | 27356040 | 483063 | 2153489 | 0 | 315536 | 0 |

Slab deltas after minus before, largest first: `kmalloc-8k:608;
kmalloc-4k:256; skbuff_small_head:189; task_struct:179; pool_workqueue:152`
(KiB). The module has no private `kmem_cache`; these are the generic caches
its frames, records and skbs come from.

Reading:

- The peak is the **public socket's receive queue**: 27.4 MB, because the
  strict phase sends all 40 MiB before reading the echo, and the near module
  writes the returning bytes into the public socket's receive queue. That is
  the application's chosen pattern, not a module leak; an interleaved
  application would not hold it.
- The module's own queues are small: tx queue 64 KB, out-of-order tree
  483 KB, replay retention 2.15 MB, speculative bytes 0. At 10 Mbit/s per
  carrier these are sub-second buffers.
- The guest returns to 67,988 kB used (7.5 MB *below* its pre-transfer
  value, because the echo/sink processes and page cache settle) and the slab
  to 20,040 kB, 5 MB above baseline. The 40 MiB itself is gone; nothing
  accumulates per transfer.
- Host-side QEMU RSS peaked at 315.5 MB for a 512 MiB guest.

### 6. Scaling limit (raw, 40 MiB, port block 41000-41015)

| N | app Mbit/s | wire Mbit/s | wire/app | carriers up | connect_fail | alloc_fail | lost | replaced | retx_ppm | rc | strict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | 18.01 | 21.60 | 1.199 | 8/8 | 0 | 0 | 0 | 0 | 202653 | 0 | 1 |
| 16 | 34.18 | 40.01 | 1.170 | 16/16 | 0 | 0 | 0 | 0 | 296890 | 0 | 1 |

16 carriers, the whole 16-port block and `max_carriers`, allocate and connect
with no failure; the transfer is byte-exact and delivers 1.90x the 8-carrier
rate for 2x the carriers. The limit on this host is the slirp/NIC path and
the retransmit overhead, not the port block: the aggregate 160 Mbit offered
by 16 x 10 Mbit classes is far above the 34 Mbit observed, and the control
shows a single flow capping at 2.6-2.7 Mbit. Going wider would need a second
port block and a second shaping range; it is not a module limit at 16.

## Reading the numbers

**The control is not 10 Mbit/s.** One TCP flow in a 10 Mbit htb class with
`delay 20ms 10ms` reaches 2.6 Mbit/s on this path (40 MiB in 129.6 s). The
20 ms delay with 10 ms jitter, not the bucket, is the single-flow ceiling:
the same ceiling appears as probe I's 2.7-3.2 Mbit/s for one tunneled carrier.
So a ratio in the throughput table answers "does spreading N 5-tuples beat one
flow on this path", not "does the tunnel fill N x 10 Mbit". With `--netem none`
the same control should approach the class rate; that run is not part of the
recorded tables.

**Use the app goodput for the tunnel, not the wire rate.** The module
retransmits on a path with no configured loss: `retx_ppm` is 115k-304k and
`wire_per_app` is 1.17-1.98 in the 40 MiB rows below, with
`holes_declared` in the tens and `holes_repaired` equal to it. The wire rate
therefore overstates what the application received; the `app_mbit` column is
the comparable one. The ratio in the table is `app_mbit / control_mbit`. This
retransmission is a real cost of the current prediction path and the largest
single finding in these numbers; see "Observed module behaviour" below.

**CPU** is `system+irq+softirq` ticks over the whole transfer divided by its
wall time, i.e. percent of one vCPU. The guest has two, so 200% is the
ceiling. `kworker` is the workqueue share of that; the rest runs in the
syscall context of the transfer process. A failed row's CPU is the CPU of the
failure and is not meaningful.

**Memory**: the peak that matters is the public socket's receive queue while
the strict phase has sent 40 MiB and not yet read the echo (`rxq_max`, 29 MB
in the memory row). The module's own queues are small by comparison:
`txq_max` 64 KB, `ooo_bytes_max` 0.4 MB, `retain_bytes_max` 2.2 MB. The guest
returns to its pre-transfer used-memory level after the stream closes, and
the slab returns to within 4 MB of its baseline. The module has no private
`kmem_cache`; `slab_top` lists the generic caches it allocates from
(`kmalloc-8k`, `kmalloc-4k`, `skbuff_small_head`).

**Latency**: the tunnel adds the near capture/actor hop and the far target
connection to the same slirp round trip the direct path already pays. On this
host that is about 50-160 us at p50; the p99 difference is similar. The
jitter buffer adds nothing in steady state because it only holds a frame when
a hole exists; the cost appears as a tail when a frame is lost. The recorded
1% phase shows no such tail (max 2.2 ms), but an earlier 50-iteration smoke
run caught one repaired hole: p99 21.7 ms, max 27.6 ms, consistent with the
20 ms retransmit floor.

**Contention**: the recorded latency and memory rows were taken with four
foreign `qemu-system` processes running (the other agent's runs), and the
throughput/profile/loss rows also saw foreign guests at some point; every row
records `host_qemu_others`. Treat the absolute Mbit/s and microsecond values
as contended-run values. The shapes (ratios, overheads, peaks) are the robust
part.

## Observed module behaviour the harness had to work around

These are measurements from this harness, not design claims. They are
reported because they bound what the numbers above can mean.

1. **`echo flush` blocks after a large transfer.** After a completed 40 MiB
   strict transfer (and after a latency stream closes), `echo flush >
   /proc/net/tbtun/config` did not return within the guest's remaining wall
   time; the next row never appeared and the QEMU watchdog ended the boot
   (480 s for throughput, 360 s for profile). Repro: run two `xfer` rows in
   one boot. Workaround: `benches/run.sh` now measures one configuration per
   boot (`--n`, `--only`, `--loss` select it), so no flush follows a measured
   row.
2. **A second public connection to the same tunnel is reset.** After a
   latency stream closes, a new `connect()` to the public port succeeds, the
   first write succeeds, and the first read returns EOF or `ECONNRESET`
   (`err=104`, first iteration). This is why the latency scenario measures
   the clean and lossy phases on one connection with a marker/go handshake.
3. **Retransmission on a clean shaped path.** With `drop_ppm=0` on every
   carrier, 40 MiB through 4 carriers produced `retx_ppm` 115k-192k per
   carrier, `wire/app` 1.29-1.34, `holes_declared=11..22`,
   `holes_repaired` equal, `holes_unrecoverable=0`. At N=1 the same transfer
   wrote 1.89x the app bytes, grew `rxq` to 26 MB, and failed the strict
   byte-exact check (`tbt_burst rc=3`) after 205 s. A plausible cause is the
   feedback lag on a 40 ms path: `tx_ack` is stale by a window's worth of
   bytes, and the retransmit path re-sends frames that are merely in flight.
   The counters to confirm it are `retx_frames`/`retx_bytes` (per tunnel) and
   `hole_checks`/`gate_fail`/`time_fail` (per stream), which this CSV does
   not yet record.
4. **One flow on this path tops out at ~2.6-3.2 Mbit/s.** This is the
   control and probe I's n=1 result agreeing; the 10 Mbit class is not the
   limit, the 20 ms +/- 10 ms jitter is.
