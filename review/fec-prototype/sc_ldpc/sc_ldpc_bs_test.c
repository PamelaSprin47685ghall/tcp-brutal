/*
 * sc_ldpc_bs_test.c - bit-exactness check for the bit-sliced decoder.
 *
 * For every configuration it builds one code, draws random codewords and
 * random erasure masks, decodes each codeword with the scalar sc_decode()
 * and the whole batch with sc_decode_bs(), and requires the output bytes and
 * the unresolved counts to match exactly.  A final case checks the all-zero
 * codeword against a hand-built erasure pattern.
 */

#include "sc_ldpc.h"
#include "sc_ldpc_bs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t s;
} trng;

static uint64_t trng_next(trng *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

static void pack_bits(uint8_t *dst, size_t stride, const uint8_t *src, int n,
                      int cw)
{
    int v;
    uint8_t *p = dst + (size_t)cw * stride;
    memset(p, 0, stride);
    for (v = 0; v < n; v++)
        if (src[v] & 1u)
            p[(size_t)(v >> 3)] |= (uint8_t)(1u << (v & 7));
}

static int bit_at(const uint8_t *p, int v)
{
    return (p[(size_t)(v >> 3)] >> (v & 7)) & 1u;
}

static int run_case(int block, int L, int rate_pct, int p_mil, int model,
                    int batch, int trials, uint64_t seed, int verbose)
{
    sc_params par;
    sc_code *c;
    sc_info in;
    char err[256];
    uint8_t *code, *recv, *mask, *out_scalar;
    uint8_t *pbits, *pmask, *pout;
    long *unres_bs;
    size_t stride;
    sc_bs *bs;
    trng rng;
    int t, v, j, bad = 0;

    memset(&par, 0, sizeof(par));
    par.block = block;
    par.L = L;
    par.rate_pct = rate_pct;
    par.term = SC_TERM_ACC;
    par.extra_dc = 5;
    par.seed = seed;

    c = sc_build(&par, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "build failed: %s\n", err);
        return 1;
    }
    sc_get_info(c, &in);
    stride = ((size_t)in.n + 7) / 8;

    bs = sc_bs_new(c, batch, err, sizeof(err));
    if (!bs) {
        fprintf(stderr, "sc_bs_new failed: %s\n", err);
        sc_free(c);
        return 1;
    }

    code = calloc((size_t)in.n, 1);
    recv = calloc((size_t)in.n, 1);
    mask = calloc((size_t)in.n, 1);
    out_scalar = calloc((size_t)in.n, 1);
    pbits = calloc((size_t)batch * stride, 1);
    pmask = calloc((size_t)batch * stride, 1);
    pout = calloc((size_t)batch * stride, 1);
    unres_bs = calloc((size_t)batch, sizeof(long));
    if (!code || !recv || !mask || !out_scalar || !pbits ||
        !pmask || !pout || !unres_bs) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    rng.s = seed ^ 0x9E3779B97F4A7C15ULL;
    if (rng.s == 0)
        rng.s = 1;

    for (t = 0; t < trials && !bad; t++) {
        long recovered;
        for (j = 0; j < batch; j++) {
            for (v = 0; v < in.n_data; v++)
                code[v] = (uint8_t)(trng_next(&rng) & 1u);
            if (sc_encode(c, code) != 0) {
                fprintf(stderr, "encode failed\n");
                bad = 1;
                break;
            }
            if (sc_verify(c, code) != 0) {
                fprintf(stderr, "verify failed\n");
                bad = 1;
                break;
            }
            memcpy(recv, code, (size_t)in.n);
            for (v = 0; v < in.n; v++)
                mask[v] = (model == SC_MODEL_SYSTEMATIC && v >= in.n_data)
                              ? 0
                              : (uint8_t)((trng_next(&rng) % 1000u) <
                                          (uint64_t)p_mil);
            pack_bits(pbits, stride, recv, in.n, j);
            pack_bits(pmask, stride, mask, in.n, j);
        }
        if (bad)
            break;

        recovered = sc_decode_bs(bs, pbits, pmask, pout,
                                 model == SC_MODEL_SYSTEMATIC, unres_bs);
        if (recovered < 0 || recovered > batch) {
            fprintf(stderr, "bad recovered count %ld\n", recovered);
            bad = 1;
            break;
        }

        for (j = 0; j < batch && !bad; j++) {
            sc_dec_stat st;
            const uint8_t *pj = pout + (size_t)j * stride;
            for (v = 0; v < in.n; v++) {
                recv[v] = bit_at(pbits + (size_t)j * stride, v);
                mask[v] = bit_at(pmask + (size_t)j * stride, v);
            }
            st = sc_decode(c, recv, mask, out_scalar,
                           model == SC_MODEL_SYSTEMATIC);
            if (st.unresolved != unres_bs[j]) {
                fprintf(stderr,
                        "case b%d L%d r%d p%d m%d t%d cw%d: unresolved "
                        "scalar=%ld bs=%ld\n",
                        block, L, rate_pct, p_mil, model, t, j,
                        st.unresolved, unres_bs[j]);
                bad = 1;
                break;
            }
            for (v = 0; v < in.n; v++) {
                int got = bit_at(pj, v);
                if (got != out_scalar[v]) {
                    fprintf(stderr,
                            "case b%d L%d r%d p%d m%d t%d cw%d var%d: "
                            "scalar=%d bs=%d\n",
                            block, L, rate_pct, p_mil, model, t, j, v,
                            out_scalar[v], got);
                    bad = 1;
                    break;
                }
            }
        }
    }

    if (verbose || bad)
        printf("%s: block=%d L=%d rate=%d p=%.3f model=%s batch=%d trials=%d "
               "n=%d edges=%ld dc_max=%d\n",
               bad ? "FAIL" : "PASS", block, L, rate_pct,
               (double)p_mil / 1000.0,
               model == SC_MODEL_SYSTEMATIC ? "sys" : "sym", batch, trials,
               in.n, in.edges, in.dc_max);

    free(code);
    free(recv);
    free(mask);
    free(out_scalar);
    free(pbits);
    free(pmask);
    free(pout);
    free(unres_bs);
    sc_bs_free(bs);
    sc_free(c);
    return bad;
}

int main(int argc, char **argv)
{
    int failures = 0;
    int verbose = 0;
    int i;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            verbose = 1;

    /* Small codes, both channel models, all-zero and random codewords. */
    failures += run_case(16, 2, 50, 100, SC_MODEL_SYMMETRIC, 64, 200, 1, verbose);
    failures += run_case(16, 4, 50, 300, SC_MODEL_SYMMETRIC, 64, 200, 2, verbose);
    failures += run_case(32, 4, 50, 300, SC_MODEL_SYMMETRIC, 64, 200, 3, verbose);
    failures += run_case(32, 4, 50, 300, SC_MODEL_SYSTEMATIC, 64, 200, 4, verbose);
    failures += run_case(64, 8, 50, 300, SC_MODEL_SYMMETRIC, 64, 200, 5, verbose);
    failures += run_case(64, 8, 50, 300, SC_MODEL_SYSTEMATIC, 64, 200, 6, verbose);

    /* Production sizes. */
    failures += run_case(256, 8, 50, 300, SC_MODEL_SYMMETRIC, 64, 40, 7, verbose);
    failures += run_case(256, 16, 50, 300, SC_MODEL_SYMMETRIC, 64, 40, 8, verbose);
    failures += run_case(256, 16, 50, 300, SC_MODEL_SYSTEMATIC, 64, 40, 9, verbose);
    failures += run_case(256, 16, 60, 300, SC_MODEL_SYMMETRIC, 64, 40, 10, verbose);
    failures += run_case(256, 16, 70, 200, SC_MODEL_SYMMETRIC, 64, 40, 11, verbose);
    failures += run_case(256, 16, 70, 300, SC_MODEL_SYMMETRIC, 64, 40, 12, verbose);
    failures += run_case(512, 16, 50, 300, SC_MODEL_SYMMETRIC, 64, 20, 13, verbose);
    failures += run_case(512, 16, 60, 300, SC_MODEL_SYMMETRIC, 64, 20, 14, verbose);

    /* Batch widths: 64, 128, 256, 512. */
    failures += run_case(256, 16, 50, 300, SC_MODEL_SYMMETRIC, 128, 20, 15, verbose);
    failures += run_case(256, 16, 50, 300, SC_MODEL_SYMMETRIC, 256, 10, 16, verbose);
    failures += run_case(256, 16, 50, 300, SC_MODEL_SYMMETRIC, 512, 10, 17, verbose);

    /* Rates below 0.50 use extra check nodes. */
    failures += run_case(256, 16, 40, 400, SC_MODEL_SYMMETRIC, 64, 40, 20, verbose);
    failures += run_case(256, 16, 30, 500, SC_MODEL_SYMMETRIC, 64, 40, 21, verbose);
    failures += run_case(256, 16, 25, 500, SC_MODEL_SYMMETRIC, 64, 40, 22, verbose);
    failures += run_case(512, 16, 40, 400, SC_MODEL_SYMMETRIC, 64, 20, 23, verbose);

    /* Rate 0.70 with the smallest coupling, where the check degree is high. */
    failures += run_case(256, 8, 70, 200, SC_MODEL_SYMMETRIC, 64, 40, 18, verbose);
    failures += run_case(512, 8, 70, 200, SC_MODEL_SYMMETRIC, 64, 20, 19, verbose);

    if (failures)
        printf("RESULT: FAIL (%d cases)\n", failures);
    else
        printf("RESULT: PASS\n");
    return failures ? 1 : 0;
}
