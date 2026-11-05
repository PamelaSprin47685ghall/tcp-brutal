#ifndef RLNC_H
#define RLNC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Sliding-window random linear network code over GF(2).
 *
 * Data packets d_0..d_{L-1} stream in order. Every k data packets produce m
 * parity packets. Group g covers
 *     [g*k + k - w, g*k + k - 1]  clipped to [0, L).
 *
 * Parity p(g, r) is the XOR of d selected positions in the window. The
 * selection is deterministic:
 *     seed = nonce ^ (g * m + r)
 *     positions = splitmix64(seed), d distinct values in [0, w).
 * The receiver regenerates the same positions. No coefficients travel on the
 * wire.
 *
 * n is the number of data carriers. It sizes caller-side buffers only and
 * does not enter the coding matrix. The matrix depends on (m, w, d).
 */

#define RLNC_KIND_DATA 0
#define RLNC_KIND_PARITY 1

enum rlnc_status
{
    RLNC_OK = 0,
    RLNC_ERR_PARAM = -1,
    RLNC_ERR_NOMEM = -2,
    RLNC_ERR_STUCK = -3
};

struct rlnc_ctx;

struct rlnc_ctx *rlnc_new(unsigned n, unsigned m, unsigned k, unsigned w,
                           size_t s, unsigned d, uint64_t nonce);
void rlnc_free(struct rlnc_ctx *ctx);

unsigned rlnc_parity_count(const struct rlnc_ctx *ctx, unsigned L);

/* Fill idx[0..d-1] with the selected positions in [0, w). */
unsigned rlnc_coeffs(const struct rlnc_ctx *ctx, unsigned g, unsigned r,
                     unsigned *idx, unsigned max);

int rlnc_encode(struct rlnc_ctx *ctx, uint8_t *data[], unsigned L,
                uint8_t *parity[]);

int rlnc_bind(struct rlnc_ctx *ctx, uint8_t *data[], uint8_t *parity[],
              unsigned L);

/*
 * Decode in place. data_present[i] and parity_present[p] are non-zero when
 * the packet arrived. Return the number of recovered packets, or a negative
 * status. On RLNC_ERR_STUCK no packet is written.
 */
int rlnc_decode(struct rlnc_ctx *ctx, uint8_t *data[], uint8_t *parity[],
                const uint8_t *data_present, const uint8_t *parity_present,
                unsigned L);

/* Mark one packet as received. Return the frontier or a negative status. */
int rlnc_advance(struct rlnc_ctx *ctx, int kind, unsigned index);

unsigned rlnc_frontier(const struct rlnc_ctx *ctx);
unsigned rlnc_stuck_at(const struct rlnc_ctx *ctx);
unsigned rlnc_recovered_packets(const struct rlnc_ctx *ctx);
int rlnc_is_known(const struct rlnc_ctx *ctx, unsigned index);
uint64_t rlnc_nonce(const struct rlnc_ctx *ctx);
/* Groups whose window ends before the frontier. */
unsigned rlnc_retired_groups(const struct rlnc_ctx *ctx);
/* Bytes of decode workspace that rlnc_bind() would allocate for L. */
size_t rlnc_memory_bytes(const struct rlnc_ctx *ctx, unsigned L);

#endif /* RLNC_H */
