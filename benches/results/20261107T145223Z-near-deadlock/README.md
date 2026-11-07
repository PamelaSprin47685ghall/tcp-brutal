# near procfs 死锁证据（2026-11-07）

## 触发

`benches/short.sh` 以 64 carriers（n=4 m=4, grouped, w=8, fec=on,
crypto=on, http-kal）配置 near/far 后，join 未完成：far `c3 n=64 up=0
lost=8`，dmesg 反复出现 `carrier 255 gen 1..8 lost: sock=1 connected=1
hello_sent=0 up=0`。near 随后失去响应。

## 现场

`near-deadlock.txt`（重启前采集）：

```
4604      DNs tbt_show_stats      python3 /usr/local/sbin/tbtun-sampler.py
1130803   Ds  tbt_session_destroy sh -s -- 64 9001 ...
1142142   D   tbt_show_stats      grep c3 /proc/net/tbtun/stats
1149622   D   tbt_show_stats      grep -m1 c3 /proc/net/tbtun/stats
1150988   Ds  tbt_config_apply    bash -c echo flush > /proc/net/tbtun/config
1154009   D   tbt_show_stats      grep -m1 c3 /proc/net/tbtun/stats
1155372   Ds  tbt_config_apply    bash -c printf 'del 1\n' > /proc/net/tbtun/config
1159827   D   proc_entry_rundown  rmmod tbtun
```

`/sys/module/tbtun/refcnt` = `-1`，模块状态 `Unloading`，
`/proc/net/tbtun` 已消失。`kill` 对 D 状态无效；`rmmod` 卡在
`proc_entry_rundown`，等待上述 stats reader。

`far-carrier-lost.txt`：far 侧的 carrier ACCEPTED / lost 段。

`near-before.txt`：重启前 boot_id `0a91d2eb-3122-41eb-913c-024a4e1fe724`。

## 恢复

经用户授权 `sync; systemctl reboot` 重启 near。重启后重新加载 fix27，
只恢复 32 carriers 的短测；64/256 在 procfs 死锁修复前不再运行。

## 结论

`tbt_show_stats` 与 `tbt_config_apply` 在 64 carriers 配置下可进入不可中断
睡眠，并让模块卸载永久阻塞。这是一个独立于吞吐的稳定性缺陷：任何在
64+ carriers 下读取 `/proc/net/tbtun/stats` 或重配隧道的操作都可能触发。
修复前，短测脚本应限制在 32 carriers，并避免 `flush`（覆盖 tunnel 配置
同样会进入 `tbt_config_apply`，但 32 carriers 下可完成）。
