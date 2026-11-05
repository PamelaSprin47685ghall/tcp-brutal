#include "gf16.h"

uint8_t gf16_mul_tab[256];
uint8_t gf16_inv_tab[16];

static int gf16_ready;

static uint8_t mul_slow(uint8_t a, uint8_t b)
{
    unsigned x = a & 0xfu;
    unsigned y = b & 0xfu;
    unsigned r = 0;

    while (y != 0)
    {
        if (y & 1u)
        {
            r ^= x;
        }
        y >>= 1;
        x <<= 1;
        if (x & 0x10u)
        {
            x ^= 0x13u;
        }
    }

    return (uint8_t)(r & 0xfu);
}

void gf16_init(void)
{
    unsigned a;
    unsigned b;

    if (gf16_ready)
    {
        return;
    }

    for (a = 0; a < 16; a++)
    {
        for (b = 0; b < 16; b++)
        {
            gf16_mul_tab[a * 16 + b] = mul_slow((uint8_t)a, (uint8_t)b);
        }
    }

    for (a = 1; a < 16; a++)
    {
        for (b = 1; b < 16; b++)
        {
            if (gf16_mul_tab[a * 16 + b] == 1)
            {
                gf16_inv_tab[a] = (uint8_t)b;
                break;
            }
        }
    }

    gf16_ready = 1;
}

uint8_t gf16_mul(uint8_t a, uint8_t b)
{
    return gf16_mul_tab[(a & 0xfu) * 16 + (b & 0xfu)];
}

uint8_t gf16_inv(uint8_t a)
{
    if (a == 0)
    {
        return 0;
    }

    return gf16_inv_tab[a & 0xfu];
}
