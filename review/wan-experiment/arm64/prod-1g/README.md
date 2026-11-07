# prod-1g — 0.2.0 的 1 GiB 真机验收

在 `.65`（near，aarch64，6.1.172）与 `45.11.1.62`（far，x86_64，7.0.0-42）
之间跑一次 1 GiB 下载，判大小、sha256、两端计数器、内核日志与 near 存活。
拓扑与判定来自 `review/s2-release-design.md` §3 和
`review/s2-merged-design.md` §8.3。

## 1. 组成

| 文件 | 运行位置 | 作用 |
|---|---|---|
| `run-1g.sh` | 工作站 | 编排：预检、far 服务、模块与配置、join、下载、取证、判定、清理 |
| `far-serve.sh` | far | 生成/校验 1 GiB 文件、算 sha256、起回环 raw HTTP sender、就绪检查 |
| `far-raw-serve.py` | far | 无应用层写超时的单流 HTTP sender（HEAD/GET，发完 SHUT_WR） |
| `far-stream-watch.sh` | far | 传输期间采样 per-stream 行，保留 teardown 时的 `tgt_state/tgt_err/tx_seq/...` |
| `near-fetch.sh` | near | 校验配置形状与 join、执行唯一一次 curl、本地判定、失败取证 |

三个脚本各自独立可跑：`far-serve.sh --status` 与 `near-fetch.sh --preflight`
都是只读的，可以单独用来确认现场。

## 2. 前置条件（缺一不可）

1. **near 的硬件喂狗者必须正在运行**。停掉它等于启动 44 s 复位倒计时
   （`MAGICCLOSE=0`）。`run-1g.sh` 预检会扫描 `/proc/<pid>/fd` 确认有进程持有
   `/dev/watchdog0`，没有就拒绝开跑。验收期间**不要**停喂狗者。
2. near 与 far 的 `tbtun.ko` 已就位（默认 `/root/tbtun-0.2.0/tbtun.ko`），
   且 `modinfo -F srcversion` 与加载后的 `/sys/module/tbtun/srcversion` 一致。
   `MODULES_LOADED=1` 时脚本不 insmod，只校验。
3. far 的 `/root` 至少 1.5 GiB 可用；**不要**放 `/tmp`（far 的 `/tmp` 是
   478 M tmpfs）。
4. far 有 `python3`（raw sender）；busybox httpd 不再是 5210 源。near 不依赖
   python3，HTTP 服务只放 far。
5. 端口空闲：near `9001`、far `9000`、far 回环 `5210`；portblock
   near `30000-30511`、far `31000-31511`。
6. near 与 far 的时钟不需要同步；判定不依赖两端时间戳对齐。

## 3. 用法

```sh
cd review/wan-experiment/arm64/prod-1g

./run-1g.sh --preflight        # 只读：ssh、喂狗者、工具、磁盘、端口、模块身份
./run-1g.sh --run              # 完整一次；默认不重试
./run-1g.sh --run --keep       # 跑完保留模块、配置与文件，便于取证
./run-1g.sh --cleanup          # 只清理：删隧道、rmmod、停 httpd、删文件
```

常用覆盖：

```sh
NEAR_KO=/root/tbtun-0.2.0/tbtun.ko FAR_KO=/root/tbtun-0.2.0/tbtun.ko \
NEAR_SRCVERSION= FAR_SRCVERSION= CARRIERS=256 JOIN_TIMEOUT=180 \
./run-1g.sh --run
```

配置形状固定为：`256 carriers, n=4 m=4 fec=on mode=grouped w=8 crypto=on
profile=http-kal`，`nonce=00112233445566778899aabbccddeeff`，far
`target=127.0.0.1:5210`，near `listen=127.0.0.1:9001`。

## 4. 判定

全部成立才算 PASS：

| # | 判据 | 来源 |
|---|---|---|
| 1 | `curl` rc=0 且 `http=200` | `near-curl.txt` |
| 2 | `size_download=1073741824` 且本地文件大小相同 | `near-curl.txt`、`near-size.txt` |
| 3 | near 的 sha256 等于 far 的 sha256 | `near-sha256.txt`、`far-sha256.txt` |
| 4 | 两端 `fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、`gaps_total`、`rx_errs`、`connect_fail`、`alloc_fail`、`join_bad` 的**增量**全为 0 | `counters-{near,far}.txt` |
| 4b | 两端 `fec_stream_refused`、`fec_replay_miss_enospc`、`fec_replay_miss_evicted`、`fec_nack_drop`、`fec_reset_hole`、`holes_unrecoverable`、`close_timeouts` 的**增量**全为 0；`fec_replay_stale` 可增（信息项） | `counters-{near,far}.txt` |
| 5 | 两端内核窗口无 `BUG:`/`Oops`/`WARNING:`/`UBSAN:`/`general protection fault` | `*-kernel-window.txt` |
| 6 | near 的 `boot_id` 前后一致（没有重启） | `near-pre.txt`、`near-post.txt` |
| 7 | 结束时仍有进程持有 `/dev/watchdog0` | `run.log` |
| 8 | `dead_now ≤ 64`；`sess_refs=0`（构建导出该字段时；缺失则 SKIP） | `counters-near.txt` |

失败判定与超时：

- 传输总上限 **7200 s**（`curl --max-time`），外层 `timeout 7500 s`。
- 停滞：**120 s** 内平均速率低于 **64 KiB/s** 即失败（`--speed-limit 65536
  --speed-time 120`，curl rc=28）。
- 单次约束：不重试、不降级、不换工具。失败就是失败，证据留下，人工决定。
- near 失联按崩溃处理：不重试，先取 `dmesg`/pstore/netconsole，再决定是否
  重启。

## 5. 证据

目录 `review/wan-experiment/arm64/accept-1g-<UTC>/`：

| 文件 | 内容 |
|---|---|
| `SUMMARY.md` | 判定、大小、sha256、耗时、吞吐、CPU、计数器、清理 |
| `timeline.txt` / `run.log` | 阶段时间线与脚本日志 |
| `far-sha256.txt`、`far-size.txt`、`far-sha256-file.txt` | far 源文件 |
| `far-serve.log`、`far-httpd.log`、`far-httpd-headers.txt` | HTTP 服务与就绪 |
| `far-config.txt`、`far-tunnels.txt`、`far-ss.txt` | far 配置回读 |
| `far-stats-pre.txt`、`far-stats-post.txt`、`counters-far.txt` | far 计数 |
| `far-kernel-window.txt`、`far-dmesg.txt` | far 内核 |
| `near-curl.txt`、`near-size.txt`、`near-sha256.txt` | 下载结果 |
| `near-config.txt`、`near-tunnels.txt`、`near-ss.txt` | near 配置回读 |
| `near-stats-pre.txt`、`near-stats-post.txt`、`counters-near.txt` | near 计数 |
| `near-kernel-window.txt`、`near-dmesg.txt`、`near-pstore.txt` | near 内核与崩溃可见性 |
| `near-netwatch.txt`、`near-watchdog.log` | 喂狗者日志 |
| `near-fetch.log`、`near-counters.txt`、`near-verdict.txt`、`perf.txt` | near 本地判定与性能 |
| `join-probes.txt` | 每秒两端 `c3 up` |
| `near-evidence/` | 失败时 near-fetch 自动抓取的现场 |
| `cleanup.txt` | 清理结果 |

## 6. 性能基线

`perf.txt` 记录：字节数、耗时、吞吐（Mbit/s）、两端 CPU busy 百分比、near
loadavg、`MemAvailable`、`tbt_*` slab、`replaced`/`data_tx`/`retx_bytes`/
`dup_bytes_ppm`。这是 0.2.0 的**记录**，不是通过条件：`B-05`/`B-06` 的吞吐
方差与单次记录仍按 `记录` 结案，1 GiB 只判完成与校验。

CPU 百分比的口径：near 由 near-fetch 在 curl 前后读 `/proc/stat` 计算；far
由编排脚本在下载前后读取。far 的窗口含少量 ssh 往返，量级不影响结论。

## 7. 失败分支

| 现象 | 处理 |
|---|---|
| 预检失败 | 退出 3，不写配置、不加载模块 |
| far 服务失败 | 退出 4；看 `far-serve.log`（磁盘、busybox、端口） |
| join 未到 256 | 退出 4；`join-probes.txt` 记录每秒进度；不触发下载 |
| curl rc=28 | 退出 5；超时或停滞，看 `near-curl.txt` 与 `near-dmesg.txt` |
| 大小不符 | 退出 5；`size_download` 与文件大小都在 `SUMMARY.md` |
| sha256 不符 | 退出 6；保留两端 hash 与文件，不重跑 |
| 计数器非零 | 退出 7；`counters-*.txt` 给出增量与字段名 |
| 内核有 Oops/BUG | 退出 7；先取 pstore/netconsole，再决定是否重启 |
| near 重启 | 退出 7；按崩溃处理，不重试 |
| 清理不完整 | 退出 9；`cleanup.txt` 列出残留 |

## 8. 仍未验证的前置条件

- **`.65` 上的模块路径**：默认 `/root/tbtun-0.2.0/tbtun.ko`，需由 0.2.0 构建
  产物确认；脚本只校验 srcversion，不校验 ko 的 sha256（可加 `NEAR_SHA256`，
  当前版本未实现，属已知缺口）。
- **`sess_refs` 字段**：0.2.0 设计要求导出，当前树尚未见该字段；脚本在字段
  缺失时 `SKIP`。0.2.0 验收时用 `REQUIRE_SESS_REFS=1 ./run-1g.sh --run`
  （会传给 near-fetch），缺失即 FAIL。
- **far 的 `journalctl -k --since`**：若 far 没有 journald，回退到
  `dmesg | tail -n 2000`，窗口可能包含历史行；`far-dmesg.txt` 同时保留，
  供人工核对。
- **raw sender 的 HEAD/GET**：`far-raw-serve.py` 对 HEAD 只发头，对 GET 发
  完整 body；两条路径都要求 `Content-Length=1073741824`。body 发送使用阻塞
  `sendall`，不设 `SO_SNDTIMEO`，发完 `shutdown(SHUT_WR)`。
- **吞吐基线**：256 载波长流没有历史数据；7200 s 上限对应最低约 1.2 Mbit/s。
  实测低于此判失败，不调大超时。

## 9. fix21 的 raw 源与 teardown 抓取（已就绪，未跑长测）

1. `far-serve.sh --start` 会先停掉遗留的 busybox httpd，再启动
   `far-raw-serve.py`（127.0.0.1:5210，单流，无应用层写超时）。`run-1g.sh`
   在上传阶段同时 scp `far-serve.sh`、`far-raw-serve.py`、
   `far-stream-watch.sh`。
2. 下载前 `run-1g.sh` 在 far 启动 `far-stream-watch.sh`；传输结束后 touch
   `.far-stream-watch.stop`，并把以下文件 scp 到证据目录：
   - `far-stream-watch.last`：最后一条 live `tunnel 1 stream ...` 行（全字段）；
   - `far-stream-watch.fields`：从该行提取的
     `tgt_state/tgt_err/tx_seq/tx_fin_written/close_since_ns/fec_out_queued/fec_outq_queued`；
   - `far-stream-watch.txt`：WATCH_START/FIRST_STREAM/NO_STREAM_LINE/WATCH_STOP。
3. 判定新增/调整：`fec_stream_refused`、`fec_replay_miss_enospc`、
   `fec_replay_miss_evicted`、`fec_nack_drop`、`fec_reset_hole`、
   `holes_unrecoverable`、`close_timeouts` 必须为 0；`fec_replay_stale`
   （可增）、`fec_reset_user`（正常拆除）、`fec_replay_miss_get`/
   `never_stored`（控制方向探测）、`fec_nack_backpressure`、
   `holes_declared/repaired` 为信息项；`dead_now` 缺失时 SKIP。
4. fix21 就绪后的命令：
   ```sh
   NEAR_KO=/root/tbtun-0.2.0-fix21-<UTC>/tbtun.ko \
   NEAR_SRCVERSION=<fix21 srcversion> FAR_SRCVERSION=<far fix21 srcversion> \
   ./run-1g.sh --run --keep
   ```
   仍为单次、7200 s、不重试；PASS 后才跑 DKMS。
