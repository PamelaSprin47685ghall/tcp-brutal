/* test_tsan_race.c - ThreadSanitizer gate for the concurrent host paths.
 *
 * WHAT THIS IS
 *   Three concurrency contracts, each driven by eight pthreads and 50,000
 *   iterations in total:
 *
 *   1. tbt_fec_out_unlink_once() (tbt_fec.h). A drain batch commit and a
 *      flow purge can both reach the same queued record; only the winner
 *      unlinks it and charges the queue. The kernel calls the helper under
 *      sess->fec_out_lock, so one pthread mutex models that lock here: every
 *      record gets exactly one winner, one list unlink and one free, with no
 *      double free and no leaked record.
 *
 *   2. tbt_crypto_body_xor_flow() (tbt_crypto.c). RX carriers share one
 *      read-only context and call the body hook concurrently. The test mixes
 *      that shared context with a per-thread context, all under one shared,
 *      read-only key, and pins that every call is deterministic and its own
 *      inverse.
 *
 *   3. The tbt_read_bool() normalization. The kernel macro (tbtun.h) cannot
 *      compile outside the kernel (READ_ONCE, BUILD_BUG_ON, printk), so this
 *      file mirrors its one-byte load and "nonzero means true" rule on a u8.
 *      Readers and writers use the host spelling of READ_ONCE/WRITE_ONCE
 *      (relaxed __atomic builtins), so TSan sees legal concurrent accesses
 *      rather than a test that manufactures the race it claims to check.
 *
 *   The binary refuses to build without -fsanitize=thread: a silent build
 *   would report "no race" from a run that never had TSan in it.
 *
 *   make host-test-tsan
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_crypto.h"
#include "tbt_fec.h"

#if !defined(__SANITIZE_THREAD__)
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TBT_TSAN_ACTIVE 1
#endif
#endif
#ifndef TBT_TSAN_ACTIVE
#error "test_tsan_race must be built with -fsanitize=thread (make host-test-tsan)"
#endif
#endif

#define NTHREADS 8
#define RACE_RECS 3125
#define RACE_PASSES 2
#define REC_ATTEMPTS (RACE_PASSES * RACE_RECS)
#define RACE_ITERS (NTHREADS * REC_ATTEMPTS)
#define REC_BYTES 64u

#define CRYPTO_ITERS (RACE_ITERS / NTHREADS)
#define CRYPTO_REF_EVERY 8

#define BOOL_ITERS (RACE_ITERS / NTHREADS)

static unsigned expect(bool cond, const char *what)
{
    if (cond)
        return 0;

    fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

static void spawn(pthread_t *t, void *(*fn)(void *), void *arg)
{
    int err = pthread_create(t, NULL, fn, arg);

    if (err != 0)
    {
        fprintf(stderr, "FAIL pthread_create: %s\n", strerror(err));
        exit(2);
    }
}

static void fill_pattern(u8 *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (u8)(seed * 131u + i * 7u + 0x5au);
}

/* ------------------------------------------------------------------ *
 * 1. drain batch commit vs flow purge: one unlink per record
 * ------------------------------------------------------------------ */

struct fec_rec
{
    bool queued;
    bool linked;
    bool freed;
    unsigned unlinks;
    unsigned frees;
};

struct unlink_worker
{
    unsigned wins[RACE_PASSES];
    unsigned losses[RACE_PASSES];
    unsigned errors;
};

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t rec_barrier;
static struct fec_rec *recs;
static unsigned queue_depth;
static unsigned queue_bytes;
static unsigned total_wins;
static unsigned total_losses;

static void *unlink_worker_main(void *arg)
{
    struct unlink_worker *w = arg;
    unsigned pass;
    unsigned slot;

    for (pass = 0; pass < RACE_PASSES; pass++)
    {
        for (slot = 0; slot < RACE_RECS; slot++)
        {
            struct fec_rec *r = &recs[slot];
            bool won;

            /* Release all eight threads onto the same record. One drain and
             * one purge path is the kernel's race; eight is a stress test. */
            pthread_barrier_wait(&rec_barrier);
            pthread_mutex_lock(&out_lock);
            won = tbt_fec_out_unlink_once(&r->queued);
            if (won)
            {
                if (!r->linked || r->freed)
                    w->errors++; /* double list_del or double free */
                r->linked = false;
                r->freed = true;
                r->unlinks++;
                r->frees++;
                w->wins[pass]++;
                total_wins++;
                if (queue_depth == 0 || queue_bytes < REC_BYTES)
                    w->errors++; /* the queue counters wrapped */
                else
                {
                    queue_depth--;
                    queue_bytes -= REC_BYTES;
                }
            }
            else
            {
                /* The helper refused: the winner must already have unlinked
                 * the record and no loser may touch the list or the free. */
                if (r->queued)
                    w->errors++;
                w->losses[pass]++;
                total_losses++;
            }
            pthread_mutex_unlock(&out_lock);
        }
    }
    return NULL;
}

static unsigned run_unlink_race(void)
{
    pthread_t tids[NTHREADS];
    struct unlink_worker workers[NTHREADS];
    unsigned worker_errors = 0;
    unsigned wins[RACE_PASSES] = {0, 0};
    unsigned errors = 0;
    unsigned i;
    unsigned slot;

    recs = calloc(RACE_RECS, sizeof(*recs));
    if (recs == NULL)
    {
        fprintf(stderr, "FAIL calloc\n");
        exit(2);
    }
    for (slot = 0; slot < RACE_RECS; slot++)
    {
        recs[slot].queued = true;
        recs[slot].linked = true;
    }
    queue_depth = RACE_RECS;
    queue_bytes = RACE_RECS * REC_BYTES;
    total_wins = 0;
    total_losses = 0;
    memset(workers, 0, sizeof(workers));

    if (pthread_barrier_init(&rec_barrier, NULL, NTHREADS) != 0)
    {
        fprintf(stderr, "FAIL pthread_barrier_init\n");
        exit(2);
    }
    for (i = 0; i < NTHREADS; i++)
        spawn(&tids[i], unlink_worker_main, &workers[i]);
    for (i = 0; i < NTHREADS; i++)
        pthread_join(tids[i], NULL);
    pthread_barrier_destroy(&rec_barrier);

    for (i = 0; i < NTHREADS; i++)
    {
        worker_errors += workers[i].errors;
        wins[0] += workers[i].wins[0];
        wins[1] += workers[i].wins[1];
    }

    errors += expect(worker_errors == 0,
                     "unlink: double unlink, double free or queue underflow");
    errors += expect(total_wins == RACE_RECS,
                     "unlink: a record had no winner or two");
    errors += expect(total_losses == RACE_ITERS - RACE_RECS,
                     "unlink: loser count");
    errors += expect(wins[0] == RACE_RECS && wins[1] == 0,
                     "unlink: the second pass unlinked a record");
    errors += expect(queue_depth == 0, "unlink: queue depth leaked");
    errors += expect(queue_bytes == 0, "unlink: queue bytes leaked");
    for (slot = 0; slot < RACE_RECS; slot++)
    {
        if (recs[slot].unlinks != 1 || recs[slot].frees != 1 ||
            recs[slot].queued || recs[slot].linked)
        {
            errors += expect(false, "unlink: per-record unlink/free count");
            break;
        }
    }
    free(recs);
    recs = NULL;
    return errors;
}

/* ------------------------------------------------------------------ *
 * 2. shared and per-thread crypto contexts under one read-only key
 * ------------------------------------------------------------------ */

struct crypto_worker
{
    unsigned tid;
    unsigned iters;
    unsigned call_errors;
    unsigned noninvertible;
    unsigned mismatches;
};

static u8 crypto_key[TBT_CRYPTO_KEY_LEN];
static u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN];
static struct tbt_crypto_ctx shared_ctx;
static u8 crypto_ref[TBT_CRYPTO_BLOCK_LEN];
static u8 crypto_ref_plain[TBT_CRYPTO_BLOCK_LEN];

static void *crypto_worker_main(void *arg)
{
    struct crypto_worker *w = arg;
    struct tbt_crypto_ctx own;
    u8 buf[96];
    u8 plain[96];
    unsigned i;

    if (tbt_crypto_init(&own, crypto_key, crypto_nonce, 0) != 0)
    {
        w->call_errors++;
        return NULL;
    }

    for (i = 0; i < CRYPTO_ITERS; i++)
    {
        struct tbt_crypto_ctx *ctx = (i & 1u) ? &own : &shared_ctx;
        u64 seq = 1 + (u64)i;
        bool parity = (i & 1u) != 0;
        u64 sid = 1 + (i % 5u);
        u16 index = (u16)(i & 0xffu);
        u16 len = (u16)(1u + (i % 96u));

        fill_pattern(buf, len, i + w->tid);
        memcpy(plain, buf, len);
        w->iters++;
        if (tbt_crypto_body_xor_flow(ctx, sid, seq, parity, index, buf,
                                     len) != 0)
        {
            w->call_errors++;
            continue;
        }
        /* The same parameters on the same context must restore the plain
         * bytes, whether the context is shared or per-thread. */
        if (tbt_crypto_body_xor_flow(ctx, sid, seq, parity, index, buf,
                                     len) != 0 ||
            memcmp(buf, plain, len) != 0)
            w->noninvertible++;

        if (i % CRYPTO_REF_EVERY == 0)
        {
            u8 probe[TBT_CRYPTO_BLOCK_LEN];

            /* One fixed vector on both contexts: a shared context must give
             * the single-threaded reference ciphertext, and so must a
             * per-thread context installed from the same read-only key. */
            memcpy(probe, crypto_ref_plain, sizeof(probe));
            if (tbt_crypto_body_xor_flow(ctx, 7, 9, true, 3, probe,
                                         sizeof(probe)) != 0 ||
                memcmp(probe, crypto_ref, sizeof(probe)) != 0)
                w->mismatches++;
        }
    }
    tbt_crypto_wipe(&own);
    return NULL;
}

static unsigned run_crypto_race(void)
{
    pthread_t tids[NTHREADS];
    struct crypto_worker workers[NTHREADS];
    struct tbt_crypto_ctx ref_ctx;
    unsigned worker_errors = 0;
    unsigned iters = 0;
    unsigned errors = 0;
    unsigned i;

    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
        crypto_key[i] = (u8)(0x40 + i);
    for (i = 0; i < TBT_CRYPTO_NONCE_LEN; i++)
        crypto_nonce[i] = (u8)(0x80 + i);

    if (tbt_crypto_init(&shared_ctx, crypto_key, crypto_nonce, 0) != 0 ||
        tbt_crypto_init(&ref_ctx, crypto_key, crypto_nonce, 0) != 0)
    {
        fprintf(stderr, "FAIL crypto: init\n");
        return 1;
    }
    fill_pattern(crypto_ref_plain, sizeof(crypto_ref_plain), 0x51u);
    memcpy(crypto_ref, crypto_ref_plain, sizeof(crypto_ref));
    if (tbt_crypto_body_xor_flow(&ref_ctx, 7, 9, true, 3, crypto_ref,
                                 sizeof(crypto_ref)) != 0)
    {
        fprintf(stderr, "FAIL crypto: reference call\n");
        return 1;
    }
    tbt_crypto_wipe(&ref_ctx);

    memset(workers, 0, sizeof(workers));
    for (i = 0; i < NTHREADS; i++)
    {
        workers[i].tid = i;
        spawn(&tids[i], crypto_worker_main, &workers[i]);
    }
    for (i = 0; i < NTHREADS; i++)
        pthread_join(tids[i], NULL);

    for (i = 0; i < NTHREADS; i++)
    {
        iters += workers[i].iters;
        worker_errors += workers[i].call_errors;
        worker_errors += workers[i].noninvertible;
        worker_errors += workers[i].mismatches;
    }
    errors += expect(iters == RACE_ITERS, "crypto: iteration count");
    errors += expect(worker_errors == 0,
                     "crypto: call error, inverse failure or reference mismatch");
    tbt_crypto_wipe(&shared_ctx);
    return errors;
}

/* ------------------------------------------------------------------ *
 * 3. tbt_read_bool() normalization under concurrent byte writes
 * ------------------------------------------------------------------ */

struct bool_word
{
    u8 byte;
};

struct bool_worker
{
    unsigned tid;
    bool writer;
    unsigned ops;
    unsigned errors;
};

static struct bool_word shared_flag;

/* Host mirror of tbtun.h's tbt_read_bool(): one relaxed byte load, then
 * nonzero means true. The kernel macro's type check and rate-limited warning
 * have no host equivalent and do not change the normalization. */
static inline bool host_tbt_read_bool(const u8 *field, u8 *raw)
{
    u8 byte = __atomic_load_n(field, __ATOMIC_RELAXED);

    if (raw != NULL)
        *raw = byte;
    return byte != 0;
}

static unsigned bool_boundary_sweep(void)
{
    static const u8 values[] = {0, 1, 2, 3, 0x7f, 0x80, 0xfe, 0xff};
    unsigned errors = 0;
    unsigned i;

    for (i = 0; i < sizeof(values); i++)
    {
        u8 raw = 0;

        shared_flag.byte = values[i];
        if (host_tbt_read_bool(&shared_flag.byte, &raw) !=
                (values[i] != 0) ||
            raw != values[i])
            errors++;
    }
    return errors;
}

static void *bool_worker_main(void *arg)
{
    static const u8 values[4] = {0, 1, 2, 0xff};
    struct bool_worker *w = arg;
    unsigned i;

    for (i = 0; i < BOOL_ITERS; i++)
    {
        if (w->writer)
        {
            __atomic_store_n(&shared_flag.byte, values[(i + w->tid) & 3u],
                             __ATOMIC_RELAXED);
        }
        else
        {
            u8 raw = 0;
            bool got = host_tbt_read_bool(&shared_flag.byte, &raw);

            /* The byte is written by atomic stores, so a read sees one of
             * the four values whole; a non-bool byte still normalizes. */
            if (got != (raw != 0) ||
                (raw != 0 && raw != 1 && raw != 2 && raw != 0xff))
                w->errors++;
        }
        w->ops++;
    }
    return NULL;
}

static unsigned run_bool_race(void)
{
    pthread_t tids[NTHREADS];
    struct bool_worker workers[NTHREADS];
    unsigned worker_errors = 0;
    unsigned ops = 0;
    unsigned errors;
    unsigned i;

    shared_flag.byte = 2; /* a byte that is neither 0 nor 1 */
    errors = bool_boundary_sweep();
    memset(workers, 0, sizeof(workers));
    for (i = 0; i < NTHREADS; i++)
    {
        workers[i].tid = i;
        workers[i].writer = (i % 2) == 0;
        spawn(&tids[i], bool_worker_main, &workers[i]);
    }
    for (i = 0; i < NTHREADS; i++)
        pthread_join(tids[i], NULL);
    for (i = 0; i < NTHREADS; i++)
    {
        ops += workers[i].ops;
        worker_errors += workers[i].errors;
    }
    errors += expect(ops == RACE_ITERS, "bool: operation count");
    errors += expect(worker_errors == 0,
                     "bool: normalization or torn byte under concurrency");
    return errors;
}

int main(void)
{
    unsigned errors = 0;

    errors += run_unlink_race();
    errors += run_crypto_race();
    errors += run_bool_race();

    if (errors != 0)
    {
        printf("test_tsan_race: FAIL (%u checks)\n", errors);
        return 1;
    }
    printf("test_tsan_race: ok: %d threads, %d iterations per phase, "
           "3 concurrent contracts, no TSan report\n",
           NTHREADS, RACE_ITERS);
    return 0;
}
