/* test_invariants.c - randomized state-machine invariant test.
 *
 * WHAT THIS IS
 *   One deterministic pseudo-random operation stream drives the real pure
 *   modules the FEC and session state machines are built from:
 *
 *     - tbt_portmap.c, the carrier source-port allocator;
 *     - the SC-LDPC estimator and rate controller in
 *       tbt_fec_adapt_sclc.c;
 *     - the receiver reorder-window classifier tbt_fec_rx_window_step().
 *
 *   After every step the test asserts the invariants that must hold in all
 *   interleavings:
 *
 *     P  allocated source ports are unique, the bitmap and the shadow set
 *        agree, and a freed port is immediately allocatable again;
 *     A  every controller output derives n + m == shares, keeps
 *        groups * shares <= 256 and names a rate inside rate_mask;
 *     R  the reorder cursor never moves backwards inside one window
 *        generation; a rebase starts a new generation only before delivery.
 *
 *   The stream is a fixed function of the seed, so a failure reproduces.
 *   No kernel headers, no root, no module.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_fec_adapt_sclc.h"
#include "tbt_portmap.h"

#define STEPS_DEFAULT 60000u
#define MS 1000000u
#define SEC 1000000000u

/* ------------------------------------------------------------------ *
 * Deterministic randomness and check accounting
 * ------------------------------------------------------------------ */

static u64 rng_state;
static u64 cur_step;
static int checks;
static int failures;
static int fail_prints;

static u64 rnd64(void)
{
    u64 z = (rng_state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static u32 rnd32(void)
{
    return (u32)(rnd64() >> 32);
}

static u32 rnd_below(u32 n)
{
    return n ? rnd32() % n : 0u;
}

static void fail_note(const char *what)
{
    failures++;
    if (fail_prints++ < 40)
        printf("FAIL step %llu: %s\n", (unsigned long long)cur_step, what);
}

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond)
        fail_note(what);
}

static void eq64(u64 got, u64 want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    if (fail_prints++ < 40)
        printf("FAIL step %llu: %s: got %llu want %llu\n",
               (unsigned long long)cur_step, what, (unsigned long long)got,
               (unsigned long long)want);
}


/* ------------------------------------------------------------------ *
 * P: source-port bitmap
 * ------------------------------------------------------------------ */

#define PORT_WORDS (TBT_PORTMAP_PORTS / 32u)
#define PORT_BITS_PER_LONG (8 * (int)sizeof(unsigned long))
#define PORT_LIVE_MAX 4096
#define PORT_WINDOWS 4

struct pwin
{
    u16 lo;
    u16 hi;
};

static unsigned long port_bits[PORT_WORDS];
static u8 port_held[TBT_PORTMAP_PORTS];
static u16 port_live[PORT_LIVE_MAX];
static u32 port_nlive;
static struct pwin port_win[PORT_WINDOWS];

static u32 port_bitmap_popcount(void)
{
    unsigned int words = tbt_portmap_words();
    unsigned int i;
    u32 n = 0;

    ok(words <= PORT_WORDS, "portmap: bitmap words fit the test array");
    if (words > PORT_WORDS)
        words = PORT_WORDS;
    for (i = 0; i < words; i++)
        n += (u32)__builtin_popcountl(port_bits[i]);
    return n;
}

static bool port_exact(void)
{
    u32 p;

    for (p = 0; p < TBT_PORTMAP_PORTS; p++)
    {
        bool bit = ((port_bits[p / (u32)PORT_BITS_PER_LONG] >>
                     (p % (u32)PORT_BITS_PER_LONG)) &
                    1ul) != 0;

        if (bit != (port_held[p] != 0))
            return false;
    }
    return true;
}

static u32 port_window_full(const struct pwin *w)
{
    u32 p, n = 0;

    for (p = w->lo; p <= w->hi; p++)
        n += port_held[p] != 0;
    return n;
}

static void port_init(void)
{
    u32 i;

    memset(port_bits, 0, sizeof(port_bits));
    memset(port_held, 0, sizeof(port_held));
    port_nlive = 0;
    for (i = 0; i < PORT_WINDOWS; i++)
    {
        u32 lo = 1024u + rnd_below(56000u);
        u32 width = 2u + rnd_below(500u);

        port_win[i].lo = (u16)lo;
        port_win[i].hi = (u16)(lo + width);
    }
}

static void port_step(void)
{
    u32 op = rnd_below(100);
    u16 p;
    int rc;

    if (op < 48)
    {
        struct pwin *w;

        if (port_nlive >= PORT_LIVE_MAX)
            return;
        w = &port_win[rnd_below(PORT_WINDOWS)];
        rc = tbt_portmap_alloc(port_bits, w->lo, w->hi, &p);
        if (rc == 0)
        {
            ok(p >= w->lo && p <= w->hi, "portmap: allocation is in window");
            ok(p != 0, "portmap: port 0 is never allocated");
            ok(port_held[p] == 0, "portmap: allocation is not live");
            port_held[p] = 1;
            port_live[port_nlive++] = p;
        }
        else
        {
            eq64((u64)(u32)rc, (u64)(u32)-ENOSPC,
                 "portmap: a failed allocation is ENOSPC");
            eq64(port_window_full(w), (u32)(w->hi - w->lo) + 1u,
                 "portmap: ENOSPC means the window is full");
        }
    }
    else if (op < 90)
    {
        u32 idx;
        u16 freed;

        if (!port_nlive)
            return;
        idx = rnd_below(port_nlive);
        p = port_live[idx];
        tbt_portmap_free(port_bits, p);
        ok(port_held[p] != 0, "portmap: freeing a live port");
        port_held[p] = 0;
        freed = p;
        port_live[idx] = port_live[--port_nlive];

        /* The released port must be handed out again at once. The direct
         * single-port window proves the bit is clear; the global allocation
         * path proves the scan can reach it. */
        rc = tbt_portmap_alloc(port_bits, freed, freed, &p);
        eq64((u64)(u32)rc, 0, "portmap: a freed port is immediately reusable");
        eq64(p, freed, "portmap: the same port comes back");
        tbt_portmap_free(port_bits, p);
    }
    else
    {
        p = (u16)(1 + rnd_below(TBT_PORTMAP_PORTS - 1u));
        if (port_held[p])
            return;
        tbt_portmap_free(port_bits, p);
        ok(port_held[p] == 0, "portmap: freeing a free port is a no-op");
        if (rnd_below(10) == 0)
        {
            u32 i = rnd_below(PORT_WINDOWS);
            u32 lo = 1024u + rnd_below(56000u);

            port_win[i].lo = (u16)lo;
            port_win[i].hi = (u16)(lo + 1u + rnd_below(500u));
        }
    }
}

static void port_check(void)
{
    eq64(port_bitmap_popcount(), port_nlive,
         "portmap: bitmap count == live allocations");
    if (cur_step % 997u == 0)
        ok(port_exact(), "portmap: bitmap matches the shadow set");
}

/* ------------------------------------------------------------------ *
 * A: SC-LDPC estimator and rate controller
 * ------------------------------------------------------------------ */

#define ADAPT_LEVELS TBT_FEC_ADAPT_SCLC_LEVELS

struct adapt
{
    struct tbt_fec_adapt_sclc_est est;
    struct tbt_fec_adapt_sclc_ctl ctl;
    struct tbt_fec_adapt_sclc_est_out o;
    u16 shares;
    u16 groups;
    u64 now;
    u32 loss_ppm;
    u16 alive;
    u16 loss_hold;
    u32 applied;
    u32 aborted;
    u32 rejected;
    u32 rollbacks;
    u32 reasons[32];
};

static const u16 adapt_shares[4] = {32, 64, 128, 256};

static void adapt_setup(struct adapt *m, u16 shares, bool enabled, u8 rate_idx)
{
    const struct tbt_fec_adapt_sclc_rate *r =
        tbt_fec_adapt_sclc_rate_at(rate_idx);
    int rc;

    memset(m, 0, sizeof(*m));
    m->shares = shares;
    m->groups = (u16)(TBT_FEC_MAX_CARRIERS / shares);
    m->now = SEC;
    m->loss_ppm = 5000;
    m->alive = 256;
    tbt_fec_adapt_sclc_est_init(&m->est, shares);
    tbt_fec_adapt_sclc_est_carriers(&m->est, 256, 0, 256);
    ok(r != NULL, "adapt: starting rate is on the ladder");
    rc = tbt_fec_adapt_sclc_ctl_init(&m->ctl, enabled, r->pct, shares,
                                     m->groups);
    eq64((u64)(u32)rc, 0, "adapt: controller init");
    tbt_fec_adapt_sclc_ctl_set_epoch(&m->ctl, 1);
    tbt_fec_adapt_sclc_ctl_set_rate_mask(
        &m->ctl, (u8)((1u << ADAPT_LEVELS) - 1u));
}

static void adapt_feed(struct adapt *m, u32 loss_ppm, u32 r_ppm)
{
    u8 miss[TBT_FEC_ADAPT_BITMAP_BYTES];
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u16 i, g, miss_n;
    u32 pkts = 256u;
    u32 req;

    memset(miss, 0, sizeof(miss));
    memset(alive, 0xff, sizeof(alive));
    miss_n = (u16)(((u64)loss_ppm * m->shares) / 1000000ull);
    if (miss_n > m->shares)
        miss_n = m->shares;
    for (i = 0; i < miss_n; i++)
        tbt_fec_adapt_bitmap_set(miss, i);
    for (g = 0; g < m->groups; g++)
        tbt_fec_adapt_sclc_est_block(&m->est, m->now, g, m->shares, miss,
                                     alive);
    tbt_fec_adapt_sclc_est_super(&m->est, m->now);
    req = (u32)(((u64)r_ppm * pkts + 500000ull) / 1000000ull);
    tbt_fec_adapt_sclc_est_fail(&m->est, m->now, req, pkts);
}

static void adapt_check(struct adapt *m,
                        const struct tbt_fec_adapt_sclc_out *out)
{
    u8 idx = out->switch_now ? out->to : m->ctl.cur;
    const struct tbt_fec_adapt_sclc_rate *r;
    u32 p_used;

    ok(m->ctl.ready != 0, "adapt: controller is ready");
    eq64(m->ctl.shares, m->shares, "adapt: controller shares");
    eq64(m->ctl.groups, m->groups, "adapt: controller groups");
    ok(m->ctl.cur < ADAPT_LEVELS, "adapt: current index is on the ladder");
    ok((m->ctl.rate_mask & (1u << m->ctl.cur)) != 0,
       "adapt: current rate is inside rate_mask");
    ok(m->ctl.rate_mask != 0 && (m->ctl.rate_mask & ~0x3fu) == 0,
       "adapt: rate_mask is a nonempty ladder subset");
    ok((u32)m->groups * m->shares <= 256,
       "adapt: groups * shares fits the carrier pool");
    ok(idx < ADAPT_LEVELS, "adapt: output index is on the ladder");
    ok((m->ctl.rate_mask & (1u << idx)) != 0,
       "adapt: output rate is inside rate_mask");
    eq64(out->from, m->ctl.cur, "adapt: from is the current index");
    if (!out->switch_now)
        eq64(out->to, m->ctl.cur, "adapt: a quiet output names the current");
    eq64((u64)out->shape.n + out->shape.m, m->shares,
         "adapt: n + m == shares");
    ok(out->shape.n > 0 && out->shape.m > 0, "adapt: both sides are nonempty");
    eq64(out->shape.groups, m->groups, "adapt: shape groups");
    r = tbt_fec_adapt_sclc_rate_at(idx);
    ok(r != NULL, "adapt: output index has a ladder rate");
    if (r)
    {
        eq64(out->shape.rate_pct, r->pct, "adapt: shape rate_pct");
        eq64(out->shape.rate_ppm, r->ppm, "adapt: shape rate_ppm");
    }
    if (!m->ctl.enabled)
    {
        ok(!out->switch_now, "adapt: a disabled controller never switches");
        eq64(out->reason, TBT_FEC_ADAPT_SCLC_R_DISABLED,
             "adapt: a disabled controller says so");
    }
    if (out->reason == TBT_FEC_ADAPT_SCLC_R_ROLLBACK)
    {
        eq64(out->to, m->ctl.commit_from, "adapt: rollback returns to the base");
        ok(out->to < m->ctl.cur, "adapt: rollback lowers the rate");
    }

    p_used = m->o.p_win_ppm > m->o.p_ewma_ppm ? m->o.p_win_ppm
                                              : m->o.p_ewma_ppm;
    eq64(m->o.p_ppm, p_used, "adapt: p_used is max(window, EWMA)");
    ok(m->o.p_ppm <= 1000000u, "adapt: p_ppm is a share");
    ok(m->o.p_ewma_ppm <= 1000000u, "adapt: EWMA is a share");
    ok(m->o.p_win_ppm <= 1000000u, "adapt: window p is a share");
    ok(m->o.p_fast_ppm <= 1000000u, "adapt: fast p is a share");
    ok(m->o.r_fail_ppm <= 1000000u, "adapt: r_fail is a share");
    ok(m->o.codec_fail_ppm <= 1000000u, "adapt: codec failure is a share");
    eq64(m->est.shares, m->shares, "adapt: estimator shares");
}

static void adapt_tick(struct adapt *m, u32 loss_ppm, u32 r_ppm,
                       struct tbt_fec_adapt_sclc_out *out)
{
    m->now += 250ull * MS;
    tbt_fec_adapt_sclc_est_carriers(&m->est, m->alive, 0, 256);
    adapt_feed(m, loss_ppm, r_ppm);
    tbt_fec_adapt_sclc_est_sample(&m->est, m->now, &m->o);
    tbt_fec_adapt_sclc_ctl_eval(&m->ctl, &m->o, m->now, out);
    if (out->reason < 32)
        m->reasons[out->reason]++;
    adapt_check(m, out);
}

static void adapt_apply(struct adapt *m,
                        const struct tbt_fec_adapt_sclc_out *out)
{
    u8 to = out->to;

    tbt_fec_adapt_sclc_ctl_applied(&m->ctl, m->now);
    eq64(m->ctl.cur, to, "adapt: applied moves the current rate");
    ok(!m->ctl.busy, "adapt: applied clears the pending switch");
    ok((m->ctl.rate_mask & (1u << m->ctl.cur)) != 0,
       "adapt: the applied rate stays inside rate_mask");
    m->applied++;
}

static void adapt_switch_action(struct adapt *m,
                                const struct tbt_fec_adapt_sclc_out *out)
{
    u32 act = rnd_below(100);

    if (out->reason == TBT_FEC_ADAPT_SCLC_R_ROLLBACK)
        m->rollbacks++;
    if (act < 62)
    {
        adapt_apply(m, out);
        if (m->ctl.commit_raise && rnd_below(3) == 0)
        {
            struct tbt_fec_adapt_sclc_est_out bad = m->o;

            bad.r_valid = true;
            bad.r_fail_ppm = 30000;
            bad.codec_fail_ppm = 0;
            tbt_fec_adapt_sclc_ctl_commit_failed(&m->ctl, &bad, m->now);
        }
    }
    else if (act < 82)
    {
        u8 cur = m->ctl.cur;

        tbt_fec_adapt_sclc_ctl_abort(&m->ctl);
        eq64(m->ctl.cur, cur, "adapt: abort keeps the rate");
        ok(!m->ctl.busy, "adapt: abort clears the pending switch");
        m->aborted++;
    }
    else
    {
        u8 cur = m->ctl.cur;

        tbt_fec_adapt_sclc_ctl_rejected(&m->ctl, m->now);
        eq64(m->ctl.cur, cur, "adapt: a refused CFG keeps the rate");
        ok(!m->ctl.busy, "adapt: a refused CFG clears the pending switch");
        ok(m->ctl.reject_target < ADAPT_LEVELS,
           "adapt: a refused CFG names a valid fallback");
        m->rejected++;
    }
}

static void adapt_step(struct adapt *m)
{
    static const u32 levels[5] = {0u, 30000u, 150000u, 350000u, 600000u};
    struct tbt_fec_adapt_sclc_out out;

    /* Sustained loss phases, not white noise: the controller needs several
     * samples and a dwell before it may move, so a one-tick jump would only
     * exercise the filters. A phase is 2..17 s. */
    if (!m->loss_hold)
    {
        m->loss_ppm = levels[rnd_below(5)];
        m->loss_hold = (u16)(40 + rnd_below(120));
    }
    m->loss_hold--;

    /* Rarely starve the carrier population; the estimator must freeze and
     * the controller must not switch from that sample. */
    m->alive = (rnd_below(2000) == 0) ? 180 : 256;

    if (rnd_below(3000) == 0)
        tbt_fec_adapt_sclc_ctl_set_enabled(&m->ctl, !m->ctl.enabled, m->now);

    /* A rate plan may change only between epochs: a mask that dropped the
     * pending target would leave applied() naming an unsupported rate. */
    if (!m->ctl.busy && rnd_below(700) == 0)
    {
        u8 mask = (u8)(1u << m->ctl.cur);
        u8 i;

        for (i = 0; i < ADAPT_LEVELS; i++)
            if (rnd_below(2) == 0)
                mask |= (u8)(1u << i);
        tbt_fec_adapt_sclc_ctl_set_rate_mask(&m->ctl, mask);
        eq64(m->ctl.rate_mask, mask, "adapt: a supported plan is installed");
    }

    adapt_tick(m, m->loss_ppm,
               rnd_below(100) < 10 ? 25000u + rnd_below(20000u)
                                   : rnd_below(5000u),
               &out);
    if (out.switch_now)
        adapt_switch_action(m, &out);
}

/* Three deterministic scenarios make the named paths unconditional: a
 * refused down CFG holds the current rate, a bad raise rolls back, and a
 * loss burst skips the dwell. */
static void adapt_scenarios(void)
{
    struct adapt m;
    struct tbt_fec_adapt_sclc_out out;
    int i;

    /* A refused down CFG: the peer does not build even lower shapes, so the
     * controller keeps the rate both ends already use and bans further down
     * steps instead of walking one rung lower. */
    adapt_setup(&m, 256, true, 3);
    out.switch_now = 0;
    for (i = 0; i < 12 && !out.switch_now; i++)
        adapt_tick(&m, 400000, 0, &out);
    ok(out.switch_now, "scenario: 40% loss switches down");
    eq64(out.to, 2, "scenario: the down target is rate 40");
    if (out.switch_now)
    {
        tbt_fec_adapt_sclc_ctl_rejected(&m.ctl, m.now);
        eq64(m.ctl.cur, 3, "scenario: the refused CFG keeps rate 50");
        eq64(m.ctl.reject_target, 3,
             "scenario: the refused down keeps the current rate");
        eq64(m.ctl.no_down, 1,
             "scenario: further down steps are banned");
        out.switch_now = 0;
        for (i = 0; i < 16 && !out.switch_now; i++)
            adapt_tick(&m, 400000, 0, &out);
        ok(!out.switch_now,
           "scenario: the refused down is neither retried nor stepped past");
    }

    /* A bad committed raise rolls back to the rate before it. */
    adapt_setup(&m, 256, true, 3);
    out.switch_now = 0;
    for (i = 0; i < 12 && !out.switch_now; i++)
        adapt_tick(&m, 0, 0, &out);
    ok(out.switch_now, "scenario: a clean line raises");
    eq64(out.to, 5, "scenario: the raise goes to rate 70");
    if (out.switch_now)
    {
        struct tbt_fec_adapt_sclc_est_out bad = m.o;

        adapt_apply(&m, &out);
        bad.r_valid = true;
        bad.r_fail_ppm = 30000;
        bad.codec_fail_ppm = 0;
        tbt_fec_adapt_sclc_ctl_commit_failed(&m.ctl, &bad, m.now);
        adapt_tick(&m, 0, 0, &out);
        eq64(out.reason, TBT_FEC_ADAPT_SCLC_R_ROLLBACK,
             "scenario: a bad raise requests a rollback");
        ok(out.switch_now, "scenario: the rollback is a switch");
        if (out.switch_now)
        {
            eq64(out.to, 3, "scenario: the rollback returns to rate 50");
            adapt_apply(&m, &out);
        }
    }

    /* A loss burst larger than the 4 s window uses the fast path. */
    adapt_setup(&m, 256, true, 3);
    out.switch_now = 0;
    for (i = 0; i < 20; i++)
    {
        adapt_tick(&m, 220000, 0, &out);
        ok(!out.switch_now, "scenario: 22% loss is steady at rate 50");
    }
    for (i = 0; i < 2 && !out.switch_now; i++)
        adapt_tick(&m, 600000, 0, &out);
    ok(out.switch_now, "scenario: a 60% burst switches at once");
    if (out.switch_now)
    {
        eq64(out.reason, TBT_FEC_ADAPT_SCLC_R_BURST,
             "scenario: the burst path is named");
        ok(out.to < 3, "scenario: the burst target steps down");
        adapt_apply(&m, &out);
    }
}

/* ------------------------------------------------------------------ *
 * R: receiver reorder window
 * ------------------------------------------------------------------ */

struct rxw
{
    u64 next_super;
    u64 gen_base;
    u64 delivered_hi;
    u64 min_block_seen;
    u64 highest_block;
    u64 generations;
    u64 rebases;
    u64 rewinds;
    u16 slots;
    bool delivered;
    bool rebase_pending;
};

static void rx_init(struct rxw *m)
{
    memset(m, 0, sizeof(*m));
    m->slots = (u16)(1 + rnd_below(16));
    m->generations = 1;
}

static void rx_check(struct rxw *m)
{
    ok(m->next_super >= m->gen_base,
       "rx_window: the cursor stays inside its generation");
    if (m->delivered)
    {
        ok(m->delivered_hi >= m->gen_base,
           "rx_window: the delivery watermark is inside the generation");
        ok(m->next_super >= m->delivered_hi,
           "rx_window: the cursor never falls behind the watermark");
    }
    ok(m->slots >= 1, "rx_window: slots are nonzero");
}

static void rx_rebase(struct rxw *m, u64 block, u64 prev_next)
{
    u64 base = block;

    if (m->min_block_seen && m->min_block_seen < base)
        base = m->min_block_seen;
    if (base < prev_next)
        m->rewinds++;
    m->generations++;
    m->gen_base = base;
    m->next_super = base;
    m->min_block_seen = base;
    if (base > m->highest_block)
        m->highest_block = base;
    m->delivered = false;
    m->delivered_hi = base;
    m->rebase_pending = false;
    m->rebases++;
}

static void rx_arrival(struct rxw *m)
{
    u64 prev_next = m->next_super;
    u64 prev_gen = m->generations;
    u64 block;
    bool step_rebase = m->rebase_pending || rnd_below(40) == 0;
    u32 kind = rnd_below(100);
    enum tbt_fec_rx_win step, want;

    if (kind < 45)
        block = m->next_super + rnd_below(m->slots);
    else if (kind < 65)
        block = m->next_super + m->slots + rnd_below(4);
    else if (kind < 85)
    {
        u64 back = 1u + rnd_below(8);

        block = m->next_super > back ? m->next_super - back : 0;
    }
    else if (kind < 95)
        block = m->next_super + rnd_below((u32)m->slots * 3u);
    else
        block = m->next_super;

    if (step_rebase)
    {
        /* A stream reset drops the delivery state before the first record
         * of the new stream arrives. */
        m->delivered = false;
        m->delivered_hi = 0;
        m->rebase_pending = true;
    }

    want = step_rebase ? TBT_FEC_RX_WIN_REBASE
           : block < m->next_super
               ? (m->delivered ? TBT_FEC_RX_WIN_LATE : TBT_FEC_RX_WIN_REBASE)
           : block >= m->next_super + m->slots ? TBT_FEC_RX_WIN_FAR
                                               : TBT_FEC_RX_WIN_PLACE;
    step = tbt_fec_rx_window_step(m->next_super, block, m->slots,
                                  step_rebase, m->delivered);
    eq64(step, want, "rx_window: classifier is exact");

    if (step == TBT_FEC_RX_WIN_REBASE)
    {
        ok(step_rebase || (!m->delivered && block < m->next_super),
           "rx_window: rebase is explicit or a pre-delivery lower block");
        rx_rebase(m, block, prev_next);
        eq64(m->generations, prev_gen + 1,
             "rx_window: a rebase starts a new generation");
    }
    else if (step == TBT_FEC_RX_WIN_PLACE)
    {
        ok(block >= m->next_super && block < m->next_super + m->slots,
           "rx_window: a placed block is inside the window");
        if (block > m->highest_block)
            m->highest_block = block;
    }
    else if (step == TBT_FEC_RX_WIN_LATE)
    {
        ok(m->delivered && block < m->next_super,
           "rx_window: a late block is below a delivered cursor");
    }
    else
    {
        ok(block >= m->next_super + m->slots,
           "rx_window: a far block is beyond the window");
    }

    if (!step_rebase && step != TBT_FEC_RX_WIN_REBASE)
    {
        eq64(m->next_super, prev_next,
             "rx_window: only a rebase moves the cursor");
    }
    if (m->next_super < prev_next)
        eq64(m->generations, prev_gen + 1,
             "rx_window: a lower cursor is a new generation");
}

static void rx_deliver(struct rxw *m)
{
    if (m->rebase_pending)
        return;
    m->next_super++;
    m->delivered = true;
    m->delivered_hi = m->next_super;
    ok(m->next_super > m->gen_base,
       "rx_window: the delivery walk advances the cursor");
}

static void rx_step(struct rxw *m)
{
    u64 prev = m->next_super;

    if (rnd_below(100) < 35)
        rx_deliver(m);
    else
        rx_arrival(m);
    if (m->delivered)
        ok(m->next_super >= prev, "rx_window: a delivered cursor never regresses");
    rx_check(m);
}

/* ------------------------------------------------------------------ *
 * Driver
 * ------------------------------------------------------------------ */

static int run(u32 steps, u64 seed)
{
    struct adapt adapt;
    struct rxw rx;
    u64 s;

    rng_state = seed;
    cur_step = 0;
    checks = 0;
    failures = 0;
    fail_prints = 0;

    port_init();
    adapt_setup(&adapt, adapt_shares[rnd_below(4)], true,
                (u8)rnd_below(ADAPT_LEVELS));
    rx_init(&rx);

    for (s = 1; s <= steps; s++)
    {
        cur_step = s;
        port_step();
        port_check();
        adapt_step(&adapt);
        rx_step(&rx);
    }

    adapt_scenarios();

    printf("test_invariants: %u steps, %d checks, %d failures "
           "(adapt: applied=%u aborted=%u rejected=%u rollbacks=%u; "
           "rx: rebases=%llu rewinds=%llu)\n",
           steps, checks, failures, adapt.applied, adapt.aborted,
           adapt.rejected, adapt.rollbacks, (unsigned long long)rx.rebases,
           (unsigned long long)rx.rewinds);
    {
        int r;

        printf("adapt reasons:");
        for (r = 0; r < 32; r++)
            if (adapt.reasons[r])
                printf(" %d=%u", r, adapt.reasons[r]);
        printf("\n");
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    u32 steps = STEPS_DEFAULT;
    u64 seed = 0x7b17c0de5eed1234ull;

    if (argc > 1)
        steps = (u32)strtoul(argv[1], NULL, 0);
    if (argc > 2)
        seed = (u64)strtoull(argv[2], NULL, 0);
    if (!steps)
        steps = STEPS_DEFAULT;
    return run(steps, seed);
}
