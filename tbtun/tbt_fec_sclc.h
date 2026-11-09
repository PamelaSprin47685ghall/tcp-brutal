/* tbt_fec_sclc.h - downlink SC-IRA codec, 256-share rate ladder.
 *
 * The code is the SC_TERM_ACC construction of review/fec-prototype/sc_ldpc:
 * a ring payload window of degree 5 over L = 8 coupling blocks and a
 * non-wrapping parity accumulator. The FEC block always has n data shares
 * and m parity shares with n + m = 256, so one codeword carries n*8 payload
 * bits and m*8 transmitted parity bits. The shipped ladder is 64/192,
 * 76/180, 102/154, 128/128, 151/105 and 178/78 (25/30/40/50/60/70 percent).
 *
 * One 8 KiB share carries one 8-bit segment of all 8192 codewords:
 *
 *     share byte 64*b + c, bit j  =  codeword b*64+c, bit 8*s+j,
 *
 * where s is the data (or parity) share index. Data share s holds payload
 * bits 8*s..8*s+7 of every codeword; parity share s holds transmitted
 * parity bits 8*s..8*s+7 of every codeword. Batch b is the 64 codewords
 * b*64..b*64+63, and its segment is share bytes 64*b .. 64*b+63. There are
 * always 128 batches, independent of n and m: a share is 8192 bytes and a
 * segment is 64 bytes. A lost share therefore erases the same eight-bit
 * position in all 64 codewords of every batch, which is exactly the --corr
 * mask shape of the bit-sliced benchmark: 64 codewords share one erasure
 * mask, but a codeword loses only 8 of its n*8 payload bits.
 *
 * Graph shape. Let c = max(n, m). Each coupling block holds n payload
 * variables, c parity variables and c checks. A payload block connects to
 * the five check supernodes (i..i+4) mod 8, one edge per check, so every
 * payload variable keeps degree 5 and every check has five payload edges
 * plus its own parity and, except supernode 0, the accumulator parity edge
 * from block 7 (degree 6 or 7). This stays a rate-1/2-style graph at every
 * ladder point.
 *
 * For n <= m the c parity variables equal the m transmitted parity shares
 * and nothing is punctured. For n > m the graph still has c = n parity
 * variables per block, but only m of them are transmitted: each block
 * punctures its last n-m parity variables. A punctured variable starts as
 * an erasure and the peeling decoder solves it like any other unknown, so
 * the transmitted code rate is n / 256 while the underlying graph keeps the
 * strong rate-1/2 check degree. That is what makes 60 and 70 percent usable
 * at their low-loss operating points: the channel erasures and the
 * punctures together stay below the base graph's erasure threshold.
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
#define TBT_SCLC_SHARE TBT_FEC_PAYLOAD_MAX /* 8192 bytes */
#define TBT_SCLC_BATCHES (TBT_SCLC_SHARE / TBT_SCLC_SEG_BYTES) /* 128 */
/* A check has five payload edges plus at most two parity edges. */
#define TBT_SCLC_CNT_MAX 7

extern const struct tbt_fec_ops tbt_fec_ops_sclc;

#endif /* _TBT_FEC_SCLC_H */
