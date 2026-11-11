/* bench_sclc_build.c - host cost of the production SC-LDPC constructors.
 *
 * WHAT THIS MEASURES
 *   The two codec builds the mode=3 data plane performs when a session
 *   starts or a shape switch lands:
 *
 *     downlink  tbt_fec_sclc_new() for a DOWN shape: one binary context,
 *               n + m = 256 shares, block 256, L 8, share 8192 bytes.
 *     uplink    tbt_fec_sclc_new() for an UP shape: eight GF(16) contexts,
 *               n + m = 32 shares, block 128, L 16, share 8192 bytes.
 *
 *   tbt_fec_sclc_new() itself lives in the kernel-only tbt_fec_data.c.
 *   The bench calls the host-buildable tail it dispatches to,
 *   tbt_fec_grouped_new_ops(), with exactly the (ops, n, m, k, w, s, d,
 *   nonce) that tbt_fec_sclc_new() derives for the same shape.  The DOWN
 *   branch passes w = block and d = 0; the UP branch passes k = block,
 *   w = L and d = rate_256.  The shape's L is not a DOWN codec parameter:
 *   the binary graph is fixed at L = 8.
 *
 * METHOD
 *   clock_gettime(CLOCK_MONOTONIC) around new(), free() outside the timer.
 *   Eight seeds x 256 builds per point after 16 warmup builds.  Samples
 *   are sorted and p50/p99 are nearest-rank.  Memory is the live heap
 *   delta of one held codec (glibc mallinfo2 uordblks + hblkhd), so it
 *   includes the grouped container and the allocator's chunk rounding.
 *   For the uplink the per-codec number also comes from the codec's own
 *   tbt_sclc_uplink_mem() helper.
 *
 *   make host-bench-sclc-build
 *   build/bin/bench_sclc_build
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ) && \
    __GLIBC_PREREQ(2, 33)
#define HAVE_MALLINFO2 1
#endif

#ifdef HAVE_MALLINFO2
#include <malloc.h>
#endif

#include "tbt_fec.h"
#include "tbt_fec_sclc.h"
#include "tbt_sclc_uplink.h"

#define ITERS 256
#define SEEDS 8
#define WARMUP 16
#define GATE_US 2000.0

static const u64 seeds[SEEDS] = {
    0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull,
    0x94D049BB133111EBull, 0x2545F4914F6CDD1Dull,
    0x0123456789ABCDEFull, 0xFEDCBA9876543210ull,
    0xA5A5A5A55A5A5A5Aull, 0x5A5A5A5AA5A5A5A5ull,
};

struct stats
{
    double p50;
    double p99;
    double max;
    size_t count;
};

typedef void *(*build_fn)(u16 n, u64 seed);
typedef void (*free_fn)(void *ctx);

static u64 now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void grouped_free(void *ctx)
{
    tbt_fec_grouped_free(ctx);
}

static void *down_build(u16 n, u64 seed)
{
    u16 m = (u16)(256u - n);
    u16 rate = (u16)((u32)n * 1000u / 256u);

    return tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc, 1, n, m, rate,
                                   TBT_FEC_SCLC_BLOCK_DOWN, TBT_FEC_S, 0,
                                   seed);
}

static void *up_build(u16 n, u64 seed)
{
    u16 m = (u16)(32u - n);
    u16 rate = (u16)((u32)n * 256u / 32u);

    return tbt_fec_grouped_new_ops(&tbt_fec_ops_sclc_gf16, 8, n, m,
                                   TBT_FEC_SCLC_BLOCK_UP, TBT_FEC_SCLC_L_UP,
                                   TBT_FEC_S, rate, seed);
}

static void up_params(struct tbt_sclc_params *par, u16 n, u64 seed)
{
    memset(par, 0, sizeof(*par));
    par->n = n;
    par->m = (u16)(32u - n);
    par->block = TBT_FEC_SCLC_BLOCK_UP;
    par->L = TBT_FEC_SCLC_L_UP;
    par->rate_num = (u16)((u32)n * 8u);
    par->rate_den = 256;
    par->max_core = 256;
    par->mapping = TBT_SCLC_MAP_BITPLANE64;
    par->seed = seed;
}

/* The direct multi-group API: one object holds all eight contexts.  It is
 * not the tbt_fec_sclc_new() path, but it is the other way to build the
 * same eight codecs and serves as a cross-check. */
static void *up_direct_build(u16 n, u64 seed)
{
    struct tbt_sclc_params par;

    up_params(&par, n, seed);
    return tbt_sclc_uplink_new(8, &par);
}

static void up_free(void *ctx)
{
    tbt_sclc_uplink_free(ctx);
}

static long live_heap(void)
{
#ifdef HAVE_MALLINFO2
    struct mallinfo2 mi = mallinfo2();

    return (long)(mi.uordblks + mi.hblkhd);
#else
    return -1;
#endif
}

static long measure_mem(build_fn build, free_fn release, u16 n)
{
    long worst = -1;
    size_t s;

    /* The downlink encoder plan's dcs depends on the seed, so report the
     * largest live footprint over the same eight seeds the timing uses. */
    for (s = 0; s < SEEDS; s++)
    {
        void *ctx;
        long before, after, delta;

        /* One build/free warms the allocator's bins for this size mix. */
        ctx = build(n, seeds[s]);
        if (!ctx)
            return -1;
        release(ctx);

        before = live_heap();
        ctx = build(n, seeds[s]);
        if (!ctx)
            return -1;
        after = live_heap();
        release(ctx);
        delta = after - before;
        if (delta > worst)
            worst = delta;
    }
    return worst;
}

/* Per-codec memory as the uplink codec accounts it.  One group is the
 * production object: tbt_fec_sclc_new() builds eight of them. */
static long up_codec_mem(u16 n, u32 *helper)
{
    long worst = -1;
    u32 worst_helper = 0;
    size_t s;

    for (s = 0; s < SEEDS; s++)
    {
        struct tbt_sclc_params par;
        struct tbt_sclc_uplink *u;
        long before, after, delta;

        up_params(&par, n, seeds[s]);
        u = tbt_sclc_uplink_new(1, &par);
        if (!u)
            return -1;
        tbt_sclc_uplink_free(u);

        before = live_heap();
        u = tbt_sclc_uplink_new(1, &par);
        if (!u)
            return -1;
        after = live_heap();
        delta = after - before;
        if (delta > worst)
        {
            worst = delta;
            worst_helper = tbt_sclc_uplink_mem(u);
        }
        tbt_sclc_uplink_free(u);
    }
    *helper = worst_helper;
    return worst;
}

static long up_direct_helper(u16 n, u32 *helper)
{
    u32 worst = 0;
    size_t s;

    for (s = 0; s < SEEDS; s++)
    {
        struct tbt_sclc_params par;
        struct tbt_sclc_uplink *u;
        u32 mem;

        up_params(&par, n, seeds[s]);
        u = tbt_sclc_uplink_new(8, &par);
        if (!u)
            return -1;
        mem = tbt_sclc_uplink_mem(u);
        tbt_sclc_uplink_free(u);
        if (mem > worst)
            worst = mem;
    }
    *helper = worst;
    return (long)worst;
}

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a;
    u64 y = *(const u64 *)b;

    return x < y ? -1 : x > y;
}

static size_t rank(size_t count, unsigned pct)
{
    size_t r = (count * pct + 99u) / 100u;

    if (!r)
        r = 1;
    return r - 1;
}

static int run_point(build_fn build, free_fn release, u16 n, u64 *samples,
                     struct stats *st)
{
    size_t i, s, count = 0;

    for (i = 0; i < WARMUP; i++)
    {
        void *ctx = build(n, seeds[i % SEEDS]);

        if (!ctx)
            return -1;
        release(ctx);
    }

    for (s = 0; s < SEEDS; s++)
    {
        for (i = 0; i < ITERS; i++)
        {
            u64 t0, t1;
            void *ctx;

            t0 = now_ns();
            ctx = build(n, seeds[s]);
            t1 = now_ns();
            if (!ctx)
                return -1;
            release(ctx);
            samples[count++] = t1 - t0;
        }
    }

    qsort(samples, count, sizeof(*samples), cmp_u64);
    st->count = count;
    st->p50 = (double)samples[rank(count, 50)] / 1000.0;
    st->p99 = (double)samples[rank(count, 99)] / 1000.0;
    st->max = (double)samples[count - 1] / 1000.0;
    return 0;
}

static void print_row(const char *dir, u16 n, u16 m, u16 groups, u16 block,
                      u16 L, const struct stats *st, long codec_mem,
                      long total_mem, long helper_mem)
{
    printf("%-6s %3u %3u %3u %5u %2u %5zu %8.1f %8.1f %8.1f %9ld %9ld",
           dir, n, m, groups, block, L, st->count, st->p50, st->p99, st->max,
           codec_mem, total_mem);
    if (helper_mem >= 0)
        printf(" %9ld", helper_mem);
    else
        printf(" %9s", "-");
    printf("\n");
}

static void print_rejected(const char *dir, u16 n, u16 m, u16 groups,
                           u16 block, u16 L)
{
    printf("%-6s %3u %3u %3u %5u %2u  build rejected by the constructor\n",
           dir, n, m, groups, block, L);
}

int main(void)
{
    static const u16 down_n[] = {64, 76, 102, 128, 151, 178};
    static const u16 up_n[] = {8, 16, 20, 24};
    u64 *samples;
    struct stats st;
    double worst_p99 = 0.0;
    const char *worst_where = "none";
    int failed = 0;
    int rejected = 0;
    size_t i;

    samples = malloc((size_t)SEEDS * ITERS * sizeof(*samples));
    if (!samples)
    {
        fprintf(stderr, "bench: sample allocation failed\n");
        return 1;
    }

    printf("SC-LDPC codec build cost, CLOCK_MONOTONIC, %d seeds x %d builds"
           " after %d warmup builds\n", SEEDS, ITERS, WARMUP);
    printf("gate: p99 <= %.0f us per codec build\n\n", GATE_US);
    printf("%-6s %3s %3s %3s %5s %2s %5s %8s %8s %8s %9s %9s %9s\n",
           "dir", "n", "m", "grp", "block", "L", "iters", "p50_us",
           "p99_us", "max_us", "codec_B", "total_B", "helper_B");
    printf("%-6s %3s %3s %3s %5s %2s %5s %8s %8s %8s %9s %9s %9s\n",
           "down", "", "", "", "", "", "", "", "", "", "measured",
           "measured", "n/a");
    printf("%-6s %3s %3s %3s %5s %2s %5s %8s %8s %8s %9s %9s %9s\n",
           "up", "", "", "", "", "", "", "", "", "", "measured",
           "8 groups", "tbt_sclc_uplink_mem");
    printf("%-6s %3s %3s %3s %5s %2s %5s %8s %8s %8s %9s %9s %9s\n",
           "up8", "", "", "", "", "", "", "", "", "", "helper/8",
           "measured", "tbt_sclc_uplink_mem");

    for (i = 0; i < sizeof(down_n) / sizeof(down_n[0]); i++)
    {
        u16 n = down_n[i];
        u16 m = (u16)(256u - n);
        long codec_mem, total_mem;

        if (run_point(down_build, grouped_free, n, samples, &st))
        {
            print_rejected("down", n, m, 1, TBT_FEC_SCLC_BLOCK_DOWN,
                           TBT_FEC_SCLC_L_DOWN);
            rejected++;
            continue;
        }
        codec_mem = measure_mem(down_build, grouped_free, n);
        total_mem = codec_mem;
        print_row("down", n, m, 1, TBT_FEC_SCLC_BLOCK_DOWN,
                  TBT_FEC_SCLC_L_DOWN, &st, codec_mem, total_mem, -1);
        if (st.p99 > GATE_US)
        {
            failed = 1;
            if (st.p99 > worst_p99)
            {
                worst_p99 = st.p99;
                worst_where = "downlink";
            }
        }
        else if (st.p99 > worst_p99)
        {
            worst_p99 = st.p99;
            worst_where = "downlink";
        }
    }

    for (i = 0; i < sizeof(up_n) / sizeof(up_n[0]); i++)
    {
        u16 n = up_n[i];
        u16 m = (u16)(32u - n);
        long codec_mem, total_mem, helper_mem = -1;
        u32 helper = 0;

        if (run_point(up_build, grouped_free, n, samples, &st))
        {
            print_rejected("up", n, m, 8, TBT_FEC_SCLC_BLOCK_UP,
                           TBT_FEC_SCLC_L_UP);
            rejected++;
            continue;
        }
        codec_mem = up_codec_mem(n, &helper);
        if (codec_mem >= 0)
            helper_mem = (long)helper;
        total_mem = measure_mem(up_build, grouped_free, n);
        print_row("up", n, m, 8, TBT_FEC_SCLC_BLOCK_UP, TBT_FEC_SCLC_L_UP,
                  &st, codec_mem, total_mem, helper_mem);
        if (st.p99 > GATE_US)
        {
            failed = 1;
            if (st.p99 > worst_p99)
            {
                worst_p99 = st.p99;
                worst_where = "uplink 8 groups";
            }
        }
        else if (st.p99 > worst_p99)
        {
            worst_p99 = st.p99;
            worst_where = "uplink 8 groups";
        }
    }

    for (i = 0; i < sizeof(up_n) / sizeof(up_n[0]); i++)
    {
        u16 n = up_n[i];
        u16 m = (u16)(32u - n);
        long total_mem, helper_mem;
        u32 helper = 0;

        if (run_point(up_direct_build, up_free, n, samples, &st))
        {
            print_rejected("up8", n, m, 8, TBT_FEC_SCLC_BLOCK_UP,
                           TBT_FEC_SCLC_L_UP);
            rejected++;
            continue;
        }
        helper_mem = up_direct_helper(n, &helper);
        total_mem = measure_mem(up_direct_build, up_free, n);
        print_row("up8", n, m, 8, TBT_FEC_SCLC_BLOCK_UP, TBT_FEC_SCLC_L_UP,
                  &st, helper_mem / 8, total_mem, helper_mem);
        if (st.p99 > GATE_US)
        {
            failed = 1;
            if (st.p99 > worst_p99)
            {
                worst_p99 = st.p99;
                worst_where = "uplink direct 8 groups";
            }
        }
        else if (st.p99 > worst_p99)
        {
            worst_p99 = st.p99;
            worst_where = "uplink direct 8 groups";
        }
    }

    printf("\n");
    if (rejected)
        printf("note: %d requested point(s) cannot be built by the codec;"
               " no p99 exists for them\n", rejected);
    printf("GATE p99 <= %.0f us: %s (worst %s p99 %.1f us)\n",
           GATE_US, failed ? "FAIL" : "PASS", worst_where, worst_p99);
    free(samples);
    return failed ? 1 : 0;
}
