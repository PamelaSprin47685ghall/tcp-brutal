/* tbt_cc.c - the carrier congestion control.
 *
 * The carriers are the product's own TCP sockets between two machines the
 * operator controls. Their loss is random (a radio hop, a lab netem rule),
 * not congestion. A loss-based algorithm halves every carrier's window on
 * every loss event; 256 carriers do it together, and the aggregate falls far
 * below the link. This CC therefore:
 *
 *  - keeps a windowed maximum of the delivery rate. The window is ten RTTs,
 *    measured from the rate_sample the ACK hook provides. A tumbling window
 *    (one start time and one maximum) is the same max filter over the same
 *    span as a ten-slot ring, and it fits the per-socket CA private block.
 *
 *  - paces at max_bw / (1 - p). p is the loss the FEC feedback path measured
 *    for this sending direction; tbt_carrier.c pushes it in every 50 ms poll
 *    from the session's cc_loss_ppm. Module parameter loss_ppm pins p so a
 *    test can hold it constant.
 *
 *  - probes above that rate. A pure max_bw / (1 - p) pacer settles at its
 *    starting estimate and can lock at the seed window, because nothing ever
 *    asks for more than the last measurement. The pacer therefore multiplies
 *    the loss-compensated rate by a gain: 2.89x during startup (until a loss
 *    or three consecutive RTTs without 25% bandwidth growth), then an 8-RTT
 *    steady cycle whose first RTT probes at 1.25x and whose remaining seven
 *    RTTs cruise at 1.0x. tcp_rate_gen() averages delivery over a full RTT,
 *    so the probe phase must last a full RTT: a sub-RTT burst inside each
 *    cycle averages out to 1.03x and barely moves the estimate. The gain is
 *    tapered by the measured loss before it is applied, so on a lossy path
 *    the probe cannot multiply the loss compensation itself.
 *
 *  - reports ssthresh and undo_cwnd at or above max(2 * BDP, min_cwnd), and
 *    holds snd_cwnd at three BDP while startup paces at 2.89x, so the pacer
 *    is never window-starved. Setting clamp_enable=0 restores the reno
 *    answer and reno cwnd growth, so the same binary can A/B the clamp.
 *
 *  - keeps the idle hold honest: while tbt_carrier.c holds an idle carrier,
 *    cwnd_event ignores CA_EVENT_CWND_RESTART and keeps the bandwidth
 *    estimate, so the first burst after the hold uses the learned rate.
 *
 * API surface: only the exported kernel CC interface. tcp_register_
 * congestion_control()/tcp_unregister_congestion_control() (EXPORT_SYMBOL_GPL)
 * and the tcp_congestion_ops callbacks. The algorithm is attached only by
 * setsockopt(TCP_CONGESTION) on a carrier socket in tbt_carrier.c. It is
 * never written to net.ipv4.tcp_congestion_control, so it is never the system
 * default and never reaches a socket this module did not create.
 *
 * ACK-hook choice (compile time): this kernel's struct tcp_congestion_ops
 * declares
 *     void (*cong_control)(struct sock *sk, u32 ack, int flag,
 *                          const struct rate_sample *rs);
 * Older headers declare the two-argument cong_control(sk, rs). The two forms
 * cannot be told apart in C (both are valid function-pointer types), so the
 * Makefile reads the installed header and defines
 * TBT_CC_HAVE_CONG_CTRL_ACK only for the four-argument form. Without it the
 * CC uses the always-present cong_avoid + pkts_acked pair. pkts_acked hands
 * no rate sample, so the legacy path reads the last completed
 * delivered/interval sample that tcp_rate_gen() stores in the tcp_sock
 * (one ACK old, never the app-limited bubble). The same Makefile test also
 * detects the optional .init callback, so the private block is initialised
 * at attach on kernels that have it and lazily on kernels that do not.
 *
 * Built with -Werror; a kernel whose header changes either signature fails
 * the build instead of attaching a callback with the wrong arguments.
 */
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <net/tcp.h>

#include "tbt_cc.h"
#include "tbt_carrier_policy.h"

/* Ten RTTs is BBR's bandwidth-window length; the task fixes the same span. */
#define TBT_CC_BW_WINDOW_RTTS 10u
/* An RTT of zero cannot size a window; an RTT beyond one second is a stall,
 * not a path, and clamping it keeps the window finite without hiding it. */
#define TBT_CC_MIN_RTT_US 1000u
#define TBT_CC_MAX_RTT_US 1000000u
/* One MSS per 125 ms at the default 1448-byte MSS is about 92 kbit/s; 64 KiB/s
 * is a floor that never rate-limits a working path into a stall. */
#define TBT_CC_FLOOR_BPS 65536ULL
/* p >= 90% leaves no capacity to divide by; the clamp keeps the arithmetic
 * finite and is far beyond any link this carrier runs on. */
#define TBT_CC_MAX_LOSS_PPM 900000u
#define TBT_CC_MAGIC 0x74424343u /* "tBCC" */

/* Pacing gains in percent: 100 is exactly the loss-compensated rate. The
 * startup gain is BBR's 2.89x; the steady probe is one full RTT at 1.25x in
 * an 8-RTT cycle followed by seven RTTs at 1.0x. No constant is below 100. */
#define TBT_CC_GAIN_ONE 100u
#define TBT_CC_GAIN_STARTUP 289u
#define TBT_CC_GAIN_PROBE 125u
/* One steady cycle phase per smoothed RTT; phase 0 probes, 1..7 cruise. */
#define TBT_CC_PROBE_PHASES 8u
/* Startup ends after this many consecutive RTT rounds in which the windowed
 * max bandwidth grew by less than 25% (max_bw * 4 < full_bw * 5). */
#define TBT_CC_FULL_BW_CNT 3u

/* The rate arithmetic multiplies max_bw by 1e6, divides by at least 1e5
 * (p is clamped to 9e5), then multiplies by at most 289. Capping the input
 * here keeps every intermediate below 2^56 instead of wrapping; a rate this
 * high is far beyond sk_pacing_rate's 32-bit ceiling and is clamped there. */
#define TBT_CC_BW_CEIL_BPS (U64_MAX / 1000000ULL)

/* Pacing probe state machine, kept in the socket's CA private block.
 *
 * STARTUP: from the first paced rate sample until either a loss signal
 * (tbt_cc_ssthresh() or rate_sample.losses) or a bandwidth plateau, the
 * pacer asks for 2.89x. The plateau test runs once per smoothed RTT: the
 * windowed max must grow by at least 25% over the round's stored value, or a
 * counter increments; three consecutive rounds without that growth exit
 * startup. A time bound alone exits before a cold estimate fills the pipe;
 * a loss exits startup immediately, in the same ACK that reports it.
 *
 * STEADY: an 8-RTT cycle, one phase per smoothed RTT. Phase 0 uses 1.25x for
 * the whole RTT, phases 1..7 use 1.0x. tcp_rate_gen() averages delivered rate
 * over a full RTT, so the probe phase must span a full RTT for the rate
 * sample to see 1.25x at all; a short burst would average out to 3.125%. A
 * cycle idle for two or more RTTs restarts at phase 0; otherwise the phase
 * advances. No state is updated without an ACK, so there is no timer and
 * nothing to spin.
 *
 * Both phases taper the part of the gain above 1.0 by (1 - p): at p = 0 the
 * gains are unchanged, at p = 90% only a tenth of the probe room remains.
 * The taper is a non-negative integer term added to 100, so the gain can
 * never go below 1.0 or above 2.89. A real cwnd restart (route or path
 * change, not the idle hold) resets the machine to STARTUP.
 */
enum
{
    TBT_CC_PROBE_STARTUP = 0,
    TBT_CC_PROBE_STEADY = 1,
};

struct tbt_cc_state
{
    u64 max_bw_bps;           /* windowed-max delivery rate */
    u64 full_bw_bps;          /* max_bw at the last >= 25% growth round */
    u64 win_start_ns;         /* start of the current bandwidth window */
    u64 full_bw_round_ns;     /* start of the current startup plateau round */
    u64 pacing_bps;           /* last rate written, to avoid redundant writes */
    u64 probe_cycle_start_ns; /* start of the current steady probe phase */
    u32 magic;
    u32 rate_cap_bps;         /* tunnel_rate, 0 = the operator left it unset */
    u32 loss_ppm;             /* pushed by the FEC feedback path */
    u32 clamp_hits;           /* ssthresh calls the clamp actually raised */
    u32 srtt_us;              /* last usable RTT sample */
    u8 probe_state;           /* TBT_CC_PROBE_STARTUP or TBT_CC_PROBE_STEADY */
    u8 probe_phase;           /* steady cycle phase, 0..TBT_CC_PROBE_PHASES-1 */
    u8 full_bw_cnt;           /* RTT rounds without 25% bandwidth growth */
    bool startup_lost;        /* a loss signal ended (or would end) startup */
    bool hold;                /* the carrier's idle hold owns the window */
};

/* clamp_enable=0 is the A/B control: reno ssthresh/undo and reno growth, so a
 * loss collapses the window exactly as it would without this algorithm. */
static bool tbt_cc_clamp_enable = true;
module_param_named(clamp_enable, tbt_cc_clamp_enable, bool, 0644);
MODULE_PARM_DESC(clamp_enable,
                 "Keep snd_cwnd at or above max(2*BDP, min_cwnd) across a loss (default 1; 0=reno A/B)");

/* The minimum window in packets. It is a floor, not a target: the pacing rate
 * is the rate control, and the window only has to be wide enough for one
 * random loss not to empty the pipe. */
static uint tbt_cc_min_cwnd = 16;
module_param_named(min_cwnd, tbt_cc_min_cwnd, uint, 0644);
MODULE_PARM_DESC(min_cwnd, "Floor for snd_cwnd in packets when clamp_enable=1 (default 16)");

/* p in parts per million. -1 follows the FEC feedback measurement; a value in
 * 0..900000 pins it, so a netem run can hold the loss the pacer sees while
 * the wire loss is what the test set. */
static int tbt_cc_loss_ppm = -1;
module_param_named(loss_ppm, tbt_cc_loss_ppm, int, 0644);
MODULE_PARM_DESC(loss_ppm,
                 "Pin the pacing loss in ppm (0..900000); -1 = use the FEC feedback value");

static struct tcp_congestion_ops tbt_cc_ops;

static void tbt_cc_state_init(struct tbt_cc_state *st)
{
    memset(st, 0, sizeof(*st));
    st->magic = TBT_CC_MAGIC;
    st->probe_state = TBT_CC_PROBE_STARTUP;
}

static struct tbt_cc_state *tbt_cc_get(struct sock *sk)
{
    struct tbt_cc_state *st = inet_csk_ca(sk);

    if (unlikely(READ_ONCE(st->magic) != TBT_CC_MAGIC))
        tbt_cc_state_init(st);
    return st;
}

/* A caller outside the CC callbacks may only touch the block when this socket
 * really runs this algorithm. A socket that selected the name by hand is
 * fine; a socket with another CC never matches. */
static struct tbt_cc_state *tbt_cc_attached(struct sock *sk)
{
    const struct tcp_congestion_ops *ops;

    if (!sk)
        return NULL;
    ops = READ_ONCE(inet_csk(sk)->icsk_ca_ops);
    if (ops != &tbt_cc_ops)
        return NULL;
    if (READ_ONCE(((struct tbt_cc_state *)inet_csk_ca(sk))->magic) !=
        TBT_CC_MAGIC)
        return NULL;
    return inet_csk_ca(sk);
}

/* p for pacing. The parameter overrides the pushed measurement; the overlap
 * case is deliberate: a pinned p lets a test measure the clamp without the
 * FEC law moving underneath it. */
static u32 tbt_cc_p(const struct tbt_cc_state *st)
{
    int pinned = READ_ONCE(tbt_cc_loss_ppm);
    u32 p;

    if (pinned >= 0)
        p = pinned > (int)TBT_CC_MAX_LOSS_PPM ? TBT_CC_MAX_LOSS_PPM
                                              : (u32)pinned;
    else
        p = READ_ONCE(st->loss_ppm);
    return min_t(u32, p, TBT_CC_MAX_LOSS_PPM);
}

/* Remove the probe's amplification of loss compensation: at p = 0 the gain
 * is returned unchanged, at p = 90% only a tenth of the room above 1.0 is
 * left. The taper term is never negative, so the result is never < 1.0. */
static u32 tbt_cc_taper_gain(u32 gain, u32 p)
{
    u64 room;

    if (gain <= TBT_CC_GAIN_ONE)
        return TBT_CC_GAIN_ONE;
    room = (u64)(gain - TBT_CC_GAIN_ONE) * (1000000ULL - p);
    return TBT_CC_GAIN_ONE + (u32)div64_u64(room, 1000000ULL);
}

/* Pacing gain for this ACK, in percent. This is the whole state machine: it
 * advances the startup plateau window and the steady cycle, then returns; it
 * has no loop and every step is bounded by the constants above. A `now` that
 * is not ahead of the stored timestamp (impossible for ktime, handled
 * defensively) simply restarts the phase at `now` instead of underflowing the
 * subtraction. */
static u32 tbt_cc_pace_gain(struct tbt_cc_state *st, u64 now, u32 p)
{
    u32 srtt_us = READ_ONCE(st->srtt_us) ?: TBT_CC_MIN_RTT_US;
    u64 rtt_ns, start;
    u8 phase;

    srtt_us = clamp_t(u32, srtt_us, TBT_CC_MIN_RTT_US, TBT_CC_MAX_RTT_US);
    rtt_ns = (u64)srtt_us * NSEC_PER_USEC;

    if (READ_ONCE(st->probe_state) == TBT_CC_PROBE_STARTUP)
    {
        if (!READ_ONCE(st->startup_lost))
        {
            u64 round = READ_ONCE(st->full_bw_round_ns);

            /* One plateau round per smoothed RTT. */
            if (!round || now < round || now - round >= rtt_ns)
            {
                u64 cur = min_t(u64, READ_ONCE(st->max_bw_bps),
                                TBT_CC_BW_CEIL_BPS);
                u64 full = min_t(u64, READ_ONCE(st->full_bw_bps),
                                 TBT_CC_BW_CEIL_BPS);
                u8 cnt = READ_ONCE(st->full_bw_cnt);

                WRITE_ONCE(st->full_bw_round_ns, now);
                if (cur * 4 >= full * 5)
                {
                    /* The path answered with at least 25% more rate. */
                    WRITE_ONCE(st->full_bw_bps, cur);
                    WRITE_ONCE(st->full_bw_cnt, 0);
                }
                else if (cnt < TBT_CC_FULL_BW_CNT)
                {
                    WRITE_ONCE(st->full_bw_cnt, (u8)(cnt + 1));
                }
            }
            if (READ_ONCE(st->full_bw_cnt) < TBT_CC_FULL_BW_CNT)
                return tbt_cc_taper_gain(TBT_CC_GAIN_STARTUP, p);
        }
        /* A loss, or three RTT rounds without 25% growth: the path has
         * answered. Enter steady on phase 0 so the step down from 2.89x
         * lands on the full-RTT 1.25x probe. */
        WRITE_ONCE(st->probe_state, TBT_CC_PROBE_STEADY);
        WRITE_ONCE(st->probe_phase, 0);
        WRITE_ONCE(st->probe_cycle_start_ns, now);
    }

    start = READ_ONCE(st->probe_cycle_start_ns);
    phase = READ_ONCE(st->probe_phase);
    if (!start || now < start)
    {
        phase = 0;
        start = now;
    }
    else if (now - start >= rtt_ns)
    {
        /* One full RTT per phase, so tcp_rate_gen() measures the whole
         * 1.25x probe rate. An idle gap of two RTTs or more begins a new
         * cycle; otherwise the phase advances. */
        phase = now - start >= 2 * rtt_ns
                    ? 0
                    : (u8)((phase + 1) & (TBT_CC_PROBE_PHASES - 1));
        start = now;
    }
    WRITE_ONCE(st->probe_phase, phase);
    WRITE_ONCE(st->probe_cycle_start_ns, start);
    return tbt_cc_taper_gain(phase == 0 ? TBT_CC_GAIN_PROBE : TBT_CC_GAIN_ONE,
                             p);
}

/* Record one delivery-rate sample in the tumbling ten-RTT window. A sample
 * from an application-limited flight is not a capacity measurement and the
 * caller filters it out. */
static void tbt_cc_bw_sample(struct tbt_cc_state *st, u64 bw_bps, u32 rtt_us,
                             u64 now)
{
    u64 win_ns;

    if (!bw_bps)
        return;
    if (!rtt_us)
        rtt_us = READ_ONCE(st->srtt_us);
    rtt_us = clamp_t(u32, rtt_us, TBT_CC_MIN_RTT_US, TBT_CC_MAX_RTT_US);
    WRITE_ONCE(st->srtt_us, rtt_us);
    win_ns = (u64)rtt_us * NSEC_PER_USEC * TBT_CC_BW_WINDOW_RTTS;

    if (!st->win_start_ns || now < st->win_start_ns ||
        now - st->win_start_ns >= win_ns)
    {
        WRITE_ONCE(st->win_start_ns, now);
        WRITE_ONCE(st->max_bw_bps, bw_bps);
    }
    else if (bw_bps > READ_ONCE(st->max_bw_bps))
    {
        WRITE_ONCE(st->max_bw_bps, bw_bps);
    }
}

/* max_bw / (1 - p) times the probe gain, clamped to [floor, tunnel_rate]. A
 * zero tunnel_rate means the operator left the cap unset; sk_max_pacing_rate
 * (SO_MAX_PACING_RATE) then remains the ceiling, and zero there still means
 * no ceiling.
 *
 * Order matters: the loss compensation is applied first, then the gain, then
 * the operator's cap, and the store is saturated with min_t(..., ~0UL). The
 * input cap keeps bw * 1e6 from wrapping, the loss divisor is at least 1e5,
 * and the gain multiplication is at most 289x; the largest intermediate is
 * below 2^56. On a 32-bit kernel the final saturation is what prevents the
 * unsigned long store from truncating. */
static void tbt_cc_pace(struct sock *sk, struct tbt_cc_state *st)
{
    u64 bw = READ_ONCE(st->max_bw_bps);
    u64 cap = READ_ONCE(st->rate_cap_bps);
    u64 rate;
    u32 p, gain;

    if (!bw)
        return;
    bw = min_t(u64, bw, TBT_CC_BW_CEIL_BPS);
    p = tbt_cc_p(st);
    gain = tbt_cc_pace_gain(st, ktime_get_ns(), p);
    rate = div64_u64(bw * 1000000ULL, 1000000ULL - p);
    rate = div64_u64(rate * gain, 100ULL);
    if (!cap)
        cap = READ_ONCE(sk->sk_max_pacing_rate);
    if (cap && rate > cap)
        rate = cap;
    if (rate < TBT_CC_FLOOR_BPS && (!cap || cap >= TBT_CC_FLOOR_BPS))
        rate = TBT_CC_FLOOR_BPS;
    rate = min_t(u64, rate, ~0UL);
    if (rate == READ_ONCE(st->pacing_bps))
        return;
    WRITE_ONCE(st->pacing_bps, rate);
    WRITE_ONCE(sk->sk_pacing_rate, (unsigned long)rate);
    smp_store_release(&sk->sk_pacing_status, SK_PACING_NEEDED);
}

/* The pipe in packets, from the same windowed maximum the pacer uses. The
 * bandwidth is capped here to keep the multiply inside u64; the stored
 * max_bw_bps itself is not capped. */
static u64 tbt_cc_bdp_pkts(const struct tcp_sock *tp,
                           const struct tbt_cc_state *st)
{
    u32 mss = READ_ONCE(tp->mss_cache) ?: 1;
    u32 srtt = READ_ONCE(st->srtt_us) ?: TBT_CC_MIN_RTT_US;
    u64 bw = min_t(u64, READ_ONCE(st->max_bw_bps), TBT_CC_BW_CEIL_BPS);

    return div64_u64(bw * (u64)srtt, (u64)USEC_PER_SEC * mss);
}

/* max(2 * BDP, min_cwnd), in packets. BDP uses the same windowed maximum the
 * pacer uses, so one random loss cannot shrink the floor: the estimate only
 * falls when a whole ten-RTT window has been slower. */
static u32 tbt_cc_floor_pkts(const struct tcp_sock *tp,
                             const struct tbt_cc_state *st)
{
    u32 min_cwnd = READ_ONCE(tbt_cc_min_cwnd) ?: 1;
    u64 floor = 2 * tbt_cc_bdp_pkts(tp, st);

    if (floor < min_cwnd)
        floor = min_cwnd;
    return floor > U32_MAX ? U32_MAX : (u32)floor;
}

/* Grow the window. With the clamp on, the pacing rate is the rate control.
 * During startup the window is held at max(3 * BDP, cur + acked): the 2.89x
 * pacer needs room to fill the pipe while max_bw is still catching up, and
 * cur + acked keeps the window growing smoothly before the first sample
 * sizes the BDP. At steady state only the model floor, max(2 * BDP,
 * min_cwnd), is enforced. With the clamp off, grow like reno so the A/B run
 * really sees a loss-based algorithm. */
static void tbt_cc_cwnd(struct sock *sk, struct tcp_sock *tp,
                        struct tbt_cc_state *st, u32 acked)
{
    u32 cur = tcp_snd_cwnd(tp);
    u32 cap = READ_ONCE(tp->snd_cwnd_clamp);

    if (!READ_ONCE(tbt_cc_clamp_enable))
    {
        u32 inc;

        if (!acked || (cap && cur >= cap))
            return;
        if (tcp_in_slow_start(tp))
            inc = acked;
        else
            inc = max_t(u32, acked / cur, 1);
        tcp_snd_cwnd_set(tp, cap ? min(cur + inc, cap) : cur + inc);
        return;
    }
    if (READ_ONCE(st->probe_state) == TBT_CC_PROBE_STARTUP)
    {
        u32 min_cwnd = READ_ONCE(tbt_cc_min_cwnd) ?: 1;
        u64 want = max_t(u64, 3 * tbt_cc_bdp_pkts(tp, st),
                         (u64)cur + acked);

        if (want < min_cwnd)
            want = min_cwnd;
        if (cap && want > cap)
            want = cap;
        if (want > U32_MAX)
            want = U32_MAX;
        if ((u32)want > cur)
            tcp_snd_cwnd_set(tp, (u32)want);
        return;
    }
    {
        u32 floor = tbt_cc_floor_pkts(tp, st);

        if (cap && floor > cap)
            floor = cap;
        if (floor && cur < floor)
            tcp_snd_cwnd_set(tp, floor);
    }
}

static void tbt_cc_ack(struct sock *sk, const struct rate_sample *rs, u32 acked)
{
    struct tbt_cc_state *st = tbt_cc_get(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 mss = READ_ONCE(tp->mss_cache) ?: 1;

    if (rs && !rs->is_app_limited && rs->delivered > 0 &&
        rs->interval_us > 0)
    {
        u64 bw = div64_u64((u64)rs->delivered * mss * USEC_PER_SEC,
                           (u64)rs->interval_us);

        tbt_cc_bw_sample(st, bw, rs->rtt_us > 0 ? (u32)rs->rtt_us : 0,
                         ktime_get_ns());
    }
    /* A loss marked on this ACK ends startup before the pacer runs, so the
     * very next rate is the steady one. cong_control is the only path that
     * sees rate_sample.losses; the legacy path relies on ssthresh below. */
    if (rs && rs->losses > 0)
        WRITE_ONCE(st->startup_lost, true);
    tbt_cc_pace(sk, st);
    tbt_cc_cwnd(sk, tp, st, acked);
}

#ifdef TBT_CC_HAVE_CONG_CTRL_ACK
static void tbt_cc_cong_control(struct sock *sk, u32 ack, int flag,
                                const struct rate_sample *rs)
{
    (void)ack;
    (void)flag;
    tbt_cc_ack(sk, rs, rs ? rs->acked_sacked : 0);
}
#else
/* Legacy: no four-argument cong_control. cong_avoid is called once per ACK
 * with the number of newly acked segments, and pkts_acked hands the RTT of
 * the acked packet. pkts_acked/rtt is not a delivery rate: one ACK consumes
 * one or two segments of a full window, so dividing that count by a whole
 * RTT understates the rate by roughly the window (a 500-packet window reads
 * as 1/500 of its real rate). tcp_rate_gen() publishes the last completed
 * delivered/interval sample in the tcp_sock for callers without the rate
 * sample argument; this hook runs just before the next one is generated, so
 * the stored sample is one ACK old and still a real measurement. */
static void tbt_cc_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
    (void)ack;
    tbt_cc_ack(sk, NULL, acked);
}

static void tbt_cc_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
    struct tbt_cc_state *st = tbt_cc_get(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 mss = READ_ONCE(tp->mss_cache) ?: 1;
    u32 rtt = sample->rtt_us > 0 ? (u32)sample->rtt_us
                                 : READ_ONCE(st->srtt_us);
    u32 delivered = READ_ONCE(tp->rate_delivered);
    u32 interval = READ_ONCE(tp->rate_interval_us);

    /* An application-limited sample is a bubble in the pipe, not the path's
     * capacity; tcp_rate_gen() also refuses to replace the saved sample with
     * one. rate_app_limited is a bitfield written under the socket lock this
     * callback already holds. */
    if (tp->rate_app_limited || !delivered || !interval)
        return;
    {
        u64 bw = div64_u64((u64)delivered * mss * USEC_PER_SEC,
                           (u64)interval);

        tbt_cc_bw_sample(st, bw, rtt, ktime_get_ns());
    }
}
#endif

/* Return at or above max(2 * BDP, min_cwnd), never above the socket's clamp.
 * When the floor is not above the reno half, the reno half is returned
 * unchanged, so a genuinely congested path can still reduce to it. */
static u32 tbt_cc_ssthresh(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct tbt_cc_state *st = tbt_cc_get(sk);
    u32 cwnd = tcp_snd_cwnd(tp);
    u32 half = max_t(u32, cwnd >> 1, 2U);
    u32 floor, cap;

    /* Entering recovery is the loss signal every callback variant sees; it
     * ends startup even when the clamp itself is disabled for the A/B run. */
    WRITE_ONCE(st->startup_lost, true);
    if (!READ_ONCE(tbt_cc_clamp_enable))
        return tcp_reno_ssthresh(sk);
    floor = tbt_cc_floor_pkts(tp, st);
    cap = READ_ONCE(tp->snd_cwnd_clamp);
    if (cap && floor > cap)
        floor = cap;
    if (floor > half)
    {
        st->clamp_hits++;
        return floor;
    }
    return half;
}

static u32 tbt_cc_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct tbt_cc_state *st = tbt_cc_get(sk);
    u32 floor, cap;

    if (!READ_ONCE(tbt_cc_clamp_enable))
        return tcp_reno_undo_cwnd(sk);
    floor = tbt_cc_floor_pkts(tp, st);
    cap = READ_ONCE(tp->snd_cwnd_clamp);
    if (cap && floor > cap)
        floor = cap;
    return max(tcp_snd_cwnd(tp), floor);
}

/* A real cwnd restart (a route or path change) invalidates the bandwidth
 * window. A restart delivered while the idle hold owns the carrier does not:
 * the hold exists precisely so the learned window and rate survive idleness,
 * and the first burst after it must not start cold. */
static void tbt_cc_cwnd_event(struct sock *sk, enum tcp_ca_event ev)
{
    struct tbt_cc_state *st = tbt_cc_get(sk);

    if (ev != CA_EVENT_CWND_RESTART || READ_ONCE(st->hold))
        return;
    WRITE_ONCE(st->win_start_ns, 0);
    WRITE_ONCE(st->max_bw_bps, 0);
    /* The measurement is gone with the window, so the next samples probe
     * like a fresh path: startup high gain until three RTT rounds without
     * 25% growth or a loss. The idle hold is excluded above, exactly like
     * the bandwidth window. */
    WRITE_ONCE(st->full_bw_bps, 0);
    WRITE_ONCE(st->full_bw_round_ns, 0);
    WRITE_ONCE(st->full_bw_cnt, 0);
    WRITE_ONCE(st->probe_state, TBT_CC_PROBE_STARTUP);
    WRITE_ONCE(st->startup_lost, false);
    WRITE_ONCE(st->probe_phase, 0);
    WRITE_ONCE(st->probe_cycle_start_ns, 0);
}

static void tbt_cc_init_sock(struct sock *sk)
{
    tbt_cc_state_init(inet_csk_ca(sk));
}

void tbt_cc_set_tunnel(struct sock *sk, u32 rate_cap_bps, u32 loss_ppm)
{
    const struct tcp_congestion_ops *ops;
    struct tbt_cc_state *st;

    if (!sk)
        return;
    ops = READ_ONCE(inet_csk(sk)->icsk_ca_ops);
    if (ops != &tbt_cc_ops)
        return;
    st = tbt_cc_get(sk);
    WRITE_ONCE(st->rate_cap_bps, rate_cap_bps);
    WRITE_ONCE(st->loss_ppm, min(loss_ppm, 1000000u));
}

void tbt_cc_set_hold(struct sock *sk, bool hold)
{
    const struct tcp_congestion_ops *ops;
    struct tbt_cc_state *st;

    if (!sk)
        return;
    ops = READ_ONCE(inet_csk(sk)->icsk_ca_ops);
    if (ops != &tbt_cc_ops)
        return;
    /* Lazy-init as set_tunnel() does: a carrier that entered the hold before
     * its first data ACK still has an all-zero private block. */
    st = tbt_cc_get(sk);
    WRITE_ONCE(st->hold, hold);
}

bool tbt_cc_snapshot(struct sock *sk, struct tbt_cc_info *out)
{
    struct tbt_cc_state *st = tbt_cc_attached(sk);

    if (!st)
        return false;
    out->max_bw_bps = READ_ONCE(st->max_bw_bps);
    out->pacing_bps = READ_ONCE(st->pacing_bps);
    out->clamp_hits = READ_ONCE(st->clamp_hits);
    out->loss_ppm = tbt_cc_p(st);
    out->hold = READ_ONCE(st->hold);
    return true;
}

/* The name this module registers. It must be its own: carrier_cc defaults to
 * bbr, and bbr is already registered in every kernel that ships tcp_bbr, so
 * registering the parameter's name failed with -EBUSY and the whole module
 * refused to load. carrier_cc still selects what a carrier socket asks for. */
#define TBT_CC_NAME "tbtcc"

static struct tcp_congestion_ops tbt_cc_ops = {
#ifdef TBT_CC_HAVE_CONG_CTRL_ACK
    .cong_control = tbt_cc_cong_control,
#else
    .cong_avoid = tbt_cc_cong_avoid,
    .pkts_acked = tbt_cc_pkts_acked,
#endif
    .ssthresh = tbt_cc_ssthresh,
    .undo_cwnd = tbt_cc_undo_cwnd,
    .cwnd_event = tbt_cc_cwnd_event,
#ifdef TBT_CC_HAVE_INIT
    .init = tbt_cc_init_sock,
#endif
    .name = TBT_CC_NAME,
    .owner = THIS_MODULE,
};

int tbt_cc_module_init(void)
{
    BUILD_BUG_ON(sizeof(struct tbt_cc_state) > ICSK_CA_PRIV_SIZE);
    BUILD_BUG_ON(sizeof(TBT_CC_NAME) > TCP_CA_NAME_MAX);
    return tcp_register_congestion_control(&tbt_cc_ops);
}

void tbt_cc_module_exit(void)
{
    tcp_unregister_congestion_control(&tbt_cc_ops);
}
