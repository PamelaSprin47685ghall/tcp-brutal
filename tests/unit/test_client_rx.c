/* test_client_rx.c - the near-side receive state machine under markers,
 * short blocks, per-block n/m and the 66-slot / 64-replay-block window.
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
 *   - ACK credit is (TBT_RX_SLOTS - 2) * groups * (rx_n + rx_m);
 *   - n/m are read from every block header, not from the session's first CFG.
 * Counters check that a stale slot below rx_next_block can never be NACKed
 * and that a late replay is counted instead of silently wedging.
 */
#define _GNU_SOURCE

#define TBT_CLIENT_TEST_HOOKS 1
#include "tbt_client_engine.h"
#include "tbt_client_engine.c"

/* The HELLO tag binds the outer carrier path; every HELLO in this test drives
 * path 0. The production receive path passes the real header path_id. */
#define hello_crypto(e, body, len) hello_crypto((e), (body), (len), 0)

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
    while (e.car_list)
    {
        struct carrier *c = e.car_list;

        e.car_list = c->next;
        if (c->fd >= 0)
            close(c->fd);
        free(c);
    }
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

static int last_ack_rtt(void)
{
    int i;

    for (i = e.npending - 1; i >= 0; i--)
        if (e.pending[i].rec[0] == TBT_FEC_ACK)
            return (int)tbt_le32(e.pending[i].rec + 60);
    return -1;
}

/* The NACK record one group's request occupies. The engine sends one NACK per
 * group, so a block with several groups has one record per group and the group
 * bitmap says which. */
static const u8 *nack_for_group(u16 group)
{
    int i;

    for (i = 0; i < e.npending; i++)
        if (e.pending[i].rec[0] == TBT_FEC_NACK &&
            (e.pending[i].rec[TBT_FEC_HDR_LEN + group / 8] &
             (u8)(1u << (group % 8))))
            return e.pending[i].rec;
    return NULL;
}

static bool nack_names(const u8 *rec, u16 share)
{
    return (rec[TBT_FEC_HDR_LEN + TBT_FEC_BITMAP_BYTES + share / 8] &
            (u8)(1u << (share % 8))) != 0;
}

static int nack_bit_count(const u8 *rec)
{
    u16 s;
    int n = 0;

    for (s = 0; s < TBT_FEC_MAX_SHARES; s++)
        if (nack_names(rec, s))
            n++;
    return n;
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * 8u * (8u + 24u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 8u * (8u + 24u),
          "sequence: ACK credit is 6 blocks of groups*(n+m)");
    CHECK(f.rx_credit_bad == 0, "sequence: credit invariant holds");
    CHECK(f.rx_stale_nack == 0 && f.rx_late_share == 0 &&
              f.rx_window_drop == 0 && f.rx_shape_bad == 0,
          "sequence: no stale request or late drop");

    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 1000000u);
    CHECK(count_nacks(ids, 16) == 0, "sequence: no NACK for delivered blocks");
}

/* The sender spends its full lead: one head under delivery plus slots-1 in
 * flight fill the ring, and no block is dropped. */
static void test_credit_inflight(void)
{
    int i;

    clear_engine();
    set_downlink_shape();
    send_data(0, 0, 132, 124, 0, 0, TBT_FEC_S, false);
    for (i = 1; i < TBT_RX_SLOTS; i++)
        marker_block((u64)i, (u64)i * 132, 132, 124);

    CHECK(f.rx_used == TBT_RX_SLOTS, "credit: every slot held");
    CHECK(f.rx_window_drop == 0, "credit: full credit causes no drop");
    CHECK(f.rx_inflight_bad == 0, "credit: in-flight count is within slots-1");
    clear_pending();
    CHECK(flow_send_ack(&f) == 0, "credit: ACK sent");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (132u + 124u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * (132u + 124u),
          "credit: ACK grants slots-2 whole blocks");
    CHECK(f.rx_credit_bad == 0, "credit: credit invariant holds");

    /* Complete the head; the markers drain behind it. */
    send_parity(0, 0, 132, 124, 0, 1, 0);
    CHECK(f.rx_next_block == TBT_RX_SLOTS,
          "credit: head and in-flight blocks drained");
    CHECK(f.rx_used == 0, "credit: ring empty after drain");
    CHECK(f.rx_window_drop == 0, "credit: drain caused no drop");
    CHECK(f.rx_inflight_bad == 0, "credit: invariant held through drain");

    /* The next block fits because the delivered slot was released. */
    marker_block(TBT_RX_SLOTS, TBT_RX_SLOTS * 132, 146, 110);
    CHECK(f.rx_next_block == TBT_RX_SLOTS + 1,
          "credit: post-drain block accepted");
    CHECK(f.rx_used == 0 && f.rx_window_drop == 0,
          "credit: post-drain ring empty and no drop");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * (146u + 110u),
          "credit: post-drain credit follows the new split");
}

/* The ring holds blocks 0..slots-1. One block past it before the head
 * completes is a sender violation and must be refused; NACK must name block
 * 0 only; the stub block 0 then completes with one real share and seven
 * group markers. */
static void test_window_and_nack_base(void)
{
    u64 ids[16];
    int n;
    int i;

    clear_engine();
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    for (i = 1; i < TBT_RX_SLOTS; i++)
        marker_block((u64)i, (u64)i * 64, 8, 24);

    send_data(TBT_RX_SLOTS, TBT_RX_SLOTS * 64, 8, 24, 0, 0, TBT_FEC_S,
              false);
    CHECK(f.rx_window_drop == 1, "window: block next+slots refused");
    CHECK(f.rx_next_block == 0, "window: head still block 0");

    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS +
                         2u * TBT_NACK_RETRY_NS + 1000000u);
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
    CHECK(f.rx_next_block == TBT_RX_SLOTS, "window: every block drained");
    CHECK(f.rx_next_deliver == 1, "window: frontier is the one real share");
    CHECK(f.rx_deliver_bytes == TBT_FEC_S, "window: delivered one share");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * 8u * (8u + 24u)),
          "window: ACK credit follows the ring");

    full_block(TBT_RX_SLOTS, TBT_RX_SLOTS * 64, 8, 24);
    CHECK(f.rx_next_block == TBT_RX_SLOTS + 1,
          "window: block next+slots accepted after drain");
    CHECK(f.rx_next_deliver == 65, "window: full block adds 64 packets");

    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_late_share == 1, "late: old replay counted");
    CHECK(f.rx_next_block == TBT_RX_SLOTS + 1, "late: cursor unchanged");
}

/* A short 1x256 downlink block: three DATA shares present, share 3 missing,
 * no PARITY yet. The kernel's tbt_fec_nack_data_limit()/parity_limit() name
 * only what the receiver can prove: the DATA gap below the highest present
 * share, and parity row 0, which states the cut. Naming the 125 padding DATA
 * slots and the 127 parity rows a short block never had is the
 * fec_replay_miss_never_stored storm (762 -> 1016 on HKG0 V6). */
static void test_nack_short_block_limits(void)
{
    const u8 *nack;
    u64 ids[8];
    u64 t1, t2;
    u16 s;

    clear_engine();
    set_downlink_shape();
    e.rtt_us = 0;
    send_data(0, 0, 128, 128, 0, 0, TBT_FEC_S, false);
    send_data(0, 0, 128, 128, 0, 1, TBT_FEC_S, false);
    send_data(0, 0, 128, 128, 0, 2, TBT_FEC_S, false);
    clear_pending();

    t1 = now_ns() + TBT_NACK_FIRST_NS + 2ull * TBT_NACK_RETRY_NS +
         1000000ull;
    flow_rx_nack(&f, t1);
    nack = nack_for_group(0);
    CHECK(nack != NULL, "nack limits: the head group is requested");
    if (!nack)
        return;
    /* Share 3 is not proven missing: win_len may be 3 and the sender may
     * never have written it. Only parity row 0 can say, so it is the whole
     * request. The old rule named 125 padding DATA slots and all 128 rows. */
    CHECK(nack_bit_count(nack) == 1,
          "nack limits: parity 0 alone, not the whole block");
    CHECK(nack_names(nack, 128), "nack limits: parity row 0 asks for the cut");
    for (s = 0; s < 128; s++)
        if (nack_names(nack, s))
            break;
    CHECK(s == 128, "nack limits: no DATA share is proven missing yet");
    for (s = 129; s < 256; s++)
        if (nack_names(nack, s))
            break;
    CHECK(s == 256, "nack limits: no parity row past row 0 before the cut");

    /* Row 0 was lost and row 1 arrived. Every PARITY row carries the block's
     * (win_len, epoch), so a single row states the cut and the request
     * becomes exact: the two real DATA gaps and the rows below win_len. No
     * widening to m is needed, and none may happen. */
    send_parity_payload(0, 0, 128, 128, 0, 1, 5, 784, payload);
    CHECK(f.rx[0].g[0].real_set && f.rx[0].g[0].real == 5,
          "nack limits: parity row 1 states the cut");
    clear_pending();
    t2 = t1 + TBT_NACK_RETRY_NS + 2ull * TBT_NACK_RETRY_NS + 1000000ull;
    flow_rx_nack(&f, t2);
    nack = nack_for_group(0);
    CHECK(nack != NULL, "nack limits: the retry is sent");
    if (!nack)
        return;
    CHECK(nack_bit_count(nack) == 6,
          "nack limits: two DATA gaps and four parity rows below win_len");
    CHECK(nack_names(nack, 3) && nack_names(nack, 4),
          "nack limits: both real DATA gaps are named");
    CHECK(nack_names(nack, 128) && nack_names(nack, 130) &&
              nack_names(nack, 131) && nack_names(nack, 132),
          "nack limits: the missing rows below win_len are named");
    CHECK(!nack_names(nack, 129), "nack limits: a present row is not named");
    for (s = 133; s < 256; s++)
        if (nack_names(nack, s))
            break;
    CHECK(s == 256, "nack limits: no parity row at or above win_len");

    /* The cut is what made the block decodable: the two missing DATA shares
     * complete it with no codec at all. */
    send_data(0, 0, 128, 128, 0, 3, TBT_FEC_S, false);
    send_data(0, 0, 128, 128, 0, 4, 784, true);
    CHECK(f.rx_next_block == 1 && f.rx_next_deliver == 5,
          "nack limits: the short block delivers five packets");
    CHECK(f.rx_deliver_bytes == 4u * TBT_FEC_S + 784u,
          "nack limits: the padded tail delivers its real length");
    /* The short-block wake line admits an attempt as soon as the present
     * shares can cover `real`, so a failed try may be counted before the
     * last DATA shares arrive. The block still delivers through the
     * systematic path, never through a successful decode. */
    CHECK(f.rx_decode_ok == 0,
          "nack limits: the short block delivers without a successful decode");
    clear_pending();
    flow_rx_nack(&f, t2 + TBT_NACK_RETRY_NS + 1000000ull);
    CHECK(count_nacks(ids, 8) == 0,
          "nack limits: a delivered block is never named again");
}

/* A block whose first share never arrived has no ring entry. The normal
 * NACK walk cannot name it, so the frontier waits forever while later blocks
 * sit in the ring (measured at 256 MiB: next=1135, ring 1136..1142). Probe
 * parity row 0 for the missing head, as the module does. */
static void test_sparse_probe(void)
{
    const u8 *nack;
    u64 t0;

    clear_engine();
    set_downlink_shape();
    marker_block(1, 1u * 128, 128, 128);
    CHECK(f.rx_next_block == 0, "probe: block 0 is the missing head");
    clear_pending();
    t0 = now_ns();
    f.rx_hole_ns = t0;
    f.rx_probe_ns = 0;
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS +
                         2ull * TBT_NACK_RETRY_NS + 1000000ull);
    nack = nack_for_group(0);
    CHECK(nack != NULL, "probe: a sparse NACK reaches the head block");
    if (nack)
        CHECK(nack_names(nack, 128),
              "probe: parity row 0 names the block cut");
    CHECK(f.rx_sparse_probe == 1, "probe: the probe is counted once");
}

/* A 200 ms carrier RTT: the first NACK waits one trip after the gap first
 * appeared, not the 30 ms floor, and the ACK carries the sampled value
 * instead of the old 1 ms placeholder. The retry interval follows the same
 * trip, so a replay still in flight is not requested twice. */
static void test_nack_hold_follows_rtt(void)
{
    u64 ids[16];
    u64 t0;
    u64 now;

    clear_engine();
    e.rtt_us = 200000;
    e.rtt_sample_ns = 0;

    /* One real share leaves block 0 incomplete, so it is NACK-eligible. */
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_next_block == 0, "rtt: head is block 0");
    t0 = now_ns();
    f.rx[0].first_ns = t0;
    f.rx[0].last_nack_ns = 0;
    f.rx[0].nack_tries = 0;

    clear_pending();
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS + 1000000ull);
    CHECK(count_nacks(ids, 16) == 0,
          "rtt: 30 ms is too early at a 200 ms RTT");

    now = t0 + TBT_NACK_FIRST_NS + 200000000ull + 1000000ull;
    clear_pending();
    flow_rx_nack(&f, now);
    CHECK(count_nacks(ids, 16) >= 1,
          "rtt: 30 ms + RTT releases the first NACK");

    clear_pending();
    flow_rx_nack(&f, now + TBT_NACK_RETRY_NS + 1000000ull);
    CHECK(count_nacks(ids, 16) == 0,
          "rtt: the retry also waits the RTT");

    clear_pending();
    flow_rx_nack(&f, now + TBT_NACK_RETRY_NS + 200000000ull + 1000000ull);
    CHECK(count_nacks(ids, 16) >= 1,
          "rtt: retry after one RTT releases again");

    clear_pending();
    CHECK(flow_send_ack(&f) == 0, "rtt: ACK sent");
    CHECK(last_ack_rtt() == 200000,
          "rtt: ACK carries the measured RTT, not 1000 us");

    /* The module's hold is one carrier round trip floored at two retry
     * intervals, with no deviation term. */
    clear_engine();
    e.rtt_us = 150000;
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    t0 = now_ns();
    f.rx[0].first_ns = t0;
    f.rx[0].last_nack_ns = 0;
    f.rx[0].nack_tries = 0;

    clear_pending();
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS + 150000000ull - 1000000ull);
    CHECK(count_nacks(ids, 16) == 0,
          "rtt: 30 ms + the RTT is still too early");

    clear_pending();
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS + 150000000ull + 1000000ull);
    CHECK(count_nacks(ids, 16) >= 1,
          "rtt: 30 ms + one RTT releases");

    clear_pending();
    CHECK(flow_send_ack(&f) == 0, "rtt: ACK sent");
    CHECK(last_ack_rtt() == 150000,
          "rtt: ACK carries the measured RTT");

    /* A 1 s sample would hold 1 s; one loss spike must not freeze repair,
     * so the hold stops at TBT_NACK_RTT_HOLD_MAX_NS (now 1 s). */
    clear_engine();
    e.rtt_us = 1000000;
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    t0 = now_ns();
    f.rx[0].first_ns = t0;
    f.rx[0].last_nack_ns = 0;
    f.rx[0].nack_tries = 0;

    clear_pending();
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS +
                         TBT_NACK_RTT_HOLD_MAX_NS - 1000000ull);
    CHECK(count_nacks(ids, 16) == 0,
          "rtt: the hold is capped at 1 s");

    clear_pending();
    flow_rx_nack(&f, t0 + TBT_NACK_FIRST_NS +
                         TBT_NACK_RTT_HOLD_MAX_NS + 1000000ull);
    CHECK(count_nacks(ids, 16) >= 1,
          "rtt: the cap releases on time");
}

/* The far side reaps a stall-dead carrier, and on a lossy WAN its FIN can be
 * lost. Only TCP keepalive then closes the near socket; the 2 h default left
 * HKG0 carriers half-open ESTABLISHED (256 -> 98). Pin the options the
 * kernel actually reports on a connected carrier socket. */
static void test_carrier_keepalive_sockopts(void)
{
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    int lfd, cfd, afd;
    int v;
    socklen_t vlen;

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(lfd >= 0, "keepalive: listen socket");
    if (lfd < 0)
        return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) == 0,
          "keepalive: bind loopback");
    CHECK(listen(lfd, 1) == 0, "keepalive: listen");
    CHECK(getsockname(lfd, (struct sockaddr *)&sin, &slen) == 0,
          "keepalive: learn the bound port");
    cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(cfd >= 0, "keepalive: carrier socket");
    if (cfd < 0)
    {
        close(lfd);
        return;
    }
    CHECK(connect(cfd, (struct sockaddr *)&sin, sizeof(sin)) == 0,
          "keepalive: loopback connect");
    afd = accept(lfd, NULL, NULL);
    CHECK(afd >= 0, "keepalive: accept");
    close(lfd);

    set_carrier_sockopts(cfd);

    v = 0;
    vlen = sizeof(v);
    CHECK(getsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &v, &vlen) == 0 &&
              v != 0,
          "keepalive: SO_KEEPALIVE is on");
#ifdef TCP_KEEPIDLE
    v = 0;
    vlen = sizeof(v);
    CHECK(getsockopt(cfd, IPPROTO_TCP, TCP_KEEPIDLE, &v, &vlen) == 0 &&
              v == TBT_CARRIER_KEEPIDLE_S,
          "keepalive: TCP_KEEPIDLE is 15 s");
#endif
#ifdef TCP_KEEPINTVL
    v = 0;
    vlen = sizeof(v);
    CHECK(getsockopt(cfd, IPPROTO_TCP, TCP_KEEPINTVL, &v, &vlen) == 0 &&
              v == TBT_CARRIER_KEEPINTVL_S,
          "keepalive: TCP_KEEPINTVL is 5 s");
#endif
#ifdef TCP_KEEPCNT
    v = 0;
    vlen = sizeof(v);
    CHECK(getsockopt(cfd, IPPROTO_TCP, TCP_KEEPCNT, &v, &vlen) == 0 &&
              v == TBT_CARRIER_KEEPCNT,
          "keepalive: TCP_KEEPCNT is 3");
#endif
#ifdef TCP_USER_TIMEOUT
    v = 0;
    vlen = sizeof(v);
    CHECK(getsockopt(cfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &v, &vlen) == 0 &&
              v == TBT_CARRIER_USER_TIMEOUT_MS,
          "keepalive: TCP_USER_TIMEOUT is 30 s");
#endif
    if (afd >= 0)
        close(afd);
    close(cfd);
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * 8u * (9u + 23u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 8u * (9u + 23u),
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
    (void)flow_rx_credit_update(&f);
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (132u + 124u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "down: credit is 6 * 256 shares");
    CHECK(f.rx_credit_bad == 0, "down: credit invariant holds");

    full_block(1, 146, 146, 110);
    CHECK(f.rx_next_block == 2 && f.rx_next_deliver == 146,
          "down: 146/110 full block delivered");
    CHECK(f.rx_n == 146 && f.rx_m == 110,
          "down: current split follows the full block");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "down: credit is 6 * 256 shares after switch");
    CHECK(f.rx_credit_bad == 0, "down: credit invariant holds after switch");
    CHECK(flow_rx_codec(&f, 132, 124) != NULL,
          "down: 132/124 codec builds");
    CHECK(flow_rx_codec(&f, 146, 110) != NULL,
          "down: 146/110 codec builds");
    CHECK(!tbt_fec_block_shape_ok(200, 56, 0, 0, 256, 1, true),
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "16m: credit is 6 * 256 shares after boundary");
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 2) * (146u + 110u)) &&
              f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "ladder: credit is 6 * 256 shares on the ladder");
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
    CHECK(f.rx_decode_ok == 1 && f.rx_decode_fail >= 1,
          "erasure: one successful codec decode, its failures counted");
    for (i = 0; i < 117; i++)
        free(parity[i]);
}

/* The 760K -> HKG0 regression: block 520 of a 64/192 downlink block has
 * real=1. The final 478 bytes ride in DATA[0]; PARITY[0] declares
 * win_len=1 epoch=478 and is the only share that arrives. DATA[1..63] are
 * the sender's zero padding slots: it marks them known zero before the
 * decode, so the receiver must do the same or the block holds 64 unknowns
 * and one equation. */
static void test_short_block_known_zero_padding(void)
{
    struct tbt_fec_grouped *codec;
    u8 *plane;
    u8 *data_ptrs[64];
    u8 *parity_ptrs[192];
    u8 expect[478];
    u16 i;

    clear_engine();
    set_downlink_shape();
    f.rx_shape.n = 64;
    f.rx_shape.m = 192;
    f.rx_n = 64;
    f.rx_m = 192;
    codec = flow_rx_codec(&f, 64, 192);
    CHECK(codec != NULL, "short pad: 64/192 codec builds");
    if (!codec)
        return;
    plane = calloc((size_t)64 + 192, TBT_FEC_S);
    CHECK(plane != NULL, "short pad: share arena");
    if (!plane)
        return;
    for (i = 0; i < 64; i++)
        data_ptrs[i] = plane + (size_t)i * TBT_FEC_S;
    for (i = 0; i < 192; i++)
        parity_ptrs[i] = plane + (size_t)(64 + i) * TBT_FEC_S;
    for (i = 0; i < 478; i++)
        expect[i] = (u8)(i * 7u + 13u);
    memcpy(data_ptrs[0], expect, sizeof(expect));
    CHECK(tbt_fec_grouped_encode(codec, 0, data_ptrs, 64, parity_ptrs) == 0,
          "short pad: encode 64/192");

    send_parity_payload(0, 0, 64, 192, 0, 0, 1, 478, parity_ptrs[0]);

    CHECK(f.rx_decode_ok == 1,
          "short pad: the one covering row recovers DATA[0]");
    CHECK(f.rx_decode_fail == 0, "short pad: no failed decode");
    CHECK(f.rx_next_block == 1, "short pad: block delivered");
    CHECK(f.rx_deliver_bytes == 478,
          "short pad: exactly the epoch's 478 bytes");
    CHECK(f.out_len == 478 && f.out_off == 0 &&
              memcmp(f.out, expect, sizeof(expect)) == 0,
          "short pad: DATA[0] byte-exact with no padding leak");
    free(plane);
}

/* A 128/128 short block with real=16 and two lost DATA shares. DATA[1..14]
 * arrive full; DATA[0] and DATA[15] are erased, and the last real share's
 * 333-byte length comes from PARITY[0]'s epoch. DATA[16..127] are known
 * zero padding. Sixteen parity rows recover both shares. */
static void test_short_block_two_lost_shares(void)
{
    struct tbt_fec_grouped *codec;
    u8 *plane;
    u8 *data_ptrs[128];
    u8 *parity_ptrs[128];
    u8 *expect;
    size_t expect_len = 15u * TBT_FEC_S + 333u;
    u16 i;

    clear_engine();
    set_downlink_shape();
    codec = flow_rx_codec(&f, 128, 128);
    CHECK(codec != NULL, "short two: 128/128 codec builds");
    if (!codec)
        return;
    plane = calloc((size_t)128 + 128, TBT_FEC_S);
    expect = calloc(1, expect_len);
    CHECK(plane != NULL && expect != NULL, "short two: share arena");
    if (!plane || !expect)
    {
        free(plane);
        free(expect);
        return;
    }
    for (i = 0; i < 128; i++)
    {
        data_ptrs[i] = plane + (size_t)i * TBT_FEC_S;
        parity_ptrs[i] = plane + (size_t)(128 + i) * TBT_FEC_S;
    }
    for (i = 0; i < 15; i++)
    {
        memset(data_ptrs[i], 0x30 + i, TBT_FEC_S);
        memcpy(expect + (size_t)i * TBT_FEC_S, data_ptrs[i], TBT_FEC_S);
    }
    memset(data_ptrs[15], 0x80, 333);
    memcpy(expect + 15u * TBT_FEC_S, data_ptrs[15], 333);
    CHECK(tbt_fec_grouped_encode(codec, 0, data_ptrs, 128, parity_ptrs) == 0,
          "short two: encode 128/128");

    for (i = 1; i < 15; i++)
        send_data_payload(0, 0, 128, 128, 0, i, TBT_FEC_S, false,
                          data_ptrs[i]);
    for (i = 0; i < 16; i++)
        send_parity_payload(0, 0, 128, 128, 0, i, 16, 333, parity_ptrs[i]);

    CHECK(f.rx_decode_ok == 1, "short two: the sixteen rows decode the block");
    CHECK(f.rx_next_block == 1, "short two: block delivered");
    CHECK(f.rx_deliver_bytes == expect_len,
          "short two: 15 full shares plus the 333-byte tail");
    CHECK(f.out_len == expect_len && f.out_off == 0 &&
              memcmp(f.out, expect, expect_len) == 0,
          "short two: delivery byte-exact across both recovered shares");
    free(plane);
    free(expect);
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
static int tx_peer_fd = -1;
static int tx_dead_fd = -1;

static void tx_fake_carriers(void)
{
    int sv[2];
    int i;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    tx_peer_fd = sv[0];
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
    if (tx_peer_fd >= 0)
    {
        close(tx_peer_fd);
        tx_peer_fd = -1;
    }
}

/* Decode the records the fake carriers have written since the last drain.
 * The carrier container is the same chunked HTTP framing the engine emits,
 * so the test reads the wire, not an internal counter. */
static int wire_drain(u8 *types, int max)
{
    static struct tbt_http_rx rx;
    static u8 buf[1 << 16];
    ssize_t n;
    size_t off = 0;
    int count = 0;

    if (tx_peer_fd < 0)
        return 0;
    tbt_http_rx_new_message(&rx);
    n = read(tx_peer_fd, buf, sizeof(buf));
    if (n <= 0)
        return 0;
    while (off < (size_t)n && count < max)
    {
        const u8 *rec = NULL;
        size_t rlen = 0;
        int err = TBT_RX_NEED_MORE;
        size_t used = tbt_http_fec_frame_decode(&rx, buf + off,
                                                (size_t)n - off, &rec, &rlen,
                                                &err);

        off += used;
        if (err == TBT_RX_OK && rec)
        {
            types[count++] = rec[0];
            if (!used)
                break;
            continue;
        }
        break;
    }
    return count;
}

/* Decode the HELLO records a fake near has written and report the epoch,
 * type and header path of each. Used by the freshness test, which must see
 * the bytes on the wire rather than an engine counter. */
static int hello_drain(u64 *epochs, u8 *types, u8 *paths, int max)
{
    static struct tbt_http_rx rx;
    static u8 buf[1 << 16];
    ssize_t n;
    size_t off = 0;
    int count = 0;

    if (tx_peer_fd < 0)
        return 0;
    tbt_http_rx_new_message(&rx);
    n = read(tx_peer_fd, buf, sizeof(buf));
    if (n <= 0)
        return 0;
    while (off < (size_t)n && count < max)
    {
        struct tbt_rec rec;
        int err = TBT_RX_NEED_MORE;
        size_t used;

        memset(&rec, 0, sizeof(rec));
        used = tbt_http_frame_decode(&rx, buf + off, (size_t)n - off, &rec,
                                     &err);
        if (err == TBT_RX_OK && rec.hdr.type == TBT_F_HELLO &&
            rec.body_len >= TBT_CRYPTO_HELLO_EPOCH + 8)
        {
            epochs[count] = tbt_le64(rec.body + TBT_CRYPTO_HELLO_EPOCH);
            types[count] = rec.hdr.type;
            paths[count] = rec.hdr.path_id;
            count++;
        }
        else if (err == TBT_RX_PROTO)
            break;
        off += used;
        if (!used)
            break;
    }
    return count;
}

/* One drain, then report which close controls were on the wire. */
static void wire_close_controls(bool *rst, bool *fin)
{
    u8 types[64];
    int i, n = wire_drain(types, (int)(sizeof(types) / sizeof(types[0])));

    *rst = false;
    *fin = false;
    for (i = 0; i < n; i++)
    {
        if (types[i] == TBT_F_RST)
            *rst = true;
        if (types[i] == TBT_F_FIN)
            *fin = true;
    }
}

/* The app fd is a TCP socket in production, and SO_LINGER 0 means RST only
 * there: the same close on an AF_UNIX socketpair is a clean EOF. */
static void app_tcp_pair(int sv[2])
{
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    if (lfd < 0)
        abort();
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 1) ||
        getsockname(lfd, (struct sockaddr *)&a, &alen))
        abort();
    sv[0] = socket(AF_INET, SOCK_STREAM, 0);
    if (sv[0] < 0 || connect(sv[0], (struct sockaddr *)&a, sizeof(a)))
        abort();
    sv[1] = accept(lfd, NULL, NULL);
    if (sv[1] < 0)
        abort();
    close(lfd);
}

/* -2 when the app never woke, otherwise the read result. */
static int app_read_result(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };

    if (poll(&p, 1, 500) <= 0)
        return -2;
    return (int)read(fd, payload, sizeof(payload));
}

/* A socket whose peer is gone: every write fails with EPIPE, exactly like a
 * carrier that died between two records. */
static void tx_kill_carrier(u16 index)
{
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    close(sv[0]);
    tx_dead_fd = sv[1];
    tx_e.car[index]->fd = tx_dead_fd;
}

static void tx_close_dead(void)
{
    if (tx_dead_fd >= 0)
    {
        close(tx_dead_fd);
        tx_dead_fd = -1;
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
    {
        struct flow *tx = flow_alloc(&tx_e, 1, -1, true, NULL);

        /* Every TX test below drives an already-admitted flow: the OPEN was
         * sent and acknowledged, so the application read gate is open. */
        if (tx)
        {
            tx->open_sent = true;
            tx->open_acked = true;
            tx->tx_ready_at_ns = 0;
        }
        return tx;
    }
}

/* A fresh RX flow that matches the TX shape and seed. */
static struct flow *tx_rx_new(struct flow *tx)
{
    struct flow *rx;

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
    return rx;
}

/* Feed every record the TX stored for its blocks into the RX flow. */
static void tx_rx_feed(struct flow *tx, struct flow *rx)
{
    int b;

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
}

static void tx_deliver(struct flow *tx, size_t *bytes, u64 *next)
{
    struct flow *rx = tx_rx_new(tx);

    tx_rx_feed(tx, rx);
    *bytes = rx->rx_deliver_bytes;
    *next = rx->rx_next_block;
    flow_free(&rx_e, rx);
}

/* Deliver and compare the bytes the application would have received. */
static void tx_deliver_check(struct flow *tx, const u8 *expect, size_t len)
{
    struct flow *rx = tx_rx_new(tx);

    tx_rx_feed(tx, rx);
    CHECK(rx->rx_deliver_bytes == len,
          "dead carrier: peer delivers the whole block");
    CHECK(rx->out_len == len &&
              (len == 0 || memcmp(rx->out, expect, len) == 0),
          "dead carrier: delivered bytes match the source");
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
 * converts to seven blocks for both groups=1 and groups=8; a 64-block grant
 * still stops at the replay ring's 63-block span. */
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
    tx->tx_seq = 7 * (u64)tx->groups * tx->n;
    CHECK(flow_tx_credit_ok(tx), "peer7: all seven granted blocks fit");
    tx->tx_seq = 7 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "peer7: beyond the grant stops");

    /* The receiver's 64-block grant cannot exceed the local replay ring:
     * 63 unacknowledged blocks fit, the 64th would wrap it. */
    tx->tx_credit_shares = 64 * block_shares;
    tx->tx_seq = 63 * (u64)tx->groups * tx->n;
    CHECK(flow_tx_credit_ok(tx), "ring: 63 outstanding blocks fit");
    tx->tx_seq = 63 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "ring: the 64th block would wrap");

    /* groups=1 downlink: the same seven-block conversion. */
    tx->groups = 1;
    tx->n = 128;
    tx->m = 128;
    block_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_credit_shares = 7 * block_shares;
    tx->tx_seq = 7 * (u64)tx->groups * tx->n;
    CHECK(tx->tx_credit_shares / block_shares == 7,
          "peer7: groups=1 converts to seven blocks");
    CHECK(flow_tx_credit_ok(tx), "peer7: groups=1 seven blocks fit");
    tx->tx_seq = 7 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "peer7: groups=1 stops at the grant");
    tx->tx_credit_shares = 64 * block_shares;
    tx->tx_seq = 63 * (u64)tx->groups * tx->n;
    CHECK(flow_tx_credit_ok(tx), "ring: groups=1 63 blocks fit");
    tx->tx_seq = 63 * (u64)tx->groups * tx->n + 1;
    CHECK(!flow_tx_credit_ok(tx), "ring: groups=1 stops at 63");
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

    /* A 64-block peer grant is still billed against the 63-block ring. */
    tx->tx_credit_shares = 64 * block_shares;
    tx->tx_ack_next = 0;
    tx->tx_block = TBT_REPLAY_BLOCKS - 2;
    CHECK(flow_tx_block_credit_ok(tx),
          "blocks: 63-block ring accepts the 63rd outstanding block");
    tx->tx_block = TBT_REPLAY_BLOCKS - 1;
    CHECK(!flow_tx_block_credit_ok(tx),
          "blocks: the 64th outstanding block would wrap the ring");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* One carrier dies with a block in flight. Its share handoff is refused and
 * counted, the retry carries the share on a live carrier, the flow survives,
 * and the peer's NACK replays it again from the ring. */
static void test_tx_dead_carrier_share(void)
{
    const size_t nbytes = (size_t)8 * 8 * TBT_FEC_S; /* groups * n shares */
    struct flow *tx = tx_flow_new();
    struct tbt_fec_nack n;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_NACK_LEN];
    u8 *expect = malloc(nbytes);
    size_t i;
    u64 before;

    if (!expect)
        abort();
    for (i = 0; i < nbytes; i++)
        expect[i] = (u8)(0x91 + i * 7u);

    tx_fake_carriers();
    tx_kill_carrier(0);
    CHECK(flow_tx_feed(tx, expect, nbytes) == 0,
          "dead carrier: the feed does not fail the flow");
    CHECK(!tx->dead, "dead carrier: the flow stays alive");
    CHECK(tx_e.tx_carrier_dropped == 1,
          "dead carrier: exactly the refused handoff is counted");
    CHECK(tx_e.car[0]->dead && tx_e.car[0]->tx_records == 0,
          "dead carrier: the written carrier is marked dead");
    CHECK(tx_e.car[1]->tx_records == 2 && tx_e.car[2]->tx_records == 1,
          "dead carrier: the retry rides a live carrier, the rest keep working");
    CHECK(tx->tx_seq == nbytes / TBT_FEC_S && tx->tx_block == 1,
          "dead carrier: the block still advances normally");
    CHECK(tx->replay[0].used && tx->replay[0].lens[0] != 0,
          "dead carrier: the dropped share stays in the replay ring");

    memset(&n, 0, sizeof(n));
    tbt_fec_bitmap_set(n.group_mask, 0);
    tbt_fec_bitmap_set(n.share_mask, 0);
    CHECK(tbt_fec_nack_pack(&n, body) == TBT_FEC_NACK_LEN,
          "dead carrier: NACK packs");
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_NACK;
    h.sid = tx->sid;
    h.block_id = 0;
    h.payload_len = sizeof(body);
    before = tx_e.car[1]->tx_records;
    handle_nack(tx, &h, body);
    CHECK(tx_e.car[1]->tx_records == before + 1,
          "dead carrier: the NACK replay takes a live carrier");
    CHECK(tx_e.car[0]->tx_records == 0,
          "dead carrier: the replay never retries the dead carrier");

    tx_deliver_check(tx, expect, nbytes);

    /* Every carrier down: the share is dropped for the NACK, not fatal. */
    for (i = 0; i < TBT_CAR_MAX; i++)
        tx_e.car[i]->dead = true;
    before = tx_e.tx_carrier_dropped;
    CHECK(flow_tx_feed(tx, expect, TBT_FEC_S) == 0,
          "dead carrier: no usable carrier does not fail the flow");
    CHECK(tx_e.tx_carrier_dropped == before + 1,
          "dead carrier: the unclaimed share waits for a NACK");
    CHECK(!tx->dead, "dead carrier: the flow survives a full carrier outage");

    free(expect);
    flow_free(&tx_e, tx);
    tx_close_dead();
    tx_clear_carriers();
}

/* The application read loop is the path that used to RST and free the flow
 * when one carrier refused its share. */
static void test_tx_dead_carrier_app_read(void)
{
    const size_t nbytes = (size_t)8 * TBT_FEC_S;
    struct flow *tx = tx_flow_new();
    u8 *in = malloc(nbytes);
    int sv[2];
    size_t off = 0;

    if (!in || socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    memset(in, 0x3c, nbytes);
    tx_fake_carriers();
    tx_kill_carrier(0);
    while (off < nbytes)
    {
        ssize_t n = write(sv[1], in + off, nbytes - off);

        if (n <= 0)
            abort();
        off += (size_t)n;
    }
    tx->app_fd = sv[0];
    flow_app_read(tx);
    CHECK(tx_e.flow[0] == tx && !tx->dead,
          "dead carrier: the app read loop keeps the flow");
    CHECK(tx_e.tx_carrier_dropped == 1,
          "dead carrier: the app read loop counts the refusal");
    CHECK(tx->tx_seq == (u64)nbytes / TBT_FEC_S,
          "dead carrier: the app read loop sends every share");

    close(sv[0]);
    close(sv[1]);
    tx->app_fd = -1;
    free(in);
    flow_free(&tx_e, tx);
    tx_close_dead();
    tx_clear_carriers();
}

/* The whole carrier population is unusable for one block: every handoff is
 * counted, the flow survives, and once the carriers are back a NACK replays
 * the block from the ring on its mapped carriers. */
static void test_tx_all_carriers_down(void)
{
    const size_t nbytes = (size_t)8 * 8 * TBT_FEC_S;
    struct flow *tx = tx_flow_new();
    struct tbt_fec_nack n;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_NACK_LEN];
    u8 *expect = malloc(nbytes);
    u64 total = 0;
    size_t i;

    if (!expect)
        abort();
    for (i = 0; i < nbytes; i++)
        expect[i] = (u8)(0x20 + i * 5u);

    tx_fake_carriers();
    for (i = 0; i < TBT_CAR_MAX; i++)
        tx_e.car[i]->dead = true;
    CHECK(flow_tx_feed(tx, expect, nbytes) == 0,
          "all down: the feed does not fail the flow");
    CHECK(!tx->dead, "all down: the flow stays alive");
    CHECK(tx_e.tx_carrier_dropped == (u64)8 * 32,
          "all down: every DATA and PARITY handoff is counted");
    CHECK(tx->replay[0].used, "all down: the block stays in the ring");

    for (i = 0; i < TBT_CAR_MAX; i++)
        tx_e.car[i]->dead = false;
    memset(&n, 0, sizeof(n));
    for (i = 0; i < 8; i++)
        tbt_fec_bitmap_set(n.group_mask, (u16)i);
    for (i = 0; i < 32; i++)
        tbt_fec_bitmap_set(n.share_mask, (u16)i);
    CHECK(tbt_fec_nack_pack(&n, body) == TBT_FEC_NACK_LEN,
          "all down: the NACK packs");
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_NACK;
    h.sid = tx->sid;
    h.block_id = 0;
    h.payload_len = sizeof(body);
    handle_nack(tx, &h, body);
    for (i = 0; i < TBT_CAR_MAX; i++)
        total += tx_e.car[i]->tx_records;
    CHECK(total == (u64)8 * 32,
          "all down: the NACK replays the whole block");
    tx_deliver_check(tx, expect, nbytes);

    free(expect);
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* A control record whose preferred carrier died moves to another carrier;
 * with no carrier at all it waits in the pending array and leaves on the
 * next flush. Carrier faults never drop control records. */
static void test_ctl_dead_carrier(void)
{
    struct flow *tx = tx_flow_new();
    u64 before;
    int i;

    tx_fake_carriers();
    tx_kill_carrier(8); /* flow_send_marker(group 0) prefers carrier 8 */
    before = tx_e.car[0]->tx_records;
    CHECK(flow_send_marker(tx, 0) == 0,
          "control: the marker survives its dead carrier");
    CHECK(tx_e.npending == 0, "control: a live carrier takes the record");
    CHECK(tx_e.car[0]->tx_records == before + 1,
          "control: the record moved to another carrier");
    CHECK(tx_e.tx_carrier_dropped == 0,
          "control: a carrier fault is not a data drop");

    for (i = 0; i < TBT_CAR_MAX; i++)
        tx_e.car[i]->up = false;
    CHECK(flow_send_open(tx, true) == 0,
          "control: OPEN parks when no carrier is up");
    CHECK(tx_e.npending == 1, "control: one record is queued");
    tx_e.car[5]->up = true;
    before = tx_e.car[5]->tx_records;
    pending_flush(&tx_e);
    CHECK(tx_e.npending == 0, "control: the pending flush empties the queue");
    CHECK(tx_e.car[5]->tx_records == before + 1,
          "control: the parked record leaves on the revived carrier");

    flow_free(&tx_e, tx);
    tx_close_dead();
    tx_clear_carriers();
}

/* A received RST ends the stream in both directions: the application must
 * see ECONNRESET at once (not a clean FIN for a transfer that never
 * finished), and the peer learns the stream is gone. */
static void test_rx_rst_aborts_app(void)
{
    struct tbt_hdr h;
    struct carrier c;
    struct flow *tx = tx_flow_new();
    u8 rec[TBT_HDR_LEN];
    int sv[2];
    bool rst, fin;

    app_tcp_pair(sv);
    tx_fake_carriers();
    tx->app_fd = sv[0];
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_RST;
    h.conv = TBT_CONV;
    h.sid_lo = (u32)tx->sid;
    h.sid_hi = (u8)(tx->sid >> 32);
    tbt_raw_put_hdr(rec, &h, 0);
    memset(&c, 0, sizeof(c));
    c.e = &tx_e;
    c.index = 0;
    c.hello_recv = true; /* the record rides a joined carrier */
    CHECK(handle_record(&tx_e, &c, rec, sizeof(rec)) == 0,
          "rx rst: the record is accepted");
    CHECK(tx->dead, "rx rst: the flow is aborted");
    CHECK(tx->app_fd == -1, "rx rst: the app fd is closed");
    CHECK(app_read_result(sv[1]) < 0 && errno == ECONNRESET,
          "rx rst: the app sees ECONNRESET, not a clean EOF");
    wire_close_controls(&rst, &fin);
    CHECK(rst && !fin, "rx rst: the answer is RST, never FIN");
    flow_tick(tx, now_ns());
    CHECK(tx_e.flow[0] == NULL && tx_e.flow_freed == 1,
          "rx rst: the slot is released on the next tick");
    close(sv[1]);
    tx_clear_carriers();
}

/* EPIPE on the app fd (the application stopped reading mid-download) is a
 * full abort: the peer gets RST. A FIN would be stream-ordered and leave the
 * far target open until its close cap, because the aborted upload can never
 * complete. */
static void test_app_epipe_sends_rst(void)
{
    struct flow *tx = tx_flow_new();
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    struct pollfd p;
    u8 data[64];
    int sv[2];
    bool rst, fin;

    app_tcp_pair(sv);
    tx_fake_carriers();
    /* The application is gone: SO_LINGER 0 makes the close a reset, and the
     * RST must be visible before the engine writes or the first write still
     * succeeds into the send buffer. */
    (void)setsockopt(sv[1], SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(sv[1]);
    memset(&p, 0, sizeof(p));
    p.fd = sv[0];
    p.events = POLLIN | POLLOUT;
    (void)poll(&p, 1, 500);
    tx->app_fd = sv[0];
    memset(data, 0x77, sizeof(data));
    CHECK(flow_out_append(tx, data, sizeof(data)) == 0,
          "app epipe: the flush consumes the write error");
    CHECK(tx->dead, "app epipe: the flow is aborted");
    CHECK(tx->app_fd == -1, "app epipe: the app fd is closed");
    CHECK(!tx->fin_sent, "app epipe: a half-close FIN is never sent");
    wire_close_controls(&rst, &fin);
    CHECK(rst && !fin, "app epipe: the peer sees RST and no FIN");
    flow_tick(tx, now_ns());
    CHECK(tx_e.flow[0] == NULL && tx_e.flow_freed == 1,
          "app epipe: the slot is released on the next tick");
    tx_clear_carriers();
}

/* EOF is the legitimate half-close and must keep its FIN: the app may still
 * be reading its downlink. */
static void test_app_eof_sends_fin(void)
{
    struct flow *tx = tx_flow_new();
    int sv[2];
    bool rst, fin;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    tx_fake_carriers();
    tx->app_fd = sv[0];
    if (write(sv[1], payload, 32) != 32)
        abort();
    flow_app_read(tx);
    CHECK(shutdown(sv[1], SHUT_WR) == 0, "app eof: half-close the sender");
    flow_app_read(tx);
    CHECK(tx->app_eof && !tx->dead,
          "app eof: EOF is recorded, not an abort");
    CHECK(!tx->fin_sent, "app eof: FIN waits for the TX gate");
    flow_tick(tx, now_ns());
    CHECK(tx->fin_sent && !tx->dead, "app eof: EOF sends FIN, not an abort");
    CHECK(tx->app_fd == sv[0], "app eof: the fd stays for the downlink");
    wire_close_controls(&rst, &fin);
    CHECK(fin && !rst, "app eof: the wire carries FIN and no RST");
    flow_free(&tx_e, tx);
    close(sv[1]);
    tx_clear_carriers();
}

/* A local encode failure is not a carrier fault: the flow must die. */
static void test_tx_encode_failure(void)
{
    static const u8 key[TBT_CRYPTO_KEY_LEN];
    static const u8 nonce[TBT_CRYPTO_NONCE_LEN];
    struct flow *tx = tx_flow_new();

    tx_fake_carriers();
    memset(payload, 0x5a, sizeof(payload));
    CHECK(tbt_crypto_init(&tx_e.crypto.tx, key, nonce, 0) == 0,
          "encode failure: crypto context");
    tx_e.crypto_on = true;
    tx->tx_blk_seq0 = TBT_CRYPTO_SEQ_MAX + 1;
    CHECK(flow_tx_feed(tx, payload, TBT_FEC_S) == -EIO,
          "encode failure fails the flow feed");
    CHECK(tx_e.tx_carrier_dropped == 0,
          "encode failure is not a carrier drop");
    CHECK(tx_e.car[0]->tx_records == 0 && !tx_e.car[0]->dead,
          "encode failure never reaches a carrier");
    CHECK(tx->tx_seq == 0 && !tx->replay[0].used,
          "encode failure stops before replay and TX accounting");
    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* A replay allocation failure is not a carrier fault either: the flow must
 * die instead of sending a share it cannot repair. */
static void test_tx_replay_alloc_failure(void)
{
    struct flow *tx = tx_flow_new();

    tx_fake_carriers();
    memset(payload, 0x5a, sizeof(payload));
    tx_e.fail_replay_alloc = true;
    CHECK(flow_tx_feed(tx, payload, TBT_FEC_S) == -EIO,
          "replay ENOMEM fails the flow feed");
    CHECK(tx_e.tx_carrier_dropped == 0,
          "replay ENOMEM is not a carrier drop");
    CHECK(tx_e.car[0]->tx_records == 0 && !tx_e.car[0]->dead,
          "replay ENOMEM never touches a carrier");
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

/* A split change must leave the blocks that are already in the reorder
 * window decodable with their own n/m, while the committed credit window
 * follows groups*(n+m) and therefore does not move at all. Eight old-shape
 * blocks fill every slot; the ninth block arrives under the new split. */
static void test_shape_switch_inflight(void)
{
    enum { OLD_N = 139, OLD_M = 117, NEW_N = 160, NEW_M = 96 };
    static const u16 drop[] = { 5, 77, 138 };
    u8 *old_data = malloc((size_t)OLD_N * TBT_FEC_S);
    u8 *new_data = malloc((size_t)NEW_N * TBT_FEC_S);
    u8 *old_parity[OLD_M];
    u8 *new_parity[NEW_M];
    u8 *dp[256], *pp[256];
    struct tbt_fec_grouped *old_codec, *new_codec;
    size_t expected_len =
        (size_t)TBT_RX_SLOTS * OLD_N * TBT_FEC_S + (size_t)NEW_N * TBT_FEC_S;
    u8 *expected = malloc(expected_len);
    u8 dig_e[32], dig_o[32];
    size_t j;
    u16 i, s;

    if (!old_data || !new_data || !expected)
        abort();
    for (j = 0; j < (size_t)OLD_N * TBT_FEC_S; j++)
        old_data[j] = (u8)(j * 7u + 3u);
    for (j = 0; j < (size_t)NEW_N * TBT_FEC_S; j++)
        new_data[j] = (u8)(j * 11u + 5u);

    clear_engine();
    set_downlink_shape();
    old_codec = flow_rx_codec(&f, OLD_N, OLD_M);
    CHECK(old_codec != NULL, "inflight: old-shape codec");
    if (!old_codec)
        return;
    for (i = 0; i < OLD_N; i++)
        dp[i] = old_data + (size_t)i * TBT_FEC_S;
    for (i = 0; i < OLD_M; i++)
    {
        old_parity[i] = calloc(1, TBT_FEC_S);
        pp[i] = old_parity[i];
    }
    CHECK(tbt_fec_grouped_encode(old_codec, 0, dp, OLD_N, pp) == 0,
          "inflight: old-shape parity");

    /* One share per block fills the window under the old split. */
    for (i = 0; i < TBT_RX_SLOTS; i++)
        send_data_payload(i, (u64)i * OLD_N, OLD_N, OLD_M, 0, 0,
                          TBT_FEC_S, false, dp[0]);
    CHECK(f.rx_used == TBT_RX_SLOTS, "inflight: every slot is occupied");

    CHECK(f.rx_n == OLD_N && f.rx_m == OLD_M,
          "inflight: the RX expectation follows the draining block");
    CHECK(f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "inflight: credit is 6 * (n+m), independent of the split");

    /* Complete the eight old blocks with their old n/m. Block 0 loses
     * three DATA shares, so the old codec must still recover it. */
    for (i = 0; i < TBT_RX_SLOTS; i++)
    {
        for (s = 0; s < OLD_N; s++)
        {
            if (i == 0 && (s == drop[0] || s == drop[1] || s == drop[2]))
                continue;
            send_data_payload(i, (u64)i * OLD_N, OLD_N, OLD_M, 0, s,
                              TBT_FEC_S, false, dp[s]);
        }
        if (i == 0)
        {
            for (s = 0; s < OLD_M && f.rx_next_block == i; s++)
                send_parity_payload(i, (u64)i * OLD_N, OLD_N, OLD_M, 0, s,
                                    OLD_N, 0, pp[s]);
        }
        CHECK(f.rx_next_block == (u64)i + 1,
              "inflight: an old-shape block still delivers");
    }
    CHECK(f.rx_n == OLD_N && f.rx_m == OLD_M,
          "inflight: the draining block owns the expectation");
    CHECK(f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 256u,
          "inflight: credit did not move with the old split");

    /* The first new-shape block follows and adopts 160/96. */
    new_codec = flow_rx_codec(&f, NEW_N, NEW_M);
    CHECK(new_codec != NULL, "inflight: new-shape codec");
    for (i = 0; i < NEW_N; i++)
        dp[i] = new_data + (size_t)i * TBT_FEC_S;
    for (i = 0; i < NEW_M; i++)
    {
        new_parity[i] = calloc(1, TBT_FEC_S);
        pp[i] = new_parity[i];
    }
    CHECK(tbt_fec_grouped_encode(new_codec, 0, dp, NEW_N, pp) == 0,
          "inflight: new-shape parity");
    for (s = 0; s < NEW_N; s++)
    {
        if (s == 3 || s == 80 || s == 159)
            continue;
        send_data_payload(TBT_RX_SLOTS, (u64)TBT_RX_SLOTS * OLD_N,
                          NEW_N, NEW_M, 0, s, TBT_FEC_S, false, dp[s]);
    }
    for (s = 0; s < NEW_M && f.rx_next_block == TBT_RX_SLOTS; s++)
        send_parity_payload(TBT_RX_SLOTS, (u64)TBT_RX_SLOTS * OLD_N,
                            NEW_N, NEW_M, 0, s, NEW_N, 0, pp[s]);

    CHECK(f.rx_next_block == TBT_RX_SLOTS + 1,
          "inflight: the new-shape block delivers after the old ones");
    CHECK(f.rx_n == NEW_N && f.rx_m == NEW_M,
          "inflight: the new split owns the expectation at the end");
    CHECK(f.rx_shape_bad == 0 && f.rx_window_drop == 0 &&
              f.rx_late_share == 0,
          "inflight: no old-shape share is rejected or late");
    CHECK(f.rx_credit_bad == 0, "inflight: credit invariant holds");

    for (i = 0; i < TBT_RX_SLOTS; i++)
        memcpy(expected + (size_t)i * OLD_N * TBT_FEC_S, old_data,
               (size_t)OLD_N * TBT_FEC_S);
    memcpy(expected + (size_t)TBT_RX_SLOTS * OLD_N * TBT_FEC_S, new_data,
           (size_t)NEW_N * TBT_FEC_S);
    CHECK(f.out_len == expected_len && f.out_off == 0,
          "inflight: every mixed-split byte is delivered");
    if (f.out_len == expected_len)
    {
        sha_digest(expected, expected_len, dig_e);
        sha_digest(f.out, f.out_len, dig_o);
        CHECK(memcmp(dig_e, dig_o, 32) == 0,
              "inflight: byte-exact SHA across the switch");
    }

    for (i = 0; i < OLD_M; i++)
        free(old_parity[i]);
    for (i = 0; i < NEW_M; i++)
        free(new_parity[i]);
    free(old_data);
    free(new_data);
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

    /* A directed FBK is the CFG handshake, not the control law: it must
     * still leave when adaptive is off, and it must not move the split. */
    clear_pending();
    CHECK(flow_send_fbk(&f, TBT_FEC_FBK_F_CFG_ACK, 1) == 0,
          "adaptive off: directed CFG_ACK still sends");
    CHECK(e.npending == 1, "adaptive off: one directed FBK is queued");
    if (e.npending == 1)
        CHECK(e.pending[0].rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_FLAGS] ==
                  TBT_FEC_FBK_F_CFG_ACK,
              "adaptive off: the directed flags survive");
    CHECK(f.rx_n == 128 && f.rx_m == 128 && f.rx_shape_bad == 0,
          "adaptive off: a directed FBK runs no control law");

    e.cfg.cfg.adaptive = 1;
    f.rx_shape_set = false;
    make_down_cfg(body, 2);
    CHECK(handle_cfg(&f, body) == 0, "adaptive on: CFG installs");
    CHECK(f.ctl_ready, "adaptive on: control law is initialized");

    /* Flip the law off under a live controller: the guard, not the missing
     * init, must keep the periodic FBK and any commit silent. */
    e.cfg.cfg.adaptive = 0;
    clear_pending();
    CHECK(flow_send_fbk(&f, 0, 1) == 0,
          "adaptive off: a live control law still sends no periodic FBK");
    CHECK(e.npending == 0, "adaptive off: the live law queues nothing");
    CHECK(f.rx_n == 128 && f.rx_m == 128 && f.rx_shape_bad == 0,
          "adaptive off: the live law commits nothing");

    /* A periodic report may carry P_VALID. It is not a directed flag, so it
     * must not reopen the control law under adaptive=off. */
    clear_pending();
    CHECK(flow_send_fbk(&f, TBT_FEC_FBK_F_P_VALID, 1) == 0,
          "adaptive off: a P_VALID report stays silent");
    CHECK(e.npending == 0, "adaptive off: P_VALID queues nothing");
    CHECK(f.rx_n == 128 && f.rx_m == 128 && f.rx_shape_bad == 0,
          "adaptive off: P_VALID commits nothing");
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

    clear_engine();
    e.near = false;
    e.cfg.cfg.nonce_set = true;
    memcpy(e.cfg.cfg.nonce, pinned, sizeof(pinned));
    engine_init_session_nonce(&e);
    CHECK(e.session_nonce_set &&
              memcmp(e.session_nonce, pinned, sizeof(pinned)) == 0,
          "nonce: a configured far pins the nonce it requires");
}

/* The HELLO nonce is the 16 bytes at body + 2. A wrong nonce must be
 * refused before hello_crypto() commits the peer key and RX nonce, or the
 * first wrong-nonce connection poisons every later HELLO. */
static void make_hello(u8 body[TBT_HELLO_BODY_LEN_V6], u8 role,
                       const u8 nonce[16], bool crypto,
                       const u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN],
                       const u8 key[TBT_CRYPTO_KEY_LEN])
{
    memset(body, 0, TBT_HELLO_BODY_LEN_V6);
    body[0] = TBT_WIRE_VERSION_V6;
    body[1] = role;
    memcpy(body + 2, nonce, 16);
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);
    body[22] = (u8)(e.cfg.cfg.profile & 3);
    if (crypto)
        tbt_crypto_hello_fill(body, true, TBT_CRYPTO_KEY_ID_HELLO,
                              crypto_nonce, key);
}

static void test_session_nonce_hello(void)
{
    static struct carrier c;
    struct tbt_rec rec;
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 good[16], bad[16], key[TBT_CRYPTO_KEY_LEN];
    u8 cn_a[TBT_CRYPTO_NONCE_LEN], cn_b[TBT_CRYPTO_NONCE_LEN];
    u16 i;

    for (i = 0; i < 16; i++)
    {
        good[i] = (u8)(0x40 + i);
        bad[i] = (u8)(0x90 + i);
    }
    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
        key[i] = (u8)(0x20 + i);
    for (i = 0; i < TBT_CRYPTO_NONCE_LEN; i++)
    {
        cn_a[i] = (u8)(0x60 + i);
        cn_b[i] = (u8)(0x70 + i);
    }

    /* Near: the far's HELLO must echo the local nonce. */
    clear_engine();
    e.near = true;
    e.session_nonce_set = true;
    memcpy(e.session_nonce, good, sizeof(good));
    memset(&c, 0, sizeof(c));
    c.index = 3;
    c.hello_sent = true;
    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = TBT_F_HELLO;
    rec.hdr.path_id = 3;
    rec.body_len = TBT_HELLO_BODY_LEN_V6;
    rec.body = body;

    make_hello(body, TBT_ROLE_FAR, good, false, NULL, NULL);
    CHECK(handle_hello(&e, &c, &rec) == 0,
          "nonce hello: the near accepts its own nonce");
    make_hello(body, TBT_ROLE_FAR, bad, false, NULL, NULL);
    CHECK(handle_hello(&e, &c, &rec) == -EPROTO,
          "nonce hello: the near rejects a different nonce");

    /* Far with crypto on and a pinned nonce: the wrong-nonce HELLO is
     * refused and must leave no key, RX nonce or carrier slot behind. */
    clear_engine();
    e.near = false;
    e.n_car = 1;
    e.crypto_on = true;
    e.session_nonce_set = true;
    memcpy(e.session_nonce, good, sizeof(good));
    memset(&c, 0, sizeof(c));
    c.index = -1;
    c.hello_sent = true;
    rec.hdr.path_id = 0;

    make_hello(body, TBT_ROLE_NEAR, bad, true, cn_a, key);
    CHECK(handle_hello(&e, &c, &rec) == -EPROTO,
          "nonce hello: the far rejects a different nonce");
    CHECK(!e.key_owned && !e.rx_ready,
          "nonce hello: a rejected HELLO commits no crypto state");
    CHECK(e.car[0] == NULL, "nonce hello: a rejected HELLO takes no slot");

    make_hello(body, TBT_ROLE_NEAR, good, true, cn_b, key);
    CHECK(handle_hello(&e, &c, &rec) == 0,
          "nonce hello: the far accepts the pinned nonce");
    CHECK(e.rx_ready && e.key_owned,
          "nonce hello: the accepted HELLO commits crypto state");
    CHECK(e.car[0] == &c && c.index == 0,
          "nonce hello: the accepted HELLO takes its slot");
    CHECK(!memcmp(e.session_nonce, good, sizeof(good)),
          "nonce hello: the accepted HELLO leaves the nonce intact");
}

/* Every accept allocates one carrier. A handshake that never completes must
 * not accumulate them: close detaches, and the tick after the epoll batch
 * reaps. */
static void test_carrier_lifecycle(void)
{
    struct carrier *c;
    int i;

    clear_engine();
    e.near = false;
    for (i = 0; i < 64; i++)
    {
        c = carrier_new(&e, -1, -1, false);
        CHECK(c != NULL, "carrier: accepted allocation");
        if (!c)
            return;
        CHECK(e.car_live == 1, "carrier: one live before the close");
        carrier_close(&e, c);
        carrier_event(&e, c, EPOLLIN); /* a batch event may still name it */
        CHECK(e.car_live == 1, "carrier: close defers the free past the batch");
        engine_tick(&e);
        CHECK(e.car_live == 0 && e.car_list == NULL,
              "carrier: a failed handshake is reaped");
    }
}

/* The real accept path, driven 32 times: every connection is accepted,
 * added to epoll, closed before HELLO, and must leave no carrier behind. */
static void test_far_accept_reap(void)
{
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    int lfd, epfd;
    int i;

    clear_engine();
    e.near = false;
    e.n_car = 1;
    epfd = epoll_create1(EPOLL_CLOEXEC);
    lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(epfd >= 0 && lfd >= 0, "accept: epoll and listener");
    if (epfd < 0 || lfd < 0)
        return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(0x7f000001u);
    sin.sin_port = 0;
    CHECK(bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) == 0,
          "accept: bind an ephemeral port");
    CHECK(listen(lfd, 16) == 0, "accept: listen");
    CHECK(getsockname(lfd, (struct sockaddr *)&sin, &slen) == 0,
          "accept: read the bound port");
    e.epfd = epfd;
    e.listen_fd = lfd;

    for (i = 0; i < 32; i++)
    {
        int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

        CHECK(cfd >= 0, "accept: client socket");
        if (cfd < 0)
            break;
        CHECK(connect(cfd, (struct sockaddr *)&sin, sizeof(sin)) == 0,
              "accept: client connects");
        engine_accept_carrier(&e);
        CHECK(e.car_live == 1, "accept: one carrier admitted");
        close(cfd);
        if (e.car_list)
            carrier_event(&e, e.car_list, EPOLLIN | EPOLLRDHUP);
        CHECK(e.car_live == 1, "accept: close defers the free");
        engine_tick(&e);
        CHECK(e.car_live == 0 && e.car_list == NULL,
              "accept: a failed handshake is reaped");
    }

    e.epfd = -1;
    e.listen_fd = -1;
    close(epfd);
    close(lfd);
    clear_engine();
}

/* A HELLO that names an occupied index replaces the old carrier. The old
 * buffer must be released while the new connection keeps the slot. */
static void test_carrier_replace(void)
{
    struct carrier *old, *newc;
    struct tbt_rec rec;
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 nonce[16];
    u16 i;

    for (i = 0; i < 16; i++)
        nonce[i] = (u8)(0x30 + i);
    clear_engine();
    e.near = false;
    e.n_car = 1;
    e.session_nonce_set = true;
    memcpy(e.session_nonce, nonce, sizeof(nonce));
    old = carrier_new(&e, -1, 0, false);
    newc = carrier_new(&e, -1, -1, false);
    CHECK(old != NULL && newc != NULL, "carrier: replacement allocations");
    if (!old || !newc)
        return;
    e.car[0] = old;
    old->hello_sent = true;
    newc->hello_sent = true;
    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = TBT_F_HELLO;
    rec.hdr.path_id = 0;
    rec.body_len = TBT_HELLO_BODY_LEN_V6;
    rec.body = body;
    make_hello(body, TBT_ROLE_NEAR, nonce, false, NULL, NULL);

    CHECK(handle_hello(&e, newc, &rec) == 0,
          "carrier: the replacement HELLO joins");
    CHECK(e.car[0] == newc && newc->index == 0,
          "carrier: the new connection owns the slot");
    CHECK(old->dead && e.car_live == 2,
          "carrier: the replaced connection is detached, not freed yet");
    engine_tick(&e);
    CHECK(e.car_live == 1,
          "carrier: replacing an index frees the old carrier");
    carrier_close(&e, newc);
    CHECK(e.car[0] == NULL && e.car_live == 1,
          "carrier: close detaches the slot before the free");
    engine_tick(&e);
    CHECK(e.car_live == 0 && e.car[0] == NULL,
          "carrier: closing the replacement drains the far side");
}

/* ---- Peer carrier retirement ----------------------------------------- */

/* One slot with a real socket, the shape engine_start_carriers() leaves on a
 * near side: retained (the slot survives a reconnect), up, HELLO exchanged. */
static struct carrier *slot_with_socket(u16 index, bool retained, bool up)
{
    int sv[2];
    struct carrier *c;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    c = carrier_new(&e, sv[0], (int)index, retained);
    if (!c)
        abort();
    close(sv[1]);
    e.car[index] = c;
    c->hello_sent = true;
    c->hello_recv = true;
    if (up)
    {
        c->up = true;
        e.up_count++;
    }
    return c;
}

/* One RETIRE exactly as the kernel emits it (tbt_carrier.c:2610): type 0x24
 * on the TBT_CONV record, one body byte naming the victim's slot, the peer's
 * 4-bit generation in the header's seq, and the notice riding a different
 * carrier in path_id. */
static size_t retire_record(u8 *rec, u8 victim, u8 gen, u8 path)
{
    struct tbt_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_RETIRE;
    h.path_id = path;
    h.seq = gen & 0xf;
    h.conv = TBT_CONV;
    return tbt_raw_put(rec, &h, &victim, 1);
}

/* A RETIRE names one slot and that slot only. The near must close the named
 * carrier and arm its redial for the next tick: a path the peer has already
 * replaced stays ESTABLISHED for the kernel's retries2 budget, which is how
 * HKG0 V6 kept 256 -> 98 sockets that no side was using. The record used to
 * fall into handle_record()'s default and return -EPROTO, so it closed the
 * healthy carrier that carried it and left the named one untouched. */
static void test_retire_carrier(void)
{
    struct carrier *c0, *c1, *c2;
    u8 rec[TBT_HDR_LEN + 1];
    u64 armed;
    size_t len;

    clear_engine();
    e.near = true;
    e.n_car = 4;
    c0 = slot_with_socket(0, true, true);
    c1 = slot_with_socket(1, true, true);
    c2 = slot_with_socket(2, true, true);
    clear_pending();

    len = retire_record(rec, 1, 7, 2);
    CHECK(rec[0] == TBT_F_RETIRE && tbt_le16(rec + 6) == 1 &&
              tbt_le64(rec + 8) == 7,
          "retire: one body byte, generation in seq");
    CHECK(handle_record(&e, c2, rec, len) == 0,
          "retire: the notice is a record, not a carrier fault");
    CHECK(e.rx_retire == 1 && e.rx_retire_bad == 0,
          "retire: the accepted notice is counted");
    CHECK(c1->dead && c1->fd == -1, "retire: the named carrier is closed");
    CHECK(c1->retry_at_ns <= now_ns(),
          "retire: the redial is armed for the next tick");
    CHECK(e.car[1] == c1 && c1->retained && c1->index == 1,
          "retire: the slot stays for the redial");
    CHECK(e.car_live == 3, "retire: nothing is detached from the engine");
    CHECK(!c0->dead && c0->fd >= 0 && !c2->dead && c2->fd >= 0,
          "retire: the other carriers keep their sockets");
    CHECK(e.up_count == 2, "retire: one carrier left the ready set");
    CHECK(!f.dead && f.rx_next_block == 0 && f.rx_used == 0,
          "retire: no flow state changes");

    /* A notice naming a slot this engine does not have is a peer whose
     * carriers= disagrees. Count it apart and keep the carrier that carried
     * it: the record itself is well formed. */
    len = retire_record(rec, 40, 7, 2);
    CHECK(handle_record(&e, c2, rec, len) == 0,
          "retire: an out-of-range index is not a carrier fault");
    CHECK(e.rx_retire == 1 && e.rx_retire_bad == 1,
          "retire: the out-of-range notice is counted apart");
    CHECK(!c0->dead && !c2->dead, "retire: the notice never hurts its sender");

    /* A duplicate for a slot already down must not restart the backoff its
     * own failing dial earned. */
    armed = now_ns() + 60000000000ull;
    c1->retry_at_ns = armed;
    len = retire_record(rec, 1, 7, 2);
    CHECK(handle_record(&e, c2, rec, len) == 0 && c1->retry_at_ns == armed,
          "retire: a duplicate leaves the armed redial alone");
    CHECK(e.rx_retire == 2, "retire: a duplicate is still a notice");

    /* The far keeps its slot: its HELLO path replaces an occupant, so a
     * replacement joins without this notice, and the near may still be
     * sending on the old socket until it dials. */
    clear_engine();
    e.near = false;
    e.n_car = 2;
    c0 = slot_with_socket(0, false, false);
    len = retire_record(rec, 0, 3, 0);
    CHECK(handle_record(&e, c0, rec, len) == 0,
          "retire: the far accepts the notice");
    CHECK(e.rx_retire == 1 && !c0->dead && c0->fd >= 0,
          "retire: the far keeps the slot for the replacement HELLO");
    clear_engine();
}

/* The parser owns the length and format: 32 hex digits are the full value,
 * 16 pin the low 64 bits (the documented rule), and every other length is
 * refused rather than truncated. */
static void test_session_nonce_parse(void)
{
    static const char *const fmt =
        "tunnel 1 role=near listen=10.0.0.1:1 peer=10.0.0.2:2 nonce=%s";
    static const u8 full[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
                                 0xee, 0xff };
    static const u8 half[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                 0x77 };
    char line[256];
    struct tbt_cmd cmd;
    struct tbt_parse_error perr;

    snprintf(line, sizeof(line), fmt,
             "00112233445566778899aabbccddeeff");
    memset(&cmd, 0, sizeof(cmd));
    memset(&perr, 0, sizeof(perr));
    CHECK(tbt_parse_line(line, &cmd, &perr) == 0 && cmd.cfg.nonce_set &&
              !memcmp(cmd.cfg.nonce, full, sizeof(full)),
          "nonce parse: 32 hex digits are the full value");

    snprintf(line, sizeof(line), fmt, "0011223344556677");
    memset(&cmd, 0, sizeof(cmd));
    CHECK(tbt_parse_line(line, &cmd, &perr) == 0 && cmd.cfg.nonce_set &&
              !memcmp(cmd.cfg.nonce, half, sizeof(half)),
          "nonce parse: 16 hex digits pin the low 64 bits");

    snprintf(line, sizeof(line), fmt, "0011");
    CHECK(tbt_parse_line(line, &cmd, &perr) != 0,
          "nonce parse: a 2-byte value is refused");
    snprintf(line, sizeof(line), fmt,
             "00112233445566778899aabbccddeeff0");
    CHECK(tbt_parse_line(line, &cmd, &perr) != 0,
          "nonce parse: 33 hex digits are refused");
    snprintf(line, sizeof(line), fmt,
             "zz112233445566778899aabbccddeeff");
    CHECK(tbt_parse_line(line, &cmd, &perr) != 0,
          "nonce parse: non-hex digits are refused");
}

/* The stall dump must fire once for an incomplete head and expose both
 * counters: rx_block_stalled and rx_shape_mismatch (a record whose h.n/h.m
 * differs from the split of the last accepted block). */
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

/* The FBK names the accepted block as m_current/n_current and the shipped
 * split as m_target. The receiver keeps following headers. */
static void test_fbk_commit(void)
{
    int i;
    int found = 0;

    clear_engine();
    f.ctl_ready = false;
    e.cfg.cfg.adaptive = 1;
    f.ctl_init_n = 8;
    f.ctl_init_m = 24;
    CHECK(f.rx_n == 8 && f.rx_m == 24,
          "fbk: the shipped split is the accepted split");
    (void)flow_rx_credit_update(&f);
    clear_pending();
    CHECK(flow_send_fbk(&f, 0, 1) == 0, "fbk: send");
    for (i = 0; i < e.npending; i++)
    {
        const u8 *rec = e.pending[i].rec;

        if (rec[0] != TBT_FEC_FBK)
            continue;
        found++;
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_M_CURRENT] == 24,
              "fbk: m_current is the accepted block's m");
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_M_TARGET] == 24,
              "fbk: m_target is the shipped split");
        CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_N_CURRENT] == 8,
              "fbk: n_current is the accepted block's n");
    }
    CHECK(found == 1, "fbk: one report queued");
    CHECK(f.rx_n == 8 && f.rx_m == 24,
          "fbk: the RX expectation stayed with the accepted block");
    CHECK(f.rx_credit_shares == (TBT_RX_SLOTS - 2) * 8u * 32u,
          "fbk: credit stays slots-2 whole blocks");
    CHECK(f.rx_credit_bad == 0, "fbk: credit invariant holds");
}

/* The sender may carry one started block past a slots-2 grant. The ring has
 * two slots of slack over the grant, so a block that started inside the
 * grant finishes inside the ring; the first block outside is refused whole
 * as rx_window_drop. */
static void test_credit_headroom(void)
{
    uint32_t credit;

    clear_engine();
    set_downlink_shape();
    credit = flow_rx_credit_update(&f);
    CHECK(credit == (TBT_RX_SLOTS - 2) * 256u,
          "credit: slots-2 whole blocks");
    CHECK(credit + 256u <= TBT_RX_SLOTS * 256u,
          "credit: the sender's extra block still fits the ring");
    CHECK(flow_send_ack(&f) == 0, "credit: ACK sent");
    CHECK(last_ack_credit() == (int)credit,
          "credit: the ACK carries the same grant");

    /* The first share past the ring is outside at next 0. */
    send_data(0, 0, 128, 128, 0, 0, TBT_FEC_S, false);
    send_data(TBT_RX_SLOTS, (u64)TBT_RX_SLOTS * 128, 128, 128, 0, 0,
              TBT_FEC_S, false);
    CHECK(f.rx_window_drop == 1,
          "credit: block next+slots is outside the ring");
}

/* The controller sees a first-attempt success as zero missing DATA shares,
 * not as the tail still in flight. At 0% loss the wire sends the whole
 * block, yet an early decode leaves ~50 DATA shares in flight; feeding that
 * tail made .92 grow parity from 128 to 150 at 0% loss. */
static void init_adaptive_down(void)
{
    clear_engine();
    set_downlink_shape();
    e.cfg.cfg.adaptive = 1;
    f.ctl_ready = !tbt_fec_adapt_sclc_init(
        &f.ctl, 256, 1, tbt_fec_sclc_wake_extra_down(128), 128,
        TBT_FEC_SCLC_DOWN);
    f.ctl_init_n = 128;
    f.ctl_init_m = 128;
    f.ctl_blocks = f.ctl.blocks;
}

static void feed_ctl_sample(u16 wake_e, u16 wake_s, bool ok, u16 real)
{
    struct rx_group g;
    struct rx_block b;

    memset(&g, 0, sizeof(g));
    memset(&b, 0, sizeof(b));
    g.n = 128;
    g.m = 128;
    g.wake_e = wake_e;
    g.wake_s = wake_s;
    flow_sample_first(&f, &b, &g, ok, real);
}

static u16 ctl_tick_once(void)
{
    struct tbt_fec_adapt_sclc_out out;

    memset(&out, 0, sizeof(out));
    tbt_fec_adapt_sclc_tick(&f.ctl, now_ns(), 1, 1000000, 256, 0, 0, &out);
    return out.m_target;
}

static void test_adaptive_sample_policy(void)
{
    u16 relaxed;
    u16 raised;
    int i;

    init_adaptive_down();
    CHECK(f.ctl_ready, "sample: controller starts");
    /* 48 covered blocks: every first attempt succeeded, so every sample is
     * E=0 even though 60 DATA shares were in flight at the wake line. */
    for (i = 0; i < 48; i++)
        feed_ctl_sample(60, 16, true, 128);
    relaxed = ctl_tick_once();
    CHECK(relaxed <= f.ctl.m_min,
          "sample: 0% loss relaxes parity to the minimum");
    CHECK(f.ctl.q_q16 < (8u << 16),
          "sample: the quantile stayed near zero");

    /* Eight first failures in a row: the safety backup must add parity. */
    for (i = 0; i < 8; i++)
        feed_ctl_sample(60, 16, false, 128);
    raised = ctl_tick_once();
    CHECK(raised >= relaxed, "sample: a failure burst never lowers parity");
    CHECK(f.ctl.q_q16 > (32u << 16),
          "sample: a failure burst raises the quantile");
    CHECK(f.ctl.backup_off >= 2, "sample: the backup added shares");

    /* The wire target never exceeds the shipped split, whatever the law
     * computes: above it the payload shrinks and the wake line moves into
     * the in-flight tail. */
    {
        int found = 0;
        int i2;

        clear_pending();
        CHECK(flow_send_fbk(&f, 0, 1) == 0, "sample: FBK sends");
        for (i2 = 0; i2 < e.npending; i2++)
        {
            const u8 *rec = e.pending[i2].rec;

            if (rec[0] != TBT_FEC_FBK)
                continue;
            found++;
            CHECK(rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_M_TARGET] ==
                      f.ctl_init_m,
                  "sample: m_target is the shipped split, not the law's output");
        }
        CHECK(found == 1, "sample: one FBK queued");
    }
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
    {
        /* The larger window must not allocate its block buffers per flow:
         * every RX slot and replay slot is still untouched, so the 64 MiB
         * of in-flight capacity is paid only by the blocks actually used. */
        bool lazy = true;

        for (i = 0; i < TBT_FLOW_MAX; i++)
        {
            struct flow *g = tx_e.flow[i];
            int k;

            for (k = 0; k < TBT_RX_SLOTS; k++)
                if (g->rx[k].used || g->rx[k].g)
                    lazy = false;
            for (k = 0; k < TBT_REPLAY_BLOCKS; k++)
                if (g->replay[k].used || g->replay[k].recs)
                    lazy = false;
        }
        CHECK(lazy, "table: eight flows hold no block buffers");
    }
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

/* ------------------------------------------------------------------ *
 * Wire v9: the PSK-authenticated HELLO tail.
 * ------------------------------------------------------------------ */

static void mk_hello_path(struct engine *e, u8 *body, u8 role,
                          const u8 session_nonce[16], const u8 tx_nonce[8],
                          u64 epoch, const u8 *psk, u8 path)
{
    memset(body, 0, TBT_HELLO_BODY_LEN_V6);
    body[0] = TBT_WIRE_VERSION_V6;
    body[1] = role;
    memcpy(body + 2, session_nonce, 16);
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);
    body[22] = (u8)(e->cfg.cfg.profile & 3);
    tbt_crypto_hello_fill(body, e->crypto_on, TBT_CRYPTO_KEY_ID_NONE, tx_nonce,
                          NULL);
    tbt_crypto_hello_auth_fill_path(body, TBT_HELLO_BODY_LEN_V6, epoch, psk,
                                    path);
}

static void mk_hello_v9(struct engine *e, u8 *body, u8 role,
                        const u8 session_nonce[16], const u8 tx_nonce[8],
                        u64 epoch, const u8 *psk)
{
    mk_hello_path(e, body, role, session_nonce, tx_nonce, epoch, psk, 0);
}

/* Seal one uplink frame at (sid, seq) under `nonce` and open it with the
 * engine's current RX context. True means that context holds `nonce`. */
static bool rx_opens(struct engine *e, const u8 *nonce, const u8 *plain,
                     u64 sid, u64 seq)
{
    struct tbt_crypto_ctx tx;
    u8 buf[32];
    size_t len = sizeof(buf);

    memcpy(buf, plain, len);
    if (tbt_crypto_init(&tx, e->key, nonce, 0) ||
        tbt_crypto_body_xor_flow(&tx, sid, seq, false, 0, buf, (u16)len))
        return false;
    tbt_crypto_wipe(&tx);
    if (tbt_crypto_body_xor_flow(&e->crypto.rx, sid, seq, false, 0, buf,
                                 (u16)len))
        return false;
    return memcmp(buf, plain, len) == 0;
}

/* A near-side restart must rotate both directions. The uplink (this side's
 * RX) moves to the new HELLO nonce; the downlink (this side's TX) gets a
 * fresh nonce of its own and a later generation so the HELLO that carries it
 * is not treated as stale. A wrong PSK, a replayed HELLO and a tag-less
 * HELLO may not move either, and may not rename the session. */
static void test_hello_reincarnation(void)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 wrong_psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN], n2[TBT_CRYPTO_NONCE_LEN];
    u8 n3[TBT_CRYPTO_NONCE_LEN];
    u8 sess_a[16], sess_b[16];
    u8 plain[32];
    u8 old_tx[TBT_CRYPTO_NONCE_LEN];
    u64 old_epoch;
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
    {
        psk[i] = (u8)(i * 5 + 7);
        wrong_psk[i] = (u8)(psk[i] ^ 0x40);
    }
    for (i = 0; i < sizeof(plain); i++)
        plain[i] = (u8)(0x80 + i);
    memset(sess_a, 0x11, sizeof(sess_a));
    memset(sess_b, 0x22, sizeof(sess_b));
    memset(n1, 0x31, sizeof(n1));
    memset(n2, 0x42, sizeof(n2));
    memset(n3, 0x53, sizeof(n3));

    clear_engine();
    e.near = false; /* the far role receives the near's HELLOs */
    e.crypto_on = true;
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));
    e.rng = 0x123456789abcdefull;

    /* Generation 1: first contact adopts the key, nonce and name. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 1000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0,
          "v9: the first HELLO adopts generation 1");
    CHECK(e.rx_ready && memcmp(e.rx_nonce, n1, sizeof(n1)) == 0,
          "v9: generation 1 nonce installed");
    CHECK(e.peer_epoch == 1000, "v9: generation 1 recorded");
    CHECK(rx_opens(&e, n1, plain, 7, 9),
          "v9: uplink opens under generation 1");
    CHECK(hello_session_nonce(&e, body) == 0 && e.session_nonce_set,
          "v9: the session name is adopted");

    /* Process #2: fresh crypto nonce, later generation, same PSK and name. */
    memcpy(old_tx, e.tx_nonce, sizeof(old_tx));
    old_epoch = e.epoch;
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n2, 2000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0,
          "v9: a restarted near re-incarnates");
    CHECK(memcmp(e.rx_nonce, n2, sizeof(n2)) == 0 && e.peer_epoch == 2000,
          "v9: the uplink nonce and generation rotate");
    CHECK(rx_opens(&e, n2, plain, 7, 9),
          "v9: uplink opens under generation 2");
    CHECK(!rx_opens(&e, n1, plain, 7, 9),
          "v9: the old uplink nonce no longer opens");
    CHECK(memcmp(e.tx_nonce, old_tx, sizeof(old_tx)) != 0,
          "v9: the downlink nonce rotated");
    CHECK(e.epoch > old_epoch, "v9: the downlink generation advanced");
    CHECK(e.tx_rotate, "v9: the far side re-announces its HELLO");
    {
        struct tbt_crypto_ctx rx;
        u8 sealed[32];
        u8 buf[32];

        /* Seal on the engine's real TX context, then open as the peer's
         * fresh RX context keyed by the announced nonce. */
        memcpy(sealed, plain, sizeof(sealed));
        CHECK(tbt_crypto_body_xor_flow(&e.crypto.tx, 7, 9, false, 0, sealed,
                                       sizeof(sealed)) == 0,
              "v9: the far seals on the rotated downlink nonce");
        memcpy(buf, sealed, sizeof(buf));
        CHECK(tbt_crypto_init(&rx, psk, e.tx_nonce, 0) == 0 &&
                  tbt_crypto_body_xor_flow(&rx, 7, 9, false, 0, buf,
                                           sizeof(buf)) == 0 &&
                  memcmp(buf, plain, sizeof(buf)) == 0,
              "v9: downlink opens under the rotated nonce");
        tbt_crypto_wipe(&rx);
    }
    e.tx_rotate = false; /* handle_hello() consumes it */

    /* A replayed generation 1 after generation 2: valid tag, older epoch.
     * The generation high-water rejects it outright, before it can reach the
     * carrier table. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 1000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) != 0 &&
              memcmp(e.rx_nonce, n2, sizeof(n2)) == 0 && e.peer_epoch == 2000,
          "v9: a replayed older generation is refused without state change");
    CHECK(rx_opens(&e, n2, plain, 7, 9),
          "v9: the replay left the RX context intact");

    /* Generation 3 also renames the session (a restart without nonce=). */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_b, n3, 3000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0 &&
              memcmp(e.rx_nonce, n3, sizeof(n3)) == 0,
          "v9: generation 3 installs its nonce");
    CHECK(hello_session_nonce(&e, body) == 0 &&
              memcmp(e.session_nonce, sess_b, sizeof(sess_b)) == 0,
          "v9: an authenticated restart renames the session");
    e.tx_rotate = false;

    /* A stranger that knows the session name but not the PSK: the tag fails,
     * and nothing may move -- nonce, generation, downlink nonce or name. */
    memcpy(old_tx, e.tx_nonce, sizeof(old_tx));
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 4000, wrong_psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) != 0,
          "v9: a wrong PSK is refused");
    CHECK(hello_session_nonce(&e, body) != 0,
          "v9: a wrong PSK cannot rename the session");
    CHECK(memcmp(e.rx_nonce, n3, sizeof(n3)) == 0 && e.peer_epoch == 3000 &&
              memcmp(e.tx_nonce, old_tx, sizeof(old_tx)) == 0,
          "v9: the wrong-PSK refusal left zero state");
    CHECK(rx_opens(&e, n3, plain, 7, 9),
          "v9: the wrong-PSK refusal did not poison the RX context");

    /* No PSK at the sender: the tail carries a zero tag, which proves
     * nothing and may not rotate anything. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 5000, NULL);
    CHECK(hello_crypto(&e, body, sizeof(body)) != 0,
          "v9: an unauthenticated nonce change is refused");
    CHECK(hello_session_nonce(&e, body) != 0 &&
              memcmp(e.session_nonce, sess_b, sizeof(sess_b)) == 0,
          "v9: an unauthenticated HELLO cannot rename the session");
    CHECK(memcmp(e.rx_nonce, n3, sizeof(n3)) == 0 && e.peer_epoch == 3000,
          "v9: the unauthenticated HELLO left zero state");

    /* Another carrier of the same generation repeats the nonce: a no-op. */
    memcpy(old_tx, e.tx_nonce, sizeof(old_tx));
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_b, n3, 3000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0 &&
              memcmp(e.tx_nonce, old_tx, sizeof(old_tx)) == 0 && !e.tx_rotate,
          "v9: a repeated carrier HELLO does not rotate");
}

/* crypto=off keeps the PSK tag but has no crypto nonce to compare, so the
 * generation high-water is the only replay defence. It must be recorded even
 * then, and a configured nonce= must stay absolute. */
static void test_hello_reincarnation_off(void)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN], n2[TBT_CRYPTO_NONCE_LEN];
    u8 sess_a[16], sess_b[16];
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
        psk[i] = (u8)(i * 9 + 3);
    memset(sess_a, 0x11, sizeof(sess_a));
    memset(sess_b, 0x22, sizeof(sess_b));
    memset(n1, 0x31, sizeof(n1));
    memset(n2, 0x42, sizeof(n2));

    clear_engine();
    e.near = false;
    e.crypto_on = false; /* cipher off, PSK still configured */
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));

    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 2000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0,
          "v9-off: the first HELLO is accepted with the cipher off");
    CHECK(e.peer_epoch_set && e.peer_epoch == 2000,
          "v9-off: the generation is recorded with the cipher off");
    CHECK(hello_session_nonce(&e, body) == 0,
          "v9-off: the session is adopted");

    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_b, n2, 3000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0,
          "v9-off: a later generation is accepted");
    CHECK(hello_session_nonce(&e, body) == 0 &&
              memcmp(e.session_nonce, sess_b, sizeof(sess_b)) == 0,
          "v9-off: a later generation renames the session");

    /* A replayed generation 2 carries a valid tag but an older epoch. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 2000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) != 0,
          "v9-off: a replayed older generation is refused");
    CHECK(hello_session_nonce(&e, body) != 0 &&
              memcmp(e.session_nonce, sess_b, sizeof(sess_b)) == 0,
          "v9-off: a replayed HELLO cannot rename the session");
    CHECK(e.peer_epoch == 3000, "v9-off: a replay cannot lower the generation");

    /* A configured nonce= is absolute even for an authenticated restart. */
    clear_engine();
    e.near = false;
    e.crypto_on = true;
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));
    e.cfg.cfg.nonce_set = true;
    memcpy(e.session_nonce, sess_a, sizeof(sess_a));
    e.session_nonce_set = true;
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_b, n1, 2000, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0,
          "v9: an authenticated HELLO passes with a pinned nonce");
    CHECK(hello_session_nonce(&e, body) != 0 &&
              memcmp(e.session_nonce, sess_a, sizeof(sess_a)) == 0,
          "v9: a pinned nonce= is never renamed");
}

/* The far's admission gate must let an authenticated newer generation
 * through even when it names a new session nonce (a restarted near without
 * nonce=), while a replayed older generation is refused before
 * hello_crypto() touches the live carrier's slot or crypto state. */
static void test_far_psk_reincarnation(void)
{
    static struct carrier c;
    static struct carrier c2;
    struct tbt_rec rec;
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN], n2[TBT_CRYPTO_NONCE_LEN];
    u8 sess_a[16], sess_b[16];
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
        psk[i] = (u8)(i * 3 + 11);
    memset(n1, 0x31, sizeof(n1));
    memset(n2, 0x42, sizeof(n2));
    memset(sess_a, 0x11, sizeof(sess_a));
    memset(sess_b, 0x22, sizeof(sess_b));

    clear_engine();
    e.near = false;
    e.n_car = 1;
    e.crypto_on = false; /* the PSK tag alone decides the generation */
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));
    memcpy(e.session_nonce, sess_a, sizeof(sess_a));
    e.session_nonce_set = true;
    e.peer_epoch = 1000;
    e.peer_epoch_set = true;

    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = TBT_F_HELLO;
    rec.hdr.path_id = 0;
    rec.body_len = TBT_HELLO_BODY_LEN_V6;
    rec.body = body;
    memset(&c, 0, sizeof(c));
    c.e = &e;
    c.index = -1;
    c.hello_sent = true; /* this test does not drive the wire */

    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_b, n2, 2000, psk);
    CHECK(handle_hello(&e, &c, &rec) == 0,
          "far: an authenticated newer generation is accepted");
    CHECK(e.session_nonce_set && !memcmp(e.session_nonce, sess_b,
                                         sizeof(sess_b)),
          "far: the restarted near renames the session nonce");
    CHECK(e.car[0] == &c && c.index == 0 && c.hello_recv,
          "far: the accepted HELLO takes the slot");

    /* A replayed generation 1 on a fresh connection: valid tag, older epoch,
     * and a nonce that no longer matches. Drop the whole connection; the
     * live carrier and slot stay untouched. */
    memset(&c2, 0, sizeof(c2));
    c2.e = &e;
    c2.index = -1;
    c2.hello_sent = true;
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess_a, n1, 500, psk);
    CHECK(handle_hello(&e, &c2, &rec) == -EPROTO,
          "far: a replayed older generation is refused");
    CHECK(e.car[0] == &c && c2.index == -1 && !c2.hello_recv,
          "far: the replay cannot replace or take a slot");
    CHECK(e.peer_epoch == 2000,
          "far: the replay cannot lower the generation high-water");
}

/* Version 11: the far keeps a per-path high-water of authenticated HELLO
 * epochs. An equal or older epoch on the same path is a replay even though
 * its tag is valid; a strictly newer epoch replaces a live carrier; a
 * non-PSK HELLO has no authenticated epoch and keeps the old replacement
 * rule. The epoch of a rejected HELLO must not raise the high-water. */
static void test_hello_epoch_path_replay(void)
{
    static struct carrier c1, c2, c3, c4;
    struct tbt_rec rec;
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN], n2[TBT_CRYPTO_NONCE_LEN];
    u8 sess[16];
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
        psk[i] = (u8)(i * 7 + 5);
    memset(n1, 0x31, sizeof(n1));
    memset(n2, 0x42, sizeof(n2));
    memset(sess, 0x55, sizeof(sess));

    clear_engine();
    e.near = false;
    e.n_car = 2;
    e.crypto_on = false; /* the tag is the only epoch authority */
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));
    memcpy(e.session_nonce, sess, sizeof(sess));
    e.session_nonce_set = true;

    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = TBT_F_HELLO;
    rec.hdr.path_id = 0;
    rec.body_len = TBT_HELLO_BODY_LEN_V6;
    rec.body = body;

    /* Generation 1000 joins path 0. */
    memset(&c1, 0, sizeof(c1));
    c1.e = &e;
    c1.index = -1;
    c1.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n1, 1000, psk, 0);
    CHECK(handle_hello(&e, &c1, &rec) == 0,
          "path epoch: the first authenticated HELLO joins");
    CHECK(e.car[0] == &c1 && c1.index == 0 && c1.hello_epoch == 1000,
          "path epoch: the joined carrier names its epoch");
    CHECK(e.peer_hello_epoch_valid[0] && e.peer_hello_epoch[0] == 1000,
          "path epoch: the high-water is published after the join");

    /* (a) An exact replay on the same path, with a valid tag and the same
     * session nonce, is refused before it can touch the live carrier. */
    memset(&c2, 0, sizeof(c2));
    c2.e = &e;
    c2.index = -1;
    c2.hello_sent = true;
    CHECK(handle_hello(&e, &c2, &rec) == -EPROTO,
          "path epoch: an equal-epoch replay is refused");
    CHECK(e.car[0] == &c1 && !c1.dead && c2.index == -1 &&
              !c2.hello_recv && e.peer_hello_epoch[0] == 1000,
          "path epoch: the replay leaves the live carrier and high-water");

    /* An older epoch is the other half of the replay rule. */
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n1, 900, psk, 0);
    CHECK(handle_hello(&e, &c2, &rec) == -EPROTO &&
              e.car[0] == &c1 && e.peer_hello_epoch[0] == 1000,
          "path epoch: an older epoch cannot replace the live carrier");

    /* (b) A legitimate reconnect with a strictly newer epoch does replace
     * the live carrier. */
    memset(&c3, 0, sizeof(c3));
    c3.e = &e;
    c3.index = -1;
    c3.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n2, 2000, psk, 0);
    CHECK(handle_hello(&e, &c3, &rec) == 0,
          "path epoch: a newer epoch replaces a live carrier");
    CHECK(e.car[0] == &c3 && c3.index == 0 && c1.dead &&
              c3.hello_epoch == 2000 && e.peer_hello_epoch[0] == 2000,
          "path epoch: the replacement owns the slot and raises the mark");

    /* A slot that is already dead still refuses its own duplicate epoch:
     * the path high-water, not the slot occupant, is the replay authority. */
    carrier_close(&e, &c3);
    memset(&c4, 0, sizeof(c4));
    c4.e = &e;
    c4.index = -1;
    c4.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n2, 2000, psk, 0);
    CHECK(handle_hello(&e, &c4, &rec) == -EPROTO,
          "path epoch: an equal epoch cannot refill a dead slot");
    CHECK(e.car[0] == NULL && e.peer_hello_epoch[0] == 2000,
          "path epoch: the refused refill leaves the slot empty");

    /* Another path has its own high-water, and a carrier HELLO may arrive
     * after a higher epoch from a different path: with the cipher off the
     * shared session nonce proves it is the same incarnation, so a lower
     * epoch is not a replay and path 1's own high-water decides. */
    rec.hdr.path_id = 1;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n2, 1500, psk, 1);
    memset(&c4, 0, sizeof(c4));
    c4.e = &e;
    c4.index = -1;
    c4.hello_sent = true;
    CHECK(handle_hello(&e, &c4, &rec) == 0,
          "path epoch: a reordered same-session epoch is accepted");
    CHECK(e.car[1] == &c4 && e.peer_hello_epoch_valid[1] &&
              e.peer_hello_epoch[1] == 1500 &&
              e.peer_hello_epoch[0] == 2000 && e.peer_epoch == 2000,
          "path epoch: path marks stay independent under reordering");

    /* Non-PSK keeps TOFU: no authenticated epoch exists, so an equal-epoch
     * replacement of a live carrier is still admitted. */
    clear_engine();
    e.near = false;
    e.n_car = 1;
    e.crypto_on = false;
    memcpy(e.session_nonce, sess, sizeof(sess));
    e.session_nonce_set = true;
    rec.hdr.path_id = 0;
    memset(&c1, 0, sizeof(c1));
    c1.e = &e;
    c1.index = -1;
    c1.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n1, 1000, NULL, 0);
    CHECK(handle_hello(&e, &c1, &rec) == 0 && e.car[0] == &c1,
          "path epoch: a non-PSK HELLO keeps TOFU");
    memset(&c2, 0, sizeof(c2));
    c2.e = &e;
    c2.index = -1;
    c2.hello_sent = true;
    CHECK(handle_hello(&e, &c2, &rec) == 0 &&
              e.car[0] == &c2 && c1.dead,
          "path epoch: non-PSK equal epochs still replace the slot");
    CHECK(!e.peer_hello_epoch_valid[0],
          "path epoch: a non-PSK HELLO publishes no authenticated mark");
}

/* With the cipher on, the matching crypto nonce identifies a carrier of the
 * same incarnation. A later carrier whose HELLO was overtaken by a higher
 * epoch on another path must still join; only a changed nonce needs a
 * strictly newer epoch. */
static void test_hello_epoch_same_nonce_reorder(void)
{
    static struct carrier c1, c2;
    struct tbt_rec rec;
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN];
    u8 sess[16];
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
        psk[i] = (u8)(i * 5 + 9);
    memset(n1, 0x6a, sizeof(n1));
    memset(sess, 0x77, sizeof(sess));

    clear_engine();
    e.near = false;
    e.n_car = 2;
    e.crypto_on = true;
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));
    memcpy(e.session_nonce, sess, sizeof(sess));
    e.session_nonce_set = true;

    memset(&rec, 0, sizeof(rec));
    rec.hdr.type = TBT_F_HELLO;
    rec.hdr.path_id = 0;
    rec.body_len = TBT_HELLO_BODY_LEN_V6;
    rec.body = body;

    memset(&c1, 0, sizeof(c1));
    c1.e = &e;
    c1.index = -1;
    c1.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n1, 2000, psk, 0);
    CHECK(handle_hello(&e, &c1, &rec) == 0,
          "nonce reorder: the first carrier installs the crypto nonce");
    CHECK(e.rx_ready && !memcmp(e.rx_nonce, n1, sizeof(n1)) &&
              e.peer_epoch == 2000,
          "nonce reorder: the session is pinned to generation 2000");

    rec.hdr.path_id = 1;
    memset(&c2, 0, sizeof(c2));
    c2.e = &e;
    c2.index = -1;
    c2.hello_sent = true;
    mk_hello_path(&e, body, TBT_ROLE_NEAR, sess, n1, 1500, psk, 1);
    CHECK(handle_hello(&e, &c2, &rec) == 0,
          "nonce reorder: a matching nonce admits the older path HELLO");
    CHECK(e.car[1] == &c2 && e.peer_hello_epoch_valid[1] &&
              e.peer_hello_epoch[1] == 1500 && e.peer_epoch == 2000 &&
              !memcmp(e.rx_nonce, n1, sizeof(n1)),
          "nonce reorder: the join moves no nonce and keeps the high-water");

    /* The same lower epoch on path 1 again is still a replay. */
    rec.hdr.path_id = 1;
    memset(&c2, 0, sizeof(c2));
    c2.e = &e;
    c2.index = -1;
    c2.hello_sent = true;
    CHECK(handle_hello(&e, &c2, &rec) == -EPROTO,
          "nonce reorder: the path high-water still refuses an equal epoch");
}

/* Every carrier HELLO names a fresh epoch, even when the crypto generation
 * did not move: the far's per-path high-water would otherwise refuse the
 * second carrier of one connection. */
static void test_hello_epoch_fresh_per_carrier(void)
{
    u64 epochs[3];
    u8 types[3];
    u8 want_path[3];
    int n, i;

    CHECK(TBT_REPLAY_BLOCKS == 64 && TBT_RX_SLOTS == 66,
          "window: replay ring 64, receive ring 66");

    clear_engine();
    memset(&tx_e, 0, sizeof(tx_e));
    tx_e.near = true;
    tx_e.crypto_on = false;
    tx_e.epoch = 5000; /* the nonce clock, which must not be reused here */
    tx_e.hello_epoch = tx_e.epoch;
    tx_fake_carriers();

    CHECK(carrier_send_hello(&tx_e, tx_e.car[0]) == 0 &&
              carrier_send_hello(&tx_e, tx_e.car[0]) == 0 &&
              carrier_send_hello(&tx_e, tx_e.car[1]) == 0,
          "hello epoch: three carrier HELLOs are written");
    n = hello_drain(epochs, types, want_path, 3);
    CHECK(n == 3, "hello epoch: all three decode from the wire");
    for (i = 0; i < n && i < 3; i++)
    {
        CHECK(types[i] == TBT_F_HELLO, "hello epoch: the record is HELLO");
        CHECK(epochs[i] > tx_e.epoch && epochs[i] < TBT_EPOCH_LIMIT,
              "hello epoch: the wire epoch is a fresh valid generation");
        if (i)
            CHECK(epochs[i] > epochs[i - 1],
                  "hello epoch: each HELLO strictly advances");
    }
    CHECK(n < 2 || want_path[0] == 0, "hello epoch: first carrier path 0");
    CHECK(n < 3 || (want_path[1] == 0 && want_path[2] == 1),
          "hello epoch: the path follows the carrier index");

    tx_clear_carriers();
}

/* A restarted client must not open with the retired fixed sid 1: the far
 * still holds the previous incarnation's sid in a tombstone for
 * TBT_TOMBSTONE_MS and would drop that OPEN. Two processes with different
 * CSPRNG seeds must also start at different 40-bit sids. */
static void test_sid_incarnation_seed(void)
{
    struct engine a;
    struct engine b;
    u64 sa, sb, next;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.epoch = 1000000;
    a.rng = 0x0123456789abcdefull;
    b.epoch = 1000001;
    b.rng = 0xfedcba9876543210ull;
    engine_sid_init(&a);
    engine_sid_init(&b);
    sa = engine_sid_next(&a);
    sb = engine_sid_next(&b);

    CHECK(sa != 0 && sa != 1,
          "sid: first allocation is neither the sentinel nor 1");
    CHECK(sb != 0 && sb != 1,
          "sid: a second incarnation also avoids 0 and 1");
    CHECK(sa != sb, "sid: two seeds start at different sids");
    CHECK((sa >> 40) == 0 && (sb >> 40) == 0,
          "sid: both fit the 40-bit wire field");

    next = engine_sid_next(&a);
    CHECK(next != sa && (next >> 40) == 0,
          "sid: the counter advances inside 40 bits");
}

/* A broken realtime clock must not name a generation the far side records
 * as an all-time high-water: a pre-1970 tv_sec cast to u64 is just under
 * 2^64, and every later restart (after the clock is repaired) would then be
 * refused as an older generation. A rotation must also keep moving after an
 * NTP step back, and saturation must never wrap the generation to 0. */
static void test_epoch_clock_clamp(void)
{
    struct timespec ts;
    /* Volatile inputs keep the constant folder from proving every CHECK at
     * build time and dropping the runtime assertions. */
    volatile time_t bad = -1;
    volatile time_t far_ahead = (time_t)(TBT_EPOCH_MAX_SECS + 1);
    volatile time_t past_limit = 9223372037; /* us conversion crosses 2^63/1000 */
    volatile time_t last_sec = 9223372036;
    volatile time_t good = 1700000000;
    volatile u64 lo = 100;
    volatile u64 hi = 200;
    volatile u64 last = TBT_EPOCH_LIMIT - 1;

    memset(&ts, 0, sizeof(ts));
    ts.tv_sec = bad;
    ts.tv_nsec = 500000000;
    CHECK(epoch_from_realtime(ts) == 0,
          "epoch: a pre-1970 sample names no generation");
    ts.tv_sec = far_ahead;
    ts.tv_nsec = 0;
    CHECK(epoch_from_realtime(ts) == 0,
          "epoch: a sample whose us conversion would wrap names none");
    ts.tv_sec = past_limit;
    ts.tv_nsec = 0;
    CHECK(epoch_from_realtime(ts) == 0,
          "epoch: a sample at or above the signed-ns limit names none");
    ts.tv_sec = last_sec;
    ts.tv_nsec = 854774999;
    CHECK(epoch_from_realtime(ts) == TBT_EPOCH_LIMIT - 1,
          "epoch: the last representable microsecond is still emitted");
    ts.tv_sec = good;
    ts.tv_nsec = 250000000;
    CHECK(epoch_from_realtime(ts) == 1700000000ull * 1000000ull + 250000ull,
          "epoch: a sane sample converts to microseconds");

    CHECK(epoch_advance(lo, hi) == 200,
          "epoch: a later realtime sample advances");
    CHECK(epoch_advance(hi, lo) == 201,
          "epoch: a backwards step still advances");
    CHECK(epoch_advance(hi, 0) == 201,
          "epoch: an unusable sample still advances");
    CHECK(epoch_advance(last, 0) == last,
          "epoch: saturation never wraps to 0 or crosses the limit");
    CHECK(epoch_now() != 0, "epoch: the live clock always names a generation");
    CHECK(epoch_now() < TBT_EPOCH_LIMIT,
          "epoch: the live clock stays below the receiver limit");
}

/* A peer whose realtime clock is pre-1970 advertises a signed-to-unsigned
 * cast as its HELLO epoch: an old client casts seconds (~2^64), an old
 * kernel casts nanoseconds before dividing, landing in [2^63/1000,
 * 2^64/1000) with the TOP BIT CLEAR. The client records peer epochs as a
 * generation high-water, so accepting either class would refuse every later
 * restart of that peer, even after its clock is repaired. Refuse the HELLO
 * before it can move any state; the last representable generation still
 * works, and the bound itself is refused. */
static void test_hello_epoch_limit(void)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 psk[TBT_CRYPTO_KEY_LEN];
    u8 n1[TBT_CRYPTO_NONCE_LEN], n2[TBT_CRYPTO_NONCE_LEN];
    u8 sess[16];
    size_t i;

    for (i = 0; i < sizeof(psk); i++)
        psk[i] = (u8)(i * 7 + 5);
    memset(sess, 0x33, sizeof(sess));
    memset(n1, 0x44, sizeof(n1));
    memset(n2, 0x55, sizeof(n2));

    clear_engine();
    e.near = false;
    e.crypto_on = false; /* the high-water exists with the cipher off */
    e.psk = true;
    e.key_owned = true;
    e.key_id = TBT_CRYPTO_KEY_ID_NONE;
    memcpy(e.key, psk, sizeof(psk));

    /* 1969-12-31 through the old kernel's ns-then-divide cast: bit 63 is
     * clear, so a top-bit-only guard would miss it. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess, n1, 18446744072709551ull,
                psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == -EPROTO,
          "epoch: the old kernel ns-cast generation is refused");
    CHECK(!e.peer_epoch_set,
          "epoch: the ns-cast refusal left no high-water");

    /* 1969 through the old client's seconds-then-scale cast (~2^64). */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess, n1, 18446744073708551616ull,
                psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == -EPROTO,
          "epoch: the old client seconds-cast generation is refused");

    /* The last representable microsecond is accepted, which also proves the
     * refused values above did not poison the high-water. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess, n2, TBT_EPOCH_LIMIT - 1, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == 0 &&
              e.peer_epoch == TBT_EPOCH_LIMIT - 1,
          "epoch: the last representable generation is accepted");

    /* The bound itself is newer, so the older-generation check cannot be
     * what refuses it; the unsigned-cast guard must. */
    mk_hello_v9(&e, body, TBT_ROLE_NEAR, sess, n1, TBT_EPOCH_LIMIT, psk);
    CHECK(hello_crypto(&e, body, sizeof(body)) == -EPROTO &&
              e.peer_epoch == TBT_EPOCH_LIMIT - 1,
          "epoch: the bound itself is refused without moving the high-water");
}

/* The far side answers the near's HTTP request with a Date built from
 * time(NULL). A pre-1970 clock makes that negative, and its u64 cast is a
 * year past 9999 that the renderer rejects: the header build returned 0,
 * carrier_send_http_header() returned -EINVAL and the carrier was closed on
 * the first request. Clamping keeps a plausible, renderable Date. */
static void test_http_date_clamp(void)
{
    char hdr[TBT_HTTP_RESP_MAX];
    struct tbt_http_resp_cfg rc;

    rc.unix_secs = http_date_secs((time_t)-1);
    CHECK(rc.unix_secs == 0 &&
              tbt_http_resp_header(hdr, sizeof(hdr), &rc) != 0,
          "http: a pre-1970 clock still renders a Date");
    rc.unix_secs = http_date_secs((time_t)(TBT_CLIENT_HTTP_DATE_MAX + 1));
    CHECK(rc.unix_secs == TBT_CLIENT_HTTP_DATE_MAX &&
              tbt_http_resp_header(hdr, sizeof(hdr), &rc) != 0,
          "http: a clock past 9999 still renders a Date");
    rc.unix_secs = http_date_secs((time_t)1700000000);
    CHECK(rc.unix_secs == 1700000000ull &&
              tbt_http_resp_header(hdr, sizeof(hdr), &rc) != 0,
          "http: a sane clock passes through");
}

/* An app socket that stays readable behind an exhausted TX window must leave
 * the poll set: EPOLLIN is level-triggered, so leaving it armed makes
 * epoll_wait return on every loop iteration (a core at 100%). flow_tick()
 * must restore it when the peer's ACK opens the window, or the tail waits
 * forever. */
static void test_app_credit_poll_cycle(void)
{
    struct flow *tx = tx_flow_new();
    struct epoll_event ev;
    int sv[2];
    int epfd;
    char ch = 'x';
    u64 block_pkts = (u64)tx->groups * tx->n;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    epfd = epoll_create1(EPOLL_CLOEXEC);
    CHECK(epfd >= 0, "credit: epoll instance");
    if (epfd < 0)
        return;
    if (write(sv[1], &ch, 1) != 1)
        abort();
    tx_e.epfd = epfd;
    tx->app_fd = sv[0];
    /* The one-block grant is fully outstanding: credit_ok is still true at
     * the edge, but no further packet may be read. */
    tx->tx_credit_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_seq = block_pkts;
    tx->tx_ack_next = 0;
    flow_app_poll_set(tx, EPOLLIN | EPOLLRDHUP);
    CHECK(tx->app_armed && epoll_wait(epfd, &ev, 1, 0) == 1,
          "credit: a readable fd is armed first");

    flow_app_read(tx);
    CHECK(!tx->app_armed, "credit: an exhausted window disarms the fd");
    CHECK(epoll_wait(epfd, &ev, 1, 0) == 0,
          "credit: no level-triggered wakeup remains");
    CHECK(tx->tx_cur_len == 0 && tx->tx_seq == block_pkts,
          "credit: no byte was read past the window");

    /* The peer grants the full window: the next tick must re-arm the fd. */
    tx->tx_credit_shares = 7 * (u64)tx->groups * (tx->n + tx->m);
    flow_tick(tx, now_ns());
    CHECK(tx->app_armed, "credit: the tick re-arms after the ACK");
    CHECK(epoll_wait(epfd, &ev, 1, 0) == 1,
          "credit: the readable fd wakes again (no lost wakeup)");
    flow_app_read(tx);
    CHECK(tx->tx_cur_len == 1, "credit: the granted window consumes the byte");

    flow_free(&tx_e, tx);
    close(sv[0]);
    close(sv[1]);
    close(epfd);
}

/* A peer that closes with bytes still unread sets EPOLLHUP. That is not an
 * error: the credit gate may have deferred the read, and resetting the app
 * here dropped the tail (the loopback ECONNRESET flake). EOF itself needs no
 * credit, so a zero-length peek finalizes a drained socket immediately. */
static void test_app_hup_keeps_unread_tail(void)
{
    struct flow *tx = tx_flow_new();
    u8 data[TBT_FEC_S];
    int sv[2];
    u64 block_pkts = (u64)tx->groups * tx->n;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    memset(data, 0x5c, sizeof(data));
    if (write(sv[1], data, sizeof(data)) != (ssize_t)sizeof(data))
        abort();
    (void)shutdown(sv[1], SHUT_WR);
    tx->app_fd = sv[0];
    tx->tx_credit_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_seq = block_pkts; /* window fully spent */
    tx->tx_ack_next = 0;
    tx_e.epfd = -1;

    flow_target_event(tx, EPOLLIN | EPOLLRDHUP | EPOLLHUP);
    CHECK(!tx->dead, "hup: an unread tail is not a reset");
    CHECK(!tx->app_eof, "hup: the EOF stays behind the pending bytes");
    CHECK(!tx->app_armed, "hup: the stalled fd leaves the poll set");
    CHECK(tx->tx_cur_len == 0 && tx->tx_seq == block_pkts,
          "hup: the stalled fd consumes nothing");

    /* Credit returns: drain the share, observe EOF, then finish and send. */
    tx->tx_credit_shares = 7 * (u64)tx->groups * (tx->n + tx->m);
    flow_tick(tx, now_ns());
    flow_target_event(tx, EPOLLIN | EPOLLRDHUP | EPOLLHUP);
    CHECK(tx->tx_bytes == TBT_FEC_S, "hup: the tail is delivered");
    CHECK(tx->app_eof && !tx->dead, "hup: the EOF is applied after the tail");
    flow_tick(tx, now_ns());
    CHECK(tx->fin_sent, "hup: the deferred FIN follows");

    flow_free(&tx_e, tx);
    close(sv[0]);
    close(sv[1]);
}

/* The drained-socket case: zero pending bytes and a hangup. The peek sees
 * the peer's EOF even though the window is closed, and EOF needs no credit. */
static void test_app_hup_clean_eof_without_credit(void)
{
    struct flow *tx = tx_flow_new();
    int sv[2];
    u64 block_pkts = (u64)tx->groups * tx->n;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    (void)shutdown(sv[1], SHUT_WR);
    tx->app_fd = sv[0];
    tx->tx_credit_shares = (u64)tx->groups * (tx->n + tx->m);
    tx->tx_seq = block_pkts;
    tx->tx_ack_next = 0;
    tx_e.epfd = -1;

    flow_target_event(tx, EPOLLHUP);
    CHECK(tx->app_eof && !tx->dead,
          "hup: a clean EOF is applied without credit");
    flow_tick(tx, now_ns());
    CHECK(tx->fin_sent, "hup: the EOF sends FIN once the gate opens");
    flow_free(&tx_e, tx);
    close(sv[0]);
    close(sv[1]);
}

/* A carrier that terminates its chunked message and starts a new one (a
 * proxy or an HTTP-aware peer may do this) must not wedge: the decoder
 * reports NEED_MORE with the parser DONE, and the next bytes are a fresh
 * header block. */
static void test_carrier_http_terminator_reset(void)
{
    static const char resp[] = "HTTP/1.1 200 OK\r\n\r\n";
    struct carrier c;
    struct tbt_hdr h;
    struct tbt_http_out out;
    u8 buf[512];
    size_t o = 0;

    clear_engine();
    e.near = true;
    memset(&c, 0, sizeof(c));
    c.e = &e;
    c.index = 0;
    tbt_http_rx_new_message(&c.rx);

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_PAD;
    h.conv = TBT_CONV;
    h.body_len = 0;
    out.hdr = &h;
    out.body = NULL;

    memcpy(buf + o, resp, sizeof(resp) - 1);
    o += sizeof(resp) - 1;
    o += tbt_http_frame_encode((char *)buf + o, sizeof(buf) - o, &out);
    CHECK(o > sizeof(resp) - 1, "http: the first PAD chunk encodes");
    memcpy(buf + o, "0\r\n\r\n", 5);
    o += 5;
    memcpy(buf + o, resp, sizeof(resp) - 1);
    o += sizeof(resp) - 1;
    o += tbt_http_frame_encode((char *)buf + o, sizeof(buf) - o, &out);
    CHECK(o < sizeof(buf), "http: two messages fit");

    memcpy(c.rbuf, buf, o);
    c.rlen = o;
    CHECK(carrier_process(&e, &c) == 0,
          "http: the post-terminator message is parsed");
    CHECK(c.rx_records == 2, "http: both PAD records are delivered");
    CHECK(c.rlen == 0, "http: the whole buffer is consumed");
    CHECK(c.http_hdr_done, "http: the carrier is back in the chunk stage");
}

/* OPEN is only meaningful after HELLO joined the carrier, only for the
 * configured target, and never for the zero sid. A refused OPEN must not
 * close the carrier; a failed target connect must reset the flow instead of
 * leaking its slot. */
static void test_open_gate_and_target(void)
{
    struct tbt_hdr h;
    struct carrier c;
    u8 body[TBT_OPEN_BODY_LEN];
    u8 rec[TBT_HDR_LEN + TBT_OPEN_BODY_LEN];
    int i;

    clear_engine();
    e.near = false;
    e.n_car = 4;
    /* The far allocates a flow with its own TX shape before it dials. */
    e.tx_shape.variant = TBT_FEC_SCLC_UP;
    e.tx_shape.n = 8;
    e.tx_shape.m = 24;
    e.tx_shape.groups = 8;
    e.tx_shape.block = TBT_FEC_SCLC_BLOCK_UP;
    e.tx_shape.L = TBT_FEC_SCLC_L_UP;
    e.tx_shape.rate_ppm = 250;
    e.tx_shape.rate_256 = 64;
    e.tx_n = 8;
    e.tx_m = 24;
    e.tx_groups = 8;
    e.tx_seed = 1;
    e.cfg.cfg.target_port = tbt_port_be16(443);
    memset(body, 0, sizeof(body));
    tbt_put_le16(body + TBT_OPEN_ROUTE_SLOT, 0);
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, 0);
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_OPEN;
    h.body_len = sizeof(body);
    h.conv = TBT_CONV;
    h.sid_lo = 7;

#define OPEN_REC()                                                          \
    do                                                                      \
    {                                                                       \
        tbt_raw_put_hdr(rec, &h, sizeof(body));                              \
        memcpy(rec + TBT_HDR_LEN, body, sizeof(body));                       \
    } while (0)

    memset(&c, 0, sizeof(c));
    c.e = &e;
    c.index = 0;
    OPEN_REC();
    CHECK(handle_record(&e, &c, rec, sizeof(rec)) == -EPROTO,
          "open: a record before HELLO is refused");
    CHECK(e.flow_accepted == 0 && e.flow[0] == NULL,
          "open: no flow and no target connect before HELLO");

    c.hello_recv = true;
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, 8080);
    OPEN_REC();
    CHECK(handle_record(&e, &c, rec, sizeof(rec)) == 0,
          "open: a foreign target is dropped, not a carrier fault");
    CHECK(e.flow_accepted == 0 && e.flow[0] == NULL,
          "open: a foreign target creates no flow");

    tbt_put_le16(body + TBT_OPEN_ROUTE_SLOT, 1);
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, 0);
    OPEN_REC();
    CHECK(handle_record(&e, &c, rec, sizeof(rec)) == 0 &&
              e.flow_accepted == 0,
          "open: an unsupported mapping slot is refused");

    tbt_put_le16(body + TBT_OPEN_ROUTE_SLOT, 0);
    h.sid_lo = 0;
    OPEN_REC();
    CHECK(handle_record(&e, &c, rec, sizeof(rec)) == 0 &&
              e.flow_accepted == 0,
          "open: sid 0 is the no-stream sentinel");
    h.sid_lo = 7;

    /* Matching the configured port reaches the connect path. An unsupported
     * target family makes connect fail synchronously: the flow must be
     * reset and then released, not left with app_fd = -1. */
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, 443);
    e.cfg.cfg.target.family = 99;
    clear_pending();
    OPEN_REC();
    CHECK(handle_record(&e, &c, rec, sizeof(rec)) == 0,
          "open: a failed connect is not a carrier fault");
    CHECK(e.flow_accepted == 1 && e.flow[0] != NULL,
          "open: the flow was allocated");
    CHECK(e.flow[0] && e.flow[0]->dead && e.flow[0]->app_fd == -1,
          "open: a failed connect resets the flow");
    CHECK(e.npending == 1 && e.pending[0].rec[0] == TBT_F_RST,
          "open: the near side is told with RST");
    if (e.flow[0])
        flow_tick(e.flow[0], now_ns());
    CHECK(e.flow[0] == NULL && e.flow_freed == 1,
          "open: the reset flow releases its slot");

    for (i = 0; i < TBT_FLOW_MAX; i++)
        CHECK(e.flow[i] == NULL, "open: no slot leaked");
#undef OPEN_REC
}

/* The sparse probe asks for a block the sender may never have built. Asking
 * forever on an idle flow is a NACK every retry interval for the life of the
 * connection; three unanswered probes stop it, a later share proves the
 * sender moved on, and a delivered FIN ends the questions entirely. */
static void test_sparse_probe_cap(void)
{
    u64 ids[16];
    u64 t;
    int i;

    clear_engine();
    set_downlink_shape();
    e.rtt_us = 0;
    clear_pending();
    t = now_ns();
    f.rx_hole_ns = t;
    f.rx_probe_ns = 0;
    f.rx_probe_block = f.rx_next_block;
    f.rx_probe_tries = 0;
    t += TBT_NACK_FIRST_NS + 2ull * TBT_NACK_RETRY_NS + 1000000ull;
    for (i = 0; i < 6; i++)
    {
        clear_pending();
        flow_rx_nack(&f, t);
        if (i == 0)
            CHECK(count_nacks(ids, 16) == 1, "probe: the first probe is sent");
        t += TBT_NACK_PROBE_NS + 1000000ull;
    }
    clear_pending();
    flow_rx_nack(&f, t);
    CHECK(count_nacks(ids, 16) == 0,
          "probe: an idle frontier stops after three unanswered probes");
    CHECK(f.rx_probe_tries == TBT_NACK_PROBE_MAX &&
              f.rx_sparse_probe == TBT_NACK_PROBE_MAX,
          "probe: the cap is exactly the module's");

    /* A later block proves the sender built the frontier: ask again. */
    marker_block(1, 128, 128, 128);
    clear_pending();
    f.rx_probe_ns = 0;
    t += TBT_NACK_PROBE_NS + 1000000ull;
    flow_rx_nack(&f, t);
    CHECK(count_nacks(ids, 16) >= 1,
          "probe: a sender ahead is asked again");

    /* The stream end is delivered: no probe may name a block past it. */
    f.peer_fin_end_set = true;
    f.peer_fin_end = f.rx_ack_frontier;
    clear_pending();
    f.rx_probe_ns = 0;
    flow_rx_nack(&f, t + TBT_NACK_PROBE_NS + 1000000ull);
    CHECK(count_nacks(ids, 16) == 0,
          "probe: a delivered FIN stops every request");
}

/* The FIN body names the packet after the sender's last DATA packet in wire
 * sequence space. An idle-sealed short block leaves rx_next_deliver (real
 * packet count) permanently behind that frontier; the half-close must use
 * the block-aligned wire frontier instead or the target never sees EOF. */
static void test_fin_frontier_after_idle_seal(void)
{
    struct flow *rf = &f;
    u8 drain[TBT_FEC_S];
    int sv[2];
    bool eof = false;
    int i;

    clear_engine();
    /* Block 0 is one idle-sealed 3-byte share plus seven markers; block 1
     * is full and starts at seq0 = groups*n = 64 (the padding jump). */
    send_data(0, 0, 8, 24, 0, 0, 3, true);
    send_parity(0, 0, 8, 24, 0, 1, 3);
    for (i = 1; i < 8; i++)
        send_marker(0, 0, 8, 24, (u16)i);
    full_block(1, 64, 8, 24);
    CHECK(rf->rx_next_block == 2, "fin: both blocks delivered");
    CHECK(rf->rx_next_deliver == 65,
          "fin: the real-packet cursor lags the wire jump");
    CHECK(rf->rx_ack_frontier == 128,
          "fin: the wire frontier reaches the FIN's end");

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    rf->app_fd = sv[0];
    rf->fin_recv = true;
    rf->peer_fin_end_set = true;
    rf->peer_fin_end = 128;
    CHECK(rf->rx_next_deliver < rf->peer_fin_end,
          "fin: the old count frontier could never reach the FIN");
    flow_out_flush(rf);
    CHECK(rf->fin_applied,
          "fin: the short-block stream applies its FIN");
    for (;;)
    {
        ssize_t n;

        do
            n = read(sv[1], drain, sizeof(drain));
        while (n < 0 && errno == EINTR);
        if (n > 0)
            continue;
        eof = n == 0;
        break;
    }
    CHECK(eof, "fin: the target write side sees EOF");
    close(sv[1]);
    rf->app_fd = -1;
}

/* OPEN and FEC_CFG are resent until the peer proves it has them; a record
 * lost with the carrier that carried it otherwise stalls the stream. */
static void test_open_cfg_retry(void)
{
    struct flow *tx = tx_flow_new();
    u8 types[64];
    u8 body[TBT_FEC_FBK_LEN];
    struct tbt_fec_fbk fb;
    u64 t0 = 1000000000ull;
    int n, i;
    int opens, cfgs;

    tx->open_sent = false;
    tx->open_acked = false;
    tx->cfg_acked = false;
    tx_fake_carriers();
    tx_e.up_count = 1;

    flow_tick(tx, t0);
    n = wire_drain(types, 64);
    opens = cfgs = 0;
    for (i = 0; i < n; i++)
    {
        opens += types[i] == TBT_F_OPEN;
        cfgs += types[i] == TBT_FEC_CFG;
    }
    CHECK(opens == 1 && cfgs == 1, "retry: the first tick sends OPEN+CFG");

    flow_tick(tx, t0 + 1000000ull);
    n = wire_drain(types, 64);
    CHECK(n == 0, "retry: nothing is resent inside the interval");

    flow_tick(tx, t0 + TBT_CTL_RETRY_NS);
    n = wire_drain(types, 64);
    opens = cfgs = 0;
    for (i = 0; i < n; i++)
    {
        opens += types[i] == TBT_F_OPEN;
        cfgs += types[i] == TBT_FEC_CFG;
    }
    CHECK(opens == 1 && cfgs == 1,
          "retry: an unacknowledged OPEN and CFG are resent");

    /* The far echoes OPEN and ACKs the CFG: both retries stop. */
    tx->open_acked = true;
    memset(&fb, 0, sizeof(fb));
    fb.flags = TBT_FEC_FBK_F_CFG_ACK;
    CHECK(tbt_fec_fbk_pack(&fb, body) == 0, "retry: CFG_ACK packs");
    handle_fbk(tx, body);
    CHECK(tx->cfg_acked, "retry: the ACK stops the CFG retry");
    flow_tick(tx, t0 + 2ull * TBT_CTL_RETRY_NS);
    n = wire_drain(types, 64);
    CHECK(n == 0, "retry: acknowledged control records stay quiet");

    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* A FIN that ctl_send() could not even park must not latch fin_sent: the
 * retry in flow_tick sends it as soon as a carrier (or queue slot) returns. */
static void test_fin_latch_retry(void)
{
    struct flow *tx = tx_flow_new();
    bool rst, fin;
    int i;

    tx_fake_carriers();
    for (i = 0; i < TBT_CAR_MAX; i++)
        tx_e.car[i]->up = false;
    tx_e.npending = TBT_CTL_PENDING; /* the control queue is full */
    tx->app_eof = true;
    tx->tx_finished = true;
    tx->tx_pkt_base_set = true;
    CHECK(flow_send_fin(tx) != 0 && !tx->fin_sent,
          "fin: a refused queue does not latch the FIN");

    tx_e.npending = 0;
    tx_e.car[0]->up = true;
    flow_tick(tx, now_ns() + TBT_CTL_RETRY_NS);
    CHECK(tx->fin_sent, "fin: the retry queues the FIN once a carrier returns");
    wire_close_controls(&rst, &fin);
    CHECK(fin && !rst, "fin: the retried FIN reaches the wire");

    flow_free(&tx_e, tx);
    tx_clear_carriers();
}

/* Key material must come from getrandom(); a failed draw aborts instead of
 * falling back to the public xorshift state. */
static void test_csprng_failure_fatal(void)
{
    clear_engine();
    e.near = true;
    test_fail_csprng = true;
    CHECK(engine_init_session_nonce(&e) == -EIO,
          "csprng: a failed draw is a fatal error");
    CHECK(!e.session_nonce_set, "csprng: no xorshift fallback nonce");
    test_fail_csprng = false;
    CHECK(engine_init_session_nonce(&e) == 0 && e.session_nonce_set,
          "csprng: the next draw fills the nonce");
}

/* profile=raw is the module's unframed container; this engine frames every
 * carrier as HTTP. It must refuse the profile instead of advertising it in
 * HELLO and mis-framing both directions. */
static void test_raw_profile_rejected(void)
{
    struct tbt_engine_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cfg.role = TBT_ROLE_NEAR;
    cfg.cfg.mode = 3;
    cfg.cfg.carriers = 256;
    cfg.cfg.profile = 3;
    CHECK(tbt_engine_run(&cfg) == -EINVAL,
          "raw: profile=3 is refused at startup");
}

/* The ACK grant is also the application backlog bound: a slow reader must
 * throttle the peer instead of letting flow_out_append() hit TBT_OUT_CAP and
 * reset the stream. The minimum grant is one block (the sender's stop-and-
 * wait floor), and a drained reader restores the full window. */
static void test_rx_credit_backpressure(void)
{
    u64 block_bytes;
    u64 full;
    int sv[2];

    clear_engine();
    set_downlink_shape();
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    f.app_fd = sv[0];
    block_bytes = (u64)f.rx_groups * f.rx_n * TBT_FEC_S;
    full = (u64)(TBT_RX_SLOTS - 2) * (u64)f.rx_groups *
           ((u32)f.rx_n + f.rx_m);

    clear_pending();
    CHECK(flow_send_ack(&f) == 0 && last_ack_credit() == (int)full,
          "backlog: a drained reader grants the full window");

    /* Withhold credit once the backlog reaches the whole grant: the sender
     * must fall back to the one-block stop-and-wait floor. */
    f.out_len = (size_t)((u64)(TBT_RX_SLOTS - 2) * block_bytes);
    f.out_off = 0;
    clear_pending();
    CHECK(flow_send_ack(&f) == 0 &&
              last_ack_credit() == (int)(f.rx_groups * (f.rx_n + f.rx_m)),
          "backlog: a deep queue withholds credit down to one block");

    f.out_off = f.out_len;
    clear_pending();
    CHECK(flow_send_ack(&f) == 0 && last_ack_credit() == (int)full,
          "backlog: draining restores the full grant");

    f.out_len = f.out_off = 0;
    f.app_fd = -1;
    close(sv[0]);
    close(sv[1]);
}

/* A partial carrier write leaves a dead front in wbuf. When a following
 * record does not fit at the tail but does fit after compaction, the queue
 * must move the live bytes down instead of reporting EAGAIN. */
static void test_carrier_queue_compaction(void)
{
    struct carrier c;
    u8 rec[1000];
    char fill[4096];
    int sv[2];
    int buf = 4096;
    size_t live;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    memset(fill, 0x5a, sizeof(fill));
    for (;;)
    {
        ssize_t n = write(sv[0], fill, sizeof(fill));

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0)
            abort();
    }
    memset(&c, 0, sizeof(c));
    c.e = &tx_e;
    c.fd = sv[0];
    c.index = 0;
    c.woff = 4096;
    c.wlen = sizeof(c.wbuf) - 500;
    live = c.wlen - c.woff;
    memset(rec, 0x22, sizeof(rec));
    CHECK(carrier_queue(&c, rec, sizeof(rec)) == 0,
          "carrier: a dead front is compacted, not EAGAIN");
    CHECK(c.woff == 0 && c.wlen == live + sizeof(rec),
          "carrier: the record follows the compacted bytes");
    close(sv[0]);
    close(sv[1]);
}

/* The 32-share GF(16) uplink block emits a ten-row parity floor on a short
 * seal. Asking min(real, m) left a lost share with no covering row; the
 * shared NACK limit helper names the rows the sender really emitted. */
static void test_nack_uplink_parity_floor(void)
{
    const u8 *nack;
    u64 t;

    clear_engine();
    e.rtt_us = 0;
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    send_parity(0, 0, 8, 24, 0, 5, 0);
    clear_pending();
    t = now_ns() + TBT_NACK_FIRST_NS + 2ull * TBT_NACK_RETRY_NS +
        1000000ull;
    flow_rx_nack(&f, t);
    nack = nack_for_group(0);
    CHECK(nack != NULL, "nack floor: the short uplink block is requested");
    if (!nack)
        return;
    CHECK(nack_bit_count(nack) == 13,
          "nack floor: four DATA gaps plus nine missing rows");
    CHECK(nack_names(nack, 1) && nack_names(nack, 4) &&
              !nack_names(nack, 5),
          "nack floor: only the DATA gaps below the cut are named");
    CHECK(nack_names(nack, 8 + 9) && !nack_names(nack, 8 + 10),
          "nack floor: ten rows are named, no row past the floor");
}

static void test_rx_out_high_pauses_frontier(void)
{
    int sv[2];
    u64 prev_frontier;

    clear_engine();
    set_downlink_shape();
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv))
        abort();
    f.app_fd = sv[0];
    f.out_cap = TBT_OUT_CAP;
    f.out = malloc(f.out_cap);
    if (!f.out)
        abort();
    f.out_len = TBT_OUT_HIGH;
    f.out_off = 0;
    prev_frontier = f.rx_ack_frontier;

    full_block(0, 0, 128, 128);
    CHECK(f.rx_next_block == 0 && f.rx_ack_frontier == prev_frontier,
          "out_high: flow_rx_check pauses delivery and freezes frontier");

    /* Draining below TBT_OUT_LOW resumes delivery and advances frontier. */
    f.out_len = TBT_OUT_LOW - 1024;
    f.out_off = f.out_len;
    flow_rx_resume(&f);
    CHECK(f.rx_next_block == 1 && f.rx_ack_frontier == 128,
          "out_high: draining below TBT_OUT_LOW resumes delivery");

    free(f.out);
    f.out = NULL;
    f.out_cap = f.out_len = f.out_off = 0;
    f.app_fd = -1;
    close(sv[0]);
    close(sv[1]);
}

static void test_tx_read_limit_idle_seal_and_cur_len(void)
{
    struct flow *tx = tx_flow_new();

    tx->tx_credit_shares = (u64)tx->groups * (tx->n + tx->m); /* 1 block */
    /* Simulate block 0 idle-sealed with 1 real share + 63 padding shares,
     * then 512 bytes already buffered in tx_cur for block 1. */
    tx->tx_seq = 1;
    tx->tx_blk_seq0 = (u64)tx->groups * tx->n; /* 64 */
    tx->tx_block = 1;
    tx->tx_ack_next = 0;
    tx->tx_cur_len = 512;
    /* Window is 1 block (64 packets). next_seq is 64, so 64 packets are already
     * occupied -> read limit must be 0. */
    CHECK(flow_tx_read_limit(tx) == 0,
          "read_limit: idle-sealed block jump counts against credit window");

    /* Grant 2 blocks (window 128). 64 remaining packets minus 512 B buffered
     * (limited by sizeof(tx->rbuf)). */
    tx->tx_credit_shares = 2ull * tx->groups * (tx->n + tx->m);
    tx->tx_ack_next = 0;
    tx->tx_group = (u16)(tx->groups - 1);
    tx->tx_share = (u16)(tx->n - 1); /* next_seq = 64 + 7*8 + 7 = 127 */
    CHECK(flow_tx_read_limit(tx) == TBT_FEC_S - 512,
          "read_limit: subtracts tx_cur_len from remaining window");

    flow_free(&tx_e, tx);
}

static void test_rx_ff_last_and_fin_tail_no_parity(void)
{
    struct tbt_fec_hdr h;

    clear_engine();
    /* Send 2 data shares on group 0 with TBT_FEC_FF_LAST on share 1, and NO
     * parity shares (simulating lost parity on a dead carrier). */
    send_data(0, 0, 8, 24, 0, 0, TBT_FEC_S, false);
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = TBT_FEC_FF_PADDED | TBT_FEC_FF_LAST;
    h.share_index = 1;
    h.n = 8;
    h.m = 24;
    h.block_id = 0;
    h.seq = 1;
    h.payload_len = TBT_FEC_S;
    h.win_len = 321;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = 0;
    flow_rx_store(&f, &h, payload);
    for (int g = 1; g < 8; g++)
        send_marker(0, 0, 8, 24, (u16)g);
    CHECK(f.rx_next_block == 1 && f.rx_deliver_bytes == TBT_FEC_S + 321,
          "ff_last: data share FF_LAST delivers short block without parity");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_marker_short_sequence();
    test_credit_inflight();
    test_window_and_nack_base();
    test_nack_short_block_limits();
    test_nack_hold_follows_rtt();
    test_carrier_keepalive_sockopts();
    test_multishape_sequence();
    test_16m_boundary();
    test_downlink_shapes();
    test_far_shape_ladder();
    test_downlink_erasure();
    test_short_block_known_zero_padding();
    test_short_block_two_lost_shares();
    test_shape_rejection();
    test_shape_switch_stream();
    test_shape_switch_inflight();
    test_adaptive_off();
    test_session_nonce_config();
    test_session_nonce_hello();
    test_session_nonce_parse();
    test_carrier_lifecycle();
    test_far_accept_reap();
    test_carrier_replace();
    test_retire_carrier();
    test_stall_dump();
    test_tx_idle_empty();
    test_tx_peer_credit();
    test_tx_block_credit();
    test_tx_idle_3();
    test_tx_idle_200();
    test_tx_idle_short_then_long();
    test_tx_dead_carrier_share();
    test_tx_dead_carrier_app_read();
    test_tx_all_carriers_down();
    test_ctl_dead_carrier();
    test_rx_rst_aborts_app();
    test_app_epipe_sends_rst();
    test_app_eof_sends_fin();
    test_tx_encode_failure();
    test_tx_replay_alloc_failure();
    test_sparse_probe();
    test_fbk_commit();
    test_credit_headroom();
    test_adaptive_sample_policy();
    test_half_open_reclaim();
    test_flow_table_refusal();
    test_hello_reincarnation();
    test_hello_reincarnation_off();
    test_far_psk_reincarnation();
    test_hello_epoch_path_replay();
    test_hello_epoch_same_nonce_reorder();
    test_hello_epoch_fresh_per_carrier();
    test_sid_incarnation_seed();
    test_epoch_clock_clamp();
    test_hello_epoch_limit();
    test_http_date_clamp();
    test_app_credit_poll_cycle();
    test_app_hup_keeps_unread_tail();
    test_app_hup_clean_eof_without_credit();
    test_carrier_http_terminator_reset();
    test_open_gate_and_target();
    test_sparse_probe_cap();
    test_fin_frontier_after_idle_seal();
    test_open_cfg_retry();
    test_fin_latch_retry();
    test_csprng_failure_fatal();
    test_raw_profile_rejected();
    test_rx_credit_backpressure();
    test_carrier_queue_compaction();
    test_nack_uplink_parity_floor();
    test_rx_out_high_pauses_frontier();
    test_tx_read_limit_idle_seal_and_cur_len();
    test_rx_ff_last_and_fin_tail_no_parity();

    printf("test_client_rx: %d checks, %d failures\n", checks, failures);
    free(f.out);
    release_blocks();
    release_codecs();
    return failures ? 1 : 0;
}
