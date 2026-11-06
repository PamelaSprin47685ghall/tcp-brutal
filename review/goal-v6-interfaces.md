# tbtun v6 接口冻结（T0）

状态：**冻结，T0 已验收**（Main 裁决，2026-11-06；验收基线 `7105507`，745 行）。本文件后续只追加裁决/跟踪，ABI 未变。基线 `master 0db5166`。
权威来源：B（`fec-v6`）的 40B header / 控制帧提案，经用户批准为 T0 冻结基线；本文件是 T0 的唯一接口契约。
变更控制：任何 wire 字段、偏移、类型、CRC 顺序的变更，必须 B 与 integrator 书面同意，并 bump `TBT_FEC_CONV`（当前 `0x1001`）。只改文档、不改 ABI 的编辑不 bump。
范围：T0 只写文档，不改 `tbtun/` 产品代码。本文中的头文件签名是 B/C/D 的实现契约，不是已落地的代码。

---

## 0. 冻结摘要

**STOP / 冒烟优先（2026-11-06 紧急）**：本地路由器被压测打挂，拔 `.65` 重启；`.65` 当前 `ping` 3/3 丢包、ARP `INCOMPLETE`、ssh `No route to host`。**立即停止 T5.2/direct P8/256 所有压测**；T5.2 HOLD。**禁止 HND0 256、8 并行 baseline、8–16MiB 压测**；先 smoke（小流量、单连接、≤1MiB、限时），经用户批准才加压。任何测试前确认路由器/`.65`/HND0 健康与限速。最终 head **`b41b259`**；arm64 T5.2 两端必须都用 b41b259 重建 ko（旧+新 parity 互不可解，会 `fec super ... stuck`）；n4m4 band=3、single band=31。`t52-arm64`/`sclc-arm64`/`arm64-build`/`wan-deploy` 已 STOP；不合并/push。

**孤儿清理/冒烟（2026-11-06，已不可执行）**：`t52-arm64-2` 已被 kill；`.65` `ping` 3/3 丢包、ARP `INCOMPLETE`、SSH `No route to host`，`6434`/`46646` 清理与 1 carrier/37B smoke **无法执行**。现在只由 `hnd0-verify` **只读清理 HND0 旧 `cb23c64` tbtun/tbt-echo**（若有），**不动 `.65`**。等用户恢复 `.65`/路由器后再重开 arm64 agents；T5.2 HOLD；不合并/push。

**STOP 执行（2026-11-06）**：`.65` `ping` 3/3 丢包、ARP `INCOMPLETE`、ssh `No route to host`；已 kill 所有 `.65` agents（`t52-arm64-2`/`sclc-arm64-2`/`arm64-build-2`/`node65-recon-2`），不再发任何流量。**HND0 256、8 并行、8–16MiB 全禁**；T5.2 HOLD；不合并/push。等用户恢复 `.65`/路由器并确认；之后只 **1 carrier/37B**，再 **8/16/32/64 × 1MiB/5s**。`hnd0-verify` 只读清理 HND0 旧 `cb23c64` tbtun/tbt-echo（若有），**不动 `.65`**。

**HND0 清理状态（2026-11-06）**：t52-arm64 确认 HND0 已清理——tbtun rmmod、9000/5202 关闭、echo 停、无测试进程；`/tmp/tbtun-hnd0.lock` 仍被两个 worker（`deploy b41b259…`、`ladder smoke`）持有，未动；`.65` 不可达，锁在 `.65` 上暂无法清（恢复后清 tbtun/ip rule/sysctl/bbr）。T5.2 HOLD；不合并/push。

**`.65` 恢复（2026-11-06）**：arm64-build 报 18:38 ping 通、18:39 SSH 22 通；t52-arm64 按 STOP 不再主动探测。HND0 已清理；`.65` 的锁/模块/sysctl/ip rule/bbr 清理与 smoke 等用户批准后做（若已重启，内核态残留可能已清）。T5.2 HOLD；不合并/push。

**`.65` 路径更正（2026-11-06）**：物理 **`192.168.123.65`** 完全可达（aarch64、up 21min、load 0.12、无 tbtun）；**`192.168.31.65` 是 Meta/mihomo 半活路径，不是目标**，脚本不要改新网段。`t52-arm64-3` 将用 `123.65` 做 **1 carrier/37B smoke**；HND0 clean；T5.2 HOLD；smoke 仍 **1 carrier/37B → 8/16 × 1MiB/5s**；256 需 `t52-pace` 首波 pacing。不合并/push。

**t52-pace（2026-11-06）**：改动只在 `w-t52-pace`（based `b41b259`），master 已还原干净；首波不再全量 `schedule_work`，`carriers_start` 只建 carrier + 16-token burst，50ms poll 用同一 `max(8,n/4)/s` 令牌桶驱动首拨；`tbtun.h`/CHANGELOG 已同步。允许 **本地 QEMU loopback** probe N 256/8MiB + A–P 作为 pacing 验证，**不碰 `.65`/HND0/WAN、不 8 并行**；T5.2 HOLD；head 待 QEMU 后提交；不 push。

**t52-pace 完成（`21e8eb2`，base `b41b259`）**：首波不再全量 `schedule_work`，50ms poll 经同一 `max(8,n/4)/s` 令牌桶（16-token burst）驱动。QEMU probe N 256/8MiB：257 次拨号/5.22s ≈49/s（cap 64/s），`connect_fail=0 connect_retry=0`，byte-exact、`tx_ack` 满；A–H/I–M/O/P/Q PASS（I 与 N 并行时首轮 flake，单独 PASS）。唯一 FAIL：probe N c9 `fec_ack_tx=8/rx=8`，在未修改 `b41b259` 上复现，属 probe 全零断言 vs 新增 ACK 计数器的**基线测试问题**，与本改动无关。未合入；T5.2 HOLD；不 push。

**t52-pace 合入 + probe 修复（`94fdfbc`）**：merge `21e8eb2`（`f242277`）；probe N 的 c9 全零断言排除 `fec_ack_tx/rx`（正常每超块 1 ACK），其余 c9 仍必须全零；make check 绿。最终 head **`94fdfbc`**；两端需按新 head 重建（pacing 改动）；HND0 256 不跑，先 **1 carrier/37B → 8/16 × 1MiB/5s**；不 push。

**fbcd820 构建标识**：x86 本机构建 `tbtun.ko` srcversion `CEC893BF68EBB155A61158E`、sha256 `36d3e23f…`、version 0.0.1、vermagic 7.2.9。**跨 arch 的 srcversion 可能不同，不要求相等**；不变式是两端同 source commit `fbcd820`（product `94fdfbc`），各自记录 ko sha/srcversion。HND0 far 用 `tbt_echo` 5202；部署后只 1 carrier/37B smoke；不 256/8 并行、不 push。

**HND0 far fbcd820（`t52-arm64-3`）**：ko sha256 `67acc024…`、srcversion `CEC893BF68EBB155A61158E`（与 x86 本机一致）、vermagic `7.0.0-42-generic x86_64`；insmod `max_carriers=16 debug=0` 成功；`tbt_echo` 5202 运行。等 `.65` aarch64 fbcd820 ko 后跑 **1 carrier/37B smoke**。

**`.65` aarch64 fbcd820 ko（`arm64-build-3`）**：ko sha256 `026d42cd…`、srcversion `D20BF3BF…`、vermagic `6.1.172 SMP mod_unload modversions aarch64`、BuildID `e776cf2f…`；insmod/rmmod 待 `t52-arm64-3` ladder smoke 释放后补验；已同步给 `t52-arm64-3`。smoke **1 carrier/37B** 进行中。

**1 carrier/37B smoke 观察（2026-11-06，未通过）**：near→far carrier 0 通、far `rx_delivered=37`，但 **far→near 无回包**、`streams_active=0`；`t52-arm64-3` 诊断 target/stream 生命周期超过 1h 无进展，已令其**收证据、清理 `.65`/HND0 后停**；`t52-debug` 在本地 QEMU 复现同一路径。**尚未通过，8/16 未跑**；不 256/8 并行、不 push。

**t52-arm64-3 无响应（2026-11-06）**：smoke 失败后 wake 无输出；`.65`/HND0 清理状态待确认。HND0 由 `hnd0-verify` 只读清理（tbtun/echo）；`.65` 清理需用户授权。T5.2 HOLD；不 8/16/256、不 push。

**HND0 清理完成（`hnd0-verify`，2026-11-06 12:15 UTC）**：发现 tbtun loaded（64 tunnels/16 carriers）、`/tmp/tbt_echo` PID 51893 监听 5202、内核 9000 listener、ESTAB `45.11.1.62:9000 ↔ 171.120.34.226:40000` 已 stall（`una_stall_us=803s`、`retx_data_ppm=636363`、wire 23.6KB、streams=2）；kill echo + `rmmod tbtun`，验证无 tbtun/`/proc/net/tbtun`/9000/5202 监听/连接/锁，生产服务 active；sysctl 仍生产 16MiB，未改。11:06:21–12:15:03 UBSAN/WARN/BUG=0。残留 `/tmp/tbt_echo`/`tbt_burst`/`tbt_ping`/日志/`tbt-http-baseline` 未删（等指示）。本地无 `tbt_*` 进程、无 9000 连接（仅 7000 的 ESTAB，非 smoke）。T5.2 HOLD；不 push。

**t52-debug QEMU 复现失败（`de335ef`，2026-11-06）**：本地 QEMU 1-carrier 37B（`carriers=1 n=1 m=0 fec=off profile=http-kal`，fork echo）**双向 byte-exact、稳定**；`streams_active=0` 是流关闭后的正常快照；**1 carrier 只能 `fec=off`（旧 ARQ）**，与 256 FEC 路径无关。HND0 失败时 `tbt_echo` listener 活着（PID 51893 5202，ESTAB 9000↔40000 stalled），但缺 `submit_bytes/capture_runs/tx_frames/rx_read_bytes` 的 5s 窗口数据；HND0 已清理，metrics 丢失。下一步需新 smoke owner 在窗口内抓 far/near stats，或怀疑 aarch64 near build/target。`de335ef` 是 test-only（probe P 1-carrier ARQ 相位），未合入。另 `/home` btrfs 95% 导致 git ref 写失败；`w-t52-debug` 已复原 `c948e88 → de335ef`；`~/.cache/uv` 19GB 非我们，未动；建议用户授权清理。T5.2 HOLD；不 8/16/256、不 push。

**1-carrier smoke 抓取配方（`t52-debug`）**：在 `tbt_ping` 5s 窗口内两端同时抓：far `stats | grep -E 'capture|rx_ready|c3 |streams|stream sid|c8'`、`peers`、`ss -tnp | grep 5202`；near `stats | grep -E 'rx_ready|c3 |streams|stream sid'`。判定：far `submit=0/tx_frames=0/near rx_read=0` → target 没回写（查 `tbt_echo` child/5202 Send-Q）；`submit=37/tx_frames=0` → ARQ TX 没发（c3/peers up/stall）；`submit=37/tx_frames≥1/near rx_read=0` → 发了没到/没交付（near `rx_proto/rx_errs/rx_delivered`）；`submit=37/tx_frames≥1/near rx_read=37` → 已通。`9000↔40000` ESTAB stalled 是长连接空闲，正常；`streams_active=0` 是关闭后快照，看 `n_streams`/`tgt_state`/`fin_*`。aarch64 near 先本地 loopback 37B + insmod/rmmod 补验。`de335ef` test-only 未合；产品修复在 `c948e88`。T5.2 HOLD；不 push。

**QEMU two37 + 替换风暴假设（`c6164b0`，2026-11-06）**：probe P `two37`（`n=1 m=1 carriers=2`，smoke knob）在 `fec=off` 和 `fec=on` 下两次 37B **均 PASS** → 不是“第二个连接/OPEN/adopt/stream 生命周期”的普适 bug。WAN 数据 `join_dup=13` + `streams_total=19` + dmesg 每秒 `ACCEPTED awaiting HELLO` + near spare `inflight=74`/`rx_lag_ppm=1000000` → **carrier 替换风暴**（spare TCP 未被 ACK；`holes_unrecoverable=2` 是后果）。stall 检测器对 idle spare 有 `una==nxt`/`paused` 刷新，不会误杀；spare 被判丢时确有未 ACK 字节。需 HND0 near `debug>=2` 的 `carrier stall idx/gen/una/nxt/wmem/snd_wnd/rcv_wnd/state` + c3 `stall_declared/carrier_lost/replaced/join_dup/connect_retry` + far `join_dup` 区分背压/真丢/RETIRE 竞态。`c6164b0` test-only 未合。环境：`~/.cache/tbtun-build*` 20:2x 被清空，QEMU 内核/headers/pahole 丢失；t52-debug 已重取到私有 cache 并重建 ko（`--no-net`）。T5.2 HOLD；不 push。

**smoke2 1-carrier 37B 失败定位（2026-11-06）**：ping#1 PASS；ping#2 **0/37**。near `capture_runs 4→5`、`submit 74→111`、`tx 148→222`、`rx_read 12711→13307`、`rx_delivered 37→37（stuck）`、`rx_proto=2`、`rx_errs=1(-71)`；near c3 `up=2 lost=18 replaced=19 spare_activated=2`、c6 `holes_unrecoverable=2`。far `submit 74→111`、`rx_delivered 74→111`、`tx 111→148`；carrier0 `inflight=74 rx_lag_ppm=1000000`；c3 `lost=3 replaced=0 spare_activated=1 join_dup=13`、`streams_total=19 rx_adopt_events=19`；dmesg 每秒 `ACCEPTED awaiting HELLO`。结论：far→near DATA 离开 HND0 但未到/未交付 `.65`；**carrier 替换风暴/HELLO-OPEN 代际竞态**（near 换得太快、far 仍占槽），不是 echo target。`t52-debug` 修；T5.2 HOLD；不 8/16/256、不 push。证据 `review/wan-experiment/arm64/smoke-20261106T1226Z-t52-smoke2/`。

**bounded debug run（2026-11-06，Main 授权）**：`t52-smoke2` 只跑 **1 carrier / 37B ×2 + debug=2**，确认 RETIRE-vs-replacement race；采集 near c3/stall 日志 + far `join_dup`；完成后清理停止；不 8/16/256、不 push。

**冒烟前部署/分工（2026-11-06）**：HND0 clean；`b41b259` x86 far ko **尚未部署**。`t52-arm64-3` = HND0 far b41b259 构建/部署 + `.65` **1 carrier/37B smoke**；`arm64-build-3` = `.65` aarch64 ko only。HND0 far 用 **`tbt_echo` 5202（不用 iperf3）**；1 carrier/37B 通过后 **8/16 × 1MiB/5s**；不 256/8 并行、不合并/push。

**HND0 清理确认（`hnd0-verify`，2026-11-06）**：10:56:45 UTC 无 tbtun、`/proc/net/tbtun` 不存在、9000/5202 无监听、无 echo/进程、无锁。sysctl 为**生产值**（rmem/wmem 16MiB、tcp 4MiB、fq/bbr），**不改成 64MiB**。10:51:39–10:56:03 窗口 UBSAN/WARN/BUG/OOM=0，未跑数据面；08:53–09:01 两轮 smoke 有 **16 UBSAN + 2 WARN**（`tbt_tx.c:2570`、array/shift OOB），已记录。**`t52-arm64` 在 HOLD 期间仍于 10:44/10:51/10:52 部署/加载/echo，agent kill 后留 PPID 1 残留**（已被并行清理）；需确认 HOLD 生效、不再部署。生产服务 active；不合并/push。

**HND0 终态（`t52-arm64` 复查，2026-11-06）**：最后孤儿 `sleep 14400` PID 47739 已清；`lsmod` 无 tbtun、`/proc/net/tbtun` 不存在、9000/5202/10000 无监听、无 `tbt_echo`/`tbt_burst`/`flock`/`sleep`、无锁文件；HND0 **完全 clean**。`.65` 残留等 smoke 计划批准后清理；T5.2 HOLD；不合并/push。

| 项 | 冻结值 | 归属 |
|---|---|---|
| FEC wire 版本 | `TBT_FEC_CONV = 0x1001`；旧 ARQ 仍是 `TBT_CONV = 0x0005` | B |
| 记录头 | 40 字节，小端；`type`/`flags`/`share_index u16`/`n`/`m`/`epoch u16`/`block_id u64`/`seq u64`/`win_len u16`/`payload_len u16`/`conv u16`/`reserved u16`/`payload_crc32c u32`/`header_crc32c u32` | B |
| 控制帧 | ACK 16B；NACK 12B/项；FEC_CFG 32B；FEC_FBK 48B（含 32B bitmap）；PAD 0B | B |
| 载体位图 | 256 bit（32B），bit i = carrier i；新 FEC 不复用旧 `path_ack_mask` | B |
| 载体槽 | `TBT_MAX_CARRIERS_LIMIT = 256`；旧 ARQ 自己的 `TBT_LEGACY_MAX_CARRIERS = 16` | B |
| `max_carriers` | 默认 256，范围 1..256 | B |
| portblock | 默认 4096 个端口；256 carriers 的最小生产值 1024 | B |
| 缓冲预算 | 128 MiB/模块；每 carrier 实际 512 KiB 上沿；256 条推荐 64 MiB | B |
| poll | session 级 50 ms 合并；稳态不做每 carrier delayed poll | B |
| 1 share/carrier | `carrier = group*(n+m) + share_index`；`groups = carriers/(n+m)`，必须整除 | B |
| 加密 | ChaCha8，body-only；默认 `crypto=on`（`fec=on` 时）；`crypto=off` 可关 | C |
| 加密范围 | 只加密 FEC payload；40B FEC 头和 HTTP 头明文 | C |
| key 来源 | 每 session 随机 256-bit；HELLO v6 body 明文下发；`crypto_key=` 预共享覆盖 | C |
| 配置键 | `fec=` `mode=` `n=` `m=` `w=` `carriers=` `crypto=` `crypto_key=` | B/C |
| 流数 | v6 一隧道一 app stream；header 无 sid；多流留 v7 | B |
| 合并顺序 | A（w-window）→ B（w-fec-v6）→ C（w-crypto）→ D（w-sclc）→ E（w-hnd0）→ F（integration/T7） | F |

---

## 1. FEC v6 ABI

### 1.1 常量与类型集

```c
#define TBT_FEC_CONV            0x1001u
#define TBT_FEC_HDR_LEN         40
#define TBT_FEC_PAYLOAD_MAX     8192
#define TBT_FEC_REC_MAX         (TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX) /* 8232 */
#define TBT_FEC_MAX_SHARES      256
#define TBT_FEC_MAX_CARRIERS    256
#define TBT_FEC_MAX_GROUPS      256
#define TBT_FEC_MAX_W           1024
#define TBT_FEC_MAX_D           256
#define TBT_FEC_NACK_MAX_ITEMS  64   /* 64 * 12 = 768 B，单条 NACK 上限 */
```

记录类型：

| 值 | 名称 | 用途 |
|---:|---|---|
| `0x13` | OPEN | 旧控制帧，v6 沿用 |
| `0x14` | FIN | 旧控制帧，v6 沿用 |
| `0x15` | RST | 旧控制帧，v6 沿用 |
| `0x22` | HELLO | 旧控制帧，v6 body 扩展（见 §2.3） |
| `0x23` | PAD | 载体保活，`payload_len=0` |
| `0x30` | DATA | 数据 share |
| `0x31` | PARITY | 校验 share |
| `0x32` | ACK | 交付前沿 + credit |
| `0x33` | NACK | 缺失报告 |
| `0x34` | FEC_CFG | 码率参数 / seed / epoch |
| `0x35` | FEC_FBK | 丢包反馈 + 256-bit bitmap |

**不新增 `0x36`。** 新 FEC 类型集就是 `0x23` + `0x30..0x35`；HELLO/OPEN/FIN/RST 仍用旧值，靠 `conv=0x1001` 与新 body 语义区分。

### 1.2 40 字节 share header

所有多字节字段为**小端**。`struct tbt_fec_hdr` 是主机序结构；`tbt_fec_hdr_pack()` / `unpack()` 才是 wire 权威，禁止直接把结构体写到线上。

| 偏移 | 大小 | 字段 | 语义 |
|---:|---:|---|---|
| +0 | 1 | `type` | §1.1 的类型；未知类型 `-EPROTO` |
| +1 | 1 | `flags` | bit0 `LAST`；bit1..7 必须为 0 |
| +2 | 2 | `share_index` | `0..n+m-1`；数据 `0..n-1`，parity `n..n+m-1`；**按 u16 比较** |
| +4 | 1 | `n` | 每组数据 share 数，1..255 |
| +5 | 1 | `m` | 每组 parity share 数，1..255；`(u16)n+(u16)m <= 256` |
| +6 | 2 | `epoch` | 参数 epoch 的低 16 位；**本 cut 借用为「本块最后一个真实 share 的 payload_len，0=全满」**（短包恢复需要；rate epoch 未实现，见 §G2 偏差） |
| +8 | 8 | `block_id` | 组内 generation 号，从 0 单调；**不是 stream id，不编码 group** |
| +16 | 8 | `seq` | 全局数据包序号；各类型含义见 §1.4 |
| +24 | 2 | `win_len` | PARITY 覆盖的组内包数 L；DATA/控制帧为 0 |
| +26 | 2 | `payload_len` | DATA 实际字节数（**任意 DATA 可短，只有 `LAST` 表示流结束**）；PARITY 恒 S；控制帧 body 长度；PAD 0 |
| +28 | 2 | `conv` | 必须 `0x1001` |
| +30 | 2 | `reserved` | 必须 0 |
| +32 | 4 | `payload_crc32c` | 只覆盖 payload（`payload_len` 字节），CRC32C |
| +36 | 4 | `header_crc32c` | 覆盖 `+0..+35`（含 `payload_crc32c` 字段），CRC32C |

**为什么 header CRC 在 +36、payload CRC 在 +32。** 这是 B 已批准的布局：接收端先读 `conv/type/payload_len`，再校验 `header_crc32c`，最后校验 `payload_crc32c`。header CRC 覆盖 payload CRC 字段，防止攻击者改 CRC 字段本身。

### 1.3 CRC 与接收顺序

CRC32C（Castagnoli），语义与 Linux `crc32c()` 一致：

- `payload_crc32c = crc32c(0, payload, payload_len)`
- `header_crc32c  = crc32c(0, header_bytes_0_35, 36)`
- 自检向量：`crc32c(0, "123456789", 9) == 0xE3069283`

接收顺序：

1. `conv == 0x1001`；`type` 在已知集合；`payload_len` 在该类型的边界内（DATA 1..S；PARITY == S；控制帧 ≤ 8192；PAD 0）。
2. `header_crc32c` 校验 `+0..+35`。
3. `n/m/share_index/win_len/flags/reserved` 的语义校验；`(u16)n+(u16)m <= 256`；`share_index < (u16)n+(u16)m`。
4. `payload_crc32c` 校验 payload；DATA/PARITY 的 CRC 覆盖明文 payload（解密后校验，见 §2.4）。

失败处理分两类：

- **1–3 失败 = 字节流 framing 不可信**：`payload_len` 可能已损坏，无法安全跳到下一条记录；关闭该 carrier（`-EPROTO`），计数 `fec_proto_fail` / `fec_hdr_crc_fail`。绝不带着未知长度继续解析。
- **4 失败 = framing 可信、payload 损坏**：丢弃该 DATA/PARITY，计数 `fec_payload_crc_fail`，carrier 保持；不交付坏字节，等待重发/恢复。

任何情况下都不把坏字节交给应用。

### 1.4 `seq` / `block_id` / `win_len` / group 语义

**一隧道一 app stream。** v6 header 没有 sid；`block_id` 只标识 window/block，不做 stream demux。多流留 v7，不静默塞进 `block_id`。

```
groups G   = carriers / (n + m)          /* 必须整除；1 share/carrier */
group(c)   = c / (n + m)                 /* c = carrier 索引 */
share(c)   = c % (n + m)                 /* 必须等于 header.share_index */
```

- DATA：`seq` 是该包的全局序号；`block_id` 是该包所属组的 generation；`win_len=0`。接收端校验 `seq % G == group(carrier)`（条带化模型）。
- PARITY：`block_id` 是该组 generation；`seq` 是窗口**最后一个**全局 seq；`win_len=L` 是组内包数。覆盖的全局 seq 为
  `seq, seq-G, seq-2G, ..., seq-(L-1)G`。
- 单组模式（`mode=rlnc`，`G=1`）退化为连续窗口 `[seq-L+1, seq]`。
- ACK：`seq=next_deliver`；NACK：`seq=第一个缺失 seq`；FEC_CFG/FEC_FBK/PAD：`seq=0`。
- `block_id` 从 0 单调；epoch 切换不清零 `block_id` 与 `seq`。窗口外不可恢复时有界 reset，不重编号。

**末包。** DATA 的 `payload_len` 是实际应用字节数；最后一个 DATA 置 `flags.LAST`，`payload_len` 可为 1..S。PARITY 恒按 S 字节零填充后编码；接收端对末包按 `payload_len` 交付，剩余补零只参与解码。

### 1.5 控制帧 body

#### ACK（`0x32`，16 B）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 8 | `next_deliver`，已连续交付到的下一个包 |
| +8 | 4 | `credit_packets`，接收端还能收的包数 |
| +12 | 4 | `rtt_us`，0 表示未测 |

#### NACK（`0x33`，12 B/项）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 8 | `missing_seq` |
| +8 | 2 | `count`，从 `missing_seq` 起的连续缺失数 |
| +10 | 2 | `reserved=0` |

单条 NACK 最多 64 项（768 B）；更多缺失用多条 NACK。`payload_len` 必须是 12 的整数倍。

#### FEC_CFG（`0x34`，32 B）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 4 | `epoch`（全 32 位） |
| +4 | 2 | `n` |
| +6 | 2 | `m` |
| +8 | 2 | `w` |
| +10 | 2 | `k` |
| +12 | 1 | `d`（RLNC degree；SC-LDPC 可为 0） |
| +13 | 3 | `reserved[3]=0` |
| +16 | 8 | `seed`（与 session nonce 异或后的 PRNG seed） |
| +24 | 4 | `crc32c`，覆盖 `+0..+23` |
| +28 | 4 | `reserved=0` |

FEC_CFG 在 epoch 边界由发送端发出，重复 3 次，走任意 `part_len==0` 的 carrier。接收端按 `epoch` 建新窗口；旧 epoch 的窗口继续解码到滑出或超时。

#### FEC_FBK（`0x35`，48 B，含 32 B bitmap）

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 2 | `epoch` |
| +2 | 1 | `m_current` |
| +3 | 1 | `m_target` |
| +4 | 4 | `p_ewma_ppm`（百万分之一） |
| +8 | 2 | `expected_shares`（本 epoch 已发 share 数） |
| +10 | 2 | `received_shares`（接收端已收 share 数） |
| +12 | 4 | `reserved=0` |
| +16 | 32 | `carrier_bitmap[32]`；bit i = carrier i 本 epoch 有丢包 |

**48 = 16 + 32，不是 80。** 旧 `path_ack_mask` 是 u16，新 FEC 用 256-bit bitmap，不复用旧字段。

### 1.6 控制帧不得排在 partial DATA 后面

**实测 bug 背景**：控制帧排进同一条 carrier 的写队列，而该 carrier 正在写一个 partial record；partial 写不完，控制帧永远发不出去（near `fbk_tx=253`、far `fbk_rx=0`，最终 EOF）。

冻结规则（三条同时成立）：

1. 每条 carrier 维护 `part_len` / `part_off`。**只有 `part_len == 0` 时**才允许把控制帧追加到该 carrier 的字节流。
2. 若选中的 carrier `part_len > 0`，调度器改选另一条 `part_len == 0` 的 carrier；若全部 carrier 都有 partial，控制帧进入 session 级 control queue，等 `sk_write_space` / 50 ms poll 重试，计数 `ctl_blocked_by_partial`。**绝不把控制帧插进 partial record 的中间。**
3. 控制帧在 `part_len == 0` 时优先于新的 DATA/PARITY；同一 carrier 上先控制、后数据。接收端 parser 仍按 `40 + payload_len` 识别记录边界，作为兜底。

host 测试必须覆盖：所有 carrier `part_len>0` 时注入 FEC_FBK，断言它等待而不是插队；任一条 carrier 完成 record 后，控制帧完整发出；接收端字节流无交错。

### 1.7 `tbt_fec.h` 接口（B 的实现契约）

```c
#ifndef _TBT_FEC_H
#define _TBT_FEC_H

#include "tbt_uapi.h"

#define TBT_FEC_CONV            0x1001u
#define TBT_FEC_HDR_LEN         40
#define TBT_FEC_PAYLOAD_MAX     8192
#define TBT_FEC_MAX_SHARES      256
#define TBT_FEC_MAX_CARRIERS    256

struct tbt_fec_hdr {
    u8  type;
    u8  flags;
    u16 share_index;
    u8  n;
    u8  m;
    u16 epoch;
    u64 block_id;
    u64 seq;
    u16 win_len;
    u16 payload_len;
    u16 conv;
    u16 reserved;
    u32 payload_crc32c;
    u32 header_crc32c;
};

/* wire <-> host；pack 写小端；unpack 不做语义校验。 */
int tbt_fec_hdr_pack(const struct tbt_fec_hdr *h, u8 out[TBT_FEC_HDR_LEN]);
int tbt_fec_hdr_unpack(const u8 in[TBT_FEC_HDR_LEN], struct tbt_fec_hdr *h);

/* conv/type/长度/index/win_len/flags/reserved 的语义校验。
 * 返回 0 或 -EPROTO/-EINVAL；不读 payload。 */
int tbt_fec_hdr_check(const struct tbt_fec_hdr *h, u16 max_payload);

/* CRC32C，Linux crc32c() 语义；seed 通常为 0。 */
u32 tbt_fec_crc32c(u32 seed, const void *data, size_t len);

/* 填 payload_crc32c 与 header_crc32c；payload 可为 NULL（PAD/控制帧无 body 时）。 */
int tbt_fec_hdr_seal(struct tbt_fec_hdr *h, const u8 *payload);
/* 校验两个 CRC；返回 0 / -EBADMSG。 */
int tbt_fec_hdr_verify(const struct tbt_fec_hdr *h, const u8 *payload);

/* 256-bit bitmap；idx 0..255；越界返回 -ERANGE。 */
void tbt_fec_bitmap_set(u8 bm[32], u16 idx);
void tbt_fec_bitmap_clear(u8 bm[32], u16 idx);
bool tbt_fec_bitmap_test(const u8 bm[32], u16 idx);
u16  tbt_fec_bitmap_count(const u8 bm[32]);

/* 控制帧 pack/unpack；返回 body 长度或负 errno。 */
int tbt_fec_ack_pack(const struct tbt_fec_ack *a, u8 out[16]);
int tbt_fec_ack_unpack(const u8 in[16], struct tbt_fec_ack *a);
int tbt_fec_nack_pack(const struct tbt_fec_nack_item *it, u16 nr, u8 *out, size_t max);
int tbt_fec_nack_unpack(const u8 *in, size_t len, struct tbt_fec_nack_item *it, u16 max);
int tbt_fec_cfg_pack(const struct tbt_fec_cfg *c, u8 out[32]);
int tbt_fec_cfg_unpack(const u8 in[32], struct tbt_fec_cfg *c);
int tbt_fec_fbk_pack(const struct tbt_fec_fbk *f, u8 out[48]);
int tbt_fec_fbk_unpack(const u8 in[48], struct tbt_fec_fbk *f);

/* 中间码：RLNC / grouped。SC-LDPC 由 D 实现同一 ops 形状。 */
struct tbt_fec_rlnc *tbt_fec_rlnc_new(u16 n, u16 m, u16 k, u16 w,
                                      size_t s, u16 d, u64 nonce);
void tbt_fec_rlnc_free(struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_parity_count(const struct tbt_fec_rlnc *c, u32 L);
u32  tbt_fec_rlnc_coeffs(const struct tbt_fec_rlnc *c, u32 g, u32 r,
                         u32 *idx, u32 max);
int  tbt_fec_rlnc_encode(struct tbt_fec_rlnc *c, u8 *data[], u32 L, u8 *parity[]);
int  tbt_fec_rlnc_decode(struct tbt_fec_rlnc *c, u8 *data[], u8 *parity[],
                         const u8 *data_present, const u8 *parity_present, u32 L);
int  tbt_fec_rlnc_advance(struct tbt_fec_rlnc *c, int kind, u32 index);
u32  tbt_fec_rlnc_frontier(const struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_stuck_at(const struct tbt_fec_rlnc *c);
u32  tbt_fec_rlnc_recovered(const struct tbt_fec_rlnc *c);
int  tbt_fec_rlnc_is_known(const struct tbt_fec_rlnc *c, u32 index);

/* codec 抽象：B 的 RLNC 与 D 的 SC-LDPC 共用；host 测试只认这张表。 */
struct tbt_fec_ops {
    const char *name;
    u8 mode; /* 1=rlnc, 2=grouped, 3=sc-ldpc */
    void *(*new)(u16 n, u16 m, u16 k, u16 w, size_t s, u16 d, u64 nonce);
    void  (*free)(void *ctx);
    u32   (*parity_count)(const void *ctx, u32 L);
    u32   (*coeffs)(const void *ctx, u32 g, u32 r, u32 *idx, u32 max);
    int   (*encode)(void *ctx, u8 *data[], u32 L, u8 *parity[]);
    int   (*decode)(void *ctx, u8 *data[], u8 *parity[],
                    const u8 *data_present, const u8 *parity_present, u32 L);
    int   (*advance)(void *ctx, int kind, u32 index);
    u32   (*frontier)(const void *ctx);
    u32   (*stuck_at)(const void *ctx);
    u32   (*recovered)(const void *ctx);
    int   (*is_known)(const void *ctx, u32 index);
};

#endif /* _TBT_FEC_H */
```

契约：

- `data[]` 是 `L` 个 `S` 字节包；`parity[]` 是 `parity_count(L)` 个 `S` 字节行。
- `data_present` / `parity_present` 是位图（L 位 / parity 位数），1 = 已收到。
- `decode` 只填缺失包，返回恢复数；rank 不足返回 `-EAGAIN`，不静默输出错字节。
- `advance` 推进解码前沿；`kind` 语义由 codec 定义（RLNC：0=数据已知，1=数据缺失，2=parity 已知）；任何实现都不得让 frontier 越过未知包。
- `coeffs` 是 RLNC 专用；SC-LDPC 可返回 `-ENOSYS`。host 测试对 SC-LDPC 只断言 `decode` 恢复率。
- `tbt_fec_rlnc_decode` 与 `tbt_fec_rlnc_encode` 的 `L` 上限 `TBT_FEC_MAX_W`；`n+m <= 256` 必须按 u16 比较。

### 1.8 旧 ARQ 隔离：16 槽 vs 256 槽

| 常量 | 值 | 用途 |
|---|---:|---|
| `TBT_CONV` | `0x0005` | 旧 wire，**不变** |
| `TBT_MAX_CARRIERS_LIMIT` | **256** | 载体槽数组、新 FEC、256-bit bitmap |
| `TBT_LEGACY_MAX_CARRIERS` | **16** | 旧 ARQ：`path_ack_mask` u16、FBK 16 项、WINDOW 16 项、`path_gen[16]`、`path_end[16]`、FIN/RST mask |
| `TBT_FBK_MAX` / `TBT_WINDOW_BODY_LEN` | 仍按 16 算 | 旧 wire 的 body 边界，**不得**跟着 256 放大 |

规则：

- 旧会话 `n+m > 16` 在 FEC 数据面落地前拒绝：`tbt_cfg_sane()` 在 `fec=off` 时返回 `-EINVAL`，错误文本写明「legacy ARQ supports n+m<=16; set fec=on for 256 carriers」。
- `fec=on` 时 `n+m <= 256`，走新 FEC；旧 `path_ack_mask`、旧 FBK/WINDOW、旧 `path_gen/path_end` 不参与。
- `TBT_MAX_CARRIERS_LIMIT=256` 只给载体槽与新 FEC；旧 ARQ 的 16 槽结构必须改用 `TBT_LEGACY_MAX_CARRIERS`，否则 `path_ack_mask` 会被截断。
- `struct tbt_session.car[]` 扩到 256；旧 `path_lost_mask`（`unsigned long`，64 位）只服务旧 ARQ；新 FEC 用 `DECLARE_BITMAP(fec_lost_mask, 256)`。

---

## 2. ChaCha8 ABI（C 的实现契约）

### 2.1 `tbt_crypto.h`

```c
#ifndef _TBT_CRYPTO_H
#define _TBT_CRYPTO_H

#include "tbt_uapi.h"

#define TBT_CRYPTO_KEY_LEN    32   /* 256-bit */
#define TBT_CRYPTO_NONCE_LEN  8    /* Bernstein 布局，64-bit counter */
#define TBT_CRYPTO_BLOCK_LEN  64

struct tbt_crypto_ctx {
    u32 state[16];
    u64 counter;
    u8  nonce[TBT_CRYPTO_NONCE_LEN];
    bool ready;
};

/* key 可为 NULL（仅当 crypto=off）；nonce 8 字节；counter 初始块号。 */
int  tbt_crypto_init(struct tbt_crypto_ctx *c,
                     const u8 key[TBT_CRYPTO_KEY_LEN],
                     const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter);
void tbt_crypto_set_counter(struct tbt_crypto_ctx *c, u64 counter);
u64  tbt_crypto_get_counter(const struct tbt_crypto_ctx *c);

/* 原地 XOR，按块推进 counter；len 任意。 */
int tbt_crypto_xor(struct tbt_crypto_ctx *c, u8 *data, size_t len);

/* 指定 counter 的随机访问；不推进 c->counter。 */
int tbt_crypto_xor_at(struct tbt_crypto_ctx *c, u64 counter, u8 *data, size_t len);

/* body-only hook：只加密/解密 FEC payload；40B header 与 HTTP 头不动。
 * counter = (seq << 8) | (parity << 7) | (offset / 64)。 */
int tbt_crypto_body_xor(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                        u8 *payload, u16 len);

void tbt_crypto_wipe(struct tbt_crypto_ctx *c);

#endif /* _TBT_CRYPTO_H */
```

实现可在同一头文件增加 `struct tbt_crypto_pair { struct tbt_crypto_ctx tx, rx; }` 与 pair 初始化/擦除便利函数（C2 `33238ca` 已加）；这是 session 层便利结构，不改变上述函数契约、不改 wire。pair 必须保证 tx/rx nonce 不同；等 nonce 直接拒绝。

实现选择：**ChaCha8，64-bit counter + 64-bit nonce**（Bernstein eSTREAM 布局），public-domain 参考或 MIT tiny-chacha 改写均可；KAT 必须与 `review/chacha8-reference.md` §7 一致。若用 tiny-chacha 的 96-bit IV/32-bit counter，必须把 8B nonce 映射为 `00 00 00 00 || nonce` 并把 counter 扩到 64 位；32-bit counter 在 1G 下约 37 分钟回绕，**不允许**直接上线。

### 2.2 body-only 的确切范围

```
HTTP request/response headers  明文（QoS 需要）
HTTP chunk size line            明文
FEC 40B share header            明文（type/seq/share_index 供调度）
FEC payload（DATA/PARITY）      加密
```

- 加密顺序：TX 先算 `payload_crc32c`（明文），再 `tbt_crypto_body_xor()`；RX 先解密，再校验 `payload_crc32c`。
- 控制帧（ACK/NACK/FEC_CFG/FEC_FBK/PAD）**不加密**；它们不含应用字节，且需要在 FEC 头明文下调度。
- HTTP 头明文是硬要求：整条 TLS/全包加密会丢掉 HTTP 容器的 QoS 可见性。
- 职责边界（B 确认）：HELLO v6 body 的 key/nonce 由 C 的 session/crypto 层填写；FEC 层（B）只接收 tx/rx 两个 `tbt_crypto_ctx` 并调用 `tbt_crypto_body_xor`，不自己生成/交换 key/nonce。

### 2.3 key 来源与 HELLO v6 body

**已裁决（§9.1）**：目标合同写「FEC_CFG 明文下发 key」；B 的冻结基线把 key 放 HELLO 层，FEC_CFG 固定 32B，不新增 `0x36`。Main 维持 B 方案，记录为等价偏差（每会话明文 key、只混淆不认证）；B/C 不改 ABI。

HELLO v6 body（`type=0x22`；**实现用旧 32B 头 `conv=0x0005` + 66B/ver2 body**；T0 原写 `conv=0x1001`，见 §9 偏差记录；固定 66 B）：

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| +0 | 1 | `ver=2`（旧 v6 之前是 1） |
| +1 | 1 | `role`（2 near / 3 far） |
| +2 | 16 | `nonce`（session nonce，旧字段） |
| +18 | 2 | `payload_max` |
| +20 | 2 | `chunk_max` |
| +22 | 1 | `profile` |
| +23 | 1 | `crypto`：bit0 enabled，bit1 key_in_hello |
| +24 | 1 | `key_id`：0=预共享/无 key，1=key 在 +34 |
| +25 | 1 | `reserved=0` |
| +26 | 8 | `crypto_nonce[8]` |
| +34 | 32 | `key[32]`（key_id=1 时有效；否则全 0） |

key 规则：

- `crypto_key=<64 hex>` 配置在两端：`key_id=0`，HELLO 不带 key；两端必须一致，否则 `-EPROTO` 关闭 carrier。
- 未配置 `crypto_key=`：near 在 session 创建时生成 32B 随机 key，`key_id=1`，HELLO 明文下发；far 采用（TOFU），若 far 自己配了 key 则拒绝。
- `crypto=off`：`crypto=0`，key 字段全 0；接收端不初始化 cipher。
- key 在 HELLO 中明文传输：**只混淆、不认证**；不提供对抗被动监听的机密性，也不防主动攻击。需要安全时用 WireGuard。

**每方向 nonce（必须，2026-11-06 补）**：`crypto_nonce[8]` 不是 session 共享值。每一端为自己的 **TX 方向**生成独立的 8B 随机 nonce，写进自己的 HELLO v6 body `+26`；**RX 方向使用对端 HELLO 的 nonce**。`crypto_key=` 预共享时 key 共享，nonce 仍各自独立。session 维护 **tx/rx 两个 `tbt_crypto_ctx`**；禁止一个 ctx 同时 seal/open 两个方向。这样 near/far 的 `seq` 都从 0 开始时，相同 `(seq,parity,block)` 也不会复用同一段 keystream。该语义不新增字段、不改 ABI；`crypto_nonce` 字段本来就在 `+26`。

### 2.4 默认与配置

- `fec=on` 时 `crypto` 默认 **on**；`fec=off` 时默认 off（旧路径没有解密点）。
- `crypto=off` 显式关闭；`crypto=on` 且 `fec=off` 拒绝（`-EINVAL`）。
- 每 session 独立 key；**每方向独立 8B nonce**（near TX / far TX 各一个），session 维护 tx/rx 两个 ctx；`crypto_nonce` 在 HELLO v6 body `+26` 交换。epoch 切换不换 key/nonce；carrier 重建不换 key/nonce（session 级）；新 session/方向重启必须换 nonce。
- counter 空间：`(seq << 8) | (parity << 7) | (offset / 64)`；S=8192 时 offset/64 ∈ 0..127，bit7 区分 parity；`seq < 2^56` 时无回绕。**counter 本身不带方向位**，方向隔离完全靠 nonce；禁止两端共用同一 nonce。

### 2.5 KAT 与性能门

- TC1：128-bit 全零 key/IV，8 轮，首块 `e28a5fa4a67f8c5defed3e6fb7303486...`。
- 256-bit 向量：key `00..1f`、IV `a0..a7`、counter 0，首块 `9a1e949f0d71a3a307da58730300dea7...`。
- `host/test_crypto` 必须逐字节比对；失败即 gate 失败。
- `host/test_crypto` 必须覆盖：同一 key、两个不同 nonce、相同 `(seq,parity,block)` 的 keystream 不同；tx/rx 两个 ctx 不串用。
- HND0 参考数字：ChaCha8 648.6 MB/s、3.39 cycles/byte；1G 单核 19.3%。app 33.72M 时约 4.2 MB/s，CPU 约 0.65%。

---

## 3. carrier 256 ABI

### 3.1 `max_carriers` 与旧 ARQ 的分界

- `max_carriers` 模块参数：默认 **256**，范围 1..256；`tbt_core.c` 的 `module_param` 描述同步改。
- `TBT_MAX_CARRIERS_LIMIT = 256`：载体槽数组、新 FEC、256-bit bitmap。
- `TBT_LEGACY_MAX_CARRIERS = 16`：旧 ARQ 的 `path_ack_mask`/FBK/WINDOW/path_gen/path_end/FIN/RST mask。
- `fec=off` 且 `n+m > 16`：`tbt_cfg_sane()` 返回 `-EINVAL`；`fec=on` 且 `n+m > 256`：`-EINVAL`。
- `struct tbt_session.car[256]`（2 KiB 指针）；`peers` 输出缓冲按 256 行分配。

### 3.2 portblock

- 默认 **4096** 个端口（建议 `30000-34095`）；256 carriers 的最小生产值 **1024**（256 × 4 次重建余量）。
- `ports lo-hi` 仍是全局唯一位图；`portblock=` 每隧道字段继续解析/渲染，但数据面用全局块（现状，不改）。
- 约束：`1 <= lo <= hi <= 65535`；`hi-lo+1 >= 256` 警告，`< 1024` 在生产 profile 中拒绝。
- 端口耗尽计数：`port_exhaust` / `port_alloc_fail`；重建时先释放再重试。

### 3.3 128 MiB 缓冲预算

| 项 | 值 |
|---|---:|
| 每 carrier `SO_SNDBUF`/`SO_RCVBUF` 设置值 | 64 KiB |
| 内核翻倍后每 carrier 实际（snd+rcv） | 256 KiB |
| 256 carriers 合计 | 64 MiB |
| 模块级 `fec_buffer_budget` 默认 | 128 MiB（`134217728`） |
| 固定开销（tcp_sock/HTTP/work item/peers） | 约 1.3 MiB |
| 生产 `max_tunnels` | 1（2 vCPU/955 MiB） |

- `max_tunnels > 1` 时，`fec_buffer_budget` 按 tunnel 数均分；创建时拒绝超限（`-ENOSPC`）。
- 每 carrier 512 KiB 是上沿，不是每流保证；实际按 64 KiB 设置值 + 内核翻倍计算。
- 256 carriers 的 socket 总数 = `max_tunnels × 256`；procfs `stats` 暴露 `fec_rx_mem_kb`、`fec_tx_mem_kb`、`carriers_up`。

### 3.4 session 级 poll 合并

- 稳态：**一个** `sess->w_poll`，每 50 ms 遍历全部 carrier；不再为每条 carrier 排 `w_poll`。
- 每 carrier 保留 `w_rx` / `w_tx`（数据面），但 `w_poll` 只在 connect/retry 阶段使用；连接建立后由 session poll 驱动。
- 效果：256 条 carrier 的 5120 wakeups/s 降到 20/s；单条故障发现延迟上限 50 ms。
- `sk_write_space` 回调只 `schedule_work(&sess->fec_sched_work)`，不直接发送；`tbt_carrier_send(c, buf, len)` 签名不变，FEC 调度器是唯一调用者。
- 256 carriers 下 `sess->pump_lock` 是串行点；B 改成每 carrier 自己的 `w_tx`，共享队列无锁取任务。旧 ARQ 的 `pump_lock` 语义保持不变，直到 `fec=on` 切换。

### 3.5 256-bit bitmap

- `DECLARE_BITMAP(carrier_mask, 256)`（32 B）；`test_bit`/`bitmap_set`/`bitmap_clear`/`bitmap_weight`。
- FEC_FBK 的 `carrier_bitmap[32]`：bit i = carrier i 本 epoch 有丢包；接收端按 carrier 索引置位，发送端按位调 `m`。
- 旧 `path_ack_mask`（u16）只服务旧 ARQ；新 FEC 任何路径都不得把 256 位截成 16 位。
- 边界测试：bit 0、bit 255、`n+m=256` 时 `share_index=255` 合法；`share_index` 比较先转 `u16`。

### 3.6 1 share/carrier 映射

```
carriers = groups * (n + m)          /* 必须整除 */
group    = carrier_index / (n + m)
share    = carrier_index % (n + m)   /* == header.share_index */
```

- 生产：`carriers=256`、`groups=32`、`n+m=8`（如 n=4,m=4）或 `groups=1`、`n+m=256`（single）。
- 一条 carrier 只承载一个 share；不允许 `carriers < n+m` 的降级映射进入 v6（旧 ARQ 仍可）。
- carrier 替换保持索引不变；新 carrier 用同一 `group/share`，否则接收端按 `-EPROTO` 关闭。
- `share_index` 是 u16；`n+m=256` 时 `share_index` 最大 255，比较必须 `(u16)share_index < (u16)n + (u16)m`。

---

## 4. `/proc/net/tbtun/config` 新键

### 4.1 键、默认值、边界

| 键 | 值 | 默认 | 边界 / 规则 |
|---|---|---|---|
| `fec=` | `off` \| `on` | `off` | `on` 启用 v6 数据面；`off` 走旧 ARQ |
| `mode=` | `rlnc` \| `grouped` \| `sc-ldpc` | `rlnc` | 仅 `fec=on` 有效；`grouped` 需 `carriers % (n+m)==0` |
| `n=` | 十进制 | `4` | 1..255；`n+m<=256` |
| `m=` | 十进制 | `1` | `fec=on` 时 1..255；`fec=off` 时可为 0；`n+m<=256` |
| `w=` | 十进制 | `0`（auto） | 0 或 1..1024；`fec=on` 时 `w=0` → `2*n`（rlnc/grouped）；`sc-ldpc` 的 0 → 256 |
| `carriers=` | 十进制 | `0`（auto = n+m） | 1..256；`carriers % (n+m)==0`；`fec=on` 时 `carriers >= n+m` |
| `crypto=` | `off` \| `on` | `fec=on` 时 `on`；`fec=off` 时 `off` | `crypto=on` 且 `fec=off` → `-EINVAL` |
| `crypto_key=` | 64 hex 或 `-` | `-`（未设置） | 64 hex = 256-bit；`-` 清除；空值拒绝 |

补充：

- `w` 与 `k`：v6 的 `k` 由 `n` 决定（`k=n`，每组数据包数）；不新增 `k=` 键。`w=0` 时渲染 effective `2*n`；`carriers=0` 时渲染 effective `n+m`。
- `mode=grouped`：`groups = carriers/(n+m)`；`n/m/w` 是**每组**参数；生产 `groups=32`。
- `mode=rlnc`：`groups=1`；`carriers=n+m`（single 256 时 `n+m=256`）。
- `mode=sc-ldpc`：`n/m` 是每块数据/parity share 数；`w=0` → 256，显式只接受 256/512（v6.0）；L 固定 8，D 若加 L 键必须先改 ABI。
- `fec=on` 且 `m=0`：拒绝（没有 parity 不是 FEC）；`n=1,m=1` 合法（最小对照）。
- `carriers=256` 时 `n+m` 必须是 8 的约数（32×8）或 256（single）；其他组合要求 `carriers % (n+m)==0`。
- `crypto_key=` 的 64 hex 只接受 `[0-9a-fA-F]`；奇数长度/非 hex 拒绝。

### 4.2 规范渲染顺序

`config` 读回仍是「每个标量键显式渲染 effective 值」。新键插在 `n m` 之后：

```
role listen [peer] [target] [peer] n m fec mode w carriers crypto crypto_key
dup profile keepalive drip_ms replay tunnel_rate max_inflate hole_budget_ms
rx_reorder_bytes tx_retain_bytes carrier_stall_ms portblock [nonce] [srcaddrs]
```

- 旧配置行不含新键，解析后按默认值渲染；`cat config > backup; cat backup > config` 仍逐字节回环。
- `crypto_key=-` 是「未设置」的规范拼写；实际 key 设置后渲染 64 hex（`/proc` 文件 0600 root，已是 root-only）。
- `fec=off` 时 `mode` 仍渲染（默认 `rlnc`），但数据面不读；`crypto` 必须 off。

### 4.3 校验顺序

`tbt_cfg_sane()` 在分配/建 session 之前：

1. `fec`/`mode` 值合法；`crypto` 与 `fec` 的组合合法。
2. `n>=1`；`fec=on` 时 `m>=1`；`(u16)n+(u16)m <= (fec?256:16)`。
3. `w==0 || (w>=n && w<=1024)`；`mode=sc-ldpc` 的显式 `w` ∈ {256,512}。
4. `carriers==0` → `n+m`；`carriers<=256`；`carriers % (n+m)==0`；`fec=on` 时 `carriers>=n+m`。
5. `crypto_key` 64 hex 或 `-`。
6. `max_carriers` 模块参数与 `max_tunnels` 预算检查。

任一失败返回 `-EINVAL`，整行不生效。

---

## 5. 分支、文件 ownership 与合并顺序

### 5.1 工作树与职责

| 工 | 分支 | 工作树 | 任务 | 文件/职责 |
|---|---|---|---|---|
| A `wan-overhaul2` | `w-window` | `/home/kunweiz/tbtun-worktrees/window` | T1 N=1 retx/控制面 | `tbt_tx.c`、`tbt_rx.c`、`tbt_carrier.c`（parser/poll/stall）、`tbt_procfs.c` 计数、QEMU A–K/adversarial |
| B `fec-v6` | `w-fec-v6` | `/home/kunweiz/tbtun-worktrees/fec-v6` | T2 FEC v6 + 256 carriers + 中间码 | `tbt_fec.h`、`tbt_fec.c`、`tbt_fec_tx.c`、`tbt_fec_rx.c`、`tbt_uapi.h`、`tbt_parse.*`、`tbt_procfs.c`、`tbt_session.c`、`tbt_carrier.c` 256 缩放、`tbt_tx.c`/`tbt_rx.c` 的 FEC 钩子 |
| C `crypto-v6` | `w-crypto` | `/home/kunweiz/tbtun-worktrees/crypto` | T3 ChaCha8 | `tbt_crypto.h/.c`、`host/test_crypto.c`、`tbt_fec.c` 的 body hook、HELLO key、`tbt_parse.*` 的 crypto 键、`tbt_procfs.c` |
| D `fec-core2` | `w-sclc`（当前 worktree `w-fec`，合并前统一命名） | `/home/kunweiz/tbtun-worktrees/fec` | T4 SC-LDPC | `review/fec-prototype/sc_ldpc/*`、`tbt_sclDPC.c`/`tbt_fec_ops` 实现；不先写内核 |
| E `wan-deploy` | `w-hnd0` | `/home/kunweiz/tbtun-worktrees/hnd0` | T5/T6 HND0 | 部署/回滚脚本、HND0 证据、`review/hnd0-v6/`；产品码只在必要时改 |
| F `goal-integrator` | `master` | `/home/kunweiz/Desktop/vibe/tcp-brutal` | T0/T7 | 本文档、合并顺序、冲突裁决、最终门禁 |

### 5.2 冲突文件与合并顺序

**合并列车（用户裁决，2026-11-06 更新）：`w-window` → `w-fec-v6`（单组 raw/http，fc5ac01）→ `w-fec-grouped`（32×8，从 fc5ac01）→ `w-crypto` → `w-sclc` → `w-hnd0` → master（F/T7）。**
每一步只在 master 上合并前一等的 head；不允许跨序合并。D 的当前 worktree 仍是 `w-fec`，合并前改名为 `w-sclc` 或由 F 记录等价映射；不得因此改变 D 的提交内容。
**`w-window` 已合入 master `c508a10`（2026-11-06；tested 0888ce4，ko a6e459f6/srcversion 711422E4；merge 后 `make check` 绿）。`w-fec-v6` 已在 `fc5ac01`（单组 raw/http）；grouped 由 `w-fec-grouped` 从 fc5ac01 独占，避免同 worktree 冲突；T2 parent 等 grouped。**

**T2 grouped ownership（2026-11-06）**：`w-fec-v6` 保持 `fc5ac01` 单组不动；`w-fec-grouped` 从 `fc5ac01` 新建，独占 32×8 grouped 数据面（组重组/ACK/FBK/HTTP、host 32×8、QEMU 小 grouped）；合并顺序 `w-fec-v6 → w-fec-grouped → integration`。T5.2 等 T2 grouped。

**Grouped WIP 交接**：B 已把未提交 grouped 改动导出为 `/home/kunweiz/tbtun-worktrees/fec-grouped-wip-fc5ac01.patch`（`git apply --check` OK；含 grouped TX/RX 重构、`n_car=cfg.carriers` 修复、probe N；**未验证、带调试打印**），由 `fec-grouped` 接手；`w-fec-v6` 保持 `fc5ac01` clean。`/tmp` 100% 满，fec-grouped 应使用 `/home` 下独立 `TBTUN_BUILD`，不要依赖 /tmp。

**空间阻塞（2026-11-06）**：`/tmp` 100% 满（31G/32G），`/home` btrfs data 94%、metadata 2.71/3.00GiB；正常 `git add`/rename 报 `unable to write new index file`/ENOSPC。`space-clean` 只清我们自己的陈旧 /tmp/缓存（不删 `/tmp/cy-*`、active worktree/tracked 文件）；docs 暂用 `write-tree`+`commit-tree`+直接写 ref 保持；T2 merge 等空间恢复后再做。

| 文件 | 冲突方 | 裁决 |
|---|---|---|
| `tbt_tx.c` | A（T1 修复） vs B（FEC packetizer/scheduler） | **A 先合**。B rebase 到 A 的 head；A 的旧 ARQ 行保留，B 只在 `fec=on` 分支替换数据面。冲突时 A 的旧路径行为优先，B 的 FEC 钩子行次之；integrator 裁决。 |
| `tbt_rx.c` | A（hole/retx） vs B（FEC rx） | 同上：A 先合；B 加 `tbt_fec_rx.c`，旧 `tbt_rx.c` 保留；`conv` 分流。 |
| `tbt_carrier.c` | A（parser/poll/stall） vs B（256 缩放/poll 合并） | A 先合；B 在其上做 256 缩放。B 必须保留 A 的 parser 自旋修复、raw 空闲 stall 重置、buffer 前置设置。 |
| `tbt_uapi.h` | B（v6 常量/类型） vs A（计数） | B 拥有 v6 常量；A 不新增 wire 常量。`TBT_MAX_CARRIERS_LIMIT=256` 与 `TBT_LEGACY_MAX_CARRIERS=16` 由 B 一次改完。 |
| `tbt_parse.c/h` | B（FEC 键） vs C（crypto 键） | B 先合；C rebase 后加 `crypto=`/`crypto_key=`。`struct tbt_cfg` 字段由 B 加 FEC 部分，C 加 crypto 部分。 |
| `tbt_procfs.c` | B vs C | B 先合 FEC 计数/配置；C 加 crypto 状态。 |
| `tbt_session.c` | B（car[]/预算） vs C（key 生成） | B 先合；C 在 session 创建处生成 key。 |
| `tbt_core.c` | B（max_carriers 默认） | B 独占。 |
| `Makefile` | B/C/D | B 加 `tbt_fec.o`；C 加 `tbt_crypto.o`；D 加 `tbt_sclDPC.o`；各自 rebase，SRCS/obj-m 同步。 |
| `DESIGN.md` | 所有人 | 各分支只加自己章节；F 在 T7 统一对齐，避免同段冲突。 |

规则：

- 每个工只提交自己的分支；**不合并、不 push**。F 负责 master 合并。
- 允许在自己分支 `git rebase`；禁止改写 master/他人分支。
- A 的 T1 必须先合，因为 B 的 256 缩放会碰同一批 carrier/tx/rx 行；A 未合前 B 只做 `tbt_fec.*`/parser/procfs，不动 `tbt_tx.c`/`tbt_rx.c`。
- B 的 v6 host 工作不被 A 阻塞：B 可以先写 `tbt_fec.c/h` + host 测试；数据面钩子等 A 合入后再接。
- C 依赖 B 的 `tbt_fec.c` body hook 与 HELLO body；B 的 ABI 冻结后 C 即可并行。
- D 依赖 B 的 `tbt_fec_ops`；D 的 host 原型与 B 的内核工作并行，不互相阻塞。

---

## 6. 门禁 G0–G7

所有门禁都要求：命令退出码 0、证据落盘、失败不静默。证据目录以主仓库 `review/` 为准；worktree 的证据在合并时带过来。

### G0 — T0 接口冻结（F）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`git status --short`；`git diff --stat` |
| 证据 | `review/goal-v6-interfaces.md`；`make check` 日志；`git show --stat HEAD` |
| 通过 | `make check` 0；`tbtun/` 无改动（docs only）；B 书面确认 40B/控制帧；本文档 commit 到 master |
| 失败 | 任何 `tbtun/` diff、接口未确认、`make check` 非 0 |

### G1 — T1 N=1 retx/控制面（A）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 480`；`bash adversarial/run.sh`；N=1 慢载体脚本 |
| 证据 | `review/final-qemu/`、`review/final-bench/`、`adversarial/results.txt`、`review/wan-experiment/t1-n1-auto/`、N=1 原始日志 |
| 通过 | N=1 慢载体 `rc=0`；far `holes_unrecoverable=0`；`fbk_rx>0`；`carrier_lost=0`；`raw lost=0`；**byte-exact = sink 读满 16MiB + EOF**（iperf3 只作吞吐/信息）；lab `W≈136–250 KB`，HND0 按 `W≈k×BDP`（203ms×17M→~862KB，1MiB cap 下 ~750KB）判，不用 lab 绝对值；A–K 11/11、0 skip；adversarial 91/91、0 failed/0 skipped；host/SAN/lint 全绿 |
| 失败 | 任一计数不符、skip、crash、hang |

**G1 现状（挂起，HND0 N=1 auto 根因已定位）**：A `w-window 3f4f1f0`（模块同 c672843）定位：① W 被 `rx_reorder_bytes=262144` 钳住，不是公式；near `bdp_bytes=375286`、`k=2` → 750572，实测 10.3 Mbit/s = 262144/192.7ms；far `W=121–125KB` 是反方向控制流。重跑：两端 `rx_reorder_bytes=1048576`、`tx_window_bytes=0`、`hole_budget_ms=0`、`carrier_stall_ms=15000`、`profile=raw`、`debug=2`，`W=min(750KB,1MiB)=750KB`，17 Mbit/s 可行。② near `holes_unrecoverable=1` 是 sender-side `tbt_tx_stall_check` 杀掉被 reaped 的 iperf3 控制流（`rx_max_stall_us=0`；数据流 `kill=5` 是 app close），不是 rx hole；控制流 `tx_ack` 为何停 >1s 待 debug 打印。RST 用例已修（near 15/15；测试 bug），lint 16/16+14/14，adversarial 全套仍在跑。T1 未绿，**不合并 `w-window`**。

**HND0 验收口径**：`W≈k×BDP`（203ms×17M→~862KB；1MiB cap 下 ~750KB），不用 lab 的 136–250KB 绝对值；通过条件仍是 `rc=0`、far `holes=0`、`fbk_rx>0`、`carrier_lost=0`、`raw lost=0`。

**重跑状态（1MiB，E `cb5771e`）**：rc=1；bulk 16MiB/3s（44.7 Mbit/s sender）、far `rx_ack=16777253`、`holes=0`、`lost=0`；`W=1MiB` cap、`bdp=1.17–1.26MB`、`k=2`；但 near `TXSTALLKILL sid=…760 ack=171 txseq=172 age=3.17s`，far 控制流 `tgt_state=7`、2.65MB 未交付；near c6 `gate_fail=314/depth_fail=67/time_fail=35`。**W 修复对 bulk 生效，剩余阻塞是控制流 stall**；A/`hnd0-n1b` 诊断中。T1 未绿，`w-window` 不合并。

**Adversarial 状态**：A 报告 91/91 cases pass；full suite 90/1，仅 `pub.writer_drain_1g` 在 host load 3.3–4.7 下超时；isolated writer 4/4 绿；A 正在安静主机重跑全套。RST 是 fake far parser 的 test bug（已修 3f4f1f0），lint 16/16+14/14。

**T1 后续**：`hnd0-n1b` 已 stand down，不重复 1MiB 跑；A 在 full adversarial 后诊断控制流 stall。T1 未绿，`w-window` 不合并。

**控制流 stall 机制（`hnd0-n1b` 诊断，A 修复中）**：`tbt_tx.c:tbt_tx_stall_check()` 的 `budget = hole_budget_ms ? hole_budget_ms : 1000ms`，再取 `max(budget, 2.5×RTO)`；本次 `hole_budget_ms=0` → 1s（RTO=200ms，floor=0.5s），`carrier_stall_ms=15000` 不参与。iperf3 控制流第 172 字节在 far 目标侧 3s 未被读走，`tx_ack` 停在 171，3.17s 后被杀；kill 发 RST → HND0 iperf3 server 关闭控制 socket、停止读数据 socket → far 数据流残留 `rxq=2.65MB`、`tgt_state=7`，target-close 未传回 near。A 修复方向：auto 用 `carrier_stall_ms`/5×RTO；target CLOSE 主动拆流/回传 close；修后重跑 HND0 N=1 auto。证据缺口：`t1-n1-auto/` 缺 `/proc/net/tbtun/config` dump；下次复跑补 config/ports 快照。

**控制流 stall 根因（A，`cb5771e`，覆盖上段的“修复方向”）**：不是 FBK/远端口 hole，是 per-carrier peer-window gate 的跨流饥饿。near 控制流 172B，far `uack=171`；kill 时 carrier window 满（`retx wndshort inflight=785496 wnd=785248`），pump gate `(queued-una) >= snd_wnd` 是 per-carrier FIFO；bulk 16.9MB backlog/1MiB window 每次 window 开启先吃满，控制流 1B frame 停在 session `tx_q` 尾部；`sends=0` 使 retx 跳过、无 HOLE copy；`tx_stall_check` 在 `max(1s,2.5×RTO)=1s` 触发 `TXSTALLKILL ack=171 txseq=172 age=3.17s`。后果：near 控制 RST 关 far iperf3 control target → iperf3 abort → far data `tgt_state=7`/2.65MB 未交付 → far 停 ack bulk → near data 也 stall（`kill=4`）。far rx hole 不是触发（`rx_max_stall_us=388ms<1s`、far `holes=0`）；bulk repair 正常（`retx_frames=26`、`hole=2`、far `rx_ack=16.7MB`）。修复：`tbt_tx_pump` 的 peer-window gate 对 `tx_seq-tx_ack <= 2*TBT_PAYLOAD_MAX` 的小 backlog DATA frame 豁免，保持流内顺序；更严格可两遍选择。A 未打补丁，先本地回归（bulk+control 同 carrier）再 HND0 auto 重跑；head 仍 `3f4f1f0`。

**代码路径确认（`hnd0-n1b`）**：`tbt_tx_pump()` 的 carrier admission 对 `TBT_F_DATA` 走 `sk_stream_memory_free(c->sk)`（只有 FBK 豁免）；控制流 1B DATA frame 与 bulk 共用同一 peer-window/sndbuf 门；`tbt_tx_window_ok()` 本身通过（`172 <= 171 + 1MiB`），饥饿点在 carrier send-memory 门，不在 tunnel 窗口公式。A 的小 backlog DATA 豁免正对此段。重跑证据放 `review/wan-experiment/t1-n1-auto-rx1m/`（不覆盖 E 的 `t1-n1-auto/`），补 `config`/`ports` 快照和两条 stream 的 per-stream 行。

**A 补丁（`w-window f8023a2`）**：① cross-stream 小 backlog DATA 豁免（`tx_seq-tx_ack <= 2*TBT_PAYLOAD_MAX`，peer-window 与 send-memory 两门）；② `hole_budget_ms=0` 的 sender stall budget 改为 `max(5s, 5×RTO, carrier_stall_ms)`（显式 `hole_budget_ms` 保持原义）；③ target close（EPIPE/ECONNRESET/ESHUTDOWN）主动 abort/传播，`-ENOTCONN` 仍 transient。本地 build/make check/near 15/15/A–K 11/11/lint 绿；完整 adversarial 待 HND0 rerun 后安静跑。**module sha 待确认**：worktree `tbtun.ko` sha256=`686797c5…`（07:22，早于 f8023a2 的 07:26），与用户报的 `3edf84e0` 不一致；`hnd0-n1b` 必须用 A 确认的 f8023a2 构建，不能用 stale ko。

**重建/模块确认（A + `hnd0-n1b`）**：`f8023a2` 干净重建确定性 sha256=`686797c5…`，路径 `/home/kunweiz/tbtun-worktrees/window/tbtun.ko`，vermagic `7.2.9-1-MANJARO SMP preempt mod_unload`，version 0.0.1；`hnd0-n1b` 从 `git archive f8023a2` 另路径重建得 `d5fcce40…`，`srcversion=475868A5` 与 686797c5 相同，证明源码同一。`3edf84e0` 是 A 的中间 cut（仍会在 `-ENOTCONN` 杀 delivery），**不用于 rerun**。**模块校验口径**：near 7.2.9 用 `686797c5`（或同源重建）；far HND0 7.0.0-42 用**同一份 f8023a2 源码**在 HND0 构建，记录 far ko sha256；两端 `srcversion` 必须一致（源码同一性），ko sha 因 vermagic/构建路径不同而不同。provenance：near 15/15 与两次 A–K 都在 `3edf84e0` 上；最终 `686797c5` 的 A–K 尚未跑，等 HND0 rx1m 后安静跑。

**用户裁决（模块 invariant）**：ko sha 依赖 build-path/kernel；`3edf84e0` 不是 f8023a2 的确定性 sha。Invariant = **same source `f8023a2` + per-end build + `srcversion=475868A5` + per-end sha 记录**；`hnd0-n1b` 按此跑 N=1 auto。`window-verify` 负责本机 A–K/完整 adversarial。

**f8023a2 rx1m 结果（`hnd0-n1b a2a37ed`）**：控制流 starvation 修复生效——`rc=0`、无 `TXSTALLKILL`、far `fbk_rx=1089`、`holes=0`、`carrier_lost=0`、`lost=0`；near `tx_window=1MiB` cap、`bdp=1321474`、`k=200`；far `tx_window=210880`、`bdp=99902`；CPU near 9.66%、far 21.71%。**但 16MiB 未逐字节**：sender 16,777,216 B，receiver 13,500,416 B（80.47%）；near `submit_bytes=15,996,575`（780,641 B 留在 public socket 接收队列未 capture）、`tx=14,007,990`、`wire=14,118,309`、流结束 `txq=1,113,317`；far `rx_delivered=13,614,558`；数据流 sid …905 close 时 `state=5 kill=5`、`rxq=24576`。机制：public socket close 直接 kill stream，未先排空 capture/txq，FIN/RST 越过队列数据。**close-drain 是新的/遗留阻塞，G1 不能判绿**；A 诊断修复。

**close-drain 修复归属**：已开 `window-close` 修 public close 的 drain 顺序（先排空 capture/txq，再 FIN/RST）；T1 未绿，不合并。

**close-drain 修复（`w-window ddef988`，srcversion `62E75E1C`）**：FIN 只在 capture 全部 submit 且 `tx_ack` 覆盖 `tx_seq` 后发出；`close_since_ns` + `tbt_tx_close_check()` 对无进展的 FIN 等待有界 reset；FIN 已写出但对端 FIN 缺失 → `max(60s, 2×budget)` 后 detach（不 reset）；对端 FIN 已到但 delivery 队列不缩 → `tbt_rx_deliver_stall_check()` 有界 reset；FIN order gate 改为 `state != DEAD`。回归：adversarial `closedrain` PASS、`closedrain_timeout`；make check/lint PASS；A–K/near/full adversarial/SAN 由 `window-verify` 补跑。T1 等 sink HND0 重跑。

**验收口径修正（2026-11-06）**：iperf3 server 在 `TEST_END` 主动 close 数据 socket，receiver 13,500,416 B **不是 tunnel 丢数据**；T1 的 byte-exact 改用 **sink 读满 16MiB + EOF**，iperf3 只作吞吐/信息。close-drain 仍是真 bug（near FIN 早于 capture/txq 排空），`window-close`/A3 继续修；T1 仍未绿。

**window-verify 重建（ddef988）**：clean rebuild HEAD `ddef988f…`，ko `/home/kunweiz/tbtun-worktrees/window/tbtun.ko` sha256 `c476bd48…`，size 6,071,368，`srcversion=62E75E1CE05E5C4887C9AAA`，vermagic `7.2.9-1-MANJARO`。A–K 运行中（`bg_670`）；随后 near 15/15、full adversarial（期望 91/91）、`host-test-san`；`writer_1g` 超时隔离复跑并分别报告。不合并/push，不碰 HND0。

**window-verify 进度（ddef988）**：A–K **11/11 PASS、0 skip**；near group **15/15 PASS**，quiet host 再重复 3 次仍 15/15；`host-test-san` rc=0（parse 385、fuzz 7,550,734、http 249、http_fuzz 4,078,276）。full adversarial run #1：**92 PASS / 1 FAIL / 0 SKIP**；唯一 FAIL `near.fin_eagain`（`fin_seen=0`、`tx_ack=3,744,115` vs `wire=4,037,236`、`rfail=1`），发生在 `hnd0-sink` 同时占用 HND0 的窗口；isolated near 4/4 PASS。quiet host full 18-group rerun `bg_716` 进行中；**G1 等 rerun 结果**，若复现则按真 bug 处理。

**T1 sink 门禁 PASS（`hnd0-n1b b3b33b2`）**：`SINK_BULK bytes=16777216 eof=1`；sender `SENDCLOSE_OK bytes=16777216 txq_after=1113052`（close 时队列非空）；near `fin_seq=16777216 fin_w=1 rst_w=0 rxf=1/1`、live `txq=0`；far `rx_delivered=16777216`、`deliver_timeouts=0`、`close_timeouts=0`、`close_detaches=0`、live `txq=0`。FIN 时序：near `fin_q=1 fin_w=0 txq=1036242` → drain 到 `txq=0` → `fin_w=1` → `rxf=1/1` detach；全程无 RST。两端同源 ddef988、`srcversion=62E75E1C`；CPU near 10.57%、far 27.93%。iperf3 `rc=0`，receiver 11,796,480 B 是 TEST_END close，不作 byte-exact。**T1 sink 门禁 PASS；G1 仍等 `window-verify` 的 A–K/near/full adversarial/SAN 才能判绿。** `hnd0-sink` 不要重复跑/覆盖该证据目录。

**残余 accept wake race（`hnd0-sink` 独立复跑，ddef988）**：near HELLO 55B 写出、far 内核已收，但 far `rx_work_runs` 33s 不跑、sink 从未 accept；near `TXSTALLKILL ack=0 txseq=1114059 age=15.0s`、`fbk_rx=0`、`tx_ack=0`、`lost=1 replaced=1`；far `rx_read_bytes=55`、`c3 up=0 lost=2`、`fbk_tx=0`。同源 ddef988 的 hnd0-n1b 成功是**非必现**；符合 accept 侧 `sk_data_ready`/HELLO wake 竞态。A3 修 accept 侧唤醒；T1 需 **N=1 连续 5 次 5/5 sink 16MiB** 才算绿。

**accept-wake 修复（`w-window 4c58b0e`，srcversion `10C45DCB`）**：`tbt_car_schedule_rx()` 统一接收唤醒；`schedule_work()` 失败（work 已 pending/in progress）置 `rx_again`，`tbt_car_rx_work` 入口清位、返回前 `atomic_xchg` 再武装；poll/accept/connect/resume 全走同一 helper。本地 closedrain **5/5 PASS**、`closedrain_timeout` 绿、make check 全绿、lint 16/16+14/14。`hnd0-n1b` 跑 HND0 sink 5x；`window-verify` hold QEMU 等 5x 后跑新 head A–K/full/SAN。

**验证状态（4c58b0e）**：`window-verify` 在 full rerun `bg_716` 的 public group 时发现 A3 的 4c58b0e 已覆盖 worktree ko（`c14bafee…`，srcversion `10C45DCB`），为避免跨 group 混用两个 ko，已 kill（仅 public 开始，`/tmp/a2-adv-full2-ddef988.log` 3 行），无 QEMU 残留。ddef988 结果全部标记 **superseded**（保留在 `~/.cache/tbtun-build/a2-evidence/ddef988/`：A–K 11/11、near 15/15×4、SAN 绿、full #1 92P/1F `near.fin_eagain` load-window）。现在 hold 所有本地 QEMU，等 hnd0-n1b 的 HND0 5x 完成后 clean-rebuild 4c58b0e，再跑 A–K/near/full/SAN。

**4c58b0e clean rebuild（`window-verify`）**：`make clean` + full compile 08:52:25→08:52:33（gcc 16.2.1）；HEAD `4c58b0e…`，ko `/home/kunweiz/tbtun-worktrees/window/tbtun.ko` sha256 `c14bafee…`，size 6,088,496，srcversion `10C45DCB…`，vermagic `7.2.9-1-MANJARO`。A–K 运行中（`bg_748`，timeout 600）；随后 near group、full 18-group adversarial + crash repro、`host-test-san`。R11 已记入报告。

**4c58b0e A–K/near（`window-verify`）**：A–K run #1 = **10/11**，仅 probe J 失败（1% loss/2% reorder 下 byte-exact 未完成，rc=1；failed stream `rst_w=1`、`live_bytes=2,276,044`、`retx=34`、`holes_unrecoverable=0`）；isolated J **PASS**（rc=0，16.98s）；A–K run #2 = **11/11 PASS、0 SKIP**（rc=0，207.97s）；near group **15/15 PASS**（`near.fin_eagain`/`rst_eagain` PASS）。J 为 **flaky**，非确定性 4c58b0e 失败；证据 `/tmp/a2-AK-4c58b0e.log`、`/tmp/a2-J-4c58b0e-1.log`、`/tmp/a2-AK-4c58b0e-run2.log`。full 18-group adversarial + crash repro `bg_751` 运行中；SAN 随后。**G1 等 full/SAN 与 flake 稳定性结论**。

**4c58b0e 本地终局（`window-verify`）**：A–K run #1 10/11（仅 J flake）、run #2 11/11、run #3 11/11；isolated J **4/4 PASS**（1 + 3 repeats）；near **15/15 PASS**；full adversarial **93 PASS / 0 FAIL / 0 SKIP**（282.48s；`pub.writer_drain_1g` 1024MiB/6.27s、`pub.closedrain`、`pub.closedrain_timeout`、`ctl.close_flush` 全 PASS）；`host-test-san` rc=0（parse 385、fuzz 7,550,734、http 249、http_fuzz 4,078,276）。J flake 为 sequence-dependent（full 1/3、isolated 0/4），非确定性失败。证据 `~/.cache/tbtun-build/a2-evidence/4c58b0e/`（AK run1/2/3、J 1/rep1-3、near/full、results.txt、SAN、build3）。**G1 本地套件通过；T1 待 Main 最终接受；R11 残余不阻塞 loaded-once gate；不合并。**

**G1 状态（conditional）**：J flake 1/3（full A–K）不能直接判绿；A3 诊断 close-drain/stall vs probe timing；目标 full A–K **3x 11/11** 或 J full **5/5**。T1 **conditional**，不合并。**合并前确认**：`w-window` 工作树有 tracked 删除 `review/dkms-qemu/artifacts/guest-tbtun.ko.sha256`（既有，c6ac615 加入；当前未提交）；合并前要么恢复，要么作为有意删除单独提交，不能静默带进 merge。

**T1 本地证据（`w-window 8980c03`）**：`review/wan-experiment/t1-4c58b0e-local.md` + `t1-4c58b0e-local-results.txt`（93 行 `ADV_RESULT`）；`git diff 4c58b0e..8980c03 -- tbtun Makefile scripts host adversarial tests` 为空，ko `c14bafee…` / srcversion `10C45DCB` 仍有效；大日志留 `~/.cache/tbtun-build/a2-evidence/4c58b0e/`；tracked 删除未动；不合并/push。T1 仍 **conditional**，等 A3 的 J 根因/判据（full A–K 3x 11/11 或 J full 5/5）。`window-verify` 追加 3 次 A–K（同 ko `c14bafee`）进行中，目标 3x 11/11。

**J flake 根因/修复（`w-window 0888ce4`，A3）**：probe J 1/3 失败是 **product**（receiver auto hole floor 1s 太紧），不是 probe 时序。失败签名：far `kill=4`（`TBT_ST_HOLE`）、`holes_unrecoverable=1`、`rx_max_stall_us=1,005,262`、`rst_w=1`；near `una_stall_us=998,975`（carrier `snd_una` 卡 ~1s，TCP RTO backoff 200+400+800ms 未走完）、`retx_slow=32`；far `ooo_frames=151`；close/deliver timeouts=0、`close_detaches=0`（close-drain 未参与）。auto hole budget 原 `max(1s,5×SRTT)`，sender 修复窗口 `max(5s,5×RTO)`，1s 短于 carrier RTO backoff + one-way。修复：auto floor **2s**（`max(2s,5×SRTT)`）；显式 `hole_budget_ms` 保持原义（probe H 500ms）；probe G auto 上限 2000ms；DESIGN §4.6。`make check` 绿；ko `a6e459f6…`、srcversion `711422E4…`。**target 改为 0888ce4**；`window-verify` 需独占 QEMU（shared initramfs/19101）跑 full A–K 3x / J 5x。tracked 删除已 `git checkout --` 恢复，w-window 无 tracked 未提交改动（仅 untracked benches）。T1 未绿，不合并。

**0888ce4 复验计划**：`window-verify` 独占 QEMU 跑 full A–K **3x**、J **5x**、near、full adversarial、SAN；`hnd0-5x` 等 QEMU 空闲后跑 **sendclose 5x on 0888ce4**（per-end build：near ko `0e4ae50c…`、far ko `95aefe35…`，srcversion `711422E4`，archive `47d8652f…`；证据 `review/wan-experiment/t1-n1-auto-close-5x-0888ce4/`，不覆盖旧 5x）。注：任务提到的 ko `a6e459f6` 本机无现成文件，按 same-source per-end 构建、srcversion 匹配即可。T1 未绿，不合并。

**G1 本地套件 PASS（0888ce4，`window-verify`）**：clean rebuild ko `a6e459f6…`（size 6,088,504）、srcversion `711422E4`；full A–K **3x = 11/11 each，0 SKIP**（611s）；J isolated **5/5 PASS**；near **15/15 PASS**；full adversarial **93 PASS / 0 FAIL / 0 SKIP**（300s；writer_drain_1g/closedrain/closedrain_timeout/ctl.close_flush PASS）；`host-test-san` rc=0（parse 385、fuzz 7,550,734、http 249、http_fuzz 4,078,276）。证据 `~/.cache/tbtun-build/a2-evidence/0888ce4/`。QEMU 已释放。`window-final` 独立重跑到 `review/final-0888ce4/`；`hnd0-5x` 跑 0888ce4 sendclose 5x；两者完成后 T1 可最终接受。不合并。

**0888ce4 证据提交（`w-window 038d31d`）**：`review/wan-experiment/t1-0888ce4-local.md` + `t1-0888ce4-local-results.txt`（93 行 `ADV_RESULT`）；`git diff 0888ce4..038d31d -- tbtun tests Makefile scripts host adversarial` 为空，ko `a6e459f6…` / srcversion `711422E4` 仍有效；大日志留 `~/.cache/tbtun-build/a2-evidence/0888ce4/`；不合并/push。**tracked 删除 `review/dkms-qemu/artifacts/guest-tbtun.ko.sha256` 在 restore 后再次出现在工作树**（未提交，window-verify 未动）；合并前必须查清删除者并恢复/有意提交。

**tracked 删除根因/修复（`w-window 33db43d`）**：删除源 `review/dkms-qemu/run.sh` 的 `rm -rf "$artifacts"`；失败/中断的 dkms-qemu run 会删 tracked 证据且不重建 sha。修复：去掉 `rm -rf`，改为 `git clean -fdxq -- review/dkms-qemu/artifacts`（只清 untracked）+ `mkdir -p`；`sh -n` 通过。tracked 文件已恢复，`git status` 无 tracked 未提交改动（仅 untracked benches）。head `33db43d`（父 0888ce4，仅测试脚本），ko 仍 `a6e459f6…`/srcversion `711422E4`。T1 等 hnd0-5x 5x + window-final；不合并。

**线性历史确认（`window-verify`）**：`ddef988 → 4c58b0e → 8980c03 → 0888ce4 → 038d31d → 33db43d`；HEAD `33db43d` clean；`git diff --stat 0888ce4..33db43d -- tbtun tests Makefile scripts host adversarial` 为空（仅 `review/dkms-qemu/run.sh` + 两个 `review/wan-experiment/t1-0888ce4-local*`）；ko `a6e459f6…` / srcversion `711422E4` 仍有效，0888ce4 的计数对 33db43d 有效。`window-final`/`hnd0-5x` 拥有 QEMU slot。

**0888ce4 HND0 sendclose 5x PASS（`hnd0-5x f5f97ec`）**：per-end ko near `0e4ae50c…`/far `95aefe35…`，srcversion `711422E4`；每轮 sink 16,777,216/EOF、sendclose 16MiB、`txq_after` 非 0（810,213/1,113,317/842,981/630,254/401,143）、far `rx_delivered` 每轮 +16,777,216、near `fin_seq=16777216 fin_w=1 rst_w=0`、far `fin_w=1 rxf=1/1`、live `txq=0`、lost/carrier_lost=0、holes=0、timeouts=0、`close_detaches=0`、无 `TXSTALLKILL`；CPU near 10.09–10.41%、far 25.59–27.00%。证据 `review/wan-experiment/t1-n1-auto-close-5x-0888ce4/`。shutdown-WR 5x 仍在 4c58b0e（0888ce4 只改 rx hole floor，close 路径未变）；本轮 HND0 门禁按用户指定的 sendclose。**T1 只等 `window-final` 独立复跑。**

**T1/G1 完成（2026-11-06）**：T1 按 0888ce4 本地门禁（A–K 3x 11/11、J 5/5、near 15/15、full 93P/0F/0S、SAN）+ HND0 5x（sendclose 5/5、shutdown-WR 5/5）标完成；`window-final` 独立复跑是额外证据，不阻塞。`w-window 33db43d` 已合入 integration **`c508a10`**，merge 后 `make check` 绿；R11 独立 ticket。B 下一步 merge integration 接 FEC 数据面；T5.2 等 T2；T6 等用户 B/C。

**window-final 独立复跑（33db43d）**：A–K 3x = 11/11 each；J clean rerun PASS；J iso 5/5；near 15/15；full adversarial 在 stall group（10:06）。首轮 J 5x rep2 因 `/init: line 780: /bin/tc: not found` + 5.4M initramfs（其他 5.7M）失败，属 initramfs race，非产品；clean rerun 通过。**QEMU 锁冲突**：`fec-v6` 正跑 `tests/qemu/run.sh --ko .../fec-v6/tbtun.ko --probes A..L`（PID 3779318）**未持 `/tmp/tbtun-qemu.lock`**，而 window-final 持锁并使用共享 initramfs/19101；已让 B 停止并改用 `flock`/独立 `TBTUN_BUILD`。

**window-final full #1（33db43d）**：**92 PASS / 1 FAIL / 0 SKIP**；唯一 FAIL `pub.writer_drain_1g`（`sent=978,321,408/1,073,741,824`、`got=885,926,101`、240.01s、4.3 Mbps、`blocked=1`），发生在 `fec-v6` 并发无锁 A–K+L QEMU 期间；`window-verify` 的 quiet-host full 同组为 1024 MiB / 6.27s / 163 Mbps PASS。window-final 正在锁内隔离重跑 writer/writerdrain；A–K 3x、J clean+iso 5/5、near 15/15 与 window-verify 一致。若隔离通过，full #1 记环境失败。

**window-final 独立门禁完成（33db43d）**：10:17 clean rebuild 同 sha `a6e459f6…`/srcversion `711422E4`，make check rc=0；A–K **3x 11/11 each，0 skip，rc=0（598.6s）**；J A–K 内 3x PASS + iso **5/5**（首轮 5x 的 `/bin/tc: not found` + 5.4M initramfs 是 env-only）；near **15/15**；full run1 92P/1F（`pub.writer_drain_1g`，240.01s/4.3Mbps，fec-v6 并发 QEMU，环境失败），run2 **93P/0F/0S**（writer_1g 6.47s/158.3MiB/s）；隔离 writer **5P/0F/0S**、writerdrain **3P/0F/0S**（writer_1g 6.88s/148.8MiB/s）；`host-test-san` rc=0。证据 `review/final-0888ce4/`（gate.txt + 58 文件）。T1 local gate 有两份独立 green（window-verify + window-final，同 ko/srcversion）。**tracked 删除 `guest-tbtun.ko.sha256` 在 10:17 又出现**，已 checkout 恢复；run.sh 修复可能不是唯一删除源，需继续查。

**T1 最终证据入库（master `5ab995e`）**：`review/final-0888ce4/` 59 文件 / 2.7 MiB（gate.txt + A–K 3x、J 5x、near、full run1/run2、隔离 writer/writerdrain、SAN 日志）；master 现含完整独立门禁证据，gate.txt 的引用路径有效。

**dkms-qemu 自愈（`w-window bed07a1`）**：10:17 的 tracked 删除无法归因仓库脚本（无 dkms-qemu run、`git reflog` 09:35 后仅 commits、无 stash；唯一 scoped `git clean` 不删 tracked）；最可能是外部/手动 `rm` 或索引暂存删除。修复：`run.sh` 在 clean 前 `git checkout -- review/dkms-qemu/artifacts` 自愈，再 `git clean -fdxq` 只清 untracked。已合入 integration **`53a28e3`**，merge 后 `make check` 绿；ko 仍 `a6e459f6…`/srcversion `711422E4`。合并前仍验 `git status`。

**B 接 FEC 数据面（`w-fec-v6`）**：已 merge integration（`ed81a11` ← c508a10、`ee244fd` ← e6afd0a）；A–L **12 PASS / 0 skip**（`/tmp/qemu-fec-final2.log`，213s），probe L 为 FEC raw 单组 1 MiB 双向 byte-exact；使用 `flock /tmp/tbtun-qemu.lock` + 独立 `TBTUN_BUILD=/tmp/tbtun-fec-cache` + hostfwd 29101。当前 `-EOPNOTSUPP` 仅剩 `fec=on` 非 raw profile（`fec=on needs profile=raw in this build`），generic gate 已去。下一步 ACK `tx_ack` 修复后持锁复跑 L + 全量 A–L 并提交；不 push。**待确认**：T2 是否允许 raw-only，还是必须支持 HTTP 容器（ChaCha8 保留 HTTP 头）。

**5x 计划（`hnd0-n1b`）**：同一 A3 源码 per-end build、两端 srcversion 一致；5 轮独立 sink/sender；每轮 sink `bytes=16777216 eof=1`、far `rx_delivered=16777216`、near `fin_seq=16777216 fin_w=1`、`rst_w=0`、`deliver/close timeouts=0`、`close_detaches=0`、near/far `live txq=0`、无 `TXSTALLKILL`；证据 `review/wan-experiment/t1-n1-auto-rx1m-sink-5x/`，保留 b3b33b2 单次证据。失败也记录并继续，不掩盖。

**5x sendclose PASS（`hnd0-n1b 1d35fb5`）**：源码 4c58b0e，srcversion `10C45DCB`；near ko `c14bafee…`、far ko `0da13480…`。同一 tunnel 连续 5 轮 `sendclose`（写满后 `close()`，sink 每轮重启）：每轮 sink `bytes=16777216 eof=1`、far `rx_delivered` 每轮 +16,777,216、near `fin_seq=16777216 fin_w=1`、far `fin_w=1 rxf=1/1`、`rst_w=0`、`deliver/close timeouts=0`、`close_detaches=0`、near/far live `txq=0`、holes/lost/carrier_lost=0、far `up=1 lost=0 join_bad=0`、无 `TXSTALLKILL`；CPU near 8.9–10.1%、far 30.3–33.2%。证据 `review/wan-experiment/t1-n1-auto-close-5x/close/`。**shutdown-WR 变体仍由 `hnd0-5x` 独立跑**（`.../shutdown-wr/`）；若 5x 门禁要求 shutdown-WR，等其结果再判。

**5x shutdown-WR PASS（`hnd0-5x 08ed3b9`）**：源码 4c58b0e，srcversion `10C45DCB`；near ko `b82999b6…`、far ko `0da13480…`。模块一次加载，5 轮 sink 重启：每轮 sink 16,777,216 + EOF、sender 16MiB + `shutdown(SHUT_WR)` + EOF、far `rx_delivered` 每轮 +16,777,216、near `fin_seq=16777216 fin_w=1 rst_w=0`、far `fin_w=1 rxf=1/1`、live `txq=0`、lost/carrier_lost=0、holes=0、timeouts=0、close_detaches=0、无 `TXSTALLKILL`；CPU near 8.85–8.99%、far 27.08–31.75%。证据 `review/wan-experiment/t1-n1-auto-close-5x/shutdown-wr/`。**残余（非门禁）**：每轮重载模块变体 attempt-1 3/5、attempt-2 1/5，失败为 far `lost=1` + klog `carrier ACCEPTED ... rport=30000`（near HELLO 超时重拨）；主 spec 不重建 carrier，故 5/5 成立；该 carrier 重建窗口需另开 ticket（§8 R11）。

**T1 状态**：HND0 5x 两个变体（sendclose `1d35fb5`、shutdown-WR `08ed3b9`）均 **5/5 PASS**，source 4c58b0e、srcversion 10C45DCB；残余 carrier-rebuild race 记 R11 独立 ticket，不阻塞 loaded-once T1 gate。**G1 等 `window-verify` 的 4c58b0e A–K/near/full/SAN 全绿后完成。**

**sink 测试就绪（`hnd0-n1b`）**：`sink.py`（accept 一次、读到 EOF、记录 bytes/expected/ok/secs）与 `send16m.py`（写满 16MiB、`shutdown(SHUT_WR)`、等 EOF）本地 loopback 验证 `16,777,216 B ok=1`；HND0 `/tmp/tbtun-sink/sink.py` `py_compile` 通过。下一轮证据目录 `review/wan-experiment/t1-n1-auto-rx1m-sink/`，保留 `a2a37ed` 的 iperf3 失败证据；等 `window-close`/A3 新 head。

### G2 — T2 FEC v6 + 256 carriers + 中间码（B）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`make host-test`（含 `host/test_fec`）；`tests/qemu/run.sh --probes A,B,C,D,E,F,G,H,I,J,K --timeout 480`；256-carrier host 资源测试 |
| 证据 | `review/fec-v6/`、`host/test_fec` 输出、QEMU 日志、`/proc/net/tbtun/peers` 256 行快照 |
| 通过 | `conv=0x1001`；40B 偏移/CRC 与本文一致；`share_index/epoch/win_len` u16；256-bit bitmap；`n+m=256` 不截断；1 share/carrier；RLNC/grouped host 向量 p=1–50%；控制帧 partial 测试通过；A–K 11/11；adversarial 91/91 |
| 失败 | 任何字段偏移不符、16 槽泄漏、`n+m=256` 截断、QEMU 回归 |

**G2 现状（T2.1–T2.4 完成，T2 parent 进行）**：B `w-fec-v6 a767b63` 落地 40B header/ABI、`tbt_fec`、256 carrier slots/portmap（4096；host test 30000–34095）、grouped RLNC host codec；`test_fec 1430/0`、`test_portmap 33999/0`、`parse 412/0`、host/SAN/lint、QEMU A–K 11/11。`fec=on` 仍是 parser/procfs，`tbt_session_create()` 返回 `-EOPNOTSUPP`；数据面等 A/G1 后接。T2.1–T2.4 完成，T2 parent 进行；不合并。

**T2 scope 裁决（2026-11-06，用户）**：**T2 必须支持 HTTP 容器，不能 raw-only**。FEC share/控制帧放进 HTTP chunked body；请求行/头/`Transfer-Encoding: chunked` 保持明文；ChaCha8 只加密 body。raw 仍支持，但 **T5.2 的 256/1G 至少跑 http-kal** 并记录 overhead/吞吐。`fec=on` 拒绝非 raw 的 `-EOPNOTSUPP` 必须在 T2 完成前去掉。host/QEMU 增加 **FEC-over-HTTP** 用例：单组 1MiB 双向 byte-exact、控制帧不排 partial DATA 后。更新 DESIGN/README；**T2 parent 完成条件含 HTTP**。

**T2 单组数据面完成（`w-fec-v6 fc5ac01`）**：`fec=on` 在 raw/http-kal 建会话；单组 `carriers==n+m`（≤256）、1 share/carrier；FEC share/控制帧进 HTTP chunk body（一 chunk 一 40B 记录），请求行/头/chunked 明文，ChaCha8 只加密 body；ACK 更新 `tx_ack`；FEC RX 用 per-session 锁串行化多 carrier block 组装/交付；控制帧只在整记录边界。QEMU A–M **13 PASS / 0 skip**（213s），probe L raw 1MiB、probe M http-kal 1MiB 双向 byte-exact；`make check`（parse 440、fec 1430、portmap 33999、http 257）、host-test-san、lint 16/16+14/14；DESIGN/README/G2 更新。**grouped 数据面仍 `-EOPNOTSUPP`**（仅 `carriers==n+m` 单组）；T5.2 的 256/1G = 32×8 grouped，必须实现 grouped 数据面 + probe N 后才能跑。**T2 parent 未完成。**

**T2 单组合入 integration（`1214ee9`）**：`w-fec-v6 fc5ac01` 已合入 master（merge commit `1214ee9`，parents `e89b7d5` + `fc5ac01`），merge 后 `make check` 绿（fec 1430、portmap 33999、http 257 等）。`space-clean` 释放约 1 GiB（/home avail 6.8→7.8G，metadata 90.48→87.45%）；`/tmp` 仍 100%（`cy-*` 外部工作线，不能删）。**TMPDIR 修复**：`export TMPDIR=/home/kunweiz/.cache/tmp` 后正常 `git add`/`commit` 恢复（docs `420f553`）；不要用 `/tmp` 作 TMPDIR。T2 parent 等 grouped。

**T2 grouped 合入 / T2 完成（`0c90f0a`）**：`w-fec-grouped aec862e` 合入 master（merge `0c90f0a`），make check 绿（fec **1725**、portmap 33999、http 257）；grouped 32×8 数据面、`carrier→group/share` 映射、组窗口/RX 超块有序交付、全局 ACK、短末块；QEMU A–N **14/14 PASS**，probe N http-kal grouped `n=4/m=4 carriers=16`（2×8）1MiB×2 双向 byte-exact，L/M 单组仍过。**T2 parent 完成**（fixed grouped；dynamic M 作为 T5/T6 风险）。已知限制：无 NACK/FEC retx、`FEC_FBK` 不驱动 M 自适应、256 live 未跑（T5.2）。**T5.2 GO：fixed `n=4/m=4`、http-kal、crypto on、256 carriers。**

**T5.2 起跑前置（2026-11-06）**：integration master `7045868` 尚无 w-crypto（`tbtun/tbt_crypto.*` 不存在），`crypto=on` 不会真正加密；**T5.2 暂停起跑**。C 需把 integration merge 进 `w-crypto`（33238ca 基于 0db5166），接 FEC TX/RX body 路径（TX `seal→body_xor`、RX `body_xor→verify`；控制帧跳过；per-direction nonce；HTTP 头明文），跑 make check/host/SAN/lint，报新 head；F 再合入 integration 并给 E 新 head。T5.2 等 crypto 合入。

**T3 crypto 合入 integration（`0d11710`）**：`w-crypto 63c0a83` 合入 master（merge `0d11710`），make check 绿（`test_crypto` 59 PASS、fec 1725、portmap 33999、http 257、SAN/lint 绿）；ChaCha8 进 FEC v6 data plane；`crypto=on` + `fec=off` → `-EINVAL`；HELLO body 66B/ver2、key_id/nonce/key；per-direction nonce；PARITY 的 `share_index` 混入 nonce；TX `seal→body_xor`、RX `body_xor→verify`；会话销毁 wipe。

**HELLO conv 偏差（T0 记录）**：HELLO 仍是 **32B 头/`conv=0x0005` + 66B/ver2 body**；FEC record 仍 `0x1001`；不改 `tbt_hdr_check`。T0 §2.3 原写 HELLO `conv=0x1001`，现按实现记录为偏差（HELLO 用旧 conv，body 版本 2），两端同实现可用；若要严格 0x1001，需另开 ABI 变更。

**FEC `epoch` 偏差（T0 记录，2026-11-06）**：T5.2 的第二个根因是 FEC TX 等 8KiB packet 填满才发，小消息死锁。修复引入短包（`payload_len < S` 可出现在任意位置，只有 `LAST` 表示流结束），交付偏移改为各 share 长度之和。短包一旦被 parity 恢复，其长度无法从记录头得知（parity 可能先于该 DATA share 到达），猜 `S` 会把块的零填充交给应用。因此**本 cut 在 DATA/PARITY 记录的 `epoch`（+6，u16）字段写「本块最后一个真实 share 的 payload_len，0 表示全满」**；T0 原定义 `epoch` 为 rate epoch，本 cut 未实现 rate epoch，字段借用已在此登记为偏差。接收端遇到「需要恢复 + 含短包 + 无长度声明」的块直接拒绝（`fec_short_unknown`），不猜长度。若日后实现 rate epoch，需另给短包长度一个独立字段。

**T5.2 short-packet/MIME 修复（`w-t52-debug 9332621`）**：2ms 静默 + ring 空 → 短 share（`payload_len<S`）；短 share 结束其 block，parity `win_len=真实 share 数`，跳过未用 seq；短 DATA 任意位置合法，只有 `LAST` 表示流结束；RX 交付按 payload 长度之和/`stream_base`；recovered short 长度在 `epoch`（0=全满）；`fec_short_unknown` 拒绝不可知块。MIME：请求/响应 `Content-Type: application/gzip`，无 `Content-Encoding`、不真 gzip；body 仍 FEC/ChaCha8，头/chunked 明文。c9 新增 `fec_short_tx/short_unknown/drop_window/drop_crypto`。QEMU A–P 15/15 两次；probe P 37B/8191/8192/8193/64K/256K/crypto on/双连接/HTTP 握手；L/M 256K、O 64K、N 1MiB；make check/SAN/live HTTP 156/lint 绿。已合入 integration **`8a3589d`**，make check 绿。

**T5.2 GO（single-connection，2026-11-06）**：`4e46578` 已合入 `85f2011`，make check 绿；E 用 `tbt_burst`/`tbt_ping` **单连接**，**8–16MiB / 5–10s**，256 http-kal crypto on；记录 app/CPU/RAM/wire/holes/raw lost/retx；**不用 iperf3**（两条并发连接超出 single-stream 范围）。

**T5.2 单连接工具陷阱（t52-debug）**：`tbt_burst`/`tbt_ping` 是 strict 先写后读；8–16MiB @300ms RTT 会被 ~200KB 应用 socket 缓冲自锁。需：两端 `net.core.rmem_max/wmem_max` 与 `SO_RCVBUF/SO_SNDBUF` ≥32MiB（probe I 做法），或 interleaved 相跑 8–16MiB、strict 留 ≤1–2MiB；`tbt_ping` 只跑小消息。单连接工具下 `fec_stream_refused` 必须 0；`fec_short_tx` 在满载段应很少（满载段增长说明发送端被应用节奏饿着，查 `txq_max`/`una_stall_us`）。

**T7 合并预检（2026-11-06）**：`w-hnd0`（`7d6139a`）vs master 的 `git merge-tree --write-tree` rc=0（无冲突）；457 files / 380K lines（`scripts/deploy_hnd0.sh` + T1/T5.2 证据）。T7 合入时按 docs/evidence 处理；HND0 恢复、T5.2、T6 完成后再合。

**HND0 hang 修复（`w-hnd0-hang 430ba74`）**：根因不是锁死锁；`path_id` 16 卡口导致 240 carrier 反复失败 + far provisional 无上限/50ms 全表 + `replace_work` 全量墓碑 O(n²) + accept 无预算 + poll 0 延迟重排 + 日志未限流。修复：prov 硬上限 320、retire 直接释放、`replace` delayed 16/20ms、拨号令牌桶、accept 32/批、poll 10ms、日志 ratelimit、c3 prov 计数、`fec=off` 销毁警告。QEMU probe Q PASS（`prov_peak=320`/`reject=8780`/心跳 200/200/洪水后 near `up=255/256`），A–P 16/16，make check/lint 绿；baseline 15.9s OOM。已合入 integration **`4db804f`**，make check 绿。HND0 已恢复（无 tbtun，生产服务正常）。

**T5.2 GO（HND0 恢复）**：integration 新 master（`4db804f` + docs）；E 先确认 HND0 无 tbtun/echo/sysctl 残留，再跑单连接 `tbt_burst`/`tbt_ping` **8–16MiB / 5–10s**、256 http-kal crypto on、port block **512**、等 `up==carriers`；`http_parse_err=0`、`fec_stream_refused=0`、`fec_short_unknown/drop=0`；不用 iperf3。

**T5.2 256 首跑（2026-11-06）**：integration `9a1dacb`/product `4db804f`；256 carriers `up=256`、`http_parse_err=0`、`connect_fail=0`、`port_exhaust=0`；但 `tbt_burst` 16MiB/10s **rc=2、recvd=0 EAGAIN**；near `submit=16MiB`、`tx_ack=5,242,880`、`wire=33.8MB`、c3 `up=254 lost=18 replaced=18 connect_retry=57 stall_declared=13`；far `submit=5,242,880`、`tx_ack=2,097,152`、c3 `up=256 lost=15 join_dup=2`、**`fec_drop_window=1527`**；c8 near `req=274/resp=0`、far `resp=273`；CPU near 8.5%/far 11.8%；dmesg `tbt_tx.c:2570` WARN_ON（pre-existing OPEN announce race，baseline 也有）。失败在 **FEC window/数据路径**（`fec_drop_window`），不是 carrier 建立/HTTP parser。E hold；`t52-256` 诊断。证据 `256-burst-*`。

**T5.2 根因假设（2026-11-06）**：far `tx_window=1MiB` < `k×BDP` 2.26MB（`fec_drop_window=1527`≈2.3MB），加上 `tbt_burst` 先跑 strict send-then-read。`t52-256` 决定不重复 HND0 256；QEMU 复现/修由 `t52-debug` 负责；等其修复/参数（**4–8MiB W、interleaved**）再决定重跑。E hold，不跑 256。

**announced_mask 16 槽根因（2026-11-06）**：`announced_mask` 仍是 `u16`、`announced_gen[16]` 仍是 16 槽；carrier >16 越界/截断，HND0 UBSAN，37B ping **0/37**。T5.2 256 在修复前**不可信**；`t52-debug` 正在改 **256-bit mask + gen[256] + guard**，QEMU OPEN 16/200/255。T5.2 HOLD，HND0 clean；**暂不合/不 push**；等 t52-debug head 后再 merge/重跑。4-bit GF(16) 仍本地；T6 等用户 1G/app 口径。

**256-wide bookkeeping + FEC credit 修复（`dc4532a`）**：`tbt_stream` per-carrier 状态原 16 槽（`u16 announced_mask` + `u8 announced_gen[16]` + 4 个 `unsigned long` FIN/RST mask）；carrier 16..255 越界写坏 `tx_paused`，UBSAN；`BIT(index)` 截断导致 `WARN tbt_tx.c:2570`。修复：`DECLARE_BITMAP(..., TBT_MAX_CARRIERS_LIMIT)` + `announced_gen[256]` + index guard + `bitmap_empty()`；旧 ARQ 仍 16。同时修 **FEC ACK credit**：packetizer 不超过 peer frontier+credit（`fec_drop_window` 的次因）；`tbt_burst` 增 `strict|interleaved`。QEMU probe N 256 carriers 8MiB interleaved byte-exact，`up=256/lost=0/all c9=0`，无新 WARNING；A–P 15/15；make check/SAN/lint 绿。已合入 integration **`e7d1c83`**（CHANGELOG 冲突保留两边），make check 绿。**T5.2 GO**：`tbt_burst ... 8 30 interleaved`，两端 `rx_reorder_bytes=8388608`，等 `up=256`，`fec_drop_window=0/http_parse_err=0/fec_stream_refused=0`；旧 `workqueue.c:4335 __flush_work` 21 条噪声另开 ticket。

**hnd0-hang 叠加修复（`f015a56`，待 T5.2 后合）**：near never-connected 重试并入拨号令牌桶、dead 2n 硬上限复用池、reconcile 重采样；QEMU Q PASS（`prov_peak=320`/`reject=8488`/`dead peak=8`/心跳 200/200/洪水后 `up=256/256`），A–P 16/16；未改 announce（t52-debug 主修）。**等 T5.2 结束再合；届时去重 announce 与 dead/dial 改动**。T5.2 当前用 `e7d1c83` 跑 8MiB interleaved；不 push。

**hnd0-hang 最终验证（`bd7d308`，待 T5.2 后合）**：`e7d1c83` + `f015a56` + 证据；A–P **16/16**、probe Q PASS、**0 WARN**；未改 announce，保留 `dc4532a`。`path_lost_mask` 仍由 t52-debug 修。**排到 T5.2 之后合；现在不动 HND0/不合并/push**。T5.2 等 t52-debug path_lost 修复 + t52-perf 22M 分析。

**T5.2 256 8MiB interleaved PASS（2026-11-06）**：integration `e7d1c83`/`124c27a`；`tbt_burst 8 30 interleaved`：**8,388,608 双向 byte-exact**；near `submit=8,388,608`、`tx_ack=8,388,608`、`up=256`、`lost=39/replaced=39`、`connect_fail=0`、`connect_retry=56`；far `submit=8,388,608`、`tx_ack=8,388,608`、`up=256`、`lost=0`；两端 c9 全 0（含 `fec_drop_window=0`）、`http_parse_err=0`；near `tx_window=8MiB`、`bdp=2,016,156`；far `http_resp=256`。证据 `256b-*`。待 E 报 CPU/wire/无新增 WARNING 后 T5.2 完成；然后合 `f015a56` 并去重 announce vs dead/dial。

**T5.2 256 复跑结果（2026-11-06，未完成）**：数据路径 **8MiB 双向 byte-exact（6.11s）**，`fec_drop_window=0`/all c9=0/`http_parse_err=0`，wire/app near 17,063,469/8,388,608、far 16,999,600/8,388,608，CPU near 8.0%/far 12.2%。**但**：① 新增 UBSAN `_Bool value 14` at `tbt_fec_data.c:459`、`tbt_procfs.c:618/892`；疑似 `sess->path_lost_mask` 仍是 `unsigned long`（64 位），`set_bit(index)` 在 carrier 64..255 越界写，覆盖相邻 bool（`crypto`/`crypto_tx_ready`/`tx_paused`/`paused`）。② app 8MiB 双向/6.11s ≈22 Mbit/s 聚合（单向 11 Mbit/s），未达 33.72M 单向。T5.2 **未完成**；t52-debug 修 `path_lost_mask` 256 bitmap 后重跑；E hold；暂不合 `f015a56`。

**path_lost_mask 修复（`d0f1eb4`）**：session `path_lost_mask` 64-bit → **256-bit bitmap**；`set_bit` 加 `index<n_car && <256` 守卫；drain 在 `tx_lock` 下 `bitmap_copy`+`bitmap_zero`（旧 `xchg` 只能排 64 位）；retain 的 16 位 mask 写点加 guard；`tbt_carrier_init()` 加编译期断言（`announced_gen`、5 个 stream bitmap、`path_lost_mask`、`sched.order` ≥256）。QEMU 256 carrier n=4/m=4 16MiB interleaved：**16,777,216 双向 byte-exact、47.1 MiB/s 每方向、guest CPU 71%、kernel warnings 0 before/after**；c3 `up=256 lost=0 replaced=0 connect_fail=0 connect_retry=126`；c9 全 0；c8 near `req=256`/far `resp=256`/`parse_err=0`。22M WAN 是 RTT×窗口/ACK 节奏/应用步进，不是 CPU/FEC/HTTP；`t52-perf` 继续。near `http_resp=0` 是设计（`http_resp` 只在 far 自增，`http_req` 只在 near 自增）。已合入 integration **`02d608c`**，make check 绿。**T5.2 GO 重跑**（8–16MiB interleaved、rx_reorder 8MiB、无新 WARN）；app≥33.72M 仍开放（t52-perf）。

**最终 integration head（`0eba714`）**：`d0f1eb4` + `bd7d308`（`f015a56` dead 池/令牌桶 + A–P/Q 合并验证证据）已合入；make check 绿；合并树 A–P 16/16、probe Q PASS、0 WARN。near `http_resp=0` 是设计（near 只计 req，far 只计 resp）。**T5.2 GO（HND0 8MiB）**：`tbt_burst ... 8 30 interleaved`、两端 `rx_reorder=8MiB`、port block 512、等 `up=256`；断言 c9 全 0/no new WARN；记录 app/CPU/wire/holes/retx。app≥33.72M 仍等 t52-perf。不 push。

**T5.2 final 256（`42ed56f`/product `0eba714`，2026-11-06）**：8MiB interleaved **byte-exact 11.76s**；`fec_drop_window=0`/`stream_refused=0`/c9 全 0/`http_parse_err=0`；far `connect_fail=0`；near `connect_fail=1/lost=1/replaced=1`；**42ed56f final 无新 WARN/UBSAN**（425330/425673 两条是 stale，早于 final load 427402.991/unload 427489.181）；吞吐 8MiB 双向/11.76s ≈**22.8 Mbit/s 聚合（11.4 Mbit/s 单向）<33.72M**；CPU near 9.6%/far 11.9%；wire near 17,075,211/far 17,029,696；`txq_max=8,454,432`。T5.2 **未完成（只因吞吐）**；t52-perf 查吞吐；E hold；不 push。

**T5.2 吞吐结论（`t52-perf`）**：22.8M 聚合/11.4M 单向 = wire 44.6M ÷ **2.03**（`n=4/m=4` 100% parity +1.5% HTTP）；c9/holes/retx=0，不是重传。QEMU 47–48.5 MiB/s 是 loopback 上限，不证明 HND0 路径未饱和；HND0 near 出口 `wlp4s0` 半双工，直连 P8 单向 56.82M，tbtun 双向各 22.3M = 78%。8MiB 全部 1024 包正好等于 credit 8MiB，无需 ACK；16MiB 才暴露 ACK（super-block 1MiB、ACK 固定最低 index carrier，单点）。**要 app 33.72M 单向，需 FEC ratio ≤1.25–1.33（`n=4/m=1` 或 `n=6/m=2`），但这与 p=30–50% 恢复率冲突（rate 0.5 时 app≤22.3M）。不得静默降冗余；等用户裁决 app/p 口径。** QEMU 350ms/50Mbit 矩阵待 t52-perf。

**WARN 更正（2026-11-06）**：final-256 的两条 `tbt_tx.c:2570` 是 **stale**（425330/425673，早于 final load 427402.991/unload 427489.181）；42ed56f final **无新 WARN/UBSAN**，与 hnd0-hang Q/A–P 的 0 WARN 一致。**不要按 WARN 阻塞**；唯一异常 near `lost=1/replaced=1/connect_fail=1`（far 干净）。`t52-debug2` 不再追 WARN；`t52-perf` 查 22.8M 吞吐。T5.2 HOLD。

**final-256 WARN 归因（`t52-debug2`）**：`f015a56` 的 2570 行是 `WARN_ON_ONCE(!tbt_stream_announced)`，其 `tbtun.h` 仍是 `u16 announced_mask` + `gen[16]`；256 carrier idx≥16 越界/`BIT(idx)` 未定义，WARN 必然。两条 WARN 425330/425673 早于 final-256 模块 load 427402.991，属旧模块；42ed56f（dc4532a 后）同一 WARN 在 2586 行，load 427402.991→unload 427489.181 **无 WARN/UBSAN**。near `lost=1/replaced=1/connect_fail=1` 是启动期一条 carrier 被判 lost + 同 index 替换 + 首次 connect 失败后 200ms 重试成功；`up=256` 后数据无影响，不是 announce race。dead 池复用审计：`take_dead` memset 只清 carrier 标量，announced 在 stream；gen 不同，OPEN 重发；pump/replace 同持 `pump_lock`，无 memset 窗口。**观察项（未改）**：pinned FIN/RST 的 `path=0xff` 在 `n_car=256` 时会落到 `sess->car[255]`；pump 注释称 never sees，但取 carrier 用 `sess->car[fr->path]` 直取；final-256 未观察到异常，另开 ticket。

**256 边角：pinned FIN/RST `path=0xff`（2026-11-06）**：pinned FIN/RST 的 `path=0xff` 在 `n_car=256` 时会落到 `sess->car[255]`；`t52-debug2` 在修（guard/专用语义），修后合入再 final。T5.2 只剩 **22.8M<33.72**；`t52-perf` 查 `una_stall`/ACK/window。不合并/push。

**T5.2 吞吐算术裁决（t52-perf）**：`p=m/(n+m)`；p30 最低 r=1.429、p40 r=1.667、p50 r=2.0。33.72M 单向需 wire **48.2/56.2/67.4M**。当前单向 wire 22.3M（tbtun=直连 P8 56.82M 的 39%）；22.3M 下所有 p–app 点 ≤22.3，降冗余不能到 33.72。p50 单向需 67.4M=直连 119% **不可达**；p40 需 99%；p30 需 85%。**关键裁决：验收是单向 33.72 还是双向同时每方向 33.72**；near 出口 `wlp4s0` 半双工，双向同时空口需 2×W，p30 需 96.4M>56.8，不可达。QEMU 350ms/50Mbit 矩阵待 t52-perf。

**T5.2 验收口径待用户裁决（2026-11-06）**：① **单向 33.72M**：p30 wire 48.2M=85% 直连（可行）、p40 56.2=99%、p50 67.4=119% **不可达**；② **双向合计 33.72M**：每向 16.86M，p30 总 wire 48.2M **可行**；③ **双向同时每方向 33.72M**：p30 空口 96.4M>56.8 **不可达**。T5.2 HOLD，不静默降级；`t52-perf` 等口径后测单向/合计；`t52-debug2` 修 `path=0xff`；不跑 256、不 push。

**t52-perf QEMU 矩阵（2026-11-06，256 carrier/350ms/50Mbit/向）**：**A `n=4 m=4` 8MiB PASS byte-exact**：wall 4.71s、wire **28.68M 单向**、app 14.2M 单向/28.4 聚合、CPU 51.5%、up=256（建链 255，1 自愈）、c9=0/无 WARN；每 carrier **3.3 MSS/RTT**，HND0 仅 **1.4 MSS**；50Mbit 链路只用 57% → cwnd/队列，不是带宽。**B `n=7 m=1` HANG >300s**：m=1 无法恢复延迟/丢失 share，super-block 永不 ready；p=12.5% 不可用，p30 的 `n=7 m=3` 同类风险。**口径含义**：单向 33.72 的 p30 需 48.2M，QEMU 28.68/HND0 22.3 均差 1.7–2.2×，p50 物理不可达；**双向合计 33.72 的 p40 `n=3 m=2` 需 28.1M，QEMU 28.68 刚好够，HND0 22.3 差 26%**；双向各 33.72 不可达。8MiB=1 个 credit 窗口（1024 包=8MiB），全程无需 ACK；`una_stall` 是空闲 carrier 累积，`tx_window/txq` 不进 FEC 数据面。**下一步建议（待口径）**：口径 B + p40 `n=3 m=2 carriers=255`；HND0 试 **128 carrier**（减半 Wi-Fi 竞争）+ 放大 rcvbuf；16MiB 前把 FEC ACK 从 carrier 0 改轮转。

**128 carrier 约束（t52-perf）**：`carriers % (n+m)==0`；128 可行点：`n=4 m=4`→128（p50,r2）、`n=6 m=2`→128（p25,r1.333）、`n=3 m=2`→**125**（p40,r1.667）、`n=7 m=1`→128（p12.5，已证 hang）。**“128+p40”= `n=3 m=2 carriers=125`**；`carriers=255` 是 256 侧的 p40 点。128 每 carrier 数据翻倍（32/66→64/132KB），350ms 需平均 cwnd **6.6 MSS**（QEMU A 3.3、HND0 1.4）；减半竞争若 cwnd 翻倍则净赚，否则时间翻倍。rcvbuf 128 carrier 时 1MiB/carrier（2×256）。**FEC ACK 轮转**：`fec_ack_rot`，control 循环 `i=(rot+k)%n_car`，成功后 `rot=i+1`；~10 行、不改 wire；8MiB 无收益（credit=8MiB），16MiB 前做。对照顺序：QEMU 8MiB `256/n4m4`（已有 4.71s/28.68 wire）vs `128/n4m4` vs `125/n3m2` → 加 rcvbuf/queue → 128 胜出则 16MiB+ACK 轮转，否则回 `256/n3m2 255`。

**验收平台更正（2026-11-06）**：用户验收口径是 **arm64 `.65`**；x86 QEMU 的 128/125/255 仅 dev 参考，**不能当验收**。`t52-arm64` 在 `.65` 上测 direct baseline + T5.2；**128 vs 256 是用户配置决策，不静默减半**。HND0 256 等 `.65` clean + 最终 head；不跑、不 push。x86 16MiB/ACK 轮转由 `t52-credit` 处理。

**最终合并等待 t52-credit（2026-11-06）**：`t52-credit` 正在 master 工作树改 `tbt_fec_data.c`/`tbt_procfs.c`/`tbtun.h`/`tests/qemu/*`（`fec_ack_rot`、credit 语义、c9 ACK/credit 计数），**未提交**；master 因此不能 merge `51780c3`/`c948e88`。要求 t52-credit 先提交到 `w-t52-credit` 并清 master，F 再按 **`51780c3`（pinned path u16）→ `c948e88`（carrier 丢失改投/按头放置）→ `t52-credit`（ACK 轮转/credit）** 合入，出最终 head 给 arm64-build。残留：FEC block 缺 1 share + 4 parity 仍不可解 → 另开 `t52-fec-loss`；不阻塞 arm64 baseline。不跑 HND0 256、不 push。

**最终合并进度（2026-11-06）**：`51780c3`（pinned path u16）与 `c948e88`（carrier 丢失改投/按头放置）已合入 master（`1370796`/`f356d78`，make check 绿）。等 **`t52-credit`**（ACK 轮转/credit 语义/c9 计数）与 **`t52-fec-loss`**（block 缺 1 share + 4 parity 仍不可解）合入后，出最终 head 给 `arm64-build`。T5.2 验收 = **arm64 `.65` direct P8 baseline up/down；tunnel 每方向必须超过同方向 baseline**。不跑 HND0 256、不 push。

**arm64 构建修复合入（`8cc568c`）**：6.1 `kernel_bind`/`kernel_connect` 的 `sockaddr` typedef（3 files +13/-3；`system_dfl_wq` shim 已有 `79dd27b`）；aarch64 ko 在 `.65` insmod/rmmod 2 次、proc 正常、0 WARN。已合入 master **`faf6280`**，make check 绿。等 `t52-credit` 后出最终 head；`arm64-build` rebase/rebuild；`t52-arm64` **先跑 direct P8 baseline**。不跑 HND0 256、不 push。

**t52-credit 合入 / 最终 head（`2bcbc89`）**：`fec_ack_rot`、zero credit closed、c9 ACK/credit 计数合入 master（merge `2bcbc89`），make check 绿。最终 head 含 `51780c3 + c948e88 + 8cc568c + t52-credit`；`arm64-build` 跳过 `15284da` 预演，等 `.65` baseline 释放后直接对最终 head clean build/insmod；`t52-arm64` tunnel 用最终 ko。`t52-fec-loss` 仍为独立残留（block 缺 1 share+4 parity）。不跑 HND0 256、不 push。

**t52-credit 验证（`1a33e6e`/`2bcbc89`）**：QEMU 256 n4m4 http-kal crypto=on rx_reorder8，8/16/32MiB **byte-exact** 180/320/690ms、44.4/50.0/46.4 MiB/s/向、CPU 84/72/63%、`ack_tx=ack_rx=8/16/32`、c9 其余 0、0 WARN；ACK 每超块 1 个；16MiB `tx_ack` 满证明 credit 回填。硬边界 `TBT_FEC_CREDIT_MAX=128` 仍 16MiB byte-exact、`credit_wait=30/44`，证明停窗+轮转 ACK 唤醒。合并预演 clean。srcversion `92FA9850…`。最终 head **`913aa3e`**；`de206c7` docs-only 未合，保持 head 稳定。

**arm64 `.65` direct P8 baseline（2026-11-06）**：8×HTTP/1.1、8MiB/req、10s×2、**bbr**（cubic 首跑 2.6M 丢弃）：**up 34.49/10.70 Mbit/s（mean 22.6、peak 34.5）**；**down 11.48/29.02（mean 20.25、peak 29.0）**；CPU near 2.5–2.8%/far 9.9–11.8%；wire up near tx 104.6MiB/far rx 73.7MiB、down near rx 79.7MiB/far tx 94.1MiB；8MiB 请求 10s 内截断 `partial=8`。**验收 tunnel 每方向 > 同方向 baseline；mean/peak 口径待用户确认。** 证据 `review/wan-experiment/arm64/runs/`。

**arm64 final ko（`cb23c64`）**：aarch64 6.1.172 clean build（0 warning/0 error，compiler-differs 提示除外）；ko `/root/tbtun-worktrees/arm64-build/tbtun.ko` sha256 `089f74f3…`、srcversion `E1096721…`、vermagic `6.1.172 SMP mod_unload modversions aarch64`；insmod/rmmod rc=0、`/proc/net/tbtun` 6 项出现/消失、load→unload 0 WARN/BUG/Oops；未建 tunnel/benchmark/HND0。`.65` 交 `t52-arm64` 跑 T5.2 256。

**`.65` aarch64 b41b259 ko（`arm64-build-3`）**：ko `/root/tbtun-arm64-build-3/src/tbtun.ko` sha256 `3582ff9c…`、srcversion `8E699409…`、vermagic `6.1.172 SMP mod_unload modversions aarch64`；insmod/rmmod rc=0、0 WARN/UBSAN；`.65` 释放给 `t52-arm64-3`。HND0 far x86 b41b259 由 `t52-arm64-3` 部署。

**fec-loss 修复合入（`849519b` → `0be41c0`）**：根因 `n4m4` parity 矩阵不是 covering code；splitmix64 每行选 4/8，某 DATA 被 4 行全漏概率 1/16，rank4 仅 35.7%；一个 DATA 丢失 + 4 parity 仍 rank0，block 卡住、`tx_ack` 停在 8MiB。修复：确定性 band（列 `c` 在行 `r` 当 `(c-r) mod m < band`，n4m4 band=3 补 identity；任意 ≤4 DATA erasure 可恢复，每 share 在 3 行），short tail zero-pad；**wire 不变，两端必须同 revision**。QEMU 256/16MiB/carrier0 down：byte-exact、`tx_ack` 满、c9 全 0、A–P 16/16；host 2563 checks/0。merge `0be41c0`（CHANGELOG/`tests/qemu/init` 冲突已解，`sh -n` + make check 绿）。最终 head 见下；arm64-build 重建，t52-arm64 用新 ko 跑最终 T5.2；`cb23c64` 的 T5.2 只是 preliminary。

**T5.2 256 判读（t52-debug）**：c9 `fec_short_tx` 应随小包非零；`fec_short_unknown`/`fec_drop_window`/`fec_drop_crypto` 必须为 0；`fec_short_unknown` 增长 = 对端不是本 revision（`epoch` 未带短包长度）。c8 `http_req == http_resp == carrier 数`；不等时 debug>=1 打 `http <stage>`/`HELLO rejected`。c3 `connect_retry` 可非 0，`connect_fail` 必须 0；`-110 ETIMEDOUT` 才是真不可达。n>1 时空闲流首包延迟下限一个 block（n×8KiB）；256 `n=4/m=4` 只影响空闲首包时延，不影响吞吐。`epoch` 借用短包长度，两端必须同 revision。

**T5.2 retest 失败（`db575ea`/`8a3589d`，2026-11-06）**：37B cookie 可过（`fec_short_tx=3/19`），但 8MiB 数据流停：near `submit=207`、`tx_ack=37`、`data_tx=0`、`capture_runs=4/actor_calls=3`；far `rx_delivered=207`、`rx_ack=0`、`tx_ack=1`；near `CLOSETIMEOUT` 15s（`ack=37 txseq=167`；另一流 `ack=0 txseq=37`）；c9 proto/hdr/payload=0、short_unknown/drop=0；c8 near `req=2/resp=0`、far `resp=2/4`；c3 `up=2 lost=0/2 connect_fail=0`。短包 flush 生效；剩余在 **http-kal 数据块封装/发送路径**（cookie 后 app 数据未进隧道）。E hold；`t52-debug` 继续。证据 `smoke-8M-crypto-off-*`、`smoke-P-crypto-off*`。

**Single-stream limit（S1，2026-11-06）**：FEC v6 数据面**一 tunnel 一 app stream**（S1；256 flows = carrier）。FEC 头无 sid；两条并发 live app stream 会把字节混给同一 far stream。修复 `4e46578`：第二条 live stream 立即 reset + `fec_stream_refused`；已 close 的流不阻挡下一条（顺序连接可）。**iperf3 固定 control+data 两条并发连接，超出 v6 范围，T5.2 不用**；改用单连接 `tbt_burst`（8–16MiB）与 `tbt_ping`（小消息）。多流 stream tag 记 T5.3 设计项。

**T5.2 256 失败 + HND0 失联（2026-11-06）**：integration `b4894ef`/product `85f2011`；第一次 256（near ports 40000-44095）peers 256 行但 c3 `up=16 lost=8864 replaced=8851 connect_fail=22 connect_retry=2440`；c8 `http_req=8880 http_resp=0 http_parse_err=8864`；c9=0。第二次（near ports 30000-34095）256 carriers 全 `-110 ETIMEDOUT`；c3 `up=0 connect_fail=1536 connect_retry=4608`；c9=0。随后 HND0 不可达：ping 100% loss；SSH 从 timeout 变 `Connection closed by 45.11.1.62 port 22`（22 有 TCP 响应但立即关闭，疑似重启/sshd/云 RST）；本机网络正常（1.1.1.1 通，出口 IP `171.120.34.226`→`38.59.230.115`）。本机已清理；HND0 上 tbtun/tbt-echo/64MiB sysctl 未清理（重启则 sysctl 自动恢复）。**T5.2 256 暂停，等云控制台确认 HND0；不直接重试；建议限制拨号速率/分批建立。** 证据 `256-attempt-near-fail.txt`。

**HND0 恢复检查（2026-11-06）**：E 已恢复直连（`ip rule pref 100`，`ip route get` → `192.168.123.254 dev wlp4s0 src 192.168.123.69`，出口 `171.120.34.226`；`38.59.230.115` 是 mihomo/FRA 路径），HND0 仍 ping 100% loss/SSH timeout。需 provider 控制台确认：`uptime`、`dmesg`（OOM/panic）、`lsmod | grep tbtun`、`ss -lnt 5202`、sysctl 64 MiB 是否残留。未再重试/跑 256。本机直连规则保留到 HND0 确认；本机无 tbtun/监听。**T5.2 HOLD；`t52-debug` 在 QEMU 查 near response parser MIME；HND0 恢复前不合并/验收。**

**HND0 旁路诊断（2026-11-06）**：本地直连（出口 `171.120.34.226`）仍 ping 100%/SSH timeout。E 从 **FRA `38.59.230.115` / HND1 `103.200.30.77`** 旁路 ping/ssh HND0，区分 HND0 宕机 vs 本地源 IP 被 256 连接风暴限流；若旁路可达，查 `uptime`/`dmesg`/`lsmod`/`ss 5202`/sysctl 并清理。T5.2 HOLD；不合并/push。

**HND0 旁路结果（2026-11-06）**：FRA `38.59.230.115` ping 100% loss + ICMP `Destination Host Unreachable` from `79.127.194.99`、ssh `No route to host`；HND1 `103.200.30.77` ping 100% loss、ssh timeout。两台旁路机正常/低负载，下一跳 unreachable/timeout → **HND0 主机或所在网络中断（宕机/panic/云侧网络），不是本地源 IP 被 256 风暴限流**。需 provider 控制台：电源/控制台、`uptime`、`dmesg`（OOM/panic）、`lsmod | grep tbtun`、`ss -lnt 5202`、sysctl 64 MiB；恢复后先清理模块/echo/sysctl，再讨论限速/分批重试。T5.2 HOLD；本机直连规则保留，无 tbtun/监听；不 push。

**256 根因修复（`72d00dd`）**：不是 MIME。`tbt_hdr_check()` 把旧 32B 头的 `path_id` 卡在 `TBT_LEGACY_MAX_CARRIERS`(16)，而 `path_id` 是 carrier 索引、载体层有 256 槽；carrier 16..255 的 HELLO/OPEN/FIN/RST 在**编码阶段**被拒（`tbt_http_frame_encode()` 返回 0）→ `http_parse_err++`、carrier poison，far 从未发响应。修复：`path_id` 去掉 16 上限（u8 全域落在 256 槽）；`path_ack_count`/`path_ack_mask` 的 16 上限保留；host fuzz pin `path_id` 15/16/200/255；probe N 改 256 carrier 生产形状并等最后一条 join；port block 512（替换余量，256 会 `port_exhaust`）。QEMU probe N 256 carrier `n=4/m=4` 32 groups 1MiB：far c3 `n=256 up=256 lost=0`、c8 `http_resp=256/http_parse_err=0`；near c8 `http_req=256/http_parse_err=0`；PASS 1MiB byte-exact；A–P 15/15。已合入 integration **`58a1df7`**，make check 绿。E 判读：`http_parse_err=0`；非 0 看发送端 builder/encoder vs 接收端 parse；`up==carriers`，不等看 `alloc_fail/port_exhaust/connect_retry`。T5.2 仍 HOLD 等 HND0 恢复。

**256 恢复后四点（t52-debug）**：① 等 `up == carriers`（256）再启动，不用固定 sleep；早启动会让 super-block 等一个不齐的 group（QEMU 250/254 启动即 `tbt_burst` 卡死）。② 端口块留 **2× 余量**；256 端口 + 256 carrier 首次替换即 `port_exhaust`（QEMU 240 次）。③ `http_parse_err=0`；非 0 先看**发送端** builder/encoder vs 接收端 parse（本次 240/8864 是发送端 `path_id`）；debug>=1 打 `tbtun http <stage>`/`HELLO rejected`。④ `fec_stream_refused=0`；非 0 = 又出现第二条并发连接（iperf3/工具重连），单连接工具才符合当前 FEC 面。`connect_retry` 非 0 正常；`fec_short_tx` 满载段应少。

**T5.2 smoke 失败（2026-11-06，blocked）**：integration `f32c03a`（crypto `0d11710`）；小 smoke `n=1/m=1/carriers=2/http-kal/crypto=on` 失败：near c3 `up=1 lost=27 replaced=27 connect_fail=41`，carrier 0 `connecting`、`failed to connect: -115`（EINPROGRESS）反复重拨；far `carrier ACCEPTED ... awaiting HELLO`，HELLO 未完成；near `http_req=28/http_resp=0`，far `http_req=0/http_resp=1`、`rx_proto=27/rx_errs=27 rx_last_err=-71`。**256 未跑；E hold**。已开 `t52-debug`（`w-t52-debug` from f32c03a）在 QEMU 复现/修。证据 `review/wan-experiment/t5.2-256-http-kal/`。T5.2 blocked。

**T5.2 修复（`w-t52-debug 7b7965f`）**：两个根因：① crypto HELLO key ownership——far 采纳 near key 后仍把本地 key 当权威，后续 carrier `key_id=1` 被 `-EPROTO` 拒；加 `crypto_key_peer`，`resolve_key` 改 `local_key_wins`，采纳 key 与后续 HELLO 比较。② `kernel_connect` 超时返回 `-EINPROGRESS`(-115) 被当失败重拨；`tbt_connect_bounded()` 在 4s 内重入等待，`connect_retry` 计数。QEMU B,C,N,O PASS：probe N n=4/m=4 carriers=16 http-kal crypto=on `up=16`、`http_req=16/http_resp=16`、1MiB byte-exact；probe O smoke phases off/on/replace/slow 全 byte-exact、`connect_retry=2/connect_fail=0`；新增 c9 `fec_proto_fail`/`fec_hdr_crc_fail`/`fec_payload_crc_fail`。已合入 integration **`9f66fba`**，make check 绿。**T5.2 GO（新 master）**。

**T5.2 256 观测点（t52-debug）**：far c9 `fec_payload_crc_fail` 必须为 0（crypto=on 下区分 key 不一致/对端沉默；增长而 `rx_proto` 不动 = key 不一致，预共享 key 只配一端会走到这里）；c3 `connect_retry` 非 0 可接受（WAN SYN >1s），但必须 `connect_fail=0`；若 `connect_fail` 涨且错误是 `-110 ETIMEDOUT` 才是真不可达。c8 `http_req == http_resp == carrier 数（含 drip spare）`；不等时 debug>=1 打 `http <stage>`/`HELLO rejected: <check>`。`clang-format-18` 本机缺失，`make format-check` 未跑，合并前注意 CI。

**T5.2 smoke phase crypto=off 失败（2026-11-06）**：integration `9f66fba`（含 `7b7965f`）；`n=1/m=1/carriers=2/http-kal/crypto=off`：carrier `up=2 lost=0 connect_fail=0`、`connect_retry=1`；c9 全 0；c8 near `http_req=2/http_resp=0`、far `http_req=0/http_resp=2`；far `rx_delivered=37`、`streams_active=0`；app 只传 37B，iperf3 180s timeout，far server `unable to receive cookie: Bad file descriptor`、无 Accepted。carrier HELLO/CRC 正常；问题在 **http-kal app 流/target 连接路径**，与 crypto 无关。E hold；`t52-debug` 继续诊断。证据 `review/wan-experiment/t5.2-256-http-kal/smoke-O-crypto-off-*`。

**T5.2 blocked：8KiB 块等满（2026-11-06）**：smoke crypto=off 中 carrier `up=2`、c9=0，但 app 37B 未发出（near `submit=37`、far `rx_delivered=0`），far target 未 accept；疑似 FEC TX 等 8KiB packet 填满才发，小消息（iperf3 cookie/control）死锁；QEMU 1MiB 大消息掩盖。`t52-debug` 加 **short-block/partial flush + 小消息回归**；E hold，256 不跑。

**HTTP MIME 伪装 + T5.2 数据量（2026-11-06 用户）**：HTTP 头改为 `Content-Type: application/gzip`；body 仍是 ChaCha8/FEC，**不真 gzip、不加 `Content-Encoding`**。`t52-debug` 实现。T5.2 最终 256 数据量降为 **8–16MiB / 5–10s**（不跑 1GiB）；QEMU/小消息优先。合并 `t52-debug` 新 head 后 T5.2 GO。

### G3 — T3 ChaCha8（C）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；`./host/test_crypto`；HND0 `review/microbench/run_chacha_ref.sh --bytes 4 --reps 15 --mhz 2200`（或模块内 benchmark） |
| 证据 | `review/crypto/`、`host/test_crypto` 输出、HND0 日志 |
| 通过 | TC1 + 256-bit KAT 逐字节；body-only（HTTP 头/FEC 头明文）；默认开、`crypto=off` 可关；HND0 MB/s/cycles/byte 记录；1G 单核占用 ≤25% |
| 失败 | KAT 不符、头被加密、无法关闭、CPU 超预算 |

**T3 现状（G3 通过，待 B 后 rebase）**：C2 `w-crypto 33238ca` 加 `tbt_crypto_pair {tx,rx}`、per-direction nonce、等 nonce 拒绝、HELLO 双向；KAT 55/55，`make check`/`host-test`/`host-test-san` 绿；HND0 ChaCha8 8KiB max **635.0 MB/s / 3.46 c/B / 19.7% 单核**（median 24.7%），body-only/per-direction/default/可关。T3 标完成；合并前等 B 后 rebase；不合并。

### G4 — T4 SC-LDPC BEC 容量门（D）

| 项 | 内容 |
|---|---|
| 命令 | `cc -O2 -std=c99 -Wall -Wextra -Werror sc_ldpc.c sc_ldpc_bench.c -lm -o sc_ldpc_bench`；`./sc_ldpc_bench --model symmetric --trials 500`，点 `(block,L,rate,p)=(256,8,0.70,0.30),(256,8,0.60,0.40),(512,16,0.50,0.50)`（或 D 证明的等价点） |
| 证据 | `review/sc-ldpc/`、`review/microbench/logs/sc-ldpc-*`、原始 CSV |
| 通过 | p=30/40/50 对应 rate ≤0.70/0.60/0.50；恢复 ≥99.9%（500 trials 的 0 失败或置信区间下界达标）；codec ≥125 MB/s（encode/decode，HND0 单/双核）；块延迟/内存记录 |
| 失败 | recovery 0、systematic-only 作弊、<125 MB/s、无置信区间 |

**G4 现状（`sclc2`，HND0 无可行点，2026-11-06）**：HND0（QEMU 2 vCPU，taskset 0）实测 decode **0.32–1.99 MB/s**，全部低于 app 4.215 MB/s（最好 47%）；2 线程最好 1.96；encode **2.65–9.54**，低于 wire 需求 8.4–14.1。恢复率不是障碍：p30 rate0.50、p40 rate0.40 为 1.0000；p50 rate0.25 block256/L16 为 0.9995、L32 为 1.0000，但 decode 0.43–0.46。本地 Ryzen 是 HND0 的 6–12×，不能当验收；extra_dc 6/8 与 5 相同、3/4 更差，已无旋钮。证据：`review/sc-ldpc-hnd0.md`/`.csv`（head `ef5f956`）、`review/sc-ldpc-sim.md`/`.csv`；`sclc2` 已停手，`w-fec` clean。**HND0 上 SC-LDPC 无可行点，G4 不修订，T6 阻塞；等用户 B（RLNC/grouped 降级）/C（SC-LDPC 硬门阻塞）裁决；不合并。**

### G5 — T5 HND0 256-flow 中间 FEC + ChaCha8（E）

| 项 | 内容 |
|---|---|
| 命令 | 部署脚本 `dry-run`；维护窗口内 `iperf3` 单流 256 carriers；`cat /proc/net/tbtun/{tunnels,peers,stats}`；`mpstat -P ALL`、`/proc/softirqs`、`ss -ti`、`free -m` |
| 证据 | `review/hnd0-v6/`（原始日志、CSV、procfs 快照、服务恢复检查） |
| 通过 | 256/256 carriers；app ≥33.72 Mbit/s；记录 p、near/far CPU/RAM、wire/app、holes、raw lost；frps/Caddy/hysteria/xray 全部 active；无持久改动 |
| 失败 | <33.72M、carrier 掉线、服务未恢复、OOM、无回滚 |

**T5.1 已接受 `c435ba9`**：全局 `/proc/net/tbtun/ports` 写入/verify、v6 行去 legacy 键、端口保留告警反转、dry-run 提交；T5.2 等 G5。

**T5.2 http-kal 部署变体（`w-hnd0 7d6139a`）**：新增 `--http-kal`/`--raw`；v6 config 支持 `profile=http-kal`（`n=4 m=4 fec=on mode=grouped w=0 carriers=256 crypto=on crypto_key=- profile=http-kal`）；dry-run `bash -n` OK、http-kal/raw 行正确、256→groups=32、全局 ports 在 config 前、服务 active、verify rc=0；未加载/未跑负载。等 T2 完成后执行 T5.2；T5.1 保持 c435ba9；不合并/push。

### G6 — T6 SC-LDPC 最终 + HND0 256-flow（D+E）

| 项 | 内容 |
|---|---|
| 命令 | G4 的 SC-LDPC 门 + G5 的 HND0 测试，但 `mode=sc-ldpc`，无中间 FEC |
| 证据 | `review/hnd0-v6/sc-ldpc/`、`review/sc-ldpc/` |
| 通过 | G4 全绿；G5 全绿；app ≥33.72M；无 RLNC/grouped 兜底；服务恢复；最终门 |
| 失败 | 任何中间码回退、G4 未过、HND0 不达标 |

**T6 状态：等待用户口径。** HND0 SC-LDPC decode 0.32–1.99 MB/s < app 4.215、encode 2.65–9.54 < wire 8.4–14.1；G4 不修订。**2026-11-06 新指令**：上行允许 **grouped SC-LDPC 4-bit 码**，本地原型 `w-sclc-gf16`（worktree `sclc-gf16`）；`w-sclc-*` 暂不合。T6 等用户确认目标是 **1G/125 MB/s** 还是 **app 33.72M**，再决定 B/C；不合并 SC-LDPC。

### G7 — T7 文档/0.0.2/合并 push（F）

| 项 | 内容 |
|---|---|
| 命令 | `make check`；A–K 11/11；`bash adversarial/run.sh`；`grep TBT_VERSION tbtun/tbt_uapi.h`；`git merge --no-ff` 各分支；`git push` |
| 证据 | `review/final/`、`git log --oneline`、`git status`、release 说明 |
| 通过 | `0.0.2`；A–K 11/11；adversarial 91/91；host/SAN/lint；master 干净；push 成功；文档与 ABI 一致 |
| 失败 | 版本不符、回归、脏树、push 失败 |

---

## 7. 机器锁

三台机器的串行化用 `flock`，锁文件固定：

| 机器 | 锁文件 | 用途 |
|---|---|---|
| QEMU（本机） | `/tmp/tbtun-qemu.lock` | 所有 `tests/qemu/run.sh`、`review/dkms-qemu/run.sh`、adversarial QEMU 组 |
| HND0 `45.11.1.62` | `/tmp/tbtun-hnd0.lock` | 维护窗口、部署、iperf3、procfs 采集 |
| RK3588 `.65` | `/tmp/tbtun-rk3588.lock` | 所有 `.65` 构建/bench；跑前 `uptime`，load >4 先避让 |

协议：

```sh
lock=/tmp/tbtun-qemu.lock          # 或 hnd0 / rk3588
exec 9>"$lock"
flock -w 1800 9 || { echo "lock busy: $lock" >&2; exit 1; }
echo "$$ $(date -Is) $(whoami) <purpose>" >&9
trap 'flock -u 9' EXIT
# ... run ...
```

规则：

- 没有锁**不得**跑 QEMU/HND0/RK3588；`flock` 释放靠进程退出/close，锁文件不删除。
- 不杀持锁进程；等锁超时后报告 Main，不绕过。
- HND0 锁同时代表维护窗口；拿锁前先在任务/消息里声明目的与预计时长，拿锁后立即记录 holder 行。
- `.65` 跑前检查 load 与其他 worker；load >4 或检测到他人进程，跳过并记录，不抢跑。
- QEMU 锁覆盖 `make check` 里的 QEMU 部分；纯 host 测试不需要锁。

---

## 8. 风险

| # | 风险 | 证据/量级 | 处理 |
|---|---|---|---|
| R1 | **SC-LDPC 最终门：HND0 无可行点** | HND0 decode 0.32–1.99 MB/s < app 4.215；2 线程 1.96；encode 2.65–9.54 < wire 8.4–14.1；恢复率可 0.9995–1.0（p50 rate0.25），但 CPU 不过；本地 Ryzen 6–12× 不作验收；extra_dc 无改善 | G4 不修订；T6 阻塞；用户 B/C 裁决；不合并 SC-LDPC；不静默降级 |
| R2 | **app ≥33.72M 依赖 256-flow 聚合** | 33.72M 是直连 P8 单流；FEC 后 app = 聚合 × 码率；若路径本身只有 ~34M，FEC 开销可能达不到 | 先测 256-flow 聚合 > 33.72/码率；记录 p；不静默改目标；达不到按 blocker 规则停问 |
| R3 | **HND0 2 vCPU/955 MiB** | TCP/HTTP 22–31% 单核；256 flows 的 softirq/slab/端口；FEC grouped 0.75–1.5 GB/s（5–12× headroom） | session poll 合并；每 carrier `w_tx`；RPS/RFS 不可用（只有 2 vCPU），靠预算与实测；`max_tunnels=1`；64 MiB socket 缓冲 |
| R4 | **v6 回归** | 旧 conv 0x0005/16 槽仍在；`TBT_MAX_CARRIERS_LIMIT` 改 256 容易把旧 mask 截断 | `TBT_LEGACY_MAX_CARRIERS=16` 分离；`fec=off` 默认；A–K/adversarial 每步回归；`n+m>16` 旧路径拒绝 |
| R5 | **ChaCha8 CPU** | HND0 648.6 MB/s、3.39 c/B、1G 19.3%；app 33.72M 约 0.65% | 默认 on 但只在 `fec=on`；可关；body-only 不加包头/HTTP 头成本；KAT 与实测门 |
| R6 | **控制帧被 partial DATA 堵死** | 实测 near `fbk_tx=253`、far `fbk_rx=0` 最终 EOF | §1.6 三条规则；host 测试覆盖全 carrier partial；`ctl_blocked_by_partial` 计数 |
| R7 | **256 条 carrier 的资源/端口** | 64–128 MiB 缓冲；portblock 4096；TIME_WAIT；`max_tunnels×256` socket | 预算上限、portblock 4096、创建时拒绝超限；HND0 实测 RSS/slab/端口 |
| R8 | **p>20% 的 single sparse RLNC 不可行** | p=30–50% 的 XOR 32–39 GB/s，超 HND0 7.8–9.1 GB/s | 中间码用 grouped；最终必须 SC-LDPC；不把 single 写成高 p 生产 |
| R9 | **key 明文/无认证** | HELLO 明文下发 key；CRC 不是 MAC | 文档写明只混淆；需要安全用 WireGuard；不做强安全承诺 |
| R10 | **多流** | v6 一隧道一 stream；第二 stream 无 FEC 状态 | 第二 stream RST/拒绝并计数；多流留 v7；不静默塞 `block_id` |
| R11 | **carrier 重建/HELLO 重拨窗口** | 每轮重载模块变体 attempt-1 3/5、attempt-2 1/5；失败 far `lost=1` + klog `carrier ACCEPTED ... rport=30000`（near HELLO 超时重拨）；数据仍 16MiB/EOF | 主 5x spec 不重建 carrier，故 5/5 成立；单独 ticket 跟踪，不纳入 T1 门禁 |
| R12 | **256 并发拨号压垮 HND0/被封** | 第一次 up=16/256、http_parse_err=8864；第二次全 `-110`；随后 HND0 ping 100% loss/SSH closed；本机网络正常、出口 IP 变化 | 暂停 256；云控制台确认/恢复；限制拨号速率、分批建立、保留回滚；未确认前不重试 |

---

## 9. 接口裁决与跟踪

### 9.1 已裁决：ChaCha8 key 载体（原 Q1）

**维持 B 的 HELLO v6 body 方案。** FEC_CFG 保持 32B，不新增 `0x36`；goal 文本里的「FEC_CFG 明文下发」按「HELLO v6 body 明文下发」记录为**等价偏差**（都是每会话明文 key、只混淆不认证）。B/C 不改 ABI。

### 9.2 跟踪项（负责人 / 截止门 / 验收）

| # | 项 | 负责人 | 截止门 | 验收 |
|---:|---|---|---|---|
| 1 | grouped 条带化与 `block_id` 映射：T0 冻结 `group=carrier/(n+m)`、`group=seq%G`、`block_id` 为组内 generation | B 确认 | G2 前 | B 在 `tbt_fec.c`/host 测试中按此实现；若改连续块，只改映射、不改 wire 字段，并书面回复 F |
| 2 | 旧 16 槽拆分：`path_ack_mask`/FBK/WINDOW/path_gen/path_end 改用 `TBT_LEGACY_MAX_CARRIERS=16`；`TBT_MAX_CARRIERS_LIMIT=256` 只给载体槽/新 FEC | B 实现；A 合入时检查；F 裁决 | G2 / A 合并时 | `n+m=256` 不截断；`fec=off` 的 `n+m>16` 被拒；旧 wire 常量不变 |
| 3 | SC-LDPC `w`/L 配置面：T0 冻结 `w=0→256`、显式 256/512、L=8；不新增键 | D 与 B/F 对齐 | G4 前 | D 的 `tbt_fec_ops` 实现接受该配置；若要 `L=`/`block=` 新键，先书面改 ABI |
| 4 | 末包 `payload_len < S`：T0 冻结 `flags.LAST` + 实际 `payload_len`；RLNC 内部按 S 零填充 | B 定稿 | G2 前 | B 的 host 测试覆盖末包 1..S；若改 FIN body 方案，先改控制帧 ABI |
| 5 | `w=`/`carriers=` auto 渲染：0 解析为 auto，读回渲染 effective 值 | B 更新 golden | G2 前 | `host/test_parse.c` 的 golden 行同步；`cat config > backup; cat backup > config` 仍逐字节回环 |
| 6 | 合并列车：`w-window` → `w-fec-v6` → `w-crypto` → `w-sclc` → `w-hnd0` → master | F | 全程 | 不合并直到门禁；D 的 `w-fec` worktree 在合并前映射到 `w-sclc` |
| 7 | `fec=on` 时 legacy 键（`dup`/`replay`/`max_inflate`/`hole_budget_ms`/`rx_reorder_bytes`/`tx_retain_bytes`）的行为 | B 定 | G2 前 | 建议「解析/渲染但数据面不读」；`dup` 绝不能在 FEC 路径复制数据；host 测试 `fec=on`+`dup=1` 无重复发送 |
| 8 | 全局端口块：部署脚本写 `/proc/net/tbtun/ports`（`ports 30000-34095`）；B 补 4096 端口分配 host 测试 | E 已修 `b9adb43`；B 补测试；F 检查 | G2 / G5 | `verify` 比对全局块；`tbt_portmap_set` 4096 端口无截断；256 次 alloc/free；256 carriers 不在第 17 条起 bind 失败 |

---

## 附录 A：40B header 速查

```
+0  type        +1  flags       +2  share_index u16
+4  n           +5  m           +6  epoch u16
+8  block_id u64
+16 seq u64
+24 win_len u16 +26 payload_len u16
+28 conv u16=0x1001             +30 reserved u16=0
+32 payload_crc32c u32          +36 header_crc32c u32
```

## 附录 B：控制帧速查

```
ACK 16:   next_deliver u64 | credit u32 | rtt_us u32
NACK 12:  missing_seq u64 | count u16 | reserved u16
CFG 32:   epoch u32 | n u16 | m u16 | w u16 | k u16 | d u8 | rsv[3]
          | seed u64 | crc32c u32 | rsv u32
FBK 48:   epoch u16 | m_cur u8 | m_tgt u8 | p_ppm u32
          | expected u16 | received u16 | rsv u32 | bitmap[32]
```

## 附录 C：冻结的证据路径

- `review/fec-design.md`（w-fec）：流式滑窗 RLNC 设计、`d=W` 退化修正、HND0 profile。
- `review/chacha8-reference.md`（w-fec-survey）：ChaCha8 参考实现、KAT、内核边界。
- `review/microbench.md`（w-microbench）：HND0/RK3588/Athlon 的 XOR/RLNC 实测。
- `review/sc-ldpc-microbench.md`（w-microbench）：SC-LDPC 原型当前状态（symmetric 未过）。
- `review/crypto-microbench.md`（w-microbench）：ChaCha8 648.6 MB/s、3.39 c/B。
- `review/hnd0-sim.md`（w-fec）：grouped 32×8 恢复率、`d=W/2` 修正。
- `review/final-checklist.md`：A–K/adversarial 的命令与证据格式。
