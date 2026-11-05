#ifndef FEC_GF256_H
#define FEC_GF256_H

#include <stdint.h>

/*
 * GF(2^8) with the primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d).
 *
 * The tables are static. Call gf256_init() once before any other function.
 * fec_new() calls gf256_init(), so callers of the FEC API need not call it.
 */

extern uint8_t gf256_exp[512];
extern uint8_t gf256_log[256];

/* Build the exp/log tables. This function is idempotent. */
void gf256_init(void);

/* Return a * b in GF(2^8). */
uint8_t gf_mul(uint8_t a, uint8_t b);

/* Return 1 / a in GF(2^8). Return 0 when a is 0. */
uint8_t gf_inv(uint8_t a);

/* Return a / b in GF(2^8). Return 0 when b is 0. */
uint8_t gf_div(uint8_t a, uint8_t b);

#endif /* FEC_GF256_H */
