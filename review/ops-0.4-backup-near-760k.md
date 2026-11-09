# 760K（192.168.123.74）备用 near：操作步骤与回滚

调查时间：2026-11-08 14:15 UTC。基线：`e723dff`。
**实测更新（2026-11-09 13:02–13:05 CST）**：已在 `.74` 完成 root 免密、
7.3.0-rc6 原生编译和 `45.11.1.62` 直达的实测，三项全部满足。边界：没有
触碰 `.85` 与 `45.11.1.62` 的发布门负载，只向 `45.11.1.62` 发了 3 个
ping；没有重启 mihomo，没有加载 tbtun.ko。下面保留操作步骤，每节的
“实测”记录本轮结果。

## 1. 已知事实

- `.74` 在本机 ARP 中 `REACHABLE`，MAC `94:de:80:be:00:22`。
- 实测：root 免密已可用
  （`ssh -o BatchMode=yes root@192.168.123.74 'id'` → `uid=0(root)`），
  无需再装 key。
- known_hosts 有 `.74` 的 ed25519 主机键
  `SHA256:FC60BmjSUvV2UIsINSyjnrtn18iN5eaBdYilyCcmLpU`，说明此前有人连过；
  2026-11-09 实测 `BatchMode=yes` 登录成功，root 免密可用。
- `review/0.3.2-scope-audit.md:154`：760K = `192.168.123.74`，AMD Athlon
  X4 760K；2026-11-06 在 `w-microbench` 做过 memcpy/XOR/GF(2) 微基准。
  没有 0.3.x tbtun 在 `.74` 的运行证据。
- 本机 `~/.ssh/config` 没有 `.74` 的 Host 项；只有 `45.11.1.62` 和
  `192.168.123.84`。
- 仓库的活动 near 默认值已迁到 `.85`（`benches/adaptive/lib.sh`、
  `review/wan-experiment/arm64/sclc-verify/verify-0.3.0.sh` 等）；
  `prod-1g/run-1g.sh` 仍保留 `.65` 历史默认。迁移清单见
  `review/ops-0.4-ip85-migration.md`。

## 2. 只读就绪检查

先跑：
```
scripts/backup-near-check.sh --plan
scripts/backup-near-check.sh
```
第二个命令会 ping、查 known_hosts、用 `BatchMode=yes` 跑 `id -u`、
`uname -m`、`lsmod`、`/dev/net/tun`、`ip route get 45.11.1.62`、
`ip rule show`，并只 `grep` mihomo 配置里的 DIRECT 行。它不写任何文件。

人工复核：
```
ssh-keygen -F 192.168.123.74 -l
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=5 \
    root@192.168.123.74 'id -u; uname -m; uname -r'
```
期望 `0`、`x86_64`、以及一个明确的内核版本。`id -u` 不是 `0` 时，先读完
第 3 节再决定是否装 key。

实测（2026-11-09）：`id -u` 返回 `0`，`uname -r` 为
`7.3.0-rc6-1-MANJARO`，`uname -m` 为 `x86_64`；
`scripts/backup-near-check.sh` 的就绪项全部通过。

## 3. root 免密：检查、安装、回滚

### 3.0 实测结论

已满足，本轮不执行安装：

- `ssh -o BatchMode=yes root@192.168.123.74 'id'` 返回 `uid=0(root)`。
- 本机密钥 `~/.ssh/id_ed25519` 指纹
  `SHA256:apK2E+1fSzd4puTTCFfqH6STRIpg6ZzSX+e5D8/vOu4` 验证通过。
- `/root/.ssh/authorized_keys` 已有 5 行（含本机公钥），无需追加。

下面的安装步骤只在换机或换钥时使用。

检查失败时的最小安装（需要一次密码或串口/控制台）：
```
ssh-copy-id -i ~/.ssh/id_ed25519.pub root@192.168.123.74
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes \
    root@192.168.123.74 'id -u'
```
安全约束：
- 只加一把操作者的公钥；不要打开 `PasswordAuthentication yes`，不要改
  `PermitRootLogin yes`。`prohibit-password` 足够。
- 先备份：`cp -a /root/.ssh/authorized_keys /root/.ssh/authorized_keys.bak-$(date +%Y%m%dT%H%M%S)`。
- 回滚只删这一行，不要清空整个文件：
  ```
  key=$(cat ~/.ssh/id_ed25519.pub)
  grep -vF "$key" /root/.ssh/authorized_keys > /tmp/ak.new
  cat /tmp/ak.new > /root/.ssh/authorized_keys
  chmod 600 /root/.ssh/authorized_keys
  ```
  更稳的做法是 `visudo` 式的逐行确认；`grep -vF` 只匹配完整公钥行。
- 如果 `.74` 是共享主机，装 key 是安全变更，备用期结束后删除。

## 4. mihomo 对 `45.11.1.62` 的 DIRECT

### 4.0 实测结果（2026-11-09）

- mihomo Meta `v1.19.33`，TUN `auto-route`。
- 原路由：`ip route get 45.11.1.62` →
  `via 198.18.0.2 dev Meta table 2022 src 198.18.0.1`。
- 配置 `/opt/mihomo/config.yaml` **已含**
  `IP-CIDR,45.11.1.62/32,DIRECT`（sha256 `539f91c1…76492207`），
  本轮没有改动配置。
- 已执行 `ip rule add pref 100 to 45.11.1.62/32 lookup main`；现规则为
  `100: from all to 45.11.1.62 lookup main`，优先级高于 mihomo 的
  9000–9020。
- 现路由：`45.11.1.62 via 192.168.123.254 dev enp2s0
  src 192.168.123.74`，走物理网卡。
- 可达性：`ping -c 3 -W 2 45.11.1.62` → 3/3 收到，270–310 ms。
- 没有重启 mihomo；已有 frpc 连接 `192.168.123.74:55930 →
  45.11.1.62:7000` 未受影响，新规则只作用于新流。

目标：tbtun near 的 carrier 拨号必须走物理网卡，不能被 mihomo 的 TUN
（Meta/198.18.0.1）劫持。仓库的既有做法是主机策略路由：
`benches/short.sh:42-43,121-123` 明确写“mihomo otherwise sends the carriers
through Meta”，并用 `ip rule ... pref 100 lookup main`。mihomo 的 DIRECT
规则是第二层，两者不冲突。

### 4.1 找服务与配置

```
systemctl list-units --type=service | grep -i -E 'mihomo|clash'
systemctl cat mihomo 2>/dev/null
ps -o pid,args= -C mihomo
```
从 `ExecStart` 的 `-d DIR` 或 `-f FILE` 确认配置文件。常见：
`/etc/mihomo/config.yaml`、`/root/.config/mihomo/config.yaml`。
不要假设路径；`backup-near-check.sh` 会打印它找到的第一个可读路径。

### 4.2 加规则

备份：
```
CFG=/etc/mihomo/config.yaml          # 以实际路径为准
cp -a "$CFG" "$CFG.bak-$(date +%Y%m%dT%H%M%S)"
```
在 `rules:` 的**最前面**加：
```
rules:
  - IP-CIDR,45.11.1.62/32,DIRECT,no-resolve
```
`no-resolve` 避免为 IP 字面量做 DNS；tbtun 配置里本来就是
`peer=45.11.1.62:9000`，不需要域名。

校验并重载：
```
mihomo -t -f "$CFG"          # 有的构建用: mihomo -t -d "$(dirname "$CFG")"
systemctl reload mihomo 2>/dev/null || systemctl restart mihomo
```
`restart` 会中断该机上所有正在走的代理连接；`reload` 不支持时只能用
`restart`，先记录 active 连接数。

### 4.3 验证

```
ip route get 45.11.1.62
ip rule show | grep '45.11.1.62' || true
```
期望 `ip route get` 显示 `dev eth0`（或实际物理网卡）和
`src 192.168.123.74`，不是 `dev Meta`/`198.18.0.1`。
如果仍显示 Meta，说明 mihomo 的 TUN 路由优先于 DIRECT，追加主机规则：
```
ip rule add to 45.11.1.62/32 lookup main pref 100
ip route get 45.11.1.62
```
这条规则是仓库验证过的权威旁路；mihomo 的 DIRECT 只影响它自己的选路，
不能保证 TUN `auto-route` 不抢先。

### 4.4 持久化（已启用）

实际落地（2026-11-09）：

- `/root/tbtun-bypass-far.sh`（0755）：幂等删/加 pref 100 规则、flush
  cache，并打印 `ip route get`。
- `/etc/systemd/system/tbtun-bypass-far.service`：`Type=oneshot`、
  `After=mihomo.service`、`PartOf=mihomo.service`、
  `WantedBy=multi-user.target`。
- 状态：`enabled`、`active`、`Result=success`、`ExecMainStatus=0`。
  开机自启；`systemctl restart mihomo` 会带动本单元重加强规则。

`ip rule` 本身不持久，所以这条 unit 是持久化的唯一入口。

## 5. 作为 near 启动

### 5.0 模块实测（2026-11-09）

- 头文件：`/usr/lib/modules/7.3.0-rc6-1-MANJARO/build`
  （`Module.symvers` 存在）。
- 内核配置：`CONFIG_CC_IS_CLANG` 未启用（gcc 构建）；
  `CONFIG_MODULE_SIG_FORCE` 未设。
- 源码：commit `aceca3a060aaf408afa94b843f5f7c2e95c0f428`，用
  `git archive | tar -x` 落到 `/tmp/tbt-760k-build`。
- 构建：`make -j$(nproc) all` → EXIT 0，33.86 s。Makefile 没有
  `tbtun.ko` 这个 target，直接用默认 target。
- 产物：`/tmp/tbt-760k-build/tbtun.ko`，9.6 MiB，vermagic
  `7.3.0-rc6-1-MANJARO SMP preempt mod_unload`，sha256
  `2d3e740c2ea1aa296c17a77c9a5768399f45210fecca84d1abd3f2223cc616a3`。
- 模块未签名；`CONFIG_MODULE_SIG_FORCE=n`，可以加载，但会 taint 内核。
- 未执行 `insmod`：`lsmod` 无 tbtun，`/dev/tbtun` 不存在。

前提：**同一时刻只有一个 near**。先停 `.85` 的 tunnel，或在 far 侧用不同的
监听端口/隧道 id，避免两个 near 抢同一个 session。

`.74` 是 x86_64，不能用 `.85` 的 aarch64 `.ko`：
```
uname -r
# 在 .74 上从目标 tag 构建 x86_64 模块，或 scp 对应内核的 .ko
insmod /root/tbtun-<ver>/tbtun.ko
lsmod | grep '^tbtun'
```
配置示例（形状按当前实验，不要照抄 256）：
```
printf 'tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 \
n=4 m=4 fec=on adaptive=off mode=grouped w=8 carriers=32 crypto=on \
crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 \
replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 \
rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 \
carrier_stall_ms=0 portblock=30000-30511 dev=eth0 \
nonce=00112233445566778899aabbccddeeff\n' > /proc/net/tbtun/config
```
先跑 `near-fetch.sh` 的 preflight，期望：
```
NEAR_CHECK bypass_route PASS (dev=eth0 rule=added ...)
```
启动后确认 far 的 9000 有入站，`ss -tnp | grep 45.11.1.62`，near 侧
carrier `up` 增长。不要在这台机器上同时跑微基准和 1 GiB，760K 的 CPU
余量有限。

## 6. 回滚（顺序固定）

### 6.1 本轮旁路的回滚（已落地）

```
systemctl disable --now tbtun-bypass-far.service
ip rule del pref 100 to 45.11.1.62/32 lookup main
# 可选：rm /root/tbtun-bypass-far.sh /etc/systemd/system/tbtun-bypass-far.service
```

mihomo 配置未修改，无需恢复；可用 4.0 节的 sha256 核对。frpc 连接未受
影响。旁路规则只作用于新流，删除规则不会中断已建立的连接。

### 6.2 备用 near 上线后的回滚

```
# 1) 停 tunnel，等 1 s
printf 'del 1\n' > /proc/net/tbtun/config
sleep 1
# 2) 卸模块
rmmod tbtun
lsmod | grep '^tbtun' || echo tbtun-not-loaded
# 3) 删主机旁路规则和 marker
ip rule del to 45.11.1.62/32 lookup main pref 100 2>/dev/null || true
rm -f /root/.tbt-adaptive-bypass-pref100
# 4) 恢复 mihomo 配置并重启
cp -a "$CFG.bak-<timestamp>" "$CFG"
systemctl restart mihomo
ip route get 45.11.1.62
# 5) 删除为备用 near 加的 root key（如果加过）
# 6) 删除临时模块/脚本/下载文件
```
恢复后 `ip route get 45.11.1.62` 应回到原来的 Meta 路径（或 mihomo 的正常
出口）。如果 `systemctl restart mihomo` 失败，先恢复备份再重启；仍失败则
检查 TUN 设备是否残留，最后才考虑重启 NetworkManager 或整机。

## 7. 风险

- `systemctl restart mihomo` 会中断 `.74` 上所有代理连接；选维护窗口。
- DIRECT 规则只影响新连接；已经建立的连接仍走旧路径。
- `ip rule` 不持久；只加 mihomo 规则而重启后忘了主机规则，carrier 会回到
  Meta，表现为连接超时而不是报错。
- fake-ip/DNS 劫持：tbtun 用 IP 字面量，绕过 DNS；不要把 peer 改成域名。
- root key 是安全变更；共享主机上要记录并到期删除。
- x86_64 模块必须匹配 `.74` 的 `uname -r`；aarch64 `.ko` 会 `insmod` 失败。
- 两个 near 同时在线会让 far 的 session/端口冲突；先停 `.85`。
- `.74` 是微基准机，可能有其他 CPU/网络负载；备用 near 的吞吐结论不能
  直接和 `.85` 对比。
- 本轮只完成了待命就绪：没有重启 mihomo、没有加载 tbtun.ko；上述吞吐
  与稳定性仍未在 `.74` 上实测。
