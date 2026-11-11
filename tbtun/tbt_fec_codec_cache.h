/* tbt_fec_codec_cache.h - a small LRU of tbt_fec_grouped codecs.
 *
 * WHAT THIS IS
 *   Route A builds one codec per (n, m, block, L, seed) and reuses it while
 *   that shape stays in the cache. The downlink keeps four shapes; the
 *   uplink keeps two (current and target). A codec build costs tens of
 *   microseconds to milliseconds, so the cache never builds one:
 *
 *     get() reserves an entry and returns it with one reference;
 *     the caller drops its lock and builds the codec;
 *     build_done() publishes the codec or records the failure;
 *     put() drops the reference when the caller stops using the codec.
 *
 *   A build failure keeps the entry. The caller applies its retry backoff
 *   and keeps using the current shape, which is a different entry.
 *
 * REFERENCE COUNTS AND DELAYED FREE
 *   Every get() adds one reference. An entry with refs > 0 is never evicted
 *   and its codec is never freed. Eviction only picks an entry with
 *   refs == 0, so a codec that a block still uses survives any number of
 *   misses. If no entry has refs == 0, get() returns NULL: the caller keeps
 *   the current shape and retries later.
 *
 *   destroy() retires every entry. A retired entry with refs == 0 frees its
 *   codec at once. A retired entry with refs > 0 frees its codec when the
 *   last put() drops the reference. The slot array is freed at the same
 *   point, so the cache object must stay alive until every reference is
 *   gone.
 *
 * LOCKING
 *   The cache has no internal lock. The caller serializes init(), get(),
 *   put(), build_done(), destroy() and stats() with the lock that owns the
 *   cache. get() returns before the caller builds, so the caller may drop
 *   that lock; the reference keeps the entry and its codec alive.
 *
 *   free_fn() runs in the caller's context. If that context is a spinlock
 *   or softirq, the callback must not sleep. The module adapter can queue
 *   tbt_fec_grouped_free() on the FEC free workqueue.
 *
 * HOST TESTS
 *   alloc_fn() and free_fn() are injected. The host test passes malloc and
 *   mock codec pointers. The module passes kvzalloc/kvfree for the slot
 *   array and a tbt_fec_grouped_free() adapter for codecs.
 */
#ifndef _TBT_FEC_CODEC_CACHE_H
#define _TBT_FEC_CODEC_CACHE_H

#include "tbt_fec.h"

/* Downlink: current shape plus three recently used shapes. Uplink: current
 * and target. init() accepts 1..TBT_FEC_CODEC_CACHE_MAX. */
#define TBT_FEC_CODEC_CACHE_DOWN 4u
#define TBT_FEC_CODEC_CACHE_UP 2u
#define TBT_FEC_CODEC_CACHE_MAX TBT_FEC_CODEC_CACHE_DOWN

/* The key is the complete codec identity. The seed is part of it because
 * two sessions can use the same shape with different graph seeds. */
struct tbt_fec_codec_key
{
    u16 n;
    u16 m;
    u16 block; /* codeword block, the codec's w */
    u16 L;     /* lifting factor */
    u64 seed;
};

/* The cache allocates its slot array through alloc_fn() and releases it,
 * and every codec it owns, through free_fn(). The kind tells the adapter
 * which destructor to run. */
enum tbt_fec_codec_cache_mem
{
    TBT_FEC_CODEC_CACHE_MEM_SLOTS = 1,
    TBT_FEC_CODEC_CACHE_MEM_CODEC = 2,
};

typedef void *(*tbt_fec_codec_cache_alloc_fn)(void *ctx, size_t bytes);
typedef void (*tbt_fec_codec_cache_free_fn)(
    void *ctx, void *ptr, enum tbt_fec_codec_cache_mem kind);

struct tbt_fec_codec_cache;

struct tbt_fec_codec_entry
{
    struct tbt_fec_codec_key key;
    struct tbt_fec_grouped *codec;
    u32 refs;
    u8 ready; /* 1: codec is published and usable; 0: build it or wait */
    u64 last_use;
    /* private */
    struct tbt_fec_codec_cache *cache;
    u8 used;    /* this slot holds a key */
    u8 failed;  /* the last build failed; ready stays 0 */
    u8 retired; /* unlinked by destroy(); free on the last put() */
};

struct tbt_fec_codec_cache_stats
{
    u64 hits;      /* get() returned a published codec */
    u64 misses;    /* get() found no usable codec (absent, pending or failed) */
    u64 builds;    /* build_done() published a codec */
    u64 build_fail;/* build_done() reported a failure */
    u64 evictions; /* a slot was reclaimed for a new key */
};

struct tbt_fec_codec_cache
{
    struct tbt_fec_codec_entry *entry;
    u32 capacity;
    u32 used;
    u64 clock; /* monotonic get() counter for LRU order */
    struct tbt_fec_codec_cache_stats stats;
    tbt_fec_codec_cache_free_fn free;
    void *ctx;
    u32 retiring; /* retired entries with refs > 0 */
    u8 destroyed;
};

/* Allocate the slot array and zero the cache. capacity is 1..MAX.
 * Returns 0, -EINVAL for a bad capacity or callback, or -ENOMEM.
 * May sleep. */
int tbt_fec_codec_cache_init(struct tbt_fec_codec_cache *c, u32 capacity,
                             tbt_fec_codec_cache_alloc_fn alloc,
                             tbt_fec_codec_cache_free_fn free, void *ctx);

/* Retire every entry and release the codecs that no caller holds. A codec
 * with refs > 0 is released by the last put(); the slot array goes with it.
 * get() returns NULL after destroy(). destroy() is idempotent. */
void tbt_fec_codec_cache_destroy(struct tbt_fec_codec_cache *c);

/* Return the entry for key with one new reference. On a hit with a
 * published codec, *ready is true. On a miss, the cache reserves a slot and
 * *ready is false; the caller builds outside its lock and calls
 * build_done(). On a hit whose codec is not published, *ready is false and
 * the same entry is returned; the caller keeps its current codec.
 *
 * Returns NULL when the cache is destroyed, the key is invalid, or the cache
 * is full and every entry has refs > 0. A NULL return never frees a codec.
 * A non-NULL return must be released with put(). */
struct tbt_fec_codec_entry *
tbt_fec_codec_cache_get(struct tbt_fec_codec_cache *c,
                        const struct tbt_fec_codec_key *key, bool *ready);

/* Drop one reference. The last reference to a retired entry frees its codec
 * and, after destroy(), the slot array. A live cached entry stays. */
void tbt_fec_codec_cache_put(struct tbt_fec_codec_entry *e);

/* Publish a built codec or record a failed build.
 *
 * ok and codec non-NULL: publish the codec unless the entry already has one.
 * A second builder that lost the race has its codec freed and the published
 * one kept.
 *
 * !ok or codec NULL: increment build_fail. Free codec when it is non-NULL.
 * If the entry has no codec, set failed and leave ready 0. If it already has
 * a published codec, keep that codec and its ready state.
 *
 * The caller must hold a reference from get(). */
void tbt_fec_codec_cache_build_done(struct tbt_fec_codec_entry *e,
                                    struct tbt_fec_grouped *codec, bool ok);

/* Copy the counters. out may be NULL. */
void tbt_fec_codec_cache_stats(const struct tbt_fec_codec_cache *c,
                               struct tbt_fec_codec_cache_stats *out);

#endif /* _TBT_FEC_CODEC_CACHE_H */
