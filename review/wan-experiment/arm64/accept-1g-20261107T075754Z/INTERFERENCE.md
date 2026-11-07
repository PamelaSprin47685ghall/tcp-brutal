# Run invalidated by external module cycling on near

This run produced no transfer. The near module was loaded, unloaded and
reloaded by something outside this run, between the join gate and the download.

near dmesg (uptime seconds, host boot 0a91d2eb):

    32334.637593  tbtun 0.2.0 loaded: 1 tunnels max, 256 carriers max   <- this run's insmod
    32359.607560  tbtun: unloaded                                        <- external
    32360.623356  tbtun 0.2.0 loaded: 1 tunnels max, 256 carriers max   <- external
    32376.317488  tbtun: tunnel 1 carrier 57 failed to connect: -110
    32385.863899  tbtun: tunnel 1 carrier 210 failed to connect: -110
    32394.887210  tbtun: unloaded                                        <- external

far dmesg (uptime seconds):

    88425.807235  tbtun: tunnel 1 carrier 38 gen 33 lost: ...
    ...           (many carriers lost in the same millisecond)
    88425.895361  tbtun: no carrier can carry: user side backpressured

Timeline (UTC):

    07:58:45  harness insmods near (32334.6)
    07:59:04  join gate PASS 256/256
    07:59:05  near unloaded (32359.6)      <- external
    07:59:06  near reloaded (32360.6)      <- external
    07:59:06  far loses every carrier (88425.8), "user side backpressured"
    07:59:11  transfer-start; near-fetch finds no /proc/net/tbtun/config
              and 9001 not listening (NEAR_CHECK procfs/listen FAIL)
    07:59:22  near carriers fail to connect: -110 (32376, 32385)
    07:59:40  near unloaded again (32394.9) <- external
    08:02:17  near-fetch gives up (rc=4); curl never ran

Consequences for the verdict:

- `size_download=127926074` and `near_sha256=5bd3dfaa…` in SUMMARY.md are the
  **round-8 leftover file** in /root/dl-1g, not a transfer from this run.
- The harness's counters are all `?` because the module was gone when it read
  `/proc/net/tbtun/stats`.
- No c10/c11/c12 data exists for this run; nothing about fix19 was exercised.

No rerun was attempted (task rule). The link and both hosts are otherwise
healthy: far still runs fix19 (C0346E98476A3A31EF512A4), 5210 serves the 1 GiB
source, near has no module loaded, the watchdog feeder pid=23184 is alive and
near's boot_id is unchanged.
