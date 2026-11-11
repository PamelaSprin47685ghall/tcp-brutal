/* test_client_rx.c - the near-side receive state machine under markers,
 * short blocks, per-block n/m and an 8-slot window.
 *
 * The engine's receive functions are static, so this test includes the
 * translation unit directly and drives flow_rx_store()/flow_rx_check() with
 * synthetic 0.8 records. A full-data block needs no codec; a missing-share
 * block uses the per-shape codec cache, which the test exercises directly.
 *
 * It pins the fixes a real .92 near side needed:
 *   - NACK anchors on rx_next_block, never on a delivery frontier that marker
 *     blocks leave behind;
 *   - rx_next_deliver advances by real packets: marker 0, short sum(win_len);
 *   - ACK credit is (TBT_RX_SLOTS - 1) * groups * (rx_n + rx_m);
 *   - n/m are read from every block header, not from the session's first CFG.
 * Counters check that a stale slot below rx_next_block can never be NACKed
 * and that a late replay is counted instead of silently wedging.
 */
#define _GNU_SOURCE

#include "tbt_client_engine.h"
#include "tbt_client_engine.c"

#include <stdio.h>
#include <string.h>

static struct engine e;
static struct flow f;
static u8 payload[TBT_FEC_S];
static int checks;
static int failures;

/* Minimal SHA-256 for the byte-exact shape-switch regression. */
struct sha256
{
    u32 h[8];
    u64 bits;
    u8 buf[64];
    size_t len;
};

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(struct sha256 *s, const u8 *p)
{
    u32 w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 |
               (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
    {
        u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        u32 s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + sha_k[i] + w[i];
        u32 s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(struct sha256 *s)
{
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                               0xa54ff53a, 0x510e527f, 0x9b05688c,
                               0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->bits = 0;
    s->len = 0;
}

static void sha_update(struct sha256 *s, const void *data, size_t len)
{
    const u8 *p = data;

    s->bits += (u64)len * 8;
    while (len)
    {
        size_t take = 64 - s->len;
        if (take > len) take = len;
        memcpy(s->buf + s->len, p, take);
        s->len += take; p += take; len -= take;
        if (s->len == 64) { sha_block(s, s->buf); s->len = 0; }
    }
}

static void sha_final(struct sha256 *s, u8 out[32])
{
    u64 bits = s->bits;
    u8 pad = 0x80, zero = 0, lenbuf[8];
    int i;

    sha_update(s, &pad, 1);
    while (s->len != 56) sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++) lenbuf[i] = (u8)(bits >> (56 - i * 8));
    sha_update(s, lenbuf, 8);
    for (i = 0; i < 8; i++)
    {
        out[i * 4] = (u8)(s->h[i] >> 24);
        out[i * 4 + 1] = (u8)(s->h[i] >> 16);
        out[i * 4 + 2] = (u8)(s->h[i] >> 8);
        out[i * 4 + 3] = (u8)s->h[i];
    }
}

static void sha_digest(const u8 *p, size_t len, u8 out[32])
{
    struct sha256 s;
    sha_init(&s);
    sha_update(&s, p, len);
    sha_final(&s, out);
}


#define CHECK(cond, name)                 \
    do                                    \
    {                                     \
        checks++;                         \
        if (!(cond))                      \
        {                                 \
            failures++;                   \
            printf("FAIL %s\n", name);    \
        }                                 \
    } while (0)

static void release_codecs(void)
{
    int i;

    for (i = 0; i < TBT_RX_CODECS; i++)
    {
        if (f.rx_codec[i].used)
            tbt_fec_grouped_free(f.rx_codec[i].codec);
        memset(&f.rx_codec[i], 0, sizeof(f.rx_codec[i]));
    }
}

static void release_blocks(void)
{
    int i;

    for (i = 0; i < TBT_RX_SLOTS; i++)
    {
        struct rx_block *b = &f.rx[i];
        int g;

        if (!b->used)
            continue;
        for (g = 0; g < b->groups; g++)
            free(b->g[g].buf);
        free(b->g);
        memset(b, 0, sizeof(*b));
    }
}

static void clear_engine(void)
{
    free(f.out);
    release_blocks();
    release_codecs();
    memset(&f, 0, sizeof(f));
    memset(&e, 0, sizeof(e));
    f.e = &e;
    f.sid = 1;
    f.app_fd = -1;
    f.rx_shape_set = true;
    f.rx_shape.variant = TBT_FEC_SCLC_UP;
    f.rx_shape.n = 8;
    f.rx_shape.m = 24;
    f.rx_shape.groups = 8;
    f.rx_shape.block = TBT_FEC_SCLC_BLOCK_UP;
    f.rx_shape.L = TBT_FEC_SCLC_L_UP;
    f.rx_shape.rate_ppm = 250;
    f.rx_shape.rate_256 = 64;
    f.rx_seed = 1;
    f.rx_groups = 8;
    f.rx_shares = 32;
    f.rx_n = 8;
    f.rx_m = 24;
    e.n_car = 0;
    e.tx_n = 8;
    e.crypto_on = false;
    memset(payload, 0x5a, sizeof(payload));
}

static void clear_pending(void)
{
    e.npending = 0;
}

static void set_downlink_shape(void);

static void send_data(u64 block, u64 seq0, u16 n, u16 m, u16 group,
                      u16 share, u16 len, bool padded)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = padded ? TBT_FEC_FF_PADDED : 0;
    h.share_index = share;
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block;
    h.seq = seq0 + (u64)group * n + share;
    h.payload_len = padded ? TBT_FEC_S : len;
    h.win_len = padded ? len : 0;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, payload);
}

static void send_parity(u64 block, u64 seq0, u16 n, u16 m, u16 group,
                        u16 win_len, u16 epoch)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = n;
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block;
    h.seq = seq0;
    h.win_len = win_len;
    h.payload_len = TBT_FEC_S;
    h.epoch = epoch;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, payload);
}

static void send_data_payload(u64 block, u64 seq0, u16 n, u16 m, u16 group,
                              u16 share, u16 len, bool padded, const u8 *p)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = padded ? TBT_FEC_FF_PADDED : 0;
    h.share_index = share;
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block;
    h.seq = seq0 + (u64)group * n + share;
    h.payload_len = padded ? TBT_FEC_S : len;
    h.win_len = padded ? len : 0;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, p);
}

static void send_parity_payload(u64 block, u64 seq0, u16 n, u16 m,
                                u16 group, u16 parity_index, u16 win_len,
                                u16 epoch, const u8 *p)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = (u16)(n + parity_index);
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block;
    h.seq = seq0;
    h.win_len = win_len;
    h.payload_len = TBT_FEC_S;
    h.epoch = epoch;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, p);
}

static void send_marker(u64 block, u64 seq0, u16 n, u16 m, u16 group)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = n;
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block;
    h.seq = seq0;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, payload);
}

static void full_block(u64 block, u64 seq0, u16 n, u16 m)
{
    u16 g, s;

    for (g = 0; g < f.rx_groups; g++)
        for (s = 0; s < n; s++)
            send_data(block, seq0, n, m, g, s, TBT_FEC_S, false);
}

/* Two real shares per group: the last one is PADDED, and the parity states
 * win_len=2. The block therefore contributes 2 * groups packets. */
static void short_block(u64 block, u64 seq0, u16 n, u16 m)
{
    u16 g;

    for (g = 0; g < f.rx_groups; g++)
    {
        send_data(block, seq0, n, m, g, 0, TBT_FEC_S, false);
        send_data(block, seq0, n, m, g, 1, 784, true);
        send_parity(block, seq0, n, m, g, 2, 784);
    }
}

static void marker_block(u64 block, u64 seq0, u16 n, u16 m)
{
    u16 g;

    for (g = 0; g < f.rx_groups; g++)
        send_marker(block, seq0, n, m, g);
}

static int count_nacks(u64 *ids, int max)
{
    int n = 0;
    int i;

    for (i = 0; i < e.npending; i++)
    {
        if (e.pending[i].rec[0] != TBT_FEC_NACK)
            continue;
        if (n < max)
            ids[n] = tbt_le64(e.pending[i].rec + 8);
        n++;
    }
    return n;
}

static int last_ack_credit(void)
{
    int i;

    for (i = e.npending - 1; i >= 0; i--)
        if (e.pending[i].rec[0] == TBT_FEC_ACK)
            return (int)tbt_le32(e.pending[i].rec + 56);
    return -1;
}

/* Full, short, marker, marker, marker, marker, full. The frontier must be
 * 64 + 16 + 0 + 0 + 0 + 0 + 64 = 144, while rx_next_block reaches 7. */
static void test_marker_short_sequence(void)
{
    u64 ids[16];
    int i;

    clear_engine();
    full_block(0, 0, 8, 24);
    short_block(1, 64, 8, 24);
    for (i = 2; i <= 5; i++)
        marker_block((u64)i, (u64)i * 64, 8, 24);
    full_block(6, 6 * 64, 8, 24);

    CHECK(f.rx_next_block == 7, "sequence: next block is 7");
    CHECK(f.rx_next_deliver == 144, "sequence: frontier counts real packets");
    CHECK(f.rx_deliver_bytes == 64u * TBT_FEC_S +
                                    8u * (TBT_FEC_S + 784u) +
                                    64u * TBT_FEC_S,
          "sequence: delivered bytes");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 8u * (8u + 24u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 8u * (8u + 24u),
          "sequence: ACK credit is 7 blocks of groups*(n+m)");
    CHECK(f.rx_credit_bad == 0, "sequence: credit invariant holds");
    CHECK(f.rx_stale_nack == 0 && f.rx_late_share == 0 &&
              f.rx_window_drop == 0 && f.rx_shape_bad == 0,
          "sequence: no stale request or late drop");

    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 1000000u);
    CHECK(count_nacks(ids, 16) == 0, "sequence: no NACK for delivered blocks");
}

/* The sender spends the full credit of TBT_RX_SLOTS-1 blocks: one head under
 * delivery plus seven in flight fill all eight slots, and no block is
 * dropped. This is the capacity invariant the old 8-block credit broke. */
static void test_credit_inflight(void)
{
    int i;

    clear_engine();
    set_downlink_shape();
    send_data(0, 0, 132, 124, 0, 0, TBT_FEC_S, false);
    for (i = 1; i < TBT_RX_SLOTS; i++)
        marker_block((u64)i, (u64)i * 132, 132, 124);

    CHECK(f.rx_used == TBT_RX_SLOTS, "credit: eight slots held");
    CHECK(f.rx_window_drop == 0, "credit: full credit causes no drop");
    CHECK(f.rx_inflight_bad == 0, "credit: in-flight count is within slots-1");
    clear_pending();
    CHECK(flow_send_ack(&f) == 0, "credit: ACK sent");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (132u + 124u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * (132u + 124u),
          "credit: ACK grants slots-1 whole blocks");
    CHECK(f.rx_credit_bad == 0, "credit: credit invariant holds");

    /* Complete the head; the seven markers drain behind it. */
    send_parity(0, 0, 132, 124, 0, 1, 0);
    CHECK(f.rx_next_block == 8, "credit: head and in-flight blocks drained");
    CHECK(f.rx_used == 0, "credit: ring empty after drain");
    CHECK(f.rx_window_drop == 0, "credit: drain caused no drop");
    CHECK(f.rx_inflight_bad == 0, "credit: invariant held through drain");

    /* The next block fits because one slot was reserved for delivery. */
    marker_block(8, 8 * 132, 146, 110);
    CHECK(f.rx_next_block == 9, "credit: post-drain block accepted");
    CHECK(f.rx_used == 0 && f.rx_window_drop == 0,
          "credit: post-drain ring empty and no drop");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * (146u + 110u),
          "credit: post-drain credit follows the new split");
}

/* Eight slots hold blocks 0..7. A ninth block before the head completes is
 * a sender violation and must be refused; NACK must name block 0 only; the
 * stub block 0 then completes with one real share and seven markers. */
static void test_window_and_nack_base(void)
{
    u64 ids[16];
    int n;
    int i;

    clear_engine();
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    for (i = 1; i < TBT_RX_SLOTS; i++)
        marker_block((u64)i, (u64)i * 64, 8, 24);

    send_data(8, 8 * 64, 8, 24, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_window_drop == 1, "window: block 8 refused");
    CHECK(f.rx_next_block == 0, "window: head still block 0");

    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 1000000u);
    n = count_nacks(ids, 16);
    CHECK(n >= 1, "nack: block 0 requested");
    for (i = 0; i < n && i < 16; i++)
        CHECK(ids[i] == 0, "nack: only the window head is named");
    CHECK(f.rx_stale_nack == 0, "nack: no stale request counter");

    /* A stale head below the delivery cursor must not be NACKed. */
    f.rx_next_block = 1;
    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 2000000u);
    CHECK(count_nacks(ids, 16) == 0, "nack: stale slot is skipped");
    f.rx_next_block = 0;

    /* Complete block 0: share 0 is present, parity says win_len=1, and the
     * other seven groups are markers. */
    clear_pending();
    send_parity(0, 0, 8, 24, 0, 1, 0);
    for (i = 1; i < 8; i++)
        send_marker(0, 0, 8, 24, (u16)i);
    CHECK(f.rx_next_block == 8, "window: blocks 0..7 drained");
    CHECK(f.rx_next_deliver == 1, "window: frontier is the one real share");
    CHECK(f.rx_deliver_bytes == TBT_FEC_S, "window: delivered one share");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 8u * (8u + 24u)),
          "window: ACK credit follows the ring");

    full_block(8, 8 * 64, 8, 24);
    CHECK(f.rx_next_block == 9, "window: block 8 accepted after drain");
    CHECK(f.rx_next_deliver == 65, "window: full block adds 64 packets");

    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_late_share == 1, "late: old replay counted");
    CHECK(f.rx_next_block == 9, "late: cursor unchanged");
}

/* Consecutive blocks use different n/m; the cursor must follow real packets
 * and the ACK credit must follow the newest rx_n. */
static void test_multishape_sequence(void)
{
    clear_engine();
    full_block(0, 0, 8, 24);      /* 64 packets */
    short_block(1, 64, 9, 23);    /* 16 packets */
    marker_block(2, 128, 10, 22); /* 0 packets */
    full_block(3, 192, 12, 20);   /* 96 packets */
    marker_block(4, 384, 9, 23);  /* 0 packets, current n becomes 9 */

    CHECK(f.rx_next_block == 5, "shape: five blocks delivered");
    CHECK(f.rx_next_deliver == 176, "shape: frontier sums real packets");
    CHECK(f.rx_deliver_bytes == 64u * TBT_FEC_S +
                                    8u * (TBT_FEC_S + 784u) +
                                    96u * TBT_FEC_S,
          "shape: delivered bytes");
    CHECK(f.rx_n == 9 && f.rx_m == 23,
          "shape: current split follows the newest block");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 8u * (9u + 23u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 8u * (9u + 23u),
          "shape: credit follows groups*(n+m)");
    CHECK(f.rx_credit_bad == 0, "shape: credit invariant holds");
    CHECK(f.rx_shape_bad == 0, "shape: all per-block splits accepted");
    CHECK(flow_rx_codec(&f, 9, 23) != NULL, "shape: 9/23 codec builds");
    CHECK(flow_rx_codec(&f, 12, 20) != NULL, "shape: 12/20 codec builds");
}

/* The downlink split may move without a new CFG; drive the user's observed
 * 132/124 and 146/110 shapes through the same receive path. */
static void set_downlink_shape(void)
{
    f.rx_shape.variant = TBT_FEC_SCLC_DOWN;
    f.rx_shape.n = 128;
    f.rx_shape.m = 128;
    f.rx_shape.groups = 1;
    f.rx_shape.block = 256;
    f.rx_shape.L = 8;
    f.rx_groups = 1;
    f.rx_shares = 256;
    f.rx_n = 128;
    f.rx_m = 128;
}

static void test_downlink_shapes(void)
{
    clear_engine();
    set_downlink_shape();

    send_marker(0, 0, 132, 124, 0);
    CHECK(f.rx_next_block == 1 && f.rx_next_deliver == 0,
          "down: 132/124 marker accepted");
    CHECK(f.rx_n == 132 && f.rx_m == 124,
          "down: current split follows the marker");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (132u + 124u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 256u,
          "down: credit is 7 * 256 shares");
    CHECK(f.rx_credit_bad == 0, "down: credit invariant holds");

    full_block(1, 146, 146, 110);
    CHECK(f.rx_next_block == 2 && f.rx_next_deliver == 146,
          "down: 146/110 full block delivered");
    CHECK(f.rx_n == 146 && f.rx_m == 110,
          "down: current split follows the full block");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 256u,
          "down: credit is 7 * 256 shares after switch");
    CHECK(f.rx_credit_bad == 0, "down: credit invariant holds after switch");
    CHECK(flow_rx_codec(&f, 132, 124) != NULL,
          "down: 132/124 codec builds");
    CHECK(flow_rx_codec(&f, 146, 110) != NULL,
          "down: 146/110 codec builds");
    CHECK(!flow_commit_shape(&f, 200, 56),
          "down: a split above 192 is refused");
}

/* A 16 MiB boundary: 128 full 128/128 downlink blocks, then a short block
 * and a marker. The old b->seq0 + blk_pkts formula jumped the delivery
 * frontier at the marker; this run must keep counting real packets. */
static void test_16m_boundary(void)
{
    int i;
    u16 s;

    clear_engine();
    set_downlink_shape();
    for (i = 0; i < 128; i++)
    {
        full_block((u64)i, (u64)i * 128, 128, 128);
        if ((i & 31) == 31)
            clear_pending();
    }
    CHECK(f.rx_deliver_bytes == 128u * 128u * TBT_FEC_S,
          "16m: exactly 16 MiB delivered");
    CHECK(f.rx_next_deliver == 16384, "16m: frontier is 16384 packets");
    CHECK(f.rx_next_block == 128, "16m: 128 blocks delivered");

    clear_pending();
    for (s = 0; s < 101; s++)
        send_data_payload(128, 128 * 128, 128, 128, 0, s,
                          s + 1 == 101 ? 784 : TBT_FEC_S, s + 1 == 101,
                          payload);
    send_parity_payload(128, 128 * 128, 128, 128, 0, 0, 101, 784, payload);
    CHECK(f.rx_next_block == 129, "16m: short block crosses the boundary");
    CHECK(f.rx_next_deliver == 16485, "16m: short block adds 101 packets");
    CHECK(f.rx_deliver_bytes == 128u * 128u * TBT_FEC_S + 100u * TBT_FEC_S +
                                    784u,
          "16m: short block adds its real bytes");

    marker_block(129, 129 * 128, 146, 110);
    CHECK(f.rx_next_block == 130, "16m: marker crosses the boundary");
    CHECK(f.rx_next_deliver == 16485, "16m: marker adds no packets");
    CHECK(f.rx_n == 146 && f.rx_m == 110,
          "16m: the marker's split is adopted");
    CHECK(f.rx_window_drop == 0 && f.rx_inflight_bad == 0 &&
              f.rx_shape_bad == 0,
          "16m: no window or shape error");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 256u,
          "16m: credit is 7 * 256 shares after boundary");
    CHECK(f.rx_credit_bad == 0, "16m: credit invariant holds");
}

/* The far ladder observed in .92: 128 -> 139/117 -> 140/116 -> 146/110
 * with markers and a short block in between. Each block must place and
 * decode with its own header. */
static void test_far_shape_ladder(void)
{
    clear_engine();
    set_downlink_shape();

    marker_block(0, 0, 128, 128);
    short_block(1, 128, 139, 117);
    marker_block(2, 256, 140, 116);
    full_block(3, 384, 146, 110);

    CHECK(f.rx_next_block == 4, "ladder: four blocks delivered");
    CHECK(f.rx_next_deliver == 148, "ladder: frontier 2 + 146");
    CHECK(f.rx_n == 146 && f.rx_m == 110,
          "ladder: current split follows the latest block");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 256u,
          "ladder: credit is 7 * 256 shares on the ladder");
    CHECK(f.rx_credit_bad == 0, "ladder: credit invariant holds");
    CHECK(f.rx_shape_bad == 0, "ladder: no shape rejection");
    CHECK(flow_rx_codec(&f, 139, 117) != NULL,
          "ladder: 139/117 codec builds");
    CHECK(flow_rx_codec(&f, 140, 116) != NULL,
          "ladder: 140/116 codec builds");
    CHECK(flow_rx_codec(&f, 146, 110) != NULL,
          "ladder: 146/110 codec builds");
}

/* A real decode at 139/117: encode one full block with the same codec the RX
 * builds, drop three DATA shares, send all parity, and require delivery. */
static void test_downlink_erasure(void)
{
    u8 *data_ptrs[139];
    u8 *parity_ptrs[117];
    u8 *parity[117];
    struct tbt_fec_grouped *codec;
    int i;

    clear_engine();
    set_downlink_shape();
    codec = flow_rx_codec(&f, 139, 117);
    CHECK(codec != NULL, "erasure: 139/117 codec");
    if (!codec)
        return;
    for (i = 0; i < 139; i++)
        data_ptrs[i] = payload;
    for (i = 0; i < 117; i++)
    {
        parity[i] = calloc(1, TBT_FEC_S);
        parity_ptrs[i] = parity[i];
    }
    CHECK(tbt_fec_grouped_encode(codec, 0, data_ptrs, 139, parity_ptrs) == 0,
          "erasure: encode 139/117");

    for (i = 0; i < 139; i++)
    {
        if (i == 5 || i == 77 || i == 138)
            continue;
        send_data_payload(0, 0, 139, 117, 0, (u16)i, TBT_FEC_S, false,
                          payload);
    }
    for (i = 0; i < 117; i++)
        send_parity_payload(0, 0, 139, 117, 0, (u16)i, 139, 0, parity[i]);
    CHECK(f.rx_next_block == 1, "erasure: block delivered");
    CHECK(f.rx_next_deliver == 139, "erasure: frontier is 139 packets");
    CHECK(f.rx_deliver_bytes == 139u * TBT_FEC_S,
          "erasure: all 139 shares delivered");
    CHECK(f.rx_shape_bad == 0, "erasure: no shape rejection");
    for (i = 0; i < 117; i++)
        free(parity[i]);
}

/* The shape gate must count a bad share sum, a bad block ladder and a
 * second n/m for an existing block id. */
static void test_shape_rejection(void)
{
    clear_engine();
    set_downlink_shape();
    send_data(0, 0, 132, 124, 0, 0, TBT_FEC_S, false);
    send_data(0, 0, 140, 116, 0, 0, TBT_FEC_S, false);
    send_data(1, 0, 200, 56, 0, 0, TBT_FEC_S, false);
    send_data(2, 0, 131, 124, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_shape_bad == 3, "reject: three inconsistent shapes counted");
}

/* ---- TX idle seal ---------------------------------------------------- */

static struct engine tx_e;
static struct engine rx_e;
static int tx_fake_fd = -1;

static void tx_fake_carriers(void)
{
    int sv[2];
    int i;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    tx_fake_fd = sv[1];
    for (i = 0; i < TBT_CAR_MAX; i++)
    {
        struct carrier *c = calloc(1, sizeof(*c));

        c->e = &tx_e;
        c->fd = sv[1];
        c->index = i;
        c->up = true;
        tx_e.car[i] = c;
    }
    tx_e.n_car = TBT_CAR_MAX;
}

static void tx_clear_carriers(void)
{
    int i;

    for (i = 0; i < TBT_CAR_MAX; i++)
    {
        free(tx_e.car[i]);
        tx_e.car[i] = NULL;
    }
    tx_e.n_car = 0;
    if (tx_fake_fd >= 0)
    {
        close(tx_fake_fd);
        tx_fake_fd = -1;
    }
}

static struct flow *tx_flow_new(void)
{
    memset(&tx_e, 0, sizeof(tx_e));
    tx_e.near = true;
    tx_e.tx_seed = 1;
    tx_e.tx_shape.variant = TBT_FEC_SCLC_UP;
    tx_e.tx_shape.n = 8;
    tx_e.tx_shape.m = 24;
    tx_e.tx_shape.groups = 8;
    tx_e.tx_shape.block = TBT_FEC_SCLC_BLOCK_UP;
    tx_e.tx_shape.L = TBT_FEC_SCLC_L_UP;
    tx_e.tx_shape.rate_ppm = 250;
    tx_e.tx_shape.rate_256 = 64;
    tx_e.tx_n = 8;
    tx_e.tx_m = 24;
    tx_e.tx_groups = 8;
    return flow_alloc(&tx_e, 1, -1, true, NULL);
}

/* Feed every record the TX stored for its blocks into a fresh RX flow. */
static void tx_deliver(struct flow *tx, size_t *bytes, u64 *next)
{
    struct flow *rx;
    int b;

    memset(&rx_e, 0, sizeof(rx_e));
    rx_e.near = false;
    rx_e.tx_seed = tx_e.tx_seed;
    rx_e.tx_n = 8;
    rx_e.tx_m = 24;
    rx_e.tx_groups = 8;
    rx_e.tx_shape = tx_e.tx_shape;
    rx = flow_alloc(&rx_e, 1, -1, false, NULL);
    rx->rx_shape = tx->tx_shape;
    rx->rx_shape_set = true;
    rx->rx_seed = tx_e.tx_seed;
    rx->rx_n = 8;
    rx->rx_m = 24;
    rx->rx_groups = 8;
    rx->rx_shares = 32;
    for (b = 0; b < TBT_REPLAY_BLOCKS; b++)
    {
        struct replay_block *r = &tx->replay[b];
        u16 g, sh;

        if (!r->used)
            continue;
        for (g = 0; g < r->groups; g++)
            for (sh = 0; sh < (u16)(r->n + r->m); sh++)
            {
                u32 idx = (u32)g * (r->n + r->m) + sh;
                const u8 *rec;
                struct tbt_fec_hdr h;

                if (!r->lens[idx])
                    continue;
                rec = r->recs + (size_t)idx * TBT_FEC_REC_MAX;
                if (tbt_fec_rec_check(rec, r->lens[idx], TBT_FEC_S, &h))
                    continue;
                flow_rx_store(rx, &h, rec + TBT_FEC_HDR_LEN);
            }
    }
    *bytes = rx->rx_deliver_bytes;
    *next = rx->rx_next_block;
    flow_free(&rx_e, rx);
}

static void tx_feed_byte_pattern(struct flow *tx, size_t n, u8 seed)
{
    u8 *p = malloc(n);
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (u8)(seed + i * 7u);
    flow_tx_feed(tx, p, n);
    free(p);
}

static void tx_tick_idle(struct flow *tx)
{
    flow_tick(tx, tx->tx_activity_ns + TBT_TX_IDLE_SEAL_NS + 1000000u);
}

static void test_tx_idle_empty(void)
{
    struct flow *tx = tx_flow_new();

    tx_fake_carriers();
    tx_tick_idle(tx);
    CHECK(tx->tx_seq == 0 && tx->tx_block == 0,
          "idle empty: no packet and no block");
    CHECK(!tx->replay[0].used, "idle empty: no record emitted");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

static void test_tx_idle_3(void)
{
    struct flow *tx = tx_flow_new();
    struct tbt_fec_ack a;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_ACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_ACK_LEN];
    u16 rlen;
    size_t bytes = 0;
    u64 next = 0;

    tx_fake_carriers();
    tx_feed_byte_pattern(tx, 3, 0x11);
    CHECK(tx->tx_cur_len == 3 && tx->tx_seq == 0,
          "idle3: three bytes stay partial before the timer");
    tx_tick_idle(tx);
    CHECK(tx->tx_seq == 1, "idle3: one short packet sealed");
    CHECK(tx->tx_block == 1, "idle3: block advanced");
    CHECK(tx->tx_cur_len == 0 && tx->tx_share == 0 && tx->tx_group == 0,
          "idle3: partial state cleared");
    tx_deliver(tx, &bytes, &next);
    CHECK(bytes == 3 && next == 1, "idle3: peer delivers three bytes");

    memset(&a, 0, sizeof(a));
    a.next_deliver = 1;
    a.credit_packets = (u32)(TBT_RX_SLOTS - 1) * 8u * (8u + 24u);
    a.deliver_bytes = 3;
    CHECK(tbt_fec_ack_pack(&a, body) == 0, "idle3: pack ACK");
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ACK;
    h.payload_len = TBT_FEC_ACK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = 1;
    CHECK(make_record(&tx_e, &h, body, rec, &rlen) == 0, "idle3: make ACK");
    CHECK(handle_fec(&tx_e, NULL, rec, rlen) == 0, "idle3: receive ACK");
    CHECK(tx->tx_ack_next == 1, "idle3: ACK advances the TX frontier");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

static void test_tx_idle_200(void)
{
    struct flow *tx = tx_flow_new();
    size_t bytes = 0;
    u64 next = 0;

    tx_fake_carriers();
    tx_feed_byte_pattern(tx, 200, 0x33);
    tx_tick_idle(tx);
    CHECK(tx->tx_seq == 1 && tx->tx_block == 1,
          "idle200: one short packet and one block");
    tx_deliver(tx, &bytes, &next);
    CHECK(bytes == 200 && next == 1, "idle200: peer delivers 200 bytes");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

static void test_tx_idle_short_then_long(void)
{
    struct flow *tx = tx_flow_new();
    size_t bytes = 0;
    u64 next = 0;

    tx_fake_carriers();
    tx_feed_byte_pattern(tx, 3, 0x55);
    tx_feed_byte_pattern(tx, 5000, 0x77);
    tx_tick_idle(tx);
    CHECK(tx->tx_seq == 5, "idle short+long: four full and one short share");
    CHECK(tx->tx_block == 1, "idle short+long: one block, no duplicate");
    CHECK(tx->tx_cur_len == 0 && tx->tx_share == 0 && tx->tx_group == 0,
          "idle short+long: no partial state left");
    tx_deliver(tx, &bytes, &next);
    CHECK(bytes == 5003 && next == 1,
          "idle short+long: peer delivers all 5003 bytes");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* Tx credit: a one-block peer grant is stop-and-wait; a seven-block grant
 * converts to seven blocks for both groups=1 and groups=8. */
static void test_tx_peer_credit(void)
{
    struct flow *tx = tx_flow_new();
    u64 block_shares;

    tx_fake_carriers();
    block_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_credit_shares = block_shares; /* one block */
    CHECK(flow_tx_credit_ok(tx), "peer1: first block open");
    tx->tx_seq = (u64)tx->groups * tx->n;
    CHECK(flow_tx_credit_ok(tx), "peer1: the one granted block fits");
    tx->tx_seq = (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "peer1: the next packet waits");

    tx->tx_credit_shares = 7 * block_shares;
    tx->tx_seq = 0;
    CHECK(tx->tx_credit_shares / block_shares == 7,
          "peer7: groups=8 converts to seven blocks");
    tx->tx_seq = 4 * (u64)tx->groups * tx->n;
    CHECK(flow_tx_credit_ok(tx), "peer7: four-block local cap is open");
    tx->tx_seq = 4 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "peer7: beyond the four-block cap stops");

    /* groups=1 downlink: the same seven-block conversion. */
    tx->groups = 1;
    tx->n = 128;
    tx->m = 128;
    block_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_credit_shares = 7 * block_shares;
    tx->tx_seq = 4 * (u64)tx->groups * tx->n;
    CHECK(tx->tx_credit_shares / block_shares == 7,
          "peer7: groups=1 converts to seven blocks");
    CHECK(flow_tx_credit_ok(tx), "peer7: groups=1 four-block cap is open");
    tx->tx_seq = 4 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "peer7: groups=1 stops at the cap");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* Consecutive short blocks cost one replay slot each. Seven may be
 * outstanding; the eighth waits for an ACK, so the ring never wraps. */
static void test_tx_block_credit(void)
{
    struct flow *tx = tx_flow_new();
    u64 block_shares;
    int i;

    tx_fake_carriers();
    block_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_credit_shares = 7 * block_shares;
    for (i = 0; i < 7; i++)
    {
        CHECK(flow_tx_block_credit_ok(tx), "blocks: next short block allowed");
        tx_feed_byte_pattern(tx, 3, (u8)(0x20 + i));
        tx_tick_idle(tx);
        CHECK(tx->tx_block == (u64)i + 1, "blocks: short block sealed");
    }
    CHECK(!flow_tx_block_credit_ok(tx),
          "blocks: the eighth unacked block is refused");

    /* Bypass the app gate: the idle sealer itself must refuse to wrap. */
    tx_feed_byte_pattern(tx, 3, 0x77);
    tx_tick_idle(tx);
    CHECK(tx->tx_block == 7, "blocks: idle sealer holds the eighth block");
    CHECK(tx->tx_cur_len == 3, "blocks: the refused block stays partial");

    /* One ACK frees exactly one slot; the held short block then seals. */
    tx->tx_ack_next = (u64)tx->groups * tx->n;
    CHECK(flow_tx_block_credit_ok(tx), "blocks: ACK opens one slot");
    tx_tick_idle(tx);
    CHECK(tx->tx_block == 8, "blocks: the held block seals after the ACK");
    CHECK(!flow_tx_block_credit_ok(tx),
          "blocks: seven unacked blocks remain the limit");
    CHECK(tx->tx_cur_len == 0, "blocks: no partial state after the seal");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* Shape switches mid-stream with three DATA shares dropped per block: the
 * old and new n/m must decode from each block header and deliver 4 MiB+
 * byte-exactly. */
static void test_shape_switch_stream(void)
{
    static const u16 shapes[] = { 128, 139, 146, 160, 163, 152 };
    const size_t target = 4u * 1024u * 1024u + 65536u;
    u8 *expected = malloc(target + 512u * 1024u);
    size_t expected_len = 0;
    u64 block_id = 0, seq0 = 0, rng = 0x123456789abcdefull;
    u8 dig_e[32], dig_o[32];
    int shape_i = 0;

    clear_engine();
    set_downlink_shape();
    while (expected_len < target)
    {
        u16 n = shapes[shape_i++ % (int)(sizeof(shapes) / sizeof(shapes[0]))];
        u16 m = (u16)(256 - n);
        u8 *data = malloc((size_t)n * TBT_FEC_S);
        u8 *parity = malloc((size_t)m * TBT_FEC_S);
        u8 *dp[256], *pp[256];
        struct tbt_fec_grouped *codec;
        u16 i;

        if (!data || !parity || !expected)
            abort();
        for (i = 0; i < (u16)(n * TBT_FEC_S); i++)
        {
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            data[i] = (u8)rng;
        }
        memcpy(expected + expected_len, data, (size_t)n * TBT_FEC_S);
        expected_len += (size_t)n * TBT_FEC_S;
        for (i = 0; i < n; i++) dp[i] = data + (size_t)i * TBT_FEC_S;
        for (i = 0; i < m; i++) pp[i] = parity + (size_t)i * TBT_FEC_S;
        codec = flow_rx_codec(&f, n, m);
        CHECK(codec != NULL, "switch: codec for the block's n/m");
        if (!codec || tbt_fec_grouped_encode(codec, 0, dp, n, pp))
        {
            free(data); free(parity);
            break;
        }
        /* Drop three DATA shares; all parity still arrives. */
        for (i = 0; i < n; i++)
        {
            if (i == 1 || i == (u16)(n / 2) || i == (u16)(n - 2))
                continue;
            send_data_payload(block_id, seq0, n, m, 0, i, TBT_FEC_S, false,
                              dp[i]);
        }
        for (i = 0; i < m; i++)
            send_parity_payload(block_id, seq0, n, m, 0, i, n, 0, pp[i]);
        CHECK(f.rx_next_block == block_id + 1,
              "switch: the mixed-shape block decodes");
        block_id++;
        seq0 += n;
        free(data);
        free(parity);
    }
    CHECK(expected_len >= 4u * 1024u * 1024u,
          "switch: more than 4 MiB transmitted");
    CHECK(f.rx_deliver_bytes == expected_len,
          "switch: every expected byte delivered");
    CHECK(f.out_len == expected_len && f.out_off == 0,
          "switch: the delivery buffer holds every byte");
    CHECK(f.rx_shape_bad == 0 && f.rx_window_drop == 0,
          "switch: no shape rejection or window drop");
    CHECK(f.rx_credit_bad == 0, "switch: credit invariant holds");
    if (f.out_len == expected_len)
    {
        sha_digest(expected, expected_len, dig_e);
        sha_digest(f.out, f.out_len, dig_o);
        CHECK(memcmp(dig_e, dig_o, 32) == 0,
              "switch: byte-exact SHA over 4 MiB+");
    }
    free(expected);
}

/* adaptive=off must not install the control law and must not emit a
 * periodic FBK target; CFG_ACK still goes out. */
static void make_down_cfg(u8 body[TBT_FEC_CFG_LEN], u32 epoch)
{
    struct tbt_fec_cfg c;

    memset(&c, 0, sizeof(c));
    c.epoch = epoch;
    c.n = 128;
    c.m = 128;
    c.groups = 1;
    c.flags = TBT_FEC_CFG_F_INIT;
    c.seed = 7;
    c.codec_variant = 2;
    c.L = 8;
    c.block = 256;
    c.rate_ppm = 500000;
    CHECK(tbt_fec_cfg_pack(&c, body) == 0, "adaptive: pack CFG");
}

static void test_adaptive_off(void)
{
    u8 body[TBT_FEC_CFG_LEN];

    clear_engine();
    e.n_car = 256;
    e.cfg.cfg.adaptive = 0;
    f.rx_shape_set = false;
    make_down_cfg(body, 1);
    CHECK(handle_cfg(&f, body) == 0, "adaptive off: CFG installs");
    CHECK(!f.ctl_ready, "adaptive off: control law not initialized");
    CHECK(f.rx_n == 128 && f.rx_m == 128, "adaptive off: shape stays 128/128");
    clear_pending();
    CHECK(flow_send_fbk(&f, 0, 1) == 0, "adaptive off: periodic FBK is a no-op");
    CHECK(e.npending == 0, "adaptive off: no FBK target is queued");

    e.cfg.cfg.adaptive = 1;
    f.rx_shape_set = false;
    make_down_cfg(body, 2);
    CHECK(handle_cfg(&f, body) == 0, "adaptive on: CFG installs");
    CHECK(f.ctl_ready, "adaptive on: control law is initialized");
}

/* nonce= on the client is the HELLO session nonce; an unset near generates
 * one, an unset far adopts the peer's. */
static void test_session_nonce_config(void)
{
    u8 pinned[16];

    memset(pinned, 0xa5, sizeof(pinned));
    clear_engine();
    e.near = true;
    e.cfg.cfg.nonce_set = true;
    memcpy(e.cfg.cfg.nonce, pinned, sizeof(pinned));
    engine_init_session_nonce(&e);
    CHECK(e.session_nonce_set &&
              memcmp(e.session_nonce, pinned, sizeof(pinned)) == 0,
          "nonce: configured near nonce is used");

    clear_engine();
    e.near = true;
    e.rng = 0x123456789abcdefull;
    engine_init_session_nonce(&e);
    CHECK(e.session_nonce_set, "nonce: unset near generates one");

    clear_engine();
    e.near = false;
    engine_init_session_nonce(&e);
    CHECK(!e.session_nonce_set, "nonce: unset far adopts the peer nonce");
}

/* The stall dump must fire once for an incomplete head and expose both
 * counters: rx_block_stalled and rx_shape_mismatch (a record whose h.n/h.m
 * differs from the committed split). */
static void test_stall_dump(void)
{
    u64 now;

    clear_engine();
    set_downlink_shape();
    e.cfg.verbose = 1;
    send_data(0, 0, 128, 128, 0, 0, TBT_FEC_S, false);
    send_data(1, 0, 139, 117, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_shape_mismatch == 1, "stall: one mismatched record counted");

    now = now_ns();
    f.rx[0].first_ns = now - 300000000ull;
    flow_tick(&f, now);
    CHECK(f.rx_block_stalled == 1, "stall: incomplete head counted once");
    CHECK(f.rx[0].stall_reported, "stall: head marked reported");
    flow_tick(&f, now + 100000000ull);
    CHECK(f.rx_block_stalled == 1, "stall: a second tick does not double count");
    e.cfg.verbose = 0;
}

/* The FBK target is a commitment: the reported m_current/m_target and
 * n_current are the split this receiver just adopted, not a suggestion that
 * its RX ignores. */
static void test_fbk_commit(void)
{
    int i;
    int found = 0;

    clear_engine();
    f.ctl_ready = false;
    e.cfg.cfg.adaptive = 1;
    CHECK(flow_commit_shape(&f, 9, 23), "fbk: commit 9/23");
    clear_pending();
    CHECK(flow_send_fbk(&f, 0, 1) == 0, "fbk: send");
    for (i = 0; i < e.npending; i++)
    {
        const u8 *rec = e.pending[i].rec;

        if (rec[0] != TBT_FEC_FBK)
            continue;
        found++;
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_M_CURRENT] == 23,
              "fbk: m_current is the committed m");
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_M_TARGET] == 23,
              "fbk: m_target is the committed m");
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_N_CURRENT] == 9,
              "fbk: n_current is the committed n");
    }
    CHECK(found == 1, "fbk: one report queued");
    CHECK(f.rx_n == 9 && f.rx_m == 23, "fbk: RX expectation moves with it");
    CHECK(f.rx_credit_shares == (TBT_RX_SLOTS - 1) * 8u * 32u,
          "fbk: credit recomputed from groups*(n+m)");
    CHECK(f.rx_credit_bad == 0, "fbk: credit invariant holds");
}

/* A local FIN whose peer FIN never arrives used to occupy a flow slot
 * forever: fin_applied stays false, so the normal release path cannot run.
 * The idle reclaim must free an acknowledged flow at TBT_FLOW_IDLE_NS, must
 * wait for the hard cap when data is still unacknowledged, and must never
 * touch a flow that is still sending. */
static void test_half_open_reclaim(void)
{
    struct flow *f;
    u64 t0 = 5000000000ull;
    int i;

    f = tx_flow_new();
    CHECK(f != NULL, "half-open: acked flow allocated");
    f->fin_sent = true;
    f->tx_seq = 0;
    f->tx_ack_next = 0;
    flow_tick(f, t0);
    CHECK(tx_e.flow[0] == f, "half-open: the first tick arms the deadline");
    flow_tick(f, t0 + TBT_FLOW_IDLE_NS - 1);
    CHECK(tx_e.flow[0] == f, "half-open: acked flow waits for the threshold");
    flow_tick(f, t0 + TBT_FLOW_IDLE_NS);
    CHECK(tx_e.flow[0] == NULL, "half-open: acked flow freed at the threshold");
    CHECK(tx_e.flow_freed == 1, "half-open: the reclaim is counted");
    f = flow_alloc(&tx_e, 100, -1, true, NULL);
    CHECK(f != NULL, "half-open: the freed slot is reusable");
    flow_free(&tx_e, f);

    /* A legal half-close followed by a long download: the local GET was
     * acknowledged immediately, but the receive direction keeps moving for
     * 120 s. The idle reclaim must never fire while shares keep arriving;
     * only a full idle window after the last one may release the flow. */
    f = tx_flow_new();
    CHECK(f != NULL, "half-open: long download allocated");
    f->fin_sent = true;
    f->tx_seq = 0;
    f->tx_ack_next = 0;
    flow_tick(f, t0);
    for (i = 1; i <= 12 && tx_e.flow[0] == f; i++)
    {
        u64 at = t0 + (u64)i * (TBT_FLOW_IDLE_NS / 3);

        flow_rx_progress(f, at);
        flow_tick(f, at);
    }
    CHECK(tx_e.flow[0] == f,
          "half-open: 120 s of rx progress keeps the download");
    if (tx_e.flow[0] == f)
    {
        u64 last = t0 + 12ull * (TBT_FLOW_IDLE_NS / 3);

        flow_tick(f, last + TBT_FLOW_IDLE_NS - 1);
        CHECK(tx_e.flow[0] == f,
              "half-open: release waits for the rx idle window");
        flow_tick(f, last + TBT_FLOW_IDLE_NS);
        CHECK(tx_e.flow[0] == NULL,
              "half-open: an idle rx releases the half-open flow");
    }

    f = tx_flow_new();
    CHECK(f != NULL, "half-open: unacked flow allocated");
    f->fin_sent = true;
    f->tx_seq = 10;
    f->tx_ack_next = 5;
    flow_tick(f, t0);
    flow_tick(f, t0 + TBT_FLOW_IDLE_NS);
    CHECK(tx_e.flow[0] == f,
          "half-open: unacked flow survives the idle threshold");
    flow_tick(f, t0 + TBT_FLOW_IDLE_NS + TBT_FLOW_CLOSE_NS - 1);
    CHECK(tx_e.flow[0] == f, "half-open: unacked flow waits for the hard cap");
    flow_tick(f, t0 + TBT_FLOW_IDLE_NS + TBT_FLOW_CLOSE_NS);
    CHECK(tx_e.flow[0] == NULL, "half-open: unacked flow freed at the hard cap");

    f = tx_flow_new();
    CHECK(f != NULL, "half-open: sending flow allocated");
    flow_tick(f, t0 + 365ull * 24ull * 3600ull * 1000000000ull);
    CHECK(tx_e.flow[0] == f,
          "half-open: a flow that never sent FIN is never idle-reclaimed");
    flow_free(&tx_e, f);
}

/* The eighth slot is the last one; the ninth allocation is refused with
 * -ENOSPC and the refusal is visible. Releasing one flow lets the next
 * allocation succeed without clearing the counter. */
static void test_flow_table_refusal(void)
{
    struct flow *f;
    int why = 0;
    int i;

    f = tx_flow_new();
    CHECK(f != NULL, "table: first flow allocated");
    for (i = 1; i < TBT_FLOW_MAX; i++)
    {
        f = flow_alloc(&tx_e, (u64)i + 1, -1, true, &why);
        CHECK(f != NULL, "table: flow fits under the limit");
    }
    CHECK(tx_e.flow_accepted == TBT_FLOW_MAX,
          "table: accepted counter reaches the limit");
    CHECK(tx_e.flow_peak == TBT_FLOW_MAX, "table: peak reaches the limit");
    why = 0;
    f = flow_alloc(&tx_e, 100, -1, true, &why);
    CHECK(f == NULL && why == -ENOSPC,
          "table: the ninth flow is refused with ENOSPC");
    CHECK(tx_e.flow_refused == 1, "table: the refusal is counted");
    CHECK(tx_e.flow_accepted == TBT_FLOW_MAX,
          "table: a refusal does not increment accepted");
    flow_free(&tx_e, tx_e.flow[3]);
    CHECK(tx_e.flow_freed == 1, "table: the slot release is counted");
    why = 0;
    f = flow_alloc(&tx_e, 101, -1, true, &why);
    CHECK(f != NULL, "table: a released slot accepts the next flow");
    CHECK(tx_e.flow_accepted == TBT_FLOW_MAX + 1,
          "table: accepted increments after reuse");
    CHECK(tx_e.flow_refused == 1, "table: reuse does not increment refused");
    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (tx_e.flow[i])
            flow_free(&tx_e, tx_e.flow[i]);
}

int main(void)
{
    test_marker_short_sequence();
    test_credit_inflight();
    test_window_and_nack_base();
    test_multishape_sequence();
    test_16m_boundary();
    test_downlink_shapes();
    test_far_shape_ladder();
    test_downlink_erasure();
    test_shape_rejection();
    test_shape_switch_stream();
    test_adaptive_off();
    test_session_nonce_config();
    test_stall_dump();
    test_tx_idle_empty();
    test_tx_peer_credit();
    test_tx_block_credit();
    test_tx_idle_3();
    test_tx_idle_200();
    test_tx_idle_short_then_long();
    test_fbk_commit();
    test_half_open_reclaim();
    test_flow_table_refusal();

    printf("test_client_rx: %d checks, %d failures\n", checks, failures);
    free(f.out);
    release_blocks();
    release_codecs();
    return failures ? 1 : 0;
}
