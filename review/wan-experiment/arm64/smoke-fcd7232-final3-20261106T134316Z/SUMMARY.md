# 1 carrier / 37B ×2 bounded run — fcd7232 (2026-11-06, owner integrator-final)

Source: master `fcd7232` (merge of w-t52-debug: 30930fc carrier-generation
supersede + 42c1522 DATA requeue fix; docs-only commits on top do not change
the module). Same source both ends.

- near .65 aarch64: `/root/tbtun-t52-arm64-fcd7232dbe7c/tbtun.ko`
  srcversion `88E99F0159454493E702A85`, sha256 `ae2185f0e1a121ac00d06cf32e34d659fcecc58b10860bc08e2e8d8e72e9aba3`,
  vermagic 6.1.172 SMP mod_unload modversions aarch64, insmod `max_carriers=16 debug=2`
- far HND0 x86_64: `/home/tbtun-t52-arm64/tbtun.ko`
  srcversion `17E7F5B5757871F760EA292`, sha256 `9f97a003a40e091aa4f4088f6e245b8ffe3810a3505a45ca095818c0188d9a59`,
  vermagic 7.0.0-42-generic, insmod `max_carriers=16 debug=2`

Config both ends (identical tokens, no nonce — same shape as smoke-debug2):
near `listen=0.0.0.0:9000 peer=45.11.1.62:40000 n=1 m=1 fec=off mode=rlnc w=2
carriers=2 crypto=off crypto_key=- dup=0 profile=raw keepalive=5000
drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0
rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=0
portblock=0-0`; far same with `listen=0.0.0.0:40000 target=127.0.0.1:5202`.
Near route: `ip rule add to 45.11.1.62/32 lookup main pref 100` (removed at cleanup).
tbt_echo 5202 on HND0 via systemd-run unit tbt-echo-5202.

Run window ~21:43:30–21:44:35 +08 (near uptime ~11090–11143). No link flap:
.last eth0 Link Down/Up on .65 remains uptime 7340/7348 (pre-run); dmesg checked
after each ping — no new link events. No concurrent smoke/build/deploy processes
(two stray smoke_37b.sh from another session were killed before this run;
goal-integrator confirmed stand-down).

## Result: FAIL — both tbt_ping 37B returned 0/37 in 5 s

- join: up=2 both ends at first poll (21:43:49), connect_retry settled to 0,
  join_dup=0 join_bad=0 on both ends.
- ping#1, ping#2: TCP connect to 127.0.0.1:9000 succeeded (no refusal),
  0/37 bytes back each.

## Counters (joined → post1 = ping#1 window; post1 → post2 = ping#2 window)

NEAR (.65):
- submit_bytes 0→37→148; tx_submit_calls 0→1→4; capture_runs 0→1→5
- rx_read_bytes 110→110→1005; rx_delivered 0→0→37; rx_proto=0 rx_errs=0
- c3: up=2 throughout; **lost=0 replaced=0 spare_activated=0 connect_fail=0
  join_dup=0 join_bad=0** — NO carrier churn this run (vs lost/replaced=62
  in smoke-debug2). fbk_tx 0→1→86; retx_frames→7; data_tx→11; tx_ack=37 rx_ack=37
- streams_total 0→1→4

FAR (HND0):
- submit_bytes 0→74→111; rx_delivered 0→74→111 (both ping requests + copies
  reached the echo path and replies were handed to tunnel TX)
- rx_read_bytes 110→678→20520; rx_proto=0
- c3: up=2; lost=0 replaced=0 join_dup=0 join_bad=0; fbk_tx 0→2→231

## dmesg (debug=2)

NEAR: `TXSTALLKILL sid=…512/513/515 ack=0 txseq=37` ×3 (~8 s after submit),
then repeated `hole depthfail seq=0 g=1 mask=0xcc75`. No DIALED storm, no
connect failures during the ping window.
FAR: `hole gatefail seq=0 mask=0x1 ack=0`, `hole depthfail seq=0 g=1 mask=0x1`.

## Reading

The replacement storm is gone: lost/replaced stayed 0 on both ends with
debug=2 across join + two pings — the 30930fc/42c1522 fixes hold under this
shape. The residual failure is different: near→far requests arrive (far
rx_delivered=111), far echo replies enter far TX (submit_bytes=111), but the
far→near return path stalls — near TXSTALLKILL ack=0 (far never ACKed near's
DATA at carrier layer), near rx_delivered only 37, and hole depthfail/gatefail
on both ends. This matches the smoke-debug2 far→near starvation signature but
WITHOUT the environmental trigger (no eth0 flap, no carrier churn): the
far→near carrier/stream data path still loses replies on this build.

## Cleanup / final state (verified)

- .65: rmmod tbtun, ip rule removed, no /proc/net/tbtun, no listeners, no tbt_ping.
- HND0: rmmod tbtun, tbt-echo-5202 stopped/reset-failed, no /proc/net/tbtun,
  no 40000/5202 listeners.
- Evidence: pre/joined/post{1,2} {near,far}.txt, join-poll.txt, ping{1,2}.txt,
  link-after-ping{1,2}.txt, ko.txt, cleanup.txt, this SUMMARY.md.
- No 8/16/256 runs, no push.

Note: earlier attempts this session (smoke-fcd7232-20261106T1323*/132610Z-retry/
133023Z-run3/133716Z-run5 and smoke-9ba6a09-*) are INVALID — killed mid-run by
concurrent smoke_37b.sh processes from another session doing rmmod/insmod on
the same hosts, or by a script edit during execution. Only this directory is
the clean bounded result.
