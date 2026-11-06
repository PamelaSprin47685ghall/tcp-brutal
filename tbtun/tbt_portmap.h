/* tbt_portmap.h - pure bitmap allocator for the global carrier port block.
 *
 * The kernel keeps the bitmap behind tbt_ports.lock; this file owns only the
 * bit arithmetic so the host test can exercise the same code with a plain
 * unsigned long array. No kernel types beyond tbt_uapi.h, no allocation, no
 * locks, no globals.
 */
#ifndef _TBT_PORTMAP_H
#define _TBT_PORTMAP_H

#include "tbt_uapi.h"

/* The production global block: 4096 ports (256 carriers x 16 rebuilds). It is
 * the tested default, not a hard cap; a larger operator block still works. */
#define TBT_PORTMAP_MAX 4096u

/* Number of unsigned long words a block needs. Returns 0 for an invalid
 * block (lo == 0 or lo > hi). */
unsigned int tbt_portmap_words(u16 lo, u16 hi);

/* Allocate the lowest free port in [lo, hi]. Returns 0 and fills *port, or
 * -EINVAL for a bad block, or -ENOSPC when every port is in use. The caller
 * owns the bitmap and must serialize calls. */
int tbt_portmap_alloc(unsigned long *used, u16 lo, u16 hi, u16 *port);

/* Release `port` if it is inside the block. A port outside the block, or a
 * double free, is a no-op. */
void tbt_portmap_free(unsigned long *used, u16 lo, u16 hi, u16 port);

#endif /* _TBT_PORTMAP_H */
