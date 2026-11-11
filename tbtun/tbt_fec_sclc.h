/* tbt_fec_sclc.h - downlink SC-IRA codec, 256-share rate ladder.
 *
 * The code is the SC_TERM_ACC construction of review/fec-prototype/sc_ldpc:
 * a ring payload window over L = 8 coupling blocks and a non-wrapping parity
 * accumulator. The FEC block always has n data shares and m parity shares
 * with n + m = 256, so one codeword carries n*8 payload bits and m*8
 * transmitted parity bits. The shipped ladder is 64/192, 76/180, 102/154,
 * 128/128, 151/105 and 178/78 (25/30/40/50/60/70 percent).
 *
 * One 1 KiB share carries one 8-bit segment of all 1024 codewords:
 *
 *     share byte 64*b + c, bit j  =  codeword b*64+c, bit 8*s+j,
 *
 * where s is the data (or parity) share index. Data share s holds payload
 * bits 8*s..8*s+7 of every codeword; parity share s holds transmitted
 * parity bits 8*s..8*s+7 of every codeword. Batch b is the 64 codewords
 * b*64..b*64+63, and its segment is share bytes 64*b .. 64*b+63. There are
 * always 16 batches, independent of n and m: a share is 1024 bytes and a
 * segment is 64 bytes. A lost share therefore erases the same eight-bit
 * position in all 64 codewords of every batch, which is exactly the --corr
 * mask shape of the bit-sliced benchmark: 64 codewords share one erasure
 * mask, but a codeword loses only 8 of its n*8 payload bits.
 *
 * Graph shape. Each coupling block holds n payload variables, m parity
 * variables and m checks. A payload block connects to dv check supernodes
 * (i..i+dv-1) mod 8, one edge per supernode. dv is 5 when n <= m and 4 when
 * n > m; above 50% the check count drops with the parity overhead, so the
 * payload degree drops too and no check outgrows the four-plane unknown
 * counter. Each incoming payload group places ceil(n/m) or floor(n/m) edges
 * on every check through a shuffled slot array, so the check degree is at
 * most 14. Every check also has its own parity edge and, except supernode 0,
 * the accumulator parity edge from the previous block.
 *
 * Nothing is punctured: the m parity variables per block are exactly the m
 * transmitted parity shares. The encoder therefore computes no parity that
 * the wire discards, and the decoder starts a parity variable as unknown
 * only when its share was lost. That is what makes 60 and 70 percent usable
 * at their controller boundaries: at p = 0.1043 the 178/78 graph sees only
 * the channel erasures, not an extra 100 punctured variables per block.
 *
 * This file and tbt_fec_sclc.c compile twice: into the module and into the
 * host test. No stdio, no math, no pthread, no SSE: the 64-bit word is the
 * vector unit.
 */
#ifndef _TBT_FEC_SCLC_H
#define _TBT_FEC_SCLC_H

#include "tbt_fec.h"

#define TBT_SCLC_BLOCK 256 /* transmitted variable nodes per coupling block */
#define TBT_SCLC_L 8       /* coupling length */
#define TBT_SCLC_SHARES TBT_FEC_MAX_SHARES /* data + parity shares */
#define TBT_SCLC_BATCH 64  /* codewords per batch */
#define TBT_SCLC_SEG_BYTES 64 /* one batch's 64 codewords in a share */
#define TBT_SCLC_SHARE TBT_FEC_PAYLOAD_MAX /* 1024 bytes */
#define TBT_SCLC_BATCHES (TBT_SCLC_SHARE / TBT_SCLC_SEG_BYTES) /* 16 */
/* Payload degree below/at 50%, and above 50%. */
#define TBT_SCLC_DV 5
#define TBT_SCLC_DV_HIGH 4
/* Highest check degree: four payload groups of at most three edges plus two
 * parity edges, so the shipped splits stay at 14 or below. The four-plane
 * counter represents 0..15; a larger degree would silently wrap it. */
#define TBT_SCLC_CNT_MAX 15

extern const struct tbt_fec_ops tbt_fec_ops_sclc;

#endif /* _TBT_FEC_SCLC_H */
