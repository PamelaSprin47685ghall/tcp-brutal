/* tbt_portmap.h - pure bitmap allocator for TCP source ports.
 *
 * One bit per port number: bit N is port N. A block [lo, hi] is a search
 * window over that one bitmap, not a private bitmap. Two tunnels whose
 * windows overlap therefore share capacity and can never be handed the same
 * live source port. The kernel keeps the bitmap behind tbt_ports.lock; this
 * file owns only the bit arithmetic so the host test can exercise the same
 * code with a plain unsigned long array. No kernel types beyond tbt_uapi.h,
 * no allocation, no locks, no globals.
 */
#ifndef _TBT_PORTMAP_H
#define _TBT_PORTMAP_H

#include "tbt_uapi.h"

/* The whole IPv4 TCP source port space, one bit each (8 KiB). Port 0 is
 * never allocated: a source port of 0 is not a port. */
#define TBT_PORTMAP_PORTS 65536u

/* Words a full port-space bitmap needs (1024 on a 64-bit build). */
unsigned int tbt_portmap_words(void);

/* True when [lo, hi] is a valid block with room for `need` live ports. A near
 * tunnel needs one port per carrier slot: n + m, or `carriers` with FEC. */
bool tbt_portmap_fits(u16 lo, u16 hi, unsigned int need);

/* Resolve a tunnel's allocation window: its own block when set, else the
 * global block. Returns 0, or -EINVAL when the chosen block is unusable. */
int tbt_portmap_window(u16 block_lo, u16 block_hi,
                       u16 global_lo, u16 global_hi, u16 *lo, u16 *hi);

/* Allocate the lowest free port in [lo, hi]. Returns 0 and fills *port, or
 * -EINVAL for a bad block, or -ENOSPC when every port in it is in use. The
 * caller owns the bitmap and must serialize calls. */
int tbt_portmap_alloc(unsigned long *used, u16 lo, u16 hi, u16 *port);

/* Release `port`. Port 0, or an already free port, is a no-op. */
void tbt_portmap_free(unsigned long *used, u16 port);

#endif /* _TBT_PORTMAP_H */
