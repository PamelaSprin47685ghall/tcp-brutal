/* bench_micro.c - the host microbenchmark base for the tbtun data path.
 *
 * WHAT THIS MEASURES
 *   The cost of the pure, host-linkable parts of the data path, one operation
 *   per row:
 *
 *     linked  the same object files the module links: tbt_fec.c (RLNC
 *             encode/decode, grouped codecs, replay ring, header CRC, bitmap,
 *             NACK pacing) and tbt_crypto.c (body XOR, per-share parity XOR).
 *     inline  tbt_tx_acct.h, the exact accounting the capture and publish
 *             paths run.
 *     model   the two hot loops that live in kernel-only files and cannot be
 *             linked on the host: tbt_tx.c's scheduler (inflight rank,
 *             insertion-sort rebuild, stalled mask) and tbt_fec_data.c's NACK
 *             bitmap scan. Each model is a line-for-line mirror of the named
 *             kernel function and is marked `model.` in its row name. A model
 *             that drifts from the kernel is a defect: keep them in lockstep
 *             the way tests/unit/test_reorder.c and tests/unit/test_fec_out.c already do.
 *
 * OUTPUT
 *   CSV on stdout, one header line and one row per benchmark:
 *
 *     name,iterations,ns_per_op,mb_per_s,checksum
 *
 *   `iterations` is the total number of operations over every measured round,
 *   `ns_per_op` the median of the per-round means, `mb_per_s` the payload
 *   bytes moved per second (10^6 bytes/s; 0 for operations that move no
 *   payload), and `checksum` a 64-bit FNV-1a over the outputs, so a compiler
 *   cannot delete the work and a diff can see a semantic change.
 *
 *   A human table goes to stderr. Redirect stdout to a file for a baseline.
 *
 * TIME BUDGET
 *   --quick (default) aims at <= 2 s, --full at <= 10 s. Each benchmark is
 *   warmed up, then measured over 5 (quick) or 7 (full) rounds; a round is
 *   calibrated to 8 ms (quick) or 40 ms (full). A hard deadline stops the
 *   run early rather than overrun the budget; the affected row keeps the
 *   rounds it completed. For a low-noise baseline use --full on an idle
 *   machine, pin with taskset, and compare medians, never single runs.
 *
 *   make host-bench-micro
 *   build/bin/bench_micro --quick > tests/throughput/results/<stamp>/host.csv
 *
 * Not measured here: skb allocation, copy_from_user, TCP, and the carrier
 * pump. Those need the guest; tests/throughput/short.sh covers them on real hosts.
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_tx_acct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ *
 * Driver
 * ------------------------------------------------------------------ */

struct bench
{
    const char *name;
    size_t bytes_per_op;
    void (*run)(unsigned int iters, u64 *sum);
    /* A stable witness for the checksum column: the input bytes, or a value
     * derived from the initial state. It must not depend on the iteration
     * count, so two runs of the same binary produce the same checksum. */
    const void *witness;
    size_t witness_len;
    u64 (*witness_fn)(void);
};

static u64 fnv1a(const void *p, size_t n)
{
    const u8 *b = p;
    u64 h = 0xcbf29ce484222325ULL;
    size_t i;

    for (i = 0; i < n; i++)
    {
        h ^= b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int quick_mode = 1;
static double deadline;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* One round is calibrated so it lasts at least `round_sec`. The doubling
 * stops at 2^26 operations: a benchmark that still finishes faster is
 * timer-noise bound, and a larger count would not help. */
static unsigned int calibrate(const struct bench *b, double round_sec)
{
    unsigned int iters = 1;

    for (;;)
    {
        u64 sum = 0;
        double t0 = now_sec();
        double dt;

        b->run(iters, &sum);
        dt = now_sec() - t0;
        if (dt >= round_sec || iters >= (1u << 26))
            return iters;
        iters *= 2;
    }
}

static void measure(const struct bench *b)
{
    double round_sec = quick_mode ? 0.008 : 0.040;
    int max_rounds = quick_mode ? 5 : 7;
    double samples[7];
    unsigned int iters;
    int rounds = 0;
    int r;
    u64 total_iters = 0;
    u64 cs;
    double median;
    double mbps;

    cs = b->witness ? fnv1a(b->witness, b->witness_len)
                    : b->witness_fn ? b->witness_fn() : 0;
    iters = calibrate(b, round_sec);
    for (r = 0; r < max_rounds; r++)
    {
        u64 sum = 0;
        double t0;
        double dt;
        double ns;

        if (rounds > 0 && now_sec() >= deadline)
            break;
        t0 = now_sec();
        b->run(iters, &sum);
        dt = now_sec() - t0;
        if (dt <= 0.0)
            dt = 1e-9;
        ns = dt * 1e9 / (double)iters;
        samples[rounds++] = ns;
        total_iters += iters;
    }
    if (!rounds)
        return;

    qsort(samples, (size_t)rounds, sizeof(samples[0]), cmp_double);
    median = samples[rounds / 2];
    mbps = b->bytes_per_op
               ? (double)b->bytes_per_op / (median * 1e-9) / 1e6
               : 0.0;
    printf("%s,%llu,%.3f,%.3f,%016llx\n", b->name,
           (unsigned long long)total_iters, median, mbps,
           (unsigned long long)cs);
    fprintf(stderr, "  %-42s %12.3f ns/op %10.3f MB/s (%d rounds)\n",
            b->name, median, mbps, rounds);
}

/* ------------------------------------------------------------------ *
 * FEC: RLNC encode and decode (linked tbt_fec.c)
 * ------------------------------------------------------------------ */

#define FEC_S 1500u
#define FEC_L4 4u
#define FEC_L8 8u

static struct tbt_fec_rlnc *fec4;
static struct tbt_fec_rlnc *fec8;
static u8 fec_data[FEC_L8][FEC_S];
static u8 fec_parity[FEC_L8][FEC_S];
static u8 *fec_data_p[FEC_L8];
static u8 *fec_parity_p[FEC_L8];

static void fec_fixture_init(void)
{
    unsigned int i;

    fec4 = tbt_fec_rlnc_new(4, 4, 4, 8, FEC_S, 4, 0x0123456789abcdefULL);
    fec8 = tbt_fec_rlnc_new(4, 4, 8, 8, FEC_S, 4, 0x0123456789abcdefULL);
    if (!fec4 || !fec8)
    {
        fprintf(stderr, "bench_micro: cannot create RLNC codecs\n");
        exit(1);
    }
    for (i = 0; i < FEC_L8; i++)
    {
        memset(fec_data[i], (int)(0x11 + i), FEC_S);
        memset(fec_parity[i], 0, FEC_S);
        fec_data_p[i] = fec_data[i];
        fec_parity_p[i] = fec_parity[i];
    }
}

static void run_fec_encode4(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_fec_rlnc_encode(fec4, fec_data_p, FEC_L4, fec_parity_p) < 0)
        {
            fprintf(stderr, "bench_micro: encode4 failed\n");
            exit(1);
        }
        *sum += fec_parity[0][0] + fec_parity[3][FEC_S - 1];
    }
}

static void run_fec_encode8(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_fec_rlnc_encode(fec8, fec_data_p, FEC_L8, fec_parity_p) < 0)
        {
            fprintf(stderr, "bench_micro: encode8 failed\n");
            exit(1);
        }
        *sum += fec_parity[0][0] + fec_parity[3][FEC_S - 1];
    }
}

/* One data share erased, every parity present. decode() resets its known
 * arrays on each call, so the same erased buffer can be decoded repeatedly;
 * the recovered bytes land in fec_data[0] after the first round, which does
 * not change the work the codec does. */
static u8 dec_dp[FEC_L8];
static u8 dec_pp[FEC_L8];

static void run_fec_decode4(unsigned int iters, u64 *sum)
{
    unsigned int i;

    memset(dec_dp, 1, FEC_L4);
    memset(dec_pp, 1, 4);
    dec_dp[0] = 0;
    for (i = 0; i < iters; i++)
    {
        int rc = tbt_fec_rlnc_decode(fec4, fec_data_p, fec_parity_p, dec_dp,
                                     dec_pp, FEC_L4);

        if (rc < 0)
        {
            fprintf(stderr, "bench_micro: decode4 failed rc=%d\n", rc);
            exit(1);
        }
        *sum += fec_data[0][0] + (u64)rc;
    }
}

static void run_fec_decode8(unsigned int iters, u64 *sum)
{
    unsigned int i;

    memset(dec_dp, 1, FEC_L8);
    memset(dec_pp, 1, 4);
    dec_dp[0] = 0;
    for (i = 0; i < iters; i++)
    {
        int rc = tbt_fec_rlnc_decode(fec8, fec_data_p, fec_parity_p, dec_dp,
                                     dec_pp, FEC_L8);

        if (rc < 0)
        {
            fprintf(stderr, "bench_micro: decode8 failed rc=%d\n", rc);
            exit(1);
        }
        *sum += fec_data[0][0] + (u64)rc;
    }
}

/* The production shape is 256 carriers as 64 groups of n4m4. One operation
 * encodes one group's block, which is what the TX worker does per group. */
#define GROUPED_N 64u

static struct tbt_fec_grouped *grouped;

static void grouped_init(void)
{
    grouped = tbt_fec_grouped_new(GROUPED_N, 4, 4, 4, 8, FEC_S, 4,
                                  0x0123456789abcdefULL);
    if (!grouped)
    {
        fprintf(stderr, "bench_micro: cannot create grouped codec\n");
        exit(1);
    }
}

static void run_fec_grouped(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        struct tbt_fec_rlnc *ctx = tbt_fec_grouped_ctx(grouped,
                                                       (u16)(i % GROUPED_N));

        if (!ctx || tbt_fec_rlnc_encode(ctx, fec_data_p, FEC_L4,
                                        fec_parity_p) < 0)
        {
            fprintf(stderr, "bench_micro: grouped encode failed\n");
            exit(1);
        }
        *sum += fec_parity[0][0];
    }
}

/* ------------------------------------------------------------------ *
 * FEC: 40-byte header seal + verify (linked)
 * ------------------------------------------------------------------ */

static struct tbt_fec_hdr hdr;
static u8 hdr_payload[FEC_S];

static void run_fec_hdr(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        hdr.payload_len = FEC_S;
        if (tbt_fec_hdr_seal(&hdr, hdr_payload) != 0 ||
            tbt_fec_hdr_verify(&hdr, hdr_payload) != 0)
        {
            fprintf(stderr, "bench_micro: header seal/verify failed\n");
            exit(1);
        }
        *sum += hdr.payload_crc32c;
    }
}

/* ------------------------------------------------------------------ *
 * FEC: replay ring (linked)
 * ------------------------------------------------------------------ */

#define REPLAY_S FEC_S
#define REPLAY_LEN (TBT_FEC_HDR_LEN + REPLAY_S)
#define REPLAY_GROUPS 64u
#define REPLAY_N 4u
#define REPLAY_M 4u

static struct tbt_fec_replay *replay;
static u8 replay_rec[REPLAY_LEN];
static u8 replay_out[REPLAY_LEN];

static void replay_init(void)
{
    unsigned int i;

    replay = tbt_fec_replay_new(REPLAY_GROUPS, REPLAY_N, REPLAY_M, REPLAY_S);
    if (!replay)
    {
        fprintf(stderr, "bench_micro: cannot create replay ring\n");
        exit(1);
    }
    for (i = 0; i < REPLAY_LEN; i++)
        replay_rec[i] = (u8)(i * 7u + 1u);
}

static void run_replay_store_get(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        u64 block = i;
        u16 group = (u16)(i % REPLAY_GROUPS);
        u16 share = (u16)(i % (REPLAY_N + REPLAY_M));
        int rc;

        rc = tbt_fec_replay_store(replay, block, group, share, replay_rec,
                                  REPLAY_LEN);
        if (rc != 0)
        {
            fprintf(stderr, "bench_micro: replay store failed rc=%d\n", rc);
            exit(1);
        }
        rc = tbt_fec_replay_get(replay, block, group, share, replay_out,
                                sizeof(replay_out));
        if (rc != REPLAY_LEN)
        {
            fprintf(stderr, "bench_micro: replay get failed rc=%d\n", rc);
            exit(1);
        }
        *sum += replay_out[1];
    }
}

/* The ring holds one block; asking for a share of that block that was never
 * stored is the common NACK miss (gen matches, block_id does not). */
static void run_replay_miss(unsigned int iters, u64 *sum)
{
    unsigned int i;

    if (tbt_fec_replay_store(replay, 0, 0, 0, replay_rec, REPLAY_LEN) != 0)
    {
        fprintf(stderr, "bench_micro: replay seed failed\n");
        exit(1);
    }
    for (i = 0; i < iters; i++)
    {
        int rc = tbt_fec_replay_get(replay, 0, 0, 1, replay_out,
                                    sizeof(replay_out));

        if (rc != -ENODATA)
        {
            fprintf(stderr, "bench_micro: replay miss rc=%d\n", rc);
            exit(1);
        }
        *sum += (u64)rc;
    }
}

/* ------------------------------------------------------------------ *
 * FEC: bitmap and NACK pacing (linked)
 * ------------------------------------------------------------------ */

static u8 bitmap[TBT_FEC_BITMAP_BYTES];

static void bitmap_init(void)
{
    memset(bitmap, 0xff, sizeof(bitmap));
}

static void run_bitmap_count(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
        *sum += tbt_fec_bitmap_count(bitmap);
}

static struct tbt_fec_nack_state nack_st;
static struct tbt_fec_nack nack_msg;
static u64 nack_now;
static u64 nack_block;

/* One new NACK per operation, ten milliseconds apart: the state machine
 * copies two 32-byte masks and stamps the block. This is the per-gap cost a
 * NACK scan pays after it found a hole. */
static void run_nack_due(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        nack_now += 10ULL * 1000000ULL;
        nack_block++;
        nack_msg.group_mask[0] = (u8)nack_block;
        if (!tbt_fec_nack_due_at(&nack_st, nack_now, nack_block, &nack_msg,
                                 100))
        {
            fprintf(stderr, "bench_micro: nack not due\n");
            exit(1);
        }
        *sum += (u64)nack_st.tries;
    }
}

/* ------------------------------------------------------------------ *
 * Crypto: body XOR (linked)
 * ------------------------------------------------------------------ */

#define CRYPTO_SMALL 1500u
#define CRYPTO_LARGE 8192u

static struct tbt_crypto_ctx crypto_ctx;
static u8 crypto_key[TBT_CRYPTO_KEY_LEN];
static u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN];
static u8 crypto_buf[CRYPTO_LARGE];

static void crypto_init(void)
{
    unsigned int i;

    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
        crypto_key[i] = (u8)(0x40 + i);
    for (i = 0; i < TBT_CRYPTO_NONCE_LEN; i++)
        crypto_nonce[i] = (u8)(0x80 + i);
    if (tbt_crypto_init(&crypto_ctx, crypto_key, crypto_nonce, 0) != 0)
    {
        fprintf(stderr, "bench_micro: crypto init failed\n");
        exit(1);
    }
    memset(crypto_buf, 0x5a, sizeof(crypto_buf));
}

static void run_crypto_small(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_crypto_body_xor(&crypto_ctx, (u64)i, false, crypto_buf,
                                CRYPTO_SMALL) != 0)
        {
            fprintf(stderr, "bench_micro: body_xor failed\n");
            exit(1);
        }
        *sum += crypto_buf[0] + crypto_buf[CRYPTO_SMALL - 1];
    }
}

static void run_crypto_large(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_crypto_body_xor(&crypto_ctx, (u64)i, false, crypto_buf,
                                CRYPTO_LARGE) != 0)
        {
            fprintf(stderr, "bench_micro: body_xor 8k failed\n");
            exit(1);
        }
        *sum += crypto_buf[0] + crypto_buf[CRYPTO_LARGE - 1];
    }
}

/* A parity share mixes share_index into the counter, which is the per-share
 * path the FEC data plane uses for every PARITY record. */
static void run_crypto_share(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_crypto_body_xor_share(&crypto_ctx, (u64)(i / 4), true,
                                      (u16)(i % 4), crypto_buf,
                                      CRYPTO_SMALL) != 0)
        {
            fprintf(stderr, "bench_micro: body_xor_share failed\n");
            exit(1);
        }
        *sum += crypto_buf[0] + crypto_buf[CRYPTO_SMALL - 1];
    }
}

/* ------------------------------------------------------------------ *
 * Scheduler (model of tbt_tx.c)
 *
 * Mirror of:
 *   tbt_path_inflight()   tbt_tx.c:2105
 *   tbt_sched_better()    tbt_tx.c:2196
 *   tbt_sched_rebuild()   tbt_tx.c:2213
 *   tbt_sched_pick()      tbt_tx.c:2258
 *   tbt_path_stalled_mask() tbt_tx.c:2128
 * The carrier pointers, atomics and TCP socket reads are replaced by plain
 * arrays with the same values; the comparison and the rotation are exact.
 * ------------------------------------------------------------------ */

#define SCHED_N 256u
#define SCHED_WINDOW 64u
/* tbt_path_stalled_mask() walks only the 16 legacy ARQ slots; FEC carriers
 * beyond 16 are paused by their own flag, not by this mask. */
#define SCHED_STALL_N 16u

static u32 sched_sent[SCHED_N];
static u32 sched_ack;
static u16 sched_rot;
static u8 sched_order[SCHED_N];
static u16 sched_n;
static u16 sched_win;
static DECLARE_BITMAP(sched_mask, SCHED_N);
static u32 sched_now_us;
static u32 sched_sort_us;
static u32 sched_last_una_move[SCHED_N];
static u8 sched_una_moved[SCHED_N];

static u32 model_inflight(u32 sent)
{
    if (sent <= sched_ack)
        return 0;
    return sent - sched_ack;
}

static bool model_better(u16 a, u16 b)
{
    u32 ia = model_inflight(sched_sent[a]);
    u32 ib = model_inflight(sched_sent[b]);

    if (ia != ib)
        return ia < ib;
    return ((a + sched_rot) % SCHED_N) < ((b + sched_rot) % SCHED_N);
}

static void model_rebuild(void)
{
    u16 i;
    u16 n = 0;
    u16 j;

    for (i = 0; i < SCHED_N; i++)
        sched_order[n++] = (u8)i;
    for (i = 1; i < n; i++)
    {
        u8 key = sched_order[i];

        for (j = i; j > 0 && model_better(key, sched_order[j - 1]); j--)
            sched_order[j] = sched_order[j - 1];
        sched_order[j] = key;
    }
    sched_n = n;
    sched_rot++;
    sched_sort_us = sched_now_us;
}

static void sched_init(void)
{
    u16 i;

    for (i = 0; i < SCHED_N; i++)
    {
        sched_sent[i] = (u32)((i * 2654435761u) % 65536u);
        sched_last_una_move[i] = (i & 1u) ? 50u : 0u;
        sched_una_moved[i] = (u8)(i & 1u);
    }
    sched_now_us = 100;
    model_rebuild();
}

/* Hot pick path: one masked lookup, a 64-frame window, and the rebuild the
 * kernel does when the window wraps or the sort is ten milliseconds old. */
static void run_sched_pick(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        u8 idx;

        sched_now_us++;
        if (sched_now_us - sched_sort_us > 10000)
            model_rebuild();
        if (sched_win >= SCHED_WINDOW)
        {
            sched_win = 0;
            model_rebuild();
        }
        idx = sched_order[sched_win % (sched_n ? sched_n : 1)];
        sched_win++;
        if (test_bit(idx, sched_mask))
            continue;
        *sum += idx;
    }
}

static void run_sched_rebuild(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        sched_sent[i % SCHED_N] += 1500;
        sched_ack += 1500;
        model_rebuild();
        *sum += sched_order[0];
    }
}

static void run_sched_stalled(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        u16 mask = 0;
        u16 c;

        sched_now_us += 10;
        for (c = 0; c < SCHED_STALL_N; c++)
        {
            u32 last = sched_last_una_move[c];

            if (!last || sched_now_us - last < 10)
                continue;
            if (sched_una_moved[c])
                mask |= (u16)(1u << (c % 16));
        }
        *sum += mask;
    }
}

/* ------------------------------------------------------------------ *
 * NACK scan (model of tbt_fec_data.c:tbt_fec_nack_scan, the bitmap walk)
 *
 * The kernel scan walks each group's block state, skips a ready block, and
 * for a block with holes walks its 256-bit present map to list the missing
 * shares. The model keeps 64 group blocks, each missing one share, and finds
 * the first missing share with the same bitmap primitives.
 * ------------------------------------------------------------------ */

#define NACK_GROUPS 64u
#define NACK_SHARES 8u

static u8 nack_bm[NACK_GROUPS][TBT_FEC_BITMAP_BYTES];

static void nack_scan_init(void)
{
    u16 g;
    u16 s;

    for (g = 0; g < NACK_GROUPS; g++)
    {
        for (s = 0; s < NACK_SHARES; s++)
        {
            if ((g + s) % 7u == 0)
                continue; /* one hole per block, rotating */
            tbt_fec_bitmap_set(nack_bm[g], s);
        }
    }
}

static void run_nack_scan(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        u16 g;

        for (g = 0; g < NACK_GROUPS; g++)
        {
            u16 s;

            if (tbt_fec_bitmap_count(nack_bm[g]) == NACK_SHARES)
                continue;
            for (s = 0; s < NACK_SHARES; s++)
            {
                if (!tbt_fec_bitmap_test(nack_bm[g], s))
                {
                    *sum += (u64)(g << 8 | s);
                    break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * Capture accounting (linked tbt_tx_acct.h)
 * ------------------------------------------------------------------ */

#define CAPTURE_CHUNK 8192u

static struct tbt_tx_acct acct;
static u8 acct_dup_pending;
static DECLARE_BITMAP(capture_paths, 256);
static u32 capture_path;

static void run_capture(unsigned int iters, u64 *sum)
{
    unsigned int i;

    for (i = 0; i < iters; i++)
    {
        u32 idx = capture_path++ & 255u;

        if (tbt_tx_acct_fits(acct.tx_queued, 1u << 22, CAPTURE_CHUNK, 1))
        {
            tbt_tx_acct_publish(&acct, CAPTURE_CHUNK, 1, &acct_dup_pending);
            set_bit(idx, capture_paths);
        }
        else
        {
            acct.tx_queued = 0;
            acct.retain_bytes = 0;
        }
        *sum += (u64)test_bit(idx, capture_paths) + acct_dup_pending;
    }
}

/* ------------------------------------------------------------------ *
 * Table
 * ------------------------------------------------------------------ */

static const u8 nack_seed = 0x5a;
static const u8 capture_seed = 0x01;

static u64 sched_witness(void)
{
    return sched_sent[SCHED_N - 1];
}

static u64 sched_stalled_witness(void)
{
    return sched_last_una_move[1];
}

static const struct bench benches[] = {
    {"fec.encode.n4m4.w8.k4.s1500", FEC_L4 * FEC_S, run_fec_encode4,
     fec_data[0], FEC_S, NULL},
    {"fec.encode.n4m4.w8.k8.s1500", FEC_L8 * FEC_S, run_fec_encode8,
     fec_data[7], FEC_S, NULL},
    {"fec.decode.n4m4.w8.k4.erase1.s1500", FEC_L4 * FEC_S, run_fec_decode4,
     fec_data[0], FEC_S, NULL},
    {"fec.decode.n4m4.w8.k8.erase1.s1500", FEC_L8 * FEC_S, run_fec_decode8,
     fec_data[7], FEC_S, NULL},
    {"fec.grouped64.encode_one_group.n4m4.w8.s1500", FEC_L4 * FEC_S,
     run_fec_grouped, fec_data[1], FEC_S, NULL},
    {"fec.hdr.seal_verify.s1500", FEC_S, run_fec_hdr, hdr_payload, FEC_S, NULL},
    {"fec.replay.store_get.s1500", 2u * REPLAY_LEN, run_replay_store_get,
     replay_rec, REPLAY_LEN, NULL},
    {"fec.replay.get_miss.never_stored", 0, run_replay_miss, replay_rec,
     REPLAY_LEN, NULL},
    {"fec.bitmap.count.256", 0, run_bitmap_count, bitmap, sizeof(bitmap), NULL},
    {"fec.nack.due.new", 0, run_nack_due, &nack_seed, 1, NULL},
    {"crypto.body_xor.1500", CRYPTO_SMALL, run_crypto_small, crypto_key,
     TBT_CRYPTO_KEY_LEN, NULL},
    {"crypto.body_xor.8192", CRYPTO_LARGE, run_crypto_large, crypto_key,
     TBT_CRYPTO_KEY_LEN, NULL},
    {"crypto.body_xor_share.parity.1500", CRYPTO_SMALL, run_crypto_share,
     crypto_key, TBT_CRYPTO_KEY_LEN, NULL},
    {"model.sched.pick.256", 0, run_sched_pick, NULL, 0, sched_witness},
    {"model.sched.rebuild.256", 0, run_sched_rebuild, NULL, 0, sched_witness},
    {"model.sched.stalled_mask.16", 0, run_sched_stalled, NULL, 0,
     sched_stalled_witness},
    {"model.nack.scan.64groups", 0, run_nack_scan, nack_bm, sizeof(nack_bm),
     NULL},
    {"capture.account.8k", 0, run_capture, &capture_seed, 1, NULL},
};

static void usage(FILE *f)
{
    fprintf(f,
            "usage: bench_micro [--quick|--full]\n"
            "  --quick  target <= 2 s (default)\n"
            "  --full   target <= 10 s\n"
            "CSV on stdout: name,iterations,ns_per_op,mb_per_s,checksum\n"
            "The human table goes to stderr.\n");
}

int main(int argc, char **argv)
{
    size_t i;
    double t0;

    for (i = 1; i < (size_t)argc; i++)
    {
        if (strcmp(argv[i], "--quick") == 0)
            quick_mode = 1;
        else if (strcmp(argv[i], "--full") == 0)
            quick_mode = 0;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "bench_micro: unknown argument %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    fec_fixture_init();
    grouped_init();
    bitmap_init();
    replay_init();
    crypto_init();
    sched_init();
    nack_scan_init();

    deadline = now_sec() + (quick_mode ? 1.8 : 9.0);
    t0 = now_sec();

    printf("name,iterations,ns_per_op,mb_per_s,checksum\n");
    fflush(stdout);

    for (i = 0; i < sizeof(benches) / sizeof(benches[0]); i++)
        measure(&benches[i]);

    fprintf(stderr, "bench_micro: %s, %.2f s\n",
            quick_mode ? "quick" : "full", now_sec() - t0);
    return 0;
}
