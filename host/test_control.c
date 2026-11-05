/* test_control.c - SPEC test for review 22's pinned control-frame lifetime.
 *
 * WHAT THIS IS
 *   A pure host model of the intended tbt_tx.c control-frame behaviour:
 *
 *     queued -> selected -> write_attempt
 *            -> written | eagain_requeued | enomem_requeued | path_dead_requeued
 *            -> failed (explicit, after a bounded wait)
 *
 *   It models HELLO/OPEN/RETIRE/FIN/RST as pinned frames and asserts:
 *     - a transient -EAGAIN/-ENOMEM requeues the frame, never frees it;
 *     - a carrier generation change re-pins a queued FIN/RST to the new gen,
 *       while HELLO/OPEN/RETIRE of the old gen are obsolete and are dropped;
 *     - at most one FIN/RST per (stream,index) is in flight;
 *     - a second tbt_tx_fin()/tbt_tx_rst() for the same stream is a no-op
 *       (tx_fin_sent/tx_rst_sent);
 *     - the control queue cap is respected and a FIN/RST is never silently
 *       dropped: the producer gets -ENOSPC and retries after the pump drains;
 *     - the 2 ms w_pump is the bounded retry for a requeued frame;
 *     - a missing carrier leads to an explicit FAILED after a bounded number
 *       of pump steps, not an infinite silent wait.
 *
 *   It does NOT link or execute tbt_tx.c. This is the executable
 *   specification for the review-22 patch, not a test of the current kernel.
 *   Keep the model and the kernel in lockstep when the patch lands.
 *
 * MAPPING TO tbt_tx.c (current anchors; re-grep before patching)
 *   model enqueue/ctl_frame       -> tbt_tx_control_pinned()      (tbt_tx.c:169)
 *   model fin_request             -> tbt_tx_fin()                 (:233)
 *   model rst_request             -> tbt_tx_rst()                 (:258)
 *   model pump_step pinned branch -> tbt_tx_pump()                (:1556)
 *   model EAGAIN/ENOMEM           -> tbt_tx_pump()                (:1749-1770)
 *                                    target: requeue pinned, no tbt_frame_free
 *   model gen re-pin (H5)         -> tbt_tx_pump()                (:1576-1606)
 *   model drop->retry (H6)        -> tbt_tx_pump()                (:1698-1717)
 *   model batch !c (H7)           -> tbt_tx_pump()                (:1719-1730)
 *   model dead/-EIO (H9)          -> tbt_tx_pump()                (:1772-1792)
 *   model carrier_replace/re-pin  -> tbt_carrier_lost()/tbt_replace_work()/
 *                                    tbt_session_join()
 *   model arm_pump/tick           -> mod_delayed_work(&c->w_pump, 2ms) (:1662)
 *   model FAILED after retries    -> target: explicit ctl_lost counter / kill;
 *                                    no current kernel equivalent
 *   model fin_wanted/fin_queued   -> target M2a fields in struct tbt_stream
 *                                    (tbtun.h:294), driven by tbt_sock.c:115-134
 *                                    and the 10 ms tbt_fbk_work() (:2254-2294)
 *   model tx_fin_sent/tx_rst_sent -> per-index de-duplication mask; target
 *                                    tx_fin_mask/tx_rst_mask (M2b, optional)
 *   tbt_tx_stall_check()          -> not modeled: FIN/RST do not change
 *                                    tx_seq/tx_ack; if data is unacked the
 *                                    hole budget owns the terminal outcome.
 *
 * M1 vs M2a (see review/analysis/p1-misc-patch-plan.md §1.6-1.8)
 *   M1  = pump keeps an already-queued pinned frame across -EAGAIN/-ENOMEM/
 *         -EIO/gen replacement/batch carrier loss. Cases fin_eagain,
 *         rst_enomem, fin_path_dead_repin, fin_eio, fin_batch_carrier_gone.
 *   M2a = tbt_tx_fin() returns the number of copies queued; the producer
 *         keeps fin_wanted until fin_queued, and the FBK tick retries.
 *         Cases fin_partial_enqueue, fin_zero_queued.
 *   M2b = per-index masks; only fin_dedup / fin_written_then_dead rely on it.
 *
 *   make host-test
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_CAR 4
#define CTL_QUEUE_MAX 32
#define CTL_MAX_RETRIES 4
#define MAX_WRITE_RECS 256

enum ctl_type
{
    CTL_HELLO,
    CTL_OPEN,
    CTL_RETIRE,
    CTL_FIN,
    CTL_RST,
};

enum ctl_state
{
    CTL_QUEUED,
    CTL_SELECTED,
    CTL_WRITE_ATTEMPT,
    CTL_WRITTEN,
    CTL_EAGAIN_REQUEUED,
    CTL_ENOMEM_REQUEUED,
    CTL_PATH_DEAD_REQUEUED,
    CTL_FAILED,
};

struct ctl_frame
{
    bool used;
    enum ctl_type type;
    enum ctl_state state;
    uint64_t sid;
    uint8_t index;
    uint32_t gen;
    int retries;
};

struct carrier
{
    bool present;
    bool connected;
    bool hello_sent;
    bool open_sent;
    bool up;
    bool stopping;
    bool full;   /* tcp_sendmsg_locked would return -EAGAIN */
    bool enomem; /* transient allocation failure */
    bool eio;    /* tbt_carrier_xmit() returned -EIO: dead path, keep frame */
    bool enqueue_enomem; /* tbt_tx_control_pinned() kmalloc failed (-ENOMEM) */
    uint32_t gen;
};

struct write_rec
{
    bool used;
    enum ctl_type type;
    uint64_t sid;
    uint8_t index;
    uint32_t gen;
    int count;
};

struct model
{
    struct ctl_frame q[CTL_QUEUE_MAX];
    int nq;
    struct carrier car[MAX_CAR];
    int ncar;
    uint64_t now_ms;
    bool pump_armed;
    uint64_t pump_at_ms;

    struct write_rec writes[MAX_WRITE_RECS];
    int nwrites;

    int duplicates;
    int failures;
    int obsolete_drops;
    int eagain_requeues;
    int enomem_requeues;
    int path_dead_requeues;
    int writes_hello;
    int writes_open;
    int writes_retire;
    int writes_fin;
    int writes_rst;
    int kill_calls;  /* tbt_stream_kill() from the bounded-failure path */
    int ctl_lost;    /* control frames abandoned after CTL_MAX_RETRIES */
    int last_queued; /* copies queued by the last fin_request_m2a() */

    /* One FIN/RST set per stream: a second tbt_tx_fin()/tbt_tx_rst() is a
     * no-op. The per-index flags make the per-(stream,index) rule explicit. */
    bool fin_sent[MAX_CAR];
    bool rst_sent[MAX_CAR];
    uint64_t fin_sid;
    uint64_t rst_sid;
    /* M2a (tbt_sock.c:115-134): wanted is sticky until a copy exists; queued
     * says at least one copy is on the queue or has been written. */
    bool fin_wanted;
    bool fin_queued;
    /* H7 hook: make the carrier disappear inside the batch window. */
    int batch_gone_index;
};

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

static void model_init(struct model *m, int ncar)
{
    int i;

    memset(m, 0, sizeof(*m));
    m->batch_gone_index = -1;
    if (ncar > MAX_CAR)
        ncar = MAX_CAR;
    m->ncar = ncar;
    for (i = 0; i < ncar; i++)
    {
        m->car[i].present = true;
        m->car[i].connected = true;
        m->car[i].hello_sent = true;
        m->car[i].open_sent = true;
        m->car[i].up = true;
        m->car[i].gen = 1;
    }
}

/* Replace the carrier at `index` with a new generation: HELLO/OPEN/up reset,
 * exactly as a new struct tbt_carrier would start. Queued FIN/RST must re-pin;
 * queued HELLO/OPEN/RETIRE of the old gen are obsolete. */
static void carrier_replace(struct model *m, int index)
{
    struct carrier *c = &m->car[index];

    c->present = true;
    c->connected = true;
    c->hello_sent = false;
    c->open_sent = false;
    c->up = false;
    c->stopping = false;
    c->full = false;
    c->enomem = false;
    c->eio = false;
    c->enqueue_enomem = false;
    c->gen++;
}

static int enqueue_raw(struct model *m, enum ctl_type type, uint64_t sid,
                       uint8_t index, uint32_t gen)
{
    struct ctl_frame *fr;

    if (m->nq >= CTL_QUEUE_MAX)
        return -1;
    fr = &m->q[m->nq++];
    fr->used = true;
    fr->type = type;
    fr->state = CTL_QUEUED;
    fr->sid = sid;
    fr->index = index;
    fr->gen = gen;
    fr->retries = 0;
    return 0;
}

static bool has_frame(const struct model *m, enum ctl_type type, uint64_t sid,
                      uint8_t index, uint32_t gen)
{
    int i;

    for (i = 0; i < m->nq; i++)
    {
        const struct ctl_frame *fr = &m->q[i];

        if (!fr->used || fr->type != type || fr->index != index ||
            fr->gen != gen)
            continue;
        if (type == CTL_FIN || type == CTL_RST)
        {
            if (fr->sid != sid)
                continue;
        }
        return true;
    }
    return false;
}

static void arm_pump(struct model *m)
{
    if (!m->pump_armed || m->now_ms + 2 < m->pump_at_ms)
    {
        m->pump_armed = true;
        m->pump_at_ms = m->now_ms + 2;
    }
}

static void compact(struct model *m)
{
    int i, j = 0;

    for (i = 0; i < m->nq; i++)
    {
        if (!m->q[i].used)
            continue;
        if (i != j)
            m->q[j] = m->q[i];
        j++;
    }
    m->nq = j;
}

static void record_write(struct model *m, const struct ctl_frame *fr)
{
    struct write_rec *rec = NULL;
    int i;

    for (i = 0; i < m->nwrites; i++)
    {
        rec = &m->writes[i];
        if (rec->used && rec->type == fr->type && rec->sid == fr->sid &&
            rec->index == fr->index && rec->gen == fr->gen)
            break;
        rec = NULL;
    }
    if (!rec)
    {
        if (m->nwrites >= MAX_WRITE_RECS)
        {
            m->failures++;
            return;
        }
        rec = &m->writes[m->nwrites++];
        rec->used = true;
        rec->type = fr->type;
        rec->sid = fr->sid;
        rec->index = fr->index;
        rec->gen = fr->gen;
        rec->count = 0;
    }
    rec->count++;
    if (rec->count > 1)
        m->duplicates++;

    switch (fr->type)
    {
    case CTL_HELLO:
        m->writes_hello++;
        break;
    case CTL_OPEN:
        m->writes_open++;
        break;
    case CTL_RETIRE:
        m->writes_retire++;
        break;
    case CTL_FIN:
        m->writes_fin++;
        break;
    case CTL_RST:
        m->writes_rst++;
        break;
    }
}

/* Abandon a FIN/RST after CTL_MAX_RETRIES: explicit loss accounting and a
 * kill, never a silent free (tbt_tx.c H6b/H8/H9). */
static void model_fail(struct model *m, struct ctl_frame *fr)
{
    fr->state = CTL_FAILED;
    fr->used = false;
    m->failures++;
    m->ctl_lost++;
    m->kill_calls++;
    if (fr->type == CTL_FIN)
        m->fin_queued = false;
}

/* A RETIRE names its victim (index, gen) in the body; the carrier that sends
 * the notice is incidental. Pick a live one after a replacement
 * (tbt_carrier.c:840-852, tbt_tx.c:1587-1588). */
static int retire_repath(struct model *m, const struct ctl_frame *fr)
{
    int i;

    for (i = 0; i < m->ncar; i++)
    {
        if (i == fr->index)
            continue;
        if (!m->car[i].present || m->car[i].stopping || !m->car[i].up)
            continue;
        return i;
    }
    return -1;
}

/* One pump pass. A frame is processed once per pass; a requeued frame stays
 * in the queue and is retried by the next pass (the 2 ms w_pump or any other
 * wake). Called with no locks: the host model is single-threaded. */
static void model_pump(struct model *m)
{
    bool requeued = false;
    int i;

    for (i = 0; i < m->nq; i++)
    {
        struct ctl_frame *fr = &m->q[i];
        struct carrier *c;

        if (!fr->used)
            continue;
        fr->state = CTL_SELECTED;
        c = &m->car[fr->index];

        if (!c->present || c->stopping)
        {
            if (fr->type == CTL_FIN || fr->type == CTL_RST)
            {
                fr->retries++;
                if (fr->retries > CTL_MAX_RETRIES)
                    model_fail(m, fr);
                else
                {
                    fr->state = CTL_PATH_DEAD_REQUEUED;
                    m->path_dead_requeues++;
                    requeued = true;
                }
            }
            else
            {
                fr->state = CTL_FAILED;
                fr->used = false;
                m->obsolete_drops++;
            }
            continue;
        }

        if (fr->type == CTL_FIN || fr->type == CTL_RST)
        {
            if (fr->gen != c->gen)
            {
                /* Stream-level control: re-pin to the replacement. */
                fr->gen = c->gen;
                fr->state = CTL_PATH_DEAD_REQUEUED;
                m->path_dead_requeues++;
                requeued = true;
                continue;
            }
            if (!c->up)
            {
                /* The replacement has not written its OPEN yet. */
                fr->state = CTL_QUEUED;
                continue;
            }
        }
        else if (fr->type == CTL_HELLO)
        {
            if (fr->gen != c->gen || c->hello_sent)
            {
                fr->state = CTL_FAILED;
                fr->used = false;
                m->obsolete_drops++;
                continue;
            }
            if (!c->connected)
            {
                fr->state = CTL_QUEUED;
                continue;
            }
        }
        else if (fr->type == CTL_OPEN)
        {
            if (fr->gen != c->gen || !c->hello_sent || c->open_sent)
            {
                fr->state = CTL_FAILED;
                fr->used = false;
                m->obsolete_drops++;
                continue;
            }
        }
        else /* CTL_RETIRE */
        {
            if (fr->gen != c->gen || !c->up)
            {
                int alt = retire_repath(m, fr);

                if (alt >= 0)
                {
                    fr->index = (uint8_t)alt;
                    fr->gen = m->car[alt].gen;
                    fr->state = CTL_PATH_DEAD_REQUEUED;
                    m->path_dead_requeues++;
                    requeued = true;
                    continue;
                }
                if (fr->gen != c->gen)
                {
                    fr->state = CTL_FAILED;
                    fr->used = false;
                    m->obsolete_drops++;
                    continue;
                }
                fr->state = CTL_QUEUED;
                continue;
            }
        }

        fr->state = CTL_WRITE_ATTEMPT;
        if (m->batch_gone_index == fr->index)
        {
            /* H7: the batch loop's carrier vanished after selection. */
            m->batch_gone_index = -1;
            m->car[fr->index].present = false;
            fr->retries++;
            if (fr->retries > CTL_MAX_RETRIES)
                model_fail(m, fr);
            else
            {
                fr->state = CTL_PATH_DEAD_REQUEUED;
                m->path_dead_requeues++;
                requeued = true;
            }
            continue;
        }
        if (c->full || c->enomem)
        {
            /* H8: requeue pinned, never free. The bound turns a permanent
             * -EAGAIN into an explicit ctl_lost + kill. */
            fr->retries++;
            if (fr->retries > CTL_MAX_RETRIES)
            {
                model_fail(m, fr);
                continue;
            }
            fr->state = c->full ? CTL_EAGAIN_REQUEUED : CTL_ENOMEM_REQUEUED;
            if (c->full)
                m->eagain_requeues++;
            else
                m->enomem_requeues++;
            requeued = true;
            continue;
        }
        if (c->eio)
        {
            /* H9: -EIO may have written a prefix; the carrier dies, but the
             * stream-level FIN/RST is still owed to the peer. */
            c->stopping = true;
            fr->retries++;
            if (fr->retries > CTL_MAX_RETRIES)
                model_fail(m, fr);
            else
            {
                fr->state = CTL_PATH_DEAD_REQUEUED;
                m->path_dead_requeues++;
                requeued = true;
            }
            continue;
        }

        fr->state = CTL_WRITTEN;
        record_write(m, fr);
        if (fr->type == CTL_HELLO)
            c->hello_sent = true;
        else if (fr->type == CTL_OPEN)
        {
            c->open_sent = true;
            c->up = true;
        }
        fr->used = false;
    }

    compact(m);
    if (requeued)
        arm_pump(m);
}

static void model_tick(struct model *m, uint64_t ms)
{
    m->now_ms += ms;
    if (m->pump_armed && m->now_ms >= m->pump_at_ms)
    {
        m->pump_armed = false;
        model_pump(m);
    }
}

/* Legacy producer: unconditional per-index enqueue, matching the current
 * tbt_tx_fin() (tbt_tx.c:233-254) and the existing 13 cases. A second call
 * is a no-op once a FIN set exists. */
static int fin_request(struct model *m, uint64_t sid)
{
    int i, rc = 0, queued = 0;

    if (m->fin_wanted)
        return 0;
    if (m->fin_sid && m->fin_sid != sid)
        return -1;
    m->fin_sid = sid;
    m->fin_wanted = true;
    for (i = 0; i < m->ncar; i++)
    {
        if (m->fin_sent[i])
            continue;
        if (enqueue_raw(m, CTL_FIN, sid, (uint8_t)i, m->car[i].gen))
        {
            rc = -1;
            break;
        }
        m->fin_sent[i] = true;
        queued++;
    }
    m->last_queued = queued;
    if (queued)
        m->fin_queued = true;
    if (rc)
    {
        /* Queue cap: leave the producer free to retry after the pump drains
         * (case_queue_cap relies on this). */
        m->fin_wanted = false;
        m->fin_sid = 0;
    }
    return rc;
}

/* M2a producer: only a carrier that can actually take the copy is counted.
 * -ENOMEM on one carrier does not break the loop; zero copies leaves
 * fin_wanted set so the FBK tick retries (tbt_sock.c:115-134,
 * tbt_tx.c:2254-2294). */
static int fin_request_m2a(struct model *m, uint64_t sid)
{
    int i, rc = 0, queued = 0;

    if (m->fin_queued)
        return 0;
    if (m->fin_sid && m->fin_sid != sid)
        return -1;
    m->fin_sid = sid;
    m->fin_wanted = true;
    for (i = 0; i < m->ncar; i++)
    {
        if (m->fin_sent[i])
            continue;
        if (!m->car[i].present || !m->car[i].up)
            continue; /* no carrier can take this copy yet */
        if (m->car[i].enqueue_enomem)
            continue; /* tbt_tx_control_pinned() -ENOMEM: next carrier */
        if (enqueue_raw(m, CTL_FIN, sid, (uint8_t)i, m->car[i].gen))
        {
            rc = -1;
            break;
        }
        m->fin_sent[i] = true;
        queued++;
    }
    m->last_queued = queued;
    if (queued)
        m->fin_queued = true;
    return rc;
}

/* The 10 ms FBK tick (tbt_fbk_work): retry a FIN that never got a copy. */
static void fin_retry_tick(struct model *m)
{
    if (m->fin_wanted && !m->fin_queued)
        (void)fin_request_m2a(m, m->fin_sid);
}

static int rst_request(struct model *m, uint64_t sid)
{
    int i, rc = 0;

    if (m->rst_sid && m->rst_sid != sid)
        return -1;
    m->rst_sid = sid;
    for (i = 0; i < m->ncar; i++)
    {
        if (m->rst_sent[i])
            continue;
        if (enqueue_raw(m, CTL_RST, sid, (uint8_t)i, m->car[i].gen))
        {
            rc = -1;
            break;
        }
        m->rst_sent[i] = true;
    }
    return rc;
}

static int hello_request(struct model *m, int index)
{
    struct carrier *c = &m->car[index];

    if (!c->present || c->hello_sent)
        return 0;
    if (has_frame(m, CTL_HELLO, 0, (uint8_t)index, c->gen))
        return 0;
    return enqueue_raw(m, CTL_HELLO, 0, (uint8_t)index, c->gen);
}

static int open_request(struct model *m, int index, uint64_t sid)
{
    struct carrier *c = &m->car[index];

    if (!c->present || !c->hello_sent || c->open_sent)
        return 0;
    if (has_frame(m, CTL_OPEN, sid, (uint8_t)index, c->gen))
        return 0;
    return enqueue_raw(m, CTL_OPEN, sid, (uint8_t)index, c->gen);
}

static int retire_request(struct model *m, int index, uint64_t sid)
{
    struct carrier *c = &m->car[index];

    if (!c->present || !c->up)
        return 0;
    if (has_frame(m, CTL_RETIRE, sid, (uint8_t)index, c->gen))
        return 0;
    return enqueue_raw(m, CTL_RETIRE, sid, (uint8_t)index, c->gen);
}

/* Invariants after every case:
 *   - queue cap respected;
 *   - no (type,sid,index,gen) written twice;
 *   - at most one queued FIN/RST per (sid,type,index);
 *   - a FIN/RST that was queued is either written or explicitly FAILED. */
static void check_invariants(const struct model *m, const char *what)
{
    int i, j;

    ok(m->nq <= CTL_QUEUE_MAX, what);
    ok(m->duplicates == 0, what);
    for (i = 0; i < m->nq; i++)
    {
        const struct ctl_frame *a = &m->q[i];

        if (!a->used || (a->type != CTL_FIN && a->type != CTL_RST))
            continue;
        for (j = i + 1; j < m->nq; j++)
        {
            const struct ctl_frame *b = &m->q[j];

            if (!b->used || b->type != a->type || b->sid != a->sid ||
                b->index != a->index)
                continue;
            ok(false, what);
        }
    }
    ok(m->failures >= 0, what);
}

/* --- cases ------------------------------------------------------------- */

static void case_fin_eagain(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin eagain: queue");
    m.car[0].full = true;
    model_pump(&m);
    ok(m.q[0].state == CTL_EAGAIN_REQUEUED, "fin eagain: requeued");
    ok(m.nq == 1 && m.writes_fin == 0, "fin eagain: not freed/written");
    ok(m.eagain_requeues == 1 && m.pump_armed, "fin eagain: w_pump armed");
    m.car[0].full = false;
    model_tick(&m, 1);
    ok(m.writes_fin == 0, "fin eagain: 1 ms is early");
    model_tick(&m, 1);
    ok(m.writes_fin == 1 && m.nq == 0, "fin eagain: written after 2 ms");
    check_invariants(&m, "fin eagain");
}

static void case_rst_enomem(void)
{
    struct model m;

    model_init(&m, 1);
    ok(rst_request(&m, 0x1234) == 0, "rst enomem: queue");
    m.car[0].enomem = true;
    model_pump(&m);
    ok(m.q[0].state == CTL_ENOMEM_REQUEUED, "rst enomem: requeued");
    ok(m.nq == 1 && m.writes_rst == 0, "rst enomem: not freed/written");
    m.car[0].enomem = false;
    model_tick(&m, 2);
    ok(m.writes_rst == 1 && m.nq == 0, "rst enomem: written");
    check_invariants(&m, "rst enomem");
}

static void case_fin_path_dead_repin(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin repin: queue");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.q[0].state == CTL_PATH_DEAD_REQUEUED, "fin repin: requeued");
    ok(m.q[0].gen == m.car[0].gen && m.nq == 1, "fin repin: new gen");
    ok(m.writes_fin == 0 && m.path_dead_requeues == 1, "fin repin: not written");
    m.car[0].up = true;
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin repin: written on new gen");
    check_invariants(&m, "fin repin");
}

static void case_rst_path_dead_repin(void)
{
    struct model m;

    model_init(&m, 1);
    ok(rst_request(&m, 0x1234) == 0, "rst repin: queue");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.q[0].state == CTL_PATH_DEAD_REQUEUED, "rst repin: requeued");
    m.car[0].up = true;
    model_pump(&m);
    ok(m.writes_rst == 1 && m.nq == 0, "rst repin: written on new gen");
    check_invariants(&m, "rst repin");
}

static void case_fin_wait_no_carrier(void)
{
    struct model m;
    int i;

    model_init(&m, 1);
    m.car[0].present = false;
    ok(fin_request(&m, 0x1234) == 0, "fin no-car: queue");
    for (i = 0; i < CTL_MAX_RETRIES + 2; i++)
        model_pump(&m);
    ok(m.failures == 1, "fin no-car: explicit failure");
    ok(m.nq == 0 && m.writes_fin == 0, "fin no-car: removed without write");
    check_invariants(&m, "fin no-car");
}

static void case_fin_dedup(void)
{
    struct model m;

    model_init(&m, 2);
    ok(fin_request(&m, 0x1234) == 0, "fin dedup: first");
    ok(fin_request(&m, 0x1234) == 0, "fin dedup: second");
    ok(m.nq == 2, "fin dedup: one per carrier, not two sets");
    model_pump(&m);
    ok(m.writes_fin == 2 && m.nq == 0, "fin dedup: written once per carrier");
    ok(fin_request(&m, 0x1234) == 0, "fin dedup: after written");
    ok(m.nq == 0, "fin dedup: tx_fin_sent suppresses a new set");
    check_invariants(&m, "fin dedup");
}

static void case_fin_per_index(void)
{
    struct model m;

    model_init(&m, 3);
    ok(fin_request(&m, 0x1234) == 0, "fin per-index: queue");
    ok(m.nq == 3, "fin per-index: one per index");
    model_pump(&m);
    ok(m.writes_fin == 3 && m.nq == 0, "fin per-index: all written");
    check_invariants(&m, "fin per-index");
}

static void case_hello_eagain(void)
{
    struct model m;

    model_init(&m, 1);
    m.car[0].hello_sent = false;
    m.car[0].open_sent = false;
    m.car[0].up = false;
    ok(hello_request(&m, 0) == 0, "hello eagain: queue");
    m.car[0].full = true;
    model_pump(&m);
    ok(m.q[0].state == CTL_EAGAIN_REQUEUED, "hello eagain: requeued");
    ok(m.nq == 1 && m.writes_hello == 0, "hello eagain: not freed");
    m.car[0].full = false;
    model_tick(&m, 2);
    ok(m.writes_hello == 1 && m.car[0].hello_sent, "hello eagain: written");
    check_invariants(&m, "hello eagain");
}

static void case_open_old_gen_drop(void)
{
    struct model m;

    model_init(&m, 1);
    m.car[0].open_sent = false;
    m.car[0].up = false;
    ok(open_request(&m, 0, 0x1234) == 0, "open old-gen: queue");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.writes_open == 0 && m.nq == 0, "open old-gen: dropped");
    ok(m.obsolete_drops == 1, "open old-gen: counted obsolete");
    m.car[0].hello_sent = true;
    ok(open_request(&m, 0, 0x1234) == 0, "open new-gen: queue");
    model_pump(&m);
    ok(m.writes_open == 1 && m.car[0].up, "open new-gen: written");
    check_invariants(&m, "open old-gen");
}

static void case_retire_eagain(void)
{
    struct model m;

    model_init(&m, 1);
    ok(retire_request(&m, 0, 0x1234) == 0, "retire eagain: queue");
    m.car[0].full = true;
    model_pump(&m);
    ok(m.q[0].state == CTL_EAGAIN_REQUEUED, "retire eagain: requeued");
    m.car[0].full = false;
    model_tick(&m, 2);
    ok(m.writes_retire == 1 && m.nq == 0, "retire eagain: written");
    check_invariants(&m, "retire eagain");
}

static void case_queue_cap(void)
{
    struct model m;
    int i;

    model_init(&m, 1);
    for (i = 0; i < CTL_QUEUE_MAX; i++)
        ok(enqueue_raw(&m, CTL_RETIRE, 0x1000 + (uint64_t)i, 0, 1) == 0,
           "queue cap: fill");
    ok(enqueue_raw(&m, CTL_RETIRE, 0x9999, 0, 1) == -1, "queue cap: full");
    ok(fin_request(&m, 0x1234) == -1, "queue cap: FIN gets -ENOSPC");
    ok(m.nq == CTL_QUEUE_MAX && m.writes_fin == 0,
       "queue cap: FIN not silently dropped");
    model_pump(&m);
    ok(m.nq == 0 && m.writes_retire == CTL_QUEUE_MAX, "queue cap: drained");
    ok(fin_request(&m, 0x1234) == 0, "queue cap: FIN retried");
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "queue cap: FIN written");
    check_invariants(&m, "queue cap");
}

static void case_pump_2ms(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "pump 2ms: queue");
    m.car[0].full = true;
    model_pump(&m);
    ok(m.pump_armed && m.pump_at_ms == 2, "pump 2ms: armed at t+2");
    m.car[0].full = false;
    model_tick(&m, 1);
    ok(m.writes_fin == 0 && m.pump_armed, "pump 2ms: t+1 no run");
    model_tick(&m, 1);
    ok(m.writes_fin == 1 && !m.pump_armed, "pump 2ms: t+2 run");
    check_invariants(&m, "pump 2ms");
}

static void case_fin_written_then_dead(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin written/dead: queue");
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin written/dead: written");
    carrier_replace(&m, 0);
    ok(fin_request(&m, 0x1234) == 0, "fin written/dead: no second set");
    ok(m.nq == 0, "fin written/dead: written is sent");
    model_pump(&m);
    ok(m.writes_fin == 1 && m.duplicates == 0, "fin written/dead: no duplicate");
    check_invariants(&m, "fin written/dead");
}

/* tbt_tx.c H9 (:1772-1792): -EIO is a carrier failure, not a licence to free
 * a FIN. The frame stays queued, follows the heir, and is written once the
 * replacement has its OPEN (c->up). */
static void case_fin_eio_repin(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin eio: queue");
    m.car[0].eio = true;
    model_pump(&m);
    ok(m.q[0].state == CTL_PATH_DEAD_REQUEUED, "fin eio: kept, not freed");
    ok(m.writes_fin == 0 && m.nq == 1, "fin eio: not written yet");
    ok(m.car[0].stopping, "fin eio: carrier lost");
    ok(m.path_dead_requeues == 1 && m.ctl_lost == 0, "fin eio: bounded retry");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.q[0].gen == m.car[0].gen && m.writes_fin == 0, "fin eio: re-pinned");
    m.car[0].up = true;
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin eio: written on heir");
    ok(m.ctl_lost == 0 && m.kill_calls == 0, "fin eio: no loss");
    check_invariants(&m, "fin eio");
}

/* tbt_tx.c H7 (:1719-1730): the batch loop re-reads sess->car[fr->path]; a
 * carrier that vanished after selection must requeue a FIN/RST, not free it. */
static void case_fin_batch_carrier_gone(void)
{
    struct model m;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin batch-gone: queue");
    m.batch_gone_index = 0;
    model_pump(&m);
    ok(m.q[0].state == CTL_PATH_DEAD_REQUEUED, "fin batch-gone: kept");
    ok(m.writes_fin == 0 && m.nq == 1, "fin batch-gone: not freed");
    ok(m.path_dead_requeues == 1, "fin batch-gone: requeued once");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.q[0].gen == m.car[0].gen && m.writes_fin == 0,
       "fin batch-gone: re-pinned");
    m.car[0].up = true;
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin batch-gone: written on heir");
    check_invariants(&m, "fin batch-gone");
}

/* tbt_tx.c H6b/H8 (:1713-1717, :1749-1770): after CTL_MAX_RETRIES the frame
 * is abandoned explicitly: ctl_lost, kill, and no silent disappearance. */
static void case_fin_retry_exhausted(void)
{
    struct model m;
    int i;

    model_init(&m, 1);
    ok(fin_request(&m, 0x1234) == 0, "fin exhausted: queue");
    m.car[0].full = true;
    for (i = 0; i <= CTL_MAX_RETRIES; i++)
        model_pump(&m);
    ok(m.failures == 1, "fin exhausted: explicit failure");
    ok(m.ctl_lost == 1 && m.kill_calls == 1,
       "fin exhausted: counted and killed");
    ok(m.nq == 0 && m.writes_fin == 0, "fin exhausted: removed without write");
    ok(!m.fin_queued, "fin exhausted: queued flag cleared");
    check_invariants(&m, "fin exhausted");
}

/* tbt_tx.c H11 (:233-254): a per-carrier -ENOMEM must not break the loop;
 * the other carriers still get a copy. */
static void case_fin_partial_enqueue(void)
{
    struct model m;

    model_init(&m, 2);
    m.car[0].enqueue_enomem = true;
    ok(fin_request_m2a(&m, 0x1234) == 0, "fin partial: enqueue");
    ok(m.last_queued == 1, "fin partial: one copy, not zero");
    ok(m.nq == 1 && m.q[0].index == 1, "fin partial: second carrier used");
    ok(m.fin_wanted && m.fin_queued, "fin partial: wanted/queued");
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin partial: written");
    check_invariants(&m, "fin partial");
}

/* tbt_sock.c:115-134 + tbt_tx.c H13 (:2254-2294): no carrier up means the
 * producer gets zero copies; fin_wanted stays true and the 10 ms FBK tick
 * retries until a copy exists. */
static void case_fin_zero_queued(void)
{
    struct model m;

    model_init(&m, 1);
    m.car[0].up = false;
    ok(fin_request_m2a(&m, 0x1234) == 0, "fin zero: request");
    ok(m.last_queued == 0 && m.nq == 0, "fin zero: no copy");
    ok(m.fin_wanted && !m.fin_queued, "fin zero: wanted, not queued");
    fin_retry_tick(&m);
    ok(m.nq == 0 && !m.fin_queued, "fin zero: tick waits for up");
    m.car[0].up = true;
    fin_retry_tick(&m);
    ok(m.nq == 1 && m.fin_queued, "fin zero: tick queues after up");
    model_pump(&m);
    ok(m.writes_fin == 1 && m.nq == 0, "fin zero: written");
    check_invariants(&m, "fin zero");
}

/* tbt_carrier.c:840-852 + tbt_tx.c H5 (:1587-1588): RETIRE names a victim in
 * its body; the carrier that carries the notice is incidental. A replaced
 * sender must re-select a live path, not drop the notice (join_dup storm). */
static void case_retire_repath(void)
{
    struct model m;

    model_init(&m, 2);
    ok(retire_request(&m, 0, 0x1234) == 0, "retire repath: queue");
    carrier_replace(&m, 0);
    model_pump(&m);
    ok(m.q[0].state == CTL_PATH_DEAD_REQUEUED, "retire repath: requeued");
    ok(m.q[0].index == 1, "retire repath: live carrier selected");
    ok(m.writes_retire == 0 && m.obsolete_drops == 0,
       "retire repath: not dropped");
    model_pump(&m);
    ok(m.writes_retire == 1 && m.nq == 0, "retire repath: written");
    check_invariants(&m, "retire repath");
}

int main(void)
{
    case_fin_eagain();
    case_rst_enomem();
    case_fin_path_dead_repin();
    case_rst_path_dead_repin();
    case_fin_wait_no_carrier();
    case_fin_dedup();
    case_fin_per_index();
    case_hello_eagain();
    case_open_old_gen_drop();
    case_retire_eagain();
    case_queue_cap();
    case_pump_2ms();
    case_fin_written_then_dead();
    case_fin_eio_repin();
    case_fin_batch_carrier_gone();
    case_fin_retry_exhausted();
    case_fin_partial_enqueue();
    case_fin_zero_queued();
    case_retire_repath();

    printf("test_control: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
