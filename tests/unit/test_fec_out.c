/* test_fec_out.c - host model of the lock-free FEC sender scheduler.
 *
 * WHAT THIS IS
 *   An executable specification of the output rules in tbt_fec_data.c, not
 *   a link against the kernel file (that file needs sk_buff, workqueues and
 *   the carrier locks):
 *
 *     - tbt_fec_out_push(): DATA records enter one session FIFO with a
 *       session record/byte ceiling; replay uses a session byte budget and
 *       control its own list.
 *     - tbt_fec_out_drain_work(): peeks the FIFO head and offers it to the
 *       carriers in async_rot order. The first socket whose send space holds
 *       the record gets it; -EAGAIN costs no wait and the scan moves on.
 *       All full: the head stays and the work retries in 2 ms.
 *     - A carrier without room for TBT_SKIP_MS becomes slow; round 0 of the
 *       scan skips it, round 1 gives it a recovery chance. Room again clears
 *       the mark. A slow socket still retransmits its own TCP bytes.
 *     - A success advances async_rot, so the healthy carriers keep taking
 *       records while a slow one is passed over.
 *     - tbt_fec_out_flush(): control first, then the session replay queue.
 *     - tbt_fec_out_try(): replay starts at the least-loaded carrier and
 *       rotates on -EAGAIN. NACK->wire latency is charged only when a
 *       carrier accepts a replay.
 *
 *   The constants are smaller than the kernel's so a test can fill the FIFO
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
/* The model's arrays are sized for the kernel limit; a case may set ncar to
 * 256 to exercise the full carrier set. */
#define NCAR_MAX 256
#define FIFO_SIZE 4096u
#define REC_LEN 64u
#define QUEUE_HIGH 512u
#define QUEUE_BYTES_HIGH (QUEUE_HIGH * REC_LEN)
#define QUEUE_MAX (QUEUE_HIGH + NCAR_MAX)
#define QUEUE_BYTES_MAX ((QUEUE_MAX + NCAR_MAX) * REC_LEN)
#define REPLAY_BYTES_MAX (16u * REC_LEN)
#define REPLAY_HIGH (REPLAY_BYTES_MAX / 2)
#define NACK_REQ_MAX 256u
/* Mirrors TBT_FEC_DRAIN_PASS_MAX: one run writes at most this many records,
 * then yields; a non-empty FIFO re-arms the work with no delay. */
#define DRAIN_PASS_MAX 64
/* Mirrors msecs_to_jiffies(2) as the retry gate. */
#define DRAIN_RETRY_MS 2
/* Mirrors TBT_SKIP_MS. */
#define SKIP_MS 20u

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
    uint16_t carrier; /* the share's scheduled index; debug injector only */
    uint32_t len;
    uint32_t epoch;
    bool control;
    bool replay;
    uint64_t stamp_ns;
    int id;
};

struct sched
{
    struct rec *fifo[FIFO_SIZE];
    uint32_t fifo_head;
    uint32_t fifo_tail;
    struct rec *ctl;
    struct rec *replay_head;
    struct rec *replay_tail;
    uint32_t outq_queued;
    uint32_t outq_bytes;
    uint32_t outq_depth_max;
    uint32_t ctl_depth;
    uint32_t ctl_bytes;
    uint32_t replay_queued;
    uint32_t replay_bytes;
    uint32_t epoch;
    uint16_t rot;
    uint16_t ncar; /* carriers in this case; 1..NCAR_MAX */
    uint64_t flush_skip;
    uint64_t flush_pass;
    uint64_t hol_wait;
    uint64_t outq_drop;
    uint64_t outq_high_wait;
    bool outq_paused;
    uint64_t replay_skip;
    uint64_t replay_enospc;
    uint64_t replay_drop;
    uint64_t nack_lat_ns;
    /* Fake carrier state. `full` means sk_stream_wspace() < REC_LEN. */
    bool up[NCAR_MAX];
    bool connected[NCAR_MAX];
    bool stalled[NCAR_MAX];
    bool full[NCAR_MAX];
    bool slow[NCAR_MAX];
    uint64_t slow_since[NCAR_MAX];
    uint32_t writes[NCAR_MAX];
    uint32_t wire_bytes[NCAR_MAX];
    int write_seq[1024];
    int nwrite;
    /* The fake clock and the work's next delay; -1 means idle. */
    uint64_t now_ms;
    int64_t drain_next_ms;
    uint32_t drain_runs;
};

static void sched_init_n(struct sched *s, uint16_t ncar)
{
    int i;

    memset(s, 0, sizeof(*s));
    if (!ncar || ncar > NCAR_MAX)
        ncar = NCAR_MAX;
    s->ncar = ncar;
    s->drain_next_ms = -1;
    for (i = 0; i < ncar; i++)
    {
        s->up[i] = true;
        s->connected[i] = true;
    }
}

static void sched_init(struct sched *s)
{
    sched_init_n(s, NCAR);
}

static struct rec *rec_new(int id, uint16_t carrier, bool control, bool replay,
                           uint64_t stamp_ns, uint32_t epoch)
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
    r->epoch = epoch;
    return r;
}

static uint32_t fifo_depth(const struct sched *s)
{
    return s->fifo_tail - s->fifo_head;
}

static bool fifo_empty(const struct sched *s)
{
    return s->fifo_head == s->fifo_tail;
}

static void fifo_push(struct sched *s, struct rec *r)
{
    s->fifo[s->fifo_tail & (FIFO_SIZE - 1)] = r;
    s->fifo_tail++;
}

static struct rec *fifo_peek(struct sched *s)
{
    if (fifo_empty(s))
        return NULL;
    return s->fifo[s->fifo_head & (FIFO_SIZE - 1)];
}

static void fifo_pop(struct sched *s)
{
    s->fifo_head++;
}

/* Mirrors tbt_fec_out_kick(): the work is queued with no delay. */
static void kick(struct sched *s)
{
    s->drain_next_ms = 0;
}

/* Mirrors the drain's "every socket full" arm: the head stays put and the
 * work retries in 2 ms. */
static void arm_retry(struct sched *s)
{
    s->drain_next_ms = DRAIN_RETRY_MS;
}

/* Mirrors tbt_fec_out_push(). */
static int push(struct sched *s, uint16_t carrier, bool control, bool replay,
                uint64_t stamp_ns, int id)
{
    struct rec *r;
    bool first = false;

    if (!control && !replay && carrier >= s->ncar)
        return -EINVAL;
    if (!control && !replay &&
        (s->outq_queued >= QUEUE_MAX ||
         s->outq_bytes + REC_LEN > QUEUE_BYTES_MAX ||
         fifo_depth(s) >= FIFO_SIZE))
    {
        s->outq_drop++;
        return -ENOSPC;
    }
    if (replay && s->replay_bytes + REC_LEN > REPLAY_BYTES_MAX)
    {
        s->replay_enospc++;
        return -ENOSPC;
    }
    r = rec_new(id, carrier, control, replay, stamp_ns, s->epoch);
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
        first = fifo_empty(s);
        fifo_push(s, r);
        s->outq_queued++;
        s->outq_bytes += r->len;
        if (s->outq_queued > s->outq_depth_max)
            s->outq_depth_max = s->outq_queued;
        /* The drain re-arms itself while records remain; waking it on the
         * empty->non-empty edge mirrors the kernel producer. */
        if (first)
            kick(s);
    }
    return 0;
}

static void rec_free(struct sched *s, struct rec *r)
{
    if (r->replay)
    {
        if (s->replay_queued)
            s->replay_queued--;
        if (s->replay_bytes >= r->len)
            s->replay_bytes -= r->len;
    }
    else if (r->control)
    {
        if (s->ctl_depth)
            s->ctl_depth--;
        if (s->ctl_bytes >= r->len)
            s->ctl_bytes -= r->len;
    }
    else
    {
        if (s->outq_queued)
            s->outq_queued--;
        if (s->outq_bytes >= r->len)
            s->outq_bytes -= r->len;
    }
    free(r);
}

/* Release whatever a case deliberately left queued, so ASan sees no leak. */
static void sched_free(struct sched *s)
{
    while (!fifo_empty(s))
    {
        struct rec *r = fifo_peek(s);

        fifo_pop(s);
        free(r);
    }
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
}

static bool usable(const struct sched *s, uint16_t idx)
{
    return s->up[idx] && s->connected[idx] && !s->stalled[idx];
}

/* Mirrors tbt_fec_out_slow_note(): a refusal starts the episode clock, and
 * only TBT_SKIP_MS of continuous refusal marks the carrier slow. */
static void slow_note(struct sched *s, uint16_t idx, bool room)
{
    if (room)
    {
        s->slow[idx] = false;
        s->slow_since[idx] = 0;
        return;
    }
    if (!s->slow_since[idx])
        s->slow_since[idx] = s->now_ms ? s->now_ms : 1;
    else if (s->now_ms - s->slow_since[idx] >= SKIP_MS)
        s->slow[idx] = true;
}

/* Mirrors the session-FIFO high-water gate in tbt_fec_tx_outq_high(). */
static bool outq_high(const struct sched *s)
{
    if (s->outq_queued >= QUEUE_HIGH || s->outq_bytes >= QUEUE_BYTES_HIGH)
        return true;
    /* A deep repair backlog yields the packetizer: more new DATA would only
     * compete with the repair for the same sockets. */
    return s->replay_bytes >= REPLAY_HIGH;
}

/* Mirrors the packetizer's gate: the transition into a pause is counted
 * once, and the pause lasts until the FIFO falls below the mark. */
static bool packetizer_gate(struct sched *s)
{
    if (!outq_high(s))
    {
        s->outq_paused = false;
        return false;
    }
    if (!s->outq_paused)
    {
        s->outq_paused = true;
        s->outq_high_wait++;
    }
    return true;
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

    for (i = 0; i < s->ncar; i++)
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

/* Mirrors tbt_fec_out_try_replay(): start at the least loaded carrier, then
 * rotate. `pace` counts the replay bytes written in this pass. */
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
    for (i = 0; i < s->ncar; i++)
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
    for (i = 0; i < s->ncar; i++)
    {
        uint16_t idx = (uint16_t)((start + i) % s->ncar);
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

/* Mirrors tbt_fec_out_dispatch(): offer one record to the carriers in
 * async_rot order. Round 0 skips carriers already slow, round 1 gives them
 * a recovery chance. Returns 0 when a carrier took the record, -EAGAIN when
 * every socket was full. */
static int dispatch(struct sched *s, struct rec *r)
{
    uint16_t start = (uint16_t)(s->rot % s->ncar);
    bool skipped_slow = false;
    int round;

    for (round = 0; round < 2; round++)
    {
        uint16_t i;

        if (round && !skipped_slow)
            break;
        for (i = 0; i < s->ncar; i++)
        {
            uint16_t idx = (uint16_t)((start + i) % s->ncar);
            int err;

            if (!usable(s, idx))
                continue;
            if (!round && s->slow[idx])
            {
                skipped_slow = true;
                continue;
            }
            if (s->full[idx])
            {
                slow_note(s, idx, false);
                continue;
            }
            slow_note(s, idx, true);
            err = carrier_send(s, idx, r->len, r->id);
            if (!err)
            {
                s->rot = (uint16_t)((idx + 1) % s->ncar);
                return 0;
            }
            if (err == -EAGAIN || err == -ENOTCONN || err == -ENODEV)
                continue;
            return err;
        }
    }
    return -EAGAIN;
}

/* Mirrors tbt_fec_out_drain_work(). A run writes at most DRAIN_PASS_MAX
 * records, then re-arms at once while the FIFO has more; a full session gets
 * the 2 ms retry. A run that wrote anything arms one extra zero-delay pass,
 * which the kernel needs to close the producer/consumer race. */
static void drain_work(struct sched *s)
{
    uint16_t served = 0;

    s->drain_runs++;
    while (served < DRAIN_PASS_MAX)
    {
        struct rec *r = fifo_peek(s);
        int err;

        if (!r)
            break;
        if (r->epoch != s->epoch)
        {
            fifo_pop(s);
            rec_free(s, r);
            continue;
        }
        err = dispatch(s, r);
        if (err == -EAGAIN)
        {
            arm_retry(s);
            return;
        }
        fifo_pop(s);
        rec_free(s, r);
        if (err)
        {
            arm_retry(s);
            return;
        }
        s->flush_pass++;
        served++;
    }
    s->drain_next_ms = served ? 0 : -1;
}

/* Run the work only while it re-arms with no delay, as the workqueue would,
 * without advancing the fake clock. Returns the number of runs. */
static int pump_zero_delay(struct sched *s, int max_runs)
{
    int n = 0;

    while (s->drain_next_ms == 0 && n < max_runs)
    {
        drain_work(s);
        n++;
    }
    return n;
}

/* Run the work across timer waits: each iteration advances the fake clock by
 * the delay the work itself armed. */
static int pump_drain(struct sched *s, int max_runs)
{
    int n = 0;

    while (s->drain_next_ms >= 0 && n < max_runs)
    {
        s->now_ms += (uint64_t)s->drain_next_ms;
        drain_work(s);
        n++;
    }
    return n;
}

/* Mirrors tbt_fec_out_flush(): control and replay only. The DATA FIFO is the
 * session drain work's responsibility. */
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
        rec_free(s, r);
        if (sent)
            pass++;
    }
    {
        uint8_t usable_mask[NCAR_MAX];
        uint32_t pace[NCAR_MAX];
        uint16_t ncand = 0;

        for (i = 0; i < s->ncar; i++)
        {
            usable_mask[i] = usable(s, (uint16_t)i) ? 1 : 0;
            pace[i] = 0;
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
                ret = -EAGAIN;
                break;
            }
            rec_free(s, r);
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

/* 256 carriers, 32 with a full send window and 224 idle: every record must
 * reach an idle carrier at once, and no record may wait behind a full one.
 * The old per-carrier queues parked each share behind its own socket and
 * paid fec_hol_wait. */
static void case_dispatch_256_224_idle(void)
{
    struct sched s;
    int i, queued = 0;
    uint32_t written = 0;

    sched_init_n(&s, 256);
    for (i = 0; i < 256; i++)
    {
        s.full[i] = i < 32;
        if (push(&s, (uint16_t)i, false, false, 0, 700 + i) == 0)
            queued++;
    }
    ok_u32((uint32_t)queued, 256, "one record queued per share");

    pump_zero_delay(&s, 16);
    ok_u32((uint32_t)s.flush_pass, 256, "all 256 records written");
    ok_u32((uint32_t)s.now_ms, 0, "healthy carriers sent with zero delay");
    ok_u32(s.hol_wait, 0, "no record waited behind a full socket");
    ok_u32((uint32_t)s.flush_skip, 0, "no carrier-side skip was charged");
    ok_u32(s.outq_queued, 0, "the session FIFO is empty");
    ok(s.drain_next_ms < 0, "the drain goes idle", NULL);
    for (i = 0; i < 32; i++)
        ok_u32(s.writes[i], 0, "full carrier took no record");
    for (i = 0; i < 256; i++)
        written += s.writes[i];
    ok_u32(written, 256, "every record reached a carrier exactly once");
    sched_free(&s);
}

/* Every socket full: the record stays at the session FIFO head and nothing
 * is written until the 2 ms retry. Releasing one socket lets the head out;
 * the rest stay queued. */
static void case_all_full_retry(void)
{
    struct sched s;
    int i, queued = 0;

    sched_init_n(&s, 256);
    for (i = 0; i < 256; i++)
    {
        s.full[i] = true;
        if (push(&s, (uint16_t)i, false, false, 0, 800 + i) == 0)
            queued++;
    }
    ok_u32((uint32_t)queued, 256, "the full carriers keep their records");
    pump_zero_delay(&s, 16);
    ok_u32((uint32_t)s.flush_pass, 0, "no record written while all are full");
    ok_u32(s.outq_queued, 256, "the FIFO keeps every record");
    ok_u32((uint32_t)s.now_ms, 0, "the scan paid no wait");
    ok_u32((uint32_t)s.drain_next_ms, DRAIN_RETRY_MS,
           "the drain sleeps on the 2 ms timer");

    s.full[7] = false;
    pump_drain(&s, 8);
    ok_u32((uint32_t)s.now_ms, DRAIN_RETRY_MS,
           "the retry took exactly one tick");
    ok_u32((uint32_t)s.flush_pass, 256, "the released carrier drains them all");
    ok_u32(s.writes[7], 256, "every record used the released carrier");
    ok_u32(s.outq_queued, 0, "the FIFO is empty");
    sched_free(&s);
}

/* A carrier that refuses for TBT_SKIP_MS is slow: later scans try it last,
 * and room again clears the mark and rejoins the normal rotation. */
static void case_slow_skip_and_recover(void)
{
    struct sched s;
    int i;

    sched_init_n(&s, 8);
    for (i = 0; i < 8; i++)
        s.full[i] = true;
    push(&s, 0, false, false, 0, 1);
    pump_drain(&s, 20);
    ok(s.now_ms >= SKIP_MS, "the refused elapsed past the slow bound", NULL);
    ok(s.slow[0], "carrier 0 is marked slow", NULL);
    ok_u32((uint32_t)s.flush_pass, 0, "a slow socket took nothing");

    s.full[3] = false;
    pump_drain(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 1, "the released carrier took the head");
    ok_u32(s.writes[3], 1, "the record used the healthy carrier");

    /* Carrier 0 recovers: the next dispatch must clear slow and use it,
     * starting after the cursor that carrier 3 advanced. */
    s.full[0] = false;
    s.full[3] = true;
    s.rot = 4;
    push(&s, 0, false, false, 0, 2);
    pump_drain(&s, 4);
    ok(!s.slow[0], "room clears the slow mark", NULL);
    ok_u32((uint32_t)s.flush_pass, 2, "the recovered carrier rejoined");
    sched_free(&s);
}

/* A congested DATA FIFO must not hold an ACK/NACK: control rides any up
 * carrier from its own list. */
static void case_control_not_blocked(void)
{
    struct sched s;
    uint32_t wrote;
    int i;

    sched_init(&s);
    for (i = 0; i < NCAR; i++)
        s.full[i] = true;
    push(&s, 0, false, false, 0, 1);
    push(&s, 0, true, false, 0, 2);
    s.full[1] = false;

    ok(flush(&s, &wrote) == 0, "session flush ignores the DATA FIFO", NULL);
    ok_u32(wrote, 1, "control record written");
    ok(s.ctl == NULL, "control queue empty", NULL);
    ok_u32(s.outq_queued, 1, "the DATA head still waits");
    ok_u32(s.writes[1], 1, "control used the released carrier");
    ok_u32(s.ctl_depth, 0, "control depth released");
    ok_u32(s.ctl_bytes, 0, "control bytes released");
    sched_free(&s);
}

/* The session record cap bounds the FIFO regardless of carrier count; the
 * packetizer pauses at the high-water mark before it reaches the cap. */
static void case_session_ceiling(void)
{
    struct sched s;
    int n = 0, cap;

    sched_init(&s);
    cap = (int)QUEUE_MAX;
    while (s.outq_queued < (uint32_t)cap)
    {
        int err = push(&s, (uint16_t)(n % NCAR), false, false, 0, n);

        if (err)
            break;
        n++;
        ok(n <= cap, "the session cap bounds the FIFO", NULL);
    }
    ok_u32(s.outq_queued, (uint32_t)cap, "the session cap is the bound");
    ok(push(&s, 0, false, false, 0, 999) == -ENOSPC,
       "the record past the session cap is refused", NULL);
    ok_u32((uint32_t)s.outq_drop, 1, "the refusal is counted");
    ok(packetizer_gate(&s), "the session cap pauses the packetizer", NULL);
    sched_free(&s);
}

/* A repair backlog at its high-water mark stops new DATA: the receiver is
 * missing shares, and more DATA only competes with the repair. */
static void case_replay_backpressure(void)
{
    struct sched s;
    int i;

    sched_init(&s);
    ok(!packetizer_gate(&s), "an empty replay queue lets the packetizer run",
       NULL);
    for (i = 0; i < (int)(REPLAY_HIGH / REC_LEN) - 1; i++)
        ok(push(&s, 0, false, true, 0, i) == 0, "queue a repair", NULL);
    ok(!packetizer_gate(&s), "below the mark the packetizer still runs", NULL);
    ok(push(&s, 0, false, true, 0, 900) == 0,
       "the record at the mark is queued", NULL);
    ok(packetizer_gate(&s), "at the mark the packetizer pauses", NULL);
    ok_u32((uint32_t)s.outq_high_wait, 1, "one pause counted");
    ok_u32((uint32_t)s.outq_drop, 0, "no DATA record was dropped");

    while (s.replay_head)
    {
        struct rec *r = s.replay_head;

        s.replay_head = r->next;
        if (!s.replay_head)
            s.replay_tail = NULL;
        rec_free(&s, r);
    }
    ok(!packetizer_gate(&s), "an empty repair queue resumes the packetizer",
       NULL);
    ok(!s.outq_paused, "the pause episode ended", NULL);
    sched_free(&s);
}

/* Replay records spread by bytes written in the pass; the session byte
 * budget rejects the excess. */
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
 * one instead of waiting behind a data head. */
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

/* Replay drains before the DATA FIFO, and a full replay budget does not stop
 * data from queueing or writing. */
static void case_replay_priority_and_isolation(void)
{
    struct sched s;
    uint32_t wrote;
    int i;

    sched_init(&s);
    for (i = 0; i < (int)REPLAY_BYTES_MAX / (int)REC_LEN; i++)
        push(&s, 0, false, true, 0, 100 + i);
    push(&s, 3, false, false, 0, 1);
    ok(flush(&s, &wrote) == 0, "replay drains first", NULL);
    ok_u32(wrote, REPLAY_BYTES_MAX / REC_LEN, "replay written");
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, REPLAY_BYTES_MAX / REC_LEN + 1,
           "data drains after replay");
    ok(s.write_seq[0] >= 100, "a replay was written first", NULL);
    ok(s.write_seq[s.nwrite - 1] == 1, "data was written after replay", NULL);

    /* The budget is full again: data must still queue and flush. */
    for (i = 0; i < (int)REPLAY_BYTES_MAX / (int)REC_LEN; i++)
        push(&s, 0, false, true, 0, 200 + i);
    ok(push(&s, 4, false, false, 0, 2) == 0,
       "data accepted while the replay budget is full", NULL);
    ok(flush(&s, &wrote) == 0, "replay drains while data waits", NULL);
    ok_u32(wrote, REPLAY_BYTES_MAX / REC_LEN, "replay written");
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass,
           REPLAY_BYTES_MAX / REC_LEN + 1 + REPLAY_BYTES_MAX / REC_LEN + 1,
           "data written after the replay burst");
    sched_free(&s);
}

/* A share may not ride a carrier whose OPEN for this stream is not on the
 * wire yet: it waits in the session FIFO, and an announced carrier carries
 * it as soon as one is up. */
static void case_open_gate(void)
{
    struct sched s;
    int i;

    sched_init(&s);
    for (i = 0; i < NCAR; i++)
        s.up[i] = false;
    push(&s, 0, false, false, 0, 1);
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 0, "DATA waits while no OPEN is on the wire");
    ok_u32(s.outq_queued, 1, "the share stays in the FIFO");
    ok_u32(s.writes[0], 0, "the unannounced carrier wrote nothing");

    s.up[0] = true;
    kick(&s);
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 1, "the share goes after the OPEN");
    ok_u32(s.outq_queued, 0, "the share left the FIFO");
    ok_u32(s.writes[0], 1, "the announced carrier wrote it");

    /* A different carrier that already has its OPEN is a valid carrier. */
    s.up[0] = false;
    s.up[1] = true;
    s.rot = 0;
    push(&s, 0, false, false, 0, 2);
    kick(&s);
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 2, "an announced carrier carries the share");
    ok_u32(s.writes[1], 1, "the announced carrier wrote it");
    sched_free(&s);
}

/* A stream switch retires the old shape's DATA records by bumping the
 * epoch; the drain drops them without a write and the new stream drains. */
static void case_stream_switch_cleanup(void)
{
    struct sched s;
    uint32_t wrote;

    sched_init(&s);
    push(&s, 0, false, false, 0, 1); /* old data */
    push(&s, 0, false, true, 0, 2);  /* old replay */
    push(&s, 0, true, false, 0, 3);  /* old control */
    ok_u32(s.outq_queued, 1, "switch: old data queued");

    /* tbt_fec_tx_clear_data_queues(): bump the epoch, free replay, keep
     * control. */
    s.epoch++;
    while (s.replay_head)
    {
        struct rec *r = s.replay_head;

        s.replay_head = r->next;
        if (!s.replay_head)
            s.replay_tail = NULL;
        rec_free(&s, r);
    }
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 0, "switch: the old record was dropped");
    ok_u32(s.outq_queued, 0, "switch: the old DATA queue is clear");
    ok(flush(&s, &wrote) == 0 && wrote == 1,
       "switch: control is not part of the shape", NULL);

    push(&s, 0, false, false, 0, 10);
    pump_zero_delay(&s, 4);
    ok_u32((uint32_t)s.flush_pass, 2, "switch: the new stream drains");
    ok_u32((uint32_t)s.write_seq[s.nwrite - 1], 10,
           "switch: the new record reached the carrier");
    sched_free(&s);
}

/* The replay ring is indexed by block/group/share, not by carrier: moving a
 * replay to another carrier must not change the record it carries. */
static void case_replay_identity(void)
{
    struct sched s;
    struct rec *r;

    sched_init(&s);
    push(&s, 0, false, true, 0, 77);
    r = s.replay_head;
    ok(r && r->id == 77, "the share identity survives the carrier choice",
       NULL);
    ok_u32(r->carrier, 0, "replay has no bound carrier");
    sched_free(&s);
}

/* Releasing a replay record must not touch the DATA outq counters. The
 * kernel free path used to decrement them for replay objects, which
 * underflowed its signed counters; the unsigned high-water test then read
 * the negative value as a huge positive and blocked the packetizer (field:
 * 0 ms 64-carrier run stalled at block 6 with outq=-2/-16480). */
static void case_replay_free_keeps_outq(void)
{
    struct sched s;
    struct rec *r;

    sched_init(&s);
    push(&s, 0, false, false, 0, 1); /* one DATA record */
    push(&s, 0, false, true, 0, 2);  /* one replay record */
    ok_u32(s.outq_queued, 1, "replay free: one DATA record queued");
    ok_u32(s.outq_bytes, REC_LEN, "replay free: DATA bytes queued");

    r = s.replay_head;
    s.replay_head = r->next;
    if (!s.replay_head)
        s.replay_tail = NULL;
    rec_free(&s, r);
    ok_u32(s.outq_queued, 1, "replay free: DATA queue unchanged");
    ok_u32(s.outq_bytes, REC_LEN, "replay free: DATA bytes unchanged");
    ok_u32(s.replay_queued, 0, "replay free: replay queue empty");
    ok_u32(s.replay_bytes, 0, "replay free: replay bytes empty");
    ok(!outq_high(&s), "replay free: the packetizer is not paused", NULL);
    sched_free(&s);
}

int main(void)
{
    case_dispatch_256_224_idle();
    case_all_full_retry();
    case_slow_skip_and_recover();
    case_control_not_blocked();
    case_session_ceiling();
    case_replay_backpressure();
    case_replay_spread_and_budget();
    case_replay_fallback();
    case_replay_priority_and_isolation();
    case_replay_identity();
    case_replay_free_keeps_outq();
    case_open_gate();
    case_stream_switch_cleanup();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
