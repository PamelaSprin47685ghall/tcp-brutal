# dkms-65 — 0.2.0 在 `.65` 真机的 DKMS 验收

在 `.65`（Ubuntu 26.04，aarch64，6.1.172）上走完 DKMS 七步：安装、加载、
配置回读、`rmmod`、`modprobe`、再回读、`--uninstall`。判定与回滚来自
`review/s2-release-design.md` §4。

## 1. 组成

| 文件 | 运行位置 | 作用 |
|---|---|---|
| `run-dkms-65.sh` | `.65`（root） | 七步流程 + 预检 + 五类失败回滚 + 判定标记 |
| `judge-dkms-log.sh` | 任意 | 按 §4.4 规则判读日志，输出 `DKMS_JUDGE PASS|FAIL` |

## 2. 前置条件

1. **dkms 3.2.2 必须已安装**。`.65` 出厂没有 dkms，脚本**不会**替你装。
   唯一允许的安装命令：
   ```sh
   apt-get update
   apt-get install -y --no-install-recommends dkms
   dkms --version        # 期望 3.2.2
   ```
   不加 `--no-install-recommends` 会顺带安装 `linux-headers-7.0.0-42-generic`
   等包，改变系统状态。
2. 源码树已在 `.65` 上，版本为 0.2.0：
   ```sh
   /root/tbtun-0.2.0/scripts/mkdkmsconf.sh --version   # 0.2.0
   ```
   版本由 `tbtun/tbt_uapi.h:30-32` 派生，不要手改 `dkms.conf`。
3. 内核头文件与工具链齐全：
   `/lib/modules/$(uname -r)/build`（含 `Module.symvers`）、`gcc`、`make`、
   `modprobe/insmod/rmmod/depmod/modinfo`。
4. **硬件喂狗者必须正在运行**。`rmmod`/`modprobe` 期间不许停；停掉喂狗者等于
   启动 44 s 复位倒计时。脚本只读扫描 `/proc/<pid>/fd` 确认持有者，不碰设备。
5. 干净起点：模块未加载、`dkms status` 为空、无 `/usr/src/tbtun-*`、无
   `updates/dkms/tbtun.ko*`。`.65` 本次验收前从未装过 DKMS，所以"回滚"就是
   清空，不存在旧版本可恢复。

## 3. 用法

```sh
# 工作站：把 0.2.0 源码送到 .65（bundle 由主代理生成）
scp /tmp/tbtun-0.2.0.bundle root@192.168.123.65:/root/
ssh root@192.168.123.65 'rm -rf /root/tbtun-0.2.0 &&
    git clone /root/tbtun-0.2.0.bundle /root/tbtun-0.2.0 &&
    cd /root/tbtun-0.2.0 && git checkout -q v0.2.0'

# .65：把脚本也送过去
scp review/wan-experiment/arm64/dkms-65/run-dkms-65.sh \
    root@192.168.123.65:/root/dkms-65/

# .65：预检（只读）
sh /root/dkms-65/run-dkms-65.sh --preflight

# .65：完整七步
REPO=/root/tbtun-0.2.0 EXPECT_HEAD=<v0.2.0 的 commit> \
    sh /root/dkms-65/run-dkms-65.sh --run | tee /root/dkms-65-run.log

# 工作站：取回证据并判读
scp -r root@192.168.123.65:/root/dkms-65-<UTC> review/wan-experiment/arm64/
review/wan-experiment/arm64/dkms-65/judge-dkms-log.sh \
    review/wan-experiment/arm64/dkms-65-<UTC>/run.log
```

`KEEP=1` 时不自动回滚，保留现场供取证；此时失败后需要人工按 §5 清理。

## 4. 七步与判定

| 步 | 名称 | 关键检查 |
|---|---|---|
| 1 | `source` | `$REPO`、`install_dkms.sh`、`tbt_uapi.h`、`mkdkmsconf.sh --version=0.2.0`、可选 `EXPECT_HEAD` |
| 2 | `clean` | 无模块、无 `/proc/net/tbtun`、`dkms status` 空、无 `/usr/src/tbtun-*`、无 `updates/dkms/tbtun.ko*` |
| 3 | `install` | `install_dkms.sh --kernel $REL`；`dkms status` 含 `tbtun/0.2.0, 6.1.172, aarch64: installed`；`modinfo` 版本/vermagic；`updates/dkms` 下的 ko；`/sys/module/tbtun/version`；dmesg `tbtun 0.2.0 loaded`；六个 procfs 文件；`make.log` 的 `# exit code: 0` |
| 4 | `config` | 写 tunnel 1（`127.0.0.1:19000`），读回含 `n=4 m=1 dup=1 profile=http-kal keepalive=5000`；`tunnels` 含 `1 near listening 127.0.0.1:19000` 与 peer；`ss` 有监听；`del 1` 后 config 空、监听消失 |
| 5 | `rmmod` | `rmmod` rc=0；`lsmod` 无；`/proc/net/tbtun` 消失；dmesg 有 `unloaded`；最后 50 行无 `BUG:/Oops/WARNING:/general protection fault` |
| 6 | `modprobe` | 版本仍 0.2.0；`modinfo -F filename` 指向 `updates/dkms/tbtun.ko*`；配置往返一致；监听出现后消失 |
| 7 | `uninstall` | `--uninstall` rc=0；`dkms status` 空；无 `/usr/src/tbtun-*`；`updates/dkms`/`updates`/`extra` 无 ko；`modinfo` 无解析；`modules.dep` 不提 tbtun；`/proc/net/tbtun` 消失；`lsmod` 无；`/var/lib/dkms` 无残留 |

标记格式（与 `review/dkms-qemu/guest-test.sh` 一致）：

```
DKMS_STEP <name> rc=<n> secs=<n>
DKMS_CHECK <name> PASS|FAIL (detail)
DKMS_ROLLBACK class=<...>
DKMS_RESULT PASS|FAIL (checks_failed=<n> rollback=<0|1>)
```

判定规则：`DKMS_RESULT` 必须是 PASS；`DKMS_STEP` 无 `rc=[1-9]`；`DKMS_CHECK`
无 ` FAIL`；不出现 `DKMS_ROLLBACK`。`judge-dkms-log.sh` 把这四条固化成退出码。

## 5. 五类失败回滚

| 类 | 触发 | 动作 |
|---|---|---|
| `build` | `dkms build` 失败（`dkms status` 无条目） | 读 `make.log`；`rm -rf /var/lib/dkms/tbtun/0.2.0`、`/usr/src/tbtun-0.2.0`；`depmod -a 6.1.172` |
| `install` | `dkms install` 失败 | 同上，再删 `/lib/modules/6.1.172/updates/dkms/tbtun.ko*` |
| `modprobe` | 安装成功但加载失败、配置回读失败 | `install_dkms.sh --uninstall`；确认 `/proc/net/tbtun` 不存在 |
| `rmmod-in-use` | `rmmod` 报 `in use`、卸载后内核 splat | `del 1` 后重试 `rmmod`；`fuser -v /proc/net/tbtun/*` 找持有者 |
| `uninstall-partial` | `--uninstall` 中途失败或残留非零 | `dkms remove -m tbtun -v 0.2.0 --all`；删 `/usr/src/tbtun-0.2.0`；删三处 ko；`depmod -a`；broken 条目直接 `rm -rf /var/lib/dkms/tbtun/0.2.0` |

脚本按失败点自动选类；`KEEP=1` 时只打印类名不执行，便于先取证。

## 6. 证据

`/root/dkms-65-<UTC>/`：

| 文件 | 内容 |
|---|---|
| `run.log` | 完整标记流（判读对象） |
| `source.log`、`clean.log`、`install.log`、`config.log`、`config-del.log`、`rmmod.log`、`modprobe.log`、`config2.log`、`config2-del.log`、`uninstall.log` | 每步原始输出 |
| `config-readback.txt`、`tunnels-readback.txt`、`ss-19000.txt` | 配置回读 |
| `dmesg-after-install.txt`、`dmesg-after-rmmod.txt` | 内核日志 |
| `rollback-*.log` | 回滚输出（若发生） |

## 7. 仍未验证的前置条件

- **dkms 3.2.2 尚未安装**。脚本预检会 FAIL 并打印唯一允许的安装命令；安装
  本身是本次验收前唯一需要改动系统的动作，需主代理确认后执行。
- **apt 候选版本**：设计记录为 `3.2.2-1ubuntu1`（华为云 `ubuntu-ports` 镜像）；
  实际以 `apt-cache policy dkms` 为准。版本不符时脚本 FAIL，不降级继续。
- **`install_dkms.sh` 的行号引用**（`:161-171`、`:180-224` 等）来自 release
  设计，0.2.0 分支改动后可能漂移；脚本只依赖命令行接口，不依赖行号。
- **`/proc/net/tbtun/http` 是否存在**：步骤 3 按设计检查六个文件；若该版本
  不导出 `http`，该项会 FAIL，需要按实际接口调整清单（不是放宽判定，而是
  修正清单）。
- **`.65` 的 `modules.dep` 由 `depmod` 重写**：卸载后检查 `grep tbtun`；
  若别的内核目录也装过 tbtun，需逐个清理。
