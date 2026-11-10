/*
 * guest.c - PID 1 for scripts/qemu-e2e-smoke.sh.
 *
 * Boots in a QEMU guest with one file, /tbtun.ko, beside it.  It mounts proc,
 * raises loopback, loads the module (0.7.0 ignores fec_groups_per_flow), and drives
 * the real data plane through /proc/net/tbtun/{ports,config,stats,streams}.
 * Every stage runs in one boot, in this order:
 *
 *   1. grouped (non-SCLC) baseline: a 256-carrier n=4 m=4 grouped pair
 *      carries two sequential streams with distinct sids, both echo-verified.
 *      0.7.0 admits one FEC flow that owns all 32 groups per session.
 *   2. multi-port MAPPORT: slot 0 public :19001 routes to :15210 and slot 1
 *      public :19002 to :15211, backed by targets with distinct response
 *      signatures.  Each client must see its own target's signature, so a
 *      swapped slot cannot pass.  The two slots are exercised sequentially:
 *      0.7.0 admits one FEC flow per session, so a second concurrent stream
 *      is refused by design.
 *   3. SC-LDPC GF(16) BITPLANE64 uplink: an n=8 m=24, block=128 L=16 pair
 *      (8 groups of 32 shares) carries one byte-exact roundtrip through the
 *      production TBT_SCLC_MAP_BITPLANE64 encoder and decoder.
 *   4. SC-LDPC single-stream over profile=http-kal: the 1x256 downlink /
 *      8x32 uplink pair carries sequential streams. One bulk stream moves
 *      6 MiB per direction past the 4 MiB plane credit, one 50 KiB stream
 *      closes with a forced short tail, and one resets mid-stream; the RST
 *      must cross the plane on both ends.
 *   5. del both tunnels and delete_module.  The unload must return within the
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
#define TGT_PORT 19070
/* Multi-port phase: slot 0 public :19001 reaches :15210, slot 1 public
 * :19002 reaches :15211.  The carrier listener moves to :19011 because
 * :19001 is now a public port. */
#define MP_PUB0 19001
#define MP_PUB1 19002
#define MP_TGT0 15210
#define MP_TGT1 15211
#define MP_SIG1 0x5au
#define MP_BYTES (64u * 1024u)
#define CARRIERS 256
#define PORTBLOCK "30000-30511"
#define STREAM_BYTES (128u * 1024u)
#define CHUNK 32768u
/* Four bulk streams carry 6 MiB per direction, past the 4 MiB plane credit;
 * one 50 KiB stream exercises the sub-window FIN tail; one stream resets
 * mid-flight. */
#define BULK_STREAMS 4
#define BULK_BYTES (1536u * 1024u)
#define SHORT_BYTES (50u * 1024u)
#define ABORT_BYTES (64u * 1024u)
#define CONCURRENCY (BULK_STREAMS + 2)
#define ABORT_AFTER_MS 200
#define WATCHDOG_S 240
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

static int connect_client_port(uint16_t port)
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
    sa.sin_port = htons(port);
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

static int connect_client(void)
{
    return connect_client_port(PUB_PORT);
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

/* One gate per concurrent child. The parent opens them only after both ends
 * report all six legs active, so the data and the mid-stream reset provably
 * overlap instead of racing the admission. */
static int stream_gate[CONCURRENCY][2];

static void gate_wait(int idx)
{
    char go;

    while (read(stream_gate[idx][0], &go, 1) < 0 && errno == EINTR)
        ;
}

static unsigned char pattern_byte(uint32_t i, uint32_t seed)
{
    uint32_t v = i * 2654435761u + 0x9e3779b9u + seed * 0x85ebca6bu;

    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    return (unsigned char)(v >> 24);
}

/* Send one stream's bytes.  The scatter is by CHUNK because the near socket
 * buffer is smaller than a stream; the FEC capture ring holds the rest. */
static int send_bytes(int fd, size_t bytes, uint32_t seed, char *why,
                      size_t cap)
{
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            chunk_send[i] = pattern_byte((uint32_t)(off + i), seed);
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

/* `seed` selects the stream's pattern; `sig` is the target's response
 * signature: target 0 echoes the byte, target 1 xors it with MP_SIG1.  A
 * stream that lands on the wrong mapping slot therefore fails on the first
 * byte rather than passing as clean echo. */
static int recv_verify(int fd, size_t bytes, uint32_t seed, unsigned char sig,
                       char *why, size_t cap)
{
    size_t off = 0;

    while (off < bytes)
    {
        size_t n = bytes - off < CHUNK ? bytes - off : CHUNK;
        size_t i;

        for (i = 0; i < n; i++)
            chunk_send[i] = pattern_byte((uint32_t)(off + i), seed);
        if (recv_all(fd, chunk_recv, n, 10000))
        {
            snprintf(why, cap, "echo stalled at %zu/%zu: %s", off, bytes,
                     strerror(errno));
            return -1;
        }
        for (i = 0; i < n; i++)
            if (chunk_recv[i] != (unsigned char)(chunk_send[i] ^ sig))
            {
                snprintf(why, cap,
                         "byte %zu differs: got %02x want %02x (sig %02x)",
                         off + i, chunk_recv[i],
                         (unsigned char)(chunk_send[i] ^ sig), sig);
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
static int roundtrip_full(int fd, size_t bytes, uint32_t seed,
                          unsigned char sig, char *why, size_t cap)
{
    if (send_bytes(fd, bytes, seed, why, cap))
        return -1;
    shutdown(fd, SHUT_WR);
    return recv_verify(fd, bytes, seed, sig, why, cap);
}

static int roundtrip_sig(int fd, size_t bytes, unsigned char sig, char *why,
                         size_t cap)
{
    return roundtrip_full(fd, bytes, 0, sig, why, cap);
}

static int roundtrip(int fd, size_t bytes, uint32_t seed, char *why,
                     size_t cap)
{
    return roundtrip_full(fd, bytes, seed, 0, why, cap);
}

static void close_stream(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* One complete sequential stream: connect, echo-verify, close. */
static void stream_once(const char *tag, size_t bytes, uint32_t seed)
{
    char why[256] = "";
    int fd = connect_client();

    if (fd < 0)
        verdict_fail("%s: connect: %s", tag, strerror(errno));
    if (roundtrip(fd, bytes, seed, why, sizeof(why)))
        verdict_fail("%s: %s", tag, why);
    close_stream(fd);
    printf("QEMU_E2E_STREAM %s bytes=%zu ok\n", tag, bytes);
    fflush(stdout);
}

/* One target connection: reply with every byte transformed by `sig`.  sig=0
 * is a plain echo; any other value is that target's response signature. */
static int echo_conn(int c, unsigned char sig)
{
    unsigned char buf[16384];
    size_t i;

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
        for (i = 0; i < (size_t)n; i++)
            buf[i] = (unsigned char)(buf[i] ^ sig);
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

static pid_t start_echo(uint16_t port, unsigned char sig)
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
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        verdict_fail("echo: bind %u: %s", port, strerror(errno));
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
                _exit(echo_conn(c, sig));
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
 * "tunnel N stream sid=..." line per live/tombstoned stream. */
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

/* The canonical tunnel table must name the asymmetric profile we asked for:
 * far TX 1x256 (n=128 m=128), near TX 8x32 (n=8 m=24). */
static void check_shape(void)
{
    static char buf[1 << 16];
    char *line, *save;
    unsigned int far_n = 0, far_m = 0, near_n = 0, near_m = 0;
    int len = read_path("/proc/net/tbtun/tunnels", buf, sizeof(buf));

    if (len < 0)
        verdict_fail("tunnels readback: %s", strerror(errno));
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        unsigned long long id, streams;
        char role[16], state[16], listen[64], peer[64], target[64];
        unsigned int n, m;

        if (sscanf(line, "%llu %15s %15s %63s %63s %63s %u %u %llu",
                   &id, role, state, listen, peer, target, &n, &m,
                   &streams) != 9)
            continue;
        if (id == 1 && !strcmp(role, "far"))
        {
            far_n = n;
            far_m = m;
        }
        else if (id == 2 && !strcmp(role, "near"))
        {
            near_n = n;
            near_m = m;
        }
    }
    if (far_n != 128 || far_m != 128 || near_n != 8 || near_m != 24)
        verdict_fail("want far=128x128 near=8x24, read far=%ux%u near=%ux%u",
                     far_n, far_m, near_n, near_m);
    printf("QEMU_E2E_SHAPE far=1x256 near=8x32\n");
    fflush(stdout);
}

/* Both ends must show `want` legs attached to the session at once. */
static int wait_active_both(unsigned int want, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    struct stream_state a = { 0, 0, 0 }, b = { 0, 0, 0 };

    while (now_ms() < deadline)
    {
        int ra = tunnel_streams(1, &a);
        int rb = tunnel_streams(2, &b);

        if (!ra && !rb && a.active >= want && b.active >= want)
            return 0;
        msleep(50);
    }
    printf("QEMU_E2E_STATE concurrent far active=%u near active=%u want=%u\n",
           a.active, b.active, want);
    return -1;
}

/* Bytes the tunnel has handed to its streams, from the rx_ready_calls line. */
static int delivered_bytes(unsigned int id, unsigned long long *out)
{
    static char buf[4u * 1024u * 1024u];
    char want[48];
    char *line, *save;
    int len = read_path("/proc/net/tbtun/stats", buf, sizeof(buf));

    if (len < 0)
        return -1;
    snprintf(want, sizeof(want), "tunnel %u rx_ready_calls=", id);
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, want, strlen(want)))
            return line_kv(line, "rx_delivered", out);
    return -1;
}

static void concurrent_child(int idx, uint32_t seed, size_t bytes)
{
    char why[256] = "";
    int fd;

    alarm(0);
    fd = connect_client();
    if (fd < 0)
    {
        printf("QEMU_E2E_NOTE concurrent%d connect: %s\n", idx,
               strerror(errno));
        _exit(1);
    }
    gate_wait(idx);
    if (roundtrip(fd, bytes, seed, why, sizeof(why)))
    {
        printf("QEMU_E2E_NOTE concurrent%d: %s\n", idx, why);
        _exit(1);
    }
    close_stream(fd);
    _exit(0);
}

/* Wait for the parent's gate, send a prefix, then close with SO_LINGER(0,0):
 * the kernel emits RST instead of FIN, so the peer sees this stream die
 * while its siblings are still moving bytes. */
static void abort_child(int idx, uint32_t seed, size_t bytes)
{
    struct linger lg = { 1, 0 };
    char why[256] = "";
    int fd;

    alarm(0);
    fd = connect_client();
    if (fd < 0)
    {
        printf("QEMU_E2E_NOTE abort%d connect: %s\n", idx, strerror(errno));
        _exit(1);
    }
    gate_wait(idx);
    if (send_bytes(fd, bytes, seed, why, sizeof(why)))
    {
        printf("QEMU_E2E_NOTE abort%d send: %s\n", idx, why);
        _exit(1);
    }
    msleep(ABORT_AFTER_MS);
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)))
    {
        printf("QEMU_E2E_NOTE abort%d linger: %s\n", idx, strerror(errno));
        _exit(1);
    }
    close(fd);
    _exit(0);
}

int main(void)
{
    long long t0 = now_ms();
    pid_t echo_pid[3];
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("QEMU_E2E_START\n");
    signal(SIGALRM, watchdog);
    alarm(WATCHDOG_S);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        verdict_fail("mount proc: %s", strerror(errno));
    /* The console is the evidence channel. Drop its loglevel after boot so a
     * per-carrier pr_info cannot interleave with a marker; the ring buffer
     * still records every line klog_check reads, and errors/panics still
     * reach the console. */
    write_path("/proc/sys/kernel/printk", "4 4 1 7\n");
    raise_loopback();
    klog_clear();
    load_module();
    printf("QEMU_E2E_MODULE loaded fec_groups_per_flow=16\n");

    echo_pid[0] = start_echo(TGT_PORT, 0x00);
    echo_pid[1] = start_echo(MP_TGT0, 0x00);
    echo_pid[2] = start_echo(MP_TGT1, MP_SIG1);

    /* --- phase 1: grouped (non-SCLC) baseline --------------------------- */
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
        int active_rows = -1;

        for (i = 0; i < 2; i++)
        {
            pid_t kid = fork();
            int status = 0;

            if (kid < 0)
                verdict_fail("multistream: fork: %s", strerror(errno));
            if (!kid)
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
                if (roundtrip(cfd, STREAM_BYTES, 0x9000u + (uint32_t)i,
                              cwhy, sizeof(cwhy)))
                {
                    printf("QEMU_E2E_NOTE stream%d: %s\n", i, cwhy);
                    _exit(1);
                }
                close_stream(cfd);
                _exit(0);
            }
            msleep(200);
            active_rows = count_stream_rows();
            printf("QEMU_E2E_STATE multistream rows=%d\n", active_rows);
            fflush(stdout);
            if (waitpid(kid, &status, 0) != kid ||
                !WIFEXITED(status) || WEXITSTATUS(status))
                verdict_fail("RLNC stream %d failed", i);
            /* One FEC flow per session: the next stream may start only
             * after this one's flow is gone. */
            if (wait_idle(IDLE_TIMEOUT_MS))
                verdict_fail("grouped stream %d did not release its flow", i);
        }
        if (active_rows < 0)
            printf("QEMU_E2E_NOTE stats file unavailable\n");
        else if (active_rows < 1)
            printf("QEMU_E2E_NOTE saw %d stream rows, expected at least 1\n",
                   active_rows);
        printf("QEMU_E2E_MULTISTREAM 2 streams ok\n");
        fflush(stdout);
    }

    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels did not go away before the map phase");
    printf("QEMU_E2E_DEL grouped ok\n");
    fflush(stdout);

    /* --- phase 2: two mappings on one tunnel, distinct signatures -------- */
    /* Slot 0 keeps :19001 -> :15210 and the map= adds :19002 -> :15211.  The
     * near side sends each slot's target port in OPEN; the far side may only
     * dial a port its target= or map= names. */
    configure("tunnel 1 role=far profile=raw listen=127.0.0.1:19011"
              " target=127.0.0.1:15210 map=19002:15211"
              " n=4 m=4 fec=on mode=grouped w=8 carriers=256 crypto=off"
              " dup=1\n",
              "tunnel 2 role=near profile=raw listen=127.0.0.1:19001"
              " peer=127.0.0.1:19011 target=127.0.0.1:15210"
              " map=19002:15211 n=4 m=4 fec=on mode=grouped w=8"
              " carriers=256 crypto=off dup=1 portblock=31200-31711\n");
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("map carriers did not come up");
    printf("QEMU_E2E_UP map up=%u\n", CARRIERS);
    fflush(stdout);

    {
        static const uint16_t ports[2] = { MP_PUB0, MP_PUB1 };
        static const unsigned char sigs[2] = { 0x00, MP_SIG1 };

        for (i = 0; i < 2; i++)
        {
            pid_t kid = fork();
            int status = 0;

            if (kid < 0)
                verdict_fail("map client %d: fork: %s", i, strerror(errno));
            if (!kid)
            {
                char cwhy[256] = "";
                int cfd;

                alarm(0);
                cfd = connect_client_port(ports[i]);
                if (cfd < 0)
                {
                    printf("QEMU_E2E_NOTE map%d connect port %u: %s\n", i,
                           ports[i], strerror(errno));
                    _exit(1);
                }
                /* Half-close first: the SC-LDPC cut and the target's EOF both
                 * need FIN before the response can be verified. */
                if (roundtrip_sig(cfd, MP_BYTES, sigs[i], cwhy, sizeof(cwhy)))
                {
                    printf("QEMU_E2E_NOTE map%d port %u: %s\n", i, ports[i],
                           cwhy);
                    _exit(1);
                }
                close_stream(cfd);
                _exit(0);
            }
            if (waitpid(kid, &status, 0) != kid ||
                !WIFEXITED(status) || WEXITSTATUS(status))
                verdict_fail("map stream on port %u failed", ports[i]);
            /* One FEC flow per session: the next slot may connect only after
             * the previous stream's flow is gone. */
            if (wait_idle(IDLE_TIMEOUT_MS))
                verdict_fail("map stream on port %u did not release its flow",
                             ports[i]);
        }
        printf("QEMU_E2E_MAPPORT slot=0 port=%u target=%u sig=00 ok\n",
               MP_PUB0, MP_TGT0);
        printf("QEMU_E2E_MAPPORT slot=1 port=%u target=%u sig=%02x ok\n",
               MP_PUB1, MP_TGT1, MP_SIG1);
        fflush(stdout);
        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("map session did not drain after close");
    }

    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels did not go away before the uplink phase");
    printf("QEMU_E2E_DEL map ok\n");
    fflush(stdout);

    /* --- phase 3: SC-LDPC GF(16) uplink, bit-plane 64 mapping ----------- *
     * n=8 m=24 over 256 carriers is 8 groups of 32 shares, so the shape is
     * TBT_FEC_SCLC_UP and the data plane builds tbt_fec_ops_sclc_gf16 with
     * TBT_SCLC_MAP_BITPLANE64 in both directions.  The echo crosses the far
     * TX encoder and the near RX decoder, so a byte-exact roundtrip fails
     * if the shared-control peel or the plane gather/scatter is wrong. */
    configure("tunnel 1 role=far profile=raw listen=127.0.0.1:19001"
              " target=127.0.0.1:19070 n=8 m=24 fec=on mode=sc-ldpc"
              " adaptive=off w=128 L=16 carriers=256 crypto=off dup=1\n",
              "tunnel 2 role=near profile=raw listen=127.0.0.1:19000"
              " peer=127.0.0.1:19001 n=8 m=24 fec=on mode=sc-ldpc"
              " adaptive=off w=128 L=16 carriers=256 crypto=off dup=1"
              " portblock=" PORTBLOCK "\n");
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("GF(16) uplink carriers did not come up");
    printf("QEMU_E2E_UP sclc-uplink up=%u\n", CARRIERS);
    fflush(stdout);
    stream_once("sclc-uplink", STREAM_BYTES, 0x2000u);
    if (wait_idle(IDLE_TIMEOUT_MS))
        verdict_fail("GF(16) uplink session did not drain after close");
    printf("QEMU_E2E_DRAIN sclc-uplink ok\n");
    fflush(stdout);

    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels did not go away before the concurrent phase");
    printf("QEMU_E2E_DEL uplink ok\n");
    fflush(stdout);

    /* --- phase 4: SC-LDPC single-plane multistream over http-kal -------- */
    configure("tunnel 1 role=far profile=http-kal listen=127.0.0.1:19001"
              " target=127.0.0.1:19070 n=128 m=128 fec=on mode=sc-ldpc"
              " adaptive=off w=256 carriers=256 crypto=off dup=1\n",
              "tunnel 2 role=near profile=http-kal listen=127.0.0.1:19000"
              " peer=127.0.0.1:19001 n=8 m=24 fec=on mode=sc-ldpc"
              " adaptive=off w=128 carriers=256 crypto=off dup=1"
              " portblock=" PORTBLOCK "\n");
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("SC-LDPC carriers did not come up");
    printf("QEMU_E2E_UP sclc up=%u\n", CARRIERS);
    fflush(stdout);
    check_shape();

    for (i = 1; i <= 3; i++)
    {
        char tag[32];

        snprintf(tag, sizeof(tag), "sclc-seq-%d", i);
        stream_once(tag, STREAM_BYTES, 0x1000u + (uint32_t)i);
        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("%s: session did not drain after close", tag);
        printf("QEMU_E2E_DRAIN %s ok\n", tag);
        fflush(stdout);
    }

    /* --- phase 4 (burst): six streams, a 50 KiB tail and a reset -------- *
     * The bulk total is 4 x 1.5 MiB = 6 MiB per direction, past the 4 MiB
     * plane credit. The short stream is shorter than one window, so only the
     * forced short tail at FIN can close its sender gate. */
    {
        pid_t kids[CONCURRENCY];
        uint32_t seeds[CONCURRENCY];
        unsigned long long far0 = 0, near0 = 0, far1 = 0, near1 = 0;
        unsigned long long http_req = 0;
        int failed = 0;
        char line[2048];

        if (delivered_bytes(1, &far0) || delivered_bytes(2, &near0))
            verdict_fail("delivered counters unreadable before the burst");
        for (i = 0; i < CONCURRENCY; i++)
            if (pipe(stream_gate[i]))
                verdict_fail("concurrent: pipe: %s", strerror(errno));
        for (i = 0; i < CONCURRENCY; i++)
        {
            seeds[i] = 0x51ed0000u + (uint32_t)i * 0x9e3779b9u;
            kids[i] = fork();
            if (kids[i] < 0)
                verdict_fail("concurrent: fork: %s", strerror(errno));
            if (!kids[i])
            {
                if (!i)
                    abort_child(i, seeds[i], ABORT_BYTES);
                else if (i <= BULK_STREAMS)
                    concurrent_child(i, seeds[i], BULK_BYTES);
                else
                    concurrent_child(i, seeds[i], SHORT_BYTES);
            }
            msleep(20);
        }
        /* Every leg must stand on that one plane at the same moment.
         * The children have not sent a byte yet, so no leg can finish early
         * and shrink the window. */
        if (wait_active_both(CONCURRENCY, 15000))
            verdict_fail("expected %d concurrent streams on both ends",
                         CONCURRENCY);
        for (i = 0; i < CONCURRENCY; i++)
        {
            if (write(stream_gate[i][1], "x", 1) != 1)
                verdict_fail("concurrent: gate %d: %s", i,
                             strerror(errno));
            close(stream_gate[i][1]);
        }
        printf("QEMU_E2E_CONCURRENT %d active ok\n", CONCURRENCY);
        fflush(stdout);

        for (i = 0; i < CONCURRENCY; i++)
        {
            int status = 0;

            if (waitpid(kids[i], &status, 0) != kids[i] ||
                !WIFEXITED(status) || WEXITSTATUS(status))
                failed++;
        }
        if (failed)
            verdict_fail("%d of %d concurrent streams failed", failed,
                         CONCURRENCY);
        printf("QEMU_E2E_STREAMS %d bulk + 1 short + 1 reset ok\n",
               BULK_STREAMS);
        fflush(stdout);

        /* Kernel-side volume, not the guest's own byte count: each direction
         * must hand more than 5 MiB to its streams, which is only possible
         * if the plane's ACK credit keeps moving past 4 MiB. */
        if (delivered_bytes(1, &far1) || delivered_bytes(2, &near1))
            verdict_fail("delivered counters unreadable after the burst");
        if (far1 - far0 <= 5u * 1024u * 1024u ||
            near1 - near0 <= 5u * 1024u * 1024u)
            verdict_fail("burst below 5 MiB: far=%llu near=%llu",
                         far1 - far0, near1 - near0);
        printf("QEMU_E2E_VOLUME far=%llu near=%llu\n", far1 - far0,
               near1 - near0);
        fflush(stdout);
        printf("QEMU_E2E_SHORT bytes=%u ok\n", SHORT_BYTES);
        fflush(stdout);

        /* Per-stream control records must survive the HTTP container dispatch. */
        if (stats_line(2, "c8", line, sizeof(line)) ||
            line_kv(line, "http_req", &http_req))
            verdict_fail("near c8 unreadable");
        if (!http_req)
            verdict_fail("http-kal carried no request");
        printf("QEMU_E2E_HTTPKAL near_req=%llu\n", http_req);
        fflush(stdout);

        if (stats_line(1, "c11", line, sizeof(line)) ||
            line_kv(line, "fec_reset_user", &far1))
            verdict_fail("far reset counter unreadable");
        if (stats_line(2, "c11", line, sizeof(line)) ||
            line_kv(line, "fec_reset_user", &near1))
            verdict_fail("near reset counter unreadable");
        if (!far1 || !near1)
            verdict_fail("reset did not cross the plane: far=%llu "
                         "near=%llu", far1, near1);
        printf("QEMU_E2E_RST far=%llu near=%llu\n", far1, near1);
        fflush(stdout);

        if (wait_idle(IDLE_TIMEOUT_MS))
            verdict_fail("SC-LDPC plane did not drain after the concurrent"
                         " streams");
        printf("QEMU_E2E_DRAIN sclc-concurrent ok\n");
        fflush(stdout);
    }

    /* --- phase 5: clean teardown ---------------------------------------- */
    for (i = 0; i < 3; i++)
    {
        kill(echo_pid[i], SIGKILL);
        waitpid(echo_pid[i], NULL, 0);
    }
    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(6000))
        verdict_fail("tunnels did not go away after del");
    if (wait_idle(IDLE_TIMEOUT_MS))
        verdict_fail("streams still active before unload");
    printf("QEMU_E2E_DEL sclc-concurrent ok\n");
    fflush(stdout);

    unload_module();
    klog_check();

    printf("QEMU_E2E_PASS %llds\n", (now_ms() - t0) / 1000);
    fflush(stdout);
    poweroff_now();
    return 0;
}
