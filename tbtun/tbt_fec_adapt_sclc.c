/* tbt_fec_adapt_sclc.c - SC-LDPC adaptive rate: estimator and rate control.
 *
 * See tbt_fec_adapt_sclc.h for the contract. This file is pure: no
 * allocation, no kernel globals, no wire access. It compiles into the module
 * and into host/test_fec_adapt_sclc.c unchanged.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "tbt_fec_adapt_sclc.h"

/* ------------------------------------------------------------------ *
 * Rate ladder
 * ------------------------------------------------------------------ */

/* The actual codec rate for each rate_pct. The graph rounds n_base, so 60 and
 * 70 are not exactly 0.60 and 0.70; n/m must use these values. */
static const struct tbt_fec_adapt_sclc_rate tbt_fec_adapt_sclc_ladder
    [TBT_FEC_ADAPT_SCLC_LEVELS] = {
        {25, 250000u}, {30, 299985u}, {40, 400000u},
        {50, 500000u}, {60, 592593u}, {70, 695652u},
};

const struct tbt_fec_adapt_sclc_rate *
tbt_fec_adapt_sclc_rate_at(u8 index)
{
    if (index >= TBT_FEC_ADAPT_SCLC_LEVELS)
        return NULL;
    return &tbt_fec_adapt_sclc_ladder[index];
}

u8 tbt_fec_adapt_sclc_rate_index(u8 rate_pct)
{
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        if (tbt_fec_adapt_sclc_ladder[i].pct == rate_pct)
            return i;
    return 0xff;
}

u32 tbt_fec_adapt_sclc_rate_ppm(u8 rate_pct)
{
    u8 i = tbt_fec_adapt_sclc_rate_index(rate_pct);

    return i == 0xff ? 0u : tbt_fec_adapt_sclc_ladder[i].ppm;
}

u8 tbt_fec_adapt_sclc_rate_index_for_shape(u16 n, u16 m)
{
    u16 shares = (u16)(n + m);
    u8 i;

    if (!n || !m)
        return 0xff;
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        if ((u16)((u64)tbt_fec_adapt_sclc_ladder[i].ppm * shares /
                  1000000ull) == n)
            return i;
    return 0xff;
}

int tbt_fec_adapt_sclc_init_plan(u16 n, u16 m, u16 groups, u8 variant,
                                 u8 *idx, u8 *mask)
{
    u8 i = tbt_fec_adapt_sclc_rate_index_for_shape(n, m);
    u8 msk = 0;

    if (!idx || !mask)
        return -1;
    if (i != 0xff)
    {
        /* The binary downlink builds every ladder split: above 50% the
         * graph lowers the payload degree and adds one check per transmitted
         * parity share, so nothing is punctured. */
        if (variant == TBT_FEC_SCLC_DOWN && groups == 1)
            msk = (u8)((1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u);
        else if (variant == TBT_FEC_SCLC_UP && groups > 1)
            msk = (u8)((1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u);
        else
            i = 0xff;
    }
    *idx = i;
    *mask = i == 0xff ? 0 : msk;
    return i == 0xff ? -1 : 0;
}

int tbt_fec_adapt_sclc_shape(u8 rate_pct, u16 shares, u16 groups,
                             struct tbt_fec_adapt_sclc_shape *out)
{
    u8 i = tbt_fec_adapt_sclc_rate_index(rate_pct);
    u16 n;

    if (!out || i == 0xff || shares < 2 || shares > TBT_FEC_MAX_SHARES ||
        !groups || (u32)groups * shares > TBT_FEC_MAX_SHARES)
        return -1;
    n = (u16)((u64)tbt_fec_adapt_sclc_ladder[i].ppm * shares / 1000000ull);
    if (!n || n >= shares)
        return -1;
    memset(out, 0, sizeof(*out));
    out->rate_pct = rate_pct;
    out->rate_ppm = tbt_fec_adapt_sclc_ladder[i].ppm;
    out->n = n;
    out->m = (u16)(shares - n);
    out->groups = groups;
    return 0;
}

int tbt_fec_adapt_sclc_profile_of(u8 rate_pct,
                                  struct tbt_fec_adapt_sclc_profile *out)
{
    if (!out || tbt_fec_adapt_sclc_rate_index(rate_pct) == 0xff)
        return -1;
    memset(out, 0, sizeof(*out));
    if (rate_pct == 40)
    {
        out->block = 512;
        out->L = 16;
        out->payload = 4096;
    }
    else
    {
        out->block = 256;
        out->L = 16;
        out->payload = 2048;
    }
    return 0;
}

int tbt_fec_adapt_sclc_wire_of_cfg(const struct tbt_fec_cfg *cfg,
                                  u16 carriers,
                                  struct tbt_fec_adapt_sclc_wire *out)
{
    u16 shares, block, L;
    u8 kind, idx;

    if (!cfg || !out || !carriers || !cfg->n || !cfg->m || !cfg->groups)
        return -1;
    shares = (u16)(cfg->n + cfg->m);
    if (shares > TBT_FEC_MAX_SHARES ||
        (u32)cfg->groups * shares != carriers)
        return -1;
    /* v8 carries the codec identity in the CFG extension; the legacy
     * w/k/d classification is no longer a second source of truth. */
    if (cfg->codec_variant != 2 ||
        (cfg->L != 8 && cfg->L != 16) ||
        cfg->rate_ppm < 250000u || cfg->rate_ppm > 700000u)
        return -1;
    if ((u32)(((u64)cfg->rate_ppm * shares) / 1000000u) != cfg->n)
        return -1;
    block = cfg->block;
    L = cfg->L;
    if (cfg->groups == 1 && shares == TBT_FEC_MAX_SHARES)
    {
        kind = TBT_FEC_SCLC_DOWN;
        if (block != TBT_FEC_SCLC_BLOCK_DOWN ||
            !tbt_fec_sclc_down_split(cfg->n, cfg->m))
            return -1;
        if (L != TBT_FEC_SCLC_L_DOWN)
            return -1;
    }
    else if (cfg->groups > 1)
    {
        kind = TBT_FEC_SCLC_UP;
        if (cfg->groups <= 1 || shares != 32 ||
            block != TBT_FEC_SCLC_BLOCK_UP || L != TBT_FEC_SCLC_L_UP)
            return -1;
    }
    else
        return -1;
    idx = tbt_fec_adapt_sclc_rate_index_for_shape(cfg->n, cfg->m);
    if (idx == 0xff)
        return -1;
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->symbol = kind == TBT_FEC_SCLC_DOWN ? 0 : 1;
    out->block = block;
    out->L = L;
    out->rate_pct = tbt_fec_adapt_sclc_ladder[idx].pct;
    out->rate_ppm = tbt_fec_adapt_sclc_ladder[idx].ppm;
    out->n = cfg->n;
    out->m = cfg->m;
    out->groups = cfg->groups;
    return 0;
}

int tbt_fec_adapt_sclc_wire_to_cfg(const struct tbt_fec_adapt_sclc_wire *p,
                                  u32 epoch, u8 flags,
                                  struct tbt_fec_cfg *cfg)
{
    u16 shares;
    u8 idx;

    if (!p || !cfg || !p->n || !p->m || !p->groups)
        return -1;
    shares = (u16)(p->n + p->m);
    if ((u32)p->groups * shares > TBT_FEC_MAX_SHARES)
        return -1;
    idx = tbt_fec_adapt_sclc_rate_index_for_shape(p->n, p->m);
    if (idx == 0xff)
        return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->epoch = epoch;
    cfg->flags = flags;
    cfg->n = p->n;
    cfg->m = p->m;
    cfg->groups = p->groups;
    cfg->codec_variant = 2;
    cfg->L = p->L;
    cfg->block = p->block;
    cfg->rate_ppm = p->rate_ppm;
    if ((u32)(((u64)p->rate_ppm * shares) / 1000000u) != p->n)
        return -1;
    if (p->kind == TBT_FEC_SCLC_DOWN)
    {
        if (shares != TBT_FEC_MAX_SHARES ||
            p->block != TBT_FEC_SCLC_BLOCK_DOWN)
            return -1;
        /* Legacy mirror for the in-kernel shape vocabulary. */
        cfg->w = TBT_FEC_SCLC_BLOCK_DOWN;
        cfg->k = (u16)((u32)p->n * 1000u / shares);
        cfg->d = 0;
    }
    else if (p->kind == TBT_FEC_SCLC_UP)
    {
        if (shares != 32 || p->block != TBT_FEC_SCLC_BLOCK_UP ||
            p->L != TBT_FEC_SCLC_L_UP)
            return -1;
        cfg->w = TBT_FEC_SCLC_L_UP;
        cfg->k = TBT_FEC_SCLC_BLOCK_UP;
        cfg->d = (u8)((u32)p->n * 256u / shares);
    }
    else
        return -1;
    return 0;
}

bool tbt_fec_adapt_sclc_wire_compatible(const struct tbt_fec_cfg *cur,
                                        const struct tbt_fec_cfg *next,
                                        u16 carriers)
{
    struct tbt_fec_adapt_sclc_wire a, b;

    if (tbt_fec_adapt_sclc_wire_of_cfg(cur, carriers, &a) ||
        tbt_fec_adapt_sclc_wire_of_cfg(next, carriers, &b))
        return false;
    return a.kind == b.kind && a.symbol == b.symbol && a.block == b.block &&
           a.L == b.L && a.groups == b.groups &&
           (u16)(a.n + a.m) == (u16)(b.n + b.m);
}

/* ------------------------------------------------------------------ *
 * Estimator
 * ------------------------------------------------------------------ */

static u32 tbt_fec_adapt_sclc_u32(u64 v)
{
    return v > 0xffffffffull ? 0xffffffffu : (u32)v;
}

/* Advance the bucket ring to `now`. A gap shorter than one bucket stays in the
 * current bucket; a gap of one to seven buckets advances that many slots; a
 * longer gap starts a fresh ring, so stale samples never outlive the window. */
static void tbt_fec_adapt_sclc_rotate(struct tbt_fec_adapt_sclc_est *e,
                                      u64 now_ns)
{
    struct tbt_fec_adapt_sclc_win *b;
    u64 elapsed;
    u64 steps;

    if (!e->win[0].start_ns)
    {
        e->cur = 0;
        e->win[0].start_ns = now_ns;
        return;
    }
    b = &e->win[e->cur];
    if (now_ns < b->start_ns)
    {
        memset(e->win, 0, sizeof(e->win));
        e->cur = 0;
        e->win[0].start_ns = now_ns;
        return;
    }
    elapsed = now_ns - b->start_ns;
    if (elapsed < TBT_FEC_ADAPT_SCLC_BUCKET_NS)
        return;
    steps = elapsed / TBT_FEC_ADAPT_SCLC_BUCKET_NS;
    if (steps >= TBT_FEC_ADAPT_SCLC_BUCKETS)
    {
        memset(e->win, 0, sizeof(e->win));
        e->cur = 0;
        e->win[0].start_ns = now_ns;
        return;
    }
    while (steps--)
    {
        u64 next = b->start_ns + TBT_FEC_ADAPT_SCLC_BUCKET_NS;

        e->cur = (u8)((e->cur + 1) % TBT_FEC_ADAPT_SCLC_BUCKETS);
        b = &e->win[e->cur];
        memset(b, 0, sizeof(*b));
        b->start_ns = next;
    }
}

void tbt_fec_adapt_sclc_est_init(struct tbt_fec_adapt_sclc_est *e, u16 shares)
{
    memset(e, 0, sizeof(*e));
    if (!shares || shares > TBT_FEC_MAX_SHARES)
        shares = TBT_FEC_MAX_SHARES;
    e->shares = shares;
}

void tbt_fec_adapt_sclc_est_carriers(struct tbt_fec_adapt_sclc_est *e,
                                     u16 alive, u16 stalled, u16 total)
{
    e->alive = alive;
    e->stalled = stalled;
    e->carriers = total;
}

void tbt_fec_adapt_sclc_est_block(
    struct tbt_fec_adapt_sclc_est *e, u64 now_ns, u16 group, u16 real,
    const u8 miss_mask[TBT_FEC_BITMAP_BYTES],
    const u8 alive[TBT_FEC_BITMAP_BYTES])
{
    u32 base;
    u32 live = 0;
    u32 miss = 0;
    u32 limit = e->carriers ? e->carriers : TBT_FEC_MAX_CARRIERS;
    u16 i;

    if (real > e->shares)
        real = e->shares;
    if (real > TBT_FEC_MAX_SHARES)
        real = TBT_FEC_MAX_SHARES;
    base = (u32)group * e->shares;
    for (i = 0; i < real; i++)
    {
        u32 carrier = base + i;

        if (carrier >= limit)
            continue;
        if (!tbt_fec_adapt_bitmap_test(alive, (u16)carrier))
            continue;
        live++;
        if (tbt_fec_adapt_bitmap_test(miss_mask, i))
            miss++;
    }
    e->sb_sent += live;
    e->sb_miss += miss;
    e->last_activity_ns = now_ns;
}

bool tbt_fec_adapt_sclc_est_credit_late(struct tbt_fec_adapt_sclc_est *e)
{
    u32 *miss;
    u8 prev;

    if (!e)
        return false;
    prev = (u8)((e->cur + TBT_FEC_ADAPT_SCLC_BUCKETS - 1) %
                TBT_FEC_ADAPT_SCLC_BUCKETS);
    if (e->win[e->cur].miss)
        miss = &e->win[e->cur].miss;
    else if (e->win[prev].miss)
        miss = &e->win[prev].miss;
    else
        return false;
    (*miss)--;
    e->p_ewma_ppm = e->p_ewma_ppm > 7800u ? e->p_ewma_ppm - 7800u : 0;
    return true;
}

void tbt_fec_adapt_sclc_est_super(struct tbt_fec_adapt_sclc_est *e,
                                  u64 now_ns)
{
    struct tbt_fec_adapt_sclc_win *w;
    int delta;
    u32 p_block;

    e->last_activity_ns = now_ns;
    tbt_fec_adapt_sclc_rotate(e, now_ns);
    if (!e->sb_sent)
    {
        e->sb_miss = 0;
        return;
    }
    w = &e->win[e->cur];
    w->sent += e->sb_sent;
    w->miss += e->sb_miss;
    w->supers++;

    p_block = (u32)((u64)e->sb_miss * 1000000ull / e->sb_sent);
    delta = (int)p_block - (int)e->p_ewma_ppm;
    e->p_ewma_ppm =
        (u32)((int)e->p_ewma_ppm + delta / (1 << TBT_FEC_ADAPT_SCLC_EWMA_SHIFT));
    if (e->p_ewma_ppm > 1000000u)
        e->p_ewma_ppm = 1000000u;

    e->sb_sent = 0;
    e->sb_miss = 0;
}

void tbt_fec_adapt_sclc_est_unknown(struct tbt_fec_adapt_sclc_est *e,
                                    u64 now_ns)
{
    e->last_activity_ns = now_ns;
    tbt_fec_adapt_sclc_rotate(e, now_ns);
    e->win[e->cur].unknown++;
    e->unknown_total++;
}

void tbt_fec_adapt_sclc_est_abandoned(struct tbt_fec_adapt_sclc_est *e,
                                      u64 now_ns, u32 shares)
{
    e->last_activity_ns = now_ns;
    tbt_fec_adapt_sclc_rotate(e, now_ns);
    e->win[e->cur].abandoned += shares;
    e->abandoned_total += shares;
}

void tbt_fec_adapt_sclc_est_fail(struct tbt_fec_adapt_sclc_est *e,
                                 u64 now_ns, u32 requested, u32 pkts)
{
    tbt_fec_adapt_sclc_rotate(e, now_ns);
    e->win[e->cur].requested += requested;
    e->win[e->cur].pkts += pkts;
}

void tbt_fec_adapt_sclc_est_codec_fail(struct tbt_fec_adapt_sclc_est *e,
                                       u64 now_ns, u32 shares)
{
    tbt_fec_adapt_sclc_rotate(e, now_ns);
    e->win[e->cur].codec_fail += shares;
}

void tbt_fec_adapt_sclc_est_sample(const struct tbt_fec_adapt_sclc_est *e,
                                   u64 now_ns,
                                   struct tbt_fec_adapt_sclc_est_out *o)
{
    u64 sent = 0, miss = 0, supers = 0, requested = 0, pkts = 0;
    u64 abandoned = 0, unknown = 0, codec_fail = 0;
    u64 fast_sent = 0, fast_miss = 0, fast_requested = 0, fast_pkts = 0;
    u64 fast_codec_fail = 0;
    u32 i;

    memset(o, 0, sizeof(*o));
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_BUCKETS; i++)
    {
        const struct tbt_fec_adapt_sclc_win *w = &e->win[i];
        u64 age;

        if (!w->start_ns || now_ns < w->start_ns)
            continue;
        age = now_ns - w->start_ns;
        if (age >= TBT_FEC_ADAPT_SCLC_WIN_NS)
            continue;
        sent += w->sent;
        miss += w->miss;
        supers += w->supers;
        requested += w->requested;
        pkts += w->pkts;
        abandoned += w->abandoned;
        unknown += w->unknown;
        codec_fail += w->codec_fail;
        if (age <= TBT_FEC_ADAPT_SCLC_FAST_NS)
        {
            fast_sent += w->sent;
            fast_miss += w->miss;
            fast_requested += w->requested;
            fast_pkts += w->pkts;
            fast_codec_fail += w->codec_fail;
        }
    }

    o->sent_win = tbt_fec_adapt_sclc_u32(sent);
    o->miss_win = tbt_fec_adapt_sclc_u32(miss);
    o->supers_win = tbt_fec_adapt_sclc_u32(supers);
    o->abandoned_win = tbt_fec_adapt_sclc_u32(abandoned);
    o->unknown_win = tbt_fec_adapt_sclc_u32(unknown);
    o->requested_win = tbt_fec_adapt_sclc_u32(requested);
    o->pkts_win = tbt_fec_adapt_sclc_u32(pkts);
    o->codec_fail_win = tbt_fec_adapt_sclc_u32(codec_fail);
    o->abandoned_total = e->abandoned_total;
    o->unknown_total = e->unknown_total;
    o->alive = e->alive;
    o->stalled = e->stalled;
    o->carriers = e->carriers;

    if (sent)
    {
        u64 p = miss * 1000000ull / sent;

        o->p_win_ppm = p > 1000000ull ? 1000000u : (u32)p;
    }
    if (fast_sent)
    {
        u64 p = fast_miss * 1000000ull / fast_sent;

        o->p_fast_ppm = p > 1000000ull ? 1000000u : (u32)p;
    }
    if (fast_pkts)
    {
        u64 r = fast_requested * 1000000ull / fast_pkts;

        o->r_fail_ppm = r > 1000000ull ? 1000000u : (u32)r;
    }
    if (fast_miss)
    {
        u64 f = fast_codec_fail * 1000000ull / fast_miss;

        o->codec_fail_ppm = f > 1000000ull ? 1000000u : (u32)f;
    }
    o->p_ewma_ppm = e->p_ewma_ppm;
    o->p_ppm = o->p_win_ppm > o->p_ewma_ppm ? o->p_win_ppm : o->p_ewma_ppm;
    {
        /* The evidence floor follows the shape's super-block size: a
         * multi-group uplink block carries groups*n DATA shares, so eight of
         * them are a much higher byte rate than eight single-group downlink
         * blocks. Keep the downlink at eight; ask the uplink for four, which
         * is still above the 512-share floor at every ladder point. */
        u16 groups = (e->carriers && e->shares)
                         ? (u16)(e->carriers / e->shares)
                         : 1;
        u32 min_supers = groups > 1 ? TBT_FEC_ADAPT_SCLC_MIN_SUPERS_UP
                                    : TBT_FEC_ADAPT_SCLC_MIN_SUPERS;

        o->p_valid = sent >= TBT_FEC_ADAPT_SCLC_MIN_SHARES &&
                     supers >= min_supers;
    }
    o->r_valid = fast_pkts > 0;
    o->pause_idle = !e->last_activity_ns || now_ns < e->last_activity_ns ||
                    now_ns - e->last_activity_ns > TBT_FEC_ADAPT_SCLC_IDLE_NS;
    o->pause_carrier =
        tbt_fec_adapt_carrier_pause(e->alive, e->stalled, e->carriers);
    o->pause_abandoned =
        abandoned && (!sent || abandoned * 100ull > sent);
    o->burst = fast_sent && o->p_fast_ppm > o->p_ppm &&
               o->p_fast_ppm > TBT_FEC_ADAPT_SCLC_BURST_PPM &&
               o->p_fast_ppm >
                   o->p_ewma_ppm + TBT_FEC_ADAPT_SCLC_BURST_GAP_PPM;
}

/* ------------------------------------------------------------------ *
 * Rate controller
 * ------------------------------------------------------------------ */

/* The p boundary at which the target leaves `index`: above it the target is
 * lower, at or below it the target is at least `index`. */
static u32 tbt_fec_adapt_sclc_boundary(u8 index)
{
    return 1000000u - tbt_fec_adapt_sclc_ladder[index].ppm -
           TBT_FEC_ADAPT_SCLC_MARGIN_PPM;
}

/* Next supported ladder index above `idx`, or `idx` when none. */
static u8 tbt_fec_adapt_sclc_next_rate(
    const struct tbt_fec_adapt_sclc_ctl *c, u8 idx)
{
    int i;

    for (i = idx + 1; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        if (c->rate_mask & (1u << i))
            return (u8)i;
    return idx;
}

/* Previous supported ladder index below `idx`, or `idx` when none. */
static u8 tbt_fec_adapt_sclc_prev_rate(
    const struct tbt_fec_adapt_sclc_ctl *c, u8 idx)
{
    int i;

    for (i = idx; i > 0; i--)
        if (c->rate_mask & (1u << (i - 1)))
            return (u8)(i - 1);
    return idx;
}

/* Highest supported ladder index below `idx` that the peer has not refused,
 * or the current rate when there is none. */
static u8 tbt_fec_adapt_sclc_fallback(
    const struct tbt_fec_adapt_sclc_ctl *c, u8 idx)
{
    u8 r = tbt_fec_adapt_sclc_prev_rate(c, idx);

    while (r != idx && (c->refused_mask & (1u << r)))
        r = tbt_fec_adapt_sclc_prev_rate(c, r);
    return r == idx ? c->cur : r;
}

/* Largest supported ladder index whose actual rate does not exceed
 * 1 - p - margin. When every supported rate is above that bound, the lowest
 * supported rate is the safe answer. */
static u8 tbt_fec_adapt_sclc_target(
    const struct tbt_fec_adapt_sclc_ctl *c, u32 p_ppm)
{
    u32 r_t;
    int i;

    if (p_ppm > 1000000u)
        p_ppm = 1000000u;
    if (p_ppm + TBT_FEC_ADAPT_SCLC_MARGIN_PPM >= 1000000u)
        r_t = tbt_fec_adapt_sclc_ladder[0].ppm;
    else
        r_t = 1000000u - p_ppm - TBT_FEC_ADAPT_SCLC_MARGIN_PPM;
    if (r_t < tbt_fec_adapt_sclc_ladder[0].ppm)
        r_t = tbt_fec_adapt_sclc_ladder[0].ppm;
    for (i = TBT_FEC_ADAPT_SCLC_LEVELS - 1; i >= 0; i--)
        if ((c->rate_mask & (1u << i)) &&
            tbt_fec_adapt_sclc_ladder[i].ppm <= r_t)
            return (u8)i;
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        if (c->rate_mask & (1u << i))
            return (u8)i;
    return c->cur;
}

static u32 tbt_fec_adapt_sclc_switches_since(
    const struct tbt_fec_adapt_sclc_ctl *c, u64 now_ns, u64 win_ns)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < c->sw_n; i++)
        if (now_ns >= c->sw[i] && now_ns - c->sw[i] < win_ns)
            n++;
    return n;
}

/* The dwell grows after an oscillation, up to the cap. `dir` is 1 up, 2 down. */
static u64 tbt_fec_adapt_sclc_dwell(const struct tbt_fec_adapt_sclc_ctl *c,
                                    u8 dir)
{
    u64 d = dir == 1 ? TBT_FEC_ADAPT_SCLC_UP_DWELL_NS
                     : TBT_FEC_ADAPT_SCLC_DOWN_DWELL_NS;

    d *= c->dwell_mul;
    if (d > TBT_FEC_ADAPT_SCLC_DWELL_CAP_NS)
        d = TBT_FEC_ADAPT_SCLC_DWELL_CAP_NS;
    return d;
}

static bool tbt_fec_adapt_sclc_can_switch(
    const struct tbt_fec_adapt_sclc_ctl *c, u64 now_ns, u8 dir)
{
    u64 since;

    if (c->last_switch_ns)
    {
        if (now_ns < c->last_switch_ns)
            return false;
        since = now_ns - c->last_switch_ns;
        if (since < TBT_FEC_ADAPT_SCLC_MIN_GAP_NS)
            return false;
        if (since < tbt_fec_adapt_sclc_dwell(c, dir))
            return false;
        if (c->last_dir && dir != c->last_dir &&
            since < TBT_FEC_ADAPT_SCLC_REVERSE_NS)
            return false;
    }
    if (tbt_fec_adapt_sclc_switches_since(c, now_ns,
                                          TBT_FEC_ADAPT_SCLC_RATE_WIN_NS) >=
        TBT_FEC_ADAPT_SCLC_RATE_MAX)
        return false;
    return true;
}

/* A burst may skip the dwell and the reverse cooldown, but not the minimum
 * gap or the 10-in-60 s cap. */
static bool tbt_fec_adapt_sclc_can_burst(
    const struct tbt_fec_adapt_sclc_ctl *c, u64 now_ns)
{
    if (c->last_switch_ns)
    {
        if (now_ns < c->last_switch_ns)
            return false;
        if (now_ns - c->last_switch_ns < TBT_FEC_ADAPT_SCLC_MIN_GAP_NS)
            return false;
    }
    return tbt_fec_adapt_sclc_switches_since(c, now_ns,
                                             TBT_FEC_ADAPT_SCLC_RATE_WIN_NS) <
           TBT_FEC_ADAPT_SCLC_RATE_MAX;
}

static void tbt_fec_adapt_sclc_issue(struct tbt_fec_adapt_sclc_ctl *c, u8 to,
                                     u8 reason, u8 probe,
                                     struct tbt_fec_adapt_sclc_out *out)
{
    struct tbt_fec_adapt_sclc_shape shape;

    /* The peer refused this shape: never put the same CFG on the wire
     * again. The callers filter their targets; this is the last guard. */
    if (c->refused_mask & (1u << to))
        return;
    /* init() proves every ladder point builds for this geometry, so this
     * cannot fail; check anyway instead of leaving `busy` stuck. */
    if (tbt_fec_adapt_sclc_shape(tbt_fec_adapt_sclc_ladder[to].pct, c->shares,
                                 c->groups, &shape))
        return;
    c->busy = 1;
    c->pending_to = to;
    c->pending_probe = probe;
    c->pending_reason = reason;
    c->epoch++;
    out->switch_now = 1;
    out->from = c->cur;
    out->to = to;
    out->reason = reason;
    out->epoch = c->epoch;
    out->shape = shape;
}

static void tbt_fec_adapt_sclc_fill_shape(
    const struct tbt_fec_adapt_sclc_ctl *c,
    struct tbt_fec_adapt_sclc_shape *shape)
{
    if (tbt_fec_adapt_sclc_shape(tbt_fec_adapt_sclc_ladder[c->cur].pct,
                                 c->shares, c->groups, shape))
        memset(shape, 0, sizeof(*shape));
}

int tbt_fec_adapt_sclc_ctl_init(struct tbt_fec_adapt_sclc_ctl *c, bool enabled,
                                u8 rate_pct, u16 shares, u16 groups)
{
    u8 idx = tbt_fec_adapt_sclc_rate_index(rate_pct);
    struct tbt_fec_adapt_sclc_shape shape;
    u8 i;

    memset(c, 0, sizeof(*c));
    if (idx == 0xff)
        return -1;
    /* Every ladder point must build, not only the start point: a later down
     * step must never discover that its n/m split is empty. */
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        if (tbt_fec_adapt_sclc_shape(tbt_fec_adapt_sclc_ladder[i].pct,
                                     shares, groups, &shape))
            return -1;
    c->ready = 1;
    c->enabled = enabled ? 1 : 0;
    c->cur = idx;
    c->prev_idx = idx;
    c->probe_from = idx;
    c->rate_mask = (u8)((1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u);
    c->dwell_mul = 1;
    c->shares = shares;
    c->groups = groups;
    return 0;
}

void tbt_fec_adapt_sclc_ctl_set_rate_mask(struct tbt_fec_adapt_sclc_ctl *c,
                                          u8 mask)
{
    if (!c->ready || !mask || !(mask & (1u << c->cur)))
        return;
    c->rate_mask = mask;
}

void tbt_fec_adapt_sclc_ctl_set_epoch(struct tbt_fec_adapt_sclc_ctl *c,
                                      u32 epoch)
{
    if (c->ready)
        c->epoch = epoch;
}

u16 tbt_fec_adapt_sclc_max_side(u16 shares, u8 mask)
{
    u16 max = 0;
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        u16 n, m, side;

        if (mask && !(mask & (1u << i)))
            continue;
        n = (u16)((u64)tbt_fec_adapt_sclc_ladder[i].ppm * shares /
                  1000000ull);
        m = (u16)(shares - n);
        side = n > m ? n : m;
        if (side > max)
            max = side;
    }
    return max;
}

void tbt_fec_adapt_sclc_ctl_set_enabled(struct tbt_fec_adapt_sclc_ctl *c,
                                        bool enabled, u64 now_ns)
{
    if (!c->ready)
        return;
    if ((enabled ? 1 : 0) == c->enabled)
        return;
    if (enabled && !c->enabled)
    {
        c->fast_start_done = 0;
        c->last_eval_ns = now_ns;
    }
    c->enabled = enabled ? 1 : 0;
    if (!c->enabled)
    {
        c->busy = 0;
        c->pending_to = c->cur;
        c->pending_reason = TBT_FEC_ADAPT_SCLC_R_NONE;
        c->pending_probe = 0;
        c->probe_active = 0;
        c->rollback_pending = 0;
    }
    c->up_run = 0;
    c->down_run = 0;
    c->fail_run = 0;
    c->probe_fail_run = 0;
    c->probe_low_since_ns = 0;
}

void tbt_fec_adapt_sclc_ctl_eval(struct tbt_fec_adapt_sclc_ctl *c,
                                 const struct tbt_fec_adapt_sclc_est_out *o,
                                 u64 now_ns,
                                 struct tbt_fec_adapt_sclc_out *out)
{
    u32 p;
    u32 p_eff;
    u32 r;
    u8 target;
    u8 next;
    u32 boundary;
    bool r_valid;

    memset(out, 0, sizeof(*out));
    out->from = c->cur;
    out->to = c->cur;
    out->epoch = c->epoch;
    out->reason = TBT_FEC_ADAPT_SCLC_R_NONE;
    if (c->ready)
        tbt_fec_adapt_sclc_fill_shape(c, &out->shape);

    if (!c->ready || !c->enabled)
    {
        out->reason = TBT_FEC_ADAPT_SCLC_R_DISABLED;
        return;
    }
    if (c->busy)
    {
        out->reason = TBT_FEC_ADAPT_SCLC_R_BUSY;
        return;
    }
    if (c->hold_until_ns && now_ns < c->hold_until_ns)
    {
        out->reason = TBT_FEC_ADAPT_SCLC_R_HOLD;
        return;
    }
    if (c->last_eval_ns)
    {
        if (now_ns < c->last_eval_ns)
        {
            /* A clock step must not replay a tick or poison the dwell. */
            c->last_eval_ns = now_ns;
            return;
        }
        if (now_ns - c->last_eval_ns < TBT_FEC_ADAPT_SCLC_EVAL_NS)
            return;
    }
    c->last_eval_ns = now_ns;

    if (o->pause_idle || o->pause_carrier || o->pause_abandoned)
    {
        /* A probe must observe live traffic: restart its window on resume. */
        if (c->probe_active)
            c->probe_start_ns = now_ns;
        c->up_run = 0;
        c->down_run = 0;
        c->fail_run = 0;
        c->probe_low_since_ns = 0;
        out->reason = TBT_FEC_ADAPT_SCLC_R_PAUSED;
        return;
    }
    if (!o->p_valid)
    {
        if (c->probe_active)
            c->probe_start_ns = now_ns;
        c->up_run = 0;
        c->down_run = 0;
        c->fail_run = 0;
        c->probe_low_since_ns = 0;
        out->reason = TBT_FEC_ADAPT_SCLC_R_WAIT;
        return;
    }

    p = o->p_ppm > 1000000u ? 1000000u : o->p_ppm;
    /* A new epoch's NACKs belong to the shape that just left; keep them out
     * of r_fail for one second. */
    r_valid = o->r_valid &&
              (!c->r_ignore_until_ns || now_ns >= c->r_ignore_until_ns);
    r = r_valid ? (o->r_fail_ppm > 1000000u ? 1000000u : o->r_fail_ppm) : 0u;
    /* A burst raises the target loss for this tick only; p_used may still be
     * dominated by the 4 s window. */
    p_eff = p;
    if (o->burst && o->p_fast_ppm > p_eff)
        p_eff = o->p_fast_ppm > 1000000u ? 1000000u : o->p_fast_ppm;
    target = tbt_fec_adapt_sclc_target(c, p_eff);
    /* A refused CFG must not be retried. The fallback applies only while
     * the fresh target points the same way: a refused raise must not force
     * a raise after the channel has turned worse. A refused raise waits out
     * its 30 s raise ban first, so its fallback window is twice as long. */
    if (c->reject_until_ns)
    {
        if (now_ns < c->reject_until_ns && target != c->cur &&
            c->reject_target != c->cur &&
            (target > c->cur) == (c->reject_target > c->cur))
            target = c->reject_target;
        else if (now_ns >= c->reject_until_ns)
            c->reject_until_ns = 0;
    }
    /* A refused lower rate means the peer does not build even lower shapes.
     * Stay at the current rate instead of walking down one NACK at a time. */
    if (c->no_down && target < c->cur)
        target = c->cur;
    /* A shape the peer refused is never suggested again. */
    if (c->refused_mask & (1u << target))
        target = c->cur;
    if (r_valid && r < TBT_FEC_ADAPT_SCLC_PROBE_LOW_PPM)
    {
        if (!c->probe_low_since_ns)
            c->probe_low_since_ns = now_ns;
    }
    else
        c->probe_low_since_ns = 0;

    if (c->rollback_pending)
    {
        c->rollback_pending = 0;
        tbt_fec_adapt_sclc_issue(c, c->commit_from,
                                 TBT_FEC_ADAPT_SCLC_R_ROLLBACK, 0, out);
        return;
    }

    if (c->probe_active)
    {
        if (r_valid && r > TBT_FEC_ADAPT_SCLC_PROBE_HIGH_PPM)
        {
            if (++c->probe_fail_run >= TBT_FEC_ADAPT_SCLC_FAIL_RUN)
            {
                tbt_fec_adapt_sclc_issue(c, c->probe_from,
                                         TBT_FEC_ADAPT_SCLC_R_PROBE_REVERT, 0,
                                         out);
                return;
            }
        }
        else
            c->probe_fail_run = 0;
        if (target < c->cur)
        {
            tbt_fec_adapt_sclc_issue(c, c->probe_from,
                                     TBT_FEC_ADAPT_SCLC_R_PROBE_ABORT, 0, out);
            return;
        }
        if (now_ns >= c->probe_start_ns &&
            now_ns - c->probe_start_ns >=
                TBT_FEC_ADAPT_SCLC_PROBE_OBSERVE_NS)
        {
            c->probe_active = 0;
            c->probe_from = c->cur;
            out->reason = TBT_FEC_ADAPT_SCLC_R_PROBE_COMMIT;
            return;
        }
        return;
    }

    /* The first valid sample may jump straight to the target: down always,
     * up only when the residual says the codec is comfortable and no earlier
     * failure banned raises. */
    if (!c->fast_start_done)
    {
        c->fast_start_done = 1;
        if (tbt_fec_adapt_sclc_switches_since(
                c, now_ns, TBT_FEC_ADAPT_SCLC_RATE_WIN_NS) <
            TBT_FEC_ADAPT_SCLC_RATE_MAX)
        {
            if (target < c->cur)
            {
                tbt_fec_adapt_sclc_issue(
                    c, target, TBT_FEC_ADAPT_SCLC_R_FAST_START, 0, out);
                return;
            }
            if (target > c->cur && now_ns >= c->no_raise_until &&
                (!r_valid || r < TBT_FEC_ADAPT_SCLC_PROBE_LOW_PPM))
            {
                tbt_fec_adapt_sclc_issue(
                    c, target, TBT_FEC_ADAPT_SCLC_R_FAST_START, 0, out);
                return;
            }
        }
    }

    /* Residual failures say the codec recovers less than the rate assumes.
     * Move one step down regardless of p, and ban raises for 30 s. */
    if (r_valid && r > TBT_FEC_ADAPT_SCLC_FAIL_HIGH_PPM)
    {
        if (++c->fail_run >= TBT_FEC_ADAPT_SCLC_FAIL_RUN)
        {
            c->no_raise_until = now_ns + TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS;
            if (c->cur > 0 && !c->no_down &&
                tbt_fec_adapt_sclc_can_switch(c, now_ns, 2))
            {
                u8 down = tbt_fec_adapt_sclc_fallback(c, c->cur);

                if (down != c->cur)
                    tbt_fec_adapt_sclc_issue(
                        c, down, TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN, 0, out);
            }
            return;
        }
    }
    else
        c->fail_run = 0;

    if (target > c->cur)
    {
        c->down_run = 0;
        if (now_ns < c->no_raise_until)
        {
            c->up_run = 0;
            return;
        }
        next = tbt_fec_adapt_sclc_next_rate(c, c->cur);
        if (next == c->cur)
        {
            c->up_run = 0;
            return;
        }
        boundary = tbt_fec_adapt_sclc_boundary(next);
        if (p_eff + TBT_FEC_ADAPT_SCLC_HYST_PPM <= boundary)
        {
            if (++c->up_run < TBT_FEC_ADAPT_SCLC_UP_RUN)
                return;
            if (!tbt_fec_adapt_sclc_can_switch(c, now_ns, 1))
                return;
            tbt_fec_adapt_sclc_issue(c, next, TBT_FEC_ADAPT_SCLC_R_UP, 0,
                                     out);
            return;
        }
        /* Inside the 2 pp up hysteresis. A very low residual may still buy
         * the step: after the switch p is at or below the new rate's
         * boundary, so the normal down path cannot immediately undo it. */
        if (p_eff <= TBT_FEC_ADAPT_SCLC_PROBE_MAX_P_PPM && r_valid &&
            r < TBT_FEC_ADAPT_SCLC_PROBE_LOW_PPM &&
            now_ns >= c->no_raise_until && now_ns >= c->no_probe_until &&
            c->probe_low_since_ns && now_ns >= c->probe_low_since_ns &&
            now_ns - c->probe_low_since_ns >=
                TBT_FEC_ADAPT_SCLC_PROBE_STABLE_NS &&
            tbt_fec_adapt_sclc_can_switch(c, now_ns, 1))
        {
            tbt_fec_adapt_sclc_issue(c, next,
                                     TBT_FEC_ADAPT_SCLC_R_PROBE_UP, 1, out);
            return;
        }
        c->up_run = 0;
        return;
    }

    if (target < c->cur)
    {
        c->up_run = 0;
        boundary = tbt_fec_adapt_sclc_boundary(c->cur);
        if (o->burst && tbt_fec_adapt_sclc_can_burst(c, now_ns))
        {
            tbt_fec_adapt_sclc_issue(c, target,
                                     TBT_FEC_ADAPT_SCLC_R_BURST, 0, out);
            return;
        }
        if (p_eff >= boundary + TBT_FEC_ADAPT_SCLC_HYST_PPM)
        {
            if (++c->down_run < TBT_FEC_ADAPT_SCLC_DOWN_RUN)
                return;
            if (!tbt_fec_adapt_sclc_can_switch(c, now_ns, 2))
                return;
            tbt_fec_adapt_sclc_issue(c, target,
                                     TBT_FEC_ADAPT_SCLC_R_DOWN, 0, out);
            return;
        }
        c->down_run = 0;
        return;
    }

    c->up_run = 0;
    c->down_run = 0;
    out->reason = TBT_FEC_ADAPT_SCLC_R_STEADY;
}

void tbt_fec_adapt_sclc_ctl_applied(struct tbt_fec_adapt_sclc_ctl *c,
                                    u64 now_ns)
{
    u8 old;
    u8 to;
    u8 reason;
    u8 probe;
    bool osc;

    if (!c->busy)
        return;
    old = c->cur;
    to = c->pending_to;
    reason = c->pending_reason;
    probe = c->pending_probe;
    c->busy = 0;

    if (to != old)
    {
        osc = c->last_switch_ns && now_ns >= c->last_switch_ns &&
              now_ns - c->last_switch_ns < TBT_FEC_ADAPT_SCLC_OSC_WIN_NS &&
              to == c->prev_idx;
        c->prev_idx = old;
        c->last_switch_ns = now_ns;
        c->last_dir = to > old ? 1 : 2;
        c->cur = to;
        c->dwell_mul = osc ? TBT_FEC_ADAPT_SCLC_DWELL_MUL : 1;
        c->sw[c->sw_head] = now_ns;
        c->sw_head = (c->sw_head + 1) % TBT_FEC_ADAPT_SCLC_RATE_MAX;
        if (c->sw_n < TBT_FEC_ADAPT_SCLC_RATE_MAX)
            c->sw_n++;
        if (tbt_fec_adapt_sclc_switches_since(c, now_ns,
                                              TBT_FEC_ADAPT_SCLC_HOLD_WIN_NS) >=
            TBT_FEC_ADAPT_SCLC_HOLD_COUNT)
            c->hold_until_ns = now_ns + TBT_FEC_ADAPT_SCLC_HOLD_NS;
        c->commit_from = old;
        c->commit_raise = to > old ? 1 : 0;
        c->commit_ns = now_ns;
        c->rollback_pending = 0;
        c->reject_until_ns = 0;
        c->probe_low_since_ns = 0;
        c->r_ignore_until_ns = now_ns + TBT_FEC_ADAPT_SCLC_SEC_NS;
        if (reason == TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN ||
            reason == TBT_FEC_ADAPT_SCLC_R_BURST ||
            reason == TBT_FEC_ADAPT_SCLC_R_ROLLBACK)
            c->no_raise_until = now_ns + TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS;
    }

    if (reason == TBT_FEC_ADAPT_SCLC_R_PROBE_REVERT)
    {
        c->no_probe_until = now_ns + TBT_FEC_ADAPT_SCLC_PROBE_BAN_NS;
        c->probe_active = 0;
        c->probe_from = c->cur;
    }
    else if (reason == TBT_FEC_ADAPT_SCLC_R_PROBE_ABORT)
    {
        c->probe_active = 0;
        c->probe_from = c->cur;
    }
    else if (probe)
    {
        c->probe_active = 1;
        c->probe_from = old;
        c->probe_start_ns = now_ns;
    }

    c->pending_to = c->cur;
    c->pending_reason = TBT_FEC_ADAPT_SCLC_R_NONE;
    c->pending_probe = 0;
    c->up_run = 0;
    c->down_run = 0;
    c->fail_run = 0;
    c->probe_fail_run = 0;
}

void tbt_fec_adapt_sclc_ctl_abort(struct tbt_fec_adapt_sclc_ctl *c)
{
    if (!c->busy)
        return;
    c->busy = 0;
    c->pending_to = c->cur;
    c->pending_reason = TBT_FEC_ADAPT_SCLC_R_NONE;
    c->pending_probe = 0;
    c->up_run = 0;
    c->down_run = 0;
    c->fail_run = 0;
    c->probe_fail_run = 0;
}

bool tbt_fec_adapt_sclc_commit_bad(
    const struct tbt_fec_adapt_sclc_est_out *o)
{
    if (!o)
        return false;
    if (o->r_valid && o->r_fail_ppm > TBT_FEC_ADAPT_SCLC_FAIL_HIGH_PPM)
        return true;
    return o->codec_fail_ppm > TBT_FEC_ADAPT_SCLC_CODEC_FAIL_PPM;
}

void tbt_fec_adapt_sclc_ctl_commit_failed(
    struct tbt_fec_adapt_sclc_ctl *c,
    const struct tbt_fec_adapt_sclc_est_out *o, u64 now_ns)
{
    if (!c->ready || !c->commit_raise)
        return;
    if (now_ns < c->commit_ns ||
        now_ns - c->commit_ns > TBT_FEC_ADAPT_SCLC_ROLLBACK_NS)
        return;
    if (c->cur == c->commit_from)
        return;
    if (!tbt_fec_adapt_sclc_commit_bad(o))
        return;
    if (tbt_fec_adapt_sclc_switches_since(c, now_ns,
                                          TBT_FEC_ADAPT_SCLC_RATE_WIN_NS) >=
        TBT_FEC_ADAPT_SCLC_RATE_MAX)
        return;
    c->rollback_pending = 1;
}

void tbt_fec_adapt_sclc_ctl_rejected(struct tbt_fec_adapt_sclc_ctl *c,
                                     u64 now_ns)
{
    u8 bad;

    if (!c->ready || !c->busy)
        return;
    bad = c->pending_to;
    c->busy = 0;
    c->pending_to = c->cur;
    c->pending_reason = TBT_FEC_ADAPT_SCLC_R_NONE;
    c->pending_probe = 0;
    /* Remember the refusal for the session: a later switch must not put the
     * same CFG on the wire after the peer said NACK. */
    c->refused_mask |= (u8)(1u << bad);
    if (bad > c->cur)
    {
        /* The refusal may be transient (the peer could not allocate the
         * codec), so retry one step lower after the raise ban. */
        c->reject_target = tbt_fec_adapt_sclc_fallback(c, bad);
        c->reject_until_ns =
            now_ns + 2 * TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS;
        c->no_raise_until = now_ns + TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS;
    }
    else
    {
        /* A refused lower rate says the peer does not support it: keep the
         * rate both ends have already used and stop walking down. */
        c->reject_target = c->cur;
        c->reject_until_ns = now_ns + TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS;
        c->no_down = 1;
    }
    c->up_run = 0;
    c->down_run = 0;
    c->fail_run = 0;
    c->probe_fail_run = 0;
}
