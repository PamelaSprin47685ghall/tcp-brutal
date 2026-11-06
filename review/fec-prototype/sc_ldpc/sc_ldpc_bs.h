/*
 * sc_ldpc_bs.h - bit-sliced BEC peeling decoder for sc_ldpc.
 *
 * One batch holds a multiple of 64 independent codewords.  Bit j of every
 * 64-bit word belongs to codeword j of the current group; a group is 64
 * codewords.  All graph state (unknown mask, value mask, per-check unknown
 * count and known-value XOR) is kept in that sliced form, so one scalar
 * uint64_t operation advances 64 codewords at once.  No AVX is required:
 * the HND0 target is qemu64 with SSE2 only.
 *
 * The semantics are exactly the scalar sc_decode(): variables are resolved
 * when a check has exactly one unknown neighbour, the value is the XOR of
 * the known neighbours, and the process stops at the peeling fixed point.
 * The resolved set and the recovered values must be bit-identical.
 *
 * Wire layout: packed, codeword-major, LSB-first.  Codeword j starts at
 * bits[j * stride], variable v sits at byte v/8, bit v%8.  The same layout
 * is used for the erasure mask and for the output.
 */

#ifndef SC_LDPC_BS_H
#define SC_LDPC_BS_H

#include "sc_ldpc.h"

#include <stddef.h>
#include <stdint.h>

typedef struct sc_bs sc_bs;

typedef struct {
    long waves;        /* wave-peeling rounds */
    long check_scans;  /* check-edge pairs inspected in a wave */
    long var_resolves; /* (variable, wave) resolutions */
    long edge_updates; /* (variable, check) counter updates */
} sc_bs_stat;

/*
 * Create a decoder for `batch` codewords.  `batch` must be a positive
 * multiple of 64.  Returns NULL and fills err[] when the code has a check
 * degree above 31 or on allocation failure.
 */
sc_bs *sc_bs_new(sc_code *c, int batch, char *err, size_t errlen);
void sc_bs_free(sc_bs *bs);

int sc_bs_batch(const sc_bs *bs);
size_t sc_bs_stride(const sc_bs *bs);

/*
 * Decode one batch.  bits/erased/out use the packed layout above; out may
 * alias neither input.  erased has a set bit for every erased variable.
 * target_data_only selects the statistic: payload variables only (1) or
 * every variable (0).  unresolved, when non-NULL, receives one entry per
 * codeword: the number of target variables still unknown.  Returns the
 * number of codewords whose target variables were all recovered.
 */
long sc_decode_bs(sc_bs *bs, const uint8_t *bits, const uint8_t *erased,
                  uint8_t *out, int target_data_only, long *unresolved);

void sc_bs_get_stat(const sc_bs *bs, sc_bs_stat *out);

#ifdef BS_TIMING
void sc_bs_timing(const sc_bs *bs, double *init, double *wave, double *unres,
                  double *out);
#endif

/*
 * Test introspection.  sc_bs_unknown_mask returns the n*groups sliced
 * unknown words; bit j of word [v*groups+g] is codeword g*64+j.
 * sc_bs_check_count decodes the sliced unknown count of one check for one
 * codeword.  Neither is needed by the decoder itself.
 */
const uint64_t *sc_bs_unknown_mask(const sc_bs *bs);
int sc_bs_groups(const sc_bs *bs);
int sc_bs_check_count(const sc_bs *bs, int check, int cw);

#endif /* SC_LDPC_BS_H */
