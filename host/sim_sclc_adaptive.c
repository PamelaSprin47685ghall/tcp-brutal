/* sim_sclc_adaptive.c - closed-loop downlink SC-LDPC adaptation over a BEC.
 *
 * This is a host simulation, not a unit test. It drives the real
 * tbt_fec_adapt_sclc estimator and rate controller with a 120 s
 * time-varying binary erasure channel, and it repairs every block with the
 * real tbt_fec_scLc downlink codec at the shape the controller selected.
 * One FEC block is always 256 shares of 8192 bytes: the codec's wire block
 * never changes, only the n/m split (rate 25..70 percent) does.
 *
 * The trace is 0% -> 8% -> 22% -> 38% -> 2% DATA-share loss. The channel
 * erases DATA and parity shares alike, which is what the receiver sees. The
 * estimator is fed the DATA-share miss mask and the codec's residual
 * failures; the controller sees its normal 4 s window / 1 s fast window and
 * evaluates on its own 250 ms tick. A committed suggestion swaps the codec
 * for the suggested n/m, exactly as the data plane does after FEC_CFG.
 *
 * The interesting number is the wire/app share ratio. At rate 50 the codec
 * sends 256 wire shares for 128 payload shares (2.00). At rate 70 it sends
 * 256 for 178 (1.44), so the same payload costs 28% fewer wire shares. The
 * simulation prints that ratio per phase, the switch log, the reversal and
 * ping-pong counts, and the share recovery rate.
 */
#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_sclc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARE TBT_SCLC_SHARE
#define SHARES TBT_FEC_MAX_SHARES
#define LEVELS TBT_FEC_ADAPT_SCLC_LEVELS
#define PPM 1000000u
#define SEC_NS 1000000000ull
#define BLOCKS_PER_SEC 8u
#define BLOCK_NS (SEC_NS / BLOCKS_PER_SEC)
#define SIM_NS (120ull * SEC_NS)
#define PHASES 5
#define LOW_LOSS_PPM 80000u
#define RATE50_RATIO 2.0
#define REVERSE_WIN_NS (10ull * SEC_NS)

struct trace_phase
{
    u64 end_ns;
    u32 loss_ppm;
    const char *label;
};

static const struct trace_phase trace[PHASES] = {
    {20ull * SEC_NS, 0u, "0%"},
    {40ull * SEC_NS, 80000u, "8%"},
    {60ull * SEC_NS, 220000u, "22%"},
    {80ull * SEC_NS, 380000u, "38%"},
    {120ull * SEC_NS, 20000u, "2%"},
};

static u64 payload_rng = 0x243F6A8885A308D3ull;
static u64 channel_rng = 0x9E3779B97F4A7C15ull;

static u64 rnd64(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void fill_random(u8 *p, size_t len, u64 *state)
{
    size_t off = 0;

    while (off + 8 <= len)
    {
        u64 v = rnd64(state);

        memcpy(p + off, &v, 8);
        off += 8;
    }
    if (off < len)
    {
        u64 v = rnd64(state);

        memcpy(p + off, &v, len - off);
    }
}

struct codec
{
    void *ctx;
    u16 n;
    u16 m;
    u8 *data;
    u8 *parity;
    u8 *truth;
    u8 **dp;
    u8 **pp;
    u8 *dpres;
    u8 *ppres;
};

static void codec_close(struct codec *c)
{
    if (c->ctx)
        tbt_fec_ops_sclc.free(c->ctx);
    free(c->data);
    free(c->parity);
    free(c->truth);
    free(c->dp);
    free(c->pp);
    free(c->dpres);
    free(c->ppres);
    memset(c, 0, sizeof(*c));
}

static int codec_open(struct codec *c, u16 n, u16 m, u64 nonce)
{
    int i;

    codec_close(c);
    c->n = n;
    c->m = m;
    c->data = calloc((size_t)n, SHARE);
    c->parity = calloc((size_t)m, SHARE);
    c->truth = malloc((size_t)n * SHARE);
    c->dp = calloc(n, sizeof(*c->dp));
    c->pp = calloc(m, sizeof(*c->pp));
    c->dpres = calloc(n, 1);
    c->ppres = calloc(m, 1);
    if (!c->data || !c->parity || !c->truth || !c->dp || !c->pp ||
        !c->dpres || !c->ppres)
        goto fail;
    for (i = 0; i < (int)n; i++)
    {
        c->dp[i] = c->data + (size_t)i * SHARE;
        fill_random(c->dp[i], SHARE, &payload_rng);
    }
    for (i = 0; i < (int)m; i++)
        c->pp[i] = c->parity + (size_t)i * SHARE;
    memcpy(c->truth, c->data, (size_t)n * SHARE);
    c->ctx = tbt_fec_ops_sclc.new(n, m,
                                  (u16)((u32)n * 1000u / (u32)(n + m)),
                                  TBT_SCLC_BLOCK, SHARE, 0, nonce);
    if (!c->ctx)
        goto fail;
    return 0;

fail:
    codec_close(c);
    return -1;
}

/* Encode one block, erase shares at `p_ppm`, repair with the real codec, and
 * report the DATA shares that were lost, recovered and left unrecovered. The
 * miss mask is what the estimator sees: every lost DATA share, whether or not
 * the codec repaired it. */
static int codec_block(struct codec *c, u32 p_ppm,
                       u8 miss[TBT_FEC_BITMAP_BYTES], u32 *missing,
                       u32 *recovered, u32 *failed)
{
    int i;
    int decode_needed = 0;

    *missing = 0;
    *recovered = 0;
    *failed = 0;
    memset(miss, 0, TBT_FEC_BITMAP_BYTES);
    for (i = 0; i < (int)c->n; i++)
    {
        c->dpres[i] = p_ppm == 0u ||
                      (rnd64(&channel_rng) % PPM) >= p_ppm;
        if (!c->dpres[i])
        {
            tbt_fec_adapt_bitmap_set(miss, (u16)i);
            decode_needed = 1;
        }
    }
    for (i = 0; i < (int)c->m; i++)
    {
        c->ppres[i] = p_ppm == 0u ||
                      (rnd64(&channel_rng) % PPM) >= p_ppm;
        if (!c->ppres[i])
            decode_needed = 1;
    }
    if (tbt_fec_ops_sclc.encode(c->ctx, c->dp, c->n, c->pp))
        return -1;
    if (!decode_needed)
        return 0;
    (void)tbt_fec_ops_sclc.decode(c->ctx, c->dp, c->pp, c->dpres, c->ppres,
                                  c->n);
    for (i = 0; i < (int)c->n; i++)
    {
        if (c->dpres[i])
            continue;
        (*missing)++;
        if (tbt_fec_ops_sclc.is_known(c->ctx, (u32)i))
        {
            if (memcmp(c->data + (size_t)i * SHARE,
                       c->truth + (size_t)i * SHARE, SHARE))
                return -1;
            (*recovered)++;
        }
        else
            (*failed)++;
        memcpy(c->data + (size_t)i * SHARE, c->truth + (size_t)i * SHARE,
               SHARE);
    }
    return 0;
}

/* One encode/erase/repair roundtrip per ladder point. This proves the six
 * shapes build and that the codec repairs a DATA and a parity share before
 * the trace starts using them. */
static int ladder_smoke(void)
{
    int i;
    int bad = 0;

    for (i = 0; i < LEVELS; i++)
    {
        const struct tbt_fec_adapt_sclc_rate *r =
            tbt_fec_adapt_sclc_rate_at((u8)i);
        struct tbt_fec_adapt_sclc_shape s;
        struct codec c;
        int ok;

        memset(&c, 0, sizeof(c));
        ok = tbt_fec_adapt_sclc_shape(r->pct, SHARES, 1, &s) == 0;
        if (ok)
            ok = codec_open(&c, s.n, s.m, 0x100 + (u64)i) == 0;
        if (ok)
            ok = tbt_fec_ops_sclc.encode(c.ctx, c.dp, c.n, c.pp) == 0;
        if (ok)
        {
            memset(c.dpres, 1, c.n);
            memset(c.ppres, 1, c.m);
            c.dpres[0] = 0;
            c.ppres[0] = 0;
            (void)tbt_fec_ops_sclc.decode(c.ctx, c.dp, c.pp, c.dpres,
                                          c.ppres, c.n);
            ok = tbt_fec_ops_sclc.is_known(c.ctx, 0) &&
                 !memcmp(c.data, c.truth, SHARE);
        }
        printf("ladder %2u%% n=%3u m=%3u: %s\n", (unsigned)r->pct,
               ok ? (unsigned)s.n : 0u, ok ? (unsigned)s.m : 0u,
               ok ? "roundtrip OK" : "FAIL");
        if (!ok)
            bad++;
        codec_close(&c);
    }
    return bad ? -1 : 0;
}

static const char *reason_name(u8 reason)
{
    switch (reason)
    {
    case TBT_FEC_ADAPT_SCLC_R_NONE:
        return "none";
    case TBT_FEC_ADAPT_SCLC_R_DISABLED:
        return "disabled";
    case TBT_FEC_ADAPT_SCLC_R_BUSY:
        return "busy";
    case TBT_FEC_ADAPT_SCLC_R_HOLD:
        return "hold";
    case TBT_FEC_ADAPT_SCLC_R_PAUSED:
        return "paused";
    case TBT_FEC_ADAPT_SCLC_R_WAIT:
        return "wait";
    case TBT_FEC_ADAPT_SCLC_R_STEADY:
        return "steady";
    case TBT_FEC_ADAPT_SCLC_R_FAST_START:
        return "fast_start";
    case TBT_FEC_ADAPT_SCLC_R_UP:
        return "up";
    case TBT_FEC_ADAPT_SCLC_R_DOWN:
        return "down";
    case TBT_FEC_ADAPT_SCLC_R_BURST:
        return "burst";
    case TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN:
        return "fail_down";
    case TBT_FEC_ADAPT_SCLC_R_PROBE_UP:
        return "probe_up";
    case TBT_FEC_ADAPT_SCLC_R_PROBE_REVERT:
        return "probe_revert";
    case TBT_FEC_ADAPT_SCLC_R_PROBE_ABORT:
        return "probe_abort";
    case TBT_FEC_ADAPT_SCLC_R_PROBE_COMMIT:
        return "probe_commit";
    case TBT_FEC_ADAPT_SCLC_R_ROLLBACK:
        return "rollback";
    default:
        return "?";
    }
}

struct sw_event
{
    u64 t_ns;
    u8 from;
    u8 to;
    u8 reason;
};

struct phase_stat
{
    u64 wire;
    u64 app;
    u64 lost;
    u64 recovered;
    u32 blocks;
    u32 level_blocks[LEVELS];
    u32 p_end_ppm;
};

struct level_stat
{
    u64 wire;
    u64 app;
    u64 lost;
    u64 recovered;
    u32 blocks;
};

static const struct trace_phase *phase_at(u64 now_ns)
{
    unsigned i;

    for (i = 0; i < PHASES; i++)
        if (now_ns < trace[i].end_ns)
            return &trace[i];
    return &trace[PHASES - 1];
}

static unsigned phase_index(u64 now_ns)
{
    unsigned i;

    for (i = 0; i < PHASES; i++)
        if (now_ns < trace[i].end_ns)
            return i;
    return PHASES - 1;
}

static u64 phase_start_ns(unsigned ph)
{
    return ph ? trace[ph - 1].end_ns : 0ull;
}

static void mix_string(const struct phase_stat *s, char *buf, size_t len)
{
    size_t off = 0;
    int i;

    buf[0] = '\0';
    for (i = LEVELS - 1; i >= 0; i--)
    {
        const struct tbt_fec_adapt_sclc_rate *r;
        int w;

        if (!s->level_blocks[i])
            continue;
        r = tbt_fec_adapt_sclc_rate_at((u8)i);
        w = snprintf(buf + off, len - off, "%s%u%%:%u",
                     off ? " " : "", (unsigned)r->pct,
                     (unsigned)s->level_blocks[i]);
        if (w > 0)
            off += (size_t)w;
        if (off >= len)
            break;
    }
}

static double delivered_ratio(u64 wire, u64 app, u64 lost, u64 recovered)
{
    u64 delivered = app - (lost - recovered);

    return delivered ? (double)wire / (double)delivered : 0.0;
}

int main(void)
{
    struct tbt_fec_adapt_sclc_est est;
    struct tbt_fec_adapt_sclc_ctl ctl;
    struct tbt_fec_adapt_sclc_shape initial;
    struct phase_stat ps[PHASES];
    struct level_stat low70;
    struct sw_event sw[64];
    unsigned nsw = 0;
    unsigned reversals = 0;
    unsigned pingpong = 0;
    int last_dir = 0;
    u64 last_switch_ns = 0;
    u64 now_ns;
    u8 alive[TBT_FEC_BITMAP_BYTES];
    u8 idx;
    u8 mask;
    struct codec c;
    u64 total_wire = 0;
    u64 total_app = 0;
    u64 total_lost = 0;
    u64 total_recovered = 0;
    int visited[LEVELS] = {0};
    unsigned i;

    printf("sim_sclc_adaptive: 120 s BEC trace, %u blocks/s, %u-share "
           "downlink blocks\n", BLOCKS_PER_SEC, (unsigned)SHARES);
    printf("trace:");
    for (i = 0; i < PHASES; i++)
        printf(" %s", trace[i].label);
    printf("\n\n");
    if (ladder_smoke())
    {
        fprintf(stderr, "sim_sclc_adaptive: ladder roundtrip failed\n");
        return 1;
    }

    memset(ps, 0, sizeof(ps));
    memset(&low70, 0, sizeof(low70));
    memset(&c, 0, sizeof(c));
    memset(alive, 0xff, sizeof(alive));
    tbt_fec_adapt_sclc_est_init(&est, SHARES);
    tbt_fec_adapt_sclc_est_carriers(&est, SHARES, 0, SHARES);
    if (tbt_fec_adapt_sclc_init_plan(128, 128, 1, TBT_FEC_SCLC_DOWN, &idx,
                                     &mask) ||
        tbt_fec_adapt_sclc_ctl_init(&ctl, true, 50, SHARES, 1) ||
        tbt_fec_adapt_sclc_shape(50, SHARES, 1, &initial) ||
        codec_open(&c, initial.n, initial.m, 0x200))
    {
        fprintf(stderr, "sim_sclc_adaptive: initial rate 50 does not build\n");
        return 1;
    }
    tbt_fec_adapt_sclc_ctl_set_rate_mask(&ctl, mask);
    tbt_fec_adapt_sclc_ctl_set_epoch(&ctl, 1);

    for (now_ns = 0; now_ns < SIM_NS; now_ns += BLOCK_NS)
    {
        const struct trace_phase *tp = phase_at(now_ns);
        unsigned ph = phase_index(now_ns);
        struct tbt_fec_adapt_sclc_est_out so;
        struct tbt_fec_adapt_sclc_out out;
        u8 miss[TBT_FEC_BITMAP_BYTES];
        u32 missing, recovered, failed;
        u8 level = ctl.cur;

        if (codec_block(&c, tp->loss_ppm, miss, &missing, &recovered,
                        &failed))
        {
            fprintf(stderr, "sim_sclc_adaptive: codec failure at %.3f s\n",
                    (double)now_ns / (double)SEC_NS);
            return 1;
        }
        tbt_fec_adapt_sclc_est_carriers(&est, SHARES, 0, SHARES);
        tbt_fec_adapt_sclc_est_block(&est, now_ns, 0, c.n, miss, alive);
        tbt_fec_adapt_sclc_est_super(&est, now_ns);
        tbt_fec_adapt_sclc_est_fail(&est, now_ns, failed, c.n);
        if (failed)
            tbt_fec_adapt_sclc_est_codec_fail(&est, now_ns, failed);
        tbt_fec_adapt_sclc_est_sample(&est, now_ns, &so);

        ps[ph].wire += SHARES;
        ps[ph].app += c.n;
        ps[ph].lost += missing;
        ps[ph].recovered += recovered;
        ps[ph].blocks++;
        ps[ph].level_blocks[level]++;
        ps[ph].p_end_ppm = so.p_ppm;
        if (level == LEVELS - 1 && tp->loss_ppm <= LOW_LOSS_PPM)
        {
            low70.wire += SHARES;
            low70.app += c.n;
            low70.lost += missing;
            low70.recovered += recovered;
            low70.blocks++;
        }
        visited[level] = 1;

        tbt_fec_adapt_sclc_ctl_eval(&ctl, &so, now_ns, &out);
        if (out.switch_now)
        {
            int dir = out.to > out.from ? 1 : -1;

            if (nsw == sizeof(sw) / sizeof(sw[0]))
            {
                fprintf(stderr, "sim_sclc_adaptive: switch log overflow\n");
                return 1;
            }
            sw[nsw].t_ns = now_ns;
            sw[nsw].from = out.from;
            sw[nsw].to = out.to;
            sw[nsw].reason = out.reason;
            nsw++;
            if (last_dir && dir != last_dir)
            {
                reversals++;
                if (now_ns - last_switch_ns < REVERSE_WIN_NS)
                    pingpong++;
            }
            last_dir = dir;
            last_switch_ns = now_ns;
            if (codec_open(&c, out.shape.n, out.shape.m, 0x200 + nsw))
            {
                fprintf(stderr, "sim_sclc_adaptive: switched shape does not "
                                "build\n");
                return 1;
            }
            tbt_fec_adapt_sclc_ctl_applied(&ctl, now_ns);
        }
    }

    printf("\nphase       loss   p_end   blocks  rate mix                 "
           "wire/app  vs50      lost   recov  recovery\n");
    for (i = 0; i < PHASES; i++)
    {
        const struct phase_stat *s = &ps[i];
        char mix[128];
        double ratio;
        double save;
        double recovery;

        mix_string(s, mix, sizeof(mix));
        ratio = delivered_ratio(s->wire, s->app, s->lost, s->recovered);
        save = (1.0 - ratio / RATE50_RATIO) * 100.0;
        recovery = s->lost ? (double)s->recovered / (double)s->lost : 0.0;
        printf("%3u-%3us  %5.1f%%  %5.1f%%  %6u  %-22s  %7.4f  %+6.1f%%  "
               "%6" PRIu64 "  %6" PRIu64 "  ",
               (unsigned)(phase_start_ns(i) / SEC_NS),
               (unsigned)(trace[i].end_ns / SEC_NS),
               (double)trace[i].loss_ppm / 10000.0,
               (double)s->p_end_ppm / 10000.0, s->blocks, mix, ratio, save,
               s->lost, s->recovered);
        if (s->lost)
            printf("%5.1f%%\n", recovery * 100.0);
        else
            printf("    -\n");
        total_wire += s->wire;
        total_app += s->app;
        total_lost += s->lost;
        total_recovered += s->recovered;
    }

    {
        double ratio = delivered_ratio(total_wire, total_app, total_lost,
                                       total_recovered);
        double save = (1.0 - ratio / RATE50_RATIO) * 100.0;
        double recovery = total_lost ? (double)total_recovered /
                                           (double)total_lost
                                     : 0.0;

        printf("\ntotal             blocks=%u  switches=%u  reversals=%u  "
               "ping-pong=%u\n", (unsigned)(SIM_NS / BLOCK_NS), nsw,
               reversals, pingpong);
        printf("wire/app total %.4f vs %.4f at rate 50 (save %+.1f%%), "
               "share recovery %.2f%%\n", ratio, RATE50_RATIO, save,
               recovery * 100.0);
    }

    {
        const struct level_stat *s = &low70;
        double ratio = delivered_ratio(s->wire, s->app, s->lost,
                                       s->recovered);
        double save = (1.0 - ratio / RATE50_RATIO) * 100.0;
        double recovery = s->lost ? (double)s->recovered / (double)s->lost
                                  : 0.0;

        printf("low-loss rate 70: %u blocks, wire/app %.4f (nominal %.4f) "
               "vs %.4f at rate 50, save %.1f%% of wire shares, recovery "
               "%.2f%%\n", s->blocks, ratio,
               (double)SHARES / (double)178.0, RATE50_RATIO, save,
               recovery * 100.0);
    }

    printf("\nswitch log:\n");
    for (i = 0; i < nsw; i++)
    {
        const struct tbt_fec_adapt_sclc_rate *from =
            tbt_fec_adapt_sclc_rate_at(sw[i].from);
        const struct tbt_fec_adapt_sclc_rate *to =
            tbt_fec_adapt_sclc_rate_at(sw[i].to);

        printf("  #%u  %7.3f s  %2u%% -> %2u%%  %s\n", i + 1,
               (double)sw[i].t_ns / (double)SEC_NS, (unsigned)from->pct,
               (unsigned)to->pct, reason_name(sw[i].reason));
    }

    {
        int bad = 0;

        if (!visited[2] || !visited[3] || !visited[5])
        {
            printf("FAIL: the trace did not visit rates 40/50/70\n");
            bad = 1;
        }
        if (nsw < 2)
        {
            printf("FAIL: controller made %u switch(es)\n", nsw);
            bad = 1;
        }
        if (pingpong)
        {
            printf("FAIL: %u reversal(s) inside the 10 s window\n", pingpong);
            bad = 1;
        }
        {
            const struct level_stat *s = &low70;
            double ratio = delivered_ratio(s->wire, s->app, s->lost,
                                           s->recovered);
            double save = (1.0 - ratio / RATE50_RATIO) * 100.0;

            if (!s->blocks || ratio > 1.45 || save < 27.0)
            {
                printf("FAIL: rate 70 wire/app %.4f (save %.1f%%)\n", ratio,
                       save);
                bad = 1;
            }
            if (total_lost && (double)total_recovered /
                                      (double)total_lost < 0.98)
            {
                printf("FAIL: overall recovery below 98%%\n");
                bad = 1;
            }
        }
        if (bad)
            return 1;
    }
    printf("\nsim_sclc_adaptive: OK (no ping-pong, all six ladder points, "
           "rate-70 wire/app <= 1.45)\n");
    return 0;
}
