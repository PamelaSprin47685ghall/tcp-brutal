#!/usr/bin/env python3
"""Supplementary run: measure the near-side iperf3 client process CPU.

The matrix sampler initialized its per-PID baseline before the child existed,
so `near_iperf_cpu` is null in results.jsonl. This run repeats config c2 for
15 s with the fixed sampler and records the client process CPU. It is not part
of the matrix table.
"""
import json
import subprocess
import sys
import time

sys.path.insert(0, "/home/kunweiz/Desktop/vibe/tcp-brutal/review/wan-experiment/matrix")
import run_matrix as m

cfg = m.CONFIGS[1]  # c2-raw-n4m1d0
tag = "supp-c2-near-iperf-cpu"
out = m.OUT + "/" + tag + ".txt"

m.del_tunnels()
e = m.far_write_cfg(m.config_text(cfg, "far", "33100-33199"))
assert e.returncode == 0, e.stderr
e = m.near_write_cfg(m.config_text(cfg, "near", "32100-32199"))
assert e.returncode == 0, e.stderr
time.sleep(3.0)

pid_holder = [None]
sampler = m.Sampler(pid_holder)
sampler.start()
t0 = time.time()
p = subprocess.Popen([m.IPERF, "-c", "127.0.0.1", "-p", "9001", "-t", str(m.DUR),
                      "-P", "1", "--json"], stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, text=True)
pid_holder[0] = p.pid
stdout, stderr = p.communicate(timeout=m.DUR + 40)
p.returncode = p.returncode
sampler.stop.set()
sampler.join(timeout=3)
wall = time.time() - t0

lines = ["config=%s run=supplementary wall=%.2f iperf_rc=%d" % (cfg["cid"], wall, p.returncode),
         "t,busy_pct,iperf_cpu_pct"]
for s in sampler.samples:
    lines.append("%s,%s,%s" % (s.get("t"), s.get("busy_pct"), s.get("iperf_cpu_pct", "")))
near_cpu = m.cpu_summary(sampler.samples, "busy_pct")
near_iperf = m.cpu_summary(sampler.samples, "iperf_cpu_pct")
lines.append("near_busy=%r" % near_cpu)
lines.append("near_iperf_cpu=%r" % near_iperf)
try:
    j = json.loads(stdout)
    lines.append("receiver_mbps=%.3f" % (j["end"]["sum_received"]["bits_per_second"] / 1e6))
except ValueError:
    lines.append("client_json_error=1 stderr=%r" % stderr[:200])
with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")
print("\n".join(lines[-5:]))
