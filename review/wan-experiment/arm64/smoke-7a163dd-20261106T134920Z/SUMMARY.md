# 1 carrier / 37B ×2 bounded run — 7a163dd (product fcd7232), single owner t52-smoke2

Source: head `7a163dd` (product `fcd7232` = merge of 30930fc carrier-generation supersede +
42c1522 DATA requeue; docs/evidence on top do not change the module). Same source both ends.

- near .65 aarch64: `/root/tbtun-t52-arm64-fcd7232dbe7c/tbtun.ko`
  srcversion `88E99F0159454493E702A85`, sha256 `ae2185f0e1a121ac00d06cf32e34d659fcecc58b10860bc08e2e8d8e72e9aba3`,
  vermagic 6.1.172 SMP mod_unload modversions aarch64, insmod `max_carriers=16 debug=2`
- far HND0 x86_64: `/home/tbtun-t52-arm64/tbtun.ko`
  srcversion `17E7F5B5757871F760EA292`, sha256 `9f97a003a40e091aa4f4088f6e245b8ffe3810a3505a45ca095818c0188d9a59`,
  vermagic 7.0.0-42-generic, insmod `max_carriers=16 debug=2`

Config both ends (identical tokens, no nonce — same shape as smoke-debug2 / integrator-final final3):
near `listen=0.0.0.0:9000 peer=45.11.1.62:40000 n=1 m=1 fec=off mode=rlnc w=2 carriers=2 crypto=off
crypto_key=- dup=0 profile=raw keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000
hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=0 portblock=0-0`;
far same with `listen=0.0.0.0:40000 target=127.0.0.1:5202`.
Near route: `ip rule add to 45.11.1.62/32 lookup main pref 100` (direct eth0; removed at cleanup).
tbt_echo 5202 on HND0 via systemd-run unit tbt-echo-5202.

## Listener pre-check (task requirement) — PASS

- `ss -ltnp`: `LISTEN 0 128 0.0.0.0:9000`
- local connect `127.0.0.1:9000` → CONNECT_OK
- LAN connect `192.168.123.65:9000` → LAN_CONNECT_OK
- No `Connection refused` on either ping (TCP connect succeeded; 0/37 = no data back).

## Join — PASS

- First join-poll: up=2 both ends (n=1 m=1 → carriers=2; both up).
- join_dup=0 join_bad=0 both ends; connect_retry=1 near / 0 far.
- up stayed 2 throughout both ping windows (never dropped to 0 → not void).

## Result: FAIL — both tbt_ping 37B returned 0/37 in 5 s

- ping#1: near submit_bytes 0→37, far rx_delivered 0→37 (request reached echo),
  far submit_bytes 0→37 (echo reply entered far tunnel TX), near rx_delivered stayed 0.
- ping#2: near submit_bytes 37→74 (second request submitted), but far rx_delivered
  STUCK at 37 — ping#2's request never reached far's app/echo. far submit_bytes
  stuck at 37. near rx_delivered stayed 0.

## Counters (joined → post1 = ping#1 window; post1 → post2 = ping#2 window)

NEAR (.65):
- capture_runs 4→6→7; submit_bytes 0→37→74; tx_submit_calls 0→1→2
- rx_read_bytes 302→1008156→1061392 (**~1 MB read from carriers — FBK/protocol flood**);
  rx_delivered **0→0→0**; rx_proto=0 rx_errs=0; rx_read_zero 2→17→25
- c3: up=2; **lost=0 replaced=0 spare_activated=0 connect_fail=0 join_dup=0 join_bad=0
  stall_declared=0 connect_retry=1** — NO carrier churn (fixes hold)
- tx_ack 0→37→37 (stuck after ping#1); rx_ack=0; data_tx 0→2→4; retx_frames 0→1→2
- peers post2: carrier0 up tx_frames=1 tx_bytes=55; carrier1 spare tx_frames=1 tx_bytes=55

FAR (HND0):
- capture_runs 3→5→5; submit_bytes 0→37→37 (**only ONE echo reply**); rx_delivered 0→37→37
  (**only ONE ping request reached echo**); tx_submit_calls 0→1→1; data_tx 0→1→1
- rx_read_bytes 87643→964953→1010607 (~1 MB); rx_proto=0 rx_errs=0
- c3: up=2; lost=0 replaced=0 join_dup=0 join_bad=0 stall_declared=0; connect_retry=0
- **fbk_tx 0→15119→15780; fbk_rx 1283→14180→14848 — massive FBK feedback storm**
- streams_total=2; streams_active=0 at snapshot

## dmesg (debug=2)

NEAR:
- `TXSTALLKILL sid=590943879170 ack=0 txseq=37` @11496.89 (ping#1 stream, ~8 s after submit)
- `TXSTALLKILL sid=590943879171 ack=0 txseq=37` @11665.42 (ping#2 stream)
- `hole depthfail seq=0 g=1 mask=0xcc75` ×9 @11123–11124
- No DIALED storm, no connect failures during ping windows; no new link flap
  (last eth0 Link Down/Up remains uptime 7340/7348, pre-run).

FAR:
- `hole gatefail seq=0 mask=0x1 ack=0` @23108.39
- carrier ACCEPTED lport=40000 rport=23843/40000 awaiting HELLO @23081/23083
- No join_dup/join_bad; no nonce rejections (no-nonce config).

## Reading

1. **Listener/join healthy**: 9000 LISTEN+accept verified, up=2 stable, join_dup=0 — the
   void conditions (Connection refused / post empty / join-poll up=0) did NOT occur.
2. **Carrier churn is gone** (lost/replaced=0 both ends, vs 62 in smoke-debug2) — the
   30930fc/42c1522 fixes hold under this shape, consistent with integrator-final final3.
3. **Residual failure is the far→near return path + a new near→far loss on ping#2**:
   - ping#1: request reached far, echo replied, reply never ACKed/delivered at near
     (TXSTALLKILL ack=0) — same far→near starvation as final3.
   - ping#2: request never reached far's app (far rx_delivered stuck 37) — near→far
     ALSO broke this run (worse than final3 where far got 74/111).
4. **New symptom: FBK feedback storm** — fbk_tx=15780 / fbk_rx=14848 on far, near
   rx_read_bytes ~1 MB, hole depthfail/gatefail on both ends. The DATA requeue fix
   (42c1522) may be requeue-looping: requeued DATA → hole → FBK → requeue, flooding
   carriers with feedback and starving real DATA/ACK. This is a PRODUCT bug candidate,
   not environmental (no link flap this run).
5. streams_total=2 on far with rx_delivered=37 — only one stream ever delivered; the
   second ping's stream likely never OPENed/completed on far.

## Cleanup / final state (verified, cleanup.txt)

- .65: rmmod tbtun, ip rule removed, no /proc/net/tbtun, no 9000 listener.
- HND0: rmmod tbtun, tbt-echo-5202 stopped/reset-failed, no /proc/net/tbtun,
  no 40000/5202 listeners, no tbt_echo process.
- Evidence: join-poll.txt, joined/post{1,2}-{near,far}.txt, ping{1,2}.txt, ko.txt,
  cleanup.txt, this SUMMARY.md. No 8/16/256 runs, no push.
