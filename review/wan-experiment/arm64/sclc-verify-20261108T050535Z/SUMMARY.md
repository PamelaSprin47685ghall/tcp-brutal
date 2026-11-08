# SC-LDPC 0.3.0 fix43 首连 + 稳态（PASS）

- 模块: tbtun 0.2.3, HEAD `830c7f7`
  - near srcversion `B66D7B86A162F744837BFF3`
  - far  srcversion `2296207711D4F7FC38D809F`
- near `root@192.168.123.82` / far `root@45.11.1.62`
- 运行: 2026-11-08T05:05:35Z – 05:09:19Z（首连 + 稳态；进程在稳态检查后、
  1 GiB 前被提前终止）
- 1 GiB 续跑与完整判定: `../sclc-verify-20261108T050950Z/SUMMARY.md`

## 判定

**首连 PASS，稳态 PASS。**

- 首连：HTTP 200；53,477,376 B；TTFB 1.262 s；far decode +4；
  replay high 4,919,280 < 5 MiB；backpressure 0；hol_wait 19,031（只记录）；
  near ack_tx +43、far ack_rx +44；无 reset/holes；窗口无 tbtun hog。
- 稳态：app 155,500,544 B/60 s；wire far 374,431,233 / near 312,192,380；
  outq_drop 0；nack_backpressure 0；near ack_tx +143、far ack_rx +142；
  near credit 0、far credit +1451（ACK 在推进）；replay high 4,919,280；
  hol_wait near 0 / far 1,834,304（加权，info）；无 reset/holes；up=256。

## 证据

`run.log`、`timeline.txt`、`first-connect.txt`、`first-size.txt`、
`first-exit-ts.txt`、`first-sampler-near.log`、`first-sampler-far.log`、
`first-during-near.txt`、`first-during-far.txt`、`first-analysis.txt/json`、
`steady-a-*-stats.txt`、`steady-b-*-stats.txt`、`steady-sampler-*.log`、
`near-kernel-first.txt`、`far-kernel-first.txt`、`console.log`。

1 GiB 的 sha256、计数器、CPU 与 cleanup 见
`../sclc-verify-20261108T050950Z/`。
