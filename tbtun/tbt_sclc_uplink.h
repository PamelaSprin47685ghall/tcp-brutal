/* tbt_sclc_uplink.h - uplink grouped GF(16) SC-IRA codec.
 *
 * Phase 1 shapes, all keeping 256 carriers:
 *   - primary:  8 groups x 32 shares, n=8,  m=24, rate 1/4;
 *   - measured: 8x32 n=14/m=18 rate 7/16, n=12/m=20 rate 3/8,
 *               n=10/m=22 rate 1/3;
 *   - fallback: 32 groups x 8 shares, n=4, m=4, rate 1/2.
 * The rate is n/(n+m) for every exact-fit shape; the receiver derives it
 * from n and m and no rate field travels on the wire.  The measured shapes
 * are kept because the host test reports their recovery: on a share-loss
 * channel the 7/16, 3/8 and 1/3 shapes miss the p=0.40 block target, while
 * 1/4 reaches the >0.99 floor.  See review/sc-ldpc-uplink-grouped.md.
 *
 * The codec is the SC-IRA construction of the host prototype
 * (review/fec-prototype/sc_ldpc on w-sclc-gf16), restricted to
 * SC_TERM_ACC, GF(16) symbols, systematic encoding and BEC peeling with a
 * bounded dense core solve.  No floating point, no libc, host/kernel dual
 * compile.
 *
 * Share<->symbol mapping, interleaved mode (production):
 *   - one GF(16) symbol is the interleave unit, packed two per byte,
 *     low nibble first;
 *   - the C codewords of a group block are interleaved: payload symbol j
 *     of codeword c sits at payload symbol position j*C + c, parity symbol
 *     j at parity position j*C + c;
 *   - data share d covers payload positions [d*2S, (d+1)*2S), parity share
 *     n+j covers parity positions [j*2S, (j+1)*2S).
 * A lost share therefore erases a thin slice of every codeword instead of
 * whole codewords.  The contiguous mode places codeword c at positions
 * [c*n_data, (c+1)*n_data) and exists for the host A/B test only.
 *
 * This module does not touch tbt_fec_data.c.  tbt_fec_ops_sclc_gf16 is the
 * integration point: the data plane creates one context per group with
 * new(n, m, block, L, S, rate_256, seed) and calls encode/decode on one
 * group FEC block.  `d` in that internal ops call is rate*256 (64 for 1/4,
 * 112 for 7/16); it is not a wire field.  block/L belong to the session
 * configuration, not to the 0x1006 FEC_CFG.
 */

#ifndef _TBT_SCLC_UPLINK_H
#define _TBT_SCLC_UPLINK_H

#include "tbt_fec.h"

#define TBT_SCLC_MAX_GROUPS 32

enum tbt_sclc_mapping
{
    TBT_SCLC_MAP_INTERLEAVED = 0, /* production: bit/symbol-mask interleave */
    TBT_SCLC_MAP_CONTIGUOUS = 1,  /* host A/B only */
    TBT_SCLC_MAP_RANDOM = 2,      /* seeded permutation, host A/B */
};

/* One group FEC block.  n + m <= 256, S is the share payload size.
 * block and L are the SC-IRA graph parameters.  The codeword count follows
 * from n and the share size: C = n*2S / (L*block/2).  Phase 1 uses
 * block=128, L=16, so C = n*16. */
struct tbt_sclc_params
{
    u16 n;        /* data shares per group */
    u16 m;        /* parity shares per group */
    u16 block;    /* variable nodes per coupling block (even) */
    u16 L;        /* coupling length */
    u16 rate_num; /* code rate = rate_num / rate_den; 7/16 or 1/2 */
    u16 rate_den;
    u16 max_core; /* dense core solve variable cap; 0 disables */
    u8 mapping;   /* enum tbt_sclc_mapping */
    u64 seed;     /* graph and coefficient seed */
};

/* Grouped uplink codec: `groups` independent SC-IRA contexts sharing the
 * same parameters.  Group g uses seed ^ ((u64)g << 32), the same rule as
 * the RLNC grouped codec, so no per-group seed travels on the wire. */
struct tbt_sclc_uplink;

struct tbt_sclc_uplink *tbt_sclc_uplink_new(u16 groups,
                                            const struct tbt_sclc_params *p);
void tbt_sclc_uplink_free(struct tbt_sclc_uplink *u);

u16 tbt_sclc_uplink_groups(const struct tbt_sclc_uplink *u);
u16 tbt_sclc_uplink_shares(const struct tbt_sclc_uplink *u);
u16 tbt_sclc_uplink_codes(const struct tbt_sclc_uplink *u);
u32 tbt_sclc_uplink_n_data(const struct tbt_sclc_uplink *u);
u32 tbt_sclc_uplink_n_par(const struct tbt_sclc_uplink *u);
u32 tbt_sclc_uplink_mem(const struct tbt_sclc_uplink *u);

/* Encode one group FEC block.  data[] holds n shares of S bytes each,
 * parity[] receives m shares of S bytes each.  Returns 0 or -EINVAL. */
int tbt_sclc_uplink_encode(struct tbt_sclc_uplink *u, u16 group,
                           u8 *data[], u8 *parity[]);

/* Decode one group FEC block.  data_present[i] / parity_present[j] are one
 * byte per share (nonzero = arrived).  Missing shares are reconstructed in
 * place when their codewords decode.  Returns the number of data shares
 * whose bytes are complete, or a negative errno.  On a rank shortage the
 * caller must NACK the shares that tbt_sclc_uplink_share_known() reports
 * absent; no incomplete share is trusted. */
int tbt_sclc_uplink_decode(struct tbt_sclc_uplink *u, u16 group,
                           u8 *data[], u8 *parity[],
                           const u8 *data_present,
                           const u8 *parity_present);

/* Per-group decode statistics of the last block. */
struct tbt_sclc_stat
{
    u32 codes;            /* codewords in the block */
    u32 codes_ok;         /* codewords fully decoded */
    u32 codes_failed;     /* codewords still erased after the core solve */
    u32 core_calls;
    u32 core_solved;
    u32 core_skipped;
    u32 shares_missing;   /* input shares absent */
    u32 shares_recovered; /* absent shares fully reconstructed */
};

void tbt_sclc_uplink_stat(const struct tbt_sclc_uplink *u, u16 group,
                          struct tbt_sclc_stat *out);

/* Share-level completeness after the last decode: 1 = every symbol of the
 * share is known.  Data shares are 0..n-1, parity shares n..n+m-1. */
int tbt_sclc_uplink_share_known(const struct tbt_sclc_uplink *u, u16 group,
                                u16 share);

/* Codec ops for the data plane.  new() maps:
 *   n = data shares, m = parity shares, k = block, w = L,
 *   s = share bytes, d = rate in 1/256 units (112 = 7/16, 128 = 1/2),
 *   nonce = seed.
 * encode/decode operate on one group block; coeffs returns -ENOSYS. */
extern const struct tbt_fec_ops tbt_fec_ops_sclc_gf16;

#endif /* _TBT_SCLC_UPLINK_H */
