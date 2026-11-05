# FEC prototype — streaming GF(2) RLNC

Host-only reference implementation of the sliding-window RLNC data plane
described in `../fec-design.md`.

## Build and test

```
make test     # test_rlnc: window recovery, edges, order/late, frontier, PRNG, fuzz
make bench    # encode/decode MB/s at the production shape
make sim      # sim_rlnc (p curve) + sim_dynamic (controller)
make asan     # same tests under ASan/UBSan
```

Current result: `test_rlnc` PASS, 1000/1000 window-loss cases recovered,
1000 fuzz rounds. Bench and sim numbers are printed by the binaries.

## What is here

| File | Purpose |
|---|---|
| `rlnc.c/.h` | GF(2) sparse RLNC: splitmix64 PRNG, Fisher-Yates selection, encode, GF(2) elimination, frontier |
| `test_rlnc.c` | Correctness: no loss, window losses (u+6 <= m), too-many-losses, order/late, frontier, PRNG, fuzz |
| `bench_rlnc.c` | Encode/decode throughput at N+M=256, K=N, W=2K |
| `sim_rlnc.c` | iid loss simulation |
| `sim_dynamic.c` | Controller: p -> M, epoch switching |
| `legacy/` | Old block/RS prototypes. **Not built.** Kept only as historical comparison; the design no longer uses them. |

## Parameters

Production shape (HND0): 256 carriers = 32 groups x 8, 1 share/carrier.
Per group `N=8-M`, `K=N`, `W=2N`, `S=8192`, `d=W/2` (never `d=W`: the
generator then produces identical all-ones rows and rank 1), with
`M=4/d=4` (500M, default), `M=5/d=3` (375M), `M=6/d=2` (250M);
`M=3/d=5` (625M) only holds at p<=1%.
The controller picks `M` per group from the measured loss rate; the packet
window `W` is not the send byte window `k*BDP`.

Small `N=4/M=1` or `N=16/M=4` runs are debug/control shapes, not the
production profile.

## Not tested here

* No arm64 or mips cross-compile in this run.
* No 1 Gbit/s link; bench is a host loop.
* The kernel port (`tbtun/tbt_rlnc.c`) is not written; this directory is host-only.
