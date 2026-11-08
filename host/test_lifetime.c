/* test_lifetime.c - the arm/cancel ownership protocol of the session and
 * stream work items.
 *
 * tbt_session_arm()/tbt_session_cancel() (tbt_session.c) and
 * tbt_stream_schedule()/tbt_stream_cancel() (tbt_sock.c) share one rule: the
 * sched bit owns exactly one reference, the arm takes that reference before
 * it publishes the bit, and an arm whose queue attempt was dropped returns
 * the reference itself. A missed put leaves tbt_session_destroy() waiting on
 * &drained forever (0.2.1: `del 1` blocked in D state and pinned rmmod behind
 * it).
 *
 * The kernel workqueue cannot run here, so this file models it: a work item
 * is idle, pending or disabled, and cancel is split into begin/end so the
 * tests can interleave an arm with the cancel window. The model follows the
 * kernel code line by line; change both together.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define NW 4

struct life {
    int refs;
    unsigned long sched;
    bool closing;
    bool pending[NW];
    bool disabled[NW];
    int completed; /* &drained fired */
};

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

static void life_init(struct life *s, int refs)
{
    memset(s, 0, sizeof(*s));
    s->refs = refs;
}

/* tbt_session_put(): the last reference only wakes the waiter. */
static void put(struct life *s)
{
    if (--s->refs == 0)
        s->completed++;
}

/* mod_delayed_work()/queue_work(): a queue attempted while the work is
 * disabled is dropped and leaves nothing pending. */
static void queue(struct life *s, int w)
{
    if (!s->disabled[w])
        s->pending[w] = true;
}

/* The work entry: the worker clears PENDING before calling the function; the
 * function clears the sched bit and returns the reference if it owned it. */
static void entry(struct life *s, int w)
{
    bool owned = s->sched & (1ul << w);

    s->pending[w] = false;
    if (owned)
    {
        s->sched &= ~(1ul << w);
        put(s);
    }
}

/* The first half of tbt_session_arm(): take the reference, claim the bit,
 * and refuse a closing session. Returns true when the bit is ours. */
static bool arm_start(struct life *s, int w)
{
    if (s->refs == 0)
        return false;
    s->refs++;
    if (s->sched & (1ul << w))
    {
        /* Another arm won the bit while this one took its reference. */
        put(s);
        return false;
    }
    s->sched |= 1ul << w;
    if (s->closing)
    {
        if (s->sched & (1ul << w))
        {
            s->sched &= ~(1ul << w);
            put(s);
        }
        return false;
    }
    return true;
}

/* The second half: queue and return the reference if no instance took it. */
static bool arm_post(struct life *s, int w)
{
    if (!s->pending[w] && (s->sched & (1ul << w)))
    {
        s->sched &= ~(1ul << w);
        put(s);
        return false;
    }
    return true;
}

static bool arm_finish(struct life *s, int w)
{
    queue(s, w);
    return arm_post(s, w);
}

static bool arm(struct life *s, int w)
{
    if (s->sched & (1ul << w))
        return true;
    if (!arm_start(s, w))
        return false;
    return arm_finish(s, w);
}

/* cancel_delayed_work_sync(): grab a pending instance and disable the work
 * while the cancel runs. Returns true when it grabbed an instance. */
static bool cancel_begin(struct life *s, int w)
{
    bool canceled = s->pending[w];

    s->disabled[w] = true;
    s->pending[w] = false;
    return canceled;
}

/* The tail of cancel: re-enable, then return the reference the grabbed
 * instance owned. A reference taken by an arm that raced the cancel is
 * returned by that arm, never guessed at here. */
static void cancel_end(struct life *s, int w, bool canceled)
{
    s->disabled[w] = false;
    if (canceled && (s->sched & (1ul << w)))
    {
        s->sched &= ~(1ul << w);
        put(s);
    }
}

static void cancel(struct life *s, int w)
{
    cancel_end(s, w, cancel_begin(s, w));
}

static void test_arm_and_entry(void)
{
    struct life s;

    life_init(&s, 1);
    ok(arm(&s, 0), "arm: queued");
    ok_int(s.refs, 2, "arm: one reference taken");
    ok(s.pending[0], "arm: instance pending");
    entry(&s, 0);
    ok_int(s.refs, 1, "entry: reference returned");
    ok_int(s.completed, 0, "entry: the owner's reference is still held");
    put(&s);
    ok_int(s.completed, 1, "entry: completion fired at zero");
    ok(!(s.sched & 1ul), "entry: bit cleared");
}

static void test_arm_twice_one_reference(void)
{
    struct life s;

    life_init(&s, 1);
    arm(&s, 1);
    arm(&s, 1);
    ok_int(s.refs, 2, "second arm: coalesced, no second reference");
    entry(&s, 1);
    ok_int(s.refs, 1, "second arm: one entry returns the one reference");
}

static void test_cancel_pending(void)
{
    struct life s;

    life_init(&s, 1);
    arm(&s, 2);
    cancel(&s, 2);
    ok_int(s.refs, 1, "cancel pending: reference returned");
    ok(!(s.sched & (1ul << 2)), "cancel pending: bit cleared");
    ok(!s.pending[2], "cancel pending: nothing queued");
}

/* The 0.2.1 hang: cancel disables the work, an arm races it, the queue is
 * dropped. The arm must return its own reference. */
static void test_arm_during_cancel(void)
{
    struct life s;
    bool canceled;

    life_init(&s, 1);
    canceled = cancel_begin(&s, 3);
    ok(!canceled, "arm during cancel: nothing was pending");
    ok(!arm(&s, 3), "arm during cancel: refused");
    cancel_end(&s, 3, canceled);
    ok_int(s.refs, 1, "arm during cancel: reference not leaked");
    ok(!(s.sched & (1ul << 3)), "arm during cancel: bit not left set");
}

/* The mirror race: the arm takes its reference and claims the bit, then a
 * cancel runs before the queue attempt. The canceller must not return a
 * reference it does not own, and the arm must return its own. */
static void test_cancel_between_bit_and_queue(void)
{
    struct life s;
    bool canceled;

    life_init(&s, 1);
    ok(arm_start(&s, 0), "bit before queue: arm owns the bit");
    canceled = cancel_begin(&s, 0);
    ok(!canceled, "bit before queue: cancel finds nothing pending");
    ok(!arm_finish(&s, 0), "bit before queue: dropped queue refused");
    cancel_end(&s, 0, canceled);
    ok_int(s.refs, 1, "bit before queue: reference still owned by the arm");
    ok_int(s.refs, 1, "bit before queue: arm returned its reference");
    ok(!(s.sched & 1ul), "bit before queue: bit cleared");
}

/* A closing session refuses the arm without leaking the reference. */
static void test_closing(void)
{
    struct life s;

    life_init(&s, 1);
    s.closing = true;
    ok(!arm(&s, 1), "closing: arm refused");
    ok_int(s.refs, 1, "closing: no reference taken");
    ok(!(s.sched & (1ul << 1)), "closing: bit not left set");
}

/* The entry may clear the bit between the queue attempt and the arm's
 * post-queue check. Exactly one of the two returns the reference. */
static void test_entry_races_post_check(void)
{
    struct life s;

    life_init(&s, 1);
    ok(arm_start(&s, 2), "post check: arm owns the bit");
    queue(&s, 2);
    entry(&s, 2); /* the worker runs and returns the reference */
    ok(arm_post(&s, 2), "post check: the entry owns the reference");
    ok_int(s.refs, 1, "post check: no double put");
    put(&s);
    ok_int(s.completed, 1, "post check: completion fired once");
}

/* A destroy that collects every reference returns from the bounded wait. */
static void test_destroy_clean(void)
{
    struct life s;
    int w;

    life_init(&s, 1); /* the tunnel's reference */
    for (w = 0; w < 3; w++)
        arm(&s, w);
    s.refs++; /* one published stream */
    for (w = 0; w < 3; w++)
        cancel(&s, w);
    put(&s); /* the stream's last reference */
    ok_int(s.refs, 1, "destroy clean: only the tunnel reference is left");
    put(&s); /* tbt_session_put() before the wait */
    ok_int(s.completed, 1, "destroy clean: the drain completes");
}

/* A reference nobody returns must not block the destroy forever: the bounded
 * wait reports the leak and leaves the session alone. */
static void test_destroy_leak_is_bounded(void)
{
    struct life s;

    life_init(&s, 2); /* the tunnel's reference plus one leaked reference */
    put(&s);          /* tbt_session_put() before the wait */
    ok_int(s.completed, 0, "destroy leak: the drain does not complete");
    ok_int(s.refs, 1, "destroy leak: the leak is visible");
    /* The kernel waits with wait_for_completion_timeout() and returns here.
     * The leaked session is not freed; a late put only fires the unused
     * completion. */
    put(&s);
    ok_int(s.completed, 1, "destroy leak: a late put still completes");
}

int main(void)
{
    test_arm_and_entry();
    test_arm_twice_one_reference();
    test_cancel_pending();
    test_arm_during_cancel();
    test_cancel_between_bit_and_queue();
    test_closing();
    test_entry_races_post_check();
    test_destroy_clean();
    test_destroy_leak_is_bounded();

    if (failures)
    {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_lifetime: %d checks passed\n", checks);
    return 0;
}
