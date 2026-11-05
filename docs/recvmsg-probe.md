# Can a module own an accepted TCP socket's read()/poll()?

**Short answer: not for the application's fd.** Replacing `sock->ops` on the
accepted socket with a copy of `inet_stream_ops` whose `.recvmsg` and `.poll`
are ours **works perfectly — on that socket object**. It does nothing for the
application, because the application's descriptor is a *different* socket: the
client half of the loopback connection. The kernel hands sockets to userspace in
exactly one place, and it is not the accept path.

Both claims were measured in QEMU on 7.2.9-1-MANJARO with `tests/probe/tbt_probe.ko`
and `tests/probe/probe_user`; the console transcript is at the end.

## 1. What the probe does

`tests/probe/tbt_probe.c` (throwaway, not in `SRCS`, not in the DKMS tarball):

- `sock_create_kern` / `kernel_bind` / `kernel_listen` on 127.0.0.1:19200.
- An accept work item scheduled from `sk_data_ready`.
- On accept: `newsock->ops = &probe_ops`, where `probe_ops` is a
  `__ro_after_init` copy of `inet_stream_ops` with `.recvmsg = probe_recvmsg`
  and `.poll = probe_poll`. Everything else — `.release`, `.shutdown`,
  `.setsockopt`, … — stays inet's.
- The payload (`"PROBE-OK\n"` repeated to 64 KiB) lives in the module, not in
  `sk_receive_queue`. `recvmsg` copies out of it; `poll` reports
  `EPOLLIN|EPOLLRDNORM` when bytes remain and `EPOLLHUP|EPOLLRDHUP` at EOF;
  a delayed work item calls
  `wake_up_interruptible_poll(sk_sleep(sk), EPOLLIN|EPOLLRDNORM)` 1500 ms after
  the descriptor is handed out.
- `/dev/tbtprobe`: `GETFD` wraps the accepted socket in a file with
  `sock_alloc_file()` and installs it in the caller's fd table; `STATS` returns
  counters for `.recvmsg`, `.poll`, wakes; `SHUTDOWN` closes the socket.

`tests/probe/probe_user.c` measures twice: once with its own connected socket
(phase A), once with the fd the module hands out (phase B).

## 2. Results

| | Phase A: the application's own fd | Phase B: the module's accepted socket |
| --- | --- | --- |
| first `poll(fd, POLLIN, 1000)` | 0 (nothing yet) | 0 (payload not released) |
| second `poll(fd, POLLIN, 5000)` | **0 — no POLLIN, ever** | **1, POLLIN** after the delayed wake |
| `read()` | `-1 EAGAIN` | 4096 bytes of `PROBE-OK` |
| `MSG_PEEK` | — | 61440 bytes, offset unchanged |
| read to EOF | — | exactly 65536 bytes, then `read()==0` |
| `epoll_wait()` | — | `EPOLLIN` from the same wake |
| module counters | `recvmsg_calls=0 poll_calls=0` | `recvmsg_calls=5 poll_calls=4 wakes=1` |

So:

- **`read()` does go through `ops->recvmsg`.** `sock_read_iter()` calls
  `sock_recvmsg()`, which is `INDIRECT_CALL_INET(READ_ONCE(sock->ops)->recvmsg,
  inet6_recvmsg, inet_recvmsg, ...)` (`net/socket.c:1123–1133`). `read()` and
  `recv()` are the same path; nothing bypasses it. In phase B the counter rose
  from 0 to 5 across four reads, one `MSG_PEEK` and one EOF read.
- **`poll()` and `epoll()` both go through `ops->poll`.** `sock_poll()` calls
  `ops->poll(file, sock, wait)` (`net/socket.c:1471–1490`), and epoll uses the
  same `file->f_op->poll`. `wake_up_interruptible_poll(sk_sleep(sk), EPOLLIN)`
  reaches a `poll_wait`/`epoll_ctl` waiter on the same wait queue. Counters:
  4 poll calls in phase B, plus the epoll wait served by the same wake.
- **The application's fd never reaches those ops, because it is another
  socket.** The app's `connect()` created the client-side socket; the module
  owns the server-side one. Replacing ops on the server side changes what the
  *module* sees, not what the app sees. Phase A's counters stayed at zero while
  the app polled and read twice.

### Why this is structural, not a detail we can paper over

A socket becomes a file descriptor in one place: `sock_map_fd()`
(`net/socket.c:553`), called only from `__sys_socket_create()` for the
`socket()` syscall (`net/socket.c:1802`). `kernel_accept()` never calls it; it
hands back a `struct socket *`. The only in-tree way to give userspace a
descriptor for an accepted socket is `sock_alloc_file()` — what the probe's
`GETFD` does — and that is not something a transparent TCP endpoint can do
without the application cooperating. A module cannot call `sock_map_fd()`
either: it is static and its only caller is the `socket()` syscall.

An application that used `splice()` would be worse still: it goes through
`ops->splice_read`, which the probe did not replace (so it stayed
`inet_splice_read`), and phase A shows nothing would reach the module anyway.

### Consequence for tbtun

The accepted-socket-plus-`recvmsg` design cannot sit transparently under an
unmodified application. The alternatives that do not depend on co-operation are
the ones `docs/prior-art.md` §8 already weighed (kernel receive-queue injection
via `tcp_send_rcvq` under `TCP_REPAIR`), or an application-side descriptor the
module hands out. This probe rules out the first of those for the *read* path,
not the *delivery* path: injection still puts bytes where a normal `read()`
finds them, and is unaffected by the finding above. The second — handing the
application a descriptor the module owns — is exactly what the probe's `GETFD`
demonstrates, and it is what the phase B numbers describe.

## 3. The two source questions

Source: `linux-7.2.9` tarball from `cdn.kernel.org`, unpacked at
`~/.cache/tbtun-build/src/linux-7.2.9` — the same version the running kernel
reports (`uname -r` = `7.2.9-1-MANJARO`). Manjaro's packaging adds its own
patches, so the line numbers are upstream 7.2.9's; the two facts below were
also confirmed against the 7.2.9 header payload's `Module.symvers`: `tcp_sendmsg`
is exported, `tcp_send_rcvq` appears zero times, i.e. it is not. Check the line,
not just the number, if the patch set ever needs auditing.

### (a) Does `tcp_send_rcvq()` check `tp->repair` itself?

**No. The check is only at the call sites.** `tcp_send_rcvq()` is a plain
enqueue: allocate, `skb_copy_datagram_from_iter`, stamp `seq = rcv_nxt`, then
`tcp_queue_rcv()` — no reference to `tp->repair` anywhere in its body
(`net/ipv4/tcp_input.c:5549–5600`).

The guard lives in `tcp_sendmsg_locked()`:

```c
/* net/ipv4/tcp.c:1202 */
if (unlikely(tp->repair)) {
        if (tp->repair_queue == TCP_RECV_QUEUE) {
                copied = tcp_send_rcvq(sk, msg, size);   /* tcp.c:1204 */
                goto out_nopush;
        }
        err = -EINVAL;
        if (tp->repair_queue == TCP_NO_QUEUE)
                goto out_err;
        ...
```

`tcp_sendmsg()` is the only caller of the locked variant
(`net/ipv4/tcp.c:1446–1456`, `EXPORT_SYMBOL`), and it takes the socket lock
around it. Consequences for tbtun:

- Calling `tbt_tcp_send_rcvq()` directly bypasses the check. The module must
  therefore guarantee `tp->repair == 1 && tp->repair_queue == TCP_RECV_QUEUE`
  itself — as `tbt_sock_arm_inject()` does — and `tcp_send_rcvq` will not
  complain if it does not.
- Injection must be called with the socket lock held, or through
  `tcp_sendmsg`, which takes it. The probe-style path in `tbt_sock.c` takes
  `lock_sock()` first, which is correct.
- `tcp_queue_rcv()` (`net/ipv4/tcp_input.c:5532–5547`) has no repair check
  either, and calls `tcp_rcv_nxt_update()`, so an injected run advances
  `rcv_nxt` and must be contiguous with it.

### (b) Does a socket with `tp->repair = 1` still process incoming data?

**Yes — nothing drops received segments for repair.** There is no `repair` test
in `tcp_v4_rcv()`/`tcp_v4_do_rcv()` (`net/ipv4/tcp_ipv4.c`), in
`tcp_rcv_established()`, in `tcp_data_queue()`, or in `tcp_queue_rcv()`
(`net/ipv4/tcp_input.c`); the only repair references in that file are a comment
at line 194 and the definition at 5549. A packet that passes the sequence checks
is queued into `sk_receive_queue` exactly as on a normal socket.

What repair does change is what the *application* can do with it:

- `tcp_recvmsg_locked()` refuses to read: `if (unlikely(tp->repair)) { err =
  -EPERM; if (!(flags & MSG_PEEK)) goto out; ... }`
  (`net/ipv4/tcp.c:2684–2698`). Data that arrived is unreadable, non-peek.
- `tcp_close()` does not send FIN or RST: `if (unlikely(tcp_sk(sk)->repair))
  sk->sk_prot->disconnect(sk, 0);` (`net/ipv4/tcp.c:3178–3179`), and
  `tcp_disconnect()` sets `ECONNABORTED` instead of sending a reset
  (`net/ipv4/tcp.c:3375–3376`).
- `tcp_sendmsg_locked()` returns `-EINVAL` for ordinary sends
  (`net/ipv4/tcp.c:1207–1210`).

For tbtun that is the useful half: the socket is not detached from the
connection — ACKs, window updates and the peer's data still flow — but the
socket is `CAP_NET_ADMIN`-gated (`tcp_can_repair_sock()`,
`net/ipv4/tcp.c:3507–3511`, requires `CAP_NET_ADMIN` in the socket's user-ns and
a non-listening socket), and whatever arrives competes with injected bytes for
`rcv_nxt`. The module owns the socket and never lets userspace `recvmsg` it, so
`-EPERM` costs nothing; the real constraint is that a genuine incoming segment
advances `rcv_nxt` behind the module's back, which is why injection has to
re-read `rcv_nxt` rather than assume it owns the sequence space.

## 4. Exact ops and fields that had to be replaced

`sock->ops` only. Two methods:

```c
probe_ops = inet_stream_ops;          /* the whole struct, by value  */
probe_ops.recvmsg = probe_recvmsg;    /* int (*)(struct socket *, struct msghdr *, size_t, int) */
probe_ops.poll    = probe_poll;       /* __poll_t (*)(struct file *, struct socket *, poll_table *) */
WRITE_ONCE(newsock->ops, &probe_ops);
```

Not replaced, and not needed for this to work: `sk->sk_data_ready` (only for
the listener, to schedule accept), `sk_prot`, `sk_receive_queue`,
`sk_wq`. The wait queue used by both the module's wake and the userspace
`poll`/`epoll` is the stock `sk_sleep(sk)` — `poll_wait(file, sk_sleep(sock->sk),
wait)` — which is why a single `wake_up_interruptible_poll()` serves a blocked
`read()` and an `epoll_wait()` at once.

Two things this probe got wrong before it worked, worth recording because they
are easy to repeat:

- **Lifetime.** After `GETFD`, the socket belongs to the file. Closing that fd
  frees the socket, and a later `kernel_sock_shutdown(pc.sock, ...)` dereferenced
  freed memory (`oops at kernel_sock_shutdown+0xd`, `sock->ops` offset 0x68).
  The probe now resolves the socket through the live file and does nothing when
  there is none.
- **Payload offsets.** Filling a read buffer from the start of the pattern
  makes a reader that consumes the payload in pieces see different bytes than
  one that reads it whole. `probe_fill()` now continues from the stream offset.

## 5. Console transcript (last run)

```
== tbt_probe loaded
== running probe_user
-- phase A: the application's own connected socket
  ok   the application holds one end of the connection: the app's local port is 19200
  ok   poll(app fd, 1000) returned 0 before the release
  NOTE rc=0 revents=0x0 after the release: the app fd is a different socket, so the module's queue is invisible to it
  NOTE n=-1 errno=Resource temporarily unavailable (no data on the app's own socket)
STATS-A recvmsg_calls=0 poll_calls=0 wakes=0
  ok   phase A never entered the module's ops: proves the two sockets are distinct
-- phase B: the accepted socket the module hands out
  ok   GETFD installed the accepted socket: fd 5, payload rearmed for +1500 ms
  ok   first poll() returned 0: the module had not released the payload
  ok   second poll() woke with POLLIN from the module's delayed wake
  ok   read() returned 4096 bytes from the module queue: PROBE-OK
  ok   MSG_PEEK returns the next bytes without consuming: 61440 bytes, stream position unchanged
  ok   read() to EOF returned exactly the payload: 65536 bytes, PROBE-OK repeated
  ok   read() after EOF returns 0
STATS-B recvmsg_calls=5 poll_calls=4 wakes=1 bytes_served=65536 ready=1 eof=1 handed_out=1
  ok   read() reached the module's .recvmsg
  ok   poll() reached the module's .poll
-- phase B2: epoll
  ok   epoll_wait() saw EPOLLIN from the module's wake
PROBE_PASS
== probe_user exited 0
TBT_TEST_PASS
```

Reproduce:

```sh
cd tests/probe && make                     # tbt_probe.ko + probe_user
cd ../.. && tests/qemu/run.sh \
    --ko tests/probe/tbt_probe.ko \
    --init tests/probe/init \
    --bin tests/probe/probe_user --timeout 90
```

## 6. Caveats

- The probe runs on 127.0.0.1 only; the guest has no NIC. Ops replacement is a
  socket-local change, so the result does not depend on the route.
- `probe_user` is dynamically linked against the host glibc, like
  `host/tbt_burst`; `mkinitramfs.sh` copies the loader and libraries.
- Phase A's "no POLLIN" is the *expected* outcome under the different-socket
  model, so it is reported as `NOTE`, and the assertion that fails the run is
  the counter check (`phase A must not enter the module's ops`). A future kernel
  that made the client and server halves share a `socket` would turn phase A
  green and that assertion red, which is exactly the signal wanted.
