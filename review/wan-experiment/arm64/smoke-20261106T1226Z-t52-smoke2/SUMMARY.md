# 1 carrier / 37B smoke — fbcd820 (2026-11-06, t52-smoke run, counters relayed by t52-smoke2)

Scope: near 192.168.123.65 (aarch64, ko sha256 cb08653b…/026d42cd…, srcversion D20BF3BFF0C9144EC4FBB72)
far 45.11.1.62 HND0 (x86_64, ko sha256 67acc024…, srcversion CEC893BF68EBB155A61158E), tbt_echo 5202.
Config both ends: `tunnel 1 role=… listen=… peer/target=… n=1 m=1 fec=off mode=rlnc w=2 carriers=2
crypto=off crypto_key=- dup=0 profile=raw keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0
max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0
carrier_stall_ms=0 portblock=0-0` (near listen 0.0.0.0:9000 peer 45.11.1.62:40000; far listen
0.0.0.0:40000 target 127.0.0.1:5202). n=1 m=1 → carriers=2; observed up=1 (spare inactive).

## Result: NOT clean — ping#1 PASS, ping#2 FAIL (0/37 in 5 s)

- ping#1: 37 B byte-exact both directions, rc=0.
- ping#2: 0/37 after 5 s. far→near return path lost on the second connection.
- tbt_echo 5202 alive throughout: pid 63265, LISTEN 0.0.0.0:5202 before and after ping#2
  (killed during teardown).

## ping#2 5 s window counters (pre → post)

NEAR (.65):
- capture_runs 4→5, actor_calls 2→3, submit_bytes 74→111, tx 148→222, wire 16166→16500
- rx_read_bytes 12711→13307, rx_delivered **37→37 (stuck)**, rx_proto 2→2, rx_errs 1→1 (-71),
  tx_submit_calls 2→3
- peers post: carrier0 up tx_frames=9 tx_bytes=459 inflight=0; carrier1 spare tx_frames=7 tx_bytes=319
- c3: up=2 lost=18 replaced=19 spare_activated=2 join_dup=0; c6 holes_unrecoverable=2

FAR (HND0):
- capture_runs 5→7, actor_calls 2→3, submit_bytes 74→111, tx 111→148, wire 15525→15823
- rx_read_bytes 15341→15675, rx_delivered 74→111, rx_proto 0→0, rx_errs 13→13 (-114),
  tx_submit_calls 2→3
- peers post: carrier0 up tx_frames=9 tx_bytes=387 **inflight=74 rx_lag_ppm=1000000**;
  carrier1 spare tx_frames=7 tx_bytes=319
- c3: up=2 lost=3 replaced=0 spare_activated=1 **join_dup=13**; streams_total=19 rx_adopt_events=19
- dmesg: repeated `tunnel 1 carrier ACCEPTED lport=40000 rport=… awaiting HELLO` (reconnect storm)

## Reading (t52-debug recipe)

far submit_bytes/rx_delivered grew by 37 (echo reply handed to tunnel TX); far carrier0 still
holds inflight=74 unacked; near rx_read_bytes grew only by protocol bytes, rx_delivered stuck at 37.
=> far→near DATA left HND0 but never arrived/was never delivered at .65. Failure sits in the
far→near carrier/stream generation on the second connection: near c3 lost=18/replaced=19 and far
join_dup=13 + ACCEPTED-awaiting-HELLO storm indicate carrier slot churn racing the new stream's
HELLO/OPEN (stale generation), not an echo-target problem.

## Cleanup / final state (verified by t52-smoke2 at ~12:28Z)

Both hosts clean: no tbtun module, no /proc/net/tbtun, no 9000/5202/40000 listeners, no tbt_echo,
no locks, no ip rule on .65, near sysctl rmem/wmem=212992 (production, untouched).
Evidence: near-final.txt / far-final.txt (full stats+peers+tunnels+dmesg) in this directory;
copies left at /tmp/t52-smoke/ on both hosts.
