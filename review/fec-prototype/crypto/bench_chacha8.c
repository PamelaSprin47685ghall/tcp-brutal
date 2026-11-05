/*
 * bench_chacha8.c - throughput of the ChaCha8/12/20 core at packet sizes.
 *
 * Measures chacha8_xor over a hot buffer at 1 KiB, 8 KiB and 64 KiB, which
 * bracket the FEC prototype's packet (1200 B), generation (9.6-38.4 KiB) and
 * grouped-window (about 537 KiB) working sets. Reports MB/s (10^6 bytes/s)
 * and cycles per byte. Cycles use the CPU MHz that /proc/cpuinfo reports at
 * start-up; on a boosting core that value drifts, so treat cycles/byte as a
 * scale reference, not an exact cycle count. Pass --mhz N to pin it.
 *
 * Build:
 *   cc -std=c99 -O3 -Wall -Wextra -Werror -o bench_chacha8 \
 *      bench_chacha8.c chacha8.c
 * Run:
 *   ./bench_chacha8 [--mhz N] [--seconds S]
 */

#define _POSIX_C_SOURCE 200809L

#include "chacha8.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KEY_SIZE 32u
#define NONCE_SIZE 8u

static volatile uint64_t sink;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 * Highest core frequency that /proc/cpuinfo reports. The pinned core boosts
 * under load while its siblings idle, so the maximum is a better proxy for
 * the core that runs the benchmark than the first entry is. It stays an
 * estimate: the reported value is an instantaneous sample, not an average.
 */
static double cpu_mhz(void)
{
    FILE *f;
    char line[256];
    double mhz = 0.0;

    f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return 0.0;

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "cpu MHz", 7) == 0) {
            const char *colon = strchr(line, ':');
            if (colon != NULL) {
                double v = strtod(colon + 1, NULL);
                if (v > mhz)
                    mhz = v;
            }
        }
    }

    fclose(f);
    return mhz;
}

/*
 * One timing sample: double the iteration count until the batch runs for at
 * least min_sec, then report the rate of that batch. The buffer stays hot.
 */
static double sample_mbps(const uint8_t key[KEY_SIZE],
                          const uint8_t nonce[NONCE_SIZE], int rounds,
                          uint8_t *buf, size_t len, double min_sec)
{
    unsigned iters = 1;
    unsigned i;
    double t0;
    double t1;
    double rate;

    for (;;) {
        t0 = now_sec();
        for (i = 0; i < iters; i++) {
            if (chacha8_xor(key, KEY_SIZE, nonce, 0, rounds,
                            buf, len, buf) != 0) {
                fprintf(stderr, "bench: chacha8_xor failed\n");
                exit(1);
            }
            sink += buf[0];
        }
        t1 = now_sec();

        if (t1 - t0 >= min_sec || iters >= (1u << 24))
            break;
        iters *= 2;
    }

    rate = ((double)len * (double)iters) / (t1 - t0) / 1e6;
    return rate;
}

static double median3(double a, double b, double c)
{
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        double t = b;
        b = c;
        c = t;
    }
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    return b;
}

/* Guard: a fast benchmark of a broken core is worthless. */
static void self_check(void)
{
    static const uint8_t want[8] = {
        0xe2, 0x8a, 0x5f, 0xa4, 0xa6, 0x7f, 0x8c, 0x5d
    };
    uint8_t key[16] = {0};
    uint8_t nonce[NONCE_SIZE] = {0};
    uint8_t out[8];

    if (chacha8_xor(key, sizeof(key), nonce, 0, 8, out, sizeof(out), out) != 0
        || memcmp(out, want, sizeof(want)) != 0) {
        fprintf(stderr, "bench: TC1 self-check failed, refusing to measure\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    static const size_t sizes[] = {1024, 8192, 65536};
    static const int rounds_list[] = {8, 12, 20};
    uint8_t key[KEY_SIZE];
    uint8_t nonce[NONCE_SIZE];
    uint8_t *buf;
    double min_sec = 0.25;
    double mhz;
    size_t i;
    size_t r;
    int a;

    mhz = cpu_mhz();

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--mhz") == 0 && a + 1 < argc) {
            mhz = strtod(argv[++a], NULL);
        } else if (strcmp(argv[a], "--seconds") == 0 && a + 1 < argc) {
            min_sec = strtod(argv[++a], NULL);
        } else {
            fprintf(stderr, "usage: %s [--mhz N] [--seconds S]\n", argv[0]);
            return 2;
        }
    }

    if (min_sec <= 0.0) {
        fprintf(stderr, "bench: --seconds must be positive\n");
        return 2;
    }

    self_check();

    for (i = 0; i < KEY_SIZE; i++)
        key[i] = (uint8_t)(i * 7 + 1);
    for (i = 0; i < NONCE_SIZE; i++)
        nonce[i] = (uint8_t)(0xa0 + i);

    buf = malloc(sizes[sizeof(sizes) / sizeof(sizes[0]) - 1]);
    if (buf == NULL) {
        fprintf(stderr, "bench: out of memory\n");
        return 1;
    }
    memset(buf, 0, sizes[sizeof(sizes) / sizeof(sizes[0]) - 1]);

    printf("cpu_mhz: %.0f\n", mhz);
    printf("buffer: hot cache, in-place xor, counter 0 per pass\n");
    printf("%-8s %8s %12s %12s\n", "rounds", "bytes", "MB/s", "cycles/B");

    for (r = 0; r < sizeof(rounds_list) / sizeof(rounds_list[0]); r++) {
        int rounds = rounds_list[r];

        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            size_t len = sizes[i];
            double s1;
            double s2;
            double s3;
            double mbps;
            double cpb;

            s1 = sample_mbps(key, nonce, rounds, buf, len, min_sec);
            s2 = sample_mbps(key, nonce, rounds, buf, len, min_sec);
            s3 = sample_mbps(key, nonce, rounds, buf, len, min_sec);
            mbps = median3(s1, s2, s3);
            cpb = (mhz > 0.0) ? mhz / mbps : 0.0;

            printf("%-8d %8zu %12.1f %12.2f\n", rounds, len, mbps, cpb);
        }
    }

    free(buf);
    printf("sink: %llu\n", (unsigned long long)sink);
    return 0;
}
