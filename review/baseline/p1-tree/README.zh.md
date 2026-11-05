# tbtun

一个 Linux 内核 TCP 隧道：把一条 TCP 流分散到 `N + M` 个独立的五元组上，
让运营商"每流限速"的额度按 `N + M` 份生效；设计上以预测而非重传来跨过载体停顿。

## 工作原理

近端在某个端口上监听。客户端连上来之后，内核接受这条连接，不会把它交给任何用户态进程。
负载被切成帧，通过 `N` 条活跃载体连接和 `M` 条备用连接送到远端；每条载体有自己的源端口，
因此各自占用一个独立的限速桶。远端把字节流重新拼起来，连到 `target=`，回程沿原路返回。
接受连接、发起连接、封帧、排序、投递，全部在内核里完成。没有字符设备，没有代理进程，
也没有辅助程序：配置模块的唯一方式是向 `/proc/net/tbtun/config` 写文本。

收益有两个独立来源。按五元组计费是确定的那一个：`N + M` 条流，就有 `N + M` 份额度。
第二个是赌注：`http-kal` 档位把载体包装成普通的 HTTP/1.1 keep-alive 流量，
假设运营商的分类器会给 HTTP 更高优先级。这个假设要被测量，而不是被相信，
见 [QoS 说明](#qos-说明)。如果测量结果相反，`profile=raw` 会关掉这层包装，
按流分散带来的收益仍然存在。

当前代码实现了 [DESIGN.md](DESIGN.md) 的 C1–C8：`/proc` 控制面、监听器与远端、
备用载体与带 RETIRE 握手的载体替换、带重放环的重复发送、带 PREDICT/CANCEL 的空洞预测，
以及 `http-kal`/`http-kal-drip` 容器。载体字节流里的空洞现在由重放环补齐，而不是终止整条流；
补不上的空洞由 `stats` 里的 `holes_unrecoverable` 统计，只有这个计数增加时才会重置流。

## 环境要求

- Linux 6.1 或更新版本，以及用于编译的对应内核头文件。
- 如果内核开启了 `CONFIG_DEBUG_INFO_BTF_MODULES=y`，需要 `pahole`（dwarves 包）；
  没有它，模块的 BTF 步骤会失败。
- 加载模块和写 `/proc/net/tbtun/*` 需要 root（所有文件权限为 `0600`）。

## 编译

```sh
make check
```

`check` 会编译 `tbtun.ko`，然后运行宿主机的测试——语法、模糊测试和 HTTP 容器；
这些测试不需要 root、不需要加载模块，也不需要内核头文件。只跑这些测试用 `make host-test`。

如果系统没有安装内核头文件，可以改用解包出来的头文件负载，不必使用
`/lib/modules/$(uname -r)/build`；一次性准备步骤、生成的 `env.sh` 以及 pahole 的注意事项见
[docs/build-env.md](docs/build-env.md)。该环境存在时，`make check` 会自动加载它。

## 打包

模块版本只有一个权威：`tbtun/tbt_uapi.h` 中的
`TBT_VERSION_MAJOR/MINOR/PATCH`。`scripts/mkdkmsconf.sh` 派生
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
| `max_carriers` | `16` | 每个会话的载体数，`1..16`；`n + m` 超过上限返回 `EINVAL` |
| `debug_loss` | `0` | 是否允许写 `drop_ppm`（确定性丢包注入） |

控制面只在 `init_net` 中创建；没有 per-netns 视图。
卸载用 `sudo rmmod tbtun`。卸载会清空所有隧道，关闭所有监听端口和载体。

## 双机示例

近端主机 `10.0.0.1`，远端主机 `203.0.113.9`，远端有一个服务监听在 `127.0.0.1:5201`。

在近端执行：

```sh
echo 'tunnel 1 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443 n=4 m=1' \
     > /proc/net/tbtun/config
```

在远端执行：

```sh
echo 'tunnel 1 role=far listen=203.0.113.9:443 target=127.0.0.1:5201 n=4 m=1' \
     > /proc/net/tbtun/config
```

然后在任意一端查看：

```sh
cat /proc/net/tbtun/tunnels
```

此时近端在 `10.0.0.1:9000` 上监听。连到这个端口的客户端，会经四条载体被送到远端的
`127.0.0.1:5201`。可以用 `iperf3 -c 10.0.0.1 -p 9000 -t 60 -P 1` 对远端的
`iperf3 -s` 做测试。

两端必须在隧道 id、`n`、`m` 以及 profile 家族上保持一致。远端的 `listen` 是载体到达的地址，
近端的 `peer` 是载体发往的地址。地址只接受字面 IPv4：v0 不支持 DNS，也不支持 IPv6。

## 能观测到什么

模块知道的全部内容都在 `/proc/net/tbtun` 下。所有文件都是 `0600 root`；
没有辅助程序，`echo` 和 `cat` 就是全部接口。

| 文件 | 读它能看到什么 |
| --- | --- |
| `config` | 规范形式的配置；隧道行、`del`、`flush`、`carrier ...` 动作都写到这里 |
| `tunnels` | 每条隧道一行：角色、状态、listen/peer/target、n、m、streams |
| `peers` | 每个载体槽一行，共 16 列（见下） |
| `ports` | 全局源端口段，以及哪些端口正在使用 |
| `stats` | 第一行是 `debug_loss=<0\|1>`，然后是流与空洞总数，再往后是每条隧道的计数器（`txq`、`rxq`、空洞、预测、重复、载体替换） |
| `http` | 仅调试（`debug>=2`）：载体最近一次真正发到线上的 HTTP 请求块，用来对照解析器检查容器 |

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
| `listen` | — | `a.b.c.d:port`；近端为对外地址，远端为载体地址 |
| `peer` | — | 仅近端：载体的目的地 |
| `target` | — | 仅远端：负载投递到的字面地址 |
| `n` | `4` | 活跃载体数，`1..16` |
| `m` | `1` | 备用载体数，`0..16`；`n + m <= 16` |
| `dup` | `1` | 重复度，`0..3` |
| `keepalive` | `5000` | 空闲 PAD 间隔，毫秒 |
| `drip_ms` | `1000` | `http-kal-drip` 空闲时每字节的间隔，毫秒 |
| `replay` | `8388608` | 重放环字节数 |
| `tunnel_rate` | `0` | 字节/秒上限，`0` 表示不设 |
| `max_inflate` | `2.000` | 预测膨胀上限 |
| `hole_budget_ms` | `500` | 空洞允许存在的时间，毫秒 |
| `rx_reorder_bytes` | `262144` | 接收端重排缓冲字节数 |
| `tx_retain_bytes` | `0` | 发送端保留字节数，`0` 表示不限 |
| `carrier_stall_ms` | `3000` | 多久没有数据就判定载体失效，毫秒 |
| `profile` | `http-kal` | `http-kal`、`http-kal-drip` 或 `raw` |
| `portblock` | `0-0` | 载体的源端口段；`0-0` 表示不设 |
| `srcaddrs` | — | 逗号分隔的本地源地址；只有第一项被使用 |
| `nonce` | — | `16` 或 `32` 个 hex 字符：固定会话 nonce；近端本地使用，远端要求对端使用 |

曾被解析但从未生效的键（`framing=`、`chunk=`、`max_retry=`、`dev=`、`uid=`）
已删除；未知键会返回 `EINVAL`。

`nonce=` 固定 128 位会话 nonce。32 个 hex 字符是完整值；16 个 hex 字符固定
低 64 位，并在规范输出中左补零。`role=near` 时它是本端放进 HELLO 的 nonce；
`role=far` 时它是远端要求近端使用的 nonce，每一条 HELLO 都必须匹配。未配置
`nonce=` 的 far 会采纳第一条 HELLO 的 nonce（首连信任，TOFU），谁赢得首个
连接谁就定义了会话。nonce 在 HELLO 里明文传输：它是会话标识与反混淆 token，
不是密码；链路不可信时套 WireGuard。64 个 hex 字符（256 位）会被拒绝：
HELLO 的 nonce 字段只有 128 位。

其它指令：`del <id>`、`flush`、`carrier <index> down|up|drop_ppm <n>`，
以及 `session <id> nonce=<hex>`（固定会话 nonce，用于可复现的测试）。
`drop_ppm` 需要加载时带 `debug_loss=1`，否则会被拒绝并返回 `EPERM`。

读取 `config` 得到的是规范形式：所有标量键按固定顺序写出，默认值也会显式写出。
这段文本可以逐字节往返，所以
`cat /proc/net/tbtun/config > /etc/tbtun.conf` 与
`cat /etc/tbtun.conf > /proc/net/tbtun/config` 能精确恢复保存时的状态。

### `tunnels`（只读）

先输出 `# id role state listen peer target n m streams`，然后每条隧道一行。

### `peers`（读写）

每个载体槽一行，共 16 列：

| 列 | 含义 |
| --- | --- |
| `id` | 隧道 id |
| `carrier` | 会话内的载体序号 |
| `sport` | 本地源端口，取自端口段 |
| `peer` | 对端地址:端口 |
| `uack` | 对端已确认的最大序号 |
| `rtt_us` | 平滑后的往返时延，微秒 |
| `inflight` | 已写出但尚未确认的字节数 |
| `state` | `up`、`spare`、`stalled` 或 `down` |
| `class` | 该载体使用的容器（`raw`、`http-kal`、`http-kal-drip`） |
| `recv_cnt` | 该载体收到的记录数 |
| `rx_lag_ppm` | **接收端滞后**，单位百万分之一：接收端的投递位置落后于该载体最新记录的程度。它不是丢包率 |
| `minrtt_us` | 观测到的最小往返时延，微秒 |
| `tx_frames` | 该载体写出的记录数 |
| `tx_bytes` | 该载体写出的字节数，含头部 |
| `dropped` | 被 `drop_ppm` 注入器丢弃的记录数（仅调试） |
| `retx_ppm` | 每百万条写出记录中的重传数，这才是丢包的代理指标 |

写入 `carrier <index> down|up|drop_ppm <n>` 会对运行中的会话生效。

### `stats`（只读）

第一行固定是 `debug_loss=<0|1>`，即 `drop_ppm` 的开关状态。之后是
`streams_total`、`gaps_total`，以及每条隧道的计数器（队列深度、暂停标志、
对外套接字的发送状态、载体读写游标）。

### `ports`（读写）

`ports <lo>-<hi>` 设置载体的全局源端口段。空行或 `ports 0-0` 是 no-op，
不会改变当前端口段。

## 验证过的性能

数字来自 [benches/](benches/)：在 KVM 客户机里通过同样的操作接口驱动，
virtio-net，每个源端口一条 10 Mbit 的 htb 类并挂 `delay 20ms 10ms` 的 netem，
用 `tbt_burst` 的严格先发后读阶段传 40 MiB（逐字节校验），每次启动只测一种配置。
对照组是单个 `dd | nc` 流，同样走在一条被整形的五元组上。

| 载体数 | 对照 (Mbit/s) | 应用 (Mbit/s) | 应用/对照 | wire/app | retx_ppm | strict / rc | CSV |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `n=1` | 2.48 | 3.46 | 1.40x | 1.0316 | 1,090 | `strict=1`, `rc=0` | `benches/results/20261105T112725-throughput.csv` |
| `n=4` | 2.24 | 11.39 | 5.08x | 1.0005 | 96,095 | `strict=1`, `rc=0` | `benches/results/20261105T112245-throughput.csv` |
| `n=8` | — | — | — | — | — | 待最终回归 | — |
| `n=16` | — | — | — | — | — | 待最终回归 | — |

当前目标是 `wire/app < 1.2`、`retx_ppm < 100k`。最新 `n=4` 两项都在目标内
（1.0005 与 96,095）；`n=1` 是 1.0316 与 1,090。`n=8`/`n=16` 在最终回归前
不给出倍数承诺。

比值仍是主张：`N` 个五元组胜过一条流。这些是 QEMU/slirp 实验室数字，
每个源端口一个 10 Mbit htb 类，netem `delay 20ms 10ms`；不是 ISP 实测。
绝对值取决于这台机器的 QEMU/slirp 路径，而不是 10 Mbit 的桶。

下表的代价数据来自更早的 bench；本次 `n=1`/`n=4` 吞吐运行没有重新测量它。

| 代价 | 实测 |
| --- | --- |
| `http-kal` 容器相对 `raw` | 字节数约多 1%（每约 4 KB 记录一个 chunk），单 vCPU 的系统态 CPU 约多 3.5 个百分点 |
| 预测与每路径丢包 | 拐点在第一个 1%：有效吞吐 9.85 → 6.91 Mbit/s，之后到 10% 基本持平在 5.40；1%、5%、10% 三档的 `holes_unrecoverable` 都是 0 |
| 时延（1 KB 回显） | 相对直连路径 p50 增加 156 µs、p99 增加 274 µs；补一个空洞会把 p99 推到 22–31 ms |

这些是单台机器上的实验室数字，说明不了运营商是否真的给 HTTP 更高优先级；
后者要用 [DESIGN.md §17](DESIGN.md) 在真实线路上测。

## 已知限制

- **整形链路上的重传。** 最新 `n=4` 为 `retx_ppm` 96,095、`wire/app` 1.0005；
  `n=1` 为 1,090 与 1.0316。当前目标是 `retx_ppm < 100k`、`wire/app < 1.2`；
  `n=8`/`n=16` 仍需最终回归。
- **载体数与吞吐。** 探针 I 的 control `n=1` 通过（`rc=0`、`strict=1`、
  `interleaved=1`，4 MiB 逐字节），`n=4` 的 strict 与 interleaved 也通过。
  最新应用吞吐为 `n=1` 3.46 Mbit/s、`n=4` 11.39 Mbit/s；`n=1` 受单流 TCP 限制，
  `n=8`/`n=16` 待最终回归。
- **传输之后 `flush` 可能阻塞。** 完成一次 40 MiB 传输后，
  `echo flush > config` 曾被观察到不返回。本次数据面运行没有复测；按“待验证”处理。
  测量之间请重发完整的隧道行，不要用 flush。
- **已删除的无效键**：`framing=`、`chunk=`、`max_retry=`、`dev=`、`uid=`
  曾被解析但从未生效，现在不再接受；HTTP 容器被中间盒破坏时用 `profile=raw`。
- **不加密。** 隧道在线上是明文，见 [安全](#安全)。

## 运维

修改速率、重复度或任何其它键都不需要重新加载模块：把完整的 `tunnel` 行再写一次即可，
隧道会原地替换。由于是整条替换，每次都要写全所有必填键。

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
- 卡住的载体在 `peers` 里最明显：`state` 不是 `up`，`loss_ppm` 持续上升，
  `rtt_us` 远高于 `minrtt_us`，或者 `inflight` 居高不下而 `tx_bytes` 不动。
- `stats` 里的 `gaps_total` 不为零，说明某条载体乱序到达，该流被终止而不是缓存。
  在没有重放环的当前阶段，这是设计内的行为。
- `debug_loss=1` 配合 `carrier <i> drop_ppm 10000`，会在载体的写边界按比例丢弃整条记录，
  从而在没有丢包线路的情况下演练丢包处理。`peers` 里的 `dropped_by_ppm` 会计数。
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

这条隧道不加密。负载字节按应用发出的原样经过网络，装在任何人于路径上都能读取的 TCP 连接里。
配置仅限 root（`0600`），加载模块同样需要 root。如果两台主机之间的路径不可信，
就把隧道放进 WireGuard 里跑：载体走在加密链路之上，此时只应选择 `raw` 档位，
因为 HTTP 解析器看不到 TLS 内部。

## 适用范围

这不是通用 VPN。它只转发 TCP，每条隧道只对应一个字面目标地址，没有路由、没有 NAT、
没有认证、也没有加密。它的用途是放大"每流限速"的额度，并验证某个 QoS 档位是否有效。
其它场景请用 WireGuard。

---

`logo.png` 是旧的 TCP Brutal 标志，仅为保留仓库历史而留下，本手册不使用它。
