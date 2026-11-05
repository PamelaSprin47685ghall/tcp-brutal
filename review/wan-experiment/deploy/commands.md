# tbtun WAN deployment — 70031a8 baseline

Date: 2026-11-06. Hosts: JYSZ (7.2.9-1-MANJARO), HND0 45.11.1.62 (7.0.0-42-generic).
Password handling: local root password supplied only via `su -P` pty; never stored here.

## 0. Source state

- Repo HEAD was `8d676dd` (one commit after `70031a8`, touching only `.gitignore`
  and `review/baseline/p1-tree/dkms.conf`). Module sources are identical:
  `git diff --stat 70031a8 HEAD -- tbtun Makefile scripts` is empty.
- Worktree clean at build time.

## 1. Local build (JYSZ)

```sh
cd /home/kunweiz/Desktop/vibe/tcp-brutal
source ~/.cache/tbtun-build/env.sh
make KERNEL_DIR="$KDIR" PAHOLE="$PAHOLE" -j2
sha256sum tbtun.ko
# 0b8f0006a7a10fe33e6e35e927aaf916047e142528bc32729ed04826fdccdb63
modinfo tbtun.ko | grep -E 'version|vermagic'
# version: 0.0.1
# vermagic: 7.2.9-1-MANJARO SMP preempt mod_unload
# load (root via su -P; password never in argv)
insmod /home/kunweiz/Desktop/vibe/tcp-brutal/tbtun.ko
lsmod | grep tbtun
ls /proc/net/tbtun/
```

## 2. HND0 build

```sh
ssh root@45.11.1.62 'mkdir -p /tmp/tbtun-exp-70031a8'
rsync -a --exclude=.git --exclude='*.o' --exclude='*.ko' --exclude=review \
      --exclude='*.cmd' --exclude=.tmp_versions \
      /home/kunweiz/Desktop/vibe/tcp-brutal/ root@45.11.1.62:/tmp/tbtun-exp-70031a8/
ssh root@45.11.1.62 'cd /tmp/tbtun-exp-70031a8 && rm -f Module.symvers && \
  make -C /lib/modules/$(uname -r)/build M=$PWD PAHOLE=false -j1 modules'
# sha256 854267873bfcbbcd11cf6af0709d7814b902e934ff8898813091c52c24e27a4f
# vermagic 7.0.0-42-generic SMP preempt mod_unload modversions
ssh root@45.11.1.62 'cd /tmp/tbtun-exp-70031a8 && insmod ./tbtun.ko'
```

Build warnings (both hosts): `-Wframe-larger-than=1024` in tbt_procfs/tbt_carrier/
tbt_tx; HND0 also skips BTF (`vmlinux` unavailable). No errors.

## 3. Direct-WAN bypass for mihomo (JYSZ)

`ip route get 45.11.1.62` originally returned `via 198.18.0.2 dev Meta table 2022`.
Temporary, non-persistent bypass (deleted at teardown):

```sh
ip rule add to 45.11.1.62/32 lookup main pref 100
```

main already had `default via 192.168.123.254 dev wlp4s0 src 192.168.123.69`.
Verification:

```sh
ip route get 45.11.1.62
# 45.11.1.62 via 192.168.123.254 dev wlp4s0 src 192.168.123.69 uid 0
curl --interface wlp4s0 -4 -sS https://ifconfig.me
# 171.120.34.226
ssh -o BatchMode=yes root@45.11.1.62 'hostname'   # hnd0.kw92.cyou
```

HND0 sees carrier sources as `171.120.34.226:<port>` (public NAT address of JYSZ),
never 198.18.x.x. `nft list ruleset` has no meta/tproxy rules for this path.

Teardown (run at end of experiment):

```sh
ip rule del to 45.11.1.62/32 lookup main pref 100
# equivalent: ip rule del pref 100
```

## 4. HND0 target

iperf3 3.20 present on HND0. Started as a transient systemd unit so it survives
the SSH session (KillUserProcesses would kill a plain setsid/nohup job):

```sh
systemd-run --unit=tbtun-iperf3-5201 \
  --property=StandardOutput=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  --property=StandardError=file:/tmp/tbtun-exp-70031a8/iperf3-server.log \
  /usr/bin/iperf3 -s -B 127.0.0.1 -p 5201
# PID 699110, LISTEN 127.0.0.1:5201
```

## 5. Tunnel configuration

Written to `/proc/net/tbtun/config` on each host (far first).

Far (HND0):

```
tunnel 1 role=far listen=0.0.0.0:9000 target=127.0.0.1:5201 n=4 m=1 profile=raw nonce=00112233445566778899aabbccddeeff portblock=31000-31099 hole_budget_ms=30000 carrier_stall_ms=15000 keepalive=2000
```

Near (JYSZ):

```
tunnel 1 role=near listen=127.0.0.1:9001 peer=45.11.1.62:9000 n=4 m=1 profile=raw nonce=00112233445566778899aabbccddeeff portblock=30000-30099 hole_budget_ms=30000 carrier_stall_ms=15000 keepalive=2000
```

Deviations from the default template and why (measured RTT to HND0 is 200-290 ms):

- `hole_budget_ms=30000` (default 500): at 200 ms RTT the default declares holes
  unrecoverable before an ACK can round-trip.
- `carrier_stall_ms=15000` (default 3000): default fires during normal TCP
  retransmit backoff on this path.
- `keepalive=2000` (default 5000): faster PAD so idle carriers stay classified up.
- `portblock=` is recorded/rendered only; actual source ports come from the global
  `/ports` block (default `40000-40063`, not reconfigured).

## 6. Smoke

JYSZ has no iperf3/nc/socat. The HND0 iperf3 3.20 binary and its non-glibc
libraries were copied to `/tmp/tbtun-iperf3` and run with
`LD_LIBRARY_PATH=/tmp/tbtun-iperf3/usr/lib/x86_64-linux-gnu`.

```sh
LD_LIBRARY_PATH=/tmp/tbtun-iperf3/usr/lib/x86_64-linux-gnu \
  /tmp/tbtun-iperf3/usr/bin/iperf3 -c 127.0.0.1 -p 9001 -t 15 -w 2M
```

## 7. State to leave running

- JYSZ: tbtun.ko loaded, tunnel 1 near, LISTEN 127.0.0.1:9001.
- HND0: tbtun.ko loaded, tunnel 1 far, LISTEN 0.0.0.0:9000.
- HND0: tbtun-iperf3-5201.service, PID 699110, LISTEN 127.0.0.1:5201.
- JYSZ: ip rule pref 100 for 45.11.1.62 (temporary).

Unload/teardown if ever needed:

```sh
# JYSZ
ip rule del pref 100
# both hosts
rmmod tbtun
# HND0
systemctl stop tbtun-iperf3-5201
```
