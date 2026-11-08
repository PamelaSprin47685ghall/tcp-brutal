/*
 * sc_ldpc_gf16.h - q-ary (GF(16)) SC-IRA prototype for the binary erasure
 * channel.
 *
 * The graph is the same SC_TERM_ACC graph as sc_ldpc.c.  Each variable and
 * check symbol is an element of GF(16); every check equation is a random
 * nonzero linear combination over GF(16).  The owned parity symbol of a
 * check has coefficient 1, so the encoder is still one forward sweep:
 *
 *     x_owned = sum_{v != owned} h_v * x_v        (addition is XOR)
 *
 * The decoder is q-ary peeling, the BEC specialization of q-ary BP: a check
 * with exactly one unknown symbol solves it as
 *
 *     x_v = syn / h_v,   syn = sum_{known u} h_u * x_u.
 *
 * Because a check with one unknown is solvable for every nonzero h_v, the
 * set of variables resolved by peeling is exactly the set resolved by the
 * binary peeling decoder on the same graph and the same erasure mask.  The
 * q-ary code therefore does not change the recovery curve; it changes the
 * payload per graph node (4 bits per symbol instead of 1).
 */

#ifndef SC_LDPC_GF16_H
#define SC_LDPC_GF16_H

#include <stddef.h>
#include <stdint.h>

#include "sc_ldpc.h"

typedef struct gf16_code gf16_code;

typedef struct {
    long unresolved;  /* target variables still erased after peeling */
    long target;      /* n_data (systematic) or n (symmetric) */
    long rounds;      /* peeling waves; one wave = one BP sweep */
    long edge_visits; /* adjacency entries touched */
    long resolved;    /* target variables recovered by peeling */
    /* Optional algebraic core solve after peeling stalls. */
    long core_calls;       /* decodes with at least one unresolved variable */
    long core_solved;      /* full-rank, verified core solves */
    long core_failed;      /* attempted but rank-deficient/inconsistent */
    long core_skipped;     /* core larger than max_core */
    long core_max_vars;    /* largest core seen */
    long core_vars_sum;    /* sum of core sizes, for the average */
    long core_gf2_deficient; /* cores whose GF(2) rank < variables */
    long core_gf16_rank_sum; /* sum of GF(16) ranks */
    long core_under;       /* nr < nu: fewer equations than unknowns */
    long core_rankdef;     /* GF(16) rank < nu */
    long core_bad;         /* inconsistent or failed whole-codeword verify */
} gf16_dec_stat;

/* Build the GF(16) log/exp and multiplication tables.  Idempotent. */
void gf16_init(void);

/*
 * Build a q-ary code on the SC_TERM_ACC graph.  p->term must be
 * SC_TERM_ACC.  coef_seed controls the random nonzero edge coefficients;
 * the graph itself is built from p->seed, exactly as sc_build().
 * Returns NULL and fills err[] on invalid parameters.
 */
gf16_code *gf16_build(const sc_params *p, uint64_t coef_seed, char *err,
                      size_t errlen);
void gf16_free(gf16_code *c);
void gf16_get_info(const gf16_code *c, sc_info *out);

/*
 * Systematic encoder.  code[0 .. n_data-1] holds the payload symbols on
 * entry; code[n_data .. n-1] receives the parity symbols.  Returns 0 on
 * success, -1 if the graph is not forward-encodable.
 */
int gf16_encode(gf16_code *c, uint8_t *code);

/* Returns 0 if every check equation holds, else 1 + index of first bad check. */
int gf16_verify(const gf16_code *c, const uint8_t *code);

/*
 * q-ary peeling decoder for the BEC.  recv[v] holds the received symbol of a
 * non-erased variable; erased[v] != 0 marks an erasure.  out[v] receives the
 * decoded symbol of every variable (0 for unresolved).  target_data_only
 * counts payload variables only; otherwise all variables are counted.
 */
gf16_dec_stat gf16_decode(gf16_code *c, const uint8_t *recv,
                          const uint8_t *erased, uint8_t *out,
                          int target_data_only, int max_core);

/* Decoder knowledge after the last gf16_decode(): 1 = value known. */
const uint8_t *gf16_known(const gf16_code *c);

/* Number of variable / check nodes, for callers that only need sizes. */
int gf16_n(const gf16_code *c);
int gf16_n_data(const gf16_code *c);

#endif /* SC_LDPC_GF16_H */
