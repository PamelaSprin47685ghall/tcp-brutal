/* tbt_client_args.c - split a tbt_client invocation into kernel and host.
 *
 * See tbt_client_args.h. The kernel line keeps every token the module's
 * grammar understands, in its original order. A userspace key is removed
 * after its value is stored, so a line copied from /proc still parses.
 */
#include "tbt_client_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tbt_client_us_key(const char *tok, const char *key, u32 *out)
{
    size_t n = strlen(key);
    const char *v;
    char *end = NULL;
    unsigned long x;

    if (strncmp(tok, key, n) || tok[n] != '=')
        return 0;
    v = tok + n + 1;
    if (!*v || *v == '-' || *v == '+')
        return -1;
    errno = 0;
    x = strtoul(v, &end, 0);
    if (errno || !end || end == v || *end || x != (u32)x)
        return -1;
    *out = (u32)x;
    return 1;
}

/* Try the four keys in one fixed order. A malformed value stops the scan:
 * leaving it in the line would report a confusing unknown-key error. */
static int us_token(const char *tok, struct tbt_client_us *us)
{
    int r;

    if ((r = tbt_client_us_key(tok, "drop_ppm", &us->drop_ppm)) != 0 ||
        (r = tbt_client_us_key(tok, "run_ms", &us->run_ms)) != 0 ||
        (r = tbt_client_us_key(tok, "local_queue_bytes",
                               &us->local_queue_bytes)) != 0 ||
        (r = tbt_client_us_key(tok, "verbose", &us->verbose)) != 0)
        return r;
    return 0;
}

int tbt_client_us_strip(char *line, size_t cap, struct tbt_client_us *us,
                        char *bad, size_t badcap)
{
    char *save = NULL;
    char *tok;
    size_t out = 0;
    bool first = true;

    if (bad && badcap)
        bad[0] = '\0';
    for (tok = strtok_r(line, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save))
    {
        size_t n;
        int r = us_token(tok, us);

        if (r < 0)
        {
            if (bad && badcap)
                snprintf(bad, badcap, "%s", tok);
            return -1;
        }
        if (r > 0)
            continue;
        n = strlen(tok);
        /* Compaction only removes bytes, and the input already fit. */
        if (out + n + 2 > cap)
        {
            if (bad && badcap)
                snprintf(bad, badcap, "line too long");
            return -1;
        }
        if (!first)
            line[out++] = ' ';
        first = false;
        memmove(line + out, tok, n);
        out += n;
    }
    line[out] = '\0';
    return 0;
}

int tbt_client_us_argv(int argc, char **argv, int start,
                       struct tbt_client_us *us)
{
    int i;

    for (i = start; i < argc; i++)
        if (us_token(argv[i], us) <= 0)
            return i;
    return -1;
}

/* A line that ran past the caller's buffer is drained to its newline, so its
 * tail can never be read as the next line. The visible prefix and the bytes
 * already consumed (c1, and c2 after a CR) decide whether the whole line was
 * blank or a comment, which the reader skips, or data, which is an overlong
 * tunnel line. Returns 0 for blank/comment, 1 for data, or a negative errno
 * on a read failure. */
static int overlong_line(FILE *f, const char *line, int c1, int c2)
{
    int pending[2];
    bool data = false;
    bool decided = false;
    int i;

    for (; *line == ' ' || *line == '\t' || *line == '\r'; line++)
        ;
    if (*line)
    {
        data = *line != '#';
        decided = true;
    }
    pending[0] = c1;
    pending[1] = c2;
    for (i = 0; i < 2 && !decided; i++)
    {
        int c = pending[i];

        if (c == EOF || c == ' ' || c == '\t' || c == '\r')
            continue;
        data = c != '#';
        decided = true;
    }
    for (;;)
    {
        int c = fgetc(f);

        if (c == EOF)
        {
            if (!ferror(f))
                break;
            return errno ? -errno : -EIO;
        }
        if (c == '\n')
            break;
        if (!decided && c != ' ' && c != '\t' && c != '\r')
        {
            data = c != '#';
            decided = true;
        }
    }
    return data ? 1 : 0;
}

int tbt_client_config_read(const char *path, char *line, size_t cap)
{
    FILE *f;
    int err = -ENODATA;

    /* fgets() needs room for at least the NUL; a cap this small can never
     * hold a line and would read line[-1] below. */
    if (cap < 2)
        return -EINVAL;
    f = fopen(path, "r");
    if (!f)
        return -errno;
    while (fgets(line, (int)cap, f))
    {
        char *p;
        size_t n;

        n = strlen(line);
        /* A full buffer without a newline may be an overlong line, not the
         * last line of the file. Read the terminator to tell them apart: LF,
         * CRLF (which may straddle the boundary), or EOF after a cap-1 byte
         * line is legal; any other byte means the line was cut off. */
        if (n == cap - 1 && line[n - 1] != '\n')
        {
            int c = fgetc(f);
            int c2 = EOF;
            bool complete;

            if (c == '\r')
                c2 = fgetc(f);
            complete = c == '\r' ? (c2 == EOF || c2 == '\n')
                                 : (c == EOF || c == '\n');
            if (!complete)
            {
                int over = overlong_line(f, line, c, c2);

                if (over < 0)
                {
                    err = over;
                    break;
                }
                if (over > 0)
                {
                    err = -E2BIG;
                    break;
                }
                /* An overlong comment or blank line: its tail is gone and
                 * the next line is the first real one. */
                continue;
            }
            if ((c == '\r' ? c2 : c) == EOF && ferror(f))
            {
                err = errno ? -errno : -EIO;
                break;
            }
        }
        /* Strip the terminator before the blank test, so a CRLF blank line
         * is skipped instead of returned as an empty first line. */
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        for (p = line; *p == ' ' || *p == '\t'; p++)
            ;
        if (*p == '\0' || *p == '#')
            continue;
        n = strlen(p);
        if (p != line)
            memmove(line, p, n + 1);
        err = 0;
        break;
    }
    if (err == -ENODATA && ferror(f))
        err = errno ? -errno : -EIO;
    fclose(f);
    return err;
}

int tbt_client_line_build(int argc, char **argv, int start, char *line,
                          size_t cap, struct tbt_client_us *us)
{
    char raw[TBT_LINE_MAX * 4];
    size_t used = 0;
    int i;
    int err;

    memset(raw, 0, sizeof(raw));
    for (i = start; i < argc; i++)
    {
        size_t n = strlen(argv[i]);

        if (used + n + 2 >= sizeof(raw))
            return -E2BIG;
        if (used)
            raw[used++] = ' ';
        memcpy(raw + used, argv[i], n);
        used += n;
    }
    raw[used] = '\0';
    if (snprintf(line, cap, "%s", raw) >= (int)cap)
        return -E2BIG;
    err = tbt_client_us_strip(line, cap, us, NULL, 0);
    if (err)
        return -EINVAL;
    if (strncmp(line, "tunnel ", 7) && strncmp(line, "tunnel\t", 7))
    {
        size_t n = strlen(line);

        if (n + 10 > cap)
            return -E2BIG;
        memmove(line + 9, line, n + 1);
        memcpy(line, "tunnel 1 ", 9);
    }
    return 0;
}

int tbt_client_role_default(char *line, size_t cap)
{
    char *p = line;
    size_t n;

    /* Only a token that begins with `role=` counts. A `role` that appears
     * inside another key or value must not suppress the default. */
    while (*p)
    {
        char *tok;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p)
            break;
        tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            p++;
        if ((size_t)(p - tok) >= 5 && !strncmp(tok, "role=", 5))
            return 0;
    }
    n = strlen(line);
    if (n + sizeof(" role=near") > cap)
        return -E2BIG;
    memcpy(line + n, " role=near", sizeof(" role=near"));
    return 0;
}

static int unsupported(const char **why, const char *reason)
{
    if (why)
        *why = reason;
    return -1;
}

int tbt_client_cfg_check(const struct tbt_cfg *cfg, const char **why)
{
    if (why)
        *why = NULL;
    if (!cfg->fec)
        return unsupported(why,
                           "the userspace engine implements the fec=on data plane only");
    if (cfg->nmap)
        return unsupported(why,
                           "map= is not implemented by the userspace client");
    if (cfg->nsrc)
        return unsupported(why,
                           "srcaddrs= is not implemented by the userspace client");
    if (cfg->dev_set)
        return unsupported(why,
                           "dev= is not implemented by the userspace client");
    if (cfg->portblock_lo || cfg->portblock_hi)
        return unsupported(why,
                           "portblock= is not implemented by the userspace client");
    if (cfg->tunnel_rate)
        return unsupported(why,
                           "tunnel_rate= is not implemented by the userspace client");
    return 0;
}
