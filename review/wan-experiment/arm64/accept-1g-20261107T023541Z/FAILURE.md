# 1 GiB acceptance run #3 (fix3) — FAIL at the join gate

- run: 2026-11-07T02:35:41Z .. 02:40:37Z (no transfer attempted)
- near: root@192.168.123.65 (aarch64, 6.1.172), fix3 srcversion D9F5F8F4E4DCBAD99583434,
  sha256 8bb555075ceda980008eba7db857aacc47879172d3f4d3ad654c94aff4daebbb
- far: root@45.11.1.62 (x86_64, 7.0.0-42), fix3 srcversion D639F67ACE6C0AF9565B204,
  sha256 eece0233493c3de280c6a62e6c9d8041530143381ca5170a2878c8f48a16bc4a
  (`/root/tbtun-0.2.0-fix3-20261107T023217Z/tbtun.ko`, TBT_FEC_CONV 0x1003)
- harness exit 4 (setup/join)

## Verdict: FAIL — `far_join FAIL (up=0 after 180s)`

Last log line:
```
02:40:37Z join gate failed; no transfer attempted
```
`near_up=256` for the whole window (with dips to 240..255); `far_up=0` throughout.

## Finding — the far side never accepted a single connection

far, captured at 02:40:37Z:

| evidence | value |
|---|---|
| `tunnels` line | `1 far listening 0.0.0.0:9000 0.0.0.0:0 127.0.0.1:5210 4 4 0` (streams 0) |
| `capture_runs` / `actor_calls` | 0 / 0 |
| `rx_work_runs` / `rx_adopt_events` | 0 / 0 |
| `wire` / `submit_bytes` | 0 / 0 |
| c3 `up` / `lost` / `replaced` | 0 / 0 / 0 |
| c9, c10 | every field 0 |
| dmesg since load (uptime 68880.660677) | no `carrier ACCEPTED`, no `refused an incoming connection`, no WARN |

near, same moment:

| evidence | value |
|---|---|
| c3 `up` / `lost` / `replaced` | 256 / 13312 / 13312 |
| `wire` | 6,433,674 B (13,312 generations x ~483 B of HELLO/profile bytes) |
| `rx_work_runs` / `rx_reads` / `rx_read_zero` | 26196 / 26159 / 26159 |
| `connect_fail` / `join_bad` / `join_nonce_reject` | 0 / 0 / 0 |
| dmesg | `carrier N gen 128xx lost: sock=1 connected=1 hello_sent=1 up=1` repeating, `tbt_carrier_lost: 246 callbacks suppressed` |

So near dials, completes the handshake, sends HELLO, and then every read
returns 0 (peer close/reset); it replaces the carrier ~52 times per slot in
180 s. The far side counts nothing at all: `tbt_carrier_adopt()` was never
called, i.e. `kernel_accept()` never returned a socket to `tbt_accept_one()`.

## Root-cause candidates (ranked)

1. **The far listener's accept loop never runs.** `tbt_listen_start()` leaves
   `t->accepting = 0`; only `tbt_tunnel_add()` sets it to 1 and queues
   `w_accept`, and `tbt_ready()` (sk_data_ready) returns early while
   `accepting == 0`. A listener that is up but whose accept work never runs
   matches every observation: LISTEN on 9000, no ACCEPTED, no refused,
   `streams 0`, all session counters 0.
2. **The config rewrite replaced a live tunnel.** The far side already had
   `tunnel 1` before this run; the run wrote the same line again, which runs
   `list_del_rcu(old)` + `tbt_listen_stop(old)` (sets `accepting=0`,
   `cancel_work_sync`) and then builds a new listener on the same port. If the
   new `accepting = 1` / `schedule_work` did not take effect, or the old socket
   still owns the accept queue, no accept work exists for the live socket.
3. **A fix3 regression in the far build.** `3994eea` only changes
   `tbt_car_schedule_rx`/`w_rx`; with zero carriers on far that code never ran,
   so it cannot explain the missing accepts directly. It is not ruled out for
   the adopt path.
4. Path/backlog is not the cause: `tcp_abort_on_overflow=0`,
   `somaxconn=4096`, listener backlog 128, and near's `connect_fail=0`.

## What was not done

- No transfer was attempted; no `size_download`, no sha256, no c10 data for a
  transfer. The acceptance cannot be judged beyond the join gate.
- DKMS was not run (the task gates it on a 1 GiB PASS). `.65` still has no
  `dkms` package.
- No rerun.

## State after the failure

- near: tunnel deleted, `tbtun` rmmod, `/proc/net/tbtun` gone, 9001 free,
  feeder pid=23184 still holds `/dev/watchdog0`, boot_id unchanged.
- far: tunnel deleted, 9000 free; the module stays loaded (it was loaded
  before the run); `/root/accept-1g` and the 5210 httpd remain up for the next
  attempt.
