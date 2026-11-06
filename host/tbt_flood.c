/* tbt_flood.c - keep a far listener's accept queue busy with carriers that
 * never present a valid HELLO.
 *
 * usage: tbt_flood HOST PORT CONN SECONDS
 *
 * Each child repeatedly connects, writes a partial HTTP request (enough for
 * the far side to accept the socket and create a provisional carrier, never
 * enough to complete HELLO), holds it briefly, then closes with RST. The far
 * side must bound the provisional list and must keep serving legitimate
 * carriers while this runs; probe Q asserts both.
 *
 * This is the shape that hung HND0: 256 near carriers plus a connect flood
 * produced 10138 "ACCEPTED ... awaiting HELLO" lines, and the replacement
 * worker spent 15 s hogging a CPU while it retired the backlog.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void sleep_until(const struct timespec *deadline)
{
    struct timespec now;

    for (;;)
    {
        struct timespec left;

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
            return;
        left.tv_sec = deadline->tv_sec - now.tv_sec;
        left.tv_nsec = deadline->tv_nsec - now.tv_nsec;
        if (left.tv_nsec < 0)
        {
            left.tv_sec--;
            left.tv_nsec += 1000000000L;
        }
        nanosleep(&left, NULL);
    }
}

static const char req[] =
    "GET / HTTP/1.1\r\n"
    "Host: flood\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "20\r\n"; /* a chunk size and no record body: HELLO never completes */

static int connect_one(const struct sockaddr_in *sa)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int fl, r;

    if (fd < 0)
        return -1;
    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
    {
        close(fd);
        return -1;
    }
    r = connect(fd, (const struct sockaddr *)sa, sizeof(*sa));
    if (r < 0 && errno == EINPROGRESS)
    {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int err = 0;
        socklen_t el = sizeof(err);

        if (poll(&pfd, 1, 500) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err)
        {
            close(fd);
            return -1;
        }
    }
    else if (r < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa = { .sin_family = AF_INET };
    volatile int *stop;
    int port, nconn, secs, i;
    long done = 0;

    if (argc != 5)
    {
        fprintf(stderr, "usage: %s HOST PORT CONN SECONDS\n", argv[0]);
        return 2;
    }
    port = atoi(argv[2]);
    nconn = atoi(argv[3]);
    secs = atoi(argv[4]);
    if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1 || nconn <= 0 ||
        nconn > 4096 || secs <= 0)
        return 2;
    sa.sin_port = htons((unsigned short)port);
    /* A write to a socket the far refused must not kill the child: a child
     * death sends SIGCHLD, which used to cut the parent's sleep short and
     * make the flood end early. */
    signal(SIGPIPE, SIG_IGN);

    stop = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (stop == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }
    *stop = 0;

    for (i = 0; i < nconn; i++)
    {
        pid_t p = fork();

        if (p < 0)
        {
            perror("fork");
            *stop = 1;
            break;
        }
        if (p == 0)
        {
            unsigned seed = (unsigned)getpid() ^ (unsigned)time(NULL);
            struct linger lg = { .l_onoff = 1, .l_linger = 0 };

            while (!*stop)
            {
                int fd = connect_one(&sa);

                if (fd < 0)
                {
                    usleep(2000);
                    continue;
                }
                (void)!write(fd, req, sizeof(req) - 1);
                setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
                /* Hold the connection open briefly so the far's provisional
                 * list actually fills; then RST it and reconnect. */
                usleep(10000 + (rand_r(&seed) % 90000));
                close(fd);
            }
            _exit(0);
        }
    }

    {
        struct timespec deadline;

        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += secs;
        sleep_until(&deadline);
    }
    *stop = 1;
    while (wait(NULL) > 0)
        done++;
    printf("flood: %d children, %d joined, %ds\n", nconn, (int)done, secs);
    return 0;
}
