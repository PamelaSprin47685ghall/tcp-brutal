# Adversarial findings

Build under test: `tbtun.ko` sha256
`b45501751182b5acd10e5d5b4881ef2b4171a72eb16d34dcc2bbc162e81d3590`
(2026-11-05, QEMU 7.2.9-1-MANJARO, 512 MiB, KVM).  The module was being edited
while this suite ran; every result below was reproduced against that hash.
`adversarial/run.sh` prints the hash it used, and each group leaves a console
log with the evidence.

The verdicts come from the wire, from `/proc`, and from `dmesg`.  A PASS means
the module survived the attack and the carrier/session was left in a safe
state; a FAIL means the attack succeeded, the guest hung, or a kernel warning
appeared.  The full table is the output of `adversarial/run.sh`.

## Summary

Every row below was observed against `b4550175...` (the full sha is at the
top).  `run.sh` stamps the sha of the image each group actually booted on
every row, so a rebuild between groups is visible in the table; the rows here
are from the run whose `results.txt` is quoted.

| ID | Attack | Observed | Severity | Build |
| --- | --- | --- | --- | --- |
| F1 | `ctl.close_flush`: unterminated `tunnel` line flushed at file release | kernel NULL deref in `tbt_tunnel_add`; guest hangs | **critical** (fix in progress) | b4550175 |
| F2 | `init.rmmod`: `rmmod` during an 8 MiB transfer | instruction fetch from freed module text; oops in TCP input; guest hangs | **critical** | b4550175 |
| F4 | `pub.churn`: 10 000 sequential connect/send/close | only 1 connection usable; the rest refused for the session's lifetime | high | b4550175 |
| F5 | `pub.halfclose`: app `shutdown(SHUT_WR)` | echo arrives, EOF never does; app hangs until its timeout | high | b4550175 |
| F6 | far-side hostile carrier (`far.*`), fake far (`near.*`) | after the first carriers, accepted connections are not read; hostile chunks are neither processed nor refused; far-side DATA never reaches the target | high | b4550175 |
| F7 | any session with a stream | `workqueue: work func tbt_fbk_work ... enqueued on deprecated workqueue` | high (kernel warning) | b4550175 |
| F8 | `init.src_key` / `pub.srcfilter` | `src=` is not a key; a connection from 127.0.0.2 is accepted | medium | b4550175 |
| F9 | `far.srcaddr_free`: replayed nonce from 127.0.0.2 | joins a free carrier slot; no source binding | medium | b4550175 |
| F10 | `init.max_tunnels`: 80 tunnel lines | all 80 accepted; `max_tunnels=64` is not enforced | medium | b4550175 |
| F11 | `init.max_carriers`: `max_carriers=4`, `n=8` | accepted; the load-time parameter does not bound the config | medium | b4550175 |
| F12 | `init.port_exhaust`: 16-port block, two 16-carrier tunnels | third tunnel accepted, `alloc_fail=10`, carriers retry forever with no error | medium | b4550175 |

**Deferred, not filed:** the `flush`/teardown hang (see the note after F2).
It is in the carrier replacement/retire area the owner is rewriting.  The
suite still reports `console.hang.*` for it, but it is not counted as a filed
defect until it is re-tested against the owner's next build.

## F1 - close-flush NULL dereference (critical)

**Sent.** A valid tunnel directive with no trailing newline, written by a
process that exits without closing the descriptor:

```
adv_probe partial /proc/net/tbtun/config \
  "tunnel 1 role=near profile=raw listen=127.0.0.1:19102 peer=127.0.0.1:19101 n=1 m=0"
```

The proc file's `release()` applies the buffered line (DESIGN §8.4.1 makes
that the documented close-flush).  `tbt_tunnel_add()` then evaluates
`get_net(current->nsproxy->net_ns)`.  `__fput()` runs from task_work after
`exit_task_namespaces()`, so `current->nsproxy` is NULL.

**Observed.** Immediate kernel oops and a hung guest:

```
BUG: kernel NULL pointer dereference, address: 0000000000000028
Oops: Oops: 0000 [#1] SMP NOPTI
RIP: 0010:tbt_tunnel_add+0xa3/0x380 [tbtun]
Call Trace:
 tbt_config_apply+0xbd/0x220 [tbtun]
 tbt_cfg_release+0xf2/0x100 [tbtun]
 close_pdeo -> proc_reg_release -> __fput -> task_work_run -> do_exit
Fixing recursive fault but reboot is needed!
```

**Repro.** `adversarial/crash-repro.sh` boots a minimal guest and prints
`ADV_RESULT ctl.close_flush FAIL ...` with the oops above; the QEMU run is
bounded at 45 s because the machine does not recover.

**Status.** Reported; the trunk owner is fixing it (capture the `struct net *`
at open time instead of reading `current->nsproxy` in the deferred release,
plus render/parse symmetry so the far line round-trips).  The repro is kept
as the regression test; it is expected to flip to PASS once that build lands.

**Notes.** On an earlier build (04:11) the same crash was reached by the
documented `cat config > config`: the canonical far line rendered
`peer=0.0.0.0:0`, `tbt_parse_line` refused it ("port must be 1..65535"), the
write failed mid-file, and the partial next line was flushed at close.  On the
build above the round-trip passes (`init.cfg_roundtrip PASS`), so the normal
`cat` path no longer trips it; the close-flush contract still does.  The
suite now also runs `cat config > config` with a far tunnel (`cat.cfg_self`)
and a rejected line followed by a far line (`cat.cfg_mix`) to pin both
symmetry directions.  A `del`/`flush` line does not use `nsproxy` and
survives.

## F2 - rmmod mid-transfer: callback into freed module text (critical)

**Sent.** `setup_raw` (one far, one near tunnel), an 8 MiB echo transfer with
20 ms between 64 KiB chunks, then `rmmod tbtun` after one second.  The
application keeps using its socket and closes it.

**Observed.** `rmmod` returns, then the application's close/FIN reaches the
accepted socket that the module detached but never released:

```
BUG: unable to handle page fault for address: ffffffffc0606d40
#PF: supervisor instruction fetch in kernel mode
RIP: 0010:0xffffffffc0606d40
Call Trace:
 <IRQ>  ? tcp_fin -> tcp_data_queue -> tcp_rcv_established -> tcp_v4_rcv
 <TASK> ? tcp_send_fin -> __tcp_close -> inet_release -> sock_close -> __fput
Comm: adv_probe
```

The faulting address is the module's old text.  `tbt_session_destroy()`
unlinks the stream, cancels `w_kill`, calls `tbt_sock_detach()` and frees the
stream, but only `tbt_kill_work()` ever calls `sock_release()` on the public
socket (`tbt_sock.c`).  `tbt_sock_detach()` leaves `sk_data_ready`,
`sk_write_space` and `sk_state_change` pointing at module functions, so a
socket that outlives the module calls freed text on the next packet.

**Repro.** `TBT_ADV_GROUPS=rmmod adversarial/run.sh --no-build`; evidence in
`adversarial/console-rmmod.log`.

The same missing release is visible without unload: `init.flush_midstream`
and `init.cfg_replay` leave the application in `ECHO_TIMEOUT op=read`
(no EOF, no reset) because the accepted socket is neither closed nor
released.  That is the leak; the teardown *hang* seen in the same phases is
deferred below.

**Severity.** Critical: use-after-free of module text; the guest hangs after
the oops.

## Deferred observation - teardown hang (not filed)

`init.flush`, `init.cfg_replay`, `init.scale` (after 1024 carriers), and the
far/near/slab groups each printed `PHASE_START` without `PHASE_END` while
tearing down; the guest had to be killed by the QEMU timeout.  The app-side
symptom (`ECHO_TIMEOUT op=read`) is F2's leaked socket; the hang itself is in
the carrier replacement/retire area the owner is rewriting.  Per the
instruction not to file a moving-target hang, this is recorded here and in
the suite's `console.hang.*` rows, and will be re-tested against the owner's
next build before it is filed.

## F4 - a session serves exactly one public connection (high)

**Sent.** 10 000 sequential `connect`, one-byte send, one-byte read, `close`
cycles against the public port; then the concurrent two-connection test.

**Observed.**

```
ADV_RESULT pub.churn FAIL 10000 connect/send/close cycles:
  usable=1 refused=9999 other=0
ADV_RESULT pub.two PASS A usable=1, B refused=1, A still usable=1
```

The first connection works; after it closes, every later connection is refused
(`sock_release` of the accepted socket, so the client sees EOF/RST).  The
numbers above are the `b4550175` evidence and stay as history.

**Status on the current build.** `pub.two` now requires both concurrent
connections to work: it PASSes only when `A usable=1`, `B usable=1` and
`A after B close=1`, and the current build meets that row.  Sequential reuse
is still bounded: the measured `usable` count stops at 128, exactly
`TBT_STREAMS_MAX` in `tbt_sock.c`/`tbt_session.c`, so 10 000 churn cycles do
not complete.  That 128-slot limit is a separate, still-unfixed defect; it
does not revive the historical one-connection limit above.

**Root cause.** `tbt_stream_start()` refuses with `-EBUSY` whenever
`sess->far_sid != 0`.  `far_sid` is written when the first public connection is
accepted and on allocation failure; it is never cleared when the stream dies.
The far side's `far_sid` has the same lifetime, so a new near session is also
refused.  One unauthenticated connection (even one that sends nothing and
closes) permanently disables the tunnel until it is reconfigured.

**Severity.** High: remote denial of service, no credentials, one packet.

## F5 - half-close never reaches the application as EOF (high)

**Sent.** `connect`, write `half-close`, `shutdown(SHUT_WR)`, read until EOF,
against the deterministic echo target (it echoes until EOF, shuts its write
side and closes).

**Observed.** `ADV_RESULT pub.halfclose FAIL app half-close: echo 10 bytes,
EOF=0` after a 3 s wait.  The echo is byte-exact; the EOF is missing, so an
application that waits for the peer's FIN hangs until its own timeout.

**Root cause.** The FIN path from the far target's close back to the near
application is not completing (`tbt_ctrl_work`/`tbt_tx_fin`/
`tbt_sock_shutdown_wr`); the target closes deterministically, so this is the
module, not busybox `nc`.

**Severity.** High: a normal TCP half-close pattern stalls.

## F6 - far-side carrier input and delivery stall (high)

**Sent.** The full hostile-carrier catalogue against a far tunnel
(`profile=http-kal`, `n=4`): wrong nonce, duplicate index, source-address
replay, `ffffffff` chunk, split record, oversize chunk, missing Host, 9000-byte
header, second sid, forged FBK, profile 0/9, then ping/pong DATA; and the fake
far catalogue against a near tunnel.

**Observed.** The first two far connections behave (`far.garbage`,
`far.hello_garbage` PASS), and `far.anchor`, `far.fbk_forged`,
`far.hello_profile0/9` and `far.bodylen_tail` pass.  The rest time out instead
of being refused:

```
ADV_RESULT far.wrong_nonce FAIL ... join_bad+nonce_reject 0 -> 0
ADV_RESULT far.dup_index FAIL ... join_dup 0 -> 0
ADV_RESULT far.srcaddr_dup FAIL ... refused as a duplicate
ADV_RESULT far.chunk_huge FAIL ...
ADV_RESULT far.split_record FAIL ...
ADV_RESULT far.drip_split FAIL ...
ADV_RESULT far.chunk_oversize FAIL ...
ADV_RESULT far.no_host FAIL ...
ADV_RESULT far.headers_big FAIL ...
ADV_RESULT far.open_other_sid FAIL ...
ADV_RESULT far.data_path FAIL ... stats rx_records=0 capture_runs=0 ...
```

The far logs `carrier ACCEPTED` for each connection, but its receive work does
not process the HELLO/chunk, and the connection is left open until the test's
timeout.  The same stall is visible without HTTP: `GARBAGE_DONE n=100
closed=2` (98 unauthenticated garbage connections were not closed), and the
near side stops dialling replacements after two poisoned carriers
(`near.scenario FAIL the near side never dialled scenario 2`).  The far-side
DATA path is dead in this state: the anchor's ping/pong never reaches the
target, and the far's capture counters stay zero.

**Counter note.** The `join_bad`/`join_nonce_reject`/`join_dup` values staying
0 is consistent with the stall: the carrier never processed the HELLO, so no
join path ran.  It is not filed as a counter bug.  The independent anomaly is
that a debug run logged 15 `carrier ACCEPTED` lines while the same session
reported `rx_adopt_events=2` and `streams_total=2`; the suite now dumps
`/proc/net/tbtun/stats` after the far phase (`FAR_STATS_BEGIN`) so the adopt
count can be compared with the number of connections the attacker made.

**Severity.** High: the far carrier endpoint becomes unusable and hostile
input is neither parsed nor refused.  The exact cause is in the current
carrier accept/read/replacement path (the build added `join_retries` and a
`TBT_F_RETIRE` control frame); it is for the module owner to localise.  The
suite reports it as a stall, not as a parser bug.

## F7 - deprecated workqueue warning on every session (high)

**Observed.** The first feedback tick of a session:

```
workqueue: work func tbt_fbk_work [tbtun] enqueued on deprecated workqueue.
Use system_{percpu|dfl}_wq instead.
```

`init.dmesg FAIL 1 kernel warning/BUG/oops lines`.  The module queues
`w_fbk`, `w_pump` and `w_kill` on `system_unbound_wq`; 7.2 marks it deprecated.

**Severity.** High by the ticket's rule (a kernel warning is high), even
though the queue still runs.

## F8 - the documented `src=` inbound allow list does not exist (medium)

**Observed.** `ADV_RESULT init.src_key FAIL src= is not a key; the documented
inbound allow list does not exist`; `ADV_RESULT pub.srcfilter FAIL connection
from 127.0.0.2 was accepted=1: no src= allow list exists`.  `srcaddrs=` is the
outgoing carrier source address (`tbt_carrier.c`), not an inbound filter.

**Severity.** Medium: DESIGN §13 promises an address allow list; the public
port accepts any source.

## F9 - a replayed nonce joins from a different source address (medium)

**Sent.** With the far session's live nonce, connect from 127.0.0.2 and send a
HELLO for a free carrier index.

**Observed.** `ADV_RESULT far.srcaddr_free FAIL same nonce from 127.0.0.2 on a
free index: ACCEPTED (no source binding; replayed nonce hijacks a slot)`.
The same nonce and index from 127.0.0.2 is refused as a duplicate (correct);
a free index is accepted.  The first-nonce source check in `tbt_rx_hello`
only applies when the far tunnel has a configured `peer=`; the common far
config has no peer, so the first joiner defines the nonce and any later
replay from any address can join a free slot.  The nonce is cleartext on the
wire; the `Host` header is syntax-checked, not matched against the listener.

**Severity.** Medium: on-path/replay attacker can add a carrier to a session;
the design says to wrap in WireGuard when peers are untrusted, but §13's
"nonce + Host match prevents a third party" is stronger than what is built.

## F10 - `max_tunnels` is not enforced (medium)

**Observed.** 80 far tunnel lines were accepted with the default
`max_tunnels=64`: `ADV_RESULT init.max_tunnels FAIL 80 tunnels accepted
although max_tunnels=64`.  `max_tunnels` is declared and validated in
`tbt_core.c` but never consulted by `tbt_tunnel_add()`.

## F11 - `max_carriers` is not enforced (medium)

**Observed.** Loaded with `max_carriers=4`, a tunnel with `n=8` was accepted:
`ADV_RESULT init.max_carriers FAIL n=8 accepted with max_carriers=4`.
`tbt_cfg_sane()` checks `TBT_MAX_CARRIERS_LIMIT`, not the parameter.

## F12 - port-block exhaustion is silent (medium)

**Sent.** `ports 40000-40015` (16 ports), a 16-carrier near tunnel, then a
second 16-carrier near tunnel.

**Observed.** `ADV_RESULT init.port_exhaust FAIL port block exhausted:
tunnel 3 accepted, alloc_fail=10, carriers retry forever with no
operator-visible error`.  `tbt_car_connect_work()` returns to the 200 ms retry
loop; the tunnel line succeeded, so the operator sees a configured tunnel
with no carriers and no error.

## What held

These attacks were clean against the build above:

* 2-4 MiB echo byte-exact (`init.echo_sane`, `init.final_echo`).
* Concurrent second connection refused while the first keeps working
  (`pub.two`).
* RST mid-stream, 4 MiB (`pub.rst`).
* 100 MiB and 1 GiB writes with no reader: backpressure stops the writer at
  ~75 MiB; `MemAvailable` 412 -> 262 MiB, no OOM (`pub.writer`).
* 1024 carriers (32 pairs x 16): all up; `MemAvailable` 404 -> 329 MiB
  (`init.scale`).
* uid-1000 write to `config`: EACCES; config unchanged (`ctl.nonroot`).
* 512/513/1024/1025-byte lines: clean refusal/buffering, nothing applied
  (`ctl.line*`).
* Canonical config replay (line by line): byte-stable on this build
  (`init.cfg_roundtrip`).
* Forged FBK with huge `uack`/`recv_cnt`, wrong generation and path 15:
  ignored (`far.fbk_forged`, `near.fbk_forged`).
* `ffffffff` chunk, oversize chunk, split record and drip-in-record on the
  near side: carrier poisoned, no warning (`near.resp_*`, `near.chunk_*`).
* Garbage and HELLO-then-garbage on the far side: carrier closed, no warning
  (`far.garbage`, `far.hello_garbage`).
* 100 unauthenticated garbage connections: no `kmalloc` growth was measured
  (`init.slab_leak PASS`), but the far closed only 2 of the 100, so this run
  does not disprove the code-level dead-carrier retention; the carrier object
  is still freed only at session teardown.

## Reproducing

```
adversarial/run.sh                 # all groups + crash repro; prints the table
TBT_ADV_GROUPS="public control" adversarial/run.sh --no-build
TBT_ADV_GROUPS=far adversarial/run.sh --no-build
adversarial/crash-repro.sh         # F1 alone
```

Each group leaves `adversarial/console-<group>.log`; the crash repro leaves
`adversarial/crash-console.log`.  The module hash is printed at the start of
`run.sh` and is the first line of this document.
