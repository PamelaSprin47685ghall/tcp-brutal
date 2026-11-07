# 1 GiB acceptance run

- start: 2026-11-07T13:42:30Z
- end: 2026-11-07T14:06:55Z
- near: `root@192.168.123.65` (aarch64, 6.1.172)
- far: `root@45.11.1.62`
- shape: 256 carriers, n=4 m=4, grouped, w=8, crypto=on, http-kal
- evidence: `/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/arm64/accept-1g-20261107T134014Z`

## Verdict

**FAIL**

| check | value |
|---|---|
| curl http | 200 |
| size_download | 1073741824 |
| file size | 1073741824 |
| near sha256 | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 |
| far sha256 | c6a256153f71328c987129abfd68d610d0da04834259b66c3addffbba7a79a93 |
| elapsed | 1451.026043 s |
| throughput | 5.92 Mbit/s |
| near CPU busy | 16.6% |
| far CPU busy | 64.4% |
| near boot_id stable | yes |
| feeder after run | pid=23184(python3) |
