/* tbt_client_main.c - command line front end for the userspace client.
 *
 * The kernel reads one tunnel line from /proc/net/tbtun/config. This program
 * accepts the same keys on the command line (or as one line in a file) and
 * hands the line to tbt_parse_line(), so the two front ends cannot drift.
 *
 *   tbt_client role=near listen=127.0.0.1:19080 peer=[::1]:9000 \
 *              target=127.0.0.1:19001 n=8 m=24 mode=sc-ldpc carriers=256 \
 *              fec=on crypto=on adaptive=off drop_ppm=100000
 *
 * role defaults to near. A line without the `tunnel <id>` prefix gets
 * `tunnel 1` in front of it. Four keys belong to this process and are
 * removed before the line parses: drop_ppm, run_ms, local_queue_bytes and
 * verbose. With `--config FILE` the tunnel line comes from the file, the
 * PSKs stay in the file, and only those four keys may follow on the command
 * line; any other token is an error rather than a silent ignore.
 *
 * The userspace engine is an fec=on SC-LDPC endpoint with no port map,
 * source-address, device, source-port block or rate limiter. A line that
 * asks for those, or for fec=off, is refused instead of silently running
 * with different behavior.
 */
#define _GNU_SOURCE

#include "tbt_client_engine.h"
#include "tbt_client_args.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--config FILE] [role=near|far] key=value ...\n"
            "       keys: listen= target= peer= n= m= mode= carriers=\n"
            "             crypto= fec= w= L= profile= adaptive= keepalive=\n"
            "       userspace: drop_ppm= run_ms= local_queue_bytes= verbose=1\n"
            "       engine: fec=on mode=sc-ldpc carriers=256 only; map=\n"
            "       srcaddrs= dev= portblock= tunnel_rate= are refused\n"
            "       --config FILE reads the first tunnel line of FILE and\n"
            "       accepts only userspace keys after it\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct tbt_engine_cfg ec;
    struct tbt_parse_error perr = { 0 };
    struct tbt_client_us us;
    struct tbt_cmd cmd;
    char line[TBT_LINE_MAX];
    char bad[64];
    const char *why = NULL;
    int err;
    int bad_arg;
    int i;

    memset(&ec, 0, sizeof(ec));
    ec.cfg.id = 1;
    ec.cfg.role = TBT_ROLE_NEAR;
    ec.local_queue_bytes = TBT_LOCAL_QUEUE_DEFAULT;
    if (argc < 2)
    {
        usage(argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        }
    memset(&us, 0, sizeof(us));
    /* An absent key keeps the value the engine had before this change. */
    us.local_queue_bytes = TBT_LOCAL_QUEUE_DEFAULT;
    if (!strcmp(argv[1], "--config"))
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 2;
        }
        err = tbt_client_config_read(argv[2], line, sizeof(line));
        if (err)
        {
            fprintf(stderr, "tbt_client: %s: %s\n", argv[2],
                    err == -ENODATA ? "no tunnel line"
                    : err == -E2BIG ? "tunnel line too long"
                                    : strerror(-err));
            return 1;
        }
        if (tbt_client_us_strip(line, sizeof(line), &us, bad, sizeof(bad)))
        {
            fprintf(stderr, "tbt_client: %s: bad userspace value %s\n",
                    argv[2], bad[0] ? bad : "in the tunnel line");
            return 1;
        }
        bad_arg = tbt_client_us_argv(argc, argv, 3, &us);
        if (bad_arg >= 0)
        {
            fprintf(stderr,
                    "tbt_client: %s: not a userspace key\n"
                    "       after --config only drop_ppm= run_ms=\n"
                    "       local_queue_bytes= verbose= may follow\n",
                    argv[bad_arg]);
            return 1;
        }
        /* A file may hold the bare key list, like the command line. Give it
         * the same missing-directive prefix the inline path adds. */
        if (line[0] == '\0')
        {
            fprintf(stderr, "tbt_client: %s: no tunnel directive\n",
                    argv[2]);
            return 1;
        }
        if (strncmp(line, "tunnel ", 7) && strncmp(line, "tunnel\t", 7))
        {
            size_t n = strlen(line);

            if (n + 10 > sizeof(line))
            {
                fprintf(stderr, "tbt_client: %s: tunnel line too long\n",
                        argv[2]);
                return 1;
            }
            memmove(line + 9, line, n + 1);
            memcpy(line, "tunnel 1 ", 9);
        }
    }
    else
    {
        err = tbt_client_line_build(argc, argv, 1, line, sizeof(line), &us);
        if (err)
        {
            fprintf(stderr, "tbt_client: %s\n",
                    err == -E2BIG ? "command line too long"
                                  : "bad userspace key value");
            return 1;
        }
    }
    ec.drop_ppm = us.drop_ppm;
    ec.run_ms = us.run_ms;
    ec.local_queue_bytes = us.local_queue_bytes;
    ec.verbose = us.verbose > (u32)INT_MAX ? INT_MAX : (int)us.verbose;

    /* The documented default lives in this front end; the module parser
     * still insists on an explicit role. */
    if (tbt_client_role_default(line, sizeof(line)))
    {
        fprintf(stderr,
                "tbt_client: line too long to add the default role\n");
        return 1;
    }

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
    if (tbt_client_cfg_check(&cmd.cfg, &why))
    {
        fprintf(stderr, "tbt_client: %s\n",
                why ? why : "unsupported tunnel configuration");
        return 1;
    }
    ec.cfg = cmd.cfg;
    return tbt_engine_run(&ec) ? 1 : 0;
}
