# Build environment for out-of-tree kernel modules

Verified 2026-11-04 against kernel `7.2.9-1-MANJARO` (GCC-built, `CONFIG_DEBUG_INFO_BTF_MODULES=y`).
The kernel headers package is **not** installed; everything below runs as the normal user.

## Payload

Extracted, unprivileged, from the Manjaro stable mirror:

| Path | Content |
| --- | --- |
| `~/.cache/tbtun-build/linux72-headers/usr/lib/modules/7.2.9-1-MANJARO/build` | full kernel build tree, incl. `Module.symvers`, `vmlinux` with `.BTF`, prebuilt `tools/bpf/resolve_btfids` |
| `~/.cache/tbtun-build/pahole/usr/bin/pahole` | pahole 1.32, with its own `libdwarves*.so` in `../lib` |
| `~/.cache/tbtun-build/env.sh` | exports `KDIR`, `PAHOLE`, `LD_LIBRARY_PATH`, `PATH` |

## Building

One command, which sources the payload itself and prints what it runs:

```sh
make check
```

`check` builds the module and then runs the host tests. They need no root, no
module and no kernel headers: they link the same parser sources the module is
built from (`tbt_parse.c` for the config grammar, `tbt_rec.c` + `tbt_proto.c`
for the wire framing) and finish in under a second.

```sh
make host-test      # both host tests, no kernel tree needed
make host-test-san  # the same with AddressSanitizer and UBSan
```

`host-test` runs `tests/unit/test_parse` (the `/proc` grammar) and `tests/unit/test_fuzz`
(200 000 random record buffers through every split size, 200 000 random length
prefixes, an adversarial set, and the full frame-type × body-length matrix).
`host-test-san` rebuilds both with `-fsanitize=address,undefined`; a compiler
that cannot link those runtimes gets a one-line skip and exit status 0.

The long form, for scripts and for readers who want to see each variable:

```sh
. ~/.cache/tbtun-build/env.sh
KDIR=$KDIR make -C $KDIR M=$PWD PAHOLE=$PAHOLE modules
```

The repo Makefile also works, because it forwards `KERNEL_DIR`:

```sh
. ~/.cache/tbtun-build/env.sh
make KERNEL_DIR=$KDIR PAHOLE=$PAHOLE
```

Result: `tbtun.ko` (3.8 MB), `vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload`,
`.BTF` and `.BTF.base` sections present, `nm -u` resolved entirely against the
payload's `Module.symvers`.

## Gotchas

- **pahole version must match.** `CONFIG_PAHOLE_VERSION=132`; the tree selects
  `--btf_features=...` flags by that number, so use pahole 1.32. A missing pahole
  is a hard error (`tools/gen-btf.sh: line 72: pahole: command not found`,
  exit 127), not a silent BTF skip, because the tree ships `vmlinux`.
- **`PAHOLE` in the environment is ignored.** The kernel Makefile assigns
  `PAHOLE = pahole`, which overrides the environment. Pass it on the *make
  command line* (`PAHOLE=/path/pahole`) or put it on `PATH`.
- **pahole needs its bundled libs.** `libdwarves*.so` are not installed
  system-wide; without `LD_LIBRARY_PATH=~/.cache/tbtun-build/pahole/usr/lib`
  it dies with `libdwarves_emit.so.1: cannot open shared object file`.
- **Headers live outside `/lib/modules`**, so `KERNEL_DIR`/`KDIR` must always be
  passed explicitly; the default `KERNEL_DIR` will not resolve.
- **No `MODVERSIONS`**, so no CRCs to match; module load depends on vermagic only.
- **Modules are zstd-compressed** (`CONFIG_MODULE_COMPRESS{,_ZSTD,_ALL}=y`).
  `make modules_install` handles this; a manual copy of a plain `.ko` into
  `/lib/modules/...` needs `zstd -19 --rm` plus `depmod` to match the rest of the tree.
- **Staging installs**: `make -C $KDIR M=$PWD INSTALL_MOD_PATH=/tmp/pkg modules_install`.
- **GCC kernel**: no `LLVM=1` needed (the repo Makefile detects this itself).
- **Root is needed to load the module and to install through DKMS** — see below.

## Root

Building never needs root. Loading does: `insmod` requires `CAP_SYS_MODULE`.

`tools/install_dkms.sh` is the exception: it writes `/usr/src` and
`/lib/modules`, so it needs root, and it requires the distribution headers at
`/lib/modules/$(uname -r)/build`. The extracted payload below serves `make` and
`tools/build.sh` only; DKMS resolves its own kernel source tree and does not
read `env.sh`. On a host without the headers package, stage the sources with
`DKMS_ROOT=<writable dir> tools/install_dkms.sh --stage-only` and build with
`tools/build.sh`.

> **Warning:** on this box `pkexec` runs commands as root *without*
> authentication, and no `/usr/share/polkit-1/rules.d` rule appears to grant
> that. Treat it as a polkit misconfiguration to be fixed, not as a path to
> use. Load the module with an explicit `sudo` only.

## Header coverage

A throwaway `obj-m` including `<net/tcp.h>`, `<net/sock.h>`, `<net/ip.h>`,
`<linux/kprobes.h>` and nine `<net/netfilter/*.h>` headers (conntrack core,
tuple, zones, extend, helper, seqadj, nat, nftables) compiles and links against
this payload. `modpost` resolves all references from the 32 088-entry
`Module.symvers`; no `undefined` warnings. Note `tcp_sendpage()` no longer
exists in this kernel.
