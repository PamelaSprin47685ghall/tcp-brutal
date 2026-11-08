# tbtun 0.3.1 fix44 再次部署（256 SC-LDPC，quiet，无流量）

- 目的：为 `sclc-verify2` 准备干净的 fix44 两端；quiet 后报告。
- 模块：`/root/tbtun-0.3.1-fix44-20261108T060817Z/`（未重编译）。
- 源码：HEAD `a21f523b469c0f7030f04edd507b86ccaeee0093`。
- 部署时间：2026-11-08T06:44Z 之后。

## 身份

| | far | near |
|---|---|---|
| host | `hnd0.kw92.cyou` `root@45.11.1.62` | `lubancat` `root@192.168.123.82` |
| arch / kernel | x86_64 / 7.0.0-42-generic | aarch64 / 6.1.172 |
| module sha256 | `b9658eb22ea8eb44a451ce0b27172df4b471ae40698de85b6ab11b97abf9d028` | `3f4e682e1918968a25bf2a4f30adb0e124e36165023d991d18cc25f58b636967` |
| srcversion | `4A34EC0ACF15D6418F03E4C` | `8286019A3E526A5D58BEF0B` |
| vermagic | `7.0.0-42-generic SMP preempt mod_unload modversions` | `6.1.172 SMP mod_unload modversions aarch64` |
| version | 0.3.0 | 0.3.0 |

干净重载（旧模块已不在，重新 `insmod` 同一 `.ko`）；默认
`max_tunnels=64 max_carriers=256`。

## 配置

far（下行 1×256 / L8 / r0.5）：

```
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5210 n=128 m=128 fec=on adaptive=off mode=sc-ldpc w=256 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 L=8 nonce=00112233445566778899aabbccddeeff
```

near（上行 8×32 / L16 / r1/4）：

```
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=8 m=24 fec=on adaptive=off mode=sc-ldpc w=128 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=16777216 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 L=16 dev=eth0 nonce=00112233445566778899aabbccddeeff
```

- ports：far `31000-31511`、near `30000-30511`，均回读 `# configured`。
- near ip rule：`100: from all to 45.11.1.62 lookup main`；
  `ip route get 45.11.1.62` → `via 192.168.123.254 dev eth0 src 192.168.123.82`。
- far 5210 source 保留：`python3 /root/sclc-verify/far-raw-serve.py
  --file /root/sclc-verify-1g/accept-1g.bin --bind 127.0.0.1 --port 5210`
  （pid 367475；文件 1 GiB，sha256
  `e926b854bd3b2d47018f21937da56565aafee2b1561dea9e878fd72b05ad7393`）。
  该进程由 sclc-verify2 启动，本次未停止/替换。

## Join 与 quiet

```
far  tunnel 1 c3 n=256 up=256
near tunnel 1 c3 n=256 up=256
```

join 后间隔 6 s 采样两次，两端以下计数器完全不变：

```
data_tx=0  tx_ack=0  fec_short_tx=0
fec_nack_tx=2  fec_nack_rx=1  fec_nack_sparse_probe=9
fec_short_parity_saved=0  fec_nack_pad_skip=0
far  fec_replay_miss=1
near fec_replay_miss=8   (join 探针残量，未增长)
```

`data_tx=0` / `fec_short_tx=0` 证明**未跑任何隧道流量**。

## dmesg

- far：`fec rx shape from peer: n=8 m=24 groups=8 slots=8 block=128 L=16 rate=64`
  （near 上行 L=16）。
- near：`... n=128 m=128 groups=1 slots=8 block=256 L=8 rate=500`。
- 当前加载后窗口 `WARNING:/BUG:/Oops/UBSAN/general protection`：
  far 0，near 0。

## 说明

- 部署后对 5210 做过一次**本机 loopback** `GET /index.html`（用于确认 source
  存活），不经过隧道；隧道 `data_tx` 仍为 0。5210 source 进程保持运行。
- 证据：`far-state.txt`、`near-state.txt`、`far-5210.txt`、`near-iprule.txt`。

## quiet 窗口之后（sclc-verify2 已开始）

quiet 两次采样（间隔 6 s，计数不变）结束后，`sclc-verify2` 开始了首个请求。
报告时只读观察到：

```
far  fec_short_parity_saved=127  fec_nack_pad_skip=0  fec_short_tx=1
near fec_short_parity_saved=23   fec_nack_pad_skip=0  fec_short_tx=1
far  fec_replay_miss=1   near fec_replay_miss=32   (join/探针残量)
```

即验证器的首个短请求已按新路径只发 1 行 parity（下行省 127、上行省 23），
padding NACK 为 0。该流量由 sclc-verify2 发起，本次部署未主动跑流量。
