/*
 * guest.c - PID 1 for scripts/qemu-e2e-smoke.sh.
 *
 * Boots in a QEMU guest with one file, /tbtun.ko, beside it.  It mounts proc,
 * raises loopback, loads the module with fec_groups_per_flow=16, and drives
 * the real data plane through /proc/net/tbtun/{ports,config,stats,streams}:
 *
 *   1. a 256-carrier SC-LDPC near+far pair on 127.0.0.1, then three
 *      sequential streams.  Each stream is echo-verified and must be followed
 *      by streams_active=0, so the next stream proves the flow was released.
 *   2. a second SC-LDPC stream opened while the first is live.  It must not
 *      be served; the first stream must keep working afterwards.
 *   3. tear the pair down, rebuild it as mode=grouped, and run two streams
 *      concurrently with a distinct sid each, both echo-verified.
 *   4. del both tunnels and delete_module.  The unload must return within the
 *      deadline (no D-state hang), and the kernel log must hold no UBSAN,
 *      BUG, Oops, WARNING, lockdep or stall report.
 *
 * The console carries one verdict line, printed last:
 *   QEMU_E2E_PASS <elapsed>s
 * or QEMU_E2E_FAIL <reason>
 *
 * Build: cc -static -O2 -Wall -Wextra -Werror -o init guest.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#define PUB_PORT 19000
#define CAR_PORT 19001
#define TGT_PORT 19070
#define CARRIERS 256
#define PORTBLOCK "30000-30511"
#define STREAM_BYTES (128u * 1024u)
#define CHUNK 32768u
#define WATCHDOG_S 40
#define UP_TIMEOUT_MS 15000
#define IDLE_TIMEOUT_MS 6000
#define UNLOAD_TIMEOUT_MS 8000

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_path(const char *path, char *buf, size_t cap);

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

/* State dump for a failing run.  Only the operator surface, and only the
 * small files: peers is 256 rows and would bury the reason. */
static void dump_state(void)
{
    static char buf[4u * 1024u * 1024u];
    static const char *const files[] = {
        "/proc/net/tbtun/tunnels",
        "/proc/net/tbtun/streams",
        "/proc/net/tbtun/stats",
    };
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        int len = read_path(files[i], buf, sizeof(buf));

        if (len < 0)
            continue;
        printf("QEMU_E2E_STATE %s:\n", files[i]);
        fwrite(buf, 1, (size_t)len, stdout);
        if (len && buf[len - 1] != '\n')
            printf("\n");
    }
    fflush(stdout);
}

static void verdict_fail(const char *fmt, ...)
{
    va_list ap;

    printf("QEMU_E2E_FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    dump_state();
    poweroff_now();
}

static void watchdog(int sig)
{
    (void)sig;
    printf("QEMU_E2E_FAIL watchdog after %ds\n", WATCHDOG_S);
    fflush(stdout);
    poweroff_now();
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

/* Read one /proc/net/tbtun/stats line for tunnel `id` matching `tag`
 * ("c3" or "streams").  Returns 0 with the line copied. */
static int stats_line(unsigned int id, const char *tag, char *out, size_t cap)
{
    static char buf[4u * 1024u * 1024u];
    char want[32];
    int len = read_path("/proc/net/tbtun/stats", buf, sizeof(buf));
    char *line, *save;
    size_t wl;

    if (len < 0)
        return -1;
    snprintf(want, sizeof(want), "tunnel %u %s ", id, tag);
    wl = strlen(want);
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        if (!strncmp(line, want, wl))
        {
            snprintf(out, cap, "%s", line);
            return 0;
        }
    }
    return -1;
}

static int tunnel_up(unsigned int id, unsigned int *up)
{
    char line[2048];
    unsigned long long v;

    if (stats_line(id, "c3", line, sizeof(line)))
        return -1;
    if (line_kv(line, " up", &v))
        return -1;
    *up = (unsigned int)v;
    return 0;
}

struct stream_state
{
    unsigned int active;
    unsigned int n_streams;
    unsigned int pending;
};

static int tunnel_streams(unsigned int id, struct stream_state *st)
{
    char line[512];
    unsigned long long v;

    if (stats_line(id, "streams", line, sizeof(line)))
        return 1; /* no live tunnel: nothing to drain */
    if (line_kv(line, "streams_active", &v))
        return -1;
    st->active = (unsigned int)v;
    if (line_kv(line, "n_streams", &v))
        return -1;
    st->n_streams = (unsigned int)v;
    if (line_kv(line, "n_pending", &v))
        return -1;
    st->pending = (unsigned int)v;
    return 0;
}

static int wait_carriers(unsigned int want, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    unsigned int a = 0, b = 0;

    while (now_ms() < deadline)
    {
        if (!tunnel_up(1, &a) && !tunnel_up(2, &b) && a == want && b == want)
            return 0;
        msleep(100);
    }
    printf("QEMU_E2E_STATE carriers far=%u near=%u want=%u\n", a, b, want);
    return -1;
}

static int wait_idle(int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    struct stream_state a = { 0, 0, 0 }, b = { 0, 0, 0 };

    while (now_ms() < deadline)
    {
        int ra = tunnel_streams(1, &a);
        int rb = tunnel_streams(2, &b);

        if (ra < 0 || rb < 0)
        {
            msleep(100);
            continue;
        }
        if ((ra || (!a.active && !a.pending)) &&
            (rb || (!b.active && !b.pending)))
            return 0;
        msleep(100);
    }
    printf("QEMU_E2E_STATE streams far active=%u n=%u pend=%u "
           "near active=%u n=%u pend=%u\n",
           a.active, a.n_streams, a.pending, b.active, b.n_streams, b.pending);
    return -1;
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
    const char *args = "fec_groups_per_flow=16";
    int fd = open("/tbtun.ko", O_RDONLY | O_CLOEXEC);
    long rc;

    if (fd < 0)
        verdict_fail("open /tbtun.ko: %s", strerror(errno));
    rc = syscall(SYS_finit_module, fd, args, 0);
    close(fd);
    if (rc < 0)
        verdict_fail("finit_module: %s", strerror(errno));
}

static int connect_client(void)
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
    sa.sin_port = htons(PUB_PORT);
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

static unsigned char chunk_send[CHUNK];
static unsigned char chunk_recv[CHUNK];

static unsigned char pattern_byte(uint32_t i)
{
    uint32_t v = i * 2654435761u + 0x9e3779b9u;

    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    return (unsigned char)(v >> 24);
}

/* Send one stream's bytes.  The scatter is by CHUNK because the near socket
 * buffer is smaller than a stream; the FEC capture ring holds the rest. */
static int send_bytes(int fd, size_t bytes, char *why, size_t cap)
{
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            chunk_send[i] = pattern_byte((uint32_t)(off + i));
        if (send_all(fd, chunk_send, n, 4000))
        {
            snprintf(why, cap, "send stalled at %zu/%zu: %s", off, bytes,
                     strerror(errno));
            return -1;
        }
        off += n;
    }
    return 0;
}

static int recv_verify(int fd, size_t bytes, char *why, size_t cap)
{
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            chunk_send[i] = pattern_byte((uint32_t)(off + i));
        if (recv_all(fd, chunk_recv, n, 10000))
        {
            snprintf(why, cap, "echo stalled at %zu/%zu: %s", off, bytes,
                     strerror(errno));
            return -1;
        }
        for (i = 0; i < n; i++)
            if (chunk_recv[i] != chunk_send[i])
            {
                snprintf(why, cap, "byte %zu differs: got %02x want %02x",
                         off + i, chunk_recv[i], chunk_send[i]);
                return -1;
            }
        off += n;
    }
    return 0;
}

/* Send a whole stream, half-close, then read the echo back and compare it.
 * The half-close is load-bearing: an SC-LDPC block is 128 data shares (1 MiB)
 * and the receiver only delivers a block at its cut or at the stream end, so
 * a short request is delivered when FIN names the end -- waiting for the
 * echo before the half-close would wait for data the sender never sends. */
static int roundtrip(int fd, size_t bytes, char *why, size_t cap)
{
    if (send_bytes(fd, bytes, why, cap))
        return -1;
    shutdown(fd, SHUT_WR);
    return recv_verify(fd, bytes, why, cap);
}

static void close_stream(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* One complete sequential stream: connect, echo-verify, close. */
static void stream_once(const char *tag, size_t bytes)
{
    char why[256] = "";
    int fd = connect_client();

    if (fd < 0)
        verdict_fail("%s: connect: %s", tag, strerror(errno));
    if (roundtrip(fd, bytes, why, sizeof(why)))
        verdict_fail("%s: %s", tag, why);
    close_stream(fd);
    printf("QEMU_E2E_STREAM %s bytes=%zu ok\n", tag, bytes);
    fflush(stdout);
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
        alarm(0);
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

static void configure(const char *role_line_far, const char *role_line_near)
{
    if (write_path("/proc/net/tbtun/ports", "ports 40000-41023\n"))
        verdict_fail("ports write: %s", strerror(errno));
    if (write_path("/proc/net/tbtun/config", role_line_far))
        verdict_fail("far config: %s", strerror(errno));
    if (write_path("/proc/net/tbtun/config", role_line_near))
        verdict_fail("near config: %s", strerror(errno));
}

static int wait_tunnels_gone(int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    static char buf[1 << 20];

    while (now_ms() < deadline)
    {
        int len = read_path("/proc/net/tbtun/tunnels", buf, sizeof(buf));
        char *line, *save;
        int one = 0, two = 0;

        if (len < 0)
            return 0;
        for (line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save))
        {
            unsigned int id;

            if (sscanf(line, "%u ", &id) == 1)
            {
                if (id == 1)
                    one = 1;
                else if (id == 2)
                    two = 1;
            }
        }
        if (!one && !two)
            return 0;
        msleep(100);
    }
    return -1;
}

static void delete_tunnel(unsigned int id)
{
    char line[32];

    snprintf(line, sizeof(line), "del %u\n", id);
    write_path("/proc/net/tbtun/config", line);
}

/* The per-stream snapshot lives in /proc/net/tbtun/stats, one
 * "tunnel N stream sid=..." line per live/tombstoned stream.  Two ends times
 * two concurrent streams is four. */
static int count_stream_rows(void)
{
    static char buf[4u * 1024u * 1024u];
    int len = read_path("/proc/net/tbtun/stats", buf, sizeof(buf));
    int n = 0;
    char *line, *save;

    if (len < 0)
        return -1;
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
        if (strstr(line, "stream sid="))
            n++;
    return n;
}

static int wait_stream_rows(int want, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    int n = -1;

    while (now_ms() < deadline)
    {
        n = count_stream_rows();
        if (n >= want)
            return n;
        msleep(100);
    }
    return n;
}

static void klog_clear(void)
{
    klogctl(5, NULL, 0);
}

static void klog_check(void)
{
    static const char *const bad[] = {
        "UBSAN", "BUG:", "kernel BUG at", "Oops", "WARNING:", "Call Trace",
        "general protection fault", "possible circular locking",
        "inconsistent lock state", "soft lockup", "rcu_sched", "RCU stall",
        "KASAN", "refcount_t:", "list_add corruption", "DEBUG_LOCKS",
    };
    int size = klogctl(10, NULL, 0);
    char *buf;
    char *line, *save;
    int i, hits = 0;

    if (size <= 0)
    {
        printf("QEMU_E2E_NOTE klog size=%d\n", size);
        return;
    }
    buf = malloc((size_t)size + 1);
    if (!buf)
        verdict_fail("klog: out of memory");
    size = klogctl(3, buf, size);
    if (size < 0)
    {
        printf("QEMU_E2E_NOTE klog read failed: %s\n", strerror(errno));
        free(buf);
        return;
    }
    buf[size] = 0;
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
        for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
            if (strstr(line, bad[i]))
            {
                printf("QEMU_E2E_KLOG %s\n", line);
                hits++;
                break;
            }
    free(buf);
    if (hits)
        verdict_fail("kernel log has %d bad line(s)", hits);
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

static void unload_module(void)
{
    pid_t pid;
    long long deadline;
    int status = 0;

    pid = fork();
    if (pid < 0)
        verdict_fail("rmmod: fork: %s", strerror(errno));
    if (!pid)
    {
        alarm(0);
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
        msleep(50);
    }
    if (now_ms() >= deadline)
    {
        unsigned long long st = pid_state(pid);

        printf("QEMU_E2E_STATE rmmod pid=%d state=%c\n", (int)pid,
               st == (unsigned long long)-1 ? '?' : (char)st);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        verdict_fail("delete_module did not return in %d ms", UNLOAD_TIMEOUT_MS);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        verdict_fail("delete_module failed (status=%d)", status);
    {
        static char mods[4096];
        int len = read_path("/proc/modules", mods, sizeof(mods));

        if (len > 0 && strstr(mods, "tbtun "))
            verdict_fail("tbtun still listed in /proc/modules");
    }
    printf("QEMU_E2E_UNLOAD ok\n");
    fflush(stdout);
}

int main(void)
{
    long long t0 = now_ms();
    pid_t echo_pid;
    int fd, i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("QEMU_E2E_START\n");
    signal(SIGALRM, watchdog);
    alarm(WATCHDOG_S);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        verdict_fail("mount proc: %s", strerror(errno));
    raise_loopback();
    klog_clear();
    load_module();
    printf("QEMU_E2E_MODULE loaded fec_groups_per_flow=16\n");

    echo_pid = start_echo();

    /* --- phase 1: 256-carrier SC-LDPC, three sequential streams ---------- */
    configure("tunnel 1 role=far profile=raw listen=127.0.0.1:19001"
              " target=127.0.0.1:19070 n=128 m=128 fec=on mode=sc-ldpc"
              " adaptive=off w=256 carriers=256 crypto=off dup=1\n",
              "tunnel 2 role=near profile=raw listen=127.0.0.1:19000"
              " peer=127.0.0.1:19001 n=128 m=128 fec=on mode=sc-ldpc"
              " adaptive=off w=256 carriers=256 crypto=off dup=1"
              " portblock=" PORTBLOCK "\n");
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("SC-LDPC carriers did not come up");
    printf("QEMU_E2E_UP sclc up=%u\n", CARRIERS);
    fflush(stdout);

    for (i = 1; i <= 3; i++)
    {
        char tag[32];

        snprintf(tag, sizeof(tag), "sclc-seq-%d", i);
        stream_once(tag, STREAM_BYTES);
        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("%s: session did not drain after close", tag);
        printf("QEMU_E2E_DRAIN %s ok\n", tag);
        fflush(stdout);
    }

    /* --- phase 2: a concurrent second SC-LDPC stream is refused --------- */
    {
        unsigned char junk[CHUNK];
        char tag2[32] = "sclc-second";
        char why[256] = "";
        size_t j;
        int first;

        /* Keep this stream live: it is the one the second must not displace
         * or wedge.  The transfer proves it is serving before the refusal. */
        first = connect_client();
        if (first < 0)
            verdict_fail("first stream: connect: %s", strerror(errno));
        if (send_bytes(first, STREAM_BYTES / 4, why, sizeof(why)))
            verdict_fail("first stream: %s", why);
        fd = connect_client();
        if (fd < 0)
            verdict_fail("second stream: connect: %s", strerror(errno));
        for (j = 0; j < sizeof(junk); j++)
            junk[j] = pattern_byte((uint32_t)j ^ 0x5a5a5a5au);
        if (send_all(fd, junk, sizeof(junk), 2000))
            printf("QEMU_E2E_NOTE second stream send: %s\n", strerror(errno));
        if (wait_fd(fd, POLLIN, 2500) > 0)
        {
            ssize_t n = recv(fd, chunk_recv, sizeof(chunk_recv), 0);

            if (n > 0)
                verdict_fail("second SC-LDPC stream was served (%zd bytes)",
                             n);
            printf("QEMU_E2E_REJECT %s closed=1\n", tag2);
        }
        else
            printf("QEMU_E2E_REJECT %s queued=1\n", tag2);
        fflush(stdout);
        close_stream(fd);

        /* The first stream must still work: the refusal came from the
         * admission boundary, not from a wedged session. */
        shutdown(first, SHUT_WR);
        if (recv_verify(first, STREAM_BYTES / 4, why, sizeof(why)))
            verdict_fail("first stream after refusal: %s", why);
        printf("QEMU_E2E_STREAM sclc-after-reject ok\n");
        fflush(stdout);
        close_stream(first);
        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("SC-LDPC session did not drain after the refusal");
    }

    /* --- phase 3: RLNC (grouped) multi-stream --------------------------- */
    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels 1/2 did not go away after del");
    printf("QEMU_E2E_DEL sclc ok\n");
    fflush(stdout);

    configure("tunnel 1 role=far profile=raw listen=127.0.0.1:19001"
              " target=127.0.0.1:19070 n=4 m=4 fec=on mode=grouped"
              " w=8 carriers=256 crypto=off dup=1\n",
              "tunnel 2 role=near profile=raw listen=127.0.0.1:19000"
              " peer=127.0.0.1:19001 n=4 m=4 fec=on mode=grouped"
              " w=8 carriers=256 crypto=off dup=1"
              " portblock=30600-31111\n");
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("grouped carriers did not come up");
    printf("QEMU_E2E_UP grouped up=%u\n", CARRIERS);
    fflush(stdout);

    {
        pid_t kids[2];
        int active_rows = -1;

        for (i = 0; i < 2; i++)
        {
            kids[i] = fork();
            if (kids[i] < 0)
                verdict_fail("multistream: fork: %s", strerror(errno));
            if (!kids[i])
            {
                int cfd = connect_client();
                char cwhy[256] = "";

                alarm(0);
                if (cfd < 0)
                {
                    printf("QEMU_E2E_NOTE stream%d connect: %s\n", i,
                           strerror(errno));
                    _exit(1);
                }
                if (roundtrip(cfd, STREAM_BYTES, cwhy, sizeof(cwhy)))
                {
                    printf("QEMU_E2E_NOTE stream%d: %s\n", i, cwhy);
                    _exit(1);
                }
                close_stream(cfd);
                _exit(0);
            }
            msleep(200);
            if (!i)
            {
                active_rows = count_stream_rows();
                printf("QEMU_E2E_STATE multistream rows=%d\n", active_rows);
                fflush(stdout);
            }
            else
            {
                active_rows = wait_stream_rows(2, 2000);
                printf("QEMU_E2E_STATE multistream rows=%d\n", active_rows);
                fflush(stdout);
            }
        }
        for (i = 0; i < 2; i++)
        {
            int status = 0;

            if (waitpid(kids[i], &status, 0) != kids[i] ||
                !WIFEXITED(status) || WEXITSTATUS(status))
                verdict_fail("RLNC concurrent stream %d failed", i);
        }
        if (active_rows < 0)
            printf("QEMU_E2E_NOTE stats file unavailable\n");
        else if (active_rows < 2)
            printf("QEMU_E2E_NOTE saw %d stream rows, expected 2\n",
                   active_rows);
        printf("QEMU_E2E_MULTISTREAM 2 streams ok\n");
        fflush(stdout);
        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("grouped session did not drain after close");
    }

    /* --- phase 4: clean teardown ---------------------------------------- */
    kill(echo_pid, SIGKILL);
    waitpid(echo_pid, NULL, 0);
    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels did not go away after del");
    if (wait_idle(IDLE_TIMEOUT_MS))
        verdict_fail("streams still active before unload");
    printf("QEMU_E2E_DEL grouped ok\n");
    fflush(stdout);

    unload_module();
    klog_check();

    printf("QEMU_E2E_PASS %llds\n", (now_ms() - t0) / 1000);
    fflush(stdout);
    poweroff_now();
    return 0;
}
