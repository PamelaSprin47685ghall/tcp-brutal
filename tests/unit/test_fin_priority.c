/* test_fin_priority.c - host test for the close-path FIN priority.
 *
 * WHAT THIS IS
 *   The exact rules tbt_tx.c runs, compiled from src/kernel/tbt_tx_acct.h:
 *
 *     - while a stream is closing, one pump pass admits at most 32 DATA
 *       frames and 8 other non-FIN frames, and always admits FIN;
 *     - the FIN retry deadline is the close timer, not the FBK/PAD
 *       TBT_CTL_MAX_RETRIES bound;
 *     - the close timer holds for FEC tail repair (no cap) and for a peer
 *       credit wait (inside the 30 s hard cap), and fires otherwise.
 *
 *   The model pass mirrors the pump's order: FIN is moved ahead of the FIFO,
 *   then the quota is applied. With 64 DATA frames ahead of a FIN, the FIN
 *   must still be written in that pass.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "tbt_tx_acct.h"

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

static void case_quota(void)
{
    u32 data = 0, ctl = 0;
    unsigned int i;

    /* No close: every frame is admitted. */
    for (i = 0; i < 100; i++)
        ok(tbt_tx_close_frame_allowed(false, TBT_F_DATA, &data, &ctl),
           "quota: no close admits data");

    /* Closing: exactly 32 DATA frames per pass. */
    data = ctl = 0;
    for (i = 0; i < TBT_TX_CLOSE_DATA_MAX; i++)
        ok(tbt_tx_close_frame_allowed(true, TBT_F_DATA, &data, &ctl),
           "quota: first 32 data admitted");
    ok(!tbt_tx_close_frame_allowed(true, TBT_F_DATA, &data, &ctl),
       "quota: 33rd data refused");
    ok(tbt_tx_close_frame_allowed(true, TBT_F_FIN, &data, &ctl),
       "quota: FIN admitted after the data cap");
    ok(!tbt_tx_close_frame_allowed(true, TBT_F_DATA, &data, &ctl),
       "quota: FIN did not consume the data quota");

    /* Other control: exactly 8 per pass. */
    data = ctl = 0;
    for (i = 0; i < TBT_TX_CLOSE_CTL_MAX; i++)
        ok(tbt_tx_close_frame_allowed(true, TBT_F_FBK, &data, &ctl),
           "quota: first 8 ctl admitted");
    ok(!tbt_tx_close_frame_allowed(true, TBT_F_FBK, &data, &ctl),
       "quota: 9th ctl refused");
    ok(tbt_tx_close_frame_allowed(true, TBT_F_FIN, &data, &ctl),
       "quota: FIN admitted after the ctl cap");
}

static void case_fin_retry(void)
{
    ok(!tbt_tx_fin_retry_allowed(100, 0, 50),
       "retry: no close timer means no FIN retry");
    ok(tbt_tx_fin_retry_allowed(100, 60, 50),
       "retry: inside the close bound");
    ok(tbt_tx_fin_retry_allowed(110, 60, 50),
       "retry: exactly at the close bound");
    ok(!tbt_tx_fin_retry_allowed(111, 60, 50),
       "retry: past the close bound");
    ok(!tbt_tx_fin_retry_allowed(59, 60, 50),
       "retry: clock before the close request");
}

static void case_close_hold(void)
{
    ok(!tbt_tx_close_hold_fec(false, true, true, 100, 30),
       "hold: non-FEC never holds");
    ok(tbt_tx_close_hold_fec(true, true, false, 100, 30),
       "hold: repair active holds without a cap");
    ok(tbt_tx_close_hold_fec(true, false, true, 30, 30),
       "hold: credit wait at the cap holds");
    ok(!tbt_tx_close_hold_fec(true, false, true, 31, 30),
       "hold: credit wait past the cap fires");
    ok(!tbt_tx_close_hold_fec(true, false, false, 10, 30),
       "hold: no repair and no credit fires");
}

static void case_repair_bound(void)
{
    u64 max = TBT_TX_FEC_REPAIR_MAX_NS;

    ok(tbt_tx_fec_repair_holds(true, true, 30ULL * 1000000000ULL, max),
       "repair: 30 s still holds");
    ok(tbt_tx_fec_repair_holds(true, true, 60ULL * 1000000000ULL, max),
       "repair: 60 s still holds");
    ok(tbt_tx_fec_repair_holds(true, true, max, max),
       "repair: exactly 90 s still holds");
    ok(!tbt_tx_fec_repair_holds(true, true, max + 1, max),
       "repair: past 90 s kills");
    ok(!tbt_tx_fec_repair_holds(true, false, 10, max),
       "repair: inactive repair does not hold");
    ok(!tbt_tx_fec_repair_holds(false, true, 10, max),
       "repair: non-FEC behaviour unchanged");
}

static void case_aggregate_window(void)
{
    u64 floor = 32ULL * 8 * TBT_PAYLOAD_MAX; /* 2 MiB */
    u64 cap = 8ULL * 1024 * 1024;
    u64 win;

    /* 32 carriers, k x BDP = 386,814, per-carrier 12,088, sum(snd_wnd) =
     * 4,349,800: the aggregate must dominate the old 378 KB window. */
    win = tbt_tx_window_auto(386814, 4349800, 12088, 32, 32, floor, cap);
    ok(win == 4349800, "window: sum(snd_wnd) dominates k x BDP");
    ok(win > 378 * 1024, "window: 32 carriers well above the old 378 KB");

    /* No snd_wnd sample: n_car x per-carrier BDP. */
    win = tbt_tx_window_auto(0, 0, 100000, 32, 32, floor, cap);
    ok(win == 3200000, "window: n_car x per-carrier BDP");

    /* The reorder budget caps an over-large aggregate. */
    win = tbt_tx_window_auto(0, 0, 1000000, 32, 32, floor, cap);
    ok(win == cap, "window: reorder budget caps");

    /* Bootstrap floor scales with the carrier count. */
    win = tbt_tx_window_auto(1000, 0, 0, 32, 0, floor, cap);
    ok(win == floor, "window: 32-carrier bootstrap floor");
    ok(win == 32ULL * 8 * TBT_PAYLOAD_MAX, "window: floor is 2 MiB");

    /* One carrier keeps the 1 MiB floor. */
    win = tbt_tx_window_auto(1000, 0, 0, 1, 0, 1 << 20, cap);
    ok(win == 1 << 20, "window: single-carrier floor");
}

static void case_car_rate(void)
{
    ok(tbt_tx_car_rate(5000000, 100000, 0, 0) == 5000000,
       "rate: BBR delivery sample wins");
    ok(tbt_tx_car_rate(0, 166727, 136000, 200000) ==
           200000ULL * 1000000 / 166727,
       "rate: cwnd/srtt fallback");
    ok(tbt_tx_car_rate(0, 166727, 0, 200000) ==
           200000ULL * 1000000 / 166727,
       "rate: cwnd fallback without snd_wnd");
    ok(tbt_tx_car_rate(0, 166727, 136000, 0) ==
           136000ULL * 1000000 / 166727,
       "rate: snd_wnd fallback without cwnd");
    ok(tbt_tx_car_rate(0, 0, 136000, 200000) == 0,
       "rate: no RTT means no fallback");
    ok(tbt_tx_car_rate(0, 166727, 0, 0) == 0,
       "rate: no window means no fallback");
}

static void case_reorder_floor(void)
{
    u64 cap256 = tbt_tx_reorder_floor(256);

    ok(tbt_tx_reorder_floor(1) == 8ULL * 1024 * 1024,
       "reorder: one carrier keeps 8 MiB");
    ok(tbt_tx_reorder_floor(32) == 8ULL * 1024 * 1024,
       "reorder: 32 carriers keep 8 MiB");
    ok(tbt_tx_reorder_floor(256) == 16ULL * 1024 * 1024,
       "reorder: 256 carriers grow to 16 MiB");
    ok(tbt_tx_reorder_floor(8192) == TBT_TX_REORDER_MAX,
       "reorder: capped at 256 MiB");

    /* 256 carriers: the 15 MiB aggregate is no longer capped at 8 MiB. */
    {
        u64 win = tbt_tx_window_auto(0, 15ULL * 1024 * 1024, 0, 256, 256,
                                     16ULL * 1024 * 1024, cap256);

        ok(win == 16ULL * 1024 * 1024,
           "window: 256 carriers reach the 16 MiB floor");
        ok(win > 8ULL * 1024 * 1024, "window: above the old 8 MiB cap");
    }
}

static void case_pump_idle(void)
{
    ok(tbt_tx_pump_idle(true, 0, 0), "pump: FEC empty queue is idle");
    ok(!tbt_tx_pump_idle(true, 0, 1),
       "pump: a queued FEC control frame is not idle");
    ok(!tbt_tx_pump_idle(true, 1, 0), "pump: queued FEC data is not idle");
    ok(!tbt_tx_pump_idle(false, 0, 0), "pump: non-FEC is never idle");
}

struct frame
{
    u8 type;
    bool written;
};

/* One pump pass: FIN first, then the FIFO under the close quota. */
static void model_pass(struct frame *q, int n, bool close_priority,
                       bool *fin_written, int *data_written, int *ctl_written)
{
    struct frame *order[80];
    int m = 0, i;
    u32 data = 0, ctl = 0;

    if (close_priority)
        for (i = 0; i < n; i++)
            if (q[i].type == TBT_F_FIN)
                order[m++] = &q[i];
    for (i = 0; i < n; i++)
        if (!close_priority || q[i].type != TBT_F_FIN)
            order[m++] = &q[i];
    for (i = 0; i < m; i++)
    {
        if (!tbt_tx_close_frame_allowed(close_priority, order[i]->type,
                                        &data, &ctl))
            continue;
        order[i]->written = true;
        if (order[i]->type == TBT_F_FIN)
            *fin_written = true;
        else if (order[i]->type == TBT_F_DATA)
            (*data_written)++;
        else
            (*ctl_written)++;
    }
}

static void case_fin_not_starved(void)
{
    struct frame q[66];
    bool fin = false;
    int data = 0, ctl = 0, i;

    for (i = 0; i < 64; i++)
    {
        q[i].type = TBT_F_DATA;
        q[i].written = false;
    }
    q[64].type = TBT_F_FIN;
    q[64].written = false;
    q[65].type = TBT_F_FBK;
    q[65].written = false;

    model_pass(q, 66, true, &fin, &data, &ctl);
    ok(fin, "model: FIN written despite 64 data frames ahead");
    ok(data == (int)TBT_TX_CLOSE_DATA_MAX, "model: data quota is 32");
    ok(ctl == 1, "model: the FBK behind the quota is still admitted");
}

static void case_no_close(void)
{
    struct frame q[2] = {
        {.type = TBT_F_DATA, .written = false},
        {.type = TBT_F_FIN, .written = false},
    };
    bool fin = false;
    int data = 0, ctl = 0;

    model_pass(q, 2, false, &fin, &data, &ctl);
    ok(fin && data == 1, "model: no close writes every frame");
}

int main(void)
{
    case_quota();
    case_fin_retry();
    case_close_hold();
    case_repair_bound();
    case_aggregate_window();
    case_car_rate();
    case_reorder_floor();
    case_pump_idle();
    case_fin_not_starved();
    case_no_close();

    printf("test_fin_priority: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
