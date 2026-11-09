/* test_rx_budget.c - one carrier RX work item's read budget, as a model.
 *
 * WHAT THIS IS
 *   The executable specification of the fix for
 *
 *     workqueue: tbt_car_rx_work [tbtun] hogged CPU for >10000us N times
 *
 *   A carrier's receive queue can hold two rcvbufs (up to 2 MiB). Before the
 *   budget, one w_rx item handed the whole queue to tcp_read_sock() and parsed
 *   every FEC record in it (HTTP deframe, two CRCs, ChaCha8 XOR, block copy).
 *   The actor now stops between TBT_RX_RUN_SLICE-byte pieces once
 *   TBT_RX_RUN_NS of wall time is spent. The budget constants and the stop
 *   condition come from tbtun/tbt_rx_budget.h, the header the module compiles.
 *
 *   What this file models, and why:
 *     - fake_actor(): tcp_read_sock()'s actor contract. It consumes at most
 *       one skb per call; a short return advances copied_seq by `used`; a
 *       0-byte return is "nothing consumed" and stops the walk. The bytes it
 *       hands to the parser are counted exactly once.
 *     - fake_body(): tbt_car_rx_body()'s re-arm rule. While bytes remain, a
 *       run that consumed anything always re-arms (copied_seq != rcv_nxt);
 *       the last run does not. That is the property that stops a budgeted
 *       yield from losing the rest of the queue. The rule itself is
 *       tbt_rx_resume_plan() in tbt_rx_budget.h, which the module calls, so
 *       the test cannot drift from the body.
 *     - case_wake_coalescing(): the two-bit rx_again state machine
 *       (TBT_RX_AGAIN_BIT / TBT_RX_RUNNING_BIT in tbt_carrier.c). At most one
 *       parser and one queued item; a wake during a run is coalesced and
 *       consumed by the entry wrapper.
 *   The skb walk itself is not linked: tbt_carrier.c needs socks, workqueues
 *   and the ktime clock. This is the same model style as host/test_reorder.c.
 *
 * CASES
 *   the deadline boundary, a 1 MiB queue drained in several runs, a slow
 *   path whose single slice is the overshoot, an empty queue, the resume
 *   truth table, a held -EAGAIN record, a stopping carrier, and the
 *   coalescing state machine across budgeted runs.
 *
 *   make host-test
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_rx_budget.h"

static int checks, failures;

static void ok(bool cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", what);
}

/* ------------------------------------------------------------------ */
/* The actor and the body.                                            */

struct fake_rx
{
    size_t total;      /* bytes queued in the fake socket */
    size_t skb_bytes;  /* payload bytes per skb */
    size_t skb;        /* current skb index */
    size_t in_skb;     /* bytes consumed from the current skb */
    size_t copied;     /* tp->copied_seq */
    size_t parsed;     /* bytes the fake parser accepted */
    u64 now_ns;        /* fake wall clock */
    u64 ns_per_byte;   /* fake per-byte parse cost */
};

static size_t fake_skb_count(const struct fake_rx *r)
{
    if (r->total == 0)
        return 0;
    return (r->total + r->skb_bytes - 1) / r->skb_bytes;
}

static size_t fake_skb_len(const struct fake_rx *r, size_t i)
{
    size_t off = i * r->skb_bytes;
    size_t left = r->total > off ? r->total - off : 0;

    return left < r->skb_bytes ? left : r->skb_bytes;
}

/* One tcp_read_sock() actor call for the current skb. A spent budget on
 * entry returns 0 before touching the skb, exactly as the module does. */
static size_t fake_actor(struct fake_rx *r, struct tbt_rx_budget *b,
                         int *budget_stops)
{
    size_t used = 0, len;

    if (r->skb >= fake_skb_count(r))
        return 0;
    if (tbt_rx_budget_stopped(b, r->now_ns))
    {
        (*budget_stops)++;
        return 0;
    }
    len = fake_skb_len(r, r->skb);
    while (r->in_skb < len)
    {
        size_t left = len - r->in_skb;
        size_t take = left < TBT_RX_RUN_SLICE ? left : TBT_RX_RUN_SLICE;

        r->in_skb += take;
        r->parsed += take;
        r->now_ns += (u64)take * r->ns_per_byte;
        used += take;
        tbt_rx_budget_charge(b, take);
        if (tbt_rx_budget_stopped(b, r->now_ns))
            break;
    }
    if (r->in_skb == len)
    {
        /* The whole skb is consumed: tcp_read_sock() eats it. A short return
         * leaves it in place and the next actor call continues at the offset
         * copied_seq now names. */
        r->skb++;
        r->in_skb = 0;
    }
    return used;
}

struct run_result
{
    size_t drained;
    u64 ns;
    bool rearm; /* tbt_car_rx_body()'s copied_seq != rcv_nxt decision */
};

static struct run_result fake_body(struct fake_rx *r, int *budget_stops)
{
    struct tbt_rx_budget b;
    struct run_result res = { 0, 0, false };
    u64 t0 = r->now_ns;

    tbt_rx_budget_start(&b, r->now_ns);
    for (;;)
    {
        size_t used = fake_actor(r, &b, budget_stops);

        if (!used)
            break;
        res.drained += used;
    }
    r->copied += res.drained;
    res.ns = r->now_ns - t0;
    /* sk_state is ESTABLISHED in every case here; the body's other branch
     * (drained <= 0 => carrier_lost) is not reachable because a run never
     * stops on entry before its first piece. */
    ok(r->copied == r->parsed, "copied_seq equals parser-accepted bytes");
    /* The module's own rule, not a copy: only unread socket bytes arm. */
    res.rearm = tbt_rx_resume_plan(false, true, r->copied != r->total, false) ==
                TBT_RX_RESUME_ARM;
    return res;
}

/* Drive budgeted work items until the queue is empty. */
static int fake_drive(struct fake_rx *r, int *budget_stops, u64 *max_run_ns)
{
    int runs = 0;

    for (;;)
    {
        struct run_result res = fake_body(r, budget_stops);

        runs++;
        if (res.ns > *max_run_ns)
            *max_run_ns = res.ns;
        ok(res.drained > 0, "a run that has data always makes progress");
        if (r->copied == r->total)
        {
            ok(!res.rearm, "the last run does not re-arm");
            break;
        }
        ok(res.rearm, "a run with bytes left re-arms");
        if (runs > 100000)
        {
            ok(false, "the model terminates");
            break;
        }
    }
    return runs;
}

/* ------------------------------------------------------------------ */
/* The two-bit rx_again state machine (private to tbt_carrier.c).      */

#define MODEL_RX_AGAIN_BIT   0
#define MODEL_RX_RUNNING_BIT 1
#define MODEL_AGAIN   (1 << MODEL_RX_AGAIN_BIT)
#define MODEL_RUNNING (1 << MODEL_RX_RUNNING_BIT)

struct fake_carrier
{
    int rx_again;
    int queued; /* schedule_work() calls that put an item on the queue */
};

static void fake_schedule_rx(struct fake_carrier *c)
{
    if (__atomic_fetch_or(&c->rx_again, MODEL_RUNNING, __ATOMIC_SEQ_CST) &
        MODEL_RUNNING)
    {
        __atomic_fetch_or(&c->rx_again, MODEL_AGAIN, __ATOMIC_SEQ_CST);
        return;
    }
    c->queued++;
}

/* tbt_car_rx_work()'s entry wrapper. A stopping carrier consumes the AGAIN
 * bit without queueing another item (tbt_carrier.c's wrapper guard). */
static struct run_result fake_wrapper(struct fake_carrier *c,
                                      struct fake_rx *r,
                                      int *budget_stops, bool stopping)
{
    struct run_result res = fake_body(r, budget_stops);

    __atomic_fetch_and(&c->rx_again, ~MODEL_RUNNING, __ATOMIC_SEQ_CST);
    if ((__atomic_fetch_and(&c->rx_again, ~MODEL_AGAIN, __ATOMIC_SEQ_CST) &
         MODEL_AGAIN) && !stopping)
        fake_schedule_rx(c);
    return res;
}

/* ------------------------------------------------------------------ */
/* Cases.                                                             */

static void case_deadline_boundary(void)
{
    struct tbt_rx_budget b;

    tbt_rx_budget_start(&b, 12345);
    ok(!tbt_rx_budget_stopped(&b, 12345), "a fresh budget has not stopped");
    ok(!tbt_rx_budget_stopped(&b, 12345 + TBT_RX_RUN_NS - 1),
       "a fresh budget is live one nanosecond before the deadline");
    tbt_rx_budget_charge(&b, 1);
    ok(!tbt_rx_budget_stopped(&b, 12345 + TBT_RX_RUN_NS - 1),
       "one byte does not spend the budget early");
    ok(tbt_rx_budget_stopped(&b, 12345 + TBT_RX_RUN_NS),
       "the deadline stops the run");
    tbt_rx_budget_start(&b, 0);
    ok(!tbt_rx_budget_stopped(&b, TBT_RX_RUN_NS),
       "an uncharged run never stops, even past the deadline");
    tbt_rx_budget_charge(&b, 1);
    ok(tbt_rx_budget_stopped(&b, TBT_RX_RUN_NS),
       "the first piece arms the stop");
}

static void case_batched_complete(void)
{
    struct fake_rx r = {
        .total = 1024 * 1024,
        .skb_bytes = 64 * 1024,
        .ns_per_byte = 8, /* 125 MB/s: a plausible far-side parse rate */
    };
    int budget_stops = 0, runs;
    u64 max_run_ns = 0;
    u64 slice_ns = (u64)TBT_RX_RUN_SLICE * r.ns_per_byte;

    runs = fake_drive(&r, &budget_stops, &max_run_ns);
    ok(r.copied == r.total, "every queued byte reached copied_seq once");
    ok(r.parsed == r.total, "every queued byte was parsed once");
    ok(r.skb == fake_skb_count(&r), "every skb was consumed");
    ok(runs >= 4, "a 1 MiB queue takes several budgeted runs");
    ok(budget_stops == runs - 1,
       "every run but the last ends on the budget");
    ok(max_run_ns <= TBT_RX_RUN_NS + slice_ns,
       "a run stays inside its deadline plus one slice");
    ok(max_run_ns < TBT_RX_WQ_HOG_NS,
       "no run reaches the 10 ms workqueue hog threshold");
}

static void case_resume_plan(void)
{
    /* Unread socket bytes always arm: the budgeted-yield path and the
     * coalesced-wake path are the same decision. */
    ok(tbt_rx_resume_plan(false, true, true, false) == TBT_RX_RESUME_ARM,
       "unread socket bytes arm the next run");
    ok(tbt_rx_resume_plan(false, true, true, true) == TBT_RX_RESUME_ARM,
       "unread socket bytes outrank a held record");
    /* A held record with an empty socket must wait for the poll: arming
     * would spin the worker without copying a byte. */
    ok(tbt_rx_resume_plan(false, true, false, true) == TBT_RX_RESUME_WAIT,
       "a held record alone waits for the poll");
    ok(tbt_rx_resume_plan(false, true, false, false) == TBT_RX_RESUME_IDLE,
       "an empty run is idle");
    /* No wake is owed once the carrier is stopping or the socket is gone. */
    ok(tbt_rx_resume_plan(true, true, true, true) == TBT_RX_RESUME_IDLE,
       "a stopping carrier owns no wake");
    ok(tbt_rx_resume_plan(false, false, true, true) == TBT_RX_RESUME_IDLE,
       "a missing socket owns no wake");
}

/* A record held by -EAGAIN after its bytes left the socket. This is the trap
 * the WAIT rule protects: copied_seq == rcv_nxt, parser->have != 0, and the
 * only retry is the 50 ms poll (tbt_rx_parse_pending()). */
struct pending_rx
{
    size_t socket_copied; /* copied_seq */
    size_t wire;          /* rcv_nxt */
    size_t parser_have;
    int deliveries;
};

static void case_pending_record(void)
{
    struct pending_rx p = {
        .socket_copied = 64 * 1024,
        .wire = 64 * 1024,
        .parser_have = 1024, /* the record that returned -EAGAIN */
    };

    ok(tbt_rx_resume_plan(false, true,
                          p.socket_copied != p.wire,
                          p.parser_have != 0) == TBT_RX_RESUME_WAIT,
       "a rejected record does not self-arm");
    /* The poll wakes w_rx; its pre-read retry parses the held record. */
    if (p.parser_have)
    {
        p.parser_have = 0;
        p.deliveries++;
    }
    ok(p.deliveries == 1, "the held record is parsed exactly once");
    ok(p.socket_copied == p.wire, "the retry copies no socket byte");
    ok(tbt_rx_resume_plan(false, true,
                          p.socket_copied != p.wire,
                          p.parser_have != 0) == TBT_RX_RESUME_IDLE,
       "the retried record leaves no wake owed");
}

static void case_stopping_no_rearm(void)
{
    struct fake_carrier c = {};
    struct fake_rx r = { .total = 0, .skb_bytes = 64 * 1024 };
    int budget_stops = 0;

    fake_schedule_rx(&c);
    ok(c.queued == 1, "one item is queued");
    c.queued--;
    /* A wake lands while the item runs, then the carrier starts stopping. */
    __atomic_fetch_or(&c.rx_again, MODEL_AGAIN, __ATOMIC_SEQ_CST);
    (void)fake_wrapper(&c, &r, &budget_stops, true);
    ok(c.queued == 0, "a stopping carrier consumes the wake without a requeue");
    ok((c.rx_again & MODEL_AGAIN) == 0, "the AGAIN bit is consumed");
}

static void case_slow_slice_overshoot(void)
{
    struct fake_rx r = {
        .total = 64 * 1024,
        .skb_bytes = 64 * 1024,
        .ns_per_byte = 100, /* 10 MB/s: the slow end of a usable path */
    };
    int budget_stops = 0, runs;
    u64 max_run_ns = 0;
    u64 slice_ns = (u64)TBT_RX_RUN_SLICE * r.ns_per_byte;

    runs = fake_drive(&r, &budget_stops, &max_run_ns);
    ok(r.copied == r.total, "the slow path consumes every byte once");
    ok(runs >= 3, "a slow 64 KiB skb is split across runs");
    ok(runs > 1, "the first run did not take the whole skb");
    ok(max_run_ns <= TBT_RX_RUN_NS + slice_ns,
       "the overshoot is bounded by exactly one slice");
    ok(max_run_ns < TBT_RX_WQ_HOG_NS,
       "even at 10 MB/s a run stays below the hog threshold");
}

static void case_empty_queue(void)
{
    struct fake_rx r = { .total = 0, .skb_bytes = 64 * 1024 };
    int budget_stops = 0;
    struct run_result res;

    res = fake_body(&r, &budget_stops);
    ok(res.drained == 0, "an empty socket drains nothing");
    ok(!res.rearm, "an empty socket is not re-armed");
    ok(budget_stops == 0, "an empty socket does not spend a budget");
    ok(r.copied == 0 && r.parsed == 0, "an empty socket stays empty");
}

static void case_wake_coalescing(void)
{
    struct fake_rx r = {
        .total = 1024 * 1024,
        .skb_bytes = 64 * 1024,
        .ns_per_byte = 8,
    };
    struct fake_carrier c = {};
    int budget_stops = 0, runs = 0;

    fake_schedule_rx(&c);
    ok(c.queued == 1, "the first wake queues one item");
    while (c.queued > 0 && runs < 100000)
    {
        struct run_result res;
        bool had_data = r.copied != r.total;

        c.queued--;
        /* A carrier wake arriving during the run (sk_data_ready) only sets
         * the AGAIN bit: the running item owns the parser. */
        if (had_data)
            fake_schedule_rx(&c);
        res = fake_wrapper(&c, &r, &budget_stops, false);
        runs++;
        ok(c.queued <= 1, "at most one RX item is queued at a time");
        if (!had_data)
            ok(res.drained == 0, "the coalesced extra item finds no data");
        if (r.copied == r.total)
        {
            ok(!res.rearm, "the final item does not re-arm");
            continue;
        }
        ok(res.rearm, "a budgeted stop re-arms the next item");
        ok(c.queued == 1, "the coalesced wake queued exactly one item");
    }
    ok(r.copied == r.total, "the coalesced walk loses no byte");
    ok(c.queued == 0, "the queue is empty after the drain");
    ok(runs >= 4, "the transfer needed several budgeted items");
}

static void case_margin(void)
{
    /* The far kernel's line is 10 ms. One slice at 10 MB/s (the slowest
     * usable path) must still leave the whole run under it. */
    u64 slow_slice_ns = (u64)TBT_RX_RUN_SLICE * 100;

    ok(TBT_RX_RUN_NS + slow_slice_ns < TBT_RX_WQ_HOG_NS,
       "budget plus one slow slice stays below the hog threshold");
    ok(TBT_RX_RUN_NS < TBT_RX_WQ_HOG_NS / 2,
       "the budget leaves at least half the threshold for the body");
}

int main(void)
{
    case_deadline_boundary();
    case_batched_complete();
    case_resume_plan();
    case_pending_record();
    case_stopping_no_rearm();
    case_slow_slice_overshoot();
    case_empty_queue();
    case_wake_coalescing();
    case_margin();

    printf("test_rx_budget: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
