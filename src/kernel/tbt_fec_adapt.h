/* tbt_fec_adapt.h - adaptive FEC: the loss estimator and the n/m control law.
 *
 * This file and tbt_fec_adapt.c compile twice: into the module and into the
 * host test (tests/unit/test_fec_adapt.c). They use no kernel facility a plain C
 * program lacks, and they allocate nothing.
 *
 * The estimator turns per-block repair evidence into a DATA-share loss rate:
 *
 *   - one block (one group) reports `real` DATA shares, a miss mask (a share
 *     that parity recovered or a NACK named) and the carrier alive bitmap;
 *   - a completed super-block is folded into a pair of 500 ms buckets (the
 *     1 s window) and into an EWMA with alpha = 1/8;
 *   - p_used = max(EWMA, window) once the window holds at least 1024
 *     alive-filtered shares and 4 super-blocks;
 *   - pause flags freeze the controller on application idle (200 ms), on a
 *     dead or stalled carrier population, and on abandoned blocks above 1%.
 *
 * The controller turns p and the repair-failure ratio r_fail into a shape
 * suggestion. It never touches the wire and never reads packet or block
 * coordinates, so the same decision applies at any base_pkt/base_block. The
 * caller sends FEC_CFG with the suggested epoch, then calls
 * tbt_fec_adapt_applied() when both ends commit, or tbt_fec_adapt_abort()
 * when the CFG fails.
 *
 * The 0.3.0 shape family is fixed: n+m = 8, groups = 32, m in 1..6.
 */
#ifndef _TBT_FEC_ADAPT_H
#define _TBT_FEC_ADAPT_H

#include "tbt_fec.h"

/* struct tbt_fec_shape comes from tbt_fec.h: the switch layer owns the
 * base_pkt/base_block/slots fields. This module fills only n, m, groups, w,
 * k and d, and leaves the coordinates zero. The adaptive mode always uses
 * w = 2n, k = n, d = n. */

#define TBT_FEC_ADAPT_SHARES 8
#define TBT_FEC_ADAPT_GROUPS 32
#define TBT_FEC_ADAPT_CLASSES 6
#define TBT_FEC_ADAPT_CARRIERS TBT_FEC_MAX_CARRIERS
#define TBT_FEC_ADAPT_BITMAP_BYTES TBT_FEC_BITMAP_BYTES

/* ------------------------------------------------------------------ *
 * Estimator
 * ------------------------------------------------------------------ */

/* A bucket covers 500 ms; the window sums the two live buckets, so it holds
 * at most 1 s of samples. */
#define TBT_FEC_EST_WIN_NS (500ull * 1000ull * 1000ull)
/* No block for this long means the application is idle: freeze. */
#define TBT_FEC_EST_IDLE_NS (200ull * 1000ull * 1000ull)
/* Minimum evidence before p is valid. */
#define TBT_FEC_EST_MIN_SHARES 1024u
#define TBT_FEC_EST_MIN_SUPERS 4u
/* p_ewma += (p_block - p_ewma) / 2^TBT_FEC_EST_EWMA_SHIFT. */
#define TBT_FEC_EST_EWMA_SHIFT 3

struct tbt_fec_est_win
{
    u64 start_ns;
    u32 sent;      /* alive-filtered DATA shares */
    u32 miss;      /* of sent: shares that needed parity or a NACK */
    u32 supers;    /* completed super-blocks folded into this bucket */
    u32 requested; /* DATA shares named by NACK */
    u32 pkts;      /* DATA packets sent; the r_fail denominator */
    u32 abandoned; /* shares in abandoned blocks */
    u32 unknown;   /* blocks skipped because real was unknown */
};

struct tbt_fec_est
{
    u16 shares; /* n+m, 8 in 0.3.0 */
    u8 cur;     /* current window bucket, 0 or 1 */
    /* The super-block being filled by tbt_fec_est_block(). */
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
    struct tbt_fec_est_win win[2];
};

/* What the estimator knows at one instant. The controller consumes exactly
 * this snapshot; a sender fills it from FEC_FBK and its local NACK counters
 * instead of from a local tbt_fec_est. */
struct tbt_fec_est_out
{
    u32 p_ppm;        /* max(p_ewma, p_win) */
    u32 p_ewma_ppm;   /* super-block EWMA */
    u32 p_win_ppm;    /* 1 s window */
    u32 r_fail_ppm;   /* requested / DATA packets in the window */
    u32 sent_win;     /* alive-filtered DATA shares in the window */
    u32 miss_win;
    u32 supers_win;
    u32 abandoned_win;
    u32 unknown_win;
    u32 requested_win;
    u32 pkts_win;
    u64 abandoned_total;
    u64 unknown_total;
    u16 alive;
    u16 stalled;
    u16 carriers;
    bool p_valid; /* sent_win >= 1024 and supers_win >= 4 */
    bool r_valid; /* pkts_win > 0 */
    bool pause_idle;
    bool pause_carrier;
    bool pause_abandoned;
};

/* Prepare e for a shape with `shares` = n+m. A zero or out-of-range value is
 * replaced by 8, the 0.3.0 family. */
void tbt_fec_est_init(struct tbt_fec_est *e, u16 shares);

/* Record the carrier population. `total` is the configured carrier count,
 * `alive` those that pass tbt_fec_adapt_carrier_alive(), `stalled` those the
 * sender marks stalled. The estimator freezes when alive < 90% or when more
 * than 10% stall. */
void tbt_fec_est_carriers(struct tbt_fec_est *e, u16 alive, u16 stalled,
                          u16 total);

/* One group block. `real` is the number of DATA shares the block carries,
 * `miss_mask` bit i marks share i as needing repair, and `alive` is the
 * 256-bit carrier bitmap. Only shares with i < real and a live carrier enter
 * the sample. Call once per group, then tbt_fec_est_super() when the
 * super-block is complete. */
void tbt_fec_est_block(struct tbt_fec_est *e, u64 now_ns, u16 group, u16 real,
                       u32 miss_mask,
                       const u8 alive[TBT_FEC_ADAPT_BITMAP_BYTES]);

/* Fold the current super-block into the window and the EWMA. A super-block
 * with no alive-filtered share is dropped, not counted. */
void tbt_fec_est_super(struct tbt_fec_est *e, u64 now_ns);

/* A block whose real DATA count is unknown: count it as a quality sample and
 * do not estimate p from it. */
void tbt_fec_est_unknown(struct tbt_fec_est *e, u64 now_ns);

/* `shares` DATA shares were lost with an abandoned block. They do not enter p;
 * more than 1% of the window freezes the controller. */
void tbt_fec_est_abandoned(struct tbt_fec_est *e, u64 now_ns, u32 shares);

/* Add the deltas since the previous call: NACK-named DATA shares and DATA
 * packets sent. Both feed the 1 s r_fail window. */
void tbt_fec_est_fail(struct tbt_fec_est *e, u64 now_ns, u32 requested,
                      u32 pkts);

/* Read the current snapshot. Pure: it does not advance the window. A bucket
 * older than 1 s is ignored, so a stalled stream stops producing p. */
void tbt_fec_est_sample(const struct tbt_fec_est *e, u64 now_ns,
                        struct tbt_fec_est_out *o);

/* ------------------------------------------------------------------ *
 * Carrier liveness
 * ------------------------------------------------------------------ */

static inline bool tbt_fec_adapt_bitmap_test(
    const u8 bm[TBT_FEC_ADAPT_BITMAP_BYTES], u16 index)
{
    if (index >= TBT_FEC_ADAPT_CARRIERS)
        return false;
    return (bm[index >> 3] >> (index & 7)) & 1u;
}

static inline void tbt_fec_adapt_bitmap_set(u8 bm[TBT_FEC_ADAPT_BITMAP_BYTES],
                                            u16 index)
{
    if (index >= TBT_FEC_ADAPT_CARRIERS)
        return;
    bm[index >> 3] |= (u8)(1u << (index & 7));
}

static inline void tbt_fec_adapt_bitmap_clear(
    u8 bm[TBT_FEC_ADAPT_BITMAP_BYTES], u16 index)
{
    if (index >= TBT_FEC_ADAPT_CARRIERS)
        return;
    bm[index >> 3] &= (u8)~(1u << (index & 7));
}

/* A carrier is alive when it is up, not stopping, TCP-established, and a
 * record arrived within max(4 RTT, 4 s). A carrier that never received has
 * last_rx_ns == 0 and is not alive.
 *
 * The floor was 1 s. A 256-carrier block gives each carrier one share per
 * block, and the uplink's loss-repaired block interval approaches a second,
 * so ten percent of the population looked dead between blocks and the FBK
 * dropped P_VALID. Four seconds still names a dead carrier long before the
 * 15 s stall bound; the stalled flag already covers a live-but-not-moving
 * carrier. */
/* The floor is deliberately independent of cfg.keepalive_ms. PAD is a
 * sender cadence, not liveness evidence: when only PADs arrive no block has
 * been folded, so the estimator is already frozen by its idle rule
 * (TBT_FEC_EST_IDLE_NS / TBT_FEC_ADAPT_SCLC_IDLE_NS). A data-carrying
 * block or super-block places a share on every carrier, so last_rx refreshes
 * well inside four seconds. A peer that never emits PAD (the userspace
 * client) still refreshes the carriers this direction uses with FBK, ACK
 * and DATA records. Raising the floor to the keepalive cadence would only
 * delay real dead-carrier detection and would not help keepalive=0, which
 * disables PAD entirely. */
static inline bool tbt_fec_adapt_carrier_alive(bool up, bool stopping,
                                               bool established, u64 now_ns,
                                               u64 last_rx_ns, u32 rtt_us)
{
    u64 stale = (u64)rtt_us * 4000ull; /* 4 * rtt_us, in ns */
    u64 limit = 4000ull * 1000ull * 1000ull;

    if (stale > limit)
        limit = stale;
    if (!up || stopping || !established)
        return false;
    /* last_rx_ns == 0 means "never received"; at now_ns == 0 the unsigned
     * difference is zero and would otherwise make it alive. */
    if (!last_rx_ns || now_ns < last_rx_ns)
        return false;
    return now_ns - last_rx_ns < limit;
}

/* Freeze the controller when fewer than 90% of the carriers are alive or when
 * more than 10% are stalled. A zero population counts as frozen. */
static inline bool tbt_fec_adapt_carrier_pause(u16 alive, u16 stalled,
                                               u16 total)
{
    if (!total)
        return true;
    if ((u32)alive * 10 < (u32)total * 9)
        return true;
    if ((u32)stalled * 10 > (u32)total)
        return true;
    return false;
}

/* ------------------------------------------------------------------ *
 * Control law
 * ------------------------------------------------------------------ */

#define TBT_FEC_ADAPT_SEC_NS 1000000000ull
#define TBT_FEC_ADAPT_EVAL_NS (250ull * 1000ull * 1000ull) /* 250 ms */
#define TBT_FEC_ADAPT_UP_RUN 3
#define TBT_FEC_ADAPT_DOWN_RUN 6
#define TBT_FEC_ADAPT_UP_DWELL_NS (3ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_DOWN_DWELL_NS (10ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_DWELL_CAP_NS (30ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_REVERSE_NS (5ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_MIN_GAP_NS (3ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_RATE_MAX 10u
#define TBT_FEC_ADAPT_RATE_WIN_NS (60ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_HOLD_COUNT 3u
#define TBT_FEC_ADAPT_HOLD_WIN_NS (30ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_HOLD_NS (30ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_OSC_WIN_NS (10ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_PROBE_STABLE_NS (15ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_PROBE_OBSERVE_NS (10ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_PROBE_LOW_PPM 5000u
#define TBT_FEC_ADAPT_PROBE_HIGH_PPM 20000u
#define TBT_FEC_ADAPT_PROBE_BAN_NS (60ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_FAIL_RUN 2
#define TBT_FEC_ADAPT_FAIL_UP_PPM 20000u
#define TBT_FEC_ADAPT_FAIL_BAN_NS (30ull * TBT_FEC_ADAPT_SEC_NS)
#define TBT_FEC_ADAPT_HYST_PPM 5000u
#define TBT_FEC_ADAPT_DWELL_MUL 2u

enum tbt_fec_adapt_reason
{
    TBT_FEC_ADAPT_R_NONE = 0,
    TBT_FEC_ADAPT_R_BUSY,
    TBT_FEC_ADAPT_R_HOLD,
    TBT_FEC_ADAPT_R_PAUSED,
    TBT_FEC_ADAPT_R_WAIT,
    TBT_FEC_ADAPT_R_UP,
    TBT_FEC_ADAPT_R_DOWN,
    TBT_FEC_ADAPT_R_FAIL_UP,
    TBT_FEC_ADAPT_R_PROBE_DOWN,
    TBT_FEC_ADAPT_R_PROBE_REVERT,
    TBT_FEC_ADAPT_R_PROBE_ABORT,
    TBT_FEC_ADAPT_R_PROBE_COMMIT,
};

struct tbt_fec_adapt
{
    u8 cur;          /* current class, 0..5 */
    u8 busy;         /* a switch suggestion is in flight */
    u8 probe_active; /* cur is one class below probe_from */
    u8 probe_from;
    u8 pending_probe;
    u8 pending_to;
    u8 pending_reason;
    u8 last_dir;  /* 1 up, 2 down, 0 none */
    u8 prev_class;
    u8 up_run;
    u8 down_run;
    u8 fail_run;
    u8 probe_fail_run;
    u32 dwell_mul; /* 2 after an A->B->A within 10 s */
    u32 epoch;     /* last suggested epoch, 0 before the first */
    u64 last_eval_ns;
    u64 last_switch_ns;
    u64 probe_start_ns;
    u64 hold_until_ns;
    u64 no_probe_until[TBT_FEC_ADAPT_CLASSES];
    u64 sw[TBT_FEC_ADAPT_RATE_MAX];
    u32 sw_n;
    u32 sw_head;
};

struct tbt_fec_adapt_out
{
    u8 switch_now;
    u8 from;
    u8 to;
    u8 reason;
    u32 epoch; /* the epoch to put in FEC_CFG when switch_now */
    struct tbt_fec_shape shape;
};

/* Nominal class for a measured p, ppm: <=1% C0, <=3% C1, <=5% C2, <=10% C3,
 * <=20% C4, above C5. */
u8 tbt_fec_adapt_class_of_p(u32 p_ppm);

/* Class whose table row is exactly (n, m), or 0xff when the shape is not in
 * the 0.3.0 family. */
u8 tbt_fec_adapt_class_of_shape(u16 n, u16 m);

/* Fill s with the shape of `cls`; an out-of-range class is clamped to C3
 * (4/4), the safe start. */
void tbt_fec_adapt_shape(u8 cls, struct tbt_fec_shape *s);

/* Start at `cls` (clamped to C3 when out of range). The initial shape counts
 * as stable since the session began, so the first down-probe needs no switch
 * history. */
void tbt_fec_adapt_init(struct tbt_fec_adapt *a, u8 cls);

/* The controller state machine below is host-test-only: the module links the
 * estimator and helpers above, but no kernel data-plane caller evaluates a
 * suggestion (tbt_fec_adapt_sclc.c owns the live adaptive controller). */
#ifndef __KERNEL__

/* Evaluate one 250 ms tick. The first call in a tick returns a suggestion;
 * later calls return R_NONE until the next tick. A suggestion sets `busy`;
 * the caller must then call tbt_fec_adapt_applied() or
 * tbt_fec_adapt_abort(). A down-probe commit is reported as
 * TBT_FEC_ADAPT_R_PROBE_COMMIT with switch_now == 0. */
void tbt_fec_adapt_eval(struct tbt_fec_adapt *a,
                        const struct tbt_fec_est_out *o, u64 now_ns,
                        struct tbt_fec_adapt_out *out);

/* The suggested switch committed on both ends. Updates the class, dwell,
 * rate and probe state. */
void tbt_fec_adapt_applied(struct tbt_fec_adapt *a, u64 now_ns);

/* The suggested switch failed. The class is unchanged; a probe keeps
 * observing, a pending probe start is forgotten. */
void tbt_fec_adapt_abort(struct tbt_fec_adapt *a);

#endif /* !__KERNEL__ */

#endif /* _TBT_FEC_ADAPT_H */
