/* test_client_rx.c - the near-side receive state machine under markers,
 * short blocks and an 8-slot window.
 *
 * The engine's receive functions are static, so this test includes the
 * translation unit directly and drives flow_rx_store()/flow_rx_check() with
 * synthetic 0.8 records. The shapes are the uplink 8x32 GF(16) block, but no
 * codec is needed: every data share of a block is present, so the fast path
 * marks the group decoded without a parity solve.
 *
 * It pins the three fixes that a real .92 near side needed:
 *   - NACK anchors on rx_next_block, never on a delivery frontier that marker
 *     blocks leave behind;
 *   - rx_next_deliver advances by real packets: marker 0, short sum(win_len);
 *   - ACK credit is TBT_RX_SLOTS * n, not the 8192-packet session ceiling.
 * A counter/guard checks that a stale slot below rx_next_block can never be
 * NACKed and that a late replay is counted instead of silently wedging.
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
    memset(&f, 0, sizeof(f));
    memset(&e, 0, sizeof(e));
    f.e = &e;
    f.sid = 1;
    f.app_fd = -1;
    f.rx_shape_set = true;
    f.rx_n = 8;
    f.rx_m = 24;
    f.rx_groups = 8;
    e.n_car = 0;
    e.tx_n = 8;
    e.crypto_on = false;
    memset(payload, 0x5a, sizeof(payload));
}

static void clear_pending(void)
{
    e.npending = 0;
}

static void send_data(u64 block, u64 seq0, u16 group, u16 share, u16 len,
                      bool padded)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = padded ? TBT_FEC_FF_PADDED : 0;
    h.share_index = share;
    h.n = 8;
    h.m = 24;
    h.block_id = block;
    h.seq = seq0 + (u64)group * 8u + share;
    h.payload_len = padded ? TBT_FEC_S : len;
    h.win_len = padded ? len : 0;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, payload);
}

static void send_parity(u64 block, u64 seq0, u16 group, u16 win_len,
                        u16 epoch)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = 8;
    h.n = 8;
    h.m = 24;
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

static void send_marker(u64 block, u64 seq0, u16 group)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = 8;
    h.n = 8;
    h.m = 24;
    h.block_id = block;
    h.seq = seq0;
    h.conv = TBT_FEC_CONV;
    h.sid = f.sid;
    h.reserved = group;
    flow_rx_store(&f, &h, payload);
}

static void full_block(u64 block, u64 seq0)
{
    u16 g, s;

    for (g = 0; g < 8; g++)
        for (s = 0; s < 8; s++)
            send_data(block, seq0, g, s, TBT_FEC_S, false);
}

/* Two real shares per group: the last one is PADDED, and the parity states
 * win_len=2. The block therefore contributes 16 packets, not 64. */
static void short_block(u64 block, u64 seq0)
{
    u16 g;

    for (g = 0; g < 8; g++)
    {
        send_data(block, seq0, g, 0, TBT_FEC_S, false);
        send_data(block, seq0, g, 1, 784, true);
        send_parity(block, seq0, g, 2, 784);
    }
}

static void marker_block(u64 block, u64 seq0)
{
    u16 g;

    for (g = 0; g < 8; g++)
        send_marker(block, seq0, g);
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
    full_block(0, 0);
    short_block(1, 64);
    for (i = 2; i <= 5; i++)
        marker_block((u64)i, (u64)i * 64);
    full_block(6, 6 * 64);

    CHECK(f.rx_next_block == 7, "sequence: next block is 7");
    CHECK(f.rx_next_deliver == 144, "sequence: frontier counts real packets");
    CHECK(f.rx_deliver_bytes == 64u * TBT_FEC_S +
                                    8u * (TBT_FEC_S + 784u) +
                                    64u * TBT_FEC_S,
          "sequence: delivered bytes");
    CHECK(last_ack_credit() == (int)(TBT_RX_SLOTS * 8u),
          "sequence: ACK credit is 8 slots * n");
    CHECK(f.rx_stale_nack == 0 && f.rx_late_share == 0 &&
              f.rx_window_drop == 0,
          "sequence: no stale request or late drop");

    /* A delivered marker block must not be resurrected by a NACK pass. */
    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 1000000u);
    CHECK(count_nacks(ids, 16) == 0, "sequence: no NACK for delivered blocks");
}

/* Eight slots hold blocks 0..7. Block 8 must be refused until block 0
 * completes; NACK must name block 0 only; the stub block 0 then completes
 * with one real share and seven markers. */
static void test_window_and_nack_base(void)
{
    u64 ids[16];
    int n;
    int i;

    clear_engine();
    send_data(0, 0, 0, 0, TBT_FEC_S, false);
    for (i = 1; i < TBT_RX_SLOTS; i++)
        marker_block((u64)i, (u64)i * 64);

    /* The ninth block is outside the 8-slot ring. */
    send_data(8, 8 * 64, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_window_drop == 1, "window: block 8 refused");
    CHECK(f.rx_next_block == 0, "window: head still block 0");

    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 1000000u);
    n = count_nacks(ids, 16);
    CHECK(n >= 1, "nack: block 0 requested");
    for (i = 0; i < n && i < 16; i++)
        CHECK(ids[i] == 0, "nack: only the window head is named");
    CHECK(f.rx_stale_nack == 0, "nack: no stale request counter");

    /* A stale head below the delivery cursor must not be NACKed. Block 0
     * is still resident but the cursor is moved past it for this probe. */
    f.rx_next_block = 1;
    clear_pending();
    flow_rx_nack(&f, now_ns() + TBT_NACK_FIRST_NS + 2000000u);
    CHECK(count_nacks(ids, 16) == 0, "nack: stale slot is skipped");
    f.rx_next_block = 0;

    /* Complete block 0: share 0 is present, parity says win_len=1, and the
     * other seven groups are markers. */
    clear_pending();
    send_parity(0, 0, 0, 1, 0);
    for (i = 1; i < 8; i++)
        send_marker(0, 0, (u16)i);
    CHECK(f.rx_next_block == 8, "window: blocks 0..7 drained");
    CHECK(f.rx_next_deliver == 1, "window: frontier is the one real share");
    CHECK(f.rx_deliver_bytes == TBT_FEC_S, "window: delivered one share");
    CHECK(last_ack_credit() == (int)(TBT_RX_SLOTS * 8u),
          "window: ACK credit follows the ring");

    /* The refused block 8 now fits and advances the frontier by a full
     * block; the eight slots never held more than eight blocks. */
    full_block(8, 8 * 64);
    CHECK(f.rx_next_block == 9, "window: block 8 accepted after drain");
    CHECK(f.rx_next_deliver == 65, "window: full block adds 64 packets");

    /* A replay older than the delivery window is counted, not reinterpreted. */
    send_data(0, 0, 0, 0, TBT_FEC_S, false);
    CHECK(f.rx_late_share == 1, "late: old replay counted");
    CHECK(f.rx_next_block == 9, "late: cursor unchanged");
}

int main(void)
{
    test_marker_short_sequence();
    test_window_and_nack_base();

    printf("test_client_rx: %d checks, %d failures\n", checks, failures);
    free(f.out);
    release_blocks();
    return failures ? 1 : 0;
}
