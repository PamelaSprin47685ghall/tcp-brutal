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
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/refcount.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>

/* Wire constants, /proc names and the version live in the shared header. */
#include "tbt_uapi.h"

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
/* The port-block bitmap and TBT_PORTMAP_MAX are pure code, shared with the
 * host test (tbt_portmap.h). */
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
extern unsigned int max_tunnels;
extern unsigned int max_carriers;
extern unsigned int carrier_sockbuf_kib;
extern unsigned int carrier_buffer_budget_mib;

/* Port block for carrier sources (DESIGN.md §7.2). Owned by tbt_session.c. */
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
    TBT_PORT_ALLOC_EXHAUSTED, /* every port in the block is in use */
};

/* tbt_core.c */
bool tbt_debug_loss(void);

/* tbt_session.c */
extern struct tbt_portmap tbt_ports;
int tbt_portmap_set(u16 lo, u16 hi);
void tbt_portmap_get(u16 *lo, u16 *hi, bool *configured);
void tbt_portmap_show(struct seq_file *m);
u16 tbt_port_alloc(enum tbt_port_alloc_err *err);
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
#define TBT_PENDING_MAX 128
#define TBT_STREAMS_MAX 128
#define TBT_STREAM_SNAP_MAX 128
#define TBT_SQ_MAX (64 * 1024)      /* per-stream frame queue cap, bytes  */
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

/* Pinned control-frame lifetime (review 22). A control frame is never freed
 * on a transient write error: it is retried this many times, 2 ms apart, and
 * only then counted in ctl_lost. A dead carrier slot waits longer because a
 * replacement dial is already in flight. */
#define TBT_CTL_MAX_RETRIES 4
#define TBT_CTL_RETRY_MS 2
#define TBT_CTL_PATH_RETRY_MS 100
#define TBT_CTL_QUEUE_MAX 256

/* Retransmit timer: a retained frame whose last write is older than this and
 * that the peer has not delivered is re-sent on another path. The floor is
 * two feedback intervals, so ordinary ack latency never looks like loss, and
 * it must stay below the receiver's byte budget divided by the data rate or
 * the budget fires first (measured: probe D, 2% loss at 8 MB/s, 256 KiB
 * reorder budget, 40 ms floor: the near reset the stream one run in three). */
#define TBT_RETX_MIN_US 20000
#define TBT_RETX_MAX_US 500000 /* must cover 2x the feedback delay */

/* Replay ring: acked frames stay available for re-send for this long, or
 * until cfg.replay bytes are held, whichever comes first (DESIGN.md §9.6). */
#define TBT_REPLAY_MS 1000

/* The sender's window: how far it may run ahead of the peer's contiguous
 * received offset. It must cover the shaped path's BDP (40 Mbit x 50 ms
 * ~= 250 KiB), so it uses the whole reorder budget; the reorder tree cannot
 * exceed that budget without the overflow kill. */
#define TBT_TX_WINDOW_DIV 2

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
    u8 path;             /* pinned path, or 0xff until the pump picks one */
    u32 gen;             /* pinned frames: carrier generation they belong to */
    u8 retries;          /* transient control failures (review 22) */
    u64 retry_at_ns;     /* earliest next attempt; 0 = now */
    u16 avoid_mask;      /* paths this copy must not reuse */
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
    u8 path_gen[16];     /* generation of the carrier that wrote each bit */
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
    bool tx_fin_sent;         /* FIN wanted: set by the producer before the
                               * first tbt_tx_fin(); the FBK tick retries while
                               * tx_fin_queued is false */
    bool tx_fin_queued;       /* at least one FIN copy entered tx_q */
    bool tx_fin_written;      /* at least one FIN copy reached the wire */
    bool tx_fin_seq_set;      /* tx_fin_seq frozen at the first request */
    u64 tx_fin_seq;           /* byte offset every FIN copy names */
    unsigned long tx_fin_mask;     /* carriers that already have a FIN copy */
    unsigned long tx_fin_inflight; /* FIN copies queued, not yet written/dropped */
    bool tx_rst_written;      /* at least one RST copy reached the wire */
    unsigned long tx_rst_mask;     /* carriers that already have an RST copy */
    unsigned long tx_rst_inflight; /* RST copies queued, not yet written/dropped */
    bool tx_open_sent;        /* tbt_tx_announce() ran before capture attached */
    u16 announced_mask;       /* carriers whose OPEN for this stream was written */
    u8 announced_gen[16];     /* generation of each announced carrier */
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
 * One full record is TBT_REC_MAX; the buffer never needs more. */
#define TBT_PARSER_BUF TBT_REC_MAX
struct tbt_parser
{
    u8 buf[TBT_PARSER_BUF];
    size_t have;
};

struct tbt_session
{
    struct list_head node;    /* tbt_tunnel.sessions */
    struct tbt_tunnel *tunnel; /* back-pointer; the session never outlives it */
    refcount_t refs;
    struct net *net;
    spinlock_t lock;          /* streams hash, car[], prov/dead lists */
    struct mutex pump_lock;   /* serialises carrier writes and carrier swap */
    struct tbt_sk_ops def_ops; /* copy of tunnel->def_ops; restore source */
    struct hlist_head streams[TBT_STREAM_HT];

    struct tbt_carrier *car[TBT_MAX_CARRIERS_LIMIT]; /* indexed by path */
    u16 n_car;   /* cfg.n + cfg.m, 1..256: every dialled carrier */
    u16 n_pri;   /* cfg.n: carriers 0..n_pri-1 carry DATA by default */
    u32 buf_kib; /* carrier socket buffer KiB reserved from the module budget;
                  * returned in tbt_session_destroy() */
    bool closing; /* session teardown started: no new joins, no new work */
    struct list_head prov; /* accepted carriers waiting for HELLO */
    struct list_head dead; /* retired carriers, freed at session destroy */

    u8 nonce[16];      /* the session key carried in HELLO */
    bool nonce_set;    /* far side: adopted from the first HELLO */
    bool nonce_pinned; /* `session <id> nonce=` set it explicitly */
    /* Far side: the only near source allowed to join carriers.  Set from a
     * configured `peer=` at session creation, else fixed by the first HELLO
     * that passes the nonce check.  A session property: it survives carrier
     * death and replacement. */
    __be32 anchor_addr;
    bool anchor_set;

    struct work_struct w_replace; /* dials replacements for dead carriers */
    struct delayed_work w_fbk;    /* feedback tick and retransmit timer */
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

    /* Scheduler: a striping window over the carriers, rebuilt on the lowest
     * inflight and at most every 10 ms. Only the pump touches it. DATA uses
     * the stream's copy; ctrl_sched serves FBK and streamless control. */
    u32 carrier_gen;  /* bumped for every carrier object; invalidates stale
                       * pinned frames when a slot is replaced */
    unsigned long path_lost_mask; /* atomic lost-carrier bits, drained by pump */
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
    atomic64_t carrier_write_space_runs;
    atomic64_t poll_rescues;
    /* Each carrier's poll work writes this for itself; several carriers run
     * concurrently, so it is atomic like the rx counters above. */
    atomic64_t stall_paused_skips;
    /* DATA admissions deferred by the peer window; tbt_tx_pump() only, and
     * the pump is serialised by sess->pump_lock, so a plain u64 is safe. */
    u64 win_block, win_resume;
    atomic64_t tx_wake_calls, tx_wake_busy, tx_work_runs;
    /* Reconciliation totals for carriers that left sess->car[] (a
     * replacement or a joining generation). The aggregates below count every
     * carrier that ever wrote; /proc/net/tbtun/peers lists only live slots,
     * so these totals close the difference on the `reconcile` line. */
    atomic64_t retired_tx_bytes, retired_data_frames, retired_retx_data;
    u64 carrier_bytes, carrier_err, carrier_short; /* pump (tbt_carrier_xmit) */
    atomic64_t carrier_ws_calls;
    u64 pump_hw_armed, pump_hw_disarmed; /* pump only */
    atomic64_t pump_hw_fired;            /* per-carrier pump work */
    u64 dropped_by_ppm;                  /* pump only */
    u64 recv_partial_discard;            /* carrier release, serialised */
    /* C3 counters (DESIGN.md §8.7). */
    atomic64_t carrier_lost, carrier_replaced, carrier_alloc_fail, connect_fail;
    atomic64_t port_exhaust, port_alloc_fail; /* tbt_port_alloc() reasons */
    u64 spare_activated; /* idle spares promoted to DATA; w_replace only */
    atomic64_t profile_mismatch; /* HELLO carried a different profile */
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
    u64 holes_declared;  /* geometric test declared a lost frame; tx_lock */
    u64 holes_repaired;  /* a declared hole was filled; tx_lock */
    atomic64_t holes_unrecoverable; /* case B: the stream was reset */
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
    atomic64_t rx_data_frames;
    atomic64_t rx_delivered_bytes;
    u64 sched_sorts;     /* pump only */
    u64 path_switches;   /* reserved; no writer today */
    atomic64_t pub_capture_runs, pub_actor_calls;
    atomic64_t tx_submit_calls, tx_submit_bytes;
    atomic64_t dbg_alloc_fail, dbg_over_qmax, dbg_copy_fail, dbg_empty_len;
    u64 dbg_announce_mismatch; /* pump only */
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
    struct list_head node;    /* sess->prov or sess->dead */
    struct socket *sock;
    struct sock *sk;
    struct tbt_session *sess;
    struct work_struct w_connect; /* sock_create/bind/connect off the writer */
    struct work_struct w_rx;      /* drains carrier input -> tbt_rx */
    struct work_struct w_tx;      /* write-space wake -> tbt_tx_pump */
    struct delayed_work w_pump;   /* correctness wake while a carrier is full */
    atomic_t connected;           /* socket installed, callbacks live */
    atomic_t up;                  /* HELLO written; usable for scheduling */
    atomic_t stopping;
    bool hello_sent;              /* our HELLO was written on this carrier */
    bool open_sent;               /* this carrier has written at least one OPEN */
    bool saw_hello;               /* the peer's first frame was a HELLO */
    bool peer_hello;              /* the peer's HELLO validated */
    bool provisional;             /* accepted, not yet joined to a slot */
    bool lost_counted;            /* carrier_lost counted once */
    unsigned long stats_retired;  /* bit 0: counters moved to the session's
                                   * retired totals (one-shot) */
    bool win_paused;              /* DATA deferred at the peer's window */
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
    u64 last_una_ns;              /* when snd_una last moved (or blocked) */
    u64 last_una_move_ns;         /* when snd_una really moved: progress */
    u64 last_rx_ns;
    u64 last_tx_ns;
    u64 join_deadline_ns;         /* provisional carriers only */
    u64 retry_at_ns;              /* next connect attempt */
    u64 rx_frames;                /* DATA frames received here; this rx work */
    u64 rx_bytes;                 /* bytes received here; this rx work */
    u8 rx_state;                  /* what our FBK reports for this path */
    u8 profile;                   /* effective container: 1 http-kal, 2 drip, 3 raw */
    u8 peer_gen;                  /* peer's generation for this path (HELLO) */
    bool http_started;            /* our request/response header block is on the wire */
    bool http_hdr_done;           /* peer header parsed; body stage */
    bool http_req_done;           /* far side: request header parsed (TX gate) */
    bool http_pending;            /* a decoded record awaits handling (-EAGAIN) */
    struct tbt_hdr http_pending_hdr;
    u16 http_pending_len;
    char *http_tx;                /* one encoded chunk (TBT_HTTP_FRAME_MAX) */
    char *http_hdr;               /* peer header block (TBT_HTTP_HDR_MAX) */
    u32 http_have;
    struct tbt_http_rx *http_rx;  /* RX record assembler (8 KiB) */
    u8 state;                     /* TBT_CAR_* */
    u8 class;                     /* profile class, for the peers view */
    u16 local_port;
    u8 *part;                     /* unwritten tail (TBT_REC_MAX) */
    u16 part_len, part_off;
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
int tbt_tx_control_pinned(struct tbt_session *sess, struct tbt_stream *st,
                          u8 path, u8 type, u64 sid, u64 seq,
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
void tbt_tx_free_stream(struct tbt_stream *st);
void tbt_tx_free_queue(struct tbt_session *sess);
void tbt_tx_frame_free(struct tbt_frame *fr);

/* tbt_rx.c */
int tbt_rx_parse_pending(struct tbt_carrier *c);
int tbt_rx_skb(struct tbt_carrier *c, struct sk_buff *skb, unsigned int offset,
               size_t len, size_t *consumed);
void tbt_rx_free_queue(struct tbt_stream *st);
int tbt_rx_flush(struct tbt_stream *st);
int tbt_target_attach(struct tbt_stream *st, struct socket *s);
int tbt_rx_hello(struct tbt_carrier *c, const struct tbt_hdr *h, const u8 *body);
void tbt_rx_check_hole(struct tbt_stream *st);
void tbt_tx_hole_check(struct tbt_session *sess, struct tbt_stream *st);

/* tbt_carrier.c */
int tbt_carriers_start(struct tbt_tunnel *t, struct tbt_session *sess);
void tbt_carriers_stop(struct tbt_session *sess);
struct tbt_carrier *tbt_carrier_new(struct tbt_session *sess, u8 index);
u8 tbt_carrier_profile(const struct tbt_session *sess, u8 index, bool spare);
void tbt_carrier_retire(struct tbt_session *sess, struct tbt_carrier *c);
void tbt_carrier_stats_retire(struct tbt_session *sess, struct tbt_carrier *c);
void tbt_carrier_free(struct tbt_carrier *c);
void tbt_carrier_lost(struct tbt_session *sess, struct tbt_carrier *c);
int tbt_carrier_xmit(struct tbt_session *sess, struct tbt_carrier *c,
                     struct tbt_frame *fr);
int tbt_carrier_adopt(struct tbt_tunnel *t, struct socket *newsock);
int tbt_carrier_open_target(struct tbt_stream *st);
void tbt_replace_work(struct work_struct *w);
int tbt_tx_hello(struct tbt_session *sess, u8 index, gfp_t gfp);
/* Session-level 50 ms liveness poll (tbt_carrier.c), armed by tbt_session.c. */
void tbt_session_poll_work(struct work_struct *w);

void tbt_far_open_seen(struct tbt_stream *st);
void tbt_target_detach(struct tbt_stream *st);
void tbt_target_work_fn(struct work_struct *w);

/* tbt_stream.c helpers shared by tx/rx/session */
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
int tbt_session_bind_sid(struct tbt_session *sess, u64 sid, bool open);
void tbt_stream_put(struct tbt_stream *st);
void tbt_stream_detach_from_session(struct tbt_stream *st);
void tbt_stream_kill(struct tbt_stream *st, u8 reason);
void tbt_stream_kill_by_sid(struct tbt_session *sess, u64 sid, u8 reason);
void tbt_session_destroy(struct tbt_tunnel *t);
int tbt_session_create(struct tbt_tunnel *t);
int tbt_session_join(struct tbt_session *sess, struct tbt_carrier *c, u8 index);
void tbt_session_retire(struct tbt_session *sess, u8 index, u8 gen);
bool tbt_carrier_dead(struct tbt_carrier *c);

/* tbt_procfs.c */
int tbt_procfs_init(void);
void tbt_procfs_exit(void);

#endif /* _TBTUN_H */
