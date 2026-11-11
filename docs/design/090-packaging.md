# 0.9.0 打包与两端配置：纯旁路集成

状态：设计稿，基于 0.8 分支，未执行。
基线：`origin/w-080-credit-int` @ `5b730dc02e8483eacdeb9aa888e29dcd42acf352`
（工作树 `/home/kunweiz/tbtun-worktrees/w-090-design`，分支 `w-090-design`）。
mihomo 基线：`v1.19.34`，`0455d0d28c4526937380505153e65a843d8612e8`，源码取自
`https://proxy.golang.org/github.com/metacubex/mihomo/@v/v1.19.34.zip`。

> 版本提醒：master `66ecd19`（0.7.3）只有内核模块。0.8 分支才引入用户态客户端：
> `host/tbt_client_main.c`、`host/tbt_client_engine.c`、`host/tbt_client_engine.h`，
> 构建目标在 `Makefile:865-866`。本设计以 0.8 为准。
> 0.8 的 `README.md:10-19` 仍写着“no userspace process”，那是 0.7 的残留文本，
> 不要拿它当 0.8 的行为依据。

## 0. 部署形态

| 侧 | 数据面 | 产物 | 是否需要 root 装载模块 |
| --- | --- | --- | --- |
| near | 用户态 `tbt_client`：监听应用、拨 256 条载体 | `host/tbt_client` 可执行文件 | 否 |
| far | 内核模块 `tbtun.ko`：接受载体、按流拨 `target=` | `tbtun.ko` | 是（`insmod`） |

证据：

- `host/tbt_client_engine.h:1-20`：引擎是“一端”：near 接受应用 TCP 并拨载体池，
  far 接受载体池并连 `target`；它链接模块同一批纯 C 编码器，所以线格式一致。
- `host/tbt_client_main.c:1-12`：`tbt_client role=near listen=... peer=...
  target=... n=... m=... mode=... carriers=...`，与
  `/proc/net/tbtun/config` 同一套行语法；`role` 默认 `near`。
- `Makefile:206-235`：`HOST_CLIENT := host/tbt_client`；
  `Makefile:865-866` 是它的链接规则。

因此 near 侧**不装 `tbtun.ko`**；far 侧**不装 `tbt_client`**。安装脚本必须分开。

## 1. 集成方式：不写插件，只写配置

mihomo 已经原生提供本产品需要的两端：

| 需要的能力 | 源码事实 |
| --- | --- |
| near VLESS 出站 | `adapter/parser.go:65-71` 识别 `type: vless` 并调用 `outbound.NewVless`；字段在 `adapter/outbound/vless.go:59-72` |
| far VLESS 入站 | `listener/parse.go:96-102` 识别 `type: vless`；实现在 `listener/sing_vless/` |
| TCP 多路复用（smux） | `adapter/parser.go:234-241` 读取顶层 `smux:`，用 `outbound.NewSingMux` 包住任意出站 |
| XUDP | `adapter/outbound/vless.go:69,387-396,458`；服务端 `listener/sing_vless/service.go:141-142` |
| 无 TLS 明文 | 出站 `tls` 默认 false（`adapter/outbound/vless.go:65`）；入站 `allow-insecure: true` 允许无 TLS 监听（`listener/sing_vless/server.go:265-277`） |

`tbt_client` 与 `tbtun.ko` 对 mihomo 都只是透明 TCP 字节管道：mihomo 不感知载体、
FEC 或分片。VLESS/smux/XUDP 的语义完全由两端 mihomo 解释。

---

## 2. 自定义编译目标

用独立 Makefile（`deploy/090/Makefile`），不改根 `Makefile`，不改上游。一次
`package` 产出两棵互不混装的目录：

```
dist/090/near/   mihomo  tbt_client  config.yaml  tbtun.conf  install-near.sh
dist/090/far/    mihomo  tbtun.ko    config.yaml  tbtun.conf  install-far.sh
```

受“只新增一个设计文件”的约束，下面四个文件的内容以代码块给出；落地时原样保存为
`deploy/090/Makefile`、`deploy/090/near.yaml.in`、`deploy/090/far.yaml.in`、
`deploy/090/install-near.sh`、`deploy/090/install-far.sh`。

### 2.1 `deploy/090/Makefile`

```make
# deploy/090/Makefile
# 用法：
#   make -f deploy/090/Makefile package
#   make -f deploy/090/Makefile package MIHOMO_VERSION=v1.19.34 UUID=....
# 升级 mihomo 只改 MIHOMO_VERSION；缺省取上游 latest。

SHELL        := /bin/sh
REPO_ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
DIST         ?= $(REPO_ROOT)/dist/090
KERNEL_DIR   ?= /lib/modules/$(shell uname -r)/build
UNAME_M      := $(shell uname -m)
GOARCH       ?= $(if $(filter x86_64,$(UNAME_M)),amd64,$(if $(filter aarch64,$(UNAME_M)),arm64,$(UNAME_M)))

MIHOMO_VERSION ?= $(shell curl -fsSL https://api.github.com/repos/MetaCubeX/mihomo/releases/latest \
                    | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)

UUID         ?= $(shell cat /proc/sys/kernel/random/uuid)
NEAR_PORT    ?= 19000
FAR_PORT     ?= 19002
VLESS_PORT   ?= 19001
MIXED_PORT   ?= 7890
FAR_IP       ?= 203.0.113.10
NEAR_IP      ?= 198.51.100.20

ifeq ($(GOARCH),amd64)
MIHOMO_ASSET := mihomo-linux-amd64-compatible-$(MIHOMO_VERSION).gz
else ifeq ($(GOARCH),arm64)
MIHOMO_ASSET := mihomo-linux-arm64-$(MIHOMO_VERSION).gz
else
$(error unsupported GOARCH=$(GOARCH))
endif
MIHOMO_URL := https://github.com/MetaCubeX/mihomo/releases/download/$(MIHOMO_VERSION)/$(MIHOMO_ASSET)

.PHONY: all package package-near package-far mihomo client module configs verify clean
all: package

$(DIST)/mihomo:
	@mkdir -p $(DIST)
	@echo "fetch $(MIHOMO_URL)"
	curl -fL --retry 3 -o $(DIST)/$(MIHOMO_ASSET) $(MIHOMO_URL)
	gzip -dc $(DIST)/$(MIHOMO_ASSET) > $@.tmp
	chmod 0755 $@.tmp
	mv $@.tmp $@
	$@ -v

mihomo: $(DIST)/mihomo

# 0.8 userspace client; Makefile:865-866 owns the link rule.
$(DIST)/near/tbt_client: FORCE
	@mkdir -p $(dir $@)
	$(MAKE) -C $(REPO_ROOT) host/tbt_client
	install -m 0755 $(REPO_ROOT)/host/tbt_client $@

client: $(DIST)/near/tbt_client

# Kernel module only exists on the far side.
$(DIST)/far/tbtun.ko: FORCE
	@mkdir -p $(dir $@)
	$(MAKE) -C $(KERNEL_DIR) M=$(REPO_ROOT) modules
	install -m 0644 $(REPO_ROOT)/tbtun.ko $@
	modinfo $@ | sed -n '1,8p'

module: $(DIST)/far/tbtun.ko

$(DIST)/near/config.yaml: $(REPO_ROOT)/deploy/090/near.yaml.in
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(NEAR_PORT)/g' \
	    -e 's/@MIXED_PORT@/$(MIXED_PORT)/g' $< > $@

$(DIST)/far/config.yaml: $(REPO_ROOT)/deploy/090/far.yaml.in
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(VLESS_PORT)/g' $< > $@

configs: $(DIST)/near/config.yaml $(DIST)/far/config.yaml

package-near: mihomo client $(DIST)/near/config.yaml
	install -m 0755 $(DIST)/mihomo $(DIST)/near/mihomo
	printf '%s\n' \
	  "tunnel 1 role=near listen=127.0.0.1:$(NEAR_PORT) peer=$(FAR_IP):$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=4 m=4 fec=on mode=grouped carriers=256" \
	  > $(DIST)/near/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-near.sh $(DIST)/near/install.sh

package-far: mihomo module $(DIST)/far/config.yaml
	install -m 0755 $(DIST)/mihomo $(DIST)/far/mihomo
	printf '%s\n' \
	  "tunnel 1 role=far listen=0.0.0.0:$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=4 m=4 fec=on mode=grouped carriers=256 peer=$(NEAR_IP)" \
	  > $(DIST)/far/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-far.sh $(DIST)/far/install.sh

package: package-near package-far
	{ $(DIST)/mihomo -v; \
	  sha256sum $(DIST)/near/mihomo $(DIST)/near/tbt_client \
	            $(DIST)/far/mihomo $(DIST)/far/tbtun.ko; } > $(DIST)/build-info.txt
	cd $(DIST) && sha256sum near/* far/* > SHA256SUMS

verify:
	$(DIST)/near/mihomo -t -f $(DIST)/near/config.yaml
	$(DIST)/far/mihomo  -t -f $(DIST)/far/config.yaml
	test -x $(DIST)/near/tbt_client
	modinfo $(DIST)/far/tbtun.ko >/dev/null

clean:
	rm -rf $(DIST)

FORCE:
```

要点：

- 客户端构建直接复用 0.8 的 `host/tbt_client` 目标（`Makefile:865-866`），不新增
  第二套编译逻辑。
- 模块构建复用根 `Makefile` 的 `all` 规则（0.8 的 `Makefile:306-307`：
  `$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules`）。
- mihomo 使用上游 Release 现成二进制，不锁定版本、不打补丁。资产名已在
  v1.19.34 的 Release API 中核实：`mihomo-linux-amd64-compatible-v1.19.34.gz`、
  `mihomo-linux-arm64-v1.19.34.gz`。
- `mihomo -v` 与 `-t` 是公开 CLI（`main.go:75-76`），不依赖内部符号。

### 2.2 near 模板 `deploy/090/near.yaml.in`

```yaml
mixed-port: @MIXED_PORT@
external-controller: 127.0.0.1:9090
mode: rule
log-level: info

proxies:
  - name: tbtun
    type: vless
    server: 127.0.0.1
    port: @PORT@
    uuid: @UUID@
    network: tcp
    tls: false
    udp: true
    xudp: true
    smux:
      enabled: true
      protocol: h2mux
      only-tcp: true
      padding: false

proxy-groups:
  - name: PROXY
    type: select
    proxies: [tbtun, DIRECT]

rules:
  - MATCH,PROXY
```

### 2.3 far 模板 `deploy/090/far.yaml.in`

```yaml
mixed-port: 0
mode: rule
log-level: info

listeners:
  - name: vless-in
    type: vless
    listen: 127.0.0.1
    port: @PORT@
    users:
      - username: tbtun
        uuid: @UUID@
    # 名字有误导性：这里不是“跳过证书校验”，而是“允许无 TLS 监听”。
    # server.go:265-277 只在所有 TLS/安全层都为空时才走到这个分支。
    allow-insecure: true

rules:
  - MATCH,DIRECT
```

### 2.4 两个安装脚本

`deploy/090/install-near.sh`（只装客户端，不装模块）：

```sh
#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install -D -m 0755 "$here/mihomo"     /usr/local/bin/mihomo
install -D -m 0755 "$here/tbt_client" /usr/local/bin/tbt_client
install -D -m 0644 "$here/config.yaml" /etc/mihomo-090/config.yaml
install -D -m 0644 "$here/tbtun.conf"  /etc/tbtun-090.conf
echo "next: tbt_client --config /etc/tbtun-090.conf"
echo "      mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml"
```

`deploy/090/install-far.sh`（只装模块，不装客户端）：

```sh
#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install -D -m 0755 "$here/mihomo"   /usr/local/bin/mihomo
install -D -m 0644 "$here/tbtun.ko" "/lib/modules/$(uname -r)/extra/tbtun.ko"
depmod -a
install -D -m 0644 "$here/config.yaml" /etc/mihomo-090/config.yaml
install -D -m 0644 "$here/tbtun.conf"  /etc/tbtun-090.conf
echo "next: insmod /lib/modules/$(uname -r)/extra/tbtun.ko"
echo "      cat /etc/tbtun-090.conf > /proc/net/tbtun/config"
echo "      mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml"
```

### 2.5 `go install` 备选（不作为默认路径）

```sh
GOBIN=$PWD/dist/090 GOOS=linux GOARCH=amd64 \
  go install github.com/metacubex/mihomo@v1.19.34
```

**不确定**：`go.mod:151` 有一条 protobuf 的 `replace`。`go install pkg@version`
是否应用目标模块自己的 `replace`，本机没有 Go 工具链，未验证。若它被忽略而该替换
又是编译必需，构建会失败。因此默认走 Release 二进制；`go install` 只作为实验项。
若使用它，Go 版本应取上游 CI 口径：`.github/workflows/build.yml:167` 是 `1.26`，
`build.yml:202` 是 `CGO_ENABLED=0`。

---

## 3. 两端配置：逐字段核实

### 3.1 near VLESS 出站

| 键 | 值 | 源码证据 | 说明 |
| --- | --- | --- | --- |
| `proxies` | 列表 | `config/config.go:454` | 顶层键 |
| `type` | `vless` | `adapter/parser.go:65-71` | 未知类型报 `unsupport proxy type`（`adapter/parser.go:226-227`） |
| `name` | `tbtun` | `adapter/outbound/vless.go:60` | |
| `server` | `127.0.0.1` | `adapter/outbound/vless.go:61` | 指向 near 客户端监听 |
| `port` | `19000` | `adapter/outbound/vless.go:62` | |
| `uuid` | 两端一致 | `adapter/outbound/vless.go:63` | |
| `network` | `tcp` | `adapter/outbound/vless.go:72` | 只选外层传输；空值等价 tcp（`vless.go:258-261`） |
| `tls` | `false` | `adapter/outbound/vless.go:65` | false 时 `streamTLSConn` 原样返回（`vless.go:336-337`） |
| `udp` | `true` | `adapter/outbound/vless.go:67` | 不置 true 则不声明 UDP |
| `xudp` | `true` | `adapter/outbound/vless.go:69` | 显式写，不依赖默认值 |
| `smux.enabled` | `true` | `adapter/parser.go:234-241` | 通用解析器只读 `smux` |
| `smux.protocol` | `h2mux` | `adapter/outbound/singmux.go:25` | 可选 `smux`/`yamux`/`h2mux`（`sing-mux/client.go:57-71`） |
| `smux.only-tcp` | `true` | `adapter/outbound/singmux.go:31` | true 时 UDP 绕过 smux，走 VLESS XUDP（`singmux.go:49-57,66-70`） |
| `smux.padding` | `false` | `adapter/outbound/singmux.go:29` | 见 5.2 |

`smux` 是出站构造完成后的通用包装，所以对 VLESS 同样生效：`adapter/parser.go:233-241`。

### 3.2 far VLESS 入站

| 键 | 值 | 源码证据 | 说明 |
| --- | --- | --- | --- |
| `listeners` | 列表 | `config/config.go:458` | 顶层键 |
| `type` | `vless` | `listener/parse.go:96-102` | |
| `name` | `vless-in` | `listener/inbound/base.go:93` | 无 `omitempty`，必填 |
| `listen` | `127.0.0.1` | `listener/inbound/base.go:94` | 只写 IP；`NewBase` 用 `netip.ParseAddr`（`base.go:24-30`），写 `ip:port` 会失败 |
| `port` | `19001` | `listener/inbound/base.go:95` | 端口单独给 |
| `users[].username` | 任意 | `listener/inbound/vless.go:33` | |
| `users[].uuid` | 与 near 一致 | `listener/inbound/vless.go:34` | 无 `omitempty`，必填 |
| `users[].flow` | 省略 | `listener/inbound/vless.go:35` | 不用 Vision |
| `allow-insecure` | `true` | `listener/inbound/vless.go:24` | 允许无 TLS 监听；语义见 3.3 |

### 3.3 `allow-insecure` 的真实语义（复查结论）

这个字段名听起来像客户端的“跳过证书校验”，但在 VLESS 入站里不是。控制流是：

```go
if shadowTLSBuilder != nil { ... }
else if restlsBuilder != nil { ... }
else if jlsBuilder != nil { ... }
else if realityBuilder != nil { ... }
else if tlsConfig.GetCertificate != nil { l = tls.NewListener(l, tlsConfig) }
else if sl.decryption == nil && !config.AllowInsecure {
    return nil, errors.New("disallow using Vless without any certificates/.../allow-insecure config")
}
```

见 `listener/sing_vless/server.go:265-277`。也就是说：只有当 certificate、
shadow-tls、res-tls、jls、reality、decryption **全部为空**时，才会检查
`AllowInsecure`；它为 true 就保留裸 listener，不套 `tls.NewListener`。所以：

- 它不是“跳过证书验证”；客户端侧的 `skip-cert-verify` 才是那个意思。
- 对于本设计的无 TLS 入站，它**是必需的**，不是可选装饰；设为 false 会在启动时
  直接报错。
- 模板里保留它，并在注释中写明“允许无 TLS 监听”，避免后来的人按名字误解。

入站不需要任何 mux 开关。smux 服务端由特殊目标域名触发，始终存在
（`listener/sing/sing.go:96-111`）；XUDP 的 `CommandMux` 分支也无条件处理
（`listener/sing_vless/service.go:141-142`）。

---

## 4. 连接拓扑、端口与启动顺序

```
 应用
  │  127.0.0.1:7890 (near mihomo mixed-port)
  ▼
 near mihomo ── VLESS/TCP ──► 127.0.0.1:19000 (tbt_client, role=near)
                                           │  256 carriers + FEC
                                           ▼
                                    FAR_IP:19002
                                           │
 far tbtun.ko (role=far) ◄─────────────────┘
  │  dial target=127.0.0.1:19001
  ▼
 far mihomo VLESS 入站 ── rules: MATCH,DIRECT ──► 任意站点
```

| 位置 | 端口 | 谁监听 | 谁连接 | 依据 |
| --- | --- | --- | --- | --- |
| near | `127.0.0.1:19000` | `tbt_client` | near mihomo 出站 | `tbt_client_main.c:6-8`，`README.md:379` |
| near | `127.0.0.1:7890` | near mihomo | 本机应用 | `config/config.go:409` |
| near | `127.0.0.1:9090` | near mihomo API | 运维核对 | 上游 config 键 |
| far | `0.0.0.0:19002` | `tbtun.ko` | near 客户端的载体 | `README.md:379-381` |
| far | `127.0.0.1:19001` | far mihomo VLESS | far 模块的 `target=` | 本文 3.2 |

两行隧道配置（由 Makefile 生成）：

```text
# near (tbt_client --config)
tunnel 1 role=near listen=127.0.0.1:19000 peer=FAR_IP:19002 target=127.0.0.1:19001 n=4 m=4 fec=on mode=grouped carriers=256

# far (写入 /proc/net/tbtun/config)
tunnel 1 role=far listen=0.0.0.0:19002 target=127.0.0.1:19001 n=4 m=4 fec=on mode=grouped carriers=256 peer=NEAR_IP
```

- near 的 `target=` 只是 OPEN 的槽 0 默认端口；far 永远拨自己配置的 `target_addr`
  （`README.md:381`）。因此 near 写 `127.0.0.1:19001` 只作为占位，真正的目的地址
  由 far 的 `target=` 决定。
- far 的 `peer=` 是允许加入的 near 源地址锚点（`README.md:380`），必须是 near 的
  公网出口 IP；地址只接受字面量，不做 DNS（`README.md:598-602`）。

启动顺序：

1. **far mihomo** 启动，确认 `127.0.0.1:19001` 已监听；日志应出现
   `Vless[vless-in] proxy listening at: 127.0.0.1:19001`
   （`listener/inbound/vless.go:161`）。
2. **far** `insmod tbtun.ko`，写 `/proc/net/tbtun/config`；开始监听 `19002`。
3. **near** 启动 `tbt_client --config /etc/tbtun-090.conf`；开始监听 `19000`
   并向 far 拨载体。
4. **near mihomo** 启动；它的 VLESS 出站只在有应用连接时才拨 `127.0.0.1:19000`。
5. 应用把代理指向 `127.0.0.1:7890`。

硬依赖只有两条：near 的 `19000` 必须先于 near mihomo 的第一次出站；far 的
`19001` 必须先于 far 模块的第一次 `target=` 拨号。后者失败只影响当条流。

**部署门**：near 的载体拨号不能被本机 mihomo 的 TUN `auto-route` 抢走。仓库既有
做法是主机策略路由 `ip rule add pref 100 to FAR_IP/32 lookup main`
（`benches/short.sh:42-43,121-123`；`review/ops-0.4-backup-near-760k.md:86-108`）。

**安全边界**：`tls: false` 是设计选择。VLESS 的 UUID 只做身份校验，不做加密；
smux/XUDP 也不加密。字节在 tbtun 载体里是明文，任何能看到载体流的人都能读出
VLESS 请求与载荷。far 的 `listen: 127.0.0.1` 只把入站入口限制在本机，不改变这一点。

---

## 5. 配置陷阱（必须显式警告）

### 5.1 smux 与 XUDP 相互独立，`only-tcp` 决定 UDP 走哪条

- 客户端 `smux.enabled: true` 且 `only-tcp: false` 时，`SingMux.ListenPacketContext`
  直接把 UDP 交给 smux 的 `client.ListenPacket`（`adapter/outbound/singmux.go:49-57`），
  VLESS 自己的 XUDP 根本不会走。
- `only-tcp: true` 时，UDP 回落到 `ProxyAdapter.ListenPacketContext`
  （`singmux.go:49-52`），VLESS 才用 `XUDP` 构造 `CommandMux` 并把流包进
  `NewXUDPConn`（`vless.go:387-396,458`）。
- 结论：要“mux + tcp + XUDP”三者同时成立，必须 `only-tcp: true`。只写
  `xudp: true` 而 `only-tcp: false`，UDP 实际走的是 smux，不是 XUDP。

### 5.2 服务端 padding 会拒绝非填充客户端

服务端 `mux-option.padding: true` 时，收到 `request.Padding == false` 的连接直接
返回 `non-padded connection rejected`（`sing-mux/server.go:56-60`）。反之，客户端
padding=true、服务端 false 可以工作。两端模板都保持 `padding: false`；far 模板
不写 `mux-option`，即服务端 false。

### 5.3 smux 与 XUDP 是两种线格式

- smux 客户端拨固定目标 `sp.mux.sing-box.arpa:444`（`sing-mux/protocol.go:27-30`），
  服务端在 `listener/sing/sing.go:96-111` 认出该 FQDN，交给 `mux.NewService`；
  协议号在 mux 请求头里（`sing-mux/server.go:52,61`）。
- XUDP 走 VLESS 的 `CommandMux`（`transport/vless/conn.go:101-102`），服务端进入
  `sing_vmess.HandleMuxConnection`（`listener/sing_vless/service.go:141-142`），
  帧格式是 16 位长度 + sessionID + status + option
  （`sing-vmess@v0.2.5/mux.go:77-90`）。
- 两者都叫“mux”，但字节完全不同；不要把 `smux` 的配置当成 XUDP 的开关。

### 5.4 裸 `mux:` 不会被读取，`packet-encoding` 会静默覆盖 `xudp`

- 通用解析器只查 `mapping["smux"]`（`adapter/parser.go:234`）。写成 `mux:` 不报错，
  也不会启用多路复用。
- `packet-encoding: packetaddr|packet` 会把 `PacketAddr=true`、`XUDP=false`
  （`vless.go:474-477`）；`packet-addr: true` 也会让 XUDP 关闭
  （`vless.go:483-485`）。这些覆盖是静默的。模板只用 `xudp: true`，不同时写另外
  两个键。
- `xudp` 在代码里受支持（`vless.go:69`），但上游 `docs/config.yaml` 全文没有这个
  键；它是“代码支持、文档未收录”，升级时要按第 6.3 节复验。

### 5.5 两端必须同一 mihomo 版本

smux 的请求头与帧由 `sing-mux` 实现（`sing-mux/protocol.go:22-70`、
`server.go:51-64`），XUDP 的帧由 `sing-vmess` 实现
（`sing-vmess@v0.2.5/mux.go:77-90`）。源码里没有版本协商字段。跨版本混用没有
可引用的兼容承诺；升级时两端一起换。

---

## 6. 版本兼容策略

### 6.1 只依赖配置文件

所有依赖都在 YAML 与隧道行里，不链接 mihomo 包、不读内部符号、不打补丁。升级：

```sh
make -f deploy/090/Makefile package MIHOMO_VERSION=vX.Y.Z
```

### 6.2 键的稳定性分级

| 键 | 级别 | 依据 |
| --- | --- | --- |
| `proxies` / `proxy-groups` / `rules` | 稳定（Clash 传统键） | `config/config.go:454-456` |
| `listeners` | 稳定（多入站唯一入口） | `config/config.go:458`；`listener/parse.go:12` |
| `type: vless`（出/入） | 稳定 | `adapter/parser.go:65`；`listener/parse.go:96` |
| `uuid` / `server` / `port` | 稳定 | `vless.go:61-63`；`inbound/vless.go:33-35` |
| `network` / `tls` / `udp` | 稳定 | `vless.go:65-72` |
| `smux.enabled` / `protocol` / `only-tcp` / `padding` | 较稳定，但**静默失败** | 见 5.4 |
| `xudp` | 半稳定：代码支持、上游文档未收录 | `vless.go:69`；`docs/config.yaml` 无此键 |
| `allow-insecure`（入站） | 稳定，且明文必需 | `inbound/vless.go:24`；`server.go:265-277` |
| `packet-addr` / `packet-encoding` | 不使用 | 与 `xudp` 有静默覆盖（`vless.go:474-485`） |

**不确定**：源码包没有 `.git`，无法证明某个键在哪一版改过名。上表只列当前版本
可验证的事实，不把记忆中的旧文档当证据。

### 6.3 升级门禁（可执行清单）

每次换 `MIHOMO_VERSION` 后按顺序做：

1. `mihomo -v`：两端版本一致（`main.go:75,116-119`）。
2. `mihomo -t -f near/config.yaml`、`far/config.yaml`：必须打印
   `test is successful`（`main.go:175-190`）。
3. 启动 far，日志出现 `Vless[...] proxy listening at: 127.0.0.1:19001`
   （`listener/inbound/vless.go:161`）。
4. 启动 near，`curl 127.0.0.1:9090/proxies/tbtun`，确认返回里 `smux` 为 true。
   该字段由 `adapter/adapter.go:155` 写入 `ProxyInfo.SMUX`，经
   `hub/route/proxies.go:62-67` 暴露。**没有 `smux` 就停下查 Release Notes**。
5. 端到端两条：TCP（`curl https://example.com` 经 7890）与 UDP
   （`dig @1.1.1.1 example.com` 经 7890）。TCP 通而 UDP 不通，优先怀疑 `xudp`
   或 `only-tcp` 的语义变化。
6. Release 资产 404 时，查
   `https://api.github.com/repos/MetaCubeX/mihomo/releases/latest` 的资产名。

---

## 7. 当前缺口：打包可以先做，端到端验收会被阻断

这套设计依赖 0.8 分支的现状。0.8 还有三项未关闭的缺陷；在它们关闭前，
`package` 与 `verify` 可以完成，但“真实链路可用”不能签字：

1. **吞吐只有约 0.55 MB/s。** 0.8 的验收数字；分支内可佐证的同级数据是
   `CHANGELOG.md:308-310`：300 ms WAN 下 payload 从 3.1 MB/s 掉到 0.65–1.0 MB/s，
   修复流量从 47 MB 涨到 270 MB/2 min。这个量级不足以承载 0.9.0 的“任意站点”目标。
2. **半开流永不回收。** `review/analysis/multiconn.md:26-27` 记录：流对象与
   `far_sid` 在会话销毁前不回收；`DESIGN.md:939-944` 的 TCP keepalive 只是把
   carrier socket 关闭，没有证明流对象会随之释放。0.8 的验收仍把它列为未解决。
3. **载体 stall-dead 抖动未定。** `CHANGELOG.md:336-341` 记录：256 载体下
   stall/recover 每小时触发数千次，99% 在 200 ms 内恢复，且这些日志曾是内核冻结前
   的最后输出。恢复时间短不等于无害；在 0.9.0 的 256 载体形状下必须先定性。

因此交付顺序应为：

- 现在可做：Makefile、两棵产物树、两个安装脚本、两份配置、`mihomo -t` 门禁。
- 阻断项：上述三条关闭前，不做“端到端可用”的验收声明；不以打包完成代替链路可用。

---

## 8. 无法从源码确定的事项，以及能定论的实验

1. **`go install pkg@version` 是否应用目标模块的 `replace`。**
   `go.mod:151` 有 protobuf 的 replace。实验：Go 1.26 下
   `GOBIN=/tmp/gobin go install github.com/metacubex/mihomo@v1.19.34`，看是否成功、
   `go version -m /tmp/gobin/mihomo` 里 protobuf 的来源。
2. **Release 资产命名是否长期稳定。** 只在 v1.19.34 核实过；每次升级先查 API。
3. **smux/XUDP 的跨版本线兼容。** 无版本协商字段。实验：near vX、far vY 各跑
   TCP+UDP；失败就回退同版本。
4. **`xudp` 的文档地位。** 代码有、`docs/config.yaml` 没有。实验：在 7890 上发
   真实 UDP 查询并确认走 `CommandMux`；升级后按 6.3 第 5 步复验。
5. **`carriers=256` 与 `mode=grouped` 的稳定性。** 语法与上限见
   `README.md:383-385`；0.8 的 stall 抖动见第 7 节。实验：按既有 smoke 矩阵逐步
   加到 256，不要一次拉满。
6. **far 的 `listen: 127.0.0.1` 是否满足部署 netns。** 源码只证明它绑本机；若模块
   与 mihomo 不在同一 netns，必须改成模块可达的地址。实验：目标 netns 里
   `ss -lntp` 确认 `19001` 的绑定地址。

## 9. 一句话结论

0.9.0 不需要“mihomo 插件”：near 一个原生 VLESS 出站（`smux.only-tcp` + `xudp`），
far 一个原生 VLESS 入站（`allow-insecure: true` 允许无 TLS），中间 near 跑
`tbt_client`、far 跑 `tbtun.ko`。打包只做三件事：取上游 mihomo 二进制、构建
`host/tbt_client` 与 `tbtun.ko`、渲染两份 YAML 与两行隧道配置。升级的唯一开关是
`MIHOMO_VERSION`，两端必须同版本；在上述三项 0.8 缺陷关闭前，只能验收装配，
不能验收端到端可用。
