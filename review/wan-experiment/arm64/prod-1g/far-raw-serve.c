/* far-raw-serve.c - minimal 1 GiB HTTP source for the acceptance run.
 *
 * The python sender kept a 1 GiB file in page cache and lived on the same
 * 955 MB host as the 256-carrier module; the 2026-11-10 freeze happened with
 * journald already reporting memory pressure. This binary keeps no heap copy
 * of the file: sendfile() moves it from the page cache, and the process
 * resident set stays at a few pages.
 *
 *   GET  -> 200 + Content-Length + body
 *   HEAD -> the same headers, no body
 *   other -> 405
 *
 * One connection at a time, sequential accept. No send timeout: a slow
 * tunnel must not truncate the response.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path;
    const char *bind_addr;
    int port, fd, srv, one = 1;
    struct stat st;
    struct sockaddr_in a;

    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <file> <bind-ip> <port>\n", argv[0]);
        return 2;
    }
    path = argv[1];
    bind_addr = argv[2];
    port = atoi(argv[3]);

    signal(SIGPIPE, SIG_IGN);
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0)
    {
        perror(path);
        return 3;
    }
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return 3;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1 ||
        bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 8) != 0)
    {
        perror("bind/listen");
        return 3;
    }
    fprintf(stderr, "listen %s:%d file=%s size=%lld\n", bind_addr, port, path,
            (long long)st.st_size);

    for (;;)
    {
        int c = accept(srv, NULL, NULL);
        char req[4096], hdr[256], b[4096];
        ssize_t n;
        int hl, head;
        struct timeval tv = {5, 0};

        if (c < 0)
            continue;
        n = recv(c, req, sizeof(req) - 1, 0);
        head = n >= 4 && memcmp(req, "HEAD", 4) == 0;
        if (!(n >= 3 && memcmp(req, "GET", 3) == 0) && !head)
        {
            const char *e = "HTTP/1.1 405 Method Not Allowed\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, e, strlen(e), MSG_NOSIGNAL);
            close(c);
            continue;
        }
        hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %lld\r\n"
                      "Connection: close\r\n\r\n",
                      (long long)st.st_size);
        if (send(c, hdr, (size_t)hl, MSG_NOSIGNAL) == hl && !head)
        {
            off_t off = 0;
            while (off < st.st_size)
            {
                ssize_t sent = sendfile(c, fd, &off, (size_t)(st.st_size - off));
                if (sent <= 0)
                    break;
            }
        }
        shutdown(c, SHUT_WR);
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (recv(c, b, sizeof(b), 0) > 0)
            ;
        close(c);
    }
}
