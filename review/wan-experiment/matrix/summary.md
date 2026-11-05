# tbtun WAN matrix summary

Date: 2026-11-06. Hosts: JYSZ 7.2.9-1-MANJARO (near), HND0 7.0.0-42-generic (far, 45.11.1.62). tbtun.ko loaded on both; far target 127.0.0.1:5201 (iperf3 server, PID 699110).

Method: every run does `del 1` on both ends, writes far, writes near, waits 3 s, reads back the canonical config, then runs `iperf3 -c 127.0.0.1 -p 9001 -t 15 -P 1 --json`. Receiver throughput is the HND0 iperf3 server report (bytes / duration) from the server log, not the client JSON. Each run uses its own portblock range (near 30100+100i, far 31100+100i). All 10 runs succeeded; no EOF, no Broken pipe, no retry. `ip route get 45.11.1.62` resolved via 192.168.123.254 dev wlp4s0 (no Meta/198.18.x) before and after the matrix; HND0 load stayed 0.19-0.73.

Common parameters: `keepalive=2000 hole_budget_ms=30000 carrier_stall_ms=15000 replay=8388608 nonce=00112233445566778899aabbccddeeff`; far `listen=0.0.0.0:9000 target=127.0.0.1:5201`; near `listen=127.0.0.1:9001 peer=45.11.1.62:9000`.

## Throughput and wire cost

| config | run | receiver Mbit/s | client Mbit/s | wire/submit | retx_ppm | dup_ppm | holes_unrec | carrier_lost | status |
|---|---|---|---|---|---|---|---|---|---|
| c1-raw-n1m0d0 | 1 | 3.580 | 3.580 | 1.020 | 2290 | 10 | 0 | 1 | OK |
| c1-raw-n1m0d0 | 2 | 3.993 | 3.993 | 0.998 | 1039 | 4 | 0 | 1 | OK |
| c2-raw-n4m1d0 | 1 | 3.851 | 3.851 | 1.038 | 5828 | 3371 | 0 | 5 | OK |
| c2-raw-n4m1d0 | 2 | 3.447 | 3.447 | 1.041 | 6161 | 4424 | 0 | 5 | OK |
| c3-raw-n8m1d0 | 1 | 4.266 | 4.267 | 1.095 | 43171 | 40452 | 0 | 9 | OK |
| c3-raw-n8m1d0 | 2 | 4.062 | 4.060 | 1.086 | 26655 | 24507 | 0 | 9 | OK |
| c4-httpkal-n4m1d0 | 1 | 4.006 | 4.007 | 1.052 | 11247 | 5203 | 0 | 0 | OK |
| c4-httpkal-n4m1d0 | 2 | 2.265 | 2.268 | 1.118 | 55258 | 43078 | 0 | 0 | OK |
| c5-raw-n4m1d1 | 1 | 4.068 | 4.064 | 2.018 | 11754 | 11037 | 0 | 8 | OK |
| c5-raw-n4m1d1 | 2 | 5.040 | 5.043 | 2.016 | 1650 | 37 | 0 | 7 | OK |

Notes: `retx_ppm` and `dup_ppm` are the near `retx_data_ppm` and `dup_bytes_ppm` window values, not cumulative ratios. `carrier_lost` is the near `lost` delta (carrier loss/replacement events), not lost data; `holes_unrecoverable` is 0 in every run.

## Resources and receive queue

| config | run | near busy avg/max % | far busy avg/max % | far iperf3 % | far rxq MiB | far rx stall ms | near txwake_busy % | near win_block |
|---|---|---|---|---|---|---|---|---|
| c1-raw-n1m0d0 | 1 | 63.7/100.0 | 14.8/67.0 | 0.39 | 0.00 | 0.0 | 17.9 | 1330 |
| c1-raw-n1m0d0 | 2 | 30.8/100.0 | 12.4/43.9 | 0.50 | 0.00 | 0.0 | 21.1 | 1227 |
| c2-raw-n4m1d0 | 1 | 83.0/96.0 | 30.9/63.0 | 0.33 | 3.43 | 322.4 | 19.8 | 583 |
| c2-raw-n4m1d0 | 2 | 53.6/69.1 | 27.6/67.7 | 0.28 | 3.71 | 601.9 | 26.8 | 2016 |
| c3-raw-n8m1d0 | 1 | 33.7/73.7 | 24.6/59.8 | 0.33 | 3.27 | 263.9 | 29.2 | 4197 |
| c3-raw-n8m1d0 | 2 | 85.0/99.9 | 32.7/63.0 | 0.28 | 3.57 | 297.2 | 17.5 | 0 |
| c4-httpkal-n4m1d0 | 1 | 83.9/100.0 | 9.8/61.9 | 0.33 | 0.00 | 0.0 | 15.9 | 0 |
| c4-httpkal-n4m1d0 | 2 | 40.9/70.9 | 9.5/43.3 | 0.17 | 0.00 | 0.0 | 26.0 | 0 |
| c5-raw-n4m1d1 | 1 | 10.9/22.1 | 36.8/94.0 | 0.44 | 3.15 | 393.7 | 30.7 | 2882 |
| c5-raw-n4m1d1 | 2 | 34.5/74.8 | 14.5/61.9 | 0.56 | 0.00 | 0.0 | 30.1 | 2457 |

## Direct-WAN baseline comparison

The baseline numbers are iperf3 receiver means with 1/4/8 parallel TCP streams to HND0 (see `../baseline-direct`). The matrix above always uses `-P 1`; `n` is the number of tbtun carriers, not iperf3 streams. The comparable baseline is P1; P4/P8 are shown as reference ceilings.

| config | tbtun mean Mbit/s | vs baseline P1 17.08 | vs baseline P4 30.03 | vs baseline P8 33.72 |
|---|---|---|---|---|
| c1-raw-n1m0d0 | 3.787 | 22.2% | 12.6% | 11.2% |
| c2-raw-n4m1d0 | 3.649 | 21.4% | 12.2% | 10.8% |
| c3-raw-n8m1d0 | 4.164 | 24.4% | 13.9% | 12.3% |
| c4-httpkal-n4m1d0 | 3.136 | 18.4% | 10.4% | 9.3% |
| c5-raw-n4m1d1 | 4.554 | 26.7% | 15.2% | 13.5% |

All configurations are far below the direct baseline. The best matrix result (c5, dup=1) is still only 26.7% of direct P1; the n=8 result is 12.3% of direct P8.

## Effect of each knob

### n (carrier count)

n=1 3.79, n=4 3.65, n=8 4.16 Mbit/s (means). Going from 1 to 8 carriers adds about 10%, while the direct path gains about 97% from 1 to 8 parallel streams. The near peer tables show why: total in-flight bytes across all carriers stay at 120-131 KB regardless of n (122880 B at n=1; 122880/130807 B at n=4; 130860/131072 B at n=8), at RTT 234-300 ms. 120 KB * 8 / 240-300 ms = 3.3-4.1 Mbit/s, which is the measured range (3.45-4.27). Adding carriers redistributes the same window instead of enlarging it. The far side also ends n>=4 runs with 3.1-3.7 MiB still queued (`rxq`) and 263-602 ms receive stalls, so the window is not released faster either. More carriers add reordering work, not delivered throughput.

### m=1 (spare carrier)

c2/c3 run with m=1. `spare_activated=1` in c2-r1 and c3-r1; the spare also carries traffic (`tx_bytes` 348 KB / 592 KB at the near peers), so it is not a pure idle standby. m=1 does not raise throughput; its value is redundancy: every raw run lost carriers during the test, yet `holes_unrecoverable=0` throughout. The m=0 baseline (c1) also lost its only carrier once and recovered.

### dup=1 (near-side duplication)

c5 (dup=1) shows the cost directly: `wire/submit` 2.018 / 2.016 versus 1.038 / 1.041 for c2 (dup=0); near `dup_bytes` 9.28 MB for 9.63 MB submitted, `inflate_ppm` 1.96e6. c5's total in-flight is larger (189175 B r1, 139264 B r2) than c2's 120-131 KB, which raises the window/RTT ceiling and is the plausible mechanism for the higher numbers. Receiver 4.07 / 5.04 Mbit/s (mean 4.55) is 25% above c2's mean, but c5 ran with much lower external near load (near busy 10.9% / 34.5% versus 83.0% / 53.6% for c2) and c5-r2 ended with `rxq=0` while both c2 runs ended with 3.4-3.7 MiB backlog. With two samples and this confounding, the matrix does not establish a throughput gain for dup; the doubling of wire bytes is unambiguous.

### http-kal

http-kal (c4) gives 4.01 / 2.27 Mbit/s (mean 3.14), 14% below raw n=4 (3.65). The HTTP container is visible: near `http_req=5`, far `http_resp=5`, `pad_tx`/`drip_tx` non-zero, and wire/submit 1.052 / 1.118 versus 1.040 for raw. Its advantages are stability and lower far CPU: `carrier_lost=0` (every raw run lost carriers), `rxq=0`, no receive stall, far busy 9.5-9.9% versus 12-37% for raw. c4-r2's 2.27 Mbit/s came with 55258 ppm retransmission. No HTTP QoS gain is visible on this path.

## Anomalies and limitations

1. **raw carriers are lost and rebuilt during every raw run.** Before stats show `lost=0`; after stats show `lost` deltas of 1/5/9/8 (r1) and 1/5/9/7 (r2) for c1/c2/c3/c5, i.e. about one loss per carrier for c1-c3 and 7-8 events over 5 carriers for c5 (dup=1), with matching `replaced` and changed source ports. http-kal shows `lost=0`. No data was lost (`holes_unrecoverable=0`), but each rebuild costs a 263-602 ms far receive stall. Working hypothesis to check later: raw carriers emit no PAD (`c8 pad_tx=0`) and `carrier_stall_ms=15000` coincides with the 15 s test window, so the stall timer may not be reset by data/ACK. This matrix did not change the common parameters to test that hypothesis.
2. **The 15 s window does not reach steady state for most raw n>=4 runs (c2 both, c3 both, c5-r1).** Near submits 9.3-10.4 MB, far delivers 6.6-8.2 MB, and 3.1-3.7 MiB remain in `far rxq` when the client stops. The server-side receiver number is therefore a lower bound on what the tunnel could deliver with a longer run.
3. **External near load.** Throughout the matrix the near host ran unrelated `cc1plus` / `test_multistream` / `test_nv12_pipeline` jobs (1-min load 3.5-9.0, machine busy 10.9-85%). They were not killed. The near CPU columns are machine-wide and are contaminated. The direct baseline was measured under similar load (`local_busy_median` 54.1%, machine busy near 100%) and still reached 17-34 Mbit/s, so load alone does not explain the ~4 Mbit/s tunnel result.
4. **near iperf3 process CPU was not captured by the matrix sampler** (baseline initialized before the child existed). A supplementary 15 s run of c2 with the fixed sampler (`supp-c2-near-iperf-cpu.txt`) measured 0.0% of one core (31 samples), machine busy 33.3%; that run reached 5.38 Mbit/s. far iperf3 process CPU was 0.17-0.56% in the matrix. The client is not the bottleneck.
5. **far `dup_bytes_ppm=1000000` under http-kal is a stale window value**; the near-side dup counters are the meaningful send-side figures. This field is not used for any conclusion above.
6. All runs used 15 s as specified. Shorter runs had produced EOF during deployment; no failure occurred here.

## Conclusion

- tbtun does **not** reach the direct-WAN baseline. Single-stream receiver throughput is 3.14-4.55 Mbit/s, 18-27% of direct P1 (17.08) and 9-15% of direct P8 (33.72).
- Increasing n from 1 to 8, adding m=1, enabling dup=1, or switching to http-kal produced no proportional throughput gain. dup=1 doubles wire bytes; http-kal removes carrier losses but is slower than raw here.
- The measured limit is the tunnel's total in-flight window, about 120-131 KB, which does not grow with n; at 234-300 ms RTT that caps single-stream throughput near 3.3-4.1 Mbit/s. far-side ordered delivery (`rxq` 3.1-3.7 MiB, stalls 0.26-0.60 s) and raw carrier rebuilds during the test compound it.
- Follow-up (not done here): inspect raw carrier stall-timer reset and PAD behaviour, raise `rx_reorder_bytes` / carrier window, and rerun with a longer steady-state window on an idle near host.

## Server log slices

| tag | start offset | slice bytes |
|---|---|---|
| c1-raw-n1m0d0-r1-a1 | 8456 | 1626 |
| c1-raw-n1m0d0-r2-a1 | 10082 | 1626 |
| c2-raw-n4m1d0-r1-a1 | 11708 | 1626 |
| c2-raw-n4m1d0-r2-a1 | 13334 | 1626 |
| c3-raw-n8m1d0-r1-a1 | 14960 | 1626 |
| c3-raw-n8m1d0-r2-a1 | 16586 | 1626 |
| c4-httpkal-n4m1d0-r1-a1 | 18212 | 1624 |
| c4-httpkal-n4m1d0-r2-a1 | 19836 | 1626 |
| c5-raw-n4m1d1-r1-a1 | 21462 | 1626 |
| c5-raw-n4m1d1-r2-a1 | 23088 | 1626 |

## Raw data index

`results.jsonl` (per-run parsed record), `config-*.txt`, `stats-*.txt`, `peers-*.txt`, `tunnels-*.txt`, `iperf-*.json` + `.stderr.txt`, `server-*.log` (server log slice per run), `cpu-*-far.csv`, `env-before.txt`, `console.log`, `supp-c2-near-iperf-cpu.txt`.
