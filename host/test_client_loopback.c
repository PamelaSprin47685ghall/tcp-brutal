/* test_client_loopback.c - end-to-end userspace near <-> far test.
 *
 * Four processes:
 *   echo   listens on the target port and echoes every byte;
 *   far    tbt_engine_run(role=far): accepts the 256-carrier pool, connects
 *          the target, forwards both directions;
 *   near   tbt_engine_run(role=near): listens for one application TCP,
 *          dials the pool, forwards both directions;
 *   client (this process) sends N MiB through near, reads the echo back and
 *          compares SHA-256 digests.
 *
 * The near and far engines are built with drop_ppm (default 10%) so the
 * NACK/replay path is exercised; the replay copies bypass the injector.
 */
#define _GNU_SOURCE

#include "tbt_client_engine.h"
#include "tbt_uapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TBT_LOOPBACK_MB
#define TBT_LOOPBACK_MB 64
#endif
#ifndef TBT_LOOPBACK_DROP
#define TBT_LOOPBACK_DROP 100000
#endif

/* ------------------------------------------------------------------ */
/* SHA-256, self-contained so the test needs no library.               */
/* ------------------------------------------------------------------ */

struct sha256
{
    u32 h[8];
    u64 bits;
    u8 buf[64];
    size_t len;
};

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 ror32(u32 x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha_block(struct sha256 *s, const u8 *p)
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 |
               (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
    {
        u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^
                 (w[i - 15] >> 3);
        u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^
                 (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        u32 s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + sha_k[i] + w[i];
        u32 s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                               0xa54ff53a, 0x510e527f, 0x9b05688c,
                               0x1f83d9ab, 0x5be0cd19 };

    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->len = 0;
}

static void sha_update(struct sha256 *s, const void *data, size_t len)
{
    const u8 *p = data;

    s->bits += (u64)len * 8;
    while (len)
    {
        size_t take = 64 - s->len;

        if (take > len)
            take = len;
        memcpy(s->buf + s->len, p, take);
        s->len += take;
        p += take;
        len -= take;
        if (s->len == 64)
        {
            sha_block(s, s->buf);
            s->len = 0;
        }
    }
}

static void sha_final(struct sha256 *s, u8 out[32])
{
    u64 bits = s->bits;
    u8 pad = 0x80;
    u8 zero = 0;
    u8 lenbuf[8];
    int i;

    sha_update(s, &pad, 1);
    while (s->len != 56)
        sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        lenbuf[i] = (u8)(bits >> (56 - i * 8));
    sha_update(s, lenbuf, 8);
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

static void sha_hex(const u8 d[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++)
    {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[64] = '\0';
}

/* ------------------------------------------------------------------ */

static void set_v4(struct tbt_addr *a, u16 port, u16 *be)
{
    a->family = TBT_AF_INET;
    a->u.v4 = htonl(0x7f000001u);
    *be = tbt_port_be16(port);
}

static void fill_cfg(struct tbt_engine_cfg *ec, bool near, u16 base,
                     u32 drop_ppm)
{
    struct tbt_cfg *c = &ec->cfg;

    memset(ec, 0, sizeof(*ec));
    c->id = near ? 1 : 2;
    c->role = near ? TBT_ROLE_NEAR : TBT_ROLE_FAR;
    c->profile = 1;
    c->fec = 1;
    c->adaptive = 0;
    c->mode = 3;
    c->crypto = 1;
    c->carriers = 256;
    if (near)
    {
        /* The configured n/m name the downlink shape. The uplink is the
         * fixed 8x32 GF(16) shape and must not inherit these values. */
        c->n = 128;
        c->m = 128;
        set_v4(&c->listen, (u16)(base + 1), &c->listen_port);
        set_v4(&c->peer, base, &c->peer_port);
        set_v4(&c->target, (u16)(base + 2), &c->target_port);
    }
    else
    {
        c->n = 128;
        c->m = 128;
        set_v4(&c->listen, base, &c->listen_port);
        set_v4(&c->target, (u16)(base + 2), &c->target_port);
    }
    ec->drop_ppm = drop_ppm;
    ec->run_ms = 120000;
    ec->verbose = 1;
}

static int wait_ready(u16 port, int ms)
{
    (void)port;
    /* Do not probe the application port: an accepted probe would create a
     * real tunnel flow, and the echo target accepts one connection. The
     * engines need a few timer ticks to finish the carrier handshakes. */
    usleep((useconds_t)ms * 1000);
    return 0;
}

static int echo_server(u16 port)
{
    struct sockaddr_in sin;
    int one = 1;
    int lfd, fd;
    u8 buf[65536];

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        return 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = tbt_port_be16(port);
    sin.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) || listen(lfd, 4))
        return 1;
    fd = accept(lfd, NULL, NULL);
    if (fd < 0)
        return 1;
    for (;;)
    {
        ssize_t n = read(fd, buf, sizeof(buf));
        size_t off = 0;

        if (n <= 0)
            break;
        while (off < (size_t)n)
        {
            ssize_t w = write(fd, buf + off, (size_t)n - off);

            if (w <= 0)
                return 0;
            off += (size_t)w;
        }
    }
    close(fd);
    close(lfd);
    return 0;
}

static void child_engine(struct tbt_engine_cfg *ec, const char *log)
{
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd >= 0)
    {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
    _exit(tbt_engine_run(ec) ? 1 : 0);
}

static void dump_log(const char *tag, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int lines = 0;

    if (!f)
        return;
    fprintf(stderr, "--- %s log (%s) ---\n", tag, path);
    while (fgets(line, sizeof(line), f) && lines++ < 80)
        fputs(line, stderr);
    fclose(f);
}

int main(void)
{
    /* A tail that is not a multiple of S forces one PADDED short DATA
     * share, so the loopback covers the 0.8.0 constant-frame path. */
    size_t total = (size_t)TBT_LOOPBACK_MB * 1024 * 1024 + 10000;
    /* Three consecutive ports. Two concurrent runs must not share them, so
     * allow a caller to partition the range (the same reason the QEMU host
     * partitions its hostfwd ports). */
    const char *base_env = getenv("TBT_LOOPBACK_BASE");
    u16 base = base_env ? (u16)atoi(base_env)
                        : (u16)(21000 + (getpid() % 1500) * 3);
    struct tbt_engine_cfg near_cfg, far_cfg;
    char far_log[128], near_log[128];
    pid_t far_pid, near_pid, echo_pid;
    int fd;
    u8 *data;
    u8 *back;
    struct sha256 sha_tx, sha_rx;
    u8 dig_tx[32], dig_rx[32];
    char hex_tx[65], hex_rx[65];
    size_t sent = 0, got = 0;
    u64 rng = 0x123456789abcdefull;
    bool shut = false;
    int status = 0;
    int i;

    signal(SIGPIPE, SIG_IGN);
    snprintf(far_log, sizeof(far_log), "/tmp/tbt-loopback-%d-far.log",
             (int)getpid());
    snprintf(near_log, sizeof(near_log), "/tmp/tbt-loopback-%d-near.log",
             (int)getpid());
    fill_cfg(&near_cfg, true, base, TBT_LOOPBACK_DROP);
    fill_cfg(&far_cfg, false, base, TBT_LOOPBACK_DROP);

    echo_pid = fork();
    if (echo_pid == 0)
        _exit(echo_server((u16)(base + 2)));
    far_pid = fork();
    if (far_pid == 0)
        child_engine(&far_cfg, far_log);
    near_pid = fork();
    if (near_pid == 0)
        child_engine(&near_cfg, near_log);

    if (wait_ready((u16)(base + 1), 700))
    {
        fprintf(stderr, "FAIL: near listen did not come up\n");
        status = 1;
        goto out;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        status = 1;
        goto out;
    }
    {
        struct sockaddr_in sin;

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = tbt_port_be16((u16)(base + 1));
        sin.sin_addr.s_addr = htonl(0x7f000001u);
        if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)))
        {
            fprintf(stderr, "FAIL: connect near: %s\n", strerror(errno));
            close(fd);
            status = 1;
            goto out;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    data = malloc(total);
    back = malloc(total);
    if (!data || !back)
    {
        fprintf(stderr, "FAIL: malloc\n");
        close(fd);
        status = 1;
        goto out;
    }
    for (i = 0; i < (int)total; i++)
    {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        data[i] = (u8)rng;
    }
    sha_init(&sha_tx);
    sha_update(&sha_tx, data, total);
    sha_final(&sha_tx, dig_tx);
    sha_init(&sha_rx);

    while (sent < total || got < total)
    {
        struct pollfd pfd;
        int pr;

        pfd.fd = fd;
        pfd.events = (sent < total ? POLLOUT : 0) | POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 30000);
        if (pr <= 0)
        {
            fprintf(stderr, "FAIL: poll timeout sent=%zu got=%zu\n", sent,
                    got);
            status = 1;
            break;
        }
        if (pfd.revents & POLLOUT)
        {
            size_t chunk = total - sent > 65536 ? 65536 : total - sent;
            ssize_t n = send(fd, data + sent, chunk, MSG_NOSIGNAL);

            if (n > 0)
                sent += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            {
                fprintf(stderr, "FAIL: send: %s\n", strerror(errno));
                status = 1;
                break;
            }
        }
        if (pfd.revents & (POLLIN | POLLHUP))
        {
            ssize_t n = recv(fd, back + got, total - got, 0);

            if (n > 0)
            {
                sha_update(&sha_rx, back + got, (size_t)n);
                got += (size_t)n;
            }
            else if (n == 0)
            {
                break;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                fprintf(stderr, "FAIL: recv: %s\n", strerror(errno));
                status = 1;
                break;
            }
        }
        if (sent == total && !shut)
        {
            /* The echo lags the last block by a NACK/ACK round trip. Shut
             * the write side now, or the reader would wait for an echo that
             * the far side cannot produce until it sees EOF. */
            (void)shutdown(fd, SHUT_WR);
            shut = true;
        }
        if (got == total)
            break;
    }
    if (sent == total && !shut)
        (void)shutdown(fd, SHUT_WR);
    close(fd);
    sha_final(&sha_rx, dig_rx);
    sha_hex(dig_tx, hex_tx);
    sha_hex(dig_rx, hex_rx);

    if (status == 0 && (got != total || memcmp(dig_tx, dig_rx, 32)))
    {
        fprintf(stderr, "FAIL: sent=%zu got=%zu tx=%s rx=%s\n", sent, got,
                hex_tx, hex_rx);
        status = 1;
    }
    if (status == 0)
        printf("PASS: %d MiB + 10000 B echo sha256=%s drop_ppm=%u\n",
               TBT_LOOPBACK_MB, hex_tx, (unsigned)TBT_LOOPBACK_DROP);
    free(data);
    free(back);

out:
    if (near_pid > 0)
        kill(near_pid, SIGKILL);
    if (far_pid > 0)
        kill(far_pid, SIGKILL);
    if (echo_pid > 0)
        kill(echo_pid, SIGKILL);
    if (near_pid > 0)
        waitpid(near_pid, NULL, 0);
    if (far_pid > 0)
        waitpid(far_pid, NULL, 0);
    if (echo_pid > 0)
        waitpid(echo_pid, NULL, 0);
    if (status)
    {
        dump_log("near", near_log);
        dump_log("far", far_log);
    }
    return status;
}
