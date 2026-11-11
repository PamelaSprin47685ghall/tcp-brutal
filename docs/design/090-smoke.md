# 0.9.0 手动验收清单（smoke）

状态：手动执行清单，未在真机跑过。
适用基线：`w-090-design`（0.8 用户态客户端 + 0.8 内核模块）。
禁止在 near/far 生产环境执行；下面所有命令只用于一对实验机。

拓扑与端口见 `docs/design/090-packaging.md`。本文的“层”只有三种：

- **M**：mihomo 配置/协议层（VLESS、smux、XUDP 的解析与线格式）。
- **T**：tbtun 数据面（载体、FEC、流的建立与回收）。
- **M×T**：两者交界（mihomo 的字节流经 tbt_client/模块到对端 mihomo）。

## 0. 实验前置

两台实验机，均已按 `deploy/090/` 的产物安装：

```sh
export NEAR=lab-near          # ssh alias；near 也可为本机
export FAR=lab-far
export NEAR_MIXED=127.0.0.1:7890
export FAR_HTTP=127.0.0.1:18080
```

far 侧准备一个受控 HTTP 源（只绑 far 的 loopback；mihomo 的 `MATCH,DIRECT`
会让它访问 far 本机）：

```sh
ssh "$FAR" '
  mkdir -p /tmp/tbt-smoke && cd /tmp/tbt-smoke
  dd if=/dev/urandom of=1k.bin  bs=1024 count=1   status=none
  dd if=/dev/urandom of=1m.bin  bs=1M   count=1   status=none
  dd if=/dev/urandom of=64m.bin bs=1M   count=64  status=none
  nohup python3 -m http.server 18080 --bind 127.0.0.1 \
    >/tmp/tbt-smoke/http.log 2>&1 &
  echo $! > /tmp/tbt-smoke/http.pid
  sha256sum 1k.bin 1m.bin 64m.bin
'
```

记录 far 打印的三个 SHA-256；后面逐步比对。

## 1. 预检（M + T）

```sh
# 两端版本必须一致
ssh "$FAR" 'mihomo -v' | head -2
mihomo -v | head -2

# 配置语法
mihomo -t -f /etc/mihomo-090/config.yaml
ssh "$FAR" 'mihomo -t -f /etc/mihomo-090/config.yaml'

# 端口与隧道行
ss -lntp | grep -E ':(7890|19000)\b'
ssh "$FAR" 'ss -lntp | grep -E ":(19001|19002)\b"; cat /proc/net/tbtun/config'

# mux 确实被配置接受（不是静默关闭）
curl -s http://127.0.0.1:9090/proxies/tbtun \
  | python3 -c 'import json,sys; p=json.load(sys.stdin); print("type",p.get("type"),"smux",p.get("smux"))'
```

预期：

- 两端 `mihomo -v` 版本相同。
- 两个 `-t` 都打印 `test is successful`。
- near 有 `:7890`、`:19000`；far 有 `:19001`、`:19002`。
- `/proxies/tbtun` 输出 `smux True`。

失败证据：两份 `config.yaml`、`mihomo -v`、`ss -lntp`、
`tail -n 100 /var/log/tbtun-090/*.log`、far `dmesg | tail -n 50`。

层：M（配置与 mux 开关）+ T（监听端口）。

---

## 2. S1 小请求 1 KB：HTTP 200

```sh
curl -x http://127.0.0.1:7890 -sS \
  -o /tmp/1k.dl -w 'code=%{http_code} bytes=%{size_download} time=%{time_total}\n' \
  "http://127.0.0.1:18080/1k.bin"
sha256sum /tmp/1k.dl
```

预期：`code=200 bytes=1024`；`/tmp/1k.dl` 的 SHA-256 等于 far 的 `1k.bin`。

失败证据：`curl -v` 全量输出；near `/proxies/tbtun`；far
`cat /proc/net/tbtun/stats` 的 c9 行；`dmesg | tail -n 80`。

层：M×T。它同时验证 VLESS 建流、smux 首流、tbtun 首个应用流。

---

## 3. S2 大文件下载与 SHA-256

```sh
time curl -x http://127.0.0.1:7890 -sS --max-time 900 \
  -o /tmp/64m.dl -w 'code=%{http_code} bytes=%{size_download} speed=%{speed_download}\n' \
  "http://127.0.0.1:18080/64m.bin"
sha256sum /tmp/64m.dl
```

预期：`code=200 bytes=67108864`；SHA-256 与 far 的 `64m.bin` 相同。

**当前会被已知缺陷阻断（吞吐约 0.55 MB/s）：** 64 MiB 预计需要约 2 分钟；
这不是 mihomo 配置问题。若 `--max-time 900` 内仍未完成，记为吞吐缺陷证据，
不要改结论为“VLESS 配置错误”。若退出码 28（超时），先记录
`/proc/net/tbtun/stats` 与 `ss -ti`，再决定是否缩短到 8 MiB 重跑。

失败证据：`curl -w` 一行、`ss -ti` 的 `retrans`/`cwnd`、
far/near `/proc/net/tbtun/stats`、`dmesg | grep -i -E 'fec|stall|reset'`。

层：T 为主（FEC/载体吞吐），M×T 的完整性由 SHA-256 证明。

---

## 4. S3 同一 mux 会话上并发 8 个请求

在一个终端发起 8 个并发下载（限速让它们重叠约 10 秒）：

```sh
for i in $(seq 1 8); do
  curl -x http://127.0.0.1:7890 -sS --limit-rate 100k \
    -o "/tmp/1m.$i.dl" -w "%{http_code}\n" \
    "http://127.0.0.1:18080/1m.bin" &
done
wait
sha256sum /tmp/1m.*.dl
```

同时在另一个终端观察 near → tbt_client 的 TCP 连接数：

```sh
ss -tn | awk '$5 ~ /:19000$/ {print}'
```

预期：

- 8 行全部为 `200`。
- 8 个文件 SHA-256 都等于 far 的 `1m.bin`。
- 运行期间到 `127.0.0.1:19000` 的 TCP 连接数为 **1 到 2 条**（smux 默认在流数
  达到 `min-streams` 时可能再开一条会话），而不是 8 条。8 条说明 smux 没生效。

为什么这能证明“同一 mux”：tbtun 只看到 mihomo 到 `19000` 的**一条**字节流；
8 个 HTTP 请求是这条流内部的 8 个 smux 子流。若看到 8 条 TCP，则 mux 未启用。

失败证据：8 行状态码；`ss -tn` 快照；`/proxies/tbtun` 的 `smux`；
near mihomo 日志；`/proc/net/tbtun/stats`。

层：M（smux 复用）+ M×T（单条 tbtun 流承载多子流）。

**注意**：0.8 的 FEC 数据面每个会话只允许一条**应用流**。smux 子流在 VLESS
内部，不会被 tbtun 当成多条流；这正是必须开 smux 的原因之一。

---

## 5. S4 一条 UDP 查询（XUDP 路径）

前提：near 配置必须是 `smux.only-tcp: true` + `xudp: true`。若 `only-tcp: false`，
UDP 会走 smux 自己的 UDP，而不是 VLESS XUDP，本步不算通过。

把下面的脚本存成 `/tmp/tbt-xudp.py`，然后执行：

```python
#!/usr/bin/env python3
import socket, struct, sys

PROXY = ("127.0.0.1", 7890)
DNS = ("1.1.1.1", 53)
NAME = "example.com"

def rx(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise RuntimeError("short read")
        b += c
    return b

def query():
    q = struct.pack("!HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0)
    for label in NAME.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + struct.pack("!HH", 1, 1)
    return q

def main():
    t = socket.create_connection(PROXY, 5)
    t.settimeout(5)
    t.sendall(b"\x05\x01\x00")
    if rx(t, 2) != b"\x05\x00":
        raise RuntimeError("socks5 greeting rejected")
    t.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
    ver, rep, _, atyp = rx(t, 4)
    if ver != 5 or rep != 0:
        raise RuntimeError(f"udp associate failed: ver={ver} rep={rep}")
    if atyp == 1:
        host = socket.inet_ntoa(rx(t, 4)); port = struct.unpack("!H", rx(t, 2))[0]
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, rx(t, 16)); port = struct.unpack("!H", rx(t, 2))[0]
    else:
        raise RuntimeError(f"bad associate atyp={atyp}")
    if host in ("0.0.0.0", "::"):
        host = PROXY[0]
    if port == 0:
        raise RuntimeError("relay returned port 0")

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(5)
    hdr = b"\x00\x00\x00\x01" + socket.inet_aton(DNS[0]) + struct.pack("!H", DNS[1])
    u.sendto(hdr + query(), (host, port))
    data, _ = u.recvfrom(4096)
    if len(data) < 10 or data[:3] != b"\x00\x00\x00":
        raise RuntimeError("bad socks5 udp header")
    p = 3
    if data[p] == 1:
        p += 1 + 4 + 2
    elif data[p] == 4:
        p += 1 + 16 + 2
    elif data[p] == 3:
        p += 1 + 1 + data[p + 1] + 2
    else:
        raise RuntimeError("bad udp atyp")
    dns = data[p:]
    tid, flags, qd, an, ns, ar = struct.unpack("!HHHHHH", dns[:12])
    rcode = flags & 0xF
    print(f"udp-ok id={tid:#06x} rcode={rcode} answers={an} bytes={len(dns)}")
    if rcode != 0 or an < 1:
        sys.exit(1)

main()
```

执行：

```sh
python3 /tmp/tbt-xudp.py
```

预期：`udp-ok ... rcode=0 answers>=1`。

失败证据：脚本的异常栈；near mihomo 日志；`/proxies/tbtun` 的 `xudp`/`smux`；
far `dmesg`；`/proc/net/tbtun/stats`。

层：M（VLESS XUDP / CommandMux）。tbtun 只承载字节，不解析 UDP。

---

## 6. S5 mux 断线重连 + `fec_stream_refused`

触发方式：重启 **near mihomo**，让 near→19000 的 tbtun 应用流关闭，同时 smux
会话消失；随后新请求必须建立新的 tbtun 流与新的 smux 会话。

```sh
# 记录基线
ssh "$FAR" 'grep -o "fec_stream_refused=[0-9]*" /proc/net/tbtun/stats'
ssh "$FAR" 'dmesg | grep -c fec_stream_refused || true'

# 重启 near mihomo（按 install-near.sh 的 pid/log 约定）
kill "$(cat /run/tbtun-090/mihomo.pid)" 2>/dev/null || true
sleep 1
nohup mihomo -d /etc/mihomo-090 -f /etc/mihomo-090/config.yaml \
  >>/var/log/tbtun-090/mihomo.log 2>&1 &
echo $! > /run/tbtun-090/mihomo.pid

# 等 mixed-port 回来，再发 3 次小请求
sleep 2
for i in 1 2 3; do
  curl -x http://127.0.0.1:7890 -sS -o /dev/null \
    -w "round=$i code=%{http_code}\n" "http://127.0.0.1:18080/1k.bin"
done

# 看拒绝计数与内核日志
ssh "$FAR" 'grep -o "fec_stream_refused=[0-9]*" /proc/net/tbtun/stats'
ssh "$FAR" 'dmesg | grep -i fec_stream_refused || true'
```

预期：

- 3 次请求都是 `200`（第一次允许失败一次后重试；把实际次数写进记录）。
- far 的 `fec_stream_refused` 计数与基线相同（通常为 0）。
- `dmesg` 没有新的 `fec_stream_refused`。

**当前会被已知缺陷阻断（半开流不回收）：** 0.8 的流对象只在会话销毁时回收
（`review/analysis/multiconn.md:26-27`）。重启 near mihomo 会让旧流以半开/关闭
状态留下；新流可能被拒绝，从而出现 `fec_stream_refused` 增长或首次请求失败。
若出现，**记录为 0.8 缺陷证据，不记为 mihomo 配置失败**，也不要通过反复重试把它
“跑绿”。

失败证据：前后两次 `fec_stream_refused` 值；`dmesg` 原文；
`/proc/net/tbtun/stats` 的 c9 行；near/far `ss -tn`；mihomo 日志。

层：T（流生命周期）+ M（smux 会话重建）。

---

## 7. 结果记录模板

| 用例 | 层 | 预期 | 实际 | 判定 | 备注 |
| --- | --- | --- | --- | --- | --- |
| S1 1 KB | M×T | 200, SHA 相同 | | PASS/FAIL | |
| S2 64 MiB | T | SHA 相同，<900 s | | PASS/FAIL/SLOW | 0.55 MB/s 缺陷 |
| S3 8 并发 | M | 8×200，1–2 条 TCP | | PASS/FAIL | |
| S4 UDP | M | rcode=0, answers≥1 | | PASS/FAIL | only-tcp=true 才算 XUDP |
| S5 重连 | T | 3×200，refused 不增 | | PASS/FAIL/BLOCKED | 半开流缺陷 |

判定规则：

- `PASS`：命令退出 0，且预期可观测全部成立。
- `FAIL`：命令退出非 0，或预期可观测不成立，且不是下面两项。
- `SLOW`：数据完整但耗时超过预算；单独记为吞吐缺陷。
- `BLOCKED`：失败原因落在第 8 节的三项已知缺陷内；仍要保留全部证据。

## 8. 当前已知阻断（0.8）

1. **吞吐约 0.55 MB/s。** S2 会慢；不是 mihomo 的问题。
2. **半开流不回收。** S5 可能失败并出现 `fec_stream_refused`；这是 0.8 的流生命
   周期缺陷，见 `review/analysis/multiconn.md:26-27`。
3. **载体 stall-dead 抖动。** 256 载体下 stall/recover 每小时数千次
   （`CHANGELOG.md:336-341`）。S2/S3 期间若出现周期性卡顿，先看
   `/proc/net/tbtun/stats` 的 `stall_*`，不要先怀疑 mihomo。

在这三项关闭前，本清单只能证明“装配正确、配置被接受、单流功能可用”，不能证明
0.9.0 的端到端产品目标。
