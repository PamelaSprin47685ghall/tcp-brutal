/* tbt_parse.h - the /proc/net/tbtun/config grammar, free of kernel state.
 *
 * This header and tbt_parse.c compile twice: into the module, and into the
 * host conformance test (host/test_parse.c). They therefore use no kernel
 * facility that a plain C program lacks, and hold no locks, no globals and no
 * procfs types. One definition of struct tbt_cfg serves both builds, so the
 * test cannot drift from the module.
 *
 * The grammar is DESIGN.md §8.3; the line rules are §8.4.1. Tunnel keys render
 * canonically in the config view, and the list keys (map, srcaddrs) only when
 * set; the action verbs (carrier, session) are
 * runtime actions and are rendered only by tbt_render_action(), for the
 * tunnels/peers state view.
 */
#ifndef _TBT_PARSE_H
#define _TBT_PARSE_H

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/printk.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint16_t __be16;
typedef uint32_t __be32;
#endif

#include "tbt_uapi.h"
#include "tbt_crypto.h"

/* Read a stored flag as a byte, then normalize it to true/false. A `_Bool`
 * load assumes the value is 0 or 1; a byte left neither by a neighbouring
 * write would otherwise be undefined and trip UBSAN's invalid-load check.
 * The kernel form keeps the compile-time type check and names a bad writer;
 * the host form only normalizes, so tbt_parse.c builds with a plain C
 * compiler and -Werror. */
#ifdef __KERNEL__
#define tbt_read_bool(field) \
    ({ \
        u8 __tbt_bool_byte; \
        BUILD_BUG_ON(!__builtin_types_compatible_p(__typeof__(field), bool)); \
        __tbt_bool_byte = READ_ONCE(*(const u8 *)&(field)); \
        if (__tbt_bool_byte > 1) \
            pr_warn_ratelimited("tbtun: %s: %s is not a bool byte (%u)\n", \
                                __func__, #field, __tbt_bool_byte); \
        __tbt_bool_byte != 0; \
    })
#else
static inline bool tbt_read_bool_at(const void *p)
{
    return *(const volatile u8 *)p != 0;
}
#define tbt_read_bool(field) tbt_read_bool_at(&(field))
#endif

/* Line buffer size. A line of TBT_LINE_MAX - 1 bytes is the longest accepted;
 * TBT_LINE_MAX itself is refused, so the buffer always has room for the NUL. */
#define TBT_LINE_MAX 1024
#define TBT_SRC_MAX 8      /* srcaddrs= entries recorded per tunnel */

/* One additional port mapping. Slot 0 is the listen=/target= pair; map=
 * entries fill slots 1..TBT_MAP_MAX-1 in the order written. Ports are host
 * order: they are compared and rendered as numbers, and only the listener
 * converts its own copy to a sockaddr. A near tunnel listens on
 * listen_port and routes the stream to target_port; a far tunnel ignores
 * listen_port and accepts target_port as an allowed destination. */
struct tbt_map
{
    u16 listen_port;
    u16 target_port;
};

/* Parse failures are negative errno values so the write() path can return them
 * unchanged and a shell sees a familiar errno. */
#define TBT_PARSE_EINVAL (-EINVAL)
#define TBT_PARSE_E2BIG (-E2BIG)
#define TBT_PARSE_EAFNOSUPPORT (-EAFNOSUPPORT)

/* Parsed tunnel configuration: one `tunnel` line, defaults filled in. */
struct tbt_cfg
{
    u64 id;
    u8 role;    /* TBT_ROLE_NEAR | TBT_ROLE_FAR */
    u8 profile; /* 1 http-kal, 2 http-kal-drip, 3 raw; never 0 after parsing */
    u8 fec;     /* 0 off (old ARQ), 1 on; parsed and rendered, no data plane */
    u8 adaptive; /* fec=on: 1 lets the control law choose n/m, 0 keeps the
                  * configured pair. Defaults to fec; requires fec=on. The
                  * configured n/m are the initial shape and the upper hint. */
    u8 mode;    /* 1 rlnc, 2 grouped, 3 sc-ldpc; never 0 after parsing */
    u16 w;      /* effective window: 1..1024, or 2*n / 256 resolved from w=0 */
    u16 L;      /* sc-ldpc coupling length; 0 = the variant default. Not a
                 * wire field: both ends must configure it, and 0.3.0 keeps
                 * the shape fixed, so the value only has to match within a
                 * session. */
    u16 carriers; /* effective slots: a multiple of n+m, or n+m resolved from 0 */
    __be32 listen_addr;
    __be16 listen_port;
    __be32 peer_addr;
    __be16 peer_port;
    __be32 target_addr;
    __be16 target_port;
    u8 nmap;                    /* extra mappings; slot 0 is listen/target */
    struct tbt_map maps[TBT_MAP_MAX - 1];
    u16 n;
    u16 m;
    u8 dup;
    u32 keepalive_ms;
    /* Per-tunnel source port window for a near tunnel's carriers (0-0 =
     * none: draw from the global /proc/net/tbtun/ports block). The parser
     * validates the range and requires room for every effective carrier
     * slot; a far tunnel records it but never dials. */
    u16 portblock_lo;
    u16 portblock_hi;

    /* List key. Only srcaddrs[0] is read by the near-side carrier bind; the
     * remaining entries are stored and rendered so the line round-trips. */
    u8 nsrc;
    u32 drip_ms;
    u32 replay;
    u32 tunnel_rate;
    u32 max_inflate_milli; /* 2.000 renders as 2000 */
    u32 hole_budget_ms;   /* hole timeout, 0 = auto (the default) */
    u32 rx_reorder_bytes; /* C3 reorder window, default 67108864 */
    u32 tx_window_bytes;  /* sender window, default 67108864 */
    u32 tx_retain_bytes;  /* C3 replay retention, 0 = unlimited */
    u32 carrier_stall_ms; /* C3 stall detector, 0 = auto (the default) */
    __be32 srcaddrs[TBT_SRC_MAX];
    /* Near carriers bind their dialling socket to this interface
     * (SO_BINDTODEVICE) before connect, so a TUN policy route cannot capture
     * them. Empty unless dev= was present. */
    char dev[IFNAMSIZ];
    bool dev_set;

    /* Tunnel nonce. 32 hex digits = the full 128-bit value; 16 hex digits =
     * the low 64 bits, left-padded with zeros (the same rule as the runtime
     * `session <id> nonce=` action). A near tunnel uses it as its local
     * session nonce; a far tunnel pins the nonce it expects from the near
     * side instead of adopting the first HELLO (trust-on-first-use). */
    u8 nonce[16];
    bool nonce_set; /* nonce= was present on the tunnel line */

    /* ChaCha8 body obfuscation (T0 §2.4, §4). `crypto` is 0 or 1; the
     * default is 1 here and becomes `fec` once B's fec= field is merged
     * (fec=off has no decrypt point, so it defaults off there).
     * `crypto_key_set` pins the 256-bit key; otherwise the near side
     * generates one per session and HELLO v6 carries it in clear text. */
    u8 crypto;
    bool crypto_key_set;
    u8 crypto_key[TBT_CRYPTO_KEY_LEN];
};

enum tbt_cmd_kind
{
    TBT_CMD_NONE = 0, /* blank line or comment */
    TBT_CMD_TUNNEL,
    TBT_CMD_DEL,
    TBT_CMD_FLUSH,
    TBT_CMD_CARRIER, /* runtime action, not configuration */
    TBT_CMD_SESSION, /* runtime action, not configuration */
};

enum tbt_carrier_action
{
    TBT_CARRIER_DOWN = 1,
    TBT_CARRIER_UP, /* inverse of down; does not dial, the replacement worker does */
    TBT_CARRIER_DROP_PPM,
};

struct tbt_cmd
{
    enum tbt_cmd_kind kind;
    u64 id; /* TBT_CMD_DEL, TBT_CMD_SESSION */
    struct tbt_cfg cfg;

    /* TBT_CMD_CARRIER: `carrier <index> ...`, no tunnel id (C3 has one trunk) */
    u16 carrier;       /* index, 0..TBT_MAX_CARRIERS_LIMIT-1 */
    u8 carrier_action; /* enum tbt_carrier_action */
    u32 drop_ppm;      /* TBT_CARRIER_DROP_PPM: 0..TBT_DROP_PPM_MAX; applying it
                        * requires the module parameter debug_loss=1 */

    /* TBT_CMD_SESSION: pinned 128-bit nonce */
    u8 nonce[16];
    /* TBT_CMD_SESSION: adaptive=on|off. Exactly one of nonce/adaptive is
     * carried, so `session <id> adaptive=off` never touches the nonce. */
    u8 adaptive;
    bool adaptive_set;
};

/* Per-open-file /proc config line buffer (DESIGN.md §8.4.1). Pure data: the
 * writer in tbt_procfs.c owns it, nothing here touches it. */
struct tbt_linebuf
{
    char buf[TBT_LINE_MAX];
    u16 len;
    bool overflow; /* a line exceeded TBT_LINE_MAX; refuse until newline */
};

/* Why a line was refused. `key` is NULL when the fault is not tied to one key;
 * otherwise it points either at a literal or into the line being parsed, so a
 * caller must report it before reusing the buffer. */
struct tbt_parse_error
{
    const char *key;
    const char *why;
};

/* Parse one line in place (NUL-terminated, mutable, shorter than
 * TBT_LINE_MAX). Returns 0 on success and fills *out, including for a blank
 * line or comment (TBT_CMD_NONE). On failure returns TBT_PARSE_EINVAL,
 * TBT_PARSE_E2BIG or TBT_PARSE_EAFNOSUPPORT and fills *err.
 *
 * Crash-regression contract: this function takes the line, the command and the
 * error sink, and nothing else. It never reads `current`, a net namespace, or
 * any task state, so a line flushed from ->release() cannot fault on task
 * context. Keep it that way: no kernel-only globals, locks or allocation. */
int tbt_parse_line(char *line, struct tbt_cmd *out, struct tbt_parse_error *err);

/* Parse the `ports lo-hi` line of /proc/net/tbtun/ports. A blank line, or
 * `ports 0-0`, leaves *lo and *hi zero and returns 0; the caller treats that
 * as a no-op and leaves the block unchanged. */
int tbt_parse_ports(const char *line, u16 *lo, u16 *hi, struct tbt_parse_error *err);

/* Render one tunnel in canonical form: every scalar key, defaults included,
 * in a fixed order; the list key (srcaddrs) only when set. Returns
 * the length written (excluding the NUL), or TBT_PARSE_E2BIG if it does not
 * fit. The result parses back into an identical struct. */
int tbt_render_cfg(const struct tbt_cfg *c, char *buf, size_t size);

/* Render a parsed action verb (TBT_CMD_CARRIER or TBT_CMD_SESSION) in
 * canonical form, for the tunnels/peers state view. Returns the length written
 * (excluding the NUL), TBT_PARSE_EINVAL for any other command kind, or
 * TBT_PARSE_E2BIG if it does not fit. The result parses back into an identical
 * command. */
int tbt_render_action(const struct tbt_cmd *cmd, char *buf, size_t size);

/* One read-only /proc/net/tbtun/stats line: "debug_loss=<0|1>\n". The module
 * parameter gates drop_ppm; the stats line makes its state visible. Returns
 * the length written (excluding the NUL) or TBT_PARSE_E2BIG. */
int tbt_render_debug_loss(bool enabled, char *buf, size_t size);

/* The stable leading block of /proc/net/tbtun/stats: debug_loss first, then
 * the global counters sorted by key, one `key=value\n` per line with no
 * spaces. Per-tunnel counter lines follow and are not part of this contract.
 * Returns the length written (excluding the NUL) or TBT_PARSE_E2BIG. */
int tbt_render_stats_prefix(bool debug_loss, u64 gaps_total, u64 streams_total, char *buf,
                            size_t size);

#endif /* _TBT_PARSE_H */
