# 0.3.3 上行 1 GiB byte-exact 验收

本目录的 `verify-uplink-1g.sh` 是 0.3.3 的上行 1 GiB 单流验收。它补的是
fix48 已有的下行 1 GiB：下行由 far 的 `far-raw-serve.py` 发送；上行由 near
生成文件、POST 进隧道、far 落盘，再比较两端 sha256。

## 路径与判定

```
near 生成 1 GiB 确定内容
  -> curl POST http://127.0.0.1:9001/
  -> tbtun 隧道（单流，256 carriers）
  -> far tbtun 转发到 127.0.0.1:5210
  -> far-uplink-recv.py 写入 /root/sclc-uplink-1g/uplink-1g.bin
  -> near/far 两端 sha256 相同
```

硬性判定：

| 检查 | 要求 |
|---|---|
| `curl` 退出码 | `0` |
| HTTP 状态 | `200`（receiver 只在完整 Content-Length 落盘后回 200） |
| `size_upload` | `1073741824` |
| near 文件大小 / far 文件大小 | 均为 `1073741824` |
| near / far sha256 | 均为 64 位十六进制且完全相同 |
| far receiver 日志 | 最后一条 POST 为 `recv=1073741824 result=ok` |
| 两端 `c3 up` | post 与采样最小值、最大值均为 `256` |
| 两端 `c3 replacing` / `empty` | 均为 `0` |
| 两端 reset | `fec_reset_tcp_failed`、`fec_reset_hole`、`stream_reset_tcp_failed` 增量均为 `0` |
| 两端 holes | `holes_unrecoverable` 增量为 `0`（`holes_declared`/`holes_repaired` 仅记录） |
| 两端 deliver_bad | `fec_deliver_bad` 增量为 `0` |
| 内核窗口 | 无 `BUG:`/`Oops`/`WARNING:`/`UBSAN:`/`general protection fault`；空窗口记 SKIP，不冒充 PASS |
| 主机重启 | near/far `boot_id` 不变 |

以下只记录、不作为硬门禁：`fec_reset_user`、`stream_reset_user`、
`holes_declared`、`holes_repaired`、`fec_outq_drop`、`fec_init_bad`、
`fec_replay_miss_enospc`、`fec_nack_*`、CPU、线速、吞吐。0.3.3 的吞吐基线
不在本脚本里造；数据完整性以 sha256 为准。

## 文件

| 文件 | 作用 |
|---|---|
| `verify-uplink-1g.sh` | 驱动 preflight、生成、传输、判定、清理；单次执行，不重试 |
| `far-uplink-recv.py` | far 侧 loopback HTTP 接收器；POST/PUT body 流式写 `.part`，fsync 后 rename，再回 200；HEAD 用于就绪检查 |
| `uplink-sampler.sh` | 两端 1 Hz 采样 c3/c6/c9/c10/c11，证明 up=256 在传输期间没有掉过 |

## 前置条件

- near/far 已 join；模块已加载；`c3 up=256`、`replacing=0`、`empty=0`。
- 配置形状：`mode=sc-ldpc`、`carriers=256`、`adaptive=off`、`fec=on`、
  `crypto=on`；near `listen=*:9001`，far `target=*:5210`。
- near 到 `FAR_IP` 的路由走物理网卡（默认 `eth0`，不接受
  `Meta`/`tun*`/`utun*`/`wg*`/`tailscale*`）；`ip rule` 中已有
  `pref 100 to FAR_IP/32 lookup main`。脚本只读检查，不添加/删除这条 rule。
- near 工具：`curl`、`openssl`、`head`、`sha256sum`、`stat`、`awk`、`ss`、
  `timeout`、`pgrep`/`pkill`；far 工具：`python3`、`curl`、`sha256sum`、
  `stat`、`ss`、`timeout`、`pgrep`/`pkill`。
- 两端 `/root` 至少 1.5 GiB 可用空间。
- far `127.0.0.1:5210` 空闲。若被占用，确认没有并行验收后使用
  `--replace-source`；脚本会记录被停止进程的 pid/cmdline。默认不杀。
- 脚本不写 tunnel config、不 insmod/rmmod、不改 ip rule、不碰 watchdog。

## 身份 pin

从 0.3.3 build report 取两个 srcversion，二者必填：

```sh
export EXPECT_NEAR_SRCVERSION=<near srcversion>
export EXPECT_FAR_SRCVERSION=<far srcversion>
```

`EXPECT_MODULE_VERSION` 默认 `0.3.3`；设为空可跳过模块版本检查。
可选的生成内容 pin：

```sh
# 默认值：AES-256-CTR(key,iv) 前 2^30 字节的 sha256
EXPECT_SHA256=369d49c2faf9dcb2b9ae2d80fd87ac56c42cc37704b70cbf90522d3d99c972f1
# 设为空则只做两端比对，不校验生成内容
```

重算该常量：

```sh
openssl enc -aes-256-ctr \
  -K 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
  -iv 000102030405060708090a0b0c0d0e0f -nosalt < /dev/zero |
  head -c 1073741824 | sha256sum
```

`openssl` 可能在 `head` 关闭管道后打印 `error writing output file`（EPIPE），
这是预期的；生成脚本以文件大小和 sha 为准，不以该条 stderr 判失败。
若改 `AES_KEY`/`AES_IV`，必须同步改 `EXPECT_SHA256`，或把它设为空。

## 用法

在 worktree `/tmp/tbt-033-uplink` 中：

```sh
cd /tmp/tbt-033-uplink
export EXPECT_NEAR_SRCVERSION=...
export EXPECT_FAR_SRCVERSION=...

# 1) 只读就绪检查；不启动 receiver、不生成文件、不过流量
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --preflight

# 2) 单次正式跑；far:5210 被旧 source 占用时加 --replace-source
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --run
# 或
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --run --replace-source

# 3) 保留 PASS 后的 1 GiB 文件（默认 PASS 删除）
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --run --keep

# 4) 查看身份、config、c3、端口、目录
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --status

# 5) 显式清理（停本脚本的 receiver/sampler，删两端 1 GiB 文件）
review/wan-experiment/arm64/sclc-verify/verify-uplink-1g.sh --cleanup
```

`--reuse-source` 只在 near 文件已存在、大小 1 GiB 且 sha 等于
`EXPECT_SHA256` 时复用；否则重新生成。它不是传输重试。

## 超时

| 变量 | 默认 | 作用 |
|---|---|---|
| `SSH_TMO` | 20 s | 普通 ssh/scp 控制命令 |
| `UPLINK_MAX` | 7200 s | near curl `-m`，也是 1 GiB 单流上限 |
| `UPLINK_TMO` | `UPLINK_MAX + 300` | 传输所在 ssh 的外层超时 |
| `GEN_TMO` | 900 s | near 生成 + 远端 `timeout` |
| `SHA_TMO` | 900 s | 1 GiB sha256 |

脚本只发一次 POST，不做传输重试。生成失败、preflight 失败、curl 失败都
直接结束并保留证据。

## 证据

默认证据根目录是 `review/wan-experiment/arm64`，每次运行创建
`sclc-uplink-1g-<UTC>/`：

- `SUMMARY.md`：判定、sha、大小、up、counter 增量、全部 CHECK
- `run.log` / `timeline.txt`
- `curl.txt`、`near-sha256.txt`、`far-sha256.txt`
- `sampler-near.log`、`sampler-far.log`（1 Hz）
- `far-recv.log`、`far-recv-err.txt`
- `near-stats-*`、`far-stats-*`、`near-procstat-*`、`far-procstat-*`、
  `near-netdev-*`、`far-netdev-*`
- `near-kernel.txt`、`far-kernel.txt`（窗口来源见 `KW_SOURCE`）
- `cleanup.txt`，以及 FAIL 时的 `diag-*` 两侧快照

PASS 默认删除两端 1 GiB 文件和远端 helper；FAIL 自动保留
`/root/sclc-uplink-1g/uplink-1g.bin`、`.part`、日志和 helper。确认证据已
复制后再执行 `--cleanup`。`--cleanup` 只清理与本次路径匹配的
receiver/sampler 和两个文件；不卸载模块、不改 tunnel config、不碰 ip rule
和 watchdog。

## 限制

- far receiver 只接受带 `Content-Length` 的 POST/PUT；`curl --data-binary
  @file` 走这条路径。chunked 的 POST 因缺少 `Content-Length` 返回 411，
  不作为验收路径。
- 单流、单连接；并发、多流、丢包注入不在本脚本范围。
- 脚本假设 session 已 join 且配置已写好；它不负责 join、不负责建立
  bypass rule，也不判断 fix48 的短块指标。
- `--replace-source` 会停止 far:5210 上的任意监听者；不要在别的 verifier
  正在使用该 source 时执行。
- 真机执行由后续 verifier 完成；本提交只包含脚本和文档，不含真机结果。
