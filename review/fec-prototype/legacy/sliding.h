#ifndef SLIDING_H
#define SLIDING_H

#include <stddef.h>
#include <stdint.h>

#include "gf16.h"
#include "gf256.h"

/*
 * Sliding-window erasure code.
 *
 * The stream holds L data packets of s bytes. Every k data packets produce m
 * parity packets. Group g covers data packets
 * [g*k - (w - k), g*k + k - 1]. The window has length w and the step is k.
 * Neighbouring groups overlap by w - k packets. A group uses only the packets
 * that exist in [0, L).
 *
 * Four layers share one API. The layer picks the coefficient function.
 *
 *   L0 xor:        m = 1. Every coefficient is 1. One XOR per byte.
 *   L1 xor-part:   Row r covers the subset j % m == r. Each subset holds one
 *                  XOR equation. It recovers at most one loss per subset.
 *                  The code is not MDS.
 *   L2 rs4:        GF(2^4) Cauchy coefficients. Requires n + m <= 15 and
 *                  m + w <= 16. Any m losses in one window are recoverable.
 *   L3 rs8:        GF(2^8) Cauchy coefficients. Requires n + m <= 255 and
 *                  m + w <= 256. Any m losses in one window are recoverable.
 *
 * The coefficients depend only on (scheme, row, j, m). The receiver computes
 * them. They never travel on the wire.
 */

#define SLIDING_KIND_DATA 0
#define SLIDING_KIND_PARITY 1

enum sliding_scheme
{
    SLIDING_L0_XOR = 0,
    SLIDING_L1_XOR_PART = 1,
    SLIDING_L2_RS4 = 2,
    SLIDING_L3_RS8 = 3
};

enum sliding_status
{
    SLIDING_OK = 0,
    SLIDING_ERR_PARAM = -1,
    SLIDING_ERR_NOMEM = -2,
    SLIDING_ERR_STUCK = -3,
    SLIDING_ERR_UNRECOVERABLE = -4
};

struct sliding_ctx;

struct sliding_parity_info
{
    unsigned group;
    unsigned m;
    int win_start; /* May be negative for group 0. */
    int win_end;
};

/*
 * Coefficient for row and window offset j.
 * j is the offset inside the window: 0 <= j < w.
 */
static inline uint8_t sliding_coef(enum sliding_scheme scheme, unsigned row,
                                   unsigned j, unsigned m)
{
    switch (scheme)
    {
    case SLIDING_L0_XOR:
        return 1;
    case SLIDING_L1_XOR_PART:
        return j % m == row ? 1 : 0;
    case SLIDING_L2_RS4:
        return gf16_inv((uint8_t)(row ^ (m + j)));
    case SLIDING_L3_RS8:
    default:
        return gf_inv((uint8_t)(row ^ (m + j)));
    }
}

/*
 * Create a context.
 * n is the number of data carriers. It only sizes caller-side buffers and
 * does not enter the coding matrix. The matrix depends on (m, w) only.
 * Reject n == 0, m == 0, k == 0, w < k, s == 0, and n + m > 255.
 * L0 requires m == 1.
 * L2 requires m + w <= 16.
 * L3 requires m + w <= 256.
 */
struct sliding_ctx *sliding_new(unsigned n, unsigned m, unsigned k,
                                unsigned w, size_t s,
                                enum sliding_scheme scheme);
void sliding_free(struct sliding_ctx *ctx);

unsigned sliding_parity_count(const struct sliding_ctx *ctx, unsigned L);

/*
 * Encode L data packets into parity packets.
 * parity[] must hold sliding_parity_count(ctx, L) buffers of s bytes.
 * info may be NULL. Otherwise it receives one entry per parity packet.
 * Return the parity packet count, or a negative status.
 */
int sliding_encode(struct sliding_ctx *ctx, uint8_t *data[], unsigned L,
                   uint8_t *parity[], struct sliding_parity_info info[]);

/*
 * Reset the receive state and bind the packet buffers.
 * The caller keeps the buffers alive until the next bind or free.
 */
int sliding_bind(struct sliding_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                 unsigned L);

/*
 * One-shot decode.
 * data_present[i] != 0 means data[i] arrived. parity_present likewise.
 * The function rebuilds every recoverable data packet in place.
 * Return SLIDING_OK, or SLIDING_ERR_STUCK when a gap remains.
 */
int sliding_decode(struct sliding_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                   const uint8_t *data_present,
                   const uint8_t *parity_present, unsigned L);

/*
 * Mark one packet as received. kind is SLIDING_KIND_DATA or
 * SLIDING_KIND_PARITY. Return the new frontier (the number of consecutive
 * known data packets from 0), or a negative status.
 * A duplicate packet is ignored.
 */
int sliding_advance(struct sliding_ctx *ctx, int kind, unsigned index);

unsigned sliding_frontier(const struct sliding_ctx *ctx);
unsigned sliding_stuck_at(const struct sliding_ctx *ctx);
unsigned sliding_retired_groups(const struct sliding_ctx *ctx);
unsigned sliding_recovered_packets(const struct sliding_ctx *ctx);
int sliding_is_known(const struct sliding_ctx *ctx, unsigned index);
enum sliding_scheme sliding_scheme_of(const struct sliding_ctx *ctx);

#endif /* SLIDING_H */
