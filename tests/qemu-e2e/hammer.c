/*
 * hammer.c - PID 1 for scripts/qemu-destroy-hammer.sh.
 *
 * It loads tbtun.ko on a kernel built with lockdep, UBSAN and
 * DEBUG_ATOMIC_SLEEP, then destroys its lifecycle from four directions at
 * once, all inside one guest on 127.0.0.1:
 *
 *   - four reader threads loop on /proc/net/tbtun/{stats,config,tunnels,peers}
 *     without a pause, so every teardown races a proc-file user;
 *   - two live FEC sessions (256-carrier SC-LDPC and 256-slot grouped) carry
 *     streams from two traffic threads;
 *   - twenty rounds of tunnel add -> carrier join -> stream -> del use two
 *     more tunnel ids while those sessions stay live;
 *   - ten full delete_module -> finit_module cycles each tear down a grouped
 *     pair while its streams are still open, then load the module again.
 *
 * A clean run prints QEMU_HAMMER_PASS after checking the kernel log for
 * UBSAN, BUG, Oops, WARNING, lockdep, sleep-in-atomic, list/refcount damage
 * and stall reports.  Any finding prints its exact log lines and fails.
 *
 * Build: cc -static -O2 -Wall -Wextra -Werror -pthread -o init hammer.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/io.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CFG_PATH "/proc/net/tbtun/config"
#define PORTS_PATH "/proc/net/tbtun/ports"
#define TUNNELS_PATH "/proc/net/tbtun/tunnels"
#define STATS_PATH "/proc/net/tbtun/stats"
#define MODULES_PATH "/proc/modules"

#define TGT_PORT 19070
#define SCLC_PUB_PORT 19000
#define SCLC_CAR_PORT 19001
#define GROUP_PUB_PORT 19400
#define GROUP_CAR_PORT 19401
#define HAMMER_PUB_PORT 19500
#define HAMMER_CAR_PORT 19501
#define RELOAD_PUB_PORT 19600
#define RELOAD_CAR_PORT 19601

#define SCLC_CARRIERS 256
#define ADD_ROUNDS 20
#define RELOAD_ROUNDS 10
#define SMALL_CARRIERS 8
#define STREAM_BYTES (16u * 1024u)
#define CHUNK 8192u
#define READ_BUF (1u << 20)
#define READER_THREADS 4
#define WATCHDOG_S 60
#define UP_TIMEOUT_MS 20000
#define SMALL_UP_TIMEOUT_MS 5000
#define GONE_TIMEOUT_MS 5000
#define UNLOAD_TIMEOUT_MS 6000
#define DEBUG_EXIT_PORT 0x501

/* The serial console can lose its tail when the guest powers off during a
 * printk storm.  QEMU's isa-debug-exit gives the verdict a channel that does
 * not depend on the console: QEMU exits with (code << 1) | 1.  When the
 * device is absent the write is a no-op and the poweroff still happens. */
static void debug_exit(unsigned char code)
{
    if (ioperm(DEBUG_EXIT_PORT, 1, 1) == 0)
        outb(code, DEBUG_EXIT_PORT);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static volatile long long g_beat;
static volatile int g_stop_readers;
static volatile unsigned long g_reader_iters[READER_THREADS];
static volatile unsigned long g_reader_errs[READER_THREADS];

static const char *const g_reader_paths[READER_THREADS] = {
    "/proc/net/tbtun/stats",
    "/proc/net/tbtun/config",
    "/proc/net/tbtun/tunnels",
    "/proc/net/tbtun/peers",
};

static void msleep(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void poweroff_now(void)
{
    sync();
    syscall(SYS_reboot, RB_POWER_OFF, RB_POWER_OFF, RB_POWER_OFF, NULL);
    syscall(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, NULL);
    for (;;)
        _exit(0);
}

static int read_path(const char *path, char *buf, size_t cap);
static int write_path(const char *path, const char *line);

/* State dump for a failing run.  The tunnel and stats views name the session
 * that was live; /proc/modules says whether the module was still there. */
static void dump_state(void)
{
    static char buf[4u * 1024u * 1024u];
    static const char *const files[] = {
        TUNNELS_PATH,
        STATS_PATH,
        MODULES_PATH,
    };
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        int len = read_path(files[i], buf, sizeof(buf));

        if (len < 0)
            continue;
        printf("QEMU_HAMMER_STATE %s:\n", files[i]);
        fwrite(buf, 1, (size_t)len, stdout);
        if (len && buf[len - 1] != '\n')
            printf("\n");
    }
    fflush(stdout);
}

static void verdict_fail(const char *fmt, ...)
{
    va_list ap;

    printf("QEMU_HAMMER_FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    dump_state();
    debug_exit(0x11);
    poweroff_now();
}

static void *watchdog_main(void *arg)
{
    (void)arg;

    for (;;)
    {
        msleep(500);
        if (now_ms() - g_beat > (long long)WATCHDOG_S * 1000)
        {
            printf("QEMU_HAMMER_FAIL watchdog after %ds\n", WATCHDOG_S);
            fflush(stdout);
            debug_exit(0x11);
            poweroff_now();
        }
    }
    return NULL;
}

static int read_path(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    size_t off = 0;

    if (fd < 0)
        return -1;
    while (off + 1 < cap)
    {
        n = read(fd, buf + off, cap - 1 - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (!n)
            break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = 0;
    return (int)off;
}

static int write_path(const char *path, const char *line)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    size_t len = strlen(line), off = 0;
    ssize_t n;

    if (fd < 0)
        return -1;
    while (off < len)
    {
        n = write(fd, line + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

/* Find key= in line and parse the decimal value. */
static int line_kv(const char *line, const char *key, unsigned long long *out)
{
    const char *p = strstr(line, key);
    char *end;
    unsigned long long v;

    if (!p)
        return -1;
    p += strlen(key);
    if (*p != '=')
        return -1;
    p++;
    if (*p < '0' || *p > '9')
        return -1;
    v = strtoull(p, &end, 10);
    if (end == p)
        return -1;
    *out = v;
    return 0;
}

/* Read the per-tunnel stats line whose tag is "c3". */
static int stats_c3_line(unsigned int id, char *out, size_t cap)
{
    static char buf[4u * 1024u * 1024u];
    char want[32];
    int len = read_path(STATS_PATH, buf, sizeof(buf));
    char *line, *save;
    size_t wl;

    if (len < 0)
        return -1;
    snprintf(want, sizeof(want), "tunnel %u c3 ", id);
    wl = strlen(want);
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, want, wl))
        {
            snprintf(out, cap, "%s", line);
            return 0;
        }
    return -1;
}

static int tunnel_up(unsigned int id, unsigned int *up)
{
    char line[2048];
    unsigned long long v;

    if (stats_c3_line(id, line, sizeof(line)))
        return -1;
    if (line_kv(line, " up", &v))
        return -1;
    *up = (unsigned int)v;
    return 0;
}

static int wait_up_pair(unsigned int a, unsigned int b, unsigned int want,
                        int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    unsigned int ua = 0, ub = 0;

    while (now_ms() < deadline)
    {
        if (!tunnel_up(a, &ua) && !tunnel_up(b, &ub) &&
            ua == want && ub == want)
            return 0;
        msleep(20);
    }
    printf("QEMU_HAMMER_STATE carriers %u=%u %u=%u want=%u\n", a, ua, b, ub,
           want);
    return -1;
}

static int wait_tunnels_gone(const unsigned int *ids, size_t n, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    static char buf[1 << 20];

    while (now_ms() < deadline)
    {
        int len = read_path(TUNNELS_PATH, buf, sizeof(buf));
        char *line, *save;
        size_t i;
        int found = 0;

        if (len < 0)
            return 0;
        for (line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save))
        {
            unsigned int id;

            if (sscanf(line, "%u ", &id) != 1)
                continue;
            for (i = 0; i < n; i++)
                if (id == ids[i])
                    found = 1;
        }
        if (!found)
            return 0;
        msleep(20);
    }
    return -1;
}

static int connect_client(int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int fl, rc;
    struct pollfd pfd;

    if (fd < 0)
        return -1;
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
    {
        close(fd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno == EINPROGRESS)
    {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = poll(&pfd, 1, 3000);
        if (rc > 0)
        {
            int err = 0;
            socklen_t el = sizeof(err);

            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err)
                rc = -1;
            else
                rc = 0;
        }
        else
            rc = -1;
    }
    else if (rc == 0)
        rc = 0;
    else
        rc = -1;
    if (rc)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_fd(int fd, short ev, int timeout_ms)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = ev;
    pfd.revents = 0;
    for (;;)
    {
        int r = poll(&pfd, 1, timeout_ms);

        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

static int send_all(int fd, const unsigned char *buf, size_t len, int timeout_ms)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);

        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (wait_fd(fd, POLLOUT, timeout_ms) <= 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int recv_all(int fd, unsigned char *buf, size_t len, int timeout_ms)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = recv(fd, buf + off, len - off, 0);

        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (!n)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            if (wait_fd(fd, POLLIN, timeout_ms) <= 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static unsigned char pattern_byte(uint32_t i)
{
    uint32_t v = i * 2654435761u + 0x9e3779b9u;

    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    return (unsigned char)(v >> 24);
}

static int send_bytes(int fd, size_t bytes, char *why, size_t cap,
                      int timeout_ms)
{
    unsigned char tx[CHUNK];
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            tx[i] = pattern_byte((uint32_t)(off + i));
        if (send_all(fd, tx, n, timeout_ms))
        {
            snprintf(why, cap, "send stalled at %zu/%zu: %s", off, bytes,
                     strerror(errno));
            return -1;
        }
        off += n;
    }
    return 0;
}

static int recv_verify(int fd, size_t bytes, char *why, size_t cap,
                       int timeout_ms)
{
    unsigned char rx[CHUNK], tx[CHUNK];
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            tx[i] = pattern_byte((uint32_t)(off + i));
        if (recv_all(fd, rx, n, timeout_ms))
        {
            snprintf(why, cap, "echo stalled at %zu/%zu", off, bytes);
            return -1;
        }
        for (i = 0; i < n; i++)
            if (rx[i] != tx[i])
            {
                snprintf(why, cap, "byte %zu differs", off + i);
                return -1;
            }
        off += n;
    }
    return 0;
}

/* Send a whole stream, half-close, then read the echo back and compare it.
 * The half-close is part of the contract: the receiver delivers a short
 * stream only when FIN names the end. */
static int roundtrip(int fd, size_t bytes, char *why, size_t cap,
                     int snd_ms, int rcv_ms)
{
    if (send_bytes(fd, bytes, why, cap, snd_ms))
        return -1;
    shutdown(fd, SHUT_WR);
    return recv_verify(fd, bytes, why, cap, rcv_ms);
}

static int echo_conn(int c)
{
    unsigned char buf[16384];

    for (;;)
    {
        ssize_t n = read(c, buf, sizeof(buf));
        size_t off = 0;

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (!n)
            break;
        while (off < (size_t)n)
        {
            ssize_t w = write(c, buf + off, (size_t)n - off);

            if (w < 0)
            {
                if (errno == EINTR)
                    continue;
                goto out;
            }
            off += (size_t)w;
        }
    }
out:
    shutdown(c, SHUT_WR);
    close(c);
    return 0;
}

static pid_t start_echo(void)
{
    struct sockaddr_in sa;
    int lfd, one = 1;
    pid_t pid;

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0)
        verdict_fail("echo: socket: %s", strerror(errno));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TGT_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        verdict_fail("echo: bind %d: %s", TGT_PORT, strerror(errno));
    if (listen(lfd, 128) < 0)
        verdict_fail("echo: listen: %s", strerror(errno));

    pid = fork();
    if (pid < 0)
        verdict_fail("echo: fork: %s", strerror(errno));
    if (!pid)
    {
        signal(SIGCHLD, SIG_IGN);
        for (;;)
        {
            int c = accept(lfd, NULL, NULL);
            pid_t child;

            if (c < 0)
            {
                if (errno == EINTR)
                    continue;
                _exit(1);
            }
            child = fork();
            if (child < 0)
            {
                close(c);
                continue;
            }
            if (!child)
            {
                close(lfd);
                _exit(echo_conn(c));
            }
            close(c);
        }
    }
    close(lfd);
    return pid;
}

static void raise_loopback(void)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        verdict_fail("lo: socket: %s", strerror(errno));
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
        verdict_fail("lo: SIOCGIFFLAGS: %s", strerror(errno));
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
        verdict_fail("lo: SIOCSIFFLAGS: %s", strerror(errno));
    close(fd);
}

static void load_module(void)
{
    int fd = open("/tbtun.ko", O_RDONLY | O_CLOEXEC);
    long rc;

    if (fd < 0)
        verdict_fail("open /tbtun.ko: %s", strerror(errno));
    rc = syscall(SYS_finit_module, fd, "fec_groups_per_flow=16", 0);
    close(fd);
    if (rc < 0)
        verdict_fail("finit_module: %s", strerror(errno));
}

static unsigned long long pid_state(pid_t pid)
{
    char path[64], buf[1024];
    char *p;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (read_path(path, buf, sizeof(buf)) < 0)
        return (unsigned long long)-1;
    p = strrchr(buf, ')');
    if (!p || p[1] != ' ')
        return (unsigned long long)-1;
    return (unsigned long long)p[2]; /* 'D' is 0x44 */
}

static void unload_module(int round)
{
    pid_t pid;
    long long deadline;
    int status = 0;

    pid = fork();
    if (pid < 0)
        verdict_fail("rmmod: fork: %s", strerror(errno));
    if (!pid)
    {
        long rc = syscall(SYS_delete_module, "tbtun", 0);

        if (rc < 0)
            fprintf(stderr, "rmmod: delete_module: %s\n", strerror(errno));
        _exit(rc < 0 ? 1 : 0);
    }
    deadline = now_ms() + UNLOAD_TIMEOUT_MS;
    while (now_ms() < deadline)
    {
        pid_t r = waitpid(pid, &status, WNOHANG);

        if (r == pid)
            break;
        msleep(10);
    }
    if (now_ms() >= deadline)
    {
        unsigned long long st = pid_state(pid);

        printf("QEMU_HAMMER_STATE rmmod round=%d pid=%d state=%c\n", round,
               (int)pid, st == (unsigned long long)-1 ? '?' : (char)st);
        fflush(stdout);
        kill(pid, SIGKILL);
        verdict_fail("delete_module round %d did not return in %d ms", round,
                     UNLOAD_TIMEOUT_MS);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        verdict_fail("delete_module round %d failed (status=%d)", round,
                     status);
    {
        static char mods[4096];
        int len = read_path(MODULES_PATH, mods, sizeof(mods));

        if (len > 0 && strstr(mods, "tbtun "))
            verdict_fail("round %d: tbtun still listed after delete_module",
                         round);
    }
}

static void cfg_write(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line))
        verdict_fail("config line too long");
    if (write_path(CFG_PATH, line))
        verdict_fail("config write: %s", strerror(errno));
}

static void write_ports(void)
{
    if (write_path(PORTS_PATH, "ports 40000-41023\n"))
        verdict_fail("ports write: %s", strerror(errno));
}

static void delete_tunnel(unsigned int id)
{
    cfg_write("del %u\n", id);
}

/* Two long-lived sessions: 256-carrier SC-LDPC and 256-slot grouped. */
static void cfg_live_sessions(void)
{
    cfg_write("tunnel 1 role=far profile=raw listen=127.0.0.1:%d"
              " target=127.0.0.1:%d n=128 m=128 fec=on mode=sc-ldpc"
              " adaptive=off w=256 carriers=%d crypto=off dup=1\n",
              SCLC_CAR_PORT, TGT_PORT, SCLC_CARRIERS);
    cfg_write("tunnel 2 role=near profile=raw listen=127.0.0.1:%d"
              " peer=127.0.0.1:%d n=128 m=128 fec=on mode=sc-ldpc"
              " adaptive=off w=256 carriers=%d crypto=off dup=1"
              " portblock=30000-30511\n",
              SCLC_PUB_PORT, SCLC_CAR_PORT, SCLC_CARRIERS);
    cfg_write("tunnel 3 role=far profile=raw listen=127.0.0.1:%d"
              " target=127.0.0.1:%d n=4 m=4 fec=on mode=grouped w=8"
              " carriers=%d crypto=off dup=1\n",
              GROUP_CAR_PORT, TGT_PORT, SCLC_CARRIERS);
    cfg_write("tunnel 4 role=near profile=raw listen=127.0.0.1:%d"
              " peer=127.0.0.1:%d n=4 m=4 fec=on mode=grouped w=8"
              " carriers=%d crypto=off dup=1 portblock=30600-31111\n",
              GROUP_PUB_PORT, GROUP_CAR_PORT, SCLC_CARRIERS);
}

/* Twenty-round pair on ids 5/6, while ids 1..4 stay live. */
static void cfg_add_pair(unsigned int far, unsigned int near, int pub_port,
                         int car_port, const char *block)
{
    cfg_write("tunnel %u role=far profile=raw listen=127.0.0.1:%d"
              " target=127.0.0.1:%d n=4 m=4 fec=on mode=grouped w=8"
              " carriers=%d crypto=off dup=1\n",
              far, car_port, TGT_PORT, SMALL_CARRIERS);
    cfg_write("tunnel %u role=near profile=raw listen=127.0.0.1:%d"
              " peer=127.0.0.1:%d n=4 m=4 fec=on mode=grouped w=8"
              " carriers=%d crypto=off dup=1 portblock=%s\n",
              near, pub_port, car_port, SMALL_CARRIERS, block);
}

static void *reader_main(void *arg)
{
    long idx = (long)(intptr_t)arg;
    char *buf = malloc(READ_BUF);

    if (!buf)
        return NULL;
    while (!g_stop_readers)
    {
        int fd = open(g_reader_paths[idx], O_RDONLY | O_CLOEXEC);
        size_t off = 0;

        if (fd < 0)
        {
            g_reader_errs[idx]++;
            continue;
        }
        for (;;)
        {
            ssize_t n = read(fd, buf + off, READ_BUF - 1 - off);

            if (n > 0)
            {
                off += (size_t)n;
                if (off >= READ_BUF - 1)
                    break;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        close(fd);
        g_reader_iters[idx]++;
    }
    free(buf);
    return NULL;
}

struct traffic
{
    int port;
    volatile int *stop;
    unsigned long rounds;
    unsigned long fails;
};

static void *traffic_main(void *arg)
{
    struct traffic *t = arg;
    char why[128];

    while (!*t->stop)
    {
        int fd = connect_client(t->port);

        if (fd < 0)
        {
            t->fails++;
            msleep(2);
            continue;
        }
        if (roundtrip(fd, STREAM_BYTES, why, sizeof(why), 1500, 2500))
            t->fails++;
        else
            t->rounds++;
        close(fd);
    }
    return NULL;
}

static int wait_traffic(struct traffic *t, unsigned long want, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline)
    {
        if (t->rounds >= want)
            return 0;
        msleep(10);
    }
    return -1;
}

static void klog_clear(void)
{
    klogctl(5, NULL, 0);
}

/* The guest owns the strict verdict.  The runner repeats the same patterns on
 * the serial log, so a lost ring line cannot hide a report. */
static void klog_check(void)
{
    static const char *const bad[] = {
        "UBSAN",
        "BUG:",
        "kernel BUG at",
        "Oops",
        "WARNING:",
        "Call Trace",
        "general protection fault",
        "invalid opcode",
        "possible circular locking",
        "possible recursive locking",
        "inconsistent lock state",
        "held lock",
        "sleeping function called from invalid context",
        "bad: scheduling from the idle thread",
        "list_add corruption",
        "list_del corruption",
        "refcount_t:",
        "KASAN",
        "soft lockup",
        "RCU stall",
        "rcu_sched",
        "blocked for more than",
        "workqueue lockup",
        "INFO: NMI",
    };
    int size = klogctl(10, NULL, 0);
    char *buf;
    char *line, *save;
    int i, hits = 0, loads = 0, unloads = 0;

    if (size <= 0)
    {
        printf("QEMU_HAMMER_NOTE klog size=%d\n", size);
        return;
    }
    buf = malloc((size_t)size + 1);
    if (!buf)
        verdict_fail("klog: out of memory");
    size = klogctl(3, buf, size);
    if (size < 0)
        verdict_fail("klog read: %s", strerror(errno));
    buf[size] = 0;
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        if (strstr(line, "tbtun: unloaded"))
            unloads++;
        else if (strstr(line, "tbtun ") && strstr(line, " loaded:"))
            loads++;
        for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
            if (strstr(line, bad[i]))
            {
                printf("QEMU_HAMMER_KLOG %s\n", line);
                hits++;
                break;
            }
    }
    free(buf);
    printf("QEMU_HAMMER_MODLOG loads=%d unloads=%d\n", loads, unloads);
    if (hits)
        verdict_fail("kernel log has %d bad line(s)", hits);
}

int main(void)
{
    long long t0;
    pthread_t readers[READER_THREADS], watchdog;
    struct traffic live[2];
    volatile int live_stop = 0;
    pthread_t live_th[2];
    static struct traffic cycles[RELOAD_ROUNDS][2];
    static volatile int cycle_stop[RELOAD_ROUNDS];
    static const unsigned int live_ids[] = { 1, 2, 3, 4 };
    pid_t echo_pid;
    int i, r;

    setvbuf(stdout, NULL, _IOLBF, 0);
    t0 = now_ms();
    g_beat = t0;
    printf("QEMU_HAMMER_START\n");
    fflush(stdout);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        verdict_fail("mount proc: %s", strerror(errno));
    raise_loopback();
    echo_pid = start_echo();
    klog_clear();
    load_module();
    printf("QEMU_HAMMER_MODULE loaded\n");
    fflush(stdout);
    write_ports();

    for (i = 0; i < READER_THREADS; i++)
        if (pthread_create(&readers[i], NULL, reader_main,
                           (void *)(intptr_t)i))
            verdict_fail("reader %d: pthread_create failed", i);
    if (pthread_create(&watchdog, NULL, watchdog_main, NULL))
        verdict_fail("watchdog: pthread_create failed");
    pthread_detach(watchdog);

    /* --- stage A: two live FEC sessions under continuous streams -------- */
    cfg_live_sessions();
    if (wait_up_pair(1, 2, SCLC_CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("SC-LDPC carriers did not come up");
    if (wait_up_pair(3, 4, SCLC_CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("grouped carriers did not come up");
    printf("QEMU_HAMMER_UP sclc=%d grouped=%d\n", SCLC_CARRIERS,
           SCLC_CARRIERS);
    fflush(stdout);

    for (i = 0; i < 2; i++)
    {
        memset(&live[i], 0, sizeof(live[i]));
        live[i].port = i ? GROUP_PUB_PORT : SCLC_PUB_PORT;
        live[i].stop = &live_stop;
        if (pthread_create(&live_th[i], NULL, traffic_main, &live[i]))
            verdict_fail("traffic %d: pthread_create failed", i);
    }
    for (i = 0; i < 2; i++)
        if (wait_traffic(&live[i], 1, 8000))
            verdict_fail("live session %d carried no stream", i);
    printf("QEMU_HAMMER_TRAFFIC sclc=%lu grouped=%lu\n",
           live[0].rounds, live[1].rounds);
    fflush(stdout);

    /* --- stage B: twenty add -> join -> stream -> del rounds ------------ */
    for (r = 1; r <= ADD_ROUNDS; r++)
    {
        char why[128] = "";
        int fd;

        cfg_add_pair(5, 6, HAMMER_PUB_PORT, HAMMER_CAR_PORT, "31200-31263");
        if (wait_up_pair(5, 6, SMALL_CARRIERS, SMALL_UP_TIMEOUT_MS))
            verdict_fail("round %d: carriers did not join", r);
        fd = connect_client(HAMMER_PUB_PORT);
        if (fd < 0)
            verdict_fail("round %d: connect: %s", r, strerror(errno));
        if (roundtrip(fd, STREAM_BYTES, why, sizeof(why), 4000, 6000))
            verdict_fail("round %d: %s", r, why);
        close(fd);
        delete_tunnel(5);
        delete_tunnel(6);
        {
            static const unsigned int ids[] = { 5, 6 };

            if (wait_tunnels_gone(ids, 2, GONE_TIMEOUT_MS))
                verdict_fail("round %d: tunnels 5/6 did not go away", r);
        }
        printf("QEMU_HAMMER_ROUND %d ok\n", r);
        fflush(stdout);
        g_beat = now_ms();
    }
    printf("QEMU_HAMMER_ROUNDS %d ok\n", ADD_ROUNDS);
    fflush(stdout);

    /* --- stage C: ten delete_module -> finit_module cycles --------------
     * Each cycle joins a grouped pair, starts two streams, deletes both
     * tunnels while the streams are open, and unloads at once.  The reader
     * threads keep opening the proc files across every teardown. */
    live_stop = 1;
    for (i = 0; i < 2; i++)
        pthread_join(live_th[i], NULL);
    delete_tunnel(1);
    delete_tunnel(2);
    delete_tunnel(3);
    delete_tunnel(4);
    if (wait_tunnels_gone(live_ids, 4, GONE_TIMEOUT_MS))
        verdict_fail("live sessions did not go away");
    printf("QEMU_HAMMER_TEARDOWN ok\n");
    fflush(stdout);

    for (r = 1; r <= RELOAD_ROUNDS; r++)
    {
        cycle_stop[r - 1] = 0;
        write_ports();
        cfg_add_pair(1, 2, RELOAD_PUB_PORT, RELOAD_CAR_PORT,
                     "31300-31363");
        if (wait_up_pair(1, 2, SMALL_CARRIERS, SMALL_UP_TIMEOUT_MS))
            verdict_fail("cycle %d: carriers did not join", r);
        for (i = 0; i < 2; i++)
        {
            cycles[r - 1][i].port = RELOAD_PUB_PORT;
            cycles[r - 1][i].stop = &cycle_stop[r - 1];
            if (pthread_create(&live_th[i], NULL, traffic_main,
                               &cycles[r - 1][i]))
                verdict_fail("cycle %d: stream thread failed", r);
            pthread_detach(live_th[i]);
        }
        msleep(120);
        delete_tunnel(1);
        delete_tunnel(2);
        msleep(25);
        unload_module(r);
        load_module();
        cycle_stop[r - 1] = 1;
        printf("QEMU_HAMMER_CYCLE %d unload+load ok\n", r);
        fflush(stdout);
        g_beat = now_ms();
    }
    printf("QEMU_HAMMER_RELOAD %d ok\n", RELOAD_ROUNDS);
    fflush(stdout);

    g_stop_readers = 1;
    for (i = 0; i < READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    printf("QEMU_HAMMER_READS stats=%lu config=%lu tunnels=%lu peers=%lu"
           " err=%lu\n",
           g_reader_iters[0], g_reader_iters[1], g_reader_iters[2],
           g_reader_iters[3],
           g_reader_errs[0] + g_reader_errs[1] + g_reader_errs[2] +
               g_reader_errs[3]);
    fflush(stdout);

    klog_check();
    printf("QEMU_HAMMER_KLOG clean\n");
    fflush(stdout);

    kill(echo_pid, SIGKILL);
    waitpid(echo_pid, NULL, 0);
    /* The serial console interleaves kernel printk inside a userspace write,
     * so the last carrier messages can split the verdict line.  Let them
     * drain, then repeat the verdict: the runner needs one intact copy. */
    msleep(200);
    for (i = 0; i < 3; i++)
    {
        printf("QEMU_HAMMER_PASS %llds rounds=%d reloads=%d\n",
               (now_ms() - t0) / 1000, ADD_ROUNDS, RELOAD_ROUNDS);
        fflush(stdout);
        msleep(50);
    }
    debug_exit(0x10);
    poweroff_now();
    return 0;
}
