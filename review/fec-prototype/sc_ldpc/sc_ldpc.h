/*
 * sc_ldpc.h - spatially coupled LDPC for the binary erasure channel.
 *
 * The construction follows npminh12's "Spatially-coupled-LDPC" simulation:
 *
 *   https://github.com/npminh12/Spatially-coupled-LDPC
 *   commit 3d403f4, MIT License, Copyright (c) 2019 npminh12.
 *
 * Only the construction rules and the BEC message semantics were taken from
 * that source.  No C++ code is copied.  See review/sc-ldpc-sim.md for the
 * rules restated in prose and for the documented deviations.
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
     * Each check owns exactly one parity bit, but the last l-1 variable
     * blocks lose their right-hand checks and form 2-variable stopping
     * sets.  Kept for comparison; it fails at any L. */
    SC_TERM_CAUSAL = 1,
    /* Ring payload window plus a non-wrapping parity accumulator.  Payload
     * block i is connected to check supernodes (base..base+4) mod n_base,
     * base = floor(i*n_base/L), degree 5; parity block a is connected to
     * checks a and a+1 (degree 2, the last block degree 1).  n_base = L
     * gives M = L*m and rate exactly 0.5; rate 0.60/0.70 shrink n_base to
     * round(L*(1-R)/R).  The encoder is a single forward sweep and no
     * 2-variable stopping set exists. */
    SC_TERM_ACC = 2
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
    int L;          /* coupling length, >= 2 */
    int rate_pct;   /* target rate in percent: 25, 30, 35, 40, 45, 50, 60, 70 */
    int term;       /* sc_term; rate_pct != 50 requires SC_TERM_ACC */
    int extra_dc;   /* payload degree of each extra check node (rate < 50) */
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
    double rate;    /* n_data / n for systematic, 1 - n_chk/n for native */
    size_t mem_bytes;
} sc_info;

typedef struct {
    long unresolved;    /* target variables still erased after peeling */
    long target;        /* n_data (systematic) or n (symmetric) */
    long rounds;        /* peeling waves; one wave = one BP sweep */
    long edge_visits;   /* adjacency entries touched */
    long resolved;      /* target variables recovered by peeling */
} sc_dec_stat;

/*
 * Read-only graph view for alternative decoders.  The pointers stay valid
 * until sc_free().  vadj is variable-major (check ids), cadj is check-major
 * (variable ids); both hold c->edges entries.
 */
typedef struct {
    int n;
    int n_data;
    int n_par;
    int n_chk;
    int n_base;   /* base check supernodes, ACC termination */
    int m;        /* lifting factor = block/q */
    int n_extra;  /* extra check nodes at rate < 0.50 */
    long edges;
    int dc_max;
    const int *voff; /* n + 1 */
    const int *coff; /* n_chk + 1 */
    const int *vadj; /* edges */
    const int *cadj; /* edges */
} sc_graph;

void sc_get_graph(const sc_code *c, sc_graph *out);

/* Build a code.  Returns NULL and fills err[] on invalid parameters. */
sc_code *sc_build(const sc_params *p, char *err, size_t errlen);
void sc_free(sc_code *c);
void sc_get_info(const sc_code *c, sc_info *out);

/*
 * Systematic encoder.  code[0 .. n_data-1] holds the payload on entry; the
 * parity bits code[n_data .. n-1] are written.  Returns 0 on success, -1 if
 * the code is not systematically encodable (SC_TERM_NATIVE).
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

/* 1 if the code is systematic (SC_TERM_CAUSAL or SC_TERM_ACC), 0 otherwise. */
int sc_systematic(const sc_code *c);

/* Decoder knowledge after the last sc_decode(): 1 = value known. */
const uint8_t *sc_known_bits(const sc_code *c);

/*
 * Degree histograms, for the matrix dump.  vhist[i] = number of variable
 * nodes of degree i, chist[i] = number of check nodes of degree i.  The
 * arrays must hold SC_DEG_MAX + 1 entries; entries above the maximum degree
 * are left at 0.  Returns the maximum degree found.
 */
#define SC_DEG_MAX 64
int sc_degrees(const sc_code *c, int *vhist, int *chist);

/* Termination name, "native", "causal" or "acc". */
const char *sc_term_name(const sc_code *c);

#endif /* SC_LDPC_H */
