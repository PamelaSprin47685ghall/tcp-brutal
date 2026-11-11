/* test_cc_shell.c - host test for tbt_cc.h, the carrier controller and the
 * downlink shell retransmission predicates.
 *
 * WHAT THIS IS
 *   The exact arithmetic and state transitions tbtun/tbt_cc.c uses, compiled
 *   from the shared header:
 *
 *     - the ten-RTT delivery-rate window (max_bw);
 *     - pacing at gain * max_bw / (1 - p), clamped to [floor, tunnel rate];
 *     - snd_cwnd at max(2 * BDP, min_cwnd), bounded by snd_cwnd_clamp;
 *     - the framed wire length of a record in the raw and HTTP containers
 *       (a 1072-byte record is 1079 bytes as one chunk);
 *     - the 64-slot mut_ring: insert, overlap lookup, DSACK hit, snd_una
 *       retirement, wrap-around sequence numbers and eviction;
 *     - the in-place fill predicates: cloned data, a nonlinear skb, a hole
 *       that already carries a record, and a length mismatch all skip;
 *     - the DSACK rescue trigger: a matching DSACK rescues once, a failed
 *       replay keeps the slot for the next attempt.
 *
 *   It does not execute the kernel. The header functions are the shared code,
 *   and the model below calls them exactly as tbt_cc.c does.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_cc.h"

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

static void ok_u64(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %llu want %llu\n", what,
           (unsigned long long)got, (unsigned long long)want);
}

static void ok_u32(uint32_t got, uint32_t want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %u want %u\n", what, got, want);
}

/* --- 1. delivery-rate samples ---------------------------------------- */

static void test_bw_sample(void)
{
    ok_u64(tbt_cc_bw_from_sample(10, 1000, 1000), 10000000ULL,
           "bw: 10 packets x 1000 B in 1 ms is 10 MB/s");
    ok_u64(tbt_cc_bw_from_sample(0, 1000, 1000), 0,
           "bw: no delivered packet is no sample");
    ok_u64(tbt_cc_bw_from_sample(10, 1000, 0), 0,
           "bw: a zero interval is no sample");
    ok_u64(tbt_cc_bw_from_sample(-3, 1000, 1000), 0,
           "bw: a negative delivered count is no sample");
    ok_u64(tbt_cc_bw_from_sample(10, 0, 1000), 0,
           "bw: an unknown MSS is no sample");
}

static void test_bw_window(void)
{
    struct tbt_cc_state st = {};
    int i;

    st.srtt_us = 1000;
    tbt_cc_bw_note(&st, 10, 1000, 1000, false);
    ok_u64(st.max_bw, 10000000ULL, "window: the first RTT sample sets max_bw");
    ok_u32(st.bw_filled, 1, "window: one slot is live");

    /* Shorter than an SRTT: part of the same round, not a new sample. */
    tbt_cc_bw_note(&st, 10, 1000, 500, false);
    ok_u32(st.bw_filled, 1, "window: an intra-RTT ACK does not advance a slot");

    /* App-limited samples say nothing about the path. */
    tbt_cc_bw_note(&st, 10, 1000, 1000, true);
    ok_u32(st.bw_filled, 1, "window: an app-limited sample is skipped");

    tbt_cc_bw_note(&st, 20, 1000, 1000, false);
    ok_u64(st.max_bw, 20000000ULL, "window: a faster RTT raises max_bw");

    /* Fill ten slots with 1 MB/s; the 20 MB/s sample must age out. */
    for (i = 0; i < TBT_CC_BW_WIN; i++)
        tbt_cc_bw_note(&st, 1, 1000, 1000, false);
    ok_u32(st.bw_filled, TBT_CC_BW_WIN, "window: it holds ten RTT samples");
    ok_u64(st.max_bw, 1000000ULL, "window: the old maximum is evicted");

    /* A window that never filled reports the maximum of its live slots. */
    memset(&st, 0, sizeof(st));
    st.srtt_us = 1;
    tbt_cc_bw_note(&st, 3, 1000, 1000, false);
    tbt_cc_bw_note(&st, 2, 1000, 1000, false);
    tbt_cc_bw_note(&st, 1, 1000, 1000, false);
    ok_u32(st.bw_filled, 3, "window: three live slots");
    ok_u64(st.max_bw, 3000000ULL, "window: max over a partly filled window");
}

/* --- 2. pacing and window -------------------------------------------- */

static void test_pacing(void)
{
    ok_u64(tbt_cc_pacing_bps(0, 0, 1000000, 1000000, 0), 0,
           "pacing: no rate sample leaves the socket alone");

    ok_u32(TBT_CC_PACING_GAIN_PPM, 1000000,
           "pacing: the default gain adds no queue above the measured rate");
    ok_u64(tbt_cc_pacing_bps(10000000ULL, 0, 1000000, 1000000, 0),
           10000000ULL, "pacing: p=0, gain=1 is max_bw");

    /* 10 MB/s, 10% loss: 10 / 0.9 = 11.111 MB/s. */
    ok_u64(tbt_cc_pacing_bps(10000000ULL, 100000, 1000000, 1, 0),
           11111111ULL, "pacing: max_bw / (1 - p)");

    /* gain 1.1: 11 / 0.9 = 12.222 MB/s. */
    ok_u64(tbt_cc_pacing_bps(10000000ULL, 100000, 1100000, 1, 0),
           12222222ULL, "pacing: gain multiplies the loss-compensated rate");

    ok_u64(tbt_cc_pacing_bps(1000ULL, 0, 1000000, 1000000, 0),
           1000000ULL, "pacing: the floor wins over a tiny sample");
    ok_u64(tbt_cc_pacing_bps(100000000ULL, 0, 1000000, 1000000, 5000000),
           5000000ULL, "pacing: the tunnel rate caps the result");
    ok_u64(tbt_cc_pacing_bps(10000000ULL, 1000000, 1000000, 1, 0),
           100000000ULL,
           "pacing: p is capped at 90%, so the divisor never reaches zero");
    ok_u64(tbt_cc_pacing_bps(10000000ULL, 900001, 1000000, 1, 0),
           100000000ULL, "pacing: p above the cap clamps to the cap");
}

static void test_cwnd(void)
{
    /* 10 MB/s x 100 ms = 1 MB BDP; 2 x BDP / 1000 B = 2000 packets. */
    ok_u32(tbt_cc_cwnd_pkts(10000000ULL, 100000, 1000, 16, 0), 2000,
           "cwnd: two BDP in packets");
    ok_u32(tbt_cc_cwnd_pkts(10000000ULL, 100000, 1000, 4000, 0), 4000,
           "cwnd: min_cwnd raises a small BDP");
    ok_u32(tbt_cc_cwnd_pkts(10000000ULL, 100000, 1000, 16, 1000), 1000,
           "cwnd: snd_cwnd_clamp bounds the result");
    ok_u32(tbt_cc_cwnd_pkts(0, 100000, 1000, 16, 0), 16,
           "cwnd: no rate keeps the minimum window");
    ok_u32(tbt_cc_cwnd_pkts(10000000ULL, 100000, 0, 16, 0), 16,
           "cwnd: an unknown MSS falls back to the minimum window");
}

/* --- 3. wire framing -------------------------------------------------- */

static void test_wire(void)
{
    /* 48-byte header + 1024-byte share = 1072; the HTTP chunk adds the
     * three-digit size line and CRLF: 5 + 1072 + 2 = 1079. */
    ok_u32(tbt_cc_wire_hdr_len(1072, true), 5,
           "wire: 1072 needs three hex digits plus CRLF");
    ok_u32(tbt_cc_wire_len(1072, true), 1079,
           "wire: the 1072-byte record is one 1079-byte cell");
    ok_u32(tbt_cc_wire_len(1072, false), 1072,
           "wire: the raw container has no framing");
    ok_u32(tbt_cc_wire_hdr_len(100, true), 4,
           "wire: 100 (0x64) needs two hex digits plus CRLF");
    ok_u32(tbt_cc_wire_len(100, true), 106, "wire: short record frame");
    ok_u32(tbt_cc_wire_hdr_len(4096, true), 6,
           "wire: 4096 (0x1000) needs four hex digits plus CRLF");
    ok_u32(tbt_cc_wire_len(0, true), 5,
           "wire: an empty chunk is still one size line");
}

/* --- 4. sequence arithmetic and the mut_ring -------------------------- */

static void test_sequence(void)
{
    ok(tbt_cc_seq_before(10, 20), "seq: 10 precedes 20");
    ok(!tbt_cc_seq_before(20, 10), "seq: 20 does not precede 10");
    ok(tbt_cc_seq_before(0xffffffffu, 0), "seq: wrap 0xffffffff precedes 0");
    ok(!tbt_cc_seq_before(0, 0xffffffffu), "seq: wrap is one-way");

    ok(tbt_cc_mut_overlaps(1000, 100, 1050, 1100),
       "overlap: a DSACK inside the hole hits");
    ok(!tbt_cc_mut_overlaps(1000, 100, 900, 1000),
       "overlap: a DSACK ending at the hole start misses (half-open)");
    ok(tbt_cc_mut_overlaps(1000, 100, 950, 1050),
       "overlap: a DSACK crossing the hole start hits");
    ok(!tbt_cc_mut_overlaps(1000, 100, 1100, 1200),
       "overlap: a DSACK after the hole misses");
    ok(tbt_cc_mut_overlaps(0xfffffff0u, 32, 0, 16),
       "overlap: a DSACK across the sequence wrap hits");
    ok(!tbt_cc_mut_overlaps(0xfffffff0u, 32, 16, 32),
       "overlap: the byte after the wrapped hole misses");
    ok(!tbt_cc_mut_overlaps(1000, 0, 1000, 1100),
       "overlap: an empty hole never overlaps");
}

static void test_ring(void)
{
    struct tbt_cc_mut ring[TBT_CC_MUT_SLOTS] = {};
    u32 next = 0;
    struct tbt_cc_mut *m;
    int i;

    m = tbt_cc_mut_put(ring, &next, 1000, 7, 1, 2, 1079);
    ok(m == &ring[0], "ring: the first put uses slot 0");
    ok_u32(next, 1, "ring: the cursor advances");
    ok(m->valid && m->tcp_seq == 1000 && m->block_id == 7 && m->group == 1 &&
           m->share == 2 && m->len == 1079,
       "ring: the slot holds the hole and the record identity");

    m = tbt_cc_mut_find(ring, 1000, 1079);
    ok(m == &ring[0], "ring: find returns the covering slot");
    ok(tbt_cc_mut_find(ring, 3000, 1079) == NULL,
       "ring: find misses an unrelated hole");
    ok(tbt_cc_mut_find(ring, 2079, 1079) == NULL,
       "ring: find misses the half-open successor");

    m = tbt_cc_mut_dsack(ring, 1500, 1600);
    ok(m == &ring[0], "ring: a DSACK inside the hole names the slot");
    ok(tbt_cc_mut_dsack(ring, 2079, 3000) == NULL,
       "ring: an unrelated DSACK names no slot");

    tbt_cc_mut_ack(ring, 2078);
    ok(ring[0].valid, "ring: snd_una below the hole end keeps the slot");
    tbt_cc_mut_ack(ring, 2079);
    ok(!ring[0].valid, "ring: snd_una at the hole end retires the slot");

    /* Fill all 64 slots and wrap: the oldest is overwritten. */
    memset(ring, 0, sizeof(ring));
    next = 0;
    for (i = 0; i < TBT_CC_MUT_SLOTS; i++)
        tbt_cc_mut_put(ring, &next, (u32)(1000 + i * 2000), (u64)i, 0, 0, 1079);
    ok_u32(next, 0, "ring: 64 puts return the cursor to slot 0");
    ok(tbt_cc_mut_find(ring, 1000, 1079) != NULL,
       "ring: the oldest slot is still present after a full pass");
    m = &ring[next];
    ok(m->valid, "ring: the next slot is live before it is recycled");
    ok_u64(m->block_id, 0, "ring: the recycled slot names the oldest record");
    tbt_cc_mut_put(ring, &next, 999999, 99, 3, 4, 1079);
    ok(tbt_cc_mut_find(ring, 1000, 1079) == NULL,
       "ring: the 65th put evicts the oldest hole");
    ok(tbt_cc_mut_find(ring, 999999, 1079) != NULL,
       "ring: the newest hole is found");
}

/* --- 5. shell fill predicates ----------------------------------------- */

static void test_eligible(void)
{
    ok(tbt_cc_shell_eligible(false, true, 1079, 1079, false),
       "fill: a private linear skb of the record's length is eligible");
    ok(!tbt_cc_shell_eligible(true, true, 1079, 1079, false),
       "fill: a cloned skb is skipped");
    ok(!tbt_cc_shell_eligible(false, false, 1079, 1079, false),
       "fill: a nonlinear skb is skipped");
    ok(!tbt_cc_shell_eligible(false, true, 1079, 1072, false),
       "fill: a length mismatch is skipped");
    ok(!tbt_cc_shell_eligible(false, true, 0, 0, false),
       "fill: an empty skb is skipped");
    ok(!tbt_cc_shell_eligible(false, true, 1079, 1079, true),
       "fill: a hole that already carries a record is skipped");

    ok(tbt_cc_shell_usable(false, true, 1079, false),
       "fill: the pre-claim predicate accepts a private linear skb");
    ok(!tbt_cc_shell_usable(true, true, 1079, false),
       "fill: the pre-claim predicate refuses a clone");
    ok(!tbt_cc_shell_usable(false, false, 1079, false),
       "fill: the pre-claim predicate refuses a nonlinear skb");
    ok(!tbt_cc_shell_usable(false, true, 1079, true),
       "fill: the pre-claim predicate refuses a tracked hole");
}

/* --- 6. DSACK rescue trigger ------------------------------------------ */

/* The kernel path: a DSACK names a ring slot, the slot is pushed back onto
 * the FEC replay queue, and the slot is cleared only when that push worked.
 * `replay_has` models tbt_fec_replay_get() finding the record. */
static int model_dsack(struct tbt_cc_mut *ring, u32 start, u32 end,
                       bool replay_has, int *rescued)
{
    struct tbt_cc_mut *m = tbt_cc_mut_dsack(ring, start, end);

    if (!m)
        return 0;
    if (!replay_has)
    {
        /* snd_una may already be past this hole; the pending flag keeps
         * the slot alive for the eviction-path retry. */
        m->pending = 1;
        return -1;
    }
    m->valid = 0;
    m->pending = 0;
    (*rescued)++;
    return 1;
}

static void test_dsack_rescue(void)
{
    struct tbt_cc_mut ring[TBT_CC_MUT_SLOTS] = {};
    u32 next = 0;
    int rescued = 0;

    tbt_cc_mut_put(ring, &next, 5000, 42, 2, 3, 1079);
    ok(0 == model_dsack(ring, 7000, 7100, true, &rescued),
       "dsack: an unrelated block does not rescue");
    ok_u32((u32)rescued, 0, "dsack: no rescue counted for a miss");

    ok(1 == model_dsack(ring, 5000, 6079, true, &rescued),
       "dsack: the covering block rescues the replacement record");
    ok_u32((u32)rescued, 1, "dsack: one rescue counted");
    ok(!ring[0].valid, "dsack: a successful rescue clears the slot");
    ok(0 == model_dsack(ring, 5000, 6079, true, &rescued),
       "dsack: a second DSACK for the same slot finds nothing");

    /* A replay queue that cannot take the record must not lose it. */
    tbt_cc_mut_put(ring, &next, 9000, 43, 4, 5, 1079);
    ok(-1 == model_dsack(ring, 9000, 10079, false, &rescued),
       "dsack: a failed replay push reports failure");
    ok(ring[1].valid, "dsack: a failed rescue keeps the slot live");
    ok(ring[1].pending, "dsack: a failed rescue marks the slot pending");
    tbt_cc_mut_ack(ring, 20000);
    ok(ring[1].valid, "dsack: snd_una does not retire a pending slot");
    ok_u32((u32)rescued, 1, "dsack: a failed rescue is not counted");
    ok(1 == model_dsack(ring, 9000, 10079, true, &rescued),
       "dsack: the retry rescues the kept slot");
    ok_u32((u32)rescued, 2, "dsack: the retry counts one rescue");
}

int main(void)
{
    test_bw_sample();
    test_bw_window();
    test_pacing();
    test_cwnd();
    test_wire();
    test_sequence();
    test_ring();
    test_eligible();
    test_dsack_rescue();

    if (failures)
    {
        printf("test_cc_shell: %d failure(s) in %d checks\n", failures, checks);
        return 1;
    }
    printf("test_cc_shell: %d checks passed\n", checks);
    return 0;
}
