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

Production shape: `N+M=256`, `K=N`, `W=2K`, `S=8192`, `d=16`.
Dynamic `M = clamp(ceil(256*(p+0.09)), 16, 128)` rounded up to 8.

## Not tested here

* No arm64 or mips cross-compile in this run.
* No 1 Gbit/s link; bench is a host loop.
* The kernel port (`tbtun/tbt_rlnc.c`) is not written; this directory is host-only.
