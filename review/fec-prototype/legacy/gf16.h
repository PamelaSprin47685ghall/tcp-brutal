#ifndef GF16_H
#define GF16_H

#include <stdint.h>

/*
 * GF(2^4) with the primitive polynomial x^4 + x + 1 (0x13).
 * The multiply table is 16 x 16 bytes. Call gf16_init() once.
 */

extern uint8_t gf16_mul_tab[256];
extern uint8_t gf16_inv_tab[16];

void gf16_init(void);

uint8_t gf16_mul(uint8_t a, uint8_t b);
uint8_t gf16_inv(uint8_t a);

#endif /* GF16_H */
