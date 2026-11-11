/* calib_sclc_de.c - deterministic BEC density evolution for the shipped
 * 1x256 downlink SC-LDPC protograph.
 *
 * The graph is read from tbt_fec_sclc.c's sclc_build_graph(): L = 8
 * supernodes, n payload variables and m parity variables/checks per
 * supernode, payload degree dv = 5 (n <= m) or 4 (n > m), and a
 * non-wrapping parity accumulator.  A payload block i connects to
 * supernodes i .. i+dv-1 mod L.  A check in supernode j gets, from each
 * source block, floor(n/m) or ceil(n/m) edges (n > m) or 0/1 edge
 * (n <= m); it also owns one parity variable and, for j > 0, consumes one
 * from supernode j-1.
 *
 * The messages are erasure probabilities of the peeling decoder on a BEC.
 * The channel erases each bit independently with probability eps, which is
 * the usual DE assumption.  A share erases eight bits at once; the
 * marginal recursion below is unchanged to first order, and the report
 * treats the share correlation as a finite-length correction.
 *
 * Usage:
 *   calib_sclc_de            # CSV for n = 1 .. 192
 *   calib_sclc_de --range    # which n the real codec constructor accepts
 *   calib_sclc_de --n N      # one row
 */
#include "tbt_fec_sclc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define L TBT_SCLC_L /* 8 */

struct de
{
    int n, m, dv, q;
    double pm; /* r / m, r = n mod m */
    double nm; /* n / m */
    double x[L][L]; /* payload block i -> supernode j */
    double z[L][L]; /* supernode j -> payload block i */
    double yown[L], yacc[L];
    double zown[L], zacc[L];
};

static double ipow(double b, int e)
{
    double r = 1.0;

    while (e-- > 0)
        r *= b;
    return r;
}

static int src_block(int j, int r)
{
    return (j + L - r) % L;
}

static double a0(const struct de *d, double x)
{
    if (d->n <= d->m)
        return 1.0 - d->nm * x;
    return ipow(1.0 - x, d->q) * (1.0 - d->pm * x);
}

/* Size-biased degree seen by an existing edge. */
static double a1(const struct de *d, double x)
{
    double t;

    if (d->n <= d->m)
        return 1.0;
    t = (1.0 - d->pm) * d->q * ipow(1.0 - x, d->q - 1);
    t += d->pm * (d->q + 1) * ipow(1.0 - x, d->q);
    return t / d->nm;
}

static void de_init(struct de *d, int n, int m)
{
    int j, i;

    memset(d, 0, sizeof(*d));
    d->n = n;
    d->m = m;
    d->dv = n > m ? TBT_SCLC_DV_HIGH : TBT_SCLC_DV;
    d->q = n / m;
    d->pm = (double)(n % m) / (double)m;
    d->nm = (double)n / (double)m;
    for (j = 0; j < L; j++)
        for (i = 0; i < L; i++)
        {
            d->x[j][i] = 0.0;
            d->z[j][i] = 0.0;
        }
}

/* One DE run at channel erasure eps.  Returns the payload erasure
 * probability of the fixed point (0 means the block decodes). */
static double de_run(struct de *d, double eps, int max_iter)
{
    int it, j, r, r2, i;
    double x[L][L], z[L][L], yown[L], yacc[L], zown[L], zacc[L];
    double nx[L][L], nyown[L], nyacc[L];

    for (j = 0; j < L; j++)
    {
        yown[j] = eps;
        yacc[j] = j ? eps : 0.0;
        for (i = 0; i < L; i++)
            x[j][i] = 0.0;
    }
    for (i = 0; i < L; i++)
        for (r = 0; r < d->dv; r++)
            x[(i + r) % L][i] = eps;

    for (it = 0; it < max_iter; it++)
    {
        double delta = 0.0, maxx = 0.0;

        for (j = 0; j < L; j++)
        {
            double prod = 1.0;

            for (r = 0; r < d->dv; r++)
            {
                i = src_block(j, r);
                prod *= a0(d, x[j][i]);
            }
            for (r = 0; r < d->dv; r++)
            {
                double rest = 1.0;

                i = src_block(j, r);
                for (r2 = 0; r2 < d->dv; r2++)
                {
                    int i2 = src_block(j, r2);

                    if (r2 != r)
                        rest *= a0(d, x[j][i2]);
                }
                z[j][i] = 1.0 - a1(d, x[j][i]) * rest *
                                    (1.0 - yown[j]) * (1.0 - yacc[j]);
            }
            zown[j] = 1.0 - prod * (1.0 - yacc[j]);
            zacc[j] = 1.0 - prod * (1.0 - yown[j]);
        }

        for (i = 0; i < L; i++)
            for (r = 0; r < d->dv; r++)
            {
                double p = 1.0;
                int jj = (i + r) % L;

                for (r2 = 0; r2 < d->dv; r2++)
                    if (r2 != r)
                        p *= z[(i + r2) % L][i];
                nx[jj][i] = eps * p;
            }
        for (j = 0; j < L; j++)
        {
            nyown[j] = (j < L - 1) ? eps * zacc[j + 1] : eps;
            nyacc[j] = j ? eps * zown[j - 1] : 0.0;
        }

        for (i = 0; i < L; i++)
            for (r = 0; r < d->dv; r++)
            {
                int jj = (i + r) % L;
                double v = nx[jj][i];

                if (v - x[jj][i] > delta)
                    delta = v - x[jj][i];
                if (x[jj][i] - v > delta)
                    delta = x[jj][i] - v;
                x[jj][i] = v;
                if (v > maxx)
                    maxx = v;
            }
        for (j = 0; j < L; j++)
        {
            double v = nyown[j];

            if (v - yown[j] > delta)
                delta = v - yown[j];
            if (yown[j] - v > delta)
                delta = yown[j] - v;
            yown[j] = v;
            v = nyacc[j];
            if (v - yacc[j] > delta)
                delta = v - yacc[j];
            if (yacc[j] - v > delta)
                delta = yacc[j] - v;
            yacc[j] = v;
        }
        if (delta < 1e-13 && it > 2)
            break;
    }
    {
        double maxx = 0.0;

        for (i = 0; i < L; i++)
            for (r = 0; r < d->dv; r++)
            {
                int jj = (i + r) % L;

                if (x[jj][i] > maxx)
                    maxx = x[jj][i];
            }
        return maxx;
    }
}

static double threshold(int n, int m, int *iters)
{
    double lo = 0.0, hi = 1.0;
    int k;

    for (k = 0; k < 48; k++)
    {
        struct de d;
        double mid = 0.5 * (lo + hi);
        double x;

        de_init(&d, n, m);
        /* The tiny boundary fixed point (an error floor) can reach 1e-2
         * before the waterfall jump; the large branch is >0.1, so 0.1
         * separates the two. */
        x = de_run(&d, mid, 20000);
        if (x < 0.1)
            lo = mid;
        else
            hi = mid;
    }
    if (iters)
        *iters = 0;
    return 0.5 * (lo + hi);
}

static int codec_builds(int n, int m)
{
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc;
    void *c = ops->new((u16)n, (u16)m, 0, TBT_SCLC_BLOCK, TBT_SCLC_SHARE, 0,
                       0x5C1C0DEull);

    if (!c)
        return 0;
    ops->free(c);
    return 1;
}

int main(int argc, char **argv)
{
    int n;

    if (argc == 2 && !strcmp(argv[1], "--range"))
    {
        int first = -1, last = -1, bad = 0;

        for (n = 1; n < TBT_FEC_MAX_SHARES; n++)
        {
            int ok = codec_builds(n, TBT_FEC_MAX_SHARES - n);

            if (ok && first < 0)
                first = n;
            if (ok)
                last = n;
            if (!ok)
            {
                bad++;
                if (bad <= 8)
                    printf("reject n=%d m=%d\n", n,
                           TBT_FEC_MAX_SHARES - n);
            }
        }
        printf("range n=%d..%d, rejected=%d of %d\n", first, last, bad,
               TBT_FEC_MAX_SHARES - 1);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--eps"))
    {
        struct de d;
        int m = TBT_FEC_MAX_SHARES - atoi(argv[2]);
        double x;

        de_init(&d, atoi(argv[2]), m);
        x = de_run(&d, atof(argv[3]), argc > 4 ? atoi(argv[4]) : 20000);
        printf("n=%d eps=%g maxx=%.12g\n", d.n, atof(argv[3]), x);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--n"))
    {
        int m;

        n = atoi(argv[2]);
        m = TBT_FEC_MAX_SHARES - n;
        if (n <= 0 || m <= 0)
            return 2;
        printf("n,m,rate,dv,q,r,eps_star,mds,delta\n");
        {
            double e = threshold(n, m, NULL);

            printf("%d,%d,%.6f,%d,%d,%d,%.9f,%.9f,%.6f\n", n, m,
                   (double)n / 256.0, n > m ? TBT_SCLC_DV_HIGH : TBT_SCLC_DV,
                   n / m, n % m, e, (double)m / 256.0,
                   256.0 * ((double)m / 256.0 - e));
        }
        return 0;
    }
    if (argc != 1)
    {
        fprintf(stderr, "usage: %s [--range|--n N]\n", argv[0]);
        return 2;
    }

    printf("n,m,rate,dv,q,r,eps_star,mds,delta\n");
    for (n = 1; n <= 192; n++)
    {
        int m = TBT_FEC_MAX_SHARES - n;
        double e = threshold(n, m, NULL);

        printf("%d,%d,%.6f,%d,%d,%d,%.9f,%.9f,%.6f\n", n, m,
               (double)n / 256.0, n > m ? TBT_SCLC_DV_HIGH : TBT_SCLC_DV,
               n / m, n % m, e, (double)m / 256.0,
               256.0 * ((double)m / 256.0 - e));
    }
    return 0;
}
