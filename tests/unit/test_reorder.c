/* test_reorder.c - SPEC test for review 20's reorder-window fix.
 *
 * WHAT THIS IS
 *   A pure host model of the patched tbt_rx.c reorder algorithm:
 *
 *     - tbt_rx_data()'s entry decision for seq < rx_next (drop vs clip)
 *     - tbt_rx_tree_insert_locked()'s exact-seq duplicate rule
 *     - tbt_rx_pull_locked()'s three branches (full stale / straddle / exact)
 *     - the rx_ooo_bytes / rx_ooo_frames / rx_queued / rx_next / off accounting
 *
 *   It does NOT link or execute tbt_rx.c: that file needs sk_buff, the rb-tree,
 *   ktime and the stream/session locks. The model is a line-by-line
 *   transcription of the intended branches in
 * §1.2/§1.3. It is the executable
 *   specification the kernel patch must match, not a test of the kernel code.
 *   When the kernel patch lands, keep this file and the kernel in lockstep;
 *   the comments below name the exact kernel branch each block mirrors.
 *
 * WHY A MODEL
 *   The only kernel state the algorithm needs is:
 *     rx_next, a set of (seq,len,off) frames in the tree, the ready queue,
 *     rx_ooo_bytes, rx_ooo_frames, rx_queued, rx_delivered.
 *   The rb-tree is an implementation detail: a sorted array has the same
 *   observable behavior (leftmost = smallest seq). Locks, allocation, the hole
 *   timer and tbt_stream_kill are outside this specification.
 *
 * CASES
 *   exact, hole-then-fill, full-stale tree node, straddling tree node,
 *   straddling entry frame, exact duplicate, duplicate tail, several stale
 *   nodes, overlapping future nodes, and a deterministic randomized run with
 *   invariant checks after every operation.
 *
 *   make host-test
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_TREE 256
#define MAX_RANDOM_OPS 4000

static int checks;
static int failures;

static void ok(bool cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

/* One frame in the reorder tree or in the ready queue. `len` is the original
 * payload length; `off` is the number of bytes already delivered from the
 * front (0 for a frame that has never been clipped). The queued stream range
 * is [seq, seq + len - off). */
struct mframe
{
    uint64_t seq;
    uint32_t len;
    uint32_t off;
};

struct model
{
    struct mframe tree[MAX_TREE]; /* sorted by seq, unique seq */
    int ntree;
    uint64_t rx_next;      /* end of the contiguous received run */
    uint64_t rx_delivered; /* app delivery offset (the model may drain) */
    uint64_t delivered_end;/* end of the queued run == rx_delivered + rx_queued */
    uint32_t rx_ooo_bytes;
    uint32_t rx_ooo_frames;
    uint32_t rx_queued;
    uint32_t dup_used;
    uint32_t dropped;
    uint32_t dups;
    /* last queued frame, for clip/off assertions */
    uint64_t last_seq;
    uint32_t last_len;
    uint32_t last_off;
    /* Byte bound and bounded-reset counter: mirrors st->rx_limit and
     * sess->rx_reorder_overflow in tbt_rx.c.  rx_limit == 0 keeps every
     * legacy case unlimited. */
    uint32_t rx_limit;
    uint32_t rx_reorder_overflow;
    bool dead; /* the stream was killed by the reorder bound */
};

static void model_init(struct model *m)
{
    memset(m, 0, sizeof(*m));
}

static uint32_t rnd_below(uint64_t *state, uint32_t n)
{
    uint64_t x = *state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return n ? (uint32_t)(x % n) : 0;
}

/* Insert one out-of-order frame. Mirrors tbt_rx_tree_insert_locked(): an
 * exact seq is a duplicate (false); every other seq is inserted. The rb-tree
 * is replaced by a sorted array; the leftmost element is tree[0]. */
static bool tree_insert(struct model *m, uint64_t seq, uint32_t len)
{
    int pos;

    for (pos = 0; pos < m->ntree; pos++)
    {
        if (m->tree[pos].seq == seq)
            return false; /* exact duplicate, kernel returns false */
        if (m->tree[pos].seq > seq)
            break;
    }
    if (m->ntree >= MAX_TREE)
        return false; /* the model's capacity, not a kernel rule */
    memmove(&m->tree[pos + 1], &m->tree[pos],
            (size_t)(m->ntree - pos) * sizeof(m->tree[0]));
    m->tree[pos].seq = seq;
    m->tree[pos].len = len;
    m->tree[pos].off = 0;
    m->ntree++;
    m->rx_ooo_bytes += len;
    m->rx_ooo_frames++;
    return true;
}

static void tree_erase_first(struct model *m)
{
    memmove(&m->tree[0], &m->tree[1],
            (size_t)(m->ntree - 1) * sizeof(m->tree[0]));
    m->ntree--;
}

/* Queue a frame for delivery. `seq` is the first stream byte the frame still
 * carries; the queued range is [seq, seq + len - off). The queued run must be
 * contiguous: its start is rx_delivered + rx_queued, which is also
 * delivered_end. This is the accounting the kernel does in
 * tbt_rx_pull_locked() and tbt_rx_data(). */
static void queue(struct model *m, uint64_t seq, uint32_t len, uint32_t off)
{
    uint64_t start = m->rx_delivered + m->rx_queued;

    ok(seq == start, "queue start is contiguous", NULL);
    ok(off < len, "queued frame has at least one byte", NULL);
    m->rx_queued += len - off;
    m->delivered_end = seq + (len - off);
    m->last_seq = seq;
    m->last_len = len;
    m->last_off = off;
}

/* Model the app draining the ready queue: rx_delivered advances to rx_next,
 * rx_queued becomes 0. The kernel's tbt_drain_work() does this asynchronously;
 * the model only needs the accounting. */
static void drain(struct model *m)
{
    m->rx_delivered += m->rx_queued;
    m->rx_queued = 0;
    ok(m->rx_delivered == m->rx_next, "drain reaches rx_next", NULL);
    ok(m->delivered_end == m->rx_next, "queued run ends at rx_next", NULL);
}

/* The three-branch pull. Kernel: tbt_rx_pull_locked() in the patched form
 * (stability-patch-plan §1.2). The loop always examines the leftmost frame;
 * the array is sorted, so tree[0] is the leftmost. */
static void pull(struct model *m)
{
    for (;;)
    {
        struct mframe *fr;
        uint32_t dup;

        if (!m->ntree)
            break;
        fr = &m->tree[0];
        if (fr->seq > m->rx_next)
            break;

        if (fr->seq + fr->len <= m->rx_next)
        {
            /* Branch 1, full stale: the whole node was delivered by another
             * copy. The node leaves the tree; rx_ooo_bytes loses the whole
             * len, not just an overlap. */
            m->rx_ooo_bytes -= fr->len;
            m->rx_ooo_frames--;
            tree_erase_first(m);
            continue;
        }

        if (fr->seq == m->rx_next)
        {
            /* Branch 2, exact: normal advance. */
            uint64_t seq = fr->seq;
            uint32_t len = fr->len;

            m->rx_ooo_bytes -= len;
            m->rx_ooo_frames--;
            tree_erase_first(m);
            queue(m, seq, len, 0);
            m->rx_next = seq + len;
            continue;
        }

        /* Branch 3, straddle: seq < rx_next < seq + len. Drop the already
         * delivered prefix; off records it; keep the tail. */
        dup = (uint32_t)(m->rx_next - fr->seq);
        {
            uint64_t seq = m->rx_next;
            uint32_t len = fr->len;
            uint32_t off = fr->off + dup;

            m->rx_ooo_bytes -= len; /* the node leaves the tree */
            m->rx_ooo_frames--;
            tree_erase_first(m);
            queue(m, seq, len, off);
            m->rx_next = seq + len - off;
        }
    }
}

/* Entry decision. Kernel: tbt_rx_data() in the patched form
 * (stability-patch-plan §1.3). Returns 0 for a tree insert, 1 for an exact
 * queue, 2 for a clipped queue, 3 for a full-stale drop, 4 for a duplicate. */
enum data_result
{
    DATA_TREE = 0,
    DATA_EXACT,
    DATA_CLIPPED,
    DATA_STALE,
    DATA_DUP,
    DATA_DEAD,
};

static enum data_result data(struct model *m, uint64_t seq, uint32_t len, bool low_prio)
{
    /* Kernel tbt_rx_data() drops every frame once state == TBT_ST_DEAD
     * (tbt_rx.c:173-181).  The model must not count those as stale/dup. */
    if (m->dead)
        return DATA_DEAD;
    if (seq < m->rx_next)
    {
        uint32_t dup = (uint32_t)(m->rx_next - seq);

        if (seq + len <= m->rx_next)
        {
            /* Entry full stale: already delivered, drop. */
            m->dropped++;
            return DATA_STALE;
        }
        /* Entry straddle: clip the delivered prefix and queue the tail. */
        queue(m, m->rx_next, len, dup);
        m->rx_next = m->rx_next + (len - dup);
        if (low_prio)
            m->dup_used++;
        pull(m);
        return DATA_CLIPPED;
    }
    if (seq == m->rx_next)
    {
        queue(m, seq, len, 0);
        m->rx_next = seq + len;
        if (low_prio)
            m->dup_used++;
        pull(m);
        return DATA_EXACT;
    }
    if (!tree_insert(m, seq, len))
    {
        m->dups++;
        return DATA_DUP;
    }
    if (low_prio)
        m->dup_used++;
    /* Kernel tbt_rx.c:257-258: the insert already added len, then a byte
     * over rx_limit kills the stream and bumps rx_reorder_overflow once
     * (tbt_rx.c:270-275).  dead makes later frames return DATA_DEAD, so
     * the counter cannot grow again. */
    if (m->rx_limit && m->rx_ooo_bytes > m->rx_limit)
    {
        m->rx_reorder_overflow++;
        m->dead = true;
    }
    return DATA_TREE;
}

/* Invariants that must hold after every operation:
 *   - rx_ooo_bytes is the sum of the tree frame lengths;
 *   - rx_ooo_frames is the tree count;
 *   - rx_queued is the number of bytes in the ready queue;
 *   - the ready queue covers [rx_delivered, rx_next), so
 *     rx_queued == rx_next - rx_delivered;
 *   - the tree has no frame that starts at or before rx_next (the pull would
 *     have removed or clipped it);
 *   - the tree is sorted by seq and has unique seqs;
 *   - the last queued frame's off is within its len.
 */
static void model_check(const struct model *m, const char *what)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < m->ntree; i++)
    {
        sum += m->tree[i].len;
        ok(m->tree[i].seq > m->rx_next, what, "tree frame not past rx_next");
        if (i)
            ok(m->tree[i - 1].seq < m->tree[i].seq, what, "tree sorted/unique");
    }
    ok(sum == m->rx_ooo_bytes, what, "rx_ooo_bytes == sum(tree len)");
    ok((uint32_t)m->ntree == m->rx_ooo_frames, what, "rx_ooo_frames == tree count");
    ok(m->rx_delivered <= m->rx_next, what, "rx_delivered <= rx_next");
    ok(m->rx_queued == m->rx_next - m->rx_delivered, what,
       "rx_queued == rx_next - rx_delivered");
    ok(m->delivered_end == m->rx_next, what, "queued run ends at rx_next");
    ok(m->last_off <= m->last_len, what, "last off <= len");
}

/* Kernel tbt_rx_free_queue() (tbt_rx.c:1010-1031) frees both queues and
 * clears the three counters.  It deliberately leaves rx_next and
 * rx_delivered alone, so callers must not run model_check() afterwards. */
static void model_free_queue(struct model *m)
{
    m->ntree = 0;
    m->rx_queued = 0;
    m->rx_ooo_bytes = 0;
    m->rx_ooo_frames = 0;
}

/* --- cases ------------------------------------------------------------- */

static void case_exact(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 0, 100, false) == DATA_EXACT, "exact: result", NULL);
    model_check(&m, "exact");
    ok(m.rx_next == 100 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 100 && m.dup_used == 0,
       "exact: accounting", NULL);
    ok(m.last_seq == 0 && m.last_len == 100 && m.last_off == 0,
       "exact: queued frame", NULL);
}

static void case_hole_fill(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "hole: future frame", NULL);
    model_check(&m, "hole: after tree insert");
    ok(m.rx_next == 0 && m.rx_ooo_bytes == 100 && m.rx_ooo_frames == 1 &&
           m.rx_queued == 0,
       "hole: tree accounting", NULL);

    ok(data(&m, 0, 100, false) == DATA_EXACT, "hole: fill", NULL);
    model_check(&m, "hole: after fill");
    ok(m.rx_next == 200 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 200,
       "hole: pulled both frames", NULL);
    ok(m.last_seq == 100 && m.last_len == 100 && m.last_off == 0,
       "hole: last pulled frame", NULL);
}

static void case_full_stale_tree(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "full-stale: tree", NULL);
    ok(data(&m, 0, 300, false) == DATA_EXACT, "full-stale: cover", NULL);
    model_check(&m, "full-stale");
    ok(m.rx_next == 300 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 300,
       "full-stale: stale node freed", NULL);
    ok(m.last_seq == 0 && m.last_len == 300 && m.last_off == 0,
       "full-stale: only the covering frame queued", NULL);
}

static void case_straddle_tree(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "straddle-tree: tree", NULL);
    ok(data(&m, 0, 150, false) == DATA_EXACT, "straddle-tree: cover prefix", NULL);
    model_check(&m, "straddle-tree");
    ok(m.rx_next == 200 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 200,
       "straddle-tree: clipped node removed from tree", NULL);
    /* The tree node [100,200) is clipped to [150,200): off=50, len=100. */
    ok(m.last_seq == 150 && m.last_len == 100 && m.last_off == 50,
       "straddle-tree: tail queued with off=50", NULL);
}

static void case_entry_straddle(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 0, 150, false) == DATA_EXACT, "entry-straddle: prefix", NULL);
    model_check(&m, "entry-straddle: before");
    ok(data(&m, 100, 100, false) == DATA_CLIPPED, "entry-straddle: clipped", NULL);
    model_check(&m, "entry-straddle: after");
    ok(m.rx_next == 200 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 200,
       "entry-straddle: accounting", NULL);
    ok(m.last_seq == 150 && m.last_len == 100 && m.last_off == 50,
       "entry-straddle: tail queued with off=50", NULL);
}

static void case_duplicate_tree(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "dup-tree: first", NULL);
    ok(data(&m, 100, 100, false) == DATA_DUP, "dup-tree: exact duplicate", NULL);
    model_check(&m, "dup-tree: after duplicate");
    ok(m.rx_ooo_bytes == 100 && m.rx_ooo_frames == 1 && m.dups == 1,
       "dup-tree: duplicate did not grow the tree", NULL);
    ok(data(&m, 0, 200, false) == DATA_EXACT, "dup-tree: cover", NULL);
    model_check(&m, "dup-tree: after cover");
    ok(m.rx_next == 200 && m.rx_ooo_bytes == 0 && m.rx_queued == 200,
       "dup-tree: no double delivery", NULL);
}

static void case_duplicate_tail(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "dup-tail: tree", NULL);
    ok(data(&m, 0, 150, false) == DATA_EXACT, "dup-tail: clip", NULL);
    ok(m.last_seq == 150 && m.last_len == 100 && m.last_off == 50,
       "dup-tail: clipped tail", NULL);
    ok(data(&m, 150, 50, false) == DATA_STALE, "dup-tail: duplicate tail", NULL);
    model_check(&m, "dup-tail: after duplicate");
    ok(m.rx_next == 200 && m.rx_queued == 200 && m.dropped == 1,
       "dup-tail: no extra bytes", NULL);
}

static void case_multiple_stale(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 50, false) == DATA_TREE, "multi-stale: 100", NULL);
    ok(data(&m, 150, 100, false) == DATA_TREE, "multi-stale: 150", NULL);
    ok(data(&m, 250, 50, false) == DATA_TREE, "multi-stale: 250", NULL);
    ok(m.rx_ooo_bytes == 200 && m.rx_ooo_frames == 3,
       "multi-stale: tree accounting", NULL);
    ok(data(&m, 0, 300, false) == DATA_EXACT, "multi-stale: cover", NULL);
    model_check(&m, "multi-stale: after cover");
    ok(m.rx_next == 300 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 300,
       "multi-stale: all stale nodes freed", NULL);
}

static void case_overlapping_future(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 200, false) == DATA_TREE, "overlap: 100..300", NULL);
    ok(data(&m, 150, 100, false) == DATA_TREE, "overlap: 150..250", NULL);
    ok(m.rx_ooo_bytes == 300 && m.rx_ooo_frames == 2,
       "overlap: tree accounting", NULL);
    ok(data(&m, 0, 150, false) == DATA_EXACT, "overlap: cover prefix", NULL);
    model_check(&m, "overlap: after prefix");
    /* [100,300) clips to [150,300); [150,250) becomes fully stale. */
    ok(m.rx_next == 300 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 &&
           m.rx_queued == 300,
       "overlap: clipped + stale node resolved", NULL);
}

/* Sixteen rounds of eight overlapping future frames, each round covered by
 * one exact frame that reaches the farthest tail.  Before the pull fix the
 * old `seq != rx_next` break left all eight nodes in the tree forever, so
 * rx_ooo_bytes never returned to zero. */
static void case_overlap_cycles(void)
{
    struct model m;
    int round, k;

    model_init(&m);
    for (round = 0; round < 16; round++)
    {
        uint64_t base = m.rx_next + 512;

        for (k = 0; k < 8; k++)
            ok(data(&m, base + (uint64_t)k * 64, 512, false) == DATA_TREE,
               "overlap-cycles: tree insert", NULL);
        model_check(&m, "overlap-cycles: tree");
        ok(m.rx_ooo_bytes == 8 * 512 && m.rx_ooo_frames == 8,
           "overlap-cycles: eight nodes", NULL);
        ok(data(&m, m.rx_next, 512 + 960, false) == DATA_EXACT,
           "overlap-cycles: cover", NULL);
        model_check(&m, "overlap-cycles: after cover");
        ok(m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0,
           "overlap-cycles: no residual tree", NULL);
        ok(m.rx_next == base + 960 && m.rx_queued == 512 + 960,
           "overlap-cycles: queued once", NULL);
        drain(&m);
    }
}

/* The entry straddle and the tree node overlap: B[50,250) is clipped to
 * [100,250), then A[200,400) is clipped to [250,400).  The shared range
 * [200,250) must be delivered exactly once. */
static void case_entry_straddle_over_tree(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 200, 200, false) == DATA_TREE, "entry-over-tree: A", NULL);
    ok(data(&m, 50, 200, false) == DATA_TREE, "entry-over-tree: B", NULL);
    ok(m.rx_ooo_bytes == 400 && m.rx_ooo_frames == 2,
       "entry-over-tree: two overlapping nodes", NULL);
    ok(data(&m, 0, 100, false) == DATA_EXACT, "entry-over-tree: prefix", NULL);
    model_check(&m, "entry-over-tree: after prefix");
    ok(m.rx_next == 400 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0,
       "entry-over-tree: drained", NULL);
    ok(m.rx_queued == 400, "entry-over-tree: queued once", NULL);
    ok(m.last_seq == 250 && m.last_len == 200 && m.last_off == 50,
       "entry-over-tree: A clipped by 50", NULL);
    drain(&m);
}

/* A full-stale node sandwiched between two straddles: A and B are dropped
 * whole, C is clipped by 20.  The order proves the pull does not subtract
 * an overlap twice. */
static void case_stale_between_straddles(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "between: A", NULL);
    ok(data(&m, 150, 100, false) == DATA_TREE, "between: B", NULL);
    ok(data(&m, 300, 100, false) == DATA_TREE, "between: C", NULL);
    ok(m.rx_ooo_bytes == 300 && m.rx_ooo_frames == 3,
       "between: three nodes", NULL);
    ok(data(&m, 0, 320, false) == DATA_EXACT, "between: cover", NULL);
    model_check(&m, "between: after cover");
    ok(m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0,
       "between: stale nodes freed", NULL);
    ok(m.rx_next == 400 && m.rx_queued == 400,
       "between: tail queued once", NULL);
    ok(m.last_seq == 320 && m.last_len == 100 && m.last_off == 20,
       "between: C clipped by 20", NULL);
    drain(&m);
}

/* The byte bound is the only thing that stops a peer that only sends future
 * frames.  Eight overlapping frames equal rx_limit and must not reset; the
 * ninth crosses it and bumps the counter exactly once. */
static void case_limit_is_bounded(void)
{
    struct model m;
    int round, k;

    model_init(&m);
    m.rx_limit = 4096;
    for (round = 0; round < 8; round++)
    {
        uint64_t base = m.rx_next + 512;

        for (k = 0; k < 8; k++)
            ok(data(&m, base + (uint64_t)k * 64, 512, false) == DATA_TREE,
               "limit: tree insert", NULL);
        ok(m.rx_ooo_bytes == 4096, "limit: peak equals limit", NULL);
        ok(m.rx_reorder_overflow == 0, "limit: equality does not reset", NULL);
        ok(data(&m, m.rx_next, 512 + 960, false) == DATA_EXACT,
           "limit: cover", NULL);
        model_check(&m, "limit: after cover");
        ok(m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0,
           "limit: drained", NULL);
        drain(&m);
    }
    {
        uint64_t base = m.rx_next + 512;

        for (k = 0; k < 9; k++)
            (void)data(&m, base + (uint64_t)k * 64, 512, false);
        ok(m.rx_reorder_overflow == 1, "limit: exactly one reset", NULL);
        ok(m.rx_ooo_bytes == 4608, "limit: over-limit bytes kept", NULL);
        ok(m.dead, "limit: stream dead after reset", NULL);
        ok(data(&m, base, 512, false) == DATA_DEAD,
           "limit: later frames are dead-dropped", NULL);
        ok(m.rx_reorder_overflow == 1, "limit: no second reset", NULL);
        model_check(&m, "limit: after reset");
    }
}

/* tree_insert() keys on the exact seq: a longer copy with the same start is
 * a duplicate, not a replacement.  It must not grow the counters. */
static void case_same_seq_different_len(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "same-seq: [100,200)", NULL);
    ok(data(&m, 100, 200, false) == DATA_DUP, "same-seq: longer is a dup", NULL);
    model_check(&m, "same-seq: after duplicate");
    ok(m.rx_ooo_bytes == 100 && m.rx_ooo_frames == 1 && m.dups == 1,
       "same-seq: no growth", NULL);
    ok(data(&m, 0, 200, false) == DATA_EXACT, "same-seq: cover", NULL);
    ok(m.rx_next == 200 && m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0,
       "same-seq: drained", NULL);
    drain(&m);
}

/* tbt_rx_free_queue() frees both queues and clears the three counters but
 * leaves rx_next/rx_delivered untouched.  Do not run model_check() after
 * this point: the kernel has already killed the stream. */
static void case_free_queue(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 100, 100, false) == DATA_TREE, "free-queue: tree", NULL);
    ok(data(&m, 0, 50, false) == DATA_EXACT, "free-queue: prefix", NULL);
    ok(data(&m, 20, 40, false) == DATA_CLIPPED, "free-queue: clip", NULL);
    ok(m.rx_ooo_bytes == 100 && m.rx_ooo_frames == 1,
       "free-queue: tree accounting", NULL);
    ok(m.rx_queued == 60 && m.rx_next == 60,
       "free-queue: queued bytes", NULL);
    ok(m.last_seq == 50 && m.last_len == 40 && m.last_off == 30,
       "free-queue: off counted once", NULL);
    model_free_queue(&m);
    ok(m.rx_ooo_bytes == 0 && m.rx_ooo_frames == 0 && m.rx_queued == 0 &&
           m.ntree == 0,
       "free-queue: counters cleared", NULL);
    ok(m.rx_next == 60, "free-queue: rx_next untouched", NULL);
}

static void case_low_prio(void)
{
    struct model m;

    model_init(&m);
    ok(data(&m, 0, 100, true) == DATA_EXACT, "low-prio: exact", NULL);
    ok(m.dup_used == 1, "low-prio: exact counts", NULL);
    ok(data(&m, 100, 100, true) == DATA_EXACT, "low-prio: second", NULL);
    ok(m.dup_used == 2, "low-prio: second counts", NULL);
    ok(data(&m, 150, 100, true) == DATA_CLIPPED, "low-prio: clipped", NULL);
    ok(m.dup_used == 3, "low-prio: clipped counts", NULL);
    ok(data(&m, 100, 100, true) == DATA_STALE, "low-prio: stale drops", NULL);
    ok(m.dup_used == 3, "low-prio: stale does not count", NULL);
    ok(data(&m, 300, 100, true) == DATA_TREE, "low-prio: tree", NULL);
    ok(m.dup_used == 4, "low-prio: tree insert counts", NULL);
    ok(data(&m, 250, 50, true) == DATA_EXACT, "low-prio: fill", NULL);
    ok(m.dup_used == 5, "low-prio: fill counts", NULL);
    model_check(&m, "low-prio");
}

static void case_random(void)
{
    struct model m;
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    int i;

    model_init(&m);
    for (i = 0; i < MAX_RANDOM_OPS; i++)
    {
        uint32_t len = 1 + rnd_below(&rng, 100);
        uint64_t seq;
        uint32_t kind = rnd_below(&rng, 4);

        switch (kind)
        {
        case 0:
            seq = m.rx_next; /* exact */
            break;
        case 1:
            seq = m.rx_next > 200 ? m.rx_next - rnd_below(&rng, 200) : 0;
            break;
        case 2:
            seq = m.rx_next + rnd_below(&rng, 500);
            break;
        default:
            seq = m.rx_next > 100 ? m.rx_next - rnd_below(&rng, 100) : 0;
            break;
        }
        if (m.ntree >= MAX_TREE - 1)
        {
            /* The sorted array is the model's capacity, not a kernel rule.
             * Fill the hole up to the head node's tail instead of advancing
             * one byte: one exact frame consumes the head and pull() then
             * drains whatever overlaps it, so the tree shrinks without
             * starving the random paths. */
            uint32_t fill = (uint32_t)(m.tree[0].seq - m.rx_next) + m.tree[0].len;

            (void)data(&m, m.rx_next, fill, false);
            drain(&m);
        }
        else
        {
            (void)data(&m, seq, len, false);
        }
        model_check(&m, "random");
        if (rnd_below(&rng, 2))
            drain(&m);
    }
}

int main(void)
{
    case_exact();
    case_hole_fill();
    case_full_stale_tree();
    case_straddle_tree();
    case_entry_straddle();
    case_duplicate_tree();
    case_duplicate_tail();
    case_multiple_stale();
    case_overlapping_future();
    case_overlap_cycles();
    case_entry_straddle_over_tree();
    case_stale_between_straddles();
    case_limit_is_bounded();
    case_same_seq_different_len();
    case_free_queue();
    case_low_prio();
    case_random();

    printf("test_reorder: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
