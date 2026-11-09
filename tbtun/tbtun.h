/* tbtun.h - internal types for the tbtun kernel tunnel.
 *
 * Spec: DESIGN.md. This header owns the tunnel/session object model (§8.2) and
 * the /proc grammar limits (§8.4.1); the wire layout and the /proc names it
 * refers to live in tbt_uapi.h.
 *
 * Invariants this file must never break (DESIGN.md §14):
 *   - The frame header is exactly TBT_HDR_LEN (32) bytes on the wire, every
 *     profile, every direction. BUILD_BUG_ON in tbt_core.c enforces it.
 *   - TBT_PAYLOAD_MAX and TBT_CHUNK_MAX bound every allocation and every
 *     carrier write; nothing may be sized from a peer-controlled field.
 *   - TBT_LINE_MAX is the /proc line limit (§8.4.1); the parser refuses longer
 *     input rather than truncating it.
 */
#ifndef _TBTUN_H
#define _TBTUN_H

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/printk.h>
#include <linux/refcount.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>

/* Wire constants, /proc names and the version live in the shared header. */
#include "tbt_uapi.h"
/* The adaptive shape/switch structs and their mapping helpers. */
#include "tbt_fec.h"
#include <linux/version.h>

/* system_dfl_wq exists from 6.5. Its documented semantics are "unbound, not
 * concurrency managed, all queued works run immediately while max_active
 * allows". 6.1's system_unbound_wq is unbound and concurrency managed; the
 * module's works are short and sleepable, so the difference does not change
 * behavior. Keep one name so a 6.1 build needs no scattered guards. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
#error "tbtun needs Linux 6.1 or newer (README, DESIGN §2)"
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define TBT_WQ system_dfl_wq
#else
#define TBT_WQ system_unbound_wq
#endif

/* Bring-up tracing for the carrier window between a completed TCP handshake
 * and the HELLO reaching the wire, plus the one-shot session and FEC setup
 * lines. Both macros are silent at the production default `debug=0`, so a
 * 256-carrier session prints nothing per poll pass, per carrier or per share;
 * the arguments are not even evaluated. At `debug>=1` they use pr_info/pr_warn,
 * never pr_debug, because the field hosts do not enable dynamic debug and the
 * value of these lines is that the last one on the console is the last step
 * that ran before a hang or a panic. The unique prefix lets a capture filter
 * them out of the module's ordinary output; function, CPU and pid are on every
 * line because a step that never logs is the step that did not return. Keep
 * the call sites one-shot or rate-bounded: logging must not move the timing it
 * measures.
 *
 * A failure an operator must see with `debug=0` does not belong here: use
 * pr_warn_ratelimited()/pr_err() at the call site. TBT_DIAG_WARN is for a
 * diagnostic that happens to describe a transient condition. */
#define TBT_DIAG(fmt, ...)                                            \
    do {                                                              \
        if (debug >= 1)                                               \
            pr_info("tbtun-diag: %s cpu=%u pid=%d " fmt "\n",         \
                    __func__, raw_smp_processor_id(),                 \
                    task_pid_nr(current), ##__VA_ARGS__);             \
    } while (0)

#define TBT_DIAG_WARN(fmt, ...)                                       \
    do {                                                              \
        if (debug >= 1)                                               \
            pr_warn("tbtun-diag: %s cpu=%u pid=%d " fmt "\n",         \
                    __func__, raw_smp_processor_id(),                 \
                    task_pid_nr(current), ##__VA_ARGS__);             \
    } while (0)

/* kernel_bind() and kernel_connect() took struct sockaddr * through 6.18.
 * The "net: Introduce struct sockaddr_unsized" series (net-next-6.19) changed
 * both to struct sockaddr_unsized *; the three call sites name the parameter
 * type once here instead of carrying a guard each. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
typedef struct sockaddr_unsized tbt_sockaddr;
#else
typedef struct sockaddr tbt_sockaddr;
#endif

/* Read a statistics counter (DESIGN.md §8.7) as the u64 the renderers print
 * with %llu. Every read of an atomic64_t counter goes through this or
 * atomic64_read(); the lint check in review/lint.sh refuses a bare field
 * read. */
static inline u64 tbt_stat_read(const atomic64_t *v)
{
    return (u64)atomic64_read(v);
}

/* struct tbt_hdr and the frame codec, shared with the host parser test. */
#include "tbt_proto.h"
/* The HTTP/1.1 carrier container (C8); pure code, host-tested. */
#include "tbt_http.h"

struct tbt_hole {
    u64 sid;          /* 40-bit stream id; u64 avoids truncating the prefix */
    u64 seq;
    u16 len;
    u64 declared_ns;
};

/* struct tbt_cfg, struct tbt_linebuf and TBT_LINE_MAX live in tbt_parse.h,
 * which compiles both into the module and into host/test_parse.c. */
#include "tbt_parse.h"
/* The absolute source-port bitmap and its window helpers are pure code,
 * shared with the host test (tbt_portmap.h). */
#include "tbt_portmap.h"

/* The four callbacks sock_init_data() installs.  Take the snapshot from a
 * fresh sock_create_kern() socket, before this module replaces anything.
 * Never snapshot an accepted socket: it inherits the listener's
 * sk_data_ready, which is already tbt_ready(). */
struct tbt_sk_ops
{
    void (*data_ready)(struct sock *sk);
    void (*write_space)(struct sock *sk);
    void (*state_change)(struct sock *sk);
    void (*error_report)(struct sock *sk);
};

/* A configured tunnel. One `/proc` line, one of these. */
struct tbt_tunnel
{
    struct list_head node; /* global list, tbt_lock */
    struct net *net;
    refcount_t refs;
    struct rcu_head rcu;
    struct tbt_cfg cfg;

    struct tbt_session *sess; /* C2: one session per tunnel */
    struct list_head sessions;

    /* Crypto state kept from the tunnel this one replaced. Each side checks
     * every later HELLO against the first one it saw: a different key is
     * "crypto key changed" and a different TX nonce is "crypto nonce
     * changed". The replaced session's last HELLOs and the replacement's
     * HELLOs are indistinguishable to the peer, so regenerating either one
     * rejects every carrier of the replacement (measured: 256 carriers, near
     * up=18, 25 far HELLO rejections). The TX nonce is inherited whenever
     * the role is unchanged. The key is inherited only by a near side, and
     * only when it generated and announced that key (key_id=1): a configured
     * or adopted key is not this tunnel's to carry. */
    u8 crypto_key_inherit[TBT_CRYPTO_KEY_LEN];
    u8 crypto_tx_nonce_inherit[TBT_CRYPTO_NONCE_LEN];
    bool crypto_key_inherit_valid;
    bool crypto_tx_nonce_inherit_valid;

    /* Listener, present for a near tunnel once bound. */
    struct socket *lsock;
    struct work_struct w_accept; /* scheduled from lsock->sk->sk_data_ready */
    atomic_t accepting;          /* 1 while the accept loop may run */
    struct tbt_sk_ops def_ops;   /* core callbacks, saved before tbt_ready */

    u64 streams;      /* accepted connections seen so far */
};

/* Module-wide state, owned by tbt_core.c. */
extern struct list_head tbt_tunnels;
extern struct mutex tbt_lock;
extern unsigned int debug;
extern unsigned int tbt_fec_groups_per_flow;
extern unsigned int max_tunnels;
extern unsigned int max_carriers;
/* Peripheral BBR hold, opt-in (module param hold_enable). */
extern bool tbt_hold_enable;

/* Carrier source ports (DESIGN.md §7.2). `used` is one absolute bitmap over
 * the whole 16-bit port space; `lo`/`hi` are the global fallback window for a
 * tunnel whose own portblock= is 0-0. Owned by tbt_session.c. */
struct tbt_portmap
{
    struct mutex lock;
    bool configured;
    u16 lo;
    u16 hi;
    unsigned long *used;
};

/* Why tbt_port_alloc() returned 0.  The caller turns this into a per-session
 * counter and a ratelimited warning; a carrier is never killed for it. */
enum tbt_port_alloc_err
{
    TBT_PORT_ALLOC_OK = 0,
    TBT_PORT_ALLOC_NOMEM,     /* the bitmap allocation failed */
    TBT_PORT_ALLOC_EXHAUSTED, /* every port in the tunnel's window is in use */
};

/* tbt_core.c */
bool tbt_debug_loss(void);

/* tbt_session.c */
extern struct tbt_portmap tbt_ports;
int tbt_portmap_set(u16 lo, u16 hi);
void tbt_portmap_get(u16 *lo, u16 *hi, bool *configured);
void tbt_portmap_exit(void);
void tbt_portmap_show(struct seq_file *m);
u16 tbt_port_alloc(const struct tbt_tunnel *t, enum tbt_port_alloc_err *err);
void tbt_port_free(u16 port);
int tbt_carrier_init(void);
int tbt_tunnel_add(const struct tbt_cfg *cfg, struct net *net);
int tbt_tunnel_del(u64 id);
void tbt_tunnel_show(struct seq_file *m);
void tbt_peers_show(struct seq_file *m);
int tbt_action_apply(const struct tbt_cmd *cmd);
void tbt_tunnel_put(struct tbt_tunnel *t);
void tbt_tunnel_shutdown(void);

/* tbt_listen.c */
void tbt_accept_work(struct work_struct *w);
int tbt_listen_start(struct tbt_tunnel *t);
void tbt_listen_stop(struct tbt_tunnel *t);

/* ------------------------------------------------------------------ *
 * C3 data path: one session, n carriers, one stream.
 * (DESIGN.md §15 C3; duplication and prediction are C5-C6.)
 * ------------------------------------------------------------------ */

#define TBT_STREAM_HT 64
#define TBT_TOMBSTONE_MS 2000
#define TBT_REAP_INTERVAL_MS 200
/* A destroy that cannot collect every session reference is a bug, not a
 * wait. The 0.2.1 `del 1` hung for hours in wait_for_completion() and pinned
 * rmmod behind it; the wait is bounded and a timeout reports and leaks the
 * session instead of the thread. */
#define TBT_SESSION_DRAIN_MS 10000
#define TBT_PENDING_MAX 128
#define TBT_STREAMS_MAX 128
#define TBT_STREAM_SNAP_MAX 128
/* Discrete RTO grid for the stall survival estimate (0.5/1/2/4/8/16 s). */
#define TBT_STALL_GRID 6
#define TBT_SQ_MAX (64 * 1024)      /* minimum frame queue cap, bytes    */
/* Ready-to-deliver queue cap. It must exceed the volume a strict
 * send-then-read application can echo back before it starts reading (4 MiB in
 * the harness), or the delivery pause stops the carrier read and with it the
 * feedback that the opposite direction needs: measured as a stall at ~300 KiB
 * of ready data with the old 256 KiB cap. The reorder tree is bounded
 * separately by rx_reorder_bytes, and the sender window by the received ack. */
/* Ready-to-deliver queue cap. It must hold the echo a strict send-then-read
 * application has not read yet: the application's own receive buffer does not
 * autotune without reads, so this queue is the only place those bytes can
 * wait. The harness raises the socket maxima to 64 MiB and moves 40 MiB. */
#define TBT_RX_RING_MAX (64 * 1024 * 1024) /* ready-to-deliver queue cap, bytes  */
#define TBT_RX_READY_LOW (TBT_RX_RING_MAX / 2)
#define TBT_RX_PAUSED_MARK (2 * 1024 * 1024) /* FBK rx_state=2 above this   */
/* The carrier write admission is the kernel's own sk_stream_memory_free()
 * (sk_wmem_queued < sk_sndbuf), not a fixed high-water line: the carrier's
 * TCP applies backpressure, sk_write_space is the fast resume, and a 2 ms
 * per-carrier w_pump covers the case where the stack does not call back
 * (DESIGN.md §5.5). */

/* Feedback cadence (DESIGN.md §5.5): one FBK per session every 10 ms, or
 * immediately after this many received DATA frames. */
#define TBT_FBK_INTERVAL_MS 10
#define TBT_FBK_FRAMES 32
/* An idle stream still needs a keepalive, but not a feedback frame every
 * TBT_FBK_INTERVAL_MS: the FBK carries per-path receive counters, so it is
 * only new information when one of them (or the stream's rx state) changes.
 * The 10 ms work tick stays: it drives the retransmit timer and the close
 * and hole checks. */
#define TBT_FBK_IDLE_MS 1000

/* Pinned control-frame lifetime (review 22). A control frame is never freed
 * on a transient write error: it is retried this many times, 2 ms apart, and
 * only then counted in ctl_lost. A dead carrier slot waits longer because a
 * replacement dial is already in flight. */
#define TBT_CTL_MAX_RETRIES 4
#define TBT_CTL_RETRY_MS 2
#define TBT_CTL_PATH_RETRY_MS 100
#define TBT_CTL_QUEUE_MAX 256

/* Retransmit timer: a retained frame whose last write is older than this and
 * that the peer has not delivered is re-sent on another path. The timeout is
 * SRTT + 4 x RTTVAR (tbt_tx_rto_ns); the floor is TCP's 200 ms minimum, so a
 * jittery path does not declare a frame lost while its ack is still inside
 * the jitter band. It must stay below the receiver's hole budget: a 200 ms
 * floor leaves room for a repair to arrive before the 500 ms default. */
#define TBT_RETX_MIN_US 200000
#define TBT_RETX_MAX_US 500000 /* must cover 2x the feedback delay */

/* Replay ring: acked frames stay available for re-send for this long, or
 * until cfg.replay bytes are held, whichever comes first (DESIGN.md §9.6). */
#define TBT_REPLAY_MS 1000

/* The sender's window (DESIGN.md §5.4): how far it may run ahead of the
 * peer's contiguous received offset. `tx_window_bytes=0` (the default)
 * derives it from the path:
 *
 *   auto = min(max(k x mean_BDP, 2 frames), rx_reorder_bytes)
 *   k = (RTO + one_way + fb_delay) / mean_srtt
 *
 * The 1 MiB bootstrap value applies only while no carrier has a sample; it
 * must not override k x B afterwards (measured: a 70 KB BDP at k=2 turned
 * into a 1 MiB window, whose 3.4 s drain at 2.4 Mbit broke the 1.5 s hole
 * budget).
 *
 * mean_BDP = mean_srtt x sum(delivery_rate), with the rate and SRTT from each
 * carrier's own TCP BBR estimator (rate_delivered/rate_interval_us, srtt_us)
 * and RTTVAR from mdev_us. The window covers one loss-repair interval, so
 * the receiver's peak reorder occupancy is the whole window and the capacity
 * identity is R >= W (DESIGN.md §5.4). A fixed 128 KiB (the old
 * rx_reorder_bytes/2) capped a 218 ms, 17 Mbit path at ~4 Mbit; that path's
 * BDP is ~465 KiB. The ceiling is the receiver's reorder budget, never a sum
 * of carrier cwnd: carrier TCP frees cwnd on its own ACK while the bytes
 * still sit in the receiver's tree. A frame beyond the budget is a stream
 * kill, not a retransmit. */
#define TBT_TX_WINDOW_AUTO_FLOOR (1024 * 1024)

/* Total carrier socket-buffer budget, divided by the carrier count. At 256
 * carriers this is 512 KiB each (the kernel doubles the setsockopt value),
 * 128 MiB in total; at n=1 it is capped by the per-carrier 1 MiB setsockopt
 * value (2 MiB effective), enough for the 1 MiB window. Without this bound,
 * an 8 MiB per-carrier setting would allocate 2 GiB at 256 carriers. */
#define TBT_CAR_SOCKBUF_TOTAL (128 * 1024 * 1024)

/* A carrier that never presents a HELLO is retired after this long. */
#define TBT_JOIN_TIMEOUT_MS 5000

/* C6: hole detection and the bounded reset (DESIGN.md §4.3-§4.6). */
#define TBT_K_MAX_HOLE 3          /* 0.919^3 is the declare threshold */
#define TBT_HOLE_BUDGET_BYTES 65536u
#define TBT_HOLES_MAX 256         /* declared-hole FIFO, per session */

/* Why a timer or injection copy exists. Stamped when the copy is queued and
 * counted when the carrier accepts the write, so retx_ppm can name its own
 * numerator instead of guessing after the fact. */
enum tbt_retx_src {
    TBT_RETX_NONE = 0,
    TBT_RETX_HOLE,     /* frame at the ack, stalled >= 2 x RTO */
    TBT_RETX_SLOW,     /* TCP delivered it, uack has not covered it */
    TBT_RETX_PATHLOSS, /* carrier_lost marked it retx_now */
};

/* A frame that has no carrier yet. Carrier indices are 0..255, so 0xff is a
 * real index in a 256-carrier session and cannot double as a sentinel: a
 * direct sess->car[fr->path] would send a poisoned frame to carrier 255.
 * Every path lookup therefore compares against sess->n_car, which rejects
 * TBT_PATH_NONE for every carrier count. */
#define TBT_PATH_NONE 0xffffu

struct tbt_frame
{
    struct list_head list;
    struct rb_node rb;   /* RX reorder tree only */
    u16 len;             /* payload bytes */
    u16 off;             /* delivery progress, RX frames only */
    u8 open_pending;     /* DATA enqueued before this stream's OPEN */
    bool pinned;         /* path is fixed: HELLO/OPEN/FIN/RST copies */
    bool dup_copy;       /* a duplicate copy (LOW_PRIO on the wire) */
    bool retx_copy;      /* a timer/injection copy: counted when written */
    u8 retx_src;         /* enum tbt_retx_src: which branch queued it */
    bool dropped;        /* drop_ppm discarded it: a real hole, not a queue */
    u16 path;            /* carrier index, or TBT_PATH_NONE before selection */
    u32 gen;             /* pinned frames: carrier generation they belong to */
    u8 retries;          /* transient control failures (review 22) */
    u64 retry_at_ns;     /* earliest next attempt; 0 = now */
    DECLARE_BITMAP(avoid_mask, TBT_MAX_CARRIERS_LIMIT);
                         /* paths this copy must not reuse */
    struct tbt_retain *ret; /* ring entry for a DATA frame, valid in the pump */
    struct tbt_stream *st;  /* DATA owner; holds a ref */
    struct tbt_hdr hdr;  /* already filled in, including sid and seq */
    u8 data[];           /* len bytes, or a PAD frame with len 0 */
};

/* One DATA frame kept for retransmission until the peer has delivered every
 * byte of it. Lives on the stream, never on the session TX queue: the queue
 * holds short-lived copies, so the two lists can never share an object. */
struct tbt_retain
{
    struct list_head list;
    u64 seq;             /* byte offset of the first payload byte */
    u16 len;
    u16 path_mask;       /* every path a copy of this seq was written on */
    u16 reserve_mask;    /* selected but not yet written (in flight) */
    u8 path_gen[TBT_MAX_CARRIERS_LIMIT]; /* generation of the carrier that
                                          * wrote each bit */
    u64 path_end[16];    /* end offset in that path's payload-byte space */
    u32 first_sent_seq[16]; /* write_seq just past the record, per path: the
                             * mark the carrier's TCP must ack before this
                             * copy counts as on the wire. Keyed by
                             * (index, path_gen) like every other per-path
                             * field here. */
    u64 una_observed_ns[16]; /* when snd_una first passed first_sent_seq;
                              * the retransmit timer runs from this */
    u8 last_path;        /* carrier the last copy went out on */
    u8 sends;            /* copies written so far */
    u8 dup_pending;      /* duplication copies queued but not yet written */
    bool pending;        /* a retransmit copy is queued for this entry */
    bool retx_now;       /* its path died: re-send without waiting for the RTO */
    bool declared;       /* the geometric test declared this frame lost */
    u64 first_sent_ns;
    u64 last_sent_ns;
    struct tbt_hdr hdr;
    u8 data[];
};

/* Per-stream scheduler state. The session owns one more copy for control
 * frames (FBK fallback, SACK before a stream is known); DATA uses the stream's
 * own copy. Single-stream sessions are equivalent because only one stream
 * advances its window. */
struct tbt_sched
{
    u8 order[TBT_MAX_CARRIERS_LIMIT];
    u8 n;
    u8 need_open;
    u32 win;
    u32 rot;
    u64 sort_ns;
};

struct tbt_stream
{
    struct hlist_node node;   /* session->streams */
    refcount_t refs;
    struct rcu_head rcu;
    struct tbt_session *sess;
    u64 sid;
    u8 state;                 /* TBT_ST_* */
    u8 kill_reason;
    bool near;                /* this end allocated the sid */

    /* The one socket this stream owns: the accepted public socket on the near
     * side, the target connection on the far side. Capture reads from it
     * (tcp_read_sock); delivery writes to it (tcp_sendmsg_locked). The app on
     * the other end of it is an ordinary TCP peer with its own fd. */
    struct sock *sk;
    unsigned long attached;     /* TBT_STREAM_F_* bits */
    u64 tombstone_until_ns;     /* P1: keep a DEAD sid as a tombstone until */
    struct work_struct w_read;  /* capture: sk_receive_queue -> tbt_tx */
    struct work_struct w_drain; /* delivery: rx_q -> transmit */
    struct work_struct w_ctrl;  /* FIN/RST noticed in sk_state_change */
    struct work_struct w_kill;  /* reset close: detach, RST, release */
    struct work_struct w_target;/* far side: dial target for this stream */
    struct socket *target;      /* far side: target socket for this stream */
    bool target_started;        /* w_target scheduled once */

    /* TX: byte-offset sequence assignment and the replay retention list. */
    spinlock_t tx_lock;
    u64 tx_seq;               /* next byte offset to assign */
    u64 tx_ack;               /* per-stream delivered offset */
    u64 last_ack_ns;          /* this stream's last tx_ack advance */
    u64 pause_cleared_ns;     /* this stream's loss timers restart time */
    bool peer_paused;         /* this stream's peer reports a full queue */
    u64 sack_next;            /* last SACK offset sent */
    u8 sack_state;            /* last SACK state sent */
    bool sack_dirty;          /* rx_next/state changed since last SACK */
    u32 fbk_frames;           /* DATA frames since this stream's last FBK */
    u64 fbk_sig;              /* carrier rx signature at the last FBK sent */
    u64 fbk_last_rx_next;     /* delivered offset carried by that FBK */
    u8 fbk_last_state;        /* rx_state carried by the last FBK sent */
    u64 fbk_last_ns;          /* when the last FBK was sent; 0 = never */
    bool tx_fin_sent;         /* FIN wanted: set by the producer before the
                               * first tbt_tx_fin(); the FBK tick retries while
                               * tx_fin_queued is false */
    bool tx_fin_queued;       /* at least one FIN copy entered tx_q */
    bool tx_fin_written;      /* at least one FIN copy reached the wire */
    bool tx_fin_seq_set;      /* tx_fin_seq frozen at the first request */
    u64 tx_fin_seq;           /* byte offset every FIN copy names */
    /* FEC FIN body (TBT_FIN_BODY_LEN): absolute session packet seqs, frozen
     * with tx_fin_seq. Non-FEC streams leave them zero. */
    u64 tx_fin_pkts;          /* pkt_end: after the last DATA packet */
    u64 tx_fin_pkt_base;      /* pkt_base: first DATA packet, block-aligned */
    /* The FEC flow this stream owns. Set when the stream is admitted (near)
     * or its OPEN is bound (far); cleared at detach. It is the only pointer
     * the capture, FIN and ACK paths use to reach the stream's FEC plane. */
    struct tbt_fec_flow *fec_flow;
    u64 fec_pkt_base;         /* first global FEC packet seq of this stream */
    bool fec_pkt_base_set;
    u64 fec_pkt_end;          /* actual end, frozen by tbt_fec_tx_finish() */
    bool fec_finished;        /* finish ran: block closed and seq advanced */
    u64 close_since_ns;       /* first FIN request; the bounded close timer
                               * runs from here and from the last ack, so a
                               * slow but progressing drain is not cut short */
    u64 close_fin_written_ns; /* our FIN reached the wire: the peer-FIN bound */
    /* These are indexed by carrier, and a FEC v6 session has up to 256
     * carriers: one `unsigned long` or a `[16]` array would write past itself
     * at index 16..255 and corrupt whatever follows in the struct (measured:
     * `announced_gen[c->index]` overwrote tx_paused with a carrier generation,
     * so every `READ_ONCE(st->tx_paused)` load read an invalid _Bool, the
     * announce check failed forever, and the stream never delivered). */
    DECLARE_BITMAP(tx_fin_mask, TBT_MAX_CARRIERS_LIMIT);
    DECLARE_BITMAP(tx_fin_inflight, TBT_MAX_CARRIERS_LIMIT);
    bool tx_rst_written;      /* at least one RST copy reached the wire */
    DECLARE_BITMAP(tx_rst_mask, TBT_MAX_CARRIERS_LIMIT);
    DECLARE_BITMAP(tx_rst_inflight, TBT_MAX_CARRIERS_LIMIT);
    bool tx_open_sent;        /* tbt_tx_announce() ran before capture attached */
    DECLARE_BITMAP(announced_mask, TBT_MAX_CARRIERS_LIMIT);
    u8 announced_gen[TBT_MAX_CARRIERS_LIMIT]; /* generation per announced carrier */
    bool tx_paused;           /* session queue full: stop draining this socket */
    u64 tx_start_ns;          /* first frame queued for this stream */
    struct tbt_sched sched;   /* DATA/stream-control path selection */
    struct list_head retain;  /* struct tbt_retain, ordered by seq;
                               * protected by sess->tx_lock */
    struct tbt_hole *holes;   /* per-stream declared-hole FIFO */
    u16 hole_head, hole_tail;
    u32 retain_bytes;
    u32 retain_max;           /* cfg.tx_retain_bytes, 0 = unlimited */

    /* RX: bytes from the carriers on their way to the output socket.
     * rx_q holds the contiguous run that starts at the delivery offset;
     * rx_tree holds frames that arrived beyond a hole. */
    spinlock_t rx_lock;
    struct list_head rx_q;    /* struct tbt_frame, contiguous from rx_next */
    struct rb_root_cached rx_tree;
    u32 rx_queued;            /* bytes in rx_q */
    u32 rx_ooo_bytes;         /* bytes in rx_tree */
    u32 rx_ooo_frames;        /* frames in rx_tree */
    u32 rx_max_depth;         /* highest rx_ooo_frames ever seen */
    u32 rx_max_stall_us;      /* longest hole before fill or reset */
    u32 rx_limit;             /* cfg.rx_reorder_bytes for this stream */
    u64 rx_next;              /* next byte offset expected from the network */
    u64 rx_delivered;         /* bytes accepted by the output socket */
    u64 hole_since_ns;        /* a hole is outstanding since this time */
    u64 rx_fin_seq;           /* byte offset the peer's FIN follows */
    bool rx_fin_seen;
    bool rx_fin_done;
    u64 rx_fin_done_ns;       /* when the peer's FIN reached the application;
                               * bounds a target that never closes its half */
    u64 deliver_mark;         /* last rx_delivered the FBK tick saw */
    u64 deliver_since_ns;     /* no delivery progress since this time */
    u8 rx_err;                /* pending socket error, 0 = none */
    bool paused;              /* rx_q full: stop reading carriers */

};

/* Bits in tbt_stream.attached.  Bit 0 is the socket callback wiring; the
 * other two make the terminal transition and the hash unlink single-winner
 * without growing the struct. */
#define TBT_STREAM_F_ATTACHED 0
#define TBT_STREAM_F_DYING    1
#define TBT_STREAM_F_UNLINKED 2
#define TBT_STREAM_F_ACTIVE   3
/* 0.2.0 stream work references (review/s2-merged-design.md §1.4): one bit per
 * scheduled work instance. Bit 1 means one instance exists that has not yet
 * entered its entry point, and that instance holds one stream reference.
 * tbt_stream_schedule() owns the 0->1 transition; the entry point and
 * tbt_stream_cancel() own 1->0. Bits 0..3 keep their 0.1.0 meaning. */
#define TBT_STREAM_F_SCHED_READ   4
#define TBT_STREAM_F_SCHED_DRAIN  5
#define TBT_STREAM_F_SCHED_CTRL   6
#define TBT_STREAM_F_SCHED_KILL   7
#define TBT_STREAM_F_SCHED_TARGET 8

enum tbt_stream_state
{
    TBT_ST_PENDING = 0, /* far side: carrier attached, no OPEN yet */
    TBT_ST_OPEN,
    TBT_ST_GAP, /* a hole C2 cannot fill: distinct from a plain reset */
    TBT_ST_REORDER, /* reorder window overflow: distinct from a gap */
    TBT_ST_HOLE, /* hole budget expired: bounded reset, DESIGN.md §4.6 B */
    TBT_ST_DEAD,
};

/* Per-carrier partial record state. Lives on the carrier, never global: two
 * tunnels (or, from C4, two carriers of one tunnel) must not share a buffer.
 * A record is TBT_REC_MAX (old ARQ) or TBT_FEC_REC_MAX (v6); the buffer must
 * hold the larger one. The write-side tail must hold a whole batched FEC
 * write (TBT_FEC_BATCH_MAX concatenated HTTP frames), because
 * tcp_sendmsg_locked() can short-write anywhere in that byte range.
 *
 * Keep the batch at 3 frames. Three frames are 24,780 B and stay in the
 * kmalloc-32k cache (order-3); eight frames are 66,080 B and need a
 * kmalloc-128k object (order-5). On the 955 MB far host the order-5 request
 * failed in tbt_carrier_send_fec_batch(), and direct reclaim/compaction
 * stalled the FEC drain for 0.2-2.4 s. tbt_core.c has a BUILD_BUG_ON that
 * keeps this buffer at or below 32 KiB. */
#define TBT_PARSER_BUF (TBT_FEC_REC_MAX > TBT_REC_MAX ? TBT_FEC_REC_MAX : TBT_REC_MAX)
#define TBT_FEC_BATCH_MAX 3
#define TBT_CAR_PART_MAX (TBT_HTTP_FEC_FRAME_MAX * TBT_FEC_BATCH_MAX)
struct tbt_parser
{
    u8 buf[TBT_PARSER_BUF];
    size_t have;
};

/* ------------------------------------------------------------------ *
 * FEC v6 data plane (grouped; one stream per session).
 *
 * carriers = groups * (n + m), one share per carrier. Global packet seq is
 * striped: group = seq % groups, block_id = (seq / groups) / n, and share =
 * (seq / groups) % n. A block of every group therefore covers one contiguous
 * span of groups * n global packets (the super-block), which is also the unit
 * the receiver delivers: all groups' block b must be complete before global
 * seq b*groups*n is handed to the stream, so the byte stream never has a hole.
 *
 * The host codec is the authority for encode/decode; this state only holds the
 * packets that are still inside the current block. A block is n DATA shares
 * followed by m PARITY shares; the sender emits the parity records as soon as
 * the block's last DATA record is queued. The receiver decodes a block when
 * its data and parity shares make the n unknowns solvable.
 * ------------------------------------------------------------------ */
struct tbt_fec_rlnc;
struct tbt_fec_grouped;

struct tbt_fec_blk
{
    u64 block_id;
    /* Stream generation this block was placed for. A new stream bind clears
     * every block whose sid differs, so the previous stream's reorder bytes
     * cannot be delivered to the new one. */
    u64 sid;
    bool used;
    bool ready;
    u16 nreal;    /* real DATA packets in this block; 0 = not yet known */
    /* Counts are derived from dpresent/ppresent and cached; count_real names
     * the real length they were computed for, so a stale count is never used
     * after the block's shape becomes known. */
    u16 ndata;        /* present real DATA shares in [0, real) */
    u16 nparity;      /* present PARITY shares */
    u16 count_real;   /* real length ndata/nparity were computed for */
    bool decoding;    /* w_fec_rx owns this block: no second decoder */
    /* A block codec that could not finish the block keeps the block
     * decodable, but every arriving share would re-run a decode that costs
     * a whole block (downlink ~1 MiB). Back off after a failure so a 50%
     * loss block does not decode 128 times before its NACK repairs arrive. */
    u8 decode_tries;
    u64 decode_next_ns;
    bool empty;       /* PARITY marker with win_len == 0 */
    bool win_len_set; /* a PARITY record fixed win_len/short_len */
    /* One shape-invariant buffer of (n+m)*s bytes. A switch only re-slices
     * data/parity inside it: Phase 1 keeps n+m = 8, so the allocation never
     * changes. */
    u8 *buf;
    u8 *data;     /* n * s bytes: buf[0..n*s) */
    u8 *parity;   /* m * s bytes: buf[n*s..(n+m)*s) */
    /* The estimator's evidence for this block: bit i marks DATA share i as
     * needing repair (parity recovered it or a NACK named it). The u32 is
     * the RLNC estimator's 8-share window; the bitmap is the SC-LDPC
     * estimator's 256-share mask. Both are filled at the same two points. */
    u32 miss_mask;
    u8 miss_bits[TBT_FEC_BITMAP_BYTES];
    /* Missing shares the last decode attempt could not recover. Rebuilt on
     * every attempt, and masked by dpresent at the estimator fold. */
    u8 codec_fail_bits[TBT_FEC_BITMAP_BYTES];
    bool judged;  /* already folded into tbt_fec_est_super() */
    u8 *dpresent; /* n flags */
    u8 *ppresent; /* m flags */
    u16 *dlen;    /* n payload lengths */
    /* Payload length of the block's last real share when it is short, from the
     * parity record's epoch field. A recovered share has no payload_len of its
     * own, so without this a receiver that decoded the block from parity would
     * have to guess S and hand the application the block's zero padding. */
    u16 short_len;
    /* The block needed recovery and holds a short share whose length no record
     * stated (a peer that does not carry it): refuse rather than guess. */
    bool short_unknown;
    /* When this block first showed a gap. The first NACK waits one round trip
     * from here: the carrier TCP and the block's own parity may still repair
     * it, and a NACK sent earlier only produces a late duplicate. Cleared
     * with the rest of the block when the slot takes a new block id. */
    u64 first_gap_ns;
};

/* One group's encoder state. The ring, packet buffer and record buffer are
 * shared by all groups because the packetizer is one ordered stream. */
struct tbt_fec_gtx
{
    u8 *buf; /* (n+m)*s bytes: blk and par are slices of it */
    u8 *blk; /* n * s bytes: this group's current block */
    u8 *par; /* m * s bytes: this group's parity scratch */
    /* Block this group's nreal/last_len belong to. A block that already
     * emitted its parity has nreal == 0, so nreal alone cannot tell "full and
     * done" from "no packet in this block"; blk_id makes the difference
     * explicit and lets close_block() emit a marker for an empty group. */
    u64 blk_id;
    /* Real DATA packets written into `blk` for the block being built. The
     * packetizer closes a block early when the application goes quiet, so the
     * block's real length is a counter, not "n" or "the last seq mod n". */
    u16 nreal;
    /* Payload length of the last real share written into `blk`, 0 when it is
     * full. It travels in the parity record's epoch field so a receiver that
     * recovers that share still knows how many bytes it carried. */
    u16 last_len;
};

struct tbt_fec_replay;
struct tbt_fec_nack_state;
struct tbt_fec_adapt;
struct tbt_fec_est;
struct tbt_fec_flow;
struct tbt_fec_drain;
struct tbt_fec_adapt_sclc_ctl;
struct tbt_fec_adapt_sclc_est;

/* One side of an FEC_ALLOC switch: the physical group set a flow will own
 * after the exchange, plus the flow-local packet/block origin of the first
 * record sent on that set. It is shared by the two TX roles (initiator and
 * responder) and by the pending RX switch. */
struct tbt_fec_alloc_flow
{
    u32 epoch;
    u8 mask[TBT_FEC_BITMAP_BYTES];
    u16 map[TBT_FEC_MAX_GROUPS];
    u16 groups;
    u64 pkt;
    u64 block;
};

struct tbt_fec_tx
{
    /* This plane belongs to one stream. `sid` is the routing key; the group
     * map turns a local group index (0..groups-1) into the physical group
     * that owns the carrier. Both ends derive the same map from the bitmap
     * OPEN/ALLOC carried. */
    u64 sid;
    u8 group_mask[TBT_FEC_BITMAP_BYTES];
    u16 group_map[TBT_FEC_MAX_GROUPS];
    struct tbt_fec_flow *flow;
    spinlock_t lock; /* protects ring head/tail and the counters below */
    u8 *ring;
    u32 ring_size;
    u32 head, tail;
    u8 *pkt; /* one packet under construction */
    u8 *rec; /* TBT_FEC_REC_MAX: one complete record */
    u16 fill;
    u64 seq; /* next global packet seq */
    /* Flow control, from the peer's ACK: the delivered packet frontier and the
     * credit it grants beyond it. The receiver buffers `slots` super-blocks, so
     * a sender that stays inside the credit cannot outrun that buffer. */
    u64 peer_pkts;
    u32 credit;
    /* Control records (ACKs) rotate over the up carriers: the index after the
     * last carrier that accepted one. A single pinned carrier can hold every
     * ACK behind its own data, and a late ACK is a stalled credit window. */
    u16 ctl_rot;
    bool flush_req;
    /* Session teardown: the packetizer, flush and every re-arm refuse new
     * work. Set by tbt_session_destroy() before the cancel pass. */
    bool stopping;
    /* When the packetizer may emit the partial packet it holds. Set when the
     * ring ran dry with bytes in `pkt`, cleared when it emits. 0 = not armed. */
    u64 short_at_ns;
    u16 groups, n, m, s;
    /* Bound data records this flow still has on the carrier queues. The FIN
     * gate reads it instead of the session-wide count, so one stream's
     * backlog cannot hold another stream's close. */
    u32 queued;
    /* This flow is closing: its replay admission uses the close budget. */
    bool closing;
    u64 cur_block; /* block being built, shared by every group */
    /* Last share the packetizer built or queued. Diagnostics only: the
     * TXSTALLKILL log reads it to name the share that was in flight. The TX
     * work is the only writer; a u16 store/load is atomic. */
    u16 last_group;
    u16 last_share;
    u8 *data_ptrs[TBT_FEC_MAX_SHARES];
    u8 *parity_ptrs[TBT_FEC_MAX_SHARES];
    struct tbt_fec_gtx *g; /* groups entries */
    u16 g_cap;             /* entries allocated in g[]; never shrinks */
    struct tbt_fec_grouped *codec;
    u64 nonce;
    /* Bounded replay: the last K super-blocks of encoded DATA+PARITY
     * records. The NACK handler reads it from carrier rx context; the
     * packetizer writes it. One lock, held only for a record copy. */
    struct tbt_fec_replay *replay;
    spinlock_t replay_lock;
    /* Peer RTT from the ACK body: how long a replayed share needs to come
     * back. The NACK reply deduplicates inside this window. */
    u32 rtt_us;
    /* No usable carrier: the first wait start and the request state. A
     * replacement is requested through the existing w_replace path; the
     * wait is retried every 2 ms and never drops a source share. */
    u64 carrier_wait_since_ns;
    bool carrier_wait;
    bool backpressure_counted;
    /* Adaptive shape (0.3.0). base_pkt/base_block are the relative origin of
     * the shape in force; the mapping helpers in tbt_fec.h use them.
     * shape_lock protects the shape fields the carrier RX path reads (the
     * NACK handler); the packetizer is the only writer and runs in w_fec_tx. */
    u64 base_pkt;
    u64 base_block;
    u32 credit_cap; /* cached credit cap: the ACK path must not read n/m */
    u16 band;       /* parity band of the shape, for the NACK reply */
    /* Initial-shape announcement (TBT_FEC_CFG_F_INIT). The codec's shape
     * fields are not the RLNC window, so they are kept apart from n/m. */
    u16 shape_w;
    u16 shape_k;
    u8 shape_d;
    u8 init_tries;
    bool init_done;
    u64 init_ns;
    /* The packetizer stopped on the outq high-water mark and re-armed; kept
     * so the transition is counted once per episode, not once per retry. */
    bool outq_paused;
    spinlock_t shape_lock;
    struct tbt_fec_grouped *old_codec; /* released on the next swap */
    u8 switch_state;                   /* enum tbt_fec_switch_state */
    u8 switch_retries;
    u32 switch_epoch;
    u64 switch_pkt;   /* P0: first new-shape record */
    u64 switch_block; /* B0: first new-shape block id */
    u64 switch_start_ns;
    u64 switch_cfg_ns; /* last CFG (re)transmission */
    struct tbt_fec_shape switch_shape;
    /* Control law state (tbt_fec_adapt.h). Allocated in init so this header
     * stays independent of the pure logic module. */
    struct tbt_fec_adapt *adapt;
    /* SC-LDPC control law (mode 3). Allocated for every mode-3 session and
     * disabled when adaptive=off, so a runtime enable needs no allocation. */
    struct tbt_fec_adapt_sclc_ctl *adapt_sclc;
    /* Peer report from FEC_FBK plus the local residual counters. */
    u32 peer_p_ppm;
    u32 peer_p_fast_ppm;     /* SC-LDPC: FBK bitmap tail, ppm/16 */
    u32 peer_codec_fail_ppm; /* SC-LDPC: FBK bitmap tail, ppm/16 */
    u16 peer_expected;
    u16 peer_received;
    u32 peer_epoch;
    u8 peer_flags;
    bool peer_est_valid;
    u64 peer_est_ns;
    u64 nack_req_shares; /* DATA shares named in received NACKs, total */
    u64 data_pkts_sent;  /* DATA packets emitted, total */
    u64 fail_win_ns;     /* start of the current r_fail window */
    u64 fail_req_last;
    u64 fail_pkts_last;
    /* Dynamic group rebalance (FEC_ALLOC). The near side is the only
     * initiator; the far side holds the same flow's TX at P0 and answers.
     * alloc_target is the new set our own TX will use; alloc_peer is the
     * peer's TX origin, which becomes our RX base. All fields are read by
     * carrier RX and written by carrier RX/w_fec_tx under alloc_lock except
     * the ones only the TX work touches. */
    struct tbt_fec_alloc_sm alloc;
    spinlock_t alloc_lock;
    struct tbt_fec_alloc_flow alloc_target;
    struct tbt_fec_alloc_flow alloc_peer;
    u8 alloc_role; /* 0 none, 1 initiator (near), 2 responder (far) */
    bool alloc_hold; /* packetizer stopped at alloc_target.pkt */
    bool alloc_remote_req; /* responder: REQUEST waiting for w_fec_tx */
    bool alloc_ack_sent;   /* responder: ACK sent for alloc_target.epoch */
    bool alloc_committed;  /* responder: TX swapped; COMMITTED sent */
    bool alloc_commit_pending; /* responder: peer DONE, swap in w_fec_tx */
    bool alloc_ack_resend;     /* responder: repeat the last ACK */
    bool alloc_committed_resend; /* responder: repeat COMMITTED */
    bool alloc_abort_pending; /* responder: ABORT received */
    bool alloc_fail;       /* this flow's exchange failed */
};

struct tbt_session;

struct tbt_fec_rx
{
    /* This receive plane belongs to one stream; sid is the routing key and
     * group_map the local-to-physical group mapping that matches the sender's
     * TX plane for the same flow. */
    u64 sid;
    u8 group_mask[TBT_FEC_BITMAP_BYTES];
    u16 group_map[TBT_FEC_MAX_GROUPS];
    struct tbt_fec_flow *flow;
    bool closing;
    spinlock_t lock; /* carrier rx works assemble blocks concurrently */
    u16 n, m, s;
    u16 groups;
    u16 slots; /* block slots per group */
    u64 next_super;   /* next super-block to deliver */
    u64 next_deliver; /* global packets delivered */
    /* Stream byte offset of the next delivered byte. Packets are not all S
     * bytes long any more: the packetizer flushes a partial packet when the
     * application goes quiet, so the delivered stream is the concatenation of
     * the shares in sequence order and the offset cannot be `seq * s`. */
    u64 deliver_bytes;
    /* The FEC plane carries the session's packet sequence, not a stream's: a
     * DATA record has no sid. `stream_base` is the session byte at which the
     * current stream (stream_sid) began, so the next connection in the same
     * session starts at stream byte 0 again. */
    u64 stream_sid;
    u64 stream_base;
    /* A reset dropped the old stream's cursors and the new stream's first FEC
     * record has not arrived: the delivery walk and NACK scan wait, and that
     * record rebases the window. Block ids come from the session's packet
     * sequence, which the old stream pushed ahead, so the old cursor names no
     * block the new stream will send and must never be inherited. */
    bool rebase_pending;
    /* The stream whose blocks the reorder tree holds, and the block the
     * window was last rebased to (0 while the flow holds the tree, whose base
     * is packet 0). A v7 flow starts at base 0, so these only describe the
     * current flow's own rebase; they never carry another stream's cursor. */
    u64 rebase_sid;
    u64 min_block_seen;
    u64 ack_deliver;  /* last frontier sent in an ACK */
    u64 total_pkts;   /* stream packet count; valid when total_known */
    u64 total_bytes;  /* FIN's byte offset; 0 when only LAST named the end */
    bool total_known;
    /* w_fec_rx owns decode and ordered delivery; carriers only place shares.
     * rx->lock is a leaf: it covers the window check, slot placement, counts
     * and decode snapshots, never allocation, decode, delivery or another
     * lock. */
    bool work_pending;      /* a block may be decodable or deliverable */
    bool stopping;          /* session teardown: w_fec_rx returns */
    bool deliver_partial;   /* resume the walk at (deliver_p, deliver_g) */
    u16 deliver_p;          /* packet index inside next_super */
    u16 deliver_g;          /* group index inside that packet */
    /* A super-block that cannot become ready is permanent on this wire: no
     * NACK and no retransmission exists, so the shares that would complete it
     * never arrive. progress_super/progress_ns time that state; a stream that
     * stays there past TBT_FEC_STALL_MS is killed so the application sees an
     * error instead of a silent stall. */
    u64 progress_super;
    u64 progress_ns;
    u8 *rec;                /* m * s: recovered-slot scratch */
    u8 *zero;               /* s zero bytes: padding and absent parity */
    u8 dpresent_snap[TBT_FEC_MAX_SHARES];
    u8 ppresent_snap[TBT_FEC_MAX_SHARES];
    u64 stream_pkt_base;    /* absolute seq of the bound stream's first packet */
    /* A FIN for the next stream can arrive before its OPEN binds it; keep one
     * slot and consume it on adoption instead of overwriting the current
     * stream's total. */
    u64 next_fin_sid;
    u64 next_fin_end;
    u64 next_fin_base;
    u64 next_fin_bytes;
    bool next_fin_valid;
    struct tbt_fec_blk *blk; /* groups * slots */
    struct tbt_fec_grouped *codec;
    u8 *data_ptrs[TBT_FEC_MAX_SHARES];   /* decode scratch */
    u8 *parity_ptrs[TBT_FEC_MAX_SHARES]; /* decode scratch */
    /* Highest super-block ever placed; the NACK window is
     * [next_super, highest_block], clipped to replay_k. */
    u64 highest_block;
    /* Effective replay K for this shape: the sender ring, the NACK window
     * and the credit cap all use the same value. 0 means no ring. */
    u16 replay_k;
    /* NACK pacing and retry state, allocated with the rx arena. */
    struct tbt_fec_nack_state *nack;
    /* Last time each share was named in a NACK, indexed like blk:
     * (group * slots + block_id % slots) * (n + m) + share. A share is not
     * requested again until one round trip could have answered it. */
    u64 *req_ns;
    /* The block id each req_ns slice belongs to. A slot is reused by a later
     * block; without this stamp the old timestamps would suppress requests
     * for shares the new block never asked for. */
    u64 *req_block;
    u64 probe_last_ns;   /* last frontier shape probe */
    /* The frontier block the probes target and how many unanswered probes it
     * has had. A block the sender never built answers nothing; after
     * TBT_FEC_NACK_PROBE_MAX the scan stops asking and leaves it to the
     * stall timer (fix22: an idle uplink frontier was probed 274k times). */
    u64 probe_block;
    u8 probe_tries;
    /* First time the frontier super-block was seen incomplete when no group
     * of it had started (a whole-block loss): the first NACK waits one round
     * trip from here, like a started block's first_gap_ns. */
    u64 gap_block;
    u64 gap_ns;
    /* First NACK that named the block being repaired, kept across bitmap
     * changes (shares arriving shrink the request): the repair latency is
     * measured from here, not from the last differing NACK. */
    u64 repair_block;
    u64 repair_first_ns;
    u64 rtt_sample_ns;   /* last carrier RTT sample */
    u32 rtt_us;          /* largest carrier RTT: the replay path's bound */
    u32 nack_cadence_ms; /* adaptive retry interval, 20..500 ms */
    /* Adaptive shape (0.3.0). The shape fields are read and written under
     * rx->lock: a carrier places a share while w_fec_rx may apply a switch. */
    u64 base_pkt;
    u64 base_block;
    u32 epoch; /* committed configuration generation */
    struct tbt_fec_shape shape;
    struct tbt_fec_shape prev_shape;
    u64 prev_base_pkt;
    u64 prev_base_block;
    struct tbt_fec_grouped *old_codec; /* kept until the new shape commits */
    struct tbt_fec_grouped *pending_codec; /* PREP allocated, not applied */
    u8 switch_state;                   /* STEADY/PREP/ARMED/APPLIED */
    u64 switch_pkt;
    u64 switch_block;
    u64 switch_start_ns;
    struct tbt_fec_cfg pending_cfg;
    bool cfg_pending;
    /* The peer's initial shape arrived: applied by w_fec_rx, which may
     * allocate. The carrier path only records it. */
    struct tbt_fec_cfg init_cfg;
    bool init_pending;
    bool commit_pending;   /* first new-shape record: free old_codec */
    bool rollback_pending; /* old-shape record after APPLIED: undo */
    u32 prev_epoch;
    /* Estimator (tbt_fec_adapt.h), owned by w_fec_rx. The SC-LDPC
     * estimator is allocated for mode 3 and uses the 256-bit miss bitmap. */
    struct tbt_fec_est *est;
    struct tbt_fec_adapt_sclc_est *est_sclc;
    u8 alive_bm[TBT_FEC_BITMAP_BYTES];
    atomic64_t abandoned_shares; /* counted by blk_reset, folded by w_fec_rx */
    u64 est_abandoned_last;
    bool est_paused;
    u64 pause_start_ns;
    u64 fbk_last_ns;
    u8 fbk_flags; /* flags owed on the next report */
    u32 fbk_epoch; /* epoch to report with a handshake flag, 0 = current */
    /* The stream's first shape, for FIN alignment across a switch. */
    u64 stream_span;
    u64 stream_base_pkt;
    /* Pending RX side of an FEC_ALLOC switch. The target is applied by
     * w_fec_rx once next_deliver reaches pkt, because the old mask must
     * place every record below that point first. The old plane is kept
     * until the first new-mask record commits the switch, so an ABORT can
     * restore it without reallocating. */
    struct tbt_fec_alloc_flow alloc;
    u8 alloc_state; /* 0 none, 1 prep, 2 armed, 3 applied */
    bool alloc_commit_pending; /* first new-mask record: free old plane */
    bool alloc_abort_pending;  /* ABORT: restore old plane, reply */
    bool alloc_done_resend;    /* responder: repeat DONE */
    bool alloc_done_sent;      /* responder: DONE sent for this epoch */
    u32 alloc_epoch;           /* last applied generation */
    u32 alloc_abort_epoch;     /* tombstone: an ABORT cancelled this one */
    struct tbt_fec_grouped *alloc_old_codec;
    struct tbt_fec_blk *alloc_old_blk;
    u64 *alloc_old_req_ns;
    u64 *alloc_old_req_block;
    u8 alloc_old_mask[TBT_FEC_BITMAP_BYTES];
    u16 alloc_old_map[TBT_FEC_MAX_GROUPS];
    u16 alloc_old_groups;
    u64 alloc_old_base_pkt;
    u64 alloc_old_base_block;
};

/* One live FEC stream in a session. The session owns a hash of these keyed by
 * sid; each one owns a TX and an RX plane whose groups are the physical
 * groups named by group_mask. The session's arena, replay budget, carrier
 * queues and work items are shared, but every cursor, credit and FIN state
 * below is per flow. A flow lives from OPEN (near: stream start) to detach;
 * late records find no flow and are dropped. */
struct tbt_fec_flow
{
    struct hlist_node node;
    struct tbt_session *sess;
    refcount_t refs;
    u64 sid;
    u8 group_mask[TBT_FEC_BITMAP_BYTES];
    u16 group_map[TBT_FEC_MAX_GROUPS];
    u16 groups;
    /* 0 = free slot, 1 = active, 2 = draining, 3 = dead. Only the control
     * path writes it; carrier RX reads it to refuse a dying flow. */
    u8 state;
    struct tbt_fec_tx *tx;
    struct tbt_fec_rx *rx;
    /* Final reference may be dropped by a carrier RX lookup, where freeing a
     * vmalloc arena is illegal. The last put schedules this work. */
    struct work_struct free_work;
};

/* One queued FEC record waiting for a carrier. Control records may be sent on
 * any up carrier; data records are bound to their share's carrier. */
struct tbt_fec_out
{
    struct list_head node;
    refcount_t refs; /* queue reference plus every in-flight reader */
    bool queued;     /* still linked; unlink is once-only */
    u16 carrier;
    u16 len;
    u8 control;
    u8 replay; /* queued from the replay ring: charged to the byte budget */
    /* Replay only: the super-block this record belongs to. The flush frees
     * it without a write once the peer's delivered frontier passes that
     * block, so an acked repair never sits ahead of the tail. */
    u64 block_id;
    /* Stream id from the record header; the flush uses it only for stats. */
    u64 sid;
    /* The flow this record belongs to, holding one reference for the record's
     * lifetime. Every per-stream record has one; a session-wide record has
     * NULL. It keeps the planes alive while the record is in flight, even if
     * the flow detaches and its table reference goes away. */
    struct tbt_fec_flow *flow;
    /* Bound data records charge this flow's FIN gate. Set for data records
     * only; equal to flow->tx. */
    struct tbt_fec_tx *owner;
    /* NACK receipt time for a replay record; the flush charges NACK->wire
     * latency when the carrier accepts it. Zero for every other record. */
    u64 stamp_ns;
    u8 data[];
};

/* One carrier's bound share queue. The flush walks every carrier and takes
 * this queue's head; -EAGAIN leaves that head here and moves on, so a full
 * socket never stops the other carriers. Control and replay records do not
 * use these queues: an ACK, a NACK, or a repair must not wait behind a
 * congested carrier's data. */
struct tbt_fec_outq
{
    struct list_head queue;
    u32 depth;
    u32 bytes;
};

/* One carrier may hold at most this many records / bytes. This is the hard
 * cap, the last line of defence: the packetizer stops building new DATA
 * before any queue reaches the high-water mark, so a healthy session never
 * gets here. The cap stays above the high-water mark by more than one FEC
 * block's parity burst (m <= 128), because a block closes by queueing all of
 * its parity at once. */
#define TBT_FEC_OUTQ_DEPTH_MAX 384u
#define TBT_FEC_OUTQ_BYTES_MAX (TBT_FEC_OUTQ_DEPTH_MAX * TBT_FEC_REC_MAX)

/* High-water mark of one carrier's bound queue. The packetizer checks this
 * before building the next DATA packet and pauses until the carrier drains,
 * which pushes the pressure into the capture ring and then into the
 * application. Dropping the record instead is what made a 1 GiB run pay a
 * 7.8 s NACK repair for 22 shares: the replay ring still had them, but only
 * after the receiver noticed the gap. */
#define TBT_FEC_OUTQ_HIGH_DEPTH 160u
#define TBT_FEC_OUTQ_HIGH_BYTES (TBT_FEC_OUTQ_HIGH_DEPTH * TBT_FEC_REC_MAX)

/* Session-wide bound on bound data records. 256 carriers times the per-
 * carrier cap would be 1.3 GiB, so the sum has its own record and byte
 * ceiling. 32 MiB is 7.6 s of queue at the 4.215 MB/s app target: enough to
 * ride out a burst of slow carriers, bounded regardless of carrier count. */
#define TBT_FEC_OUTQ_QUEUE_HIGH 2048u
#define TBT_FEC_OUTQ_QUEUE_BYTES_HIGH (16u * 1024 * 1024)
/* The hard session ceiling stays one FEC block (n+m <= 256 records) above
 * the mark: the packetizer checks the mark before a block, and the block
 * then queues its n+m records without touching the ceiling. */
#define TBT_FEC_OUTQ_QUEUE_MAX (TBT_FEC_OUTQ_QUEUE_HIGH + TBT_FEC_MAX_SHARES)
#define TBT_FEC_OUTQ_QUEUE_BYTES_MAX (32u * 1024 * 1024)

/* One NACK may name at most this many shares; the receiver retries the rest.
 * This bounds the work one NACK can create, not the storage it needs. */
#define TBT_FEC_NACK_REQ_MAX 256u

/* Replay records in flight across every carrier queue: a byte budget, not a
 * record count. One NACK names at most 64 shares (512 KiB), so 8 MiB holds
 * sixteen bursts; a slow carrier must not consume it, and the records
 * spread over the carriers by their queue bytes. A miss is retried by the
 * receiver's NACK, not turned into a source-frame drop. fix23 measured a
 * 4 MiB cap with a one-record high-water throttle: the repair drain fell
 * behind, flush skips rose 116x and the ordered walk's HOL wait 1278x. The
 * bound is 8 MiB again; the high-water throttle keeps four records per
 * pass, not one. */
#define TBT_FEC_REPLAY_BYTES_MAX (8u * 1024 * 1024)
/* Replay records one session flush pass may write before the packetizer gets
 * a turn. 8 records of one 8 KiB share is 64 KiB: replay keeps its priority,
 * but a NACK burst cannot hold the close path's FIN behind it for the whole
 * 2 MiB burst. The TX work is re-armed immediately while records remain. */
#define TBT_FEC_REPLAY_PASS_MAX 8u
/* NACK replies admitted per record while the replay queue is above its
 * high-water mark. The receiver retries the rest. fix42 queued 1348 shares
 * (11 MB) in one initial burst, filled the 8 MiB ring, and made the sender
 * refuse eight further replies; a burst must queue at a rate the drain can
 * match, not at the speed the NACKs arrive. */
#define TBT_FEC_NACK_REQ_THROTTLED 4u

/* The eight session work items whose reference is owned by the bit in
 * tbt_session.sched (review/s2-merged-design.md §1.3/§5.1). The order is the
 * destroy cancel order in §5.2; do not renumber without updating it. */
enum tbt_sess_work
{
    TBT_SW_POLL = 0,
    TBT_SW_REPLACE,
    TBT_SW_PENDING,
    TBT_SW_REAP,
    TBT_SW_FBK,
    TBT_SW_FEC_TX,
    TBT_SW_FEC_RX,
    TBT_SW_TX,
};

/* Adaptive FEC control-surface snapshot (0.3.0). The estimator/controller in
 * the FEC data plane owns every field; c13 only reads it. `sent`/`miss` are
 * the current epoch's alive-filtered DATA shares and repairs, so an epoch
 * change starts a fresh window. `state` and `reason` stay numeric here; the
 * stats line maps them to the names below. */
enum tbt_fec_adapt_state
{
    TBT_FEC_ADAPT_STEADY = 0,
    TBT_FEC_ADAPT_CFG_SENT,
    TBT_FEC_ADAPT_DRAIN,
    TBT_FEC_ADAPT_SWAP,
    TBT_FEC_ADAPT_ABORT,
    TBT_FEC_ADAPT_COOLDOWN,
};

enum tbt_fec_adapt_ctl_reason
{
    TBT_FEC_ADAPT_CTL_R_NONE = 0,
    TBT_FEC_ADAPT_CTL_R_OFF,       /* adaptive=off: the configured shape is fixed */
    TBT_FEC_ADAPT_CTL_R_IDLE,      /* application idle: p frozen */
    TBT_FEC_ADAPT_CTL_R_CARRIER,   /* fewer than 90% of carriers alive */
    TBT_FEC_ADAPT_CTL_R_ABANDONED, /* abandoned/sent above 1% */
    TBT_FEC_ADAPT_CTL_R_SAMPLE,    /* too few shares or blocks in the window */
    TBT_FEC_ADAPT_CTL_R_SWITCHING, /* a switch is in flight */
    TBT_FEC_ADAPT_CTL_R_HOLD,      /* rate/hold freeze after a jitter episode */
    TBT_FEC_ADAPT_CTL_R_ERROR,     /* protocol or shape errors: freeze */
};

struct tbt_fec_adapt_ctl
{
    u32 epoch;
    u16 n, m, groups; /* shape in force */
    u8 class;         /* control class 0..5, derived from n/m */
    u8 state;         /* enum tbt_fec_adapt_state */
    u8 reason;        /* enum tbt_fec_adapt_reason */
    u32 p_ppm;        /* max(p_ewma, p_win): the value the law used */
    u32 p_win_ppm;    /* RLNC 1 s window, SC-LDPC 4 s window */
    u32 r_fail_ppm;   /* residual: NACK-named shares / DATA packets */
    u64 sent, miss;   /* alive-filtered DATA shares and repairs, this epoch */
    u32 switches, aborts, rollbacks;
    u64 sample_unknown, abandoned; /* blocks skipped / dropped unfinished */
    u64 pauses, pause_ns;          /* entries into a frozen state, time spent */
};

struct tbt_session
{
    struct list_head node;    /* tbt_tunnel.sessions */
    struct tbt_tunnel *tunnel; /* back-pointer; the session never outlives it */
    refcount_t refs;
    /* 0.2.0 lifetime: refs reaches zero exactly once, and only then is
     * &drained completed. tbt_session_destroy() waits on it and remains the
     * only freer; a put never frees (review/s2-merged-design.md §5.1). */
    struct completion drained;
    /* One bit per TBT_SW_* item, changed with atomic bit ops. Bit 1 means one
     * reference is reserved for that item: a queued instance owns it until its
     * entry returns it, or the arm that could not queue returns it itself. */
    unsigned long sched;
    struct net *net;
    /* Lock order (review/s2-merged-design.md §2): tbt_lock -> pump_lock ->
     * {->lock -> tx_lock, fec_out_lock, c->http_lock}; socket lock -> ->lock.
     * ->lock is a leaf below pump_lock. A path may take at most one of
     * pump_lock, socket lock and ->lock as its outer lock. */
    spinlock_t lock;          /* streams hash, car[], prov/dead_release, sched */
    struct mutex pump_lock;   /* serialises carrier writes and carrier swap;
                               * owns c->sk on every write path */
    struct tbt_sk_ops def_ops; /* copy of tunnel->def_ops; restore source */
    struct hlist_head streams[TBT_STREAM_HT];

    struct tbt_carrier *car[TBT_MAX_CARRIERS_LIMIT]; /* indexed by path */
    u16 n_car;   /* cfg.n + cfg.m, 1..256: every dialled carrier */
    u16 n_pri;   /* cfg.n: carriers 0..n_pri-1 carry DATA by default */
    bool fec;    /* v6 data plane: FEC records, one share per carrier */
    /* ChaCha8 body-only obfuscation (T0 §2.3/§2.4). Each direction has its
     * own 8-byte nonce; the near side owns the session key unless
     * crypto_key= is configured on both ends. tx/rx contexts never mix. */
    bool crypto;
    bool crypto_have_key; /* crypto_key[] is valid */
    /* crypto_key[] came from the peer's HELLO, not from this side. The peer
     * therefore keeps announcing it with key_id=HELLO on every carrier, and
     * every later HELLO must repeat the same key (T0 §2.3). A key this side
     * generated or configured is the opposite: the peer must use key_id 0. */
    bool crypto_key_peer;
    u8 crypto_key_id;     /* TBT_CRYPTO_KEY_ID_NONE or _HELLO in our HELLO */
    u8 crypto_key[TBT_CRYPTO_KEY_LEN];
    u8 crypto_tx_nonce[TBT_CRYPTO_NONCE_LEN];
    u8 crypto_rx_nonce[TBT_CRYPTO_NONCE_LEN];
    struct tbt_crypto_pair crypto_ctx;
    bool crypto_tx_ready; /* published after tx context init */
    bool crypto_ready;    /* rx nonce learned; both contexts initialized */
    /* FEC flows, keyed by sid. `fec_tx`/`fec_rx` name the primary flow (the
     * first live one) for the old single-stream readers; every data-plane
     * path looks its flow up by sid and never assumes these are the only
     * plane. */
    struct hlist_head fec_flows[TBT_STREAM_HT];
    u16 n_fec_flows;
    struct tbt_fec_flow *fec_primary;
    struct tbt_fec_tx *fec_tx;
    struct tbt_fec_rx *fec_rx;
    /* Session-level work serialisation: the flow table is shared, so the
     * packetizer and the delivery walk must have one owner each even when
     * the primary flow changes. Bits are TBT_FEC_RUN_BIT/AGAIN_BIT. */
    atomic_t fec_tx_run;
    atomic_t fec_rx_run;
    /* Session geometry: total physical groups, the groups already owned by a
     * flow, the per-group RX slots and the replay K shared by every flow. */
    u16 fec_groups_total;
    u16 fec_groups_used;
    u16 fec_slots;
    u16 fec_replay_k;
    /* Session shape, copied into every flow's planes. */
    u16 fec_n, fec_m, fec_w, fec_k;
    u8 fec_d;
    bool fec_sclc;
    struct tbt_fec_sclc_shape fec_sclc_shape;
    /* Serialises flow admission: the group check and the plane allocation
     * must not interleave between two carriers' OPEN handlers. */
    struct mutex fec_alloc_lock;
    /* Dynamic group rebalance (FEC_ALLOC). The near side is the only
     * allocator. An admission round is driven from w_fec_tx while the
     * caller sleeps on fec_rebalance_lock; the async growth after a detach
     * uses the same round machinery without a waiter. Round fields are
     * mutated under sess->lock; a per-flow switch lives in its TX/RX
     * planes. */
    struct mutex fec_rebalance_lock;
    bool fec_allocator;      /* near side: owns the group split */
    bool fec_alloc_admit;    /* a dynamic admission owns the free pool */
    bool fec_alloc_round;    /* a round is in flight */
    bool fec_alloc_req;      /* an admission is waiting for a round */
    bool fec_alloc_failed;   /* the last round could not complete */
    bool fec_alloc_grow;     /* a flow left: grow the survivors */
    u8 fec_alloc_grow_tries; /* bounded retries of a failed grow round */
    u32 fec_alloc_epoch;     /* next allocation generation */
    u16 fec_alloc_flows;     /* flows expected to finish this round */
    u16 fec_alloc_finished;  /* flows that reached DONE/FAILED */
    u64 fec_alloc_deadline_ns;
    u64 fec_alloc_pending_sid; /* admission waiting for a result */
    struct tbt_fec_alloc_flow fec_alloc_pending; /* reserved new-flow mask */
    bool fec_alloc_pending_valid;
    bool fec_alloc_has_pending; /* this round also reserves a new flow */
    /* Round-robin cursor for the TX/RX work bodies. */
    u16 fec_rr;
    /* Control-record rotation and teardown flag: session resources, not per
     * flow. */
    u16 fec_ctl_rot;
    bool fec_stopping;
    /* One delayed work drains every ready carrier's bound data queue. A
     * carrier is set in fec_drain_ready when the packetizer or write_space
     * wakes it; the work takes one batch from each ready carrier in a
     * round-robin cursor, so a full socket cannot hold the others behind
     * it. The cursor and the bitmap are owned by the work. */
    struct delayed_work fec_drain_work;
    DECLARE_BITMAP(fec_drain_ready, TBT_MAX_CARRIERS_LIMIT);
    u16 fec_drain_cursor;
    u16 fec_ndrain;
    struct delayed_work w_fec_tx; /* packetizer + control/replay writer */
    /* 0.2.0: FEC receive work (decode outside rx->lock, then deliver).
     * Declared only in this cut: M4/M8 initialise it, and nothing arms it
     * yet. See review/s2-merged-design.md §1.3 and §2.3. */
    struct delayed_work w_fec_rx;
    struct list_head fec_out_ctl;    /* ACK/NACK: any up, non-stalled carrier */
    struct list_head fec_out_replay; /* NACK repairs: any up carrier, byte-budgeted */
    struct tbt_fec_outq *fec_outq; /* n_car queues, indexed by the share's carrier */
    u32 fec_out_queued;          /* records on every queue, control included */
    u32 fec_outq_queued;         /* bound data records across carrier queues */
    u32 fec_outq_bytes;          /* bound data bytes across carrier queues */
    u32 fec_outq_depth_max;      /* high-water per-carrier depth */
    u32 fec_out_ctl_depth;       /* control records queued */
    u32 fec_out_ctl_bytes;       /* control bytes queued */
    u32 fec_replay_bytes;        /* replay bytes queued across carriers */
    /* Last repair activity: a NACK was received or sent, or a replay record
     * was queued. tbt_tx.c reads this to hold off the TX stall kill while a
     * repair is in progress; only this file writes it. */
    u64 fec_repair_last_ns;
    spinlock_t fec_out_lock;
    /* Debug-only FEC share loss, indexed by the share's scheduled carrier.
     * It lives on the session, not the carrier object, so the injector keeps
     * dropping that share after its carrier dies and is replaced. */
    u32 fec_drop_ppm[TBT_MAX_CARRIERS_LIMIT];
    /* Adaptive FEC control surface (0.3.0). `adaptive` is the runtime switch
     * copied from the tunnel config and toggled by `session <id>
     * adaptive=on|off`; the snapshot is written by the FEC data plane and
     * read by the c13 stats line. */
    bool adaptive;
    struct tbt_fec_adapt_ctl adapt;
    bool closing; /* session teardown started: no new joins, no new work */
    struct list_head prov; /* accepted carriers waiting for HELLO */
    /* Join-replaced carriers whose socket release is deferred out of the
     * carrier's socket lock. w_replace drains this in bounded batches. */
    struct list_head dead_release;
    atomic_t n_prov;       /* carriers on `prov`, bounded by tbt_prov_cap() */
    u32 prov_peak;         /* high-water mark of n_prov, /proc only */

    u8 nonce[16];      /* the session key carried in HELLO */
    bool nonce_set;    /* far side: adopted from the first HELLO */
    bool nonce_pinned; /* `session <id> nonce=` set it explicitly */
    /* Far side: the only near source allowed to join carriers.  Set from a
     * configured `peer=` at session creation, else fixed by the first HELLO
     * that passes the nonce check.  A session property: it survives carrier
     * death and replacement. */
    __be32 anchor_addr;
    bool anchor_set;

    struct delayed_work w_replace; /* dials replacements for dead carriers;
                                    * delayed so a bounded pass can defer the
                                    * rest without spinning */
    /* Dial pacing for every connect: the first wave (the poll's retry of a
     * never-connected carrier), replacements and failed-connect retries all
     * spend these tokens, so neither a session start nor losing every carrier
     * can turn into a connect storm. tbt_carriers_start() seeds the bucket;
     * tbt_dial_take() is the only writer under dial_lock. */
    u32 dial_tokens;
    u64 dial_refill_ns;
    spinlock_t dial_lock;  /* tbt_dial_take(): w_replace, the poll, start */
    struct delayed_work w_fbk;    /* feedback tick and retransmit timer */
    struct delayed_work w_tx;     /* one ARQ/control pump wake for the session */
    struct delayed_work w_reap;   /* P1: tombstone sweeper */
    struct delayed_work w_pending; /* P1: starts one queued public socket */
    struct delayed_work w_poll;   /* 50 ms liveness net: every carrier slot and
                                   * every provisional carrier, one pass */
    struct list_head pending;     /* P1: accepted sockets waiting for the gate */
    u16 n_pending;
    u16 n_streams;    /* hash entries, including tombstones */
    u16 n_active;     /* non-detached streams, the concurrency bound */

    u64 far_sid;      /* the session's single stream id (40 bits) */
    u64 next_sid;
    u32 sid_prefix;   /* 16 random bits, DESIGN.md §3.6 */

    /* HTTP container capture for /proc/net/tbtun/http (debug >= 2). The near
     * side's carrier 0 only. Headers and a framing prefix only, never a full
     * body. Snapshot, never cleared; each length is published after its
     * bytes, so a reader that sees the length has the bytes it covers.
     *
     * http_cap_req holds the request block the near side emitted; it is the
     * only field /proc renders today. http_cap_resp has no writer yet: nothing
     * fills it, so its length stays 0. http_cap_chunk holds the first emitted
     * chunk's size line and record prefix; it is captured, but /proc does not
     * output it. Both fields stay until a writer or a reader is decided. */
    char http_cap_req[TBT_HTTP_REQ_MAX];   /* request block incl. CRLFCRLF */
    u16 http_cap_req_len;
    char http_cap_resp[TBT_HTTP_HDR_MAX];  /* reserved: no writer yet */
    u16 http_cap_resp_len;
    char http_cap_chunk[TBT_HTTP_CAP_CHUNK]; /* size line + record prefix;
                                              * captured, not rendered */
    u16 http_cap_chunk_len;

    /* Per-direction ack state. tx_ack is the peer's delivered offset of our
     * TX direction; rx_ack is our delivered offset, reported in FBK and in
     * every outgoing frame's uack field. */
    u64 tx_ack;
    u64 rx_ack;
    bool peer_paused; /* the peer reports a full delivery queue */
    u64 last_ack_ns;      /* mirror: last stream ack, proc/control only */

    /* Auto sender window (DESIGN.md §5.4). tx_window is the effective byte
     * budget, recomputed by the pump from the carriers' own BBR rate samples
     * (rate_delivered/rate_interval_us and srtt_us), not from a second
     * congestion controller. rtt_max_us is the largest current carrier RTT.
     * txq_max is the frame-queue bound derived from the window, so a larger
     * window cannot be throttled by the old 64 KiB queue. */
    u32 tx_window;
    u32 txq_max;
    u32 bdp_bytes;    /* aggregate BDP B = sum(rate_i x (srtt_i + fb)) */
    u32 k_x100;       /* window multiplier k = RTO / mean_srtt, x100 */
    u64 last_loss_ns; /* last retransmit queued: hold the window for 1 s */
    u32 rtt_mean_us;  /* delivery-weighted mean SRTT: the window's base */
    u32 rttvar_us;    /* largest carrier mdev_us: the window's jitter term */
    u32 rtt_max_us;   /* largest carrier SRTT, a distribution fact */
    /* BDP estimator coverage: how many live carriers contributed and how much
     * rate/window the aggregate actually saw. */
    u64 bdp_sum_rate; /* aggregate delivery rate used for BDP (bytes/s) */
    u64 bdp_agg_wnd;  /* sum of live carriers' snd_wnd (bytes) */
    u32 bdp_n_live;   /* carriers with a socket */
    u32 bdp_coverage; /* carriers that contributed a rate sample */

    /* Scheduler: a striping window over the carriers, rebuilt on the lowest
     * inflight and at most every 10 ms. Only the pump touches it. DATA uses
     * the stream's copy; ctrl_sched serves FBK and streamless control. */
    u32 carrier_gen;  /* bumped for every carrier object; invalidates stale
                       * pinned frames when a slot is replaced */
    /* Lost-carrier bits, indexed by carrier: 256 slots, so a plain
     * `unsigned long` would take a `set_bit()` for carrier 64..255 32 bytes
     * past itself and overwrite join_retries/ctrl_sched (and, on a debug
     * kernel, show up as an invalid _Bool load of a neighbouring flag). */
    DECLARE_BITMAP(path_lost_mask, TBT_MAX_CARRIERS_LIMIT);
    u8 join_retries;  /* consecutive join_dup retries, capped before failing */
    struct tbt_sched ctrl_sched;

    /* Statistics (DESIGN.md §8.7). A counter with more than one writer
     * context (a carrier's rx work, a stream work, sk_write_space softirq,
     * the pump, /proc) is atomic64_t and is only touched through
     * atomic64_inc/add and tbt_stat_read(). A counter with a single writer
     * stays u64 and names that writer, so a second writer is a visible
     * change instead of a silent lost update. */
    atomic64_t stats_gap; /* holes detected (TBT_ST_GAP / reorder overflow);
                           * written by every carrier's rx work */
    u64 stats_tx;         /* DATA bytes handed to a carrier; pump only */
    /* Far-side liveness counters (DESIGN §8.5). They exist so "the far side
     * saw nothing" can be told apart from "the far side never ran". Carrier
     * work and socket callbacks write them, so they are atomic. */
    atomic64_t rx_work_runs, rx_drain_entries, rx_drain_bytes;
    atomic64_t rx_paused_skips, rx_parse_proto, rx_records;
    u64 rx_adopt_events;  /* listen work only */
    atomic64_t rx_ready_calls, rx_work_early, rx_reads, rx_read_bytes;
    atomic64_t rx_resume_calls, rx_read_zero;
    atomic64_t rx_resume_busy;
    u32 rx_last_drained;  /* last value; carrier rx work */
    atomic64_t rx_errs;
    s32 rx_last_err;      /* last value; carrier rx work */
    u64 prov_reject;      /* accepts refused by the provisional cap; listen */
    atomic64_t carrier_write_space_runs;
    atomic64_t poll_rescues;
    /* Diagnostic only: 50 ms poll passes since session creation.
     * tbt_session_poll_work() is the only writer (a single delayed work), and
     * the per-carrier poll reads it to print the first passes of a carrier's
     * life. It gates logging, never behavior. */
    u32 diag_poll_round;
    /* Each carrier's poll work writes this for itself; several carriers run
     * concurrently, so it is atomic like the rx counters above. */
    atomic64_t stall_paused_skips;
    atomic64_t stall_idle_skips; /* poll: no unacked bytes, clock reset */
    atomic64_t stall_declared;   /* poll: a stall paused a FEC carrier */
    atomic64_t stall_recovered;  /* poll: snd_una moved again after a pause */
    u64 stall_drop;              /* frames dropped by tbt_tx_path_stalled();
                                  * that helper has no caller today (ARQ kills,
                                  * FEC has no retain), so this stays 0 until
                                  * ARQ adopts pause semantics */
    atomic64_t stall_dead;       /* poll: a paused carrier outlived its bound
                                  * and was declared lost after all */
    atomic64_t stall_replaced;   /* poll: a long stall replaced its carrier */
    /* Economic stall decision (poll). The discrete RTO grid estimates the
     * recovery survival function online; risk[i] counts stalls that were
     * still running at grid[i], events[i] those that recovered in
     * (grid[i-1], grid[i]], censored[i] those that were replaced or died
     * there. Both outcomes enter the estimate; using only recoveries would
     * bias it toward optimism. */
    u32 stall_grid_risk[TBT_STALL_GRID];
    u32 stall_grid_events[TBT_STALL_GRID];
    u32 stall_grid_censored[TBT_STALL_GRID];
    u64 stall_recover_samples;   /* poll: finished stalls observed */
    u64 stall_recover_ns_ewma;   /* poll: EWMA of observed recovery times */
    u64 stall_wait_loss_bytes;   /* poll: L_wait of the last decision */
    u64 replace_cost_bytes;      /* poll: C_r of the last decision */
    u64 replace_cost_ewma;       /* poll: EWMA of C_r */
    u64 last_stall_replace_ns;   /* poll: last replacement, for the T_r wait */
    u64 last_carrier_add_ns;     /* poll: last growth, paced like a dial */
    /* Measured send rate: DATA bytes the pump handed to carriers since the
     * last sample. The window/RTT estimate overstates a receive-heavy session
     * (the far side sends little but has a large window), so the measured
     * rate wins once two samples exist. All three are poll-only. */
    u64 rate_sample_ns;
    u64 rate_sample_bytes;
    u64 rate_bps_ewma;
    u32 stall_survival_bucket;   /* poll: grid bucket of the last decision */
    u8 stall_replace_reason;     /* poll: enum tbt_stall_replace_reason */
    atomic64_t stall_replace_econ;    /* poll: replaced, economics said so */
    atomic64_t stall_replace_retrans; /* poll: replaced, retrans exhausted */
    atomic64_t stall_replace_dead;    /* poll: replaced, stall_dead backstop */
    atomic64_t up_rescued;       /* poll: a connected carrier whose HELLO was
                                  * written but whose OPEN never queued was
                                  * re-announced (up 0 -> 1) */
    /* Connect latency samples for the replacement cost. Every dialling
     * carrier's work writes them, so both halves are atomic. */
    atomic64_t connect_ns_sum, connect_ns_count;
    atomic64_t carrier_add;      /* poll: a carrier grew the stripe; 0 until
                                  * the far side can extend n_car */
    atomic64_t carrier_add_fail; /* poll: a growth attempt found no room; 0 */
    atomic64_t backpressure_enter; /* poll: no carrier can carry any more */
    bool backpressure;           /* poll: edge state for the counter above */
    /* DATA admissions deferred by the peer window; tbt_tx_pump() only, and
     * the pump is serialised by sess->pump_lock, so a plain u64 is safe. */
    u64 win_block, win_resume;
    atomic64_t tx_wake_calls, tx_wake_busy, tx_work_runs;
    /* TX scheduling when no carrier can take a frame (tbt_tx.c). The pump is
     * the only writer of the two plain fields; the capture work reads
     * tx_wait_since_ns to decide when the wait became a user-visible
     * backpressure. tx_backpressure has several writers (pump and capture). */
    u64 tx_wait_carrier;         /* pump: pumps that found no usable carrier */
    u64 tx_wait_since_ns;        /* pump: start of the current carrier wait */
    u64 tx_wait_carrier_bytes;   /* pump: tx_queued of the last wait event */
    u64 tx_wait_carrier_ns;      /* pump: elapsed wait of the last event */
    atomic64_t tx_backpressure;  /* capture: entered user-side backpressure */
    atomic64_t tx_backpressure_bytes; /* capture: tx_queued at entry */
    atomic64_t tx_backpressure_ns;    /* capture: wait before backpressure */
    /* Reconciliation totals for carriers that left sess->car[] (a
     * replacement or a joining generation). The aggregates below count every
     * carrier that ever wrote; /proc/net/tbtun/peers lists only live slots,
     * so these totals close the difference on the `reconcile` line. */
    atomic64_t retired_tx_bytes, retired_data_frames, retired_retx_data;
    u64 carrier_bytes, carrier_err, carrier_short; /* pump (tbt_carrier_xmit) */
    atomic64_t carrier_ws_calls;
    atomic64_t carrier_nospace_armed; /* FEC/ARQ EAGAIN armed SOCK_NOSPACE */
    u64 pump_hw_armed, pump_hw_disarmed; /* pump only */
    atomic64_t pump_hw_fired;            /* per-carrier pump work */
    u64 dropped_by_ppm;                  /* pump only */
    u64 recv_partial_discard;            /* carrier release, serialised */
    /* C3 counters (DESIGN.md §8.7). */
    atomic64_t carrier_lost, carrier_replaced, carrier_retired;
    /* Stream resets, split by cause so the containment rule is measurable:
     * stream_reset_tcp_failed may only grow when every carrier has failed,
     * stream_reset_user when the application closes. */
    atomic64_t stream_reset_tcp_failed, stream_reset_user;
    atomic64_t carrier_alloc_fail, connect_fail;
    /* A dial that outlived one TBT_CONNECT_TIMEOUT_MS wait. Non-zero means the
     * path's SYN round trip (or a SYN retransmission) is longer than one
     * window; it is the difference between "the peer is down" and "the dial
     * needed a second wait", and it is what a -EINPROGRESS return used to hide
     * behind connect_fail. */
    atomic64_t connect_retry;
    atomic64_t port_exhaust, port_alloc_fail; /* tbt_port_alloc() reasons */
    u64 spare_activated; /* idle spares promoted to DATA; w_replace only */
    atomic64_t profile_mismatch; /* HELLO carried a different profile */
    /* FEC v6 receive failures (T0 §1.3): a share the receiver could not trust.
     * The three are distinct on purpose. A proto/header-CRC failure means the
     * framing itself is untrustworthy, so the carrier closes; a payload CRC
     * failure means the framing is sound and only the body is damaged, so the
     * share is dropped and the carrier stays. With crypto=on a wrong key looks
     * exactly like a damaged body, and this counter is the only thing that
     * separates "the key is wrong" from "the peer is silent". */
    atomic64_t fec_proto_fail;      /* header unpack/field check failed */
    atomic64_t fec_hdr_crc_fail;    /* 48-byte header CRC mismatch */
    atomic64_t fec_payload_crc_fail; /* body CRC mismatch: share dropped */
    /* A partial packet the packetizer flushed because the application went
     * quiet, and a block the receiver had to refuse because it held one of
     * those and a recovered share whose length is therefore unknown. */
    atomic64_t fec_short_tx;
    atomic64_t fec_short_unknown;
    /* Parity shares a short SC-LDPC block did not send: the block sends
     * row 0 alone as its cut announcement, and the NACK replay repairs the
     * few real shares. Counted in shares, so a 128-row block saves 127. */
    atomic64_t fec_short_parity_saved;
    /* Shares the receiver dropped before they reached a block: outside the
     * block window, or a DATA/PARITY share that arrived before the session key
     * was installed. Both leave the block to its parity, and a recovered share
     * has no payload_len. */
    /* A public connection refused because the FEC data plane already carries
     * a live stream: one stream per session is all the FEC record can name. */
    atomic64_t fec_stream_refused;
    /* Far side: a DATA/FIN/RST dropped because no stream existed yet. A FEC
     * share sent before its stream's OPEN is consumed by the sender and can
     * never be NACKed; this counter proves that path. */
    atomic64_t bind_drop_no_stream;
    atomic64_t fec_drop_window;
    atomic64_t fec_drop_crypto;
    /* FEC flow-control observability: ACK records emitted and applied, and
     * packetizer runs that found the peer's window closed. */
    atomic64_t fec_ack_tx;
    atomic64_t fec_ack_rx;
    atomic64_t fec_credit_wait;
    /* Per-carrier sender scheduling: a flush pass that skipped a carrier on
     * -EAGAIN, the records it wrote, and the records left behind a skipped
     * head. fec_outq_drop names shares refused because their own carrier's
     * queue reached TBT_FEC_OUTQ_DEPTH_MAX/BYTES_MAX; the replay ring still
     * holds them, so a NACK repairs the loss. */
    atomic64_t fec_flush_skip;
    atomic64_t fec_flush_pass;
    atomic64_t fec_hol_wait;
    atomic64_t fec_outq_drop;
    /* Packetizer pauses spent waiting for a bound data queue to fall below
     * its high-water mark. Non-zero proves the backpressure path ran; a
     * healthy session with no slow carrier keeps it at 0. */
    atomic64_t fec_outq_high_wait;
    /* A parity encode refused by the codec: the block can then be repaired
     * only by NACKing its shares, never by its own parity. */
    atomic64_t fec_encode_fail;
    /* Peer initial shapes installed / refused. A refused INIT leaves the
     * local receive shape in force; the first DATA then shows the mismatch. */
    atomic64_t fec_init_rx;
    atomic64_t fec_init_bad;
    /* A block decode that could not finish the block: counted so the cost of
     * a lossy block is visible, not repeated silently. */
    atomic64_t fec_decode_backoff;
    atomic64_t fec_replay_skip; /* replay head found no carrier with room */
    /* NACK/replay repair, reset causes, and the carrier wait that precedes
     * backpressure. The reset counters prove the FEC layer only tears a
     * stream down when every carrier TCP is gone (or the user/peer closed
     * it); a live TCP always keeps the stream waiting and repairing. */
    atomic64_t fec_nack_tx;          /* NACK records queued */
    atomic64_t fec_nack_rx;          /* valid NACK records received */
    atomic64_t fec_nack_retx;        /* shares queued from the replay ring */
    atomic64_t fec_nack_drop;        /* NACK send failed: retry next pass */
    atomic64_t fec_nack_enomem;      /* ... -ENOMEM from the out queue */
    atomic64_t fec_nack_einval;      /* ... pack/seal rejected the record */
    atomic64_t fec_nack_enospc;      /* ... queue bound reached */
    atomic64_t fec_nack_eother;      /* ... any other error */
    atomic64_t fec_nack_bad;         /* NACK bitmap/field outside the shape */
    atomic64_t fec_nack_retx_bytes;  /* replay bytes queued */
    atomic64_t fec_replace_retx_bytes; /* replay bytes after a replacement */
    atomic64_t fec_replay_hit;       /* requested share found in the ring */
    atomic64_t fec_replay_miss;      /* requested share not served */
    atomic64_t fec_replay_miss_no_ring; /* no local ring / shape over budget */
    atomic64_t fec_replay_miss_alloc;   /* kmalloc failed */
    atomic64_t fec_replay_miss_enospc;  /* replay byte budget or queue cap */
    atomic64_t fec_replay_miss_get;     /* total of the two causes below */
    atomic64_t fec_replay_miss_evicted; /* block no longer in the ring */
    atomic64_t fec_replay_miss_never_stored; /* block resident, share absent */
    atomic64_t fec_nack_sparse_probe;   /* unstarted group: probe parity 0 only */
    atomic64_t fec_nack_empty_reply;    /* empty marker sent for an absent share */
    atomic64_t fec_nack_parity_reply;   /* parity/cut sent for a short block */
    atomic64_t fec_replay_drop;         /* queued replay freed without a write */
    /* NACK filtering and replay deduplication. A request is dropped when the
     * share already arrived, when an earlier request is still inside its
     * round trip, or when the block is still young enough for the carrier
     * TCP and the parity to repair it. Each drop is one fewer replay record
     * on the sender's 8 MiB queue. */
    atomic64_t fec_nack_filtered_ready;    /* block became ready/delivered */
    atomic64_t fec_nack_filtered_inflight; /* share asked within its hold */
    atomic64_t fec_nack_filtered_dup;      /* share already present */
    atomic64_t fec_nack_filtered_young;    /* block gap younger than one RTT */
    atomic64_t fec_replay_dedup;           /* share reply already in flight */
    atomic64_t fec_nack_req_shares;        /* shares named in received NACKs */
    atomic64_t fec_nack_backpressure;      /* replay queue full: NACK stopped */
    atomic64_t fec_replay_stale;           /* queued replay freed: block delivered */
    atomic64_t fec_fin_wait_replay;        /* FIN released while replay queued */
    /* Absent-share handling: a NACK named a share the sender never stored.
     * fec_nack_never_stored counts NACK records with at least one such
     * share; fec_nack_absent_reply counts the explicit answers (parity row
     * or empty marker); fec_nack_req_invalid counts shares that can never
     * exist (padding in a resident block). */
    atomic64_t fec_nack_never_stored;
    atomic64_t fec_nack_absent_reply;
    atomic64_t fec_nack_req_invalid;
    /* A received NACK named a DATA share at or past the block's cut: padding
     * the sender never wrote. The cut row answered it and no replay miss was
     * counted. A 0.3.1 receiver never sends these; a non-zero value names a
     * peer whose NACK scan still names padding. */
    atomic64_t fec_nack_pad_skip;
    /* Replay high-water: episodes where the queue crossed 75% of its cap,
     * and flush passes that were held to one replay record so new DATA got
     * a turn. */
    atomic64_t fec_replay_high_water;
    atomic64_t fec_replay_pass_throttled;
    /* Replay drain: bytes/s over the last sampling window (an EWMA gauge),
     * the high-water of queued replay bytes, and the window accumulator. */
    atomic64_t fec_replay_drain_bps;
    atomic64_t fec_replay_queue_high;
    u64 fec_replay_drain_bytes; /* written since the last sample; flush only */
    u64 fec_replay_drain_ns;    /* last sample time; flush only */
    bool fec_replay_over_high;  /* queue above 75% at the last flush pass */
    /* The one stream is closing: the replay admission cap drops to
     * TBT_FEC_REPLAY_CLOSE_BYTES_MAX so the tail repair fits the close
     * budget. Written by the FEC TX work, read by the NACK handler. */
    bool fec_closing;
    atomic64_t fec_reset_tcp_failed; /* FEC reset: no carrier TCP alive */
    atomic64_t fec_reset_user;       /* user/peer/application teardown */
    /* A FEC-owned TBT_ST_HOLE kill (ordered-walk hole or an unrecoverable
     * super-block). fec_reset_user deliberately excludes TBT_ST_HOLE, so
     * without this counter those resets were invisible. tbt_tx.c's
     * TXSTALLKILL is also TBT_ST_HOLE; C increments this there. */
    atomic64_t fec_reset_hole;
    atomic64_t fec_wait_carrier;     /* waits for a usable carrier */
    atomic64_t fec_backpressure;     /* last resort: source ring full */
    atomic64_t fec_nack_lat_ns;      /* NACK receipt -> carrier accepted */
    atomic64_t fec_nack_lat_max_ns;
    atomic64_t fec_nack_repair_ns;   /* first NACK -> block repaired */
    atomic64_t fec_nack_repair_max_ns;
    u32 fec_replay_queued; /* replay records on the session replay queue */
    /* TX stall checks held off while a FEC repair is in progress. C owns the
     * tbt_tx.c side; this field is exported here so the count is visible. */
    atomic64_t tx_stall_suppressed_fec;
    /* 0.2.0 FEC data plane: decode, delivery, placement and wire-shape
     * failures. Each names one way a share or a stream can be lost, so a
     * stalled stream is diagnosed from /proc instead of guesswork. */
    atomic64_t fec_decode_blocks;     /* blocks handed to the codec */
    atomic64_t fec_decode_recovered;  /* shares recovered from parity */
    atomic64_t fec_decode_nomem;      /* codec workspace allocation failed */
    atomic64_t fec_deliver_nomem;     /* delivery allocation failed: retry */
    atomic64_t fec_deliver_dup;       /* share already in the stream: realign */
    atomic64_t fec_deliver_ooo;       /* ordered walk found a hole: stop */
    atomic64_t fec_deliver_bad;       /* delivery offset overflow */
    atomic64_t fec_dup_share;         /* share for a slot already present */
    atomic64_t fec_late_share;        /* share for a block already ready */
    atomic64_t fec_drop_sid;          /* share from a non-current stream */
    atomic64_t fec_late_data;         /* late DATA whose slot is still absent */
    atomic64_t fec_pad_share;         /* empty-block marker received */
    atomic64_t fec_dup_marker;        /* replayed marker for a settled block */
    atomic64_t fec_marker_conflict;   /* marker and real shares disagree */
    atomic64_t fec_epoch_conflict;    /* PARITY shape disagrees with block */
    /* Adaptive switch health. A record whose shape matches no epoch, and a
     * NACK for a block below the current base, are dropped instead of
     * killing the carrier; these counters name them. */
    atomic64_t fec_epoch_bad;
    atomic64_t fec_nack_stale_epoch;
    atomic64_t fec_switch_begin;
    atomic64_t fec_switch_done;
    atomic64_t fec_switch_abort;
    atomic64_t fec_switch_rollback;
    /* Dynamic group rebalance health: rounds started/finished/aborted,
     * REQUEST retransmissions, a peer answer that named an unknown epoch,
     * and a new mask that overlapped a live flow. */
    atomic64_t fec_alloc_begin;
    atomic64_t fec_alloc_done;
    atomic64_t fec_alloc_abort;
    atomic64_t fec_alloc_retry;
    atomic64_t fec_alloc_bad;
    atomic64_t fec_alloc_overlap;
    /* A share sits in a slot the block's stated cut says holds no stream
     * byte: the sender's win_len and its own DATA records disagree. Skipping
     * the slot would delete its payload while the ACK ran past it, so the
     * block is refused and this counter names the reason. */
    atomic64_t fec_slot_past_cut;
    atomic64_t fec_fin_bad;           /* FIN pkt_end/pkt_base invalid */
    atomic64_t fec_ack_bad;           /* ACK frontier/credit clamped */
    atomic64_t fec_tx_share_gap;      /* sender share sequence skipped */
    atomic64_t fec_empty_block;       /* markers emitted for empty groups */
    atomic64_t http_parse_err;   /* container framing/header protocol error */
    atomic64_t http_status_err;  /* non-200 or non-chunked/non-keepalive */
    u64 http_req, http_resp; /* header blocks emitted; pump only */
    atomic64_t pad_tx, drip_tx;  /* keepalive/drip chunks emitted */
    atomic64_t dup_bytes;       /* duplicate payload bytes created, minus
                                 * copies the pump dropped unwritten */
    u64 dup_tx;          /* duplicate copies written; pump only */
    atomic64_t dup_used;        /* duplicate copies that filled a gap first */
    atomic64_t dup_suppressed;  /* duplicates skipped by max_inflate */
    u64 dup_same_path;   /* duplicates forced onto an already-used path */
    atomic64_t dbg_retx_evident; /* hole at the ack was evident */
    atomic64_t dbg_retx_cand;    /* retransmit candidates queued */
    atomic64_t dbg_retx_nosends; /* skipped: never written */
    /* Why a retransmit copy did not reach the wire. A copy that is created
     * but never written is the failure that let the receiver's hole budget
     * reset a stream with the bytes still in our own queue; each branch of
     * the pump and of the carrier's all-or-nothing pre-check owns one
     * counter, so /proc names the blocking gate. */
    u64 dbg_retx_txqdrop;  /* dropped by the txq byte bound */
    u64 dbg_retx_nopick;   /* no carrier after the avoid/stalled fallback */
    u64 dbg_retx_stalled;  /* ... and the stalled-path mask was non-empty */
    u64 dbg_retx_notup;    /* carrier not up */
    u64 dbg_retx_notann;   /* stream not announced on that carrier */
    u64 dbg_retx_batch;    /* entered the write batch */
    u64 dbg_retx_eagain;   /* carrier returned -EAGAIN */
    u64 dbg_retx_memshort; /* pre-check: sndbuf has no room for the record */
    u64 dbg_retx_wndshort; /* pre-check: peer window short (copy still queued) */
    u64 holes_declared;  /* geometric test declared a lost frame; tx_lock */
    u64 holes_repaired;  /* a declared hole was filled; tx_lock */
    atomic64_t holes_unrecoverable; /* case B: the stream was reset */
    atomic64_t close_timeouts;   /* a FIN wait exceeded its bound: reset */
    atomic64_t fin_starved;      /* FIN copies requeued under the close timer */
    atomic64_t fin_written;      /* FIN copies that reached the wire */
    atomic64_t close_hold_fec;   /* close timer held for FEC repair/credit */
    atomic64_t close_detaches;   /* FIN written, peer FIN absent: detached */
    atomic64_t deliver_timeouts; /* peer FIN seen, delivery made no progress */
    atomic64_t dbg_deliver_checks; /* FBK: peer FIN seen, rxq non-empty */
    atomic64_t hole_checks;      /* diagnostic: hole test had a candidate */
    atomic64_t hole_gate_fail;   /* diagnostic: consistency gate refused */
    atomic64_t hole_depth_fail;  /* diagnostic: depth below k_max_hole */
    atomic64_t hole_time_fail;   /* diagnostic: younger than 2 x RTT */
    /* Tunnel-wide token bucket (DESIGN.md §5.3); rate_bps 0 = unlimited. */
    u64 tokens;
    u64 last_refill_ns;
    u64 pause_cleared_ns; /* loss timers restart when backpressure ends */
    u64 last_uack_sum;
    u64 session_join_dup; /* tbt_session_join(), under sess->lock */
    atomic64_t session_join_bad, join_nonce_reject;
    atomic64_t join_src_reject; /* far: HELLO from a source other than the anchor */
    u64 fbk_tx;             /* w_fbk work only */
    atomic64_t fbk_rx;
    u64 retx_frames, retx_bytes; /* pump only */
    u64 retx_hole, retx_slow, retx_pathloss, retx_other; /* by source; pump */
    u64 data_tx;         /* DATA records written, copies included; pump */
    u64 inject_skip;     /* hole fillers refused by the queue bound; tx_lock */
    atomic64_t retx_queued; /* pump and hole check (tx_lock) */
    u32 fb_delay_us;     /* EWMA: app-ack lag after the carrier TCP ack */
    u32 rto_us;          /* last RTO the retransmit timer used */
    u32 una_stall_us;    /* oldest snd_una progress at the last retx pass */
    u64 retain_drop;     /* pump only */
    u64 sack_tx;         /* w_fbk work only */
    atomic64_t sack_rx;
    atomic64_t sack_unknown;
    atomic64_t rx_reorder_overflow;
    /* Sum of every stream's out-of-order bytes, so the reorder budget is a
     * session bound and not 128 x rx_reorder_bytes. Written under each
     * stream's rx_lock; different streams' rx work runs concurrently, so the
     * total is atomic. */
    atomic64_t rx_ooo_total;
    atomic64_t rx_data_frames;
    atomic64_t rx_delivered_bytes;
    u64 sched_sorts;     /* pump only */
    u64 path_switches;   /* reserved; no writer today */
    atomic64_t pub_capture_runs, pub_actor_calls;
    atomic64_t tx_submit_calls, tx_submit_bytes;
    atomic64_t dbg_alloc_fail, dbg_over_qmax, dbg_copy_fail, dbg_empty_len;
    u64 dbg_announce_mismatch; /* pump only */
    u64 dbg_pinned_bad_path;   /* pump only: pinned frame with no live path */
    bool dbg_path_selftest_done; /* pump only: sentinel lookup checked once */
    u64 dbg_snap_overflow;     /* stream snapshot, under sess->lock */
    u64 dbg_pump_hiwat, dbg_pump_dead, dbg_pump_moved; /* pump only */
    atomic64_t dbg_pump_runs, dbg_pump_nocar; /* counted before pump_lock */
    u64 dbg_pump_down, dbg_pump_stop;          /* reserved; no writer */
    u64 dbg_pump_empty, dbg_pump_qlen;         /* reserved; no writer */
    /* Review 22: pinned control-frame lifetime. ctl_retry counts failed
     * attempts, ctl_requeue counts frames put back on tx_q (including a
     * generation re-pin), ctl_lost counts frames abandoned after the retry
     * bound, and ctl_obsolete counts HELLO/OPEN/RETIRE copies dropped because
     * their carrier generation is gone. */
    atomic64_t ctl_lost;  /* pump and the RST retry path */
    u64 ctl_retry, ctl_requeue, ctl_obsolete; /* pump only */
    u32 pub_last_rcv_nxt, pub_last_copied, pub_last_after;
    u32 pub_last_qlen;
    /* Live snapshot, read by /proc/net/tbtun/stats. Diagnostic only. */
    u32 snap_rx_queued, snap_tx_queued;
    u32 snap_pub_wmem, snap_pub_snd_una, snap_pub_snd_nxt;
    u32 snap_car_wmem, snap_car_rcv_nxt, snap_car_copied, snap_car_rcvbuf;
    u32 snap_car_sndbuf;
    u8 snap_pub_state, snap_rx_paused, snap_tx_paused;
    u8 snap_car_state, snap_car_stopping, snap_car_ud, snap_car_ready_ok;
    u8 snap_car_ws_ok;
    u8 snap_txw_pending, snap_txw_busy;

    /* Frames waiting for the carrier, strictly ordered. C2 has one stream, so
     * a single queue is ordered; C4 keys this by path instead. */
    spinlock_t tx_lock;
    struct list_head tx_q;
    u32 tx_queued;            /* payload bytes queued */
    u32 ctl_queued;           /* pinned control frames queued or in a batch */
};

#define TBT_STREAM_BUCKET(sid) \
    (((u32)(sid) ^ (u32)((sid) >> 32)) & (TBT_STREAM_HT - 1))

struct tbt_carrier
{
    struct list_head node;    /* sess->prov; self-linked otherwise */
    struct list_head release_node; /* sess->dead_release while a release is
                                    * pending; self-linked otherwise */
    struct socket *sock;      /* installer and tbt_carrier_close() only; never
                               * published to a read path */
    /* Published socket pointer (review/s2-merged-design.md §1.1/§1.2).
     * Write paths that own sess->pump_lock use rcu_dereference_protected()
     * once and then only the local `sk`; every other reader uses
     * tbt_carrier_sk_get()/tbt_carrier_sk_present(). Clearing it requires a
     * grace period before the socket is released. __rcu makes a bare
     * dereference a sparse error instead of a silent race. */
    struct sock __rcu *sk;
    struct tbt_session *sess; /* holds one session reference; see §5.1 */
    refcount_t refs;          /* 1 = container/installer; each reader adds
                               * one; the last put -> kfree_rcu */
    struct rcu_head rcu;      /* tbt_carrier_put() free path */
    struct work_struct w_connect; /* sock_create/bind/connect off the writer */
    struct work_struct w_rx;      /* drains carrier input -> tbt_rx */
    struct delayed_work w_pump;   /* correctness wake while a carrier is full */
    /* Serialises every byte write on this carrier: the pump's ARQ/control
     * record and the FEC drain's batched frame. Replacing pump_lock with this
     * per-carrier lock is what lets 256 carrier drains run in parallel. */
    struct mutex tx_lock;
    atomic_t connected;           /* socket installed, callbacks live */
    atomic_t up;                  /* HELLO written; usable for scheduling */
    atomic_t stopping;
    atomic_t rx_again;            /* a wake was coalesced into a running rx */
    bool released;                /* tbt_carrier_close() has run */
    /* join() has installed this carrier in sess->car[] (review/
     * s2-lifecycle-design.md §3.2.2): joined => in car[] and not on prov.
     * Protected by sess->lock. adopt() must skip a joined carrier instead of
     * re-linking it onto prov. */
    bool joined;
    bool hello_sent;              /* our HELLO was written on this carrier */
    bool open_sent;               /* this carrier has written at least one OPEN */
    bool saw_hello;               /* the peer's first frame was a HELLO */
    bool peer_hello;              /* the peer's HELLO validated */
    bool provisional;             /* accepted, not yet joined to a slot */
    bool lost_counted;            /* carrier_lost counted once */
    unsigned long stats_retired;  /* bit 0: counters moved to the session's
                                   * retired totals (one-shot) */
    bool win_paused;              /* DATA deferred at the peer's window */
    /* snd_una has not moved for k RTOs: this path is not delivering right now,
     * but it is not dead. Only the FEC container pauses a carrier; ARQ kills
     * and repaths it instead. The socket, the slot and the generation all stay,
     * and the pump stops giving it new frames. The shares already in flight are
     * the FEC data plane's loss to handle (parity rebuild); this layer drops
     * nothing. Cleared when snd_una moves again; stall_dead kills the carrier
     * if it never does. Only the 50 ms poll writes it, the pump and /proc read
     * it, so WRITE_ONCE/READ_ONCE is enough: one writer, no atomic. */
    bool stalled;
    bool spare;                   /* index >= n_pri: idle unless activated */
    bool active;                  /* spare that now carries DATA */
    u32 drop_ppm;                 /* debug-only loss injection, 0..1e6 */
    u32 drop_snd_nxt;             /* snd_nxt when the injector dropped */
    /* Per-carrier counters. The pump is the only writer of the TX side
     * (tbt_carrier_xmit() runs under sess->pump_lock), and this carrier's rx
     * work is the only writer of the RX side; both are u64 on purpose. The
     * cross-context readers use READ_ONCE(). A counter that gains a second
     * writer moves to atomic64_t, like the session counters in §8.7. */
    u64 dropped_by_ppm;           /* pump */
    u64 uack;                     /* peer's per-path high-water, this path */
    u64 uack_ns;                  /* when that value was last published */
    u64 data_frames;              /* DATA records written here; pump */
    u64 retx_data;                /* DATA copies written here; pump */
    s32 loss_ppm;                 /* confirmed record loss / records acked */
    u64 payload_bytes_sent;       /* DATA payload bytes written here; pump */
    u32 rtt_us;                   /* EWMA RTT; a gauge, not a counter: FBK
                                   * entries for this path are applied by
                                   * whichever rx work handles the frame, and
                                   * a lost EWMA sample is harmless */
    u32 rtt_min_us;               /* minimum observed RTT; gauge */
    u32 inflight;                 /* snapshot for peers */
    u64 tx_frames;                /* records written here; pump */
    u64 tx_bytes;                 /* bytes written here; pump */
    u32 recv_cnt;                 /* peer's frame count on this path; rx work
                                   * publishes the whole value */
    u64 recv_lag;                 /* confirmed record loss: sent below the
                                   * peer's high-water minus its recv_cnt */
    u64 last_una;                 /* last snd_una observed */
    u32 last_nxt;                 /* last snd_nxt observed (wire progress) */
    u64 last_una_ns;              /* carrier stall clock: start of the current
                                   * unacked, no-progress interval */
    u64 stall_since_ns;           /* when the current stall was declared; the
                                   * stall_dead bound counts from here */
    u32 stall_packets_out;        /* packets_out at that moment; the bound
                                   * needs a wire that never moved */
    u64 last_una_move_ns;         /* when snd_una really moved: progress */
    u64 up_rescue_ns;             /* poll: first sighting of connected +
                                   * hello_sent + up==0; the rescue waits one
                                   * interval so a normal HELLO->OPEN window
                                   * is never touched */
    u64 last_rx_ns;
    u64 last_tx_ns;
    u64 join_deadline_ns;         /* provisional carriers only */
    /* hold_enable: last good cwnd/pacing captured while the carrier is idle.
     * The 50 ms poll restores them until real data, loss or the deadline. */
    u32 hold_cwnd;
    unsigned long hold_pacing;
    u64 hold_until_ns;
    u64 hold_since_ns;
    u64 hold_last_loss_ns;
    bool hold_active;
    u64 retry_at_ns;              /* next connect attempt */
    u64 rx_frames;                /* DATA frames received here; this rx work */
    u64 rx_bytes;                 /* bytes received here; this rx work */
    u8 rx_state;                  /* what our FBK reports for this path */
    u8 profile;                   /* effective container: 1 http-kal, 2 drip, 3 raw */
    u8 peer_gen;                  /* peer's generation for this path (HELLO) */
    u8 dbg_stall_dumped;          /* debug>=2: the stall dump ran for this gen */
    bool http_started;            /* our request/response header block is on the wire */
    struct mutex http_lock;       /* serialises the one-time header start */
    bool http_hdr_done;           /* peer header parsed; body stage */
    bool http_req_done;           /* far side: request header parsed (TX gate) */
    bool http_pending;            /* a decoded record awaits handling (-EAGAIN) */
    struct tbt_hdr http_pending_hdr;
    u16 http_pending_len;
    char *http_tx;                /* one encoded chunk (TBT_HTTP_FRAME_MAX) */
    char *http_hdr;               /* peer header block (TBT_HTTP_HDR_MAX) */
    char *fec_frame;              /* one FEC chunk frame (TBT_HTTP_FEC_FRAME_MAX) */
    u32 http_have;
    struct tbt_http_rx *http_rx;  /* RX record assembler (8 KiB) */
    u8 state;                     /* TBT_CAR_* */
    u8 class;                     /* profile class, for the peers view */
    u16 local_port;
    u8 *part;                     /* unwritten tail (TBT_CAR_PART_MAX) */
    u32 part_len, part_off;
    u8 index;
    u32 gen;                  /* session-unique; pinned frames carry it */
    struct tbt_parser *parser; /* this carrier's partial record (8 KiB) */
};

enum tbt_carrier_state
{
    TBT_CAR_DOWN = 0,
    TBT_CAR_CONNECTING,
    TBT_CAR_UP,
    TBT_CAR_SPARE, /* connected, HELLO'd, kept warm, no DATA */
    TBT_CAR_STALLED,
};

/* Why the poll replaced a stalled carrier (c3 stall_replace_reason). */
enum tbt_stall_replace_reason
{
    TBT_STALL_REPLACE_NONE = 0,
    TBT_STALL_REPLACE_ECONOMIC, /* waiting costs more than a new carrier */
    TBT_STALL_REPLACE_RETRANS,  /* TCP retransmissions are exhausted */
    TBT_STALL_REPLACE_DEAD,     /* stall_dead backstop */
};

/* tbt_sock.c */
void tbt_sock_kick(struct tbt_stream *st);
int tbt_sock_deliver(struct tbt_stream *st, const u8 *data, size_t len);
void tbt_stream_init_work(struct tbt_stream *st);
void tbt_sock_attach(struct tbt_stream *st);
void tbt_sock_detach(struct tbt_stream *st);
void tbt_stream_release_socket(struct tbt_stream *st);
void tbt_sock_shutdown_wr(struct tbt_stream *st);
void tbt_sk_ops_save(struct tbt_sk_ops *ops, struct sock *sk);
void tbt_sk_ops_restore(struct sock *sk, const struct tbt_sk_ops *ops);
void tbt_sock_abort_on_close(struct socket *sock);
void tbt_carrier_resume(struct tbt_session *sess);
void tbt_carrier_snapshot(struct tbt_session *sess);
int tbt_stream_start(struct tbt_session *sess, struct socket *sock);
void tbt_pending_work(struct work_struct *w);
void tbt_pending_drain(struct tbt_session *sess);

/* tbt_tx.c */
size_t tbt_tx_submit(struct tbt_stream *st, struct sk_buff *skb, unsigned int off, size_t len);
int tbt_tx_pump(struct tbt_session *sess);
void tbt_tx_wake(struct tbt_session *sess);
void tbt_tx_window_init(struct tbt_session *sess);
int tbt_tx_control_pinned(struct tbt_session *sess, struct tbt_stream *st,
                          u16 path, u8 type, u64 sid, u64 seq,
                          const u8 *body, u16 len, bool head, gfp_t gfp);
/* One OPEN per live carrier, so a carrier that dies before the first copy
 * still cannot strand the stream (DESIGN.md §3.6, C3). */
void tbt_tx_announce(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp);
/* FIN with the stream's next byte offset as its seq, one copy per live
 * carrier; the receiver applies it only when every earlier byte arrived.
 * Returns the number of copies actually enqueued: 0 means no carrier was
 * usable and the caller must retry, not treat the FIN as sent. */
int tbt_tx_fin(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp);
int tbt_tx_fbk(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp);
int tbt_tx_sack(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp);
int tbt_tx_rst(struct tbt_session *sess, struct tbt_stream *st, gfp_t gfp);
void tbt_fbk_work(struct work_struct *w);
void tbt_fbk_kick(struct tbt_session *sess, bool now);
void tbt_tx_ack(struct tbt_session *sess, struct tbt_carrier *c,
                u64 uack, u64 sid);
void tbt_tx_path_lost(struct tbt_session *sess, u8 index);
/* Drop a stalled path's unacknowledged ARQ frames without repathing them.
 * Reserved for an ARQ pause semantics: today ARQ kills and repaths a stalled
 * carrier (tbt_carrier_lost()), and FEC has no retain entries to drop, so
 * nothing calls this yet. */
void tbt_tx_path_stalled(struct tbt_session *sess, u8 index);
void tbt_tx_free_stream(struct tbt_stream *st);
void tbt_tx_free_queue(struct tbt_session *sess);
void tbt_tx_frame_free(struct tbt_frame *fr);
/* Account a frame the pump could not write. The caller holds tx_lock; only a
 * duplicate DATA copy changes the dup counters. */
void tbt_tx_frame_drop_account(struct tbt_session *sess, struct tbt_frame *fr);

/* tbt_rx.c */
int tbt_rx_parse_pending(struct tbt_carrier *c);
int tbt_rx_skb(struct tbt_carrier *c, struct sk_buff *skb, unsigned int offset,
               size_t len, size_t *consumed);
void tbt_rx_free_queue(struct tbt_stream *st);
int tbt_rx_flush(struct tbt_stream *st);
int tbt_target_attach(struct tbt_stream *st, struct socket *s);
int tbt_rx_hello(struct tbt_carrier *c, const struct tbt_hdr *h, const u8 *body);
void tbt_rx_check_hole(struct tbt_stream *st);
void tbt_rx_deliver_stall_check(struct tbt_stream *st);
u64 tbt_tx_wait_budget_ns(struct tbt_session *sess, struct tbt_stream *st);
void tbt_tx_hole_check(struct tbt_session *sess, struct tbt_stream *st);

/* tbt_carrier.c */
/* 0.2.0 carrier lifetime and work API (review/s2-merged-design.md §1.2).
 * Declarations only in this cut: M3/M5 implement them. Lock contract: get/put/
 * gen and sk_get/sk_put are RCU + refcount only, take no sess->lock and never
 * sleep; arm_* take sess->lock and are safe from softirq; quiesce may sleep
 * and must not be called with pump_lock or tx_lock held. */
struct tbt_carrier *tbt_carrier_get(struct tbt_session *sess, u8 index);
void tbt_carrier_put(struct tbt_carrier *c);
u32 tbt_carrier_gen(struct tbt_session *sess, u8 index);
struct sock *tbt_carrier_sk_get(struct tbt_carrier *c);
void tbt_carrier_sk_put(struct sock *sk);
/* Existence check only: no reference, no sleep. Safe under RCU, and safe for
 * the advisory checks in lost/action/connect work. */
static inline bool tbt_carrier_sk_present(struct tbt_carrier *c)
{
    return rcu_access_pointer(c->sk) != NULL;
}
bool tbt_carrier_arm_tx(struct tbt_carrier *c);
bool tbt_carrier_arm_pump(struct tbt_carrier *c, unsigned int ms);
bool tbt_carrier_arm_rx(struct tbt_carrier *c);
void tbt_carrier_quiesce(struct tbt_carrier *c);
int tbt_carriers_start(struct tbt_tunnel *t, struct tbt_session *sess);
void tbt_carriers_stop(struct tbt_session *sess);
struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess, u8 index);
u8 tbt_carrier_profile(const struct tbt_session *sess, u8 index, bool spare);
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c);
void tbt_carrier_stats_retire(struct tbt_session *sess, struct tbt_carrier *c);
void tbt_carrier_free(struct tbt_carrier *c);
void tbt_carrier_lost(struct tbt_session *sess, struct tbt_carrier *c);
/* Pause a live carrier whose snd_una has not moved: no RETIRE, no
 * replacement, no socket teardown, no path_lost. Its unacknowledged frames are
 * dropped and the pump stops picking it; the poll clears c->stalled when
 * snd_una moves again. */
void tbt_carrier_stall(struct tbt_session *sess, struct tbt_carrier *c);
int tbt_carrier_xmit(struct tbt_session *sess, struct tbt_carrier *c,
                     struct tbt_frame *fr);
int tbt_carrier_adopt(struct tbt_tunnel *t, struct socket *newsock);
void tbt_car_setsockbuf(struct socket *s, const struct tbt_cfg *cfg, u16 n_pri);
int tbt_carrier_open_target(struct tbt_stream *st);
void tbt_replace_work(struct work_struct *w);
int tbt_tx_hello(struct tbt_session *sess, u8 index, gfp_t gfp);
/* FEC v6 data plane (tbt_fec_data.c). */
int tbt_fec_data_init(struct tbt_session *sess);
void tbt_fec_data_free(struct tbt_session *sess);
/* Flow table. `admit` allocates a flow for sid with `want` physical groups
 * (0 = the fair share) and returns it with one reference, or an ERR_PTR:
 * -ENOSPC when no group is free, -EEXIST for a duplicate sid. `lookup`
 * returns a referenced flow or NULL; every caller must put it. */
struct tbt_fec_flow *tbt_fec_flow_admit(struct tbt_session *sess, u64 sid,
                                        u16 want);
/* Admit a flow with an exact bitmap (the far side's OPEN). Returns -EPROTO
 * for an invalid/overlapping/out-of-range mask and -ENOSPC when it names no
 * free group. */
struct tbt_fec_flow *tbt_fec_flow_admit_mask(struct tbt_session *sess,
                                             u64 sid,
                                             const u8 mask[TBT_FEC_BITMAP_BYTES]);
struct tbt_fec_flow *tbt_fec_flow_lookup(struct tbt_session *sess, u64 sid);
void tbt_fec_flow_put(struct tbt_fec_flow *f);
/* Bind a stream to its flow (one reference moves to the stream) and release
 * it at detach. */
void tbt_fec_flow_attach(struct tbt_stream *st, struct tbt_fec_flow *f);
void tbt_fec_flow_abort(struct tbt_session *sess, struct tbt_fec_flow *f);
void tbt_fec_flow_detach(struct tbt_stream *st);
/* Build the next free group bitmap for a new flow. Returns the group count,
 * or 0 when no group is free. */
u16 tbt_fec_flow_mask_alloc(struct tbt_session *sess, u16 want,
                            u8 mask[TBT_FEC_BITMAP_BYTES]);
/* Whether any FEC flow can still be admitted (free groups exist). */
bool tbt_fec_flow_can_admit(struct tbt_session *sess);
size_t tbt_fec_tx_submit(struct tbt_stream *st, struct sk_buff *skb,
                         unsigned int off, size_t len);
int tbt_fec_tx_flush(struct tbt_stream *st);
bool tbt_fec_tx_idle(struct tbt_stream *st);
void tbt_fec_tx_work(struct work_struct *w);
/* Wake the per-carrier FEC data-queue drain. The packetizer calls it when a
 * bound queue turns non-empty; the carrier write-space path may call it too. */
void tbt_fec_out_kick(struct tbt_session *sess, u16 carrier);
/* Decode outside rx->lock, then deliver ready super-blocks in order. */
void tbt_fec_rx_work(struct work_struct *w);
int tbt_rx_fec_deliver(struct tbt_stream *st, u64 off, const u8 *data, u16 len);
int tbt_fec_rx_record(struct tbt_carrier *c, u8 *rec, size_t len);
/* A new stream takes over the session's FEC plane: drop the previous stream's
 * reorder blocks and delivery cursors before its bytes can reach the new
 * stream. The first FEC record after this call sets the new frontier. */
void tbt_fec_rx_new_stream(struct tbt_session *sess, u64 sid);
/* The old FIN record names the stream's total byte offset. The FEC receiver
 * uses it to complete a final block whose parity and LAST flag were lost. */
void tbt_fec_rx_fin(struct tbt_session *sess, struct tbt_stream *st,
                    u64 total_bytes, u64 pkt_end, u64 pkt_base);
int tbt_fec_record_len(const u8 *buf, size_t have, size_t *need);
/* tbt_carrier.c: write one whole record, no frame header. */
int tbt_carrier_send_raw(struct tbt_carrier *c, const void *data, size_t len);
/* Write one FEC record in the carrier's container (raw or HTTP chunk). */
int tbt_carrier_send_fec(struct tbt_carrier *c, const u8 *rec, size_t len);
/* Write up to TBT_FEC_BATCH_MAX FEC records with one HTTP header and one socket write.
 * Holds only c->tx_lock, not sess->pump_lock. Returns 0 and sets *sent=count
 * when the whole batch is committed to the carrier's ordered byte stream;
 * -EAGAIN leaves the batch for the caller to retry. */
int tbt_carrier_send_fec_batch(struct tbt_carrier *c,
                               const u8 *const *recs, const u16 *lens,
                               u16 count, u16 *sent);
/* Session-level 50 ms liveness poll (tbt_carrier.c), armed by tbt_session.c. */
void tbt_session_poll_work(struct work_struct *w);

void tbt_far_open_seen(struct tbt_stream *st);
void tbt_target_detach(struct tbt_stream *st);
void tbt_target_work_fn(struct work_struct *w);

/* tbt_stream.c helpers shared by tx/rx/session */
/* 0.2.0 stream work reference (review/s2-merged-design.md §1.4). schedule()
 * owns the 0->1 transition of `bit` and takes one reference; cancel() owns
 * 1->0. Both may run under sess->lock. Declarations only in this cut. */
bool tbt_stream_schedule(struct tbt_stream *st, struct work_struct *w,
                         unsigned long bit);
bool tbt_stream_cancel(struct tbt_stream *st, struct work_struct *w,
                       unsigned long bit);
struct tbt_stream *tbt_stream_get(struct tbt_session *sess, u64 sid, bool alloc, bool near);
int tbt_session_stream_snapshot(struct tbt_session *sess,
                                struct tbt_stream **out, int max);
enum tbt_bind_result
{
    TBT_BIND_OK = 0,
    TBT_BIND_ADOPT,
    TBT_BIND_DROP,
    TBT_BIND_BAD,
};
int tbt_session_bind_sid(struct tbt_session *sess, u64 sid, bool open,
                         const u8 *body, u16 len);
void tbt_stream_put(struct tbt_stream *st);
void tbt_stream_detach_from_session(struct tbt_stream *st);
void tbt_stream_maybe_detach(struct tbt_stream *st);
void tbt_stream_kill(struct tbt_stream *st, u8 reason);
void tbt_stream_kill_by_sid(struct tbt_session *sess, u64 sid, u8 reason);
/* 0.2.0 session lifetime (review/s2-merged-design.md §1.3/§5). tbt_session_get()
 * is the admission gate and takes sess->lock; tbt_session_put() completes
 * &drained on the last reference and never frees. tbt_session_arm()/cancel()
 * own the sched bit and its reference; arm's fast path may run in softirq and
 * under sess->lock. Declarations only in this cut. */
bool tbt_session_get(struct tbt_session *s);
void tbt_session_put(struct tbt_session *s);
bool tbt_session_arm(struct tbt_session *s, enum tbt_sess_work w,
                     unsigned long delay);
void tbt_session_cancel(struct tbt_session *s, enum tbt_sess_work w);
void tbt_session_destroy(struct tbt_tunnel *t);
int tbt_session_create(struct tbt_tunnel *t);
int tbt_session_join(struct tbt_session *sess, struct tbt_carrier *c, u8 index);
void tbt_session_retire(struct tbt_session *sess, u8 index, u8 gen);
bool tbt_carrier_dead(struct tbt_carrier *c);

/* tbt_procfs.c */
int tbt_procfs_init(void);
void tbt_procfs_exit(void);

#endif /* _TBTUN_H */
