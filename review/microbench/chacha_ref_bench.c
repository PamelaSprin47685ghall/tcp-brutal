/*
 * chacha_ref_bench.c -- ChaCha8/12/20 benchmark around Bernstein's
 * public-domain eSTREAM reference sources (chacha-ref.c version 20080118).
 *
 * The three files under chacha-ref/ are vendored verbatim; the only addition
 * is chacha-ref/ecrypt-sync.h, a minimal type/endianness shim.  Each file is
 * compiled with its ECRYPT_* symbols renamed (see run_chacha_ref.sh) so all
 * three round counts can live in one binary.
 *
 * Test vectors from review/chacha8-reference.md (commit 040304a):
 *   TC1: 128-bit zero key, 8-byte zero IV, 8 rounds, counter 0.
 *   256: key 00..1f, IV a0..a7, 256-bit key, 8 rounds, counter 0.
 * Both are compared byte for byte before any timing.
 *
 * Build: see run_chacha_ref.sh
 * Usage: ./chacha_ref_bench [--bytes MiB] [--reps N]
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chacha-ref/ecrypt-sync.h"

#define MIB (1024u * 1024u)

typedef void (*ks_fn)(ECRYPT_ctx *, const u8 *, u32, u32);
typedef void (*iv_fn)(ECRYPT_ctx *, const u8 *);
typedef void (*enc_fn)(ECRYPT_ctx *, const u8 *, u8 *, u32);

void ref8_init(void);
void ref8_keysetup(ECRYPT_ctx *, const u8 *, u32, u32);
void ref8_ivsetup(ECRYPT_ctx *, const u8 *);
void ref8_encrypt_bytes(ECRYPT_ctx *, const u8 *, u8 *, u32);
void ref12_init(void);
void ref12_keysetup(ECRYPT_ctx *, const u8 *, u32, u32);
void ref12_ivsetup(ECRYPT_ctx *, const u8 *);
void ref12_encrypt_bytes(ECRYPT_ctx *, const u8 *, u8 *, u32);
void ref20_init(void);
void ref20_keysetup(ECRYPT_ctx *, const u8 *, u32, u32);
void ref20_ivsetup(ECRYPT_ctx *, const u8 *);
void ref20_encrypt_bytes(ECRYPT_ctx *, const u8 *, u8 *, u32);

typedef struct {
	const char *name;
	int rounds;
	ks_fn keysetup;
	iv_fn ivsetup;
	enc_fn encrypt;
} variant;

static const variant variants[3] = {
	{ "chacha8",  8,  ref8_keysetup,  ref8_ivsetup,  ref8_encrypt_bytes  },
	{ "chacha12", 12, ref12_keysetup, ref12_ivsetup, ref12_encrypt_bytes },
	{ "chacha20", 20, ref20_keysetup, ref20_ivsetup, ref20_encrypt_bytes },
};

static volatile uint64_t g_sink;

static double now_cpu(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double median(double *v, int n)
{
	int i, j;
	double t;

	for (i = 1; i < n; i++) {
		t = v[i];
		for (j = i - 1; j >= 0 && v[j] > t; j--)
			v[j + 1] = v[j];
		v[j + 1] = t;
	}
	return v[n / 2];
}

static long cpu_mhz(void)
{
	FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
	long khz = 0;

	if (f) {
		if (fscanf(f, "%ld", &khz) != 1)
			khz = 0;
		fclose(f);
	}
	if (khz > 0)
		return khz / 1000;
	f = fopen("/proc/cpuinfo", "r");
	if (f) {
		char line[256];

		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "cpu MHz", 7)) {
				double m = 0;

				if (sscanf(line, "cpu MHz : %lf", &m) == 1)
					khz = (long)(m * 1000);
				break;
			}
		}
		fclose(f);
	}
	return khz / 1000;
}

static void print_cpu(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[1024], model[256] = "", flags[4096] = "";
	int cores = 0;

	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "processor", 9)) {
				cores++;
			} else if (!strncmp(line, "model name", 10)) {
				char *c = strchr(line, ':');
				if (c && !model[0])
					snprintf(model, sizeof(model), "%s", c + 1);
			} else if (!strncmp(line, "Hardware", 8)) {
				char *c = strchr(line, ':');
				if (c && !model[0])
					snprintf(model, sizeof(model), "%s", c + 1);
			} else if ((!strncmp(line, "flags", 5) ||
				    !strncmp(line, "Features", 8)) && !flags[0]) {
				char *c = strchr(line, ':');
				if (c)
					snprintf(flags, sizeof(flags), "%s", c + 1);
			}
		}
		fclose(f);
	}
	{
		char *nl = strchr(model, '\n');
		if (nl) *nl = 0;
	}
	printf("cpu_model:%s\n", model[0] ? model : "unknown");
	printf("cpu_cores:%d cpu_mhz:%ld\n", cores, cpu_mhz());
	{
		static const char *want[] = { "sse2", "ssse3", "avx", "avx2",
					      "avx512f", "asimd", "neon", "sve",
					      "sve2", "aes", "pmull" };
		char *save = NULL, *tok;
		size_t k;

		printf("cpu_flags:");
		for (tok = strtok_r(flags, " \n", &save); tok;
		     tok = strtok_r(NULL, " \n", &save)) {
			for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
				if (!strcmp(tok, want[k])) {
					printf(" %s", tok);
					break;
				}
			}
		}
		printf("\n");
	}
}

static const u8 tc1_key[16] = { 0 };
static const u8 tc1_iv[8] = { 0 };
static const u8 tc1_ks[64] = {
	0xe2, 0x8a, 0x5f, 0xa4, 0xa6, 0x7f, 0x8c, 0x5d,
	0xef, 0xed, 0x3e, 0x6f, 0xb7, 0x30, 0x34, 0x86,
	0xaa, 0x84, 0x27, 0xd3, 0x14, 0x19, 0xa7, 0x29,
	0x57, 0x2d, 0x77, 0x79, 0x53, 0x49, 0x11, 0x20,
	0xb6, 0x4a, 0xb8, 0xe7, 0x2b, 0x8d, 0xeb, 0x85,
	0xcd, 0x6a, 0xea, 0x7c, 0xb6, 0x08, 0x9a, 0x10,
	0x18, 0x24, 0xbe, 0xeb, 0x08, 0x81, 0x4a, 0x42,
	0x8a, 0xab, 0x1f, 0xa2, 0xc8, 0x16, 0x08, 0x1b,
};

static const u8 v256_key[32] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const u8 v256_iv[8] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7 };
static const u8 v256_ks[64] = {
	0x9a, 0x1e, 0x94, 0x9f, 0x0d, 0x71, 0xa3, 0xa3,
	0x07, 0xda, 0x58, 0x73, 0x03, 0x00, 0xde, 0xa7,
	0x25, 0x34, 0x2e, 0xe4, 0x5c, 0xf0, 0x66, 0xd8,
	0x35, 0x00, 0x95, 0xe3, 0xc4, 0x43, 0xce, 0xe2,
	0x68, 0xdb, 0x83, 0xe2, 0xce, 0x8d, 0x89, 0x9d,
	0x90, 0xe9, 0x6a, 0xef, 0x64, 0x4b, 0x1c, 0x84,
	0x93, 0xc2, 0x28, 0xcd, 0xaa, 0x42, 0x0c, 0x56,
	0x03, 0xd7, 0x45, 0xf1, 0x41, 0x27, 0xb4, 0xba,
};

static int check_vector(const char *name, const u8 *key, u32 kbits,
			const u8 *iv, const u8 *want)
{
	ECRYPT_ctx ctx;
	u8 zero[64] = { 0 }, out[64];
	int i;

	ref8_init();
	ref8_keysetup(&ctx, key, kbits, 64);
	ref8_ivsetup(&ctx, iv);
	ref8_encrypt_bytes(&ctx, zero, out, sizeof(out));
	for (i = 0; i < 64; i++) {
		if (out[i] != want[i]) {
			printf("SELFTEST %s FAIL at byte %d: got %02x want %02x\n",
			       name, i, out[i], want[i]);
			return 1;
		}
	}
	printf("SELFTEST %s PASS\n", name);
	return 0;
}

static void fill(u8 *p, size_t n)
{
	size_t i;
	uint64_t x = 0x243f6a8885a308d3ull;

	for (i = 0; i < n; i++) {
		if ((i & 7) == 0) {
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		}
		p[i] = (u8)(x >> (8 * (i & 7)));
	}
}

int main(int argc, char **argv)
{
	size_t bytes = 4 * MIB;
	int reps = 15, r, k;
	long mhz_override = 0;
	u8 *in, *out;
	u8 key[32], nonce[8];
	long mhz;

	for (r = 1; r < argc; r++) {
		if (!strcmp(argv[r], "--bytes") && r + 1 < argc)
			bytes = (size_t)strtoul(argv[++r], NULL, 10) * MIB;
		else if (!strcmp(argv[r], "--reps") && r + 1 < argc)
			reps = atoi(argv[++r]);
		else if (!strcmp(argv[r], "--mhz") && r + 1 < argc)
			mhz_override = strtol(argv[++r], NULL, 10);
		else {
			fprintf(stderr, "usage: %s [--bytes MiB] [--reps N] [--mhz N]\n",
				argv[0]);
			return 2;
		}
	}
	if (bytes < 64 || reps < 1)
		return 2;

	print_cpu();
	mhz = mhz_override > 0 ? mhz_override : cpu_mhz();
	printf("impl:Bernstein chacha-ref 20080118 (public domain), 8/12/20\n");
	printf("argv_bytes:%zu reps:%d\n", bytes, reps);

	if (check_vector("tc1", tc1_key, 128, tc1_iv, tc1_ks))
		return 1;
	if (check_vector("v256", v256_key, 256, v256_iv, v256_ks))
		return 1;

	in = malloc(bytes);
	out = malloc(bytes);
	if (!in || !out) {
		perror("malloc");
		return 1;
	}
	fill(in, bytes);
	memset(key, 0x42, sizeof(key));
	memset(nonce, 0x24, sizeof(nonce));

	for (k = 0; k < 3; k++) {
		double *v = malloc((size_t)reps * sizeof(double));

		if (!v)
			return 1;
		for (r = 0; r < reps; r++) {
			ECRYPT_ctx ctx;
			double t0, t1;

			variants[k].keysetup(&ctx, key, 256, 64);
			variants[k].ivsetup(&ctx, nonce);
			t0 = now_cpu();
			variants[k].encrypt(&ctx, in, out, (u32)bytes);
			t1 = now_cpu();
			v[r] = (double)bytes / (t1 - t0) / 1e6;
		}
		{
			double mbps = median(v, reps);
			double best = v[0];
			double nsb, cpb;
			int i;

			for (i = 1; i < reps; i++)
				if (v[i] > best)
					best = v[i];
			nsb = 1000.0 / best;
			cpb = mhz > 0 ? (double)mhz * 1e6 * nsb * 1e-9 : 0.0;
			printf("RESULT %s mbps=%.1f mbps_max=%.1f "
			       "ns_per_byte=%.4f cycles_per_byte=%.2f\n",
			       variants[k].name, mbps, best, nsb, cpb);
		}
		free(v);
	}
	g_sink = out[0] ^ out[bytes - 1];
	printf("sink:%llu\n", (unsigned long long)g_sink);
	free(in);
	free(out);
	return 0;
}
