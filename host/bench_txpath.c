/* bench_txpath.c - host cost model for the FEC TX record path.
 *
 * WHAT THIS MEASURES
 *   The per-record work tbtun does between the packetizer and one carrier
 *   socket write, minus the socket write itself:
 *
 *     txpath.push_alloc.8k    tbt_fec_out_push(): kmalloc(record) + memcpy
 *                             + free, the per-record queue allocation.
 *     txpath.batch_alloc8.8k  tbt_carrier_send_fec_batch() with count=8:
 *                             kmalloc_array(8 frames) + 8 HTTP chunk
 *                             encodes + free.
 *     txpath.batch_reuse8.8k  The same eight encodes into one preallocated
 *                             frame buffer: the allocation is gone.
 *     txpath.chunk_encode.8k  One tbt_http_fec_chunk_encode() of an 8240 B
 *                             record: chunk size line + memcpy + CRLF.
 *     txpath.copy.8k          One memcpy of the 8192 B payload: the floor
 *                             every buffer-reuse scheme still pays.
 *
 *   The record is the production maximum: TBT_FEC_HDR_LEN + 8192 payload =
 *   8240 B, framed to at most TBT_HTTP_FEC_FRAME_MAX bytes.
 *
 * WHY
 *   A 256-carrier run at 27 MB/s of wire bytes emits about 3400 records/s.
 *   If the per-record allocation and copy chain costs N ns, its CPU share is
 *   N * 3400 / 1e9. This benchmark supplies N, so the "per-record allocation
 *   is the bottleneck" hypothesis can be checked against the CPU counters
 *   instead of assumed.
 *
 * LIMITS
 *   malloc/free is faster than kmalloc/kfree on a cold slab, and this does
 *   not include the socket write, skb allocation, or TCP. The measured
 *   per-record cost is therefore a lower bound on the kernel cost.
 *
 * OUTPUT
 *   CSV on stdout: name,iterations,ns_per_op,mb_per_s,checksum.
 *   ns_per_op is per batch for the two batch rows (8 records), per record
 *   otherwise. The human table on stderr prints ns per record for every row
 *   and the implied CPU share at 3400 records/s.
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_http.h"
#include "tbt_uapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REC_LEN TBT_FEC_REC_MAX /* 8240 */
#define PAYLOAD 8192
#define BATCH 8
#define FRAME_MAX TBT_HTTP_FEC_FRAME_MAX
#define ROUNDS 7

static u8 rec[REC_LEN];
static u8 *frame_buf;

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

enum mode
{
    M_PUSH_ALLOC = 0,
    M_BATCH_ALLOC,
    M_BATCH_REUSE,
    M_CHUNK_ENCODE,
    M_COPY,
    M_COUNT,
};

static const char *mode_name[M_COUNT] = {
    "txpath.push_alloc.8k",
    "txpath.batch_alloc8.8k",
    "txpath.batch_reuse8.8k",
    "txpath.chunk_encode.8k",
    "txpath.copy.8k",
};

/* Records per operation: the batch rows move eight records. */
static unsigned mode_records[M_COUNT] = { 1, BATCH, BATCH, 1, 1 };

static void run_mode(enum mode m, unsigned iters, u64 *sum)
{
    unsigned i;

    for (i = 0; i < iters; i++)
    {
        switch (m)
        {
        case M_PUSH_ALLOC:
        {
            u8 *p = malloc(REC_LEN);

            if (!p)
                abort();
            memcpy(p, rec, REC_LEN);
            *sum += p[0] + p[REC_LEN - 1];
            free(p);
            break;
        }
        case M_BATCH_ALLOC:
        {
            size_t cap = (size_t)BATCH * FRAME_MAX;
            u8 *p = malloc(cap);
            size_t off = 0;
            unsigned j;

            if (!p)
                abort();
            for (j = 0; j < BATCH; j++)
            {
                size_t n = tbt_http_fec_chunk_encode((char *)p + off,
                                                     cap - off, rec, REC_LEN);

                if (!n)
                    abort();
                off += n;
            }
            *sum += off;
            free(p);
            break;
        }
        case M_BATCH_REUSE:
        {
            size_t cap = (size_t)BATCH * FRAME_MAX;
            size_t off = 0;
            unsigned j;

            for (j = 0; j < BATCH; j++)
            {
                size_t n = tbt_http_fec_chunk_encode((char *)frame_buf + off,
                                                     cap - off, rec, REC_LEN);

                if (!n)
                    abort();
                off += n;
            }
            *sum += off;
            break;
        }
        case M_CHUNK_ENCODE:
        {
            size_t n = tbt_http_fec_chunk_encode((char *)frame_buf, FRAME_MAX,
                                                 rec, REC_LEN);

            if (!n)
                abort();
            *sum += n;
            break;
        }
        case M_COPY:
            memcpy(frame_buf, rec, PAYLOAD);
            *sum += frame_buf[0];
            break;
        default:
            abort();
        }
    }
}

int main(void)
{
    double ns[M_COUNT];
    u64 checksum[M_COUNT];
    unsigned m;

    memset(rec, 0x5a, sizeof(rec));
    /* A valid FEC header is not needed for framing cost, but the record
     * length must be inside the encoder's accepted range. */
    frame_buf = malloc((size_t)BATCH * FRAME_MAX);
    if (!frame_buf)
        return 1;

    for (m = 0; m < M_COUNT; m++)
    {
        double samples[ROUNDS];
        unsigned r;
        unsigned iters = mode_records[m] == 1 ? 20000 : 2500;
        u64 sink = 0;

        /* Warm up the allocator and the encode path. */
        run_mode((enum mode)m, 200, &sink);
        for (r = 0; r < ROUNDS; r++)
        {
            double t0, t1;

            sink = 0;
            t0 = now_sec();
            run_mode((enum mode)m, iters, &sink);
            t1 = now_sec();
            samples[r] = (t1 - t0) * 1e9 / (double)iters;
        }
        qsort(samples, ROUNDS, sizeof(samples[0]), cmp_double);
        ns[m] = samples[ROUNDS / 2];
        checksum[m] = fnv1a(rec, sizeof(rec)) ^ sink;
    }

    printf("name,iterations,ns_per_op,mb_per_s,checksum\n");
    for (m = 0; m < M_COUNT; m++)
    {
        unsigned iters = mode_records[m] == 1 ? 20000 : 2500;
        double bytes = (double)mode_records[m] * PAYLOAD;
        double mb_s = bytes / (ns[m] * 1e-9) / 1e6;

        printf("%s,%u,%.3f,%.3f,%llx\n", mode_name[m], iters * ROUNDS, ns[m],
               mb_s, (unsigned long long)checksum[m]);
    }

    /* Per-record view and the CPU share at the short-run record rate. */
    {
        double push = ns[M_PUSH_ALLOC];
        double batch_alloc = ns[M_BATCH_ALLOC] / BATCH;
        double batch_reuse = ns[M_BATCH_REUSE] / BATCH;
        double now = push + batch_alloc;
        double opt = batch_reuse;
        double rec_s = 3400.0;

        fprintf(stderr,
                "%-28s %8.1f ns/record  %7.2f %% cpu @3400/s\n"
                "%-28s %8.1f ns/record  %7.2f %% cpu @3400/s\n"
                "%-28s %8.1f ns/record  %7.2f %% cpu @3400/s\n"
                "%-28s %8.1f ns/record  %7.2f %% cpu @3400/s\n"
                "%-28s %8.1f ns/record\n"
                "%-28s %8.1f ns/record\n"
                "current = push_alloc + batch_alloc/8 = %.1f ns/record (%.2f %% cpu)\n"
                "reuse   = batch_reuse/8             = %.1f ns/record (%.2f %% cpu)\n"
                "saving  = %.1f ns/record = %.2f %% cpu @3400/s\n",
                mode_name[M_PUSH_ALLOC], push, push * rec_s / 1e7,
                mode_name[M_BATCH_ALLOC], batch_alloc,
                batch_alloc * rec_s / 1e7, mode_name[M_BATCH_REUSE],
                batch_reuse, batch_reuse * rec_s / 1e7,
                mode_name[M_CHUNK_ENCODE], ns[M_CHUNK_ENCODE],
                ns[M_CHUNK_ENCODE] * rec_s / 1e7, mode_name[M_COPY],
                ns[M_COPY], "memcpy floor", ns[M_COPY], now,
                now * rec_s / 1e7, opt, opt * rec_s / 1e7, now - opt,
                (now - opt) * rec_s / 1e7);
    }

    free(frame_buf);
    return 0;
}
