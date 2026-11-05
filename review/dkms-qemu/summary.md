# DKMS acceptance on a clean kernel in QEMU

Tree: `c641c5a` on branch `w-dkms-qemu`. This directory is the only change; no
product code was touched. Verdict: **criterion 8 passes** — on a clean
`7.2.9-1-MANJARO` kernel, `scripts/install_dkms.sh` staged, built and installed
`tbtun/0.0.1` through the real dkms, `modprobe tbtun` published
`/proc/net/tbtun`, a tunnel line was written and read back canonically,
`rmmod tbtun` unloaded it, and `scripts/install_dkms.sh --uninstall` removed
every DKMS entry, source tree and module file, leaving no residue.

## Why a guest

The host cannot run this flow: uid 1000, no `dkms` on PATH, no
`/lib/modules/7.2.9-1-MANJARO/build`, and `sudo` needs a password. A user
namespace is not a substitute — `CAP_SYS_MODULE` is needed in the initial user
namespace. `host-env.txt` records exactly that:

```
uid=1000
dkms=absent
host_headers=absent
host_module_tree=present
```

Booting the distribution kernel in QEMU is the only way to get real root,
real `CAP_SYS_MODULE` and a real module tree without touching the host. The
guest's kernel is the `vmlinuz` from the `linux72` 7.2.9-1 package
(`sha256 f2976ed7…`), i.e. the same build the module's vermagic names.

## What the guest is

- Kernel: `Linux manjaro 7.2.9-1-MANJARO #1 SMP PREEMPT_DYNAMIC … x86_64`,
  KVM, 2 vCPU, 2 GiB, no NIC (`console=ttyS0 panic=-1 loglevel=7`).
- Userland: the host's Manjaro `/usr`, `/etc`, `/home` bind-mounted into a
  chroot over **read-only 9p** exports; gcc 16.2.1, GNU Make 4.4.1, binutils,
  kmod 34.2, zstd 1.5.7, pahole 1.32 (from `~/.cache/tbtun-build/pahole`).
- Writable state: an ext4 image (`/dev/vda`) holding the real module tree
  (6441 `*.ko.zst`, the `linux72` payload), the real kernel build tree
  (`linux72-headers`), an empty `/usr/src` and an empty `/var/lib/dkms`. It is
  rebuilt from those payloads for **every** run, so `/lib/modules/<rel>/build`
  points at a genuine header tree and nothing from a previous run survives.
- dkms: the extracted Manjaro package in `/tmp/rel-dkms`; `.PKGINFO` says
  `dkms 3.4.4-1`, packager `Mark Wagie <mark@manjaro.org>`; the script's
  `sha256 165cfb35…` equals the digest recorded in the package `.MTREE`.
  `/etc/dkms/framework.conf` is not installed in the guest; every assignment
  in the package's copy is commented out, so dkms used exactly the values a
  real install would (`/var/lib/dkms`, `/usr/src`, `/usr/lib/modules`).
- No prebuilt module: the work tree contains no `tbtun.ko` (checked in the
  guest), and `/lib/modules/<rel>/updates/dkms` is empty before the run. The
  module the guest loads is the one dkms compiled there — `make.log` shows
  `CC [M] tbtun/*.o`, `LD [M] tbtun.ko`, `BTF [M] tbtun.ko`, `# exit code: 0`
  under `make -C /usr/lib/modules/7.2.9-1-MANJARO/build
  M=/var/lib/dkms/tbtun/0.0.1/build`.

## Flow and results

Every step prints its command line and `DKMS_STEP <name> rc=<n> secs=<n>`;
every assertion prints `DKMS_CHECK <name> PASS|FAIL`. The committed run has
38 steps and 39 checks, all `rc=0`/`PASS`, 0 failures. The raw console is
`console.log`; per-step output is in `artifacts/<name>.log`. `run.sh` judges
from those markers, not from QEMU's exit status.

| Step | Command | rc | s |
| --- | --- | --- | --- |
| pre-checks | `dkms status`; `/proc/net/tbtun`; `/lib/modules/<rel>/updates/dkms/tbtun.ko*`; `/usr/src/tbtun-*`; `grep tbtun modules.dep` | 0 | 0 |
| install | `scripts/install_dkms.sh --kernel 7.2.9-1-MANJARO` | 0 | 7 |
| dkms status | `dkms status` → `tbtun/0.0.1, 7.2.9-1-MANJARO, x86_64: installed` | 0 | 0 |
| module files | `ls -l /lib/modules/<rel>/updates/dkms/` → `tbtun.ko.zst`, 109749 B, `sha256 340e5d60…` | 0 | 0 |
| modinfo | `modinfo tbtun` → `filename …/updates/dkms/tbtun.ko.zst`, `version 0.0.1`, `vermagic 7.2.9-1-MANJARO SMP preempt mod_unload` | 0 | 0 |
| loaded | `grep '^tbtun ' /proc/modules` → `tbtun 172032 0 - Live … (OE)`; `lsmod` shows it | 0 | 0 |
| procfs | `/proc/net/tbtun/{config,tunnels,ports,stats,peers,http}` exist | 0 | 0 |
| config write | `echo 'tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443' > /proc/net/tbtun/config` | 0 | 0 |
| config read-back | `cat /proc/net/tbtun/config` → `tunnel 1 role=near listen=127.0.0.1:9000 peer=203.0.113.9:443 n=4 m=1 dup=1 profile=http-kal keepalive=5000 …` | 0 | 0 |
| tunnels | `cat /proc/net/tbtun/tunnels` → `1 near listening 127.0.0.1:9000 203.0.113.9:443 - 4 1 0` | 0 | 0 |
| listener | `/proc/net/tcp` has `0100007F:2328 … 0A` (127.0.0.1:9000 LISTEN) | 0 | 0 |
| del | `echo 'del 1' > /proc/net/tbtun/config`; config empty; listener gone | 0 | 0 |
| rmmod | `rmmod tbtun`; `/proc/net/tbtun` gone; no `tbtun` in `/proc/modules` | 0 | 0 |
| modprobe | `modprobe tbtun`; `/proc/net/tbtun` back; `modinfo` vermagic matches; config round-trip repeated | 0 | 0 |
| rmmod again | `rmmod tbtun`; procfs gone | 0 | 0 |
| uninstall | `scripts/install_dkms.sh --uninstall` → `Deleting …/updates/dkms/tbtun.ko.zst`, `Running depmod… done`, `Deleting module tbtun/0.0.1 completely from the DKMS tree`, `Removing staged sources`, `Done.` | 0 | 2 |
| residue | `dkms status` empty; no `/usr/src/tbtun-*`; no `tbtun.ko*` under `updates/dkms` or `extra`; `/proc/net/tbtun` absent; no `tbtun` in `modules.dep`; `modinfo tbtun` fails | 0 | 0 |

Post-uninstall `leftovers` shows `ls: cannot access
'/lib/modules/7.2.9-1-MANJARO/updates/dkms': No such file or directory`, an
empty `/usr/src`, and `find /var/lib/dkms -maxdepth 3` printing only
`/var/lib/dkms`.

`dmesg` after the run: `tbtun 0.0.1 loaded: 64 tunnels max, 16 carriers max`,
`tbtun: unloaded`, and the expected
`module verification failed: signature and/or required key missing - tainting
kernel` (this kernel has `CONFIG_MODULE_SIG_FORCE` unset). No `BUG:`, `Oops` or
`WARNING:` lines.

## Observations, not failures

- **`Deprecated feature: CLEAN`** (three times per install) — dkms 3.4.4
  warns about `CLEAN` in `dkms.conf`. It is still honoured today; the build
  itself is driven by `MAKE[0]`. Worth a follow-up if dkms 4 drops the key.
- **`Running in chroot, modules won't be signed`** — dkms detects the chroot
  and skips signing. The guest has no MOK keys and does not enforce
  signatures, so the unsigned module loads; on a secure-boot machine the
  signing path would run instead.
- **`tbtun: tunnel 1 carrier N failed to connect: -101`** — the test peer is
  `203.0.113.9` (TEST-NET-3) and the guest has no route to it, so each
  configured tunnel dials its carriers and fails with `ENETUNREACH`. This is
  the kernel proving the tunnel was really created; the acceptance evidence
  here is the canonical config read-back and the listening socket, not a data
  path (probes A–K cover that).
- 9p caps `msize` at 512000 for virtio; irrelevant to the result.
- The guest is not a full disk install: it is the host's userland over 9p.
  What the criterion needs — a kernel with no tbtun, an empty DKMS tree, real
  dkms, real headers, real module tree, and root — is real, and the pre-checks
  prove the clean start on every run.

## Reproduce

```sh
review/dkms-qemu/run.sh                    # rebuilds the image, boots, judges
review/dkms-qemu/run.sh --keep-image       # reuse the image (not "clean")
review/dkms-qemu/run.sh --timeout 900
```

Inputs: `~/.cache/tbtun-build/dl/vmlinuz-7.2.9-1-MANJARO`,
`~/.cache/tbtun-build/{linux72-full,linux72-headers,busybox,pahole}` and the
extracted dkms package in `/tmp/rel-dkms` (see `docs/build-env.md` and
`tests/qemu/README.md`). The runner takes `/tmp/tbtun-qemu.lock` for its whole
duration, so it never overlaps another QEMU or bench.

Wall clock: 73 s total, of which ~7 s image build, ~1 s boot, 7 s install
(dkms build 5 s, depmod the rest), 2 s uninstall, ~12 s inside the guest.
A second run reproduced the identical module digest (`340e5d60…`).

## Files

| File | Content |
| --- | --- |
| `run.sh` | host runner: image, initramfs, QEMU under `flock`, verdict |
| `mkimage.sh` | builds the ext4 image from the distro payloads (`mke2fs -d`) |
| `mkinitramfs.sh` | busybox, 9p modules, `init`, `guest-test.sh`, dkms |
| `init` | guest PID 1: mounts, 9p, ext4, chroot |
| `guest-test.sh` | the acceptance flow and its assertions |
| `console.log` | complete guest console |
| `artifacts/` | per-step logs, `dkms status`, `modinfo`, `lsmod`, `/proc` dumps, `make.log`, module `sha256`, `host-env.txt` |
