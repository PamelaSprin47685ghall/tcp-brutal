# tbtun review — findings

Reviewer: independent (no file ownership). Scope: `tbtun/*.c`, `tbtun/*.h` against
`DESIGN.md` and `docs/*.md`. At the time of the original review, no file outside
`review/` was modified.

> **Update 2026-11-05:** the status table below now records the P3 grammar, config-read
> and documentation changes. Those changes were made outside `review/` (DESIGN.md,
> README.md, tbtun/tbt_parse.*, tbtun/tbt_procfs.c); the original review itself did not
> modify them.

**Snapshot.** The tree changed while this review ran (`tbt_carrier.c`,
`tbt_rx.c`, `tbt_session.c`, `tbt_tx.c`, `tbt_uapi.h` were rewritten at
03:34–03:39; a `TBT_F_RETIRE` frame was added). Findings and line numbers below
are against the working tree as of that rewrite, git HEAD `d2397ff`. If the
files move again, re-check the cited lines before acting.

**Status (05:27 snapshot).** 16 of 16 lint checks pass and 14 of 14 self-tests
pass. The code findings 1–11, 17, 27–29, 31, 33, 36a–d, 37, 38 and 39 were fixed
at that snapshot (23 was fixed later, after the parser change); the resolution tables
below give the line numbers as of 05:19/05:27.
At that snapshot, the spec-drift table and findings 12–16, 18–26, 30, 32, 34 and
35 were open; see the "Current status (2026-11-05, after P0/P1 partial fixes)" table
below for what has since changed.

Build status (verbatim; 03:38 build, superseded by the 355/0 run recorded in the current
table; kbuild found the 03:41 objects up to date at that time):

- `source ~/.cache/tbtun-build/env.sh && make check` → EXIT=0, `235 checks, 0 failures`,
  `test_fuzz: seed=1 checks=7879428 failures=0`, `test_http: seed=0x9e3779b97f4a7c15
  checks=249 failures=0`, h11 cross-check accepted the emitted request/response.
- `make host-test-san` → EXIT=0, `235 checks, 0 failures`, `test_fuzz: … failures=0`,
  `test_http: … failures=0`, `test_http_fuzz: seed=0x0000000000000001 checks=4078276
  failures=0`.

The host suites exercise `tbt_parse.c`, `tbt_rec.c`, `tbt_proto.c` and `tbt_http.c`
only. Findings 1-10 are not reachable from those units: they are in the kernel object
lifetime, locking and accounting code. Of the later findings the host suites cover the
parser side (23, 25, 27, 29 and the key removal in 35); they do not cover `tbt_procfs.c`,
`tbt_carrier.c`, `tbt_rx.c`, `tbt_session.c` or `tbt_tx.c`.

---

## Final status (0.0.1, 2026-11-06)

**Tree.** `b57b438`; `a9d7c47` adds only the evidence under `review/final/`,
`review/final-qemu/`, `review/final-bench/` and `review/dkms-qemu/`. The module
built from this tree is
`sha256 0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63`
(the same hash in every 2026-11-06 bench and adversarial row quoted below).
Evidence: `review/final/` (host, QEMU A–K, adversarial), `review/final-qemu/`
(run logs), `review/final-bench/` (throughput, flush, writer) and
`review/dkms-qemu/` (DKMS install/load/unload on a clean guest kernel). Line
numbers are final-tree line numbers; the per-finding sections that follow keep the
original analysis and the "Fixed (…)" rationale.

**Status values.** `FIXED` = the fix is present and a named check covers it.
`FIXED (static)` = the fix is present; no runtime test targets that unit.
`OPEN` = the defect is present; the release rationale is stated in the row.
`FIXED (contract)` = the code deliberately does not provide the old behaviour; the
test contract was rewritten and the new contract passes.

**Acceptance on this tree (commands and measured output).**

```sh
source ~/.cache/tbtun-build/env.sh && make check
# test_parse 357 checks, 0 failures
# test_fuzz seed=1 checks=7550734 failures=0
# test_http seed=0x9e3779b97f4a7c15 checks=249 failures=0
# test_reorder 52792 checks, 0 failures
# test_ack 10026 checks, 0 failures
# test_control 181 checks, 0 failures
# test_multistream 68580 checks, 0 failures
# test_anchor 124 checks, 0 failures
# h11: accepted POST /api/v2/upload HTTP/1.1 and HTTP/1.1 200 OK

make host-test-san
# test_parse 357 checks, 0 failures
# test_fuzz checks=7550734 failures=0
# test_http checks=249 failures=0
# test_http_fuzz checks=4078276 failures=0

bash review/lint.sh
# 16/16 checks passed, 0 failed
# 14/14 self-tests passed, 0 failed

grep -rn 'predict_\|PREDICT' tbtun/          # only the removal comment in tbt_uapi.h
grep -rn 'spec_bytes' tbtun/                 # empty
grep -rn 'fbk_mask' tbtun/                   # empty
grep -rn 'src=' tbtun/                       # empty (the key does not exist)
```

`test_parse` now reports 357 checks; 365 was the older snapshot taken before the
five dead keys `framing`/`chunk`/`max_retry`/`dev`/`uid` and their parse cases
were deleted. What was deleted is the tests for keys already removed from the
grammar, not behavioural coverage; `make check` still reports 0 failures.

The QEMU A–K probes and the adversarial groups are separate suites; their final
results are archived under `review/final/`, `review/final-qemu/` and
`review/final-bench/`. On this tree (f1282df, same module sha as b57b438): QEMU
A–K is 11 PASS / 0 FAIL / 0 SKIP (165.7 s); adversarial is 91 PASS / 0 FAIL /
0 SKIP (284.4 s, all 18 groups plus the crash repro); `pub.churn` is 10000/10000
with `retried=0`; `pub.writer` stays blocked in `write()` (`errno=0 eagain=0
blocked=1 child_state=running`, no `EAGAIN`) and `flush` returns the memory,
while `pub.writer_drain`/`_1g` complete 100 MiB/1 GiB byte-exact; DKMS
install/load/unload/uninstall is 38 steps / 39 checks / 0 failures on a clean
guest kernel
(`review/dkms-qemu/`). The host suites
exercise `tbt_parse.c`, `tbt_rec.c`, `tbt_proto.c`, `tbt_http.c` and the stream
control unit only. Findings 1–10 are in kernel object lifetime, locking and
accounting code: the host suites cannot reach them, so their evidence is the
static fix plus the named QEMU/adversarial probe. Findings 23, 25, 27, 29 and the
key removal in 35 are host-covered.

### Code findings 1–39

| Finding | Severity | Status | Final evidence |
|---|---|---|---|
| 1 orphaned carrier | critical | FIXED | `tbt_carrier_retire()` parks the replaced carrier on `sess->dead` (`tbt_carrier.c:1016`); the dead drain frees it after retire (`:1242`). QEMU E/K cover replacement. |
| 2 trim UAF | critical | FIXED | `tbt_tx_retain_trim()` (`tbt_tx.c:421`) runs only from the pump (`:1732`) under `pump_lock`; it clears `fr->ret` for every queued frame before `kfree()`. |
| 3 create failure / old leak | high | FIXED | `tbt_session_destroy(t)` on create failure (`tbt_session.c:231`); `old` destroyed on a successful replace (`:240`) and on the undo path (`:252`). |
| 4 callback teardown | high | FIXED | every `sk_user_data` reader takes `sk_callback_lock` (8 sites in `tbt_carrier.c`, `tbt_listen.c`, `tbt_sock.c`); `tbt_listen_stop()` restores the core callbacks and clears `accepting` before `cancel_work_sync` (`tbt_listen.c:145-150`). |
| 5 double stream insert | high | FIXED | the re-check and `hlist_add_head_rcu()` share one `sess->lock` section (`tbt_session.c:642`). |
| 6 pump-lock deadlock | high | FIXED | `w_tx`/`w_pump` are cancelled after `pump_lock` is dropped (`tbt_carrier.c:1172-1185`); `tbt_carrier_retire()` no longer cancels them. |
| 7 sk release order | high | FIXED | `WRITE_ONCE(st->sk, NULL)` precedes `sock_release()` (`tbt_sock.c:540`). |
| 8 accept before session | medium-high | FIXED | `accepting` is set only after the session exists (`tbt_session.c:236`); `tbt_listen_start()` leaves it 0. |
| 9 short write | medium-high | FIXED | the unwritten tail stays in `c->part` and is drained first (`tbt_carrier.c:763-783`); admission is the real `sk_stream_memory_free()` test (`tbt_tx.c:1965`); the retx mark adds `part_len` (`:2221`). |
| 10 peers `c->sk` | medium | FIXED | `sock_hold`/`sock_put` around each `c->sk` dereference (`tbt_session.c:385`, `:402`). |
| 11 RETIRE / TBT_CONV | medium | FIXED | `TBT_CONV 0x0005` (`tbt_uapi.h:61`), `TBT_F_RETIRE 0x24` (`:80`), bounded in `tbt_body_bounds()`; DESIGN §3.3 lists the type once. |
| 12 HELLO bit layout | medium | FIXED | TX `profile \| (gen << 2)`, RX `& 3` / `>> 2`; the code comment gives the 5:2 layout (`tbt_carrier.c:1397-1398`). |
| 13 PREDICT inert | medium | FIXED (removed) | 0x11/0x12 deleted; `tbt_uapi.h:70` records the removal; `TBT_CONV 0x0005`; `grep -rn 'predict_' tbtun/` is empty. |
| 14 `data_frames` before send | medium | FIXED | `data_frames++` only after a successful write or a deliberate injector drop (`tbt_carrier.c:658`, `:741`, `:825`). |
| 15 `fbk_mask` unused | medium | FIXED | the parameter and its call-site argument are gone; `grep -rn fbk_mask tbtun/` is empty. |
| 16 first-HELLO nonce | medium | FIXED | source anchor is checked before the nonce (`join_src_reject`, `tbt_rx.c:470`); `nonce=`/`peer=` pin it (`tbt_session.c:1001-1013`); TOFU warns once; QEMU `far.anchor`/`far.dup_index`/`far.srcaddr_free`/`far.nonce_pinned`/`far.src_anchor` PASS; `test_anchor` 124/0. |
| 17 http proc entry | medium | FIXED | `remove_proc_subtree(TBT_PROC_SUBDIR, init_net.proc_net)` (`tbt_procfs.c:994`). |
| 18 module parameters | medium | FIXED | `max_carriers` (`tbt_session.c:123`), `max_tunnels` (`:199`); `netns_optin` removed. |
| 19 one-shot config read | low | FIXED | exact-size `kvmalloc` snapshot served by offset (`tbt_procfs.c:292`). |
| 20 overlapping reorder | low | FIXED | `tbt_rx_pull_locked()` evicts a fully stale node and clips a straddling one (`tbt_rx.c:33-90`); QEMU `far.overlap` PASS (ooo_bytes stays 0). |
| 21 non-atomic counters | low | FIXED | multi-writer session counters are `atomic64_t` read through `tbt_stat_read()` (`tbtun.h:36`, `:483-568`); `review/lint.sh` `atomic-stats` pins the set. |
| 22 pinned control on `-EAGAIN` | low | FIXED | control frames are retried/requeued, not freed (`tbt_tx.c:1585`); `ctl_lost/ctl_retry/ctl_requeue/ctl_obsolete/ctl_queued` in the `tunnel <id> ctl` line; QEMU `near.fin_eagain`/`near.rst_eagain` PASS. |
| 23 zero reorder/hole budgets | low | FIXED | both are refused at 0 (`tbt_parse.c:474-482`); `test_parse` 357/0. |
| 24 hole age | nit | FIXED | age is computed before `hole_since_ns` is cleared (`tbt_rx.c:923-924`). |
| 25 `ports 0-0` | nit | FIXED | 0-0 is the canonical no-block spelling (`tbt_parse.c:230`, `:247`); the writer treats blank/0-0 as a no-op (`tbt_procfs.c:427`). |
| 26 carrier `SO_REUSEADDR` | nit | FIXED | the code sets it (`tbt_carrier.c:1336`, `tbt_listen.c:130`); DESIGN §7.2 says "on". |
| 27 zero-length DATA | high | FIXED | `TBT_F_DATA` min is 1 (`tbt_uapi.h:151-156`); `test_fuzz` 7,550,734/0. |
| 28 http header buffer | medium | FIXED | `c->http_hdr = kmalloc(TBT_HTTP_HDR_MAX)` (`tbt_carrier.c:846`), `TBT_HTTP_HDR_MAX` = 4096. |
| 29 render > line max | medium | FIXED | `TBT_LINE_MAX 1024` (`tbt_parse.h:33`); maximal render 642; `test_parse` round-trips it. |
| 30 32 KiB truncation | medium | FIXED | exact-size `kvmalloc` (`tbt_procfs.c:292`); `TBT_CFG_RENDER_MAX` is gone. |
| 31 release overflow | medium | FIXED | the drain refuses an over-long tail (`lb->overflow`). |
| 32 blank line to `/ports` | low | FIXED | blank line and 0-0 are no-ops (`tbt_procfs.c:427`). |
| 33 torn http read | low | FIXED | the block is copied under `pump_lock` (`tbt_procfs.c:523-568`). |
| 34 partial procfs init | low | FIXED | subtree removal (`tbt_procfs.c:994`); lint `proc-entries-removed` accepts the subtree proof. |
| 35 dead keys/functions | nit | FIXED | the five keys are gone; `tbt_passive_port()`/`tbt_tunnel_flush()` are removed; `grep` is empty. |
| 36a snd_una gate | high | FIXED | `first_sent_seq[]`/`una_observed_ns[]` (`tbtun.h:259-264`); the gate tests `snd_una` (`tbt_tx.c:902-908`) and the mark is set after the write (`:2219-2222`). |
| 36b trim vs batch | high | FIXED | the trim runs only from the pump (`tbt_tx.c:421`, `:1732`). |
| 36c teardown | high | FIXED | `tbt_carriers_stop()` then `tbt_tx_free_queue()` before the unlink loop (`tbt_session.c:1056`, `:1084`). |
| 36d short-write accounting | high | FIXED | the mark is `write_seq + c->part_len` (`tbt_tx.c:2221`). |
| 37 `http_rx` NULL | critical | FIXED | `http_rx` is allocated (`tbt_carrier.c:849`) and in the failure test (`:850`). |
| 38 buffer leak | medium | FIXED | `tbt_carrier_free()` (`tbt_carrier.c:1024`) is used at `:853`, `:1151`, `:1251`. |
| 39 constructor order | high | FIXED | `INIT_LIST_HEAD(&c->node)` (`tbt_carrier.c:844`) precedes the failure path at `:853`. |

### Adversarial findings F1–F12

| ID | Severity | Status | Final evidence |
|---|---|---|---|
| F1 close-flush | critical | FIXED | `ADV_RESULT ctl.close_flush PASS` (`review/final/adversarial-results.txt`, module `0b8f0006…`). |
| F2 rmmod | critical | FIXED (static + runtime) | teardown order `tbt_procfs_exit` → `tbt_tunnel_shutdown` → `synchronize_rcu` → `rcu_barrier` (`tbt_core.c`); every stream socket is detached/released before the module text goes away. QEMU A–K 11/11 PASS on `0b8f0006…` exercises load/unload; the final 91 PASS run includes `init.rmmod` (rmmod rc=0, app closed cleanly) and `init.dmesg_after_rmmod` (no warning/BUG/oops), archived in `review/final/adversarial-results.txt`. The older 12:02 FAIL was the probe printing `ECHO_ABORT op=read errno=0`; `read_to()` now reports the pending `SO_ERROR` (`adv_probe.c:189-218`). |
| F4 two connections / churn | high | FIXED | `pub.two` PASS; `pub.churn` PASS 10,000/10,000 usable, 0 refused, 0 other; `pub.concurrent` PASS 100/100. The churn verdict uses a 2 s timeout per cycle and one retry only after a timeout (`retried=`); two final-criterion runs read `retried=1` and `retried=0`, and the archived `review/final/adversarial-results.txt` is the `retried=0` run. The far stream detaches on the graceful FIN path (`tbt_stream_detach_from_session()`, `tbt_session.c:800`), so `n_active` no longer climbs to 128. |
| F5 half-close | high | FIXED | `pub.halfclose` PASS, EOF=1. |
| F6 far-side carrier input / delivery stall | high | FIXED | the RX work is re-armed on every `-EAGAIN`/read-zero path (`review/analysis/multiconn-p01-plan.md`); QEMU `far.second_carrier` PASS (`rx_adopt_events=2`), `far.open_other_sid` PASS, `far.data_path` PASS, `near.data_path` PASS, `near.second_conn` PASS. |
| F7 deprecated workqueue | high | FIXED | no `system_unbound_wq`/`alloc_workqueue` in `tbtun/`; `w_fbk` runs on `system_dfl_wq` (`tbt_tx.c:2578`). |
| F8 `src=` allow list | medium | FIXED (contract) | `src=` does not exist; a connection is identified by the nonce, not by a source allow list. `pub.srcfilter` PASS under the rewritten contract (`adversarial/results.txt`: "no src= allow list; the tunnel identity is the nonce"); `init.src_key` now checks that `src=` is refused and `nonce=` is accepted and rendered (`adversarial/init:182-203`). |
| F9 replayed nonce from another source | medium | FIXED | `far.srcaddr_dup` PASS, `far.srcaddr_free` PASS; a configured `peer=`/`nonce=` anchors the source (`far.src_anchor`, `far.nonce_pinned` PASS). |
| F10 `max_tunnels` | medium | FIXED | checked under `tbt_lock` before allocation (`tbt_session.c:199`). |
| F11 `max_carriers` | medium | FIXED | checked in `tbt_cfg_sane()` (`tbt_session.c:123`). |
| F12 port-block exhaustion | medium | FIXED | `port_exhaust`/`port_alloc_fail` in stats; `init.port_exhaust` PASS (`port_exhaust=16`, `alloc_fail=16`, tunnel stays up). |

### P0–P3 checklist

| Item | Status | Final evidence |
|---|---|---|
| P0-1 replacement/retire lock-order regression | FIXED | `review/lint.sh` 16/16 + 14/14; QEMU A–K 11/11 PASS (E/K included). |
| P0-2 F1/F2 regression | FIXED | `ctl.close_flush` PASS; `init.dmesg_after_rmmod` PASS; final `init.rmmod` archive as above. |
| P1-3 PREDICT receiver advance | FIXED (removed) | placeholder path deleted; `TBT_CONV 0x0005`; QEMU A–K 11/11 PASS, G/H included, `holes_unrecoverable=0`. |
| P1-4 sequential churn | FIXED | `pub.churn` 10,000/10,000 usable, 0 refused, 0 other in `review/final/adversarial-results.txt` (`retried=0`); the second final-criterion run read `retried=1`. Detach is in the final tree (`tbt_session.c:800`). |
| P1-5 first-HELLO nonce | FIXED | `far.nonce_pinned`/`far.src_anchor` PASS; `test_anchor` 124/0. |
| P1-6 F6 far-side input | FIXED | `far.second_carrier`/`far.data_path` PASS. |
| P1-7 near.data_path / near.second_conn | FIXED | both PASS in `adversarial/console-near.log`. |
| P2-8 `retx_ppm` | FIXED | Final matrix in `review/final-bench/summary.md`: n=1 0 (3.11 Mbit/s), n=4 2,333 (10.74), n=8 2,041 (22.36), n=16 12,719 (32.75); whole-run `wire/app` 1.0123–1.0268, all `strict=1 rc=0`, `holes_unrecoverable=0`. At n=4 the CSV's 2 s window reads 1.0333 while the whole run reads 1.0138. One earlier n=4 run of the same module read 1,842 ppm (8.38 Mbit/s) and another 11.37 Mbit/s, which is the measured variance. The 96k–304k figures were the old accounting; the split is `retx_hole/retx_slow/retx_pathloss/retx_other`. |
| P2-9 `flush` block | FIXED | `init.flush_midstream` PASS (app closes cleanly, `ECHO_ABORT errno=104`), `init.echo_after_flush` PASS, `stall.flush` PASS. |
| P3-10 comment/doc drift | FIXED | `tbt_proto.c` header/payload comment; HELLO 5:2 comment; DESIGN §3.3. |
| P3-11 F8 test contract | FIXED (contract) | `init.src_key`/`pub.srcfilter` rewritten to the nonce/peer contract and passing. |
| P3-12 low-priority open items | FIXED | review 21 atomic stats, 15/22/24/34 all closed above. |
| P3-13 dead declarations | FIXED | `tbt_passive_port()`/`tbt_tunnel_flush()` gone; grep empty. |
| P3-14 port-exhaustion visibility | FIXED | `init.port_exhaust` PASS; `port_exhaust`/`port_alloc_fail` in stats. |

### History

The 05:19 and 18:0x status tables that used to sit here were removed: they recorded
the tree while the P0/P1 fixes were landing, and several rows still said `OPEN` for
defects the final tree fixes. Keeping them next to the final table invited the wrong
reading. The per-finding analysis and the "Fixed (…)" paragraphs below are kept
verbatim; the full old file is in git history (`git show 68bfbaa:review/findings.md`).

---

## 1. critical — a replaced carrier is orphaned; its socket and work items outlive the session

**Where:** `tbtun/tbt_session.c:753-775` (join), `tbtun/tbt_carrier.c:786-857`
(`tbt_replace_work`), `tbtun/tbt_carrier.c:860-905` (`tbt_carriers_stop`).

**What breaks.** `tbt_session_join()` does `sess->car[index] = c` (line 760) and, for a
dead occupant, only `atomic_set(&old->stopping, 1)` plus `schedule_work(&sess->w_replace)`
(lines 767-775). `old` is on no list: its own join did `list_del_init(&c->node)` (line
766), so it left `sess->prov`, and the array slot no longer names it.
`tbt_replace_work()` scans only `sess->car[i]` and `sess->prov` (lines 786-857);
`tbt_carriers_stop()` scans only `sess->car[]`, `sess->prov` and `sess->dead`. Therefore
`old` is never passed to `tbt_carrier_retire()`: its socket is never released, its
port-block bit is never freed, and its `w_poll`/`w_pump` are never cancelled. At
`tbt_session_destroy()` the session is `kfree()`d (line 716) while `old->sess` still
points at it and its delayed work can still fire (`tbt_car_poll_work` dereferences
`sess->tunnel` at line 190), and after `rmmod` the live socket still has
`sk_data_ready = tbt_car_ready` pointing into unloaded module text.

The new `tbt_session_retire()` (`tbt_session.c:727-738`, reached by the
`TBT_F_RETIRE` frame) only sets `stopping`; it does not put `old` on a list, so it
does not close this hole.

**Fixed (05:19, code; rerun pending).** `tbt_session_join()` adds `old` to `sess->dead`
(`tbt_session.c:831`) and the dead drain retires then frees
(`tbt_carrier.c:1031`, `:841-853`), so the post-unload UAF is closed. The
replaced socket is still held until session teardown, so a long-lived session
accumulates one socket per replacement; that is a resource cost, not a UAF.

**Reachable** whenever a far-side replacement HELLO arrives before `w_replace` has
retired the previous occupant — i.e. exactly the carrier-replacement path the design
exists for (`tbt_session_join` counts it as `carrier_replaced`).

**Minimal fix.** Keep `old` reachable for retirement: add `struct list_head retire` to
the session, `list_add_tail(&old->node, &sess->retire)` under `sess->lock` in the join
replacement branch, and have `tbt_replace_work()` and `tbt_carriers_stop()` drain that
list through `tbt_carrier_retire()` (which already moves the object to `sess->dead`).

## 2. critical — the retention trim frees `struct tbt_retain` objects that queued frames still point at

**Where:** `tbtun/tbt_tx.c:1615-1637` (trim in `tbt_tx_submit`), used at
`tbtun/tbt_tx.c:1221-1222`, `1326-1329`, `1344-1347`, `1405-1414`.

**What breaks.** With `tx_retain_bytes` set (`cfg.tx_retain_bytes`, parsed at
`tbt_parse.c:552-556`), the trim moves the oldest `struct tbt_retain` entries to a local
list, drops `tx_lock`, and `kfree()`s them. It never clears the `fr->ret` pointers of
frames still in `sess->tx_q`. The pump later reads and writes `fr->ret->path_mask`,
`reserve_mask`, `dup_pending`, `first_sent_seq`, `last_path`, `sends`, `data_frames` —
a use-after-free write into freed slab. `tbt_tx_retx()` additionally collects raw
`struct tbt_retain *` under `tx_lock` and copies them after dropping it
(`tbt_tx.c:388-500`), on the stated assumption that "only the pump frees retention
entries"; this path violates that assumption from the capture work item.
If `tx_retain_bytes < 8192` the entry the just-queued frame points at is freed in the
same call, so the UAF needs no race at all.

**Evidence that the correct pattern exists:** `tbt_tx_retain_gc()` does clear
`fr->ret` for every queued frame before freeing (`tbt_tx.c:248-290`, the loop at
`tbt_tx.c:280`).

**Minimal fix.** Reuse that loop in the trim: under `tx_lock`, for each victim walk
`sess->tx_q` and set `fr->ret = NULL` (and clear `dup_copy` bookkeeping), then free
outside the lock. Alternatively free only entries whose `seq + len <= tx_ack`, which is
what `retain_gc` already guarantees.

## 3. high — tunnel creation failure frees the tunnel while the session and carriers still reference it; a failed re-apply leaks the old tunnel

**Where:** `tbtun/tbt_session.c:186-215`.

**What breaks.** Two defects at one site:

(a) `tbt_session_create()` returns the result of `tbt_carriers_start()` (line 637). On
failure the code does `tbt_listen_stop(t); goto undo;` (lines 199-204) and `undo` calls
`tbt_tunnel_put(t)` (line 214) — but never `tbt_session_destroy(t)`. `t->sess` is set
(`tbt_session.c:636`), so the session, its `get_net()` reference, and any carrier whose
`w_connect` was already scheduled survive with `sess->tunnel` pointing at a tunnel that
`kfree_rcu()` will free. `tbt_car_connect_work()` then dereferences `t->cfg`
(`tbt_carrier.c:929-940`).

(b) The old tunnel is removed from `tbt_tunnels` and its listener stopped at lines
186-191 *before* the new configuration is started. If `tbt_listen_start(t)` fails —
reachable without OOM, e.g. `EADDRINUSE` when the same `listen=` port is held by
another process — `goto undo` frees only the new tunnel; `old` is never
`tbt_session_destroy()`ed or `tbt_tunnel_put()`ed. Its carriers, target connection and
feedback timer keep running forever, unreachable from `/proc`.

**Minimal fix.** On the failure paths call `tbt_session_destroy(t)` before
`tbt_tunnel_put(t)`; and do not detach `old` until the replacement has started, or
restore it on failure (destroy+put it in `undo` when it was removed).

## 4. high — listener teardown cancels work before clearing the callback, and the listener callback reads `sk_user_data` without `sk_callback_lock`

**Where:** `tbtun/tbt_listen.c:141-151` (order), `tbtun/tbt_listen.c:33-41` (`tbt_ready`).

**What breaks.** `tbt_listen_stop()` calls `cancel_work_sync(&t->w_accept)` (line 142)
and only afterwards clears `sk_user_data`/`sk_data_ready` (lines 145-148). A softirq
that entered `tbt_ready()` before the clear can call `schedule_work(&t->w_accept)`
after the cancel has returned. `tbt_tunnel_put()` then `kfree_rcu()`s the tunnel
(`tbt_session.c:143-148`), and the requeued work runs `tbt_accept_work()` on freed
memory (`tbt_listen.c:82-90`). `tbt_ready()` reads `sk->sk_user_data` with no
`sk_callback_lock` (line 35), unlike `tbt_pub_ready()`/`tbt_pub_write_space()` which do
take it (`tbt_sock.c:112-121`). `tbt_car_ready()` (`tbt_carrier.c:64-74`) has the same
unlocked read; the carrier struct survives longer, but the same race exists at session
teardown.

**Minimal fix.** Clear the callbacks under `write_lock_bh(&sk->sk_callback_lock)` first,
then `cancel_work_sync()`; take `read_lock(&sk->sk_callback_lock)` in `tbt_ready()` and
`tbt_car_ready()`.

## 5. high — `tbt_stream_get(alloc=true)` can insert two streams for one sid

**Where:** `tbtun/tbt_session.c:519-561`.

**What breaks.** Lookup (lines 520-529) and insertion (lines 558-560) are two separate
critical sections with an allocation in between. `tbt_far_open_seen()` (called from a
carrier's `w_rx`) and `tbt_target_attach()` (called from `w_target`) both call
`tbt_stream_get(sess, sess->far_sid, true, false)`; both can miss and both
`hlist_add_head_rcu()` an object for the same sid. The hash then holds two streams with
the same key: one owns the target socket and the queues, the other is an orphan holding
its two references, its work items and its `rx_limit`. Later lookups return whichever
was inserted last, so delivery and control can act on the object that is not attached.

**Minimal fix.** After re-taking `sess->lock` for the insert, re-run the lookup; if an
entry appeared, unlock and free the newly allocated object (return the existing one with
a reference).

## 6. high — `tbt_carriers_stop()` can deadlock on `cancel_work_sync(&c->w_tx)` while holding `pump_lock`

**Where:** `tbtun/tbt_carrier.c:869-881` (pre-cancel), `tbtun/tbt_carrier.c:883-898`
(retire under `pump_lock`), `tbtun/tbt_carrier.c:700-711` (retire's second cancel).

**What breaks.** The pre-loop cancels `w_tx` once, then inside
`mutex_lock(&sess->pump_lock)` `tbt_carrier_retire()` calls
`cancel_work_sync(&c->w_tx)` again (line 711). Between the two cancels a `w_tx` can be
queued by a poll or write-space callback that passed its `stopping` test before line 874
set the flag (`tbt_car_poll_work` line 302, `tbt_car_write_space` line 109). That work
starts, calls `tbt_tx_pump()`, and blocks on the `pump_lock` this thread holds;
`cancel_work_sync()` then waits for it forever. `rmmod` / `del` hangs unkillably.

**Fixed (05:19, code; rerun pending).** `w_tx` and `w_pump` are cancelled before `pump_lock`
(`tbt_carrier.c:967-968`, `:999-1000`, `:1005-1006`), and
`tbt_carrier_retire()` no longer cancels either (`:826-840`), so a work item
that waits on the lock can no longer be waited on while the lock is held.

**Minimal fix.** Split retire: `tbt_carrier_retire_locked()` without the `w_tx` cancel
for the `carriers_stop()` path (a queued `w_tx` that starts after retire returns sees
`stopping` in `tbt_car_tx_work()` and exits without taking `pump_lock`). Keep the
cancel in the `w_replace` path, which calls retire outside the lock.

## 7. high — the stream's socket is released before `st->sk` is cleared, so a concurrent kill shuts down a freed socket

**Where:** `tbtun/tbt_sock.c:330-344`; racer at `tbtun/tbt_session.c:574-596`.

**What breaks.** `tbt_kill_work()` calls `sock_release(sock)` (line 343) and only then
`WRITE_ONCE(st->sk, NULL)` (line 344). `tbt_stream_kill()` reads `st->sk` and calls
`kernel_sock_shutdown(sk->sk_socket, SHUT_RDWR)` (lines 576, 586-587); it is called from
the RX path (`tbt_rx_data` reorder overflow, `tbt_rx_check_hole`, `tbt_rx_frame` RST) and
from `tbt_ctrl_work`. A kill in that window dereferences a released `struct socket`.
The same window exists between `sock_release()` and the `st->sess->target` clear at
line 341 (the clear is before release, correctly, but the socket is still reachable
through `st->sk`).

**Minimal fix.** Publish the removal first: `sock = sk->sk_socket; WRITE_ONCE(st->sk,
NULL);` (and clear `sess->target`) before `sock_release(sock)`.

## 8. medium-high — an accepted public connection can reach `tbt_stream_start()` with `t->sess == NULL`

**Where:** `tbtun/tbt_session.c:196-199`, `tbtun/tbt_listen.c:57-70`.

**What breaks.** `tbt_listen_start()` installs `sk_data_ready` and sets `accepting=1`
(`tbt_listen.c:117-126`) before `tbt_session_create()` runs. A connection completing in
that window schedules `w_accept`; `tbt_accept_one()` calls
`tbt_stream_start(t->sess, newsock)` with `t->sess == NULL`, and
`tbt_stream_start()` immediately does `spin_lock(&sess->lock)` (`tbt_sock.c:409`) →
NULL-pointer oops. The far-side path is guarded (`tbt_carrier_adopt()` returns
`-ENOTCONN`), the near-side path is not.

**Minimal fix.** Create the session before starting the listener, or make
`tbt_accept_one()` refuse while `t->sess` is NULL (release the socket and return).

## 9. medium-high — a short carrier write is treated as a dead carrier; the admission test leaves only one byte of headroom

**Where:** `tbtun/tbt_carrier.c:486-489`, `tbtun/tbt_carrier.c:590-604`,
`tbtun/tbt_tx.c:1241-1255`.

**What breaks.** The pump admits a record when `sk_stream_memory_free(c->sk)` is true.
In this kernel that is `sk_wmem_queued < sk_sndbuf` (`include/net/sock.h:1413-1426`,
verified in the 7.2.9 headers), i.e. one byte of room, not `len` bytes. A raw record is
32 + up to 8192 bytes; `tcp_sendmsg_locked(MSG_DONTWAIT)` copies what fits and returns a
short count, and `tbt_carrier_xmit()` maps any short count to `-EIO` (line 603), which
`tbt_tx_pump()` treats as a dead carrier (`tbt_carrier_lost()`, `tbt_tx.c:1385-1395`).
So a healthy carrier at its high-water mark is killed and redialled instead of waiting
one ACK. This is the normal state near the send-buffer limit, not an error.

**Minimal fix.** Make admission require the whole record to fit:
`sk_wmem_queued + total <= sk_sndbuf` (the record size is known before selection), and
keep `-EIO` for genuine failures.

## 10. medium — `/proc/net/tbtun/peers` and `stats` dereference `c->sk` while retire may release it

**Where:** `tbtun/tbt_session.c:337-350` (peers), `tbtun/tbt_carrier.c:1082-1100`
(snapshot called from `tbt_procfs.c:430-445`).

**What breaks.** Both readers take `tbt_lock`; `tbt_carrier_retire()` takes no lock,
releases the socket and sets `c->sk = NULL` (`tbt_carrier.c:713-726`). The readers test
`c->sk` and then dereference `inet_sk(c->sk)` / `tcp_sk(c->sk)`; the socket can be
released between the test and the dereference, giving a UAF on a root read of `peers`
or `stats` concurrent with carrier replacement or tunnel teardown.

**Minimal fix.** Snapshot the needed values into the carrier struct under `pump_lock`
(the same lock that serialises retire in `carriers_stop`), or read them through an RCU
pointer that retire clears before `sock_release()` and then `synchronize_rcu()`s.

---

## Additional findings (11–26)

11. **medium — `TBT_F_RETIRE` is a new wire value without a `TBT_CONV` bump, and
    DESIGN §3.3 does not define it.** `tbt_uapi.h:57-59` adds `TBT_F_RETIRE = 0x24`
    and `tbt_uapi.h:146-149` bounds its body to one byte, but `TBT_CONV` stays
    `0x0002` (`tbt_uapi.h:42`) although the header's own rule says any change to a wire
    value is a protocol change. A peer built from DESIGN rejects the frame as an
    unknown type (`tbt_body_bounds()` returns false), so the far slot is never retired
    and the `join_dup` storm the frame exists to prevent returns. The frame is
    authenticated by the HELLO gate (`tbt_rx_frame` is only reached after
    `saw_hello`), so there is no unauthenticated path. Fix: add the type and its
    body/seq meaning to DESIGN §3.3 and bump `TBT_CONV`.

12. **medium — HELLO profile/generation bit layout disagrees with DESIGN** (fixed).
    `tbtun/tbt_carrier.c:1202-1204` writes `profile | (gen << 2)` (profile bits 1:0, gen
    bits 5:2); `tbtun/tbt_rx.c:460-461` reads `& 3` / `>> 2`. DESIGN §3.1.1 and §3.3 now
    both give the same 5:2 layout; the old §3.3 "high four bits" sentence and the
    "TBT_CONV not bumped" claim were stale (`TBT_CONV` is `0x0003`). The code comment in
    `tbt_carrier.c` now gives the same 5:2 layout; it no longer says "zero flags byte".

13. **medium — PREDICT is inert on the receiver.** **Fixed (removed in `TBT_CONV
    0x0005`):** the placeholder frames, the `predict_*` fields and the procfs counters
    are gone; only the removal comment in `tbt_uapi.h` remains. The original defect:
    `tbtun/tbt_rx.c:438-458` records
    `st->predict_seq`/`predict_token`, and nothing ever reads them to advance
    `rx_next` or to promote a duplicate (DESIGN §4.4/§4.5 case B/C). The only working
    latency mechanism is the sender's immediate retransmit (`tbt_tx_inject_locked`).
    `TBT_MAX_SPEC_BYTES`/`spec_bytes` therefore bound a backlog that has no effect, and
    `predict_used` counts placeholders that do nothing. Fix: implement the token match
    and the bounded advance, or delete the placeholder path and its counters.

14. **medium — the hole gate counts failed writes** (fixed; final host run 357/0).
    `tbtun/tbt_carrier.c:528`, `:611`, `:689` increment `c->data_frames` only after a
    successful send; the comment at `:518-523` names review 14. The old before-send
    increment at `:488-489` is gone.

15. **medium — `fbk_mask` is unused** (**fixed (removed)**). The original
    `tbt_tx_hole_check(struct tbt_session *, u16 fbk_mask)` never read the parameter;
    the gate uses persistent per-carrier `uack`/`recv_cnt` instead (DESIGN §4.3).
    The parameter and its call-site argument are deleted; `grep -rn fbk_mask tbtun/`
    is empty.

16. **medium — the far carrier port is an open relay to `target=` until a nonce is
    pinned.** `tbt_rx_hello()` adopts the nonce from the first HELLO on any carrier
    (`tbt_rx.c:449-465`) and the nonce travels in cleartext in that same HELLO; the
    join then dials the target (`tbt_session_join` → `tbt_carrier_open_target`). An
    attacker who connects first owns the session; `nonce_pinned` is opt-in
    (`tbt_session.c:380-400`). DESIGN's claim that a hostile connection "cannot name a
    stream without the session nonce" does not hold for the first join. Fix: require
    `nonce_pinned` for far tunnels (refuse the first HELLO otherwise), or document the
    trust model in §9.5. **Fixed (2026-11-05):** a pinned nonce is checked, a configured
    `peer=` fixes the source anchor at session creation, and a far tunnel with neither
    still trusts the first connection but warns once. The anchor is checked before the
    nonce (`join_src_reject`), set only by a HELLO that passes the nonce check, and
    asserted again in `tbt_session_join()`.

17. **medium — `/proc/net/tbtun/http` is created but never removed.** `tbt_procfs.c:643`
    creates it; `tbt_procfs_exit()` (`tbt_procfs.c:652-663`) removes every other file
    and then the directory. `remove_proc_entry()` on a non-empty directory warns and
    leaks the child. Fix: one `remove_proc_entry(TBT_HTTP_NAME, tbt_proc_dir)`.

18. **medium — module parameters are decorative** (fixed in code: limits enforced; `netns_optin`
    removed). `max_tunnels` is checked in `tbt_tunnel_add()` under `tbt_lock` before
    `kzalloc` (`-ENOSPC`); `max_carriers` is checked in `tbt_cfg_sane()` before allocation
    (`-EINVAL`). `netns_optin`/`tbt_netns_optin()` were removed; `/proc/net/tbtun` is
    init_net-only (`tbt_procfs.c:165`).

19. **low — `tbt_cfg_read` truncates at the first short read.** `tbt_procfs.c:221`
    returns 0 whenever `*ppos` is non-zero, after advancing `*ppos` by whatever fitted
    in the caller's buffer. Any reader with a buffer smaller than the rendered config
    sees a silently truncated file. Fix: keep the rendered buffer in
    `file->private_data` and serve by offset, or return 0 only when `*ppos >= n`.

20. **low — overlapping reorder frames are never evicted** (fixed; QEMU
    `far.overlap` PASS). `tbt_rx_pull_locked()` (`tbt_rx.c:33-60`) only pulled nodes whose
    `seq == rx_next`; a node that starts inside an already-delivered range stayed in the
    tree forever, inflating `rx_ooo_bytes` until the reorder bound killed the stream. A
    peer sending overlapping records triggers it. Fix: on pull, also drop nodes with
    `seq < rx_next` (and clip partial overlaps). The current `tbt_rx_pull_locked()`
    (`tbt_rx.c:33-80`) evicts a fully stale node and clips a straddling one; no targeted
    test exists.

21. **low — load-bearing RX counters are not atomic.** (fixed) `c->rx_frames++`,
    `sess->rx_data_frames++` (`tbt_rx.c:175-177`) were plain `u64` increments from
    concurrent carriers. `sess->rx_data_frames` and every other session counter with
    more than one writer are now `atomic64_t`, incremented with `atomic64_inc/add`
    and read with `tbt_stat_read()`. `c->rx_frames/rx_bytes` are single-writer per
    carrier (that carrier's rx work) and stay `u64`, with `READ_ONCE()` at the FBK
    reader. `c->data_frames` is written only by `tbt_carrier_xmit()` under
    `pump_lock`, so it had no lost update; it stays `u64` and the hole gate's
    comparison is unchanged. `st->fbk_frames` is a per-stream cadence counter
    whose lost update only delays one FBK kick; it stays a plain `u32` on purpose.
    `review/lint.sh` now pins the atomic set.

22. **low — pinned control frames are dropped on `-EAGAIN`** (**fixed**). The original
    code freed a pinned frame when the carrier returned `-EAGAIN`. Pinned frames are
    now requeued with a bounded retry (`tbt_tx.c:1585`); the stats line
    `tunnel <id> ctl` reports `ctl_lost/ctl_retry/ctl_requeue/ctl_obsolete/ctl_queued`.
    A FIN/RST that crosses a generation is re-pinned; HELLO/OPEN/RETIRE copies for an
    old generation count `ctl_obsolete`; only an exhausted retry bound counts
    `ctl_lost`. QEMU `near.fin_eagain` and `near.rst_eagain` PASS.

23. **low — zero-valued reorder/hole budgets are accepted and make the tunnel
    non-functional** (fixed 2026-11-05; host run 355/0). `tbt_parse.c:536-544` accepted
    `rx_reorder_bytes=0` and `hole_budget_ms=0`; the first out-of-order frame then
    exceeded the reorder limit and the first hole killed the stream. Fix: reject 0 with
    `-EINVAL`. The current parser refuses both (`tbt_parse.c:474-482`), `test_parse` is
    355/0, and `make check` and `make host-test-san` are green.

24. **nit — `tbt_rx_check_hole()` prints the hole age after zeroing the timestamp.**
    **Fixed:** the age is computed before `hole_since_ns` is cleared
    (`tbt_rx.c:923-924`), so `RXHOLEKILL` prints the real age.

25. **nit — `ports 0-0` is rejected.** `tbt_parse.c:821` refuses `lo == 0` although
    `tbt_parse_range()` documents 0-0 as the canonical "no block" spelling (line 246)
    and DESIGN §8.4.1 calls it canonical. Fix: accept 0-0 for `ports` too.

26. **nit — carrier `SO_REUSEADDR` contradicts DESIGN §7.2** (DESIGN fixed; code already on).
    The code sets it deliberately (`tbt_carrier.c:1126-1129`, `tbt_listen.c:130-134`):
    a replaced tunnel line or a reload rebinds the reserved block while old connections
    sit in `TIME_WAIT`. The cost is a possible `connect()` failure, covered by the
    port-block retry. DESIGN §7.2 now says "on"; no code change.

---

## Hostile-input review: /proc, the parser, the HTTP container

These are findings 27-35, from a second pass over `tbt_procfs.c`, `tbt_parse.c` and
`tbt_http.c` only. They are independent of the ranks 1-10 fixes.

27. **high — a zero-length DATA frame stalls delivery permanently and grows `rx_q`
    without bound.** `tbt_hdr_check()` admits `TBT_F_DATA` with `body_len == 0`
    (`tbt_uapi.h:130-133`). `tbt_rx_data()` then takes the `seq == rx_next` branch,
    appends the frame (`tbt_rx.c:182`), adds 0 to `rx_queued` and leaves `rx_next`
    unchanged (`:183-184`). `tbt_drain_work()` calls `tbt_sock_deliver(..., 0)`, which
    returns 0 (`tbt_sock.c:158-159`), and `if (done <= 0) break;` (`:219`) leaves that
    zero-length frame at the head forever. The pause rule is byte-based
    (`rx_queued >= TBT_RX_RING_MAX`, `tbt_rx.c:884`), so it never trips; every later
    frame queues behind the stuck head and kernel memory grows at the peer's frame
    rate. A zero-length frame with `seq > rx_next` also enters the rb-tree with
    `rx_ooo_bytes += 0`, so the byte bound never sees it (only the 500 ms hole timer
    bounds that variant). The emitter never produces such a frame, so the minimal fix
    is to drop `len == 0` in `tbt_rx_data()` before allocating; setting
    `TBT_F_DATA` min to 1 in `tbt_body_bounds()` is equivalent and wire-compatible.
    Combined with finding 16 (first-HELLO nonce adoption), this is remotely reachable
    without a secret: connect first, set the nonce, send 32-byte records.

28. **medium — a response header block over 512 bytes kills the carrier, although the
    parser advertises 8192.** `struct tbt_carrier.http_hdr` is
    `TBT_HTTP_REQ_MAX` = 512 (`tbtun.h:490`, `tbt_http.h:48`) and is used for both
    directions; `tbt_http_resp_parse()` bounds the block at `TBT_HTTP_HDR_MAX` = 8192
    (`tbt_http.h:54`), and `tbt_http_rx_parse()` returns `-EPROTO` once the 512-byte
    buffer is full (`tbt_rx.c:542`, `:560`/`:585`). A middlebox that appends a few
    headers to the far side's 200 response (or any peer with a longer response block)
    kills the carrier; the near side redials and repeats. Fix: size `http_hdr` to
    `TBT_HTTP_HDR_MAX` (8 KiB x 16 carriers = 128 KiB worst case), or bound the
    response parser to `TBT_HTTP_REQ_MAX` and document the limit in §3.3.

29. **medium — the canonical renderer emits lines its own parser rejects (measured
    642 > 512).** `TBT_LINE_MAX` is 512 (`tbt_parse.h:31`) and `tbt_parse_line()`
    rejects `strlen(line) >= TBT_LINE_MAX` (`tbt_parse.c:727`). A legal config with
    every scalar at its maximum, 8 `srcaddrs`, 8 `uid`s and a 15-character `dev`
    renders to **642 bytes** (host probe linking `tbt_parse.c`; the render is one line,
    printed by the probe). `tbt_cfg_write()` clamps each write to 512 bytes and returns
    `-E2BIG` when the line buffer overflows (`tbt_procfs.c:141-171`), so
    `cat config > backup; cat backup > config` fails for a config the parser accepted.
    Fix: raise `TBT_LINE_MAX` to at least the renderer's maximum (or cap list keys so
    the render fits), and add a host test that round-trips a maximal `struct tbt_cfg`
    (the current `test_parse` does not).

30. **medium — `config` silently truncates at 32 KiB.** `tbt_config_render()` breaks
    on the first `tbt_render_cfg()` failure and returns the partial length
    (`tbt_procfs.c:197-213`); `tbt_cfg_read()` serves it as a complete file. With
    `max_tunnels` unenforced and ~642 bytes per maximal line, 52 tunnels exceed the
    buffer; the operator's backup is silently short and `cat backup > config` then
    deletes the missing tunnels. Fix: return `-E2BIG` to the reader, or size the buffer
    from `max_tunnels` and the maximum rendered line.

31. **medium — the release-time flush applies an over-long line that the write path
    rejected.** `tbt_cfg_write()` sets `lb->overflow` and returns `-E2BIG` once the
    line reaches 511 bytes (`tbt_procfs.c:164-171`); `tbt_cfg_release()` calls
    `tbt_config_drain(lb, true)` (`:185`), which applies `lb->buf` without checking
    `lb->overflow` (`:89-125`). A crafted line that is valid at 511 bytes is therefore
    applied although the write reported failure, contradicting §8.4.1's "no partial
    application". Fix: if `lb->overflow`, discard the tail in `tbt_config_drain()`.

32. **low — a blank line written to `/ports` returns `-EINVAL`.** `tbt_parse_ports()`
    returns 0 with `lo = hi = 0` for a blank line, then `tbt_portmap_set(0, 0)` rejects
    it (`tbt_session.c:60-66`), so `echo "" > /proc/net/tbtun/ports` fails. Fix: return
    `len` when the parser produced 0/0, or treat 0/0 as a no-op in `tbt_ports_write()`.

33. **low — `/proc/net/tbtun/http` is a torn read of the last request block.** The pump
    writes `sess->http_last_req` / `http_last_req_len` under `pump_lock`
    (`tbt_carrier.c:536-537`), while `tbt_show_http()` reads them under `tbt_lock`
    only (`tbt_procfs.c:361-368`). A reader can see the new length with old bytes, or a
    half-written block; this is the debug surface meant to prove conformance. Fix: copy
    the block under `pump_lock` into a local buffer before `seq_write()`.

34. **low — a partial procfs init removes entries that were never created** (fixed).
    `tbt_procfs_init()` `goto fail` calls `tbt_procfs_exit()` (`tbt_procfs.c:842-855`),
    which calls `remove_proc_entry()` for every name, including those after the failed
    one; procfs warns on a missing entry. Fixed by removing the subtree: only entries
    that exist are deleted, and a missing subtree returns `-ENOENT` without a WARN.
    `review/lint.sh` verifies that every created name is either removed by name or
    created under the removed directory.

35. **nit — dead code and accepted-but-unused keys on this surface** (fixed).
    The inert keys (`cfg.framing`, `cfg.chunk`, `cfg.max_retry`, `cfg.uid`, `cfg.dev`)
    were removed from the grammar, and the dead declarations `tbt_passive_port()` and
    `tbt_tunnel_flush()` are removed from `tbtun.h`. `tbt_http_skip_request_header()` is
    not dead: `host/test_http.c` exercises it.

36. **high — the retransmit state machine lost its per-copy `snd_una` gate, and the
    retention trim can still free an entry the pump is using.** Four defects in the
    same subsystem, all in `tbt_tx.c`; (b) and (c) are the "freed retain still
    referenced" class.

    **Fixed (05:19, code):** (a) `tbt_retain_wire_ns()` (`tbt_tx.c:630`) restores the
    `snd_una` mark and the RTO origin; (b) the trim is `tbt_tx_retain_trim()`
    (`:305-338`), called only from the pump at `:1268` under `pump_lock`; (c) teardown
    stops the carriers and frees `tx_q` before unlinking streams
    (`tbt_session.c:723-724`, `:738`); (d) the mark includes `c->part_len`
    (`tbt_tx.c:1516-1518`). The text below is the record of what was wrong.

    **(a) The gate is gone.** DESIGN §4.3: a frame is a retransmit candidate "only
    after the carrier's TCP has itself acknowledged its bytes"; each copy stamps
    `first_sent_seq = snd_nxt`, and "the RTO runs from that observation, never from the
    handoff to `tcp_sendmsg_locked()`". `struct tbt_retain` no longer has
    `first_sent_seq[]` or `una_observed_ns[]` (`tbtun.h:190-208`), `tbt_retain_una_ns()`
    is deleted, and `tbt_tx_retx()` gates only on `last_sent_ns`
    (`tbt_tx.c:418-421`); the geometric test's time gate now uses
    `hole->first_sent_ns` (`:849-851`) instead of the snd_una observation. A frame
    still sitting in the carrier's 1 MiB send buffer is
    therefore "overdue" after one RTO and re-sent on another path — the exact bug the
    design measured and removed ("a frame still in our own send queue ... cannot be
    lost, however long it waits"). Under shaping this doubles carrier bytes and inflates
    `holes_declared`; the design's `retx_ppm = 0` claim is no longer upheld.

    **(b) The trim races the pump's batch.** `tbt_tx_submit()` enforces `retain_max`
    (`tbt_tx.c:1615-1643`): it walks only `sess->tx_q`, clears `fr->ret` there
    (`:1630-1632`), and frees the entry (`:1642`). The pump moves frames out of `tx_q`
    into a local `batch` under `tx_lock` (`:1173`, `:1279`) and then uses `fr->ret` after
    dropping the lock (`:1329`, `:1347`, `:1405`). Those frames are no longer in `tx_q`,
    so the trim does not clear them and frees the entry they point at: a UAF write. The
    error paths widen it: they clear `reserve_mask` and re-enqueue the frame
    (`:1326-1341`, `:1344-1360`), and the trim can free the entry between the clear and
    the enqueue, leaving a dangling `fr->ret` in `tx_q`. `tbt_tx_retx()`'s `cand[8]`
    has the same exposure: it holds raw `struct tbt_retain *` after dropping `tx_lock`
    (`:388`, `:464`, `:475-517`) and the trim does not skip `pending`.

    **(c) Teardown frees retains before the pumps stop.** `tbt_session_destroy()`
    unlinks and puts the streams (`tbt_session.c:721-744`) before
    `tbt_carriers_stop()` (`:758`) and `tbt_tx_free_queue()` (`:764`). A carrier
    `w_tx` pump running during the unlink loop processes `sess->tx_q` frames whose
    `fr->ret` points at entries freed by the stream's RCU callback; the pump's `st` is
    NULL (unlinked), so it skips `retain_gc`/`retx` but still uses `fr->ret` in
    selection (`:1222`) and in the write phase (`:1329`, `:1347`, `:1405`).

    **(d) A short write is accounted as a complete send.** `tbt_carrier_xmit()` saves
    the unwritten tail in `c->part` and returns 0 (`tbt_carrier.c:651-675`); the pump
    then sets `path_mask`, `sends++`, `last_sent_ns` (`tbt_tx.c:1404-1418`) while the
    tail is still in `part`. With (a) the RTO starts at that moment, so the frame can be
    retransmitted before its tail reaches the wire.

    **Minimal fix.** (a) restore `u32 first_sent_seq[16]`, `u64 una_observed_ns[16]`,
    set `first_sent_seq[i] = snd_nxt + c->part_len` after a write (the `part_len` term
    covers (d)), and gate the candidate on `snd_una >= first_sent_seq[i]` plus the RTO
    from that observation. (b) free retention entries only under `pump_lock` — move the
    trim into `tbt_tx_pump()` after the batch write (the capture path sets a
    `trim_needed` flag and wakes the pump), so no frame outside `tx_q` can reference the
    entry. (c) stop the pumps and free/clear `sess->tx_q` before dropping the last
    stream reference (take `pump_lock`, `tbt_tx_free_queue(s)`, then unlink/put, then
    release), or move `tbt_carriers_stop()` before the unlink loop and free the queue
    immediately after it.

37. **critical — `c->http_rx` is never allocated, so every HTTP carrier NULL-derefs on
    its first record.** `tbt_carrier_new()` allocates `http_tx`, `http_hdr`, `part` and
    `parser` (`tbt_carrier.c:688-691`), but not `http_rx`; the failure test omits it
    (`:692`) and `kfree(c->http_rx)` at `:698` is a no-op. `tbt_rx_parse()` selects
    `tbt_http_rx_parse()` for every profile except raw (`tbt_rx.c:752`), and that
    function dereferences `c->http_rx->rec` (`:542`) and passes `c->http_rx` to
    `tbt_http_frame_decode()` (`:644`). The default profile is `http-kal`, so the first
    carrier record of the first session oopses. Fix: allocate
    `c->http_rx = kmalloc(sizeof(*c->http_rx), GFP_KERNEL)` in `tbt_carrier_new()`, add
    `!c->http_rx` to the failure test, and free it there; the dead-drain
    `kfree(c->http_rx)` then becomes real.

    **Fixed (05:19, code):** `http_rx` is allocated at `tbt_carrier.c:698` and guarded at
    `:699`; the text above is the record of the failure.

38. **medium — the constructor-failure free in `tbt_replace_work()` leaks the carrier's
    four buffers.** When `tbt_port_alloc()` fails, the code does `if (nc) kfree(nc)`
    (`tbt_carrier.c:904-905`), but `nc` owns `http_tx`, `http_hdr`, `part` and `parser`
    from `tbt_carrier_new()`; only the struct is freed, leaking ~32 KiB per failed
    replacement. The retry loop can run indefinitely, so the leak is unbounded in time.
    `tbt_carriers_stop()` frees the four explicitly (`:1005-1007`), so the two paths
    disagree. Fix: one `tbt_carrier_free(c)` helper used by the constructor failure, the
    replace failure and the dead drain; the `/* ctor-fail */` marker then covers a
    complete destructor.

    **Fixed (05:19, code):** `tbt_carrier_free()` (`tbt_carrier.c:841-853`) is used by the
    constructor failure (`:702`), the replace failure (`:936`) and the dead drain
    (`:1031`).

39. **high — the constructor failure path unlinks a zeroed list node.** In the pre-fix
    tree `tbt_carrier_new()` called `tbt_carrier_free(c)` when a buffer allocation failed
    (`tbt_carrier.c:699-703`), but `INIT_LIST_HEAD(&c->node)` was only at `:712`, after
    the failure test. `tbt_carrier_free()` starts with `list_del_init(&c->node)`
    (`:845`); on the `kzalloc`'d carrier `node.next`/`node.prev` are NULL, so
    `list_del_init()` writes through NULL (or DEBUG_LIST dereferences it) before
    reaching `kfree(c)`. The `/* ctor-fail */` comment at `:697` says the helper's
    unlink "is a no-op", which is false for a zeroed node. Fix: move
    `INIT_LIST_HEAD(&c->node)` immediately after the `kzalloc` (`:687-689`), before the
    buffer allocations, so the helper's unlink is the intended self-unlink no-op.

    **Fixed (05:27, code):** `INIT_LIST_HEAD(&c->node)` is at `tbt_carrier.c:693`, before the
    failure path at `:702`; the text above is the record. The `kfree-marker-proof`
    constructor-order self-test covers the regression.

**Sound on these surfaces** (checked, no finding): the chunk decoder bounds the size
line (1024), the trailer (8192) and the size value (checked against `TBT_CHUNK_MAX`
on every digit, so the u32 accumulator cannot overflow before the check); bare LF is
rejected everywhere; one chunk must equal one record (`rec_need == chunk_size`,
`rec_len == chunk_size`, `rec_len == rec_need`); the request validator requires
exactly the eight emitted headers, a canonical `Host`, a 26-character `[a-z2-7]`
`X-Request-Id` and a blank line; the response validator requires 200, `chunked`,
no `Content-Length`, and a bounded header block; every `copy_from_user`/`copy_to_user`
return is checked; all parser buffers are fixed-size and every loop is bounded by
`len`/`TBT_LINE_MAX`; the line-buffer index stays within `TBT_LINE_MAX`.

---

## Spec drift: DESIGN states an invariant the code does not uphold

These are the silent-drift findings: the prose promises a property, the code does not
implement it, and no test fails. Ordered by DESIGN section.

| DESIGN | Invariant the text stated | Final code behaviour | Status |
|---|---|---|---|
| §2.2 | "`tbt_stats.c` — Per-CPU counters" | No per-CPU block. Multi-writer session counters are `atomic64_t` read through `tbt_stat_read()`; single-writer counters are documented as such (`tbtun.h:483-568`). | RESOLVED (doc + code) |
| §2.5, §9.6 | The replay ring keeps a reference to the `skb`, "instead of copying it" | `skb_copy_bits()` into `r->data` (`tbt_tx.c:2364`), then `memcpy()` into the frame (`:2371`): two copies per byte. DESIGN §2.5/§9.6 now say "copy" and describe `struct tbt_retain` with `u8 data[]`. | RESOLVED (doc, rewritten) |
| §3.1.1, §3.3 | Profile in bits 1:0, generation in bits 5:2, 7:6 zero | TX `profile \| (gen << 2)`, RX `& 3` / `>> 2`; the code comment gives the 5:2 layout (`tbt_carrier.c:1397-1398`). | RESOLVED |
| §3.2 rule 5 | A data-bearing chunk is `[0x800, 0x2020]` (2048..8224) | No `TBT_CHUNK_MIN` exists; a 1-byte DATA frame emits a 39-byte chunk. Small writes are outside the classification floor; DESIGN §3.2 rule 5 now says `[0x800, 0x2020]` is the typical bulk shape, not a floor. | RESOLVED (doc, rewritten) |
| §3.2 (Host) | Port in `Host` only when it is not 80 | Always emitted: `snprintf("%pI4:%u", ...)` (`tbt_carrier.c:603`). DESIGN §3.2 and the request-emission ruling now say the port is always included. | RESOLVED (doc, rewritten) |
| §3.3 | Frame types are 10..15, 20..24 | `TBT_CONV 0x0005`; 0x24 RETIRE documented once; 0x11/0x12 removed and recorded as removed. | RESOLVED |
| §3.4 | `framing=ext` is a working fallback | The key was removed; `profile=raw` is the fallback. DESIGN §3.4 documents the removal. | RESOLVED (doc) |
| §3.5, §8.3 | `chunk=` sets the chunk-data ceiling | The key was removed; the emitter sizes each chunk from the record (`rec_len`), bounded by `TBT_CHUNK_MAX`. | RESOLVED (doc) |
| §3.7 | A segment ends with `0\r\n\r\n`; the next POST starts after it | No chunked terminator is ever emitted (`grep '0\\r\\n\\r\\n' tbtun/*.c` finds none): one unbounded chunked message per carrier. DESIGN §3.2/§3.7 now describe one chunked message for the life of the carrier, no 0-chunk. | RESOLVED (doc, rewritten) |
| §4.5 | On hole death the receiver delivers a transparent hole and advances `next_expected` | Never advances past a hole; `tbt_rx_pull_locked()` evicts stale and clips straddling nodes, but advances only on `seq == rx_next` (`tbt_rx.c:33-90`), otherwise case D resets. This is deliberate (byte exactness); DESIGN §4.4/§4.6 now record it as the choice. | RESOLVED (doc, rewritten) |
| §6.1, §8.3 | `max_retry=0` leaves a dead carrier dead | The key was removed; there is no `max_retry`; retries are the 200 ms connect loop plus the 20-round `join_dup` cap. | RESOLVED (doc) |
| §7.2 | `SO_REUSEADDR` on; one listener on `listen=`; only `srcaddrs[0]` used | `sock_set_reuseaddr()` on (`tbt_carrier.c:1336`, `tbt_listen.c:130`), one listener, `srcaddrs[0]` only. | RESOLVED |
| §8.4, §8.4.1 | `cat config` round-trips; lines are at most 1023; no partial application | `TBT_LINE_MAX=1024`; maximal render 642; exact-size snapshot + offset read (`tbt_procfs.c:292`); the drain refuses the over-long tail. | RESOLVED |
| §8.5, §13 | Who may connect is filtered by `src=`/`dev=`/`uid=` | No inbound filter; those keys do not exist. Only `srcaddrs[0]` selects the near-side source; a far tunnel is identified by nonce/peer. | RESOLVED (doc) |
| §9.5 | A hostile connection "cannot name a stream at all without the session nonce" | The far side adopts the nonce from the first HELLO unless `peer=` or `nonce=` is configured; without either it is TOFU and warns once (`tbt_session.c:1001-1013`). The anchor is checked before the nonce. | RESOLVED (doc) |

---

## Invariants a reviewer can check mechanically

Each invariant is phrased so `review/lint.sh` (or a one-line `grep`/`awk`) can decide
it. The script prints one PASS/FAIL line per invariant and exits non-zero on any FAIL.
On the final tree it reports 16/16 checks and 14/14 self-tests. The "motivated by"
reference names the finding that produced the check, not a claim about the current
code.

1. **No work cancelled under the pump lock can wait on it.** Neither a textual
   `cancel_*work_sync()` between `mutex_lock(&sess->pump_lock)` and its unlock, nor a
   `cancel_*work_sync(&c->w_tx|&c->w_pump)` inside `tbt_carrier_retire()`: both work
   items call `tbt_tx_pump()`. Motivated by finding 6.
2. **No pointer release before its clear.** In `tbt_kill_work()`,
   `WRITE_ONCE(st->sk, NULL)` appears before `sock_release(sock)`.
   Motivated by finding 7.
3. **Insert-after-check is atomic.** `tbt_stream_get()` contains a second
   `hlist_for_each_entry()` (the re-check) after the allocation and before
   `hlist_add_head_rcu()`. Motivated by finding 5.
4. **Every `sk_user_data` reader holds `sk_callback_lock`.** Each of
   `tbt_ready`, `tbt_car_ready`, `tbt_car_write_space`, `tbt_car_state_change`,
   `tbt_pub_ready`, `tbt_pub_write_space`, `tbt_pub_state_change` that reads
   `sk_user_data` also contains `sk_callback_lock`. Motivated by finding 4.
5. **Clear the callback before cancelling its work.** In `tbt_listen_stop()`,
   `sk_user_data = NULL` appears before `cancel_work_sync(&t->w_accept)`.
   Motivated by finding 4.
6. **Every proc entry created is removed.** Each `TBT_*_NAME` passed to
   `proc_create`/`tbt_proc_create` is also passed to `remove_proc_entry`.
   Motivated by finding 17.
7. **Every wire type is bounded and documented.** Each `TBT_F_*` in `tbt_uapi.h` has a
   `case` in `tbt_body_bounds()` and appears in `DESIGN.md`. Motivated by finding 11.
8. **No zero-length DATA frame reaches the queues.** Either `TBT_F_DATA` min is >= 1 in
   `tbt_body_bounds()`, or `tbt_rx_data()` returns early on `!len`. Motivated by
   finding 27.
9. **Every carrier buffer pointer is allocated and guarded.** `http_tx`,
   `http_hdr`, `part`, `parser` and `http_rx` are all `kmalloc`'d in
   `tbt_carrier_new()` and all appear in its failure test. Motivated by findings 28
   and 37.
10. **The renderer emits what the parser accepts.** A maximal legal `struct tbt_cfg`
    renders to fewer than `TBT_LINE_MAX` bytes (compiled probe). Motivated by
    finding 29.
11. **The retention trim is serialized with the pump.** The `retain_max` trim runs in
    `tbt_tx_pump()` (under `pump_lock`), not in `tbt_tx_submit()`, and clears `fr->ret`
    for every queued frame before `kfree()`. Motivated by findings 2 and 36b.
12. **A removed carrier stays reachable for retirement.** A carrier overwritten in
    `sess->car[]` is added to `sess->dead`, and exactly one `kfree(c)` exists in
    `tbt_carrier.c`, inside the dead drain after `tbt_carrier_retire()`; and
    `tbt_carrier_new()` runs `INIT_LIST_HEAD(&c->node)` before any
    `tbt_carrier_free()`. Motivated by findings 1 and 39.
13. **No sleeping allocation on the RX path.** `tbt_rx.c` contains no `GFP_KERNEL`.
14. **The retransmit path has a per-copy `snd_una` mark.** `struct tbt_retain` has
    `first_sent_seq[]`/`una_observed_ns[]`, and `tbt_tx_retx()` gates a candidate on the
    carrier's `snd_una` covering that mark. Motivated by finding 36a.
15. **One chunk is one record.** `tbt_http.c` contains the three equality tests
    `rec_need != chunk_size`, `rec_len != chunk_size`, `rec_len != rec_need`.

---

## Checked and found sound

- `tbt_hdr_check()` is total over every field: `conv`, `flags & ~TBT_FF_MASK`,
  `path_id < 16`, `path_ack_count <= 16`, `path_ack_mask` (vacuous but correct by the
  `BUILD_BUG_ON(TBT_MAX_CARRIERS_LIMIT > 16)`), `body_len <= TBT_PAYLOAD_MAX`,
  `sid_hi == 0`, `reserved2 == 0`, per-type body bounds, and the exact FBK
  length. A record that passes is safe for every consumer.
- Record framing is `32-byte header + body_len` with no prefix; `tbt_rec_scan()` never
  reads past `len` and uses `body_len` only after the header check.
- The chunk decoder is bounded: size is checked against `TBT_CHUNK_MAX` on every digit
  (no u32 overflow before the check), the size/extension line and the trailer section
  are bounded, bare LF is rejected, and one chunk is required to carry exactly one
  record (`rec_need == chunk_size`, `rec_len == chunk_size`), which removes the
  smuggling ambiguity. PAD (size 32) and drip (size 1) are handled as documented.
- FBK entries are 18 bytes and `body_len == path_ack_count * 18` is enforced; the path
  byte's high nibble carries the peer generation and stale generations are ignored.
- Sequence arithmetic is u64 and overflow-guarded at ingest (`seq > U64_MAX - len`); the
  window and retention comparisons are additions of bounded lengths, so wraparound is
  not reachable for realistic stream lengths.
- Duplication never reuses a path: `avoid = path_mask | reserve_mask` is applied at
  selection time and the path is reserved under `tx_lock` before the write, and a
  duplicate with no free path is suppressed (`dup_suppressed`) rather than placed on
  the primary's path. `dup_same_path` is a counted fallback for primaries only.
- Every kernel symbol the module references is exported in the 7.2.9-1-MANJARO
  `Module.symvers` (`tcp_sendmsg_locked` GPL, `tcp_read_sock`, `sock_create_kern`,
  `kernel_bind/connect/listen/accept`, `kernel_sock_shutdown`, `sock_setsockopt`,
  `skb_copy_bits`, `print_hex_dump`, `synchronize_rcu`, `rcu_barrier`, …); the module
  builds and modposts clean, and there is no kprobe/kallsyms use.
- The `/proc` write discipline is respected: the `drop_ppm` gate is before the lock
  (`tbt_session.c:405-409`), no early return occurs inside the `tbt_lock` loop, and the
  config parser applies whole lines with first-error-wins. `mutex_lock(&tbt_lock)` and
  `mutex_unlock(&tbt_lock)` are balanced 11/11 across `tbt_session.c` and
  `tbt_procfs.c` (checked by the retired `tbt-lock-balanced` lint check; the slot is now
  the `snd_una` gate, finding 36a).
- The kill path cannot deadlock: `tbt_stream_kill()` only shuts the socket down (waking
  a blocked delivery; delivery uses `MSG_DONTWAIT`) and queues `w_kill` on
  `system_unbound_wq`; the kill work detaches before releasing. The teardown deadlock
  in finding 6 is fixed in the current tree (the `w_tx`/`w_pump` cancels moved before
  `pump_lock`); finding 36 (a-d) is fixed as well, per the table above.
