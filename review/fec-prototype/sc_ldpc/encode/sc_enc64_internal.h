/*
 * sc_enc64_internal.h - plan representation shared by the scalar and SSE2
 * kernels.  Not part of the public interface.
 *
 * The plan is the fast decoder check layout of sc_ldpc (commit 345682d):
 * cadj[ck*dcs + k] is the full adjacency of check ck with uint16 variable
 * indices, exactly like cadj_f[], except that the parity variable the check
 * owns is moved to the last slot of the row.  The XOR list is [0, cdeg-1);
 * cdeg[ck] is the degree.  A row is a permutation of the decoder's row, so
 * the array can be shared unchanged.  pair[ck] != 0 means check ck and ck+1
 * are independent and may keep both accumulators live.
 */
#ifndef SC_ENC64_INTERNAL_H
#define SC_ENC64_INTERNAL_H

#include "sc_enc64.h"

struct sc_enc64 {
    int n;
    int n_data;
    int n_par;
    int n_chk;
    int dcs;        /* check stride: max check degree */
    uint16_t *cadj; /* n_chk * dcs, full adjacency including the owned parity */
    uint8_t *cdeg;  /* n_chk */
    int *owned;     /* n_chk: parity variable solved by this check */
    unsigned char *pair;
    long xors;      /* XORs per batch: sum(cdeg) = edges */
};

#endif /* SC_ENC64_INTERNAL_H */
