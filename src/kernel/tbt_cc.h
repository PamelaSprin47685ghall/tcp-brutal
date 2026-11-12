/* tbt_cc.h - the carrier congestion control's narrow interface.
 *
 * tbt_carrier.c pushes per-socket context in (the tunnel's rate cap and the
 * loss the FEC feedback path measured) and reads a snapshot out for /proc.
 * The CC itself never holds a carrier or session pointer: a pointer would
 * outlive the socket's attachment, and a socket can be released after the
 * carrier object it belonged to. State lives in the socket's CA private
 * block, which the kernel frees with the socket.
 *
 * The congestion-control name is TBT_CAR_CC_NAME from
 * tbt_carrier_policy.h, the same literal the host test pins.
 */
#ifndef _TBT_CC_H
#define _TBT_CC_H

#include <linux/types.h>

struct sock;

/* One carrier's current view, copied out for /proc/net/tbtun/stats. */
struct tbt_cc_info
{
    u64 max_bw_bps;  /* windowed-max delivery estimate */
    u64 pacing_bps;  /* last rate written to sk_pacing_rate, probe gain included */
    u32 clamp_hits;  /* ssthresh calls raised above the reno half */
    u32 loss_ppm;    /* effective p: pinned value or the pushed measurement */
    bool hold;       /* the carrier's idle hold owns the window */
};

int tbt_cc_module_init(void);
void tbt_cc_module_exit(void);

/* Push tunnel context. rate_cap_bps is this carrier's share of the tunnel
 * rate (the session divides its aggregate by the stripe width). No-op unless
 * sk currently runs this CC. */
void tbt_cc_set_tunnel(struct sock *sk, u32 rate_cap_bps, u32 loss_ppm);
void tbt_cc_set_hold(struct sock *sk, bool hold);

/* Copy the socket's CC state. False when sk does not run this CC. */
bool tbt_cc_snapshot(struct sock *sk, struct tbt_cc_info *out);

#endif /* _TBT_CC_H */
