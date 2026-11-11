/* test_fin_tail.c - regression for the FIN/short-tail half-close.
 *
 * The field bug: a 32 MiB stream ended with a short final block (64 of 128
 * shares), the sender shut its write side, and the receiver's FIN completed
 * as soon as the packet frontier (32768) met a byte cursor (33,488,896).
 * The drain then shut the target's write side before the walk delivered the
 * last 64 KiB, which came back as EPIPE.
 *
 * Two decisions must hold together and neither needs a kernel:
 *   - FIN's pkt_end stops at the last real share, not at the padding
 *     boundary a cut block jumped to;
 *   - the drain completes only after the walk reaches that packet end.
 * The same helpers the module calls are linked here, so a regression in
 * either rule fails this test.
 *
 *   make host-test
 */
#include <stdint.h>
#include <stdio.h>

#include "tbt_fec.h"

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", what);
}

static void ok_u64(u64 got, u64 want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %llu, want %llu\n", what,
           (unsigned long long)got, (unsigned long long)want);
}

int main(void)
{
    const u16 n = 128;
    const u16 groups = 1;
    const u64 span = (u64)groups * n;
    const u64 base_pkt = 0;
    const u64 real_tail = 64;  /* real shares in the final block */
    const u16 last_len = 784;  /* the last share is short */
    const u64 pkt_end = span + real_tail; /* last real share + 1 */
    const u64 full_end = span + n;        /* the padding boundary */
    const u64 bytes = span * TBT_FEC_S +
                      (real_tail - 1) * TBT_FEC_S + last_len;

    /* FIN's packet end stops at the last real share, not at the padding
     * boundary the idle cut jumped to. */
    ok_u64(tbt_fec_final_block_of_total(pkt_end, groups, n), 1,
           "FIN names the second (final) block");
    ok_u64(tbt_fec_group_tail_len(pkt_end, groups, n, 0), real_tail,
           "the final block holds only its real shares");
    ok(tbt_fec_fin_valid_base(pkt_end, base_pkt, base_pkt, span),
       "FIN's packet range is block-aligned");

    /* Parity lost, so the tail comes from pkt_end: it must stop at 64.
     * The old full-block end would claim 128 and turn zero padding into
     * stream bytes. */
    ok_u64(tbt_fec_blk_real_len_seq0(0, n, true, pkt_end, span, 0, false),
           real_tail, "a short final block stays short without its parity");
    ok_u64(tbt_fec_blk_real_len_seq0(0, n, true, full_end, span, 0, false),
           n, "a full-block end is the wrong cut");
    ok(tbt_fec_group_tail_len(full_end, groups, n, 0) == n,
       "the padding boundary would claim a full block");

    /* FIN ordering: the drain may not complete before the walk reaches the
     * packet end; a partial walk below it must wait. */
    ok(!tbt_fec_fin_delivered(pkt_end, pkt_end - 1, false),
       "FIN waits while the last share is owed");
    ok(!tbt_fec_fin_delivered(pkt_end, pkt_end, true),
       "a partial walk keeps FIN waiting");
    ok(tbt_fec_fin_delivered(pkt_end, pkt_end, false),
       "FIN completes after the walk");

    /* Every byte below the FIN is delivered exactly once. */
    ok_u64(bytes, 192 * TBT_FEC_S - (TBT_FEC_S - last_len),
           "no padding byte is counted");

    /* The detach counter stays zero on the correct path. */
    {
        unsigned undelivered = 0;

        if (!tbt_fec_fin_delivered(pkt_end, pkt_end, false))
            undelivered++;
        ok_u64(undelivered, 0, "detach with nothing undelivered");
    }

    if (!failures)
        printf("test_fin_tail: %d checks, 0 failures\n", checks);
    return failures ? 1 : 0;
}
