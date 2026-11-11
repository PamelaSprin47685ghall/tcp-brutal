/* test_client_args.c - --config FILE and trailing userspace keys.
 *
 * The client keeps four keys out of the kernel grammar: drop_ppm, run_ms,
 * local_queue_bytes and verbose. With --config FILE the tunnel line (and
 * its PSKs) stays in the file; only those four keys may follow on the
 * command line, and any other token is an error. These cases pin that
 * split, the file precedence order and the byte-exact kernel line.
 */
#define _GNU_SOURCE

#include "tbt_client_args.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

#define CHECK(cond, name)                 \
    do                                    \
    {                                     \
        checks++;                         \
        if (!(cond))                      \
        {                                 \
            failures++;                   \
            printf("FAIL %s\n", name);    \
        }                                 \
    } while (0)

static const char *const key_hex =
    "a1b2c3d4e5f60718293a4b5c6d7e8f90"
    "a1b2c3d4e5f60718293a4b5c6d7e8f90";

static char path[256];

/* One temp file per case. The name is released by unlink in the caller. */
static int write_file(const char *body)
{
    int fd;

    snprintf(path, sizeof(path), "%s/tbt-args-XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    if (write(fd, body, strlen(body)) != (ssize_t)strlen(body))
    {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

static int parse_ok(char *line, struct tbt_cmd *cmd)
{
    struct tbt_parse_error perr = { 0 };

    return tbt_parse_line(line, cmd, &perr) == 0 &&
           cmd->kind == TBT_CMD_TUNNEL;
}

/* The exact invocation from the ticket: the file owns the tunnel line and
 * the PSK, and two userspace flags follow the path. */
static void test_config_trailing(void)
{
    char body[TBT_LINE_MAX];
    char line[TBT_LINE_MAX];
    char bad[64];
    char *argv[8];
    struct tbt_client_us us;
    struct tbt_cmd cmd;
    int argc;

    snprintf(body, sizeof(body),
             "# comment\n"
             "\n"
             "tunnel 1 role=near listen=127.0.0.1:19080 "
             "peer=127.0.0.1:19081 n=128 m=128 fec=on mode=sc-ldpc w=256 L=8 "
             "carriers=256 crypto=on crypto_key=%s\n",
             key_hex);
    CHECK(write_file(body) == 0, "config: temp file written");
    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"--config";
    argv[2] = path;
    argv[3] = (char *)"verbose=1";
    argv[4] = (char *)"local_queue_bytes=2097152";
    argc = 5;
    memset(&us, 0, sizeof(us));
    us.local_queue_bytes = TBT_LOCAL_QUEUE_DEFAULT;
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0,
          "config: first tunnel line read");
    CHECK(strstr(line, "crypto_key=") != NULL, "config: PSK lives in the file");
    CHECK(tbt_client_us_strip(line, sizeof(line), &us, bad, sizeof(bad)) == 0,
          "config: the line holds no userspace key");
    CHECK(tbt_client_us_argv(argc, argv, 3, &us) == -1,
          "config: both trailing keys are userspace keys");
    CHECK(us.verbose == 1, "config: verbose applies");
    CHECK(us.local_queue_bytes == 2097152,
          "config: local_queue_bytes applies");
    CHECK(us.drop_ppm == 0 && us.run_ms == 0,
          "config: absent keys keep their defaults");
    CHECK(strstr(argv[3], "crypto_key") == NULL &&
              strstr(argv[4], "crypto_key") == NULL,
          "config: no PSK is named on the command line");
    CHECK(parse_ok(line, &cmd) && cmd.cfg.crypto == 1,
          "config: the stripped line still parses with crypto on");
    unlink(path);
}

/* A non-userspace token after --config is an error, never a silent ignore. */
static void test_config_rejects_kernel_key(void)
{
    char body[TBT_LINE_MAX];
    char line[TBT_LINE_MAX];
    char bad[64];
    char *argv[5];
    struct tbt_client_us us;
    int bad_arg;

    snprintf(body, sizeof(body),
             "tunnel 1 role=near listen=127.0.0.1:19080 "
             "peer=127.0.0.1:19081\n");
    CHECK(write_file(body) == 0, "reject: temp file written");
    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"--config";
    argv[2] = path;
    argv[3] = (char *)"n=64";
    memset(&us, 0, sizeof(us));
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0,
          "reject: line read");
    CHECK(tbt_client_us_strip(line, sizeof(line), &us, bad, sizeof(bad)) == 0,
          "reject: line clean");
    bad_arg = tbt_client_us_argv(4, argv, 3, &us);
    CHECK(bad_arg == 3, "reject: n=64 is refused at its index");
    unlink(path);
}

/* A userspace key with a malformed value must not be left in the kernel
 * line for the parser to mis-name. */
static void test_bad_value(void)
{
    char line[64];
    char bad[64];
    struct tbt_client_us us;
    char *argv[3];

    snprintf(line, sizeof(line), "%s", "tunnel 1 verbose=abc n=64");
    memset(&us, 0, sizeof(us));
    CHECK(tbt_client_us_strip(line, sizeof(line), &us, bad, sizeof(bad)) == -1,
          "bad value: strip reports it");
    CHECK(!strcmp(bad, "verbose=abc"), "bad value: strip names the token");

    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"drop_ppm=1x";
    argv[2] = (char *)"run_ms=5";
    CHECK(tbt_client_us_argv(3, argv, 1, &us) == 1,
          "bad value: argv reports the malformed token");
    CHECK(tbt_client_us_key("verbose=1", "verbose", &us.verbose) == 1 &&
              us.verbose == 1,
          "bad value: a decimal value parses");
    CHECK(tbt_client_us_key("verbose=0x10", "verbose", &us.verbose) == 1 &&
              us.verbose == 16,
          "bad value: a hex value parses");
    CHECK(tbt_client_us_key("verbose=-1", "verbose", &us.verbose) == -1,
          "bad value: a negative value is refused");
    CHECK(tbt_client_us_key("verbose=1x", "verbose", &us.verbose) == -1,
          "bad value: a trailing byte is refused");
    CHECK(tbt_client_us_key("verbosex=1", "verbose", &us.verbose) == 0,
          "bad value: a longer key is not this key");
}

/* The file supplies a default and the command line overrides it. */
static void test_file_then_argv_precedence(void)
{
    char body[64];
    char line[128];
    char bad[32];
    struct tbt_client_us us;
    char *argv[3];

    snprintf(body, sizeof(body), "drop_ppm=7 run_ms=100\n");
    CHECK(write_file(body) == 0, "precedence: file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0,
          "precedence: line read");
    memset(&us, 0, sizeof(us));
    CHECK(tbt_client_us_strip(line, sizeof(line), &us, bad, sizeof(bad)) == 0,
          "precedence: file keys stripped");
    CHECK(us.drop_ppm == 7 && us.run_ms == 100,
          "precedence: file keys apply");
    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"drop_ppm=9";
    CHECK(tbt_client_us_argv(2, argv, 1, &us) == -1,
          "precedence: command line parsed");
    CHECK(us.drop_ppm == 9 && us.run_ms == 100,
          "precedence: command line overrides, others stay");
    unlink(path);
}

/* The inline form keeps its kernel tokens in order, adds the tunnel
 * prefix once and never forwards a userspace key. */
static void test_inline(void)
{
    char line[TBT_LINE_MAX];
    char *argv[6];
    struct tbt_client_us us;
    struct tbt_cmd cmd;

    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"role=near";
    argv[2] = (char *)"listen=127.0.0.1:19080";
    argv[3] = (char *)"drop_ppm=1000";
    argv[4] = (char *)"verbose=1";
    argv[5] = (char *)"peer=127.0.0.1:19081";
    memset(&us, 0, sizeof(us));
    us.local_queue_bytes = TBT_LOCAL_QUEUE_DEFAULT;
    CHECK(tbt_client_line_build(6, argv, 1, line, sizeof(line), &us) == 0,
          "inline: line builds");
    CHECK(!strncmp(line, "tunnel 1 role=near listen=127.0.0.1:19080 peer=",
                   strlen("tunnel 1 role=near listen=127.0.0.1:19080 "
                          "peer=")),
          "inline: kernel order survives and the prefix is added");
    CHECK(strstr(line, "drop_ppm") == NULL && strstr(line, "verbose") == NULL,
          "inline: userspace keys left the line");
    CHECK(us.drop_ppm == 1000 && us.verbose == 1,
          "inline: userspace keys applied");
    CHECK(parse_ok(line, &cmd), "inline: the line parses");
}

/* A config file with no tunnel line is a clear -ENODATA. */
static void test_empty_config(void)
{
    char line[TBT_LINE_MAX];

    CHECK(write_file("# only a comment\n\n") == 0, "empty: file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == -ENODATA,
          "empty: no tunnel line reports -ENODATA");
    unlink(path);
}

int main(void)
{
    test_config_trailing();
    test_config_rejects_kernel_key();
    test_bad_value();
    test_file_then_argv_precedence();
    test_inline();
    test_empty_config();
    printf("test_client_args: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
