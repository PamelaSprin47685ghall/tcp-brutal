/* tbt_echo.c - echo or discard server for the bench guest.
 *
 * One process, one listener, a fork per connection. SO_REUSEADDR lets the
 * listener survive the TIME_WAIT a reset leaves behind, which busybox `nc`
 * does not: the bench reconfigures tunnels many times per boot and a stale
 * listener would turn a correct reset into a broken next row.
 *
 * Usage: tbt_echo PORT [echo|sink]
 *   echo  send every received byte back (the tunnel's target, latency peer)
 *   sink  discard (the control flow's receive end)
 *
 * TCP_NODELAY on the accepted socket keeps the echo from coalescing a 1 KB
 * request behind Nagle.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sin;
    int echo = 1;
    int lfd, one = 1, port;

    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: tbt_echo PORT [echo|sink]\n");
        return 2;
    }
    port = atoi(argv[1]);
    if (argc == 3)
    {
        if (!strcmp(argv[2], "sink"))
            echo = 0;
        else if (strcmp(argv[2], "echo"))
        {
            fprintf(stderr, "tbt_echo: mode must be echo or sink\n");
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
    {
        perror("tbt_echo: socket");
        return 1;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("tbt_echo: bind");
        return 1;
    }
    if (listen(lfd, 64) < 0)
    {
        perror("tbt_echo: listen");
        return 1;
    }
    printf("tbt_echo: listening on %d (%s)\n", port, echo ? "echo" : "sink");
    fflush(stdout);

    for (;;)
    {
        int c = accept(lfd, NULL, NULL);
        char buf[16384];

        if (c < 0)
        {
            if (errno == EINTR)
                continue;
            perror("tbt_echo: accept");
            return 1;
        }
        if (fork() != 0)
        {
            close(c);
            continue;
        }
        close(lfd);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        for (;;)
        {
            ssize_t n = recv(c, buf, sizeof(buf), 0);

            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            if (echo && write_all(c, buf, (size_t)n))
                break;
        }
        close(c);
        _exit(0);
    }
}
