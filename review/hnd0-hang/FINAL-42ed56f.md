# Local QEMU validation of the final integration head 42ed56f

Head: `42ed56f12fe7d3e8eccde4816948b7b2206f6d3a` (product `0eba714`).
It contains `d0f1eb4` (256-bit `path_lost_mask`), `dc4532a` (256-wide
announce bookkeeping), and `bd7d308` (`430ba74` + `f015a56` hang fixes).

Commands (QEMU/KVM, 2 vCPU, 512 MiB, kernel `7.2.9-1-MANJARO`, hostfwd 19107):

```sh
TBT_HOSTFWD_PORT=19107 tests/qemu/run.sh --ko "$PWD/tbtun.ko" --probes Q \
    --no-net --timeout 240
TBT_HOSTFWD_PORT=19107 tests/qemu/run.sh --ko "$PWD/tbtun.ko" \
    --probes A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P --timeout 900
```

Results:

- probe Q: PASS. `prov_now=0 peak=320 reject=8740`, `dead now=8 peak=8`
  (cap 512), 200/200 heartbeats, near recovers 256/256.
- probes A-P: 16 PASS / 0 FAIL / 0 SKIP, 262 s.
- Console scan: 0 `WARNING:`, 0 `BUG:`, 0 `KASAN`, 0 `Kernel panic`,
  0 `Out of memory`, 0 `soft lockup`, 0 `blocked for more`, 0 `RCU Stall`.

UBSAN: the QEMU kernel has `CONFIG_UBSAN=n`, so a UBSAN report cannot be
produced locally. The `UBSAN` scan is 0 by construction, not evidence; the
HND0 8 MiB run on this head covers UBSAN.

Two earlier full-suite runs on this head each had one timing flake (probe C
`replaced carrier 0 carried 1088 bytes`; probe L `transfer stalled`). Both
probes passed 5/5 and 3/3 in isolation, and the third full run above is clean.
The flake is a transfer/timing sensitivity, not a counter mismatch.

Optional follow-up on `w-hnd0-hang`: `5c06db6` makes carrier allocation use
`mutex_trylock(pump_lock)` and fall back to a fresh object, so the accept path
never waits for an active pump. Probe C passed 5/5 with it. It is not part of
42ed56f and is not required by the result above.
