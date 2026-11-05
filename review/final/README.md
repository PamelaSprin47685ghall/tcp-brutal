# Final acceptance evidence

Tree under test: 9f5bfda (w-final-qemu + w-adv2 merged on cfa2e46).
Module: sha256 0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63.
Host was quiet before every run (`pgrep -af 'qemu-system|bench'` empty); no bench
was started, as required for a stable measurement.

## Host suites

- `make check`: 357 checks, 0 failures; fuzz 7550734/0; http 249/0; reorder
  52792/0; ack 10026/0; control 181/0; multistream 68580/0; anchor 124/0; h11
  cross-check accepted.
- `make host-test-san`: rc=0; parse 357/0, fuzz 7550734/0, http 249/0,
  http_fuzz 4078276/0.
- `review/lint.sh`: 16/16 checks, 14/14 self-tests.

## QEMU probes A-K

`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 900`:
11 passed, 0 failed, 0 skipped in 159 s. Per-probe lines and key evidence are
in `qemu-AK-summary.txt`.

## Adversarial suite

`TBT_ADV_PUBLIC_TIMEOUT=3000 adversarial/run.sh --no-build`: 78 passed,
0 failed, 0 skipped in 238 s. All 15 default groups ran (public, control,
resource, slab, final, replay, rmmod, flush, catconfig, cfg_mix, param, port,
near, far, stall) plus the isolated close-flush crash repro. Full result lines
are in `adversarial-results.txt`; the klog scanner reported no oops/warning.
