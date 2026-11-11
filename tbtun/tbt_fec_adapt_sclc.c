/* tbt_fec_adapt_sclc.c - SC-LDPC continuous rate control.
 *
 * See tbt_fec_adapt_sclc.h for the contract. This file is pure: no
 * allocation, no kernel globals, no wire access. It compiles into the module
 * and into host/test_fec_adapt_sclc.c unchanged.
 *
 * The control law has three parts, each with its own time scale:
 *
 *   q      Robbins-Monro quantile, gamma_q = min(1/16, pi/(8d)) per block;
 *   delta  analytic margin, gamma_delta = 1/64 per successful block;
 *   burst  EWMA_fast(E) - q, capped at n/4 and decayed by 7/8 per tick.
 *
 * The 16-block cold start collects E and seeds q with the empirical
 * (1-eps) quantile, because a constant-gamma quantile estimator cannot walk
 * down from a far-off initial guess: with eps ~ 1/256 its downward step is
 * gamma*eps, while its upward step is gamma*(1-eps). Starting near the
 * observed quantile keeps the loop honest; after that only the update moves
 * q. The output never subtracts a margin from E.
 */
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_sclc_delta.h"

/* ------------------------------------------------------------------ *
 * Buildable splits and the wire label
 * ------------------------------------------------------------------ */

static bool tbt_fec_adapt_sclc_up_has(u16 n)
{
    u32 i;

    for (i = 0; i < TBT_FEC_SCLC_UP_N; i++)
        if (tbt_fec_sclc_up_n[i] == n)
            return true;
    return false;
}

bool tbt_fec_adapt_sclc_buildable(u8 variant, u16 n, u16 m)
{
    u16 shares = (u16)(n + m);

    if (!n || !m)
        return false;
    if (variant == TBT_FEC_SCLC_DOWN)
        return shares == TBT_FEC_MAX_SHARES && n <= 192;
    if (variant == TBT_FEC_SCLC_UP)
        return shares == 32 && tbt_fec_adapt_sclc_up_has(n);
    return false;
}

u8 tbt_fec_adapt_sclc_rate_pct(u16 n, u16 m)
{
    u32 shares = (u32)n + m;
    u32 pct;

    if (!n || !shares)
        return 0;
    /* round-half-up(n*100/shares), at least 1. A label only. */
    pct = ((u32)n * 100u + shares / 2u) / shares;
    if (!pct)
        pct = 1;
    if (pct > 100u)
        pct = 100u;
    return (u8)pct;
}

int tbt_fec_adapt_sclc_wire_of_cfg(const struct tbt_fec_cfg *cfg,
                                  u16 carriers,
                                  struct tbt_fec_adapt_sclc_wire *out)
{
    u16 shares, block, L;
    u8 kind;
    u32 rate_ppm;

    if (!cfg || !out || !carriers || !cfg->n || !cfg->m || !cfg->groups)
        return -1;
    shares = (u16)(cfg->n + cfg->m);
    if (shares > TBT_FEC_MAX_SHARES ||
        (u32)cfg->groups * shares != carriers)
        return -1;
    if (cfg->codec_variant != 2)
        return -1;
    if (cfg->groups == 1 && shares == TBT_FEC_MAX_SHARES)
    {
        kind = TBT_FEC_SCLC_DOWN;
        if (cfg->block != TBT_FEC_SCLC_BLOCK_DOWN ||
            cfg->L != TBT_FEC_SCLC_L_DOWN)
            return -1;
    }
    else if (cfg->groups > 1 && shares == 32)
    {
        kind = TBT_FEC_SCLC_UP;
        if (cfg->block != TBT_FEC_SCLC_BLOCK_UP ||
            cfg->L != TBT_FEC_SCLC_L_UP)
            return -1;
    }
    else
        return -1;
    if (!tbt_fec_adapt_sclc_buildable(kind, cfg->n, cfg->m))
        return -1;
    rate_ppm = cfg->rate_ppm;
    if (rate_ppm)
    {
        /* The parser rounds up; accept the exact rate too. */
        if ((u32)(((u64)rate_ppm * shares) / 1000000u) != cfg->n)
            return -1;
    }
    else
    {
        rate_ppm = (u32)((((u64)cfg->n * 1000000u) + shares - 1u) / shares);
    }
    block = cfg->block;
    L = cfg->L;
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->symbol = kind == TBT_FEC_SCLC_DOWN ? 0 : 1;
    out->block = block;
    out->L = L;
    out->rate_pct = tbt_fec_adapt_sclc_rate_pct(cfg->n, cfg->m);
    out->rate_ppm = rate_ppm;
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
    u32 rate_ppm;

    if (!p || !cfg || !p->n || !p->m || !p->groups)
        return -1;
    if (!tbt_fec_adapt_sclc_buildable(p->kind, p->n, p->m))
        return -1;
    shares = (u16)(p->n + p->m);
    if ((u32)p->groups * shares > TBT_FEC_MAX_SHARES)
        return -1;
    rate_ppm = (u32)((((u64)p->n * 1000000u) + shares - 1u) / shares);
    if (p->rate_ppm &&
        (u32)(((u64)p->rate_ppm * shares) / 1000000u) != p->n)
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
    cfg->rate_ppm = rate_ppm;
    if (p->kind == TBT_FEC_SCLC_DOWN)
    {
        if (shares != TBT_FEC_MAX_SHARES ||
            p->block != TBT_FEC_SCLC_BLOCK_DOWN ||
            p->L != TBT_FEC_SCLC_L_DOWN)
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
 * Loss-rate estimator (diagnostic)
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

void tbt_fec_adapt_sclc_est_recompute(struct tbt_fec_adapt_sclc_est *e)
{
    u32 sent = 0, miss = 0;
    u8 i;

    if (!e)
        return;
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_BUCKETS; i++)
    {
        sent += e->win[i].sent;
        miss += e->win[i].miss;
    }
    e->p_ewma_ppm = sent ? (u32)((u64)miss * 1000000ull / sent) : 0;
}

static void tbt_fec_adapt_sclc_replay_ewma(struct tbt_fec_adapt_sclc_est *e);

bool tbt_fec_adapt_sclc_est_correct_miss(struct tbt_fec_adapt_sclc_est *e,
                                         u8 bucket)
{
    if (!e || bucket >= TBT_FEC_ADAPT_SCLC_BUCKETS ||
        !e->win[bucket].miss)
        return false;
    e->win[bucket].miss--;
    tbt_fec_adapt_sclc_est_recompute(e);
    return true;
}

bool tbt_fec_adapt_sclc_est_correct_super(struct tbt_fec_adapt_sclc_est *e,
                                          u64 super_id)
{
    u8 i;

    if (!e)
        return false;
    for (i = 0; i < e->ewma_n; i++)
    {
        struct tbt_fec_adapt_sclc_sample *smp =
            &e->ewma_ring[(e->ewma_head + i) %
                          TBT_FEC_ADAPT_SCLC_EWMA_RING];

        if (!smp->used || smp->super_id != super_id)
            continue;
        if (!smp->miss || smp->bucket >= TBT_FEC_ADAPT_SCLC_BUCKETS ||
            e->win[smp->bucket].start_ns != smp->bucket_start_ns ||
            !e->win[smp->bucket].miss)
            return false;
        smp->miss--;
        e->win[smp->bucket].miss--;
        tbt_fec_adapt_sclc_replay_ewma(e);
        return true;
    }
    return false;
}

static void tbt_fec_adapt_sclc_replay_ewma(struct tbt_fec_adapt_sclc_est *e)
{
    u32 p = e->ewma_anchor_ppm;
    u8 i;

    for (i = 0; i < e->ewma_n; i++)
    {
        struct tbt_fec_adapt_sclc_sample *smp =
            &e->ewma_ring[(e->ewma_head + i) %
                          TBT_FEC_ADAPT_SCLC_EWMA_RING];
        u32 p_block = smp->sent ?
                      (u32)((u64)smp->miss * 1000000ull / smp->sent) : 0;
        int delta = (int)p_block - (int)p;

        p = (u32)((int)p + delta / (1 << TBT_FEC_ADAPT_SCLC_EWMA_SHIFT));
        if (p > 1000000u)
            p = 1000000u;
    }
    e->p_ewma_ppm = p;
}

static void tbt_fec_adapt_sclc_push_sample(struct tbt_fec_adapt_sclc_est *e,
                                           u16 sent, u16 miss, u8 bucket)
{
    struct tbt_fec_adapt_sclc_sample *smp;

    if (e->ewma_n == TBT_FEC_ADAPT_SCLC_EWMA_RING)
    {
        smp = &e->ewma_ring[e->ewma_head];
        {
            u32 p_block = smp->sent ?
                          (u32)((u64)smp->miss * 1000000ull / smp->sent) : 0;
            int delta = (int)p_block - (int)e->ewma_anchor_ppm;

            e->ewma_anchor_ppm =
                (u32)((int)e->ewma_anchor_ppm +
                      delta / (1 << TBT_FEC_ADAPT_SCLC_EWMA_SHIFT));
        }
        e->ewma_head = (u8)((e->ewma_head + 1) %
                            TBT_FEC_ADAPT_SCLC_EWMA_RING);
        e->ewma_n--;
    }
    smp = &e->ewma_ring[(e->ewma_head + e->ewma_n) %
                        TBT_FEC_ADAPT_SCLC_EWMA_RING];
    smp->super_id = ++e->super_id;
    smp->bucket_start_ns = e->win[bucket].start_ns;
    smp->sent = sent;
    smp->miss = miss;
    smp->bucket = bucket;
    smp->used = true;
    e->ewma_n++;
    tbt_fec_adapt_sclc_replay_ewma(e);
}

void tbt_fec_adapt_sclc_est_super(struct tbt_fec_adapt_sclc_est *e,
                                  u64 now_ns)
{
    struct tbt_fec_adapt_sclc_win *w;

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

    tbt_fec_adapt_sclc_push_sample(e, (u16)(e->sb_sent > 0xffffu ?
                                            0xffffu : e->sb_sent),
                                   (u16)(e->sb_miss > 0xffffu ?
                                         0xffffu : e->sb_miss), e->cur);
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
         * is still above the 512-share floor at every buildable point. */
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
}

/* ------------------------------------------------------------------ *
 * Continuous controller
 * ------------------------------------------------------------------ */

/* Analytic downlink margin at n: the density-evolution delta. The table is
 * generated from review/calib/sclc-de.csv. */
static u32 tbt_fec_adapt_sclc_delta_down(u16 n)
{
    if (n < 1)
        n = 1;
    if (n > TBT_FEC_SCLC_DELTA_DOWN_N)
        n = TBT_FEC_SCLC_DELTA_DOWN_N;
    return tbt_fec_sclc_delta_down_q16[n - 1];
}

/* Analytic uplink margin at n: the conservative core bound. n must be in
 * the constructible set; init rejects anything else. */
static u32 tbt_fec_adapt_sclc_delta_up(u16 n)
{
    u32 i;

    for (i = 0; i < TBT_FEC_SCLC_UP_N; i++)
        if (tbt_fec_sclc_up_n[i] == n)
            return tbt_fec_sclc_up_delta_q16[i];
    return TBT_FEC_ADAPT_SCLC_ONE;
}

/* Finite-length term delta(eps) * 256 shares, linearly interpolated over the
 * generated eps grid. */
static u32 tbt_fec_adapt_sclc_len(u32 eps_q16)
{
    u32 ppm = (u32)(((u64)eps_q16 * 1000000u) >> 16);
    u32 i;

    if (ppm <= tbt_fec_sclc_len_eps_ppm[0])
        return tbt_fec_sclc_len_q16[0];
    for (i = 1; i < TBT_FEC_SCLC_LEN_N; i++)
    {
        u32 e0 = tbt_fec_sclc_len_eps_ppm[i - 1];
        u32 e1 = tbt_fec_sclc_len_eps_ppm[i];
        u32 v0 = tbt_fec_sclc_len_q16[i - 1];
        u32 v1 = tbt_fec_sclc_len_q16[i];
        long long dv = (long long)v1 - (long long)v0;

        if (ppm <= e1)
            return (u32)((long long)v0 +
                         (dv * (long long)(ppm - e0)) /
                             (long long)(e1 - e0));
    }
    return tbt_fec_sclc_len_q16[TBT_FEC_SCLC_LEN_N - 1];
}

/* eps = sigma / ((n+m)(1+w)), clamped to [1/(n+m), 5%]. All Q16.16. */
static u32 tbt_fec_adapt_sclc_eps(u32 sigma_q16, u16 shares, u32 w_ppm)
{
    u32 w_q16;
    u64 den;
    u32 eps;
    u32 lo;

    if (w_ppm > 10000000u)
        w_ppm = 10000000u;
    w_q16 = (u32)(((u64)w_ppm * TBT_FEC_ADAPT_SCLC_ONE + 500000u) /
                  1000000u);
    den = (u64)shares * (TBT_FEC_ADAPT_SCLC_ONE + w_q16);
    eps = den ? (u32)(((u64)sigma_q16 << 16) / den) : 0;
    lo = TBT_FEC_ADAPT_SCLC_ONE / shares;
    if (eps < lo)
        eps = lo;
    if (eps > TBT_FEC_ADAPT_SCLC_EPS_MAX_Q16)
        eps = TBT_FEC_ADAPT_SCLC_EPS_MAX_Q16;
    return eps;
}

/* gamma_q = min(1/16, pi/(8d)). pi/8 in Q16 is 25736. */
static u32 tbt_fec_adapt_sclc_gamma_q(u16 d)
{
    u32 g;

    if (!d)
        return TBT_FEC_ADAPT_SCLC_GAMMA_Q_Q16;
    g = 25736u / d;
    if (g > TBT_FEC_ADAPT_SCLC_GAMMA_Q_Q16)
        g = TBT_FEC_ADAPT_SCLC_GAMMA_Q_Q16;
    return g;
}

static u32 tbt_fec_adapt_sclc_popcount(u32 v)
{
    u32 n = 0;

    while (v)
    {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

static void tbt_fec_adapt_sclc_sort(u16 *v, u32 n)
{
    u32 i, j;

    for (i = 1; i < n; i++)
    {
        u16 x = v[i];

        j = i;
        while (j && v[j - 1] > x)
        {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* Analytic margin at the current n. The downlink adds the finite-length
 * term; the uplink uses the core bound alone. */
static u32 tbt_fec_adapt_sclc_delta_base(const struct tbt_fec_adapt_sclc_ctl *c,
                                         u16 n, u32 eps_q16)
{
    u32 base = c->variant == TBT_FEC_SCLC_UP
                   ? tbt_fec_adapt_sclc_delta_up(n)
                   : tbt_fec_adapt_sclc_delta_down(n);

    if (c->variant == TBT_FEC_SCLC_DOWN)
        base += tbt_fec_adapt_sclc_len(eps_q16);
    return base;
}

/* Lower bound of the online offset for a candidate base. The total
 * delta = base + off stays at or above one share uplink and base/2
 * downlink. */
static int tbt_fec_adapt_sclc_delta_lo(u8 variant, u32 base)
{
    if (variant == TBT_FEC_SCLC_UP)
        return (int)TBT_FEC_ADAPT_SCLC_ONE - (int)base;
    return -(int)(base / 2u);
}

/* Clamp the integrated offset at the current n and derive delta. The offset
 * is the only integrated state; delta0 is recomputed from n. */
static void tbt_fec_adapt_sclc_delta_clamp(
    struct tbt_fec_adapt_sclc_ctl *c)
{
    u32 base = tbt_fec_adapt_sclc_delta_base(c, c->n_cur, c->eps_q16);
    int lo = tbt_fec_adapt_sclc_delta_lo(c->variant, base);
    int off = c->delta_off_q16;

    if (off < lo)
        off = lo;
    if (off > (int)(8u << 16))
        off = (int)(8u << 16);
    c->delta0_q16 = base;
    c->delta_off_q16 = off;
    c->delta_q16 = (u32)((int)base + off);
}

/* 128-bit first-failure history, newest bit in bit 0 of [0]. */
static void tbt_fec_adapt_sclc_hist_push(u32 h[4], bool fail, u16 *count)
{
    int i;

    for (i = 3; i > 0; i--)
        h[i] = (h[i] << 1) | (h[i - 1] >> 31);
    h[0] = (h[0] << 1) | (fail ? 1u : 0u);
    if (*count < TBT_FEC_ADAPT_SCLC_BACKUP_MAX)
        (*count)++;
}

static u32 tbt_fec_adapt_sclc_hist_count(const u32 h[4], u32 win)
{
    u32 full = win / 32u;
    u32 rem = win % 32u;
    u32 n = 0;
    u32 i;

    for (i = 0; i < full; i++)
        n += tbt_fec_adapt_sclc_popcount(h[i]);
    if (rem)
        n += tbt_fec_adapt_sclc_popcount(h[full] & ((1u << rem) - 1u));
    return n;
}

/* max(32, ceil(1/(2*eps))), capped by the 128-bit history. */
static u32 tbt_fec_adapt_sclc_backup_win(u32 eps_q16)
{
    u32 win = (32768u + eps_q16 - 1u) / eps_q16;

    if (win < TBT_FEC_ADAPT_SCLC_BACKUP_WIN)
        win = TBT_FEC_ADAPT_SCLC_BACKUP_WIN;
    if (win > TBT_FEC_ADAPT_SCLC_BACKUP_MAX)
        win = TBT_FEC_ADAPT_SCLC_BACKUP_MAX;
    return win;
}

/* The 16 cold samples seed q with the empirical (1-eps) quantile when that
 * quantile is resolvable, and with m_init - delta0 otherwise. Sixteen
 * samples cannot resolve a quantile below 1/16, and using the sample maximum
 * there would pull q down and m to its floor. Sigma comes from the median
 * absolute deviation. This is the only bootstrap; every later q change is
 * the Robbins-Monro update. */
static void tbt_fec_adapt_sclc_cold_seed(struct tbt_fec_adapt_sclc_ctl *c)
{
    u16 v[TBT_FEC_ADAPT_SCLC_COLD_BLOCKS];
    u32 mad_sum = 0;
    u32 mad, sigma, eps, rank, q_emp, q_init, mean = 0;
    bool enough;
    u16 med;
    u32 i;

    memcpy(v, c->cold_e, sizeof(v));
    tbt_fec_adapt_sclc_sort(v, TBT_FEC_ADAPT_SCLC_COLD_BLOCKS);
    med = v[TBT_FEC_ADAPT_SCLC_COLD_BLOCKS / 2];
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_COLD_BLOCKS; i++)
        mad_sum += v[i] > med ? v[i] - med : med - v[i];
    mad = (mad_sum + TBT_FEC_ADAPT_SCLC_COLD_BLOCKS / 2) /
          TBT_FEC_ADAPT_SCLC_COLD_BLOCKS;
    if (!mad)
        mad = 1;
    sigma = mad << 16;
    eps = tbt_fec_adapt_sclc_eps(sigma, c->shares, c->w_ppm);
    rank = (u32)(((u64)(TBT_FEC_ADAPT_SCLC_ONE - eps) *
                      TBT_FEC_ADAPT_SCLC_COLD_BLOCKS +
                  TBT_FEC_ADAPT_SCLC_ONE - 1) >>
                 16);
    if (!rank)
        rank = 1;
    if (rank > TBT_FEC_ADAPT_SCLC_COLD_BLOCKS)
        rank = TBT_FEC_ADAPT_SCLC_COLD_BLOCKS;
    q_emp = (u32)v[rank - 1] << 16;
    if (c->m_cur > (c->delta0_q16 >> 16))
        q_init = (u32)(c->m_cur - (c->delta0_q16 >> 16)) << 16;
    else
        q_init = (u32)c->extra << 16;
    enough = (u64)TBT_FEC_ADAPT_SCLC_COLD_BLOCKS * eps >=
             TBT_FEC_ADAPT_SCLC_ONE;
    c->q_q16 = (enough && q_emp > q_init) ? q_emp : q_init;
    if (c->q_q16 > ((u32)c->shares << 16))
        c->q_q16 = (u32)c->shares << 16;
    c->q_act_q16 = c->q_q16;
    c->sigma_q16 = sigma;
    c->eps_q16 = eps;
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_COLD_BLOCKS; i++)
        mean += v[i];
    mean = (mean + TBT_FEC_ADAPT_SCLC_COLD_BLOCKS / 2) /
           TBT_FEC_ADAPT_SCLC_COLD_BLOCKS;
    c->ewma_fast_q16 = mean << 16;
    c->ewma_slow_q16 = mean << 16;
    c->blocks_tick = 0;
    c->cold_done = 1;
    c->fail_run = 0;
    memset(c->fail_hist, 0, sizeof(c->fail_hist));
    c->fail_hist_n = 0;
}

int tbt_fec_adapt_sclc_init(struct tbt_fec_adapt_sclc_ctl *c, u16 shares,
                            u16 groups, u16 extra, u16 n_init, u8 variant)
{
    u16 m_init;
    u16 n_max;
    u32 delta0;

    if (!c)
        return -1;
    memset(c, 0, sizeof(*c));
    if (!shares || shares > TBT_FEC_MAX_SHARES || !groups)
        return -1;
    if ((u32)groups * shares > TBT_FEC_MAX_SHARES)
        return -1;
    if (variant == TBT_FEC_SCLC_DOWN)
    {
        if (shares != TBT_FEC_MAX_SHARES || groups != 1)
            return -1;
        n_max = 192;
    }
    else if (variant == TBT_FEC_SCLC_UP)
    {
        if (shares != 32 || groups < 2)
            return -1;
        n_max = 30;
    }
    else
        return -1;
    if (!n_init || n_init > n_max || extra >= shares)
        return -1;
    m_init = (u16)(shares - n_init);
    if (!tbt_fec_adapt_sclc_buildable(variant, n_init, m_init))
        return -1;

    c->ready = 1;
    c->variant = variant;
    c->shares = shares;
    c->groups = groups;
    c->extra = extra;
    c->n_cur = n_init;
    c->m_cur = m_init;
    c->m_min = (u16)(shares - n_max);
    c->m_max = (u16)(shares - 1);
    c->w_ppm = 1000000u;
    c->sigma_q16 = 2u * TBT_FEC_ADAPT_SCLC_ONE;
    c->eps_q16 = tbt_fec_adapt_sclc_eps(c->sigma_q16, shares, c->w_ppm);
    delta0 = tbt_fec_adapt_sclc_delta_base(c, n_init, c->eps_q16);
    c->delta0_q16 = delta0;
    c->delta_off_q16 = 0;
    tbt_fec_adapt_sclc_delta_clamp(c);
    if (m_init > (c->delta_q16 >> 16))
        c->q_q16 = (u32)(m_init - (c->delta_q16 >> 16)) << 16;
    else
        c->q_q16 = (u32)extra << 16;
    if (c->q_q16 > ((u32)shares << 16))
        c->q_q16 = (u32)shares << 16;
    c->q_act_q16 = c->q_q16;
    c->ewma_fast_q16 = (u32)extra << 16;
    c->ewma_slow_q16 = (u32)extra << 16;
    return 0;
}

void tbt_fec_adapt_sclc_block_sample(struct tbt_fec_adapt_sclc_ctl *c,
                                     u16 E, u16 S, bool first_ok, u16 real)
{
    u32 e;
    u32 gamma;

    if (!c || !c->ready || !real)
        return;
    if (c->frozen)
    {
        /* The tick contract freezes q, delta, the failure window and the
         * backup. Only the abandoned-share denominator moves. */
        c->delivered += real;
        return;
    }
    if ((u32)real * 2 < c->n_cur)
    {
        c->delivered += real;
        return;
    }
    if (E > real)
        E = real;
    if (S > c->shares)
        S = c->shares;
    c->delivered += real;
    c->blocks++;

    if (!c->cold_done)
    {
        if (c->cold_n < TBT_FEC_ADAPT_SCLC_COLD_BLOCKS)
            c->cold_e[c->cold_n++] = E;
        if (c->cold_n == TBT_FEC_ADAPT_SCLC_COLD_BLOCKS)
            tbt_fec_adapt_sclc_cold_seed(c);
        return;
    }

    if (c->blocks_tick < 0xffffu)
        c->blocks_tick++;
    e = (u32)E << 16;
    gamma = tbt_fec_adapt_sclc_gamma_q(c->d);
    {
        u32 ind = e > c->q_q16 ? TBT_FEC_ADAPT_SCLC_ONE : 0u;
        long long step = ((long long)gamma *
                          ((long long)ind - (long long)c->eps_q16)) >>
                         16;
        long long q;

        /* Do not wind q up while the actuator is pinned at m_max. */
        if (!(c->sat_max && ind))
        {
            q = (long long)c->q_q16 + step;
            if (q < 0)
                q = 0;
            if (q > ((long long)c->shares << 16))
                q = (long long)c->shares << 16;
            c->q_q16 = (u32)q;
        }
    }
    {
        /* Scale around the slow mean, not around q: a deliberately
         * conservative q seed must not inflate sigma and eps. */
        u32 diff = e > c->ewma_slow_q16 ? e - c->ewma_slow_q16
                                        : c->ewma_slow_q16 - e;
        int ds = (int)diff - (int)c->sigma_q16;

        c->sigma_q16 = (u32)((int)c->sigma_q16 + ds / 16);
        if (c->sigma_q16 < TBT_FEC_ADAPT_SCLC_SIGMA_MIN_Q16)
            c->sigma_q16 = TBT_FEC_ADAPT_SCLC_SIGMA_MIN_Q16;
    }
    {
        int df = (int)e - (int)c->ewma_fast_q16;
        int ds = (int)e - (int)c->ewma_slow_q16;

        c->ewma_fast_q16 = (u32)((int)c->ewma_fast_q16 + df / 4);
        c->ewma_slow_q16 = (u32)((int)c->ewma_slow_q16 + ds / 64);
    }

    if (first_ok)
    {
        int err = (int)(c->extra + 1u) - (int)S;
        bool grow = err > 0;

        if (err > 4)
            err = 4;
        if (err < -4)
            err = -4;
        if (!(grow && c->sat_max) && !(!grow && c->sat_min))
        {
            c->delta_off_q16 += err * 1024;
            tbt_fec_adapt_sclc_delta_clamp(c);
        }
    }

    tbt_fec_adapt_sclc_hist_push(c->fail_hist, !first_ok, &c->fail_hist_n);
    if (first_ok)
        c->fail_run = 0;
    else if (c->fail_run < 0xffffu)
        c->fail_run++;

    {
        u32 win = tbt_fec_adapt_sclc_backup_win(c->eps_q16);
        u32 fails = tbt_fec_adapt_sclc_hist_count(c->fail_hist, win);
        bool trip = c->fail_run >= TBT_FEC_ADAPT_SCLC_BACKUP_RUN ||
                    (c->fail_hist_n >= win &&
                     (fails << 16) > (u32)(2u * win) * c->eps_q16);

        if (trip)
        {
            u32 add = c->n_cur / 16u;

            if (add < 2)
                add = 2;
            if ((u32)c->backup_off + add > c->shares)
                c->backup_off = c->shares;
            else
                c->backup_off = (u16)(c->backup_off + add);
            c->freeze_ticks = 1;
            c->fail_run = 0;
            memset(c->fail_hist, 0, sizeof(c->fail_hist));
            c->fail_hist_n = 0;
        }
    }
}

static void tbt_fec_adapt_sclc_fill_out(
    const struct tbt_fec_adapt_sclc_ctl *c, struct tbt_fec_adapt_sclc_out *out,
    u8 reason)
{
    out->n_target = c->n_cur;
    out->m_target = c->m_cur;
    out->m_req = c->m_cur;
    out->q_q16 = c->q_act_q16;
    out->q_raw_q16 = c->q_q16;
    out->delta_q16 = c->delta_q16;
    out->burst_q16 = c->burst_q16;
    out->eps_q16 = c->eps_q16;
    out->sigma_q16 = c->sigma_q16;
    out->backup = c->backup_off;
    out->delta_off_q16 = c->delta_off_q16;
    out->reason = reason;
    out->frozen = c->frozen;
}

void tbt_fec_adapt_sclc_tick(struct tbt_fec_adapt_sclc_ctl *c, u64 now_ns,
                             u16 d, u32 w_ppm, u16 alive, u16 stalled,
                             u32 abandoned,
                             struct tbt_fec_adapt_sclc_out *out)
{
    u32 total;
    u32 m_req;
    u16 k;
    u16 j;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!c || !c->ready)
    {
        out->reason = TBT_FEC_ADAPT_SCLC_R_NONE;
        return;
    }
    total = (u32)c->groups * c->shares;
    c->last_tick_ns = now_ns;
    c->d = d;
    c->w_ppm = w_ppm;
    c->frozen = (alive == 0) ||
                (total && (u32)stalled * 10u > total) ||
                (abandoned && (u64)abandoned * 100u > c->delivered)
                    ? 1
                    : 0;
    k = c->blocks_tick;
    c->blocks_tick = 0;

    if (c->frozen)
    {
        tbt_fec_adapt_sclc_fill_out(c, out, TBT_FEC_ADAPT_SCLC_R_PAUSED);
        c->delivered = 0;
        return;
    }

    if (c->blocks <= TBT_FEC_ADAPT_SCLC_COLD_BLOCKS)
    {
        tbt_fec_adapt_sclc_fill_out(c, out, TBT_FEC_ADAPT_SCLC_R_COLD);
        c->delivered = 0;
        return;
    }

    c->backup_hold = c->freeze_ticks ? 1 : 0;
    if (c->freeze_ticks)
        c->freeze_ticks--;

    /* A safety backup holds the loop for one feedback period: q, delta and
     * the feedforward keep their last values; only the backup offset moves
     * the output. */
    if (!c->backup_hold)
    {
        c->eps_q16 =
            tbt_fec_adapt_sclc_eps(c->sigma_q16, c->shares, c->w_ppm);
        tbt_fec_adapt_sclc_delta_clamp(c);
        for (j = 0; j < k && j < 64u; j++)
        {
            int dq = (int)c->q_q16 - (int)c->q_act_q16;

            c->q_act_q16 = (u32)((int)c->q_act_q16 + dq / 4);
        }
        {
            u32 raw = c->ewma_fast_q16 > c->ewma_slow_q16
                          ? c->ewma_fast_q16 - c->ewma_slow_q16
                          : 0u;
            u32 b = c->burst_q16;
            u32 cap = ((u32)c->n_cur << 16) / 4u;

            for (j = 0; j < k && j < 64u; j++)
                b -= b >> 3;
            if (raw > b)
                b = raw;
            if (b < TBT_FEC_ADAPT_SCLC_ONE / 8u)
                b = 0;
            if (b > cap)
                b = cap;
            c->burst_q16 = b;
        }
        if (alive < total)
        {
            u32 bc = (u32)((((u64)(total - alive) * c->n_cur) << 16) +
                           total - 1u) /
                     total;

            /* Physical estimate, not a burst: a dead carrier removes its
             * share of the block, so this term is not capped at n/4. */
            if (bc > c->burst_q16)
                c->burst_q16 = bc;
        }
    }

    {
        /* Least fixed point: the smallest buildable m whose own n gives
         * m >= q_act + base(n) + off + burst + backup. The base grows as n
         * falls, so evaluating it at the candidate, not at the previous n,
         * is what adds the analytic margin when n moves. */
        u64 qb = (u64)c->q_act_q16 + c->burst_q16 +
                 ((u64)c->backup_off << 16);
        u32 best_m = 0, best_n = 0, best_base = 0, best_delta = 0;
        u32 best_rhs = 0;
        int best_off = 0;

        if (c->variant == TBT_FEC_SCLC_DOWN)
        {
            u32 m;

            for (m = c->m_min; m <= c->m_max; m++)
            {
                u32 n = (u32)c->shares - m;
                u32 base = tbt_fec_adapt_sclc_delta_base(
                    c, (u16)n, c->eps_q16);
                int lo = tbt_fec_adapt_sclc_delta_lo(c->variant, base);
                int off = c->delta_off_q16;
                u32 delta;
                u32 rhs;

                if (off < lo)
                    off = lo;
                delta = (u32)((int)base + off);
                rhs = (u32)(qb + delta);
                if ((u64)m << 16 >= rhs)
                {
                    best_m = m;
                    best_n = n;
                    best_base = base;
                    best_off = off;
                    best_delta = delta;
                    best_rhs = rhs;
                    break;
                }
            }
        }
        else
        {
            u32 i;

            for (i = TBT_FEC_SCLC_UP_N; i-- > 0;)
            {
                u32 n = tbt_fec_sclc_up_n[i];
                u32 m = (u32)c->shares - n;
                u32 base = tbt_fec_adapt_sclc_delta_base(
                    c, (u16)n, c->eps_q16);
                int lo = tbt_fec_adapt_sclc_delta_lo(c->variant, base);
                int off = c->delta_off_q16;
                u32 delta;
                u32 rhs;

                if (off < lo)
                    off = lo;
                delta = (u32)((int)base + off);
                rhs = (u32)(qb + delta);
                if ((u64)m << 16 >= rhs)
                {
                    best_m = m;
                    best_n = n;
                    best_base = base;
                    best_off = off;
                    best_delta = delta;
                    best_rhs = rhs;
                    break;
                }
            }
        }
        if (!best_m)
        {
            int lo;
            int off;

            best_m = c->m_max;
            best_n = (u32)c->shares - best_m;
            best_base = tbt_fec_adapt_sclc_delta_base(
                c, (u16)best_n, c->eps_q16);
            lo = tbt_fec_adapt_sclc_delta_lo(c->variant, best_base);
            off = c->delta_off_q16;
            if (off < lo)
                off = lo;
            best_off = off;
            best_delta = (u32)((int)best_base + off);
            best_rhs = (u32)(qb + best_delta);
        }
        c->delta0_q16 = best_base;
        c->delta_off_q16 = best_off;
        c->delta_q16 = best_delta;
        c->n_cur = (u16)best_n;
        c->m_cur = (u16)best_m;
        m_req = (best_rhs + 32768u) >> 16;
        c->sat_max = best_m >= c->m_max ? 1 : 0;
        c->sat_min = best_m <= c->m_min ? 1 : 0;
    }
    tbt_fec_adapt_sclc_fill_out(
        c, out,
        c->backup_hold
            ? TBT_FEC_ADAPT_SCLC_R_BACKUP
            : (c->burst_q16 >= TBT_FEC_ADAPT_SCLC_ONE / 2
                   ? TBT_FEC_ADAPT_SCLC_R_BURST
                   : ((c->sat_max || c->sat_min)
                          ? TBT_FEC_ADAPT_SCLC_R_SATURATED
                          : TBT_FEC_ADAPT_SCLC_R_STEADY)));
    out->m_req = (u16)m_req;
    if (!c->backup_hold && c->backup_off)
        c->backup_off--;
    c->delivered = 0;
}
