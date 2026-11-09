/* tbt_map.h - the port-mapping table shared by the parser and the data path.
 *
 * One tunnel carries one N+M carrier session for several listen/target port
 * mappings instead of one session per port. Slot 0 is the original
 * listen=/target= pair; map= entries fill slots 1..TBT_MAP_MAX-1.
 *
 * The far side is the security boundary: an OPEN names a target port, and it
 * may only reach a port its own configuration names in target= or map=.
 * tbt_map_target_allowed() is that whole check, kept pure and host-testable
 * so the module and the test cannot disagree about it.
 *
 * No locks, no allocation, no kernel-only type: this compiles into the module
 * and into host/test_map.c from the same source.
 */
#ifndef _TBT_MAP_H
#define _TBT_MAP_H

#include "tbt_parse.h"

/* Target port for one mapping slot, host order. Slot 0 returns the configured
 * target= (0 when a near tunnel left it out, meaning "the far side's
 * default"). An out-of-range slot returns 0 and is never a valid target. */
u16 tbt_map_target_for_slot(const struct tbt_cfg *c, u8 slot);

/* True when `port` may be reached through this tunnel. Port 0 is the wire
 * spelling of "the far side's default target" and is always allowed; every
 * other value must equal target= or one map= target. */
bool tbt_map_target_allowed(const struct tbt_cfg *c, u16 port);

/* Structural and role-aware checks the parser and tbt_cfg_sane() share:
 * slot count, nonzero ports, and for a near tunnel the listen ports must be
 * distinct and must not collide with listen=. Returns 0 or -EINVAL. */
int tbt_map_check(const struct tbt_cfg *c);

#endif /* _TBT_MAP_H */
