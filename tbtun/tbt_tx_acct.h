/* tbt_tx_acct.h - the pure accounting and path-bound rules of tbt_tx.c.
 *
 * This header compiles twice: into the module and into the host tests
 * (host/test_tx_publish.c, host/test_tx_index.c). It uses no kernel facility
 * a plain C program lacks, so the tests exercise the exact code the data path
 * runs instead of a second copy of the rules.
 *
 * The invariants it encodes:
 *   tx_queued    = sum(primary len) + sum(duplicate len)
 *   dup_bytes    = sum(created duplicate len) - sum(dropped duplicate len)
 *   dup_pending  = duplicates queued for one retain entry, not yet written or
 *                  dropped
 *   retain_bytes = sum(retain entry len), one chunk per entry
 */
#ifndef _TBT_TX_ACCT_H
#define _TBT_TX_ACCT_H

#include "tbt_uapi.h"

/* Kernel bitmaps are a plain unsigned-long array; the host build needs the
 * same three primitives the TX scheduler uses so the tests exercise the real
 * index arithmetic (0..255), not a narrower copy. */
#ifndef __KERNEL__
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + 63) / 64]
static inline void tbt_host_bitmap_set(unsigned int nr, unsigned long *b)
{
    b[nr / 64] |= 1ul << (nr % 64);
}
static inline int tbt_host_bitmap_test(unsigned int nr,
                                       const unsigned long *b)
{
    return (int)((b[nr / 64] >> (nr % 64)) & 1ul);
}
#define set_bit(nr, addr) tbt_host_bitmap_set((nr), (addr))
#define test_bit(nr, addr) tbt_host_bitmap_test((nr), (addr))
#endif

/* The legacy retain entry has 16 slots per path (path_gen/path_end/
 * first_sent_seq/una_observed_ns). FEC sessions have up to 256 carriers and
 * never create these entries. */
#define TBT_ARQ_MAX_PATHS TBT_LEGACY_MAX_CARRIERS

/* One publish/drop accounting snapshot. `dup_bytes` is a delta for a single
 * publish; the caller adds it to its atomic64 counter. */
struct tbt_tx_acct
{
    u32 tx_queued;
    u64 dup_bytes;
    u32 retain_bytes;
};

static inline u32 tbt_tx_acct_total(u16 chunk, unsigned int ndup)
{
    return (u32)chunk * (1u + ndup);
}

static inline u32 tbt_tx_acct_dup_len(u16 chunk, unsigned int ndup)
{
    return (u32)chunk * ndup;
}

/* The authoritative queue-capacity check: the whole set must fit, or nothing
 * is published and seq does not move. */
static inline bool tbt_tx_acct_fits(u32 tx_queued, u32 txq_max, u16 chunk,
                                    unsigned int ndup)
{
    return (u64)tx_queued + tbt_tx_acct_total(chunk, ndup) <= txq_max;
}

/* Account one successful publish: primary plus ndup duplicates, one retain
 * entry. dup_pending is set once, not incremented per duplicate. */
static inline void tbt_tx_acct_publish(struct tbt_tx_acct *a, u16 chunk,
                                       unsigned int ndup, u8 *dup_pending)
{
    a->tx_queued += tbt_tx_acct_total(chunk, ndup);
    a->dup_bytes += tbt_tx_acct_dup_len(chunk, ndup);
    a->retain_bytes += chunk;
    if (dup_pending)
        *dup_pending = (u8)ndup;
}

/* Account one duplicate frame removed from the queue without a write. The
 * caller subtracts its len from tx_queued. Returns the duplicate bytes
 * actually subtracted (0 or len); the non-zero guard prevents underflow when
 * the counter was already reduced by another path. dup_pending is decremented
 * only when it is non-zero and the retain entry still exists (a trim/gc that
 * freed it clears fr->ret first). */
static inline u64 tbt_tx_acct_drop(u64 dup_bytes, u16 len, u8 *dup_pending)
{
    u64 sub = dup_bytes >= len ? len : 0;

    if (dup_pending && *dup_pending)
        (*dup_pending)--;
    return sub;
}

static inline bool tbt_tx_acct_is_dup(u8 type, bool dup_copy)
{
    return type == TBT_F_DATA && dup_copy;
}

/* Pure path bound: idx must name a slot the session owns and fit the 16-slot
 * retain arrays. No warning here; FEC sessions legally walk 256 slots. */
static inline bool tbt_tx_path_in_range(unsigned int idx, unsigned int n_car)
{
    return idx < TBT_ARQ_MAX_PATHS && idx < n_car;
}

/* Retain-array bound with an explicit out-of-range signal. The kernel wrapper
 * maps `warned` to WARN_ON_ONCE; the host test asserts that only idx >= 16
 * sets it, never a legal 0..15 slot. */
static inline bool tbt_tx_retain_path_in_range(unsigned int idx,
                                               unsigned int n_car,
                                               bool *warned)
{
    if (idx >= TBT_ARQ_MAX_PATHS)
    {
        if (warned)
            *warned = true;
        return false;
    }
    return idx < n_car;
}

#endif /* _TBT_TX_ACCT_H */
