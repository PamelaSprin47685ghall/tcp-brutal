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
#include "tbt_fec_adapt_sclc.h"
#include "tbt_map.h"
#include "tbt_portmap.h"

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

/* a.b.c.d, nothing else. A name gets its own reason because the operator has
 * to know that DNS is never resolved (§1.1, §8.3). */
static int tbt_parse_ipv4(const char *s, __be32 *out, struct tbt_parse_error *e)
{
    u8 b[4];
    const char *p = s;
    int i;

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

/* One 1..4 digit hexadecimal group of an IPv6 literal. */
static int tbt_parse_hex16(const char *s, size_t n, u16 *out)
{
    u16 v = 0;
    size_t i;

    if (!n || n > 4)
        return -EINVAL;
    for (i = 0; i < n; i++)
    {
        char c = s[i];
        u16 d;

        if (c >= '0' && c <= '9')
            d = (u16)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (u16)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (u16)(c - 'A' + 10);
        else
            return -EINVAL;
        v = (u16)(v << 4 | d);
    }
    *out = v;
    return 0;
}

/* Parse the colon-separated groups of one half of an IPv6 literal. Returns the
 * number of 16-bit groups, or -EINVAL. `allow_v4` admits one dotted quad in
 * the final position; it occupies the last two groups (RFC 4291 §2.2). */
static int tbt_parse_v6_half(const char *s, size_t len, u16 *out, int max, bool allow_v4)
{
    const char *p = s;
    const char *end = s + len;
    int n = 0;

    if (p == end)
        return 0;
    for (;;)
    {
        const char *colon = p;
        const char *dot = NULL;

        while (colon < end && *colon != ':')
        {
            if (*colon == '.')
                dot = colon;
            colon++;
        }
        if (dot)
        {
            __be32 v4;
            struct tbt_parse_error ignored = { NULL, NULL };
            char quad[16];
            size_t qlen = (size_t)(colon - p);
            const u8 *qb = (const u8 *)&v4;

            if (!allow_v4 || colon != end || n > max - 2)
                return -EINVAL;
            if (qlen >= sizeof(quad))
                return -EINVAL;
            memcpy(quad, p, qlen);
            quad[qlen] = '\0';
            if (tbt_parse_ipv4(quad, &v4, &ignored))
                return -EINVAL;
            /* v4 holds the four network-order bytes; split them into two
             * 16-bit groups explicitly instead of copying the __be32 over
             * the u16 array, whose byte order is the host's. */
            out[n] = (u16)((u16)qb[0] << 8 | qb[1]);
            out[n + 1] = (u16)((u16)qb[2] << 8 | qb[3]);
            return n + 2;
        }
        if (colon == p || n >= max)
            return -EINVAL;
        if (tbt_parse_hex16(p, (size_t)(colon - p), &out[n]))
            return -EINVAL;
        n++;
        if (colon == end)
            return n;
        p = colon + 1;
        if (p == end)
            return -EINVAL; /* a trailing single colon */
    }
}

/* A full IPv6 literal: no brackets, no port. `::` may appear once and stands
 * for at least one zero group. */
static int tbt_parse_ipv6(const char *s, struct tbt_addr *out, struct tbt_parse_error *e)
{
    u16 head[8], tail[8];
    int nhead, ntail = 0;
    const char *dc = NULL;
    const char *p;
    u8 bytes[16];
    int i;

    for (p = s; *p; p++)
    {
        if (*p == ':' && p[1] == ':')
        {
            if (dc)
                return tbt_fail(e, NULL, "IPv6 literal has more than one ::");
            dc = p++;
        }
    }
    if (dc)
    {
        nhead = tbt_parse_v6_half(s, (size_t)(dc - s), head, 8, false);
        ntail = tbt_parse_v6_half(dc + 2, strlen(dc + 2), tail, 8, true);
        if (nhead < 0 || ntail < 0)
            return tbt_fail(e, NULL, "invalid IPv6 group");
        if (nhead + ntail > 7)
            return tbt_fail(e, NULL, ":: must stand for at least one zero group");
    }
    else
    {
        nhead = tbt_parse_v6_half(s, strlen(s), head, 8, true);
        if (nhead < 0)
            return tbt_fail(e, NULL, "invalid IPv6 group");
        if (nhead != 8)
            return tbt_fail(e, NULL, "an IPv6 literal needs 8 groups or ::");
    }
    memset(bytes, 0, sizeof(bytes));
    for (i = 0; i < nhead; i++)
    {
        bytes[2 * i] = (u8)(head[i] >> 8);
        bytes[2 * i + 1] = (u8)head[i];
    }
    for (i = 0; i < ntail; i++)
    {
        int slot = 8 - ntail + i;

        bytes[2 * slot] = (u8)(tail[i] >> 8);
        bytes[2 * slot + 1] = (u8)tail[i];
    }
    out->family = TBT_AF_INET6;
    memcpy(out->u.v6, bytes, sizeof(bytes));
    return 0;
}

/* Ports are stored in network byte order without arpa/inet.h, so the host and
 * the kernel agree on the byte layout. */
static __be16 tbt_port_store(u16 host_order)
{
    return tbt_port_be16(host_order);
}

static u16 tbt_port_load(__be16 net_order)
{
    return tbt_port_host16(net_order);
}

static int tbt_parse_addr(const char *s, struct tbt_addr *addr, __be16 *port,
                          struct tbt_parse_error *e)
{
    char buf[80];
    char *colon;
    u16 p;
    size_t len = strlen(s);

    if (len >= sizeof(buf))
        return tbt_fail(e, NULL, "address field too long");
    memcpy(buf, s, len + 1);
    if (buf[0] == '[')
    {
        char *close = strchr(buf, ']');
        int rc;

        if (!close)
            return tbt_fail(e, NULL, "expected ] after an IPv6 literal");
        if (close[1] != ':')
            return tbt_fail(e, NULL, "expected :port after the IPv6 literal");
        *close = '\0';
        rc = tbt_parse_ipv6(buf + 1, addr, e);
        if (rc)
            return rc;
        colon = close + 1;
    }
    else
    {
        colon = strrchr(buf, ':');
        if (!colon)
            return tbt_fail(e, NULL, "expected address:port");
        *colon = '\0';
        if (strchr(buf, ':'))
            return tbt_fail(e, NULL, "an IPv6 literal must be bracketed: [addr]:port");
        addr->family = TBT_AF_INET;
        if (tbt_parse_ipv4(buf, &addr->u.v4, e))
            return TBT_PARSE_EINVAL;
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

/* `fec=off|on`. The key is part of the canonical grammar now, so the value is
 * parsed and rendered exactly like the other keys; whether the session may
 * actually use FEC is decided later, in tbt_session_create(). */
static int tbt_parse_fec(const char *key, const char *v, u8 *out,
                         struct tbt_parse_error *e)
{
    if (!strcmp(v, "off"))
        *out = 0;
    else if (!strcmp(v, "on"))
        *out = 1;
    else
        return tbt_fail(e, key, "fec must be off or on");
    return 0;
}

/* `mode=rlnc|grouped|sc-ldpc`. The mode is always rendered, even with
 * fec=off, so the canonical line states the rate shape the data plane would
 * use; only a fec=on session would read it. */
static int tbt_parse_mode(const char *key, const char *v, u8 *out,
                          struct tbt_parse_error *e)
{
    if (!strcmp(v, "rlnc"))
        *out = 1;
    else if (!strcmp(v, "grouped"))
        *out = 2;
    else if (!strcmp(v, "sc-ldpc"))
        *out = 3;
    else
        return tbt_fail(e, key, "mode must be rlnc, grouped or sc-ldpc");
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

/* One srcaddrs entry: a bare literal, or an IPv6 literal in brackets for
 * symmetry with the listen=/peer=/target= grammar. The list separator is a
 * comma, so a bare IPv6 literal is unambiguous too. */
static int tbt_parse_srcaddr_one(const char *s, struct tbt_addr *out,
                                 struct tbt_parse_error *e)
{
    size_t len = strlen(s);

    if (len >= 2 && s[0] == '[' && s[len - 1] == ']')
    {
        char buf[TBT_ADDR_STRLEN];

        if (len - 2 >= sizeof(buf))
            return tbt_fail(e, NULL, "srcaddrs entry too long");
        memcpy(buf, s + 1, len - 2);
        buf[len - 2] = '\0';
        return tbt_parse_ipv6(buf, out, e);
    }
    if (strchr(s, ':'))
        return tbt_parse_ipv6(s, out, e);
    out->family = TBT_AF_INET;
    return tbt_parse_ipv4(s, &out->u.v4, e);
}

static int tbt_parse_srcaddrs(const char *v, struct tbt_cfg *cfg, struct tbt_parse_error *e)
{
    /* Eight full IPv6 literals plus commas: 8 * 45 + 7, still inside one
     * TBT_LINE_MAX line. */
    char buf[384];
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
            rc = tbt_parse_srcaddr_one(one, &cfg->srcaddrs[cfg->nsrc], e);
            if (rc)
                return rc;
            cfg->nsrc++;
            one = comma + 1;
        }
        if (cfg->nsrc >= TBT_SRC_MAX)
            return tbt_fail(e, NULL, "too many srcaddrs entries");
        rc = tbt_parse_srcaddr_one(one, &cfg->srcaddrs[cfg->nsrc], e);
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
static int tbt_hexval(char c);

static int tbt_parse_dev(const char *s, char out[IFNAMSIZ]);

/* `map=listen_port:target_port[,listen_port:target_port...]`. The value has no
 * spaces, so a comma is the only list separator. Repeating map= on one line
 * replaces the list, like every other key: one line, one value. */
static int tbt_parse_map(const char *v, struct tbt_cfg *cfg,
                         struct tbt_parse_error *e)
{
    char buf[128];
    char *p = buf;

    if (strlen(v) >= sizeof(buf))
        return tbt_fail(e, "map", "map list is too long");
    memcpy(buf, v, strlen(v) + 1);
    cfg->nmap = 0;
    while (*p)
    {
        char *comma = strchr(p, ',');
        char *colon;
        u16 lp, tp;

        if (comma)
            *comma = '\0';
        if (cfg->nmap >= TBT_MAP_MAX - 1)
            return tbt_fail(e, "map", "too many map entries (max 7)");
        colon = strchr(p, ':');
        if (!colon || strchr(colon + 1, ':'))
            return tbt_fail(e, "map", "map entry must be listen_port:target_port");
        *colon = '\0';
        if (tbt_parse_u16(p, &lp) || !lp ||
            tbt_parse_u16(colon + 1, &tp) || !tp)
            return tbt_fail(e, "map", "map ports must be 1..65535");
        cfg->maps[cfg->nmap].listen_port = lp;
        cfg->maps[cfg->nmap].target_port = tp;
        cfg->nmap++;
        if (!comma)
            break;
        p = comma + 1;
        if (!*p)
            return tbt_fail(e, "map", "empty map entry");
    }
    return 0;
}

static int tbt_parse_tunnel(char *args, struct tbt_cfg *cfg, struct tbt_parse_error *e)
{
    char *tok;
    u64 id;
    int have_listen = 0, have_peer = 0, have_target = 0, have_window = 0;
    bool have_crypto = false, have_adaptive = false;

    memset(cfg, 0, sizeof(*cfg));
    cfg->profile = 1; /* http-kal: the default is an effective value, not 0 */
    cfg->fec = 0;     /* old ARQ: the default is an effective value, not 0 */
    cfg->mode = 1;    /* rlnc */
    cfg->w = 0;       /* auto: resolved below once n and mode are known */
    cfg->carriers = 0; /* auto: resolved to n + m */
    cfg->n = 4;
    cfg->m = 1;
    cfg->dup = 1;
    cfg->keepalive_ms = 5000;
    cfg->drip_ms = 1000;
    cfg->replay = 8388608;
    cfg->tunnel_rate = 0;
    cfg->max_inflate_milli = 2000;
    cfg->hole_budget_ms = 0; /* auto: the receiver derives it from the RTT */
    cfg->rx_reorder_bytes = 67108864;
    cfg->tx_window_bytes = 67108864;
    cfg->tx_retain_bytes = 0; /* unlimited */
    cfg->carrier_stall_ms = 0; /* auto: derived from the measured RTO */
    /* T0 §2.4: crypto follows fec unless crypto= is explicit. Resolved
     * after the key loop, because fec= may appear before or after it. */
    cfg->crypto = 0;

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
            int rc = tbt_parse_addr(val, &cfg->listen, &cfg->listen_port, e);

            if (rc)
                return rc;
            have_listen = 1;
        }
        else if (!strcmp(key, "peer"))
        {
            int rc = tbt_parse_addr(val, &cfg->peer, &cfg->peer_port, e);

            if (rc)
                return rc;
            have_peer = 1;
        }
        else if (!strcmp(key, "target"))
        {
            int rc = tbt_parse_addr(val, &cfg->target, &cfg->target_port, e);

            if (rc)
                return rc;
            have_target = 1;
        }
        else if (!strcmp(key, "map"))
        {
            if (tbt_parse_map(val, cfg, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "n"))
        {
            if (tbt_parse_u16(val, &cfg->n) || !cfg->n || cfg->n > TBT_MAX_CARRIERS_LIMIT)
                return tbt_fail(e, key, "n must be 1..256");
        }
        else if (!strcmp(key, "m"))
        {
            if (tbt_parse_u16(val, &cfg->m) || cfg->m > TBT_MAX_CARRIERS_LIMIT)
                return tbt_fail(e, key, "m must be 0..256");
        }
        else if (!strcmp(key, "dup"))
        {
            u32 v;

            /* The old code stored a u16 through a u8 pointer; parse into a
             * wide temporary and range-check instead. */
            if (tbt_parse_u32(val, &v) || v > TBT_DUP_MAX)
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
            if (tbt_parse_u32(val, &cfg->hole_budget_ms) || cfg->hole_budget_ms > 60000u)
                return tbt_fail(e, key, "hole_budget_ms must be 0..60000 ms (0 = auto)");
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
            have_window = 1;
        }
        else if (!strcmp(key, "tx_retain_bytes"))
        {
            if (tbt_parse_u32(val, &cfg->tx_retain_bytes))
                return tbt_fail(e, key, "tx_retain_bytes must be 0..4294967295 (0 = unlimited)");
        }
        else if (!strcmp(key, "carrier_stall_ms"))
        {
            if (tbt_parse_u32(val, &cfg->carrier_stall_ms) || cfg->carrier_stall_ms > TBT_STALL_MAX)
                return tbt_fail(e, key, "carrier_stall_ms must be 0..3600000 ms (0 = auto)");
        }
        else if (!strcmp(key, "profile"))
        {
            if (tbt_parse_profile(val, &cfg->profile, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "fec"))
        {
            if (tbt_parse_fec(key, val, &cfg->fec, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "adaptive"))
        {
            /* The control law chooses n/m; the configured pair is the
             * initial shape and the upper hint (design §4.5). */
            if (!strcmp(val, "on"))
                cfg->adaptive = 1;
            else if (!strcmp(val, "off"))
                cfg->adaptive = 0;
            else
                return tbt_fail(e, key, "adaptive must be on or off");
            have_adaptive = true;
        }
        else if (!strcmp(key, "mode"))
        {
            if (tbt_parse_mode(key, val, &cfg->mode, e))
                return TBT_PARSE_EINVAL;
        }
        else if (!strcmp(key, "w"))
        {
            if (tbt_parse_u16(val, &cfg->w) || cfg->w > TBT_FEC_MAX_W)
                return tbt_fail(e, key, "w must be 0..1024");
        }
        else if (!strcmp(key, "L"))
        {
            if (tbt_parse_u16(val, &cfg->L) || cfg->L > 4096)
                return tbt_fail(e, key, "L must be 0..4096");
        }
        else if (!strcmp(key, "carriers"))
        {
            if (tbt_parse_u16(val, &cfg->carriers) ||
                cfg->carriers > TBT_MAX_CARRIERS_LIMIT)
                return tbt_fail(e, key, "carriers must be 0..256");
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
        else if (!strcmp(key, "dev"))
        {
            if (tbt_parse_dev(val, cfg->dev))
                return tbt_fail(e, key,
                                "dev must be a 1..15 byte interface name with no whitespace");
            cfg->dev_set = true;
        }
        else if (!strcmp(key, TBT_ACT_NONCE))
        {
            if (tbt_parse_nonce(val, cfg->nonce))
                return tbt_fail(e, key, "nonce must be 16 or 32 hex digits");
            cfg->nonce_set = true;
        }
        else if (!strcmp(key, "crypto"))
        {
            if (!strcmp(val, "on"))
                cfg->crypto = 1;
            else if (!strcmp(val, "off"))
                cfg->crypto = 0;
            else
                return tbt_fail(e, key, "crypto must be on or off");
            have_crypto = true;
        }
        else if (!strcmp(key, "crypto_key"))
        {
            size_t i;

            /* T0 §4.1: 64 hex digits, or `-` to clear the override. */
            if (!strcmp(val, "-"))
            {
                memset(cfg->crypto_key, 0, sizeof(cfg->crypto_key));
                cfg->crypto_key_set = false;
                continue;
            }
            if (strlen(val) != 2 * TBT_CRYPTO_KEY_LEN)
                return tbt_fail(e, key, "crypto_key must be 64 hex digits or -");
            for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
            {
                int hi = tbt_hexval(val[2 * i]);
                int lo = tbt_hexval(val[2 * i + 1]);

                if (hi < 0 || lo < 0)
                    return tbt_fail(e, key, "crypto_key must be 64 hex digits or -");
                cfg->crypto_key[i] = (u8)((hi << 4) | lo);
            }
            cfg->crypto_key_set = true;
        }
        else
        {
            return tbt_fail(e, key, "unknown key");
        }
    }

    /* T0 §2.4: crypto defaults to fec; crypto=on with fec=off has no data
     * plane to hook, so it is a configuration error. */
    if (!have_crypto)
        cfg->crypto = cfg->fec;
    if (cfg->crypto && !cfg->fec)
        return tbt_fail(e, "crypto", "crypto=on requires fec=on");
    /* Adaptive is meaningful only where a control law runs. Unset, it
     * follows fec; mode=3 defaults on only for a split on the shipped rate
     * ladder, so a special fixed test shape still parses as adaptive=off. */
    if (!have_adaptive)
    {
        u16 shares = (u16)(cfg->n + cfg->m);
        bool ladder = false;

        if (cfg->fec && cfg->mode == 3)
            ladder = (shares == TBT_FEC_MAX_SHARES &&
                      (cfg->n == 64 || cfg->n == 76 || cfg->n == 102 ||
                       cfg->n == 128 || cfg->n == 151 || cfg->n == 178)) ||
                     (shares == 32 &&
                      (cfg->n == 8 || cfg->n == 16 || cfg->n == 20 ||
                       cfg->n == 24));
        cfg->adaptive = cfg->fec && (cfg->mode != 3 || ladder);
    }
    if (cfg->adaptive && !cfg->fec)
        return tbt_fail(e, "adaptive", "adaptive=on requires fec=on");
    if (!cfg->role)
        return tbt_fail(e, "role", "tunnel needs role=near or role=far");
    if (!have_listen)
        return tbt_fail(e, "listen", "tunnel needs listen=addr:port");
    if (cfg->role == TBT_ROLE_NEAR && !have_peer)
        return tbt_fail(e, "peer", "role=near needs peer=addr:port");
    if (cfg->role == TBT_ROLE_FAR && !have_target)
        return tbt_fail(e, "target", "role=far needs target=addr:port");
    /* A near carrier binds srcaddrs[0] on the socket that dials peer=; a
     * source address of the other family can only fail that bind at runtime,
     * so refuse the line now. A far tunnel records srcaddrs but never dials. */
    if (cfg->role == TBT_ROLE_NEAR && cfg->nsrc)
    {
        int i;

        for (i = 0; i < cfg->nsrc; i++)
            if (cfg->srcaddrs[i].family != cfg->peer.family)
                return tbt_fail(e, "srcaddrs",
                                "srcaddrs must match the peer= address family");
    }
    /* A near tunnel binds slot 0 and every map on the same address, so two
     * equal listen ports cannot both exist. A far tunnel ignores the map
     * listen half, so its numbers need not be distinct. */
    if (cfg->role == TBT_ROLE_NEAR)
    {
        u16 base = tbt_port_host16(cfg->listen_port);
        int i, j;

        for (i = 0; i < cfg->nmap; i++)
        {
            if (cfg->maps[i].listen_port == base)
                return tbt_fail(e, "map", "map listen port collides with listen=");
            for (j = 0; j < i; j++)
                if (cfg->maps[i].listen_port == cfg->maps[j].listen_port)
                    return tbt_fail(e, "map", "duplicate map listen port");
        }
    }
    if (cfg->n + cfg->m > TBT_MAX_CARRIERS_LIMIT)
        return tbt_fail(e, "n", "n + m exceeds the 256-carrier limit");
    /* w=0 and carriers=0 mean "auto". Resolve them before validating, so the
     * struct holds the effective value and the canonical render is stable:
     * a written 0 reads back as the value it stands for. */
    /* mode=3 reuses w as the codeword block: 256 is the binary downlink, 64
     * the GF(16) uplink; 512 is reserved for the L16/block512 profile. It is
     * not an RLNC window, so w >= n does not apply to it. */
    if (cfg->mode == 3 && cfg->w && cfg->w != 64 && cfg->w != 256 &&
        cfg->w != 512)
        return tbt_fail(e, "w", "sc-ldpc w must be 64, 256 or 512");
    if (!cfg->w)
        cfg->w = cfg->mode == 3 ? 256 : (u16)(2 * cfg->n);
    if (cfg->mode != 3 && cfg->w < cfg->n)
        return tbt_fail(e, "w", "w must be 0 or at least n");
    /* An explicit carriers= is used as written. An omitted one starts at
     * n + m, except grouped FEC: it starts at the largest whole number of
     * groups that fits TBT_FEC_DEFAULT_GROUPED_CARRIERS, because 64 carriers
     * measured +21.5% app over 256 at lower CPU (CHANGELOG 0.2.1). rlnc is
     * one group, and the sc-ldpc profiles ship an explicit 256, so both keep
     * the n + m default here. */
    if (!cfg->carriers)
    {
        u16 shares = (u16)(cfg->n + cfg->m);

        cfg->carriers = shares;
        if (cfg->fec && cfg->mode == 2 &&
            shares <= TBT_FEC_DEFAULT_GROUPED_CARRIERS)
            cfg->carriers = (u16)((TBT_FEC_DEFAULT_GROUPED_CARRIERS / shares) * shares);
    }
    if (cfg->carriers % (cfg->n + cfg->m))
        return tbt_fail(e, "carriers", "carriers must be a multiple of n + m");
    /* mode=rlnc is one group: carriers == n+m. Grouped/sc-ldpc may use more
     * carriers as long as the multiple holds. */
    if (cfg->fec && cfg->mode == 1 && cfg->carriers != cfg->n + cfg->m)
        return tbt_fail(e, "carriers", "mode=rlnc needs carriers = n + m");
    if (cfg->fec && cfg->m < 1)
        return tbt_fail(e, "m", "fec=on needs m>=1");
    if (cfg->fec && cfg->carriers < cfg->n + cfg->m)
        return tbt_fail(e, "carriers", "fec=on needs carriers >= n + m");
    /* mode=3 adaptive starts from a shipped codec profile on the rate
     * ladder: binary 1x256/L8/r0.5, or GF(16) 8x32/L16 with n/m on the
     * ladder. v8 names L/block/rate in the CFG extension for both variants. */
    if (cfg->adaptive && cfg->mode == 3)
    {
        struct tbt_fec_cfg fc;
        struct tbt_fec_adapt_sclc_wire wp;
        u16 shares = (u16)(cfg->n + cfg->m);
        u16 groups = (u16)(cfg->carriers / shares);

        memset(&fc, 0, sizeof(fc));
        fc.n = cfg->n;
        fc.m = cfg->m;
        fc.groups = groups;
        fc.codec_variant = 2;
        fc.rate_ppm =
            (u32)((((u64)cfg->n * 1000000u) + shares - 1) / shares);
        if (groups == 1 && shares == TBT_FEC_MAX_SHARES)
        {
            if (cfg->w != TBT_FEC_SCLC_BLOCK_DOWN ||
                (cfg->L && cfg->L != TBT_FEC_SCLC_L_DOWN))
                return tbt_fail(e, "adaptive",
                                "downlink adaptive needs block=256 L=8");
            fc.w = cfg->w;
            fc.k = (u16)((u32)cfg->n * 1000u / shares);
            fc.d = 0;
            fc.L = cfg->L ? cfg->L : TBT_FEC_SCLC_L_DOWN;
            fc.block = cfg->w;
        }
        else
        {
            if (cfg->w != TBT_FEC_SCLC_BLOCK_UP ||
                (cfg->L && cfg->L != TBT_FEC_SCLC_L_UP))
                return tbt_fail(e, "adaptive",
                                "uplink adaptive needs block=64 L=8");
            fc.w = cfg->L ? cfg->L : TBT_FEC_SCLC_L_UP;
            fc.k = cfg->w;
            fc.d = (u8)((u32)cfg->n * 256u / shares);
            fc.L = cfg->L ? cfg->L : TBT_FEC_SCLC_L_UP;
            fc.block = cfg->w;
        }
        if (tbt_fec_adapt_sclc_wire_of_cfg(&fc, cfg->carriers, &wp))
            return tbt_fail(e, "adaptive",
                            "mode=3 adaptive needs a supported profile on the rate ladder");
    }
    /* The old ARQ record names at most 16 carriers (path_ack_mask is u16 and
     * its FBK/WINDOW bodies hold 16 entries). A larger session is only
     * meaningful with FEC. */
    if (!cfg->fec && cfg->n + cfg->m > TBT_LEGACY_MAX_CARRIERS)
        return tbt_fail(e, "fec",
                        "legacy ARQ supports n+m<=16; set fec=on for 256 carriers");
    /* A near tunnel's carriers hold one source port each. A per-tunnel block
     * smaller than the effective carrier count can never bring the session
     * up; refuse it here so the operator sees the key and the reason. A far
     * tunnel never dials, so its portblock= is recorded but not sized. */
    if (cfg->role == TBT_ROLE_NEAR &&
        (cfg->portblock_lo || cfg->portblock_hi))
    {
        unsigned int slots = cfg->fec ? cfg->carriers
                                      : (unsigned int)cfg->n + cfg->m;

        if (!tbt_portmap_fits(cfg->portblock_lo, cfg->portblock_hi, slots))
            return tbt_fail(e, "portblock",
                            "portblock must hold at least n + m (or carriers) ports");
    }
    /* A sender window above the peer's reorder budget kills the stream on the
     * first frame past that budget, so the two keys are checked together once
     * both are known; the line may name them in either order. */
    /* Both defaults are 64 MiB. A line that lowers rx_reorder_bytes without
     * naming tx_window_bytes gets the matching smaller sender window instead
     * of failing the cross-check; an explicit tx_window_bytes is still
     * validated against rx_reorder_bytes. */
    if (!have_window && cfg->tx_window_bytes > cfg->rx_reorder_bytes)
        cfg->tx_window_bytes = cfg->rx_reorder_bytes;
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

/* Interface name for SO_BINDTODEVICE: 1..IFNAMSIZ-1 bytes, no whitespace,
 * control characters or '/'. The kernel's dev_valid_name() has the same
 * shape; keeping the check here makes the canonical line portable. */
static int tbt_parse_dev(const char *s, char out[IFNAMSIZ])
{
    size_t len, i;

    if (!s)
        return -EINVAL;
    len = strlen(s);
    if (!len || len >= IFNAMSIZ)
        return -EINVAL;
    for (i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (c <= ' ' || c == 0x7f || c == '/')
            return -EINVAL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
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
        return tbt_fail(e, "index", "carrier index must be 0..255");

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

/* `session <tunnel> nonce=<hex>` pins the 128-bit session nonce for
 * reproducible tests; `session <tunnel> adaptive=on|off` flips the FEC
 * control law at runtime. Both are runtime actions, never part of the
 * canonical config, and one line carries exactly one of them. */
static int tbt_parse_session(char *args, struct tbt_cmd *out, struct tbt_parse_error *e)
{
    char *tok = tbt_next_tok(&args);
    char *eq;

    if (!tok || tbt_parse_u64(tok, &out->id) || !out->id)
        return tbt_fail(e, "session", "session needs a non-zero tunnel id");
    tok = tbt_next_tok(&args);
    if (!tok)
        return tbt_fail(e, TBT_ACT_NONCE,
                        "session needs nonce=<hex> or adaptive=on|off");
    eq = strchr(tok, '=');
    if (!eq)
        return tbt_fail(e, tok, "expected nonce=<hex> or adaptive=on|off");
    *eq = '\0';
    if (!strcmp(tok, TBT_ACT_NONCE))
    {
        if (tbt_parse_nonce(eq + 1, out->nonce))
            return tbt_fail(e, TBT_ACT_NONCE, "nonce must be 16 or 32 hex digits");
        if (tbt_next_tok(&args))
            return tbt_fail(e, "session", "trailing tokens after session <tunnel> nonce=<hex>");
        return 0;
    }
    if (!strcmp(tok, "adaptive"))
    {
        if (!strcmp(eq + 1, "on"))
            out->adaptive = 1;
        else if (!strcmp(eq + 1, "off"))
            out->adaptive = 0;
        else
            return tbt_fail(e, "adaptive", "adaptive must be on or off");
        out->adaptive_set = true;
        if (tbt_next_tok(&args))
            return tbt_fail(e, "session",
                            "trailing tokens after session <tunnel> adaptive=on|off");
        return 0;
    }
    return tbt_fail(e, tok, "unknown key; session accepts nonce=<hex> or adaptive=on|off");
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

/* Minimal-width lowercase hexadecimal, 1..4 digits. */
static size_t tbt_fmt_hex16(char *out, u16 v)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    int shift;

    for (shift = 12; shift > 0 && !(v >> shift); shift -= 4)
        ;
    for (; shift >= 0; shift -= 4)
        out[n++] = hex[(v >> shift) & 0xf];
    return n;
}

/* RFC 5952: lowercase, no leading zeros, the longest run of two or more zero
 * groups (leftmost on a tie) compressed to "::". An IPv4-mapped address keeps
 * the ::ffff:a.b.c.d spelling, exactly as %pI6c renders it. */
static void tbt_fmt_ipv6(const u8 addr[16], char *out, size_t size)
{
    static const u8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    u16 g[8];
    int best = -1, best_len = 0;
    size_t n = 0;
    int i;

    if (size < TBT_ADDR_STRLEN)
    {
        if (size)
            out[0] = '\0';
        return;
    }
    if (!memcmp(addr, mapped, sizeof(mapped)))
    {
        out[n++] = ':';
        out[n++] = ':';
        out[n++] = 'f';
        out[n++] = 'f';
        out[n++] = 'f';
        out[n++] = 'f';
        out[n++] = ':';
        n += tbt_fmt_octet(out + n, addr[12]);
        out[n++] = '.';
        n += tbt_fmt_octet(out + n, addr[13]);
        out[n++] = '.';
        n += tbt_fmt_octet(out + n, addr[14]);
        out[n++] = '.';
        n += tbt_fmt_octet(out + n, addr[15]);
        out[n] = '\0';
        return;
    }
    for (i = 0; i < 8; i++)
        g[i] = (u16)((u16)addr[2 * i] << 8 | addr[2 * i + 1]);
    for (i = 0; i < 8;)
    {
        int j = i;

        while (j < 8 && !g[j])
            j++;
        if (j - i >= 2 && j - i > best_len)
        {
            best = i;
            best_len = j - i;
        }
        i = j > i ? j : i + 1;
    }
    for (i = 0; i < 8;)
    {
        if (i == best)
        {
            out[n++] = ':';
            out[n++] = ':';
            i += best_len;
            continue;
        }
        if (n && out[n - 1] != ':')
            out[n++] = ':';
        n += tbt_fmt_hex16(out + n, g[i]);
        i++;
    }
    out[n] = '\0';
}

void tbt_addr_format(const struct tbt_addr *a, char *out, size_t size)
{
    if (!out || !size)
        return;
    if (!a || !tbt_addr_set(a))
    {
        out[0] = '\0';
        return;
    }
    if (a->family == TBT_AF_INET6)
    {
        tbt_fmt_ipv6(a->u.v6, out, size);
        return;
    }
    if (size < 16)
    {
        out[0] = '\0';
        return;
    }
    tbt_fmt_ipv4(a->u.v4, out);
}

void tbt_endpoint_format(const struct tbt_addr *a, __be16 port, char *out, size_t size)
{
    char addr[TBT_ADDR_STRLEN];
    size_t n;
    u16 p;

    if (!out || !size)
        return;
    if (!a || !tbt_addr_set(a))
    {
        out[0] = '\0';
        return;
    }
    tbt_addr_format(a, addr, sizeof(addr));
    p = tbt_port_load(port);
    if (a->family == TBT_AF_INET6)
        n = (size_t)snprintf(out, size, "[%s]:%u", addr, p);
    else
        n = (size_t)snprintf(out, size, "%s:%u", addr, p);
    if (n >= size)
        out[0] = '\0';
}

static void tbt_fmt_hex(const u8 *in, size_t n, char *out);

int tbt_render_cfg(const struct tbt_cfg *c, char *buf, size_t size)
{
    static const char *const profiles[] = { "http-kal", "http-kal", "http-kal-drip", "raw" };
    static const char *const fec_modes[] = { "off", "on" };
    static const char *const modes[] = { "rlnc", "rlnc", "grouped", "sc-ldpc" };
    char ls[TBT_ENDPOINT_STRLEN], ps[TBT_ENDPOINT_STRLEN], ts[TBT_ENDPOINT_STRLEN];
    size_t off = 0;
    int i, rc;

    tbt_endpoint_format(&c->listen, c->listen_port, ls, sizeof(ls));
    tbt_endpoint_format(&c->peer, c->peer_port, ps, sizeof(ps));
    tbt_endpoint_format(&c->target, c->target_port, ts, sizeof(ts));

    rc = tbt_addf(buf, size, &off, "tunnel %llu role=%s listen=%s",
                  (unsigned long long)c->id,
                  c->role == TBT_ROLE_NEAR ? "near" : "far",
                  ls);
    if (rc)
        return rc;
    if (c->role == TBT_ROLE_NEAR)
    {
        rc = tbt_addf(buf, size, &off, " peer=%s", ps);
        if (rc)
            return rc;
        /* A near tunnel may name slot 0's target port explicitly; the address
         * is grammar filler (the far side always dials its own target_addr).
         * Rendered when set, so the line still round-trips. */
        if (c->target_port)
        {
            rc = tbt_addf(buf, size, &off, " target=%s", ts);
            if (rc)
                return rc;
        }
    }
    else
    {
        rc = tbt_addf(buf, size, &off, " target=%s", ts);
        if (rc)
            return rc;
        /* Far: the near source address allowed to join carriers.  The port is
         * grammar filler; only the address is used.  Omitted when unset, so a
         * far line without an anchor stays unchanged. */
        if (tbt_addr_set(&c->peer) && c->peer_port)
        {
            rc = tbt_addf(buf, size, &off, " peer=%s", ps);
            if (rc)
                return rc;
        }
    }
    if (c->nmap)
    {
        rc = tbt_addf(buf, size, &off, " map=");
        if (rc)
            return rc;
        for (i = 0; i < c->nmap && i < TBT_MAP_MAX - 1; i++)
        {
            rc = tbt_addf(buf, size, &off, "%s%u:%u", i ? "," : "",
                          c->maps[i].listen_port, c->maps[i].target_port);
            if (rc)
                return rc;
        }
    }
    {
        char kb[2 * TBT_CRYPTO_KEY_LEN + 1];

        if (tbt_read_bool(c->crypto_key_set))
            tbt_fmt_hex(c->crypto_key, sizeof(c->crypto_key), kb);
        else
        {
            kb[0] = '-';
            kb[1] = '\0';
        }
        /* T0 §4.2 order: n m fec adaptive mode w carriers crypto crypto_key dup
         * profile keepalive ... */
        rc = tbt_addf(buf, size, &off,
                      " n=%u m=%u fec=%s adaptive=%s mode=%s w=%u carriers=%u"
                      " crypto=%s crypto_key=%s dup=%u profile=%s keepalive=%u"
                      " drip_ms=%u replay=%u tunnel_rate=%u"
                      " max_inflate=%u.%03u hole_budget_ms=%u rx_reorder_bytes=%u"
                      " tx_window_bytes=%u tx_retain_bytes=%u carrier_stall_ms=%u"
                      " portblock=%u-%u",
                      c->n, c->m, fec_modes[c->fec ? 1 : 0],
                      c->adaptive ? "on" : "off",
                      modes[c->mode & 3], c->w, c->carriers,
                      c->crypto ? "on" : "off", kb, c->dup,
                      profiles[c->profile & 3], c->keepalive_ms,
                      c->drip_ms, c->replay, c->tunnel_rate,
                      c->max_inflate_milli / 1000, c->max_inflate_milli % 1000,
                      c->hole_budget_ms, c->rx_reorder_bytes, c->tx_window_bytes,
                      c->tx_retain_bytes, c->carrier_stall_ms, c->portblock_lo,
                      c->portblock_hi);
        if (rc)
            return rc;
        /* L exists only for mode=3; rendering it for every mode would rewrite
         * every stored line. The auto value renders as 0 and parses back to
         * 0, so the canonical form round-trips. */
        if (c->mode == 3)
        {
            rc = tbt_addf(buf, size, &off, " L=%u", c->L);
            if (rc)
                return rc;
        }
    }
    if (rc)
        return rc;
    if (tbt_read_bool(c->dev_set))
    {
        rc = tbt_addf(buf, size, &off, " dev=%s", c->dev);
        if (rc)
            return rc;
    }
    if (tbt_read_bool(c->nonce_set))
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
            tbt_addr_format(&c->srcaddrs[i], ls, sizeof(ls));
            rc = tbt_addf(buf, size, &off, i ? ",%s" : "%s", ls);
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
        if (tbt_read_bool(c->adaptive_set))
            rc = tbt_addf(buf, size, &off, "session %llu adaptive=%s\n",
                          (unsigned long long)c->id, c->adaptive ? "on" : "off");
        else
        {
            tbt_fmt_hex(c->nonce, sizeof(c->nonce), nonce);
            rc = tbt_addf(buf, size, &off, "session %llu nonce=%s\n",
                          (unsigned long long)c->id, nonce);
        }
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
