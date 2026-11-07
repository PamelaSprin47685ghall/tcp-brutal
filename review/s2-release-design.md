# 0.1.0 发布与 0.2.0 验收执行方案

只读设计，不执行 commit/push、不改代码、不部署。全部结论来自当前工作树
`2a8d509`（`origin/master` 之后 278 个 commit）、`git status --porcelain`
62 项、`.65`（`192.168.123.65`）与 `45.11.1.62` 的只读 SSH 探测。

---

## 0. 勘察事实

| 项 | 值 |
|---|---|
| 工作树 HEAD | `2a8d509f37a3ad2cbae4cbfc01a3b345426b951a` |
| 分支状态 | `master`，领先 `origin/master` 278 个 commit |
| `git status --porcelain` | 62 项：14 个 `M` + 1 个 `D` + 46 个未跟踪条目 |
| 本地 tag | `v1.0.0`…`v2.0.1`（6 个） |
| 远端 tag | 与本地逐字相同 6 个；`v0.1.0` 未占用 |
| 当前模块版本 | `0.0.1`（`tbtun/tbt_uapi.h:30-32`） |
| `.65` 内核 | `6.1.172`，aarch64，Ubuntu 26.04 |
| `.65` 工具 | `curl 8.18.0`、`wget`、`sha256sum`、`python3`、`dd`、`fallocate`；**无 busybox** |
| `.65` dkms | **未安装**；apt 候选 `3.2.2-1ubuntu1` |
| `.65` 头文件 | `/lib/modules/6.1.172/build -> /usr/src/linux-headers-6.1.172`，含 `Module.symvers` |
| `.65` 模块压缩/签名 | `CONFIG_MODULE_COMPRESS_NONE=y`，`CONFIG_MODULE_SIG` 未设置 |
| `.65` 空间 | `/` 42 G 可用；`/tmp` 为 tmpfs 3.9 G |
| far 内核 | `7.0.0-42-generic`，x86_64 |
| far 工具 | `busybox 1.37.0`（含 `httpd`）、`python3`、`curl`、`wget`、`sha256sum` |
| far 空间 | `/` 13 G 可用；`/tmp` 为 tmpfs **478 M** |
| far 模块压缩 | `CONFIG_MODULE_COMPRESS_ZSTD=y` |
| 端口占用 | 两端 9000/9001/5202/5210 当前全部空闲 |

---

## 1. 0.1.0 版本号改动点

### 1.1 版本权威链（只读确认）

```
tbtun/tbt_uapi.h:30-32  TBT_VERSION_MAJOR/MINOR/PATCH
        │
        ├─ tbtun/tbt_uapi.h:33   TBT_VERSION = (M<<16)|(m<<8)|p
        ├─ tbtun/tbt_core.c:132  pr_info("tbtun %u.%u.%u loaded: ...")
        ├─ tbtun/tbt_core.c:163  MODULE_VERSION(__stringify(...))
        └─ scripts/mkdkmsconf.sh:54-56  version_field 读取三个宏
                 │
                 ├─ scripts/mkdkmsconf.sh:79  PACKAGE_VERSION="${VERSION}"
                 └─ Makefile:243-244          ./scripts/mkdkmsconf.sh > dkms.conf
```

`scripts/mkdkmsconf.sh:24-48` 的 `version_field` 要求每个宏**恰好一个**数字
`#define`，解析失败即 `die`，不会回退旧值。`scripts/install_dkms.sh:319-322`
通过 `mkdkmsconf.sh --version` 取同一值，`:337-341` 生成 stage 的 `dkms.conf`
并逐字校验 `PACKAGE_VERSION="$VERSION"`。

### 1.2 必须改的文件:行与目标值

| # | 文件:行 | 现值 | 目标值 | 说明 |
|---|---|---|---|---|
| 1 | `tbtun/tbt_uapi.h:31` | `#define TBT_VERSION_MINOR 0` | `... 1` | 唯一权威，0.0.1 → 0.1.0 |
| 2 | `tbtun/tbt_uapi.h:32` | `#define TBT_VERSION_PATCH 1` | `... 0` | 同上；`:30` MAJOR 保持 `0`，`:33` 组合宏不变 |
| 3 | `README.md:91` | `` `tbtun/tbt_uapi.h` (currently `0.0.1`). `` | `(currently `0.1.0`)` | Packaging 段 |
| 4 | `README.md:256` | `in 0.0.1.` | `in 0.1.0.` | “no DNS, no IPv6” 的能力边界 |
| 5 | `README.zh.md:67` | ``（当前 `0.0.1`）`` | `（当前 `0.1.0`）` | 与 3 对应 |
| 6 | `README.zh.md:178` | `0.0.1 不支持 DNS，也不支持 IPv6。` | `0.1.0 不支持 …` | 与 4 对应 |
| 7 | `DESIGN.md:2446` | ``the current tree reports `0.0.1` `` | `` `0.1.0` `` | 版本小节 |
| 8 | `DESIGN.md:2456` | ``current generated value is `PACKAGE_VERSION="0.0.1"` `` | `"0.1.0"` | 打包小节 |
| 9 | `CHANGELOG.md:6` | `## [Unreleased]` | 上方插入新的 `## [Unreleased]`，本行改为 `## [0.1.0] - 2026-11-07` | 见 1.3 |
| 10 | `交接.md:593` | `### 剩余（不阻塞 0.0.1）` | `### 剩余（不阻塞 0.1.0）` | 发布后仍成立 |

**不要改**（历史事实，改了就是伪造证据）：

- `README.md:548`、`README.zh.md:421`：描述 QEMU guest 上 `tbtun/0.0.1` 的 DKMS
  验证。0.2.0 真机验收通过后另起一段更新。
- `CHANGELOG.md:272-274`、`:617-631`、`:738-752`、`:766-776`、`:811`、`:829`：
  `[0.0.1]` 节内的版本引用。
- `review/dkms-qemu/summary.md` 与 `review/final*/` 中的 `0.0.1`。

### 1.3 `CHANGELOG.md` 的改法

当前 `[Unreleased]` 覆盖 6–270 行（含 `### Known limitations` 于 261 行），其
内容全部属于 0.1.0。改法：

```diff
 ## [Unreleased]
 
+## [0.1.0] - 2026-11-07
+
 ### Added
```

即：把 `## [Unreleased]`（第 6 行）改名为 `## [0.1.0] - 2026-11-07`，并在第 6
行位置之上插入一个空的 `## [Unreleased]`。`## [0.0.1] - 2026-11-06`（272 行）
保持不动。

### 1.4 `dkms.conf` 是生成物，不是手改对象

证据链：

- `Makefile:241-244` 注释明写 “dkms.conf is generated … do not edit dkms.conf by
  hand”，规则依赖 `scripts/mkdkmsconf.sh tbtun/tbt_uapi.h .always.make`，命令
  `./scripts/mkdkmsconf.sh > dkms.conf`。
- `.gitignore:56` 与 `:263` 忽略 `dkms.conf`；`.gitignore:264` 忽略 `dkms.tar.gz`。
- `Makefile:246-247` 的 `clean-dkms.conf` 删除它；`Makefile:220` 的 `clean`
  依赖它。
- `Makefile:252-260` 的 `dkms-srcs-check` 校验 `SRCS` 覆盖 `tbtun/*.c`、
  `tbtun/*.h`。

**结论：发布时 `dkms.conf` 既不手改也不入库。** 版本变化只由
`tbtun/tbt_uapi.h` 驱动，重新生成即可。

### 1.5 版本改动的验证与回滚

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal

# 1) 权威值
./scripts/mkdkmsconf.sh --version                 # 期望：0.1.0

# 2) 生成物与权威一致
make dkms.conf
grep -F 'PACKAGE_VERSION="0.1.0"' dkms.conf       # 必须命中，且只有一行
grep -c 'PACKAGE_VERSION=' dkms.conf              # 期望：1

# 3) 模块内嵌版本（构建后）
modinfo -F version tbtun.ko                        # 期望：0.1.0

# 4) 文档引用没有漏网
grep -rnE '(^|[^0-9.])0\.0\.1([^0-9]|$)' README.md README.zh.md DESIGN.md
# 期望只剩 README.md:548 / README.zh.md:421 两条历史记录

# 5) 生成物不入库
git status --porcelain | grep dkms.conf           # 期望：空
```

判定：1–5 全过才允许进入第 2 节。任一条失败即停止，不得提交。

回滚：版本改动尚未提交时
`git checkout -- tbtun/tbt_uapi.h README.md README.zh.md DESIGN.md CHANGELOG.md 交接.md`；
已提交未推送时 `git reset --hard <该 commit 的父>`（只在本分支、无并发工作者
时）；已推送时 `git revert --no-edit <release commit>`，**不要** force-push 已
发布的 master。

---

## 2. 0.1.0 提交分组

### 2.1 当前工作树事实

`git status --porcelain` 共 62 项：15 个已跟踪修改/删除（14 个 `M` + 1 个
`D`），46 个未跟踪条目（目录按一个条目计）。`master` 领先 `origin/master`
278 个 commit。远端已有 tag `v1.0.0`、`v1.0.1`、`v1.0.2`、`v1.0.3`、`v2.0.0`、
`v2.0.1`；`v0.1.0` 未占用。

### 2.2 fix 组（9 个文件）

```
tbtun/tbt_carrier.c       +161 -11
tbtun/tbt_fec.c             +6  -0
tbtun/tbt_fec.h             +1  -0
tbtun/tbt_fec_data.c       +65  -0
tbtun/tbt_rx.c             +53  -0
tbtun/tbt_session.c        +26  -2
tbtun/tbt_tx.c             +52  -3
tbtun/tbtun.h              +39  -0
host/test_fec.c            +12  -0
```

内容（`CHANGELOG.md` 的 `### Fixed` 已逐条记录）：`parser`/`http_rx` 出生即零
（`kzalloc`）、复用路径 `memset` + `tbt_http_rx_new_message()`、
`have`/`http_have` 越界 `-EPROTO`、`fr->dropped` 初始化、`retx_copy` 先置后入
队、`tbt_tx_fbk()` 单次 `READ_ONCE(c->sk)`、FEC `epoch > max_payload` 拒绝并在
投递处二次钳制、`TBT_DIAG`/`TBT_DIAG_WARN` 收进 `debug >= 1`。

```sh
git add tbtun/tbt_carrier.c tbtun/tbt_fec.c tbtun/tbt_fec.h \
        tbtun/tbt_fec_data.c tbtun/tbt_rx.c tbtun/tbt_session.c \
        tbtun/tbt_tx.c tbtun/tbtun.h host/test_fec.c
git commit -F - <<'EOF'
fix: zero the carrier parsers and bound every receive length

A fresh carrier kmalloc'd parser and http_rx and never zeroed them, so a
dirty slab made tbt_http_rx_parse() memmove past its buffer (the 2026-11-06
near-side Oops). Both objects are now born zeroed, the reuse path resets
them, and the parser refuses have/http_have beyond their buffers with
-EPROTO.

The same audit found four more uninitialized or unbounded fields: fr->dropped,
retx_copy's publication order, tbt_tx_fbk()'s double c->sk read, and the FEC
epoch used as a delivery length. TBT_DIAG/TBT_DIAG_WARN now expand to
if (debug >= 1), so a production host is silent and the arguments are not
evaluated.

Covered by host/test_fec.c (2566 checks) and the c8/prod-smoke evidence in
review/wan-experiment/arm64/.
EOF
```

判定：提交后 `make check` 与 `bash review/lint.sh` 仍全绿；该 commit 单独
checkout 可编译。若要求更细粒度，可按“carrier 零初始化 / rx 上界 / FEC 记账 /
tx 与诊断”拆四个 commit，但每个 commit 必须单独可编译。

### 2.3 scripts 组（4 个文件）

```
review/lint.sh
review/wan-experiment/arm64/deploy_hnd0.sh
review/wan-experiment/arm64/controlled-c8-once.sh      (新)
review/wan-experiment/arm64/prod-smoke-10s.sh          (新)
```

`review/lint.sh` 的 `check_carrier_buffers` 从 `kmalloc` 改为 `kzalloc` + 复用
重置检查（`:327-340`）；`deploy_hnd0.sh` 增加 `EXPECT_SRCVERSION` 与 insmod 后
srcversion 闸门（`:321-345`）；两个新脚本是单次受控实验与 256 载波生产冒烟。

```sh
git add review/lint.sh review/wan-experiment/arm64/deploy_hnd0.sh \
        review/wan-experiment/arm64/controlled-c8-once.sh \
        review/wan-experiment/arm64/prod-smoke-10s.sh
git commit -F - <<'EOF'
review: pin the zeroed-parser invariant and gate the far deploy on srcversion

lint.sh now requires kzalloc for parser/http_rx and an explicit reset on the
reuse path, which is the invariant the 2026-11-06 crash violated.

deploy_hnd0.sh reads srcversion from the staged ko before insmod and refuses
a mismatch with EX_GATE, so a known-bad build cannot be loaded by accident.

controlled-c8-once.sh and prod-smoke-10s.sh are the single-shot harnesses:
one burst, no retry, full forensics on either side of a near-side crash.
EOF
```

### 2.4 docs 组（4 个文件，不含版本号）

```
CHANGELOG.md    （+47：Fixed 七条、Known limitations 一条）
README.md       （+75：生产部署段、崩溃根因、身份门）
README.zh.md    （+60：同上）
交接.md         （+22：第 11 节第 13 条、第 12 节步骤）
```

```sh
git add CHANGELOG.md README.md README.zh.md 交接.md
git commit -F - <<'EOF'
docs: record the parser crash, the identity gate and the production shape

The 2026-11-06 near-side Oops was a dirty slab behind parser->have, not a
network fault. README and 交接 now say so, give the srcversion identity gate,
and describe the 256-carrier production line. CHANGELOG lists each fixed
field with the failure it caused.
EOF
```

### 2.5 evidence 组

**入库范围（文本）**

| 路径 | 文件数 | 内容 |
|---|---|---|
| `review/wan-experiment/arm64/stop-65-down-20261106T2250Z/` | 18 | 崩溃现场：`STOP-REPORT.md`、`forensics-65/`（oops 段、boot0、pstore、时间窗） |
| `review/wan-experiment/arm64/controlled-c8-20261106T232415Z/` | 8 | c8 预检 |
| `review/wan-experiment/arm64/controlled-c8-20261106T232518Z/` | 5 | 第一次单次运行 |
| `review/wan-experiment/arm64/controlled-c8-20261106T232605Z/` | 4 | 第二次单次运行 |
| `review/wan-experiment/arm64/controlled-c8-20261106T232839Z/` | 38 | 决定性运行：`SUMMARY.md`、`burst.txt`、两端 stats/c9、pstore、netconsole |
| `review/wan-experiment/arm64/prod-smoke-20261106T235809Z/` | 47 | 256 载波 1 MiB 冒烟：`SUMMARY.md`、`c9-*`、`join-probes.txt` |
| `review/wan-experiment/arm64/build-fixed-2a8d509f37a3-20261106T231212Z/` | 4 | `build.log`、`identity.txt`、`src.tar.sha256`、`summary.txt` |
| `review/wan-experiment/arm64/build-prod-2a8d509f37a3-20261106T235122Z/` | 4 | 同上 |
| `review/wan-experiment/arm64/far-deploy-20261106T232058Z/` | 11 | far 部署记录 |
| `review/wan-experiment/arm64/far-prod-20261106T235114Z/` | 13 | far 生产构建与部署（`BUILD-IDENTITY.txt` 为准） |
| `review/qemu-fbk-baseline-cbd880d.log` 等 7 个 | 7 | QEMU FBK/linkflap/all4 日志 |
| `review/0001-fbk-throttle.patch`、`review/diag-trace.patch` | 2 | 诊断补丁 |

**不入库（二进制）**：`build-*/tbtun.ko`、`build-*/worktree-src.tar`、
`far-prod-*/tbtun.ko`。它们已有 sha256 记录（`identity.txt`、
`src.tar.sha256`、`BUILD-IDENTITY.txt`），入库只增加约 14 MB 且无法 diff。

```sh
EV=review/wan-experiment/arm64
git add $EV/stop-65-down-20261106T2250Z \
        $EV/controlled-c8-20261106T232415Z \
        $EV/controlled-c8-20261106T232518Z \
        $EV/controlled-c8-20261106T232605Z \
        $EV/controlled-c8-20261106T232839Z \
        $EV/prod-smoke-20261106T235809Z \
        $EV/build-fixed-2a8d509f37a3-20261106T231212Z \
        $EV/build-prod-2a8d509f37a3-20261106T235122Z \
        $EV/far-deploy-20261106T232058Z \
        $EV/far-prod-20261106T235114Z \
        review/qemu-fbk-baseline-cbd880d.log \
        review/qemu-fbk-fix-13150c5.log \
        review/qemu-fbk-full-13150c5.log \
        review/qemu-linkflap-ba25e04.log \
        review/qemu-p-master-398b36e.log \
        review/qemu-review-all4-a3bda34.log \
        review/qemu-review-master-ba25e04.log \
        review/0001-fbk-throttle.patch review/diag-trace.patch

# 把二进制移出索引（reset 只动索引，不删工作树文件）
git reset -- $EV/*/tbtun.ko $EV/*/worktree-src.tar

git diff --cached --stat | tail -3
git diff --cached --name-only | grep -E '\.(ko|tar)$'   # 期望：空
```

**不提交的临时/重复目录及理由**

| 目录/文件 | 理由 |
|---|---|
| `build-2a8d509f37a3-20261106T165118Z/`、`…T165145Z/` | 同 commit 的重复构建 |
| `build-398b36ef4e85-20261106T152145Z/`、`build-b1a5c8461f14-20261106T160351Z/`、`build-cbd880d0aee9-20261106T164841Z/` | 修复前的中间构建，已被 `build-fixed`/`build-prod` 取代 |
| `deploy-hnd0-20261106T152145Z/` 至 `…T223911Z/`（7 个） | 中间部署，结论已并入 `far-deploy`/`far-prod` |
| `smoke-*`（15 个） | 旧冒烟矩阵；`smoke-b1a5c84-…-retry`、`-30s`、`-30s-retry` 是失败重试 |
| `ladder-20261106T224401Z/` | 仅含 `c8` 子目录，是 `ladder-20261106T224003Z` 的重复运行 |
| `smoke_37b_evidence.sh`、`smoke_verdict.sh` | 中间工具，与不提交的 `smoke-*` 同进退；被 `controlled-c8-once.sh`/`prod-smoke-10s.sh` 取代 |
| `review/dkms-qemu/artifacts/guest-tbtun.ko.sha256` 的删除（`D`） | **恢复**，不作为删除提交。`review/dkms-qemu/run.sh:139` 会重新生成它；`review/goal-v6-interfaces.md:758`、`:768` 明确要求“要么恢复，要么作为有意删除单独提交，不能静默带进 merge” |

```sh
git checkout -- review/dkms-qemu/artifacts/guest-tbtun.ko.sha256
git status --porcelain review/dkms-qemu/artifacts/   # 期望：空
```

```sh
git commit -F - <<'EOF'
review: archive the crash forensics, the c8 runs and the 256-carrier smoke

stop-65-down-20261106T2250Z holds the 2026-11-06 crash: the Oops sections,
boot0, pstore and the UTC window. controlled-c8-* are the single-shot runs
that proved the fix; prod-smoke-20261106T235809Z is the 256-carrier 1 MiB
burst with both c9 lines at zero. Build and deploy records carry the ko
sha256 and srcversion; the ko and source tarballs themselves stay out of the
repository.
EOF
```

### 2.6 第 5 个 commit：版本发布（不属于四组，必须独立）

```sh
git add tbtun/tbt_uapi.h README.md README.zh.md DESIGN.md CHANGELOG.md 交接.md
git commit -F - <<'EOF'
release: 0.1.0

Bump TBT_VERSION_MINOR/PATCH to 1/0. dkms.conf is generated from these macros
and is not committed. README, README.zh and DESIGN carry the new value; the
0.0.1 DKMS record in README is left as the historical measurement it is.
EOF
```

判定：`git show --stat HEAD` 只含这 6 个文件；`./scripts/mkdkmsconf.sh
--version` 输出 `0.1.0`；`git status --porcelain` 为空。

### 2.7 `git tag v0.1.0` 与 `git push origin master --tags`

**前置检查（全部必须通过）**

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal

# 1) 工作树干净，且没有未跟踪的实验残留
git status --porcelain                      # 期望：空

# 2) 待推 commit 数是预期的 278+5
git rev-list --count origin/master..master

# 3) 远端 master 是本地祖先（可快进）
git fetch origin
git merge-base --is-ancestor origin/master master && echo FAST_FORWARD_OK

# 4) 版本链路
./scripts/mkdkmsconf.sh --version           # 0.1.0
make dkms.conf && grep -F 'PACKAGE_VERSION="0.1.0"' dkms.conf

# 5) 发布门槛
source ~/.cache/tbtun-build/env.sh && make check
make host-test-san
bash review/lint.sh                          # 期望 16/16 + 14/14
make dkms-srcs-check
make dkms-tarball && tar tzf dkms.tar.gz | head -3 && rm -f dkms.tar.gz dkms.conf

# 6) tag 不冲突（本地与远端）
git tag -l v0.1.0                            # 期望：空
git ls-remote --tags origin | grep -F 'refs/tags/v0.1.0'   # 期望：空

# 7) 记录回滚锚点
git rev-parse master | tee /tmp/tbtun-pre-release.sha
```

判定：1–7 全过。第 5 步任一失败即停止发布；`make check` 的 `test_parse` 应为
357 checks、`test_fec` 2566 checks，0 failures。

**打 tag 与推送**

```sh
git tag -a v0.1.0 -m "tbtun 0.1.0

Single-authority module version; wire TBT_CONV 0x0005 and TBT_WIRE_VERSION 1
are unchanged. Fixes the 2026-11-06 per-carrier parser crash and bounds every
receive length."

git show --stat v0.1.0 | head -5
git push origin master --tags
```

`--tags` 只新增 `v0.1.0`：本地 6 个历史 tag 与远端逐字相同
（`v1.0.0`…`v2.0.1`），已用 `git ls-remote --tags origin` 核对。

**回滚**

| 阶段 | 命令 | 说明 |
|---|---|---|
| tag 已打、未推 | `git tag -d v0.1.0` | 无外部影响 |
| tag 已推、master 未推 | `git push origin :refs/tags/v0.1.0` | 删除远端 tag；GitHub 上 release 若已建需同步删除 |
| master 已推 | `git revert --no-edit <release-commit>` 后 `git push origin master` | 保留历史，推荐 |
| master 必须回到发布前（仅无人拉取时） | `git push --force-with-lease origin $(cat /tmp/tbtun-pre-release.sha):master` | 破坏性，需主 agent 明确批准 |

注意：历史 tag `v1.0.0`–`v2.0.1` 的版本序高于 `v0.1.0`。这是既成事实，
**不删不改**（Chesterton's Fence）；在 GitHub release 说明里写明“0.1.0 是当前
发布线，v1.x/v2.x 是早期实验 tag”。

---

## 3. 1 GiB 真机验收拓扑

### 3.1 只读探测结论（决定可行性）

| 项 | `.65`（near，Ubuntu 26.04 aarch64，6.1.172） | `45.11.1.62`（far，Ubuntu x86_64，7.0.0-42-generic） |
|---|---|---|
| `curl` | 8.18.0，支持 `--max-time/--speed-time/--speed-limit` | 有 |
| `wget` / `sha256sum` / `dd` / `fallocate` | 全部有 | 全部有 |
| `busybox` | **无** | 1.37.0，含 `httpd` |
| `python3` | 有 | 有 |
| `/` 可用空间 | 42 G（`/dev/mmcblk0p2`） | 13 G（`/dev/vda2`） |
| `/tmp` | tmpfs 3.9 G | tmpfs **478 M** |
| 当前监听 9000/9001/5202/5210 | 全部空闲 | 全部空闲 |
| 模块 | 未加载 | 未加载 |

判定：1 GiB 文件**必须**放 `/root`（或 `/var/tmp`），不能放 `/tmp`；far 的
`/tmp` 只有 478 M，放了必然 `ENOSPC`。`.65` 无 busybox，HTTP 服务放在 far
侧，near 只做 `curl`。

### 3.2 拓扑与端口

```
near .65                                     far 45.11.1.62
┌──────────────────────────┐                ┌──────────────────────────────┐
│ curl http://127.0.0.1:9001│                │ busybox httpd 127.0.0.1:5210 │
│        │                 │                │        ▲                     │
│        ▼                 │  256 carriers  │        │ target=127.0.0.1:5210│
│ tbtun near listen :9001  │◄──────────────►│ tbtun far listen 0.0.0.0:9000│
│ portblock 30000-30511    │  HTTP/1.1-ish  │ portblock 31000-31511        │
└──────────────────────────┘                └──────────────────────────────┘
```

| 角色 | 地址:端口 | 用途 |
|---|---|---|
| far HTTP | `127.0.0.1:5210` | 只回环，公网不可达 |
| far tbtun | `0.0.0.0:9000` | 载体入口（nonce + `crypto=on`） |
| near tbtun | `127.0.0.1:9001` | 本地应用入口 |
| near portblock | `30000-30511` | 512 个源端口 |
| far portblock | `31000-31511` | 512 个源端口 |
| 文件路径 | far `/root/accept-1g/accept-1g.bin` | 1 GiB |
| 下载路径 | near `/root/dl-1g/accept-1g.bin` | 1 GiB |

配置形状沿用已验证的 256 生产形状：`n=4 m=4 fec=on mode=grouped w=8
carriers=256 crypto=on crypto_key=- profile=http-kal`，
`nonce=00112233445566778899aabbccddeeff`。`crypto_key=-` 的语义是“清除覆盖，
密钥由 nonce 派生”（`tbtun/tbt_parse.c:601-605`）。

### 3.3 far 侧步骤（在 `45.11.1.62` 上执行）

```bash
set -euo pipefail
FAR_DIR=/root/accept-1g
mkdir -p "$FAR_DIR"

# 1) 1 GiB 随机文件（约 10-20 s；全零文件用 fallocate 更快，但暴露不了位翻转）
dd if=/dev/urandom of="$FAR_DIR/accept-1g.bin" bs=1M count=1024 status=progress
stat -c '%s' "$FAR_DIR/accept-1g.bin"                    # 期望 1073741824
sha256sum "$FAR_DIR/accept-1g.bin" | tee "$FAR_DIR/accept-1g.bin.sha256"

# 2) HTTP 服务，只绑回环
setsid busybox httpd -f -p 127.0.0.1:5210 -h "$FAR_DIR" \
    >"$FAR_DIR/httpd.log" 2>&1 &
echo $! >"$FAR_DIR/httpd.pid"
sleep 1
ss -ltn | grep -F '127.0.0.1:5210'
curl -sSI http://127.0.0.1:5210/accept-1g.bin | grep -iE '^HTTP/|^Content-Length'
# 期望：HTTP/1.0 200 OK 且 Content-Length: 1073741824

# 3) 模块：发布产物（v0.1.0）在 far 本地构建，避免 vermagic 不匹配
git -C /home/kunweiz/Desktop/vibe/tcp-brutal archive --format=tar.gz v0.1.0 |
    ssh root@45.11.1.62 'rm -rf /root/tbtun-0.1.0 && mkdir -p /root/tbtun-0.1.0 && tar xz -C /root/tbtun-0.1.0'
ssh root@45.11.1.62 'cd /root/tbtun-0.1.0 && make -C /lib/modules/$(uname -r)/build M=$PWD modules -j2'

# 4) 身份闸门：版本 + srcversion
KO=/root/tbtun-0.1.0/tbtun.ko
modinfo -F version "$KO"        # 0.1.0
modinfo -F vermagic "$KO"       # 7.0.0-42-generic SMP preempt mod_unload modversions
WANT_SV=$(modinfo -F srcversion "$KO")

insmod "$KO" max_carriers=256 max_tunnels=1 debug=0
[ "$(cat /sys/module/tbtun/srcversion)" = "$WANT_SV" ] || { rmmod tbtun; exit 8; }
printf 'ports 31000-31511\n' > /proc/net/tbtun/ports
grep '^ports ' /proc/net/tbtun/ports        # ports 31000-31511

# 5) far 配置：target 指向 HTTP，不是 echo
printf '%s\n' 'tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5210 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=31000-31511 nonce=00112233445566778899aabbccddeeff' \
    > /proc/net/tbtun/config
cat /proc/net/tbtun/config
ss -ltn | grep -F ':9000'
```

判定：`Content-Length` 恰为 `1073741824`；`modinfo -F version` 为 `0.1.0`；
加载后 srcversion 与 ko 一致；`config` 读回是同一行（内核会渲染 `w=8`，输入
`w=8` 时逐字一致）；`tunnels` 出现 `1 far listening 0.0.0.0:9000`。

### 3.4 near 侧步骤（在 `.65` 上执行）

```bash
set -euo pipefail
# 1) 发布产物：v0.1.0 bundle，本地构建（6.1.172 aarch64）
#    工作站：git bundle create /tmp/tbtun-0.1.0.bundle v0.1.0 && scp 到 .65
rm -rf /root/tbtun-0.1.0
git clone /root/tbtun-0.1.0.bundle /root/tbtun-0.1.0
cd /root/tbtun-0.1.0 && git checkout -q v0.1.0
make -C /lib/modules/6.1.172/build M=$PWD modules -j4

KO=/root/tbtun-0.1.0/tbtun.ko
modinfo -F version "$KO"        # 0.1.0
modinfo -F vermagic "$KO"       # 6.1.172 SMP mod_unload modversions aarch64
WANT_SV=$(modinfo -F srcversion "$KO")

insmod "$KO" max_carriers=256 max_tunnels=1 debug=0
[ "$(cat /sys/module/tbtun/srcversion)" = "$WANT_SV" ] || { rmmod tbtun; exit 8; }
printf 'ports 30000-30511\n' > /proc/net/tbtun/ports
grep '^ports ' /proc/net/tbtun/ports

printf '%s\n' 'tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000 replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=0 rx_reorder_bytes=8388608 tx_window_bytes=0 tx_retain_bytes=0 carrier_stall_ms=15000 portblock=30000-30511 nonce=00112233445566778899aabbccddeeff' \
    > /proc/net/tbtun/config
cat /proc/net/tbtun/config
```

**join gate（两端都到 256，上限 180 s）**

```bash
up() { awk '$1=="tunnel"&&$2==1&&$3=="c3"{for(i=1;i<=NF;i++) if($i ~ /^up=/){print substr($i,4); exit}}' \
        /proc/net/tbtun/stats; }
for i in $(seq 1 180); do
    n=$(up)
    printf '%s near_up=%s\n' "$(date -u +%FT%TZ)" "${n:-?}" | tee -a /root/dl-1g/join-probes.txt
    [ "${n:-0}" = 256 ] && break
    sleep 1
done
[ "${n:-0}" = 256 ] || { echo 'JOIN_FAIL'; exit 9; }
```

far 侧同样读 `c3`，两端都 `up=256` 才继续。`prod-smoke` 的实测 join 在
4–20 s 内完成（token bucket `max(8, 256/4)=64/s`）。

**下载与校验**

```bash
mkdir -p /root/dl-1g
timeout 1900 curl -sS --fail --max-time 1800 \
    --speed-limit 65536 --speed-time 120 \
    -o /root/dl-1g/accept-1g.bin \
    -w 'http=%{http_code} size=%{size_download} time=%{time_total} speed=%{speed_download}\n' \
    http://127.0.0.1:9001/accept-1g.bin | tee /root/dl-1g/curl.txt

stat -c '%s' /root/dl-1g/accept-1g.bin | tee /root/dl-1g/size.txt
sha256sum /root/dl-1g/accept-1g.bin | tee /root/dl-1g/sha256.txt

# 与 far 的 hash 对比（FAR_HASH 由工作站从 far 读取）
printf '%s  %s\n' "$FAR_HASH" /root/dl-1g/accept-1g.bin | sha256sum -c -
```

### 3.5 超时与失败判定

| 阶段 | 超时 | 失败判定 |
|---|---|---|
| far 生成 1 GiB | 300 s | `stat -c %s` ≠ 1073741824 |
| httpd 就绪 | 10 s | `ss` 无 5210，或 `curl -I` 非 200，或 `Content-Length` ≠ 1073741824 |
| 两端 insmod + srcversion | 60 s | srcversion 不等（脚本立即 rmmod，退出 8） |
| join gate | 180 s | 任一端的 `c3 up` < 256；或 near SSH 连续 3 次失败 |
| curl 传输 | 总 1800 s；停滞 120 s 且速率 < 64 KiB/s | `curl` rc≠0（28 超时、56 接收失败、18 部分传输） |
| 大小 | — | `size_download` ≠ 1073741824 |
| 完整性 | — | `sha256sum -c` 失败 |
| 计数器 | 传输后 | 两端 `c9` 行的 `fec_proto_fail`、`fec_hdr_crc_fail`、`fec_payload_crc_fail`、`gaps_total` 增量非零；`rx_errs`、`connect_fail`、`alloc_fail`、`join_bad` 非零 |
| 内核 | 传输后 | 任一端 `dmesg` 出现 `BUG:`、`Oops`、`WARNING:`、`general protection fault` |
| 存活 | 全程 | near 失联且 240 s 内不恢复 → 按崩溃处理，不重试 |

吞吐参考：历史 `n=16` 为 32.75 Mbit/s，1 GiB 约 262 s；256 载波未测长流，
1800 s 上限对应最低 4.8 Mbit/s。若 `curl` 在 120 s 内速率低于 64 KiB/s，判为
停滞而非慢速。

### 3.6 证据清单

目录 `review/wan-experiment/arm64/accept-1g-<UTC>/`：

| 文件 | 来源 | 内容 |
|---|---|---|
| `SUMMARY.md` | 工作站汇总 | 判定、时间线、两端 sha256、吞吐、失败分支 |
| `timeline.txt` | 工作站 | 每个阶段 UTC 时间戳 |
| `run.log` | 工作站 | 驱动脚本自身日志 |
| `far-sha256.txt`、`far-size.txt` | far | 源文件 hash 与大小 |
| `far-httpd.log`、`far-httpd-headers.txt` | far | 服务日志与 `curl -I` 输出 |
| `far-module.txt` | far | `modinfo` version/srcversion/vermagic、`/sys/module/tbtun/srcversion` |
| `far-config.txt`、`far-tunnels.txt`、`far-ss.txt` | far | 配置回读、隧道行、监听 |
| `far-stats-pre.txt`、`far-stats-post.txt`、`far-c9-post.txt` | far | 传输前后 stats 与 c9 |
| `far-dmesg.txt` | far | 内核日志 |
| `near-curl.txt`、`near-size.txt`、`near-sha256.txt` | near | 下载结果与校验 |
| `near-module.txt`、`near-config.txt`、`near-tunnels.txt`、`near-ss.txt` | near | 同上 |
| `near-stats-pre.txt`、`near-stats-post.txt`、`near-c9-post.txt` | near | 同上 |
| `near-dmesg.txt`、`near-pstore.txt` | near | 崩溃可见性（空文件也是证据） |
| `join-probes.txt` | 两端 | 每秒 `c3 up/n` |
| `cleanup.txt` | 工作站 | 清理结果 |

### 3.7 清理与回滚

```bash
# near
rmmod tbtun 2>/dev/null || true
pkill -f 'curl .*accept-1g' 2>/dev/null || true
rm -rf /root/dl-1g
# far
rmmod tbtun 2>/dev/null || true
kill "$(cat /root/accept-1g/httpd.pid)" 2>/dev/null || pkill -f 'busybox httpd .*5210'
rm -rf /root/accept-1g
# 两端确认
! lsmod | grep -q '^tbtun ' && ! test -e /proc/net/tbtun && echo CLEAN
ss -ltn | grep -E ':9000|:9001|:5210' || echo PORTS_FREE
```

若 near 崩溃：等待 `panic=10` 自动重启，从 pstore 与 `.80` 的 netconsole 取
证据，**不重跑**同一实验（单次约束）。回滚仅指恢复主机状态，不指重试。

---

## 4. DKMS 在 `.65` 真机

### 4.1 现状与前置缺口

只读探测结果：

- `.65` **没有 dkms**（`dkms: command not found`），也没有 `/var/lib/dkms`。
- `apt-cache policy dkms` → 候选 `3.2.2-1ubuntu1`（华为云 `ubuntu-ports` 镜像）。
- `apt-get -s install dkms` 会顺带拉 `linux-headers-7.0.0-42-generic` 等；
  `apt-get -s --no-install-recommends install dkms` **只装 dkms 本身**（已核对
  输出只有 `Inst dkms` / `Conf dkms`）。
- 内核头文件齐全：`/lib/modules/6.1.172/build -> /usr/src/linux-headers-6.1.172`，
  含 `Module.symvers`（1222182 字节）。
- `gcc 15.2.0`、`make`、`kmod`（`modprobe/insmod/rmmod/depmod/modinfo`）齐全。
- `CONFIG_MODULE_SIG` 未设置，`CONFIG_MODULE_COMPRESS_NONE=y` → 安装产物是未
  压缩的 `tbtun.ko`，`install_dkms.sh` 的 `tbtun.ko*` glob 兼容。
- 与 QEMU 验证的差异：QEMU 用 dkms 3.4.4、x86_64、7.2.9 内核；真机是 dkms
  3.2.2、aarch64、6.1.172。这是本次验收要消除的差异，也是唯一的新风险。

### 4.2 版本号来源（0.2.0）

```
tbtun/tbt_uapi.h:30-32  (0,2,0)
   → scripts/mkdkmsconf.sh:54-56,79   VERSION=0.2.0
   → dkms.conf  PACKAGE_VERSION="0.2.0"
   → dkms add/build/install           /var/lib/dkms/tbtun/0.2.0/
   → modinfo -F version tbtun         0.2.0
   → /sys/module/tbtun/version        0.2.0
   → dmesg                            "tbtun 0.2.0 loaded: ..."
```

`dkms status` 的条目形如 `tbtun/0.2.0, 6.1.172, aarch64: installed`。
`install_dkms.sh:319-322` 用 `--version` 取版本，`:337-341` 校验生成的
`dkms.conf` 里 `PACKAGE_VERSION` 与之一致，`:406-412` 的 `dkms_has_version` 用
`tbtun/0.2.0,` 与 `tbtun/0.2.0:` 两种结尾匹配 installed 与 added/broken。

0.2.0 的版本改动点与第 1 节同构：`tbtun/tbt_uapi.h:31` `1→2`、`:32` `0→0`，
以及 `README.md:91/256`、`README.zh.md:67/178`、`DESIGN.md:2446/2456`、
`CHANGELOG.md` 新节。建议在 `v0.1.0` 之后开 `release/0.2.0-dkms` 分支，只含
版本号与文档引用；验收通过后合入 master，失败则丢弃分支。这样 0.2.0 验收对象
明确：**代码等于 v0.1.0，只差版本字符串**。

### 4.3 `.65` 上的精确步骤

**步骤 0：安装 dkms（唯一需要改动系统的前置）**

```bash
apt-get update
apt-get install -y --no-install-recommends dkms 2>&1 | tee /root/dkms-install.log
dkms --version                     # 期望 3.2.2
command -v dkms
```

判定：`dkms --version` 输出 `3.2.2`；`dpkg -l dkms` 为 `ii`；`ls
/var/lib/dkms` 存在（可能为空目录）。失败回滚：`apt-get remove -y --purge
dkms`。

**步骤 1：源码入机**

工作站：

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal
git bundle create /tmp/tbtun-0.2.0.bundle v0.2.0
sha256sum /tmp/tbtun-0.2.0.bundle | tee /tmp/tbtun-0.2.0.bundle.sha256
scp /tmp/tbtun-0.2.0.bundle root@192.168.123.65:/root/
```

`.65`：

```bash
rm -rf /root/tbtun-0.2.0
git clone /root/tbtun-0.2.0.bundle /root/tbtun-0.2.0
cd /root/tbtun-0.2.0
git checkout -q v0.2.0
git rev-parse HEAD                         # 必须等于工作站 tag 的 commit
./scripts/mkdkmsconf.sh --version          # 0.2.0
test -x scripts/install_dkms.sh && test -f tbtun/tbt_uapi.h
```

判定：`rev-parse HEAD` 与工作站 `git rev-parse v0.2.0` 一致；`--version` 为
`0.2.0`。失败：重新生成 bundle，不要手工改文件。

**步骤 2：干净前置**

```bash
uname -r                                   # 6.1.172
test -e /lib/modules/6.1.172/build
! lsmod | grep -q '^tbtun '
! test -e /proc/net/tbtun
dkms status                                # 期望空
! ls -d /usr/src/tbtun-* 2>/dev/null
! ls /lib/modules/6.1.172/updates/dkms/tbtun.ko* 2>/dev/null
```

任一条不满足就停止，先清理残留（见 4.5）。

**步骤 3：安装 0.2.0**

```bash
cd /root/tbtun-0.2.0
./scripts/install_dkms.sh --kernel 6.1.172 2>&1 | tee /root/dkms-0.2.0-install.log
echo "rc=$?"
```

判定（逐条，全部必须成立）：

```bash
dkms status | grep -F 'tbtun/0.2.0, 6.1.172, aarch64: installed'
modinfo -F version tbtun                       # 0.2.0
modinfo -F vermagic tbtun | cut -d' ' -f1      # 6.1.172
ls -l /lib/modules/6.1.172/updates/dkms/tbtun.ko
cat /sys/module/tbtun/version                  # 0.2.0
grep -F 'tbtun 0.2.0 loaded' <(dmesg)
for f in config tunnels ports stats peers http; do test -e /proc/net/tbtun/$f; done
grep -F '# exit code: 0' /var/lib/dkms/tbtun/0.2.0/build/make.log
```

`install_dkms.sh` 默认在安装后 `modprobe`（`:452-470`）。若 `modprobe` 失败，
脚本会打印已安装文件路径并 `exit 1`，此时按 4.5 回滚。

**步骤 4：配置回读**

```bash
printf 'tunnel 1 role=near listen=127.0.0.1:19000 peer=203.0.113.9:443\n' \
    > /proc/net/tbtun/config
cat /proc/net/tbtun/config
cat /proc/net/tbtun/tunnels
ss -ltn | grep -F '127.0.0.1:19000'
```

判定：读回为规范渲染行（含 `n=4 m=1 dup=1 profile=http-kal
keepalive=5000`）；`tunnels` 恰好一行 `1 near listening 127.0.0.1:19000
203.0.113.9:443 - 4 1 0`；`ss` 有监听。用 19000 避开 9000/9001。

清理：

```bash
echo 'del 1' > /proc/net/tbtun/config
test -z "$(cat /proc/net/tbtun/config)" && ! ss -ltn | grep -qF ':19000'
```

**步骤 5：卸载**

```bash
rmmod tbtun
```

判定：`! lsmod | grep -q '^tbtun '`；`! test -e /proc/net/tbtun`；
`dmesg | tail -5` 含 `tbtun: unloaded`；`dmesg` 无
`BUG:`/`Oops`/`WARNING:`。失败（`rmmod` 报 `in use`）：先
`echo 'del 1' > /proc/net/tbtun/config` 删隧道再试。

**步骤 6：重载**

```bash
modprobe tbtun
cat /sys/module/tbtun/version                  # 0.2.0
modinfo -F filename tbtun                      # /lib/modules/6.1.172/updates/dkms/tbtun.ko
# 配置往返重复步骤 4
printf 'tunnel 1 role=near listen=127.0.0.1:19000 peer=203.0.113.9:443\n' > /proc/net/tbtun/config
cat /proc/net/tbtun/config
echo 'del 1' > /proc/net/tbtun/config
```

判定：版本仍为 0.2.0；`modinfo` 指向 `updates/dkms`；配置往返一致；监听出现
后消失。

**步骤 7：`--uninstall`**

```bash
rmmod tbtun 2>/dev/null || true
cd /root/tbtun-0.2.0
./scripts/install_dkms.sh --uninstall 2>&1 | tee /root/dkms-0.2.0-uninstall.log
echo "rc=$?"
```

判定（残留必须为零）：

```bash
dkms status                                   # 空
! ls -d /usr/src/tbtun-* 2>/dev/null
! ls /lib/modules/6.1.172/updates/dkms/tbtun.ko* 2>/dev/null
! ls /lib/modules/6.1.172/extra/tbtun.ko* 2>/dev/null
! modinfo tbtun 2>/dev/null
! grep -q tbtun /lib/modules/6.1.172/modules.dep
! test -e /proc/net/tbtun
! lsmod | grep -q '^tbtun '
find /var/lib/dkms -maxdepth 2 -name 'tbtun*'   # 空
```

`install_dkms.sh --uninstall` 的路径覆盖：`unload_module`（`:161-171`）→
`remove_all_dkms_entries`（`:180-224`，含 broken 条目 `rm -rf`）→ 删除
`/usr/src/tbtun-*`（`:283-297`）→ 删除 `updates/dkms`、`updates`、`extra` 下的
`tbtun.ko*`（`:299-310`）→ `depmod_kernels`（`:236-246`）。

### 4.4 判定标记（与 QEMU 一致）

沿用 `review/dkms-qemu/guest-test.sh:45-63` 的格式，便于机器判定：

```
DKMS_STEP <name> rc=<n> secs=<n>
DKMS_CHECK <name> PASS|FAIL (<detail>)
DKMS_RESULT PASS|FAIL
```

判定规则照抄 `review/dkms-qemu/run.sh:143-169`：`DKMS_RESULT` 必须存在且为
PASS；`DKMS_STEP` 中不得有 `rc=[1-9]`；`DKMS_CHECK` 中不得有 ` FAIL`。真机
脚本在 `.65` 本地运行，不需要 QEMU 与 ssh 驱动。

### 4.5 失败回滚

| 失败点 | 处置 |
|---|---|
| `dkms build` 失败 | 读 `/var/lib/dkms/tbtun/0.2.0/build/make.log`；`rm -rf /var/lib/dkms/tbtun/0.2.0`；`rm -rf /usr/src/tbtun-0.2.0`；`depmod -a 6.1.172` |
| `dkms install` 失败 | 同上，再 `rm -f /lib/modules/6.1.172/updates/dkms/tbtun.ko*` |
| `modprobe` 失败 | `dmesg | tail -50`；`./scripts/install_dkms.sh --uninstall`；确认 `/proc/net/tbtun` 不存在 |
| `rmmod` 报 in use | `echo 'del 1' > /proc/net/tbtun/config` 后再 `rmmod`；仍失败则 `fuser -v /proc/net/tbtun/*` 找持有者 |
| `--uninstall` 中途失败 | 脚本以 `FAILED=1` 继续并最终 `exit 1`；按警告逐项手工：`dkms remove -m tbtun -v 0.2.0 --all`、`rm -rf /usr/src/tbtun-0.2.0`、`rm -f /lib/modules/6.1.172/{updates/dkms,updates,extra}/tbtun.ko*`、`depmod -a 6.1.172` |
| broken 条目 | `dkms status` 显示 `: broken` 时 `dkms remove` 会拒绝；直接 `rm -rf /var/lib/dkms/tbtun/0.2.0`（脚本 `:200-208` 也是这么做） |
| 需要回到“无模块”状态 | `rmmod tbtun` + 上一行全部清理；`.65` 在本次验收前从未装过 DKMS，所以“回滚”就是清空，不存在旧版本可恢复 |

验收通过后的文档动作（0.2.0 发布前）：把 `DESIGN.md:2458-2460` 的
“Real-machine DKMS install is not validated” 段改写为已验真机，引用证据目录；
`README.md:548` 与 `README.zh.md:421` 的 `tbtun/0.0.1` 更新为 0.2.0 并注明
平台（aarch64/6.1.172）。

---

## 5. 缺陷清单结案表模板

### 5.1 字段

| 字段 | 取值 |
|---|---|
| `ID` | `A-H-01`、`A-M-01`、`B-01`…`B-07`、`C-01`…、`D-<findings 编号>` |
| `来源` | A=本次审计高危/中危；B=prod-code 未修 7 项；C=文档漂移；D=`review/findings.md` 的 OPEN |
| `严重度` | critical / high / medium / low / nit |
| `一句话` | 现象 + 触发条件，不超过 60 字 |
| `状态` | `FIXED` / `FIXED (static)` / `FIXED (contract)` / `记录` / `撤销` |
| `处置理由` | 为什么这样结案；`记录` 必须写明“为什么这一版不修” |
| `证据` | `文件:行` 或可复制执行的命令 + 期望输出；不接受“已修”两个字 |
| `复核人/日期` | 独立复核者与 UTC 日期 |

状态语义沿用 `review/findings.md:56-61`：`FIXED` = 修复存在且有命名检查覆盖；
`FIXED (static)` = 修复存在但无运行时测试直达该单元；`FIXED (contract)` = 代码
有意不提供旧行为，测试契约已改写并通过；`记录` = 缺陷存在，本版不修，理由
成立；`撤销` = 复核后不成立。

### 5.2 来源 A：本次审计高危/中危

主审计输入的编号与描述不在仓库文本中，模板固定字段与判定规则，行内容由主
审计逐条填入。每条必须能落到下面三种证据之一：

```sh
# 代码位置
grep -n '<符号>' tbtun/<file>.c
# 运行时
grep -E '<检查名>' review/wan-experiment/arm64/*/SUMMARY.md
# 静态检查
bash review/lint.sh | grep -E '<check-name>'
```

门槛：0.1.0 发布前，A 类**全部**为
`FIXED`/`FIXED (static)`/`FIXED (contract)`；任何 `记录` 必须由主 agent 明确
批准并写进 `CHANGELOG.md` 的 `### Known limitations`。

### 5.3 来源 B：prod-code 未修 7 项

仓库中没有这份编号清单。可核对的 7 个候选（若主审计的 7 项不同，按主审计
覆盖，字段不变）：

| ID | 候选 | 证据锚点 | 本次能否关闭 |
|---|---|---|---|
| B-01 | FEC 无 NACK/选择性重传，丢数据 + 丢足够校验时块内停滞 | `CHANGELOG.md:263-270` | 否，`记录` |
| B-02 | rate epoch 未实现 | `CHANGELOG.md` 中 “does not implement rate epochs” | 否，`记录` |
| B-03 | bench CSV header 缺 `retx_other`/`data_tx`/`retx_data_ppm` | `交接.md:603-604` | 可修，非发布阻塞 |
| B-04 | 真机 DKMS/`modprobe`/`rmmod` 未验证 | `交接.md:601-602`、`DESIGN.md:2458-2460` | **是**，第 4 节关闭 |
| B-05 | `n=4` 吞吐运行间方差 8.38–11.37 Mbit/s | `交接.md:599-600` | 否，`记录` |
| B-06 | `n=8`/`n=16` 仅单次 40 MiB 记录 | `交接.md:599-600` | 否，`记录` |
| B-07 | 透明空洞推进未实现，只按精确 seq 前进 | `交接.md` 决策记录表 | 否，`FIXED (contract)`（有意设计） |

门槛：B 类全部有状态；B-04 在 0.2.0 验收后改为 `FIXED` 并附第 4 节证据；其余
`记录` 项必须在 `CHANGELOG.md` 的 `### Known limitations` 有对应条目，否则不
允许 `记录`。

### 5.4 来源 C：文档漂移

| ID | 漂移 | 处置 |
|---|---|---|
| C-01 | `tbtun/tbt_uapi.h:30-32` 与 `README.md:91/256`、`README.zh.md:67/178`、`DESIGN.md:2446/2456` 版本不一致 | 第 1 节修复，`FIXED` |
| C-02 | `DESIGN.md:2458-2460` “Real-machine DKMS install is not validated” | 0.2.0 验收后改写，`FIXED` |
| C-03 | `交接.md:593` “不阻塞 0.0.1” | 随 0.1.0 改，`FIXED` |
| C-04 | `review/findings.md:752` 的 spec-drift 表 14 行全部标 `RESOLVED`，需逐行复核代码 | 按 `review/final-checklist.md:801` 复核，`FIXED`/`撤销` |
| C-05 | `README.md:548`、`README.zh.md:421` 的 DKMS 记录仍是 0.0.1/x86_64 | 历史事实，保留；0.2.0 验收后补新段，`记录` |

复核命令：

```sh
grep -nE '(^|[^0-9.])0\.0\.1([^0-9]|$)' README.md README.zh.md DESIGN.md 交接.md
grep -c '^| .*| RESOLVED' review/findings.md      # spec-drift 表行数
```

### 5.5 来源 D：`review/findings.md` 的 OPEN

当前快照（`review/findings.md:56-61` 的 Final status 表）**没有** `| OPEN` 行；
`grep -n '| OPEN' review/findings.md` 返回空。历史说明在 `:200`。因此 D 类当前
为空，但必须复核，因为表是快照而非实时状态：

```sh
grep -n '| OPEN' review/findings.md               # 期望：空
grep -nE '\| (critical|high|medium|low|nit) \| (OPEN|FIXED)' review/findings.md | wc -l
bash review/lint.sh                               # 16/16 + 14/14
source ~/.cache/tbtun-build/env.sh && make check  # test_parse 357、test_fec 2566、0 failures
```

复核后若出现 `OPEN`，一律进表，不得留在正文。

### 5.6 结案表模板（直接复制使用）

```markdown
| ID | 来源 | 严重度 | 一句话 | 状态 | 处置理由 | 证据 | 复核人/日期 |
|---|---|---|---|---|---|---|---|
| A-H-01 | 本次审计 | high | <现象> | FIXED | <修复要点> | `tbtun/<file>.c:<line>`；`<检查名> PASS` | <name> <YYYY-MM-DD> |
| A-M-01 | 本次审计 | medium | <现象> | 记录 | <为何本版不修> | `<命令>` 输出 `<期望>` | <name> <YYYY-MM-DD> |
| B-04 | prod-code | medium | 真机 DKMS 未验证 | FIXED | 第 4 节全流程通过 | `review/wan-experiment/arm64/dkms-65-<UTC>/`；`DKMS_RESULT PASS` | <name> <YYYY-MM-DD> |
| B-07 | prod-code | low | 透明空洞未实现 | FIXED (contract) | 只按精确 seq 前进是有意设计 | `交接.md` 决策记录；`tbt_rx.c:33-90` | <name> <YYYY-MM-DD> |
| C-01 | 文档漂移 | low | 版本引用不一致 | FIXED | 第 1 节 | `./scripts/mkdkmsconf.sh --version` → `0.1.0` | <name> <YYYY-MM-DD> |
| D-<n> | findings | <sev> | <现象> | <状态> | <理由> | `review/findings.md:<line>` | <name> <YYYY-MM-DD> |
```

### 5.7 结案门槛

| 时点 | 门槛 |
|---|---|
| 0.1.0 发布前 | A 类无 `记录`（除非主 agent 批准且写入 Known limitations）；B 类每项有状态；C-01/C-03 为 `FIXED`；D 类无 `OPEN` |
| 0.1.0 发布后 | 表格随 tag 归档到 `review/findings.md` 顶部或 `review/release-0.1.0.md`；不得只存在于对话中 |
| 0.2.0 验收前 | B-04 必须 `FIXED`；C-02 改写；1 GiB 验收与 DKMS 验收各有独立证据目录与 `SUMMARY.md` |
| 0.2.0 发布前 | 1 GiB 的 sha256 一致、两端 c9 错误计数为零、无 Oops；DKMS 七步全 PASS；否则不发 |

---

## 6. 关键事实与残余风险

1. **`.65` 没有 dkms**，必须 `apt-get install -y --no-install-recommends
   dkms`（3.2.2-1ubuntu1）。不加 `--no-install-recommends` 会顺带安装
   `linux-headers-7.0.0-42-generic` 等包，改变系统状态。
2. **`.65` 的 `/tmp` 是 3.9 G tmpfs，far 的 `/tmp` 只有 478 M**。1 GiB 文件
   必须放 `/root`，否则要么占内存，要么 `ENOSPC`。
3. **far 的 busybox 有 `httpd`，`.65` 没有 busybox**。HTTP 服务只能放 far，
   near 用 `curl`。
4. **1 GiB 长流没有历史吞吐数据**。256 载波只跑过 1 MiB/5 s。1800 s 超时
   对应最低 4.8 Mbit/s；若实测低于此，判失败而不是调大超时。
5. **远端已有 `v1.0.0`–`v2.0.1` tag**，版本序高于 `v0.1.0`。不删除、不改写；
   在 release 说明中解释。
6. **`review/dkms-qemu/artifacts/guest-tbtun.ko.sha256` 的删除必须恢复**，不得
   静默带入发布（`review/goal-v6-interfaces.md:758`、`:768` 已明确要求）。
7. **“prod-code 未修 7 项”的编号不在仓库中**。第 5.3 节给出 7 个可核对候选；
   若主审计编号不同，用主审计覆盖，表结构与门槛不变。
