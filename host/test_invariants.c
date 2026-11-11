/* test_invariants.c - randomized state-machine invariant test.
 *
 * WHAT THIS IS
 *   One deterministic pseudo-random operation stream drives the real pure
 *   modules the FEC and session state machines are built from:
 *
 *     - tbt_portmap.c, the carrier source-port allocator;
 *     - the SC-LDPC continuous rate controller in
 *       tbt_fec_adapt_sclc.c;
 *     - the receiver reorder-window classifier tbt_fec_rx_window_step().
 *
 *   After every step the test asserts the invariants that must hold in all
 *   interleavings:
 *
 *     P  allocated source ports are unique, the bitmap and the shadow set
 *        agree, and a freed port is immediately allocatable again;
 *     A  every controller output derives n + m == shares, names a
 *        buildable split inside [m_min, m_max], keeps its continuous
 *        quantities bounded, and holds m while carriers are paused;
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

struct adapt
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u16 shares;
    u16 groups;
    u16 extra;
    u16 n_init;
    u16 n_prev;
    u16 n_cap;
    u16 d;
    u16 alive;
    u16 stalled;
    u16 m_prev;
    u32 w_ppm;
    u32 abandoned;
    u32 tick_delivered;
    u32 ticks;
    u32 frozen;
    u32 reasons[8];
    u8 variant;
    u64 now;
};

static void adapt_setup(struct adapt *m, u8 variant)
{
    int rc;

    memset(m, 0, sizeof(*m));
    m->variant = variant;
    if (variant == TBT_FEC_SCLC_UP)
    {
        m->shares = 32;
        m->groups = 8;
        m->extra = 2;
        m->n_init = 16;
    }
    else
    {
        m->shares = 256;
        m->groups = 1;
        m->extra = 8;
        m->n_init = 128;
    }
    rc = tbt_fec_adapt_sclc_init(&m->c, m->shares, m->groups, m->extra,
                                 m->n_init, variant);
    eq64((u64)(u32)rc, 0, "adapt: controller init");
    m->now = SEC;
    m->d = 1;
    m->w_ppm = 1000000;
    m->alive = (u16)(m->groups * m->shares);
    m->m_prev = (u16)(m->shares - m->n_init);
    m->n_cap = m->n_init;
    ok(m->c.ready != 0, "adapt: controller is ready");
    eq64(m->c.shares, m->shares, "adapt: controller shares");
    eq64(m->c.groups, m->groups, "adapt: controller groups");
    eq64(m->c.variant, variant, "adapt: controller variant");
}

static void adapt_check(struct adapt *m,
                        const struct tbt_fec_adapt_sclc_out *out)
{
    u32 total = (u32)m->groups * m->shares;
    bool pause;

    ok(out->reason <= TBT_FEC_ADAPT_SCLC_R_SATURATED,
       "adapt: reason is in the enum");
    /* A paused or cold tick reports the burst of the last control tick; that
     * burst was capped by the n in force then, which is not today's n. */
    if (out->reason != TBT_FEC_ADAPT_SCLC_R_PAUSED &&
        out->reason != TBT_FEC_ADAPT_SCLC_R_COLD &&
        out->reason != TBT_FEC_ADAPT_SCLC_R_BACKUP)
        m->n_cap = m->n_prev;
    eq64((u64)out->n_target + out->m_target, m->shares,
         "adapt: n + m == shares");
    ok(out->m_target >= m->c.m_min && out->m_target <= m->c.m_max,
       "adapt: m_target is inside the clamp");
    ok(tbt_fec_adapt_sclc_buildable(m->variant, out->n_target,
                                    out->m_target),
       "adapt: the output split is buildable");
    ok(out->eps_q16 >= 65536u / m->shares,
       "adapt: eps is at least one share");
    ok(out->eps_q16 <= TBT_FEC_ADAPT_SCLC_EPS_MAX_Q16,
       "adapt: eps is at most 5%");
    ok(out->sigma_q16 >= TBT_FEC_ADAPT_SCLC_SIGMA_MIN_Q16,
       "adapt: sigma is at least one share");
    ok(out->burst_q16 <= ((u32)m->n_cap << 16) / 4u,
       "adapt: burst is bounded by a quarter block");
    ok(out->q_raw_q16 < ((u32)m->shares << 16),
       "adapt: q_raw is inside the block");
    ok(out->delta_q16 < ((u32)m->shares << 16),
       "adapt: delta is inside the block");
    ok(out->backup <= m->shares, "adapt: backup fits the block");

    pause = m->alive == 0 || (u32)m->stalled * 10u > total;
    if (m->tick_delivered)
        pause = pause || (u64)m->abandoned * 100u > m->tick_delivered;
    else
        pause = pause || m->abandoned != 0;
    if (pause)
    {
        eq64(out->frozen, 1, "adapt: a paused tick is frozen");
        eq64(out->m_target, m->m_prev, "adapt: a paused tick holds m");
    }
    if (out->frozen)
        m->frozen++;
    m->m_prev = out->m_target;
}

static void adapt_feed(struct adapt *m, u16 E, u16 S, bool first_ok,
                       u16 real)
{
    tbt_fec_adapt_sclc_block_sample(&m->c, E, S, first_ok, real);
    m->now += 10ull * MS;
    m->tick_delivered = m->c.delivered;
    m->n_prev = m->c.n_cur;
    tbt_fec_adapt_sclc_tick(&m->c, m->now, m->d, m->w_ppm, m->alive,
                            m->stalled, m->abandoned, &m->out);
    if (m->out.reason < 8)
        m->reasons[m->out.reason]++;
    m->ticks++;
    adapt_check(m, &m->out);
}

static void adapt_random_feed(struct adapt *m)
{
    u16 real = m->c.n_cur;

    m->d = (u16)rnd_below(9);
    m->w_ppm = 500000u + rnd_below(1500001u);
    m->alive = (u16)(200u + rnd_below(57u));
    m->stalled = (u16)rnd_below(31u);
    m->abandoned = rnd_below(4u);
    adapt_feed(m, (u16)rnd_below(m->shares / 2u),
               (u16)(m->extra + rnd_below(9u)),
               rnd_below(100u) < 95u, real);
}

/* A deterministic warm start: the 16 cold samples and a few steady ticks. */
static void adapt_warm(struct adapt *m)
{
    int i;

    for (i = 0; i < 24; i++)
        adapt_feed(m, (u16)(m->shares / 8u), m->extra, true, m->c.n_cur);
}

/* Both directions, every tick field drawn from the stream. */
static void adapt_random(void)
{
    struct adapt down;
    struct adapt up;
    u32 i;

    adapt_setup(&down, TBT_FEC_SCLC_DOWN);
    adapt_setup(&up, TBT_FEC_SCLC_UP);
    for (i = 0; i < 20000u; i++)
    {
        adapt_random_feed(&down);
        adapt_random_feed(&up);
    }
}

/* The freeze paths are unconditional: a dead population, more than 10%
 * stalled carriers, and abandoned shares above 1% of the block. */
static void adapt_freeze_cases(void)
{
    struct adapt m;
    u16 held;

    adapt_setup(&m, TBT_FEC_SCLC_DOWN);
    adapt_warm(&m);
    held = m.out.m_target;
    m.alive = 0;
    adapt_feed(&m, 32, m.extra, true, m.c.n_cur);
    eq64(m.out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
         "freeze: a dead population pauses");
    eq64(m.out.m_target, held, "freeze: a dead population holds m");

    adapt_setup(&m, TBT_FEC_SCLC_DOWN);
    adapt_warm(&m);
    held = m.out.m_target;
    m.stalled = 27;
    adapt_feed(&m, 32, m.extra, true, m.c.n_cur);
    eq64(m.out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
         "freeze: 27 stalled of 256 pauses");
    eq64(m.out.m_target, held, "freeze: stalled carriers hold m");

    adapt_setup(&m, TBT_FEC_SCLC_DOWN);
    adapt_warm(&m);
    held = m.out.m_target;
    m.abandoned = 3;
    adapt_feed(&m, 12, m.extra, true, 256);
    eq64(m.out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
         "freeze: 3 abandoned shares of 256 pause");
    eq64(m.out.m_target, held, "freeze: abandoned shares hold m");
}

/* The 16-sample warm start holds m; the 17th sample lets it move. */
static void adapt_low_sample(void)
{
    struct adapt m;
    u16 held;
    u32 q;
    int i;

    adapt_setup(&m, TBT_FEC_SCLC_DOWN);
    held = (u16)(m.shares - m.n_init);
    for (i = 0; i < 16; i++)
    {
        adapt_feed(&m, 12, m.extra, true, m.c.n_cur);
        eq64(m.out.m_target, held, "cold: m is held for 16 samples");
    }
    q = m.out.q_raw_q16;
    held = m.out.m_target;
    adapt_feed(&m, 12, m.extra, true, m.c.n_cur);
    ok(m.out.q_raw_q16 != q || m.out.m_target != held,
       "cold: the 17th sample moves q or m");
}

static void adapt_scenarios(void)
{
    adapt_random();
    adapt_freeze_cases();
    adapt_low_sample();
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
    adapt_setup(&adapt, (u8)(rnd_below(2) ? TBT_FEC_SCLC_UP
                                          : TBT_FEC_SCLC_DOWN));
    rx_init(&rx);

    for (s = 1; s <= steps; s++)
    {
        cur_step = s;
        port_step();
        port_check();
        adapt_random_feed(&adapt);
        rx_step(&rx);
    }

    adapt_scenarios();

    printf("test_invariants: %u steps, %d checks, %d failures "
           "(adapt: ticks=%u frozen=%u; "
           "rx: rebases=%llu rewinds=%llu)\n",
           steps, checks, failures, adapt.ticks, adapt.frozen,
           (unsigned long long)rx.rebases,
           (unsigned long long)rx.rewinds);
    {
        int r;

        printf("adapt reasons:");
        for (r = 0; r < 8; r++)
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
