/* tbt_fec_data.c - FEC v6 data plane: packetizer, grouped block codec,
 * record writer, receiver reassembly.
 *
 * Scope (T2):
 *   - carriers = groups * (n + m), one share per carrier;
 *   - carrier c carries group c/(n+m) share c%(n+m);
 *   - a block starts at block_seq0 and packs its groups contiguously:
 *     seq = block_seq0 + group*n + share_index, while PARITY carries
 *     block_seq0 in h.seq. A block's own n/m travel in every share, so a
 *     shape switch changes the next block, not the mapping of an older one.
 *   - a block is n DATA packets followed by m parity packets; the codec is the
 *     host-tested GF(2) RLNC in tbt_fec.c, reused with a per-group, per-block
 *     nonce;
 *   - both containers: raw records, or one 48-byte record per HTTP chunk.
 *
 * The capture actor runs in softirq: tbt_fec_tx_submit only copies bytes into
 * a ring and schedules a work item. Packetization, encoding, and socket writes
 * run in tbt_fec_tx_work (process context), so no sleeping call is reachable
 * from the actor.
 *
 * The receiver runs inside the carrier parser, which may also be softirq. It
 * copies shares into preallocated block buffers, decodes with preallocated
 * workspaces, and queues ACKs to the same work item; it never allocates a
 * codec or calls a socket from that path.
 *
 * Delivery is by super-block: every group's block b covers one contiguous
 * global range starting at that block's seq0. A super-block is handed to the
 * stream only when every group's block b is decoded, so the byte stream has
 * no hole and the ACK frontier is a real global frontier, not a per-group
 * guess.
 */
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <net/tcp.h>

#include "tbtun.h"
#include "tbt_fec.h"
#include "tbt_fec_adapt.h"
#include "tbt_fec_adapt_sclc.h"
#include "tbt_sclc_uplink.h"
#include "tbt_rec.h"

static struct tbt_fec_blk *tbt_fec_rx_blk(struct tbt_fec_rx *rx, u16 group,
                                          u64 block_id);
static struct tbt_fec_grouped *tbt_fec_codec_new(
    const struct tbt_fec_sclc_shape *sclc, u16 groups, u16 n, u16 m, u16 k,
    u16 w, u8 d, u64 nonce);
static void tbt_fec_tx_init_tick(struct tbt_session *sess, struct tbt_fec_tx *tx);
static void tbt_fec_tx_sent_clear(struct tbt_fec_tx *tx);
static struct workqueue_struct *tbt_fec_free_wq;
static void tbt_fec_rx_blk_attach_locked(struct tbt_fec_rx *rx,
                                         struct tbt_fec_blk *b);
static void tbt_fec_rx_codec_queue_locked(struct tbt_fec_rx *rx,
                                          const struct tbt_fec_codec_key *key,
                                          struct tbt_fec_codec_entry *e);

/* ------------------------------------------------------------------ *
 * Continuous codec cache
 *
 * A codec build runs only in w_fec_tx/w_fec_rx. Cache eviction can run under
 * rx->lock or shape_lock, where kvfree() must not sleep, so the release
 * callback defers a vmalloc-backed codec to the module's free workqueue.
 * ------------------------------------------------------------------ */
struct tbt_fec_codec_free_req
{
    struct work_struct work;
    struct tbt_fec_grouped *codec;
};

static void tbt_fec_codec_free_work(struct work_struct *w)
{
    struct tbt_fec_codec_free_req *r =
        container_of(w, struct tbt_fec_codec_free_req, work);

    tbt_fec_grouped_free(r->codec);
    kfree(r);
}

static void *tbt_fec_codec_cache_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    return kvzalloc(bytes, GFP_KERNEL);
}

static void tbt_fec_codec_cache_release(void *ctx, void *ptr,
                                        enum tbt_fec_codec_cache_mem kind)
{
    struct tbt_fec_codec_free_req *r;

    (void)ctx;
    if (!ptr)
        return;
    if (kind == TBT_FEC_CODEC_CACHE_MEM_SLOTS)
    {
        kvfree(ptr);
        return;
    }
    if (!in_atomic() && !irqs_disabled())
    {
        tbt_fec_grouped_free(ptr);
        return;
    }
    r = kmalloc(sizeof(*r), GFP_ATOMIC);
    if (!r || !tbt_fec_free_wq)
    {
        /* A failed free would leak a codec; report it rather than call
         * kvfree() from an atomic section. */
        kfree(r);
        pr_warn_ratelimited("tbtun: fec codec free deferred allocation failed\n");
        return;
    }
    INIT_WORK(&r->work, tbt_fec_codec_free_work);
    r->codec = ptr;
    if (!queue_work(tbt_fec_free_wq, &r->work))
    {
        kfree(r);
        pr_warn_ratelimited("tbtun: fec codec free queue rejected a codec\n");
    }
}

/* The cache object is allocated separately from its entry array, so its
 * destroy() cannot free the object while a block still holds an entry: the
 * last put() calls this release hook. */
static void tbt_fec_codec_cache_release_obj(struct tbt_fec_codec_cache *c)
{
    kfree(c);
}

static int tbt_fec_codec_cache_setup(struct tbt_fec_codec_cache *c,
                                     u8 variant)
{
    u32 cap = variant == TBT_FEC_SCLC_UP ? TBT_FEC_CODEC_CACHE_UP
                                         : TBT_FEC_CODEC_CACHE_DOWN;
    int rc;

    rc = tbt_fec_codec_cache_init(c, cap, tbt_fec_codec_cache_alloc,
                                  tbt_fec_codec_cache_release, NULL);
    if (!rc)
        c->release = tbt_fec_codec_cache_release_obj;
    return rc;
}

/* A cache object lives at a stable address: its entries point back to it, so
 * INIT swaps the pointer under rx->lock and retires the old object outside
 * it. */
static struct tbt_fec_codec_cache *tbt_fec_codec_cache_new_obj(u8 variant)
{
    struct tbt_fec_codec_cache *c = kzalloc(sizeof(*c), GFP_KERNEL);

    if (!c)
        return NULL;
    if (tbt_fec_codec_cache_setup(c, variant))
    {
        kfree(c);
        return NULL;
    }
    return c;
}

static void tbt_fec_codec_cache_free_obj(struct tbt_fec_codec_cache *c)
{
    if (!c)
        return;
    c->free_self = 1;
    tbt_fec_codec_cache_destroy(c);
    if (!c->retiring)
    {
        c->free_self = 0;
        kfree(c);
    }
}

static void tbt_fec_codec_key_fill(struct tbt_fec_codec_key *k,
                                   const struct tbt_fec_sclc_shape *sh,
                                   u16 n, u16 m, u64 seed)
{
    memset(k, 0, sizeof(*k));
    k->n = n;
    k->m = m;
    k->block = sh->block;
    k->L = sh->L;
    k->seed = seed;
}

static struct tbt_fec_grouped *tbt_fec_codec_build(
    const struct tbt_fec_sclc_shape *sh, u16 n, u16 m, u16 groups, u64 seed)
{
    struct tbt_fec_sclc_shape s = *sh;
    u16 shares = (u16)(n + m);

    s.n = n;
    s.m = m;
    s.groups = groups;
    s.rate_ppm = (u16)((u32)n * 1000u / shares);
    s.rate_256 = (u16)((u32)n * 256u / shares);
    return tbt_fec_sclc_new(&s, TBT_FEC_S, seed);
}


/* The source ring is the capture's pause granularity: tbt_fec_tx_submit()
 * sets tx_paused only when the ring cannot take the next chunk. 256 KiB
 * paused capture every 32 packets while the credit window allowed 1024, so
 * the pump had to resume capture repeatedly. Size the ring from the credit
 * window, with the old 256 KiB floor and a 16 MiB ceiling. The 64-block
 * flight window lives in the lazy replay plane, not in this unencoded FIFO;
 * 16 MiB already holds two 8 MiB writes ahead of the packetizer. The ceiling
 * is allocated by tbt_fec_vzalloc() (vzalloc/kvzalloc), never a high-order
 * kzalloc. */
#define TBT_FEC_TX_RING (256 * 1024)
#define TBT_FEC_TX_RING_MAX (16u * 1024 * 1024)

/* tx->run / rx->run ownership bits. The sched bit in the session owns the
 * reference; this word owns the shared buffers and cursors. A work entry that
 * finds bit 0 set must only raise bit 1 and return: workqueue can run a
 * re-queued work_struct concurrently with its previous instance, and two
 * packetizers share tx->pkt/tx->rec, while two delivery walks share
 * next_super/deliver_bytes. */
#define TBT_FEC_RUN_BIT   0u
#define TBT_FEC_AGAIN_BIT 1u

/* How long a partial packet may wait for company. The packetizer emits a
 * share only when its packet is full, so an application that writes a small
 * request and then waits for the answer -- an HTTP request, iperf3's control
 * cookie -- would wait for S bytes that never come, and the tunnel would
 * look connected and silent. This timer is the interactive tail only: a
 * block that merely holds real shares is not sealed on it (see
 * TBT_FEC_BLOCK_IDLE_MS), or a 1 MB/s stream that drains the ring after
 * every 8 KiB write would cut a one-share block per write. */
#define TBT_FEC_SHORT_MS 2
/* A block that still holds real shares is sealed after this much silence.
 * The receiver learns a short block's cut only from its PARITY win_len, so a
 * stream that stops between blocks without closing must eventually say so.
 * 25 ms is inside the 20-50 ms short-block window: above the 8 ms gap
 * between the writes of a 1 MB/s sender (so a sustained block fills to
 * groups*n shares and sends parity only then), below any user-visible
 * request timeout (so a request/response stream that never half-closes is
 * delivered). FIN still closes the final block at once. */
#define TBT_FEC_BLOCK_IDLE_MS 25
/* After a block is sealed mid-write, the bytes left in the packetizer belong
 * to the same application write. A relay's source arrives in whole delivered
 * blocks, so the next bytes of that write can be a scheduling quantum late;
 * flushing the remainder on the same 25 ms timer turns every block-quantised
 * source into a short share at the next block's first slot. Give the seam one
 * longer, still sub-second window before the interactive tail takes it. */
#define TBT_FEC_CUT_GRACE_MS 100
/* A super-block that has not become ready for this long cannot become ready:
 * this wire has no NACK and no retransmission, so a share that never arrived
 * is never fetched. The bound is well above any recovery the parity can do
 * (all shares of one block arrive within one carrier burst) and below curl's
 * 120 s no-progress limit, so the application sees a reset instead of a
 * silent stall. */
#define TBT_FEC_STALL_MS 30000
#define TBT_FEC_NONCE_GOLDEN 0x9e3779b97f4a7c15ULL
/* 66 logical RX slots hold the 64-block sliding window (K-2) plus two
 * blocks of slack. Only the 66 lightweight descriptors are eager; a slot
 * allocates its (n+m)*S block buffer on the first valid share and returns
 * it at delivery, so a receive-light side commits only its live blocks. */
#define TBT_FEC_RX_SLOTS 66u
/* Free block buffers kept for reuse: at most two hot blocks, 4 MiB at the
 * 2 MiB 1x256 block size. Active blocks hold their own until delivery. */
#define TBT_FEC_RX_POOL_BLOCKS 2u
/* One FEC drain run sends at most this many batches before yielding to the
 * packetizer and the other carriers. A full 256-carrier burst is served in
 * four such runs, each of which re-arms with no delay while fresh bits
 * remain; the cap bounds the worker hold time, not the delivery latency. */
#define TBT_FEC_DRAIN_PASS_MAX 64
/* A carrier whose socket has refused a DATA record for this long is slow:
 * later scans try it after every healthy carrier, and one admission clears
 * the mark. A slow socket keeps its own TCP retransmissions; the flag
 * filters new dispatch only. */
#define TBT_SKIP_MS 20u
/* A block decode is attempted the moment the shares already present reach
 * the codec's wake line (tbt_fec_sclc_decode_wake()): every real DATA share,
 * or enough shares to cover the missing ones with the variant's spare-row
 * margin. There is no quiet window: waiting for one let every slow share
 * push the attempt out by 50 ms, so a block that lost only late shares held
 * the delivery frontier while complete later blocks were ready. A failed
 * attempt still backs off (TBT_FEC_DECODE_RETRY_MS doubling to
 * TBT_FEC_DECODE_RETRY_MAX_MS); a new share is new evidence and clears the
 * deadline so the next pass retries at once. */

/* No carrier TCP is left to deliver a share: a retransmit count at the
 * default tcp_retries2 (15) is the point where TCP itself gives up. */
#define TBT_FEC_TCP_RETX_MAX 15

/* The credit cap in shares for one direction. Both SC-LDPC parent
 * windows are 64 blocks * 256 shares; TBT_FEC_S is the wire slot, so the
 * share cap is the window divided by it. The RLNC path keeps the flat
 * TBT_FEC_CREDIT_MAX.
 *
 * The direction must be the one the credit is for, not the local sender's:
 * on the near end the local TX is the 8x32 uplink while the ACK it grants
 * covers the far's 1x256 downlink. Choosing by fec_sclc_shape.variant there
 * gave the downlink the uplink window and halved its credit. */
static u32 tbt_fec_credit_max_packets_dir(const struct tbt_session *sess,
                                          bool up)
{
    if (!sess->fec_sclc)
        return TBT_FEC_CREDIT_MAX;
    return (up ? TBT_FEC_WINDOW_UP : TBT_FEC_WINDOW_DOWN) / TBT_FEC_S;
}

/* The local sender's cap, used for the TX credit and its own start value. */
static u32 tbt_fec_credit_max_packets(const struct tbt_session *sess)
{
    return tbt_fec_credit_max_packets_dir(
        sess, sess->fec_sclc_shape.variant == TBT_FEC_SCLC_UP);
}

/* Raise an atomic maximum without losing a concurrent larger sample. */
static void tbt_fec_stat_max(atomic64_t *max, u64 v)
{
    u64 cur = atomic64_read(max);

    while (v > cur)
    {
        u64 prev = atomic64_cmpxchg(max, cur, v);

        if (prev == cur)
            break;
        cur = prev;
    }
}

/* True while at least one carrier TCP can still deliver a byte: ESTABLISHED
 * and below the default tcp_retries2. Only when every carrier fails this
 * test may the FEC receive side reset the stream; a merely slow or stalled
 * carrier keeps the repair (NACK, replay, TCP retransmission) alive. */
static bool tbt_fec_any_tcp_alive(struct tbt_session *sess)
{
    int i;

    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);
        struct sock *sk;
        bool alive = false;

        if (!c)
            continue;
        if (!atomic_read(&c->stopping))
        {
            sk = tbt_carrier_sk_get(c);
            if (sk)
            {
                alive = READ_ONCE(sk->sk_state) == TCP_ESTABLISHED &&
                        READ_ONCE(inet_csk(sk)->icsk_retransmits) <
                            TBT_FEC_TCP_RETX_MAX;
                tbt_carrier_sk_put(sk);
            }
        }
        tbt_carrier_put(c);
        if (alive)
            return true;
    }
    return false;
}

/* Both ends derive the same coefficients from the configured parameters. The
 * session nonce is not available on the far side until HELLO, and FEC state is
 * built before that; a fixed base keeps sender and receiver aligned. FEC_CFG
 * can carry a negotiated seed later without changing the 48-byte header. */
static u64 tbt_fec_base_nonce(const struct tbt_session *sess)
{
    (void)sess;
    return 0x5a5a5a5a5a5a5a5aULL;
}

/* Group g uses seed_g = base ^ (g << 32); the block generation then mixes in
 * the same golden ratio as the single-group cut, so two blocks never reuse a
 * coefficient set. Sender and receiver derive this independently. */
static u64 tbt_fec_group_nonce(u64 base, u16 group, u64 block_id)
{
    return base ^ ((u64)group << 32) ^ (block_id * TBT_FEC_NONCE_GOLDEN);
}

/* ------------------------------------------------------------------ *
 * Output queue
 * ------------------------------------------------------------------ */

/* Drop one record reference. The record owns a flow reference for as long as
 * it exists, so a detached flow's planes cannot be freed under an in-flight
 * flush. */
static void tbt_fec_out_put(struct tbt_fec_out *o)
{
    if (!o)
        return;
    if (refcount_dec_and_test(&o->refs))
    {
        tbt_fec_flow_put(o->flow);
        kfree(o);
    }
}

/* Unlink one queued record and drop the queue's reference. Control and replay
 * records are linked in their session lists, so their node is removed here; a
 * DATA record is already out of the FIFO and owns no node. The caller holds
 * fec_out_lock for the list paths, and the drain work owns a DATA record
 * alone. A record already unlinked by a flow purge is left alone, so purge
 * and an in-flight flush can race without a double list_del. */
static void tbt_fec_out_free(struct tbt_session *sess, struct tbt_fec_out *o)
{
    if (!tbt_fec_out_unlink_once(&o->queued))
    {
        /* The queue reference is already gone (a flow purge won the race);
         * the caller still owns whatever reader reference it took. */
        return;
    }
    if (o->control || o->replay)
        list_del(&o->node);
    if (o->replay)
    {
        if (sess->fec_replay_queued)
            sess->fec_replay_queued--;
        if (sess->fec_replay_bytes >= o->len)
            sess->fec_replay_bytes -= o->len;
    }
    /* A replay record owns the replay counters, not the DATA outq: it never
     * entered the DATA FIFO, so decrementing those counters here underflows
     * them. The unsigned high-water test then reads the negative value as a
     * huge positive and blocks the packetizer forever (field: 30 s 64-carrier
     * run stopped at block 6 with outq=-2/-16480 and a full TX ring). */
    else if (o->control)
    {
        if (sess->fec_out_ctl_depth)
            sess->fec_out_ctl_depth--;
        if (sess->fec_out_ctl_bytes >= o->len)
            sess->fec_out_ctl_bytes -= o->len;
    }
    else
    {
        atomic_dec(&sess->fec_outq_queued);
        atomic_sub(o->len, &sess->fec_outq_bytes);
    }
    if (o->owner && o->owner->queued)
        o->owner->queued--;
    tbt_fec_out_put(o);
}

/* Single-producer/single-consumer ring for DATA records. The packetizer owns
 * fec_fifo_tail, the drain work owns fec_fifo_head. Acquire/release pairs
 * publish a slot and its advance without a lock. */
static bool tbt_fec_out_fifo_push(struct tbt_session *sess,
                                  struct tbt_fec_out *o)
{
    u32 tail = sess->fec_fifo_tail;
    u32 head = smp_load_acquire(&sess->fec_fifo_head);

    if (tail - head >= TBT_FEC_OUTQ_FIFO_SIZE)
        return false;
    WRITE_ONCE(sess->fec_fifo[tail & (TBT_FEC_OUTQ_FIFO_SIZE - 1)], o);
    smp_store_release(&sess->fec_fifo_tail, tail + 1);
    return true;
}

static struct tbt_fec_out *tbt_fec_out_fifo_peek(struct tbt_session *sess)
{
    u32 head = sess->fec_fifo_head;
    u32 tail = smp_load_acquire(&sess->fec_fifo_tail);

    if (head == tail)
        return NULL;
    return smp_load_acquire(
        &sess->fec_fifo[head & (TBT_FEC_OUTQ_FIFO_SIZE - 1)]);
}

static void tbt_fec_out_fifo_pop(struct tbt_session *sess)
{
    smp_store_release(&sess->fec_fifo_head, sess->fec_fifo_head + 1);
}

void tbt_fec_out_kick(struct tbt_session *sess)
{
    if (!sess->fec || READ_ONCE(sess->fec_stopping) || !sess->fec_ndrain ||
        !sess->fec_fifo)
        return;
    mod_delayed_work(TBT_WQ, &sess->fec_drain_work, 0);
}

static int tbt_fec_out_push(struct tbt_session *sess, u16 carrier,
                            const u8 *buf, u16 len, bool control, bool replay,
                            u64 stamp_ns, struct tbt_fec_flow *flow)
{
    struct tbt_fec_out *o;
    struct tbt_fec_tx *owner = flow ? flow->tx : NULL;
    s64 queued, bytes;
    bool first = false;

    /* A record is per-flow work. Refuse one whose flow is already dying:
     * purge may have run, and this would otherwise reach a carrier after the
     * flow left the table. A NULL flow is reserved for session-wide records
     * (v7 has none). */
    if (flow && !tbt_fec_flow_live(READ_ONCE(flow->state)))
        return -ESHUTDOWN;
    if (control)
    {
        /* The session control queue is carrier-agnostic; the flush picks an
         * up carrier for each record. */
    }
    else if (replay)
    {
        /* The replay queue is also carrier-agnostic: the record's header
         * names its slot, and the flush picks the least-loaded carrier. */
        carrier = 0;
    }
    else
    {
        if (carrier >= sess->n_car || !sess->fec_fifo)
            return -EINVAL;
    }
    o = kmalloc(struct_size(o, data, len), GFP_ATOMIC);
    if (!o)
        return -ENOMEM;
    refcount_set(&o->refs, 1); /* the queue reference */
    o->queued = false;
    o->flow = flow;
    if (flow && !refcount_inc_not_zero(&flow->refs))
    {
        /* The caller raced the final put. Do not publish a record that
         * outlives its flow. */
        kfree(o);
        return -ESHUTDOWN;
    }
    o->carrier = carrier;
    o->len = len;
    o->control = control ? 1 : 0;
    o->replay = replay ? 1 : 0;
    o->block_id = 0;
    o->sid = 0;
    o->owner = NULL;
    {
        struct tbt_fec_hdr h;

        /* The record is complete here, header included; unpack is a plain
         * byte copy. The block id lets the flush drop a repair the peer has
         * already delivered. */
        if (!tbt_fec_hdr_unpack(buf, &h))
        {
            o->block_id = h.block_id;
            o->sid = h.sid;
        }
    }
    o->stamp_ns = stamp_ns;
    memcpy(o->data, buf, len);
    spin_lock(&sess->fec_out_lock);
    if (READ_ONCE(sess->closing) || READ_ONCE(sess->fec_stopping) ||
        !sess->fec ||
        (flow && !tbt_fec_flow_live(READ_ONCE(flow->state))))
    {
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        return -ESHUTDOWN;
    }
    if (replay &&
        sess->fec_replay_bytes + len >
            tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                               TBT_FEC_REPLAY_BYTES_MAX))
    {
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        return -ENOSPC;
    }
    /* These are the hard caps, not the normal stop: the packetizer pauses at
     * TBT_FEC_OUTQ_QUEUE_HIGH_* and pushes the pressure into the capture ring,
     * so a record reaches this branch only if the pause was bypassed (a
     * parity burst larger than the margin, or a shape switch). The replay
     * ring still holds the share and a NACK repairs it, but every hit is a
     * multi-second repair, so it stays counted and visible. */
    queued = atomic_read(&sess->fec_outq_queued);
    bytes = atomic_read(&sess->fec_outq_bytes);
    if (!control && !replay &&
        (queued >= (s64)TBT_FEC_OUTQ_QUEUE_MAX ||
         bytes + (s64)len > (s64)TBT_FEC_OUTQ_QUEUE_BYTES_MAX))
    {
        spin_unlock(&sess->fec_out_lock);
        tbt_fec_out_put(o);
        atomic64_inc(&sess->fec_outq_drop);
        return -ENOSPC;
    }
    if (replay)
    {
        sess->fec_replay_queued++;
        sess->fec_replay_bytes += len;
        if (sess->fec_replay_bytes >
            atomic64_read(&sess->fec_replay_queue_high))
            atomic64_set(&sess->fec_replay_queue_high,
                         sess->fec_replay_bytes);
    }
    if (control)
    {
        list_add(&o->node, &sess->fec_out_ctl);
        sess->fec_out_ctl_depth++;
        sess->fec_out_ctl_bytes += len;
    }
    else if (replay)
    {
        struct tbt_fec_out *pos;
        bool placed = false;

        /* Frontier first: a later super-block's repair must not sit ahead
         * of the block that holds the peer's ACK frontier. fix24's far had
         * an 8 MiB replay queue and the frontier's repair waited 6.13 s, so
         * the TX stall timer reset the stream. Within one block the NACK
         * order is kept. */
        list_for_each_entry(pos, &sess->fec_out_replay, node)
        {
            if (pos->block_id > o->block_id)
            {
                list_add_tail(&o->node, &pos->node);
                placed = true;
                break;
            }
        }
        if (!placed)
            list_add_tail(&o->node, &sess->fec_out_replay);
    }
    else
    {
        u32 depth;

        /* The FIFO turns non-empty exactly when head meets tail. */
        first = smp_load_acquire(&sess->fec_fifo_head) ==
                sess->fec_fifo_tail;
        o->owner = owner;
        if (owner)
            owner->queued++;
        /* Publish queued and the counters before the FIFO release-store:
         * the lockless drain may pop the record immediately. */
        o->queued = true;
        atomic_inc(&sess->fec_outq_queued);
        atomic_add(len, &sess->fec_outq_bytes);
        if (!tbt_fec_out_fifo_push(sess, o))
        {
            o->queued = false;
            atomic_dec(&sess->fec_outq_queued);
            atomic_sub(len, &sess->fec_outq_bytes);
            if (owner)
                owner->queued--;
            spin_unlock(&sess->fec_out_lock);
            tbt_fec_out_put(o);
            atomic64_inc(&sess->fec_outq_drop);
            return -ENOSPC;
        }
        depth = (u32)atomic_read(&sess->fec_outq_queued);
        if (depth > sess->fec_outq_depth_max)
            sess->fec_outq_depth_max = depth;
    }
    if (control || replay)
        o->queued = true;
    spin_unlock(&sess->fec_out_lock);
    if (!control && !replay)
    {
        /* The drain re-arms itself while the FIFO has records. Waking it only
         * on the empty->non-empty edge keeps a burst from re-queueing the
         * work once per record; a 2 ms retry is already armed by the drain
         * when every socket was full. */
        if (first)
            tbt_fec_out_kick(sess);
    }
    else
    {
        /* Control and replay are session-wide: the TX work picks a carrier. */
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    }
    return 0;
}

/* No usable carrier: ask the session's replacement path for one and keep the
 * record queued. The 2 ms flush arm retries; a source share is never dropped
 * here. The first wait in an episode is counted once, and the source ring is
 * the backpressure of last resort. */
static void tbt_fec_wait_carrier(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    if (tx && !tx->carrier_wait)
    {
        tx->carrier_wait = true;
        tx->carrier_wait_since_ns = ktime_get_ns();
        tx->backpressure_counted = false;
        atomic64_inc(&sess->fec_wait_carrier);
    }
    if (!READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_REPLACE, 0);
}

static u16 tbt_fec_replay_usable(struct tbt_session *sess, u8 *usable);
static u16 tbt_fec_replay_pick(struct tbt_session *sess, const u8 *usable,
                               const u32 *pace);

/* Bytes this carrier still holds in its TCP send queue. The repair flush
 * appends to that FIFO, so this is the wire delay a replay pays there. A
 * hint only: the socket may change before the write, and the write's own
 * wspace check still decides. */
static u32 tbt_fec_car_backlog(struct tbt_session *sess, u16 index)
{
    struct tbt_carrier *c = tbt_carrier_get(sess, (u8)index);
    struct sock *sk;
    u32 queued = 0;

    if (!c)
        return 0;
    sk = tbt_carrier_sk_get(c);
    if (sk)
    {
        queued = (u32)READ_ONCE(sk->sk_wmem_queued);
        tbt_carrier_sk_put(sk);
    }
    tbt_carrier_put(c);
    return queued;
}

/* Write one replay record. The carrier is not part of the record's identity:
 * start at the emptiest usable queue, then rotate so one full socket cannot
 * hold the repair. `usable` is a per-flush snapshot; every candidate is
 * rechecked under its reference before the write. */
static int tbt_fec_out_try_replay(struct tbt_session *sess,
                                  struct tbt_fec_out *o, bool *sent,
                                  const u8 *usable, u16 ncand, u32 *pace)
{
    struct tbt_fec_tx *tx = o->flow ? o->flow->tx : NULL;
    u16 start, i;
    int rc = -ENODEV;

    start = ncand ? tbt_fec_replay_pick(sess, usable, pace) : U16_MAX;
    if (start == U16_MAX)
    {
        tbt_fec_wait_carrier(sess, tx);
        return -EAGAIN;
    }
    for (i = 0; i < sess->n_car; i++)
    {
        u16 idx = (u16)((start + i) % sess->n_car);
        struct tbt_carrier *c;
        int err;

        if (!usable[idx])
            continue;
        c = tbt_carrier_get(sess, (u8)idx);
        if (!c)
            continue;
        if (!atomic_read(&c->up) || atomic_read(&c->stopping) ||
            !atomic_read(&c->connected) || READ_ONCE(c->stalled))
        {
            tbt_carrier_put(c);
            continue;
        }
        err = tbt_carrier_send_fec(c, o->data, o->len);
        tbt_carrier_put(c);
        if (!err)
        {
            if (tx)
            {
                tx->carrier_wait = false;
                tx->backpressure_counted = false;
            }
            if (sent)
                *sent = true;
            pace[idx] += o->len;
            return 0;
        }
        if (err == -EAGAIN)
        {
            rc = -EAGAIN;
            continue;
        }
        if (err == -ENOTCONN || err == -ENODEV)
            continue;
        rc = err;
    }
    if (rc == -ENODEV)
    {
        tbt_fec_wait_carrier(sess, tx);
        return -EAGAIN;
    }
    return rc;
}

/* Try to write one record. `sent` is true only when a carrier accepted bytes;
 * a deliberate drop (debug loss, a stalled share) and -EAGAIN both leave it
 * false, which keeps the NACK->wire latency honest. */
static int tbt_fec_out_try(struct tbt_session *sess, struct tbt_fec_out *o,
                           bool *sent, const u8 *usable, u16 ncand,
                           u32 *pace)
{
    struct tbt_carrier *c;
    int i;

    if (sent)
        *sent = false;
    /* The flow may have detached after this record was queued. Drop it
     * instead of putting a dead sid on the wire; the receiver would discard
     * it as an unknown flow. */
    if (o->flow && !tbt_fec_flow_live(READ_ONCE(o->flow->state)))
        return 0;
    if (o->replay)
        return tbt_fec_out_try_replay(sess, o, sent, usable, ncand, pace);
    if (o->control)
    {
        /* Control records (ACK and NACK) may ride any up carrier. Start after
         * the carrier that last accepted one and try every up carrier:
         * pinning the queue to the lowest up carrier lets one congested
         * socket hold every ACK behind its data, and a late ACK is a stalled
         * credit window. */
        struct tbt_fec_tx *tx = o->flow ? o->flow->tx : NULL;
        u16 start = tx ? tx->ctl_rot : 0;
        u16 fallback = 0xffff;
        int rc = -ENODEV;
        int err;

        for (i = 0; i < sess->n_car; i++)
        {
            u16 idx = (u16)((start + i) % sess->n_car);

            c = tbt_carrier_get(sess, (u8)idx);
            if (!c)
                continue;
            if (!atomic_read(&c->up) || atomic_read(&c->stopping))
            {
                tbt_carrier_put(c);
                continue;
            }
            if (READ_ONCE(c->stalled))
            {
                /* A stalled carrier sends no new record, ACKs included. */
                tbt_carrier_put(c);
                continue;
            }
            {
                struct sock *sk = tbt_carrier_sk_get(c);
                u32 notsent;

                if (!sk || sk_stream_wspace(sk) < (int)o->len)
                {
                    if (sk)
                        tbt_carrier_sk_put(sk);
                    tbt_carrier_put(c);
                    continue;
                }
                notsent = READ_ONCE(tcp_sk(sk)->write_seq) -
                          READ_ONCE(tcp_sk(sk)->snd_nxt);
                tbt_carrier_sk_put(sk);
                if (notsent > 32768u)
                {
                    if (fallback == 0xffff)
                        fallback = idx;
                    tbt_carrier_put(c);
                    continue;
                }
            }
            err = tbt_carrier_send_fec(c, o->data, o->len);
            tbt_carrier_put(c);
            if (!err)
            {
                if (tx)
                {
                    tx->ctl_rot = (u16)((idx + 1) % sess->n_car);
                    tx->carrier_wait = false;
                    tx->backpressure_counted = false;
                }
                if (sent)
                    *sent = true;
                return 0;
            }
            /* A dead socket must not end the scan; a full one must not stop
             * it either, because the next carrier may have room. A full
             * carrier anywhere keeps the record queued even if every later
             * one is dead: -EAGAIN wins over -ENODEV. */
            if (err == -EAGAIN)
                rc = -EAGAIN;
        }
        /* No light carrier: use the first one with socket space, without a
         * notsent test. A CFG/FBK must not wait behind 192 KiB of DATA. */
        if (fallback != 0xffff)
        {
            struct sock *sk;

            c = tbt_carrier_get(sess, (u8)fallback);
            if (c)
            {
                sk = tbt_carrier_sk_get(c);
                if (sk && sk_stream_wspace(sk) >= (int)o->len)
                {
                    tbt_carrier_sk_put(sk);
                    err = tbt_carrier_send_fec(c, o->data, o->len);
                    tbt_carrier_put(c);
                    if (!err)
                    {
                        if (tx)
                        {
                            tx->ctl_rot =
                                (u16)((fallback + 1) % sess->n_car);
                            tx->carrier_wait = false;
                            tx->backpressure_counted = false;
                        }
                        if (sent)
                            *sent = true;
                        return 0;
                    }
                }
                else
                {
                    if (sk)
                        tbt_carrier_sk_put(sk);
                    tbt_carrier_put(c);
                }
            }
        }
        /* No usable carrier. Keep the record queued and ask the replacement
         * path for a carrier: the receiver sends an ACK only when delivery
         * advances, and delivery cannot advance again until this ACK reopens
         * the sender's credit window. The flush re-arms in 2 ms, so the
         * record goes out when a carrier recovers or a replacement appears. */
        if (rc == -ENODEV)
        {
            tbt_fec_wait_carrier(sess, tx);
            rc = -EAGAIN;
        }
        return rc;
    }
    /* DATA records never reach this function: the session FIFO drain owns
     * them, and a record there is bound to no carrier. */
    return -EINVAL;
}

/* Record the replay bytes one flush pass wrote and publish a decaying
 * bytes/s gauge. The flush runs under pump_lock, so the accumulators have a
 * single writer; /proc reads them with READ_ONCE. */
static void tbt_fec_replay_rate_sample(struct tbt_session *sess, u64 bytes)
{
    u64 now = ktime_get_ns();
    u64 dt;

    sess->fec_replay_drain_bytes += bytes;
    if (!sess->fec_replay_drain_ns)
    {
        sess->fec_replay_drain_ns = now;
        return;
    }
    dt = now - sess->fec_replay_drain_ns;
    if (dt < 100000000ULL) /* one sample per 100 ms */
        return;
    {
        u64 rate = sess->fec_replay_drain_bytes * 1000000000ULL / dt;
        u64 old = atomic64_read(&sess->fec_replay_drain_bps);

        /* EWMA: a stopped drain decays instead of showing the last burst. */
        atomic64_set(&sess->fec_replay_drain_bps,
                     old ? (old * 7 + rate) / 8 : rate);
    }
    sess->fec_replay_drain_bytes = 0;
    sess->fec_replay_drain_ns = now;
}

/* Remember one wspace probe. A socket with room clears the running-slow
 * state and rejoins the normal scan; a socket without room keeps the first
 * refusal time and turns slow after TBT_SKIP_MS. Only the drain work writes
 * these fields, so no lock is needed; /proc reads them with READ_ONCE. */
static void tbt_fec_out_slow_note(struct tbt_carrier *c, bool room)
{
    if (room)
    {
        WRITE_ONCE(c->slow, false);
        c->slow_since = 0;
        return;
    }
    if (!c->slow_since)
        c->slow_since = jiffies ? jiffies : 1;
    else if (time_after(jiffies,
                        c->slow_since + msecs_to_jiffies(TBT_SKIP_MS)))
        WRITE_ONCE(c->slow, true);
}

/* Offer one DATA record to the carriers in async_rot order and write it on
 * the first socket whose send space holds it. A refusal costs no wait: the
 * scan moves to the next carrier, and a refusal after the wspace check is
 * the same skip. Round 0 leaves out carriers already known slow; round 1
 * gives them a recovery chance, and room clears the mark. The cursor
 * advances only after a successful write, so a healthy carrier keeps taking
 * records while a slow one is passed over.
 *
 * Returns 0 when the record reached a carrier and -EAGAIN when every socket
 * was full (the caller keeps the record at the FIFO head), or another error
 * from the first carrier that failed for a non-capacity reason. */
static int tbt_fec_out_dispatch(struct tbt_session *sess,
                                struct tbt_fec_out *o)
{
    u16 start = (u16)(atomic_read(&sess->async_rot) % sess->n_car);
    u16 fallback = 0xffff;
    bool skipped_slow = false;
    int round;

    for (round = 0; round < 2; round++)
    {
        u16 idx = start;
        u16 i;

        if (round && !skipped_slow)
            break;
        for (i = 0; i < sess->n_car; i++)
        {
            struct tbt_carrier *c;
            struct sock *sk;
            int err;

            c = tbt_carrier_get(sess, (u8)idx);
            if (!c)
            {
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            if (!atomic_read(&c->up) || atomic_read(&c->stopping) ||
                !atomic_read(&c->connected) || READ_ONCE(c->stalled))
            {
                tbt_carrier_put(c);
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            if (!round && READ_ONCE(c->slow))
            {
                skipped_slow = true;
                tbt_carrier_put(c);
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            if (round && !READ_ONCE(c->slow))
            {
                tbt_carrier_put(c);
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            sk = tbt_carrier_sk_get(c);
            if (!sk)
            {
                tbt_carrier_put(c);
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            if (sk_stream_wspace(sk) < (int)o->len)
            {
                tbt_carrier_sk_put(sk);
                tbt_fec_out_slow_note(c, false);
                tbt_carrier_put(c);
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            {
                u32 notsent = READ_ONCE(tcp_sk(sk)->write_seq) -
                              READ_ONCE(tcp_sk(sk)->snd_nxt);

                if (notsent > TBT_FEC_DISPATCH_LIGHT_NOTSENT)
                {
                    if (fallback == 0xffff)
                        fallback = idx;
                    tbt_carrier_sk_put(sk);
                    tbt_carrier_put(c);
                    if (++idx == sess->n_car)
                        idx = 0;
                    continue;
                }
            }
            tbt_carrier_sk_put(sk);
            tbt_fec_out_slow_note(c, true);
            err = tbt_carrier_send_fec(c, o->data, o->len);
            if (err == -EAGAIN)
                tbt_fec_out_slow_note(c, false);
            tbt_carrier_put(c);
            if (!err)
            {
                atomic_set(&sess->async_rot,
                           (u16)((idx + 1) % sess->n_car));
                return 0;
            }
            if (err == -EAGAIN || err == -ENOTCONN || err == -ENODEV)
            {
                if (++idx == sess->n_car)
                    idx = 0;
                continue;
            }
            return err;
        }
    }
    /* No light carrier: use the first one with socket space, without any
     * notsent test. Rejecting here is what made BBR app-limited. */
    if (fallback != 0xffff)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)fallback);
        struct sock *sk;
        int err;

        if (!c)
            return -EAGAIN;
        if (!atomic_read(&c->up) || atomic_read(&c->stopping) ||
            !atomic_read(&c->connected) || READ_ONCE(c->stalled))
        {
            tbt_carrier_put(c);
            return -EAGAIN;
        }
        sk = tbt_carrier_sk_get(c);
        if (!sk || sk_stream_wspace(sk) < (int)o->len)
        {
            if (sk)
                tbt_carrier_sk_put(sk);
            tbt_carrier_put(c);
            return -EAGAIN;
        }
        {
            u32 notsent = READ_ONCE(tcp_sk(sk)->write_seq) -
                          READ_ONCE(tcp_sk(sk)->snd_nxt);

            if (notsent > TBT_FEC_DISPATCH_FALLBACK_NOTSENT)
            {
                tbt_carrier_sk_put(sk);
                tbt_carrier_put(c);
                return -EAGAIN;
            }
        }
        tbt_carrier_sk_put(sk);
        tbt_fec_out_slow_note(c, true);
        err = tbt_carrier_send_fec(c, o->data, o->len);
        if (err == -EAGAIN)
            tbt_fec_out_slow_note(c, false);
        tbt_carrier_put(c);
        if (!err)
        {
            atomic_set(&sess->async_rot,
                       (u16)((fallback + 1) % sess->n_car));
            return 0;
        }
        if (err == -EAGAIN || err == -ENOTCONN || err == -ENODEV)
            return -EAGAIN;
        return err;
    }
    return -EAGAIN;
}

/* Drain the session FIFO. A record is offered to every carrier from
 * async_rot; a full socket is skipped without waiting and the scan moves on.
 * No carrier owns a queue, so a slow socket cannot hold a record behind it:
 * when every socket is full the record stays at the FIFO head and the work
 * retries in 2 ms. A run writes at most TBT_FEC_DRAIN_PASS_MAX records, then
 * re-arms at once while the FIFO has more. */
static void tbt_fec_out_drain_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, fec_drain_work);
    u16 served = 0;

    if (!sess->fec || !sess->fec_fifo || !sess->n_car ||
        READ_ONCE(sess->fec_stopping))
        return;
    while (served < TBT_FEC_DRAIN_PASS_MAX)
    {
        struct tbt_fec_out *o = tbt_fec_out_fifo_peek(sess);
        int err;

        if (!o)
            break;
        /* A record whose flow detached while it waited must not put a dead
         * sid on the wire. The purge leaves it in the FIFO; this is the one
         * consumer that can drop it safely. */
        if (o->flow && !tbt_fec_flow_live(READ_ONCE(o->flow->state)))
        {
            tbt_fec_out_fifo_pop(sess);
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, o);
            spin_unlock(&sess->fec_out_lock);
            continue;
        }
        /* The injector is keyed by the share's scheduled carrier: it models
         * a share handed to a carrier that then died, so it must not eat a
         * record that merely used another carrier. The sender reports
         * success and frees the record; only the codec can recover it. */
        {
            u32 drop = READ_ONCE(sess->fec_drop_ppm[o->carrier]);

            if (drop && get_random_u32() % TBT_DROP_PPM_MAX < drop)
            {
                pr_warn_once("tbtun: dropping FEC records: debug_loss=1 and drop_ppm=%u\n",
                             drop);
                sess->dropped_by_ppm++;
                tbt_fec_out_fifo_pop(sess);
                spin_lock(&sess->fec_out_lock);
                tbt_fec_out_free(sess, o);
                spin_unlock(&sess->fec_out_lock);
                continue;
            }
        }
        err = tbt_fec_out_dispatch(sess, o);
        if (err == -EAGAIN)
        {
            /* Every socket is full: zero wait was paid, and the session
             * retries the head in 2 ms. */
            mod_delayed_work(TBT_WQ, &sess->fec_drain_work,
                             msecs_to_jiffies(2));
            return;
        }
        tbt_fec_out_fifo_pop(sess);
        spin_lock(&sess->fec_out_lock);
        tbt_fec_out_free(sess, o);
        spin_unlock(&sess->fec_out_lock);
        if (err)
        {
            /* A dead socket or a framing error: do not spin on it. */
            mod_delayed_work(TBT_WQ, &sess->fec_drain_work,
                             msecs_to_jiffies(2));
            return;
        }
        atomic64_inc(&sess->fec_flush_pass);
        served++;
    }
    if (served)
        /* Close the producer/consumer race: a push that saw the FIFO
         * non-empty does not kick, and this run may have consumed that
         * record before the push stored its tail. One extra zero-delay run
         * confirms an empty FIFO. */
        mod_delayed_work(TBT_WQ, &sess->fec_drain_work, 0);
}

/* Write the session-wide queues that can make progress: control (ACK/NACK,
 * any up carrier) and replay (any up carrier, paced by queue bytes). The
 * DATA FIFO drains in the session drain work, so a full socket here never
 * holds an ACK, a NACK, a repair, or the packetizer. The session pump lock
 * still serialises these writes with the old control path
 * (HELLO/OPEN/FIN/PAD), so a record is never spliced into another record's
 * bytes on one carrier. */
static int tbt_fec_out_flush(struct tbt_session *sess, u32 *written)
{
    int ret = 0;
    u32 pass = 0;
    u32 replay_sent = 0;
    u64 replay_bytes = 0;
    bool replay_more = false;
    u16 i;

    if (written)
        *written = 0;
    mutex_lock(&sess->pump_lock);
    /* Pass 1: control. */
    if (READ_ONCE(sess->fec_out_ctl_depth))
    {
        for (;;)
        {
            struct tbt_fec_out *o;
            bool sent = false;
            int err;

            spin_lock(&sess->fec_out_lock);
            o = list_first_entry_or_null(&sess->fec_out_ctl,
                                         struct tbt_fec_out, node);
            if (o)
                refcount_inc(&o->refs);
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            err = tbt_fec_out_try(sess, o, &sent, NULL, 0, NULL);
            if (err == -EAGAIN)
            {
                ret = -EAGAIN;
                tbt_fec_out_put(o);
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, o);
            spin_unlock(&sess->fec_out_lock);
            tbt_fec_out_put(o);
            if (sent)
                pass++;
        }
    }
    /* Pass 2: replay. The usable set is built once; every write still
     * rechecks its carrier, so a socket that dies mid-pass is skipped. */
    if (READ_ONCE(sess->fec_replay_queued))
    {
        u8 *usable = sess->fec_replay_usable;
        u32 *pace = sess->fec_replay_pace;
        u16 ncand = tbt_fec_replay_usable(sess, usable);
        u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                     TBT_FEC_REPLAY_BYTES_MAX);
        u32 high = tbt_fec_replay_high_water(cap);
        u32 bytes, limit;
        bool over;

        /* Start from each carrier's TCP send backlog. A repair is appended
         * to that FIFO, so the bytes already waiting are the delay it pays
         * before it reaches the wire. The replay queue reads 0 while the
         * record sits behind megabytes of DATA in one socket: the field run
         * saw fec_nack_repair_max_ns 120.8 s with fec_replay_queued 0. Pick
         * the least-loaded usable carrier, then spread this pass's writes on
         * top of it. */
        for (i = 0; i < sess->n_car; i++)
            pace[i] = usable[i] ? tbt_fec_car_backlog(sess, i) : 0;
        spin_lock(&sess->fec_out_lock);
        bytes = sess->fec_replay_bytes;
        spin_unlock(&sess->fec_out_lock);
        over = bytes > high;
        if (over != READ_ONCE(sess->fec_replay_over_high))
        {
            WRITE_ONCE(sess->fec_replay_over_high, over);
            if (over)
                atomic64_inc(&sess->fec_replay_high_water);
        }
        /* Above the mark one replay record per pass keeps new DATA moving;
         * replay still drains, and the TX work is re-armed immediately. */
        limit = tbt_fec_replay_pass_limit(over, TBT_FEC_REPLAY_PASS_MAX);
        if (over)
            atomic64_inc(&sess->fec_replay_pass_throttled);
        for (;;)
        {
            struct tbt_fec_out *o;
            bool sent = false;
            u64 stamp_ns = 0;
            u64 block_id = 0;
            u16 olen = 0;
            int err;

            spin_lock(&sess->fec_out_lock);
            o = list_first_entry_or_null(&sess->fec_out_replay,
                                         struct tbt_fec_out, node);
            if (o)
                refcount_inc(&o->refs);
            stamp_ns = o ? o->stamp_ns : 0;
            block_id = o ? o->block_id : 0;
            olen = o ? o->len : 0;
            spin_unlock(&sess->fec_out_lock);
            if (!o)
                break;
            if (block_id && o->flow && o->flow->tx)
            {
                struct tbt_fec_tx *rtx = o->flow->tx;
                bool stale = tbt_fec_block_delivered_base(
                    block_id, READ_ONCE(rtx->peer_pkts), rtx->base_pkt,
                    rtx->base_block, rtx->groups, rtx->n);

                if (stale)
                {
                /* The delivered frontier passed this block while the repair
                 * waited in the queue: every share in it is a duplicate of
                 * data the peer already has. Free it instead of writing it,
                 * so the tail repair and the FIN are not stuck behind it. */
                    spin_lock(&sess->fec_out_lock);
                    tbt_fec_out_free(sess, o);
                    spin_unlock(&sess->fec_out_lock);
                    tbt_fec_out_put(o);
                    atomic64_inc(&sess->fec_replay_stale);
                    continue;
                }
            }
            err = tbt_fec_out_try(sess, o, &sent, usable, ncand, pace);
            if (err == -EAGAIN)
            {
                /* No carrier has room for this repair now. It keeps its
                 * place; the bound data queues still drain below. */
                atomic64_inc(&sess->fec_replay_skip);
                ret = -EAGAIN;
                tbt_fec_out_put(o);
                break;
            }
            spin_lock(&sess->fec_out_lock);
            tbt_fec_out_free(sess, o);
            spin_unlock(&sess->fec_out_lock);
            tbt_fec_out_put(o);
            if (sent)
            {
                replay_bytes += olen;
                /* Charge NACK->wire, not NACK->enqueue: the repair is
                 * complete only when a carrier accepted the record. */
                u64 now = ktime_get_ns();
                u64 dt = now > stamp_ns ? now - stamp_ns : 0;

                atomic64_add(dt, &sess->fec_nack_lat_ns);
                tbt_fec_stat_max(&sess->fec_nack_lat_max_ns, dt);
                pass++;
                if (++replay_sent >= limit)
                {
                    /* Give the packetizer a turn; the TX work is re-armed
                     * below while replay records remain. */
                    replay_more = true;
                    break;
                }
            }
            else
            {
                atomic64_inc(&sess->fec_replay_drop);
            }
        }
    }
    else if (READ_ONCE(sess->fec_replay_over_high))
    {
        /* The queue emptied: the next burst counts as a new episode. */
        WRITE_ONCE(sess->fec_replay_over_high, false);
    }
    tbt_fec_replay_rate_sample(sess, replay_bytes);
    mutex_unlock(&sess->pump_lock);
    if (replay_more && READ_ONCE(sess->fec_replay_queued))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    if (pass)
        atomic64_add(pass, &sess->fec_flush_pass);
    if (written)
        *written = pass;
    return ret;
}

/* Queue one complete record. Control records may ride any up carrier; data
 * records are bound to their share's carrier. Returns 0 when queued. */
static int tbt_fec_send_record_flow(struct tbt_session *sess, u16 carrier,
                                    u8 *buf, u16 len, bool control,
                                    bool replay, struct tbt_fec_flow *flow)
{
    return tbt_fec_out_push(sess, carrier, buf, len, control, replay, 0,
                            flow);
}

static int tbt_fec_send_record(struct tbt_session *sess, u16 carrier, u8 *buf,
                               u16 len, bool control, bool replay,
                               struct tbt_fec_flow *flow)
{
    return tbt_fec_send_record_flow(sess, carrier, buf, len, control, replay,
                                    flow);
}

/* Carriers that may carry a replay record right now. The share header names
 * its slot, so a replay is not bound to the carrier that would carry the
 * source share; the set is built once per flush pass instead of once per
 * record. */
static u16 tbt_fec_replay_usable(struct tbt_session *sess, u8 *usable)
{
    u16 i, count = 0;

    memset(usable, 0, TBT_MAX_CARRIERS_LIMIT);
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (atomic_read(&c->up) && atomic_read(&c->connected) &&
            !atomic_read(&c->stopping) && !READ_ONCE(c->stalled))
        {
            usable[i] = 1;
            count++;
        }
        tbt_carrier_put(c);
    }
    return count;
}

/* The usable carrier with the fewest queued bytes, or U16_MAX when none is
 * usable. `pace` starts as each carrier's bound-queue bytes and grows by the
 * replay bytes already written in this pass, so one NACK's 256 shares spread
 * over the carriers instead of filling the first socket. */
static u16 tbt_fec_replay_pick(struct tbt_session *sess, const u8 *usable,
                               const u32 *pace)
{
    u16 best = U16_MAX;
    u32 best_bytes = U32_MAX;
    u16 i;

    for (i = 0; i < sess->n_car; i++)
    {
        u32 bytes;

        if (!usable[i])
            continue;
        bytes = pace[i];
        if (bytes < best_bytes)
        {
            best_bytes = bytes;
            best = i;
        }
    }
    return best;
}

/* Queue one replay record on the session replay queue. The carrier is chosen
 * at write time by queue bytes; `nack_ns` is when the NACK arrived and the
 * flush charges NACK->wire when a carrier accepts the record. */
static int tbt_fec_replay_enqueue(struct tbt_session *sess, const u8 *buf,
                                  u16 len, u64 nack_ns,
                                  struct tbt_fec_flow *flow)
{
    int err = tbt_fec_out_push(sess, 0, buf, len, false, true, nack_ns,
                               flow);

    if (!err)
        WRITE_ONCE(sess->fec_repair_last_ns, ktime_get_ns());
    return err;
}

/* Replay bytes the 8 MiB queue can still take. The NACK handler checks this
 * before queueing a record, so a full queue stops the reply at a clean point
 * instead of letting tbt_fec_out_push() refuse it with -ENOSPC: the receiver
 * retries after its hold, and no share is counted as an allocation failure. */
static u32 tbt_fec_replay_space(struct tbt_session *sess)
{
    u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                 TBT_FEC_REPLAY_BYTES_MAX);
    u32 space;

    spin_lock(&sess->fec_out_lock);
    space = sess->fec_replay_bytes < cap ? cap - sess->fec_replay_bytes : 0;
    spin_unlock(&sess->fec_out_lock);
    return space;
}

static int tbt_fec_make_record(struct tbt_session *sess, struct tbt_fec_hdr *h,
                               const u8 *payload, u8 *out, u16 *out_len)
{
    if (tbt_fec_hdr_seal(h, payload))
        return -EINVAL;
    if (tbt_fec_hdr_pack(h, out))
        return -EINVAL;
    if (h->payload_len)
        memcpy(out + TBT_FEC_HDR_LEN, payload, h->payload_len);
    /* The payload CRC above covers the clear bytes (T0 §2.2). Only DATA and
     * PARITY are transformed; the 48-byte header stays clear. The TX work is
     * serialized and only runs once the TX context is ready. */
    if (sess->crypto &&
        (h->type == TBT_FEC_DATA || h->type == TBT_FEC_PARITY))
    {
        if (!sess->crypto_ctx.tx.ready)
            return -EINVAL;
        if (tbt_crypto_body_xor_flow(&sess->crypto_ctx.tx, h->sid, h->seq,
                                     h->type == TBT_FEC_PARITY,
                                     h->share_index, out + TBT_FEC_HDR_LEN,
                                     h->payload_len))
            return -EINVAL;
    }
    *out_len = (u16)(TBT_FEC_HDR_LEN + h->payload_len);
    return 0;
}

/* ------------------------------------------------------------------ *
 * TX
 * ------------------------------------------------------------------ */

static u32 tbt_fec_ring_put(struct tbt_fec_tx *tx, struct sk_buff *skb,
                            unsigned int off, u32 len)
{
    u32 free = tx->ring_size - (tx->head - tx->tail);
    u32 take = min(len, free);
    u32 first = min(take, tx->ring_size - (tx->head % tx->ring_size));
    u32 pos = tx->head % tx->ring_size;

    if (!take)
        return 0;
    if (skb_copy_bits(skb, off, tx->ring + pos, first))
        return 0;
    if (take > first &&
        skb_copy_bits(skb, off + first, tx->ring, take - first))
        return 0;
    tx->head += take;
    return take;
}

static u32 tbt_fec_ring_get(struct tbt_fec_tx *tx, u8 *dst, u32 len)
{
    u32 avail = tx->head - tx->tail;
    u32 take = min(avail, len);
    u32 first = min(take, tx->ring_size - (tx->tail % tx->ring_size));
    u32 pos = tx->tail % tx->ring_size;

    if (!take)
        return 0;
    memcpy(dst, tx->ring + pos, first);
    if (take > first)
        memcpy(dst + first, tx->ring, take - first);
    tx->tail += take;
    return take;
}

/* Encode one group block and send the parity shares the codec needs.
 *
 * win_len is the number of real DATA packets in the block: n for a full
 * block, less for a block the short-write path closed early. The unused
 * positions are already zero-filled. A full block sends all m rows; a short
 * SC-LDPC block sends min(m, win_len) rows, the first of which states
 * win_len/epoch so the receiver stops waiting for shares the sender never
 * wrote. The rows cover shares 0..win_len-1; the coupled graph may still
 * need a NACK replay for a loss (see tbt_fec_tx_parity_count()). */
/* Account a replay ensure failure. -ENOSPC means the slot still holds an
 * unacked block; -ENOMEM is a real allocation failure. A -ENOSPC that the
 * exact slot test calls reclaimable is a race, counted separately. */
static void tbt_fec_replay_account(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx, u64 block_id,
                                   int er)
{
    if (!er)
        return;
    if (er == -ENOSPC)
    {
        if (tbt_fec_replay_reclaimable(tx->replay, block_id,
                                       READ_ONCE(tx->peer_pkts),
                                       &tx->replay_lock))
            atomic64_inc(&sess->fec_replay_false_enospc);
        atomic64_inc(&sess->fec_replay_miss_enospc);
    }
    else
        atomic64_inc(&sess->fec_replay_miss_alloc);
}

static void tbt_fec_tx_emit_parity(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx, u16 group,
                                   u64 block_id, u16 win_len, bool final)
{
    struct tbt_fec_gtx *g = &tx->g[group];
    struct tbt_fec_hdr h;
    bool block_code = tbt_fec_grouped_mode(tx->codec) == 3;
    u16 count, rec_len, i;
    int st;
    int err;

    if (!tx->codec || !win_len || win_len > tx->blk_n)
        return;
    if (win_len >= tx->blk_n)
        atomic64_inc(&sess->fec_tx_block_complete);
    else if (final)
        /* The stream's own last block: a short tail here is the intended
         * close, not a block the packetizer failed to fill. */
        atomic64_inc(&sess->fec_tx_block_final);
    else
        atomic64_inc(&sess->fec_tx_block_partial);
    count = tbt_fec_tx_parity_count(win_len, tx->blk_n, tx->blk_m,
                                    block_code);
    if (!count)
        return;
    tbt_fec_grouped_set_nonce(tx->codec, group,
                              tbt_fec_group_nonce(tx->nonce, group, block_id));
    /* The block's own n/m place the parity plane: a block may use a split
     * other than the session default, and the buffer is (n+m)*S wide. */
    for (i = 0; i < tx->blk_n; i++)
        tx->data_ptrs[i] = g->buf + (size_t)i * TBT_FEC_S;
    for (i = 0; i < tx->blk_m; i++)
        tx->parity_ptrs[i] = g->buf + (size_t)(tx->blk_n + i) * TBT_FEC_S;
    if (tbt_fec_grouped_encode(tx->codec, group, tx->data_ptrs, tx->blk_n,
                               tx->parity_ptrs) < 0)
    {
        /* No parity means the peer can only repair this block by NACKing the
         * shares themselves; name it instead of failing silently. */
        atomic64_inc(&sess->fec_encode_fail);
        pr_warn_ratelimited("tbtun: fec parity encode failed block=%llu group=%u n=%u m=%u mode=%u\n",
                            block_id, group, tx->blk_n, tx->blk_m,
                            tbt_fec_grouped_mode(tx->codec));
        return;
    }
    if (tx->replay)
        tbt_fec_replay_account(sess, tx, block_id,
                               tbt_fec_replay_ensure_block(
                                   tx->replay, block_id, tx->blk_n,
                                   tx->blk_m, tx->blk_seq0,
                                   READ_ONCE(tx->peer_pkts),
                                   &tx->replay_lock));
    for (i = 0; i < count; i++)
    {
        memset(&h, 0, sizeof(h));
        h.type = TBT_FEC_PARITY;
        h.share_index = (u16)(tx->blk_n + i);
        h.n = (u8)tx->blk_n;
        h.m = (u8)tx->blk_m;
        h.block_id = block_id;
        /* PARITY names the block's first packet; the DATA shares around it
         * carry block_seq0 + group*n + share. */
        h.seq = tx->blk_seq0;
        h.win_len = win_len;
        h.payload_len = TBT_FEC_S;
        h.epoch = (u16)(g->last_len && g->last_len < TBT_FEC_S ? g->last_len : 0);
        h.conv = TBT_FEC_CONV;
        h.sid = tx->sid;
        h.reserved = group;
        if (tbt_fec_make_record(sess, &h, tx->parity_ptrs[i], tx->rec, &rec_len))
            continue;
        tbt_fec_bitmap_set(tx->g[group].sent, (u16)(tx->blk_n + i));
        WRITE_ONCE(tx->last_group, group);
        WRITE_ONCE(tx->last_share, (u16)(tx->blk_n + i));
        st = -EINVAL;
        if (tx->replay)
        {
            spin_lock_bh(&tx->replay_lock);
            st = tbt_fec_replay_store(tx->replay, block_id, group,
                                      (u16)(tx->blk_n + i), tx->rec, rec_len);
            spin_unlock_bh(&tx->replay_lock);
        }
        err = tbt_fec_send_record_flow(
            sess,
            (u16)(tx->group_map[group] * (tx->blk_n + tx->blk_m) +
                  tx->blk_n + i),
            tx->rec, rec_len, false, false, tx->flow);
        if (debug >= 3)
            pr_info("tbtun: fec parity tx block=%llu group=%u parity=%u win_len=%u epoch=%u store=%d send=%d\n",
                    block_id, group, i, win_len, h.epoch, st, err);
        if (err)
        {
            /* The ring keeps the parity; the peer's NACK probe can replay it.
             * Re-arm so a transient queue failure is not silent. */
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
        }
    }
    if (count < tx->blk_m)
        atomic64_add((u64)(tx->blk_m - count), &sess->fec_short_parity_saved);
}

/* One PARITY record that says "this group has no packet in this block".
 * Without it the receiver waits forever for a block that was never sent: a
 * group with no real share emits no parity, and the super-block walk requires
 * every group's block to be ready. The marker carries no payload, so crypto
 * has nothing to transform and the record is header-only. */
static void tbt_fec_tx_emit_empty(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx, u16 group,
                                  u64 block_id)
{
    struct tbt_fec_hdr h;
    u16 rec_len;
    int err;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = tx->blk_n; /* parity 0 */
    h.n = (u8)tx->blk_n;
    h.m = (u8)tx->blk_m;
    h.block_id = block_id;
    h.seq = tx->blk_seq0;
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    h.reserved = group;
    if (tbt_fec_make_record(sess, &h, NULL, tx->rec, &rec_len))
        return;
    tbt_fec_bitmap_set(tx->g[group].sent, tx->blk_n);
    WRITE_ONCE(tx->last_group, group);
    WRITE_ONCE(tx->last_share, tx->blk_n);
    /* A marker is not stored in the replay ring: it is a statement about a
     * block, not a share of it, and replaying an old marker for a block the
     * receiver has already settled was one source of the duplicate-marker
     * protocol errors. A NACK for share n gets a fresh marker from
     * tbt_fec_send_empty_marker() instead. */
    if (debug >= 3)
        pr_info("tbtun: fec empty tx block=%llu group=%u share=%u\n",
                block_id, group, tx->blk_n);
    /* A marker is control-like: it must ride any up carrier. The parity-0
     * carrier may be stalled or full, and the data path would drop the
     * marker as a stalled share. */
    err = tbt_fec_send_record(sess, 0, tx->rec, rec_len, true, false,
                              tx->flow);
    if (err)
    {
        if (debug >= 3)
            pr_info("tbtun: fec empty tx block=%llu group=%u send=%d; retry\n",
                    block_id, group, err);
        /* The ring keeps the marker; the peer's NACK probe can replay it.
         * Re-arm the TX work so a transient queue failure gets another
         * pass instead of staying silent. */
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
    }
    atomic64_inc(&sess->fec_empty_block);
}

/* End the block under construction. Every group that wrote packets into it
 * gets parity over its zero-filled tail with win_len = its real packet count,
 * which is how the receiver learns the block is short; the parity also states
 * the last real share's length in `epoch`, so a receiver that has to recover
 * that share still knows how long it was.
 *
 * A group with no packet in this block emits an empty-block marker instead of
 * silence, so the super-block walk can complete. A group whose block already
 * filled emitted its parity when the last share was queued: nreal == 0 and
 * blk_id == block_id is that state, not "empty".
 *
 * `advance` moves the packet numbering to the first packet of the next block,
 * for a block closed before it filled. The jump is block-aligned: every group
 * starts the next block at share 0, which keeps `nreal = share + 1` and the
 * parity's last_seq valid. The old jump kept `seq % groups` and landed in the
 * middle of a block, so a group before the landing point skipped share 0
 * while packetize() still counted the skipped slots as real packets. */
static void tbt_fec_tx_close_block(struct tbt_session *sess,
                                   struct tbt_fec_tx *tx, bool advance,
                                   bool final)
{
    u64 block_id = tx->cur_block;
    u16 group;

    /* Keep the end of the last real share before the padding jump below.
     * A FIN names this watermark, never the block boundary seq reaches. */
    if (tx->seq > tx->last_real_end)
        tx->last_real_end = tx->seq;
    for (group = 0; group < tx->groups; group++)
    {
        struct tbt_fec_gtx *g = &tx->g[group];
        u16 len = g->nreal;

        if (debug >= 3)
            pr_info("tbtun: fec close block=%llu group=%u gblk=%llu nreal=%u path=%s\n",
                    block_id, group, g->blk_id, len,
                    g->blk_id != block_id ? "empty" :
                    (len && len < tx->blk_n ? "parity" : "skip"));
        if (g->blk_id != block_id)
        {
            tbt_fec_tx_emit_empty(sess, tx, group, block_id);
            continue;
        }
        if (!len || len >= tx->blk_n)
            continue; /* full: parity already sent */
        atomic64_inc(&sess->fec_short_blocks);
        memset(g->buf + (size_t)len * TBT_FEC_S, 0,
               (size_t)(tx->blk_n - len) * TBT_FEC_S);
        tbt_fec_tx_emit_parity(sess, tx, group, block_id, len, final);
        g->nreal = 0;
    }
    if (advance)
    {
        /* Jump to the first packet of the next block. Every group starts
         * that block at share 0, so the next block's shape is the session
         * default until a scheduler sets blk_n/blk_m again. */
        tx->seq = tx->blk_seq0 + (u64)tx->groups * tx->blk_n;
        tx->cur_block = block_id + 1;
        tx->blk_seq0 = tx->seq;
        tx->blk_n = tx->n;
        tx->blk_m = tx->m;
        tbt_fec_tx_sent_clear(tx);
    }
}

/* Flush the stream: every group whose final block is short gets parity over
 * its zero-filled tail, so a lost real share is still recoverable and the
 * receiver learns the real length from win_len. Groups whose final block is
 * full already sent their parity when the block completed; a group with no
 * packet in the final block sends a marker.
 *
 * pkt_end is frozen before the jump: it names the packet after the stream's
 * last DATA packet, and the FIN body carries it so a receiver whose LAST or
 * parity was lost still knows where the stream ends. */
static void tbt_fec_tx_finish(struct tbt_session *sess,
                              struct tbt_fec_tx *tx, struct tbt_stream *st)
{
    u64 block_start;

    if (READ_ONCE(st->fec_finished))
        return;
    /* A block cut by the idle path before FIN arrived is this stream's final
     * short block, not an under-filled middle block. Move the one count the
     * cut already took so the two counters keep their meaning. */
    if (tx->last_real_end < tx->blk_seq0)
    {
        if (atomic64_read(&sess->fec_tx_block_partial))
        {
            atomic64_dec(&sess->fec_tx_block_partial);
            atomic64_inc(&sess->fec_tx_block_final);
        }
    }
    /* last_real_end, not seq: a block the idle path cut short advanced seq
     * over padding positions that carry no stream byte. FIN must name the
     * packet after the last real share, so the receiver can derive the last
     * block's tail from it even when every parity row was lost. */
    WRITE_ONCE(st->fec_pkt_end, tx->last_real_end);
    /* close_block() advances tx->seq to the next block even when the last
     * block was already closed by the short-packet path. Calling it again on
     * an empty next block would emit a full set of empty markers for a block
     * that never carried a packet (field: fec_empty_block=63 for a 198-byte
     * request, i.e. 31 group markers plus a second all-empty block). Only
     * close a block that actually received a packet. */
    block_start = tx->blk_seq0;
    if (st->fec_pkt_base_set && tx->seq > block_start)
        tbt_fec_tx_close_block(sess, tx, true, true);
    smp_store_release(&st->fec_finished, true);
    /* The close path's tbt_tx_fin() retry is driven by w_fbk. If the tick
     * slept while the session had no stream, wake it now that the final block
     * is finished and the FIN can be named. */
    tbt_fbk_kick(sess, false);
}

/* The replay ring is the largest per-flow object: on the 256-carrier shape K
 * lands on 7 and the record plane alone is ~14.1 MiB. Only a sender needs it.
 * Allocating it with the first DATA packet keeps a receive-only session from
 * reserving a repair window it will never read. The packetizer runs in
 * w_fec_tx (process context), so vzalloc() may sleep here. Failure is not
 * fatal: the caller stops and retries, preserving the old "no repair ring,
 * no DATA" contract instead of quietly losing repair ability. */
static bool tbt_fec_tx_replay_ensure(struct tbt_session *sess,
                                     struct tbt_fec_tx *tx)
{
    struct tbt_fec_replay *r;

    if (READ_ONCE(tx->replay))
        return true;
    r = tbt_fec_replay_new_k(tx->groups, tx->n, tx->m, TBT_FEC_S,
                             sess->fec_replay_k);
    if (!r)
    {
        atomic64_inc(&sess->fec_replay_miss_no_ring);
        return false;
    }
    spin_lock_bh(&tx->replay_lock);
    if (tx->replay)
    {
        spin_unlock_bh(&tx->replay_lock);
        tbt_fec_replay_free(r);
        return true;
    }
    WRITE_ONCE(tx->replay, r);
    spin_unlock_bh(&tx->replay_lock);
    return true;
}

static void tbt_fec_packetize(struct tbt_session *sess, struct tbt_fec_tx *tx,
                              struct tbt_stream *st, bool last)
{
    struct tbt_fec_hdr h;
    u16 rec_len, group, share, len, carrier;
    u64 seq, block_id, rel;
    u8 *rec = tx->rec;

    if (!tx->fill && !last)
        return;
    /* Runtime guard for the 0.8 credit contract: a DATA packet must never
     * leave while it is more than the granted window past the peer's packet
     * frontier. The gate should make this unreachable; the counter and the
     * debug line make a regression visible instead of silent. */
    {
        u64 peer = READ_ONCE(tx->peer_pkts);
        u32 credit = READ_ONCE(tx->credit);
        u64 span = (u64)tx->groups * tx->blk_n;
        bool started = span && tx->seq > tx->blk_seq0 &&
                       tx->seq - tx->blk_seq0 < span;

        /* Mid-block sends are exempt: the receiver cannot ACK a partial
         * block, so finishing it is the contract, not a violation. Only a
         * new block that opens past the packet window is counted. */
        {
            u32 per_shares = (u32)tx->groups *
                             ((u32)tx->n + tx->m);
            u32 per_data = (u32)tx->groups * tx->n;
            u64 credit_data = per_shares ?
                (u64)(credit / per_shares) * per_data : 0;

            if (!started && tx->seq > peer &&
                tx->seq - peer >= credit_data + per_data)
        {
            atomic64_inc(&sess->fec_credit_violation);
            if (debug)
                pr_info_ratelimited("tbtun: fec credit violation seq=%llu peer=%llu credit=%u n=%u groups=%u\n",
                                    tx->seq, peer, credit, tx->n,
                                    tx->groups);
            }
        }
    }
    len = tx->fill;
    /* The block under construction owns a fixed shape. Its packets are laid
     * group-major: seq = blk_seq0 + group*blk_n + share. A block that is
     * already full advances to the next one before this packet is placed. */
    if (!tx->blk_n || !tx->blk_m)
    {
        tx->blk_n = tx->n;
        tx->blk_m = tx->m;
    }
    seq = tx->seq;
    if (seq < tx->blk_seq0)
        tx->blk_seq0 = seq;
    rel = seq - tx->blk_seq0;
    while (rel >= (u64)tx->groups * tx->blk_n)
    {
        tx->blk_seq0 += (u64)tx->groups * tx->blk_n;
        tx->cur_block++;
        tx->blk_n = tx->n;
        tx->blk_m = tx->m;
        rel = seq - tx->blk_seq0;
    }
    tx->seq++;
    /* The stream's packet end follows real shares only. A block closed
     * short jumps over padding slots; FIN must not inherit that jump. */
    tx->last_real_end = tx->seq;
    group = (u16)(rel / tx->blk_n);
    share = (u16)(rel % tx->blk_n);
    block_id = tx->cur_block;
    carrier = (u16)(tx->group_map[group] * (tx->blk_n + tx->blk_m) + share);

    /* The first packet of a stream fixes its absolute base; the FIN body
     * carries it so the peer can turn its global packet frontier into a
     * stream-relative byte offset. A mux block has no single stream, so the
     * caller passes st == NULL and the subframe FIN carries the end. */
    if (st && !st->fec_pkt_base_set)
    {
        WRITE_ONCE(st->fec_pkt_base, seq);
        /* Publish the value before the flag: the ACK path reads the flag with
         * acquire and then the base, so it never sees a set flag with a stale
         * zero base. */
        smp_store_release(&st->fec_pkt_base_set, true);
    }
    /* A group's nreal/last_len belong to one block. Entering a new block
     * resets them; a share that is not the next one means the sender skipped
     * a slot (the early-close jump used to do exactly that), and the parity
     * would then describe a hole. */
    if (tx->g[group].blk_id != block_id)
    {
        tx->g[group].blk_id = block_id;
        tx->g[group].nreal = 0;
        tx->g[group].last_len = 0;
        memset(tx->g[group].sent, 0, sizeof(tx->g[group].sent));
    }
    if (share != tx->g[group].nreal)
        atomic64_inc(&sess->fec_tx_share_gap);

    tx->short_at_ns = 0;
    tx->block_at_ns = 0;
    if (len < TBT_FEC_S)
    {
        atomic64_inc(&sess->fec_short_tx);
        if (debug >= 2)
            pr_info("tbtun: fec short tx len=%u seq=%llu block=%llu group=%u share=%u base=%llu last=%d\n",
                    len, seq, block_id, group, share,
                    st ? READ_ONCE(st->fec_pkt_base) : 0, last);
    }
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = last ? TBT_FEC_FF_LAST : 0;
    /* 0.8.0 constant frame: a short slot goes on the wire as a full S-byte
     * body, zero-filled after the real bytes, and win_len names the real
     * length. The receiver decrypts and CRC-checks the whole slot, then
     * delivers only win_len bytes. */
    if (len < TBT_FEC_S)
    {
        h.flags |= TBT_FEC_FF_PADDED;
        h.win_len = len;
        h.payload_len = TBT_FEC_S;
        memset(tx->pkt + len, 0, TBT_FEC_S - len);
    }
    else
    {
        h.win_len = 0;
        h.payload_len = len;
    }
    h.share_index = share;
    h.n = (u8)tx->blk_n;
    h.m = (u8)tx->blk_m;
    if (debug >= 2 && share == 0)
        pr_info("tbtun: fec tx hdr block=%llu n=%u m=%u want_m=%u tx_n=%u tx_m=%u blk_n=%u blk_m=%u peer_epoch=%llu\n",
                block_id, h.n, h.m, READ_ONCE(tx->want_m),
                READ_ONCE(tx->n), READ_ONCE(tx->m), tx->blk_n, tx->blk_m,
                (unsigned long long)READ_ONCE(tx->peer_epoch));
    h.block_id = block_id;
    h.seq = seq;
    /* epoch is block-level and belongs to PARITY only: a mid-block short
     * share that carried it overwrote the block's last-share length. */
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    h.reserved = group;
    if (!tbt_fec_make_record(sess, &h, tx->pkt, rec, &rec_len))
    {
        WRITE_ONCE(tx->last_group, group);
        WRITE_ONCE(tx->last_share, share);
        if (tx->replay)
        {
            tbt_fec_replay_account(
                sess, tx, block_id,
                tbt_fec_replay_ensure_block(tx->replay, block_id,
                                            tx->blk_n, tx->blk_m,
                                            tx->blk_seq0,
                                            READ_ONCE(tx->peer_pkts),
                                            &tx->replay_lock));
            spin_lock_bh(&tx->replay_lock);
            tbt_fec_replay_store(tx->replay, block_id, group, share,
                                 rec, rec_len);
            spin_unlock_bh(&tx->replay_lock);
        }
        tbt_fec_bitmap_set(tx->g[group].sent, share);
        (void)tbt_fec_send_record_flow(sess, carrier, rec, rec_len, false,
                                       false, tx->flow);
        tx->data_pkts_sent++;
    }
    /* The block buffer must hold the real byte even when the share did not
     * reach its carrier: parity then describes the truth, and the receiver
     * can recover the dropped share instead of decoding zeros. */
    memcpy(tx->g[group].buf + (size_t)share * TBT_FEC_S, tx->pkt, len);
    if (len < TBT_FEC_S)
        memset(tx->g[group].buf + (size_t)share * TBT_FEC_S + len, 0,
               TBT_FEC_S - len);
    tx->fill = 0;
    tx->cur_block = block_id;
    tx->g[group].nreal = (u16)(share + 1);
    tx->g[group].last_len = len;

    if (share + 1 == tx->blk_n)
    {
        tbt_fec_tx_emit_parity(sess, tx, group, block_id,
                               tx->g[group].nreal, false);
        tx->g[group].nreal = 0;
    }
    if (last && st)
        tbt_fec_tx_finish(sess, tx, st);
}

/* The peer's ACK grants `credit` shares beyond the frontier it has
 * delivered. The receiver buffers `slots * groups * (n+m)` shares ahead of
 * delivery, and the credit is the same number, so a sender that respects
 * it can never outrun the buffer. Before this was enforced the packetizer ran as far ahead as the
 * carriers would carry it, the receiver dropped 1527 shares as "too far ahead"
 * (fec_drop_window), and the blocks they belonged to could never complete: the
 * stream stalled with the application's bytes still in the ring.
 *
 * The window is spent in whole super-blocks. A block closed before it
 * filled advances `seq` to the next block boundary without sending the
 * skipped slots, so a packet distance spends a full block of credit for a
 * one-share block and can close in the middle of the next block. The block
 * gate is the unit the receiver buffers (`slots` blocks) and the replay
 * ring holds (K), so a short cut costs exactly one block and a block the
 * packetizer started can always be finished. See tbt_fec_credit_block_ok(). */
/* True while the packetizer is inside a block it has already started. The
 * credit gate must never stop there: a partial block at the receiver is
 * invisible to its ACK (rx_next waits for the whole block), so a mid-block
 * stop is a deadlock, not backpressure. */
static bool tbt_fec_tx_block_started(const struct tbt_fec_tx *tx)
{
    u64 span = (u64)tx->groups * tx->blk_n;

    return span && tx->seq > tx->blk_seq0 && tx->seq - tx->blk_seq0 < span;
}

static void tbt_fec_tx_sent_clear(struct tbt_fec_tx *tx)
{
    u16 g;

    for (g = 0; g < tx->groups; g++)
        memset(tx->g[g].sent, 0, sizeof(tx->g[g].sent));
}

static bool tbt_fec_tx_credit_ok(const struct tbt_fec_tx *tx)
{
    /* The gate is a block-boundary gate. Once the block has a share, send
     * the whole block; the next block start checks the window again. */
    if (tbt_fec_tx_block_started(tx))
        return true;
    /* Zero credit is a closed window, not an open one. The receiver always
     * grants at least one block, so zero means no grant has arrived (or a
     * bug); sending on it would overrun the receiver's block slots and the
     * shares would come back as fec_drop_window. */
    return !tbt_fec_credit_waiting(READ_ONCE(tx->peer_pkts),
                                   READ_ONCE(tx->seq),
                                   READ_ONCE(tx->credit),
                                   READ_ONCE(tx->groups),
                                   READ_ONCE(tx->n), READ_ONCE(tx->m));
}

/* The session FIFO at or above its high-water mark. The packetizer checks
 * this before it builds the next packet: pausing there pushes the pressure
 * into the capture ring, which pauses the socket once full
 * (tbt_fec_tx_submit), so the sender never drops a share it would have to
 * repair with a NACK round trip. The hard caps in tbt_fec_out_push() remain
 * as the last line of defence. */
static bool tbt_fec_tx_outq_high(struct tbt_session *sess)
{
    s64 queued, bytes;
    bool high = false;

    if (!sess->fec_fifo)
        return false;
    /* Signed: an accounting bug must not turn a negative counter into a
     * huge unsigned high-water hit and stop the packetizer. */
    queued = atomic_read(&sess->fec_outq_queued);
    bytes = atomic_read(&sess->fec_outq_bytes);
    if (queued >= (s64)TBT_FEC_OUTQ_QUEUE_HIGH ||
        bytes >= (s64)TBT_FEC_OUTQ_QUEUE_BYTES_HIGH)
        high = true;
    spin_lock(&sess->fec_out_lock);
    if (sess->fec_replay_bytes >=
        tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                           TBT_FEC_REPLAY_BYTES_MAX) / 2)
        /* Half the ring is a repair backlog the receiver is waiting on; more
         * new DATA only competes with the repair for the same sockets. Stop
         * the packetizer until the queue drains below the mark. The flush
         * high-water (75%) still throttles the drain itself. */
        high = true;
    spin_unlock(&sess->fec_out_lock);
    return high;
}

static bool tbt_fec_tx_is_idle(struct tbt_session *sess,
                               struct tbt_fec_tx *tx)
{
    bool pkt_idle;
    u32 queued;

    spin_lock(&tx->lock);
    pkt_idle = tx->head == tx->tail && tx->fill == 0 &&
               !READ_ONCE(tx->flush_req);
    spin_unlock(&tx->lock);
    spin_lock(&sess->fec_out_lock);
    queued = tx->queued;
    spin_unlock(&sess->fec_out_lock);
    /* Only new DATA orders the FIN. Replay records are duplicates of shares
     * already sent once and control records carry no stream bytes, so an
     * 8 MiB replay backlog must not hold the close (fix20: close_timeouts=1
     * with 3.98 MB left). */
    return tbt_fec_fin_ready(pkt_idle, queued);
}

/* Debug-only bitmaps. out must hold 2*TBT_FEC_BITMAP_BYTES+1 bytes. */
static void tbt_fec_diag_bm(char *out, const u8 *src, u16 first, u16 count)
{
    u8 bm[TBT_FEC_BITMAP_BYTES];
    u16 i;

    memset(bm, 0, sizeof(bm));
    for (i = 0; i < count; i++)
        if (tbt_fec_bitmap_test(src, (u16)(first + i)))
            tbt_fec_bitmap_set(bm, i);
    for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
        snprintf(out + (size_t)i * 2, 3, "%02x", bm[i]);
    out[TBT_FEC_BITMAP_BYTES * 2] = 0;
}

static void tbt_fec_diag_hex(char *out, const u8 *bm)
{
    u16 i;

    for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
        snprintf(out + (size_t)i * 2, 3, "%02x", bm[i]);
    out[TBT_FEC_BITMAP_BYTES * 2] = 0;
}

/* The bounded TX reset in tbt_tx.c fires after 30 s without an ACK advance.
 * Acceptance runs debug=0, so its TXSTALLKILL line is invisible; this
 * ratelimited line names the same state from the FEC side before the reset:
 * the ack/txseq gap, the frontier block, the replay backlog and the repair
 * age. It returns without printing while the ack is inside its budget. */
static void tbt_fec_tx_stall_diag(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx,
                                  struct tbt_stream *st)
{
    u64 now = ktime_get_ns();
    u64 last = READ_ONCE(st->last_ack_ns);
    u64 budget = tbt_tx_wait_budget_ns(sess, st);
    u64 tx_pkts = READ_ONCE(tx->seq);
    u64 peer_pkts = READ_ONCE(tx->peer_pkts);
    u16 n = READ_ONCE(tx->n);
    u16 groups = READ_ONCE(tx->groups);
    u32 credit = READ_ONCE(tx->credit);
    u16 m = READ_ONCE(tx->m);
    u32 credit_blocks = tbt_fec_credit_blocks(credit, groups, n, m);
    u64 base_pkt = READ_ONCE(tx->base_pkt);
    u64 base_block = READ_ONCE(tx->base_block);
    u32 ring_bytes = (u32)(READ_ONCE(tx->head) - READ_ONCE(tx->tail)) +
                     READ_ONCE(tx->fill);
    u32 tx_run = atomic_read(&sess->fec_tx_run);
    u32 rq, rb;

    /* An idle stream has nothing outstanding: ack == txseq is not a stall.
     * The near's uplink GET sat at ack=txseq=91 and printed this line for
     * 954 s, drowning the log (fix25). */
    if (READ_ONCE(st->tx_ack) >= READ_ONCE(st->tx_seq))
        return;
    if (!last || now <= last || now - last <= budget)
        return;
    spin_lock(&sess->fec_out_lock);
    rq = sess->fec_replay_queued;
    rb = sess->fec_replay_bytes;
    spin_unlock(&sess->fec_out_lock);
    /* Units matter: st->tx_seq/tx_ack are application bytes, while
     * tx->seq/peer_pkts are FEC DATA packets (S bytes each). The credit gate
     * compares packets, so a byte-count difference is not a packet lead. At
     * S=1024, 9152 "packets" derived from bytes is 9152*1024 bytes of
     * submitted application data, most of it still in the TX ring. */
    pr_info_ratelimited("tbtun: fec TX stall diag ack_bytes=%llu tx_bytes=%llu tx_pkts=%llu peer_pkts=%llu lead_pkts=%lld credit_pkts=%u credit_blocks=%u n=%u groups=%u hdr_n=%u hdr_m=%u want_m=%u peer_epoch=%llu cur_block=%llu base_pkt=%llu base_block=%llu credit_wait=%llu txp=%d txq=%u fec_tx_run=%u sched_tx=%d g=%u s=%u replay_q=%u replay_bytes=%u high=%d replay_throttled=%llu age_us=%llu\n",
                        READ_ONCE(st->tx_ack), READ_ONCE(st->tx_seq),
                        tx_pkts, peer_pkts,
                        (long long)(tx_pkts - peer_pkts), credit,
                        credit_blocks, n, groups,
                        READ_ONCE(tx->blk_n), READ_ONCE(tx->blk_m),
                        READ_ONCE(tx->want_m),
                        (unsigned long long)READ_ONCE(tx->peer_epoch),
                        READ_ONCE(tx->cur_block), base_pkt, base_block,
                        atomic64_read(&sess->fec_credit_wait),
                        READ_ONCE(st->tx_paused) ? 1 : 0, ring_bytes, tx_run,
                        test_bit(TBT_SW_FEC_TX, &sess->sched),
                        READ_ONCE(tx->last_group), READ_ONCE(tx->last_share),
                        rq, rb, READ_ONCE(sess->fec_replay_over_high),
                        atomic64_read(&sess->fec_replay_pass_throttled),
                        (now - last) / NSEC_PER_USEC);
    if (debug >= 1)
    {
        u16 g;

        for (g = 0; g < tx->groups; g++)
        {
            char dh[TBT_FEC_BITMAP_BYTES * 2 + 1];
            char ph[TBT_FEC_BITMAP_BYTES * 2 + 1];

            u8 rbm[TBT_FEC_BITMAP_BYTES];
            char rdh[TBT_FEC_BITMAP_BYTES * 2 + 1];
            char rph[TBT_FEC_BITMAP_BYTES * 2 + 1];
            u16 s;

            tbt_fec_diag_bm(dh, tx->g[g].sent, 0, tx->blk_n);
            tbt_fec_diag_bm(ph, tx->g[g].sent, tx->blk_n, tx->blk_m);
            memset(rbm, 0, sizeof(rbm));
            if (tx->replay)
                for (s = 0; s < (u16)(tx->blk_n + tx->blk_m); s++)
                    if (tbt_fec_replay_has(tx->replay,
                                           READ_ONCE(tx->cur_block), g, s))
                        tbt_fec_bitmap_set(rbm, s);
            tbt_fec_diag_bm(rdh, rbm, 0, tx->blk_n);
            tbt_fec_diag_bm(rph, rbm, tx->blk_n, tx->blk_m);
            pr_info_ratelimited("tbtun: fec TX block bitmap block=%llu group=%u n=%u m=%u seq0=%llu data=%s parity=%s replay_data=%s replay_parity=%s\n",
                                READ_ONCE(tx->cur_block), g,
                                READ_ONCE(tx->blk_n), READ_ONCE(tx->blk_m),
                                READ_ONCE(tx->blk_seq0), dh, ph, rdh, rph);
        }
    }
}

/* Move to the next block when the one under construction is closed: every
 * group has emitted its parity (nreal == 0), no partial packet is buffered,
 * and seq has reached the block's end. The advance uses the old blk_n, so a
 * shape switch can never reinterpret an existing block's packet span. Only
 * w_fec_tx calls this. */
static void tbt_fec_tx_block_advance(struct tbt_fec_tx *tx)
{
    u64 rel;
    u16 group;

    if (tx->fill || !tx->blk_n)
        return;
    for (group = 0; group < tx->groups; group++)
        if (tx->g[group].nreal)
            return;
    if (tx->seq < tx->blk_seq0)
        return;
    rel = tx->seq - tx->blk_seq0;
    while (rel >= (u64)tx->groups * tx->blk_n)
    {
        tx->blk_seq0 += (u64)tx->groups * tx->blk_n;
        tx->cur_block++;
        tx->blk_n = tx->n;
        tx->blk_m = tx->m;
        tbt_fec_tx_sent_clear(tx);
        rel = tx->seq - tx->blk_seq0;
    }
}

/* True while the current block still owns state that a shape switch would
 * reinterpret: a buffered partial packet, any group's real shares, or a full
 * block whose seq/blk_seq0 boundary has not advanced yet. Full blocks reset
 * nreal when their parity leaves, so the nreal loop alone missed the last
 * case and let codec_tick change blk_n before packetize advanced the block.
 * Only w_fec_tx writes this state, and every caller runs there. */
static bool tbt_fec_tx_block_dirty(const struct tbt_fec_tx *tx)
{
    u16 group;

    if (tx->fill)
        return true;
    for (group = 0; group < tx->groups; group++)
        if (tx->g[group].nreal)
            return true;
    if (!tx->blk_n || tx->seq < tx->blk_seq0)
        return false;
    return tx->seq - tx->blk_seq0 >= (u64)tx->groups * tx->blk_n;
}

/* Apply the peer's m_target at a block boundary. The target codec is built
 * here, in w_fec_tx, before the first block of the new split; a dirty block
 * keeps the current n/m until it closes. A build failure keeps the current
 * split and backs off exponentially, so a broken codec cannot become a retry
 * storm. */
static void tbt_fec_tx_codec_tick(struct tbt_session *sess,
                                  struct tbt_fec_tx *tx)
{
    struct tbt_fec_codec_key key;
    struct tbt_fec_codec_entry *e;
    struct tbt_fec_grouped *codec;
    u16 want_m, shares, n, old_m;
    u32 rk;
    bool ready = false;
    u64 now;

    if (!tx)
        return;
    /* Close a full block at its old boundary before any switch. Doing this
     * unconditionally also keeps the pump's dirty test false after a block
     * that just filled, so the unacked and idle-cut gates still see "no open
     * block" rather than a phantom one waiting to be sealed. */
    tbt_fec_tx_block_advance(tx);
    if (!tx->sclc_codec || !tx->codec_cache)
        return;
    want_m = READ_ONCE(tx->want_m);
    shares = (u16)(tx->n + tx->m);
    if (!want_m || want_m >= shares)
        return;
    n = (u16)(shares - want_m);
    if (n == tx->n && want_m == tx->m)
    {
        if (tx->codec_pending)
        {
            tbt_fec_codec_cache_put(tx->codec_pending);
            tx->codec_pending = NULL;
        }
        return;
    }
    if (!tbt_fec_adapt_sclc_buildable(tx->sclc_shape.variant, n, want_m))
        return;
    tbt_fec_codec_key_fill(&key, &tx->sclc_shape, n, want_m, tx->nonce);
    if (tx->codec_pending &&
        (tx->codec_pending->key.n != key.n ||
         tx->codec_pending->key.m != key.m ||
         tx->codec_pending->key.block != key.block ||
         tx->codec_pending->key.L != key.L ||
         tx->codec_pending->key.seed != key.seed))
    {
        tbt_fec_codec_cache_put(tx->codec_pending);
        tx->codec_pending = NULL;
    }
    if (tx->codec_pending)
        e = tx->codec_pending;
    else
    {
        now = ktime_get_ns();
        if (now < tx->codec_next_ns)
            return;
        e = tbt_fec_codec_cache_get(tx->codec_cache, &key, &ready);
        if (!e)
            return;
        if (!ready)
        {
            codec = tbt_fec_codec_build(&tx->sclc_shape, n, want_m,
                                        tx->groups, tx->nonce);
            tbt_fec_codec_cache_build_done(e, codec, codec != NULL);
            if (!codec)
            {
                tx->codec_build_fail++;
                tx->codec_backoff = tx->codec_backoff
                                        ? (u8)min_t(u32,
                                                    tx->codec_backoff * 2u,
                                                    1000u)
                                        : 50u;
                tx->codec_next_ns =
                    now + (u64)tx->codec_backoff * NSEC_PER_MSEC;
                tbt_fec_codec_cache_put(e);
                return;
            }
            tx->codec_backoff = 0;
            tx->codec_next_ns = 0;
        }
        tx->codec_pending = e;
    }
    if (tbt_fec_tx_block_dirty(tx))
        return;
    old_m = tx->m;
    /* The NACK and ACK paths read the shape from carrier RX context. Publish
     * every field they use under the same lock; the packetizer is the only
     * writer, so a NACK either sees the old split or the new one, never a
     * mixture. */
    spin_lock_bh(&tx->shape_lock);
    tx->codec = e->codec;
    tx->n = n;
    tx->m = want_m;
    tx->blk_n = n;
    tx->blk_m = want_m;
    tx->band = tbt_fec_grouped_band(e->codec, 0);
    tx->rate_ppm = (u32)((((u64)n * 1000000u) + shares - 1u) / shares);
    tx->base_pkt = tx->seq;
    tx->base_block = tx->cur_block;
    tbt_fec_tx_sent_clear(tx);
    spin_unlock_bh(&tx->shape_lock);
    rk = tx->replay ? tbt_fec_replay_k(tx->replay) : sess->fec_replay_k;
    tx->credit_cap = tbt_fec_credit_packets(
        tx->groups, n, tx->m, rk, sess->fec_slots,
        tbt_fec_credit_max_packets(sess));
    if (tx->credit > tx->credit_cap)
        tx->credit = tx->credit_cap;
    if (tx->codec_entry)
        tbt_fec_codec_cache_put(tx->codec_entry);
    tx->codec_entry = e;
    tx->codec_pending = NULL;
    atomic64_inc(&sess->fec_switch_done);
    spin_lock(&sess->lock);
    sess->adapt.n = n;
    sess->adapt.m = want_m;
    sess->adapt.groups = tx->groups;
    sess->adapt.class = tbt_fec_adapt_sclc_rate_pct(n, want_m);
    spin_unlock(&sess->lock);
    if (debug >= 2)
        pr_info("tbtun: fec tx codec n=%u m=%u (was m=%u) build_fail=%u\n",
                n, want_m, old_m, tx->codec_build_fail);
}

/* Ask w_fec_cut to run at the earliest live deadline. The bit owns its
 * reference, so an arm while an earlier instance is queued is a no-op; that
 * instance wakes w_fec_tx, which recomputes and arms the remainder. Keeping
 * both deadlines off w_fec_tx leaves its 0 ms submit wake untouched. */
static void tbt_fec_tx_cut_arm(struct tbt_session *sess,
                               const struct tbt_fec_tx *tx)
{
    u64 now = ktime_get_ns();
    u64 deadline = 0;
    unsigned long delay = 0;

    if (tx->short_at_ns && (!deadline || tx->short_at_ns < deadline))
        deadline = tx->short_at_ns;
    if (tx->block_at_ns && (!deadline || tx->block_at_ns < deadline))
        deadline = tx->block_at_ns;
    /* An expired deadline that could not act is waiting for credit, and the
     * ACK that reopens the window arms w_fec_tx itself. Arming a 0 ms cut
     * here would spin the two works against a closed credit window. */
    if (!deadline || deadline <= now)
        return;
    delay = nsecs_to_jiffies(deadline - now);
    if (!delay)
        delay = 1;
    tbt_session_arm(sess, TBT_SW_FEC_CUT, delay);
}

/* Kept out of line: inlining it into the work body doubled that frame, which
 * already holds the 128-entry flow snapshot. */
static noinline void tbt_fec_tx_pump(struct tbt_session *sess,
                                     struct tbt_fec_flow *f)
{
    struct tbt_fec_tx *tx = f->tx;

    WRITE_ONCE(tx->last_pump_ns, ktime_get_ns());
    WRITE_ONCE(tx->last_pump_seq, READ_ONCE(tx->seq));
    if (READ_ONCE(tx->stall_seq) != READ_ONCE(tx->seq))
    {
        WRITE_ONCE(tx->stall_seq, READ_ONCE(tx->seq));
        WRITE_ONCE(tx->stall_ns, ktime_get_ns());
    }
    struct tbt_stream *st;
    bool paused = false;
    bool pulled = false;
    bool fill_ready;
    bool block_dirty;
    u32 pass = 0;

    if (!tx || f->state != 1 || READ_ONCE(sess->closing) ||
        READ_ONCE(sess->fec_stopping))
        return;
    st = tbt_stream_get(sess, f->sid, false, false);
    if (!st)
        return;

    /* SC-LDPC announces its transmit shape with a per-flow INIT CFG, and the
     * receiver installs it from w_fec_rx asynchronously. Pumping shares
     * first makes the peer drop every one of them as a stale shape; the
     * repair burst then looks like channel loss to the brand-new control law
     * and pins the first rate decision low. Hold the packetizer until the
     * peer acknowledged INIT, or the bounded retry gave up. Grouped flows
     * never run the INIT handshake, so they are exempt. */
    if (sess->fec_sclc && sess->n_fec_flows <= 1 && tx->sid &&
        !tx->init_done)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
        tbt_stream_put(st);
        return;
    }

    /* A closing stream caps replay admission at
     * TBT_FEC_REPLAY_CLOSE_BYTES_MAX: the last block's repair must fit the
     * close budget, so an older backlog must not refill the queue. */
    WRITE_ONCE(tx->closing, READ_ONCE(st->close_since_ns) != 0);
    tbt_fec_tx_stall_diag(sess, tx, st);
    for (;;)
    {
        u32 got, k, hard, max_unacked;

        /* A new m_target from the peer applies only at a block boundary and
         * only after its codec is ready; until then the current n/m stays. */
        tbt_fec_tx_codec_tick(sess, tx);
        /* Yield after one burst so the control-law tick, FBK and ACK work
         * get a turn. An unbounded pump can run for the whole transfer and
         * never let tx_adapt_tick switch the shape. */
        if (pass++ >= 64)
        {
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            break;
        }
        long mem;
        u32 unacked;

        /* Smooth step window. Never open a block when the replay ring has
         * k-2 unacked blocks: ensure_block would return -ENOSPC and the
         * share would be sent but never stored. The tier below keeps far's
         * vmalloc flat instead of sawtoothing between 97 and 254 MiB. */
        mem = tbt_mem_avail_kb();
        unacked = tx->replay ?
                  tbt_fec_replay_unacked(tx->replay, &tx->replay_lock) : 0;
        k = tx->replay ? tbt_fec_replay_k(tx->replay) : 0;
        hard = k > 2 ? k - 2 : 1;
        if (mem >= 512L * 1024L)
            max_unacked = 64;
        else if (mem >= 240L * 1024L)
            max_unacked = 20;
        else if (mem >= TBT_FEC_MEM_PAUSE_KB)
            max_unacked = 12;
        else
            max_unacked = 8;
        if (max_unacked > hard)
            max_unacked = hard;
        if (mem < TBT_FEC_MEM_PAUSE_KB && tx->replay)
            tbt_fec_replay_shrink(tx->replay, true, 0, &tx->replay_lock);
        if (unacked >= max_unacked && tx->fill == 0 &&
            !tbt_fec_tx_block_dirty(tx))
        {
            u32 ring_bytes = (u32)(READ_ONCE(tx->head) -
                                   READ_ONCE(tx->tail));

            /* The replay ring is at its soft cap, but the source ring may
             * still hold the bytes the close needs to drain. Breaking here
             * before ring_get left a full ring untouched forever. If the
             * packet window is open, read and send one block; the hard
             * refusal in tbt_fec_out_push() still bounds memory. */
            if (ring_bytes &&
                !tbt_fec_credit_waiting(READ_ONCE(tx->peer_pkts),
                                        READ_ONCE(tx->seq),
                                        READ_ONCE(tx->credit),
                                        READ_ONCE(tx->groups),
                                        READ_ONCE(tx->n),
                                        READ_ONCE(tx->m)))
            {
                atomic64_inc(&sess->fec_unacked_ring_drain);
            }
            else
            {
                tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
                break;
            }
        }
        if (tbt_fec_tx_outq_high(sess))
        {
            if (!tx->outq_paused)
            {
                tx->outq_paused = true;
                atomic64_inc(&sess->fec_outq_high_wait);
            }
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            break;
        }
        tx->outq_paused = false;
        spin_lock(&tx->lock);
        got = tbt_fec_ring_get(tx, tx->pkt + tx->fill, TBT_FEC_S - tx->fill);
        if (got)
        {
            tx->fill += (u16)got;
            pulled = true;
        }
        /* Resume capture at half, not only when the ring is empty. The
         * parent socket's receive window is all the application sees: holding
         * it shut until the last byte drains collapses the sender (field:
         * far loopback BBR 22 kbps, rwnd_limited 34 %, then 2 KB per RTO and
         * one-share blocks for 118 s). tbt_sock.c already documents the half
         * contract; this is the pump that honours it. */
        paused = READ_ONCE(st->tx_paused) &&
                 (u32)(tx->head - tx->tail) <= tx->ring_size / 2;
        spin_unlock(&tx->lock);
        /* Allocate the repair ring only when this flow actually has bytes to
         * send. A receive-only session's TX ring stays empty, so it never
         * reserves the ~14 MiB window. No ring means no DATA: stop and retry
         * rather than send shares a NACK could never recover. */
        if ((got || tx->fill) && !tbt_fec_tx_replay_ensure(sess, tx))
        {
            tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
            break;
        }
        if (got)
        {
            if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
                tbt_fec_packetize(sess, tx, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S && tbt_fec_tx_credit_ok(tx))
        {
            tbt_fec_packetize(sess, tx, st, false);
            continue;
        }
        if (tx->fill == TBT_FEC_S)
        {
            atomic64_inc(&sess->fec_credit_wait);
            if (tbt_fec_tx_block_started(tx))
            {
                atomic64_inc(&sess->fec_credit_midblock_wait);
                if (debug)
                    pr_info_ratelimited("tbtun: fec credit mid-block wait block=%llu seq=%llu base=%llu blk_n=%u g=%u s=%u credit=%u peer=%llu\n",
                                        READ_ONCE(tx->cur_block),
                                        READ_ONCE(tx->seq),
                                        READ_ONCE(tx->blk_seq0),
                                        READ_ONCE(tx->blk_n),
                                        READ_ONCE(tx->last_group),
                                        READ_ONCE(tx->last_share),
                                        READ_ONCE(tx->credit),
                                        READ_ONCE(tx->peer_pkts));
            }
            /* A closed credit window is not stream idle: the block may be
             * partial only because the peer has not granted the next round.
             * Restart both idle clocks, so reopening credit does not seal a
             * short block the moment a deadline that expired during the wait
             * is rechecked. */
            tx->short_at_ns = 0;
            tx->block_at_ns = 0;
            break;
        }
        if (READ_ONCE(tx->flush_req) && tx->head == tx->tail)
        {
            if (tx->fill && !tbt_fec_tx_credit_ok(tx))
                break;
            if (tx->fill)
                tbt_fec_packetize(sess, tx, st, true);
            else
                tbt_fec_tx_finish(sess, tx, st);
            WRITE_ONCE(tx->flush_req, false);
            continue;
        }
        break;
    }
    /* New bytes push both idle deadlines forward. A partial fill is not a
     * pause while TCP is still delivering the same 8 KiB write one segment
     * at a time: at 1 MB/s the gaps between segments can exceed the 2 ms
     * interactive tail, and flushing there would emit a short share and cut
     * the block per segment. The tail deadline is therefore measured from
     * the last byte, not from the last full share. */
    if (pulled)
    {
        tx->short_at_ns = 0;
        tx->block_at_ns = 0;
    }
    /* The interactive tail keeps its 2 ms; a block that merely holds real
     * shares waits for TBT_FEC_BLOCK_IDLE_MS. Sealing on the 2 ms timer was
     * the release blocker: at 1 MB/s one 8 KiB write every 8 ms drains the
     * ring after every share, so the pump cut a one-share block per write
     * (0 ms field run: 123 blocks in 14 s). The receiver then NACKed padding
     * shares the sender never wrote and waited for a cut the next short block
     * kept replacing (400 ms: peer_pkts=144, frontier=9, cur=9, s=8).
     *
     * A full share arrives with `fill == 0`; a partial packet arms the 2 ms
     * tail only on the stream's first packet, where the request must not wait
     * for 8 KiB that never come. Later partials are TCP segmentation of the
     * bulk stream and wait for the same block idle threshold, so they
     * coalesce into a full share. FIN still closes at once. Both deadlines
     * live on w_fec_cut: tbt_session_arm() never retimes a pending item, and
     * a 25 ms timer on w_fec_tx would swallow the next write's 0 ms wake. */
    fill_ready = tx->fill != 0;
    block_dirty = tbt_fec_tx_block_dirty(tx);
    spin_lock(&tx->lock);
    if (tx->head != tx->tail)
        fill_ready = block_dirty = false;
    spin_unlock(&tx->lock);
    if ((fill_ready || block_dirty) && !READ_ONCE(tx->flush_req) &&
        !tbt_fec_tx_outq_high(sess))
    {
        u64 now = ktime_get_ns();
        u64 deadline = 0;

        if (fill_ready)
        {
            u64 wait_ns = (READ_ONCE(st->fec_pkt_base_set)
                               ? TBT_FEC_BLOCK_IDLE_MS
                               : TBT_FEC_SHORT_MS) *
                          NSEC_PER_MSEC;

            if (!tx->short_at_ns)
                tx->short_at_ns = now + wait_ns;
            deadline = tx->short_at_ns;
        }
        else
            tx->short_at_ns = 0;
        if (block_dirty)
        {
            if (!tx->block_at_ns)
                tx->block_at_ns =
                    now + (u64)TBT_FEC_BLOCK_IDLE_MS * NSEC_PER_MSEC;
            if (!deadline || tx->block_at_ns < deadline)
                deadline = tx->block_at_ns;
        }
        else
            tx->block_at_ns = 0;

        if (now >= deadline && tbt_fec_tx_credit_ok(tx))
        {
            /* The block idle deadline ends the block at its last real
             * share. A partial packet that happens to be in front of it is
             * stream bytes, not a share of this block yet: cutting it into
             * a short DATA record gives one extra share for the same bytes,
             * an unaligned final tail, and a block of credit spent on fewer
             * than groups*n shares. Those bytes instead start the next
             * block, where more of the same write usually follows and they
             * coalesce into a full share. */
            bool cut_block = block_dirty && tx->block_at_ns &&
                             now >= tx->block_at_ns;

            if (tx->fill && !cut_block)
            {
                tbt_fec_packetize(sess, tx, st, false);
                tx->short_at_ns = 0;
            }
            /* The partial tail is the interactive cut: seal it now. A block
             * that is merely waiting for its next share keeps the partial
             * packet buffered and gets its own tail deadline below. */
            if (fill_ready || cut_block)
            {
                tbt_fec_tx_close_block(sess, tx, true, false);
                tx->block_at_ns = 0;
                tx->short_at_ns = 0;
                /* The bytes kept above still need the interactive tail:
                 * closing the block is a new quiet interval, not the end
                 * of the stream. Without this arm the pump would sleep
                 * forever after sealing a block mid-write. */
                if (cut_block && tx->fill)
                    tx->short_at_ns =
                        now + (u64)(READ_ONCE(st->fec_pkt_base_set)
                                        ? TBT_FEC_CUT_GRACE_MS
                                        : TBT_FEC_SHORT_MS) *
                                  NSEC_PER_MSEC;
            }
        }
        /* Credit is a closed window, not a lost deadline: the ACK that
         * reopens it is the wake this wait needs. */
        if (tx->short_at_ns || tx->block_at_ns)
            tbt_fec_tx_cut_arm(sess, tx);
    }
    else
    {
        tx->short_at_ns = 0;
        tx->block_at_ns = 0;
    }
    if (paused)
    {
        WRITE_ONCE(st->tx_paused, false);
        tbt_stream_schedule(st, &st->w_read, TBT_STREAM_F_SCHED_READ);
    }
    tbt_stream_put(st);
}


/* Take a reference on the session's primary flow. The pointer is a borrowed
 * view, so a work body that keeps it across an unlock must own a reference:
 * detach can swap it and drop the last table reference at any time. */
static struct tbt_fec_flow *tbt_fec_primary_get(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;

    if (!sess || !sess->fec)
        return NULL;
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !refcount_inc_not_zero(&f->refs))
        f = NULL;
    spin_unlock(&sess->lock);
    return f;
}

/* Snapshot the live flow table with one reference per entry. A flow owns at
 * least one group and every flow has a stream, so TBT_STREAMS_MAX covers
 * every possible entry. The snapshot is taken under sess->lock, which
 * is also where admit, detach and abort link/unlink; processing runs after
 * the unlock with the references held, so a flow can be detached but not
 * freed under the walk. */
static u16 tbt_fec_flow_snapshot(struct tbt_session *sess,
                                 struct tbt_fec_flow **out, u16 max)
{
    struct tbt_fec_flow *f;
    u16 n = 0, b;

    if (!sess || !sess->fec || !out || !max)
        return 0;
    spin_lock(&sess->lock);
    for (b = 0; b < TBT_STREAM_HT && n < max; b++)
        hlist_for_each_entry(f, &sess->fec_flows[b], node)
            if (tbt_fec_flow_live(f->state) && f->sid &&
                refcount_inc_not_zero(&f->refs))
                out[n++] = f;
    spin_unlock(&sess->lock);
    return n;
}

static void tbt_fec_flow_put_all(struct tbt_fec_flow **flows, u16 n)
{
    u16 i;

    for (i = 0; i < n; i++)
        tbt_fec_flow_put(flows[i]);
}

/* Publish the primary compatibility pointers for the RCU readers outside the
 * data plane (tbt_tx.c and tbt_carrier.c). The caller holds sess->lock. */
static void tbt_fec_primary_publish(struct tbt_session *sess,
                                    struct tbt_fec_flow *f)
{
    rcu_assign_pointer(sess->fec_primary, f);
    rcu_assign_pointer(sess->fec_tx, f ? f->tx : NULL);
    rcu_assign_pointer(sess->fec_rx, f ? f->rx : NULL);
}


static void tbt_fec_tx_work_body(struct tbt_session *sess)
{
    struct tbt_fec_flow *primary;

    if (READ_ONCE(sess->closing) || READ_ONCE(sess->fec_stopping))
        return;
    primary = tbt_fec_primary_get(sess);
    if (!primary)
        return;
    /* Control and replay are session-wide and flush here; the bound data
     * queues drain in their per-carrier works, so a full socket never stops
     * the packetizer. */
    (void)tbt_fec_out_flush(sess, NULL);
    /* The initial shape is a session property; with one flow it is also the
     * flow's shape. N>1 freezes CFG/FBK (design §3.8). */
    if (sess->n_fec_flows <= 1)
        tbt_fec_tx_init_tick(sess, primary->tx);

    if (sess->crypto && !smp_load_acquire(&sess->crypto_tx_ready))
    {
        tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(10));
        goto out;
    }

    tbt_fec_tx_pump(sess, primary);
    if (tbt_fec_out_flush(sess, NULL) == -EAGAIN)
        tbt_session_arm(sess, TBT_SW_FEC_TX, msecs_to_jiffies(2));
out:
    tbt_fec_flow_put(primary);
}

/* Send one FEC_CFG. APPLY announces the pending shape; ABORT cancels one the
 * receiver may have armed. Control records are plaintext and ride the
 * control queue, so a dead share carrier does not lose them. */
/* The peer's receive shape is this side's transmit shape, and the wire
 * carries no codec field, so a session whose directions differ (downlink
 * 1x256, uplink 8x32) has to announce it. INIT is not a switch: the receiver
 * installs it before its first DATA and the sender keeps its own codec. It is
 * retried until the peer's CFG_ACK names epoch 1. */
static void tbt_fec_tx_send_init(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    struct tbt_fec_cfg c;
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_CFG_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_CFG_LEN];
    u16 len;

    if (!tx)
        return;
    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = tx->n;
    c.m = tx->m;
    c.w = tx->shape_w;
    c.k = tx->shape_k;
    c.d = tx->shape_d;
    /* INIT installs the peer shape before the first share; it does not
     * apply a mid-stream switch. An APPLY here made the receiver treat the
     * announcement as a stale switch body. */
    c.flags = TBT_FEC_CFG_F_INIT;
    c.groups = tx->groups;
    c.switch_pkt = 0;
    c.block_base = 0;
    c.seed = tx->nonce;
    c.codec_variant = tx->codec_variant;
    c.L = tx->sclc_L;
    c.block = tx->sclc_block;
    c.rate_ppm = tx->rate_ppm;
    if (tbt_fec_cfg_pack(&c, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_CFG;
    h.payload_len = TBT_FEC_CFG_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    (void)tbt_fec_send_record(sess, 0, rec, len, true, false, tx->flow);
}


/* Pace the announcement: first pass immediately, then every 500 ms, at most
 * six times. A peer that never acks keeps the two-sided shape it configured
 * itself; the mismatch then shows as a shape drop, not as silence. */
static void tbt_fec_tx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_tx *tx)
{
    u64 now = ktime_get_ns();

    if (!tx || tx->init_done || !tx->sid)
        return;
    if (tx->peer_est_valid && tx->peer_epoch == 1 &&
        (tx->peer_flags & TBT_FEC_FBK_F_CFG_ACK))
    {
        tx->init_done = true;
        return;
    }
    if (tx->peer_est_valid && tx->peer_epoch == 1 &&
        (tx->peer_flags & TBT_FEC_FBK_F_CFG_NACK))
    {
        /* The peer cannot build this shape. Retrying cannot help; keep the
         * two-sided configured shape and name the mismatch once. */
        tx->init_done = true;
        pr_warn_ratelimited("tbtun: tunnel %llu: peer refused the INIT shape sid=%llu n=%u m=%u groups=%u w=%u k=%u d=%u\n",
                            (unsigned long long)sess->tunnel->cfg.id,
                            (unsigned long long)tx->sid, tx->n, tx->m,
                            tx->groups, tx->shape_w, tx->shape_k,
                            tx->shape_d);
        return;
    }
    if (tx->init_tries >= 6)
    {
        tx->init_done = true;
        return;
    }
    if (tx->init_tries && now - tx->init_ns < 500000000ULL)
    {
        tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(500));
        return;
    }
    tbt_fec_tx_send_init(sess, tx);
    tx->init_tries++;
    tx->init_ns = now;
    tbt_session_arm(sess, TBT_SW_FEC_TICK, msecs_to_jiffies(500));
}

/* Drop the queued DATA-side records that belong to `f`. The DATA FIFO has
 * one consumer and is never walked here; its records carry the block's own
 * n/m, and the drain drops those whose flow is dead. Only this flow's
 * replay records are shape-bound and freed now, so aborting one stream
 * cannot discard another stream's FIFO DATA. */
static void tbt_fec_tx_clear_data_queues(struct tbt_session *sess,
                                         struct tbt_fec_flow *f)
{
    struct tbt_fec_out *o, *tmp;

    if (!sess || !f)
        return;
    spin_lock(&sess->fec_out_lock);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
        if (o->flow == f)
            tbt_fec_out_free(sess, o);
    spin_unlock(&sess->fec_out_lock);
    tbt_fec_out_kick(sess);
}

/* Release a finished stream's FEC planes before a new accept is judged
 * against the memory floor. Keeps two planes per ring. Queued records are
 * purged by their own flow's detach/abort, never by a session-wide sweep:
 * a live stream keeps its FIFO DATA and repairs. Runs in process context. */
void tbt_fec_mem_shrink(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;

    if (!sess)
        return;
    f = tbt_fec_primary_get(sess);
    if (!f)
        return;
    if (f->tx && READ_ONCE(f->tx->replay))
        tbt_fec_replay_shrink(f->tx->replay, false, 2,
                              &f->tx->replay_lock);
    if (f->rx)
        tbt_fec_rx_shrink(f->rx, true);
    tbt_fec_flow_put(f);
}

static void tbt_fec_tick_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tick);
    bool owned = test_and_clear_bit(TBT_SW_FEC_TICK, &sess->sched);

    if (!READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    if (owned)
        tbt_session_put(sess);
}

/* Idle deadlines for the packetizer's current block. The entry owns no state:
 * it asks w_fec_tx for one pass, which either cuts the block or recomputes
 * the remaining delay. A dedicated bit is required because a 25 ms deadline
 * queued on w_fec_tx would hold the next write's 0 ms submit wake behind it
 * (tbt_session_arm() never retimes a pending instance). */
static void tbt_fec_cut_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_cut);
    bool owned = test_and_clear_bit(TBT_SW_FEC_CUT, &sess->sched);

    if (!READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    if (owned)
        tbt_session_put(sess);
}

void tbt_fec_tx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_tx);
    bool owned = test_and_clear_bit(TBT_SW_FEC_TX, &sess->sched);

    /* The run word lives on the session, not on a flow's TX plane: detach can
     * swap the primary while a body is running, and two bodies over the same
     * flow would corrupt its packetizer. */
    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &sess->fec_tx_run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &sess->fec_tx_run);
        goto out;
    }
    {
        /* Stamp the work entry, not the pump entry: a body stuck before
         * tbt_fec_tx_pump() must still be visible to the close-tick
         * watchdog. */
        struct tbt_fec_tx *tx;

        rcu_read_lock();
        tx = rcu_dereference(sess->fec_tx);
        if (tx)
            WRITE_ONCE(tx->last_pump_ns, ktime_get_ns());
        rcu_read_unlock();
    }
    tbt_fec_tx_work_body(sess);
    if (!(atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &sess->fec_tx_run) &
          (1u << TBT_FEC_RUN_BIT)))
        atomic64_inc(&sess->fec_tx_run_leak);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &sess->fec_tx_run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
out:
    if (owned)
        tbt_session_put(sess);
}

size_t tbt_fec_tx_submit(struct tbt_stream *st, struct sk_buff *skb,
                         unsigned int off, size_t len)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_tx *tx = st->fec_flow ? st->fec_flow->tx : NULL;
    u32 take;

    if (!tx)
        return 0;
    spin_lock(&tx->lock);

    take = tbt_fec_ring_put(tx, skb, off, (u32)min_t(size_t, len, U32_MAX));
    if (take)
    {
        /* The FIN path freezes tx_seq after the ring is drained; keeping the
         * accepted byte count here is what makes that FIN name the real end
         * of the stream and lets the ACK clamp stay exact. */
        WRITE_ONCE(st->tx_seq, READ_ONCE(st->tx_seq) + take);
    }
    if (take < len)
    {
        WRITE_ONCE(st->tx_paused, true);
        /* The source ring is full while the sender waits for a carrier:
         * this is the last-resort backpressure, not a dropped share. */
        if (READ_ONCE(tx->carrier_wait) &&
            !READ_ONCE(tx->backpressure_counted))
        {
            WRITE_ONCE(tx->backpressure_counted, true);
            atomic64_inc(&sess->fec_backpressure);
        }
    }
    spin_unlock(&tx->lock);
    if (take)
        atomic64_add(take, &sess->tx_submit_bytes);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return take;
}

bool tbt_fec_tx_idle(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_flow *f = st->fec_flow;
    bool idle;

    if (!f || !f->tx)
        return true;
    idle = tbt_fec_tx_is_idle(sess, f->tx);

    /* The FIN may proceed while replay records are still queued; count how
     * often the old all-queues gate would have held it. */
    if (idle && READ_ONCE(sess->fec_replay_queued))
        atomic64_inc(&sess->fec_fin_wait_replay);
    return idle;
}

int tbt_fec_tx_flush(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    struct tbt_fec_flow *f = st->fec_flow;
    struct tbt_fec_tx *tx = f ? f->tx : NULL;

    if (!tx)
        return -ENODEV;
    if (READ_ONCE(sess->fec_stopping))
        return -ESHUTDOWN;
    /* The packetizer is owned by tbt_fec_tx_work; a flush from the stream
     * close path only raises a flag, so two CPUs never touch tx->pkt. */
    WRITE_ONCE(tx->flush_req, true);
    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    return 0;
}

/* ------------------------------------------------------------------ *
 * RX
 * ------------------------------------------------------------------ */

int tbt_fec_record_len(const u8 *buf, size_t have, size_t *need)
{
    u16 len;

    if (have < TBT_FEC_HDR_LEN)
        return TBT_REC_NEED_MORE;
    len = (u16)(buf[TBT_FEC_H_PAYLEN] | ((u16)buf[TBT_FEC_H_PAYLEN + 1] << 8));
    if (len > TBT_FEC_PAYLOAD_MAX)
        return TBT_REC_PROTO;
    *need = TBT_FEC_HDR_LEN + len;
    /* Defence in depth: the parser buffer must hold the whole record, or it
     * would fill up and never complete (p->have == sizeof(p->buf)). */
    if (*need > TBT_PARSER_BUF)
        return TBT_REC_PROTO;
    return have < *need ? TBT_REC_NEED_MORE : TBT_REC_OK;
}

/* Lazy block buffers. The free list lives on the first word of each buffer;
 * push and pop run under rx->lock, while allocation and freeing run in
 * process context outside it. */
static void tbt_fec_rx_buf_push_locked(struct tbt_fec_rx *rx, u8 *buf)
{
    if (!buf)
        return;
    *(u8 **)buf = rx->buf_pool;
    rx->buf_pool = buf;
    rx->buf_pool_n++;
}

static u8 *tbt_fec_rx_buf_pop_locked(struct tbt_fec_rx *rx)
{
    u8 *buf = rx->buf_pool;

    if (!buf)
        return NULL;
    rx->buf_pool = *(u8 **)buf;
    rx->buf_pool_n--;
    return buf;
}

static void tbt_fec_rx_buf_free_list(u8 *buf)
{
    while (buf)
    {
        u8 *next = *(u8 **)buf;

        kvfree(buf);
        buf = next;
    }
}

/* A DATA/PARITY record is about to take rx->lock. If the free list is empty,
 * allocate one block buffer now, in process context, so the placement path
 * never allocates under the leaf lock. The record's own block may already
 * have a buffer; the spare then stays pooled for the next new block. */
static void tbt_fec_rx_buf_prepare(struct tbt_fec_rx *rx)
{
    u32 bytes;
    u8 *buf;

    /* One refill at a time: a 256-record burst must not transiently
     * allocate one block buffer per record while only the pool winner
     * keeps it. */
    mutex_lock(&rx->buf_alloc_lock);
    spin_lock_bh(&rx->lock);
    bytes = rx->buf_bytes;
    if (rx->buf_pool || !bytes)
    {
        spin_unlock_bh(&rx->lock);
        mutex_unlock(&rx->buf_alloc_lock);
        return;
    }
    spin_unlock_bh(&rx->lock);
    if (tbt_mem_avail_kb() < TBT_FEC_MEM_PAUSE_KB)
        tbt_fec_rx_shrink(rx, false);
    if (tbt_mem_avail_kb() < TBT_FEC_MEM_HARD_KB)
    {
        mutex_unlock(&rx->buf_alloc_lock);
        return;
    }
    buf = tbt_fec_vzalloc(bytes);
    if (!buf)
    {
        mutex_unlock(&rx->buf_alloc_lock);
        return;
    }
    spin_lock_bh(&rx->lock);
    if (rx->buf_bytes == bytes && !rx->buf_pool)
    {
        tbt_fec_rx_buf_push_locked(rx, buf);
        buf = NULL;
    }
    spin_unlock_bh(&rx->lock);
    kvfree(buf);
    mutex_unlock(&rx->buf_alloc_lock);
}

/* Return a buffer to the capped hot pool. Overflow is linked through the
 * buffer itself and freed by the caller after it drops rx->lock. */
static void tbt_fec_rx_buf_shed(struct tbt_fec_rx *rx, u8 *buf, u8 **overflow)
{
    if (!buf)
        return;
    if (rx->buf_pool_n + 1 > TBT_FEC_RX_POOL_BLOCKS)
    {
        *(u8 **)buf = *overflow;
        *overflow = buf;
        return;
    }
    tbt_fec_rx_buf_push_locked(rx, buf);
}

/* Re-slice the shape-invariant buffer for the current n/m. A switch keeps the
 * allocation and moves only these two pointers. A lazy slot with no buffer
 * yet has neither pointer. */
static void tbt_fec_blk_slice(const struct tbt_fec_rx *rx,
                              struct tbt_fec_blk *b)
{
    if (!b->buf)
    {
        b->data = NULL;
        b->parity = NULL;
        return;
    }
    b->data = b->buf;
    b->parity = b->buf + (size_t)b->n * rx->s;
}

static void tbt_fec_blk_reset(struct tbt_session *sess,
                              struct tbt_fec_flow *f, struct tbt_fec_blk *b,
                              u64 block_id, u64 seq0, u16 n, u16 m)
{
    struct tbt_fec_rx *rx = f->rx;

    /* A block that never became ready is dropped with its slot: its DATA
     * shares are not evidence for p (they would understate it), but a high
     * abandoned rate freezes the controller. Carrier RX counts here under
     * rx->lock; w_fec_rx folds the delta into the estimator. */
    if (rx && b->used && !b->ready && !b->empty && !b->judged)
        atomic64_add(b->nreal ? b->nreal : b->n, &rx->abandoned_shares);
    if (b->codec_entry)
    {
        tbt_fec_codec_cache_put(b->codec_entry);
        b->codec_entry = NULL;
    }
    b->gen++;
    b->used = true;
    b->ready = false;
    b->seq0 = seq0;
    b->n = n;
    b->m = m;
    b->nreal = 0;
    b->ndata = 0;
    b->nparity = 0;
    b->count_real = 0xffff;
    b->decoding = false;
    b->decode_tries = 0;
    b->decode_next_ns = 0;
    b->empty = false;
    b->win_len_set = false;
    b->short_len = 0;
    b->short_unknown = false;
    b->first_gap_ns = 0;
    b->wake_ns = 0;
    b->wake_ndata = 0;
    b->wake_nparity = 0;
    b->wake_real = 0;
    b->wake_first_ok = 0;
    b->wake_attempts = 0;
    b->wake_sampled = 0;
    b->miss_mask = 0;
    memset(b->miss_bits, 0, sizeof(b->miss_bits));
    memset(b->codec_fail_bits, 0, sizeof(b->codec_fail_bits));
    b->judged = false;
    b->block_id = block_id;
    b->sid = 0;
    memset(b->dpresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->ppresent, 0, TBT_FEC_MAX_SHARES);
    memset(b->dlen, 0, TBT_FEC_MAX_SHARES * sizeof(u16));
    tbt_fec_blk_slice(rx, b);
}

/* Release block buffers on a stream reset or under memory pressure. reset
 * frees every active and pooled plane except two; the low-memory form frees
 * only the idle pool. It also clears the NACK request/stamp state so a new
 * sid cannot inherit an old stream's req_block entries. */
void tbt_fec_rx_shrink(struct tbt_fec_rx *rx, bool reset)
{
    u8 *free_list = NULL, *buf;
    u32 i, total, keep;

    if (!rx)
        return;
    total = (u32)rx->groups * rx->slots;
    keep = reset ? 2 : 0;
    spin_lock_bh(&rx->lock);
    if (reset)
        memset(&rx->late, 0, sizeof(rx->late));
    if (reset && rx->blk)
    {
        for (i = 0; i < total; i++)
        {
            struct tbt_fec_blk *b = &rx->blk[i];

            /* The decoder reads b->data/b->parity outside rx->lock. The
             * block owns its plane until its own re-lock path clears
             * decoding and frees it. */
            if (b->decoding)
                continue;
            if (b->codec_entry)
            {
                tbt_fec_codec_cache_put(b->codec_entry);
                b->codec_entry = NULL;
            }
            if (!b->buf)
            {
                b->used = false;
                b->ready = false;
                b->block_id = ~0ULL;
                continue;
            }
            *(u8 **)b->buf = free_list;
            free_list = b->buf;
            b->buf = NULL;
            b->data = NULL;
            b->parity = NULL;
            b->used = false;
            b->ready = false;
            b->decoding = false;
            b->block_id = ~0ULL;
        }
        if (rx->req_block)
            memset(rx->req_block, 0xff,
                   (size_t)total * sizeof(*rx->req_block));
        if (rx->req_ns)
            memset(rx->req_ns, 0,
                   (size_t)total * rx->shares * sizeof(*rx->req_ns));
        if (rx->nack)
            memset(rx->nack, 0, sizeof(*rx->nack));
    }
    while ((buf = tbt_fec_rx_buf_pop_locked(rx)) != NULL)
    {
        *(u8 **)buf = free_list;
        free_list = buf;
    }
    while (keep > 0 && free_list)
    {
        buf = free_list;
        free_list = *(u8 **)free_list;
        tbt_fec_rx_buf_push_locked(rx, buf);
        keep--;
    }
    spin_unlock_bh(&rx->lock);
    while (free_list)
    {
        buf = free_list;
        free_list = *(u8 **)free_list;
        kvfree(buf);
    }
}

/* Real length of this block, or 0 when the group has no packet in it. The
 * parity's win_len is authoritative for the block; the stream's total packet
 * count is the second source, used when every parity record of this block was
 * lost. A known value never grows: an inconsistent peer cannot turn padding
 * back into data. */
static u16 tbt_fec_blk_real_len(const struct tbt_fec_rx *rx, u16 group,
                                const struct tbt_fec_blk *b)
{
    bool has_data = false;
    u16 i;

    /* A marker says the group has no packet in this block; nothing in it is
     * ever delivered, whatever the stream-length fallback would say. */
    if (b->empty)
        return 0;
    for (i = 0; i < b->n; i++)
        if (b->dpresent[i])
        {
            has_data = true;
            break;
        }
    /* The FIN's packet end and this block's own seq0/n state the cut. */
    return tbt_fec_blk_real_len_seq0(b->nreal, b->n, rx->total_known,
                                     rx->total_pkts, b->seq0, group,
                                     has_data);
}

/* Lazy recount: ndata is the number of present shares in [0, real). It only
 * changes when the block's real length changes (the first PARITY, or a FIN
 * that names the stream end), so the O(n) scan runs on those edges only. */
static void tbt_fec_blk_counts(const struct tbt_fec_rx *rx, u16 group,
                               struct tbt_fec_blk *b)
{
    u16 real = tbt_fec_blk_real_len(rx, group, b);
    u16 i, nd = 0;

    if (b->count_real == real)
        return;
    for (i = 0; i < real && i < b->n; i++)
        nd = (u16)(nd + (b->dpresent[i] ? 1 : 0));
    b->ndata = nd;
    b->count_real = real;
}

/* The shape, presence and payload lengths of one block, copied under
 * rx->lock before the codec runs. A carrier may place another share while
 * the decode is in flight; the codec must see the state its caller claimed,
 * not a mixture. `gen` and `block_id` gate the commit: a slot reused, or a
 * share placed, makes this snapshot stale. */
struct tbt_fec_decode_snap
{
    u64 block_id;
    u64 seq0;
    u32 gen;
    u16 n, m, real;
    u16 short_len;
    bool short_unknown;
    struct tbt_fec_codec_entry *codec_entry;
    u8 *data;
    u8 *parity;
    u16 dlen[TBT_FEC_MAX_SHARES];
    u8 dpresent[TBT_FEC_MAX_SHARES];
    u8 ppresent[TBT_FEC_MAX_SHARES];
};

/* Decode one claimed block outside rx->lock. Present slots point into the
 * block's data plane; missing slots point into rx->rec; padding points at
 * rx->zero. The codec only writes the missing slots, so a share placed after
 * the snapshot is never overwritten. Returns 0 when the codec ran,
 * -EAGAIN when more shares are needed. *short_bad reports a short-share
 * layout the commit must record under rx->lock. */
static int tbt_fec_blk_decode(struct tbt_session *sess,
                              struct tbt_fec_flow *f, u16 group,
                              const struct tbt_fec_decode_snap *snap,
                              bool *short_bad)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_grouped *codec;
    bool block_code = rx->sclc;
    u16 real, i, k, missing = 0, have_parity = 0;
    int rc;

    if (short_bad)
        *short_bad = false;
    if (block_code)
    {
        /* The block's own entry must be published. A block that arrived
         * before its codec was built waits here; it never decodes with the
         * shape of another block. */
        if (!snap->codec_entry || !snap->codec_entry->ready ||
            !snap->codec_entry->codec)
            return -EAGAIN;
        codec = snap->codec_entry->codec;
    }
    else
        codec = rx->codec;
    real = snap->real;
    if (!real)
        return 0; /* this group has no packet in this block */
    for (i = 0; i < snap->n; i++)
        if (!snap->dpresent[i])
            missing++;
    for (i = 0; i < snap->m; i++)
        if (snap->ppresent[i])
            have_parity++;
    if (!missing)
        return 0;
    /* RLNC needs one independent row per missing share; an SC-LDPC block code
     * needs any parity at all and decides itself how much it can recover, so
     * the count test would both reject decodable blocks and admit blocks the
     * parity indices do not cover. */
    if (!codec || (!block_code && have_parity < missing) ||
        (block_code && !have_parity))
        return -EAGAIN;
    /* A recovered share has no payload_len of its own, so the block's shape
     * must name every length the delivery walk will use. Only the last real
     * position may be shorter than S, and the parity's epoch must state that
     * same length; every earlier position is S. A short share anywhere else,
     * or an epoch that disagrees with the share it describes, leaves a
     * recovered slot without a length, and a guess would hand the application
     * the block's zero padding. Refuse the block instead of guessing. */
    for (i = 0; i < real; i++)
    {
        u16 dl = snap->dlen[i];

        if (!snap->dpresent[i] || !dl || dl >= rx->s)
            continue;
        if (i + 1 != real || snap->short_len != dl)
        {
            if (short_bad)
                *short_bad = true;
            return -EAGAIN;
        }
    }
    for (i = 0, k = 0; i < real; i++)
        rx->data_ptrs[i] = snap->dpresent[i] ?
            snap->data + (size_t)i * rx->s :
            rx->rec + (size_t)(k++) * rx->s;
    for (i = real; i < snap->n; i++)
        rx->data_ptrs[i] = rx->zero; /* padding is known zero */
    for (i = 0; i < snap->m; i++)
        rx->parity_ptrs[i] = snap->ppresent[i] ?
            snap->parity + (size_t)i * rx->s : rx->zero;

    tbt_fec_grouped_set_nonce(
        codec, group,
        tbt_fec_group_nonce(snap->codec_entry
                                ? snap->codec_entry->key.seed
                                : tbt_fec_base_nonce(sess),
                            group, snap->block_id));
    atomic64_inc(&sess->fec_decode_blocks);
    rc = tbt_fec_grouped_decode(codec, group, rx->data_ptrs,
                                rx->parity_ptrs, snap->dpresent,
                                snap->ppresent, snap->n);
    if (rc == TBT_FEC_RLNC_ERR_NOMEM)
        atomic64_inc(&sess->fec_decode_nomem);
    if (rc > 0)
        atomic64_add(rc, &sess->fec_decode_recovered);
    if (rc < 0)
        return -EAGAIN;
    /* A block codec returns how much it recovered, not whether the block is
     * whole. The commit path checks is_known() per share, so a partial
     * recovery is committed and the block stays open for the rest. Refusing
     * the whole block here threw the parity recovery away and forced a NACK
     * for every share the codec had already restored: the 0.3.0 first-connect
     * repair took 102 s on one block because of it. */
    return 0;
}

/* next_deliver is the packet frontier the sender's credit window uses.
 * deliver_bytes is the stream-relative byte count this receiver has actually
 * handed over; it is the only value the sender may use as tx_ack, because a
 * packet frontier can name bytes that no delivered share carried. */
/* The shape of the frontier block, or the session view when that slot is
 * not occupied. A late/out-of-order block must not define the ACK credit. */
static void tbt_fec_frontier_shape(struct tbt_fec_rx *rx, u64 s,
                                   u16 *n, u16 *m)
{
    struct tbt_fec_blk *b;

    spin_lock_bh(&rx->lock);
    b = tbt_fec_rx_blk(rx, 0, s);
    *n = tbt_fec_frontier_shape_n(b->used, b->block_id, s, b->n, rx->n);
    *m = tbt_fec_frontier_shape_m(b->used, b->block_id, s, b->m, rx->m);
    spin_unlock_bh(&rx->lock);
}

static void tbt_fec_ack(struct tbt_session *sess, struct tbt_fec_flow *f,
                        struct tbt_stream *st, u64 next_deliver,
                        u64 deliver_bytes)
{
    struct tbt_fec_hdr h;
    struct tbt_fec_ack a;
    u8 body[TBT_FEC_ACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_ACK_LEN];
    u16 len;

    memset(&a, 0, sizeof(a));
    a.next_deliver = next_deliver;
    a.deliver_bytes = deliver_bytes;
    /* The credit is K_eff-2 super-blocks: the ring holds K, and two are
     * slack so a block the receiver can still NACK is never evicted. The
     * receiver's arena `slots` is the other bound (a sender inside the
     * credit cannot overrun it), and the parent window caps the packets
     * (16 MiB downlink, 8 MiB uplink). The sender applies the same cap.
     * The cap follows the direction being ACKed: a multi-group receive
     * flow is the peer's 8x32 uplink, a single-group one its 1x256
     * downlink. Using the local TX variant here halved the downlink. */
    {
        struct tbt_fec_rx *rx = f->rx;

        u16 fn, fm;

        tbt_fec_frontier_shape(rx, READ_ONCE(rx->next_super), &fn, &fm);
        a.credit_packets = tbt_fec_credit_packets(
            rx->groups, fn, fm, rx->replay_k, rx->slots,
            tbt_fec_credit_max_packets_dir(sess, rx->groups > 1));
        /* The peer uses this as the replay round trip: a share asked again
         * before it can arrive would only duplicate the repair. The sample
         * is the NACK scan's carrier RTT gauge, 0 before the first one. */
        a.rtt_us = READ_ONCE(rx->rtt_us);
    }
    if (tbt_fec_ack_pack(&a, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_ACK;
    h.seq = next_deliver;
    h.payload_len = TBT_FEC_ACK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = f ? f->sid : (st ? st->sid : 0);
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    if (tbt_fec_send_record(sess, 0, rec, len, true, false, f))
        return;
    atomic64_inc(&sess->fec_ack_tx);
}

/* Ask the peer to replay the shares missing from one super-block. The NACK
 * is a control record: it may ride any up, non-stalled carrier and it is
 * queued at the head, ahead of new shares. */
static int tbt_fec_send_nack(struct tbt_session *sess,
                             struct tbt_fec_flow *f, u64 block_id,
                             const struct tbt_fec_nack *n)
{
    struct tbt_fec_hdr h;
    u8 body[TBT_FEC_NACK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_NACK_LEN];
    u16 len;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_NACK;
    h.block_id = block_id;
    h.payload_len = TBT_FEC_NACK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = f->sid;
    /* tbt_fec_nack_pack() returns the positive body length on success. Only
     * a negative value is an error; treating 64 as an error kept every NACK
     * off the wire (fec_nack_tx=0, fec_nack_drop climbing). */
    if (tbt_fec_nack_pack(n, body) != TBT_FEC_NACK_LEN)
        return -EINVAL;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return -EINVAL;
    {
        int err = tbt_fec_send_record(sess, 0, rec, len, true, false, f);

        if (!err)
            WRITE_ONCE(sess->fec_repair_last_ns, ktime_get_ns());
        return err;
    }
}

/* A closed block has no packet in `group`: its empty marker is the truth.
 * Answer a NACK for a share that was never stored with that marker, so the
 * receiver can mark the group empty instead of waiting for a share that will
 * never exist. The marker is also stored in the replay ring, so a later NACK
 * for share n hits it. */
static int tbt_fec_send_empty_marker(struct tbt_session *sess,
                                     struct tbt_fec_tx *tx, u16 group,
                                     u64 block_id, u16 n, u16 m,
                                     bool have_seq0, u64 seq0)
{
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_HDR_LEN];
    u16 len;

    if (!tx || !n || !m)
        return -EINVAL;
    if (debug >= 2)
        pr_info("tbtun: fec NACK empty reply block=%llu group=%u share=%u win_len=0\n",
                block_id, group, n);
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = n;
    h.n = (u8)n;
    h.m = (u8)m;
    h.block_id = block_id;
    /* PARITY names the block's first packet (group 0, share 0). The replay
     * ring stores that origin with the block's n/m; the fallback mapping is
     * for a block the ring no longer remembers. */
    h.seq = have_seq0
                ? seq0
                : tbt_fec_seq_of_block_pkt_base(block_id, 0, 0,
                                                tx->base_pkt,
                                                tx->base_block, tx->groups,
                                                n);
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = tx->sid;
    if (tbt_fec_make_record(sess, &h, NULL, rec, &len))
        return -EINVAL;
    /* The reply is built fresh every time and is never stored: a marker for
     * an old block id shares its ring slot with block_id + K, so storing it
     * would reclaim a block the receiver may still be waiting for. */
    if (debug >= 2)
        pr_info("tbtun: fec empty reply tx block=%llu group=%u share=%u cur=%llu\n",
                block_id, group, tx->n, READ_ONCE(tx->cur_block));
    return tbt_fec_send_record(sess, 0, rec, len, true, false, tx->flow);
}

/* One failed NACK, split by error code. The masks are logged once per rate
 * window so a field run can tell a bad shape from an allocation failure. */
static void tbt_fec_nack_drop(struct tbt_session *sess, int err, u64 block_id,
                              const struct tbt_fec_nack *n)
{
    switch (err)
    {
    case -ENOMEM:
        atomic64_inc(&sess->fec_nack_enomem);
        break;
    case -EINVAL:
        atomic64_inc(&sess->fec_nack_einval);
        break;
    case -ENOSPC:
        atomic64_inc(&sess->fec_nack_enospc);
        break;
    default:
        atomic64_inc(&sess->fec_nack_eother);
        break;
    }
    atomic64_inc(&sess->fec_nack_drop);
    pr_info_ratelimited("tbtun: fec NACK drop err=%d block=%llu group=%*phN share=%*phN\n",
                        err, block_id, TBT_FEC_BITMAP_BYTES, n->group_mask,
                        TBT_FEC_BITMAP_BYTES, n->share_mask);
}

/* A NACK must not name a group or share the session shape does not have. */
static bool tbt_fec_bitmap_clear_from(const u8 bm[TBT_FEC_BITMAP_BYTES],
                                      u16 from)
{
    u16 i;

    for (i = from; i < TBT_FEC_MAX_SHARES; i++)
        if (tbt_fec_bitmap_test(bm, i))
            return false;
    return true;
}

static struct tbt_fec_blk *tbt_fec_rx_blk(struct tbt_fec_rx *rx, u16 group,
                                          u64 block_id)
{
    return &rx->blk[(size_t)group * rx->slots + (block_id % rx->slots)];
}

/* Every group must have decoded block s before any byte of it is delivered.
 * In the final super-block a group with no packet is complete by definition.
 * The caller holds rx->lock: block slots are reset by a carrier as soon as
 * the window advances, and reading used/ready/block_id without the lock races
 * that reset. */
static bool tbt_fec_super_ready_locked(const struct tbt_fec_rx *rx, u64 s)
{
    u64 seq0 = 0;
    u16 bn = 0;
    u16 group;

    /* The super-block's own shape comes from any block already placed for
     * it. All groups of one super-block share it: the sender builds one
     * block shape at a time. */
    for (group = 0; group < rx->groups; group++)
    {
        struct tbt_fec_blk *b =
            tbt_fec_rx_blk((struct tbt_fec_rx *)rx, group, s);

        if (b->used && b->block_id == s)
        {
            seq0 = b->seq0;
            bn = b->n;
            break;
        }
    }
    for (group = 0; group < rx->groups; group++)
    {
        struct tbt_fec_blk *b;

        b = tbt_fec_rx_blk((struct tbt_fec_rx *)rx, group, s);
        if (rx->total_known && bn)
        {
            u64 first = seq0 + (u64)group * bn;

            if (rx->total_pkts <= first)
            {
                /* The stream ends before this group's first share. Skip it
                 * only when no block was ever placed; a block that does
                 * exist must still be ready. */
                if (!b->used || b->block_id != s)
                    continue;
            }
        }
        /* A block placed for the old stream must not make the new stream's
         * super-block look ready: the delivery walk would advance its cursor
         * without handing over any byte. */
        if (!b->used || !b->ready || b->block_id != s ||
            !tbt_fec_blk_sid_match(b->sid, rx->stream_sid))
            return false;
    }
    return true;
}

/* Missing shares in the frontier super-block, for the reset log. The block
 * may not exist at all (a whole-block loss): every DATA share of every group
 * is then missing. The caller must not hold rx->lock. */
static u16 tbt_fec_frontier_missing(struct tbt_fec_rx *rx, u64 s)
{
    u16 g, i, missing = 0;

    spin_lock_bh(&rx->lock);
    for (g = 0; g < rx->groups; g++)
    {
        struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);
        u16 real;

        if (!b->used || b->block_id != s)
        {
            missing = (u16)(missing + rx->n);
            continue;
        }
        if (b->ready)
            continue;
        real = tbt_fec_blk_real_len(rx, g, b);
        for (i = 0; i < real; i++)
            if (!b->dpresent[i])
                missing++;
        for (i = 0; i < b->m; i++)
            if (!b->ppresent[i])
                missing++;
    }
    spin_unlock_bh(&rx->lock);
    return missing;
}

/* Name the state a FEC hole reset is about to throw away: the frontier, the
 * stream's known end, the missing share count, how long the walk has made no
 * progress, and the replay queue depth. The acceptance runs debug=0, so the
 * per-block dumps are off; this line is unconditional (ratelimited). */
static void tbt_fec_reset_diag(struct tbt_session *sess,
                               struct tbt_fec_flow *f, u64 s,
                               const char *why)
{
    struct tbt_fec_rx *rx = f->rx;
    u64 now = ktime_get_ns();
    u32 rq;

    spin_lock(&sess->fec_out_lock);
    rq = sess->fec_replay_queued;
    spin_unlock(&sess->fec_out_lock);
    pr_info_ratelimited("tbtun: fec reset diag why=%s super=%llu next=%llu total_known=%d total_pkts=%llu missing=%u repair_age_us=%llu replay_q=%u\n",
                        why, s, READ_ONCE(rx->next_super),
                        READ_ONCE(rx->total_known),
                        READ_ONCE(rx->total_pkts),
                        tbt_fec_frontier_missing(rx, s),
                        rx->progress_ns && now > rx->progress_ns
                            ? (now - rx->progress_ns) / NSEC_PER_USEC
                            : 0,
                        rq);
}


/* Deliver every ready super-block, in order, from the exact cursor the last
 * attempt stopped at. Only w_fec_rx calls this, so it is the only writer of
 * next_super / deliver_bytes / next_deliver / ack_deliver.
 *
 * -ENOMEM is a per-packet stop, not a lost packet: the successful prefix is
 * committed, (deliver_p, deliver_g) names the first packet that failed, and
 * next_super / next_deliver / ack_deliver do not move. The work item retries
 * in 2 ms and resumes from the cursor; no byte is skipped and no ACK runs
 * past a byte the application never received. */
static void tbt_fec_rx_late_take(struct tbt_fec_rx *rx, u64 block_id,
                                 u16 group, u16 n, u16 m, u8 *out);
static void tbt_fec_rx_late_note_miss(struct tbt_fec_rx *rx, u64 block_id,
                                      u16 group, u16 n, u16 m,
                                      const u8 *sc_miss,
                                      const u8 *codec_fail, u16 real);
static void tbt_fec_rx_late_finalize(struct tbt_fec_rx *rx, u64 block_id,
                                     u8 bucket);

static noinline void tbt_fec_deliver_est_fold(struct tbt_session *sess,
                                              struct tbt_fec_rx *rx, u64 s)
{
    bool sc_mode = rx->est_sclc && rx->sclc &&
                   rx->groups <= TBT_FEC_ADAPT_GROUPS;
    bool rlnc_mode = READ_ONCE(sess->adaptive) && rx->est &&
                     rx->groups <= TBT_FEC_ADAPT_GROUPS &&
                     rx->shares == TBT_FEC_ADAPT_SHARES;

    if (sc_mode || rlnc_mode)
    {
        u64 est_now = ktime_get_ns();
        u16 eg;
        u8 late_bits[TBT_FEC_MAX_SHARES];

        for (eg = 0; eg < rx->groups; eg++)
        {
            struct tbt_fec_blk *eb;
            u16 real = 0;
            u32 miss = 0, fail = 0;
            u8 sc_miss[TBT_FEC_BITMAP_BYTES];
            bool unknown = false;

            spin_lock_bh(&rx->lock);
            eb = tbt_fec_rx_blk(rx, eg, s);
            if (!eb->used || eb->block_id != s || eb->judged)
            {
                spin_unlock_bh(&rx->lock);
                continue;
            }
            eb->judged = true;
            real = tbt_fec_blk_real_len(rx, eg, eb);
            miss = eb->miss_mask;
            if (sc_mode)
            {
                u16 si;

                memcpy(sc_miss, eb->miss_bits, TBT_FEC_BITMAP_BYTES);
                for (si = 0; si < real; si++)
                {
                    if (eb->dpresent[si])
                        continue;
                    tbt_fec_adapt_bitmap_set(sc_miss, si);
                    if (tbt_fec_adapt_bitmap_test(eb->codec_fail_bits,
                                                  si))
                        fail++;
                }
            }
            /* Without a parity cut and without the stream end, a block
             * that never filled n slots has an unknown real length: do
             * not estimate p from it. */
            unknown = !eb->win_len_set && !rx->total_known &&
                      eb->ndata < eb->n;
            if (sc_mode)
            {
                u16 si;

                memset(late_bits, 0, sizeof(late_bits));
                tbt_fec_rx_late_take(rx, s, eg, eb->n, eb->m,
                                     late_bits);
                for (si = 0; si < real; si++)
                {
                    if (!late_bits[si] ||
                        !tbt_fec_adapt_bitmap_test(sc_miss, si) ||
                        tbt_fec_adapt_bitmap_test(eb->codec_fail_bits, si))
                        continue;
                    tbt_fec_adapt_bitmap_clear(sc_miss, si);
                    if (si < 32)
                        miss &= ~(1u << si);
                }
                tbt_fec_rx_late_note_miss(rx, s, eg, eb->n, eb->m,
                                          sc_miss,
                                          eb->codec_fail_bits, real);
            }
            spin_unlock_bh(&rx->lock);
            if (unknown)
            {
                if (sc_mode)
                {
                    spin_lock_bh(&rx->lock);
                    tbt_fec_adapt_sclc_est_unknown(rx->est_sclc,
                                                   est_now);
                    spin_unlock_bh(&rx->lock);
                }
                else
                    tbt_fec_est_unknown(rx->est, est_now);
            }
            else if (real)
            {
                if (sc_mode)
                {
                    spin_lock_bh(&rx->lock);
                    tbt_fec_adapt_sclc_est_block(
                        rx->est_sclc, est_now, eg, real, sc_miss,
                        rx->alive_bm);
                    if (fail)
                        tbt_fec_adapt_sclc_est_codec_fail(
                            rx->est_sclc, est_now, fail);
                    spin_unlock_bh(&rx->lock);
                }
                else
                    tbt_fec_est_block(rx->est, est_now, eg, real, miss,
                                      rx->alive_bm);
            }
        }
        if (sc_mode)
        {
            spin_lock_bh(&rx->lock);
            tbt_fec_adapt_sclc_est_super(rx->est_sclc, est_now);
            tbt_fec_rx_late_finalize(rx, s, rx->est_sclc->cur);
            spin_unlock_bh(&rx->lock);
        }
        else
            tbt_fec_est_super(rx->est, est_now);
    }
}

static void tbt_fec_rx_fin_publish(struct tbt_stream *st,
                                   struct tbt_fec_rx *rx);

static int tbt_fec_deliver_ready(struct tbt_session *sess,
                                 struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_stream *st;
    bool delivered = false;
    bool blocked = false;
    const char *blocked_why = "?";
    u8 *overflow = NULL;
    u16 dg;

    if (READ_ONCE(rx->rebase_pending))
        return 0;
    if (READ_ONCE(rx->total_known) && !READ_ONCE(rx->total_pkts) &&
        !rx->deliver_partial)
        return 0;

    st = tbt_stream_get(sess, f->sid, false, false);
    if (!st)
        return 0;
    for (;;)
    {
        u64 s = rx->next_super;
        u64 seq0 = 0, end = 0, off = rx->deliver_bytes;
        u64 limit;
        u16 group, bn = 0, bm = 0;
        bool ready, have_shape = false;
        bool known = READ_ONCE(rx->total_known);
        u64 total_pkts = READ_ONCE(rx->total_pkts);

        blocked = false;
        /* Teardown: stop handing bytes to a stream that is being destroyed.
         * The ACK below still covers what was delivered before the flag. */
        if (READ_ONCE(rx->stopping))
            break;
        /* A FIN names the packet end; no super-block starts at or after it. */
        if (known && rx->next_deliver >= total_pkts)
            break;
        spin_lock_bh(&rx->lock);
        /* The super-block's own shape and seq origin come from any block
         * already placed for it. All groups of one super-block share them:
         * the sender builds one block shape at a time. */
        for (group = 0; group < rx->groups; group++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, group, s);
            u64 bend;

            if (!b->used || b->block_id != s)
                continue;
            if (!have_shape)
            {
                seq0 = b->seq0;
                bn = b->n;
                bm = b->m;
                have_shape = true;
            }
            bend = b->seq0 + (u64)rx->groups * b->n;
            if (bend > end)
                end = bend;
        }
        ready = tbt_fec_super_ready_locked(rx, s);
        if (!ready)
        {
            /* Name the group whose block holds the whole stream. This is a
             * per-frontier diagnostic: a v7 flow is per stream, and a detached
             * or aborted flow must not fill the log (0.2.2 saw 20-40 lines per
             * abort). Keep it behind debug. */
            u64 now_diag = ktime_get_ns();

            for (group = 0; group < rx->groups; group++)
            {
                struct tbt_fec_blk *sb = tbt_fec_rx_blk(rx, group, s);
                u16 k, have = 0;

                if (sb->used && sb->ready && sb->block_id == s)
                    continue;
                for (k = 0; k < sb->n; k++)
                    have = (u16)(have + (sb->dpresent[k] ? 1 : 0));
                if (debug >= 2)
                    pr_info_ratelimited("tbtun: fec super %llu stuck: next=%llu total_known=%d total_pkts=%llu next_deliver=%llu deliver_bytes=%llu group=%u/%u used=%d ready=%d id=%llu nreal=%u shares=%u/%u parity=%u wset=%d empty=%d short=%u cnt=%d gap_us=%llu\n",
                                        s, READ_ONCE(rx->next_super), known,
                                        total_pkts,
                                        READ_ONCE(rx->next_deliver),
                                        READ_ONCE(rx->deliver_bytes), group,
                                        rx->groups, sb->used, sb->ready,
                                        sb->block_id, sb->nreal, have, sb->n,
                                        (unsigned)sb->m, sb->win_len_set,
                                        sb->empty, sb->short_len,
                                        (int)sb->count_real,
                                        sb->first_gap_ns &&
                                                now_diag > sb->first_gap_ns
                                            ? (now_diag - sb->first_gap_ns) /
                                                  NSEC_PER_USEC
                                            : 0);
                break;
            }
        }
        spin_unlock_bh(&rx->lock);
        if (!ready)
        {
            /* The walk stopped at a super-block that is not ready. The NACK
             * scan above asks the peer for the missing shares while they are
             * in its replay window; the carrier TCP still holds the rest.
             * Reset only when every carrier TCP has failed: an independent
             * timer here would make the tunnel reset in cases where a direct
             * TCP stream would keep retransmitting. */
            u64 now = ktime_get_ns();
            u64 budget = (u64)TBT_FEC_STALL_MS * NSEC_PER_MSEC;
            u64 left_ms;

            if (!rx->progress_ns)
                rx->progress_ns = now;
            if (now - rx->progress_ns >= budget &&
                !tbt_fec_any_tcp_alive(sess))
            {
                spin_lock_bh(&rx->lock);
                rx->stopping = true;
                spin_unlock_bh(&rx->lock);
                pr_info_ratelimited("tbtun: fec reset: super %llu not ready and no carrier TCP alive (total_pkts=%llu)\n",
                                    s, READ_ONCE(rx->total_pkts));
                tbt_fec_reset_diag(sess, f, s, "no tcp alive");
                atomic64_inc(&sess->fec_reset_tcp_failed);
                atomic64_inc(&sess->fec_reset_hole);
                atomic64_inc(&sess->holes_unrecoverable);
                /* The stream reset the same edge: keep the session-level
                 * cause counter truthful for runs that read c3/c9 only. */
                atomic64_inc(&sess->stream_reset_tcp_failed);
                if (st)
                    tbt_stream_kill(st, TBT_ST_HOLE);
                break;
            }
            if (now - rx->progress_ns >= budget)
            {
                /* At least one carrier TCP is alive. Keep the stream waiting
                 * and repairing: the NACK/replay path and the carrier's own
                 * retransmission own the recovery, and only the user's own
                 * timeout may end the stream. */
                rx->progress_ns = now;
                left_ms = TBT_FEC_STALL_MS;
            }
            else
            {
                left_ms = (budget - (now - rx->progress_ns)) / NSEC_PER_MSEC + 1;
            }
            tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                            msecs_to_jiffies(tbt_fec_rx_poll_ms(
                                left_ms, now - rx->progress_ns)));
            break;
        }

        if (!have_shape || !bn)
            break; /* no block to deliver; the NACK path owns this frontier */
        if (rx->next_deliver < seq0 || rx->next_deliver >= end)
        {
            /* The delivery cursor and the block's seq base disagree. A
             * silent skip would move the ACK past bytes no share carried. */
            pr_info_ratelimited("tbtun: fec deliver seq mismatch: super %llu next=%llu seq0=%llu end=%llu\n",
                                s, rx->next_deliver, seq0, end);
            spin_lock_bh(&rx->lock);
            rx->stopping = true;
            spin_unlock_bh(&rx->lock);
            atomic64_inc(&sess->fec_reset_hole);
            atomic64_inc(&sess->holes_unrecoverable);
            tbt_fec_reset_diag(sess, f, s, "seq base");
            if (st)
                tbt_stream_kill(st, TBT_ST_HOLE);
            break;
        }
        limit = known && total_pkts < end ? total_pkts : end;
        while (rx->next_deliver < limit)
        {
            u64 cur = rx->next_deliver;
            u64 rel = cur - seq0;
            u16 g = (u16)(rel / bn);
            u16 p = (u16)(rel % bn);
            struct tbt_fec_blk *b;
            u16 len, real;
            int err;

            if (g >= rx->groups)
            {
                blocked = true;
                blocked_why = "seq range";
                break;
            }
            b = tbt_fec_rx_blk(rx, g, s);
            if (!b->used || b->block_id != s ||
                (st && !tbt_fec_blk_sid_match(b->sid, st->sid)))
            {
                /* This group has no block in the super-block: its shares
                 * carry no stream byte. Skip the whole group run. */
                u64 next = seq0 + (u64)(g + 1) * bn;

                if (next > limit)
                    next = limit;
                WRITE_ONCE(rx->next_deliver, next);
                continue;
            }
            if (b->n != bn || b->m != bm || b->seq0 != seq0)
            {
                /* One super-block has one shape: the sender's packetizer
                 * builds every group from the same block. */
                blocked = true;
                blocked_why = "block shape";
                break;
            }
            /* A block that was closed early has fewer real shares than n;
             * the rest are the encoder's zero padding and carry no stream
             * bytes. A marker block has no real share at all. */
            real = tbt_fec_blk_real_len(rx, g, b);
            if (p >= real)
            {
                /* Padding carries no stream byte, so skipping it is
                 * correct -- unless a share is actually present here. Then
                 * the sender's cut and its DATA records disagree, and the
                 * skip would delete that share's payload while the packet
                 * frontier and the ACK ran past it. Refuse the block and
                 * name the reason instead. */
                if (b->dpresent[p])
                {
                    atomic64_inc(&sess->fec_slot_past_cut);
                    blocked = true;
                    blocked_why = "cut";
                    break;
                }
                WRITE_ONCE(rx->next_deliver, cur + 1);
                continue;
            }
            if (known && cur >= total_pkts)
            {
                /* Past the stream's last packet: no byte exists here.
                 * A present share would mean the stream end is wrong. */
                if (b->dpresent[p])
                {
                    atomic64_inc(&sess->fec_slot_past_cut);
                    blocked = true;
                    blocked_why = "past_end";
                    break;
                }
                WRITE_ONCE(rx->next_deliver, cur + 1);
                continue;
            }
            len = tbt_fec_rx_share_len(b->dlen[p], b->short_len, p, real,
                                       rx->s);
            if (!len)
            {
                /* A recovered share whose length no record stated, in a
                 * block that also holds a short share. Guessing S would
                 * hand the application the block's zero padding. */
                atomic64_inc(&sess->fec_short_unknown);
                blocked = true;
                blocked_why = "short";
                break;
            }
            /* Defence in depth: hdr_check() already refuses an epoch
             * larger than S, but a delivery length that named more than
             * one slot would read past this block's buffer. */
            if (len > rx->s)
                len = rx->s;
            err = tbt_rx_fec_deliver(st, off,
                                     b->data + (size_t)p * rx->s, len);
            if (err == -ENOMEM)
            {
                /* Commit only the successful prefix and keep the cursor.
                 * next_super, next_deliver and ack_deliver stay put. */
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off;
                rx->deliver_p = p;
                rx->deliver_g = g;
                rx->deliver_partial = true;
                spin_unlock_bh(&rx->lock);
                atomic64_inc(&sess->fec_deliver_nomem);
                tbt_session_arm(sess, TBT_SW_FEC_RX, msecs_to_jiffies(2));
                tbt_stream_put(st);
                return -ENOMEM;
            }
            if (err == 1)
            {
                /* The share's bytes are already in the stream (a duplicate
                 * or an expired retry). Advancing off here would count
                 * them twice and the ACK would acknowledge bytes the
                 * application never received. Realign the cursor to what
                 * the stream socket has actually accepted, then continue
                 * with the next share. */
                u64 frontier;

                spin_lock_bh(&st->rx_lock);
                frontier = READ_ONCE(st->rx_next);
                spin_unlock_bh(&st->rx_lock);
                atomic64_inc(&sess->fec_deliver_dup);
                off = frontier;
                spin_lock_bh(&rx->lock);
                rx->deliver_bytes = off;
                rx->next_deliver = cur + 1;
                spin_unlock_bh(&rx->lock);
                delivered = true; /* the ACK may now name the real bytes */
                continue;
            }
            if (err == 2)
            {
                /* The ordered walk found a hole: the caller's cursor ran
                 * ahead of the stream socket. Queueing that share in the
                 * reorder tree would deliver bytes out of order, which is
                 * the silent corruption this walk exists to prevent.
                 * Stop the FEC receiver and reset the stream instead. */
                u64 frontier;

                spin_lock_bh(&st->rx_lock);
                frontier = READ_ONCE(st->rx_next);
                spin_unlock_bh(&st->rx_lock);
                atomic64_inc(&sess->fec_deliver_ooo);
                pr_info_ratelimited("tbtun: fec ordered walk hole: super %llu p=%u group=%u off=%llu frontier=%llu\n",
                                    s, p, g, off, frontier);
                tbt_fec_reset_diag(sess, f, s, "ordered walk hole");
                spin_lock_bh(&rx->lock);
                rx->stopping = true;
                spin_unlock_bh(&rx->lock);
                atomic64_inc(&sess->fec_reset_hole);
                atomic64_inc(&sess->holes_unrecoverable);
                tbt_stream_kill(st, TBT_ST_HOLE);
                tbt_stream_put(st);
                return 0;
            }
            if (err == -EPROTO)
                atomic64_inc(&sess->fec_deliver_bad);
            off += len;
            spin_lock_bh(&rx->lock);
            rx->deliver_bytes = off; /* per-packet commit */
            rx->next_deliver = cur + 1;
            spin_unlock_bh(&rx->lock);
            delivered = true;
        }
        if (blocked)
        {
            /* Commit the successful prefix and keep the cursor: a retry
             * resumes at the same share. next_super and next_deliver do
             * not move, so the ACK cannot name a byte that was never
             * delivered. */
            u64 rel = rx->next_deliver - seq0;

            spin_lock_bh(&rx->lock);
            rx->deliver_bytes = off;
            rx->deliver_p = (u16)(rel % bn);
            rx->deliver_g = (u16)(rel / bn);
            rx->deliver_partial = true;
            spin_unlock_bh(&rx->lock);
            if (debug >= 2)
                pr_info("tbtun: fec deliver partial: s=%llu p=%u g=%u off=%llu blocked=%s next_deliver=%llu ack=%llu\n",
                        s, rx->deliver_p, rx->deliver_g, off, blocked_why,
                        rx->next_deliver, rx->ack_deliver);
            break;
        }
        /* A blocked walk did not deliver this super-block. Advancing here
         * would clear the cursor the commit above just saved and move the
         * frontier past bytes the application never received. */
        if (blocked)
            break;
        /* Fold this super-block's repair evidence into the estimator before
         * next_super moves: a carrier reuses a slot as soon as the window
         * advances, and the mask belongs to the block just delivered. */
        tbt_fec_deliver_est_fold(sess, rx, s);
        /* Every group's bytes for s are now out of the block buffers, so
         * return those buffers to the hot pool before the slot can be
         * reused. Detach under the leaf lock; free overflow after it. */
        spin_lock_bh(&rx->lock);
        for (dg = 0; dg < rx->groups; dg++)
        {
            struct tbt_fec_blk *db = tbt_fec_rx_blk(rx, dg, s);

            if (db->used && db->block_id == s && db->buf)
            {
                u8 *buf = db->buf;

                db->buf = NULL;
                tbt_fec_blk_slice(rx, db);
                tbt_fec_rx_buf_shed(rx, buf, &overflow);
            }
        }
        spin_unlock_bh(&rx->lock);
        tbt_fec_rx_buf_free_list(overflow);
        overflow = NULL;
        rx->deliver_partial = false;
        rx->deliver_p = 0;
        rx->deliver_g = 0;
        rx->next_super++;
        rx->progress_ns = 0; /* progress: the stall budget restarts */
        /* The block's own seq0/n names the end of its packet span. */
        WRITE_ONCE(rx->next_deliver, end);
        if (known && rx->next_deliver > total_pkts)
            WRITE_ONCE(rx->next_deliver, total_pkts);
        if (debug >= 2)
            pr_info("tbtun: fec deliver super: s=%llu next=%llu next_deliver=%llu ack=%llu\n",
                    s, rx->next_super, rx->next_deliver, rx->ack_deliver);
    }
    if (delivered && rx->next_deliver > rx->ack_deliver)
    {
        u64 stream_bytes = rx->deliver_bytes > rx->stream_base ?
                           rx->deliver_bytes - rx->stream_base : 0;

        rx->ack_deliver = rx->next_deliver;
        /* The ACK names the frontier and the bytes. The frontier keeps the
         * sender's credit window in packet units; the byte count is what
         * tx_ack and every retention decision use. They are not
         * interchangeable: a block closed early, or a share whose payload is
         * shorter than S, leaves the frontier ahead of the bytes. */
        tbt_fec_ack(sess, f, st, rx->next_deliver, stream_bytes);
    }
    if (blocked)
    {
        u64 stream_off = rx->deliver_bytes > rx->stream_base ?
                         rx->deliver_bytes - rx->stream_base : 0;

        /* A ready block cannot change, so a share at or past its cut, a
         * share past the stream end, or a recovered share with no stated
         * length can never be reconciled. Retrying the same cursor would
         * spin forever while the ACK stayed behind it. Fail the stream
         * instead of stalling silently. */
        spin_lock_bh(&rx->lock);
        rx->stopping = true;
        spin_unlock_bh(&rx->lock);
        atomic64_inc(&sess->fec_reset_hole);
        atomic64_inc(&sess->holes_unrecoverable);
        pr_info_ratelimited("tbtun: fec deliver conflict: super %llu p=%u group=%u reason=%s off=%llu; stream reset\n",
                            rx->next_super, rx->deliver_p, rx->deliver_g,
                            blocked_why, stream_off);
        tbt_fec_reset_diag(sess, f, rx->next_super, blocked_why);
        if (st)
            tbt_stream_kill(st, TBT_ST_HOLE);
    }
    /* A FIN may have arrived before this walk reached its packet end. Now
     * that it has, publish the byte end so the drain can shut the target's
     * write side in order. */
    if (st)
    {
        tbt_fec_rx_fin_publish(st, rx);
        tbt_stream_put(st);
    }
    return 0;
}

/* Refresh rx->rtt_us from the carriers' own RTT estimators, at most every
 * 100 ms. A replay may ride any up carrier, so the largest RTT is the bound
 * a re-request must respect. */
static void tbt_fec_rx_sample_rtt(struct tbt_session *sess,
                                  struct tbt_fec_flow *f, u64 now)
{
    struct tbt_fec_rx *rx = f->rx;
    u32 max_rtt = 0;
    u16 i;

    if (now - rx->rtt_sample_ns < 100000000ULL)
        return;
    rx->rtt_sample_ns = now;
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (READ_ONCE(c->rtt_us) > max_rtt)
            max_rtt = READ_ONCE(c->rtt_us);
        tbt_carrier_put(c);
    }
    if (max_rtt)
        rx->rtt_us = max_rtt;
}

/* Last NACK request time for one share, 0 when the slice belongs to another
 * block or the share was never asked. The caller holds rx->lock. */
static u64 tbt_fec_req_ns(const struct tbt_fec_rx *rx, u16 group, u64 block_id,
                          u16 share)
{
    size_t bs = (size_t)group * rx->slots + (size_t)(block_id % rx->slots);

    if (rx->req_block[bs] != block_id)
        return 0;
    return rx->req_ns[bs * (size_t)rx->shares + share];
}

/* Add one missing share unless its replay is still inside the hold window.
 * Returns 1 when the bit was set. The caller holds rx->lock. */
static u16 tbt_fec_nack_add(struct tbt_session *sess, struct tbt_fec_rx *rx,
                            struct tbt_fec_nack *n, u16 group, u64 block_id,
                            u16 share, u64 now, u64 hold, u16 *count)
{
    struct tbt_fec_blk *b;

    if (tbt_fec_nack_share_skip(false, now,
                                tbt_fec_req_ns(rx, group, block_id, share),
                                hold) == TBT_FEC_NACK_SKIP_INFLIGHT)
    {
        atomic64_inc(&sess->fec_nack_filtered_inflight);
        return 0;
    }
    if (*count >= TBT_FEC_NACK_SHARES_MAX)
        return 0;
    tbt_fec_bitmap_set(n->group_mask, group);
    tbt_fec_bitmap_set(n->share_mask, share);
    /* A DATA share the NACK names needed repair. The estimator reads the
     * block's mask when the super-block completes. */
    b = tbt_fec_rx_blk(rx, group, block_id);
    if (b->used && b->block_id == block_id && share < b->n)
    {
        if (share < 32)
            b->miss_mask |= 1u << share;
        tbt_fec_adapt_bitmap_set(b->miss_bits, share);
    }
    (*count)++;
    return 1;
}

/* Stamp every share a NACK named, so the next scan leaves it alone for one
 * round trip, and advance the shape-probe pacing when this NACK was only a
 * probe. */
static void tbt_fec_nack_stamp(struct tbt_fec_rx *rx, u64 block_id,
                               const struct tbt_fec_nack *n, u64 now,
                               bool probed)
{
    u16 g, i, shares = rx->shares;

    spin_lock_bh(&rx->lock);
    for (g = 0; g < rx->groups; g++)
    {
        size_t bs;
        size_t base;

        if (!tbt_fec_bitmap_test(n->group_mask, g))
            continue;
        bs = (size_t)g * rx->slots + (size_t)(block_id % rx->slots);
        base = bs * shares;
        if (rx->req_block[bs] != block_id)
        {
            memset(&rx->req_ns[base], 0, shares * sizeof(u64));
            rx->req_block[bs] = block_id;
        }
        for (i = 0; i < shares; i++)
            if (tbt_fec_bitmap_test(n->share_mask, i))
                rx->req_ns[base + i] = now;
    }
    spin_unlock_bh(&rx->lock);
    if (probed)
    {
        rx->probe_last_ns = now;
        if (rx->probe_tries < TBT_FEC_NACK_PROBE_MAX)
            rx->probe_tries++;
    }
}

/* Ask the peer for the shares missing from the oldest super-block that is
 * still inside the replay window. One NACK per pass, rate-limited and
 * retried without a total cap: a live carrier TCP keeps the repair going.
 * A block outside the window is left to that TCP's own retransmission; the
 * stream is reset only when every carrier TCP has failed.
 *
 * Three filters keep the request rate below what the sender can serve:
 *   - the first NACK waits one round trip from the block's first gap (the
 *     carrier TCP and the parity may still repair it);
 *   - a share is not named again inside max(RTT, 2 x cadence), so a reply
 *     that is still in flight is not duplicated;
 *   - one NACK names at most TBT_FEC_NACK_SHARES_MAX shares, so a burst
 *     loss cannot fill the sender's 8 MiB replay queue with one request.
 * A shape probe for an unbuilt block runs at TBT_FEC_NACK_PROBE_MS, not at
 * the repair cadence, and probes only shares whose hold has expired. */
/* Retry interval for one block: the repair cadence, raised by the block's
 * own exponential backoff. A frontier that survives several NACKs is either
 * waiting on a replay that is still queued or naming shares the sender no
 * longer holds; asking faster only doubles the repair traffic. */
static u32 tbt_fec_retry_ms(const struct tbt_fec_rx *rx, u64 block)
{
    u32 ms = rx->nack_cadence_ms;

    if (rx->nack_backoff_valid && rx->nack_backoff_block == block &&
        rx->nack_backoff_ms > ms)
        ms = rx->nack_backoff_ms;
    return ms;
}

/* One unanswered request doubles this block's interval, up to the cap. The
 * first request starts at the current cadence, so a healthy repair keeps the
 * old timing and only a stuck frontier backs off. */
static void tbt_fec_backoff_note(struct tbt_fec_rx *rx, u64 block)
{
    u32 next;

    if (!rx->nack_backoff_valid || rx->nack_backoff_block != block)
    {
        rx->nack_backoff_valid = true;
        rx->nack_backoff_block = block;
        rx->nack_backoff_ms = rx->nack_cadence_ms;
        return;
    }
    next = rx->nack_backoff_ms ? rx->nack_backoff_ms * 2
                               : rx->nack_cadence_ms;
    if (next > TBT_FEC_NACK_BACKOFF_MAX_MS)
        next = TBT_FEC_NACK_BACKOFF_MAX_MS;
    rx->nack_backoff_ms = next;
}

static void tbt_fec_nack_scan(struct tbt_session *sess,
                              struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_nack n;
    u64 now = ktime_get_ns();
    u64 hold;
    u64 s, hi;
    u16 g, i;
    bool found = false;
    bool young = false;
    bool any_probe = false; /* the NACK being built names probe shares */
    bool probe_only = false; /* the request being sent is only a shape probe */
    bool block_code;

    if (!rx || !rx->nack || READ_ONCE(rx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    if (READ_ONCE(rx->rebase_pending))
        return;
    /* The shape must support a ring. Both ends derive the same effective K,
     * so the receiver NACKs exactly the blocks the sender can replay; the
     * sender counts fec_replay_miss_no_ring if its own allocation failed. */
    if (!rx->replay_k)
        return;
    block_code = rx->sclc;

    tbt_fec_rx_sample_rtt(sess, f, now);

    /* A previous NACK's block is now ready or delivered: that interval is
     * the repair latency the sender's cost model needs. */
    if (rx->nack->valid)
    {
        bool repaired;

        spin_lock_bh(&rx->lock);
        repaired = rx->nack->block_id < rx->next_super ||
                   tbt_fec_super_ready_locked(rx, rx->nack->block_id);
        spin_unlock_bh(&rx->lock);
        if (repaired)
        {
            u64 dt = now - (rx->repair_first_ns &&
                                    rx->repair_block == rx->nack->block_id
                                ? rx->repair_first_ns
                                : rx->nack->first_ns);

            atomic64_add(dt, &sess->fec_nack_repair_ns);
            tbt_fec_stat_max(&sess->fec_nack_repair_max_ns, dt);
            /* A slow repair means the sender's replay queue is behind;
             * asking faster only adds duplicates. */
            rx->nack_cadence_ms =
                tbt_fec_nack_cadence_ms(rx->nack_cadence_ms, dt);
            rx->repair_first_ns = 0;
            rx->nack_backoff_valid = false;
            rx->nack_backoff_ms = 0;
            tbt_fec_nack_reset(rx->nack);
        }
    }

    /* The frontier advanced past the block that was backing off: the next
     * block starts with the plain cadence. */
    if (rx->nack_backoff_valid &&
        rx->nack_backoff_block < rx->next_super)
    {
        rx->nack_backoff_valid = false;
        rx->nack_backoff_ms = 0;
    }

    /* How long a requested share is left alone: one round trip, never less
     * than two repair intervals. Computed after the cadence update so this
     * pass uses the latency the last repair taught. */
    hold = tbt_fec_nack_hold_ns(
        rx->rtt_us, rx->nack_backoff_ms > rx->nack_cadence_ms
                       ? rx->nack_backoff_ms
                       : rx->nack_cadence_ms);

    hi = READ_ONCE(rx->highest_block);
    /* The delivery frontier may be ahead of the highest block seen: every
     * share of the next super-block was lost. Probe it anyway (parity 0 for
     * each group) so the sender's replay ring can state its shape; otherwise
     * the receiver waits for a share that never arrives and the sender's TX
     * stall fires. */
    if (hi < rx->next_super)
        hi = rx->next_super;
    /* The ring covers [highest - K + 1, highest]. An older block is outside
     * the window: do not NACK it, and do not reset it early. The sender's
     * credit is capped to K-2 super-blocks, so this window always covers the
     * block the sender can still be waiting on. */
    if (hi - rx->next_super >= rx->replay_k)
        return;

    if (debug >= 2)
    {
        spin_lock_bh(&rx->lock);
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *db = tbt_fec_rx_blk(rx, g, rx->next_super);

            /* Rate-limited: the walk now polls at the NACK cadence while a
             * super-block is not ready, and four unthrottled lines per poll
             * would evict the delivery trace from the kernel ring buffer. */
            pr_info_ratelimited("tbtun: fec super %llu group %u used=%d ready=%d id=%llu empty=%d nparity=%u ndata=%u\n",
                                rx->next_super, g, db->used, db->ready,
                                db->block_id, db->empty, db->nparity,
                                db->ndata);
        }
        spin_unlock_bh(&rx->lock);
    }

    spin_lock_bh(&rx->lock);
    for (s = rx->next_super; s <= hi; s++)
    {
        bool missing = false;
        bool started = false;
        u64 first_gap = 0;
        u16 nmissing = 0;

        probe_only = true; /* every request so far is a shape probe */
        any_probe = false;

        /* The stream ends at total_pkts: no later share exists, so
         * requesting one would send a NACK every retry interval until the
         * connection closed. The delivery walk already refuses to deliver
         * past the end; stop asking here too. */
        if (READ_ONCE(rx->total_known) &&
            rx->next_deliver >= READ_ONCE(rx->total_pkts))
            break;
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);

            if (b->used && b->block_id == s)
            {
                started = true;
                break;
            }
        }
        /* A super-block ahead of the delivery frontier with no block at all
         * has not started: its shares may simply not be sent yet. Do not
         * request them. The delivery frontier itself is probed even when no
         * share of it arrived: a whole-block loss is invisible to `hi`. */
        if (!started && s != rx->next_super)
            continue;
        memset(&n, 0, sizeof(n));
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, s);
            u16 real;

            if (!b->used || b->block_id != s)
            {
                /* A block that holds only parity is not "unstarted": its
                 * DATA shares are the missing ones. Request them instead of
                 * probing parity 0 again (which is already present). */
                if (tbt_fec_nack_parity_only(b->used, b->nparity, b->block_id,
                                             s, b->ndata, b->ready))
                {
                    u16 dlimit, plimit, seen_hi = 0;

                    missing = true;
                    probe_only = false;
                    if (!b->first_gap_ns)
                        b->first_gap_ns = now;
                    if (!first_gap || b->first_gap_ns < first_gap)
                        first_gap = b->first_gap_ns;
                    real = tbt_fec_blk_real_len(rx, g, b);
                    for (i = b->n; i > 0; i--)
                        if (b->dpresent[i - 1])
                        {
                            seen_hi = i;
                            break;
                        }
                    dlimit = tbt_fec_nack_data_limit(b->win_len_set, real,
                                                     seen_hi);
                    plimit = tbt_fec_nack_parity_limit(
                        b->win_len_set, real, b->n, b->m, block_code);
                    for (i = 0; i < dlimit; i++)
                        if (!b->dpresent[i])
                            tbt_fec_nack_add(sess, rx, &n, g, s, i, now,
                                             hold, &nmissing);
                    for (i = 0; i < plimit; i++)
                        if (!b->ppresent[i])
                            tbt_fec_nack_add(sess, rx, &n, g, s,
                                             (u16)(b->n + i), now, hold,
                                             &nmissing);
                    continue;
                }
                /* Before the stream end is known, a group with no block may
                 * be legitimately empty: a short/sparse block. Wait for the
                 * parity win_len, the empty marker or FIN to state the real
                 * share count. Requesting the whole range here produced a
                 * NACK storm the sender could not serve and left the block
                 * unready until the reset. */
                if (tbt_fec_nack_unstarted_group_base(
                        READ_ONCE(rx->total_pkts), rx->base_pkt,
                        rx->base_block, rx->groups, rx->n, s, g,
                        READ_ONCE(rx->total_known)))
                {
                    /* Total is known and this group has packets in the
                     * block: a wholly missing block is a real loss. A block
                     * code has no present share to prove any DATA slot real,
                     * so ask for parity 0 alone: it states win_len, and the
                     * next pass names the real DATA range. Asking for all n
                     * here named padding on every short block. RLNC has no
                     * cut to learn and keeps its full range. */
                    missing = true;
                    probe_only = false;
                    if (rx->gap_block != s)
                    {
                        rx->gap_block = s;
                        rx->gap_ns = now;
                    }
                    if (!first_gap || rx->gap_ns < first_gap)
                        first_gap = rx->gap_ns;
                    if (!block_code)
                        for (i = 0; i < rx->n; i++)
                            tbt_fec_nack_add(sess, rx, &n, g, s, i, now,
                                             hold, &nmissing);
                    for (i = 0; i < (block_code ? 1 : rx->m); i++)
                        tbt_fec_nack_add(sess, rx, &n, g, s,
                                         (u16)(rx->n + i), now, hold,
                                         &nmissing);
                    continue;
                }
                /* Before total is known, probe only parity 0 of the groups.
                 * A block the sender never built answers nothing; after
                 * TBT_FEC_NACK_PROBE_MAX unanswered probes stop asking and
                 * leave it to the stall timer. Probing an idle frontier
                 * forever asked for the same absent shares 274k times on the
                 * fix22 run. */
                if (rx->probe_block != s)
                {
                    rx->probe_block = s;
                    rx->probe_tries = 0;
                }
                if (!READ_ONCE(rx->total_known) &&
                    tbt_fec_nack_probe_due(now, rx->probe_last_ns) &&
                    tbt_fec_nack_probe_open(hi > s, rx->probe_tries) &&
                    tbt_fec_nack_add(sess, rx, &n, g, s, rx->n, now, hold,
                                     &nmissing))
                {
                    missing = true;
                    any_probe = true;
                    atomic64_inc(&sess->fec_nack_sparse_probe);
                }
                continue;
            }
            if (b->ready)
                continue;
            real = tbt_fec_blk_real_len(rx, g, b);
            if (!real)
                continue;
            /* The decode pass ran before this scan. A block that is still
             * not ready cannot be completed by the shares it already holds:
             * "ndata + nparity >= real" is a count test, not a rank test,
             * and a rank-deficient block was skipped forever (fix25: the
             * frontier never got the parity row it needed, the ACK stopped
             * and the download stalled for 120 s). Request the missing DATA
             * and parity shares; the per-share hold keeps repeats down.
             *
             * A DATA share is named only when it is proven real: the parity's
             * win_len states the cut, or a present share after it proves the
             * block has that many shares. The tail of a short block is zero
             * padding the sender never wrote; naming it bought a round trip
             * and an absent-share miss per share. */
            {
                u16 dlimit, plimit, seen_hi = 0;

                for (i = b->n; i > 0; i--)
                    if (b->dpresent[i - 1])
                    {
                        seen_hi = i;
                        break;
                    }
                dlimit = tbt_fec_nack_data_limit(b->win_len_set, real,
                                                 seen_hi);
                plimit = tbt_fec_nack_parity_limit(
                    b->win_len_set, real, b->n, b->m, block_code);
                missing = true;
                probe_only = false;
                if (!b->first_gap_ns)
                    b->first_gap_ns = now;
                if (!first_gap || b->first_gap_ns < first_gap)
                    first_gap = b->first_gap_ns;
                for (i = 0; i < dlimit; i++)
                    if (!b->dpresent[i])
                        tbt_fec_nack_add(sess, rx, &n, g, s, i, now, hold,
                                         &nmissing);
                for (i = 0; i < plimit; i++)
                    if (!b->ppresent[i])
                        tbt_fec_nack_add(sess, rx, &n, g, s,
                                         (u16)(b->n + i), now, hold,
                                         &nmissing);
            }
        }
        if (!(missing && tbt_fec_bitmap_count(n.share_mask)))
        {
            /* Every missing share of the frontier is inside its hold (or
             * already present). Never move on to a later block: it cannot be
             * delivered before the frontier, and asking for it leaves the
             * ACK stuck (fix26: next=185 was skipped for s=186 563 times).
             * Arm and wait for the hold to expire. */
            if (tbt_fec_nack_frontier_only(s, rx->next_super))
                break;
            continue;
        }
        /* A real request waits one round trip from the first gap: the
         * carrier TCP still holds the share and the block's parity may still
         * decode it. A NACK sent before that returns a replay that is almost
         * always late (field: 338k late shares, 853 ms mean repair). A shape
         * probe has its own pacing and never waits here. */
        if (!probe_only && first_gap &&
            !tbt_fec_nack_first_due(now, first_gap, hold))
        {
            atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                         &sess->fec_nack_filtered_young);
            young = true;
            break; /* later blocks are younger: nothing to send this pass */
        }
        found = true;
        break;
    }
    spin_unlock_bh(&rx->lock);

    if (found && s > rx->next_super)
        pr_info_ratelimited("tbtun: fec NACK skips frontier next=%llu for s=%llu hi=%llu\n",
                            rx->next_super, s, hi);
    if (!found)
    {
        struct tbt_fec_nack last;
        bool started = false;
        bool blocked;
        u64 last_block;
        u16 nmissing = 0;

        if (!rx->nack->valid)
        {
            /* A young block has nothing to ask yet; wake when its hold may
             * have expired instead of waiting for the next share. */
            if (young)
                tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                                msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
            return;
        }
        last_block = rx->nack->block_id;
        if (last_block > rx->next_super)
        {
            /* A NACK for a later block cannot move the ACK. Drop it and let
             * the next scan target the frontier. */
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        if (last_block < rx->next_super)
        {
            /* The block was delivered while its NACK was in flight: nothing
             * in that bitmap needs a replay. */
            atomic64_add(tbt_fec_bitmap_count(rx->nack->share_mask),
                         &sess->fec_nack_filtered_ready);
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        if (READ_ONCE(rx->total_known) &&
            rx->next_deliver >= READ_ONCE(rx->total_pkts))
        {
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        spin_lock_bh(&rx->lock);
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b = tbt_fec_rx_blk(rx, g, rx->next_super);

            if (b->used && b->block_id == rx->next_super)
            {
                started = true;
                break;
            }
        }
        blocked = started &&
                  !tbt_fec_super_ready_locked(rx, rx->next_super);
        if (!blocked)
        {
            spin_unlock_bh(&rx->lock);
            tbt_fec_nack_reset(rx->nack);
            return;
        }
        /* The scan found no fresh missing set, but the blocked super-block
         * is still not ready. Rebuild the previous bitmap from the block's
         * current state: shares that arrived are dropped, and a share whose
         * replay is still inside its round trip is not asked again. */
        memset(&last, 0, sizeof(last));
        for (g = 0; g < rx->groups; g++)
        {
            struct tbt_fec_blk *b;
            bool have;
            u16 shares = rx->shares;
            u16 bn;

            if (!tbt_fec_bitmap_test(rx->nack->group_mask, g))
                continue;
            b = tbt_fec_rx_blk(rx, g, last_block);
            have = b->used && b->block_id == last_block;
            bn = have ? b->n : rx->n;
            for (i = 0; i < shares; i++)
            {
                bool present;
                enum tbt_fec_nack_skip skip;

                if (!tbt_fec_bitmap_test(rx->nack->share_mask, i))
                    continue;
                present = have &&
                          (i < bn ? b->dpresent[i] :
                                    b->ppresent[i - bn]);
                skip = tbt_fec_nack_share_skip(
                    present, now,
                    tbt_fec_req_ns(rx, g, last_block, i), hold);
                if (skip != TBT_FEC_NACK_KEEP)
                {
                    if (skip == TBT_FEC_NACK_SKIP_PRESENT)
                        atomic64_inc(&sess->fec_nack_filtered_dup);
                    else
                        atomic64_inc(&sess->fec_nack_filtered_inflight);
                    continue;
                }
                if (nmissing >= TBT_FEC_NACK_SHARES_MAX)
                    continue;
                tbt_fec_bitmap_set(last.group_mask, g);
                tbt_fec_bitmap_set(last.share_mask, i);
                nmissing++;
            }
        }
        spin_unlock_bh(&rx->lock);
        if (!tbt_fec_bitmap_count(last.share_mask))
        {
            /* Every share arrived or its reply is still in flight. Nothing
             * to put on the wire; the next pass decodes the block or the
             * hold expires. */
            tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                            msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
            return;
        }
        if (tbt_fec_nack_due_at(rx->nack, now, last_block, &last,
                                tbt_fec_retry_ms(rx, last_block)))
        {
            int err = tbt_fec_send_nack(sess, f, last_block, &last);

            if (debug >= 2)
                pr_info("tbtun: fec NACK retry block=%llu next=%llu group=%*phN share=%*phN\n",
                        last_block, rx->next_super, TBT_FEC_BITMAP_BYTES,
                        last.group_mask, TBT_FEC_BITMAP_BYTES,
                        last.share_mask);
            if (!err)
            {
                atomic64_inc(&sess->fec_nack_tx);
                tbt_fec_nack_stamp(rx, last_block, &last, now, false);
                /* The bitmap is a subset of the last sent one; if that one
                 * was only a shape probe, this retry is too. */
                if (!rx->nack_probe_only)
                    tbt_fec_backoff_note(rx, last_block);
                if (!rx->repair_first_ns ||
                    rx->repair_block != last_block)
                {
                    rx->repair_block = last_block;
                    rx->repair_first_ns = now;
                }
            }
            else
                tbt_fec_nack_drop(sess, err, last_block, &last);
        }
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                        msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
        return;
    }
    if (probe_only && rx->nack->valid &&
        now - rx->nack->last_ns < (u64)TBT_FEC_NACK_PROBE_MS * 1000000ULL)
    {
        /* The sender may not have sent this super-block at all: an idle
         * connection would put a probe on the wire every retry interval for
         * as long as it stays open. Pace it; a real repair keeps the retry
         * cadence. */
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                        msecs_to_jiffies(TBT_FEC_RX_POLL_SLOW_MS));
        return;
    }
    if (tbt_fec_nack_due_at(rx->nack, now, s, &n, tbt_fec_retry_ms(rx, s)))
    {
        int err;

        if (debug >= 2)
            pr_info("tbtun: fec NACK tx block=%llu next=%llu hi=%llu group=%*phN share=%*phN\n",
                    s, rx->next_super, hi, TBT_FEC_BITMAP_BYTES,
                    n.group_mask, TBT_FEC_BITMAP_BYTES, n.share_mask);
        err = tbt_fec_send_nack(sess, f, s, &n);
        if (!err)
        {
            atomic64_inc(&sess->fec_nack_tx);
            tbt_fec_nack_stamp(rx, s, &n, now, any_probe);
            rx->nack_probe_only = probe_only;
            /* A shape probe is not a repair request; its own 250 ms pacing
             * already bounds it, and backing off here would delay the first
             * real repair of a block that starts late. */
            if (!probe_only)
                tbt_fec_backoff_note(rx, s);
            if (!rx->repair_first_ns || rx->repair_block != s)
            {
                rx->repair_block = s;
                rx->repair_first_ns = now;
            }
        }
        else
            tbt_fec_nack_drop(sess, err, s, &n);
    }
    /* The NACK itself may be lost; retry while the block is in the window.
     * No total attempt cap: only the window and the carrier TCP lifetime
     * bound the repair. */
    tbt_session_arm(sess, TBT_SW_FEC_RX_POLL,
                    msecs_to_jiffies(TBT_FEC_NACK_MIN_MS));
}

/* Claim one block under rx->lock, decode it outside, commit under the lock.
 * The snapshot is what the codec sees; a share that arrives during the decode
 * is placed into b->data as usual, and the commit only writes slots that are
 * still missing. */
static bool tbt_fec_rx_decode_one(struct tbt_session *sess,
                                  struct tbt_fec_flow *f, u16 group,
                                  u16 slot)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_blk *b = &rx->blk[(size_t)group * rx->slots + slot];
    struct tbt_fec_decode_snap *snap = rx->decode_snap;
    u16 real = 0, i, k;
    bool claimed = false;
    bool sample = false;
    bool short_bad = false;
    bool block_code;
    u16 sample_e = 0, sample_s = 0, sample_real = 0;
    bool sample_first = false;
    u64 now = ktime_get_ns();
    int rc;

    if (!snap)
        return false;
    memset(snap, 0, sizeof(*snap));
    spin_lock_bh(&rx->lock);
    block_code = rx->sclc;
    if (block_code && b->used && !b->ready && !b->empty)
    {
        /* Make sure the block has its own entry; a share may have arrived
         * before the codec was built. The decode waits until it is ready. */
        if (!b->codec_entry)
            tbt_fec_rx_blk_attach_locked(rx, b);
        else if (!b->codec_entry->ready)
        {
            struct tbt_fec_codec_key key;

            tbt_fec_codec_key_fill(&key, &rx->sclc_shape, b->n, b->m,
                                   rx->codec_seed);
            tbt_fec_rx_codec_queue_locked(rx, &key, b->codec_entry);
        }
    }
    /* Only the delivery frontier can use a decoded block.  Decoding a later
     * slot speculatively costs a whole block of CPU and delays the frontier
     * it cannot overtake; the field body ran up to eight 46 ms decodes
     * before it delivered the one block the application was waiting for. */
    if (b->used && !b->ready && !b->decoding && now >= b->decode_next_ns &&
        (!block_code || b->block_id == rx->next_super) &&
        (!block_code ||
         (b->codec_entry && b->codec_entry->ready &&
          b->codec_entry->codec)))
    {
        real = tbt_fec_blk_real_len(rx, group, b);
        tbt_fec_blk_counts(rx, group, b);
        if (b->empty || b->ndata >= real)
        {
            b->ready = true; /* complete without the codec */
        }
        else if (block_code ? tbt_fec_sclc_decode_wake(
                                  real, b->n, b->m, rx->groups, b->ndata,
                                  b->nparity)
                            : b->nparity >= (u16)(real - b->ndata))
        {
            /* An SC-LDPC block codec attempts as soon as its share count
             * can cover the block; the RLNC path needs one row per missing
             * share. */
            if (block_code && !b->wake_ns)
            {
                /* Snapshot the share counts that first crossed the wake
                 * line, before this attempt changes the block. */
                b->wake_ns = now;
                b->wake_ndata = b->ndata;
                b->wake_nparity = b->nparity;
                b->wake_real = real;
            }
            /* The codec runs outside rx->lock and carriers keep placing
             * shares into this block. Copy every input the decode and the
             * commit need, plus the generation that invalidates them. */
            snap->block_id = b->block_id;
            snap->seq0 = b->seq0;
            snap->gen = b->gen;
            snap->n = b->n;
            snap->m = b->m;
            snap->real = real;
            snap->short_len = b->short_len;
            snap->short_unknown = b->short_unknown;
            snap->codec_entry = b->codec_entry;
            snap->data = b->data;
            snap->parity = b->parity;
            memcpy(snap->dlen, b->dlen, (size_t)b->n * sizeof(b->dlen[0]));
            memcpy(snap->dpresent, b->dpresent, b->n);
            memcpy(snap->ppresent, b->ppresent, b->m);
            for (i = real; i < b->n; i++)
                snap->dpresent[i] = 1; /* padding is known zero */
            b->decoding = true;
            b->wake_attempts++;
            claimed = true;
        }
    }
    spin_unlock_bh(&rx->lock);
    if (!claimed)
        return false;

    rc = tbt_fec_blk_decode(sess, f, group, snap, &short_bad);
    spin_lock_bh(&rx->lock);
    b->decoding = false;
    if (READ_ONCE(rx->stopping))
    {
        u8 *stale = b->buf;

        b->buf = NULL;
        b->data = NULL;
        b->parity = NULL;
        b->used = false;
        b->ready = false;
        spin_unlock_bh(&rx->lock);
        kvfree(stale);
        return false;
    }
    /* A share placed while the codec ran, or a slot reused for another
     * block, makes this result describe an older state. Discard it without
     * touching the block; the placement path already cleared the retry
     * deadline, so the next pass snapshots the new state. */
    if (!tbt_fec_rx_snap_current(b->used, snap->block_id, snap->gen,
                                 b->block_id, b->gen))
    {
        spin_unlock_bh(&rx->lock);
        return false;
    }
    if (short_bad && !b->short_unknown)
    {
        b->short_unknown = true;
        b->gen++;
        atomic64_inc(&sess->fec_short_unknown);
    }
    if (rc == 0)
    {
        struct tbt_fec_grouped *codec =
            snap->codec_entry ? snap->codec_entry->codec : rx->codec;
        bool commit_block_code = block_code;
        u8 fail[TBT_FEC_BITMAP_BYTES];

        memset(fail, 0, sizeof(fail));
        for (i = 0, k = 0; i < snap->real; i++)
        {
            if (snap->dpresent[i])
                continue;
            /* Belt and braces for a block codec: copy a recovered slot only
             * when the codec says that share is complete. A partial decode
             * must not turn stale rx->rec bytes into delivered data. */
            if (commit_block_code &&
                !tbt_fec_grouped_is_known(codec, group, i))
            {
                tbt_fec_adapt_bitmap_set(fail, i);
                k++;
                continue;
            }
            if (!b->dpresent[i])
            {
                memcpy(b->data + (size_t)i * rx->s,
                       rx->rec + (size_t)k * rx->s, rx->s);
                b->dpresent[i] = 1;
                b->dlen[i] = 0; /* a recovered share has no own length */
                b->ndata++;
                /* Parity recovered this DATA share: it is a repair, the
                 * estimator's miss evidence for this block. */
                if (i < 32)
                    b->miss_mask |= 1u << i;
                tbt_fec_adapt_bitmap_set(b->miss_bits, i);
            }
            k++;
        }
        memcpy(b->codec_fail_bits, fail, sizeof(fail));
        /* Ready only when every real share is present; a skipped share keeps
         * the block decodable on the next pass. */
        b->ready = b->ndata >= snap->real;
        b->gen++;
    }
    if (b->ready)
    {
        b->decode_tries = 0;
        b->decode_next_ns = 0;
    }
    else
    {
        /* Exponential backoff, 50 ms doubling to 400 ms. A share that
         * arrives after this attempt is new evidence and clears this
         * deadline in the placement path, so the next pass retries at
         * once. */
        u64 wait = (u64)tbt_fec_decode_backoff_ms(b->decode_tries) *
                   NSEC_PER_MSEC;
        u64 deadline;

        if (b->decode_tries < 0xff)
            b->decode_tries++;
        deadline = ktime_get_ns() + wait;
        if (b->decode_next_ns < deadline)
            b->decode_next_ns = deadline;
        atomic64_inc(&sess->fec_decode_backoff);
    }
    if (b->wake_attempts == 1 && !b->wake_sampled)
    {
        b->wake_first_ok = (u8)(rc == 0 && b->ready);
        b->wake_sampled = 1;
        sample_e = b->wake_real > b->wake_ndata
                       ? (u16)(b->wake_real - b->wake_ndata)
                       : 0;
        sample_s = (u16)(b->wake_ndata + b->wake_nparity - b->wake_real);
        sample_real = b->wake_real;
        sample_first = b->wake_first_ok != 0;
        sample = true;
    }
    b->decoding = false;
    spin_unlock_bh(&rx->lock);
    if (sample && rx->ctl.ready)
    {
        /* The wake line used this block's own n (a switch may have moved
         * the controller's initial extra since INIT). Rebase S onto the
         * controller's basis so its surplus correction compares like with
         * like: (extra + 1) - S must name the same margin the block woke
         * on. */
        u16 extra = rx->groups > 1 ? TBT_FEC_SCLC_WAKE_EXTRA_UP
                                   : tbt_fec_sclc_wake_extra_down(snap->n);

        if (sample_s >= extra)
            sample_s = (u16)(sample_s + rx->ctl.extra - extra);
        tbt_fec_adapt_sclc_block_sample(&rx->ctl, sample_e, sample_s,
                                        sample_first, sample_real);
    }
    return rc == 0;
}

/* ------------------------------------------------------------------ *
 * RX codec cache
 *
 * A block holds its entry from placement to slot reuse. The build queue
 * holds one extra reference per pending entry, so a build that finishes
 * after the block is gone still writes to a live slot. Every cache call
 * runs under rx->lock; the build itself runs outside it in w_fec_rx.
 * ------------------------------------------------------------------ */
static bool tbt_fec_rx_build_queued(const struct tbt_fec_rx *rx,
                                    const struct tbt_fec_codec_entry *e)
{
    u8 i;

    for (i = 0; i < rx->build_q_n; i++)
        if (rx->build_q[i] == e)
            return true;
    return false;
}

static void tbt_fec_rx_codec_queue_locked(struct tbt_fec_rx *rx,
                                          const struct tbt_fec_codec_key *key,
                                          struct tbt_fec_codec_entry *e)
{
    struct tbt_fec_codec_entry *q;
    bool ready = false;

    if (!e || e->ready || tbt_fec_rx_build_queued(rx, e))
        return;
    if (rx->build_q_n >= ARRAY_SIZE(rx->build_q))
        return;
    if (ktime_get_ns() < rx->codec_next_ns)
        return;
    q = tbt_fec_codec_cache_get(rx->codec_cache, key, &ready);
    if (!q)
        return;
    if (q != e || ready)
    {
        tbt_fec_codec_cache_put(q);
        return;
    }
    rx->build_q[rx->build_q_n++] = q;
}

static void tbt_fec_rx_codec_publish_locked(struct tbt_fec_rx *rx,
                                            struct tbt_fec_codec_entry *e,
                                            u64 block_id)
{
    struct tbt_fec_codec_entry *pin;
    bool ready = false;
    u16 shares;

    if (!e || !e->ready || !e->codec)
        return;
    /* An out-of-order old block must not drag rx->n/m (and the FBK
     * n_current/m_current) back to its retired shape. Only a block at or
     * beyond the delivery frontier may publish the session view. */
    if (block_id < READ_ONCE(rx->next_super))
        return;
    shares = (u16)(e->key.n + e->key.m);
    pin = tbt_fec_codec_cache_get(rx->codec_cache, &e->key, &ready);
    if (pin && ready && pin != rx->codec_entry)
    {
        struct tbt_fec_codec_entry *old = rx->codec_entry;

        rx->codec_entry = pin;
        if (old)
            tbt_fec_codec_cache_put(old);
    }
    else if (pin)
        tbt_fec_codec_cache_put(pin);
    rx->n = e->key.n;
    rx->m = e->key.m;
    rx->shape.n = e->key.n;
    rx->shape.m = e->key.m;
    rx->shape.codec_variant = 2;
    rx->shape.L = e->key.L;
    rx->shape.block = e->key.block;
    rx->shape.rate_ppm =
        (u32)((((u64)e->key.n * 1000000u) + shares - 1u) / shares);
    /* The switch is effective once a block of the new shape is attached.
     * The target pin is no longer needed; dropping it here keeps one cache
     * slot free for the next target change. Blocks keep their own entry
     * references, so the codec stays alive until they are done. */
    if (rx->target_entry && rx->target_entry->key.n == e->key.n &&
        rx->target_entry->key.m == e->key.m)
    {
        struct tbt_fec_codec_entry *old = rx->target_entry;

        rx->target_entry = NULL;
        tbt_fec_codec_cache_put(old);
    }
}

static void tbt_fec_rx_blk_attach_locked(struct tbt_fec_rx *rx,
                                         struct tbt_fec_blk *b)
{
    struct tbt_fec_codec_key key;
    struct tbt_fec_codec_entry *e;
    bool ready = false;

    if (!rx->sclc || !b->n || !b->m || b->codec_entry)
        return;
    tbt_fec_codec_key_fill(&key, &rx->sclc_shape, b->n, b->m,
                           rx->codec_seed);
    e = tbt_fec_codec_cache_get(rx->codec_cache, &key, &ready);
    if (!e)
        return;
    b->codec_entry = e;
    if (ready)
        tbt_fec_rx_codec_publish_locked(rx, e, b->block_id);
    else
        tbt_fec_rx_codec_queue_locked(rx, &key, e);
}

/* Build every queued entry. Runs in w_fec_rx: the build may sleep, while the
 * cache calls stay under rx->lock. A failure backs off before the next
 * attempt and keeps the current shape and m_target. */
static void tbt_fec_rx_build_pending(struct tbt_fec_rx *rx)
{
    struct tbt_fec_codec_entry *e;
    struct tbt_fec_grouped *codec;
    u64 now;

    for (;;)
    {
        spin_lock_bh(&rx->lock);
        if (!rx->build_q_n)
        {
            spin_unlock_bh(&rx->lock);
            break;
        }
        e = rx->build_q[0];
        rx->build_q_n--;
        memmove(&rx->build_q[0], &rx->build_q[1],
                (size_t)rx->build_q_n * sizeof(rx->build_q[0]));
        now = ktime_get_ns();
        if (e->ready || now < rx->codec_next_ns)
        {
            if (!e->ready && rx->build_q_n < ARRAY_SIZE(rx->build_q))
                rx->build_q[rx->build_q_n++] = e;
            else
                tbt_fec_codec_cache_put(e);
            spin_unlock_bh(&rx->lock);
            break;
        }
        spin_unlock_bh(&rx->lock);

        codec = tbt_fec_codec_build(&rx->sclc_shape, e->key.n, e->key.m,
                                    rx->groups, e->key.seed);
        spin_lock_bh(&rx->lock);
        tbt_fec_codec_cache_build_done(e, codec, codec != NULL);
        if (!codec)
        {
            rx->codec_build_fail++;
            rx->codec_backoff = rx->codec_backoff
                                    ? (u8)min_t(u32, rx->codec_backoff * 2u,
                                                1000u)
                                    : 50u;
            rx->codec_next_ns =
                now + (u64)rx->codec_backoff * NSEC_PER_MSEC;
        }
        else
        {
            rx->codec_backoff = 0;
            rx->codec_next_ns = 0;
            /* A build completion publishes the codec entry but must not
             * change the session n/m; the frontier block does that when it
             * attaches. */
            (void)e;
        }
        tbt_fec_codec_cache_put(e);
        spin_unlock_bh(&rx->lock);
        if (!codec)
            break; /* honour the backoff before the next build */
    }
}

/* One FEC_FBK report. Control records are plaintext and ride the control
 * queue; the sender's control law consumes the fields. `extra` carries the
 * handshake flags (CFG_ACK / SWITCH_DONE / ROLLBACK / CFG_NACK). */
static bool tbt_fec_rx_sc_mode(const struct tbt_fec_rx *rx)
{
    return rx->sclc && rx->est_sclc;
}

static u64 tbt_fec_rx_est_activity(const struct tbt_fec_rx *rx)
{
    if (tbt_fec_rx_sc_mode(rx))
        return rx->est_sclc->last_activity_ns;
    return rx->est ? rx->est->last_activity_ns : 0;
}

/* Sample whichever estimator owns this shape. The RLNC-shaped out is the
 * common view the c13 snapshot already consumes; the SC out carries the
 * p_fast/codec_fail values the FBK overlay needs. */
static void tbt_fec_rx_sample(struct tbt_fec_rx *rx, u64 now,
                              struct tbt_fec_est_out *o,
                              struct tbt_fec_adapt_sclc_est_out *so)
{
    if (tbt_fec_rx_sc_mode(rx))
    {
        spin_lock_bh(&rx->lock);
        tbt_fec_adapt_sclc_est_sample(rx->est_sclc, now, so);
        spin_unlock_bh(&rx->lock);
        memset(o, 0, sizeof(*o));
        o->p_ppm = so->p_ppm;
        o->p_ewma_ppm = so->p_ewma_ppm;
        o->p_win_ppm = so->p_win_ppm;
        o->r_fail_ppm = so->r_fail_ppm;
        o->sent_win = so->sent_win;
        o->miss_win = so->miss_win;
        o->supers_win = so->supers_win;
        o->abandoned_win = so->abandoned_win;
        o->unknown_win = so->unknown_win;
        o->requested_win = so->requested_win;
        o->pkts_win = so->pkts_win;
        o->abandoned_total = so->abandoned_total;
        o->unknown_total = so->unknown_total;
        o->alive = so->alive;
        o->stalled = so->stalled;
        o->carriers = so->carriers;
        o->p_valid = so->p_valid;
        o->r_valid = so->r_valid;
        o->pause_idle = so->pause_idle;
        o->pause_carrier = so->pause_carrier;
        o->pause_abandoned = so->pause_abandoned;
    }
    else
    {
        memset(so, 0, sizeof(*so));
        if (rx->est)
            tbt_fec_est_sample(rx->est, now, o);
        else
            memset(o, 0, sizeof(*o));
    }
}

static void tbt_fec_rx_fbk_send(struct tbt_session *sess,
                                struct tbt_fec_flow *flow, u8 extra)
{
    struct tbt_fec_rx *rx = flow->rx;
    struct tbt_fec_est_out o;
    struct tbt_fec_adapt_sclc_est_out so;
    struct tbt_fec_fbk f;
    struct tbt_fec_hdr h;
    struct tbt_fec_shape suggest;
    u8 body[TBT_FEC_FBK_LEN];
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_FBK_LEN];
    u8 alive_snap[TBT_FEC_BITMAP_BYTES];
    u16 len;
    u64 now = ktime_get_ns();
    u32 expected;
    bool sc;

    if (!rx || (!rx->est && !rx->est_sclc))
        return;
    sc = tbt_fec_rx_sc_mode(rx);
    tbt_fec_rx_sample(rx, now, &o, &so);
    spin_lock_bh(&rx->lock);
    memcpy(alive_snap, rx->alive_bm, sizeof(alive_snap));
    spin_unlock_bh(&rx->lock);
    memset(&f, 0, sizeof(f));
    tbt_fec_fbk_set_epoch(&f, rx->fbk_epoch ? rx->fbk_epoch : rx->epoch);
    rx->fbk_epoch = 0;
    if (sc)
    {
        /* v8 keeps m_current/m_target as parity share counts. m_target moves
         * only after the target codec is ready; until then it names the
         * current m so the sender keeps its shape. */
        u16 shares = (u16)(rx->n + rx->m);
        u16 tn = rx->m_target < shares ? (u16)(shares - rx->m_target) : 0;

        f.m_current = (u8)rx->m;
        f.m_target = (u8)rx->m_target;
        f.rate_ppm_current = READ_ONCE(rx->shape.rate_ppm);
        f.rate_ppm_target = tn
                                ? (u32)((((u64)tn * 1000000u) + shares - 1u) /
                                        shares)
                                : f.rate_ppm_current;
    }
    else
    {
        f.m_current = (u8)rx->m;
        tbt_fec_adapt_shape(tbt_fec_adapt_class_of_p(o.p_ppm), &suggest);
        f.m_target = (u8)suggest.m;
    }
    /* The field keeps its 0x1006 name, but the control law consumes the same
     * p_used the receiver used: max(EWMA, 1 s window). */
    f.p_ewma_ppm = o.p_ppm;
    expected = o.sent_win;
    f.expected_shares = expected > 0xffffu ? 0xffffu : (u16)expected;
    f.received_shares =
        (u16)(f.expected_shares -
              (o.miss_win > f.expected_shares ? f.expected_shares
                                              : o.miss_win));
    f.n_current = (u8)rx->n;
    if (o.p_valid && !o.pause_idle && !o.pause_carrier &&
        !o.pause_abandoned)
        extra |= TBT_FEC_FBK_F_P_VALID;
    f.flags = extra;
    memcpy(f.carrier_bitmap, alive_snap, TBT_FEC_BITMAP_BYTES);
    f.codec_variant = READ_ONCE(rx->shape.codec_variant);
    f.L = READ_ONCE(rx->shape.L);
    f.block = READ_ONCE(rx->shape.block);
    if (sc)
    {
        /* p_fast moved to the extension; codec_fail stays local (procfs). */
        f.p_fast_ppm = so.p_fast_ppm;
    }
    if (tbt_fec_fbk_pack(&f, body))
        return;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_FBK;
    h.payload_len = TBT_FEC_FBK_LEN;
    h.conv = TBT_FEC_CONV;
    h.sid = flow->sid;
    if (tbt_fec_make_record(sess, &h, body, rec, &len))
        return;
    {
        int err = tbt_fec_send_record(sess, 0, rec, len, true, false,
                                      flow);

        if (!err)
            rx->fbk_last_ns = now;
    }
}

static void tbt_fec_rx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_flow *f);

/* Refresh the alive bitmap, fold the abandoned delta, run the continuous
 * controller once per feedback period and publish the c13 snapshot. Called
 * only from w_fec_rx, the controller's single writer. */
static void tbt_fec_rx_adapt_tick(struct tbt_session *sess,
                                  struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_adapt_ctl *a = &sess->adapt;
    struct tbt_fec_est_out o;
    struct tbt_fec_adapt_sclc_est_out so;
    struct tbt_fec_adapt_sclc_out out;
    u64 now = ktime_get_ns();
    u64 abandoned;
    u16 i, alive = 0, stalled = 0, d;
    u8 flags;
    u8 alive_bm[TBT_FEC_BITMAP_BYTES];
    bool paused, sc;

    if (!rx || (!rx->est && !rx->est_sclc) || READ_ONCE(rx->stopping) ||
        READ_ONCE(sess->closing))
        return;
    sc = tbt_fec_rx_sc_mode(rx);
    /* INIT is not a switch: it installs the peer's asymmetric shape (the
     * downlink 1x256 and uplink 8x32 differ) before the first share can be
     * placed. */
    if (rx->init_pending)
        tbt_fec_rx_init_tick(sess, f);
    if (!READ_ONCE(sess->adaptive))
        return;
    if (rx->build_q_n)
        tbt_fec_rx_build_pending(rx);
    memset(alive_bm, 0, sizeof(alive_bm));
    for (i = 0; i < sess->n_car; i++)
    {
        struct tbt_carrier *c = tbt_carrier_get(sess, (u8)i);

        if (!c)
            continue;
        if (tbt_fec_adapt_carrier_alive(atomic_read(&c->up),
                                        atomic_read(&c->stopping),
                                        atomic_read(&c->connected), now,
                                        READ_ONCE(c->last_rx_ns),
                                        READ_ONCE(c->rtt_us)))
        {
            tbt_fec_adapt_bitmap_set(alive_bm, i);
            alive++;
        }
        if (READ_ONCE(c->stalled))
            stalled++;
        tbt_carrier_put(c);
    }
    abandoned = (u64)atomic64_read(&rx->abandoned_shares);
    spin_lock_bh(&rx->lock);
    memcpy(rx->alive_bm, alive_bm, sizeof(rx->alive_bm));
    if (rx->est)
        tbt_fec_est_carriers(rx->est, alive, stalled, sess->n_car);
    if (rx->est_sclc)
        tbt_fec_adapt_sclc_est_carriers(rx->est_sclc, alive, stalled,
                                        sess->n_car);
    if (abandoned > rx->est_abandoned_last)
    {
        if (rx->est)
            tbt_fec_est_abandoned(rx->est, now,
                                  (u32)(abandoned -
                                        rx->est_abandoned_last));
        if (rx->est_sclc)
            tbt_fec_adapt_sclc_est_abandoned(
                rx->est_sclc, now,
                (u32)(abandoned - rx->est_abandoned_last));
        rx->est_abandoned_last = abandoned;
    }
    spin_unlock_bh(&rx->lock);
    tbt_fec_rx_sample(rx, now, &o, &so);

    /* A receive-idle side must not overwrite the live TX view with its own
     * zero-window estimate; the TX side publishes the peer FBK below. */
    if (o.pause_idle && !o.sent_win && f->tx && f->tx->peer_est_valid)
        return;
    a->epoch = rx->epoch;
    a->n = rx->n;
    a->m = rx->m;
    a->groups = rx->groups;
    if (sc)
        a->class = tbt_fec_adapt_sclc_rate_pct(rx->n, rx->m);
    else
    {
        a->class = tbt_fec_adapt_class_of_shape(rx->n, rx->m);
        if (a->class == 0xff)
            a->class = 3;
    }
    a->p_ppm = o.p_ppm;
    a->p_win_ppm = o.p_win_ppm;
    a->r_fail_ppm = 0; /* the sender measures the residual */
    a->sent = o.sent_win;
    a->miss = o.miss_win;
    a->sample_unknown = o.unknown_total;
    a->abandoned = o.abandoned_total;

    paused = o.pause_idle || o.pause_carrier || o.pause_abandoned;
    if (paused && !rx->est_paused)
    {
        rx->est_paused = true;
        rx->pause_start_ns = now;
        a->pauses++;
    }
    else if (!paused && rx->est_paused)
    {
        rx->est_paused = false;
        a->pause_ns += now - rx->pause_start_ns;
    }
    a->state = TBT_FEC_ADAPT_STEADY;
    if (o.pause_idle)
        a->reason = TBT_FEC_ADAPT_CTL_R_IDLE;
    else if (o.pause_carrier)
        a->reason = TBT_FEC_ADAPT_CTL_R_CARRIER;
    else if (o.pause_abandoned)
        a->reason = TBT_FEC_ADAPT_CTL_R_ABANDONED;
    else if (!o.p_valid)
        a->reason = TBT_FEC_ADAPT_CTL_R_SAMPLE;
    else
        a->reason = TBT_FEC_ADAPT_CTL_R_NONE;

    /* One control tick per feedback period. d is the blocks observed since
     * the previous tick (the feedback loop's block delay); w_ppm is W/T_b
     * with the same block count as the window. */
    if (sc && rx->ctl.ready &&
        (!rx->tick_last_ns || now - rx->tick_last_ns >= 100000000ULL))
    {
        u32 blocks = rx->ctl.blocks - rx->tick_last_blocks;
        u32 ab = abandoned > rx->tick_abandoned_last
                     ? (u32)(abandoned - rx->tick_abandoned_last)
                     : 0;
        u32 w_ppm;

        d = blocks ? (u16)min_t(u32, blocks, 0xffffu) : 1u;
        w_ppm = (u32)d * 1000000u;
        if (w_ppm > 10000000u)
            w_ppm = 10000000u;
        tbt_fec_adapt_sclc_tick(&rx->ctl, now, d, w_ppm, alive, stalled,
                                ab, &out);
        rx->tick_last_ns = now;
        rx->tick_last_blocks = rx->ctl.blocks;
        rx->tick_abandoned_last = abandoned;

        /* The peer may see a new m_target only after the target codec is
         * ready. A failed build keeps the old target and backs off. */
        if (out.m_target && out.m_target != rx->m_target)
        {
            struct tbt_fec_codec_key key;
            struct tbt_fec_codec_entry *e;
            bool ready = false;
            u16 shares = (u16)(rx->n + rx->m);

            if (out.m_target < shares)
            {
                u16 tn = (u16)(shares - out.m_target);

                if (tbt_fec_adapt_sclc_buildable(rx->sclc_shape.variant, tn,
                                                 out.m_target))
                {
                    tbt_fec_codec_key_fill(&key, &rx->sclc_shape, tn,
                                           out.m_target, rx->codec_seed);
                    spin_lock_bh(&rx->lock);
                    e = tbt_fec_codec_cache_get(rx->codec_cache, &key,
                                                &ready);
                    if (e && !ready)
                        tbt_fec_rx_codec_queue_locked(rx, &key, e);
                    spin_unlock_bh(&rx->lock);
                    if (e && ready)
                    {
                        struct tbt_fec_codec_entry *old = rx->target_entry;

                        rx->target_entry = e;
                        rx->m_target = out.m_target;
                        if (old)
                            tbt_fec_codec_cache_put(old);
                    }
                    else if (e)
                        tbt_fec_codec_cache_put(e);
                }
            }
            if (rx->build_q_n)
                tbt_fec_rx_build_pending(rx);
            /* A build that just finished publishes on the next pass. */
            if (rx->target_entry && rx->target_entry->ready)
                rx->m_target = rx->target_entry->key.m;
        }
        a->m_target = rx->m_target;
        a->q_raw_q16 = out.q_raw_q16;
        a->q_q16 = out.q_q16;
        a->delta_q16 = out.delta_q16;
        a->delta_off_q16 = out.delta_off_q16;
        a->burst_q16 = out.burst_q16;
        a->eps_q16 = out.eps_q16;
        a->sigma_q16 = out.sigma_q16;
        a->backup = out.backup;
        a->ctl_reason = out.reason;
        a->frozen = out.frozen;
    }

    flags = rx->fbk_flags;
    rx->fbk_flags = 0;
    {
        u64 activity = tbt_fec_rx_est_activity(rx);

        if (flags ||
            (activity && now - activity < 1000000000ULL &&
             now - rx->fbk_last_ns >= 100000000ULL))
            tbt_fec_rx_fbk_send(sess, f, flags);
    }
}

/* Free one block array allocated by tbt_fec_rx_blk_alloc(). */
static void tbt_fec_rx_blk_free(struct tbt_fec_blk *blk, u32 total)
{
    u32 i;

    if (!blk)
        return;
    for (i = 0; i < total; i++)
    {
        if (blk[i].codec_entry)
        {
            tbt_fec_codec_cache_put(blk[i].codec_entry);
            blk[i].codec_entry = NULL;
        }
        kvfree(blk[i].buf);
        kvfree(blk[i].dpresent);
        kvfree(blk[i].ppresent);
        kvfree(blk[i].dlen);
    }
    kvfree(blk);
}

/* `total` block slots of `shares` shares each. Only the small metadata is
 * eager; a slot's (n+m)*S buffer is allocated on its first share. Returns
 * NULL on the first failure and releases what it took. */
static struct tbt_fec_blk *tbt_fec_rx_blk_alloc(u32 total, u16 shares, u16 n)
{
    struct tbt_fec_blk *blk = tbt_fec_vzalloc((size_t)total * sizeof(*blk));
    u32 i;

    (void)shares;
    (void)n;
    if (!blk)
        return NULL;
    for (i = 0; i < total; i++)
    {
        struct tbt_fec_blk *b = &blk[i];

        b->dpresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->ppresent = kvzalloc(TBT_FEC_MAX_SHARES, GFP_KERNEL);
        b->dlen = kvzalloc(TBT_FEC_MAX_SHARES * sizeof(u16), GFP_KERNEL);
        if (!b->dpresent || !b->ppresent || !b->dlen)
        {
            tbt_fec_rx_blk_free(blk, total);
            return NULL;
        }
        tbt_fec_blk_slice(NULL, b);
    }
    return blk;
}

/* Install the peer's initial shape (TBT_FEC_CFG_F_INIT). This is the only
 * path that changes the receive geometry outside a switch, and it is accepted
 * only before the first share: a carrier may be placing a share under
 * rx->lock, so the pointer swap happens there and the old arena is freed
 * after the unlock. Runs in w_fec_rx (process context) because it allocates.
 *
 * The peer's L is not on the wire; the local session's L is the only source.
 * Both ends must configure the same codec, so a mismatch is a configuration
 * error the caller reports rather than a shape this path can detect. */
/* Scratch slots for recovered DATA shares. A block may use any buildable
 * split, so size for the largest n the direction can build once, at INIT. */
static u16 tbt_fec_sclc_rec_slots(const struct tbt_fec_cfg *cfg, u16 shares)
{
    u16 side = cfg->n > cfg->m ? cfg->n : cfg->m;
    u16 max_n = cfg->groups > 1 ? 30u : 192u;

    (void)shares;
    return max_n > side ? max_n : side;
}

static int tbt_fec_rx_apply_init(struct tbt_session *sess,
                                 struct tbt_fec_flow *f,
                                 const struct tbt_fec_cfg *cfg)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_sclc_shape sclc;
    struct tbt_fec_codec_entry *entry = NULL, *old_entry;
    struct tbt_fec_codec_entry *old_target;
    struct tbt_fec_codec_entry *old_build_q[TBT_FEC_CODEC_CACHE_MAX];
    u8 old_build_q_n;
    struct tbt_fec_codec_cache *new_cache = NULL, *old_cache;
    struct tbt_fec_grouped *codec, *old_codec;
    struct tbt_fec_blk *blk, *old_blk;
    u64 *req_ns, *req_block, *old_req_ns, *old_req_block;
    u8 *rec, *old_rec;
    u8 *old_pool;
    u32 slots, total, old_total;
    u16 shares = (u16)(cfg->n + cfg->m);
    u16 extra;
    bool same, fresh, block_code = false;

    if (!rx)
        return -EINVAL;
    /* On the far side the flow mask came from the peer's OPEN, so the INIT
     * shape must name the same peer group count. The near side's flow mask is
     * its own TX partition, not this RX direction. */
    if (sess->fec_sclc && sess->tunnel->cfg.role == TBT_ROLE_FAR &&
        !tbt_fec_open_groups_match(true, f->rx->group_mask, cfg->groups))
        return -EINVAL;
    /* The shape names every share the session's carriers carry: groups is
     * carriers/(n+m), checked against the actual carrier count rather than
     * a fixed 256. */
    if ((u32)cfg->groups * shares != sess->n_car)
        return -EINVAL;
    memset(&sclc, 0, sizeof(sclc));
    spin_lock_bh(&rx->lock);
    same = rx->n == cfg->n && rx->m == cfg->m &&
           rx->groups == cfg->groups && rx->shape.w == cfg->w &&
           rx->shape.k == cfg->k && rx->shape.d == cfg->d &&
           rx->shape.codec_variant == cfg->codec_variant &&
           rx->shape.L == cfg->L && rx->shape.block == cfg->block &&
           rx->shape.rate_ppm == cfg->rate_ppm;
    fresh = rx->next_deliver == 0 && rx->next_super == 0 &&
            rx->highest_block == 0 && rx->epoch == 0;
    spin_unlock_bh(&rx->lock);
    if (same)
        return 0;
    if (!fresh)
    {
        atomic64_inc(&sess->fec_epoch_bad);
        return -EBUSY;
    }

    /* Every graph parameter comes from the announced shape, including L:
     * this side's own L describes its transmit codec, and the two directions
     * legitimately differ (downlink L 8, uplink L 16). */
    if (cfg->codec_variant == 2)
    {
        struct tbt_fec_codec_key key;
        bool ready = false;

        if (tbt_fec_sclc_shape_of_cfg(cfg, (u16)(cfg->groups * shares),
                                      &sclc))
            return -EINVAL;
        /* Start the direction's cache at its real capacity in a fresh
         * object. The pointer swap happens under rx->lock; blocks that still
         * hold an old entry keep its codec alive until their own put(). */
        new_cache = tbt_fec_codec_cache_new_obj(sclc.variant);
        if (!new_cache)
            return -ENOMEM;
        tbt_fec_codec_key_fill(&key, &sclc, cfg->n, cfg->m, cfg->seed);
        entry = tbt_fec_codec_cache_get(new_cache, &key, &ready);
        if (!entry)
            goto fail;
        if (!ready)
        {
            codec = tbt_fec_codec_build(&sclc, cfg->n, cfg->m,
                                        cfg->groups, cfg->seed);
            tbt_fec_codec_cache_build_done(entry, codec, codec != NULL);
            if (!codec)
                goto fail;
        }
        else
            codec = entry->codec;
        block_code = true;
    }
    else if (sess->fec_sclc)
    {
        /* Mode 3 has no RLNC fallback. A body that is not a buildable
         * SC-LDPC shape is a permanent profile mismatch. */
        return -EINVAL;
    }
    else
    {
        codec = tbt_fec_grouped_new_ops(&tbt_fec_ops_rlnc, cfg->groups,
                                        cfg->n, cfg->m, cfg->k, cfg->w,
                                        TBT_FEC_S, cfg->d, cfg->seed);
        if (!codec)
            return -ENOMEM;
    }

    slots = TBT_FEC_RX_SLOTS;
    if (slots < 4)
        slots = 4;
    if (slots > 256)
        slots = 256;
    total = (u32)cfg->groups * slots;
    blk = tbt_fec_rx_blk_alloc(total, shares, cfg->n);
    req_ns = kvzalloc((size_t)total * shares * sizeof(*req_ns), GFP_KERNEL);
    req_block = kvzalloc((size_t)total * sizeof(*req_block), GFP_KERNEL);
    {
        u16 slots_per_share = cfg->n > cfg->m ? cfg->n : cfg->m;

        if (sess->fec_sclc)
            slots_per_share = tbt_fec_sclc_rec_slots(cfg, shares);
        if (slots_per_share < TBT_FEC_ADAPT_CLASSES)
            slots_per_share = TBT_FEC_ADAPT_CLASSES;
        rec = tbt_fec_vzalloc((size_t)slots_per_share * TBT_FEC_S);
    }
    if (!blk || !req_ns || !req_block || !rec)
    {
        tbt_fec_rx_blk_free(blk, total);
        kvfree(req_ns);
        kvfree(req_block);
        kvfree(rec);
        goto fail;
    }

    extra = block_code
                ? (cfg->groups > 1 ? TBT_FEC_SCLC_WAKE_EXTRA_UP
                                   : tbt_fec_sclc_wake_extra_down(cfg->n))
                : 0;
    if (block_code &&
        tbt_fec_adapt_sclc_init(&rx->ctl, shares, cfg->groups, extra,
                                cfg->n, sclc.variant))
    {
        tbt_fec_rx_blk_free(blk, total);
        kvfree(req_ns);
        kvfree(req_block);
        kvfree(rec);
        goto fail;
    }

    spin_lock_bh(&rx->lock);
    old_blk = rx->blk;
    old_req_ns = rx->req_ns;
    old_req_block = rx->req_block;
    old_rec = rx->rec;
    old_codec = rx->codec;
    old_entry = rx->codec_entry;
    old_target = rx->target_entry;
    old_build_q_n = rx->build_q_n;
    memcpy(old_build_q, rx->build_q,
           (size_t)old_build_q_n * sizeof(old_build_q[0]));
    old_pool = rx->buf_pool;
    old_total = (u32)rx->groups * rx->slots;
    old_cache = rx->codec_cache;
    rx->n = cfg->n;
    rx->m = cfg->m;
    rx->shares = shares;
    rx->groups = cfg->groups;
    rx->slots = (u16)slots;
    rx->buf_pool = NULL;
    rx->buf_pool_n = 0;
    rx->buf_bytes = (u32)shares * TBT_FEC_S;
    if (block_code)
    {
        u16 g;

        memset(rx->group_mask, 0, TBT_FEC_BITMAP_BYTES);
        for (g = 0; g < cfg->groups && g < TBT_FEC_MAX_GROUPS; g++)
            tbt_fec_bitmap_set(rx->group_mask, g);
        tbt_fec_group_map_build(rx->group_mask, TBT_FEC_MAX_GROUPS,
                                rx->group_map);
        rx->codec_cache = new_cache;
        new_cache = NULL;
    }
    rx->blk = blk;
    rx->req_ns = req_ns;
    rx->req_block = req_block;
    rx->rec = rec;
    rx->codec = codec;
    rx->codec_entry = entry;
    rx->sclc = block_code;
    rx->sclc_shape = sclc;
    rx->codec_seed = cfg->seed;
    rx->shape.n = cfg->n;
    rx->shape.m = cfg->m;
    rx->shape.groups = cfg->groups;
    rx->shape.slots = (u16)slots;
    rx->shape.w = cfg->w;
    rx->shape.k = cfg->k;
    rx->shape.d = cfg->d;
    rx->shape.codec_variant = cfg->codec_variant;
    rx->shape.L = cfg->L;
    rx->shape.block = cfg->block;
    rx->shape.rate_ppm = cfg->rate_ppm;
    rx->shape.base_pkt = 0;
    rx->shape.base_block = 0;
    rx->base_pkt = 0;
    rx->base_block = 0;
    rx->epoch = cfg->epoch;
    rx->replay_k = tbt_fec_replay_k_for(cfg->groups, cfg->n, cfg->m,
                                        TBT_FEC_S);
    rx->nack_cadence_ms = TBT_FEC_NACK_RETRY_MS;
    rx->m_target = cfg->m;
    rx->target_entry = NULL;
    rx->tick_last_ns = 0;
    rx->tick_last_blocks = 0;
    rx->tick_abandoned_last = (u64)atomic64_read(&rx->abandoned_shares);
    rx->build_q_n = 0;
    rx->codec_build_fail = 0;
    rx->codec_backoff = 0;
    rx->codec_next_ns = 0;
    spin_unlock_bh(&rx->lock);

    tbt_fec_rx_blk_free(old_blk, old_total);
    tbt_fec_rx_buf_free_list(old_pool);
    kvfree(old_req_ns);
    kvfree(old_req_block);
    kvfree(old_rec);
    if (block_code)
    {
        u8 i;

        /* The swap cleared the old pins; drop them before retiring the old
         * object, or the entries and their codecs leak with it. */
        tbt_fec_codec_cache_put(old_entry);
        if (old_target)
            tbt_fec_codec_cache_put(old_target);
        for (i = 0; i < old_build_q_n; i++)
            tbt_fec_codec_cache_put(old_build_q[i]);
        /* The old object stays alive until the plane dies: a block may still
         * hold one of its entries, and put() dereferences entry->cache. */
        if (!rx->codec_cache_old)
            rx->codec_cache_old = old_cache;
    }
    else
        tbt_fec_grouped_free(old_codec);
    spin_lock_bh(&rx->lock);
    if (rx->est)
        tbt_fec_est_init(rx->est, shares);
    if (rx->est_sclc)
        tbt_fec_adapt_sclc_est_init(rx->est_sclc, shares);
    spin_unlock_bh(&rx->lock);
    pr_info_ratelimited("tbtun: tunnel %llu: fec rx shape from peer: n=%u m=%u groups=%u slots=%u block=%u L=%u rate=%u\n",
                        (unsigned long long)(sess->tunnel ? sess->tunnel->cfg.id
                                                          : 0),
                        cfg->n, cfg->m, cfg->groups, slots,
                        block_code ? sclc.block : 0,
                        block_code ? sclc.L : 0,
                        block_code
                            ? (sclc.variant == TBT_FEC_SCLC_DOWN
                                   ? sclc.rate_ppm
                                   : sclc.rate_256)
                            : 0);
    return 0;

fail:
    if (entry)
        tbt_fec_codec_cache_put(entry);
    tbt_fec_codec_cache_free_obj(new_cache);
    return -ENOMEM;
}

/* w_fec_rx side of TBT_FEC_CFG_F_INIT: apply and acknowledge. The ACK names
 * epoch 1, which is what stops the sender's retry. */
static void tbt_fec_rx_init_tick(struct tbt_session *sess,
                                 struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_cfg cfg;
    int err;

    if (!rx)
        return;
    spin_lock_bh(&rx->lock);
    if (!rx->init_pending)
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    cfg = rx->init_cfg;
    rx->init_pending = false;
    spin_unlock_bh(&rx->lock);
    err = tbt_fec_rx_apply_init(sess, f, &cfg);
    if (err)
    {
        atomic64_inc(&sess->fec_init_bad);
        pr_warn_ratelimited("tbtun: tunnel %llu: fec init shape rejected: %d n=%u m=%u groups=%u w=%u k=%u d=%u\n",
                            (unsigned long long)sess->tunnel->cfg.id, err,
                            cfg.n, cfg.m, cfg.groups, cfg.w, cfg.k, cfg.d);
        /* A permanent profile mismatch is refused once; a transient
         * allocation failure is left to the sender's retry window. */
        if (err == -EINVAL)
        {
            rx->fbk_epoch = cfg.epoch;
            tbt_fec_rx_fbk_send(sess, f, TBT_FEC_FBK_F_CFG_NACK);
            rx->fbk_epoch = 0;
        }
        return;
    }
    atomic64_inc(&sess->fec_init_rx);
    rx->fbk_flags |= TBT_FEC_FBK_F_CFG_ACK;
    rx->fbk_epoch = cfg.epoch;
    tbt_fec_rx_fbk_send(sess, f, rx->fbk_flags);
    rx->fbk_flags = 0;
}

/* FEC_CFG from the sender. INIT is the only CFG this protocol still sends:
 * it installs the peer's asymmetric shape before the first share. A rate
 * change travels as a per-block header and an FBK m_target, never as a CFG,
 * so any APPLY/ABORT body is a stale record and is refused. */
static int tbt_fec_rx_on_cfg(struct tbt_session *sess,
                             struct tbt_fec_flow *f,
                             const struct tbt_fec_cfg *cfg)
{
    struct tbt_fec_rx *rx = f->rx;

    if (!rx)
        return 0;
    /* The protocol sends only INIT. A CFG without it is a stale APPLY/ABORT
     * body from a previous wire; silently ignoring it hid a profile
     * mismatch, so name it as a protocol error. */
    if (!(cfg->flags & TBT_FEC_CFG_F_INIT))
        return -EPROTO;
    /* The arena swap allocates, and a carrier parser can run in softirq:
     * record the shape here and let w_fec_rx install it. */
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping) || rx->init_pending)
    {
        spin_unlock_bh(&rx->lock);
        return 0; /* teardown, or a duplicate of the pending announcement */
    }
    rx->init_cfg = *cfg;
    rx->init_pending = true;
    spin_unlock_bh(&rx->lock);
    tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    return 0;
}


/* One pass over this flow's groups*slots. Called only by w_fec_rx. Returns
 * the number of blocks decoded, so the caller can yield after a whole-block
 * decode instead of running several of them before it delivers the first
 * one. */
static int tbt_fec_rx_pass_flow(struct tbt_session *sess,
                                struct tbt_fec_flow *f)
{
    struct tbt_fec_rx *rx = f->rx;
    u16 group, slot;
    int decoded = 0;

    for (group = 0; group < rx->groups; group++)
        for (slot = 0; slot < rx->slots; slot++)
        {
            if (tbt_fec_rx_decode_one(sess, f, group, slot))
                decoded++;
            /* A grouped block code decodes one group per slot and a group
             * block can run tens of milliseconds on the target CPU.  Let
             * the carrier works and the timer softirq run between them. */
            cond_resched();
        }
    return decoded;
}

static void tbt_fec_rx_work_body(struct tbt_session *sess)
{
    bool again;
    int rounds = 0;
    int decoded = 0;

    if (!sess->fec || READ_ONCE(sess->closing) ||
        READ_ONCE(sess->fec_stopping))
        return;
    do
    {
        struct tbt_fec_flow **flows = sess->fec_rx_snap;
        u16 n, i;

        again = false;
        n = tbt_fec_flow_snapshot(sess, flows, TBT_STREAMS_MAX);
        for (i = 0; i < n; i++)
        {
            struct tbt_fec_flow *f = flows[i];
            struct tbt_fec_rx *rx = f->rx;

            if (!rx || READ_ONCE(rx->stopping))
                continue;
            if (rx->build_q_n)
                tbt_fec_rx_build_pending(rx);
            decoded += tbt_fec_rx_pass_flow(sess, f);
            /* Ask for the missing shares before the delivery walk arms
             * its own (possibly 30 s) stall timer: the earlier 10 ms arm
             * wins and keeps the NACK retry alive. */
            tbt_fec_nack_scan(sess, f);
            if (tbt_fec_deliver_ready(sess, f) == -ENOMEM)
                continue; /* a retry is already armed */
            spin_lock_bh(&rx->lock);
            if (rx->work_pending)
            {
                rx->work_pending = false;
                again = true;
            }
            spin_unlock_bh(&rx->lock);
        }
        tbt_fec_flow_put_all(flows, n);
        /* A block decode is the expensive unit (tens of milliseconds on the
         * target).  Deliver and re-arm after one such round instead of
         * stacking up to four of them, which starved the ACK and NACK walk
         * behind the decode and held the delivery cursor for hundreds of
         * milliseconds.  The re-arm below runs the next round at once. */
        if (decoded)
            break;
    } while (again && ++rounds < 4);
    if (again)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
}

/* Lock-free fast path for a share of a block the delivery walk has already
 * consumed. `delivered` is monotone inside a flow and `next_super` only
 * grows once a byte was delivered, so a snapshot can only miss a late share
 * (the locked check below then decides) -- it cannot call a live block
 * late. A shape switch is refused here: the fields are read without the
 * lock, and the placement path rechecks the shape. */
static void tbt_fec_rx_late_sync(struct tbt_fec_rx *rx)
{
    rx->late.epoch = rx->epoch;
    rx->late.next_super = READ_ONCE(rx->next_super);
    rx->late.n = rx->n;
    rx->late.m = rx->m;
    rx->late.groups = rx->groups;
}

/* The post-fold correction needs the record's super id; the generic
 * tbt_fec_late_credit() above only reports SETTLED. Do the lookup here so
 * the estimator is corrected under the same rx->lock. */
static void tbt_fec_rx_late_credit(struct tbt_fec_rx *rx, u8 type,
                                    u64 block_id, u16 group, u16 share,
                                    u16 n, u16 m, bool locked)
{
    struct tbt_fec_late_rec *rec;
    enum tbt_fec_late_evt ev;
    bool has_miss = false;

    if (type != TBT_FEC_DATA)
        return;
    if (!locked)
        spin_lock_bh(&rx->lock);
    if (!rx->est_sclc || !n || !m ||
        (u16)(n + m) != READ_ONCE(rx->shares))
    {
        atomic64_inc(&rx->flow->sess->fec_late_unknown);
        if (!locked)
            spin_unlock_bh(&rx->lock);
        return;
    }
    tbt_fec_rx_late_sync(rx);
    rec = tbt_fec_late_find(&rx->late, block_id);
    ev = tbt_fec_late_credit(&rx->late, block_id, group, share,
                             n, m, &has_miss);
    if (ev == TBT_FEC_LATE_UNKNOWN)
        atomic64_inc(&rx->flow->sess->fec_late_unknown);
    else if (ev == TBT_FEC_LATE_SETTLED && has_miss)
    {
        if (!rec || !tbt_fec_adapt_sclc_est_correct_super(
                         rx->est_sclc, rec->super_id))
            atomic64_inc(&rx->flow->sess->fec_late_unknown);
        else
        {
            u16 bit = (u16)(group * READ_ONCE(rx->shares) + share);

            tbt_fec_late_clear(rec->miss_bits, bit);
            if (rec->miss)
                rec->miss--;
        }
    }
    if (!locked)
        spin_unlock_bh(&rx->lock);
}

static void tbt_fec_rx_late_take(struct tbt_fec_rx *rx, u64 block_id,
                                 u16 group, u16 n, u16 m, u8 *out)
{
    tbt_fec_rx_late_sync(rx);
    tbt_fec_late_take(&rx->late, block_id, group, out);
    (void)n;
    (void)m;
}

static void tbt_fec_rx_late_note_miss(struct tbt_fec_rx *rx, u64 block_id,
                                      u16 group, u16 n, u16 m,
                                      const u8 *sc_miss,
                                      const u8 *codec_fail, u16 real)
{
    u32 limit = rx->est_sclc && rx->est_sclc->carriers ?
                rx->est_sclc->carriers : TBT_FEC_MAX_CARRIERS;

    tbt_fec_rx_late_sync(rx);
    tbt_fec_late_note_miss(&rx->late, block_id, group, n, m, sc_miss,
                           codec_fail, rx->alive_bm, limit, real);
}

static void tbt_fec_rx_late_finalize(struct tbt_fec_rx *rx, u64 block_id,
                                     u8 bucket)
{
    u8 correct[TBT_FEC_BITMAP_BYTES] = {0};
    struct tbt_fec_late_rec *rec;
    u16 i;

    (void)bucket;
    tbt_fec_rx_late_sync(rx);
    rec = tbt_fec_late_finalize(&rx->late, block_id,
                                rx->est_sclc->super_id, correct);
    for (i = 0; i < TBT_FEC_BITMAP_BYTES * 8u; i++)
    {
        if (!tbt_fec_late_bit(correct, i))
            continue;
        if (!rec || !tbt_fec_adapt_sclc_est_correct_super(
                         rx->est_sclc, rec->super_id))
            atomic64_inc(&rx->flow->sess->fec_late_unknown);
        else
        {
            tbt_fec_late_clear(rec->miss_bits, i);
            if (rec->miss)
                rec->miss--;
        }
    }
}

static bool tbt_fec_rx_late_fast(const struct tbt_fec_rx *rx,
                                 const struct tbt_fec_hdr *h)
{
    u64 next = READ_ONCE(rx->next_super);
    u64 next_deliver = READ_ONCE(rx->next_deliver);
    u64 base = READ_ONCE(rx->base_pkt);
    u16 groups = READ_ONCE(rx->groups);
    u64 seq0, block_end;
    u16 n = h->n, m = h->m;

    if (!tbt_fec_block_shape_ok(n, m, READ_ONCE(rx->n), READ_ONCE(rx->m),
                                READ_ONCE(rx->shares), groups, rx->sclc) ||
        h->reserved >= groups)
        return false;
    if (h->type == TBT_FEC_DATA)
    {
        if (!tbt_fec_data_seq0(h->seq, h->reserved, n, h->share_index,
                               &seq0))
            return false;
    }
    else
        seq0 = h->seq;
    if (seq0 < base)
        return false;
    if (READ_ONCE(rx->rebase_pending) ||
        (!READ_ONCE(rx->deliver_bytes) && !READ_ONCE(rx->deliver_partial)))
        return false;
    block_end = seq0 + (u64)groups * n;
    return tbt_fec_rx_block_late(false, true, next, h->block_id, block_end,
                                 next_deliver);
}

/* Slow RX cadence: a NACK retry, the estimator sample or the stall poll.
 * The delays range from 10 ms to the 30 s stall budget; on w_fec_rx they
 * would hold a ready block's 0 ms wake for the whole delay. The entry asks
 * w_fec_rx for one pass, and that pass arms this work again when the next
 * super-block is still not ready. */
static void tbt_fec_rx_poll_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx_poll);
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX_POLL, &sess->sched);

    if (!READ_ONCE(sess->closing) && !READ_ONCE(sess->fec_stopping) &&
        READ_ONCE(sess->fec))
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    if (owned)
        tbt_session_put(sess);
}

void tbt_fec_rx_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_fec_rx);
    bool owned = test_and_clear_bit(TBT_SW_FEC_RX, &sess->sched);

    if (atomic_fetch_or(1u << TBT_FEC_RUN_BIT, &sess->fec_rx_run) &
        (1u << TBT_FEC_RUN_BIT))
    {
        atomic_fetch_or(1u << TBT_FEC_AGAIN_BIT, &sess->fec_rx_run);
        goto out;
    }
    tbt_fec_rx_work_body(sess);
    if (sess->n_fec_flows <= 1)
    {
        struct tbt_fec_flow *primary = tbt_fec_primary_get(sess);

        if (primary)
        {
            tbt_fec_rx_adapt_tick(sess, primary);
            tbt_fec_flow_put(primary);
        }
    }
    atomic_fetch_and(~(1u << TBT_FEC_RUN_BIT), &sess->fec_rx_run);
    if (atomic_fetch_and(~(1u << TBT_FEC_AGAIN_BIT), &sess->fec_rx_run) &
        (1u << TBT_FEC_AGAIN_BIT))
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    /* The estimator needs a periodic sample even on an idle queue: the 1 s
     * window expires and the FBK cadence is 100 ms. */
    if (READ_ONCE(sess->adaptive) && !READ_ONCE(sess->fec_stopping) &&
        !READ_ONCE(sess->closing) && sess->n_fec_flows <= 1)
        tbt_session_arm(sess, TBT_SW_FEC_RX_POLL, msecs_to_jiffies(100));
out:
    if (owned)
        tbt_session_put(sess);
}

static int tbt_fec_rx_record_body(struct tbt_session *sess,
                                  struct tbt_fec_flow *f,
                                  struct tbt_fec_hdr *hp, u8 *rec, size_t len)
{
    struct tbt_fec_rx *rx = f->rx;
    struct tbt_fec_hdr h = *hp;
    struct tbt_fec_blk *b;
    const u8 *payload;
    u16 shares, group, share, slot;
    u64 block_seq0 = 0;
    bool wake = false, rebase;
    bool fresh = false;

    if (!rx || len < TBT_FEC_HDR_LEN)
        return -EPROTO;
    {
        /* Teardown may start between the carrier parse and this record. Stop
         * before decryption so a non-zero return leaves the bytes untouched;
         * the raw parser consumes the record on any return, so a retry can
         * never decrypt the same buffer twice. */
        /* A stale record after teardown is consumed silently. Returning
         * -ESHUTDOWN here made the carrier RX actor kill a persistent TCP
         * carrier for every in-flight tail record. */
        if (READ_ONCE(sess->fec_stopping) || READ_ONCE(rx->stopping))
            return 0;
        /* A duplicate record must be dropped before decryption. The record
         * buffer is decrypted in place; a second pass XORs the plaintext and
         * fails the payload CRC, which looks like wire corruption and hides
         * the real duplicate. The header CRC above makes these names
         * trustworthy. */
        if (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY)
        {
            u16 cg = h.reserved;
            u16 csh = h.share_index;
            u64 seq0 = 0;
            u64 next = READ_ONCE(rx->next_super);
            bool dup = false, late = false;
            bool shape_ok;

            shape_ok = h.reserved < READ_ONCE(rx->groups) &&
                       tbt_fec_block_shape_ok(h.n, h.m, READ_ONCE(rx->n),
                                              READ_ONCE(rx->m),
                                              READ_ONCE(rx->shares),
                                              READ_ONCE(rx->groups),
                                              rx->sclc);
            if (shape_ok && h.type == TBT_FEC_DATA)
                shape_ok = tbt_fec_data_seq0(h.seq, h.reserved, h.n,
                                             h.share_index, &seq0);
            else if (shape_ok)
                seq0 = h.seq;
            if (tbt_fec_rx_late_fast(rx, &h))
            {
                if (h.type == TBT_FEC_DATA && shape_ok)
                    tbt_fec_rx_late_credit(rx, h.type, h.block_id, cg,
                                           csh, h.n, h.m, false);
                else
                    atomic64_inc(&sess->fec_late_unknown);
                atomic64_inc(&sess->fec_late_share);
                return 0;
            }
            spin_lock_bh(&rx->lock);
            /* The shape fields are read under rx->lock: w_fec_rx may apply a
             * switch between the header check and this prefilter. A record
             * whose shape is not the current one is left to the placement
             * path, which decides between late, epoch_bad and rollback. */
            if (shape_ok)
            {
                u64 block_end = seq0 + (u64)rx->groups * h.n;

                /* A whole block behind the delivery frontier is gone: count
                 * the share and return before decrypting, placing, or
                 * handing anything to the stream. */
                if (tbt_fec_rx_block_late(
                        READ_ONCE(rx->rebase_pending),
                        rx->deliver_bytes != 0 || rx->deliver_partial, next,
                        h.block_id, block_end,
                        READ_ONCE(rx->next_deliver)))
                    late = true;
            }
            if (!late && shape_ok &&
                h.block_id >= next && h.block_id < next + rx->slots)
            {
                b = &rx->blk[(size_t)cg * rx->slots +
                             (h.block_id % rx->slots)];
                if (b->used && b->block_id == h.block_id)
                {
                    if (b->empty)
                    {
                        /* A repeated marker is a duplicate; a DATA or real
                         * PARITY for an empty block stays -EPROTO below. */
                        if (h.type == TBT_FEC_PARITY && h.win_len == 0)
                            dup = true;
                    }
                    else if (b->ready)
                    {
                        late = true;
                    }
                    else if (b->n == h.n && b->m == h.m &&
                             h.type == TBT_FEC_DATA && b->dpresent[csh])
                    {
                        dup = true;
                    }
                    else if (h.type == TBT_FEC_PARITY && h.win_len &&
                             b->n == h.n && b->m == h.m &&
                             csh >= h.n &&
                             b->ppresent[csh - h.n])
                    {
                        dup = true;
                    }
                }
            }
            spin_unlock_bh(&rx->lock);
            if (debug >= 3 && (dup || late))
                pr_info("tbtun: fec %s share block=%llu group=%u share=%u type=%u win_len=%u epoch=%u next=%llu\n",
                        late ? "late" : "dup", h.block_id, cg, csh, h.type,
                        h.win_len, h.epoch, next);
            if (dup)
            {
                atomic64_inc(&sess->fec_dup_share);
                return 0;
            }
            if (late)
            {
                tbt_fec_rx_late_credit(rx, h.type, h.block_id, cg, csh,
                                       h.n, h.m, false);
                atomic64_inc(&sess->fec_late_share);
                return 0;
            }
        }
        /* Header framing is trusted here; decrypt DATA/PARITY before the
         * payload CRC, which covers the clear bytes (T0 §1.3, §2.2). The rx
         * context is shared by carriers and body_xor is read-only. */
        if (sess->crypto &&
            (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY))
        {
            if (!smp_load_acquire(&sess->crypto_ready))
            {
                atomic64_inc(&sess->fec_drop_crypto);
                return 0;
            }
            if (tbt_crypto_body_xor_flow(&sess->crypto_ctx.rx, h.sid, h.seq,
                                         h.type == TBT_FEC_PARITY,
                                         h.share_index,
                                         rec + TBT_FEC_HDR_LEN,
                                         h.payload_len))
                return 0; /* counter overflow: drop this share */
        }
        if (tbt_fec_crc32c(0, rec + TBT_FEC_HDR_LEN, h.payload_len) !=
            h.payload_crc32c)
        {
            /* The framing was sound, so only this share is lost. */
            if (debug)
                pr_info_ratelimited("tbtun: fec payload crc fail: type=%u seq=%llu share=%u len=%u block=%llu hdr_crc=%08x\n",
                                    h.type, h.seq, h.share_index,
                                    h.payload_len, h.block_id,
                                    h.header_crc32c);
            atomic64_inc(&sess->fec_payload_crc_fail);
            return 0;
        }
    }
    if (h.type == TBT_FEC_ACK)
    {
        struct tbt_fec_ack a;
        struct tbt_stream *st;
        struct tbt_fec_tx *tx;

        if (tbt_fec_ack_unpack(rec + TBT_FEC_HDR_LEN, &a))
            return 0;
        atomic64_inc(&sess->fec_ack_rx);
        tx = f->tx;
        st = tbt_stream_get(sess, f->sid, false, false);
        if (tx)
        {
            u64 sent = READ_ONCE(tx->seq);
            bool ack_advanced = false;
            u32 per_block = (u32)tx->groups *
                            ((u32)tx->n + tx->m);

            /* The grant is in shares and may name a partial block during
             * a shape switch; the sender floors it. Only a grant below one
             * whole block is a unit error. */
            if (a.credit_packets && per_block &&
                a.credit_packets < per_block)
                atomic64_inc(&sess->fec_credit_unit_bad);

            /* The plane frontier never moves past what this side sent. */
            if (a.next_deliver > sent)
            {
                a.next_deliver = sent;
                atomic64_inc(&sess->fec_ack_bad);
            }
            /* Flow control: the peer has delivered up to this packet, and
             * grants the credit it sent. Wake the packetizer, which may have
             * stopped at the window. */
            /* ACKs now rotate over the carriers, so they can arrive out of
             * order; the frontier is cumulative and must only move forward.
             * A stale ACK that moved it back would shrink the sender's
             * window and stall it until the next ACK caught up. */
            {
                u64 cur = READ_ONCE(tx->peer_pkts);

                /* Carriers process records concurrently, so two ACKs can
                 * race here; a plain write would let the older one win. */
                while (a.next_deliver > cur)
                {
                    u64 prev = cmpxchg(&tx->peer_pkts, cur, a.next_deliver);

                    if (prev == cur)
                    {
                        ack_advanced = true;
                        break;
                    }
                    cur = prev;
                }
            }
            /* Release the record planes of blocks this ACK has delivered.
             * The frontier is cumulative; only a forward move can retire a
             * block. */
            if (ack_advanced && READ_ONCE(tx->replay))
            {
                u64 frontier = tbt_fec_block_of_seq_base(
                    READ_ONCE(tx->peer_pkts), tx->base_pkt, tx->base_block,
                    tx->groups, tx->n);

                tbt_fec_replay_ack(
                    tx->replay, frontier,
                    tbt_mem_avail_kb() < TBT_FEC_REPLAY_MEM_LOW_KB,
                    &tx->replay_lock);
            }
            /* Zero credit is not "unlimited": it would stop the packetizer
             * forever if it replaced a real window, and accepting it as a
             * grant would overrun the receiver's arena. Ignore it and keep
             * the last good credit. A peer cannot widen the window past what
             * the receiver can buffer. */
            if (!a.credit_packets)
                atomic64_inc(&sess->fec_ack_bad);
            else
            {
                /* The peer cannot widen the window past the replay ring:
                 * the oldest block it may still NACK must stay in the ring. */
                WRITE_ONCE(tx->credit,
                           min_t(u32, a.credit_packets,
                                 READ_ONCE(tx->credit_cap)));
            }
            /* The peer's measured RTT is how long a replay needs to come
             * back; the NACK reply deduplicates inside it. A zero sample is
             * "not measured yet", not a zero-latency path. */
            if (a.rtt_us)
                WRITE_ONCE(tx->rtt_us, a.rtt_us);
            tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
            tbt_tx_wake(sess);
        }
        if (st)
        {
            u64 acked = a.deliver_bytes;

            if (acked > READ_ONCE(st->tx_seq))
            {
                acked = READ_ONCE(st->tx_seq);
                atomic64_inc(&sess->fec_ack_bad);
            }
            if (acked)
            {
                spin_lock(&sess->lock);
                if (acked > READ_ONCE(st->tx_ack))
                {
                    WRITE_ONCE(st->tx_ack, acked);
                    st->last_ack_ns = ktime_get_ns();
                    sess->tx_ack = acked;
                    sess->last_ack_ns = st->last_ack_ns;
                }
                spin_unlock(&sess->lock);
            }
        }
        if (st)
            tbt_stream_put(st);
        return 0;
    }
    if (h.type == TBT_FEC_NACK)
    {
        struct tbt_fec_nack n;
        struct tbt_fec_tx *tx = f->tx;
        u64 t0 = ktime_get_ns();
        u64 hold;
        u8 *buf;
        u8 marker_done[TBT_FEC_BITMAP_BYTES];
        u16 g, s, shares, tn, tm, tg;
        u16 cur_tn, cur_tm;
        u64 tbase, tseq0 = 0;
        u16 tband;
        bool have_shape = false;
        u32 requested = 0;
        u32 nack_limit = TBT_FEC_NACK_REQ_MAX;
        bool full = false;
        bool never = false; /* a requested share was not resident */

        if (tbt_fec_nack_unpack(rec + TBT_FEC_HDR_LEN, &n))
            return -EPROTO;
        if (!tx || !READ_ONCE(tx->replay))
        {
            atomic64_inc(&sess->fec_replay_miss_no_ring);
            atomic64_inc(&sess->fec_replay_miss);
            return 0;
        }
        /* Snapshot the shape once: a switch may run between this NACK and
         * the replay lookup. A NACK for a block below the new base is from
         * the old epoch; the ring has been reset, so it cannot be served. */
        spin_lock_bh(&tx->shape_lock);
        cur_tn = tx->n;
        cur_tm = tx->m;
        tg = tx->groups;
        tbase = tx->base_block;
        tband = tx->band;
        spin_unlock_bh(&tx->shape_lock);
        if (h.block_id < tbase)
        {
            atomic64_inc(&sess->fec_nack_stale_epoch);
            return 0;
        }
        /* A block names its own split. The ring stores that split when the
         * block is built, so a NACK is parsed with the requested block's
         * n/m and seq0, never the sender's current shape. */
        spin_lock_bh(&tx->replay_lock);
        have_shape = tbt_fec_replay_shape(tx->replay, h.block_id, &tn, &tm,
                                          &tseq0);
        spin_unlock_bh(&tx->replay_lock);
        if (!have_shape)
        {
            tn = cur_tn;
            tm = cur_tm;
        }
        else if (tn != cur_tn || tm != cur_tm)
        {
            /* The parity band belongs to that block's codec; only the
             * block-code path can differ from the current shape here, and
             * its band is zero. */
            tband = 0;
        }
        shares = (u16)(tn + tm);
        if (!tbt_fec_bitmap_clear_from(n.group_mask, tg) ||
            !tbt_fec_bitmap_clear_from(n.share_mask, shares))
        {
            /* A NACK naming a share outside the shape in force is a record
             * from before the initial-shape install or from the other
             * direction's geometry. Drop it: returning -EPROTO here counted
             * against the carrier, and a join-time burst of stale NACKs is
             * not a reason to kill a carrier. */
            atomic64_inc(&sess->fec_nack_bad);
            return 0;
        }
        atomic64_inc(&sess->fec_nack_rx);
        atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                     &sess->fec_nack_req_shares);
        if (debug >= 1)
        {
            char req[TBT_FEC_BITMAP_BYTES * 2 + 1];

            tbt_fec_diag_hex(req, n.share_mask);
            for (g = 0; g < tg; g++)
            {
                u8 avail[TBT_FEC_BITMAP_BYTES];
                char ah[TBT_FEC_BITMAP_BYTES * 2 + 1];
                u16 s;

                if (!tbt_fec_bitmap_test(n.group_mask, g))
                    continue;
                memset(avail, 0, sizeof(avail));
                for (s = 0; s < shares; s++)
                    if (tbt_fec_bitmap_test(n.share_mask, s) &&
                        tbt_fec_replay_has(tx->replay, h.block_id, g, s))
                        tbt_fec_bitmap_set(avail, s);
                tbt_fec_diag_hex(ah, avail);
                pr_info_ratelimited("tbtun: fec NACK bitmap block=%llu group=%u n=%u m=%u req=%s avail=%s\n",
                                    h.block_id, g, tn, tm, req, ah);
            }
        }
        /* Repair is active from this point: the sender is about to build and
         * queue replay records, and the TX stall check must not kill the
         * stream while that work is in flight. */
        WRITE_ONCE(sess->fec_repair_last_ns, t0);
        /* The block is already delivered: a replay cannot help, and every
         * share of it would be a duplicate on the wire. This races a NACK
         * sent before the ACK reached the peer. */
        if (tbt_fec_block_delivered_base(
                h.block_id, READ_ONCE(tx->peer_pkts), tx->base_pkt,
                tx->base_block, tx->groups, tn))
        {
            atomic64_add(tbt_fec_bitmap_count(n.share_mask),
                         &sess->fec_replay_dedup);
            return 0;
        }
        /* A replayed share needs one round trip to arrive. Naming it again
         * inside that window only puts a duplicate record ahead of the
         * repair already queued. */
        hold = tbt_fec_nack_hold_ns(READ_ONCE(tx->rtt_us),
                                    TBT_FEC_NACK_RETRY_MS);
        buf = kmalloc(TBT_FEC_REC_MAX, GFP_ATOMIC);
        if (!buf)
        {
            atomic64_inc(&sess->fec_replay_miss_alloc);
            atomic64_inc(&sess->fec_replay_miss);
            return -ENOMEM;
        }
        memset(marker_done, 0, sizeof(marker_done));
        {
            u32 cap = tbt_fec_replay_cap(READ_ONCE(sess->fec_closing),
                                         TBT_FEC_REPLAY_BYTES_MAX);
            u32 limit = TBT_FEC_NACK_REQ_MAX;

            spin_lock(&sess->fec_out_lock);
            if (sess->fec_replay_bytes >= cap / 2)
                limit = TBT_FEC_NACK_REQ_THROTTLED;
            spin_unlock(&sess->fec_out_lock);
            if (limit > TBT_FEC_NACK_REQ_MAX)
                limit = TBT_FEC_NACK_REQ_MAX;
            nack_limit = limit;
        }
        for (g = 0; g < tg && !full; g++)
        {
            if (!tbt_fec_bitmap_test(n.group_mask, g))
                continue;
            if (requested >= nack_limit)
                break;
            for (s = 0; s < shares; s++)
            {
                struct tbt_carrier *pc;
                u16 cidx;
                bool replaced = false;
                int err, len;

                if (!tbt_fec_bitmap_test(n.share_mask, s))
                    continue;
                /* r_fail counts DATA shares the peer had to name, not the
                 * parity rows it also asked for. */
                if (s < tn)
                    tx->nack_req_shares++;
                /* A NACK may name a large bitmap; bound the replay work per
                 * record. The receiver retries the remainder. */
                if (++requested >= nack_limit)
                    break;
                spin_lock_bh(&tx->replay_lock);
                len = tbt_fec_replay_get(tx->replay, h.block_id, g, s, buf,
                                         TBT_FEC_REC_MAX);
                spin_unlock_bh(&tx->replay_lock);
                if (len == -ESTALE)
                    atomic64_inc(&sess->fec_replay_evicted_before_nack);
                if (len > 0)
                {
                    u64 last;
                    bool dup;

                    spin_lock_bh(&tx->replay_lock);
                    last = tbt_fec_replay_sent_ns(tx->replay, h.block_id,
                                                  g, s);
                    dup = tbt_fec_replay_dup(t0, last, hold);
                    if (!dup)
                        tbt_fec_replay_mark_sent(tx->replay, h.block_id, g,
                                                 s, t0);
                    spin_unlock_bh(&tx->replay_lock);
                    if (dup)
                    {
                        atomic64_inc(&sess->fec_replay_dedup);
                        continue;
                    }
                    if (tbt_fec_replay_space(sess) < (u32)len)
                    {
                        atomic64_inc(&sess->fec_nack_backpressure);
                        full = true;
                        break;
                    }
                }
                if (len == -ENODATA)
                {
                    bool replied = false;
                    u16 band = tband;
                    u16 first;
                    u16 ri;

                    if (debug >= 2)
                        pr_info("tbtun: fec NACK absent block=%llu group=%u share=%u cur=%llu gblk=%llu\n",
                                h.block_id, g, s, READ_ONCE(tx->cur_block),
                                READ_ONCE(tx->g[g].blk_id));
                    if (h.block_id >= READ_ONCE(tx->cur_block))
                        continue; /* still being built: the share may appear */
                    /* A block code stores parity 0 for every block it wrote.
                     * If the named DATA share sits at or past the cut that
                     * row states, it is padding the sender never wrote: not
                     * a replay miss. Answer with the row (the receiver then
                     * learns win_len and stops naming padding), and count
                     * the skipped request. A request for a real share that
                     * was never stored is a miss and takes the same row as
                     * its answer. */
                    if (s < tn && !band)
                    {
                        struct tbt_fec_hdr ph0;
                        int plen0;

                        spin_lock_bh(&tx->replay_lock);
                        plen0 = tbt_fec_replay_get(tx->replay, h.block_id, g,
                                                   tn, buf, TBT_FEC_REC_MAX);
                        spin_unlock_bh(&tx->replay_lock);
                        if (plen0 == -ESTALE)
                            atomic64_inc(&sess->fec_replay_evicted_before_nack);
                        if (plen0 > 0 && !tbt_fec_hdr_unpack(buf, &ph0) &&
                            ph0.win_len && s >= ph0.win_len)
                            atomic64_inc(&sess->fec_nack_pad_skip);
                        else
                        {
                            atomic64_inc(&sess->fec_replay_miss_never_stored);
                            atomic64_inc(&sess->fec_replay_miss_get);
                            atomic64_inc(&sess->fec_replay_miss);
                        }
                        if (plen0 > 0)
                        {
                            u64 last;
                            bool dup;

                            spin_lock_bh(&tx->replay_lock);
                            last = tbt_fec_replay_sent_ns(tx->replay,
                                                          h.block_id, g, tn);
                            dup = tbt_fec_replay_dup(t0, last, hold);
                            if (!dup)
                                tbt_fec_replay_mark_sent(tx->replay,
                                                         h.block_id, g, tn,
                                                         t0);
                            spin_unlock_bh(&tx->replay_lock);
                            if (dup)
                            {
                                atomic64_inc(&sess->fec_replay_dedup);
                                continue;
                            }
                            if (tbt_fec_replay_space(sess) < (u32)plen0)
                            {
                                atomic64_inc(&sess->fec_nack_backpressure);
                                full = true;
                                break;
                            }
                            if (tbt_fec_replay_enqueue(sess, buf, (u16)plen0,
                                                       t0, tx->flow))
                                continue;
                            atomic64_inc(&sess->fec_nack_retx);
                            atomic64_add((u64)plen0,
                                         &sess->fec_nack_retx_bytes);
                            atomic64_inc(&sess->fec_nack_parity_reply);
                            atomic64_inc(&sess->fec_nack_absent_reply);
                            continue;
                        }
                        atomic64_inc(&sess->fec_nack_req_invalid);
                        continue;
                    }
                    atomic64_inc(&sess->fec_replay_miss_never_stored);
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    /* With a parity band, row r covers DATA share s when
                     * (s + m - (r % m)) % m < band. r = s % m always covers
                     * it (band >= 1); a fixed row 0 does not cover s with
                     * s % m >= band, and repeating it cannot decode the
                     * block. Start at r = s % m, then try the other covering
                     * rows if that row is not in the ring. A no-band DATA
                     * share was answered above. */
                    first = s < tn ? (u16)(s % tm) : (u16)(s - tn);
                    for (ri = 0; ri < tm && !replied; ri++)
                    {
                        u16 r = (u16)((first + ri) % tm);
                        u64 last;
                        bool dup;
                        int plen;

                        if (s < tn &&
                            (u16)((s + tm - (r % tm)) % tm) >= band)
                            continue;
                        spin_lock_bh(&tx->replay_lock);
                        plen = tbt_fec_replay_get(tx->replay, h.block_id, g,
                                                  (u16)(tn + r), buf,
                                                  TBT_FEC_REC_MAX);
                        spin_unlock_bh(&tx->replay_lock);
                        if (plen == -ESTALE)
                            atomic64_inc(&sess->fec_replay_evicted_before_nack);
                        if (plen <= 0)
                            continue;
                        spin_lock_bh(&tx->replay_lock);
                        last = tbt_fec_replay_sent_ns(
                            tx->replay, h.block_id, g, (u16)(tn + r));
                        dup = tbt_fec_replay_dup(t0, last, hold);
                        if (!dup)
                            tbt_fec_replay_mark_sent(tx->replay, h.block_id,
                                                     g, (u16)(tn + r),
                                                     t0);
                        spin_unlock_bh(&tx->replay_lock);
                        if (dup)
                        {
                            /* This row is already on its way; it answers the
                             * request, so no empty marker is needed. */
                            atomic64_inc(&sess->fec_replay_dedup);
                            replied = true;
                            break;
                        }
                        if (tbt_fec_replay_space(sess) < (u32)plen)
                        {
                            atomic64_inc(&sess->fec_nack_backpressure);
                            full = true;
                            break;
                        }
                        if (tbt_fec_replay_enqueue(sess, buf, (u16)plen, t0,
                                                   tx->flow))
                            continue;
                        atomic64_inc(&sess->fec_nack_retx);
                        atomic64_add((u64)plen, &sess->fec_nack_retx_bytes);
                        atomic64_inc(&sess->fec_nack_parity_reply);
                        atomic64_inc(&sess->fec_nack_absent_reply);
                        if (debug >= 2)
                        {
                            struct tbt_fec_hdr ph;

                            if (!tbt_fec_hdr_unpack(buf, &ph))
                                pr_info("tbtun: fec NACK parity reply block=%llu group=%u parity=%u win_len=%u epoch=%u\n",
                                        h.block_id, g, r, ph.win_len,
                                        ph.epoch);
                        }
                        replied = true;
                    }
                    /* The block is resident, so this group did write into
                     * it: the group is not empty and a marker would falsely
                     * mark real data as padding. The share can never exist
                     * (padding beyond the block's cut, or a parity row that
                     * was never stored); no answer exists, and asking again
                     * cannot change that. */
                    if (!replied && !full)
                        atomic64_inc(&sess->fec_nack_req_invalid);
                    continue;
                }
                if (len == -ESTALE)
                {
                    u64 cur = READ_ONCE(tx->cur_block);
                    u16 rk = tbt_fec_replay_k(tx->replay);

                    /* A probe for a block this side has not built yet is not
                     * a storage miss: the receiver is at the packet frontier
                     * waiting for application bytes, and the block will
                     * exist when they arrive. Counting it as
                     * never_stored made a 200 s application stall look like a
                     * replay-ring defect (field: +124 on a 1 GiB rerun). Do
                     * not answer it (a marker would falsely close a future
                     * block); the receiver retries on its probe cadence. */
                    if (h.block_id > cur)
                    {
                        atomic64_inc(&sess->fec_nack_req_invalid);
                        if (debug >= 2)
                            pr_info("tbtun: fec NACK ahead block=%llu group=%u share=%u cur=%llu\n",
                                    h.block_id, g, s, cur);
                        continue;
                    }
                    /* -ESTALE means "this block is not resident". A block
                     * the sender never built reads the same as one the ring
                     * has wrapped past; counting both as evictions makes a
                     * probe for a block that does not exist yet look like a
                     * ring that is too shallow. Only a block the ring has
                     * moved at least K ids past is really evicted. */
                    if (cur >= h.block_id && cur - h.block_id >= rk)
                    {
                        atomic64_inc(&sess->fec_replay_miss_evicted);
                        if (debug >= 2)
                            pr_info("tbtun: fec NACK evicted block=%llu group=%u share=%u cur=%llu k=%u\n",
                                    h.block_id, g, s, cur, rk);
                    }
                    else
                    {
                        atomic64_inc(&sess->fec_replay_miss_never_stored);
                        never = true;
                        if (debug >= 2)
                            pr_info("tbtun: fec NACK unbuilt block=%llu group=%u share=%u cur=%llu k=%u\n",
                                    h.block_id, g, s, cur, rk);
                    }
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    /* The block is closed, still inside the ring, and no
                     * record of it exists for this group: the group is empty
                     * in that block. Markers are not stored, so this is the
                     * only path that answers such a NACK. */
                    if (tbt_fec_nack_empty_group(
                            cur > h.block_id,
                            cur >= h.block_id && cur - h.block_id < rk,
                            READ_ONCE(tx->g[g].blk_id), h.block_id) &&
                        !tbt_fec_bitmap_test(marker_done, g))
                    {
                        tbt_fec_bitmap_set(marker_done, g);
                        if (!tbt_fec_send_empty_marker(sess, tx, g,
                                                       h.block_id, tn, tm,
                                                       have_shape, tseq0))
                        {
                            atomic64_inc(&sess->fec_nack_empty_reply);
                            atomic64_inc(&sess->fec_nack_absent_reply);
                        }
                    }
                    continue;
                }
                if (len <= 0)
                {
                    atomic64_inc(&sess->fec_replay_miss_get);
                    atomic64_inc(&sess->fec_replay_miss);
                    continue;
                }
                atomic64_inc(&sess->fec_replay_hit);
                /* Attribute the replay to a carrier replacement when the
                 * share's own slot has lost its carrier. */
                cidx = (u16)(g * (tn + tm) + s);
                if (cidx < sess->n_car)
                {
                    pc = tbt_carrier_get(sess, (u8)cidx);
                    if (pc)
                    {
                        replaced = atomic_read(&pc->stopping) ||
                                   !atomic_read(&pc->connected);
                        tbt_carrier_put(pc);
                    }
                }
                err = tbt_fec_replay_enqueue(sess, buf, (u16)len, t0,
                                             tx->flow);
                if (err)
                {
                    if (err == -ENOMEM)
                        atomic64_inc(&sess->fec_replay_miss_alloc);
                    else
                        atomic64_inc(&sess->fec_replay_miss_enospc);
                    atomic64_inc(&sess->fec_replay_miss);
                    continue;
                }
                atomic64_inc(&sess->fec_nack_retx);
                atomic64_add((u64)len, &sess->fec_nack_retx_bytes);
                if (replaced)
                    atomic64_add((u64)len,
                                 &sess->fec_replace_retx_bytes);
            }
        }
        kfree(buf);
        if (never)
            atomic64_inc(&sess->fec_nack_never_stored);
        return 0;
    }
    if (h.type == TBT_FEC_FBK)
    {
        struct tbt_fec_fbk fb;
        struct tbt_fec_tx *tx = f->tx;

        if (tbt_fec_fbk_unpack(rec + TBT_FEC_HDR_LEN, &fb))
            return -EPROTO;
        if (!tx)
            return 0;
        /* The report names the receive shape; a mismatch means the two
         * ends disagree about the committed codec, so the values would be
         * meaningless. Drop it and count, as §7 requires. */
        if (!tbt_fec_bitmap_tail_zero(fb.carrier_bitmap, sess->n_car) ||
            !tbt_fec_fbk_variant_match(fb.codec_variant, fb.L, fb.block,
                                       tx->codec_variant, tx->sclc_L,
                                       tx->sclc_block, tx->groups))
        {
            atomic64_inc(&sess->fec_epoch_bad);
            return 0;
        }
        /* A CFG_ACK-only FBK can carry zero sample fields and must not
         * overwrite a real p sample that arrived earlier. */
        if ((fb.flags & TBT_FEC_FBK_F_P_VALID) || fb.expected_shares ||
            fb.p_ewma_ppm)
        {
            tx->peer_p_ppm = fb.p_ewma_ppm;
            if (sess->fec_sclc)
                tx->peer_p_fast_ppm = fb.p_fast_ppm;
            tx->peer_expected = fb.expected_shares;
            tx->peer_received = fb.received_shares;
            tx->peer_flags = fb.flags;
            tx->peer_est_ns = ktime_get_ns();
            tx->peer_est_valid = true;
        }
        else
            tx->peer_flags = fb.flags;
        if (tx->sclc_codec && fb.m_target)
            WRITE_ONCE(tx->want_m, fb.m_target);
        tx->peer_epoch = tbt_fec_fbk_epoch(&fb);
        tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
        return 0;
    }
    if (h.type == TBT_FEC_CFG)
    {
        struct tbt_fec_cfg cfg;
        u16 carriers = sess->tunnel->cfg.carriers;

        if (tbt_fec_cfg_unpack(rec + TBT_FEC_HDR_LEN, carriers, &cfg))
            return -EPROTO;
        if (tbt_fec_rx_on_cfg(sess, f, &cfg))
            return -EPROTO;
        return 0;
    }
    if (h.type != TBT_FEC_DATA && h.type != TBT_FEC_PARITY)
        return 0;

    payload = rec + TBT_FEC_HDR_LEN;
    /* The carrier RX work is process context: refill the lazy block buffer
     * pool before entering the leaf lock. */
    tbt_fec_rx_buf_prepare(rx);
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return 0;
    }
    /* Shape validation belongs under rx->lock. A block names its own n/m;
     * only the direction's n+m is fixed. A record outside the buildable set
     * is a stale epoch, not a broken carrier. */
    {
        u16 cur_groups = rx->groups, cur_shares = rx->shares;
        u64 cur_base = rx->base_pkt;
        bool shape_ok;

        /* A block names its own n/m; only the direction's n+m is fixed.
         * A shape outside the codec's buildable set is a protocol error,
         * not a stale epoch: the sender cannot have built it. */
        shape_ok = tbt_fec_block_shape_ok(h.n, h.m, rx->n, rx->m,
                                          cur_shares, cur_groups,
                                          rx->sclc) &&
                   h.reserved < cur_groups;
        if (shape_ok && h.type == TBT_FEC_DATA)
            shape_ok = tbt_fec_data_seq0(h.seq, h.reserved, h.n,
                                         h.share_index, &block_seq0);
        else if (shape_ok)
            block_seq0 = h.seq;
        if (!shape_ok)
        {
            spin_unlock_bh(&rx->lock);
            /* A DATA tail from the previous shape after the switch base is
             * late, not a foreign epoch. Credit it so the estimator can
             * cancel the parity-recovered miss it caused. */
            if (h.seq < cur_base && h.type == TBT_FEC_DATA)
            {
                /* The record predates this shape's base: its epoch is not
                 * recoverable, so count it unknown rather than crediting. */
                atomic64_inc(&sess->fec_late_unknown);
                atomic64_inc(&sess->fec_late_share);
                return 0;
            }
            atomic64_inc(&sess->fec_epoch_bad);
            return 0;
        }
        if (block_seq0 < cur_base)
        {
            atomic64_inc(&sess->fec_late_share);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
        shares = cur_shares;
        group = h.reserved;
        share = h.share_index;
        if (!shares || group >= cur_groups || share >= shares)
        {
            atomic64_inc(&sess->fec_epoch_bad);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
    }
    {
        u64 block_end = block_seq0 + (u64)rx->groups * h.n;

        /* The prefilter may have run before the delivery walk consumed this
         * block (the record was decrypted in between). Re-check under the
         * same lock the walk uses, and drop a share whose bytes are already
         * gone instead of placing it into a recycled slot. */
        if (tbt_fec_rx_block_late(READ_ONCE(rx->rebase_pending),
                                  rx->deliver_bytes != 0 ||
                                      rx->deliver_partial,
                                  rx->next_super, h.block_id, block_end,
                                  rx->next_deliver))
        {
            tbt_fec_rx_late_credit(rx, h.type, h.block_id, group, share,
                                   h.n, h.m, true);
            atomic64_inc(&sess->fec_late_share);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
    }
    /* Freeze the shape the stream started in before any switch can move
     * base_pkt. FIN validates its pkt_base (the stream's first packet, which
     * predates every later switch) against this origin and span. Capturing
     * them lazily at FIN time measured the post-switch origin, so every FIN
     * of a stream that had adapted failed alignment and the peer reset the
     * stream instead of delivering its tail. */
    if (!rx->stream_span)
    {
        /* The stream's span comes from its first block, not the session
         * default: a per-block n is the shape the sender actually used. */
        rx->stream_span = (u64)rx->groups * h.n;
        rx->stream_base_pkt = rx->base_pkt;
    }
    /* The reorder window belongs to one stream. A reset leaves it empty
     * (rebase_pending), and a record of another sid may only move it once the
     * delivery walk has drained the stream it holds; the first such record
     * rebases it to its own block. A lower block before the stream's first
     * delivery lowers the window to itself, so an out-of-order first record
     * costs a NACK instead of skipping the blocks below it.
     *
     * The session's first stream is not rebased: it starts at packet 0, so its
     * window is already at the base and a first block whose records were lost
     * must be NACKed, not skipped. `rebase_sid` is bound here so a later
     * switch can tell whose blocks the tree holds. */
    rebase = READ_ONCE(rx->rebase_pending);
    switch (tbt_fec_rx_window_step(rx->next_super, h.block_id, rx->slots,
                                   rebase,
                                   rx->deliver_bytes != 0 ||
                                       rx->deliver_partial))
    {
    case TBT_FEC_RX_WIN_REBASE:
    {
        /* The first rebase uses the smallest block seen for this sid: a
         * record that arrived before the reset kept its minimum, so the
         * shares the reset wiped stay inside the window and the NACK scan
         * asks for them. */
        u64 base = h.block_id;

        if (rx->min_block_seen && rx->min_block_seen < base)
            base = rx->min_block_seen;
        if (debug)
            pr_info("tbtun: fec rx rebase sid=%llu block=%llu base=%llu next=%llu highest=%llu\n",
                    h.sid, h.block_id, base, rx->next_super,
                    rx->highest_block);
        rx->rebase_sid = h.sid;
        rx->min_block_seen = base;
        rx->next_super = base;
        if (base > rx->highest_block)
            rx->highest_block = base;
        /* The current record's block seq0 and the block-id distance state
         * the older block's origin; every block spans groups*n packets. */
        {
            u64 back = (h.block_id - base) * (u64)rx->groups * h.n;

            rx->next_deliver = back <= block_seq0 ? block_seq0 - back
                                                  : block_seq0;
        }
        if (!rx->stream_pkt_base)
            rx->stream_pkt_base = rx->next_deliver;
        rx->progress_super = 0;
        rx->progress_ns = 0;
        rx->rebase_pending = false;
        break;
    }
    case TBT_FEC_RX_WIN_LATE:
        tbt_fec_rx_late_credit(rx, h.type, h.block_id, group, share,
                               h.n, h.m, true);
        atomic64_inc(&sess->fec_late_share);
        spin_unlock_bh(&rx->lock);
        return 0; /* already delivered */
    case TBT_FEC_RX_WIN_FAR:
        atomic64_inc(&sess->fec_drop_window);
        spin_unlock_bh(&rx->lock);
        return 0; /* too far ahead to buffer */
    case TBT_FEC_RX_WIN_PLACE:
        break;
    }
    if (h.block_id >= rx->next_super + rx->slots)
    {
        atomic64_inc(&sess->fec_drop_window);
        spin_unlock_bh(&rx->lock);
        return 0; /* the kept minimum from before the reset left this record
                   * beyond the window; the NACK scan recovers the rest */
    }
    slot = (u16)((size_t)group * rx->slots + (h.block_id % rx->slots));
    b = &rx->blk[slot];
    if (!b->used || b->block_id != h.block_id)
    {
        /* A block being decoded owns its plane outside rx->lock; a new
         * block must not reset that slot under the decoder. */
        if (b->decoding)
        {
            atomic64_inc(&sess->fec_rx_nobuf);
            spin_unlock_bh(&rx->lock);
            return 0;
        }
        /* An empty-block marker stores no bytes; it needs no buffer. */
        bool needs_buf = !(h.type == TBT_FEC_PARITY && h.win_len == 0);

        if (needs_buf && !b->buf)
        {
            b->buf = tbt_fec_rx_buf_pop_locked(rx);
            if (!b->buf)
            {
                /* The pool and the allocation both failed; the share is
                 * repaired by the NACK path once memory is available. */
                atomic64_inc(&sess->fec_rx_nobuf);
                spin_unlock_bh(&rx->lock);
                return 0;
            }
        }
        tbt_fec_blk_reset(sess, f, b, h.block_id, block_seq0, h.n, h.m);
        if (needs_buf)
            tbt_fec_rx_blk_attach_locked(rx, b);
        b->sid = f->sid;
    }
    else if (b->seq0 != block_seq0 || b->n != h.n || b->m != h.m)
    {
        /* The block's first share fixes its origin and shape. A later share
         * that disagrees is a protocol error, not a late record. */
        atomic64_inc(&sess->fec_epoch_conflict);
        spin_unlock_bh(&rx->lock);
        return -EPROTO;
    }
    /* Defensive: a parity record is a share of this block. A path that
     * placed parity without the reset must still make the block visible to
     * the decode/NACK walk. */
    b->used = true;
    if (h.block_id > rx->highest_block)
        rx->highest_block = h.block_id;
    if (debug >= 3)
        pr_info("tbtun: fec marker rx block=%llu next=%llu used=%d ready=%d empty=%d type=%u win_len=%u\n",
                h.block_id, READ_ONCE(rx->next_super), b->used, b->ready,
                b->empty, h.type, h.win_len);
    /* A settled block is immutable. Classify the record before anything
     * mutates it: a replayed empty marker, a duplicate marker for an empty
     * block, and a real share that lost the race are all normal on a NACK
     * path, not protocol errors. Returning -EPROTO here made the carrier
     * parser kill the carrier that carried the record, which dropped every
     * share in flight on it (field: rx_proto=93, 102 replacements). */
    switch (tbt_fec_arrival_kind(b->ready, b->empty, h.type, h.win_len,
                                 b->ndata || b->nparity || b->win_len_set ||
                                     b->count_real != 0xffff))
    {
    case TBT_FEC_ARR_LATE:
        /* The block is immutable once ready: a duplicate or late share must
         * not change a byte that may already be on its way to the stream. */
        atomic64_inc(&sess->fec_late_share);
        /* A DATA share for a real slot that is still absent would mean the
         * block became ready without it. The ready rule (ndata >= real) makes
         * this impossible; count it separately so a field run can tell it
         * from the normal parity-after-complete case. */
        if (h.type == TBT_FEC_DATA &&
            share < tbt_fec_blk_real_len(rx, group, b) &&
            !b->dpresent[share])
        {
            tbt_fec_rx_late_credit(rx, h.type, h.block_id, group, share,
                                   b->n, b->m, true);
            atomic64_inc(&sess->fec_late_data);
        }
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_DUP_MARKER:
        /* A replayed marker for a block that is already empty. It carries no
         * payload, so placing it again changes nothing; a NACK retry can
         * deliver it several times. */
        atomic64_inc(&sess->fec_late_share);
        atomic64_inc(&sess->fec_dup_marker);
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_CONFLICT:
        /* Marker and real shares disagree. The block is settled, so the
         * record is ignored and named; killing the carrier would punish every
         * share in flight on it for one shape disagreement. */
        atomic64_inc(&sess->fec_marker_conflict);
        spin_unlock_bh(&rx->lock);
        return 0;
    case TBT_FEC_ARR_PLACE:
        break;
    }

    if (h.type == TBT_FEC_DATA)
    {
        /* A PARITY record may have fixed this block's cut before this share
         * arrived. If the cut says this slot holds no stream byte, placing the
         * share would make it invisible to the delivery walk (which skips
         * every slot at or past the cut) while the packet frontier and the
         * ACK ran past it: the exact silent S-byte deletion this check
         * exists to refuse. */
        if (tbt_fec_share_past_cut(b->win_len_set, b->nreal, share))
        {
            atomic64_inc(&sess->fec_slot_past_cut);
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        if (b->dpresent[share])
        {
            atomic64_inc(&sess->fec_dup_share);
        }
        else
        {
            fresh = true;
            memcpy(b->data + (size_t)share * rx->s, payload, h.payload_len);
            /* The sender zero-pads a short share before encoding parity. The
             * decoder XORs the whole S-byte slot into the right-hand side, so
             * the receiver must zero the same tail; otherwise stale bytes
             * from the slot's previous block corrupt every share recovered
             * from this one. The tail is never delivered: dlen names the real
             * payload. */
            if (h.payload_len < rx->s)
                memset(b->data + (size_t)share * rx->s + h.payload_len, 0,
                       rx->s - h.payload_len);
            /* A PADDED share fills the slot but carries win_len real bytes;
             * the delivery walk must see the real length, never S. */
            b->dlen[share] = (h.flags & TBT_FEC_FF_PADDED) ?
                                 h.win_len : h.payload_len;
            b->dpresent[share] = 1;
            b->gen++;
            /* ndata counts present shares inside the block's current real
             * range. A share outside it is padding for a shape the block no
             * longer has, and the next counts pass drops it. */
            if (share < tbt_fec_blk_real_len(rx, group, b))
                b->ndata++;
            if (h.flags & TBT_FEC_FF_LAST)
            {
                if (!rx->total_known)
                {
                    rx->total_pkts = h.seq + 1;
                    rx->total_known = true;
                }
            }
        }
    }
    else if (h.win_len == 0)
    {
        /* Empty-block marker. It fixes the block as having no real share, so
         * a later DATA or PARITY for it is a mixed-source protocol error. */
        if (b->ndata || b->nparity || b->win_len_set ||
            b->count_real != 0xffff)
        {
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
        b->empty = true;
        b->win_len_set = true;
        b->ready = true;
        b->gen++;
        atomic64_inc(&sess->fec_pad_share);
        rx->work_pending = true;
        if (debug >= 3)
            pr_info("tbtun: fec marker placed block=%llu group=%u next=%llu empty=%d ready=%d\n",
                    h.block_id, group, READ_ONCE(rx->next_super), b->empty,
                    b->ready);
    }
    else
    {
        u16 idx = (u16)(h.share_index - b->n);

        if (b->ppresent[idx])
        {
            atomic64_inc(&sess->fec_dup_share);
        }
        else
        {
            fresh = true;
            memcpy(b->parity + (size_t)idx * rx->s, payload, h.payload_len);
            b->ppresent[idx] = 1;
            b->nparity++;
            b->gen++;
        }
        if (!b->win_len_set)
        {
            u16 i;

            /* The first PARITY fixes the block's cut. A DATA share at or past
             * win_len means the peer used a different block shape for the
             * same block; recovering it would deliver padding as data. */
            for (i = h.win_len; i < b->n; i++)
            {
                if (b->dpresent[i])
                {
                    atomic64_inc(&sess->fec_epoch_conflict);
                    spin_unlock_bh(&rx->lock);
                    return -EPROTO;
                }
            }
            b->nreal = h.win_len;
            b->short_len = h.epoch;
            b->win_len_set = true;
        }
        else if (b->nreal != h.win_len || b->short_len != h.epoch)
        {
            atomic64_inc(&sess->fec_epoch_conflict);
            spin_unlock_bh(&rx->lock);
            return -EPROTO;
        }
    }
    tbt_fec_blk_counts(rx, group, b);
    {
        u16 real = tbt_fec_blk_real_len(rx, group, b);

        if (b->empty || b->ndata >= real)
        {
            b->ready = true;
            rx->work_pending = true; /* deliver_ready() has work to hand over */
        }
        else if (rx->sclc
                     ? tbt_fec_sclc_decode_wake(real, b->n, b->m,
                                                rx->groups, b->ndata,
                                                b->nparity)
                     : b->nparity >= (u16)(real - b->ndata))
        {
            if (rx->sclc && !b->wake_ns)
            {
                b->wake_ns = ktime_get_ns();
                b->wake_ndata = b->ndata;
                b->wake_nparity = b->nparity;
                b->wake_real = real;
            }
            /* The wake line met: attempt now, not after a quiet window.
             * A fresh share is new evidence, so it clears the failure
             * backoff and the next pass retries at once; a duplicate
             * changes no block state and must not clear it. */
            if (fresh)
            {
                b->decode_next_ns = 0;
                b->decode_tries = 0;
            }
            rx->work_pending = true;
        }
    }
    wake = rx->work_pending;
    spin_unlock_bh(&rx->lock);
    if (wake)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    return 0;
}

/* Every per-stream record names its flow in h.sid. The wrapper parses and
 * verifies the header once, takes a flow reference, and runs the body; a late
 * record for a detached flow finds no entry and is dropped, never delivered
 * into another flow's arena. */
int tbt_fec_rx_record(struct tbt_carrier *c, u8 *rec, size_t len)
{
    struct tbt_session *sess = c->sess;
    struct tbt_fec_hdr h;
    struct tbt_fec_flow *f;
    int rc;

    if (!sess || !sess->fec)
        return -EPROTO;
    rc = tbt_fec_rec_check(rec, len, TBT_FEC_S, &h);
    if (rc)
    {
        if (rc == -EBADMSG)
            atomic64_inc(&sess->fec_hdr_crc_fail);
        else
            atomic64_inc(&sess->fec_proto_fail);
        return -EPROTO;
    }
    if (READ_ONCE(sess->fec_stopping))
        return 0;
    if (!h.sid)
    {
        /* v7 requires a routing key on every FEC record. A zero sid is a
         * pre-v7 or hostile record: drop it rather than guess a flow. */
        atomic64_inc(&sess->fec_drop_sid);
        return 0;
    }
    f = tbt_fec_flow_lookup(sess, h.sid);
    if (!f)
    {
        atomic64_inc(&sess->fec_drop_sid);
        return 0;
    }
    rc = tbt_fec_rx_record_body(sess, f, &h, rec, len);
    tbt_fec_flow_put(f);
    return rc;
}

/* Publish FIN's byte end once the walk reached the packet end it names.
 * deliver_bytes is written by w_fec_rx only; a stable read of it plus the
 * delivered predicate means the walk has queued every real share. */
static void tbt_fec_rx_fin_publish(struct tbt_stream *st,
                                   struct tbt_fec_rx *rx)
{
    u64 bytes;

    if (!READ_ONCE(st->rx_fin_seen) || READ_ONCE(st->rx_fin_done))
        return;
    if (!READ_ONCE(rx->total_known) ||
        !tbt_fec_fin_delivered(READ_ONCE(rx->total_pkts),
                               READ_ONCE(rx->next_deliver),
                               READ_ONCE(rx->deliver_partial)))
        return;
    bytes = READ_ONCE(rx->deliver_bytes) > READ_ONCE(rx->stream_base)
                ? READ_ONCE(rx->deliver_bytes) - READ_ONCE(rx->stream_base)
                : 0;
    tbt_rx_fec_fin_ready(st, bytes);
}

/* FIN names the stream's absolute packet range: pkt_end is the packet after
 * its last DATA packet, pkt_base the block-aligned packet of its first one,
 * and the frame header repeats pkt_end (0.8 FEC has no byte offset here).
 * The packet count is therefore pkt_end itself, not ceil(bytes / S): a short
 * share makes the byte count an upper bound on the packet count, and the
 * second stream's pkt_end must not be compared with the first stream's
 * next_super base.
 *
 * FIN alone does not mean the bytes reached the target. The walk may still
 * owe the last short super-block, so the drain completes only after
 * tbt_fec_rx_fin_publish() states the byte end. This is the half-close
 * ordering the field lost: FIN arrived first on another carrier, the drain
 * compared its packet frontier with rx_next's byte cursor, shut the target's
 * write side, and the 64 KiB tail was discarded as EPIPE.
 *
 * A FIN whose stream is already bound names the current flow; the carrier
 * parser drops it if no flow exists for its sid. */
void tbt_fec_rx_fin(struct tbt_session *sess, struct tbt_stream *st,
                    u64 pkt_end, u64 pkt_base)
{
    struct tbt_fec_flow *f;
    struct tbt_fec_rx *rx;
    u64 span;
    bool wake = false;

    if (!sess || !st)
        return;
    f = st->fec_flow;
    if (!f || !f->rx)
        return;
    rx = f->rx;
    spin_lock_bh(&rx->lock);
    if (READ_ONCE(rx->stopping))
    {
        spin_unlock_bh(&rx->lock);
        return;
    }
    span = (u64)rx->groups * rx->n;
    if (!rx->stream_span)
    {
        rx->stream_span = span;
        rx->stream_base_pkt = rx->base_pkt;
    }
    if (!tbt_fec_fin_valid_base(pkt_end, pkt_base, rx->stream_base_pkt,
                                rx->stream_span))
    {
        atomic64_inc(&sess->fec_fin_bad);
        spin_unlock_bh(&rx->lock);
        return;
    }
    if (READ_ONCE(rx->rebase_pending))
    {
        /* The stream's span was frozen from its first block's n. Use it,
         * not the current shape's n, so a FIN after a block-shape change
         * still names the block the stream actually started in. */
        u64 span0 = rx->stream_span ? rx->stream_span : span;
        u64 base;

        if (!span0 || pkt_base < rx->base_pkt)
        {
            atomic64_inc(&sess->fec_fin_bad);
            spin_unlock_bh(&rx->lock);
            return;
        }
        base = rx->base_block + (pkt_base - rx->base_pkt) / span0;

        rx->next_super = base;
        rx->next_deliver = pkt_base;
        rx->stream_pkt_base = pkt_base;
        rx->highest_block = base;
        rx->progress_super = 0;
        rx->progress_ns = 0;
        rx->rebase_pending = false;
    }
    if (!rx->total_known)
    {
        rx->total_pkts = pkt_end;
        rx->total_known = true;
    }
    else if (pkt_end != rx->total_pkts)
        atomic64_inc(&sess->fec_fin_bad); /* keep LAST's value */
    rx->work_pending = true;
    wake = true;
    spin_unlock_bh(&rx->lock);
    if (wake)
        tbt_session_arm(sess, TBT_SW_FEC_RX, 0);
    tbt_fec_rx_fin_publish(st, rx);
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

/* Flow frees run on a module-owned workqueue, not the system-wide one: the
 * last tbt_fec_flow_put() often runs inside tbt_session_destroy(), and rmmod
 * must be able to wait for every queued free before the module text is
 * unmapped. synchronize_rcu()/rcu_barrier() only cover RCU callbacks.
 * flush_scheduled_work() is not an option either: it is deprecated and warns
 * at runtime since 6.6, it waits for every unrelated item on a system-wide
 * queue (the deadlock its deprecation exists to prevent), and its target
 * queue changed from system_wq to system_percpu_wq in 7.0. A private queue
 * gives a barrier that covers exactly our frees on every supported release;
 * destroy_workqueue() is that barrier. */
int tbt_fec_data_module_init(void)
{
    tbt_fec_free_wq = alloc_workqueue("tbt_fec_free",
                                      WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    return tbt_fec_free_wq ? 0 : -ENOMEM;
}

void tbt_fec_data_module_exit(void)
{
    if (!tbt_fec_free_wq)
        return;
    /* tbt_tunnel_shutdown() has returned, so no session, stream or work item
     * can take another flow reference. Flush every pending stream RCU
     * callback first: each may detach and enqueue the last flow_put. */
    rcu_barrier();
    destroy_workqueue(tbt_fec_free_wq);
    tbt_fec_free_wq = NULL;
}

void tbt_fec_data_free(struct tbt_session *sess)
{
    struct tbt_fec_flow *f;
    struct tbt_fec_out *o, *tmp;
    u16 b;

    /* The work items are cancelled before this point. The session drain work
     * is session-owned and must be stopped before its queues go. ndrain is
     * zero until INIT_DELAYED_WORK has run, so a partial init is safe. */
    if (sess->fec_ndrain)
    {
        cancel_delayed_work_sync(&sess->fec_drain_work);
        sess->fec_ndrain = 0;
    }

    /* Clear the RCU-published views before the table reference can be the
     * last put. free_work waits one grace period before releasing the
     * planes; a reader that loaded fec_tx/fec_rx must be ordered before
     * that grace period, not after it. This also covers a partially
     * initialised session (fec == false). */
    tbt_fec_primary_publish(sess, NULL);

    if (sess->fec)
    {
        spin_lock(&sess->fec_out_lock);
        list_for_each_entry_safe(o, tmp, &sess->fec_out_ctl, node)
            tbt_fec_out_free(sess, o);
        list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
            tbt_fec_out_free(sess, o);
        /* The drain work is cancelled above, so this is the only consumer
         * left and the FIFO can be emptied in order. */
        while ((o = tbt_fec_out_fifo_peek(sess)) != NULL)
        {
            tbt_fec_out_fifo_pop(sess);
            tbt_fec_out_free(sess, o);
        }
        spin_unlock(&sess->fec_out_lock);

        /* Drop the table's reference. A flow whose stream still holds one is
         * freed by tbt_fec_flow_detach(); session destroy detaches every
         * stream before this call, so a non-empty remainder is a bug. */
        for (b = 0; b < TBT_STREAM_HT; b++)
        {
            while (!hlist_empty(&sess->fec_flows[b]))
            {
                f = hlist_entry(sess->fec_flows[b].first,
                                struct tbt_fec_flow, node);
                hlist_del_init(&f->node);
                if (sess->n_fec_flows)
                    sess->n_fec_flows--;
                tbt_fec_flow_put(f);
            }
        }
    }
    kvfree(sess->fec_fifo);
    sess->fec_fifo = NULL;
    tbt_fec_primary_publish(sess, NULL);
    sess->fec = false;
}

/* One grouped container for the configured shape. The RLNC path keeps the
 * caller's window; mode 3 resolves the variant to its ops table and the
 * parameter permutation that table documents (see tbt_fec.h). */
struct tbt_fec_grouped *tbt_fec_sclc_new(const struct tbt_fec_sclc_shape *sh,
                                         size_t s, u64 nonce)
{
    const struct tbt_fec_ops *ops;
    u16 k, w, d;

    if (!sh)
        return NULL;
    if (sh->variant == TBT_FEC_SCLC_DOWN)
    {
        ops = &tbt_fec_ops_sclc;
        k = sh->rate_ppm;
        w = sh->block;
        d = 0;
    }
    else if (sh->variant == TBT_FEC_SCLC_UP)
    {
        ops = &tbt_fec_ops_sclc_gf16;
        k = sh->block;
        w = sh->L;
        d = sh->rate_256;
    }
    else
    {
        return NULL;
    }
    return tbt_fec_grouped_new_ops(ops, sh->groups, sh->n, sh->m, k, w, s, d,
                                   nonce);
}

static struct tbt_fec_grouped *tbt_fec_codec_new(
    const struct tbt_fec_sclc_shape *sclc, u16 groups, u16 n, u16 m, u16 k,
    u16 w, u8 d, u64 nonce)
{
    if (sclc)
        return tbt_fec_sclc_new(sclc, TBT_FEC_S, nonce);
    return tbt_fec_grouped_new(groups, n, m, k, w, TBT_FEC_S, d, nonce);
}

/* ------------------------------------------------------------------ *
 * Flow table and per-flow planes (TBT_FEC_CONV 0x1008)
 *
 * A flow owns a subset of the session's physical groups. Its TX and RX planes
 * are full copies of the old session planes, sized by G_i groups; the session
 * keeps only the carrier queues, the work items, the shape and the replay K.
 * With sum(G_i) == total_groups the memory and the aggregate credit equal the
 * old single-stream allocation (design §3.1).
 * ------------------------------------------------------------------ */

static struct tbt_fec_flow *
tbt_fec_flow_find_locked(struct tbt_session *sess, u64 sid)
{
    struct tbt_fec_flow *f;

    hlist_for_each_entry(f, &sess->fec_flows[TBT_STREAM_BUCKET(sid)], node)
        if (f->sid == sid)
            return f;
    return NULL;
}

struct tbt_fec_flow *tbt_fec_flow_lookup(struct tbt_session *sess, u64 sid)
{
    struct tbt_fec_flow *f;

    if (!sess || !sess->fec || !sid)
        return NULL;
    spin_lock(&sess->lock);
    f = tbt_fec_flow_find_locked(sess, sid);
    if (f && !refcount_inc_not_zero(&f->refs))
        f = NULL;
    spin_unlock(&sess->lock);
    return f;
}

static void tbt_fec_tx_plane_free(struct tbt_fec_tx *tx)
{
    u16 i;

    if (!tx)
        return;
    if (tx->g)
    {
        for (i = 0; i < tx->g_cap; i++)
            kvfree(tx->g[i].buf);
        kvfree(tx->g);
    }
    tbt_fec_replay_free(tx->replay);
    if (tx->sclc_codec)
    {
        if (tx->codec_pending)
            tbt_fec_codec_cache_put(tx->codec_pending);
        if (tx->codec_entry)
            tbt_fec_codec_cache_put(tx->codec_entry);
        tbt_fec_codec_cache_free_obj(tx->codec_cache);
        tx->codec_cache = NULL;
    }
    else
        tbt_fec_grouped_free(tx->codec);
    kfree(tx->adapt);
    kvfree(tx->ring);
    kvfree(tx->pkt);
    kvfree(tx->rec);
    kvfree(tx);
}

static void tbt_fec_rx_plane_free(struct tbt_fec_rx *rx)
{
    u32 total;

    if (!rx)
        return;
    total = (u32)rx->groups * rx->slots;
    tbt_fec_rx_blk_free(rx->blk, total);
    tbt_fec_rx_buf_free_list(rx->buf_pool);
    kvfree(rx->rec);
    kvfree(rx->zero);
    kvfree(rx->decode_snap);
    kvfree(rx->nack);
    kvfree(rx->req_ns);
    kvfree(rx->req_block);
    if (rx->sclc)
    {
        u8 i;

        for (i = 0; i < rx->build_q_n; i++)
            tbt_fec_codec_cache_put(rx->build_q[i]);
        rx->build_q_n = 0;
        if (rx->target_entry)
            tbt_fec_codec_cache_put(rx->target_entry);
        if (rx->codec_entry)
            tbt_fec_codec_cache_put(rx->codec_entry);
        tbt_fec_codec_cache_free_obj(rx->codec_cache);
        rx->codec_cache = NULL;
        tbt_fec_codec_cache_free_obj(rx->codec_cache_old);
        rx->codec_cache_old = NULL;
    }
    else
        tbt_fec_grouped_free(rx->codec);
    kfree(rx->est);
    kfree(rx->est_sclc);
    kvfree(rx);
}

static void tbt_fec_flow_free_work(struct work_struct *w)
{
    struct tbt_fec_flow *f = container_of(w, struct tbt_fec_flow, free_work);

    /* The primary compatibility pointers are RCU-published and read by
     * tbt_tx.c/tbt_carrier.c without a flow reference. Wait one grace period
     * so no reader can still hold a pointer into these planes. */
    synchronize_rcu();
    tbt_fec_tx_plane_free(f->tx);
    tbt_fec_rx_plane_free(f->rx);
    kfree(f);
}

void tbt_fec_flow_put(struct tbt_fec_flow *f)
{
    if (!f)
        return;
    if (refcount_dec_and_test(&f->refs))
    {
        /* The zero transition happens once, so the queue cannot already hold
         * this work. If the module is past tbt_fec_data_module_exit(), the
         * missing WQ is a lifecycle bug; leaking one flow is safer than
         * queueing into freed module state. */
        if (WARN_ON_ONCE(!tbt_fec_free_wq))
            return;
        WARN_ON_ONCE(!queue_work(tbt_fec_free_wq, &f->free_work));
    }
}

static struct tbt_fec_tx *
tbt_fec_tx_plane_new(struct tbt_session *sess, struct tbt_fec_flow *flow,
                     u16 groups, u64 sid, const u8 *mask, u64 nonce)
{
    const struct tbt_fec_sclc_shape *sclc =
        sess->fec_sclc ? &sess->fec_sclc_shape : NULL;
    struct tbt_fec_tx *tx;
    u16 n = sess->fec_n, m = sess->fec_m;
    u16 shares = (u16)(n + m);
    u32 i;

    tx = kzalloc(sizeof(*tx), GFP_KERNEL);
    if (!tx)
        return NULL;
    tx->sid = sid;
    tx->flow = flow;
    tx->nonce = nonce;
    memcpy(tx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    if (tbt_fec_group_map_build(mask, sess->fec_groups_total, tx->group_map) !=
        groups)
        goto fail;
    spin_lock_init(&tx->lock);
    spin_lock_init(&tx->replay_lock);
    spin_lock_init(&tx->shape_lock);
    tx->base_pkt = 0;
    tx->base_block = 0;
    tx->adapt = kzalloc(sizeof(*tx->adapt), GFP_KERNEL);
    if (tx->adapt)
        tbt_fec_adapt_init(tx->adapt, sess->adapt.class);
    /* tx->replay is allocated lazily by tbt_fec_tx_replay_ensure() when the
     * packetizer builds the first DATA packet. A receive-only session never
     * pays the ~14 MiB repair window. */
    tx->groups = groups;
    tx->n = n;
    tx->m = m;
    tx->s = TBT_FEC_S;
    tx->blk_seq0 = 0;
    tx->blk_n = n;
    tx->blk_m = m;
    tx->shape_w = sess->fec_w;
    tx->shape_k = sess->fec_k;
    tx->shape_d = sess->fec_d;
    if (sclc)
    {
        /* The v8 CFG extension names the codec and its graph. The rate is
         * derived from the announced split; cfg_unpack recomputes n from it
         * with the same floor. */
        tx->codec_variant = 2;
        tx->sclc_L = sclc->L;
        tx->sclc_block = sclc->block;
        tx->rate_ppm = (u32)((((u64)n * 1000000u) + shares - 1) / shares);
    }
    else
    {
        tx->codec_variant = groups > 1 ? 1 : 0;
    }
    tx->credit_cap = tbt_fec_credit_packets(groups, n, m, sess->fec_replay_k,
                                            sess->fec_slots,
                                            tbt_fec_credit_max_packets(sess));
    {
        /* Before the first ACK the peer has not named its window. Opening
         * the full cap here let the sender blast K-2 blocks at a receiver
         * whose arena may be smaller; the receiver dropped the excess and
         * its ACK froze. Start with one super-block and let the first ACK
         * open the window it can actually buffer. */
        u32 two = 2u * (u32)groups * ((u32)n + m);

        tx->credit = two && two < tx->credit_cap ? two : tx->credit_cap;
    }
    {
        u64 want = (u64)tx->credit * TBT_FEC_S;

        if (want > TBT_FEC_TX_RING_MAX)
            want = TBT_FEC_TX_RING_MAX;
        if (want < TBT_FEC_TX_RING)
            want = TBT_FEC_TX_RING;
        tx->ring_size = (u32)want;
    }
    tx->ring = tbt_fec_vzalloc(tx->ring_size);
    tx->pkt = tbt_fec_vzalloc(TBT_FEC_S);
    tx->rec = tbt_fec_vzalloc(TBT_FEC_REC_MAX);
    /* The per-group structs live for the flow's lifetime. */
    tx->g_cap = groups;
    tx->g = tbt_fec_vzalloc((size_t)tx->g_cap * sizeof(*tx->g));
    if (sclc)
    {
        struct tbt_fec_codec_key key;
        struct tbt_fec_grouped *codec;
        bool ready = false;

        tx->sclc_codec = true;
        tx->sclc_shape = *sclc;
        tx->want_m = m;
        tx->codec_cache = tbt_fec_codec_cache_new_obj(sclc->variant);
        if (!tx->codec_cache)
            goto fail;
        tbt_fec_codec_key_fill(&key, sclc, n, m, nonce);
        tx->codec_entry = tbt_fec_codec_cache_get(tx->codec_cache, &key,
                                                  &ready);
        if (!tx->codec_entry)
            goto fail;
        if (!ready)
        {
            codec = tbt_fec_codec_build(sclc, n, m, groups, nonce);
            tbt_fec_codec_cache_build_done(tx->codec_entry, codec,
                                           codec != NULL);
            if (!codec)
                goto fail;
        }
        tx->codec = tx->codec_entry->codec;
    }
    else
        tx->codec = tbt_fec_codec_new(NULL, groups, n, m, n, sess->fec_w,
                                      sess->fec_d, nonce);
    if (tx->codec)
        tx->band = tbt_fec_grouped_band(tx->codec, 0);
    if (!tx->ring || !tx->pkt || !tx->rec || !tx->g || !tx->codec ||
        !tx->adapt)
        goto fail;
    for (i = 0; i < groups; i++)
    {
        tx->g[i].buf = tbt_fec_vzalloc((size_t)shares * TBT_FEC_S);
        if (!tx->g[i].buf)
            goto fail;
        tx->g[i].blk_id = ~0ULL;
    }
    return tx;

fail:
    tbt_fec_tx_plane_free(tx);
    return NULL;
}

static struct tbt_fec_rx *
tbt_fec_rx_plane_new(struct tbt_session *sess, struct tbt_fec_flow *flow,
                     u16 groups, u64 sid, const u8 *mask)
{
    const struct tbt_fec_sclc_shape *sclc =
        sess->fec_sclc ? &sess->fec_sclc_shape : NULL;
    struct tbt_fec_rx *rx;
    u16 n = sess->fec_n, m = sess->fec_m;
    u16 shares = (u16)(n + m);
    u32 total = (u32)groups * sess->fec_slots;

    rx = kzalloc(sizeof(*rx), GFP_KERNEL);
    if (!rx)
        return NULL;
    rx->sid = sid;
    rx->stream_sid = sid;
    rx->flow = flow;
    memcpy(rx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    {
        u16 map_count = tbt_fec_group_map_build(
            mask, sess->fec_sclc ? TBT_FEC_MAX_GROUPS
                                 : sess->fec_groups_total,
            rx->group_map);

        /* SC-LDPC may start with the local TX group count while the peer
         * bitmap already names the RX groups; CFG INIT installs the matching
         * RX geometry later. */
        if (!map_count || (!sess->fec_sclc && map_count != groups))
            goto fail;
    }
    spin_lock_init(&rx->lock);
    mutex_init(&rx->buf_alloc_lock);
    rx->n = n;
    rx->m = m;
    rx->shares = shares;
    rx->s = TBT_FEC_S;
    rx->groups = groups;
    rx->slots = sess->fec_slots;
    rx->buf_bytes = (u32)shares * TBT_FEC_S;
    rx->base_pkt = 0;
    rx->base_block = 0;
    rx->epoch = 0;
    rx->shape.n = n;
    rx->shape.m = m;
    rx->shape.groups = groups;
    rx->shape.slots = sess->fec_slots;
    rx->shape.w = sess->fec_w;
    rx->shape.k = sess->fec_k;
    rx->shape.d = sess->fec_d;
    if (sess->fec_sclc)
    {
        rx->shape.codec_variant = 2;
        rx->shape.L = sess->fec_sclc_shape.L;
        rx->shape.block = sess->fec_sclc_shape.block;
        rx->shape.rate_ppm =
            (u32)((((u64)n * 1000000u) + (n + m) - 1) / (n + m));
    }
    else
    {
        rx->shape.codec_variant = groups > 1 ? 1 : 0;
    }
    rx->replay_k = sess->fec_replay_k;
    rx->est = kzalloc(sizeof(*rx->est), GFP_KERNEL);
    if (rx->est)
        tbt_fec_est_init(rx->est, shares);
    if (sess->fec_sclc)
    {
        rx->est_sclc = kzalloc(sizeof(*rx->est_sclc), GFP_KERNEL);
        if (rx->est_sclc)
            tbt_fec_adapt_sclc_est_init(rx->est_sclc, shares);
    }
    rx->nack = tbt_fec_vzalloc(sizeof(*rx->nack));
    rx->blk = tbt_fec_rx_blk_alloc(total, shares, n);
    rx->req_ns = tbt_fec_vzalloc((size_t)total * shares * sizeof(*rx->req_ns));
    rx->req_block = tbt_fec_vzalloc((size_t)total * sizeof(*rx->req_block));
    rx->nack_cadence_ms = TBT_FEC_NACK_RETRY_MS;
    {
        u16 rec_slots = n > m ? n : m;

        if (rec_slots < TBT_FEC_ADAPT_CLASSES)
            rec_slots = TBT_FEC_ADAPT_CLASSES;
        if (sess->fec_sclc)
        {
            u16 sc_slots = groups > 1 ? 30u : 192u;

            if (sc_slots > rec_slots)
                rec_slots = sc_slots;
        }
        rx->rec = tbt_fec_vzalloc((size_t)rec_slots * TBT_FEC_S);
    }
    rx->zero = tbt_fec_vzalloc(TBT_FEC_S);
    rx->decode_snap = tbt_fec_vzalloc(sizeof(*rx->decode_snap));
    if (sclc)
    {
        struct tbt_fec_codec_key key;
        struct tbt_fec_grouped *codec;
        bool ready = false;

        rx->sclc = true;
        rx->sclc_shape = *sclc;
        rx->codec_seed = tbt_fec_base_nonce(sess);
        rx->m_target = m;
        rx->codec_cache = tbt_fec_codec_cache_new_obj(sclc->variant);
        if (!rx->codec_cache)
            goto fail;
        tbt_fec_codec_key_fill(&key, sclc, n, m, rx->codec_seed);
        rx->codec_entry = tbt_fec_codec_cache_get(rx->codec_cache, &key,
                                                  &ready);
        if (!rx->codec_entry)
            goto fail;
        if (!ready)
        {
            codec = tbt_fec_codec_build(sclc, n, m, groups,
                                        rx->codec_seed);
            tbt_fec_codec_cache_build_done(rx->codec_entry, codec,
                                           codec != NULL);
            if (!codec)
                goto fail;
        }
        rx->codec = rx->codec_entry->codec;
    }
    else
        rx->codec = tbt_fec_codec_new(NULL, groups, n, m, n, sess->fec_w,
                                      sess->fec_d, tbt_fec_base_nonce(sess));
    if (!rx->blk || !rx->req_ns || !rx->req_block || !rx->codec ||
        !rx->rec || !rx->zero || !rx->decode_snap || !rx->est || !rx->nack ||
        (sess->fec_sclc && !rx->est_sclc))
        goto fail;
    return rx;

fail:
    tbt_fec_rx_plane_free(rx);
    return NULL;
}

static u16 tbt_fec_free_groups_locked(struct tbt_session *sess, u16 want,
                                      u8 mask[TBT_FEC_BITMAP_BYTES])
{
    struct tbt_fec_flow *f;
    u16 p, free_bits = 0, take;

    memset(mask, 0, TBT_FEC_BITMAP_BYTES);
    for (p = 0; p < sess->fec_groups_total; p++)
    {
        bool used = false;
        u16 b;

        for (b = 0; b < TBT_STREAM_HT && !used; b++)
            hlist_for_each_entry(f, &sess->fec_flows[b], node)
            {
                if (tbt_fec_bitmap_test(f->group_mask, p))
                {
                    used = true;
                    break;
                }
            }
        if (!used)
            free_bits++;
    }
    take = want && want < free_bits ? want : free_bits;
    if (!take)
        return 0;
    for (p = 0; p < sess->fec_groups_total && take; p++)
    {
        bool used = false;
        u16 b;

        for (b = 0; b < TBT_STREAM_HT && !used; b++)
            hlist_for_each_entry(f, &sess->fec_flows[b], node)
            {
                if (tbt_fec_bitmap_test(f->group_mask, p))
                {
                    used = true;
                    break;
                }
            }
        if (!used)
        {
            tbt_fec_bitmap_set(mask, p);
            take--;
        }
    }
    return (u16)(tbt_fec_bitmap_count(mask));
}

bool tbt_fec_flow_can_admit(struct tbt_session *sess)
{
    bool can;

    if (!sess || !sess->fec)
        return false;
    spin_lock(&sess->lock);
    can = sess->fec_groups_used < sess->fec_groups_total;
    spin_unlock(&sess->lock);
    return can;
}

/* Allocate and publish a flow. The caller holds fec_alloc_lock, which
 * serialises admission and detach; the spinlock is taken only for the table
 * check/reserve and for the final publish. The plane allocations sleep and
 * must never run under sess->lock. */
static struct tbt_fec_flow *
tbt_fec_flow_create(struct tbt_session *sess, u64 sid,
                    const u8 mask[TBT_FEC_BITMAP_BYTES], u16 count,
                    const u8 *rx_mask)
{
    struct tbt_fec_flow *f, *it;
    u16 p, b;
    u64 nonce;
    int err = -ENOMEM;

    spin_lock(&sess->lock);
    err = tbt_fec_flow_create_code(
        sid && tbt_fec_flow_find_locked(sess, sid) != NULL,
        sess->fec_sclc, sess->n_fec_flows, sess->fec_groups_total, count);
    if (err)
    {
        spin_unlock(&sess->lock);
        return ERR_PTR(err);
    }
    if (!tbt_fec_groups_fit(sess->fec_groups_total,
                            sess->fec_groups_used, count))
    {
        spin_unlock(&sess->lock);
        return ERR_PTR(-ENOSPC);
    }
    for (b = 0; b < TBT_STREAM_HT; b++)
        hlist_for_each_entry(it, &sess->fec_flows[b], node)
            for (p = 0; p < sess->fec_groups_total; p++)
                if (tbt_fec_bitmap_test(mask, p) &&
                    tbt_fec_bitmap_test(it->group_mask, p))
                {
                    spin_unlock(&sess->lock);
                    return ERR_PTR(-EPROTO);
                }
    /* Reserve the groups before the sleeping allocation. fec_alloc_lock
     * prevents another admission from stealing them; a failure below puts
     * the reservation back. */
    sess->fec_groups_used += count;
    spin_unlock(&sess->lock);

    f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (!f)
        goto fail_unreserve;
    f->sess = sess;
    f->sid = sid;
    f->groups = count;
    f->state = 1;
    memcpy(f->group_mask, mask, TBT_FEC_BITMAP_BYTES);
    if (tbt_fec_group_map_build(mask, sess->fec_groups_total, f->group_map) !=
        count)
    {
        kfree(f);
        err = -EPROTO;
        goto fail_unreserve;
    }
    nonce = tbt_fec_base_nonce(sess);
    f->tx = tbt_fec_tx_plane_new(sess, f, count, sid, mask, nonce);
    f->rx = tbt_fec_rx_plane_new(sess, f, count, sid,
                                 rx_mask ? rx_mask : mask);
    if (!f->tx || !f->rx)
    {
        tbt_fec_tx_plane_free(f->tx);
        tbt_fec_rx_plane_free(f->rx);
        kfree(f);
        goto fail_unreserve;
    }
    refcount_set(&f->refs, 2); /* one table reference, one caller reference */
    INIT_WORK(&f->free_work, tbt_fec_flow_free_work);

    spin_lock(&sess->lock);
    hlist_add_head(&f->node, &sess->fec_flows[TBT_STREAM_BUCKET(sid)]);
    sess->n_fec_flows++;
    if (!sess->fec_primary)
    {
        tbt_fec_primary_publish(sess, f);
    }
    spin_unlock(&sess->lock);
    return f;

fail_unreserve:
    spin_lock(&sess->lock);
    sess->fec_groups_used -= count;
    spin_unlock(&sess->lock);
    return ERR_PTR(err);
}

/* Bind the unbound primary flow to the first stream. Its TX plane keeps the
 * local shape; for SC-LDPC the OPEN bitmap names the peer's TX groups, which
 * are this side's RX groups, so the RX plane's mask moves to that bitmap. The
 * RX plane is reallocated later by FEC_CFG INIT, which also checks that the
 * INIT group count matches this bitmap. The caller holds fec_alloc_lock and
 * the spinlock. */
static struct tbt_fec_flow *
tbt_fec_flow_bind_primary_locked(struct tbt_session *sess,
                                 struct tbt_fec_flow *f, u64 sid,
                                 const u8 mask[TBT_FEC_BITMAP_BYTES])
{
    hlist_del_init(&f->node);
    f->sid = sid;
    if (f->tx)
        f->tx->sid = sid;
    if (f->rx)
    {
        f->rx->sid = sid;
        f->rx->stream_sid = sid;
        /* A prior shrink/reset left this persistent plane marked stopping;
         * the new stream must be able to receive FEC_CFG and DATA. */
        WRITE_ONCE(f->rx->stopping, false);
    }
    if (sess->fec_sclc && f->rx)
    {
        /* The flow mask is this side's TX partition (used by OPEN announce);
         * the peer bitmap belongs to the RX plane. The TX plane keeps its
         * local shape, and CFG INIT later installs the RX geometry. */
        memcpy(f->rx->group_mask, mask, TBT_FEC_BITMAP_BYTES);
        tbt_fec_group_map_build(mask, TBT_FEC_MAX_GROUPS, f->rx->group_map);
    }
    hlist_add_head(&f->node, &sess->fec_flows[TBT_STREAM_BUCKET(sid)]);
    refcount_inc(&f->refs); /* caller reference */
    return f;
}

struct tbt_fec_flow *tbt_fec_flow_admit_mask(struct tbt_session *sess, u64 sid,
                                             const u8 mask[TBT_FEC_BITMAP_BYTES])
{
    struct tbt_fec_flow *f;
    u16 count;
    bool bound = false;

    if (!sess || !sess->fec || !sid)
        return ERR_PTR(-EINVAL);
    if (!tbt_fec_open_mask_valid(sess->fec_sclc, sess->fec_groups_total,
                                 mask, &count))
        return ERR_PTR(-EPROTO);
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !f->sid && sess->n_fec_flows == 1)
    {
        /* The OPEN names the peer's TX bitmap, which is this side's RX
         * bitmap. The two directions are allowed to differ (1x256 downlink
         * against 8x32 uplink), so for SC-LDPC the local TX mask is not the
         * thing to compare: nothing has bound a peer bitmap yet. A repeated
         * OPEN is checked against rx->group_mask by the caller. */
        if (!sess->fec_sclc &&
            !tbt_fec_primary_mask_match(false, f->group_mask, mask))
            f = ERR_PTR(-EPROTO);
        else
        {
            /* The peer OPEN names this side's RX bitmap. */
            f = tbt_fec_flow_bind_primary_locked(sess, f, sid, mask);
            bound = true;
        }
    }
    spin_unlock(&sess->lock);
    if (bound || (f && IS_ERR(f)))
    {
        mutex_unlock(&sess->fec_alloc_lock);
        return f;
    }
    spin_lock(&sess->lock);
    if (tbt_fec_flow_find_locked(sess, sid))
    {
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return ERR_PTR(-EEXIST);
    }
    if (sess->n_fec_flows)
    {
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return ERR_PTR(-ENOSPC);
    }
    spin_unlock(&sess->lock);
    if (sess->fec_sclc)
    {
        u8 local[TBT_FEC_BITMAP_BYTES];
        u16 local_count = sess->fec_groups_total;
        u16 g;

        memset(local, 0, sizeof(local));
        for (g = 0; g < local_count && g < TBT_FEC_MAX_GROUPS; g++)
            tbt_fec_bitmap_set(local, g);
        f = tbt_fec_flow_create(sess, sid, local, local_count, mask);
    }
    else
    {
        f = tbt_fec_flow_create(sess, sid, mask, count, NULL);
    }
    mutex_unlock(&sess->fec_alloc_lock);
    return f;
}


struct tbt_fec_flow *tbt_fec_flow_admit(struct tbt_session *sess, u64 sid,
                                        u16 want)
{
    struct tbt_fec_flow *f;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 got;

    (void)want;
    if (!sess || !sess->fec || !sid)
        return ERR_PTR(-EINVAL);
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    f = sess->fec_primary;
    if (f && !f->sid && sess->n_fec_flows == 1)
    {
        /* Near side: the local TX partition is the plane's bitmap; the
         * peer's RX shape arrives later in CFG INIT. */
        f = tbt_fec_flow_bind_primary_locked(sess, f, sid, f->group_mask);
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return f;
    }
    if (sess->n_fec_flows)
    {
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return ERR_PTR(-ENOSPC);
    }
    got = tbt_fec_free_groups_locked(sess, sess->fec_groups_total, mask);
    if (!got)
    {
        spin_unlock(&sess->lock);
        mutex_unlock(&sess->fec_alloc_lock);
        return ERR_PTR(-ENOSPC);
    }
    spin_unlock(&sess->lock);
    f = tbt_fec_flow_create(sess, sid, mask, got, NULL);
    mutex_unlock(&sess->fec_alloc_lock);
    return f;
}

/* Drop this flow's control and replay records. DATA records stay in the
 * session FIFO: the drain work is their only consumer, and it drops one
 * whose flow is no longer live before writing it. Walking the FIFO here
 * would race that consumer for a record it is already sending; a kick makes
 * the cleanup immediate instead. */
static void tbt_fec_flow_purge(struct tbt_session *sess,
                               struct tbt_fec_flow *f)
{
    struct tbt_fec_out *o, *tmp;

    spin_lock(&sess->fec_out_lock);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_ctl, node)
        if (o->flow == f)
            tbt_fec_out_free(sess, o);
    list_for_each_entry_safe(o, tmp, &sess->fec_out_replay, node)
        if (o->flow == f)
            tbt_fec_out_free(sess, o);
    spin_unlock(&sess->fec_out_lock);
    tbt_fec_out_kick(sess);
}

/* Undo an admission whose stream could not be created. `f` is the caller's
 * admission reference; the table holds another. This function consumes both,
 * so a failed stream start cannot leave the flow unlinked but alive. */
void tbt_fec_flow_abort(struct tbt_session *sess, struct tbt_fec_flow *f)
{
    bool unlinked = false;

    if (!f)
        return;
    if (!sess || !sess->fec)
    {
        tbt_fec_flow_put(f); /* consume the caller's admission reference */
        return;
    }
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    /* Publish the death before unlinking: a snapshot or a send that already
     * has a reference must see a dead flow and stop producing work for it. */
    WRITE_ONCE(f->state, 3);
    if (!hlist_unhashed(&f->node))
    {
        hlist_del_init(&f->node);
        unlinked = true;
        if (sess->n_fec_flows)
            sess->n_fec_flows--;
        if (sess->fec_groups_used >= f->groups)
            sess->fec_groups_used -= f->groups;
        if (sess->fec_primary == f)
            tbt_fec_primary_publish(sess, NULL);
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);
    if (f->rx)
    {
        spin_lock_bh(&f->rx->lock);
        WRITE_ONCE(f->rx->stopping, true);
        spin_unlock_bh(&f->rx->lock);
        /* Release the RX block planes now, not after the RCU flow_put. */
        tbt_fec_rx_shrink(f->rx, true);
    }
    if (f->tx && READ_ONCE(f->tx->replay))
        tbt_fec_replay_shrink(f->tx->replay, false, 2,
                              &f->tx->replay_lock);
    /* Retire every queued record for this sid before the reference count
     * can decide the flow's fate; a FIFO DATA record holds a flow reference
     * and otherwise strands the 16 MiB TX ring until session destroy. */
    tbt_fec_tx_clear_data_queues(sess, f);
    /* Purge after the flow is unreachable: a racing producer now refuses at
     * tbt_fec_out_push(), and an in-flight flush owns a record reference. */
    tbt_fec_flow_purge(sess, f);
    if (unlinked)
        tbt_fec_flow_put(f); /* table reference, only if we took it out */
    tbt_fec_flow_put(f); /* caller's admission reference */
}

int tbt_fec_flow_attach(struct tbt_stream *st, struct tbt_fec_flow *f)
{
    if (!st || !f)
        return -EINVAL;
    st->fec_flow = f; /* consumes the caller's reference */
    return 0;
}

void tbt_fec_flow_detach(struct tbt_stream *st)
{
    struct tbt_session *sess;
    struct tbt_fec_flow *f;
    bool unlinked = false;

    if (!st)
        return;
    f = xchg(&st->fec_flow, NULL);
    if (!f)
        return;
    sess = f->sess;
    /* Detach is the last point a stream can still own undelivered FEC
     * bytes. A FIN in flight with a short final block is the case this
     * guards: completion must wait for the walk, never for the unlink. */
    if (f->rx && READ_ONCE(st->rx_fin_seen) &&
        !READ_ONCE(st->rx_fin_done) && READ_ONCE(f->rx->total_known) &&
        !tbt_fec_fin_delivered(READ_ONCE(f->rx->total_pkts),
                               READ_ONCE(f->rx->next_deliver),
                               READ_ONCE(f->rx->deliver_partial)))
    {
        atomic64_inc(&sess->fec_fin_with_undelivered);
        pr_info_ratelimited("tbtun: fec detach with undelivered sid=%llu next=%llu total=%llu bytes=%llu\n",
                            (unsigned long long)st->sid,
                            (unsigned long long)READ_ONCE(f->rx->next_deliver),
                            (unsigned long long)READ_ONCE(f->rx->total_pkts),
                            (unsigned long long)READ_ONCE(f->rx->deliver_bytes));
    }
    mutex_lock(&sess->fec_alloc_lock);
    spin_lock(&sess->lock);
    WRITE_ONCE(f->state, 3);
    if (!hlist_unhashed(&f->node))
    {
        hlist_del_init(&f->node);
        unlinked = true;
        if (sess->n_fec_flows)
            sess->n_fec_flows--;
        if (sess->fec_groups_used >= f->groups)
            sess->fec_groups_used -= f->groups;
        if (sess->fec_primary == f)
            tbt_fec_primary_publish(sess, NULL);
    }
    spin_unlock(&sess->lock);
    mutex_unlock(&sess->fec_alloc_lock);
    if (f->rx)
    {
        spin_lock_bh(&f->rx->lock);
        WRITE_ONCE(f->rx->stopping, true);
        spin_unlock_bh(&f->rx->lock);
        /* Return every RX block plane to the kernel before the delayed
         * flow_put; a new stream can start immediately after detach. */
        tbt_fec_rx_shrink(f->rx, true);
    }
    if (f->tx && READ_ONCE(f->tx->replay))
        tbt_fec_replay_shrink(f->tx->replay, false, 2,
                              &f->tx->replay_lock);
    tbt_fec_tx_clear_data_queues(sess, f);
    tbt_fec_flow_purge(sess, f);
    if (unlinked)
        tbt_fec_flow_put(f); /* table reference, only if we took it out */
    tbt_fec_flow_put(f); /* stream reference */
}

int tbt_fec_data_init(struct tbt_session *sess)
{
    struct tbt_tunnel *t = sess->tunnel;
    struct tbt_fec_sclc_shape sclc;
    struct tbt_fec_flow *f;
    bool is_sclc = t->cfg.mode == 3;
    u16 n = t->cfg.n, m = t->cfg.m, shares, groups, w, d;
    u32 slots, total, i;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 primary_groups;

    /* Every record the parsers accept must fit the buffer that assembles it.
     * Without these, raising TBT_FEC_PAYLOAD_MAX would make a full record
     * impossible to complete and the parser would stick at p->have ==
     * sizeof(p->buf) with no error path. */
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(TBT_FEC_REC_MAX != TBT_FEC_HDR_LEN + TBT_FEC_PAYLOAD_MAX);
    BUILD_BUG_ON(TBT_PARSER_BUF < TBT_REC_MAX);
    BUILD_BUG_ON(TBT_CAR_PART_MAX < TBT_FEC_REC_MAX);
    BUILD_BUG_ON(sizeof(((struct tbt_http_rx *)0)->rec) !=
                 TBT_HTTP_CHUNK_DATA_MAX);
    /* The estimator fold uses TBT_FEC_ADAPT_GROUPS entries on the stack; the
     * SC-LDPC codec's own group limit must not outgrow that scratch. */
    BUILD_BUG_ON(TBT_FEC_ADAPT_GROUPS < TBT_SCLC_MAX_GROUPS);

    if (!n || !m || (u16)n + (u16)m > TBT_FEC_MAX_SHARES)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: n=%u m=%u exceeds %u shares\n",
                            (unsigned long long)t->cfg.id, n, m,
                            TBT_FEC_MAX_SHARES);
        return -EINVAL;
    }
    if (is_sclc && tbt_fec_sclc_shape_of(n, m, t->cfg.carriers, t->cfg.w,
                                         t->cfg.L, &sclc))
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: not an SC-LDPC shape n=%u m=%u carriers=%u w=%u L=%u\n",
                            (unsigned long long)t->cfg.id, n, m,
                            t->cfg.carriers, t->cfg.w, t->cfg.L);
        return -EINVAL;
    }
    shares = (u16)(n + m);
    if (!t->cfg.carriers || t->cfg.carriers % shares)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: carriers=%u is not a multiple of n+m=%u\n",
                            (unsigned long long)t->cfg.id, t->cfg.carriers,
                            shares);
        return -EINVAL;
    }
    groups = (u16)(t->cfg.carriers / shares);
    if (!groups || groups > TBT_FEC_MAX_GROUPS)
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: groups=%u outside 1..%u\n",
                            (unsigned long long)t->cfg.id, groups,
                            TBT_FEC_MAX_GROUPS);
        return -EINVAL;
    }
    if (is_sclc)
    {
        if (sclc.variant == TBT_FEC_SCLC_DOWN)
        {
            w = sclc.block;
            d = 0;
        }
        else
        {
            w = sclc.L;
            d = sclc.rate_256;
        }
        /* The controller may move to any split this codec builds; the
         * configured start must be one of them. */
        if (!tbt_fec_adapt_sclc_buildable(sclc.variant, n, m))
        {
            pr_warn_ratelimited("tbtun: tunnel %llu: fec rejected: n/m not buildable for mode 3 (n=%u m=%u)\n",
                                (unsigned long long)t->cfg.id, n, m);
            return -EINVAL;
        }
    }
    else
    {
        w = n;
        if (groups > 1)
        {
            w = t->cfg.w ? t->cfg.w : (u16)(2 * n);
            if (w < n)
                w = n;
        }
        d = (u16)(w / 2);
        if (!d)
            d = 1;
        if (w > 1 && d >= w)
            d = (u16)(w - 1);
    }

    /* One super-block across every group is groups*(n+m)*S bytes. Keep the
     * whole RX arena at or below 16 MiB: 32x8 gets 8 slots, one group keeps
     * the historical 256. The slots value is per session: every flow's arena
     * is G_i*slots blocks, so sum(G_i)*slots is the single-stream total. */
    slots = TBT_FEC_RX_SLOTS;
    if (slots < 4)
        slots = 4;
    if (slots > 256)
        slots = 256;
    total = (u32)groups * slots;
    TBT_DIAG("fec init start tunnel=%llu n=%u m=%u carriers=%u mode=%u groups=%u w=%u d=%u slots=%u total_blk=%u arena_kb=%u",
             (unsigned long long)t->cfg.id, n, m, t->cfg.carriers,
             t->cfg.mode, groups, w, d, slots, total,
             (u32)(((size_t)total * shares * TBT_FEC_S) >> 10));

    /* Session scratch: the DATA FIFO and the drain work belong to the
     * session, not to one flow. */
    sess->fec_fifo = tbt_fec_vzalloc(sizeof(*sess->fec_fifo) *
                                     TBT_FEC_OUTQ_FIFO_SIZE);
    if (!sess->fec_fifo)
        goto fail;
    sess->fec_fifo_head = 0;
    sess->fec_fifo_tail = 0;
    sess->fec_outq_depth_max = 0;
    atomic_set(&sess->async_rot, 0);
    atomic_set(&sess->fec_outq_queued, 0);
    atomic_set(&sess->fec_outq_bytes, 0);

    INIT_DELAYED_WORK(&sess->fec_drain_work, tbt_fec_out_drain_work);
    /* Publish the gate last: tbt_fec_out_kick() refuses while ndrain is 0,
     * so it cannot arm an uninitialised work during a partial init. */
    sess->fec_ndrain = sess->n_car;

    for (i = 0; i < TBT_STREAM_HT; i++)
        INIT_HLIST_HEAD(&sess->fec_flows[i]);
    mutex_init(&sess->fec_alloc_lock);
    spin_lock_init(&sess->fec_fbk_lock);
    sess->fec_fbk_busy = false;
    sess->fec_fbk_pending = 0;
    sess->n_fec_flows = 0;
    sess->fec_groups_total = groups;
    sess->fec_groups_used = 0;
    sess->fec_slots = (u16)slots;
    sess->fec_replay_k = tbt_fec_replay_k_for(groups, n, m, TBT_FEC_S);
    sess->fec_n = n;
    sess->fec_m = m;
    sess->fec_w = w;
    sess->fec_k = is_sclc ? (u16)(sclc.variant == TBT_FEC_SCLC_DOWN
                                      ? sclc.rate_ppm
                                      : sclc.block)
                          : n;
    sess->fec_d = (u8)d;
    sess->fec_sclc = is_sclc;
    if (is_sclc)
        sess->fec_sclc_shape = sclc;
    sess->fec_rr = 0;
    sess->fec_ctl_rot = 0;
    sess->fec_stopping = false;
    atomic_set(&sess->fec_tx_run, 0);
    atomic_set(&sess->fec_rx_run, 0);
    tbt_fec_primary_publish(sess, NULL);

    INIT_LIST_HEAD(&sess->fec_out_ctl);
    INIT_LIST_HEAD(&sess->fec_out_replay);
    spin_lock_init(&sess->fec_out_lock);
    INIT_DELAYED_WORK(&sess->w_fec_tx, tbt_fec_tx_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx, tbt_fec_rx_work);
    INIT_DELAYED_WORK(&sess->w_fec_tick, tbt_fec_tick_work);
    INIT_DELAYED_WORK(&sess->w_fec_rx_poll, tbt_fec_rx_poll_work);
    INIT_DELAYED_WORK(&sess->w_fec_cut, tbt_fec_cut_work);
    sess->fec = true;

    /* One flow per session owns the whole carrier geometry. A far-side OPEN
     * later replaces the RX mask; the TX mask stays local. */
    primary_groups = groups;
    memset(mask, 0, sizeof(mask));
    for (i = 0; i < primary_groups; i++)
        tbt_fec_bitmap_set(mask, (u16)i);

    mutex_lock(&sess->fec_alloc_lock);
    f = tbt_fec_flow_create(sess, 0, mask, primary_groups, NULL);
    mutex_unlock(&sess->fec_alloc_lock);
    if (IS_ERR(f))
    {
        pr_warn_ratelimited("tbtun: tunnel %llu: fec primary flow failed: %ld\n",
                            (unsigned long long)t->cfg.id, PTR_ERR(f));
        goto fail;
    }
    /* The table keeps one reference; the primary pointer is a borrowed view
     * and is cleared by tbt_fec_flow_detach(). */
    tbt_fec_flow_put(f);

    tbt_session_arm(sess, TBT_SW_FEC_TX, 0);
    TBT_DIAG("fec init done tunnel=%llu groups=%u slots=%u replay_k=%u primary_groups=%u mode=%u",
             (unsigned long long)t->cfg.id, groups, slots,
             sess->fec_replay_k, primary_groups, t->cfg.mode);
    return 0;

fail:
    pr_warn_ratelimited("tbtun: tunnel %llu: fec init failed\n",
                        (unsigned long long)t->cfg.id);
    tbt_fec_data_free(sess);
    return -ENOMEM;
}
