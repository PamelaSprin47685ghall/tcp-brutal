/* test_portmap.c - host conformance test for the global carrier port block.
 *
 * Links tbtun/tbt_portmap.c and tbtun/tbt_parse.c, the same code the module
 * uses. No root, no module, no kernel headers.
 *
 * The production shape is 256 carriers. A 64-port default would fail at the
 * 17th bind, so this test proves that a 4096-port block really hands out 256
 * distinct ports, exhausts, and reuses freed ports. It also pins the
 * `ports lo-hi` grammar the deployment script writes.
 */
#include <stdio.h>
#include <string.h>

#include "tbt_parse.h"
#include "tbt_portmap.h"

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

static void test_parse_ports(void)
{
    struct tbt_parse_error e = { NULL, NULL };
    u16 lo = 0, hi = 0;

    ok(tbt_parse_ports("ports 30000-34095\n", &lo, &hi, &e) == 0 && lo == 30000 && hi == 34095,
       "ports 30000-34095 parses", NULL);
    ok(hi - lo + 1 == 4096, "4096 ports", NULL);
    ok(tbt_parse_ports("ports 30000-34095\n", &lo, &hi, &e) == 0 && lo == 30000 &&
           tbt_portmap_words(lo, hi) == TBT_PORTMAP_MAX / 64,
       "4096 bits is TBT_PORTMAP_MAX/64 words", NULL);
    ok(tbt_parse_ports("ports 0-0\n", &lo, &hi, &e) == 0 && !lo && !hi,
       "ports 0-0 is a no-op", NULL);
    ok(tbt_parse_ports("ports 30000-29999\n", &lo, &hi, &e) != 0, "reversed block refused", NULL);
    ok(tbt_parse_ports("ports 0-30000\n", &lo, &hi, &e) != 0, "block starting at 0 refused", NULL);
    ok(tbt_parse_ports("ports 30000\n", &lo, &hi, &e) != 0, "single port refused", NULL);
}

static void test_alloc_256(void)
{
    unsigned long used[64];
    u16 lo = 30000, hi = 34095;
    u16 port[256];
    unsigned int i;
    int j;

    memset(used, 0, sizeof(used));
    for (i = 0; i < 256; i++)
        ok(tbt_portmap_alloc(used, lo, hi, &port[i]) == 0, "256th port allocates", NULL);

    /* All distinct, all in range, and the low 256 are exactly lo..lo+255. */
    for (i = 0; i < 256; i++)
    {
        ok(port[i] >= lo && port[i] <= hi, "port in range", NULL);
        ok(port[i] == (u16)(lo + i), "lowest free first", NULL);
        for (j = 0; j < (int)i; j++)
            ok(port[i] != port[j], "ports are distinct", NULL);
    }

    /* A 4096-port block has room for all of them. */
    for (i = 256; i < 4096; i++)
    {
        u16 p;

        if (tbt_portmap_alloc(used, lo, hi, &p))
            break;
    }
    ok(i == 4096, "all 4096 ports allocate", NULL);
    {
        u16 p;

        ok(tbt_portmap_alloc(used, lo, hi, &p) == -ENOSPC, "exhausted block reports ENOSPC",
           NULL);
    }

    /* Free the 256 carrier ports and hand them out again, in the same order. */
    for (i = 0; i < 256; i++)
        tbt_portmap_free(used, lo, hi, port[i]);
    for (i = 0; i < 256; i++)
    {
        u16 p;

        ok(tbt_portmap_alloc(used, lo, hi, &p) == 0 && p == port[i],
           "freed ports are reused lowest-first", NULL);
    }

    /* Out-of-block and double free are no-ops, not corruption. */
    tbt_portmap_free(used, lo, hi, 29999);
    tbt_portmap_free(used, lo, hi, 34096);
    tbt_portmap_free(used, lo, hi, port[0]);
    tbt_portmap_free(used, lo, hi, port[0]);
    {
        u16 p;

        ok(tbt_portmap_alloc(used, lo, hi, &p) == 0 && p == port[0],
           "double free did not lose the port", NULL);
    }
}

static void test_alloc_1024(void)
{
    unsigned long used[16];
    u16 lo = 40000, hi = 41023;
    u16 p;
    unsigned int i;

    memset(used, 0, sizeof(used));
    ok(tbt_portmap_words(lo, hi) == 16, "1024 bits is 16 words", NULL);
    for (i = 0; i < 256; i++)
        ok(tbt_portmap_alloc(used, lo, hi, &p) == 0, "1024 block: 256 allocs", NULL);
    ok(p == (u16)(lo + 255), "1024 block: last port", NULL);
    for (i = 0; i < 256; i++)
        tbt_portmap_free(used, lo, hi, (u16)(lo + i));
    ok(tbt_portmap_alloc(used, lo, hi, &p) == 0 && p == lo, "1024 block: reuse", NULL);
}

static void test_alloc_64(void)
{
    unsigned long used[1];
    u16 lo = 40000, hi = 40063;
    u16 p;
    unsigned int i;

    memset(used, 0, sizeof(used));
    ok(tbt_portmap_words(lo, hi) == 1, "64 bits is 1 word", NULL);
    for (i = 0; i < 64; i++)
        ok(tbt_portmap_alloc(used, lo, hi, &p) == 0, "64 block: alloc", NULL);
    ok(tbt_portmap_alloc(used, lo, hi, &p) == -ENOSPC, "64 block: 65th fails", NULL);
}

int main(void)
{
    test_parse_ports();
    test_alloc_256();
    test_alloc_1024();
    test_alloc_64();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
