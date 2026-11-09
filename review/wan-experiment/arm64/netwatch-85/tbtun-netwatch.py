#!/usr/bin/env python3
"""tbtun-netwatch - hardware watchdog feeder gated on the LAN gateway.

Why this shape
--------------
Target board: LubanCat-5 V2 (RK3588, dw_wdt at feaf0000.watchdog), host .85.
Capabilities read from the running kernel image (System.map plus vmlinuz,
`dw_wdt_ident` at ffff80000982fd00 = file offset 0x182fd00):

    options        0x8180  SETTIMEOUT | MAGICCLOSE | KEEPALIVEPING
    identity       "Synopsys DesignWare Watchdog"
    timeout        44 s     (fixed; re-checked with WDIOC_GETTIMEOUT on open)
    nowayout       0        (CONFIG_WATCHDOG_NOWAYOUT=n, no dw_wdt.nowayout=1)

MAGICCLOSE=1 changes the close semantics: closing the descriptor without
first writing the magic byte 'V' does NOT stop the watchdog; the hardware
keeps counting. Only an explicit 'V' write followed by close, or
WDIOS_DISABLECARD while nowayout=0, can disarm it. This process never writes
'V', never calls WDIOS_DISABLECARD and never closes the descriptor on
purpose. With nowayout=0 that is a contract kept in code, not a
kernel-enforced guarantee; the deployment report lists the optional
`dw_wdt.nowayout=1` cmdline step.

Contract
--------
* One process owns /dev/watchdog0. Nothing else may open it (EBUSY).
* The feeder thread writes one byte every FEED_SECS. It never blocks on I/O
  other than that single write, so a slow curl cannot starve the watchdog.
* The prober thread checks the gateway every PROBE_SECS. After
  FAIL_THRESHOLD consecutive bad answers it decides: reset, or keep feeding
  because the reset budget is spent or WATCHDOG_DRY_RUN=1.
* For the first BOOT_GRACE seconds of system uptime (read from /proc/uptime,
  not from our own start time) the prober does nothing at all: no probe, no
  failure count, no verdict. It only logs that it is in grace. This is the
  guard against a reset loop - a board that boots into a broken network must
  stay up long enough for someone to notice, not reboot every 44 seconds.
* This process dying for any reason (signal, crash, OOM, kernel panic) also
  stops the feed, because the descriptor closes and the hardware keeps
  counting. There is no `Restart=always`: a restart would resume feeding and
  hide the fault.

Timing budget (all values are env-overridable, defaults are the design)
----------------------------------------------------------------------
    grace 600 s + 3 probes x 30 s + hardware 44 s = 734 s total tolerance
    feed every 5 s (8.8x margin against the fixed 44 s timeout)

Reset budget
------------
`STATE_DIR/armed-log` records one epoch per armed start. Entries older than
RESET_WINDOW_S are dropped at start, so the budget is "armed starts in the
last 30 minutes". Each armed start is at most one reset away, so the board
may reset RESET_BUDGET times; when the window already holds more than
RESET_BUDGET armed starts the prober keeps feeding and only logs. Thirty
minutes of healthy probes clear the record. Grace-period failures never touch
the budget because grace runs no probes.

Handover
--------
.85 has no previous feeder: no process holds /dev/watchdog0 and the
bootloader does not arm it. The handover is therefore "no feeder -> this
process", and the only vulnerable interval is between open() (which arms the
device) and the first feed, which is milliseconds. install-85.sh refuses to
start if any other process already holds the device. The EBUSY retry loop is
kept for the case where someone starts a second feeder by mistake; it is not
used in the normal path.

What counts as a good answer
----------------------------
On .85 the gateway answered `HTTP/1.0 401 Unauthorized` with
`WWW-Authenticate: Basic realm="JDC-1"` and a 140-byte body, in ~1.5 ms,
3/3 times (2026-11-08). So:

  * curl exit status 0
  * HTTP status code in WATCHDOG_EXPECT_STATUS (default 100-499)
  * the status line parses as `HTTP/1.[01] <3 digits>` and agrees with curl

5xx, 000, a non-HTTP first line, or a curl error all count as failure. The
body is not matched on purpose: firmware upgrades change it.

`--selftest` is read-only. It never opens /dev/watchdog0: on this board
`open()` itself arms the watchdog, so a "probe" that opens the device is a
write, not a read.
"""

import errno
import fcntl
import os
import re
import struct
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time

DEV = os.environ.get("WATCHDOG_DEV", "/dev/watchdog0")
GW = os.environ.get("WATCHDOG_PROBE_URL", "http://192.168.123.254:80/")
LOG = os.environ.get("WATCHDOG_LOG", "/var/log/tbtun-watchdog.log")
STATE_DIR = os.environ.get("WATCHDOG_STATE_DIR", "/var/lib/tbtun-netwatch")
BOOT_COUNT = os.path.join(STATE_DIR, "boot-count")
ARMED_LOG = os.path.join(STATE_DIR, "armed-log")
PRE_RESET = os.path.join(STATE_DIR, "pre-reset.log")


def _int(name, default):
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return int(raw)
    except ValueError:
        print("bad %s=%r, using %d" % (name, raw, default), file=sys.stderr)
        return default


def _bool(name, default=False):
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "yes", "true", "on")


FEED_SECS = _int("WATCHDOG_FEED_S", 5)          # 44 s / 5 s = 8.8x margin
PROBE_SECS = _int("WATCHDOG_PROBE_S", 30)
PROBE_TIMEOUT = _int("WATCHDOG_PROBE_TIMEOUT_S", 5)
CONNECT_TIMEOUT = _int("WATCHDOG_CONNECT_TIMEOUT_S", 3)
FAIL_THRESHOLD = _int("WATCHDOG_FAIL_THRESHOLD", 3)   # 3 x 30 s = 90 s
HARD_TIMEOUT = _int("WATCHDOG_HARD_TIMEOUT_S", 44)    # measured, fixed
BOOT_GRACE = _int("WATCHDOG_GRACE_S", 600)
GRACE_LOG_SECS = _int("WATCHDOG_GRACE_LOG_S", 120)
HEALTHY_RESET_SECS = _int("WATCHDOG_HEALTHY_RESET_S", 1800)
RESET_BUDGET = _int("WATCHDOG_RESET_BUDGET", 5)
RESET_WINDOW_S = _int("WATCHDOG_RESET_WINDOW_S", 1800)
OPEN_RETRY_SECS = _int("WATCHDOG_OPEN_RETRY_S", 120)
OPEN_RETRY_STEP = 2
HEARTBEAT_SECS = _int("WATCHDOG_HEARTBEAT_S", 300)
DRY_RUN = _bool("WATCHDOG_DRY_RUN", False)
EXPECT_STATUS_SPEC = os.environ.get("WATCHDOG_EXPECT_STATUS", "100-499")

TOTAL_TOLERANCE = BOOT_GRACE + PROBE_SECS * FAIL_THRESHOLD + HARD_TIMEOUT

STATUS_RE = re.compile(r"^HTTP/1\.[01] ([1-5][0-9]{2})")
CODE_RE = re.compile(r"^[1-5][0-9]{2}$")

stop = threading.Event()
give_up = threading.Event()
fail_count = 0
fail_lock = threading.Lock()
observing = False          # at threshold but not allowed to stop feeding


def _parse_status_spec(spec):
    allow = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            allow.update(range(int(lo), int(hi) + 1))
        else:
            allow.add(int(part))
    if not allow:
        raise ValueError("empty status set")
    return allow


try:
    STATUS_ALLOWED = _parse_status_spec(EXPECT_STATUS_SPEC)
except (ValueError, TypeError):
    STATUS_ALLOWED = set(range(100, 500))
    print("bad WATCHDOG_EXPECT_STATUS=%r, using 100-499"
          % EXPECT_STATUS_SPEC, file=sys.stderr)


# ---------------------------------------------------------------------------
# Device capabilities. These ioctls are read-only, but they require an open
# descriptor, and open() is what arms dw_wdt. They are therefore called only
# after open_watchdog() succeeded, never from --selftest.

WDIOC_GETSUPPORT = 0x80285702       # _IOR('W', 2, struct watchdog_info)
WDIOC_GETTIMEOUT = 0x80045707       # _IOR('W', 7, int)
WDIOC_GETBOOTSTATUS = 0x80045704    # _IOR('W', 4, int)
WDIOF_BITS = (
    (0x0001, "OVERHEAT"), (0x0002, "FANFAULT"),
    (0x0004, "EXTERN1"), (0x0008, "EXTERN2"),
    (0x0010, "POWERUNDER"), (0x0020, "CARDRESET"),
    (0x0040, "POWEROVER"), (0x0080, "SETTIMEOUT"),
    (0x0100, "MAGICCLOSE"), (0x0200, "PRETIMEOUT"),
    (0x0400, "ALARMONLY"), (0x8000, "KEEPALIVEPING"),
)
DEVICE_INFO = os.path.join(STATE_DIR, "device-info.log")


def _ioctl_int(fd, request):
    buf = bytearray(4)
    fcntl.ioctl(fd, request, buf, True)
    return struct.unpack_from("<i", buf, 0)[0]


def device_caps(fd):
    """Best-effort capability report. Never changes device state."""
    caps = {}
    try:
        buf = bytearray(40)          # struct watchdog_info
        fcntl.ioctl(fd, WDIOC_GETSUPPORT, buf, True)
        options, firmware = struct.unpack_from("<II", buf, 0)
        ident = bytes(buf[8:40]).split(b"\0", 1)[0].decode("ascii", "replace")
        caps["options"] = options
        caps["identity"] = ident
        caps["firmware"] = firmware
        caps["flags"] = ",".join(n for b, n in WDIOF_BITS if options & b)
    except OSError as e:
        caps["support_error"] = str(e)
    try:
        caps["timeout"] = _ioctl_int(fd, WDIOC_GETTIMEOUT)
    except OSError as e:
        caps["timeout_error"] = str(e)
    try:
        caps["bootstatus"] = _ioctl_int(fd, WDIOC_GETBOOTSTATUS)
    except OSError as e:
        caps["bootstatus_error"] = str(e)
    return caps


def read_nowayout():
    """Module parameter when the built-in driver exposes it; else unknown."""
    try:
        with open("/sys/module/dw_wdt/parameters/nowayout") as f:
            return f.read().strip()
    except OSError:
        pass
    # The built-in driver does not expose /sys/module/dw_wdt on this kernel.
    # Fall back to the cmdline token; mark it unverified.
    try:
        with open("/proc/cmdline") as f:
            for tok in f.read().split():
                if tok in ("dw_wdt.nowayout=1", "dw_wdt.nowayout=0"):
                    return "%s (cmdline, unverified)" % tok.split("=", 1)[1]
    except OSError:
        pass
    return "unknown"


def record_device_info(caps):
    """Log and persist one capability line per armed start."""
    timeout = caps.get("timeout")
    options = caps.get("options")
    bootstatus = caps.get("bootstatus")
    flags = caps.get("flags", "unknown")
    ident = caps.get("identity", "unknown")
    nowayout = read_nowayout()
    bs_flags = ",".join(
        n for b, n in WDIOF_BITS
        if bootstatus is not None and bootstatus & b)
    log("opened %s: timeout=%ss options=%s[%s] identity=%r nowayout=%s"
        % (DEV,
           timeout if timeout is not None else "unknown",
           "0x%04x" % options if options is not None else "unknown",
           flags, ident, nowayout))
    log("bootstatus=%s[%s]%s"
        % ("0x%04x" % bootstatus if bootstatus is not None else "unknown",
           bs_flags,
           " CARDRESET: previous reset was caused by the watchdog"
           if bootstatus is not None and bootstatus & 0x0020 else ""))
    if timeout is not None and timeout != HARD_TIMEOUT:
        log("WARNING: hardware timeout is %ds, expected %ds; feed=%ds is %s"
            % (timeout, HARD_TIMEOUT, FEED_SECS,
               "still safe" if FEED_SECS * 3 < timeout else "UNSAFE"))
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(DEVICE_INFO, "a") as f:
            f.write("%s timeout=%s options=%s flags=%s identity=%r "
                    "nowayout=%s bootstatus=%s\n"
                    % (time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                       timeout,
                       "0x%04x" % options if options is not None else "unknown",
                       flags, ident, nowayout,
                       "0x%04x" % bootstatus
                       if bootstatus is not None else "unknown"))
            f.flush()
            os.fsync(f.fileno())
    except OSError as e:
        log("cannot write %s: %s" % (DEVICE_INFO, e))


def kmsg(msg):
    """Feed the existing tbtun-kmsg-relay (-> 192.168.123.80:6666)."""
    try:
        with open("/dev/kmsg", "w") as f:
            f.write("<4>tbtun-netwatch: %s\n" % msg)
    except OSError:
        pass


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%dT%H:%M:%S%z"), msg)
    print(line, flush=True)          # -> journal (SyslogIdentifier)
    try:
        with open(LOG, "a") as f:
            f.write(line + "\n")
            f.flush()
            os.fsync(f.fileno())
    except OSError:
        pass
    kmsg(msg)


def probe_once():
    """Return (ok, detail). Never raises."""
    with tempfile.TemporaryDirectory(prefix="netwatch.") as td:
        hdr = os.path.join(td, "h")
        body = os.path.join(td, "b")
        cmd = [
            "curl", "-sS",
            "--connect-timeout", str(CONNECT_TIMEOUT),
            "-m", str(PROBE_TIMEOUT),
            "-o", body, "-D", hdr,
            "-w", "%{http_code}", GW,
        ]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=PROBE_TIMEOUT + 3)
        except subprocess.TimeoutExpired:
            return False, "curl killed after %ds" % (PROBE_TIMEOUT + 3)
        except OSError as e:
            return False, "curl exec failed: %s" % e

        if r.returncode != 0:
            return False, "curl rc=%d: %s" % (r.returncode,
                                              r.stderr.strip()[:120])

        code = (r.stdout or "").strip()
        if not CODE_RE.match(code):
            return False, "status code %r not a 3-digit HTTP code" % code
        if int(code) not in STATUS_ALLOWED:
            return False, "status code %s outside %s" % (code, EXPECT_STATUS_SPEC)

        try:
            with open(hdr, "r", errors="replace") as f:
                first = f.readline().strip()
        except OSError:
            first = ""

        m = STATUS_RE.match(first)
        if not m:
            return False, "bad status line %r" % first[:80]
        if m.group(1) != code:
            return False, "curl said %s but header says %s" % (code, m.group(1))
        return True, "code=%s" % code


# ---------------------------------------------------------------------------
# reset budget: armed-log holds one epoch per armed start inside the window.

def read_armed_times():
    times = []
    try:
        with open(ARMED_LOG) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    times.append(int(line))
                except ValueError:
                    continue
    except OSError:
        return []
    return times


def _write_atomic(path, text):
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def write_boot_count(n):
    try:
        _write_atomic(BOOT_COUNT, "%d\n" % n)
    except OSError as e:
        log("cannot persist boot count: %s" % e)


def read_boot_count():
    try:
        with open(BOOT_COUNT) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return len(read_armed_times())


def write_armed_times(times):
    try:
        _write_atomic(ARMED_LOG, "".join("%d\n" % t for t in times))
        write_boot_count(len(times))
    except OSError as e:
        log("cannot persist armed log: %s" % e)


def record_armed_boot():
    """Append this armed start, drop entries outside the window, return count."""
    now = int(time.time())
    times = [t for t in read_armed_times() if t > now - RESET_WINDOW_S]
    times.append(now)
    write_armed_times(times)
    return len(times)


def armed_in_window():
    now = int(time.time())
    return len([t for t in read_armed_times() if t > now - RESET_WINDOW_S])


def clear_budget():
    write_armed_times([])


def uptime_secs():
    """System uptime, or None if /proc/uptime is unreadable.

    Grace is measured from boot, not from our own start: a service started by
    hand on a board that has been up for hours must probe immediately, and a
    service that starts 30 s after boot must still wait out the full 600 s.
    """
    try:
        with open("/proc/uptime") as f:
            return float(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def write_pre_reset(reason, detail):
    """Evidence written before any decision to stop feeding (also in dry-run)."""
    up = uptime_secs()
    text = (
        "utc=%s\nreason=%s\ndetail=%s\nuptime=%s\narmed_in_window=%d\n"
        "boot_count=%d\npid=%d\ndry_run=%d\n"
        % (time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), reason, detail,
           "unknown" if up is None else "%.1f" % up, armed_in_window(),
           read_boot_count(), os.getpid(), 1 if DRY_RUN else 0))
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(PRE_RESET, "a") as f:
            f.write(text + "\n")
            f.flush()
            os.fsync(f.fileno())
    except OSError as e:
        log("cannot write %s: %s" % (PRE_RESET, e))


def open_watchdog():
    """Retry through the handover window (old feeder still holds the fd)."""
    deadline = time.monotonic() + OPEN_RETRY_SECS
    last_log = 0.0
    first = True
    while True:
        try:
            fd = os.open(DEV, os.O_WRONLY)
            # open() has armed the device; capability ioctls are read-only.
            record_device_info(device_caps(fd))
            return fd
        except OSError as e:
            if e.errno != errno.EBUSY:
                log("open %s failed: %s" % (DEV, e))
                return None
            now = time.monotonic()
            if first or now - last_log >= 10.0:
                log("open %s: busy (another feeder holds it), retrying every "
                    "%d s for %d s" % (DEV, OPEN_RETRY_STEP, OPEN_RETRY_SECS))
                first = False
                last_log = now
            if now >= deadline:
                log("open %s: still busy after %d s, not arming"
                    % (DEV, OPEN_RETRY_SECS))
                return None
            time.sleep(OPEN_RETRY_STEP)


def feeder(fd):
    next_feed = time.monotonic()
    next_beat = time.monotonic() + HEARTBEAT_SECS
    fed = False
    while not stop.is_set():
        if give_up.is_set():
            # Deliberately keep the descriptor open and stop writing.
            stop.wait(1.0)
            continue
        now = time.monotonic()
        if now >= next_feed:
            try:
                os.write(fd, b"1")
            except OSError as e:
                log("watchdog write failed: %s" % e)
                return
            next_feed = now + FEED_SECS
            if not fed:
                # Proof for the handover check: the new owner really feeds.
                log("first feed ok (watchdog kept alive)")
                fed = True
            elif now >= next_beat:
                up = uptime_secs()
                log("feed ok, uptime=%s"
                    % ("unknown" if up is None else "%.0fs" % up))
                next_beat = now + HEARTBEAT_SECS
        stop.wait(0.5)


def decide(detail):
    """Return True to keep feeding (observe), False to stop feeding."""
    armed = armed_in_window()
    if DRY_RUN:
        log("DRY-RUN: %d consecutive gateway failures (last: %s); a real run "
            "would stop feeding now, hardware reset in <=%d s. Feeding "
            "continues." % (FAIL_THRESHOLD, detail, HARD_TIMEOUT))
        write_pre_reset("dry-run", detail)
        return True
    if armed > RESET_BUDGET:
        log("reset budget exhausted: %d armed starts in the last %d s > %d; "
            "keeping the feed, no reset, manual action required"
            % (armed, RESET_WINDOW_S, RESET_BUDGET))
        write_pre_reset("budget-exhausted", detail)
        return True
    log("VERDICT: %d consecutive gateway failures (last: %s); "
        "stopping the watchdog feed, hardware reset in <=%d s"
        % (FAIL_THRESHOLD, detail, HARD_TIMEOUT))
    write_pre_reset("reset", detail)
    kmsg("GIVING UP: gateway %s unusable, hardware watchdog will reset the "
         "board within %d s" % (GW, HARD_TIMEOUT))
    try:
        subprocess.run(["sync"], timeout=10)
        log("sync done")
    except Exception as e:
        log("sync failed/skipped: %s" % e)
    give_up.set()
    return False


def prober():
    global fail_count, observing
    healthy_since = None
    in_grace = None          # None: not decided yet
    last_grace_log = 0.0

    while not stop.is_set():
        if give_up.is_set():
            return

        up = uptime_secs()
        if up is None:
            # Unreadable uptime is not evidence of a broken gateway. Stay in
            # grace and keep feeding: the safe direction is "do not reset".
            log("cannot read /proc/uptime; staying in grace, feeding only")
            stop.wait(GRACE_LOG_SECS)
            continue

        if up < BOOT_GRACE:
            # Grace: no probe, no failure count, no verdict. Nothing here can
            # reach decide(), so a boot with a dead gateway stays up.
            if in_grace is not True:
                log("boot grace start: uptime=%.1fs < %ds; feeding only, "
                    "no probe, no failure count, no reset"
                    % (up, BOOT_GRACE))
                in_grace = True
                last_grace_log = time.monotonic()
            elif time.monotonic() - last_grace_log >= GRACE_LOG_SECS:
                log("boot grace: uptime=%.1fs, %.0fs left"
                    % (up, BOOT_GRACE - up))
                last_grace_log = time.monotonic()
            stop.wait(PROBE_SECS)
            continue

        if in_grace is None:
            # Started after the grace window (hand start, or a late unit):
            # probe now, there is no boot-relative schedule to keep.
            log("uptime=%.1fs already past the %ds grace; probing begins"
                % (up, BOOT_GRACE))
            in_grace = False
        elif in_grace is True:
            # The documented budget is 600 + 3 x 30 + 44 = 734 s: the first
            # probe is one interval after the grace ends (uptime 630), so the
            # third failure lands at 690 and the hardware resets at 734.
            # Align to boot time, not to when this loop noticed the boundary.
            log("boot grace end: uptime=%.1fs >= %ds; first probe at "
                "uptime=%ds, then every %ds"
                % (up, BOOT_GRACE, BOOT_GRACE + PROBE_SECS, PROBE_SECS))
            in_grace = False
            delay = BOOT_GRACE + PROBE_SECS - up
            if delay > 0:
                stop.wait(delay)
            continue

        ok, detail = probe_once()

        with fail_lock:
            if ok:
                if fail_count:
                    log("gateway recovered after %d failed probes" % fail_count)
                fail_count = 0
                observing = False
                if healthy_since is None:
                    healthy_since = time.monotonic()
                elif time.monotonic() - healthy_since >= HEALTHY_RESET_SECS:
                    if read_boot_count():
                        log("%d s healthy, clearing reset budget"
                            % HEALTHY_RESET_SECS)
                        clear_budget()
                    healthy_since = time.monotonic()
            else:
                healthy_since = None
                if observing:
                    # Already at the threshold and told to keep feeding: do
                    # not re-log the same condition every 30 s.
                    pass
                else:
                    fail_count += 1
                    log("gateway probe failed %d/%d: %s"
                        % (fail_count, FAIL_THRESHOLD, detail))
                    if fail_count >= FAIL_THRESHOLD:
                        if decide(detail):
                            observing = True
                        else:
                            return

        stop.wait(PROBE_SECS)


# ---------------------------------------------------------------------------
# --selftest: read-only. Never opens DEV.

def fd_holders():
    """PIDs holding DEV open, read from /proc only. No open() of the device."""
    holders = []
    try:
        real = os.path.realpath(DEV)
    except OSError:
        real = DEV
    try:
        pids = [d for d in os.listdir("/proc") if d.isdigit()]
    except OSError:
        return holders
    for pid in pids:
        fddir = "/proc/%s/fd" % pid
        try:
            fds = os.listdir(fddir)
        except OSError:
            continue
        for fd in fds:
            try:
                target = os.readlink(os.path.join(fddir, fd))
            except OSError:
                continue
            if target in (DEV, real):
                try:
                    with open("/proc/%s/comm" % pid) as f:
                        comm = f.read().strip()
                except OSError:
                    comm = "?"
                holders.append((int(pid), comm))
    return holders


def _cmd(argv, timeout=10):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)
    return r.stdout.strip(), None


def selftest(do_probe=True):
    rows = []

    def rec(name, status, detail=""):
        rows.append(status)
        suffix = " (%s)" % detail if detail else ""
        print("SELFTEST %s %s%s" % (name, status, suffix), flush=True)

    # 1. device node, without opening it.
    try:
        st = os.stat(DEV)
        if not stat.S_ISCHR(st.st_mode):
            rec("device_node", "FAIL", "%s is not a character device" % DEV)
        else:
            rec("device_node", "PASS",
                "char %d:%d" % (os.major(st.st_rdev), os.minor(st.st_rdev)))
    except OSError as e:
        rec("device_node", "FAIL", "%s: %s" % (DEV, e))

    # 2. current holder (read-only /proc scan).
    holders = fd_holders()
    if holders:
        rec("device_holder", "PASS",
            " ".join("pid=%d(%s)" % (p, c) for p, c in holders))
    else:
        rec("device_holder", "PASS", "none; watchdog not armed")

    # 3. uptime and grace.
    up = uptime_secs()
    if up is None:
        rec("uptime", "FAIL", "/proc/uptime unreadable; runtime stays in grace")
    elif up < BOOT_GRACE:
        rec("uptime", "PASS",
            "%.1fs; grace %.0fs left, no probe no reset" % (up, BOOT_GRACE - up))
    else:
        rec("uptime", "PASS", "%.1fs; grace over, probing active" % up)

    # 4. timing invariants: 600 + 30x3 + 44 = 734, feed 3x under the timeout.
    problems = []
    if FEED_SECS * 3 >= HARD_TIMEOUT:
        problems.append("feed %ds x3 >= timeout %ds" % (FEED_SECS, HARD_TIMEOUT))
    if TOTAL_TOLERANCE != 734:
        problems.append("total tolerance %ds != 734" % TOTAL_TOLERANCE)
    if PROBE_SECS * FAIL_THRESHOLD < 60:
        problems.append("failure window %ds < 60s"
                        % (PROBE_SECS * FAIL_THRESHOLD))
    if problems:
        rec("timing", "FAIL", "; ".join(problems))
    else:
        rec("timing", "PASS",
            "grace=%d probe=%dx%d hard=%d total=%d feed=%d"
            % (BOOT_GRACE, PROBE_SECS, FAIL_THRESHOLD, HARD_TIMEOUT,
               TOTAL_TOLERANCE, FEED_SECS))

    # 4b. kernel watchdog configuration (read-only files).
    cfg = "/boot/config-%s" % os.uname().release
    try:
        with open(cfg) as f:
            cfg_text = f.read()

        def cfgval(key):
            for line in cfg_text.splitlines():
                if line.startswith(key + "="):
                    return line.split("=", 1)[1]
                if line.strip() == "# " + key + " is not set":
                    return "n"
            return "?"

        rec("kernel_config", "PASS",
            "NOWAYOUT=%s SYSFS=%s DW_WATCHDOG=%s"
            % (cfgval("CONFIG_WATCHDOG_NOWAYOUT"),
               cfgval("CONFIG_WATCHDOG_SYSFS"),
               cfgval("CONFIG_DW_WATCHDOG")))
    except OSError as e:
        rec("kernel_config", "SKIP", "%s: %s" % (cfg, e))

    # 5. systemd must not feed the watchdog.
    out, err = _cmd(["systemctl", "show", "-p", "RuntimeWatchdogUSec",
                     "--value"])
    if out is None:
        rec("systemd_watchdog", "SKIP", "systemctl unavailable: %s" % err)
    elif out in ("", "0", "0s", "off"):
        rec("systemd_watchdog", "PASS", "RuntimeWatchdogUSec=%r" % out)
    else:
        rec("systemd_watchdog", "FAIL",
            "RuntimeWatchdogUSec=%r; systemd would mask our gate" % out)

    # 6. unit contract and fd, when the unit exists.
    state, err = _cmd(["systemctl", "show", "-p", "LoadState", "--value",
                       "tbtun-netwatch"])
    if state != "loaded":
        rec("unit_restart", "SKIP", "unit not loaded (%s)" % (state or err))
        rec("unit_fd", "SKIP", "unit not loaded")
    else:
        restart, err = _cmd(["systemctl", "show", "-p", "Restart", "--value",
                             "tbtun-netwatch"])
        if restart == "no":
            rec("unit_restart", "PASS", "Restart=no")
        else:
            rec("unit_restart", "FAIL", "Restart=%r, expected no" % restart)

        pid, _ = _cmd(["systemctl", "show", "-p", "MainPID", "--value",
                       "tbtun-netwatch"])
        if not pid or pid == "0":
            # Loaded but not started yet (install-85.sh runs this before the
            # handover) is a normal state, not a failure.
            active, _ = _cmd(["systemctl", "is-active", "tbtun-netwatch"])
            if active == "active":
                rec("unit_fd", "FAIL", "unit active but MainPID=0")
            else:
                rec("unit_fd", "SKIP", "unit loaded, not running (state=%s)"
                    % (active or "unknown"))
        else:
            got = [p for p, _c in fd_holders() if p == int(pid)]
            if got:
                rec("unit_fd", "PASS", "pid=%s holds %s" % (pid, DEV))
            else:
                rec("unit_fd", "FAIL",
                    "pid=%s does not hold %s" % (pid, DEV))

    # 7. reset budget state (informational).
    rec("reset_budget", "PASS",
        "armed_in_window=%d budget=%d window=%ds boot_count=%d"
        % (armed_in_window(), RESET_BUDGET, RESET_WINDOW_S, read_boot_count()))

    # 8. kernel lines, informational.
    out, err = _cmd(["dmesg"])
    if out is None:
        rec("dmesg", "SKIP", "dmesg unavailable: %s" % err)
    else:
        hits = [l for l in out.splitlines() if "watchdog" in l.lower()]
        rec("dmesg", "PASS",
            "%d watchdog lines; last=%s"
            % (len(hits), hits[-1][:100] if hits else "none"))

    # 9. gateway, the only part that touches the network.
    if not do_probe:
        rec("gateway", "SKIP", "--no-probe")
    else:
        oks = 0
        last = ""
        for _ in range(5):
            ok, detail = probe_once()
            last = detail
            if ok:
                oks += 1
        if oks == 5:
            rec("gateway", "PASS", "5/5 %s" % last)
        else:
            rec("gateway", "FAIL", "%d/5 ok, last=%s" % (oks, last))

    # 10. log file, informational.
    try:
        with open(LOG) as f:
            tail = f.read().splitlines()[-3:]
        rec("log", "PASS", "last=%s" % (tail[-1][:120] if tail else "empty"))
    except OSError:
        rec("log", "SKIP", "%s missing" % LOG)

    failed = [r for r in rows if r == "FAIL"]
    print("SELFTEST_RESULT %s" % ("FAIL" if failed else "PASS"), flush=True)
    return 3 if failed else 0


def main():
    args = sys.argv[1:]
    if args and args[0] in ("-h", "--help"):
        print("usage: %s [--selftest [--no-probe]]" % sys.argv[0])
        print("  (no argument: arm %s and feed it)" % DEV)
        return 0
    if "--selftest" in args:
        # Safe check: never opens the watchdog device.
        return selftest(do_probe="--no-probe" not in args)
    if args:
        print("unknown argument: %s" % args[0], file=sys.stderr)
        return 2

    os.makedirs(STATE_DIR, exist_ok=True)

    armed = record_armed_boot()
    log("start pid=%d armed_in_window=%d budget=%d/%ds"
        % (os.getpid(), armed, RESET_BUDGET, RESET_WINDOW_S))
    if DRY_RUN:
        log("DRY-RUN=1: a verdict writes evidence but never stops the feed")
    log("timing: grace=%ds probe=%ds x %d hard=%ds total=%ds feed=%ds"
        % (BOOT_GRACE, PROBE_SECS, FAIL_THRESHOLD, HARD_TIMEOUT,
           TOTAL_TOLERANCE, FEED_SECS))

    fd = open_watchdog()
    if fd is None:
        log("not armed; exiting (watchdog untouched, machine runs unprotected)")
        return 1

    log("armed %s, feed=%ds probe=%ds threshold=%d"
        % (DEV, FEED_SECS, PROBE_SECS, FAIL_THRESHOLD))

    def on_term(signum, _frame):
        log("signal %d, exiting; feed stops, hardware resets in <=%d s"
            % (signum, HARD_TIMEOUT))
        stop.set()

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    up = uptime_secs()
    log("uptime=%s, boot grace=%ds, verdict window=%ds"
        % ("unknown" if up is None else "%.1fs" % up, BOOT_GRACE,
           PROBE_SECS * FAIL_THRESHOLD))

    t = threading.Thread(target=prober, daemon=True)
    t.start()

    feeder(fd)          # returns only on stop / write error
    log("feeder stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
