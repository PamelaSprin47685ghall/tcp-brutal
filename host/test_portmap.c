/* test_portmap.c - host conformance test for the carrier source-port map.
 *
 * Links tbtun/tbt_portmap.c and tbtun/tbt_parse.c, the same code the module
 * uses. No root, no module, no kernel headers.
 *
 * One absolute bitmap covers ports 1..65535; a block is a search window over
 * it. The production shape is 256 carriers. This test pins the window rules
 * (per-tunnel block, 0-0 fallback, range validation), the parse rules, the
 * boundary ports 1 and 65535, exhaustion and lowest-first reuse, and the
 * conflict rule that two overlapping windows - including the global fallback
 * and a per-tunnel block - can never be handed the same live port.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tbt_parse.h"
#include "tbt_portmap.h"

/* Large enough for a 32-bit `unsigned long` (2048 words); a 64-bit build
 * needs 1024. main() checks that the real requirement fits. */
#define TEST_USED_WORDS (TBT_PORTMAP_PORTS / 32u)
static unsigned long used[TEST_USED_WORDS];

static int failures;
static int checks;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static void reset_used(void)
{
    memset(used, 0, sizeof(used));
}

static int parse_line(const char *text, struct tbt_cmd *cmd,
                      struct tbt_parse_error *err)
{
    char buf[TBT_LINE_MAX];
    size_t len = strlen(text);

    if (len >= sizeof(buf))
        return -1;
    memcpy(buf, text, len + 1);
    return tbt_parse_line(buf, cmd, err);
}

static void test_words_and_fits(void)
{
    unsigned int bits = tbt_portmap_words() * (unsigned int)(8 * sizeof(unsigned long));

    ok(bits >= TBT_PORTMAP_PORTS, "bitmap covers the whole port space", NULL);
    ok(tbt_portmap_words() <= TEST_USED_WORDS, "test bitmap is large enough", NULL);
    ok(tbt_portmap_fits(30000, 30511, 512), "512-port block holds 512 slots", NULL);
    ok(!tbt_portmap_fits(30000, 30510, 512), "511-port block does not hold 512", NULL);
    ok(tbt_portmap_fits(40000, 40063, 64), "64-port block holds 64", NULL);
    ok(!tbt_portmap_fits(40000, 40062, 64), "63-port block does not hold 64", NULL);
    ok(tbt_portmap_fits(1, 1, 1), "port 1 is a valid one-port block", NULL);
    ok(tbt_portmap_fits(65535, 65535, 1), "port 65535 is a valid one-port block", NULL);
    ok(tbt_portmap_fits(65534, 65535, 2), "the top two ports hold two slots", NULL);
    ok(!tbt_portmap_fits(0, 0, 1), "0-0 is not a block", NULL);
    ok(!tbt_portmap_fits(40000, 39999, 1), "reversed block is not a block", NULL);
    ok(tbt_portmap_fits(1, 65535, 256), "the whole space holds a 256-carrier session", NULL);
}

static void test_window(void)
{
    u16 lo = 0, hi = 0;

    ok(tbt_portmap_window(30000, 30511, 40000, 44095, &lo, &hi) == 0 &&
           lo == 30000 && hi == 30511,
       "per-tunnel block wins over the global block", NULL);
    ok(tbt_portmap_window(0, 0, 40000, 44095, &lo, &hi) == 0 &&
           lo == 40000 && hi == 44095,
       "0-0 falls back to the global block", NULL);
    ok(tbt_portmap_window(30000, 30511, 0, 0, &lo, &hi) == 0 &&
           lo == 30000 && hi == 30511,
       "per-tunnel block needs no valid global block", NULL);
    ok(tbt_portmap_window(0, 0, 0, 0, &lo, &hi) == -EINVAL,
       "no per-tunnel block and no global block is invalid", NULL);
    ok(tbt_portmap_window(500, 400, 40000, 44095, &lo, &hi) == -EINVAL,
       "reversed per-tunnel block is invalid", NULL);
    ok(tbt_portmap_window(0, 5, 40000, 44095, &lo, &hi) == -EINVAL,
       "per-tunnel block starting at 0 is invalid", NULL);
    ok(tbt_portmap_window(40000, 44095, 40000, 44095, NULL, &hi) == -EINVAL,
       "missing output pointer is invalid", NULL);
}

static void test_alloc_256(void)
{
    unsigned int i;
    u16 p;

    reset_used();
    for (i = 0; i < 256; i++)
        ok(tbt_portmap_alloc(used, 30000, 30511, &p) == 0 &&
               p == (u16)(30000 + i),
           "256 carriers allocate lowest-first", NULL);
    for (i = 256; i < 512; i++)
        ok(tbt_portmap_alloc(used, 30000, 30511, &p) == 0 &&
               p == (u16)(30000 + i),
           "the window fills to its last port", NULL);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == -ENOSPC,
       "exhausted window reports ENOSPC", NULL);

    /* Freed ports come back lowest-first. */
    tbt_portmap_free(used, 30005);
    tbt_portmap_free(used, 30100);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == 0 && p == 30005,
       "first freed port is reused", NULL);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == 0 && p == 30100,
       "second freed port is reused", NULL);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == -ENOSPC,
       "window is full again", NULL);

    /* Out-of-window frees and port 0 are no-ops, not corruption. */
    tbt_portmap_free(used, 29999);
    tbt_portmap_free(used, 30512);
    tbt_portmap_free(used, 0);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == -ENOSPC,
       "out-of-window free did not lose a port", NULL);

    /* Double free is idempotent: the bit is clear, not a counter. */
    tbt_portmap_free(used, 30005);
    tbt_portmap_free(used, 30005);
    ok(tbt_portmap_alloc(used, 30000, 30511, &p) == 0 && p == 30005,
       "double free leaves the port reusable", NULL);
}

static void test_overlap(void)
{
    u16 p;
    unsigned int i;

    reset_used();
    for (i = 0; i < 4; i++)
        ok(tbt_portmap_alloc(used, 30000, 30003, &p) == 0 &&
               p == (u16)(30000 + i),
           "window A allocates its four ports", NULL);

    /* Window B overlaps A's top three ports. The shared bitmap makes B skip
     * them; a per-window bitmap would hand out 30001 here. */
    ok(tbt_portmap_alloc(used, 30001, 30005, &p) == 0 && p == 30004,
       "overlapping window skips live ports from the other window", NULL);
    ok(tbt_portmap_alloc(used, 30001, 30005, &p) == 0 && p == 30005,
       "overlapping window takes its own free port", NULL);
    ok(tbt_portmap_alloc(used, 30001, 30005, &p) == -ENOSPC,
       "overlapping window exhausts before reusing a live port", NULL);

    tbt_portmap_free(used, 30001);
    ok(tbt_portmap_alloc(used, 30001, 30005, &p) == 0 && p == 30001,
       "a port freed in one window is available to the other", NULL);

    /* The global fallback and a per-tunnel block share the same bitmap. */
    reset_used();
    for (i = 0; i < 16; i++)
        ok(tbt_portmap_alloc(used, 40000, 44095, &p) == 0 &&
               p == (u16)(40000 + i),
           "global window allocates its first 16 ports", NULL);
    ok(tbt_portmap_alloc(used, 40000, 40063, &p) == 0 && p == 40016,
       "per-tunnel window skips the global window's live ports", NULL);
    tbt_portmap_free(used, 40003);
    ok(tbt_portmap_alloc(used, 40000, 40063, &p) == 0 && p == 40003,
       "a global port freed becomes available to the tunnel window", NULL);
}

static void test_boundaries(void)
{
    u16 p;

    reset_used();
    ok(tbt_portmap_alloc(used, 1, 1, &p) == 0 && p == 1,
       "port 1 allocates", NULL);
    ok(tbt_portmap_alloc(used, 1, 1, &p) == -ENOSPC,
       "port 1 exhausts after one allocation", NULL);
    tbt_portmap_free(used, 1);
    ok(tbt_portmap_alloc(used, 65535, 65535, &p) == 0 && p == 65535,
       "port 65535 allocates", NULL);
    ok(tbt_portmap_alloc(used, 65535, 65535, &p) == -ENOSPC,
       "port 65535 exhausts after one allocation", NULL);
    tbt_portmap_free(used, 65535);
    ok(tbt_portmap_alloc(used, 65534, 65535, &p) == 0 && p == 65534,
       "top two ports: first allocation", NULL);
    ok(tbt_portmap_alloc(used, 65534, 65535, &p) == 0 && p == 65535,
       "top two ports: second allocation", NULL);
    ok(tbt_portmap_alloc(used, 65534, 65535, &p) == -ENOSPC,
       "top two ports: third allocation fails", NULL);

    ok(tbt_portmap_alloc(NULL, 1, 1, &p) == -EINVAL,
       "NULL bitmap is invalid", NULL);
    ok(tbt_portmap_alloc(used, 0, 1, &p) == -EINVAL,
       "block starting at 0 is invalid", NULL);
    ok(tbt_portmap_alloc(used, 2, 1, &p) == -EINVAL,
       "reversed block is invalid", NULL);
    ok(tbt_portmap_alloc(used, 0, 0, &p) == -EINVAL,
       "0-0 is not an allocatable block", NULL);
    ok(tbt_portmap_alloc(used, 1, 1, NULL) == -EINVAL,
       "NULL port pointer is invalid", NULL);
    tbt_portmap_free(NULL, 1);
    tbt_portmap_free(used, 0);
}

static void test_parse_portblock(void)
{
    struct tbt_parse_error err = { NULL, NULL };
    struct tbt_cmd cmd;

    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=1 portblock=40000-40004",
                  &cmd, &err) == 0 &&
           cmd.cfg.portblock_lo == 40000 && cmd.cfg.portblock_hi == 40004,
       "near portblock with room for n + m parses", NULL);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=1 portblock=0-0",
                  &cmd, &err) == 0 &&
           !cmd.cfg.portblock_lo && !cmd.cfg.portblock_hi,
       "portblock=0-0 parses as no per-tunnel block", NULL);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=1 portblock=40000-40063",
                  &cmd, &err) == 0,
       "a larger near block parses", NULL);

    /* A near block that cannot hold every slot is refused with the key named. */
    err.key = NULL;
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=1 portblock=40000-40003",
                  &cmd, &err) == TBT_PARSE_EINVAL &&
           err.key && !strcmp(err.key, "portblock"),
       "near block smaller than n + m is refused", err.why);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=1 m=0 portblock=65535-65535",
                  &cmd, &err) == 0,
       "near block at port 65535 parses", NULL);

    /* FEC counts carrier slots, not n + m. */
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=4 fec=on mode=grouped w=8 carriers=16"
                  " portblock=40000-40015",
                  &cmd, &err) == 0,
       "FEC block holding every carrier slot parses", NULL);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " n=4 m=4 fec=on mode=grouped w=8 carriers=16"
                  " portblock=40000-40014",
                  &cmd, &err) == TBT_PARSE_EINVAL,
       "FEC block one port short is refused", NULL);

    /* A far tunnel never dials, so its portblock= is recorded but not sized. */
    ok(parse_line("tunnel 2 role=far listen=10.0.0.1:1 target=10.0.0.2:2"
                  " n=4 m=1 portblock=40000-40000",
                  &cmd, &err) == 0 &&
           cmd.cfg.portblock_lo == 40000 && cmd.cfg.portblock_hi == 40000,
       "far portblock is recorded without a slot check", NULL);

    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " portblock=500-400",
                  &cmd, &err) == TBT_PARSE_EINVAL,
       "reversed portblock is refused", NULL);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " portblock=0-1",
                  &cmd, &err) == TBT_PARSE_EINVAL,
       "portblock starting at 0 is refused", NULL);
    ok(parse_line("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                  " portblock=65536-65536",
                  &cmd, &err) == TBT_PARSE_EINVAL,
       "portblock above 65535 is refused", NULL);
}

static void test_parse_global(void)
{
    struct tbt_parse_error err = { NULL, NULL };
    u16 lo = 0, hi = 0;

    ok(tbt_parse_ports("ports 30000-34095\n", &lo, &hi, &err) == 0 &&
           lo == 30000 && hi == 34095,
       "global ports 30000-34095 parses", NULL);
    ok(tbt_parse_ports("ports 0-0\n", &lo, &hi, &err) == 0 && !lo && !hi,
       "ports 0-0 is a no-op", NULL);
    ok(tbt_parse_ports("ports 30000-29999\n", &lo, &hi, &err) != 0,
       "reversed global block is refused", NULL);
    ok(tbt_parse_ports("ports 0-30000\n", &lo, &hi, &err) != 0,
       "global block starting at 0 is refused", NULL);
    ok(tbt_parse_ports("ports 30000\n", &lo, &hi, &err) != 0,
       "single-port global directive is refused", NULL);
}

int main(void)
{
    test_words_and_fits();
    test_window();
    test_alloc_256();
    test_overlap();
    test_boundaries();
    test_parse_portblock();
    test_parse_global();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
