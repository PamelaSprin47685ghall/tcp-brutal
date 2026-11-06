/*
 * dump.c - differential bit-exactness harness for sc_ldpc decoders.
 *
 * It compiles one implementation directly into the binary (so both the
 * public API and the file-static helpers are visible), then prints a hash of
 * out[] for every mask.  Build the same harness against the reference and
 * the candidate and diff the two outputs.
 *
 * Usage:
 *   cc -O2 -std=gnu99 -w -DSC_IMPL='"orig/sc_ldpc.c"' dump.c -lm -o d_base
 *   cc -O2 -std=gnu99 -w -DSC_IMPL='"sc_ldpc.c"'      dump.c -lm -o d_new
 *   ./d_base 256 16 50 30 2 > b.txt
 *   ./d_new  256 16 50 30 2 > n.txt
 *   diff b.txt n.txt
 *
 * Arguments: block L rate_pct p_pct term [trials]
 *   term: 0 = native, 1 = causal, 2 = acc.  trials defaults to 40.
 *
 * Each line is "t=<mask> tdo=<0|1> unres=<n> target=<n> hash=<fnv64>".
 * tdo=1 hashes only out[0..n_data), tdo=0 hashes out[0..n).  out[] is
 * pre-filled with 0xaa so a decoder that forgets to write a byte shows up.
 * A candidate matches the reference only if every line is identical.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include SC_IMPL

static uint32_t st;
static uint32_t rnd(void)
{
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}

static unsigned long long fnv(const uint8_t *p, int n)
{
    unsigned long long h = 1469598103934665603ULL;
    int i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int main(int argc, char **argv)
{
    sc_params p;
    sc_code *c;
    char err[256];
    uint8_t *code, *mask, *out;
    int i, n, nd, t, tdo, trials = argc > 6 ? atoi(argv[6]) : 40;
    unsigned long long h;
    long bad = 0;
    int block = argc > 1 ? atoi(argv[1]) : 256;
    int L = argc > 2 ? atoi(argv[2]) : 16;
    int rate = argc > 3 ? atoi(argv[3]) : 50;
    int pct = argc > 4 ? atoi(argv[4]) : 30;
    int term = argc > 5 ? atoi(argv[5]) : 2;

    memset(&p, 0, sizeof(p));
    p.block = block; p.L = L; p.rate_pct = rate; p.term = term;
    p.extra_dc = 2; p.seed = 7;
    c = sc_build(&p, err, sizeof(err));
    if (!c) { printf("build failed: %s\n", err); return 1; }
    n = c->n; nd = c->n_data;
    code = calloc(n, 1); mask = calloc(n, 1); out = calloc(n, 1);
    st = 424242u;
    for (i = 0; i < nd; i++) code[i] = (uint8_t)(rnd() & 1u);
    if (sc_systematic(c)) (void)sc_encode(c, code);
    printf("case block=%d L=%d rate=%d p=%d term=%d n=%d nd=%d\n",
           block, L, rate, pct, term, n, nd);
    for (t = 0; t < trials; t++) {
        for (i = 0; i < n; i++)
            mask[i] = (uint8_t)(rnd() % 100u < (uint32_t)pct);
        for (tdo = 0; tdo <= 1; tdo++) {
            sc_dec_stat s;
            memset(out, 0xAA, (size_t)n);
            s = sc_decode(c, code, mask, out, tdo);
            h = fnv(out, tdo ? nd : n);
            printf("t=%d tdo=%d unres=%ld target=%ld hash=%016llx\n", t, tdo,
                   s.unresolved, s.target, h);
            if (s.unresolved != 0) bad++;
        }
    }
    printf("done bad=%ld\n", bad);
    free(code); free(mask); free(out); sc_free(c);
    return 0;
}
