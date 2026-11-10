/* tbt_carrier_policy.h - pure carrier admission and visibility rules.
 *
 * The kernel module and host/test_carrier_policy.c compile this same header.
 * Keeping the three rules here means a host test proves the decision, not a
 * second copy of it: whether a dial may create a socket, whether a far-side
 * HELLO supersedes the slot occupant, and whether a peers row is still the
 * current generation.
 *
 * No kernel types beyond tbt_uapi.h. No state, no allocation, no locks.
 */
#ifndef _TBT_CARRIER_POLICY_H
#define _TBT_CARRIER_POLICY_H

#include "tbt_uapi.h"

/* 0.7.0 carrier transport: every subflow owns the same buffer floor and runs
 * BBR. The floor is 0.8 MiB — the kernel's own SO_*BUF path doubles a request
 * on the way to sk_sndbuf/sk_rcvbuf, but 0.7.0 assigns those fields directly,
 * so the byte count below is the whole buffer, not half of it. It must stay
 * inside the 0.75..1 MiB acceptance band (786,432 .. 1,048,576 B). */
#define TBT_CAR_SOCKBUF_PER 838861u

/* SOCK_SNDBUF_LOCK | SOCK_RCVBUF_LOCK from include/uapi/linux/socket.h.
 * Both must stay clear: either bit freezes sk_sndbuf/sk_rcvbuf and disables
 * the TCP send/receive autotuning that grows the buffer past the floor. */
#define TBT_CAR_SOCKBUF_LOCK_MASK 3u

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

#endif /* _TBT_CARRIER_POLICY_H */
