/* test_carrier_policy.c - host contract for the carrier admission rules.
 *
 * The three rules live in src/kernel/tbt_carrier_policy.h, compiled by the module
 * and by this test, so the test calls the same predicates the kernel calls:
 *
 *   1. tbt_car_connect_work() creates a socket only when
 *      tbt_carrier_dial_ready() accepts the port allocation. EXHAUSTED or
 *      ENOMEM parks the carrier; it never dials from an ephemeral port.
 *   2. tbt_session_join() lets the same or a newer four-bit peer generation
 *      supersede a live far slot, and closes the old socket. An older
 *      generation is still a duplicate.
 *   3. tbt_peers_show() hides a stopping far carrier, so the view names only
 *      the current generation.
 *   4. tbt_car_setsockbuf() gives every carrier one 0.8 MiB buffer per
 *      direction and clears the buffer userlocks, so TCP/BBR autotuning
 *      may grow both fields;
 *      tbt_car_set_cc() asks for the configured carrier CC (bbr by default)
 *      and keeps the carrier's default when the kernel refuses it.
 *   5. tbt_carrier_stall_bound_ns() and tbt_carrier_stall_is_dead() decide
 *      when a paused FEC carrier becomes a loss. The backstop never fires
 *      inside a loss episode: 4 x the 1 s detection floor is 4 s, while the
 *      RTO ladder needs 6.2 s for five failed retransmissions and the design
 *      trusts a 30 s TCP keepalive to close a half-open path.
 *
 * The slot and socket models below follow those functions line by line.
 * Change both together.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tbt_carrier_policy.h"

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

/* One carrier slot and its socket. `socket_open` counts the dialled or
 * accepted connection; `stopping` is the flag RETIRE sets before w_replace
 * closes it. */
struct slot {
    bool occupied;
    u8 gen;
    bool stopping;
    bool socket_open;
};

static int dials;

/* tbt_car_connect_work(): the port gate runs before sock_create_kern(). */
static bool try_dial(struct slot *s, u16 port, bool alloc_failed)
{
    if (!tbt_carrier_dial_ready(port, alloc_failed))
        return false;
    s->occupied = true;
    dials++;
    s->socket_open = true;
    return true;
}

/* tbt_session_join(): a live occupant is replaced only when its generation is
 * superseded. A replaced socket is queued for close; the slot then names the
 * joining generation. */
static bool join(struct slot *slot, u8 gen, bool *old_closed)
{
    *old_closed = false;
    if (slot->occupied && slot->socket_open &&
        !tbt_carrier_gen_supersedes(gen, slot->gen))
        return false;
    if (slot->occupied && slot->socket_open)
        *old_closed = true;
    slot->occupied = true;
    slot->gen = gen;
    slot->stopping = false;
    slot->socket_open = true;
    return true;
}

/* tbt_peers_show(): one row per visible slot, in slot order. */
static int render(const struct slot *slots, int n, bool far, u8 *out)
{
    int i, rows = 0;

    for (i = 0; i < n; i++)
    {
        if (!slots[i].occupied)
            continue;
        if (!tbt_carrier_row_visible(far, slots[i].stopping))
            continue;
        out[rows++] = slots[i].gen;
    }
    return rows;
}

/* --- 1. port allocation failure never dials --------------------------- */

static void test_exhausted_never_dials(void)
{
    struct slot s = { 0 };

    dials = 0;
    ok(!try_dial(&s, 0, true),
       "EXHAUSTED: the gate refuses the dial");
    ok_int(dials, 0, "EXHAUSTED: no socket was created");
    ok(!s.socket_open, "EXHAUSTED: the carrier has no socket");

    /* Defensive: a reason other than OK must refuse even if an allocator bug
     * hands back a port. */
    ok(!try_dial(&s, 41000, true),
       "non-OK reason refuses a returned port");
    ok_int(dials, 0, "non-OK reason: still no dial");

    /* A zero port is never a source port, even with an OK reason. */
    ok(!try_dial(&s, 0, false),
       "port 0 refuses the dial");
    ok_int(dials, 0, "port 0: still no dial");

    ok(try_dial(&s, 41000, false),
       "OK: a real port admits the dial");
    ok_int(dials, 1, "OK: exactly one socket was created");
    ok(s.socket_open, "OK: the carrier owns the socket");
}

/* --- 2. same/newer generation closes the old far socket --------------- */

static void test_generation_supersedes(void)
{
    ok(tbt_carrier_gen_supersedes(5, 5),
       "equal generation supersedes (same-carrier redial)");
    ok(tbt_carrier_gen_supersedes(6, 5), "next generation supersedes");
    ok(!tbt_carrier_gen_supersedes(4, 5), "older generation is a duplicate");
    ok(tbt_carrier_gen_supersedes(0, 15), "0 supersedes 15 across the ring");
    ok(!tbt_carrier_gen_supersedes(15, 0),
       "15 does not supersede 0 across the ring");
    ok(tbt_carrier_gen_supersedes(12, 5),
       "seven steps forward supersedes");
    ok(!tbt_carrier_gen_supersedes(13, 5),
       "eight steps forward is the duplicate half");
}

static void test_replacement_closes_old_socket(void)
{
    struct slot s = { 0 };
    bool closed = false;

    dials = 0;
    ok(try_dial(&s, 40000, false), "join: the first carrier dials");
    s.gen = 7;

    ok(join(&s, 7, &closed),
       "same generation replaces the slot");
    ok(closed, "same generation closes the old socket");
    ok_int(s.gen, 7, "same generation: the slot keeps its generation");
    ok(s.socket_open, "same generation: the new socket is live");

    ok(join(&s, 8, &closed), "newer generation replaces the slot");
    ok(closed, "newer generation closes the old socket");
    ok_int(s.gen, 8, "newer generation: the slot moves forward");

    ok(!join(&s, 6, &closed), "older generation is refused");
    ok(!closed, "refused join leaves the current socket open");
    ok_int(s.gen, 8, "refused join leaves the current generation");

    s.gen = 15;
    ok(join(&s, 0, &closed) && closed,
       "generation 0 replaces 15 and closes its socket");
    ok_int(s.gen, 0, "generation 0 is now current");
}

/* --- 3. peers renders only the current far generation ----------------- */

static void test_peers_render(void)
{
    struct slot slots[2] = {
        { .occupied = true, .gen = 4, .stopping = true, .socket_open = true },
        { .occupied = true, .gen = 5, .stopping = false, .socket_open = true },
    };
    u8 rows[2];
    int n;

    n = render(slots, 2, true, rows);
    ok_int(n, 1, "far peers: the stopping row is hidden");
    ok_int(rows[0], 5, "far peers: only the current generation is shown");

    n = render(slots, 2, false, rows);
    ok_int(n, 2, "near peers: stopping rows stay visible");
    ok_int(rows[0], 4, "near peers: the stopping row is first");
    ok_int(rows[1], 5, "near peers: the live row follows");

    /* RETIRE hides the old row before the replacement joins; the join then
     * installs exactly one current row. */
    n = render(&slots[0], 1, true, rows);
    ok_int(n, 0, "far peers: a retiring slot renders no stale row");

    slots[0].gen = 6;
    slots[0].stopping = false;
    slots[0].socket_open = true;
    n = render(&slots[0], 1, true, rows);
    ok_int(n, 1, "far peers: the replacement renders one row");
    ok_int(rows[0], 6, "far peers: the row names the new generation");
}

/* --- 4. carrier transport: 0.8 MiB buffers, autotuning, BBR ----------- */

/* The four fields tbt_car_setsockbuf() and tbt_car_set_cc() touch. The
 * starting values describe a socket whose buffer fields were locked by an
 * earlier SO_SNDBUF/SO_RCVBUF request: the assignment must clear both lock
 * bits and keep every other bit. */
struct transport {
    unsigned int sndbuf;
    unsigned int rcvbuf;
    unsigned short userlocks;
    char cc[16];
};

/* tbt_car_setsockbuf(): one 0.8 MiB buffer per direction, then clear only
 * the two buffer locks. SOCK_CONNECT_BIND (16) and every other bit survive. */
static void model_setsockbuf(struct transport *t, unsigned int carriers)
{
    t->sndbuf = tbt_car_sockbuf_of(carriers);
    t->rcvbuf = t->sndbuf;
    t->userlocks &= ~TBT_CAR_SOCKBUF_LOCK_MASK;
}

/* tbt_car_set_cc(): one setsockopt(SOL_TCP, TCP_CONGESTION, "bbr", 4),
 * where optlen counts the terminating NUL. -ENOENT (no CONFIG_TCP_CONG_BBR)
 * is ignored and the carrier keeps the CC it already had. */
static int model_set_bbr(struct transport *t, bool kernel_has_bbr)
{
    if (!kernel_has_bbr)
        return -2; /* -ENOENT */
    strcpy(t->cc, TBT_CAR_CC_NAME);
    return 0;
}

static void test_carrier_transport(void)
{
    struct transport t = {
        .sndbuf = 65536,
        .rcvbuf = 65536,
        .userlocks = TBT_CAR_SOCKBUF_LOCK_MASK | 16, /* | SOCK_CONNECT_BIND */
        .cc = "cubic",
    };

    ok_int(TBT_CAR_SOCKBUF_PER, 838861,
           "transport: one direction holds 0.8 MiB");
    ok_int(tbt_car_sockbuf_of(0), TBT_CAR_SOCKBUF_PER,
           "transport: zero carriers still get 0.8 MiB per direction");
    ok_int(tbt_car_sockbuf_of(1), TBT_CAR_SOCKBUF_PER,
           "transport: one carrier gets 0.8 MiB per direction");
    ok_int(tbt_car_sockbuf_of(8), TBT_CAR_SOCKBUF_PER,
           "transport: eight carriers get 0.8 MiB per direction");
    ok_int(tbt_car_sockbuf_of(128), TBT_CAR_SOCKBUF_PER,
           "transport: 128 carriers get 0.8 MiB per direction");
    ok_int(tbt_car_sockbuf_of(256), TBT_CAR_SOCKBUF_PER,
           "transport: 256 carriers get 0.8 MiB per direction");

    model_setsockbuf(&t, 256);
    ok_int(t.sndbuf, TBT_CAR_SOCKBUF_PER,
           "transport: sk_sndbuf takes the per-carrier size");
    ok_int(t.rcvbuf, TBT_CAR_SOCKBUF_PER,
           "transport: sk_rcvbuf takes the per-carrier size");
    ok_int(t.userlocks, 16,
           "transport: both buffer locks are clear, other bits stay");
    ok(!(t.userlocks & TBT_CAR_SOCKBUF_LOCK_MASK),
       "transport: autotuning is enabled on both buffers");

    ok_int(model_set_bbr(&t, true), 0,
           "bbr: the setsockopt path accepts BBR");
    ok(!strcmp(t.cc, "bbr"),
       "bbr: the carrier requests the shared TBT_CAR_CC_NAME");
    ok_int((int)sizeof(TBT_CAR_CC_NAME), 4,
           "bbr: optlen covers the three letters and the NUL");
    ok_int(model_set_bbr(&t, false), -2,
           "bbr: a kernel without BBR answers -ENOENT");
    ok(!strcmp(t.cc, "bbr"),
       "bbr: a refused request keeps the carrier's current CC");
}

/* --- 5. stall-dead: a late ACK is not a dead carrier ------------------ */

#define NS_PER_MS 1000000ULL
#define NS_PER_S 1000000000ULL
#define POLL_NS (50ULL * NS_PER_MS)
/* The detection clock the poll computes for a 200 ms RTO. Five RTOs land
 * under the 1 s floor, so every timeline starts from that floor. */
#define STALL_NS (1ULL * NS_PER_S)

/* One paused carrier and the five fields the decision reads. */
struct wire {
    bool stalled;
    bool dead;
    bool paused;
    u32 una;
    u32 nxt;
    u32 snd_wnd;
    u32 packets_out;
    u32 packets_out_at_stall;
    u32 retransmits;
    u64 last_una;          /* c->last_una */
    u64 last_una_move_ns;  /* c->last_una_move_ns */
    u64 last_una_ns;       /* c->last_una_ns, the detection clock */
    u64 stall_since_ns;    /* c->stall_since_ns */
};

/* One 50 ms pass, in tbt_car_poll_one() order: the stall-dead decision
 * first, then the recovery that clears the latch, then the stall clock and
 * the declaration in tbt_carrier_stall(). */
static void poll_once(struct wire *w, u64 now, u64 dead_ns)
{
    bool progress = w->una != w->last_una;
    struct tbt_stall_view v = {
        .stalled = w->stalled,
        .paused = w->paused,
        .una_frozen = w->una == w->last_una,
        .snd_wnd = w->snd_wnd,
        .packets_out = w->packets_out,
        .packets_out_at_stall = w->packets_out_at_stall,
        .retransmits = w->retransmits,
        .silent_ns = now - w->stall_since_ns,
    };

    if (tbt_carrier_stall_is_dead(&v, dead_ns))
    {
        w->dead = true;
        return;
    }
    if (progress)
    {
        w->last_una = w->una;
        w->last_una_move_ns = now;
        w->stalled = false;
    }
    if (w->una == w->nxt)
        w->last_una_ns = now;
    else if (progress)
        w->last_una_ns = now;
    else if (w->paused || !w->snd_wnd)
        w->last_una_ns = now;
    else if (!w->last_una_ns)
        w->last_una_ns = now;
    else if (now - w->last_una_ns > STALL_NS && !w->stalled &&
             w->packets_out && now - w->last_una_move_ns >= NS_PER_S)
    {
        w->stalled = true;
        w->stall_since_ns = now;
        w->packets_out_at_stall = w->packets_out;
        w->last_una_ns = now;
    }
}

/* Run the poll from `from` to `to`; a nonzero `ack_at_ns` delivers every
 * owed byte on the first pass at or after that time. */
static void run_wire(struct wire *w, u64 from, u64 to, u64 ack_at_ns,
                     u64 dead_ns)
{
    u64 now;

    for (now = from; now <= to; now += POLL_NS)
    {
        if (ack_at_ns && now >= ack_at_ns)
        {
            w->una = w->nxt;
            w->packets_out = 0;
        }
        poll_once(w, now, dead_ns);
        if (w->dead)
            return;
    }
}

/* A carrier paused with one share on the wire: snd_una frozen, one segment
 * out, peer window open. One second of silence declares the pause. */
static struct wire stalled_wire(void)
{
    struct wire w = {
        .una = 5000,
        .nxt = 5000 + 1024,
        .snd_wnd = 65535,
        .packets_out = 1,
        .last_una = 5000,
    };

    run_wire(&w, 0, 1200 * NS_PER_MS, 0, tbt_carrier_stall_bound_ns(STALL_NS));
    return w;
}

static void test_stall_bound(void)
{
    ok_int((int)(tbt_carrier_stall_bound_ns(STALL_NS) / NS_PER_S), 30,
           "bound: the 1 s detection floor yields the 30 s keepalive floor");
    ok_int((int)(tbt_carrier_stall_bound_ns(8 * NS_PER_S) / NS_PER_S), 32,
           "bound: four detection windows once they clear the floor");
    ok_int((int)(tbt_carrier_stall_bound_ns(60 * NS_PER_S) / NS_PER_S), 240,
           "bound: four 60 s windows stay under the cap");
    ok_int((int)(tbt_carrier_stall_bound_ns(120 * NS_PER_S) / NS_PER_S), 300,
           "bound: the 300 s cap holds");
}

/* The field false positive: one segment lost, the ACK lands six seconds
 * later. The pre-fix 4 s backstop killed the carrier first; the 30 s floor
 * lets the same timeline recover. */
static void test_late_ack_is_not_death(void)
{
    u64 bound = tbt_carrier_stall_bound_ns(STALL_NS);
    u64 legacy_dead_ns = 4 * STALL_NS; /* the pre-fix tbt_stall_dead_ns() */
    struct wire old_wire = stalled_wire();
    struct wire new_wire = stalled_wire();

    ok(old_wire.stalled, "late-ack: one frozen second pauses the carrier");

    run_wire(&old_wire, 1250 * NS_PER_MS, 10 * NS_PER_S, 6000 * NS_PER_MS,
             legacy_dead_ns);
    ok(old_wire.dead,
       "late-ack: the 4 s backstop kills a carrier whose ACK arrives at 6 s");

    run_wire(&new_wire, 1250 * NS_PER_MS, 10 * NS_PER_S, 6000 * NS_PER_MS,
             bound);
    ok(!new_wire.dead, "late-ack: the 30 s floor keeps that carrier alive");
    ok(!new_wire.stalled, "late-ack: the late ACK clears the pause");
    ok_int((int)new_wire.una, (int)new_wire.nxt,
           "late-ack: the wire delivered the share");
}

static void test_dead_carrier_dies(void)
{
    u64 bound = tbt_carrier_stall_bound_ns(STALL_NS);
    struct wire w = stalled_wire();
    u64 died_at = w.stall_since_ns + bound;

    run_wire(&w, 1250 * NS_PER_MS, died_at - NS_PER_MS, 0, bound);
    ok(!w.dead, "dead: a black hole just under the bound is still alive");

    run_wire(&w, died_at, died_at + 100 * NS_PER_MS, 0, bound);
    ok(w.dead, "dead: a black hole dies at the bound");
}

static void test_idle_never_stalls(void)
{
    u64 bound = tbt_carrier_stall_bound_ns(STALL_NS);
    struct wire w = { .una = 1000, .nxt = 1000, .snd_wnd = 65535 };
    u64 t;

    run_wire(&w, 0, 600 * NS_PER_S, 0, bound);
    ok(!w.stalled, "idle: ten silent minutes never pause the carrier");
    ok(!w.dead, "idle: an idle carrier is never dead");

    /* One share arrives and is acknowledged inside the next two polls. */
    t = 600 * NS_PER_S;
    w.nxt = 2024;
    w.packets_out = 1;
    poll_once(&w, t + POLL_NS, bound);
    w.una = w.nxt;
    w.packets_out = 0;
    poll_once(&w, t + 2 * POLL_NS, bound);
    ok(!w.stalled, "idle: one delivered share does not pause the carrier");
    ok(!w.dead, "idle: one delivered share does not kill the carrier");
}

static void test_wire_movement_blocks_death(void)
{
    u64 bound = tbt_carrier_stall_bound_ns(STALL_NS);
    struct wire w = stalled_wire();

    /* TCP still moves packets: the count differs from the pause snapshot
     * and the retry count is below tcp_retries2. */
    w.packets_out = w.packets_out_at_stall + 2;
    run_wire(&w, 1250 * NS_PER_MS, 600 * NS_PER_S, 0, bound);
    ok(!w.dead, "movement: a wire that still moves packets is alive");
}

static void test_retrans_exhaustion(void)
{
    u64 bound = tbt_carrier_stall_bound_ns(STALL_NS);
    struct wire w = stalled_wire();

    w.retransmits = TBT_STALL_RETX_MAX - 1;
    w.packets_out = w.packets_out_at_stall + 1;
    run_wire(&w, 1250 * NS_PER_MS, w.stall_since_ns + bound - NS_PER_MS, 0,
             bound);
    ok(!w.dead, "retx: below tcp_retries2 a moved wire stays alive");

    w.retransmits = TBT_STALL_RETX_MAX;
    run_wire(&w, w.stall_since_ns + bound,
             w.stall_since_ns + bound + 100 * NS_PER_MS, 0, bound);
    ok(w.dead, "retx: tcp_retries2 ends the pause although packets_out moved");
}

static void test_window_and_pause_gates(void)
{
    struct wire zero_wnd = stalled_wire();
    struct wire paused = stalled_wire();

    /* A one-second bound proves the gate, not the bound, keeps them alive. */
    zero_wnd.snd_wnd = 0;
    run_wire(&zero_wnd, 1250 * NS_PER_MS, 600 * NS_PER_S, 0, NS_PER_S);
    ok(!zero_wnd.dead, "window: a closed peer window never kills");

    paused.paused = true;
    run_wire(&paused, 1250 * NS_PER_MS, 600 * NS_PER_S, 0, NS_PER_S);
    ok(!paused.dead, "pause: our own delivery pause never kills");
}

static void test_moved_una_blocks_death(void)
{
    struct wire w = stalled_wire();

    /* Half the share acknowledged, packets_out unchanged: progress alone
     * ends the pause on this pass. */
    w.una += 512;
    poll_once(&w, 1250 * NS_PER_MS, NS_PER_S);
    ok(!w.dead, "una: any snd_una movement is progress, not death");
    ok(!w.stalled, "una: progress clears the pause");
}

int main(void)
{
    test_exhausted_never_dials();
    test_generation_supersedes();
    test_replacement_closes_old_socket();
    test_peers_render();
    test_carrier_transport();
    test_stall_bound();
    test_late_ack_is_not_death();
    test_dead_carrier_dies();
    test_idle_never_stalls();
    test_wire_movement_blocks_death();
    test_retrans_exhaustion();
    test_window_and_pause_gates();
    test_moved_una_blocks_death();

    if (failures)
    {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_carrier_policy: %d checks passed\n", checks);
    return 0;
}
