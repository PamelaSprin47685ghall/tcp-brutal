/* test_mem_budget - the 256-carrier memory ledger, checked against the
 * constants the module really uses.
 *
 * The module cannot be loaded here, so this test cannot observe
 * /proc/slabinfo. It checks the two facts the optimisations rest on:
 *
 *   1. Every FEC plane above TBT_FEC_VMALLOC_MIN is routed to vmalloc, and
 *      every smaller one stays on kvzalloc.
 *   2. A raw carrier (profile 3) never allocates the three HTTP buffers, so
 *      its per-carrier slab is smaller by exactly those three objects.
 *
 * slab_bytes() models the target kernel: kmalloc caches stop at 8 KiB, and a
 * larger request becomes a power-of-two page block. That is the accounting
 * the 0.3.0 A-B pass used (tests/throughput/results/.../memory-accounting.txt), so the
 * numbers here are comparable with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_http.h"

#define CARRIERS 256u
#define PAGE 4096u
#define KMALLOC_MAX_CACHE 8192u

static int failures;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        if (!(cond))                                                       \
        {                                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* Bytes a kmalloc() of n occupies: a cache object up to 8 KiB, otherwise a
 * whole power-of-two page block. */
static size_t slab_bytes(size_t n)
{
    static const size_t cache[] = {
        8, 16, 32, 64, 96, 128, 192, 256,
        512, 1024, 2048, 4096, 8192,
    };
    size_t i, pages, block;

    if (n <= KMALLOC_MAX_CACHE)
    {
        for (i = 0; i < sizeof(cache) / sizeof(cache[0]); i++)
            if (n <= cache[i])
                return cache[i];
    }
    pages = (n + PAGE - 1) / PAGE;
    block = 1;
    while (block < pages)
        block <<= 1;
    return block * PAGE;
}

static size_t high_order_bytes(size_t n)
{
    return n > KMALLOC_MAX_CACHE ? slab_bytes(n) : 0;
}

static void test_size_classes(void)
{
    /* Pin the classes the ledger quotes. If a constant moves, the table and
     * the module move together. */
    CHECK(slab_bytes(TBT_HTTP_FEC_FRAME_MAX) == 2048);
    CHECK(slab_bytes(TBT_HTTP_FRAME_MAX) == 16384);
    CHECK(slab_bytes(TBT_PARSER_BUF) == 16384);
    CHECK(slab_bytes(sizeof(struct tbt_http_rx)) == 16384);
    CHECK(slab_bytes(TBT_HTTP_HDR_MAX) == 4096);
    CHECK(slab_bytes(TBT_HTTP_FEC_FRAME_MAX * TBT_FEC_BATCH_MAX) == 4096);
    CHECK(slab_bytes(TBT_HTTP_CTL_FRAME_MAX) == 512);
    CHECK(TBT_HTTP_CTL_FRAME_MAX < TBT_HTTP_FRAME_MAX);
}

static void test_vmalloc_policy(void)
{
    CHECK(!tbt_fec_plane_uses_vmalloc(8240));
    CHECK(!tbt_fec_plane_uses_vmalloc(TBT_FEC_VMALLOC_MIN - 1));
    CHECK(tbt_fec_plane_uses_vmalloc(TBT_FEC_VMALLOC_MIN));
    CHECK(tbt_fec_plane_uses_vmalloc(256u * 1024u));
    CHECK(tbt_fec_plane_uses_vmalloc(2u * 1024u * 1024u));
}

static void test_vzalloc_is_zeroed(void)
{
    size_t len = 70000;
    unsigned char *p = tbt_fec_vzalloc(len);
    size_t i;
    int zero = 1;

    CHECK(p != NULL);
    if (!p)
        return;
    for (i = 0; i < len; i++)
        zero &= p[i] == 0;
    CHECK(zero);
    free(p);
}

static size_t http_tx_request(bool fec)
{
    return fec ? TBT_HTTP_CTL_FRAME_MAX : TBT_HTTP_FRAME_MAX;
}

static size_t carrier_request(bool http, bool fec)
{
    size_t n = TBT_HTTP_FEC_FRAME_MAX * TBT_FEC_BATCH_MAX + /* part */
               TBT_PARSER_BUF;                 /* parser, every profile */

    if (http)
        n += http_tx_request(fec) + TBT_HTTP_HDR_MAX +
             sizeof(struct tbt_http_rx);
    return n;
}

static size_t carrier_slab(bool http, bool fec)
{
    size_t n = slab_bytes(TBT_HTTP_FEC_FRAME_MAX * TBT_FEC_BATCH_MAX) +
               slab_bytes(TBT_PARSER_BUF);

    if (http)
        n += slab_bytes(http_tx_request(fec)) + slab_bytes(TBT_HTTP_HDR_MAX) +
             slab_bytes(sizeof(struct tbt_http_rx));
    return n;
}

static size_t carrier_high_order(bool http, bool fec)
{
    size_t n = high_order_bytes(TBT_HTTP_FEC_FRAME_MAX * TBT_FEC_BATCH_MAX) +
               high_order_bytes(TBT_PARSER_BUF);

    if (http)
        n += high_order_bytes(http_tx_request(fec)) +
             high_order_bytes(TBT_HTTP_HDR_MAX) +
             high_order_bytes(sizeof(struct tbt_http_rx));
    return n;
}

static void test_carrier_ledger(void)
{
    size_t base = carrier_slab(true, false);   /* 5192dd0: full ARQ buffer */
    size_t fec = carrier_slab(true, true);     /* FEC control-only encoder */
    size_t raw = carrier_slab(false, true);    /* raw container */
    size_t base_ho = carrier_high_order(true, false);
    size_t fec_ho = carrier_high_order(true, true);
    size_t raw_ho = carrier_high_order(false, true);
    size_t fec_saved = (size_t)CARRIERS * (base - fec);
    size_t raw_saved = (size_t)CARRIERS * (base - raw);
    size_t fec_saved_ho = (size_t)CARRIERS * (base_ho - fec_ho);
    size_t raw_saved_ho = (size_t)CARRIERS * (base_ho - raw_ho);

    CHECK(base == 57344u);
    CHECK(fec == 41472u);
    CHECK(raw == 20480u);
    CHECK(slab_bytes(TBT_HTTP_CTL_FRAME_MAX) == 512);
    CHECK(fec_saved == 256u * (16384u - 512u));
    CHECK(raw_saved == 256u * (16384u + 4096u + 16384u));
    /* The high-order block disappears entirely; the 512 B replacement is a
     * normal slab object, so the two totals differ by 256 x 512. */
    CHECK(fec_saved_ho == 256u * 16384u);
    CHECK(raw_saved_ho == 256u * (16384u + 16384u));
    CHECK(carrier_request(true, true) < carrier_request(true, false));
    printf("carrier: base %.1f KiB, http+fec %.1f KiB (-%.2f MiB), "
           "raw %.1f KiB (-%.2f MiB) x %u\n",
           base / 1024.0, fec / 1024.0, fec_saved / 1048576.0,
           raw / 1024.0, raw_saved / 1048576.0, CARRIERS);
}

/* One super-block is groups * (n + m) * S. Near: 8 groups of 8+24 shares.
 * Far: 1 group of 128+128 shares. At S=1024 both are 256 KiB per
 * super-block; the arena keeps 66 logical slots but allocates each block
 * plane on demand. */
static void test_fec_arena(void)
{
    const size_t s = TBT_FEC_S;
    size_t near_per = 8u * 32u * s;
    size_t far_per = 1u * 256u * s;
    size_t near_buf = 32u * s;
    size_t far_buf = 256u * s;
    size_t near_blocks = 8u * 66u; /* groups x slots */
    size_t far_blocks = 1u * 66u;
    size_t ring = 256u * 1024u;

    CHECK(near_per == 256u * 1024u);
    CHECK(far_per == 256u * 1024u);
    CHECK(near_buf == 32u * 1024u);
    CHECK(far_buf == 256u * 1024u);
    CHECK(tbt_fec_plane_uses_vmalloc(near_buf));
    CHECK(tbt_fec_plane_uses_vmalloc(far_buf));
    CHECK(tbt_fec_plane_uses_vmalloc(ring));
    CHECK(tbt_fec_plane_uses_vmalloc((size_t)near_blocks * near_buf));
    CHECK(tbt_fec_plane_uses_vmalloc((size_t)far_blocks * far_buf));
    printf("arena: near %zu x %zu KiB, far %zu x %zu KiB (vmalloc)\n",
           near_blocks, near_buf / 1024u, far_blocks, far_buf / 1024u);
}

/* A 256-carrier shape lands on K=66: a 64-block sliding window plus two
 * blocks of slack. Only descriptors and an 8-block hot pool are resident;
 * the ring is allocated by the first packetizer pass. */
static void test_replay_ledger(void)
{
    u16 k = tbt_fec_replay_k_for(1, 128, 128, TBT_FEC_S);
    size_t bytes;

    CHECK(k == 66);
    bytes = tbt_fec_replay_bytes(1, 128, 128, TBT_FEC_S, k);
    CHECK(bytes > 2u * 1024u * 1024u);
    CHECK(bytes <= TBT_FEC_REPLAY_BUDGET);
    CHECK(tbt_fec_plane_uses_vmalloc(bytes));
    /* The same carrier count in 8 groups has the same slot total; only the
     * one block-generation word per (slot, group) grows. */
    CHECK(tbt_fec_replay_bytes(8, 8, 24, TBT_FEC_S, k) ==
          bytes + (size_t)(k * 7) * sizeof(u64));
    printf("replay: K=%u, %.2f MiB resident (8 blocks + descriptors)\n",
           k, bytes / 1048576.0);
}

int main(void)
{
    test_size_classes();
    test_vmalloc_policy();
    test_vzalloc_is_zeroed();
    test_carrier_ledger();
    test_fec_arena();
    test_replay_ledger();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_mem_budget: PASS\n");
    return 0;
}
