/* test_carrier_policy.c - host contract for the carrier admission rules.
 *
 * The three rules live in tbtun/tbt_carrier_policy.h, compiled by the module
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

int main(void)
{
    test_exhausted_never_dials();
    test_generation_supersedes();
    test_replacement_closes_old_socket();
    test_peers_render();
    test_carrier_transport();

    if (failures)
    {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_carrier_policy: %d checks passed\n", checks);
    return 0;
}
