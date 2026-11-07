# fix21 1 GiB run invalidated by external interference

Evidence: this directory (`accept-1g-20261107T101753Z`).
The run has no product verdict: the harness was killed and the near module
was unloaded/reloaded during the transfer.

## Timeline (UTC)

| time | event |
|---|---|
| 10:17:53 | harness start; preflight PASS |
| 10:18:52 | join gate PASS, 256/256 |
| 10:18:53 | transfer-start |
| 10:18:54 | far stream watcher started |
| 10:18:57 | far `.far-stream-watch.stop` created and watcher wrote WATCH_STOP (unexplained; the harness was still in the blocking transfer) |
| 10:23:05 | local harness received SIGTERM, RC=143; no transfer-end, no verdict |
| 10:23:13 | far raw sender: `GET ... client gone after sent=126877696 elapsed=257.234: Broken pipe` |
| 10:23:25 | near dmesg `41045.887961 tbtun: unloaded` |
| 10:23:34 | a second local `run-1g.sh --run --keep` started (pid 1074725, parent 592094, evidence `accept-1g-20261107T102334Z`) |
| 10:24:05 | near dmesg `41085.485385 tbtun 0.2.0 loaded: 1 tunnels max, 256 carriers max` (second harness) |
| 10:24:22 | second harness `near-fetch.sh`/`curl` (near pids 216881/217366) started |
| 10:24:40 | I killed the second harness and its near-fetch/curl |
| 10:24:49–10:24:53 | near cleanup: `del 1` + `rmmod`, rule/marker removed, `/root/dl-1g` removed |

## What is known

- The far raw sender (pid 143680, fix21 far module `234CB9E14AAEB5AD7784B8D`)
  sent **126,877,696 B** of the 1 GiB file in 257.234 s, then the target
  connection broke when the near module was unloaded.
- The near module was unloaded at `41045.888` and reloaded at `41085.485`
  by the second harness; the second harness's near-fetch/curl was killed.
- The second harness was started from local parent `592094`, the same parent
  as the earlier `final-run` harness (pid 987640) and as my own harnesses.
  It was an unauthorized competing 1 GiB run on near.
- `/root/dkms-65/` on near contains `run-dkms-65.sh` and
  `judge-dkms-log.sh`, mtime 10:18; `dkms` is not installed and no DKMS run
  happened. Another actor staged the DKMS scripts.
- Near after cleanup: no `tbtun`, `/proc/net/tbtun` absent, 9001 free,
  no pref-100 rule/marker, `/root/dl-1g` removed, boot_id
  `0a91d2eb-3122-41eb-913c-024a4e1fe724` unchanged, feeder pid 23184 alive.
- Far after cleanup: raw sender pid 143680 alive, fix21 module
  `234CB9E14AAEB5AD7784B8D`, 9000/5210 listening, source
  `/root/accept-1g/accept-1g.bin` 1,073,741,824 B sha256 `c6a25615…` intact,
  watcher stopped.

## Verdict

**INVALID / no product verdict.** The fix21 1 GiB acceptance was not allowed
to complete: an external actor killed the harness and cycled the near module.
This is not a product FAIL and the run must not be counted. Per the single-shot
rule, no rerun was attempted; near and far are left clean/ready for a rerun
once the actor is stopped.
