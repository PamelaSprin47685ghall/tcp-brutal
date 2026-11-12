/* tbt_carrier_policy.h - pure carrier admission, visibility and liveness rules.
 *
 * The kernel module and tests/unit/test_carrier_policy.c compile this same header.
 * Keeping the rules here means a host test proves the decision, not a
 * second copy of it: whether a dial may create a socket, whether a far-side
 * HELLO supersedes the slot occupant, whether a peers row is still the
 * current generation, and whether a paused carrier is dead.
 *
 * No kernel types beyond tbt_uapi.h. No state, no allocation, no locks.
 */
#ifndef _TBT_CARRIER_POLICY_H
#define _TBT_CARRIER_POLICY_H

#include "tbt_uapi.h"

/* 0.7.2 carrier transport: one 0.8 MiB buffer per direction on every
 * subflow, with TCP/BBR autotuning enabled. One subflow at 400 ms RTT and
 * 0.5 MB/s needs 200 KiB of payload BDP; an skb truesize near twice the
 * payload makes 0.8 MiB the working size. */
#define TBT_CAR_SOCKBUF_PER 838861u

/* SOCK_SNDBUF_LOCK | SOCK_RCVBUF_LOCK from include/uapi/linux/socket.h.
 * tbt_car_setsockbuf() clears both bits, so tcp_sndbuf_expand() and
 * tcp_rcv_space_adjust() may grow sk_sndbuf and sk_rcvbuf. */
#define TBT_CAR_SOCKBUF_LOCK_MASK 3u

/* Bytes for one direction of one carrier. The size does not depend on the
 * carrier count: each subflow carries its own BDP. */
static inline u32 tbt_car_sockbuf_of(u32 carriers)
{
    (void)carriers;
    return TBT_CAR_SOCKBUF_PER;
}

/* The one congestion control 0.7.0 asks for by name. The module and the host
 * test share the literal, so a rename cannot pass the test while the kernel
 * still requests the old name. */
#define TBT_CAR_CC_NAME "bbr"

/* A near carrier may create, bind and connect a socket only after
 * tbt_port_alloc() returned a real port with TBT_PORT_ALLOC_OK. A zero port
 * or any failure reason (EXHAUSTED/ENOSPC, NOMEM) must park the carrier for
 * a paced retry: letting sock_create_kern()/kernel_connect() run would take
 * an ephemeral source port from outside the operator's port block. */
static inline bool tbt_carrier_dial_ready(u16 local_port, bool alloc_failed)
{
    return local_port != 0 && !alloc_failed;
}

/* A far-side HELLO supersedes a live slot occupant when its four-bit peer
 * generation is equal (the near redialled the same carrier allocation after
 * its socket died) or lies in the forward half of the generation ring. Only
 * an older generation is a duplicate. Equal generations must replace: the
 * near does not advance the counter for a retry of the same carrier, and
 * refusing the retry leaves the far's half-open socket in the slot forever. */
static inline bool tbt_carrier_gen_supersedes(u8 newer, u8 older)
{
    u8 diff = (u8)((newer - older) & 0xf);

    return diff == 0 || diff < 8;
}

/* /proc/net/tbtun/peers renders only carriers that can still serve the
 * current generation. On the far side a stopping carrier is a socket the
 * near has already replaced; its row is stale until w_replace closes it, so
 * the view must drop it at once. The near side keeps its own stopping rows:
 * they are its replacement diagnostics. */
static inline bool tbt_carrier_row_visible(bool far, bool stopping)
{
    return !(far && stopping);
}

/* Stall-dead: when a paused FEC carrier counts as a loss.
 *
 * A FEC stall pauses a carrier instead of killing it: the shares it owed are
 * rebuilt from parity. Only a wire that shows no life at all may end the
 * pause with a loss, and the far side pays the most for that call: it cannot
 * dial, so killing one of its carriers makes the near side redial and
 * discards the shares in flight.
 *
 * The detection clock (5 x RTO, floored at 1 s) is deliberately short, so a
 * BBR probe cycle or one lost ACK does not keep a dead path in the stripe.
 * The backstop must not inherit that floor: 4 x 1 s is 4 s, which is inside
 * the RTO ladder of an ordinary multi-loss episode (0.2+0.4+0.8+1.6+3.2 =
 * 6.2 s for five failed attempts) and a seventh of the 30 s TCP keepalive
 * this design calls its first line of defence. The 30 s floor sits
 * behind both: seven RTO doublings without one ACK, and a half-open socket
 * still closes at the keepalive before the 300 s cap. */
#define TBT_STALL_DEAD_MIN_NS 30000000000ULL
#define TBT_STALL_DEAD_MAX_NS 300000000000ULL
/* tcp_retries2 defaults to 15; at that count TCP itself is about to give up,
 * so a wire that has not moved either way is dead. */
#define TBT_STALL_RETX_MAX 15

/* The observable state of one carrier pause, as the 50 ms poll samples it
 * without the socket lock. `silent_ns` counts from the pause, not from the
 * last ACK: an idle carrier never reaches this decision, because the poll
 * declares a stall only with packets_out > 0, and any snd_una movement
 * clears `stalled` on that same pass. */
struct tbt_stall_view {
    bool stalled;             /* c->stalled: the pump avoids this carrier */
    bool paused;              /* our own delivery queue is full */
    bool una_frozen;          /* sampled snd_una equals c->last_una */
    u32 snd_wnd;              /* peer's advertised window */
    u32 packets_out;          /* TCP segments on the wire, unacknowledged */
    u32 packets_out_at_stall; /* the same sample when the pause began */
    u32 retransmits;          /* icsk_retransmits */
    u64 silent_ns;            /* now - c->stall_since_ns */
};

/* Four detection windows, but never less than the keepalive horizon and
 * never more than the backstop cap. */
static inline u64 tbt_carrier_stall_bound_ns(u64 stall_ns)
{
    u64 bound = 4 * stall_ns;

    if (bound < TBT_STALL_DEAD_MIN_NS)
        bound = TBT_STALL_DEAD_MIN_NS;
    if (bound > TBT_STALL_DEAD_MAX_NS)
        bound = TBT_STALL_DEAD_MAX_NS;
    return bound;
}

/* The poll's stall-dead rule, shared with tests/unit/test_carrier_policy.c so the
 * test decides with the module's own code. `dead_ns` is
 * tbt_carrier_stall_bound_ns(stall_ns). */
static inline bool tbt_carrier_stall_is_dead(const struct tbt_stall_view *v,
                                             u64 dead_ns)
{
    if (!v->stalled || v->paused || !v->snd_wnd || !v->una_frozen)
        return false;
    /* Either TCP still owes exactly the packets it owed when the pause
     * began, or its retry count has reached the point where TCP itself
     * gives up. A wire that pushed more segments (packets_out grew, or
     * changed before any retransmission) is still moving; once TCP enters
     * retransmission (retransmits > 0), tcp_retrans_collapse() can shrink
     * packets_out while snd_una stays frozen, which must not defeat the
     * dead backstop. */
    if (v->retransmits < TBT_STALL_RETX_MAX)
    {
        if (v->packets_out > v->packets_out_at_stall)
            return false;
        if (v->retransmits == 0 && v->packets_out != v->packets_out_at_stall)
            return false;
    }
    return v->silent_ns > dead_ns;
}

#endif /* _TBT_CARRIER_POLICY_H */
