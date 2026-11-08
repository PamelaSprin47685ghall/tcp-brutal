/* test_fec_adapt.c - host test for the adaptive FEC estimator and control law.
 *
 * WHAT THIS IS
 *   The exact code from tbtun/tbt_fec_adapt.c, compiled for the host. It
 *   checks:
 *
 *     - the class table (p boundaries, n/m, groups=32, w=2n, k=n, d=n) and
 *       the reverse lookup from a configured shape;
 *     - the estimator: 1 s bucket window, EWMA, alive filtering, unknown and
 *       abandoned samples, r_fail, idle and carrier pauses;
 *     - the control law: up/down hysteresis, dwell, reverse cooldown, the
 *       3-in-30 s hold, the 10-in-60 s rate cap, the down-probe and its 60 s
 *       ban, and the oscillation dwell multiplier;
 *     - that a fresh session (base_pkt = base_block = 0) gets epoch 1 and a
 *       shape whose coordinates the switch layer fills.
 *
 *   Time is virtual: every case passes explicit nanosecond timestamps, so the
 *   test runs instantly and is deterministic.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_fec_adapt.h"

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

#define MS 1000000ull
#define SEC 1000000000ull

/* ------------------------------------------------------------------ *
 * Estimator helpers
 * ------------------------------------------------------------------ */

static void alive_all(u8 bm[TBT_FEC_ADAPT_BITMAP_BYTES])
{
    memset(bm, 0xff, TBT_FEC_ADAPT_BITMAP_BYTES);
}

/* One super-block: 32 groups, `real` DATA shares each, and one miss on share
 * 0 of the first `miss_groups` groups. */
static void feed_super(struct tbt_fec_est *e, u64 now_ns, u16 real,
                       u16 miss_groups,
                       const u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES])
{
    u16 g;

    for (g = 0; g < TBT_FEC_ADAPT_GROUPS; g++)
        tbt_fec_est_block(e, now_ns, g, real, g < miss_groups ? 1u : 0u,
                          alive);
    tbt_fec_est_super(e, now_ns);
}

/* ------------------------------------------------------------------ *
 * Control helpers
 * ------------------------------------------------------------------ */

static void snapshot(struct tbt_fec_est_out *o, u32 p_ppm, u32 r_ppm)
{
    memset(o, 0, sizeof(*o));
    o->p_ppm = p_ppm;
    o->p_ewma_ppm = p_ppm;
    o->p_win_ppm = p_ppm;
    o->r_fail_ppm = r_ppm;
    o->r_valid = true;
    o->p_valid = true;
    o->sent_win = 4096;
    o->supers_win = 16;
}

static void eval(struct tbt_fec_adapt *a, u32 p_ppm, u32 r_ppm, u64 now_ns,
                 struct tbt_fec_adapt_out *out)
{
    struct tbt_fec_est_out o;

    snapshot(&o, p_ppm, r_ppm);
    tbt_fec_adapt_eval(a, &o, now_ns, out);
}

/* ------------------------------------------------------------------ *
 * Class table
 * ------------------------------------------------------------------ */

static void case_class_table(void)
{
    static const u32 p[] = {0,      5000,   10000,  10001, 30000,
                            30001,  50000,  50001,  100000, 100001,
                            200000, 200001, 1000000};
    static const u8 want[] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
    struct tbt_fec_adapt a;
    struct tbt_fec_shape s;
    u8 i;

    for (i = 0; i < (u8)(sizeof(p) / sizeof(p[0])); i++)
    {
        checks++;
        if (tbt_fec_adapt_class_of_p(p[i]) != want[i])
        {
            failures++;
            printf("FAIL class_of_p(%u) = %u want %u\n", p[i],
                   tbt_fec_adapt_class_of_p(p[i]), want[i]);
        }
    }

    for (i = 0; i < TBT_FEC_ADAPT_CLASSES; i++)
    {
        tbt_fec_adapt_shape(i, &s);
        eq_u32(s.n, 7u - i, "shape n");
        eq_u32(s.m, 1u + i, "shape m");
        eq_u32(s.n + s.m, 8u, "shape n+m");
        eq_u32(s.groups, 32u, "shape groups");
        eq_u32(s.w, 2u * (7u - i), "shape w");
        eq_u32(s.k, 7u - i, "shape k");
        eq_u32(s.d, 7u - i, "shape d");
        eq_u64(s.slots, 0, "shape slots left to the receiver");
        eq_u64(s.base_pkt, 0, "shape base_pkt");
        eq_u64(s.base_block, 0, "shape base_block");
        eq_u32(tbt_fec_adapt_class_of_shape(s.n, s.m), i, "shape lookup");
    }

    eq_u32(tbt_fec_adapt_class_of_shape(8, 0), 0xff, "lookup unknown");
    eq_u32(tbt_fec_adapt_class_of_shape(4, 5), 0xff, "lookup unknown m");
    eq_u32(tbt_fec_adapt_class_of_shape(4, 4), 3, "lookup 4/4");

    tbt_fec_adapt_init(&a, 9);
    eq_u32(a.cur, 3, "init clamps to C3");
    tbt_fec_adapt_init(&a, 5);
    eq_u32(a.cur, 5, "init keeps C5");
}

/* ------------------------------------------------------------------ *
 * Estimator
 * ------------------------------------------------------------------ */

static void case_estimator_ewma(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;
    u32 first;
    int i;

    alive_all(alive);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 256, 0, 256);

    for (i = 0; i < 5; i++)
    {
        feed_super(&e, t, 8, 8, alive); /* 256 sent, 8 miss */
        t += 50 * MS;
    }
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 1280, "ewma sent");
    eq_u32(o.miss_win, 40, "ewma miss");
    eq_u32(o.supers_win, 5, "ewma supers");
    eq_u32(o.p_win_ppm, 31250, "ewma p_win");
    first = o.p_ewma_ppm;
    ok(first > 14800 && first < 15600, "ewma p_ewma after 5 blocks");
    eq_u32(o.p_ppm, o.p_win_ppm, "ewma p_used is the window");
    ok(o.p_valid, "ewma p_valid");
    ok(o.r_valid == false, "ewma r invalid without packets");
    ok(!o.pause_idle, "ewma not idle");
    ok(!o.pause_carrier, "ewma carriers alive");
    ok(!o.pause_abandoned, "ewma nothing abandoned");

    /* One clean super-block pulls the EWMA down. */
    feed_super(&e, t, 8, 0, alive);
    tbt_fec_est_sample(&e, t, &o);
    ok(o.p_ewma_ppm < first, "ewma falls toward a clean block");
    eq_u32(o.supers_win, 6, "ewma sixth super");
}

static void case_estimator_alive(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;
    u16 i;
    int n;

    /* Groups 0..3 (carriers 0..31) are dead; their misses disappear. */
    alive_all(alive);
    for (i = 0; i < 32; i++)
        tbt_fec_adapt_bitmap_clear(alive, i);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 224, 0, 256);
    for (n = 0; n < 5; n++)
    {
        feed_super(&e, t, 8, 4, alive);
        t += 10 * MS;
    }
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 1120, "alive dead groups excluded from sent");
    eq_u32(o.miss_win, 0, "alive dead groups excluded from miss");
    eq_u32(o.p_win_ppm, 0, "alive dead-group misses do not raise p");

    /* Share 0 of groups 0..15 is dead; half the misses are filtered. */
    alive_all(alive);
    for (i = 0; i < 16; i++)
        tbt_fec_adapt_bitmap_clear(alive, (u16)(i * 8));
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 240, 0, 256);
    for (n = 0; n < 5; n++)
    {
        feed_super(&e, t, 8, 32, alive);
        t += 10 * MS;
    }
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 1200, "alive mixed sent");
    eq_u32(o.miss_win, 80, "alive mixed miss");
    eq_u32(o.p_win_ppm, 66666, "alive mixed p");

    /* Last carrier: group 31, share 7. */
    alive_all(alive);
    tbt_fec_adapt_bitmap_clear(alive, 255);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 255, 0, 256);
    tbt_fec_est_block(&e, t, 31, 8, 0x80u, alive);
    tbt_fec_est_super(&e, t);
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 7, "alive last carrier sent");
    eq_u32(o.miss_win, 0, "alive last carrier miss");
}

static void case_estimator_quality(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;
    int i;

    alive_all(alive);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 256, 0, 256);
    for (i = 0; i < 5; i++)
    {
        feed_super(&e, t, 8, 0, alive);
        t += 10 * MS;
    }

    tbt_fec_est_unknown(&e, t);
    tbt_fec_est_unknown(&e, t);
    tbt_fec_est_abandoned(&e, t, 10);
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.unknown_win, 2, "unknown window");
    eq_u64(o.unknown_total, 2, "unknown total");
    eq_u32(o.abandoned_win, 10, "abandoned window");
    eq_u64(o.abandoned_total, 10, "abandoned total");
    ok(!o.pause_abandoned, "abandoned under 1%");

    tbt_fec_est_abandoned(&e, t, 3); /* 13/1280 > 1% */
    tbt_fec_est_sample(&e, t, &o);
    ok(o.pause_abandoned, "abandoned over 1% freezes");
}

static void case_estimator_idle_and_carriers(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;

    alive_all(alive);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 256, 0, 256);
    feed_super(&e, t, 8, 0, alive);
    tbt_fec_est_sample(&e, t + 200 * MS, &o);
    ok(!o.pause_idle, "idle at 200 ms does not freeze");
    tbt_fec_est_sample(&e, t + 201 * MS, &o);
    ok(o.pause_idle, "idle at 201 ms freezes");

    tbt_fec_est_carriers(&e, 200, 0, 256);
    tbt_fec_est_sample(&e, t, &o);
    ok(o.pause_carrier, "alive under 90% freezes");

    tbt_fec_est_carriers(&e, 256, 30, 256);
    tbt_fec_est_sample(&e, t, &o);
    ok(o.pause_carrier, "stall over 10% freezes");

    tbt_fec_est_carriers(&e, 231, 25, 256);
    tbt_fec_est_sample(&e, t, &o);
    ok(!o.pause_carrier, "alive 90% and stall under 10% pass");
}

static void case_estimator_r_fail(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u64 t = SEC;

    tbt_fec_est_init(&e, 8);
    tbt_fec_est_sample(&e, t, &o);
    ok(!o.r_valid, "r_fail invalid without packets");
    eq_u32(o.r_fail_ppm, 0, "r_fail zero without packets");

    tbt_fec_est_fail(&e, t, 20, 1000);
    tbt_fec_est_sample(&e, t, &o);
    ok(o.r_valid, "r_fail valid");
    eq_u32(o.r_fail_ppm, 20000, "r_fail 20/1000");
    eq_u32(o.requested_win, 20, "r_fail requested");
    eq_u32(o.pkts_win, 1000, "r_fail packets");
}

static void case_estimator_window(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;
    int i;

    alive_all(alive);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 256, 0, 256);
    for (i = 0; i < 5; i++)
    {
        feed_super(&e, t, 8, 8, alive);
        t += 10 * MS;
    }
    tbt_fec_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 1280, "window before the gap");

    /* A 2 s gap starts a fresh window: the old samples are gone. */
    feed_super(&e, t + 2 * SEC, 8, 8, alive);
    tbt_fec_est_sample(&e, t + 2 * SEC, &o);
    eq_u32(o.sent_win, 256, "window after a long gap");
    eq_u32(o.supers_win, 1, "window after a long gap: one super");
    ok(!o.p_valid, "window after a long gap: not enough evidence");
}

static void case_estimator_two_buckets(void)
{
    struct tbt_fec_est e;
    struct tbt_fec_est_out o;
    u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES];
    u64 t = SEC;
    int i;

    alive_all(alive);
    tbt_fec_est_init(&e, 8);
    tbt_fec_est_carriers(&e, 256, 0, 256);
    for (i = 0; i < 2; i++)
        feed_super(&e, t, 8, 8, alive); /* first 500 ms bucket */
    for (i = 0; i < 2; i++)
        feed_super(&e, t + 600 * MS, 8, 8, alive); /* second bucket */

    tbt_fec_est_sample(&e, t + 700 * MS, &o);
    eq_u32(o.sent_win, 1024, "two buckets: sent");
    eq_u32(o.supers_win, 4, "two buckets: supers");
    eq_u32(o.p_win_ppm, 31250, "two buckets: p");
    ok(o.p_valid, "two buckets: the 1 s window spans both");
}

/* ------------------------------------------------------------------ *
 * Control law
 * ------------------------------------------------------------------ */

static void case_up_hysteresis(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 0);

    eval(&a, 10001, 0, t, &out); /* target C1, inside +0.5 pp */
    ok(!out.switch_now, "up: 1.0001% is inside hysteresis");
    eval(&a, 15000, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "up: exactly +0.5 pp is inside hysteresis");
    eval(&a, 15001, 0, t + 500 * MS, &out);
    ok(!out.switch_now, "up: first sample");
    eval(&a, 15001, 0, t + 750 * MS, &out);
    ok(!out.switch_now, "up: second sample");
    eval(&a, 15001, 0, t + 1000 * MS, &out);
    ok(out.switch_now, "up: third sample switches");
    eq_u32(out.from, 0, "up: from C0");
    eq_u32(out.to, 1, "up: to C1");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_UP, "up: reason");
    eq_u32(out.epoch, 1, "up: first epoch");
    eq_u32(out.shape.n, 6, "up: shape n");
    eq_u32(out.shape.m, 2, "up: shape m");
    eq_u32(out.shape.groups, 32, "up: shape groups");
    eq_u32(out.shape.w, 12, "up: shape w");
    eq_u32(out.shape.k, 6, "up: shape k");
    eq_u32(out.shape.d, 6, "up: shape d");

    eval(&a, 40000, 0, t + 1250 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_BUSY, "up: busy until applied");
    tbt_fec_adapt_applied(&a, t + 1000 * MS);
    eq_u32(a.cur, 1, "up: applied");
}

static void case_dwell(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 0);
    eval(&a, 15001, 0, t, &out);
    eval(&a, 15001, 0, t + 250 * MS, &out);
    eval(&a, 15001, 0, t + 500 * MS, &out);
    ok(out.switch_now, "dwell: first up switch");
    tbt_fec_adapt_applied(&a, t + 500 * MS);

    eval(&a, 40000, 0, t + 750 * MS, &out);
    eval(&a, 40000, 0, t + 1000 * MS, &out);
    eval(&a, 40000, 0, t + 1250 * MS, &out);
    ok(!out.switch_now, "dwell: three samples before 3 s");
    eval(&a, 40000, 0, t + 2750 * MS, &out);
    ok(!out.switch_now, "dwell: 2.75 s after the switch");
    eval(&a, 40000, 0, t + 3500 * MS, &out);
    ok(out.switch_now, "dwell: 3 s after the switch");
    eq_u32(out.to, 2, "dwell: to C2");
}

static void case_down(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;
    int i;

    /* r_fail = 1% keeps the down-probe away; this is the six-sample path. */
    tbt_fec_adapt_init(&a, 2);
    for (i = 1; i <= 5; i++)
    {
        eval(&a, 0, 10000, t + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "down: before six samples");
    }
    eval(&a, 0, 10000, t + 1500 * MS, &out);
    ok(out.switch_now, "down: six samples switch");
    eq_u32(out.to, 0, "down: to C0");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_DOWN, "down: reason");
    eq_u32(out.epoch, 1, "down: epoch");
}

static void case_down_dwell(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;
    int i;

    tbt_fec_adapt_init(&a, 0);
    eval(&a, 15001, 0, t, &out);
    eval(&a, 15001, 0, t + 250 * MS, &out);
    eval(&a, 15001, 0, t + 500 * MS, &out);
    ok(out.switch_now, "down-dwell: up to C1");
    tbt_fec_adapt_applied(&a, t + 500 * MS);

    for (i = 1; i <= 6; i++)
        eval(&a, 0, 10000, t + 500 * MS + (u64)i * 250 * MS, &out);
    ok(!out.switch_now, "down-dwell: six samples before 10 s");
    eval(&a, 0, 10000, t + 10250 * MS, &out);
    ok(!out.switch_now, "down-dwell: 9.75 s after the switch");
    eval(&a, 0, 10000, t + 10500 * MS, &out);
    ok(out.switch_now, "down-dwell: 10 s after the switch");
    eq_u32(out.to, 0, "down-dwell: to C0");
}

static void case_reverse_cooldown(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;
    int i;

    tbt_fec_adapt_init(&a, 2);
    for (i = 1; i <= 6; i++)
        eval(&a, 0, 10000, t + (u64)i * 250 * MS, &out);
    ok(out.switch_now, "reverse: down to C0");
    tbt_fec_adapt_applied(&a, t + 1500 * MS);

    eval(&a, 40000, 0, t + 1750 * MS, &out);
    eval(&a, 40000, 0, t + 2000 * MS, &out);
    eval(&a, 40000, 0, t + 2250 * MS, &out);
    ok(!out.switch_now, "reverse: up blocked by the 3 s dwell");
    eval(&a, 40000, 0, t + 4500 * MS, &out);
    ok(!out.switch_now, "reverse: up blocked by the 5 s reverse cooldown");
    eval(&a, 40000, 0, t + 6500 * MS, &out);
    ok(out.switch_now, "reverse: up after 5 s");
    eq_u32(out.to, 2, "reverse: to C2");
}

static void case_hold(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 0);
    eval(&a, 15001, 0, t, &out);
    eval(&a, 15001, 0, t + 250 * MS, &out);
    eval(&a, 15001, 0, t + 500 * MS, &out);
    ok(out.switch_now, "hold: switch 1");
    tbt_fec_adapt_applied(&a, t + 500 * MS);

    eval(&a, 35001, 0, t + 750 * MS, &out);
    eval(&a, 35001, 0, t + 1000 * MS, &out);
    eval(&a, 35001, 0, t + 1250 * MS, &out);
    ok(!out.switch_now, "hold: switch 2 waits for the 3 s dwell");
    eval(&a, 35001, 0, t + 3500 * MS, &out);
    ok(out.switch_now, "hold: switch 2");
    tbt_fec_adapt_applied(&a, t + 3500 * MS);

    eval(&a, 55001, 0, t + 3750 * MS, &out);
    eval(&a, 55001, 0, t + 4000 * MS, &out);
    eval(&a, 55001, 0, t + 4250 * MS, &out);
    ok(!out.switch_now, "hold: switch 3 waits for the 3 s dwell");
    eval(&a, 55001, 0, t + 6500 * MS, &out);
    ok(out.switch_now, "hold: switch 3");
    tbt_fec_adapt_applied(&a, t + 6500 * MS);

    eval(&a, 55001, 0, t + 6750 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_HOLD, "hold: frozen after 3 in 30 s");
    ok(!out.switch_now, "hold: no switch while frozen");
    eval(&a, 55001, 0, t + 37000 * MS, &out);
    ok(out.reason != TBT_FEC_ADAPT_R_HOLD, "hold: expires after 30 s");
}

static void case_rate_limit(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = 100 * SEC;
    int i;

    tbt_fec_adapt_init(&a, 0);
    /* White box: ten applied switches inside the 60 s window. */
    for (i = 0; i < 10; i++)
        a.sw[i] = t - (u64)i * SEC;
    a.sw_n = 10;
    a.sw_head = 0;

    eval(&a, 15001, 0, t, &out);
    eval(&a, 15001, 0, t + 250 * MS, &out);
    eval(&a, 15001, 0, t + 500 * MS, &out);
    ok(!out.switch_now, "rate: ten switches in 60 s block the eleventh");

    a.sw[9] = t - 61 * SEC; /* one leaves the window */
    eval(&a, 15001, 0, t + 750 * MS, &out);
    ok(out.switch_now, "rate: the eleventh switch after one leaves");
}

static void case_probe_commit(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 1);
    eval(&a, 0, 0, t, &out);
    ok(out.switch_now, "probe: down");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PROBE_DOWN, "probe: reason");
    eq_u32(out.to, 0, "probe: to C0");
    eq_u32(out.epoch, 1, "probe: epoch");
    tbt_fec_adapt_applied(&a, t);
    eq_u32(a.cur, 0, "probe: applied");
    ok(a.probe_active, "probe: active");

    eval(&a, 0, 0, t + 9 * SEC, &out);
    ok(!out.switch_now, "probe: 9 s is not enough to commit");
    eval(&a, 0, 0, t + 11 * SEC, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PROBE_COMMIT, "probe: commit");
    ok(!out.switch_now, "probe: commit does not switch");
    eq_u32(a.cur, 0, "probe: commit keeps C0");
    ok(!a.probe_active, "probe: inactive after commit");
}

static void case_probe_revert(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 1);
    eval(&a, 0, 0, t, &out);
    ok(out.switch_now, "revert: probe down");
    tbt_fec_adapt_applied(&a, t);

    eval(&a, 0, 30000, t + 500 * MS, &out);
    ok(!out.switch_now, "revert: one high r_fail sample");
    eval(&a, 0, 30000, t + 750 * MS, &out);
    ok(out.switch_now, "revert: two high r_fail samples");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PROBE_REVERT, "revert: reason");
    eq_u32(out.to, 1, "revert: back to C1");
    eq_u32(out.epoch, 2, "revert: epoch");
    tbt_fec_adapt_applied(&a, t + 750 * MS);
    eq_u32(a.cur, 1, "revert: applied");
    ok(!a.probe_active, "revert: probe inactive");

    /* The probed class is banned for 60 s; 15 s of stability is not enough. */
    eval(&a, 0, 0, t + 750 * MS + 15 * SEC, &out);
    ok(!out.switch_now, "revert: 15 s later the ban still holds");
    eval(&a, 0, 0, t + 750 * MS + 60 * SEC, &out);
    ok(out.switch_now, "revert: the ban expires after 60 s");
    eq_u32(out.to, 0, "revert: probe C0 again");
}

static void case_probe_abort(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 1);
    eval(&a, 0, 0, t, &out);
    ok(out.switch_now, "abort: probe down");
    tbt_fec_adapt_applied(&a, t);

    eval(&a, 40000, 0, t + SEC, &out);
    ok(out.switch_now, "abort: p rose, return to the table");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PROBE_ABORT, "abort: reason");
    eq_u32(out.to, 1, "abort: back to C1");
    tbt_fec_adapt_applied(&a, t + SEC);
    ok(!a.probe_active, "abort: probe inactive");

    eval(&a, 0, 0, t + SEC + 250 * MS, &out);
    ok(!out.switch_now, "abort: no immediate re-probe");
}

static void case_jitter_dwell(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 1);
    eval(&a, 0, 0, t, &out);
    ok(out.switch_now, "jitter: probe down");
    tbt_fec_adapt_applied(&a, t);

    eval(&a, 0, 30000, t + 500 * MS, &out);
    eval(&a, 0, 30000, t + 750 * MS, &out);
    ok(out.switch_now, "jitter: revert");
    tbt_fec_adapt_applied(&a, t + 750 * MS);
    eq_u32(a.dwell_mul, 2, "jitter: dwell multiplier");

    eval(&a, 40000, 0, t + SEC, &out);
    eval(&a, 40000, 0, t + 1250 * MS, &out);
    eval(&a, 40000, 0, t + 1500 * MS, &out);
    ok(!out.switch_now, "jitter: three up samples");
    eval(&a, 40000, 0, t + 6500 * MS, &out);
    ok(!out.switch_now, "jitter: 5.75 s after the revert is not enough");
    eval(&a, 40000, 0, t + 6750 * MS, &out);
    ok(out.switch_now, "jitter: 6 s after the revert switches");
    eq_u32(out.to, 2, "jitter: to C2");
}

static void case_pause_and_wait(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    struct tbt_fec_est_out o;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 0);

    snapshot(&o, 40000, 0);
    o.pause_idle = true;
    tbt_fec_adapt_eval(&a, &o, t, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PAUSED, "pause: idle");
    ok(!out.switch_now, "pause: idle does not switch");

    snapshot(&o, 40000, 0);
    o.pause_carrier = true;
    tbt_fec_adapt_eval(&a, &o, t + 250 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PAUSED, "pause: carrier");

    snapshot(&o, 40000, 0);
    o.pause_abandoned = true;
    tbt_fec_adapt_eval(&a, &o, t + 500 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_PAUSED, "pause: abandoned");

    snapshot(&o, 40000, 0);
    o.p_valid = false;
    tbt_fec_adapt_eval(&a, &o, t + 750 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_R_WAIT, "pause: warm-up");
    ok(!out.switch_now, "pause: warm-up does not switch");
}

static void case_fail_up(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 1);
    eval(&a, 0, 30000, t, &out);
    ok(!out.switch_now, "fail-up: one sample");
    eval(&a, 0, 30000, t + 250 * MS, &out);
    ok(out.switch_now, "fail-up: two samples");
    eq_u32(out.reason, TBT_FEC_ADAPT_R_FAIL_UP, "fail-up: reason");
    eq_u32(out.to, 2, "fail-up: one class up");
    eq_u32(out.epoch, 1, "fail-up: epoch");
}

static void case_epoch_abort(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    u64 t = SEC;

    tbt_fec_adapt_init(&a, 0);
    eval(&a, 15001, 0, t, &out);
    eval(&a, 15001, 0, t + 250 * MS, &out);
    eval(&a, 15001, 0, t + 500 * MS, &out);
    eq_u32(out.epoch, 1, "epoch: first suggestion");
    tbt_fec_adapt_abort(&a);
    eq_u32(a.cur, 0, "epoch: abort keeps the class");

    eval(&a, 15001, 0, t + 750 * MS, &out);
    eval(&a, 15001, 0, t + 1000 * MS, &out);
    eval(&a, 15001, 0, t + 1250 * MS, &out);
    ok(out.switch_now, "epoch: second suggestion");
    eq_u32(out.epoch, 2, "epoch: monotonic after abort");
}

static void case_base_zero(void)
{
    struct tbt_fec_adapt a;
    struct tbt_fec_adapt_out out;
    struct tbt_fec_adapt_out again;
    u64 t = SEC;

    /* A fresh session: the switch layer will set base_pkt/base_block at the
     * frontier, so the pure decision leaves them zero. The controller has no
     * packet or block coordinate at all; the same inputs give the same
     * suggestion. */
    tbt_fec_adapt_init(&a, 0);
    eval(&a, 40000, 0, t, &out);
    eval(&a, 40000, 0, t + 250 * MS, &out);
    eval(&a, 40000, 0, t + 500 * MS, &out);
    ok(out.switch_now, "base 0: first switch");
    eq_u32(out.epoch, 1, "base 0: epoch starts at 1");
    eq_u32(out.to, 2, "base 0: to C2");
    eq_u32(out.shape.n + out.shape.m, 8, "base 0: n+m");
    eq_u64(out.shape.base_pkt, 0, "base 0: base_pkt left to the switch");
    eq_u64(out.shape.base_block, 0, "base 0: base_block left to the switch");

    tbt_fec_adapt_init(&a, 0);
    eval(&a, 40000, 0, t, &again);
    eval(&a, 40000, 0, t + 250 * MS, &again);
    eval(&a, 40000, 0, t + 500 * MS, &again);
    eq_u32(again.to, out.to, "base 0: deterministic target");
    eq_u32(again.epoch, out.epoch, "base 0: deterministic epoch");
    eq_u32(again.shape.n, out.shape.n, "base 0: deterministic shape");
}

static void case_carrier_helpers(void)
{
    u64 now = 10 * SEC;

    ok(tbt_fec_adapt_carrier_pause(89, 0, 100), "carrier: 89% alive");
    ok(!tbt_fec_adapt_carrier_pause(90, 0, 100), "carrier: 90% alive");
    ok(tbt_fec_adapt_carrier_pause(100, 11, 100), "carrier: 11% stalled");
    ok(!tbt_fec_adapt_carrier_pause(100, 10, 100), "carrier: 10% stalled");
    ok(tbt_fec_adapt_carrier_pause(0, 0, 0), "carrier: empty population");

    ok(tbt_fec_adapt_carrier_alive(true, false, true, now, now - 500 * MS,
                                   100000),
       "alive: 0.5 s at rtt 100 ms");
    ok(!tbt_fec_adapt_carrier_alive(true, false, true, now, now - SEC,
                                    100000),
       "alive: 1 s at rtt 100 ms");
    ok(tbt_fec_adapt_carrier_alive(true, false, true, now, now - 1900 * MS,
                                   500000),
       "alive: 1.9 s at rtt 500 ms");
    ok(!tbt_fec_adapt_carrier_alive(true, false, true, now, now - 2 * SEC,
                                    500000),
       "alive: 2 s at rtt 500 ms");
    ok(!tbt_fec_adapt_carrier_alive(false, false, true, now, now, 100000),
       "alive: down");
    ok(!tbt_fec_adapt_carrier_alive(true, true, true, now, now, 100000),
       "alive: stopping");
    ok(!tbt_fec_adapt_carrier_alive(true, false, false, now, now, 100000),
       "alive: not established");
    ok(!tbt_fec_adapt_carrier_alive(true, false, true, now, 0, 100000),
       "alive: never received");
}

int main(void)
{
    case_class_table();
    case_estimator_ewma();
    case_estimator_alive();
    case_estimator_quality();
    case_estimator_idle_and_carriers();
    case_estimator_r_fail();
    case_estimator_window();
    case_estimator_two_buckets();
    case_up_hysteresis();
    case_dwell();
    case_down();
    case_down_dwell();
    case_reverse_cooldown();
    case_hold();
    case_rate_limit();
    case_probe_commit();
    case_probe_revert();
    case_probe_abort();
    case_jitter_dwell();
    case_pause_and_wait();
    case_fail_up();
    case_epoch_abort();
    case_base_zero();
    case_carrier_helpers();

    printf("test_fec_adapt: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
