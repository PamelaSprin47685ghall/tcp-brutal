/*
 * tbt_sc_ldpc_gf16.h - q-ary (GF(16)) SC-IRA codec for the uplink grouped
 * FEC block.
 *
 * Kernel port of the 0.3 host prototype (w-sclc-gf16; dropped 2026-11-12).
 * The graph is the SC_TERM_ACC graph; every variable and
 * check symbol is a GF(16) element and every check is a random nonzero
 * linear combination.  The owned parity symbol of a check has coefficient
 * 1, so encoding is one forward sweep.
 *
 * The decoder is q-ary peeling plus a bounded dense core solve.  The core
 * solve is capped by max_core variables and by a fixed byte budget so a
 * pathological stopping set cannot allocate without bound in the module.
 */

#ifndef _TBT_SC_LDPC_GF16_H
#define _TBT_SC_LDPC_GF16_H

#include "tbt_sc_ldpc.h"

typedef struct gf16_code gf16_code;

typedef struct
{
    long unresolved;  /* target variables still erased after peeling */
    long target;      /* n_data (systematic) or n (symmetric) */
    long rounds;      /* peeling waves */
    long edge_visits; /* adjacency entries touched */
    long resolved;    /* target variables recovered */
    long core_calls;
    long core_solved;
    long core_failed;
    long core_skipped;
    long core_max_vars;
    long core_vars_sum;
    long core_gf2_deficient;
    long core_gf16_rank_sum;
    long core_under;
    long core_rankdef;
    long core_bad;
} gf16_dec_stat;

/* Build a q-ary code on the SC_TERM_ACC graph.  p->term must be
 * SC_TERM_ACC.  coef_seed controls the random nonzero edge coefficients.
 * Returns NULL and fills err[] on invalid parameters or allocation failure. */
gf16_code *gf16_build(const sc_params *p, uint64_t coef_seed, char *err,
                      size_t errlen);
void gf16_free(gf16_code *c);

/* Allocate the decoder-only scratch on first use.  The constructor keeps
 * only the graph and the encoder state, so a transmit codec never pays for
 * the peel, batch, core and 64-lane buffers.  Every decode entry point
 * calls this; it is idempotent.  Returns nonzero when an allocation fails
 * and leaves the object unchanged.  Must run in process context (the module
 * decodes on its FEC workqueue). */
int gf16_ensure_decode(gf16_code *c);

int gf16_n(const gf16_code *c);
int gf16_n_data(const gf16_code *c);
int gf16_n_par(const gf16_code *c);
u32 gf16_mem(const gf16_code *c);

/* Systematic encoder.  code[0 .. n_data-1] holds the payload symbols on
 * entry; code[n_data .. n-1] receives the parity symbols. */
#ifndef __KERNEL__
int gf16_encode(gf16_code *c, uint8_t *code);
#endif

/* Returns 0 if every check equation holds, else 1 + index of first bad check. */
int gf16_verify(const gf16_code *c, const uint8_t *code);

#ifndef __KERNEL__
/* q-ary peeling decoder for the BEC.  recv[v] holds the received symbol of
 * a non-erased variable; erased[v] != 0 marks an erasure.  out[v] receives
 * the decoded symbol of every variable (0 for unresolved).  max_core caps
 * the dense core solve in variables; 0 disables it. */
/* Success is a code-consistency result, not just "no erasure left": before
 * any target symbol is returned the decoder certifies the recovered data
 * against every check row (and every received parity symbol).  A caller
 * must therefore trust out[]/unresolved only when unresolved == 0. */
gf16_dec_stat gf16_decode(gf16_code *c, const uint8_t *recv,
                          const uint8_t *erased, uint8_t *out,
                          int target_data_only, int max_core);
#endif

/* 64-lane bit-sliced encoder.  vp holds four uint64 planes per variable,
 * interleaved (vp[v*4 + b]); bit c is codeword c's bit b, for c < 64.
 * The caller fills the payload planes (v < n_data) and zeroes the lanes
 * it does not use; the function writes the parity planes.  The check
 * order and every coefficient match gf16_encode(), so the result is
 * bit-exact. */
void gf16_encode_planes(gf16_code *c, int B, uint64_t *vp);

/* Decode one 64-lane batch in place.  planes[] holds four uint64 planes
 * per variable (bits >= B are ignored); erased[v] is one byte for the whole
 * batch, so lane c's symbol v is erased exactly when erased[v] is nonzero.
 * The function reads a known plane and writes a decoded one at the same
 * address; an unresolved variable is zeroed.  The peel runs once for the
 * batch; after it stalls the dense core solve performs one GF(16)
 * elimination on the shared coefficient matrix and carries the 64
 * right-hand sides in planes.  Before reporting success the decoder walks
 * every check row: a known owned symbol is compared against the value its
 * other neighbours imply and an erased one is filled, so the residual is
 * checked in all 64 lanes (the core solve's own verify covers lane 0).
 * Returns the batch statistics; unresolved == 0 means every target
 * variable decoded and the residual is clean. */
gf16_dec_stat gf16_decode64(gf16_code *c, uint64_t *planes,
                            const uint8_t *erased, int target_data_only,
                            int max_core);

/* Lane count of the bit-sliced encoder: one uint64 plane holds 64
 * codewords at once. */
#define GF16_LANES 64

/* Codewords processed together by gf16_decode_batch().  The graph is the
 * same for every slot; only recv/erased differ.  The slots are peeled in
 * lockstep so several independent walks hide each other's cache misses. */
#ifndef __KERNEL__
#define GF16_BATCH 4

/* Decode B <= GF16_BATCH codewords of one graph.  recv_b and erased_b hold
 * B blocks of n bytes (slot cw at offset cw*n); out_b receives B blocks of
 * n bytes and st_b one statistic block per slot.  The per-slot buffers are
 * preallocated with the code. */
void gf16_decode_batch(gf16_code *c, int B, const uint8_t *recv_b,
                       const uint8_t *erased_b, uint8_t *out_b,
                       int target_data_only, int max_core,
                       gf16_dec_stat *st_b);
#endif /* !__KERNEL__ */

#endif /* _TBT_SC_LDPC_GF16_H */
