/* tbt_map.c - pure multi-port routing table. See tbt_map.h. */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#else
#include <errno.h>
#include <stddef.h>
#endif

#include "tbt_map.h"

u16 tbt_map_target_for_slot(const struct tbt_cfg *c, u8 slot)
{
    if (!c)
        return 0;
    if (!slot)
        return tbt_port_host16(c->target_port);
    if (slot > c->nmap || slot > TBT_MAP_MAX - 1)
        return 0;
    return c->maps[slot - 1].target_port;
}

bool tbt_map_target_allowed(const struct tbt_cfg *c, u16 port)
{
    unsigned int i;

    if (!c)
        return false;
    /* 0 means "the far side's target=": the pre-0.5.0 near side sent no
     * port at all, and that intent must stay valid. */
    if (!port)
        return true;
    if (port == tbt_port_host16(c->target_port))
        return true;
    for (i = 0; i < c->nmap && i < TBT_MAP_MAX - 1; i++)
        if (c->maps[i].target_port == port)
            return true;
    return false;
}

int tbt_map_check(const struct tbt_cfg *c)
{
    unsigned int i, j;

    if (!c || c->nmap > TBT_MAP_MAX - 1)
        return -EINVAL;
    for (i = 0; i < c->nmap; i++)
        if (!c->maps[i].listen_port || !c->maps[i].target_port)
            return -EINVAL;
    if (c->role != TBT_ROLE_NEAR)
        return 0;
    /* A near tunnel binds every listen port on the same address, so two equal
     * numbers cannot both exist, and slot 0 is not free either. */
    if (!c->listen_port)
        return -EINVAL;
    for (i = 0; i < c->nmap; i++)
    {
        if (c->maps[i].listen_port == tbt_port_host16(c->listen_port))
            return -EINVAL;
        for (j = 0; j < i; j++)
            if (c->maps[i].listen_port == c->maps[j].listen_port)
                return -EINVAL;
    }
    return 0;
}
