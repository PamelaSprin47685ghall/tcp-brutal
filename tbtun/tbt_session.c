/* tbt_session.c - tunnel objects, the session, the carrier array, and the
 * global list's contents.
 *
 * Owns:
 *   - turning a parsed struct tbt_cfg into a live tbt_tunnel (or rejecting it
 *     without side effects, DESIGN.md §8.3 "no partial application").
 *   - the module-wide carrier source-port bitmap and the global fallback
 *     block (§7.2).
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
#include <net/inet_sock.h>
#include <net/tcp.h>
#include <net/sock.h>

#include "tbtun.h"
#include "tbt_map.h"
#include "tbt_carrier_policy.h"
#include "tbt_portmap.h"

struct tbt_portmap tbt_ports = {
    .lock = __MUTEX_INITIALIZER(tbt_ports.lock),
    .lo = 40000,
    .hi = 44095,
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
    if (t->nlisteners)
        return "listening";
    return "idle";
}

// ---------------------------------------------------------------------------
// Port block

int tbt_portmap_set(u16 lo, u16 hi)
{
    int err = 0;

    if (!lo || lo > hi)
        return -EINVAL;
    mutex_lock(&tbt_ports.lock);
    if (!tbt_ports.used)
    {
        tbt_ports.used = bitmap_zalloc(TBT_PORTMAP_PORTS, GFP_KERNEL);
        if (!tbt_ports.used)
            err = -ENOMEM;
    }
    if (!err)
    {
        /* The bitmap is absolute: changing the window never invalidates a
         * port a live carrier still holds. A port outside the new window
         * simply stops being a candidate until its carrier frees it. */
        tbt_ports.lo = lo;
        tbt_ports.hi = hi;
        tbt_ports.configured = true;
    }
    mutex_unlock(&tbt_ports.lock);
    return err;
}

void tbt_portmap_get(u16 *lo, u16 *hi, bool *configured)
{
    mutex_lock(&tbt_ports.lock);
    *lo = tbt_ports.lo;
    *hi = tbt_ports.hi;
    *configured = tbt_read_bool(tbt_ports.configured);
    mutex_unlock(&tbt_ports.lock);
}

/* Called once from module exit, after every tunnel and carrier is gone. */
void tbt_portmap_exit(void)
{
    mutex_lock(&tbt_ports.lock);
    bitmap_free(tbt_ports.used);
    tbt_ports.used = NULL;
    mutex_unlock(&tbt_ports.lock);
}

void tbt_portmap_show(struct seq_file *m)
{
    u16 lo, hi;

    mutex_lock(&tbt_ports.lock);
    lo = tbt_ports.lo;
    hi = tbt_ports.hi;
    seq_printf(m, "# global block %u-%u (%u ports)\n", lo, hi, hi - lo + 1);
    seq_printf(m, "# %s\n", tbt_ports.configured ? "configured" : "default, not yet set");
    mutex_unlock(&tbt_ports.lock);
}

// ---------------------------------------------------------------------------
// Tunnel lifecycle

/* Control class of a configured shape (design §4.1). Phase 1 adapts only
 * n+m=8; any other pair starts at C3 (4/4), the safe fixed point, and the
 * controller may replace it. */
static u8 tbt_fec_adapt_class(u16 n, u16 m)
{
    if (n + m != 8)
        return 3;
    switch (n)
    {
    case 7: return 0;
    case 6: return 1;
    case 5: return 2;
    case 4: return 3;
    case 3: return 4;
    case 2: return 5;
    default: return 3;
    }
}

static bool tbt_cfg_sane(const struct tbt_cfg *c, int *err)
{
    unsigned int limit;

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
    /* crypto=on has no decrypt point without the FEC data plane (T0 §2.4). */
    if (c->crypto && !c->fec)
    {
        *err = -EINVAL;
        return false;
    }
    /* The control law lives in the FEC data plane; without it there is no
     * shape to adapt. The parser already rejects this pair. */
    if (c->adaptive && !c->fec)
    {
        *err = -EINVAL;
        return false;
    }
    /* The old ARQ data plane is frozen at 16 carrier slots: its path_ack_mask
     * is u16 and its FBK/WINDOW bodies hold 16 entries. Only a session that
     * asked for FEC may claim more, and even that is capped by max_carriers.
     * max_carriers is a module_param (kernel-only); the host parser cannot see
     * it, so the bound is enforced here, before any allocation or session
     * creation. The compile-time limit still applies. */
    limit = READ_ONCE(max_carriers);
    if (!c->fec && limit > TBT_LEGACY_MAX_CARRIERS)
        limit = TBT_LEGACY_MAX_CARRIERS;
    if (limit > TBT_MAX_CARRIERS_LIMIT)
        limit = TBT_MAX_CARRIERS_LIMIT;
    if (!c->n || c->n + c->m > limit)
    {
        *err = -EINVAL;
        return false;
    }
    /* A near tunnel's carriers hold one source port each. A per-tunnel block
     * smaller than the effective carrier count can never bring the session
     * up, so refuse the line instead of letting every replacement retry into
     * port_exhaust. The global block keeps its runtime behaviour: an operator
     * may widen it after the tunnel exists. */
    if (c->role == TBT_ROLE_NEAR && (c->portblock_lo || c->portblock_hi))
    {
        unsigned int slots = c->fec ? c->carriers
                                    : (unsigned int)c->n + c->m;

        if (!tbt_portmap_fits(c->portblock_lo, c->portblock_hi, slots))
        {
            *err = -EINVAL;
            return false;
        }
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
    /* The parser already names the bad map entry; this is the kernel-side
     * re-proof for any future caller that builds a struct tbt_cfg directly. */
    if (tbt_map_check(c))
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
    /* Both key copies outlive every session; this is their last holder. */
    tbt_memzero_explicit(t->cfg.crypto_key, sizeof(t->cfg.crypto_key));
    tbt_memzero_explicit(t->crypto_key_inherit,
                         sizeof(t->crypto_key_inherit));
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

/* Carry the session crypto state across a tunnel overwrite. The peer checks
 * every later HELLO against the first one it saw: a new key or a new TX
 * nonce is refused on every carrier, and the peer cannot tell the replaced
 * session's last HELLOs from the replacement's. The TX nonce therefore
 * survives whenever the role is unchanged. The key survives only when this
 * near side generated and announced it; a configured or adopted key is not
 * this tunnel's to carry. The old session is still alive here:
 * tbt_tunnel_add() destroys it after the new one is installed. */
static void tbt_tunnel_inherit_crypto(struct tbt_tunnel *t,
                                      const struct tbt_tunnel *old)
{
    struct tbt_session *s = old->sess;

    if (!t->cfg.crypto || t->cfg.role != old->cfg.role)
        return;
    if (!s)
    {
        /* The old session is already gone; the tunnel-level key and nonce
         * are the only state that can keep the far side's pin valid. */
        if (old->crypto_key_inherit_valid)
        {
            memcpy(t->crypto_key_inherit, old->crypto_key_inherit,
                   sizeof(t->crypto_key_inherit));
            t->crypto_key_inherit_valid = true;
        }
        if (old->crypto_tx_nonce_inherit_valid)
        {
            memcpy(t->crypto_tx_nonce_inherit, old->crypto_tx_nonce_inherit,
                   sizeof(t->crypto_tx_nonce_inherit));
            t->crypto_tx_nonce_inherit_valid = true;
        }
        return;
    }
    spin_lock(&s->lock);
    if (s->crypto)
    {
        memcpy(t->crypto_tx_nonce_inherit, s->crypto_tx_nonce,
               sizeof(t->crypto_tx_nonce_inherit));
        t->crypto_tx_nonce_inherit_valid = true;
    }
    else if (old->crypto_tx_nonce_inherit_valid)
    {
        memcpy(t->crypto_tx_nonce_inherit, old->crypto_tx_nonce_inherit,
               sizeof(t->crypto_tx_nonce_inherit));
        t->crypto_tx_nonce_inherit_valid = true;
    }
    if (t->cfg.role == TBT_ROLE_NEAR && !t->cfg.crypto_key_set)
    {
        bool got = false;

        if (s->crypto_have_key && !s->crypto_key_peer &&
            s->crypto_key_id == TBT_CRYPTO_KEY_ID_HELLO)
        {
            memcpy(t->crypto_key_inherit, s->crypto_key,
                   sizeof(t->crypto_key_inherit));
            got = true;
        }
        if (!got && old->crypto_key_inherit_valid)
        {
            memcpy(t->crypto_key_inherit, old->crypto_key_inherit,
                   sizeof(t->crypto_key_inherit));
            got = true;
        }
        if (got)
            t->crypto_key_inherit_valid = true;
    }
    spin_unlock(&s->lock);
}

/* Installs `cfg`, replacing an existing tunnel with the same id
 * (DESIGN.md §8.4.1 "re-applying the same line is idempotent"). The list/cap
 * section runs under tbt_lock; listener/session teardown runs
 * outside it. A session destroy that waits for a work item must not pin the
 * global lock: every /proc reader and every other config writer would queue
 * behind it (fix21 64-carrier hang: stats + two config writers + rmmod all
 * blocked behind one destroy). */
int tbt_tunnel_add(const struct tbt_cfg *cfg, struct net *net)
{
    struct tbt_tunnel *old, *t;
    int err = 0;
    bool listening = false;
    u8 i;

    if (!tbt_cfg_sane(cfg, &err))
        return err;

    mutex_lock(&tbt_lock);
    /* Replacing an existing id is always allowed; only a new id consumes a
     * slot. Check before kzalloc and before detaching `old`, so a refusal
     * leaves the running configuration untouched. */
    old = tbt_tunnel_find_locked(cfg->id);
    if (!old && tbt_tunnel_count_locked() >= READ_ONCE(max_tunnels))
    {
        mutex_unlock(&tbt_lock);
        return -ENOSPC;
    }

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
    {
        mutex_unlock(&tbt_lock);
        return -ENOMEM;
    }
    /* The caller captured this at open time: config flushes run from
     * __fput task_work during process exit, where current->nsproxy is NULL. */
    t->net = get_net(net);
    refcount_set(&t->refs, 1);
    t->cfg = *cfg;
    // Initialised here, not in tbt_listen_start: the listening socket can
    // deliver data before start returns, and sk_data_ready would then
    // schedule an uninitialised work item.
    for (i = 0; i < TBT_MAP_MAX; i++)
        INIT_WORK(&t->listeners[i].w_accept, tbt_accept_work);

    if (old)
    {
        tbt_tunnel_inherit_crypto(t, old);
        list_del_rcu(&old->node);
        tbt_listen_stop(old);
    }
    list_add_tail_rcu(&t->node, &tbt_tunnels);

    // Both roles listen: a near tunnel on its public port, a far tunnel on its
    // carrier port (DESIGN.md §2.1, §9.5).
    err = tbt_listen_start(t);
    if (err)
    {
        list_del_rcu(&t->node);
        mutex_unlock(&tbt_lock);
        goto undo;
    }
    listening = true;
    err = tbt_session_create(t);
    if (err)
    {
        list_del_rcu(&t->node);
        mutex_unlock(&tbt_lock);
        goto undo;
    }
    /* A connection accepted before the session exists would reach
     * tbt_stream_start(NULL, ...); enable accepting only now. */
    atomic_set(&t->accepting, 1);
    for (i = 0; i < t->nlisteners; i++)
        schedule_work(&t->listeners[i].w_accept);
    mutex_unlock(&tbt_lock);

    /* The old tunnel is off the list and its listener is stopped. Destroy its
     * session outside the global lock. */
    if (old)
    {
        tbt_session_destroy(old);
        tbt_tunnel_put(old);
    }
    return 0;

undo:
    if (listening)
        tbt_listen_stop(t);
    tbt_session_destroy(t);
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
    struct tbt_tunnel *t;

    mutex_lock(&tbt_lock);
    t = tbt_tunnel_find_locked(id);
    if (!t)
    {
        mutex_unlock(&tbt_lock);
        return -ENOENT;
    }
    list_del_rcu(&t->node);
    mutex_unlock(&tbt_lock);

    /* Teardown waits for session work; never hold tbt_lock across it. */
    tbt_listen_stop(t);
    tbt_session_destroy(t);
    tbt_tunnel_put(t);
    return 0;
}

/* Marks the list empty and tears every tunnel down. The list detach runs
 * under tbt_lock; listener/session teardown runs outside it, so a destroy that
 * waits for work cannot pin every /proc reader. */
void tbt_tunnel_shutdown(void)
{
    struct tbt_tunnel *t, *tmp;
    LIST_HEAD(victims);

    mutex_lock(&tbt_lock);
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
        list_add_tail(&t->node, &victims);
    }
    mutex_unlock(&tbt_lock);

    list_for_each_entry_safe(t, tmp, &victims, node)
    {
        list_del_init(&t->node);
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
            struct tbt_carrier *c = tbt_carrier_get(s, (u8)i);
            struct sock *sk;
            __be32 peer = t->cfg.peer_addr;
            __be16 port = t->cfg.peer_port;
            u16 sport;
            u32 inflight, rx_lag_ppm = 0, retx_ppm = 0, rtt;
            u32 cwnd_bytes = 0, snd_wnd = 0;
            u32 wscale = 0, rcv_wnd = 0, window_clamp = 0, rmem_alloc = 0;
            u64 uack, frames, bytes, dropped, delivery_bps = 0;

            if (!c)
                continue;
            /* A far slot whose carrier is stopping holds the socket the
             * near already replaced. Its peer port is stale; hide the row
             * until w_replace closes the socket and frees the slot. */
            if (!tbt_carrier_row_visible(t->cfg.role == TBT_ROLE_FAR,
                                         atomic_read(&c->stopping) != 0))
            {
                tbt_carrier_put(c);
                continue;
            }
            sk = tbt_carrier_sk_get(c);
            sport = c->local_port;
            rtt = READ_ONCE(c->rtt_us);
            if (sk)
            {
                peer = inet_sk(sk)->inet_daddr;
                port = inet_sk(sk)->inet_dport;
                sport = ntohs(inet_sk(sk)->inet_sport);
            }
            uack = READ_ONCE(c->uack);
            inflight = READ_ONCE(c->payload_bytes_sent) > uack ?
                (u32)(READ_ONCE(c->payload_bytes_sent) - uack) : 0;
            if (sk)
            {
                struct tcp_sock *tp;
                u32 srtt, delivered, interval, mss;

                tp = tcp_sk(sk);
                srtt = READ_ONCE(tp->srtt_us) >> 3;
                delivered = READ_ONCE(tp->rate_delivered);
                interval = READ_ONCE(tp->rate_interval_us);
                mss = READ_ONCE(tp->mss_cache);
                cwnd_bytes = READ_ONCE(tp->snd_cwnd) * mss;
                snd_wnd = READ_ONCE(tp->snd_wnd);
                /* Window-scale diagnostics: the handshake freezes
                 * rcv_wscale/window_clamp, and a receive queue that is
                 * full shows up as a small rcv_wnd with a large
                 * rmem_alloc. Both look like a small snd_wnd from the
                 * sender, so the four fields tell them apart. */
                /* Bit-fields: no READ_ONCE (it cannot take their
                 * address); a diagnostic read of a 4-bit field is fine. */
                wscale = ((u32)tp->rx_opt.rcv_wscale << 4) |
                         (tp->rx_opt.snd_wscale & 0xf);
                rcv_wnd = READ_ONCE(tp->rcv_wnd);
                window_clamp = READ_ONCE(tp->window_clamp);
                rmem_alloc = atomic_read(&sk->sk_rmem_alloc);
                if (srtt > READ_ONCE(c->rtt_us))
                    rtt = srtt;
                /* The BBR rate sample the auto window is built from, so a
                 * regression can show the input next to the window. */
                if (delivered && interval && mss)
                    delivery_bps = (u64)delivered * mss * 1000000ULL /
                                   interval;
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
            seq_printf(m, "%llu %u %u %pI4:%u %llu %u %u %s %s %u %u %u %llu %llu %llu %u %llu %u %u %u %u %u %u\n",
                       t->cfg.id, i, sport, &peer, ntohs(port), uack,
                       rtt, inflight, tbt_car_state_name(c->state),
                       tbt_profile_name(c->profile), READ_ONCE(c->recv_cnt),
                       rx_lag_ppm, READ_ONCE(c->rtt_min_us), frames, bytes,
                       dropped, retx_ppm, delivery_bps, cwnd_bytes, snd_wnd,
                       wscale, rcv_wnd, window_clamp, rmem_alloc);
            tbt_carrier_sk_put(sk);
            tbt_carrier_put(c);
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
        if (cmd->adaptive_set)
        {
            struct tbt_session *s = t->sess;

            /* The control law needs the FEC data plane; a legacy session has
             * no shape to adapt. */
            if (cmd->adaptive && !s->fec)
            {
                err = -EINVAL;
                goto out_unlock;
            }
            /* A mode=3 session can adapt only when the data plane built the
             * SC-LDPC controller from an on-ladder initial profile. */
            if (cmd->adaptive && s->tunnel &&
                s->tunnel->cfg.mode == 3 &&
                (!s->fec_tx || !s->fec_tx->adapt_sclc))
            {
                err = -EOPNOTSUPP;
                goto out_unlock;
            }
            spin_lock(&s->lock);
            WRITE_ONCE(s->adaptive, cmd->adaptive != 0);
            if (cmd->adaptive)
                s->adapt.reason = TBT_FEC_ADAPT_CTL_R_NONE;
            else
            {
                /* Fixed-shape fallback: the shape in force stays exactly as
                 * it is and the controller stops evaluating. No new epoch is
                 * started, so the c13 window keeps its last values. */
                s->adapt.state = TBT_FEC_ADAPT_STEADY;
                s->adapt.reason = TBT_FEC_ADAPT_CTL_R_OFF;
            }
            spin_unlock(&s->lock);
            err = 0;
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
        {
            TBT_DIAG("carrier action tunnel=%llu carrier=%u skipped (sess=%d n_car=%u)",
                     (unsigned long long)t->cfg.id, (unsigned)cmd->carrier,
                     !!s, s ? (unsigned)s->n_car : 0u);
            continue;
        }
        c = tbt_carrier_get(s, cmd->carrier);
        if (!c)
        {
            TBT_DIAG("carrier action tunnel=%llu carrier=%u skipped (no carrier object)",
                     (unsigned long long)t->cfg.id, (unsigned)cmd->carrier);
            continue;
        }
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
            if (tbt_carrier_sk_present(c))
                c->state = TBT_CAR_UP;
            err = 0;
        }
        else if (cmd->carrier_action == TBT_CARRIER_DROP_PPM)
        {
            WRITE_ONCE(c->drop_ppm, cmd->drop_ppm);
            WRITE_ONCE(s->fec_drop_ppm[cmd->carrier], cmd->drop_ppm);
            TBT_DIAG("drop_ppm set tunnel=%llu carrier=%u value=%u sess_drop=%u",
                     (unsigned long long)t->cfg.id, (unsigned)cmd->carrier,
                     (unsigned)cmd->drop_ppm,
                     (unsigned)READ_ONCE(s->fec_drop_ppm[cmd->carrier]));
            err = 0;
        }
        tbt_carrier_put(c);
    }
    mutex_unlock(&tbt_lock);
    return err;
}

// ---------------------------------------------------------------------------
// Carrier port block allocation (DESIGN.md §7.2)
//
// One absolute bitmap covers the whole 16-bit source port space and is
// guarded by tbt_ports.lock. A tunnel's window is its own portblock= when
// set, else the global /ports block. Overlapping windows share the bitmap, so
// they can only compete for capacity; they can never hand out the same live
// port. Ports are released when the carrier goes away, so a replacement
// carrier can reuse one immediately.

u16 tbt_port_alloc(const struct tbt_tunnel *t, enum tbt_port_alloc_err *err)
{
    enum tbt_port_alloc_err why = TBT_PORT_ALLOC_OK;
    u16 lo = 0, hi = 0, port = 0;
    int rc;

    mutex_lock(&tbt_ports.lock);
    /* The global block has a usable default (40000-44095, 4096 ports): a
     * tunnel must work without the operator writing `ports` first, and a
     * session may dial up to 256 carriers. Allocate the bitmap on first
     * use. */
    if (!tbt_ports.used)
    {
        tbt_ports.used = bitmap_zalloc(TBT_PORTMAP_PORTS, GFP_KERNEL);
        if (!tbt_ports.used)
        {
            why = TBT_PORT_ALLOC_NOMEM;
            goto out;
        }
    }
    rc = tbt_portmap_window(t->cfg.portblock_lo, t->cfg.portblock_hi,
                            tbt_ports.lo, tbt_ports.hi, &lo, &hi);
    if (!rc)
        rc = tbt_portmap_alloc(tbt_ports.used, lo, hi, &port);
    if (rc == -ENOSPC)
        why = TBT_PORT_ALLOC_EXHAUSTED;
    else if (rc)
        why = TBT_PORT_ALLOC_NOMEM; /* -EINVAL: the window is unusable */
out:
    mutex_unlock(&tbt_ports.lock);
    if (err)
        *err = why;
    return port;
}

void tbt_port_free(u16 port)
{
    mutex_lock(&tbt_ports.lock);
    tbt_portmap_free(tbt_ports.used, port);
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
    /* Every published stream holds one session reference from
     * tbt_stream_get() until this callback. */
    tbt_session_put(st->sess);
    kfree(st);
}

void tbt_stream_put(struct tbt_stream *st)
{
    if (st && refcount_dec_and_test(&st->refs))
        call_rcu(&st->rcu, tbt_stream_free_rcu);
}

/* Look up (or create) the stream for `sid`. Callers hold no lock; the hash is
 * protected by sess->lock. `created` tells the caller whether this call
 * published the object: only the publisher may run the one-shot adoption
 * (OPEN announce/target start), so 256 carriers that race on the same OPEN
 * cannot each adopt the stream. */
static struct tbt_stream *tbt_stream_get_created(struct tbt_session *sess,
                                                 u64 sid, bool alloc,
                                                 bool near, bool *created)
{
    struct tbt_stream *st;

    if (created)
        *created = false;
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
    /* The admission gate and the lifetime gate are checked under the same
     * lock as the publish: a stream cannot appear after destroy's sweep. */
    if (READ_ONCE(sess->closing) ||
        !refcount_inc_not_zero(&sess->refs) ||
        sess->n_active >= TBT_STREAMS_MAX)
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
    if (created)
        *created = true;
    return st;
}

struct tbt_stream *tbt_stream_get(struct tbt_session *sess, u64 sid,
                                  bool alloc, bool near)
{
    return tbt_stream_get_created(sess, sid, alloc, near, NULL);
}

/* Refcounted snapshot of the active streams. The caller processes the array
 * with sess->lock dropped and puts each entry afterwards. killable skips
 * streams another killer already marked and detached tombstones: a batch
 * loop then always makes progress, and its candidate set is bounded by
 * n_active rather than by the hash's tombstone population. */
static int tbt_stream_snapshot(struct tbt_session *sess,
                               struct tbt_stream **out, int max,
                               bool killable)
{
    int n = 0, i;

    spin_lock(&sess->lock);
    for (i = 0; i < TBT_STREAM_HT; i++)
    {
        struct tbt_stream *st;

        hlist_for_each_entry(st, &sess->streams[i], node)
        {
            if (READ_ONCE(st->state) == TBT_ST_DEAD ||
                (killable &&
                 (test_bit(TBT_STREAM_F_DYING, &st->attached) ||
                  !test_bit(TBT_STREAM_F_ACTIVE, &st->attached))))
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

int tbt_session_stream_snapshot(struct tbt_session *sess,
                                struct tbt_stream **out, int max)
{
    return tbt_stream_snapshot(sess, out, max, false);
}

int tbt_session_stream_snapshot_killable(struct tbt_session *sess,
                                         struct tbt_stream **out, int max)
{
    return tbt_stream_snapshot(sess, out, max, true);
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

/* Find the flow an OPEN names, or admit one from its bitmap. Returns a
 * referenced flow or an ERR_PTR. Two carriers deliver the same OPEN, so a
 * lost admission race (-EEXIST) is resolved by looking the winner up. */
static struct tbt_fec_flow *tbt_session_open_flow(struct tbt_session *sess,
                                                  u64 sid, const u8 *body,
                                                  bool *admitted)
{
    struct tbt_fec_flow *flow = tbt_fec_flow_lookup(sess, sid);

    if (admitted)
        *admitted = false;
    if (flow)
        return flow;
    flow = tbt_fec_flow_admit_mask(sess, sid, body);
    if (IS_ERR(flow) && PTR_ERR(flow) == -EEXIST)
        return tbt_fec_flow_lookup(sess, sid);
    if (admitted && !IS_ERR(flow) && flow)
        *admitted = true;
    return flow;
}

/* Bind one frame's sid to the session. OPEN may create the stream and, on a
 * FEC session, the flow its leading 32-byte bitmap describes. A repeated OPEN is
 * idempotent only when it names the groups the flow already owns. DATA, FIN
 * and RST never create a stream.
 *
 * The flow admission allocates its planes with GFP_KERNEL, so this runs in
 * the carrier RX work item, never in softirq. */
int tbt_session_bind_sid(struct tbt_session *sess, u64 sid, bool open,
                         const u8 *body, u16 len)
{
    bool near = sess->tunnel->cfg.role == TBT_ROLE_NEAR;
    bool adopt = false, drop = false, tombstone_open = false;
    struct tbt_fec_flow *flow = NULL;
    u16 target_port = 0;
    u8 route_slot = 0;
    bool flow_admitted = false;

    if (!sid)
        return TBT_BIND_DROP;
    /* v8 OPEN: fixed 36 bytes. Validate in the contract order — mask, route
     * slot, target port — before any flow is admitted. A rejected step must
     * not create a flow. */
    BUILD_BUG_ON(TBT_OPEN_BODY_LEN != TBT_FEC_BITMAP_BYTES + 4);
    BUILD_BUG_ON(TBT_HELLO_BODY_LEN_V6 != 66);
    if (open)
    {
        const struct tbt_cfg *cfg = &sess->tunnel->cfg;
        u16 i;

        if (!body || len != TBT_OPEN_BODY_LEN)
            return TBT_BIND_BAD;
        route_slot = (u8)tbt_le16(body + TBT_OPEN_ROUTE_SLOT);
        target_port = tbt_le16(body + TBT_OPEN_TARGET_PORT);
        /* group_mask is the first 32 bytes on both planes, but only a FEC
         * session may set a bit. The OPEN mask is the peer's TX partition, so
         * admission validates it against the 256-bit wire map, not the local
         * TX group count. */
        if (sess->fec)
        {
            u16 count;

            if (!tbt_fec_open_mask_valid(sess->fec_sclc,
                                         sess->fec_groups_total, body,
                                         &count))
                return TBT_BIND_BAD;
        }
        else
        {
            /* A non-FEC session has no group bitmap, so the whole mask must
             * be zero and a stale v7 OPEN cannot pass as this one. The port
             * mapping is independent of the FEC plane: route_slot and
             * target_port keep their meaning below. */
            for (i = 0; i < TBT_FEC_BITMAP_BYTES; i++)
                if (body[i])
                    return TBT_BIND_BAD;
        }
        /* route_slot indexes the mapping table both ends configure: slot 0 is
         * the listen=/target= pair and map= fills 1..nmap. The far side is
         * the security boundary; it may reach only a slot its own config
         * names, and the wire's port must be that slot's port (0 means "the
         * configured one"). A bad name is dropped, never allowed to close the
         * carrier: one forged OPEN must not take the session down. */
        if (!near)
        {
            if (!tbt_map_slot_target(cfg, route_slot, target_port,
                                     &target_port))
            {
                pr_warn_ratelimited("tbtun: tunnel %llu refused OPEN for unconfigured target port %u on slot %u\n",
                                    (unsigned long long)cfg->id,
                                    target_port, route_slot);
                return TBT_BIND_DROP;
            }
        }
        else
        {
            /* The far side never names a port back to the near: the echo
             * repeats the slot and nothing else. A near config may leave its
             * slot 0 target unset (0), so only the slot range is checked. */
            if (route_slot > cfg->nmap || target_port)
                return TBT_BIND_DROP;
        }
    }

    spin_lock(&sess->lock);
    if (READ_ONCE(sess->closing))
    {
        drop = true;
    }
    else if (near)
    {
        struct tbt_stream *st = tbt_stream_find_locked(sess, sid);

        /* The near side allocates every sid. Frames for an unknown or
         * tombstoned sid are late; a known live stream accepts any type. */
        if (!st || READ_ONCE(st->state) == TBT_ST_DEAD ||
            st->tombstone_until_ns)
            drop = true;
        else if (open && st->route_slot != route_slot)
            drop = true; /* the far echoed a different mapping slot */
    }
    else
    {
        struct tbt_stream *st = tbt_stream_find_locked(sess, sid);

        if (st && (READ_ONCE(st->state) == TBT_ST_DEAD ||
                    st->tombstone_until_ns))
        {
            /* A tombstone protects the old sid's late frames for
             * TBT_TOMBSTONE_MS. An OPEN that names it can only be a duplicate
             * of the old one: the near allocator never reuses a live or
             * tombstoned sid. Drop it, but say so; the reaper is the only
             * thing that frees the sid. */
            drop = true;
            tombstone_open = open;
        }
        if (!drop && !st && !open)
        {
            drop = true; /* DATA/FIN/RST cannot create a stream */
            atomic64_inc(&sess->bind_drop_no_stream);
        }
        else if (!drop && st && open && tbt_stream_is_closing(st))
            drop = true; /* late OPEN for a closing stream */
        else if (open)
        {
            /* A second OPEN no longer takes the session away from the first
             * stream: `far_sid` is only the last/primary compatibility
             * pointer. */
            adopt = !st;
            if (adopt)
                sess->far_sid = sid;
        }
    }
    spin_unlock(&sess->lock);

    if (drop)
    {
        if (tombstone_open)
            pr_warn_ratelimited("tbtun: tunnel %llu dropped OPEN for tombstoned sid %llu\n",
                                (unsigned long long)sess->tunnel->cfg.id,
                                (unsigned long long)sid);
        return TBT_BIND_DROP;
    }
    if (open && sess->fec)
    {
        int err;

        if (near)
        {
            /* The far side echoes the near's own bitmap. Never admit a flow
             * here: the near is the only allocator. */
            flow = tbt_fec_flow_lookup(sess, sid);
            if (!flow && sess->fec_sclc)
            {
                /* A second stream's echoed OPEN names its own sid, while the
                 * single SC-LDPC plane is keyed by the first stream's sid. */
                flow = tbt_fec_flow_shared(sess);
            }
            if (!flow)
            {
                atomic64_inc(&sess->bind_drop_no_stream);
                return TBT_BIND_DROP;
            }
        }
        else
        {
            flow = tbt_session_open_flow(sess, sid, body, &flow_admitted);
            if (IS_ERR(flow))
            {
                err = PTR_ERR(flow);
                /* A malformed or overlapping bitmap is a framing error:
                 * close the carrier. No free group only refuses this OPEN;
                 * the carrier and the other flows stay. */
                if (err == -EPROTO)
                    return TBT_BIND_BAD;
                atomic64_inc(&sess->fec_stream_refused);
                return TBT_BIND_DROP;
            }
            if (!flow)
            {
                /* The flow vanished between the admission race and the
                 * lookup: a stream without one would have no FEC plane. */
                atomic64_inc(&sess->bind_drop_no_stream);
                return TBT_BIND_DROP;
            }
        }
        {
            /* Near side compares the echoed bitmap with its own TX mask;
             * the far side compares it with the peer bitmap bound at the
             * first OPEN. A different bitmap on the same plane cannot be
             * served by one codec. */
            const u8 *bound = (!near && sess->fec_sclc && flow->rx)
                                  ? flow->rx->group_mask
                                  : flow->group_mask;

            if (!tbt_fec_primary_mask_match(sess->fec_sclc, bound, body))
            {
                /* One sid, one bitmap. A repeated OPEN that renames the
                 * groups of a live flow is refused; the local allocation
                 * stands. */
                if (flow_admitted)
                    tbt_fec_flow_abort(sess, flow);
                else
                    tbt_fec_flow_put(flow);
                atomic64_inc(&sess->bind_drop_no_stream);
                return TBT_BIND_DROP;
            }
        }
    }

    if (adopt && !near)
    {
        bool created = false;
        struct tbt_stream *ast;

        /* Every carrier that intends to adopt the stream owns one admission
         * slot until it learns whether it won the creation. Claim a lookup
         * flow's slot here, before get_created: otherwise the carrier that
         * admitted the flow can abort the last remaining slot and unlink the
         * plane while this winner is still between lookup and attach. A
         * non-SC-LDPC flow belongs to one stream, so its lookup reference
         * is not an admission and must stay a plain put. */
        if (flow && !flow_admitted && sess->fec_sclc)
        {
            if (tbt_fec_flow_claim_slot(sess, flow))
            {
                /* The admitting carrier aborted the last other slot and
                 * unlinked the plane just before this claim. Re-resolve
                 * once: the dead flow is gone from the table, so a fresh
                 * admission (or another carrier's fresh flow) is the normal
                 * path. Only a second failure, against a plane that is also
                 * dying, drops the OPEN. */
                tbt_fec_flow_put(flow);
                flow = tbt_session_open_flow(sess, sid, body,
                                             &flow_admitted);
                if (IS_ERR(flow))
                {
                    int rerr = PTR_ERR(flow);

                    /* A malformed or overlapping bitmap is a framing error,
                     * exactly as on the first admission. */
                    if (rerr == -EPROTO)
                        return TBT_BIND_BAD;
                    atomic64_inc(&sess->fec_stream_refused);
                    return TBT_BIND_DROP;
                }
                if (!flow)
                {
                    atomic64_inc(&sess->bind_drop_no_stream);
                    return TBT_BIND_DROP;
                }
                {
                    /* Re-run the one-sid-one-bitmap rule: the retry may
                     * have built a fresh plane. */
                    const u8 *bound = (!near && sess->fec_sclc && flow->rx)
                                          ? flow->rx->group_mask
                                          : flow->group_mask;

                    if (!tbt_fec_primary_mask_match(sess->fec_sclc, bound,
                                                    body))
                    {
                        if (flow_admitted)
                            tbt_fec_flow_abort(sess, flow);
                        else
                            tbt_fec_flow_put(flow);
                        atomic64_inc(&sess->bind_drop_no_stream);
                        return TBT_BIND_DROP;
                    }
                }
                if (!flow_admitted &&
                    tbt_fec_flow_claim_slot(sess, flow))
                {
                    tbt_fec_flow_put(flow);
                    atomic64_inc(&sess->bind_drop_no_stream);
                    return TBT_BIND_DROP;
                }
            }
            flow_admitted = true; /* now owns a pending slot */
        }
        ast = tbt_stream_get_created(sess, sid, true, false, &created);

        if (!ast)
        {
            spin_lock(&sess->lock);
            if (READ_ONCE(sess->far_sid) == sid)
                WRITE_ONCE(sess->far_sid, 0);
            spin_unlock(&sess->lock);
            if (flow)
            {
                if (flow_admitted)
                    tbt_fec_flow_abort(sess, flow);
                else
                    tbt_fec_flow_put(flow);
            }
            return TBT_BIND_DROP;
        }
        /* 256 carriers deliver the same OPEN. tbt_stream_get_created()
         * double-checks under sess->lock, so exactly one caller sees
         * created=true; only that caller attaches the flow and may
         * announce/target the stream. The others hold the same object, put
         * their flow reference and just put their stream reference. */
        if (created)
        {
            if (flow)
            if (flow)
            {
                if (tbt_fec_flow_attach(ast, flow)) /* moves the reference */
                {
                    /* The shared mux could not take another leg; the OPEN
                     * cannot be served, so drop it instead of leaving a
                     * stream with no data plane. */
                    tbt_stream_kill(ast, TBT_ST_DEAD);
                    tbt_stream_put(ast);
                    return TBT_BIND_DROP;
                }
            }
            tbt_far_open_seen(ast, route_slot, target_port);
        }
        else if (flow)
        {
            /* created=false proves another OPEN copy created the stream. On
             * the shared SC-LDPC plane abort() also returns this carrier's
             * admission slot. A grouped flow belongs to the winner's single
             * stream: use a plain put, because abort() would unlink the flow
             * that stream is about to attach. */
            if (sess->fec_sclc)
                tbt_fec_flow_abort(sess, flow);
            else
                tbt_fec_flow_put(flow);
        }
        tbt_stream_put(ast);
    }
    else if (flow)
    {
        if (flow_admitted)
            tbt_fec_flow_abort(sess, flow);
        else
            tbt_fec_flow_put(flow);
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

    /* Release the FEC flow before the readiness gate opens. A reader that
     * sees streams_active=0 must see the plane unlinked, not "about to be":
     * a sequential stream admitted inside this window shared a plane whose
     * only owner was already leaving, and the peer dropped every record
     * under the old sid. */
    tbt_fec_flow_detach(st);

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
        tbt_session_arm(sess, TBT_SW_REAP,
                        msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
}

/* A graceful stream leaves the session only after both halves are complete:
 * the peer's FIN has been delivered to the output socket, and our own FIN has
 * reached the wire.  A written FIN also means every DATA byte before it was
 * acknowledged: the pump holds the FIN while tx_ack is short of tx_seq.
 * Detaching on the queued FIN instead would clear ACTIVE, release the pump's
 * order gate, and let the FIN run past DATA that is still unacknowledged.
 * A kill never needs this: tbt_stream_kill() detaches unconditionally. */
void tbt_stream_maybe_detach(struct tbt_stream *st)
{
    if (!READ_ONCE(st->tx_fin_written) || !READ_ONCE(st->rx_fin_done))
        return;
    if (!test_bit(TBT_STREAM_F_ACTIVE, &st->attached))
        return;
    tbt_stream_detach_from_session(st);
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
        tbt_tx_frame_drop_account(sess, fr);
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
    /* One winner.  A read-then-write let two carriers both queue w_kill and
     * leak one reference. */
    if (test_and_set_bit(TBT_STREAM_F_DYING, &st->attached))
        return;
    st->kill_reason = reason;
    /* A FEC stream torn down by the user, the peer or the application (not
     * by the FEC TCP-failed reset) is the external-reset half of the
     * E_tunnel subset-of E_direct argument. Keep the two causes separate. */
    if (st->sess && READ_ONCE(st->sess->fec) && reason != TBT_ST_HOLE)
        atomic64_inc(&st->sess->fec_reset_user);
    WRITE_ONCE(st->state, TBT_ST_DEAD);

    /* Detach the sid from the session now: this is the gate that lets the
     * next queued public connection start.  It also resets the session ACK
     * space and stamps the tombstone deadline. */
    tbt_stream_detach_from_session(st);
    tbt_tx_purge_stream(st->sess, st);

    /* The socket wake and the reset close both sleep (inet_shutdown and
     * sock_release take the sk_lock), so w_kill does them in process context.
     * A carrier RX callback must never do it: it already holds the carrier's
     * sk_lock, and lockdep reports the second k-sk_lock-AF_INET acquisition
     * as recursive even though the sockets differ. w_kill still wakes the
     * socket before it cancels w_drain, so a sender blocked in
     * tcp_sendmsg_locked() cannot stall the reset in cancel_work_sync().
     *
     * w_kill can outlive the hash reference; tbt_stream_schedule() takes the
     * reference the work entry will return. A refused schedule means the
     * stream is already leaving; clear DYING so a later kill may retry. */
    if (!tbt_stream_schedule(st, &st->w_kill, TBT_STREAM_F_SCHED_KILL))
        clear_bit(TBT_STREAM_F_DYING, &st->attached);
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

static void tbt_reap_body(struct tbt_session *sess)
{
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
        tbt_fec_flow_detach(dead[i]);
        tbt_stream_release_socket(dead[i]);
        tbt_stream_put(dead[i]); /* drops the hash reference */
    }

    if (again && !READ_ONCE(sess->closing))
        tbt_session_arm(sess, TBT_SW_REAP,
                        msecs_to_jiffies(TBT_REAP_INTERVAL_MS));
}

static void tbt_reap_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_reap);
    bool owned = test_and_clear_bit(TBT_SW_REAP, &sess->sched);

    tbt_reap_body(sess);
    if (owned)
        tbt_session_put(sess);
}

/* One session-wide ARQ/control pump wake.  tbt_tx_wake() used to schedule
 * one work item per carrier, and all of them then serialised on pump_lock;
 * this is the same pump with one queue entry. */
static void tbt_tx_pump_work(struct work_struct *w)
{
    struct tbt_session *sess =
        container_of(to_delayed_work(w), struct tbt_session, w_tx);
    bool owned = test_and_clear_bit(TBT_SW_TX, &sess->sched);

    atomic64_inc(&sess->tx_work_runs);
    tbt_tx_pump(sess);
    if (owned)
        tbt_session_put(sess);
}

/* Map a TBT_SW_* bit to its work item. NULL means the work is not live for
 * this session: fec=off has no w_fec_tx, and w_fec_rx stays inert until the
 * FEC receive track initialises it. */
static struct delayed_work *tbt_sess_dw(struct tbt_session *s,
                                        enum tbt_sess_work w)
{
    switch (w)
    {
    case TBT_SW_POLL:
        return &s->w_poll;
    case TBT_SW_REPLACE:
        return &s->w_replace;
    case TBT_SW_PENDING:
        return &s->w_pending;
    case TBT_SW_REAP:
        return &s->w_reap;
    case TBT_SW_FBK:
        return &s->w_fbk;
    case TBT_SW_FEC_TX:
        return READ_ONCE(s->fec) ? &s->w_fec_tx : NULL;
    case TBT_SW_FEC_RX:
        /* The receive work is declared but not initialised in this cut. */
        return READ_ONCE(s->w_fec_rx.work.func) ? &s->w_fec_rx : NULL;
    case TBT_SW_TX:
        return &s->w_tx;
    case TBT_SW_FEC_TICK:
        return READ_ONCE(s->w_fec_tick.work.func) ? &s->w_fec_tick : NULL;
    case TBT_SW_FEC_RX_POLL:
        return READ_ONCE(s->w_fec_rx_poll.work.func) ? &s->w_fec_rx_poll :
                                                      NULL;
    }
    return NULL;
}

/* Authoritative admission gate: takes sess->lock, so it must run in process
 * context. Softirq callers use tbt_session_arm()'s lockless fast path. */
bool tbt_session_get(struct tbt_session *s)
{
    bool ok;

    if (!s)
        return false;
    spin_lock(&s->lock);
    ok = !s->closing && refcount_inc_not_zero(&s->refs);
    spin_unlock(&s->lock);
    return ok;
}

/* The last reference only wakes tbt_session_destroy(); it never frees. */
void tbt_session_put(struct tbt_session *s)
{
    if (s && refcount_dec_and_test(&s->refs))
        complete(&s->drained);
}

/* Arm one session work item. The bit owns the reference: the 0->1 transition
 * takes it, and either the entry point or tbt_session_cancel() returns it.
 * The fast path takes no lock, so this is safe from softirq and from code
 * already holding sess->lock. `delay` is in jiffies.
 *
 * The reference is taken before the bit is published: a canceller that sees
 * the bit must always have a reference it may return. The queue attempt can
 * still be refused. cancel_work_sync() disables a work item while it runs,
 * and a queue that races the cancel is dropped silently (kernel 6.10+
 * workqueue). The work_pending() check below detects that and returns the
 * reference here. Without it the reference would be owed to an instance that
 * never exists, so refs would never reach zero and tbt_session_destroy()
 * would wait forever -- the 0.2.1 `del 1` hang. */
bool tbt_session_arm(struct tbt_session *s, enum tbt_sess_work w,
                     unsigned long delay)
{
    struct delayed_work *dw;

    if (!s)
        return false;
    dw = tbt_sess_dw(s, w);
    if (!dw)
        return false;
    /* The common case: an instance already owns the bit and its reference.
     * Losing this wake only delays the pass; every one of these works is
     * periodic and self-rearming. */
    if (test_bit(w, &s->sched))
        return true;
    if (!refcount_inc_not_zero(&s->refs))
        return false;
    if (test_and_set_bit(w, &s->sched))
    {
        /* Lost the race to another arm. Do not retime it: a retime would let
         * tbt_session_cancel() clear the bit and return that same reference
         * while this caller re-queues the work, leaving a pending instance
         * with no reference. */
        tbt_session_put(s);
        return true;
    }
    if (READ_ONCE(s->closing))
    {
        /* A canceller may have cleared the bit and returned this reference
         * already; only the winner returns it. */
        if (test_and_clear_bit(w, &s->sched))
            tbt_session_put(s);
        return false;
    }
    mod_delayed_work(TBT_WQ, dw, delay);
    /* A queue dropped by a concurrent cancel leaves no instance to consume
     * the bit. The entry clears the bit before it runs, so a lost race with
     * the entry is not mistaken for a dropped queue: exactly one of the two
     * clears it and returns the reference. */
    if (!work_pending(&dw->work) && test_and_clear_bit(w, &s->sched))
    {
        tbt_session_put(s);
        return false;
    }
    return true;
}

/* Cancel one session work item. A pending instance never runs its entry, so
 * the canceller returns its reference. A running instance returns its own at
 * the end of the entry wrapper, and cancel_delayed_work_sync() waits for it.
 * A reference taken by an arm that raced this cancel is returned by that
 * arm's own post-queue check; the canceller must not guess at it. */
void tbt_session_cancel(struct tbt_session *s, enum tbt_sess_work w)
{
    struct delayed_work *dw;

    if (!s)
        return;
    dw = tbt_sess_dw(s, w);
    if (!dw)
        return;
    if (cancel_delayed_work_sync(dw) && test_and_clear_bit(w, &s->sched))
        tbt_session_put(s);
}


// ---------------------------------------------------------------------------
// Session lifecycle (C3: one session per tunnel, n carriers, one stream)

int tbt_session_create(struct tbt_tunnel *t)
{
    struct tbt_session *s;
    int err;

    TBT_DIAG("session create tunnel=%llu role=%u n=%u m=%u carriers=%u fec=%d crypto=%d profile=%u mode=%u w=%u",
             (unsigned long long)t->cfg.id, t->cfg.role, t->cfg.n, t->cfg.m,
             t->cfg.carriers, t->cfg.fec, t->cfg.crypto, t->cfg.profile,
             t->cfg.mode, t->cfg.w);
    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s)
        return -ENOMEM;
    s->net = get_net(t->net);
    s->tunnel = t;
    s->def_ops = t->def_ops;
    refcount_set(&s->refs, 1);
    init_completion(&s->drained);
    s->sched = 0;
    spin_lock_init(&s->lock);
    spin_lock_init(&s->tx_lock);
    spin_lock_init(&s->dial_lock);
    mutex_init(&s->pump_lock);
    init_rwsem(&s->stats_lock);
    INIT_LIST_HEAD(&s->tx_q);
    INIT_LIST_HEAD(&s->prov);
    INIT_LIST_HEAD(&s->dead_release);
    atomic_set(&s->n_prov, 0);
    INIT_LIST_HEAD(&s->pending);
    s->n_pending = 0;
    /* FEC v6 maps one share to each carrier, so the session owns all
     * `carriers` slots and none of them is a legacy spare. The old ARQ keeps
     * its n DATA carriers plus m spares. */
    s->n_car = t->cfg.fec ? t->cfg.carriers : (u16)(t->cfg.n + t->cfg.m);
    s->n_pri = t->cfg.fec ? t->cfg.carriers : t->cfg.n;
    /* Adaptive FEC control surface: the switch is effective only with the
     * FEC data plane, and the snapshot starts at the configured shape. The
     * control law replaces n/m and the class as it switches. */
    s->adaptive = t->cfg.fec && t->cfg.adaptive;
    s->adapt.n = t->cfg.n;
    s->adapt.m = t->cfg.m;
    s->adapt.groups = t->cfg.fec && t->cfg.n + t->cfg.m ?
        (u16)(t->cfg.carriers / (t->cfg.n + t->cfg.m)) : 0;
    s->adapt.class = tbt_fec_adapt_class(t->cfg.n, t->cfg.m);
    s->adapt.state = TBT_FEC_ADAPT_STEADY;
    s->adapt.reason = s->adaptive ? TBT_FEC_ADAPT_CTL_R_NONE : TBT_FEC_ADAPT_CTL_R_OFF;
    /* The sender window is a session property from birth: capture, the pump
     * and the carrier socket sizing all read it before the first ACK. */
    tbt_tx_window_init(s);
    /* Every session-owned work item is initialised at birth. cancel_work_sync()
     * on a work_struct that was never INIT_WORK'd is a WARNING and, once the
     * memory is reused, a call through a stale function pointer. */
    INIT_DELAYED_WORK(&s->w_replace, tbt_replace_work);
    INIT_DELAYED_WORK(&s->w_pending, tbt_pending_work);
    INIT_DELAYED_WORK(&s->w_fbk, tbt_fbk_work);
    INIT_DELAYED_WORK(&s->w_tx, tbt_tx_pump_work);
    INIT_DELAYED_WORK(&s->w_reap, tbt_reap_work);
    INIT_DELAYED_WORK(&s->w_poll, tbt_session_poll_work);
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
    /* ChaCha8 body obfuscation (T0 §2.3/§2.4). Each direction owns a random
     * 8-byte nonce. A configured crypto_key= is shared by both ends and
     * key_id stays 0; otherwise the near side generates the session key and
     * advertises it with key_id=1. The far side learns it from the near's
     * HELLO before its own TX context can be initialized. */
    s->crypto = t->cfg.crypto;
    bool inherited_key = false;
    if (t->cfg.crypto_key_set)
    {
        memcpy(s->crypto_key, t->cfg.crypto_key, sizeof(s->crypto_key));
        s->crypto_have_key = true;
        s->crypto_key_id = TBT_CRYPTO_KEY_ID_NONE;
    }
    if (s->crypto)
    {
        TBT_DIAG("crypto setup start tunnel=%llu role=%u key_set=%d",
                 (unsigned long long)t->cfg.id, t->cfg.role,
                 t->cfg.crypto_key_set);
        /* Replacing a tunnel must repeat this direction's nonce: the peer
         * checks every later HELLO against the first one it saw, and the
         * replacement's HELLOs are indistinguishable from the old ones.
         * Only a tunnel with no predecessor generates a fresh nonce. */
        if (t->crypto_tx_nonce_inherit_valid)
            memcpy(s->crypto_tx_nonce, t->crypto_tx_nonce_inherit,
                   sizeof(s->crypto_tx_nonce));
        else
        {
            get_random_bytes(s->crypto_tx_nonce, sizeof(s->crypto_tx_nonce));
            memcpy(t->crypto_tx_nonce_inherit, s->crypto_tx_nonce,
                   sizeof(t->crypto_tx_nonce_inherit));
            t->crypto_tx_nonce_inherit_valid = true;
        }
        if (t->cfg.role == TBT_ROLE_NEAR && !s->crypto_have_key)
        {
            /* The tunnel owns the generated key: the first near session
             * creates it, every later session for this tunnel reuses it. A
             * fresh key would make the far reject every new HELLO. */
            inherited_key = tbt_crypto_key_inherit(
                s->crypto_key, t->crypto_key_inherit,
                t->crypto_key_inherit_valid);
            if (!inherited_key)
            {
                get_random_bytes(s->crypto_key, sizeof(s->crypto_key));
                memcpy(t->crypto_key_inherit, s->crypto_key,
                       sizeof(t->crypto_key_inherit));
                t->crypto_key_inherit_valid = true;
            }
            s->crypto_have_key = true;
            s->crypto_key_id = TBT_CRYPTO_KEY_ID_HELLO;
        }
        if (s->crypto_have_key)
        {
            int rc = tbt_crypto_init(&s->crypto_ctx.tx, s->crypto_key,
                                     s->crypto_tx_nonce, 0);

            smp_store_release(&s->crypto_tx_ready, true);
            TBT_DIAG("crypto tx init tunnel=%llu key_id=%u source=%s rc=%d nonce=%*phN",
                     (unsigned long long)t->cfg.id, s->crypto_key_id,
                     t->cfg.crypto_key_set ? "configured" :
                     inherited_key ? "inherited" : "generated",
                     rc,
                     TBT_CRYPTO_NONCE_LEN, s->crypto_tx_nonce);
        }
        else
            TBT_DIAG("crypto tx init tunnel=%llu waits for the peer key",
                     (unsigned long long)t->cfg.id);
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
    /* Set before any allocation that can fail: tunnel_add() destroys a
     * partially created session through this pointer, so get_net() and
     * init_completion() have exactly one owner on every path. */
    t->sess = s;
    if (t->cfg.fec)
    {
        /* FEC v6 works in both containers: raw records, or one 40-byte record
         * per HTTP chunk with the request/response headers plaintext. The
         * data plane is one group in this cut; grouped remains -EOPNOTSUPP. */
        TBT_DIAG("fec data init tunnel=%llu n=%u m=%u carriers=%u mode=%u w=%u",
                 (unsigned long long)t->cfg.id, t->cfg.n, t->cfg.m,
                 t->cfg.carriers, t->cfg.mode, t->cfg.w);
        err = tbt_fec_data_init(s);
        TBT_DIAG("fec data init tunnel=%llu ret=%d fec=%d",
                 (unsigned long long)t->cfg.id, err, s->fec);
        if (err)
            return err;
    }
    err = tbt_carriers_start(t, s);
    TBT_DIAG("carriers_start tunnel=%llu ret=%d n_car=%u n_pri=%u",
             (unsigned long long)t->cfg.id, err, s->n_car, s->n_pri);
    if (err)
        return err;
    /* One session-wide liveness timer drives every carrier slot and every
     * provisional carrier; a per-carrier timer would cost one wakeup per
     * carrier per 50 ms at 256 carriers. */
    tbt_session_arm(s, TBT_SW_POLL, msecs_to_jiffies(50));
    TBT_DIAG("session create done tunnel=%llu n_car=%u poll_ms=50",
             (unsigned long long)t->cfg.id, s->n_car);
    return 0;
}

/* Tear a session down in the reference-counted order of
 * review/s2-merged-design.md §5.2. The invariant: after step 3 no scheduler
 * can take a new session reference, and step 10 waits until every holder has
 * returned its reference. */
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
    /* Stats readers hold a session reference but not the child objects it
     * renders (car[], fec_outq, streams). Block them before the first child
     * is cleared. The write side is released before the drained wait, since
     * a blocked reader holds one of the references that wait is for. */
    down_write(&s->stats_lock);

    /* Stop the two FEC entry points. rx->lock is a leaf; the stopping flag
     * makes record()/arm() refuse new work after this point. */
    WRITE_ONCE(s->fec_stopping, true);
    if (s->fec_rx)
    {
        spin_lock_bh(&s->fec_rx->lock);
        s->fec_rx->stopping = true;
        spin_unlock_bh(&s->fec_rx->lock);
    }
    /* The FEC transmit work must not re-arm after this point: tbt_tx_fin()
     * can still run during the stream sweep, and its flush would otherwise
     * queue a packetizer pass behind the cancel. The flag makes flush and
     * the work body return -ESHUTDOWN / do nothing. */
    if (s->fec_tx)
        WRITE_ONCE(s->fec_tx->stopping, true);

    /* Every session work returns its reference through this cancel. w_fec_rx
     * is inert until its work function is initialised; tbt_sess_dw() returns
     * NULL for it and this call is a no-op. */
    tbt_session_cancel(s, TBT_SW_POLL);
    tbt_session_cancel(s, TBT_SW_REPLACE);
    tbt_session_cancel(s, TBT_SW_PENDING);
    tbt_session_cancel(s, TBT_SW_REAP);
    tbt_session_cancel(s, TBT_SW_FBK);
    tbt_session_cancel(s, TBT_SW_FEC_TX);
    tbt_session_cancel(s, TBT_SW_FEC_RX);
    tbt_session_cancel(s, TBT_SW_FEC_TICK);
    tbt_session_cancel(s, TBT_SW_FEC_RX_POLL);
    tbt_session_cancel(s, TBT_SW_TX);

    /* Stop every carrier: cancel its works, restore callbacks, unpublish,
     * one grace period, close, put. Each put returns one session reference. */
    tbt_carriers_stop(s); // cancels work items, releases sockets, frees structs

    /* First stream pass: w_target is the only work that can re-attach, then
     * w_kill, then detach (ctrl -> read -> drain inside it). Do not unlink
     * yet; the hash reference keeps each stream alive. */
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
                tbt_stream_cancel(v, &v->w_target,
                                  TBT_STREAM_F_SCHED_TARGET);
                tbt_stream_cancel(v, &v->w_kill,
                                  TBT_STREAM_F_SCHED_KILL);
                tbt_sock_detach(v);
                tbt_stream_put(v);
            }
            pos = next;
        }
    }

    tbt_tx_free_queue(s);

    /* Second pass: unlink, release the socket, drop the hash reference. The
     * last stream reference runs tbt_stream_free_rcu() and returns the
     * session reference there. */
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
             * for it and return a pending instance's reference. */
            tbt_stream_cancel(victim, &victim->w_kill,
                              TBT_STREAM_F_SCHED_KILL);
            tbt_fec_flow_detach(victim);
            /* Detach restores core callbacks, then release the socket this
             * stream owns.  A detached but unreleased accepted socket keeps
             * module text and oopses after unload (F2). */
            tbt_stream_release_socket(victim);
            tbt_stream_put(victim);
        }
    }

    tbt_pending_drain(s);

    /* Assertion pass: closing + the FEC stopping flags mean nothing should
     * have re-armed these. A leak shows up as a cancel that still found a
     * pending instance; the reference count below is the final judge. */
    tbt_session_cancel(s, TBT_SW_FBK);
    tbt_session_cancel(s, TBT_SW_REPLACE);
    tbt_session_cancel(s, TBT_SW_FEC_TX);
    tbt_session_cancel(s, TBT_SW_FEC_RX);
    tbt_session_cancel(s, TBT_SW_FEC_TICK);
    tbt_session_cancel(s, TBT_SW_FEC_RX_POLL);
    tbt_session_cancel(s, TBT_SW_TX);

    tbt_fec_data_free(s);

    up_write(&s->stats_lock);

    /* Drop the tunnel's reference, then wait for every stream/work/carrier
     * reference. No scheduler can take a new one after the steps above. */
    tbt_session_put(s);
    if (!wait_for_completion_timeout(&s->drained,
                                     msecs_to_jiffies(TBT_SESSION_DRAIN_MS)))
    {
        /* A reference nobody returns used to block this thread forever
         * (0.2.1: `del 1` waited hours in D state and pinned rmmod behind
         * it). Do not free a session that a holder can still reach: report
         * what is left and leak it. */
        pr_err("tbtun: tunnel %llu session destroy: %u references still held after %u ms (sched=%#lx streams=%u active=%u carriers=%u); session leaked\n",
               (unsigned long long)t->cfg.id,
               (unsigned int)refcount_read(&s->refs), TBT_SESSION_DRAIN_MS,
               READ_ONCE(s->sched), (unsigned int)s->n_streams,
               (unsigned int)s->n_active, (unsigned int)s->n_car);
        return;
    }

    tbt_crypto_pair_wipe(&s->crypto_ctx);
    tbt_memzero_explicit(s->crypto_key, sizeof(s->crypto_key));
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
    bool retired = false;

    if (!sess || index >= sess->n_car)
        return;
    spin_lock(&sess->lock);
    old = sess->car[index];
    /* RETIRE names the *peer's* generation of the victim. `old->gen` is this
     * side's own allocation counter and drifts from the peer's (every failed
     * or refused dial increments one side only), so comparing it here can
     * miss the victim -- leaving it to refuse the replacement -- or match a
     * newer carrier and kill it. `peer_gen` is the generation the peer named
     * in its HELLO for this slot, which is what the RETIRE body carries. */
    if (old && (old->peer_gen & 0xf) == (gen & 0xf))
    {
        atomic_set(&old->stopping, 1);
        retired = true;
    }
    spin_unlock(&sess->lock);
    /* Close the replaced socket now, not at the next poll: until w_replace
     * runs, the far holds a dead 5-tuple and its peers row names the old
     * generation. */
    if (retired)
        tbt_session_arm(sess, TBT_SW_REPLACE, 0);
}

int tbt_session_join(struct tbt_session *sess, struct tbt_carrier *c, u8 index)
{
    struct tbt_carrier *old;
    struct tbt_stream **snap;
    struct sock *csk;
    __be32 src;
    int nsnap, i;

    if (!c || index >= sess->n_car)
        return -EINVAL;
    csk = tbt_carrier_sk_get(c);
    if (!csk)
        return -EINVAL;
    src = inet_sk(csk)->inet_daddr;
    tbt_carrier_sk_put(csk);
    spin_lock(&sess->lock);
    /* w_replace() marks a provisional carrier stopping when it moves it to
     * its victims list; a HELLO that arrives after that must not reinstall
     * it, because the victim is released and freed outside the lock. */
    if (atomic_read(&c->stopping))
    {
        spin_unlock(&sess->lock);
        return -ENOTCONN;
    }
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
        /* A HELLO from the same or a newer generation supersedes the slot.
         * The near replaces a carrier as soon as its own bytes stop being
         * acknowledged, which is often before this side has noticed anything
         * wrong with the old socket: the old connection is still ESTABLISHED
         * here while the near has already abandoned it and dialled the
         * replacement. A retry of the same carrier keeps its generation, so
         * an equal generation is also a replacement: refusing it left the far
         * holding a half-open socket in the slot while the near retried
         * forever. Only an older generation (the backward half of the
         * four-bit ring) is a duplicate. */
        if (!tbt_carrier_gen_supersedes(c->peer_gen, old->peer_gen))
        {
            sess->session_join_dup++;
            spin_unlock(&sess->lock);
            return -EALREADY;
        }
    }
    rcu_assign_pointer(sess->car[index], c);
    c->index = index;
    c->spare = index >= sess->n_pri;
    c->active = !c->spare;
    c->profile = tbt_carrier_profile(sess, index, c->spare);
    c->joined = true;
    if (c->provisional)
    {
        /* Leaves the bounded provisional list; the cap in
         * tbt_carrier_adopt() counts it until now. */
        c->provisional = false;
        atomic_dec(&sess->n_prov);
    }
    list_del_init(&c->node);
    if (old && old != c)
    {
        atomic64_inc(&sess->carrier_replaced);
        atomic_set(&old->stopping, 1);
        /* w_replace may drain dead_release and free the object as soon as
         * this lock is dropped; keep it alive through stats_retire(). */
        refcount_inc(&old->refs);
        /* The socket release sleeps, and this runs under the carrier's
         * socket lock, so it cannot happen here. w_replace drains this list
         * in bounded batches instead of re-walking all of `dead` every pass. */
        if (list_empty(&old->release_node))
            list_add_tail(&old->release_node, &sess->dead_release);
    }
    spin_unlock(&sess->lock);

    if (old && old != c)
    {
        /* The old slot was dead or stopping, so no pump can still be writing
         * it; move its counters now, while its values are final. Taking
         * pump_lock here would deadlock: this runs under the carrier socket
         * lock, and a pump holding pump_lock may wait for that same lock. */
        tbt_carrier_stats_retire(sess, old);
        tbt_carrier_put(old);
        tbt_session_arm(sess, TBT_SW_REPLACE, 0);
    }
    /* Our HELLO is the first frame in this direction. */
    tbt_tx_hello(sess, index, GFP_ATOMIC);
    /* Announce and target every active stream; the pump writes HELLO first
     * because it is ahead in the queue. HELLO runs under the carrier's
     * socket lock, so the snapshot is allocated atomically instead of
     * costing a TBT_STREAM_SNAP_MAX array on the stack. A refused
     * allocation is repaired by the 50 ms poll: a carrier whose up flag
     * never rose has every stream's OPEN re-announced there. */
    snap = kmalloc_array(TBT_STREAM_SNAP_MAX, sizeof(*snap), GFP_ATOMIC);
    if (!snap)
    {
        tbt_fbk_kick(sess, false);
        return 0;
    }
    nsnap = tbt_session_stream_snapshot(sess, snap, TBT_STREAM_SNAP_MAX);
    for (i = 0; i < nsnap; i++)
    {
        /* The stream's announce bookkeeping stores four bits of the carrier
         * generation. A replacement whose generation is 16, 32, ... greater
         * than the dead one then looks "already announced" and the OPEN is
         * never queued, leaving a connected carrier at up=0 forever. The
         * slot changed generation, so forget the old announcement first. */
        if (index < TBT_MAX_CARRIERS_LIMIT)
            clear_bit(index, snap[i]->announced_mask);
        tbt_tx_announce(sess, snap[i], GFP_ATOMIC);
        tbt_carrier_open_target(snap[i]);
        tbt_stream_put(snap[i]);
    }
    kfree(snap);
    tbt_fbk_kick(sess, false);
    return 0;
}
