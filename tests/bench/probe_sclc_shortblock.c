/* probe_sclc_shortblock.c - deterministic short-block recovery probe.
 *
 * Measures the two SC-LDPC codecs exactly as the wire uses them: the sender
 * transmits only tbt_fec_tx_parity_count(win_len, n, m, true) parity shares,
 * the receiver marks the never-written tail known-zero, and one or two real
 * DATA shares are lost. It prints one CSV row per (shape, win_len, loss set,
 * parity set) so the same numbers can be re-read without the probe.
 *
 * Build (no Makefile dependency, no kernel, no network):
 *
 *   cc -std=gnu99 -O2 -Wall -Wextra -Werror -I tbtun \
 *      -o host/probe_sclc_shortblock host/probe_sclc_shortblock.c \
 *      src/kernel/tbt_fec_sclc.c src/kernel/tbt_sc_ldpc.c src/kernel/tbt_sc_ldpc_gf16.c \
 *      src/kernel/tbt_sclc_uplink.c
 *
 * Downlink: one group of 256 shares (n + m = 256). Uplink: the production
 * 8x32 shape (eight groups of n + m = 32, block 64, L 8); a short block
 * lives in group 0, because real shares are group-major.
 *
 * Columns:
 *   dir,shape,nonce,win_len,parity_sent,wake,losses,ndata,nparity,rc,
 *   known,real,exact
 *   losses is a '+' list of lost DATA indices, 'n' for the empty set.
 *   known and exact count only the real DATA shares (0..win_len-1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec_sclc.h"
#include "tbt_sclc_uplink.h"

#define SHARE TBT_FEC_S
#define DL_N 128
#define DL_M 128
#define UP_N 8
#define UP_M 24
#define MAX_LOSS 4

static u64 rng_state;

static void rng_seed(u64 seed)
{
    rng_state = seed ? seed : 1;
}

static u64 rng_next(void)
{
    u64 z = (rng_state += 0x9E3779B97F4A7C15ULL);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Fill every real share with a seed-dependent pattern. The decoder must
 * reproduce these bytes for a recovery to count as exact. */
static void fill_real(u8 *base, u16 n, u16 win_len)
{
    u16 i;
    size_t j;

    for (i = 0; i < n; i++) {
        u8 *p = base + (size_t)i * SHARE;

        if (i >= win_len) {
            memset(p, 0, SHARE);
            continue;
        }
        for (j = 0; j < SHARE; j++)
            p[j] = (u8)(rng_next() >> 24);
    }
}

static void print_row(const char *dir, const char *shape, u64 nonce,
                      u16 win_len, u16 parity_sent, int wake,
                      const u16 *loss, u16 nloss, u16 ndata, u16 nparity,
                      int rc, u16 known, u16 real, int exact)
{
    u16 i;

    printf("%s,%s,%llu,%u,%u,%d,", dir, shape, (unsigned long long)nonce,
           win_len, parity_sent, wake);
    if (!nloss) {
        printf("n");
    } else {
        for (i = 0; i < nloss; i++)
            printf(i ? "+%u" : "%u", loss[i]);
    }
    printf(",%u,%u,%d,%u,%u,%d\n", ndata, nparity, rc, known, real, exact);
}

/* ---- downlink: one group of n + m = 256 binary shares ---------------- */

static void run_dl(u16 n, u16 m, u64 nonce, u16 win_len, u16 p,
                   const u16 *loss, u16 nloss)
{
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc;
    void *c = ops->new(n, m, 500, 256, SHARE, 0, nonce);
    u8 *data, *parity, *orig;
    u8 **dp, **pp;
    u8 *pd, *ppr;
    u16 i;
    u16 ndata = win_len;
    u16 real = win_len;
    u16 known = 0;
    int exact = 1;
    int wake;
    int rc;

    if (!c) {
        printf("dl,%u/%u,%llu,%u,%u,-1,new-failed\n", n, m,
               (unsigned long long)nonce, win_len, p);
        return;
    }
    data = calloc((size_t)n, SHARE);
    parity = calloc((size_t)m, SHARE);
    orig = calloc((size_t)win_len, SHARE);
    dp = calloc(n, sizeof(*dp));
    pp = calloc(m, sizeof(*pp));
    pd = calloc(n, 1);
    ppr = calloc(m, 1);
    if (!data || !parity || !orig || !dp || !pp || !pd || !ppr)
        exit(2);

    rng_seed(0xD1B54A32D192ED03ULL ^ ((u64)n << 32) ^ nonce ^ win_len);
    fill_real(data, n, win_len);
    for (i = 0; i < win_len; i++)
        memcpy(orig + (size_t)i * SHARE, data + (size_t)i * SHARE, SHARE);
    for (i = 0; i < n; i++)
        dp[i] = data + (size_t)i * SHARE;
    for (i = 0; i < m; i++)
        pp[i] = parity + (size_t)i * SHARE;
    if (ops->encode(c, dp, n, pp) != 0)
        exit(2);

    /* The receiver sees the written prefix plus the known-zero tail. */
    memset(pd, 1, n);
    for (i = 0; i < nloss; i++) {
        if (loss[i] < win_len) {
            pd[loss[i]] = 0;
            memset(data + (size_t)loss[i] * SHARE, 0, SHARE);
            ndata--;
        }
    }
    memset(ppr, 0, m);
    for (i = 0; i < p && i < m; i++)
        ppr[i] = 1;

    wake = tbt_fec_sclc_decode_wake(real, n, m, 1, ndata, p);
    rc = ops->decode(c, dp, pp, pd, ppr, n);
    for (i = 0; i < win_len; i++) {
        if (ops->is_known(c, i))
            known++;
        if (memcmp(data + (size_t)i * SHARE, orig + (size_t)i * SHARE,
                   SHARE) != 0)
            exact = 0;
    }
    print_row("dl", n == DL_N ? "1x256-128/128" : "1x256-64/192",
              nonce, win_len, p, wake, loss, nloss, ndata, p, rc, known,
              real, exact);

    ops->free(c);
    free(data);
    free(parity);
    free(orig);
    free(dp);
    free(pp);
    free(pd);
    free(ppr);
}

/* ---- uplink: the production 8x32 group, short block in group 0 ------ */

static struct tbt_sclc_uplink *up_new(u64 nonce)
{
    struct tbt_sclc_params par;

    memset(&par, 0, sizeof(par));
    par.n = UP_N;
    par.m = UP_M;
    par.block = TBT_FEC_SCLC_BLOCK_UP;
    par.L = TBT_FEC_SCLC_L_UP;
    par.rate_num = (u16)(UP_N * 8u);
    par.rate_den = 256;
    par.max_core = 256;
    par.mapping = TBT_SCLC_MAP_BITPLANE64;
    par.seed = nonce;
    return tbt_sclc_uplink_new(1, &par);
}

static void run_up(u64 nonce, u16 win_len, u16 p, const u16 *loss, u16 nloss)
{
    struct tbt_sclc_uplink *u = up_new(nonce);
    u8 *data, *parity, *orig;
    u8 *dp[UP_N], *pp[UP_M];
    u8 pd[UP_N], ppr[UP_M];
    struct tbt_sclc_stat st;
    u16 i;
    u16 ndata = win_len;
    u16 real = win_len;
    u16 known = 0;
    int exact = 1;
    int wake;
    int rc;

    if (!u) {
        printf("up,8x32,%llu,%u,%u,-1,new-failed\n",
               (unsigned long long)nonce, win_len, p);
        return;
    }
    data = calloc(UP_N, SHARE);
    parity = calloc(UP_M, SHARE);
    orig = calloc(win_len, SHARE);
    if (!data || !parity || !orig)
        exit(2);

    rng_seed(0x9E3779B97F4A7C15ULL ^ nonce ^ win_len);
    fill_real(data, UP_N, win_len);
    for (i = 0; i < win_len; i++)
        memcpy(orig + (size_t)i * SHARE, data + (size_t)i * SHARE, SHARE);
    for (i = 0; i < UP_N; i++)
        dp[i] = data + (size_t)i * SHARE;
    for (i = 0; i < UP_M; i++)
        pp[i] = parity + (size_t)i * SHARE;
    if (tbt_sclc_uplink_encode(u, 0, dp, pp) != 0)
        exit(2);

    memset(pd, 1, UP_N);
    for (i = 0; i < nloss; i++) {
        if (loss[i] < win_len) {
            pd[loss[i]] = 0;
            memset(data + (size_t)loss[i] * SHARE, 0, SHARE);
            ndata--;
        }
    }
    memset(ppr, 0, UP_M);
    for (i = 0; i < p && i < UP_M; i++)
        ppr[i] = 1;

    wake = tbt_fec_sclc_decode_wake(real, UP_N, UP_M, 8, ndata, p);
    rc = tbt_sclc_uplink_decode(u, 0, dp, pp, pd, ppr);
    for (i = 0; i < win_len; i++) {
        if (tbt_sclc_uplink_share_known(u, 0, i))
            known++;
        if (memcmp(data + (size_t)i * SHARE, orig + (size_t)i * SHARE,
                   SHARE) != 0)
            exact = 0;
    }
    tbt_sclc_uplink_stat(u, 0, &st);
    print_row("up", "8x32", nonce, win_len, p, wake, loss, nloss, ndata, p,
              rc, known, real, exact);
    printf("# up-stat codes_ok=%u codes=%u shares_missing=%u recovered=%u\n",
           st.codes_ok, st.codes, st.shares_missing, st.shares_recovered);

    tbt_sclc_uplink_free(u);
    free(data);
    free(parity);
    free(orig);
}

int main(void)
{
    static const u16 dl_win[] = { 1, 2, 4, 8, 16, 32 };
    static const u64 nonces[] = { 0xabc, 0x1234, 0xdead };
    u16 loss1[1];
    u16 loss2[2];
    unsigned int wi, ni, li;
    u16 p;

    printf("# tx_parity_count(win_len,128,128,true): ");
    for (wi = 0; wi < sizeof(dl_win) / sizeof(dl_win[0]); wi++)
        printf("%u->%u ", dl_win[wi],
               tbt_fec_tx_parity_count(dl_win[wi], DL_N, DL_M, true));
    printf("\n");
    printf("# wake_extra_down(128)=%u wake_extra_down(64)=%u "
           "wake_extra_down(32)=%u\n",
           tbt_fec_sclc_wake_extra_down(128),
           tbt_fec_sclc_wake_extra_down(64),
           tbt_fec_sclc_wake_extra_down(32));
    printf("# wake_extra_up=%u\n", TBT_FEC_SCLC_WAKE_EXTRA_UP);
    printf("# columns: dir,shape,nonce,win_len,parity_sent,wake,losses,"
           "ndata,nparity,rc,known,real,exact\n");

    /* 1. The wire rule: p = tbt_fec_tx_parity_count(). One and two losses
     * at the first and the last real share. */
    for (ni = 0; ni < sizeof(nonces) / sizeof(nonces[0]); ni++) {
        for (wi = 0; wi < sizeof(dl_win) / sizeof(dl_win[0]); wi++) {
            u16 w = dl_win[wi];

            p = tbt_fec_tx_parity_count(w, DL_N, DL_M, true);
            loss1[0] = 0;
            run_dl(DL_N, DL_M, nonces[ni], w, p, loss1, 1);
            loss1[0] = (u16)(w - 1);
            run_dl(DL_N, DL_M, nonces[ni], w, p, loss1, 1);
            if (w >= 2) {
                loss2[0] = 0;
                loss2[1] = 1;
                run_dl(DL_N, DL_M, nonces[ni], w, p, loss2, 2);
            }
            if (w >= 4) {
                loss2[0] = 0;
                loss2[1] = (u16)(w - 1);
                run_dl(DL_N, DL_M, nonces[ni], w, p, loss2, 2);
            }
        }
    }

    /* 2. Forced decode with more of the same parity rows. This answers
     * whether extra prefix parities alone can recover the losses. */
    {
        static const u16 pset[] = { 2,  4,  8,  12, 16, 17,
                                    18, 24, 32, 48, 64, 128 };

        for (ni = 0; ni < 2; ni++) {
            for (wi = 0; wi < sizeof(dl_win) / sizeof(dl_win[0]); wi++) {
                u16 w = dl_win[wi];

                for (li = 0; li < sizeof(pset) / sizeof(pset[0]); li++) {
                    p = pset[li];
                    if (p < w)
                        continue;
                    loss1[0] = 0;
                    run_dl(DL_N, DL_M, nonces[ni], w, p, loss1, 1);
                    if (w >= 2) {
                        loss2[0] = 0;
                        loss2[1] = (u16)(w - 1);
                        run_dl(DL_N, DL_M, nonces[ni], w, p, loss2, 2);
                    }
                }
            }
        }
    }

    /* 3. Geometry: with every parity row of the first coupling block
     * (m / 8 = 16 rows) present, a single loss should recover at every
     * index inside that block and fail beyond it. */
    for (ni = 0; ni < 2; ni++) {
        for (li = 0; li < 24; li++) {
            loss1[0] = (u16)li;
            run_dl(DL_N, DL_M, nonces[ni], 24, 16, loss1, 1);
        }
    }

    /* 4. Uplink 8x32 group at the wire rule. */
    {
        static const u16 up_win[] = { 1, 2, 3, 4, 6, 8 };
        static const u16 upset[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24 };

        for (ni = 0; ni < 2; ni++) {
            for (wi = 0; wi < sizeof(up_win) / sizeof(up_win[0]); wi++) {
                u16 w = up_win[wi];

                p = tbt_fec_tx_parity_count(w, UP_N, UP_M, true);
                loss1[0] = 0;
                run_up(nonces[ni], w, p, loss1, 1);
                loss1[0] = (u16)(w - 1);
                run_up(nonces[ni], w, p, loss1, 1);
                if (w >= 2) {
                    loss2[0] = 0;
                    loss2[1] = 1;
                    run_up(nonces[ni], w, p, loss2, 2);
                }
            }
        }
        for (ni = 0; ni < 2; ni++) {
            for (wi = 0; wi < sizeof(up_win) / sizeof(up_win[0]); wi++) {
                u16 w = up_win[wi];

                for (li = 0; li < sizeof(upset) / sizeof(upset[0]); li++) {
                    p = upset[li];
                    if (p < w)
                        continue;
                    loss1[0] = 0;
                    run_up(nonces[ni], w, p, loss1, 1);
                }
            }
        }
    }
    return 0;
}
