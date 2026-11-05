/*
 * sc_ldpc.h - spatially coupled (5,10) LDPC for the binary erasure channel.
 *
 * Construction and BEC peeling decoder re-implemented in C99 from the
 * description of npminh12's "Spatially-coupled-LDPC" simulation:
 *
 *   https://github.com/npminh12/Spatially-coupled-LDPC
 *   commit 3d403f4, MIT License, Copyright (c) 2019 npminh12.
 *
 * Only the construction rules and the BEC message semantics were taken from
 * that source.  No C++ code is copied.  See review/sc-ldpc-sim.md for the
 * rules restated in prose and for the three documented deviations:
 *
 *   1. causal termination (L check blocks instead of L+4) so that the
 *      systematic code has design rate exactly n_data/(n_data+n_parity);
 *   2. a systematic split (stream 0 = payload, stream 1 = parity);
 *   3. extra check nodes for target rates below 0.50.
 *
 * Host-only, no external dependencies.
 */

#ifndef SC_LDPC_H
#define SC_LDPC_H

#include <stddef.h>
#include <stdint.h>

/* Termination / boundary rule of the coupling. */
typedef enum {
    /* npminh12 rule with the trailing l-1 check blocks kept: M = (L+4)*m.
     * Not systematic; only useful as the literal reference code. */
    SC_TERM_NATIVE = 0,
    /* npminh12 rule with the trailing l-1 check blocks dropped: M = L*m.
     * Each check owns exactly one parity bit, so the code is systematic and
     * the design rate is exact for every L. */
    SC_TERM_CAUSAL = 1
} sc_term;

/* Which bits the channel erases. */
typedef enum {
    /* Every code bit is erased with probability p (npminh12's channel). */
    SC_MODEL_SYMMETRIC = 0,
    /* Only payload variables are erased; every parity bit is known. */
    SC_MODEL_SYSTEMATIC = 1
} sc_model;

typedef struct {
    int block;      /* variable nodes per coupling block, q*m; even, >= 16 */
    int L;          /* coupling length, >= 5 */
    int rate_pct;   /* target rate in percent: 50, 40 or 35 */
    int term;       /* sc_term; rate_pct != 50 requires SC_TERM_CAUSAL */
    int extra_dc;   /* data degree of each extra check node (rate < 50) */
    uint64_t seed;  /* construction seed */
} sc_params;

typedef struct sc_code sc_code;

typedef struct {
    int n;          /* total variable nodes */
    int n_data;     /* payload variable nodes */
    int n_par;      /* parity variable nodes */
    int n_chk;      /* check nodes */
    int n_extra;    /* extra check nodes (rate < 0.5) */
    long edges;
    int dl_max;
    int dc_max;
    double rate;    /* n_data / n for causal, 1 - n_chk/n for native */
    size_t mem_bytes;
} sc_info;

typedef struct {
    long unresolved;    /* target variables still erased after peeling */
    long target;        /* n_data (systematic) or n (symmetric) */
    long rounds;        /* peeling waves; one wave = one BP sweep */
    long edge_visits;   /* adjacency entries touched */
    long resolved;      /* target variables recovered by peeling */
} sc_dec_stat;

/* Build a code.  Returns NULL and fills err[] on invalid parameters. */
sc_code *sc_build(const sc_params *p, char *err, size_t errlen);
void sc_free(sc_code *c);
void sc_get_info(const sc_code *c, sc_info *out);

/*
 * Systematic encoder.  code[0 .. n_data-1] holds the payload on entry; the
 * parity bits code[n_data .. n-1] are written.  Returns 0 on success, -1 if
 * the code is not systematic (SC_TERM_NATIVE).
 */
int sc_encode(sc_code *c, uint8_t *code);

/* Returns 0 if every check equation holds, else 1 + index of first bad check. */
int sc_verify(const sc_code *c, const uint8_t *code);

/*
 * Peeling / BP decoder for the BEC.  recv[v] is the received value of a
 * non-erased variable; erased[v] != 0 marks an erasure.  out[v] receives the
 * decoded value of every variable (recovered values for erased ones).  The
 * statistic counts target variables: payload only for SC_MODEL_SYSTEMATIC,
 * all variables for SC_MODEL_SYMMETRIC.
 */
sc_dec_stat sc_decode(sc_code *c, const uint8_t *recv, const uint8_t *erased,
                      uint8_t *out, int target_data_only);

/* Variable index of payload bit k; k must be < n_data. */
int sc_data_var(const sc_code *c, int k);

/* 1 if the code is systematic (SC_TERM_CAUSAL), 0 otherwise. */
int sc_systematic(const sc_code *c);

/* Decoder knowledge after the last sc_decode(): 1 = value known. */
const uint8_t *sc_known_bits(const sc_code *c);

#endif /* SC_LDPC_H */
