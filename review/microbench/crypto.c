/*
 * crypto.c -- ChaCha20 / ChaCha12 / ChaCha8 stream-cipher microbenchmark.
 *
 * Scalar C implementation (RFC 8439 layout, 32-bit words).  It measures the
 * keystream+XOR throughput on a hot 1 MiB buffer, three repetitions, median.
 * The AES-128-CBC anchor is measured separately with `openssl speed` by the
 * run script; this program deliberately does not link a crypto library.
 *
 * Build: cc -O3 -march=native -o crypto crypto.c   (fallback: -O3)
 * Usage: ./crypto [--bytes MiB] [--reps N]
 *
 * Nothing here touches the network or a kernel module.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024u * 1024u)

static volatile uint64_t g_sink;

/* Process CPU time, not wall time: a busy host then cannot turn a
 * preemption into a fake throughput collapse. */
static double now_s(void)
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

static inline uint32_t rotl32(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

#define QR(a, b, c, d)                    \
	do {                              \
		a += b; d ^= a; d = rotl32(d, 16); \
		c += d; b ^= c; b = rotl32(b, 12); \
		a += b; d ^= a; d = rotl32(d, 8);  \
		c += d; b ^= c; b = rotl32(b, 7);  \
	} while (0)

static void chacha_block(uint32_t out[16], const uint32_t in[16], int rounds)
{
	uint32_t x0 = in[0], x1 = in[1], x2 = in[2], x3 = in[3];
	uint32_t x4 = in[4], x5 = in[5], x6 = in[6], x7 = in[7];
	uint32_t x8 = in[8], x9 = in[9], x10 = in[10], x11 = in[11];
	uint32_t x12 = in[12], x13 = in[13], x14 = in[14], x15 = in[15];
	int i;

	for (i = 0; i < rounds; i += 2) {
		QR(x0, x4, x8,  x12);
		QR(x1, x5, x9,  x13);
		QR(x2, x6, x10, x14);
		QR(x3, x7, x11, x15);
		QR(x0, x5, x10, x15);
		QR(x1, x6, x11, x12);
		QR(x2, x7, x8,  x13);
		QR(x3, x4, x9,  x14);
	}
	out[0] = x0 + in[0];   out[1] = x1 + in[1];
	out[2] = x2 + in[2];   out[3] = x3 + in[3];
	out[4] = x4 + in[4];   out[5] = x5 + in[5];
	out[6] = x6 + in[6];   out[7] = x7 + in[7];
	out[8] = x8 + in[8];   out[9] = x9 + in[9];
	out[10] = x10 + in[10]; out[11] = x11 + in[11];
	out[12] = x12 + in[12]; out[13] = x13 + in[13];
	out[14] = x14 + in[14]; out[15] = x15 + in[15];
}

static void chacha_xor(uint8_t *buf, size_t len, int rounds,
		       const uint8_t key[32], const uint8_t nonce[12])
{
	static const uint32_t sigma[4] = {
		0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
	};
	uint32_t st[16], ks[16];
	uint32_t *b = (uint32_t *)buf;
	size_t nw = len / 4, i;
	int j;

	st[0] = sigma[0]; st[1] = sigma[1]; st[2] = sigma[2]; st[3] = sigma[3];
	for (i = 0; i < 8; i++)
		st[4 + i] = (uint32_t)key[i * 4] | ((uint32_t)key[i * 4 + 1] << 8) |
			    ((uint32_t)key[i * 4 + 2] << 16) |
			    ((uint32_t)key[i * 4 + 3] << 24);
	for (i = 0; i < 3; i++)
		st[13 + i] = (uint32_t)nonce[i * 4] |
			     ((uint32_t)nonce[i * 4 + 1] << 8) |
			     ((uint32_t)nonce[i * 4 + 2] << 16) |
			     ((uint32_t)nonce[i * 4 + 3] << 24);
	st[12] = 0;   /* 32-bit block counter (RFC 8439 layout) */

	for (i = 0; i + 16 <= nw; i += 16) {
		chacha_block(ks, st, rounds);
		for (j = 0; j < 16; j++)
			b[i + j] ^= ks[j];
		st[12]++;
	}
}


/* Four blocks in parallel using GCC vector extensions: 16 bytes on x86-64
 * (SSE2) and AArch64 (NEON).  Only the block counter differs between lanes,
 * so the whole state is broadcast and the counter vector is built once. */
typedef uint32_t v4u __attribute__((vector_size(16)));

static inline v4u vrotl(v4u x, int n)
{
	return (x << n) | (x >> (32 - n));
}

#define QR4V(a, b, c, d)                    \
	do {                                \
		a += b; d ^= a; d = vrotl(d, 16); \
		c += d; b ^= c; b = vrotl(b, 12); \
		a += b; d ^= a; d = vrotl(d, 8);  \
		c += d; b ^= c; b = vrotl(b, 7);  \
	} while (0)

static void chacha_xor4v(uint8_t *buf, size_t len, int rounds,
			 const uint8_t key[32], const uint8_t nonce[12])
{
	static const uint32_t sigma[4] = {
		0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
	};
	uint32_t st[16], t[16][4];
	uint32_t *b = (uint32_t *)buf;
	size_t nw = len / 4, i;
	int j, l, r;

	st[0] = sigma[0]; st[1] = sigma[1]; st[2] = sigma[2]; st[3] = sigma[3];
	for (i = 0; i < 8; i++)
		st[4 + i] = (uint32_t)key[i * 4] | ((uint32_t)key[i * 4 + 1] << 8) |
			    ((uint32_t)key[i * 4 + 2] << 16) |
			    ((uint32_t)key[i * 4 + 3] << 24);
	for (i = 0; i < 3; i++)
		st[13 + i] = (uint32_t)nonce[i * 4] |
			     ((uint32_t)nonce[i * 4 + 1] << 8) |
			     ((uint32_t)nonce[i * 4 + 2] << 16) |
			     ((uint32_t)nonce[i * 4 + 3] << 24);
	st[12] = 0;

	for (i = 0; i + 64 <= nw; i += 64) {
		v4u x0, x1, x2, x3, x4, x5, x6, x7;
		v4u x8, x9, x10, x11, x12, x13, x14, x15;
		v4u o0, o1, o2, o3, o4, o5, o6, o7;
		v4u o8, o9, o10, o11, o12, o13, o14, o15;
		v4u ctr = { st[12], st[12] + 1, st[12] + 2, st[12] + 3 };

		o0 = (v4u){ st[0], st[0], st[0], st[0] };
		o1 = (v4u){ st[1], st[1], st[1], st[1] };
		o2 = (v4u){ st[2], st[2], st[2], st[2] };
		o3 = (v4u){ st[3], st[3], st[3], st[3] };
		o4 = (v4u){ st[4], st[4], st[4], st[4] };
		o5 = (v4u){ st[5], st[5], st[5], st[5] };
		o6 = (v4u){ st[6], st[6], st[6], st[6] };
		o7 = (v4u){ st[7], st[7], st[7], st[7] };
		o8 = (v4u){ st[8], st[8], st[8], st[8] };
		o9 = (v4u){ st[9], st[9], st[9], st[9] };
		o10 = (v4u){ st[10], st[10], st[10], st[10] };
		o11 = (v4u){ st[11], st[11], st[11], st[11] };
		o12 = ctr;
		o13 = (v4u){ st[13], st[13], st[13], st[13] };
		o14 = (v4u){ st[14], st[14], st[14], st[14] };
		o15 = (v4u){ st[15], st[15], st[15], st[15] };
		x0 = o0; x1 = o1; x2 = o2; x3 = o3;
		x4 = o4; x5 = o5; x6 = o6; x7 = o7;
		x8 = o8; x9 = o9; x10 = o10; x11 = o11;
		x12 = o12; x13 = o13; x14 = o14; x15 = o15;

		for (r = 0; r < rounds; r += 2) {
			QR4V(x0, x4, x8,  x12);
			QR4V(x1, x5, x9,  x13);
			QR4V(x2, x6, x10, x14);
			QR4V(x3, x7, x11, x15);
			QR4V(x0, x5, x10, x15);
			QR4V(x1, x6, x11, x12);
			QR4V(x2, x7, x8,  x13);
			QR4V(x3, x4, x9,  x14);
		}
		st[12] += 4;

		*(v4u *)t[0] = x0 + o0;   *(v4u *)t[1] = x1 + o1;
		*(v4u *)t[2] = x2 + o2;   *(v4u *)t[3] = x3 + o3;
		*(v4u *)t[4] = x4 + o4;   *(v4u *)t[5] = x5 + o5;
		*(v4u *)t[6] = x6 + o6;   *(v4u *)t[7] = x7 + o7;
		*(v4u *)t[8] = x8 + o8;   *(v4u *)t[9] = x9 + o9;
		*(v4u *)t[10] = x10 + o10; *(v4u *)t[11] = x11 + o11;
		*(v4u *)t[12] = x12 + o12; *(v4u *)t[13] = x13 + o13;
		*(v4u *)t[14] = x14 + o14; *(v4u *)t[15] = x15 + o15;
		for (l = 0; l < 4; l++)
			for (j = 0; j < 16; j++)
				b[i + l * 16 + j] ^= t[j][l];
	}
}

static void fill(uint8_t *p, size_t n)
{
	size_t i;
	uint64_t x = 0x243f6a8885a308d3ull;

	for (i = 0; i < n; i++) {
		if ((i & 7) == 0) {
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		}
		p[i] = (uint8_t)(x >> (8 * (i & 7)));
	}
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

int main(int argc, char **argv)
{
	size_t bytes = 1 * MIB;
	int reps = 5, r;
	uint8_t *buf;
	uint8_t key[32], nonce[12];
	long mhz;
	static const int rounds[] = { 20, 12, 8 };
	size_t k;

	for (r = 1; r < argc; r++) {
		if (!strcmp(argv[r], "--bytes") && r + 1 < argc)
			bytes = (size_t)strtoul(argv[++r], NULL, 10) * MIB;
		else if (!strcmp(argv[r], "--reps") && r + 1 < argc)
			reps = atoi(argv[++r]);
		else {
			fprintf(stderr, "usage: %s [--bytes MiB] [--reps N]\n",
				argv[0]);
			return 2;
		}
	}
	if (bytes < 64 || reps < 1)
		return 2;

	print_cpu();
	mhz = cpu_mhz();
	printf("argv_bytes:%zu reps:%d\n", bytes, reps);

	buf = malloc(bytes);
	if (!buf) {
		perror("malloc");
		return 1;
	}
	fill(buf, bytes);
	memset(key, 0x42, sizeof(key));
	memset(nonce, 0x24, sizeof(nonce));

	for (k = 0; k < sizeof(rounds) / sizeof(rounds[0]); k++) {
		double *v = malloc((size_t)reps * sizeof(double));
		int w;

		for (w = 0; w < 2; w++) {
			for (r = 0; r < reps; r++) {
				double t0 = now_s(), t1;

				if (w == 0)
					chacha_xor(buf, bytes, rounds[k], key, nonce);
				else
					chacha_xor4v(buf, bytes, rounds[k], key, nonce);
				t1 = now_s();
				v[r] = (double)bytes / (t1 - t0) / 1e6;
			}
			{
				double mbps = median(v, reps);
				double nsb = 1000.0 / mbps;
				double cpb = mhz > 0 ?
					(double)mhz * 1e6 * nsb * 1e-9 : 0.0;

				printf("RESULT chacha%d%s mbps=%.1f ns_per_byte=%.4f "
				       "cycles_per_byte=%.2f\n", rounds[k],
				       w ? "x4" : "", mbps, nsb, cpb);
			}
		}
	}
	g_sink = buf[0] ^ buf[bytes - 1];
	printf("sink:%llu\n", (unsigned long long)g_sink);
	free(buf);
	return 0;
}
