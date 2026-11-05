/* tbt_parse.c - the /proc/net/tbtun/config grammar, free of kernel state.
 *
 * Compiles into the module and into host/test_parse.c. Nothing here locks,
 * allocates, sleeps, or touches a procfs type: a line goes in, a struct or an
 * error comes out. tbt_procfs.c owns the plumbing that calls it.
 *
 * Canonical form (DESIGN.md §8.4): every scalar key is rendered explicitly,
 * defaults included, in a fixed order; the list key (srcaddrs) is rendered
 * only when set. Rendering then parsing yields an identical struct,
 * so `cat config > backup; cat backup > config` is a no-op.
 */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/stdarg.h>
#include <linux/string.h>
#else
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#endif

#include "tbt_parse.h"

#define TBT_U32_MAX 0xffffffffu
#define TBT_U64_MAX (~(u64)0)
#define TBT_REPLAY_MAX 268435456u /* max_replay_bytes, §8.4.2 */
#define TBT_REORDER_MAX 268435456u /* rx_reorder_bytes, same ceiling as replay */
#define TBT_WINDOW_MAX 268435456u  /* tx_window_bytes, same ceiling as replay */
#define TBT_STALL_MAX 3600000u     /* carrier_stall_ms, one hour */
#define TBT_INFLATE_MAX 1000000u  /* 1000.000 */

static int tbt_fail(struct tbt_parse_error *e, const char *key, const char *why)
{
    if (e)
    {
        e->key = key;
        e->why = why;
    }
    return TBT_PARSE_EINVAL;
}

/* IPv6 is a distinct refusal: the operator asked for a family this version
 * does not speak, and the errno says so (§8.3). */
static int tbt_fail_ipv6(struct tbt_parse_error *e)
{
    if (e)
    {
        e->key = NULL;
        e->why = "IPv6 is unsupported in v0; use an IPv4 literal";
    }
    return TBT_PARSE_EAFNOSUPPORT;
}

/* ------------------------------------------------------------------------- */
/* Primitives. No libc beyond string.h, so the host build and the kernel build
 * run the same code. */

static char *tbt_next_tok(char **pp)
{
    char *s = *pp;
    char *end;

    if (!s)
        return NULL;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
    {
        *pp = NULL;
        return NULL;
    }
    end = s;
    while (*end && *end != ' ' && *end != '\t')
        end++;
    if (*end)
    {
        *end = '\0';
        *pp = end + 1;
    }
    else
    {
        *pp = NULL;
    }
    return s;
}

/* Decimal, or 0x-prefixed hexadecimal. No sign, no empty string. */
static int tbt_parse_u64(const char *s, u64 *out)
{
    u64 v = 0;
    unsigned base = 10;

    if (!s || !*s)
        return -EINVAL;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        s += 2;
        if (!*s)
            return -EINVAL;
    }
    for (; *s; s++)
    {
        unsigned d;

        if (*s >= '0' && *s <= '9')
            d = (unsigned)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = (unsigned)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = (unsigned)(*s - 'A' + 10);
        else
            return -EINVAL;
        if (v > (TBT_U64_MAX - d) / base)
            return -ERANGE;
        v = v * base + d;
    }
    *out = v;
    return 0;
}

static int tbt_parse_u32(const char *s, u32 *out)
{
    u64 v;

    if (tbt_parse_u64(s, &v) || v > TBT_U32_MAX)
        return -ERANGE;
    *out = (u32)v;
    return 0;
}

static int tbt_parse_u16(const char *s, u16 *out)
{
    u64 v;

    if (tbt_parse_u64(s, &v) || v > 0xffff)
        return -ERANGE;
    *out = (u16)v;
    return 0;
}

/* a.b.c.d, nothing else. Rejects names and IPv6 with distinct reasons, because
 * the operator needs to know which rule they hit (§1.1, §8.3). */
static int tbt_parse_ipv4(const char *s, __be32 *out, struct tbt_parse_error *e)
{
    u8 b[4];
    const char *p = s;
    int i;

    if (*p == '[' || *p == ':')
        return tbt_fail_ipv6(e);
    for (i = 0; i < 4; i++)
    {
        unsigned v = 0;
        int digits = 0;

        while (*p >= '0' && *p <= '9')
        {
            v = v * 10 + (unsigned)(*p - '0');
            if (++digits > 3 || v > 255)
                return tbt_fail(e, NULL, "IPv4 octet out of range");
            p++;
        }
        if (!digits)
        {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
                return tbt_fail(e, NULL, "hostname refused: DNS is never resolved, use an IPv4 literal");
            return tbt_fail(e, NULL, "expected an IPv4 literal");
        }
        b[i] = (u8)v;
        if (i < 3)
        {
            if (*p != '.')
                return tbt_fail(e, NULL, "expected an IPv4 literal");
            p++;
        }
    }
    if (*p)
        return tbt_fail(e, NULL, "expected an IPv4 literal");
    memcpy(out, b, sizeof(b));
    return 0;
}

/* Ports are stored in network byte order without arpa/inet.h, so the host and
 * the kernel agree on the byte layout. */
static __be16 tbt_port_store(u16 host_order)
{
    u8 b[2] = { (u8)(host_order >> 8), (u8)(host_order & 0xff) };
    __be16 out;

    memcpy(&out, b, sizeof(out));
    return out;
}

static u16 tbt_port_load(__be16 net_order)
{
    u8 b[2];

    memcpy(b, &net_order, sizeof(b));
    return (u16)((b[0] << 8) | b[1]);
}

static int tbt_parse_addr(const char *s, __be32 *addr, __be16 *port,
                          struct tbt_parse_error *e)
{
    char buf[64];
    char *colon;
    u16 p;

    if (strlen(s) >= sizeof(buf))
        return tbt_fail(e, NULL, "address field too long");
    memcpy(buf, s, strlen(s) + 1);
    colon = strrchr(buf, ':');
    if (!colon)
        return tbt_fail(e, NULL, "expected address:port");
    *colon = '\0';
    if (strchr(buf, ':') || strchr(buf, '['))
        return tbt_fail_ipv6(e);
    {
        int rc = tbt_parse_ipv4(buf, addr, e);

        if (rc)
            return rc;
    }
    if (tbt_parse_u16(colon + 1, &p) || !p)
        return tbt_fail(e, NULL, "port must be 1..65535");
    *port = tbt_port_store(p);
    return 0;
}

/* "lo-hi"; 0-0 is the canonical spelling of "no block configured". */
static int tbt_parse_range(const char *s, u16 *lo, u16 *hi, struct tbt_parse_error *e)
{
    char buf[32];
    char *dash;

    if (strlen(s) >= sizeof(buf))
        return tbt_fail(e, NULL, "port range too long");
    memcpy(buf, s, strlen(s) + 1);
    dash = strchr(buf, '-');
    if (!dash)
        return tbt_fail(e, NULL, "expected lo-hi");
    *dash = '\0';
    if (tbt_parse_u16(buf, lo) || tbt_parse_u16(dash + 1, hi))
        return tbt_fail(e, NULL, "expected lo-hi");
    if (*lo > *hi)
        return tbt_fail(e, NULL, "port range is reversed");
    if (*lo == 0 && *hi != 0)
        return tbt_fail(e, NULL, "port range must start at 1");
    return 0;
}

static int tbt_parse_profile(const char *v, u8 *out, struct tbt_parse_error *e)
{
    if (!strcmp(v, "http-kal"))
        *out = 1;
    else if (!strcmp(v, "http-kal-drip"))
        *out = 2;
    else if (!strcmp(v, "raw"))
        *out = 3;
    else
        return tbt_fail(e, NULL, "profile must be http-kal, http-kal-drip or raw");
    return 0;
}

static int tbt_parse_role(const char *v, u8 *out, struct tbt_parse_error *e)
{
    if (!strcmp(v, "near"))
        *out = TBT_ROLE_NEAR;
    else if (!strcmp(v, "far"))
        *out = TBT_ROLE_FAR;
    else
        return tbt_fail(e, NULL, "role must be near or far");
    return 0;
}

/* "2", "2.0", "2.000" -> milli. Bounded by TBT_INFLATE_MAX. */
static int tbt_parse_inflate(const char *s, u32 *out, struct tbt_parse_error *e)
{
    u64 whole = 0, frac = 0;
    int frac_digits = 0;

    if (!*s)
        return tbt_fail(e, NULL, "expected a decimal such as 2.0");
    while (*s >= '0' && *s <= '9')
    {
        whole = whole * 10 + (u64)(*s - '0');
        if (whole > 1000)
            return tbt_fail(e, NULL, "max_inflate is out of range");
        s++;
    }
    if (*s == '.')
    {
        s++;
        while (*s >= '0' && *s <= '9')
        {
            if (++frac_digits > 3)
                return tbt_fail(e, NULL, "max_inflate takes at most 3 decimals");
            frac = frac * 10 + (u64)(*s - '0');
            s++;
        }
    }
    if (*s || (!whole && !frac_digits))
        return tbt_fail(e, NULL, "expected a decimal such as 2.0");
    while (frac_digits++ < 3)
        frac *= 10;
    if (whole * 1000 + frac > TBT_INFLATE_MAX)
        return tbt_fail(e, NULL, "max_inflate is out of range");
    *out = (u32)(whole * 1000 + frac);
    return 0;
}

static int tbt_parse_srcaddrs(const char *v, struct tbt_cfg *cfg, struct tbt_parse_error *e)
{
    char buf[128];
    char *p = buf, *tok;

    if (strlen(v) >= sizeof(buf))
        return tbt_fail(e, NULL, "srcaddrs is too long");
    memcpy(buf, v, strlen(v) + 1);
    cfg->nsrc = 0;
    while ((tok = tbt_next_tok(&p)))
    {
        char *comma;
        char *one = tok;
        int rc;

        while ((comma = strchr(one, ',')))
        {
            *comma = '\0';
            if (cfg->nsrc >= TBT_SRC_MAX)
                return tbt_fail(e, NULL, "too many srcaddrs entries");
            rc = tbt_parse_ipv4(one, &cfg->srcaddrs[cfg->nsrc], e);
            if (rc)
                return rc;
            cfg->nsrc++;
            one = comma + 1;
        }
        if (cfg->nsrc >= TBT_SRC_MAX)
            return tbt_fail(e, NULL, "too many srcaddrs entries");
        rc = tbt_parse_ipv4(one, &cfg->srcaddrs[cfg->nsrc], e);
        if (rc)
            return rc;
        cfg->nsrc++;
    }
    if (!cfg->nsrc)
        return tbt_fail(e, NULL, "srcaddrs needs at least one address");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* One `tunnel` directive. Defaults are the canonical values, so a parsed
 * struct is already normalised and renders back identically. */

/* Defined with the action parser below; the tunnel key uses the same
 * 128-bit/64-bit-pin rules as `session <id> nonce=`. */
static int tbt_parse_nonce(const char *s, u8 out[16]);

static int tbt_parse_tunnel(char *args, struct tbt_cfg *cfg, struct tbt_parse_error *e)
{
    char *tok;
    u64 id;
    int have_listen = 0, have_peer = 0, have_target = 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->profile = 1; /* http-kal: the default is an effective value, not 0 */
    cfg->n = 4;
    cfg->m = 1;
    cfg->dup = 1;
    cfg->keepalive_ms = 5000;
    cfg->drip_ms = 1000;
    cfg->replay = 8388608;
    cfg->tunnel_rate = 0;
    cfg->max_inflate_milli = 2000;
    cfg->hole_budget_ms = 500;
    cfg->rx_reorder_bytes = 8388608;
    cfg->tx_window_bytes = 0; /* auto: the sender derives it from the BDP */
    cfg->tx_retain_bytes = 0; /* unlimited */
    cfg->carrier_stall_ms = 3000;

    tok = tbt_next_tok(&args);
    if (!tok || tbt_parse_u64(tok, &id) || !id)
        return tbt_fail(e, tok, "tunnel needs a non-zero id");
    cfg->id = id;

    while ((tok = tbt_next_tok(&args)))
    {
        char *eq = strchr(tok, '=');
        const char *key;
        char *val;

        if (!eq)
            return tbt_fail(e, tok, "expected key=value");
        *eq = '\0';
        key = tok;
        val = eq + 1;
        if (!*val)
            return tbt_fail(e, key, "empty value");

        if (!strcmp(key, "role"))
        {
            if (tbt_parse_role(val, &cfg->role, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "listen"))
        {
            int rc = tbt_parse_addr(val, &cfg->listen_addr, &cfg->listen_port, e);

            if (rc)
                return rc;
            have_listen = 1;
        }
        else if (!strcmp(key, "peer"))
        {
            int rc = tbt_parse_addr(val, &cfg->peer_addr, &cfg->peer_port, e);

            if (rc)
                return rc;
            have_peer = 1;
        }
        else if (!strcmp(key, "target"))
        {
            int rc = tbt_parse_addr(val, &cfg->target_addr, &cfg->target_port, e);

            if (rc)
                return rc;
            have_target = 1;
        }
        else if (!strcmp(key, "n"))
        {
            if (tbt_parse_u16(val, &cfg->n) || !cfg->n || cfg->n > TBT_MAX_CARRIERS_LIMIT)
                return tbt_fail(e, key, "n must be 1..16");
        }
        else if (!strcmp(key, "m"))
        {
            if (tbt_parse_u16(val, &cfg->m) || cfg->m > TBT_MAX_CARRIERS_LIMIT)
                return tbt_fail(e, key, "m must be 0..16");
        }
        else if (!strcmp(key, "dup"))
        {
            u32 v;

            /* The old code stored a u16 through a u8 pointer; parse into a
             * wide temporary and range-check instead. */
            if (tbt_parse_u32(val, &v) || v > 3)
                return tbt_fail(e, key, "dup must be 0..3");
            cfg->dup = (u8)v;
        }
        else if (!strcmp(key, "keepalive"))
        {
            if (tbt_parse_u32(val, &cfg->keepalive_ms) || cfg->keepalive_ms > 86400000u)
                return tbt_fail(e, key, "keepalive must be 0..86400000 ms");
        }
        else if (!strcmp(key, "drip_ms"))
        {
            if (tbt_parse_u32(val, &cfg->drip_ms) || cfg->drip_ms > 3600000u)
                return tbt_fail(e, key, "drip_ms must be 0..3600000");
        }
        else if (!strcmp(key, "replay"))
        {
            if (tbt_parse_u32(val, &cfg->replay) || cfg->replay > TBT_REPLAY_MAX)
                return tbt_fail(e, key, "replay must be 0..268435456 bytes");
        }
        else if (!strcmp(key, "tunnel_rate"))
        {
            if (tbt_parse_u32(val, &cfg->tunnel_rate))
                return tbt_fail(e, key, "tunnel_rate must be a number");
        }
        else if (!strcmp(key, "max_inflate"))
        {
            if (tbt_parse_inflate(val, &cfg->max_inflate_milli, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "hole_budget_ms"))
        {
            if (tbt_parse_u32(val, &cfg->hole_budget_ms) || !cfg->hole_budget_ms ||
                cfg->hole_budget_ms > 60000u)
                return tbt_fail(e, key, "hole_budget_ms must be 1..60000");
        }
        else if (!strcmp(key, "rx_reorder_bytes"))
        {
            if (tbt_parse_u32(val, &cfg->rx_reorder_bytes) || !cfg->rx_reorder_bytes ||
                cfg->rx_reorder_bytes > TBT_REORDER_MAX)
                return tbt_fail(e, key, "rx_reorder_bytes must be 1..268435456 bytes");
        }
        else if (!strcmp(key, "tx_window_bytes"))
        {
            if (tbt_parse_u32(val, &cfg->tx_window_bytes) ||
                cfg->tx_window_bytes > TBT_WINDOW_MAX)
                return tbt_fail(e, key, "tx_window_bytes must be 0..268435456 bytes"
                                           " and no more than rx_reorder_bytes");
        }
        else if (!strcmp(key, "tx_retain_bytes"))
        {
            if (tbt_parse_u32(val, &cfg->tx_retain_bytes))
                return tbt_fail(e, key, "tx_retain_bytes must be 0..4294967295 (0 = unlimited)");
        }
        else if (!strcmp(key, "carrier_stall_ms"))
        {
            if (tbt_parse_u32(val, &cfg->carrier_stall_ms) || cfg->carrier_stall_ms > TBT_STALL_MAX)
                return tbt_fail(e, key, "carrier_stall_ms must be 0..3600000 ms");
        }
        else if (!strcmp(key, "profile"))
        {
            if (tbt_parse_profile(val, &cfg->profile, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "portblock"))
        {
            if (tbt_parse_range(val, &cfg->portblock_lo, &cfg->portblock_hi, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "srcaddrs"))
        {
            if (tbt_parse_srcaddrs(val, cfg, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, TBT_ACT_NONCE))
        {
            if (tbt_parse_nonce(val, cfg->nonce))
                return tbt_fail(e, key, "nonce must be 16 or 32 hex digits");
            cfg->nonce_set = true;
        }
        else
        {
            return tbt_fail(e, key, "unknown key");
        }
    }

    if (!cfg->role)
        return tbt_fail(e, "role", "tunnel needs role=near or role=far");
    if (!have_listen)
        return tbt_fail(e, "listen", "tunnel needs listen=addr:port");
    if (cfg->role == TBT_ROLE_NEAR && !have_peer)
        return tbt_fail(e, "peer", "role=near needs peer=addr:port");
    if (cfg->role == TBT_ROLE_FAR && !have_target)
        return tbt_fail(e, "target", "role=far needs target=addr:port");
    if (cfg->n + cfg->m > TBT_MAX_CARRIERS_LIMIT)
        return tbt_fail(e, "n", "n + m exceeds the 16-carrier limit");
    /* A sender window above the peer's reorder budget kills the stream on the
     * first frame past that budget, so the two keys are checked together once
     * both are known; the line may name them in either order. */
    if (cfg->tx_window_bytes > cfg->rx_reorder_bytes)
        return tbt_fail(e, "tx_window_bytes", "tx_window_bytes must be 0..268435456 bytes"
                                                " and no more than rx_reorder_bytes");
    return 0;
}

/* One hex digit, or -1. */
static int tbt_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A session nonce is 128 bits. Accept 32 hex digits, or 16 as the low 64 bits
 * left-padded with zeros; the canonical render is always 32 digits. Shared by
 * the tunnel `nonce=` key and the runtime `session <id> nonce=` action. */
static int tbt_parse_nonce(const char *s, u8 out[16])
{
    size_t len, i, start;

    if (!s)
        return -EINVAL;
    len = strlen(s);
    if (len != 16 && len != 32)
        return -EINVAL;
    memset(out, 0, 16);
    start = 16 - len / 2;
    for (i = 0; i < len / 2; i++)
    {
        int hi = tbt_hexval(s[2 * i]);
        int lo = tbt_hexval(s[2 * i + 1]);

        if (hi < 0 || lo < 0)
            return -EINVAL;
        out[start + i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

/* `carrier <index> down|up|drop_ppm <n>`: a runtime action on one carrier,
 * never part of the canonical config (DESIGN.md §8.3). `up` is the inverse of
 * `down`; applying `drop_ppm` requires the module parameter debug_loss=1. */
static int tbt_parse_carrier(char *args, struct tbt_cmd *out, struct tbt_parse_error *e)
{
    char *tok = tbt_next_tok(&args);
    char *act;

    if (!tok || tbt_parse_u16(tok, &out->carrier) || out->carrier >= TBT_MAX_CARRIERS_LIMIT)
        return tbt_fail(e, "index", "carrier index must be 0..15");

    act = tbt_next_tok(&args);
    if (!act)
        return tbt_fail(e, "carrier", "carrier needs an action: down, up or drop_ppm");
    if (!strcmp(act, TBT_ACT_DOWN))
    {
        if (tbt_next_tok(&args))
            return tbt_fail(e, "carrier", "trailing tokens after carrier <index> down");
        out->carrier_action = TBT_CARRIER_DOWN;
        return 0;
    }
    if (!strcmp(act, TBT_ACT_UP))
    {
        if (tbt_next_tok(&args))
            return tbt_fail(e, "carrier", "trailing tokens after carrier <index> up");
        out->carrier_action = TBT_CARRIER_UP;
        return 0;
    }
    if (!strcmp(act, TBT_ACT_DROP_PPM))
    {
        tok = tbt_next_tok(&args);
        if (!tok || tbt_parse_u32(tok, &out->drop_ppm) || out->drop_ppm > TBT_DROP_PPM_MAX)
            return tbt_fail(e, "drop_ppm", "drop_ppm must be 0..1000000");
        if (tbt_next_tok(&args))
            return tbt_fail(e, "carrier", "trailing tokens after carrier <index> drop_ppm <n>");
        out->carrier_action = TBT_CARRIER_DROP_PPM;
        return 0;
    }
    return tbt_fail(e, act, "carrier action must be down, up or drop_ppm");
}

/* `session <tunnel> nonce=<hex>`: pin the 128-bit session nonce for
 * reproducible tests. A runtime action, never part of the canonical config. */
static int tbt_parse_session(char *args, struct tbt_cmd *out, struct tbt_parse_error *e)
{
    char *tok = tbt_next_tok(&args);
    char *eq;

    if (!tok || tbt_parse_u64(tok, &out->id) || !out->id)
        return tbt_fail(e, "session", "session needs a non-zero tunnel id");
    tok = tbt_next_tok(&args);
    if (!tok)
        return tbt_fail(e, TBT_ACT_NONCE, "session needs nonce=<hex>");
    eq = strchr(tok, '=');
    if (!eq)
        return tbt_fail(e, tok, "expected nonce=<hex>");
    *eq = '\0';
    if (strcmp(tok, TBT_ACT_NONCE))
        return tbt_fail(e, tok, "unknown key; session accepts nonce=<hex>");
    if (tbt_parse_nonce(eq + 1, out->nonce))
        return tbt_fail(e, TBT_ACT_NONCE, "nonce must be 16 or 32 hex digits");
    if (tbt_next_tok(&args))
        return tbt_fail(e, "session", "trailing tokens after session <tunnel> nonce=<hex>");
    return 0;
}

int tbt_parse_line(char *line, struct tbt_cmd *out, struct tbt_parse_error *e)
{
    char *p = line;
    char *tok;
    size_t len;

    memset(out, 0, sizeof(*out));
    out->kind = TBT_CMD_NONE;
    if (e)
    {
        e->key = NULL;
        e->why = NULL;
    }

    if (strlen(line) >= TBT_LINE_MAX)
    {
        if (e)
        {
            e->key = NULL;
            e->why = "line is longer than 1023 bytes";
        }
        return TBT_PARSE_E2BIG;
    }
    len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';

    tok = tbt_next_tok(&p);
    if (!tok || tok[0] == '#')
        return 0;

    if (!strcmp(tok, "tunnel"))
    {
        int rc = tbt_parse_tunnel(p, &out->cfg, e);

        if (rc)
            return rc;
        out->kind = TBT_CMD_TUNNEL;
        return 0;
    }
    if (!strcmp(tok, "del"))
    {
        char *idtok = tbt_next_tok(&p);

        if (!idtok || tbt_parse_u64(idtok, &out->id) || !out->id)
            return tbt_fail(e, "del", "del needs a non-zero id");
        if (tbt_next_tok(&p))
            return tbt_fail(e, "del", "trailing tokens after del <id>");
        out->kind = TBT_CMD_DEL;
        return 0;
    }
    if (!strcmp(tok, "flush"))
    {
        if (tbt_next_tok(&p))
            return tbt_fail(e, "flush", "trailing tokens after flush");
        out->kind = TBT_CMD_FLUSH;
        return 0;
    }
    if (!strcmp(tok, TBT_ACT_CARRIER))
    {
        int rc = tbt_parse_carrier(p, out, e);

        if (rc)
            return rc;
        out->kind = TBT_CMD_CARRIER;
        return 0;
    }
    if (!strcmp(tok, TBT_ACT_SESSION))
    {
        int rc = tbt_parse_session(p, out, e);

        if (rc)
            return rc;
        out->kind = TBT_CMD_SESSION;
        return 0;
    }
    return tbt_fail(e, tok, "unknown directive");
}

/* `/proc/net/tbtun/ports` carries one directive: `ports lo-hi`. A blank line
 * and `ports 0-0` are no-ops: both outputs stay zero, which the caller treats
 * as "leave the block unchanged". */
int tbt_parse_ports(const char *line, u16 *lo, u16 *hi, struct tbt_parse_error *e)
{
    char buf[64];
    char *p = buf, *tok;
    size_t len = strlen(line);

    *lo = 0;
    *hi = 0;
    if (len >= sizeof(buf))
        return tbt_fail(e, NULL, "ports line too long");
    memcpy(buf, line, len + 1);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
        buf[--len] = '\0';

    tok = tbt_next_tok(&p);
    if (!tok)
        return 0; /* blank line: nothing to do */
    if (strcmp(tok, "ports"))
        return tbt_fail(e, tok, "unknown directive; expected ports lo-hi");
    tok = tbt_next_tok(&p);
    if (!tok)
        return tbt_fail(e, "ports", "ports needs lo-hi");
    if (tbt_next_tok(&p))
        return tbt_fail(e, "ports", "trailing tokens after ports lo-hi");
    if (tbt_parse_range(tok, lo, hi, e))
        return TBT_PARSE_EINVAL;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Rendering */

static int tbt_addf(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= size)
        return TBT_PARSE_E2BIG;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return TBT_PARSE_EINVAL;
    if ((size_t)n >= size - *off)
        return TBT_PARSE_E2BIG;
    *off += (size_t)n;
    return 0;
}

/* Minimal-width decimal: 1 renders as "1", not "001". */
static size_t tbt_fmt_octet(char *out, u8 v)
{
    size_t n = 0;

    if (v >= 100)
        out[n++] = (char)('0' + v / 100);
    if (v >= 10)
        out[n++] = (char)('0' + (v / 10) % 10);
    out[n++] = (char)('0' + v % 10);
    return n;
}

static void tbt_fmt_ipv4(__be32 addr, char *out)
{
    u8 b[4];
    size_t n = 0;
    int i;

    memcpy(b, &addr, sizeof(b));
    for (i = 0; i < 4; i++)
    {
        n += tbt_fmt_octet(out + n, b[i]);
        out[n++] = i < 3 ? '.' : '\0';
    }
}

static void tbt_fmt_hex(const u8 *in, size_t n, char *out);

int tbt_render_cfg(const struct tbt_cfg *c, char *buf, size_t size)
{
    static const char *const profiles[] = { "http-kal", "http-kal", "http-kal-drip", "raw" };
    char la[16], pa[16], ta[16];
    size_t off = 0;
    int i, rc;

    tbt_fmt_ipv4(c->listen_addr, la);
    tbt_fmt_ipv4(c->peer_addr, pa);
    tbt_fmt_ipv4(c->target_addr, ta);

    rc = tbt_addf(buf, size, &off, "tunnel %llu role=%s listen=%s:%u",
                  (unsigned long long)c->id,
                  c->role == TBT_ROLE_NEAR ? "near" : "far",
                  la, tbt_port_load(c->listen_port));
    if (rc)
        return rc;
    if (c->role == TBT_ROLE_NEAR)
    {
        rc = tbt_addf(buf, size, &off, " peer=%s:%u", pa, tbt_port_load(c->peer_port));
        if (rc)
            return rc;
    }
    else
    {
        rc = tbt_addf(buf, size, &off, " target=%s:%u", ta, tbt_port_load(c->target_port));
        if (rc)
            return rc;
        /* Far: the near source address allowed to join carriers.  The port is
         * grammar filler; only the address is used.  Omitted when unset, so a
         * far line without an anchor stays unchanged. */
        if (c->peer_addr && c->peer_port)
        {
            rc = tbt_addf(buf, size, &off, " peer=%s:%u", pa,
                          tbt_port_load(c->peer_port));
            if (rc)
                return rc;
        }
    }
    rc = tbt_addf(buf, size, &off,
                  " n=%u m=%u dup=%u profile=%s keepalive=%u"
                  " drip_ms=%u replay=%u tunnel_rate=%u"
                  " max_inflate=%u.%03u hole_budget_ms=%u rx_reorder_bytes=%u"
                  " tx_window_bytes=%u tx_retain_bytes=%u carrier_stall_ms=%u"
                  " portblock=%u-%u",
                  c->n, c->m, c->dup, profiles[c->profile & 3], c->keepalive_ms,
                  c->drip_ms, c->replay, c->tunnel_rate,
                  c->max_inflate_milli / 1000, c->max_inflate_milli % 1000,
                  c->hole_budget_ms, c->rx_reorder_bytes, c->tx_window_bytes,
                  c->tx_retain_bytes, c->carrier_stall_ms, c->portblock_lo,
                  c->portblock_hi);
    if (rc)
        return rc;
    if (c->nonce_set)
    {
        char nb[2 * sizeof(c->nonce) + 1];

        tbt_fmt_hex(c->nonce, sizeof(c->nonce), nb);
        rc = tbt_addf(buf, size, &off, " nonce=%s", nb);
        if (rc)
            return rc;
    }
    if (c->nsrc)
    {
        rc = tbt_addf(buf, size, &off, " srcaddrs=");
        if (rc)
            return rc;
        for (i = 0; i < c->nsrc && i < TBT_SRC_MAX; i++)
        {
            tbt_fmt_ipv4(c->srcaddrs[i], la);
            rc = tbt_addf(buf, size, &off, i ? ",%s" : "%s", la);
            if (rc)
                return rc;
        }
    }
    rc = tbt_addf(buf, size, &off, "\n");
    if (rc)
        return rc;
    return (int)off;
}

static void tbt_fmt_hex(const u8 *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++)
    {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

int tbt_render_action(const struct tbt_cmd *c, char *buf, size_t size)
{
    char nonce[2 * sizeof(c->nonce) + 1];
    size_t off = 0;
    int rc;

    switch (c->kind)
    {
    case TBT_CMD_CARRIER:
        if (c->carrier_action == TBT_CARRIER_DOWN)
            rc = tbt_addf(buf, size, &off, "carrier %u down\n", (unsigned)c->carrier);
        else if (c->carrier_action == TBT_CARRIER_UP)
            rc = tbt_addf(buf, size, &off, "carrier %u up\n", (unsigned)c->carrier);
        else if (c->carrier_action == TBT_CARRIER_DROP_PPM)
            rc = tbt_addf(buf, size, &off, "carrier %u drop_ppm %u\n", (unsigned)c->carrier,
                          (unsigned)c->drop_ppm);
        else
            return TBT_PARSE_EINVAL;
        break;
    case TBT_CMD_SESSION:
        tbt_fmt_hex(c->nonce, sizeof(c->nonce), nonce);
        rc = tbt_addf(buf, size, &off, "session %llu nonce=%s\n", (unsigned long long)c->id, nonce);
        break;
    default:
        return TBT_PARSE_EINVAL; /* not an action verb */
    }
    if (rc)
        return rc;
    return (int)off;
}

int tbt_render_debug_loss(bool enabled, char *buf, size_t size)
{
    size_t off = 0;
    int rc = tbt_addf(buf, size, &off, "debug_loss=%u\n", enabled ? 1u : 0u);

    if (rc)
        return rc;
    return (int)off;
}

int tbt_render_stats_prefix(bool debug_loss, u64 gaps_total, u64 streams_total, char *buf,
                            size_t size)
{
    size_t off = 0;
    int rc;

    rc = tbt_addf(buf, size, &off, "debug_loss=%u\n", debug_loss ? 1u : 0u);
    if (rc)
        return rc;
    rc = tbt_addf(buf, size, &off, "gaps_total=%llu\n", (unsigned long long)gaps_total);
    if (rc)
        return rc;
    rc = tbt_addf(buf, size, &off, "streams_total=%llu\n", (unsigned long long)streams_total);
    if (rc)
        return rc;
    return (int)off;
}
