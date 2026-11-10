/* test_fec_adapt_sclc.c - host test for the SC-LDPC estimator and rate law.
 *
 * WHAT THIS IS
 *   The exact code from tbtun/tbt_fec_adapt_sclc.c, compiled for the host.
 *   It checks:
 *
 *     - the rate ladder: actual rates, n/m for the 256-share downlink, the
 *       32-share uplink and the 64/32-carrier variants, and rejection of
 *       rates and geometries the codec cannot build;
 *     - the estimator: eight 500 ms buckets, the 4 s window, the 1 s fast
 *       window, the EWMA, 256-bit miss and alive masks, grouped blocks,
 *       r_fail, codec failures, bursts, pauses and clock steps;
 *     - the rate law: the 1-p-0.20 target, down quantization, 2 pp
 *       hysteresis, fast start, two-sample down, six-sample up, dwell,
 *       reverse cooldown, the 10-in-60 s cap, the 3-in-30 s hold, fail-down,
 *       probe up/commit/revert/abort, post-commit rollback, and the default
 *       off / runtime enable / disable paths;
 *     - convergence and non-oscillation over 60 s at 0, 1, 3, 5, 10, 15,
 *       25, 35, 45, 55 and 100% loss, driving the real estimator.
 *
 *   Time is virtual: every case passes explicit nanosecond timestamps, so the
 *   test runs instantly and is deterministic.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_late.h"

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

#define MS 1000000ull
#define SEC 1000000000ull

/* ------------------------------------------------------------------ *
 * Bitmap and estimator helpers
 * ------------------------------------------------------------------ */

static void alive_all(u8 bm[TBT_FEC_BITMAP_BYTES])
{
    memset(bm, 0xff, TBT_FEC_BITMAP_BYTES);
}

static void mask_zero(u8 m[TBT_FEC_BITMAP_BYTES])
{
    memset(m, 0, TBT_FEC_BITMAP_BYTES);
}

static void mask_set(u8 m[TBT_FEC_BITMAP_BYTES], u16 bit)
{
    m[bit >> 3] |= (u8)(1u << (bit & 7));
}

static void mask_first(u8 m[TBT_FEC_BITMAP_BYTES], u16 count)
{
    u16 i;

    mask_zero(m);
    for (i = 0; i < count && i < TBT_FEC_MAX_SHARES; i++)
        mask_set(m, i);
}

/* One downlink super-block: one group of `real` DATA shares, the first
 * `miss` shares missing. */
static void feed_down(struct tbt_fec_adapt_sclc_est *e, u64 now_ns, u16 real,
                      u16 miss, const u8 alive[TBT_FEC_BITMAP_BYTES])
{
    u8 mask[TBT_FEC_BITMAP_BYTES];

    mask_first(mask, miss);
    tbt_fec_adapt_sclc_est_block(e, now_ns, 0, real, mask, alive);
}

/* One uplink super-block: eight groups of 32 shares, share 0 of each group
 * missing. */
static void feed_up(struct tbt_fec_adapt_sclc_est *e, u64 now_ns,
                    u16 miss_per_group,
                    const u8 alive[TBT_FEC_BITMAP_BYTES])
{
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 g;

    mask_first(mask, miss_per_group);
    for (g = 0; g < 8; g++)
        tbt_fec_adapt_sclc_est_block(e, now_ns, g, 32, mask, alive);
}

/* ------------------------------------------------------------------ *
 * Controller helpers
 * ------------------------------------------------------------------ */

static void snap(struct tbt_fec_adapt_sclc_est_out *o, u32 p_ppm, u32 r_ppm)
{
    memset(o, 0, sizeof(*o));
    o->p_ppm = p_ppm;
    o->p_ewma_ppm = p_ppm;
    o->p_win_ppm = p_ppm;
    o->p_fast_ppm = p_ppm;
    o->r_fail_ppm = r_ppm;
    o->r_valid = true;
    o->p_valid = true;
    o->sent_win = 4096;
    o->supers_win = 16;
    o->alive = 256;
    o->carriers = 256;
}

static void eval(struct tbt_fec_adapt_sclc_ctl *c, u32 p_ppm, u32 r_ppm,
                 u64 now_ns, struct tbt_fec_adapt_sclc_out *out)
{
    struct tbt_fec_adapt_sclc_est_out o;

    snap(&o, p_ppm, r_ppm);
    tbt_fec_adapt_sclc_ctl_eval(c, &o, now_ns, out);
}

static void init_ctl(struct tbt_fec_adapt_sclc_ctl *c, bool enabled,
                     u8 rate_pct, u16 shares, u16 groups)
{
    int rc = tbt_fec_adapt_sclc_ctl_init(c, enabled, rate_pct, shares, groups);

    eq_u32((u32)rc, 0, "ctl init");
}

/* Start a controller at rate 50 and spend its one fast start on a steady
 * sample, so later cases exercise the normal run/dwell paths. */
static void init_steady(struct tbt_fec_adapt_sclc_ctl *c, u32 p_ppm,
                        u32 r_ppm, u64 now_ns)
{
    struct tbt_fec_adapt_sclc_out out;

    init_ctl(c, true, 50, 256, 1);
    eval(c, p_ppm, r_ppm, now_ns, &out);
    ok(!out.switch_now, "steady: the start sample does not switch");
}

/* ------------------------------------------------------------------ *
 * Rate ladder and shape validation
 * ------------------------------------------------------------------ */

static void case_rate_ladder(void)
{
    static const u8 pct[TBT_FEC_ADAPT_SCLC_LEVELS] = {25, 30, 40, 50, 60, 70};
    static const u32 ppm[TBT_FEC_ADAPT_SCLC_LEVELS] = {
        250000u, 299985u, 400000u, 500000u, 592593u, 695652u};
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        eq_u32(tbt_fec_adapt_sclc_rate_index(pct[i]), i, "rate: index");
        eq_u32(tbt_fec_adapt_sclc_rate_ppm(pct[i]), ppm[i], "rate: ppm");
        ok(tbt_fec_adapt_sclc_rate_at(i) != NULL, "rate: entry");
        eq_u32(tbt_fec_adapt_sclc_rate_at(i)->ppm, ppm[i], "rate: entry ppm");
    }
    eq_u32(tbt_fec_adapt_sclc_rate_index(35), 0xff, "rate: 35 absent");
    eq_u32(tbt_fec_adapt_sclc_rate_index(45), 0xff, "rate: 45 absent");
    eq_u32(tbt_fec_adapt_sclc_rate_index(0), 0xff, "rate: 0 absent");
    eq_u32(tbt_fec_adapt_sclc_rate_index(80), 0xff, "rate: 80 absent");
    eq_u32(tbt_fec_adapt_sclc_rate_ppm(35), 0, "rate: absent ppm");
    ok(tbt_fec_adapt_sclc_rate_at(TBT_FEC_ADAPT_SCLC_LEVELS) == NULL,
       "rate: past the end");
}

static void case_shape_downlink(void)
{
    static const u8 pct[TBT_FEC_ADAPT_SCLC_LEVELS] = {25, 30, 40, 50, 60, 70};
    static const u16 n[TBT_FEC_ADAPT_SCLC_LEVELS] = {64, 76, 102, 128, 151,
                                                     178};
    static const u16 m[TBT_FEC_ADAPT_SCLC_LEVELS] = {192, 180, 154, 128, 105,
                                                     78};
    struct tbt_fec_adapt_sclc_shape s;
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        eq_u32((u32)tbt_fec_adapt_sclc_shape(pct[i], 256, 1, &s), 0,
               "shape: downlink ok");
        eq_u32(s.n, n[i], "shape: downlink n");
        eq_u32(s.m, m[i], "shape: downlink m");
        eq_u32(s.groups, 1, "shape: downlink groups");
        eq_u32(s.rate_pct, pct[i], "shape: downlink rate_pct");
        eq_u32(s.rate_ppm, tbt_fec_adapt_sclc_rate_ppm(pct[i]),
               "shape: downlink ppm");
    }
}

static void case_shape_uplink(void)
{
    static const u8 pct[TBT_FEC_ADAPT_SCLC_LEVELS] = {25, 30, 40, 50, 60, 70};
    static const u16 n[TBT_FEC_ADAPT_SCLC_LEVELS] = {8, 9, 12, 16, 18, 22};
    static const u16 m[TBT_FEC_ADAPT_SCLC_LEVELS] = {24, 23, 20, 16, 14, 10};
    struct tbt_fec_adapt_sclc_shape s;
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        eq_u32((u32)tbt_fec_adapt_sclc_shape(pct[i], 32, 8, &s), 0,
               "shape: uplink ok");
        eq_u32(s.n, n[i], "shape: uplink n");
        eq_u32(s.m, m[i], "shape: uplink m");
        eq_u32(s.groups, 8, "shape: uplink groups");
    }

    /* 64-carrier downlink and 2x32-carrier uplink use the same formula. */
    eq_u32((u32)tbt_fec_adapt_sclc_shape(70, 64, 1, &s), 0, "shape: 64/70");
    eq_u32(s.n, 44, "shape: 64/70 n");
    eq_u32(s.m, 20, "shape: 64/70 m");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(30, 32, 2, &s), 0, "shape: 2x32/30");
    eq_u32(s.n, 9, "shape: 2x32/30 n");
    eq_u32(s.m, 23, "shape: 2x32/30 m");
}

static void case_shape_reject(void)
{
    struct tbt_fec_adapt_sclc_shape s;

    eq_u32((u32)tbt_fec_adapt_sclc_shape(35, 256, 1, &s), (u32)-1,
           "shape: reject 35");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(45, 256, 1, &s), (u32)-1,
           "shape: reject 45");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 0, 1, &s), (u32)-1,
           "shape: reject zero shares");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 1, 1, &s), (u32)-1,
           "shape: reject one share");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 257, 1, &s), (u32)-1,
           "shape: reject 257 shares");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 256, 0, &s), (u32)-1,
           "shape: reject zero groups");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 256, 2, &s), (u32)-1,
           "shape: reject 512 carriers");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(25, 2, 1, &s), (u32)-1,
           "shape: reject empty n");
    eq_u32((u32)tbt_fec_adapt_sclc_shape(50, 256, 1, NULL), (u32)-1,
           "shape: reject NULL");
}

/* ------------------------------------------------------------------ *
 * Estimator
 * ------------------------------------------------------------------ */

static void case_est_window(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;
    u16 i;

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    alive_all(alive);

    /* 16 supers, 250 ms apart, one miss each. */
    for (i = 0; i < 16; i++)
    {
        feed_down(&e, t + (u64)i * 250 * MS, 256, 1, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 3750 * MS, &o);
    eq_u32(o.sent_win, 4096, "est: 16 supers sent");
    eq_u32(o.miss_win, 16, "est: 16 supers miss");
    eq_u32(o.supers_win, 16, "est: 16 supers");
    eq_u32(o.p_win_ppm, 3906, "est: p = 16/4096");
    eq_u32(o.p_ppm, o.p_ewma_ppm > o.p_win_ppm ? o.p_ewma_ppm : o.p_win_ppm,
           "est: p_used = max");
    ok(o.p_valid, "est: valid after 8 supers and 512 shares");
    ok(!o.pause_idle, "est: not idle");
    ok(!o.pause_carrier, "est: all carriers alive");
    ok(!o.pause_abandoned, "est: no abandoned shares");

    /* At 4 s the oldest bucket (2 supers) leaves the window. */
    tbt_fec_adapt_sclc_est_sample(&e, t + 4 * SEC, &o);
    eq_u32(o.sent_win, 3584, "est: 4 s window drops the oldest bucket");
    eq_u32(o.supers_win, 14, "est: 4 s window supers");
    eq_u32(o.miss_win, 14, "est: 4 s window miss");
    eq_u32(o.p_win_ppm, 3906, "est: 4 s window p");
    /* The fast 1 s window keeps the two newest buckets (4 supers). */
    eq_u32(o.p_fast_ppm, 3906, "est: fast p");
}

static void case_est_ewma(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;
    int i;

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    alive_all(alive);

    feed_down(&e, t, 256, 26, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    eq_u32(o.p_ewma_ppm, 12695, "est: first EWMA is p/8");
    eq_u32(o.p_win_ppm, 101562, "est: window p");
    eq_u32(o.p_ppm, 101562, "est: p_used takes the window");

    for (i = 0; i < 64; i++)
    {
        feed_down(&e, t + (u64)(i + 1) * 250 * MS, 256, 26, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)(i + 1) * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 16 * SEC, &o);
    ok(o.p_ewma_ppm > 101000 && o.p_ewma_ppm < 101563,
       "est: EWMA converges to the block rate");
}

static void case_late_ledger(void)
{
    struct tbt_fec_late_ctx ctx;
    struct tbt_fec_late_rec *rec;
    u8 alive[TBT_FEC_BITMAP_BYTES] = {0};
    u8 sc[TBT_FEC_BITMAP_BYTES] = {0};
    u8 cf[TBT_FEC_BITMAP_BYTES] = {0};
    u8 out[TBT_FEC_MAX_SHARES];
    u8 correct[TBT_FEC_BITMAP_BYTES];
    bool has;
    int i;

    for (i = 0; i < 256; i++)
        tbt_fec_late_set(alive, (u16)i);
    memset(&ctx, 0, sizeof(ctx));
    ctx.epoch = 7;
    ctx.n = 128;
    ctx.m = 128;
    ctx.groups = 1;
    ctx.next_super = 100;
    for (i = 0; i < 51; i++)
        tbt_fec_late_set(sc, (u16)i);
    ok(tbt_fec_late_note_miss(&ctx, 100, 0, sc, cf, alive, 256, 128),
       "ledger: note 51 counted misses");
    memset(correct, 0, sizeof(correct));
    tbt_fec_late_finalize(&ctx, 100, 42, correct);
    rec = tbt_fec_late_find(&ctx, 100);
    eq_u32(rec ? rec->miss : 0, 51, "ledger: 51 misses committed");
    for (i = 0; i < 38; i++)
    {
        enum tbt_fec_late_evt ev = tbt_fec_late_credit(
            &ctx, 100, 0, (u16)i, &has);

        ok(ev == TBT_FEC_LATE_SETTLED && has,
           "ledger: post-fold miss settles once");
        if (ev == TBT_FEC_LATE_SETTLED && has && rec->miss)
            rec->miss--;
    }
    eq_u32(rec->miss, 13, "ledger: 38 in-flight shares cancel to 13");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, &has) == TBT_FEC_LATE_DUP,
       "ledger: repeated late share is a duplicate");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 40, &has) ==
           TBT_FEC_LATE_SETTLED && has,
       "ledger: a new late share still settles");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 40, &has) == TBT_FEC_LATE_DUP,
       "ledger: the second copy is a duplicate");

    /* Block B's miss is untouched by block A's late share. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.epoch = 7;
    ctx.n = 128;
    ctx.m = 128;
    ctx.groups = 1;
    ctx.next_super = 102;
    memset(sc, 0, sizeof(sc));
    tbt_fec_late_set(sc, 0);
    tbt_fec_late_note_miss(&ctx, 100, 0, sc, cf, alive, 256, 128);
    tbt_fec_late_note_miss(&ctx, 101, 0, sc, cf, alive, 256, 128);
    memset(correct, 0, sizeof(correct));
    tbt_fec_late_finalize(&ctx, 100, 1, correct);
    tbt_fec_late_finalize(&ctx, 101, 2, correct);
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, &has) ==
           TBT_FEC_LATE_SETTLED,
       "ledger: block A settles");
    eq_u32(tbt_fec_late_find(&ctx, 101)->miss, 1,
           "ledger: block B miss is untouched");

    /* Old epoch is unknown, never credited. */
    ctx.epoch = 8;
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, &has) ==
           TBT_FEC_LATE_UNKNOWN,
       "ledger: old epoch is unknown");

    /* Eviction is bounded and old records become unknown. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.epoch = 9;
    ctx.n = 128;
    ctx.m = 128;
    ctx.groups = 1;
    ctx.next_super = 1000;
    memset(sc, 0, sizeof(sc));
    tbt_fec_late_set(sc, 0);
    for (i = 0; i < TBT_FEC_LATE_RECS + 1; i++)
    {
        tbt_fec_late_note_miss(&ctx, (u64)(900 + i), 0, sc, cf, alive,
                               256, 128);
        memset(correct, 0, sizeof(correct));
        tbt_fec_late_finalize(&ctx, (u64)(900 + i), (u64)i, correct);
    }
    ok(tbt_fec_late_credit(&ctx, 900, 0, 0, &has) ==
           TBT_FEC_LATE_UNKNOWN,
       "ledger: evicted record is unknown");

    /* share 255 on a 1x256 shape is in bounds. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.epoch = 10;
    ctx.n = 128;
    ctx.m = 128;
    ctx.groups = 1;
    ctx.next_super = 200;
    memset(out, 0, sizeof(out));
    ok(tbt_fec_late_credit(&ctx, 200, 0, 255, &has) ==
           TBT_FEC_LATE_PENDING,
       "ledger: share 255 pending");
    tbt_fec_late_take(&ctx, 200, 0, out);
    ok(out[255] == 1, "ledger: share 255 take is in bounds");
}

static void case_est_late_credit(void)
{
    struct tbt_fec_adapt_sclc_est e;

    memset(&e, 0, sizeof(e));
    e.shares = 256;
    e.cur = 1;
    e.win[0].sent = 1000;
    e.win[0].miss = 100;
    tbt_fec_adapt_sclc_est_recompute(&e);
    eq_u32(e.p_ewma_ppm, 100000, "late credit: true 10% loss");
    ok(tbt_fec_adapt_sclc_est_correct_miss(&e, 0),
       "late credit: exact bucket corrected");
    eq_u32(e.p_ewma_ppm, 99000,
           "late credit: p drops by the exact 1/1000, not 7800");
    ok(!tbt_fec_adapt_sclc_est_correct_miss(&e, 1),
       "late credit: an empty bucket cannot be credited");
    eq_u32(e.p_ewma_ppm, 99000,
           "late credit: empty-bucket correction leaves p unchanged");
    ok(tbt_fec_adapt_sclc_est_correct_miss(&e, 0),
       "late credit: a second real miss still corrects");
    eq_u32(e.p_ewma_ppm, 98000,
           "late credit: second correction is exact");
}

static void case_est_ewma_ring(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES] = {0};
    u8 miss[TBT_FEC_BITMAP_BYTES] = {0};
    u64 now = 1000000000ull;
    int i;

    memset(&e, 0, sizeof(e));
    tbt_fec_adapt_sclc_est_init(&e, 256);
    e.carriers = 256;
    for (i = 0; i < 256; i++)
        tbt_fec_adapt_bitmap_set(alive, (u16)i);
    for (i = 0; i < 51; i++)
        tbt_fec_adapt_bitmap_set(miss, (u16)i);
    tbt_fec_adapt_sclc_est_block(&e, now, 0, 128, miss, alive);
    tbt_fec_adapt_sclc_est_super(&e, now);
    for (i = 0; i < 38; i++)
        ok(tbt_fec_adapt_sclc_est_correct_super(&e, 1),
           "ewma ring: in-flight miss corrects");
    memset(&o, 0, sizeof(o));
    tbt_fec_adapt_sclc_est_sample(&e, now, &o);
    eq_u32(o.p_win_ppm, 101562, "ewma ring: p_win exact 13/128");
    {
        u8 miss13[TBT_FEC_BITMAP_BYTES] = {0};

        for (i = 0; i < 13; i++)
            tbt_fec_adapt_bitmap_set(miss13, (u16)i);
        for (i = 0; i < 63; i++)
        {
            tbt_fec_adapt_sclc_est_block(&e, now, 0, 128, miss13, alive);
            tbt_fec_adapt_sclc_est_super(&e, now);
        }
    }
    memset(&o, 0, sizeof(o));
    tbt_fec_adapt_sclc_est_sample(&e, now, &o);
    eq_u32(o.p_win_ppm, 101562, "ewma ring: p_win exact 13/128");
    ok(o.p_ewma_ppm > 99500 && o.p_ewma_ppm < 103500,
       "ewma ring: p_ewma converges to the corrected history");

    {
        u8 zero[TBT_FEC_BITMAP_BYTES] = {0};

        for (i = 0; i < 65; i++)
        {
            tbt_fec_adapt_sclc_est_block(&e, now, 0, 1, zero, alive);
            tbt_fec_adapt_sclc_est_super(&e, now);
        }
    }
    ok(!tbt_fec_adapt_sclc_est_correct_super(&e, 1),
       "ewma ring: evicted super cannot be corrected");
}

static void case_est_alive(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u8 miss[TBT_FEC_BITMAP_BYTES];
    u16 i;
    u64 t = SEC;

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 128, 0, 256);
    alive_all(alive);
    for (i = 0; i < 128; i++)
        tbt_fec_adapt_bitmap_clear(alive, i);

    /* Misses on dead carriers do not enter either side of the ratio. */
    mask_first(miss, 128);
    tbt_fec_adapt_sclc_est_block(&e, t, 0, 256, miss, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 128, "est alive: only live shares sent");
    eq_u32(o.miss_win, 0, "est alive: dead-carrier miss ignored");
    eq_u32(o.p_win_ppm, 0, "est alive: p zero");

    /* A miss on a live carrier counts. */
    mask_zero(miss);
    mask_set(miss, 200);
    tbt_fec_adapt_sclc_est_block(&e, t + 250 * MS, 0, 256, miss, alive);
    tbt_fec_adapt_sclc_est_super(&e, t + 250 * MS);
    tbt_fec_adapt_sclc_est_sample(&e, t + 250 * MS, &o);
    eq_u32(o.sent_win, 256, "est alive: two supers sent");
    eq_u32(o.miss_win, 1, "est alive: live miss counts");
    eq_u32(o.p_win_ppm, 3906, "est alive: p = 1/256");
}

static void case_est_groups(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;
    u16 i;

    tbt_fec_adapt_sclc_est_init(&e, 32);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    alive_all(alive);

    for (i = 0; i < 8; i++)
    {
        feed_up(&e, t + (u64)i * 250 * MS, 1, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 1750 * MS, &o);
    eq_u32(o.sent_win, 2048, "est group: 8 supers x 256 shares");
    eq_u32(o.miss_win, 64, "est group: one miss per group");
    eq_u32(o.supers_win, 8, "est group: eight supers");
    eq_u32(o.p_win_ppm, 31250, "est group: p = 8/256");
    ok(o.p_valid, "est group: valid");
}

static void case_est_pauses(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;

    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(o.pause_idle, "pause: no activity is idle");

    feed_down(&e, t, 256, 0, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_sample(&e, t + 1900 * MS, &o);
    ok(!o.pause_idle, "pause: 1.9 s is not idle");
    tbt_fec_adapt_sclc_est_sample(&e, t + 2100 * MS, &o);
    ok(o.pause_idle, "pause: 2.1 s is idle");

    tbt_fec_adapt_sclc_est_carriers(&e, 89, 0, 100);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(o.pause_carrier, "pause: 89% alive");
    tbt_fec_adapt_sclc_est_carriers(&e, 90, 0, 100);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(!o.pause_carrier, "pause: 90% alive");
    tbt_fec_adapt_sclc_est_carriers(&e, 100, 11, 100);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(o.pause_carrier, "pause: 11% stalled");
    tbt_fec_adapt_sclc_est_carriers(&e, 100, 10, 100);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(!o.pause_carrier, "pause: 10% stalled");

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    feed_down(&e, t, 256, 0, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_abandoned(&e, t, 3);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(o.pause_abandoned, "pause: abandoned 3/256 is above 1%");
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    feed_down(&e, t, 256, 0, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_abandoned(&e, t, 2);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(!o.pause_abandoned, "pause: abandoned 2/256 is below 1%");
}

static void case_est_fail(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u64 t = SEC;

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_fail(&e, t, 10, 1000);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    ok(o.r_valid, "r_fail: valid");
    eq_u32(o.r_fail_ppm, 10000, "r_fail: 10/1000");

    tbt_fec_adapt_sclc_est_sample(&e, t + 1 * SEC, &o);
    eq_u32(o.r_fail_ppm, 10000, "r_fail: the fast window keeps 1 s");
    tbt_fec_adapt_sclc_est_sample(&e, t + 1500 * MS, &o);
    eq_u32(o.r_fail_ppm, 0, "r_fail: 1.5 s later the sample expired");
    ok(!o.r_valid, "r_fail: no packets means invalid");

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_fail(&e, t, 20, 1000);
    tbt_fec_adapt_sclc_est_codec_fail(&e, t, 10);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    eq_u32(o.codec_fail_ppm, 0, "codec_fail: no missing shares");
    eq_u32(o.codec_fail_win, 10, "codec_fail: window count");
}

static void case_est_codec_fail(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;

    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    feed_down(&e, t, 256, 100, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_codec_fail(&e, t, 10);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    eq_u32(o.codec_fail_ppm, 100000, "codec_fail: 10 of 100 missing");
}

static void case_est_burst(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;
    int i;

    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    /* 4 s at 5%, then 1 s at 50%. */
    for (i = 0; i < 16; i++)
    {
        feed_down(&e, t + (u64)i * 250 * MS, 256, 13, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    for (i = 0; i < 4; i++)
    {
        feed_down(&e, t + (u64)(16 + i) * 250 * MS, 256, 128, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)(16 + i) * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 5 * SEC, &o);
    ok(o.p_fast_ppm > 400000, "burst: the fast window sees the burst");
    ok(o.p_fast_ppm > o.p_ewma_ppm + TBT_FEC_ADAPT_SCLC_BURST_GAP_PPM,
       "burst: fast is above EWMA");
    ok(o.burst, "burst: flagged");

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    for (i = 0; i < 20; i++)
    {
        feed_down(&e, t + (u64)i * 250 * MS, 256, 13, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 5 * SEC, &o);
    ok(!o.burst, "burst: steady loss is not a burst");
}

static void case_est_clock(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = 10 * SEC;

    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    feed_down(&e, t, 256, 26, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);

    /* A backward clock step starts a fresh window instead of corrupting it. */
    tbt_fec_adapt_sclc_est_super(&e, t - 5 * SEC);
    tbt_fec_adapt_sclc_est_sample(&e, t - 5 * SEC, &o);
    eq_u32(o.sent_win, 0, "clock: backward step clears the window");
    eq_u32(o.p_win_ppm, 0, "clock: backward step p zero");

    feed_down(&e, t - 5 * SEC + 250 * MS, 256, 26, alive);
    tbt_fec_adapt_sclc_est_super(&e, t - 5 * SEC + 250 * MS);
    tbt_fec_adapt_sclc_est_sample(&e, t - 5 * SEC + 250 * MS, &o);
    eq_u32(o.sent_win, 256, "clock: resumes after the step");
}

/* ------------------------------------------------------------------ *
 * Rate law: target, hysteresis, dwell
 * ------------------------------------------------------------------ */

static void case_ctl_targets(void)
{
    static const struct
    {
        u32 p_ppm;
        u8 idx;
    } cases[] = {
        {0, 5},          {10000, 5},    {30000, 5},    {50000, 5},
        {100000, 5},     {104348, 5},   {104349, 4},   {105000, 4},
        {120000, 4},     {200000, 4},   {207407, 4},   {207408, 3},
        {208000, 3},     {250000, 3},   {300000, 3},   {300001, 2},
        {350000, 2},     {400000, 2},   {400001, 1},   {450000, 1},
        {500000, 1},     {500016, 0},   {550000, 0},   {1000000, 0},
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct tbt_fec_adapt_sclc_ctl c;
        struct tbt_fec_adapt_sclc_out out;

        init_ctl(&c, true, 50, 256, 1);
        eval(&c, cases[i].p_ppm, 0, SEC, &out);
        eq_u32(out.to, cases[i].idx, "target: ladder index");
        eq_u32(out.shape.rate_pct,
               tbt_fec_adapt_sclc_rate_at(cases[i].idx)->pct,
               "target: rate_pct");
        if (cases[i].idx != 3)
            ok(out.switch_now, "target: fast start switches");
        else
            ok(!out.switch_now, "target: the start rate is the target");
    }

    /* An over-range p is clamped, not wrapped. */
    {
        struct tbt_fec_adapt_sclc_ctl c;
        struct tbt_fec_adapt_sclc_out out;

        init_steady(&c, 250000, 0, SEC);
        eval(&c, 2000000, 0, SEC + 250 * MS, &out);
        ok(!out.switch_now, "target: clamp needs the second sample");
        eval(&c, 2000000, 0, SEC + 500 * MS, &out);
        eq_u32(out.to, 0, "target: p above 1e6 clamps to the low rate");
    }
}

static void case_ctl_hysteresis_up(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    int i;

    /* Target 4 begins at p = 20.74%; the 2 pp guard needs p <= 18.74%. */
    init_steady(&c, 250000, 10000, t);
    for (i = 1; i <= 12; i++)
    {
        eval(&c, 190000, 10000, t + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "up hyst: 19% stays inside the band");
    }
    for (i = 1; i <= 6; i++)
        eval(&c, 187400, 10000, t + 3 * SEC + (u64)i * 250 * MS, &out);
    ok(out.switch_now, "up hyst: 18.74% clears the band after six samples");
    eq_u32(out.to, 4, "up hyst: to rate 60");
}

static void case_ctl_down_hysteresis(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    int i;

    /* Target 2 begins at p = 30%; the 2 pp guard needs p >= 32%. */
    init_steady(&c, 250000, 0, t);
    for (i = 1; i <= 12; i++)
    {
        eval(&c, 310000, 0, t + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "down hyst: 31% stays inside the band");
    }
    eval(&c, 320000, 0, t + 3250 * MS, &out);
    ok(!out.switch_now, "down hyst: first 32% sample");
    eval(&c, 320000, 0, t + 3500 * MS, &out);
    ok(out.switch_now, "down hyst: second 32% sample");
    eq_u32(out.to, 2, "down hyst: to rate 40");
    eq_u32(out.shape.n, 102, "down hyst: n");
    eq_u32(out.shape.m, 154, "down hyst: m");
}

static void case_ctl_up_dwell(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    /* The first sample has r_fail above 0.3%, so fast start cannot jump up. */
    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 10000, t, &out);
    ok(!out.switch_now, "up dwell: fast start blocked by r_fail");

    /* Six samples, counting the start sample, move 50 -> 60. */
    eval(&c, 50000, 10000, t + 250 * MS, &out);
    eval(&c, 50000, 10000, t + 500 * MS, &out);
    eval(&c, 50000, 10000, t + 750 * MS, &out);
    eval(&c, 50000, 10000, t + 1000 * MS, &out);
    ok(!out.switch_now, "up dwell: five samples are not enough");
    eval(&c, 50000, 10000, t + 1250 * MS, &out);
    ok(out.switch_now, "up dwell: six samples move one step");
    eq_u32(out.to, 4, "up dwell: to rate 60");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 1250 * MS);

    /* Five more samples arm the run; the 10 s dwell still holds. */
    {
        int i;

        for (i = 1; i <= 5; i++)
        {
            eval(&c, 50000, 10000, t + 1250 * MS + (u64)i * 250 * MS,
                 &out);
            ok(!out.switch_now, "up dwell: armed samples do not switch");
        }
    }
    eval(&c, 50000, 10000, t + 11000 * MS, &out);
    ok(!out.switch_now, "up dwell: 9.75 s after the switch");
    eval(&c, 50000, 10000, t + 11250 * MS, &out);
    ok(out.switch_now, "up dwell: 10 s after the switch");
    eq_u32(out.to, 5, "up dwell: to rate 70");
}

static void case_ctl_down_fast(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 70, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(!out.switch_now, "down fast: the start rate is the target");

    eval(&c, 400000, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "down fast: one sample is not enough");
    eval(&c, 400000, 0, t + 500 * MS, &out);
    ok(out.switch_now, "down fast: two samples switch");
    eq_u32(out.to, 2, "down fast: multi-step to rate 40");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_DOWN, "down fast: reason");
}

static void case_ctl_fail_down(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 60, 256, 1);
    eval(&c, 150000, 10000, t, &out);
    ok(!out.switch_now, "fail down: start at rate 60");

    eval(&c, 150000, 30000, t + 250 * MS, &out);
    ok(!out.switch_now, "fail down: one high sample");
    eval(&c, 150000, 30000, t + 500 * MS, &out);
    ok(out.switch_now, "fail down: two high samples");
    eq_u32(out.to, 3, "fail down: one step down");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN, "fail down: reason");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 500 * MS);

    /* Raises are banned for 30 s even when p says the top rate. */
    eval(&c, 0, 0, t + 30 * SEC, &out);
    ok(!out.switch_now, "fail down: raise still banned at 29.5 s");
    {
        int i;

        /* The ban expires at 30.5 s; six allowed samples then move up. */
        eval(&c, 0, 0, t + 30250 * MS, &out);
        ok(!out.switch_now, "fail down: still banned at 30.25 s");
        for (i = 0; i < 5; i++)
        {
            eval(&c, 0, 0, t + 30500 * MS + (u64)i * 250 * MS, &out);
            ok(!out.switch_now, "fail down: five samples after the ban");
        }
        eval(&c, 0, 0, t + 30500 * MS + 5 * 250 * MS, &out);
        ok(out.switch_now, "fail down: raise after the ban");
        eq_u32(out.to, 4, "fail down: one step up");
    }
}

static void case_ctl_burst(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_est_out o;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 60, 256, 1);
    eval(&c, 150000, 0, t, &out);
    ok(!out.switch_now, "burst: start at rate 60");

    /* p_used is still 15% (target rate 60); the 1 s burst is 40% and must
     * drive this tick's target down to rate 40. */
    snap(&o, 150000, 0);
    o.p_fast_ppm = 400000;
    o.burst = true;
    tbt_fec_adapt_sclc_ctl_eval(&c, &o, t + 250 * MS, &out);
    ok(out.switch_now, "burst: one sample switches");
    eq_u32(out.to, 2, "burst: p_fast drives the target to rate 40");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_BURST, "burst: reason");

    /* Without the flag the same sample waits for the second evaluation. */
    init_ctl(&c, true, 60, 256, 1);
    eval(&c, 150000, 0, t, &out);
    eval(&c, 150000, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "burst: no flag, no switch");
}

static void case_ctl_hold(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 250000, 10000, t, &out);
    ok(!out.switch_now, "hold: start at rate 50");

    /* Three fail-down steps, each after the 3 s down dwell. */
    eval(&c, 250000, 30000, t + 250 * MS, &out);
    eval(&c, 250000, 30000, t + 500 * MS, &out);
    ok(out.switch_now, "hold: switch 1");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 500 * MS);
    eval(&c, 250000, 30000, t + 3500 * MS, &out);
    eval(&c, 250000, 30000, t + 3750 * MS, &out);
    ok(out.switch_now, "hold: switch 2");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 3750 * MS);
    eval(&c, 250000, 30000, t + 6750 * MS, &out);
    eval(&c, 250000, 30000, t + 7000 * MS, &out);
    ok(out.switch_now, "hold: switch 3");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 7000 * MS);

    eval(&c, 250000, 10000, t + 7250 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_HOLD, "hold: frozen");
    eval(&c, 250000, 10000, t + 37000 * MS, &out);
    ok(out.reason != TBT_FEC_ADAPT_SCLC_R_HOLD, "hold: expires after 30 s");
}

static void case_ctl_rate_cap(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = 100 * SEC;
    int i;

    init_steady(&c, 250000, 0, t);
    /* White box: ten applied switches inside the 60 s window. */
    for (i = 0; i < 10; i++)
        c.sw[i] = t - (u64)i * SEC;
    c.sw_n = 10;
    c.sw_head = 0;

    for (i = 1; i <= 8; i++)
        eval(&c, 0, 0, t + (u64)i * 250 * MS, &out);
    ok(!out.switch_now, "rate: ten switches in 60 s block the eleventh");

    c.sw[9] = t - 61 * SEC; /* one leaves the window */
    eval(&c, 0, 0, t + 9 * 250 * MS, &out);
    ok(out.switch_now, "rate: the eleventh switch after one leaves");
}

/* ------------------------------------------------------------------ *
 * Probe
 * ------------------------------------------------------------------ */

/* Build a controller at rate 50 whose first valid sample has r_fail above the
 * probe threshold, then lower r_fail so the 15 s stable window starts. */
static u64 probe_ready(struct tbt_fec_adapt_sclc_ctl *c, u64 t)
{
    struct tbt_fec_adapt_sclc_out out;

    init_ctl(c, true, 50, 256, 1);
    eval(c, 195000, 10000, t, &out);
    ok(!out.switch_now, "probe: fast start blocked");
    eval(c, 195000, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "probe: the stable window just started");
    return t + 250 * MS;
}

static void case_ctl_probe_commit(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    u64 low = probe_ready(&c, t);

    eval(&c, 195000, 0, low + 15 * SEC, &out);
    ok(out.switch_now, "probe: 15 s of low residual buys the step");
    eq_u32(out.to, 4, "probe: to rate 60");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PROBE_UP, "probe: reason");
    eq_u32(out.shape.n, 151, "probe: n");
    eq_u32(out.shape.m, 105, "probe: m");
    tbt_fec_adapt_sclc_ctl_applied(&c, low + 15 * SEC);
    ok(c.probe_active, "probe: active");

    eval(&c, 195000, 0, low + 24 * SEC, &out);
    ok(!out.switch_now, "probe: 9 s of observation is not a commit");
    eval(&c, 195000, 0, low + 25 * SEC + 250 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PROBE_COMMIT, "probe: commit");
    ok(!out.switch_now, "probe: commit does not switch");
    eq_u32(c.cur, 4, "probe: commit keeps rate 60");
    ok(!c.probe_active, "probe: inactive after commit");
}

static void case_ctl_probe_revert(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    u64 low = probe_ready(&c, t);
    u64 applied;

    eval(&c, 195000, 0, low + 15 * SEC, &out);
    ok(out.switch_now, "revert: probe up");
    tbt_fec_adapt_sclc_ctl_applied(&c, low + 15 * SEC);
    applied = low + 15 * SEC;

    /* The first second of the new epoch ignores old-epoch NACKs. */
    eval(&c, 195000, 20000, applied + 250 * MS, &out);
    ok(!out.switch_now, "revert: old-epoch sample ignored");
    eval(&c, 195000, 20000, applied + 1250 * MS, &out);
    ok(!out.switch_now, "revert: one valid high sample");
    eval(&c, 195000, 20000, applied + 1500 * MS, &out);
    ok(out.switch_now, "revert: two valid high samples");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PROBE_REVERT, "revert: reason");
    eq_u32(out.to, 3, "revert: back to rate 50");
    tbt_fec_adapt_sclc_ctl_applied(&c, applied + 1500 * MS);
    ok(!c.probe_active, "revert: probe inactive");

    /* The probed step is banned for 60 s; the 15 s stable window then
     * starts again from the first low sample. */
    eval(&c, 195000, 0, applied + 1500 * MS + 59 * SEC, &out);
    ok(!out.switch_now, "revert: 59 s later the ban holds");
    eval(&c, 195000, 0, applied + 1500 * MS + 60 * SEC, &out);
    ok(!out.switch_now, "revert: the ban just expired, stable window is new");
    eval(&c, 195000, 0, applied + 1500 * MS + 75 * SEC, &out);
    ok(out.switch_now, "revert: 15 s after the ban, probe again");
    eq_u32(out.to, 4, "revert: probe rate 60 again");
}

static void case_ctl_probe_abort(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    u64 low = probe_ready(&c, t);

    eval(&c, 195000, 0, low + 15 * SEC, &out);
    ok(out.switch_now, "abort: probe up");
    tbt_fec_adapt_sclc_ctl_applied(&c, low + 15 * SEC);

    eval(&c, 250000, 0, low + 16 * SEC, &out);
    ok(out.switch_now, "abort: p rose above the new boundary");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PROBE_ABORT, "abort: reason");
    eq_u32(out.to, 3, "abort: back to rate 50");
    tbt_fec_adapt_sclc_ctl_applied(&c, low + 16 * SEC);
    ok(!c.probe_active, "abort: probe inactive");
}

/* ------------------------------------------------------------------ *
 * Rollback, enable/disable, pause and clock
 * ------------------------------------------------------------------ */

static void case_ctl_rollback(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "rollback: fast start to rate 70");
    eq_u32(out.to, 5, "rollback: at rate 70");
    tbt_fec_adapt_sclc_ctl_applied(&c, t);

    {
        struct tbt_fec_adapt_sclc_est_out bad;

        snap(&bad, 0, 30000);
        tbt_fec_adapt_sclc_ctl_commit_failed(&c, &bad, t + SEC);
    }
    eval(&c, 0, 0, t + 1250 * MS, &out);
    ok(out.switch_now, "rollback: bad commit schedules the revert");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_ROLLBACK, "rollback: reason");
    eq_u32(out.to, 3, "rollback: back to rate 50");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 1250 * MS);
    eq_u32(c.cur, 3, "rollback: applied");

    /* A commit report outside the 10 s window is ignored. */
    eval(&c, 0, 0, t + 1250 * MS + 250 * MS, &out);
    ok(!out.switch_now, "rollback: steady after the revert");
    {
        struct tbt_fec_adapt_sclc_est_out bad;

        snap(&bad, 0, 30000);
        tbt_fec_adapt_sclc_ctl_commit_failed(&c, &bad, t + 20 * SEC);
    }
    eval(&c, 0, 0, t + 20 * SEC + 250 * MS, &out);
    ok(!out.switch_now, "rollback: late report ignored");

    /* A good snapshot never rolls back, and codec_fail alone does. */
    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "rollback: raise");
    tbt_fec_adapt_sclc_ctl_applied(&c, t);
    {
        struct tbt_fec_adapt_sclc_est_out good;

        snap(&good, 0, 10000);
        tbt_fec_adapt_sclc_ctl_commit_failed(&c, &good, t + SEC);
        eval(&c, 0, 0, t + 1250 * MS, &out);
        ok(!out.switch_now, "rollback: a good snapshot stays");
        good.codec_fail_ppm = 110000;
        tbt_fec_adapt_sclc_ctl_commit_failed(&c, &good, t + 1500 * MS);
        eval(&c, 0, 0, t + 1750 * MS, &out);
        ok(out.switch_now, "rollback: codec_fail above 10% rolls back");
        eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_ROLLBACK,
               "rollback: codec reason");
    }
}

static void case_ctl_default_off(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;

    init_ctl(&c, false, 50, 256, 1);
    eval(&c, 0, 0, SEC, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_DISABLED, "off: disabled");
    ok(!out.switch_now, "off: no switch");
    eq_u32(out.shape.n, 128, "off: the fixed shape n");
    eq_u32(out.shape.m, 128, "off: the fixed shape m");

    tbt_fec_adapt_sclc_ctl_set_enabled(&c, true, SEC);
    eval(&c, 0, 0, SEC + 250 * MS, &out);
    ok(out.switch_now, "off: enabling starts control");
    eq_u32(out.to, 5, "off: fast start to rate 70");

    /* Re-asserting the same state must not reset the run counters. */
    {
        struct tbt_fec_adapt_sclc_ctl r;
        struct tbt_fec_adapt_sclc_out d;

        init_ctl(&r, true, 50, 256, 1);
        eval(&r, 50000, 10000, SEC, &d);
        eq_u32(r.up_run, 1, "enable: first up sample");
        tbt_fec_adapt_sclc_ctl_set_enabled(&r, true, SEC + 250 * MS);
        eq_u32(r.up_run, 1, "enable: the same state keeps the run");
    }

    /* Disabling drops a suggestion that was already returned. */
    tbt_fec_adapt_sclc_ctl_set_enabled(&c, false, SEC + 250 * MS);
    ok(!c.busy, "off: disable clears the pending switch");
    tbt_fec_adapt_sclc_ctl_applied(&c, SEC + 500 * MS);
    eq_u32(c.cur, 3, "off: the rate did not move");
    eval(&c, 0, 0, SEC + 750 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_DISABLED, "off: disabled again");
}

static void case_ctl_invalid_init(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;

    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(&c, true, 35, 256, 1), (u32)-1,
           "init: rate 35 rejected");
    eval(&c, 0, 0, SEC, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_DISABLED, "init: not ready");

    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(&c, true, 50, 0, 1), (u32)-1,
           "init: zero shares rejected");
    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(&c, true, 50, 256, 2), (u32)-1,
           "init: too many carriers rejected");
    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(&c, true, 50, 2, 1), (u32)-1,
           "init: a geometry without a low rate rejected");
    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(&c, true, 50, 32, 8), 0,
           "init: uplink geometry accepted");
}

static void case_ctl_pause_wait_busy(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_est_out o;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 50, 256, 1);

    snap(&o, 400000, 0);
    o.pause_idle = true;
    tbt_fec_adapt_sclc_ctl_eval(&c, &o, t, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED, "pause: idle");
    ok(!out.switch_now, "pause: no switch");

    snap(&o, 400000, 0);
    o.p_valid = false;
    tbt_fec_adapt_sclc_ctl_eval(&c, &o, t + 250 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_WAIT, "wait: p invalid");

    eval(&c, 400000, 0, t + 500 * MS, &out);
    ok(out.switch_now, "wait: a valid sample resumes");
    eval(&c, 400000, 0, t + 750 * MS, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_BUSY, "busy: until applied");
    tbt_fec_adapt_sclc_ctl_abort(&c);
    ok(!c.busy, "busy: abort clears it");
    eq_u32(c.cur, 3, "busy: abort keeps the rate");

    /* A clock step backward does not replay a tick. */
    eval(&c, 400000, 0, t - 5 * SEC, &out);
    ok(!out.switch_now, "clock: backward eval does not switch");
    eval(&c, 400000, 0, t + 1000 * MS, &out);
    ok(!out.switch_now, "clock: one fresh sample");
    eval(&c, 400000, 0, t + 1250 * MS, &out);
    ok(out.switch_now, "clock: resumes at a fresh tick");
}

/* ------------------------------------------------------------------ *
 * Estimator -> controller convergence
 * ------------------------------------------------------------------ */

struct rig
{
    struct tbt_fec_adapt_sclc_est est;
    struct tbt_fec_adapt_sclc_ctl ctl;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u32 switches;
};

static void rig_init(struct rig *r, u16 shares, u16 groups, u8 rate_pct)
{
    memset(r, 0, sizeof(*r));
    alive_all(r->alive);
    tbt_fec_adapt_sclc_est_init(&r->est, shares);
    tbt_fec_adapt_sclc_est_carriers(&r->est, 256, 0, 256);
    init_ctl(&r->ctl, true, rate_pct, shares, groups);
}

/* One 250 ms tick: one super-block with `loss_ppm` of its DATA shares
 * missing, `r_ppm` residual failures, one estimator sample, one evaluation. */
static void rig_step(struct rig *r, u64 now_ns, u32 loss_ppm, u32 r_ppm)
{
    struct tbt_fec_adapt_sclc_est_out o;
    struct tbt_fec_adapt_sclc_out d;
    u16 shares = r->est.shares;
    u32 req = (u32)(((u64)r_ppm * 256 + 500000ull) / 1000000ull);

    if (shares == 32)
    {
        u16 g;
        u16 per = (u16)(((u64)loss_ppm * 32 + 500000ull) / 1000000ull);

        for (g = 0; g < 8; g++)
        {
            u8 mask[TBT_FEC_BITMAP_BYTES];

            mask_first(mask, per);
            tbt_fec_adapt_sclc_est_block(&r->est, now_ns, g, 32, mask,
                                         r->alive);
        }
    }
    else
    {
        u8 mask[TBT_FEC_BITMAP_BYTES];
        u16 miss = (u16)(((u64)loss_ppm * shares + 500000ull) / 1000000ull);

        mask_first(mask, miss);
        tbt_fec_adapt_sclc_est_block(&r->est, now_ns, 0, shares, mask,
                                     r->alive);
    }
    tbt_fec_adapt_sclc_est_super(&r->est, now_ns);
    tbt_fec_adapt_sclc_est_fail(&r->est, now_ns, req, 256);
    tbt_fec_adapt_sclc_est_sample(&r->est, now_ns, &o);
    tbt_fec_adapt_sclc_ctl_eval(&r->ctl, &o, now_ns, &d);
    if (d.switch_now)
    {
        tbt_fec_adapt_sclc_ctl_applied(&r->ctl, now_ns);
        r->switches++;
    }
}

static void case_convergence(void)
{
    static const struct
    {
        u32 loss_ppm;
        u8 idx;
    } cases[] = {
        {0, 5},       {10000, 5},  {30000, 5},  {50000, 5},  {100000, 5},
        {150000, 4},  {250000, 3}, {350000, 2}, {450000, 1}, {550000, 0},
        {1000000, 0},
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct rig r;
        u64 t = SEC;
        int step;

        rig_init(&r, 256, 1, 50);
        for (step = 0; step < 240; step++)
            rig_step(&r, t + (u64)step * 250 * MS, cases[i].loss_ppm, 0);
        eq_u32(r.ctl.cur, cases[i].idx, "converge: final rate index");
        ok(r.switches <= 1, "converge: at most the fast-start switch");
        eq_u32(r.switches, cases[i].idx == 3 ? 0u : 1u,
               "converge: switch count");
        {
            struct tbt_fec_adapt_sclc_shape s;

            eq_u32((u32)tbt_fec_adapt_sclc_shape(
                       tbt_fec_adapt_sclc_rate_at(r.ctl.cur)->pct, 256, 1,
                       &s),
                   0, "converge: shape builds");
            eq_u32(s.n + s.m, 256, "converge: shape spans the block");
        }
    }
}

static void case_convergence_uplink(void)
{
    static const struct
    {
        u32 loss_ppm;
        u8 idx;
        u16 n;
        u16 m;
    } cases[] = {
        {0, 5, 22, 10},   {100000, 5, 22, 10}, {150000, 4, 18, 14},
        {250000, 3, 16, 16}, {350000, 2, 12, 20}, {450000, 1, 9, 23},
        {550000, 0, 8, 24},
    };
    u32 i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct rig r;
        struct tbt_fec_adapt_sclc_shape s;
        u64 t = SEC;
        int step;

        rig_init(&r, 32, 8, 50);
        for (step = 0; step < 240; step++)
            rig_step(&r, t + (u64)step * 250 * MS, cases[i].loss_ppm, 0);
        eq_u32(r.ctl.cur, cases[i].idx, "uplink converge: rate index");
        eq_u32((u32)tbt_fec_adapt_sclc_shape(
                   tbt_fec_adapt_sclc_rate_at(r.ctl.cur)->pct, 32, 8, &s),
               0, "uplink converge: shape builds");
        eq_u32(s.n, cases[i].n, "uplink converge: n");
        eq_u32(s.m, cases[i].m, "uplink converge: m");
    }
}

static void case_no_oscillation(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    int i;

    /* Alternate inside the up band: six consecutive safe samples never form. */
    init_steady(&c, 250000, 10000, t);
    for (i = 0; i < 240; i++)
    {
        u32 p = (i & 1) ? 190000 : 186000;

        eval(&c, p, 10000, t + (u64)(i + 1) * 250 * MS, &out);
        ok(!out.switch_now, "osc: up band does not flap");
    }

    /* Alternate across the down boundary: the 2 pp guard is never cleared. */
    init_steady(&c, 250000, 0, t);
    for (i = 0; i < 240; i++)
    {
        u32 p = (i & 1) ? 301000 : 299000;

        eval(&c, p, 0, t + (u64)(i + 1) * 250 * MS, &out);
        ok(!out.switch_now, "osc: down boundary does not flap");
    }
}


static void case_ctl_rejected(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    /* A refused raise must not be retried; the next target is one below. */
    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "reject: fast start to rate 70");
    eq_u32(out.to, 5, "reject: at rate 70");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t);
    ok(!c.busy, "reject: clears the suggestion");
    eval(&c, 0, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "reject: raises banned");
    eval(&c, 0, 0, t + 30 * SEC, &out);
    ok(!out.switch_now, "reject: one sample after the ban");
    {
        int i;

        for (i = 1; i <= 5; i++)
            eval(&c, 0, 0, t + 30 * SEC + (u64)i * 250 * MS, &out);
        ok(out.switch_now, "reject: retries the lower rate after the ban");
        eq_u32(out.to, 4, "reject: rate 60, not 70");
    }

    /* A refused down switch keeps the current rate: a lower shape would
     * only earn another NACK from a peer that refused this one. */
    init_ctl(&c, true, 70, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(!out.switch_now, "reject: start at rate 70");
    eval(&c, 400000, 0, t + 250 * MS, &out);
    eval(&c, 400000, 0, t + 500 * MS, &out);
    ok(out.switch_now, "reject: down to rate 40");
    eq_u32(out.to, 2, "reject: at rate 40");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t + 500 * MS);
    eval(&c, 400000, 0, t + 750 * MS, &out);
    ok(!out.switch_now, "reject: quiet after the refused down");
    eval(&c, 400000, 0, t + 31 * SEC, &out);
    ok(!out.switch_now, "reject: the refused down is not retried");
    eq_u32(c.cur, 5, "reject: stays at rate 70");

    /* The lowest rate has no lower target: the ban keeps the current rate. */
    init_ctl(&c, true, 30, 256, 1);
    eval(&c, 550000, 0, t, &out);
    ok(out.switch_now, "reject: fast down to rate 25");
    eq_u32(out.to, 0, "reject: at rate 25");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t);
    eval(&c, 550000, 0, t + 250 * MS, &out);
    ok(!out.switch_now, "reject: lowest rate is not retried");
    eval(&c, 550000, 0, t + 29 * SEC, &out);
    ok(!out.switch_now, "reject: lowest rate stays banned");
}

/* A legacy 0.4.0 peer supports only 128/128 (rate 50%). The controller may
 * propose 178/78, its 151/105 fallback, and later a 102/154 down step; the
 * peer NACKs every CFG. Each refusal must return to 128/128, and after the
 * last one the controller must stay there without generating any further
 * CFG for the rest of the session. */
static void case_ctl_legacy_nack(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    u32 refused = 0;
    int i;

    init_ctl(&c, true, 50, 256, 1);

    /* Low loss (2%) proposes the top rung 178/78. The legacy peer refuses. */
    eval(&c, 20000, 0, t, &out);
    ok(out.switch_now, "legacy: low loss proposes a raise");
    eq_u32(out.to, 5, "legacy: 70% proposal");
    eq_u32(out.shape.n, 178, "legacy: 178/78 n");
    eq_u32(out.shape.m, 78, "legacy: 178/78 m");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t);
    refused++;
    ok(!c.busy, "legacy: NACK clears the suggestion");
    eq_u32(c.cur, 3, "legacy: 128/128 still in force");

    /* The 30 s raise ban holds; the single fallback is 151/105 at 60%. */
    for (i = 1; i <= 119; i++)
    {
        eval(&c, 20000, 0, t + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "legacy: no CFG during the raise ban");
    }
    for (i = 1; i <= 12; i++)
    {
        eval(&c, 20000, 0, t + 30 * SEC + (u64)i * 250 * MS, &out);
        if (out.switch_now)
            break;
    }
    ok(out.switch_now, "legacy: one lower fallback is tried");
    eq_u32(out.to, 4, "legacy: 60% fallback");
    eq_u32(out.shape.n, 151, "legacy: 151/105 n");
    eq_u32(out.shape.m, 105, "legacy: 151/105 m");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t + 33 * SEC);
    refused++;
    eq_u32(c.cur, 3, "legacy: the fallback NACK also keeps 128/128");

    /* Both raises are remembered: 180 s of low loss must produce no CFG. */
    for (i = 1; i <= 720; i++)
    {
        eval(&c, 20000, 0, t + 33 * SEC + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "legacy: no retry of a refused raise");
        eq_u32(out.shape.n, 128, "legacy: out shape stays 128");
        eq_u32(out.shape.m, 128, "legacy: out shape stays 128");
    }
    eq_u32(c.cur, 3, "legacy: steady on 128/128 after the raises");

    /* Loss rises to 35%: the law wants 102/154 at 40%, and the peer refuses
     * that too. The controller must keep 128/128 and stop walking down. */
    for (i = 1; i <= 4; i++)
    {
        eval(&c, 350000, 0, t + 213 * SEC + (u64)i * 250 * MS, &out);
        if (out.switch_now)
            break;
    }
    ok(out.switch_now, "legacy: 35% loss proposes a down step");
    eq_u32(out.to, 2, "legacy: 40% down proposal");
    eq_u32(out.shape.n, 102, "legacy: 102/154 n");
    eq_u32(out.shape.m, 154, "legacy: 102/154 m");
    tbt_fec_adapt_sclc_ctl_rejected(&c, t + 214 * SEC);
    refused++;

    for (i = 1; i <= 720; i++)
    {
        eval(&c, 350000, 0, t + 214 * SEC + (u64)i * 250 * MS, &out);
        ok(!out.switch_now, "legacy: no retry of a refused down");
        eq_u32(out.shape.n, 128, "legacy: down refusal keeps 128/128");
        eq_u32(out.shape.m, 128, "legacy: down refusal keeps 128/128");
    }
    eq_u32(c.cur, 3, "legacy: steady on 128/128 after the down NACK");
    eq_u32(refused, 3, "legacy: three NACKs, then silence");
    ok(c.ready && c.enabled, "legacy: the controller stays live");
    eq_u32(c.shares, 256, "legacy: the block still spans 256 shares");
}

static void case_ctl_r_ignore(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "r ignore: fast start");
    tbt_fec_adapt_sclc_ctl_applied(&c, t);

    /* Old-epoch NACKs inside the first second must not fail-down. */
    eval(&c, 0, 30000, t + 250 * MS, &out);
    ok(!out.switch_now, "r ignore: old-epoch sample");
    eq_u32(c.fail_run, 0, "r ignore: the run counter did not move");
    eval(&c, 0, 30000, t + 750 * MS, &out);
    ok(!out.switch_now, "r ignore: still inside the first second");
    eq_u32(c.fail_run, 0, "r ignore: still no run");
    eval(&c, 0, 30000, t + 1000 * MS, &out);
    ok(!out.switch_now, "r ignore: first valid sample");
    eq_u32(c.fail_run, 1, "r ignore: the first valid sample counts");
    eval(&c, 0, 30000, t + 1250 * MS, &out);
    ok(!out.switch_now, "r ignore: min gap still holds");
    eq_u32(c.fail_run, 2, "r ignore: two valid samples arm fail-down");
    eval(&c, 0, 30000, t + 5000 * MS, &out);
    ok(out.switch_now, "r ignore: fail-down after the reverse cooldown");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN, "r ignore: reason");
}

static void case_ctl_no_raise_probe(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    /* Fail-down at rate 60 sets the 30 s raise ban. */
    init_ctl(&c, true, 60, 256, 1);
    eval(&c, 150000, 10000, t, &out);
    eval(&c, 150000, 30000, t + 250 * MS, &out);
    eval(&c, 150000, 30000, t + 500 * MS, &out);
    ok(out.switch_now, "no-raise probe: fail down");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 500 * MS);
    eq_u32(c.cur, 3, "no-raise probe: at rate 50");

    /* p sits inside the up band and the residual is perfect, but the ban
     * still holds, so no probe may fire. */
    eval(&c, 195000, 0, t + 1750 * MS, &out);
    ok(!out.switch_now, "no-raise probe: stable window starts, ban holds");
    eval(&c, 195000, 0, t + 16 * SEC, &out);
    ok(!out.switch_now, "no-raise probe: 15.5 s is still inside the ban");
    eval(&c, 195000, 0, t + 29 * SEC, &out);
    ok(!out.switch_now, "no-raise probe: 28.5 s is still inside the ban");
    eval(&c, 195000, 0, t + 30250 * MS, &out);
    ok(!out.switch_now, "no-raise probe: 29.75 s is still inside the ban");
    eval(&c, 195000, 0, t + 30750 * MS, &out);
    ok(out.switch_now, "no-raise probe: the ban expires");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PROBE_UP,
           "no-raise probe: probe after the ban");
}

static void case_est_carrier_limit(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;

    /* A 32-carrier link: group 1 does not exist. Its carriers 32..63 must
     * not enter the sample even when the alive bitmap has those bits set. */
    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 32);
    tbt_fec_adapt_sclc_est_carriers(&e, 32, 0, 32);
    mask_first(mask, 32);
    tbt_fec_adapt_sclc_est_block(&e, t, 1, 32, mask, alive);
    tbt_fec_adapt_sclc_est_super(&e, t);
    tbt_fec_adapt_sclc_est_sample(&e, t, &o);
    eq_u32(o.sent_win, 0, "carrier limit: nonexistent group ignored");
}

static void case_profile(void)
{
    struct tbt_fec_adapt_sclc_profile p;
    static const u8 pct[TBT_FEC_ADAPT_SCLC_LEVELS] = {25, 30, 40, 50, 60, 70};
    u8 i;

    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        eq_u32((u32)tbt_fec_adapt_sclc_profile_of(pct[i], &p), 0,
               "profile: ladder point accepted");
        if (pct[i] == 40)
        {
            eq_u32(p.block, 512, "profile: rate 40 uses block 512");
            eq_u32(p.L, 16, "profile: rate 40 uses L 16");
            eq_u32(p.payload, 4096, "profile: rate 40 uses 4096 bits");
        }
        else
        {
            eq_u32(p.block, 256, "profile: other rates use block 256");
            eq_u32(p.L, 16, "profile: other rates use L 16");
            eq_u32(p.payload, 2048, "profile: other rates use 2048 bits");
        }
    }
    eq_u32((u32)tbt_fec_adapt_sclc_profile_of(35, &p), (u32)-1,
           "profile: 35 absent");
    eq_u32((u32)tbt_fec_adapt_sclc_profile_of(50, NULL), (u32)-1,
           "profile: NULL rejected");
}


static void case_est_p_valid_boundary(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u64 t = SEC;
    u16 i;

    alive_all(alive);
    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_carriers(&e, 256, 0, 256);
    for (i = 0; i < 7; i++)
    {
        feed_down(&e, t + (u64)i * 250 * MS, 256, 0, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 1750 * MS, &o);
    eq_u32(o.supers_win, 7, "p_valid: seven supers");
    eq_u32(o.sent_win, 1792, "p_valid: 1792 shares");
    ok(!o.p_valid, "p_valid: seven supers are not enough");

    feed_down(&e, t + 1750 * MS, 256, 0, alive);
    tbt_fec_adapt_sclc_est_super(&e, t + 1750 * MS);
    tbt_fec_adapt_sclc_est_sample(&e, t + 1750 * MS, &o);
    ok(o.p_valid, "p_valid: eight supers and 2048 shares");

    /* The 512-share floor is exactly met by eight 64-share supers. */
    tbt_fec_adapt_sclc_est_init(&e, 64);
    tbt_fec_adapt_sclc_est_carriers(&e, 64, 0, 64);
    for (i = 0; i < 8; i++)
    {
        feed_down(&e, t + (u64)i * 250 * MS, 64, 0, alive);
        tbt_fec_adapt_sclc_est_super(&e, t + (u64)i * 250 * MS);
    }
    tbt_fec_adapt_sclc_est_sample(&e, t + 1750 * MS, &o);
    eq_u32(o.sent_win, 512, "p_valid: 512 shares");
    ok(o.p_valid, "p_valid: eight supers and 512 shares");
}

static void case_est_unknown(void)
{
    struct tbt_fec_adapt_sclc_est e;
    struct tbt_fec_adapt_sclc_est_out o;
    u64 t = SEC;

    tbt_fec_adapt_sclc_est_init(&e, 256);
    tbt_fec_adapt_sclc_est_unknown(&e, t);
    tbt_fec_adapt_sclc_est_unknown(&e, t + 250 * MS);
    tbt_fec_adapt_sclc_est_sample(&e, t + 250 * MS, &o);
    eq_u32(o.unknown_win, 2, "unknown: window count");
    eq_u32((u32)o.unknown_total, 2, "unknown: total");
    ok(!o.p_valid, "unknown: no p sample");
}

static void case_ctl_fail_down_zero(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 25, 256, 1);
    eval(&c, 550000, 10000, t, &out);
    ok(!out.switch_now, "fail down zero: start at rate 25");
    eval(&c, 550000, 30000, t + 250 * MS, &out);
    eval(&c, 550000, 30000, t + 500 * MS, &out);
    ok(!out.switch_now, "fail down zero: no lower rate");
    ok(c.no_raise_until > t + 500 * MS, "fail down zero: raises still banned");
}

static void case_ctl_probe_gate(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    /* At rate 25 the probe band for rate 30 is p in (48%, 50%]. The probe
     * also requires p <= 30%, so a 49% sample must not probe. */
    init_ctl(&c, true, 25, 256, 1);
    eval(&c, 490000, 10000, t, &out);
    ok(!out.switch_now, "probe gate: fast start blocked");
    eval(&c, 490000, 0, t + 250 * MS, &out);
    eval(&c, 490000, 0, t + 16 * SEC, &out);
    ok(!out.switch_now, "probe gate: p above 30% never probes");
}

static void case_ctl_reverse(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;
    int i;

    /* Up to rate 70, then a down request: the 5 s reverse cooldown, not the
     * 3 s down dwell, holds the down switch. */
    init_ctl(&c, true, 60, 256, 1);
    eval(&c, 0, 10000, t, &out);
    ok(!out.switch_now, "reverse: start at rate 60");
    for (i = 1; i <= 5; i++)
        eval(&c, 50000, 10000, t + (u64)i * 250 * MS, &out);
    ok(out.switch_now, "reverse: six samples move up");
    eq_u32(out.to, 5, "reverse: at rate 70");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 1250 * MS);

    eval(&c, 450000, 0, t + 1500 * MS, &out);
    eval(&c, 450000, 0, t + 1750 * MS, &out);
    ok(!out.switch_now, "reverse: min gap and dwell");
    eval(&c, 450000, 0, t + 4250 * MS, &out);
    ok(!out.switch_now, "reverse: 3 s dwell is not enough");
    eval(&c, 450000, 0, t + 6250 * MS, &out);
    ok(out.switch_now, "reverse: 5 s cooldown clears");
    eq_u32(out.to, 1, "reverse: multi-step down to rate 30");
}

static void case_ctl_epoch_shape(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    init_ctl(&c, true, 50, 256, 1);
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "epoch: first switch");
    eq_u32(out.epoch, 1, "epoch: first");
    eq_u32(out.shape.n, 178, "epoch: first n");
    eq_u32(out.shape.m, 78, "epoch: first m");
    tbt_fec_adapt_sclc_ctl_applied(&c, t);

    /* The up switch's 5 s reverse cooldown delays the down move. */
    eval(&c, 450000, 0, t + 500 * MS, &out);
    ok(!out.switch_now, "epoch: down waits for the cooldown");
    eval(&c, 450000, 0, t + 5 * SEC, &out);
    ok(out.switch_now, "epoch: second switch");
    eq_u32(out.epoch, 2, "epoch: monotonic");
    eq_u32(out.shape.n, 76, "epoch: second n");
    eq_u32(out.shape.m, 180, "epoch: second m");
    tbt_fec_adapt_sclc_ctl_applied(&c, t + 5 * SEC);
    eq_u32(c.cur, 1, "epoch: at rate 30");
}


static void case_rate_index_for_shape(void)
{
    static const struct
    {
        u16 n, m;
        u8 idx;
    } down[] = {{128, 128, 3}, {64, 192, 0}, {76, 180, 1},
                {102, 154, 2}, {151, 105, 4}, {178, 78, 5}};
    static const struct
    {
        u16 n, m;
        u8 idx;
    } up[] = {{8, 24, 0}, {9, 23, 1}, {12, 20, 2},
              {16, 16, 3}, {18, 14, 4}, {22, 10, 5}};
    u32 i;

    for (i = 0; i < sizeof(down) / sizeof(down[0]); i++)
        eq_u32(tbt_fec_adapt_sclc_rate_index_for_shape(down[i].n,
                                                       down[i].m),
               down[i].idx, "shape index: downlink");
    for (i = 0; i < sizeof(up) / sizeof(up[0]); i++)
        eq_u32(tbt_fec_adapt_sclc_rate_index_for_shape(up[i].n, up[i].m),
               up[i].idx, "shape index: uplink");
    eq_u32(tbt_fec_adapt_sclc_rate_index_for_shape(14, 18), 0xff,
           "shape index: 14/18 is not on the ladder");
    eq_u32(tbt_fec_adapt_sclc_rate_index_for_shape(10, 22), 0xff,
           "shape index: 10/22 is not on the ladder");
    eq_u32(tbt_fec_adapt_sclc_rate_index_for_shape(0, 0), 0xff,
           "shape index: empty shape");
}

/* The data plane allocates the SC controller only when this plan resolves.
 * These assertions pin the index/mask it will use for the two shipped
 * profiles and the disabled start when adaptive=off. */
static void case_init_plan(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u8 idx, mask;

    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(128, 128, 1,
                                             TBT_FEC_SCLC_DOWN, &idx, &mask),
           0, "plan: downlink");
    eq_u32(idx, 3, "plan: downlink index");
    eq_u32(mask, (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u,
           "plan: downlink supports the ladder");
    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(151, 105, 1,
                                             TBT_FEC_SCLC_DOWN, &idx, &mask),
           0, "plan: downlink rate 60");
    eq_u32(idx, 4, "plan: downlink rate 60 index");

    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(8, 24, 8, TBT_FEC_SCLC_UP,
                                             &idx, &mask),
           0, "plan: uplink");
    eq_u32(idx, 0, "plan: uplink index");
    eq_u32(mask, (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u,
           "plan: uplink mask");

    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(16, 16, 8, TBT_FEC_SCLC_UP,
                                             &idx, &mask),
           0, "plan: uplink rate 50");
    eq_u32(idx, 3, "plan: uplink rate 50 index");

    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(14, 18, 8, TBT_FEC_SCLC_UP,
                                             &idx, &mask),
           (u32)-1, "plan: off-ladder refused");
    eq_u32(idx, 0xff, "plan: off-ladder index");
    eq_u32(mask, 0, "plan: off-ladder mask");
    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(128, 128, 2,
                                             TBT_FEC_SCLC_DOWN, &idx, &mask),
           (u32)-1, "plan: downlink needs one group");
    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(8, 24, 1, TBT_FEC_SCLC_UP,
                                             &idx, &mask),
           (u32)-1, "plan: uplink needs more than one group");

    /* The resolved plan starts the controller at the configured point. */
    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(
               &c, true, tbt_fec_adapt_sclc_rate_at(3)->pct, 256, 1),
           0, "plan: downlink controller init");
    tbt_fec_adapt_sclc_ctl_set_rate_mask(&c, 1u << 3);
    eq_u32(c.rate_mask, 1u << 3, "plan: downlink controller mask");
    ok(c.ready && c.enabled, "plan: downlink controller ready");

    eq_u32((u32)tbt_fec_adapt_sclc_ctl_init(
               &c, false, tbt_fec_adapt_sclc_rate_at(0)->pct, 32, 8),
           0, "plan: disabled controller init");
    ok(c.ready && !c.enabled, "plan: adaptive=off starts disabled");
    eval(&c, 0, 0, SEC, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_DISABLED,
           "plan: disabled controller does not switch");
}

static void case_rate_mask(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 t = SEC;

    /* A downlink codec that builds only rate 50 never leaves it. */
    init_ctl(&c, true, 50, 256, 1);
    tbt_fec_adapt_sclc_ctl_set_rate_mask(&c, 1u << 3);
    eval(&c, 0, 0, t, &out);
    ok(!out.switch_now, "mask: rate 70 is not supported");
    eq_u32(out.to, 3, "mask: stays at rate 50");
    eval(&c, 600000, 0, t + 250 * MS, &out);
    eval(&c, 600000, 0, t + 500 * MS, &out);
    ok(!out.switch_now, "mask: rate 25 is not supported either");
    eq_u32(out.to, 3, "mask: still rate 50");

    /* Supported 50 and 70 only: fast start to 70, then back to 50. */
    init_ctl(&c, true, 50, 256, 1);
    tbt_fec_adapt_sclc_ctl_set_rate_mask(&c, (1u << 3) | (1u << 5));
    eval(&c, 0, 0, t, &out);
    ok(out.switch_now, "mask: fast start to a supported rate");
    eq_u32(out.to, 5, "mask: to rate 70");
    tbt_fec_adapt_sclc_ctl_applied(&c, t);
    eval(&c, 600000, 0, t + 5 * SEC, &out);
    ok(!out.switch_now, "mask: one down sample");
    eval(&c, 600000, 0, t + 5250 * MS, &out);
    ok(out.switch_now, "mask: down to the next supported rate");
    eq_u32(out.to, 3, "mask: to rate 50");

    /* A mask without the current rate is ignored. */
    init_ctl(&c, true, 50, 256, 1);
    tbt_fec_adapt_sclc_ctl_set_rate_mask(&c, 1u << 5);
    eq_u32(c.rate_mask, (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u,
           "mask: unsupported current rate is ignored");

    eq_u32(tbt_fec_adapt_sclc_max_side(256, 1u << 3), 128,
           "max side: downlink rate 50");
    eq_u32(tbt_fec_adapt_sclc_max_side(256,
                                       (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u),
           192, "max side: downlink full ladder");
    eq_u32(tbt_fec_adapt_sclc_max_side(32, 0), 24,
           "max side: uplink all rates");
    eq_u32(tbt_fec_adapt_sclc_max_side(32, 1u << 5), 22,
           "max side: uplink rate 70");
}

static void make_sclc_cfg(struct tbt_fec_cfg *cfg, u16 n, u16 m, u16 groups,
                          u8 L, u16 block, u32 rate_ppm)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->n = n;
    cfg->m = m;
    cfg->groups = groups;
    cfg->codec_variant = 2;
    cfg->L = L;
    cfg->block = block;
    cfg->rate_ppm = rate_ppm;
}

static void case_wire_profile(void)
{
    struct tbt_fec_adapt_sclc_wire w;
    struct tbt_fec_cfg cfg, back;
    u16 carriers = 256;

    /* Downlink 1x256/L8/r0.5. */
    make_sclc_cfg(&cfg, 128, 128, 1, 8, 256, 500000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: downlink accepted");
    eq_u32(w.kind, TBT_FEC_SCLC_DOWN, "wire: downlink kind");
    eq_u32(w.symbol, 0, "wire: downlink binary");
    eq_u32(w.block, 256, "wire: downlink block");
    eq_u32(w.L, 8, "wire: downlink L");
    eq_u32(w.rate_pct, 50, "wire: downlink rate");
    eq_u32(w.n, 128, "wire: downlink n");
    eq_u32(w.m, 128, "wire: downlink m");
    eq_u32(w.groups, 1, "wire: downlink groups");

    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 7,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "wire: downlink to cfg");
    eq_u32(back.w, 256, "wire: downlink cfg w");
    eq_u32(back.k, 500, "wire: downlink cfg k");
    eq_u32(back.d, 0, "wire: downlink cfg d");
    eq_u32(back.codec_variant, 2, "wire: downlink cfg variant");
    eq_u32(back.L, 8, "wire: downlink cfg L");
    eq_u32(back.block, 256, "wire: downlink cfg block");
    eq_u32(back.rate_ppm, 500000, "wire: downlink cfg rate");

    /* Uplink 8x32/L16/r1/4. */
    make_sclc_cfg(&cfg, 8, 24, 8, 16, 128, 250000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: uplink accepted");
    eq_u32(w.kind, TBT_FEC_SCLC_UP, "wire: uplink kind");
    eq_u32(w.symbol, 1, "wire: uplink gf16");
    eq_u32(w.block, 128, "wire: uplink block");
    eq_u32(w.L, 16, "wire: uplink L");
    eq_u32(w.rate_pct, 25, "wire: uplink rate");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 8,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "wire: uplink to cfg");
    eq_u32(back.w, 16, "wire: uplink cfg w");
    eq_u32(back.k, 128, "wire: uplink cfg k");
    eq_u32(back.d, 64, "wire: uplink cfg d");
    eq_u32(back.codec_variant, 2, "wire: uplink cfg variant");
    eq_u32(back.L, 16, "wire: uplink cfg L");
    eq_u32(back.block, 128, "wire: uplink cfg block");
    eq_u32(back.rate_ppm, 250000, "wire: uplink cfg rate");

    /* The 60% ladder point is 18/14 of 32, d=144. */
    make_sclc_cfg(&cfg, 18, 14, 8, 16, 128, 592593);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: uplink 18/14 accepted");
    eq_u32(w.rate_pct, 60, "wire: uplink 18/14 is rate 60");

    /* Every uplink ladder point round-trips through the CFG fields. */
    {
        static const u8 pct[TBT_FEC_ADAPT_SCLC_LEVELS] = {25, 30, 40, 50,
                                                          60, 70};
        u8 i;

        for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
        {
            struct tbt_fec_adapt_sclc_shape s;

            eq_u32((u32)tbt_fec_adapt_sclc_shape(pct[i], 32, 8, &s), 0,
                   "wire: uplink ladder shape");
            make_sclc_cfg(&cfg, s.n, s.m, 8, 16, 128,
                          tbt_fec_adapt_sclc_rate_ppm(pct[i]));
            eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w),
                   0, "wire: uplink ladder resolves");
            eq_u32(w.rate_pct, pct[i], "wire: uplink ladder rate");
            eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(
                       &w, 2, TBT_FEC_CFG_F_APPLY, &back),
                   0, "wire: uplink ladder to cfg");
            eq_u32(back.n, s.n, "wire: uplink ladder n");
            eq_u32(back.m, s.m, "wire: uplink ladder m");
            eq_u32(back.d, (u32)s.n * 256u / 32u,
                   "wire: uplink ladder legacy d mirror");
            eq_u32(back.codec_variant, 2, "wire: uplink ladder variant");
            eq_u32(back.L, 16, "wire: uplink ladder L");
            eq_u32(back.block, 128, "wire: uplink ladder block");
        }
    }

    /* Rejections: every hard profile boundary. */
    make_sclc_cfg(&cfg, 127, 129, 1, 8, 256, 500000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: off-ladder downlink split rejected");
    cfg.n = 151;
    cfg.m = 105;
    cfg.w = 256;
    cfg.k = 589;
    cfg.d = 0;
    cfg.codec_variant = 2;
    cfg.block = 256;
    cfg.L = 8;
    cfg.rate_ppm = 592593;
    cfg.groups = 1;
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: rate 60 downlink accepted");
    eq_u32(w.rate_pct, 60, "wire: rate 60 label");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 9,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "wire: rate 60 downlink to cfg");
    eq_u32(back.n, 151, "wire: rate 60 cfg n");
    eq_u32(back.m, 105, "wire: rate 60 cfg m");
    eq_u32(back.k, 589, "wire: rate 60 cfg k");
    cfg.rate_ppm = 600000;
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: downlink rate must agree with n/m");
    cfg.n = 128;
    cfg.m = 128;
    cfg.w = 512;
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: downlink block 512 rejected");
    make_sclc_cfg(&cfg, 128, 128, 2, 8, 256, 500000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: downlink groups must be 1");
    make_sclc_cfg(&cfg, 8, 24, 8, 16, 256, 250000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: uplink block 256 rejected");
    make_sclc_cfg(&cfg, 8, 24, 8, 8, 128, 250000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: uplink L 8 rejected");
    make_sclc_cfg(&cfg, 14, 18, 8, 16, 128, 592593);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: 14/18 is not on the ladder");
    make_sclc_cfg(&cfg, 8, 24, 8, 16, 128, 500000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: uplink rate must agree with n/m");
    make_sclc_cfg(&cfg, 8, 24, 8, 16, 128, 250000);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 255, &w), (u32)-1,
           "wire: carriers must equal groups*(n+m)");

    /* Compatibility: a rate change inside the same codec is allowed. */
    {
        struct tbt_fec_cfg a, b;

        make_sclc_cfg(&a, 8, 24, 8, 16, 128, 250000);
        b = a;
        b.n = 16;
        b.m = 16;
        b.rate_ppm = 500000;
        ok(tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "wire: uplink rate change is compatible");
        b.L = 8;
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "wire: an L change is incompatible");
        b = a;
        b.block = 256;
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "wire: a block change is incompatible");

        /* A direction change (uplink vs downlink) is never compatible. */
        make_sclc_cfg(&b, 128, 128, 1, 8, 256, 500000);
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "wire: a direction change is incompatible");
    }
}

static void case_fbk_overlay(void)
{
    struct tbt_fec_fbk f;

    memset(&f, 0, sizeof(f));
    f.p_fast_ppm = 400000;
    f.codec_variant = 2;
    f.L = 8;
    f.block = 256;
    f.rate_ppm_current = 500000;
    f.rate_ppm_target = 400000;
    f.carrier_bitmap[0] |= 0x01;
    f.carrier_bitmap[31] |= 0x80;
    eq_u32(f.p_fast_ppm, 400000, "fbk: p_fast field");
    eq_u32((u32)offsetof(struct tbt_fec_fbk, p_fast_ppm),
           TBT_FEC_FBK_P_FAST_PPM, "fbk: p_fast offset");
    eq_u32((u32)offsetof(struct tbt_fec_fbk, codec_variant),
           TBT_FEC_FBK_CODEC_VARIANT, "fbk: variant offset");
    eq_u32((u32)offsetof(struct tbt_fec_fbk, block), TBT_FEC_FBK_BLOCK,
           "fbk: block offset");
    eq_u32((u32)sizeof(f.carrier_bitmap), TBT_FEC_BITMAP_BYTES,
           "fbk: full bitmap size");
    ok((f.carrier_bitmap[0] & 0x01) && (f.carrier_bitmap[31] & 0x80) &&
           f.carrier_bitmap[28] == 0 && f.carrier_bitmap[29] == 0 &&
           f.carrier_bitmap[30] == 0 && f.carrier_bitmap[31] == 0x80,
       "fbk: bitmap tail is not overlaid");
}

/* The 0.4.0 pair runs adaptive=on with an asymmetric INIT: the near TX is
 * the fixed binary downlink (1x256/L8/r0.50), the far TX is the GF(16)
 * uplink (8x32/L16). tbt_fec_rx_apply_init() runs this resolution whenever
 * adaptive is on, before the first share can be placed; if either direction
 * failed it, no block would complete. Pin both admissions, the downlink
 * ladder plan, and the fact that the two directions are an INIT, not a rate
 * switch. */
static void case_asymmetric_init(void)
{
    struct tbt_fec_adapt_sclc_wire down, up;
    struct tbt_fec_cfg d, u;
    u8 idx, mask;

    memset(&d, 0, sizeof(d));
    d.n = 128;
    d.m = 128;
    d.groups = 1;
    d.codec_variant = 2;
    d.L = 8;
    d.block = 256;
    d.rate_ppm = 500000;
    memset(&u, 0, sizeof(u));
    u.n = 8;
    u.m = 24;
    u.groups = 8;
    u.codec_variant = 2;
    u.L = 16;
    u.block = 128;
    u.rate_ppm = 250000;

    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&d, 256, &down), 0,
           "init: adaptive=on admits the downlink INIT");
    eq_u32(down.kind, TBT_FEC_SCLC_DOWN, "init: near RX installs binary");
    eq_u32(down.n, 128, "init: downlink n");
    eq_u32(down.m, 128, "init: downlink m");
    eq_u32(down.groups, 1, "init: downlink one group");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&u, 256, &up), 0,
           "init: adaptive=on admits the uplink INIT");
    eq_u32(up.kind, TBT_FEC_SCLC_UP, "init: far RX installs gf16");
    eq_u32(up.n, 8, "init: uplink n");
    eq_u32(up.m, 24, "init: uplink m");
    eq_u32(up.groups, 8, "init: uplink eight groups");
    eq_u32(up.rate_pct, 25, "init: uplink starts at rate 25");
    ok(!tbt_fec_adapt_sclc_wire_compatible(&d, &u, 256),
       "init: the two directions are not a rate switch");

    /* The 0.4.3 downlink opens the six-point ladder; the 0.4.0 peer's fixed
     * 128/128 INIT is still admitted above. */
    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(128, 128, 1,
                                             TBT_FEC_SCLC_DOWN, &idx, &mask),
           0, "init: downlink plan");
    eq_u32(idx, 3, "init: downlink starts at rate 50");
    eq_u32(mask, (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u,
           "init: downlink supports the six-rung ladder");
    eq_u32((u32)tbt_fec_adapt_sclc_init_plan(8, 24, 8, TBT_FEC_SCLC_UP,
                                             &idx, &mask),
           0, "init: uplink plan");
    eq_u32(idx, 0, "init: uplink starts at the lowest rung");
    eq_u32(mask, (1u << TBT_FEC_ADAPT_SCLC_LEVELS) - 1u,
           "init: uplink supports the six-rung ladder");
}

static void case_epoch_base(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;

    init_ctl(&c, true, 50, 256, 1);
    tbt_fec_adapt_sclc_ctl_set_epoch(&c, 1);
    eval(&c, 0, 0, SEC, &out);
    ok(out.switch_now, "epoch base: first switch");
    eq_u32(out.epoch, 2, "epoch base: after INIT generation 1");
}

/* The adaptive bitmap helpers must refuse an index outside the 256-carrier
 * map. An unchecked write here would land in the bool fields next to the
 * block's masks and produce an invalid-load report. */
static void case_bitmap_bounds(void)
{
    u8 bm[TBT_FEC_ADAPT_BITMAP_BYTES];
    u8 copy[TBT_FEC_ADAPT_BITMAP_BYTES];

    memset(bm, 0, sizeof(bm));
    tbt_fec_adapt_bitmap_set(bm, 255);
    ok(tbt_fec_adapt_bitmap_test(bm, 255), "bitmap: index 255 is valid");
    memcpy(copy, bm, sizeof(bm));
    tbt_fec_adapt_bitmap_set(bm, 256);
    tbt_fec_adapt_bitmap_clear(bm, 256);
    ok(!memcmp(bm, copy, sizeof(bm)),
       "bitmap: index 256 is a no-op for set and clear");
    ok(!tbt_fec_adapt_bitmap_test(bm, 256),
       "bitmap: index 256 tests false");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(void)
{
    case_rate_ladder();
    case_shape_downlink();
    case_shape_uplink();
    case_shape_reject();
    case_est_window();
    case_est_ewma();
    case_est_late_credit();
    case_late_ledger();
    case_est_ewma_ring();
    case_est_alive();
    case_est_groups();
    case_est_pauses();
    case_est_fail();
    case_est_codec_fail();
    case_est_burst();
    case_est_clock();
    case_ctl_targets();
    case_ctl_hysteresis_up();
    case_ctl_down_hysteresis();
    case_ctl_up_dwell();
    case_ctl_down_fast();
    case_ctl_fail_down();
    case_ctl_burst();
    case_ctl_hold();
    case_ctl_rate_cap();
    case_ctl_probe_commit();
    case_ctl_probe_revert();
    case_ctl_probe_abort();
    case_ctl_rollback();
    case_ctl_default_off();
    case_ctl_invalid_init();
    case_ctl_pause_wait_busy();
    case_ctl_rejected();
    case_ctl_legacy_nack();
    case_ctl_r_ignore();
    case_ctl_no_raise_probe();
    case_est_carrier_limit();
    case_est_p_valid_boundary();
    case_est_unknown();
    case_profile();
    case_rate_index_for_shape();
    case_init_plan();
    case_rate_mask();
    case_wire_profile();
    case_asymmetric_init();
    case_fbk_overlay();
    case_epoch_base();
    case_ctl_fail_down_zero();
    case_ctl_probe_gate();
    case_ctl_reverse();
    case_ctl_epoch_shape();
    case_convergence();
    case_convergence_uplink();
    case_no_oscillation();
    case_bitmap_bounds();

    printf("test_fec_adapt_sclc: %d checks, %d failures\n", checks,
           failures);
    return failures ? 1 : 0;
}
