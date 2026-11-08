/* tbt_fec_sclc.h - downlink SC-IRA codec (rate 0.50, block 256, L 8).
 *
 * The code is the SC_TERM_ACC construction of review/fec-prototype/sc_ldpc:
 * a ring payload window of degree 5 over L = 8 coupling blocks and a
 * non-wrapping parity accumulator of degree 2.  block = 256 variable nodes
 * per coupling block gives N = 2048 variables, n_data = 1024 payload bits
 * and n_chk = 1024 parity bits per codeword.
 *
 * One 8 KiB share carries one 8-bit segment of all 8192 codewords:
 *
 *     share byte 64*b + c, bit j  =  codeword b*64+c, bit 8*s+j,
 *
 * where s is the data (or parity) share index.  Data share s holds payload
 * bits 8*s..8*s+7 of every codeword; parity share s holds parity bits
 * 8*s..8*s+7 of every codeword.  Batch b is the 64 codewords b*64..b*64+63,
 * and its segment is share bytes 64*b .. 64*b+63.  A lost share therefore
 * erases the same eight-bit position in all 64 codewords of every batch,
 * which is exactly the --corr mask shape of the bit-sliced benchmark: 64
 * codewords share one erasure mask, but a codeword loses only 1/128 of its
 * payload, so peeling still has a seed.  A 256-share FEC block carries 128
 * data shares and 128 parity shares, 8192 codewords, and 128 batches.
 *
 * This file and tbt_fec_sclc.c compile twice: into the module and into the
 * host test.  No stdio, no math, no pthread, no SSE: the 64-bit word is the
 * vector unit.
 */
#ifndef _TBT_FEC_SCLC_H
#define _TBT_FEC_SCLC_H

#include "tbt_fec.h"

#define TBT_SCLC_BLOCK 256 /* variable nodes per coupling block */
#define TBT_SCLC_L 8       /* coupling length */
#define TBT_SCLC_N 2048    /* variables per codeword (block * L) */
#define TBT_SCLC_N_DATA 1024
#define TBT_SCLC_N_PAR 1024
#define TBT_SCLC_N_CHK 1024
#define TBT_SCLC_BATCH 64  /* codewords per batch */
#define TBT_SCLC_SEGS 128  /* data (and parity) shares per block */
#define TBT_SCLC_SEG_BYTES 64 /* one batch's 64 codewords in a share */
#define TBT_SCLC_SHARE TBT_FEC_PAYLOAD_MAX /* 8192 bytes */
#define TBT_SCLC_RATE 500  /* parts per thousand */

extern const struct tbt_fec_ops tbt_fec_ops_sclc;

#endif /* _TBT_FEC_SCLC_H */
