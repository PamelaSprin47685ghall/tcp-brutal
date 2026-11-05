/* tbt_procfs.c - /proc/net/tbtun/{config,tunnels,ports,stats}.
 *
 * Owns:
 *   - the /proc files and their permissions (0600 root, DESIGN.md §13).
 *   - the line-buffered writer of §8.4.1: apply only on newline or at
 *     release(), -E2BIG past TBT_LINE_MAX, -EINVAL on a bad line with no
 *     partial application, first error wins for a multi-line cat.
 *   - the canonical rendering of `config`, so `cat config > backup` and
 *     `cat backup > config` round-trip (§8.4).
 *
 * Must never:
 *   - parse a line itself. The grammar lives in tbt_parse.c, which compiles
 *     into host/test_parse.c as well, so the tested code is the shipped code.
 *   - apply part of a line. Every directive is parsed into a local struct and
 *     committed in one call under tbt_lock, or rejected whole.
 *   - trust a peer- or shell-controlled length; all parsing is bounded by the
 *     line buffer.
 *   - hold tbt_lock while starting a listener that sleeps on socket locks
 *     longer than necessary; tbt_tunnel_add owns that ordering.
 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/overflow.h>
#include <net/net_namespace.h>
#include <net/tcp.h>

#include "tbtun.h"

#define TBT_CFG_NAME TBT_PROC_CONFIG
#define TBT_TUNNELS_NAME TBT_PROC_TUNNELS
#define TBT_PORTS_NAME TBT_PROC_PORTS
#define TBT_PEERS_NAME TBT_PROC_PEERS
#define TBT_STATS_NAME TBT_PROC_STATS
#define TBT_HTTP_NAME TBT_PROC_HTTP

// ---------------------------------------------------------------------------
// config: write

// Apply one complete line. Returns 0, or a negative errno that the write()
// propagates so `echo` fails visibly (DESIGN.md §8.4.1).
struct tbt_cfg_file {
    struct tbt_linebuf lb;
    struct net *net; /* captured at open; valid for every line at release */

    /* Read snapshot. `config` is write-line-buffered, so it cannot be a
     * seq_file; the snapshot is rendered once per open file description and
     * served by offset. The mutex protects out/out_len/snapshot_valid against
     * a concurrent read() and write() on the same fd. */
    struct mutex lock;
    char *out;
    size_t out_len;
    bool snapshot_valid;
};

static void tbt_cfg_drop_snapshot(struct tbt_cfg_file *cf)
{
    mutex_lock(&cf->lock);
    kvfree(cf->out);
    cf->out = NULL;
    cf->out_len = 0;
    cf->snapshot_valid = false;
    mutex_unlock(&cf->lock);
}

static int tbt_config_apply(char *line, struct net *net)
{
    struct tbt_parse_error perr = { NULL, NULL };
    struct tbt_cmd cmd;
    int err;

    err = tbt_parse_line(line, &cmd, &perr);
    if (err)
    {
        if (err == TBT_PARSE_EAFNOSUPPORT)
            pr_info_once("tbtun: IPv6 is unsupported in v0; rejected a config line\n");
        pr_debug("tbtun: config rejected (%s%s%s): %d\n",
                 perr.key ? perr.key : "", perr.key && perr.why ? ": " : "",
                 perr.why ? perr.why : "invalid line", err);
        return err;
    }

    switch (cmd.kind)
    {
    case TBT_CMD_NONE:
        return 0;
    case TBT_CMD_TUNNEL:
        mutex_lock(&tbt_lock);
        err = tbt_tunnel_add(&cmd.cfg, net);
        mutex_unlock(&tbt_lock);
        return err;
    case TBT_CMD_DEL:
        mutex_lock(&tbt_lock);
        err = tbt_tunnel_del(cmd.id);
        mutex_unlock(&tbt_lock);
        return err;
    case TBT_CMD_FLUSH:
        mutex_lock(&tbt_lock);
        tbt_tunnel_shutdown();
        mutex_unlock(&tbt_lock);
        return 0;
    case TBT_CMD_CARRIER:
    case TBT_CMD_SESSION:
        return tbt_action_apply(&cmd);
    }
    return -EINVAL;
}

// Parse whole lines out of the buffer; keep an unterminated tail. Returns the
// first error seen, but keeps going so `cat file > config` applies every line.
static int tbt_config_drain(struct tbt_linebuf *lb, bool final, struct net *net)
{
    char *p, *nl;
    int first_err = 0;

    p = lb->buf;
    while ((nl = memchr(p, '\n', lb->len - (p - lb->buf))))
    {
        int err;

        *nl = '\0';
        err = tbt_config_apply(p, net);
        if (err && !first_err)
            first_err = err;
        p = nl + 1;
    }

    lb->len = lb->buf + lb->len - p;
    memmove(lb->buf, p, lb->len);

    if (final && lb->len)
    {
        int err;

        /* A buffer this full never had a newline: it is an over-long line,
         * not a valid final one. Refuse and reset; applying the prefix would
         * be a partial application. */
        if (lb->len >= TBT_LINE_MAX - 1)
        {
            lb->len = 0;
            return first_err ? first_err : -EINVAL;
        }
        lb->buf[lb->len] = '\0';
        err = tbt_config_apply(lb->buf, net);
        if (err && !first_err)
            first_err = err;
        lb->len = 0;
    }
    return first_err;
}

static int tbt_cfg_open(struct inode *inode, struct file *file)
{
    struct tbt_cfg_file *cf;

    cf = kzalloc(sizeof(*cf), GFP_KERNEL);
    if (!cf)
        return -ENOMEM;
    mutex_init(&cf->lock);
    /* The control surface is created in init_net only (DESIGN.md §8.3).
     * Capture init_net, not the writer's netns: a bind-mounted /proc from
     * init_net must not move the tunnel into the caller's netns. */
    cf->net = get_net(&init_net);
    file->private_data = cf;
    return 0;
}

static ssize_t tbt_cfg_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
    struct tbt_cfg_file *cf = file->private_data;
    struct tbt_linebuf *lb = &cf->lb;
    char tmp[TBT_LINE_MAX];
    size_t n, i;
    int err = 0;

    if (!len)
        return 0;
    /* Any write can change the rendered text; drop the snapshot so the next
     * read renders the post-write configuration. */
    tbt_cfg_drop_snapshot(cf);
    if (len > TBT_LINE_MAX)
        len = TBT_LINE_MAX; // process at most one line's worth per chunk
    n = min(len, sizeof(tmp));
    if (copy_from_user(tmp, ubuf, n))
        return -EFAULT;

    for (i = 0; i < n; i++)
    {
        char c = tmp[i];

        if (c == '\n')
        {
            int e;

            lb->buf[lb->len] = '\0';
            e = lb->overflow ? TBT_PARSE_E2BIG
                             : tbt_config_apply(lb->buf, cf->net);
            if (e && !err)
                err = e;
            lb->len = 0;
            lb->overflow = false;
            continue;
        }
        if (lb->overflow)
            continue; // discard the rest of this over-long line
        if (lb->len + 1 >= TBT_LINE_MAX)
        {
            lb->overflow = true;
            if (!err)
                err = TBT_PARSE_E2BIG;
            continue;
        }
        lb->buf[lb->len++] = c;
    }

    /* A read that raced this write may have snapshotted an intermediate
     * config; drop it again so the next read sees the final state. */
    tbt_cfg_drop_snapshot(cf);
    return err ? err : len;
}

static int tbt_cfg_release(struct inode *inode, struct file *file)
{
    struct tbt_cfg_file *cf = file->private_data;

    if (cf)
    {
        tbt_config_drain(&cf->lb, true, cf->net); // flush a final line
        mutex_lock(&cf->lock);
        kvfree(cf->out);
        mutex_unlock(&cf->lock);
        put_net(cf->net);
        kfree(cf);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// config: read

// Render every tunnel into one exact-size snapshot. The first pass measures
// with a stack line buffer; the second fills the kvmalloc'd buffer under the
// same tbt_lock, so the list cannot change between passes. This is process
// context (read/llseek), tbt_lock is a mutex, and kvmalloc(GFP_KERNEL) may
// sleep; no atomic context is involved. A line that does not fit in
// TBT_LINE_MAX is a visible -E2BIG, never a silent truncation.
//
// tbt_render_cfg() returns the byte count *excluding* the NUL and needs room
// for n + 1 bytes. The concatenated snapshot therefore needs one extra byte:
// the last line's NUL lands at out[total], while intermediate NULs are
// overwritten by the next line. cf->out_len stays `total` (the NUL is not
// part of the file).
static int tbt_config_snapshot(struct tbt_cfg_file *cf)
{
    struct tbt_tunnel *t;
    char line[TBT_LINE_MAX];
    char *out;
    size_t total = 0, off = 0, alloc;
    int n, err = 0;

    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        n = tbt_render_cfg(&t->cfg, line, sizeof(line));
        if (n < 0)
        {
            err = n;
            goto unlock;
        }
        if ((size_t)n > (size_t)-1 - total)
        {
            err = -E2BIG;
            goto unlock;
        }
        total += (size_t)n;
    }
    if (!total)
    {
        cf->out = NULL;
        cf->out_len = 0;
        cf->snapshot_valid = true;
        goto unlock;
    }
    if (total == (size_t)-1)
    {
        err = -E2BIG;
        goto unlock;
    }
    alloc = total + 1; /* room for the final NUL */
    out = kvmalloc(alloc, GFP_KERNEL);
    if (!out)
    {
        err = -ENOMEM;
        goto unlock;
    }
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        n = tbt_render_cfg(&t->cfg, out + off, alloc - off);
        if (n < 0)
        {
            kvfree(out);
            err = n;
            goto unlock;
        }
        off += (size_t)n;
    }
    if (off != total)
    {
        kvfree(out);
        err = -E2BIG;
        goto unlock;
    }
    cf->out = out;
    cf->out_len = total;
    cf->snapshot_valid = true;

unlock:
    mutex_unlock(&tbt_lock);
    return err;
}

static ssize_t tbt_cfg_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
    struct tbt_cfg_file *cf = file->private_data;
    size_t out;
    int err;

    if (!len)
        return 0;
    if (*ppos < 0)
        return -EINVAL;

    mutex_lock(&cf->lock);
    if (!cf->snapshot_valid)
    {
        err = tbt_config_snapshot(cf);
        if (err)
        {
            mutex_unlock(&cf->lock);
            return err;
        }
    }
    if ((u64)*ppos >= (u64)cf->out_len)
    {
        mutex_unlock(&cf->lock);
        return 0;
    }
    out = min_t(size_t, len, cf->out_len - (size_t)*ppos);
    if (copy_to_user(ubuf, cf->out + (size_t)*ppos, out))
    {
        mutex_unlock(&cf->lock);
        return -EFAULT;
    }
    *ppos += (loff_t)out;
    mutex_unlock(&cf->lock);
    return (ssize_t)out;
}

// `config` is not a seq_file, so it needs its own seek. SEEK_END forces the
// snapshot (the only way to know the length); SEEK_SET/SEEK_CUR only move the
// position and let read() return 0 past the end.
static loff_t tbt_cfg_llseek(struct file *file, loff_t offset, int whence)
{
    struct tbt_cfg_file *cf = file->private_data;
    loff_t pos;
    int err;

    switch (whence)
    {
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        if (check_add_overflow(file->f_pos, offset, &pos))
            return -EOVERFLOW;
        break;
    case SEEK_END:
        mutex_lock(&cf->lock);
        if (!cf->snapshot_valid)
        {
            err = tbt_config_snapshot(cf);
            if (err)
            {
                mutex_unlock(&cf->lock);
                return err;
            }
        }
        if (check_add_overflow((loff_t)cf->out_len, offset, &pos))
        {
            mutex_unlock(&cf->lock);
            return -EOVERFLOW;
        }
        mutex_unlock(&cf->lock);
        break;
    default:
        return -EINVAL;
    }
    if (pos < 0)
        return -EINVAL;
    file->f_pos = pos;
    return pos;
}

// ---------------------------------------------------------------------------
// ports

static ssize_t tbt_ports_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
    char buf[64];
    struct tbt_parse_error perr = { NULL, NULL };
    u16 lo, hi;
    int err;

    if (!len)
        return 0; /* zero-length write: no-op */
    if (len >= sizeof(buf))
        return -E2BIG; /* line too long, same as config */
    if (copy_from_user(buf, ubuf, len))
        return -EFAULT;
    buf[len] = '\0';

    err = tbt_parse_ports(buf, &lo, &hi, &perr);
    if (err)
        return err;
    /* A blank line and `ports 0-0` are no-ops: the global block cannot be
     * empty, so 0/0 means "leave it unchanged" (DESIGN.md §8.4.1). */
    if (!lo && !hi)
        return len;
    err = tbt_portmap_set(lo, hi);
    return err ? err : len;
}

static ssize_t tbt_peers_write(struct file *file, const char __user *ubuf, size_t len, loff_t *ppos)
{
    char buf[TBT_LINE_MAX];
    struct tbt_parse_error perr = { NULL, NULL };
    struct tbt_cmd cmd;
    int err;

    if (!len)
        return 0;
    if (len >= sizeof(buf))
        return -E2BIG;
    if (copy_from_user(buf, ubuf, len))
        return -EFAULT;
    buf[len] = '\0';
    err = tbt_parse_line(buf, &cmd, &perr);
    if (err)
        return err;
    if (cmd.kind == TBT_CMD_NONE)
        return len;
    err = tbt_action_apply(&cmd);
    return err ? err : (ssize_t)len;
}

// ---------------------------------------------------------------------------
// Registration

// Each read-only file gets its own seq_operations with that file's show
// callback; file->private must be a seq_file, so `config` keeps its line
// buffer in file->private_data instead (see tbt_cfg_open()).
static int tbt_seq_open(struct inode *inode, struct file *file)
{
    return seq_open(file, pde_data(inode));
}

static const struct proc_ops tbt_cfg_ops = {
    .proc_open = tbt_cfg_open,
    .proc_read = tbt_cfg_read,
    .proc_lseek = tbt_cfg_llseek,
    .proc_release = tbt_cfg_release,
    .proc_write = tbt_cfg_write,
};

static const struct proc_ops tbt_ports_ops = {
    .proc_open = tbt_seq_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = seq_release,
    .proc_write = tbt_ports_write,
};

static const struct proc_ops tbt_peers_ops = {
    .proc_open = tbt_seq_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = seq_release,
    .proc_write = tbt_peers_write,
};

static const struct proc_ops tbt_readonly_ops = {
    .proc_open = tbt_seq_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = seq_release,
};

static int tbt_show_tunnels(struct seq_file *m, void *v)
{
    tbt_tunnel_show(m);
    return 0;
}

static int tbt_show_ports(struct seq_file *m, void *v)
{
    u16 lo, hi;
    bool cfg;

    tbt_portmap_get(&lo, &hi, &cfg);
    tbt_portmap_show(m);
    seq_printf(m, "ports %u-%u\n", lo, hi);
    return 0;
}

static int tbt_show_peers(struct seq_file *m, void *v)
{
    tbt_peers_show(m);
    return 0;
}

static int tbt_show_http(struct seq_file *m, void *v)
{
    struct tbt_tunnel *t;
    struct tbt_session *best = NULL;
    u16 best_len = 0;
    char *snap;
    int err = 0;

    /* Debug-only conformance surface: the bytes the ISP actually sees. */
    if (debug < 2)
        return 0;
    /* A request block is ~380 B, but the capture holds up to
     * TBT_HTTP_REQ_MAX. Size the copy to the capture instead of a fixed 512 B
     * stack array, which would truncate a large header set silently. show()
     * runs in process context and holds no atomic lock, so GFP_KERNEL may
     * sleep; a 4 KiB stack array would be the wrong shape here. */
    snap = kmalloc(TBT_HTTP_REQ_MAX, GFP_KERNEL);
    if (!snap)
        return -ENOMEM;
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        struct tbt_session *s = t->sess;

        if (s && READ_ONCE(s->http_cap_req_len) > best_len)
        {
            best = s;
            best_len = READ_ONCE(s->http_cap_req_len);
        }
    }
    if (best)
    {
        u16 cap;
        size_t len;

        /* http_cap_req is written by the pump under pump_lock; the procfs
         * reader takes tbt_lock, so copy under the writer's lock. */
        mutex_lock(&best->pump_lock);
        cap = READ_ONCE(best->http_cap_req_len);
        len = min_t(size_t, cap, TBT_HTTP_REQ_MAX);
        memcpy(snap, best->http_cap_req, len);
        mutex_unlock(&best->pump_lock);
        err = seq_write(m, snap, len);
    }
    mutex_unlock(&tbt_lock);
    kfree(snap);
    return err;
}

static int tbt_show_stats(struct seq_file *m, void *v)
{
    struct tbt_tunnel *t;
    u64 streams = 0, gaps = 0;
    char prefix[64];
    int n;

    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        streams += t->streams;
        if (t->sess)
            gaps += READ_ONCE(t->sess->stats_gap);
    }
    mutex_unlock(&tbt_lock);
    /* The parser module renders the pinned leading block
     * (`debug_loss=`, `gaps_total=`, `streams_total=`), so the kernel emits
     * exactly the text the host test pins. A non-zero gaps count means a
     * carrier delivered out of sequence and the stream was killed rather than
     * buffered. */
    n = tbt_render_stats_prefix(tbt_debug_loss(), gaps, streams, prefix,
                                sizeof(prefix));
    if (n > 0)
        seq_write(m, prefix, n);
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        if (t->sess)
        {
            struct tbt_stream *st = NULL;
            struct tbt_session *s = t->sess;
            int i;

            /* One live snapshot per tunnel, so a stalled direction is visible
             * without a rebuild: queue depths, pause flags, the public
             * socket's send state, and the carrier's write/read cursors. */
            rcu_read_lock();
            for (i = 0; i < TBT_STREAM_HT && !st; i++)
                hlist_for_each_entry_rcu(st, &s->streams[i], node)
                    break;
            if (st)
            {
                struct sock *sk = READ_ONCE(st->sk);

                s->snap_rx_queued = READ_ONCE(st->rx_queued);
                s->snap_rx_paused = READ_ONCE(st->paused);
                s->snap_tx_paused = READ_ONCE(st->tx_paused);
                if (sk)
                {
                    s->snap_pub_state = READ_ONCE(sk->sk_state);
                    s->snap_pub_wmem = READ_ONCE(sk->sk_wmem_queued);
                    s->snap_pub_snd_una = tcp_sk(sk)->snd_una;
                    s->snap_pub_snd_nxt = tcp_sk(sk)->snd_nxt;
                }
            }
            rcu_read_unlock();
            s->snap_tx_queued = READ_ONCE(s->tx_queued);
            tbt_carrier_snapshot(s);
            seq_printf(m, "tunnel %llu capture_runs=%llu actor_calls=%llu submit_bytes=%llu "
                          "tx=%llu wire=%llu err=%llu short=%llu\n",
                       t->cfg.id, t->sess->pub_capture_runs, t->sess->pub_actor_calls,
                       t->sess->tx_submit_bytes, t->sess->stats_tx,
                       t->sess->carrier_bytes, t->sess->carrier_err,
                       t->sess->carrier_short);
            seq_printf(m, "tunnel %llu rx_work_runs=%llu rx_drain_entries=%llu "
                          "rx_drain_bytes=%llu rx_paused_skips=%llu rx_adopt_events=%llu "
                          "rx_records=%llu carrier_write_space_runs=%llu poll_rescues=%llu\n",
                       t->cfg.id, t->sess->rx_work_runs, t->sess->rx_drain_entries,
                       t->sess->rx_drain_bytes, t->sess->rx_paused_skips,
                       t->sess->rx_adopt_events, t->sess->rx_records,
                       t->sess->carrier_write_space_runs, t->sess->poll_rescues);
            seq_printf(m, "tunnel %llu rx_ready_calls=%llu rx_reads=%llu rx_read_bytes=%llu "
                          "rx_read_zero=%llu rx_resume=%llu rx_resume_busy=%llu rx_last_drained=%u "
                          "rx_proto=%llu rx_errs=%llu rx_last_err=%d tx_submit_calls=%llu "
                          "rx_delivered=%llu\n",
                       t->cfg.id, t->sess->rx_ready_calls, t->sess->rx_reads,
                       t->sess->rx_read_bytes, t->sess->rx_read_zero,
                       t->sess->rx_resume_calls, t->sess->rx_resume_busy,
                       t->sess->rx_last_drained,
                       t->sess->rx_parse_proto,
                       t->sess->rx_errs, t->sess->rx_last_err,
                       t->sess->tx_submit_calls, t->sess->rx_delivered_bytes);
            seq_printf(m, "tunnel %llu dbg rcv_nxt=%u copied=%u after=%u qlen=%u "
                          "allocf=%llu over=%llu copyf=%llu hiwat=%llu pdead=%llu "
                          "prun=%llu pmoved=%llu p_nocar=%llu p_down=%llu p_stop=%llu "
                          "p_empty=%llu p_qlen=%llu\n",
                       t->cfg.id, t->sess->pub_last_rcv_nxt, t->sess->pub_last_copied,
                       t->sess->pub_last_after, t->sess->pub_last_qlen,
                       t->sess->dbg_alloc_fail, t->sess->dbg_over_qmax,
                       t->sess->dbg_copy_fail, t->sess->dbg_pump_hiwat,
                       t->sess->dbg_pump_dead, t->sess->dbg_pump_runs,
                       t->sess->dbg_pump_moved, t->sess->dbg_pump_nocar,
                       t->sess->dbg_pump_down, t->sess->dbg_pump_stop,
                       t->sess->dbg_pump_empty, t->sess->dbg_pump_qlen);
            seq_printf(m, "tunnel %llu live rxq=%u rxp=%u txq=%u txp=%u "
                          "pub_state=%u pub_wmem=%u pub_una=%u pub_nxt=%u "
                          "car_wmem=%u car_nxt=%u car_copied=%u car_rcvbuf=%u "
                          "car_state=%u car_stop=%u car_ud=%u car_ready=%u "
                          "car_ws=%u txw_pend=%u txw_busy=%u\n",
                       t->cfg.id, s->snap_rx_queued, s->snap_rx_paused,
                       s->snap_tx_queued, s->snap_tx_paused,
                       s->snap_pub_state, s->snap_pub_wmem,
                       s->snap_pub_snd_una, s->snap_pub_snd_nxt,
                       s->snap_car_wmem, s->snap_car_rcv_nxt,
                       s->snap_car_copied, s->snap_car_rcvbuf,
                       s->snap_car_state, s->snap_car_stopping,
                       s->snap_car_ud, s->snap_car_ready_ok, s->snap_car_ws_ok,
                       s->snap_txw_pending, s->snap_txw_busy);
            seq_printf(m, "tunnel %llu txwake=%llu txwake_busy=%llu txwork=%llu\n",
                       t->cfg.id, s->tx_wake_calls, s->tx_wake_busy,
                       s->tx_work_runs);
            seq_printf(m, "tunnel %llu ws_calls=%llu\n", t->cfg.id, s->carrier_ws_calls);
            /* C3: the multi-carrier session's own counters. `up` counts the
             * carrier slots currently schedulable; `lost` and `replaced` are
             * the replacement record; `rx_reorder_max_depth` is the deepest
             * out-of-order backlog ever held, in frames. */
            {
                unsigned int up = 0, n = s->n_car;
                u32 max_depth = 0, ooo_bytes = 0, retain_bytes = 0;
                u32 ooo_frames = 0, max_stall_us = 0;
                int k;

                for (k = 0; k < s->n_car; k++)
                {
                    struct tbt_carrier *c = READ_ONCE(s->car[k]);

                    if (c && atomic_read(&c->up))
                        up++;
                }
                rcu_read_lock();
                {
                    struct tbt_stream *one = NULL;
                    int b;

                    for (b = 0; b < TBT_STREAM_HT && !one; b++)
                        hlist_for_each_entry_rcu(one, &s->streams[b], node)
                            break;
                    if (one)
                    {
                        max_depth = READ_ONCE(one->rx_max_depth);
                        ooo_bytes = READ_ONCE(one->rx_ooo_bytes);
                        ooo_frames = READ_ONCE(one->rx_ooo_frames);
                        retain_bytes = READ_ONCE(one->retain_bytes);
                        max_stall_us = READ_ONCE(one->rx_max_stall_us);
                    }
                }
                rcu_read_unlock();
                seq_printf(m, "tunnel %llu c3 n=%u up=%u lost=%llu replaced=%llu "
                              "spare_activated=%llu alloc_fail=%llu "
                              "port_exhaust=%llu port_alloc_fail=%llu connect_fail=%llu "
                              "join_dup=%llu join_bad=%llu join_nonce_reject=%llu "
                              "join_src_reject=%llu "
                              "fbk_tx=%llu fbk_rx=%llu "
                              "retx_frames=%llu retx_bytes=%llu retain_drop=%llu "
                              "reorder_overflow=%llu reorder_max_depth=%u ooo_bytes=%u "
                              "ooo_frames=%u retain_bytes=%u tx_ack=%llu rx_ack=%llu "
                              "peer_paused=%u fb_delay_us=%u rto_us=%u una_stall_us=%u\n",
                           t->cfg.id, n, up, s->carrier_lost, s->carrier_replaced,
                           s->spare_activated, s->carrier_alloc_fail,
                           s->port_exhaust, s->port_alloc_fail, s->connect_fail,
                           s->session_join_dup, s->session_join_bad,
                           s->join_nonce_reject, s->join_src_reject, s->fbk_tx,
                           s->fbk_rx, s->retx_frames, s->retx_bytes, s->retain_drop,
                           s->rx_reorder_overflow, max_depth, ooo_bytes, ooo_frames,
                           retain_bytes, READ_ONCE(s->tx_ack), READ_ONCE(s->rx_ack),
                           READ_ONCE(s->peer_paused), READ_ONCE(s->fb_delay_us),
                           READ_ONCE(s->rto_us), READ_ONCE(s->una_stall_us));
                /* Review 22: pinned control-frame lifetime. ctl_queued is the
                 * live control-queue depth; the other four are the retry,
                 * re-pin, obsolete-generation and explicit-loss record. */
                seq_printf(m, "tunnel %llu ctl ctl_lost=%llu ctl_retry=%llu "
                              "ctl_requeue=%llu ctl_obsolete=%llu ctl_queued=%u\n",
                           t->cfg.id, s->ctl_lost, s->ctl_retry,
                           s->ctl_requeue, s->ctl_obsolete,
                           READ_ONCE(s->ctl_queued));
                /* Session stream census, next to the carrier block. n_active
                 * is the non-detached stream count (the concurrency bound),
                 * n_streams includes tombstones that reap has not yet
                 * removed, and n_pending counts accepted sockets still
                 * waiting for the gate. All three are updated under
                 * sess->lock, which this reader does not take; READ_ONCE
                 * keeps each load single instead of letting the compiler
                 * fold or cache it. streams_active=0 is the proof that a
                 * drained test released every stream. */
                seq_printf(m, "tunnel %llu streams streams_active=%u n_streams=%u "
                              "n_pending=%u\n",
                           t->cfg.id, READ_ONCE(s->n_active),
                           READ_ONCE(s->n_streams), READ_ONCE(s->n_pending));
                seq_printf(m, "tunnel %llu c6 holes_declared=%llu holes_repaired=%llu "
                              "holes_unrecoverable=%llu rx_max_stall_us=%u "
                              "hole_checks=%llu gate_fail=%llu depth_fail=%llu "
                              "time_fail=%llu\n",
                           t->cfg.id, s->holes_declared, s->holes_repaired,
                           s->holes_unrecoverable, max_stall_us, s->hole_checks,
                           s->hole_gate_fail, s->hole_depth_fail, s->hole_time_fail);
                seq_printf(m, "tunnel %llu c8 profile_mismatch=%llu http_parse_err=%llu "
                              "http_status_err=%llu http_req=%llu http_resp=%llu "
                              "pad_tx=%llu drip_tx=%llu\n",
                           t->cfg.id, s->profile_mismatch, s->http_parse_err,
                           s->http_status_err, s->http_req, s->http_resp,
                           s->pad_tx, s->drip_tx);
                {
                    u64 goodput = READ_ONCE(s->tx_submit_bytes);
                    u64 dupb = READ_ONCE(s->dup_bytes);
                    u64 inflate = goodput ? (goodput + dupb) * 1000000 / goodput : 0;

                    seq_printf(m, "tunnel %llu c5 dup_tx=%llu dup_used=%llu "
                                  "dup_suppressed=%llu dup_same_path=%llu "
                                  "inflate_ppm=%llu dup_bytes=%llu\n",
                               t->cfg.id, s->dup_tx, s->dup_used, s->dup_suppressed,
                               s->dup_same_path, inflate, dupb);
                }
            }
        }
    }
    mutex_unlock(&tbt_lock);
    return 0;
}

/* seq_read_iter calls ->start unconditionally and never tolerates a missing
 * ->next: NULL there is a call through a NULL pointer, not "no iterator". One
 * record per file, produced by show(), is all any of these files needs. */
static void *tbt_seq_start(struct seq_file *m, loff_t *pos)
{
    return *pos ? NULL : SEQ_START_TOKEN;
}

static void *tbt_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
    ++*pos; /* must advance, or seq_file prints "buggy .next" */
    return NULL;
}

static void tbt_seq_stop(struct seq_file *m, void *v)
{
}

static const struct seq_operations tbt_tunnels_seq = {
    .start = tbt_seq_start,
    .next = tbt_seq_next,
    .stop = tbt_seq_stop,
    .show = tbt_show_tunnels,
};
static const struct seq_operations tbt_ports_seq = {
    .start = tbt_seq_start,
    .next = tbt_seq_next,
    .stop = tbt_seq_stop,
    .show = tbt_show_ports,
};
static const struct seq_operations tbt_peers_seq = {
    .start = tbt_seq_start,
    .next = tbt_seq_next,
    .stop = tbt_seq_stop,
    .show = tbt_show_peers,
};
static const struct seq_operations tbt_stats_seq = {
    .start = tbt_seq_start,
    .next = tbt_seq_next,
    .stop = tbt_seq_stop,
    .show = tbt_show_stats,
};
static const struct seq_operations tbt_http_seq = {
    .start = tbt_seq_start,
    .next = tbt_seq_next,
    .stop = tbt_seq_stop,
    .show = tbt_show_http,
};

static struct proc_dir_entry *tbt_proc_dir;

static struct proc_dir_entry *tbt_proc_create(const char *name, umode_t mode,
                                              const struct proc_ops *ops,
                                              const struct seq_operations *seq)
{
    return proc_create_data(name, mode, tbt_proc_dir, ops, (void *)seq);
}

int tbt_procfs_init(void)
{
    // init_net.proc_net is the real parent of /proc/net/*; it is not reachable
    // by path-walking, which is why the "net/tbtun" one-shot name creates
    // nothing at all (found in QEMU, probe A).
    tbt_proc_dir = proc_mkdir(TBT_PROC_SUBDIR, init_net.proc_net);
    if (!tbt_proc_dir)
        return -ENOMEM;

    if (!proc_create(TBT_CFG_NAME, 0600, tbt_proc_dir, &tbt_cfg_ops))
        goto fail;
    if (!tbt_proc_create(TBT_TUNNELS_NAME, 0600, &tbt_readonly_ops, &tbt_tunnels_seq))
        goto fail;
    if (!tbt_proc_create(TBT_PORTS_NAME, 0600, &tbt_ports_ops, &tbt_ports_seq))
        goto fail;
    if (!tbt_proc_create(TBT_PEERS_NAME, 0600, &tbt_peers_ops, &tbt_peers_seq))
        goto fail;
    if (!tbt_proc_create(TBT_STATS_NAME, 0600, &tbt_readonly_ops, &tbt_stats_seq))
        goto fail;
    if (!tbt_proc_create(TBT_HTTP_NAME, 0400, &tbt_readonly_ops, &tbt_http_seq))
        goto fail;
    return 0;

fail:
    /* review 34: never remove names one by one.  Entries after the failed
     * one were never created, and remove_proc_entry() WARNs on a missing
     * name.  tbt_procfs_exit() removes the subtree, which deletes exactly
     * the children that exist and is silent when a partial failure left
     * none. */
    tbt_procfs_exit();
    return -ENOMEM;
}

void tbt_procfs_exit(void)
{
    if (!tbt_proc_dir)
        return;
    /* One recursive delete covers every entry that tbt_procfs_init() did
     * create, and stays silent about the ones a partial failure never made.
     * remove_proc_entry() WARNs on a missing name and taints the kernel, so
     * the fail: path could not use it safely. A missing subtree returns
     * -ENOENT without a WARN. */
    remove_proc_subtree(TBT_PROC_SUBDIR, init_net.proc_net);
    tbt_proc_dir = NULL;
}
