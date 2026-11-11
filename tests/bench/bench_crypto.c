/*
 * bench_crypto.c - throughput of the ChaCha8/12/20 core at packet sizes.
 *
 * Measures tbt_chacha8_xor over a hot buffer at 1 KiB, 8 KiB and 64 KiB,
 * which bracket a TBT_PAYLOAD_MAX record (8 KiB) and a grouped window. It
 * reports MB/s (10^6 bytes/s) and cycles per byte. Cycles use the highest
 * CPU MHz /proc/cpuinfo reports at start-up; on a boosting core that value
 * drifts, so treat cycles/byte as a scale reference, not an exact cycle
 * count. Pass --mhz N to pin it.
 *
 * Build: make host-bench-crypto
 * Run:   build/bin/bench_crypto [--mhz N] [--seconds S]
 *
 * The self-check refuses to measure a core that fails TC1, so a broken
 * optimization cannot produce a fast, meaningless number.
 */
#define _POSIX_C_SOURCE 200809L

#include "tbt_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile u64 sink;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double cpu_mhz(void)
{
    FILE *f;
    char line[256];
    double mhz = 0.0;

    f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return 0.0;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "cpu MHz", 7) == 0)
        {
            const char *colon = strchr(line, ':');

            if (colon != NULL)
            {
                double v = strtod(colon + 1, NULL);

                if (v > mhz)
                    mhz = v;
            }
        }
    }

    fclose(f);
    return mhz;
}

static double sample_mbps(const u8 key[TBT_CRYPTO_KEY_LEN],
                          const u8 nonce[TBT_CRYPTO_NONCE_LEN], int rounds,
                          u8 *buf, size_t len, double min_sec)
{
    unsigned iters = 1;
    unsigned i;
    double t0;
    double t1;

    for (;;)
    {
        t0 = now_sec();
        for (i = 0; i < iters; i++)
        {
            if (tbt_chacha8_xor(key, TBT_CRYPTO_KEY_LEN, nonce, 0, rounds, buf,
                                len) != 0)
            {
                fprintf(stderr, "bench: tbt_chacha8_xor failed\n");
                exit(1);
            }
            sink += buf[0];
        }
        t1 = now_sec();

        if (t1 - t0 >= min_sec || iters >= (1u << 24))
            break;
        iters *= 2;
    }

    return ((double)len * (double)iters) / (t1 - t0) / 1e6;
}

static double median3(double a, double b, double c)
{
    if (a > b)
    {
        double t = a;

        a = b;
        b = t;
    }
    if (b > c)
    {
        double t = b;

        b = c;
        c = t;
    }
    if (a > b)
    {
        double t = a;

        a = b;
        b = t;
    }
    return b;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static int from_hex(const char *hex, u8 *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);

        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

/*
 * Both frozen KATs, full first block, before any timing. A fast number from a
 * broken core is worse than no number.
 */
static void self_check(void)
{
    static const char *const tc1 =
        "e28a5fa4a67f8c5defed3e6fb7303486aa8427d31419a729572d777953491120"
        "b64ab8e72b8deb85cd6aea7cb6089a101824beeb08814a428aab1fa2c816081b";
    static const char *const gen256 =
        "9a1e949f0d71a3a307da58730300dea725342ee45cf066d8350095e3c443cee2"
        "68db83e2ce8d899d90e96aef644b1c8493c228cdaa420c5603d745f14127b4ba";
    u8 key16[16] = {0};
    u8 key32[32];
    u8 nonce[TBT_CRYPTO_NONCE_LEN] = {0};
    u8 want[64];
    u8 out[64] = {0};
    size_t i;

    for (i = 0; i < sizeof(key32); i++)
        key32[i] = (u8)i;

    memset(out, 0, sizeof(out));
    if (tbt_chacha8_xor(key16, sizeof(key16), nonce, 0, 8, out, sizeof(out)) != 0 ||
        from_hex(tc1, want, sizeof(want)) != 0 ||
        memcmp(out, want, sizeof(want)) != 0)
    {
        fprintf(stderr, "bench: TC1 self-check failed, refusing to measure\n");
        exit(1);
    }

    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0xa0 + i);
    memset(out, 0, sizeof(out));
    if (tbt_chacha8_xor(key32, sizeof(key32), nonce, 0, 8, out, sizeof(out)) != 0 ||
        from_hex(gen256, want, sizeof(want)) != 0 ||
        memcmp(out, want, sizeof(want)) != 0)
    {
        fprintf(stderr, "bench: 256-bit KAT failed, refusing to measure\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    static const size_t sizes[] = {1024, 8192, 65536};
    static const int rounds_list[] = {8, 12, 20};
    u8 key[TBT_CRYPTO_KEY_LEN];
    u8 nonce[TBT_CRYPTO_NONCE_LEN];
    u8 *buf;
    double min_sec = 0.25;
    double mhz;
    size_t i;
    size_t r;
    int a;

    mhz = cpu_mhz();

    for (a = 1; a < argc; a++)
    {
        if (strcmp(argv[a], "--mhz") == 0 && a + 1 < argc)
            mhz = strtod(argv[++a], NULL);
        else if (strcmp(argv[a], "--seconds") == 0 && a + 1 < argc)
            min_sec = strtod(argv[++a], NULL);
        else
        {
            fprintf(stderr, "usage: %s [--mhz N] [--seconds S]\n", argv[0]);
            return 2;
        }
    }

    if (min_sec <= 0.0)
    {
        fprintf(stderr, "bench: --seconds must be positive\n");
        return 2;
    }

    self_check();

    for (i = 0; i < TBT_CRYPTO_KEY_LEN; i++)
        key[i] = (u8)(i * 7 + 1);
    for (i = 0; i < TBT_CRYPTO_NONCE_LEN; i++)
        nonce[i] = (u8)(0xa0 + i);

    buf = malloc(sizes[sizeof(sizes) / sizeof(sizes[0]) - 1]);
    if (buf == NULL)
    {
        fprintf(stderr, "bench: out of memory\n");
        return 1;
    }
    memset(buf, 0, sizes[sizeof(sizes) / sizeof(sizes[0]) - 1]);

    printf("cpu_mhz: %.0f\n", mhz);
    printf("buffer: hot cache, in-place xor, counter 0 per pass\n");
    printf("%-8s %8s %12s %12s %12s\n", "rounds", "bytes", "MB/s", "cycles/B",
           "1G core%");
    printf("1G core%% = 125 MB/s divided by this rate\n");

    for (r = 0; r < sizeof(rounds_list) / sizeof(rounds_list[0]); r++)
    {
        int rounds = rounds_list[r];

        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
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

            printf("%-8d %8zu %12.1f %12.2f %11.1f%%\n", rounds, len, mbps,
                   cpb, 125.0 / mbps * 100.0);
        }
    }

    free(buf);
    printf("sink: %llu\n", (unsigned long long)sink);
    return 0;
}
