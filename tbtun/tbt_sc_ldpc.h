/*
 * tbt_sc_ldpc.h - SC-IRA graph construction for the uplink GF(16) codec.
 *
 * Kernel port of the host prototype in
 * review/fec-prototype/sc_ldpc/sc_ldpc.{c,h} (w-sclc-gf16).  Only the
 * SC_TERM_ACC construction is used in production; the other termination
 * modes stay for the host diagnostic tools and are not reachable from the
 * uplink codec.  No floating point, no libc: allocation is sclc_zalloc().
 */

#ifndef _TBT_SC_LDPC_H
#define _TBT_SC_LDPC_H

#include "tbt_uapi.h"

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* kvzalloc/kvfree in the module, calloc/free on the host.  All calls happen
 * in process context while a codec is built or freed. */
static inline void *sclc_zalloc(size_t len)
{
#ifdef __KERNEL__
    return kvzalloc(len ? len : 1, GFP_KERNEL);
#else
    return calloc(1, len ? len : 1);
#endif
}

static inline void sclc_free(void *p)
{
#ifdef __KERNEL__
    kvfree(p);
#else
    free(p);
#endif
}

/* Termination / boundary rule of the coupling. */
typedef enum
{
    SC_TERM_NATIVE = 0,
    SC_TERM_CAUSAL = 1,
    /* Production: ring payload window plus a non-wrapping parity
     * accumulator.  M = L*m checks at rate 0.50; lower rates append extra
     * checks. */
    SC_TERM_ACC = 2
} sc_term;

typedef enum
{
    SC_MODEL_SYMMETRIC = 0,
    SC_MODEL_SYSTEMATIC = 1
} sc_model;

typedef struct
{
    int block;      /* variable nodes per coupling block, q*m; even, >= 16 */
    int L;          /* coupling length, >= 2 */
    u16 rate_num;   /* code rate = rate_num / rate_den */
    u16 rate_den;
    int term;       /* sc_term; production uses SC_TERM_ACC */
    int extra_dc;   /* payload degree of each extra check node */
    u64 seed;       /* construction seed */
} sc_params;

typedef struct sc_code sc_code;

typedef struct
{
    int n;
    int n_data;
    int n_par;
    int n_chk;
    int n_base;
    int m;
    int n_extra;
    long edges;
    int dc_max;
    const int *voff;
    const int *coff;
    const int *vadj;
    const int *cadj;
    const int *owned;
} sc_graph;

typedef struct
{
    long unresolved;
    long target;
    long rounds;
    long edge_visits;
    long resolved;
} sc_dec_stat;

void sc_get_graph(const sc_code *c, sc_graph *out);

/* Build a code.  Returns NULL and fills err[] on invalid parameters. */
sc_code *sc_build(const sc_params *p, char *err, size_t errlen);
void sc_free(sc_code *c);

int sc_data_var(const sc_code *c, int k);
int sc_encode(sc_code *c, uint8_t *code);
int sc_verify(const sc_code *c, const uint8_t *code);
sc_dec_stat sc_decode(sc_code *c, const uint8_t *recv, const uint8_t *erased,
                      uint8_t *out, int target_data_only);
int sc_systematic(const sc_code *c);
const uint8_t *sc_known_bits(const sc_code *c);

#endif /* _TBT_SC_LDPC_H */
