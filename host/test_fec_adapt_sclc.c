/* test_fec_adapt_sclc.c - host test for the SC-LDPC estimator and controller.
 *
 * WHAT THIS IS
 *   The exact code from tbtun/tbt_fec_adapt_sclc.c, compiled for the host.
 *   It checks:
 *
 *     - the estimator: eight 500 ms buckets, the 4 s window, the 1 s fast
 *       window, the EWMA and its replay ring, 256-bit miss and alive masks,
 *       grouped blocks, r_fail, codec failures, pauses and clock steps;
 *     - the continuous controller: the Robbins-Monro quantile against an
 *       empirical quantile, a step response, loop stability under 0..8
 *       blocks of feedback delay, burst feedforward, the 16-block cold
 *       start, the online surplus integrator and its anti-windup, the
 *       eight-failure backup, and the carrier/abandoned freeze;
 *     - the buildable split mapping and the continuous wire profile;
 *     - the late-DATA ledger and the shared bitmap helpers.
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
 * Continuous controller harness
 * ------------------------------------------------------------------ */

static void init_down(struct tbt_fec_adapt_sclc_ctl *c)
{
    eq_u32((u32)tbt_fec_adapt_sclc_init(c, 256, 1, 8, 128,
                                        TBT_FEC_SCLC_DOWN),
           0, "init: downlink");
}

static void init_up(struct tbt_fec_adapt_sclc_ctl *c)
{
    eq_u32((u32)tbt_fec_adapt_sclc_init(c, 32, 8, 2, 16, TBT_FEC_SCLC_UP),
           0, "init: uplink");
}

static void feed(struct tbt_fec_adapt_sclc_ctl *c, u16 E, u16 S,
                 bool first_ok, u16 real)
{
    tbt_fec_adapt_sclc_block_sample(c, E, S, first_ok, real);
}

static void tick_ex(struct tbt_fec_adapt_sclc_ctl *c, u64 *now, u16 d,
                    u32 w_ppm, u16 alive, u16 stalled, u32 abandoned,
                    struct tbt_fec_adapt_sclc_out *out)
{
    tbt_fec_adapt_sclc_tick(c, *now, d, w_ppm, alive, stalled, abandoned,
                            out);
    *now += 10 * MS;
}

/* One feedback tick with the shipped defaults: one block of delay, W/T_b =
 * 1.0, the whole carrier population alive and nothing abandoned. */
static void tick(struct tbt_fec_adapt_sclc_ctl *c, u64 *now,
                 struct tbt_fec_adapt_sclc_out *out)
{
    tick_ex(c, now, 1, 1000000u, (u16)(c->groups * c->shares), 0, 0, out);
}

/* Feed `n` blocks with the same evidence, one tick per block. */
static void run(struct tbt_fec_adapt_sclc_ctl *c, u64 *now, u16 n, u16 E,
                u16 S, bool first_ok, u16 real,
                struct tbt_fec_adapt_sclc_out *out)
{
    u16 i;

    for (i = 0; i < n; i++)
    {
        feed(c, E, S, first_ok, real);
        tick(c, now, out);
    }
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
    ok(tbt_fec_late_note_miss(&ctx, 100, 0, 128, 128, sc, cf, alive, 256,
                              128),
       "ledger: note 51 counted misses");
    memset(correct, 0, sizeof(correct));
    tbt_fec_late_finalize(&ctx, 100, 42, correct);
    rec = tbt_fec_late_find(&ctx, 100);
    eq_u32(rec ? rec->miss : 0, 51, "ledger: 51 misses committed");
    for (i = 0; i < 38; i++)
    {
        enum tbt_fec_late_evt ev = tbt_fec_late_credit(
            &ctx, 100, 0, (u16)i, 128, 128, &has);

        ok(ev == TBT_FEC_LATE_SETTLED && has,
           "ledger: post-fold miss settles once");
        if (ev == TBT_FEC_LATE_SETTLED && has && rec->miss)
            rec->miss--;
    }
    eq_u32(rec->miss, 13, "ledger: 38 in-flight shares cancel to 13");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, 128, 128, &has) ==
           TBT_FEC_LATE_DUP,
       "ledger: repeated late share is a duplicate");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 40, 128, 128, &has) ==
           TBT_FEC_LATE_SETTLED && has,
       "ledger: a new late share still settles");
    ok(tbt_fec_late_credit(&ctx, 100, 0, 40, 128, 128, &has) ==
           TBT_FEC_LATE_DUP,
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
    tbt_fec_late_note_miss(&ctx, 100, 0, 128, 128, sc, cf, alive, 256, 128);
    tbt_fec_late_note_miss(&ctx, 101, 0, 128, 128, sc, cf, alive, 256, 128);
    memset(correct, 0, sizeof(correct));
    tbt_fec_late_finalize(&ctx, 100, 1, correct);
    tbt_fec_late_finalize(&ctx, 101, 2, correct);
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, 128, 128, &has) ==
           TBT_FEC_LATE_SETTLED,
       "ledger: block A settles");
    eq_u32(tbt_fec_late_find(&ctx, 101)->miss, 1,
           "ledger: block B miss is untouched");

    /* Old epoch is unknown, never credited. */
    ctx.epoch = 8;
    ok(tbt_fec_late_credit(&ctx, 100, 0, 0, 128, 128, &has) ==
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
        tbt_fec_late_note_miss(&ctx, (u64)(900 + i), 0, 128, 128, sc, cf,
                               alive, 256, 128);
        memset(correct, 0, sizeof(correct));
        tbt_fec_late_finalize(&ctx, (u64)(900 + i), (u64)i, correct);
    }
    ok(tbt_fec_late_credit(&ctx, 900, 0, 0, 128, 128, &has) ==
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
    ok(tbt_fec_late_credit(&ctx, 200, 0, 255, 128, 128, &has) ==
           TBT_FEC_LATE_PENDING,
       "ledger: share 255 pending");
    tbt_fec_late_take(&ctx, 200, 0, out);
    ok(out[255] == 1, "ledger: share 255 take is in bounds");

    /* A record keeps the block's own n/m. The bitmap index is
     * group*(n+m)+share, so n+m must stay direction-constant while n names
     * the DATA side. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.epoch = 11;
    ctx.n = 4;
    ctx.m = 4;
    ctx.groups = 1;
    ctx.next_super = 300;
    ok(tbt_fec_late_credit(&ctx, 300, 0, 3, 4, 4, &has) ==
           TBT_FEC_LATE_PENDING,
       "ledger: block n/m 4/4 pending");
    rec = tbt_fec_late_find(&ctx, 300);
    ok(rec && rec->n == 4 && rec->m == 4,
       "ledger: record stores the block's n/m");
    ok(tbt_fec_late_credit(&ctx, 300, 0, 4, 5, 3, &has) ==
           TBT_FEC_LATE_UNKNOWN,
       "ledger: one block cannot change n/m");
    ctx.next_super = 301;
    ok(tbt_fec_late_credit(&ctx, 301, 0, 5, 5, 3, &has) ==
           TBT_FEC_LATE_PENDING,
       "ledger: another block may use 5/3");
    rec = tbt_fec_late_find(&ctx, 301);
    ok(rec && rec->n == 5 && rec->m == 3,
       "ledger: second record keeps 5/3");
    memset(out, 0, sizeof(out));
    tbt_fec_late_take(&ctx, 301, 0, out);
    ok(out[5] == 1 && out[4] == 0,
       "ledger: 5/3 take indexes the parity share");
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

    {
        struct tbt_fec_adapt_sclc_est r;
        u8 one[TBT_FEC_BITMAP_BYTES] = {0};
        u32 before;

        memset(&r, 0, sizeof(r));
        tbt_fec_adapt_sclc_est_init(&r, 256);
        r.carriers = 256;
        tbt_fec_adapt_bitmap_set(one, 0);
        tbt_fec_adapt_sclc_est_block(&r, now, 0, 1, one, alive);
        tbt_fec_adapt_sclc_est_super(&r, now);
        before = r.win[0].miss;
        r.win[0].start_ns++;
        ok(!tbt_fec_adapt_sclc_est_correct_super(&r, 1),
           "ewma ring: reused bucket generation rejects correction");
        eq_u32(r.win[0].miss, before,
               "ewma ring: reused bucket miss is unchanged");
    }
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

/* ------------------------------------------------------------------ *
 * Quantile convergence and step response
 * ------------------------------------------------------------------ */

static u32 rng_state;

static u32 rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* P(k) = 0.3 * 0.7^k, capped at 8 shares: the cold seed can only land on
 * or below the (1-eps) quantile, so the estimate never has to walk down. */
static u16 geom_e(void)
{
    double u = (double)(rng_next() >> 8) / 16777216.0;
    u16 k = 0;

    while (u >= 0.3 && k < 8)
    {
        u = (u - 0.3) / 0.7;
        k++;
    }
    return k;
}

static void sort_u16(u16 *v, u32 n)
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

static void case_q_convergence(void)
{
    static const char *const msg[2] = {
        "q converge: geometric q_raw is the empirical quantile",
        "q converge: uniform q_raw is the empirical quantile",
    };
    int kind;

    for (kind = 0; kind < 2; kind++)
    {
        struct tbt_fec_adapt_sclc_ctl c;
        struct tbt_fec_adapt_sclc_out out;
        u16 hist[512];
        u64 now = SEC;
        u32 i, rank;
        double q, diff;

        rng_state = kind ? 0x9e3779b9u : 0x12345678u;
        init_up(&c);
        for (i = 0; i < 16 + 2000; i++)
        {
            u16 E = kind ? (u16)(rng_next() % 9u) : geom_e();

            if (E > c.n_cur)
                E = c.n_cur;
            feed(&c, E, 3, true, 32);
            tick(&c, &now, &out);
            if (i >= 16 + 2000 - 512)
                hist[i - (16 + 2000 - 512)] = E;
        }
        sort_u16(hist, 512);
        rank = (u32)((((u64)(TBT_FEC_ADAPT_SCLC_ONE - out.eps_q16) * 512u) +
                      TBT_FEC_ADAPT_SCLC_ONE - 1u) >> 16);
        if (!rank)
            rank = 1;
        if (rank > 512)
            rank = 512;
        q = (double)hist[rank - 1];
        diff = out.q_raw_q16 / 65536.0 - q;
        ok(diff <= 1.5 && diff >= -1.5, msg[kind]);
        ok(out.eps_q16 >= TBT_FEC_ADAPT_SCLC_ONE / 32u &&
           out.eps_q16 <= TBT_FEC_ADAPT_SCLC_EPS_MAX_Q16,
           "q converge: eps stays inside the clamp");
    }
}

static void case_q_step(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u32 i, q_prev, m_before, m8 = 0;
    bool monotone = true, bounded = true;
    double diff;

    init_up(&c);
    run(&c, &now, 100, 2, 3, true, 32, &out);
    m_before = out.m_target;
    q_prev = out.q_raw_q16;
    for (i = 0; i < 200; i++)
    {
        feed(&c, 12, 3, true, 32);
        tick(&c, &now, &out);
        if ((u64)out.q_raw_q16 + TBT_FEC_ADAPT_SCLC_ONE < q_prev)
            monotone = false;
        if (out.q_raw_q16 > 14u * TBT_FEC_ADAPT_SCLC_ONE)
            bounded = false;
        q_prev = out.q_raw_q16;
        if (i == 7)
            m8 = out.m_target;
    }
    ok(m8 > m_before, "q step: m_target rises within 8 ticks");
    ok(monotone, "q step: q_raw never drops more than one share");
    ok(bounded, "q step: q_raw never exceeds 14 shares");
    diff = out.q_raw_q16 / 65536.0 - 12.0;
    ok(diff <= 1.5 && diff >= -1.5, "q step: q_raw settles on 12 shares");
}

/* ------------------------------------------------------------------ *
 * Closed-loop stability, burst, cold start, integrator and safety
 * ------------------------------------------------------------------ */

static void case_delay_stability(void)
{
    int d;

    for (d = 0; d <= 8; d++)
    {
        struct tbt_fec_adapt_sclc_ctl c;
        struct tbt_fec_adapt_sclc_out out;
        static u16 mh[1200];
        u64 now = SEC;
        int i, dir = 0, changes = 0;
        u16 mn = 0xffff, mx = 0;
        bool in_range = true;

        init_down(&c);
        for (i = 0; i < 1200; i++)
        {
            int j = i - 1 - d;
            u16 md = j >= 0 ? mh[j] : 128;
            u16 E = md < 160 ? (u16)(160 - md) : 0;

            feed(&c, E, 9, true, 256);
            tick_ex(&c, &now, (u16)d, 1000000u, 256, 0, 0, &out);
            mh[i] = out.m_target;
            if (out.m_target < c.m_min || out.m_target > c.m_max)
                in_range = false;
            if (i >= 800)
            {
                if (mh[i] < mn)
                    mn = mh[i];
                if (mh[i] > mx)
                    mx = mh[i];
                if (i > 800)
                {
                    int step = (int)mh[i] - (int)mh[i - 1];

                    if (step)
                    {
                        step = step > 0 ? 1 : -1;
                        if (dir && step != dir)
                            changes++;
                        dir = step;
                    }
                }
            }
        }
        ok(in_range, "delay: m_target stays inside the clamp");
        ok(mn > c.m_min && mx < c.m_max,
           "delay: the loop is not pinned to a clamp");
        ok(mx - mn <= 8, "delay: the last 400 ticks span at most 8 shares");
        ok(changes <= 60, "delay: no persistent oscillation");
    }
}

static void case_burst_onset(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u32 i, m0, peak = 0;
    bool saw_burst = false;

    init_up(&c);
    run(&c, &now, 100, 2, 3, true, 32, &out);
    m0 = out.m_target;
    for (i = 0; i < 2; i++)
    {
        feed(&c, 20, 3, true, 32);
        tick(&c, &now, &out);
        if (out.burst_q16 > 0)
            saw_burst = true;
        if (out.m_target > peak)
            peak = out.m_target;
    }
    ok(saw_burst, "burst: burst_q16 is positive during onset");
    ok(peak >= m0 + 3, "burst: m_target rises by at least 3 shares");
    run(&c, &now, 32, 2, 3, true, 32, &out);
    ok((u32)out.m_target + 2u >= m0 && m0 + 2u >= (u32)out.m_target,
       "burst: m_target returns to the pre-burst value");
    eq_u32(out.burst_q16, 0,
           "burst: burst_q16 is zero after 32 clean blocks");
}

static void case_low_sample(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u32 i, q16;
    u16 m_init;
    bool held = true;

    init_up(&c);
    eq_u32(c.m_cur, 16, "low sample: m starts at shares - n_init");
    m_init = c.m_cur;
    for (i = 0; i < 16; i++)
    {
        feed(&c, 12, 3, true, 32);
        tick(&c, &now, &out);
        if (out.m_target != m_init ||
            out.reason != TBT_FEC_ADAPT_SCLC_R_COLD)
            held = false;
    }
    ok(held, "low sample: 16 cold blocks hold m and report COLD");
    q16 = out.q_raw_q16;
    feed(&c, 12, 3, true, 32);
    tick(&c, &now, &out);
    ok(out.m_target != m_init || out.q_raw_q16 != q16,
       "low sample: the 17th block leaves the cold start");
    m_init = out.m_target;
    tick_ex(&c, &now, 1, 1000000u, 0, 0, 0, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
           "low sample: alive 0 pauses the loop");
    eq_u32(out.m_target, m_init, "low sample: a pause holds m");
}

static void case_delta_online(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u32 i, d0, d1, d2, d3;
    bool held = true;

    init_up(&c);
    run(&c, &now, 16, 2, 3, true, 32, &out);
    d0 = out.delta_q16;
    run(&c, &now, 256, 2, 5, true, 32, &out);
    d1 = out.delta_q16;
    ok((u64)d1 + 4u * TBT_FEC_ADAPT_SCLC_ONE <= d0,
       "delta: a surplus above the margin lowers delta by four shares");
    run(&c, &now, 256, 2, 1, true, 32, &out);
    d2 = out.delta_q16;
    ok(d2 > d1, "delta: a surplus below the margin raises delta");

    /* Saturate m_max first: E is far above any quantile and S = extra + 1
     * leaves delta alone. */
    run(&c, &now, 512, 200, 3, true, 256, &out);
    eq_u32(out.m_target, c.m_max, "delta: E = 200 saturates m_max");
    d3 = out.delta_q16;
    for (i = 0; i < 64; i++)
    {
        feed(&c, 200, 1, true, 256);
        tick(&c, &now, &out);
        if (out.delta_q16 != d3)
            held = false;
    }
    ok(held, "delta: saturation stops the surplus integrator");
}

static void case_backup(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u32 i, m0;

    init_up(&c);
    run(&c, &now, 16 + 10, 2, 3, true, 32, &out);
    m0 = out.m_target;
    for (i = 0; i < 8; i++)
    {
        feed(&c, 2, 3, false, 32);
        tick(&c, &now, &out);
    }
    ok(out.backup >= 2, "backup: eight failures add at least two shares");
    ok(out.m_target >= m0 + 2, "backup: m_target grows by the backup");
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_BACKUP, "backup: reason");
    run(&c, &now, 20, 2, 3, true, 32, &out);
    eq_u32(out.backup, 0, "backup: decays after 20 good blocks");

    /* Downlink: the first failure in a full 32-block window is above the
     * 2*eps first-failure rate, so one failure backs the loop up. */
    init_down(&c);
    run(&c, &now, 16, 2, 9, true, 256, &out);
    run(&c, &now, 32, 2, 9, true, 256, &out);
    feed(&c, 2, 9, false, 256);
    tick(&c, &now, &out);
    ok(out.backup > 0, "backup: one downlink failure in 32 blocks");
}

static void case_freeze(void)
{
    struct tbt_fec_adapt_sclc_ctl c;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = SEC;
    u16 i, m0;

    init_up(&c);
    run(&c, &now, 16 + 16, 2, 3, true, 32, &out);
    m0 = out.m_target;
    tick_ex(&c, &now, 1, 1000000u, 0, 0, 0, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
           "freeze: a dead population pauses");
    eq_u32(out.m_target, m0, "freeze: a dead population holds m");
    tick_ex(&c, &now, 1, 1000000u, 256, 26, 0, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
           "freeze: 26 of 256 stalled pauses");
    eq_u32(out.m_target, m0, "freeze: a stalled population holds m");
    for (i = 0; i < 8; i++)
        feed(&c, 2, 3, true, 32);
    tick_ex(&c, &now, 1, 1000000u, 256, 0, 3, &out);
    eq_u32(out.reason, TBT_FEC_ADAPT_SCLC_R_PAUSED,
           "freeze: 3 abandoned shares of 256 pause");
    eq_u32(out.m_target, m0, "freeze: abandoned shares hold m");
    tick_ex(&c, &now, 1, 1000000u, 256, 0, 0, &out);
    eq_u32(out.frozen, 0, "freeze: recovery clears frozen");
    ok(out.reason != TBT_FEC_ADAPT_SCLC_R_PAUSED,
       "freeze: recovery resumes the loop");
}

/* ------------------------------------------------------------------ *
 * Buildable mapping and wire profiles
 * ------------------------------------------------------------------ */

static bool up_constructible(u16 n)
{
    return n == 1 || n == 2 || (n >= 4 && n <= 18) ||
           (n >= 21 && n <= 25) || n == 28 || n == 30;
}

static void case_buildable_output(void)
{
    static const char *const msg[2] = {
        "buildable: downlink output",
        "buildable: uplink output",
    };
    int dir;

    for (dir = 0; dir < 2; dir++)
    {
        struct tbt_fec_adapt_sclc_ctl c;
        struct tbt_fec_adapt_sclc_out out;
        u64 now = SEC;
        u16 E, S = dir ? 3 : 9;
        u16 real = dir ? 32 : 256;
        u16 i;
        bool good = true;

        if (dir)
            init_up(&c);
        else
            init_down(&c);
        run(&c, &now, 16, 2, S, true, real, &out);
        for (E = 0; E <= 40; E += 2)
        {
            for (i = 0; i < 8; i++)
            {
                feed(&c, E, S, true, real);
                tick(&c, &now, &out);
                if ((u16)(out.n_target + out.m_target) != c.shares ||
                    !tbt_fec_adapt_sclc_buildable(c.variant,
                                                  out.n_target,
                                                  out.m_target) ||
                    out.m_target < c.m_min || out.m_target > c.m_max)
                    good = false;
                if (dir && !up_constructible(out.n_target))
                    good = false;
            }
        }
        ok(good, msg[dir]);
    }
}

static void make_cfg(struct tbt_fec_cfg *cfg, u16 n, u16 m, u16 groups,
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

static u32 ppm_ceil(u16 n, u16 shares)
{
    return (u32)((((u64)n * 1000000u) + shares - 1u) / shares);
}

static void case_wire_profile(void)
{
    struct tbt_fec_adapt_sclc_wire w;
    struct tbt_fec_cfg cfg, back;
    u16 carriers = 256;

    /* Continuous downlink: 192/64 is the widest payload. */
    make_cfg(&cfg, 192, 64, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "profile: downlink 192/64");
    eq_u32(w.kind, TBT_FEC_SCLC_DOWN, "profile: downlink kind");
    eq_u32(w.symbol, 0, "profile: downlink binary");
    eq_u32(w.block, 256, "profile: downlink block");
    eq_u32(w.L, 8, "profile: downlink L");
    eq_u32(w.rate_pct, 75, "profile: downlink 192/64 rate");
    eq_u32(w.n, 192, "profile: downlink n");
    eq_u32(w.m, 64, "profile: downlink m");
    eq_u32(w.groups, 1, "profile: downlink groups");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 7,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "profile: downlink to cfg");
    eq_u32(back.w, 256, "profile: downlink cfg w");
    eq_u32(back.k, 750, "profile: downlink cfg k");
    eq_u32(back.d, 0, "profile: downlink cfg d");
    eq_u32(back.codec_variant, 2, "profile: downlink cfg variant");
    eq_u32(back.L, 8, "profile: downlink cfg L");
    eq_u32(back.block, 256, "profile: downlink cfg block");
    eq_u32(back.rate_ppm, 750000, "profile: downlink cfg rate");

    /* Continuous downlink: 101/155 carries the 39% label. */
    make_cfg(&cfg, 101, 155, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "profile: downlink 101/155");
    eq_u32(w.rate_pct, 39, "profile: downlink 101/155 rate");
    eq_u32(w.rate_ppm, 394532, "profile: downlink 101/155 ppm");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 8, 0, &back), 0,
           "profile: downlink 101/155 to cfg");
    eq_u32(back.k, 394, "profile: downlink 101/155 cfg k");
    eq_u32(back.d, 0, "profile: downlink 101/155 cfg d");

    /* Continuous uplink: 16/16 of 8x32. */
    make_cfg(&cfg, 16, 16, 8, 16, 128, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "profile: uplink 16/16");
    eq_u32(w.kind, TBT_FEC_SCLC_UP, "profile: uplink kind");
    eq_u32(w.symbol, 1, "profile: uplink gf16");
    eq_u32(w.block, 128, "profile: uplink block");
    eq_u32(w.L, 16, "profile: uplink L");
    eq_u32(w.rate_pct, 50, "profile: uplink 16/16 rate");
    eq_u32(w.rate_ppm, 500000, "profile: uplink 16/16 ppm");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 9,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "profile: uplink to cfg");
    eq_u32(back.w, 16, "profile: uplink cfg w");
    eq_u32(back.k, 128, "profile: uplink cfg k");
    eq_u32(back.d, 128, "profile: uplink cfg d");
    eq_u32(back.codec_variant, 2, "profile: uplink cfg variant");
    eq_u32(back.L, 16, "profile: uplink cfg L");
    eq_u32(back.block, 128, "profile: uplink cfg block");
    eq_u32(back.rate_ppm, 500000, "profile: uplink cfg rate");

    /* Continuous uplink: 17/15 is buildable but was never on the ladder. */
    make_cfg(&cfg, 17, 15, 8, 16, 128, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "profile: uplink 17/15");
    eq_u32(w.rate_pct, 53, "profile: uplink 17/15 rate");
    eq_u32(w.rate_ppm, 531250, "profile: uplink 17/15 ppm");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 10, 0, &back), 0,
           "profile: uplink 17/15 to cfg");
    eq_u32(back.d, 136, "profile: uplink 17/15 cfg d");

    /* The hard profile boundaries. */
    make_cfg(&cfg, 193, 63, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "profile: downlink n above 192 rejected");
    make_cfg(&cfg, 128, 127, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 255, &w), (u32)-1,
           "profile: a downlink split must span 256 shares");
    make_cfg(&cfg, 128, 128, 2, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 512, &w), (u32)-1,
           "profile: a downlink uses one group");
    make_cfg(&cfg, 128, 128, 1, 16, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "profile: downlink L 16 rejected");
    make_cfg(&cfg, 128, 128, 1, 8, 512, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "profile: downlink block 512 rejected");
    make_cfg(&cfg, 16, 16, 8, 8, 128, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "profile: uplink L 8 rejected");
    make_cfg(&cfg, 16, 16, 8, 16, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "profile: uplink block 256 rejected");
    make_cfg(&cfg, 16, 16, 8, 16, 128, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 255, &w), (u32)-1,
           "profile: carriers must match groups*(n+m)");

    /* Compatibility: only the split may change. */
    {
        struct tbt_fec_cfg a, b;

        make_cfg(&a, 16, 16, 8, 16, 128, 0);
        b = a;
        b.n = 17;
        b.m = 15;
        ok(tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "profile: an uplink rate change is compatible");
        b = a;
        b.L = 8;
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "profile: an L change is incompatible");
        b = a;
        b.block = 256;
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "profile: a block change is incompatible");
        make_cfg(&b, 192, 64, 1, 8, 256, 0);
        ok(!tbt_fec_adapt_sclc_wire_compatible(&a, &b, carriers),
           "profile: a direction change is incompatible");
    }
}

static void case_wire_continuous(void)
{
    struct tbt_fec_adapt_sclc_wire w;
    struct tbt_fec_cfg cfg, back;
    u16 carriers = 256;

    make_cfg(&cfg, 192, 64, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: downlink 192/64 accepted");
    eq_u32(w.rate_pct, 75, "wire: downlink 192/64 is 75%");
    eq_u32(w.rate_ppm, ppm_ceil(192, 256), "wire: downlink 192/64 ppm");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 3,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "wire: downlink 192/64 to cfg");
    eq_u32(back.rate_ppm, ppm_ceil(192, 256),
           "wire: downlink 192/64 roundtrip ppm");

    make_cfg(&cfg, 101, 155, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), 0,
           "wire: downlink 101/155 accepted");
    eq_u32(w.rate_pct, 39, "wire: downlink 101/155 is 39%");
    eq_u32(w.rate_ppm, ppm_ceil(101, 256), "wire: downlink 101/155 ppm");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 4,
                                               TBT_FEC_CFG_F_APPLY, &back),
           0, "wire: downlink 101/155 to cfg");
    eq_u32(back.rate_ppm, ppm_ceil(101, 256),
           "wire: downlink 101/155 roundtrip ppm");
    eq_u32(back.n, 101, "wire: downlink 101/155 roundtrip n");
    eq_u32(back.m, 155, "wire: downlink 101/155 roundtrip m");

    /* Rejections. */
    make_cfg(&cfg, 193, 63, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: downlink n above 192 rejected");
    make_cfg(&cfg, 3, 29, 8, 16, 128, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, carriers, &w), (u32)-1,
           "wire: uplink n = 3 rejected");
    make_cfg(&cfg, 192, 64, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 255, &w), (u32)-1,
           "wire: groups*(n+m) must equal carriers");
    make_cfg(&cfg, 128, 127, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 255, &w), (u32)-1,
           "wire: a downlink split must span 256 shares");
}

/* The 0.4.0 pair runs adaptive=on with an asymmetric INIT: the near TX is
 * the fixed binary downlink (1x256/L8/r0.50), the far TX is the GF(16)
 * uplink (8x32/L16). Both directions must resolve, and they are an INIT,
 * not a rate switch. */
static void case_asymmetric_init(void)
{
    struct tbt_fec_adapt_sclc_wire down, up;
    struct tbt_fec_cfg d, u;

    make_cfg(&d, 128, 128, 1, 8, 256, 500000);
    make_cfg(&u, 8, 24, 8, 16, 128, 250000);

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

/* The controller no longer carries an epoch; the CFG body does. A switch
 * stamps the caller's generation and flags, and the body still resolves to
 * the same continuous split. */
static void case_epoch_base(void)
{
    struct tbt_fec_adapt_sclc_wire w, back;
    struct tbt_fec_cfg cfg, body;

    make_cfg(&cfg, 101, 155, 1, 8, 256, 0);
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&cfg, 256, &w), 0,
           "epoch base: the split resolves");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_to_cfg(&w, 1,
                                               TBT_FEC_CFG_F_APPLY, &body),
           0, "epoch base: to cfg");
    eq_u32(body.epoch, 1, "epoch base: the generation is stamped");
    eq_u32(body.flags, TBT_FEC_CFG_F_APPLY,
           "epoch base: the flags are stamped");
    eq_u32((u32)tbt_fec_adapt_sclc_wire_of_cfg(&body, 256, &back), 0,
           "epoch base: the stamped body resolves");
    eq_u32(back.n, 101, "epoch base: n is unchanged");
    eq_u32(back.m, 155, "epoch base: m is unchanged");
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
    case_est_clock();
    case_est_carrier_limit();
    case_est_p_valid_boundary();
    case_est_unknown();
    case_q_convergence();
    case_q_step();
    case_delay_stability();
    case_burst_onset();
    case_low_sample();
    case_delta_online();
    case_backup();
    case_freeze();
    case_buildable_output();
    case_wire_profile();
    case_wire_continuous();
    case_asymmetric_init();
    case_fbk_overlay();
    case_epoch_base();
    case_bitmap_bounds();

    printf("test_fec_adapt_sclc: %d checks, %d failures\n", checks,
           failures);
    return failures ? 1 : 0;
}
