#include "gf256.h"

uint8_t gf256_exp[512];
uint8_t gf256_log[256];

static int gf256_ready;

void gf256_init(void)
{
    unsigned i;
    unsigned x;

    if (gf256_ready)
    {
        return;
    }

    x = 1;
    for (i = 0; i < 255; i++)
    {
        gf256_exp[i] = (uint8_t)x;
        gf256_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u)
        {
            x ^= 0x11du;
        }
    }

    /* Duplicate the cycle. Then log(a) + log(b) needs no modulo. */
    for (i = 255; i < 512; i++)
    {
        gf256_exp[i] = gf256_exp[i - 255];
    }

    gf256_log[0] = 0;
    gf256_ready = 1;
}

uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
    {
        return 0;
    }

    return gf256_exp[(unsigned)gf256_log[a] + gf256_log[b]];
}

uint8_t gf_inv(uint8_t a)
{
    if (a == 0)
    {
        return 0;
    }

    return gf256_exp[255u - gf256_log[a]];
}

uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
    {
        return 0;
    }

    return gf256_exp[(unsigned)gf256_log[a] + 255u - gf256_log[b]];
}
