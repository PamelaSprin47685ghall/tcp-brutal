# STOP — .65 unreachable during c8 (8-carrier) level, 2026-11-06 ~22:44–22:50Z

Single owner: t52-smoke2. Head 2a8d509 (product fcd7232 + FBK/requeue fixes). Far HND0 deployed
from 2a8d509 (`deploy_hnd0.sh 2a8d509 -`, ko srcversion 846D47A6 / sha 49d632d3). Near staged
tree /root/tbtun-t52-arm64-2a8d509f37a3 (ko srcversion 279034E1 / sha 5ea1f123).
Level c8 = 8 carriers, n=4 m=4 fec=on grouped w=8 crypto=on profile=http-kal, 1 x 1 MiB / 5 s.

## Sequence

1. 22:39:11Z far deployed (module max_carriers=256 max_tunnels=1 debug=0, ports 31000-31511,
   echo 5202). Far verified clean and listening.
2. 22:40:03Z first ladder attempt: blocked on `/tmp/tbtun-hnd0.lock` held by a stale
   `deploy_hnd0.sh` holder (bash 96107 + flock 96573; deploy's lock_release killed the local ssh
   but the remote holder survived). Ladder failed after ~200 s; no level ran.
3. Stale holders killed on both hosts; 22:44:01Z second ladder attempt:
   - locks acquired, load OK, gw ping from .65 0% loss
   - far config rewritten to carriers=8, echo restarted
   - near ip rule added, route verified via eth0, sysctl raised, insmod OK, ports/config written
   - **near dialed 8 carriers; far ACCEPTED all 8 at 55128.29 (uptime), all `awaiting HELLO`;
     far up=0, rx_read_bytes=0, streams_total=8, rx_adopt_events=8 — HELLO never completed**
   - next step (tbt_burst build) returned rc!=0 with an EMPTY build log
4. 22:45–22:50Z: every ssh to 192.168.123.65 hangs; one probe timed out after 300 s.
   - local ping .65: 100% loss, `ip neigh`: `192.168.123.65 dev wlp4s0 FAILED`
   - TCP connect to .65:22: `No route to host`
   - router 192.168.123.254: 0% loss, 2 ms — LAN/router healthy
   - HND0: healthy, load 0.07, production services active; ping to 171.120.34.226 100% loss
     (ICMP may be filtered; not decisive)

## Far-side evidence captured before teardown (far-state.txt)

- config: `n=4 m=4 fec=on mode=grouped w=8 carriers=8 crypto=on ... portblock=31000-31015 nonce=…`
- dmesg: 8x `tunnel 1 carrier ACCEPTED lport=9000 rport=…, awaiting HELLO` at 55128.29
  (rports 30000,30001,30002,30004,30006,24287,24289,24291 — note 3 of 8 used ephemeral ports
  outside the near portblock 30000-30015)
- stats: up=0, lost=0 replaced=0 join_dup=0 join_bad=0, rx_read_bytes=0, rx_ready_calls=0,
  tx=0 wire=0, streams_total=8, streams_active=0

## Reading

.65 died (or lost its NIC/link) during the c8 join, right after insmod + config with
carriers=8 / crypto=on / http-kal. Two candidates, indistinguishable without console access:
(a) aarch64 kernel panic/hang in the 2a8d509 module on the 8-carrier FEC/crypto path;
(b) eth0/NIC/link failure on .65 (it had a documented 7 s Link Down/Up flap on 2026-11-06
    at uptime 7340/7348).
The far side never saw HELLO, so the near side stopped before the tunnel handshake completed.
This is a .65 anomaly: STOPPED per instruction; no retry.

## Cleanup

- HND0: echo stopped, module removed, verified no tbtun / no /proc/net/tbtun / no 9000-5202
  listeners / no tbt_echo; stale lock holders and stray `sleep 14400` killed; production
  services frps/caddy/xray active.
- .65: UNREACHABLE — cannot rmmod/clean the near module or its lock remotely. Its state
  (module loaded? ip rule present? sysctl raised?) is unknown until it is reachable again.
- No 256 run, no 8–16 MiB runs, no push.

## Evidence

- review/wan-experiment/arm64/stop-65-down-20261106T2250Z/
  far-state.txt, local-ping-65.txt, local-ping-gw.txt, far-ping-65-public.txt
- review/wan-experiment/arm64/ladder-20261106T224401Z/c8/
  config-near.txt, config-far.txt, route.txt, burst-build.log (empty)
- review/wan-experiment/arm64/deploy-hnd0-20261106T223911Z/ (far deploy evidence)
