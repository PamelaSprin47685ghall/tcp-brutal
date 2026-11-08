# Kernel matrix

`scripts/kernel-matrix.sh` builds tbtun against several kernel series on two
architectures, runs the host tests in every cell, and loads the module in QEMU
where a QEMU binary for the architecture exists.  It is the 0.4.0 compatibility
check.  It does not install anything and it does not run on real hardware.

| Series | Tarball | Architecture |
| --- | --- | --- |
| 5.15 LTS | `5.15.225` | x86_64, aarch64 |
| 6.6 LTS | `6.6.162` | x86_64, aarch64 |
| 6.12 LTS | `6.12.116` | x86_64, aarch64 |
| 7.0 | `7.0.14` | x86_64, aarch64 |

The tarballs are the official `kernel.org` releases, fetched from a mirror
(`KM_KERNEL_MIRROR`, default Tsinghua) and cached.  Each cell records the URL
and the SHA-256 of the file it used.  No distribution header package is
involved: the headers are the tarball's own tree after `defconfig` and
`modules_prepare`, so the module and the kernel image always come from the same
source.

`README.md` declares Linux 6.1 as the minimum.  The 5.15 column is kept on
purpose: it records where the declared boundary is, and whether the source still
compiles there.

## What one cell does

0. Check the free space under the work directory (default requires 5 GiB;
   `KM_MIN_FREE_GB` changes it).  A cell that cannot fit says `disk:` in its
   reason instead of failing later with a truncated kernel tree.
1. Extract the tarball into `work/<version>-<arch>/kernel`.
2. `make defconfig`, then adjust the configuration:
   - `DEBUG_INFO`, `DEBUG_INFO_BTF`, `DEBUG_INFO_BTF_MODULES`, `GDB_SCRIPTS`
     off.  The check needs a loadable module, not debug info; `pahole` is not a
     dependency and the build stays small.
   - `MODULE_SIG` off.  The guests have no keyring; unsigned modules load.
   - `WERROR` off.  A new compiler warning is reported, not raised to a kernel
     build failure.  The module itself is still judged by its first error.
   - `LIBCRC32C=y`.  tbtun calls `crc32c()`; defconfig builds it as a module,
     and the minimal guest does not load that module first.  The load check
     found exactly this one missing symbol on 6.12.
3. `make modules_prepare`.
4. `make -C <tree> M=<private copy of this repository> modules`.  The copy
   keeps the build and the host tests out of the checkout and out of each
   other.  kbuild's architecture names are `x86` and `arm64`; `x86_64` and
   `aarch64` are the names in this script's cells and report.
5. `make host-test` in the same copy.  These are the ordinary host tests; they
   need no kernel tree.
6. `modinfo` on the built `.ko`: the name must be `tbtun` and the vermagic must
   be present.  Both are recorded per cell.
7. Optional load check.  Build `bzImage` (x86_64) or `Image` (aarch64), pack an
   initramfs with only `/init`, `/tbtun.ko`, `/dev/console` and `/proc`, and
   boot it.  The static `/init` is `tests/kernel-matrix/init.c`; it calls
   `finit_module`, checks `/proc/modules`, calls `delete_module`, checks that
   the module is gone, and repeats the load/unload cycle once.  It prints
   `KMATRIX_LOAD_PASS` only after both cycles succeed.
   QEMU runs with `-nic none`: the default virtio-net asks for
   `efi-virtio.rom`, which an unpacked `qemu-system-aarch64` does not find even
   when `qemu-common` ships it, and the guest has no network anyway.
8. Collect the status files, delete the kernel tree and the source copy unless
   `--keep` was given, and keep `tbtun.ko`, the logs, `modinfo.txt` and the
   status files.

`KBUILD_MODPOST_WARN=1` is required for step 4.  A tree that has only run
`modules_prepare` has no `vmlinux` and therefore no `Module.symvers`, so
modpost cannot resolve the module's external symbols.  Without the flag,
6.12 and later turn every unresolved symbol into a build error.  With it the
build produces the `.ko`, and the guest load check is what proves the symbols
exist.  The module log keeps the warnings.

## Running

```sh
scripts/kernel-matrix.sh                     # the whole matrix, load checks on
scripts/kernel-matrix.sh --no-load           # build + host tests only
scripts/kernel-matrix.sh --only 6.12.116:x86_64 --keep
scripts/kernel-matrix.sh --list
```

| Option | Meaning |
| --- | --- |
| `--versions LIST` | Comma or space separated tarball versions |
| `--arches LIST` | `x86_64`, `aarch64` |
| `--only VER:ARCH` | Repeatable; runs one cell |
| `--jobs N` | Cells in parallel (default 1, so one kernel tree is on disk at a time) |
| `--make-jobs N` | `make -j` inside a cell (default `nproc / jobs`, capped at 12) |
| `--cache DIR` | Default `~/.cache/tbtun-kmatrix` |
| `--work DIR` | Default `<cache>/work` |
| `--timeout SEC` | QEMU timeout (default 300) |
| `--qemu-aarch64 PATH` | Override the aarch64 emulator |
| `--no-load` | Skip the QEMU step; cells become `BUILD_ONLY` |
| `--keep` | Keep the kernel tree and the source copy |

`qemu-system-x86_64` comes from the host.  `qemu-system-aarch64` is taken from
`PATH`, or from the Manjaro package named by `pacman -Sp` and unpacked under
`<cache>/tool/qemu-aarch64`.  An architecture with no QEMU binary is still
built and tested; its load column says `skip` and its verdict is `BUILD_ONLY`.

Each cell writes:

```
work/<version>-<arch>/
    tbtun.ko      the module
    cell.log      commands and output
    module.log    kbuild output, including modpost warnings
    host.log      make host-test
    image.log     the kernel image build
    qemu.log      guest console
    modinfo.txt   modinfo output
    status.*      one KEY=VALUE file per result
```

`work/report.md` is the matrix table, with the compiler, the tarball hash and
the vermagic for every cell.

## Result matrix

Measured on 2026-11-08 with the tarballs above.  `load` is the QEMU check;
`wall` is one cell's wall clock.

| kernel | arch | module | host tests | load | verdict | compiler | vermagic | wall |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 5.15.225 | x86_64 | fail | ok | skip | FAIL | gcc 16.2.1 | — | 47 s |
| 5.15.225 | aarch64 | fail | ok | skip | FAIL | aarch64-linux-gnu-gcc 16.1.0 | — | 48 s |
| 6.6.162 | x86_64 | ok | ok | PASS | PASS | gcc 16.2.1 | 6.6.162 SMP preempt mod_unload | 240 s |
| 6.6.162 | aarch64 | ok | ok | PASS | PASS | aarch64-linux-gnu-gcc 16.1.0 | 6.6.162 SMP preempt mod_unload aarch64 | 296 s |
| 6.12.116 | x86_64 | ok | ok | PASS | PASS | gcc 16.2.1 | 6.12.116 SMP preempt mod_unload | 116 s |
| 6.12.116 | aarch64 | ok | ok | PASS | PASS | aarch64-linux-gnu-gcc 16.1.0 | 6.12.116 SMP preempt mod_unload aarch64 | 306 s |
| 7.0.14 | x86_64 | ok | ok | PASS | PASS | gcc 16.2.1 | 7.0.14 SMP preempt mod_unload | 239 s |
| 7.0.14 | aarch64 | ok | ok | PASS | PASS | aarch64-linux-gnu-gcc 16.1.0 | 7.0.14 SMP preempt mod_unload aarch64 | 380 s |

Six cells pass.  The two 5.15 cells fail in the module build, not in the host
tests or the load check: the source refuses that series and the 5.15 headers
lack `pde_data`.  The host tests still run, and only the module and the guest
check are skipped.

The 5.15 cells fail before any API check can run: `tbtun/tbtun.h` contains
`#error "tbtun needs Linux 6.1 or newer (README, DESIGN §2)"`, and the compiler
also reports `pde_data` missing.  The matrix records this as a `kernel-api`
failure, which is the intended boundary, not a regression.

## Cost

- Sum of the eight cell wall clocks: 1672 s (27.9 min) on 16 CPUs with
  `make -j12`, one cell at a time.
- x86_64 cells take 18-240 s; aarch64 cells take 19-380 s.  The aarch64 image
  is cross-built and its guest runs under TCG, so it is the expensive side.
- A cell peaks at about 4 GiB on disk (kernel tree, objects, image).  The
  script refuses to start below 5 GiB free (`KM_MIN_FREE_GB`).  An image build
  that hits `ENOSPC` retries once after 60 s.
- The tarball cache is 0.6 GiB; after the run the whole cache is about
  0.9 GiB, because each cell deletes its kernel tree and source copy.
- Reusing an interrupted kernel tree is much cheaper: 6.12.116 x86_64 took
  116 s on the second attempt.
- `--jobs 3` was tried first.  Other processes on the host filled the
  filesystem and two cells died with `ENOSPC`, so the default is now one cell
  at a time.  `KM_JOBS` still overrides it.

## Limits

- The kernel is `defconfig`, not a distribution configuration.  The check
  answers "does the source build and load against this series", not "does it
  match Manjaro/Ubuntu/Fedora module lists".
- Debug info and BTF are off; the check does not validate BTF generation.
  `docs/build-env.md` covers the BTF path separately.
- aarch64 runs under TCG.  It proves the architecture and the load path; it is
  not a performance measurement.
- No real hardware and no host `insmod`: the guest is the privileged side.
- The load check uses the module's own init and exit; it does not run traffic.
  Traffic behaviour belongs to `tests/qemu/run.sh` and the benches.

## Reproducing the tarballs

The SHA-256 values the cells recorded (the cache writes them to
`<cache>/dl/linux-<version>.tar.xz.sha256`):

| Tarball | SHA-256 |
| --- | --- |
| `linux-5.15.225.tar.xz` | `b36b9fd6fa854787687c4c980aee4747577f563a8d4f45b7da2fdbd914c8224f` |
| `linux-6.6.162.tar.xz` | `4aa89f3652fc131e5d39990f3bcd4b9e131028af3f315dbe83f3a8557dc06f06` |
| `linux-6.12.116.tar.xz` | `cb3ed3dfdd47e6ade49dc933db334fd86e24cadee920da612c262b0e9b6a511b` |
| `linux-7.0.14.tar.xz` | `de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98` |

`KM_KERNEL_MIRROR` changes the mirror; the tarball names and the hashes do not
change.
