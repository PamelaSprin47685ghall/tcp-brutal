# 1 carrier / 37B ×2 smoke — fbcd820, debug=2 (2026-11-06, owner t52-smoke2)

Scope: near 192.168.123.65 (aarch64 lubancat, ko /root/tbtun-t52-arm64-fbcd820c6f49/tbtun.ko,
srcversion D20BF3BFF0C9144EC4FBB72) ↔ far 45.11.1.62 HND0 (x86_64, ko /home/tbtun-t52-fbcd820/tbtun.ko,
srcversion CEC893BF68EBB155A61158E). Both insmod `max_carriers=16 debug=2`.
Config (identical tokens, no nonce — matches t52-smoke reference run):
near `listen=0.0.0.0:9000 peer=45.11.1.62:40000 n=1 m=1 fec=off mode=rlnc w=2 carriers=2 crypto=off
crypto_key=- dup=0 profile=raw keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000
hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=0 portblock=0-0`
far same with `listen=0.0.0.0:40000 target=127.0.0.1:5202`.
Near route: `ip rule add to 45.11.1.62/32 lookup main pref 100` (direct eth0; removed at cleanup).
tbt_echo 5202 on HND0 via systemd unit tbt-echo-5202 (pid 65827).

## Collision note (resolved)

t52-smoke3 concurrently deployed the same run and overwrote the far config with
`nonce=00112233445566778899aabbccddeeff` while near had none → far `join_bad=15`, up=0
(HELLO "session nonce" reject storm, far dmesg 18735–18745). Main confirmed t52-smoke2 is the
sole owner; configs restored to consistent no-nonce; far join_bad reset to 0. The nonce-conflict
window is NOT a product result.

## Result: BOTH pings FAILED (0/37 in 5 s each) — worse than t52-smoke (ping#1 had passed)

- join: up=2 on both ends (n=1 m=1 → carriers=2; both carriers up at ping time).
- ping#1 (20:40:40–45 +08): near→far OK — far rx_delivered 0→37, far submit_bytes 0→37
  (echo reply entered far tunnel TX). far→near: near rx_delivered stayed 0.
- ping#2 (20:40:5x +08): far rx_delivered 37→74, far submit_bytes 37→74 (second reply also
  handed to TX). near tx_ack stuck at 37 (no new far ACK). near rx_delivered stayed 0.
- tbt_echo 5202 alive throughout (systemd unit, LISTEN 0.0.0.0:5202).

## Counters (pre1 → mid1 = ping#1 window; mid1 → post2 = ping#2 window)

NEAR (.65):
- capture_runs 0→1→2; submit_bytes 0→37→74; tx_submit_calls 0→1→2; tx 0→74→148
- rx_read_bytes 220→518→762; **rx_delivered 0→0→0**; rx_proto=0; rx_errs=0; rx_read_zero 6→6→97
- c3: up=2; **lost 2→2→62; replaced 2→2→62; spare_activated 0→0→16; connect_fail 0→0→9;
  connect_retry 2→2→26; stall_declared 0→0→2**; tx_ack 0→37→37 (stuck); rx_ack=0;
  data_tx 0→2→4; retx_frames 0→1→2
- peers post2: carrier0 up tx_frames=1 tx_bytes=55; carrier1 spare tx_frames=1 tx_bytes=55

FAR (HND0):
- capture_runs 0→2→4; submit_bytes 0→37→74; tx_submit_calls 0→1→2; tx 0→37→74; wire 110→408→816
- rx_read_bytes 110→444→888; **rx_delivered 0→37→74**; rx_proto=0; rx_errs=0
- c3: up=2; lost 0→0→2; replaced=0; spare_activated 0→0→1; join_dup=0 join_bad=0 (after nonce fix);
  stall_declared 0→0→2; fbk_tx 0→1→2; fbk_rx 0→1→2; data_tx 0→1→2; tx_ack=0; rx_ack 0→37→37
- streams_total 2→2→4; streams_active=0 at snapshot
- peers post2: carrier0 up tx_frames=1 tx_bytes=55; carrier1 spare tx_frames=1 tx_bytes=55
- dmesg: `hole gatefail seq=0 mask=0x1 ack=0` at uptime 18922

## Root-cause reading

1. **Environmental trigger — .65 eth0 link flap during the run.** dmesg (dmesg-links-near.txt):
   `7330.80 Link is Up - 1Gbps/Full` → `7340.93 Link is Down` → `7348.03 Link is Up` — a ~7 s
   outage squarely inside ping#1's window (pre1 20:40:35, ping#1 ~20:40:40). Carrier connects
   failed `-113` (EHOSTUNREACH) at 7344/7346/7348, then a redial storm: 60× `carrier N DIALED`
   between uptime 7346–7463 (~2 min), lport cycling 40000–40003.
2. **Product amplification — carrier RETIRE/replacement churn.** near lost/replaced went 2→62
   and spare_activated 0→16 across the two ping windows; far stayed stable (lost=2). Every
   far→near DATA frame was written onto a carrier generation that near then retired/replaced,
   so near read carrier bytes (rx_read_bytes grew) but delivered 0 app bytes (rx_delivered=0,
   rx_proto=0, rx_errs=0 — no parse errors; frames belong to stale generations / never complete).
3. near tx_ack stuck at 37 after ping#1: far never ACKed ping#2's data at carrier layer even
   though far's app side received both requests (rx_delivered=74) — ACK path (far→near) dead
   with the data path.
4. This run cannot isolate a ping#2-specific race: BOTH pings failed because the link flap +
   replacement storm spanned both. It IS consistent with "carrier replacement races stream
   data" (the task's hypothesis) but the trigger here is the .65 eth0 flap, an environmental
   anomaly — flagged per the stop rule; no re-run attempted.

## Cleanup / final state (verified)

- HND0: flush + rmmod tbtun, tbt-echo-5202 stopped/reset-failed; no module/proc/5202/40000
  listeners/echo process.
- .65: flush + rmmod tbtun, ip rule removed, /tmp/tbt_ping removed; no module/proc/9000
  listeners; route to 45.11.1.62 back to Meta table 2022 (original state).
- Evidence: pre1/mid1/post2 {near,far}.txt (config+peers+stats), dmesg-links-near.txt,
  dmesg-tbtun-far.txt, this SUMMARY.md. No 8/16/256, no push.
