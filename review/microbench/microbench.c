/*
 * microbench.c -- tbtun memory / GF(2) RLNC microbenchmark.
 *
 * Measures, per case, three repetitions and reports the median:
 *   1. memcpy of a large buffer                     (MB/s)
 *   2. XOR of two large buffers, 64-bit words       (MB/s)
 *   3. GF(2) RLNC encode: W packets per generation,
 *      every parity packet covers d packets          (MB/s, ns/byte)
 *   4. GF(2) RLNC decode: k missing packets,
 *      Gauss-Jordan elimination on the k x k matrix (MB/s, ns/byte)
 *
 * Nothing here touches the network, a kernel module or a privileged
 * interface.  Buffers are allocated and touched before timing so that
 * page faults are not measured.
 *
 * Build: cc -O3 -march=native -pthread -o microbench microbench.c
 *        (fall back to -O3 if -march=native is rejected)
 *
 * Usage: ./microbench [--mem MiB] [--threads N]
 *        --mem      per-buffer size for memcpy/XOR (default 256 MiB)
 *        --threads  threads for the multi-core memcpy/XOR rows (default 4)
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define PKT 1200u        /* payload bytes per packet */
#define GEN_W 16         /* generation width used by the decode test */
#define MAXK 8

static volatile uint64_t g_sink;

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double median3(double v[3])
{
	double a = v[0], b = v[1], c = v[2], t;

	if (a > b) { t = a; a = b; b = t; }
	if (b > c) { t = b; b = c; c = t; }
	if (a > b) { t = a; a = b; b = t; }
	return b;
}

static uint8_t *xalloc(size_t n)
{
	void *p = NULL;

	if (posix_memalign(&p, 64, n) != 0) {
		perror("posix_memalign");
		exit(1);
	}
	memset(p, 0xA5, n);   /* fault every page before timing */
	return p;
}

/* ---- CPU description -------------------------------------------------- */

static int is_flag(const char *t)
{
	static const char *want[] = {
		"sse2", "avx", "avx2", "avx512f", "avx512bw", "fma", "aes",
		"asimd", "neon", "sve", "sve2", "crc32", "pmull", "sha2",
		"sha3", "asimdrdm", "i8mm", "bf16",
	};
	size_t i;

	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
		if (!strcmp(t, want[i]))
			return 1;
	return 0;
}

static void print_cpuinfo(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[4096], model[256] = "", hw[256] = "", flags[4096] = "";
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
				if (c)
					snprintf(hw, sizeof(hw), "%s", c + 1);
			} else if ((!strncmp(line, "flags", 5) ||
				    !strncmp(line, "Features", 8)) && !flags[0]) {
				char *c = strchr(line, ':');
				if (c)
					snprintf(flags, sizeof(flags), "%s", c + 1);
			}
		}
		fclose(f);
	}

	if (model[0]) {
		char *nl = strchr(model, '\n');
		if (nl) *nl = 0;
	}
	if (hw[0]) {
		char *nl = strchr(hw, '\n');
		if (nl) *nl = 0;
	}

	printf("cpu_model:%s\n", model[0] ? model : (hw[0] ? hw : "unknown"));
	printf("cpu_cores:%d\n", cores ? cores : (int)sysconf(_SC_NPROCESSORS_ONLN));

	{
		FILE *cf = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
		long khz = 0;

		if (cf) {
			if (fscanf(cf, "%ld", &khz) != 1)
				khz = 0;
			fclose(cf);
		}
		printf("cpu_mhz_cpu0:%ld\n", khz / 1000);
	}

	printf("cpu_flags:");
	if (flags[0]) {
		char *save = NULL, *tok = strtok_r(flags, " \n", &save);

		while (tok) {
			if (is_flag(tok))
				printf(" %s", tok);
			tok = strtok_r(NULL, " \n", &save);
		}
	}
	printf("\n");

	printf("build_macros:");
#if defined(__AVX512F__)
	printf(" avx512f");
#endif
#if defined(__AVX2__)
	printf(" avx2");
#endif
#if defined(__AVX__)
	printf(" avx");
#endif
#if defined(__SSE2__)
	printf(" sse2");
#endif
#if defined(__ARM_NEON)
	printf(" neon");
#endif
#if defined(__ARM_FEATURE_SVE)
	printf(" sve");
#endif
#if defined(__aarch64__)
	printf(" aarch64");
#endif
	printf(" gcc=%d.%d\n", __GNUC__, __GNUC_MINOR__);
}

/* ---- primitives -------------------------------------------------------- */

static inline void xor_bytes(uint8_t *restrict d, const uint8_t *restrict s,
			     size_t n)
{
	uint64_t *restrict dw = (uint64_t *)d;
	const uint64_t *restrict sw = (const uint64_t *)s;
	size_t i, w = n / 8;

	for (i = 0; i < w; i++)
		dw[i] ^= sw[i];
	for (i = w * 8; i < n; i++)
		d[i] ^= s[i];
}

/* ---- memcpy / XOR ------------------------------------------------------ */

typedef struct {
	const uint8_t *src;
	uint8_t *dst;
	size_t n;
	int xor;
	pthread_barrier_t *bar;
} worker_arg;

static void *worker(void *p)
{
	worker_arg *a = p;

	pthread_barrier_wait(a->bar);
	if (a->xor)
		xor_bytes(a->dst, a->src, a->n);
	else
		memcpy(a->dst, a->src, a->n);
	return NULL;
}

/* Run one pass over `total` bytes with `nt` threads; timing starts after
 * every thread has been created and parked on the barrier. */
static double mt_pass(const uint8_t *src, uint8_t *dst, size_t total, int nt,
		      int use_xor)
{
	pthread_t th[16];
	worker_arg a[16];
	pthread_barrier_t bar;
	size_t chunk = (total / nt) & ~(size_t)7;
	double t0, t1;
	int i;

	if (nt > 16)
		nt = 16;
	pthread_barrier_init(&bar, NULL, nt + 1);
	for (i = 0; i < nt; i++) {
		a[i].src = src + (size_t)i * chunk;
		a[i].dst = dst + (size_t)i * chunk;
		a[i].n = chunk;
		a[i].xor = use_xor;
		a[i].bar = &bar;
		if (pthread_create(&th[i], NULL, worker, &a[i]) != 0) {
			perror("pthread_create");
			exit(1);
		}
	}
	t0 = now_s();
	pthread_barrier_wait(&bar);
	for (i = 0; i < nt; i++)
		pthread_join(th[i], NULL);
	t1 = now_s();
	pthread_barrier_destroy(&bar);

	/* aggregate MB/s over all threads */
	return (double)(chunk * (size_t)nt) / (t1 - t0) / 1e6;
}

/* ---- GF(2) RLNC encode ------------------------------------------------- */

static void encode_generation(const uint8_t *base, int W, int d, uint8_t *par)
{
	int j, t;

	for (j = 0; j < W; j++) {
		uint8_t *out = par + (size_t)j * PKT;

		memcpy(out, base + (size_t)j * PKT, PKT);
		for (t = 1; t < d; t++)
			xor_bytes(out, base + (size_t)((j + t) % W) * PKT, PKT);
	}
}

static double encode_mbps(const uint8_t *data, size_t total, int W, int d,
			  uint8_t *par, double *nsb)
{
	size_t gen = (size_t)W * PKT;
	size_t ng = total / gen;
	double best[3];
	int r;

	for (r = 0; r < 3; r++) {
		double t0 = now_s(), t1;
		size_t g;

		for (g = 0; g < ng; g++)
			encode_generation(data + g * gen, W, d, par);
		t1 = now_s();
		best[r] = (double)(ng * gen) / (t1 - t0) / 1e6;
		g_sink ^= par[0] ^ par[(size_t)(W - 1) * PKT];
	}
	*nsb = 1000.0 / median3(best);
	return median3(best);
}

/* ---- GF(2) RLNC decode ------------------------------------------------- */

/* Gauss-Jordan inverse of a k x k bit matrix; returns -1 if singular. */
static int gf2_invert(const uint32_t *rows, int k, uint32_t *inv)
{
	uint32_t m[MAXK], iv[MAXK];
	int c, r;

	for (r = 0; r < k; r++) {
		m[r] = rows[r] & ((1u << k) - 1u);
		iv[r] = 1u << r;
	}
	for (c = 0; c < k; c++) {
		int piv = -1;

		for (r = c; r < k; r++)
			if ((m[r] >> c) & 1u) {
				piv = r;
				break;
			}
		if (piv < 0)
			return -1;
		if (piv != c) {
			uint32_t t = m[c]; m[c] = m[piv]; m[piv] = t;
			t = iv[c]; iv[c] = iv[piv]; iv[piv] = t;
		}
		for (r = 0; r < k; r++) {
			if (r != c && ((m[r] >> c) & 1u)) {
				m[r] ^= m[c];
				iv[r] ^= iv[c];
			}
		}
	}
	for (r = 0; r < k; r++)
		inv[r] = iv[r];
	return 0;
}

/* Recover k packets of one generation.  The k parity packets were built
 * as A * missing + (present packets assigned one-to-one to parities). */
static void decode_generation(const uint8_t *base, const uint8_t *par, int k,
			      const uint32_t *A, uint8_t *rec)
{
	uint32_t inv[MAXK];
	int i, j, p;

	if (gf2_invert(A, k, inv) != 0) {
		fprintf(stderr, "singular matrix\n");
		exit(1);
	}
	for (i = 0; i < k; i++) {
		uint8_t *out = rec + (size_t)i * PKT;

		memset(out, 0, PKT);
		for (j = 0; j < k; j++)
			if ((inv[i] >> j) & 1u)
				xor_bytes(out, par + (size_t)j * PKT, PKT);
		for (p = k; p < GEN_W; p++)
			if ((inv[i] >> ((p - k) % k)) & 1u)
				xor_bytes(out, base + (size_t)p * PKT, PKT);
	}
}

/* Deterministic pseudo-random invertible k x k matrix. */
static void make_matrix(int k, uint32_t *A)
{
	uint64_t s = 0x9e3779b97f4a7c15ull ^ (uint64_t)k;
	uint32_t inv[MAXK];
	int i, tries;

	for (tries = 0; tries < 4096; tries++) {
		for (i = 0; i < k; i++) {
			s ^= s << 13; s ^= s >> 7; s ^= s << 17;
			A[i] = (uint32_t)(s >> 32) & ((1u << k) - 1u);
		}
		if (gf2_invert(A, k, inv) == 0)
			return;
	}
	fprintf(stderr, "no invertible matrix\n");
	exit(1);
}

static void build_parities(const uint8_t *data, size_t total, int k,
			   const uint32_t *A, uint8_t *par)
{
	size_t gen = (size_t)GEN_W * PKT;
	size_t ng = total / gen;
	size_t g;
	int j, i, p;

	for (g = 0; g < ng; g++) {
		const uint8_t *base = data + g * gen;

		for (j = 0; j < k; j++) {
			uint8_t *pj = par + (g * (size_t)k + (size_t)j) * PKT;

			memset(pj, 0, PKT);
			for (i = 0; i < k; i++)
				if ((A[j] >> i) & 1u)
					xor_bytes(pj, base + (size_t)i * PKT, PKT);
			for (p = k; p < GEN_W; p++)
				if (((p - k) % k) == j)
					xor_bytes(pj, base + (size_t)p * PKT, PKT);
		}
	}
}

static double decode_mbps(const uint8_t *data, size_t total, int k,
			  const uint32_t *A, const uint8_t *par, uint8_t *rec,
			  double *nsb)
{
	size_t gen = (size_t)GEN_W * PKT;
	size_t ng = total / gen;
	double best[3];
	int r;

	for (r = 0; r < 3; r++) {
		double t0 = now_s(), t1;
		size_t g;

		for (g = 0; g < ng; g++)
			decode_generation(data + g * gen,
					  par + g * (size_t)k * PKT, k, A, rec);
		t1 = now_s();
		best[r] = (double)(ng * (size_t)k * PKT) / (t1 - t0) / 1e6;
		g_sink ^= rec[0] ^ rec[(size_t)(k - 1) * PKT];
	}
	*nsb = 1000.0 / median3(best);
	return median3(best);
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
	size_t mem_mib = 256, total;
	int nt = 4, i, r;
	uint8_t *src, *dst, *par, *rec;
	double mb[3], ns;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--mem") && i + 1 < argc)
			mem_mib = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
			nt = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [--mem MiB] [--threads N]\n",
				argv[0]);
			return 2;
		}
	}
	if (mem_mib < 1)
		mem_mib = 1;
	total = mem_mib * MIB;
	if (nt < 1)
		nt = 1;

	print_cpuinfo();
	printf("argv_mem_mib:%zu\nargv_threads:%d\n", mem_mib, nt);
	printf("packet_bytes:%u generation_packets:%u rlnc_data_bytes:%u\n",
	       PKT, GEN_W, MIB);

	src = xalloc(total);
	dst = xalloc(total);

	/* memcpy, 1 thread */
	for (r = 0; r < 3; r++) {
		double t0 = now_s(), t1;

		memcpy(dst, src, total);
		t1 = now_s();
		mb[r] = (double)total / (t1 - t0) / 1e6;
	}
	g_sink ^= dst[0] ^ dst[total - 1];
	printf("RESULT memcpy threads=1 mbps=%.1f ns_per_byte=%.4f\n",
	       median3(mb), 1000.0 / median3(mb));

	/* XOR, 1 thread */
	for (r = 0; r < 3; r++) {
		double t0 = now_s(), t1;

		xor_bytes(dst, src, total);
		t1 = now_s();
		mb[r] = (double)total / (t1 - t0) / 1e6;
	}
	g_sink ^= dst[0] ^ dst[total - 1];
	printf("RESULT xor threads=1 mbps=%.1f ns_per_byte=%.4f\n",
	       median3(mb), 1000.0 / median3(mb));

	/* multi-core memcpy / XOR */
	if (nt > 1) {
		double m1 = mt_pass(src, dst, total, nt, 0);
		double x1 = mt_pass(src, dst, total, nt, 1);

		printf("RESULT memcpy threads=%d mbps=%.1f ns_per_byte=%.4f\n",
		       nt, m1, 1000.0 / m1);
		printf("RESULT xor threads=%d mbps=%.1f ns_per_byte=%.4f\n",
		       nt, x1, 1000.0 / x1);
	}
	free(dst);
	free(src);

	/* RLNC encode over 1 MiB of packet data */
	{
		const size_t rlnc_total = 1u * MIB;
		uint8_t *data = xalloc(rlnc_total);
		static const int ws[] = { 8, 16, 32 };
		static const int ds[] = { 8, 12, 16 };

		par = xalloc(32 * PKT);
		for (i = 0; i < 3; i++) {
			int j;

			for (j = 0; j < 3; j++) {
				double m;

				if (ds[j] > ws[i]) {
					printf("SKIP encode W=%d d=%d (d>W)\n",
					       ws[i], ds[j]);
					continue;
				}
				m = encode_mbps(data, rlnc_total, ws[i], ds[j],
						par, &ns);
				printf("RESULT encode W=%d d=%d mbps=%.1f ns_per_byte=%.4f\n",
				       ws[i], ds[j], m, ns);
			}
		}
		free(par);

		/* RLNC decode, W = GEN_W */
		par = xalloc((rlnc_total / ((size_t)GEN_W * PKT)) * MAXK * PKT);
		rec = xalloc(MAXK * PKT);
		for (i = 2; i <= 8; i *= 2) {
			uint32_t A[MAXK];
			double m;

			make_matrix(i, A);
			build_parities(data, rlnc_total, i, A, par);
			/* untimed correctness check on generation 0 */
			decode_generation(data, par, i, A, rec);
			if (memcmp(rec, data, (size_t)i * PKT) != 0) {
				fprintf(stderr, "decode verify FAILED k=%d\n", i);
				return 1;
			}
			m = decode_mbps(data, rlnc_total, i, A, par, rec, &ns);
			printf("RESULT decode W=%d k=%d mbps=%.1f ns_per_byte=%.4f verify=ok\n",
			       GEN_W, i, m, ns);
		}
		free(par);
		free(rec);
		free(data);
	}

	printf("sink:%llu\n", (unsigned long long)g_sink);
	return 0;
}
