/* test_capture_poke.c - the pump's capture wake predicate.
 *
 * The field stall: ring=9 (nearly empty), credit=1792 (open), seq frozen for
 * 2.4 s, and the next target segment waiting for a zero-window RTO. The pump
 * only cleared tx_paused and woke w_read in the ring-full case, so an empty
 * ring with tx_paused already clear had no wake source between the target's
 * probes.
 *
 * The predicate is the contract: a shallow ring always asks capture for
 * more, a ring at or above half does not, and a paused ring asks regardless
 * of depth because the pump just made room.
 *
 * The second predicate guards the run word: a stale bit may be cleared only
 * when no body is running. Clearing one underneath a live body admitted a
 * second packetizer and made the first count fec_tx_run_leak (field: 1).
 */
#include "tbt_fec.h"

#include <stdio.h>

static int checks;
static int failures;

#define CHECK(cond, msg)                                                       \
    do                                                                         \
    {                                                                          \
        checks++;                                                              \
        if (!(cond))                                                           \
        {                                                                      \
            failures++;                                                        \
            printf("FAIL: %s\n", msg);                                         \
        }                                                                      \
    } while (0)

int main(void)
{
    const u32 ring_size = 1792u * 1024u;

    /* The field value: 9 bytes in flight, tx_paused already clear. */
    CHECK(tbt_fec_capture_poke_due(9, ring_size, false),
          "empty ring pokes capture with tx_paused clear");
    CHECK(tbt_fec_capture_poke_due(9, ring_size, true),
          "empty ring pokes capture when paused");

    /* At or above half the ring still holds work for the pump. */
    CHECK(!tbt_fec_capture_poke_due(ring_size / 2, ring_size, false),
          "a ring at half does not poke capture");
    CHECK(!tbt_fec_capture_poke_due(ring_size, ring_size, false),
          "a full ring does not poke capture");

    /* A pause wins over the depth test: the pump just drained a full ring. */
    CHECK(tbt_fec_capture_poke_due(ring_size, ring_size, true),
          "paused overrides the depth test");

    /* A zero ring size is inert, not a divide-by-zero and not a false
     * "there is room" claim. */
    CHECK(!tbt_fec_capture_poke_due(0, 0, false),
          "zero ring size is inert");

    /* A 1-byte ring below half is shallow even with a tiny window. */
    CHECK(tbt_fec_capture_poke_due(1, 16, false),
          "small ring below half pokes capture");

    /* Run-word stale recovery: never clear a word whose body is running. */
    CHECK(tbt_fec_run_word_stale(1, true, 2000000000ull, 899999999ull,
                                 false),
          "a one-second-old run word with no body is stale");
    CHECK(!tbt_fec_run_word_stale(1, true, 2000000000ull, 899999999ull,
                                  true),
          "a running body keeps its run word");
    CHECK(!tbt_fec_run_word_stale(0, true, 2000000000ull, 0, false),
          "a clear run word is not stale");
    CHECK(!tbt_fec_run_word_stale(1, false, 2000000000ull, 0, false),
          "an empty ring never clears the run word");
    CHECK(!tbt_fec_run_word_stale(1, true, 2000000000ull, 1000000000ull,
                                  false),
          "exactly one second is not yet stale");

    printf("test_capture_poke: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
