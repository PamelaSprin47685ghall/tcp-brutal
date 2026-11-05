/* test_parse.c - host conformance test for the /proc/net/tbtun grammar.
 *
 * Links tbtun/tbt_parse.c, the same object the module is built from, so a rule
 * that passes here is a rule the kernel enforces. No root, no module, no
 * kernel headers, no fixture files: it runs in milliseconds and exits non-zero
 * on the first broken rule (it keeps going, to list every failure).
 *
 *   make host-test
 */
#include <stdio.h>
#include <string.h>

#include "tbt_parse.h"

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

/* Parse a copy of `text`; the parser writes NULs into the line. */
static int parse(const char *text, struct tbt_cmd *cmd, struct tbt_parse_error *perr)
{
    char buf[TBT_LINE_MAX + 1];
    static char keybuf[TBT_LINE_MAX + 1];
    int rc;

    if (strlen(text) > TBT_LINE_MAX)
        return -1; /* the caller is testing over-long input separately */
    strcpy(buf, text);
    rc = tbt_parse_line(buf, cmd, perr);
    /* perr->key may point into buf, which dies with this frame; keep a copy so
     * the caller can report it (tbt_parse.h documents that rule). */
    if (perr->key)
    {
        snprintf(keybuf, sizeof(keybuf), "%s", perr->key);
        perr->key = keybuf;
    }
    return rc;
}

/* Parse `text`, render it, and compare with `want` byte for byte. */
static void roundtrip(const char *what, const char *text, const char *want)
{
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    char out[TBT_LINE_MAX * 2];
    int n;

    if (parse(text, &cmd, &perr))
    {
        ok(0, what, "input was refused");
        return;
    }
    n = tbt_render_cfg(&cmd.cfg, out, sizeof(out));
    if (n < 0)
    {
        ok(0, what, "render failed");
        return;
    }
    ok(cmd.kind == TBT_CMD_TUNNEL, what, "not parsed as a tunnel");
    ok(!strcmp(out, want), what, out);
}

static void refuse(const char *what, const char *text, int want_err, const char *want_key,
                   const char *want_why)
{
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    int rc = parse(text, &cmd, &perr);

    ok(rc == want_err, what, rc == 0 ? "accepted" : "wrong errno");
    if (want_key)
        ok(perr.key && !strcmp(perr.key, want_key), what, perr.key ? perr.key : "no key");
    if (want_why)
        ok(perr.why && strstr(perr.why, want_why), what, perr.why ? perr.why : "no reason");
}

/* An action verb must render to exactly `want`, and that text must parse back
 * into the same command: this is the contract the kernel-side parser and the
 * peers renderer must meet. */
static void action_roundtrip(const char *what, const char *text, const char *want)
{
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    char out[256];
    int n;

    if (parse(text, &cmd, &perr))
    {
        ok(0, what, "input was refused");
        return;
    }
    n = tbt_render_action(&cmd, out, sizeof(out));
    if (n < 0)
    {
        ok(0, what, "render failed");
        return;
    }
    ok(!strcmp(out, want), what, out);
    {
        struct tbt_cmd again;
        struct tbt_parse_error perr2 = { NULL, NULL };

        if (parse(out, &again, &perr2))
        {
            ok(0, what, "rendered line was refused");
            return;
        }
        ok(again.kind == cmd.kind && again.id == cmd.id && again.carrier == cmd.carrier &&
               again.carrier_action == cmd.carrier_action && again.drop_ppm == cmd.drop_ppm &&
               !memcmp(again.nonce, cmd.nonce, sizeof(cmd.nonce)),
           what, "round-trip changed the action");
    }
}

static void test_actions(void)
{
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    char buf[TBT_LINE_MAX * 2];

    /* Canonical text, exactly as the kernel side must accept it. */
    action_roundtrip("carrier 0 down", "carrier 0 down", "carrier 0 down\n");
    action_roundtrip("carrier 15 down", "carrier 15 down", "carrier 15 down\n");
    action_roundtrip("carrier index leading zeros", "carrier 03 down", "carrier 3 down\n");
    action_roundtrip("carrier 0 up", "carrier 0 up", "carrier 0 up\n");
    action_roundtrip("carrier 15 up", "carrier 15 up", "carrier 15 up\n");
    action_roundtrip("carrier 3 up", "carrier 3 up", "carrier 3 up\n");
    action_roundtrip("carrier drop_ppm 0", "carrier 3 drop_ppm 0", "carrier 3 drop_ppm 0\n");
    action_roundtrip("carrier drop_ppm max", "carrier 3 drop_ppm 1000000", "carrier 3 drop_ppm 1000000\n");
    action_roundtrip("drop_ppm leading zeros", "carrier 3 drop_ppm 00042", "carrier 3 drop_ppm 42\n");
    action_roundtrip("session nonce 128-bit", "session 7 nonce=00112233445566778899aabbccddeeff",
                     "session 7 nonce=00112233445566778899aabbccddeeff\n");
    action_roundtrip("session nonce 64-bit pin", "session 7 nonce=0011223344556677",
                     "session 7 nonce=00000000000000000011223344556677\n");
    action_roundtrip("session nonce uppercase", "session 7 nonce=00112233445566778899AABBCCDDEEFF",
                     "session 7 nonce=00112233445566778899aabbccddeeff\n");
    ok(parse("carrier 3 down\r\n", &cmd, &perr) == 0 && cmd.kind == TBT_CMD_CARRIER,
       "action line with CRLF", NULL);

    /* Action verbs live outside the config view. */
    parse("tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2", &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(!strstr(buf, "carrier ") && !strstr(buf, "drop_ppm") && !strstr(buf, "session "),
       "config render has no action verbs", buf);
    ok(!strncmp(buf, "tunnel ", 7), "config render is a tunnel line", buf);
    ok(tbt_render_action(&cmd, buf, sizeof(buf)) == TBT_PARSE_EINVAL,
       "a config command is not an action", NULL);

    refuse("carrier index 16", "carrier 16 down", TBT_PARSE_EINVAL, "index", "0..15");
    refuse("carrier index missing", "carrier", TBT_PARSE_EINVAL, "index", "0..15");
    refuse("carrier action missing", "carrier 3", TBT_PARSE_EINVAL, "carrier", "down, up or drop_ppm");
    refuse("carrier action unknown", "carrier 3 side", TBT_PARSE_EINVAL, "side", "down, up or drop_ppm");
    refuse("drop_ppm over max", "carrier 3 drop_ppm 1000001", TBT_PARSE_EINVAL, "drop_ppm", "0..1000000");
    refuse("drop_ppm missing", "carrier 3 drop_ppm", TBT_PARSE_EINVAL, "drop_ppm", "0..1000000");
    refuse("drop_ppm not a number", "carrier 3 drop_ppm x", TBT_PARSE_EINVAL, "drop_ppm", "0..1000000");
    refuse("carrier trailing token", "carrier 3 down extra", TBT_PARSE_EINVAL, "carrier", "trailing");
    refuse("carrier up trailing token", "carrier 3 up extra", TBT_PARSE_EINVAL, "carrier", "trailing");
    refuse("drop_ppm trailing token", "carrier 3 drop_ppm 1 extra", TBT_PARSE_EINVAL, "carrier", "trailing");
    refuse("carrier with a tunnel id", "carrier 1 3 drop_ppm 1000001", TBT_PARSE_EINVAL, "3",
           "down, up or drop_ppm");
    refuse("up index 16", "carrier 16 up", TBT_PARSE_EINVAL, "index", "0..15");
    refuse("up without index", "carrier up", TBT_PARSE_EINVAL, "index", "0..15");

    refuse("session id 0", "session 0 nonce=00112233445566778899aabbccddeeff", TBT_PARSE_EINVAL, "session",
           "non-zero");
    refuse("session without nonce", "session 7", TBT_PARSE_EINVAL, "nonce", "nonce");
    refuse("session short nonce", "session 7 nonce=0011", TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("session non-hex nonce", "session 7 nonce=zz112233445566778899aabbccddeeff", TBT_PARSE_EINVAL,
           "nonce", "hex");
    refuse("session unknown key", "session 7 frob=1", TBT_PARSE_EINVAL, "frob", "unknown key");
    refuse("session trailing token", "session 7 nonce=00112233445566778899aabbccddeeff extra",
           TBT_PARSE_EINVAL, "session", "trailing");

    refuse("unknown action verb", "carrierx 1 down", TBT_PARSE_EINVAL, "carrierx", "unknown directive");
    refuse("peers is read-only", "peers", TBT_PARSE_EINVAL, "peers", "unknown directive");

    /* The debug_loss gate is visible in stats, and its refusal text names the
     * parameter the operator must set. */
    {
        char line[32];

        ok(tbt_render_debug_loss(false, line, sizeof(line)) == 13 && !strcmp(line, "debug_loss=0\n"),
           "stats debug_loss=0", line);
        ok(tbt_render_debug_loss(true, line, sizeof(line)) == 13 && !strcmp(line, "debug_loss=1\n"),
           "stats debug_loss=1", line);
        ok(tbt_render_debug_loss(true, line, 5) == TBT_PARSE_E2BIG, "short stats buffer refused", NULL);
    }
    ok(strstr(TBT_DROP_PPM_GATE_MSG, "debug_loss=1") != NULL,
       "drop_ppm refusal names debug_loss=1", TBT_DROP_PPM_GATE_MSG);
}

/* Tunnel-level nonce=: the same 128-bit / 64-bit-pin rule as the runtime
 * `session <id> nonce=` action, but part of the canonical config. */
static void test_tunnel_nonce(void)
{
    static const char *const base_near =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2";
    static const char *const canon_near =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
        " n=4 m=1 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
        " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
        " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
        " portblock=0-0 nonce=00112233445566778899aabbccddeeff\n";
    char text[TBT_LINE_MAX];
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    char out[TBT_LINE_MAX * 2];

    snprintf(text, sizeof(text), "%s nonce=00112233445566778899aabbccddeeff", base_near);
    roundtrip("tunnel nonce 128-bit", text, canon_near);

    /* 16 hex digits pin the low 64 bits and render left-padded. */
    snprintf(text, sizeof(text), "%s nonce=0011223344556677", base_near);
    roundtrip("tunnel nonce 64-bit pin", text,
              "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
              " n=4 m=1 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
              " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
              " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
              " portblock=0-0 nonce=00000000000000000011223344556677\n");

    /* Uppercase input normalises to lowercase. */
    snprintf(text, sizeof(text), "%s nonce=00112233445566778899AABBCCDDEEFF", base_near);
    roundtrip("tunnel nonce uppercase", text, canon_near);

    /* A far tunnel accepts the key too; it is the pinned peer nonce. */
    roundtrip("tunnel nonce far",
              "tunnel 2 role=far listen=10.0.0.1:1 target=10.0.0.2:2"
              " nonce=00112233445566778899aabbccddeeff",
              "tunnel 2 role=far listen=10.0.0.1:1 target=10.0.0.2:2"
              " n=4 m=1 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
              " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
              " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
              " portblock=0-0 nonce=00112233445566778899aabbccddeeff\n");

    /* A line without the key must not grow one. */
    parse(base_near, &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, out, sizeof(out));
    ok(!strstr(out, "nonce="), "no nonce= when unset", out);
    ok(!cmd.cfg.nonce_set, "nonce_set is false by default", NULL);

    /* Invalid lengths and characters are refused with -EINVAL. */
    refuse("tunnel nonce empty",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 nonce=",
           TBT_PARSE_EINVAL, "nonce", "empty");
    refuse("tunnel nonce 4 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 nonce=0011",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce 15 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=001122334455667",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce 17 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=00112233445566778",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce 31 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=00112233445566778899aabbccddee",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce 33 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=00112233445566778899aabbccddeeff0",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce 64 hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=00112233445566778899aabbccddeeff"
           "00112233445566778899aabbccddeeff",
           TBT_PARSE_EINVAL, "nonce", "16 or 32");
    refuse("tunnel nonce non-hex",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
           " nonce=zz112233445566778899aabbccddeeff",
           TBT_PARSE_EINVAL, "nonce", "hex");
}

/* --- crypto= / crypto_key= (T0 §4): default, off, key round-trip, clear,
 * and the refusals. The canonical position is right after m=. -------------- */
static void test_crypto_cfg(void)
{
    static const char *const base =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2";
    static const char *const canon_on =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
        " n=4 m=1 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
        " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
        " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
        " portblock=0-0\n";
    static const char *const canon_off =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
        " n=4 m=1 crypto=off crypto_key=- dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
        " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
        " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
        " portblock=0-0\n";
    static const char *const key_hex =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    char text[TBT_LINE_MAX];
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };

    /* Default: on, key unset. */
    roundtrip("crypto default on", base, canon_on);
    parse(base, &cmd, &perr);
    ok(cmd.cfg.crypto == 1, "crypto defaults to on", NULL);
    ok(!cmd.cfg.crypto_key_set, "crypto_key is unset by default", NULL);

    /* crypto=off round-trips and is reported. */
    snprintf(text, sizeof(text), "%s crypto=off", base);
    roundtrip("crypto off", text, canon_off);
    parse(text, &cmd, &perr);
    ok(cmd.cfg.crypto == 0, "crypto=off is stored", NULL);

    /* crypto=on is explicit and normalises to the same default. */
    snprintf(text, sizeof(text), "%s crypto=on", base);
    roundtrip("crypto on", text, canon_on);

    /* crypto_key= stores 64 hex digits and renders them. */
    snprintf(text, sizeof(text), "%s crypto_key=%s", base, key_hex);
    {
        char want[TBT_LINE_MAX * 2];

        snprintf(want, sizeof(want), "%s",
                 "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
                 " n=4 m=1 crypto=on crypto_key=");
        strncat(want, key_hex, sizeof(want) - strlen(want) - 1);
        strncat(want,
                " dup=1 profile=http-kal keepalive=5000 drip_ms=1000"
                " replay=8388608 tunnel_rate=0 max_inflate=2.000 hole_budget_ms=500"
                " rx_reorder_bytes=262144 tx_retain_bytes=0 carrier_stall_ms=3000"
                " portblock=0-0\n",
                sizeof(want) - strlen(want) - 1);
        roundtrip("crypto_key set", text, want);
    }
    parse(text, &cmd, &perr);
    ok(cmd.cfg.crypto_key_set, "crypto_key_set is true", NULL);
    ok(cmd.cfg.crypto_key[0] == 0x00 && cmd.cfg.crypto_key[31] == 0x1f,
       "crypto_key bytes are big-endian hex", NULL);

    /* Uppercase hex is accepted and stored byte-identically. */
    {
        char upper[sizeof(text)];
        char upper_key[64 + 1];
        size_t i;

        for (i = 0; key_hex[i]; i++)
            upper_key[i] = (key_hex[i] >= 'a' && key_hex[i] <= 'f')
                               ? (char)(key_hex[i] - 'a' + 'A')
                               : key_hex[i];
        upper_key[i] = '\0';
        snprintf(upper, sizeof(upper), "%s crypto_key=%s", base, upper_key);
        parse(upper, &cmd, &perr);
        ok(cmd.cfg.crypto_key_set && cmd.cfg.crypto_key[0] == 0x00 &&
               cmd.cfg.crypto_key[31] == 0x1f,
           "crypto_key uppercase hex is accepted", perr.why);
    }

    /* crypto_key=- clears a previous key. */
    snprintf(text, sizeof(text), "%s crypto_key=%s crypto_key=-", base, key_hex);
    roundtrip("crypto_key clear", text, canon_on);

    /* Refusals. */
    snprintf(text, sizeof(text), "%s crypto=maybe", base);
    refuse("crypto value", text, TBT_PARSE_EINVAL, "crypto", "on or off");
    snprintf(text, sizeof(text), "%s crypto=1", base);
    refuse("crypto numeric", text, TBT_PARSE_EINVAL, "crypto", "on or off");
    snprintf(text, sizeof(text), "%s crypto_key=0011", base);
    refuse("crypto_key short", text, TBT_PARSE_EINVAL, "crypto_key", "64 hex");
    snprintf(text, sizeof(text), "%s crypto_key=%s00", base, key_hex);
    refuse("crypto_key long", text, TBT_PARSE_EINVAL, "crypto_key", "64 hex");
    snprintf(text, sizeof(text), "%s crypto_key=zz%s", base, key_hex + 2);
    refuse("crypto_key non-hex", text, TBT_PARSE_EINVAL, "crypto_key", "64 hex");
    snprintf(text, sizeof(text), "%s crypto_key=", base);
    refuse("crypto_key empty", text, TBT_PARSE_EINVAL, "crypto_key", "empty");
}

/* --- stats prefix: a key=value parser, so a stray space or a missing '='
 * in a future counter fails this test rather than an operator's script. ---- */

struct kv_pair
{
    char key[32];
    u64 val;
};

/* Parse `key=value\n` lines: exactly one '=' per line, no spaces or tabs in
 * the key, a non-empty decimal value, and a newline after every line. */
static int parse_kv_block(const char *text, struct kv_pair *out, size_t max, size_t *count)
{
    const char *p = text;
    size_t n = 0;

    while (*p)
    {
        const char *eol = strchr(p, '\n');
        const char *eq;
        size_t klen, vlen, i;
        u64 v = 0;

        if (!eol)
            return -1;
        eq = memchr(p, '=', (size_t)(eol - p));
        if (!eq || eq == p)
            return -1;
        klen = (size_t)(eq - p);
        vlen = (size_t)(eol - eq - 1);
        if (!vlen || klen >= sizeof(out[0].key) || n == max)
            return -1;
        for (i = 0; i < klen; i++)
        {
            if (p[i] == ' ' || p[i] == '\t')
                return -1;
            out[n].key[i] = p[i];
}
        out[n].key[klen] = '\0';
        for (i = 0; i < vlen; i++)
        {
            if (eq[1 + i] < '0' || eq[1 + i] > '9')
                return -1;
            v = v * 10 + (u64)(eq[1 + i] - '0');
}
        out[n].val = v;
        n++;
        p = eol + 1;
    }
    *count = n;
    return 0;
}

static size_t count_words(const char *s)
{
    size_t n = 0;

    while (*s)
    {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        n++;
        while (*s && *s != ' ')
            s++;
    }
    return n;
}

static size_t count_lines(const char *s)
{
    size_t n = 0;

    for (; *s; s++)
        if (*s == '\n')
            n++;
    return n;
}

static void test_stats_prefix(void)
{
    static const char *const want[] = { "debug_loss", "gaps_total", "streams_total" };
    char text[128], again[128];
    struct kv_pair kv[8];
    size_t n = 0, i, off = 0;

    ok(tbt_render_stats_prefix(true, 7, 42, text, sizeof(text)) > 0, "stats prefix renders", NULL);
    ok(!strcmp(text, "debug_loss=1\ngaps_total=7\nstreams_total=42\n"),
       "stats prefix is key=value, debug_loss first, counters sorted", text);
    ok(count_lines(text) == 3, "stats prefix is exactly three lines", NULL);
    ok(parse_kv_block(text, kv, 8, &n) == 0, "stats prefix parses as key=value", NULL);
    ok(n == 3, "stats prefix has three keys", NULL);
    for (i = 0; i < n && i < 3; i++)
        ok(!strcmp(kv[i].key, want[i]), "stats key order", kv[i].key);
    for (i = 0; i < n; i++)
        off += (size_t)snprintf(again + off, sizeof(again) - off, "%s=%llu\n", kv[i].key,
                                (unsigned long long)kv[i].val);
    ok(!strcmp(again, text), "stats prefix round-trips", again);
    ok(tbt_render_stats_prefix(false, 0, 0, text, 8) == TBT_PARSE_E2BIG,
       "short stats buffer refused", NULL);

    /* A stray space, a missing '=', or an empty value is a contract break. */
    ok(parse_kv_block("gaps_total 7\n", kv, 8, &n) != 0, "space-separated counter rejected", NULL);
    ok(parse_kv_block("gaps_total7\n", kv, 8, &n) != 0, "missing = rejected", NULL);
    ok(parse_kv_block("gaps_total=\n", kv, 8, &n) != 0, "empty value rejected", NULL);
}

/* A valid tunnel line padded with spaces to exactly `len` content bytes.
 * Trailing spaces are between-token whitespace, so the line stays valid. */
static void make_line(char *out, size_t len)
{
    static const char base[] = "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2";
    size_t blen = strlen(base);

    memcpy(out, base, blen);
    memset(out + blen, ' ', len - blen);
    out[len] = '\0';
}

static void test_line_limit(void)
{
    char line[TBT_LINE_MAX + 2];
    struct tbt_cmd cmd;
    struct tbt_parse_error perr;

    make_line(line, TBT_LINE_MAX - 1);
    ok(tbt_parse_line(line, &cmd, &perr) == 0 && cmd.kind == TBT_CMD_TUNNEL,
       "line at TBT_LINE_MAX-1 accepted", NULL);
    make_line(line, TBT_LINE_MAX);
    ok(tbt_parse_line(line, &cmd, &perr) == TBT_PARSE_E2BIG, "line at TBT_LINE_MAX refused", NULL);
    make_line(line, TBT_LINE_MAX + 1);
    ok(tbt_parse_line(line, &cmd, &perr) == TBT_PARSE_E2BIG, "line at TBT_LINE_MAX+1 refused", NULL);
}

/* Every key at its maximum, so the canonical line is as long as the grammar
 * allows. */
static void maximal_cfg(struct tbt_cfg *c, u64 id)
{
    int i;

    memset(c, 0, sizeof(*c));
    c->id = id;
    c->role = TBT_ROLE_FAR;
    c->listen_addr = (__be32)0xffffffffu;
    c->listen_port = (__be16)0xffff;
    c->peer_addr = (__be32)0xffffffffu;
    c->peer_port = (__be16)0xffff;
    c->target_addr = (__be32)0xffffffffu;
    c->target_port = (__be16)0xffff;
    c->n = TBT_MAX_CARRIERS_LIMIT;
    c->m = 0;
    c->dup = 3;
    c->keepalive_ms = 86400000;
    c->portblock_lo = 65535;
    c->portblock_hi = 65535;
    c->profile = 2; /* http-kal-drip, the longest name */
    c->drip_ms = 3600000;
    c->replay = 268435456;
    c->tunnel_rate = 0xffffffffu;
    c->max_inflate_milli = 1000000;
    c->hole_budget_ms = 60000;
    c->rx_reorder_bytes = 268435456;
    c->tx_retain_bytes = 0xffffffffu;
    c->carrier_stall_ms = 3600000;
    c->nsrc = TBT_SRC_MAX;
    for (i = 0; i < TBT_SRC_MAX; i++)
        c->srcaddrs[i] = (__be32)0xffffffffu;
    c->nonce_set = true;
    memset(c->nonce, 0xa5, sizeof(c->nonce));
}

static void test_maximal_lines(void)
{
    struct tbt_cfg cfg;
    struct tbt_cmd cmd;
    struct tbt_parse_error perr;
    char line[TBT_LINE_MAX * 2], again[TBT_LINE_MAX * 2];
    int n, n2;

    maximal_cfg(&cfg, ~(u64)0);
    n = tbt_render_cfg(&cfg, line, sizeof(line));
    ok(n > 0 && n < TBT_LINE_MAX, "maximal render fits TBT_LINE_MAX", NULL);
    ok(parse(line, &cmd, &perr) == 0 && cmd.kind == TBT_CMD_TUNNEL, "maximal line parses", NULL);
    n2 = tbt_render_cfg(&cmd.cfg, again, sizeof(again));
    ok(n2 == n && !strcmp(line, again), "maximal line round-trips", again);

    /* Three maximal lines in one config block, as `cat config` would emit. */
    {
        char block[3 * TBT_LINE_MAX];
        size_t off = 0;
        int k, parsed = 0;
        char *p, *nl;

        memset(block, 0, sizeof(block));
        for (k = 0; k < 3; k++)
        {
            maximal_cfg(&cfg, ~(u64)0 - (u64)k);
            n = tbt_render_cfg(&cfg, block + off, sizeof(block) - off);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        ok(k == 3, "three maximal lines render", NULL);
        for (p = block; (nl = strchr(p, '\n')) != NULL; p = nl + 1)
        {
            *nl = '\0';
            if (parse(p, &cmd, &perr) == 0 && cmd.kind == TBT_CMD_TUNNEL)
                parsed++;
        }
        ok(parsed == 3, "three maximal lines parse back", NULL);
    }
}

/* The canonical text must be a fixed point for every role/profile and for
 * each optional key present or absent. The parser is pure: it takes no
 * task/net argument, so the release-path flush (where `current` may be
 * unusable) cannot fault inside it. */
static void test_config_matrix(void)
{
    static const char *const profiles[] = { "http-kal", "http-kal-drip", "raw" };
    static const char *const near_base =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2";
    static const char *const far_base =
        "tunnel 1 role=far listen=10.0.0.1:1 target=10.0.0.2:2";
    int r, p, mask;

    for (r = 0; r < 2; r++)
    {
        for (p = 0; p < 3; p++)
        {
            for (mask = 0; mask < 2; mask++)
            {
                struct tbt_cmd cmd, again;
                struct tbt_parse_error perr = { NULL, NULL };
                char line[TBT_LINE_MAX], text[TBT_LINE_MAX], twice[TBT_LINE_MAX];
                int n, n2;

                snprintf(line, sizeof(line), "%s profile=%s", r ? far_base : near_base,
                         profiles[p]);
                if (parse(line, &cmd, &perr))
                {
                    ok(0, "config matrix: base line parses", line);
                    continue;
                }
                if (mask & 1)
                {
                    cmd.cfg.nsrc = 2;
                    cmd.cfg.srcaddrs[0] = cmd.cfg.listen_addr;
                    cmd.cfg.srcaddrs[1] = r ? cmd.cfg.target_addr : cmd.cfg.peer_addr;
                }
                n = tbt_render_cfg(&cmd.cfg, text, sizeof(text));
                if (n <= 0)
                {
                    ok(0, "config matrix: render", NULL);
                    continue;
                }
                if (r)
                    ok(strstr(text, " peer=") == NULL, "far line renders no peer", text);
                else
                    ok(strstr(text, " peer=") != NULL, "near line renders peer", text);
                if (parse(text, &again, &perr))
                {
                    ok(0, "config matrix: rendered line parses", text);
                    continue;
                }
                n2 = tbt_render_cfg(&again.cfg, twice, sizeof(twice));
                ok(n2 == n && !strcmp(text, twice), "config matrix round-trip", text);
            }
        }
    }
}

static void test_far_peer(void)
{
    struct tbt_cmd cmd;
    struct tbt_parse_error perr = { NULL, NULL };
    char buf[TBT_LINE_MAX * 2];

    /* peer=0.0.0.0:0 is the old far render and is not parseable: port 0 is
     * refused, with the port named. */
    refuse("far with peer port 0",
           "tunnel 9 role=far listen=203.0.113.9:443 target=127.0.0.1:7000 peer=0.0.0.0:0",
           TBT_PARSE_EINVAL, NULL, "port");
    /* A far line that does carry a valid peer parses and renders it: the
     * address is the only near source allowed to join the session. */
    ok(parse("tunnel 9 role=far listen=203.0.113.9:443 target=127.0.0.1:7000"
             " peer=198.51.100.7:8443",
             &cmd, &perr) == 0,
       "far line with a valid peer parses", NULL);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(strstr(buf, " peer=198.51.100.7:8443") != NULL,
       "far line renders its peer anchor", buf);
    /* Without peer= the canonical far line stays anchor-free. */
    ok(parse("tunnel 9 role=far listen=203.0.113.9:443 target=127.0.0.1:7000",
             &cmd, &perr) == 0,
       "far line without a peer parses", NULL);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(strstr(buf, " peer=") == NULL, "far line without a peer omits it", buf);
}

/* ------------------------------------------------------------------------- */
/* /proc/net/tbtun/http frozen text format (docs/http-profile-integration.md):
 *   req=<escaped>, resp=<escaped>, first_chunk=<escaped>, req_len=<n>,
 *   resp_len=<n>
 * in that order. \r and \n are two-character escapes, a non-printable byte is
 * \xNN, and a literal backslash is \x5c. A partial snapshot is NEED_MORE
 * (SKIP), never an error; malformed text is PROTO. */

enum http_block_status
{
    HTTP_BLOCK_OK = 0,
    HTTP_BLOCK_NEED_MORE,
    HTTP_BLOCK_PROTO,
};

struct http_block
{
    u8 req[1024], resp[1024], first[512];
    size_t req_len, resp_len, first_len;
    size_t req_declared, resp_declared;
};

static int http_hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int unescape_value(const char *s, size_t n, u8 *out, size_t cap, size_t *out_len)
{
    size_t o = 0, i;

    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (c != '\\')
        {
            if (c < 0x20 || c > 0x7e || o == cap)
                return -1;
            out[o++] = c;
            continue;
        }
        i++;
        if (i >= n)
            return -1;
        if (s[i] == 'r')
            c = '\r';
        else if (s[i] == 'n')
            c = '\n';
        else if (s[i] == '\\')
            c = '\\';
        else if (s[i] == 'x')
        {
            int hi, lo;

            if (i + 2 >= n)
                return -1;
            hi = http_hexval(s[i + 1]);
            lo = http_hexval(s[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
        }
        else
        {
            return -1;
        }
        if (o == cap)
            return -1;
        out[o++] = c;
    }
    *out_len = o;
    return 0;
}

static int parse_decimal(const char *s, size_t n, size_t *out)
{
    size_t v = 0, i;

    if (!n)
        return -1;
    for (i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (size_t)(s[i] - '0');
        if (v > (1u << 20))
            return -1;
    }
    *out = v;
    return 0;
}

static int parse_http_block(const char *text, struct http_block *b)
{
    static const char *const names[] = { "req", "resp", "first_chunk", "req_len", "resp_len" };
    size_t expect = 0;
    const char *p = text;

    memset(b, 0, sizeof(*b));
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        const char *eq;
        size_t len, name_len, value_len;

        if (!nl)
            return HTTP_BLOCK_PROTO;
        len = (size_t)(nl - p);
        if (len && p[len - 1] == '\r')
            len--;
        eq = memchr(p, '=', len);
        if (!eq || eq == p)
            return HTTP_BLOCK_PROTO;
        name_len = (size_t)(eq - p);
        value_len = len - name_len - 1;
        if (expect >= sizeof(names) / sizeof(names[0]) || name_len != strlen(names[expect]) ||
            memcmp(p, names[expect], name_len) != 0)
            return HTTP_BLOCK_PROTO; /* unknown, duplicate or out of order */
        switch (expect)
        {
        case 0:
            if (unescape_value(eq + 1, value_len, b->req, sizeof(b->req), &b->req_len))
                return HTTP_BLOCK_PROTO;
            break;
        case 1:
            if (unescape_value(eq + 1, value_len, b->resp, sizeof(b->resp), &b->resp_len))
                return HTTP_BLOCK_PROTO;
            break;
        case 2:
            if (unescape_value(eq + 1, value_len, b->first, sizeof(b->first), &b->first_len))
                return HTTP_BLOCK_PROTO;
            break;
        case 3:
            if (parse_decimal(eq + 1, value_len, &b->req_declared))
                return HTTP_BLOCK_PROTO;
            break;
        case 4:
            if (parse_decimal(eq + 1, value_len, &b->resp_declared))
                return HTTP_BLOCK_PROTO;
            break;
        }
        expect++;
        p = nl + 1;
    }
    return expect == 5 ? HTTP_BLOCK_OK : HTTP_BLOCK_NEED_MORE;
}

static size_t escape_value(char *out, size_t cap, const u8 *in, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0, i;

    for (i = 0; i < n; i++)
    {
        char tmp[4];
        const char *rep;
        size_t rl;

        if (in[i] == '\r')
        {
            rep = "\\r";
            rl = 2;
        }
        else if (in[i] == '\n')
        {
            rep = "\\n";
            rl = 2;
        }
        else if (in[i] == '\\')
        {
            rep = "\\x5c";
            rl = 4;
        }
        else if (in[i] < 0x20 || in[i] > 0x7e)
        {
            tmp[0] = '\\';
            tmp[1] = 'x';
            tmp[2] = hex[in[i] >> 4];
            tmp[3] = hex[in[i] & 0x0f];
            rep = tmp;
            rl = 4;
        }
        else
        {
            tmp[0] = (char)in[i];
            rep = tmp;
            rl = 1;
        }
        if (o + rl >= cap)
            return 0;
        memcpy(out + o, rep, rl);
        o += rl;
    }
    out[o] = '\0';
    return o;
}

static size_t make_http_block(char *out, size_t cap, const u8 *req, size_t req_len,
                              const u8 *resp, size_t resp_len, const u8 *first, size_t first_len)
{
    char er[4096], ep[4096], ef[1024];
    size_t nr, np, nf;

    nr = escape_value(er, sizeof(er), req, req_len);
    np = escape_value(ep, sizeof(ep), resp, resp_len);
    nf = escape_value(ef, sizeof(ef), first, first_len);
    if (!nr || !np || !nf)
        return 0;
    return (size_t)snprintf(out, cap, "req=%s\nresp=%s\nfirst_chunk=%s\nreq_len=%zu\nresp_len=%zu\n",
                            er, ep, ef, req_len, resp_len);
}

/* A realistic 512-byte request header block, ending in CRLFCRLF, with one
 * literal backslash so the \x5c rule is exercised. */
static size_t make_http_req(u8 *out, size_t cap)
{
    static const char head[] = "POST /api/v2/upload HTTP/1.1\r\nHost: 203.0.113.9\r\n";
    size_t n = sizeof(head) - 1, pad;

    if (cap < 512)
        return 0;
    memcpy(out, head, n);
    memcpy(out + n, "X-Pad: ", 7);
    n += 7;
    pad = 512 - n - 4;
    memset(out + n, 'a', pad);
    n += pad;
    out[n - 3] = '\\';
    memcpy(out + n, "\r\n\r\n", 4);
    return n + 4;
}

static void test_http_contract(void)
{
    static const char resp_text[] =
        "HTTP/1.1 200 OK\r\nServer: nginx\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n";
    u8 req[512], resp[256], first[128];
    size_t req_len, resp_len, first_len;
    char block[8192];
    struct http_block b;
    size_t i, start, maxline = 0;

    req_len = make_http_req(req, sizeof(req));
    resp_len = sizeof(resp_text) - 1;
    memcpy(resp, resp_text, resp_len);
    memcpy(first, "2020\r\n", 6);
    memset(first + 6, 'B', 64);
    first_len = 70;

    ok(req_len == 512, "http: 512-byte request block", NULL);
    ok(make_http_block(block, sizeof(block), req, req_len, resp, resp_len, first, first_len) > 0,
       "http: block renders", NULL);
    for (i = 0, start = 0; block[i]; i++)
    {
        if (block[i] == '\n')
        {
            size_t len = i - start;

            if (len > maxline)
                maxline = len;
            start = i + 1;
        }
    }
    ok(maxline <= TBT_LINE_MAX, "http: every escaped line fits TBT_LINE_MAX", NULL);

    ok(parse_http_block(block, &b) == HTTP_BLOCK_OK, "http: block parses", NULL);
    ok(b.req_len == req_len && !memcmp(b.req, req, req_len), "http: req unescapes to the original",
       NULL);
    ok(b.req_declared == req_len, "http: req_len equals the original length", NULL);
    ok(b.resp_len == resp_len && !memcmp(b.resp, resp, resp_len), "http: resp unescapes", NULL);
    ok(b.resp_declared == resp_len, "http: resp_len equals the original length", NULL);
    ok(b.first_len == first_len && !memcmp(b.first, first, first_len), "http: first_chunk unescapes",
       NULL);
    ok(strstr(block, "\\x5c") != NULL, "http: literal backslash is \\x5c", NULL);

    /* A partial snapshot is SKIP (NEED_MORE), not an error. */
    ok(parse_http_block("req=abc\n", &b) == HTTP_BLOCK_NEED_MORE,
       "http: missing fields are NEED_MORE", NULL);
    ok(parse_http_block("", &b) == HTTP_BLOCK_NEED_MORE, "http: empty snapshot is NEED_MORE", NULL);
    ok(parse_http_block("req=abc\nresp=def\n", &b) == HTTP_BLOCK_NEED_MORE,
       "http: block prefix is NEED_MORE", NULL);

    /* Malformed text is PROTO. */
    ok(parse_http_block("req=abc\\q\n", &b) == HTTP_BLOCK_PROTO, "http: bad escape is PROTO", NULL);
    ok(parse_http_block("req=abc\nreq=def\n", &b) == HTTP_BLOCK_PROTO, "http: duplicate is PROTO",
       NULL);
    ok(parse_http_block("resp=abc\n", &b) == HTTP_BLOCK_PROTO, "http: out of order is PROTO", NULL);
    ok(parse_http_block("req=abc\nresp=def\nfirst_chunk=g\nreq_len=x\nresp_len=3\n", &b) ==
           HTTP_BLOCK_PROTO,
       "http: bad decimal is PROTO", NULL);
    ok(parse_http_block("req=ab\001c\n", &b) == HTTP_BLOCK_PROTO, "http: raw control byte is PROTO",
       NULL);
    ok(parse_http_block("nonsense\n", &b) == HTTP_BLOCK_PROTO, "http: line without = is PROTO", NULL);
}

int main(void)
{
    static const char *const minimal =
        "tunnel 1 role=near listen=10.123.0.1:9000 peer=10.123.0.2:443";
    static const char *const minimal_canon =
        "tunnel 1 role=near listen=10.123.0.1:9000 peer=10.123.0.2:443"
        " n=4 m=1 crypto=on crypto_key=- dup=1 profile=http-kal keepalive=5000"
        " drip_ms=1000 replay=8388608 tunnel_rate=0"
        " max_inflate=2.000 hole_budget_ms=500 rx_reorder_bytes=262144"
        " tx_retain_bytes=0 carrier_stall_ms=3000 portblock=0-0\n";
    static const char *const full_near =
        "tunnel 7 role=near listen=10.0.0.1:9000 peer=203.0.113.9:443"
        " n=4 m=1 crypto=on crypto_key=- dup=2 profile=http-kal-drip keepalive=5000"
        " drip_ms=1000 replay=8388608 tunnel_rate=0"
        " max_inflate=2.000 hole_budget_ms=500 rx_reorder_bytes=262144"
        " tx_retain_bytes=0 carrier_stall_ms=3000 portblock=40000-40063"
        " srcaddrs=10.0.0.10,10.0.0.11\n";
    static const char *const full_far =
        "tunnel 9 role=far listen=203.0.113.9:443 peer=198.51.100.7:8443"
        " target=127.0.0.1:7000 n=2 m=0 crypto=on crypto_key=- dup=0 profile=raw keepalive=1000"
        " drip_ms=250 replay=0 tunnel_rate=1048576"
        " max_inflate=1.500 hole_budget_ms=50 rx_reorder_bytes=262144"
        " tx_retain_bytes=0 carrier_stall_ms=3000 portblock=0-0\n";
    /* A far peer= is an anchor: the renderer emits it when set and never
     * emits peer=0.0.0.0:0 (port 0 is not parseable). */
    static const char *const full_far_canon =
        "tunnel 9 role=far listen=203.0.113.9:443 target=127.0.0.1:7000"
        " peer=198.51.100.7:8443"
        " n=2 m=0 crypto=on crypto_key=- dup=0 profile=raw keepalive=1000"
        " drip_ms=250 replay=0 tunnel_rate=1048576 max_inflate=1.500"
        " hole_budget_ms=50 rx_reorder_bytes=262144 tx_retain_bytes=0"
        " carrier_stall_ms=3000 portblock=0-0\n";
    struct tbt_cmd cmd;
    struct tbt_parse_error perr;
    char buf[TBT_LINE_MAX * 2];
    int i;

    /* 1. Canonical form is a fixed point, from the first read. */
    roundtrip("minimal line", minimal, minimal_canon);
    roundtrip("full near line", full_near, full_near);
    roundtrip("full far line", full_far, full_far_canon);

    /* 2. Rendering twice changes nothing. */
    parse(full_near, &cmd, &perr);
    {
        char once[TBT_LINE_MAX * 2], twice[TBT_LINE_MAX * 2];
        int n1 = tbt_render_cfg(&cmd.cfg, once, sizeof(once));
        struct tbt_cmd again;

        parse(once, &again, &perr);
        tbt_render_cfg(&again.cfg, twice, sizeof(twice));
        ok(n1 > 0 && !strcmp(once, twice), "render is idempotent", twice);
    }

    /* 3. portblock is rendered, not silently dropped. */
    parse(full_near, &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(strstr(buf, "portblock=40000-40063") != NULL, "portblock is rendered", buf);

    /* 4. profile=0 (unset) becomes the effective default http-kal. */
    parse(minimal, &cmd, &perr);
    ok(cmd.cfg.profile == 1, "unset profile normalises to http-kal", NULL);
    ok(strstr(minimal_canon, "profile=http-kal") != NULL, "canonical text names it", NULL);

    /* 5. dup is stored in a u8 without touching its neighbours. */
    parse("tunnel 3 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 dup=2", &cmd, &perr);
    ok(cmd.cfg.dup == 2, "dup=2 stored", NULL);
    ok(cmd.cfg.drip_ms == 1000, "dup did not overwrite drip_ms", NULL);
    ok(cmd.cfg.keepalive_ms == 5000, "dup did not overwrite keepalive", NULL);

    /* 6. Unknown keys are refused and named. */
    refuse("unknown key", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 frobnicate=1",
           TBT_PARSE_EINVAL, "frobnicate", "unknown key");
    refuse("unknown directive", "wibble 1", TBT_PARSE_EINVAL, "wibble", "unknown directive");
    refuse("hash after a token", "tunnel 1 # not a comment", TBT_PARSE_EINVAL, NULL, NULL);

    /* 6b. Keys removed in 0.0.1 are unknown, not silently ignored. */
    refuse("framing removed",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 framing=ext",
           TBT_PARSE_EINVAL, "framing", "unknown key");
    refuse("chunk removed",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 chunk=1024",
           TBT_PARSE_EINVAL, "chunk", "unknown key");
    refuse("max_retry removed",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 max_retry=3",
           TBT_PARSE_EINVAL, "max_retry", "unknown key");
    refuse("uid removed",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 uid=1000",
           TBT_PARSE_EINVAL, "uid", "unknown key");
    refuse("dev removed",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 dev=eth0",
           TBT_PARSE_EINVAL, "dev", "unknown key");
    refuse("src is not a key",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 src=10.0.0.0/8",
           TBT_PARSE_EINVAL, "src", "unknown key");

    /* 7. Comments and blanks are no-ops. */
    {
        struct tbt_cmd c;

        ok(parse("# a comment", &c, &perr) == 0 && c.kind == TBT_CMD_NONE, "comment ignored", NULL);
        ok(parse("   # indented", &c, &perr) == 0 && c.kind == TBT_CMD_NONE, "indented comment ignored", NULL);
        ok(parse("", &c, &perr) == 0 && c.kind == TBT_CMD_NONE, "blank line ignored", NULL);
    }

    /* 8. Over-long lines are refused, with the line-too-long errno. */
    {
        char longline[TBT_LINE_MAX + 2];

        memset(longline, 'a', sizeof(longline) - 1);
        longline[sizeof(longline) - 1] = '\0';
        ok(tbt_parse_line(longline, &cmd, &perr) == TBT_PARSE_E2BIG, "over-long line refused", NULL);
    }

    /* 9. IPv6 is refused, and says so. */
    refuse("IPv6 listen", "tunnel 1 role=near listen=[::1]:9000 peer=10.0.0.2:2",
           TBT_PARSE_EAFNOSUPPORT, NULL, "IPv6");
    refuse("IPv6 peer", "tunnel 1 role=near listen=10.0.0.1:1 peer=[2001:db8::1]:2",
           TBT_PARSE_EAFNOSUPPORT, NULL, "IPv6");

    /* 10. Hostnames are refused, never resolved. */
    refuse("hostname", "tunnel 1 role=near listen=example.com:9000 peer=10.0.0.2:2",
           TBT_PARSE_EINVAL, NULL, "hostname");

    /* 11. Out-of-range values are refused. */
    refuse("n=0", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 n=0", TBT_PARSE_EINVAL, "n", NULL);
    refuse("n=17", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 n=17", TBT_PARSE_EINVAL, "n", NULL);
    refuse("n+m=17", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 n=16 m=1",
           TBT_PARSE_EINVAL, "n", NULL);
    refuse("m=17", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 m=17", TBT_PARSE_EINVAL, "m", NULL);
    refuse("dup=4", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 dup=4", TBT_PARSE_EINVAL, "dup", NULL);
    refuse("replay too big", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 replay=268435457",
           TBT_PARSE_EINVAL, "replay", NULL);
    refuse("portblock reversed", "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 portblock=500-400",
           TBT_PARSE_EINVAL, NULL, NULL);
    refuse("port 0", "tunnel 1 role=near listen=10.0.0.1:0 peer=10.0.0.2:2", TBT_PARSE_EINVAL, NULL, NULL);

    /* 12. Required keys. */
    refuse("no role", "tunnel 1 listen=10.0.0.1:1 peer=10.0.0.2:2", TBT_PARSE_EINVAL, "role", NULL);
    refuse("near without peer", "tunnel 1 role=near listen=10.0.0.1:1", TBT_PARSE_EINVAL, "peer", NULL);
    refuse("far without target", "tunnel 1 role=far listen=10.0.0.1:1", TBT_PARSE_EINVAL, "target", NULL);
    refuse("tunnel without id", "tunnel role=near", TBT_PARSE_EINVAL, NULL, NULL);

    /* 13. del / flush. */
    ok(parse("del 5", &cmd, &perr) == 0 && cmd.kind == TBT_CMD_DEL && cmd.id == 5, "del 5", NULL);
    refuse("del without id", "del", TBT_PARSE_EINVAL, "del", NULL);
    ok(parse("flush", &cmd, &perr) == 0 && cmd.kind == TBT_CMD_FLUSH, "flush", NULL);
    refuse("flush with junk", "flush now", TBT_PARSE_EINVAL, "flush", NULL);

    /* 14. The ports file. */
    {
        u16 lo = 0, hi = 0;

        ok(tbt_parse_ports("ports 41000-41015\n", &lo, &hi, &perr) == 0 && lo == 41000 && hi == 41015,
           "ports 41000-41015", NULL);
        ok(tbt_parse_ports("  \n", &lo, &hi, &perr) == 0 && !lo && !hi, "blank ports line", NULL);
        ok(tbt_parse_ports("ports 0-0\n", &lo, &hi, &perr) == 0 && !lo && !hi,
           "ports 0-0 is a no-op", NULL);
        ok(tbt_parse_ports("ports 0-1\n", &lo, &hi, &perr) != 0, "ports 0-1 refused", NULL);
        ok(tbt_parse_ports("ports 41015-41000\n", &lo, &hi, &perr) != 0, "reversed ports refused", NULL);
    }

    /* 15. A short output buffer is an error, not a truncated line. */
    parse(full_near, &cmd, &perr);
    ok(tbt_render_cfg(&cmd.cfg, buf, 40) == TBT_PARSE_E2BIG, "short buffer refused", NULL);

    /* 15b. The render length is exact: size == n has no room for the NUL, so
     * it must be refused; size == n + 1 is the smallest accepted buffer. This
     * is the pure-logic half of the procfs snapshot contract (the kernel side
     * is exercised by adversarial cat.cfg_self / init.cfg_roundtrip). */
    {
        int need = tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));

        ok(need > 0, "render length measured", NULL);
        ok(tbt_render_cfg(&cmd.cfg, buf, (size_t)need) == TBT_PARSE_E2BIG,
           "buffer without room for NUL refused", NULL);
        ok(tbt_render_cfg(&cmd.cfg, buf, (size_t)need + 1) == need,
           "exact-size buffer accepted", NULL);
    }

    /* 16. Every key of the grammar survives one parse. */
    parse(full_far, &cmd, &perr);
    ok(cmd.cfg.n == 2 && cmd.cfg.m == 0 && cmd.cfg.dup == 0, "far n/m/dup", NULL);
    ok(cmd.cfg.profile == 3, "far profile=raw", NULL);
    ok(cmd.cfg.drip_ms == 250 && cmd.cfg.replay == 0, "far drip/replay", NULL);
    ok(cmd.cfg.tunnel_rate == 1048576 && cmd.cfg.max_inflate_milli == 1500, "far rate/inflate", NULL);
    ok(cmd.cfg.hole_budget_ms == 50, "far hole budget", NULL);
    parse(full_near, &cmd, &perr);
    ok(cmd.cfg.nsrc == 2, "near srcaddrs list", NULL);
    ok(cmd.cfg.portblock_lo == 40000 && cmd.cfg.portblock_hi == 40063, "near portblock", NULL);

    /* 17. The t1_skeleton read-back prefix still holds. */
    parse("tunnel 1 role=near listen=10.123.0.1:9000 peer=10.123.0.2:443 n=2 m=1", &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(strncmp(buf, "tunnel 1 role=near listen=10.123.0.1:9000 peer=10.123.0.2:443 n=2 m=1 ",
               strlen("tunnel 1 role=near listen=10.123.0.1:9000 peer=10.123.0.2:443 n=2 m=1 ")) == 0,
       "t1 read-back prefix", buf);

    /* 18. C3 queue keys: bounds, explicit values, canonical render. */
    parse("tunnel 2 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
          " rx_reorder_bytes=1 hole_budget_ms=1 tx_retain_bytes=4294967295"
          " carrier_stall_ms=3600000",
          &cmd, &perr);
    ok(cmd.cfg.rx_reorder_bytes == 1 && cmd.cfg.hole_budget_ms == 1 &&
           cmd.cfg.tx_retain_bytes == 4294967295u && cmd.cfg.carrier_stall_ms == 3600000,
       "C3 keys at their bounds", NULL);
    parse("tunnel 2 role=near listen=10.0.0.1:1 peer=10.0.0.2:2"
          " rx_reorder_bytes=4096 tx_retain_bytes=1024 carrier_stall_ms=250",
          &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(strstr(buf, " rx_reorder_bytes=4096 tx_retain_bytes=1024 carrier_stall_ms=250 ") != NULL,
       "C3 keys render canonically", buf);
    {
        struct tbt_cmd again;

        parse(buf, &again, &perr);
        ok(again.cfg.rx_reorder_bytes == 4096 && again.cfg.tx_retain_bytes == 1024 &&
               again.cfg.carrier_stall_ms == 250,
           "C3 keys round-trip through the render", NULL);
    }
    parse("tunnel 4 role=near listen=10.0.0.1:1 peer=10.0.0.2:2", &cmd, &perr);
    tbt_render_cfg(&cmd.cfg, buf, sizeof(buf));
    ok(cmd.cfg.hole_budget_ms == 500 && cmd.cfg.rx_reorder_bytes == 262144,
       "C3 budgets default when omitted", NULL);
    ok(strstr(buf, " hole_budget_ms=500 rx_reorder_bytes=262144"
                    " tx_retain_bytes=0 carrier_stall_ms=3000 ") != NULL,
       "C3 defaults render when omitted", buf);
    /* Zero is not a value for the two budgets that gate stream death: a zero
     * reorder bound kills the first out-of-order frame, a zero hole budget
     * kills the first hole, so both are refused before the tunnel exists. */
    refuse("rx_reorder_bytes=0",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 rx_reorder_bytes=0",
           TBT_PARSE_EINVAL, "rx_reorder_bytes", "1..268435456");
    refuse("hole_budget_ms=0",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 hole_budget_ms=0",
           TBT_PARSE_EINVAL, "hole_budget_ms", "1..60000");
    refuse("rx_reorder_bytes over max",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 rx_reorder_bytes=268435457",
           TBT_PARSE_EINVAL, "rx_reorder_bytes", "268435456");
    refuse("rx_reorder_bytes over u32",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 rx_reorder_bytes=4294967296",
           TBT_PARSE_EINVAL, "rx_reorder_bytes", "268435456");
    refuse("rx_reorder_bytes negative",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 rx_reorder_bytes=-1",
           TBT_PARSE_EINVAL, "rx_reorder_bytes", "268435456");
    refuse("rx_reorder_bytes non-numeric",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 rx_reorder_bytes=abc",
           TBT_PARSE_EINVAL, "rx_reorder_bytes", "268435456");
    refuse("tx_retain_bytes over u32",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 tx_retain_bytes=4294967296",
           TBT_PARSE_EINVAL, "tx_retain_bytes", "4294967295");
    refuse("tx_retain_bytes negative",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 tx_retain_bytes=-1",
           TBT_PARSE_EINVAL, "tx_retain_bytes", "4294967295");
    refuse("tx_retain_bytes non-numeric",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 tx_retain_bytes=abc",
           TBT_PARSE_EINVAL, "tx_retain_bytes", "4294967295");
    refuse("carrier_stall_ms over max",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 carrier_stall_ms=3600001",
           TBT_PARSE_EINVAL, "carrier_stall_ms", "3600000");
    refuse("carrier_stall_ms negative",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 carrier_stall_ms=-1",
           TBT_PARSE_EINVAL, "carrier_stall_ms", "3600000");
    refuse("carrier_stall_ms non-numeric",
           "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 carrier_stall_ms=abc",
           TBT_PARSE_EINVAL, "carrier_stall_ms", "3600000");

    /* 19. FBK body: 18-byte entries, 0..16 of them, no padding. */
    ok(TBT_FBK_ENTRY_LEN == 18 && TBT_FBK_MAX == 288, "FBK entry is 18 bytes, 16 entries", NULL);
    ok(TBT_FBK_ENTRY_LEN == 1 + 1 + 8 + 4 + 4, "FBK entry fields add up", NULL);
    ok(TBT_FBK_PATH == 0 && TBT_FBK_RXSTATE == 1 && TBT_FBK_UACK == 2 && TBT_FBK_RECVCNT == 10 &&
           TBT_FBK_RTT == 14 && TBT_FBK_RTT + 4 == TBT_FBK_ENTRY_LEN,
       "FBK entry offsets", NULL);
    {
        u16 min = 1, max = 0;
        unsigned n;

        ok(tbt_body_bounds(TBT_F_FBK, &min, &max) && min == 0 && max == TBT_FBK_MAX,
           "FBK body bounds are 0..16 x 18", NULL);
        for (n = 0; n <= TBT_MAX_CARRIERS_LIMIT; n++)
            ok((unsigned)n * TBT_FBK_ENTRY_LEN <= TBT_FBK_MAX, "FBK n x 18 fits the max", NULL);
        ok((unsigned)TBT_MAX_CARRIERS_LIMIT * TBT_FBK_ENTRY_LEN == TBT_FBK_MAX,
           "FBK max is exactly 16 entries", NULL);
    }

    /* 20. The peers column contract, pinned verbatim. */
    ok(!strcmp(TBT_PEERS_COLUMNS, "id carrier sport peer uack rtt_us inflight state class"
                                  " recv_cnt rx_lag_ppm minrtt_us tx_frames tx_bytes dropped"
                                  " retx_ppm"),
       "peers column contract", TBT_PEERS_COLUMNS);
    ok(count_words(TBT_PEERS_COLUMNS) == 16, "peers has exactly 16 columns", NULL);
    ok(count_words(TBT_TUNNELS_COLUMNS) == 9, "tunnels has exactly 9 columns", NULL);

    /* 21. Action verbs: canonical text, round-trip, refusals, config exclusion. */
    test_actions();

    /* 22. The stable stats prefix: keys, order, key=value form, round-trip. */
    test_stats_prefix();

    /* 23. The line limit and maximal-length config lines. */
    test_line_limit();
    test_maximal_lines();

    /* 24. Role/profile/optional-key round-trips, and the far-line peer fix. */
    test_config_matrix();
    test_far_peer();

    /* 24b. Tunnel-level nonce=: 128-bit, 64-bit pin, refusals, render. */
    test_tunnel_nonce();

    /* 24c. crypto= / crypto_key=: default on, off, key, clear, refusals. */
    test_crypto_cfg();

    /* 25. The frozen /proc/net/tbtun/http text contract. */
    test_http_contract();

    (void)i;
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
