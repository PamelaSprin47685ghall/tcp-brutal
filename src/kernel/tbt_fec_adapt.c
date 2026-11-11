/* tbt_fec_adapt.c - adaptive FEC estimator and n/m control law.
 *
 * See tbt_fec_adapt.h for the contract. This file is pure: no allocation,
 * no kernel globals, no wire access. It compiles into the module and into
 * tests/unit/test_fec_adapt.c unchanged.
 */
#ifdef __KERNEL__
#include <linux/bitops.h>
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "tbt_fec_adapt.h"

/* ------------------------------------------------------------------ *
 * Estimator
 * ------------------------------------------------------------------ */

static u32 tbt_fec_est_popcount(u32 v)
{
#ifdef __KERNEL__
    return (u32)hweight32(v);
#else
    return (u32)__builtin_popcount(v);
#endif
}

static u32 tbt_fec_est_u32(u64 v)
{
    return v > 0xffffffffull ? 0xffffffffu : (u32)v;
}

/* Rotate the 500 ms bucket pair. A gap shorter than one bucket moves to the
 * other bucket; a longer gap starts a fresh window, so stale samples never
 * outlive their second. */
static void tbt_fec_est_rotate(struct tbt_fec_est *e, u64 now_ns)
{
    struct tbt_fec_est_win *b = &e->win[e->cur];
    u64 elapsed;

    if (!b->start_ns)
    {
        b->start_ns = now_ns;
        return;
    }
    if (now_ns < b->start_ns)
    {
        memset(e->win, 0, sizeof(e->win));
        e->cur = 0;
        e->win[0].start_ns = now_ns;
        return;
    }
    elapsed = now_ns - b->start_ns;
    if (elapsed < TBT_FEC_EST_WIN_NS)
        return;
    if (elapsed < 2 * TBT_FEC_EST_WIN_NS)
    {
        e->cur ^= 1;
        memset(&e->win[e->cur], 0, sizeof(e->win[e->cur]));
        e->win[e->cur].start_ns = b->start_ns + TBT_FEC_EST_WIN_NS;
        return;
    }
    memset(e->win, 0, sizeof(e->win));
    e->cur = 0;
    e->win[0].start_ns = now_ns;
}

void tbt_fec_est_init(struct tbt_fec_est *e, u16 shares)
{
    memset(e, 0, sizeof(*e));
    if (!shares || shares > TBT_FEC_ADAPT_CARRIERS)
        shares = TBT_FEC_ADAPT_SHARES;
    e->shares = shares;
}

void tbt_fec_est_carriers(struct tbt_fec_est *e, u16 alive, u16 stalled,
                          u16 total)
{
    e->alive = alive;
    e->stalled = stalled;
    e->carriers = total;
}

void tbt_fec_est_block(struct tbt_fec_est *e, u64 now_ns, u16 group, u16 real,
                       u32 miss_mask,
                       const u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES])
{
    u32 base;
    u32 live = 0;
    u32 mask;
    u16 i;

    if (real > 32)
        real = 32;
    base = (u32)group * e->shares;
    for (i = 0; i < real; i++)
    {
        u32 carrier = base + i;

        if (carrier < TBT_FEC_ADAPT_CARRIERS &&
            tbt_fec_adapt_bitmap_test(alive, (u16)carrier))
            live |= 1u << i;
    }
    mask = real >= 32 ? 0xffffffffu : ((1u << real) - 1u);
    live &= mask;
    e->sb_sent += tbt_fec_est_popcount(live);
    e->sb_miss += tbt_fec_est_popcount(miss_mask & live);
    e->last_activity_ns = now_ns;
}

void tbt_fec_est_super(struct tbt_fec_est *e, u64 now_ns)
{
    struct tbt_fec_est_win *w;
    int delta;
    u32 p_block;

    e->last_activity_ns = now_ns;
    tbt_fec_est_rotate(e, now_ns);
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
    e->p_ewma_ppm = (u32)((int)e->p_ewma_ppm + delta / 8);
    if (e->p_ewma_ppm > 1000000u)
        e->p_ewma_ppm = 1000000u;

    e->sb_sent = 0;
    e->sb_miss = 0;
}

void tbt_fec_est_unknown(struct tbt_fec_est *e, u64 now_ns)
{
    e->last_activity_ns = now_ns;
    tbt_fec_est_rotate(e, now_ns);
    e->win[e->cur].unknown++;
    e->unknown_total++;
}

void tbt_fec_est_abandoned(struct tbt_fec_est *e, u64 now_ns, u32 shares)
{
    e->last_activity_ns = now_ns;
    tbt_fec_est_rotate(e, now_ns);
    e->win[e->cur].abandoned += shares;
    e->abandoned_total += shares;
}

void tbt_fec_est_fail(struct tbt_fec_est *e, u64 now_ns, u32 requested,
                      u32 pkts)
{
    tbt_fec_est_rotate(e, now_ns);
    e->win[e->cur].requested += requested;
    e->win[e->cur].pkts += pkts;
}

void tbt_fec_est_sample(const struct tbt_fec_est *e, u64 now_ns,
                        struct tbt_fec_est_out *o)
{
    u64 sent = 0;
    u64 miss = 0;
    u64 supers = 0;
    u64 requested = 0;
    u64 pkts = 0;
    u64 abandoned = 0;
    u64 unknown = 0;
    u32 i;

    memset(o, 0, sizeof(*o));
    for (i = 0; i < 2; i++)
    {
        const struct tbt_fec_est_win *w = &e->win[i];

        if (!w->start_ns || now_ns < w->start_ns)
            continue;
        /* A bucket is live while it overlaps the last second: its start is
         * at most one window (two buckets) old. */
        if (now_ns - w->start_ns > 2 * TBT_FEC_EST_WIN_NS)
            continue;
        sent += w->sent;
        miss += w->miss;
        supers += w->supers;
        requested += w->requested;
        pkts += w->pkts;
        abandoned += w->abandoned;
        unknown += w->unknown;
    }

    o->sent_win = tbt_fec_est_u32(sent);
    o->miss_win = tbt_fec_est_u32(miss);
    o->supers_win = tbt_fec_est_u32(supers);
    o->abandoned_win = tbt_fec_est_u32(abandoned);
    o->unknown_win = tbt_fec_est_u32(unknown);
    o->requested_win = tbt_fec_est_u32(requested);
    o->pkts_win = tbt_fec_est_u32(pkts);
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
    o->p_ewma_ppm = e->p_ewma_ppm;
    o->p_ppm = o->p_win_ppm > o->p_ewma_ppm ? o->p_win_ppm : o->p_ewma_ppm;
    if (pkts)
    {
        u64 r = requested * 1000000ull / pkts;

        o->r_fail_ppm = r > 1000000ull ? 1000000u : (u32)r;
    }
    o->p_valid = sent >= TBT_FEC_EST_MIN_SHARES && supers >= TBT_FEC_EST_MIN_SUPERS;
    o->r_valid = pkts > 0;
    o->pause_idle = !e->last_activity_ns ||
                    now_ns - e->last_activity_ns > TBT_FEC_EST_IDLE_NS;
    o->pause_carrier =
        tbt_fec_adapt_carrier_pause(e->alive, e->stalled, e->carriers);
    o->pause_abandoned = abandoned && (!sent || abandoned * 100 > sent);
}

/* ------------------------------------------------------------------ *
 * Control law
 * ------------------------------------------------------------------ */

static const struct
{
    u16 n;
    u16 m;
} tbt_fec_adapt_tbl[TBT_FEC_ADAPT_CLASSES] = {
    {7, 1}, {6, 2}, {5, 3}, {4, 4}, {3, 5}, {2, 6},
};

u8 tbt_fec_adapt_class_of_p(u32 p_ppm)
{
    if (p_ppm <= 10000u)
        return 0;
    if (p_ppm <= 30000u)
        return 1;
    if (p_ppm <= 50000u)
        return 2;
    if (p_ppm <= 100000u)
        return 3;
    if (p_ppm <= 200000u)
        return 4;
    return 5;
}

u8 tbt_fec_adapt_class_of_shape(u16 n, u16 m)
{
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_CLASSES; i++)
        if (tbt_fec_adapt_tbl[i].n == n && tbt_fec_adapt_tbl[i].m == m)
            return i;
    return 0xff;
}

void tbt_fec_adapt_shape(u8 cls, struct tbt_fec_shape *s)
{
    if (cls >= TBT_FEC_ADAPT_CLASSES)
        cls = 3;
    memset(s, 0, sizeof(*s));
    s->n = tbt_fec_adapt_tbl[cls].n;
    s->m = tbt_fec_adapt_tbl[cls].m;
    s->groups = TBT_FEC_ADAPT_GROUPS;
    s->w = (u16)(2 * s->n);
    s->k = s->n;
    s->d = (u8)s->n;
}

void tbt_fec_adapt_init(struct tbt_fec_adapt *a, u8 cls)
{
    memset(a, 0, sizeof(*a));
    if (cls >= TBT_FEC_ADAPT_CLASSES)
        cls = 3;
    a->cur = cls;
    a->prev_class = cls;
    a->probe_from = cls;
    a->dwell_mul = 1;
}

/* Nominal upper and lower p boundary of a class, ppm. */
static u32 tbt_fec_adapt_up_threshold(u8 cls)
{
    static const u32 t[TBT_FEC_ADAPT_CLASSES] = {10000u, 30000u, 50000u,
                                                 100000u, 200000u, 200000u};

    return t[cls];
}

static u32 tbt_fec_adapt_down_threshold(u8 cls)
{
    static const u32 t[TBT_FEC_ADAPT_CLASSES] = {0u, 10000u, 30000u, 50000u,
                                                 100000u, 200000u};

    return t[cls];
}

static u32 tbt_fec_adapt_switches_since(const struct tbt_fec_adapt *a,
                                        u64 now_ns, u64 win_ns)
{
    u32 i;
    u32 n = 0;

    for (i = 0; i < a->sw_n; i++)
        if (now_ns >= a->sw[i] && now_ns - a->sw[i] < win_ns)
            n++;
    return n;
}

/* The dwell grows after an oscillation, up to the cap. `dir` is 1 up, 2
 * down. */
static u64 tbt_fec_adapt_dwell(const struct tbt_fec_adapt *a, u8 dir)
{
    u64 d = dir == 1 ? TBT_FEC_ADAPT_UP_DWELL_NS
                     : TBT_FEC_ADAPT_DOWN_DWELL_NS;

    d *= a->dwell_mul;
    if (d > TBT_FEC_ADAPT_DWELL_CAP_NS)
        d = TBT_FEC_ADAPT_DWELL_CAP_NS;
    return d;
}

static bool tbt_fec_adapt_can_switch(const struct tbt_fec_adapt *a, u64 now_ns,
                                     u8 dir)
{
    u64 since;

    if (a->last_switch_ns)
    {
        if (now_ns < a->last_switch_ns)
            return false;
        since = now_ns - a->last_switch_ns;
        if (since < TBT_FEC_ADAPT_MIN_GAP_NS)
            return false;
        if (since < tbt_fec_adapt_dwell(a, dir))
            return false;
        if (a->last_dir && dir != a->last_dir &&
            since < TBT_FEC_ADAPT_REVERSE_NS)
            return false;
    }
    if (tbt_fec_adapt_switches_since(a, now_ns, TBT_FEC_ADAPT_RATE_WIN_NS) >=
        TBT_FEC_ADAPT_RATE_MAX)
        return false;
    return true;
}

static void tbt_fec_adapt_issue(struct tbt_fec_adapt *a, u8 to, u8 reason,
                                u8 probe, struct tbt_fec_adapt_out *out)
{
    a->busy = 1;
    a->pending_to = to;
    a->pending_probe = probe;
    a->pending_reason = reason;
    a->epoch++;
    out->switch_now = 1;
    out->from = a->cur;
    out->to = to;
    out->reason = reason;
    out->epoch = a->epoch;
    tbt_fec_adapt_shape(to, &out->shape);
}

void tbt_fec_adapt_eval(struct tbt_fec_adapt *a,
                        const struct tbt_fec_est_out *o, u64 now_ns,
                        struct tbt_fec_adapt_out *out)
{
    u32 p;
    u32 r;
    u8 target;

    memset(out, 0, sizeof(*out));
    out->from = a->cur;
    out->to = a->cur;
    out->epoch = a->epoch;
    out->reason = TBT_FEC_ADAPT_R_NONE;
    tbt_fec_adapt_shape(a->cur, &out->shape);

    if (a->busy)
    {
        out->reason = TBT_FEC_ADAPT_R_BUSY;
        return;
    }
    if (a->hold_until_ns && now_ns < a->hold_until_ns)
    {
        out->reason = TBT_FEC_ADAPT_R_HOLD;
        return;
    }
    if (a->last_eval_ns && now_ns >= a->last_eval_ns &&
        now_ns - a->last_eval_ns < TBT_FEC_ADAPT_EVAL_NS)
        return;
    a->last_eval_ns = now_ns;

    if (o->pause_idle || o->pause_carrier || o->pause_abandoned)
    {
        /* A probe must observe live traffic: restart its window on resume. */
        if (a->probe_active)
            a->probe_start_ns = now_ns;
        a->up_run = 0;
        a->down_run = 0;
        a->fail_run = 0;
        out->reason = TBT_FEC_ADAPT_R_PAUSED;
        return;
    }
    if (!o->p_valid)
    {
        if (a->probe_active)
            a->probe_start_ns = now_ns;
        a->up_run = 0;
        a->down_run = 0;
        a->fail_run = 0;
        out->reason = TBT_FEC_ADAPT_R_WAIT;
        return;
    }

    p = o->p_ppm;
    r = o->r_fail_ppm;

    if (a->probe_active)
    {
        if (o->r_valid && r > TBT_FEC_ADAPT_PROBE_HIGH_PPM)
        {
            if (++a->probe_fail_run >= TBT_FEC_ADAPT_FAIL_RUN)
            {
                tbt_fec_adapt_issue(a, a->probe_from,
                                    TBT_FEC_ADAPT_R_PROBE_REVERT, 0, out);
                return;
            }
        }
        else
            a->probe_fail_run = 0;
        if (now_ns >= a->probe_start_ns &&
            now_ns - a->probe_start_ns >= TBT_FEC_ADAPT_PROBE_OBSERVE_NS)
        {
            a->probe_active = 0;
            a->probe_from = a->cur;
            out->reason = TBT_FEC_ADAPT_R_PROBE_COMMIT;
            return;
        }
        if (tbt_fec_adapt_class_of_p(p) > a->cur)
        {
            tbt_fec_adapt_issue(a, a->probe_from,
                                TBT_FEC_ADAPT_R_PROBE_ABORT, 0, out);
            return;
        }
        return;
    }

    /* Residual failures say the codec recovers less than the table assumes.
     * Move one class up regardless of p, but keep the dwell and rate gates. */
    if (o->r_valid && r > TBT_FEC_ADAPT_FAIL_UP_PPM)
    {
        if (++a->fail_run >= TBT_FEC_ADAPT_FAIL_RUN)
        {
            target = (u8)(a->cur + 1);
            if (target < TBT_FEC_ADAPT_CLASSES)
            {
                a->no_probe_until[target] =
                    now_ns + TBT_FEC_ADAPT_FAIL_BAN_NS;
                if (tbt_fec_adapt_can_switch(a, now_ns, 1))
                    tbt_fec_adapt_issue(a, target,
                                        TBT_FEC_ADAPT_R_FAIL_UP, 0, out);
            }
            return;
        }
    }
    else
        a->fail_run = 0;

    target = tbt_fec_adapt_class_of_p(p);

    if (target > a->cur)
    {
        a->down_run = 0;
        if (p <= tbt_fec_adapt_up_threshold(a->cur) +
                     TBT_FEC_ADAPT_HYST_PPM)
        {
            a->up_run = 0;
            return;
        }
        if (++a->up_run < TBT_FEC_ADAPT_UP_RUN)
            return;
        if (!tbt_fec_adapt_can_switch(a, now_ns, 1))
            return;
        tbt_fec_adapt_issue(a, target, TBT_FEC_ADAPT_R_UP, 0, out);
        return;
    }

    /* Down-probe: 15 s of low residual failure buys one class down without
     * waiting for the six-sample down path. A failed probe bans that class
     * for 60 s. */
    if (a->cur > 0 && o->r_valid && r < TBT_FEC_ADAPT_PROBE_LOW_PPM &&
        (!a->last_switch_ns ||
         (now_ns >= a->last_switch_ns &&
          now_ns - a->last_switch_ns >= TBT_FEC_ADAPT_PROBE_STABLE_NS)) &&
        now_ns >= a->no_probe_until[a->cur - 1] &&
        tbt_fec_adapt_can_switch(a, now_ns, 2))
    {
        tbt_fec_adapt_issue(a, (u8)(a->cur - 1),
                            TBT_FEC_ADAPT_R_PROBE_DOWN, 1, out);
        return;
    }

    if (target < a->cur)
    {
        a->up_run = 0;
        if (p >= tbt_fec_adapt_down_threshold(a->cur) -
                     TBT_FEC_ADAPT_HYST_PPM)
        {
            a->down_run = 0;
            return;
        }
        if (++a->down_run < TBT_FEC_ADAPT_DOWN_RUN)
            return;
        if (!tbt_fec_adapt_can_switch(a, now_ns, 2))
            return;
        tbt_fec_adapt_issue(a, target, TBT_FEC_ADAPT_R_DOWN, 0, out);
        return;
    }

    a->up_run = 0;
    a->down_run = 0;
}

void tbt_fec_adapt_applied(struct tbt_fec_adapt *a, u64 now_ns)
{
    u8 old;
    u8 to;
    u8 reason;
    bool osc;

    if (!a->busy)
        return;
    old = a->cur;
    to = a->pending_to;
    reason = a->pending_reason;
    a->busy = 0;

    if (to != old)
    {
        osc = a->last_switch_ns && now_ns >= a->last_switch_ns &&
              now_ns - a->last_switch_ns < TBT_FEC_ADAPT_OSC_WIN_NS &&
              to == a->prev_class;
        a->prev_class = old;
        a->last_switch_ns = now_ns;
        a->last_dir = to > old ? 1 : 2;
        a->cur = to;
        a->dwell_mul = osc ? TBT_FEC_ADAPT_DWELL_MUL : 1;
        a->sw[a->sw_head] = now_ns;
        a->sw_head = (a->sw_head + 1) % TBT_FEC_ADAPT_RATE_MAX;
        if (a->sw_n < TBT_FEC_ADAPT_RATE_MAX)
            a->sw_n++;
        if (tbt_fec_adapt_switches_since(a, now_ns,
                                         TBT_FEC_ADAPT_HOLD_WIN_NS) >=
            TBT_FEC_ADAPT_HOLD_COUNT)
            a->hold_until_ns = now_ns + TBT_FEC_ADAPT_HOLD_NS;
    }

    if (reason == TBT_FEC_ADAPT_R_PROBE_REVERT)
    {
        if (a->probe_active)
            a->no_probe_until[old] = now_ns + TBT_FEC_ADAPT_PROBE_BAN_NS;
        a->probe_active = 0;
        a->probe_from = a->cur;
    }
    else if (reason == TBT_FEC_ADAPT_R_PROBE_ABORT)
    {
        a->probe_active = 0;
        a->probe_from = a->cur;
    }
    else if (a->pending_probe)
    {
        a->probe_active = 1;
        a->probe_from = old;
        a->probe_start_ns = now_ns;
    }

    a->pending_probe = 0;
    a->pending_reason = 0;
    a->up_run = 0;
    a->down_run = 0;
    a->fail_run = 0;
    a->probe_fail_run = 0;
}

void tbt_fec_adapt_abort(struct tbt_fec_adapt *a)
{
    if (!a->busy)
        return;
    a->busy = 0;
    a->pending_probe = 0;
    a->pending_reason = 0;
    a->up_run = 0;
    a->down_run = 0;
    a->fail_run = 0;
    a->probe_fail_run = 0;
}
