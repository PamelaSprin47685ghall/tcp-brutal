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

/* The inline builder adds `tunnel 1 ` only to a line that lacks the
 * directive, and the prefix must fit the caller's buffer exactly. */
static void test_inline_cap(void)
{
    char line[32];
    char *argv[2];
    struct tbt_client_us us;

    memset(&us, 0, sizeof(us));
    argv[0] = (char *)"tbt_client";
    argv[1] = (char *)"listen=127.0.0.1:1";
    CHECK(tbt_client_line_build(2, argv, 1, line, 28, &us) == 0 &&
              !strcmp(line, "tunnel 1 listen=127.0.0.1:1"),
          "cap: the added prefix fits exactly");
    CHECK(tbt_client_line_build(2, argv, 1, line, 27, &us) == -E2BIG,
          "cap: one byte short for the added prefix is refused");

    argv[1] = (char *)"tunnel\t1 listen=127.0.0.1:1";
    CHECK(tbt_client_line_build(2, argv, 1, line, sizeof(line), &us) == 0 &&
              !strcmp(line, "tunnel 1 listen=127.0.0.1:1"),
          "cap: a tab after tunnel is normalized, not given a second prefix");
}

/* The documented role default is applied by the front end after the line
 * exists; the module parser still refuses a line without an explicit role. */
static void test_role_default(void)
{
    char line[TBT_LINE_MAX];
    char far_line[TBT_LINE_MAX];
    char small[64];
    struct tbt_cmd cmd;
    size_t n;

    snprintf(line, sizeof(line),
             "tunnel 1 listen=127.0.0.1:19080 peer=127.0.0.1:19081");
    n = strlen(line);
    CHECK(tbt_client_role_default(line, sizeof(line)) == 0,
          "role: default appended");
    CHECK(!strcmp(line + n, " role=near"),
          "role: the appended token is role=near");
    CHECK(parse_ok(line, &cmd) && cmd.cfg.role == TBT_ROLE_NEAR,
          "role: absent role parses as near");

    snprintf(far_line, sizeof(far_line),
             "tunnel 1 role=far listen=127.0.0.1:19080 "
             "target=127.0.0.1:19001");
    CHECK(tbt_client_role_default(far_line, sizeof(far_line)) == 0,
          "role: explicit far is accepted");
    CHECK(!strcmp(far_line, "tunnel 1 role=far listen=127.0.0.1:19080 "
                            "target=127.0.0.1:19001"),
          "role: explicit far is not rewritten");
    CHECK(parse_ok(far_line, &cmd) && cmd.cfg.role == TBT_ROLE_FAR,
          "role: explicit far survives parsing");

    snprintf(small, sizeof(small), "tunnel 1 listen=127.0.0.1:19080");
    n = strlen(small);
    CHECK(tbt_client_role_default(small, n + sizeof(" role=near") - 1) ==
              -E2BIG,
          "role: a buffer one byte short is -E2BIG");
    CHECK(strlen(small) == n, "role: a failed append leaves the line alone");
    CHECK(tbt_client_role_default(small, n + sizeof(" role=near")) == 0,
          "role: the exact fit succeeds");
    CHECK(!strcmp(small + n, " role=near"), "role: the exact fit appends");

    snprintf(line, sizeof(line),
             "tunnel 1 listen=127.0.0.1:19080 peer=127.0.0.1:19081 "
             "crypto_key=00role00 role_hint=far");
    CHECK(tbt_client_role_default(line, sizeof(line)) == 0 &&
              strstr(line, "crypto_key=00role00 role_hint=far role=near"),
          "role: 'role' inside another token does not suppress the default");

    snprintf(line, sizeof(line),
             "\t tunnel 1\trole=far listen=127.0.0.1:19080 "
             "target=127.0.0.1:19001\r\n");
    n = strlen(line);
    CHECK(tbt_client_role_default(line, sizeof(line)) == 0 &&
              strlen(line) == n,
          "role: a tab-delimited explicit role is not duplicated");
    CHECK(parse_ok(line, &cmd) && cmd.cfg.role == TBT_ROLE_FAR,
          "role: leading whitespace and CRLF still parse");

    snprintf(line, sizeof(line),
             "tunnel 1 listen=127.0.0.1:19080 peer=127.0.0.1:19081 "
             "keepalive=2500");
    CHECK(tbt_client_role_default(line, sizeof(line)) == 0 && parse_ok(line, &cmd) &&
              cmd.cfg.keepalive_ms == 2500,
          "role: keepalive= survives the default-role append");
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

/* A CRLF file may use blank lines for spacing; they are not the tunnel line.
 * The returned line carries no terminator, so the role default and the parser
 * see the same bytes a command line would. */
static void test_config_crlf(void)
{
    char line[TBT_LINE_MAX];
    struct tbt_cmd cmd;

    CHECK(write_file("  # comment\r\n\r\n \t\r\n"
                     "tunnel 1 listen=127.0.0.1:19080 "
                     "peer=127.0.0.1:19081\r\n") == 0,
          "crlf: file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0,
          "crlf: blank CRLF lines are skipped");
    CHECK(tbt_client_role_default(line, sizeof(line)) == 0,
          "crlf: the default role fits after a CRLF line");
    CHECK(parse_ok(line, &cmd) && cmd.cfg.role == TBT_ROLE_NEAR,
          "crlf: the terminator is stripped before parsing");
    unlink(path);
}

/* The reader's cap is a hard buffer bound: cap-1 content bytes plus the
 * newline fill it exactly, while one more byte is an overlong line. */
static void test_config_line_limit(void)
{
    char line[16];

    CHECK(write_file("123456789012345\n") == 0, "limit: file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0,
          "limit: cap-1 bytes before a newline fit");
    CHECK(!strcmp(line, "123456789012345"),
          "limit: the whole line is returned");
    unlink(path);

    CHECK(write_file("123456789012345\r\n") == 0, "limit: CRLF file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0 &&
              !strcmp(line, "123456789012345"),
          "limit: a CRLF terminator at the boundary fits");
    unlink(path);

    CHECK(write_file("1234567890123456\n") == 0, "limit: long file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == -E2BIG,
          "limit: one byte over the buffer is refused");
    unlink(path);

    CHECK(tbt_client_config_read("/nonexistent", line, 1) == -EINVAL,
          "limit: a buffer too small for any line is refused");
}

/* An unreadable path must report the read error, not the "no tunnel line"
 * sentinel, or the operator sees the wrong reason. */
static void test_config_read_error(void)
{
    char line[TBT_LINE_MAX];

    CHECK(tbt_client_config_read(".", line, sizeof(line)) == -EISDIR,
          "read error: a directory reports EISDIR, not -ENODATA");
}

/* A comment or blank line longer than the buffer must be drained: its tail
 * must never be mistaken for the tunnel line. Only an overlong data line is
 * an error. */
static void test_config_overlong(void)
{
    char line[32];

    CHECK(write_file("  # a comment much longer than the buffer\n"
                     "tunnel 1\n") == 0,
          "overlong: comment file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0 &&
              !strcmp(line, "tunnel 1"),
          "overlong: an overlong comment is discarded");
    unlink(path);

    CHECK(write_file("                               # late comment\n"
                     "tunnel 1\n") == 0,
          "overlong: late-comment file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0 &&
              !strcmp(line, "tunnel 1"),
          "overlong: a comment past the buffer boundary is still a comment");
    unlink(path);

    CHECK(write_file("                                        \n"
                     "tunnel 1\n") == 0,
          "overlong: blank file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == 0 &&
              !strcmp(line, "tunnel 1"),
          "overlong: an overlong blank line is discarded");
    unlink(path);

    CHECK(write_file("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"
                     "tunnel 1\n") == 0,
          "overlong: data file written");
    CHECK(tbt_client_config_read(path, line, sizeof(line)) == -E2BIG,
          "overlong: an overlong data line is refused");
    unlink(path);
}

/* The userspace engine is an fec=on SC-LDPC endpoint with no port map,
 * source-address, device, source-port block or rate limiter. The front end
 * refuses those instead of letting the engine ignore them. */
static void test_cfg_check(void)
{
    char line[TBT_LINE_MAX];
    struct tbt_cmd cmd;
    struct tbt_cfg c;
    const char *why = (const char *)"unset";

    snprintf(line, sizeof(line),
             "tunnel 1 role=near listen=127.0.0.1:19080 "
             "peer=127.0.0.1:19081 target=127.0.0.1:19001 n=8 m=24 "
             "w=64 L=8 fec=on mode=sc-ldpc carriers=256 crypto=on "
             "adaptive=off");
    CHECK(parse_ok(line, &cmd), "check: the base line parses");
    CHECK(tbt_client_cfg_check(&cmd.cfg, &why) == 0 && why == NULL,
          "check: the supported shape is accepted");

    c = cmd.cfg;
    c.fec = 0;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why &&
              strstr(why, "fec=on"),
          "check: fec=off is refused");

    c = cmd.cfg;
    c.profile = 3;
    CHECK(tbt_client_cfg_check(&c, &why) == 0,
          "check: profile= is left to the engine's framing");

    c = cmd.cfg;
    c.nmap = 1;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why && strstr(why, "map="),
          "check: map= is refused");

    c = cmd.cfg;
    c.nsrc = 1;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why &&
              strstr(why, "srcaddrs="),
          "check: srcaddrs= is refused");

    c = cmd.cfg;
    c.dev_set = true;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why && strstr(why, "dev="),
          "check: dev= is refused");

    c = cmd.cfg;
    c.portblock_lo = 40000;
    c.portblock_hi = 40100;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why &&
              strstr(why, "portblock="),
          "check: portblock= is refused");

    c = cmd.cfg;
    c.tunnel_rate = 1000;
    CHECK(tbt_client_cfg_check(&c, &why) == -1 && why &&
              strstr(why, "tunnel_rate="),
          "check: tunnel_rate= is refused");

    c = cmd.cfg;
    c.portblock_lo = 0;
    c.portblock_hi = 0;
    c.tunnel_rate = 0;
    CHECK(tbt_client_cfg_check(&c, &why) == 0,
          "check: explicit no-op defaults are accepted");
}

int main(void)
{
    test_config_trailing();
    test_config_rejects_kernel_key();
    test_bad_value();
    test_file_then_argv_precedence();
    test_inline();
    test_inline_cap();
    test_role_default();
    test_empty_config();
    test_config_crlf();
    test_config_line_limit();
    test_config_read_error();
    test_config_overlong();
    test_cfg_check();
    printf("test_client_args: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
