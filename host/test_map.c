/* test_map.c - host conformance test for the multi-port mapping table.
 *
 * Links tbtun/tbt_map.c (and the parser sources it includes), the same code
 * the module uses. No root, no module, no kernel headers.
 *
 * The test pins the two runtime decisions that must never drift:
 *   - which target port a near slot sends for an accepted connection;
 *   - which target ports a far tunnel lets an OPEN reach.
 * It also pins the structural checks tbt_cfg_sane() re-runs, and the port
 * byte-order helpers both planes use.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tbt_map.h"
#include "tbt_parse.h"
#include "tbt_proto.h"

static int failures;
static int checks;

static void ok(int cond, const char *what, const char *detail)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
}

static void add_map(struct tbt_cfg *c, u16 listen_port, u16 target_port)
{
    c->maps[c->nmap].listen_port = listen_port;
    c->maps[c->nmap].target_port = target_port;
    c->nmap++;
}

static void near_cfg(struct tbt_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->role = TBT_ROLE_NEAR;
    c->listen_port = tbt_port_be16(9000);
    c->peer_port = tbt_port_be16(443);
    add_map(c, 9001, 80);
    add_map(c, 9002, 443);
}

static void far_cfg(struct tbt_cfg *c)
{
    memset(c, 0, sizeof(*c));
    c->role = TBT_ROLE_FAR;
    c->listen_port = tbt_port_be16(7000);
    c->target_port = tbt_port_be16(8080);
    add_map(c, 9001, 80);
    add_map(c, 9002, 443);
}

static void test_ports(void)
{
    u16 ports[] = { 1, 80, 8080, 443, 65535 };
    size_t i;
    u8 body[TBT_OPEN_BODY_LEN];

    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
    {
        u16 host = ports[i];
        u16 net = tbt_port_be16(host);
        const u8 *b = (const u8 *)&net;

        ok(tbt_port_host16(net) == host, "port byte-order round-trip", NULL);
        ok(b[0] == (u8)(host >> 8) && b[1] == (u8)host,
           "network-order bytes are big-endian in memory", NULL);
    }
    ok(tbt_port_host16(tbt_port_be16(0)) == 0, "port 0 round-trips", NULL);

    /* The OPEN body contract: 32 bitmap bytes then a little-endian port. */
    memset(body, 0, sizeof(body));
    ok(TBT_OPEN_BODY_LEN == 34 && TBT_OPEN_TARGET_PORT == 32,
       "OPEN body is bitmap + u16", NULL);
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, 443);
    ok(tbt_le16(body + TBT_OPEN_TARGET_PORT) == 443,
       "OPEN target port round-trips little-endian", NULL);
    ok(body[32] == 0xbb && body[33] == 0x01,
       "443 is encoded as bb 01 on the wire", NULL);
}

static void test_slot_targets(void)
{
    struct tbt_cfg near, far;

    near_cfg(&near);
    ok(tbt_map_target_for_slot(&near, 0) == 0,
       "near slot 0 without target= names the far default", NULL);
    ok(tbt_map_target_for_slot(&near, 1) == 80, "near slot 1 maps to 80", NULL);
    ok(tbt_map_target_for_slot(&near, 2) == 443, "near slot 2 maps to 443", NULL);
    ok(tbt_map_target_for_slot(&near, 3) == 0, "out-of-range slot has no target", NULL);
    ok(tbt_map_target_for_slot(&near, 255) == 0, "slot 255 has no target", NULL);
    ok(tbt_map_target_for_slot(NULL, 0) == 0, "NULL cfg has no target", NULL);

    far_cfg(&far);
    ok(tbt_map_target_for_slot(&far, 0) == 8080,
       "far slot 0 resolves target=", NULL);
    ok(tbt_map_target_for_slot(&far, 1) == 80, "far slot 1 resolves its map", NULL);
}

static void test_authorization(void)
{
    struct tbt_cfg far, bare;

    far_cfg(&far);
    ok(tbt_map_target_allowed(&far, 0), "0 means the default target", NULL);
    ok(tbt_map_target_allowed(&far, 8080), "target= is allowed", NULL);
    ok(tbt_map_target_allowed(&far, 80), "map target 80 is allowed", NULL);
    ok(tbt_map_target_allowed(&far, 443), "map target 443 is allowed", NULL);
    ok(!tbt_map_target_allowed(&far, 1), "port 1 is refused", NULL);
    ok(!tbt_map_target_allowed(&far, 22), "port 22 is refused", NULL);
    ok(!tbt_map_target_allowed(&far, 81), "a neighbour of an allowed port is refused", NULL);
    ok(!tbt_map_target_allowed(&far, 65535), "port 65535 is refused", NULL);
    ok(!tbt_map_target_allowed(NULL, 80), "NULL cfg allows nothing but its default", NULL);

    memset(&bare, 0, sizeof(bare));
    bare.role = TBT_ROLE_FAR;
    bare.target_port = tbt_port_be16(9000);
    ok(tbt_map_target_allowed(&bare, 0), "no map: 0 still names default", NULL);
    ok(tbt_map_target_allowed(&bare, 9000), "no map: target= is allowed", NULL);
    ok(!tbt_map_target_allowed(&bare, 9001), "no map: another port is refused", NULL);
}

static void test_check(void)
{
    struct tbt_cfg c;

    near_cfg(&c);
    ok(tbt_map_check(&c) == 0, "near mappings are consistent", NULL);

    near_cfg(&c);
    c.maps[0].listen_port = 9000;
    ok(tbt_map_check(&c) == -EINVAL, "map listen colliding with listen= is refused", NULL);

    near_cfg(&c);
    c.maps[1].listen_port = 9001;
    ok(tbt_map_check(&c) == -EINVAL, "duplicate map listen port is refused", NULL);

    near_cfg(&c);
    c.maps[0].target_port = 0;
    ok(tbt_map_check(&c) == -EINVAL, "zero target port is refused", NULL);

    near_cfg(&c);
    c.maps[0].listen_port = 0;
    ok(tbt_map_check(&c) == -EINVAL, "zero listen port is refused", NULL);

    near_cfg(&c);
    c.nmap = TBT_MAP_MAX;
    ok(tbt_map_check(&c) == -EINVAL, "more slots than TBT_MAP_MAX is refused", NULL);

    far_cfg(&c);
    c.maps[1].listen_port = c.maps[0].listen_port;
    ok(tbt_map_check(&c) == 0,
       "far ignores the map listen half, duplicates included", NULL);

    ok(tbt_map_check(NULL) == -EINVAL, "NULL cfg is refused", NULL);
}

int main(void)
{
    test_ports();
    test_slot_targets();
    test_authorization();
    test_check();

    printf("test_map: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
