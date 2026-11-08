/* tbt_portmap.c - pure port-space bitmap arithmetic, host and kernel.
 *
 * See tbt_portmap.h. The scan is linear over the requested window, at most
 * 65536 bits (8 KiB of bitmap); a session allocates at most 256 carriers and
 * a replacement path allocates one at a time, so the linear scan is below the
 * noise of a socket creation. Keeping it pure is what lets the host test
 * prove that a 4096-port window really hands out 256 distinct ports, that two
 * overlapping windows never collide, and that a freed port is reused
 * lowest-first.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#else
#include <errno.h>
#include <stddef.h>
#endif

#include "tbt_portmap.h"

#ifndef BITS_PER_LONG
#define BITS_PER_LONG (8 * (int)sizeof(unsigned long))
#endif

unsigned int tbt_portmap_words(void)
{
    return (TBT_PORTMAP_PORTS + (unsigned int)BITS_PER_LONG - 1) /
           (unsigned int)BITS_PER_LONG;
}

bool tbt_portmap_fits(u16 lo, u16 hi, unsigned int need)
{
    if (!lo || lo > hi)
        return false;
    return (unsigned int)(hi - lo) + 1u >= need;
}

int tbt_portmap_window(u16 block_lo, u16 block_hi,
                       u16 global_lo, u16 global_hi, u16 *lo, u16 *hi)
{
    if (!lo || !hi)
        return -EINVAL;
    if (block_lo || block_hi)
    {
        if (!block_lo || block_lo > block_hi)
            return -EINVAL;
        *lo = block_lo;
        *hi = block_hi;
        return 0;
    }
    if (!global_lo || global_lo > global_hi)
        return -EINVAL;
    *lo = global_lo;
    *hi = global_hi;
    return 0;
}

static int tbt_portmap_test(const unsigned long *used, unsigned int bit)
{
    return (used[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1ul;
}

static void tbt_portmap_set(unsigned long *used, unsigned int bit)
{
    used[bit / BITS_PER_LONG] |= 1ul << (bit % BITS_PER_LONG);
}

static void tbt_portmap_clear(unsigned long *used, unsigned int bit)
{
    used[bit / BITS_PER_LONG] &= ~(1ul << (bit % BITS_PER_LONG));
}

int tbt_portmap_alloc(unsigned long *used, u16 lo, u16 hi, u16 *port)
{
    unsigned int p;

    if (!used || !port || !lo || lo > hi)
        return -EINVAL;
    for (p = lo; p <= hi; p++)
    {
        if (tbt_portmap_test(used, p))
            continue;
        tbt_portmap_set(used, p);
        *port = (u16)p;
        return 0;
    }
    return -ENOSPC;
}

void tbt_portmap_free(unsigned long *used, u16 port)
{
    if (!used || !port)
        return;
    tbt_portmap_clear(used, port);
}
