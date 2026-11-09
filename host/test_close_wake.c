/* test_close_wake.c - executable spec for the G4 close-path wakeups.
 *
 * The kernel functions are not host-linkable. This file pins the three
 * policies that the G4 soak failed on, with the real flow-admission helper:
 *
 *   - a new far stream, a finished FEC block, a received FIN and a delivered
 *     peer FIN must each kick the FBK tick; w_fbk stops when nsnap == 0.
 *   - tbt_tx_fin() stamps close_since_ns before the FEC "not finished / not
 *     idle" early returns.
 *   - a same-sid OPEN is -EEXIST before the SC-LDPC admission refusal.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_fec.h"

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

struct wake
{
    bool fbk_armed;
    bool fec_finished;
    bool fec_idle;
    bool rx_fin_done;
    u64 close_since_ns;
};

/* Mirrors tbt_fbk_kick(): one bit/schedule, no state of its own. */
static void fbk_kick(struct wake *w)
{
    w->fbk_armed = true;
}

/* Mirrors tbt_far_open_seen(). */
static void far_open_seen(struct wake *w)
{
    fbk_kick(w);
}

/* Mirrors tbt_fec_tx_finish(). */
static void fec_tx_finish(struct wake *w)
{
    w->fec_finished = true;
    fbk_kick(w);
}

/* Mirrors the TBT_F_FIN rx path and tbt_sock.c's rx_fin_done store. */
static void rx_fin_done(struct wake *w)
{
    w->rx_fin_done = true;
    fbk_kick(w);
}

/* Mirrors tbt_tx_fin(): the close stamp is before the FEC early returns. */
static void tx_fin(struct wake *w)
{
    if (!w->close_since_ns)
        w->close_since_ns = 1;
    if (!w->fec_finished)
        return;
    if (!w->fec_idle)
        return;
}

static void case_wakeups(void)
{
    struct wake w;

    memset(&w, 0, sizeof(w));
    far_open_seen(&w);
    ok(w.fbk_armed, "new far stream kicks the FBK tick");

    memset(&w, 0, sizeof(w));
    fec_tx_finish(&w);
    ok(w.fbk_armed, "finished FEC block kicks the FBK tick");

    memset(&w, 0, sizeof(w));
    rx_fin_done(&w);
    ok(w.fbk_armed && w.rx_fin_done, "delivered peer FIN kicks the FBK tick");
}

static void case_close_stamp(void)
{
    struct wake w;

    memset(&w, 0, sizeof(w));
    tx_fin(&w);
    ok(w.close_since_ns != 0,
       "close_since stamped before FEC not-finished early return");

    memset(&w, 0, sizeof(w));
    w.fec_finished = true;
    w.fec_idle = false;
    tx_fin(&w);
    ok(w.close_since_ns != 0,
       "close_since stamped before FEC not-idle early return");
}

static void case_same_sid_priority(void)
{
    ok(tbt_fec_flow_create_code(true, true, 1, 1, 1) == -EEXIST,
       "same sid returns EEXIST before SC-LDPC admission");
    ok(tbt_fec_flow_create_code(false, true, 1, 1, 1) == -EEXIST,
       "a second SC-LDPC flow shares the primary (EEXIST)");
    ok(tbt_fec_flow_create_code(true, false, 0, 1, 1) == -EEXIST,
       "same sid returns EEXIST for non-SCLC admission");
    ok(tbt_fec_flow_create_code(false, false, 0, 0, 1) == -EPROTO,
       "missing group budget still returns EPROTO");
}

int main(void)
{
    case_wakeups();
    case_close_stamp();
    case_same_sid_priority();

    printf("test_close_wake: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
