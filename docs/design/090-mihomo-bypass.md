# 090 — 用原版 mihomo 做 tbtun 的 VLESS 旁路（v0.9.0 设计）

状态：设计稿，未改任何代码，未提交。
日期：2026-11-11。
适用树：`w-080-credit-int`，HEAD `5b730dc`（`TBT_VERSION` 0.8.0）。
上游基线：`MetaCubeX/mihomo` Meta 分支 `0455d0d2`（2026-11-11），
`constant.Version = "1.10.0"`（`constant/version.go:3-8`）。

这份文档只回答一件事：**在不修改 mihomo、不 fork、不打补丁、不导入它的源码的前提下，
怎样把原版 mihomo 的 VLESS+TCP+mux 流量接进 tbtun，并诚实地说明它今天能做什么、
不能做什么。**

文中的源码行号都指向上面两个固定版本。升级 mihomo 后必须按 §3 的方法重新核对；
行号会漂移，结论不一定漂移。

---

## 0. 结论（先看这一段）

1. **零改动基线今天就能跑，不依赖任何插件、扩展或重新编译 mihomo。**
   far 侧内核 tbtun 把载体流交给
   `target=127.0.0.1:19100`，该端口由原版 mihomo 的 VLESS 入站监听；near 侧
   mihomo 的 VLESS 出站指向 `127.0.0.1:19080`，由内核 tbtun 的 near 监听。
   mihomo 两端都只看见本机回环上的一个普通 VLESS 服务端。整条路径不改一行
   mihomo 源码。

2. **mihomo 的 sing-mux 把多条应用 TCP 连接复用到一条底层 TCP 上。** 这条
   底层 TCP 就是 tbtun 搬运的唯一一条字节流。当前 tbtun（0.8.0）的 FEC 数据面
   **只允许一条并发 FEC 流**（`n_fec_flows != 0` 时直接 `-ENOSPC`，
   `src/kernel/tbt_fec_data.c:8813-8818`、`:8764-8769`）。mux 与这个限制正好互补：
   不需要给 tbtun 增加任何子流复用。

3. **mux 与 XUDP 在当前 tbtun 上不能同时生效。** 这不是配置写法问题，是路径
   问题：
   - `smux.only-tcp: false`（默认）时，UDP 由 sing-mux 自己封装成 mux 流
     （`adapter/outbound/singmux.go:49-62` → `sing-mux/client.go:112-119`），
     **根本不会调用 VLESS 的 XUDP 分支**（`adapter/outbound/vless.go:369-404`）。
   - `smux.only-tcp: true` 时，UDP 才走到 VLESS 的 XUDP（`CommandMux`）。
     但每条 UDP `ListenPacket` 都会新建一条底层 TCP
     （`adapter/outbound/vless.go:369-404` 每次 `dialContext`）；这条 TCP 到达
     tbtun near 时是**第二条并发流**，被 `fec_stream_refused` 拒绝并 reset。

   所以 v0.9.0 的基线必须在两者中选一个，并把另一个写成实验项，不能宣称
   “mux + XUDP 都开着”而实际只跑通了一条。§4.5 给出判定表和选择。

4. **隧道内预期只有一条长连接。** 这是设计目标，不是巧合：mihomo 的
   `smux.max-connections` 必须显式设为 `1`。默认 0/0 时，现有 session 只要
   已有一条流，下一次 `openStream` 就会 `offerNew`（`sing-mux/client.go:180-189`），
   第二条逻辑连接就可能新建底层 TCP。`only-tcp` 决定 UDP 是否也挤进这条
   连接。断线重连的时序与风险见 §4.3。

5. **VLESS 不加密 = 端到端没有机密性。** tbtun 的 `crypto=on` 只是 ChaCha8
   异或混淆，密钥在 HELLO 里明文传输（§8.1），不构成对被动或主动攻击者的
   安全边界。要机密性只能在 mihomo 的 VLESS 上开 TLS/REALITY（纯配置），
   但那会改变握手外观与 CPU 成本。

---

## 1. 目标与非目标

### 1.1 目标

- 用**原版 mihomo 二进制**（官方源码直接构建，零补丁）作为 VLESS 客户端与
  服务端，接进 tbtun 的两端。
- near 侧 mihomo 配置：VLESS 出站，`network: tcp`、`tls: false`、开启
  sing-mux、`udp: true`。
- far 侧 mihomo 配置：VLESS 入站，只监听 `127.0.0.1`，`allow-insecure: true`。
- 隧道内只承载**一条** mihomo 底层 TCP；浏览器/应用的多条连接由 mihomo 的
  mux 合并。
- 外层流量保持 tbtun 现有的 HTTP/1.1 chunked keep-alive 外观
  （`src/kernel/tbt_http.h:39-48`）。
- 交付可复现的装配目标（Makefile / Dockerfile / compose），只组装，不改上游。

### 1.2 非目标

- 不修改 mihomo 源码，不 fork，不打补丁，不 vendor 改源码。
- 不导入或链接 mihomo 的源码，不做进程内扩展；所有定制只发生在配置层与
  旁路进程。
- 不给 tbtun 增加新的应用层 mux。mihomo 已经有 mux。
- 不改 near/far 生产环境，不碰生产 mihomo 的 `127.0.0.1:7890`。
- 不做多用户、多租户、按域名分流到不同 far 目标。far 只有一个
  `target=127.0.0.1:19100`。

---

## 2. 零改动基线（今天可跑）

### 2.1 拓扑与端口

```
                       near host                                   far host
 ┌──────────────────────────────────────────┐   WAN   ┌──────────────────────────────────────┐
 │ browser / app                            │         │                                      │
 │   │ SOCKS/HTTP 127.0.0.1:7899            │         │                                      │
 │   ▼                                      │         │                                      │
 │ mihomo (原版, 独立实例)                   │         │                                      │
 │   VLESS 出站 tcp, no tls, smux=on        │         │                                      │
 │   │ 127.0.0.1:19080                      │         │                                      │
 │   ▼                                      │         │                                      │
 │ 内核 tbtun near                          │         │                                      │
 │   listen=127.0.0.1:19080                 │         │                                      │
 │   peer=<far>:19000                       │         │                                      │
 │   256 条 carrier TCP ────────────────────┼────────►│ 内核 tbtun far                        │
 │                                          │         │   listen=0.0.0.0:19000                │
 │                                          │         │   target=127.0.0.1:19100             │
 │                                          │         │        │                             │
 │                                          │         │        ▼                             │
 │                                          │         │ mihomo (原版) VLESS 入站              │
 │                                          │         │   listen=127.0.0.1:19100              │
 │                                          │         │   allow-insecure: true                │
 │                                          │         │        │ rules: MATCH,DIRECT         │
 │                                          │         │        ▼                             │
 │                                          │         │ 任意网站 / 目标                      │
 └──────────────────────────────────────────┘         └──────────────────────────────────────┘
```

端口表：

| 位置 | 进程 | 地址 | 用途 |
|---|---|---|---|
| near | mihomo | `127.0.0.1:7899` | mixed SOCKS/HTTP 入口；**不是**生产 7890 |
| near | 内核 tbtun near | `127.0.0.1:19080` | 接受 mihomo 的 VLESS TCP；app listener |
| near | 内核 tbtun near | 临时源端口 → `far:19000` | 256 条 carrier，near 主动拨号 |
| far | 内核 tbtun far | `0.0.0.0:19000` | carrier listener |
| far | mihomo | `127.0.0.1:19100` | VLESS 入站；只绑回环 |
| far | mihomo | 按 rules 直连 | 访问真实网站 |

谁发起 carrier：**near 发起**。far 只监听 `19000`，不主动连 near
（`src/kernel/tbt_parse.c:890-895` 要求 near 有 `peer=`、far 有 `target=`）。

### 2.2 far：内核 tbtun 配置

```sh
echo 'tunnel 1 role=far listen=0.0.0.0:19000 target=127.0.0.1:19100 \
n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on adaptive=off \
profile=http-kal keepalive=5000 nonce=<32-hex>' \
  > /proc/net/tbtun/config
```

要点：

- `n=128 m=128` 是下行 1x256 分片（`groups = carriers / (n+m) = 1`，
  `n+m=256`）。far 的 TX 形状由 `n/m/carriers` 决定；`tbt_parse.c:946-956`
  解析默认值并检查 `carriers` 是 `n+m` 的整数倍。far 侧没有“必须是
  downlink”的独立开关，形状就是这组数算出来的。
- `carriers=256` 且必须是 `n+m` 的整数倍（`tbt_parse.c:955-956`）。
- `crypto=on` 必须配 `fec=on`（`tbt_parse.c` 的交叉校验）；它只做混淆，见 §8.1。
- `target=127.0.0.1:19100` 是 far 的默认目标端口。OPEN 里如果带非 0 端口，
  会覆盖它；near 内核默认发 0，所以 far 用这里的 19100。
- `nonce=` 是可选的固定会话 nonce（32 hex）。far 设了，near 就必须用同一个；
  否则 far 会拒绝 HELLO。冒烟里建议固定，便于复现。

### 2.3 near：内核 tbtun 配置

```sh
echo 'tunnel 1 role=near listen=127.0.0.1:19080 peer=<far-ip>:19000 \
n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on adaptive=off \
profile=http-kal keepalive=5000 nonce=<32-hex>' \
  > /proc/net/tbtun/config
```

要点：

- near 的 `listen=` 是**应用监听**：mihomo 的 VLESS 出站连到这里。
- near 的 `target=` 可以省略；内核 near 发给 far 的 OPEN 目标端口默认是 0
  （`src/kernel/tbt_tx.c:370-374`），far 用自身的 `target=`。这与用户态客户端
  不同（见 §2.4）。
- near 的 `n/m` **决定它自己的 TX 形状**。`n=8 m=24` + `carriers=256` 得到
  `groups=8`，走 GF(16) 上行（`tbt_fec.c:2496-2534`：`groups == 1` 才是
  binary downlink，`groups > 1` 才是 uplink）。如果这里写 `128/128`，
  near 的 TX 会变成 1x256 downlink，far 的 RX 也会按这个 shape 解——能跑，
  但不是 8x32 上行，FEC 行为与设计不符。**两端的方向不同，配置也不同：
  near 写 8/24，far 写 128/128。**
- `w` 不只是 adaptive 的参数：`tbt_parse.c:930-937` 在 `mode=3` 时把
  `w=0` 默认成 **256**，而 `tbt_fec_sclc_shape_of` 的 UP 分支只在
  `block=0` 时才用 `TBT_FEC_SCLC_BLOCK_UP=64`（`tbt_fec.c:2527-2534`）。
  所以 near 必须显式 `w=64`，否则 8/24 会拿到 block=256 的 GF(16) 形状，
  与上行 codec 不符。far 的 downlink 默认 256 正确，显式写出来更清楚。
  `adaptive=on` 还要求同一组 `w/L`（`tbt_parse.c:968-1008`）；本文基线用
  `adaptive=off`，但 `w` 仍然必须写对。
- 256 条 carrier 全部由 near 拨出；`portblock` 不设时用临时源端口。

### 2.4 near 的第二种实现：用户态 `tbt_client`

如果 near 不想加载内核模块，可以用仓库里的用户态客户端。它和内核 far 使用
同一套 0.8 wire，但有三处配置差异，必须写清楚：

```sh
src/client/tbt_client role=near listen=127.0.0.1:19080 peer=<far-ip>:19000 \
  target=127.0.0.1:19100 n=8 m=24 mode=sc-ldpc carriers=256 \
  fec=on crypto=on adaptive=off verbose=1
```

差异：

1. `carriers` 必须是 **256**，否则启动直接报错
   （`src/client/tbt_client_engine.c:3212-3216`）。
2. 只实现 `mode=sc-ldpc`（`:3190-3193`）。
3. `target=` 的端口会被塞进 OPEN，far 用它覆盖自己的 `target=` 端口
   （`src/client/tbt_client_engine.c:1202-1232`；far 端 `:2385-2414`）。
   near 的 `target=` 地址本身不用于拨号，但 `tbt_parse_line` 需要它非零才能
   产出端口。**所以用户态 near 必须写 `target=127.0.0.1:19100`**，否则 OPEN
   带 0，far 会去连端口 0。
4. 用户态客户端的 TX 形状是固定的：near 发 8x24 上行，far 发 128x128
   下行（`src/client/tbt_client_engine.c:3219-3257`）。所以 `n/m` 在 near 上只
   影响 `down_shape` 的初始值；写 8/24 或 128/128 都能启动，但写 8/24 与
   内核 near 的配置一致，便于对照。

用户态 near 的并发上限是 `TBT_FLOW_MAX = 8`
（`src/client/tbt_client_engine.c:52-53`），第 9 条应用连接在 accept 后被立即
`close()`（`:2991-2996`）。内核 near 的上限是**一条 FEC 流**（§4.1）。
两者都不要和“256 条 carrier”混淆：carrier 是传输子流，不是应用流。

### 2.5 mihomo 客户端（near）配置

文件：`/etc/mihomo-near/config.yaml`。只列与本文有关的键；其余用默认值。

```yaml
mixed-port: 7899
allow-lan: false
mode: rule
log-level: info

proxies:
  - name: tbtun-vless
    type: vless
    server: 127.0.0.1
    port: 19080
    uuid: 11111111-1111-1111-1111-111111111111
    network: tcp
    tls: false
    udp: true
    packet-encoding: xudp   # 仅声明；only-tcp:false 时不会走 XUDP（§4.5）
    smux:
      enabled: true
      protocol: h2mux
      max-connections: 1
      padding: false
      statistic: false
      only-tcp: false        # 见 §4.5：当前 tbtun 单流下必须为 false
    # flow 不设置；smux 与 xtls-rprx-vision 不共存
    # servername / client-fingerprint / alpn 不设置：tls=false 时无意义

proxy-groups:
  - name: PROXY
    type: select
    proxies: [tbtun-vless]

rules:
  - MATCH,PROXY
```

字段来源与“为什么是这个名字”：

| 键 | 源码证据 | 说明 |
|---|---|---|
| `type: vless` | `adapter/parser.go:65-71` | 解析进 `outbound.NewVless` |
| `network: tcp` | `adapter/outbound/vless.go:155-262`，`default:` 在 `:258-261` | 非 ws/http/h2/grpc/xhttp 一律走 TCP |
| `tls: false` | `adapter/outbound/vless.go:65-72`（字段）、`:340-365`（不套 TLS） | 明文 VLESS；这里的“明文”只在内层 |
| `udp: true` | `adapter/outbound/vless.go:67`，`adapter/outbound/base.go:SupportUDP` | 不开则 UDP 流量根本不会交给这个出站 |
| `packet-encoding: xudp` | `adapter/outbound/vless.go:70`、`:474-485` | 字符串 `xudp` 走 `default:`，把 `XUDP` 置 true |
| `xudp: true`（另一种写法） | `adapter/outbound/vless.go:69` | 源码里确实有这个 bool 字段；当前 wiki 未列。两种写法等价，选一种 |
| `smux.enabled` | `adapter/parser.go:234-246` | 只有 `enabled: true` 才会包 `NewSingMux` |
| `smux.protocol` | `adapter/outbound/singmux.go:23-33`、`sing-mux/client.go:71-73` | 空/`h2mux`/`smux`/`yamux`；默认 `h2mux` |
| `smux.max-connections` | `adapter/outbound/singmux.go:26`、`sing-mux/client.go:150-190` | 必须显式 `1` |
| `smux.only-tcp` | `adapter/outbound/singmux.go:31`、`:49-62` | 决定 UDP 是否绕开 mux（§4.5） |
| `smux.padding` | `adapter/outbound/singmux.go:29`、`sing-mux/client.go:199-212` | mux 协议内 padding；与 tbtun 的 HTTP PAD 无关 |

不存在或不该写的字段（避免“凭记忆”写错）：

- 没有 `mux: true` 这种顶层键。mux 只在代理项的 `smux:` 下。
- 没有 `xudp: "true"` 字符串；`xudp` 是 bool，`packet-encoding` 是字符串。
- `smux.max-connections` 与 `max-streams` / `min-streams` 互斥
  （wiki 明确写了；源码里三者都读，见 `singmux.go:23-33`）。只设一个。
- VLESS 的 `flow: xtls-rprx-vision` 不要设。它走的是另一条 TLS/REALITY
  路径，与本文的明文 TCP 无关；设了还需要 TLS，配置会直接失败或行为改变。
- `skip-cert-verify`、`servername`、`alpn` 在 `tls: false` 下不会生效，
  写了只会让配置看起来像 TLS。

### 2.6 mihomo 服务端（far）配置

文件：`/etc/mihomo-far/config.yaml`。

```yaml
mixed-port: 0
allow-lan: false
mode: rule
log-level: info

listeners:
  - name: vless-in
    type: vless
    listen: 127.0.0.1
    port: 19100
    allow-insecure: true
    users:
      - username: u1
        uuid: 11111111-1111-1111-1111-111111111111
    # mux-option 只控制 padding/brutal；服务端总是能接 sing-mux
    mux-option:
      padding: false

rules:
  - MATCH,DIRECT
```

字段来源：

| 键 | 源码证据 | 说明 |
|---|---|---|
| `listeners[].type: vless` | `listener/parse.go:96-102` | 分派到 `IN.NewVless` |
| `listeners[].listen` | `listener/inbound/base.go:92-98` | `BaseOption.Listen` |
| `listeners[].port` | 同上 | 字符串区间，单端口可写整数 |
| `allow-insecure: true` | `listener/inbound/vless.go:24`、`listener/sing_vless/server.go:275-277` | **没有证书时必须为 true**，否则启动报错 |
| `users[].uuid` | `listener/inbound/vless.go:32-36` | 与客户端 `uuid` 一致 |
| `mux-option` | `listener/config/vless.go:34`、`listener/sing/sing.go:75-91` | 服务端 mux 不是开关；这里只调 padding/brutal |
| `rules: MATCH,DIRECT` | mihomo 规则引擎 | far 侧按目标直连；冒烟里目标是 far 主机上的 HTTP 服务 |

关键事实：**mihomo 的 VLESS 入站没有“开启 mux”这个开关。**
`sing.NewListenerHandler` 总是创建 `mux.NewService`
（`listener/sing/sing.go:75-91`），mux 会话通过目标地址是 sing-mux 的 magic
FQDN 来识别（`listener/sing/sing.go:94-109`）。`mux-option` 只把
`padding`/`brutal` 传给这个 service。所以服务端配置里“少写 mux”不会关闭
mux；“写 mux”也不会多开一个端口。

### 2.7 零改动基线能验证什么

1. **字节无损**：mihomo VLESS+mux 产生的单条 TCP 能穿过 tbtun 的 256 条
   carrier 与 FEC，下载大文件后 SHA-256 一致（S2）。
2. **mux 合并有效**：浏览器/N 条并发连接只产生 **1** 条到 `19080` 的 TCP，
   因而只占 **1** 条 FEC 流（S3）。这正好绕过 tbtun 当前的单流限制。
3. **UDP 可达**：`only-tcp: false` 时 UDP 走 mux 流，能收发数据报（S4）。
   注意这不是 XUDP（§4.5）。
4. **外观**：WAN 上只看到 HTTP/1.1 chunked POST `/api/v2/upload`、
   `Content-Type: application/gzip`、Chrome UA、1079 字节 chunk（S7）。
5. **零补丁**：以上全部只用原版 mihomo 二进制 + 配置文件 + tbtun。

### 2.8 零改动基线不能解决什么

1. **端到端机密性**。VLESS 不加密；tbtun 的 `crypto=on` 是混淆，不是 AEAD
   （§8.1）。任何能终结 tbtun 的一方都能读到明文 HTTP。
2. **mux 与 XUDP 同时生效**。当前 tbtun 只允许一条 FEC 流，而 XUDP 的 UDP
   路径会另开一条 TCP（§4.5）。要么 UDP 走 mux（非 XUDP），要么 XUDP 但 UDP
   被拒。
3. **重连期间的第二条流**。旧流未 detach 时，新 TCP 会被
   `fec_stream_refused` 拒绝；半开链路的窗口由 mihomo 的 30 s h2mux idle
   timeout 兜底（§4.3）。
4. **多目标/多用户**。far 只有一个 `target=127.0.0.1:19100`；多目标要靠
   mihomo 入站自己的 rules 或多 listener，不是 tbtun 的能力。
5. **进程内扩展**。不改 mihomo 就不能在它里面加自定义传输/出站；所有定制
   都停在配置层与旁路进程。
6. **near 生产机的 7890**。基线必须用独立的 mihomo 实例与 7899 端口；
   不能 reload 或复用生产进程（§2.9）。

### 2.9 near 生产机的隔离

near 上已有生产 mihomo 监听 `127.0.0.1:7890`。
本设计额外起一个**独立** mihomo 进程，端口 7899；两者互不 reload、互不共享
配置目录。tbtun 的 carrier 拨号必须绕开生产 mihomo 的 TUN，否则 carrier 会被
自己的 TUN 抓走形成环路。仓库已有做法是主机策略路由：

```sh
ip rule add pref 100 to <far-ip>/32 lookup main   # 已存在则跳过
ip route get <far-ip>                            # 必须显示物理网卡，不是 Meta/198.18.0.1
```

证据：`tests/throughput/short.sh:42-43`、`:120-123`（注释明确写 mihomo 的 TUN 会把
carrier 送进 Meta）。这条规则只作用于到 far 的新流，不重启生产 mihomo。

---

## 3. 源码核对：字段、默认值、真实路径

这一节的目的，是让“配置面”可以逐条追溯到源码，而不是靠记忆。
版本：mihomo `0455d0d2`（Meta），`constant.Version = "1.10.0"`。

### 3.1 VLESS 出站字段

`adapter/outbound/vless.go:60-92` 的 `VlessOption` 里，与本设计有关的是：

```
65  Flow              string  `proxy:"flow,omitempty"`
66  TLS               bool    `proxy:"tls,omitempty"`
67  UDP               bool    `proxy:"udp,omitempty"`
68  PacketAddr        bool    `proxy:"packet-addr,omitempty"`
69  XUDP              bool    `proxy:"xudp,omitempty"`
70  PacketEncoding    string  `proxy:"packet-encoding,omitempty"`
72  Network           string  `proxy:"network,omitempty"`
```

`network` 的分派在 `:155-262`：`ws`/`http`/`h2`/`grpc`/`xhttp` 各有分支，
其余全部落到 `default:`（`:258-261`）——“default tcp network”。所以
`network: tcp` 不是特例，是默认路径；写出来只是为了显式。

`tls: false` 时 `streamTLSConn` 直接返回原连接（`:343-363`），不会做 TLS
握手。

### 3.2 XUDP 的默认值陷阱

`adapter/outbound/vless.go:474-485`：

```go
switch option.PacketEncoding {
case "packetaddr", "packet":
    option.PacketAddr = true
    option.XUDP = false
default:
    if !option.PacketAddr {
        option.XUDP = true
    }
}
if option.XUDP {
    option.PacketAddr = false
}
```

这段的意思是：**只要没写 `packet-encoding: packetaddr`，XUDP 默认就是开的。**
所以 `packet-encoding: xudp` 是显式化，不是“打开一个默认关闭的开关”。
真正决定 UDP 走哪条路径的是下面的 `ListenPacketContext`。
（如果写 `packet-encoding: packetaddr`，则 XUDP 关闭、走 packetaddr 编码；
这是 v2ray 5+ 的兼容路径，不是 XUDP。）

反过来，**`xudp: false` 不是关闭开关**：`default:` 分支在
`!option.PacketAddr` 时无条件把 `XUDP` 置 true，显式的 `false` 会被覆盖。
要关闭 XUDP，只能写 `packet-encoding: packetaddr`，并同时确认服务端支持
packetaddr。这是配置面里第二个容易踩的默认值。

`ListenPacketContext`（`:369-404`）：先 `dialContext` 新建一条 TCP，再
`StreamConnContext` 发 VLESS 请求；然后

```
387  if v.option.XUDP {
392      return ... vmessSing.NewXUDPConn(c, globalID, dst)
397  } else if v.option.PacketAddr {
403  return ... v.client.PacketConn(c, ...)
```

也就是说，**每次 `ListenPacketContext` 都是一次新的底层 TCP**。这条 TCP
在 tbtun near 看来就是一条新的应用连接。

### 3.3 sing-mux 的字段与默认值

mihomo 的包装：`adapter/parser.go:234-246`

```
234  if muxMapping, muxExist := mapping["smux"].(map[string]any); muxExist {
235      muxOption := &outbound.SingMuxOption{}
...
240      if muxOption.Enabled {
241          proxy, err = outbound.NewSingMux(*muxOption, proxy)
```

`SingMuxOption`：`adapter/outbound/singmux.go:23-33`

```
23  type SingMuxOption struct {
24      Enabled        bool   `proxy:"enabled,omitempty"`
25      Protocol       string `proxy:"protocol,omitempty"`
26      MaxConnections int    `proxy:"max-connections,omitempty"`
27      MinStreams     int    `proxy:"min-streams,omitempty"`
28      MaxStreams     int    `proxy:"max-streams,omitempty"`
29      Padding        bool   `proxy:"padding,omitempty"`
30      Statistic      bool   `proxy:"statistic,omitempty"`
31      OnlyTcp        bool   `proxy:"only-tcp,omitempty"`
32      BrutalOpts     ...
```

sing-mux 客户端（`github.com/metacubex/sing-mux v0.3.12`，commit `714fc2e`）：

- 默认协议 h2mux：`client.go:71-73`。
- `maxStreams == 0 && maxConnections == 0` → `minStreams = 8`
  （`client.go:65-67`）。但 `minStreams` 只在 `maxConnections > 0` 的分支里
  被使用（`client.go:180-183`）；默认的 `maxConnections == 0` 走
  `else`，那里只看 `maxStreams`（`:184-187`）。两者都是 0 时，`offer`
  在“现有 session 已经有一条流”之后就直接 `offerNew`（`:189`）。
  **也就是说，不写 `max-connections: 1` 的默认行为比“每 8 条流开一条”
  更激进：第二条逻辑连接就可能新建底层 TCP。**
- `DialContext` 对 TCP 和 UDP 都调 `openStream`（`client.go:84-110`）。
- `ListenPacket` 也调 `openStream`（`client.go:112-119`），返回
  `clientPacketAddrConn`（`client_conn.go` 里的带地址包封装）。
- `offer`（`client.go:150-190`）先清掉 `IsClosed()` 的 session；`maxConnections=1`
  时，如果现有 session 还能接新请求就复用，否则 `offerNew`
  （`client.go:192-228`）新建 TCP。
- h2mux 客户端 `ReadIdleTimeout = 30s`（`h2mux.go:173-190`，值在 `:179`），
  `CanTakeNewRequest` 直接代理 `http2.ClientConn.CanTakeNewRequest()`
  （`h2mux.go:260-262`）。
- smux 协议的 keepalive 被显式禁用（`session.go:128-131`：
  `config.KeepAliveDisabled = true`）。默认的 h2mux 有 30 s idle 探活；
  如果切到 `protocol: smux`，应用层就没有 keepalive。

### 3.4 VLESS 入站

`listener/inbound/vless.go:12-30` 的 `VlessOption` 包含
`AllowInsecure`（`:24`）与 `MuxOption`（`:29`）。
构造时把 `AllowInsecure` 与 `MuxOption.Build()` 传给
`LC.VlessServer`（`:99-136`）。

`listener/sing_vless/server.go:275-277`：

```go
} else if sl.decryption == nil && !config.AllowInsecure {
    return nil, errors.New("disallow using Vless without any certificates/shadow-tls/res-tls/jls/reality/decryption/allow-insecure config")
}
```

**这是 `allow-insecure: true` 的硬性理由**：没有证书/REALITY/解密时，不为
true 直接启动失败。绑 `127.0.0.1` 是这个开关的安全前提——它只应被本机的
tbtun 访问。

mux 的服务端路径：`listener/sing/sing.go:75-91` 总是
`mux.NewService(...)`；`MuxOption` 只映射 `Padding` 与 `Brutal`
（`:83-89`）。mux 会话通过目标 FQDN 是 magic 域名识别
（`listener/sing/sing.go:94-109`）。

XUDP 的服务端路径：`listener/sing_vless/service.go:100-145`。当 VLESS 请求的
command 是 `CommandMux` 时，走 `vmess.HandleMuxConnection`（`:141-142`）；
普通 UDP 走 `CommandUDP`（`:136-140`）。客户端在 XUDP 下发的正是
`CommandMux`：`transport/vless/conn.go:101-105`：

```go
if vc.dst.Mux {
    buf.Must(buffer.WriteByte(CommandMux))
} else {
    if vc.dst.UDP {
        buf.Must(buffer.WriteByte(CommandUDP))
```

`CommandMux` 的常量在 `transport/vless/vless.go:23-24`；
`DstAddr.Mux` 的注释是 “currently used for XUDP only”（`:40`）。

**服务端没有 xudp 开关。** 它无条件处理 `CommandMux`。所以“两端一致”的
准确含义是：客户端 `udp: true` + XUDP 打开；服务端是支持 VLESS
`CommandMux` 的实现。mihomo 的入站在 `listener/sing_vless/service.go:141-142`
明确处理 `CommandMux`，这是本文核对过的服务端。xray 等其它实现的对应版本
需要在选定版本上单独核对，本文不替它背书。如果服务端只有 packetaddr，
则应改用 `packet-encoding: packetaddr`，两端都换。

### 3.5 字段存在性表（结论）

| 想写的字段 | 是否存在 | 结论/替代 |
|---|---|---|
| `smux.enabled: true` | 是 | 唯一开启 mux 的方式 |
| `smux.protocol: h2mux` | 是 | 默认就是 h2mux；可省略 |
| `smux.max-connections: 1` | 是 | **必须显式写**，否则默认会按 8 条流开新连接 |
| `smux.only-tcp` | 是 | §4.5 的关键开关 |
| `xudp: true` | 是（`vless.go:69`） | 与 `packet-encoding: xudp` 等价；wiki 未列 |
| `packet-encoding: xudp` | 是（`vless.go:70`） | 字符串；默认分支会打开 XUDP |
| 入站 `mux: true` / `xudp: true` | **不存在** | 入站总是能接 mux；XUDP 由 `CommandMux` 自动处理 |
| 入站 `allow-insecure` | 是 | 无证书时必须 true |
| `flow: xtls-rprx-vision` + mux | 不建议 | 与本文的明文 TCP 路径无关；会引入 TLS/REALITY 要求 |

---

## 4. 一条长连接：流语义

### 4.1 tbtun 当前只允许一条并发 FEC 流

这是 v0.9.0 设计必须围绕的中心事实。

内核侧：

- `src/kernel/tbt_fec_data.c:8813-8818`：

  ```c
  if (sess->n_fec_flows)
  {
      spin_unlock(&sess->lock);
      mutex_unlock(&sess->fec_alloc_lock);
      return ERR_PTR(-ENOSPC);
  }
  ```

  第二条流直接 `-ENOSPC`，不管 group 是否空闲。

- `tbt_fec_data.c:8764-8769` 的 `tbt_fec_flow_admit_mask` 同样。

- `tbt_sock.c:989-1004`：`tbt_fec_flow_admit` 返回 `-ENOSPC` 时
  `atomic64_inc(&sess->fec_stream_refused)`，然后
  `return err == -ENOSPC ? -EOPNOTSUPP : -EINVAL;`。

- `tbt_sock.c:1086-1103`：`-EOPNOTSUPP` 的 pending socket 会被
  `tbt_sock_abort_on_close(sock)`，即连接被 reset。注释写得很直白：
  “a two-connection protocol ... must see the refusal as an error instead of
  hanging”。

- 第一条流本身拿到**全部** group。near 的 `tbt_fec_flow_admit`
  （`tbt_fec_data.c:8819-8827`）用 `tbt_fec_free_groups_locked` 取走所有
  空闲组；far 的 SC-LDPC 分支（`:8771-8780`）把本地 `fec_groups_total` 个
  bit 全部置 1，再 `tbt_fec_flow_create(... local, local_count, mask)`。
  上行 8 组全被这一条流占满。`tbt_fec_free_groups_locked`（`:8531-8578`）
  对此后没有剩余。

- `tbt_core.c:77-81`：`fec_groups_per_flow` 参数被保留但**忽略**。注释：
  “0.7.0 carries one FEC flow per session and that flow owns every group”。

用户态侧：

- `TBT_FLOW_MAX = 8`（`src/client/tbt_client_engine.c:52-53`）。每条应用连接一个
  `flow` 槽；第 9 条在 `engine_accept_app` 里 `flow_alloc` 失败后立即
  `close(fd)`（`:2991-2996`），没有 RST、没有日志（除非 verbose）。
- 用户态没有 `fec_stream_refused` 计数器；它的“拒绝”表现为 accept 后关闭。

结论：**同一时刻，near 只能有一条应用连接对应的 FEC 流。** mihomo 的 mux
必须把所有逻辑连接压到这一条 TCP 上。

### 4.2 mihomo 怎样把 N 条逻辑流压成一条 TCP

链条：

1. 浏览器连 mihomo 的 7899，产生 N 条本地 TCP。
2. mihomo 路由到 `tbtun-vless`。`parser.go:240-241` 用 `SingMux` 包住
   `Vless`。
3. `SingMux.DialContext` 调 `sing-mux` 的 `client.DialContext`
   （`singmux.go:41-47`），后者 `openStream`（`client.go:84-110`）。
4. `openStream` → `offer`（`client.go:150-190`）：已有 session 可接新请求
   就复用；否则 `offerNew`（`:192-228`）用底层 proxy 新建一条 TCP。
5. 这条底层 TCP 由 `Vless.DialContext` 建立，目标是 sing-mux 的 magic FQDN
   （`client.go:195` 的 `Destination`），所以服务端能识别为 mux 会话。
6. 于是 N 条逻辑流共享 1 条 TCP；tbtun 只看到 1 条应用连接、1 条 FEC 流。

`max-connections: 1` 的作用：让第 4 步在现有 session 仍 `CanTakeNewRequest`
时不再新建。默认 0/0 虽然会设 `minStreams = 8`（`client.go:65-67`），但
`minStreams` 只在 `maxConnections > 0` 时参与判断（`:180-183`）；默认路径
在现有 session 已有一条流后就 `offerNew`（`:189`）。**不写
`max-connections: 1`，第二条逻辑连接就可能新建底层 TCP，直接撞上 tbtun
的单流限制。** 这是本设计里最容易漏掉的默认值。

### 4.3 断线重连时序

mihomo 的 mux 客户端没有“重连”这个显式动作；它是在下一次 `openStream`
时由 `offer` 发现旧 session `IsClosed()`，移除后 `offerNew`。所以时序是
“懒重建”。

| 事件 | mihomo 侧 | tbtun near 侧 | 结果 |
|---|---|---|---|
| 对端 RST / FIN，链路正常 | `IsClosed()` 为真；旧 session 移除；下次请求 `offerNew` → 新 TCP | 旧 TCP 的 FIN 到达，stream teardown，`tbt_fec_flow_detach` 立即释放 flow（CHANGELOG 0.8.0） | 新 TCP 通常立刻能 admit；新旧重叠窗口是毫秒级 |
| 半开（链路黑洞，无 RST） | h2mux `ReadIdleTimeout = 30s`（`h2mux.go:179`）之前，`CanTakeNewRequest` 仍为真，新请求继续复用这条已经发不出去的连接 | 旧 flow 没有 FIN，`n_fec_flows` 保持 1 | 新请求超时/失败；**不会**立刻新建 TCP；约 30–45 s（30 s 读空闲 + Go http2 的 PING 超时，默认 15 s 量级）后才 `offerNew` |
| GOAWAY / 达到 HTTP/2 并发上限 | `http2.ClientConn.CanTakeNewRequest()` 为 false（`h2mux.go:260-262`）；`offer` 的 `Filter` 为空 → `offerNew`，**即使 `max-connections: 1`** | 旧 flow 可能还没 detach | 新 TCP 可能被 `-ENOSPC` 拒绝；旧连接关闭后重试才成功 |
| 进程重启（kill -9） | 所有 socket 由内核关闭，发 FIN/RST | 旧 flow 收到 FIN 后释放 | 新进程首次连接通常成功 |
| 用户态 near | 与上面相同 | 旧 flow 在 `fin_sent && fin_applied` 后释放；最坏等 `close_at_ns = +10s`（`tbt_client_engine.c:2764-2773`） | 8 个槽；正常 1–2 个；风暴才可能撞上限 |

需要实测的两点（§9 Q1/Q2）：

1. 半开窗口里，mihomo 是否真的会复用旧连接约 30–45 s，期间所有新请求都失败。
2. 窗口结束、`offerNew` 发生时，旧 flow 是否已经 detach；如果没有，新 TCP 会
   不会持续被 `fec_stream_refused` 拒绝，直到某个更长的超时。

### 4.4 `TBT_FLOW_MAX` 与 `fec_stream_refused` 的判定表

| near 实现 | 计数器/上限 | 正常基线占用 | 触发条件 | 观测 |
|---|---|---|---|---|
| 内核 | `n_fec_flows`（每条会话 1） | 1 | 第二条应用连接（XUDP、重连重叠、第二条未合并的 mux TCP） | `fec_stream_refused` 增加；连接被 RST；`/proc/net/tbtun/stats` c9 |
| 内核 | `TBT_STREAMS_MAX`（128，`tbtun.h`） | 1 | 流表满 | `-EBUSY`，pending 排队而不是 reset |
| 用户态 | `TBT_FLOW_MAX = 8`（`tbt_client_engine.c:53`） | 1 | 第 9 条应用连接 | accept 后 `close(fd)`，无日志 |
| 用户态 | `mode=sc-ldpc` + `carriers=256` | — | 配置不对 | 启动报错（`:3190-3193`、`:3212-3216`） |

注意：内核的 `fec_stream_refused` 不是“网络错误”，而是“tbtun 的 FEC 流表
没有位置”。它与 carrier 数量、带宽、丢包都无关。看到它增长，第一反应应该
是“有第二条应用连接”，而不是“链路不好”。

### 4.5 mux 与 XUDP 的硬冲突

先把两条路径摆出来（均以 `smux.enabled: true` 为前提）：

**路径 A：`only-tcp: false`（默认）**

- `SingMux.ListenPacketContext` 不调用底层 `Vless.ListenPacketContext`，
  而是 `s.client.ListenPacket`（`singmux.go:49-62`）。
- sing-mux 的 `ListenPacket` 调 `openStream`（`client.go:112-119`），
  在**同一条底层 TCP** 上开一条 UDP 流，用 `clientPacketAddrConn` 的
  socksaddr+length 封装（`client_conn.go`）。
- 结果：UDP 可用，隧道内仍只有 **1** 条 TCP / 1 条 FEC 流。
- 代价：`packet-encoding: xudp` **完全不参与**。客户端根本不会发
  `CommandMux`；`vless.go:387` 的 XUDP 分支不会执行。

**路径 B：`only-tcp: true`**

- `SingMux.ListenPacketContext` 直接返回
  `s.ProxyAdapter.ListenPacketContext`（`singmux.go:49-52`），即
  `Vless.ListenPacketContext`。
- 它先 `dialContext` 新建一条 TCP（`vless.go:369-385`），再按 XUDP 发
  `CommandMux`（`vless.go:387-396`、`conn.go:101-105`）。
- 结果：XUDP 真正生效；但这条 TCP 是 tbtun near 的**第二条应用连接**。
  内核 `n_fec_flows != 0` → `-ENOSPC` → `fec_stream_refused` → reset。
- 只有在 mux 那条 TCP 还没建立、UDP 先到时，UDP 会占住唯一 flow，随后
  TCP 被拒——所有网页都打不开。两个方向都不可接受。

**判定**：

| 需求 | 选择 | 今天能否跑通 |
|---|---|---|
| 网页 + UDP 都要，且只占一条流 | `only-tcp: false` | 能；但 UDP 是 mux 封装，不是 XUDP |
| 必须 XUDP | `only-tcp: true` | 不能；UDP 触发第二条 FEC 流被拒 |
| 只测 TCP，不需要 UDP | `only-tcp: true` 或 false 均可 | 能；UDP 不触发 |
| 将来 tbtun 支持多 FEC 流后 | `only-tcp: true` | 可以同时要 XUDP 与 mux |

因此 v0.9.0 的基线建议：

- **默认配置 `only-tcp: false`**，把“UDP 可用”作为基线能力，把
  `packet-encoding: xudp` 保留在配置里但明确注释“当前不参与”。
- 单独开一个实验配置 `only-tcp: true`，只用来证明冲突（S5），不作为交付。
- 如果产品必须 XUDP，则阻塞项是 tbtun 的多 FEC 流支持，不是 mihomo 配置。

这不是“配置没写对”，而是两条代码路径在当前 tbtun 上互斥。文档必须这样写，
否则运维会以为“把 xudp 打开就好了”。

---

## 5. 分段、MTU 与时延

### 5.1 固定的“MTU”

FEC cell 是固定的：

- `TBT_FEC_S = 1024`：每个 DATA/PARITY 槽 1024 字节。
- FEC 记录 = 48 字节头 + 1024 字节体 = **1072 字节**。
- http-kal chunk = `"430\r\n"`（5）+ 1072 + `"\r\n"`（2）= **1079 字节**。
- 没有 `S=` 或 `mtu=` 配置键。`S` 是编译期常量，不能按链路调。

### 5.2 大写入怎样变成很多 share

应用的一次 `write()` 在内核里被切成 1024 字节的 share：

- 上行（near→far）：TX shape 是 uplink `8x24`、`groups=8`，
  即每 block `8×8=64` 个 DATA share，**64 KiB 源数据**，随后 192 个 parity
  share（`8×24`）。
- 下行（far→near）：TX shape 是 downlink `128x128`、`groups=1`，
  即每 block `128` 个 DATA share，**128 KiB 源数据**，随后 128 个 parity
  share。
- 每个 share 走一条固定 carrier（`carrier = group × (n+m) + share`），
  所以 256 条 carrier 各自只承载自己那个 share 位置。

一个 100 MiB 下载大约是 102400 个下行 share、800 个下行 block。这个量级
不是问题；问题是每个 block 的 parity 比例（这里 50%）会在 WAN 上放大流量。

### 5.3 小请求的可观察时延

小请求（比如 HTTP GET，几十到几百字节）不会填满 1024 字节，所以要等
“封口”定时器：

- 用户态客户端：`TBT_TX_IDLE_SEAL_NS = 25 ms`
  （`src/client/tbt_client_engine.c:64-66`），在 `flow_tick` 里检查
  （`:2697-2705`）。所以一个方向至少 +25 ms。
- 内核 near：交互尾部 `TBT_FEC_SHORT_MS = 2 ms`，但只对“流的第一个
  partial packet”生效；后续 partial 等 `TBT_FEC_BLOCK_IDLE_MS = 25 ms`
  （`src/kernel/tbt_fec.h:216-233`，逻辑在 `tbt_fec_data.c:2726-2740`）。
  还有一个 `TBT_FEC_CUT_GRACE_MS = 100 ms` 的接缝宽限
  （`tbt_fec.h:234-240`）。
- 两个方向都要封口：请求一次、响应一次。所以**交互请求的额外时延大约是
  `25 ms + RTT + 25 ms`**（用户态）或 `2–25 ms + RTT + 2–25 ms`（内核，
  取决于是否首包）。300 ms WAN RTT 下，总时延约 350 ms；这不是 mihomo 的
  问题，是 tbtun 的 cell 封口成本。
- FIN 会立即封口，不等定时器（`tbt_fec.h:232-233`）。短连接（HTTP/1.0 +
  close）比 keep-alive 快，但 mihomo 的 mux 本身是长连接。

可观测证据：S1 用 `curl -w '%{time_total}'` 记录；对比直连 WAN 的 RTT，
多出来的部分应落在 50–60 ms（用户态）或 5–50 ms（内核）区间。若远超，
先查是不是触发了 25 ms block idle 的重复封口。

---

## 6. 零补丁装配

### 6.1 原则

- **只组装，不修改。** mihomo 用官方仓库固定 ref 构建；tbtun 用本树构建；
  配置文件由模板渲染。没有 patch、没有 fork、没有 vendor。
- **两种装配面**：
  - host 装配：内核模块 + mihomo 二进制 + systemd。far 必须走这条（ko 只能
    在宿主内核加载）。
  - 容器装配：用户态 `tbt_client` + mihomo 二进制。near 可以走这条；far 若
    不用 ko 也可以。
- **升级 mihomo 只换 ref 与校验和**，不改我们的文件；升级后按 §3 重跑
  字段核对（`deploy/tools/verify-versions.sh`）。

### 6.2 文件布局

新增目录 `deploy/`（不碰上游源码树的其他部分）：

```
deploy/
  Makefile                     # 自定义装配目标；根 Makefile 只做转发
  Dockerfile.mihomo            # 从官方源码构建原版 mihomo
  Dockerfile.tbtun             # 构建 tbt_client（容器用）
  compose.near.yaml            # near: mihomo + tbt_client（用户态变体）
  compose.far.yaml             # far:  mihomo + tbt_client（用户态变体）
  config/
    mihomo-near.yaml.tmpl
    mihomo-far.yaml.tmpl
    tbtun-near.conf.tmpl
    tbtun-far.conf.tmpl
  systemd/
    tbtun-near.service
    tbtun-far.service
    mihomo-near.service
    mihomo-far.service
  tools/
    render-config.sh
    verify-versions.sh
    smoke-netns.sh
    smoke-udp.py
  README.md                    # 只写操作步骤，不重复本设计
```

根 `Makefile` 只加转发目标（可选）：

```make
dist-mihomo dist-tbtun dist-configs dist-bundle:
	$(MAKE) -C deploy $@
```

### 6.3 `deploy/Makefile` 的目标设计

变量：

```make
MIHOMO_REPO   ?= https://github.com/MetaCubeX/mihomo
MIHOMO_REF    ?= 0455d0d2        # 本文核对的是这个 commit；生产改成 tag/commit 并固定
MIHOMO_SHA256 ?=                  # 构建产物的期望校验和；空则打印不校验
GO_VERSION    ?= 1.20             # mihomo go.mod 声明 go 1.20；用 1.20+ 构建
ARCH          ?= amd64
OUT           ?= $(CURDIR)/out
```

目标：

1. `dist-mihomo`
   - `git clone --depth 1 --branch $(MIHOMO_REF) $(MIHOMO_REPO) $(OUT)/src/mihomo`
     （如果 ref 是 commit，用 `git init && git fetch --depth 1 origin <sha> && git checkout FETCH_HEAD`）。
   - `cd $(OUT)/src/mihomo && CGO_ENABLED=0 GOOS=linux GOARCH=$(ARCH) \
     go build -tags with_gvisor -trimpath \
     -ldflags '-X github.com/metacubex/mihomo/constant.Version=$(MIHOMO_REF)' \
     -o $(OUT)/bin/mihomo-$(ARCH) .`
   - 这段命令与上游 `Makefile:16-21` 的 `GOBUILD` 一致；我们不复制源码，只
     调用它。
   - 成功后 `$(OUT)/bin/mihomo -v` 必须打印版本；`sha256sum` 写入
     `$(OUT)/mihomo.sha256`。
   - 若 `MIHOMO_SHA256` 非空，校验失败即退出。

2. `dist-tbtun`
   - 模块：`$(MAKE) -C .. all`，产物
     `../tbtun.ko`，复制到 `$(OUT)/bin/tbtun.ko`。
   - 用户态：`$(MAKE) -C .. src/client/tbt_client`，复制到
     `$(OUT)/bin/tbt_client`。
   - 记录 `git -C .. rev-parse HEAD`、`TBT_VERSION`（从
     `../src/kernel/tbt_uapi.h` 解析）到 `$(OUT)/tbtun.version`。

3. `dist-configs`
   - 对每个 `.tmpl` 用 `envsubst` 渲染；必需变量：
     `NEAR_VLESS_PORT`（默认 19080）、`FAR_VLESS_PORT`（默认 19100）、
     `FAR_CARRIER_PORT`（默认 19000）、`FAR_IP`、`UUID`、`NONCE`、
     `MIHOMO_MIXED_PORT`（默认 7899）。
   - 渲染前检查 `UUID` 是合法 UUID、`NONCE` 是 32 hex；不合法直接失败。

4. `dist-bundle`
   - `tar --zstd` 打包 `out/bin`、`out/config`、`deploy/systemd`、
     `deploy/README.md` 与 `out/*.sha256`。
   - 命名 `tbtun-mihomo-<tbtun-version>-<mihomo-ref>-<arch>.tar.zst`。

5. `dist-verify`
   - 对 `out/bin/mihomo-*` 跑 `-v`；对 `mihomo` 源码目录
     `git diff --quiet`（证明没改）；对 `tbtun.ko` 跑 `modinfo`。
   - `deploy/tools/verify-versions.sh` 重跑 §3 的源码行号检查（见 6.6）。

6. `dist-compose-up` / `dist-compose-down`
   - 只用于 near 的容器变体或实验室；生产 far 用 systemd + ko。

### 6.4 `Dockerfile.mihomo`

多阶段，不 patch：

```dockerfile
FROM golang:1.22 AS build
ARG MIHOMO_REPO=https://github.com/MetaCubeX/mihomo
ARG MIHOMO_REF=0455d0d2
RUN git clone --depth 1 --branch "$MIHOMO_REF" "$MIHOMO_REPO" /src/mihomo
WORKDIR /src/mihomo
RUN CGO_ENABLED=0 go build -tags with_gvisor -trimpath \
    -ldflags "-X github.com/metacubex/mihomo/constant.Version=$MIHOMO_REF" \
    -o /out/mihomo .

FROM scratch
COPY --from=build /out/mihomo /mihomo
ENTRYPOINT ["/mihomo"]
```

`Dockerfile.tbtun` 类似：构建 `tbt_client`，运行阶段只放二进制。内核模块
**不放进容器**；far 的 ko 由宿主加载。

### 6.5 compose

`deploy/compose.near.yaml`：

```yaml
services:
  mihomo-near:
    build:
      context: ..
      dockerfile: deploy/Dockerfile.mihomo
    network_mode: host
    volumes:
      - ./out/config/mihomo-near.yaml:/config.yaml:ro
    command: ["-d", "/", "-f", "/config.yaml"]
    restart: unless-stopped

  tbt-client:
    build:
      context: ..
      dockerfile: deploy/Dockerfile.tbtun
    network_mode: host
    volumes:
      - ./out/config/tbtun-near.conf:/etc/tbtun.conf:ro
    command: ["--config", "/etc/tbtun.conf"]
    restart: unless-stopped
    depends_on: [mihomo-near]
```

`network_mode: host` 是为了让两个进程共享 near 的回环与 19080 端口。
far 的 compose 同理，只是配置反过来；但生产 far 推荐 systemd + ko，不推荐
容器（模块在宿主，容器里的 tbt_client 不是同一条路径）。

**注意**：默认 bridge netns 的容器看不到 `/proc/net/tbtun/*`。tbtun 的控制面
只创建在 init_net（`src/kernel/tbt_procfs.c:199-202` 的
`cf->net = get_net(&init_net)`，`:1600-1605` 的 `init_net.proc_net`）。
`network_mode: host` 的容器与宿主共享 netns，理论上可以写这个文件，但这要求
root/CAP_NET_ADMIN，且把内核模块的配置权限交给容器并不划算。生产 far 用
宿主 systemd + ko；容器只承载用户态 `tbt_client`。

### 6.6 版本锁定与升级

- `MIHOMO_REF` 固定到 tag 或 commit；构建产物记录 SHA-256。
- 升级流程：
  1. 改 `MIHOMO_REF`；
  2. `make dist-mihomo`；
  3. `deploy/tools/verify-versions.sh` 重新核对本文 §3 的行号与字段；
  4. 若行号漂移但字段仍在，更新文档的行号；若字段消失，按 §3.5 的替代方案
     改配置，不 patch；
  5. 跑 S1–S3。
- `verify-versions.sh` 的检查项（只读，不改源码）：
  - `constant/version.go` 的 `Version`；
  - `adapter/outbound/vless.go` 里 `XUDP` / `PacketEncoding` / `Network` 字段；
  - `adapter/outbound/singmux.go` 里 `only-tcp` / `max-connections`；
  - `listener/sing_vless/server.go` 里 `allow-insecure` 的报错；
  - `src/kernel/tbt_fec_data.c` 里 `n_fec_flows` 的 `-ENOSPC`。
  - 用 `grep -n` 定位后与本文记录的行号对比，输出 drift 报告。

---

## 7. 冒烟计划

环境：两台 Linux 实验主机，记为 `$NEAR` 与 `$FAR`（也可以是“一台宿主 +
一台 QEMU guest”，guest 跑 far）。**不碰 near/far 生产机。**

为什么不用 network namespace：tbtun 的控制面只创建在 init_net。
`src/kernel/tbt_procfs.c:199-202` 明确写 `cf->net = get_net(&init_net)`，
`:1600-1605` 用 `init_net.proc_net` 创建 `/proc/net/tbtun`；同一段注释
还明确说不要靠 bind-mount 的 `/proc` 把 tunnel 移进调用者的 netns。
netns 里看不到 `/proc/net/tbtun/config`，把 tbtun 放进 netns 的冒烟方案
跑不起来。所以两端 tbtun 各在自己的 init_net，WAN 用两台机器的真实链路
（或 QEMU guest 的内部链路）加 `netem` 模拟。

变量：

```sh
NEAR=root@lab-near          # near 实验机
FAR=root@lab-far            # far 实验机
FAR_IP=203.0.113.10         # far 的地址；按实验环境改
```

每个步骤都假设 S0 已执行；S0 之外的步骤互相独立，可以单独重跑。

### S0 — 环境、版本、端口

```sh
set -e
# 版本（两台都要）
ssh $NEAR 'modinfo tbtun | sed -n "1,6p"; mihomo -v'
ssh $FAR  'modinfo tbtun | sed -n "1,6p"; mihomo -v'

# 端口空闲
ssh $NEAR 'ss -lnt | grep -E ":(7899|19080)\b" && exit 1 || true'
ssh $FAR  'ss -lnt | grep -E ":(19000|19100)\b" && exit 1 || true'

# 只在实验链路加 netem；不要在生产机或管理网卡上加。
# 网卡名按实验环境改；300 ms RTT + 1% 丢包。
ssh $NEAR 'tc qdisc replace dev eth0 root netem delay 150ms loss 1%'
ssh $FAR  'tc qdisc replace dev eth0 root netem delay 150ms loss 1%'
```

预期：两台都是 tbtun 0.8.0；mihomo 打印 1.10.0；端口无输出；两条 `netem`
规则存在。`netem` 会同时影响 SSH，操作时用 `-o ConnectTimeout=10`。

**独立运行**：S0 可重复执行；重复时 `tc qdisc replace` 是幂等的。

### S1 — 小 HTTP 请求（单流、单请求）

far 启动一个 HTTP 服务，然后按顺序启动 far 与 near 的两端：

```sh
# far: 目标站点（模拟“任意网站”）
ssh $FAR 'python3 -m http.server 18080 --bind 0.0.0.0 \
  --directory /srv/http >/tmp/far-http.log 2>&1 &'

# far: tbtun far（tunnel 2）+ mihomo 服务端
ssh $FAR "echo 'tunnel 2 role=far listen=0.0.0.0:19000 \
  target=127.0.0.1:19100 n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 \
  crypto=on adaptive=off profile=http-kal keepalive=5000 \
  nonce=00112233445566778899aabbccddeeff' > /proc/net/tbtun/config"
ssh $FAR 'mihomo -d /etc/mihomo-far -f /etc/mihomo-far/config.yaml \
  >/tmp/mihomo-far.log 2>&1 &'
sleep 2

# near: tbtun near（tunnel 1）+ mihomo 客户端
ssh $NEAR "echo 'tunnel 1 role=near listen=127.0.0.1:19080 \
  peer=$FAR_IP:19000 n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 \
  crypto=on adaptive=off profile=http-kal keepalive=5000 \
  nonce=00112233445566778899aabbccddeeff' > /proc/net/tbtun/config"
ssh $NEAR 'mihomo -d /etc/mihomo-near -f /etc/mihomo-near/config.yaml \
  >/tmp/mihomo-near.log 2>&1 &'
sleep 3

# 断言：256 条 carrier 已建立、mihomo 只开了一条 app 连接
ssh $NEAR "ss -tn '( dport = :19000 )' | tail -n +2 | wc -l"  # 期望 256
ssh $NEAR "ss -tn '( dport = :19080 )' | tail -n +2 | wc -l"  # 期望 1

# 请求（在 near 执行；7899 是 near 的回环端口）
ssh $NEAR "curl -sS -x socks5h://127.0.0.1:7899 \
  -o /tmp/hello.out -w 'code=%{http_code} time=%{time_total}\n' \
  http://$FAR_IP:18080/hello.txt"
ssh $NEAR 'cat /tmp/hello.out'
```

预期：`code=200`；`hello.out` 与 far 的文件一致；`19080` 只有一条 established
TCP；near 的 `/proc/net/tbtun/stats` 里 `fec_stream_refused=0`：

```sh
ssh $NEAR "grep -o 'fec_stream_refused=[0-9]*' \
  /proc/net/tbtun/stats | cut -d= -f2"     # 期望 0
```

**独立运行**：只重跑最后两条（`ss` 与 `curl`）；前提是 S0 与两端的进程还在。

### S2 — 大下载 + SHA-256

```sh
ssh $FAR 'head -c 104857600 /dev/urandom > /srv/http/100m.bin'

ssh $NEAR "curl -sS -x socks5h://127.0.0.1:7899 -o /tmp/100m.out \
  -w 'code=%{http_code} bytes=%{size_download} time=%{time_total}\n' \
  http://$FAR_IP:18080/100m.bin"

FAR_SUM=$(ssh $FAR 'sha256sum /srv/http/100m.bin' | cut -d' ' -f1)
NEAR_SUM=$(ssh $NEAR 'sha256sum /tmp/100m.out' | cut -d' ' -f1)
[ "$FAR_SUM" = "$NEAR_SUM" ] && echo SHA_OK
```

预期：`code=200`、`bytes=104857600`、`SHA_OK`；`fec_stream_refused`
仍为 0；near 的 `ss` 仍只有 1 条到 19080。

**独立运行**：只重跑 `curl` 与两个校验命令；文件可预先生成。

### S3 — mux 并发（证明 N 条逻辑连接只占 1 条流）

```sh
for i in $(seq 1 8); do
  ssh $NEAR "curl -sS -x socks5h://127.0.0.1:7899 -o /tmp/c$i.out \
    -w 'req=$i code=%{http_code} time=%{time_total}\n' \
    http://$FAR_IP:18080/hello.txt" &
done
wait
ssh $NEAR "ss -tn '( dport = :19080 )' | tail -n +2 | wc -l"  # 期望 1
ssh $NEAR "grep -o 'fec_stream_refused=[0-9]*' \
  /proc/net/tbtun/stats | cut -d= -f2"     # 期望 0
FAR_SUM=$(ssh $FAR 'sha256sum /srv/http/hello.txt' | cut -d' ' -f1)
ssh $NEAR 'sha256sum /tmp/c*.out' | awk -v want="$FAR_SUM" '$1!=want{print "BAD",$2}'
```

预期：8 条请求全部成功；`19080` 仍只有 **1** 条 TCP；没有
`fec_stream_refused`。这条是“mux 绕开 tbtun 单流限制”的直接证据。

如果 `ss` 显示 2 条：先查 `smux.max-connections` 是否真的是 1，再查是否
并发超过了 h2mux 的 `MaxConcurrentStreams`（由服务端 HTTP/2 设置决定；
8 条通常远低于默认值，但这一点需要实验确认，见 §9 Q3）。

**独立运行**：可重复；每次先清 `/tmp/c*.out`。

### S4 — UDP 走 mux（`only-tcp: false`）

需要两个脚本（放在 `deploy/tools/`，随装配包分发）：

- `udp-echo.py`：far 上 `bind(("0.0.0.0", 18081))`，回 `echo:` + 原数据。
- `smoke-udp.py`：near 上连 `127.0.0.1:7899`，做 SOCKS5 无认证握手；
  `CMD=3`（UDP ASSOCIATE），拿到中继地址；向 `$FAR_IP:18081` 发一个
  UDP 数据报；读回包，打印 `OK <payload>` 或 `FAIL`。

far 上的 UDP echo：

```sh
scp deploy/tools/udp-echo.py $FAR:/tmp/udp-echo.py
ssh $FAR 'python3 /tmp/udp-echo.py >/tmp/udp-echo.log 2>&1 &'
```

运行：

```sh
scp deploy/tools/smoke-udp.py $NEAR:/tmp/smoke-udp.py
ssh $NEAR "python3 /tmp/smoke-udp.py 127.0.0.1 7899 $FAR_IP 18081 hello"
ssh $NEAR "ss -tn '( dport = :19080 )' | tail -n +2 | wc -l"  # 仍期望 1
```

预期：打印 `OK echo:hello`；`19080` 仍只有 1 条 TCP（UDP 在 mux 流内）；
`fec_stream_refused` 为 0。

**独立运行**：echo 服务在跑即可；每次发一个包。

### S5 — XUDP 判定（`only-tcp: true`，预期失败）

把 near 的 mihomo 配置改成：

```yaml
    smux:
      enabled: true
      protocol: h2mux
      max-connections: 1
      only-tcp: true
```

重启 near mihomo（命令同 S6a），重跑 S1（TCP 应该成功），再跑 S4 的 UDP。

预期：

- TCP 仍成功；`19080` 有 1 条 mux TCP。
- UDP 触发第二条 TCP；tbtun near 的 `fec_stream_refused` 变为 1（或增长），
  UDP 客户端 `FAIL`；`dmesg` 可能有 reset 记录。
- 这证明“mux + XUDP 在当前 tbtun 上互斥”，不是配置错误。

**独立运行**：只改 near 配置并重启 mihomo；far 不动。

### S6 — 断线重连

**S6a 正常关闭**：

```sh
ssh $NEAR "pkill -f '/etc/mihomo-near' ; sleep 1"
ssh $NEAR 'mihomo -d /etc/mihomo-near -f /etc/mihomo-near/config.yaml \
  >/tmp/mihomo-near.log 2>&1 &'
sleep 3
ssh $NEAR "curl -sS -x socks5h://127.0.0.1:7899 -o /dev/null \
  -w '%{http_code}\n' http://$FAR_IP:18080/hello.txt"
```

预期：`200`；`fec_stream_refused` 不增长；`19080` 回到 1 条。

**S6b 半开（无 RST）**：

```sh
# 在 near 丢弃到 far:19000 的包，但不发 RST
ssh $NEAR 'iptables -A OUTPUT -p tcp --dport 19000 -j DROP'
# 触发一次请求，观察 mihomo 复用了旧连接
ssh $NEAR "time curl -sS -m 5 -x socks5h://127.0.0.1:7899 \
  http://$FAR_IP:18080/hello.txt || echo TIMEOUT"
# 等过 30 s 读空闲 + PING 超时；留 50 s 余量
sleep 50
ssh $NEAR 'iptables -D OUTPUT -p tcp --dport 19000 -j DROP'
ssh $NEAR "curl -sS -m 10 -x socks5h://127.0.0.1:7899 \
  http://$FAR_IP:18080/hello.txt -w '%{http_code}\n'"
```

预期：第一次在 5 s 内超时（h2mux 仍在复用旧连接）；约 30–45 s 后 h2mux
判定 dead，重建 TCP；第二次 `200`。如果在这个窗口内看到
`fec_stream_refused` 增长，说明新 TCP 抢在旧 flow detach 前到达——这就是
§4.3 的风险，需要记录窗口宽度。

**独立运行**：S6a 与 S6b 互不影响；S6b 必须清理 iptables 规则。

### S7 — 外观抓包

```sh
ssh $NEAR 'timeout 15 tcpdump -i eth0 -s 0 -A -c 60 "tcp port 19000" \
  > /tmp/cap.txt 2>/dev/null &'
sleep 1
ssh $NEAR "curl -sS -x socks5h://127.0.0.1:7899 -o /dev/null \
  http://$FAR_IP:18080/hello.txt"
sleep 3
ssh $NEAR "grep -E 'POST /api/v2/upload|Content-Type: application/gzip|User-Agent: Mozilla' /tmp/cap.txt"
ssh $NEAR "grep -E '^0*[0-9a-f]+\r$' /tmp/cap.txt | head"   # 大文件时是 430（1072 的十六进制）
```

预期：出现 `POST /api/v2/upload HTTP/1.1`、`Transfer-Encoding: chunked`、
`Connection: keep-alive`、`Content-Type: application/gzip`、Chrome UA；
大文件时 chunk 大小行是 `430`（1072 的十六进制）。**不应**出现
`VLESS`、`tbtun`、`vless` 等字符串。

**独立运行**：抓包需要 tcpdump；可重复。

---

## 8. 风险

### 8.1 VLESS 不加密时，隧道内层的可识别性

这是本设计最需要说清楚的安全边界。

- **VLESS 请求头是明文**：version(1) + uuid(16) + addons_len(1) +
  command(1) + port(2) + addr_type(1) + addr。TCP 请求的第一个字节通常是
  `0x00`，后面紧跟 16 字节 UUID。任何能看到这层字节的人都能识别出
  “这是 VLESS”，并读出 UUID（从而冒充客户端，如果服务端只认 UUID）。
- **mux 层也是明文**：`h2mux` 在 VLESS 之上跑 HTTP/2，首帧是
  `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`。解开 VLESS 后，mux 的帧结构完全可读。
- **tbtun 的 `crypto=on` 不解决这个问题**：它是 ChaCha8 异或，密钥在 HELLO
  里明文传输（`tbt_crypto_hello_fill`），data 的 nonce 折叠也只防重放/重用，
  不提供认证加密。被动观察者只要解析 tbtun 的 HELLO，就能拿到 key 并解密
  所有 DATA。所以 `crypto=on` 的定位是“让外层不像明文”，不是“机密性”。
- **WAN 上的实际可见性**：默认 `crypto=on` 时，WAN 只看到 HTTP/1.1 chunked
  的 1079 字节块；chunk 体是密文，熵高但长度恒定。若 `crypto=off`，chunk 体
  就是明文 VLESS+mux，DPI 可以直接匹配 UUID 与 h2 preface。**因此基线必须
  `crypto=on`，并且文档不能把它宣传成加密。**
- **要真机密性**：在 mihomo 的 VLESS 上开 TLS 或 REALITY（纯配置：
  `tls: true` + 证书/`reality-opts`）。代价是握手、证书分发、以及外层
  HTTP 容器里再套 TLS 记录的指纹变化。那是另一个设计，不在本文范围。

### 8.2 外层 HTTP 指纹

tbtun 的 http-kal 容器是固定的：`POST /api/v2/upload HTTP/1.1`、Chrome UA、
`Content-Type: application/gzip`、`Transfer-Encoding: chunked`、
`Connection: keep-alive`，每条 carrier 一个随机的 26 字符 `X-Request-Id`。
256 条同时在线、每个完整 chunk 1079 字节（chunk 体 1072）、5 s keepalive
PAD 38 字节。这些是
**稳定特征**：目标明确的一方可以按“同一 UA + 同一路径 + 256 条长连接 +
恒定 chunk 大小”识别。设计上不假装它是“普通浏览器流量”；它只是“看起来像
HTTP/1.1 keep-alive 上传”。若要进一步弱化，需要改 tbtun 的容器（不在本次
范围）。

### 8.3 单流与重连

见 §4.3/§4.4。最重要的两个风险：

1. **半开窗口**：h2mux 30 s 内继续复用坏连接，用户看到的是超时，而不是
   快速失败。30 s 后才重建。
2. **重建竞态**：新 TCP 到达 tbtun 时，旧 flow 可能还没 detach；内核直接
   `fec_stream_refused` + reset。如果旧 flow 因为半开永远收不到 FIN，新连接
   会被持续拒绝。需要 S6b 实测窗口；若窗口不可接受，需要在 tbtun 侧加
   “同源替换/旧流超时回收”，那是代码工作，不在本设计内。

### 8.4 XUDP 的静默偏离

`only-tcp: false` 时，`packet-encoding: xudp` 写在配置里但完全不生效。
这不是报错，也不会在日志里提示。运维会以为“XUDP 开着”，实际走的是
sing-mux 的 packet-addr 流。文档、监控、验收都必须以 `only-tcp` 的值为准，
不能以 `packet-encoding` 的存在为准。

### 8.5 mihomo 版本漂移

本文的行号绑定 `0455d0d2` / 1.10.0。上游随时会改字段默认值（例如
`NewVless` 的 XUDP 默认、`sing-mux` 的 `minStreams` 默认）。升级必须跑
`verify-versions.sh` 并重跑 S1–S3；不要把本文当永久契约。

### 8.6 生产 near 的隔离

新增 mihomo 用 7899，不改 7890；新增 tbtun 用 19080/19000，不碰既有端口。
carrier 的 `ip rule pref 100` 只加到 far 地址，必须先 `ip route get` 确认走
物理网卡。任何 reload/restart 生产 mihomo 的动作都不在本文范围。

---

## 9. 需要实验回答的问题

以下问题读代码不能确定，必须真机/实验：

1. **半开重连窗口**：S6b 里 h2mux 30 s 超时后，新 TCP 是否立刻被 admit？
   旧 flow 的 detach 延迟是多少？`fec_stream_refused` 会增长几次？
2. **`only-tcp: false` 的 UDP 质量**：sing-mux 的 packet-addr 流对 DNS、
   QUIC、游戏 UDP 的实际表现（队头阻塞、丢包恢复）如何？是否满足“跳板访问
   任意网站”的最低要求？
3. **h2mux 并发上限**：默认 `MaxConcurrentStreams` 下，多少条并发浏览器
   连接会触发 `CanTakeNewRequest=false` 并新开 TCP？新开的那条会不会撞
   `fec_stream_refused`？
4. **256 carrier 的建立时间**：near 启动到 `up=256` 需要多久？mihomo 的
   首次连接是否会在载体未就绪时超时？是否需要 near 的 listen 先于 mihomo
   启动？
5. **小请求时延**：S1 的 `time_total` 减去直连 RTT，是否落在预期的
   50–60 ms（用户态）或 5–50 ms（内核）？25 ms block idle 是否被重复触发？
6. **XUDP 抓包**：S5 里客户端是否真的发出 `CommandMux`（首字节 0x03 的
   VLESS command）？服务端是否在第二条 TCP 上回 RST？需要 tcpdump 证据。
7. **mihomo 服务端的 rules/DNS**：far 的 `MATCH,DIRECT` 对域名目标是否
   走本机 DNS？DNS 查询本身是否会被 mihomo 再代理回 near？冒烟用 IP，生产
   必须验证。
8. **生产 WAN 的 carrier 旁路**：`ip rule pref 100` 与生产 mihomo TUN 的
   共存是否稳定？是否需要 `srcaddrs` 绑定？这是部署前的一次性验证。

---

## 10. 附：最小验收清单

- [ ] far 的 mihomo 只监听 `127.0.0.1:19100`，`allow-insecure: true`。
- [ ] near 的 mihomo 出站 `network: tcp`、`tls: false`、`smux.enabled: true`、
      `max-connections: 1`、`only-tcp` 明确写死。
- [ ] `19080` 只有 1 条 established TCP（S1/S3）。
- [ ] 8 条并发请求全部成功且仍只有 1 条 TCP（S3）。
- [ ] 100 MiB 下载 SHA-256 一致（S2）。
- [ ] `fec_stream_refused` 在 S1–S4 保持 0。
- [ ] S5 明确记录“XUDP 触发第二条流被拒”的证据。
- [ ] S6b 记录半开窗口与重连结果。
- [ ] S7 抓到的是 HTTP/1.1 chunked，不是 VLESS 明文。
- [ ] 生产 near 的 7890 未被重启/未改配置。
- [ ] mihomo 源码目录 `git diff --quiet` 为空（未打补丁）。
