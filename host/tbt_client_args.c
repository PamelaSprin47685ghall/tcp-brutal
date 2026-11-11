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

int tbt_client_config_read(const char *path, char *line, size_t cap)
{
    FILE *f;
    int err = -ENODATA;

    f = fopen(path, "r");
    if (!f)
        return -errno;
    while (fgets(line, (int)cap, f))
    {
        char *p = line;
        size_t n;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;
        n = strlen(line);
        /* A full buffer without a newline may be an overlong line, not the
         * last line of the file. Read one byte to tell them apart. */
        if (n == cap - 1 && line[n - 1] != '\n' && fgetc(f) != EOF)
        {
            err = -E2BIG;
            break;
        }
        n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r'))
            p[--n] = '\0';
        if (p != line)
            memmove(line, p, n + 1);
        err = 0;
        break;
    }
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
