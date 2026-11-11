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

/* --- FEC flow free work and module unload ----------------------------- */

/* tbt_fec_flow_put(): the table, the stream and every in-flight packetizer
 * pass hold one reference.  The zero transition queues free_work.  The module
 * queues it on tbt_fec_free_wq, and tbtun_exit() destroys that queue after
 * tbt_tunnel_shutdown(), before the module text is unmapped.  Neither
 * synchronize_rcu() nor rcu_barrier() waits for a work item. */
struct fec_module {
    bool wq_live;
    bool text_live;
};

struct fec_flow {
    int refs;
    bool queued;  /* free_work is on the queue */
    bool running; /* the worker entered tbt_fec_flow_free_work() */
    bool ran;     /* the free completed */
    bool fault;   /* queue_work() after the queue was destroyed */
};

static void flow_put(struct fec_module *m, struct fec_flow *f)
{
    if (--f->refs)
        return;
    if (!m->wq_live)
    {
        /* WARN_ON_ONCE(!tbt_fec_free_wq) in the kernel: leak the flow, do
         * not call into a queue that no longer exists. */
        f->fault = true;
        return;
    }
    f->queued = true;
}

static void flow_work_start(struct fec_module *m, struct fec_flow *f)
{
    ok(m->text_live, "flow free: module text is mapped while it runs");
    f->queued = false;
    f->running = true;
}

static void flow_work_finish(struct fec_flow *f)
{
    f->running = false;
    f->ran = true;
}

/* destroy_workqueue(): run everything still queued, then wait for every
 * running instance.  Only after this may the module text go away. */
static void wq_destroy(struct fec_module *m, struct fec_flow **flows, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        if (flows[i]->queued)
            flow_work_start(m, flows[i]);
        if (flows[i]->running)
            flow_work_finish(flows[i]);
    }
    m->wq_live = false;
}

/* tbt_session_destroy(): tbt_fec_data_free() drops the table reference,
 * then tbt_fec_flow_detach() drops the stream reference.  The last put
 * queues the free; the module-exit drain completes it while the text lives. */
static void test_flow_free_is_drained_before_unload(void)
{
    struct fec_module m = { .wq_live = true, .text_live = true };
    struct fec_flow f = { .refs = 2 };
    struct fec_flow *flows[] = { &f };

    flow_put(&m, &f); /* the table reference */
    ok_int(f.refs, 1, "flow free: the stream reference keeps it alive");
    ok(!f.queued, "flow free: no queue while a reference remains");
    flow_put(&m, &f); /* the stream reference */
    ok_int(f.refs, 0, "flow free: the last reference drops to zero");
    ok(f.queued, "flow free: the last put queues free_work");
    wq_destroy(&m, flows, 1);
    ok(f.ran, "flow free: destroy_workqueue ran the queued free");
    ok(!f.queued && !f.running, "flow free: nothing is left queued");
    ok(!f.fault, "flow free: the queue was alive when the put ran");
    m.text_live = false; /* tbtun_exit() returns after the drain */
}

/* The pre-0.4.1 exit path only called synchronize_rcu() and rcu_barrier().
 * Both are no-ops here because the instance is a work item, not an RCU
 * callback: the free stays queued when the module text goes away. */
static void test_rcu_barriers_do_not_drain_flow_free(void)
{
    struct fec_module m = { .wq_live = true, .text_live = true };
    struct fec_flow f = { .refs = 1 };

    flow_put(&m, &f);
    ok(f.queued, "rcu exit: free_work is still queued after the barriers");
    ok(!f.ran, "rcu exit: the free has not run");
    m.text_live = false;
    ok(f.queued && !m.text_live,
       "rcu exit: a system_wq free would outlive the module text");
}

/* A free that already started (inside its own synchronize_rcu()) is as
 * dangerous as a queued one. destroy_workqueue() must wait for it too. */
static void test_wq_destroy_waits_for_running_free(void)
{
    struct fec_module m = { .wq_live = true, .text_live = true };
    struct fec_flow a = { .refs = 1 }, b = { .refs = 1 }, c = { .refs = 1 };
    struct fec_flow *flows[] = { &a, &b, &c };

    flow_put(&m, &a);
    flow_put(&m, &b);
    flow_work_start(&m, &a); /* the worker is inside synchronize_rcu() */
    flow_put(&m, &c);
    ok(a.running && !a.ran, "wq drain: one free is in flight");
    ok(b.queued && c.queued, "wq drain: two frees are queued");
    wq_destroy(&m, flows, 3);
    ok(a.ran && b.ran && c.ran, "wq drain: queued and running frees complete");
    ok(!m.wq_live, "wq drain: the queue is gone before the text");
}

/* No producer may put a flow after the drain.  The kernel WARNs and leaks
 * rather than queueing into a destroyed workqueue. */
static void test_late_put_is_reported_not_queued(void)
{
    struct fec_module m = { .wq_live = false, .text_live = false };
    struct fec_flow f = { .refs = 1 };

    flow_put(&m, &f);
    ok(f.fault, "late put: queueing into a destroyed WQ is reported");
    ok(!f.ran, "late put: no module text is called");
}

/* --- /proc stats render against session destroy ----------------------- */

/* tbt_show_stats() holds a session reference, but destroy frees car[],
 * fec_outq and the stream hash before it waits for that reference. The
 * per-session stats_lock closes the window: the render takes the read side,
 * destroy takes the write side before the first child goes and releases it
 * before the drained wait. */
struct stats_gate {
    int readers;
    bool writer;
    bool children_valid;
};

static bool stats_enter(struct stats_gate *g)
{
    if (g->writer)
        return false; /* down_read() blocks behind the writer */
    g->readers++;
    return true;
}

static void stats_leave(struct stats_gate *g)
{
    g->readers--;
}

/* down_write() waits for every reader that already holds the gate. */
static bool destroy_take_gate(struct stats_gate *g)
{
    if (g->readers)
        return false;
    g->writer = true;
    return true;
}

static void destroy_free_children(struct stats_gate *g)
{
    ok(g->writer, "stats gate: destroy owns the write side");
    ok_int(g->readers, 0, "stats gate: no render is in flight");
    g->children_valid = false;
}

static void test_stats_render_gate(void)
{
    struct stats_gate g = { .children_valid = true };

    ok(stats_enter(&g), "stats gate: a reader enters");
    ok(!destroy_take_gate(&g), "stats gate: destroy waits for the render");
    ok(g.children_valid, "stats gate: children are alive during the render");
    stats_leave(&g);
    ok(destroy_take_gate(&g), "stats gate: destroy proceeds after the render");
    destroy_free_children(&g);
    g.writer = false; /* up_write() precedes the drained wait */
    ok(stats_enter(&g), "stats gate: a late reader enters after teardown");
    ok(!g.children_valid, "stats gate: a late reader sees no children");
    stats_leave(&g);
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
    test_flow_free_is_drained_before_unload();
    test_rcu_barriers_do_not_drain_flow_free();
    test_wq_destroy_waits_for_running_free();
    test_late_put_is_reported_not_queued();
    test_stats_render_gate();

    if (failures)
    {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_lifetime: %d checks passed\n", checks);
    return 0;
}
