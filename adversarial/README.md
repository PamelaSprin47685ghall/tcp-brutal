# Adversarial suite for tbtun

This directory attacks `tbtun` from outside: a hostile carrier on the far
listen port, a fake far endpoint that attacks the near side's HTTP response
parser and feedback path, an abusive application on the public port, an
unprivileged `/proc` writer, and resource-exhaustion drivers. It never edits
the module, and it does not trust the module's own tests: the verdicts come
from the wire, from `/proc`, and from `dmesg`.

The module's normal tests (`tests/qemu/`) prove it works. This suite tries to
make it fail; the observed defects are in `findings.md`.

## Run

```sh
adversarial/run.sh                  # build tbtun.ko, boot the guests, print the table
adversarial/run.sh --no-build       # use the existing tbtun.ko
adversarial/run.sh --no-crash       # skip the isolated close-flush repro
TBT_ADV_GROUPS="public control" adversarial/run.sh --no-build
TBT_ADV_GROUPS=far TBT_ADV_CARRIER_TIMEOUT=180 adversarial/run.sh --no-build
```

Requirements are the same as `tests/qemu/` (see that README): the kernel image
and busybox in `~/.cache/tbtun-build`, `qemu-system-x86_64`, and the header
payload that `make check` uses. The host needs no root and no network; KVM is
used when `/dev/kvm` is writable, otherwise TCG.

`run.sh` builds `tbtun.ko` with the documented `source ~/.cache/tbtun-build/env.sh;
make check` (unless `--no-build`), builds `adv_probe.c`, and boots **one guest
per group**. Each guest runs the phases listed in `/adv-phases.txt` and prints

```
PHASE_START <phase>
ADV_RESULT <attack> PASS|FAIL|SKIP <detail>
PHASE_END <phase>
```

The host prints one table row per attack. The grouping exists because a defect
can hang the kernel: the hostile-carrier hang must not hide the public-port
results. A phase that starts but never ends is reported as
`console.hang.<phase> FAIL`; a phase that never starts is
`console.missing.<phase> FAIL`; kernel oops/warning lines found on the console
add `console.klog.<group> FAIL`. The default groups are the individual phases
(`public control resource slab final replay rmmod flush param port near far`)
plus the isolated crash repro.

Two groups run more than one phase: `public` also runs `srcfilter`, `near`
also runs `nearx`, and `far` also runs `farx`. The extra phases are separate
`PHASE_START`/`PHASE_END` sections so a hang in one attack still leaves the
others' verdicts on the console.

The full console of each group is left in `adversarial/console-<group>.log`;
the crash repro writes `adversarial/crash-console.log`. The module hash under
test is printed at the start and recorded in `findings.md`.

`adversarial/qemu/mkinitramfs.sh` is a private copy of
`tests/qemu/mkinitramfs.sh`, so a concurrent edit to `tests/qemu/` cannot
change what this suite boots.

## Attack catalogue

### Hostile carrier on the far listen port (`far.*`)

A far tunnel (`profile=http-kal`, `n=4`) with a deterministic echo target.
`adv_probe far` speaks the wire protocol itself. The far adopts the nonce from
the first valid HELLO and anchors that source; later connections must present
the same nonce from the same address. `nonce=` and `peer=` pin both at
configuration time; without them the first authenticated HELLO wins (TOFU) and
the module warns once.

| Attack | What is sent | Safe result |
| --- | --- | --- |
| `far.garbage` | 9000 bytes of `G`, no HTTP request | carrier closed, no warning |
| `far.hello_garbage` | valid request + HELLO, then 64 garbage bytes | carrier closed, no warning |
| `far.wrong_nonce` | HELLO with a different nonce | refused, carrier closed |
| `far.dup_index` | second HELLO with the live nonce and index | refused; anchor survives |
| `far.srcaddr_dup` | same, from 127.0.0.2 | refused as a duplicate |
| `far.srcaddr_free` | same nonce, free index, from 127.0.0.2 | refused (a replay must not join) |
| `far.chunk_huge` | chunk size `ffffffff` | PROTO, carrier closed |
| `far.bodylen_tail` | chunk 8224, header says `body_len=8192`, 3 body bytes, then FIN | no overread, no warning |
| `far.split_record` | chunk 32 whose header says `body_len=100` | PROTO |
| `far.drip_split` | drip chunk between a record's header and body | PROTO |
| `far.chunk_oversize` | chunk 8229 (`TBT_CHUNK_MAX` 8228) | PROTO |
| `far.no_host` | request line, no `Host` | PROTO |
| `far.headers_big` | 9000-byte header block | PROTO at the carrier buffer |
| `far.open_other_sid` | OPEN for a second sid on a second carrier, then DATA for it | multiconn adopts the second stream: its 4 bytes echo under that sid, stream 1 never sees them, and the carrier stays open (no PROTO close) |
| `far.fbk_forged` | FBK huge `uack`/`recv_cnt`, wrong generation, path 15 | ignored; carrier 0 `uack` unchanged |
| `far.hello_profile0` | HELLO profile 0 | refused |
| `far.hello_profile9` | HELLO byte 9 | accepted as profile 1, generation 2 |
| `far.data_path` | ping, pong, duplicate seq with different bytes | the assembled anchor echo is exactly 8 bytes `pingpong` (a coalesced frame and its retransmits are legal), no `XXXX`; the peers table before/after names the carrier that carried it, so a 1 s read timeout cannot be blamed on the wrong layer |
| `far.nonce_pinned` | far configured with `nonce=`; the first source joins, a wrong nonce is refused, then the same nonce from 127.0.0.2 on a free index | the stranger is REFUSED and `join_src_reject` grows; a stranger that joins means the nonce alone hijacks a slot |
| `far.src_anchor` | far configured with `peer=127.0.0.1:1`; the anchored source joins index 0 and 1, then 127.0.0.2 sends the same nonce on index 2 | the stranger is REFUSED, the anchored source works, and `peer=` renders in the canonical config |
| `far.overlap` | on one authenticated carrier: three rounds of DATA `seq+4096 len 8192`, `seq+12288 len 4096`, `seq len 8192` | the app echoes all 48 KiB in order, `ooo_bytes`/`ooo_frames` return to 0, `reorder_overflow` and `holes_unrecoverable` do not grow; a stale tree node leaves 12 KiB per round and the hole/reorder budget kills the stream |
| `far.second_carrier` | after the first carrier and OPEN, a second TCP connection on index 1 sends PAD and the next DATA | the far answers the second HELLO and echoes the second record; an unanswered HELLO or a missing echo is the F6 stall |

### Fake far endpoint (`near.*`)

`adv_probe fakefar` listens on the near tunnel's peer port and learns the
near's nonce, index and generation from its own HELLO.

| Attack | What is sent | Safe result |
| --- | --- | --- |
| `near.resp_not_chunked` | 200 with `Content-Length` | carrier poisoned |
| `near.resp_500` | 500 response | carrier poisoned |
| `near.resp_hdr_big` | 9000-byte response header | carrier poisoned |
| `near.resp_close` | 200 chunked with `Connection: close` | carrier poisoned |
| `near.hello_wrong_nonce` | HELLO with a wrong nonce | carrier poisoned |
| `near.chunk_huge` | chunk size `ffffffff` | carrier poisoned |
| `near.split_record` | chunk 32 with `body_len=100` | carrier poisoned |
| `near.chunk_oversize` | chunk 8229 | carrier poisoned |
| `near.drip_pad` | drip chunk and PAD chunk between records | accepted, carrier stays up |
| `near.fbk_forged` | huge `uack`/`recv_cnt`, gen+1, path 15 | ignored; `uack` unchanged |
| `near.data_path` | DATA with the sid from the near's OPEN | the app reads exactly the sent bytes |
| `near.second_conn` | a second application after the first closed | accepted and usable |
| `near.fin_eagain` | raw near carrier; the fake far stops reading (4 KiB receive buffer), the app writes 560 KiB and `shutdown(SHUT_WR)`, then the far drains | the near's FIN crosses, the app reads EOF, `pump_hiwat` grew; a FIN freed on `-EAGAIN` never appears and the app times out |
| `near.rst_eagain` | same raw backpressure, the app closes with `SO_LINGER 0` | the near's RST crosses and `pump_hiwat` grew; a pinned RST freed on `-EAGAIN` never appears and the far never learns |

### Public port abuse (`pub.*`, `init.*`)

| Attack | What is sent | Safe result |
| --- | --- | --- |
| `init.echo_sane` / `init.final_echo` | 2-4 MiB echo | byte-exact |
| `pub.churn` | 10 000 sequential connect/send/close; about 31 min at 185 ms per cycle, so the public group needs `TBT_ADV_PUBLIC_TIMEOUT>=2400` | every connection usable; a closed stream frees the session |
| `pub.two` | two concurrent applications | both usable: `A usable=1`, `B usable=1`, `A after B close=1`; all three must be 1 to PASS |
| `pub.concurrent` | 100 concurrent connections, 4 KiB each | all echo byte-exact; isolation and recovery |
| `pub.halfclose` | app `shutdown(SHUT_WR)` | echo then EOF |
| `pub.rst` | 4 MiB then `SO_LINGER 0` | RST mid-stream, no warning |
| `init.src_key` | a `src=` line, then `nonce=` and a short `nonce=` | `src=` is refused, `nonce=` is accepted and rendered, a short nonce is refused |
| `pub.srcfilter` | far+near pinned with `nonce=` and far `peer=`, then an application from 127.0.0.2 | the application is served (no `src=` allow list exists; identity is the nonce) and both keys render in the canonical config |
| `pub.writer` (100 MiB) | app writes and never reads | backpressure: writes stop, no OOM |
| `pub.writer` (1024 MiB) | app attempts 1 GiB and never reads | backpressure, `MemAvailable` stays high |

### Control surface (`ctl.*`, `init.*`)

| Attack | What is sent | Safe result |
| --- | --- | --- |
| `ctl.nonroot` | uid 1000 opens `config` for write | EACCES/EPERM; config unchanged |
| `ctl.line513`, `ctl.line512` | a line at/just over the old 512-byte bound | refused or buffered; nothing applied |
| `ctl.line1025`, `ctl.line1024nonl` | line at/over the current 1024-byte buffer | `-E2BIG`; nothing applied |
| `ctl.line512nonl` | 512 bytes, no newline | buffered or refused; never applied |
| `init.cfg_roundtrip` | read the canonical config, write every line back | all lines accepted |
| `init.cfg_replay` | line-by-line replay during an 8 MiB transfer | no deadlock; old stream closes cleanly |
| `init.flush` | `flush` during an 8 MiB transfer | app sees EOF/reset, not a timeout |
| `init.rmmod` | `rmmod tbtun` during an 8 MiB transfer | rmmod returns, app sees a clean error, guest survives |
| `ctl.close_flush` | unterminated `tunnel` line flushed at process exit | no crash |

### Resource exhaustion (`init.*`)

| Attack | What is sent | Safe result |
| --- | --- | --- |
| `init.max_tunnels` | 80 tunnel lines with `max_tunnels=64` | the 65th is refused |
| `init.scale` | 32 near/far pairs, 16 carriers each | carriers up; memory reported; no OOM |
| `init.port_exhaust` | a 16-port block, then two 16-carrier tunnels | the third tunnel is refused (no `tunnels` row), or a dedicated `port_alloc_fail`/`port_exhaust`/`port_retry`/`degraded` counter or a `port block exhausted` log line names the reason; a bare `alloc_fail=16` with no visible error is a FAIL |
| `init.max_carriers` | `n=8` with `max_carriers=4` | refused |

### Kernel-state sanity (`ADV_KLOG`, `init.dmesg`, `console.klog.*`)

Every hostile-carrier attack calls `dmesg` before and after; any new
`WARNING:`, `BUG:`, `Oops`, `INFO: task`, `general protection fault`,
`refcount_t:`, `KASAN`, `UBSAN`, `Call Trace:` or `deprecated workqueue` line
is printed and fails the attack. The host also scans each console, because a
guest that hangs never prints its own dmesg. `init.slab_leak` runs 100
unauthenticated garbage connections and compares `/proc/slabinfo`.

## The isolated crash repro

`adversarial/crash-repro.sh` boots a minimal guest and writes an unterminated
`tunnel` line from a process that exits without closing the descriptor. The
proc file's release applies the line, and `tbt_tunnel_add()` dereferences
`current->nsproxy` after `exit_task_namespaces()`. The guest is expected to
hang after the oops, so the repro uses a 45 s QEMU bound and judges from the
console. It prints one `ADV_RESULT ctl.close_flush ...` line.

## What a safe run looks like

```
ATTACK                           RESULT DETAIL
-------------------------------- ------ ------
...
run.sh: 60 passed, 0 failed, 0 skipped
```

Every group reaches `TBT_ADV_DONE`, no `console.hang.*`/`console.missing.*`
rows appear, and there are no `console.klog.*` or `ADV_KLOG` lines. Any FAIL
is a finding in `findings.md`; the detail names the observed behaviour and
the console log holds the evidence.

## Limits

- The guest is one 512 MiB machine. The suite measures the direction and the
  amount of backpressure; it does not try to force an OOM kill.
- The fake far and the hostile carrier are both in the guest, so they can bind
  the ports the module dials. That is deliberate: the module's peer is
  untrusted by assumption.
- No TLS: `profile=http-kal` is plain HTTP by design. The QoS classification
  claim is `tests/qemu/` probe I, not this suite.
- `near.fin_eagain`/`near.rst_eagain` do not inject `-EAGAIN` directly; they
  make it overwhelmingly likely by shrinking the fake far's receive buffer,
  stopping its read loop, and pushing 560 KiB through one raw carrier whose send
  buffer is 512 KiB. 560 KiB is chosen to exceed the send queue but stay
  below queue + the stream's 64 KiB capture gate, so the capture can finish
  and queue the FIN/RST while the carrier is still full. The verdict requires `stats`' `hiwat` (the pump's
  `-EAGAIN` count) to grow, so a run that never reached backpressure cannot
  pass by accident. If a future kernel changes the buffer accounting enough
  that `hiwat` stays flat, the attack reports FAIL with `hiwat ... -> ...`
  unchanged rather than a false PASS; the same construction repeated in one
  guest would only widen the window, not prove the exact errno.
- On the 0.0.1 baseline both near backpressure attacks report
  `fin_seen=0`/`rst_seen=0` with `holes_unrecoverable=0` and
  `tx_ack == tx_seq`: the carrier's send queue is full, the pump cannot move
  the last retained frames, and the capture stops with `copied < rcv_nxt`, so
  the FIN is never emitted (`txq=0`, `retain_bytes=48 KiB`). The result line
  and the `NEARX_STATS` block are the evidence; this is a capture/queue-gate
  finding, not a timeout in the probe. The RST variant additionally shows the
  carrier replaced (`lost=1 replaced=1 connect_fail=1 streams_active=0`) once
  the fake far stops accepting the replacement, which is why the test keeps
  its own listener open only for the first connection.
- `far.overlap` uses 8 KiB DATA records because `TBT_PAYLOAD_MAX` is 8192; the
  straddle is produced by two out-of-order nodes plus a shorter direct record,
  not by a record longer than the wire limit.
- The suite owns only `adversarial/`. It copies `tests/qemu/mkinitramfs.sh`
  and never writes to `tbtun/`, `tests/qemu/`, `host/`, or `Makefile`.
