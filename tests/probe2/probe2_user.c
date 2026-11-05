/* probe2_user.c - the peer half of tests/probe2/tbt_probe2.ko.
 *
 * Two descriptors exist per connection and they are different objects:
 *   peer  our client half (what connect() returned). Bytes the module sends
 *         arrive here; bytes the module INJECTS never do.
 *   srv   the module's accepted socket, handed out by GETFD. Injected bytes
 *         are here, and this is what an application on the near side reads.
 * All read assertions are on `srv`; `peer` only sends.
 *
 * Phases, each on a fresh connection:
 *   A  inject into a live socket; read it back; inspect the queue skb
 *   B  peer sends 16 bytes first, then inject: order and counts
 *   C  1 byte every 10 ms while injecting: interleave under a moving rcv_nxt
 *   D  peer-side counters and our advertised window afterwards
 *   E  MSG_PEEK / SO_RCVLOWAT / SO_RCVTIMEO / MSG_WAITALL on injected data
 *   F  close with injected bytes unread: FIN or RST at the peer
 *
 * Final line PROBE2_PASS or PROBE2_FAIL; exit status mirrors it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "tbt_probe2_uapi.h"

#define PORT 19210
#define INJ 4096

static int failures;
static int ctl = -1;

struct conn
{
    int peer;
    int srv;
};

static struct conn cur = {-1, -1};

static void ok(const char *what, const char *detail)
{
    printf("  ok   %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static void okf(const char *what, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void okf(const char *what, const char *fmt, ...)
{
    char d[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(d, sizeof(d), fmt, ap);
    va_end(ap);
    ok(what, d);
}

static void bad(const char *what, const char *detail)
{
    printf("  FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
    failures++;
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...)
{
    va_list ap;

    printf("  NOTE ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void close_conn(void)
{
    if (cur.srv >= 0)
        close(cur.srv);
    if (cur.peer >= 0)
        close(cur.peer);
    cur.srv = cur.peer = -1;
}

static int open_conn(void)
{
    struct sockaddr_in sa;

    close_conn();
    cur.peer = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(cur.peer, (struct sockaddr *)&sa, sizeof(sa)))
    {
        perror("connect");
        return -1;
    }
    usleep(200 * 1000); /* let the accept work item run */
    cur.srv = ioctl(ctl, TBT_P2_GETFD);
    if (cur.srv < 0)
    {
        perror("GETFD");
        return -1;
    }
    return 0;
}

static int status(struct tbt_p2_status *st)
{
    memset(st, 0, sizeof(*st));
    return ioctl(ctl, TBT_P2_STATUS, st);
}

static int skbinfo(struct tbt_p2_skbinfo *si)
{
    memset(si, 0, sizeof(*si));
    return ioctl(ctl, TBT_P2_SKBINFO, si);
}

static int inject(size_t len, uint8_t byte)
{
    struct tbt_p2_inject req = {.len = (uint32_t)len, .byte = byte};

    return ioctl(ctl, TBT_P2_INJECT, &req);
}

static int read_upto(int fd, char *buf, size_t want, int timeout_ms)
{
    size_t got = 0;
    long long deadline = now_ms() + timeout_ms;

    while (got < want)
    {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int left = (int)(deadline - now_ms());
        ssize_t n;

        if (left <= 0 || poll(&p, 1, left) <= 0)
            break;
        n = read(fd, buf + got, want - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    return (int)got;
}

/* Digest of the live socket, printed on every failure: this is the evidence. */
static void dump_state(const char *tag)
{
    struct tbt_p2_status st;
    struct tbt_p2_skbinfo si;

    if (status(&st) == 0)
        note("%s status: state=%u done=%u shut=0x%x err=%d qlen=%u nxt=%u copied=%u "
             "wup=%u wnd=%u rmem=%u/%u repair=%u rq=%u urg=0x%x",
             tag, st.state, st.sock_done, st.shutdown, st.sk_err, st.qlen, st.rcv_nxt,
             st.copied_seq, st.rcv_wup, st.rcv_wnd, st.mem_alloc, st.rcvbuf, st.repair,
             st.repair_queue, st.urg_data);
    if (skbinfo(&si) == 0)
        note("%s head skb: seq=%u end=%u ack=%u len=%u data_len=%u flags=0x%x "
             "users=%u off_from_copied=%d",
             tag, si.seq, si.end_seq, si.ack_seq, si.len, si.data_len, si.tcp_flags,
             si.users, (int)si.offset_from_copied);
}

/* ---------------------------------------------------------------------- */

/* If the module armed repair, show the gate first, then clear it and show the
 * same read succeeding. This is the discriminator that identifies the cause. */
static void phase_a0(void)
{
    char buf[8192];
    int n;

    printf("-- A0: does tp->repair decide whether the app can read?\n");
    if (open_conn())
    {
        bad("A0 open connection", "see errno");
        return;
    }
    if (ioctl(ctl, TBT_P2_DISARM) == 0)
        note("A0 repair cleared before injecting (TBT_P2_DISARM)");
    inject(INJ, 0x40);

    n = read_upto(cur.srv, buf, INJ, 2000);
    if (n == INJ)
        okf("A0 read after arming is cleared returns the injected bytes",
            "%d bytes", n);
    else
        note("A0 read after clearing returned %d", n);
}

static void phase_a(void)
{
    struct tbt_p2_status st;
    struct tbt_p2_skbinfo si;
    char buf[8192];
    int n, rc;
    uint32_t before;

    printf("-- A: inject into a live socket and read it back\n");
    if (open_conn())
    {
        bad("A open connection", "see errno");
        return;
    }
    if (status(&st))
    {
        bad("A status", strerror(errno));
        close_conn();
        return;
    }
    before = st.rcv_nxt;
    rc = inject(INJ, 0x41);
    if (rc)
    {
        bad("A tcp_send_rcvq on a live accepted socket", strerror(-rc));
        close_conn();
        return;
    }
    ok("A tcp_send_rcvq accepted 4 KiB", NULL);
    if (status(&st) == 0 && st.rcv_nxt - before == INJ)
        ok("A rcv_nxt advanced by exactly 4096", NULL);
    else
        note("A rcv_nxt %u -> %u", before, st.rcv_nxt);

    dump_state("A");

    memset(buf, 0, sizeof(buf));
    n = read_upto(cur.srv, buf, INJ, 2000);
    if (n == INJ && buf[0] == 0x41 && buf[INJ - 1] == 0x41)
        ok("A the application socket reads the injected bytes", "4 KiB of 0x41");
    else
    {
        char d[128];
        snprintf(d, sizeof(d), "read returned %d (errno=%s, first byte 0x%02x)", n,
                 n < 0 ? strerror(errno) : "n/a", n > 0 ? (unsigned char)buf[0] : 0);
        bad("A the application socket reads the injected bytes", d);
        dump_state("A after failed read");
    }

    /* The connection must still carry a real segment the other way. */
    {
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        char b[64];
        int rc2, n2;

        setsockopt(cur.peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        rc2 = ioctl(ctl, TBT_P2_FORCE_SEND);
        memset(b, 0, sizeof(b));
        n2 = read(cur.peer, b, sizeof(b));
        if (rc2 >= 0 && n2 > 0)
            ok("A a real module->peer segment still reaches the client", NULL);
        else
            note("A server send returned %d, peer read %d errno=%s", rc2, n2,
                 n2 < 0 ? strerror(errno) : "n/a");
    }
    (void)si;
}

static void phase_b(void)
{
    struct tbt_p2_status st;
    char buf[8192];
    int n, rc;
    const char *hello = "PEER-BYTE-012345";
    uint32_t before;

    printf("-- B: peer sends first, then the module injects\n");
    if (open_conn())
    {
        bad("B open connection", "see errno");
        return;
    }
    if (write(cur.peer, hello, 16) != 16)
    {
        bad("B peer write of 16 bytes", strerror(errno));
        return;
    }
    usleep(100 * 1000);
    before = status(&st) == 0 ? st.rcv_nxt : 0;
    rc = inject(INJ, 0x42);
    if (rc)
    {
        bad("B inject after a real peer segment", strerror(-rc));
        dump_state("B");
    }
    else if (status(&st) == 0)
    {
        if (st.rcv_nxt - before == INJ)
            ok("B injection lands after the peer's bytes (rcv_nxt +4096)", NULL);
        else
            note("B rcv_nxt %u -> %u", before, st.rcv_nxt);
    }

    memset(buf, 0, sizeof(buf));
    n = read_upto(cur.srv, buf, 16 + INJ, 3000);
    if (n >= 16 && memcmp(buf, hello, 16) == 0)
        ok("B the peer's own bytes are queued first, unchanged", NULL);
    else
    {
        char d[96];
        snprintf(d, sizeof(d), "read %d bytes; prefix differs", n);
        bad("B the peer's own bytes are queued first, unchanged", d);
        dump_state("B");
    }
    if (n >= 16 + INJ && buf[16] == 0x42)
        ok("B the injected run follows the peer's bytes", NULL);
    else
        note("B byte 16 is 0x%02x of %d read", n > 16 ? (unsigned char)buf[16] : 0, n);
}

static void phase_c(void)
{
    struct tbt_p2_status st;
    unsigned char *rx;
    size_t got = 0;
    int i, inj_fail = 0, write_fail = 0;
    long long max_write_ms = 0;
    int last_wr_errno = 0;

    printf("-- C: 1 byte every 10 ms while injection runs\n");
    rx = malloc(65536);
    if (!rx || open_conn())
    {
        bad("C open connection", "see errno");
        free(rx);
        return;
    }
    for (i = 0; i < 40; i++)
    {
        char b = (char)('a' + (i % 26));
        long long t0 = now_ms();

        if (write(cur.peer, &b, 1) != 1)
        {
            write_fail++;
            last_wr_errno = errno;
        }
        if (now_ms() - t0 > max_write_ms)
            max_write_ms = now_ms() - t0;
        if (i % 8 == 3 && inject(INJ, (uint8_t)(0x60 + (i / 8))))
            inj_fail++;
        usleep(10 * 1000);
    }
    usleep(100 * 1000);

    if (write_fail == 0)
        ok("C every 1-byte peer write succeeded", NULL);
    else
        note("C %d of 40 peer writes failed (last errno=%s)", write_fail,
             strerror(last_wr_errno));
    if (max_write_ms < 1000)
        okf("C no peer write blocked", "max %lld ms", max_write_ms);
    else
        note("C a peer write blocked for %lld ms", max_write_ms);
    if (inj_fail == 0)
        ok("C every injection between peer bytes succeeded", NULL);
    else
        note("C %d of 5 injections failed", inj_fail);

    {
        struct pollfd p = {.fd = cur.srv, .events = POLLIN};
        long long deadline = now_ms() + 3000;

        while (got < 65536 && poll(&p, 1, 200) > 0)
        {
            ssize_t r = read(cur.srv, rx + got, 65536 - got);

            if (r <= 0)
                break;
            got += (size_t)r;
            if (now_ms() > deadline)
                break;
        }
    }
    {
        size_t want = 40 + 5 * INJ;

        if (got == want)
            okf("C nothing was lost across the interleave", "read %zu bytes", got);
        else
        {
            /* 40 individual bytes plus 5 whole runs can only add up to
             * 40 mod 4096. A different remainder means a partial injected run
             * was cut short, not that peer bytes went missing. */
            note("C %zu of %zu: %zu whole injected runs + %zu peer bytes",
                 got, want, got / INJ, got % INJ);
            dump_state("C");
        }
    }
    if (status(&st) == 0)
        note("C wup=%u wnd=%u nxt=%u copied=%u rmem=%u/%u", st.rcv_wup, st.rcv_wnd,
             st.rcv_nxt, st.copied_seq, st.mem_alloc, st.rcvbuf);
    free(rx);
}

static void phase_d(void)
{
    struct tcp_info ti;
    struct tbt_p2_status st;
    char b[512];
    size_t i;

    printf("-- D: counters and advertised window\n");
    if (open_conn())
    {
        bad("D open connection", "see errno");
        return;
    }
    inject(INJ, 0x43);
    for (i = 0; i < sizeof(b); i++)
        b[i] = (char)i;
    if (write(cur.peer, b, sizeof(b)) != (ssize_t)sizeof(b))
        note("D peer write of 512 bytes failed: %s", strerror(errno));
    usleep(300 * 1000);
    if (getsockopt(cur.peer, IPPROTO_TCP, TCP_INFO, &ti, &(socklen_t){sizeof(ti)}) == 0)
    {
        note("D peer: state=%u retrans=%u total_retrans=%u rcv_space=%u rcv_ssthresh=%u",
             ti.tcpi_state, ti.tcpi_retransmits, ti.tcpi_total_retrans, ti.tcpi_rcv_space,
             ti.tcpi_rcv_ssthresh);
        if (ti.tcpi_total_retrans == 0)
            ok("D the peer needed no retransmission", NULL);
        else
            note("D the peer retransmitted %u segment(s)", ti.tcpi_total_retrans);
    }
    if (status(&st) == 0)
    {
        long long window = (long long)st.rcv_wup + (long long)st.rcv_wnd -
                           (long long)st.rcv_nxt;
        long long used = (long long)st.rcv_nxt - (long long)st.rcv_wup;

        note("D us: snd_wnd=%u snd_nxt=%u snd_una=%u pkt_out=%u retrans=%u wmem=%u",
             st.snd_wnd, st.snd_nxt, st.snd_una, st.packets_out, st.retrans,
             st.wmem_queued);
        note("D us: wup=%u wnd=%u nxt=%u -> window left %lld, consumed since wup %lld",
             st.rcv_wup, st.rcv_wnd, st.rcv_nxt, window, used);
        if (window > 0)
            ok("D a non-zero window can still be advertised", NULL);
        else
            note("D the advertised window is exhausted");
    }
}

static void phase_e(void)
{
    struct tbt_p2_status st;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300 * 1000};
    char buf[16384];
    int n;

    printf("-- E: app-side semantics on injected data\n");
    if (open_conn())
    {
        bad("E open connection", "see errno");
        return;
    }

    n = recv(cur.srv, buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        ok("E MSG_PEEK on an empty queue returns EAGAIN", NULL);
    else
        note("E MSG_PEEK on an empty queue returned %d (errno=%s)", n,
             n < 0 ? strerror(errno) : "n/a");

    inject(2048, 0x44);
    n = recv(cur.srv, buf, 2048, MSG_PEEK | MSG_DONTWAIT);
    if (n == 2048 && buf[0] == 0x44)
        ok("E MSG_PEEK returns injected bytes", NULL);
    else
        note("E MSG_PEEK with 2048 injected returned %d", n);

    n = recv(cur.srv, buf, 2048, MSG_DONTWAIT);
    if (n == 2048 && buf[0] == 0x44)
        ok("E a read after MSG_PEEK still returns the same bytes", NULL);
    else
        note("E read after MSG_PEEK returned %d", n);

    setsockopt(cur.srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    {
        long long t0 = now_ms();

        n = read(cur.srv, buf, 4096);
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            okf("E SO_RCVTIMEO times out on an injected-only queue", "EAGAIN after %lld ms",
                now_ms() - t0);
        else
            note("E read with SO_RCVTIMEO returned %d (errno=%s)", n,
                 n < 0 ? strerror(errno) : "n/a");
    }

    /* MSG_WAITALL across two injected runs that arrive separately. */
    {
        struct timeval big = {.tv_sec = 2, .tv_usec = 0};

        setsockopt(cur.srv, SOL_SOCKET, SO_RCVTIMEO, &big, sizeof(big));
        inject(1024, 0x45);
        n = recv(cur.srv, buf, 6144, MSG_WAITALL);
        if (n > 1024)
            okf("E MSG_WAITALL kept reading past the first injected run",
                "returned %d bytes", n);
        else
            note("E MSG_WAITALL(6144) returned %d with 1024 queued", n);
    }

    if (status(&st) == 0)
        note("E nxt=%u copied=%u", st.rcv_nxt, st.copied_seq);
}

static void phase_f(void)
{
    struct tcp_info ti;
    char buf[4096];
    int n;
    long long deadline;

    printf("-- F: close with injected bytes unread\n");
    if (open_conn())
    {
        bad("F open connection", "see errno");
        return;
    }
    inject(8192, 0x47);
    /* Hand the socket back: closing this fd is the application's close(). */
    close(cur.srv);
    cur.srv = -1;

    deadline = now_ms() + 3000;
    while (now_ms() < deadline)
    {
        struct pollfd p = {.fd = cur.peer, .events = POLLIN};

        if (poll(&p, 1, 200) > 0)
            break;
    }
    if (getsockopt(cur.peer, IPPROTO_TCP, TCP_INFO, &ti, &(socklen_t){sizeof(ti)}) == 0)
    {
        if (ti.tcpi_state == TCP_CLOSE_WAIT)
            ok("F the peer sees FIN, not RST", "state CLOSE_WAIT");
        else if (ti.tcpi_state == TCP_CLOSE)
            note("F the peer is in CLOSE after our close (reset or full teardown)");
        else
            note("F peer TCP state %u after our close", ti.tcpi_state);
    }
    n = read(cur.peer, buf, sizeof(buf));
    if (n == 0)
        ok("F the peer reads EOF", NULL);
    else if (n > 0)
        note("F the peer read %d byte(s) before EOF", n);
    else
        note("F the peer read returned %d (errno=%s)", n, strerror(errno));
}

int main(void)
{
    struct tbt_p2_claim claim;

    signal(SIGPIPE, SIG_IGN);
    ctl = open("/dev/tbtprobe2", O_RDWR);
    if (ctl < 0)
    {
        perror("open /dev/tbtprobe2");
        return 1;
    }
    claim.tgid = (uint32_t)getpid();
    ioctl(ctl, TBT_P2_CLAIM, &claim);

    phase_a0();
    phase_a();
    phase_b();
    phase_c();
    phase_d();
    phase_e();
    phase_f();

    close(ctl);
    printf("%s\n", failures ? "PROBE2_FAIL" : "PROBE2_PASS");
    return failures ? 1 : 0;
}
