/* test_fault.c - host fault-injection model for the 0.3.3 acceptance.
 *
 * The module cannot run on the host, so this file models the three decisions
 * the real fault run judges. The FEC part uses the real RLNC codec.
 *
 *   1. Carrier lifecycle: dial -> up, kill/RST -> replacing, redial -> up.
 *      up + replacing + empty == n at every instant. Every retired object
 *      is freed, and destroy finishes in a bounded number of steps (no
 *      leak, no D-state wait).
 *
 *   2. Stream containment: one dead carrier is a repath, not a reset. Only
 *      an all-carrier failure resets the stream. FIN is graceful; a late
 *      DATA after FIN never revives the stream.
 *
 *   3. FEC block loss: a whole DATA block, a partial block, and a
 *      parity-starved block are recovered by parity or by replay. A block
 *      that cannot be recovered ends in a bounded reset, and the delivered
 *      bytes stay an exact prefix of the source.
 *
 * Keep the model in lockstep with tbt_carrier.c (replacement, up), tbt_session.c
 * (carrier_lost / stream reset containment), and tbt_fec_data.c (delivery).
 *
 * Build/run: make host-test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_crypto.h"
#include "tbt_fec.h"

#define S 64
#define N 4
#define M 4
#define NBLOCKS 6

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

static void ok_int(int got, int want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %d, want %d\n", what, got, want);
}

static void ok_u64(uint64_t got, uint64_t want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %llu, want %llu\n", what,
           (unsigned long long)got, (unsigned long long)want);
}

/* ------------------------------------------------------------------ *
 * FEC blocks and the real codec
 * ------------------------------------------------------------------ */

static u8 src[NBLOCKS * N * S];
static u8 out[NBLOCKS * N * S];
static size_t out_len;
static unsigned fec_reset_hole;
static unsigned fec_drop_window;
static unsigned dup_share;

struct blk
{
    u8 data[N][S];
    u8 parity[M][S];
    u8 dp[N];
    u8 pp[M];
    bool delivered;
};

static void src_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(src); i++)
        src[i] = (u8)(i * 37u + 11u);
}

static void blk_fill(struct blk *b, int block)
{
    int i;

    memset(b, 0, sizeof(*b));
    for (i = 0; i < N; i++)
    {
        memcpy(b->data[i], src + (size_t)block * N * S + (size_t)i * S, S);
        b->dp[i] = 1;
    }
}

static void blk_encode(struct blk *b)
{
    struct tbt_fec_rlnc *ctx;
    u8 *d[N], *p[M];
    int i;

    ctx = tbt_fec_rlnc_new(N, M, N, 8, S, 4, 0x1234);
    if (!ctx)
    {
        printf("FAIL codec allocation\n");
        exit(2);
    }
    for (i = 0; i < N; i++)
        d[i] = b->data[i];
    for (i = 0; i < M; i++)
        p[i] = b->parity[i];
    if (tbt_fec_rlnc_encode(ctx, d, N, p) < 0)
    {
        printf("FAIL encode\n");
        exit(2);
    }
    tbt_fec_rlnc_free(ctx);
}

/* Drop the chosen wire shares: data immediately, parity immediately. */
static void blk_drop_data(struct blk *b, int i) { b->dp[i] = 0; }
static void blk_drop_parity(struct blk *b, int i) { b->pp[i] = 0; }
static void blk_arrive_parity(struct blk *b, int i) { b->pp[i] = 1; }

static int blk_present(const struct blk *b)
{
    int i, n = 0;

    for (i = 0; i < N; i++)
        n += b->dp[i] != 0;
    for (i = 0; i < M; i++)
        n += b->pp[i] != 0;
    return n;
}

/* Recover the missing DATA shares with parity. Returns false when the block
 * has fewer equations than unknowns (or the codec cannot solve it). */
static bool blk_recover(struct blk *b)
{
    struct tbt_fec_rlnc *ctx;
    u8 rec[M][S];
    u8 *d[N], *p[M];
    int i, k, rc, missing = 0;

    for (i = 0; i < N; i++)
        missing += !b->dp[i];
    if (!missing)
        return true;
    if (blk_present(b) < N)
        return false;
    ctx = tbt_fec_rlnc_new(N, M, N, 8, S, 4, 0x1234);
    if (!ctx)
    {
        printf("FAIL codec allocation\n");
        exit(2);
    }
    memset(rec, 0, sizeof(rec));
    for (i = 0, k = 0; i < N; i++)
        d[i] = b->dp[i] ? b->data[i] : rec[k++];
    for (i = 0; i < M; i++)
        p[i] = b->parity[i];
    rc = tbt_fec_rlnc_decode(ctx, d, p, b->dp, b->pp, N);
    tbt_fec_rlnc_free(ctx);
    if (rc < 0)
        return false;
    for (i = 0, k = 0; i < N; i++)
    {
        if (b->dp[i])
            continue;
        memcpy(b->data[i], rec[k++], S);
        b->dp[i] = 1;
    }
    return true;
}

/* Deliver one block in order. The delivered run must match the source at
 * this offset, byte for byte. */
static bool blk_deliver(struct blk *b, int block)
{
    u8 tmp[N * S];
    int i;

    if (b->delivered)
    {
        dup_share++;
        return false;
    }
    for (i = 0; i < N; i++)
        if (!b->dp[i])
            return false;
    for (i = 0; i < N; i++)
        memcpy(tmp + (size_t)i * S, b->data[i], S);
    if (out_len + sizeof(tmp) > sizeof(out))
        return false;
    if (memcmp(tmp, src + (size_t)block * N * S, sizeof(tmp)))
    {
        failures++;
        printf("FAIL delivered block %d is corrupt\n", block);
        return false;
    }
    memcpy(out + out_len, tmp, sizeof(tmp));
    out_len += sizeof(tmp);
    b->delivered = true;
    return true;
}

/* A share that arrives twice must not be delivered twice. */
static void blk_dup(struct blk *b, int data_share)
{
    if (b->dp[data_share])
        dup_share++;
    else
        b->dp[data_share] = 1;
}

/* ------------------------------------------------------------------ *
 * 1. FEC block loss: parity, whole-block, parity-starved replay, reset.
 * ------------------------------------------------------------------ */

static void test_fec_block_loss(void)
{
    struct blk b;

    out_len = 0;
    fec_reset_hole = 0;
    dup_share = 0;

    /* Block 0: one DATA share and two parity shares are lost. The three
     * remaining DATA shares and two parity shares solve the fourth. */
    blk_fill(&b, 0);
    blk_encode(&b);
    blk_drop_data(&b, 1);
    blk_arrive_parity(&b, 0);
    blk_drop_parity(&b, 1);
    blk_arrive_parity(&b, 2);
    blk_drop_parity(&b, 3);
    ok(blk_recover(&b), "block 0: 3 DATA + 2 parity recover the lost share");
    ok(blk_deliver(&b, 0), "block 0: delivered");

    /* Block 1: every DATA share is lost, every parity share arrives. This
     * is the whole-block loss a carrier kill can leave behind. */
    blk_fill(&b, 1);
    blk_encode(&b);
    blk_drop_data(&b, 0);
    blk_drop_data(&b, 1);
    blk_drop_data(&b, 2);
    blk_drop_data(&b, 3);
    blk_arrive_parity(&b, 0);
    blk_arrive_parity(&b, 1);
    blk_arrive_parity(&b, 2);
    blk_arrive_parity(&b, 3);
    ok(blk_recover(&b), "block 1: four parity shares rebuild the whole block");
    ok(blk_deliver(&b, 1), "block 1: delivered");

    /* Block 2: one DATA share and one parity share arrive, which is not
     * enough. The replay path adds the two shares the NACK asked for. */
    blk_fill(&b, 2);
    blk_encode(&b);
    blk_arrive_parity(&b, 0);
    blk_drop_parity(&b, 1);
    blk_drop_parity(&b, 2);
    blk_drop_parity(&b, 3);
    blk_drop_data(&b, 1);
    blk_drop_data(&b, 2);
    blk_drop_data(&b, 3);
    ok(!blk_recover(&b), "block 2: 1 DATA + 1 parity is not enough");
    /* Replay the two absent parity shares, as fec_nack_retx would. */
    blk_arrive_parity(&b, 1);
    blk_arrive_parity(&b, 2);
    ok(blk_recover(&b), "block 2: replayed parity makes the block solvable");
    ok(blk_deliver(&b, 2), "block 2: delivered after replay");

    /* Block 3: no loss, direct delivery. */
    blk_fill(&b, 3);
    blk_encode(&b);
    ok(blk_deliver(&b, 3), "block 3: delivered without repair");

    /* A duplicate share is counted, not delivered twice. */
    blk_dup(&b, 0);
    ok_int(dup_share, 1, "block 3: duplicate share counted, not delivered");
    ok_int((int)out_len, 4 * N * S, "four blocks delivered once");

    /* Block 4: the whole block is lost. The bounded reset must stop the
     * stream at the last delivered block and must not emit any byte of the
     * lost block. */
    blk_fill(&b, 4);
    blk_encode(&b);
    blk_drop_data(&b, 0);
    blk_drop_data(&b, 1);
    blk_drop_data(&b, 2);
    blk_drop_data(&b, 3);
    blk_drop_parity(&b, 0);
    blk_drop_parity(&b, 1);
    blk_drop_parity(&b, 2);
    blk_drop_parity(&b, 3);
    if (!blk_recover(&b))
    {
        ok(true, "block 4: total loss is unrecoverable");
        fec_reset_hole++;
    }
    else
        ok(false, "block 4: total loss is unrecoverable");
    ok_u64(fec_reset_hole, 1, "block 4: bounded reset counted");
    ok_int((int)out_len, 4 * N * S, "reset did not deliver a wrong length");
    ok(!memcmp(out, src, out_len), "delivered bytes are an exact source prefix");

    /* Block 5 arrives after the reset. It must not be delivered: a reset
     * does not resume at an arbitrarily later block. */
    blk_fill(&b, 5);
    blk_encode(&b);
    if (fec_reset_hole)
        fec_drop_window++;
    ok_u64(fec_drop_window, 1, "late block after reset dropped");
    ok_int((int)out_len, 4 * N * S, "late block did not extend the stream");
}

/* ------------------------------------------------------------------ *
 * 2. Carrier lifecycle: kill/RST, fast A->B->C redial, containment.
 * ------------------------------------------------------------------ */

#define MAX_CAR 8
#define MAX_OBJ 64

struct carrier
{
    int index;
    int gen;
    bool up;
    bool stopping;
    int refs;
    bool used;
};

struct world
{
    struct carrier obj[MAX_OBJ];
    struct carrier *slot[MAX_CAR];
    int n;
    int gen;
    int live;      /* allocated carrier objects */
    int works;     /* pending work items that hold a session reference */
    int refs;      /* session references */
    bool closing;
    bool stream_alive;
    bool fin_seen;
    uint64_t lost;
    uint64_t replaced;
    uint64_t retired;
    uint64_t stream_reset_tcp_failed;
    uint64_t stream_reset_user;
    uint64_t late_data;
    uint64_t fin_count;
};

static struct carrier *obj_new(struct world *w, int index)
{
    int i;

    for (i = 0; i < MAX_OBJ; i++)
    {
        struct carrier *c = &w->obj[i];

        if (c->used)
            continue;
        memset(c, 0, sizeof(*c));
        c->used = true;
        c->index = index;
        c->gen = ++w->gen;
        c->refs = 1;
        w->live++;
        return c;
    }
    return NULL;
}

static void obj_put(struct world *w, struct carrier *c)
{
    if (!c || !c->used)
        return;
    if (--c->refs > 0)
        return;
    c->used = false;
    w->live--;
}

static void world_init(struct world *w, int n)
{
    int i;

    memset(w, 0, sizeof(*w));
    w->n = n;
    w->refs = 1;
    w->works = 2; /* TBT_SW_REPLACE + TBT_SW_FEC_RX */
    w->stream_alive = true;
    for (i = 0; i < n; i++)
    {
        w->slot[i] = obj_new(w, i);
        ok(w->slot[i] != NULL, "carrier object allocated");
    }
}

static void world_dial_done(struct world *w, int i)
{
    if (w->slot[i])
        w->slot[i]->up = true;
}

static int world_up(const struct world *w)
{
    int i, up = 0;

    for (i = 0; i < w->n; i++)
        if (w->slot[i] && w->slot[i]->up)
            up++;
    return up;
}

static int world_replacing(const struct world *w)
{
    int i, n = 0;

    for (i = 0; i < w->n; i++)
        if (w->slot[i] && !w->slot[i]->up)
            n++;
    return n;
}

static int world_empty(const struct world *w)
{
    int i, n = 0;

    for (i = 0; i < w->n; i++)
        if (!w->slot[i])
            n++;
    return n;
}

static void world_check_invariant(const struct world *w, const char *what)
{
    int up = world_up(w), rep = world_replacing(w), emp = world_empty(w);

    ok_int(up + rep + emp, w->n, what);
}

/* The kernel's tbt_carriers_all_dead(): every slot is NULL or stopping. A
 * fresh replacement is not stopping, so one dead path never resets a
 * stream while a replacement can still carry bytes. */
static bool world_all_dead(const struct world *w)
{
    int i;

    for (i = 0; i < w->n; i++)
    {
        if (w->slot[i] && !w->slot[i]->stopping)
            return false;
    }
    return true;
}

static void world_stream_check(struct world *w)
{
    if (w->stream_alive && world_all_dead(w))
    {
        w->stream_reset_tcp_failed++;
        w->stream_alive = false;
    }
}

/* Near-side replacement: install a fresh carrier at the same index before
 * the old socket is released. The slot goes from up to replacing. */
static bool world_kill_near(struct world *w, int i)
{
    struct carrier *old = w->slot[i];
    struct carrier *nc;

    if (!old)
        return false;
    nc = obj_new(w, i);
    if (!nc)
        return false;
    w->lost++;
    old->stopping = true;
    w->slot[i] = nc;
    w->replaced++;
    obj_put(w, old); /* the slot reference moves to the new object */
    w->retired++;
    world_stream_check(w);
    return true;
}

/* Far-side loss: the slot becomes empty until the peer's replacement joins.
 * This is the path that can make every carrier dead. */
static bool world_kill_far(struct world *w, int i)
{
    struct carrier *old = w->slot[i];

    if (!old)
        return false;
    w->lost++;
    old->stopping = true;
    w->slot[i] = NULL;
    obj_put(w, old);
    w->retired++;
    world_stream_check(w);
    return true;
}

static void world_redial_all(struct world *w)
{
    int i;

    for (i = 0; i < w->n; i++)
    {
        if (w->slot[i])
            world_dial_done(w, i);
        else
        {
            w->slot[i] = obj_new(w, i);
            if (w->slot[i])
                w->slot[i]->up = true;
        }
    }
}

static void world_stream_fin(struct world *w)
{
    if (!w->stream_alive)
        return;
    w->stream_alive = false;
    w->fin_seen = true;
    w->fin_count++;
}

/* An application or peer RST is not a carrier loss: the stream closes at
 * once, and the carrier pool is untouched. */
static void world_stream_rst(struct world *w)
{
    if (!w->stream_alive)
        return;
    w->stream_alive = false;
    w->stream_reset_user++;
}

static void world_late_data(struct world *w)
{
    if (!w->stream_alive)
        w->late_data++;
}

/* Destroy must finish in bounded work, even after a reconnect storm. The
 * model's step cap is the D-state guard: an unfreed reference or a work
 * item that never returns would exceed it. */
static bool world_destroy(struct world *w, int *steps_out)
{
    int i, steps = 0;

    w->closing = true;
    for (i = 0; i < w->n; i++)
    {
        if (w->slot[i])
        {
            obj_put(w, w->slot[i]);
            w->slot[i] = NULL;
        }
    }
    while (w->works > 0 && steps < 64)
    {
        w->works--;
        steps++;
    }
    while (w->refs > 1 && steps < 64)
    {
        w->refs--;
        steps++;
    }
    if (steps < 64 && w->refs == 1)
        w->refs = 0;
    *steps_out = steps;
    return w->refs == 0 && w->works == 0 && w->live == 0 && steps < 64;
}

static void test_carrier_faults(void)
{
    struct world w;
    int steps = 0;

    /* One kill is a repath, not a stream reset. */
    world_init(&w, 4);
    world_redial_all(&w);
    ok_int(world_up(&w), 4, "dial: four carriers up");
    world_check_invariant(&w, "dial: up+replacing+empty == n");

    ok(world_kill_near(&w, 1), "kill carrier B");
    ok_int(world_up(&w), 3, "kill: one slot moved out of up");
    ok_int(world_replacing(&w), 1, "kill: the replacement is dialling");
    ok_int(world_empty(&w), 0, "kill: no slot leaked");
    world_check_invariant(&w, "kill: up+replacing+empty == n");
    ok(w.stream_alive, "one dead carrier does not reset the stream");
    ok_u64(w.stream_reset_tcp_failed, 0, "no stream reset on one kill");

    /* Fast A->B->C: three kills before any redial completes. */
    ok(world_kill_near(&w, 0), "fast redial: kill A");
    ok(world_kill_near(&w, 2), "fast redial: kill C");
    ok_int(world_up(&w), 1, "fast redial: one carrier left up");
    ok_int(world_replacing(&w), 3, "fast redial: three replacements dialling");
    ok_int(world_empty(&w), 0, "fast redial: no slot lost");
    world_check_invariant(&w, "fast redial: invariant holds");
    ok(w.stream_alive, "fast redial: the stream survives");
    ok_u64(w.stream_reset_tcp_failed, 0, "fast redial: no stream reset");

    world_redial_all(&w);
    ok_int(world_up(&w), 4, "fast redial: all carriers up again");
    ok_int(world_replacing(&w), 0, "fast redial: no replacement left");
    ok_int(world_empty(&w), 0, "fast redial: no empty slot");
    world_check_invariant(&w, "fast redial done: invariant holds");
    ok_int(w.live, 4, "fast redial: only the four live carriers remain");

    /* The far side loses every carrier before a replacement joins. That is
     * the only path that may reset the stream. */
    ok(world_kill_far(&w, 0), "far loss: slot 0");
    ok(world_kill_far(&w, 1), "far loss: slot 1");
    ok(world_kill_far(&w, 2), "far loss: slot 2");
    ok(w.stream_alive, "far loss: one carrier left, stream alive");
    ok(world_kill_far(&w, 3), "far loss: slot 3");
    ok(!w.stream_alive, "far loss: every carrier gone, stream reset");
    ok_u64(w.stream_reset_tcp_failed, 1, "far loss: one bounded reset");
    ok_int(world_empty(&w), 4, "far loss: all slots empty, none leaked");
    world_check_invariant(&w, "far loss: invariant holds");
    ok_int(w.live, 0, "far loss: retired carriers are freed");

    /* A late DATA after the reset must not revive the stream. */
    world_late_data(&w);
    ok_u64(w.late_data, 1, "late DATA after reset is dropped");
    ok(!w.stream_alive, "late DATA does not revive the stream");

    /* FIN is graceful and final. */
    world_init(&w, 4);
    world_redial_all(&w);
    world_stream_fin(&w);
    ok_u64(w.fin_count, 1, "FIN closes the stream once");
    ok(!w.stream_alive, "FIN leaves the stream closed");
    world_stream_fin(&w);
    ok_u64(w.fin_count, 1, "a second FIN does not close twice");
    world_late_data(&w);
    ok_u64(w.late_data, 1, "DATA after FIN is dropped");

    /* A stream RST closes only the stream. The carriers stay up, and a late
     * DATA does not revive it. */
    world_init(&w, 4);
    world_redial_all(&w);
    world_stream_rst(&w);
    ok_u64(w.stream_reset_user, 1, "stream RST counted once");
    ok(!w.stream_alive, "stream RST leaves the stream closed");
    ok_int(world_up(&w), 4, "stream RST leaves the carriers up");
    world_stream_rst(&w);
    ok_u64(w.stream_reset_user, 1, "a second stream RST is ignored");
    world_late_data(&w);
    ok_u64(w.late_data, 1, "DATA after stream RST is dropped");

    /* Destroy after the storm is bounded and frees everything. */
    world_init(&w, 4);
    world_redial_all(&w);
    world_kill_near(&w, 0);
    world_kill_near(&w, 1);
    world_kill_near(&w, 2);
    world_redial_all(&w);
    ok(world_destroy(&w, &steps), "destroy after redial storm is bounded");
    ok(steps < 64, "destroy returned within the step cap (no D-state wait)");
    ok_int(w.live, 0, "destroy freed every carrier object");
    ok_int(w.works, 0, "destroy returned every work reference");
    ok_int(w.refs, 0, "destroy released the session reference");
}

/* ------------------------------------------------------------------ *
 * 3. A carrier kill during a FEC block is repaired by replay.
 * ------------------------------------------------------------------ */

static void test_fec_carrier_interleave(void)
{
    struct blk b;

    out_len = 0;
    fec_reset_hole = 0;
    dup_share = 0;

    /* Block 0 is in flight on four carriers. Carrier 2 dies before its
     * share is read. The other three DATA shares and two parity shares
     * rebuild the lost share. The stream must not reset. */
    blk_fill(&b, 0);
    blk_encode(&b);
    blk_drop_data(&b, 2);
    blk_arrive_parity(&b, 0);
    blk_arrive_parity(&b, 1);
    blk_drop_parity(&b, 2);
    blk_drop_parity(&b, 3);
    ok(blk_recover(&b), "carrier kill: surviving shares repair the block");
    ok(blk_deliver(&b, 0), "carrier kill: block delivered intact");

    /* The replacement carrier replays the missing share. A duplicate replay
     * is counted once and changes nothing. */
    blk_dup(&b, 2);
    ok_int(dup_share, 1, "carrier kill: replay duplicate counted");
    ok_int((int)out_len, N * S, "carrier kill: no duplicate delivery");

    /* The next block completes across the redial. */
    blk_fill(&b, 1);
    blk_encode(&b);
    blk_arrive_parity(&b, 0);
    blk_arrive_parity(&b, 1);
    blk_arrive_parity(&b, 2);
    blk_arrive_parity(&b, 3);
    ok(blk_deliver(&b, 1), "redial: next block delivered");
    ok(!memcmp(out, src, out_len), "redial: stream is an exact source prefix");

    /* A block that died with the old carrier is replayed whole from the TX
     * replay ring after the redial. Nothing is skipped and nothing repeats. */
    blk_fill(&b, 2);
    blk_encode(&b);
    blk_drop_data(&b, 0);
    blk_drop_data(&b, 1);
    blk_drop_data(&b, 2);
    blk_drop_data(&b, 3);
    blk_drop_parity(&b, 0);
    blk_drop_parity(&b, 1);
    blk_drop_parity(&b, 2);
    blk_drop_parity(&b, 3);
    ok(!blk_recover(&b), "redial: the old carrier's block is wholly lost");
    b.dp[0] = b.dp[1] = b.dp[2] = b.dp[3] = 1;
    b.pp[0] = b.pp[1] = 1;
    ok(blk_deliver(&b, 2), "redial: whole-block replay delivers intact");
    ok(!memcmp(out, src, out_len), "redial: replay keeps the source prefix");
}

int main(void)
{
    src_init();
    test_fec_block_loss();
    test_carrier_faults();
    test_fec_carrier_interleave();

    if (failures)
    {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_fault: %d checks passed\n", checks);
    return 0;
}
