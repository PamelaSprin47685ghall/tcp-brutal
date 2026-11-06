/*
 * sc_enc64_pack.c - bit-plane transpose between the wire byte format and the
 * 64-codeword bit planes.  See sc_enc64.h for the exact bit mapping.
 *
 * The transpose of an 8x8 bit matrix is done with three masked swap stages
 * (Hacker's Delight, "transpose8"), so one byte column of 64 codewords costs
 * eight transposes and eight byte insertions instead of 512 bit tests.
 */

#include "sc_enc64.h"

static uint64_t transpose8(uint64_t x)
{
    uint64_t t;

    t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
    x = x ^ t ^ (t << 7);
    t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
    x = x ^ t ^ (t << 14);
    t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
    x = x ^ t ^ (t << 28);
    return x;
}

/* Gather the byte at column j of codewords 8*g .. 8*g+7 into a uint64. */
static uint64_t gather8(const uint8_t *in, int stride, int j, int g)
{
    const uint8_t *p = in + (size_t)(8 * g) * (size_t)stride + j;
    uint64_t x = 0;
    int b;

    for (b = 0; b < 8; b++)
        x |= (uint64_t)p[(size_t)b * (size_t)stride] << (8 * b);
    return x;
}

void sc_pack64(uint64_t *out, const uint8_t *in, int n_bits)
{
    int stride = n_bits / 8;
    int j, g, k;

    for (j = 0; j < stride; j++) {
        uint64_t w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (g = 0; g < 8; g++) {
            uint64_t x = transpose8(gather8(in, stride, j, g));
            for (k = 0; k < 8; k++)
                w[k] |= ((x >> (8 * k)) & 0xffu) << (8 * g);
        }
        for (k = 0; k < 8; k++)
            out[8 * j + k] = w[k];
    }
}

void sc_unpack64(uint8_t *out, const uint64_t *in, int n_bits)
{
    int stride = n_bits / 8;
    int j, g, k, b;

    for (j = 0; j < stride; j++) {
        for (g = 0; g < 8; g++) {
            uint64_t x = 0;
            for (k = 0; k < 8; k++)
                x |= ((in[8 * j + k] >> (8 * g)) & 0xffu) << (8 * k);
            x = transpose8(x);
            for (b = 0; b < 8; b++)
                out[(size_t)(8 * g + b) * (size_t)stride + j] =
                    (uint8_t)((x >> (8 * b)) & 0xffu);
        }
    }
}
