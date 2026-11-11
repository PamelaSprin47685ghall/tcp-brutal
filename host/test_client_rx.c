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
 *   - ACK credit is (TBT_RX_SLOTS - 1) * the current rx_n;
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 8u),
          "sequence: ACK credit is 7 slots * n");
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 132u),
          "credit: ACK grants slots-1 blocks");

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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 146u),
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 8u),
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 9u),
          "shape: credit follows the current rx_n");
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 132u),
          "down: credit is 8 * 132");

    full_block(1, 146, 146, 110);
    CHECK(f.rx_next_block == 2 && f.rx_next_deliver == 146,
          "down: 146/110 full block delivered");
    CHECK(f.rx_n == 146 && f.rx_m == 110,
          "down: current split follows the full block");
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 146u),
          "down: credit is 8 * 146");
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 146u),
          "16m: credit follows the post-boundary split");
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
    CHECK(last_ack_credit() == (int)((TBT_RX_SLOTS - 1) * 146u),
          "ladder: credit follows the latest rx_n");
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
    test_stall_dump();
    test_fbk_commit();

    printf("test_client_rx: %d checks, %d failures\n", checks, failures);
    free(f.out);
    release_blocks();
    release_codecs();
    return failures ? 1 : 0;
}
