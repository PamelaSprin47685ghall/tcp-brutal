/*
 * sc_enc64_sse2.c - SSE2 kernel for the bit-sliced batch encoder.
 *
 * Two independent 64-codeword batches share one 128-bit word: the low 64
 * bits (lane 0) carry codewords 0..63, the high 64 bits (lane 1) carry
 * codewords 64..127.  The check plan is the same fixed-stride uint16 plan as
 * the scalar kernel, so one forward sweep with 128-bit XORs encodes both
 * lanes.  See sc_enc64.h for the layout and sc_enc64_internal.h for the plan.
 */

#include "sc_enc64_internal.h"

#if defined(__SSE2__)

#include <emmintrin.h>

static inline __m128i check_xor_sse2(const sc_enc64 *e, int ck,
                                     const __m128i *restrict w)
{
    const uint16_t *restrict a =
        e->cadj + (size_t)ck * (size_t)e->dcs;
    int d = e->cdeg[ck] - 1;
    __m128i x = _mm_setzero_si128();
    int k;

    for (k = 0; k < d; k++)
        x = _mm_xor_si128(x, _mm_load_si128(w + a[k]));
    return x;
}

int sc_enc128_run_sse2(const sc_enc64 *e, void *v)
{
    __m128i *restrict w = (__m128i *)v;
    int ck = 0;
    int nchk = e->n_chk;

    while (ck < nchk) {
        if (e->pair[ck]) {
            int p0 = e->owned[ck];
            int p1 = e->owned[ck + 1];
            __m128i x0, x1;
            x0 = check_xor_sse2(e, ck, w);
            x1 = check_xor_sse2(e, ck + 1, w);
            _mm_store_si128(w + p0, x0);
            _mm_store_si128(w + p1, x1);
            ck += 2;
        } else {
            _mm_store_si128(w + e->owned[ck], check_xor_sse2(e, ck, w));
            ck++;
        }
    }
    return 0;
}

#else /* !__SSE2__ */

int sc_enc128_run_sse2(const sc_enc64 *e, void *v)
{
    (void)e;
    (void)v;
    return -1;
}

#endif /* __SSE2__ */
