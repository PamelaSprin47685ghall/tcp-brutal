/* tbt_portmap.c - pure port-block bitmap arithmetic, host and kernel.
 *
 * See tbt_portmap.h. The scan is linear over at most 65536 bits (8 KiB of
 * bitmap); a session allocates at most 256 carriers and a replacement path
 * allocates one at a time, so the linear scan is below the noise of a socket
 * creation. Keeping it pure is what lets the host test prove that a 4096-port
 * block really hands out 256 distinct ports and then reuses them after free.
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

unsigned int tbt_portmap_words(u16 lo, u16 hi)
{
    unsigned int n;

    if (!lo || lo > hi)
        return 0;
    n = (unsigned int)hi - lo + 1;
    return (n + BITS_PER_LONG - 1) / BITS_PER_LONG;
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
    unsigned int n;
    unsigned int bit;

    if (!used || !port || !lo || lo > hi)
        return -EINVAL;
    n = (unsigned int)hi - lo + 1;
    for (bit = 0; bit < n; bit++)
    {
        if (tbt_portmap_test(used, bit))
            continue;
        tbt_portmap_set(used, bit);
        *port = (u16)(lo + bit);
        return 0;
    }
    return -ENOSPC;
}

void tbt_portmap_free(unsigned long *used, u16 lo, u16 hi, u16 port)
{
    unsigned int bit;

    if (!used || !lo || lo > hi || port < lo || port > hi)
        return;
    bit = (unsigned int)port - lo;
    tbt_portmap_clear(used, bit);
}
