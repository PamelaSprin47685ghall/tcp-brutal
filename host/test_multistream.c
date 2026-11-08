/* test_multistream.c - SPEC model for concurrent streams (P3b2/P3b3).
 *
 * Pure C. It does not link or execute the module. It models the state that
 * P3b2a got wrong:
 *
 *   - stream lifecycle: create -> OPEN -> data/ack -> retire -> tombstone;
 *   - per-stream announced matrix keyed by (carrier index, carrier gen);
 *   - the scheduler's need_open decision (carrier-level c->open_sent vs
 *     per-stream announced);
 *   - per-stream target attach/detach (exactly one target per stream);
 *   - per-stream tx_ack / SACK and retain path_end ownership;
 *   - carrier replacement at the same index with a new gen;
 *   - churn and 2/10/100 concurrent streams.
 *
 * The model deliberately has four modes so the P3b2a failure can be
 * reproduced and the P3b3 fix can be checked:
 *
 *   P2      : c->open_sent is the OPEN/DATA gate and the scheduler filter.
 *   P3B1    : announced_mask is index-only; scheduler still c->open_sent.
 *   P3B2A   : announced_mask + announced_gen gate DATA, scheduler still
 *             c->open_sent (the mixed state that failed in QEMU).
 *   P3B3    : announced_mask + announced_gen gate DATA, and the scheduler
 *             need_open decision is per stream.
 *
 * A carrier replacement creates a new carrier object with a new gen and
 * open_sent=false. A stream's old announced bit must not suppress the new
 * OPEN. A new stream on an already-open carrier must not send DATA before its
 * own OPEN, even though c->open_sent is true for the old stream.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_CARRIERS 4
#define MAX_STREAMS 4096
#define MAX_PENDING 512

/* Kernel constants, copied so the model fails where the module fails.
 * tbtun.h:130-148 and tbtun.h:391-395. */
#define TBT_STREAMS_MAX 128
#define TBT_STREAM_HT 64
#define TBT_TOMBSTONE_MS 2000
#define TBT_RX_RING_MAX (64u * 1024u * 1024u)

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

enum mode
{
    MODE_P2 = 0,
    MODE_P3B1,
    MODE_P3B2A,
    MODE_P3B3,
};

static const char *mode_name(enum mode m)
{
    switch (m)
    {
    case MODE_P2:
        return "P2";
    case MODE_P3B1:
        return "P3B1";
    case MODE_P3B2A:
        return "P3B2A";
    case MODE_P3B3:
        return "P3B3";
    }
    return "?";
}

struct carrier
{
    int index;
    int gen;
    bool hello_sent;
    bool open_sent;
    bool up;
    bool stopping;
    bool open_pending;
};

struct stream
{
    uint32_t sid;
    bool active;
    bool retired;
    bool tombstone;
    int refs;              /* frames/work references in the model */
    int target;            /* -1 = none, otherwise carrier-independent target id */
    int target_attached;   /* 0/1: exactly one target per stream */
    uint64_t tx_ack;
    uint64_t tx_seq;
    uint64_t rx_next;
    uint16_t announced_mask;
    uint8_t announced_gen[MAX_CARRIERS];
    uint32_t data_pending;
    uint32_t data_sent;
    uint16_t data_paths;  /* carriers that actually carried DATA */
    uint64_t retain_path_end[MAX_CARRIERS];
    bool sack_dirty;
    uint64_t sack_last;

    /* P3B3 close/reap state.  tbtun.h:294, tbtun.h:328-331. */
    bool session_detached;  /* tbt_stream_detach_from_session() ran */
    bool tx_fin_sent;       /* local FIN queued once */
    bool rx_fin_seen;       /* peer FIN arrived */
    bool rx_fin_done;       /* peer FIN delivered (rx_q empty, seq reached) */
    bool paused;            /* rx_q full: this stream stops reading */
    bool has_sk;            /* st->sk != NULL, as tbt_carrier.c:445 checks */
    bool unlinked;          /* reaped from the hash */
    uint64_t tombstone_until_ns;
    uint64_t rx_fin_seq;
    uint32_t rx_pending;    /* DATA waiting on rx_carrier */
    uint32_t rx_queued;
    int rx_carrier;
};

struct session
{
    enum mode mode;
    int ncar;
    struct carrier car[MAX_CARRIERS];
    int nst;
    struct stream st[MAX_STREAMS];
    uint32_t far_sid;      /* primary stream, 0 = none */
    int pending[MAX_PENDING];
    int npend;
    uint64_t stale;
    uint64_t cross_talk;
    uint64_t target_leaks;
    uint64_t ref_leaks;

    /* P3B3 session bookkeeping.  tbtun.h:391-395. */
    int n_active;           /* non-detached streams, the concurrency bound */
    int n_streams;          /* hash entries, including tombstones */
    uint64_t next_sid;      /* 24-bit monotonic allocator */
    uint64_t now_ns;        /* model clock for tombstone deadlines */
    int free_slots[MAX_STREAMS];
    int nfree;
    uint64_t rx_paused_skips;
};


static struct stream *stream_new(struct session *s, uint32_t sid)
{
    struct stream *st;

    /* tbt_sock.c:548-553 and tbt_session.c:632-636 reject the 129th live
     * stream; the model uses the same bound. */
    if (s->n_active >= TBT_STREAMS_MAX)
        return NULL;
    if (s->nfree)
        st = &s->st[s->free_slots[--s->nfree]];
    else if (s->nst < MAX_STREAMS)
        st = &s->st[s->nst++];
    else
        return NULL;
    memset(st, 0, sizeof(*st));
    st->sid = sid;
    st->active = true;
    st->target = -1;
    st->tx_ack = 0;
    st->tx_seq = 0;
    st->rx_next = 0;
    st->refs = 2; /* hash + caller */
    s->far_sid = sid;
    s->n_streams++;
    s->n_active++;
    return st;
}

/* Hash lookup, including tombstones: tbt_session.c:679-686 and the near-side
 * drop rule at tbt_session.c:728-736. */
static struct stream *stream_find(struct session *s, uint32_t sid)
{
    int i;

    for (i = 0; i < s->nst; i++)
    {
        const struct stream *st = &s->st[i];

        if (st->unlinked || st->sid != sid)
            continue;
        if (st->active || st->tombstone)
            return &s->st[i];
    }
    return NULL;
}

/* tbt_session.c:779-799: detach is idempotent, decrements n_active once,
 * stamps the tombstone deadline, and leaves the hash entry for the reaper. */
static void stream_detach_from_session(struct session *s, struct stream *st)
{
    if (!st || st->session_detached)
        return;
    st->session_detached = true;
    st->tombstone = true;
    st->tombstone_until_ns = s->now_ns +
        (uint64_t)TBT_TOMBSTONE_MS * 1000000ull;
    if (s->n_active)
        s->n_active--;
    if (s->far_sid == st->sid)
        s->far_sid = 0;
}


static bool announced(const struct stream *st, const struct carrier *c)
{
    if (!st || !c)
        return false;
    if (!(st->announced_mask & (1u << c->index)))
        return false;
    if (st->announced_gen[c->index] != (c->gen & 0xf))
        return false;
    return true;
}

/* tbt_tx_announce(): queue one OPEN per carrier. */
static void announce(struct session *s, struct stream *st)
{
    int i;

    for (i = 0; i < s->ncar; i++)
    {
        struct carrier *c = &s->car[i];

        if (!c->hello_sent || c->stopping)
            continue;
        if (s->mode == MODE_P2)
        {
            if (c->open_sent)
                continue;
        }
        else if (s->mode == MODE_P3B1)
        {
            if (st->announced_mask & (1u << i))
                continue;
        }
        else
        {
            if (announced(st, c))
                continue;
        }
        c->open_pending = true;
        c->up = false;
    }
}

/* Write pending OPENs. A real pump prioritises pinned OPEN over DATA; the
 * model keeps that order unless a scenario deliberately tests the opposite. */
static void write_opens(struct session *s, struct stream *st, bool before_data)
{
    int i;

    if (!before_data)
        return;
    for (i = 0; i < s->ncar; i++)
    {
        struct carrier *c = &s->car[i];

        if (!c->open_pending)
            continue;
        c->open_pending = false;
        c->open_sent = true;
        c->up = true;
        st->announced_mask |= (1u << i);
        st->announced_gen[i] = c->gen & 0xf;
    }
}

/* Scheduler need_open: P2/P3B1/P3B2A keep the carrier-level c->open_sent
 * filter; P3B3 asks whether this stream announced this (index,gen). */
static bool sched_need_open(struct session *s, struct stream *st,
                            struct carrier *c)
{
    if (s->mode == MODE_P3B3)
        return announced(st, c);
    return c->open_sent;
}

/* DATA gate: P2 uses the carrier flag; P3B1 uses index-only mask; P3B2A/P3B3
 * use the generation-aware matrix. */
static bool data_gate(struct session *s, struct stream *st, struct carrier *c)
{
    if (!c->up)
        return false;
    if (s->mode == MODE_P2)
        return c->open_sent;
    if (s->mode == MODE_P3B1)
        return (st->announced_mask & (1u << c->index)) != 0;
    return announced(st, c);
}

static void pump(struct session *s, struct stream *st, bool open_first)
{
    int i;

    if (!st || !st->active || st->session_detached)
        return;
    write_opens(s, st, open_first);
    for (i = 0; i < s->ncar && st->data_pending; i++)
    {
        struct carrier *c = &s->car[i];

        if (c->stopping || !sched_need_open(s, st, c))
            continue;
        if (!data_gate(s, st, c))
            continue;
        st->data_sent += st->data_pending;
        st->data_paths |= (uint16_t)(1u << c->index);
        st->retain_path_end[c->index] = st->data_sent;
        st->data_pending = 0;
        st->sack_dirty = true;
    }
    /* open_first=false is a DATA-only pass: OPEN stays queued, so the
     * scenario can inspect the gate before announcement. */
}

/* A new carrier at the same index: new object, new gen, open_sent=false. */
static void replace_carrier(struct session *s, int index, int gen)
{
    struct carrier *c = &s->car[index];

    c->gen = gen;
    c->open_sent = false;
    c->up = false;
    c->hello_sent = false;
    c->open_pending = false;
}

static void carrier_hello(struct session *s, int index)
{
    s->car[index].hello_sent = true;
    s->car[index].up = false;
}

static void stream_retire(struct session *s, struct stream *st)
{
    if (!st || !st->active)
        return;
    st->active = false;
    st->retired = true;
    /* tbt_stream_kill() detaches before the object can be reaped
     * (tbt_session.c:834-848). */
    stream_detach_from_session(s, st);
    st->announced_mask = 0;
    memset(st->announced_gen, 0, sizeof(st->announced_gen));
}

static void stream_ack(struct stream *st, uint64_t uack)
{
    /* A late ACK for a detached stream is dropped at bind time
     * (tbt_session.c:728-736). */
    if (!st || !st->active || st->session_detached)
        return;
    if (uack > st->tx_seq)
        uack = st->tx_seq;
    if (uack > st->tx_ack)
        st->tx_ack = uack;
}

static void stream_sack(struct stream *st)
{
    if (!st || !st->active || st->session_detached)
        return;
    st->sack_dirty = true;
}

static void frame_ref_inc(struct stream *st)
{
    st->refs++;
}

static void frame_ref_dec(struct stream *st)
{
    if (st->refs > 0)
        st->refs--;
}

static void target_attach(struct stream *st)
{
    if (st->target < 0)
    {
        st->target = (int)st->sid;
        st->target_attached++;
    }
}

static void target_detach(struct stream *st)
{
    if (st->target >= 0)
    {
        st->target = -1;
        if (st->target_attached > 0)
            st->target_attached--;
    }
}

/* ------------------------------------------------------------------------- */
/* P3B3 close, delivery, reaping and sid allocation                          */

enum sid_result
{
    SID_OK = 0,
    SID_COLLISION,
    SID_FULL,
};

/* tbt_sock.c:116-132: the local FIN is queued after every earlier byte.  If
 * the peer's FIN was already delivered, both directions are done and the
 * stream detaches; otherwise it stays attached for the return traffic. */
static bool stream_tx_fin(struct session *s, struct stream *st)
{
    if (!st || !st->active || st->session_detached)
        return false;
    if (st->tx_fin_sent)
        return true;
    st->tx_fin_sent = true;
    if (st->rx_fin_done)
        stream_detach_from_session(s, st);
    return true;
}

/* tbt_sock.c:284-314: the peer's FIN takes effect only when rx_q is empty and
 * rx_next reached the FIN's byte offset.  The other half of the close then
 * detaches. */
static bool stream_rx_fin(struct session *s, struct stream *st, uint64_t seq)
{
    if (!st || !st->active || st->session_detached)
        return false;
    st->rx_fin_seen = true;
    st->rx_fin_seq = seq;
    if (st->rx_queued == 0 && st->rx_next >= st->rx_fin_seq)
    {
        st->rx_fin_done = true;
        if (st->tx_fin_sent)
            stream_detach_from_session(s, st);
    }
    return true;
}

/* tbt_rx.c:149-227: DATA is delivered only in order, only while the queue has
 * room, and never for a detached or tombstoned stream. */
static bool rx_deliver(struct stream *st, uint64_t seq, uint32_t len)
{
    if (!st || st->session_detached || !st->active || st->tombstone)
        return false;
    if (st->paused || st->rx_queued >= TBT_RX_RING_MAX)
        return false;
    if (seq != st->rx_next)
        return false;
    st->rx_next += len;
    return true;
}

/* tbt_carrier.c:438-456: the current kernel scans the whole session hash and
 * returns from tbt_car_rx_work() on the first paused stream that has a socket.
 * One stream's delivery backpressure therefore stops the carrier for every
 * stream sharing it.  isolate=true is the expected behaviour: the pause
 * belongs to the paused stream's own rx_q (tbt_rx.c:989-993). */
static int carrier_rx_work(struct session *s, int ci, bool isolate)
{
    int i, delivered = 0;

    if (!isolate)
    {
        for (i = 0; i < s->nst; i++)
        {
            struct stream *st = &s->st[i];

            if (st->unlinked || !st->active || !st->paused || !st->has_sk)
                continue;
            s->rx_paused_skips++;
            return 0;
        }
    }
    for (i = 0; i < s->nst; i++)
    {
        struct stream *st = &s->st[i];

        if (st->unlinked || !st->active || st->rx_carrier != ci)
            continue;
        if (!st->rx_pending || st->paused)
            continue;
        if (rx_deliver(st, st->rx_next, st->rx_pending))
        {
            st->rx_pending = 0;
            delivered++;
        }
    }
    return delivered;
}

/* tbt_session.c:876-921: only a tombstone past its deadline is unlinked;
 * n_streams tracks hash entries and drops with the unlink. */
static int session_reap(struct session *s)
{
    int i, n = 0;

    for (i = 0; i < s->nst; i++)
    {
        struct stream *st = &s->st[i];

        if (st->unlinked || !st->tombstone)
            continue;
        if (!st->tombstone_until_ns || s->now_ns < st->tombstone_until_ns)
            continue;
        st->unlinked = true;
        st->tombstone = false;
        if (s->n_streams)
            s->n_streams--;
        s->free_slots[s->nfree++] = i;
        n++;
    }
    return n;
}

static int session_hash_count(const struct session *s)
{
    int i, n = 0;

    for (i = 0; i < s->nst; i++)
        if (!s->st[i].unlinked)
            n++;
    return n;
}

/* tbt_sock.c:554-559: sid = prefix<<24 | (next_sid & 0xffffff); next_sid++;
 * sid 0 becomes 1 because 0 is the "no stream" sentinel.  A wrap onto a live
 * sid must be a collision, not a silent reuse. */
static enum sid_result session_alloc_sid(struct session *s, uint32_t *out)
{
    uint32_t sid;

    if (s->n_active >= TBT_STREAMS_MAX)
        return SID_FULL;
    sid = (uint32_t)(s->next_sid & 0xffffff);
    s->next_sid++;
    if (!sid)
        sid = 1;
    if (stream_find(s, sid))
        return SID_COLLISION;
    *out = sid;
    return SID_OK;
}

/* Current kernel behaviour, kept visible for the test: tbt_session.c:622-630
 * returns the stream it found in the hash ("lost the race"), and
 * tbt_stream_start_now() then overwrites that stream's socket. */
static struct stream *session_create_current(struct session *s, uint32_t sid)
{
    struct stream *found = stream_find(s, sid);

    if (found)
        return found;
    return stream_new(s, sid);
}

/* ------------------------------------------------------------------------- */
/* Scenarios                                                                  */

static void scenario_carrier_replace(enum mode mode)
{
    struct session s = {0};
    struct stream *st;
    int i;

    s.mode = mode;
    s.ncar = 1;
    for (i = 0; i < s.ncar; i++)
        s.car[i].index = i;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    st = stream_new(&s, 100);
    announce(&s, st);
    pump(&s, st, true);
    st->data_pending = 10;
    pump(&s, st, true);
    ok(st->data_sent == 10, "replace: first carrier carries DATA",
       mode_name(mode));

    replace_carrier(&s, 0, 2);
    carrier_hello(&s, 0);
    announce(&s, st);
    st->data_pending = 20;
    pump(&s, st, true);

    if (mode == MODE_P3B1)
    {
        /* index-only mask: the old bit suppresses the new OPEN. */
        ok(st->data_sent == 10, "P3B1: stale announced bit blocks new gen",
           "reproduced");
        ok(s.car[0].up == false, "P3B1: new carrier stays down", "reproduced");
    }
    else
    {
        ok(st->data_sent == 30, "replace: new gen re-announces and carries DATA",
           mode_name(mode));
        ok(announced(st, &s.car[0]), "replace: announced matches new gen",
           mode_name(mode));
    }
}

static void scenario_new_stream_existing_carrier(enum mode mode)
{
    struct session s = {0};
    struct stream *a, *b;

    s.mode = mode;
    s.ncar = 1;
    s.car[0].index = 0;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    a = stream_new(&s, 200);
    announce(&s, a);
    pump(&s, a, true);
    a->data_pending = 5;
    pump(&s, a, true);

    b = stream_new(&s, 201);
    announce(&s, b);
    b->data_pending = 7;
    /* Deliberately let DATA be considered before B's OPEN: this is the
     * window c->open_sent cannot express. */
    pump(&s, b, false);

    if (mode == MODE_P2)
    {
        ok(b->data_sent == 7,
           "P2: c->open_sent admits B DATA before B OPEN", "reproduced");
        ok(!announced(b, &s.car[0]),
           "P2: B DATA sent without per-stream OPEN", "reproduced");
    }
    else
    {
        ok(b->data_sent == 0,
           "multi: B DATA blocked until B announced", mode_name(mode));
        pump(&s, b, true);
        ok(b->data_sent == 7, "multi: B DATA after B OPEN", mode_name(mode));
        ok(announced(b, &s.car[0]), "multi: B announced on (index,gen)",
           mode_name(mode));
    }
}

static void scenario_churn(enum mode mode, int n)
{
    struct session s = {0};
    uint32_t sid;
    int i;

    s.mode = mode;
    s.ncar = 2;
    for (i = 0; i < s.ncar; i++)
    {
        s.car[i].index = i;
        s.car[i].gen = 1;
        carrier_hello(&s, i);
    }

    for (i = 0; i < n; i++)
    {
        struct stream *st;
        uint64_t old_ack;

        sid = 1000 + (uint32_t)i;
        st = stream_new(&s, sid);
        ok(st != NULL, "churn alloc", "");
        if (!st)
            break;
        target_attach(st);
        announce(&s, st);
        st->tx_seq = 1;
        st->data_pending = 1;
        pump(&s, st, true);
        old_ack = (uint64_t)(i + 1);
        st->tx_seq = old_ack;
        stream_ack(st, old_ack);
        ok(st->tx_ack == old_ack, "churn ack independent", "");
        stream_sack(st);
        ok(st->sack_dirty, "churn SACK dirty", "");
        stream_retire(&s, st);
        target_detach(st);
        ok(st->target_attached == 0, "churn target detached", "");
        stream_ack(st, old_ack + 1000);
        ok(st->tx_ack == old_ack, "retired stream ignores late ack", "");
        ok(s.far_sid == 0, "churn no primary after retire", "");
    }
    ok(s.far_sid == 0, "churn ends idle", "");
}

static void scenario_concurrent(enum mode mode, int n)
{
    struct session s = {0};
    int i;

    s.mode = mode;
    s.ncar = 4;
    for (i = 0; i < s.ncar; i++)
    {
        s.car[i].index = i;
        s.car[i].gen = 1;
        carrier_hello(&s, i);
    }

    for (i = 0; i < n; i++)
    {
        struct stream *st = stream_new(&s, 5000 + (uint32_t)i);

        ok(st != NULL, "concurrent alloc", "");
        if (!st)
            return;
        target_attach(st);
        announce(&s, st);
        st->tx_seq = 4;
        st->data_pending = 4;
        pump(&s, st, true);
        ok(st->data_sent == 4, "concurrent DATA sent", mode_name(mode));
        stream_ack(st, 4);
        ok(st->tx_ack == 4, "concurrent ack independent", "");
        ok(st->target_attached == 1, "concurrent exactly one target", "");
        if (mode != MODE_P2)
        {
            ok(announced(st, &s.car[0]), "concurrent announced gen",
               mode_name(mode));
            ok((st->data_paths & ~st->announced_mask) == 0,
               "DATA only on announced carriers", mode_name(mode));
        }
    }

    for (i = 0; i < n; i++)
    {
        struct stream *st = &s.st[i];

        stream_retire(&s, st);
        target_detach(st);
        ok(st->target_attached == 0, "concurrent target released", "");
        ok(st->refs == 2, "concurrent refs balanced (hash+caller)", "");
    }
}

static void scenario_tombstone(enum mode mode)
{
    struct session s = {0};
    struct stream *old, *fresh;

    s.mode = mode;
    s.ncar = 1;
    s.car[0].index = 0;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    old = stream_new(&s, 700);
    target_attach(old);
    announce(&s, old);
    old->tx_seq = 10;
    old->data_pending = 10;
    pump(&s, old, true);
    stream_ack(old, 10);
    stream_retire(&s, old);

    fresh = stream_new(&s, 701);
    ok(fresh->tx_ack == 0, "fresh stream ack starts at 0", "");
    ok(!announced(fresh, &s.car[0]), "fresh stream not announced yet", "");
    stream_ack(old, 999);
    ok(old->tx_ack == 10 && fresh->tx_ack == 0,
       "retired ack does not migrate", "");
    announce(&s, fresh);
    fresh->data_pending = 1;
    pump(&s, fresh, true);
    ok(fresh->data_sent == 1, "fresh stream carries DATA", "");
}

static void scenario_frame_refs(enum mode mode)
{
    struct session s = {0};
    struct stream *st = stream_new(&s, 800);

    (void)mode;
    target_attach(st);
    frame_ref_inc(st);
    frame_ref_inc(st);
    ok(st->refs == 4, "two frames hold refs", "");
    frame_ref_dec(st);
    ok(st->refs == 3, "write/drop puts once", "");
    stream_retire(&s, st);
    ok(st->refs == 3, "retire does not drop frame refs", "");
    frame_ref_dec(st);
    frame_ref_dec(st);
    frame_ref_dec(st);
    ok(st->refs == 0, "all refs returned", "");
    target_detach(st);
}

/* P3B3: the session admits TBT_STREAMS_MAX (128) live streams.  tbt_sock.c:
 * 548-553 rejects at the bound and tbt_session.c:632-636 repeats the check
 * after the hash lookup.  A 101st stream must still fit; the 129th must fail,
 * and the rejection must not disturb n_active. */
static void scenario_limits_p3b3(void)
{
    struct session s = {0};
    struct stream *st;
    uint32_t targets[100];
    int i, j;
    bool unique = true;

    s.mode = MODE_P3B3;
    s.ncar = 4;
    for (i = 0; i < s.ncar; i++)
    {
        s.car[i].index = i;
        s.car[i].gen = 1;
        carrier_hello(&s, i);
    }

    for (i = 0; i < 100; i++)
    {
        st = stream_new(&s, 5000 + (uint32_t)i);
        ok(st != NULL, "limits: 100 concurrent streams fit", "");
        if (!st)
            return;
        target_attach(st);
        ok(st->active && st->target == (int)st->sid &&
           st->target_attached == 1,
           "limits: each stream has its own target", "");
        targets[i] = (uint32_t)st->target;
    }
    for (i = 0; i < 100 && unique; i++)
        for (j = 0; j < i; j++)
            if (targets[i] == targets[j])
                unique = false;
    ok(unique, "limits: 100 targets are distinct", "");
    ok(s.n_active == 100, "limits: n_active is exactly 100", "");
    ok(s.n_streams == 100, "limits: n_streams is exactly 100", "");

    st = stream_new(&s, 5100);
    ok(st != NULL && s.n_active == 101,
       "limits: stream 101 still fits under the 128 bound", "");
    for (i = 102; i <= TBT_STREAMS_MAX; i++)
    {
        st = stream_new(&s, 5000 + (uint32_t)i);
        ok(st != NULL, "limits: stream fits under the 128 bound", "");
    }
    ok(s.n_active == TBT_STREAMS_MAX, "limits: n_active reaches 128", "");
    ok(stream_new(&s, 6000) == NULL, "limits: stream 129 is rejected", "");
    ok(s.n_active == TBT_STREAMS_MAX,
       "limits: rejected stream does not change n_active", "");

    for (i = 0; i < s.nst; i++)
        if (!s.st[i].unlinked)
            stream_retire(&s, &s.st[i]);
    ok(s.n_active == 0, "limits: all streams retire", "");
}

/* P3B3: FIN is stream-ordered and each direction is independent.  Only both
 * directions finished detach the stream (tbt_sock.c:124-132 and
 * tbt_sock.c:313-314); a half-closed stream stays attached so return traffic
 * still reaches it (tbt_sock.c:130-131). */
static void scenario_fin_detach_p3b3(void)
{
    struct session s = {0};
    struct stream *a, *b, *c;

    s.mode = MODE_P3B3;
    s.ncar = 1;
    s.car[0].index = 0;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    a = stream_new(&s, 900);
    b = stream_new(&s, 901);
    c = stream_new(&s, 902);
    ok(a && b && c, "fin: three streams allocated", "");

    /* A: our FIN first; the peer's FIN has not arrived.  The stream stays
     * attached and the peer's return DATA still reaches it. */
    ok(stream_tx_fin(&s, a), "fin: local FIN queued", "");
    ok(!a->session_detached && s.n_active == 3,
       "fin: one-way local FIN keeps the stream attached", "");
    ok(rx_deliver(a, 0, 4) && a->rx_next == 4,
       "fin: return DATA still delivered after local FIN", "");

    /* B: the peer's FIN is delivered first; our FIN has not been sent.  The
     * stream stays attached and our DATA still flows. */
    ok(stream_rx_fin(&s, b, 0), "fin: peer FIN accepted", "");
    ok(b->rx_fin_done && !b->session_detached && s.n_active == 3,
       "fin: one-way peer FIN keeps the stream attached", "");
    announce(&s, b);
    b->data_pending = 4;
    pump(&s, b, true);
    ok(b->data_sent == 4, "fin: local DATA still flows after peer FIN", "");

    /* C: both directions finish, in the peer-first order. */
    ok(stream_rx_fin(&s, c, 0), "fin: peer FIN accepted on C", "");
    ok(!c->session_detached, "fin: peer FIN alone does not detach", "");
    ok(stream_tx_fin(&s, c), "fin: local FIN queued on C", "");
    ok(c->session_detached && s.n_active == 2,
       "fin: both FINs detach and drop n_active", "");
    ok(!rx_deliver(c, 0, 4) && c->rx_next == 0,
       "fin: late DATA is dropped after detach", "");
    c->tx_seq = 8;
    stream_ack(c, 8);
    ok(c->tx_ack == 0, "fin: late ack is dropped after detach", "");
    ok(s.n_active == 2, "fin: half-closed streams remain active", "");
}

/* P3B3: one paused stream must not stop the carrier for every other stream.
 * Current kernel: tbt_carrier.c:438-456 scans the whole session hash and
 * returns from tbt_car_rx_work() on the first paused stream that has a
 * socket.  That is the defect this test pins; isolate=true is the expected
 * behaviour, and the current behaviour is asserted separately so the bug
 * cannot be silently "fixed" in the model. */
static void scenario_pause_isolation_p3b3(void)
{
    struct session s = {0};
    struct stream *a, *b;
    int delivered;

    s.mode = MODE_P3B3;
    s.ncar = 1;
    s.car[0].index = 0;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    a = stream_new(&s, 950);
    b = stream_new(&s, 951);
    ok(a && b, "pause: two streams share one carrier", "");
    a->has_sk = true;
    a->paused = true; /* rx_q full: tbt_rx.c:989-993 */
    a->rx_queued = TBT_RX_RING_MAX;
    a->rx_carrier = 0;
    b->has_sk = true;
    b->rx_carrier = 0;
    b->rx_pending = 4;

    delivered = carrier_rx_work(&s, 0, false);
    ok(delivered == 0 && b->rx_next == 0 && s.rx_paused_skips == 1,
       "pause: current kernel blocks the peer stream (known defect)", "");

    delivered = carrier_rx_work(&s, 0, true);
    ok(delivered == 1 && b->rx_next == 4 && a->rx_next == 0,
       "pause: one paused stream must not block another", "");
}

/* P3B3: 100 streams churn 200 times.  Each round creates 100, carries one
 * DATA frame, closes both directions (which detaches), leaves a tombstone,
 * and reaps after the deadline.  n_active returns to 0, the hash does not
 * grow without bound, and a reap empties it.  tbt_session.c:876-921 reaps
 * only after TBT_TOMBSTONE_MS (tbtun.h:131) and unlinks the hash entry. */
static void scenario_churn_reap_p3b3(void)
{
    struct session s = {0};
    int round, i, max_streams = 0;

    s.mode = MODE_P3B3;
    /* The allocator starts at 1 so the first 100 sids are 1..100.  The kernel
     * stores 0 in next_sid (tbt_session.c:985) and rewrites the zero sid to 1
     * (tbt_sock.c:554-559), which repeats sid 1 on the second allocation. */
    s.next_sid = 1;
    s.ncar = 2;
    for (i = 0; i < s.ncar; i++)
    {
        s.car[i].index = i;
        s.car[i].gen = 1;
        carrier_hello(&s, i);
    }

    for (round = 0; round < 200; round++)
    {
        for (i = 0; i < 100; i++)
        {
            struct stream *st;
            uint32_t sid = 0;

            ok(session_alloc_sid(&s, &sid) == SID_OK,
               "churn: sid allocation succeeds", "");
            st = stream_new(&s, sid);
            ok(st != NULL, "churn: stream allocated", "");
            if (!st)
                return;
            target_attach(st);
            announce(&s, st);
            st->data_pending = 1;
            pump(&s, st, true);
            /* Both directions finish; the second FIN detaches. */
            stream_tx_fin(&s, st);
            stream_rx_fin(&s, st, 0);
        }
        ok(s.n_active == 0, "churn: round leaves no active stream", "");
        ok(s.n_streams == 100,
           "churn: tombstones are bounded by the round", "");
        if (s.n_streams > max_streams)
            max_streams = s.n_streams;
        s.now_ns += (uint64_t)TBT_TOMBSTONE_MS * 1000000ull;
        ok(session_reap(&s) == 100, "churn: reap removes the round", "");
        ok(s.n_streams == 0 && session_hash_count(&s) == 0,
           "churn: tombstone expiry clears the hash", "");
    }
    ok(max_streams <= 100, "churn: n_streams does not grow without bound", "");
    ok(s.n_active == 0 && s.n_streams == 0, "churn: churn ends empty", "");
}

/* P3B3: the 24-bit sid allocator must never hand out a live sid twice.
 * tbt_sock.c:554-559 wraps next_sid & 0xffffff; tbt_session.c:622-630
 * currently returns the found stream ("lost the race") and tbt_stream_start_now
 * then overwrites its socket.  A wrap onto a live sid must be a collision. */
static void scenario_sid_alloc_p3b3(void)
{
    struct session s = {0};
    struct stream *first, *found;
    uint32_t sid = 0;
    uint32_t seen[100];
    int i, j;
    bool unique = true;

    s.mode = MODE_P3B3;
    s.next_sid = 1;
    s.ncar = 1;
    s.car[0].index = 0;
    s.car[0].gen = 1;
    carrier_hello(&s, 0);

    ok(s.next_sid == 1, "sid: next_sid starts at 1", "");
    for (i = 0; i < 100; i++)
    {
        enum sid_result rc = session_alloc_sid(&s, &sid);

        ok(rc == SID_OK && sid == (uint32_t)(i + 1),
           "sid: first 100 allocations are 1..100", "");
        seen[i] = sid;
    }
    for (i = 0; i < 100 && unique; i++)
        for (j = 0; j < i; j++)
            if (seen[i] == seen[j])
                unique = false;
    ok(unique, "sid: 100 consecutive sids are distinct", "");

    first = stream_new(&s, 1);
    ok(first != NULL, "sid: stream 1 is live", "");
    first->has_sk = true;   /* an attached socket */
    first->target = 42;
    first->target_attached = 1;
    s.next_sid = 1u << 24; /* 2^24 wrap lands on sid 1 */
    ok(session_alloc_sid(&s, &sid) == SID_COLLISION,
       "sid: 2^24 wrap onto a live sid is a collision", "");
    ok(s.next_sid == (1ull << 24) + 1,
       "sid: allocator advances past the wrap", "");
    found = stream_find(&s, 1);
    ok(found == first && first->has_sk && first->target == 42 && first->active,
       "sid: collision does not overwrite the live stream", "");

    /* Current kernel behaviour, kept visible: tbt_session.c:622-630 returns
     * the found object instead of reporting a collision. */
    ok(session_create_current(&s, 1) == first,
       "sid: current kernel reuses the live stream (known defect)", "");
}

/* tbt_session.c:627-712: 256 carriers deliver the same OPEN. Every carrier
 * may pass the "!st" precheck, but the get-or-create helper rechecks under
 * sess->lock and reports whether it published the object. Only that one
 * caller may run tbt_far_open_seen(); the rest reuse the stream. */
static struct stream *stream_get_created(struct session *s, uint32_t sid,
                                         bool *created)
{
    struct stream *st = stream_find(s, sid);

    *created = false;
    if (st)
    {
        st->refs++;
        return st;
    }
    st = stream_new(s, sid);
    if (st)
        *created = true;
    return st;
}

static void scenario_open_race_p3b3(void)
{
    enum { carriers = 256 };
    struct session s = {0};
    int created = 0, reused = 0, adopted = 0;
    int i;

    s.mode = MODE_P3B3;
    s.ncar = 1;

    /* All 256 carriers observe an empty hash before any of them publishes. */
    for (i = 0; i < carriers; i++)
        ok(stream_find(&s, 42) == NULL, "open race: precheck sees empty", "");

    for (i = 0; i < carriers; i++)
    {
        bool made = false;
        struct stream *st = stream_get_created(&s, 42, &made);

        if (!st)
            continue;
        if (made)
        {
            created++;
            adopted++; /* only the publisher calls tbt_far_open_seen() */
        }
        else
        {
            reused++;
        }
    }
    ok(created == 1, "open race: exactly one stream published", "");
    ok(reused == carriers - 1, "open race: all other carriers reuse", "");
    ok(adopted == 1, "open race: only one OPEN adoption/announce", "");
    ok(s.n_streams == 1 && s.n_active == 1,
       "open race: one hash entry and one active stream", "");
}

/* tbt_session_fec_rx_barrier(): an aborted stream may still have one FEC RX
 * frontier byte queued. The new stream must not start until that old RX work
 * is quiesced and the data plane drops its cursor; the first byte the new
 * stream sees is its own HTTP request, never the old tail. */
static void scenario_fec_abort_barrier(void)
{
    static const char rx[] = "XHTTP/1.1 200 OK\r\n";
    int head = 0;
    int barrier = 0;
    uint64_t far_sid = 1;

    /* Old stream aborted; its stale byte X is still at the RX cursor. */
    if (rx[head] != 'H')
    {
        barrier++;
        head++; /* tbt_session_fec_rx_barrier() drops the old frontier */
    }
    far_sid = 2; /* the new stream is installed only after the barrier */
    ok(barrier == 1, "fec abort: barrier ran once", "");
    ok(far_sid == 2, "fec abort: new sid installed after barrier", "");
    ok(!strncmp(rx + head, "HTTP/", 5),
       "fec abort: new stream first bytes are HTTP/", "");
}

/* tbt_stream_release_socket_now(): a target published before st->sk must
 * still be closed when the stream aborts. The next stream's target is then
 * readable; the old target cannot stay in FIN_WAIT/CLOSE_WAIT. */
static int stream_release_target(struct stream *st)
{
    if (st->target < 0)
        return 0;
    st->target = -1;
    st->target_attached = 0;
    return 1;
}

static void scenario_target_close_on_abort(void)
{
    struct session s = {0};
    struct stream *old = stream_new(&s, 1);
    struct stream *fresh;
    static const char response[] = "HTTP/1.1 200 OK\r\n";

    /* Race shape: target published, st->sk not yet visible. */
    old->target = 7;
    old->has_sk = false;
    ok(stream_release_target(old) == 1,
       "target abort: target-only stream still closes", "");
    ok(old->target == -1, "target abort: old target released", "");

    fresh = stream_new(&s, 2);
    fresh->target = 8;
    fresh->has_sk = true;
    ok(fresh->target == 8 && !strncmp(response, "HTTP/", 5),
       "target abort: new stream target is readable", "");
}

int main(void)
{
    static const enum mode modes[] = {
        MODE_P2, MODE_P3B1, MODE_P3B2A, MODE_P3B3,
    };
    size_t i;

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
        enum mode m = modes[i];

        scenario_carrier_replace(m);
        scenario_new_stream_existing_carrier(m);
        scenario_churn(m, 1000);
        scenario_concurrent(m, 2);
        scenario_concurrent(m, 10);
        scenario_concurrent(m, 100);
        scenario_tombstone(m);
        scenario_frame_refs(m);
    }

    /* P3B3-only boundaries: they assert the expected kernel behaviour, so
     * running them under P2/P3B1/P3B2A would only restate the known bugs. */
    scenario_limits_p3b3();
    scenario_fin_detach_p3b3();
    scenario_pause_isolation_p3b3();
    scenario_churn_reap_p3b3();
    scenario_sid_alloc_p3b3();
    scenario_open_race_p3b3();
    scenario_fec_abort_barrier();
    scenario_target_close_on_abort();

    printf("test_multistream: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
