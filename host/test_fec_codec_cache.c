/* test_fec_codec_cache.c - host test for the codec LRU.
 *
 * WHAT THIS IS
 *   The exact code from tbtun/tbt_fec_codec_cache.c, compiled for the host.
 *   It checks:
 *
 *     - capacity: down 4, up 2, init rejects 0 and more than the maximum;
 *     - hit and miss, and that the seed is part of the key;
 *     - LRU eviction, including a held reference that must not be evicted;
 *     - a failed build keeps the entry and the current codec stays usable;
 *     - a partial codec and a duplicate publish are freed, not leaked;
 *     - destroy() defers a codec with live references and frees the slot
 *       array after the last put();
 *     - the five counters.
 *
 *   Codecs are mock pointers: the cache only stores them and hands them to
 *   free_fn(). The allocator is malloc, so the slot array is real memory and
 *   the free callback can prove it is released exactly once.
 *
 *   make host-test
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec_codec_cache.h"

#define MOCK_FREED_MAX 16

struct mock
{
    unsigned allocs;
    unsigned slot_frees;
    unsigned codec_frees;
    unsigned fail_alloc;
    void *freed[MOCK_FREED_MAX];
    unsigned nfreed;
};

static int checks;
static int failures;

static void ok(bool cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", what);
}

static void eq_u32(u32 got, u32 want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %u want %u\n", what, got, want);
}

static void eq_u64(u64 got, u64 want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %llu want %llu\n", what, (unsigned long long)got,
           (unsigned long long)want);
}

static void eq_i32(int got, int want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %d want %d\n", what, got, want);
}

static void eq_ptr(const void *got, const void *want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %p want %p\n", what, got, want);
}

static void *mock_alloc(void *ctx, size_t bytes)
{
    struct mock *m = ctx;

    m->allocs++;
    if (m->fail_alloc)
        return NULL;
    return malloc(bytes);
}

static void mock_free(void *ctx, void *ptr,
                      enum tbt_fec_codec_cache_mem kind)
{
    struct mock *m = ctx;

    if (kind == TBT_FEC_CODEC_CACHE_MEM_SLOTS)
    {
        m->slot_frees++;
        free(ptr);
        return;
    }
    m->codec_frees++;
    if (m->nfreed < MOCK_FREED_MAX)
        m->freed[m->nfreed++] = ptr;
}

static bool codec_freed(const struct mock *m, const void *codec)
{
    unsigned i;

    for (i = 0; i < m->nfreed; i++)
        if (m->freed[i] == codec)
            return true;
    return false;
}

#define MOCK_CODEC(id) \
    ((struct tbt_fec_grouped *)(uintptr_t)(0x1000u + (unsigned)(id)))

static struct tbt_fec_codec_key make_key(u16 n, u16 m, u16 block, u16 L,
                                         u64 seed)
{
    struct tbt_fec_codec_key k;

    k.n = n;
    k.m = m;
    k.block = block;
    k.L = L;
    k.seed = seed;
    return k;
}

static void cache_init(struct tbt_fec_codec_cache *c, struct mock *m,
                       u32 capacity)
{
    memset(m, 0, sizeof(*m));
    memset(c, 0, sizeof(*c));
    eq_i32(tbt_fec_codec_cache_init(c, capacity, mock_alloc, mock_free, m), 0,
           "init");
}

static struct tbt_fec_codec_cache_stats stats_of(
    const struct tbt_fec_codec_cache *c)
{
    struct tbt_fec_codec_cache_stats s;

    tbt_fec_codec_cache_stats(c, &s);
    return s;
}

static void test_init_capacity(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_cache_stats s;
    struct mock m;

    eq_u32(TBT_FEC_CODEC_CACHE_DOWN, 4, "down capacity constant");
    eq_u32(TBT_FEC_CODEC_CACHE_UP, 2, "up capacity constant");
    eq_u32(TBT_FEC_CODEC_CACHE_MAX, 4, "maximum capacity constant");

    memset(&m, 0, sizeof(m));
    memset(&c, 0, sizeof(c));
    eq_i32(tbt_fec_codec_cache_init(&c, 4, mock_alloc, mock_free, &m), 0,
           "init down 4");
    eq_u32(c.capacity, 4, "capacity stored");
    eq_u32(c.used, 0, "cache starts empty");
    eq_u64(c.stats.hits, 0, "cache starts with no hits");
    ok(m.allocs == 1, "init allocates the slot array once");
    tbt_fec_codec_cache_stats(&c, NULL);
    tbt_fec_codec_cache_stats(NULL, &s);
    eq_u64(s.hits, 0, "NULL stats snapshot is zero");
    tbt_fec_codec_cache_destroy(&c);
    ok(m.slot_frees == 1, "destroy frees the slot array");
    tbt_fec_codec_cache_destroy(&c);
    ok(m.slot_frees == 1, "destroy is idempotent");

    memset(&m, 0, sizeof(m));
    memset(&c, 0, sizeof(c));
    eq_i32(tbt_fec_codec_cache_init(&c, 2, mock_alloc, mock_free, &m), 0,
           "init up 2");
    tbt_fec_codec_cache_destroy(&c);
    ok(m.slot_frees == 1, "up cache frees its slots");

    memset(&m, 0, sizeof(m));
    memset(&c, 0, sizeof(c));
    eq_i32(tbt_fec_codec_cache_init(&c, 0, mock_alloc, mock_free, &m),
           -EINVAL, "init rejects 0");
    eq_i32(tbt_fec_codec_cache_init(&c, 5, mock_alloc, mock_free, &m),
           -EINVAL, "init rejects 5");
    eq_i32(tbt_fec_codec_cache_init(NULL, 4, mock_alloc, mock_free, &m),
           -EINVAL, "init rejects NULL cache");
    eq_i32(tbt_fec_codec_cache_init(&c, 4, NULL, mock_free, &m), -EINVAL,
           "init rejects NULL alloc");
    eq_i32(tbt_fec_codec_cache_init(&c, 4, mock_alloc, NULL, &m), -EINVAL,
           "init rejects NULL free");

    memset(&m, 0, sizeof(m));
    m.fail_alloc = 1;
    memset(&c, 0, sizeof(c));
    eq_i32(tbt_fec_codec_cache_init(&c, 4, mock_alloc, mock_free, &m),
           -ENOMEM, "init reports allocation failure");
    ok(c.entry == NULL, "failed init leaves no slots");
}

static void test_hit_miss_seed(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *e, *e2;
    struct tbt_fec_codec_key k1, k1b, k2, k3;
    struct tbt_fec_codec_cache_stats s;
    struct mock m;
    bool ready = true;

    cache_init(&c, &m, 4);
    k1 = make_key(128, 128, 256, 8, 0x1111u);
    k1b = make_key(128, 128, 256, 8, 0x2222u);
    k2 = make_key(154, 102, 256, 8, 0x1111u);
    k3 = make_key(128, 128, 256, 16, 0x1111u);

    e = tbt_fec_codec_cache_get(&c, &k1, &ready);
    ok(e != NULL, "miss returns an entry");
    ok(!ready, "miss is not ready");
    eq_u32(e->refs, 1, "miss holds one reference");
    eq_ptr(e->codec, NULL, "miss has no codec");
    ok(e->ready == 0, "miss ready is 0");
    s = stats_of(&c);
    eq_u64(s.misses, 1, "miss counted");
    eq_u64(s.hits, 0, "no hit yet");

    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(1), true);
    eq_ptr(e->codec, MOCK_CODEC(1), "build publishes the codec");
    ok(e->ready == 1, "build marks ready");
    eq_u64(stats_of(&c).builds, 1, "build counted");
    tbt_fec_codec_cache_put(e);
    eq_u32(e->refs, 0, "put drops the reference");

    e = tbt_fec_codec_cache_get(&c, &k1, &ready);
    ok(e != NULL && ready, "hit is ready");
    eq_ptr(e->codec, MOCK_CODEC(1), "hit returns the codec");
    eq_u64(stats_of(&c).hits, 1, "hit counted");
    tbt_fec_codec_cache_put(e);

    /* The seed is part of the key. */
    e = tbt_fec_codec_cache_get(&c, &k1b, &ready);
    ok(e != NULL && !ready, "other seed is a miss");
    eq_u64(stats_of(&c).misses, 2, "seed miss counted");
    eq_u32(c.used, 2, "two entries after the seed miss");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(2), true);
    tbt_fec_codec_cache_put(e);

    /* A failed build keeps the entry and the current codec. */
    e = tbt_fec_codec_cache_get(&c, &k2, &ready);
    ok(e != NULL && !ready, "shape miss");
    tbt_fec_codec_cache_build_done(e, NULL, false);
    ok(e->ready == 0, "failed build stays not ready");
    ok(e->failed == 1, "failed build sets the flag");
    eq_u64(stats_of(&c).build_fail, 1, "failed build counted");
    eq_u32(c.used, 3, "failed entry stays in the cache");

    e2 = tbt_fec_codec_cache_get(&c, &k2, &ready);
    ok(e2 == e, "failed entry is reused");
    ok(!ready, "failed entry is not ready");
    eq_u32(e2->refs, 2, "failed entry takes a second reference");
    tbt_fec_codec_cache_put(e2);
    tbt_fec_codec_cache_put(e);

    e = tbt_fec_codec_cache_get(&c, &k1b, &ready);
    ok(e != NULL && ready, "the other seed still hits");
    eq_ptr(e->codec, MOCK_CODEC(2), "the other seed keeps its codec");
    tbt_fec_codec_cache_put(e);

    /* L is part of the key too. */
    e = tbt_fec_codec_cache_get(&c, &k3, &ready);
    ok(e != NULL && !ready, "different L is a miss");
    eq_u32(e->key.L, 16, "new entry keeps its L");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(3), true);
    tbt_fec_codec_cache_put(e);

    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 3, "destroy frees all published codecs");
    ok(m.slot_frees == 1, "destroy frees the slots");
}

static void test_lru_eviction(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *e;
    struct tbt_fec_codec_key ka, kb, kc;
    struct mock m;
    bool ready;

    cache_init(&c, &m, 2);
    ka = make_key(128, 128, 256, 8, 1);
    kb = make_key(128, 128, 256, 8, 2);
    kc = make_key(128, 128, 256, 8, 3);

    e = tbt_fec_codec_cache_get(&c, &ka, &ready);
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(10), true);
    tbt_fec_codec_cache_put(e);
    e = tbt_fec_codec_cache_get(&c, &kb, &ready);
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(11), true);
    tbt_fec_codec_cache_put(e);

    /* Touch A, so B is the least recently used entry. */
    e = tbt_fec_codec_cache_get(&c, &ka, &ready);
    ok(e != NULL && ready, "A hits before the eviction");
    tbt_fec_codec_cache_put(e);

    e = tbt_fec_codec_cache_get(&c, &kc, &ready);
    ok(e != NULL && !ready, "C reserves a slot");
    eq_u64(stats_of(&c).evictions, 1, "C evicts one entry");
    ok(codec_freed(&m, MOCK_CODEC(11)), "B is freed");
    ok(!codec_freed(&m, MOCK_CODEC(10)), "A is kept");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(12), true);
    tbt_fec_codec_cache_put(e);

    e = tbt_fec_codec_cache_get(&c, &ka, &ready);
    ok(e != NULL && ready, "A is still cached");
    eq_ptr(e->codec, MOCK_CODEC(10), "A keeps its codec");
    tbt_fec_codec_cache_put(e);

    /* A was touched above, so C is now the oldest entry. */
    e = tbt_fec_codec_cache_get(&c, &kb, &ready);
    ok(e != NULL && !ready, "B misses again");
    eq_u64(stats_of(&c).evictions, 2, "B evicts C");
    ok(codec_freed(&m, MOCK_CODEC(12)), "C is freed");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(13), true);
    tbt_fec_codec_cache_put(e);

    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 4, "each codec is freed exactly once");
    ok(m.slot_frees == 1, "slots are freed once");
}

static void test_ref_defer(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *ea, *eb, *ec, *ed, *ee;
    struct tbt_fec_codec_key ka, kb, kc, kd, ke;
    struct mock m;
    bool ready;

    cache_init(&c, &m, 2);
    ka = make_key(128, 128, 256, 8, 1);
    kb = make_key(128, 128, 256, 8, 2);
    kc = make_key(128, 128, 256, 8, 3);
    kd = make_key(128, 128, 256, 8, 4);
    ke = make_key(128, 128, 256, 8, 5);

    ea = tbt_fec_codec_cache_get(&c, &ka, &ready);
    tbt_fec_codec_cache_build_done(ea, MOCK_CODEC(20), true);
    /* Keep A's reference: it is the in-use current codec. */

    eb = tbt_fec_codec_cache_get(&c, &kb, &ready);
    tbt_fec_codec_cache_build_done(eb, MOCK_CODEC(21), true);
    tbt_fec_codec_cache_put(eb);

    /* The cache is full. A has a reference, so B is the only victim. */
    ec = tbt_fec_codec_cache_get(&c, &kc, &ready);
    ok(ec != NULL && !ready, "C reserves the B slot");
    eq_u64(stats_of(&c).evictions, 1, "C evicts one entry");
    ok(codec_freed(&m, MOCK_CODEC(21)), "B is freed");
    ok(!codec_freed(&m, MOCK_CODEC(20)), "held A is not freed");
    tbt_fec_codec_cache_build_done(ec, MOCK_CODEC(22), true);
    /* Keep C's reference too. */

    /* Both entries are held: no eviction is allowed. */
    ed = tbt_fec_codec_cache_get(&c, &kd, &ready);
    eq_ptr(ed, NULL, "no victim returns NULL");
    eq_u64(stats_of(&c).evictions, 1, "no eviction without a free entry");
    ok(m.codec_frees == 1, "no codec freed by the failed get");
    ok(!codec_freed(&m, MOCK_CODEC(20)), "A still held");
    ok(!codec_freed(&m, MOCK_CODEC(22)), "C still held");

    tbt_fec_codec_cache_put(ec);
    ed = tbt_fec_codec_cache_get(&c, &kd, &ready);
    ok(ed != NULL && !ready, "D reserves after C is released");
    eq_u64(stats_of(&c).evictions, 2, "D evicts C");
    ok(codec_freed(&m, MOCK_CODEC(22)), "C is freed after release");
    ok(!codec_freed(&m, MOCK_CODEC(20)), "A is still held");
    tbt_fec_codec_cache_build_done(ed, MOCK_CODEC(23), true);
    tbt_fec_codec_cache_put(ed);

    tbt_fec_codec_cache_put(ea);
    ee = tbt_fec_codec_cache_get(&c, &ke, &ready);
    ok(ee != NULL && !ready, "E reserves after A is released");
    eq_u64(stats_of(&c).evictions, 3, "E evicts A");
    ok(codec_freed(&m, MOCK_CODEC(20)), "A is freed at its last reference");
    tbt_fec_codec_cache_build_done(ee, MOCK_CODEC(24), true);
    tbt_fec_codec_cache_put(ee);

    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 5, "all five codecs are freed exactly once");
    ok(m.slot_frees == 1, "slots are freed once");
}

static void test_build_failure(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *ea, *e, *e2;
    struct tbt_fec_codec_key ka, kb, kc, kd;
    struct mock m;
    bool ready;

    cache_init(&c, &m, 4);
    ka = make_key(128, 128, 256, 8, 1);
    kb = make_key(154, 102, 256, 8, 2);
    kc = make_key(178, 78, 256, 8, 3);
    kd = make_key(128, 128, 256, 8, 4);

    /* Keep the current codec's reference, as the data plane does. */
    ea = tbt_fec_codec_cache_get(&c, &ka, &ready);
    tbt_fec_codec_cache_build_done(ea, MOCK_CODEC(30), true);

    /* The target build fails. */
    e = tbt_fec_codec_cache_get(&c, &kb, &ready);
    ok(e != NULL && !ready, "target miss");
    tbt_fec_codec_cache_build_done(e, NULL, false);
    ok(e->ready == 0 && e->failed == 1, "target is marked failed");
    eq_u64(stats_of(&c).build_fail, 1, "failure counted");

    /* The current codec is unaffected. */
    e2 = tbt_fec_codec_cache_get(&c, &ka, &ready);
    ok(e2 != NULL && ready, "current codec still hits");
    eq_ptr(e2->codec, MOCK_CODEC(30), "current codec is intact");
    tbt_fec_codec_cache_put(e2);

    /* Retry the target on the same entry. */
    e2 = tbt_fec_codec_cache_get(&c, &kb, &ready);
    ok(e2 == e && !ready, "retry reuses the failed entry");
    tbt_fec_codec_cache_put(e2);
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(31), true);
    ok(e->codec == MOCK_CODEC(31) && e->ready == 1, "retry publishes");
    eq_u64(stats_of(&c).builds, 2, "two successful builds");
    tbt_fec_codec_cache_put(e);

    /* A partial codec returned with ok=false is freed at once. */
    e = tbt_fec_codec_cache_get(&c, &kc, &ready);
    ok(e != NULL && !ready, "third shape miss");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(32), false);
    ok(codec_freed(&m, MOCK_CODEC(32)), "partial codec is freed");
    eq_u64(stats_of(&c).build_fail, 2, "partial failure counted");
    ok(e->codec == NULL && e->ready == 0, "partial codec is not published");

    /* A duplicate publish keeps the first codec and frees the second. */
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(33), true);
    ok(e->codec == MOCK_CODEC(33) && e->ready == 1, "third shape publishes");
    e2 = tbt_fec_codec_cache_get(&c, &kc, &ready);
    ok(e2 != NULL && ready, "third shape hits");
    tbt_fec_codec_cache_build_done(e2, NULL, false);
    ok(e2->ready == 1 && e2->codec == MOCK_CODEC(33),
       "failure keeps the published codec");
    eq_u64(stats_of(&c).build_fail, 3, "ready-entry failure counted");
    tbt_fec_codec_cache_build_done(e2, MOCK_CODEC(34), true);
    ok(e2->codec == MOCK_CODEC(33), "duplicate keeps the first codec");
    ok(codec_freed(&m, MOCK_CODEC(34)), "duplicate codec is freed");
    eq_u64(stats_of(&c).builds, 3, "duplicate is not a build");
    tbt_fec_codec_cache_put(e2);
    tbt_fec_codec_cache_put(e);

    /* ok=true with no codec is a failed build, not a publication. */
    e = tbt_fec_codec_cache_get(&c, &kd, &ready);
    ok(e != NULL && !ready, "fourth shape miss");
    tbt_fec_codec_cache_build_done(e, NULL, true);
    ok(e->ready == 0 && e->failed == 1, "NULL codec is a failure");
    eq_u64(stats_of(&c).build_fail, 4, "NULL codec failure counted");
    tbt_fec_codec_cache_put(e);

    tbt_fec_codec_cache_put(ea);

    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 5, "three published, one partial and one duplicate");
    ok(m.slot_frees == 1, "slots are freed once");
}

static void test_capacity_four_two(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *e;
    struct tbt_fec_codec_key k[5];
    struct mock m;
    bool ready;
    int i;

    /* Downlink: four entries. */
    cache_init(&c, &m, TBT_FEC_CODEC_CACHE_DOWN);
    for (i = 0; i < 4; i++)
    {
        k[i] = make_key(128, 128, 256, 8, (u64)(i + 1));
        e = tbt_fec_codec_cache_get(&c, &k[i], &ready);
        ok(e != NULL && !ready, "down entry reserved");
        tbt_fec_codec_cache_build_done(e, MOCK_CODEC(40 + i), true);
        tbt_fec_codec_cache_put(e);
    }
    eq_u32(c.used, 4, "down cache holds four");
    eq_u64(stats_of(&c).evictions, 0, "no eviction at capacity");

    k[4] = make_key(128, 128, 256, 8, 5);
    e = tbt_fec_codec_cache_get(&c, &k[4], &ready);
    ok(e != NULL && !ready, "fifth key reserves");
    eq_u64(stats_of(&c).evictions, 1, "fifth key evicts");
    ok(codec_freed(&m, MOCK_CODEC(40)), "oldest down codec is freed");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(44), true);
    tbt_fec_codec_cache_put(e);

    for (i = 1; i < 5; i++)
    {
        e = tbt_fec_codec_cache_get(&c, &k[i], &ready);
        ok(e != NULL && ready, "surviving down entry hits");
        tbt_fec_codec_cache_put(e);
    }
    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 5, "five down codecs are freed once");
    ok(m.slot_frees == 1, "down slots are freed once");

    /* Uplink: two entries. */
    cache_init(&c, &m, TBT_FEC_CODEC_CACHE_UP);
    for (i = 0; i < 2; i++)
    {
        k[i] = make_key(8, 24, 128, 16, (u64)(i + 1));
        e = tbt_fec_codec_cache_get(&c, &k[i], &ready);
        ok(e != NULL && !ready, "up entry reserved");
        tbt_fec_codec_cache_build_done(e, MOCK_CODEC(50 + i), true);
        tbt_fec_codec_cache_put(e);
    }
    eq_u32(c.used, 2, "up cache holds two");
    eq_u64(stats_of(&c).evictions, 0, "no up eviction at capacity");

    k[2] = make_key(8, 24, 128, 16, 3);
    e = tbt_fec_codec_cache_get(&c, &k[2], &ready);
    ok(e != NULL && !ready, "third up key reserves");
    eq_u64(stats_of(&c).evictions, 1, "third up key evicts");
    ok(codec_freed(&m, MOCK_CODEC(50)), "oldest up codec is freed");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(52), true);
    tbt_fec_codec_cache_put(e);

    tbt_fec_codec_cache_destroy(&c);
    ok(m.codec_frees == 3, "three up codecs are freed once");
    ok(m.slot_frees == 1, "up slots are freed once");
}

static void test_destroy_defer(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_entry *e, *e2;
    struct tbt_fec_codec_key ka, kb;
    struct mock m;
    bool ready;

    cache_init(&c, &m, 2);
    ka = make_key(128, 128, 256, 8, 1);
    kb = make_key(128, 128, 256, 8, 2);

    e = tbt_fec_codec_cache_get(&c, &ka, &ready);
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(60), true);
    /* Keep A's reference. */
    e2 = tbt_fec_codec_cache_get(&c, &kb, &ready);
    tbt_fec_codec_cache_build_done(e2, MOCK_CODEC(61), true);
    tbt_fec_codec_cache_put(e2);

    tbt_fec_codec_cache_destroy(&c);
    ok(c.destroyed == 1, "cache is destroyed");
    ok(codec_freed(&m, MOCK_CODEC(61)), "unreferenced codec is freed");
    ok(!codec_freed(&m, MOCK_CODEC(60)), "held codec is deferred");
    ok(m.slot_frees == 0, "slot array is deferred");

    e2 = tbt_fec_codec_cache_get(&c, &kb, &ready);
    eq_ptr(e2, NULL, "get after destroy returns NULL");
    ok(!ready, "get after destroy is not ready");

    tbt_fec_codec_cache_put(e);
    ok(codec_freed(&m, MOCK_CODEC(60)), "held codec is freed at last put");
    ok(m.slot_frees == 1, "slot array is freed at last put");
    tbt_fec_codec_cache_destroy(&c);
    ok(m.slot_frees == 1, "destroy is idempotent after the drain");

    /* A build that finishes after destroy is still owned and freed. */
    cache_init(&c, &m, 2);
    e = tbt_fec_codec_cache_get(&c, &ka, &ready);
    ok(e != NULL && !ready, "reservation before destroy");
    tbt_fec_codec_cache_destroy(&c);
    ok(!codec_freed(&m, MOCK_CODEC(70)), "nothing to free yet");
    tbt_fec_codec_cache_build_done(e, MOCK_CODEC(70), true);
    ok(e->codec == MOCK_CODEC(70), "build publishes into the retired entry");
    tbt_fec_codec_cache_put(e);
    ok(codec_freed(&m, MOCK_CODEC(70)), "retired codec is freed");
    ok(m.slot_frees == 1, "retired slots are freed");
}

static void test_null_arguments(void)
{
    struct tbt_fec_codec_cache c;
    struct tbt_fec_codec_key k;
    struct mock m;
    bool ready = true;

    cache_init(&c, &m, 2);
    k = make_key(128, 128, 256, 8, 1);

    eq_ptr(tbt_fec_codec_cache_get(NULL, &k, &ready), NULL,
           "get with a NULL cache");
    ok(!ready, "NULL cache is not ready");
    eq_ptr(tbt_fec_codec_cache_get(&c, NULL, &ready), NULL,
           "get with a NULL key");
    tbt_fec_codec_cache_put(NULL);
    tbt_fec_codec_cache_build_done(NULL, MOCK_CODEC(80), false);
    ok(m.codec_frees == 0, "NULL entry leaves the codec alone");
    tbt_fec_codec_cache_destroy(NULL);
    ok(m.slot_frees == 0, "NULL destroy does nothing");

    tbt_fec_codec_cache_destroy(&c);
    ok(m.slot_frees == 1, "cache still frees its slots");
}

int main(void)
{
    test_init_capacity();
    test_hit_miss_seed();
    test_lru_eviction();
    test_ref_defer();
    test_build_failure();
    test_capacity_four_two();
    test_destroy_defer();
    test_null_arguments();

    printf("test_fec_codec_cache: %d checks, %d failures\n", checks,
           failures);
    return failures ? 1 : 0;
}
