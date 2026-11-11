/* tbt_fec_adapt_sclc.h - SC-LDPC continuous rate control.
 *
 * This file and tbt_fec_adapt_sclc.c compile twice: into the module and into
 * the host test. They allocate nothing, read no clock and touch no wire, so
 * the host test can drive every decision with virtual time.
 *
 * There is no rate ladder. One controller instance owns one direction. It
 * turns per-block repair evidence into a target number of parity shares m:
 *
 *     m* = min{ m' buildable : m' >= q + delta(n') + burst + backup }
 *
 *     q      Robbins-Monro estimate of Q_{1-eps}(E), the DATA shares a block
 *            still misses when it first crosses the decode wake line;
 *     delta  base(n) + delta_off: the BEC density-evolution margin at the
 *            candidate n (plus the finite-length term downlink) and the
 *            online surplus correction S = T - real;
 *     burst  fast feedforward above a slow mean of E (not above q, which a
 *            short burst never reaches), or the physical
 *            ceil((1 - alive/carriers) * n) term when a carrier is dead.
 *            The burst term is capped at n/4; the carrier term is a
 *            physical estimate and is not capped.
 *
 * eps is not a constant: the marginal trade-off between one more parity
 * share and one more failed block sets eps ~ sigma / ((n+m)(1+w)), clamped
 * to [1/(n+m), 5%]. sigma is an EWMA of |E - slow mean(E)|, at least one
 * share; measuring around q would inflate the scale when the cold seed is
 * deliberately conservative.
 *
 * The output is mapped onto the splits the codec can build. The downlink
 * builds every n in 1..192 of its 256 shares; the uplink builds the set
 * listed in tbt_fec_sclc_delta.h. The mapping never chooses less parity than
 * m*, so a quantization step is always on the safe side. An uplink sample is
 * one group; eps is per group, while a super-block fails when any of its
 * eight groups fails. `rate_pct` survives only as a compatibility label
 * derived from n/m; it never enters a decision.
 *
 * The controller never touches the wire. The caller sends FBK with
 * m_target, and the data plane puts n/m in the next block header.
 *
 * Safety: eight consecutive first failures, or a first-failure rate above
 * 2*eps in the last max(32, ceil(1/(2*eps))) blocks, add max(2, n/16) parity
 * shares and freeze one feedback period. A dead carrier population, more
 * than 10% stalled carriers, or more than 1% abandoned shares freeze q,
 * delta, burst and m; a frozen block sample updates none of them. E enters
 * the quantile update unchanged; no path subtracts a margin from the loss
 * estimate.
 */
#ifndef _TBT_FEC_ADAPT_SCLC_H
#define _TBT_FEC_ADAPT_SCLC_H

#include "tbt_fec.h"
#include "tbt_fec_adapt.h" /* bitmap and carrier helpers, shared with RLNC */

/* ------------------------------------------------------------------ *
 * Buildable splits and the wire label
 * ------------------------------------------------------------------ */

/* True when the codec builds this split. DOWN requires n + m == 256 and
 * n in 1..192 (the density-evolution range). UP requires n + m == 32 and n
 * in the constructible set {1,2,4..18,21..25,28,30}. */
bool tbt_fec_adapt_sclc_buildable(u8 variant, u16 n, u16 m);

/* Compatibility label only: n*100/(n+m) rounded half up, at least 1 when
 * n > 0 and 0 when n == 0. No control decision reads it. */
u8 tbt_fec_adapt_sclc_rate_pct(u16 n, u16 m);

/* A wire profile resolved from FEC_CFG. `kind` is TBT_FEC_SCLC_DOWN or
 * TBT_FEC_SCLC_UP, `symbol` is 0 (binary) or 1 (GF(16)), `rate_pct` is the
 * derived label and `rate_ppm` is ceil(n*1e6/(n+m)). */
struct tbt_fec_adapt_sclc_wire
{
    u8 kind;
    u8 symbol;
    u16 block;
    u16 L;
    u8 rate_pct;
    u32 rate_ppm;
    u16 n;
    u16 m;
    u16 groups;
};

/* Resolve a CFG into a supported profile. Returns 0, or -1 when the body is
 * not a complete SC-LDPC profile, the variant/block/L does not match a
 * shipped codec, groups*(n+m) != carriers, the split is not buildable, or a
 * nonzero rate_ppm disagrees with n. */
int tbt_fec_adapt_sclc_wire_of_cfg(const struct tbt_fec_cfg *cfg,
                                  u16 carriers,
                                  struct tbt_fec_adapt_sclc_wire *out);

/* Fill a CFG body for `p`. The caller sets switch_pkt/block_base/seed/CRC
 * afterwards. DOWN carries block in w and rate_ppm in k; UP carries L in w,
 * block in k and the codec rate in d. Returns 0 or -1. */
int tbt_fec_adapt_sclc_wire_to_cfg(const struct tbt_fec_adapt_sclc_wire *p,
                                  u32 epoch, u8 flags,
                                  struct tbt_fec_cfg *cfg);

/* True when `next` may replace `cur` by a rate-only switch: same variant,
 * symbol, block, L, groups and n+m. n/m may differ. */
bool tbt_fec_adapt_sclc_wire_compatible(const struct tbt_fec_cfg *cur,
                                        const struct tbt_fec_cfg *next,
                                        u16 carriers);

/* ------------------------------------------------------------------ *
 * Loss-rate estimator (diagnostic; the control law does not read it)
 * ------------------------------------------------------------------ */

#define TBT_FEC_ADAPT_SCLC_BUCKETS 8
#define TBT_FEC_ADAPT_SCLC_BUCKET_NS (500ull * 1000ull * 1000ull)
#define TBT_FEC_ADAPT_SCLC_WIN_NS \
    (TBT_FEC_ADAPT_SCLC_BUCKETS * TBT_FEC_ADAPT_SCLC_BUCKET_NS)
/* A bucket is fast when it started less than 1 s ago (the last two buckets). */
#define TBT_FEC_ADAPT_SCLC_FAST_NS (2ull * TBT_FEC_ADAPT_SCLC_BUCKET_NS)
/* No super-block for this long means the application is idle: freeze.
 * 200 ms was shorter than one uplink super-block at a loss-repaired rate
 * (8x22, 1.4 MB payload, ~1 s), so the estimator sat "idle" between every
 * two blocks, the FBK dropped P_VALID, and the sender's control law never
 * saw a valid sample. Two seconds still freezes an idle app promptly. */
#define TBT_FEC_ADAPT_SCLC_IDLE_NS (2000ull * 1000ull * 1000ull)
/* Minimum evidence before p is valid. */
#define TBT_FEC_ADAPT_SCLC_MIN_SHARES 512u
#define TBT_FEC_ADAPT_SCLC_MIN_SUPERS 8u
/* A multi-group uplink super-block carries groups * (n+m) shares (256) but
 * its DATA side is groups * n, i.e. 176 shares at 70% on 8x22 versus 128 on
 * the single-group downlink. Eight uplink super-blocks inside the 4 s window
 * therefore demand ~2.9 MB/s of delivered payload; a 25% loss plus repair
 * drops below that and P_VALID never rises. Four super-blocks still hold
 * 704 shares (above the 512 floor) and keep the downlink at eight. */
#define TBT_FEC_ADAPT_SCLC_MIN_SUPERS_UP 4u
/* p_ewma += (p_block - p_ewma) / 2^TBT_FEC_ADAPT_SCLC_EWMA_SHIFT. */
#define TBT_FEC_ADAPT_SCLC_EWMA_SHIFT 3
#define TBT_FEC_ADAPT_SCLC_EWMA_RING 64

struct tbt_fec_adapt_sclc_win
{
    u64 start_ns;
    u32 sent;      /* alive-filtered DATA shares */
    u32 miss;      /* of sent: shares that needed parity or a NACK */
    u32 supers;    /* completed super-blocks folded into this bucket */
    u32 requested; /* DATA shares named by NACK */
    u32 pkts;      /* DATA packets sent; the r_fail denominator */
    u32 abandoned; /* shares in abandoned blocks */
    u32 unknown;   /* blocks skipped because real was unknown */
    u32 codec_fail; /* missing shares the codec could not recover */
};

struct tbt_fec_adapt_sclc_sample
{
    u64 super_id;
    u64 bucket_start_ns;
    u16 sent;
    u16 miss;
    u8 bucket;
    u8 used;
};

struct tbt_fec_adapt_sclc_est
{
    u16 shares; /* n+m for one group: 256 downlink, 32 uplink */
    u8 cur;     /* current bucket, 0..7 */
    u8 ewma_head;
    u8 ewma_n;
    u64 super_id;
    u32 ewma_anchor_ppm;
    struct tbt_fec_adapt_sclc_sample ewma_ring[TBT_FEC_ADAPT_SCLC_EWMA_RING];
    /* The super-block being filled by tbt_fec_adapt_sclc_est_block(). */
    u32 sb_sent;
    u32 sb_miss;
    /* EWMA over super-blocks, ppm. */
    u32 p_ewma_ppm;
    /* Liveness of the carrier population, set by est_carriers(). */
    u16 alive;
    u16 stalled;
    u16 carriers;
    u64 last_activity_ns;
    u64 abandoned_total;
    u64 unknown_total;
    struct tbt_fec_adapt_sclc_win win[TBT_FEC_ADAPT_SCLC_BUCKETS];
};

/* What the estimator knows at one instant. The diagnostic consumer reads
 * exactly this snapshot. A sender may fill it from FEC_FBK and local NACK
 * counters instead of from a local estimator. */
struct tbt_fec_adapt_sclc_est_out
{
    u32 p_ppm;       /* max(p_ewma, p_win) */
    u32 p_ewma_ppm;  /* super-block EWMA */
    u32 p_win_ppm;   /* 4 s window */
    u32 p_fast_ppm;  /* 1 s window */
    u32 r_fail_ppm;  /* requested / DATA packets in the fast window */
    u32 codec_fail_ppm; /* codec_fail / miss in the fast window */
    u32 sent_win;
    u32 miss_win;
    u32 supers_win;
    u32 abandoned_win;
    u32 unknown_win;
    u32 requested_win;
    u32 pkts_win;
    u32 codec_fail_win;
    u64 abandoned_total;
    u64 unknown_total;
    u16 alive;
    u16 stalled;
    u16 carriers;
    bool p_valid; /* sent_win >= 512 and supers_win >= 8 */
    bool r_valid; /* pkts_win > 0 */
    bool pause_idle;
    bool pause_carrier;
    bool pause_abandoned;
};

/* Prepare e for a shape with `shares` = n+m. A zero or out-of-range value is
 * replaced by 256, the downlink block. */
void tbt_fec_adapt_sclc_est_init(struct tbt_fec_adapt_sclc_est *e, u16 shares);

/* Record the carrier population. The estimator freezes when alive < 90% or
 * when more than 10% stall. */
void tbt_fec_adapt_sclc_est_carriers(struct tbt_fec_adapt_sclc_est *e,
                                     u16 alive, u16 stalled, u16 total);

/* One group block. `real` is the number of DATA shares the block carries,
 * `miss_mask` bit i marks share i as needing repair, and `alive` is the
 * 256-bit carrier bitmap. Only shares with i < real and a live carrier enter
 * the sample. Call once per group, then est_super() when the super-block is
 * complete. */
void tbt_fec_adapt_sclc_est_block(
    struct tbt_fec_adapt_sclc_est *e, u64 now_ns, u16 group, u16 real,
    const u8 miss_mask[TBT_FEC_BITMAP_BYTES],
    const u8 alive[TBT_FEC_BITMAP_BYTES]);

/* Fold the current super-block into the window and the EWMA. A super-block
 * with no alive-filtered share is dropped, not counted. */
void tbt_fec_adapt_sclc_est_super(struct tbt_fec_adapt_sclc_est *e,
                                  u64 now_ns);
/* Recompute p_ewma from the bounded 8-bucket sent/miss totals. Used after
 * an attributed late-DATA correction, never as a fixed ppm subtraction. */
void tbt_fec_adapt_sclc_est_recompute(struct tbt_fec_adapt_sclc_est *e);
/* Remove one miss from an exact bucket; returns false when it has none. */
bool tbt_fec_adapt_sclc_est_correct_miss(struct tbt_fec_adapt_sclc_est *e,
                                         u8 bucket);
/* Correct one miss in the exact committed super-block. Replays the 64-super
 * EWMA ring so p_ewma matches the corrected history. */
bool tbt_fec_adapt_sclc_est_correct_super(struct tbt_fec_adapt_sclc_est *e,
                                          u64 super_id);

/* A block whose real DATA count is unknown: count it as a quality sample and
 * do not estimate p from it. */
void tbt_fec_adapt_sclc_est_unknown(struct tbt_fec_adapt_sclc_est *e,
                                    u64 now_ns);

/* `shares` DATA shares were lost with an abandoned block. They do not enter p;
 * more than 1% of the window freezes the controller. */
void tbt_fec_adapt_sclc_est_abandoned(struct tbt_fec_adapt_sclc_est *e,
                                      u64 now_ns, u32 shares);

/* Add the deltas since the previous call: NACK-named DATA shares and DATA
 * packets sent. Both feed the 1 s r_fail window. */
void tbt_fec_adapt_sclc_est_fail(struct tbt_fec_adapt_sclc_est *e,
                                 u64 now_ns, u32 requested, u32 pkts);

/* Add the missing shares the codec could not recover. This is diagnostic and
 * never enters p. */
void tbt_fec_adapt_sclc_est_codec_fail(struct tbt_fec_adapt_sclc_est *e,
                                       u64 now_ns, u32 shares);

/* Read the current snapshot. Pure: it does not advance the window. A bucket
 * older than 4 s is ignored, so a stalled stream stops producing p. */
void tbt_fec_adapt_sclc_est_sample(const struct tbt_fec_adapt_sclc_est *e,
                                   u64 now_ns,
                                   struct tbt_fec_adapt_sclc_est_out *o);

/* ------------------------------------------------------------------ *
 * Continuous controller
 * ------------------------------------------------------------------ */

/* All continuous quantities are Q16.16 shares (or fractions for eps). */
#define TBT_FEC_ADAPT_SCLC_ONE 65536u
#define TBT_FEC_ADAPT_SCLC_COLD_BLOCKS 16u
#define TBT_FEC_ADAPT_SCLC_BACKUP_RUN 8u
#define TBT_FEC_ADAPT_SCLC_BACKUP_WIN 32u  /* minimum failure window */
#define TBT_FEC_ADAPT_SCLC_BACKUP_MAX 128u /* history depth */
/* gamma_q = min(1/16, pi/(8d)); gamma_delta = 1/64. */
#define TBT_FEC_ADAPT_SCLC_GAMMA_Q_Q16 4096u
#define TBT_FEC_ADAPT_SCLC_GAMMA_DELTA_Q16 1024u
/* eps is clamped to [1/(n+m), 5%]. */
#define TBT_FEC_ADAPT_SCLC_EPS_MAX_Q16 3277u /* 0.05 * 65536 */
/* sigma floor: one share. */
#define TBT_FEC_ADAPT_SCLC_SIGMA_MIN_Q16 TBT_FEC_ADAPT_SCLC_ONE

enum tbt_fec_adapt_sclc_reason
{
    TBT_FEC_ADAPT_SCLC_R_NONE = 0,
    TBT_FEC_ADAPT_SCLC_R_COLD,      /* fewer than 16 samples: m held */
    TBT_FEC_ADAPT_SCLC_R_STEADY,    /* q + delta + burst */
    TBT_FEC_ADAPT_SCLC_R_BURST,     /* the fast feedforward moved m */
    TBT_FEC_ADAPT_SCLC_R_BACKUP,    /* safety backup or its hold tick */
    TBT_FEC_ADAPT_SCLC_R_PAUSED,    /* carrier/abandoned freeze */
    TBT_FEC_ADAPT_SCLC_R_SATURATED, /* m hit m_min or m_max */
};

struct tbt_fec_adapt_sclc_out
{
    u16 n_target;    /* payload shares after the buildable mapping */
    u16 m_target;    /* parity shares after the buildable mapping */
    u32 q_q16;       /* low-passed Q_{1-eps}(E) used by the output */
    u32 q_raw_q16;   /* the Robbins-Monro estimate before the low-pass */
    u32 delta_q16;   /* analytic margin plus online surplus correction */
    u32 burst_q16;   /* max(burst feedforward, carrier feedforward) */
    u32 eps_q16;     /* target first-failure probability */
    u32 sigma_q16;   /* |E-q| scale */
    u16 backup;      /* safety shares currently added */
    u8 reason;
    u8 frozen;
    u16 m_req;       /* round(q_act + delta + burst) + backup at the chosen
                      * n; m_target is never below min(m_req, m_max) */
    int delta_off_q16; /* the integrated surplus offset, delta - delta0 */
};

/* One controller per direction. Zeroing is not enough; call init. */
struct tbt_fec_adapt_sclc_ctl
{
    u8 ready;
    u8 variant;     /* TBT_FEC_SCLC_DOWN or TBT_FEC_SCLC_UP */
    u8 frozen;      /* this tick is paused */
    u8 cold_done;   /* the 16-sample warm start has run */
    u8 backup_hold; /* freeze_ticks was set for this tick */
    u8 sat_min;     /* the last control sum hit m_min */
    u8 sat_max;     /* the last control sum hit m_max */
    u8 cold_n;      /* cold samples collected */
    u16 shares;     /* n + m per group */
    u16 groups;
    u16 extra;      /* wake margin: 8 downlink, 2 uplink */
    u16 n_cur;      /* payload shares in force */
    u16 m_cur;      /* parity shares in force */
    u16 m_min;
    u16 m_max;
    u32 blocks;     /* samples seen */
    u16 fail_run;   /* consecutive first failures */
    u16 backup_off; /* safety shares added to the control sum */
    u16 freeze_ticks;
    u16 d;          /* feedback delay, blocks, from the last tick */
    u16 cold_e[TBT_FEC_ADAPT_SCLC_COLD_BLOCKS];
    u32 q_q16;
    u32 q_act_q16;  /* low-passed q used by the output */
    u32 delta_q16;
    u32 delta0_q16; /* analytic margin at the current n, clamp reference */
    int delta_off_q16; /* the only integrated state; delta = base + off */
    u32 eps_q16;
    u32 sigma_q16;
    u32 ewma_fast_q16; /* alpha 1/4, per block */
    u32 ewma_slow_q16; /* alpha 1/64, the burst baseline, per block */
    u32 burst_q16;
    u32 w_ppm;      /* W/T_b from the last tick, 1.0 = 1000000 */
    u32 delivered;  /* DATA shares since the last tick */
    u16 blocks_tick; /* accepted samples since the last tick */
    u32 fail_hist[TBT_FEC_ADAPT_SCLC_BACKUP_MAX / 32u];
    /* bit 0 of [0] is the newest block */
    u16 fail_hist_n;
    u64 last_tick_ns;
};

/* Start a direction. `shares` is n+m per group (256 downlink, 32 uplink),
 * `groups` the group count, `extra` the decode wake margin, `n_init` the
 * payload shares in force at start, and `variant` TBT_FEC_SCLC_DOWN or
 * TBT_FEC_SCLC_UP. The controller holds m at n_init until 16 samples have
 * arrived. The cold seed keeps q at max(empirical quantile, m_init - delta0)
 * when 16 samples can resolve 1-eps, and at m_init - delta0 otherwise; 16
 * samples cannot estimate a quantile below 1/16. delta0 is recomputed from
 * the current n on every tick: the density-evolution margin plus the
 * finite-length term downlink, the conservative core bound uplink. The
 * online offset is the only integrated state, and the output is the least
 * buildable m whose own n satisfies m >= q_act + base(n) + off + burst +
 * backup; a change of n therefore carries the analytic margin into m. The
 * offset is clamped to [delta_lo(n), 8] so the total stays at least one
 * share uplink and base/2 downlink. Returns 0, or -1 when the geometry,
 * margin or split is invalid. */
int tbt_fec_adapt_sclc_init(struct tbt_fec_adapt_sclc_ctl *c, u16 shares,
                            u16 groups, u16 extra, u16 n_init, u8 variant);

/* One group block, at the instant it first crosses the decode wake line.
 * `E` is real - ndata (DATA shares missing), `S` is ndata + nparity - real
 * (surplus over the DATA count), `first_ok` is the first decode attempt's
 * result and `real` is the block's DATA share count. Blocks with
 * real < n/2 are ignored, as are blocks whose E is above real. A block
 * sampled while the last tick was frozen updates only the abandoned-share
 * denominator; q, delta, the failure window and the backup do not move.
 * E enters the quantile unchanged. */
void tbt_fec_adapt_sclc_block_sample(struct tbt_fec_adapt_sclc_ctl *c,
                                     u16 E, u16 S, bool first_ok, u16 real);

/* One feedback tick. `d` is the loop delay in blocks, `w_ppm` is W/T_b in
 * ppm (1.0 = 1000000), `alive`/`stalled` are carrier counts and `abandoned`
 * is the DATA shares abandoned since the previous tick. A dead population,
 * more than 10% stalled, or abandoned > 1% of delivered freezes q, delta,
 * burst and m. The output is the next m_target; call once per feedback
 * period. q's low-pass and the burst decay advance by the number of block
 * samples since the previous tick, so an eight-group uplink super-block
 * moves them eight steps, not one. */
void tbt_fec_adapt_sclc_tick(struct tbt_fec_adapt_sclc_ctl *c, u64 now_ns,
                             u16 d, u32 w_ppm, u16 alive, u16 stalled,
                             u32 abandoned,
                             struct tbt_fec_adapt_sclc_out *out);

#endif /* _TBT_FEC_ADAPT_SCLC_H */
