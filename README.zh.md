# tbtun

一个 Linux 内核 TCP 隧道：把 TCP 流分散到 `N + M` 个独立的五元组上，
让运营商"每流限速"的额度按 `N + M` 份生效；载体停顿时，用重放环里的真实字节补洞，
而不是让一次停顿杀死整条流。

## 工作原理

近端在某个端口上监听。客户端连上来之后，内核接受每条连接，不会把它交给任何用户态进程。
每条连接成为一个流，并发流共用同一组载体。每条流的负载被切成帧，
通过 `N` 条活跃载体连接和 `M` 条备用连接送到远端；每条载体有自己的源端口，
因此各自占用一个独立的限速桶。远端把字节流重新拼起来，连到 `target=`，回程沿原路返回。
接受连接、发起连接、封帧、排序、投递，全部在内核里完成。没有字符设备，没有代理进程，
也没有辅助程序：配置模块的唯一方式是向 `/proc/net/tbtun/config` 写文本。

收益有两个独立来源。按五元组计费是确定的那一个：`N + M` 条流，就有 `N + M` 份额度。
第二个是赌注：`http-kal` 档位把载体包装成普通的 HTTP/1.1 keep-alive 流量，
假设运营商的分类器会给 HTTP 更高优先级。这个假设要被测量，而不是被相信，
见 [QoS 说明](#qos-说明)。如果测量结果相反，`profile=raw` 会关掉这层包装，
按流分散带来的收益仍然存在。

FEC v6（`fec=on`）是可选的第二数据面：应用的字节流被切成 8 KiB 的 DATA share
和 GF(2) band-RLNC 校验 share，每条载体承载一个 share；接收端先用同一块内的校验 share
恢复丢失的 share。最近 `K` 个 super-block 内仍缺的 share 由 NACK 请求
（`K` 是 15 MiB 重放环能容纳的最大值，最多 64）、
发送端从重放环重发；更老的丢失留在载体的 TCP 发送队列里，由 TCP 重传。
它同时支持 `profile=raw` 和 HTTP chunk 容器：
HTTP 档位下一个 chunk body 恰好是一条 48 字节 FEC 记录（`TBT_FEC_CONV=0x100a`；
`0x1009` 是 0.6.0 版本，`0x1008`/`0x1007` 是两条并行的 0.5.0 版本，
`0x1006` 是自适应版本，`0x1005` 是 0.2.2 的 stream-id 版本，0.1.0 为 `0x1001`），
请求行、头部和 chunk 分帧保持明文。`mode=grouped` 把载体分成 `groups` 组，
每组 `n` 个 DATA 加 `m` 个校验，`carriers = groups * (n + m)`，最多 256 条；生产形状是
32 组 `n=4, m=4`。`crypto=on`（`fec=on` 时的默认值）只对 DATA/PARITY 的
负载做 ChaCha8 混淆，48 字节 FEC 头、HTTP 头部与控制帧仍是明文。
`fec=off` 时旧的 ARQ 路径不变。

FEC 码型由 `mode` 选择。`mode=sc-ldpc` 已实现：下行是 binary SC-LDPC 块
（单组 256 shares，`L=8`，rate 0.5），上行是 grouped GF(16) SC-LDPC
（8 组 × 32 shares，`L=16`，rate 1/4）。`w` 是 codeword block（下行 256、
GF(16) 上行 128、512 保留），码率由 `n/(n+m)` 决定。per-direction
`FEC_CFG` INIT 携带 `L`/block/rate，接收端使用对端形状。
`mode=rlnc`/`grouped` 保留为 GF(2) band RLNC 回退。

自适应 FEC（`adaptive=`）在 `fec=on` 时默认开启。GF(2) 的 `mode=rlnc`/`grouped`
保留 Phase 1：形状固定在 `n + m = 8`、32 组的族内，`m` 在 1..6 之间移动。
`mode=sc-ldpc` 使用 per-flow SC-LDPC 估计器/控制器，沿出厂码率阶梯切换
（下行 `n+m=256`，`n` 为 64/76/102/128/151/178；上行 `n+m=32`，`n` 为
8/16/20/24）。切换走 `FEC_CFG`/`FEC_FBK`（wire `0x100a`）。非阶梯形状的
`mode=sc-ldpc` 仍默认 `adaptive=off`；显式 `adaptive=off` 永远优先，
`session <id> adaptive=off` 可在运行时关闭。GF(2) 族的 `n + m != 8` 尚未实现。

每个会话只承载一条 FEC 流（wire `0x100a`）。0.6.0 的子帧复用与其每流接收
窗口记录已经移除，DATA/PARITY 载荷重新就是应用字节。一次完整的 8 KiB 写入直接
成为一个 DATA share（cut-through），`TBT_FEC_SHORT_MS`（2）只用于交互式尾部。
一个 1x256 块是 128 个 DATA share，之后才是校验 share。36 字节 OPEN 的
`route_slot`/`target_port`、64 字节 `FEC_CFG`/`FEC_FBK` 与 GF(16) bit-plane64
映射保留。FEC 头仍携带 stream id（`0x1005` 引入），排队中的旧流记录不会进入新流。

当前代码实现了 [DESIGN.md](DESIGN.md) 的 C1–C8，以及 FEC v6 grouped 数据面与
自适应 Phase 1、256 载体扩展：`/proc` 控制面、监听器与远端、
备用载体与带 RETIRE 握手的载体替换、带重放环的重复发送、保留字节重放的空洞修复，
以及 `http-kal`/`http-kal-drip` 容器。载体字节流里的空洞由重放环补齐，而不是终止整条流。
超过 `hole_budget_ms` 或 `rx_reorder_bytes` 仍补不上的空洞计入 `holes_unrecoverable`；
重排溢出导致的重置计入 `gaps_total`；这两种情况都会重置流。

背压不是丢包。stall 阈值是 5 × 该载体的 RTO（`SRTT + 4×RTTVAR`），
钳制在 1..60 s；`carrier_stall_ms=` 只是上限，只能把它调低。FEC 下 stall
暂停载体：新 share 绕开它，已发未确认的 share 按丢失处理并由 parity 恢复，
只有 `max(4×stall, 30 s)`（上限 300 s）内线路毫无进展时才替换（`stall_dead`）。旧 ARQ
仍然激进：stall 即杀载体，并用 retain 立即改派。空闲载体、投递暂停和对端窗口
关闭都会重置计时器（`stall_idle_skips`、`stall_paused_skips`）。每条载体
socket 还启用 TCP keepalive（空闲 30 s、间隔 10 s、3 次探测），半开路径会先在
socket 层关闭，早于 `stall_dead` 上限。DATA 的准入在
对端通告窗口处停止，因此载体 TCP 发送缓冲里最多只有一个窗口的数据
（`win_block`/`win_resume`）；控制帧不受这个门限约束。窗口检查读取的是 `snd_wnd`，
所以它只是一个启发式判断；窗口未知时只放行一个 chunk。隧道自己的发送窗口按各载体的
BBR 速率样本与聚合通告窗口自动调整；bootstrap 下限与自动上限都随载体数缩放，
`tx_window_bytes=` 可以固定它且不会被截断。被钉住的控制帧在第一次
`-EAGAIN` 时不会被丢弃，而是重试并重新入队；即使队列里没有负载字节，泵也会因
`ctl_queued` 被唤醒。

## 环境要求

- Linux 6.1 或更新版本，以及用于编译的对应内核头文件。
- 如果内核开启了 `CONFIG_DEBUG_INFO_BTF_MODULES=y`，需要 `pahole`（dwarves 包）；
  没有它，模块的 BTF 步骤会失败。
- 加载模块和写 `/proc/net/tbtun/*` 需要 root（文件权限为 `0600`，
  仅调试用的 `http` 为 `0400`）。

## 编译

```sh
make check
```

`check` 会编译 `tbtun.ko`，然后运行宿主机的测试——语法、模糊测试、HTTP 容器，
以及 reorder/ack/control/multistream/anchor 模型；
这些测试不需要 root、不需要加载模块，也不需要内核头文件。只跑这些测试用 `make host-test`。

`test_parse` 现为 357 checks；365 是删除 `framing`/`chunk`/`max_retry`/`dev`/`uid`
五个死键及其解析用例之前的旧快照。删除的是已从语法移除的键的测试，不是行为覆盖；
`make check` 仍为 0 failures。

如果系统没有安装内核头文件，可以改用解包出来的头文件负载，不必使用
`/lib/modules/$(uname -r)/build`；一次性准备步骤、生成的 `env.sh` 以及 pahole 的注意事项见
[docs/build-env.md](docs/build-env.md)。该环境存在时，`make check` 会自动加载它。

## 打包

模块版本只有一个权威：`tbtun/tbt_uapi.h` 中的
`TBT_VERSION_MAJOR/MINOR/PATCH`（当前 `0.8.0`）。
线上版本是 `TBT_WIRE_VERSION=1`、旧帧的 `TBT_CONV=0x000b` 与 FEC 头/FEC 记录的
`TBT_FEC_CONV=0x100b`（48 字节头、64 字节 `FEC_CFG`/`FEC_FBK`），三者都与模块版本独立。
`TBT_CONV 0x0005` 移除了未使用的 PREDICT/CANCEL 占位帧：仍发送它们的对端会在头部检查处
失败，其载体关闭；0x0004 的对端在第一个头部就会因为 conv 不匹配而被拒绝。
36 字节 OPEN 沿用 0.6.0 的定义：32 字节 FEC group bitmap、小端
route slot（近端监听映射槽）与该槽的小端 target 端口。0 表示沿用远端该槽
配置的端口；其他值必须与它相等。`TBT_WIRE_VERSION_V6` 为 8，0.7.3 的
FEC 对端会在第一条 HELLO 就被拒绝，而不是等到第一条 OPEN。
当前 FEC wire 是 `0x100a`：`0x1002` 增加 FIN 的 16 字节 body
（`pkt_end`/`pkt_base`）与 PARITY 空块 marker（`win_len=0`、
`payload_len=0`、`epoch=0`）；`0x1003` 把 ACK 扩到 24 字节并加入
`deliver_bytes`；`0x1004` 把 NACK 变成 64 字节位图修复请求；`0x1005`
在头部 +32 加入 64 位 stream id，并把头部扩到 48 字节；`0x1006` 把
`FEC_CFG` 扩到 48 字节、把 `FEC_FBK` 变成 48 字节报告，并把 HELLO body[0]
升到版本 3，用于自适应形状切换。`0x1007` 曾是 0.4.0 的多流版本：stream-local FEC
seq/block id、32 字节 OPEN group bitmap 与 ALLOC (0x36)；HELLO body[0] 升到
版本 4；`0x1008` 把 `FEC_CFG`/`FEC_FBK` 扩到 64 字节、恢复完整 256 位
carrier 位图，并把 HELLO body[0] 升到版本 5。并行的 0.5.0 多流线也占用
`0x1008`，用于 32 字节子帧与每流接收窗口记录 (0x37)，因此同一个号在两线含义不同。
`0x1009` 曾是 0.6.0 的合并切点：36 字节 OPEN（`route_slot`/`target_port`）、
64 字节 `FEC_CFG`/`FEC_FBK`、GF(16) bit-plane64 映射、32 字节子帧流、
16 流 1x256 平面与每流接收窗口记录 (0x37) 合成一份契约，HELLO body[0] 升到版本 6。
0.7.0 再次移除子帧复用、每流接收窗口记录与 `FEC_ALLOC`：`0x100a` 保留 48 字节头、
所有偏移与 0x30..0x35 记录类型，HELLO body[0] 升到版本 7。三个 wire 版本常量
一起移动；0.6.0 对端会在首个 HELLO 与首条 FEC conv 检查处失败，必须整对升级。
`scripts/mkdkmsconf.sh` 派生
`PACKAGE_VERSION="<major>.<minor>.<patch>"`；`dkms.conf` 是生成物，由
`make dkms.conf` / `make dkms-tarball` 写出，不要手改。
`scripts/install_dkms.sh` 通过 `mkdkmsconf.sh --version` 读取同一个值。
开发构建可用 `TBT_DKMS_GIT_SUFFIX=1` 追加 `.r<commits>.<sha>`；
默认是纯语义版本。

## 加载

```sh
sudo insmod tbtun.ko debug_loss=0
```

模块参数在加载后只读：

| 参数 | 默认值 | 含义 |
|---|---|---|
| `debug` | `0` | 日志详细程度，`0..3` |
| `max_tunnels` | `64` | 最多可配置的隧道数；新 id 超过上限返回 `ENOSPC` |
| `max_carriers` | `256` | 每个会话的载体槽，`1..256`；`n + m` 或 `carriers` 超过上限返回 `EINVAL` |
| `debug_loss` | `0` | 是否允许写 `drop_ppm`（确定性丢包注入） |
| `hold_enable` | `1` | BBR hold：载体空闲 200 ms 后，50 ms poll 保存 cwnd/pacing（受 `snd_wnd/mss`、`snd_cwnd_clamp`、`sk_sndbuf/mss`、`sk_max_pacing_rate` 约束），并在会话持续空闲且 TCP 无在途字节时恢复两者；非 PAD 写入会先取消 hold。默认开启；写 `0` 关闭。它直接写 `tcp_sock` 字段，不是稳定的内核 API |

控制面只在 `init_net` 中创建；没有 per-netns 视图。
卸载用 `sudo rmmod tbtun`。卸载会清空所有隧道，关闭所有监听端口和载体。

### 生产部署

两端必须运行同一个修复版本。2026-11-06 的现场崩溃是模块内部的内存破坏：
`tbt_carrier_new_locked()` 用 `kmalloc()` 分配每条载体的 `parser` 与 `http_rx`，
复用 slab 对象时 `parser->have` 可能是脏值，而 `tbt_http_rx_parse()` 把它当作
`memmove()` 的长度。表现是加载后几秒内的 `Internal error: Oops`，常常落在无关
进程里，因为破坏是 slab 级的。新旧混搭不会"基本能用"：未修复的那一端自己就会
破坏内存，与对端做什么无关。

`/sys/module/tbtun/srcversion` 是身份门，因为线上没有任何字段能区分这两个构建。
把已加载的模块和你要部署的那个文件对一下：

```sh
modinfo -F srcversion tbtun.ko      # 加载前
cat /sys/module/tbtun/srcversion    # 加载后
```

`srcversion` 是每个构建各自的哈希，所以两端架构或内核配置不同时，近端与远端的值
本来就该不同（现场那一对是 aarch64 的 `72168C1C…` 与 x86_64 的 `332759AF…`，
源码相同）。要求的是每台主机对上自己的产物；两端都要查，每次重新部署后都要再查。

`debug=0` 是生产设置，也正是崩溃后现场所剩无几的原因：按包、按轮的诊断都由
`debug` 把关，最多只剩一次性的 `tbtun-diag:` bring-up 行（会话与载体创建、
最初几轮 poll、第一个接收窗口）。可见性要在需要之前就搭好：

- journald 持久化（`Storage=persistent`，或者至少有 `/var/log/journal` 目录），
  这样上一次启动的记录能活过重启。
- `sysctl -w kernel.panic_on_oops=1 kernel.panic=10`：被破坏的内存触发 Oops 时，
  十秒后重启，而重启本身就是"出事了"的信号。不设的话，主机会带着被污染的 slab
  继续跑，证据散落在互不相关的进程里。
- 保持 ramoops 已注册、每次加载前 `/sys/fs/pstore` 为空。它是唯一能活过 panic 的通道。
- netconsole 在 `rk_gmac-dwmac` 上只能算尽力而为，不能当主通道：`netconsole` 是模块，
  用户态加载它之前的任何事都覆盖不到；`eth0` 一重新注册，它自己就会断开
  （`network logging stopped on interface eth0 as it unregistered`）。
  主通道改成用户态转发 `/dev/kmsg`，netconsole 留作第二份副本。

受控检查只跑一次，不要循环：

```sh
# 前提：隧道已配置好并监听 127.0.0.1:9001
timeout 10 ./host/tbt_burst 127.0.0.1 9001 1 5 interleaved   # 1 MiB，5 秒
cat /proc/net/tbtun/stats | head -3
dmesg | tail -20
```

重试会掩盖它本该暴露的问题：第二次加载用的正是第一次已经破坏过的 slab，
所以第二次干净通过什么也证明不了。完整流程（身份核对、可见性预检、join 门、
单次 burst、证据收集）是 `review/wan-experiment/arm64/controlled-c8-once.sh`。

## 双机示例

近端主机 `10.0.0.1`，远端主机 `203.0.113.9`，远端有一个服务监听在 `127.0.0.1:5201`。

在近端执行：

```sh
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=1' \
     > /proc/net/tbtun/config
```

在远端执行：

```sh
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=1 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

然后在任意一端查看：

```sh
cat /proc/net/tbtun/tunnels
```

此时近端在 `10.0.0.1:9000` 上监听。连到这个端口的客户端，会经四条载体被送到远端的
`127.0.0.1:5201`。可以用 `iperf3 -c 10.0.0.1 -p 9000 -t 60 -P 1` 对远端的
`iperf3 -s` 做测试。

两端必须在隧道 id、`n`、`m`、`w`、`mode`、`carriers` 以及 profile 家族上保持一致。远端的 `listen` 是载体到达的地址，
近端的 `peer` 是载体发往的地址。远端的 `peer` 是锚：只有来自 `10.0.0.1` 的 HELLO 才能加入，
far `peer=` 里的端口只是语法占位。给两行加上相同的 `nonce=<32 个 hex 字符>` 可以固定会话
nonce。没有 `nonce=` 时，远端会采纳第一条 HELLO 的 nonce；如果连 `peer=` 也没有，
这条 HELLO 还会固定锚，并打印一次首连信任告警。地址接受字面 IPv4
（`a.b.c.d:port`）或方括号 IPv6（`[addr]:port`）；本版本不支持 DNS。

HTTP 容器上的 FEC v6：一组六条载体（四条 DATA、两条校验）：

```sh
# 近端
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=2 fec=on carriers=6' \
     > /proc/net/tbtun/config
# 远端
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=2 fec=on carriers=6 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

grouped 会话每条载体一个 share：`carriers = groups * (n + m)`，
`groups = carriers / (n + m)`。生产形状是 32 组 `n=4, m=4`，共 256 条载体：

```sh
# 近端
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=4 fec=on mode=grouped carriers=256' \
     > /proc/net/tbtun/config
# 远端
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=4 fec=on mode=grouped carriers=256 peer=10.0.0.1:9000' \
     > /proc/net/tbtun/config
```

全局包序号按组条带化（`group = seq % groups`）。每组有自己的块窗口和校验，
接收端只有在跨度内每组的块都解码后才投递这个全局跨度。载体 `c` 永远承载
组 `c / (n + m)` 的 share `c % (n + m)`；替换载体保持同一序号。

默认档位是 `http-kal`，所以每条 48 字节 FEC 记录就是一个 HTTP chunk body，
请求行、头部和 chunk 分帧保持明文。`profile=raw` 去掉 HTTP 包装，记录本身不变。

## 能观测到什么

模块知道的全部内容都在 `/proc/net/tbtun` 下。文件仅 root 可访问：除仅调试用的
`http` 为 `0400` 外，其余为 `0600`；没有辅助程序，`echo` 和 `cat` 就是全部接口。

| 文件 | 读它能看到什么 |
| --- | --- |
| `config` | 规范形式的配置；隧道行、`del`、`flush`、`carrier ...`、`session ...` 动作都写到这里 |
| `tunnels` | 每条隧道一行：角色、状态、listen/peer/target、n、m、streams |
| `peers` | 每个载体槽一行，共 19 列（见下） |
| `ports` | 全局后备源端口段，以及它是否已经配置 |
| `stats` | 前三行是 `debug_loss=<0\|1>`、空洞与流总数，之后每条隧道一个计数器块（见下） |
| `http` | 仅调试（`debug>=2`）：近端 0 号载体最近一次**发出的** HTTP 请求头块（只有头部），用来对照解析器检查容器；不是收到的请求、不是响应、也不是实时流 |

除非加载时带 `debug_loss=1`，否则 `drop_ppm` 会被拒绝并返回 `EPERM`；
`stats` 的第一行就是这个开关的状态，脚本可以先读它再决定是否注入。

### `config`（读写）

一行一条指令。只有整行到达时才会生效：遇到换行符，或者 `close()` 时冲刷最后一段没有换行的内容。
非法行会让 `write()` 返回 `EINVAL`，`echo` 因此会明确失败，并且该行不会有任何部分被应用。
超过 1023 字节的行会被拒绝并返回 `E2BIG`。整行以 `#` 开头的注释会被忽略；
写在某个 token 之后的 `#` 不算注释，整行会被拒绝。

`tunnel <id>` 接受以下键：

| 键 | 默认值 | 含义 |
|---|---|---|
| `role` | — | `near` 或 `far`，必填 |
| `listen` | — | `a.b.c.d:port` 或 `[v6]:port`；近端为对外地址（`[::]` 绑定 IPv6 通配地址，是否同时接受 IPv4 客户端取决于内核 `bindv6only`），远端为载体地址 |
| `peer` | — | `a.b.c.d:port` 或 `[v6]:port`；近端：载体的目的地；远端：唯一允许加入载体的近端源地址（锚），端口只是语法占位 |
| `target` | — | `a.b.c.d:port` 或 `[v6]:port`；远端：必填的字面目的地；slot 0 的目的地，也是 OPEN 里端口为 0 时的默认值。近端：可选的 slot 0 target 端口，地址只是语法占位（远端始终拨自己的 `target_addr`） |
| `map` | — | `listen_port:target_port[,listen_port:target_port...]`，最多 7 个附加槽；只映射端口，所有映射监听都绑定在 `listen=` 的地址上。近端监听每个 listen 端口并把 target 端口写进 OPEN；远端把这些 target 端口作为允许的目的地（listen 半边不绑定）。slot 0 就是 `listen=`/`target=`。近端拒绝与 `listen=` 或彼此重复的 listen 端口 |
| `n` | `4` | 每组的 DATA share 数，`1..255`；`n + m <= 256` |
| `m` | `1` | 每组的校验 share 数，`0..255`；`fec=on` 需要 `m >= 1`；`n + m <= 256` |
| `dup` | `1` | 重复度，`0..3` |
| `keepalive` | `5000` | 空闲 PAD 间隔，毫秒。载体的 TCP keepalive 固定为 30 s / 10 s / 3 次探测，不是这个键 |
| `drip_ms` | `1000` | `http-kal-drip` 空闲时每字节的间隔，毫秒 |
| `replay` | `8388608` | 重放环字节数 |
| `tunnel_rate` | `0` | 字节/秒上限，`0` 表示不设 |
| `max_inflate` | `2.000` | 重复膨胀上限 |
| `hole_budget_ms` | `0` | 空洞允许存在的时间，毫秒，`0..60000`；`0` 表示按 RTT 自动推导（下限 2 s） |
| `rx_reorder_bytes` | `67108864` | 接收端重排预算，字节，`1..268435456`；同时约束每条流的重排树和全会话总和；`0` 会被拒绝 |
| `tx_window_bytes` | `67108864` | 发送端窗口字节数；`0` 表示按各载体的 BBR 样本与聚合通告窗口自动推导（上限随载体数缩放）；显式值不得大于 `rx_reorder_bytes`，且不会被截断 |
| `tx_retain_bytes` | `0` | 发送端保留字节数，`0` 表示不限 |
| `carrier_stall_ms` | `0` | stall 阈值的上限，毫秒，`0..3600000`；`0` 表示自动：5 × RTO（`SRTT + 4×RTTVAR`），钳制在 1..60 s。显式值只能调低自动阈值 |
| `profile` | `http-kal` | `http-kal`、`http-kal-drip` 或 `raw` |
| `fec` | `off` | `off`（旧 ARQ）或 `on`（FEC v6 数据面） |
| `adaptive` | `fec=on` 时 `on` | `on` 让控制律移动 `n/m`；`on` 需要 `fec=on`；`mode=sc-ldpc` 仅在出厂码率阶梯形状上默认 `on`，否则 `off`；显式 `adaptive=off` 永远优先 |
| `mode` | `rlnc` | `rlnc`（单组）、`grouped`（GF(2) band RLNC），或 `sc-ldpc`（下行 1×256 binary、上行 grouped GF(16)；`w` 是 codeword block） |
| `w` | `0` | RLNC 窗口，`0..1024`；`0` 解析为 `2*n` |
| `carriers` | `0` | 载体槽，`0..256`；`0` 解析为 `n + m`；必须是 `n + m` 的整数倍 |
| `crypto` | 跟随 `fec` | `on` 对 FEC 的 DATA/PARITY 负载做 ChaCha8 混淆；`on` 需要 `fec=on`；不写时等于 `fec` |
| `crypto_key` | — | 64 个 hex 字符的预共享 256 位密钥；`-` 清除。不配置时由近端生成并在 HELLO 中明文携带（`key_id=1`） |
| `portblock` | `0-0` | 仅近端：本条隧道载体的源端口窗口，`1..65535`；`0-0` 表示使用 `/ports` 的全局端口段。至少容纳 `n + m` 个端口（FEC 时为 `carriers`）。远端只记录，不拨号 |
| `dev` | — | 仅近端：拨号前把每条 carrier socket 用 `SO_BINDTODEVICE` 绑到该网卡，`1..15` 字节。绑定失败按拨号失败处理：`connect_fail` 增加，200 ms 后重试。用于多网卡或代理 TUN 环境强制直连 |
| `srcaddrs` | — | 逗号分隔的本地源地址，IPv4 或 IPv6 字面量；只有第一项被使用；近端 `srcaddrs=` 必须与 `peer=` 同地址族 |
| `nonce` | — | `16` 或 `32` 个 hex 字符：固定会话 nonce；近端本地使用，远端要求对端使用 |

曾被解析但从未生效的键（`framing=`、`chunk=`、`max_retry=`、`uid=`）
已删除；未知键会返回 `EINVAL`。

`nonce=` 固定 128 位会话 nonce。32 个 hex 字符是完整值；16 个 hex 字符固定
低 64 位，并在规范输出中左补零。`role=near` 时它是本端放进 HELLO 的 nonce；
`role=far` 时它是远端要求近端使用的 nonce，每一条 HELLO 都必须匹配。未配置
`nonce=` 时，far 会采纳第一条 HELLO 的 nonce；如果连 `peer=` 也没有，这条 HELLO
还会固定锚（首连信任，TOFU），模块会打印一次告警。第一条通过 nonce 检查的
HELLO 的源地址会成为会话锚：之后来自其他地址的 HELLO 在检查 nonce 之前就被拒绝
（`join_src_reject`）；far 的 `peer=` 则在创建会话时就固定锚。加入计数器区分原因：
`join_bad` 是 nonce 不匹配，`join_nonce_reject` 是未固定 nonce 时第一条 HELLO
来自 far `peer=` 之外的地址，`join_dup` 是槽位已被占用。nonce 在 HELLO 里明文
传输：它是会话标识与反混淆 token，不是密码；链路不可信时套 WireGuard。
64 个 hex 字符（256 位）会被拒绝：HELLO 的 nonce 字段只有 128 位。

其它指令：`del <id>`、`flush`、`carrier <index> down|up|drop_ppm <n>`，
以及 `session <id> nonce=<hex>`（固定会话 nonce，用于可复现的测试）。
`drop_ppm` 需要加载时带 `debug_loss=1`，否则会被拒绝并返回 `EPERM`。

读取 `config` 得到的是规范形式：所有标量键按固定顺序写出，默认值也会显式写出。
这段文本可以逐字节往返，所以
`cat /proc/net/tbtun/config > /etc/tbtun.conf` 与
`cat /etc/tbtun.conf > /proc/net/tbtun/config` 能精确恢复保存时的状态。
读取按 offset 从同一份快照返回，所以配置再大也能完整读出；每次写入后快照会重建。

### `tunnels`（只读）

先输出 `# id role state listen peer target n m streams`，然后每条隧道一行。

### `peers`（读写）

每个载体槽一行，共 19 列：

| 列 | 含义 |
| --- | --- |
| `id` | 隧道 id |
| `carrier` | 会话内的载体序号 |
| `sport` | 本地源端口，取自端口段 |
| `peer` | 对端地址:端口 |
| `uack` | 对端已确认的最大字节偏移 |
| `rtt_us` | 平滑后的往返时延，微秒 |
| `inflight` | 已写出但尚未确认的字节数 |
| `state` | `connecting`、`up`、`spare`、`stalled` 或 `down` |
| `class` | 该载体使用的容器（`raw`、`http-kal`、`http-kal-drip`） |
| `recv_cnt` | 该载体收到的记录数 |
| `rx_lag_ppm` | 接收端尚未统计的、位于发送端写高水位以下的记录数，单位百万分之一；包含在途记录，因此不是丢包率 |
| `minrtt_us` | 观测到的最小往返时延，微秒 |
| `tx_frames` | 该载体写出的记录数 |
| `tx_bytes` | 该载体写出的字节数，含头部 |
| `dropped` | 被 `drop_ppm` 注入器丢弃的记录数（仅调试） |
| `retx_ppm` | 每百万条唯一 DATA 记录中的重传副本数，这才是丢包的代理指标 |
| `delivery_rate_bps` | 该载体的 BBR 速率样本，字节/秒（自动窗口的输入） |
| `cwnd_bytes` | 该载体 TCP 的 `snd_cwnd`，字节（仅作交叉检查，不是输入） |
| `snd_wnd` | 对端通告窗口，字节 |

写入 `carrier <index> down|up|drop_ppm <n>` 会对所有拥有该载体序号的运行中隧道生效。

### `stats`（只读）

前三行是全局前缀：`debug_loss=<0|1>`（`drop_ppm` 的开关）、`gaps_total`
（重排溢出导致的重置次数）、`streams_total`。之后每条隧道一个块。
有多个写者的计数器是 `atomic64_t`，通过同一个访问器读取，因此每个值都是一次加载；
只有一个写者的计数器是普通 `u64`，并以写者命名。块按所属的设计步骤分组：
最前面是捕获与 RX 工作，然后是 `dbg` 行和 `live` 快照、`txwake`、`c3`
（载体、加入、重传、重排、门限）、`ctl`（被钉住的控制帧）、`streams`（流普查）、
`c6`（空洞）、`c8`（HTTP）、`c9`（FEC 接收判定）、`c10`（FEC 丢失原因）、
`c11`（NACK/重放修复）、`c5`（重复），最后是
`reconcile`。

| 键 | 含义 |
| --- | --- |
| `submit_bytes`、`wire` | 提交的应用字节数 / 载体写出的字节数（含头部） |
| `data_tx`、`retx_frames`、`retx_bytes` | 写出的 DATA 记录数（含副本）/ 副本数 / 副本负载 |
| `retx_hole`、`retx_slow`、`retx_pathloss`、`retx_other` | 按来源分的副本：补洞（ack 处的记录停顿至少 2×RTO）、慢路径宽限（TCP 已送达，但 3×RTO 后 ack 仍未覆盖）、路径丢失、其它 |
| `retx_data_ppm`、`dup_bytes_ppm` | 每百万条唯一 DATA 记录中的副本数 / 每百万应用字节中的副本负载 |
| `port_exhaust`、`port_alloc_fail` | 本隧道的源端口窗口耗尽 / 端口位图分配失败；载体重试，不会杀死任何东西 |
| `join_src_reject`、`join_nonce_reject`、`join_bad`、`join_dup` | HELLO 被锚、首条 HELLO 的来源、nonce 或已占用槽位拒绝 |
| `ctl_lost`、`ctl_retry`、`ctl_requeue`、`ctl_obsolete`、`ctl_queued` | 被钉住的控制帧：重试上限后的显式丢失、重试、重新入队、过期代、当前队列深度 |
| `stall_paused_skips`、`win_block`、`win_resume` | 暂停时推迟失效判定 / DATA 在对端窗口门限处被挡住 / 放行 |
| `stall_idle_skips`、`stall_declared`、`stall_recovered`、`stall_drop`、`stall_dead` | 因没有未确认字节而重置计时 / 暂停了一个 FEC 载体 / 该载体 ack 又开始前进 / 被 ARQ 暂停助手丢弃的帧（当前无调用者，恒为 0）/ 暂停载体超过 `max(4×stall, 30 s)`（上限 300 s）无进展而被替换 |
| `tx_window`、`rtt_mean_us`、`rttvar_us`、`rtt_max_us`、`txq_max` | 生效的发送窗口及其输入 / 帧队列上限 |
| `holes_declared`、`holes_repaired`、`holes_unrecoverable`、`rx_max_stall_us` | 空洞统计 |
| `dup_tx`、`dup_used`、`dup_suppressed`、`inflate_ppm` | 重复发送 |
| `c9` 行（`fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、`fec_short_tx`、`fec_short_unknown`、`fec_drop_window`、`fec_drop_crypto`） | FEC 接收判定：头/CRC 失败关闭载体，payload CRC 失败只丢该 share；`fec_drop_crypto` 统计密钥就绪前到达的 share |
| `c10` 行（`fec_decode_blocks`、`fec_decode_recovered`、`fec_decode_nomem`、`fec_deliver_nomem`、`fec_deliver_dup`、`fec_deliver_ooo`、`fec_deliver_bad`、`fec_dup_share`、`fec_late_share`、`fec_late_data`、`fec_pad_share`、`fec_epoch_conflict`、`fec_slot_past_cut`、`fec_fin_bad`、`fec_ack_bad`、`fec_tx_share_gap`、`fec_empty_block`） | FEC share/块丢失的每种原因：解码/投递失败、重复/迟到/填充 share、epoch 冲突、越过块切口的槽、坏 FIN/ACK、发送端 share 缺口、空块。若流停住而它们全为 0，说明丢失不在这几条路径上 |
| `c11` 行（`fec_nack_tx`、`fec_nack_rx`、`fec_nack_retx`、`fec_nack_drop`、`fec_nack_bad`、`fec_replay_hit`、`fec_replay_miss`、`fec_nack_retx_bytes`、`fec_replace_retx_bytes`、`fec_nack_lat_ns`、`fec_nack_lat_max_ns`、`fec_nack_repair_ns`、`fec_nack_repair_max_ns`、`fec_reset_tcp_failed`、`fec_reset_user`、`fec_wait_carrier`、`fec_backpressure`） | NACK/重放修复：NACK 收发、重放 share 数、重放命中/未命中、重放字节、修复时延与 reset 原因。`fec_reset_tcp_failed` 是唯一由 FEC 触发的 reset，要求所有 carrier TCP 都已失败；`fec_wait_carrier`/`fec_backpressure` 表示生产者在等可用载体或源环已满 |
| `reconcile live_*`/`retired_*` | 活跃 + 已退休等于会话总数（字节、DATA、重传） |

`reconcile` 行用来确认载体替换没有让字节凭空消失：
`wire == live_bytes + retired_bytes`、`data_tx == live_data + retired_data`、
`retx_frames == live_retx + retired_retx`。

### `ports`（读写）

`ports <lo>-<hi>` 设置载体的全局源端口段；默认是 `40000-44095`
（4096 个端口 = 256 条载体 × 16 次重建）。
空行或 `ports 0-0` 是 no-op，不会改变当前端口段。读取该文件会输出端口段、
它是否已经配置，以及那条指令。

模块维护一张覆盖整个 16 位源端口空间的位图。隧道的 `portblock=` 是这张位图上的
一个窗口；`0-0` 表示改用这里的全局端口段。窗口可以重叠：它们共享容量，分配器绝不会
把同一个仍在使用的源端口交给两条载体。因此窗口是搜索范围，不是硬预留；需要隔离时
请使用互不重叠的窗口。近端窗口至少要容纳每个载体槽一个端口（`n + m`，FEC 时为
`carriers`），否则整行配置被拒绝。修改 `/ports` 立即影响新分配；已经建立的载体保留
原端口直到关闭。修改隧道的 `portblock=` 需要重发整条隧道行，这会替换隧道及其会话。
所有用到的端口段都要写进 `net.ipv4.ip_local_reserved_ports`，不只是全局段。

## 验证过的性能

数字来自 [benches/](benches/)：在 KVM 客户机里通过同样的操作接口驱动，
virtio-net，每个源端口一条 10 Mbit 的 htb 类并挂 `delay 20ms 10ms` 的 netem，
用 `tbt_burst` 的严格先发后读阶段传 40 MiB（逐字节校验），每次启动只测一种配置。
对照组是单个 `dd | nc` 流，同样走在一条被整形的五元组上。`app` 是近端隧道的
`submit_bytes` 速率；`wire/app` 是整次运行的载体字节除以应用字节；`retx_ppm`
是每百万条唯一 DATA 记录中的副本数。被测模块是 sha256 `0b8f0006…`，也就是
最终树 `f1282df` 构建出的模块；原始 CSV、console 与验收表在
`review/final-bench/`。

| 载体数 | 对照 (Mbit/s) | 应用 (Mbit/s) | 应用/对照 | wire/app | retx_ppm | strict / rc | CSV |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `n=1` | 2.43 | 3.11 | 1.28x | 1.0198 | 0 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T003712`) |
| `n=4` | 2.73 | 10.74 | 3.93x | 1.0138 | 2,333 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004314`) |
| `n=8` | 2.58 | 22.36 | 8.67x | 1.0123 | 2,041 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004635`) |
| `n=16` | 2.63 | 32.75 | 12.45x | 1.0268 | 12,719 | `strict=1`, `rc=0` | `review/final-bench/` (`20261106T004925`) |

每一行都逐字节校验通过，`holes_unrecoverable=0`；整段 `wire/app` 范围是
1.0123–1.0268，`retx_ppm` 是 0–12,719，所有行都满足当前目标
（`wire/app < 1.2`、`retx_ppm < 100k`）。表中的 `wire/app` 是整次运行的比值；
CSV 的 2 s 窗口列在 `n=4` 读到 1.0333。同一模块（`0b8f0006…`）的一次更早
`n=4` 运行是 8.38 Mbit/s（`wire/app` 1.0146、`retx_ppm` 1,842），另一次是
11.37 Mbit/s，所以方差是真实的；比值才是主张。`n=16` 是单载体应用速率的
10.5 倍。完整矩阵与 CSV 在 `review/final-bench/summary.md`。

代码树 `f1282df`（模块 `0b8f0006…`，证据提交 `af3b9ba`）的验收证据：主机、
QEMU A–K（11/11）与 adversarial（18 组外加崩溃复现，共 91 PASS / 0 FAIL / 0 SKIP，
284.4 s）在 `review/final/` 和 `review/final-qemu/`；吞吐、flush 与 writer 在
`review/final-bench/`；干净客户机内核上的 DKMS 安装/加载/卸载全流程在
`review/dkms-qemu/`。

writer 探针覆盖两种契约。`pub.writer` 中应用从不读取，一直阻塞在 `write()` 内：
100 MiB 那次仍阻塞在 `sent=73400320/104857600`，1 GiB 那次仍阻塞在
`sent=74448896/1073741824`，两者都是 `errno=0 eagain=0 blocked=1`，
没有 `EAGAIN`（探针未设置 `SO_SNDTIMEO`）；
内存有界（`oom=0->0`），flush 在 0.03 s 内归还内存。`pub.writer_drain` 与
`pub.writer_drain_1g` 中应用持续读取，写能完成：100 MiB 为
`sent=104857600/104857600`、`got=104857600/104857600`，71.5 MiB/s，用时 1.40 s；
1 GiB 为 `sent=1073741824/1073741824`、`got=1073741824/1073741824`，69.9 MiB/s，
用时 14.64 s。两者都是 `errno=0/0 eagain=0 blocked=0 oom=0->0`，flush 用时 0.01 s。
详见 `review/final/README.md`。

比值仍是主张：`N` 个五元组胜过一条流。这些是 QEMU/slirp 实验室数字，
每个源端口一个 10 Mbit htb 类，netem `delay 20ms 10ms`；不是 ISP 实测。
绝对值取决于这台机器的 QEMU/slirp 路径，而不是 10 Mbit 的桶。

下表的代价数据来自更早的 bench；本次 `n=1`/`n=4` 吞吐运行没有重新测量它。

| 代价 | 实测 |
| --- | --- |
| `http-kal` 容器相对 `raw` | 字节数约多 1%（每约 4 KB 记录一个 chunk），单 vCPU 的系统态 CPU 约多 3.5 个百分点 |
| 补洞与每路径丢包 | 拐点在第一个 1%：应用吞吐 9.85 → 6.91 Mbit/s，之后到 10% 基本持平在 5.40；1%、5%、10% 三档的 `holes_unrecoverable` 都是 0 |
| 时延（1 KB 回显） | 相对直连路径 p50 增加 156 µs、p99 增加 274 µs；补一个空洞会把 p99 推到约 24 ms，最大 32 ms |

这些是单台机器上的实验室数字，说明不了运营商是否真的给 HTTP 更高优先级；
后者要用 [DESIGN.md §17](DESIGN.md) 在真实线路上测。

## 已知限制

- **没有 DNS。** 所有地址都是字面量：`listen=`、`peer=`、`target=` 接受
  `a.b.c.d:port` 或 `[ipv6]:port`，`srcaddrs=` 接受 `a.b.c.d` 或 IPv6
  字面量；不做域名解析。
- **默认不加密，也不认证；`crypto=on` 只是混淆。** `crypto=on`（`fec=on`
  时的默认值）对 FEC 的 DATA/PARITY 负载做 ChaCha8 混淆，目的是把应用字节
  从朴素的 DPI 特征里藏起来；48 字节 FEC 头、HTTP 头部和控制帧仍是明文，
  而且没有认证。未配置 `crypto_key=` 时，近端生成 32 字节会话密钥并在
  HELLO v6 body 中明文发送（`key_id=1`），远端采纳；两端都配置
  `crypto_key=` 时 `key_id=0`，HELLO 的 key 字段为零，密钥不匹配表现为每个
  share 的 payload CRC 失败。nonce 是会话标识，不是密钥。路径不可信时套
  WireGuard；此时只应选 `raw` 档位，因为 HTTP 解析器看不到 TLS 内部。
- **FEC 可选；grouped GF(2) 是生产形状。** `fec=on` 支持单组
  （`carriers == n + m`）和 grouped（`carriers = groups * (n + m)`，最多
  256 条载体），`raw` 与 `http-kal` 容器都支持。发送端每组每块发 `n` 个
  DATA share 加 `m` 个校验 share；接收端在块的校验足以解出缺失 share 时
  解码，然后按全局序号投递整个 super-block。修复有界：接收端用 NACK
  （记录类型 `0x33`）只为最近
  `K` 个 super-block 内最老的未完成块发 NACK（`K` 是 15 MiB 重放环能容纳
  的最大值，最多 64），发送端从该重放环
  在任意 up 且非 stalled 的载体重发。窗口内是 RTT 级修复（一次 NACK 往返
  加重放写入），不是 30 s 计时器。窗口外的丢失不发 NACK，留在载体 TCP
  发送队列里由 TCP 重传。就丢失恢复而言，只有所有 carrier TCP 都失败
  （没有 ESTABLISHED socket 且低于 `tcp_retries2=15`）或用户/对端关闭时才
  重置流；30 s FEC 计时器只重新计时。协议违规（乱序投递、坏头）仍然
  fail-closed。每个 FEC 会话的重放环与 RX arena 是常数（15 MiB + 16 MiB），
  不随流长度增长。流的最后一块用零填充，
  真实长度放在校验的 `win_len`（FIN 偏移是第二来源），所以最后一段短包不会
  被当作填充投递。`fec=off` 时丢包靠重放环重传与重复发送修复，没有校验块或
  纠删码；环里不再保留这些字节时，接收端重置流（`holes_unrecoverable`），
  而不是猜测。
- **SC-LDPC 与其 per-flow 自适应已实现。** `mode=sc-ldpc` 运行下行 binary
  块（1×256）或上行 grouped GF(16)（8×32，`L=16`）；`adaptive=on` 沿出厂
  码率阶梯双向切换。`rlnc`/`grouped` 保留为 GF(2) band RLNC 回退。
- **自适应 FEC。** GF(2) 族保留 Phase 1：`adaptive=on` 只在固定的 `n + m = 8`、
  32 组族内移动 `m`；`n + m != 8` 尚未实现。`mode=sc-ldpc` 沿阶梯移动配置的
  split；非阶梯 split 保持固定。`adaptive=off` 保持配置的形状。
- **单流 FEC。** 每个会话承载一条 FEC 流（`0x100a`）。DATA/PARITY 载荷就是
  应用字节，一次 8 KiB 写入就是一个 DATA share，没有 per-stream 解复用或窗口状态。
- **每条远端隧道只有一个锚。** 一个 `peer=`（或第一条 HELLO）固定源地址。
  近端多宿主，或者 NAT 重绑源地址，都会让后续 HELLO 被拒绝（`join_src_reject`）；
  请保持一个稳定的源地址，例如用 `srcaddrs=`。
- **不固定就是首连信任。** 既没有 `nonce=` 也没有 `peer=` 时，第一条 HELLO
  同时固定 nonce 和锚，模块会打印一次告警。路径不可信时把两者都固定。
- **吞吐会逐次波动。** 最终 `n=4` 运行是 10.74 Mbit/s（`retx_ppm` 2,333，
  整段 `wire/app` 1.0138）；同一模块的一次更早运行是 8.38 Mbit/s
  （`retx_ppm` 1,842、`wire/app` 1.0146），另一次是 11.37 Mbit/s。最终
  `n=1`/`n=8`/`n=16` 分别是 3.11/22.36/32.75 Mbit/s；完整矩阵在
  `review/final-bench/`。
- **发送窗口取 `k × mean BDP` 与载体聚合的较大者。** `tx_window_bytes=0` 以
  `sum(snd_wnd)`（没有窗口样本时用 `n_car × 单载体 BDP`）作为单流
  `k × mean BDP` 目标之上的下限，bootstrap 下限为
  `max(1 MiB, n_car × 8 × payload)`。`k =（RTO + 单向延迟 + 反馈延迟）/
  mean SRTT`（实测路径上约 2–5）。自动上限与接收端重排预算都随载体数缩放
  （至少 8 MiB、每载体 8 个整帧、上限 256 MiB），因此 256 载体不会被单路径的
  8 MiB 卡住。显式 `tx_window_bytes=` 是运维契约，不会被截断。重排预算仍必须
  覆盖窗口，因为重排树的峰值占用就是整个窗口。两端应使用相同的
  `rx_reorder_bytes`，因为接收端的预算不在线上传输：发送端只能用自己的配置值
  给自己封顶。将来由 WINDOW 帧通告接收信用后，这个配置契约就不再需要。
- **重排树和就绪队列是两笔不同的开销。** 乱序到达的帧等待在隧道的重排树里
  （`rx_reorder_bytes`，默认 64 MiB，全会话共享）；已按序到达但应用尚未读取的字节
  等待在 64 MiB 的就绪队列里。发送窗口只需要装下前者；后者是普通的背压。
- **客户机 CPU 是瓶颈。** 最新 QEMU 运行中 `kworker` 约占一个 vCPU 的 99%。
  上面的绝对 Mbit/s 是这个客户机给出的下限，不是线路的属性。
- **接收环的物理内存约为负载的两倍。** `TBT_RX_RING_MAX` 是每条流 64 MiB，
  但每个入队帧都是 `kmalloc(struct_size(frame, data, len))`；帧头会让每条记录
  跨到下一档 slab：4 KiB 记录占 8 KiB 对象，满 8 KiB 记录占 16 KiB 对象。
  严格的先发后读传输还会把回显留在公共套接字的接收队列里；客户机已用内存的峰值
  约为所传 40 MiB 的两倍（内存行中 75 MB → 163 MB）。请按此规划客户机内存。
- **DKMS 安装已在干净客户机内核上验证，宿主本机未直接执行。** 本机无法运行：
  `sudo` 需要密码，`dkms` 和匹配的内核头文件都没有安装。在 QEMU 客户机里，
  用干净的 `7.2.9-1-MANJARO` 内核、真实的 `dkms 3.4.4` 和 `linux72` 头文件，
  `scripts/install_dkms.sh` 完成暂存、构建并安装 `tbtun/0.0.1`，`modprobe`
  发布 `/proc/net/tbtun`，隧道行写入后按规范读回，`rmmod` 卸载，再次
  `modprobe`/`rmmod` 通过，`--uninstall` 删除全部条目与文件：38 步、
  39 项检查、0 失败（`review/dkms-qemu/`）。不要把这读成宿主安装成功。
- **2026-11-07 之前的构建会破坏内核内存。** 每条载体的 `parser` 与 `http_rx`
  当时用 `kmalloc()` 分配，复用 slab 对象时 `parser->have` 可能是脏值，
  `tbt_http_rx_parse()` 又把它当作 `memmove()` 长度；现场主机在加载后几秒内
  出了六次 Oops。修复后的构建会把这两个对象清零（`kzalloc()`，复用路径再加一次
  显式 `memset()`），`tbt_rx_parse_pending()` 在 `have` 超过缓冲区时直接拒绝该载体，
  作为第二道防线。线上没有任何字段能区分新旧构建；两端都查 `srcversion`。

## 运维

修改速率、重复度或任何其它键都不需要重新加载模块：把完整的 `tunnel` 行再写一次即可，
隧道会被替换，现有流会被重置。由于是整条替换，每次都要写全所有必填键。
`flush` 会关闭所有隧道；只想改一条隧道时，重发它的完整行即可。

规则不会跨重启保留。保存一次，开机时恢复：

```sh
cat /proc/net/tbtun/config > /etc/tbtun.conf
# 开机后、客户端连入之前：
cat /etc/tbtun.conf > /proc/net/tbtun/config
```

一个 systemd unit，`ExecStart=/bin/sh -c 'cat /etc/tbtun.conf > /proc/net/tbtun/config'`，
`After=systemd-modules-load.service`，就够了；`rc.local` 里写一行也可以。
模块本身要先加载，可以用 `/etc/modules-load.d/tbtun.conf` 或 `modprobe tbtun`。

## 排障

- `tunnels` 里看 `state` 和 `streams`。近端一直停在 `idle`，说明监听端口没绑上，
  查 `dmesg` 和端口占用。
- 卡住的载体在 `peers` 里最明显：`state` 不是 `up`，`rx_lag_ppm` 持续上升，
  `rtt_us` 远高于 `minrtt_us`，或者 `inflight` 居高不下而 `tx_bytes` 不动。
- `stats` 里的 `gaps_total` 不为零，说明重排溢出重置了流；
  `holes_unrecoverable` 统计重放环补不上的空洞。
- `port_exhaust` 上升，说明全局或本隧道的源端口段对当前载体数太小；用 `ports`
  加宽它，或加宽该隧道的 `portblock=`。`port_alloc_fail` 说明端口位图分配失败
  （`ENOMEM`）；两种情况下载体都会重试。
- `win_block` 上升而 `wire` 停住，说明限制来自对端通告窗口，而不是丢包；
  窗口打开后 `win_resume` 会跟着上升。
- `join_src_reject` 上升，说明有锚之外的近端源地址在尝试加入；
  常见原因是近端多宿主或 NAT 重绑源地址。
- `debug_loss=1` 配合 `carrier <i> drop_ppm 10000`，会在载体的写边界按比例丢弃整条记录，
  从而在没有丢包线路的情况下演练丢包处理。`peers` 里的 `dropped` 列会计数。
- `__memcpy` 里的 `Internal error: Oops`，且栈上是
  `tbt_http_rx_parse → tbt_rx_parse_pending → tbt_car_rx_work`，是解析器未初始化
  的那个 bug，不是网络故障。两端都用 `cat /sys/module/tbtun/srcversion` 核对
  你要加载的那个产物。2026-11-06 的证据与取证流程在
  `review/wan-experiment/arm64/stop-65-down-20261106T2250Z/forensics-65/`。
- 卸载模块会关闭所有监听端口和载体；已连接的客户端会看到 EOF，而不是挂住。

## QoS 说明

`http-kal` 档位是一个赌注：假设运营商把 HTTP/1.1 当作高优先级流量。这是假设，不是事实，
模块也无法让分类器优待任何东西。这个赌注验证起来很便宜，而且验证本身就是使用这套软件的
一部分：[DESIGN.md §17](DESIGN.md) 给出了十分钟的测量步骤和四种配置——纯 TCP、一条
`http-kal` 载体、四条载体、四条 `raw` 载体——并说明每种结果意味着什么。

如果测量显示 HTTP 档位没有带来任何提升，就设 `profile=raw`：按五元组分散的收益与它无关。
如果 `N + M` 也没有带来提升，说明线路上存在针对整个用户的聚合限速，
这类隧道帮不上忙；此时诚实的做法是用单条流的 WireGuard 链路。

## 安全

默认不加密，也不认证。`crypto=on`（`fec=on` 时的默认值）对 FEC 的
DATA/PARITY 负载做 ChaCha8 混淆：它把应用字节从朴素的 DPI 特征里藏起来，
但不是机密性——未配置 `crypto_key=` 时会话密钥在 HELLO v6 body 里明文传输
（`key_id=1`），而且没有消息认证。两端都配置 `crypto_key=` 时 `key_id=0`，
密钥不匹配表现为 payload CRC 失败，连接不会静默降级。FEC 头、HTTP 头、
chunk 分帧和控制帧始终是明文。配置仅限 root（`0600`），加载模块同样需要
root。如果两台主机之间的路径不可信，就把隧道放进 WireGuard 里跑：载体走在
加密链路之上，此时只应选择 `raw` 档位，因为 HTTP 解析器看不到 TLS 内部。

## 适用范围

这不是通用 VPN。它只转发 TCP，每条隧道只对应一个字面目标地址，没有路由、没有 NAT、
没有认证。FEC v6 可选（`fec=on`）；`mode=sc-ldpc` 已实现
（binary 下行、grouped GF(16) 上行），`mode=rlnc`/`grouped` 保留为 GF(2)
band RLNC 回退，`carriers = groups * (n + m)` 最多 256 条载体。0.7.0 每个
会话只承载一条 FEC 流（`0x100a`）。
`crypto=on` 只做 ChaCha8 body
混淆，不是加密，也不做认证。它的用途是放大"每流限速"的额度，并验证某个
QoS 档位是否有效。其它场景请用 WireGuard。

---

`logo.png` 是旧的 TCP Brutal 标志，仅为保留仓库历史而留下，本手册不使用它。
