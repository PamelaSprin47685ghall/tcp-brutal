/* test_tx_index.c - host test for the TX-6 16-slot path bounds.
 *
 * WHAT THIS IS
 *   The exact bound rules tbt_tx.c uses, compiled from src/kernel/tbt_tx_acct.h:
 *
 *     - tbt_tx_path_in_range() is the pure test behind tbt_arq_path_ok():
 *       it accepts slots 0..15 that the session owns and refuses 16..255,
 *       even when a FEC session has 256 carriers;
 *     - tbt_tx_retain_path_in_range() is the test behind
 *       tbt_retain_path_ok(): idx >= 16 is a code error and sets `warned`
 *       (the kernel wrapper turns that into WARN_ON_ONCE), while a legal
 *       0..15 slot never warns, even when it is above the current n_car and
 *       therefore simply not present.
 *
 *   The host cannot execute WARN_ON_ONCE; asserting the explicit `warned`
 *   signal is the same decision the kernel wrapper makes.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void case_arq_bounds(void)
{
    unsigned int idx;

    for (idx = 0; idx <= 15; idx++)
        ok(tbt_tx_path_in_range(idx, 16), "arq: 0..15 accepted");

    ok(!tbt_tx_path_in_range(16, 16), "arq: 16 refused at n_car=16");
    ok(!tbt_tx_path_in_range(18, 256), "arq: 18 refused at n_car=256");
    ok(!tbt_tx_path_in_range(64, 256), "arq: 64 refused at n_car=256");
    ok(!tbt_tx_path_in_range(255, 256), "arq: 255 refused at n_car=256");
    ok(tbt_tx_path_in_range(15, 256),
       "arq: 15 accepted even when the session has 256 slots");
    ok(!tbt_tx_path_in_range(4, 4), "arq: idx == n_car refused");
    ok(!tbt_tx_path_in_range(0, 0), "arq: empty session refuses 0");
}

static void case_retain_bounds(void)
{
    unsigned int idx;
    bool warned;

    for (idx = 0; idx <= 15; idx++)
    {
        warned = false;
        ok(tbt_tx_retain_path_in_range(idx, 16, &warned),
           "retain: 0..15 accepted");
        ok(!warned, "retain: legal slot does not warn");
    }

    warned = false;
    ok(!tbt_tx_retain_path_in_range(16, 256, &warned),
       "retain: 16 refused at n_car=256");
    ok(warned, "retain: 16 warns");

    warned = false;
    ok(!tbt_tx_retain_path_in_range(255, 256, &warned),
       "retain: 255 refused at n_car=256");
    ok(warned, "retain: 255 warns");

    warned = false;
    ok(!tbt_tx_retain_path_in_range(4, 4, &warned),
       "retain: missing slot refused");
    ok(!warned, "retain: missing slot is not a code error");

    warned = false;
    ok(!tbt_tx_retain_path_in_range(0, 0, &warned),
       "retain: empty session refuses 0");
    ok(!warned, "retain: empty session is not a code error");
}

/* The scheduler mask is 256 bits, not the legacy u16: every carrier index the
 * session can own must have its own bit. */
static void case_blocked_mask(void)
{
    DECLARE_BITMAP(b, TBT_MAX_CARRIERS_LIMIT);
    unsigned int idx;

    memset(b, 0, sizeof(b));
    for (idx = 0; idx < TBT_MAX_CARRIERS_LIMIT; idx++)
        ok(!test_bit(idx, b), "blocked: starts clear");

    set_bit(0, b);
    set_bit(15, b);
    set_bit(16, b);
    set_bit(63, b);
    set_bit(64, b);
    set_bit(255, b);
    ok(test_bit(0, b), "blocked: index 0");
    ok(test_bit(15, b), "blocked: index 15");
    ok(test_bit(16, b), "blocked: index 16");
    ok(test_bit(63, b), "blocked: index 63");
    ok(test_bit(64, b), "blocked: index 64");
    ok(test_bit(255, b), "blocked: index 255");
    ok(!test_bit(1, b) && !test_bit(17, b) && !test_bit(62, b) &&
           !test_bit(65, b) && !test_bit(254, b),
       "blocked: neighbours stay clear");

    /* A bit just past the legacy 16-slot boundary must not alias back into
     * the low word. */
    memset(b, 0, sizeof(b));
    set_bit(16, b);
    ok(test_bit(16, b) && !test_bit(0, b) && !test_bit(15, b) &&
           !test_bit(17, b),
       "blocked: index 16 is independent");

    memset(b, 0, sizeof(b));
    set_bit(64, b);
    ok(test_bit(64, b) && !test_bit(63, b) && !test_bit(65, b),
       "blocked: index 64 is independent");

    memset(b, 0, sizeof(b));
    set_bit(255, b);
    ok(test_bit(255, b) && !test_bit(254, b) && !test_bit(0, b),
       "blocked: index 255 is independent");
}

/* Fold a legacy 16-bit mask into the 256-bit avoid/blocked mask without
 * aliasing or shifting past bit 15. */
static void case_mask_fold(void)
{
    DECLARE_BITMAP(b, TBT_MAX_CARRIERS_LIMIT);

    memset(b, 0, sizeof(b));
    tbt_mask_add_u16(b, (u16)(1u | (1u << 15)));
    ok(test_bit(0, b), "fold: bit 0");
    ok(test_bit(15, b), "fold: bit 15");
    ok(!test_bit(16, b) && !test_bit(64, b) && !test_bit(255, b),
       "fold: no alias above the legacy width");

    memset(b, 0, sizeof(b));
    set_bit(64, b);
    set_bit(255, b);
    tbt_mask_add_u16(b, (u16)(1u << 15));
    ok(test_bit(15, b) && test_bit(64, b) && test_bit(255, b),
       "fold: high bits survive");
}

int main(void)
{
    case_arq_bounds();
    case_retain_bounds();
    case_blocked_mask();
    case_mask_fold();

    printf("test_tx_index: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
