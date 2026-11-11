/* test_local_queue.c - the local queue bound in front of the FEC flow.
 *
 * The module bounds local queuing with local_queue_bytes: the parent
 * socket's SO_SNDBUF/SO_RCVBUF request takes a quarter (the kernel doubles
 * it) and the unencoded capture ring takes half. This test pins the pure
 * sizing rule and measures it on a real loopback TCP pair: fill the write
 * side until it blocks, then read the bytes still queued in the sender and
 * the receiver. The old 8 MiB request + 16 MiB ring allowed tens of MiB
 * ahead of a control frame; the default 1 MiB allows about one.
 */
#include "tbt_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/sockios.h>
#endif
#ifndef TIOCOUTQ
#define TIOCOUTQ 0x5411
#endif

static int failures;

static void ok(int cond, const char *what)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int set_int(int fd, int opt, int val)
{
    return setsockopt(fd, SOL_SOCKET, opt, &val, sizeof(val)) == 0;
}

static int get_int(int fd, int opt)
{
    int val = 0;
    socklen_t len = sizeof(val);

    if (getsockopt(fd, SOL_SOCKET, opt, &val, &len))
        return -1;
    return val;
}

/* Fill the client's send queue and the accepted socket's receive queue
 * with non-blocking writes. Returns the bytes still queued (unsent plus
 * unread), or -1. */
static long queue_after_fill(u32 local_queue, int *eff_snd, int *eff_rcv,
                             int *bytes_sent)
{
    int srv = -1, cli = -1, acc = -1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    char buf[16384];
    int req = (int)tbt_local_sockbuf_request(local_queue);
    long sent = 0;
    int outq = 0, inq = 0;

    *eff_snd = *eff_rcv = 0;
    *bytes_sent = 0;
    memset(buf, 0x5a, sizeof(buf));
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return -1;
    set_int(srv, SO_REUSEADDR, 1);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) ||
        listen(srv, 1) ||
        getsockname(srv, (struct sockaddr *)&a, &alen))
        goto out;
    cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0)
        goto out;
    if (!set_int(cli, SO_SNDBUF, req))
        goto out;
    if (fcntl(cli, F_SETFL, fcntl(cli, F_GETFL, 0) | O_NONBLOCK) < 0)
        goto out;
    if (connect(cli, (struct sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS)
        goto out;
    acc = accept(srv, NULL, NULL);
    if (acc < 0)
        goto out;
    if (!set_int(acc, SO_RCVBUF, req))
        goto out;
    for (;;)
    {
        ssize_t n = send(cli, buf, sizeof(buf), MSG_NOSIGNAL);

        if (n > 0)
        {
            sent += n;
            if (sent >= 64 * 1024 * 1024)
                break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n < 0 && errno == EINTR)
            continue;
        goto out;
    }
    if (ioctl(cli, TIOCOUTQ, &outq) || ioctl(acc, FIONREAD, &inq))
        goto out;
    *eff_snd = get_int(cli, SO_SNDBUF);
    *eff_rcv = get_int(acc, SO_RCVBUF);
    *bytes_sent = (int)sent;
    if (cli >= 0)
        close(cli);
    if (acc >= 0)
        close(acc);
    close(srv);
    return (long)outq + (long)inq;
out:
    if (cli >= 0)
        close(cli);
    if (acc >= 0)
        close(acc);
    if (srv >= 0)
        close(srv);
    return -1;
}

int main(void)
{
    u32 dflt = TBT_LOCAL_QUEUE_DEFAULT;
    int eff_snd, eff_rcv, sent;
    long q_dflt, q_legacy;
    double dflt_ms = 0.0, legacy_ms = 0.0;

    /* The pure sizing rule: a quarter to each socket request, half to the
     * capture ring, and the legacy 32 MiB budget stays exactly the old
     * 8 MiB + 16 MiB sizing. */
    ok(tbt_local_sockbuf_request(dflt) == 256u * 1024u,
       "default: 256 KiB socket request");
    ok(tbt_local_ring_cap(dflt) == 512u * 1024u,
       "default: 512 KiB capture ring cap");
    ok(2u * tbt_local_sockbuf_request(dflt) + tbt_local_ring_cap(dflt) ==
           dflt,
       "default: the doubling and the ring add to one budget");
    ok(tbt_local_sockbuf_request(TBT_LOCAL_QUEUE_MAX) == 8u * 1024u * 1024u,
       "32 MiB: 8 MiB socket request (the old value)");
    ok(tbt_local_ring_cap(TBT_LOCAL_QUEUE_MAX) == 16u * 1024u * 1024u,
       "32 MiB: 16 MiB ring cap (the old value)");
    ok(tbt_local_sockbuf_request(0) == 256u * 1024u,
       "0 selects the default");
    ok(tbt_local_ring_cap(1u) == 512u * 1024u,
       "a value below the minimum selects the default");
    ok(tbt_local_sockbuf_request(64u * 1024u * 1024u) == 8u * 1024u * 1024u,
       "a value above the maximum clamps to 32 MiB");

    q_dflt = queue_after_fill(dflt, &eff_snd, &eff_rcv, &sent);
    ok(q_dflt >= 0, "default: loopback fill completes");
    if (q_dflt >= 0)
    {
        dflt_ms = (double)q_dflt * 1000.0 / (128.0 * 1024.0 * 1024.0);
        printf("default: sent=%d sndbuf=%d rcvbuf=%d queued=%ld "
               "latency_at_128MBps=%.2fms\n",
               sent, eff_snd, eff_rcv, q_dflt, dflt_ms);
        /* The queue measured here is the sender's send buffer plus the
         * receiver's receive buffer; the doubling makes it the local
         * budget at most. */
        ok(q_dflt <= (long)2 * eff_snd + 2 * eff_rcv + 512 * 1024,
           "default: queued bytes stay inside the two buffers");
        ok(q_dflt <= (long)2 * dflt, "default: queued bytes stay near 1 MiB");
        ok(dflt_ms < 20.0, "default: under 20 ms at 128 MB/s");
    }

    q_legacy = queue_after_fill(TBT_LOCAL_QUEUE_MAX, &eff_snd, &eff_rcv,
                                &sent);
    ok(q_legacy >= 0, "legacy: loopback fill completes");
    if (q_legacy >= 0)
    {
        legacy_ms = (double)q_legacy * 1000.0 / (128.0 * 1024.0 * 1024.0);
        printf("legacy:  sent=%d sndbuf=%d rcvbuf=%d queued=%ld "
               "latency_at_128MBps=%.2fms\n",
               sent, eff_snd, eff_rcv, q_legacy, legacy_ms);
        ok(q_legacy <= (long)2 * eff_snd + 2 * eff_rcv + 512 * 1024,
           "legacy: queued bytes stay inside the two buffers");
        /* The comparison needs the kernel to grant a large buffer; a host
         * with tiny rmem_max/wmem_max cannot show the old 32 MiB error. */
        if (eff_snd >= 2 * 1024 * 1024 && eff_rcv >= 2 * 1024 * 1024)
        {
            ok(q_legacy > q_dflt,
               "legacy: a bigger budget really queues more");
            ok(legacy_ms > dflt_ms,
               "legacy: the same fill waits longer at 128 MB/s");
        }
        else
            printf("legacy: kernel capped the buffers; comparison skipped\n");
    }
    if (failures)
    {
        fprintf(stderr, "test_local_queue: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_local_queue: OK\n");
    return 0;
}
