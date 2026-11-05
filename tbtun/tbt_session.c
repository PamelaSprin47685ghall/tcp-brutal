/* tbt_session.c - tunnel objects, the session, the carrier array, and the
 * global list's contents.
 *
 * Owns:
 *   - turning a parsed struct tbt_cfg into a live tbt_tunnel (or rejecting it
 *     without side effects, DESIGN.md §8.3 "no partial application").
 *   - the per-netns carrier port block (§7.2) and its bitmap.
 *   - the session's carrier slots (0..n-1), its nonce, and the far side's
 *     HELLO join rule (§9.5).
 *   - the reference counts and RCU freeing of tunnels.
 *   - the `tunnels` / `peers` /proc views and the runtime action verbs.
 *
 * Must never:
 *   - start the listener while holding tbt_lock with interrupts disabled; the
 *     listener start path takes socket locks and may sleep.
 *   - free a tunnel that the accept work item may still touch; see
 *     tbt_tunnel_put().
 *   - accept a configuration the design calls impossible (DNS names, role
 *     without its required keys, more carriers than path_ack_mask can name).
 */
#include <linux/bitmap.h>
#include <linux/ktime.h>
#include <linux/nsproxy.h>
#include <linux/random.h>
#include <linux/vmalloc.h>
#include <net/inet_sock.h>
#include <net/tcp.h>
#include <net/sock.h>

#include "tbtun.h"

struct tbt_portmap tbt_ports = {
    .lock = __MUTEX_INITIALIZER(tbt_ports.lock),
    .lo = 40000,
    .hi = 40063,
};

static const char *tbt_role_name(u8 role)
{
    switch (role)
    {
    case TBT_ROLE_NEAR:
        return "near";
    case TBT_ROLE_FAR:
        return "far";
    default:
        return "?";
    }
}

static const char *tbt_state_name(const struct tbt_tunnel *t)
{
    if (t->lsock)
        return "listening";
    return "idle";
}

// ---------------------------------------------------------------------------
// Port block

int tbt_portmap_set(u16 lo, u16 hi)
{
    unsigned long *used;
    size_t n;

    if (!lo || lo > hi)
        return -EINVAL;
    n = BITS_TO_LONGS(hi - lo + 1);
    used = kcalloc(n, sizeof(*used), GFP_KERNEL);
    if (!used)
        return -ENOMEM;

    mutex_lock(&tbt_ports.lock);
    kvfree(tbt_ports.used);
    tbt_ports.used = used;
    tbt_ports.lo = lo;
    tbt_ports.hi = hi;
    tbt_ports.configured = true;
    mutex_unlock(&tbt_ports.lock);
    return 0;
}

void tbt_portmap_get(u16 *lo, u16 *hi, bool *configured)
{
    mutex_lock(&tbt_ports.lock);
    *lo = tbt_ports.lo;
    *hi = tbt_ports.hi;
    *configured = tbt_ports.configured;
    mutex_unlock(&tbt_ports.lock);
}

void tbt_portmap_show(struct seq_file *m)
{
    u16 lo, hi;

    mutex_lock(&tbt_ports.lock);
    lo = tbt_ports.lo;
    hi = tbt_ports.hi;
    seq_printf(m, "# block %u-%u (%u ports)\n", lo, hi, hi - lo + 1);
    seq_printf(m, "# %s\n", tbt_ports.configured ? "configured" : "default, not yet set");
    mutex_unlock(&tbt_ports.lock);
}

// ---------------------------------------------------------------------------
// Tunnel lifecycle

static bool tbt_cfg_sane(const struct tbt_cfg *c, int *err)
{
    if (!c->id)
    {
        *err = -EINVAL;
        return false;
    }
    if (c->role != TBT_ROLE_NEAR && c->role != TBT_ROLE_FAR)
    {
        *err = -EINVAL;
        return false;
    }
    /* max_carriers is a module_param (kernel-only). The host parser cannot
     * see it, so the bound is enforced here, before any allocation or session
     * creation. The compile-time limit still applies. */
    if (!c->n || c->n + c->m > TBT_MAX_CARRIERS_LIMIT ||
        c->n + c->m > READ_ONCE(max_carriers))
    {
        *err = -EINVAL;
        return false;
    }
    // Role-specific requirements, exactly as DESIGN.md §8.3 states them:
    //   near: listen= (public port, where apps connect) and peer= (the far end)
    //   far:  listen= (carrier port, where carriers arrive) and target=
    // The other role's fields are legitimately zero and must not be demanded.
    if (!c->listen_port)
    {
        *err = -EINVAL;
        return false;
    }
    if (c->role == TBT_ROLE_NEAR && !c->peer_port)
    {
        *err = -EINVAL;
        return false;
    }
    if (c->role == TBT_ROLE_FAR && !c->target_port)
    {
        *err = -EINVAL;
        return false;
    }
    return true;
}

void tbt_tunnel_put(struct tbt_tunnel *t)
{
    if (!t || !refcount_dec_and_test(&t->refs))
        return;
    put_net(t->net); // paired with the get_net() in tbt_tunnel_add()
    kfree_rcu(t, rcu);
}

static struct tbt_tunnel *tbt_tunnel_find_locked(u64 id)
{
    struct tbt_tunnel *t;

    list_for_each_entry(t, &tbt_tunnels, node)
    {
        if (t->cfg.id == id)
            return t;
    }
    return NULL;
}

// Count the global tunnel list. Caller holds tbt_lock.
static unsigned int tbt_tunnel_count_locked(void)
{
    struct tbt_tunnel *t;
    unsigned int n = 0;

    list_for_each_entry(t, &tbt_tunnels, node)
        n++;
    return n;
}

// Caller holds tbt_lock. Installs `cfg`, replacing an existing tunnel with the
// same id (DESIGN.md §8.4.1 "re-applying the same line is idempotent").
//
// Lock invariant: tbt_config_apply() takes tbt_lock around this call, so this
// function must never take it again. The count and the list insert are both
// serialized by that lock; two concurrent writers cannot both pass the cap.
int tbt_tunnel_add(const struct tbt_cfg *cfg, struct net *net)
{
    struct tbt_tunnel *old, *t;
    int err = 0;

    if (!tbt_cfg_sane(cfg, &err))
        return err;

    /* Replacing an existing id is always allowed; only a new id consumes a
     * slot. Check before kzalloc and before detaching `old`, so a refusal
     * leaves the running configuration untouched. */
    old = tbt_tunnel_find_locked(cfg->id);
    if (!old && tbt_tunnel_count_locked() >= READ_ONCE(max_tunnels))
        return -ENOSPC;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    /* The caller captured this at open time: config flushes run from
     * __fput task_work during process exit, where current->nsproxy is NULL. */
    t->net = get_net(net);
    refcount_set(&t->refs, 1);
    t->cfg = *cfg;
    // Initialised here, not in tbt_listen_start: the listening socket can
    // deliver data before start returns, and sk_data_ready would then
    // schedule an uninitialised work item.
    INIT_WORK(&t->w_accept, tbt_accept_work);

    if (old)
    {
        list_del_rcu(&old->node);
        tbt_listen_stop(old);
    }
    list_add_tail_rcu(&t->node, &tbt_tunnels);

    // Both roles listen: a near tunnel on its public port, a far tunnel on its
    // carrier port (DESIGN.md §2.1, §9.5).
    err = tbt_listen_start(t);
    if (err)
        goto undo;
    err = tbt_session_create(t);
    if (err)
    {
        tbt_listen_stop(t);
        tbt_session_destroy(t);
        goto undo;
    }
    /* A connection accepted before the session exists would reach
     * tbt_stream_start(NULL, ...); enable accepting only now. */
    atomic_set(&t->accepting, 1);
    schedule_work(&t->w_accept);
    if (old)
    {
        tbt_session_destroy(old);
        tbt_tunnel_put(old);
    }
    return 0;

undo:
    list_del_rcu(&t->node);
    if (old)
    {
        /* We detached the previous tunnel before starting the new one; on a
         * failure it must not be left running detached. */
        tbt_listen_stop(old);
        tbt_session_destroy(old);
        tbt_tunnel_put(old);
    }
    tbt_tunnel_put(t);
    return err;
}

int tbt_tunnel_del(u64 id)
{
    struct tbt_tunnel *t = tbt_tunnel_find_locked(id);

    if (!t)
        return -ENOENT;
    list_del_rcu(&t->node);
    tbt_listen_stop(t);
    tbt_session_destroy(t);
    tbt_tunnel_put(t);
    return 0;
}

// Caller holds tbt_lock. Marks the list empty; the caller frees.
void tbt_tunnel_shutdown(void)
{
    struct tbt_tunnel *t, *tmp;

    /* Mark every session closing before the first listener goes away: tearing
     * one tunnel down first makes the others' carriers retry a connect that
     * can only fail, and the retry log lands in the middle of a probe's own
     * output. */
    list_for_each_entry(t, &tbt_tunnels, node)
    {
        if (t->sess)
        {
            spin_lock(&t->sess->lock);
            t->sess->closing = true;
            spin_unlock(&t->sess->lock);
        }
    }
    list_for_each_entry_safe(t, tmp, &tbt_tunnels, node)
    {
        list_del_rcu(&t->node);
        tbt_listen_stop(t);
        tbt_session_destroy(t);
        tbt_tunnel_put(t);
    }
}

void tbt_tunnel_show(struct seq_file *m)
{
    struct tbt_tunnel *t;

    seq_puts(m, "# id role state listen peer target n m streams\n");
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        seq_printf(m, "%llu %s %s %pI4:%u %pI4:%u ", t->cfg.id,
                   tbt_role_name(t->cfg.role), tbt_state_name(t),
                   &t->cfg.listen_addr, ntohs(t->cfg.listen_port),
                   &t->cfg.peer_addr, ntohs(t->cfg.peer_port));
        if (t->cfg.role == TBT_ROLE_FAR)
            seq_printf(m, "%pI4:%u ", &t->cfg.target_addr, ntohs(t->cfg.target_port));
        else
            seq_puts(m, "- ");
        seq_printf(m, "%u %u %llu\n", t->cfg.n, t->cfg.m, t->streams);
    }
    mutex_unlock(&tbt_lock);
}

static const char *tbt_car_state_name(u8 s)
{
    switch (s)
    {
    case TBT_CAR_CONNECTING:
        return "connecting";
    case TBT_CAR_UP:
        return "up";
    case TBT_CAR_SPARE:
        return "spare";
    case TBT_CAR_STALLED:
        return "stalled";
    default:
        return "down";
    }
}

static const char *tbt_profile_name(u8 p)
{
    switch (p)
    {
    case 1:
        return "http-kal";
    case 2:
        return "http-kal-drip";
    default:
        return "raw";
    }
}

/* One line per carrier slot. The columns are TBT_PEERS_COLUMNS; the host test
 * pins the header verbatim, so the renderer and the constant move together. */
void tbt_peers_show(struct seq_file *m)
{
    struct tbt_tunnel *t;

    seq_puts(m, "# " TBT_PEERS_COLUMNS "\n");
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        struct tbt_session *s = t->sess;
        int i;

        if (!s)
            continue;
        for (i = 0; i < s->n_car; i++)
        {
            struct tbt_carrier *c = READ_ONCE(s->car[i]);
            __be32 peer = t->cfg.peer_addr;
            __be16 port = t->cfg.peer_port;
            u16 sport;
            u32 inflight, rx_lag_ppm = 0, retx_ppm = 0, rtt;
            u64 uack, frames, bytes, dropped;

            if (!c)
                continue;
            sport = c->local_port;
            rtt = READ_ONCE(c->rtt_us);
            {
                struct sock *sk = READ_ONCE(c->sk);

                if (sk)
                {
                    /* Retire clears c->sk without tbt_lock; hold a reference so
                     * a concurrent release cannot free it under the deref. */
                    sock_hold(sk);
                    peer = inet_sk(sk)->inet_daddr;
                    port = inet_sk(sk)->inet_dport;
                    sport = ntohs(inet_sk(sk)->inet_sport);
                    sock_put(sk);
                }
            }
            uack = READ_ONCE(c->uack);
            inflight = READ_ONCE(c->payload_bytes_sent) > uack ?
                (u32)(READ_ONCE(c->payload_bytes_sent) - uack) : 0;
            {
                struct sock *sk = READ_ONCE(c->sk);

                if (sk)
                {
                    u32 srtt;

                    sock_hold(sk);
                    srtt = READ_ONCE(tcp_sk(sk)->srtt_us) >> 3;
                    sock_put(sk);
                    if (srtt > READ_ONCE(c->rtt_us))
                        rtt = srtt;
                }
            }
            frames = READ_ONCE(c->tx_frames);
            bytes = READ_ONCE(c->tx_bytes);
            dropped = READ_ONCE(c->dropped_by_ppm);
            /* rx_lag_ppm: frames the receiver has not yet accounted for below
             * the sender's write high-water; includes in-flight frames and
             * frames sitting in the peer's parser. Not packet loss.
             * retx_ppm: DATA copies written / unique DATA records written.
             * The old formula divided copies by every record on the path,
             * including FBK (45% of them in a bench run), which under-reported
             * the retransmit rate by about half. Both counters are stamped
             * where the carrier accepts the write, so neither includes a copy
             * the pump later dropped. */
            rx_lag_ppm = READ_ONCE(c->loss_ppm) > 0 ? (u32)READ_ONCE(c->loss_ppm) : 0;
            {
                u64 copies = READ_ONCE(c->retx_data);
                u64 data = READ_ONCE(c->data_frames);
                u64 unique = data > copies ? data - copies : 0;

                retx_ppm = unique ? (u32)min_t(u64, copies * 1000000 / unique,
                                               1000000) : 0;
            }
            seq_printf(m, "%llu %u %u %pI4:%u %llu %u %u %s %s %u %u %u %llu %llu %llu %u\n",
                       t->cfg.id, i, sport, &peer, ntohs(port), uack,
                       rtt, inflight, tbt_car_state_name(c->state),
                       tbt_profile_name(c->profile), READ_ONCE(c->recv_cnt),
                       rx_lag_ppm, READ_ONCE(c->rtt_min_us), frames, bytes,
                       dropped, retx_ppm);
        }
    }
    mutex_unlock(&tbt_lock);
}

/* Runtime action from `peers` (or `config`): `carrier <index> down|up|drop_ppm`
 * or `session <id> nonce=<hex>`. C3 has one trunk, so the index selects the
 * carrier in every tunnel that has it. */
int tbt_action_apply(const struct tbt_cmd *cmd)
{
    struct tbt_tunnel *t;
    int err = -ENOENT;

    if (cmd->kind == TBT_CMD_SESSION)
    {
        mutex_lock(&tbt_lock);
        t = tbt_tunnel_find_locked(cmd->id);
        if (!t)
            goto out_unlock;
        if (!t->sess)
        {
            err = -ENOTCONN;
            goto out_unlock;
        }
        spin_lock(&t->sess->lock);
        memcpy(t->sess->nonce, cmd->nonce, sizeof(t->sess->nonce));
        t->sess->nonce_pinned = true;
        t->sess->nonce_set = true;
        spin_unlock(&t->sess->lock);
        err = 0;
out_unlock:
        mutex_unlock(&tbt_lock);
        return err;
    }
    if (cmd->kind != TBT_CMD_CARRIER)
        return -EINVAL;
    /* The gate lives before the lock and outside the per-carrier loop: an
     * early return inside the loop would leave tbt_lock held. */
    if (cmd->carrier_action == TBT_CARRIER_DROP_PPM && !tbt_debug_loss())
    {
        pr_warn_once("tbtun: " TBT_DROP_PPM_GATE_MSG "\n");
        return -EPERM;
    }

    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        struct tbt_session *s = t->sess;
        struct tbt_carrier *c;

        if (!s || cmd->carrier >= s->n_car)
            continue;
        c = READ_ONCE(s->car[cmd->carrier]);
        if (!c)
            continue;
        if (cmd->carrier_action == TBT_CARRIER_DOWN)
        {
            /* Marking it down is what removes it from scheduling, counts
             * carrier_lost once, and asks the replacement worker to dial. */
            tbt_carrier_lost(s, c);
            err = 0;
        }
        else if (cmd->carrier_action == TBT_CARRIER_UP)
        {
            /* `up` is the inverse of `down`; it never dials. A carrier that
             * was shut down has no socket, so this only clears the flag for a
             * carrier that is still connected. */
            atomic_set(&c->stopping, 0);
            if (c->sock)
                c->state = TBT_CAR_UP;
            err = 0;
        }
        else if (cmd->carrier_action == TBT_CARRIER_DROP_PPM)
        {
            WRITE_ONCE(c->drop_ppm, cmd->drop_ppm);
            err = 0;
        }
    }
    mutex_unlock(&tbt_lock);
    return err;
}

// ---------------------------------------------------------------------------
// Carrier port block allocation (DESIGN.md §7.2)
//
// C3 keeps one global block (the §8.3 "known gap"); the bitmap is per module,
// guarded by tbt_ports.lock. Ports are released when the carrier goes away, so
// a replacement carrier can reuse one immediately.

u16 tbt_port_alloc(enum tbt_port_alloc_err *err)
{
    enum tbt_port_alloc_err why = TBT_PORT_ALLOC_OK;
    unsigned long n;
    u16 port = 0;

    mutex_lock(&tbt_ports.lock);
    /* The block has a usable default (40000-40063, DESIGN.md §7.3): a tunnel
     * must work without the operator writing `ports` first. Allocate the
     * bitmap on first use. */
    if (!tbt_ports.used)
    {
        n = tbt_ports.hi - tbt_ports.lo + 1;
        tbt_ports.used = bitmap_zalloc(n, GFP_KERNEL);
        if (!tbt_ports.used)
        {
            why = TBT_PORT_ALLOC_NOMEM;
            goto out;
        }
    }
    n = tbt_ports.hi - tbt_ports.lo + 1;
    {
        unsigned long bit = find_next_zero_bit(tbt_ports.used, n, 0);

        if (bit >= n)
        {
            why = TBT_PORT_ALLOC_EXHAUSTED;
            goto out;
        }
        __set_bit(bit, tbt_ports.used);
        port = tbt_ports.lo + bit;
    }
out:
    mutex_unlock(&tbt_ports.lock);
    if (err)
        *err = why;
    return port;
}

void tbt_port_free(u16 port)
{
    mutex_lock(&tbt_ports.lock);
    if (tbt_ports.used && port >= tbt_ports.lo && port <= tbt_ports.hi)
        __clear_bit(port - tbt_ports.lo, tbt_ports.used);
    mutex_unlock(&tbt_ports.lock);
}

// ---------------------------------------------------------------------------
// Streams

static void tbt_stream_free_rcu(struct rcu_head *head)
{
    struct tbt_stream *st = container_of(head, struct tbt_stream, rcu);

    // Only the stream's own queues belong to the stream; frames on the session
    // TX queue are released when the session dies.
    tbt_rx_free_queue(st);
    tbt_tx_free_stream(st);
    kfree(st->holes);
    kfree(st);
}

void tbt_stream_put(struct tbt_stream *st)
{
    if (st && refcount_dec_and_test(&st->refs))
        call_rcu(&st->rcu, tbt_stream_free_rcu);
}

// Look up (or create) the stream for `sid`. Callers hold no lock; the hash is
// protected by sess->lock.
struct tbt_stream *tbt_stream_get(struct tbt_session *sess, u64 sid, bool alloc, bool near)
{
    struct tbt_stream *st;

    spin_lock(&sess->lock);
    hlist_for_each_entry(st, &sess->streams[TBT_STREAM_BUCKET(sid)], node)
    {
        if (st->sid == sid && refcount_inc_not_zero(&st->refs))
        {
            spin_unlock(&sess->lock);
            return st;
        }
    }
    spin_unlock(&sess->lock);

    if (!alloc)
        return NULL;

    st = kzalloc(sizeof(*st), GFP_ATOMIC);
    if (!st)
        return NULL;
    /* Two references: the stream hash owns one until the stream is unlinked,
     * and the caller owns the other. Without the hash reference a caller that
     * simply put its stream (tbt_stream_start does) would free an object that
     * is still reachable through sess->streams. */
    refcount_set(&st->refs, 2);
    st->sess = sess;
    st->sid = sid;
    st->tx_ack = 0;
    st->near = near;
    spin_lock_init(&st->tx_lock);
    spin_lock_init(&st->rx_lock);
    INIT_LIST_HEAD(&st->rx_q);
    INIT_LIST_HEAD(&st->retain);
    st->rx_tree = RB_ROOT_CACHED;
    st->rx_limit = sess->tunnel ? sess->tunnel->cfg.rx_reorder_bytes : TBT_RX_RING_MAX;
    st->retain_max = sess->tunnel ? sess->tunnel->cfg.tx_retain_bytes : 0;
    /* Initialised at birth, not at attach: RX can queue a frame before the far
     * side's target socket exists, and scheduling an uninitialised work item is
     * a call through a NULL function pointer. */
    tbt_stream_init_work(st);
    WRITE_ONCE(st->state, TBT_ST_PENDING);

    spin_lock(&sess->lock);
    {
        struct tbt_stream *found;

        hlist_for_each_entry(found, &sess->streams[TBT_STREAM_BUCKET(sid)], node)
        {
            if (found->sid == sid && refcount_inc_not_zero(&found->refs))
            {
                spin_unlock(&sess->lock);
                /* Lost the race: our object was never published. */
                kfree(st);
                return found;
            }
        }
    }
    if (sess->n_active >= TBT_STREAMS_MAX)
    {
        spin_unlock(&sess->lock);
        kfree(st);
        return NULL;
    }
    set_bit(TBT_STREAM_F_ACTIVE, &st->attached);
    hlist_add_head_rcu(&st->node, &sess->streams[TBT_STREAM_BUCKET(sid)]);
    sess->n_streams++;
    sess->n_active++;
    spin_unlock(&sess->lock);
    return st;
}

/* Refcounted snapshot of the active streams. The caller processes the array
 * with sess->lock dropped and puts each entry afterwards. */
int tbt_session_stream_snapshot(struct tbt_session *sess,
                                struct tbt_stream **out, int max)
{
    int n = 0, i;

    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        struct tbt_stream *st;

        hlist_for_each_entry(st, &sess->streams[i], node)
        {
            if (READ_ONCE(st->state) == TBT_ST_DEAD)
                continue;
            if (n >= max)
            {
                sess->dbg_snap_overflow++;
                break;
            }
            if (refcount_inc_not_zero(&st->refs))
                out[n++] = st;
        }
        if (n >= max)
            break;
    }
    spin_unlock(&sess->lock);
    return n;
}

/* Hash lookup for callers that already hold sess->lock.  The hash reference
 * keeps the object alive; no extra reference is taken. */
static struct tbt_stream *tbt_stream_find_locked(struct tbt_session *sess,
                                                 u64 sid)
{
    struct tbt_stream *st;

    hlist_for_each_entry(st, &sess->streams[TBT_STREAM_BUCKET(sid)], node)
        if (st->sid == sid)
            return st;
    return NULL;
}

/* The old stream has started its close handshake.  P1 may hand the session
 * to a new sid once the old stream can no longer carry application data:
 * either it is DEAD, it has seen the peer's FIN, or its socket is closing.
 * The near side only starts the next stream after its own TCP_CLOSE, so this
 * is a safety net for the far side's lag. */
static bool tbt_stream_is_closing(const struct tbt_stream *st)
{
    struct sock *sk = READ_ONCE(st->sk);

    if (READ_ONCE(st->state) == TBT_ST_DEAD)
        return true;
    if (READ_ONCE(st->rx_fin_seen))
        return true;
    if (sk)
    {
        u8 s = READ_ONCE(sk->sk_state);

        if (s == TCP_CLOSE_WAIT || s == TCP_LAST_ACK ||
            s == TCP_CLOSING || s == TCP_CLOSE)
            return true;
    }
    return false;
}

int tbt_session_bind_sid(struct tbt_session *sess, u64 sid, bool open)
{
    bool adopt = false, drop = false;

    if (!sid)
        return TBT_BIND_DROP;

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing))
    {
        drop = true;
    }
    else if (sess->tunnel->cfg.role == TBT_ROLE_NEAR)
    {
        struct tbt_stream *st = tbt_stream_find_locked(sess, sid);

        /* The near side allocates every sid. Frames for an unknown or
         * tombstoned sid are late; a known live stream accepts any type. */
        if (!st || READ_ONCE(st->state) == TBT_ST_DEAD ||
            st->tombstone_until_ns)
            drop = true;
    }
    else
    {
        struct tbt_stream *st = tbt_stream_find_locked(sess, sid);

        if (st && (READ_ONCE(st->state) == TBT_ST_DEAD ||
                    st->tombstone_until_ns))
            drop = true; /* late frame for a tombstone */
        else if (!st && !open)
            drop = true; /* DATA/FIN/RST cannot create a stream */
        else if (st && open && tbt_stream_is_closing(st))
            drop = true; /* late OPEN for a closing stream */
        else if (!st)
        {
            sess->far_sid = sid; /* last/primary compatibility only */
            adopt = true;
        }
    }
    spin_unlock(&sess->lock);

    if (drop)
        return TBT_BIND_DROP;
    if (adopt && sess->tunnel->cfg.role == TBT_ROLE_FAR)
    {
        struct tbt_stream *ast = tbt_stream_get(sess, sid, true, false);

        if (ast)
        {
            tbt_far_open_seen(ast);
            tbt_stream_put(ast);
        }
    }
    return TBT_BIND_OK;
}

static void tbt_stream_tombstone_locked(struct tbt_stream *st)
{
    st->tombstone_until_ns = ktime_get_ns() +
        (u64)TBT_TOMBSTONE_MS * NSEC_PER_MSEC;
}

/* Detach a terminal stream from the session. `far_sid` is only a
 * last/primary compatibility pointer now; every per-stream ack and hole
 * state is released with the stream. */
void tbt_stream_detach_from_session(struct tbt_stream *st)
{
    struct tbt_session *sess = st->sess;
    bool detached;

    spin_lock(&sess->lock);
    detached = test_and_clear_bit(TBT_STREAM_F_ACTIVE, &st->attached);
    if (detached && sess->n_active)
        sess->n_active--;
    if (READ_ONCE(sess->far_sid) == st->sid)
        WRITE_ONCE(sess->far_sid, 0);
    tbt_stream_tombstone_locked(st);
    spin_unlock(&sess->lock);

    /* A graceful FIN path never queues w_kill, so without this the reaper
     * would never run and the tombstone (and its socket) would stay forever.
     * The tombstone deadline still decides when the stream may be unlinked. */
    if (detached && !READ_ONCE(sess->closing))
        mod_delayed_work(system_dfl_wq, &sess->w_reap,
                         msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
}

/* Drop queued payload for a dead sid.  Control frames stay: the far side
 * still needs the FIN/RST.  Lives here, not in tbt_tx.c, so P1 does not
 * touch the data-plane file. */
static void tbt_tx_purge_stream(struct tbt_session *sess, struct tbt_stream *st)
{
    LIST_HEAD(free);
    struct tbt_frame *fr, *tmp;

    spin_lock(&sess->tx_lock);
    list_for_each_entry_safe(fr, tmp, &sess->tx_q, list)
    {
        u8 type = fr->hdr.type;

        if (fr->st != st)
            continue;
        if (type != TBT_F_DATA)
            continue;
        list_del(&fr->list);
        sess->tx_queued -= fr->len;
        list_add_tail(&fr->list, &free);
    }
    spin_unlock(&sess->tx_lock);
    list_for_each_entry_safe(fr, tmp, &free, list)
    {
        list_del(&fr->list);
        tbt_tx_frame_free(fr);
    }
}

void tbt_stream_kill(struct tbt_stream *st, u8 reason)
{
    struct sock *sk = READ_ONCE(st->sk);

    /* One winner.  A read-then-write let two carriers both queue w_kill and
     * leak one reference. */
    if (test_and_set_bit(TBT_STREAM_F_DYING, &st->attached))
        return;
    st->kill_reason = reason;
    WRITE_ONCE(st->state, TBT_ST_DEAD);

    /* Detach the sid from the session now: this is the gate that lets the
     * next queued public connection start.  It also resets the session ACK
     * space and stamps the tombstone deadline. */
    tbt_stream_detach_from_session(st);
    tbt_tx_purge_stream(st->sess, st);

    /* Wake anything asleep on the socket first: a delivery blocked in
     * tcp_sendmsg_locked() would otherwise keep w_drain running and the reset
     * work would wait forever in cancel_work_sync(). The reset close itself
     * sleeps (detach, socket release), so it runs in a work item. */
    if (sk && sk->sk_socket)
        kernel_sock_shutdown(sk->sk_socket, SHUT_RDWR);

    /* w_kill can outlive the hash reference; it owns this reference. */
    refcount_inc(&st->refs);
    if (!queue_work(system_dfl_wq, &st->w_kill))
        tbt_stream_put(st);
}

void tbt_stream_kill_by_sid(struct tbt_session *sess, u64 sid, u8 reason)
{
    struct tbt_stream *st = tbt_stream_get(sess, sid, false, false);

    if (!st)
        return;
    tbt_stream_kill(st, reason);
    tbt_stream_put(st);
}

#define TBT_REAP_BATCH 16

static void tbt_reap_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_reap);
    struct tbt_stream *dead[TBT_REAP_BATCH];
    struct tbt_stream *st;
    struct hlist_node *tmp;
    u64 now = ktime_get_ns();
    int n = 0, i;
    bool again = false;

    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        hlist_for_each_entry_safe(st, tmp, &sess->streams[i], node)
        {
            /* Reap a stream once it can no longer carry data: it is DEAD, or
             * it has detached from the session after both FINs.  The
             * tombstone check below still decides when the sid may vanish. */
            if (READ_ONCE(st->state) != TBT_ST_DEAD &&
                test_bit(TBT_STREAM_F_ACTIVE, &st->attached))
                continue;
            if (!st->tombstone_until_ns || now < st->tombstone_until_ns)
            {
                again = true;
                continue;
            }
            if (test_and_set_bit(TBT_STREAM_F_UNLINKED, &st->attached))
                continue;
            if (READ_ONCE(sess->far_sid) == st->sid)
                WRITE_ONCE(sess->far_sid, 0);
            hlist_del_rcu(&st->node);
            sess->n_streams--;
            dead[n++] = st;
            if (n == TBT_REAP_BATCH)
            {
                again = true;
                goto out;
            }
        }
    }
out:
    spin_unlock(&sess->lock);

    for (i = 0; i < n; i++)
    {
        /* No stream owns this socket any more; release it or every graceful
         * connection leaks one.  Called with sess->lock dropped: release takes
         * it again, and cancel_work_sync(&w_kill) must not run under a lock
         * the kill work itself needs. */
        tbt_stream_release_socket(dead[i]);
        tbt_stream_put(dead[i]); /* drops the hash reference */
    }

    if (again && !READ_ONCE(sess->closing))
        mod_delayed_work(system_dfl_wq, &sess->w_reap,
                         msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
}

// ---------------------------------------------------------------------------
// Session lifecycle (C3: one session per tunnel, n carriers, one stream)

int tbt_session_create(struct tbt_tunnel *t)
{
    struct tbt_session *s;

    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s)
        return -ENOMEM;
    s->net = get_net(t->net);
    s->tunnel = t;
    s->def_ops = t->def_ops;
    refcount_set(&s->refs, 1);
    spin_lock_init(&s->lock);
    spin_lock_init(&s->tx_lock);
    mutex_init(&s->pump_lock);
    INIT_LIST_HEAD(&s->tx_q);
    INIT_LIST_HEAD(&s->prov);
    INIT_LIST_HEAD(&s->dead);
    INIT_LIST_HEAD(&s->pending);
    s->n_pending = 0;
    s->n_car = t->cfg.n + t->cfg.m;
    s->n_pri = t->cfg.n;
    /* Every session-owned work item is initialised at birth. cancel_work_sync()
     * on a work_struct that was never INIT_WORK'd is a WARNING and, once the
     * memory is reused, a call through a stale function pointer. */
    INIT_WORK(&s->w_replace, tbt_replace_work);
    INIT_DELAYED_WORK(&s->w_pending, tbt_pending_work);
    INIT_DELAYED_WORK(&s->w_fbk, tbt_fbk_work);
    INIT_DELAYED_WORK(&s->w_reap, tbt_reap_work);
    for (int i = 0; i < TBT_STREAM_HT; i++)
        INIT_HLIST_HEAD(&s->streams[i]);
    /* A tunnel `nonce=` turns a far side's trust-on-first-use into a
     * pre-shared nonce: the first HELLO must already carry this value.  The
     * nonce travels in clear text in HELLO, so it is a session identifier and
     * an anti-confusion token, not a password; WireGuard is the confidentiality
     * layer.  Without it the historical behaviour is unchanged: a random
     * nonce for a near session, or adoption of the first HELLO on a far one. */
    if (t->cfg.nonce_set)
    {
        memcpy(s->nonce, t->cfg.nonce, sizeof(s->nonce));
        s->nonce_pinned = true;
        s->nonce_set = true;
    }
    else
    {
        get_random_bytes(s->nonce, sizeof(s->nonce));
    }
    /* A configured far `peer=` fixes the anchor before the first HELLO.  The
     * port is grammar filler: the far side compares the carrier's source
     * address only.  Without `peer=` or `nonce=` the first HELLO that passes
     * the nonce check still fixes the anchor (trust-on-first-use), but say so
     * once because that first connection chooses the session. */
    if (t->cfg.role == TBT_ROLE_FAR && t->cfg.peer_addr)
    {
        s->anchor_addr = t->cfg.peer_addr;
        s->anchor_set = true;
    }
    if (t->cfg.role == TBT_ROLE_FAR && !t->cfg.nonce_set && !t->cfg.peer_addr)
        pr_warn_once("tbtun: tunnel %llu far side uses trust-on-first-use; "
                     "configure nonce= or peer= to pin it\n",
                     (unsigned long long)t->cfg.id);
    s->sid_prefix = get_random_u32() & 0xffff; // 16 random bits (§3.6)
    s->next_sid = 0;                           // monotonic 24-bit counter
    t->sess = s;
    return tbt_carriers_start(t, s);
}

// Tear a session down in the only order that is safe for module unload:
//   1. stop new work from being scheduled (listener already stopped by caller),
//   2. stop the feedback/replacement/target workers,
//   3. detach every stream from its socket (clears sk_user_data under the
//      callback lock, then cancel_work_sync),
//   4. close the target connection,
//   5. stop and free every carrier,
//   6. free.
// Skipping (3) leaves a socket whose sk_data_ready points into freed module
// text; the next packet then oopses in another module's stack frame.
void tbt_session_destroy(struct tbt_tunnel *t)
{
    struct tbt_session *s = t->sess;
    struct tbt_stream *st;
    struct hlist_node *tmp;
    struct hlist_node *pos, *next;
    int i;

    if (!s)
        return;
    t->sess = NULL;
    spin_lock(&s->lock);
    s->closing = true;
    spin_unlock(&s->lock);

    cancel_delayed_work_sync(&s->w_fbk);
    cancel_work_sync(&s->w_replace);
    cancel_delayed_work_sync(&s->w_pending);
    cancel_delayed_work_sync(&s->w_reap);

    /* Stop every pump before any stream goes away. A pump takes the stream's
     * retention list and its frames after dropping tx_lock, so a stream freed
     * first (its RCU callback releases the retains) would be used after free.
     * carriers_stop cancels w_tx and w_pump for every carrier, and the queue
     * is dropped while no pump can run. */
    tbt_carriers_stop(s); // cancels work items, releases sockets, frees structs

    /* First pass: stop every producer that can enqueue into tx_q.  The hash
     * is stable here: carriers are stopped, closing is set, and w_reap /
     * w_pending are cancelled.  Do not unlink yet; the hash reference keeps
     * each stream alive. */
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        spin_lock(&s->lock);
        pos = s->streams[i].first;
        spin_unlock(&s->lock);

        while (pos)
        {
            struct tbt_stream *v = hlist_entry(pos, struct tbt_stream, node);

            next = READ_ONCE(pos->next);
            if (refcount_inc_not_zero(&v->refs))
            {
                cancel_work_sync(&v->w_target);
                cancel_work_sync(&v->w_kill);
                tbt_sock_detach(v);
                tbt_stream_put(v);
            }
            pos = next;
        }
    }

    tbt_tx_free_queue(s);

    // Unlink every stream under the lock; detach outside it, because detach()
    // cancels work and therefore sleeps. Unlinking transfers the hash's own
    // reference to this loop, so the object cannot disappear in between.
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        for (;;)
        {
            struct tbt_stream *victim = NULL;

            spin_lock(&s->lock);
            hlist_for_each_entry_safe(st, tmp, &s->streams[i], node)
            {
                hlist_del_rcu(&st->node);
                victim = st;
                break; // one at a time: detach sleeps
            }
            spin_unlock(&s->lock);

            if (!victim)
                break;
            /* w_kill may be running and calling tbt_sock_detach itself; wait
             * for it before the session can free the stream. */
            cancel_work_sync(&victim->w_kill);
            /* Detach restores core callbacks, then release the socket this
             * stream owns.  A detached but unreleased accepted socket keeps
             * module text and oopses after unload (F2). */
            tbt_stream_release_socket(victim);
            tbt_stream_put(victim);
        }
    }

    tbt_pending_drain(s);



    /* A health check that started before `closing` was set may have queued a
     * replacement after the first cancel; no work item can do that now, so a
     * second cancel is the barrier that makes freeing the session safe. */
    cancel_delayed_work_sync(&s->w_fbk);
    cancel_work_sync(&s->w_replace);
    put_net(s->net);
    kfree(s);
}

/* Install an accepted carrier at its HELLO-named index. A live occupant is a
 * duplicate join: count it and refuse this carrier without touching the
 * session. A dead occupant (already stopping, or its socket is gone) is
 * replaced, which is how the far side accepts the near side's replacement. */
/* A near-side replacement must retire the far's slot for the old generation
 * before its HELLO can join. Marking the old carrier dead here is what lets
 * tbt_session_join() accept the new generation instead of returning EALREADY
 * (a join_dup storm, measured: 15 duplicates, 191 replacements, 32 s hole). */
void tbt_session_retire(struct tbt_session *sess, u8 index, u8 gen)
{
    struct tbt_carrier *old;

    if (!sess || index >= sess->n_car)
        return;
    spin_lock(&sess->lock);
    old = sess->car[index];
    if (old && (old->gen & 0xf) == (gen & 0xf))
        atomic_set(&old->stopping, 1);
    spin_unlock(&sess->lock);
}

int tbt_session_join(struct tbt_session *sess, struct tbt_carrier *c, u8 index)
{
    struct tbt_carrier *old;
    struct tbt_stream *snap[TBT_STREAM_SNAP_MAX];
    struct sock *csk;
    __be32 src;
    int nsnap, i;

    csk = c ? READ_ONCE(c->sk) : NULL;
    if (index >= sess->n_car || !csk)
        return -EINVAL;
    src = inet_sk(csk)->inet_daddr;
    spin_lock(&sess->lock);
    if (!sess->anchor_set || src != sess->anchor_addr)
    {
        /* tbt_rx_hello() is the only caller and already counted this
         * rejection; fail closed for any future caller.  Do not count it
         * here, or one rejection would increment twice. */
        spin_unlock(&sess->lock);
        return -EPROTO;
    }
    if (sess->closing)
    {
        spin_unlock(&sess->lock);
        return -ENOTCONN;
    }
    old = sess->car[index];
    if (old && old != c && !tbt_carrier_dead(old))
    {
        sess->session_join_dup++;
        spin_unlock(&sess->lock);
        return -EALREADY;
    }
    sess->car[index] = c;
    c->index = index;
    c->spare = index >= sess->n_pri;
    c->active = !c->spare;
    c->profile = tbt_carrier_profile(sess, index, c->spare);
    c->provisional = false;
    list_del_init(&c->node);
    if (old && old != c)
    {
        sess->carrier_replaced++;
        atomic_set(&old->stopping, 1);
        /* The replaced carrier is on no list once joined; put it on the
         * graveyard so carriers_stop/w_replace retire its socket and port. */
        if (list_empty(&old->node))
            list_add_tail(&old->node, &sess->dead);
    }
    spin_unlock(&sess->lock);

    if (old && old != c)
        schedule_work(&sess->w_replace);
    /* Our HELLO is the first frame in this direction. */
    tbt_tx_hello(sess, index, GFP_ATOMIC);
    /* Announce and target every active stream; the pump writes HELLO first
     * because it is ahead in the queue. */
    nsnap = tbt_session_stream_snapshot(sess, snap, ARRAY_SIZE(snap));
    for (i = 0; i < nsnap; i++)
    {
        tbt_tx_announce(sess, snap[i], GFP_ATOMIC);
        tbt_carrier_open_target(snap[i]);
        tbt_stream_put(snap[i]);
    }
    tbt_fbk_kick(sess, false);
    return 0;
}
