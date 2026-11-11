/* test_cell_tail.c - regression for the 0.8.0 fixed-cell sender tail.
 *
 * A 0.8.0 DATA share always carries S = 1024 bytes; a short tail is padded
 * and names its real length in win_len. The packetizer emits a share only
 * when its packet is full, so the tail of a write that is not a multiple of
 * S stays buffered until the idle cut or FIN seals it.
 *
 * Probe P lost exactly that tail: an 8191-byte echo returned 7168 bytes
 * (7 x 1024) and an 8193-byte echo returned 8192. Multiples of 1024 passed.
 * The block deadline counted the buffered partial packet as a real share, so
 * every grace pass closed an empty block, re-armed the 25 ms block deadline,
 * and never reached the 100 ms tail grace. The tail stayed buffered until
 * the connection died.
 *
 * tbt_fec_cut_pass() is the idle-cut state machine the packetizer runs; this
 * test drives it against a model of the sender for every size that crosses a
 * cell boundary. The delivered byte stream must equal the source after the
 * idle cut and after a half-close.
 *
 *   make host-test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_fec.h"

#define NS_PER_MS 1000000ULL
#define MAX_BYTES 16384
#define TICK_MS 5000

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

static void ok_size(size_t got, size_t want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %zu, want %zu\n", what, got, want);
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

/* ------------------------------------------------------------------ *
 * A model of the sender's cell state. n = 1, m = 1, groups = 1 is the
 * shape probe P runs, so one share is one block and one PARITY.
 * ------------------------------------------------------------------ */

struct cell_tx
{
    struct tbt_fec_cut_timer cut;
    const u8 *src;
    size_t size;
    size_t tail_off;
    size_t out_len;
    u8 out[MAX_BYTES];
    u64 seq;
    u64 blk_seq0;
    u16 groups;
    u16 blk_n;
    bool fill;
    u32 fill_len;
    bool any_nreal;
    bool block_had_share;
    bool base_set;
    bool credit_ok;
    unsigned short_shares;
    unsigned empty_blocks;
};

static void cell_init(struct cell_tx *t, const u8 *src, size_t size)
{
    memset(t, 0, sizeof(*t));
    t->src = src;
    t->size = size;
    t->groups = 1;
    t->blk_n = 1;
    t->credit_ok = true;
    t->tail_off = size;
}

/* The pump's block_advance: no buffered packet, no real share left, and the
 * block reached its span. n = 1 advances after every share. */
static void cell_advance(struct cell_tx *t)
{
    u64 span = (u64)t->groups * t->blk_n;

    if (t->fill || t->any_nreal)
        return;
    while (t->seq >= t->blk_seq0 + span)
    {
        t->blk_seq0 += span;
        t->block_had_share = false;
    }
}

/* tbt_fec_tx_close_block(advance = true): a block that never got a share
 * emits its empty marker, then the packet numbering jumps one block. */
static void cell_close_block(struct cell_tx *t)
{
    u64 span = (u64)t->groups * t->blk_n;

    if (!t->block_had_share)
        t->empty_blocks++;
    t->any_nreal = false;
    t->seq = t->blk_seq0 + span;
    t->blk_seq0 = t->seq;
    t->block_had_share = false;
}

/* One cell leaving the packetizer. n = 1: the share completes the block, so
 * its PARITY leaves at once and nreal falls back to zero. */
static void cell_share(struct cell_tx *t)
{
    t->any_nreal = true;
    t->block_had_share = true;
    t->base_set = true;
    t->seq++;
    if (t->seq - t->blk_seq0 >= (u64)t->groups * t->blk_n)
        t->any_nreal = false;
}

/* Submit one application write. Full cells go out at once; the remainder
 * waits for the idle cut or the FIN flush. */
static void cell_submit(struct cell_tx *t, const u8 *src, size_t size)
{
    size_t off = 0;

    cell_init(t, src, size);
    while (size - off >= TBT_FEC_S)
    {
        memcpy(t->out + t->out_len, src + off, TBT_FEC_S);
        t->out_len += TBT_FEC_S;
        cell_share(t);
        cell_advance(t);
        off += TBT_FEC_S;
    }
    t->tail_off = off;
    t->fill_len = (u32)(size - off);
    t->fill = t->fill_len != 0;
}

/* One pump pass at the current time. Returns the plan the packetizer got. */
static unsigned int cell_pump(struct cell_tx *t, u64 now_ns)
{
    struct tbt_fec_cut_state cs;
    unsigned int plan;

    cs.fill = t->fill;
    cs.any_nreal = t->any_nreal;
    cs.base_set = t->base_set;
    cs.credit_ok = t->credit_ok;
    cs.seq = t->seq;
    cs.blk_seq0 = t->blk_seq0;
    cs.groups = t->groups;
    cs.blk_n = t->blk_n;
    cs.now_ns = now_ns;
    plan = tbt_fec_cut_pass(&t->cut, &cs);
    if (plan & TBT_FEC_CUT_TAIL)
    {
        memcpy(t->out + t->out_len, t->src + t->tail_off, t->fill_len);
        t->out_len += t->fill_len;
        t->short_shares++;
        t->fill = false;
        t->fill_len = 0;
        cell_share(t);
    }
    if (plan & TBT_FEC_CUT_BLOCK)
        cell_close_block(t);
    return plan;
}

/* Run the idle cut until it emits the tail or the budget expires. */
static u64 cell_idle(struct cell_tx *t)
{
    u64 ms;

    for (ms = 0; ms <= TICK_MS; ms++)
    {
        cell_pump(t, ms * NS_PER_MS);
        if (!t->fill)
            return ms;
    }
    return ms;
}

/* A half-close flushes the buffered tail at once. */
static void cell_flush(struct cell_tx *t)
{
    if (!t->fill)
        return;
    memcpy(t->out + t->out_len, t->src + t->tail_off, t->fill_len);
    t->out_len += t->fill_len;
    t->short_shares++;
    t->fill = false;
    t->fill_len = 0;
    cell_share(t);
}

static u8 src[MAX_BYTES];

static void fill_src(size_t size)
{
    size_t i;

    for (i = 0; i < size; i++)
        src[i] = (u8)(i * 37u + 11u);
}

static void check_bytes(struct cell_tx *t, size_t size, const char *what)
{
    ok_size(t->out_len, size, what);
    ok(!memcmp(t->out, src, size), what);
}

/* ------------------------------------------------------------------ *
 * Decisions.
 * ------------------------------------------------------------------ */

static void test_block_owns_share(void)
{
    /* The buffered tail is not a share of the current block. This is the
     * first breakpoint: the old block_dirty test returned true here. */
    ok(!tbt_fec_block_owns_share(false, 7, 7, 1, 1),
       "a buffered tail alone is not a real share");
    ok(tbt_fec_block_owns_share(true, 7, 7, 1, 1),
       "a written share makes the block real");
    ok(tbt_fec_block_owns_share(false, 8, 7, 1, 1),
       "a full block waiting for its advance is real");
    ok(!tbt_fec_block_owns_share(false, 7, 7, 1, 2),
       "an empty half-filled block is not real");
}

static void test_cut_pass(void)
{
    struct tbt_fec_cut_timer t = {0, 0};
    struct tbt_fec_cut_state s;

    memset(&s, 0, sizeof(s));
    s.fill = true;
    s.base_set = true;
    s.credit_ok = true;
    s.seq = 7;
    s.blk_seq0 = 7;
    s.groups = 1;
    s.blk_n = 1;
    /* A tail behind seven full cells arms only the 25 ms tail deadline. */
    ok_u64(tbt_fec_cut_pass(&t, &s), 0, "the tail deadline does not fire early");
    ok_u64(t.short_at_ns, 25 * NS_PER_MS, "the tail deadline is 25 ms");
    ok_u64(t.block_at_ns, 0, "no block deadline for a buffered tail");
    s.now_ns = 25 * NS_PER_MS;
    ok_u64(tbt_fec_cut_pass(&t, &s),
           TBT_FEC_CUT_TAIL | TBT_FEC_CUT_BLOCK,
           "the 25 ms tail cut seals the partial packet");
    ok_u64(t.short_at_ns, 0, "the tail deadline clears after the cut");
}

static void test_credit_holds(void)
{
    struct cell_tx t;
    u64 ms;

    fill_src(8191);
    cell_submit(&t, src, 8191);
    t.credit_ok = false;
    for (ms = 0; ms < 40; ms++)
        cell_pump(&t, ms * NS_PER_MS);
    ok_size(t.out_len, 7168, "no tail leaves before credit");
    ok_u64(t.short_shares, 0, "no short share before credit");
    ok(t.cut.short_at_ns != 0, "the tail deadline stays armed");
    t.credit_ok = true;
    cell_pump(&t, 40 * NS_PER_MS);
    check_bytes(&t, 8191, "the tail leaves when the credit reopens");
    ok_u64(t.short_shares, 1, "one short share after credit reopens");
}

/* ------------------------------------------------------------------ *
 * Sizes.
 * ------------------------------------------------------------------ */

static void test_idle(size_t size)
{
    struct cell_tx t;
    u64 ms;

    fill_src(size);
    cell_submit(&t, src, size);
    ms = cell_idle(&t);
    check_bytes(&t, size, "idle cut delivers every byte");
    ok_u64(t.short_shares, size % TBT_FEC_S ? 1 : 0,
           "exactly one short share for a non-multiple");
    ok_u64(t.empty_blocks, 0, "the idle cut never closes an empty block");
    ok(ms <= 60, "the idle cut seals the tail within 60 ms");
}

static void test_flush(size_t size)
{
    struct cell_tx t;

    fill_src(size);
    cell_submit(&t, src, size);
    cell_flush(&t);
    check_bytes(&t, size, "half-close delivers every byte");
    ok_u64(t.short_shares, size % TBT_FEC_S ? 1 : 0,
           "half-close sends exactly one short share");
}

static void test_grace(void)
{
    struct tbt_fec_cut_timer cut = {0, 0};
    struct tbt_fec_cut_state s;
    unsigned int plan;

    /* A block with two real shares and a buffered tail: the block deadline
     * closes the block first, then the tail keeps its grace. */
    memset(&s, 0, sizeof(s));
    s.fill = true;
    s.any_nreal = true;
    s.base_set = true;
    s.credit_ok = true;
    s.seq = 2;
    s.blk_seq0 = 0;
    s.groups = 1;
    s.blk_n = 4;
    tbt_fec_cut_pass(&cut, &s);
    s.now_ns = 25 * NS_PER_MS;
    plan = tbt_fec_cut_pass(&cut, &s);
    ok_u64(plan, TBT_FEC_CUT_BLOCK, "a real block closes before its tail");
    ok_u64(cut.short_at_ns, 125 * NS_PER_MS, "the tail gets its 100 ms grace");
    ok_u64(cut.block_at_ns, 0, "the block deadline is spent");
    /* close_block() reset nreal and advanced the block. */
    s.any_nreal = false;
    s.seq = 4;
    s.blk_seq0 = 4;
    s.now_ns = 124 * NS_PER_MS;
    ok_u64(tbt_fec_cut_pass(&cut, &s), 0, "the grace has not expired");
    s.now_ns = 125 * NS_PER_MS;
    ok_u64(tbt_fec_cut_pass(&cut, &s),
           TBT_FEC_CUT_TAIL | TBT_FEC_CUT_BLOCK,
           "the grace seals the buffered tail");
}

int main(void)
{
    static const size_t sizes[] = {1, 37, 1023, 1024, 1025, 8191, 8192, 8193};
    size_t i;

    test_block_owns_share();
    test_cut_pass();
    test_credit_holds();
    test_grace();
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        test_idle(sizes[i]);
        test_flush(sizes[i]);
    }

    if (!failures)
        printf("test_cell_tail: %d checks, 0 failures\n", checks);
    return failures ? 1 : 0;
}
