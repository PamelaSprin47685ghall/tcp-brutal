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

/* Close-path scheduling quotas: while a stream is closing, one pump pass
 * writes at most 32 DATA frames and 8 other non-FIN frames before the next
 * pass, so a queued FIN is never starved by a replay/data burst. FIN is
 * always admitted. */
#define TBT_TX_CLOSE_DATA_MAX 32u
#define TBT_TX_CLOSE_CTL_MAX 8u

static inline bool tbt_tx_close_frame_allowed(bool close_priority, u8 type,
                                              u32 *data_used, u32 *ctl_used)
{
    if (!close_priority || type == TBT_F_FIN)
        return true;
    if (type == TBT_F_DATA)
    {
        if (*data_used >= TBT_TX_CLOSE_DATA_MAX)
            return false;
        (*data_used)++;
        return true;
    }
    if (*ctl_used >= TBT_TX_CLOSE_CTL_MAX)
        return false;
    (*ctl_used)++;
    return true;
}

/* FIN retries are owned by the close timer, not by TBT_CTL_MAX_RETRIES (that
 * bound exists for FBK/PAD/HELLO copies). A FIN may be requeued on any
 * hello_sent carrier while the close is inside its bound. */
static inline bool tbt_tx_fin_retry_allowed(u64 now, u64 close_since,
                                            u64 budget)
{
    return close_since && now >= close_since && now - close_since <= budget;
}

/* The close timer must not turn FEC tail repair or a peer credit wait into an
 * RST: the FIN gate is waiting for those last bytes. Repair activity holds
 * without a cap (it refreshes fec_repair_last_ns); a credit wait is held only
 * inside the same hard cap the stall path uses. */
#define TBT_TX_FEC_REPAIR_MAX_NS (90ULL * 1000000000ULL)

static inline bool tbt_tx_close_hold_fec(bool fec, bool repair_active,
                                         bool credit_waiting, u64 age,
                                         u64 max_ns)
{
    if (!fec)
        return false;
    if (repair_active)
        return true;
    return credit_waiting && age <= max_ns;
}

/* The sender stall reset holds while FEC repair is active, but only inside the
 * hard no-progress cap. Beyond it the stream is dead even if the repair clock
 * moved: a repair that never completes must still end. Non-FEC streams never
 * take this branch. */
static inline bool tbt_tx_fec_repair_holds(bool fec, bool repair_active,
                                           u64 age, u64 max_ns)
{
    return fec && repair_active && age <= max_ns;
}

/* Auto TX window: k x BDP alone is a single-flow estimate and can lock a
 * multi-carrier tunnel into a feedback loop (small window -> low rate -> small
 * BDP sample). The aggregate the carriers actually advertise is the floor:
 * sum(snd_wnd), or n_car x the per-carrier BDP when no window sample exists.
 * `floor` is the per-carrier bootstrap minimum scaled by the carrier count;
 * `cap` is the receiver's reorder budget and always wins. */
static inline u64 tbt_tx_window_auto(u64 k_bdp, u64 sum_wnd,
                                     u64 per_car_bdp, unsigned int n_car,
                                     unsigned int n_live, u64 floor,
                                     u64 cap)
{
    u64 agg = sum_wnd;
    u64 win;

    if (n_live && per_car_bdp)
    {
        u64 scaled = per_car_bdp * n_car;

        if (scaled > agg)
            agg = scaled;
    }
    win = k_bdp > agg ? k_bdp : agg;
    if (win < floor)
        win = floor;
    if (cap && win > cap)
        win = cap;
    return win ? win : 1;
}

/* One carrier's rate sample. BBR's delivery rate is preferred; a carrier that
 * has not filled that sample yet is still live, so fall back to the window it
 * advertises divided by its SRTT. Without this, a 32-carrier aggregate is
 * estimated from the few carriers that happened to produce a sample and the
 * BDP collapses to a single-flow value. */
static inline u64 tbt_tx_car_rate(u64 delivery_bps, u32 srtt_us,
                                  u32 snd_wnd, u32 snd_cwnd)
{
    u64 wnd;

    if (delivery_bps)
        return delivery_bps;
    wnd = snd_wnd > snd_cwnd ? snd_wnd : snd_cwnd;
    if (!wnd || !srtt_us)
        return 0;
    return wnd * 1000000ULL / srtt_us;
}

/* The receiver's reorder budget is also the sender window's auto cap. Eight
 * MiB is one path's budget; 256 carriers advertise far more, so scale it with
 * the carrier count (eight full frames each), bounded by the parser's 256 MiB
 * ceiling. An explicit tx_window_bytes leaves the operator's value alone. */
#define TBT_TX_REORDER_MIN (8ULL * 1024 * 1024)
#define TBT_TX_REORDER_MAX (256ULL * 1024 * 1024)

static inline u64 tbt_tx_reorder_floor(unsigned int n_car)
{
    u64 need = (u64)n_car * 8 * TBT_PAYLOAD_MAX;

    if (need < TBT_TX_REORDER_MIN)
        need = TBT_TX_REORDER_MIN;
    if (need > TBT_TX_REORDER_MAX)
        need = TBT_TX_REORDER_MAX;
    return need;
}

/* FEC data never enters the ARQ queue: it lives in the packetizer's ring and
 * the FEC out queues. With an empty tx_q/ctl_q the TX pump has nothing to do,
 * and running it only takes pump_lock away from the per-carrier FEC drain.
 * Non-FEC sessions always need the pump for their ARQ queue and timers. */
static inline bool tbt_tx_pump_idle(bool fec, u32 tx_queued, u32 ctl_queued)
{
    return fec && !tx_queued && !ctl_queued;
}

/* True when the session queues hold no application or control work. PAD owns
 * a control-queue slot but is keepalive, so it is not counted in ctl_real; a
 * PAD queued by carrier A's poll must not make carrier B look busy in the
 * same pass. Both counters are single-word reads, so the test needs no lock. */
static inline bool tbt_hold_session_idle(u32 tx_queued, u32 ctl_real)
{
    return !tx_queued && !ctl_real;
}

/* Peripheral BBR hold state rules (tbt_carrier.c). When a carrier goes idle
 * the kernel snapshots snd_cwnd and sk_pacing_rate; while it stays idle the
 * poll writes both back, so the next burst does not start from BBR's
 * idle-collapsed window. BBR keeps ownership of its own model: these rules
 * never touch delivered/rate_delivered, and every non-PAD writer cancels the
 * hold before it queues bytes, so the first real ACK owns the next decision.
 *
 * Enter requires no bytes in flight: an ACK is about to rewrite the window,
 * and the 200 ms idle interval already implies an empty send path for a real
 * carrier. */
static inline bool tbt_hold_enter(bool enable, bool no_inflight, bool tbt_idle,
                                  bool ca_open, bool lost, bool recent_loss,
                                  u64 idle_since, u64 now, u64 enter_ns)
{
    if (!enable || !no_inflight || !tbt_idle || !ca_open || lost ||
        recent_loss)
        return false;
    return idle_since && now >= idle_since && now - idle_since >= enter_ns;
}

static inline bool tbt_hold_keep(bool active, bool real_data, bool loss,
                                 bool wnd_shrunk, u64 now, u64 until_ns)
{
    return active && !real_data && !loss && !wnd_shrunk &&
           (!until_ns || now <= until_ns);
}

/* Snapshot bounds. The hold restores a window the path already accepted, but
 * the snapshot must not exceed any current limit: the peer window
 * (snd_wnd/mss), tcp_snd_cwnd_clamp, or the send buffer in packets
 * (sk_sndbuf/mss). A zero limit is "no limit" for the first three; mss==0 is
 * treated as 1 so the division cannot trap. A zero cwnd returns 0, which
 * means "no snapshot". */
static inline u32 tbt_hold_cwnd_save(u32 cwnd, u32 wnd_pkts, u32 clamp,
                                     u32 sndbuf, u32 mss)
{
    u32 buf_pkts;

    if (!cwnd)
        return 0;
    if (wnd_pkts && wnd_pkts < cwnd)
        cwnd = wnd_pkts;
    if (clamp && clamp < cwnd)
        cwnd = clamp;
    if (sndbuf)
    {
        buf_pkts = sndbuf / (mss ? mss : 1);
        if (buf_pkts < cwnd)
            cwnd = buf_pkts;
    }
    return cwnd ? cwnd : 1;
}

/* The clamp again at write time: tcp_snd_cwnd_clamp may have moved while the
 * hold was active. 0 still means "no snapshot". */
static inline u32 tbt_hold_cwnd_restore(u32 cwnd, u32 clamp)
{
    if (!cwnd)
        return 0;
    if (clamp && clamp < cwnd)
        cwnd = clamp;
    return cwnd ? cwnd : 1;
}

/* sk_max_pacing_rate is the operator's ceiling; a zero ceiling means none. */
static inline unsigned long tbt_hold_pacing_save(unsigned long pacing,
                                                 unsigned long max_rate)
{
    if (max_rate && pacing > max_rate)
        return max_rate;
    return pacing;
}

/* Why tbt_car_hold_apply() may not write this poll.
 *
 * Every non-PAD writer clears hold_active before it queues bytes:
 * tbt_carrier_xmit() for the ARQ/control pump, tbt_carrier_send_fec_batch()
 * for FEC data. A hold that is still active with bytes in flight is therefore
 * waiting for a PAD, whose writer deliberately exempts TBT_F_PAD. That is a
 * SKIP, not an exit: ending the hold here re-paid the 200 ms enter delay and
 * restarted the 10 s cap once per keepalive period. An inactive hold or a CA
 * that left Open is a real race with a live congestion decision and ends it.
 */
enum tbt_hold_apply
{
    TBT_HOLD_APPLY_OK = 0,
    TBT_HOLD_APPLY_SKIP,
    TBT_HOLD_APPLY_EXIT,
};

static inline enum tbt_hold_apply tbt_hold_apply_gate(bool active,
                                                      bool ca_open,
                                                      bool una_eq_nxt,
                                                      u32 packets_out)
{
    if (!active || !ca_open)
        return TBT_HOLD_APPLY_EXIT;
    if (!una_eq_nxt || packets_out)
        return TBT_HOLD_APPLY_SKIP;
    return TBT_HOLD_APPLY_OK;
}

/* Fold the legacy 16-bit path mask into a 256-bit carrier mask. The source is
 * a retained DATA copy, which only the old ARQ creates, so the loop bound is
 * the retain width, not TBT_MAX_CARRIERS_LIMIT. */
static inline void tbt_mask_add_u16(unsigned long *mask, u16 bits)
{
    int i;

    for (i = 0; i < TBT_ARQ_MAX_PATHS; i++)
        if (bits & (u16)(1u << i))
            set_bit(i, mask);
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
