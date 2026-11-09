/* test_fec_switch.c - the per-flow contract of a stream switch.
 *
 * tbt_fec_data.c owns the flow table, the carrier batch writer and the RX
 * delivery walk; it pulls in the kernel headers and cannot be linked here.
 * The decisions that let one stream replace another without carrying state
 * across are pure, so they live in tbt_fec.h and are exercised here against
 * models of those three paths:
 *
 *   tbt_fec_flow_live()         a detached flow accepts no new work;
 *   tbt_fec_out_unlink_once()   purge and an in-flight commit unlink once;
 *   tbt_fec_blk_sid_match()     another stream's block never delivers;
 *   tbt_fec_rx_window_step()    the new stream rebases its reorder window;
 *   tbt_fec_flow_create_code()  one SC-LDPC flow, duplicates first.
 *
 * The field failure these models pin is a switch under backpressure: the old
 * stream's data was still queued and its block still resident when the new
 * stream arrived. No byte of the old stream may reach the new one, the old
 * records must die with their flow, and a third connection must start with
 * fresh TX/RX state again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_fec_ms.h"

#define S 64   /* the model uses a short slot; the code is size-agnostic */
#define N 4    /* data shares per block */
#define M 4    /* parity shares per block */
#define SLOTS 8 /* receiver reorder blocks, from the arena */
/* tbtun.h's TBT_FEC_BATCH_MAX; that header cannot be included here. */
#define BATCH_MAX 3

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

static void ok_int(int got, int want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %d, want %d\n", what, got, want);
}

/* ------------------------------------------------------------------ *
 * 1. One stream's ready block, ring and queue belong to that stream.
 * ------------------------------------------------------------------ */

struct blk
{
    u64 sid;  /* the stream the block was built for */
    u64 id;   /* block id, from the session packet sequence */
    bool ready;
    u8 data[S];
    u16 len;
};

/* The delivery walk of tbt_fec_deliver_ready(), reduced to one block. A
 * block another stream owns contributes nothing, whatever bytes it holds. */
static int blk_deliver(const struct blk *b, u64 stream_sid, u8 *out,
                       size_t cap)
{
    if (!b->ready || !tbt_fec_blk_sid_match(b->sid, stream_sid))
        return 0;
    if (b->len > cap)
        return 0;
    memcpy(out, b->data, b->len);
    return b->len;
}

static void blk_init(struct blk *b, u64 sid, u64 id, const char *body)
{
    memset(b, 0, sizeof(*b));
    b->sid = sid;
    b->id = id;
    b->ready = true;
    b->len = (u16)strlen(body);
    memcpy(b->data, body, b->len);
}

static void test_flow_isolation(void)
{
    const u64 sid_a = 0x0a;
    const u64 sid_b = 0x0b;
    struct tbt_fec_replay *ra, *rb;
    struct blk a, b;
    u8 share[S];
    u8 out[2 * S];
    int n;

    /* A's block is ready when the stream is replaced; B has no block yet. */
    blk_init(&a, sid_a, 7, "OLD-BODY");
    blk_init(&b, sid_b, 140, "HTTP/1.1");

    ok(tbt_fec_flow_live(1), "switch: a live flow accepts work");
    ok(!tbt_fec_flow_live(3), "switch: a detached flow accepts none");

    /* The old block is resident and would deliver every byte if the sid were
     * ignored. The sid check is what keeps it out of the new stream. */
    ok(!tbt_fec_blk_sid_match(a.sid, sid_b),
       "switch: A's block never matches B's sid");
    ok(tbt_fec_blk_sid_match(b.sid, sid_b), "switch: B's block matches B");
    ok(tbt_fec_blk_sid_match(a.sid, 0),
       "switch: an unadopted stream has no sid to compare");
    ok(tbt_fec_blk_sid_match(a.sid, sid_a), "switch: A's block matches A");

    n = blk_deliver(&a, sid_b, out, sizeof(out));
    ok_int(n, 0, "switch: B receives nothing from A's ready block");
    n = blk_deliver(&b, sid_b, out, sizeof(out));
    ok_int(n, 8, "switch: B receives its own first block");
    ok(!memcmp(out, "HTTP/1.1", 8), "switch: B's first bytes are its own");

    /* The old block also cannot fill B's frontier under a stale block id. */
    ok(!tbt_fec_blk_deliverable(true, a.ready, a.id, b.id),
       "switch: A's block id is not B's frontier");
    ok(tbt_fec_blk_deliverable(true, b.ready, b.id, b.id),
       "switch: B's block is deliverable at its own frontier");

    /* Each flow owns its replay ring. A share resident for A is not resident
     * for B, and purging A's ring leaves B's ring alone. */
    ra = tbt_fec_replay_new(1, N, M, S);
    rb = tbt_fec_replay_new(1, N, M, S);
    ok(ra != NULL && rb != NULL, "switch: one replay ring per flow");
    if (!ra || !rb)
        exit(2);
    memset(share, 0xa5, sizeof(share));
    ok_int(tbt_fec_replay_store(ra, a.id, 0, 0, share, S), 0,
           "switch: A stores a share in A's ring");
    ok_int(tbt_fec_replay_get(rb, a.id, 0, 0, share, S), -ESTALE,
           "switch: B's ring holds no A block");
    ok_int(tbt_fec_replay_get(ra, a.id, 0, 0, share, S), S,
           "switch: A's ring still serves A while A lives");
    tbt_fec_replay_reset(ra); /* the detach frees the old flow's ring */
    ok_int(tbt_fec_replay_get(ra, a.id, 0, 0, share, S), -ESTALE,
           "switch: A's ring drops its blocks on purge");
    ok_int(tbt_fec_replay_store(rb, b.id, 0, 0, b.data, S), 0,
           "switch: B stores its own first share");
    ok_int(tbt_fec_replay_get(rb, b.id, 0, 0, share, S), S,
           "switch: B's ring serves B's own share");
    tbt_fec_replay_free(ra);
    tbt_fec_replay_free(rb);
}

/* ------------------------------------------------------------------ *
 * 2. A flow purge and an in-flight batch commit unlink a record once.
 * ------------------------------------------------------------------ */

struct mflow
{
    int refs; /* table/stream plus one per resident record */
    u8 state;
};

struct mrec
{
    struct mflow *flow;
    int refs; /* one queue reference, plus one per batch peek */
    bool queued;
};

struct mq
{
    struct mrec *rec[BATCH_MAX];
    int n;
};

static int rec_frees;
static int flow_puts;

static void mrec_init(struct mrec *r, struct mflow *f)
{
    memset(r, 0, sizeof(*r));
    r->flow = f;
    r->refs = 1;
    r->queued = true;
    f->refs++;
}

static void mrec_put(struct mrec *r)
{
    if (--r->refs != 0)
        return;
    r->flow->refs--;
    flow_puts++;
    rec_frees++;
}

/* The queue reference goes exactly once: purge and a commit can both reach
 * the record, and the loser must not unlink or charge it twice. */
static int mrec_unlink(struct mrec *r)
{
    if (!tbt_fec_out_unlink_once(&r->queued))
        return 0;
    mrec_put(r);
    return 1;
}

static void mq_remove(struct mq *q, const struct mrec *r)
{
    int i;

    for (i = 0; i < q->n; i++)
    {
        if (q->rec[i] != r)
            continue;
        for (; i + 1 < q->n; i++)
            q->rec[i] = q->rec[i + 1];
        q->n--;
        return;
    }
}

/* tbt_fec_out_batch_peek(): take one reader reference per head record. */
static int mq_peek(struct mq *q, struct mrec **outs, int max)
{
    int i;

    for (i = 0; i < q->n && i < max; i++)
    {
        outs[i] = q->rec[i];
        outs[i]->refs++;
    }
    return i;
}

/* tbt_fec_flow_purge(): every record of the dying flow leaves its queue. */
static int mq_purge(struct mq *q, struct mflow *f)
{
    int i = 0, n = 0;

    while (i < q->n)
    {
        struct mrec *r = q->rec[i];

        if (r->flow != f)
        {
            i++;
            continue;
        }
        mq_remove(q, r);
        if (mrec_unlink(r))
            n++;
    }
    return n;
}

/* tbt_fec_out_batch_commit(): only a record still at the head is unlinked
 * and freed. A record another flow has queued behind it is never touched. */
static void mq_commit(struct mq *q, struct mrec **outs, int n, int sent)
{
    int i;

    for (i = 0; i < n; i++)
    {
        struct mrec *r = outs[i];

        if (i < sent && q->n && q->rec[0] == r)
        {
            mq_remove(q, r);
            mrec_unlink(r);
        }
        mrec_put(r); /* the peek reference */
    }
}

static void mq_reset(struct mq *q, struct mrec *a1, struct mrec *a2,
                     struct mrec *b1, struct mflow *fa, struct mflow *fb)
{
    memset(q, 0, sizeof(*q));
    memset(fa, 0, sizeof(*fa));
    memset(fb, 0, sizeof(*fb));
    fa->state = 1;
    fb->state = 1;
    mrec_init(a1, fa);
    mrec_init(a2, fa);
    mrec_init(b1, fb);
    q->rec[0] = a1;
    q->rec[1] = a2;
    q->rec[2] = b1;
    q->n = 3;
    rec_frees = 0;
    flow_puts = 0;
}

/* tbt_fec_out_push(): a dead flow refuses the record before it is born. */
static int mrec_enqueue(struct mflow *f)
{
    if (!tbt_fec_flow_live(f->state))
        return -ESHUTDOWN;
    f->refs++;
    return 0;
}

static void test_purge_vs_commit(void)
{
    struct mflow fa, fb;
    struct mrec a1, a2, b1;
    struct mq q;
    struct mrec *outs[BATCH_MAX];
    int n, refs;

    /* Purge wins the race: both A records unlink, the in-flight batch then
     * finds B's record at the head and must leave it there. */
    mq_reset(&q, &a1, &a2, &b1, &fa, &fb);
    n = mq_peek(&q, outs, BATCH_MAX);
    ok_int(n, 3, "purge: the batch peeks three records");
    ok_int(mq_purge(&q, &fa), 2, "purge: A's two records unlink");
    mq_commit(&q, outs, n, 2);
    ok_int(q.n, 1, "purge: B's record is still queued");
    ok(q.rec[0] == &b1, "purge: the queue head is B's record");
    ok_int(b1.refs, 1, "purge: B's queue reference survives the commit");
    ok_int(fb.refs, 1, "purge: B's flow reference survives the commit");
    ok_int(fa.refs, 0, "purge: A is freed after its last record");
    ok_int(rec_frees, 2, "purge: each A record frees once");
    ok_int(flow_puts, 2, "purge: A is put once per record");

    /* Commit wins: A's records are unlinked by the accepted batch, and the
     * later purge is a no-op with the same totals. */
    mq_reset(&q, &a1, &a2, &b1, &fa, &fb);
    n = mq_peek(&q, outs, BATCH_MAX);
    mq_commit(&q, outs, n, 2);
    ok_int(q.n, 1, "commit: B's record is still queued");
    ok_int(mq_purge(&q, &fa), 0, "commit: the late purge finds nothing");
    ok_int(b1.refs, 1, "commit: B's queue reference survives");
    ok_int(fb.refs, 1, "commit: B's flow reference survives");
    ok_int(fa.refs, 0, "commit: A is freed after its last record");
    ok_int(rec_frees, 2, "commit: each A record frees once");
    ok_int(flow_puts, 2, "commit: A is put once per record");

    /* A second unlink of the same record is a no-op, never a double charge. */
    mq_reset(&q, &a1, &a2, &b1, &fa, &fb);
    ok_int(mrec_unlink(&a1), 1, "unlink: the first call removes the record");
    ok_int(mrec_unlink(&a1), 0, "unlink: the second call does nothing");
    ok_int(rec_frees, 1, "unlink: the record frees exactly once");
    ok_int(fa.refs, 1, "unlink: the remaining A record still holds A");

    /* A push for a detached flow is refused before it takes a reference. */
    fa.state = 3;
    refs = fa.refs;
    ok_int(mrec_enqueue(&fa), -ESHUTDOWN,
           "push: a detached flow refuses new work");
    ok_int(fa.refs, refs, "push: the refusal takes no flow reference");
    ok_int(mrec_enqueue(&fb), 0, "push: the new flow accepts new work");
    ok_int(fb.refs, 2, "push: the new record holds the new flow");
}

/* ------------------------------------------------------------------ *
 * 3. Rapid reconnect A -> B -> C: every generation starts fresh.
 * ------------------------------------------------------------------ */

static void test_rapid_reconnect(void)
{
    const u64 sid_a = 0xa1;
    const u64 sid_b = 0xb2;
    const u64 sid_c = 0xc3;
    struct tbt_fec_replay *ra, *rb, *rc;
    struct mflow fa, fb;
    struct mrec oa;
    struct blk a, b, c;
    u8 share[S];
    u8 out[S];
    int n;

    blk_init(&a, sid_a, 100, "AAA");
    blk_init(&b, sid_b, 140, "BBB");
    blk_init(&c, sid_c, 200, "CCC");

    /* A delivered through block 100 before it was torn down. The inherited
     * cursor would call B's first block (140) far outside its window; the
     * new stream's first record must rebase the window instead. */
    ok_int(tbt_fec_rx_window_step(101, 140, SLOTS, false, false),
           TBT_FEC_RX_WIN_FAR,
           "reconnect: the stale cursor drops B's first block");
    ok_int(tbt_fec_rx_window_step(101, 140, SLOTS, true, false),
           TBT_FEC_RX_WIN_REBASE,
           "reconnect: B's first record rebases the window");

    n = blk_deliver(&a, sid_b, out, sizeof(out));
    ok_int(n, 0, "reconnect: A's resident block is not B's first bytes");
    n = blk_deliver(&b, sid_b, out, sizeof(out));
    ok_int(n, 3, "reconnect: B delivers its own first block");
    ok(!memcmp(out, "BBB", 3), "reconnect: B's bytes are its own");
    ok(!tbt_fec_blk_deliverable(true, a.ready, a.id, 140),
       "reconnect: A's block is not deliverable at B's frontier");
    ok(tbt_fec_blk_deliverable(true, b.ready, b.id, 140),
       "reconnect: B's block is deliverable at its frontier");

    /* B is admitted while an A record is still peeked by the batch writer.
     * The record keeps A's planes alive; the detach purge unlinks it; the
     * late commit only drops the peek, never B's state. */
    memset(&fa, 0, sizeof(fa));
    fa.state = 1;
    mrec_init(&oa, &fa);
    oa.refs++; /* the batch writer holds one reader reference */
    fa.state = 3; /* detach publishes the death */
    ok(!tbt_fec_flow_live(fa.state), "reconnect: A is detached");
    ok_int(mrec_unlink(&oa), 1, "reconnect: the purge unlinks A's record");
    ok_int(fa.refs, 1, "reconnect: the in-flight batch keeps A alive");

    memset(&fb, 0, sizeof(fb));
    fb.state = 1;
    ok(tbt_fec_flow_live(fb.state), "reconnect: B is admitted");
    ok_int(mrec_enqueue(&fb), 0, "reconnect: B accepts a new record");
    mrec_put(&oa); /* the commit's reader reference */
    ok_int(fa.refs, 0, "reconnect: the last put frees A");
    ok_int(fb.refs, 1, "reconnect: B's state is untouched");

    /* B -> C: B delivered through 140, so its cursor is 141. C's first block
     * at 200 is again outside the old window and must rebase. */
    ok_int(tbt_fec_rx_window_step(141, 200, SLOTS, false, false),
           TBT_FEC_RX_WIN_FAR,
           "reconnect: B's cursor would drop C's first block");
    ok_int(tbt_fec_rx_window_step(141, 200, SLOTS, true, false),
           TBT_FEC_RX_WIN_REBASE,
           "reconnect: C's first record rebases the window");
    n = blk_deliver(&b, sid_c, out, sizeof(out));
    ok_int(n, 0, "reconnect: B's block is not C's first bytes");
    n = blk_deliver(&c, sid_c, out, sizeof(out));
    ok_int(n, 3, "reconnect: C delivers its own first block");
    ok(!memcmp(out, "CCC", 3), "reconnect: C's bytes are its own");
    n = blk_deliver(&c, sid_b, out, sizeof(out));
    ok_int(n, 0, "reconnect: C's block never returns to B");

    /* A stream in its tombstone window is not usable even while the state
     * still says OPEN; the packetizer must not run for it. */
    ok(!tbt_fec_tx_stream_usable(true, true, 0),
       "reconnect: a dead stream is not usable");
    ok(!tbt_fec_tx_stream_usable(false, false, 0),
       "reconnect: a detached stream is not usable");
    ok(!tbt_fec_tx_stream_usable(false, true, 1),
       "reconnect: a tombstoned stream is not usable");
    ok(tbt_fec_tx_stream_usable(false, true, 0),
       "reconnect: the new stream is usable");

    /* Three generations, three rings. A's purge and B's purge drop only
     * their own blocks; C's ring starts empty. */
    ra = tbt_fec_replay_new(1, N, M, S);
    rb = tbt_fec_replay_new(1, N, M, S);
    rc = tbt_fec_replay_new(1, N, M, S);
    ok(ra != NULL && rb != NULL && rc != NULL,
       "reconnect: one replay ring per generation");
    if (!ra || !rb || !rc)
        exit(2);
    memset(share, 0x11, sizeof(share));
    ok_int(tbt_fec_replay_store(ra, a.id, 0, 0, share, S), 0,
           "reconnect: A stores its block");
    ok_int(tbt_fec_replay_get(rb, a.id, 0, 0, share, S), -ESTALE,
           "reconnect: B's ring holds no A block");
    tbt_fec_replay_reset(ra);
    ok_int(tbt_fec_replay_get(ra, a.id, 0, 0, share, S), -ESTALE,
           "reconnect: A's ring drops its blocks on purge");
    ok_int(tbt_fec_replay_store(rb, b.id, 0, 0, share, S), 0,
           "reconnect: B stores its block");
    ok_int(tbt_fec_replay_get(rb, b.id, 0, 0, share, S), S,
           "reconnect: B's ring serves its own block");
    tbt_fec_replay_reset(rb);
    ok_int(tbt_fec_replay_get(rc, b.id, 0, 0, share, S), -ESTALE,
           "reconnect: C's ring holds no B block");
    ok_int(tbt_fec_replay_store(rc, c.id, 0, 0, share, S), 0,
           "reconnect: C stores its block");
    ok_int(tbt_fec_replay_get(rc, c.id, 0, 0, share, S), S,
           "reconnect: C's ring serves C's own block");
    tbt_fec_replay_free(ra);
    tbt_fec_replay_free(rb);
    tbt_fec_replay_free(rc);
}

/* ------------------------------------------------------------------ *
 * 4. An SC-LDPC session admits one flow at a time; a replacement is
 *    admitted only after the old flow detaches. A duplicate sid wins over
 *    every admission rule.
 * ------------------------------------------------------------------ */

static void test_sclc_admission(void)
{
    u8 m8[TBT_FEC_BITMAP_BYTES];
    u8 m8b[TBT_FEC_BITMAP_BYTES];
    u8 mhigh[TBT_FEC_BITMAP_BYTES];
    u8 empty[TBT_FEC_BITMAP_BYTES];
    u16 map[TBT_FEC_MAX_GROUPS];
    u16 count;
    u16 i;

    memset(m8, 0, sizeof(m8));
    memset(m8b, 0, sizeof(m8b));
    memset(mhigh, 0, sizeof(mhigh));
    memset(empty, 0, sizeof(empty));
    for (i = 0; i < 8; i++)
    {
        tbt_fec_bitmap_set(m8, i);
        tbt_fec_bitmap_set(m8b, (u16)(i + 8));
    }
    tbt_fec_bitmap_set(mhigh, 200);

    /* A bitmap is valid only with at least one group, and a non-SC-LDPC
     * session cannot name a group past its local pool. SC-LDPC advertises
     * the peer's TX groups against the 256-bit wire ceiling. */
    count = 0;
    ok(!tbt_fec_group_mask_valid(empty, 32, &count), "mask: empty is invalid");
    ok(!tbt_fec_group_mask_valid(m8, 0, &count), "mask: no groups is invalid");
    ok(!tbt_fec_group_mask_valid(mhigh, 32, &count),
       "mask: a local group past the pool is invalid");
    ok(tbt_fec_group_mask_valid(m8, 32, &count), "mask: eight groups are valid");
    ok_int((int)count, 8, "mask: the count is the bits set");
    ok(tbt_fec_open_mask_valid(true, 32, mhigh, &count),
       "mask: SC-LDPC may name a peer group past the local pool");
    ok_int((int)count, 1, "mask: the wire ceiling still counts bits");
    ok(!tbt_fec_open_mask_valid(false, 32, mhigh, &count),
       "mask: RLNC keeps the local pool");
    ok_int((int)tbt_fec_group_map_build(m8, 32, map), 8,
       "map: every set bit maps in order");
    ok(map[0] == 0 && map[7] == 7, "map: the local order is ascending");

    /* One plane, one bitmap: every stream's OPEN must repeat the same
     * 32-byte mask. A differing mask is refused before it can attach. */
    ok(!tbt_fec_primary_mask_match(true, m8, m8b),
       "primary: SC-LDPC refuses a differing peer bitmap");
    ok(tbt_fec_primary_mask_match(true, m8, m8),
       "primary: SC-LDPC keeps the bound bitmap");
    ok(tbt_fec_primary_mask_match(false, m8, m8), "primary: RLNC keeps its mask");
    ok(!tbt_fec_primary_mask_match(false, m8, m8b),
       "primary: RLNC refuses a different mask");
    ok(tbt_fec_open_groups_match(true, m8, 8),
       "open: the INIT group count matches the bitmap");
    ok(!tbt_fec_open_groups_match(true, m8, 32),
       "open: a bogus INIT group count is refused");
    ok(tbt_fec_open_groups_match(false, m8, 32),
       "open: RLNC has no SC-LDPC group-count rule");

    /* Admission ledger: 256 carriers deliver one OPEN. Every copy claims a
     * pending slot, the winner attaches, and the losers abort; the plane is
     * only unlinked when both counters are zero. */
    {
        struct tbt_ms_admit a;

        memset(&a, 0, sizeof(a));
        for (i = 0; i < 16; i++)
            tbt_ms_admit_claim(&a);
        ok_int((int)a.pending, 16, "admit: sixteen carrier claims counted");
        ok(!tbt_ms_admit_last(&a), "admit: a pending slot keeps the plane");
        tbt_ms_admit_attach(&a);
        for (i = 0; i < 15; i++)
            tbt_ms_admit_abort(&a);
        ok_int((int)a.pending, 0, "admit: winner attaches, losers abort");
        ok_int((int)a.refs, 1, "admit: one mux leg is attached");
        ok(!tbt_ms_admit_last(&a), "admit: an attached leg keeps the plane");
        tbt_ms_admit_claim(&a);
        tbt_ms_admit_attach(&a);
        ok_int((int)a.refs, 2, "admit: streams share one plane");
        tbt_ms_admit_detach(&a);
        tbt_ms_admit_detach(&a);
        ok(tbt_ms_admit_last(&a), "admit: the last detach frees the plane");
    }

    /* The replacement starts on its own sid and window; the old flow's block
     * is not deliverable to it. */
    ok(!tbt_fec_blk_sid_match(0x51, 0x52),
       "sclc: the old flow's block is not the replacement's");
    ok_int(tbt_fec_rx_window_step(9, 40, SLOTS, false, false),
           TBT_FEC_RX_WIN_FAR,
           "sclc: the inherited cursor would drop the replacement's block");
    ok_int(tbt_fec_rx_window_step(9, 40, SLOTS, true, false),
           TBT_FEC_RX_WIN_REBASE,
           "sclc: the replacement's first record rebases the window");

    ok(tbt_fec_groups_fit(32, 16, 16), "groups: the remainder fits");
    ok(!tbt_fec_groups_fit(32, 32, 1), "groups: a full pool fits nothing");
    ok(!tbt_fec_groups_fit(32, 33, 1), "groups: used above the pool cannot fit");
    ok(!tbt_fec_groups_fit(32, 0, 0), "groups: zero is never a fit");
}

int main(void)
{
    test_flow_isolation();
    test_purge_vs_commit();
    test_rapid_reconnect();
    test_sclc_admission();

    printf("test_fec_switch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
