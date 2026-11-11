# 256 carriers 首连 0 字节 + near 死锁证据（fix31）

## 版本与时间

- near: fix31 `B52199BAC7DF4D9A206AA5F`（`/root/tbtun-0.2.1-fix31-20261107T230808Z/tbtun.ko`）
- far: fix31 `8898C4C2C05DFABAD2D47C4`
- 2026-11-07 23:43-23:52 UTC

## 现象

新建 256 carriers tunnel（`crypto=on crypto_key=-`，即 near 自动生成
session key）后，第一条公共连接 `connect()` 成功（约 9 ms），但 15 s 内
没有收到任何 HTTP 头字节。near 的 carrier 反复 lost/replaced，far 侧
`up=0`。

## 根因证据

far dmesg（`far.txt` / `second-far.txt`）反复出现：

```
tbtun: tunnel 1: HELLO crypto resolve failed: crypto key changed (-71)
```

near c3（`near-deadlock.txt` / `second-near.txt`）：

```
tunnel 1 c3 n=256 up=18 lost=1036 replaced=1023 ...
```

far c3：`n=256 up=0 lost=13`。near 的 `hello_sent=1 up=1` 后立即 lost，
说明 near 本地认为 carrier up，但 far 拒绝了 HELLO 的 key。

**far 重载模块后仍复现**（`rmmod`/`insmod` fix31 后再次出现
`crypto key changed`），排除 far 侧旧 key 残留；问题在 near 的自动 key
在 256 carrier 的重连之间不一致。crypto key 重连问题已另派工人分析。

## Workaround（已实测）

两端固定同一个 `crypto_key=<64 hex>` 后，join 立即 `up=256`，首连
`connect=0.008 first_byte=0.303`（`psk-first-connect.txt`）。这说明
DATA-before-OPEN 不是主因；主因是 HELLO/crypto key 协商失败导致
carrier 无法 up、没有可用数据路径。

## 伴随死锁（near 需要现场恢复）

为在 fix31 上复现，near 从当前模块回退到 fix31 时，`rmmod` 卡在
`proc_entry_rundown`（refcnt=-1，模块 Unloading），一个写 config 的
bash 卡在 `tbt_session_destroy`。重启后 near 的 ping/ARP 正常
（MAC 02:51:22:e5:67:53），但 TCP 22 先是 reset、后为 refused，
sshd 未启动。现场需要串口/电源恢复；不要反复 ssh。

`near-unreachable.txt` 记录了重启后的 ping/ARP/22 状态。

## 文件

| 文件 | 内容 |
|---|---|
| `near-deadlock.txt` | 第一次复现：near ps D 状态、refcnt、dmesg |
| `far.txt` | 第一次复现：far stats/ss/dmesg |
| `second-near.txt` | 第二次复现：near c3/c10/c11/config/dmesg |
| `second-far.txt` | 第二次复现：far c3/c10/c11/ss/dmesg（25 次 crypto key changed） |
| `psk-first-connect.txt` | 固定 PSK 后的首连结果 |
| `boot_id_before.txt` | 重启前 boot_id `d6999c60-…` |
| `near-unreachable.txt` | 重启后 ping/ARP/22 状态 |

另有第一次 join 风暴的原始目录：
`tests/throughput/results/20261107T234336Z-first-connect-join-storm/`。

## fix33 验证清单（0.2.2，near 恢复后）

1. 加载 fix33（HEAD 3bfa36b），确认 srcversion；不要武装 watchdog。
2. 新建 256 tunnel（`crypto_key=-`），join 后立即用 15 s 首连客户端：
   `connect`、`first_byte`、header 字节数。
3. 同时采集 near c3/c10/c11/c12、`tx_submit_calls`、
   `fec_flush_pass/skip`、`fec_credit_wait`；far `streams_total`、
   `rx_records`、`rx_delivered`、`ss -tin` cwnd。
4. 判据：far `streams_total=0`/`rx_records=0` 且 near
   `tx_submit_calls>0`、`fec_flush_pass>0` → DATA-before-OPEN drop；
   若 far 仍有 `crypto key changed`，则是 key 协商未修。

## 0.2.2 验证结果（fix33，2026-11-08）

- 32 carriers：join 与首连 78/72 Mbit/s。
- 顺序流：前一条连接 abort 后新建流，15 s 交付 92.7 MB；无 HTTP/0.9，
  `fec_drop_window=0`，无 stuck 流。
- 32-carrier 短测 32.1 Mbit/s，无回归。
