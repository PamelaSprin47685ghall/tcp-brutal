/*
 * sc_enc64.h - bit-sliced batch encoder for the SC-IRA construction.
 *
 * One batch carries 64 independent codewords.  Every variable node of the
 * code owns one uint64_t word; bit b of that word is the variable's value in
 * codeword b.  Payload words occupy [0, n_data), parity words [n_data, n).
 * The numbering is exactly the variable numbering of sc_ldpc.h: parity words
 * of coupling block a are n_data + a*m + b.  The lifting permutation
 * scatters ownership inside the block, so the check that solves a parity
 * word is recorded in sc_graph.owned[], not by index arithmetic.  The extra
 * parity of check n_base*m + j is word n_data + n_base*m + j.
 *
 * Encoding is the same forward sweep as sc_encode(): for check ck, XOR the
 * 64-bit words of every adjacent variable except the parity variable that
 * check owns, then store the result into that parity word.  The accumulator
 * structure guarantees that every word read is either payload or parity of an
 * earlier check, so one pass over the checks is exact; sc_enc64_build()
 * verifies that property and rejects a graph that lacks it.  All arithmetic
 * is 64-way XOR; there is no carry, no shift and no per-codeword branch.
 *
 * The plan uses the fast decoder check layout of sc_ldpc (commit 345682d):
 * a fixed-stride uint16 adjacency, dcs = max check degree, with the owned
 * parity in the last slot of each row.  Check and variable order are the
 * code object's order, so a row is a permutation of the decoder's cadj_f row
 * and the decoder can share the array.  Codes with 65536 or more variables
 * are rejected; the app's codes are far below that.
 *
 * The SSE2 kernel keeps two independent batches in the two 64-bit lanes of
 * one 128-bit word (codewords 0..63 in lane 0, 64..127 in lane 1), so the
 * same check plan runs at 128 bits per XOR.  See review/sc-ldpc-bit64.md.
 */

#ifndef SC_ENC64_H
#define SC_ENC64_H

#include <stddef.h>
#include <stdint.h>

#include "sc_ldpc.h"

#define SC_ENC64_BATCH 64

typedef struct sc_enc64 sc_enc64;

/*
 * Compile the check plan of c.  The plan is read-only afterwards and holds
 * no reference to c; the code object may be freed first.  Returns NULL and
 * fills err[] if the code is not systematically encodable or on OOM.
 */
sc_enc64 *sc_enc64_build(sc_code *c, char *err, size_t errlen);
void sc_enc64_free(sc_enc64 *e);

int sc_enc64_n(const sc_enc64 *e);
int sc_enc64_n_data(const sc_enc64 *e);
int sc_enc64_n_par(const sc_enc64 *e);
int sc_enc64_n_chk(const sc_enc64 *e);
long sc_enc64_xors(const sc_enc64 *e); /* XORs per batch, all checks */

/*
 * Scalar kernel.  v[] holds n words: payload in [0, n_data) on entry, parity
 * in [n_data, n) on exit.  Payload words are left unchanged.
 */
void sc_enc64_run(const sc_enc64 *e, uint64_t *v);

/*
 * SSE2 kernel, 2 x 64 bits.  v[] holds n 128-bit words, 16-byte aligned; the
 * low 64 bits of word k are the codewords 0..63 plane of variable k and the
 * high 64 bits are the codewords 64..127 plane.  Returns 0, or -1 if this
 * build has no SSE2 support.
 */
int sc_enc128_run_sse2(const sc_enc64 *e, void *v);

/*
 * Bit-plane transpose for the wire format.  in[] holds 64 codewords of
 * n_bits bits each, codeword-major, 8 bits per byte, least significant bit
 * first: bit k of codeword b is (in[b * (n_bits / 8) + k / 8] >> (k % 8)) & 1.
 * sc_pack64() writes out[0 .. n_bits-1], out[k] bit b = that bit.
 * sc_unpack64() is its inverse.  n_bits must be a multiple of 8.
 */
void sc_pack64(uint64_t *out, const uint8_t *in, int n_bits);
void sc_unpack64(uint8_t *out, const uint64_t *in, int n_bits);

#endif /* SC_ENC64_H */
