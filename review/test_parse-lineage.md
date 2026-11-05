# test_parse 365 → 357：逐项谱系与证据边界

本文只回答一个问题：目标里的 **365 checks** 与当前 **357 checks** 差在哪里。
每个数字给出仓库内可复现的命令或归档路径；无法从本仓库 git 复现的，明确标注
**“归档，不可由 git 复现”**，不把归档数字伪装成当前测试的计数。

## 0. 结论

1. **365 不在本仓库 git 历史里。** 它是 2026-11-05 06:28 旧树的归档输出
   （`review/baseline/02-make-check.txt:6`）。本仓库的 `host/test_parse.c`
   历史只有两个提交：`694b335`（355 checks）和 `bd0cdff`（357 checks）。
   删除五个键的提交不在本仓库历史中；`694b335` 已经是删除后的快照。
2. **当前 357 = 355（694b335）+ 2（bd0cdff 的 far-peer 用例）。**
   这两个增量都能用 `git diff 694b335 HEAD -- host/test_parse.c` 逐行核对。
3. **365 → 357 的净 −8 不是“删了 8 个旧用例”。** 按归档链条：
   `365 → 311`（删五键，净 −54）→ `314`（+3）→ `348`（+34）→ `355`（+7）→ `357`（+2）。
   净 −8 = −65（旧用例毛减少）+ 57（后续新增：11 + 3 + 34 + 7 + 2）。
   8 是净值；删除五键的毛减少按归档是 65。
4. **五键的行为覆盖由当前拒绝用例保留。** `host/test_parse.c:964-978` 对
   `framing`/`chunk`/`max_retry`/`uid`/`dev` 各有一个
   `refuse(..., TBT_PARSE_EINVAL, "<key>", "unknown key")`，每个 `refuse`
   计 3 checks（errno、key、reason），共 15 checks。`src` 另有一个
   `refuse`（`:979-981`），它不是五键之一。
5. **逐键旧 check 数在本仓库中不可得。** `review/analysis/p3-contract.md`
   只记录聚合 311 和“新增 11 个拒绝用例”，不记录每个键删了多少 checks；
   删除前的 `host/test_parse.c` 不在 git 中。本文因此不编造逐键旧计数，
   只给出可核对的当前替代计数（每键 3 checks）和聚合 65 的出处。

## 1. 365 快照的来源

### 1.1 哪个文档给出 365

| 证据 | 位置 | 值 |
|---|---|---|
| 旧树 `make check` 原始输出 | `review/baseline/02-make-check.txt:6` | `365 checks, 0 failures` |
| 基线复验表 | `review/baseline.md:43-44` | `365 checks / 0 failures` |
| P0-rmmod 实施记录 | `review/analysis/p0-rmmod.md:1007` | `365 checks / 0 failures` |
| 发布清单的 goal 原文 | `review/final-checklist.md:79` | `365 checks, 0 failures`（goal 原文） |

`review/baseline/02-make-check.txt` 的第一行命令是
`make -C ... M=/home/kunweiz/Desktop/vibe/tcp-brutal ... modules`，
生成时间 2026-11-05 06:28。该文件是旧树的归档输出，不是本仓库某个提交的构建产物。

### 1.2 为什么 git 历史里找不到 365

```sh
$ git log --all --oneline -- host/test_parse.c
bd0cdff p1-nonce: pin nonce and bind the far side to its near source
694b335 snapshot: tbtun pre-parallel baseline

$ git log -p -- host/test_parse.c
# 同样只有这两个提交；694b335 的 patch 是“新增 355-check 文件”，
# 不是“删除五键”。删除五键的 diff 不在本仓库。

$ git log --all --oneline -S'framing' -- tbtun/tbt_parse.c
# 空：本仓库历史里没有增加或删除 framing 键的提交

$ git rev-list --max-parents=0 HEAD
d2276eb...
$ git log --oneline | wc -l
139
```

`694b335` 是“pre-parallel baseline”快照；它已经包含五个拒绝用例：

```sh
$ git show 694b335:host/test_parse.c | grep -n 'framing removed'
956:    refuse("framing removed",
```

也就是说，删除五键发生在 `694b335` 之前，发生在旧树
（`/home/kunweiz/Desktop/vibe/tcp-brutal` 的 2026-11-05 06:28 状态）里；
本仓库在 2026-11-05 19:07 才以 `694b335` 重新快照，历史从这里开始。
**结论：365 是旧树归档值；本仓库没有产生 365 的提交，也没有删除五键的提交。**

### 1.3 最接近的可复现计数

| 树 | 复现命令 | 结果 |
|---|---|---|
| `694b335` | 取该提交的 `host/test_parse.c`、`tbtun/tbt_parse.c`、`tbtun/tbt_parse.h`、`tbtun/tbt_uapi.h`，`gcc -std=gnu99 -O2 -I. test_parse.c tbt_parse.c -o test_parse && ./test_parse` | **355 checks, 0 failures** |
| `review/baseline/p1-tree` | `gcc -std=gnu99 -O2 -I tbtun host/test_parse.c tbtun/tbt_parse.c -o test_parse && ./test_parse` | **348 checks, 0 failures** |
| 当前 `HEAD` | `source ~/.cache/tbtun-build/env.sh && make check` | **357 checks, 0 failures** |

355 与 357 的差是 `bd0cdff` 的 far-peer 用例（见 §3.3）；348 与 355 的差是
review 23（见 §3.2）。365 与 355 的差是 10：其中 review 23 是 +7，
删除五键 / 15b / nonce 三步的净变化是 −17（−54 + 3 + 34，见 §3.1）。
这个 −17 不是“五键只删了 17 个 check”的结论；它是三步净值的合计，
真正的删除毛减少是 65（§3.1）。

## 2. 删除五键：旧用例与当前替代

### 2.1 删除的键与解析/渲染点

`review/analysis/p3-contract.md:564-568`（§12.1）逐键记录：

| 键 | 删除的解析/渲染 | 删除的字段/常量 | 测试变更（§12.1 原文） |
|---|---|---|---|
| `framing=`（整个） | `tbt_parse_framing()`；key 分支；`framings[]`；render 的 `framing=%s` | `struct tbt_cfg.framing`；`TBT_FRAMING_LEN/EXT` | `framing=ext` 现在是未知键，`refuse(... "framing" "unknown key")` |
| `chunk=` | key 分支；默认 `cfg->chunk`；render 的 `chunk=%u` | `struct tbt_cfg.chunk` | `chunk=1024` 现在是未知键 |
| `max_retry=` | key 分支（含 `-1` 特判）；默认 `cfg->max_retry`；render 的 `max_retry=%u` | `struct tbt_cfg.max_retry` | `max_retry=3` 现在是未知键；dup 邻接检查改用 `drip_ms` |
| `dev=` | `tbt_parse_dev()`；key 分支；render 的 `dev=%s` | `struct tbt_cfg.dev`；`TBT_IFNAMSIZ` | `dev=eth0` 现在是未知键 |
| `uid=` | `tbt_parse_uids()`；key 分支；render 的 `uid=` | `struct tbt_cfg.uid[]`、`nuid`；`TBT_UID_MAX` | `uid=1000` 现在是未知键 |
| `src=` | 本来就不是键 | 无 | 新增 `src=10.0.0.0/8` 的未知键测试 |

### 2.2 当前替代：每个键 3 checks

当前文件（`host/test_parse.c`）的替代用例是 5 个 `refuse`，每个 3 checks：

| 键 | 当前用例行 | 断言 | checks |
|---|---|---|---|
| `framing` | `host/test_parse.c:964-966` | errno `EINVAL`；`perr.key == "framing"`；`perr.why` 含 `"unknown key"` | 3 |
| `chunk` | `host/test_parse.c:967-969` | 同上，key `chunk` | 3 |
| `max_retry` | `host/test_parse.c:970-972` | 同上，key `max_retry` | 3 |
| `uid` | `host/test_parse.c:973-975` | 同上，key `uid` | 3 |
| `dev` | `host/test_parse.c:976-978` | 同上，key `dev` | 3 |
| 合计 | | | **15** |

`src` 不是五键之一，但同一节也有 `refuse("src is not a key", ...)`
（`host/test_parse.c:979-981`），3 checks。五个键 + `src` 合计 18 checks；
这是当前文件的可核对计数，不是删除前的旧计数。

### 2.3 旧用例的逐键 check 数：仓库内没有依据

`review/analysis/p3-contract.md:590`（§12.3）只给出聚合：

> `make check`：**311 checks, 0 failures**（用户给的基线是 365；减少来自删除的键/矩阵分支，
> 新增 11 个拒绝用例）

它没有给出 `framing`/`chunk`/`max_retry`/`uid`/`dev` 各自的旧 check 数。
删除前的 `host/test_parse.c` 不在本仓库 git 中；`694b335` 已经是删除后的文件。
因此本文**不给出**逐键旧 check 数，也不把 15（当前替代）说成“每键删了 3 个”。
可以核对的只有：

- 删除的键和测试变更：`review/analysis/p3-contract.md:564-568`；
- 删除后的聚合：311（§12.3）；
- 当前替代：每个键 3 checks，共 15（§2.2 的行号）。

如果需要一个“旧用例毛减少”的数字，只能用归档的聚合反推：
`365 + 11 − 311 = 65`。这里的 11 是 §12.3 记录的“新增 11 个拒绝用例”，
它与当前文件里五个键的 15 checks 不是同一口径；不要把这 11 和 15 相加，
也不要把 65 拆成五个“大概”的数。仓库没有逐键拆分的证据。

### 2.4 归档片段里能看到的旧断言（不在 git）

删除前的 `host/test_parse.c` 片段（归档在 2026-11-05 00:22 的 eng-contract
会话记录里，不在本仓库）显示，五键在 section 16 的旧断言是：

| 键 | 旧断言 | 旧 check 数 |
|---|---|---|
| `framing` | `ok(cmd.cfg.framing == TBT_FRAMING_EXT, "far framing=ext")` | 1 |
| `chunk` | `ok(cmd.cfg.chunk == 1024 && cmd.cfg.drip_ms == 250 && cmd.cfg.replay == 0, "far chunk/drip/replay")` | 1（与 `drip_ms`/`replay` 共享） |
| `max_retry` | `ok(cmd.cfg.hole_budget_ms == 50 && cmd.cfg.max_retry == 3, "far hole/retry")`；`ok(cmd.cfg.max_retry == 0xffffffffu, "dup did not overwrite max_retry")` | 2（1 共享 + 1 邻接） |
| `uid` | `ok(cmd.cfg.nsrc == 2 && cmd.cfg.nuid == 2 && !strcmp(cmd.cfg.dev, "eth0"), "near lists")` | 1（与 `dev` 共享） |
| `dev` | 同上（`near lists`） | 1（与 `uid` 共享） |

这些可见旧断言合计 5 个（其中 3 个与仍保留的字段共享）。当前文件把
`chunk/drip/replay`、`hole_budget`、`near srcaddrs` 三个断言保留为收窄后的
`ok`，把 `max_retry` 邻接断言换成 `drip_ms`，并删掉 `framing=ext` 的断言；
因此**可见片段的净删除只有 1 个 check（framing）**。归档的 65 个毛减少
不能由这段可见片段解释，剩余旧用例不在归档片段或 git 中；不要从这张表
推断逐键旧计数。

## 3. 当前 357 的构成

### 3.1 归档链条

| 步骤 | test_parse | 变化 | 证据 |
|---|---|---|---|
| 旧树 | 365 | — | `review/baseline/02-make-check.txt:6`（归档） |
| 删除五键后 | 311 | −54 | `review/analysis/p3-contract.md:590`（归档） |
| + 15b render-length 用例 | 314 | +3 | `review/analysis/p3-contract.md:670`（归档） |
| + review 16/F8 等 | 348 | +34 | `review/baseline/p1-freeze.md:58`（归档） |
| + review 23 | 355 | +7 | `diff review/baseline/p1-tree/host/test_parse.c <(git show 694b335:host/test_parse.c)` |
| + far-peer | 357 | +2 | `git diff 694b335 HEAD -- host/test_parse.c` |

其中 311→314→348 是归档值，不能由本仓库 git 逐提交复现；348、355、357
可以分别由 p1-tree、`694b335`、`HEAD` 构建复现（§1.3）。

### 3.2 +7：review 23 的两个零值拒绝和一个默认断言

`694b335` 相对 p1-tree 的 diff 只增加这 7 个 checks：

- `refuse("rx_reorder_bytes=0", ...)`：3 checks（`host/test_parse.c:1117-1119`）；
- `refuse("hole_budget_ms=0", ...)`：3 checks（`host/test_parse.c:1120-1122`）；
- `ok(cmd.cfg.hole_budget_ms == 500 && cmd.cfg.rx_reorder_bytes == 262144,
  "C3 budgets default when omitted")`：1 check（`host/test_parse.c:1109-1110`）。

`review/analysis/stability.md:820` 的“review 23”就是
`rx_reorder_bytes=0` / `hole_budget_ms=0` 被接受的问题；这里用 6 + 1 个 checks
钉住“零值必须拒绝、省略时默认值必须正确”。

### 3.3 +2：far-peer 锚点

`bd0cdff` 相对 `694b335` 的 diff 在 `test_far_peer()` 里：

- 旧：`ok(strstr(buf, " peer=") == NULL, "far line normalizes peer away")`（1 check）；
- 新：`ok(strstr(buf, " peer=198.51.100.7:8443") != NULL, "far line renders its peer anchor")`（1 check）；
- 新：`ok(parse(... 无 peer ...) == 0, "far line without a peer parses")`（1 check）；
- 新：`ok(strstr(buf, " peer=") == NULL, "far line without a peer omits it")`（1 check）。

旧 2 checks → 新 4 checks，净 +2。行号见 `host/test_parse.c:571-581`。

### 3.4 +3：15b render-length

`review/analysis/p3-contract.md:653-666`（§13.3）新增三个纯逻辑断言，
当前在 `host/test_parse.c:1053-1065`：

- `ok(need > 0, "render length measured")`；
- `ok(tbt_render_cfg(..., need) == TBT_PARSE_E2BIG, "buffer without room for NUL refused")`；
- `ok(tbt_render_cfg(..., need + 1) == need, "exact-size buffer accepted")`。

### 3.5 +34：review 16/F8 等

`review/baseline/p1-freeze.md:58` 记录 348；`review/analysis/p3-contract.md:670`
记录 314。两者相差 34，来自 nonce/源锚（review 16/F8）等用例；
本仓库没有 314 状态的提交，所以这 34 不能再按测试函数逐项拆分。
当前文件里能看到的 nonce 用例在 `test_tunnel_nonce()`（`host/test_parse.c:202` 起）
和 `test_actions()` 的 `session ... nonce=` 用例（`:139-179`），但它们的
精确合计不能倒推 314→348 的 34；本文不把“看到多少”当成“就是 34”。

## 4. 减少的 8 个 check 是否全是已删除键的旧用例？

**不是。** 三个可核对的理由：

1. **8 是净值，不是删除量。** 归档链条的净变化是
   `−54 + 3 + 34 + 7 + 2 = −8`。其中删除五键一步的净变化是 −54；
   按 §12.3 的 +11 反推，旧用例毛减少是 65。把 8 说成“删了 8 个旧用例”
   会把 65 个删除和 57 个新增（11 + 3 + 34 + 7 + 2）混成一个数。
2. **当前 357 里包含 15 个“五键拒绝”checks。** 如果五键的旧用例只是被
   删除而没有替代，当前文件不会在 `host/test_parse.c:964-978` 有这 15 个
   checks。它们是新增的拒绝契约，不是旧用例的残留。
3. **旧用例的具体逐键计数不在仓库里。** `p3-contract` 只记录聚合 311 和
   §12.1 的用例变更；删除前的测试源不在 git 中。任何“framing 删了 N 个、
   chunk 删了 M 个”的逐键表都会是猜测，本文不写。

### 行为覆盖是否由当前拒绝用例保留？

是。当前拒绝用例钉住的是**当前语法**的契约，而不是旧键的数据路径：

- 五个键的旧解析/渲染点已从语法删除（§2.1）；
- 当前对每个键断言 `EINVAL`、错误 key 指向该键、reason 含 `unknown key`
  （`host/test_parse.c:964-978`）；
- 这比“旧键还能解析并渲染”更强：它保证写配置的人不会以为一个不生效的键被接受
  （p3-contract §0 的 poka-yoke 理由）。

旧键在数据路径上从未被读取（`review/analysis/p3-contract.md:27-31` 的
“数据路径读取点”列全部为“无”），所以删除它们的解析用例没有删除任何
数据路径行为覆盖；当前拒绝用例覆盖的是“未知键必须报错”这一条真实契约。

## 5. 复现命令汇总

```sh
# 1) 365 的归档来源（不是 git）
sed -n '1,10p' review/baseline/02-make-check.txt
grep -n '365' review/baseline.md review/analysis/p0-rmmod.md review/final-checklist.md

# 2) 本仓库的 test_parse 历史只有两个提交
git log --all --oneline -- host/test_parse.c

# 3) 694b335 = 355
mkdir -p /tmp/tbt355 && cd /tmp/tbt355
for f in host/test_parse.c tbtun/tbt_parse.c tbtun/tbt_parse.h tbtun/tbt_uapi.h; do
    git -C /home/kunweiz/tbtun-worktrees/docs5 show 694b335:$f > "$(basename "$f")"
done
gcc -std=gnu99 -O2 -I. test_parse.c tbt_parse.c -o test_parse
./test_parse | tail -1        # 355 checks, 0 failures

# 4) p1-tree = 348
cd review/baseline/p1-tree
gcc -std=gnu99 -O2 -I tbtun host/test_parse.c tbtun/tbt_parse.c -o /tmp/tbt348/test_parse
/tmp/tbt348/test_parse | tail -1   # 348 checks, 0 failures

# 5) 当前 = 357
cd /home/kunweiz/tbtun-worktrees/docs5
source ~/.cache/tbtun-build/env.sh && make check

# 6) 五键拒绝用例
grep -n 'framing removed\|chunk removed\|max_retry removed\|uid removed\|dev removed\|src is not a key' host/test_parse.c
```

## 6. 证据边界（哪些数字不在 git 里）

| 数字 | 出处 | 能否由本仓库 git 复现 |
|---|---|---|
| 365 | `review/baseline/02-make-check.txt:6` | 否；旧树归档 |
| 311 | `review/analysis/p3-contract.md:590` | 否；删除后的归档 |
| 314 | `review/analysis/p3-contract.md:670` | 否；归档 |
| 348 | `review/baseline/p1-freeze.md:58` + `review/baseline/p1-tree` | **是**；构建 p1-tree |
| 355 | `694b335` | **是**；构建该提交 |
| 357 | `HEAD` | **是**；`make check` |
| 65 | `365 + 11 − 311`（§12.3 的 +11） | 否；由归档数字反推 |
| 34 | `348 − 314` | 否；由归档数字反推 |

这张表是本文的边界：**能复现的 348/355/357 用命令复现；不能复现的
365/311/314/65/34 只作为归档值引用，不冒充当前测试计数。**
