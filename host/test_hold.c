/* test_hold.c - host state-machine test for the peripheral BBR hold.
 *
 * WHAT THIS IS
 *   The exact predicates tbt_carrier.c uses, compiled from tbt_tx_acct.h:
 *
 *     - enter only when hold_enable is set, the carrier is idle in tbtun
 *       (no ARQ/FEC data), TCP is app-limited or has no in-flight bytes, CA
 *       is Open, no loss, and the idle interval has lasted enter_ns;
 *     - keep only while there is no real data, loss, window shrink or
 *       deadline expiry;
 *     - exit on real data, loss, window shrink or the 10 s idle cap.
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

    ok(!tbt_hold_enter(false, true, true, true, true, false, false,
                       100, 300 * MS, enter),
       "enter: disabled");
    ok(!tbt_hold_enter(true, true, true, false, true, false, false,
                       100, 300 * MS, enter),
       "enter: tbtun not idle");
    ok(!tbt_hold_enter(true, true, true, true, false, false, false,
                       100, 300 * MS, enter),
       "enter: CA not Open");
    ok(!tbt_hold_enter(true, true, true, true, true, true, false,
                       100, 300 * MS, enter),
       "enter: loss");
    ok(!tbt_hold_enter(true, true, true, true, true, false, true,
                       100, 300 * MS, enter),
       "enter: recent loss");
    ok(!tbt_hold_enter(true, false, false, true, true, false, false,
                       100, 300 * MS, enter),
       "enter: neither app-limited nor no-inflight");
    ok(!tbt_hold_enter(true, true, true, true, true, false, false,
                       0, 300 * MS, enter),
       "enter: no idle start");
    ok(!tbt_hold_enter(true, true, true, true, true, false, false,
                       100, 150 * MS, enter),
       "enter: before the enter delay");
    ok(tbt_hold_enter(true, true, true, true, true, false, false,
                      100, 300 * MS, enter),
       "enter: at the enter delay");
    ok(tbt_hold_enter(true, false, true, true, true, false, false,
                      100, 300 * MS, enter),
       "enter: no-inflight alone");
    ok(tbt_hold_enter(true, true, false, true, true, false, false,
                      100, 300 * MS, enter),
       "enter: app-limited alone");
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

struct model
{
    bool active;
    u64 since;
    u64 until;
};

static void model_step(struct model *m, u64 now, bool idle,
                       bool app_limited, bool no_inflight, bool loss,
                       bool real_data, bool wnd_shrunk)
{
    if (m->active)
    {
        if (!tbt_hold_keep(true, real_data, loss, wnd_shrunk, now,
                           m->until))
        {
            m->active = false;
            m->since = 0;
        }
        return;
    }
    if (!tbt_hold_enter(true, app_limited, no_inflight, idle, true, loss,
                        false, m->since, now, 200 * MS))
    {
        if (!real_data && idle && !loss && (app_limited || no_inflight))
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
    m->active = true;
    m->until = now + 10 * 1000 * MS;
}

static void case_state(void)
{
    struct model m = {0};

    model_step(&m, 1, true, true, true, false, false, false);
    ok(m.since == 1 && !m.active, "state: idle starts the enter timer");
    model_step(&m, 100 * MS, true, true, true, false, false, false);
    ok(!m.active, "state: still inside the enter delay");
    model_step(&m, 200 * MS + 1, true, true, true, false, false, false);
    ok(m.active, "state: enters after 200 ms");
    model_step(&m, 1 * 1000 * MS, true, true, true, false, false, false);
    ok(m.active, "state: holds while idle");
    model_step(&m, 11 * 1000 * MS, true, true, true, false, false, false);
    ok(!m.active, "state: 10 s idle cap exits");

    m = (struct model){0};
    model_step(&m, 1, true, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, true, false, false, false);
    ok(m.active, "state: re-enters");
    model_step(&m, 300 * MS, true, true, true, false, true, false);
    ok(!m.active, "state: real data exits");
    model_step(&m, 400 * MS, true, true, true, false, true, false);
    ok(!m.active && m.since == 0, "state: stays out while real data");

    m = (struct model){0};
    model_step(&m, 1, true, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, true, false, false, false);
    model_step(&m, 300 * MS, true, true, true, true, false, false);
    ok(!m.active, "state: loss exits");

    m = (struct model){0};
    model_step(&m, 1, true, true, true, false, false, false);
    model_step(&m, 200 * MS + 1, true, true, true, false, false, false);
    model_step(&m, 300 * MS, true, true, true, false, false, true);
    ok(!m.active, "state: window shrink exits");
}

static void case_default_on(void)
{
    /* The module default is hold_enable=1. */
    ok(tbt_hold_enter(true, true, true, true, true, false, false,
                      100, 300 * MS, 200 * MS),
       "default: hold enters with the shipped default");
    ok(tbt_hold_keep(true, false, false, false, 100, 10 * 1000 * MS),
       "default: hold keeps while idle");
    /* Opt-out: hold_enable=0 must leave BBR untouched. */
    ok(!tbt_hold_enter(false, true, true, true, true, false, false,
                       100, 300 * MS, 200 * MS),
       "opt-out: hold never enters");
    ok(!tbt_hold_keep(false, false, false, false, 100, 10 * 1000 * MS),
       "opt-out: inactive hold never keeps");
}

int main(void)
{
    case_enter();
    case_keep();
    case_state();
    case_default_on();

    printf("test_hold: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
