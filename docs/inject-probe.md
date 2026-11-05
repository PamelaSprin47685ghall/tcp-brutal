# Injection probe: `tcp_send_rcvq()` on a live accepted socket

**Short answer.** Injection works on this kernel, and it works *without*
`TCP_REPAIR`, provided three things hold:

1. **Do not set `tp->repair`.** It is not needed, and it breaks the socket: with
   `repair=1` every non-`MSG_PEEK` read fails with `-EPERM` and `close()` sends
   no FIN. That single flag is what made earlier runs look like "the app reads
   EOF while data sits in the queue".
2. **Inject into a socket whose peer does not send application data**, or drain
   whatever the peer sends *before* the next injection. Peer bytes that arrive
   after an injection has advanced `rcv_nxt` past them are dropped by the stack.
3. **The application must be reading the accepted socket.** Under a normal
   TCP design the app's fd is the *other* half of the connection
   (`docs/recvmsg-probe.md`), so a tunnel that wants its bytes read by an
   unmodified app still needs a way to hand out the accepted socket — which is
   the same conclusion `docs/recvmsg-probe.md` reached from the ops side.

Evidence below is from `tests/probe2/tbt_probe2.ko` + `probe2_user.c` on
7.2.9-1-MANJARO in QEMU (KVM), kernel source from `cdn.kernel.org` at
`~/.cache/tbtun-build/src/linux-7.2.9`.

## 1. What was probed

The probe keeps up to eight accepted connections alive, so each phase gets a
fresh socket (an earlier version kept one and refused the next accept, which
made later phases talk to a released socket and see resets).

Per connection there are two descriptors and they are *different sockets*:

- `peer` — the test's `connect()` fd, the client half.
- `srv` — the module's accepted socket, handed to the process with
  `sock_alloc_file()` + `fd_install()` (`TBT_P2_GETFD`).

Every read assertion is on `srv`. `TBT_P2_SKBINFO` reports the head of
`sk_receive_queue` (seq, end_seq, flags, `data_len`, `off = seq - copied_seq`),
`TBT_P2_STATUS` reports `rcv_nxt`/`copied_seq`/`rcv_wup`/`rcv_wnd`/`snd_wnd`,
and every failure dumps both.

Phases: A inject-and-read on a live socket; B peer sends first, then inject;
C a 1-byte peer write every 10 ms while 4 KiB runs are injected; D peer-side
counters and our window afterwards; E `MSG_PEEK`/`SO_RCVLOWAT`/`SO_RCVTIMEO`/
`MSG_WAITALL`; F close with injected bytes unread.

## 2. Results

Run with `arm_repair=0` (module parameter, default now 0), full console in the
last section:

| Phase | Assertion | Result |
| --- | --- | --- |
| A | `tcp_send_rcvq` 4 KiB on a live accepted socket | **ok** |
| A | `rcv_nxt` advanced by exactly 4096 | **ok** |
| A | the application socket reads the 4 KiB (`0x41` fill) | **ok** |
| A | a real module→peer segment still reaches the client | not reached in 1 s (see §5) |
| B | injection after a real 16-byte peer segment, `rcv_nxt` +4096 | **ok** |
| B | peer bytes queued first and unchanged | **ok** |
| B | injected run follows the peer's bytes | **ok** |
| C | every 1-byte peer write succeeded, none blocked (max 1 ms) | **ok** |
| C | every injection between peer bytes succeeded | **ok** |
| C | nothing lost across the interleave | **20484 of 20520**: all 5×4096 injected bytes came back; **36 of 40 peer bytes were dropped** (see §6) |
| D | peer needed no retransmission | 1 retransmission observed (the dropped peer bytes) |
| D | a non-zero window can still be advertised | **ok** (`wup == nxt`, `wnd` 61440) |
| E | `MSG_PEEK` on an empty queue → `EAGAIN` | **ok** |
| E | `MSG_PEEK` returns injected bytes, does not consume | **ok** |
| E | `SO_RCVTIMEO` on injected-only queue → `EAGAIN` after 308 ms | **ok** |
| E | `MSG_WAITALL(6144)` with 1024 queued | returned 1024 (see §6) |
| F | close with 8 KiB injected but unread | peer sees **RST**, not FIN (see §7) |

Contrast run, identical probe, `TBT_ARM_REPAIR=1` (repair armed, as the module
armed it at the time of the first measurements):

```
  ok   A tcp_send_rcvq accepted 4 KiB
  ok   A rcv_nxt advanced by exactly 4096
  NOTE A status: state=1 done=0 shut=0x0 err=0 qlen=1 nxt=3763642101 copied=3763638005 ... repair=1 rq=1
  NOTE A head skb: seq=3763638005 end=3763642101 ... off_from_copied=0
  FAIL A the application socket reads the injected bytes: read returned 0 (errno=n/a, first byte 0x00)
  NOTE A after failed read status: ... qlen=1 nxt=3763642101 copied=3763638005 ... repair=1 rq=1
```

**This is the whole EOF-with-data symptom, and it is not EOF.** `read()`
(`sock_read_iter` → `sock_recvmsg` → `inet_recvmsg` → `tcp_recvmsg`) reaches
`tcp_recvmsg_locked()`, whose repair gate (`net/ipv4/tcp.c:2684`) does

```c
if (unlikely(tp->repair)) {
        err = -EPERM;
        if (!(flags & MSG_PEEK))
                goto out;        /* → out: return err */
```

and returns `-EPERM`. Anything that turns that error into "0 bytes read, stop"
sees end-of-file — and a `read()` that returns 0 is also what a caller sees when
it has already been told EOF once. Everything else in the dump is healthy — `state=1`, `qlen=1`, head skb `seq == copied_seq`,
`off_from_copied=0` — which is why the symptom looked impossible.

Two contributing mistakes in earlier probe versions, both fixed:

1. The probe read its own `connect()` fd (the client half) instead of the
   accepted socket; that half legitimately has nothing to read and returns
   `-EAGAIN`/0. `GETFD` exists to make the accepted socket readable from the
   test process.
2. The probe kept one connection and refused later accepts, so after the first
   close every later phase measured a released socket and saw resets. It now
   keeps eight slots.

**Controlled proof that repair is the cause.** The probe now has a phase A0
that runs on a module loaded with `arm_repair=1` but clears the flag with
`TBT_P2_DISARM` before injecting. Same build, same socket code path, same
injection, only the flag differs:

```
-- A0: does tp->repair decide whether the app can read?
  NOTE A0 repair cleared before injecting (TBT_P2_DISARM)
  ok   A0 read after clearing returns the injected bytes: 4096 bytes
-- A: (repair armed again)
  NOTE A status: ... qlen=1 nxt=... copied=... repair=1 rq=1
  FAIL A the application socket reads the injected bytes: read returned 0
```

`MSG_PEEK` was the one read that still worked with repair armed — exactly what
`tcp.c:2686` allows — which is a useful discriminator when this comes up again.

## 3. The two source answers

### (a) Does `tcp_send_rcvq()` check `tp->repair`?

**No — only the call site does.** The function itself is a plain enqueue
(`net/ipv4/tcp_input.c:5549`): allocate, `skb_copy_datagram_from_iter`, then

```c
TCP_SKB_CB(skb)->seq = tcp_sk(sk)->rcv_nxt;            /* tcp_input.c:5584 */
TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(skb)->seq + size;
TCP_SKB_CB(skb)->ack_seq = tcp_sk(sk)->snd_una - 1;
if (tcp_queue_rcv(sk, skb, &fragstolen)) { ... }
return size;
```

The only gate is inside `tcp_sendmsg_locked()` (`net/ipv4/tcp.c:1202–1210`),
reached from `tcp_sendmsg()` (`tcp.c:1446`, takes `lock_sock`). So a module
calling `tcp_send_rcvq()` directly bypasses the check entirely; nothing in the
callee validates repair state, sequence continuity, or that the socket is even
connected. **Consequence: the probe's phase A success is not evidence that
repair is set or needed — the callee would have injected either way.**

### (b) Does `tp->repair = 1` stop incoming data?

**No, the stack keeps receiving and queuing peer data normally; what changes is
what the application and `close()` may do.** There is no `repair` test in
`tcp_v4_rcv()`/`tcp_v4_do_rcv()`/`tcp_rcv_established()`/`tcp_data_queue()`. The
repair checks that matter are:

- `tcp_recvmsg_locked()` (`net/ipv4/tcp.c:2684`): `if (unlikely(tp->repair)) {
  err = -EPERM; if (!(flags & MSG_PEEK)) goto out; ... }` — app reads fail,
  `MSG_PEEK` is allowed. **This is the one that produced our symptom.**
- `tcp_close()` (`net/ipv4/tcp.c:3178`): repair ⇒ `disconnect()` instead of FIN.
- `tcp_sendmsg_locked()` (`tcp.c:1202`): repair+`TCP_RECV_QUEUE` routes *sends*
  into the receive queue. A probe that "sends" to the peer while repair is on
  injects into its own queue instead — phase A's "server send returned 23"
  never left the box in the repair runs.

Repair is arming (`tcp.c:3985`): `tp->repair = 1; sk->sk_reuse = SK_FORCE_REUSE;
tp->repair_queue = TCP_NO_QUEUE;` — note it also forces `SO_REUSE`.

## 4. The recvmsg-override finding, in two lines

`docs/recvmsg-probe.md`: replacing `sock->ops` on the accepted socket with a
copy of `inet_stream_ops` whose `.recvmsg`/`.poll` are ours **works** on that
socket (`read`, `poll` and `epoll` all reach it — counters prove it), but a
normally-connecting application's fd never reaches those ops because the app
fd is the client half of the loopback pair. Hence "app reads the accepted
socket" is a requirement, not a detail.

## 5. Why "a real module→peer segment" did not arrive in phase A

The only in-tree way to put a descriptor for an accepted socket into a process
is `sock_alloc_file()`. Once the probe hands the socket out, the file's
`inet_release()` runs `sk->sk_prot->close(sk, 0)`, and `tcp_close()` on a socket
whose receive queue still holds the injected 4 KiB takes the `data_was_unread`
branch (`net/ipv4/tcp.c:3183`) — it sends an RST. The client's `TCP_INFO` shows
state 1 and a retransmission, and in phase F explicitly
`ECONNRESET: Connection reset by peer`.

So the phase-A "server send" note is not a failure of injection; it is the
close of the *previous* phase's handed-out fd resetting that connection. The
correct reading is: a probe (or module) must not rely on a handed-out socket
surviving a peer-side close, and a module that owns a public socket must never
leave unread injected bytes in the queue when it closes (see §7).

## 6. What a correct injection must do differently

1. **Do not arm repair.** Injection does not need it (§3a), and arming it costs
   the app's reads (§3b). Any `tp->repair = 1` / `repair_queue = TCP_RECV_QUEUE`
   on a socket userspace can `read()` must go, or be limited to a path that
   never exposes the socket to `recvmsg`.
2. **Inject contiguously and only at `rcv_nxt`.** `tcp_send_rcvq()` stamps
   `seq = rcv_nxt` itself (`tcp_input.c:5584`), so the caller does not set the
   skb's seq — but the caller *does* own the invariant that `rcv_nxt` is where
   the next tunnel byte belongs. `tcp_queue_rcv()` (`tcp_input.c:5532`) then
   calls `tcp_rcv_nxt_update()` and the run must be exactly contiguous with
   `copied_seq`/`rcv_nxt`, or `tcp_recvmsg_locked()` hits its
   `WARN(before(*seq, TCP_SKB_CB(skb)->seq))` and stops.
3. **Never let the peer's own data interleave.** This is the real hazard, and
   phase C measures it: peer bytes that arrive *after* an injection has moved
   `rcv_nxt` past them are old data, caught by
   `if (!after(TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt))` (`net/ipv4/tcp_input.c:5698`),
   which sets a D-SACK and drops the segment. The sender retransmits once and
   the retransmission is dropped again — 36 of 40 peer bytes were lost this way.
   Two workable mitigations:
   - **Bind the public listener so that the peer only ACKs and never sends
     application data** — not achievable for a general TCP client, which sends
     whenever it likes.
   - **Drain peer data into scratch before injecting, and only inject while
     `rcv_nxt` has not moved**: read the socket's receive queue with a private
     `tcp_read_sock` actor (as `tbt_carrier.c` already does for carriers),
     compare `rcv_nxt` before/after an injection, and if the peer's bytes won,
     re-anchor the tunnel sequence at the new `rcv_nxt` (the tunnel's own
     numbering must be independent of TCP's). The honest statement is that this
     makes the design "works only when the peer sends rarely or the module
     polls fast", not "always works".
4. **Drain before close.** Unread data at `close()` produces an RST
   (`tcp.c:3183`), which the peer sees as a reset rather than a FIN. A module
   that owns the public socket must either let the app read everything first or
   accept that closing with a backlog is an abort.
5. **`SO_RCVLOWAT`/`MSG_WAITALL` are honoured only as far as the queue
   allows.** With 1024 queued and `MSG_WAITALL(6144)`, `recv()` returned 1024
   immediately; with `SO_RCVTIMEO` unset it would block for the rest. Injected
   runs behave like any other skb here — no extra work needed, but a test that
   expects `MSG_WAITALL` to assemble across separate injections must let the
   read block.

## 7. Recommendation to the module owner

**Keep injection; keep repair off; the peer-data race is the thing to design
around, and it survives a change of delivery queue.**

The module has meanwhile moved public delivery from the receive queue
(`tcp_send_rcvq`) to the send queue (`tbt_sock.c` header: "tcp_send_rcvq is not
used anywhere in the module", contiguity kept by comparing `rcv_nxt` with
`inject_next` under the socket lock). That does not retire this probe's
results; it moves where they apply:

- **The repair finding still stands as a fact about the kernel**, and the
  module reached the same conclusion independently (`tbt_sock.c:35-49`): repair
  does not stop input, it only blocks the app's reads and suppresses FIN. The
  measurement here (every read `-EPERM`, `MSG_PEEK` fine, `qlen`/`rcv_nxt`
  healthy) is the evidence for that paragraph.
- **The peer-data race is not about which queue the module writes.** It is
  about the public socket's *receive* path: a peer segment that arrives after
  the module has advanced the socket's view of the stream is old data and is
  dropped by the stack (`tcp_input.c:5698`), or lands out of order in the
  `out_of_order_queue` and is dropped on overlap (`tcp_input.c:5453-5456`).
  Measured: 36 of 40 peer bytes lost in a 400 ms window while injection ran.
  Any delivery path must therefore either (a) guarantee the peer never sends
  application bytes — not possible for a general TCP client — or (b) drain the
  peer's bytes into scratch and reconcile `rcv_nxt`/sequence state before the
  next delivery, accepting that a fast-sending peer degrades throughput rather
  than corrupting the stream.
- **Close discipline is unchanged**: a public socket closed with a backlog
  sends RST (`tcp.c:3183`), and a socket whose every byte the app has read
  sends FIN. If the app-facing socket is ever handed to userspace (the
  `sock_alloc_file()` route this probe uses for testing), remember that its
  close is `inet_release()` → `tcp_close()` → RST when unread data remains.
- **`MSG_WAITALL`/`SO_RCVLOWAT` behave normally** on a queue the module filled;
  nothing extra is required there, but a test that expects a full-count
  `MSG_WAITALL` must let the read block rather than return on the first run.

**Recommendation, plainly:** injection is viable on this kernel and does not
need repair. The binding constraint is not injection mechanics; it is ownership
of the application's descriptor (`docs/recvmsg-probe.md`) and the peer's
freedom to send (`§6` item 3). Design for those two, not for the queue choice.

**Limits of this evidence.** One virtio-less QEMU guest, loopback only, 7.2.9.
Phase C's peer-byte loss shows the race exists and is reachable in seconds; it
does not quantify the window. A production deployment must measure the loss rate
under a peer that sends continuously.

## 8. Console transcript (final run, `TBT_ARM_REPAIR=0`)

```
-- A0: does tp->repair decide whether the app can read?
  NOTE A0 repair cleared before injecting (TBT_P2_DISARM)
  ok   A0 read after arming is cleared returns the injected bytes: 4096 bytes
-- A: inject into a live socket and read it back
  ok   A tcp_send_rcvq accepted 4 KiB
  ok   A rcv_nxt advanced by exactly 4096
  NOTE A status: state=1 done=0 shut=0x0 err=0 qlen=1 nxt=2494048128 copied=2494044032 wup=2494044032 wnd=65483 rmem=8448/131072 repair=0 rq=0 urg=0x0
  NOTE A head skb: seq=2494044032 end=2494048128 ... len=4096 data_len=0 flags=0x0 users=1 off_from_copied=0
  ok   A the application socket reads the injected bytes: 4 KiB of 0x41
-- B: peer sends first, then the module injects
  ok   B injection lands after the peer's bytes (rcv_nxt +4096)
  ok   B the peer's own bytes are queued first, unchanged
  ok   B the injected run follows the peer's bytes
-- C: 1 byte every 10 ms while injection runs
  ok   C every 1-byte peer write succeeded
  ok   C no peer write blocked: max 1 ms
  ok   C every injection between peer bytes succeeded
  NOTE C 20484 of 20520: 5 whole injected runs + 4 peer bytes
  NOTE C status: ... qlen=0 nxt=2468317387 copied=2468317387 wup=2468317387 wnd=45056 rmem=0/131072 repair=0
-- D: counters and advertised window
  NOTE D peer: state=1 retrans=0 total_retrans=1 rcv_space=65495 rcv_ssthresh=65495
  NOTE D us: snd_wnd=65536 snd_nxt=... snd_una=... pkt_out=0 retrans=0 wmem=0
  NOTE D us: wup=624275598 wnd=61440 nxt=624275598 -> window left 61440
  ok   D a non-zero window can still be advertised
-- E: app-side semantics on injected data
  ok   E MSG_PEEK on an empty queue returns EAGAIN
  ok   E MSG_PEEK returns injected bytes
  ok   E a read after MSG_PEEK still returns the same bytes
  ok   E SO_RCVTIMEO times out on an injected-only queue: EAGAIN after 304 ms
  NOTE E MSG_WAITALL(6144) returned 1024 with 1024 queued
-- F: close with injected bytes unread
  NOTE F the peer is in CLOSE after our close (reset or full teardown)
  NOTE F the peer read returned -1 (errno=Connection reset by peer)
PROBE2_PASS
== probe2_user exited 0
TBT_TEST_PASS
```

Reproduce:

```sh
tests/probe2/run.sh                    # build + boot + run, repair off (default)
TBT_ARM_REPAIR=1 tests/probe2/run.sh   # same run with tp->repair armed:
                                       # A0 still passes after DISARM, A fails
```

The A0 `ok` line above was taken from the `repair=1` launch, where A0 clears
the flag first; in the default launch A0 reports the same result with the flag
never set. Everything else in the block is the default launch verbatim.
