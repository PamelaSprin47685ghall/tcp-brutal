/* test_tx_publish.c - host test for the pure DATA publish/drop accounting.
 *
 * WHAT THIS IS
 *   The exact rules tbt_tx.c runs, compiled from src/kernel/tbt_tx_acct.h:
 *
 *     - one publish adds the primary plus every duplicate to tx_queued,
 *       adds only the duplicate bytes to dup_bytes, adds one chunk to
 *       retain_bytes, and sets dup_pending once to ndup;
 *     - the capacity check rejects the whole set without changing any
 *       counter or consuming seq;
 *     - tbt_tx_acct_drop() removes one duplicate's accounting, with a
 *       non-zero guard on both dup_bytes and dup_pending;
 *     - a non-dup frame (primary DATA or control) owns no duplicate
 *       accounting;
 *     - the three removal paths (pump drop, purge, free_queue) all end with
 *       tx_queued, dup_bytes and dup_pending balanced.
 *
 *   It does not link tbt_tx.c: the header is the shared implementation, so a
 *   change to the rules breaks this test in lockstep with the module.
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

/* A minimal TX queue: the accounting snapshot plus the retain entry's
 * dup_pending. Frame identity is not needed; every duplicate of one publish
 * carries the same len. */
struct fake_q
{
    struct tbt_tx_acct a;
    u8 dup_pending;
};

static bool publish(struct fake_q *q, u16 chunk, unsigned int ndup,
                    u32 txq_max)
{
    if (!tbt_tx_acct_fits(q->a.tx_queued, txq_max, chunk, ndup))
        return false;
    tbt_tx_acct_publish(&q->a, chunk, ndup, &q->dup_pending);
    return true;
}

/* The caller owns tx_queued; the shared helper owns dup_bytes/dup_pending. */
static void drop_dup(struct fake_q *q, u16 len)
{
    q->a.tx_queued -= len;
    q->a.dup_bytes -= tbt_tx_acct_drop(q->a.dup_bytes, len, &q->dup_pending);
}

static void case_publish_counts(void)
{
    unsigned int n;

    for (n = 0; n <= TBT_DUP_MAX; n++)
    {
        struct fake_q q = {0};

        ok(publish(&q, 1000, n, 1u << 20), "publish: fits");
        ok(q.a.tx_queued == 1000u * (1u + n), "publish: tx_queued");
        ok(q.a.dup_bytes == 1000u * n, "publish: dup_bytes");
        ok(q.a.retain_bytes == 1000, "publish: retain_bytes");
        ok(q.dup_pending == n, "publish: dup_pending set once");
    }
}

static void case_capacity(void)
{
    struct fake_q q = {0};
    u32 room = 10000;

    ok(!tbt_tx_acct_fits(room, 10000 + 3999, 1000, 3),
       "capacity: one byte short rejects");
    ok(tbt_tx_acct_fits(room, 10000 + 4000, 1000, 3),
       "capacity: exact fit accepts");

    /* A rejected publish must not move any counter or dup_pending. */
    ok(!publish(&q, 1000, 3, 3999), "capacity: publish rejected");
    ok(q.a.tx_queued == 0 && q.a.dup_bytes == 0 && q.a.retain_bytes == 0 &&
           q.dup_pending == 0,
       "capacity: rejection leaves counters unchanged");
}

static void case_drop_dup(void)
{
    struct fake_q q = {0};
    u64 sub;

    publish(&q, 1000, 2, 1u << 20);
    sub = tbt_tx_acct_drop(q.a.dup_bytes, 1000, &q.dup_pending);
    q.a.tx_queued -= 1000;
    q.a.dup_bytes -= sub;

    ok(sub == 1000, "drop: duplicate bytes subtracted");
    ok(q.a.dup_bytes == 1000, "drop: dup_bytes balance");
    ok(q.dup_pending == 1, "drop: dup_pending balance");
    ok(q.a.tx_queued == 2000, "drop: tx_queued balance");
}

static void case_non_dup(void)
{
    ok(tbt_tx_acct_is_dup(TBT_F_DATA, true), "kind: DATA duplicate");
    ok(!tbt_tx_acct_is_dup(TBT_F_DATA, false), "kind: primary DATA");
    ok(!tbt_tx_acct_is_dup(TBT_F_FIN, false), "kind: control frame");
    ok(!tbt_tx_acct_is_dup(TBT_F_FBK, false), "kind: FBK");
}

static void case_guards(void)
{
    u8 pending = 0;
    u64 sub;

    sub = tbt_tx_acct_drop(0, 1000, &pending);
    ok(sub == 0 && pending == 0, "guard: empty dup_bytes cannot underflow");

    pending = 2;
    sub = tbt_tx_acct_drop(500, 1000, &pending);
    ok(sub == 0 && pending == 1,
       "guard: short dup_bytes still releases one dup_pending");

    sub = tbt_tx_acct_drop(1000, 1000, NULL);
    ok(sub == 1000, "guard: freed retain entry still subtracts dup_bytes");

    pending = 0;
    sub = tbt_tx_acct_drop(1000, 1000, &pending);
    ok(sub == 1000 && pending == 0,
       "guard: zero dup_pending is not decremented past zero");
}

static void case_pump_drop(void)
{
    struct fake_q q = {0};

    publish(&q, 800, 2, 1u << 20);
    /* The pump drops one duplicate before it is written. */
    drop_dup(&q, 800);
    ok(q.a.tx_queued == 1600 && q.a.dup_bytes == 800 && q.dup_pending == 1,
       "pump drop: balanced");
}

static void case_purge(void)
{
    struct fake_q q = {0};

    publish(&q, 600, 3, 1u << 20);
    /* Purge removes the primary and every duplicate of a dead sid. The
     * primary subtracts only tx_queued; each duplicate subtracts its
     * accounting. */
    q.a.tx_queued -= 600;
    drop_dup(&q, 600);
    drop_dup(&q, 600);
    drop_dup(&q, 600);
    ok(q.a.tx_queued == 0 && q.a.dup_bytes == 0 && q.dup_pending == 0,
       "purge: balanced");
    ok(q.a.retain_bytes == 600,
       "purge: retain entry is freed by trim/gc, not by the queue purge");
}

static void case_free_queue(void)
{
    struct fake_q q = {0};

    publish(&q, 700, 3, 1u << 20);
    /* Session teardown walks the queue: primary first, then duplicates. */
    q.a.tx_queued -= 700;
    drop_dup(&q, 700);
    drop_dup(&q, 700);
    drop_dup(&q, 700);
    ok(q.a.tx_queued == 0 && q.a.dup_bytes == 0 && q.dup_pending == 0,
       "free_queue: balanced");

    /* A repeated drop on an already-drained entry must not underflow the
     * duplicate counters (tx_queued is not part of this helper). */
    ok(tbt_tx_acct_drop(q.a.dup_bytes, 700, &q.dup_pending) == 0,
       "free_queue: drained dup_bytes cannot underflow");
    ok(q.a.dup_bytes == 0 && q.dup_pending == 0,
       "free_queue: drained counters stay zero");
}

int main(void)
{
    case_publish_counts();
    case_capacity();
    case_drop_dup();
    case_non_dup();
    case_guards();
    case_pump_drop();
    case_purge();
    case_free_queue();

    printf("test_tx_publish: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
