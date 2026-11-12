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
 *     into tests/unit/test_parse.c as well, so the tested code is the shipped code.
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
#include "tbt_cc.h"

#define TBT_CFG_NAME TBT_PROC_CONFIG
#define TBT_TUNNELS_NAME TBT_PROC_TUNNELS
#define TBT_PORTS_NAME TBT_PROC_PORTS
#define TBT_PEERS_NAME TBT_PROC_PEERS
#define TBT_STATS_NAME TBT_PROC_STATS
#define TBT_HTTP_NAME TBT_PROC_HTTP

/* c13 state/reason names. The numbers are the enums in tbtun.h; the stats
 * line spells them out because a paused controller is diagnosed by why it
 * paused, not by a bare index. */
static const char *tbt_adapt_state_name(u8 state)
{
    static const char *const names[] = {
        "steady", "cfg_sent", "drain", "swap", "abort", "cooldown",
    };

    return state < ARRAY_SIZE(names) ? names[state] : "?";
}

static const char *tbt_adapt_reason_name(u8 reason)
{
    static const char *const names[] = {
        "none", "off", "idle", "carrier", "abandoned", "sample",
        "switching", "hold", "error",
    };

    return reason < ARRAY_SIZE(names) ? names[reason] : "?";
}

/* The continuous controller's own reason enum. */
static const char *tbt_adapt_sclc_reason_name(u8 reason)
{
    static const char *const names[] = {
        "none", "cold", "steady", "burst", "backup", "paused", "saturated",
    };

    return reason < ARRAY_SIZE(names) ? names[reason] : "?";
}

// ---------------------------------------------------------------------------
// config: write

// Apply one complete line. Returns 0, or a negative errno that the write()
// propagates so `echo` fails visibly (DESIGN.md §8.4.1).
struct tbt_cfg_file {
    struct tbt_linebuf lb;
    struct net *net; /* captured at open; valid for every line at release */

    /* True when the last write() did not leave lb's unterminated tail as the
     * caller's complete final line: a line in the chunk failed (the writer
     * was told -EINVAL/-E2BIG), or the write supplied more than one
     * TBT_LINE_MAX chunk and the remainder was never consumed. release()
     * must not apply such a tail as the documented final unterminated line.
     * A later write that consumes everything it was given and reports no
     * error clears the flag. */
    bool discard_tail;

    /* Serializes write() on one file description. Nothing below the handler
     * does it: proc_reg_write() only brackets the call with use_pde(), and
     * fdget_pos() skips f_pos_lock when file_count(file) == 1, which is the
     * normal case for two threads sharing the process fd table. Without this
     * lock two writers interleave fragments of different lines in lb, so a
     * line neither of them wrote can be parsed and applied. The handler may
     * sleep (copy_from_user, kmalloc, tbt_tunnel_add), so a mutex is the
     * right primitive. It nests above cf->lock and tbt_lock; neither of
     * those is ever held while taking it. */
    struct mutex write_lock;

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
    /* The snapshot is the canonical config text, so it contains
     * crypto_key=<hex> for every tunnel that has one. Do not leave that copy
     * in freed memory. */
    if (cf->out)
    {
        tbt_memzero_explicit(cf->out, cf->out_len);
        kvfree(cf->out);
    }
    cf->out = NULL;
    cf->out_len = 0;
    cf->snapshot_valid = false;
    mutex_unlock(&cf->lock);
}

static int tbt_config_apply(char *line, struct net *net)
{
    struct tbt_parse_error perr = { NULL, NULL };
    struct tbt_cmd cmd = { 0 };
    int err;

    err = tbt_parse_line(line, &cmd, &perr);
    if (err)
    {
        pr_debug("tbtun: config rejected (%s%s%s): %d\n",
                 perr.key ? perr.key : "", perr.key && perr.why ? ": " : "",
                 perr.why ? perr.why : "invalid line", err);
        tbt_memzero_explicit(cmd.cfg.crypto_key, sizeof(cmd.cfg.crypto_key));
        return err;
    }

    switch (cmd.kind)
    {
    case TBT_CMD_NONE:
        err = 0;
        break;
    case TBT_CMD_TUNNEL:
        /* tbt_tunnel_add() owns tbt_lock for the list/cap section and drops
         * it before the listener/session teardown. A stuck session destroy
         * must not block every /proc reader behind the global lock. */
        err = tbt_tunnel_add(&cmd.cfg, net);
        break;
    case TBT_CMD_DEL:
        err = tbt_tunnel_del(cmd.id);
        break;
    case TBT_CMD_FLUSH:
        tbt_tunnel_shutdown();
        err = 0;
        break;
    case TBT_CMD_CARRIER:
    case TBT_CMD_SESSION:
        err = tbt_action_apply(&cmd);
        break;
    default:
        err = -EINVAL;
        break;
    }
    /* The parsed line may carry crypto_key=; the stack copy must not outlive
     * the call once the tunnel has copied what it needs. */
    tbt_memzero_explicit(cmd.cfg.crypto_key, sizeof(cmd.cfg.crypto_key));
    return err;
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
    mutex_init(&cf->write_lock);
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
    char *tmp;
    size_t n, i, supplied = len;
    int err = 0;

    if (!len)
        return 0;
    mutex_lock(&cf->write_lock);
    /* Any write can change the rendered text; drop the snapshot so the next
     * read renders the post-write configuration. */
    tbt_cfg_drop_snapshot(cf);
    if (len > TBT_LINE_MAX)
        len = TBT_LINE_MAX; // process at most one line's worth per chunk
    n = len;
    /* The line state lives in cf->lb; the chunk read only needs a staging
     * copy, and this is process context, so a full TBT_LINE_MAX buffer does
     * not belong on the kernel stack. */
    tmp = kmalloc(n, GFP_KERNEL);
    if (!tmp)
    {
        /* The write failed. Do not let release() apply a tail buffered by an
         * earlier write as though this handler had accepted the stream. */
        cf->discard_tail = true;
        lb->len = 0;
        lb->overflow = false;
        mutex_unlock(&cf->write_lock);
        return -ENOMEM;
    }
    if (copy_from_user(tmp, ubuf, n))
    {
        cf->discard_tail = true;
        lb->len = 0;
        lb->overflow = false;
        tbt_memzero_explicit(tmp, n);
        kfree(tmp);
        mutex_unlock(&cf->write_lock);
        return -EFAULT;
    }
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
    /* A failed line and a chunk-cut write both mean the unterminated bytes
     * left in lb are not a final line the caller stood behind. On error drop
     * them now as well, so a later write cannot append to a rejected line;
     * on a short success keep them, because the caller must resend the
     * remainder and the prefix belongs to that line. */
    cf->discard_tail = err != 0 || supplied > TBT_LINE_MAX;
    if (err)
    {
        lb->len = 0;
        lb->overflow = false;
    }
    /* tmp holds a copy of user input, which may include crypto_key=<hex>. */
    tbt_memzero_explicit(tmp, n);
    kfree(tmp);

    /* A read that raced this write may have snapshotted an intermediate
     * config; drop it again so the next read sees the final state. */
    tbt_cfg_drop_snapshot(cf);
    mutex_unlock(&cf->write_lock);
    return err ? err : len;
}

static int tbt_cfg_release(struct inode *inode, struct file *file)
{
    struct tbt_cfg_file *cf = file->private_data;

    if (cf)
    {
        /* The last write was rejected or cut at TBT_LINE_MAX: the
         * unterminated tail is not a line the caller completed, so the
         * close-flush must not install it. A fully consumed, error-free
         * final line (including the documented write-without-newline) still
         * flushes. */
        if (cf->discard_tail)
            cf->lb.len = 0;
        tbt_config_drain(&cf->lb, true, cf->net); // flush a final line
        tbt_cfg_drop_snapshot(cf); /* zeroes the rendered crypto_key text */
        /* The line buffer holds a copy of user input, which may include
         * crypto_key=<hex>. */
        tbt_memzero_explicit(&cf->lb, sizeof(cf->lb));
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
    char *line;
    char *out;
    size_t total = 0, off = 0, alloc;
    int n, err = 0;

    line = kvmalloc(TBT_LINE_MAX, GFP_KERNEL);
    if (!line)
        return -ENOMEM;
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        n = tbt_render_cfg(&t->cfg, line, TBT_LINE_MAX);
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
            /* Earlier lines may already contain crypto_key=<hex>. */
            tbt_memzero_explicit(out, alloc);
            kvfree(out);
            err = n;
            goto unlock;
        }
        off += (size_t)n;
    }
    if (off != total)
    {
        tbt_memzero_explicit(out, alloc);
        kvfree(out);
        err = -E2BIG;
        goto unlock;
    }
    cf->out = out;
    cf->out_len = total;
    cf->snapshot_valid = true;

unlock:
    mutex_unlock(&tbt_lock);
    /* The measuring pass leaves the last rendered line, crypto key included. */
    tbt_memzero_explicit(line, TBT_LINE_MAX);
    kvfree(line);
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
    if (!tbt_read_bool(cf->snapshot_valid))
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
        if (!tbt_read_bool(cf->snapshot_valid))
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
    char *buf;
    struct tbt_parse_error perr = { NULL, NULL };
    struct tbt_cmd cmd = { 0 };
    int err;

    if (!len)
        return 0;
    if (len >= TBT_LINE_MAX)
        return -E2BIG;
    buf = kvmalloc(len + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;
    if (copy_from_user(buf, ubuf, len))
    {
        kvfree(buf);
        return -EFAULT;
    }
    buf[len] = '\0';
    err = tbt_parse_line(buf, &cmd, &perr);
    if (err)
    {
        kvfree(buf);
        tbt_memzero_explicit(cmd.cfg.crypto_key, sizeof(cmd.cfg.crypto_key));
        return err;
    }
    if (cmd.kind != TBT_CMD_NONE)
        err = tbt_action_apply(&cmd);
    kvfree(buf);
    tbt_memzero_explicit(cmd.cfg.crypto_key, sizeof(cmd.cfg.crypto_key));
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
    struct tbt_session *s;
    u64 id;
    struct tbt_stats_ref {
        struct tbt_tunnel *t;
        struct tbt_session *s;
        u64 id;
    } *refs;
    unsigned int nrefs = 0, i, max_refs;
    struct tbt_stream_stat {
        u64 sid, fin_seq, dage_ms;
        unsigned long attached;
        u32 rxq, rxp, txp, peer_paused;
        int skerr, tgt_err;
        u8 state, kill, tgt_state, fin_q, fin_w, rst_w, rxf, rxfd;
        /* The flow's committed group set, copied from the session flow
         * table under sess->lock after the RCU pass. The stream's own
         * fec_flow pointer is not usable here: detach clears it before the
         * flow's last reference is dropped. */
        u16 fec_groups;
        u8 fec_mask[TBT_FEC_BITMAP_BYTES];
    } *ss;
    u64 streams = 0, gaps = 0;
    char prefix[64];
    int n;

    ss = kcalloc(TBT_STREAM_SNAP_MAX, sizeof(*ss), GFP_KERNEL);
    if (!ss)
        return -ENOMEM;
    max_refs = min_t(unsigned int, READ_ONCE(max_tunnels), 4096);
    refs = kcalloc(max_refs ? max_refs : 1, sizeof(*refs), GFP_KERNEL);
    if (!refs)
    {
        kfree(ss);
        return -ENOMEM;
    }
    mutex_lock(&tbt_lock);
    list_for_each_entry_rcu(t, &tbt_tunnels, node)
    {
        streams += t->streams;
        if (!t->sess)
            continue;
        gaps += tbt_stat_read(&t->sess->stats_gap);
        if (nrefs >= max_refs || !refcount_inc_not_zero(&t->refs))
            continue;
        if (!tbt_session_get(t->sess))
        {
            tbt_tunnel_put(t);
            continue;
        }
        refs[nrefs].t = t;
        refs[nrefs].s = t->sess;
        refs[nrefs].id = t->cfg.id;
        nrefs++;
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
    for (i = 0; i < nrefs; i++)
    {
        t = refs[i].t;
        s = refs[i].s;
        id = refs[i].id;
        if (s)
        {
            struct tbt_stream *st = NULL;
            u32 qdepth = 0, qbytes = 0, qmax = 0;
            u32 qctl = 0, qctl_bytes = 0;
            u32 rq = 0, rb = 0;
            int i;

            /* The session reference keeps this struct alive, but destroy
             * frees car[] and fec_fifo before it waits for that reference.
             * Render under the gate destroy takes before the first child
             * goes; a reader that starts after the teardown sees NULL. */
            down_read(&s->stats_lock);

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
                s->snap_rx_paused = tbt_read_bool(st->paused);
                s->snap_tx_paused = tbt_read_bool(st->tx_paused);
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
                       id, tbt_stat_read(&s->pub_capture_runs),
                       tbt_stat_read(&s->pub_actor_calls),
                       tbt_stat_read(&s->tx_submit_bytes),
                       READ_ONCE(s->stats_tx),
                       READ_ONCE(s->carrier_bytes),
                       READ_ONCE(s->carrier_err),
                       READ_ONCE(s->carrier_short));
            seq_printf(m, "tunnel %llu rx_work_runs=%llu rx_drain_entries=%llu "
                          "rx_drain_bytes=%llu rx_paused_skips=%llu rx_adopt_events=%llu "
                          "rx_records=%llu carrier_write_space_runs=%llu poll_rescues=%llu\n",
                       id, tbt_stat_read(&s->rx_work_runs),
                       tbt_stat_read(&s->rx_drain_entries),
                       tbt_stat_read(&s->rx_drain_bytes),
                       tbt_stat_read(&s->rx_paused_skips),
                       READ_ONCE(s->rx_adopt_events),
                       tbt_stat_read(&s->rx_records),
                       tbt_stat_read(&s->carrier_write_space_runs),
                       tbt_stat_read(&s->poll_rescues));
            seq_printf(m, "tunnel %llu rx_ready_calls=%llu rx_reads=%llu rx_read_bytes=%llu "
                          "rx_read_zero=%llu rx_resume=%llu rx_resume_busy=%llu rx_last_drained=%u "
                          "rx_proto=%llu rx_errs=%llu rx_last_err=%d tx_submit_calls=%llu "
                          "rx_delivered=%llu rx_budget_yields=%llu\n",
                       id, tbt_stat_read(&s->rx_ready_calls),
                       tbt_stat_read(&s->rx_reads),
                       tbt_stat_read(&s->rx_read_bytes),
                       tbt_stat_read(&s->rx_read_zero),
                       tbt_stat_read(&s->rx_resume_calls),
                       tbt_stat_read(&s->rx_resume_busy),
                       READ_ONCE(s->rx_last_drained),
                       tbt_stat_read(&s->rx_parse_proto),
                       tbt_stat_read(&s->rx_errs),
                       READ_ONCE(s->rx_last_err),
                       tbt_stat_read(&s->tx_submit_calls),
                       tbt_stat_read(&s->rx_delivered_bytes),
                       tbt_stat_read(&s->rx_budget_yields));
            seq_printf(m, "tunnel %llu dbg rcv_nxt=%u copied=%u after=%u qlen=%u "
                          "allocf=%llu over=%llu copyf=%llu hiwat=%llu pdead=%llu "
                          "prun=%llu pmoved=%llu p_nocar=%llu p_down=%llu p_stop=%llu "
                          "p_empty=%llu p_qlen=%llu\n",
                       id, s->pub_last_rcv_nxt, s->pub_last_copied,
                       s->pub_last_after, s->pub_last_qlen,
                       tbt_stat_read(&s->dbg_alloc_fail),
                       tbt_stat_read(&s->dbg_over_qmax),
                       tbt_stat_read(&s->dbg_copy_fail),
                       READ_ONCE(s->dbg_pump_hiwat),
                       READ_ONCE(s->dbg_pump_dead),
                       tbt_stat_read(&s->dbg_pump_runs),
                       READ_ONCE(s->dbg_pump_moved),
                       tbt_stat_read(&s->dbg_pump_nocar),
                       READ_ONCE(s->dbg_pump_down),
                       READ_ONCE(s->dbg_pump_stop),
                       READ_ONCE(s->dbg_pump_empty),
                       READ_ONCE(s->dbg_pump_qlen));
            seq_printf(m, "tunnel %llu live rxq=%u rxp=%u txq=%u txp=%u "
                          "pub_state=%u pub_wmem=%u pub_una=%u pub_nxt=%u "
                          "car_wmem=%u car_nxt=%u car_copied=%u car_rcvbuf=%u "
                          "car_sndbuf=%u "
                          "car_state=%u car_stop=%u car_ud=%u car_ready=%u "
                          "car_ws=%u txw_pend=%u txw_busy=%u\n",
                       id, s->snap_rx_queued, s->snap_rx_paused,
                       s->snap_tx_queued, s->snap_tx_paused,
                       s->snap_pub_state, s->snap_pub_wmem,
                       s->snap_pub_snd_una, s->snap_pub_snd_nxt,
                       s->snap_car_wmem, s->snap_car_rcv_nxt,
                       s->snap_car_copied, s->snap_car_rcvbuf,
                       s->snap_car_sndbuf,
                       s->snap_car_state, s->snap_car_stopping,
                       s->snap_car_ud, s->snap_car_ready_ok, s->snap_car_ws_ok,
                       s->snap_txw_pending, s->snap_txw_busy);
            seq_printf(m, "tunnel %llu txwake=%llu txwake_busy=%llu txwork=%llu\n",
                       id, tbt_stat_read(&s->tx_wake_calls),
                       tbt_stat_read(&s->tx_wake_busy),
                       tbt_stat_read(&s->tx_work_runs));
            seq_printf(m, "tunnel %llu ws_calls=%llu nospace_armed=%llu\n", id,
                       tbt_stat_read(&s->carrier_ws_calls),
                       tbt_stat_read(&s->carrier_nospace_armed));
            /* C3: the multi-carrier session's own counters.
             *
             * `up` counts the slots whose carrier has written its HELLO and
             * whose socket has not been closed. A paused (FEC-stalled)
             * carrier stays counted: the pause is a scheduling state, not a
             * death. A stopping carrier also stays counted until the release
             * closes its socket, because the slot is still owned.
             *
             * `replacing` counts slots that hold a carrier which is not up
             * yet (a replacement dialling, or a joined carrier whose OPEN
             * has not been written). `empty` counts slots with no carrier:
             * a near dial that could not allocate, or a far slot whose
             * carrier was lost and whose replacement has not joined. The
             * invariant is up + replacing + empty == n at every instant.
             * A repair therefore moves a slot from `up` to `replacing` (or
             * `empty` on the far) and back; it does not leak it.
             *
             * `lost`/`replaced` are the replacement record;
             * `rx_reorder_max_depth` is the deepest out-of-order backlog
             * ever held, in frames. */
            {
                unsigned int up = 0, replacing = 0, empty = 0, n = s->n_car;
                u32 max_depth = 0, ooo_bytes = 0, retain_bytes = 0;
                u32 ooo_frames = 0, max_stall_us = 0;
                u64 rt_copies, rt_data, rt_unique, app_bytes, dup_copies;
                u64 retx_data_ppm, dup_bytes_ppm;
                unsigned int dead_release = 0;
                struct tbt_carrier *dc;
                int k;

                for (k = 0; k < s->n_car; k++)
                {
                    /* w_replace() and join() swap sess->car[] under
                     * sess->lock and drop the slot's reference; a naked
                     * READ_ONCE followed by a dereference can outlive the
                     * last tbt_carrier_put(). Pin the object, as the cc and
                     * c12 blocks below already do. */
                    struct tbt_carrier *c = tbt_carrier_get(s, (u8)k);

                    if (!c)
                        empty++;
                    else if (atomic_read(&c->up))
                        up++;
                    else
                        replacing++;
                    tbt_carrier_put(c);
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
                spin_lock(&s->lock);
                list_for_each_entry(dc, &s->dead_release, release_node)
                    dead_release++;
                spin_unlock(&s->lock);
                /* Honest retransmit rates. The numerator is copies the
                 * carrier accepted; the denominator is unique DATA records
                 * (all DATA writes minus those copies), so FBK, PAD and HELLO
                 * can no longer dilute the ratio. dup_bytes_ppm is the copy
                 * payload over application bytes, the wire cost that the
                 * receiver pays. */
                rt_copies = READ_ONCE(s->retx_frames);
                rt_data = READ_ONCE(s->data_tx);
                /* Duplication-tunnel copies are unique-by-design work, not
                 * retransmits: subtract them too or the ratio understates. */
                dup_copies = READ_ONCE(s->dup_tx);
                rt_unique = rt_data > rt_copies + dup_copies ?
                    rt_data - rt_copies - dup_copies : 0;
                app_bytes = tbt_stat_read(&s->tx_submit_bytes);
                retx_data_ppm = rt_unique ?
                    min_t(u64, rt_copies * 1000000 / rt_unique, 1000000) : 0;
                dup_bytes_ppm = app_bytes ?
                    min_t(u64, READ_ONCE(s->retx_bytes) * 1000000 / app_bytes,
                          1000000) : 0;
                seq_printf(m, "tunnel %llu c3 n=%u up=%u replacing=%u empty=%u "
                              "lost=%llu replaced=%llu "
                              "stream_reset_tcp_failed=%llu stream_reset_user=%llu "
                              "carrier_add=%llu carrier_add_fail=%llu "
                              "backpressure_enter=%llu "
                              "spare_activated=%llu alloc_fail=%llu "
                              "port_exhaust=%llu port_alloc_fail=%llu connect_fail=%llu "
                              "connect_retry=%llu "
                              "join_dup=%llu join_bad=%llu join_nonce_reject=%llu "
                              "join_src_reject=%llu "
                              "hello_tag_reject=%llu crypto_reincarnations=%llu "
                              "tombstones_reincarnated=%llu "
                              "fbk_tx=%llu fbk_rx=%llu "
                              "retx_frames=%llu retx_bytes=%llu retain_drop=%llu "
                              "retx_hole=%llu retx_slow=%llu retx_pathloss=%llu "
                              "retx_other=%llu data_tx=%llu retx_data_ppm=%llu "
                              "dup_bytes_ppm=%llu "
                              "reorder_overflow=%llu reorder_max_depth=%u ooo_bytes=%u "
                              "ooo_frames=%u retain_bytes=%u tx_ack=%llu rx_ack=%llu "
                              "peer_paused=%u fb_delay_us=%u rto_us=%u una_stall_us=%u "
                              "stall_paused_skips=%llu stall_idle_skips=%llu "
                              "stall_declared=%llu stall_recovered=%llu "
                              "stall_drop=%llu stall_dead=%llu stall_replaced=%llu "
                              "stall_survival_bucket=%u "
                              "stall_wait_loss_bytes=%llu replace_cost_bytes=%llu "
                              "replace_cost_ewma=%llu "
                              "stall_replace_econ=%llu stall_replace_retrans=%llu "
                              "stall_replace_dead=%llu up_rescued=%llu "
                              "tx_window=%u bdp_bytes=%u k_x100=%u "
                              "rtt_mean_us=%u rttvar_us=%u "
                              "rtt_max_us=%u bdp_sum_rate=%llu "
                              "bdp_agg_wnd=%llu bdp_n_live=%u bdp_coverage=%u "
                              "txq_max=%u win_block=%llu win_resume=%llu "
                              "tx_wait_carrier=%llu tx_backpressure=%llu "
                              "tx_wait_carrier_bytes=%llu tx_wait_carrier_ns=%llu "
                              "tx_backpressure_bytes=%llu tx_backpressure_ns=%llu "
                              "pump_hw_armed=%llu pump_hw_fired=%llu "
                              "retx_evident=%llu retx_cand=%llu retx_nosends=%llu "
                              "retx_txqdrop=%llu retx_nopick=%llu retx_stalled=%llu "
                              "retx_notup=%llu retx_notann=%llu retx_batch=%llu "
                              "retx_eagain=%llu retx_memshort=%llu retx_wndshort=%llu "
                              "prov_now=%d prov_peak=%u prov_reject=%llu "
                              "dead_release=%u carrier_retired=%llu "
                              "cc_bbr_ok=%llu cc_bbr_fail=%llu "
                              "slow_evicted=%llu\n",
                           id, n, up, replacing, empty,
                           tbt_stat_read(&s->carrier_lost),
                           tbt_stat_read(&s->carrier_replaced),
                           tbt_stat_read(&s->stream_reset_tcp_failed),
                           tbt_stat_read(&s->stream_reset_user),
                           tbt_stat_read(&s->carrier_add),
                           tbt_stat_read(&s->carrier_add_fail),
                           tbt_stat_read(&s->backpressure_enter),
                           READ_ONCE(s->spare_activated),
                           tbt_stat_read(&s->carrier_alloc_fail),
                           tbt_stat_read(&s->port_exhaust),
                           tbt_stat_read(&s->port_alloc_fail),
                           tbt_stat_read(&s->connect_fail),
                           tbt_stat_read(&s->connect_retry),
                           READ_ONCE(s->session_join_dup),
                           tbt_stat_read(&s->session_join_bad),
                           tbt_stat_read(&s->join_nonce_reject),
                           tbt_stat_read(&s->join_src_reject),
                           tbt_stat_read(&s->hello_tag_reject),
                           tbt_stat_read(&s->crypto_reincarnations),
                           tbt_stat_read(&s->tombstones_reincarnated),
                           READ_ONCE(s->fbk_tx),
                           tbt_stat_read(&s->fbk_rx), READ_ONCE(s->retx_frames),
                           READ_ONCE(s->retx_bytes), READ_ONCE(s->retain_drop),
                           READ_ONCE(s->retx_hole), READ_ONCE(s->retx_slow),
                           READ_ONCE(s->retx_pathloss),
                           READ_ONCE(s->retx_other), READ_ONCE(s->data_tx),
                           retx_data_ppm,
                           dup_bytes_ppm,
                           tbt_stat_read(&s->rx_reorder_overflow), max_depth,
                           ooo_bytes, ooo_frames,
                           retain_bytes, READ_ONCE(s->tx_ack), READ_ONCE(s->rx_ack),
                           tbt_read_bool(s->peer_paused), READ_ONCE(s->fb_delay_us),
                           READ_ONCE(s->rto_us), READ_ONCE(s->una_stall_us),
                           tbt_stat_read(&s->stall_paused_skips),
                           tbt_stat_read(&s->stall_idle_skips),
                           tbt_stat_read(&s->stall_declared),
                           tbt_stat_read(&s->stall_recovered),
                           READ_ONCE(s->stall_drop),
                           tbt_stat_read(&s->stall_dead),
                           tbt_stat_read(&s->stall_replaced),
                           READ_ONCE(s->stall_survival_bucket),
                           READ_ONCE(s->stall_wait_loss_bytes),
                           READ_ONCE(s->replace_cost_bytes),
                           READ_ONCE(s->replace_cost_ewma),
                           tbt_stat_read(&s->stall_replace_econ),
                           tbt_stat_read(&s->stall_replace_retrans),
                           tbt_stat_read(&s->stall_replace_dead),
                           tbt_stat_read(&s->up_rescued),
                           READ_ONCE(s->tx_window), READ_ONCE(s->bdp_bytes),
                           READ_ONCE(s->k_x100), READ_ONCE(s->rtt_mean_us),
                           READ_ONCE(s->rttvar_us), READ_ONCE(s->rtt_max_us),
                           READ_ONCE(s->bdp_sum_rate),
                           READ_ONCE(s->bdp_agg_wnd),
                           READ_ONCE(s->bdp_n_live),
                           READ_ONCE(s->bdp_coverage),
                           READ_ONCE(s->txq_max),
                           READ_ONCE(s->win_block), READ_ONCE(s->win_resume),
                           READ_ONCE(s->tx_wait_carrier),
                           tbt_stat_read(&s->tx_backpressure),
                           READ_ONCE(s->tx_wait_carrier_bytes),
                           READ_ONCE(s->tx_wait_carrier_ns),
                           tbt_stat_read(&s->tx_backpressure_bytes),
                           tbt_stat_read(&s->tx_backpressure_ns),
                           READ_ONCE(s->pump_hw_armed),
                           tbt_stat_read(&s->pump_hw_fired),
                           tbt_stat_read(&s->dbg_retx_evident),
                           tbt_stat_read(&s->dbg_retx_cand),
                           tbt_stat_read(&s->dbg_retx_nosends),
                           READ_ONCE(s->dbg_retx_txqdrop),
                           READ_ONCE(s->dbg_retx_nopick),
                           READ_ONCE(s->dbg_retx_stalled),
                           READ_ONCE(s->dbg_retx_notup),
                           READ_ONCE(s->dbg_retx_notann),
                           READ_ONCE(s->dbg_retx_batch),
                           READ_ONCE(s->dbg_retx_eagain),
                           READ_ONCE(s->dbg_retx_memshort),
                           READ_ONCE(s->dbg_retx_wndshort),
                           atomic_read(&s->n_prov), READ_ONCE(s->prov_peak),
                           READ_ONCE(s->prov_reject), dead_release,
                           tbt_stat_read(&s->carrier_retired),
                           tbt_stat_read(&s->car_bbr_ok),
                           tbt_stat_read(&s->car_bbr_fail),
                           tbt_stat_read(&s->slow_evicted));
                /* Carrier CC: per-tunnel sums over the live carrier sockets.
                 * max_bw and pacing are sums because the tunnel's capacity is
                 * the sum of its carriers; the hold edges count every carrier
                 * whether or not it selects the module's CC. */
                {
                    u64 cc_bw = 0, cc_pace = 0, cc_clamp = 0;
                    u64 cc_enter = 0, cc_exit = 0;
                    u32 cc_socks = 0, ci;

                    for (ci = 0; ci < s->n_car; ci++)
                    {
                        struct tbt_carrier *car = tbt_carrier_get(s, (u8)ci);
                        struct sock *csk;
                        struct tbt_cc_info info;

                        if (!car)
                            continue;
                        cc_enter += tbt_stat_read(&car->hold_enter);
                        cc_exit += tbt_stat_read(&car->hold_exit);
                        csk = tbt_carrier_sk_get(car);
                        if (csk && tbt_cc_snapshot(csk, &info))
                        {
                            cc_bw += info.max_bw_bps;
                            cc_pace += info.pacing_bps;
                            cc_clamp += info.clamp_hits;
                            cc_socks++;
                        }
                        tbt_carrier_sk_put(csk);
                        tbt_carrier_put(car);
                    }
                    seq_printf(m, "tunnel %llu cc cc_carriers=%u "
                                  "cc_max_bw_bps=%llu cc_pacing_bps=%llu "
                                  "cc_clamp_hits=%llu cc_hold_enter=%llu "
                                  "cc_hold_exit=%llu p_ppm=%u\n",
                               id, cc_socks, cc_bw, cc_pace, cc_clamp,
                               cc_enter, cc_exit, READ_ONCE(s->cc_loss_ppm));
                }
                /* Review 22: pinned control-frame lifetime. ctl_queued is the
                 * live control-queue depth; the other four are the retry,
                 * re-pin, obsolete-generation and explicit-loss record. */
                seq_printf(m, "tunnel %llu ctl ctl_lost=%llu ctl_retry=%llu "
                              "ctl_requeue=%llu ctl_obsolete=%llu ctl_queued=%u "
                              "pinned_bad_path=%llu "
                              "close_timeouts=%llu fin_starved=%llu "
                              "fin_written=%llu close_hold_fec=%llu "
                              "close_detaches=%llu\n",
                           id, tbt_stat_read(&s->ctl_lost),
                           READ_ONCE(s->ctl_retry), READ_ONCE(s->ctl_requeue),
                           READ_ONCE(s->ctl_obsolete),
                           READ_ONCE(s->ctl_queued),
                           READ_ONCE(s->dbg_pinned_bad_path),
                           tbt_stat_read(&s->close_timeouts),
                           tbt_stat_read(&s->fin_starved),
                           tbt_stat_read(&s->fin_written),
                           tbt_stat_read(&s->close_hold_fec),
                           tbt_stat_read(&s->close_detaches));
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
                           id, READ_ONCE(s->n_active),
                           READ_ONCE(s->n_streams), READ_ONCE(s->n_pending));
                seq_printf(m, "tunnel %llu groups total=%u used=%u\n",
                           id, READ_ONCE(s->fec_groups_total),
                           READ_ONCE(s->fec_groups_used));
                /* Per-stream terminal state. Copy the fields under RCU
                 * into a heap snapshot; seq_printf() may sleep (it grows the
                 * seq_file buffer with GFP_KERNEL), and sleeping inside an
                 * RCU read section can stall synchronize_rcu() and hang config
                 * teardown/rmmod. */
                {
                    struct tbt_stream *st;
                    int bi, si = 0;

                    rcu_read_lock();
                    for (bi = 0; bi < TBT_STREAM_HT && si < TBT_STREAM_SNAP_MAX; bi++)
                        hlist_for_each_entry_rcu(st, &s->streams[bi], node)
                        {
                            struct sock *ps = READ_ONCE(st->sk);
                            struct socket *tg = READ_ONCE(st->target);
                            struct sock *ts = tg ? READ_ONCE(tg->sk) : NULL;
                            u64 ds = READ_ONCE(st->deliver_since_ns);
                            u64 now = ktime_get_ns();

                            if (si >= TBT_STREAM_SNAP_MAX)
                                break;
                            ss[si].sid = st->sid;
                            ss[si].state = READ_ONCE(st->state);
                            ss[si].kill = READ_ONCE(st->kill_reason);
                            ss[si].attached = READ_ONCE(st->attached);
                            ss[si].rxq = READ_ONCE(st->rx_queued);
                            ss[si].rxp = tbt_read_bool(st->paused);
                            ss[si].txp = tbt_read_bool(st->tx_paused);
                            ss[si].peer_paused = tbt_read_bool(st->peer_paused);
                            ss[si].skerr = ps ? READ_ONCE(ps->sk_err) : 0;
                            ss[si].tgt_state = ts ? READ_ONCE(ts->sk_state) : 0;
                            ss[si].tgt_err = ts ? READ_ONCE(ts->sk_err) : 0;
                            ss[si].fin_seq = READ_ONCE(st->tx_fin_seq);
                            ss[si].fin_q = tbt_read_bool(st->tx_fin_queued);
                            ss[si].fin_w = tbt_read_bool(st->tx_fin_written);
                            ss[si].rst_w = tbt_read_bool(st->tx_rst_written);
                            ss[si].rxf = tbt_read_bool(st->rx_fin_seen);
                            ss[si].rxfd = tbt_read_bool(st->rx_fin_done);
                            ss[si].dage_ms = ds && now > ds
                                ? (now - ds) / NSEC_PER_MSEC : 0;
                            si++;
                        }
                    rcu_read_unlock();
                    /* f->groups and f->group_mask are written under
                     * sess->lock (tbt_fec_flow_mask_commit), so one pass
                     * under that lock gives every rendered row a consistent
                     * allocation snapshot; a closed sid has no entry and
                     * keeps zeros. */
                    if (si)
                    {
                        struct tbt_fec_flow *f;
                        int bj;

                        spin_lock(&s->lock);
                        for (bi = 0; bi < TBT_STREAM_HT; bi++)
                            hlist_for_each_entry(f, &s->fec_flows[bi], node)
                                for (bj = 0; bj < si; bj++)
                                    if (ss[bj].sid == f->sid)
                                    {
                                        ss[bj].fec_groups = f->groups;
                                        memcpy(ss[bj].fec_mask, f->group_mask,
                                               TBT_FEC_BITMAP_BYTES);
                                        break;
                                    }
                        spin_unlock(&s->lock);
                    }
                    for (bi = 0; bi < si; bi++)
                    {
                        static const char hex[] = "0123456789abcdef";
                        char fec_hex[2 * TBT_FEC_BITMAP_BYTES + 1];
                        int hj;

                        for (hj = 0; hj < TBT_FEC_BITMAP_BYTES; hj++)
                        {
                            fec_hex[2 * hj] = hex[ss[bi].fec_mask[hj] >> 4];
                            fec_hex[2 * hj + 1] =
                                hex[ss[bi].fec_mask[hj] & 0xf];
                        }
                        fec_hex[2 * TBT_FEC_BITMAP_BYTES] = 0;
                        seq_printf(m, "tunnel %llu stream sid=%llu state=%u "
                                      "kill=%u attached=%#lx rxq=%u rxp=%u "
                                      "txp=%u peer_paused=%u skerr=%d "
                                      "tgt_state=%u tgt_err=%d "
                                      "fin_seq=%llu fin_q=%u fin_w=%u rst_w=%u "
                                      "rxf=%u/%u dage_ms=%llu "
                                      "fec_groups=%u fec_mask=%s\n",
                                   id, ss[bi].sid, ss[bi].state,
                                   ss[bi].kill, ss[bi].attached,
                                   ss[bi].rxq, ss[bi].rxp, ss[bi].txp,
                                   ss[bi].peer_paused, ss[bi].skerr,
                                   ss[bi].tgt_state, ss[bi].tgt_err,
                                   ss[bi].fin_seq, ss[bi].fin_q,
                                   ss[bi].fin_w, ss[bi].rst_w,
                                   ss[bi].rxf, ss[bi].rxfd,
                                   ss[bi].dage_ms, ss[bi].fec_groups, fec_hex);
                    }
                }
                seq_printf(m, "tunnel %llu c6 holes_declared=%llu holes_repaired=%llu "
                              "holes_unrecoverable=%llu rx_max_stall_us=%u "
                              "hole_checks=%llu gate_fail=%llu depth_fail=%llu "
                              "time_fail=%llu deliver_timeouts=%llu "
                              "deliver_checks=%llu\n",
                           id, READ_ONCE(s->holes_declared),
                           READ_ONCE(s->holes_repaired),
                           tbt_stat_read(&s->holes_unrecoverable), max_stall_us,
                           tbt_stat_read(&s->hole_checks),
                           tbt_stat_read(&s->hole_gate_fail),
                           tbt_stat_read(&s->hole_depth_fail),
                           tbt_stat_read(&s->hole_time_fail),
                           tbt_stat_read(&s->deliver_timeouts),
                           tbt_stat_read(&s->dbg_deliver_checks));
                seq_printf(m, "tunnel %llu c9 fec_proto_fail=%llu "
                              "fec_hdr_crc_fail=%llu fec_payload_crc_fail=%llu "
                              "fec_short_tx=%llu fec_short_unknown=%llu "
                              "fec_stream_refused=%llu fec_refused_rst=%llu "
                              "bind_drop_no_stream=%llu "
                              "fec_drop_window=%llu fec_drop_sid=%llu fec_drop_crypto=%llu "
                              "fec_ack_tx=%llu fec_ack_rx=%llu "
                              "fec_credit_wait=%llu fec_credit_violation=%llu fec_credit_midblock_wait=%llu fec_tx_block_complete=%llu fec_tx_block_partial=%llu fec_tx_block_final=%llu fec_replay_evicted_before_nack=%llu fec_tx_pump_rearm=%llu fec_tx_run_leak=%llu fec_unacked_ring_drain=%llu fec_fin_frozen_with_pending=%llu fec_fin_with_undelivered=%llu fec_replay_false_enospc=%llu fec_block_complete_without_replay=%llu fec_credit_unit_bad=%llu\n",
                           id, tbt_stat_read(&s->fec_proto_fail),
                           tbt_stat_read(&s->fec_hdr_crc_fail),
                           tbt_stat_read(&s->fec_payload_crc_fail),
                           tbt_stat_read(&s->fec_short_tx),
                           tbt_stat_read(&s->fec_short_unknown),
                           tbt_stat_read(&s->fec_stream_refused),
                           tbt_stat_read(&s->fec_refused_rst),
                           tbt_stat_read(&s->bind_drop_no_stream),
                           tbt_stat_read(&s->fec_drop_window),
                           tbt_stat_read(&s->fec_drop_sid),
                           tbt_stat_read(&s->fec_drop_crypto),
                           tbt_stat_read(&s->fec_ack_tx),
                           tbt_stat_read(&s->fec_ack_rx),
                           tbt_stat_read(&s->fec_credit_wait),
                           tbt_stat_read(&s->fec_credit_violation),
                           tbt_stat_read(&s->fec_credit_midblock_wait),
                           tbt_stat_read(&s->fec_tx_block_complete),
                           tbt_stat_read(&s->fec_tx_block_partial),
                           tbt_stat_read(&s->fec_tx_block_final),
                           tbt_stat_read(&s->fec_replay_evicted_before_nack),
                           tbt_stat_read(&s->fec_tx_pump_rearm),
                           tbt_stat_read(&s->fec_tx_run_leak),
                           tbt_stat_read(&s->fec_unacked_ring_drain),
                           tbt_stat_read(&s->fec_fin_frozen_with_pending),
                           tbt_stat_read(&s->fec_fin_with_undelivered),
                           tbt_stat_read(&s->fec_replay_false_enospc),
                           tbt_stat_read(&s->fec_block_complete_without_replay),
                           tbt_stat_read(&s->fec_credit_unit_bad));
                /* Every way a share or a block can be lost. Without these a
                 * stalled FEC stream reports zero errors: the counters that
                 * name the loss were simply not exported. */
                seq_printf(m, "tunnel %llu c10 fec_decode_blocks=%llu "
                              "fec_decode_recovered=%llu fec_decode_nomem=%llu "
                              "fec_deliver_nomem=%llu fec_deliver_dup=%llu "
                              "fec_deliver_ooo=%llu "
                              "fec_deliver_bad=%llu "
                              "fec_dup_share=%llu fec_late_share=%llu "
                              "fec_late_data=%llu fec_late_unknown=%llu "
                              "fec_pad_share=%llu fec_dup_marker=%llu "
                              "fec_marker_conflict=%llu "
                              "fec_epoch_conflict=%llu "
                              "fec_slot_past_cut=%llu fec_fin_bad=%llu "
                              "fec_ack_bad=%llu fec_tx_share_gap=%llu "
                              "fec_empty_block=%llu\n",
                           id,
                           tbt_stat_read(&s->fec_decode_blocks),
                           tbt_stat_read(&s->fec_decode_recovered),
                           tbt_stat_read(&s->fec_decode_nomem),
                           tbt_stat_read(&s->fec_deliver_nomem),
                           tbt_stat_read(&s->fec_deliver_dup),
                           tbt_stat_read(&s->fec_deliver_ooo),
                           tbt_stat_read(&s->fec_deliver_bad),
                           tbt_stat_read(&s->fec_dup_share),
                           tbt_stat_read(&s->fec_late_share),
                           tbt_stat_read(&s->fec_late_data),
                           tbt_stat_read(&s->fec_late_unknown),
                           tbt_stat_read(&s->fec_pad_share),
                           tbt_stat_read(&s->fec_dup_marker),
                           tbt_stat_read(&s->fec_marker_conflict),
                           tbt_stat_read(&s->fec_epoch_conflict),
                           tbt_stat_read(&s->fec_slot_past_cut),
                           tbt_stat_read(&s->fec_fin_bad),
                           tbt_stat_read(&s->fec_ack_bad),
                           tbt_stat_read(&s->fec_tx_share_gap),
                           tbt_stat_read(&s->fec_empty_block));
                /* NACK/replay repair, reset causes, and the carrier wait
                 * that precedes backpressure. A user-visible stall can be
                 * checked against these: fec_reset_tcp_failed is the only
                 * FEC-owned reset, and it requires every carrier TCP to be
                 * gone. fec_nack_lat_ns now measures NACK receipt to the
                 * carrier write, not to enqueue. */
                if (s->fec_fifo)
                {
                    spin_lock(&s->fec_out_lock);
                    qdepth = (u32)atomic_read(&s->fec_outq_queued);
                    qbytes = (u32)atomic_read(&s->fec_outq_bytes);
                    qmax = READ_ONCE(s->fec_outq_depth_max);
                    qctl = s->fec_out_ctl_depth;
                    qctl_bytes = s->fec_out_ctl_bytes;
                    rq = s->fec_replay_queued;
                    rb = s->fec_replay_bytes;
                    spin_unlock(&s->fec_out_lock);
                }
                seq_printf(m, "tunnel %llu c11 fec_nack_tx=%llu "
                              "fec_nack_rx=%llu fec_nack_retx=%llu "
                              "fec_nack_drop=%llu fec_nack_enomem=%llu "
                              "fec_nack_einval=%llu fec_nack_enospc=%llu "
                              "fec_nack_eother=%llu fec_nack_bad=%llu "
                              "fec_replay_hit=%llu fec_replay_miss=%llu "
                              "fec_replay_miss_no_ring=%llu "
                              "fec_replay_miss_alloc=%llu "
                              "fec_replay_miss_enospc=%llu "
                              "fec_replay_miss_get=%llu "
                              "fec_replay_miss_evicted=%llu "
                              "fec_replay_miss_never_stored=%llu "
                              "fec_nack_sparse_probe=%llu "
                              "fec_nack_empty_reply=%llu "
                              "fec_nack_parity_reply=%llu "
                              "fec_replay_drop=%llu "
                              "fec_nack_retx_bytes=%llu "
                              "fec_replace_retx_bytes=%llu "
                              "fec_nack_lat_ns=%llu fec_nack_lat_max_ns=%llu "
                              "fec_nack_repair_ns=%llu "
                              "fec_nack_repair_max_ns=%llu "
                              "fec_reset_tcp_failed=%llu fec_reset_user=%llu "
                              "fec_wait_carrier=%llu fec_backpressure=%llu "
                              "fec_flush_skip=%llu fec_flush_pass=%llu "
                              "fec_hol_wait=%llu fec_outq_drop=%llu "
                              "fec_outq_high_wait=%llu fec_encode_fail=%llu "
                              "fec_init_rx=%llu fec_init_bad=%llu fec_decode_backoff=%llu "
                              "fec_outq_depth=%u fec_outq_max=%u "
                              "fec_outq_bytes=%u fec_outq_ctl=%u "
                              "fec_out_ctl_bytes=%u "
                              "fec_replay_queued=%u fec_replay_bytes=%u "
                              "fec_replay_skip=%llu "
                              "fec_nack_filtered_ready=%llu "
                              "fec_nack_filtered_inflight=%llu "
                              "fec_nack_filtered_dup=%llu "
                              "fec_nack_filtered_young=%llu "
                              "fec_replay_dedup=%llu "
                              "fec_nack_req_shares=%llu "
                              "fec_nack_backpressure=%llu "
                              "fec_replay_stale=%llu "
                              "fec_fin_wait_replay=%llu "
                              "fec_nack_never_stored=%llu "
                              "fec_nack_absent_reply=%llu "
                              "fec_nack_req_invalid=%llu "
                              "fec_replay_high_water=%llu "
                              "fec_replay_pass_throttled=%llu "
                              "fec_replay_drain_bps=%llu "
                              "fec_replay_queue_high=%llu "
                              "fec_reset_hole=%llu "
                              "tx_stall_suppressed_fec=%llu "
                              "fec_repair_last_ns=%llu "
                              "fec_short_parity_saved=%llu "
                              "fec_nack_pad_skip=%llu "
                              "fec_short_blocks=%llu\n",
                           id,
                           tbt_stat_read(&s->fec_nack_tx),
                           tbt_stat_read(&s->fec_nack_rx),
                           tbt_stat_read(&s->fec_nack_retx),
                           tbt_stat_read(&s->fec_nack_drop),
                           tbt_stat_read(&s->fec_nack_enomem),
                           tbt_stat_read(&s->fec_nack_einval),
                           tbt_stat_read(&s->fec_nack_enospc),
                           tbt_stat_read(&s->fec_nack_eother),
                           tbt_stat_read(&s->fec_nack_bad),
                           tbt_stat_read(&s->fec_replay_hit),
                           tbt_stat_read(&s->fec_replay_miss),
                           tbt_stat_read(&s->fec_replay_miss_no_ring),
                           tbt_stat_read(&s->fec_replay_miss_alloc),
                           tbt_stat_read(&s->fec_replay_miss_enospc),
                           tbt_stat_read(&s->fec_replay_miss_get),
                           tbt_stat_read(&s->fec_replay_miss_evicted),
                           tbt_stat_read(&s->fec_replay_miss_never_stored),
                           tbt_stat_read(&s->fec_nack_sparse_probe),
                           tbt_stat_read(&s->fec_nack_empty_reply),
                           tbt_stat_read(&s->fec_nack_parity_reply),
                           tbt_stat_read(&s->fec_replay_drop),
                           tbt_stat_read(&s->fec_nack_retx_bytes),
                           tbt_stat_read(&s->fec_replace_retx_bytes),
                           tbt_stat_read(&s->fec_nack_lat_ns),
                           tbt_stat_read(&s->fec_nack_lat_max_ns),
                           tbt_stat_read(&s->fec_nack_repair_ns),
                           tbt_stat_read(&s->fec_nack_repair_max_ns),
                           tbt_stat_read(&s->fec_reset_tcp_failed),
                           tbt_stat_read(&s->fec_reset_user),
                           tbt_stat_read(&s->fec_wait_carrier),
                           tbt_stat_read(&s->fec_backpressure),
                           tbt_stat_read(&s->fec_flush_skip),
                           tbt_stat_read(&s->fec_flush_pass),
                           tbt_stat_read(&s->fec_hol_wait),
                           tbt_stat_read(&s->fec_outq_drop),
                           tbt_stat_read(&s->fec_outq_high_wait),
                           tbt_stat_read(&s->fec_encode_fail),
                           tbt_stat_read(&s->fec_init_rx),
                           tbt_stat_read(&s->fec_init_bad),
                           tbt_stat_read(&s->fec_decode_backoff),
                           qdepth, qmax, qbytes, qctl, qctl_bytes, rq, rb,
                           tbt_stat_read(&s->fec_replay_skip),
                           tbt_stat_read(&s->fec_nack_filtered_ready),
                           tbt_stat_read(&s->fec_nack_filtered_inflight),
                           tbt_stat_read(&s->fec_nack_filtered_dup),
                           tbt_stat_read(&s->fec_nack_filtered_young),
                           tbt_stat_read(&s->fec_replay_dedup),
                           tbt_stat_read(&s->fec_nack_req_shares),
                           tbt_stat_read(&s->fec_nack_backpressure),
                           tbt_stat_read(&s->fec_replay_stale),
                           tbt_stat_read(&s->fec_fin_wait_replay),
                           tbt_stat_read(&s->fec_nack_never_stored),
                           tbt_stat_read(&s->fec_nack_absent_reply),
                           tbt_stat_read(&s->fec_nack_req_invalid),
                           tbt_stat_read(&s->fec_replay_high_water),
                           tbt_stat_read(&s->fec_replay_pass_throttled),
                           tbt_stat_read(&s->fec_replay_drain_bps),
                           tbt_stat_read(&s->fec_replay_queue_high),
                           tbt_stat_read(&s->fec_reset_hole),
                           tbt_stat_read(&s->tx_stall_suppressed_fec),
                           READ_ONCE(s->fec_repair_last_ns),
                           tbt_stat_read(&s->fec_short_parity_saved),
                           tbt_stat_read(&s->fec_nack_pad_skip),
                           tbt_stat_read(&s->fec_short_blocks));
                /* Session FIFO detail: records/bytes still waiting for a
                 * socket, the next carrier cursor, and how many carriers are
                 * marked slow. A healthy session keeps the FIFO empty or
                 * shallow; a congested one shows the carriers the drain now
                 * tries last. */
                if (s->fec_fifo)
                {
                    u32 slow = 0;
                    int ci;

                    for (ci = 0; ci < s->n_car; ci++)
                    {
                        struct tbt_carrier *c = tbt_carrier_get(s, (u8)ci);

                        if (!c)
                            continue;
                        if (READ_ONCE(c->slow))
                            slow++;
                        tbt_carrier_put(c);
                    }
                    {
                        struct tbt_fec_tx *tx;
                        u32 ring = 0, queued = 0;

                        /* The source ring is what `tx_paused` reflects and
                         * what the application's window sees. A freeze with
                         * a non-empty ring and a closed application window
                         * is a capture resume problem; an empty ring is an
                         * application or link problem. */
                        rcu_read_lock();
                        tx = rcu_dereference(s->fec_tx);
                        if (tx)
                        {
                            ring = READ_ONCE(tx->ring_size);
                            spin_lock(&tx->lock);
                            queued = tx->head - tx->tail;
                            spin_unlock(&tx->lock);
                        }
                        rcu_read_unlock();
                        seq_printf(m, "tunnel %llu c12 fec_outq fifo=%u/%u "
                                      "rot=%u slow=%u txring=%u/%u\n", id,
                                   atomic_read(&s->fec_outq_queued),
                                   atomic_read(&s->fec_outq_bytes),
                                   atomic_read(&s->async_rot), slow, queued,
                                   ring);
                    }
                }
                /* c13: the adaptive control surface. The shape is the one in
                 * force (updated by the data plane on every epoch), and the
                 * window counters are alive-filtered DATA shares since that
                 * epoch began. adaptive=off means the configured n/m is
                 * fixed and reason=off names the fallback. */
                {
                    const struct tbt_fec_adapt_ctl *a = &s->adapt;
                    u16 an = READ_ONCE(a->n), am = READ_ONCE(a->m);
                    u16 ag = READ_ONCE(a->groups);

                    seq_printf(m, "tunnel %llu c13 adaptive=%s epoch=%u "
                                  "n=%u m=%u groups=%u class=%u "
                                  "p_ppm=%u p_win_ppm=%u r_fail_ppm=%u "
                                  "sent=%llu miss=%llu "
                                  "switches=%u aborts=%u rollbacks=%u "
                                  "pauses=%llu pause_ns=%llu "
                                  "sample_unknown=%llu abandoned=%llu "
                                  "state=%s reason=%s "
                                  "m_target=%u q_raw=%u q=%u delta=%u "
                                  "delta_off=%d burst=%u eps=%u sigma=%u backup=%u "
                                  "ctl_reason=%s frozen=%u\n",
                               id, tbt_read_bool(s->adaptive) ? "on" : "off",
                               READ_ONCE(a->epoch), an, am, ag,
                               READ_ONCE(a->class), READ_ONCE(a->p_ppm),
                               READ_ONCE(a->p_win_ppm),
                               READ_ONCE(a->r_fail_ppm),
                               READ_ONCE(a->sent), READ_ONCE(a->miss),
                               READ_ONCE(a->switches), READ_ONCE(a->aborts),
                               READ_ONCE(a->rollbacks),
                               READ_ONCE(a->pauses), READ_ONCE(a->pause_ns),
                               READ_ONCE(a->sample_unknown),
                               READ_ONCE(a->abandoned),
                               tbt_adapt_state_name(READ_ONCE(a->state)),
                               tbt_adapt_reason_name(READ_ONCE(a->reason)),
                               READ_ONCE(a->m_target),
                               READ_ONCE(a->q_raw_q16),
                               READ_ONCE(a->q_q16),
                               READ_ONCE(a->delta_q16),
                               READ_ONCE(a->delta_off_q16),
                               READ_ONCE(a->burst_q16),
                               READ_ONCE(a->eps_q16),
                               READ_ONCE(a->sigma_q16),
                               READ_ONCE(a->backup),
                               tbt_adapt_sclc_reason_name(
                                   READ_ONCE(a->ctl_reason)),
                               READ_ONCE(a->frozen));
                }
                seq_printf(m, "tunnel %llu c8 profile_mismatch=%llu http_parse_err=%llu "
                              "http_status_err=%llu http_req=%llu http_resp=%llu "
                              "pad_tx=%llu drip_tx=%llu\n",
                           id, tbt_stat_read(&s->profile_mismatch),
                           tbt_stat_read(&s->http_parse_err),
                           tbt_stat_read(&s->http_status_err),
                           READ_ONCE(s->http_req), READ_ONCE(s->http_resp),
                           tbt_stat_read(&s->pad_tx),
                           tbt_stat_read(&s->drip_tx));
                {
                    u64 goodput = tbt_stat_read(&s->tx_submit_bytes);
                    u64 dupb = tbt_stat_read(&s->dup_bytes);
                    u64 inflate = goodput ? (goodput + dupb) * 1000000 / goodput : 0;

                    seq_printf(m, "tunnel %llu c5 dup_tx=%llu dup_used=%llu "
                                  "dup_suppressed=%llu dup_same_path=%llu "
                                  "inflate_ppm=%llu dup_bytes=%llu\n",
                               id, READ_ONCE(s->dup_tx),
                               tbt_stat_read(&s->dup_used),
                               tbt_stat_read(&s->dup_suppressed),
                               READ_ONCE(s->dup_same_path), inflate, dupb);
                }
                /* Reconciliation: the session aggregates count every carrier
                 * that ever wrote, while `peers` lists only the live slots.
                 * `live_*` is recomputed here and `retired_*` is moved by
                 * tbt_carrier_stats_retire(), so at rest
                 *   wire == live_bytes + retired_bytes
                 *   data_tx == live_data + retired_data
                 *   retx_frames == live_retx + retired_retx
                 * holds, and tests/qemu/init checks it against the peers
                 * rows. */
                {
                    u64 live_bytes = 0, live_data = 0, live_retx = 0;
                    int r;

                    for (r = 0; r < s->n_car; r++)
                    {
                        /* Same lifetime rule: the slot can be swapped and its
                         * carrier freed by w_replace()/join(). */
                        struct tbt_carrier *lc = tbt_carrier_get(s, (u8)r);

                        if (!lc)
                            continue;
                        live_bytes += READ_ONCE(lc->tx_bytes);
                        live_data += READ_ONCE(lc->data_frames);
                        live_retx += READ_ONCE(lc->retx_data);
                        tbt_carrier_put(lc);
                    }
                    seq_printf(m, "tunnel %llu reconcile live_bytes=%llu "
                                  "live_data=%llu live_retx=%llu "
                                  "retired_bytes=%llu retired_data=%llu "
                                  "retired_retx=%llu\n",
                               id, live_bytes, live_data, live_retx,
                               tbt_stat_read(&s->retired_tx_bytes),
                               tbt_stat_read(&s->retired_data_frames),
                               tbt_stat_read(&s->retired_retx_data));
                }
            }
            up_read(&s->stats_lock);
        }
        tbt_session_put(s);
        tbt_tunnel_put(t);
    }
    kfree(refs);
    kfree(ss);
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
