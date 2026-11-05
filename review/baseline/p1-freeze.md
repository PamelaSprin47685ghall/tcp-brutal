# P1 树冻结与复验（2026-11-05 13:27）

目的：P2 大改失败并回退后，给当前可编译树留一个**单步回退参照点**，并记录已完成的复验。
本文只记录事实，不修改、不回退、不重置任何源码。

## 1. 快照

| 项 | 值 |
|---|---|
| 路径 | `review/baseline/p1-tree/` |
| 文件数 | 47 个源/文档文件 + `SHA256SUMS`（共 48 个文件） |
| 大小 | 1.3 MB |
| 校验 | `cd review/baseline/p1-tree && sha256sum -c SHA256SUMS` → 全部 OK |
| 覆盖范围 | `tbtun/`、`host/`、`tests/qemu/`、`Makefile`、`scripts/`、`README.md`、`README.zh.md`、`DESIGN.md`、`CHANGELOG.md` |
| 排除 | `*.o`、`*.ko`、`*.cmd`、`*.mod`、`*.mod.c`；`benches/` 未复制（其未提交改动不在本快照范围） |

`host/` 下的三个已编译测试二进制（`test_parse`、`test_fuzz`、`test_http`、`tbt_burst`）被一并复制，因为排除规则只针对内核构建产物。它们不是源码，回退时可以重新构建。

### 1.1 `dkms.conf` 不在当前树中

`ls dkms.conf` → `No such file or directory`；`dkms.tar.gz` 也不存在。
为了让快照可独立用于 DKMS 回退，快照内用仓库脚本生成了一份：

```sh
./scripts/mkdkmsconf.sh > review/baseline/p1-tree/dkms.conf
```

生成结果：`PACKAGE_NAME="tbtun"`，`PACKAGE_VERSION="1.0.0"`。
这是**生成物，不是原树文件**；原树当前没有 `dkms.conf`。这一点与 P1 阶段根目录存在 `dkms.conf`（`1.0.0.r74.ee0e5e6`）不同，是 P2 回退后的状态。

## 2. 树状态

原始记录：`review/baseline/p1-tree/tree-state.txt`。

| 项 | 值 |
|---|---|
| 时间 | 2026-11-05T13:27:21+08:00 |
| `git rev-parse HEAD` | `ee0e5e632c90704c77a4364182e185fe0aed61e1` |
| 工作树 | dirty：`tbtun/`、`host/`、`tests/`、`benches/`、`review/`、`adversarial/`、`docs/`、`DESIGN.md`、`CHANGELOG.md` 等未跟踪；`brutal*` 删除未提交；`Makefile`、`README*.md`、`benches/*`、`scripts/*` 已修改 |
| `git diff --stat` | 12 files changed, 1408 insertions(+), 1586 deletions(-)；其中 `scripts/install_dkms.sh` 1221 行变化、`scripts/mkdkmsconf.sh` 85 行、`benches/qemu/init` 173 行、`benches/run.sh` 81 行 |
| `uname -r` | `7.2.9-1-MANJARO` |
| `tbtun.ko` | 5,521,408 B，mtime 2026-11-05 13:04 |
| `sha256sum tbtun.ko` | `5d2e1209f639726dbb4056f34c5b11958a1ced05015abf941327720da8ff57d4` |

`make check` 没有重建模块（命中 13:04 的对象），所以本节的模块 sha 在复验前后一致。

## 3. 已完成的复验

### 3.1 `make check` — PASS

```sh
source ~/.cache/tbtun-build/env.sh && make check
```

| 项 | 结果 |
|---|---|
| 退出码 | 0 |
| `test_parse` | **348 checks, 0 failures** |
| `test_fuzz` | `checks=8349443 failures=0 buffers=200000 bodies=200000` |
| `test_http` | `checks=249 failures=0` |
| h11 交叉校验 | accepted POST /api/v2/upload，4 chunks / 112 body bytes，headers in order |
| 耗时 | 2.32 s |

原始输出：`review/baseline/p1-freeze/01-make-check.txt`。

348 与用户给的预期一致；与旧基线的 365 不同，原因是 P3 删除了 `framing/chunk/max_retry/dev/uid` 等键后 `test_parse` 用例减少。这不是本次复验的失败。

### 3.2 `review/lint.sh` — PASS

| 项 | 结果 |
|---|---|
| 退出码 | 0 |
| checks | 15/15 passed, 0 failed |
| self-tests | 12/12 passed, 0 failed |

原始输出：`review/baseline/p1-freeze/02-lint.txt`。

### 3.3 QEMU 探针 — 部分完成，被中止

命令：

```sh
tests/qemu/run.sh --probes A,B,E,G,I,J,K --timeout 600 \
  --log review/baseline/p1-freeze/03-qemu-run1.log
```

在 `probe I` 进行到约 67 s 时，按用户要求终止 QEMU（开发工程师需要环境）。已完成的部分：

| 探针 | 结果 | 证据 |
|---|---|---|
| A | **PASS** | `03-qemu-run1.log:429-434` |
| B | **PASS** | `03-qemu-run1.log:438-520` |
| E | **PASS** | `03-qemu-run1.log:521-570` |
| G | **PASS** | `03-qemu-run1.log:571-635` |
| I | **未完成**（运行中被终止） | `03-qemu-run1.log:636` 起，console 最后是 hole gatefail 打印 |
| J | **未跑** | — |
| K | **未跑** | — |

console 中没有 oops/BUG/WARNING；最后一行是 QEMU 收到 SIGTERM：

```text
qemu-system-x86_64: terminating on signal 15 from pid 2028733 (omp)
```

**未复验的部分：**

- 探针 I 的 control 与 n=4 两个阶段都没有判定结果。
- J/K 各 3/3 没有开始；当前只有“未跑”，不能引用任何 3/3 结论。
- C/D/F/H 也不在本轮命令内。

## 4. bench — 未复验

按用户要求，探针中止后不再启动 bench；`benches/run.sh throughput --n 4 --mib 40` 与 `--n 1 --mib 40` **没有在本次运行**。
因此 `review/baseline/p1-freeze/` 下没有新的 throughput CSV。

用户提供的当前树数字（**未由本次复验确认**，仅记录，用于后续对照）：

| 配置 | P2 回退后（用户报告） | P1 基线（README/CHANGELOG） | 差异 |
|---|---|---|---|
| `n=4` `wire/app` | 1.0128 | 1.0005 | +1.23% |
| `n=1` `wire/app` | 1.0626 | 1.0316 | +3.0% |

这两个差异在单次 40 MiB 运行的噪声范围内还是真实回退，**需要下一次完整的 n=4/n=1 bench 才能判定**；本次没有足够数据，不做结论，也不据此改代码。

`n=8`/`n=16` 按用户要求未跑。

## 5. 环境释放

复验中止后检查：

```text
pgrep -af qemu-system        → none
ss -ltn | grep 19100/19101   → ports free
/tmp/tbtun-bench.lock        → no lock
```

QEMU 进程已退出，hostfwd 端口空闲，bench flock 未持有。环境已交给开发工程师。

## 6. 结论

- **快照可冻结**：`review/baseline/p1-tree/` 的 47 个文件 + `SHA256SUMS` 完整，校验通过；`tbtun.ko` 的 sha 与 HEAD 已记录。
- **host 侧全绿**：`make check` 348/0、`lint` 15/15 + 12/12。
- **QEMU 只完成 A/B/E/G 各 1 次**：四项 PASS；I 被中止，J/K 未跑，3/3 不成立。
- **bench 未复验**：n=4/n=1 的新数字不存在；用户报告的 1.0128/1.0626 只作为待验证线索。
- **没有修改任何源码**；没有回退或重置；`benches/` 未提交改动未被触碰。
