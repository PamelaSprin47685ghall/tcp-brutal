/* tbt_fec_sclc.h - downlink SC-IRA codec (rate 0.50, block 64/128/256, L 8).
 *
 * The code is the SC_TERM_ACC construction of review/fec-prototype/sc_ldpc:
 * a ring payload window of degree 5 over L = 8 coupling blocks and a
 * non-wrapping parity accumulator of degree 2.  One FEC group is a complete
 * codec block of n data plus n parity shares, n in {32, 64, 128}; the
 * coupling block is w = 2n, so a group carries n_data = 8n payload bits and
 * n_chk = 8n parity bits per codeword.  A session may hold G groups
 * (G = 1, 2 or 4 at 256 carriers) and give each flow a subset of them: the
 * codec object only ever sees the groups that flow owns.  The historical
 * 1x256 downlink is G = 1, n = 128, block = 256.
 *
 * One 8 KiB share carries one 8-bit segment of all 8192 codewords:
 *
 *     share byte 64*b + c, bit j  =  codeword b*64+c, bit 8*s+j,
 *
 * where s is the data (or parity) share index.  Data share s holds payload
 * bits 8*s..8*s+7 of every codeword; parity share s holds parity bits
 * 8*s..8*s+7 of every codeword.  Batch b is the 64 codewords b*64..b*64+63,
 * and its segment is share bytes 64*b .. 64*b+63.  The share is 8 KiB for
 * every group size, so every group holds 128 batches; a group's payload is
 * n shares x 8 KiB.  A lost share erases the same eight-bit position in all
 * 64 codewords of every batch: 64 codewords share one erasure mask, but a
 * codeword loses only 1/(8n) of its payload, so peeling still has a seed.
 * A 256-share group carries 128 data + 128 parity shares (8192 codewords,
 * 128 batches); a 128-share group carries 64 + 64; a 64-share group 32 + 32.
 *
 * This file and tbt_fec_sclc.c compile twice: into the module and into the
 * host test.  No stdio, no math, no pthread, no SSE: the 64-bit word is the
 * vector unit.
 */
#ifndef _TBT_FEC_SCLC_H
#define _TBT_FEC_SCLC_H

#include "tbt_fec.h"

#define TBT_SCLC_MAX_SEGS 128  /* max data (and parity) shares per group */
#define TBT_SCLC_L 8           /* coupling length */
#define TBT_SCLC_BATCH 64      /* codewords per batch */
#define TBT_SCLC_SEG_BYTES 64  /* one batch's 64 codewords in a share */
#define TBT_SCLC_SHARE TBT_FEC_PAYLOAD_MAX /* 8192 bytes */
#define TBT_SCLC_RATE 500      /* parts per thousand */

extern const struct tbt_fec_ops tbt_fec_ops_sclc;

#endif /* _TBT_FEC_SCLC_H */
