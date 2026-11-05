/* test_ack.c - SPEC test for P2a: per-stream tx_ack ownership.
 *
 * WHAT THIS IS
 *   A pure host model of the intended P2a rules, not a link against the
 *   kernel module. It mirrors exactly four decisions:
 *
 *     - a stream starts with tx_ack = 0 and owns its own ack;
 *     - a session-level ack (sid 0, e.g. FBK) applies only to the current
 *       active stream (session.far_sid);
 *     - a frame for a retired/old sid is ignored for ack accounting and its
 *       DATA is dropped by the pump;
 *     - retiring a stream does not migrate its tx_ack to the next stream.
 *
 *   When the kernel P2a lands, keep this model and tbt_tx.c in lockstep:
 *   tbt_tx_ack() must use far_sid (not the frame sid) for session-level acks,
 *   and the pump must drop DATA whose sid is not far_sid.
 *
 * WHY A MODEL
 *   The kernel needs sockets, RCU, workqueues and the TX queue. The ownership
 *   rule needs only sid, far_sid, tx_ack and a retired flag.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_STREAMS 4096

static int checks;
static int failures;

static void ok(bool cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "",
           detail ? detail : "");
}

struct mstream
{
    uint32_t sid;
    uint64_t tx_ack;
    int refs;
    bool retired;
};

struct msession
{
    uint32_t far_sid; /* 0 = no active stream */
    struct mstream st[MAX_STREAMS];
    int n;
    uint64_t stale_frames;
};

static struct mstream *find(struct msession *s, uint32_t sid)
{
    int i;

    for (i = 0; i < s->n; i++)
        if (s->st[i].sid == sid)
            return &s->st[i];
    return NULL;
}

static struct mstream *active(struct msession *s)
{
    return s->far_sid ? find(s, s->far_sid) : NULL;
}

static struct mstream *stream_new(struct msession *s, uint32_t sid)
{
    struct mstream *st;

    if (s->n == MAX_STREAMS)
        return NULL;
    st = &s->st[s->n++];
    memset(st, 0, sizeof(*st));
    st->sid = sid;
    st->tx_ack = 0;
    s->far_sid = sid;
    return st;
}

static void stream_retire(struct msession *s, uint32_t sid)
{
    struct mstream *st = find(s, sid);

    if (!st)
        return;
    st->retired = true;
    if (s->far_sid == sid)
        s->far_sid = 0;
}

/* The kernel rule: a session-level ack (sid 0) names the active stream; a
 * nonzero frame sid must still be the active sid. Retired sids are ignored.
 * The ack is monotone and clamped to the stream's sent offset. */
static void ack_in(struct msession *s, uint32_t frame_sid, uint64_t uack,
                   uint64_t tx_seq)
{
    struct mstream *st;
    uint32_t sid = s->far_sid;

    if (!uack || !sid)
        return;
    if (frame_sid && frame_sid != sid)
        return; /* old/retired frame */
    st = active(s);
    if (!st || st->retired)
        return;
    if (uack > tx_seq)
        uack = tx_seq;
    if (uack > st->tx_ack)
        st->tx_ack = uack;
}

/* The pump's DATA admission: the frame must name the active sid. */
static bool data_admitted(struct msession *s, uint32_t frame_sid)
{
    if (!s->far_sid || frame_sid != s->far_sid)
    {
        s->stale_frames++;
        return false;
    }
    return true;
}

static bool window_ok(const struct mstream *st, uint64_t seq, uint32_t len,
                      uint64_t window)
{
    return seq + len <= st->tx_ack + window;
}

static void test_independent_acks(void)
{
    struct msession s = {0};
    struct mstream *a = stream_new(&s, 1);
    struct mstream *b = stream_new(&s, 2);

    ok(a && b, "two streams exist", "");
    ok(a->tx_ack == 0 && b->tx_ack == 0, "both start at 0", "");
    ack_in(&s, 0, 100, 1000);
    ok(a->tx_ack == 0, "sid 0 applies to active (2), not stream 1", "");
    ok(b->tx_ack == 100, "active stream 2 got the session ack", "");
    ack_in(&s, 1, 50, 1000);
    ok(a->tx_ack == 0, "old sid 1 is ignored while 2 is active", "");
    ack_in(&s, 2, 40, 1000);
    ok(b->tx_ack == 100, "ack is monotone", "");
    ack_in(&s, 2, 200, 150);
    ok(b->tx_ack == 150, "ack clamps to tx_seq", "");
}

static void test_retire_and_new(void)
{
    struct msession s = {0};
    struct mstream *a = stream_new(&s, 7);
    struct mstream *b;

    ack_in(&s, 7, 500, 500);
    ok(a->tx_ack == 500, "stream 7 acked to 500", "");
    stream_retire(&s, 7);
    ok(s.far_sid == 0, "retire clears far_sid", "");
    b = stream_new(&s, 8);
    ok(b && b->tx_ack == 0, "new stream starts at 0", "");
    ack_in(&s, 7, 900, 1000);
    ok(a->tx_ack == 500, "retired stream 7 is not updated", "");
    ok(b->tx_ack == 0, "old sid does not touch new stream 8", "");
    ack_in(&s, 8, 10, 1000);
    ok(b->tx_ack == 10, "new stream takes its own ack", "");
    ok(!window_ok(b, 1000, 10, 100), "new stream window uses its own ack", "");
}

static void test_churn_no_cross_talk(void)
{
    struct msession s = {0};
    uint32_t sid;
    int i;

    for (i = 0; i < 2000; i++)
    {
        struct mstream *st;
        uint64_t old_ack;

        sid = 100 + i;
        st = stream_new(&s, sid);
        ok(st != NULL, "churn stream alloc", "");
        if (!st)
            break;
        old_ack = (uint64_t)i * 17;
        ack_in(&s, 0, old_ack, old_ack + 1);
        ok(st->tx_ack == old_ack, "churn session ack hits current", "");
        ok(data_admitted(&s, sid), "current DATA admitted", "");
        if (i > 0)
            ok(!data_admitted(&s, sid - 1), "old DATA dropped", "");
        stream_retire(&s, sid);
        ack_in(&s, sid, old_ack + 1000, old_ack + 1000);
        ok(st->tx_ack == old_ack, "late ack after retire ignored", "");
    }
    ok(s.far_sid == 0, "churn ends with no active stream", "");
}


struct mframe
{
    uint32_t sid;
    bool queued;
};

static void frame_enqueue(struct mstream *st, struct mframe *f, uint32_t sid)
{
    f->sid = sid;
    f->queued = true;
    st->refs++;
}

static void frame_put(struct mstream *st, struct mframe *f)
{
    if (!f->queued)
        return;
    f->queued = false;
    st->refs--;
}

/* CLOSE_WAIT early retire: session gate clears, but queued DATA stays for
 * half-close delivery. */
static void close_wait_retire(struct msession *s, struct mstream *st)
{
    st->retired = true;
    if (s->far_sid == st->sid)
        s->far_sid = 0;
}

/* Terminal kill: purge only frames owned by this stream. */
static void kill_purge(struct mframe *frames, int n, struct mstream *st)
{
    int i;

    for (i = 0; i < n; i++)
        if (frames[i].queued && frames[i].sid == st->sid)
            frame_put(st, &frames[i]);
}

static void test_frame_refs(void)
{
    struct msession s = {0};
    struct mstream *st = stream_new(&s, 42);
    struct mframe f[3];
    int i;

    for (i = 0; i < 3; i++)
        frame_enqueue(st, &f[i], 42);
    ok(st->refs == 3, "three queued frames hold three refs", "");
    frame_put(st, &f[0]);
    ok(st->refs == 2, "write/drop puts once", "");
    close_wait_retire(&s, st);
    ok(s.far_sid == 0 && st->refs == 2, "CLOSE_WAIT retire keeps queued refs", "");
    ok(f[1].queued && f[2].queued, "half-close data still queued", "");
    kill_purge(f, 3, st);
    ok(st->refs == 0, "kill purge releases owned frames", "");
    ok(!f[1].queued && !f[2].queued, "purged frames marked", "");
    frame_put(st, &f[1]);
    ok(st->refs == 0, "double put is idempotent", "");
}

static void test_data_ownership(void)
{
    struct msession s = {0};

    ok(!data_admitted(&s, 1), "no stream: DATA dropped", "");
    stream_new(&s, 1);
    ok(data_admitted(&s, 1), "current sid admitted", "");
    ok(!data_admitted(&s, 2), "foreign sid dropped", "");
    stream_retire(&s, 1);
    ok(!data_admitted(&s, 1), "retired sid dropped", "");
    ok(s.stale_frames == 3, "stale frame count", "");
}

int main(void)
{
    test_independent_acks();
    test_retire_and_new();
    test_churn_no_cross_talk();
    test_frame_refs();
    test_data_ownership();

    printf("test_ack: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
