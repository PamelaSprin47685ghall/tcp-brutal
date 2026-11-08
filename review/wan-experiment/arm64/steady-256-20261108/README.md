# 256-carrier steady state (fix39, 2026-11-08)

- Code: fix39 (a6eb228), `hold_enable=1` (the new default).
- Shape: 256 carriers, `n=4 m=4` grouped `w=8`, `fec=on`, `crypto=on`,
  `profile=http-kal`, one stream.
- 10 s sliding window: max 27.9 Mbit/s (t=9.75 s); 60 s average
  23.9 Mbit/s (179.31 MB in 60.07 s).
- wire/app 2.03 (363.51 MB wire); retx 37,177 ppm (6.67 MB); NACK rx 103 /
  tx 104; replay hit 809 / miss 0.
- far CPU 65.7%, near 9.7%; carriers up 256..256 for the whole run.
- `bind_drop_no_stream=0`, `fec_drop_window=0`; no WARNING in either kernel
  window.

Files: `summary.txt` and `analysis.json` are the verdict and the per-window
data; `near-sampler.txt` / `far-sampler.txt` are the raw samples; `curl.txt`,
`module.txt`, `join.txt`, `bypass.txt`, the `*-config.txt` read-backs, the
`*-dmesg-delta.txt` windows and `cleanup.txt` are the run evidence;
`ev82-steady256-hold1.log` is the driver log.
