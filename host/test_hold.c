/* test_hold.c - host state-machine test for the peripheral BBR hold.
 *
 * WHAT THIS IS
 *   The exact predicates tbt_carrier.c uses, compiled from tbt_tx_acct.h:
 *
 *     - enter only when hold_enable is set, the carrier is idle in tbtun
 *       (no ARQ/FEC data; a queued PAD does not count), TCP has no bytes in
 *       flight, CA is Open, no loss, and the idle interval has lasted
 *       enter_ns;
 *     - snapshot snd_cwnd and sk_pacing_rate at entry, bounded by snd_wnd,
 *       tcp_snd_cwnd_clamp, sk_sndbuf/mss and sk_max_pacing_rate;
 *     - keep only while there is no real data, loss, window shrink or
 *       deadline expiry, and write the snapshot back on every kept poll;
 *     - exit on real data, loss, window shrink or the 10 s idle cap.
 *     - a write is skipped, not cancelled, while only a PAD is in flight;
 *       it exits only when the hold is gone or CA left Open.
 *   The hold never touches a BBR model field (delivered, rate_delivered,
 *   mode); every non-PAD writer clears hold_active before it queues bytes,
 *   so the first real ACK owns the next decision.
 *
 *   The model mirrors the poll: idle -> wait enter_ns -> active -> exit.
 *   It does not execute the kernel; the predicates are the shared code.
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

#define MS 1000000ULL

static void case_enter(void)
{
    u64 enter = 200 * MS;

    ok(!tbt_hold_enter(false, true, true, true, false, false,
                       100, 300 * MS, enter),
       "enter: disabled");
    ok(!tbt_hold_enter(true, true, false, true, false, false,
                       100, 300 * MS, enter),
       "enter: tbtun not idle");
    ok(!tbt_hold_enter(true, true, true, false, false, false,
                       100, 300 * MS, enter),
       "enter: CA not Open");
    ok(!tbt_hold_enter(true, true, true, true, true, false,
                       100, 300 * MS, enter),
       "enter: loss");
    ok(!tbt_hold_enter(true, true, true, true, false, true,
                       100, 300 * MS, enter),
       "enter: recent loss");
    ok(!tbt_hold_enter(true, false, true, true, false, false,
                       100, 300 * MS, enter),
       "enter: bytes in flight refuse the snapshot");
    ok(!tbt_hold_enter(true, true, true, true, false, false,
                       0, 300 * MS, enter),
       "enter: no idle start");
    ok(!tbt_hold_enter(true, true, true, true, false, false,
                       100, 150 * MS, enter),
       "enter: before the enter delay");
    ok(tbt_hold_enter(true, true, true, true, false, false,
                      100, 300 * MS, enter),
       "enter: at the enter delay with an empty send path");
}

static void case_keep(void)
{
    u64 until = 10 * 1000 * MS;

    ok(!tbt_hold_keep(false, false, false, false, 100, until),
       "keep: inactive");
    ok(tbt_hold_keep(true, false, false, false, 100, until),
       "keep: active and idle");
    ok(!tbt_hold_keep(true, true, false, false, 100, until),
       "keep: real data exits");
    ok(!tbt_hold_keep(true, false, true, false, 100, until),
       "keep: loss exits");
    ok(!tbt_hold_keep(true, false, false, true, 100, until),
       "keep: window shrink exits");
    ok(!tbt_hold_keep(true, false, false, false, until + 1, until),
       "keep: deadline exits");
    ok(tbt_hold_keep(true, false, false, false, until, until),
       "keep: exactly at the deadline");
    ok(tbt_hold_keep(true, false, false, false, 100, 0),
       "keep: zero deadline means no cap");
}

static void case_save(void)
{
    ok(tbt_hold_cwnd_save(0, 100, 64, 838861, 1448) == 0,
       "save: a zero cwnd has no snapshot");
    ok(tbt_hold_cwnd_save(100, 0, 0, 0, 1448) == 100,
       "save: no bound keeps the window");
    ok(tbt_hold_cwnd_save(100, 32, 0, 0, 1448) == 32,
       "save: the peer window bounds the snapshot");
    ok(tbt_hold_cwnd_save(100, 0, 64, 0, 1448) == 64,
       "save: snd_cwnd_clamp bounds the snapshot");
    ok(tbt_hold_cwnd_save(1000, 0, 0, 838861, 1448) == 579,
       "save: sk_sndbuf/mss bounds the snapshot");
    ok(tbt_hold_cwnd_save(1000, 0, 0, 1000, 0) == 1000,
       "save: mss==0 is treated as 1");
    ok(tbt_hold_cwnd_save(100, 32, 16, 0, 1448) == 16,
       "save: the tightest bound wins");
    ok(tbt_hold_cwnd_save(1, 0, 0, 0, 1448) == 1,
       "save: one packet stays one packet");
    ok(tbt_hold_pacing_save(1000, 0) == 1000,
       "save: no pacing ceiling keeps the rate");
    ok(tbt_hold_pacing_save(1000, 500) == 500,
       "save: sk_max_pacing_rate bounds the rate");
}

static void case_restore(void)
{
    ok(tbt_hold_cwnd_restore(0, 64) == 0,
       "restore: no snapshot writes nothing");
    ok(tbt_hold_cwnd_restore(100, 0) == 100,
       "restore: no clamp keeps the snapshot");
    ok(tbt_hold_cwnd_restore(100, 64) == 64,
       "restore: a new clamp still bounds the write");
    ok(tbt_hold_cwnd_restore(100, 200) == 100,
       "restore: a loose clamp changes nothing");
    ok(tbt_hold_cwnd_restore(1, 0) == 1,
       "restore: one packet stays one packet");
}

static void case_apply(void)
{
    ok(tbt_hold_apply_gate(true, true, true, 0) == TBT_HOLD_APPLY_OK,
       "apply: idle carrier writes");
    ok(tbt_hold_apply_gate(true, true, true, 1) == TBT_HOLD_APPLY_SKIP,
       "apply: PAD in flight skips, does not cancel");
    ok(tbt_hold_apply_gate(true, true, false, 1) == TBT_HOLD_APPLY_SKIP,
       "apply: unacked PAD skips");
    ok(tbt_hold_apply_gate(false, true, true, 0) == TBT_HOLD_APPLY_EXIT,
       "apply: a non-PAD write cancelled the hold");
    ok(tbt_hold_apply_gate(true, false, true, 0) == TBT_HOLD_APPLY_EXIT,
       "apply: CA left Open exits");
}

struct session_q
{
    u32 tx_queued;
    u32 pad_queued; /* the kernel's ctl_queued PAD slot; never ctl_real */
    u32 ctl_real;
};

static void pad_enqueue(struct session_q *q)
{
    q->pad_queued++;
}

static void pad_finish(struct session_q *q)
{
    if (q->pad_queued)
        q->pad_queued--;
}

static void case_pad_session(void)
{
    struct session_q q = {0};
    bool real_data[4];
    int i;

    /* One poll pass: carrier 0 queues its PAD, then every carrier runs
     * hold_check. The PAD must not make any of them look busy. */
    pad_enqueue(&q);
    for (i = 0; i < 4; i++)
        real_data[i] = !tbt_hold_session_idle(q.tx_queued, q.ctl_real);
    ok(!real_data[0] && !real_data[1] && !real_data[2] && !real_data[3],
       "pad: a queued PAD leaves every carrier idle in the same pass");
    ok(tbt_hold_keep(true, real_data[3], false, false, 100,
                     10 * 1000 * MS),
       "pad: a later carrier keeps the hold in the same pass");

    /* A real DATA frame queued in the same pass is still real data. */
    q.tx_queued = 8192;
    ok(!tbt_hold_session_idle(q.tx_queued, q.ctl_real),
       "pad: real data in the same pass is not idle");

    /* A non-PAD control frame is real work as well. */
    q.tx_queued = 0;
    q.ctl_real = 1;
    ok(!tbt_hold_session_idle(q.tx_queued, q.ctl_real),
       "pad: a non-PAD control frame is not idle");

    /* Every real frame drains and the PAD finishes: idle again. */
    q.ctl_real = 0;
    pad_finish(&q);
    ok(tbt_hold_session_idle(q.tx_queued, q.ctl_real),
       "pad: finishing the PAD restores idle");
    ok(q.tx_queued == 0 && q.ctl_real == 0 && q.pad_queued == 0,
       "pad: accounting returns to zero");
}

struct model
{
    bool active;
    u64 since;
    u64 until;
    u32 cwnd;          /* the live tcp_sock value */
    unsigned long pacing;
    u32 hold_cwnd;     /* the snapshot tbt_car_hold_apply() writes */
    unsigned long hold_pacing;
    u32 clamp;
    u32 wnd_pkts;
    u32 sndbuf;
    u32 mss;
    unsigned long max_pacing;
    bool pad_inflight;
};

static void model_apply(struct model *m)
{
    u32 cwnd = tbt_hold_cwnd_restore(m->hold_cwnd, m->clamp);
    enum tbt_hold_apply gate;

    if (!cwnd)
        return;
    gate = tbt_hold_apply_gate(m->active, true, !m->pad_inflight,
                               m->pad_inflight ? 1u : 0u);
    if (gate == TBT_HOLD_APPLY_EXIT)
    {
        m->active = false;
        m->since = 0;
        return;
    }
    if (gate != TBT_HOLD_APPLY_OK)
        return;
    m->cwnd = cwnd;
    if (m->hold_pacing)
        m->pacing = m->hold_pacing;
}

static void model_step(struct model *m, u64 now, bool idle, bool no_inflight,
                       bool loss, bool real_data, bool wnd_shrunk)
{
    if (m->active)
    {
        if (!tbt_hold_keep(true, real_data, loss, wnd_shrunk, now, m->until))
        {
            m->active = false;
            m->since = 0;
            return;
        }
        model_apply(m);
        return;
    }
    if (!tbt_hold_enter(true, no_inflight, idle, true, loss, false,
                        m->since, now, 200 * MS))
    {
        if (!real_data && idle && !loss && no_inflight)
        {
            if (!m->since)
                m->since = now;
        }
        else
        {
            m->since = 0;
        }
        return;
    }
    m->hold_cwnd = tbt_hold_cwnd_save(m->cwnd, m->wnd_pkts, m->clamp,
                                      m->sndbuf, m->mss);
    if (!m->hold_cwnd)
        return;
    m->hold_pacing = tbt_hold_pacing_save(m->pacing, m->max_pacing);
    m->active = true;
    m->until = now + 10 * 1000 * MS;
    model_apply(m);
}

/* A live tcp_sock always has a nonzero window; the snapshot of a zero cwnd is
 * "no snapshot", which the save case pins separately. */
static struct model model_new(void)
{
    struct model m = {0};

    m.cwnd = 10;
    m.mss = 1448;
    return m;
}

static void case_state(void)
{
    struct model m = model_new();

    model_step(&m, 1, true, true, false, false, false);
    ok(m.since == 1 && !m.active, "state: idle starts the enter timer");
    model_step(&m, 100 * MS, true, true, false, false, false);
    ok(!m.active, "state: still inside the enter delay");
    model_step(&m, 200 * MS + 1, true, true, false, false, false);
    ok(m.active, "state: enters after 200 ms");
    model_step(&m, 1 * 1000 * MS, true, true, false, false, false);
    ok(m.active, "state: holds while idle");
    model_step(&m, 11 * 1000 * MS, true, true, false, false, false);
    ok(!m.active, "state: 10 s idle cap exits");

    m = model_new();
    model_step(&m, 1, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, false, false, false);
    ok(m.active, "state: re-enters");
    model_step(&m, 300 * MS, true, true, false, true, false);
    ok(!m.active, "state: real data exits");
    model_step(&m, 400 * MS, true, true, false, true, false);
    ok(!m.active && m.since == 0, "state: stays out while real data");

    m = model_new();
    model_step(&m, 1, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, false, false, false);
    model_step(&m, 300 * MS, true, true, true, false, false);
    ok(!m.active, "state: loss exits");

    m = model_new();
    model_step(&m, 1, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, false, false, false);
    model_step(&m, 300 * MS, true, true, false, false, true);
    ok(!m.active, "state: window shrink exits");
}

/* The save/restore loop as the poll runs it: an idle carrier snapshots its
 * window, BBR collapses cwnd and pacing while nothing is in flight, and the
 * next poll writes the snapshot back. A PAD in flight skips the write; a real
 * write cancels the hold before it queues, so no later poll touches BBR's
 * window. */
static void case_restore_loop(void)
{
    struct model m = {0};

    m.cwnd = 500;
    m.pacing = 50 * 1000 * 1000;
    m.sndbuf = 838861;
    m.mss = 1448;
    model_step(&m, 1, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, false, false, false);
    ok(m.active && m.hold_cwnd == 500,
       "restore: entry snapshots the idle window");
    ok(m.hold_pacing == 50 * 1000 * 1000,
       "restore: entry snapshots the idle pacing rate");

    /* BBR idle restart or ProbeRTT collapses both fields. */
    m.cwnd = 4;
    m.pacing = 1 * 1000 * 1000;
    model_step(&m, 300 * MS, true, true, false, false, false);
    ok(m.cwnd == 500, "restore: the next idle poll restores cwnd");
    ok(m.pacing == 50 * 1000 * 1000,
       "restore: the next idle poll restores pacing");

    /* A keepalive PAD is in flight: the gate skips, the hold stays. */
    m.cwnd = 4;
    m.pad_inflight = true;
    model_step(&m, 400 * MS, true, false, false, false, false);
    ok(m.active && m.cwnd == 4,
       "restore: a PAD in flight skips the write");

    m.pad_inflight = false;
    model_step(&m, 500 * MS, true, true, false, false, false);
    ok(m.cwnd == 500,
       "restore: the next poll after the PAD writes again");

    /* A real FEC/ARQ writer clears hold_active before it queues bytes. */
    m.active = false;
    m.since = 0;
    m.cwnd = 4;
    model_step(&m, 600 * MS, true, true, false, false, false);
    ok(!m.active && m.cwnd == 4,
       "restore: a real write leaves BBR's window alone");
}

static void case_default_on(void)
{
    /* The module default is hold_enable=1. */
    ok(tbt_hold_enter(true, true, true, true, false, false,
                      100, 300 * MS, 200 * MS),
       "default: hold enters with the shipped default");
    ok(tbt_hold_keep(true, false, false, false, 100, 10 * 1000 * MS),
       "default: hold keeps while idle");
    /* Opt-out: hold_enable=0 must leave BBR untouched. */
    ok(!tbt_hold_enter(false, true, true, true, false, false,
                       100, 300 * MS, 200 * MS),
       "opt-out: hold never enters");
    ok(!tbt_hold_keep(false, false, false, false, 100, 10 * 1000 * MS),
       "opt-out: inactive hold never keeps");
}

int main(void)
{
    case_enter();
    case_keep();
    case_save();
    case_restore();
    case_apply();
    case_pad_session();
    case_state();
    case_restore_loop();
    case_default_on();

    printf("test_hold: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
