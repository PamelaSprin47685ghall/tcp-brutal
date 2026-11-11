/* calib_sclc_up.c - host calibration of the uplink 8x32 GF(16) SC-LDPC
 * ladder against the real codec.
 *
 * The harness builds the shipped codec (TBT_SCLC_MAP_BITPLANE64, block 128,
 * L 16, max_core 256, eight groups) for each ladder point that
 * tbt_fec_adapt_sclc_shape() derives for 32 shares per group, then measures:
 *
 *   scan     i.i.d. share loss: per-group and 8-group super-block failure
 *            over p = 0..60 percent, plus a 0.25 percent dense scan around
 *            the 1e-2/1e-3/1e-4 super-block failure crossings.
 *   wake     the production 0 ms wake line (TBT_FEC_SCLC_WAKE_EXTRA_UP):
 *            arrival order is data shares then parity shares, the first
 *            attempt fires at real+2 present shares, and every later share
 *            clears the backoff and retries.  Reports the first-attempt
 *            failure rate and the distribution of the share count at the
 *            first successful decode.
 *   ge       Gilbert-Elliott share loss with mean burst 2/4/8 shares.
 *   carrier  k of the 256 carriers dead, random and contiguous.
 *   cpu      encode and decode cost per group.
 *   selftest byte-exact round trip and the zero-buffer scan invariant.
 *
 * All randomness is a splitmix64 stream seeded from the section, ladder
 * point, p and a phase tag, so a run is reproducible regardless of order or
 * parallelism.  p is carried in integer ppm; a loss is
 * (rng % 1000000) < p_ppm, so 0.25 percent steps are exact.
 *
 * CSV schema (long format):
 *   section,level,pct,n,m,rate_ppm,boundary,param,param2,metric,value,
 *   events,trials,ci_lo,ci_hi,note
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tbt_fec_adapt_sclc.h"
#include "tbt_sclc_uplink.h"

#define CAL_GROUPS 8u
#define CAL_SHARES 32u
#define CAL_MAX_SHARES 32u
#define CAL_CARRIERS 256u
#define CAL_BASE_SEED 0x5C1C0A1B20261111ULL
#define CAL_PPM 1000000u
#define CAL_P_MAX 600000u
#define CAL_COARSE_STEP 10000u  /* 1 percent */
#define CAL_DENSE_STEP 2500u    /* 0.25 percent */
#define CAL_COARSE_TRIALS 10000L
#define CAL_DENSE_TRIALS 50000L
#define CAL_WAKE_TRIALS 4000L
#define CAL_GE_TRIALS 10000L
#define CAL_CARRIER_TRIALS 10000L
#define CAL_CPU_TRIALS 2000L

#define CAL_TAG_SCAN_COARSE 0x11u
#define CAL_TAG_SCAN_DENSE 0x12u
#define CAL_TAG_WAKE 0x21u
#define CAL_TAG_GE 0x31u
#define CAL_TAG_CARRIER 0x41u
#define CAL_TAG_CPU 0x51u

static int cal_core = 256;

struct cal_rng
{
    u64 s;
};

static u64 cal_mix(u64 x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void cal_rng_seed(struct cal_rng *r, u32 tag, u32 pct, u32 a, u32 b)
{
    u64 x = CAL_BASE_SEED ^ ((u64)tag << 56);

    x = cal_mix(x ^ ((u64)pct << 32) ^ a);
    x = cal_mix(x ^ ((u64)b << 17) ^ 0xA5A5A5A5A5A5A5A5ULL);
    r->s = x | 1u;
}

static u32 cal_rng_u32(struct cal_rng *r)
{
    r->s += 0x9E3779B97F4A7C15ULL;
    {
        u64 z = r->s;

        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return (u32)(z >> 32);
    }
}

/* 1 = share lost. p_ppm is exact, so the sweep has no float rounding. */
static int cal_loss(struct cal_rng *r, u32 p_ppm)
{
    return (cal_rng_u32(r) % CAL_PPM) < p_ppm;
}

static double cal_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void cal_wilson(long k, long n, double *lo, double *hi)
{
    double z = 1.96, p, d, c, s;

    if (n <= 0)
    {
        *lo = 0;
        *hi = 1;
        return;
    }
    p = (double)k / (double)n;
    d = 1.0 + z * z / (double)n;
    c = p + z * z / (2.0 * (double)n);
    s = z * sqrt((p * (1.0 - p) + z * z / (4.0 * (double)n)) / (double)n);
    *lo = (c - s) / d;
    *hi = (c + s) / d;
    if (*lo < 0.0)
        *lo = 0.0;
    if (*hi > 1.0)
        *hi = 1.0;
}

struct level
{
    u8 idx;
    u8 pct;
    u16 n;
    u16 m;
    u16 rate_num;
    u32 rate_ppm;
    u32 boundary_ppm;
    u32 codes;
    u32 n_par;
    u32 n_data;
    u32 nsym;
    struct tbt_sclc_uplink *u;
    u8 *dat[CAL_GROUPS][CAL_MAX_SHARES];
    u8 *par[CAL_GROUPS][CAL_MAX_SHARES];
};

static void cal_header(void)
{
    printf("section,level,pct,n,m,rate_ppm,boundary,param,param2,metric,"
           "value,events,trials,ci_lo,ci_hi,note\n");
}

/* A note is free text; a comma would add a CSV column. */
static void cal_note(char *dst, size_t len, const char *src)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < len; i++)
        dst[i] = src[i] == ',' ? ';' : src[i];
    dst[i] = 0;
}

static void cal_row(const char *section, const struct level *L,
                    const char *param, const char *param2,
                    const char *metric, double value, long events,
                    long trials, double lo, double hi, const char *note)
{
    char clean[256];

    cal_note(clean, sizeof(clean), note);
    printf("%s,%u,%u,%u,%u,%u,%u,%s,%s,%s,%.9g,%ld,%ld,%.6f,%.6f,%s\n",
           section, (unsigned)L->idx, (unsigned)L->pct, (unsigned)L->n,
           (unsigned)L->m, (unsigned)L->rate_ppm,
           (unsigned)L->boundary_ppm, param, param2, metric, value, events,
           trials, lo, hi, clean);
}

/* ------------------------------------------------------------------ *
 * Level: one ladder point built from the controller's own shape call.
 * ------------------------------------------------------------------ */


static void level_close(struct level *L)
{
    u32 g, i;

    for (g = 0; g < CAL_GROUPS; g++)
    {
        for (i = 0; i < CAL_MAX_SHARES; i++)
        {
            free(L->dat[g][i]);
            free(L->par[g][i]);
            L->dat[g][i] = NULL;
            L->par[g][i] = NULL;
        }
    }
    tbt_sclc_uplink_free(L->u);
    L->u = NULL;
}

static int level_open(struct level *L, u8 pct)
{
    struct tbt_fec_adapt_sclc_shape sh;
    struct tbt_sclc_params p;
    u32 g, i;

    memset(L, 0, sizeof(*L));
    L->pct = pct;
    L->idx = tbt_fec_adapt_sclc_rate_index(pct);
    if (L->idx == 0xff)
    {
        fprintf(stderr, "calib: rate %u is not on the ladder\n", pct);
        return -1;
    }
    if (tbt_fec_adapt_sclc_shape(pct, CAL_SHARES, CAL_GROUPS, &sh) != 0)
    {
        fprintf(stderr, "calib: shape(%u, %u shares, %u groups) failed\n",
                pct, CAL_SHARES, CAL_GROUPS);
        return -1;
    }
    if (sh.groups != CAL_GROUPS || (u32)sh.n + sh.m != CAL_SHARES)
    {
        fprintf(stderr, "calib: shape %u returned n=%u m=%u groups=%u\n",
                pct, sh.n, sh.m, sh.groups);
        return -1;
    }
    if (tbt_fec_adapt_sclc_rate_index_for_shape(sh.n, sh.m) != L->idx)
    {
        fprintf(stderr, "calib: n=%u m=%u does not map back to ladder %u\n",
                sh.n, sh.m, L->idx);
        return -1;
    }
    L->n = sh.n;
    L->m = sh.m;
    L->rate_ppm = sh.rate_ppm;
    L->boundary_ppm = CAL_PPM - sh.rate_ppm - TBT_FEC_ADAPT_SCLC_MARGIN_PPM;
    L->rate_num = (u16)((u32)sh.n * 256u / CAL_SHARES);

    memset(&p, 0, sizeof(p));
    p.n = sh.n;
    p.m = sh.m;
    p.block = TBT_FEC_SCLC_BLOCK_UP;
    p.L = TBT_FEC_SCLC_L_UP;
    p.rate_num = L->rate_num;
    p.rate_den = 256;
    p.max_core = (u16)cal_core;
    p.mapping = TBT_SCLC_MAP_BITPLANE64;
    p.seed = CAL_BASE_SEED;
    L->u = tbt_sclc_uplink_new((u16)CAL_GROUPS, &p);
    if (!L->u)
    {
        fprintf(stderr, "calib: codec build failed for %u/%u\n", sh.n, sh.m);
        return -1;
    }
    L->codes = tbt_sclc_uplink_codes(L->u);
    L->n_par = tbt_sclc_uplink_n_par(L->u);
    L->n_data = tbt_sclc_uplink_n_data(L->u);
    L->nsym = L->n_data + L->n_par;

    for (g = 0; g < CAL_GROUPS; g++)
    {
        for (i = 0; i < sh.n; i++)
            L->dat[g][i] = calloc(1, TBT_FEC_S);
        for (i = 0; i < sh.m; i++)
            L->par[g][i] = calloc(1, TBT_FEC_S);
        for (i = 0; i < sh.n; i++)
            if (!L->dat[g][i])
                return -1;
        for (i = 0; i < sh.m; i++)
            if (!L->par[g][i])
                return -1;
    }
    return 0;
}

/* Decode one group. Returns 1 when every data share is known. The data plane
 * treats a block with all real DATA shares present as ready without a codec
 * call; tbt_sclc_uplink_decode has the same fast path. */
static int level_group_ok(struct level *L, u32 g, const u8 *dp,
                          const u8 *pp)
{
    struct tbt_sclc_stat st;
    int ret;

    ret = tbt_sclc_uplink_decode(L->u, (u16)g, L->dat[g], L->par[g], dp, pp);
    tbt_sclc_uplink_stat(L->u, (u16)g, &st);
    if ((ret == (int)L->n) != (st.codes_failed == 0))
    {
        fprintf(stderr,
                "calib: group outcome mismatch n=%u ret=%d failed=%u ok=%u\n",
                L->n, ret, st.codes_failed, st.codes_ok);
        exit(2);
    }
    return ret == (int)L->n;
}

static void p_name(char *buf, size_t len, u32 ppm)
{
    snprintf(buf, len, "%.4f", (double)ppm / (double)CAL_PPM);
}

/* ------------------------------------------------------------------ *
 * meta
 * ------------------------------------------------------------------ */

static void run_meta(struct level *L)
{
    char note[96];

    cal_row("meta", L, "block", "", "value", TBT_FEC_SCLC_BLOCK_UP, -1, -1,
            0, 0, "TBT_FEC_SCLC_BLOCK_UP");
    cal_row("meta", L, "L", "", "value", TBT_FEC_SCLC_L_UP, -1, -1, 0, 0,
            "TBT_FEC_SCLC_L_UP");
    cal_row("meta", L, "groups", "", "value", CAL_GROUPS, -1, -1, 0, 0, "");
    cal_row("meta", L, "shares", "", "value", CAL_SHARES, -1, -1, 0, 0, "");
    cal_row("meta", L, "rate_num", "", "value", L->rate_num, -1, -1, 0, 0,
            "codec d = n*256/32");
    cal_row("meta", L, "codes", "", "value", L->codes, -1, -1, 0, 0, "");
    cal_row("meta", L, "n_par", "", "value", L->n_par, -1, -1, 0, 0,
            "graph-quantized parity symbols");
    cal_row("meta", L, "n_data", "", "value", L->n_data, -1, -1, 0, 0, "");
    cal_row("meta", L, "nsym", "", "value", L->nsym, -1, -1, 0, 0, "");
    cal_row("meta", L, "code_rate_ppm", "", "value",
            (double)L->n_data * CAL_PPM / (double)L->nsym, -1, -1, 0, 0,
            "n_data/(n_data+n_par), the real symbol rate");
    cal_row("meta", L, "share_rate_ppm", "", "value",
            (double)L->n * CAL_PPM / (double)CAL_SHARES, -1, -1, 0, 0,
            "n/(n+m), the wire share rate");
    cal_row("meta", L, "boundary_ppm", "", "value", L->boundary_ppm, -1, -1,
            0, 0, "1 - ladder ppm - 200000");
    cal_row("meta", L, "down_switch_ppm", "", "value",
            (double)L->boundary_ppm + TBT_FEC_ADAPT_SCLC_HYST_PPM, -1, -1, 0,
            0, "boundary + 2 pp hysteresis");
    cal_row("meta", L, "max_core", "", "value", cal_core, -1, -1, 0, 0,
            "sclc_ops_new default 256");
    cal_row("meta", L, "mapping", "", "value", TBT_SCLC_MAP_BITPLANE64, -1,
            -1, 0, 0, "production Wire v8");
    snprintf(note, sizeof(note), "0x%016llX splitmix64 base seed",
             (unsigned long long)CAL_BASE_SEED);
    cal_row("meta", L, "seed", "", "value", 0, -1, -1, 0, 0, note);
    snprintf(note, sizeof(note), "wake_extra_up=%u", TBT_FEC_SCLC_WAKE_EXTRA_UP);
    cal_row("meta", L, "wake_extra", "", "value",
            TBT_FEC_SCLC_WAKE_EXTRA_UP, -1, -1, 0, 0, note);
}

/* ------------------------------------------------------------------ *
 * scan: i.i.d. share loss, per group and 8-group super-block.
 * ------------------------------------------------------------------ */

struct cal_point
{
    u32 ppm;
    long sup;
    long sup_fail;
    long grp;
    long grp_fail;
};

struct cal_iv
{
    u32 lo;
    u32 hi;
};

static void scan_point(struct level *L, u32 ppm, long trials, u32 tag,
                       u32 phase, struct cal_point *out)
{
    struct cal_rng rng;
    long t, grp = 0, grp_fail = 0, sup = 0, sup_fail = 0;
    u32 g, i;

    cal_rng_seed(&rng, tag, L->pct, ppm, phase);
    for (t = 0; t < trials; t++)
    {
        int any = 0;

        for (g = 0; g < CAL_GROUPS; g++)
        {
            u8 dp[CAL_MAX_SHARES];
            u8 pp[CAL_MAX_SHARES];

            for (i = 0; i < L->n; i++)
                dp[i] = (u8)!cal_loss(&rng, ppm);
            for (i = 0; i < L->m; i++)
                pp[i] = (u8)!cal_loss(&rng, ppm);
            grp++;
            if (!level_group_ok(L, g, dp, pp))
            {
                grp_fail++;
                any = 1;
            }
        }
        sup++;
        if (any)
            sup_fail++;
    }
    out->ppm = ppm;
    out->sup = sup;
    out->sup_fail = sup_fail;
    out->grp = grp;
    out->grp_fail = grp_fail;
}

static void emit_point(struct level *L, const struct cal_point *p,
                       const char *phase)
{
    char ps[24], note[64];
    double lo, hi, rate;

    p_name(ps, sizeof(ps), p->ppm);
    rate = p->grp ? (double)p->grp_fail / (double)p->grp : 0.0;
    cal_wilson(p->grp_fail, p->grp, &lo, &hi);
    cal_row("scan", L, ps, phase, "group_fail_rate", rate, p->grp_fail,
            p->grp, lo, hi, "pooled over 8 groups");
    rate = p->sup ? (double)p->sup_fail / (double)p->sup : 0.0;
    cal_wilson(p->sup_fail, p->sup, &lo, &hi);
    snprintf(note, sizeof(note), "any of %u groups", CAL_GROUPS);
    cal_row("scan", L, ps, phase, "super_fail_rate", rate, p->sup_fail,
            p->sup, lo, hi, note);
}

/* Monotone rate at a point index, using the pooled samples sorted by p. */
static double point_rate(const struct cal_point *p)
{
    return p->sup ? (double)p->sup_fail / (double)p->sup : 0.0;
}

static int point_cmp(const void *a, const void *b)
{
    const struct cal_point *x = a, *y = b;

    if (x->ppm < y->ppm)
        return -1;
    if (x->ppm > y->ppm)
        return 1;
    return 0;
}

/* Collapse equal p values (coarse and dense can share a point) and enforce a
 * monotone envelope before interpolation. */
static int points_merge(struct cal_point *dst, const struct cal_point *src,
                        int n, int cap)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
    {
        if (m && dst[m - 1].ppm == src[i].ppm)
        {
            dst[m - 1].sup += src[i].sup;
            dst[m - 1].sup_fail += src[i].sup_fail;
            dst[m - 1].grp += src[i].grp;
            dst[m - 1].grp_fail += src[i].grp_fail;
        }
        else if (m < cap)
        {
            dst[m++] = src[i];
        }
    }
    return m;
}

static double threshold_at(const struct cal_point *pts, int n, double target,
                           char *note, size_t notelen)
{
    double prev_rate = 0.0;
    u32 prev_ppm = 0;
    double cum = 0.0;
    int i;

    note[0] = 0;
    for (i = 0; i < n; i++)
    {
        double rate = point_rate(&pts[i]);

        if (rate > cum)
            cum = rate;
        if (cum >= target)
        {
            double t;

            if (i == 0 || pts[i].ppm == prev_ppm)
                t = (double)pts[i].ppm;
            else if (cum == prev_rate)
                t = (double)pts[i].ppm;
            else
                t = (double)prev_ppm +
                    (target - prev_rate) / (cum - prev_rate) *
                        (double)(pts[i].ppm - prev_ppm);
            snprintf(note, notelen, "bracket %.4f:%.6g %.4f:%.6g",
                     (double)prev_ppm / CAL_PPM, prev_rate,
                     (double)pts[i].ppm / CAL_PPM, cum);
            return t;
        }
        prev_rate = cum;
        prev_ppm = pts[i].ppm;
    }
    snprintf(note, notelen, "not reached by p=%.2f",
             (double)CAL_P_MAX / CAL_PPM);
    return -1.0;
}

static void run_scan(struct level *L, long coarse_trials, long dense_trials)
{
    static struct cal_point coarse[128];
    static struct cal_point dense[256];
    static struct cal_point merged[384];
    static struct cal_iv iv[8];
    int nc = 0, nd = 0, nm, niv = 0;
    u32 ppm;
    int i;

    for (ppm = 0; ppm <= CAL_P_MAX; ppm += CAL_COARSE_STEP)
    {
        scan_point(L, ppm, coarse_trials, CAL_TAG_SCAN_COARSE, 0,
                   &coarse[nc]);
        emit_point(L, &coarse[nc], "coarse");
        nc++;
    }

    /* Bracket every threshold on the coarse curve, expand by 1 pp, merge. */
    {
        static const double targets[3] = {1e-2, 1e-3, 1e-4};
        int ti;

        for (ti = 0; ti < 3; ti++)
        {
            double cum = 0.0;
            int hit = -1;

            for (i = 0; i < nc; i++)
            {
                double rate = point_rate(&coarse[i]);

                if (rate > cum)
                    cum = rate;
                if (cum >= targets[ti])
                {
                    hit = i;
                    break;
                }
            }
            if (hit < 0)
                continue;
            {
                u32 lo = hit ? coarse[hit - 1].ppm : 0u;
                u32 hi = coarse[hit].ppm;

                lo = lo > CAL_COARSE_STEP ? lo - CAL_COARSE_STEP : 0u;
                hi = hi + CAL_COARSE_STEP < CAL_P_MAX
                         ? hi + CAL_COARSE_STEP
                         : CAL_P_MAX;
                for (i = 0; i < niv; i++)
                    if (lo <= iv[i].hi && hi >= iv[i].lo)
                        break;
                if (i < niv)
                {
                    if (lo < iv[i].lo)
                        iv[i].lo = lo;
                    if (hi > iv[i].hi)
                        iv[i].hi = hi;
                }
                else if (niv < (int)(sizeof(iv) / sizeof(iv[0])))
                {
                    iv[niv].lo = lo;
                    iv[niv].hi = hi;
                    niv++;
                }
            }
        }
    }

    /* Merge overlapping intervals again (a third can bridge two). */
    for (i = 0; i < niv; i++)
    {
        int j;

        for (j = i + 1; j < niv; j++)
        {
            if (iv[j].lo <= iv[i].hi && iv[j].hi >= iv[i].lo)
            {
                if (iv[j].lo < iv[i].lo)
                    iv[i].lo = iv[j].lo;
                if (iv[j].hi > iv[i].hi)
                    iv[i].hi = iv[j].hi;
                iv[j] = iv[niv - 1];
                niv--;
                j = i;
            }
        }
    }

    for (i = 0; i < niv; i++)
    {
        u32 p;

        for (p = (iv[i].lo + CAL_DENSE_STEP - 1) / CAL_DENSE_STEP *
                 CAL_DENSE_STEP;
             p <= iv[i].hi; p += CAL_DENSE_STEP)
        {
            int skip = 0, k;

            for (k = 0; k < nc; k++)
                if (coarse[k].ppm == p)
                    skip = 1;
            if (skip)
                continue;
            if (nd >= (int)(sizeof(dense) / sizeof(dense[0])))
                break;
            scan_point(L, p, dense_trials, CAL_TAG_SCAN_DENSE,
                       (u32)i + 1u, &dense[nd]);
            emit_point(L, &dense[nd], "dense");
            nd++;
        }
    }

    /* Thresholds from the pooled coarse + dense samples. */
    {
        static const double targets[3] = {1e-2, 1e-3, 1e-4};
        static const char *names[3] = {"1e-2", "1e-3", "1e-4"};
        struct cal_point all[512];
        int na = 0, ti;

        for (i = 0; i < nc; i++)
            all[na++] = coarse[i];
        for (i = 0; i < nd; i++)
            all[na++] = dense[i];
        qsort(all, (size_t)na, sizeof(all[0]), point_cmp);
        nm = points_merge(merged, all, na,
                          (int)(sizeof(merged) / sizeof(merged[0])));
        for (ti = 0; ti < 3; ti++)
        {
            char note[96];
            double t = threshold_at(merged, nm, targets[ti], note,
                                    sizeof(note));

            cal_row("threshold", L, names[ti], "super", "p_threshold", t, -1,
                    -1, 0, 0, note);
            if (t >= 0.0)
            {
                cal_row("threshold", L, names[ti], "super",
                        "boundary_minus_thr_pp",
                        ((double)L->boundary_ppm - t) / 10000.0, -1, -1, 0,
                        0, "positive means the controller boundary is above "
                           "the measured threshold");
                cal_row("threshold", L, names[ti], "super",
                        "margin_needed_pp",
                        (CAL_PPM - (double)L->rate_ppm - t) / 10000.0, -1,
                        -1, 0, 0, "1 - ladder rate - threshold, in pp");
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * wake: data shares first, parity after; first attempt at real+2.
 * ------------------------------------------------------------------ */

struct wake_group
{
    int ready;      /* all DATA shares arrived: no codec call */
    int stalled;    /* missing DATA but never reached real+2 present */
    int giveup;     /* reached the wake line, no attempt succeeded */
    int attempts;
    int first_fail;
    int first_k;    /* present shares at first success, 0 = none */
};

struct wake_stats
{
    long grp;
    long ready;
    long stalled;
    long giveup;
    long first_fail;
    long attempts;
    long sup;
    long sup_first_fail;
    long sup_giveup;
    long k_grp[CAL_MAX_SHARES + 2];  /* n and n+2..32 */
    long k_sup[CAL_MAX_SHARES + 2];
    long sup_incomplete;
};

static void wake_one(struct level *L, struct cal_rng *rng, u32 ppm,
                     struct wake_group *w)
{
    u8 lost_d[CAL_MAX_SHARES], lost_p[CAL_MAX_SHARES];
    u8 dp[CAL_MAX_SHARES], pp[CAL_MAX_SHARES];
    int i, present = 0, ndata = 0;

    memset(w, 0, sizeof(*w));
    for (i = 0; i < (int)L->n; i++)
    {
        lost_d[i] = (u8)cal_loss(rng, ppm);
        ndata += !lost_d[i];
    }
    for (i = 0; i < (int)L->m; i++)
        lost_p[i] = (u8)cal_loss(rng, ppm);
    for (i = 0; i < (int)L->m; i++)
        present += !lost_p[i];
    present += ndata;

    if (ndata == (int)L->n)
    {
        w->ready = 1;
        w->first_k = (int)L->n;
        return;
    }
    if (present < (int)L->n + (int)TBT_FEC_SCLC_WAKE_EXTRA_UP)
    {
        w->stalled = 1;
        return;
    }

    memset(dp, 0, sizeof(dp));
    memset(pp, 0, sizeof(pp));
    present = 0;
    for (i = 0; i < (int)L->n; i++)
        if (!lost_d[i])
            dp[i] = 1;
    for (i = 0; i < (int)L->n; i++)
        present += dp[i];
    for (i = 0; i < (int)L->m; i++)
    {
        int ok;

        if (lost_p[i])
            continue;
        pp[i] = 1;
        present++;
        if (present < (int)L->n + (int)TBT_FEC_SCLC_WAKE_EXTRA_UP)
            continue;
        w->attempts++;
        ok = level_group_ok(L, 0, dp, pp);
        if (w->attempts == 1 && !ok)
            w->first_fail = 1;
        if (ok)
        {
            w->first_k = present;
            return;
        }
    }
    w->giveup = 1;
}

static void run_wake(struct level *L, long trials)
{
    static const u32 ppm_list[] = {0,     50000,  100000, 150000, 200000,
                                   250000, 300000, 350000, 400000, 450000,
                                   500000, 550000, 600000};
    u32 pi;

    for (pi = 0; pi < sizeof(ppm_list) / sizeof(ppm_list[0]); pi++)
    {
        struct wake_stats s;
        struct cal_rng rng;
        u32 ppm = ppm_list[pi];
        char ps[24], kbuf[24], note[64];
        long t;
        u32 g;

        memset(&s, 0, sizeof(s));
        cal_rng_seed(&rng, CAL_TAG_WAKE, L->pct, ppm, 0);
        for (t = 0; t < trials; t++)
        {
            int any_fail = 0, any_giveup = 0, all_solved = 1;
            int max_k = 0;

            for (g = 0; g < CAL_GROUPS; g++)
            {
                struct wake_group w;

                wake_one(L, &rng, ppm, &w);
                s.grp++;
                s.ready += w.ready;
                s.stalled += w.stalled;
                s.giveup += w.giveup;
                s.first_fail += w.first_fail;
                s.attempts += w.attempts;
                if (w.attempts && w.first_fail)
                    any_fail = 1;
                if (w.giveup)
                    any_giveup = 1;
                if (w.first_k)
                {
                    if (w.first_k > max_k)
                        max_k = w.first_k;
                }
                else
                {
                    all_solved = 0;
                }
                if (w.first_k == (int)L->n)
                    s.k_grp[0]++;
                else if (w.first_k)
                    s.k_grp[w.first_k - (int)L->n]++;
                else if (w.stalled)
                    s.k_grp[CAL_MAX_SHARES]++;
                else
                    s.k_grp[CAL_MAX_SHARES + 1]++;
            }
            s.sup++;
            s.sup_first_fail += any_fail;
            s.sup_giveup += any_giveup;
            if (all_solved)
                s.k_sup[max_k == (int)L->n ? 0
                                           : max_k - (int)L->n]++;
            else
                s.sup_incomplete++;
        }

        p_name(ps, sizeof(ps), ppm);
        cal_row("wake", L, ps, "", "ready_rate",
                (double)s.ready / (double)s.grp, s.ready, s.grp, 0, 0,
                "all DATA shares arrived, no codec call");
        cal_row("wake", L, ps, "", "stalled_rate",
                (double)s.stalled / (double)s.grp, s.stalled, s.grp, 0, 0,
                "missing DATA and never reached real+2 present");
        cal_row("wake", L, ps, "", "first_fail_rate",
                (double)s.first_fail / (double)s.grp, s.first_fail, s.grp, 0,
                0, "unconditional over group trials");
        cal_row("wake", L, ps, "", "first_fail_cond",
                s.attempts ? (double)s.first_fail / (double)s.attempts : 0.0,
                s.first_fail, s.attempts, 0, 0,
                "conditional on a first codec attempt");
        cal_row("wake", L, ps, "", "giveup_rate",
                (double)s.giveup / (double)s.grp, s.giveup, s.grp, 0, 0,
                "no attempt succeeded within the arrived shares");
        cal_row("wake", L, ps, "", "super_first_fail_rate",
                (double)s.sup_first_fail / (double)s.sup, s.sup_first_fail,
                s.sup, 0, 0, "any group's first codec attempt failed");
        cal_row("wake", L, ps, "", "super_giveup_rate",
                (double)s.sup_giveup / (double)s.sup, s.sup_giveup, s.sup, 0,
                0, "any group did not decode within its arrivals");
        snprintf(kbuf, sizeof(kbuf), "k=%u", L->n);
        cal_row("wake", L, ps, kbuf, "first_k_rate",
                (double)s.k_grp[0] / (double)s.grp, s.k_grp[0], s.grp, 0, 0,
                "ready at n, no codec");
        for (g = 2; g <= CAL_SHARES - L->n; g++)
        {
            snprintf(kbuf, sizeof(kbuf), "k=%u", L->n + g);
            cal_row("wake", L, ps, kbuf, "first_k_rate",
                    (double)s.k_grp[g] / (double)s.grp, s.k_grp[g], s.grp, 0,
                    0, "present shares at the first successful decode");
        }
        cal_row("wake", L, ps, "k=stalled", "first_k_rate",
                (double)s.k_grp[CAL_MAX_SHARES] / (double)s.grp,
                s.k_grp[CAL_MAX_SHARES], s.grp, 0, 0, "no wake line");
        cal_row("wake", L, ps, "k=never", "first_k_rate",
                (double)s.k_grp[CAL_MAX_SHARES + 1] / (double)s.grp,
                s.k_grp[CAL_MAX_SHARES + 1], s.grp, 0, 0,
                "woke but never decoded");
        snprintf(kbuf, sizeof(kbuf), "k=%u", L->n);
        cal_row("wake", L, ps, kbuf, "super_first_k_rate",
                (double)s.k_sup[0] / (double)s.sup, s.k_sup[0], s.sup, 0, 0,
                "max over groups: ready at n");
        for (g = 2; g <= CAL_SHARES - L->n; g++)
        {
            snprintf(kbuf, sizeof(kbuf), "k=%u", L->n + g);
            cal_row("wake", L, ps, kbuf, "super_first_k_rate",
                    (double)s.k_sup[g] / (double)s.sup, s.k_sup[g], s.sup, 0,
                    0, "max over groups of the first successful share count");
        }
        cal_row("wake", L, ps, "k=incomplete", "super_first_k_rate",
                (double)s.sup_incomplete / (double)s.sup, s.sup_incomplete,
                s.sup, 0, 0, "at least one group did not decode");
        snprintf(note, sizeof(note), "attempts=%ld", s.attempts);
        cal_row("wake", L, ps, "", "mean_attempts",
                (double)s.attempts / (double)s.grp, s.attempts, s.grp, 0, 0,
                note);
    }
}

/* ------------------------------------------------------------------ *
 * Gilbert-Elliott share loss, mean burst 2/4/8 shares.
 * ------------------------------------------------------------------ */

static void ge_losses(struct cal_rng *rng, u32 ppm, u32 burst, u8 *lost,
                      u32 count)
{
    u32 i;
    u32 q_ppm;
    u32 r_ppm = CAL_PPM / burst;
    int bad;

    if (r_ppm == 0)
        r_ppm = 1;
    if (ppm >= CAL_PPM)
        q_ppm = CAL_PPM;
    else
        q_ppm = (u32)((u64)ppm * CAL_PPM /
                      ((u64)burst * (CAL_PPM - ppm)));
    if (q_ppm > CAL_PPM)
        q_ppm = CAL_PPM;
    bad = (cal_rng_u32(rng) % CAL_PPM) < ppm;
    for (i = 0; i < count; i++)
    {
        lost[i] = (u8)bad;
        if (bad)
            bad = (cal_rng_u32(rng) % CAL_PPM) >= r_ppm;
        else
            bad = (cal_rng_u32(rng) % CAL_PPM) < q_ppm;
    }
}

static void run_ge(struct level *L, long trials)
{
    static const u32 ppm_list[] = {50000,  100000, 150000, 200000, 250000,
                                   300000, 350000, 400000, 450000, 500000,
                                   550000};
    static const u32 bursts[3] = {2, 4, 8};
    u32 bi, pi;

    for (bi = 0; bi < 3; bi++)
    {
        for (pi = 0; pi < sizeof(ppm_list) / sizeof(ppm_list[0]); pi++)
        {
            struct cal_rng rng;
            long t, grp = 0, grp_fail = 0, sup = 0, sup_fail = 0;
            u32 ppm = ppm_list[pi];
            char ps[24], bbuf[24];
            u32 g, i;

            cal_rng_seed(&rng, CAL_TAG_GE, L->pct, ppm, bursts[bi]);
            for (t = 0; t < trials; t++)
            {
                int any = 0;

                for (g = 0; g < CAL_GROUPS; g++)
                {
                    u8 ld[CAL_MAX_SHARES], lp[CAL_MAX_SHARES];
                    u8 dp[CAL_MAX_SHARES], pp[CAL_MAX_SHARES];

                    ge_losses(&rng, ppm, bursts[bi], ld, L->n);
                    ge_losses(&rng, ppm, bursts[bi], lp, L->m);
                    for (i = 0; i < L->n; i++)
                        dp[i] = (u8)!ld[i];
                    for (i = 0; i < L->m; i++)
                        pp[i] = (u8)!lp[i];
                    grp++;
                    if (!level_group_ok(L, g, dp, pp))
                    {
                        grp_fail++;
                        any = 1;
                    }
                }
                sup++;
                if (any)
                    sup_fail++;
            }
            p_name(ps, sizeof(ps), ppm);
            snprintf(bbuf, sizeof(bbuf), "B=%u", bursts[bi]);
            cal_row("ge", L, ps, bbuf, "group_fail_rate",
                    (double)grp_fail / (double)grp, grp_fail, grp, 0, 0,
                    "per-group Gilbert-Elliott");
            cal_row("ge", L, ps, bbuf, "super_fail_rate",
                    (double)sup_fail / (double)sup, sup_fail, sup, 0, 0,
                    "any of 8 groups");
        }
    }
}

/* ------------------------------------------------------------------ *
 * k dead carriers of the 256; carrier c is group c/32 share c%32.
 * ------------------------------------------------------------------ */

static void pick_dead(struct cal_rng *rng, u32 k, u8 *dead)
{
    u16 perm[CAL_CARRIERS];
    u32 i;

    for (i = 0; i < CAL_CARRIERS; i++)
        perm[i] = (u16)i;
    for (i = 0; i < k && i < CAL_CARRIERS; i++)
    {
        u32 j = i + cal_rng_u32(rng) % (CAL_CARRIERS - i);
        u16 t = perm[i];

        perm[i] = perm[j];
        perm[j] = t;
    }
    memset(dead, 0, CAL_CARRIERS);
    for (i = 0; i < k && i < CAL_CARRIERS; i++)
        dead[perm[i]] = 1;
}

static void carrier_point(struct level *L, u32 k, int contiguous,
                          long trials, struct cal_point *out)
{
    struct cal_rng rng;
    long t, grp = 0, grp_fail = 0, sup = 0, sup_fail = 0;
    u8 dead[CAL_CARRIERS];
    u32 g, i;

    cal_rng_seed(&rng, CAL_TAG_CARRIER, L->pct, k, (u32)contiguous);
    for (t = 0; t < trials; t++)
    {
        int any = 0;

        if (contiguous)
        {
            memset(dead, 0, sizeof(dead));
            for (i = 0; i < k && i < CAL_CARRIERS; i++)
                dead[i] = 1;
        }
        else
        {
            pick_dead(&rng, k, dead);
        }
        for (g = 0; g < CAL_GROUPS; g++)
        {
            u8 dp[CAL_MAX_SHARES], pp[CAL_MAX_SHARES];

            for (i = 0; i < L->n; i++)
                dp[i] = (u8)!dead[g * CAL_SHARES + i];
            for (i = 0; i < L->m; i++)
                pp[i] = (u8)!dead[g * CAL_SHARES + L->n + i];
            grp++;
            if (!level_group_ok(L, g, dp, pp))
            {
                grp_fail++;
                any = 1;
            }
        }
        sup++;
        if (any)
            sup_fail++;
    }
    out->ppm = k;
    out->sup = sup;
    out->sup_fail = sup_fail;
    out->grp = grp;
    out->grp_fail = grp_fail;
}

static void run_carrier(struct level *L, long trials)
{
    static const u32 ks[] = {0,  2,  4,  6,  8,  10, 12, 14, 16, 20,
                             24, 28, 32, 36, 40, 44, 48, 52, 56, 60,
                             64, 68, 72, 76, 80, 88, 96};
    static const u32 cks[] = {0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 32, 48, 64};
    u32 i;

    for (i = 0; i < sizeof(ks) / sizeof(ks[0]); i++)
    {
        struct cal_point p;
        char kb[24];
        double lo, hi;

        carrier_point(L, ks[i], 0, trials, &p);
        snprintf(kb, sizeof(kb), "k=%u", ks[i]);
        cal_wilson(p.grp_fail, p.grp, &lo, &hi);
        cal_row("carrier", L, kb, "random", "group_fail_rate",
                (double)p.grp_fail / (double)p.grp, p.grp_fail, p.grp, lo,
                hi, "k of 256 carriers dead, uniform subset");
        cal_wilson(p.sup_fail, p.sup, &lo, &hi);
        cal_row("carrier", L, kb, "random", "super_fail_rate",
                (double)p.sup_fail / (double)p.sup, p.sup_fail, p.sup, lo,
                hi, "any of 8 groups");
    }
    for (i = 0; i < sizeof(cks) / sizeof(cks[0]); i++)
    {
        struct cal_point p;
        char kb[24];
        double lo, hi;

        carrier_point(L, cks[i], 1, trials, &p);
        snprintf(kb, sizeof(kb), "k=%u", cks[i]);
        cal_wilson(p.grp_fail, p.grp, &lo, &hi);
        cal_row("carrier", L, kb, "contig", "group_fail_rate",
                (double)p.grp_fail / (double)p.grp, p.grp_fail, p.grp, lo,
                hi, "carriers 0..k-1 dead");
        cal_wilson(p.sup_fail, p.sup, &lo, &hi);
        cal_row("carrier", L, kb, "contig", "super_fail_rate",
                (double)p.sup_fail / (double)p.sup, p.sup_fail, p.sup, lo,
                hi, "any of 8 groups");
    }
}

/* ------------------------------------------------------------------ *
 * cpu: encode and decode cost per group.
 * ------------------------------------------------------------------ */

static void run_cpu(struct level *L, long trials)
{
    static const u32 ppm_list[] = {0,     50000,  100000, 150000, 200000,
                                   250000, 300000, 350000, 400000, 450000,
                                   500000};
    struct cal_rng rng;
    double t0;
    long n;
    u32 g, i, pi;
    char ps[24], note[64];

    cal_rng_seed(&rng, CAL_TAG_CPU, L->pct, 0, 0);
    for (g = 0; g < CAL_GROUPS; g++)
    {
        for (i = 0; i < L->n; i++)
            for (u32 j = 0; j < TBT_FEC_S; j++)
                L->dat[g][i][j] = (u8)cal_rng_u32(&rng);
    }
    t0 = cal_now();
    for (n = 0; n < trials; n++)
        for (g = 0; g < CAL_GROUPS; g++)
            if (tbt_sclc_uplink_encode(L->u, (u16)g, L->dat[g], L->par[g]))
                exit(2);
    {
        double us = (cal_now() - t0) * 1e6 / ((double)trials * CAL_GROUPS);

        cal_row("cpu", L, "encode", "", "us_per_group", us, -1, -1, 0, 0,
                "");
        cal_row("cpu", L, "encode", "", "mbps",
                (double)L->n * TBT_FEC_S / us, -1, -1, 0, 0,
                "payload MB/s, one group");
    }
    /* Decode timing uses the all-zero codeword so a failed attempt cannot
     * leave an invalid partial codeword in a present share. */
    for (g = 0; g < CAL_GROUPS; g++)
    {
        for (i = 0; i < L->n; i++)
            memset(L->dat[g][i], 0, TBT_FEC_S);
        for (i = 0; i < L->m; i++)
            memset(L->par[g][i], 0, TBT_FEC_S);
    }
    for (pi = 0; pi < sizeof(ppm_list) / sizeof(ppm_list[0]); pi++)
    {
        u32 ppm = ppm_list[pi];

        cal_rng_seed(&rng, CAL_TAG_CPU, L->pct, ppm, 1);
        t0 = cal_now();
        for (n = 0; n < trials; n++)
            for (g = 0; g < CAL_GROUPS; g++)
            {
                u8 dp[CAL_MAX_SHARES], pp[CAL_MAX_SHARES];

                for (i = 0; i < L->n; i++)
                    dp[i] = (u8)!cal_loss(&rng, ppm);
                for (i = 0; i < L->m; i++)
                    pp[i] = (u8)!cal_loss(&rng, ppm);
                (void)tbt_sclc_uplink_decode(L->u, (u16)g, L->dat[g],
                                             L->par[g], dp, pp);
            }
        {
            double us =
                (cal_now() - t0) * 1e6 / ((double)trials * CAL_GROUPS);

            p_name(ps, sizeof(ps), ppm);
            snprintf(note, sizeof(note), "i.i.d. mask, production max_core");
            cal_row("cpu", L, ps, "decode", "us_per_group", us, -1, -1, 0, 0,
                    note);
            cal_row("cpu", L, ps, "decode", "mbps",
                    (double)L->n * TBT_FEC_S / us, -1, -1, 0, 0,
                    "payload MB/s, one group");
        }
    }
}

/* ------------------------------------------------------------------ *
 * selftest: byte-exact round trip, and the zero-buffer scan invariant.
 * ------------------------------------------------------------------ */

static void run_selftest(struct level *L)
{
    struct cal_rng rng;
    u8 *orig_d[CAL_MAX_SHARES];
    u8 *orig_p[CAL_MAX_SHARES];
    u8 *zero_d[CAL_MAX_SHARES];
    u8 *zero_p[CAL_MAX_SHARES];
    struct tbt_sclc_uplink *zu = NULL;
    struct tbt_sclc_params zp;
    long checks = 0, bad_bytes = 0, zero_diff = 0;
    u32 i, g;

    memset(orig_d, 0, sizeof(orig_d));
    memset(orig_p, 0, sizeof(orig_p));
    memset(zero_d, 0, sizeof(zero_d));
    memset(zero_p, 0, sizeof(zero_p));
    for (i = 0; i < L->n; i++)
    {
        orig_d[i] = malloc(TBT_FEC_S);
        zero_d[i] = calloc(1, TBT_FEC_S);
    }
    for (i = 0; i < L->m; i++)
    {
        orig_p[i] = malloc(TBT_FEC_S);
        zero_p[i] = calloc(1, TBT_FEC_S);
    }
    memset(&zp, 0, sizeof(zp));
    zp.n = L->n;
    zp.m = L->m;
    zp.block = TBT_FEC_SCLC_BLOCK_UP;
    zp.L = TBT_FEC_SCLC_L_UP;
    zp.rate_num = L->rate_num;
    zp.rate_den = 256;
    zp.max_core = (u16)cal_core;
    zp.mapping = TBT_SCLC_MAP_BITPLANE64;
    zp.seed = CAL_BASE_SEED;
    zu = tbt_sclc_uplink_new(1, &zp);

    cal_rng_seed(&rng, 0x77, L->pct, 0, 0);
    for (g = 0; g < CAL_GROUPS && zu; g++)
    {
        static const u32 ppm_list[3] = {100000, 300000, 500000};
        u32 pi;

        for (i = 0; i < L->n; i++)
            for (u32 j = 0; j < TBT_FEC_S; j++)
                L->dat[g][i][j] = (u8)cal_rng_u32(&rng);
        for (i = 0; i < L->n; i++)
            memcpy(orig_d[i], L->dat[g][i], TBT_FEC_S);
        if (tbt_sclc_uplink_encode(L->u, (u16)g, L->dat[g], L->par[g]))
        {
            fprintf(stderr, "calib: selftest encode failed\n");
            exit(2);
        }
        for (i = 0; i < L->m; i++)
            memcpy(orig_p[i], L->par[g][i], TBT_FEC_S);

        for (pi = 0; pi < 3; pi++)
        {
            u32 round;

            for (round = 0; round < 40; round++)
            {
                u8 dp[CAL_MAX_SHARES], pp[CAL_MAX_SHARES];
                u8 zdp[CAL_MAX_SHARES], zpp[CAL_MAX_SHARES];
                int ok, zok = 0;

                for (i = 0; i < L->n; i++)
                {
                    dp[i] = (u8)!cal_loss(&rng, ppm_list[pi]);
                    zdp[i] = dp[i];
                }
                for (i = 0; i < L->m; i++)
                {
                    pp[i] = (u8)!cal_loss(&rng, ppm_list[pi]);
                    zpp[i] = pp[i];
                }
                for (i = 0; i < L->n; i++)
                    memcpy(L->dat[g][i], orig_d[i], TBT_FEC_S);
                for (i = 0; i < L->m; i++)
                    memcpy(L->par[g][i], orig_p[i], TBT_FEC_S);
                ok = level_group_ok(L, g, dp, pp);
                for (i = 0; i < L->n; i++)
                {
                    if (tbt_sclc_uplink_share_known(L->u, (u16)g,
                                                    (u16)i))
                    {
                        checks++;
                        if (memcmp(L->dat[g][i], orig_d[i], TBT_FEC_S) !=
                            0)
                            bad_bytes++;
                    }
                }
                if (g == 0)
                {
                    zok = tbt_sclc_uplink_decode(zu, 0, zero_d, zero_p, zdp,
                                                 zpp) == (int)L->n;
                    if (zok != ok)
                        zero_diff++;
                }
            }
        }
    }

    cal_row("selftest", L, "byte_exact", "", "checks", (double)checks, -1,
            -1, 0, 0, "known DATA shares compared with the original");
    cal_row("selftest", L, "byte_exact", "", "mismatches",
            (double)bad_bytes, bad_bytes, checks, 0, 0,
            "must be 0");
    cal_row("selftest", L, "zero_buffer", "", "outcome_diffs",
            (double)zero_diff, zero_diff, checks, 0, 0,
            "encoded vs all-zero buffers, same mask");
    cal_row("selftest", L, "summary", "", "result",
            (bad_bytes || zero_diff) ? 0.0 : 1.0, -1, -1, 0, 0,
            (bad_bytes || zero_diff) ? "FAIL" : "PASS");

    for (i = 0; i < L->n; i++)
    {
        free(orig_d[i]);
        free(zero_d[i]);
    }
    for (i = 0; i < L->m; i++)
    {
        free(orig_p[i]);
        free(zero_p[i]);
    }
    tbt_sclc_uplink_free(zu);
}

/* ------------------------------------------------------------------ *
 * construct: which n build inside a given group width.
 * ------------------------------------------------------------------ */

static void run_construct(void)
{
    static const unsigned widths[] = {16, 32, 64, 128};
    unsigned wi;

    for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++)
    {
        unsigned width = widths[wi];
        unsigned groups = CAL_CARRIERS / width;
        unsigned n;

        for (n = 1; n < width; n++)
        {
            struct tbt_sclc_params p;
            struct tbt_sclc_uplink *u;
            struct level tmp;
            char wbuf[24], nbuf[24], note[128];
            unsigned m = width - n;

            memset(&p, 0, sizeof(p));
            p.n = (u16)n;
            p.m = (u16)m;
            p.block = TBT_FEC_SCLC_BLOCK_UP;
            p.L = TBT_FEC_SCLC_L_UP;
            p.rate_num = (u16)(n * 256u / width);
            p.rate_den = 256;
            p.max_core = 256;
            p.mapping = TBT_SCLC_MAP_BITPLANE64;
            p.seed = CAL_BASE_SEED;
            u = tbt_sclc_uplink_new((u16)groups, &p);
            memset(&tmp, 0, sizeof(tmp));
            tmp.n = (u16)n;
            tmp.m = (u16)m;
            tmp.rate_ppm = n * CAL_PPM / width;
            snprintf(wbuf, sizeof(wbuf), "w=%u", width);
            snprintf(nbuf, sizeof(nbuf), "n=%u", n);
            if (!u)
            {
                cal_row("construct", &tmp, wbuf, nbuf, "build", 0, -1, -1,
                        0, 0, "codec build failed");
                continue;
            }
            snprintf(note, sizeof(note),
                     "codes=%u npar=%u nsym=%u rate=%.4f slack=%d",
                     tbt_sclc_uplink_codes(u), tbt_sclc_uplink_n_par(u),
                     tbt_sclc_uplink_n_data(u) + tbt_sclc_uplink_n_par(u),
                     (double)tbt_sclc_uplink_n_data(u) /
                         (double)(tbt_sclc_uplink_n_data(u) +
                                  tbt_sclc_uplink_n_par(u)),
                     (int)m * 16384 -
                         (int)tbt_sclc_uplink_codes(u) *
                             (int)tbt_sclc_uplink_n_par(u));
            cal_row("construct", &tmp, wbuf, nbuf, "build", 1, -1, -1, 0, 0,
                    note);
            tbt_sclc_uplink_free(u);
        }
    }
}

/* ------------------------------------------------------------------ *
 * analyze: closed forms for the share-erasure channel.
 *
 * A codeword has n_data systematic symbols and n_par checks.  A missing
 * data share erases n_data/n symbols, a missing parity share n_par/m.  The
 * MDS share threshold is therefore m_eff = floor(n_par*n/n_data): above it
 * even a perfect code cannot recover.  For a random GF(16) r x u matrix the
 * rank-deficiency probability is 1 - prod(1 - 16^(i-r)) ~ 16^-(r-u+1), so
 * in share units
 *
 *     P_fail(k) ~ 16^-((m_eff - k) * n_data/n + 1).
 *
 * The last MDS share is therefore lost (1/16), and a core larger than
 * max_core variables is skipped.  A whole erasure that fits the core is at
 * most C*32/N missing shares, N = n_data+n_par.
 * ------------------------------------------------------------------ */

static double rank_fail(double k_star, int k, int n, int n_data)
{
    double expo;

    if ((double)k > k_star)
        return 1.0;
    expo = (k_star - (double)k) * (double)n_data / (double)n + 1.0;
    if (expo > 300.0)
        return 0.0;
    return pow(16.0, -expo);
}

static void run_analyze(struct level *L)
{
    int n = (int)L->n;
    int m = (int)L->m;
    int n_data = (int)L->n_data;
    int n_par = (int)L->n_par;
    int nsym = (int)L->nsym;
    double k_star = (double)n_par * (double)n / (double)n_data;
    int m_eff = (int)k_star;
    int k_rank;
    int k_core = (int)((long long)cal_core * CAL_SHARES / nsym);
    char note[96];

    if (m_eff > m)
        m_eff = m;
    k_rank = m_eff;
    if (k_star == (double)m_eff && m_eff > 0)
        k_rank = m_eff - 1;
    cal_row("analyze", L, "m_eff", "mds", "shares", m_eff, -1, -1, 0, 0,
            "floor(n_par*n/n_data), perfect-code share threshold");
    cal_row("analyze", L, "delta_mds", "mds", "shares", m - m_eff, -1, -1, 0,
            0, "m - m_eff, parity rounding loss");
    snprintf(note, sizeof(note), "k*=%d*%d/%d=%.3f", n_par, n, n_data,
             k_star);
    cal_row("analyze", L, "k_star", "rank", "shares", k_star, -1, -1, 0, 0,
            note);
    snprintf(note, sizeof(note), "16^-((k*-k)*%d/%d+1)", n_data, n);
    cal_row("analyze", L, "p_fail_at_k_rank", "rank", "probability",
            rank_fail(k_star, k_rank, n, n_data), -1, -1, 0, 0, note);
    cal_row("analyze", L, "p_fail_at_k_rank_plus_1", "rank", "probability",
            rank_fail(k_star, k_rank + 1, n, n_data), -1, -1, 0, 0, note);
    cal_row("analyze", L, "delta_rank", "rank", "shares", m - k_rank, -1, -1,
            0, 0, "m - k_rank; one share lost vs nominal m");
    cal_row("analyze", L, "k_core", "core", "shares", k_core, -1, -1, 0, 0,
            "floor(core*32/nsym), erasure that fits the core");
    cal_row("analyze", L, "delta_core", "core", "shares", m - k_core, -1, -1,
            0, 0, "m - k_core, sufficient-recovery bound");
    cal_row("analyze", L, "code_rate_ppm", "shape", "value",
            (double)n_data * CAL_PPM / (double)nsym, -1, -1, 0, 0, "");
    cal_row("analyze", L, "share_rate_ppm", "shape", "value",
            (double)n * CAL_PPM / (double)CAL_SHARES, -1, -1, 0, 0,
            "n/(n+m)");
    cal_row("analyze", L, "ladder_ppm", "shape", "value", L->rate_ppm, -1,
            -1, 0, 0, "rate_ppm on the wire, not n/(n+m) for 30/60/70");
    {
        static const double pg[3] = {1e-4, 1e-3, 1e-2};
        int i;

        for (i = 0; i < 3; i++)
        {
            char pb[24];
            double indep = 1.0 - pow(1.0 - pg[i], (double)CAL_GROUPS);
            double uni = pg[i] * CAL_GROUPS > 1.0 ? 1.0 : pg[i] * CAL_GROUPS;

            snprintf(pb, sizeof(pb), "pg=%.0e", pg[i]);
            cal_row("analyze", L, pb, "combine", "super_independent", indep,
                    -1, -1, 0, 0, "1-(1-pg)^8");
            cal_row("analyze", L, pb, "combine", "super_union_bound", uni,
                    -1, -1, 0, 0, "min(1,8*pg), any correlation");
            cal_row("analyze", L, pb, "combine", "super_correlated", pg[i],
                    -1, -1, 0, 0, "all groups fail together");
        }
    }
}

/* ------------------------------------------------------------------ *
 * spot: the retained small measurement, not the stopped scan.
 * ------------------------------------------------------------------ */

static void run_spot(struct level *L, long trials)
{
    static const u32 ppm_list[] = {100000, 200000, 300000, 400000, 500000};
    u32 i;

    for (i = 0; i < sizeof(ppm_list) / sizeof(ppm_list[0]); i++)
    {
        struct cal_point p;
        char ps[24];
        double lo, hi;

        scan_point(L, ppm_list[i], trials, CAL_TAG_SCAN_COARSE, 0x55, &p);
        p_name(ps, sizeof(ps), ppm_list[i]);
        cal_wilson(p.grp_fail, p.grp, &lo, &hi);
        {
            char note[64];

            snprintf(note, sizeof(note), "retained spot check, core=%d",
                     cal_core);
            cal_row("spot", L, ps, "group", "group_fail_rate",
                    (double)p.grp_fail / (double)p.grp, p.grp_fail, p.grp,
                    lo, hi, note);
        }
        cal_wilson(p.sup_fail, p.sup, &lo, &hi);
        {
            char note[64];

            snprintf(note, sizeof(note), "any of 8 groups; core=%d",
                     cal_core);
            cal_row("spot", L, ps, "super", "super_fail_rate",
                    (double)p.sup_fail / (double)p.sup, p.sup_fail, p.sup,
                    lo, hi, note);
        }
    }
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *mode;
    int want = -1;
    long trials = -1;
    int header = 0;
    int i;

    if (argc < 2)
    {
        fprintf(stderr,
                "usage: %s <construct|analyze|spot|meta|scan|wake|ge|carrier|"
                "cpu|selftest|all>\n"
                "          [--level 25|30|40|50|60|70] [--trials N]\n"
                "          [--core N] [--header]\n",
                argv[0]);
        return 1;
    }
    mode = argv[1];
    for (i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--level") && i + 1 < argc)
            want = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc)
            trials = atol(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc)
            cal_core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--header"))
            header = 1;
        else
        {
            fprintf(stderr, "calib: bad argument %s\n", argv[i]);
            return 1;
        }
    }
    if (header || !strcmp(mode, "header"))
        cal_header();
    if (!strcmp(mode, "header"))
        return 0;
    if (!strcmp(mode, "construct"))
    {
        run_construct();
        return 0;
    }

    {
    int ran = 0;
    for (i = 0; i < TBT_FEC_ADAPT_SCLC_LEVELS; i++)
    {
        const struct tbt_fec_adapt_sclc_rate *r =
            tbt_fec_adapt_sclc_rate_at((u8)i);
        struct level L;

        if (!r)
            continue;
        if (want >= 0 && (u8)want != r->pct)
            continue;
        if (level_open(&L, r->pct) != 0)
        {
            level_close(&L);
            return 1;
        }
        if (!strcmp(mode, "meta") || !strcmp(mode, "all"))
            run_meta(&L);
        if (!strcmp(mode, "selftest") || !strcmp(mode, "all"))
            run_selftest(&L);
        if (!strcmp(mode, "scan") || !strcmp(mode, "all"))
            run_scan(&L, trials > 0 ? trials : CAL_COARSE_TRIALS,
                     trials > 0 ? trials * 5 : CAL_DENSE_TRIALS);
        if (!strcmp(mode, "wake") || !strcmp(mode, "all"))
            run_wake(&L, trials > 0 ? trials : CAL_WAKE_TRIALS);
        if (!strcmp(mode, "ge") || !strcmp(mode, "all"))
            run_ge(&L, trials > 0 ? trials : CAL_GE_TRIALS);
        if (!strcmp(mode, "carrier") || !strcmp(mode, "all"))
            run_carrier(&L, trials > 0 ? trials : CAL_CARRIER_TRIALS);
        if (!strcmp(mode, "cpu") || !strcmp(mode, "all"))
            run_cpu(&L, trials > 0 ? trials : CAL_CPU_TRIALS);
        if (!strcmp(mode, "analyze") || !strcmp(mode, "all"))
            run_analyze(&L);
        if (!strcmp(mode, "spot") || !strcmp(mode, "all"))
            run_spot(&L, trials > 0 ? trials : 1000L);
        level_close(&L);
        ran++;
    }
    if (want >= 0 && !ran)
    {
        fprintf(stderr, "calib: level %d is not on the ladder\n", want);
        return 1;
    }
    }
    return 0;
}
