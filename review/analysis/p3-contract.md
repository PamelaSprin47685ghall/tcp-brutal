# P3 契约审计：未生效键、config/ports、资源上限、规格漂移、版本

- 仓库：`/home/kunweiz/Desktop/vibe/tcp-brutal`
- 代码基线：工作树（HEAD `ee0e5e6`，含未提交改动）；行号以当前文件为准。
- 边界：只读审计。**未编译、未加载模块、未跑 QEMU**；所有结论来自源码与文档的静态对照。
- 结论强度：标“实测”的引用来自 `review/findings.md`、`adversarial/*.log`、`benches/` 的既有记录；本次没有复跑。

## 0. 一页结论

1. **用户列的六个名字里，没有一个是“解析后真正生效”的**：`framing=ext`、`chunk=`、`max_retry`、`dev=`、`uid=` 都只经过 `tbt_parse.c` 的解析与渲染，数据路径从不读取；`src=` 根本不是键。建议 0.0.1 **从语法中删除**这五个键，`src=` 只改文档与 adversarial 期望。理由：保留一个“写了不报错、也不生效”的键，比拒绝它更危险；`echo` 的成功就是操作员唯一的反馈。
2. **`srcaddrs=` 只有第 0 项被读取**（近端载体绑定，`tbt_carrier.c:1098`），第 1..7 项只保存/渲染；远端的 `srcaddrs` 完全不参与监听，监听绑定的是 `listen=`（`tbt_listen.c:101-102`），与 DESIGN §7.2 的“far side binds the first srcaddr”相反。
3. **`portblock=` 完全无人读取**：只在 `tbt_parse.c:571-575` 解析、`:915` 渲染；全局端口段只由 `/ports` 的 `tbt_ports` 决定（`tbt_session.c:31-35`）。它与 review 25 的 `0-0` 混淆直接相关。
4. **`max_tunnels` / `max_carriers` / `netns_optin` 三个模块参数都是装饰性的**：前两个只在 `tbt_core.c:114-123` 做加载时自检，从不约束 `tbt_tunnel_add()` / `tbt_cfg_sane()`；`tbt_netns_optin()` 没有任何调用者，`/proc/net/tbtun` 只在 `init_net.proc_net` 创建（`tbt_procfs.c:661`）。
5. **`config` 读有两个独立缺陷**：固定 32 KiB 缓冲的静默截断（`tbt_procfs.c:37`、`:214-230`）和一次性读（`:238-239`）。修复方向是“按实际长度精确分配 + 在 open file 内缓存 + 按 offset 服务”；任何放不下都必须返回 `-E2BIG`，不能返回短文件。
6. **`ports 0-0` 与 DESIGN §8.4.1 没有直接矛盾**：§8.4.1 那句的主语是 `portblock=`（`:1195-1196`），不是 `/ports`。真正的矛盾在 `tbt_parse.c:230` 的共享注释。代码拒绝 `ports 0-0` 是正确的（全局端口段不能为空），应改注释与文档，不应放宽。
7. **空行写 `/ports` 返回 `-EINVAL` 是调用方的错**：`tbt_parse_ports()` 对空行返回 0 且 `lo=hi=0`（`tbt_parse.c:809-810`），`tbt_ports_write()` 却把 0/0 传给 `tbt_portmap_set()`，后者以 `!lo` 拒绝（`tbt_session.c:65`）。把 0/0 当 no-op 即可。
8. **版本统一的唯一权威是 `tbtun/tbt_uapi.h:30-32`**；`MODULE_VERSION`、`dkms.conf`、`dkms.tar.gz`、`install_dkms.sh` 全部派生。但把 `TBT_VERSION_MAJOR` 从 1 改成 0 会同时改变 **HELLO body[0]**（`tbt_carrier.c:1197`、`tbt_rx.c:450`），这是 wire 可见变化。建议新增独立的 `TBT_WIRE_VERSION 1`，把打包版本与线上版本解耦；否则必须在 CHANGELOG 声明 0.0.1 与预发布 1.0.0 构建不互通。

---

## 1. 键/参数契约总表

“读取位置”只列数据路径或状态机的读取；`tbt_parse.c` 自己的渲染不算读取。行号均为当前工作树。

| 键/参数 | 解析位置 | 保存字段 | 读取位置 | 文档声明 | 结论 | 验收命令（静态 / 运行时） |
|---|---|---|---|---|---|---|
| `framing=ext` | `tbt_parse.c:566-570`；`tbt_parse_framing()` `:276-288` | `struct tbt_cfg.framing`（`tbt_parse.h:69`） | **无**；仅 `tbt_render_cfg()` `:914` 读它来渲染 | DESIGN §3.4 `:461-465` 说“parses but the mode is deferred, not implemented”；§8.3 例子 `:1062`、§8.4 顺序 `:1155`；README.md `:160`、README.zh.md `:141` 仍把它列成 `len\|ext`；§17.4 `:2168` 还建议“Try `framing=ext`” | **删除**（推荐）；若坚持保留，必须实现并有 wire 测试 | `grep -rn 'cfg\.framing\|TBT_FRAMING' tbtun/` 应为空；`printf 'tunnel 1 role=near listen=127.0.0.1:9000 peer=127.0.0.1:9001 framing=ext\n' > /proc/net/tbtun/config; echo $?` 应为非 0（未知键 `EINVAL`） |
| `chunk=` | `tbt_parse.c:516-520` | `u32 chunk`（`tbt_parse.h:72`） | **无**；发射端按记录长度编码（`tbt_http.c:1082` 的 `rec_len`；PAD 用 `TBT_HTTP_PAD_DATA_LEN`，`:768`），`chunk=` 不参与 | DESIGN §8.3 `:1120-1122` 明说“accepted and **ignored**”；README.md `:150` 写“Chunk-data bytes”；README.zh.md `:131` 同 | **删除** | `grep -rn 'cfg\.chunk' tbtun/` 应为空；QEMU 写 `chunk=1024` 应 `EINVAL` |
| `max_retry` | `tbt_parse.c:501-510` | `u32 max_retry`（`tbt_parse.h:63`） | **无**；重连每 200 ms 重试一次，由 50 ms 的 `w_poll` 驱动（`tbt_carrier.c:233-238`） | DESIGN §6.1 `:918-921` 说“accepted … **not used**”；§8.9 `:1477-1478` 又把它列为 config 写入；README.md `:148` 写“Carrier reconnect attempts, -1 = forever”；README.zh.md `:129` 同 | **删除** | `grep -rn 'max_retry' tbtun/` 应只剩文档；QEMU 写 `max_retry=3` 应 `EINVAL` |
| `dev=` | `tbt_parse.c:586-590`；`tbt_parse_dev()` `:396-403` | `char dev[16]`（`tbt_parse.h:83`） | **无** | DESIGN §7.2 `:946` 说“`SO_BINDTODEVICE` optional”；§13 `:1807` 说“parsed and ignored”；README.md `:164` 写“Interface to bind carriers to”；README.zh.md `:145` 同 | **删除**（`srcaddrs[0]` 已覆盖源地址选择）；若要保留则实现 `SO_BINDTODEVICE` 并测 | `grep -rn 'cfg\.dev' tbtun/` 应为空；QEMU 写 `dev=eth0` 应 `EINVAL` |
| `uid=` | `tbt_parse.c:581-585`；`tbt_parse_uids()` `:361-394` | `u32 uid[8]`、`u8 nuid`（`tbt_parse.h:71,82`） | **无** | DESIGN §13 `:1803-1804` 明说“There is no per-app credential to check — the socket is created by the module, so `sk_uid` is meaningless”；README.md `:163` 写“uids allowed to use the tunnel”；README.zh.md `:144` 同 | **删除**（按构造不可能实现） | `grep -rn 'cfg\.nuid\|cfg\.uid' tbtun/` 应为空；QEMU 写 `uid=1000` 应 `EINVAL` |
| `src=` | **无解析点**；未知键在 `tbt_parse.c:591-594` 被拒 | **无** | **无** | DESIGN §13 `:1805-1808` 把它列为“planned … **not enforced**”；adversarial/README.md `:113` 与 findings F8 期望它存在 | **改文档/删测试期望**；不要新增这个键（`srcaddrs=` 已占名） | `printf 'tunnel 1 role=near listen=127.0.0.1:9000 peer=127.0.0.1:9001 src=10.0.0.0/8\n' > /proc/net/tbtun/config; echo $?` 应为非 0；adversarial 用例改成“`src=` 是未知键” |
| `srcaddrs=` | `tbt_parse.c:576-580`；`tbt_parse_srcaddrs()` `:323-359` | `__be32 srcaddrs[8]`、`u8 nsrc`（`tbt_parse.h:70,81`） | **仅第 0 项**：近端载体 bind 源地址（`tbt_carrier.c:1098`）。第 1..7 项无读取；远端监听绑定 `listen=`（`tbt_listen.c:101-102`），不读 `srcaddrs` | DESIGN §7.2 `:948-951` 说“the code binds **one** listener on the first `srcaddr`”；§8.3 例子 `:1059`、`:1064`；README.md `:162`、README.zh.md `:143` 写“local source addresses” | **收窄为 1 条 + 改文档**（近端源地址）；多地址/远端多监听要么实现，要么删掉多余条目 | `grep -rn 'srcaddrs\[' tbtun/*.c` 只应命中 `tbt_carrier.c:1098` 与解析器；QEMU 远端 `srcaddrs=127.0.0.2` 仍应绑定 `listen=` 的地址 |
| `portblock=` | `tbt_parse.c:571-575`；`tbt_parse_range()` `:231-250` | `u16 portblock_lo/hi`（`tbt_parse.h:65-66`） | **无**；全局分配器只看 `tbt_ports`（`tbt_session.c:486-514`） | DESIGN §8.3 `:1082-1084` 说“accepted and recorded per tunnel, but C1 has one global port block … Known gap”；§8.4.1 `:1195-1196` 把 `0-0` 说成“no per-tunnel block”；README.md `:161` 写“Source port block for carriers; 0-0 = none” | **删除**（推荐；同时消除 `0-0` 的文档歧义）；若保留则必须接进 `tbt_port_alloc` | `grep -rn 'portblock_' tbtun/*.c` 只应剩解析/渲染；QEMU 两条隧道写不同 `portblock` 仍共用 `/ports` 的全局段 |
| `max_tunnels` | `tbt_core.c:30-32`（module_param） | 全局 `unsigned int max_tunnels` | **仅加载自检** `:119-123` 与打印 `:140-142`；`tbt_tunnel_add()` 不查（`tbt_session.c:167-174`） | README.md `:68`“Maximum configured tunnels”；README.zh.md `:56`；DESIGN §8.4.2 `:1207`、§8.9 `:1473`、§13 `:1814`；adversarial/README.md `:135` 期望第 65 条被拒 | **实现** | adversarial `init.max_tunnels`；强化版见 §4 |
| `max_carriers` | `tbt_core.c:34-36` | 全局 `unsigned int max_carriers` | **仅加载自检** `:114-118`；`tbt_cfg_sane()` 只比 `TBT_MAX_CARRIERS_LIMIT`（`tbt_session.c:118`） | README.md `:69`；README.zh.md `:57`；DESIGN §8.9 `:1473`；adversarial/README.md `:138` 期望 `n=8` 在 `max_carriers=4` 下被拒 | **实现** | adversarial `init.max_carriers`；强化版见 §4 |
| `netns_optin` | `tbt_core.c:46-48` | 全局 `bool netns_optin` | `tbt_netns_optin()` `:90-92` **无调用者**；proc 目录只在 `init_net.proc_net` 创建（`tbt_procfs.c:661`） | README.md `:70`；README.zh.md `:59`；DESIGN §8.3 `:1123-1124` 说“per-netns when … `netns_optin=1`”；§8.4.2 `:1207`、§8.9 `:1474` | **删除**（全局隧道表 + init_net-only proc，per-netns 视图会泄漏/混淆）；文档改为“控制面只在 init_net” | `grep -rn 'netns_optin\|tbt_netns_optin' tbtun/` 应为空；`modinfo -p tbtun` 不再列出该参数 |
| `debug_loss`（对照项） | `tbt_core.c:42-44` | 全局 `bool debug_loss` | `tbt_action_apply()` 门控（`tbt_session.c:435-439`）返回 `-EPERM` | README.md `:71`；DESIGN §8.3 `:1099-1103`；§8.9 `:1473` | **保留**（已生效） | `echo 'carrier 0 drop_ppm 1' > peers` 在 `debug_loss=0` 下应为 `EPERM`；`stats` 首行 `debug_loss=0` |

### 1.1 为什么建议“删除”而不是“继续解析但忽略”

`tbt_parse.h:68` 有一句总注释：“Recorded for C2+; C1 stores and renders them but does not act on them.” 这句话现在已经不准确：`drip_ms`、`replay`、`tunnel_rate`、`max_inflate_milli`、`hole_budget_ms`、`rx_reorder_bytes`、`tx_retain_bytes`、`carrier_stall_ms` 都在数据路径有读取（见 `tbt_carrier.c`、`tbt_tx.c`、`tbt_rx.c`、`tbt_session.c` 的 `cfg.` 命中）。真正“只存不读”的只有本表列出的那几项。把“存了但不用”与“存了且生效”混在同一条注释里，正是操作员误判的来源。

删除的代价是可见的：`host/test_parse.c:818-842` 的 canonical 期望串、`maximal_cfg()` `:337-410`、README 两张键表、DESIGN §8.3/§8.4 的键序都要同步改。但这是一次性、可测试的改动；保留的代价是长期的“写了不生效”承诺。按 poka-yoke，错误应在写入时暴露，而不是在故障排查时才发现。

### 1.2 逐项理由（实现 vs 删除）

- **`framing=ext`：删除。** DESIGN §3.4 `:461-465` 自己承认“the mode is deferred, not implemented”，但 §17.4 `:2168` 又建议故障时用它，README 键表也把它列为可用值。保留一个“看起来能救场、实际不改任何字节”的键，会把一次可诊断的配置错误变成一次无声的故障排查。实现它需要接收端解析 chunk extension、两端协商（HELLO 没有 framing 位），并改 §3.1.1 的“same frame header in every profile”承诺；这不是 0.0.1 的 P3。删除后 `profile=raw` 仍是可用的逃生口。
- **`chunk=`：删除。** DESIGN §8.3 `:1120-1122` 已经写明“accepted and ignored”；实际发射端按 `rec_len` 编码（`tbt_http.c:1082`），`TBT_CHUNK_MAX` 只是上限。既然不生效，就不该出现在 canonical 配置里；否则 `cat config` 会让操作员以为当前 chunk 是 1024。
- **`max_retry`：删除。** DESIGN §6.1 `:918-921` 明说“not used”；载体重连每 200 ms 重试一次，由 50 ms 的 `w_poll` 驱动（`tbt_carrier.c:233-238`），`join_dup` 另有 20 轮上限（`:782-784`）。README.md `:148` 却把它描述成“Carrier reconnect attempts”，这是最危险的一种文档：操作员会以为设置 3 就能限制重连。若确实需要上限，应在 replacement loop 里实现并有测试；不要保留一个装饰键。
- **`dev=`：删除。** `srcaddrs[0]` 已经提供源地址选择（`tbt_carrier.c:1098`）；`SO_BINDTODEVICE` 是可实现的，但它对远端 listener 的语义（绑定到哪个接口）与 per-tunnel 配置并不清晰，而且当前没有任何测试。0.0.1 删除，等有明确需求再做；DESIGN §7.2 `:946` 的“optional”改成“not implemented”。
- **`uid=`：删除，且不应再实现成“允许列表”。** DESIGN §13 `:1803-1804` 已经说明模块自己创建 socket，`sk_uid` 没有应用含义；TCP 包不携带应用 uid。唯一能做到的是“按公共连接的源 IP 过滤”，那是 `src=` 的语义，不是 `uid=`。保留 `uid=` 就是在文档里承诺一个无法兑现的访问控制。
- **`src=`：不新增，改文档/测试。** 当前语法没有这个键（`tbt_parse.c:591-594` 拒绝未知键），DESIGN §13 `:1805-1808` 也明确说“planned … not enforced”。adversarial 的 `init.src_key`/`pub.srcfilter` 期望它存在（README `:113`、F8），这会把“未实现”误报成“安全缺陷”。要么把它改成“`src=` 被拒绝”的契约测试，要么在后续版本实现真正的 CIDR 允许列表；0.0.1 不要加一个半成品。
- **`srcaddrs`（额外）：收窄为 1 条 + 改文档。** 只有 `[0]` 被读取（`tbt_carrier.c:1098`），第 1..7 项和远端的整个列表都没有执行点；DESIGN §7.2 `:948-951` 说“far side binds the first srcaddr”，但 `tbt_listen_start` 绑的是 `listen=`（`tbt_listen.c:101-102`）。如果保留列表，就必须实现 per-carrier 源地址轮转或 per-srcaddr listener；0.0.1 收窄为 1 条最诚实。
- **`portblock`（额外）：删除或实现。** 没有任何读取点；DESIGN §8.3 `:1082-1084` 自己标成“Known gap”。保留它还会让 §8.4.1 `:1195` 的 `0-0` 句子继续误导 review 25。删除是最小的一致性修复；若要实现，必须把它接进 `tbt_port_alloc()`，并处理“同一端口段被多个隧道共享”的位图。

---

## 2. `config` 读写：精确位置与修复方向

### 2.1 32 KiB 静默截断

- 固定缓冲：`tbt_procfs.c:37` `#define TBT_CFG_RENDER_MAX 32768`。
- 渲染：`tbt_procfs.c:214-230`。`tbt_render_cfg()` 返回负值时 `break`（`:224-225`），函数返回已写入的 `off`（`:229`）。因此输出在**某一行的边界**被截断，但调用方无法区分“完整文件”与“前缀”。
- 读取：`tbt_procfs.c:232-251`。`kmalloc(32768)`（`:240`）、`tbt_config_render(buf, 32768)`（`:243`）、`out = min(len, n)`（`:244`）。`n` 是截断后的长度，读端看到的就是一个“完整”的短文件。
- 触发条件：默认 `max_tunnels=64`，`review/findings.md:440` 记录实测最大 canonical 行 642 字节（`host/test_parse.c:388` 只断言它不超过 `TBT_LINE_MAX`）；`64 × 642 = 41088 > 32768`。约第 52 条隧道开始丢失（`32768 / 642 ≈ 51.0`）。若按本审计删除 `uid=`/`dev=`/`chunk=`/`max_retry=`/`framing=`，最大行会缩短，但 `max_tunnels` 一旦调大（例如 128）仍然会触发；契约不能依赖默认值。
- 后果：`cat /proc/net/tbtun/config > /etc/tbtun.conf` 得到短备份；恢复时 `cat backup > config` 会**删除**备份里没有的隧道。DESIGN §8.4 `:1162-1167` 承诺“round-trips byte for byte”。

**修复方向（推荐，最小改动）**

1. 在 `struct tbt_cfg_file`（`tbt_procfs.c:40-43`）增加 `char *out; size_t out_len;`，在 `tbt_cfg_open()`（`:135-146`）初始化为 `NULL/0`。
2. 在 `tbt_cfg_write()` 开头使缓存失效：`kvfree(cf->out); cf->out = NULL; cf->out_len = 0;`。同一 fd 先写后读会重新渲染；别的 fd 写入不影响本 fd 的快照（快照语义，文档写清）。
3. 新增 `tbt_config_render_alloc(struct tbt_cfg_file *cf)`：
   - 持 `tbt_lock`，第一遍对每条隧道用 `char line[TBT_LINE_MAX]` 调 `tbt_render_cfg()`，累加长度；任何一行返回 `TBT_PARSE_E2BIG` 就整体返回 `-E2BIG`（可见失败，不静默截断）。
   - `kvmalloc(total, GFP_KERNEL)`；第二遍再渲染到精确大小的缓冲。两遍都在同一把 `tbt_lock` 下，列表不会在两遍之间变化。
   - 分配失败返回 `-ENOMEM`。
4. `tbt_cfg_read()` 改为按 offset 服务缓存：
   ```c
   if (*ppos < 0) return -EINVAL;
   if (!cf->out) { n = tbt_config_render_alloc(cf); if (n < 0) return n; }
   if (*ppos >= cf->out_len) return 0;
   out = min(len, cf->out_len - *ppos);
   if (copy_to_user(ubuf, cf->out + *ppos, out)) return -EFAULT;
   *ppos += out; return out;
   ```
   同时把 `size_t out` 改成 `ssize_t ret`，避免 `tbt_procfs.c:245-246` 把 `-EFAULT` 塞进 `size_t`。
5. `tbt_cfg_release()`（`:196-206`）里 `kvfree(cf->out)`。

**替代方案（不推荐）**：把 `config` 改成 `seq_file`。`tbt_procfs.c:211-213` 的注释已经说明障碍：`file->private_data` 被写侧的 line buffer 占用。可以用 `seq_open_private()` 把 line buffer 挂到 `m->private`，但 `config` 需要“一次读出一个一致快照”，而 `seq_file` 的 `start/next/show` 会在多次 `read()` 之间重新遍历；要保证快照仍需自己缓存。收益不大，改动面更大。

**边界**：如果 `max_tunnels` 允许任意大，精确分配的最坏大小是 `max_tunnels × ~1023`。建议在 `tbt_core.c` 的加载自检里给 `max_tunnels` 一个上限（例如 4096），或在渲染总长超过某个硬上限时返回 `-E2BIG`。这是资源上限，不是契约；二者选一，文档写清。

### 2.2 一次性读

- 位置：`tbt_procfs.c:238-239` `if (*ppos) return 0; // one-shot read; shells read to EOF`，以及 `:248` `*ppos += out`。
- 行为：第一次 `read(fd, buf, 4096)` 返回 4096，`*ppos=4096`；第二次 `read()` 直接返回 0。任何缓冲区小于渲染结果的读者（`dd bs=4096`、部分语言运行时、逐块校验工具）都只看到前 4 KiB，且没有错误。
- 与 32 KiB 截断的区别：32 KiB 是“渲染端丢数据”，一次性读是“传输端丢数据”。两者独立，必须都修；§2.1 的缓存方案同时解决二者。
- `cat` 通常用 128 KiB 缓冲，所以 `cat config > backup` 在 32 KiB 以内看起来正常；这正是缺陷长期未被发现的原因。

### 2.3 写侧的两个相关事实（不是本次要改的缺陷，但验收时要知道）

- 行缓冲上限是 `TBT_LINE_MAX=1024`（`tbt_parse.h:33`），不是 README 写的 511（`README.md:134`、`README.zh.md:115`）。review 29 已把 `TBT_LINE_MAX` 从 512 提到 1024，README 没跟上。
- `tbt_config_drain()` 在 `release()` 时拒绝“塞满且无换行”的尾巴（`tbt_procfs.c:114-125`），review 31 已修；不要再按 review 31 的旧描述改。

---

## 3. `ports` 边界：精确位置与修复方向

### 3.1 `ports 0-0`

- 解析：`tbt_parse_ports()` `tbt_parse.c:794-823`。`tbt_parse_range()` 允许 `0-0`（`:245-249` 只拒绝 `lo==0 && hi!=0`），随后 `tbt_parse_ports()` 在 `:820-821` 显式拒绝 `!*lo`，错误文本是“port must be 1..65535”。
- 测试：`host/test_parse.c:947` 明确钉住 `tbt_parse_ports("ports 0-0\n", …) != 0`。
- 文档：DESIGN §8.4.1 `:1195-1196` 的原文是“`portblock=lo-hi` is parsed, stored and rendered; `0-0` is the canonical spelling of ‘no per-tunnel block’”。主语是 **`portblock=`**，不是 `/ports`。DESIGN §8.3 `:1115` 对 `/ports` 只说“free/used bits of the port block”，没有 `0-0` 语义。
- 真正的矛盾：`tbt_parse.c:230` 的共享注释“`0-0` is the canonical spelling of ‘no block configured’”写在 `tbt_parse_range()` 上，而 `tbt_parse_ports()` 也用这个函数。注释让读者以为 `/ports` 也该接受 `0-0`。
- 以谁为准：**代码的拒绝是对的**。全局端口段不能为空：`tbt_port_alloc()` 直接用 `tbt_ports.lo..hi`（`tbt_session.c:486-514`），`0-0` 会让所有载体分配失败。`portblock=` 是 per-tunnel 的记录值，`0-0` 在那里才有“未设置”的意义。
- 修复：
  1. `tbt_parse.c:230` 注释改为：“`lo-hi`; `0-0` means ‘no per-tunnel block’ for `portblock=`. `tbt_parse_ports()` rejects it because the global block cannot be empty.”
  2. DESIGN §8.4.1 `:1195-1196` 保持（若保留 `portblock=`）；在 §8.3 `:1115` 补一句“`ports` requires `1..65535`; `0-0` is only valid for `portblock=`”。
  3. 若按本审计删除 `portblock=`，则删掉 §8.4.1 那句，并把 `tbt_parse_range()` 注释改成“`tbt_parse_ports()` requires `lo>=1`”。
- 验收：`echo 'ports 0-0' > /proc/net/tbtun/ports; echo $?` 非 0；`echo 'ports 41000-41015'` 为 0；`host/test_parse` 的 `ports 0-0 refused` 保持 PASS。

### 3.2 空行写 `/ports`

- 解析：`tbt_parse_ports()` 对空行返回 0，`*lo=*hi=0`（`tbt_parse.c:800-810`）。`tbt_parse.h:148-149` 的契约也是“A blank line leaves *lo and *hi zero and returns 0”。
- 调用方：`tbt_ports_write()` `tbt_procfs.c:256-274`。`:269-271` 只检查 `err`，随后 `:272` 无条件 `tbt_portmap_set(lo, hi)`；`tbt_portmap_set()` 在 `tbt_session.c:65` 以 `!lo` 返回 `-EINVAL`。
- 结果：`echo "" > /proc/net/tbtun/ports`（写入 1 字节 `\n`）失败；`printf '' > /ports`（0 字节）在 `:263` 也返回 `-EINVAL`。
- 修复（`tbt_procfs.c:263-273`）：
  ```c
  if (!len) return 0;                 /* zero-length write is a no-op */
  if (len >= sizeof(buf)) return -E2BIG; /* line too long, same as config */
  ...
  if (!lo && !hi) return len;         /* blank line: nothing to do */
  err = tbt_portmap_set(lo, hi);
  ```
  注意 `-E2BIG` 与 `-EINVAL` 的区别：长度超限是 `E2BIG`（与 `tbt_parse_ports()` 的 `TBT_PARSE_E2BIG` 一致），不是语法错误。
- 验收：`printf '\n' > /proc/net/tbtun/ports; echo $?` 为 0；`printf '' > /proc/net/tbtun/ports; echo $?` 为 0；随后 `cat /proc/net/tbtun/ports` 的 block 不变。

### 3.3 相邻缺陷：`tbt_portmap_set()` 不保留在用端口（建议同批修）

`tbt_portmap_set()`（`tbt_session.c:60-79`）分配一张全新的零位图并替换 `tbt_ports.used`（`:72-77`），但**没有把当前活跃载体的 `c->local_port` 标回新位图**。在传输中写 `ports`，`tbt_port_alloc()` 可能返回一个已被活跃载体绑定的端口，随后 `kernel_bind()` 以 `EADDRINUSE` 失败、载体重试。没有内存破坏，但一次控制面写入会扰动数据面。

修复二选一：
- **拒绝**：`tbt_portmap_set()` 在检测到任何 session 有活跃 carrier 时返回 `-EBUSY`，要求先 `flush`；文档写清“改端口段需要先停隧道”。
- **重建位图**：遍历 `tbt_tunnels` / `sess->car[]`，把落在新 `[lo,hi]` 内的 `local_port` 置位。需要 `tbt_lock` + `tbt_ports.lock` 的锁序；比拒绝复杂。

推荐第一种（poka-yoke：不允许在数据面上做会破坏 5 元组的操作），并在 `/ports` 的写错误里返回 `EBUSY`。

---

## 4. 资源上限：实现点与 adversarial 证明

### 4.1 `max_tunnels`

- 现状：`tbt_core.c:30-32` 定义；`:119-123` 只检查非零；`:140-142` 打印。`tbt_tunnel_add()`（`tbt_session.c:167-174`）直接 `kzalloc`，不计数。
- 实现点：`tbt_session.c:167-174`，在 `old = tbt_tunnel_find_locked(cfg->id)`（当前在 `:188`）之后、`kzalloc` 之前：
  ```c
  old = tbt_tunnel_find_locked(cfg->id);
  if (!old) {
      unsigned int n = 0;
      list_for_each_entry(t, &tbt_tunnels, node) n++;
      if (n >= READ_ONCE(max_tunnels)) return -ENOSPC;
  }
  ```
  先找 `old` 再计数，保证“在限额上重发同一条隧道行”仍然幂等成功（DESIGN §8.4.1 `:1187-1188`）。
- 声明：`tbtun.h` 目前只 `extern unsigned int debug;`（`:71`），需要加 `extern unsigned int max_tunnels, max_carriers;`（或提供 accessor）。`max_tunnels` 是 0444，读侧用 `READ_ONCE`。
- 错误码：建议 `-ENOSPC`（资源耗尽），不要复用 `-EINVAL`（语法错误）。`echo` 都会失败，但 `dmesg`/日志能区分。DESIGN §8.4.1 只承诺“非零 + 不部分应用”，没有规定 errno。
- 上限：`max_tunnels` 目前只要求 `>=1`。若按 §2.1 精确分配渲染缓冲，建议加载时限制 `max_tunnels <= 4096`，避免一次 `config` 读分配过大；文档写清。

### 4.2 `max_carriers`

- 现状：`tbt_core.c:34-36` 定义；`:114-118` 只做 `1..16` 自检；`tbt_cfg_sane()`（`tbt_session.c:106-143`）在 `:118` 只比编译期 `TBT_MAX_CARRIERS_LIMIT`。
- 实现点：`tbt_session.c:118` 改为同时检查模块参数：
  ```c
  if (!c->n || c->n + c->m > TBT_MAX_CARRIERS_LIMIT ||
      c->n + c->m > READ_ONCE(max_carriers)) { *err = -EINVAL; return false; }
  ```
  `max_carriers` 已在加载时限制为 `1..16`，所以这只会收紧，不会放宽。
- 语义：`n + m` 是会话的载体 socket 总数（`tbt_session.c:673` `s->n_car = t->cfg.n + t->cfg.m`），与 `MODULE_PARM_DESC(max_carriers)` 的“Carrier sockets per session”一致。
- 验收：`max_carriers=4` 下 `n=2 m=2` 接受，`n=2 m=3` 拒绝，`n=4 m=0` 接受，`n=5 m=0` 拒绝；`max_carriers=1` 下 `n=1 m=1` 拒绝。

### 4.3 adversarial 的证明方式（现状与强化）

现有用例：

- `init.max_tunnels`：`adversarial/init:205-216`。循环写 80 条不同的 far 隧道，`|| break`，然后 `grep -vc '^#' /proc/net/tbtun/tunnels`；`_n >= 80` 判 FAIL，否则 PASS。当前日志 `adversarial/console-resource.log:405`、`console-stable.log:432` 记录 FAIL。
- `init.max_carriers`：`adversarial/init:429-437`。`insmod … max_carriers=4`，写 `n=8 m=0`；写成功判 FAIL。当前日志 `console-param.log:409` 记录 FAIL。

这两个用例**能证明“参数没生效”**，但证明“参数生效”还不够强：

- `init.max_tunnels` 的 PASS 条件是 `_n < 80`，任何在第 65 条之前的无关失败（端口冲突、重复 id、其他键错误）都会让它 PASS。它用的是默认 `max_tunnels=64`，没有证明“值”被使用。
- `init.max_carriers` 的 `n=8` 同时受编译期 16 的上限约束；虽然 8 不会触发 16，但用例没有测边界（4 接受 / 5 拒绝），不能排除“恰好 8 被别的规则拒绝”。

建议的强化验收（两次加载，测边界与幂等）：

```sh
# max_tunnels: 用参数值本身证明
insmod tbtun.ko max_tunnels=2
echo 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=1 m=0' > /proc/net/tbtun/config
echo 'tunnel 2 role=far profile=raw listen=127.0.0.1:22002 target=127.0.0.1:5201 n=1 m=0' > /proc/net/tbtun/config
if echo 'tunnel 3 role=far profile=raw listen=127.0.0.1:22003 target=127.0.0.1:5201 n=1 m=0' > /proc/net/tbtun/config; then
    echo "FAIL: third tunnel accepted with max_tunnels=2"; fi
[ "$(grep -vc '^#' /proc/net/tbtun/tunnels)" -eq 2 ] || echo "FAIL: count != 2"
# 幂等替换：在限额上重发 id 1 必须成功
echo 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=1 m=0' > /proc/net/tbtun/config \
    || echo "FAIL: replacing an existing tunnel at the limit was refused"
rmmod tbtun

# max_carriers: 测 n+m 的边界
insmod tbtun.ko max_carriers=4
echo 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=2 m=2' > /proc/net/tbtun/config \
    || echo "FAIL: n+m=4 refused with max_carriers=4"
if echo 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=2 m=3' > /proc/net/tbtun/config; then
    echo "FAIL: n+m=5 accepted with max_carriers=4"; fi
if echo 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=5 m=0' > /proc/net/tbtun/config; then
    echo "FAIL: n=5 accepted with max_carriers=4"; fi
rmmod tbtun
```

注意：`echo` 到 proc 文件的写失败会让 shell 命令返回非零，但错误文本由内核决定；如果 `dmesg` 里没有原因，`tbt_config_apply()` 的 `pr_debug` 只在 `debug>=1` 时可见。验收时用 `debug=1` 加载，或让 `tbt_cfg_sane()` 的失败路径 `pr_info` 一行（只在拒绝时打印，不会刷屏）。

### 4.4 `netns_optin`

- 现状：参数 `tbt_core.c:46-48`；accessor `:90-92`；**无调用者**（`grep -rn 'tbt_netns_optin' tbtun/` 只有定义与声明）。proc 目录只在 `init_net.proc_net` 创建（`tbt_procfs.c:661`），exit 也从 `init_net.proc_net` 删除（`:694`）。
- 为什么建议删除而不是实现：
  1. 隧道表 `tbt_tunnels` 与 `tbt_lock` 是**全局**的（`tbt_core.c:23-24`），不是 per-netns。把同一个目录暴露到每个 netns，会让 netns A 看到、修改、`del` netns B 的隧道；DESIGN §8.4.2 `:1208-1209` 自己说“sysfs module params are per-module, not per-netns, so two netns cannot have different tunnels”，但当前的全局表同样做不到。
  2. 真正 per-netns 需要 `pernet_operations` + 按 `t->net` 过滤每一条 list walk（`tbt_tunnel_find_locked`、`tbt_tunnel_show`、`tbt_tunnel_shutdown`、`tbt_action_apply`、stats…），这是一次数据模型改造，不是 0.0.1 的 P3。
  3. 当前 `tbt_cfg_open()` 用 `current->nsproxy->net_ns`（`tbt_procfs.c:142`），而文件属于 init_net。一个子 netns 的进程通过 bind mount 打开 init netns 的 `/proc/net/tbtun/config` 时，隧道 socket 会被创建在子 netns，控制面却在 init netns——这是一个真实的跨 netns 错配。删除 `netns_optin` 时顺手把 `cf->net` 改成 `get_net(&init_net)`，与文件的归属一致。
- 文档：删除 README.md `:70`、README.zh.md `:59` 的参数行；DESIGN §8.3 `:1123-1124` 改为“All files are under `init_net.proc_net`; the control surface is init_net-only”；§8.4.2 `:1207`、§8.9 `:1474` 同步。
- 验收：`modinfo -p tbtun` 不再有 `netns_optin`；在子 netns 里看不到 `/proc/net/tbtun`（这是预期行为，不是缺陷）。

---

## 5. 规格漂移逐条

### 5.1 review 12：HELLO `profile|gen` 位布局

| 项 | 原文/代码 | 判定 |
|---|---|---|
| 代码发射 | `tbt_carrier.c:1202-1204`：`profile \| (gen & 0xf) << 2` → profile 在 bit 1:0，gen 在 bit 5:2 | — |
| 代码接收 | `tbt_rx.c:460-461`：`prof = body[22] & 3`，`gen = (body[22] >> 2) & 0xf` | — |
| DESIGN §3.1.1 `:225-232` | 图示 `[unused][ gen (4) ][ prof (2)]`，正文“bits 1..0 are the profile … bits 5..2 are the carrier's path generation … bits 7..6 are zero” | **与代码一致** |
| DESIGN §3.3 `:415-417` | “The last byte's low two bits are the profile and its **high four bits** are the carrier's path generation” → “high four bits”=7:4 | **与 §3.1.1 和代码矛盾** |
| `tbt_carrier.c:1188-1191` | 注释仍说“a zero flags byte (DESIGN.md §3.3)” | **陈旧**（该字节早已是 profile\|gen） |
| `tbt_uapi.h:119-122` | 注释只写 `u8 profile`，没写 gen 位 | **不完整**（不致命，但会让读者以为 gen 不在这个字节） |
| review 12 原文 | `review/findings.md:307-314` 说 DESIGN 把 gen 放在 7:4、且 `TBT_CONV` 未 bump | **评审快照已过期**：`TBT_CONV` 已是 `0x0003`（`tbt_uapi.h:44`、DESIGN `:232`/`:385`），§3.1.1 已改成 5:2 |

**以谁为准**：代码 + DESIGN §3.1.1。两者一致，且 `TBT_CONV=0x0003` 已经承载这个布局（`tbt_uapi.h:39-44` 的注释写明“0x0003 carries … the HELLO profile|gen byte”）。

**动作**：
1. 改 DESIGN §3.3 `:416`：“high four bits” → “bits 5..2”；补一句“bits 7..6 are zero”。
2. 改 `tbt_carrier.c:1188-1191` 的注释：删除“zero flags byte”，写成“body[22] = profile | (gen << 2)”。
3. 补 `tbt_uapi.h:119-122` 的注释：写明 profile 在 1:0、gen 在 5:2、7:6 为零。
4. **不要** bump `TBT_CONV`：布局没有变，只是文档对齐；review 的“未 bump”是旧快照。
5. 加一个 host 测试钉住布局（当前 `host/test_fuzz.c` 只钉长度 23，不钉 body[22]）：构造 `gen=0xf, profile=3`，断言 `body[22] == 0x3f`（`(0xf<<2)|3`），并断言 `(body[22]>>2)&0xf == 0xf`、`body[22]&3 == 3`。这能防止未来有人按 §3.3 的旧文改代码。

**验收**：`grep -n 'high four bits' DESIGN.md` 为空；`grep -n 'zero flags byte' tbtun/tbt_carrier.c` 为空；新增的 host 测试 PASS。

### 5.2 review 26：载体 `SO_REUSEADDR` 与 DESIGN §7.2

| 项 | 原文/代码 | 判定 |
|---|---|---|
| 代码（载体） | `tbt_carrier.c:1126-1128`：`sock_set_reuseaddr(s->sk)` 在 `kernel_bind()`（`:1127`）之前 | — |
| 代码（监听） | `tbt_listen.c:130-132`：同样在 `kernel_bind()`（`:132`）之前设置，注释解释“tunnel line that replaces an existing one rebinds the same port while the old carrier connections are still in TIME_WAIT” | — |
| DESIGN §7.2 `:943-947` | “`SO_REUSEADDR` off (a restart must not silently steal another session's port)” | **与代码相反** |
| DESIGN §7.2 `:952-953` | “Allocation retry: if the chosen port is in `TIME_WAIT` … advance to the next free bit” | 承认 TIME_WAIT 是现实问题 |
| 代码注释自相矛盾 | `tbt_carrier.c:903-905` 说“The new port is allocated while the old carrier still holds its own, so the replacement never reuses the dead 4-tuple”；`:1115-1116` 却说“The replacement carrier often reuses the port the dead one just freed” | 两处必有一处过时 |

**以谁为准**：代码。理由：
- `tbt_listen.c:130-132` 的注释记录了一个实测问题：替换隧道行时，旧载体连接还在 TIME_WAIT，不设 REUSEADDR 会 `EADDRINUSE` 一分钟（probes C/D）。
- 端口来自**保留块**（默认 40000-40063，DESIGN §7.2 `:936-942`），模块用位图独占分配（`tbt_session.c:486-514`）；`SO_REUSEADDR` 在这里的作用是允许重绑 TIME_WAIT 的同一 4 元组，而不是从无关的临时端口里“偷”一个。
- DESIGN §7.2 的“SO_REUSEADDR off”写在“Bind sequence”条目里，针对的是“restart must not steal another session's port”；实际风险应由保留块 + 位图来消除，而不是靠关掉 REUSEADDR。

**动作**：
1. 改 DESIGN §7.2 `:945-946`：“`SO_REUSEADDR` **on** for the carrier and listener binds: a replaced tunnel line or a reloaded module rebinds the reserved block while old connections sit in `TIME_WAIT`. The reserved block is excluded from ephemeral allocation and the module's bitmap prevents two live carriers from sharing a port, so this cannot steal an unrelated session's port.”
2. 改 `tbt_carrier.c:1126-1127` 的注释，使它和 `:903-905` 一致：替换载体本身分配新端口；REUSEADDR 是为**隧道行替换/模块重载**时重绑保留块服务的。
3. 不改代码；不需要 `TBT_CONV`。

**验收**：`grep -n 'SO_REUSEADDR' DESIGN.md` 只剩“on”的表述；QEMU 里在传输中重发同一条 tunnel 行，监听/载体都能重新绑定（probes C/D 已覆盖）；`dmesg` 无 `EADDRINUSE` 风暴。

### 5.3 review 35：死代码与未用键

| 项 | 代码证据 | 结论 |
|---|---|---|
| `cfg.framing` | 只在 `tbt_parse.c:566-570` 解析、`:914` 渲染 | 删除（见 §1） |
| `cfg.chunk` | 只在 `:516-520` 解析、`:907` 渲染 | 删除 |
| `cfg.max_retry` | 只在 `:501-510` 解析、`:906` 渲染 | 删除 |
| `cfg.uid` / `cfg.nuid` | 只在 `:581-585`、`:361-394`、`:931-942` | 删除（不可实现，见 §1） |
| `cfg.dev` | 只在 `:586-590`、`:396-403`、`:943-948` | 删除 |
| `cfg.srcaddrs[1..7]` | 唯一读取是 `tbt_carrier.c:1098` 的 `[0]`；`:918-930` 渲染全部 | 收窄为 1 条或实现多地址 |
| `cfg.portblock_lo/hi` | 只在 `:571-575` 解析、`:915` 渲染 | 删除或接进分配器 |
| `tbt_passive_port()` | 声明在 `tbtun.h:98`，**没有定义、没有调用** | 删除声明 |
| `tbt_tunnel_flush()` | 声明在 `tbtun.h:111`，**没有定义、没有调用** | 删除声明 |
| `tbt_netns_optin()` | 定义 `tbt_core.c:90-92`，声明 `tbtun.h:97`，**无调用** | 随 `netns_optin` 一起删除 |
| `tbt_http_skip_request_header()` | 定义 `tbt_http.c:696`，模块内无调用；但 `host/test_http.c:1432-1476` 的 `test_skip_header` 覆盖它 | **不是死代码**（测试 API）；保留，或在删测试的同时删除 |
| `TBT_VERSION` 宏 | 定义 `tbt_uapi.h:33`，全仓库无使用 | 删除，或改成 `TBT_WIRE_VERSION` 的派生（见 §6） |
| `tbt_carrier_profile(…, u8 index, …)` | `tbt_carrier.c:49-57` 里 `(void)index;` | 删参数或使用它（spare 判定已经由 `spare` 参数完成） |

**动作**：按上表删除；`host/test_parse.c` 的 canonical 串、`maximal_cfg()`、`README` 两张键表、DESIGN §8.3/§8.4 的键序同步。删除后 `tbt_parse.h` 的 `TBT_SRC_MAX`/`TBT_UID_MAX`/`TBT_IFNAMSIZ`/`TBT_FRAMING_*` 也要一并清理（若 `srcaddrs` 收窄为 1，`TBT_SRC_MAX` 改为 1 或删掉数组）。

**验收**：
```sh
grep -rn 'cfg\.\(framing\|chunk\|max_retry\|uid\|dev\)' tbtun/    # 应为空
grep -rn 'tbt_passive_port\|tbt_tunnel_flush' tbtun/             # 应为空
grep -rn 'tbt_netns_optin' tbtun/                                # 应为空
grep -rn '\bTBT_VERSION\b' tbtun/                                # 应为空（或只剩新定义）
make host-test                                                   # 期望串同步后全绿
```

### 5.4 review 15：`fbk_mask` 未用

- 定义：`tbt_tx.c:897` `void tbt_tx_hole_check(struct tbt_session *sess, u16 fbk_mask)`。
- 唯一调用：`tbt_rx.c:310` `tbt_tx_hole_check(sess, hdr->path_ack_mask);`。
- 函数体：`tbt_tx.c:897-1098` 从不读 `fbk_mask`；`grep -n fbk_mask tbtun/tbt_tx.c` 只有形参那一行。
- 函数头注释 `tbt_tx.c:892-896` 说“The gate is the important part: the FBK must cover every path the frame used…”，但实际门控用的是每载体的持久 `uack`/`recv_cnt`（`:845-880` 的 `sent_below`/`lag` 计算），不是本次 FBK 的 mask。
- DESIGN §4.3 `:580-583` 描述的门控是：“the sender's writes on `p` below the peer's high-water for `p`, minus the peer's `recv_cnt` for `p`, is greater than zero — and every other live path's `uack` is ≥ `q`.” **没有要求 FBK mask 覆盖所有路径**。所以设计已经改成持久状态，形参是旧设计的残留。

**以谁为准**：DESIGN §4.3 + 当前代码。**动作**：删掉 `fbk_mask` 形参、`tbt_rx.c:310` 的实参、`tbtun.h:576` 的声明，并把 `tbt_tx.c:892-896` 的注释改成实际门控（per-path `uack`/`recv_cnt`，不依赖本次 FBK 的 mask）。不需要 `TBT_CONV`，行为不变。

**验收**：`grep -rn fbk_mask tbtun/` 为空；QEMU probe G（1% 丢包）`holes_declared > 0`、`holes_repaired == holes_declared`、`holes_unrecoverable=0` 与改动前一致。

### 5.5 相邻漂移（不在 review 12/26/35/15，但属于同一张契约）

| 位置 | 现状 | 文档原文 | 建议 |
|---|---|---|---|
| `DESIGN.md:415-417` | 见 §5.1 | “high four bits” | 改成 bits 5..2 |
| `DESIGN.md:1473-1474` | 代码只有 `debug, max_tunnels, max_carriers, debug_loss, netns_optin`（`tbt_core.c:24-48`） | 还列了 `default_replay_bytes`、`max_replay_bytes`、`fallback_redirect` | 改成实际的五个参数；删除幻影参数 |
| `DESIGN.md:1477-1479` | `max_retry` 未用；proc 只在 init_net，隧道表全局 | 说 `max_retry` 是 config 写入、可 per-netns | 删 `max_retry`；改成“init_net-only，全局表” |
| `DESIGN.md:1120-1122` | 发射端按记录长度编码（`tbt_http.c:1082` 的 `rec_len`；PAD `:768`），`TBT_CHUNK_MAX` 只是上限 | 说“the emitter always uses `TBT_CHUNK_MAX`” | 改成“the emitter sizes each chunk from the record (`rec_len`), bounded by `TBT_CHUNK_MAX`”；删除 `chunk=` 后这句只保留常量说明 |
| `DESIGN.md:1116-1119` | `tbt_parse_profile()` 只接受 `http-kal`/`http-kal-drip`/`raw`（`tbt_parse.c:252-263`）；spare 的 drip 是自动的（`tbt_carrier.c:49-57`） | 说可以用 `profile=m-spare-http-kal-drip` | 删掉这个伪值，写“`http-kal` 的 `M` 个 spare 自动使用 drip 节奏” |
| `DESIGN.md:1778-1783` | HELLO `body[0]` 用 `TBT_VERSION_MAJOR` 并在 `tbt_rx.c:450` 精确比较 | 当前 §11 已改成：当前 1.0.0、HELLO body[0] 是第二兼容轴、解耦计划见 §6.5、未实施 | 文档已对齐；代码改动等 §6.5 执行 |
| `DESIGN.md:1814-1816` | 没有 `max_streams`、`tx_queue_depth`；`max_tunnels` 未执行 | 说资源上限有 `max_tunnels`、per-tunnel `max_streams`、per-session `replay_bytes`/`tx_queue_depth` | 实现 `max_tunnels` 后改文；删掉不存在的上限，或标明“planned” |
| `README.md:134` / `README.zh.md:115` | `TBT_LINE_MAX=1024`（`tbt_parse.h:33`），最大行 642（`review/findings.md:440` 实测；`host/test_parse.c:388` 断言不超过 `TBT_LINE_MAX`） | 说“超过 511 字节 … `E2BIG`” | 改成 1023（`TBT_LINE_MAX-1`） |
| `README.md:256-259` / `README.zh.md:231-233` | 与键表冲突：键表说这些键有语义，限制段说“parsed but not effective” | — | 删除键表里的行，限制段只保留“已删除”说明 |
| `tbt_parse.h:68` | `drip_ms`/`replay`/`tunnel_rate`/`max_inflate_milli`/`hole_budget_ms`/`rx_reorder_bytes`/`tx_retain_bytes`/`carrier_stall_ms` 都有读取 | 注释说“C1 stores and renders them but does not act on them” | 改成逐字段注释；只对真正未用的字段标注 |
| `tbt_carrier.c:1188-1191` | 见 §5.1 | 注释说“zero flags byte” | 改注释 |
| `tbt_uapi.h:119-122` | 见 §5.1 | 注释只写 profile | 补 gen 位 |
| `tbt_uapi.h:33` | `TBT_VERSION` 无使用 | — | 删，或用于 `TBT_WIRE_VERSION` |

---

## 6. 版本统一为 `0.0.1`：逐处清单

### 6.1 权威与派生链

```
tbtun/tbt_uapi.h:30-32  (TBT_VERSION_MAJOR/MINOR/PATCH)
        ├── tbtun/tbt_core.c:140-142   pr_info("tbtun %u.%u.%u loaded …")
        ├── tbtun/tbt_core.c:172-173   MODULE_VERSION(__stringify(...))
        ├── scripts/mkdkmsconf.sh:27-59 version_field()/VERSION 读这三个宏
        │       ├── dkms.conf:2                    (生成物)
        │       ├── dkms.tar.gz 的 dkms_source_tree/dkms.conf (生成物)
        │       └── scripts/install_dkms.sh:147-150 (读 mkdkmsconf --version 的输出)
        └── tbtun/tbt_carrier.c:1197   body[0] = TBT_VERSION_MAJOR  ← 线上可见！
            tbtun/tbt_rx.c:450         body[0] != TBT_VERSION_MAJOR → -EPROTO
```

`TBT_CONV`（`tbt_uapi.h:44`）是独立的 wire revision，当前 `0x0003`，不要因为打包版本变化而改它。

### 6.2 需要改/需要重新生成的位置

**先看两点，避免被 `grep 1.0.0` 误导：**

- 真正的版本源是 `tbtun/tbt_uapi.h:30-32` 的三个宏（`1`/`0`/`0`），文件里**没有**字面量 `1.0.0`；只 grep `1.0.0` 会漏掉它。
- `grep -rn '1\.0\.0'` 还会命中 `DESIGN.md:1589` 的 User-Agent `Chrome/131.0.0.0`，以及 `.pi/*` 的会话元数据；这些不是版本号，不要替换。


| 位置 | 当前值/行为 | 是否手改 | 动作 |
|---|---|---|---|
| `tbtun/tbt_uapi.h:30-32` | `1` / `0` / `0` | **是** | 改成 `0` / `0` / `1`。这是唯一权威。 |
| `tbtun/tbt_uapi.h:33` | `TBT_VERSION` 宏，无使用 | 可选 | 保留（派生值自动变 1），或删掉；不要手写版本号。 |
| `tbtun/tbt_core.c:172-173` | `MODULE_VERSION` 由宏 stringify | 否 | 重新编译后 `modinfo -F version tbtun.ko` 应为 `0.0.1`。 |
| `tbtun/tbt_core.c:140-142` | 加载日志由宏派生；当前二进制为 `tbtun 0.0.1 loaded` | 否 | 当前宏为 0/0/1；`modinfo -F version tbtun.ko` 也是 0.0.1。 |
| `dkms.conf:2` | `PACKAGE_VERSION="0.0.1"` | **否（生成物）** | `make dkms.conf` 或 `make dkms-tarball` 重新生成；默认纯语义版本，`TBT_DKMS_GIT_SUFFIX=1` 才加 `.r<rev>.<sha>`。该文件被 `.gitignore:56`/`:260` 忽略，不要提交。 |
| `dkms.tar.gz` 的 `./dkms_source_tree/dkms.conf:2` | 同上 | **否（生成物）** | `make dkms-tarball`；`.gitignore:261` 忽略。归档里的 `Makefile`/`tbtun/*` 也会随之更新。 |
| `DESIGN.md:1804` | 当前 0.0.1 release candidate；HELLO `body[0]` 用 `TBT_WIRE_VERSION=1`，wire 已解耦 | **已完成（文档）** | 最终回归见 §16.5。 |
| `review/baseline/01-env-sha.txt:72` | `version: 1.0.0`（`modinfo` 输出） | **否（生成物）** | 重新构建后重新采集该 baseline（或删掉旧值，由下一次 baseline 覆盖）；不要手改。 |
| `adversarial/console-*.log`（16 个）与 `crash-console.log` | 每份都有 `tbtun 1.0.0 loaded` | **否（证据快照）** | 重跑 adversarial 后自然变成 0.0.1；不要手改历史证据。 |
| `tbtun.ko` | 当前二进制 `modinfo -F version` = `0.0.1` | **否** | 重新编译后仍应为 0.0.1；不要手改二进制。 |
| `.github/workflows/release.yml:22-27` | 用 `mkdkmsconf.sh` 的输出与 tag 比较 | 否 | tag 必须是 `v0.0.1`，且 `mkdkmsconf.sh` 在该 commit 上必须输出 `0.0.1`（`git describe --tags --exact-match` 成功）；否则会输出 `0.0.1.rN.hash`，tag 检查失败。 |
| `scripts/mkdkmsconf.sh:27-59` | 从 `tbt_uapi.h` 读版本，默认纯语义版本 | 否 | 不改；解析失败会报错退出，不再静默加 `.r` 后缀。 |
| `scripts/install_dkms.sh:147-150` | 从 `mkdkmsconf.sh --version` 读版本 | 否 | 不改；DKMS stage 会变成 `/usr/src/tbtun-<version>`。 |
| `flake.nix:11` | `version = self.shortRev or …` | 否 | 不是模块版本；不改。 |
| `README.md` / `README.zh.md` | 没有版本字符串 | 否 | 不用改；若要加“0.0.1”，放在标题下，但不要与 `MODULE_VERSION` 形成第二个权威。 |
| `.pi/*` | 会话元数据里提到 1.0.0/0.0.1 | 否 | 不是交付物；忽略。 |

### 6.3 关键副作用：`TBT_VERSION_MAJOR` 在 HELLO 线上

旧实现（0.0.1 cutover 前）把 `TBT_VERSION_MAJOR` 写进 HELLO body[0]；现在已改为 `TBT_WIRE_VERSION=1`（`tbt_carrier.c:1289`、`tbt_rx.c:483`），`TBT_CONV=0x0004` 由 P2c 保持。模块宏从 1/0/0 改成 0/0/1 不再改变 HELLO 字节；旧 1.0.0 对端的 `body[0]=1` 仍被接受。

两个选项：

- **推荐：解耦**。在 `tbt_uapi.h` 新增 `#define TBT_WIRE_VERSION 1`（放在 `TBT_CONV` 旁边），`tbt_carrier.c:1197` 与 `tbt_rx.c:450` 改用它；`TBT_VERSION_MAJOR` 只用于打包/日志。这样 0.0.1 的线上字节与当前构建完全一致，`TBT_CONV` 仍是唯一 wire 轴。
- **备选：接受耦合**。保持 `body[0] = TBT_VERSION_MAJOR`，在 CHANGELOG 明确写“0.0.1 的 HELLO ver=0，与预发布 1.0.0 构建不互通；首个正式版不承诺与预发布互操作”。这更简单，但 DESIGN §11 的“only conv”必须改掉。

无论选哪个，`host/test_fuzz.c` 只钉 HELLO 长度 23，不钉 body[0]；建议加一个 host 测试钉住 `TBT_WIRE_VERSION`，防止打包版本变化再次悄悄改 wire。

### 6.4 CHANGELOG 应覆盖的内容来源

当前没有 `CHANGELOG.md`。0.0.1 的首个条目建议从以下来源取，**不要**直接抄 review 的旧快照（review 12/26/29/31 等已过期）：

| CHANGELOG 小节 | 来源 | 取什么 |
|---|---|---|
| 首个版本的范围 | `README.md:20-27`、`DESIGN.md` §15（实现顺序 C1-C8）、§2 | 全内核端点、N+M 载体、`http-kal`/`http-kal-drip`/`raw`、replay/dup/predict、RETIRE 替换、`/proc` 控制面 |
| 删除的旧实现 | `DESIGN.md` §11 `:1735-1747`、`git status` 的 `D brutal*.c`/`D tools/`/`D example/` | brutal CC、`brutalctl`、Python 示例全部移除；不再有用户态程序 |
| 已修复的缺陷 | `review/findings.md:26-60` 的 resolution 表（1-11、17、27-29、31、33、36-39）；`adversarial/findings.md` 的 F1/F2/F7 状态；`交接.md` §8 | 只写“当前代码已修”的项；F1 在 `交接.md:334-338` 仍写“没有复跑”，不要写成已验收 |
| 已知限制 | `README.md:245-260`、`README.zh.md:220-234`、`交接.md` §8、`DESIGN.md` §19 | 单公共连接（F4）、半关闭 EOF（F5）、`retx_ppm`、`flush` 阻塞、`n=1` 不适用、无 IPv6、无加密、PREDICT 接收端惰性（review 13） |
| 性能 | `benches/README.md`、`benches/results/*.csv`、`README.md:224-241` | 只写“实验室数字”；注明 virtio-net + htb + netem 条件；不要把 QEMU/slirp 绝对值当产品指标 |
| 协议/兼容性 | `DESIGN.md` §3.1.1/§3.3、`tbt_uapi.h:39-44` | `TBT_CONV=0x0003`；HELLO body 布局；0.0.1 的 HELLO ver 决策（§6.3） |
| 打包/安装 | `Makefile` 的 `dkms.conf`/`dkms-tarball` 目标、`scripts/mkdkmsconf.sh`、`scripts/install_dkms.sh`、`README.md:36-60` | DKMS 安装/卸载、`min kernel 6.1`、`pahole` 依赖、`dkms.tar.gz` 的生成方式 |
| 测试证据 | `host/test_parse.c`、`host/test_fuzz.c`、`host/test_http.c`、`tests/qemu/run.sh`、`adversarial/run.sh`、`review/lint.sh` | 哪些套件在 0.0.1 上跑过；不要把未复跑的项写成 PASS |
| 版本/日期 | 本文件 §6、`git log` | 0.0.1 的日期、commit、`TBT_CONV`；如果选了 §6.3 的“解耦”，写明 wire ver 未变 |

**写法约束**：CHANGELOG 只陈述**可观察的行为**和**已验证的证据**；不要写“支持 `framing=ext`”“`uid=` 允许列表”这类当前不生效的承诺。若 P3 按本审计删除了这些键，CHANGELOG 的“Removed/Changed”应明确写“removed inert keys `framing=`, `chunk=`, `max_retry=`, `dev=`, `uid=`; `src=` was never a key”。


### 6.5 执行计划（2026-11-05：wire 解耦与 0.0.1 宏已完成；最终回归待做）

目标：把“模块版本”和“HELLO wire ver”解耦，使模块版本可以独立走到 `0.0.1`，而线上字节不变。

> **Status 2026-11-05:** 步骤 1-5 已实施（`TBT_WIRE_VERSION=1`、HELLO `body[0]` 使用它、模块宏 `0/0/1`、`dkms.conf` 重新生成为 `0.0.1`）。步骤 6 的最终 QEMU/回归与 §16.5 的收尾检查仍未完成。`TBT_CONV` 由 P2c 保持在 `0x0004`，本次 cutover 没有改它。

1. `tbtun/tbt_uapi.h`：在 `TBT_CONV` 旁新增 `#define TBT_WIRE_VERSION 1`；`TBT_CONV` 保持 P2c 的 `0x0004`（当前 `:46`）。
2. `tbtun/tbt_carrier.c` HELLO：`body[0] = TBT_WIRE_VERSION`（当前已实现，`:1289`）。
3. `tbtun/tbt_rx.c`：`body[0] != TBT_WIRE_VERSION`（当前已实现，`:483`）。
4. `tbtun/tbt_uapi.h`：`TBT_VERSION_MAJOR/MINOR/PATCH` 改为 `0/0/1`（已完成，`:30-32`）。
5. `MODULE_VERSION` 自动变成 `0.0.1`（`tbt_core.c:172-173`）；`dkms.conf`/`dkms.tar.gz` 用 `make dkms-tarball` 重新生成——`scripts/mkdkmsconf.sh:27-59` 从 `tbt_uapi.h` 的三个宏派生纯语义版本，`Makefile:160-162`/`:180-186` 已有目标，不要手改生成物。
6. 测试：host test 钉住 HELLO `body[0] == TBT_WIRE_VERSION`（当前 `host/test_fuzz.c` 只钉长度 23）；QEMU 两端跑 `init.rmmod`、`cat.cfg_self`、`init.cfg_roundtrip`；`modinfo -F version tbtun.ko` 应为 `0.0.1`；`dkms.conf` 的 `PACKAGE_VERSION` 应为 `0.0.1`；旧 `1.0.0` peer 的 HELLO `body[0]=1` 仍应被接受（wire 字节未变）。
7. **不 bump `TBT_CONV`**：保持 P2c 的 `0x0004`；`body[0]` 仍是 `1`，帧布局未变；`TBT_CONV` 只在帧布局变化时 bump。
8. 若不做解耦（历史备选，未采用）：`0.0.1` 的 `body[0]=0`，预发布 `1.0.0` 对端会在 HELLO 阶段拒绝。当前已选择解耦，见 §16.5。


---

## 7. 验收命令汇总

以下命令分“静态”和“QEMU/运行时”。静态命令可在不加载模块的情况下跑；运行时命令需要另一个工程师的 QEMU 环境。

### 7.1 静态

```sh
# 未用键/字段
grep -rn 'cfg\.\(framing\|chunk\|max_retry\|uid\|dev\)' tbtun/
grep -rn 'portblock_' tbtun/*.c
grep -rn 'srcaddrs\[' tbtun/*.c
grep -rn 'tbt_passive_port\|tbt_tunnel_flush\|tbt_netns_optin' tbtun/
grep -rn 'fbk_mask' tbtun/

# 文档漂移
grep -n 'high four bits' DESIGN.md
grep -n 'zero flags byte' tbtun/tbt_carrier.c
grep -n '511' README.md README.zh.md
grep -n 'SO_REUSEADDR' DESIGN.md
grep -n 'm-spare-http-kal-drip' DESIGN.md tbtun/tbt_parse.c
grep -n 'default_replay_bytes\|max_replay_bytes\|fallback_redirect' DESIGN.md tbtun/

# 版本
grep -rn 'TBT_VERSION_MAJOR\|TBT_VERSION_MINOR\|TBT_VERSION_PATCH' tbtun/
grep -rn 'PACKAGE_VERSION' dkms.conf scripts/ .github/
```

### 7.2 QEMU/运行时

```sh
# 1) 未生效键：全部应 EINVAL（未知键），而不是被接受
echo flush > /proc/net/tbtun/config   # 从空配置开始，避免端口/旧隧道干扰
for k in 'framing=ext' 'chunk=1024' 'max_retry=3' 'dev=eth0' 'uid=1000' 'portblock=40000-40010'; do
    printf 'tunnel 1 role=far profile=raw listen=127.0.0.1:22001 target=127.0.0.1:5201 n=1 m=0 %s\n' "$k" \
        > /proc/net/tbtun/config 2>/dev/null && echo "FAIL: accepted $k" || echo "OK: refused $k"
done
echo flush > /proc/net/tbtun/config

# 2) ports 边界
printf '\n' > /proc/net/tbtun/ports; echo "blank rc=$?"          # 期望 0
printf ''   > /proc/net/tbtun/ports; echo "empty rc=$?"          # 期望 0
echo 'ports 0-0' > /proc/net/tbtun/ports; echo "0-0 rc=$?"       # 期望非 0
echo 'ports 41000-41015' > /proc/net/tbtun/ports; echo "set rc=$?" # 期望 0

# 3) config 一次性读：两次 1 KiB 读应拼成完整文件
cat /proc/net/tbtun/config | wc -c
dd if=/proc/net/tbtun/config bs=1024 count=1 2>/dev/null | wc -c   # 期望 1024（或文件尾）
dd if=/proc/net/tbtun/config bs=1024 skip=1 count=1 2>/dev/null | wc -c # 期望 >0，而不是 0

# 4) config 32 KiB：用 max_tunnels=128 + 长行，确认不是 32768
insmod tbtun.ko max_tunnels=128 debug=1
# 写入 128 条带 8 个 srcaddrs 的隧道（脚本生成），然后：
wc -c /proc/net/tbtun/config        # 期望等于各行长度之和，且 > 32768
cat /proc/net/tbtun/config > /tmp/c; cat /tmp/c > /proc/net/tbtun/config; echo "restore rc=$?"
grep -vc '^#' /proc/net/tbtun/tunnels  # 期望 128

# 5) max_tunnels/max_carriers：见 §4.3 的强化脚本
# 6) netns_optin 删除后
modinfo -p tbtun | grep netns_optin && echo "FAIL: param still present" || echo "OK: removed"
```

### 7.3 代码级单元测试（建议新增）

- `host/test_parse.c`：
  - 每个删除的键写一行，断言 `TBT_PARSE_EINVAL` 且错误 `key` 指向该键。
  - `ports 0-0` 仍拒绝；空行返回 0/0；`portblock=0-0` 若保留则接受。
  - 最大 canonical 行仍 `< TBT_LINE_MAX`；删除键后重算期望串。
- `host/test_http.c` / `host/test_fuzz.c`：
  - 若实现 `TBT_WIRE_VERSION`，钉住 HELLO body[0] 与 body[22] 的位布局。
- 新增一个 host 级“config 渲染总长”测试：用 `tbt_render_cfg()` 渲染 N 条最大行，断言总长计算与逐行相加一致；这能在不加载模块的情况下覆盖 §2.1 的分配逻辑（如果把渲染分配抽成纯函数）。

---

## 8. 附加缺陷（本次范围外，但与 P3 契约相邻）

这些不在用户列的 1-6 项里，静态阅读时发现，按严重度排列。建议主 agent 决定是否并入 P3。

1. **`tbt_portmap_set()` 丢弃在用端口**（`tbt_session.c:60-79`）。见 §3.3。控制面写入会扰动数据面；建议返回 `-EBUSY`。
2. **`tbt_cfg_open()` 用 `current` 的 netns**（`tbt_procfs.c:142`）。proc 文件属于 init_net，但 `cf->net` 取的是写入者的 netns；子 netns 通过 bind mount 写 init netns 的 `/proc` 时，隧道 socket 会落在子 netns（静态推断；未在 QEMU 复现）。删除 `netns_optin` 时改成 `get_net(&init_net)`。
3. **`tbt_procfs_init()` 失败路径删除未创建的条目**（`tbt_procfs.c:679-694`）。`goto fail` 后 `tbt_procfs_exit()` 无条件 `remove_proc_entry` 六个名字；review 34。改成记录已创建到哪一步，或让 `remove_proc_entry` 的返回值可忽略但不要打印 warning。
4. **`tbt_cfg_read()` 的 `out = -EFAULT`**（`tbt_procfs.c:245-246`）。`size_t out` 存负 errno 再返回 `ssize_t`，在二补数上“碰巧”是 -14；改成 `ssize_t ret`。
5. **`tbt_carrier_profile()` 的 `index` 参数未用**（`tbt_carrier.c:49-57`，`(void)index;`）。删参数或真正按 index 选择。
6. **`TBT_VERSION` 宏无使用**（`tbt_uapi.h:33`）。见 §5.3/§6.3。
7. **`tbt_parse.h:68` 的批量注释过期**。见 §5.5。

---

## 9. 需要主 agent 决策的点

1. **`framing=ext`：删除还是实现？** 本审计建议删除（0.0.1 的时间预算、`profile=raw` 已是可用的逃生口）。若实现，需要：HELLO 协商位或“两端必须同配”的明确规则、接收端 chunk-extension 解析、wire 测试；这不是 P3 的文档改动。
2. **`portblock=` / `srcaddrs[1..]`：删除/收窄还是实现？** 本审计建议删除 `portblock=`、把 `srcaddrs` 收窄为 1 条并改 DESIGN §7.2 的远端描述。若保留 `portblock=`，必须接进 `tbt_port_alloc()`，否则就是第二个“写了不生效”的键。
3. **`netns_optin`：删除还是实现？** 本审计建议删除；实现需要 per-netns 隧道表或按 `t->net` 过滤所有 list walk，属于数据模型改造。
4. **版本：** 已选择解耦（`TBT_WIRE_VERSION=1`，wire 字节不变）；最终回归见 §16.5。
5. **`max_tunnels` 的 errno 与上限**：建议 `-ENOSPC`，加载时限制 `<=4096`；若主 agent 希望与 `-EINVAL` 统一，也可以，但要在文档/测试里固定下来。
6. **`tbt_http_skip_request_header()`**：模块内无调用，但 host 测试覆盖。保留（作为纯库 API）或连同测试一起删；不要只删函数留下测试。

---

## 10. 证据索引（本文件引用的关键原文）

- 交接清单：`交接.md:374-380`（未生效键、dev/uid、max_tunnels 等）、`:432-441`（P3 12-16 项）。
- review 条目：`review/findings.md:307-314`（12）、`:330-335`（15）、`:351-356`（18）、`:358-362`（19）、`:393-400`（25/26）、`:448-455`（30）、`:464-468`（32）、`:482-487`（35）。
- adversarial：`adversarial/README.md:135`（max_tunnels）、`:138`（max_carriers）；`adversarial/init:205-216`、`:429-437`；`adversarial/findings.md:30-33`（F8/F10/F11）。
- 解析器：`tbt_parse.c:231-250`（range）、`:252-288`（profile/framing）、`:323-403`（srcaddrs/uids/dev）、`:409-607`（tunnel 键与默认值）、`:794-823`（ports）、`:860-953`（render）。
- procfs：`tbt_procfs.c:37`、`:100-133`（drain）、`:135-146`（open）、`:148-206`（write/release）、`:211-251`（render/read）、`:256-274`（ports write）、`:656-696`（init/exit）。
- 会话/上限：`tbt_session.c:60-79`（portmap）、`:106-143`（cfg_sane）、`:167-226`（tunnel_add）、`:486-514`（port_alloc）、`:673`（n_car）。
- 载体/接收：`tbt_carrier.c:233-237`（retry）、`:1098`（srcaddrs[0]）、`:1126-1129`（reuseaddr/bind）、`:1197-1204`（HELLO）；`tbt_listen.c:101-134`（bind/reuseaddr）；`tbt_rx.c:310`（hole check）、`:443-468`（HELLO）。
- 文档：`DESIGN.md:225-232`（§3.1.1）、`:351-465`（§3.3/§3.4）、`:557-645`（§4.3）、`:943-951`（§7.2）、`:1051-1124`（§8.3）、`:1171-1198`（§8.4.1）、`:1470-1479`（§8.9）、`:1749-1751`（§11）、`:1801-1816`（§13）。
- 版本：`tbt_uapi.h:30-44`；`tbt_core.c:140-142`、`:172-173`；`dkms.conf:2`；`Makefile:160-162`、`:180-186`；`scripts/mkdkmsconf.sh:27-59`；`scripts/install_dkms.sh:147-150`；`.github/workflows/release.yml:22-27`；`.gitignore:56`、`:260-261`；`review/baseline/01-env-sha.txt:72`。
- 测试：`host/test_parse.c:322-332`（行长）、`:337-410`（maximal）、`:818-842`（canonical 期望）、`:944-948`（ports）；`host/test_http.c:1432-1476`（skip header）；`host/test_fuzz.c:45`/`:211`（HELLO 长度）。

---

## 11. 本次没有做的事

- 没有编译、没有 `make check`、没有加载模块、没有跑 QEMU/adversarial；这些由占用构建环境的工程师执行。
- 没有修改任何源文件；本文件是唯一新增产物。
- 没有复跑 review 的 39 条与 adversarial 的 F1-F11；状态以 `review/findings.md` 的 05:19/05:27 快照与当前代码的静态对照为准，凡“快照说已修”的项没有重新验证。
- 行号会随并行编辑漂移；若主 agent 在合并前发现行号不符，以 `grep` 的符号名为准（本文件每条都给了函数名/键名）。

---

## 12. 实施记录（P3 可分离部分，2026-11-05）

范围：只改 `tbtun/tbt_parse.c`、`tbtun/tbt_parse.h`、`tbtun/tbt_procfs.c`、`host/test_parse.c`、`README.md`、`README.zh.md`、`DESIGN.md`，以及 `review/lint.sh` 的 host 探针（它不是内核源码，但会因 `struct tbt_cfg` 变化而编译失败）。未触碰 `tbt_session.c`、`tbt_core.c`、`tbt_rx.c`、`tbt_tx.c`、`tbt_carrier.c`、`tbt_sock.c`、`tbt_listen.c`；未跑 QEMU。

### 12.1 删除的键与符号

| 键 | 删除的解析/渲染 | 删除的字段/常量 | 测试变更 |
|---|---|---|---|
| `framing=`（整个） | `tbt_parse_framing()`；key 分支；`framings[]`；render 的 `framing=%s` | `struct tbt_cfg.framing`；`TBT_FRAMING_LEN/EXT` | `framing=ext` 现在是未知键，`refuse(... "framing" "unknown key")` |
| `chunk=` | key 分支；默认 `cfg->chunk`；render 的 `chunk=%u` | `struct tbt_cfg.chunk` | `chunk=1024` 现在是未知键 |
| `max_retry=` | key 分支（含 `-1` 特判）；默认 `cfg->max_retry`；render 的 `max_retry=%u` | `struct tbt_cfg.max_retry` | `max_retry=3` 现在是未知键；dup 邻接检查改用 `drip_ms` |
| `dev=` | `tbt_parse_dev()`；key 分支；render 的 `dev=%s` | `struct tbt_cfg.dev`；`TBT_IFNAMSIZ` | `dev=eth0` 现在是未知键 |
| `uid=` | `tbt_parse_uids()`；key 分支；render 的 `uid=` | `struct tbt_cfg.uid[]`、`nuid`；`TBT_UID_MAX` | `uid=1000` 现在是未知键 |
| `src=` | 本来就不是键 | 无 | 新增 `src=10.0.0.0/8` 的未知键测试 |

保留：`srcaddrs=`（只有第 0 项被近端载体读取；README/DESIGN 已注明）、`portblock=`（仍按 DESIGN §8.3 的“recorded, one global block”处理）。

`struct tbt_cfg` 的新 canonical 渲染顺序：

```
role listen [peer] [target] n m dup profile keepalive drip_ms replay
tunnel_rate max_inflate hole_budget_ms rx_reorder_bytes tx_retain_bytes
carrier_stall_ms portblock [srcaddrs]
```

### 12.2 `ports` 语义

- `tbt_parse_ports()` 现在接受 `ports 0-0`，返回 `lo=hi=0`；`tbt_ports_write()` 把 `0/0`（以及空行、零长度写）当作 no-op，不调用 `tbt_portmap_set()`，因此不改当前端口段。
- `ports 0-1` 仍被 `tbt_parse_range()` 拒绝（“port range must start at 1”）；`ports 41000-41015` 仍正常设置。
- `DESIGN.md` §8.3/§8.4.1 已写明：全局 `ports` 接受 `1 <= lo <= hi <= 65535`；空行和 `ports 0-0` 是 no-op，不清空端口段。
- 长度超限从 `-EINVAL` 改为 `-E2BIG`，与 `tbt_parse_ports()` 的 `TBT_PARSE_E2BIG` 一致。

### 12.3 验证结果（本次实际运行）

- `source ~/.cache/tbtun-build/env.sh && make check`：**311 checks, 0 failures**（用户给的基线是 365；减少来自删除的键/矩阵分支，新增 11 个拒绝用例）；`test_fuzz: seed=1 checks=8349443 failures=0`；`test_http: 249 checks, 0 failures`；h11 交叉校验通过。内核模块编译、modpost、BTF 均成功。
- `make host-test-san`：**311 checks, 0 failures**；`test_fuzz` 8349443、`test_http` 249、`test_http_fuzz` 4078276，全部 0 failures。
- `bash review/lint.sh`：**15/15 checks passed, 12/12 self-tests passed**。
- 说明：`review/lint.sh` 的 `render-fits-line-max` 探针原先直接写 `c.max_retry`/`c.chunk`/`c.framing`/`c.nuid`/`c.uid`/`c.dev`，字段删除后无法编译；已把探针改成只设置仍然存在的字段。这不是内核源文件改动。

### 12.4 grep 结果

`grep -rn "framing\|max_retry\|dev=\|uid=" tbtun/*.c tbtun/*.h host/*.c README*.md DESIGN.md` 的剩余命中分三类，均非悬空契约：

1. **HTTP/记录 framing 的普通英文词**：`tbt_http.c`、`tbt_http.h`、`tbt_proto.c`、`tbt_rec.c`、`tbtun.h`、`host/test_fuzz.c`、`host/test_http_live.c` 的注释/测试文本；与配置键无关。
2. **有意保留的历史/拒绝说明**：`DESIGN.md` §3.4/§6.1/§8.3/§13/§17.4 说明这些键曾存在、现已删除；`README.md`/`README.zh.md` 的“removed inert keys”说明；`host/test_parse.c` 的 `refuse(... "framing" "unknown key")` 等用例。
3. **`tbt_cfg` 的读取点**：`grep -rn 'cfg\.' tbtun/` 已不再出现 `cfg.max_retry`、`cfg.framing`、`cfg.chunk`、`cfg.uid`、`cfg.nuid`、`cfg.dev`；`srcaddrs[0]` 仍是唯一被数据路径读取的列表项（`tbt_carrier.c:1098`）。

`grep -rn 'TBT_UID_MAX\|TBT_IFNAMSIZ\|TBT_FRAMING\|nuid' tbtun/ host/*.c` 为空（HTTP 的 `chunk_*` 字段是容器解析器，不属于 `tbt_cfg`）。

### 12.5 仍未做（按用户指示留到后续）

- **`config` 32 KiB 静默截断与一次性读**：`tbt_procfs.c:37/214-251` 未改；修复方案见本文件 §2。
- **`max_tunnels`/`max_carriers`/`netns_optin` 生效**：涉及 `tbt_session.c`/`tbt_core.c`/`tbt_procfs.c`，本次不碰；实现点见 §4。
- **版本统一为 0.0.1**：`TBT_VERSION_MAJOR` 与 HELLO body[0] 耦合，需等 `tbt_tx.c`/`tbt_rx.c` 空闲后加 `TBT_WIRE_VERSION` 解耦；见 §6。
- **review 12/26/15 的规格漂移**：HELLO 位布局、`SO_REUSEADDR` 文档、`fbk_mask` 形参；本次未改（其中 §5.1 的文档修正可独立做，但用户本轮只要求键删除与 ports）。
- **`srcaddrs` 收窄为 1 条、`portblock` 删除**：本次保留现状；README/DESIGN 已注明“只有第一项被使用”和“recorded, one global block”。
- **adversarial 的 `init.src_key`/`pub.srcfilter`**：仍按“`src=` 是键”期望，会继续 FAIL；本次未改 adversarial（不在允许文件内）。应改成“`src=` 是未知键”的契约测试，或等实现真正的 CIDR 过滤后再恢复。

### 12.6 变更文件清单

- `tbtun/tbt_parse.h`：删除 6 个字段、4 个常量；更新 list-key 注释、`ports` 契约注释。
- `tbtun/tbt_parse.c`：删除 3 个解析函数与 5 个 key 分支、默认值、render 字段；`ports 0-0` 接受；更新顶部注释。
- `tbtun/tbt_procfs.c`：`tbt_ports_write()` 的零长度/空行/0-0 no-op 与 `E2BIG`。
- `host/test_parse.c`：canonical 串、maximal、matrix、dup 邻接、ports、far/near 字段检查；新增 6 个拒绝用例。
- `README.md`、`README.zh.md`：键表删除 5 行、1023 字节、removed-keys 说明、`ports` no-op、`srcaddrs` 仅首项。
- `DESIGN.md`：§3.4、§6.1、§8.3、§8.4、§8.4.1、§8.9、§13、§15 C8、§17.4、§18 第 4 条的同步。
- `review/lint.sh`：`render-fits-line-max` 探针同步新 `struct tbt_cfg`。

---

## 13. 实施记录（config 读的两个缺陷，2026-11-05）

范围：只改 `tbtun/tbt_procfs.c` 与 `host/test_parse.c`；未触碰任何内核数据路径文件（`tbt_session.c`、`tbt_core.c`、`tbt_rx.c`、`tbt_tx.c`、`tbt_carrier.c`、`tbt_sock.c`、`tbt_listen.c`）；未跑 QEMU。

### 13.1 改动的函数

| 函数 | 变更 |
|---|---|
| `struct tbt_cfg_file` | 新增 `struct mutex lock`、`char *out`、`size_t out_len`、`bool snapshot_valid`；快照按 open file description 缓存。 |
| `tbt_cfg_open()` | `mutex_init(&cf->lock)`；其余不变（仍捕获 netns）。 |
| `tbt_cfg_drop_snapshot()`（新） | 在 `cf->lock` 下 `kvfree(out)`、清空 `out/out_len/snapshot_valid`；写路径与释放路径共用。 |
| `tbt_cfg_write()` | 写入前后各丢弃一次快照：写前丢弃避免读到写前旧快照，写后丢弃覆盖“读在写中途拿到中间态”的窗口；行协议本身不变。 |
| `tbt_cfg_release()` | 先 `tbt_config_drain()` 冲刷最后一行，再在 `cf->lock` 下释放快照，最后 `put_net`/`kfree`。 |
| `tbt_config_render()`（删除） | 原固定 32 KiB 渲染函数删除。 |
| `tbt_config_snapshot()`（新） | 在 `tbt_lock` 下两遍渲染：第一遍用 `char line[TBT_LINE_MAX]` 逐行测量总长，第二遍 `kvmalloc(total, GFP_KERNEL)` 精确填充；任一行放不下、总长溢出或两遍长度不一致都返回 `-E2BIG`；分配失败 `-ENOMEM`。`tbt_lock` 是 mutex、调用点是 read/llseek 的进程上下文，`kvmalloc(GFP_KERNEL)` 可以睡眠，不在原子上下文。 |
| `tbt_cfg_read()` | 首次读时建立快照；按 `*ppos` 服务，返回 `min(len, out_len - *ppos)`，EOF 返回 0；`copy_to_user` 失败返回 `-EFAULT`；负 offset `-EINVAL`；零长度读直接返回 0。 |
| `tbt_cfg_llseek()`（新） | 支持 `SEEK_SET/SEEK_CUR/SEEK_END`；`SEEK_END` 先建立快照；负结果 `-EINVAL`，加法溢出 `-EOVERFLOW`；其余 whence `-EINVAL`。`proc_ops.proc_lseek` 从 `noop_llseek` 改为它。 |

### 13.2 错误路径与语义

- **不再有 32 KiB 固定上限**：`TBT_CFG_RENDER_MAX` 已删除；缓冲大小是实际渲染总长。隧道多到总长超过内存时返回 `-ENOMEM`，而不是短文件。
- **不再有一次读截断**：同一 fd 的第二次、第三次 `read()` 从 `*ppos` 继续，直到 `out_len` 才返回 0；`dd bs=4096`、逐块校验工具都能拿到完整文本。
- **`-E2BIG` 的三种来源**：单行渲染放不进 `TBT_LINE_MAX`；总长 `size_t` 溢出；第二遍渲染长度与第一遍不一致（列表在 `tbt_lock` 下不应变化，属于防御性检查）。这三种都让 `read()` 明确失败，不返回前缀。
- **快照一致性**：同一 fd 的读是“首次读时的配置快照”；写会在开始和结束各丢弃快照，所以“写后读”会重新渲染。不同 fd 各自快照，互不影响。
- **并发安全**：`cf->lock` 保护 `out/out_len/snapshot_valid`，读在 `copy_to_user` 期间持锁，写丢弃快照时等待；`cf->lock -> tbt_lock` 的锁序在 read/llseek 一致，写路径先释放 `cf->lock` 再取 `tbt_lock`，无反向锁序。
- **offset 语义**：`read()` 使用传入的 `*ppos`（普通 `read` 是 `file->f_pos`，`pread` 是调用者的局部 offset）；`llseek` 设置 `file->f_pos`。`SEEK_END` 会触发快照，因为长度只有渲染后才知道；`SEEK_SET/SEEK_CUR` 不强制渲染，越界读返回 0。

### 13.3 host 新用例

`host/test_parse.c` 第 15b 节（纯逻辑，不依赖 procfs）：

```c
int need = tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
ok(need > 0, "render length measured", NULL);
ok(tbt_render_cfg(&cmd.cfg, buf, (size_t)need) == TBT_PARSE_E2BIG,
   "buffer without room for NUL refused", NULL);
ok(tbt_render_cfg(&cmd.cfg, buf, (size_t)need + 1) == need,
   "exact-size buffer accepted", NULL);
```

同时把 `test_maximal_lines()` 的断言从 `n <= TBT_LINE_MAX` 收紧为 `n < TBT_LINE_MAX`，与 `tbt_config_snapshot()` 的 `char line[TBT_LINE_MAX]` 测量缓冲一致（1024 字节的行会被 parser 拒绝，渲染也不应产出）。

### 13.4 验证结果（本次实际运行）

- `source ~/.cache/tbtun-build/env.sh && make check`：**314 checks, 0 failures**；`test_fuzz: seed=1 checks=8349443 failures=0`；`test_http: 249 checks, 0 failures`；h11 通过；`tbt_procfs.o`/`tbtun.ko`/modpost/BTF 编译成功。
- `make host-test-san`：**314 checks, 0 failures**；`test_fuzz` 8349443、`test_http` 249、`test_http_fuzz` 4078276，全部 0 failures。
- `bash review/lint.sh`：**15/15 checks passed, 12/12 self-tests passed**。

### 13.5 需要 QEMU 验收的清单（perf 释放后）

1. `cat /proc/net/tbtun/config > /tmp/c; cat /tmp/c > /proc/net/tbtun/config`：无错误，隧道数/内容不变；对应 `adversarial init.cfg_roundtrip`。
2. `cat /proc/net/tbtun/config` 与 `dd if=/proc/net/tbtun/config bs=1024 count=1` + `dd ... skip=1 count=1` 拼接结果逐字节一致；验证短读与 offset。
3. 配置总长超过 32768 字节（例如 `max_tunnels` 调大后写多条长 `srcaddrs` 行）：`wc -c` 等于逐行渲染之和，且 `cat config > backup; cat backup > config` 不丢隧道；对应 `adversarial cat.cfg_self`。
4. `lseek(fd, 0, SEEK_END)` 返回 `out_len`，`pread(fd, buf, n, offset)` 返回对应片段；越界读返回 0。
5. 同一 fd 上并发 `read()` 与 `write()`（两个线程）：无 UAF/警告，写后读看到新配置；`dmesg` 无 `WARNING/BUG/KASAN`。
6. `config` 写入期间 `cat config`：允许看到写前或写后的完整快照，不允许看到半行/截断行；写结束后再读必须是新配置。
7. 单行渲染超过 `TBT_LINE_MAX` 的极端配置（如果未来新增键）：`read()` 返回 `-E2BIG`，不是短文件；当前键集不会触发（最大行 642）。

### 13.6 回归修复：探针 A 的 `config` 读返回 `-E2BIG`（2026-11-05）

**症状。** 探针 A 在启动约 0.6 s 时 `cat /proc/net/tbtun/config` 报
`read error: Argument list too long`，`config:` 为空。`E2BIG` 来自
`tbt_cfg_read()`，不是 `cat` 的参数问题。

**根因。** `tbt_render_cfg()` 返回的是**不含 NUL 的字节数 n**，但调用方必须给它
`n + 1` 字节（`tbt_addf()` 的 `vsnprintf` 要写结尾 NUL；host `test_parse` 第 15b 节
也钉住了 `size == n` 返回 `TBT_PARSE_E2BIG`、`size == n + 1` 返回 `n`）。
`tbt_config_snapshot()` 第一遍逐行测量得到 `total = Σ n`，第二遍却用
`kvmalloc(total)` 和 `tbt_render_cfg(..., total - off)`：
- 对最后一行，`total - off == n_last`，没有给 NUL 留字节；
- `tbt_addf()` 在 `(size_t)n >= size - *off` 时返回 `TBT_PARSE_E2BIG`；
- 快照返回 `-E2BIG`，`tbt_cfg_read()` 原样返回，`cat` 看到 E2BIG。

探针 A 的 canonical 行约 **277 字节**（`tunnel 1 role=near listen=127.0.0.1:… peer=… n=1 m=0 dup=0` 加上默认键），
远小于 `TBT_LINE_MAX=1024`，所以不是单行超长，而是最后一行缺 NUL 的 1 字节。

**最小修复（已写入 `tbt_procfs.c`，未构建）。**

```c
if (total == (size_t)-1) { err = -E2BIG; goto unlock; }
alloc = total + 1;                 /* final NUL */
out = kvmalloc(alloc, GFP_KERNEL);
...
n = tbt_render_cfg(&t->cfg, out + off, alloc - off);
...
if (off != total) { ... }          /* render returns bytes excluding NUL */
cf->out_len = total;               /* the NUL is not served */
```

中间行的 NUL 会被下一行覆盖；只有最后一行的 NUL 落在 `out[total]`。
`off != total` 的比较仍然正确，因为渲染返回值不含 NUL；`out_len` 仍是 `total`，
不把结尾 NUL 算进文件长度。

**覆盖范围。** host `test_parse` 的 15b 只能覆盖 `tbt_render_cfg()` 的
`n` vs `n+1` 契约；`tbt_config_snapshot()` 依赖内核的 `tbt_tunnels`/`tbt_lock`，
host 无法直接覆盖。必须由 QEMU 探针 A / `cat config` / `init.cfg_roundtrip` 验证。
本次按指示未构建、未跑 QEMU；上一轮的 314/0 不包含这个修复。

---

## 14. 实施记录（DESIGN/README/review 同步，2026-11-05）

本波只改文档与 review 工具，不碰内核源文件；未编译、未跑 QEMU。

### 14.1 改动章节

| 文件 | 章节/条目 | 变更 |
|---|---|---|
| `DESIGN.md` | §3.3 HELLO | 删除“high four bits”说法，给出 5:2 位图：`bit 7 6 = zero`、`5..2 = gen`、`1..0 = prof`；写明 TX `profile \| (gen << 2)`、RX `& 3` / `>> 2`。 |
| `DESIGN.md` | §4.3 门控 | 新增 pending 注记：`tbt_tx_hole_check()` 的 `u16 fbk_mask` 形参未使用；门控实际用 per-path `uack`/`recv_cnt`；待数据面冻结后删形参，行为不变。 |
| `DESIGN.md` | §7.2 绑定 | `SO_REUSEADDR` 从 “off” 改为 “on”，说明替换载体复用刚释放的端口、TIME_WAIT 可能让 connect 失败、由端口段重试覆盖；指向 `tbt_carrier.c:1126` / `tbt_listen.c:130`；同时把“bitmap per netns”改为“one global bitmap”。 |
| `DESIGN.md` | §8.9 参数/卸载 | 参数列表改为实际五个（`debug`、`max_tunnels`、`max_carriers`、`debug_loss`、`netns_optin`）；卸载顺序改为 `tbt_procfs_exit → tbt_tunnel_shutdown → synchronize_rcu → rcu_barrier`，删除 `flush_workqueue`；新增 review 35 的 dead-declaration 注记（`tbt_passive_port`/`tbt_tunnel_flush`/`tbt_netns_optin`），并说明 `tbt_http_skip_request_header` 不是死代码。 |
| `DESIGN.md` | §11 版本 | 先改为“当前 1.0.0、HELLO body[0] 是第二兼容轴、计划见 §6.5”；§16.5 再更新为 0.0.1 release candidate、wire 已解耦。 |
| `DESIGN.md` | §3.4/§6.1/§8.3/§13/§17.4/§18 | 与上一波删除的键保持一致：`framing=ext` 已删、`chunk=`/`max_retry=`/`dev=`/`uid=` 已删、`src=` 不是键；保留历史/“removed”说明。 |
| `README.md` / `README.zh.md` | 模块参数表 | `max_tunnels`/`max_carriers`/`netns_optin` 先标注“declared; not enforced yet — review 18”；§15 随后删除 `netns_optin` 行并把 `max_*` 改为 enforced。 |
| `review/findings.md` | 顶部与状态表 | 新增 2026-11-05 current-status 表：P0 rmmod（code fixed/QEMU pending）、12（doc fixed/comment pending）、14（fixed）、15（pending）、18（pending）、19/25/30/32（fixed）、20/22（pending）、26（doc fixed/code on）、35（keys fixed/dead decls pending）、P3 key deletion、P3 config read/write；附证据命令。 |
| `review/findings.md` | 12/14/15/20/22/26/35 条目 | 标题/正文改为当前状态；spec-drift 表中 §3.1.1/§3.3/§3.4/§3.5/§6.1/§7.2/§8.4/§8.5/§13 行同步。 |
| `review/analysis/p3-contract.md` | §6.5 | 新增版本解耦执行计划（`TBT_WIRE_VERSION=1`、HELLO body[0]、模块 0.0.1、dkms 派生、测试与不 bump `TBT_CONV` 的规则），明确未实施。 |

### 14.2 一致性 grep（本次实际运行）

```sh
grep -n 'high four bits' DESIGN.md                 # empty
grep -n 'zero flags byte' DESIGN.md                # empty
grep -n 'SO_REUSEADDR.*off' DESIGN.md              # empty
grep -n 'starting 1.0.0' DESIGN.md                 # empty
grep -n 'only compatibility axis' DESIGN.md        # empty
grep -n 'default_replay_bytes\|fallback_redirect' DESIGN.md  # empty
grep -n 'per netns' DESIGN.md                      # empty
grep -n 'SO_REUSEADDR' DESIGN.md                   # "on" (tbt_carrier.c:1126 / tbt_listen.c:130)
grep -n 'fbk_mask' DESIGN.md                       # pending note in §4.3
```

`max_replay_bytes` 仍出现在 §9.6/R16 的容量说明里，但不再是 §8.9 的“模块参数”；这是设计常量，不是参数表。

### 14.3 剩余漂移项

- **review 15**：`tbt_tx.c:897` 的 `fbk_mask` 形参仍未删；等数据面冻结。
- **review 20/22**：重叠重排帧未淘汰（`tbt_rx.c:63` 只拉精确 seq）；pinned 控制帧在 `-EAGAIN` 仍 `kfree`（`tbt_tx.c:1525/1544`）。
- **review 12**：DESIGN 已对齐；`tbt_carrier.c:1190` 的“zero flags byte”注释仍在（内核源文件，本波不改）。
- **review 35**：死键已删；`tbt_passive_port`/`tbt_tunnel_flush`/`tbt_netns_optin` 声明仍在（等冻结后删）。
- **review 18**：已由 §15 实施（`max_tunnels`/`max_carriers` enforced；`netns_optin` removed）；§14 记录的是实施前的文档状态。
- **review 13/16/21/23/24/34**：未变。
- **版本解耦**：已实施（`TBT_WIRE_VERSION=1`、HELLO `body[0]`、模块宏 `0/0/1`、`dkms.conf` 0.0.1）；最终回归见 §16.5。

---

## 15. 实施记录（max_tunnels / max_carriers / netns_optin，2026-11-05）

范围：`tbt_core.c`、`tbt_session.c`、`tbtun.h`、`tbt_procfs.c`、`README.md`、`README.zh.md`、`DESIGN.md`、`review/findings.md`。未触碰 `tbt_tx.c`/`tbt_rx.c`/`tbt_sock.c`/`tbt_carrier.c`；未跑 QEMU。

### 15.1 `max_tunnels`

- **检查点**：`tbt_tunnel_add()`，`tbt_cfg_sane()` 之后、`kzalloc()` 之前（`tbt_session.c:186-199`）。先 `tbt_tunnel_find_locked(cfg->id)`；只有**新 id** 才计数，重发已有 id 仍按 §8.4.1 幂等替换，不消耗新槽位。
- **计数**：`tbt_tunnel_count_locked()`（`tbt_session.c:169-178`）遍历全局 `tbt_tunnels`。
- **锁不变量**：`tbt_config_apply()` 在调用 `tbt_tunnel_add()` 前已持有 `tbt_lock`（`tbt_procfs.c:90-92`），所以 `tbt_tunnel_add()` **不得再次加锁**；计数、`list_del_rcu(old)`、`list_add_tail_rcu(t)` 都由同一把锁串行化，两个并发写不会同时通过上限检查。注释写在 `tbt_session.c:180-185`。
- **错误路径**：`!old && count >= READ_ONCE(max_tunnels)` → `-ENOSPC`，在 `kzalloc`/`list_del_rcu`/`tbt_listen_start` 之前返回，运行中的配置完全不变。
- **文档**：DESIGN §8.3/§8.9/§13 写明 `-ENOSPC`、替换语义与锁；README 参数表写明新 id 超限失败。

### 15.2 `max_carriers`

- **检查点**：`tbt_cfg_sane()`（`tbt_session.c:118-126`），在 `tbt_tunnel_add()` 的任何分配之前调用。
- **条件**：`!c->n || c->n + c->m > TBT_MAX_CARRIERS_LIMIT || c->n + c->m > READ_ONCE(max_carriers)` → `-EINVAL`。
- **为什么放这里**：`max_carriers` 是 module_param，只有内核构建可见；`tbt_parse.c` 与 host `test_parse` 不引用它，语法层仍只校验编译期 `1..16`。
- **文档**：DESIGN §8.3/§8.9/§13 与 README 参数表写明 `n + m > max_carriers` 返回 `-EINVAL`，且在 session 创建前拒绝。

### 15.3 `netns_optin` 删除

- `tbt_core.c`：删除 `bool netns_optin`、`module_param(netns_optin, …)`、`MODULE_PARM_DESC`、`tbt_netns_optin()`。
- `tbtun.h`：删除 `bool tbt_netns_optin(void);` 声明。
- `tbt_procfs.c`：没有 `netns_optin` 条件分支；顺手把 `tbt_cfg_open()` 的 netns 捕获从 `current->nsproxy->net_ns` 改为 `get_net(&init_net)`（`tbt_procfs.c:161-165`），与“控制面只在 init_net”一致，避免 bind mount 把隧道建到调用者 netns。
- `README.md`/`README.zh.md`：删除参数行，改为“控制面只在 `init_net` 创建；没有 per-netns 视图”。
- `DESIGN.md`：§8.3 改为 init_net-only；§8.4.2/§8.9 参数表删除 `netns_optin`；§8.9 不再把 `tbt_netns_optin` 列为 dead declaration。
- `review/findings.md`：review 18 标为 fixed（limits enforced; netns_optin removed），review 35 的 dead-declaration 列表只剩 `tbt_passive_port`/`tbt_tunnel_flush`。

### 15.4 验证（本次实际运行）

- `source ~/.cache/tbtun-build/env.sh && make check`：**314 checks, 0 failures**；`test_fuzz: seed=1 checks=8349443 failures=0`；`test_http: 249 checks, 0 failures`；h11 通过；`tbt_core.o`/`tbt_session.o`/`tbt_procfs.o`/`tbtun.ko`/modpost/BTF 编译成功。
- `make host-test-san`：**314 checks, 0 failures**；`test_fuzz` 8349443、`test_http` 249、`test_http_fuzz` 4078276，全部 0 failures。
- `bash review/lint.sh`：**15/15 checks passed, 12/12 self-tests passed**。
- `modinfo -p tbtun.ko`：只剩 `debug`、`max_tunnels`、`max_carriers`、`debug_loss`，没有 `netns_optin`。
- `grep -rn 'netns_optin\|tbt_netns_optin' tbtun/*.c tbtun/*.h README.md README.zh.md DESIGN.md`：空（历史 `review/findings.md` 中保留记录）。

### 15.5 待 QEMU 验收（perf 释放后）

1. `adversarial/run.sh init.max_tunnels`：默认 `max_tunnels=64` 下 80 条隧道，第 65 条 `echo` 必须失败（`-ENOSPC`），`/proc/net/tbtun/tunnels` 计数为 64；再验证在 64 条时重发已有 id 成功。
2. 强化边界：`max_tunnels=2` 下第 3 个新 id 失败、第 1 个 id 重发成功；`dmesg` 无 WARNING。
3. `adversarial/run.sh init.max_carriers`：`max_carriers=4` 下 `n=8 m=0` 必须失败（`-EINVAL`）；边界 `n=2 m=2` 成功、`n=2 m=3` 失败、`n=5 m=0` 失败。
4. 失败路径无部分配置：超限写入后 `cat /proc/net/tbtun/config` 与写入前逐字节一致，`/proc/net/tbtun/tunnels` 不变。
5. `modinfo -p tbtun.ko` 无 `netns_optin`；`grep netns_optin /proc/kallsyms` 无模块符号；在子 netns 中看不到 `/proc/net/tbtun`（预期）。
6. 卸载/重载：`rmmod`/`insmod` 后 `max_tunnels`/`max_carriers` 新值生效，旧隧道不残留。

---

## 16. 实施记录（DKMS 版本派生，2026-11-05）

本波只改脚本与文档；未碰内核源文件、未构建、未跑 QEMU。

### 16.1 `dkms.conf` 的性质

- `dkms.conf` 是**生成物**：`Makefile:160-162` 的 `dkms.conf` 目标运行
  `./scripts/mkdkmsconf.sh > dkms.conf`；`.gitignore:56`/`:260` 忽略它，
  `git ls-files dkms.conf` 为空。
- 当前值已由新脚本重新生成为 `PACKAGE_VERSION="0.0.1"`。生成文件与脚本输出逐字节一致。
- `dkms.tar.gz` 是另一个生成物（`.gitignore:261`），本次**没有**重新打包；
  它内部的 `dkms_source_tree/dkms.conf` 仍是旧的
  `PACKAGE_VERSION="1.0.0.r74.ee0e5e6"`。等最终回归后跑
  `make dkms-tarball`。

### 16.2 派生逻辑

`scripts/mkdkmsconf.sh`：

- 从 `tbtun/tbt_uapi.h` 读取 `TBT_VERSION_MAJOR/MINOR/PATCH`；
  `version_field()` 用 `awk` 要求**恰好一个**数值 `#define`，缺失、非数字或重复都
  报错退出（`mkdkmsconf.sh: expected exactly one numeric #define …`）。
- 默认 `PACKAGE_VERSION="<major>.<minor>.<patch>"`，当前为 `0.0.1`。
- `TBT_DKMS_GIT_SUFFIX=1` 才追加 `.r<commits>.<sha>`；当前仓库会得到
  `0.0.1.r74.ee0e5e6`，但这不是默认值。
- `--version` 只打印版本，供安装脚本使用；其他参数报错。
- 不再使用 `PACKAGE_VERSION` 环境变量覆盖，版本唯一权威是 `tbt_uapi.h`。

`scripts/install_dkms.sh:147-150` 改为
`VERSION=$("$SRC_DIR/scripts/mkdkmsconf.sh" --version)`；失败时 `die`，不再静默
解析空值。`--dry-run` 打印 `Installing tbtun 0.0.1 for kernel …` 与
`stage: /usr/src/tbtun-0.0.1`。

`Makefile` 的 `dkms.conf` 目标增加 `tbtun/tbt_uapi.h` 依赖，并注释说明生成物；
`dkms-tarball` 仍依赖 `dkms.conf`，因此使用同一个派生值。

### 16.3 验证（未构建/QEMU）

- `bash -n scripts/mkdkmsconf.sh`、`bash -n scripts/install_dkms.sh`：通过。
- `./scripts/mkdkmsconf.sh --version` → `0.0.1`。
- `./scripts/mkdkmsconf.sh` 完整输出与 `dkms.conf` 逐字节一致。
- 临时目录删除 `TBT_VERSION_MINOR` 后运行脚本：退出非零，错误为
  `expected exactly one numeric #define TBT_VERSION_MINOR in tbtun/tbt_uapi.h`。
- `TBT_DKMS_GIT_SUFFIX=1 ./scripts/mkdkmsconf.sh --version` →
  `0.0.1.r74.ee0e5e6`。
- `./scripts/install_dkms.sh --dry-run --no-load` 打印版本 `0.0.1`。
- `make -n dkms.conf` 打印 `./scripts/mkdkmsconf.sh > dkms.conf`。

### 16.4 0.0.1 cutover 状态

- `tbtun/tbt_uapi.h`：`TBT_VERSION_*` = `0/0/1`；`TBT_WIRE_VERSION=1`；`TBT_CONV=0x0004` 未改。**已完成。**
- `tbtun/tbt_carrier.c`/`tbtun/tbt_rx.c`：HELLO `body[0]` 使用 `TBT_WIRE_VERSION`。**已完成（P2c）。**
- `dkms.conf`：重新生成为 `PACKAGE_VERSION="0.0.1"`。**已完成。**
- `dkms.tar.gz`：仍是旧的 `1.0.0.r74.ee0e5e6`；**待最终回归后 `make dkms-tarball`**。
- `DESIGN.md` §11、`README.md`/`README.zh.md` Packaging、`CHANGELOG.md` 标题与 Packaging：**已改为 0.0.1 release candidate**。
- `review/baseline/*`、`adversarial/*.log`：**待最终回归重新采集**，不手改。

### 16.5 modinfo 与 install dry-run 期望（未构建）

- `MODULE_VERSION` 由 `TBT_VERSION_MAJOR/MINOR/PATCH` 经 `__stringify` 派生
  （`tbt_core.c:172-173`）。当前 `modinfo -F version tbtun.ko` 报 `0.0.1`，与宏一致；
  干净重建后仍应为 `0.0.1`。
- `./scripts/mkdkmsconf.sh --version` → `0.0.1`；
  `dkms.conf` 的 `PACKAGE_VERSION="0.0.1"`。
- `./scripts/install_dkms.sh --dry-run --no-load` → `Installing tbtun 0.0.1 for kernel …`，
  `stage: /usr/src/tbtun-0.0.1`。
- `TBT_DKMS_GIT_SUFFIX=1` → `0.0.1.r74.ee0e5e6`（可选，不是默认）。
- 最终回归仍需确认：重新构建后的 `modinfo -F version`、`dkms.tar.gz` 内容、
  QEMU/adversarial/bench 全绿。本次未构建、未跑 QEMU。
