/* tbt_fec_adapt_sclc.h - SC-LDPC adaptive rate: estimator and rate control.
 *
 * This file and tbt_fec_adapt_sclc.c compile twice: into the module and into
 * the host test. They allocate nothing, read no clock and touch no wire, so
 * the host test can drive every decision with virtual time.
 *
 * The estimator turns per-block repair evidence into a DATA-share loss rate:
 *
 *   - one group block reports `real` DATA shares, a 256-bit miss mask and the
 *     256-bit carrier alive bitmap;
 *   - a completed super-block is folded into one of eight 500 ms buckets
 *     (the 4 s window) and into an EWMA with alpha = 1/8;
 *   - p_used = max(EWMA, window) once the window holds at least 512
 *     alive-filtered shares and 8 super-blocks;
 *   - the last two buckets give p_fast and the 1 s residual-failure ratio
 *     r_fail; a fast rise above the slow estimate marks a burst;
 *   - pause flags freeze the controller on application idle (200 ms), on a
 *     dead or stalled carrier population, and on abandoned blocks above 1%.
 *
 * The controller maps p to a target codec rate and quantizes down onto the
 * ladder the SC-LDPC codec can build:
 *
 *     r_t = clamp(1 - p - 0.20, 0.25, 0.70)
 *     n   = floor(r_actual * (n + m)), m = (n + m) - n
 *
 * It never touches the wire. A suggestion names an epoch, the target rate and
 * the derived n/m; the caller sends FEC_CFG, then calls
 * tbt_fec_adapt_sclc_ctl_applied() when both ends commit, or
 * tbt_fec_adapt_sclc_ctl_abort() when the CFG fails.
 *
 * Safety rules: a lower rate is fast (two evaluations), a higher rate is slow
 * (six evaluations plus a 10 s dwell), hysteresis is 2 pp, the minimum gap is
 * 3 s, a direction reversal waits 5 s, and 10 switches in 60 s are the cap.
 * A residual-failure rise lowers one rate step and bans raises for 30 s. A
 * probe may cross the up hysteresis when the residual is below 0.3%; it
 * observes for 10 s and reverts on a residual rise. A committed raise can be
 * rolled back when the caller reports a bad commit.
 *
 * The controller is off by default. When off it never suggests a switch, so
 * the configured fixed shape stays in force and the wire shape does not move.
 */
#ifndef _TBT_FEC_ADAPT_SCLC_H
#define _TBT_FEC_ADAPT_SCLC_H

#include "tbt_fec.h"
#include "tbt_fec_adapt.h" /* bitmap and carrier helpers, shared with RLNC */

/* ------------------------------------------------------------------ *
 * Rate ladder
 * ------------------------------------------------------------------ */

#define TBT_FEC_ADAPT_SCLC_LEVELS 6

/* One point on the codec rate ladder. `ppm` is the actual codec rate, not the
 * nominal rate_pct: 60 -> 0.592593 and 70 -> 0.695652 because the SC-IRA
 * graph quantizes n_base. n/m must be derived from `ppm`, not from `pct`. */
struct tbt_fec_adapt_sclc_rate
{
    u8 pct; /* codec rate_pct: 25, 30, 40, 50, 60, 70 */
    u32 ppm; /* actual codec rate in parts per million */
};

/* The ladder entry at `index`, or NULL when index >= TBT_FEC_ADAPT_SCLC_LEVELS. */
const struct tbt_fec_adapt_sclc_rate *
tbt_fec_adapt_sclc_rate_at(u8 index);

/* Ladder index of a codec rate_pct, or 0xff when the codec cannot build it.
 * 35 and 45 are deliberately absent: the 0.3.1 measurements are weak there. */
u8 tbt_fec_adapt_sclc_rate_index(u8 rate_pct);

/* Actual rate in ppm, or 0 when rate_pct is not on the ladder. */
u32 tbt_fec_adapt_sclc_rate_ppm(u8 rate_pct);

/* Ladder index whose n/m split for `shares = n + m` is exactly (n, m), or
 * 0xff. The split uses the actual codec rate, so 18/14 of 32 maps to rate_pct
 * 60 even though 18/32 is 0.5625, not 0.592593. */
u8 tbt_fec_adapt_sclc_rate_index_for_shape(u16 n, u16 m);

/* Resolve the controller's initial ladder index and rate mask from a
 * configured SC-LDPC shape. `variant` is TBT_FEC_SCLC_DOWN or
 * TBT_FEC_SCLC_UP. Both directions support every ladder split: the binary
 * downlink lowers its payload degree above 50% and adds one check per
 * parity share (no puncturing), and the GF(16) uplink builds each split as
 * its own graph. Returns 0
 * and fills idx/mask when n/m is on the ladder, or -1 with
 * idx=0xff/mask=0 when it is not or the variant/group count does not
 * match. */
int tbt_fec_adapt_sclc_init_plan(u16 n, u16 m, u16 groups, u8 variant,
                                 u8 *idx, u8 *mask);

/* A rate and the n/m it derives for one direction. `groups * (n + m)` is the
 * carrier count. The struct is local to the control law; the wire still
 * carries n, m, groups, block and rate_pct. */
struct tbt_fec_adapt_sclc_shape
{
    u8 rate_pct;
    u32 rate_ppm;
    u16 n;
    u16 m;
    u16 groups;
};

/* Derive n = floor(rate_ppm * shares / 1e6) and m = shares - n. Returns 0, or
 * -1 when rate_pct is not on the ladder, shares is outside 2..256, groups is
 * zero, groups * shares exceeds 256, or the split leaves an empty side. */
int tbt_fec_adapt_sclc_shape(u8 rate_pct, u16 shares, u16 groups,
                             struct tbt_fec_adapt_sclc_shape *out);

/* The codec profile the adaptive law selects for a rate. rate_pct 40 uses
 * profile B (block 512, L 16, 4096 payload bits per codeword); every other
 * ladder point uses profile A (block 256, L 16, 2048). This applies only
 * while the control law is enabled: adaptive=off keeps the configured codec
 * (0.3.0's block 256 / L 8 / rate 50). Returns 0, or -1 when rate_pct is not
 * on the ladder. */
struct tbt_fec_adapt_sclc_profile
{
    u16 block;   /* codeword block, the codec's w */
    u16 L;       /* coupling length */
    u16 payload; /* payload bits per codeword */
};

int tbt_fec_adapt_sclc_profile_of(u8 rate_pct,
                                  struct tbt_fec_adapt_sclc_profile *out);

/* A wire profile resolved from FEC_CFG. `kind` is TBT_FEC_SCLC_DOWN or
 * TBT_FEC_SCLC_UP, `symbol` is 0 (binary) or 1 (GF(16)), and `rate_pct` is
 * the ladder label (0 when the n/m split is not on the ladder). */
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

/* Resolve a CFG into the supported profile set. Returns 0, or -1 when the
 * body is not a complete SC-LDPC profile, the variant/block/L does not match
 * a shipped codec, or the n/m split is not on the rate ladder. */
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
 * Estimator
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
/* A fast window this far above the slow estimate is a burst. */
#define TBT_FEC_ADAPT_SCLC_BURST_PPM 100000u
#define TBT_FEC_ADAPT_SCLC_BURST_GAP_PPM 50000u

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

/* What the estimator knows at one instant. The controller consumes exactly
 * this snapshot. A sender may fill it from FEC_FBK and local NACK counters
 * instead of from a local estimator. */
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
    bool burst;
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
 * drives the post-commit rollback; it never enters p. */
void tbt_fec_adapt_sclc_est_codec_fail(struct tbt_fec_adapt_sclc_est *e,
                                       u64 now_ns, u32 shares);

/* Read the current snapshot. Pure: it does not advance the window. A bucket
 * older than 4 s is ignored, so a stalled stream stops producing p. */
void tbt_fec_adapt_sclc_est_sample(const struct tbt_fec_adapt_sclc_est *e,
                                   u64 now_ns,
                                   struct tbt_fec_adapt_sclc_est_out *o);

/* ------------------------------------------------------------------ *
 * Rate controller
 * ------------------------------------------------------------------ */

#define TBT_FEC_ADAPT_SCLC_SEC_NS 1000000000ull
#define TBT_FEC_ADAPT_SCLC_EVAL_NS (250ull * 1000ull * 1000ull) /* 250 ms */
#define TBT_FEC_ADAPT_SCLC_UP_RUN 6u
#define TBT_FEC_ADAPT_SCLC_DOWN_RUN 2u
#define TBT_FEC_ADAPT_SCLC_UP_DWELL_NS (10ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_DOWN_DWELL_NS (3ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_DWELL_CAP_NS (30ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_REVERSE_NS (5ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_MIN_GAP_NS (3ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_RATE_MAX 10u
#define TBT_FEC_ADAPT_SCLC_RATE_WIN_NS (60ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_HOLD_COUNT 3u
#define TBT_FEC_ADAPT_SCLC_HOLD_WIN_NS (30ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_HOLD_NS (30ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_OSC_WIN_NS (10ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_PROBE_STABLE_NS (15ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_PROBE_OBSERVE_NS (10ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_PROBE_LOW_PPM 3000u
#define TBT_FEC_ADAPT_SCLC_PROBE_HIGH_PPM 15000u
#define TBT_FEC_ADAPT_SCLC_PROBE_MAX_P_PPM 300000u
#define TBT_FEC_ADAPT_SCLC_PROBE_BAN_NS (60ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_FAIL_RUN 2u
#define TBT_FEC_ADAPT_SCLC_FAIL_HIGH_PPM 20000u
#define TBT_FEC_ADAPT_SCLC_FAIL_BAN_NS (30ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_CODEC_FAIL_PPM 100000u
#define TBT_FEC_ADAPT_SCLC_ROLLBACK_NS (10ull * TBT_FEC_ADAPT_SCLC_SEC_NS)
#define TBT_FEC_ADAPT_SCLC_HYST_PPM 20000u
#define TBT_FEC_ADAPT_SCLC_MARGIN_PPM 200000u
#define TBT_FEC_ADAPT_SCLC_DWELL_MUL 2u

enum tbt_fec_adapt_sclc_reason
{
    TBT_FEC_ADAPT_SCLC_R_NONE = 0,
    TBT_FEC_ADAPT_SCLC_R_DISABLED,
    TBT_FEC_ADAPT_SCLC_R_BUSY,
    TBT_FEC_ADAPT_SCLC_R_HOLD,
    TBT_FEC_ADAPT_SCLC_R_PAUSED,
    TBT_FEC_ADAPT_SCLC_R_WAIT,
    TBT_FEC_ADAPT_SCLC_R_STEADY,
    TBT_FEC_ADAPT_SCLC_R_FAST_START,
    TBT_FEC_ADAPT_SCLC_R_UP,
    TBT_FEC_ADAPT_SCLC_R_DOWN,
    TBT_FEC_ADAPT_SCLC_R_BURST,
    TBT_FEC_ADAPT_SCLC_R_FAIL_DOWN,
    TBT_FEC_ADAPT_SCLC_R_PROBE_UP,
    TBT_FEC_ADAPT_SCLC_R_PROBE_REVERT,
    TBT_FEC_ADAPT_SCLC_R_PROBE_ABORT,
    TBT_FEC_ADAPT_SCLC_R_PROBE_COMMIT,
    TBT_FEC_ADAPT_SCLC_R_ROLLBACK,
};

struct tbt_fec_adapt_sclc_ctl
{
    u8 ready;   /* init accepted the rate and geometry */
    u8 enabled; /* 0 keeps the fixed shape; eval never suggests a switch */
    u8 rate_mask; /* bit i: ladder index i is supported by this codec */
    u8 refused_mask; /* bit i: the peer refused ladder index i with a CFG
                      * NACK; no later suggestion may select it */
    u8 no_down; /* a refused lower rate bans further down steps */
    u8 cur;     /* current ladder index */
    u8 busy;    /* a suggestion is in flight */
    u8 pending_to;
    u8 pending_reason;
    u8 pending_probe;
    u8 last_dir;  /* 1 up, 2 down, 0 none */
    u8 prev_idx;
    u8 up_run;
    u8 down_run;
    u8 fail_run;
    u8 probe_fail_run;
    u8 probe_active;
    u8 probe_from;
    u8 fast_start_done;
    u8 rollback_pending;
    u8 commit_raise;
    u8 commit_from;
    u32 dwell_mul;
    u32 epoch; /* last suggested epoch, 0 before the first */
    u16 shares;
    u16 groups;
    u64 last_eval_ns;
    u64 last_switch_ns;
    u64 probe_start_ns;
    u64 hold_until_ns;
    u64 no_probe_until;
    u64 no_raise_until;
    u64 probe_low_since_ns; /* residual has been below 0.3% since here */
    u64 commit_ns;
    u64 r_ignore_until_ns; /* new epoch: old-epoch NACKs must not count */
    u64 reject_until_ns;   /* quiet window after a refused CFG */
    u8 reject_target;      /* the fallback to try inside that window */
    u64 sw[TBT_FEC_ADAPT_SCLC_RATE_MAX];
    u32 sw_n;
    u32 sw_head;
};

struct tbt_fec_adapt_sclc_out
{
    u8 switch_now;
    u8 from;
    u8 to;
    u8 reason;
    u32 epoch; /* the epoch to put in FEC_CFG when switch_now */
    struct tbt_fec_adapt_sclc_shape shape;
};

/* Start at `rate_pct` (must be on the ladder) with the given per-group shares
 * and group count. The controller is off unless `enabled` is true. Returns 0,
 * or -1 when the rate or the geometry is invalid; then `ready` stays 0 and
 * eval reports DISABLED. */
int tbt_fec_adapt_sclc_ctl_init(struct tbt_fec_adapt_sclc_ctl *c, bool enabled,
                                u8 rate_pct, u16 shares, u16 groups);

/* Restrict the ladder to the rates this codec can build. Bit i is ladder
 * index i; the current rate must stay supported. A mask of 0 is ignored.
 * The default after init is every ladder point. */
void tbt_fec_adapt_sclc_ctl_set_rate_mask(struct tbt_fec_adapt_sclc_ctl *c,
                                          u8 mask);

/* Set the last suggested epoch. The data plane starts at 1 after the INIT
 * CFG, so the first switch suggestion is 2 and the receiver never mistakes
 * it for the INIT generation. */
void tbt_fec_adapt_sclc_ctl_set_epoch(struct tbt_fec_adapt_sclc_ctl *c,
                                      u32 epoch);

/* Largest n or m over the supported ladder points for `shares`. The caller
 * sizes the recovered-slot scratch so a later rate switch cannot outgrow
 * it. `mask` 0 means every ladder point. */
u16 tbt_fec_adapt_sclc_max_side(u16 shares, u8 mask);

/* Turn the control law on or off at runtime. Turning it off abandons any
 * pending suggestion and probe; the rate in force stays fixed. Turning it on
 * starts a fresh fast-start window. The caller must not apply a suggestion
 * that was already returned before the change. */
void tbt_fec_adapt_sclc_ctl_set_enabled(struct tbt_fec_adapt_sclc_ctl *c,
                                        bool enabled, u64 now_ns);

/* Evaluate one 250 ms tick. The first call in a tick returns a suggestion;
 * later calls return R_NONE until the next tick. A suggestion sets `busy`;
 * the caller must then call applied() or abort(). A probe commit is reported
 * as R_PROBE_COMMIT with switch_now == 0. */
void tbt_fec_adapt_sclc_ctl_eval(struct tbt_fec_adapt_sclc_ctl *c,
                                 const struct tbt_fec_adapt_sclc_est_out *o,
                                 u64 now_ns,
                                 struct tbt_fec_adapt_sclc_out *out);

/* The suggested switch committed on both ends. Updates the rate, dwell, rate
 * cap and probe state. */
void tbt_fec_adapt_sclc_ctl_applied(struct tbt_fec_adapt_sclc_ctl *c,
                                    u64 now_ns);

/* The suggested switch failed. The rate is unchanged; a probe keeps observing,
 * a pending probe start is forgotten. */
void tbt_fec_adapt_sclc_ctl_abort(struct tbt_fec_adapt_sclc_ctl *c);

/* True when the fresh epoch shows too many residual failures or too many
 * unrecovered missing shares. `r_fail` must be valid; `codec_fail` is a
 * share of the missing shares. */
bool tbt_fec_adapt_sclc_commit_bad(
    const struct tbt_fec_adapt_sclc_est_out *o);

/* Feed the fresh epoch's snapshot. If the last applied switch was a raise,
 * still inside its 10 s probation, and the snapshot is bad, the next eval
 * suggests a switch back to the rate before the raise. A bad lower-rate
 * commit is left to the normal fail-down logic. */
void tbt_fec_adapt_sclc_ctl_commit_failed(
    struct tbt_fec_adapt_sclc_ctl *c,
    const struct tbt_fec_adapt_sclc_est_out *o, u64 now_ns);

/* The receiver refused the suggested CFG (for example, a legacy peer that
 * only supports 128/128). Clears the suggestion and remembers the refused
 * shape for the rest of the session: eval never suggests it again. A refused
 * raise bans raises for 30 s and retries one lower step (after 30 s, inside a
 * 60 s quiet window) in case the refusal was transient. A refused lower rate
 * keeps the current rate and bans every further down step. */
void tbt_fec_adapt_sclc_ctl_rejected(struct tbt_fec_adapt_sclc_ctl *c,
                                     u64 now_ns);

#endif /* _TBT_FEC_ADAPT_SCLC_H */
