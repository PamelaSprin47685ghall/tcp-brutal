#ifndef FEC_H
#define FEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Block erasure code over GF(2^8).
 *
 * The code is (n + m, n). The encoder splits the input into n data shares.
 * It produces m parity shares. Any n of the n + m shares rebuild all shares.
 * M = 1 uses a single XOR parity share.
 * M > 1 uses a systematic Cauchy matrix.
 */

#define FEC_MAX_SHARES 255u
#define FEC_MASK_BYTES 32u

enum fec_status
{
    FEC_OK = 0,
    FEC_ERR_PARAM = -1,   /* NULL pointer or bad share_len */
    FEC_ERR_TOO_FEW = -2, /* fewer than n shares are present */
    FEC_ERR_MASK = -3,    /* mask has a bit outside n + m */
    FEC_ERR_DUP = -4,     /* two share buffers are the same pointer */
    FEC_ERR_NOMEM = -5,
    FEC_ERR_SINGULAR = -6 /* matrix inversion failed */
};

struct fec_ctx;

/*
 * Create a context for n data shares and m parity shares.
 * Return NULL for n == 0, m == 0, n + m > 255, or on memory failure.
 * The function checks the MDS property before it returns.
 */
struct fec_ctx *fec_new(unsigned n, unsigned m);
void fec_free(struct fec_ctx *ctx);

unsigned fec_n(const struct fec_ctx *ctx);
unsigned fec_m(const struct fec_ctx *ctx);

/*
 * Encode n data shares into m parity shares.
 * Each share holds share_len bytes. share_len must be a power of two.
 * Buffers must not overlap.
 */
int fec_encode(struct fec_ctx *ctx, uint8_t *data_shares[],
               uint8_t *parity_shares[], size_t share_len);

/*
 * Decode in place.
 *
 * shares[] holds n + m buffers, each share_len bytes.
 * Bit i of present_mask says that shares[i] holds valid data.
 * Bit 0..n-1 are data shares. Bit n..n+m-1 are parity shares.
 * The function rebuilds every missing share. All buffers must be distinct.
 * It returns FEC_ERR_TOO_FEW when fewer than n shares are present.
 */
int fec_decode(struct fec_ctx *ctx, uint8_t *shares[],
               const uint8_t present_mask[FEC_MASK_BYTES], size_t share_len);

#endif /* FEC_H */
