/* bench_sclc_pipe.c - full-chain host bench for the production SC-LDPC path.
 *
 * WHAT THIS MEASURES
 *   One "block set" per iteration is the production mode=sc-ldpc unit: 256
 *   carriers in both directions.
 *
 *     downlink  one binary SC-IRA block, n=128 data + 128 parity shares,
 *               block=256, L=8, rate 500/1000, share S = 8192 bytes.
 *     uplink    eight GF(16) SC-IRA blocks, n=8 data + 24 parity shares
 *               each, block=128, L=16, rate 64/256, share S = 8192 bytes.
 *
 *   TX runs the same calls as tbt_fec_make_record() in tbt_fec_data.c:
 *   FEC encode, header seal (payload CRC32C) and pack (header CRC32C),
 *   payload copy, then the ChaCha8 body transform through
 *   tbt_crypto_body_xor_flow().  RX runs the same calls as
 *   tbt_fec_rx_record(): header parse and CRCs, ChaCha8 open, payload
 *   CRC32C, then the block decode and one is_known() per data share.
 *
 *   The share erasure pattern is fixed per p and reused on every round, so
 *   repeated rounds do the same decode work.  p=0.0, 0.13 and 0.30 match
 *   the loss points the FEC reports use; the realized loss ratio is printed
 *   next to the requested one.
 *
 *   RX consumes the records TX just sealed, so the loopback uses one crypto
 *   nonce for both contexts; the nonce value does not change the work, and
 *   a real session keeps one nonce per direction.  No skb copy is charged
 *   to the RX stages: the record payload is opened in place, as
 *   tbt_fec_rx_record_body() does.
 *
 * OUTPUT
 *   CSV on stdout, one header line and one row per stage:
 *
 *     shape,direction,p,stage,records,ns_per_record,pct,mb_per_s,checksum
 *
 *   `records` is the population the stage processed: n+m for TX and for
 *   the FEC stages, the received share count for the RX record stages.
 *   `ns_per_record` divides the median stage time by that population.
 *   `pct` is the stage's share of the direction's median wall time.
 *   `mb_per_s` is payload MB/s (10^6 bytes/s): the FEC stages report
 *   application bytes per block set, the record stages report the record
 *   payload they transformed.  A human table goes to stderr.
 *
 *   The `checksum` column is FNV-1a over a fixed sample of the inputs and
 *   the final outputs, so two runs of the same binary agree.
 *
 *   make host-bench-sclc-pipe
 *   build/bin/bench_sclc_pipe --full
 *
 * TIME BUDGET
 *   --quick (default) aims at 0.25 s of measured time per (shape, p) point,
 *   --full at 1.0 s, with at least three rounds.  A round that is already
 *   slower than the budget is not repeated more than three times.  Pin with
 *   taskset and compare medians, never single runs.
 *
 * Not measured: skb allocation, copy_from_user, TCP, and the carrier pump.
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_fec_sclc.h"
#include "tbt_sclc_uplink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PIPE_S TBT_FEC_S
#define PIPE_REC (TBT_FEC_HDR_LEN + PIPE_S)
#define PIPE_SID 0x0123456789ULL
#define PIPE_BLOCK 7ULL

struct shape
{
    const char *name;
    const char *desc;
    const struct tbt_fec_ops *ops;
    u16 groups;
    u16 n;
    u16 m;
    u16 k; /* codec new(): rate_ppm for binary, block for GF(16) */
    u16 w; /* codec new(): block for binary, L for GF(16) */
    u16 d; /* 0 for binary, rate in 1/256 for GF(16) */
    u64 seed;
};

static const struct shape shapes[] = {
    {"downlink", "binary 1x256 L8 r0.50", &tbt_fec_ops_sclc, 1, 128, 128, 500,
     256, 0, 0x51c1a7e3ULL},
    {"uplink", "GF16 8x32 L16 r0.25", &tbt_fec_ops_sclc_gf16, 8, 8, 24, 128,
     16, 64, 0x9d2f5b11ULL},
};

/* One populated shape: both codec ends, the TX share buffers and the wire
 * records.  RX decrypts a record payload in place and hands that same
 * buffer to the decoder, exactly like tbt_fec_rx_record_body(); the next
 * TX round writes fresh ciphertext over it. */
struct pipe
{
    const struct shape *sh;
    struct tbt_fec_grouped *enc;
    struct tbt_fec_grouped *dec;
    u16 records;
    u16 per_group;
    u8 *tx_plain;
    u8 *wire;
    u8 **tx_share;
    u8 **rx_share;
    u8 **wire_rec;
    u8 **tx_data;
    u8 **tx_par;
    u8 **rx_data;
    u8 **rx_par;
    u8 *present;
    u16 lost;
    u32 known_last;
};

enum
{
    TX_ENC,
    TX_HDR,
    TX_COPY,
    TX_CRYPTO,
    TX_TOTAL,
    TX_NSTAGE
};

enum
{
    RX_PARSE,
    RX_CRYPTO,
    RX_CRC,
    RX_DECODE,
    RX_TOTAL,
    RX_NSTAGE
};

#define MAX_ROUNDS 128

static int quick_mode = 1;
static u64 rng_state = 1;

static u64 now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static u64 rnd64(void)
{
    u64 z = (rng_state += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

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

static void die(const char *what)
{
    fprintf(stderr, "bench_sclc_pipe: %s\n", what);
    exit(1);
}

static void *xcalloc(size_t n, size_t len)
{
    void *p = calloc(n ? n : 1, len ? len : 1);

    if (!p)
        die("out of memory");
    return p;
}

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a;
    u64 y = *(const u64 *)b;

    return x < y ? -1 : x > y;
}

static u64 median_u64(u64 *v, int n)
{
    qsort(v, (size_t)n, sizeof(v[0]), cmp_u64);
    return v[n / 2];
}

static void pipe_init(struct pipe *p, const struct shape *sh)
{
    u16 g;
    u16 j;
    u16 s;

    memset(p, 0, sizeof(*p));
    p->sh = sh;
    p->per_group = (u16)(sh->n + sh->m);
    p->records = (u16)(sh->groups * p->per_group);

    p->enc = tbt_fec_grouped_new_ops(sh->ops, sh->groups, sh->n, sh->m, sh->k,
                                     sh->w, PIPE_S, sh->d, sh->seed);
    p->dec = tbt_fec_grouped_new_ops(sh->ops, sh->groups, sh->n, sh->m, sh->k,
                                     sh->w, PIPE_S, sh->d, sh->seed);
    if (!p->enc || !p->dec)
        die("cannot build the codec pair");

    p->tx_plain = xcalloc(p->records, PIPE_S);
    p->wire = xcalloc(p->records, PIPE_REC);
    p->tx_share = xcalloc(p->records, sizeof(*p->tx_share));
    p->rx_share = xcalloc(p->records, sizeof(*p->rx_share));
    p->wire_rec = xcalloc(p->records, sizeof(*p->wire_rec));
    p->tx_data = xcalloc((size_t)sh->groups * sh->n, sizeof(*p->tx_data));
    p->tx_par = xcalloc((size_t)sh->groups * sh->m, sizeof(*p->tx_par));
    p->rx_data = xcalloc((size_t)sh->groups * sh->n, sizeof(*p->rx_data));
    p->rx_par = xcalloc((size_t)sh->groups * sh->m, sizeof(*p->rx_par));
    p->present = xcalloc(p->records, 1);

    for (s = 0; s < p->records; s++)
    {
        p->tx_share[s] = p->tx_plain + (size_t)s * PIPE_S;
        p->wire_rec[s] = p->wire + (size_t)s * PIPE_REC;
        /* RX works on the record payload in place. */
        p->rx_share[s] = p->wire_rec[s] + TBT_FEC_HDR_LEN;
    }
    for (g = 0; g < sh->groups; g++)
    {
        for (j = 0; j < sh->n; j++)
        {
            u16 idx = (u16)(g * p->per_group + j);

            p->tx_data[(size_t)g * sh->n + j] = p->tx_share[idx];
            p->rx_data[(size_t)g * sh->n + j] = p->rx_share[idx];
        }
        for (j = 0; j < sh->m; j++)
        {
            u16 idx = (u16)(g * p->per_group + sh->n + j);

            p->tx_par[(size_t)g * sh->m + j] = p->tx_share[idx];
            p->rx_par[(size_t)g * sh->m + j] = p->rx_share[idx];
        }
    }

    /* A share-sized pattern per data share; parity starts zero and is
     * rewritten by every encode. */
    for (g = 0; g < sh->groups; g++)
    {
        for (j = 0; j < sh->n; j++)
        {
            u8 *buf = p->tx_data[(size_t)g * sh->n + j];
            u32 jj;

            for (jj = 0; jj < PIPE_S; jj++)
                buf[jj] = (u8)(rnd64() >> 24);
        }
    }
}

static void pipe_free(struct pipe *p)
{
    tbt_fec_grouped_free(p->enc);
    tbt_fec_grouped_free(p->dec);
    free(p->tx_plain);
    free(p->wire);
    free(p->tx_share);
    free(p->rx_share);
    free(p->wire_rec);
    free(p->tx_data);
    free(p->tx_par);
    free(p->rx_data);
    free(p->rx_par);
    free(p->present);
    memset(p, 0, sizeof(*p));
}

static void make_mask(struct pipe *p, u32 p_ppm, u64 seed)
{
    u16 s;

    rng_state = seed;
    p->lost = 0;
    for (s = 0; s < p->records; s++)
    {
        int lost = p_ppm && (u32)(rnd64() % 1000000ull) < p_ppm;

        p->present[s] = (u8)!lost;
        p->lost = (u16)(p->lost + (lost ? 1 : 0));
    }
}

static void hdr_build(struct tbt_fec_hdr *h, const struct pipe *p, u16 within,
                      u64 seq)
{
    memset(h, 0, sizeof(*h));
    h->type = within < p->sh->n ? TBT_FEC_DATA : TBT_FEC_PARITY;
    h->share_index = within;
    h->n = (u8)p->sh->n;
    h->m = (u8)p->sh->m;
    h->block_id = PIPE_BLOCK;
    h->seq = seq;
    h->payload_len = PIPE_S;
    h->conv = TBT_FEC_CONV;
    h->sid = PIPE_SID;
    h->win_len = h->type == TBT_FEC_PARITY ? p->sh->n : 0;
    h->epoch = 0;
}

static void tx_round(struct pipe *p, struct tbt_crypto_ctx *crypto, u64 *acc)
{
    const struct shape *sh = p->sh;
    u64 t0 = now_ns();
    u16 g;
    u16 j;

    for (g = 0; g < sh->groups; g++)
    {
        u64 t = now_ns();

        if (tbt_fec_grouped_encode(p->enc, g,
                                   p->tx_data + (size_t)g * sh->n, sh->n,
                                   p->tx_par + (size_t)g * sh->m) != 0)
            die("encode failed");
        acc[TX_ENC] += now_ns() - t;
    }

    for (g = 0; g < sh->groups; g++)
    {
        for (j = 0; j < p->per_group; j++)
        {
            u16 global = (u16)(g * p->per_group + j);
            int parity = j >= sh->n;
            u16 within = parity ? (u16)(j - sh->n) : j;
            u64 seq = tbt_fec_seq_of_block_pkt(
                PIPE_BLOCK, g, parity ? 0 : within, sh->groups, sh->n);
            struct tbt_fec_hdr h;
            u64 t;

            hdr_build(&h, p, j, seq);

            t = now_ns();
            if (tbt_fec_hdr_seal(&h, p->tx_share[global]) != 0 ||
                tbt_fec_hdr_pack(&h, p->wire_rec[global]) != 0)
                die("header seal/pack failed");
            acc[TX_HDR] += now_ns() - t;

            t = now_ns();
            memcpy(p->wire_rec[global] + TBT_FEC_HDR_LEN,
                   p->tx_share[global], PIPE_S);
            acc[TX_COPY] += now_ns() - t;

            t = now_ns();
            if (tbt_crypto_body_xor_flow(
                    crypto, PIPE_SID, seq, parity != 0, j,
                    p->wire_rec[global] + TBT_FEC_HDR_LEN, PIPE_S) != 0)
                die("tx crypto failed");
            acc[TX_CRYPTO] += now_ns() - t;
        }
    }
    acc[TX_TOTAL] += now_ns() - t0;
}

static void rx_round(struct pipe *p, struct tbt_crypto_ctx *crypto, u64 *acc,
                     u64 *known)
{
    const struct shape *sh = p->sh;
    u64 t0;
    u16 g;
    u16 j;
    u16 s;
    u32 known_round = 0;

    /* An absent share enters the decoder as a zero slot; a recovered share
     * is rewritten by the next decode. */
    for (s = 0; s < p->records; s++)
        if (!p->present[s])
            memset(p->rx_share[s], 0, PIPE_S);

    t0 = now_ns();
    for (s = 0; s < p->records; s++)
    {
        struct tbt_fec_hdr h;
        u8 hb[TBT_FEC_HDR_LEN];
        u32 want;
        u64 t;

        if (!p->present[s])
            continue;

        t = now_ns();
        if (tbt_fec_hdr_unpack(p->wire_rec[s], &h) != 0 ||
            tbt_fec_hdr_check(&h, PIPE_S) != 0)
            die("rx header check failed");
        if (tbt_fec_hdr_pack(&h, hb) != 0)
            die("rx header repack failed");
        want = tbt_fec_crc32c(0, hb, TBT_FEC_H_HDRCRC);
        if (want != h.header_crc32c)
            die("rx header crc failed");
        acc[RX_PARSE] += now_ns() - t;

        t = now_ns();
        if (tbt_crypto_body_xor_flow(crypto, h.sid, h.seq,
                                     h.type == TBT_FEC_PARITY,
                                     h.share_index, p->rx_share[s],
                                     PIPE_S) != 0)
            die("rx crypto failed");
        acc[RX_CRYPTO] += now_ns() - t;

        t = now_ns();
        if (tbt_fec_crc32c(0, p->rx_share[s], PIPE_S) != h.payload_crc32c)
            die("rx payload crc failed");
        acc[RX_CRC] += now_ns() - t;
    }

    for (g = 0; g < sh->groups; g++)
    {
        u32 got;
        u64 t = now_ns();
        int rc = tbt_fec_grouped_decode(
            p->dec, g, p->rx_data + (size_t)g * sh->n,
            p->rx_par + (size_t)g * sh->m,
            p->present + (size_t)g * p->per_group,
            p->present + (size_t)g * p->per_group + sh->n, sh->n);

        if (rc < 0 && rc != -EAGAIN)
            die("decode failed");
        for (j = 0; j < sh->n; j++)
        {
            got = (u32)tbt_fec_grouped_is_known(p->dec, g, j);
            known_round += got;
        }
        acc[RX_DECODE] += now_ns() - t;
    }
    p->known_last = known_round;
    *known = known_round;
    acc[RX_TOTAL] += now_ns() - t0;
}

static void report(const struct shape *sh, double p, const u64 *tx_med,
                   const u64 *rx_med, u16 records,
                   u16 present, u32 known, u64 app_bytes, u64 tx_cs,
                   u64 rx_cs)
{
    static const char *const tx_names[] = {"fec.encode", "hdr.seal_pack",
                                           "payload.copy", "chacha8.seal"};
    static const char *const rx_names[] = {"hdr.parse", "chacha8.open",
                                           "payload.crc", "fec.decode"};
    u64 tx_sum = 0;
    u64 rx_sum = 0;
    int i;

    fprintf(stderr,
            "\n%s: %s, p=%.3f (lost %u/%u shares, data known %u/%u)\n",
            sh->name, sh->desc, p, (unsigned)(records - present),
            (unsigned)records, (unsigned)known,
            (unsigned)(sh->groups * sh->n));
    fprintf(stderr, "  %-3s %-14s %8s %12s %7s %10s\n", "dir", "stage",
            "records", "ns/record", "%", "MB/s");

    printf("%s,tx,%.3f,fec.encode,%u,%.3f,%.2f,%.3f,%016llx\n", sh->name, p,
           (unsigned)records, (double)tx_med[TX_ENC] / (double)records,
           100.0 * (double)tx_med[TX_ENC] / (double)tx_med[TX_TOTAL],
           (double)app_bytes / ((double)tx_med[TX_ENC] * 1e-9) / 1e6,
           (unsigned long long)tx_cs);
    fprintf(stderr, "  tx  %-14s %8u %12.3f %7.2f %10.3f\n", tx_names[0],
            (unsigned)records, (double)tx_med[TX_ENC] / (double)records,
            100.0 * (double)tx_med[TX_ENC] / (double)tx_med[TX_TOTAL],
            (double)app_bytes / ((double)tx_med[TX_ENC] * 1e-9) / 1e6);

    for (i = 1; i < TX_TOTAL; i++)
    {
        double mb = (double)records * PIPE_S / ((double)tx_med[i] * 1e-9) / 1e6;

        printf("%s,tx,%.3f,%s,%u,%.3f,%.2f,%.3f,%016llx\n", sh->name, p,
               tx_names[i], (unsigned)records,
               (double)tx_med[i] / (double)records,
               100.0 * (double)tx_med[i] / (double)tx_med[TX_TOTAL], mb,
               (unsigned long long)tx_cs);
        fprintf(stderr, "  tx  %-14s %8u %12.3f %7.2f %10.3f\n", tx_names[i],
                (unsigned)records, (double)tx_med[i] / (double)records,
                100.0 * (double)tx_med[i] / (double)tx_med[TX_TOTAL], mb);
        tx_sum += tx_med[i];
    }
    tx_sum += tx_med[TX_ENC];
    printf("%s,tx,%.3f,total,%u,%.3f,100.00,%.3f,%016llx\n", sh->name, p,
           (unsigned)records, (double)tx_med[TX_TOTAL] / (double)records,
           (double)app_bytes / ((double)tx_med[TX_TOTAL] * 1e-9) / 1e6,
           (unsigned long long)tx_cs);
    fprintf(stderr, "  tx  %-14s %8u %12.3f %7.2f %10.3f\n", "total",
            (unsigned)records, (double)tx_med[TX_TOTAL] / (double)records,
            100.0, (double)app_bytes / ((double)tx_med[TX_TOTAL] * 1e-9) / 1e6);
    if (tx_med[TX_TOTAL] > tx_sum)
        fprintf(stderr, "  tx  %-14s %8u %12.3f %7.2f\n", "other",
                (unsigned)records,
                (double)(tx_med[TX_TOTAL] - tx_sum) / (double)records,
                100.0 * (double)(tx_med[TX_TOTAL] - tx_sum) /
                    (double)tx_med[TX_TOTAL]);

    for (i = 0; i < RX_TOTAL; i++)
    {
        u64 pop = i == RX_DECODE ? records : present;
        double mb = (double)pop * PIPE_S / ((double)rx_med[i] * 1e-9) / 1e6;

        if (i == RX_DECODE)
            mb = (double)app_bytes / ((double)rx_med[i] * 1e-9) / 1e6;
        printf("%s,rx,%.3f,%s,%u,%.3f,%.2f,%.3f,%016llx\n", sh->name, p,
               rx_names[i], (unsigned)pop,
               (double)rx_med[i] / (double)pop,
               100.0 * (double)rx_med[i] / (double)rx_med[RX_TOTAL], mb,
               (unsigned long long)rx_cs);
        fprintf(stderr, "  rx  %-14s %8u %12.3f %7.2f %10.3f\n", rx_names[i],
                (unsigned)pop, (double)rx_med[i] / (double)pop,
                100.0 * (double)rx_med[i] / (double)rx_med[RX_TOTAL], mb);
        rx_sum += rx_med[i];
    }
    printf("%s,rx,%.3f,total,%u,%.3f,100.00,%.3f,%016llx\n", sh->name, p,
           (unsigned)records, (double)rx_med[RX_TOTAL] / (double)records,
           (double)app_bytes / ((double)rx_med[RX_TOTAL] * 1e-9) / 1e6,
           (unsigned long long)rx_cs);
    fprintf(stderr, "  rx  %-14s %8u %12.3f %7.2f %10.3f\n", "total",
            (unsigned)records, (double)rx_med[RX_TOTAL] / (double)records,
            100.0, (double)app_bytes / ((double)rx_med[RX_TOTAL] * 1e-9) / 1e6);
    if (rx_med[RX_TOTAL] > rx_sum)
        fprintf(stderr, "  rx  %-14s %8u %12.3f %7.2f\n", "other",
                (unsigned)records,
                (double)(rx_med[RX_TOTAL] - rx_sum) / (double)records,
                100.0 * (double)(rx_med[RX_TOTAL] - rx_sum) /
                    (double)rx_med[RX_TOTAL]);
}

static void run_point(struct pipe *p, size_t shape_idx, u32 p_ppm,
                      double loss)
{
    struct tbt_crypto_ctx tx_crypto;
    struct tbt_crypto_ctx rx_crypto;
    u8 key[TBT_CRYPTO_KEY_LEN];
    u8 nonce[TBT_CRYPTO_NONCE_LEN];
    u64 tx_samples[MAX_ROUNDS][TX_NSTAGE];
    u64 rx_samples[MAX_ROUNDS][RX_NSTAGE];
    u64 tx_med[TX_NSTAGE];
    u64 rx_med[RX_NSTAGE];
    u64 tx_acc[TX_NSTAGE];
    u64 rx_acc[RX_NSTAGE];
    u64 known = 0;
    u64 app_bytes;
    u64 tx_cs;
    u64 rx_cs;
    double target = quick_mode ? 0.25 : 1.0;
    double t_start;
    int rounds = 0;
    int i;
    u16 g;
    u16 j;

    make_mask(p, p_ppm, 0x9E3779B97F4A7C15ULL ^
                            ((u64)shape_idx << 32) ^ (u64)p_ppm);

    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
        key[i] = (u8)(0x40 + i);
    for (i = 0; i < TBT_CRYPTO_NONCE_LEN; i++)
        nonce[i] = (u8)(0x80 + i);
    if (tbt_crypto_init(&tx_crypto, key, nonce, 0) != 0 ||
        tbt_crypto_init(&rx_crypto, key, nonce, 0) != 0)
        die("crypto init failed");

    memset(tx_acc, 0, sizeof(tx_acc));
    memset(rx_acc, 0, sizeof(rx_acc));

    /* One untimed round fills the records and the codec buffers. */
    tx_round(p, &tx_crypto, tx_acc);
    rx_round(p, &rx_crypto, rx_acc, &known);

    t_start = (double)now_ns() / 1e9;
    for (;;)
    {
        memset(tx_acc, 0, sizeof(tx_acc));
        memset(rx_acc, 0, sizeof(rx_acc));
        tx_round(p, &tx_crypto, tx_acc);
        rx_round(p, &rx_crypto, rx_acc, &known);
        for (i = 0; i < TX_NSTAGE; i++)
            tx_samples[rounds][i] = tx_acc[i];
        for (i = 0; i < RX_NSTAGE; i++)
            rx_samples[rounds][i] = rx_acc[i];
        rounds++;
        /* Three rounds give a median; a round already longer than the
         * budget is not repeated further. */
        if (rounds >= 3 &&
            ((double)now_ns() / 1e9 - t_start >= target ||
             rounds >= MAX_ROUNDS))
            break;
    }

    for (i = 0; i < TX_NSTAGE; i++)
    {
        u64 col[MAX_ROUNDS];
        int r;

        for (r = 0; r < rounds; r++)
            col[r] = tx_samples[r][i];
        tx_med[i] = median_u64(col, rounds);
    }
    for (i = 0; i < RX_NSTAGE; i++)
    {
        u64 col[MAX_ROUNDS];
        int r;

        for (r = 0; r < rounds; r++)
            col[r] = rx_samples[r][i];
        rx_med[i] = median_u64(col, rounds);
    }

    app_bytes = (u64)p->sh->groups * p->sh->n * PIPE_S;

    /* A fixed sample of the final state: deterministic for a fixed mask. */
    tx_cs = fnv1a(p->wire_rec[0], PIPE_REC);
    tx_cs ^= fnv1a(p->tx_share[p->records - 1], 64);
    rx_cs = fnv1a(p->rx_share[0], 64);
    for (g = 0; g < p->sh->groups; g++)
        for (j = 0; j < p->sh->n; j++)
            rx_cs ^= fnv1a(p->rx_data[(size_t)g * p->sh->n + j], 16);

    report(p->sh, loss, tx_med, rx_med, p->records,
           (u16)(p->records - p->lost), p->known_last, app_bytes, tx_cs,
           rx_cs);
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: bench_sclc_pipe [--quick|--full]\n"
            "  --quick  ~0.25 s per shape and p (default)\n"
            "  --full   ~1.0 s per shape and p\n"
            "CSV on stdout, human table on stderr.\n");
}

int main(int argc, char **argv)
{
    static const u32 ps[] = {0, 130000, 300000};
    size_t si;
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
            fprintf(stderr, "bench_sclc_pipe: unknown argument %s\n",
                    argv[i]);
            usage(stderr);
            return 2;
        }
    }

    printf("shape,direction,p,stage,records,ns_per_record,pct,mb_per_s,"
           "checksum\n");
    fflush(stdout);

    t0 = (double)now_ns() / 1e9;
    for (si = 0; si < sizeof(shapes) / sizeof(shapes[0]); si++)
    {
        struct pipe p;

        pipe_init(&p, &shapes[si]);
        for (i = 0; i < sizeof(ps) / sizeof(ps[0]); i++)
            run_point(&p, si, ps[i], (double)ps[i] / 1000000.0);
        pipe_free(&p);
    }
    fprintf(stderr, "\nbench_sclc_pipe: %s, %.2f s\n",
            quick_mode ? "quick" : "full", (double)now_ns() / 1e9 - t0);
    return 0;
}
