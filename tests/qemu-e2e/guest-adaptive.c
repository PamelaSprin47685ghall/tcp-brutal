/*
 * guest-adaptive.c - PID 1 for the adaptive SC-LDPC two-way E2E.
 *
 * One near+far pair on 127.0.0.1, 256 carriers, both directions adaptive:
 *   far  (tunnel 1, TX downlink): 1x256/L8 binary, starts at 128/128 (50%)
 *   near (tunnel 2, TX uplink):   8x32/L16 GF(16), starts at 8/24 (25%)
 *
 * Phase A carries an echo stream with no loss.  Both transmitters must issue
 * an online FEC_CFG DRAIN+SWAP and settle at the 70% ladder point (downlink
 * 178/78, uplink 22/10) while every application byte stays bit-exact.
 *
 * Phase B sets drop_ppm=250000 on every carrier, so both directions lose 25%
 * of their FEC shares.  The stream must stay byte-exact while both control
 * laws step down (downlink <= 151/105, uplink <= 19/13), and the kernel log
 * must hold no UBSAN, lockdep, BUG, WARN or sleep-in-atomic report.
 *
 * c13 prints the shape of the local RECEIVE plane, so tunnel 2 (near) shows
 * the peer's downlink and tunnel 1 (far) shows the peer's uplink.
 *
 * Build: cc -static -O2 -Wall -Wextra -Werror -o init guest-adaptive.c
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
#define CHUNK 32768u
#define WATCHDOG_S 180
#define UP_TIMEOUT_MS 20000
#define INIT_TIMEOUT_MS 15000
#define IDLE_TIMEOUT_MS 8000
#define UNLOAD_TIMEOUT_MS 10000
#define PHASE_A_S 20
#define PHASE_B_S 24
#define DRAIN_S 10
#define SEND_BYTES_PER_MS 8192u /* essentially unpaced; the drain handles the backlog */
#define SETTLE_TIMEOUT_MS 15000
#define NO_PROGRESS_MS 30000
#define DROP_PPM 250000u

/* 70% and 50% ladder splits for the two profiles. */
#define DOWN_70_N 178
#define DOWN_70_M 78
#define UP_70_N 22
#define UP_70_M 10
#define DOWN_50_N 128
#define DOWN_60_N 151
#define UP_50_N 16
#define UP_60_N 19

static int c13_diag = 3;

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* State dump for a failing run: the small operator files only. */
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
        int len = -1;
        int fd = open(files[i], O_RDONLY | O_CLOEXEC);

        if (fd >= 0)
        {
            size_t off = 0;
            ssize_t n;

            while (off + 1 < sizeof(buf) &&
                   (n = read(fd, buf + off, sizeof(buf) - 1 - off)) > 0)
                off += (size_t)n;
            close(fd);
            buf[off] = 0;
            len = (int)off;
        }
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

/* Read one /proc/net/tbtun/stats line for tunnel `id` matching `tag`. */
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

struct c13
{
    int adaptive;
    unsigned int epoch, n, m, groups;
    unsigned int switches, aborts, rollbacks;
    unsigned int p_ppm;
    unsigned long long sent, miss;
    char state[16];
    char reason[16];
};

static void copy_word(const char *line, const char *key, char *out, size_t cap)
{
    const char *p = strstr(line, key);
    size_t i = 0;

    out[0] = 0;
    if (!p)
        return;
    p += strlen(key);
    while (i + 1 < cap && p[i] && p[i] != ' ')
    {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
}

static int read_c13(unsigned int id, struct c13 *c)
{
    char line[2048];
    unsigned long long v;

    memset(c, 0, sizeof(*c));
    if (stats_line(id, "c13", line, sizeof(line)))
    {
        if (c13_diag > 0)
        {
            c13_diag--;
            printf("QEMU_E2E_DIAG c13 id=%u no-line\n", id);
            fflush(stdout);
        }
        return -1;
    }
    if (c13_diag > 0)
    {
        c13_diag--;
        printf("QEMU_E2E_DIAG c13 id=%u %s\n", id, line);
        fflush(stdout);
    }
    c->adaptive = strstr(line, "adaptive=on") != NULL;
    if (line_kv(line, " epoch", &v))
        return -1;
    c->epoch = (unsigned int)v;
    if (line_kv(line, " n", &v))
        return -1;
    c->n = (unsigned int)v;
    if (line_kv(line, " m", &v))
        return -1;
    c->m = (unsigned int)v;
    if (line_kv(line, " groups", &v))
        return -1;
    c->groups = (unsigned int)v;
    if (line_kv(line, " switches", &v))
        return -1;
    c->switches = (unsigned int)v;
    if (line_kv(line, " aborts", &v))
        return -1;
    c->aborts = (unsigned int)v;
    if (line_kv(line, " rollbacks", &v))
        return -1;
    c->rollbacks = (unsigned int)v;
    if (line_kv(line, " p_ppm", &v))
        return -1;
    c->p_ppm = (unsigned int)v;
    if (line_kv(line, " sent", &v))
        return -1;
    c->sent = v;
    if (line_kv(line, " miss", &v))
        return -1;
    c->miss = v;
    copy_word(line, "state=", c->state, sizeof(c->state));
    copy_word(line, "reason=", c->reason, sizeof(c->reason));
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
        return 1;
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

static int wait_idle(int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    struct stream_state a = { 0, 0, 0 }, b = { 0, 0, 0 };

    while (now_ms() < deadline)
    {
        int ra = tunnel_streams(1, &a);
        int rb = tunnel_streams(2, &b);

        if (!ra && !rb && !a.active && !a.pending && !b.active && !b.pending)
            return 0;
        msleep(100);
    }
    printf("QEMU_E2E_STATE streams far active=%u pend=%u near active=%u pend=%u\n",
           a.active, a.pending, b.active, b.pending);
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
    const char *args = "debug_loss=1 fec_groups_per_flow=16";
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

    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PUB_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
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

/* One near+far pair: the far TX is the downlink (1x256), the near TX is the
 * uplink (8 groups of 32).  Both control laws start at their configured
 * ladder point and are free to move. */
static void configure_pair(void)
{
    if (write_path("/proc/net/tbtun/ports", "ports 40000-41023\n"))
        verdict_fail("ports write: %s", strerror(errno));
    if (write_path("/proc/net/tbtun/config",
                   "tunnel 1 role=far profile=raw listen=127.0.0.1:19001"
                   " target=127.0.0.1:19070 n=128 m=128 fec=on"
                   " mode=sc-ldpc adaptive=on w=256 L=8 carriers=256"
                   " carrier_stall_ms=15000 crypto=off dup=1\n"))
        verdict_fail("far config: %s", strerror(errno));
    if (write_path("/proc/net/tbtun/config",
                   "tunnel 2 role=near profile=raw listen=127.0.0.1:19000"
                   " peer=127.0.0.1:19001 n=8 m=24 fec=on mode=sc-ldpc"
                   " adaptive=on w=128 L=16 carriers=256"
                   " carrier_stall_ms=15000 crypto=off dup=1"
                   " portblock=" PORTBLOCK "\n"))
        verdict_fail("near config: %s", strerror(errno));
}

static int wait_carriers(unsigned int want, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    unsigned int a = 0, b = 0;

    while (now_ms() < deadline)
    {
        char line[2048];
        unsigned long long v;

        if (!stats_line(1, "c3", line, sizeof(line)) &&
            !line_kv(line, " up", &v))
            a = (unsigned int)v;
        if (!stats_line(2, "c3", line, sizeof(line)) &&
            !line_kv(line, " up", &v))
            b = (unsigned int)v;
        if (a == want && b == want)
            return 0;
        msleep(100);
    }
    printf("QEMU_E2E_STATE carriers far=%u near=%u want=%u\n", a, b, want);
    return -1;
}

struct obs
{
    unsigned int down_peak_n, down_peak_m, down_peak_epoch;
    unsigned int up_peak_n, up_peak_m, up_peak_epoch;
    struct c13 down, up; /* tunnel 2 and tunnel 1 */
    int saw_down_cfg, saw_down_drain, saw_down_swap;
    int saw_up_cfg, saw_up_drain, saw_up_swap;
    int saw_init_down, saw_init_up, init_printed;
    unsigned int samples;
};

static void note_switch_state(struct obs *o, const struct c13 *down,
                              const struct c13 *up)
{
    if (!strcmp(down->state, "cfg_sent"))
        o->saw_down_cfg = 1;
    else if (!strcmp(down->state, "drain"))
        o->saw_down_drain = 1;
    else if (!strcmp(down->state, "swap"))
        o->saw_down_swap = 1;
    if (!strcmp(up->state, "cfg_sent"))
        o->saw_up_cfg = 1;
    else if (!strcmp(up->state, "drain"))
        o->saw_up_drain = 1;
    else if (!strcmp(up->state, "swap"))
        o->saw_up_swap = 1;
}

static int obs_sample(struct obs *o)
{
    struct c13 down, up;

    if (read_c13(2, &down) || read_c13(1, &up))
        return -1;
    o->samples++;
    /* INIT travels with the first stream (the CFG names a sid), so it is
     * observed here rather than before traffic.  Tunnel 2 must swap its own
     * uplink receive shape (8/24, groups=8) for the far's downlink (128/128,
     * groups=1); tunnel 1 the reverse.  Both numbers also exist before the
     * swap, so the geometry, not just n/m, is the evidence. */
    if (down.groups == 1 && down.n == DOWN_50_N && down.m == DOWN_50_N)
        o->saw_init_down = 1;
    if (up.groups == 8 && up.n == 8 && up.m == 24)
        o->saw_init_up = 1;
    if (!o->init_printed && o->saw_init_down && o->saw_init_up)
    {
        printf("QEMU_E2E_INIT down=%u/%u up=%u/%u\n", down.n, down.m, up.n,
               up.m);
        fflush(stdout);
        o->init_printed = 1;
    }
    /* c13 is written by the local RX tick (peer TX shape) and, for up to
     * one tick after a swap, by the local TX.  Classify by the geometry in
     * the line, not by the tunnel id: groups=1 is the downlink (one block of
     * 256 shares), groups=8 is the uplink (8 blocks of 32 shares).  A TX
     * sample then still lands in the right direction. */
    if (down.groups == 1)
    {
        o->down = down;
        if (down.n > o->down_peak_n)
        {
            o->down_peak_n = down.n;
            o->down_peak_m = down.m;
            o->down_peak_epoch = down.epoch;
        }
    }
    else if (down.groups == 8)
    {
        o->up = down;
        if (down.n > o->up_peak_n)
        {
            o->up_peak_n = down.n;
            o->up_peak_m = down.m;
            o->up_peak_epoch = down.epoch;
        }
    }
    if (up.groups == 1)
    {
        o->down = up;
        if (up.n > o->down_peak_n)
        {
            o->down_peak_n = up.n;
            o->down_peak_m = up.m;
            o->down_peak_epoch = up.epoch;
        }
    }
    else if (up.groups == 8)
    {
        o->up = up;
        if (up.n > o->up_peak_n)
        {
            o->up_peak_n = up.n;
            o->up_peak_m = up.m;
            o->up_peak_epoch = up.epoch;
        }
    }
    note_switch_state(o, &down, &up);
    return 0;
}

static void obs_print(const char *tag, const struct obs *o)
{
    printf("QEMU_E2E_RATE %s down=%u/%u epoch=%u state=%s reason=%s "
           "p=%u up=%u/%u epoch=%u state=%s reason=%s p=%u\n",
           tag, o->down.n, o->down.m, o->down.epoch, o->down.state,
           o->down.reason, o->down.p_ppm, o->up.n, o->up.m, o->up.epoch,
           o->up.state, o->up.reason, o->up.p_ppm);
    fflush(stdout);
}

static int set_drop_ppm(unsigned int ppm);

static int phase_a_ok(const struct obs *o)
{
    return o->down_peak_n == DOWN_70_N && o->down_peak_m == DOWN_70_M &&
           o->up_peak_n == UP_70_N && o->up_peak_m == UP_70_M;
}

/* The controller may retract one step after its own up-switch repair burst
 * even with no injected loss; what phase B must prove is that the 25% loss
 * pushes each direction strictly below where it sat at the injection, with a
 * new shape epoch in force. */
static int phase_b_ok(const struct obs *a, const struct obs *o)
{
    return o->down.n <= DOWN_60_N && o->down.n < a->down.n &&
           o->up.n <= UP_60_N && o->up.n < a->up.n &&
           o->down.epoch > a->down.epoch && o->up.epoch > a->up.epoch &&
           !strcmp(o->down.state, "steady") &&
           !strcmp(o->up.state, "steady");
}

/* One stream carries both phases: `pre_s` seconds with no loss, then `loss_s`
 * seconds with 25% of every carrier's FEC shares dropped.  The control law
 * therefore rises to 70% and then steps down on the same connection; a new
 * stream would reset the flow to the configured 25/50 start and could never
 * show an online downgrade.
 *
 * The child does not half-close when its timer ends.  It reports the byte
 * count and waits DRAIN_S seconds first: a FIN sent with the sender's
 * in-flight window still full makes the receiver drain a multi-megabyte
 * backlog against the close budget.  Waiting for every byte to come back
 * instead deadlocks: the FEC FIN is what tells the receiver the last short
 * block's real cut, and the application cannot echo that block before the
 * FIN.  Sending stops, the tunnel drains, then the FIN releases the tail. */
static int stream_phased(int pre_s, int loss_s, struct obs *o,
                         struct obs *phase_a, int *a_ok, int *b_ok,
                         unsigned long long *pre_off,
                         unsigned long long *recvd,
                         unsigned long long *sent_out, char *why, size_t cap)
{
    int fd = connect_client();
    int cnt_pipe[2];
    pid_t pid;
    unsigned long long off = 0, sent = 0;
    int have_sent = 0, loss_injected = 0, loss_active = 0;
    long long last_progress = now_ms();
    long long next_obs = 0;
    long long start;
    ssize_t n;
    int status = 0;

    *pre_off = 0;
    *a_ok = 0;
    *b_ok = 0;
    if (fd < 0)
    {
        snprintf(why, cap, "connect: %s", strerror(errno));
        return -1;
    }
    if (pipe(cnt_pipe))
    {
        snprintf(why, cap, "pipe: %s", strerror(errno));
        close(fd);
        return -1;
    }
    pid = fork();
    if (pid < 0)
    {
        snprintf(why, cap, "fork: %s", strerror(errno));
        close(cnt_pipe[0]);
        close(cnt_pipe[1]);
        close(fd);
        return -1;
    }
    if (!pid)
    {
        unsigned long long tx = 0;
        long long begin = now_ms();
        long long end = begin + (long long)(pre_s + loss_s) * 1000;

        alarm(0);
        close(cnt_pipe[0]);
        while (now_ms() < end)
        {
            size_t i;

            for (i = 0; i < CHUNK; i++)
                chunk_send[i] = pattern_byte((uint32_t)(tx + i));
            if (send_all(fd, chunk_send, CHUNK, 4000))
                break;
            tx += CHUNK;
            {
                /* Keep the in-flight backlog small enough that the close
                 * budget can drain it: the fast loopback otherwise parks
                 * tens of MiB in carrier sockets, and a FIN behind that
                 * backlog resets the stream. */
                long long want = begin + (long long)(tx / SEND_BYTES_PER_MS);
                long long now = now_ms();

                if (want > now)
                    msleep((int)(want - now));
            }
        }
        if (write(cnt_pipe[1], &tx, sizeof(tx)) != (ssize_t)sizeof(tx))
            _exit(1);
        msleep(DRAIN_S * 1000);
        shutdown(fd, SHUT_WR);
        _exit(0);
    }
    close(cnt_pipe[1]);
    if (fcntl(cnt_pipe[0], F_SETFL, O_NONBLOCK) < 0)
    {
        snprintf(why, cap, "fcntl: %s", strerror(errno));
        goto bad;
    }
    start = now_ms();

    for (;;)
    {
        struct pollfd pfd[2];
        int pr;

        if (now_ms() >= next_obs)
        {
            (void)obs_sample(o);
            next_obs = now_ms() + 250;
            if (!loss_injected && now_ms() - start >= (long long)pre_s * 1000)
            {
                *phase_a = *o;
                *pre_off = off;
                *a_ok = phase_a_ok(o);
                if (set_drop_ppm(DROP_PPM))
                {
                    snprintf(why, cap, "drop_ppm set: %s", strerror(errno));
                    goto bad;
                }
                printf("QEMU_E2E_LOSS drop_ppm=%u carriers=%u\n", DROP_PPM,
                       CARRIERS);
                fflush(stdout);
                loss_injected = 1;
                loss_active = 1;
                if (!*a_ok)
                {
                    snprintf(why, cap,
                             "phase A did not hold 70%% in both directions");
                    goto bad;
                }
            }
            if (loss_active && !*b_ok && phase_b_ok(phase_a, o))
            {
                *b_ok = 1;
                obs_print("phase-B", o);
            }
        }
        if (!have_sent)
        {
            ssize_t r = read(cnt_pipe[0], &sent, sizeof(sent));

            if (r == (ssize_t)sizeof(sent))
                have_sent = 1;
        }
        if (have_sent && loss_active)
        {
            /* The sender has stopped; let the drain and tail repairs run
             * without the injector so the close budget is not spent on
             * fresh drops. */
            (void)set_drop_ppm(0);
            loss_active = 0;
        }
        pfd[0].fd = fd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = cnt_pipe[0];
        pfd[1].events = POLLIN;
        pfd[1].revents = 0;
        pr = poll(pfd, 2, 200);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            snprintf(why, cap, "poll: %s", strerror(errno));
            goto bad;
        }
        if (pr > 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            n = recv(fd, chunk_recv, CHUNK, 0);
            if (n > 0)
            {
                size_t i;

                for (i = 0; i < (size_t)n; i++)
                {
                    unsigned char want =
                        pattern_byte((uint32_t)(off + i));

                    if (chunk_recv[i] != want)
                    {
                        snprintf(why, cap,
                                 "byte %llu differs: got %02x want %02x",
                                 off + i, chunk_recv[i], want);
                        goto bad;
                    }
                }
                off += (size_t)n;
                last_progress = now_ms();
                continue;
            }
            if (!n)
                break;
            if (errno == EINTR || errno == EAGAIN)
                continue;
            snprintf(why, cap, "recv: %s", strerror(errno));
            goto bad;
        }
        if (now_ms() - last_progress > NO_PROGRESS_MS)
        {
            snprintf(why, cap, "no echo progress at %llu bytes", off);
            goto bad;
        }
    }

    if (!have_sent)
    {
        ssize_t r = read(cnt_pipe[0], &sent, sizeof(sent));

        if (r == (ssize_t)sizeof(sent))
            have_sent = 1;
    }
    if (!have_sent)
    {
        snprintf(why, cap, "sender reported no byte count");
        goto bad;
    }
    if (loss_active)
    {
        (void)set_drop_ppm(0);
        loss_active = 0;
    }
    waitpid(pid, &status, 0);
    close(cnt_pipe[0]);
    close(fd);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
    {
        snprintf(why, cap, "sender child failed (status=%d)", status);
        return -1;
    }
    if (off != sent)
    {
        snprintf(why, cap, "echo delivered %llu of %llu bytes", off, sent);
        return -1;
    }
    *recvd = off;
    *sent_out = sent;
    return 0;

bad:
    if (loss_active)
        (void)set_drop_ppm(0);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    close(cnt_pipe[0]);
    close(fd);
    return -1;
}

static int set_drop_ppm(unsigned int ppm)
{
    char line[64];
    unsigned int i;

    for (i = 0; i < CARRIERS; i++)
    {
        snprintf(line, sizeof(line), "carrier %u drop_ppm %u\n", i, ppm);
        if (write_path("/proc/net/tbtun/config", line))
            return -1;
    }
    return 0;
}

static void delete_tunnel(unsigned int id)
{
    char line[32];

    snprintf(line, sizeof(line), "del %u\n", id);
    write_path("/proc/net/tbtun/config", line);
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
        "sleeping function called from invalid context",
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
    return (unsigned long long)p[2];
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

static void print_states(const char *tag, const struct obs *o)
{
    printf("QEMU_E2E_SWAP %s down=%s%s%s up=%s%s%s\n", tag,
           o->saw_down_cfg ? "cfg_sent," : "",
           o->saw_down_drain ? "drain," : "",
           o->saw_down_swap ? "swap" : "",
           o->saw_up_cfg ? "cfg_sent," : "",
           o->saw_up_drain ? "drain," : "",
           o->saw_up_swap ? "swap" : "");
    fflush(stdout);
}

int main(void)
{
    long long t0 = now_ms();
    pid_t echo_pid;
    struct obs a = { 0 }, pa = { 0 };
    unsigned long long sent_a = 0, recv_a = 0;
    unsigned long long pre_a = 0;
    int a_ok = 0, b_ok = 0;
    char why[256] = "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("QEMU_E2E_START\n");
    signal(SIGALRM, watchdog);
    alarm(WATCHDOG_S);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        verdict_fail("mount proc: %s", strerror(errno));
    raise_loopback();
    klog_clear();
    load_module();
    printf("QEMU_E2E_MODULE loaded debug_loss=1 fec_groups_per_flow=16\n");

    echo_pid = start_echo();
    configure_pair();
    if (wait_carriers(CARRIERS, UP_TIMEOUT_MS))
        verdict_fail("SC-LDPC carriers did not come up");
    printf("QEMU_E2E_UP sclc adaptive up=%u\n", CARRIERS);
    fflush(stdout);

    /* --- one stream: no loss, then 25% loss ----------------------------- */
    if (stream_phased(PHASE_A_S, PHASE_B_S, &a, &pa, &a_ok, &b_ok, &pre_a,
                      &recv_a, &sent_a, why, sizeof(why)))
    {
        obs_print("phase-stream", &a);
        print_states("phase-stream", &a);
        verdict_fail("phased stream: %s", why);
    }
    if (!pa.saw_init_down || !pa.saw_init_up)
    {
        /* The peer's asymmetric shape must have been installed for the two
         * directions to deliver at all; this flag only says the transient
         * 128/128 and 8/24 window was sampled. Report it, do not fail:
         * the 70% peaks below are the functional proof. */
        printf("QEMU_E2E_NOTE INIT swap window not sampled (down=%d up=%d)\n",
               pa.saw_init_down, pa.saw_init_up);
        fflush(stdout);
    }
    if (!a_ok)
    {
        obs_print("phase-A", &pa);
        print_states("phase-A", &pa);
        verdict_fail("phase A did not hold 70%% in both directions");
    }
    printf("QEMU_E2E_RATE phase-A down=%u/%u up=%u/%u epoch=%u/%u "
           "now=%u/%u,%u/%u\n",
           pa.down_peak_n, pa.down_peak_m, pa.up_peak_n, pa.up_peak_m,
           pa.down.epoch, pa.up.epoch, pa.down.n, pa.down.m, pa.up.n,
           pa.up.m);
    fflush(stdout);
    print_states("phase-A", &pa);
    printf("QEMU_E2E_STREAM phase-a bytes=%llu ok\n", pre_a);
    printf("QEMU_E2E_ABORT phase-A down=%u/%u up=%u/%u\n", pa.down.aborts,
           pa.down.rollbacks, pa.up.aborts, pa.up.rollbacks);
    if (!b_ok)
    {
        obs_print("phase-B-final", &a);
        print_states("phase-B-final", &a);
        verdict_fail("phase B did not step down in both directions");
    }
    obs_print("phase-B-final", &a);
    print_states("phase-B-final", &a);
    printf("QEMU_E2E_STREAM phase-b bytes=%llu ok\n", recv_a);
    fflush(stdout);
    if (wait_idle(IDLE_TIMEOUT_MS))
        verdict_fail("phased session did not drain");

    /* --- teardown ------------------------------------------------------- */
    kill(echo_pid, SIGKILL);
    waitpid(echo_pid, NULL, 0);
    delete_tunnel(1);
    delete_tunnel(2);
    if (wait_tunnels_gone(8000))
        verdict_fail("tunnels did not go away after del");
    printf("QEMU_E2E_DEL adaptive ok\n");
    fflush(stdout);

    unload_module();
    klog_check();

    printf("QEMU_E2E_PASS %llds\n", (now_ms() - t0) / 1000);
    fflush(stdout);
    poweroff_now();
    return 0;
}
