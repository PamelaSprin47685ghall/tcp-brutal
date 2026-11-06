# arm64 direct P8 baseline / T5.2 record template

## Run identity

- UTC start/end:
- near: `root@192.168.123.65`, aarch64, kernel 6.1.172, eth0 1 GbE full duplex
- far: `root@45.11.1.62` (HND0), x86_64, kernel 7.0.0-42
- harness: `review/wan-experiment/arm64/{http_server.py,http_client.py,run_baseline_direct.sh}`
- harness sha256:
- route bypass: `ip rule` pref 100 `to 45.11.1.62/32 lookup main`; `ip route get 45.11.1.62` → `dev eth0 src 192.168.123.65`
- loadavg before / after:
- resident noise: `.65` mediamtx + 6×ffmpeg + mihomo; HND0 frps/caddy/xray

## Direct baseline — 8 parallel HTTP/1.1, 8 MiB/request, 10 s × 2

| direction | run | app MiB | app Mbit/s | near eth0 tx MiB | near eth0 rx MiB | far eth0 rx MiB | far eth0 tx MiB | near CPU busy % | far CPU busy % | near RAM used | far RAM used | reqs | partial | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| up | 1 | | | | | | | | | | | | | |
| up | 2 | | | | | | | | | | | | | |
| down | 1 | | | | | | | | | | | | | |
| down | 2 | | | | | | | | | | | | | |

- up evidence dir:
- down evidence dir:
- notes (route, sysctl, retransmits, `ss -ti`):

## Tunnel T5.2 — 256 carriers, `n=4 m=4`, http-kal, crypto=on, rx_reorder=8 MiB

- final head:
- `tbtun.ko` sha256 / srcversion / vermagic:
- near/far config (verbatim):
- port blocks: near 30000-30511, far 31000-31511
- up / down evidence dirs:

| direction | run | app MiB | app Mbit/s | wire MiB | near CPU % | far CPU % | txq_max | una_stall | lost | c9 | parse_err | new WARN/UBSAN |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| up | 1 | | | | | | | | | | | |
| up | 2 | | | | | | | | | | | |
| down | 1 | | | | | | | | | | | |
| down | 2 | | | | | | | | | | | |

## Verdict

- baseline_up = __ Mbit/s; baseline_down = __ Mbit/s
- tunnel_up = __ Mbit/s; tunnel_down = __ Mbit/s
- pass = `tunnel_up > baseline_up` AND `tunnel_down > baseline_down`
- limitations / open questions:
