/* tbt_map.h - the port-mapping table shared by the parser and the data path.
 *
 * One tunnel carries one N+M carrier session for several listen/target port
 * mappings instead of one session per port. Slot 0 is the original
 * listen=/target= pair; map= entries fill slots 1..TBT_MAP_MAX-1.
 *
 * The far side is the security boundary: an OPEN names a mapping slot and a
 * target port, and it may only reach the port its own configuration gives
 * that slot. tbt_map_slot_target() is that whole check, kept pure and
 * host-testable so the module and the test cannot disagree about it.
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

/* Resolve one OPEN's routing instruction against the local table. `wire_port`
 * 0 means "the configured port for this slot"; any other value must equal it.
 * On success stores the host-order destination in *port (when non-NULL) and
 * returns true. A slot the table does not have, or a port that does not
 * match, returns false. */
bool tbt_map_slot_target(const struct tbt_cfg *c, u8 slot, u16 wire_port,
                         u16 *port);

/* Structural and role-aware checks the parser and tbt_cfg_sane() share:
 * slot count, nonzero ports, and for a near tunnel the listen ports must be
 * distinct and must not collide with listen=. Returns 0 or -EINVAL. */
int tbt_map_check(const struct tbt_cfg *c);

#endif /* _TBT_MAP_H */
