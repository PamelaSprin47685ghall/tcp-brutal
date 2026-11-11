/* bench_rx_stages.c - per-record cost of the tbtun FEC RX chain on the host.
 *
 * WHAT THIS MEASURES
 *   The stages tbtun runs on one received FEC share, in the order the module
 *   runs them, each against the real function the module links:
 *
 *     http_chunk_decode     tbt_http_fec_frame_decode(): the chunked HTTP
 *                           framing (size line, body copy into rx->rec, CRLF,
 *                           record validation) up to "rec is available".
 *                           Fed the steady-state buffer the carrier loop
 *                           hands it: the previous chunk's trailing CRLF
 *                           followed by the next complete frame, one call
 *                           per record. The memmove() of the remaining
 *                           carrier bytes that tbt_rx.c runs after the call
 *                           is outside this row.
 *     fec_hdr_unpack_check  tbt_fec_hdr_unpack() + tbt_fec_hdr_check():
 *                           the structural gate before the CRC (conv, type,
 *                           lengths, share bounds).
 *     hdr_crc32c            tbt_fec_hdr_pack() + tbt_fec_crc32c(0, hb, 44):
 *                           header_crc32c covers +0..+43, exactly the RX
 *                           check at tbt_fec_data.c:rx_record.
 *     crypto_chacha8        tbt_crypto_body_xor_flow(): the RX decrypt of an
 *                           8192-byte DATA payload, per-share counter form.
 *     payload_crc32c        tbt_fec_crc32c(0, payload, 8192): the CRC over
 *                           the clear payload, after decrypt.
 *     fec_slot_place.model  the decision + store path of
 *                           tbt_fec_data.c:rx_record:
 *                             tbt_fec_rx_window_step()
 *                             tbt_fec_arrival_kind()
 *                             tbt_fec_share_past_cut()
 *                             memcpy into b->data + share * S
 *                             dpresent/dlen/ndata bookkeeping
 *                           Those predicates are real inline functions from
 *                           tbt_fec.h; the surrounding function is
 *                           kernel-only (spin_lock_bh, atomics, session), so
 *                           this row is a model, marked `.model` like
 *                           bench_micro.c's kernel-loop rows.
 *     fec_deliver.model     the ordered-delivery inner loop of
 *                           tbt_fec_data.c:tbt_fec_deliver_ready():
 *                             tbt_fec_blk_deliverable()
 *                             tbt_fec_rx_share_len()
 *                             tbt_fec_deliver_step()
 *                             memcpy from the block slot to the stream
 *                           The stream socket write (tbt_rx_fec_deliver,
 *                           skb/queue) and the per-super-block ACK are
 *                           kernel-only and are NOT in this row.
 *
 * THE RECORD
 *   S = TBT_FEC_S = TBT_FEC_PAYLOAD_MAX = 8192 bytes; the FEC record is
 *   TBT_FEC_HDR_LEN + S = 8240 bytes, and the chunked frame is 8248 bytes
 *   ("2030\r\n" + 8240 + "\r\n"). This is the module's only packet size:
 *   tbt_fec.h defines TBT_FEC_S as TBT_FEC_PAYLOAD_MAX, and TBT_FEC_REC_MAX
 *   is that header plus that slot. A 1200/1400-byte record is not a shape
 *   this cut encodes.
 *   Shape for the slot rows: n=4, m=4, groups=32 (the production grouped
 *   profile, 32 groups x 8 shares), slots=8; the per-share work does not
 *   depend on n/m beyond the group/slot arithmetic.
 *
 * REQUIREMENTS
 *   Each stage runs >= 200000 iterations per round, 5 measured rounds after
 *   one warm-up round, and the reported value is the median of the round
 *   means. One round moves 200000 x 8192 B = 1.53 GiB and the five rounds
 *   give 1M operations per row, so both ">= 200k ops" and ">= 1 GiB" hold
 *   for every stage.
 *
 * OUTPUT
 *   CSV on stdout, one row per stage:
 *     stage,ops,ns_per_pkt,pct_chain,mb_per_s,checksum
 *   `ns_per_pkt` is per record (per share), `pct_chain` is this stage's share
 *   of the measured chain, and `mb_per_s` is 8192 payload bytes per second
 *   (10^6 bytes/s) for every row, so the rows compare. Leading `#` lines
 *   carry the CPU, compiler and build command; trailing `#` lines carry the
 *   five per-round means of every stage (`# samples.<stage>=...`) and the
 *   chain total.
 *
 * LIMITS (stated, not measured)
 *   - The host CRC32C is the table fallback in tbt_fec.c; the module calls
 *     the kernel crc32c(), which on x86_64 uses SSE4.2. The two CRC rows are
 *     therefore an upper bound on the target, not the target cost.
 *   - The model rows run no spin_lock_bh, no atomic64 counters, no
 *     cond_resched and no skb queue: they are a lower bound on the kernel
 *     cost of those stages.
 *   - malloc/calloc is faster than kvzalloc/kvfree; no row allocates per
 *     record, so this does not affect the numbers except as a floor.
 *   - The codec decode of a block (RLNC/SC-LDPC), NACK build/scan and the
 *     ACK pack happen per block or per super-block, not per share, and are
 *     out of scope here.
 *
 * BUILD
 *   cc -std=gnu99 -O2 -Wall -Wextra -Werror -I tbtun -o tests/bench/bench_rx_stages \
 *      tests/bench/bench_rx_stages.c src/kernel/tbt_http.c src/kernel/tbt_fec.c \
 *      src/kernel/tbt_crypto.c src/kernel/tbt_proto.c src/kernel/tbt_rec.c
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_http.h"
#include "tbt_uapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef BENCH_BUILD_CMD
#define BENCH_BUILD_CMD                                                                        \
    "cc -std=gnu99 -O2 -Wall -Wextra -Werror -I tbtun -o tests/bench/bench_rx_stages "                \
    "tests/bench/bench_rx_stages.c src/kernel/tbt_http.c src/kernel/tbt_fec.c src/kernel/tbt_crypto.c "              \
    "src/kernel/tbt_proto.c src/kernel/tbt_rec.c"
#endif

#define S_BYTES TBT_FEC_S /* 8192 */
#define REC_BYTES TBT_FEC_REC_MAX /* 8240 */
#define ROUNDS 5
#define ITERS_DEFAULT 200000u

/* Production grouped shape and its RX arena dimensions. */
#define BN 4u
#define BM 4u
#define BGROUPS 32u
#define BSLOTS 8u

static u8 g_rec[REC_BYTES];
static u8 g_payload[S_BYTES];
static u8 g_frame[TBT_HTTP_FEC_FRAME_MAX];
static size_t g_frame_len;
static u8 g_stream[2 + TBT_HTTP_FEC_FRAME_MAX]; /* "\r\n" + next frame */
static u8 g_crypto_buf[S_BYTES];
static struct tbt_crypto_ctx g_crypto_rx;
static struct tbt_fec_hdr g_hdr;
static u8 g_hdr_wire[TBT_FEC_HDR_LEN];

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

static const char *cpu_model(void)
{
    static char model[128];
    FILE *f;
    char line[256];

    f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return "unknown";
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "model name", 10) == 0)
        {
            const char *colon = strchr(line, ':');

            if (colon)
            {
                const char *v = colon + 1;
                size_t n;

                while (*v == ' ' || *v == '\t')
                    v++;
                n = strlen(v);
                while (n && (v[n - 1] == '\n' || v[n - 1] == ' '))
                    n--;
                if (n >= sizeof(model))
                    n = sizeof(model) - 1;
                memcpy(model, v, n);
                model[n] = '\0';
            }
            break;
        }
    }
    fclose(f);
    return model[0] ? model : "unknown";
}

/* ------------------------------------------------------------------ *
 * Fixture: one DATA share of the production shape, sealed and framed.
 * ------------------------------------------------------------------ */

static void fixture_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(g_payload); i++)
        g_payload[i] = (u8)(i * 31u + 7u);
    memset(&g_hdr, 0, sizeof(g_hdr));
    g_hdr.type = TBT_FEC_DATA;
    g_hdr.share_index = 0;
    g_hdr.n = BN;
    g_hdr.m = BM;
    g_hdr.block_id = 1;
    g_hdr.seq = 1;
    g_hdr.win_len = 0;
    g_hdr.payload_len = S_BYTES;
    g_hdr.conv = TBT_FEC_CONV;
    g_hdr.sid = 0x123456789aULL;

    /* Seal signs the header and the clear payload, exactly as TX does. */
    if (tbt_fec_hdr_seal(&g_hdr, g_payload) != 0 ||
        tbt_fec_hdr_pack(&g_hdr, g_rec) != 0)
    {
        fprintf(stderr, "bench: cannot seal the fixture header\n");
        exit(1);
    }
    memcpy(g_rec + TBT_FEC_HDR_LEN, g_payload, S_BYTES);

    /* The chunk the carrier would put on the wire; encode is a host API. */
    g_frame_len = tbt_http_fec_chunk_encode((char *)g_frame, sizeof(g_frame),
                                            g_rec, REC_BYTES);
    if (g_frame_len != REC_BYTES + 8)
    {
        fprintf(stderr, "bench: unexpected frame length %zu\n", g_frame_len);
        exit(1);
    }
    g_stream[0] = '\r';
    g_stream[1] = '\n';
    memcpy(g_stream + 2, g_frame, g_frame_len);

    memcpy(g_crypto_buf, g_payload, sizeof(g_crypto_buf));
    {
        u8 key[TBT_CRYPTO_KEY_LEN];
        u8 nonce[TBT_CRYPTO_NONCE_LEN];

        for (i = 0; i < sizeof(key); i++)
            key[i] = (u8)(i * 7 + 1);
        for (i = 0; i < sizeof(nonce); i++)
            nonce[i] = (u8)(0xa0 + i);
        if (tbt_crypto_init(&g_crypto_rx, key, nonce, 0) != 0)
        {
            fprintf(stderr, "bench: crypto init failed\n");
            exit(1);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Stage 1: chunked HTTP framing to a usable record.
 *
 * The first call starts in CHUNK_SIZE and consumes g_frame; every measured
 * call starts at the previous frame's CHUNK_DATA_END, consumes the two CRLF
 * bytes, then the next size line and body, and returns with one record. That
 * is exactly the steady state of tbt_rx.c, which leaves the CRLF in p->buf
 * and lets the next iteration consume it.
 * ------------------------------------------------------------------ */

static void run_http_decode(unsigned iters, u64 *sum)
{
    static struct tbt_http_rx rx;
    unsigned i;

    memset(&rx, 0, sizeof(rx));
    tbt_http_chunk_reset(&rx.chunk);
    {
        const u8 *rec = NULL;
        size_t rlen = 0;
        int err = TBT_RX_NEED_MORE;
        size_t used = tbt_http_fec_frame_decode(&rx, g_frame, g_frame_len, &rec,
                                                &rlen, &err);

        if (err != TBT_RX_OK || used != g_frame_len - 2 || rec != rx.rec ||
            rlen != REC_BYTES || memcmp(rec, g_rec, REC_BYTES) != 0)
        {
            fprintf(stderr, "bench: prime decode failed (used=%zu err=%d)\n",
                    used, err);
            exit(1);
        }
    }
    for (i = 0; i < iters; i++)
    {
        const u8 *rec = NULL;
        size_t rlen = 0;
        int err = TBT_RX_NEED_MORE;
        size_t used;

        used = tbt_http_fec_frame_decode(&rx, g_stream, 2 + g_frame_len, &rec,
                                         &rlen, &err);
        if (err != TBT_RX_OK || used != 2 + g_frame_len - 2 ||
            rec != rx.rec || rlen != REC_BYTES)
        {
            fprintf(stderr, "bench: chunk decode failed at %u (used=%zu err=%d)\n",
                    i, used, err);
            exit(1);
        }
        *sum += (u64)rec[0] + (u64)rlen + (u64)rec[TBT_FEC_HDR_LEN];
    }
}

/* ------------------------------------------------------------------ *
 * Stage 2: header unpack + structural check (the pre-CRC gate).
 * ------------------------------------------------------------------ */

static void run_hdr_unpack_check(unsigned iters, u64 *sum)
{
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        struct tbt_fec_hdr h;

        if (tbt_fec_hdr_unpack(g_rec, &h) != 0 ||
            tbt_fec_hdr_check(&h, TBT_FEC_S) != 0)
        {
            fprintf(stderr, "bench: header gate failed\n");
            exit(1);
        }
        *sum += h.payload_len + h.share_index;
    }
}

/* ------------------------------------------------------------------ *
 * Stage 3: header CRC. rx_record packs the decoded header, then runs
 * crc32c(0, hb, TBT_FEC_H_HDRCRC) and compares with h.header_crc32c.
 * ------------------------------------------------------------------ */

static void run_hdr_crc(unsigned iters, u64 *sum)
{
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        u32 want;

        if (tbt_fec_hdr_pack(&g_hdr, g_hdr_wire) != 0)
        {
            fprintf(stderr, "bench: header pack failed\n");
            exit(1);
        }
        want = tbt_fec_crc32c(0, g_hdr_wire, TBT_FEC_H_HDRCRC);
        if (want != g_hdr.header_crc32c)
        {
            fprintf(stderr, "bench: header CRC mismatch\n");
            exit(1);
        }
        *sum += want;
    }
}

/* ------------------------------------------------------------------ *
 * Stage 4: ChaCha8 body XOR. The RX path calls body_xor_flow on the
 * ciphertext in place, after the header CRC and before the payload CRC.
 * ------------------------------------------------------------------ */

static void run_crypto(unsigned iters, u64 *sum)
{
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        if (tbt_crypto_body_xor_flow(&g_crypto_rx, g_hdr.sid, g_hdr.seq,
                                     g_hdr.type == TBT_FEC_PARITY,
                                     g_hdr.share_index, g_crypto_buf,
                                     S_BYTES) != 0)
        {
            fprintf(stderr, "bench: body_xor_flow failed\n");
            exit(1);
        }
        *sum += g_crypto_buf[0];
    }
}

/* ------------------------------------------------------------------ *
 * Stage 5: payload CRC over the clear 8192-byte body.
 * ------------------------------------------------------------------ */

static void run_payload_crc(unsigned iters, u64 *sum)
{
    const u8 *payload = g_rec + TBT_FEC_HDR_LEN;
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        u32 want = tbt_fec_crc32c(0, payload, S_BYTES);

        if (want != g_hdr.payload_crc32c)
        {
            fprintf(stderr, "bench: payload CRC mismatch\n");
            exit(1);
        }
        *sum += want;
    }
}

/* ------------------------------------------------------------------ *
 * Stage 6: model of the FEC slot placement store.
 *
 * Mirrors tbt_fec_data.c:rx_record() after the CRCs: window step, arrival
 * classification, cut check, then the payload copy into the block slot. The
 * model block carries the fields the real tbt_fec_blk uses for these
 * decisions. The arena is [groups][slots] so the group/slot index arithmetic
 * is the real one.
 * ------------------------------------------------------------------ */

struct model_blk
{
    u8 data[BN * S_BYTES];
    u8 dpresent[BN];
    u16 dlen[BN];
    u16 ndata;
    u16 nreal;
    u16 short_len;
    u64 block_id;
    bool used;
    bool ready;
    bool empty;
    bool win_len_set;
};

static struct model_blk g_arena[BGROUPS][BSLOTS];

static void model_arena_init(void)
{
    size_t i, j, k;

    for (i = 0; i < BGROUPS; i++)
    {
        for (j = 0; j < BSLOTS; j++)
        {
            /* A slot's previous block left bytes behind; fill them so the
             * measured copy moves real data, not zero pages. */
            for (k = 0; k < sizeof(g_arena[i][j].data); k++)
                g_arena[i][j].data[k] = (u8)(k * 13u + i + j);
        }
    }
}

static void run_slot_place(unsigned iters, u64 *sum)
{
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        u64 block_id = 1u + (u64)i / BN;
        u16 share = (u16)(i % BN);
        u16 group = (u16)(block_id % BGROUPS);
        struct model_blk *b = &g_arena[group][block_id % BSLOTS];
        enum tbt_fec_rx_win win;
        enum tbt_fec_arrival arr;

        if (share == 0)
        {
            /* A fresh block takes the slot: reset is part of the real path
             * once per block, amortized here over its BN shares. */
            memset(b->dpresent, 0, sizeof(b->dpresent));
            b->ndata = 0;
            b->nreal = BN;
            b->short_len = 0;
            b->block_id = block_id;
            b->used = true;
            b->ready = false;
            b->empty = false;
            b->win_len_set = false;
        }

        /* The delivery frontier equals this block: PLACE. */
        win = tbt_fec_rx_window_step(block_id, block_id, BSLOTS, false, true);
        arr = tbt_fec_arrival_kind(b->ready, b->empty, TBT_FEC_DATA, 0,
                                   b->ndata > 0);
        if (win != TBT_FEC_RX_WIN_PLACE || arr != TBT_FEC_ARR_PLACE ||
            tbt_fec_share_past_cut(b->win_len_set, b->nreal, share) ||
            b->dpresent[share])
        {
            fprintf(stderr, "bench: slot model refused a valid share\n");
            exit(1);
        }
        memcpy(b->data + (size_t)share * S_BYTES, g_payload, S_BYTES);
        b->dlen[share] = S_BYTES;
        b->dpresent[share] = 1;
        b->ndata++;
        /* Every share of this shape is full, so the block's real length is
         * BN and it becomes ready when the last one lands. */
        if (b->ndata >= BN)
            b->ready = true;
        *sum += b->data[(size_t)share * S_BYTES] + b->ndata + (u64)b->ready;
    }
}

/* ------------------------------------------------------------------ *
 * Stage 7: model of the ordered delivery walk.
 *
 * tbt_fec_deliver_ready() visits p = 0..n-1 and, inside, group =
 * 0..groups-1, for the ready super-block s. Each visit decides
 * deliverability, the delivery length, and the ordered cursor, then copies
 * the slot to the stream. The copy target here is a ring; the kernel target
 * is a stream skb (out of scope).
 * ------------------------------------------------------------------ */

/* Eight slots, so `off` always lands on a slot boundary and one 8192-byte
 * copy never crosses the ring end. */
#define OUT_CAP (8u * S_BYTES)

static u8 g_out[OUT_CAP];

static void run_deliver(unsigned iters, u64 *sum)
{
    u64 off = 0;
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        u16 group = (u16)(i % BGROUPS);
        u16 p = (u16)((i / BGROUPS) % BN);
        u64 s = (u64)(i / (BGROUPS * BN));
        struct model_blk *b = &g_arena[group][s % BSLOTS];
        u16 len;

        if (!tbt_fec_blk_deliverable(true, true, s, s))
        {
            fprintf(stderr, "bench: deliver model skipped a ready block\n");
            exit(1);
        }
        len = tbt_fec_rx_share_len(b->dlen[p], b->short_len, p, BN, S_BYTES);
        if (!len || tbt_fec_deliver_step(off, len, off) != 0)
        {
            fprintf(stderr, "bench: deliver model rejected a valid slot\n");
            exit(1);
        }
        memcpy(g_out + (size_t)(off & (OUT_CAP - 1)),
               b->data + (size_t)p * S_BYTES, len);
        off += len;
        *sum += g_out[(size_t)(off & (OUT_CAP - 1))];
    }
}

/* ------------------------------------------------------------------ *
 * Driver
 * ------------------------------------------------------------------ */

struct stage
{
    const char *name;
    void (*run)(unsigned iters, u64 *sum);
};

static const struct stage stages[] = {
    { "http_chunk_decode", run_http_decode },
    { "fec_hdr_unpack_check", run_hdr_unpack_check },
    { "hdr_crc32c", run_hdr_crc },
    { "crypto_chacha8", run_crypto },
    { "payload_crc32c", run_payload_crc },
    { "fec_slot_place.model", run_slot_place },
    { "fec_deliver.model", run_deliver },
};

#define NSTAGES (sizeof(stages) / sizeof(stages[0]))

int main(void)
{
    double ns[NSTAGES];
    double raw[NSTAGES][ROUNDS]; /* per-round means, in run order */
    u64 checksum[NSTAGES];
    double total = 0.0;
    unsigned iters = ITERS_DEFAULT;
    unsigned s;
    unsigned r;

    fixture_init();
    model_arena_init();

    printf("# tbtun RX stage bench: record %d B = %d B header + %d B payload\n",
           REC_BYTES, TBT_FEC_HDR_LEN, S_BYTES);
    printf("# wire: one HTTP chunk per record, %zu B; shape n=%u m=%u groups=%u slots=%u\n",
           g_frame_len, BN, BM, BGROUPS, BSLOTS);
    printf("# cpu: %s\n", cpu_model());
    printf("# compiler: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__,
           __GNUC_PATCHLEVEL__);
    printf("# build: %s\n", BENCH_BUILD_CMD);
    printf("# iters_per_round=%u rounds=%d warmup=1 median_of_rounds\n", iters,
           ROUNDS);
    printf("# mb_per_s = %d payload bytes per record / ns\n", S_BYTES);

    for (s = 0; s < NSTAGES; s++)
    {
        double samples[ROUNDS];
        u64 sink = 0;

        /* Warm-up: page in the fixture and let the branch predictors settle. */
        stages[s].run(iters / 8u, &sink);
        for (r = 0; r < ROUNDS; r++)
        {
            u64 round_sink = 0;
            double t0, t1;

            t0 = now_sec();
            stages[s].run(iters, &round_sink);
            t1 = now_sec();
            samples[r] = (t1 - t0) * 1e9 / (double)iters;
            raw[s][r] = samples[r];
            sink ^= round_sink;
        }
        qsort(samples, ROUNDS, sizeof(samples[0]), cmp_double);
        ns[s] = samples[ROUNDS / 2];
        checksum[s] = sink;
        total += ns[s];
    }
    printf("stage,ops,ns_per_pkt,pct_chain,mb_per_s,checksum\n");
    for (s = 0; s < NSTAGES; s++)
    {
        double mb_s = (double)S_BYTES / (ns[s] * 1e-9) / 1e6;

        printf("%s,%u,%.2f,%.2f,%.2f,%llx\n", stages[s].name, iters * ROUNDS,
               ns[s], ns[s] * 100.0 / total, mb_s,
               (unsigned long long)checksum[s]);
    }
    for (s = 0; s < NSTAGES; s++)
    {
        printf("# samples.%s", stages[s].name);
        for (r = 0; r < ROUNDS; r++)
            printf("%s%.2f", r ? "," : "=", raw[s][r]);
        printf("\n");
    }
    printf("# total_chain_ns=%.2f\n", total);
    printf("# chain_mb_per_s=%.2f\n", (double)S_BYTES / (total * 1e-9) / 1e6);
    printf("# fixture_fnv=%llx\n",
           (unsigned long long)fnv1a(g_rec, sizeof(g_rec)));
    return 0;
}
