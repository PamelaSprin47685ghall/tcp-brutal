# Prior art for a kernel TCP proxy over N+M multiplexed HTTP/1.1 keepalive connections

Scope: what exists that we can copy, what exists that we must not copy, and which
kernel primitives are actually reachable from an out-of-tree module.

## 0. Provenance — read this first

Four repositories named in the brief do not exist. Verified with plain HTTP
requests, not memory:

| Requested | HTTP status | Substituted with |
|---|---|---|
| `github.com/oyangliming/nopacket` | 404 | `net/netfilter/nf_conntrack_seqadj.c` (the real in-flight TCP seq rewriter) |
| `github.com/vipul-sharma20/snoop` | 404 | MPTCP DSN mapping + reinjection (`net/mptcp/protocol.c`, `protocol.h`) |
| `github.com/wutianze/KeLS` | 404 | `tcp_splice_read()` + sockmap redirect (`net/ipv4/tcp.c`, `net/ipv4/tcp_bpf.c`) |
| `github.com/theanarkh/kernel-esp-morph` | 404 | `__tcp_transmit_skb()` seq assignment (`net/ipv4/tcp_output.c`) |

Also 404: the user `oyangliming` (GitHub API returns `Not Found`), and Sourcegraph
literal search for `NATRECURSE_ENCODE_TCP`, `DOUBLE_TCP_SCALE`, `PROCTLN` returns
zero hits across all indexed public repos (forks and archived included). The
Internet Archive API answered 429 and then "Temporarily Offline", so there is no
archived copy either. `HyPrism` could not be located (see §6).

Consequence: **§2, §3, §4 and §5 describe mechanisms, not the named code.** Where I
reason about intent rather than a read source, the line is tagged `[INFERENCE]`.
Do not cite the four dead URLs anywhere in our design docs; they will not resolve
for a reviewer.

All kernel citations are from `torvalds/linux` tag `v6.12`, fetched as raw files
and read locally. Line numbers are from those exact files.

---

## 1. MPTCP path manager: hook points and what a module can reuse

Sources read: `net/mptcp/pm_netlink.c` (2469 lines), `net/mptcp/pm.c` (550),
`net/mptcp/subflow.c` (2194), `net/mptcp/protocol.c` (4228),
`net/mptcp/protocol.h` (1279); mptcpd `include/mptcpd/plugin.h`,
`plugins/path_managers/sspi.c`, `src/netlink_pm_upstream.c`.

- **The generic PM entry points are thin wrappers that only set state and schedule
  work.** `void mptcp_pm_add_addr_received(const struct sock *ssk, const struct
  mptcp_addr_info *addr)` (`net/mptcp/pm.c:208`) does three things: emits
  `mptcp_event_addr_announced()`, and — for the in-kernel PM — either rejects the
  address (id 0 with a different address, or id > 0 when `pm->accept_addr` is
  false) or calls `mptcp_pm_schedule_work(msk, MPTCP_PM_ADD_ADDR_RECEIVED)` after
  copying `pm->remote = *addr`. All actual subflow creation happens later in work
  context. That split is the single most reusable idea here: *signal path never
  sleeps, work queue owns socket creation.*

- **Subflow creation and ADD_ADDR signalling are one function.**
  `static void mptcp_pm_create_subflow_or_signal_addr(struct mptcp_sock *msk)`
  (`pm_netlink.c:535`) first tries to announce an address if
  `msk->pm.add_addr_signaled < add_addr_signal_max`, then falls into:

  ```c
  while (msk->pm.local_addr_used < local_addr_max &&
         msk->pm.subflows < subflows_max) {
          ...
          nr = fill_remote_addresses_vec(msk, &local.addr, fullmesh, addrs);
          if (nr == 0) continue;
          spin_unlock_bh(&msk->pm.lock);
          for (i = 0; i < nr; i++)
                  __mptcp_subflow_connect(sk, &local, &addrs[i]);
          spin_lock_bh(&msk->pm.lock);
  }
  ```

  Note the lock dance: `__mptcp_subflow_connect()` is called with `pm.lock`
  dropped, because it does a full `connect()` on a new socket. We need the same
  discipline for our N+M connection pool, for the same reason.

- **`mptcp_pm_nl_add_addr_received()` (`pm_netlink.c:723`) is the template for
  "peer told me about an address, open a path to it".** Order of operations:
  echo the ADD_ADDR back (`mptcp_pm_announce_addr(msk, &remote, true)`), send an
  ACK, dedup with `lookup_subflow_by_daddr(&msk->conn_list, &remote)`, fill local
  addresses with `fill_local_addresses_vec()`, connect once per local address,
  then ratchet `add_addr_accepted` and clear `msk->pm.accept_addr` when either
  cap is reached. The dedup-before-connect step is the part people forget.

- **SYN-sent and SYN-received hooks, exactly:**
  - passive open, on the third ACK: `mptcp_pm_new_connection(owner, child, 1)`
    at `subflow.c:895`, inside `subflow_syn_recv_sock()`, immediately after
    `mptcp_sk_clone_init()`.
  - active open, after the handshake: `mptcp_pm_new_connection(msk, ssk, 0)` at
    `protocol.c:3608`, at the end of `mptcp_finish_connect()`.
  - incoming MP_JOIN acceptance: `mptcp_pm_allow_new_subflow(msk)` at
    `protocol.c:3642` inside `mptcp_finish_join()`; it returns false when
    `pm->subflows >= subflows_max` and pre-decrements nothing, so a rejected join
    costs nothing.
  - both sides converge on `mptcp_pm_create_subflow_or_signal_addr()` through
    `mptcp_pm_fully_established()` (`pm.c:127`) and
    `mptcp_pm_subflow_established()` (`pm.c:159`).

- **The ADD_ADDR option is emitted from the options writer, not from the PM.**
  `bool mptcp_pm_add_addr_signal(struct mptcp_sock *msk, const struct sk_buff
  *skb, unsigned int opt_size, unsigned int remaining, struct mptcp_addr_info
  *addr, bool *echo, bool *drop_other_suboptions)` (`pm.c:323`) is a *pull* API:
  the TCP option builder asks "do you have an address for me, and does it fit in
  `remaining` bytes". It also drops every other suboption when the carrier is a
  pure ACK. This is the right shape for us too — never push control bytes into a
  data stream from a timer; let the writer pull.

- **Userspace PM exists and is a separate code path, not a plugin.**
  `net/mptcp/pm_userspace.c` (760 lines) registers the same genl family
  (`MPTCP_PM_CMD_GRP_NAME "mptcp_pm_cmds"`, `include/linux/mptcp_upstream.h:26`)
  and `mptcp_pm_is_userspace(msk)` gates every handler. mptcpd is only a client
  of that netlink API: `src/netlink_pm_upstream.c` (1271 lines) serialises
  `MPTCP_PM_CMD_ADD_ADDR` / `_GET_ADDR` / `_SET_LIMITS` / `_SET_FLAGS`, and the
  path-manager plugins (`plugins/path_managers/sspi.c`, 864 lines) implement
  `struct mptcpd_plugin_ops` with `new_connection`, `new_address`, `new_subflow`,
  `subflow_closed`, `listener_created` … registered as
  `mptcpd_plugin_register_ops("sspi", &pm_ops)` (`sspi.c:828-830`).

- **Gotcha that transfers directly:** every limit is a per-namespace sysctl-backed
  counter (`mptcp_pm_get_subflows_max()`, `..._add_addr_signal_max()`,
  `..._local_addr_max()`, `pm_netlink.c:209-239`), and the kernel refuses to
  exceed it silently. If we copy this design, our N and M must be explicit,
  enforced counters with a defined failure mode, or the proxy will stall with no
  error.

- **None of the interesting MPTCP functions are exported.** Grepping the fetched
  tree: `net/mptcp/pm_netlink.c` exports only the four limit getters
  (`pm_netlink.c:215,223,231,239`), and `net/mptcp/subflow.c` exports only
  `mptcp_subflow_init_cookie_req` (`:303`) and `mptcp_subflow_reqsk_alloc`
  (`:745`). `mptcp_pm_add_addr_received`, `mptcp_pm_new_connection`,
  `mptcp_pm_allow_new_subflow` and `mptcp_pm_create_subflow_or_signal_addr` have
  **no** `EXPORT_SYMBOL*` at all. So a module cannot call the PM; it can only
  (a) speak the `mptcp_pm_cmds` generic-netlink family like mptcpd does, or
  (b) kprobe those symbols, or (c) implement its own PM. This kills the "reuse
  mptcp_pm_add_addr_received from our module" idea outright.

---

## 2. In-flight TCP sequence rewriting ("re-inject with doubled seq")

The requested `nopacket.c` is not retrievable (§0). What such a module must use is
`net/netfilter/nf_conntrack_seqadj.c` (234 lines) — the only in-tree code that
rewrites TCP sequence numbers of packets already in flight, per direction.

- **The state is three integers per direction**, `include/net/netfilter/nf_conntrack_seqadj.h`:

  ```c
  struct nf_ct_seqadj {
          u32 correction_pos;   /* seq of the last modification */
          s32 offset_before;    /* offset for seq <= correction_pos */
          s32 offset_after;     /* offset for seq >  correction_pos  */
  };
  struct nf_conn_seqadj { struct nf_ct_seqadj seq[IP_CT_DIR_MAX]; };
  ```

  Two offsets, not one, is the whole trick: packets that were already in flight
  when the offset changed still carry the old mapping, and must be adjusted by the
  old value. Any design that stores a single `delta` will corrupt retransmissions.

- **Setting an offset is monotonic and seq-gated:**

  ```c
  int nf_ct_seqadj_set(struct nf_conn *ct, enum ip_conntrack_info ctinfo,
                       __be32 seq, s32 off);            /* seqadj.c:30, EXPORT_SYMBOL_GPL:58 */
  ```

  It only advances when `this_way->offset_before == this_way->offset_after ||
  before(this_way->correction_pos, ntohl(seq))`, then sets
  `correction_pos = ntohl(seq)`, `offset_before = offset_after`,
  `offset_after += off`. The comparison is in host order, under `ct->lock`
  (`spin_lock_bh`).

- **Applying it is a rewrite of seq, ack, and every SACK block:**

  ```c
  int nf_ct_seq_adjust(struct sk_buff *skb, struct nf_conn *ct,
                       enum ip_conntrack_info ctinfo, unsigned int protoff);  /* seqadj.c:164 */
  ```

  Body: `skb_ensure_writable(skb, protoff + sizeof(*tcph))`, then
  `newseq = htonl(ntohl(tcph->seq) + seqoff)` with
  `inet_proto_csum_replace4(&tcph->check, skb, tcph->seq, newseq, false)` before
  the store; the ACK number uses the *other* direction's offset and is
  **subtracted** (`newack = htonl(ntohl(tcph->ack_seq) - ackoff)`); then
  `nf_ct_sack_adjust()` walks SACK blocks and fixes each one. Skipping the SACK
  fixup produces a connection that works until the first loss, then wedges.

- **`s32 nf_ct_seq_offset(const struct nf_conn *ct, enum ip_conntrack_dir dir,
  u32 seq)` (`seqadj.c:220`) answers "what offset applies to arbitrary seq N".**
  This is the function that makes a *predictive* injector possible at all: given
  the DSN of a placeholder you are about to synthesise, it tells you which
  sequence-space the peer will see.

- **All five entry points are `EXPORT_SYMBOL_GPL`** (`seqadj.c:28,58,72,218,234`),
  so a GPL module can call them. `nf_ct_seqadj_init()` requires the
  `NF_CT_EXT_SEQADJ` extension to exist, which means the conntrack entry must
  have been created by `nf_ct_seqadj_ext_add()`; without it
  `nf_ct_seqadj_set()` hits `WARN_ONCE(1, "Missing nfct_seqadj_ext_add() setup
  call")` and silently does nothing.

- **Why "doubling" could work at all** `[INFERENCE]`: a per-flow shaper that
  counts *bytes* from `seq` deltas (rather than from IP length) sees twice the
  volume when consecutive packets carry `seq, seq+2*len, seq+4*len`. A shaper that
  counts `skb->len` is unaffected. This means the technique is
  middlebox-model-specific and dies against any shaper that reassembles or
  measures octets — consistent with the brief calling it "compliant" only at the
  HTTP layer.

- **Hard blockers for us, all visible in the code above:** the transform is only
  invertible if the peer applies the exact inverse; retransmissions reuse
  `TCP_SKB_CB(skb)->seq` (`tcp_output.c:1382` writes `th->seq = htonl(tcb->seq)`),
  so a transform applied at transmit time must also be applied to the retransmit
  copy or the peer's `rcv_nxt` desynchronises; and TSO/GSO means one `skb` may
  cover many sequence numbers, so a per-packet constant offset is not enough —
  the offset must be a function of `seq`, exactly as `nf_ct_seq_offset()` is.

---

## 3. Predictive duplication and DSN mapping

The requested `snoop.c` is not retrievable (§0). The real, load-bearing prior art
for "map stream bytes onto per-path sequence numbers, then re-send selected bytes
on a different path" is MPTCP's data-level mapping.

- **The mapping is two inline functions over `copied_seq`:**

  ```c
  static inline u64
  mptcp_subflow_get_map_offset(const struct mptcp_subflow_context *subflow)
  {
          return tcp_sk(mptcp_subflow_tcp_sock(subflow))->copied_seq -
                        subflow->ssn_offset -
                        subflow->map_subflow_seq;
  }

  static inline u64
  mptcp_subflow_get_mapped_dsn(const struct mptcp_subflow_context *subflow)
  {
          return subflow->map_seq + mptcp_subflow_get_map_offset(subflow);
  }
  ```

  (`net/mptcp/protocol.h:638-650`.) Everything is derived from the subflow's
  `copied_seq`; there is no per-byte table. A placeholder that has been written
  into a subflow's receive queue therefore automatically acquires a DSN.

- **The send-side record carries a reinjection flag per fragment:**

  ```c
  struct mptcp_data_frag {
          struct list_head list;
          u64 data_seq;   u16 data_len;  u16 offset;
          u16 overhead;   u16 already_sent;
          struct page *page;
  };
  ```

  (`protocol.h`.) `already_sent` is what makes "send this again on another
  subflow" idempotent: `__mptcp_retransmit_pending_data()` walks the rtx queue and
  clears it for exactly the fragments it re-arms.

- **Path selection for a re-send prefers a non-backup subflow that has nothing
  outstanding**, and only falls back to a backup one when no subflow has made
  progress for more than one stale period:

  ```c
  struct sock *mptcp_subflow_get_retrans(struct mptcp_sock *msk)  /* protocol.c:2334 */
  ...
  if (!tcp_rtx_and_write_queues_empty(ssk)) {
          mptcp_pm_subflow_chk_stale(msk, ssk);
          min_stale_count = min_t(int, min_stale_count, subflow->stale_count);
          continue;
  }
  ...
  return min_stale_count > 1 ? backup : NULL;
  ```

  Staleness itself is `subflow->stale_count`, incremented in
  `mptcp_pm_subflow_chk_stale()` (`pm.c:456`) only while `tp->rcv_tstamp` is
  unchanged. That is a cheap, purely local loss signal — no timers, no
  timestamps — and it is exactly the signal a predictive injector needs.

- **What the brief's "if fewer than M packets are missing, don't wait" needs that
  MPTCP does not have:** MPTCP only re-sends data it *knows* was lost (or whose
  path went stale). It never speculates. The speculative half — a reorder-distance
  estimator with a geometric threshold — has no in-tree equivalent I could read.
  `[INFERENCE]` The k≈0.919 constant in the brief looks like a geometric tail
  parameter fitted to a reorder-gap distribution; it is a tuning number, not an
  algorithm, and we cannot copy it from a source we do not have.

- **Verified alternative prior art for blind duplication:** RFC 7198, *Duplicating
  RTP Streams* (`https://www.rfc-editor.org/rfc/rfc7198.txt`), standardises
  sending the same payload on two paths with a shared sequence space and a
  receiver that discards the second copy. It solves the "duplicate arrives after
  the original" case explicitly, which is the case our placeholder scheme creates.

- **Gotcha:** a placeholder must be *distinguishable* from real data at the
  receiver, and it must occupy DSN space that the original will also occupy. If
  the receiver has already delivered the placeholder to the application, the
  arriving original must be dropped, not appended. MPTCP gets this for free
  because mapping is by `data_seq` and `mptcp_data_queue_ofo()` rejects
  duplicates; a byte-stream splice does not.

---

## 4. Kernel-level splice proxy

The requested `kels.c` is not retrievable (§0). Two real mechanisms cover the same
ground, and one of them is sanctioned.

- **`tcp_splice_read()` is the in-tree "move socket bytes to a pipe without
  copying" primitive:**

  ```c
  ssize_t tcp_splice_read(struct socket *sock, loff_t *ppos,
                          struct pipe_inode_info *pipe, size_t len,
                          unsigned int flags);        /* net/ipv4/tcp.c:789 */
  static int __tcp_splice_read(struct sock *sk, struct tcp_splice_state *tss);  /* tcp.c:766 */
  ```

  It takes `lock_sock()`, loops `__tcp_splice_read()`, and on empty data calls
  `sk_wait_data(sk, &timeo, NULL)`; it returns `-ESPIPE` for a non-zero `*ppos`.
  `tcp_splice_eof()` (`tcp.c:1364`, `EXPORT_SYMBOL_GPL:1378`) exists solely to push a
  pending partial frame, which is the "flush the tail" problem we will also have.

- **The ACK-learn problem is structural, not a bug in any one proxy.**
  `tcp_sendmsg_locked()` returns the number of bytes *copied into the write
  queue*, never the number acknowledged; on pressure it calls
  `sk_stream_wait_memory(sk, &timeo)` and retries (`tcp.c:1313-1318`). The only
  truth about delivery is `tp->snd_una`, advanced in the ACK path. A splice proxy
  that frees its internal buffer on `sendmsg()` return will drop data that is
  still unacked, and cannot retransmit it on another path.

- **sockmap is the supported way to do a kernel splice proxy.** `tcp_bpf_sendmsg()`
  (`net/ipv4/tcp_bpf.c:496`) replaces `sk->sk_prot->sendmsg` for a socket in a
  sockmap, copies user data into an `sk_msg` (`sk_msg_alloc()`), runs the
  `BPF_PROG_TYPE_SK_MSG` verdict, and on `__SK_PASS` pushes it with
  `tcp_bpf_push()` → `tcp_sendmsg_locked()` using `MSG_SPLICE_PAGES`
  (`tcp_bpf.c:116`). On `__SK_REDIRECT` the *same* `sk_msg` goes to another socket
  via `tcp_bpf_sendmsg_redir(sk, ingress, msg, bytes, flags)`
  (`tcp_bpf.c:161`, `EXPORT_SYMBOL_GPL:178`).

- **`tcp_bpf_push()` is the reference implementation of "write into another
  socket's send path correctly":** it sets `MSG_MORE` whenever more elements
  remain (`if (size < sge->length && msg->sg.start != msg->sg.end)
  msghdr.msg_flags |= MSG_MORE;`), retries the short-write remainder with
  `goto retry`, uncharges memory only when it owns the charge
  (`if (uncharge) sk_mem_uncharge(sk, ret);`), and stops on `ret <= 0` returning
  the error upward. Copy that loop shape; do not invent one.

- **Memory accounting is the actual design constraint.** `sk_msg` is charged to
  the *source* socket (`sk_msg_alloc(sk, ...)`), and `tcp_bpf_sendmsg()` blocks in
  `sk_stream_memory_free()` / `wait_for_sndbuf` when the destination is full. A
  proxy that allocates its own buffers must decide which socket's `sk_wmem_alloc`
  they belong to, or it will either deadlock (both sides waiting for the other to
  drain) or blow past `wmem_max` invisibly.

- **Why kretprobe-on-`tcp_sendmsg` is the wrong tool** `[INFERENCE]`, but the
  mechanism is visible: `tcp_sendmsg()` (`tcp.c:1352`) is a 6-line wrapper around
  `lock_sock()` + `tcp_sendmsg_locked()` + `release_sock()`. A kretprobe fires
  after `release_sock()`, so any work you do there races every other writer on
  that socket, and you get only the byte count — not the `skb`s, not their
  sequence numbers. To see the `skb`s you must hook `tcp_sendmsg_locked()` or
  below, at which point you are reimplementing sockmap with worse locking.

---

## 5. Send-path transformation

The requested `kernel-esp-morph` is not retrievable (§0). The transform point is
nonetheless fixed by the kernel's own structure.

- **Sequence numbers are written once, at transmit time, from the control block:**

  ```c
  th->seq      = htonl(tcb->seq);      /* net/ipv4/tcp_output.c:1382 */
  th->ack_seq  = htonl(rcv_nxt);
  ```

  inside `__tcp_transmit_skb()` (`tcp_output.c:1285`), where `tcb` is
  `TCP_SKB_CB(skb)`. `tcp_transmit_skb()` (`tcp_output.c:1481`) is a two-line
  wrapper that passes `tcp_sk(sk)->rcv_nxt`.

- **`TCP_SKB_CB(skb)->seq` is assigned when the `skb` is queued, not when it is
  sent.** `tcp_queue_skb()` (`tcp_output.c:1493`) does
  `WRITE_ONCE(tp->write_seq, TCP_SKB_CB(skb)->end_seq);` then
  `tcp_add_write_queue_tail()`. So there are exactly two transform windows:
  (a) mutate `TCP_SKB_CB()->seq/end_seq` of the queued `skb`s, which then flows
  into retransmits, SACK scoring, and `snd_una` comparisons; or (b) rewrite the
  header in `__tcp_transmit_skb()` after line 1382, which must then be undone on
  every path that reads the header back (loopback, `tcp_v4_rcv` on the same host,
  `TCP_REPAIR`).

- **Window (a) breaks retransmission accounting unless you also adjust
  `tp->write_seq`, `tp->snd_nxt`, `tp->snd_una`-relative offsets, and every
  `TCP_SKB_CB` in the write queue** — they are compared with `before()`/`after()`
  against each other (`tcp_output.c:384,1448,1545`). A partial transform makes
  `tcp_rtx_queue` think segments are outside the window and either stalls or
  spins.

- **The sanctioned transform hook is `BPF_PROG_TYPE_SK_MSG`.** It runs before the
  data reaches `tcp_sendmsg_locked()`, receives `struct sk_msg_md`, and can rewrite
  payload only after `bpf_msg_pull_data()`. It has **no access to sequence
  numbers** — that is deliberate: the verifier cannot let a program desynchronise
  TCP state. So a seq-based transform cannot be expressed in sockmap at all.

- **If a payload transform is all we need, `MSG_SPLICE_PAGES` + page frags is
  enough** and avoids the seq problem entirely: `tcp_bpf_push()` builds a
  `bvec` per `sg` element and hands it to `tcp_sendmsg_locked()`
  (`tcp_bpf.c:108-128`). Zero-copy is preserved only while the transform is
  length-preserving; any length change forces a copy.

- **Probe mechanism, if we insist on a kprobe:** kprobes on
  `tcp_sendmsg_locked` fire with `regs` only; the `msghdr` is in a register at
  entry, so you can read `msg_iter`, but you cannot safely write it. `fexit`/
  `kretprobe` gives you the return value only. Neither gives you the `skb`s. The
  only place with both `skb` and `tp` in hand is inside `tcp_write_xmit()` /
  `__tcp_transmit_skb()`, which are `static` — reachable only by kprobe on a
  symbol address, which is exactly the fragile thing we should avoid.

---

## 6. "MD5-derived stream id" — absent from OpenVPN; three real keys instead

Sources: OpenVPN master `f1721dc7` (2026-11-03), `src/openvpn/{mudp.c,multi.c,
mroute.c,ssl.c,options.c,push.c,init.c,session_id.h,ssl_pkt.h}`; plus release
tarballs 2.0.9 / 2.1.4 / 2.2.2 and raw `mudp.c` from `release/2.3`, `release/2.4`.

- **Negative result.** `grep -rn fill_md5` over master, `release/2.3`,
  `release/2.4`, and the 2.0.9/2.1.4/2.2.2 tarballs returns **nothing**, and
  Sourcegraph literal search for `fill_md5` hits only `fio`, `Provenance`, `vutuv`.
  There is no `struct openvpn_peer_info` and no `PEER_ID_*` macro; the only
  constant is `#define MAX_PEER_ID 0xFFFFFF` (`openvpn.h:552`).
  `struct packet_id_net { uint64_t id; time_t time; }` (`packet_id.h:190-196`) is
  a replay counter, not an id. Item 6 as written cannot be cited.

- **Key 1 — remote-address hash in `m->hash`, SipHash, remote only.**
  `multi_get_create_instance_udp()` builds `struct mroute_addr real` from
  `m->top.c2.from.dest` via `mroute_extract_openvpn_sockaddr()` (`mudp.c:419-422`,
  `mroute.c:264-268`: `addr->len = 6`, copies `sin_addr`/`sin_port` in network
  order), then `hash_value(m->hash, real)` (`mudp.c:315-322`). The hashed bytes are
  `mroute_addr_hash_ptr()` = `&a->proto` for `(uint32_t)a->len + 3` bytes
  (`mroute.h:234,240`) — 3 header bytes plus 4-byte IPv4 plus 2-byte port, through
  `siphash_hash_func()` (`mroute.c:359-362`). The *local* address never enters.

- **Key 2 — the 3-byte peer-id sits in the P_DATA_V2 header and indexes an array;
  no hashing at all.**

  ```c
  ssl.c:4198  tls_prepend_opcode_v2(const struct tls_multi *multi, struct buffer *buf)
  ssl.c:4206      peer = htonl(((P_DATA_V2 << P_OPCODE_SHIFT) | ks->key_id) << 24
  ssl.c:4207                   | (multi->tx_peer_id & 0xFFFFFF));
  ssl.c:4208      ASSERT(buf_write_prepend(buf, &peer, 4));
  ```

  On the wire: byte 0 = opcode `P_DATA_V2` (=9, `ssl_pkt.h:48`) in the top 5 bits
  plus `key_id`; bytes 1–3 = peer-id, big-endian (one `htonl` of a 4-byte word, so
  the 24-bit value lands in the last three bytes). It is prepended **before**
  encryption so AEAD authenticates it (`forward.c:697-699`), the AD starts at the
  opcode (`ssl.c:3694-3697`), and RX skips 3 bytes (`ssl.c:3703-3713`).

  ```c
  mudp.c:372  uint32_t peer_id = ((uint32_t)ptr[1] << 16) | ((uint32_t)ptr[2] << 8) | ((uint32_t)ptr[3]);
  mudp.c:373  peer_id_disabled = (peer_id == MAX_PEER_ID);
  mudp.c:375  if (!peer_id_disabled && (peer_id < m->max_clients) && (m->instances[peer_id]))
  ```

  `struct multi_context.instances` is documented as "An instance can be accessed
  using peer-id as an index" (`multi.h:171-174`). The id is a 24-bit slot the
  **server** picks (`multi.c:4261-4266`), pushed in PUSH_REPLY as
  `"peer-id %d"` (`push.c:720-723`), parsed at `options.c:7872-7876`, installed as
  `tx_peer_id`/`rx_peer_id` (`init.c:2678-2684`). Nothing is derived from packet
  contents.

- **Key 3 — 8-byte session id in `m->sid_hash`, control packets only.**
  `struct session_id { uint8_t id[8]; }` (`session_id.h:37-40`);
  `multi_get_instance_udp_control()` advances one opcode byte,
  `session_id_read(&sid, &tmp)` (`mudp.c:335-341`), then
  `hash_value(m->sid_hash, sid)` (`sid_hash.h:52-54`). Registered per session with
  `multi_hash_sid_add(m, &state.peer_session_id, mi)` (`mudp.c:291`).

- **Dispatch order is opcode → peer-id → address hash; never a digest.**
  `mudp.c:428-441` sends `P_DATA_V1/V2` to `multi_get_instance_udp_data()`, other
  opcodes to `..._control()`. Inside, `if (!v2 || peer_id_disabled) return
  multi_get_instance_udp_real(m, real);` (`mudp.c:399-401`). For a valid V2
  peer-id the 5-tuple is **not** consulted; a mismatch only marks the flow as
  floated (`mudp.c:383-395`).

- **HyPrism / `attach_md5`: unavailable.** GitHub repo search `HyPrism` returns a
  Hytale launcher and Hyprland projects; Sourcegraph literal `attach_md5` returns
  zero results. Nothing is reported about it here.

- **Consequence for us.** OpenVPN is a *counter-example*: the id that keys a flow
  is carried explicitly (3 bytes in-band, or 8 random bytes for control), and the
  only hash is a remote-only SipHash over `proto/type/netbits + IP + port`, computed
  per lookup and never transmitted. If we want a handshake-free stream id, we are
  designing it, not copying it.

---

## 7. Mapping a 4-tuple to a live `struct sock`; the `NF_INET_LOCAL_IN` hook

Sources (all v6.12, fetched): `net/core/sock_diag.c`, `include/linux/sock_diag.h`,
`include/linux/cookie.h`, `include/uapi/linux/{sock_diag.h,inet_diag.h}`,
`include/net/sock.h`, `include/net/inet_hashtables.h`,
`net/ipv4/{inet_hashtables.c,inet_diag.c,tcp_diag.c,tcp_ipv4.c,ip_input.c,af_inet.c}`,
`net/netfilter/core.c`, `include/trace/events/sock.h`, `kernel/tracepoint.c`.

- **The cookie API is a trap door: the worker is not exported.** The real function
  is `u64 __sock_gen_cookie(struct sock *sk)` (`net/core/sock_diag.c:27`);
  `sock_gen_cookie()` is only a `static inline` wrapper adding
  `preempt_disable()` (`include/linux/sock_diag.h:36-44`). `grep EXPORT_SYMBOL
  net/core/sock_diag.c` yields only `check_cookie:55`, `save_cookie:64`,
  `put_meminfo:74`, `put_filterinfo:112`, `register_inet_compat:192`,
  `unregister_inet_compat:201`, `sock_diag_register:214`,
  `sock_diag_unregister:226`, `sock_diag_destroy:325`. A module that compiles the
  inline gets an unresolved symbol at load. Use the exported pair instead:
  `int sock_diag_check_cookie(struct sock *sk, const __u32 *cookie)`
  (`.c:42`, `EXPORT_SYMBOL_GPL:55`) and
  `void sock_diag_save_cookie(struct sock *sk, __u32 *cookie)`
  (`.c:57`, `EXPORT_SYMBOL_GPL:64`) — same 64-bit value, reached through the
  inline internally.

- **`sock_gen_cookie_lookup()` does not exist.** The brief's hint is wrong:
  elixir's identifier lookup for it returns 404 while `sock_gen_cookie`,
  `__sock_gen_cookie`, `sock_diag_check_cookie`, `inet_diag_find_one_icsk` and
  `inet_lookup_established` all return 200, and grep over the four relevant files
  finds no other cookie symbol. There is **no cookie → `struct sock` map** in the
  kernel, no `idr`/`xa_`. Cookies come from per-CPU batched counters
  (`struct gen_cookie { … atomic64_t forward_last, reverse_last; }`,
  `gen_cookie_next()` in `include/linux/cookie.h:14-49`, `DEFINE_COOKIE(sock_cookie)`
  at `sock_diag.c:25`) and are strictly one-way. A reverse index is ours to build.

- **Tuple → socket is `__inet_lookup_established()`, and it is GPL-exported.**
  ```c
  struct sock *__inet_lookup_established(const struct net *net,
          struct inet_hashinfo *hashinfo, const __be32 saddr, const __be16 sport,
          const __be32 daddr, const u16 hnum, const int dif, const int sdif);
          /* net/ipv4/inet_hashtables.c:491, EXPORT_SYMBOL_GPL:535 */
  ```
  The inline `inet_lookup_established(net, hashinfo, saddr, sport, daddr, dport, dif)`
  (`include/net/inet_hashtables.h:398-406`) does the `ntohs()` for you. It walks
  `ehash` under RCU with `sk_nulls_for_each_rcu`, does
  `refcount_inc_not_zero(&sk->sk_refcnt)`, then **re-runs `inet_match()` after the
  bump** and retries on failure (`inet_hashtables.c:508-530`). That re-check is
  the whole safety argument.

- **`inet_diag_find_one_icsk()` is the "one exact socket" path `ss` uses, also
  GPL-exported.**
  ```c
  struct sock *inet_diag_find_one_icsk(struct net *net,
          struct inet_hashinfo *hashinfo, const struct inet_diag_req_v2 *req);
          /* net/ipv4/inet_diag.c:519-521, EXPORT_SYMBOL_GPL:561 */
  ```
  It takes RCU, calls `inet_lookup()` with `req->id.idiag_{sport,dport,src,dst,if}`
  (`:527-529`), handles v4-mapped AF_INET6 (`:531-536`), drops RCU, then applies
  `sock_diag_check_cookie()` (`:554`), returning `ERR_PTR(-ENOENT)` on mismatch.
  On success the socket is **already refcounted** and must be released with
  `sock_gen_put()`, never `sock_put()`. The request struct is plain uapi
  (`struct inet_diag_req_v2`, `include/uapi/linux/inet_diag.h:38-45`;
  `struct inet_diag_sockid` with `idiag_cookie[2]`, `:14-22`), so a module can fill
  it directly.

- **Refcounting: the release function is not interchangeable.** `sk_refcnt` is
  `refcount_t skc_refcnt` (`include/net/sock.h:223`, alias `:350`);
  `sock_hold()` is `refcount_inc` (`:779`), `sock_put()` is
  `refcount_dec_and_test` → `sk_free` (`:1885-1889`). But the lookup can return a
  `TCP_TIME_WAIT` or `TCP_NEW_SYN_RECV` pseudo-socket, so only
  `void sock_gen_put(struct sock *sk)` (`inet_hashtables.c:471-484`,
  `EXPORT_SYMBOL_GPL:483`) dispatches correctly to
  `inet_twsk_free()`/`reqsk_free()`/`sk_free()`. Gate every `tcp_sk()` cast on
  `sk_fullsock(sk)` = `(1 << sk->sk_state) & ~(TCPF_TIME_WAIT | TCPF_NEW_SYN_RECV)`
  (`sock.h:2752-2755`).

- **At `NF_INET_LOCAL_IN` the tuple is final and the conntrack is attached.**
  `NF_INET_LOCAL_IN == 1` (`include/uapi/linux/netfilter.h:42-50`). Register with
  `int nf_register_net_hook(struct net *net, const struct nf_hook_ops *reg)`
  (`net/netfilter/core.c:557`, **plain `EXPORT_SYMBOL:585`** — no GPL taint),
  `.pf = NFPROTO_IPV4`, `.priority = NF_IP_PRI_FILTER` (0). Priorities ascend:
  `CONNTRACK_DEFRAG=-400`, `CONNTRACK=-200`, `NAT_DST=-100`, `FILTER=0`,
  `CONNTRACK_CONFIRM=INT_MAX` (`include/uapi/linux/netfilter_ipv4.h:23-38`). So
  fragments are reassembled, DNAT has already rewritten the header, and
  `nf_ct_get(skb, &ctinfo)` returns a live `ct` whose
  `nf_ct_tuple(ct, IP_CT_DIR_ORIGINAL)` still holds the pre-NAT tuple.
  `nf_ct_get()` **does not take a reference**; retain past the hook only with
  `nf_conntrack_get()`.

- **`skb->sk` there is opportunistic, not a contract.** `NF_HOOK` passes `NULL` as
  the `sk` argument, so `state->sk` is always NULL; only `skb->sk` may be set, and
  only by IP early demux, gated on `sysctl_ip_early_demux && sysctl_tcp_early_demux`
  (both default 1, `af_inet.c:1821-1823`), `!skb->sk`, `!skb_dst(skb)`,
  `!ip_is_fragment()` and `PACKET_HOST` (`ip_input.c:334-350`,
  `tcp_ipv4.c:1965-1977`). It resolves only via `__inet_lookup_established()` —
  **never a listener**, so a bare SYN to a `LISTEN` socket arrives with
  `skb->sk == NULL`. It installs `skb->destructor = sock_edemux`
  (`tcp_ipv4.c:1990-1991`), i.e. the skb owns a real reference. Read it,
  `sock_hold()` if you keep it, and do **not** call `skb_steal_sock()`
  (`include/net/request_sock.h:98`) from the hook — it nulls `skb->sk` and robs
  `tcp_v4_rcv()` of its demux result.

- **Discovery, ranked.** (a) Registering our own `sock_diag_handler` is impossible
  for TCP: `sock_diag_register()` is a `cmpxchg(..., NULL, hndl)` returning
  `-EBUSY` (`sock_diag.c:203-214`), and `inet_diag_init()` already claims
  `AF_INET`/`AF_INET6` (`inet_diag.c:1556-1561`); `inet_diag_register()` likewise
  returns `-EEXIST` on `IPPROTO_TCP` (`:1516-1526`). (b) kprobe on `tcp_v4_rcv`
  works (`register_kprobe` is `EXPORT_SYMBOL_GPL`) but is per-segment cost on the
  hottest path, and `tcp_v4_rcv` is an `INDIRECT_CALLABLE_DECLARE` target
  (`ip_input.c:187-190`). (c) The **`inet_sock_set_state` tracepoint** fires on
  exactly the transitions we care about: `tcp_set_state()` (`tcp.c:2868`) →
  `inet_sk_state_store()` → `trace_inet_sock_set_state()` (`af_inet.c:1365-1368`),
  with `TP_PROTO(const struct sock *sk, int oldstate, int newstate)` and an entry
  carrying `skaddr`, family, protocol, sport/dport, saddr/daddr
  (`include/trace/events/sock.h:141-190`). It is *not* in the module-visible
  export list (`net/core/net-traces.c:40-68`), so
  `register_trace_inet_sock_set_state()` will not link — but
  `tracepoint_probe_register()` (`kernel/tracepoint.c:533`, `EXPORT_SYMBOL_GPL`)
  plus `for_each_kernel_tracepoint()` (`:793`) attach by name, and BPF attaches
  with no module at all. (d) A `SOCK_DIAG_BY_FAMILY` netlink dump (what `ss -tnp`
  does) needs no kernel code: `struct sock_diag_req { __u8 sdiag_family; __u8
  sdiag_protocol; }` (`uapi/linux/sock_diag.h:8-13`), dispatched via
  `sock_diag_rcv_msg()` → `__sock_diag_cmd()` → `hndl->dump` (`sock_diag.c:228-300`);
  `inet_diag_dump_icsk()` honours `idiag_states`, widening `TCPF_SYN_RECV` to
  include `TCPF_NEW_SYN_RECV` (`inet_diag.c:1026-1033`), and applies the `ss`-style
  bytecode filter (`inet_diag_bc_run()`, `:663`), exposed to modules as
  `inet_diag_bc_sk()` (`EXPORT_SYMBOL_GPL`, `:826`).

- **Recommendation.** Data path: do the lookup in the `LOCAL_IN` hook at priority 0
  with `inet_lookup()`/`inet_lookup_established()` against
  `sock_net(sk)->ipv4.tcp_death_row.hashinfo`, hold RCU, `sock_hold()` any
  `skb->sk` retained, always release with `sock_gen_put()`, gate field access on
  `sk_fullsock()`. Identity: never trust the 4-tuple alone — store the 64-bit
  cookie from `sock_diag_save_cookie()` on first sight and re-validate with
  `sock_diag_check_cookie()` on every mapping, which is exactly the guard
  `inet_diag_find_one_icsk()` applies at `inet_diag.c:554` and the reason tuple
  reuse cannot silently redirect our state.

---

## 8. Receive-path injection; KCM as the multiplexor prior art

Sources: `net/kcm/kcmsock.c`, `net/kcm/kcmproc.c`, `net/strparser/strparser.c`,
`net/ipv4/tcp.c`, `net/ipv4/tcp_input.c`, `include/net/kcm.h`.

- **KCM attaches by taking over three socket callbacks, under the socket lock.**
  `static int kcm_attach(struct socket *sock, struct socket *csock, struct bpf_prog *prog)`
  (`kcmsock.c:1217`) does `lock_sock(csk)` (`:1237`), rejects anything but
  `AF_INET/AF_INET6 + IPPROTO_TCP`, rejects `TCP_LISTEN`/`TCP_CLOSE`, and refuses
  if `csk->sk_user_data` is already set (`-EALREADY`, `:1267`). It installs
  `struct strp_callbacks cb = { .rcv_msg = kcm_rcv_strparser, .parse_msg =
  kcm_parse_func_strparser, .read_sock_done = kcm_read_sock_done }`
  (`:1226-1230`), calls `strp_init(&psock->strp, csk, &cb)`, then swaps
  `sk_data_ready`, `sk_write_space`, `sk_state_change` while saving the originals
  (`:1281-1285`). `kcm_unattach()` (`:1350`) restores all three and `strp_stop()`s.
  This is a removable decorator on a live TCP socket — the pattern we want.

- **RX needs no `tp->rcv_nxt` surgery at all.** `psock_data_ready()` (`:349`) only
  calls `strp_data_ready()`, which calls `sock->ops->read_sock()` — for TCP,
  `tcp_read_sock()` (`net/ipv4/tcp.c:1567`). That function starts at
  `u32 seq = tp->copied_seq`, walks `tcp_recv_skb()`, `tcp_eat_recv_skb()`s what
  the actor consumed, then `WRITE_ONCE(tp->copied_seq, seq)` and
  `tcp_cleanup_rbuf()`. The socket sees exactly what an application read would
  produce. `kcmsock.c` contains **zero** references to `rcv_nxt` or `copied_seq`;
  its `sk_receive_queue` uses (`:193, :338, :1091, :1647`) are all the *KCM*
  socket's own queue. Data leaves TCP's sequence space and enters KCM's
  record space.

- **Framing and back-pressure.** `__strp_recv()` (`strparser.c:90`) accumulates one
  length-delimited message, hands it to `rcv_msg`, and the skb (now carrying
  `struct strp_msg`) is queued with `kcm_queue_rcv_skb()` (`:191`):
  `sk_rmem_schedule()`, `skb->destructor = kcm_rfree`,
  `atomic_add(skb->truesize, &sk->sk_rmem_alloc)`, `sk_mem_charge()`,
  `skb_queue_tail()`, `sk->sk_data_ready()`. If no KCM socket is waiting, the
  message is parked and the parser is *paused* (`strp_pause`, `:271-276`).
  Back-pressure is "stop reading", never "refuse to advance the window" — that is
  the whole reason this design is safe.

- **TX: `static int kcm_write_msgs(struct kcm_sock *kcm)` (`:582`)** builds
  `struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES }`
  (`:606-608`), sends skb frags as bvecs via
  `sock_sendmsg(psock->sk->sk_socket, &msg)` (`:646`), and on `-EAGAIN` saves
  `txm->frag_skb`/`frag_offset` (`:649-657`). A hard error aborts the psock
  "since it has lost framing synchronization" (`:658-670`). Batching is *not*
  `MSG_MORE`: `if (msg->msg_flags & MSG_BATCH) kcm->tx_wait_more = true;`
  (`:904-905`), flushed by `kcm_push()` (`:741`) or when the queue was empty.

- **`struct kcm_psock` (`include/net/kcm.h:90-121`)** is the bookkeeping we should
  imitate: one `strparser`, saved callbacks, `rx_kcm`/`tx_kcm` reservation
  pointers, `ready_rx_msg` (the parked message), `saved_rx_bytes/saved_tx_bytes`
  deltas, and `tx_stopped/done/unattaching` flags.

- **The unsafe path is `tcp_queue_rcv()` + `__skb_queue_tail()`, and it is
  unreachable.** `static int __must_check tcp_queue_rcv(struct sock *sk, struct
  sk_buff *skb, bool *fragstolen)` (`tcp_input.c:5148`) coalesces with the tail,
  calls `tcp_rcv_nxt_update(tcp_sk(sk), TCP_SKB_CB(skb)->end_seq)` (`:5157`),
  then `__skb_queue_tail(&sk->sk_receive_queue, skb); skb_set_owner_r(skb, sk);`
  (`:5159-5160`). `tcp_rcv_nxt_update()` (`:3675`) asserts `sock_owned_by_me()`.
  The only in-tree injector is `int tcp_send_rcvq(struct sock *sk, struct msghdr
  *msg, size_t size)` (`tcp_input.c:5165`), reachable **only** under TCP_REPAIR
  (`tcp.c:1113-1116`) — and it has **no `EXPORT_SYMBOL`**.

- **Minimal invariant set, if we ever do it ourselves** (all with `lock_sock(sk)`
  held): `TCP_SKB_CB(skb)->seq == tp->rcv_nxt`, `end_seq == seq + skb->len`, and
  after the append `rcv_nxt == end_seq` with `bytes_received` grown by `len`;
  `tp->copied_seq <= TCP_SKB_CB(skb)->seq`; `tp->rcv_wup <= tp->rcv_nxt` (else
  `tcp_sequence()` drops the segment, `:4499`); `sk_rmem_alloc` charged with
  `skb_set_owner_r()` after a successful `tcp_try_rmem_schedule()`; `ack_seq` and
  `tcp_flags` initialised; and an ACK/window update emitted afterwards, or the
  advertised window never moves.

- **Two concrete ways it kills the box.** `tcp_prune_queue()` calls
  `tcp_collapse(sk, &sk->sk_receive_queue, NULL, skb_peek(...), NULL,
  tp->copied_seq, tp->rcv_nxt)` (`tcp_input.c:5645`), which rebuilds that window
  and contains `BUG_ON(offset < 0)` (`:5451`) — a hole or out-of-range seq inside
  it is a kernel BUG. Separately, `tcp_recv_skb()` (`tcp.c:1531`) frees any skb
  whose `seq` is ahead of `copied_seq` without reading it (`:1550`), so an
  injected skb that is not contiguous is silently discarded, data and all.

- **Verdict for us:** do not append to a live socket's receive queue. Own the
  receive side (KCM shape: take `sk_data_ready`, drain with `tcp_read_sock()`,
  which *is* exported at `tcp.c:1639`, and queue into our own record space), or
  terminate TCP in userspace. `tcp_read_done()` is also exported (`tcp.c:1708`).

---

## 9. tc/act_connmark + iproute2 marking as a netfilter-module alternative

Sources: `net/sched/act_connmark.c`, `act_ctinfo.c`, `act_ct.c`, `act_skbedit.c`,
`net/netfilter/xt_connmark.c`, `xt_TPROXY.c`, `xt_mark.c`,
`net/ipv4/{ip_output.c,ip_sockglue.c}`, `include/net/dsfield.h`,
`include/net/inet_ecn.h`, RFC 2474, RFC 3168; iproute2 `e4cd9f1d`.

- **tc only *reads* `ct->mark`; netfilter writes it.** `tcf_connmark_act()` does
  `skb->mark = READ_ONCE(c->mark)` (`act_connmark.c:68` for the attached-ct case,
  `:84` for the tuple-lookup case) and never writes `ct->mark`. Among tc actions,
  only `act_ct` writes it, and it writes a **constant** from its own attribute:
  `tcf_ct_act_set_mark(ct, p->mark, p->mark_mask)` (`act_ct.c:898-912`, called at
  `:1072` under `if (commit)`). The only `skb->mark` → `ct->mark` copy is netfilter's
  `XT_CONNMARK_SAVE` (`xt_connmark.c:53-65`). So "no netfilter module" still means
  "netfilter *rules*" (`iptables -t mangle -j CONNMARK --save-mark`).

- **`connmark` assigns, it does not merge.** Any pre-existing fwmark (policy
  routing, `--tproxy-mark`, `SO_MARK`) is destroyed and replaced by `ct->mark`,
  which is 0 for every connection nobody saved. Use
  `action ctinfo cpmark` instead: `skb->mark = READ_ONCE(ct->mark) & cp->cpmarkmask`
  (`act_ctinfo.c:76`).

- **Only the process can tag packet 1.** `SO_MARK` → `__sock_set_mark()` (`sock.c:1509`,
  needs `CAP_NET_RAW || CAP_NET_ADMIN`), landing as
  `skb->mark = READ_ONCE(sk->sk_mark)` on transmit (`ip_output.c:534`). Per-socket DS
  field: `IP_TOS` → `ip_sock_set_tos()` (`ip_sockglue.c:1057`), landing as
  `iph->tos = (cork->tos != -1) ? cork->tos : READ_ONCE(inet->tos)` (`ip_output.c:1447`).
  Both are set before the first SYN leaves.

- **Verified CLI, each checked against its parser:**
  ```sh
  tc qdisc add dev eth0 clsact                                  # tc/q_clsact.c:10-22
  tc filter add dev eth0 egress matchall action connmark        # tc/m_connmark.c:20,63-90
  tc filter add dev eth0 egress matchall action ctinfo cpmark 0xff   # tc/m_ctinfo.c:20,89-98
  tc filter add dev eth0 egress matchall action ctinfo dscp 0xfc000000 0x01000000
  tc filter add dev eth0 ingress protocol ip flower ip_tos 0x04/0xfc action skbedit mark 0x1/0xff
  tc filter add dev eth0 egress matchall action ct commit mark 0x1/0xff   # tc/m_ct.c:24,130-155
  ```
  `ctinfo` rejects anything but six contiguous bits twice — in userspace
  (`"ctinfo: dscp mask must be 6 contiguous bits long"`, `m_ctinfo.c:132-137`) and in
  the kernel (`act_ctinfo.c:192-198`). `skbedit` does a masked read-modify-write:
  `skb->mark &= ~params->mask; skb->mark |= params->mark & params->mask`
  (`act_skbedit.c:83-84`).

- **Kernel config.** `CONFIG_NET_ACT_CONNMARK` needs
  `NET_CLS_ACT && NETFILTER && NF_CONNTRACK && NF_CONNTRACK_MARK`
  (`net/sched/Kconfig:829-833`); `CONFIG_NET_ACT_CTINFO` is identical (`:841-845`);
  `clsact` is `NET_SCH_INGRESS` (`:347-349`, alias `sch_clsact` `:361`).
  `NF_CONNTRACK_MARK` is a bool (`net/netfilter/Kconfig:114-116`) and `ct->mark`
  exists only under it (`nf_conntrack.h:113-114`). Without a ct, `tcf_connmark_act()`
  falls back to `nf_conntrack_find_get()` (`:72-82`) and on a miss jumps to `out`
  leaving `skb->mark` **untouched** (`:79-90`). NOTRACK'd packets are
  `IP_CT_UNTRACKED`, `nf_ct_get()` returns NULL, action is a no-op.

- **Six bits are available, and ctinfo preserves the other two.** IPv4 has
  `__u8 tos` (`uapi/linux/ip.h:97`); RFC 2474 §3 defines a 6-bit DSCP plus a 2-bit
  CU field (rfc2474.txt:371-372, :403-405). ECN is the low 2 bits
  (`INET_ECN_NOT_ECT=0, ECT_1=1, ECT_0=2, CE=3, INET_ECN_MASK=3`,
  `include/net/inet_ecn.h:13-19`). `tcf_ctinfo_dscp_set()` computes
  `newdscp = (((ct->mark & cp->dscpmask) >> cp->dscpmaskshift) << 2) & ~INET_ECN_MASK`
  (`act_ctinfo.c:36-37`) and calls
  `ipv4_change_dsfield(ip_hdr(skb), INET_ECN_MASK, newdscp)` (`:44-46`), where the
  mask argument means *preserve* (`dsfield.h:28-40`). Consequence: ECN survives,
  but **all six DSCP bits are replaced** — ctinfo cannot write a sub-field, so we
  must own the whole codepoint. Of 64 codepoints RFC 2474 §6 allocates 32 as
  `xxxxx0`, 16 as `xxxx11`, 16 as `xxxx01` (rfc2474.txt:795-800), with `xxx000`
  reserved for Class Selector compatibility (`:600`). Practical private budget:
  16 codepoints, minus whatever the path already uses; DSCP 0 is indistinguishable
  from "unmarked".

- **TPROXY and PREROUTING do not touch the DS field, but they consume
  `skb->mark`.** `tproxy_tg4()` reads `iph->protocol/saddr/daddr` plus L4 and
  assigns a socket (`xt_TPROXY.c:39-52`); its only skb write is
  `skb->mark = (skb->mark & ~mark_mask) ^ mark_value` (`:76`). `grep -c tos`
  returns 0 for both `xt_TPROXY.c` and `nf_tproxy_ipv4.c`. So TOS survives, but
  `--tproxy-mark`, our tag, and policy-routing fwmarks contend for the same 32-bit
  `skb->mark`. That asymmetry is why the usual carrier is fwmark (32 local bits)
  rather than DSCP (6 global ones).

- **First-packet asymmetry, exactly.** Egress: `sch_handle_egress()` runs inside
  `__dev_queue_xmit()` (`dev.c:4374`), i.e. *after* `NF_INET_POST_ROUTING`, and
  conntrack attached at LOCAL_OUT (`nf_conntrack_proto.c:243-247`) — so a
  POSTROUTING `CONNMARK --save-mark` has already written `ct->mark` when
  `action connmark` runs, and packet 1 works. Ingress: `sch_handle_ingress()` runs
  in `__netif_receive_skb_core()` (`dev.c:5530`), *before* `NF_INET_PRE_ROUTING`
  (`nf_conntrack_proto.c:236-241`), so for packet 1 of a new inbound connection
  there is no ct, the action falls through, and `skb->mark` keeps its old value.
  A fresh `nf_conn` is zeroed (`nf_conntrack.h:107-114`), so "unmarked" and
  "marked 0" are the same value.

- **Synthesis:** tag subflows with `SO_MARK` (packet 1, per socket), persist with
  `CONNMARK --save-mark`, recognise inbound with `action ctinfo cpmark`, and treat
  DSCP only as the optional on-wire carrier — accepting that a missing or bleached
  DSCP is indistinguishable from "not ours".

---

## 10. Verdict — what to copy, what not to

**Copy (shape, not code):**

1. **KCM's decorator attach/detach.** Take `sk_data_ready`/`sk_write_space`/
   `sk_state_change` under `lock_sock()`, save the originals, restore on detach
   (`kcmsock.c:1281-1285`, `:1350`). Drain with `tcp_read_sock()` (exported) so
   `copied_seq` and the window stay consistent, and keep the payload in a queue we
   own with `sk_rmem_schedule()` + a destructor for accounting. Back-pressure is
   "pause the reader", never "stall the window".
2. **sockmap's push loop** (`tcp_bpf_push()`, `tcp_bpf.c:88-148`): `MSG_MORE` while
   more elements remain, `goto retry` on a short write, `sk_mem_uncharge()` only
   when we own the charge, `ret <= 0` propagated. This is the correct way to write
   into another socket's send path from kernel context.
3. **MPTCP's mapping discipline**: derive the stream offset from
   `copied_seq - ssn_offset - map_subflow_seq` (`protocol.h:638-650`) rather than
   keeping a per-byte table, and carry an `already_sent` flag per fragment
   (`struct mptcp_data_frag`) so reinjection is idempotent.
4. **`nf_ct_seq_offset()`'s two-offset model** (`correction_pos` / `offset_before`
   / `offset_after`) if we ever rewrite sequence numbers: one offset is always
   wrong for packets already in flight.
5. **mptcpd's pull-based option writer** (`mptcp_pm_add_addr_signal()`, `pm.c:323`):
   control information is offered to the writer with a size budget and dropped if
   it does not fit, never pushed from a timer.

**Do not copy:**

1. **Receive-queue injection.** `tcp_queue_rcv()`/`tcp_send_rcvq()` are static or
   unexported, require TCP_REPAIR and a quiesced socket, and a single non-contiguous
   skb inside `[copied_seq, rcv_nxt)` reaches `BUG_ON(offset < 0)` in
   `tcp_collapse()` (`tcp_input.c:5451`). There is no safe module-side path.
2. **Calling the MPTCP path manager.** `mptcp_pm_add_addr_received`,
   `mptcp_pm_new_connection`, `mptcp_pm_create_subflow_or_signal_addr` have no
   `EXPORT_SYMBOL*`; only four limit getters are exported. Either speak
   `mptcp_pm_cmds` netlink like mptcpd, or implement our own PM.
3. **kretprobe on `tcp_sendmsg`.** It fires after `release_sock()`, yields only a
   byte count, and races every other writer. The `skb`s and their sequence numbers
   are only visible inside `tcp_write_xmit()`/`__tcp_transmit_skb()`, which are
   `static`.
4. **The seq-doubling trick as the core mechanism.** It requires the peer to invert
   the mapping exactly, it must survive retransmission (which reuses
   `TCP_SKB_CB(skb)->seq`) and SACK rewriting, and it only defeats a shaper that
   trusts `seq` deltas rather than `skb->len`. Keep it as an experiment behind a
   flag, not as the architecture.
5. **connmark/DSCP as the primary identification path.** `action connmark` only
   *reads* `ct->mark`, the first inbound packet has no ct yet, and DSCP gives 6
   bleachable bits. `SO_MARK` on each subflow socket is strictly better and works
   on packet 1.
6. **OpenVPN's id scheme as a precedent.** The id that keys a flow is a 24-bit
   server-assigned slot carried in the first three payload bytes; the only hash is
   a remote-only SipHash that never goes on the wire. A handshake-free
   MD5-derived stream id is our invention, not prior art.

**What is genuinely ours to build** (no source to copy, and no source should be
invented):

- The **predictive injector**: a reorder-gap estimator plus the decision rule "if
  fewer than M packets are missing, extrapolate and inject now". MPTCP only
  re-sends what it knows is lost; RFC 7198 duplicates blindly at a fixed rate.
  Neither implements a threshold on the *observed* gap.
- The **N+M HTTP/1.1 framing** and its control channel. HTTP/1.1 keepalive means
  the payload rides inside a request/response body; parsing that in kernel is a
  liability. Put the HTTP layer in userspace, keep the duplication and pacing
  decisions in the module, and define one narrow interface between them.
- The **bucket arithmetic**. The ISP limits per 5-tuple, so N+M sockets give N+M
  buckets — but every duplicate spends capacity in two buckets at once. The
  speedup ceiling is `(N+M)/N` only while the injector is wrong rarely enough that
  duplicates stay a small fraction of bytes. That trade-off, not the kernel
  plumbing, is the thing to measure first.

**One structural warning.** Items 2–5 all failed to resolve, and two of the
"facts" in the brief (`fill_md5()`, `sock_gen_cookie_lookup()`) do not exist in the
code they name. Treat the brief's other unverified claims the same way: check the
symbol exists and is exported before designing around it. The kernel's export list,
not a blog post, decides what a module can call.
