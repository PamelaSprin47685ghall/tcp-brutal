/* tbt_client_main.c - command line front end for the userspace client.
 *
 * The kernel reads one tunnel line from /proc/net/tbtun/config. This program
 * accepts the same keys on the command line (or as one line in a file) and
 * hands the line to tbt_parse_line(), so the two front ends cannot drift.
 *
 *   tbt_client role=near listen=127.0.0.1:19080 peer=[::1]:9000 \
 *              target=127.0.0.1:19001 n=8 m=24 mode=sc-ldpc carriers=256 \
 *              crypto=on drop_ppm=100000
 *
 * role defaults to near. A line without the `tunnel <id>` prefix gets
 * `tunnel 1` in front of it.
 */
#define _GNU_SOURCE

#include "tbt_client_engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--config FILE] [role=near|far] key=value ...\n"
            "       keys: listen= target= peer= n= m= mode= carriers= crypto=\n"
            "             w= L= profile= adaptive= keepalive_ms=\n"
            "       userspace: drop_ppm= run_ms= verbose=1\n"
            "       --config FILE reads the first tunnel line of FILE\n",
            argv0);
}

/* Remove a userspace-only key from a token stream. Returns true when the
 * token was consumed. */
static bool take_u32_key(const char *tok, const char *key, u32 *out)
{
    size_t n = strlen(key);

    if (strncmp(tok, key, n) || tok[n] != '=')
        return false;
    *out = (u32)strtoul(tok + n + 1, NULL, 0);
    return true;
}

static int build_line(int argc, char **argv, char *line, size_t cap,
                      struct tbt_engine_cfg *ec)
{
    char raw[TBT_LINE_MAX * 4];
    size_t used = 0;
    int i;

    memset(raw, 0, sizeof(raw));
    for (i = 1; i < argc; i++)
    {
        size_t n;

        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            exit(0);
        }
        n = strlen(argv[i]);
        if (used + n + 2 >= sizeof(raw))
        {
            fprintf(stderr, "tbt_client: command line too long\n");
            return -E2BIG;
        }
        if (used)
            raw[used++] = ' ';
        memcpy(raw + used, argv[i], n);
        used += n;
    }
    raw[used] = '\0';
    /* A userspace key must not reach the kernel grammar. */
    {
        char clean[TBT_LINE_MAX * 4];
        char *save = NULL;
        char *tok;
        size_t out = 0;
        bool first = true;

        memset(clean, 0, sizeof(clean));
        for (tok = strtok_r(raw, " \t\r\n", &save); tok;
             tok = strtok_r(NULL, " \t\r\n", &save))
        {
            if (take_u32_key(tok, "drop_ppm", &ec->drop_ppm) ||
                take_u32_key(tok, "run_ms", &ec->run_ms) ||
                take_u32_key(tok, "verbose", (u32 *)&ec->verbose))
                continue;
            if (!first && out + 1 < sizeof(clean))
                clean[out++] = ' ';
            first = false;
            if (out + strlen(tok) + 1 >= sizeof(clean))
            {
                fprintf(stderr, "tbt_client: line too long\n");
                return -E2BIG;
            }
            memcpy(clean + out, tok, strlen(tok));
            out += strlen(tok);
        }
        clean[out] = '\0';
        if (!strncmp(clean, "tunnel ", 7) || !strncmp(clean, "tunnel\t", 7))
            snprintf(line, cap, "%s", clean);
        else
            snprintf(line, cap, "tunnel 1 %s", clean);
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct tbt_engine_cfg ec;
    struct tbt_parse_error perr = { 0 };
    struct tbt_cmd cmd;
    char line[TBT_LINE_MAX];
    int err;

    memset(&ec, 0, sizeof(ec));
    ec.cfg.id = 1;
    ec.cfg.role = TBT_ROLE_NEAR;

    if (argc < 2)
    {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--config"))
    {
        FILE *f;
        char file_line[TBT_LINE_MAX];

        if (argc < 3)
        {
            usage(argv[0]);
            return 2;
        }
        f = fopen(argv[2], "r");
        if (!f)
        {
            fprintf(stderr, "tbt_client: %s: %s\n", argv[2],
                    strerror(errno));
            return 1;
        }
        memset(file_line, 0, sizeof(file_line));
        while (fgets(file_line, sizeof(file_line), f))
        {
            char *p = file_line;
            size_t n;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '\0' || *p == '\n' || *p == '#')
                continue;
            n = strlen(p);
            while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
                p[--n] = '\0';
            if (snprintf(line, sizeof(line), "%s", p) >= (int)sizeof(line))
            {
                fclose(f);
                fprintf(stderr, "tbt_client: config line too long\n");
                return 1;
            }
            fclose(f);
            goto parsed;
        }
        fclose(f);
        fprintf(stderr, "tbt_client: %s: no tunnel line\n", argv[2]);
        return 1;
    }
    err = build_line(argc, argv, line, sizeof(line), &ec);
    if (err)
        return 1;

parsed:
    memset(&cmd, 0, sizeof(cmd));
    err = tbt_parse_line(line, &cmd, &perr);
    if (err)
    {
        fprintf(stderr, "tbt_client: %s: %s (line: %s)\n",
                perr.key ? perr.key : "line",
                perr.why ? perr.why : strerror(-err), line);
        return 1;
    }
    if (cmd.kind != TBT_CMD_TUNNEL)
    {
        fprintf(stderr, "tbt_client: not a tunnel line: %s\n", line);
        return 1;
    }
    ec.cfg = cmd.cfg;
    return tbt_engine_run(&ec) ? 1 : 0;
}
