/* tbt_fec_codec_cache.c - see tbt_fec_codec_cache.h for the contract.
 *
 * The cache is a fixed-capacity array of entries. Each get() stamps
 * last_use from a monotonic counter; eviction takes the smallest stamp among
 * the entries with refs == 0. The array is allocated once by init() so a get
 * never allocates.
 */
#include "tbt_fec_codec_cache.h"

static bool key_eq(const struct tbt_fec_codec_key *a,
                   const struct tbt_fec_codec_key *b)
{
    return a->n == b->n && a->m == b->m && a->block == b->block &&
           a->L == b->L && a->seed == b->seed;
}

static void codec_free(struct tbt_fec_codec_cache *c,
                       struct tbt_fec_grouped *codec)
{
    if (codec)
        c->free(c->ctx, codec, TBT_FEC_CODEC_CACHE_MEM_CODEC);
}

/* Zero one slot but keep its back pointer. */
static void entry_reset(struct tbt_fec_codec_entry *e)
{
    struct tbt_fec_codec_cache *c = e->cache;

    memset(e, 0, sizeof(*e));
    e->cache = c;
}

static struct tbt_fec_codec_entry *
entry_find(struct tbt_fec_codec_cache *c,
           const struct tbt_fec_codec_key *key)
{
    u32 i;

    for (i = 0; i < c->capacity; i++)
        if (c->entry[i].used && key_eq(&c->entry[i].key, key))
            return &c->entry[i];
    return NULL;
}

/* Least recently used entry with no live reference, or NULL. */
static struct tbt_fec_codec_entry *
entry_victim(struct tbt_fec_codec_cache *c)
{
    struct tbt_fec_codec_entry *best = NULL;
    u32 i;

    for (i = 0; i < c->capacity; i++)
    {
        struct tbt_fec_codec_entry *e = &c->entry[i];

        if (!e->used || e->refs)
            continue;
        if (!best || e->last_use < best->last_use)
            best = e;
    }
    return best;
}

/* Reserve a slot for key and return it with one reference. */
static struct tbt_fec_codec_entry *
entry_reserve(struct tbt_fec_codec_cache *c,
              const struct tbt_fec_codec_key *key)
{
    struct tbt_fec_codec_entry *e = NULL;
    u32 i;

    if (c->used < c->capacity)
    {
        for (i = 0; i < c->capacity; i++)
        {
            if (c->entry[i].used)
                continue;
            e = &c->entry[i];
            c->used++;
            break;
        }
    }
    else
    {
        e = entry_victim(c);
        if (!e)
            return NULL;
        if (e->codec)
            codec_free(c, e->codec);
        c->stats.evictions++;
    }

    if (!e)
        return NULL;
    entry_reset(e);
    e->key = *key;
    e->refs = 1;
    e->ready = 0;
    e->last_use = ++c->clock;
    e->used = 1;
    return e;
}

static void slots_free(struct tbt_fec_codec_cache *c)
{
    if (!c->entry)
        return;
    c->free(c->ctx, c->entry, TBT_FEC_CODEC_CACHE_MEM_SLOTS);
    c->entry = NULL;
    c->capacity = 0;
    c->used = 0;
}

int tbt_fec_codec_cache_init(struct tbt_fec_codec_cache *c, u32 capacity,
                             tbt_fec_codec_cache_alloc_fn alloc,
                             tbt_fec_codec_cache_free_fn free, void *ctx)
{
    struct tbt_fec_codec_entry *entry;
    u32 i;

    if (!c)
        return -EINVAL;
    memset(c, 0, sizeof(*c));
    if (!alloc || !free || !capacity || capacity > TBT_FEC_CODEC_CACHE_MAX)
        return -EINVAL;
    entry = alloc(ctx, (size_t)capacity * sizeof(*entry));
    if (!entry)
        return -ENOMEM;
    memset(entry, 0, (size_t)capacity * sizeof(*entry));
    c->entry = entry;
    c->capacity = capacity;
    c->free = free;
    c->ctx = ctx;
    for (i = 0; i < capacity; i++)
        entry[i].cache = c;
    return 0;
}

void tbt_fec_codec_cache_destroy(struct tbt_fec_codec_cache *c)
{
    u32 i;

    if (!c || !c->entry || c->destroyed)
        return;
    c->destroyed = 1;
    c->retiring = 0;
    for (i = 0; i < c->capacity; i++)
    {
        struct tbt_fec_codec_entry *e = &c->entry[i];

        if (!e->used)
            continue;
        e->retired = 1;
        if (e->refs)
        {
            c->retiring++;
            continue;
        }
        if (e->codec)
            codec_free(c, e->codec);
        entry_reset(e);
        e->used = 0;
        c->used--;
    }
    if (c->retiring)
        return;
    /* The release decision belongs to the object itself, whichever side
     * drains the last reference. Reading c->retiring in the caller after
     * this function returns would race the last put(): that put frees the
     * slot array and calls release(c), so a caller-side test would read
     * freed memory and could free the object a second time. */
    slots_free(c);
    if (c->free_self && c->release)
    {
        c->free_self = 0;
        c->release(c);
    }
}

struct tbt_fec_codec_entry *
tbt_fec_codec_cache_get(struct tbt_fec_codec_cache *c,
                        const struct tbt_fec_codec_key *key, bool *ready)
{
    struct tbt_fec_codec_entry *e;

    if (ready)
        *ready = false;
    if (!c || !key || !c->entry || c->destroyed)
        return NULL;

    e = entry_find(c, key);
    if (e)
    {
        e->refs++;
        e->last_use = ++c->clock;
        if (e->ready && e->codec)
        {
            c->stats.hits++;
            if (ready)
                *ready = true;
        }
        else
        {
            /* The key is present but no usable codec exists yet. The
             * caller keeps its current shape and may retry the build. */
            c->stats.misses++;
        }
        return e;
    }

    c->stats.misses++;
    return entry_reserve(c, key);
}

void tbt_fec_codec_cache_put(struct tbt_fec_codec_entry *e)
{
    struct tbt_fec_codec_cache *c;

    if (!e || !e->refs)
        return;
    c = e->cache;
    e->refs--;
    if (e->refs || !e->retired)
        return;

    if (e->codec)
        codec_free(c, e->codec);
    entry_reset(e);
    e->used = 0;
    if (c->used)
        c->used--;
    if (c->retiring)
    {
        c->retiring--;
        if (!c->retiring)
        {
            slots_free(c);
            if (c->free_self && c->release)
            {
                c->free_self = 0;
                c->release(c);
            }
        }
    }
}

void tbt_fec_codec_cache_build_done(struct tbt_fec_codec_entry *e,
                                    struct tbt_fec_grouped *codec, bool ok)
{
    struct tbt_fec_codec_cache *c;

    if (!e)
        return;
    c = e->cache;
    if (!ok || !codec)
    {
        c->stats.build_fail++;
        if (codec)
            codec_free(c, codec);
        if (!e->codec)
        {
            e->ready = 0;
            e->failed = 1;
        }
        return;
    }

    if (e->codec)
    {
        /* A second builder lost the race. Keep the published codec. */
        codec_free(c, codec);
        return;
    }
    e->codec = codec;
    e->ready = 1;
    e->failed = 0;
    c->stats.builds++;
}

void tbt_fec_codec_cache_stats(const struct tbt_fec_codec_cache *c,
                               struct tbt_fec_codec_cache_stats *out)
{
    if (!out)
        return;
    if (!c)
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = c->stats;
}
