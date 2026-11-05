/* test_anchor.c - host specification test for the far side's HELLO admission.
 *
 * This is a model, not the kernel code: it runs with no kernel headers and no
 * module.  It fixes the ordering and the counters that tbt_rx_hello() and
 * tbt_session_join() must implement (review 16 + F8, plan §6):
 *
 *   source check -> nonce -> set nonce peer -> join
 *
 * Kernel mapping (review/analysis/p1-misc-patch-plan.md §6):
 *
 *   model step                          kernel site
 *   ----------------------------------  ----------------------------------------
 *   anchor_init(peer, nonce, pinned)    tbt_session_create: cfg.nonce/nonce_set
 *                                       -> sess->nonce, nonce_pinned/nonce_set;
 *                                       cfg.peer_addr -> nonce_peer_addr/
 *                                       nonce_peer_set (optional pre-seed)
 *   anchor_admit source check           tbt_rx_hello far branch, before nonce:
 *                                       nonce_peer_set && src != nonce_peer_addr
 *                                       -> join_src_reject++; return -EPROTO
 *   pinned memcmp                       tbt_rx_hello nonce_pinned branch:
 *                                       session_join_bad++; return -EPROTO
 *   adoption (!nonce_set)               tbt_rx_hello: cfg.peer_addr mismatch ->
 *                                       join_nonce_reject++; else adopt
 *   nonce_set memcmp                    tbt_rx_hello else branch:
 *                                       session_join_bad++; return -EPROTO
 *   set nonce peer                      tbt_rx_hello after the nonce checks:
 *                                       nonce_peer_addr = src;
 *                                       nonce_peer_set = true
 *   occupant check                      tbt_session_join: live old ->
 *                                       session_join_dup++; return -EALREADY
 *   accept                              tbt_session_join: sess->car[index] = c
 *
 * Invariants this test pins:
 *   - A source mismatch is rejected before the nonce is examined, so a replayed
 *     nonce from a new source increments join_src_reject, never join_bad.
 *   - Only a HELLO that passes the nonce check may set nonce_peer_addr /
 *     nonce_peer_set; a rejected HELLO never fixes the peer.
 *   - A configured peer= pre-seeds nonce_peer_addr/nonce_peer_set at session
 *     creation; without one the first authenticated source becomes the peer
 *     (trust-on-first-use).
 *   - join_src_reject, join_dup, join_bad and join_nonce_reject are disjoint:
 *     one rejection increments exactly one of them.  A nonce error never
 *     increments join_src_reject.
 *   - A dead occupant can be replaced at the same index; a live one is
 *     join_dup (the kernel's -EALREADY).
 *
 * The addresses mirror adversarial/adv_probe.c: 127.0.0.1 is the legitimate
 * near; 127.0.0.2 replays the nonce in far.srcaddr_free.
 *
 * Build/run: make host-test
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NONCE_LEN 16
#define MAX_INDEX 16

static int checks;
static int failures;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "",
           detail ? detail : "");
}

enum admit_result
{
    ADMIT_ACCEPT = 0,
    ADMIT_SRC,    /* source peer mismatch: join_src_reject */
    ADMIT_BAD,    /* nonce mismatch: join_bad or join_nonce_reject */
    ADMIT_DUP,    /* live occupant at the index: join_dup / -EALREADY */
    ADMIT_INDEX,  /* index out of range */
};

struct counters
{
    unsigned join_src_reject;
    unsigned join_dup;
    unsigned join_bad;
    unsigned join_nonce_reject;
};

struct delta
{
    unsigned join_src_reject;
    unsigned join_dup;
    unsigned join_bad;
    unsigned join_nonce_reject;
};

struct anchor_state
{
    uint32_t cfg_peer_addr; /* 0 = no configured peer */
    uint8_t nonce[NONCE_LEN];
    bool nonce_set;    /* a nonce is fixed (pinned or adopted) */
    bool nonce_pinned; /* nonce= was configured */
    uint32_t nonce_peer_addr; /* far: the only accepted near source */
    bool nonce_peer_set;      /* nonce_peer_addr is valid */
    bool occupant_live[MAX_INDEX];
    struct counters c;
};

static const uint8_t nonce_a[NONCE_LEN] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

static const uint8_t nonce_b[NONCE_LEN] = {
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

/* 127.0.0.1 is the legitimate near; 127.0.0.2 is the far.srcaddr_free
 * replay source.  Both are loopback addresses, as in adversarial/adv_probe.c. */
static const uint32_t src_a = 0x7f000001u;
static const uint32_t src_b = 0x7f000002u;

static const char *result_name(enum admit_result r)
{
    switch (r)
    {
    case ADMIT_ACCEPT:
        return "ACCEPT";
    case ADMIT_SRC:
        return "SRC";
    case ADMIT_BAD:
        return "BAD";
    case ADMIT_DUP:
        return "DUP";
    case ADMIT_INDEX:
        return "INDEX";
    }
    return "?";
}

static void anchor_init(struct anchor_state *st, uint32_t peer,
                        const uint8_t *nonce, bool pinned)
{
    memset(st, 0, sizeof(*st));
    st->cfg_peer_addr = peer;
    if (nonce)
    {
        memcpy(st->nonce, nonce, NONCE_LEN);
        st->nonce_set = true;
        st->nonce_pinned = pinned;
    }
    if (peer)
    {
        /* A configured far peer= is the peer from session creation. */
        st->nonce_peer_addr = peer;
        st->nonce_peer_set = true;
    }
}

static struct counters snap(const struct anchor_state *st)
{
    return st->c;
}

/* The far-side admission order.  Keep this in lockstep with the kernel mapping
 * above: a source mismatch must be rejected before the nonce is read, and the
 * nonce peer must be set only after the nonce check passes. */
static enum admit_result anchor_admit(struct anchor_state *st, uint32_t src,
                                      const uint8_t *nonce, unsigned index)
{
    /* The kernel rejects an invalid path_id before it looks at the nonce or
     * the peer, so an invalid index must not set the peer either. */
    if (index >= MAX_INDEX)
        return ADMIT_INDEX;

    /* 1. A fixed peer: any source change is rejected first, free index
     * included.  This is far.srcaddr_free. */
    if (st->nonce_peer_set && src != st->nonce_peer_addr)
    {
        st->c.join_src_reject++;
        return ADMIT_SRC;
    }

    /* 2. Nonce: pinned, adopt-once (TOFU), or match. */
    if (st->nonce_pinned)
    {
        if (memcmp(nonce, st->nonce, NONCE_LEN))
        {
            st->c.join_bad++;
            return ADMIT_BAD;
        }
    }
    else if (!st->nonce_set)
    {
        /* A configured peer= must match its source before adoption.  This is
         * the fallback path when the peer was not pre-seeded. */
        if (st->cfg_peer_addr && src != st->cfg_peer_addr)
        {
            st->c.join_nonce_reject++;
            return ADMIT_BAD;
        }
        memcpy(st->nonce, nonce, NONCE_LEN);
        st->nonce_set = true;
    }
    else if (memcmp(nonce, st->nonce, NONCE_LEN))
    {
        st->c.join_bad++;
        return ADMIT_BAD;
    }

    /* 3. The first HELLO that passed the nonce check fixes the peer. */
    if (!st->nonce_peer_set)
    {
        st->nonce_peer_addr = src;
        st->nonce_peer_set = true;
    }

    /* 4. Join: only a live occupant at the same index is a duplicate. */
    if (st->occupant_live[index])
    {
        st->c.join_dup++;
        return ADMIT_DUP;
    }
    st->occupant_live[index] = true;
    return ADMIT_ACCEPT;
}

static void check_delta(const char *what, const char *which, unsigned got,
                        unsigned want)
{
    char detail[96];

    (void)snprintf(detail, sizeof(detail), "%s: got %u want %u", which, got,
                   want);
    ok(got == want, what, detail);
}

static void expect(const char *what, struct anchor_state *st, uint32_t src,
                   const uint8_t *nonce, unsigned index,
                   enum admit_result want, struct delta want_delta)
{
    struct counters before = snap(st);
    struct counters after;
    enum admit_result got = anchor_admit(st, src, nonce, index);

    after = snap(st);
    ok(got == want, what, result_name(got));
    check_delta(what, "join_src_reject",
                after.join_src_reject - before.join_src_reject,
                want_delta.join_src_reject);
    check_delta(what, "join_dup", after.join_dup - before.join_dup,
                want_delta.join_dup);
    check_delta(what, "join_bad", after.join_bad - before.join_bad,
                want_delta.join_bad);
    check_delta(what, "join_nonce_reject",
                after.join_nonce_reject - before.join_nonce_reject,
                want_delta.join_nonce_reject);
}

int main(void)
{
    struct anchor_state st;
    struct delta zero = { 0, 0, 0, 0 };

    /* 0. An invalid index is rejected before the nonce or the peer check. */
    anchor_init(&st, 0, NULL, false);
    expect("invalid index", &st, src_a, nonce_a, MAX_INDEX, ADMIT_INDEX, zero);
    ok(!st.nonce_peer_set && !st.nonce_set,
       "invalid index does not adopt or set a peer", NULL);

    /* 1. far.hello_garbage: the first HELLO from 127.0.0.1 adopts the nonce
     * and fixes the peer. */
    anchor_init(&st, 0, NULL, false);
    expect("far.hello_garbage first accept", &st, src_a, nonce_a, 0,
           ADMIT_ACCEPT, zero);
    ok(st.nonce_peer_set && st.nonce_peer_addr == src_a,
       "first HELLO fixes nonce_peer_addr", NULL);
    ok(st.nonce_set && !st.nonce_pinned, "first HELLO adopts nonce", NULL);
    ok(!memcmp(st.nonce, nonce_a, NONCE_LEN), "adopted nonce value", NULL);

    /* 2. Multi-carrier: the same source and nonce may join free indices. */
    expect("same source free index 1", &st, src_a, nonce_a, 1,
           ADMIT_ACCEPT, zero);
    expect("same source free index 2", &st, src_a, nonce_a, 2,
           ADMIT_ACCEPT, zero);
    ok(st.c.join_src_reject == 0, "same-source joins do not reject", NULL);

    /* 3. far.srcaddr_free: 127.0.0.2 replays the same nonce on a free index;
     * the peer check rejects it before the nonce is compared. */
    expect("far.srcaddr_free replayed nonce on free index", &st, src_b,
           nonce_a, 3, ADMIT_SRC, (struct delta){ 1, 0, 0, 0 });
    ok(st.c.join_src_reject == 1 && st.c.join_bad == 0,
       "far.srcaddr_free increments only join_src_reject", NULL);

    /* 4. A wrong nonce from the fixed peer is a nonce error, not a source
     * error: join_src_reject stays unchanged. */
    expect("anchored peer wrong nonce", &st, src_a, nonce_b, 3, ADMIT_BAD,
           (struct delta){ 0, 0, 1, 0 });
    ok(st.c.join_src_reject == 1 && st.c.join_bad == 1,
       "nonce error leaves join_src_reject unchanged", NULL);

    /* 5. Source is checked before the nonce: a new source with a wrong nonce
     * still increments only join_src_reject. */
    expect("source before nonce", &st, src_b, nonce_b, 4, ADMIT_SRC,
           (struct delta){ 1, 0, 0, 0 });
    ok(st.c.join_src_reject == 2 && st.c.join_bad == 1,
       "source rejection does not increment join_bad", NULL);

    /* 6. far.dup_index: the fixed peer reuses a live index; the kernel
     * returns -EALREADY and counts join_dup. */
    expect("far.dup_index live occupant", &st, src_a, nonce_a, 0,
           ADMIT_DUP, (struct delta){ 0, 1, 0, 0 });
    ok(st.c.join_src_reject == 2, "far.dup_index is not a source rejection",
       NULL);

    /* 7. A dead occupant at the same index may be replaced. */
    st.occupant_live[0] = false;
    expect("dead occupant replaced", &st, src_a, nonce_a, 0,
           ADMIT_ACCEPT, zero);

    /* 8. nonce_pinned: a wrong first HELLO is refused and fixes no peer. */
    anchor_init(&st, 0, nonce_a, true);
    expect("pinned wrong first HELLO", &st, src_a, nonce_b, 0, ADMIT_BAD,
           (struct delta){ 0, 0, 1, 0 });
    ok(!st.nonce_peer_set,
       "pinned bad HELLO does not set nonce_peer_addr", NULL);
    ok(st.c.join_src_reject == 0,
       "pinned nonce error leaves join_src_reject at zero", NULL);

    /* 9. nonce_pinned: the correct first HELLO fixes the peer. */
    expect("pinned correct first HELLO", &st, src_a, nonce_a, 0,
           ADMIT_ACCEPT, zero);
    ok(st.nonce_peer_set && st.nonce_peer_addr == src_a,
       "pinned correct HELLO fixes nonce_peer_addr", NULL);

    /* 10. nonce_pinned: the correct nonce from 127.0.0.2 on a free index is
     * still refused; only the source check can stop this replay. */
    expect("pinned correct nonce from wrong source", &st, src_b, nonce_a, 1,
           ADMIT_SRC, (struct delta){ 1, 0, 0, 0 });
    /* case 8 already counted one join_bad; the delta above is the assertion
     * that this source rejection did not add another. */
    ok(st.c.join_src_reject == 1 && st.c.join_bad == 1,
       "pinned wrong source increments only join_src_reject", NULL);

    /* 11. nonce_pinned: the fixed peer may open another free carrier. */
    expect("pinned same source free index", &st, src_a, nonce_a, 2,
           ADMIT_ACCEPT, zero);

    /* 12. cfg.peer_addr != 0 pre-seeds the peer at session creation: the
     * first connection from another source is refused before adoption. */
    anchor_init(&st, src_a, NULL, false);
    ok(st.nonce_peer_set && st.nonce_peer_addr == src_a,
       "configured peer pre-seeds nonce_peer_addr", NULL);
    expect("configured peer wrong first source", &st, src_b, nonce_a, 0,
           ADMIT_SRC, (struct delta){ 1, 0, 0, 0 });
    ok(!st.nonce_set && st.nonce_peer_addr == src_a,
       "wrong first source neither adopts nor moves the peer", NULL);

    /* 13. cfg.peer_addr != 0: the configured source adopts the nonce. */
    expect("configured peer correct first source", &st, src_a, nonce_a, 0,
           ADMIT_ACCEPT, zero);
    ok(st.nonce_set && st.nonce_peer_addr == src_a,
       "configured peer adopts on the configured source", NULL);

    /* 14. cfg.peer_addr + nonce_pinned: a wrong nonce from the right source
     * is join_bad and leaves the peer intact. */
    anchor_init(&st, src_a, nonce_a, true);
    expect("configured peer pinned wrong nonce", &st, src_a, nonce_b, 0,
           ADMIT_BAD, (struct delta){ 0, 0, 1, 0 });
    ok(st.nonce_peer_addr == src_a && st.c.join_src_reject == 0,
       "pinned nonce error leaves the configured peer intact", NULL);

    /* 15. cfg.peer_addr + nonce_pinned: a correct nonce from the configured
     * source may join a free carrier. */
    expect("configured peer pinned free carrier", &st, src_a, nonce_a, 1,
           ADMIT_ACCEPT, zero);

    /* 16. Adoption fallback: cfg.peer_addr is set but the peer was not
     * pre-seeded (the pre-P2 path); the adoption check still refuses a wrong
     * source and counts join_nonce_reject, not join_src_reject. */
    anchor_init(&st, src_a, NULL, false);
    st.nonce_peer_set = false; /* the fallback path before the peer is set */
    expect("adoption source fallback", &st, src_b, nonce_a, 0, ADMIT_BAD,
           (struct delta){ 0, 0, 0, 1 });
    ok(!st.nonce_peer_set,
       "adoption fallback reject does not set nonce_peer_addr", NULL);

    /* 17. Counter separation: source rejections and nonce errors never
     * contaminate each other. */
    anchor_init(&st, 0, nonce_a, true);
    expect("counter separation first accept", &st, src_a, nonce_a, 0,
           ADMIT_ACCEPT, zero);
    expect("counter separation source reject", &st, src_b, nonce_a, 1,
           ADMIT_SRC, (struct delta){ 1, 0, 0, 0 });
    expect("counter separation nonce error", &st, src_a, nonce_b, 2,
           ADMIT_BAD, (struct delta){ 0, 0, 1, 0 });
    ok(st.c.join_src_reject == 1 && st.c.join_dup == 0 &&
           st.c.join_bad == 1 && st.c.join_nonce_reject == 0,
       "source and nonce counters stay disjoint", NULL);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
