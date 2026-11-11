# 0.9.0 打包与两端配置：纯旁路集成

状态：打包与配置已实现（`deploy/090/`），真机端到端验收未执行。
基线：`origin/w-090-design` @ `46fbb60`（工作树
`/home/kunweiz/tbtun-worktrees/w-090-design`，分支 `w-090-design`）。
mihomo 基线：`v1.19.34`，`0455d0d28c4526937380505153e65a843d8612e8`，源码取自
`https://proxy.golang.org/github.com/metacubex/mihomo/@v/v1.19.34.zip`。

> 版本提醒：master `66ecd19`（0.7.3）只有内核模块。0.8 分支才引入用户态客户端：
> `src/client/tbt_client_main.c`、`src/client/tbt_client_engine.c`、`src/client/tbt_client_engine.h`，
> 构建目标在 `Makefile:865-866`。本设计以 0.8 为准。
> 0.8 的 `README.md:10-19` 仍写着“no userspace process”，那是 0.7 的残留文本，
> 不要拿它当 0.8 的行为依据。

## 0. 部署形态

| 侧 | 数据面 | 产物 | 是否需要 root 装载模块 |
| --- | --- | --- | --- |
| near | 用户态 `tbt_client`：监听应用、拨 256 条载体 | `src/client/tbt_client` 可执行文件 | 否 |
| far | 内核模块 `tbtun.ko`：接受载体、按流拨 `target=` | `tbtun.ko` | 是（`insmod`） |

证据：

- `src/client/tbt_client_engine.h:1-20`：引擎是“一端”：near 接受应用 TCP 并拨载体池，
  far 接受载体池并连 `target`；它链接模块同一批纯 C 编码器，所以线格式一致。
- `src/client/tbt_client_main.c:1-12`：`tbt_client role=near listen=... peer=...
  target=... n=... m=... mode=... carriers=...`，与
  `/proc/net/tbtun/config` 同一套行语法；`role` 默认 `near`。
- `Makefile:206-235`：`HOST_CLIENT := src/client/tbt_client`；
  `Makefile:865-866` 是它的链接规则。

因此 near 侧**不装 `tbtun.ko`**；far 侧**不装 `tbt_client`**。安装脚本必须分开。

## 1. 集成方式：不写插件，只写配置

mihomo 已经原生提供本产品需要的两端：

| 需要的能力 | 源码事实 |
| --- | --- |
| near VLESS 出站 | `adapter/parser.go:65-71` 识别 `type: vless` 并调用 `outbound.NewVless`；字段在 `adapter/outbound/vless.go:59-72` |
| far VLESS 入站 | `listener/parse.go:96-102` 识别 `type: vless`；实现在 `listener/sing_vless/` |
| TCP 多路复用（smux） | `adapter/parser.go:234-241` 读取顶层 `smux:`，用 `outbound.NewSingMux` 包住任意出站 |
| XUDP（可选，默认不走） | `adapter/outbound/vless.go:69,387-396,458`；服务端 `listener/sing_vless/service.go:141-142` |
| 无 TLS 明文 | 出站 `tls` 默认 false（`adapter/outbound/vless.go:65`）；入站 `allow-insecure: true` 允许无 TLS 监听（`listener/sing_vless/server.go:265-277`） |

`tbt_client` 与 `tbtun.ko` 对 mihomo 都只是透明 TCP 字节管道：mihomo 不感知载体、
FEC 或分片。VLESS/smux/XUDP 的语义完全由两端 mihomo 解释。

---

## 2. 自定义编译目标

用独立 Makefile（`deploy/090/Makefile`），不改根 `Makefile`，不改上游。一次
`package` 产出两棵互不混装的目录：

```
dist/090/near/   mihomo  tbt_client  config.yaml  tbtun.conf  install-near.sh  systemd/
dist/090/far/    mihomo  tbtun.ko    config.yaml  tbtun.conf  install-far.sh   systemd/
```

这些文件现已落在 `deploy/090/`。下面代码块是设计快照；可执行版本以目录中的
`Makefile`、`near.yaml.in`、`far.yaml.in`、`install-near.sh`、`install-far.sh`
（以及 `systemd/*.service.in`）为准。相对初版快照，当前实现已加固：两端写入
同一份 `CRYPTO_KEY`/`NONCE`、
`NEAR_IP` 默认留空、渲染 systemd 单元、`verify` 检查加密字段与两份
`tbtun.conf` 的解析结果。

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
# Honor KDIR from docs/build-env.md's env.sh when exported.
KERNEL_DIR   ?= $(if $(KDIR),$(KDIR),/lib/modules/$(shell uname -r)/build)
UNAME_M      := $(shell uname -m)
GOARCH       ?= $(if $(filter x86_64,$(UNAME_M)),amd64,$(if $(filter aarch64,$(UNAME_M)),arm64,$(UNAME_M)))

MIHOMO_VERSION ?= $(shell curl -fsSL https://api.github.com/repos/MetaCubeX/mihomo/releases/latest \
                    | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)

# `:=`：`?=` 是递归展开，每次引用都会重跑 shell。near/far 的 config.yaml 与
# 两份 tbtun.conf 各引用一次，递归定义会让两端拿到不同的 UUID 与 PSK。
UUID         := $(shell cat /proc/sys/kernel/random/uuid)
CRYPTO_KEY   := $(shell od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
NONCE        := $(shell od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
NEAR_PORT    ?= 19000
FAR_PORT     ?= 19002
VLESS_PORT   ?= 19001
MIXED_PORT   ?= 7890
FAR_IP       ?= 203.0.113.10
# 默认为空：动态 ISP / CGNAT 的 near 没有可固定的公网地址。
NEAR_IP      ?=

TBTUN_COMMIT := $(shell git -C $(REPO_ROOT) rev-parse --short HEAD 2>/dev/null || echo unknown)
SYSTEMD_UNITS_NEAR := tbt-client-090.service mihomo-090-near.service
SYSTEMD_UNITS_FAR  := mihomo-090-far.service tbtun-090-far.service
NEAR_UNITS := $(addprefix $(DIST)/near/systemd/,$(SYSTEMD_UNITS_NEAR))
FAR_UNITS  := $(addprefix $(DIST)/far/systemd/,$(SYSTEMD_UNITS_FAR))

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
	$(MAKE) -C $(REPO_ROOT) src/client/tbt_client
	install -m 0755 $(REPO_ROOT)/src/client/tbt_client $@

client: $(DIST)/near/tbt_client

# Kernel module only exists on the far side.
$(DIST)/far/tbtun.ko: FORCE
	@mkdir -p $(dir $@)
	@test -d "$(KERNEL_DIR)" || { \
	  echo "KERNEL_DIR=$(KERNEL_DIR) not found; see docs/build-env.md"; \
	  exit 1; }
	$(MAKE) -C $(REPO_ROOT) KERNEL_DIR=$(KERNEL_DIR) all
	install -m 0644 $(REPO_ROOT)/tbtun.ko $@
	modinfo $@ | sed -n '1,8p'

module: $(DIST)/far/tbtun.ko

# FORCE：UUID/CRYPTO_KEY/NONCE 每次 make 重新生成；stale 的 config.yaml 会
# 保留旧 UUID，与新 tbtun.conf 的 PSK 不匹配。
$(DIST)/near/config.yaml: $(REPO_ROOT)/deploy/090/near.yaml.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(NEAR_PORT)/g' \
	    -e 's/@MIXED_PORT@/$(MIXED_PORT)/g' $< > $@

$(DIST)/far/config.yaml: $(REPO_ROOT)/deploy/090/far.yaml.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@UUID@/$(UUID)/g' \
	    -e 's/@PORT@/$(VLESS_PORT)/g' $< > $@

configs: $(DIST)/near/config.yaml $(DIST)/far/config.yaml

$(DIST)/near/systemd/%.service: $(REPO_ROOT)/deploy/090/systemd/%.service.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@TBTUN_COMMIT@/$(TBTUN_COMMIT)/g' $< > $@

$(DIST)/far/systemd/%.service: $(REPO_ROOT)/deploy/090/systemd/%.service.in FORCE
	@mkdir -p $(dir $@)
	sed -e 's/@TBTUN_COMMIT@/$(TBTUN_COMMIT)/g' $< > $@

package-near: mihomo client $(DIST)/near/config.yaml $(NEAR_UNITS)
	install -m 0755 $(DIST)/mihomo $(DIST)/near/mihomo
	printf '%s\n' \
	  "tunnel 1 role=near listen=127.0.0.1:$(NEAR_PORT) peer=$(FAR_IP):$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=$(CRYPTO_KEY) nonce=$(NONCE) adaptive=off" \
	  > $(DIST)/near/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-near.sh $(DIST)/near/install-near.sh

package-far: mihomo module $(DIST)/far/config.yaml $(FAR_UNITS)
	install -m 0755 $(DIST)/mihomo $(DIST)/far/mihomo
	printf '%s\n' \
	  "tunnel 1 role=far listen=0.0.0.0:$(FAR_PORT) target=127.0.0.1:$(VLESS_PORT) n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=$(CRYPTO_KEY) nonce=$(NONCE) adaptive=off$(if $(strip $(NEAR_IP)), peer=$(NEAR_IP):$(NEAR_PORT))" \
	  > $(DIST)/far/tbtun.conf
	install -m 0755 $(REPO_ROOT)/deploy/090/install-far.sh $(DIST)/far/install-far.sh

package: package-near package-far
	{ echo "mihomo_version: $(MIHOMO_VERSION)"; \
	  echo "mihomo_asset: $(MIHOMO_ASSET)"; \
	  echo "tbtun_commit: $$(git -C $(REPO_ROOT) rev-parse HEAD)"; \
	  echo "built_at: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	  $(DIST)/near/mihomo -v; \
	  sha256sum $(DIST)/near/mihomo $(DIST)/near/tbt_client \
	            $(DIST)/far/mihomo $(DIST)/far/tbtun.ko; } > $(DIST)/build-info.txt
	cd $(DIST) && find near far -type f ! -name SHA256SUMS -print0 \
	  | sort -z | xargs -0 sha256sum > SHA256SUMS

verify:
	$(DIST)/near/mihomo -t -f $(DIST)/near/config.yaml
	$(DIST)/far/mihomo  -t -f $(DIST)/far/config.yaml
	grep -q 'max-connections: 1' $(DIST)/near/config.yaml
	grep -q 'only-tcp: false' $(DIST)/near/config.yaml
	grep -q 'AND,((NETWORK,UDP),(DST-PORT,443)),REJECT' $(DIST)/near/config.yaml
	test -x $(DIST)/near/tbt_client
	modinfo $(DIST)/far/tbtun.ko >/dev/null
	# 完整实现还检查：64/32 hex、两端 crypto_key/nonce 相同、NEAR_IP 为空时
	# far 无 peer=、以及两份 tbtun.conf 都能被 tbt_client 解析 2 秒。
	@for side in near far; do \
	  timeout 2 $(DIST)/near/tbt_client --config $(DIST)/$$side/tbtun.conf; \
	  rc=$$?; [ $$rc -eq 124 ] || exit 1; \
	done

clean:
	rm -rf $(DIST)

FORCE:
```

要点：

- 客户端构建直接复用 0.8 的 `src/client/tbt_client` 目标（`Makefile:865-866`），不新增
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
      max-connections: 1   # or sing-mux opens a 2nd TCP session (client.go:180-189)
      only-tcp: false      # UDP shares this smux connection; XUDP needs true
      padding: false

proxy-groups:
  - name: PROXY
    type: select
    proxies: [tbtun, DIRECT]

rules:
  # 浏览器 QUIC 不能进 smux：QUIC 自带拥塞控制，再套一层可靠有序流会叠加
  # 第二个拥塞控制器与队头阻塞。REJECT 让浏览器立刻回落 HTTP/2 over TCP。
  - AND,((NETWORK,UDP),(DST-PORT,443)),REJECT
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

`deploy/090/install-near.sh`（只装客户端，不装模块）与
`deploy/090/install-far.sh`（只装模块，不装客户端）都保留两条启动路径：

- **systemd**：若 `/run/systemd/system` 存在且有 `systemctl`，脚本把
  `dist/090/{near,far}/systemd/*.service` 安装到 `/etc/systemd/system/` 并
  `daemon-reload`，但不 `enable`。若同名 unit 已经是 active，脚本直接退出，
  不再起 pid 文件实例，避免两个 mihomo 抢同一端口。
- **pid 文件**：没有 systemd 的 Alpine/OpenWrt 或冒烟环境，脚本按原顺序
  `nohup` 启动，并把 pid 写到 `/run/tbtun-090/*.pid`。

`wait_port()` 先试 `ss -lnt`。最小镜像没有 iproute2 时，改读内核自己的表：
`/proc/net/tcp`（以及 `tcp6`）的第 2 字段是 `HEXADDR:HEXPORT`，第 4 字段
`0A` 表示 `TCP_LISTEN`，端口用 `printf '%04X'` 转十六进制后比对后缀。
两条路径都用纯 POSIX sh + awk，不依赖 `grep` 或 `seq`。

far 侧在 `modprobe tbtun` 之前执行 `modprobe tcp_bbr`。失败不终止安装，但会
打印可用拥塞控制算法并明确警告：模块会自动回落到内核默认 CC，长途链路吞吐
会下降。`tbt_car_set_bbr()` 的失败计数在 `/proc/net/tbtun/stats` 的
`car_bbr_ok` 可查。

### 2.5 systemd 单元（可选）

`deploy/090/systemd/` 有四个模板，`package` 时渲染到
`dist/090/{near,far}/systemd/`：

| 单元 | 侧 | 关键关系 |
| --- | --- | --- |
| `tbt-client-090.service` | near | 起 `tbt_client --config /etc/tbtun-090.conf` |
| `mihomo-090-near.service` | near | `After=tbt-client-090.service`、`Requires=tbt-client-090.service` |
| `mihomo-090-far.service` | far | 起 far 的 VLESS 监听 |
| `tbtun-090-far.service` | far | `After=mihomo-090-far.service`、`Requires=mihomo-090-far.service`；`oneshot` + `RemainAfterExit=yes` |

顺序的硬约束只有两条：near 的 `127.0.0.1:19000` 必须先于 mihomo 的出站；
far 的 `127.0.0.1:19001` 必须先于模块的 `target=` 拨号。`Requires=` 同时把
停止传播到下游：停掉 far mihomo 会一并停掉隧道单元。

四个单元都设 `Restart=on-failure`、`RestartSec=2`、`LimitNOFILE=1048576`。
far 隧道单元用 `ExecStartPre=/sbin/modprobe tbtun`，`ExecStart` 写
`/proc/net/tbtun/config` 一次；模块在单元进入 active 后继续持有状态。

安装脚本只把 unit 复制到 `/etc/systemd/system/` 并 `daemon-reload`，不
`enable`、不 `start`。选择 systemd 时：

```sh
systemctl enable --now tbt-client-090 mihomo-090-near   # near
systemctl enable --now mihomo-090-far tbtun-090-far      # far
```

**边界**：mihomo 是 `Type=simple`，systemd 认为它 started 时端口可能还未
`bind`。`tbtun-090-far.service` 的 `After=` 只保证启动顺序，不保证 19001
已就绪。模块的 `target=` 拨号是每流惰性的，所以开机时没有近端流就不会命中
这个窗口；`install-far.sh` 的 pid 文件路径则显式 `wait_port` 等待监听。

### 2.6 `go install` 备选（不作为默认路径）

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
| `xudp` | `true` | `adapter/outbound/vless.go:69` | 显式写；默认部署下 `only-tcp: false`，UDP 实际走 smux，不走 XUDP |
| `smux.enabled` | `true` | `adapter/parser.go:234-241` | 通用解析器只读 `smux` |
| `smux.protocol` | `h2mux` | `adapter/outbound/singmux.go:25` | 可选 `smux`/`yamux`/`h2mux`（`sing-mux/client.go:57-71`） |
| `smux.max-connections` | `1` | `adapter/outbound/singmux.go:26` | 不写时默认 0；达到 `min-streams` 后 `offerNew` 会开第二条 TCP（`sing-mux/client.go:180-189`），撞上远端单 FEC flow |
| `smux.only-tcp` | `false` | `adapter/outbound/singmux.go:31` | false 时 UDP 走同一条 smux TCP（`singmux.go:49-57`）；true 时 UDP 改走 VLESS XUDP，另拨 TCP（`vless.go:369-404`），受单 FEC flow 限制 |
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

两行隧道配置（由 Makefile 生成；`CKEY`/`NONCE` 是同一次 `package` 内生成的
同一份随机值，near 和 far 必须相同）：

```text
# near (tbt_client --config)
tunnel 1 role=near listen=127.0.0.1:19000 peer=FAR_IP:19002 target=127.0.0.1:19001 n=8 m=24 w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=CKEY nonce=NONCE adaptive=off

# far (写入 /proc/net/tbtun/config)，只在 NEAR_IP 非空时追加 peer=
tunnel 1 role=far listen=0.0.0.0:19002 target=127.0.0.1:19001 n=128 m=128 w=256 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on crypto_key=CKEY nonce=NONCE adaptive=off [peer=NEAR_IP:19000]
```

- near 的 `target=` 只是 OPEN 的槽 0 默认端口；far 永远拨自己配置的 `target_addr`
  （`README.md:381`）。因此 near 写 `127.0.0.1:19001` 只作为占位，真正的目的地址
  由 far 的 `target=` 决定。
- far 的 `peer=` 是允许加入的 near 源地址锚点（`README.md:380`），必须是 near 的
  公网出口 IP；地址只接受字面量，不做 DNS（`README.md:598-602`）。
  Makefile 的 `NEAR_IP` 默认**为空**，此时 `far/tbtun.conf` 完全不带 `peer=`：
  动态 ISP 地址或 CGNAT 后的 near 没有可固定的公网地址，这是正常部署形态。
  far 改为用 `crypto_key=` 认证客户端，并用 `nonce=` 锚定第一个通过校验的
  HELLO。**当前边界**：锚点仍固定在该首个客户端地址上，直到 session 重建；
  session-reincarnation / roaming 落地前，换源地址（重拨、切网）会被拒绝。
  只有明确知道 near 的固定出口地址时才传 `NEAR_IP=<literal>`。
- `fec`、`mode`、`carriers`、`crypto`、`adaptive` 两端一致；`n`/`m`/`w` 按方向
  不同：near 是上行 8×24、`w=64`，far 是下行 128×128、`w=256`。`w` 在
  `mode=sc-ldpc` 下只接受 64/256/512，不写会默认 256（`tbt_parse.c:934-937`），
  所以 near 必须显式 `w=64`。`L=8` 也是生产形状的一部分。接收端按对端的
  `FEC_CFG` 形状解码（`README.md:52-53`），不要两端互相抄 `n/m/w`。
- `crypto` 未写时默认跟随 `fec`（`README.md:402`），所以 `fec=on` 两端都会加密；
  `crypto_key` 由 near 生成并在 HELLO 下发（`README.md:403`），不要只在一端手写。

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
（`tests/throughput/short.sh:42-43,121-123`）。

**安全边界**：`tls: false` 是设计选择。VLESS 的 UUID 只做身份校验，不做加密；
smux/XUDP 也不加密。机密性完全来自 tbtun 的 `crypto=on`：ChaCha8 对 body 做
流式异或，HELLO 元数据仍是明文。`crypto_key=` 的配置方式决定 HELLO 里有没有
密钥：

- 未配置 `crypto_key`（旧行为）：near 生成 256-bit 会话密钥，并用 `key_id=1`
  把它放进**每一个** HELLO。任何看到载体流的被动观察者都能拿到 key、解开 body，
  读出 VLESS UUID 与请求载荷。
- 配置 `crypto_key`（当前 Makefile 默认）：两端从同一份 PSK 派生，near 的
  `crypto_key_id` 是 `TBT_CRYPTO_KEY_ID_NONE`，`tbt_crypto_hello_fill()` 把
  HELLO 的 key 字段清零。线上只有 `key_id=0`、`nonce=` 与密文 body。

`nonce=` 是 128-bit session 标识与 anti-confusion token，**明文传输，不是密码**。
它的作用是在 `peer=` 为空时，让 far 只接受持有预共享 nonce 的客户端。
ChaCha8 body 是流式异或，不提供 AEAD 完整性；观察者仍能看出包长与时序。
far 的 `listen: 127.0.0.1` 只把入站入口限制在本机，不改变以上任何一点。

---

## 5. 配置陷阱（必须显式警告）

### 5.1 smux 与 XUDP 相互独立，`only-tcp` 决定 UDP 走哪条

- 客户端 `smux.enabled: true` 且 `only-tcp: false` 时，`SingMux.ListenPacketContext`
  直接把 UDP 交给 smux 的 `client.ListenPacket`（`adapter/outbound/singmux.go:49-57`），
  UDP 与 TCP 共用同一条 TCP 连接，tbtun 只看到一条应用流。
- `only-tcp: true` 时，UDP 回落到 `ProxyAdapter.ListenPacketContext`
  （`singmux.go:49-52`），VLESS 的 `ListenPacketContext` 先调用 `v.dialContext`
  新拨一条 TCP（`adapter/outbound/vless.go:369-404`），再用 XUDP 构造
  `CommandMux`（`vless.go:387-396,458`）。
- 0.8 的 far 模块只允许一条 FEC flow（`README.md:661-662`），第二条 TCP 会被
  `fec_stream_refused` 拒掉。所以默认部署用 `only-tcp: false`，UDP 走 smux；
  `only-tcp: true` 只是验证原生 VLESS CommandMux XUDP 的单独路径（smoke S4b），
  在单 FEC flow 限制解除前不能作为产品配置。
- `max-connections: 1` 是配套项：不写时默认 0，smux 在流数达到 `min-streams`
  后会 `offerNew` 开第二条 TCP（`sing-mux/client.go:180-189`），同样撞上单 flow。

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

### 5.6 浏览器 QUIC（UDP:443）被显式拒绝

near 模板在 `MATCH,PROXY` 之前有一条：

```yaml
rules:
  - AND,((NETWORK,UDP),(DST-PORT,443)),REJECT
  - MATCH,PROXY
```

依据（v1.19.34）：`rules/parser.go` 把 `AND` 交给 `logic.NewAND`；payload 必须
是 `((rule),(rule))` 形式，每个子规则再走同一个 `ParseRule`，所以
`NETWORK,UDP` 与 `DST-PORT,443` 都按普通规则解析。`REJECT` 是内建目标
（`config/config.go:893-899` 注册 `REJECT`），不需要在 `proxies` 里声明。

为什么拒绝：QUIC 自带拥塞控制与丢包恢复。把它塞进 smux 的一条可靠、有序 TCP
流，再经过单条 FEC flow，会叠加第二个拥塞控制器与队头阻塞；UDP 的丢包恢复还会
被 TCP 的重传掩盖，表现为浏览器卡住而不是降速。`REJECT` 让浏览器立刻回落
HTTP/2 over TCP，走同一条 smux 连接。

影响面：只匹配 `NETWORK=UDP` 且目的端口 443。DNS（UDP:53）与其它 UDP 仍由
`MATCH,PROXY` 送进 smux；TCP:443 不受影响。代价是使用 HTTP/3 的站点会多一次
回落，首包更慢，但不再产生双层拥塞控制。

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
| `smux.enabled` / `protocol` / `max-connections` / `only-tcp` / `padding` | 较稳定，但**静默失败** | 见 5.1 / 5.4 |
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
3. 检查 `near/config.yaml` 含 `AND,((NETWORK,UDP),(DST-PORT,443)),REJECT`；
   两份 `tbtun.conf` 的 `crypto_key`（64 hex）与 `nonce`（32 hex）相同。这两项
   由 `make ... verify` 自动执行；`mihomo -t` 同时证明当前版本仍接受 `AND`
   语法。
4. 启动 far，日志出现 `Vless[...] proxy listening at: 127.0.0.1:19001`
   （`listener/inbound/vless.go:161`）。
5. 启动 near，`curl 127.0.0.1:9090/proxies/tbtun`，确认返回里 `smux` 为 true。
   该字段由 `adapter/adapter.go:155` 写入 `ProxyInfo.SMUX`，经
   `hub/route/proxies.go:62-67` 暴露。**没有 `smux` 就停下查 Release Notes**。
6. 端到端两条：TCP（`curl https://example.com` 经 7890）与 UDP（DNS 经 7890）。
   默认 `only-tcp: false`，UDP 走 smux；要验证原生 XUDP 必须改 `only-tcp: true`，
   它会另拨一条 TCP，当前受单 FEC flow 限制（smoke S4b，预期阻断）。
7. Release 资产 404 时，查
   `https://api.github.com/repos/MetaCubeX/mihomo/releases/latest` 的资产名。

---

## 7. 当前缺口：打包可以先做，端到端验收会被阻断

这套设计依赖 0.8 分支的现状。0.8 还有三项未关闭的缺陷，另有一条单流架构限制；
另有两条打包后才暴露的会话边界。在它们解除前，`package` 与 `verify` 可以完成，
但“真实链路可用”不能签字。

**配置冻结**：两份 `tbtun.conf` 保持 `adaptive=off`。在 Route-A 的 NACK 修复
与 `adaptive=on` 的 KVM 验证都完成之前，不要把它翻成 `adaptive=on`。0.9.0 的
打包与烟测只验证固定形状：near 8×24、`w=64`，far 128×128、`w=256`。

1. **吞吐只有约 0.55 MB/s。** 300 ms WAN 下 payload 从 3.1 MB/s 掉到 0.65–1.0 MB/s，
   修复流量从 47 MB 涨到 270 MB/2 min。这个量级不足以承载 0.9.0 的“任意站点”目标。
2. **半开流永不回收。** 流对象与
   `far_sid` 在会话销毁前不回收；TCP keepalive 只是把
   carrier socket 关闭，没有证明流对象会随之释放。0.8 的验收仍把它列为未解决。
3. **载体 stall-dead 抖动未定。** `CHANGELOG.md:336-341` 记录：256 载体下
   stall/recover 每小时触发数千次，99% 在 200 ms 内恢复，且这些日志曾是内核冻结前
   的最后输出。恢复时间短不等于无害；在 0.9.0 的 256 载体形状下必须先定性。
4. **单 FEC flow 限制（架构限制，不是缺陷）。** far 每个会话只接受一条 FEC flow
   （`README.md:661-662`）。`only-tcp: true` 的 XUDP 会为 UDP 另拨一条 TCP
   （`vless.go:369-404`），第二条必然被 `fec_stream_refused` 拒绝。因此产品默认
   `only-tcp: false`，UDP 走 smux；原生 XUDP 只能作为 S4b 的边界实验。
5. **客户端重启触发 far 的 `crypto nonce changed`。** near 每次新建 session 都
   生成新的 `crypto_tx_nonce`（`tbt_session.c:1869-1878`）；隧道行里的 `nonce=`
   固定的是 HELLO 的 session 标识，不是这个方向 nonce。若旧 session 尚未销毁
   （见第 2 条），far 仍持有旧的 `crypto_rx_nonce`，新 HELLO 会在
   `tbt_rx.c:714-722` 被 `-EPROTO` 拒绝，日志是 `crypto nonce changed`。
   在 PSK session re-incarnation 落地前，near 重启后必须先让 far 的旧 session
   失效（重启模块或等它回收）才能重新建链。
6. **`tbt_parent_setsockbuf` 的 8 MiB 与 16 MiB 环形缓冲会放大 bufferbloat。**
   near 父监听 socket 设 8 MiB（`tbt_sock.c:709`），FEC 环是 16 MiB。0.9.0 用
   `local_queue_bytes` 把它收成一个默认 1 MiB 的本地队列预算：socket 请求
   256 KiB（内核翻倍为 512 KiB），未编码环 512 KiB；一个 PING/DNS 帧在饱和上传
   后面最多等约 1 MiB，而不是 32 MiB。400 ms / 128 MB/s 的高 BDP 实验把该参数
   设为 32 MiB（33554432）即恢复旧的 8 MiB 请求 + 16 MiB 环。宿主客户端用同名
   userspace 键。

因此交付顺序应为：

- 现在可做：Makefile、两棵产物树、两个安装脚本、两份配置、`mihomo -t` 门禁。
- 阻断项：上述三项缺陷关闭前，不做“端到端可用”的验收声明；第 4 条限制解除前，
  不把 XUDP 写成产品能力；第 5 条关闭前，near 重启不是无损运维动作。不以打包
  完成代替链路可用。

### 7.1 长期运行：用 DKMS，不要拷贝 `.ko`

`install-far.sh` 把 `tbtun.ko` 复制到 `/lib/modules/$(uname -r)/extra/` 再
`depmod -a`。这只对**当前**内核有效。VPS 若启用无人值守的内核升级，重启后
新内核找不到匹配模块，`modprobe tbtun` 失败，far 侧隧道直接不可用，而且
`install-far.sh` 不会在升级时自动重跑。

生产部署走 DKMS：

```sh
make dkms-tarball                 # 生成 dkms.tar.gz（含 dkms.conf 与模块源）
sudo tools/install_dkms.sh      # 需要 /lib/modules/$(uname -r)/build
```

`tools/install_dkms.sh` 只安装模块，不写 `/usr/bin` 或 `/usr/local/bin`；
`--uninstall` 删除 DKMS 条目与暂存树。每次内核升级后 DKMS 自动重编，
`modprobe tbtun` 仍然可用。

两条配套：

- BBR 的 `modprobe tcp_bbr` 只在安装时执行一次。需要重启后仍然生效时，
  写 `/etc/modules-load.d/tbtun.conf`，内容一行 `tcp_bbr`。
- 继续用 `.ko` 拷贝的机器，必须把“内核升级后重新 `package` 并重装 far”
  写进升级流程；否则模块与内核的 `vermagic` 不匹配。

---

## 8. 无法从源码确定的事项，以及能定论的实验

1. **`go install pkg@version` 是否应用目标模块的 `replace`。**
   `go.mod:151` 有 protobuf 的 replace。实验：Go 1.26 下
   `GOBIN=/tmp/gobin go install github.com/metacubex/mihomo@v1.19.34`，看是否成功、
   `go version -m /tmp/gobin/mihomo` 里 protobuf 的来源。
2. **Release 资产命名是否长期稳定。** 只在 v1.19.34 核实过；每次升级先查 API。
3. **smux/XUDP 的跨版本线兼容。** 无版本协商字段。实验：near vX、far vY 各跑
   TCP+UDP（默认的 UDP 走 smux）；XUDP 变体需 `only-tcp: true`，见 smoke S4b，
   当前受单 FEC flow 限制。失败就回退同版本。
4. **`xudp` 的文档地位。** 代码有、`docs/config.yaml` 没有。实验：把
   `only-tcp` 改成 true 后发真实 UDP 查询，确认走 `CommandMux`，同时预期第二条
   TCP 被 `fec_stream_refused` 拒绝（smoke S4b）。升级后按 6.3 第 5 步复验。
5. **`carriers=256` 与 `mode=sc-ldpc` 的稳定性。** 语法与形状见
   `README.md:397-401`、`tbt_parse.c:934-937`；0.8 的 stall 抖动见第 7 节。实验：
   按既有 smoke 矩阵逐步
   加到 256，不要一次拉满。
6. **far 的 `listen: 127.0.0.1` 是否满足部署 netns。** 源码只证明它绑本机；若模块
   与 mihomo 不在同一 netns，必须改成模块可达的地址。实验：目标 netns 里
   `ss -lntp` 确认 `19001` 的绑定地址。

## 9. 一句话结论

0.9.0 不需要“mihomo 插件”：near 一个原生 VLESS 出站（`smux` + `max-connections: 1`，
UDP 走 smux，`UDP:443` 在规则层被 `REJECT` 以便浏览器回落 HTTP/2；原生 XUDP
变体受单 FEC flow 限制，见 smoke S4b），
far 一个原生 VLESS 入站（`allow-insecure: true` 允许无 TLS），中间 near 跑
`tbt_client`、far 跑 `tbtun.ko`。打包只做三件事：取上游 mihomo 二进制、构建
`src/client/tbt_client` 与 `tbtun.ko`、渲染两份 YAML、两行隧道配置与四个 systemd
单元。一次 `package` 为两端写入同一份随机 `crypto_key`/`nonce`，near 的 HELLO
不再广播会话密钥；`NEAR_IP` 默认为空，far 不写 `peer=`。升级的唯一开关是
`MIHOMO_VERSION`，两端必须同版本。`adaptive=off` 在 Route-A NACK 修复与
`adaptive=on` KVM 验证完成前冻结；在上述 0.8 缺陷与两条会话边界关闭前，只能
验收装配，不能验收端到端可用。
