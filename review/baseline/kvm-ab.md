# KVM A/B：`-cpu max -smp 2` vs `-cpu host -smp 4`

日期：2026-11-05 16:27–16:31。只改 `tests/qemu/run.sh` 的一个参数块；未改 `benches/run.sh` 的 CPU 模型，未改任何测试判据，未跑完整套件。

## 1. 预检

原始记录：`review/baseline/kvm-ab/00-preflight.txt`。

| 项 | 值 |
|---|---|
| 时间 | 2026-11-05T16:27:19+08:00 |
| `pgrep -af qemu-system` | 空 |
| 19100 / 19101 监听 | 无 |
| `/tmp/tbtun-bench.lock` | 未持有 |
| `git rev-parse HEAD` | `ee0e5e632c90704c77a4364182e185fe0aed61e1` |
| `tbtun.ko` | 5,589,640 B，mtime 16:25 |
| `sha256sum tbtun.ko` | `96d5a997e6ab86ae410c63304c5d66ad6f1271b01af1859ad7cd4827989da0f1` |

四次 bench CSV 的 `ko_sha256` 都是 `96d5a997…`，与预检一致。

## 2. 改动

备份：`review/baseline/kvm-ab/run.sh.orig`（sha256 `1b28694c…`，与改动前一致）。

`tests/qemu/run.sh` 的 diff：

```diff
@@ -114,6 +114,15 @@
     accel=kvm
 fi
 
+# The guest CPU model and vCPU count are tunable for A/B runs.  The default
+# pair (max/2) is the historical baseline.  `-cpu host` is KVM-only, so a TCG
+# run falls back to max/2 instead of failing to boot.
+cpu=${TBT_TEST_CPU:-max}
+if [ "$accel" != kvm ] && [ "$cpu" = host ]; then
+    cpu=max
+    smp=2
+fi
+
@@ -132,7 +141,7 @@
 timeout --foreground "$timeout_s" qemu-system-x86_64 \
-    -accel "$accel" -cpu max -smp "$smp" -m 512 \
+    -accel "$accel" -cpu "$cpu" -smp "$smp" -m 512 \
```

默认行为：不设环境变量时 `cpu=max`、`smp=${TBT_TEST_SMP:-2}`，启动行展开为 `-cpu max -smp 2`，与改动前逐字节相同。
B 变体：`TBT_TEST_CPU=host TBT_TEST_SMP=4`。TCG 下 `host` 自动回退为 `max/2`。

## 3. 探针 B：A/B 各 2 次（交替）

命令：

```sh
tests/qemu/run.sh --probes B --timeout 120 --log review/baseline/kvm-ab/a1.log
TBT_TEST_CPU=host TBT_TEST_SMP=4 tests/qemu/run.sh --probes B --timeout 120 --log review/baseline/kvm-ab/b1.log
# 再重复 a2/b2
```

| 运行 | 变体 | wall (s) | `Run /init` (s) | `nr_cpu_ids` | 探针结果 |
|---|---|---|---|---|---|
| a1 | A（max/2） | 5.94 | 0.626 | 2 | B PASS |
| b1 | B（host/4） | 5.91 | 0.568 | 4 | B PASS |
| a2 | A（max/2） | 5.90 | 0.619 | 2 | B PASS |
| b2 | B（host/4） | 5.94 | 0.563 | 4 | B PASS |

汇总：

- A wall 平均 5.92 s；B 平均 5.925 s。差异 <0.1%，方向不一致。
- A `Run /init` 平均 0.6226 s；B 平均 0.5658 s。B 快 9.1%，绝对值 0.057 s，未达 10% 阈值。
- 四次 `TBT_PROBE_RESULT B PASS`，字节数都是 `4 MiB crossed each way`，没有结果变化。
- `nr_cpu_ids` 确认 B 真的用了 4 vCPU；A 是 2。
- 两次的 `smpboot: CPU0:` 都报 `AMD Ryzen 9 7940HS`，`-cpu max` 在 KVM 下也报告宿主型号；CPU 模型差异没有体现在这一行。

## 4. bench `--quick --n 4`：A/B 各 2 次

命令：

```sh
benches/run.sh throughput --quick --n 4 --tag kvm-ab-a
TBT_BENCH_SMP=4 benches/run.sh throughput --quick --n 4 --tag kvm-ab-b
# 再重复 a2/b2
```

`--quick` 对 throughput 是 8 MiB、1 s 速率窗口。bench 侧没有 CPU 模型环境变量，所以 B 只改 SMP=4，`-cpu` 仍是 `max`；这与探针的 host/4 不同构。

| 运行 | 变体 | wall (s) | control | app | app/control | wire/app | retx_ppm | txq_max | rxq_max | rc | strict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| a1 | A（max/2） | 50.00 | 2.67 | 10.41 | 3.90x | 1.0252 | 7,490 | 98,304 | 49,152 | 0 | 1 |
| b1 | B（max/4） | 47.57 | 2.54 | 11.92 | 4.69x | 1.0005 | 254,856 | 98,304 | 0 | 0 | 1 |
| a2 | A（max/2） | 46.75 | 3.39 | 11.80 | 3.48x | 0.9894 | 37,291 | 98,304 | 0 | 0 | 1 |
| b2 | B（max/4） | 53.70 | 2.58 | 11.26 | 4.36x | 0.9885 | 208,966 | 98,455 | 0 | 0 | 1 |

两轮平均：

| 指标 | A 平均 | B 平均 | B 相对 A |
|---|---|---|---|
| wall | 48.38 s | 50.64 s | 慢 4.7% |
| control | 3.03 | 2.56 | 低 15.5% |
| app | 11.11 | 11.59 | 高 4.4% |
| `wire/app` | 1.0073 | 0.9945 | 低 1.3% |
| `retx_ppm` | 22,391 | 231,911 | 高 10 倍 |

`rc=0`、`strict=1` 四次全部成立；`holes_*` 全 0。

## 5. 判定：保留 A

依据用户给的判定规则：

- **探针结果不变**：四次 B PASS，A/B 没有判定差异。
- **wall 没有稳定改善**：探针 wall 差异 <0.1%，方向不一致；bench wall A 48.38 vs B 50.64，B 反而慢 4.7%。
- **启动时间改善未达阈值**：`Run /init` 0.6226 → 0.5657 s，改善 9.1%，低于 10%，且只占 5.9 s 总 wall 的约 1%。
- **负面信号**：B 的 `retx_ppm` 两次都显著高于 A（254,856 / 208,966 vs 7,490 / 37,291），与 SMP=4 的并发/调度一致，不是收益。

因此 `-cpu max -smp 2` 保持为默认。`tests/qemu/run.sh` 只保留可覆盖参数与注释，默认行为与改动前逐字节相同；没有把 B 设为默认。

**结论：KVM 已启用；`-cpu host -smp 4` 相对当前 `-cpu max -smp 2` 无显著收益，保留 A。**

## 6. 限制

- `--quick` 是 8 MiB / 1 s 窗口，单次噪声大；A 的 control 在 2.67–3.39 之间波动（33%），这比 A/B 的 app 差异更大。
- bench 的 B 只改了 SMP；`-cpu host` 的收益只能在探针 B 上观察，而探针 B 走 loopback、总时长只有 5.9 s。
- `-smp 4` 改变 vCPU 数，`cpu_total_pct` / `kworker_pct` 不能与 2 vCPU 直接比较；本报告只比 wall、app、wire/app、retx、rc/strict。
- 没有跑 I/J/K 的整形路径；本结论只覆盖 loopback 数据面与 `--quick` 吞吐。

## 7. 证据

- 预检：`review/baseline/kvm-ab/00-preflight.txt`
- 改动备份与 diff：`review/baseline/kvm-ab/run.sh.orig`；`tests/qemu/run.sh`
- 探针原始输出：`review/baseline/kvm-ab/{a1,a2,b1,b2}.txt` 与同名 `.log`
- bench 原始输出：`review/baseline/kvm-ab/{bench-a,bench-a2,bench-b,bench-b2}.txt`
- bench CSV（新增，未覆盖已发布数据）：
  - `benches/results/20261105T162817-throughput-kvm-ab-a.csv`
  - `benches/results/20261105T162911-throughput-kvm-ab-b.csv`
  - `benches/results/20261105T163005-throughput-kvm-ab-a2.csv`
  - `benches/results/20261105T163052-throughput-kvm-ab-b2.csv`
- 环境释放：无 qemu 残留，19100/19101 空闲，bench lock 未持有。
