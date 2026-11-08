/* test_fec_out.c - host model of the per-carrier FEC sender scheduler.
 *
 * WHAT THIS IS
 *   An executable specification of the output-queue rules in
 *   tbt_fec_data.c, not a link against the kernel file (that file needs
 *   sk_buff, workqueues and the carrier locks):
 *
 *     - tbt_fec_out_flush(): control first, then the session replay queue.
 *       The bound data queues are drained by one work per carrier
 *       (tbt_fec_out_flush_carrier()): -EAGAIN leaves that carrier's head
 *       and re-arms only its own work; it never gates the other carriers or
 *       the packetizer.
 *     - tbt_fec_out_drain_work(): takes the queue head up to BATCH_MAX
 *       records (and a bounded framed total) and commits them with one
 *       tbt_carrier_send_fec_batch(); -EAGAIN keeps the whole batch.
 *     - tbt_fec_out_push(): data bound to its share's carrier with per-carrier
 *       depth/byte caps; replay on a session queue with a session byte
 *       budget; control on its own queue.
 *     - tbt_fec_out_try(): replay starts at the carrier with the fewest
 *       queued bytes and rotates on -EAGAIN, so one full socket cannot hold
 *       a repair; a data fallback scan does not stop at the first -EAGAIN.
 *     - NACK->wire latency is charged only when a carrier accepts a replay.
 *
 *   The constants are smaller than the kernel's so a test can fill a queue
 *   without allocating megabytes. The branch order and the accounting are
 *   the same; a change here that the kernel does not share is a defect.
 *
 *   make host-test
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCAR 8
#define OUTQ_DEPTH_MAX 32u
#define OUTQ_BYTES_MAX (OUTQ_DEPTH_MAX * REC_LEN)
#define REPLAY_BYTES_MAX (16u * REC_LEN)
#define REC_LEN 64u
#define NACK_REQ_MAX 256u
#define BATCH_MAX 8
/* The kernel caps a batch by the carrier's short-write tail, which holds
 * BATCH_MAX frames. The model's cap matches that whole-part budget. */
#define BATCH_BYTES_MAX (BATCH_MAX * REC_LEN)

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

static void ok_u32(uint32_t got, uint32_t want, const char *what)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "got %u, want %u", got, want);
    ok(got == want, what, buf);
}

struct rec
{
    struct rec *next;
    uint16_t carrier;
    uint32_t len;
    bool control;
    bool replay;
    uint64_t stamp_ns;
    int id;
};

struct outq
{
    struct rec *head;
    struct rec *tail;
    uint32_t depth;
    uint32_t bytes;
};

struct sched
{
    struct outq q[NCAR];
    struct rec *ctl;
    struct rec *replay_head;
    struct rec *replay_tail;
    uint32_t queued;
    uint32_t ctl_depth;
    uint32_t ctl_bytes;
    uint32_t replay_queued;
    uint32_t replay_bytes;
    uint64_t flush_skip;
    uint64_t flush_pass;
    uint64_t hol_wait;
    uint64_t outq_drop;
    uint64_t replay_skip;
    uint64_t replay_enospc;
    uint64_t replay_drop;
    uint64_t nack_lat_ns;
    uint32_t batches[NCAR];
    /* Fake carrier state. */
    bool up[NCAR];
    bool connected[NCAR];
    bool stalled[NCAR];
    bool full[NCAR];
    uint32_t writes[NCAR];
    uint32_t wire_bytes[NCAR];
    int write_seq[128];
    int nwrite;
};

static void sched_init(struct sched *s)
{
    int i;

    memset(s, 0, sizeof(*s));
    for (i = 0; i < NCAR; i++)
    {
        s->up[i] = true;
        s->connected[i] = true;
    }
}

static struct rec *rec_new(int id, uint16_t carrier, bool control, bool replay,
                           uint64_t stamp_ns)
{
    struct rec *r = calloc(1, sizeof(*r));

    if (!r)
        abort();
    r->id = id;
    r->carrier = carrier;
    r->len = REC_LEN;
    r->control = control;
    r->replay = replay;
    r->stamp_ns = stamp_ns;
    return r;
}

static void q_push_tail(struct outq *q, struct rec *r)
{
    if (q->tail)
        q->tail->next = r;
    else
        q->head = r;
    q->tail = r;
}

static struct rec *q_pop_head(struct outq *q)
{
    struct rec *r = q->head;

    if (!r)
        return NULL;
    q->head = r->next;
    if (!q->head)
        q->tail = NULL;
    r->next = NULL;
    return r;
}

/* Mirrors tbt_fec_out_push(). */
static int push(struct sched *s, uint16_t carrier, bool control, bool replay,
                uint64_t stamp_ns, int id)
{
    struct rec *r;
    struct outq *q = NULL;

    if (!control && !replay)
    {
        if (carrier >= NCAR)
            return -EINVAL;
        q = &s->q[carrier];
    }
    r = rec_new(id, carrier, control, replay, stamp_ns);
    if (replay && s->replay_bytes + r->len > REPLAY_BYTES_MAX)
    {
        free(r);
        s->replay_enospc++;
        return -ENOSPC;
    }
    if (q && (q->depth >= OUTQ_DEPTH_MAX ||
              q->bytes + r->len > OUTQ_BYTES_MAX))
    {
        free(r);
        s->outq_drop++;
        return -ENOSPC;
    }
    if (control)
    {
        r->next = s->ctl;
        s->ctl = r;
        s->ctl_depth++;
        s->ctl_bytes += r->len;
    }
    else if (replay)
    {
        if (s->replay_tail)
            s->replay_tail->next = r;
        else
            s->replay_head = r;
        s->replay_tail = r;
        s->replay_queued++;
        s->replay_bytes += r->len;
    }
    else
    {
        q_push_tail(q, r);
        q->depth++;
        q->bytes += r->len;
    }
    s->queued++;
    return 0;
}

static void rec_free(struct sched *s, struct outq *q, struct rec *r)
{
    if (r->replay)
    {
        if (s->replay_queued)
            s->replay_queued--;
        if (s->replay_bytes >= r->len)
            s->replay_bytes -= r->len;
    }
    if (r->control)
    {
        if (s->ctl_depth)
            s->ctl_depth--;
        if (s->ctl_bytes >= r->len)
            s->ctl_bytes -= r->len;
    }
    if (q)
    {
        q->depth--;
        q->bytes -= r->len;
    }
    if (s->queued)
        s->queued--;
    free(r);
}

/* Release whatever a case deliberately left queued, so ASan sees no leak. */
static void sched_free(struct sched *s)
{
    int i;

    while (s->ctl)
    {
        struct rec *r = s->ctl;

        s->ctl = r->next;
        free(r);
    }
    while (s->replay_head)
    {
        struct rec *r = s->replay_head;

        s->replay_head = r->next;
        free(r);
    }
    for (i = 0; i < NCAR; i++)
        while (s->q[i].head)
            free(q_pop_head(&s->q[i]));
}

static bool usable(const struct sched *s, uint16_t idx)
{
    return s->up[idx] && s->connected[idx] && !s->stalled[idx];
}

/* Mirrors tbt_carrier_send_fec()'s backpressure check. */
static int carrier_send(struct sched *s, uint16_t idx, uint32_t len, int id)
{
    if (s->full[idx])
        return -EAGAIN;
    s->writes[idx]++;
    s->wire_bytes[idx] += len;
    if (s->nwrite < (int)(sizeof(s->write_seq) / sizeof(s->write_seq[0])))
        s->write_seq[s->nwrite++] = id;
    return 0;
}

static int out_try_control(struct sched *s, struct rec *r, bool *sent)
{
    int rc = -ENODEV;
    int i;

    for (i = 0; i < NCAR; i++)
    {
        int err;

        if (!s->up[i] || s->stalled[i])
            continue;
        err = carrier_send(s, (uint16_t)i, r->len, r->id);
        if (!err)
        {
            *sent = true;
            return 0;
        }
        if (err == -EAGAIN)
            rc = -EAGAIN;
    }
    return rc;
}

/* Mirrors tbt_fec_out_try_replay(): start at the least queued carrier, then
 * rotate. `pace` includes the replay bytes already written in this pass. */
static int out_try_replay(struct sched *s, struct rec *r, bool *sent,
                          const uint8_t *usable_mask, uint16_t ncand,
                          uint32_t *pace)
{
    uint16_t start = UINT16_MAX;
    uint32_t best = UINT32_MAX;
    int rc = -ENODEV;
    int i;

    if (!ncand)
        return -EAGAIN;
    for (i = 0; i < NCAR; i++)
    {
        if (!usable_mask[i])
            continue;
        if (pace[i] < best)
        {
            best = pace[i];
            start = (uint16_t)i;
        }
    }
    if (start == UINT16_MAX)
        return -EAGAIN;
    for (i = 0; i < NCAR; i++)
    {
        uint16_t idx = (uint16_t)((start + i) % NCAR);
        int err;

        if (!usable_mask[idx] || !usable(s, idx))
            continue;
        err = carrier_send(s, idx, r->len, r->id);
        if (!err)
        {
            *sent = true;
            pace[idx] += r->len;
            return 0;
        }
        if (err == -EAGAIN)
        {
            rc = -EAGAIN;
            continue;
        }
        if (err == -ENOTCONN || err == -ENODEV)
            continue;
        rc = err;
    }
    return rc;
}

/* Mirrors the data branch of tbt_fec_out_try(). */
static int out_try_data(struct sched *s, struct rec *r, bool *sent)
{
    int rc = -ENODEV;
    int i;

    if (r->carrier >= NCAR)
        return -ENODEV;
    if (s->stalled[r->carrier])
        return 0; /* source share dropped; the replay ring repairs it */
    if (usable(s, r->carrier))
    {
        int err = carrier_send(s, r->carrier, r->len, r->id);

        if (!err)
        {
            *sent = true;
            return 0;
        }
        if (err == -EAGAIN)
            return -EAGAIN;
        return err;
    }
    for (i = 0; i < NCAR; i++)
    {
        int err;

        if (i == r->carrier || !usable(s, (uint16_t)i))
            continue;
        err = carrier_send(s, (uint16_t)i, r->len, r->id);
        if (!err)
        {
            *sent = true;
            return 0;
        }
        rc = err; /* a full carrier must not hide a later one */
    }
    return rc == -ENODEV ? -EAGAIN : rc;
}

/* Mirrors tbt_fec_out_flush_carrier(): one carrier's bound queue. -EAGAIN
 * keeps that carrier's head and reports `again`; no other queue is touched. */
static uint32_t flush_carrier(struct sched *s, int idx, bool *again)
{
    struct outq *q = &s->q[idx];
    uint32_t wrote = 0;

    *again = false;
    for (;;)
    {
        struct rec *r = q->head;
        uint32_t depth = q->depth;
        bool sent = false;
        int err;

        if (!r)
            break;
        err = out_try_data(s, r, &sent);
        if (err == -EAGAIN)
        {
            s->flush_skip++;
            s->hol_wait += depth;
            *again = true;
            break;
        }
        q_pop_head(q);
        rec_free(s, q, r);
        if (sent)
            wrote++;
    }
    s->flush_pass += wrote;
    return wrote;
}

/* The per-carrier works: every carrier gets its own attempt; a full one does
 * not stop the others. */
static uint32_t drain_all(struct sched *s, bool *again_any)
{
    uint32_t wrote = 0;
    int i;

    *again_any = false;
    for (i = 0; i < NCAR; i++)
    {
        bool again = false;

        wrote += flush_carrier(s, i, &again);
        if (again)
            *again_any = true;
    }
    return wrote;
}

/* Mirrors tbt_carrier_send_fec_batch(): one carrier write for the whole
 * batch. -EAGAIN means no record of the batch was committed. */
static int carrier_send_batch(struct sched *s, int idx, struct rec **recs,
                              int n)
{
    uint32_t bytes = 0;
    int i;

    if (s->full[idx])
        return -EAGAIN;
    s->batches[idx]++;
    for (i = 0; i < n; i++)
    {
        s->writes[idx]++;
        bytes += recs[i]->len;
        if (s->nwrite < (int)(sizeof(s->write_seq) / sizeof(s->write_seq[0])))
            s->write_seq[s->nwrite++] = recs[i]->id;
    }
    s->wire_bytes[idx] += bytes;
    return 0;
}

/* Mirrors tbt_fec_out_drain_work()'s batch loop: take the queue head up to
 * BATCH_MAX records and BATCH_BYTES_MAX framed bytes, send once, and remove
 * exactly what the carrier accepted. */
static uint32_t drain_batch(struct sched *s, int idx, bool *again)
{
    struct outq *q = &s->q[idx];
    uint32_t wrote = 0;

    *again = false;
    for (;;)
    {
        struct rec *recs[BATCH_MAX];
        struct rec *r;
        uint32_t bytes = 0;
        int n = 0, i, err;

        for (r = q->head; r && n < BATCH_MAX; r = r->next)
        {
            if (bytes + r->len > BATCH_BYTES_MAX)
                break;
            recs[n++] = r;
            bytes += r->len;
        }
        if (!n)
            break;
        err = carrier_send_batch(s, idx, recs, n);
        if (err == -EAGAIN)
        {
            s->flush_skip++;
            s->hol_wait += q->depth;
            *again = true;
            break;
        }
        for (i = 0; i < n; i++)
            rec_free(s, q, q_pop_head(q));
        wrote += n;
        s->flush_pass += n;
    }
    return wrote;
}

/* Mirrors tbt_fec_out_flush(): control and replay only. The bound data
 * queues are the per-carrier works' responsibility. */
static int flush(struct sched *s, uint32_t *written)
{
    int ret = 0;
    uint32_t pass = 0;
    int i;

    *written = 0;
    while (s->ctl)
    {
        struct rec *r = s->ctl;
        bool sent = false;
        int err;

        s->ctl = r->next;
        r->next = NULL;
        err = out_try_control(s, r, &sent);
        if (err == -EAGAIN)
        {
            r->next = s->ctl;
            s->ctl = r;
            ret = -EAGAIN;
            break;
        }
        rec_free(s, NULL, r);
        if (sent)
            pass++;
    }
    {
        uint8_t usable_mask[NCAR];
        uint32_t pace[NCAR];
        uint16_t ncand = 0;

        for (i = 0; i < NCAR; i++)
        {
            usable_mask[i] = usable(s, (uint16_t)i) ? 1 : 0;
            pace[i] = s->q[i].bytes;
            if (usable_mask[i])
                ncand++;
        }
        while (s->replay_head)
        {
            struct rec *r = s->replay_head;
            bool sent = false;
            int err;

            s->replay_head = r->next;
            if (!s->replay_head)
                s->replay_tail = NULL;
            r->next = NULL;
            err = out_try_replay(s, r, &sent, usable_mask, ncand, pace);
            if (err == -EAGAIN)
            {
                r->next = s->replay_head;
                s->replay_head = r;
                if (!s->replay_tail)
                    s->replay_tail = r;
                s->replay_skip++;
                s->hol_wait += s->replay_queued;
                ret = -EAGAIN;
                break;
            }
            rec_free(s, NULL, r);
            if (sent)
            {
                s->nack_lat_ns += 1000; /* one fake tick */
                pass++;
            }
            else
            {
                s->replay_drop++;
            }
        }
    }
    if (pass)
        s->flush_pass += pass;
    *written = pass;
    return ret;
}

/* ---------------------------------------------------------------- *
 * Cases
 * ---------------------------------------------------------------- */

/* One carrier's socket is full; every other carrier keeps sending. The old
 * single-list flush wrote nothing in this state. */
static void case_eagain_isolated(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    s.full[0] = true;
    for (i = 0; i < NCAR; i++)
        push(&s, (uint16_t)i, false, false, 0, i);

    wrote = drain_all(&s, &again);
    ok(again, "isolated EAGAIN reports its carrier", NULL);
    ok_u32(wrote, NCAR - 1, "every other carrier wrote");
    ok_u32(s.writes[0], 0, "full carrier wrote nothing");
    ok_u32(s.q[0].depth, 1, "full carrier kept its head");
    for (i = 1; i < NCAR; i++)
        ok_u32(s.q[i].depth, 0, "healthy carrier drained");
    ok_u32((uint32_t)s.flush_skip, 1, "one carrier skipped");
    ok_u32((uint32_t)s.hol_wait, 1, "one record waits behind the head");

    s.full[0] = false;
    wrote = drain_all(&s, &again);
    ok(!again, "retry clears the flag", NULL);
    ok_u32(wrote, 1, "retry wrote the head");
    ok_u32(s.queued, 0, "no record left");
    sched_free(&s);
}

/* A congested data head must not hold an ACK/NACK on the control queue. */
static void case_control_not_blocked(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;

    sched_init(&s);
    s.full[0] = true;
    push(&s, 0, false, false, 0, 1); /* data head on carrier 0 */
    push(&s, 0, true, false, 0, 2);  /* ACK */

    ok(flush(&s, &wrote) == 0, "session flush ignores the data queue", NULL);
    ok_u32(wrote, 1, "control record written");
    ok(s.ctl == NULL, "control queue empty", NULL);
    ok_u32(s.q[0].depth, 1, "data head still queued");
    ok_u32(s.writes[1], 1, "control used the next carrier");
    ok_u32(s.writes[0], 0, "full carrier took no record");
    ok_u32(s.ctl_depth, 0, "control depth released");
    ok_u32(s.ctl_bytes, 0, "control bytes released");
    wrote = flush_carrier(&s, 0, &again);
    ok(again, "the data carrier reports its own backpressure", NULL);
    ok_u32(wrote, 0, "the full carrier wrote nothing");
    sched_free(&s);
}

/* A per-carrier queue cap rejects only that carrier's record. */
static void case_cap_is_per_carrier(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    s.full[0] = true;
    for (i = 0; i < (int)OUTQ_DEPTH_MAX; i++)
        ok(push(&s, 0, false, false, 0, i) == 0, "fill carrier 0", NULL);
    ok(push(&s, 0, false, false, 0, 99) == -ENOSPC,
       "cap rejects the next carrier-0 record", NULL);
    ok_u32((uint32_t)s.outq_drop, 1, "drop counted");
    ok_u32(s.q[0].depth, OUTQ_DEPTH_MAX, "carrier 0 stays at the cap");

    ok(push(&s, 1, false, false, 0, 100) == 0,
       "carrier 1 accepts while carrier 0 is full", NULL);
    wrote = drain_all(&s, &again);
    ok(again, "the capped carrier reports backpressure", NULL);
    ok_u32(wrote, 1, "carrier 1 wrote");
    ok_u32(s.q[1].depth, 0, "carrier 1 drained");
    sched_free(&s);
}

/* The sweep visits every carrier and skips only the full one, in any order. */
static void case_sweep_visits_all(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    s.full[3] = true;
    for (i = 0; i < NCAR; i++)
        push(&s, (uint16_t)i, false, false, 0, i);

    wrote = drain_all(&s, &again);
    ok(again, "sweep leaves one carrier", NULL);
    ok_u32(wrote, NCAR - 1, "sweep wrote every other carrier");
    ok_u32((uint32_t)s.flush_skip, 1, "exactly one skip");
    ok_u32(s.writes[7], 1, "carrier after the full one was visited");
    ok_u32(s.writes[4], 1, "carrier after the full one was visited (2)");
    ok_u32(s.q[3].depth, 1, "full carrier keeps its head");
    sched_free(&s);
}

/* Replay records spread by queue bytes; one NACK's 256 shares cannot fill one
 * queue, and the session byte budget rejects the excess instead of a global
 * 256-record cap failing the whole batch. */
static void case_replay_spread_and_budget(void)
{
    struct sched s;
    uint32_t wrote;
    int i;
    int enospc = 0;

    sched_init(&s);
    for (i = 0; i < (int)NACK_REQ_MAX; i++)
    {
        if (push(&s, 0, false, true, 1000, i) == -ENOSPC)
            enospc++;
    }
    ok(enospc > 0, "byte budget rejects beyond the bound", NULL);
    ok_u32((uint32_t)s.replay_enospc, (uint32_t)enospc,
           "each rejection counted");
    ok_u32(s.replay_bytes, REPLAY_BYTES_MAX, "budget filled exactly");
    ok_u32(s.replay_queued, REPLAY_BYTES_MAX / REC_LEN,
           "queued replay records");

    ok(flush(&s, &wrote) == 0, "all replay written", NULL);
    ok_u32(wrote, REPLAY_BYTES_MAX / REC_LEN, "flush wrote the whole budget");
    ok_u32((uint32_t)s.nack_lat_ns, (REPLAY_BYTES_MAX / REC_LEN) * 1000,
           "latency charged per write");
    for (i = 0; i < NCAR; i++)
        ok_u32(s.writes[i], 2, "replay spread evenly over the carriers");
    ok_u32(s.replay_bytes, 0, "replay byte budget released");
    ok_u32(s.replay_queued, 0, "replay count released");
    sched_free(&s);
}

/* A replay is not bound to a stalled or full carrier: it rotates to a live
 * one instead of being dropped or waiting behind a data head. */
static void case_replay_fallback(void)
{
    struct sched s;
    uint32_t wrote;

    sched_init(&s);
    s.stalled[0] = true;
    s.full[1] = true;
    ok(push(&s, 0, false, true, 500, 1) == 0, "replay queued", NULL);
    ok(flush(&s, &wrote) == 0, "replay flushed through a live carrier", NULL);
    ok_u32(wrote, 1, "one replay written");
    ok_u32(s.writes[2], 1, "rotation passed stalled 0 and full 1");
    ok_u32((uint32_t)s.nack_lat_ns, 1000, "latency charged on fallback");
    ok_u32(s.replay_drop, 0, "replay was not dropped");
    sched_free(&s);
}

/* Replay drains before bound data, and a full replay budget does not stop
 * data from queuing or writing. */
static void case_replay_priority_and_isolation(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    for (i = 0; i < (int)REPLAY_BYTES_MAX / (int)REC_LEN; i++)
        push(&s, 0, false, true, 0, 100 + i);
    push(&s, 3, false, false, 0, 1); /* data behind the replay pass */
    ok(flush(&s, &wrote) == 0, "replay drains first", NULL);
    ok_u32(wrote, REPLAY_BYTES_MAX / REC_LEN, "replay written");
    wrote = drain_all(&s, &again);
    ok(!again, "data drains after replay", NULL);
    ok_u32(wrote, 1, "data written after replay");
    ok(s.write_seq[0] >= 100, "a replay was written first", NULL);
    ok(s.write_seq[s.nwrite - 1] == 1, "data was written after replay", NULL);

    /* The budget is full again: data must still queue and flush. */
    for (i = 0; i < (int)REPLAY_BYTES_MAX / (int)REC_LEN; i++)
        push(&s, 0, false, true, 0, 200 + i);
    ok(push(&s, 4, false, false, 0, 2) == 0,
       "data accepted while the replay budget is full", NULL);
    ok(flush(&s, &wrote) == 0, "replay drains while data waits", NULL);
    ok_u32(wrote, REPLAY_BYTES_MAX / REC_LEN, "replay written");
    wrote = drain_all(&s, &again);
    ok(!again, "data drains even if replay cannot", NULL);
    ok_u32(wrote, 1, "data written after the replay burst");
    sched_free(&s);
}

/* A full carrier's retry belongs to that carrier alone: it does not block a
 * ready carrier, and the session flush never touches the data queues. */
static void case_per_carrier_retry(void)
{
    struct sched s;
    uint32_t wrote;
    bool again0, again1;

    sched_init(&s);
    s.full[0] = true;
    push(&s, 0, false, false, 0, 1);
    push(&s, 1, false, false, 0, 2);

    ok(flush(&s, &wrote) == 0, "session flush leaves data alone", NULL);
    ok_u32(wrote, 0, "session flush wrote no data");
    wrote = flush_carrier(&s, 0, &again0);
    ok(again0 && wrote == 0, "the full carrier keeps its head", NULL);
    wrote = flush_carrier(&s, 1, &again1);
    ok(!again1 && wrote == 1, "the ready carrier drains independently", NULL);
    ok_u32(s.writes[0], 0, "full carrier wrote nothing");
    ok_u32(s.writes[1], 1, "ready carrier wrote its record");

    s.full[0] = false;
    wrote = flush_carrier(&s, 0, &again0);
    ok(!again0 && wrote == 1, "only the blocked carrier retries", NULL);
    sched_free(&s);
}

/* The per-carrier drain batches the queue head: one carrier write for up to
 * BATCH_MAX records / BATCH_BYTES_MAX bytes, in queue order; -EAGAIN keeps
 * the whole batch. */
static void case_batch_drain(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    for (i = 0; i < 20; i++)
        push(&s, 0, false, false, 0, i);

    wrote = drain_batch(&s, 0, &again);
    ok(!again, "batch drain completes", NULL);
    ok_u32(wrote, 20, "batch drain wrote every record");
    ok_u32(s.q[0].depth, 0, "batch drain emptied the queue");
    ok_u32(s.batches[0], 3, "20 records at 8 per batch take three writes");
    ok_u32(s.writes[0], 20, "every record accepted");
    ok_u32(s.write_seq[0], 0, "the first batch keeps queue order");
    ok_u32(s.write_seq[19], 19, "the last batch keeps queue order");

    /* Eight full records fit one batch: the whole part buffer is used. */
    for (i = 0; i < BATCH_MAX; i++)
        push(&s, 1, false, false, 0, 200 + i);
    wrote = drain_batch(&s, 1, &again);
    ok(!again && wrote == BATCH_MAX, "eight records fit one batch", NULL);
    ok_u32(s.batches[1], 1, "one carrier write for eight records");

    s.full[0] = true;
    push(&s, 0, false, false, 0, 100);
    push(&s, 0, false, false, 0, 101);
    wrote = drain_batch(&s, 0, &again);
    ok(again && wrote == 0, "EAGAIN keeps the whole batch", NULL);
    ok_u32(s.q[0].depth, 2, "both records stay queued");
    ok_u32(s.batches[0], 3, "a refused batch did not count as a write");

    s.full[0] = false;
    wrote = drain_batch(&s, 0, &again);
    ok(!again && wrote == 2, "retry writes the kept batch", NULL);
    ok_u32(s.batches[0], 4, "the retry is one batch");
    sched_free(&s);
}

/* A stream switch drops the old stream's queued TX state and releases its
 * credit frontier; the new stream's records still drain. */
static void case_stream_switch_cleanup(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    push(&s, 0, false, false, 0, 1); /* old data */
    push(&s, 0, false, true, 0, 2);  /* old replay */
    push(&s, 0, true, false, 0, 3);  /* old control */
    ok_u32(s.queued, 3, "switch: old records queued");

    /* The kernel's switch frees the old sid's queues and resets peer_pkts;
     * this model has no sid, so all old queues are dropped. */
    while (s.ctl)
    {
        struct rec *r = s.ctl;

        s.ctl = r->next;
        rec_free(&s, NULL, r);
    }
    while (s.replay_head)
    {
        struct rec *r = s.replay_head;

        s.replay_head = r->next;
        if (!s.replay_head)
            s.replay_tail = NULL;
        rec_free(&s, NULL, r);
    }
    for (i = 0; i < NCAR; i++)
        while (s.q[i].head)
            rec_free(&s, &s.q[i], q_pop_head(&s.q[i]));
    ok_u32(s.queued, 0, "switch: old TX state cleared");
    ok(s.ctl == NULL && s.replay_head == NULL,
       "switch: old control/replay cleared", NULL);

    /* The new stream sends after the switch. */
    push(&s, 0, false, false, 0, 10);
    wrote = drain_all(&s, &again);
    ok(!again && wrote == 1, "switch: the new stream drains", NULL);
    ok_u32(s.writes[0], 1, "switch: the new record reached the carrier");
    sched_free(&s);
}

/* A share may not ride a carrier whose OPEN for this stream is not on the
 * wire yet: it waits in its own queue, and only an up carrier may carry it
 * (or serve as the fallback). */
static void case_open_gate(void)
{
    struct sched s;
    uint32_t wrote;
    bool again;
    int i;

    sched_init(&s);
    s.up[0] = false;
    for (i = 1; i < NCAR; i++)
        s.up[i] = false; /* no carrier has announced this stream yet */
    push(&s, 0, false, false, 0, 1);
    wrote = flush_carrier(&s, 0, &again);
    ok(again && wrote == 0, "DATA waits while no OPEN is on the wire", NULL);
    ok_u32(s.q[0].depth, 1, "the share stays queued");
    ok_u32(s.writes[0], 0, "the unannounced carrier wrote nothing");

    s.up[0] = true; /* the OPEN is written */
    wrote = flush_carrier(&s, 0, &again);
    ok(!again && wrote == 1, "the share goes after the OPEN", NULL);
    ok_u32(s.q[0].depth, 0, "the share left the queue");
    ok_u32(s.writes[0], 1, "the announced carrier wrote it");

    /* A different carrier that already has its OPEN is a valid fallback. */
    s.up[0] = false;
    s.up[1] = true;
    push(&s, 0, false, false, 0, 2);
    wrote = flush_carrier(&s, 0, &again);
    ok(!again && wrote == 1, "an announced fallback carries the share", NULL);
    ok_u32(s.writes[1], 1, "the announced fallback wrote it");
    sched_free(&s);
}

/* The replay ring is indexed by block/group/share, not by carrier: moving a
 * replay to another carrier must not change the record it carries. */
static void case_replay_identity(void)
{
    struct sched s;
    struct rec *r;

    sched_init(&s);
    s.q[0].bytes = 100;
    push(&s, 0, false, true, 0, 77);
    r = s.replay_head;
    ok(r && r->id == 77, "the share identity survives the carrier choice",
       NULL);
    ok_u32(r->carrier, 0, "replay has no bound carrier");
    sched_free(&s);
}

int main(void)
{
    case_eagain_isolated();
    case_control_not_blocked();
    case_cap_is_per_carrier();
    case_sweep_visits_all();
    case_replay_spread_and_budget();
    case_replay_fallback();
    case_replay_priority_and_isolation();
    case_replay_identity();
    case_per_carrier_retry();
    case_batch_drain();
    case_open_gate();
    case_stream_switch_cleanup();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
