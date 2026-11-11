/* tbt_client_engine.c - userspace tbtun client session engine.
 *
 * Single-threaded epoll + timerfd. One engine is one endpoint:
 *
 *   near: listens on cfg.listen for application TCP, dials cfg.carriers
 *         sockets to cfg.peer, and forwards application bytes as FEC shares.
 *   far:  listens on cfg.listen for the carrier pool, accepts the near's
 *         HTTP/HELLO handshake, connects cfg.target, and forwards bytes the
 *         other way.
 *
 * The FEC data plane is the module's own: tbt_fec_* records, SC-LDPC codecs,
 * ACK/NACK/CFG/FBK bodies and the ChaCha8 body transform. The engine adds
 * only the socket session, the deterministic carrier mapping
 * (carrier = group * (n + m) + share), the reorder/decode loop and the
 * local TCP endpoints.
 */
#define _GNU_SOURCE

#include "tbt_client_engine.h"

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_fec_adapt_sclc.h"
#include "tbt_fec_batch.h"
#include "tbt_http.h"
#include "tbt_proto.h"
#include "tbt_rec.h"
#include "tbt_fec_sclc.h"
#include "tbt_sclc_uplink.h"
#include "tbt_uapi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define TBT_CAR_MAX 256
#define TBT_FLOW_MAX 8
#define TBT_RX_SLOTS 8
#define TBT_REPLAY_BLOCKS 8
#define TBT_RX_CODECS 4
#define TBT_CAR_RBUF 65536
#define TBT_CAR_WBUF 131072
#define TBT_FEC_FBK_F_DIRECTED \
    (TBT_FEC_FBK_F_CFG_ACK | TBT_FEC_FBK_F_CFG_NACK | \
     TBT_FEC_FBK_F_SWITCH_DONE | TBT_FEC_FBK_F_ROLLBACK)
#define TBT_APP_RBUF 65536
#define TBT_OUT_CAP (64u * 1024 * 1024)
#define TBT_CTL_PENDING 64
#define TBT_HDR_STAGE_MAX TBT_HTTP_HDR_MAX
#define TBT_OPEN_DELAY_NS (50ull * 1000 * 1000)
/* 0.7.x TBT_SW_FEC_TICK semantics: a partial packet is flushed after the
 * application goes quiet, so a small request is not held behind S bytes. */
#define TBT_TX_IDLE_SEAL_NS (25ull * 1000 * 1000)
#define TBT_NACK_FIRST_NS (30ull * 1000 * 1000)
#define TBT_NACK_RETRY_NS (20ull * 1000 * 1000)
#define TBT_FBK_NS (100ull * 1000 * 1000)
#define TBT_ACK_NS (10ull * 1000 * 1000)
#define TBT_FLOW_IDLE_NS (30ull * 1000 * 1000 * 1000)
#define TBT_CONNECT_RETRY_NS (200ull * 1000 * 1000)

enum ep_kind
{
    EP_NONE = 0,
    EP_CARRIER,
    EP_LISTEN_CARRIER,
    EP_LISTEN_APP,
    EP_FLOW_APP,
    EP_TIMER,
};

struct ep_src
{
    int kind;
    void *p;
};

struct engine;
struct carrier;
struct flow;

struct carrier
{
    struct engine *e;
    struct ep_src ep;
    int fd;
    int index; /* -1 until a far-side HELLO names it */
    u8 gen;
    bool connecting;
    bool connected;
    bool http_hdr_done;
    bool hello_sent;
    bool hello_recv;
    bool up;
    bool dead;
    bool retained;      /* engine_start_carriers keeps it across reconnects */
    struct carrier *prev; /* engine->car_list, every allocation */
    struct carrier *next;
    u64 connect_at_ns;
    u64 retry_at_ns;
    u64 last_rx_ns;
    struct tbt_http_rx rx;
    u8 rbuf[TBT_CAR_RBUF];
    size_t rlen;
    u8 wbuf[TBT_CAR_WBUF];
    size_t wlen;
    size_t woff;
    u64 tx_records;
    u64 rx_records;
    u64 dropped;
};

struct replay_block
{
    bool used;
    u64 id;
    u64 seq0;
    u16 n, m, groups;
    u8 *recs; /* groups * (n + m) * TBT_FEC_REC_MAX */
    u16 *lens;
};

struct rx_group
{
    u16 n, m;
    u16 real;
    u16 epoch;
    bool real_set;
    bool marker;
    bool decoded;
    bool sampled;
    u16 ndata;
    u16 nparity;
    u8 present[TBT_FEC_MAX_SHARES];
    u16 plen[TBT_FEC_MAX_SHARES];
    u8 *buf; /* (n + m) * TBT_FEC_S */
};

struct rx_codec_entry
{
    bool used;
    u16 n, m;
    u64 last_ns;
    struct tbt_fec_grouped *codec;
};

struct rx_block
{
    bool used;
    u64 id;
    u64 seq0;
    u16 n, m, groups;
    u64 first_ns;
    u64 last_nack_ns;
    u8 nack_tries;
    bool stall_reported;
    bool done;
    struct rx_group *g;
};

struct flow
{
    struct engine *e;
    struct ep_src ep;
    u64 sid;
    int app_fd;
    bool near;
    bool open_sent;
    bool open_acked;
    bool fin_sent;
    bool fin_recv;
    bool app_eof;
    bool tx_finished;
    u64 tx_activity_ns; /* last byte fed into the current FEC block */
    bool dead;
    bool target_connecting;
    u64 created_ns;
    u64 tx_ready_at_ns;

    /* TX */
    struct tbt_fec_grouped *tx_codec;
    struct tbt_fec_sclc_shape tx_shape;
    u16 n, m, groups;
    u64 tx_seq;
    u64 tx_block;
    u64 tx_blk_seq0;
    u16 tx_group;
    u16 tx_share;
    u8 *tx_cur;
    size_t tx_cur_len;
    u8 *tx_plane; /* groups * (n + m) * TBT_FEC_S */
    u64 tx_bytes;
    u64 tx_pkt_base;
    bool tx_pkt_base_set;
    u64 tx_last_len;
    u64 tx_ack_next;
    u64 tx_credit_shares;
    u64 last_stat_ns;
    struct replay_block replay[TBT_REPLAY_BLOCKS];

    /* RX */
    struct tbt_fec_sclc_shape rx_shape;
    bool rx_shape_set;
    u32 cfg_epoch;
    u8 peer_m_target;
    u8 peer_n_target;
    u16 rx_n, rx_m, rx_groups, rx_shares;
    u64 rx_seed;
    u64 rx_shape_block;
    struct rx_codec_entry rx_codec[TBT_RX_CODECS];
    u64 rx_codec_clock;
    u64 rx_shape_bad;
    struct rx_block rx[TBT_RX_SLOTS];
    u64 rx_next_block;
    u64 rx_next_deliver;  /* real packets, for FIN/delivery */
    u64 rx_ack_frontier;  /* block-aligned packets, for ACK credit */
    bool rx_deliver_started;
    u64 rx_late_share;
    u64 rx_stale_nack;
    u64 rx_window_drop;
    u64 rx_inflight_bad;
    u64 rx_block_stalled;
    u64 rx_shape_mismatch;
    u32 rx_credit_shares; /* units: shares, one block is groups*(n+m) */
    u64 rx_credit_bad;
    u16 rx_used;
    u64 rx_deliver_bytes;
    u64 peer_fin_end;
    bool peer_fin_end_set;
    bool fin_applied;
    u64 close_at_ns;
    u64 rx_last_ack_ns;
    u32 rtt_us;

    /* feedback controller */
    struct tbt_fec_adapt_sclc_ctl ctl;
    bool ctl_ready;
    u64 last_fbk_ns;

    /* application output */
    u8 *out;
    size_t out_len;
    size_t out_off;
    size_t out_cap;
    u8 rbuf[TBT_APP_RBUF];
};

struct pending_ctl
{
    u8 rec[TBT_FEC_REC_MAX];
    u16 len;
};

struct engine
{
    struct tbt_engine_cfg cfg;
    struct ep_src listen_ep;
    struct ep_src timer_ep;
    int epfd;
    int timerfd;
    int listen_fd;
    bool near;
    bool running;
    u16 n_car;
    struct carrier *car[TBT_CAR_MAX];
    u16 up_count;
    struct flow *flow[TBT_FLOW_MAX];
    struct carrier *car_list; /* every carrier, joined or not */
    u32 car_live;
    u64 next_sid;
    u64 start_ns;
    u64 stop_at_ns;

    /* session crypto */
    bool crypto_on;
    u8 session_nonce[16];
    u8 tx_nonce[TBT_CRYPTO_NONCE_LEN];
    u8 key[TBT_CRYPTO_KEY_LEN];
    bool key_owned;
    bool key_peer;
    bool rx_ready;
    bool session_nonce_set;
    u8 key_id;
    u8 rx_nonce[TBT_CRYPTO_NONCE_LEN];
    struct tbt_crypto_pair crypto;

    /* The two 0.8 FEC directions. The uplink is always the 8x32 GF(16)
     * shape; it never inherits the configured downlink n/m. tx_shape is the
     * shape this role actually transmits: up for near, down for far. */
    struct tbt_fec_sclc_shape down_shape;
    struct tbt_fec_sclc_shape up_shape;
    struct tbt_fec_sclc_shape tx_shape;
    u16 tx_n, tx_m, tx_groups;
    u64 tx_seed;

    /* userspace loss injection */
    u32 drop_ppm;
    u64 rng;
    u64 drops;
    u64 rx_unknown_sid;

    struct pending_ctl pending[TBT_CTL_PENDING];
    int npending;
};

static u64 now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void logmsg(const struct engine *e, const char *fmt, ...)
{
    va_list ap;

    if (!e->cfg.verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void rng_seed(struct engine *e)
{
    u64 v = 0;

    if (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v))
        v = now_ns() ^ (u64)getpid();
    e->rng = v ? v : 0x9e3779b97f4a7c15ull;
}

static u64 rng_next(struct engine *e)
{
    u64 x = e->rng;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    e->rng = x;
    return x;
}

static void fill_random(struct engine *e, u8 *p, size_t n)
{
    size_t off = 0;

    while (off < n)
    {
        u64 v = rng_next(e);
        size_t take = n - off < sizeof(v) ? n - off : sizeof(v);

        memcpy(p + off, &v, take);
        off += take;
    }
}

static int addr_to_ss(const struct tbt_addr *a, u16 port_be,
                      struct sockaddr_storage *ss, socklen_t *len)
{
    memset(ss, 0, sizeof(*ss));
    if (a->family == TBT_AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)ss;

        sin->sin_family = AF_INET;
        sin->sin_port = port_be;
        memcpy(&sin->sin_addr, &a->u.v4, 4);
        *len = sizeof(*sin);
        return 0;
    }
    if (a->family == TBT_AF_INET6)
    {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ss;

        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = port_be;
        memcpy(&sin6->sin6_addr, a->u.v6, 16);
        *len = sizeof(*sin6);
        return 0;
    }
    return -EAFNOSUPPORT;
}

static void set_carrier_sockopts(int fd)
{
    int one = 1;
    int buf = 838861;
    int lowat = 131072;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#ifdef TCP_NOTSENT_LOWAT
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

static int ep_add(struct engine *e, int fd, uint32_t events,
                  struct ep_src *src)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = src;
    return epoll_ctl(e->epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int ep_mod(struct engine *e, int fd, uint32_t events,
                  struct ep_src *src)
{
    struct epoll_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = src;
    return epoll_ctl(e->epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void ep_del(struct engine *e, int fd)
{
    if (fd >= 0)
        (void)epoll_ctl(e->epfd, EPOLL_CTL_DEL, fd, NULL);
}

/* ------------------------------------------------------------------ *
 * Carrier writes and the deterministic carrier map
 * ------------------------------------------------------------------ */

static int carrier_flush(struct carrier *c)
{
    while (c->woff < c->wlen)
    {
        ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);

        if (n > 0)
        {
            c->woff += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            (void)ep_mod(c->e, c->fd,
                         EPOLLIN | EPOLLOUT | EPOLLRDHUP, &c->ep);
            return 0;
        }
        if (n < 0 && errno == EINTR)
            continue;
        c->dead = true;
        return n < 0 ? -errno : -EPIPE;
    }
    c->woff = c->wlen = 0;
    (void)ep_mod(c->e, c->fd, EPOLLIN | EPOLLRDHUP, &c->ep);
    return 0;
}

static int carrier_queue(struct carrier *c, const u8 *data, size_t len)
{
    if (c->dead || c->fd < 0)
        return -ENOTCONN;
    if (c->wlen + len > sizeof(c->wbuf))
    {
        int err = carrier_flush(c);

        if (err)
            return err;
        if (c->wlen + len > sizeof(c->wbuf))
            return -EAGAIN;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
    return carrier_flush(c);
}

static struct carrier *carrier_at(struct engine *e, u16 index)
{
    if (index >= e->n_car)
        return NULL;
    return e->car[index];
}

static struct carrier *ctl_carrier(struct engine *e)
{
    u16 i;

    /* Control frames ride a parity carrier (share >= n) when one is up. The
     * kernel accepts a control record on any carrier, so fall back to any up
     * carrier rather than block the session. */
    for (i = e->tx_n; i < e->n_car; i++)
    {
        struct carrier *c = e->car[i];

        if (c && c->up && !c->dead)
            return c;
    }
    for (i = 0; i < e->n_car; i++)
    {
        struct carrier *c = e->car[i];

        if (c && c->up && !c->dead)
            return c;
    }
    return NULL;
}

/* Wrap one FEC record in the HTTP carrier container and write it. A data
 * record that finds its carrier full is dropped; the peer's NACK replays it.
 * A control record is queued for the timer instead. */
static int send_record_to(struct engine *e, struct carrier *c, const u8 *rec,
                          size_t len, bool control)
{
    char frame[TBT_HTTP_FEC_FRAME_MAX];
    size_t flen;
    int err;

    if (!c)
        return -ENOTCONN;
    flen = tbt_http_fec_chunk_encode(frame, sizeof(frame), rec, len);
    if (!flen)
        return -EINVAL;
    err = carrier_queue(c, (const u8 *)frame, flen);
    if (err == -EAGAIN)
    {
        if (control)
        {
            if (e->npending < TBT_CTL_PENDING && len <= TBT_FEC_REC_MAX)
            {
                memcpy(e->pending[e->npending].rec, rec, len);
                e->pending[e->npending].len = (u16)len;
                e->npending++;
                return 0;
            }
        }
        return -EAGAIN;
    }
    if (!err)
        c->tx_records++;
    return err;
}

static int send_record(struct engine *e, struct carrier *c, const u8 *rec,
                       size_t len, bool control)
{
    if (!c)
        c = ctl_carrier(e);
    if (!c)
    {
        if (control && e->npending < TBT_CTL_PENDING && len <= TBT_FEC_REC_MAX)
        {
            memcpy(e->pending[e->npending].rec, rec, len);
            e->pending[e->npending].len = (u16)len;
            e->npending++;
            return 0;
        }
        return -ENOTCONN;
    }
    return send_record_to(e, c, rec, len, control);
}

static void pending_flush(struct engine *e)
{
    int i = 0;

    while (i < e->npending)
    {
        struct carrier *c = ctl_carrier(e);

        if (!c)
            break;
        if (send_record_to(e, c, e->pending[i].rec, e->pending[i].len,
                           false) == 0)
        {
            e->pending[i] = e->pending[--e->npending];
            continue;
        }
        i++;
    }
}

/* ------------------------------------------------------------------ *
 * Record construction
 * ------------------------------------------------------------------ */

static int make_record(struct engine *e, const struct tbt_fec_hdr *h,
                       const u8 *payload, u8 *out, u16 *out_len)
{
    struct tbt_fec_hdr copy = *h;

    if (tbt_fec_hdr_seal(&copy, payload))
        return -EINVAL;
    if (tbt_fec_hdr_pack(&copy, out))
        return -EINVAL;
    if (copy.payload_len)
        memcpy(out + TBT_FEC_HDR_LEN, payload, copy.payload_len);
    if (e->crypto_on &&
        (copy.type == TBT_FEC_DATA || copy.type == TBT_FEC_PARITY))
    {
        if (tbt_crypto_body_xor_flow(&e->crypto.tx, copy.sid, copy.seq,
                                     copy.type == TBT_FEC_PARITY,
                                     copy.share_index,
                                     out + TBT_FEC_HDR_LEN,
                                     copy.payload_len))
            return -EINVAL;
    }
    *out_len = (u16)(TBT_FEC_HDR_LEN + copy.payload_len);
    return 0;
}

/* tbt_fec_data.c owns the module's convenience wrapper, but the userspace
 * client links only the codec units. The wrapper is three field mappings
 * onto the public ops table, so it lives here rather than pulling the whole
 * kernel data plane into the executable. */
static struct tbt_fec_grouped *
client_sclc_new(const struct tbt_fec_sclc_shape *sh, size_t s, u64 nonce)
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

static int send_ctl_record(struct engine *e, struct flow *f, u8 type, u64 sid,
                           u64 seq, u64 block_id, const u8 *body, u16 len)
{
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_HDR_LEN + TBT_FEC_CFG_LEN];
    u16 rlen;
    int err;

    if (len > TBT_FEC_CFG_LEN)
        return -EINVAL;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.payload_len = len;
    h.conv = TBT_FEC_CONV;
    h.sid = sid;
    h.seq = seq;
    h.block_id = block_id;
    if (make_record(e, &h, body, rec, &rlen))
        return -EINVAL;
    (void)f;
    err = send_record(e, NULL, rec, rlen, true);
    return err;
}

/* ------------------------------------------------------------------ *
 * HELLO and the HTTP handshake
 * ------------------------------------------------------------------ */

static int carrier_send_hello(struct engine *e, struct carrier *c)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 rec[TBT_HDR_LEN + TBT_HELLO_BODY_LEN_V6];
    struct tbt_hdr h;
    int err;

    if (c->index < 0)
        return -EINVAL;
    memset(body, 0, sizeof(body));
    body[0] = TBT_WIRE_VERSION_V6;
    body[1] = e->near ? TBT_ROLE_NEAR : TBT_ROLE_FAR;
    memcpy(body + 2, e->session_nonce, sizeof(e->session_nonce));
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);
    body[22] = (u8)(e->cfg.cfg.profile & 3);
    tbt_crypto_hello_fill(body, e->crypto_on, e->key_id, e->tx_nonce,
                          e->key_id == TBT_CRYPTO_KEY_ID_HELLO ? e->key
                                                               : NULL);
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_HELLO;
    h.path_id = (u8)c->index;
    h.body_len = sizeof(body);
    h.conv = TBT_CONV;
    tbt_raw_put_hdr(rec, &h, sizeof(body));
    memcpy(rec + TBT_HDR_LEN, body, sizeof(body));
    err = send_record_to(e, c, rec, sizeof(rec), true);
    if (!err)
        c->hello_sent = true;
    return err;
}

static void carrier_up(struct engine *e, struct carrier *c)
{
    if (!c->up && c->hello_sent && c->hello_recv)
    {
        c->up = true;
        e->up_count++;
        logmsg(e, "tbt_client: carrier %d up (up=%u)\n", c->index,
               e->up_count);
    }
}

static int carrier_send_http_header(struct engine *e, struct carrier *c)
{
    char hdr[TBT_HTTP_REQ_MAX];
    size_t hlen;

    if (e->near)
    {
        static const char b32[] = "abcdefghijklmnopqrstuvwxyz234567";
        struct tbt_http_cfg hc;
        int i;

        memset(&hc, 0, sizeof(hc));
        tbt_endpoint_format(&e->cfg.cfg.peer, e->cfg.cfg.peer_port, hc.host,
                            sizeof(hc.host));
        for (i = 0; i < TBT_HTTP_REQID_LEN; i++)
            hc.request_id[i] = b32[rng_next(e) % 32];
        hlen = tbt_http_req_header(hdr, sizeof(hdr), &hc);
    }
    else
    {
        struct tbt_http_resp_cfg rc;

        rc.unix_secs = (u64)time(NULL);
        hlen = tbt_http_resp_header(hdr, sizeof(hdr), &rc);
    }
    if (!hlen)
        return -EINVAL;
    return carrier_queue(c, (const u8 *)hdr, hlen);
}

static int carrier_connected(struct engine *e, struct carrier *c)
{
    int err;

    c->connected = true;
    c->connect_at_ns = now_ns();
    if (e->near)
    {
        err = carrier_send_http_header(e, c);
        if (err)
            return err;
        err = carrier_send_hello(e, c);
        if (err)
            return err;
        carrier_up(e, c);
    }
    return 0;
}

static int carrier_connect(struct engine *e, struct carrier *c)
{
    struct sockaddr_storage ss;
    socklen_t slen;
    int fd;
    int err;

    if (c->fd >= 0)
    {
        ep_del(e, c->fd);
        close(c->fd);
    }
    c->fd = -1;
    c->connected = false;
    c->connecting = false;
    c->http_hdr_done = false;
    c->hello_sent = false;
    c->hello_recv = false;
    c->up = false;
    c->dead = false;
    c->rlen = 0;
    c->wlen = c->woff = 0;
    tbt_http_rx_new_message(&c->rx);

    err = addr_to_ss(&e->cfg.cfg.peer, e->cfg.cfg.peer_port, &ss, &slen);
    if (err)
        return err;
    fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    set_carrier_sockopts(fd);
    c->fd = fd;
    c->ep.kind = EP_CARRIER;
    c->ep.p = c;
    err = ep_add(e, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP, &c->ep);
    if (err)
        return -errno;
    if (connect(fd, (struct sockaddr *)&ss, slen) == 0)
        return carrier_connected(e, c);
    if (errno == EINPROGRESS)
    {
        c->connecting = true;
        return 0;
    }
    return -errno;
}

static void carrier_close(struct engine *e, struct carrier *c)
{
    logmsg(e, "tbt_client: carrier %d close fd=%d up=%d hello_sent=%d hello_recv=%d\n",
           c->index, c->fd, c->up, c->hello_sent, c->hello_recv);
    if (c->fd >= 0)
    {
        ep_del(e, c->fd);
        close(c->fd);
        c->fd = -1;
    }
    if (c->up)
    {
        c->up = false;
        if (e->up_count)
            e->up_count--;
    }
    c->dead = true;
    c->connected = false;
    c->connecting = false;
    c->hello_sent = false;
    c->hello_recv = false;
    c->retry_at_ns = now_ns() + TBT_CONNECT_RETRY_NS;
    if (!c->retained)
    {
        /* A far-side carrier owns one connection. Detach its slot now; the
         * reaper frees it after the current epoll batch, so no event that
         * was already copied out of the kernel can reach freed memory. */
        if (c->index >= 0 && c->index < (int)e->n_car &&
            e->car[c->index] == c)
            e->car[c->index] = NULL;
    }
}

static void carrier_unlink(struct engine *e, struct carrier *c)
{
    if (c->prev)
        c->prev->next = c->next;
    else if (e->car_list == c)
        e->car_list = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->prev = c->next = NULL;
    if (e->car_live)
        e->car_live--;
}

static struct carrier *carrier_new(struct engine *e, int fd, int index,
                                   bool retained)
{
    struct carrier *c = calloc(1, sizeof(*c));

    if (!c)
        return NULL;
    c->e = e;
    c->fd = fd;
    c->index = index;
    c->retained = retained;
    c->ep.kind = EP_CARRIER;
    c->ep.p = c;
    tbt_http_rx_new_message(&c->rx);
    c->prev = NULL;
    c->next = e->car_list;
    if (e->car_list)
        e->car_list->prev = c;
    e->car_list = c;
    e->car_live++;
    return c;
}

static void carrier_free(struct engine *e, struct carrier *c)
{
    if (c->index >= 0 && c->index < (int)e->n_car && e->car[c->index] == c)
        e->car[c->index] = NULL;
    if (c->fd >= 0)
    {
        ep_del(e, c->fd);
        close(c->fd);
        c->fd = -1;
    }
    carrier_unlink(e, c);
    free(c);
}

static void carrier_event(struct engine *e, struct carrier *c,
                          uint32_t events);

/* ------------------------------------------------------------------ *
 * Flow allocation, TX plane and replay ring
 * ------------------------------------------------------------------ */

static void flow_free(struct engine *e, struct flow *f);
static void flow_out_flush(struct flow *f);
static void flow_fin_maybe(struct flow *f);
static void flow_app_close(struct flow *f);
static void flow_tx_finish(struct flow *f);
static bool flow_tx_seal_block(struct flow *f, bool finish);
static int flow_send_fin(struct flow *f);
static void flow_tick(struct flow *f, u64 now);
static int flow_target_connect(struct flow *f, u16 port);
static int handle_cfg(struct flow *f, const u8 *body);
static void handle_nack(struct flow *f, const struct tbt_fec_hdr *h,
                        const u8 *body);
static void handle_fbk(struct flow *f, const u8 *body);

static struct flow *flow_alloc(struct engine *e, u64 sid, int app_fd, bool near)
{
    struct flow *f;
    int i;

    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (!e->flow[i])
            break;
    if (i == TBT_FLOW_MAX)
        return NULL;
    f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->e = e;
    f->sid = sid;
    f->app_fd = app_fd;
    f->near = near;
    f->created_ns = now_ns();
    f->tx_ready_at_ns = f->created_ns + TBT_OPEN_DELAY_NS;
    f->n = e->tx_n;
    f->m = e->tx_m;
    f->groups = e->tx_groups;
    f->tx_shape = e->tx_shape;
    f->tx_codec = client_sclc_new(&e->tx_shape, TBT_FEC_S, e->tx_seed);
    f->tx_cur = calloc(1, TBT_FEC_S);
    f->tx_plane = calloc((size_t)f->groups * (f->n + f->m), TBT_FEC_S);
    if (!f->tx_codec || !f->tx_cur || !f->tx_plane)
    {
        free(f->tx_cur);
        free(f->tx_plane);
        if (f->tx_codec)
            tbt_fec_grouped_free(f->tx_codec);
        free(f);
        return NULL;
    }
    f->ep.kind = EP_FLOW_APP;
    f->ep.p = f;
    e->flow[i] = f;
    return f;
}

static void replay_reset(struct flow *f)
{
    int i, j;

    for (i = 0; i < TBT_REPLAY_BLOCKS; i++)
    {
        struct replay_block *r = &f->replay[i];

        if (!r->used)
            continue;
        free(r->recs);
        free(r->lens);
        memset(r, 0, sizeof(*r));
    }
    (void)j;
}

static void flow_free(struct engine *e, struct flow *f)
{
    int i;

    if (!f)
        return;
    logmsg(e, "tbt_client: flow %llu free near=%d fin_sent=%d fin_recv=%d tx=%llu ack=%llu out=%zu/%zu\n",
           (unsigned long long)f->sid, f->near, f->fin_sent, f->fin_recv,
           (unsigned long long)f->tx_seq,
           (unsigned long long)f->tx_ack_next, f->out_len, f->out_off);
    f->dead = true;
    if (f->app_fd >= 0)
    {
        ep_del(e, f->app_fd);
        close(f->app_fd);
        f->app_fd = -1;
    }
    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (e->flow[i] == f)
            e->flow[i] = NULL;
    for (i = 0; i < TBT_RX_SLOTS; i++)
    {
        struct rx_block *b = &f->rx[i];
        int g;

        if (!b->used)
            continue;
        for (g = 0; g < b->groups; g++)
            free(b->g[g].buf);
        free(b->g);
        memset(b, 0, sizeof(*b));
    }
    replay_reset(f);
    free(f->tx_cur);
    free(f->tx_plane);
    if (f->tx_codec)
        tbt_fec_grouped_free(f->tx_codec);
    for (i = 0; i < TBT_RX_CODECS; i++)
    {
        if (f->rx_codec[i].used)
            tbt_fec_grouped_free(f->rx_codec[i].codec);
        memset(&f->rx_codec[i], 0, sizeof(f->rx_codec[i]));
    }
    free(f->out);
    free(f);
}

static int replay_store(struct flow *f, u64 block_id, u16 group, u16 share,
                        const u8 *rec, u16 len)
{
    struct replay_block *r = &f->replay[block_id % TBT_REPLAY_BLOCKS];
    u32 idx;
    u32 total;

    if (!f->groups || !f->n || !f->m)
        return -EINVAL;
    total = (u32)f->groups * (f->n + f->m);
    if (!r->used || r->id != block_id)
    {
        if (!r->recs)
        {
            r->recs = calloc(total, TBT_FEC_REC_MAX);
            r->lens = calloc(total, sizeof(*r->lens));
            if (!r->recs || !r->lens)
                return -ENOMEM;
        }
        else
        {
            memset(r->lens, 0, total * sizeof(*r->lens));
        }
        r->used = true;
        r->id = block_id;
        r->n = f->n;
        r->m = f->m;
        r->groups = f->groups;
        r->seq0 = f->tx_blk_seq0;
    }
    idx = (u32)group * (r->n + r->m) + share;
    if (idx >= total || len > TBT_FEC_REC_MAX)
        return -EINVAL;
    memcpy(r->recs + (size_t)idx * TBT_FEC_REC_MAX, rec, len);
    r->lens[idx] = len;
    return 0;
}

static int flow_send_data(struct flow *f, u16 group, u16 share, u16 len,
                          bool last)
{
    struct engine *e = f->e;
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_REC_MAX];
    u16 rlen;
    u16 carrier;
    u8 *plane;
    int err;

    plane = f->tx_plane + ((size_t)group * (f->n + f->m) + share) * TBT_FEC_S;
    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_DATA;
    h.flags = last ? TBT_FEC_FF_LAST : 0;
    if (len < TBT_FEC_S)
    {
        logmsg(e, "tbt_client: flow %llu short data group=%u share=%u len=%u\n",
               (unsigned long long)f->sid, group, share, len);
        /* 0.8.0 constant frame: the slot is always S bytes on the wire.
         * The real length travels in win_len and the tail is zero, because
         * the payload CRC covers the full slot. */
        h.flags |= TBT_FEC_FF_PADDED;
        h.win_len = len;
        h.payload_len = TBT_FEC_S;
        memset(plane + len, 0, TBT_FEC_S - len);
    }
    else
    {
        h.win_len = 0;
        h.payload_len = len;
    }
    h.share_index = share;
    h.n = (u8)f->n;
    h.m = (u8)f->m;
    h.block_id = f->tx_block;
    h.seq = f->tx_blk_seq0 + (u64)group * f->n + share;
    h.conv = TBT_FEC_CONV;
    h.sid = f->sid;
    h.reserved = group;
    if (make_record(e, &h, plane, rec, &rlen))
        return -EIO;
    (void)replay_store(f, f->tx_block, group, share, rec, rlen);
    f->tx_last_len = len;
    f->tx_seq = h.seq + 1;
    if (!f->tx_pkt_base_set)
    {
        f->tx_pkt_base = h.seq;
        f->tx_pkt_base_set = true;
    }
    carrier = (u16)(group * (f->n + f->m) + share);
    if (e->drop_ppm && (u32)(rng_next(e) % 1000000u) < e->drop_ppm)
    {
        e->drops++;
        return 0;
    }
    err = send_record(e, carrier_at(e, carrier), rec, rlen, false);
    return err == -EAGAIN ? 0 : err;
}

static int flow_send_parity(struct flow *f, u16 group, u16 win_len,
                            u16 last_len)
{
    struct engine *e = f->e;
    struct tbt_fec_hdr h;
    u8 *data_ptrs[TBT_FEC_MAX_SHARES];
    u8 *parity_ptrs[TBT_FEC_MAX_SHARES];
    u8 rec[TBT_FEC_REC_MAX];
    u16 count, rlen, i;
    u16 carrier;
    int err;

    if (!f->tx_codec || !win_len || win_len > f->n)
        return -EINVAL;
    for (i = 0; i < f->n; i++)
        data_ptrs[i] = f->tx_plane +
                       ((size_t)group * (f->n + f->m) + i) * TBT_FEC_S;
    for (i = 0; i < f->m; i++)
        parity_ptrs[i] = f->tx_plane +
                         ((size_t)group * (f->n + f->m) + f->n + i) *
                             TBT_FEC_S;
    if (tbt_fec_grouped_encode(f->tx_codec, group, data_ptrs, f->n,
                               parity_ptrs) < 0)
        return -EIO;
    count = tbt_fec_tx_parity_count(win_len, f->n, f->m, true);
    for (i = 0; i < count; i++)
    {
        memset(&h, 0, sizeof(h));
        h.type = TBT_FEC_PARITY;
        h.share_index = (u16)(f->n + i);
        h.n = (u8)f->n;
        h.m = (u8)f->m;
        h.block_id = f->tx_block;
        h.seq = f->tx_blk_seq0;
        h.win_len = win_len;
        h.payload_len = TBT_FEC_S;
        h.epoch = last_len && last_len < TBT_FEC_S ? last_len : 0;
        h.conv = TBT_FEC_CONV;
        h.sid = f->sid;
        h.reserved = group;
        if (make_record(e, &h, parity_ptrs[i], rec, &rlen))
            return -EIO;
        (void)replay_store(f, f->tx_block, group, (u16)(f->n + i), rec,
                           rlen);
        carrier = (u16)(group * (f->n + f->m) + f->n + i);
        if (e->drop_ppm && (u32)(rng_next(e) % 1000000u) < e->drop_ppm)
        {
            e->drops++;
            continue;
        }
        err = send_record(e, carrier_at(e, carrier), rec, rlen, false);
        if (err && err != -EAGAIN)
            return err;
    }
    return 0;
}

static int flow_send_marker(struct flow *f, u16 group)
{
    struct engine *e = f->e;
    struct tbt_fec_hdr h;
    u8 rec[TBT_FEC_HDR_LEN];
    u16 rlen;
    u16 carrier;

    memset(&h, 0, sizeof(h));
    h.type = TBT_FEC_PARITY;
    h.share_index = f->n;
    h.n = (u8)f->n;
    h.m = (u8)f->m;
    h.block_id = f->tx_block;
    h.seq = f->tx_blk_seq0;
    h.win_len = 0;
    h.payload_len = 0;
    h.epoch = 0;
    h.conv = TBT_FEC_CONV;
    h.sid = f->sid;
    h.reserved = group;
    if (make_record(e, &h, NULL, rec, &rlen))
        return -EIO;
    (void)replay_store(f, f->tx_block, group, f->n, rec, rlen);
    carrier = (u16)(group * (f->n + f->m) + f->n);
    return send_record(e, carrier_at(e, carrier), rec, rlen, true);
}

static void flow_tx_advance(struct flow *f)
{
    f->tx_share++;
    if (f->tx_share < f->n)
        return;
    (void)flow_send_parity(f, f->tx_group, f->n, 0);
    f->tx_share = 0;
    f->tx_group++;
    if (f->tx_group >= f->groups)
    {
        f->tx_group = 0;
        f->tx_block++;
        f->tx_blk_seq0 += (u64)f->groups * f->n;
        /* The next block starts from zero: an unused share left over from
         * this one would corrupt that block's parity. */
        memset(f->tx_plane, 0,
               (size_t)f->groups * (f->n + f->m) * TBT_FEC_S);
    }
}

static int flow_tx_feed(struct flow *f, const u8 *data, size_t len)
{
    if (len)
        f->tx_activity_ns = now_ns();
    while (len)
    {
        size_t room = TBT_FEC_S - f->tx_cur_len;
        size_t take = len < room ? len : room;
        u8 *plane;

        memcpy(f->tx_cur + f->tx_cur_len, data, take);
        f->tx_cur_len += take;
        f->tx_bytes += take;
        data += take;
        len -= take;
        if (f->tx_cur_len < TBT_FEC_S)
            break;
        plane = f->tx_plane +
                ((size_t)f->tx_group * (f->n + f->m) + f->tx_share) *
                    TBT_FEC_S;
        memcpy(plane, f->tx_cur, TBT_FEC_S);
        if (flow_send_data(f, f->tx_group, f->tx_share, TBT_FEC_S, false))
            return -EIO;
        f->tx_cur_len = 0;
        flow_tx_advance(f);
    }
    return 0;
}

static int flow_send_open(struct flow *f, bool echo)
{
    struct engine *e = f->e;
    u8 body[TBT_OPEN_BODY_LEN];
    u8 rec[TBT_HDR_LEN + TBT_OPEN_BODY_LEN];
    struct tbt_hdr h;
    u16 port = 0;

    memset(body, 0, sizeof(body));
    if (f->groups)
    {
        u16 g;

        for (g = 0; g < f->groups && g < TBT_FEC_MAX_CARRIERS; g++)
            body[g / 8] |= (u8)(1u << (g % 8));
    }
    if (!echo && e->cfg.cfg.target_port)
        port = tbt_port_host16(e->cfg.cfg.target_port);
    tbt_put_le16(body + TBT_OPEN_ROUTE_SLOT, 0);
    tbt_put_le16(body + TBT_OPEN_TARGET_PORT, port);
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_OPEN;
    h.body_len = sizeof(body);
    h.conv = TBT_CONV;
    h.sid_lo = (u32)f->sid;
    h.sid_hi = (u8)(f->sid >> 32);
    tbt_raw_put_hdr(rec, &h, sizeof(body));
    memcpy(rec + TBT_HDR_LEN, body, sizeof(body));
    f->open_sent = true;
    return send_record(e, NULL, rec, sizeof(rec), true);
}

static int flow_send_fin(struct flow *f)
{
    struct engine *e = f->e;
    u8 body[TBT_FIN_BODY_LEN];
    u8 rec[TBT_HDR_LEN + TBT_FIN_BODY_LEN];
    struct tbt_hdr h;

    if (f->fin_sent)
        return 0;
    memset(body, 0, sizeof(body));
    tbt_put_le64(body + 0, f->tx_seq);
    tbt_put_le64(body + 8, f->tx_pkt_base);
    memset(&h, 0, sizeof(h));
    h.type = TBT_F_FIN;
    h.body_len = sizeof(body);
    h.conv = TBT_CONV;
    h.seq = f->tx_seq;
    h.sid_lo = (u32)f->sid;
    h.sid_hi = (u8)(f->sid >> 32);
    tbt_raw_put_hdr(rec, &h, sizeof(body));
    memcpy(rec + TBT_HDR_LEN, body, sizeof(body));
    f->fin_sent = true;
    return send_record(e, NULL, rec, sizeof(rec), true);
}

static int flow_send_rst(struct flow *f)
{
    struct engine *e = f->e;
    u8 rec[TBT_HDR_LEN];
    struct tbt_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_RST;
    h.body_len = 0;
    h.conv = TBT_CONV;
    h.sid_lo = (u32)f->sid;
    h.sid_hi = (u8)(f->sid >> 32);
    tbt_raw_put_hdr(rec, &h, 0);
    return send_record(e, NULL, rec, sizeof(rec), true);
}

static int flow_send_cfg(struct flow *f)
{
    struct engine *e = f->e;
    struct tbt_fec_cfg c;
    u8 body[TBT_FEC_CFG_LEN];

    memset(&c, 0, sizeof(c));
    c.epoch = 1;
    c.n = f->tx_shape.n;
    c.m = f->tx_shape.m;
    c.flags = TBT_FEC_CFG_F_INIT;
    c.groups = f->tx_shape.groups;
    c.seed = e->tx_seed;
    c.codec_variant = 2;
    c.L = f->tx_shape.L;
    c.block = f->tx_shape.block;
    c.rate_ppm = (u32)(((u64)f->tx_shape.n * 1000000u +
                        (f->tx_shape.n + f->tx_shape.m) - 1u) /
                       (f->tx_shape.n + f->tx_shape.m));
    logmsg(e, "tbt_client: flow %llu cfg tx %s n=%u m=%u groups=%u block=%u L=%u\n",
           (unsigned long long)f->sid, e->near ? "up" : "down", c.n, c.m,
           c.groups, c.block, c.L);
    if (tbt_fec_cfg_pack(&c, body))
        return -EINVAL;
    return send_ctl_record(e, f, TBT_FEC_CFG, f->sid, 0, 0, body, sizeof(body));
}

/* One ring slot holds a whole block: n + m shares, DATA and PARITY alike.
 * The grant is slots-1 blocks of shares, leaving one slot for the block
 * under delivery. A short block still occupies the slot, so the grant does
 * not shrink with its real share count. */
static u32 flow_rx_credit_update(struct flow *f)
{
    u64 block = (u64)f->rx_groups * ((u32)f->rx_n + f->rx_m);
    u64 credit = (u64)(TBT_RX_SLOTS - 1) * block;
    u64 cap = (u64)TBT_RX_SLOTS * block;

    if (!block || credit < block || credit > cap ||
        credit > (u64)0xffffffffu)
        f->rx_credit_bad++;
    f->rx_credit_shares = (u32)credit;
    return (u32)credit;
}

static int flow_send_ack(struct flow *f)
{
    struct engine *e = f->e;
    struct tbt_fec_ack a;
    u8 body[TBT_FEC_ACK_LEN];

    memset(&a, 0, sizeof(a));
    a.next_deliver = f->rx_ack_frontier;
    a.credit_packets = flow_rx_credit_update(f);
    a.rtt_us = f->rtt_us ? f->rtt_us : 1000;
    a.deliver_bytes = f->rx_deliver_bytes;
    if (tbt_fec_ack_pack(&a, body))
        return -EINVAL;
    f->rx_last_ack_ns = now_ns();
    return send_ctl_record(e, f, TBT_FEC_ACK, f->sid, 0, 0, body, sizeof(body));
}

/* A codec for one block's n/m, not the session default. The 0.8 sender may
 * move the split per block, so the cache is keyed by (n, m); block, L and the
 * seed stay fixed for the direction. Four entries cover a transition. */
static struct tbt_fec_grouped *flow_rx_codec(struct flow *f, u16 n, u16 m)
{
    struct tbt_fec_sclc_shape sh;
    struct tbt_fec_grouped *codec;
    u16 shares = (u16)(n + m);
    int slot = -1;
    int i;

    if (!f->rx_shape_set || !n || !m || shares != f->rx_shares)
        return NULL;
    for (i = 0; i < TBT_RX_CODECS; i++)
    {
        if (f->rx_codec[i].used && f->rx_codec[i].n == n &&
            f->rx_codec[i].m == m)
            return f->rx_codec[i].codec;
        if (!f->rx_codec[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0)
    {
        u64 oldest = ~0ull;

        for (i = 0; i < TBT_RX_CODECS; i++)
        {
            if (f->rx_codec[i].last_ns < oldest)
            {
                oldest = f->rx_codec[i].last_ns;
                slot = i;
            }
        }
        tbt_fec_grouped_free(f->rx_codec[slot].codec);
        memset(&f->rx_codec[slot], 0, sizeof(f->rx_codec[slot]));
    }
    sh = f->rx_shape;
    sh.n = n;
    sh.m = m;
    sh.groups = f->rx_groups;
    sh.rate_ppm = (u32)(((u64)n * 1000u) / shares);
    sh.rate_256 = (u16)(((u32)n * 256u) / shares);
    codec = client_sclc_new(&sh, TBT_FEC_S, f->rx_seed);
    if (!codec)
        return NULL;
    f->rx_codec[slot].used = true;
    f->rx_codec[slot].n = n;
    f->rx_codec[slot].m = m;
    f->rx_codec[slot].last_ns = ++f->rx_codec_clock;
    f->rx_codec[slot].codec = codec;
    return codec;
}

/* Adopt one n/m as this receiver's current expectation. The FBK target is a
 * commitment: the ACK credit and the expected shape move with it, even while
 * older blocks of the previous split are still draining. */
static bool flow_commit_shape(struct flow *f, u16 n, u16 m)
{
    if (!n || !m || (u16)(n + m) != f->rx_shares)
        return false;
    if (!tbt_fec_block_shape_ok(n, m, 0, 0, f->rx_shares, f->rx_groups,
                               true))
        return false;
    if (f->rx_n != n || f->rx_m != m)
    {
        f->rx_n = n;
        f->rx_m = m;
        logmsg(f->e, "tbt_client: flow %llu cfg rx n=%u m=%u (fbk)\n",
               (unsigned long long)f->sid, n, m);
        (void)flow_rx_credit_update(f);
    }
    if (f->rx_shape_block < f->rx_next_block)
        f->rx_shape_block = f->rx_next_block;
    return true;
}

static int flow_send_fbk(struct flow *f, u8 flags, u32 epoch)
{
    struct engine *e = f->e;
    struct tbt_fec_fbk fb;
    struct tbt_fec_adapt_sclc_out out;
    u8 body[TBT_FEC_FBK_LEN];
    u64 now = now_ns();

    memset(&fb, 0, sizeof(fb));
    tbt_fec_fbk_set_epoch(&fb, epoch);
    fb.flags = flags;
    if (!(flags & TBT_FEC_FBK_F_DIRECTED) && !e->cfg.cfg.adaptive)
        return 0;
    if (f->ctl_ready && !(flags & TBT_FEC_FBK_F_DIRECTED))
    {
        u16 n_new;

        tbt_fec_adapt_sclc_tick(&f->ctl, now, 2, 1000000, e->up_count, 0, 0,
                                &out);
        if (out.m_target >= f->rx_shares)
        {
            f->rx_shape_bad++;
        }
        else
        {
            n_new = (u16)(f->rx_shares - out.m_target);
            if (!flow_commit_shape(f, n_new, (u16)out.m_target))
                f->rx_shape_bad++;
        }
    }
    fb.m_current = (u8)f->rx_m;
    fb.m_target = (u8)f->rx_m;
    fb.n_current = (u8)f->rx_n;
    fb.expected_shares = (u16)(f->rx_groups * f->rx_shares);
    fb.received_shares = fb.expected_shares;
    fb.codec_variant = 2;
    fb.L = (u8)f->rx_shape.L;
    fb.block = f->rx_shape.block;
    fb.rate_ppm_current =
        (u32)(((u64)f->rx_n * 1000000u + f->rx_shares - 1u) / f->rx_shares);
    fb.rate_ppm_target = fb.rate_ppm_current;
    {
        u16 i;

        for (i = 0; i < e->n_car; i++)
        {
            struct carrier *c = e->car[i];

            if (c && c->up)
                fb.carrier_bitmap[i / 8] |= (u8)(1u << (i % 8));
        }
    }
    if (tbt_fec_fbk_pack(&fb, body))
        return -EINVAL;
    f->last_fbk_ns = now;
    return send_ctl_record(e, f, TBT_FEC_FBK, f->sid, 0, 0, body, sizeof(body));
}

/* ------------------------------------------------------------------ *
 * Receive side: reorder window, SC-LDPC decode, ordered delivery
 * ------------------------------------------------------------------ */

static void rx_block_release(struct flow *f, struct rx_block *b)
{
    int g;

    if (!b->used)
        return;
    if (f && f->rx_used)
        f->rx_used--;
    for (g = 0; g < b->groups; g++)
        free(b->g[g].buf);
    free(b->g);
    memset(b, 0, sizeof(*b));
}

static struct rx_block *rx_block_get(struct flow *f,
                                     const struct tbt_fec_hdr *h, u16 group,
                                     u16 share)
{
    u64 id = h->block_id;
    u64 seq0;
    u32 slot;
    struct rx_block *b;
    int g;

    if (!f->rx_shape_set || !f->rx_groups)
        return NULL;
    if (id < f->rx_next_block)
    {
        /* A replay that arrived after delivery moved past its block. The
         * counter is the deadlock sentinel: a NACK must never name a block
         * below rx_next_block, or the pair can spin forever. */
        f->rx_late_share++;
        return NULL;
    }
    if (id >= f->rx_next_block + TBT_RX_SLOTS)
    {
        f->rx_window_drop++;
        return NULL;
    }
    /* n/m are per block: the 0.8 sender may move the split every block.
     * Only the share count and the group count are session-fixed. */
    if (!h->n || !h->m || (u16)(h->n + h->m) != f->rx_shares ||
        h->reserved >= f->rx_groups ||
        !tbt_fec_block_shape_ok(h->n, h->m, 0, 0, f->rx_shares,
                                f->rx_groups, true))
    {
        f->rx_shape_bad++;
        return NULL;
    }
    if (h->type == TBT_FEC_DATA)
    {
        if (!tbt_fec_data_seq0(h->seq, group, h->n, share, &seq0))
            return NULL;
    }
    else
    {
        seq0 = h->seq;
    }
    slot = (u32)(id % TBT_RX_SLOTS);
    b = &f->rx[slot];
    if (b->used && b->id != id)
        rx_block_release(f, b);
    if (!b->used)
    {
        f->rx_used++;
        b->used = true;
        b->id = id;
        b->seq0 = seq0;
        b->n = h->n;
        b->m = h->m;
        b->groups = f->rx_groups;
        b->first_ns = now_ns();
        b->g = calloc(b->groups, sizeof(*b->g));
        if (!b->g)
        {
            if (f->rx_used)
                f->rx_used--;
            memset(b, 0, sizeof(*b));
            return NULL;
        }
        for (g = 0; g < b->groups; g++)
        {
            b->g[g].n = h->n;
            b->g[g].m = h->m;
        }
    }
    else if (b->seq0 != seq0 || b->n != h->n || b->m != h->m)
    {
        /* A block id is a stream-local identity and one block has one
         * split; a second origin or a different n/m for the same id is a
         * protocol error, not a reason to rewrite history. */
        f->rx_shape_bad++;
        return NULL;
    }
    return b;
}

static void flow_out_flush(struct flow *f)
{
    struct engine *e = f->e;

    if (f->app_fd < 0)
        return;
    while (f->out_off < f->out_len)
    {
        ssize_t n = write(f->app_fd, f->out + f->out_off,
                          f->out_len - f->out_off);

        if (n > 0)
        {
            f->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            (void)ep_mod(e, f->app_fd,
                         EPOLLIN | EPOLLOUT | EPOLLRDHUP, &f->ep);
            return;
        }
        if (n < 0 && errno == EINTR)
            continue;
        flow_app_close(f);
        return;
    }
    if (f->app_eof)
    {
        f->out_off = f->out_len = 0;
        /* The read side is done and there is nothing left to write: stop
         * polling this fd until FIN processing closes it. */
        ep_del(e, f->app_fd);
    }
    else
    {
        f->out_off = f->out_len = 0;
        (void)ep_mod(e, f->app_fd, EPOLLIN | EPOLLRDHUP, &f->ep);
    }
    flow_fin_maybe(f);
}

static int flow_out_append(struct flow *f, const u8 *data, size_t len)
{
    size_t need;

    if (f->dead)
        return -EPIPE;
    if (f->out_off)
    {
        memmove(f->out, f->out + f->out_off, f->out_len - f->out_off);
        f->out_len -= f->out_off;
        f->out_off = 0;
    }
    need = f->out_len + len;
    if (need > f->out_cap)
    {
        size_t cap = f->out_cap ? f->out_cap : 262144;

        while (cap < need)
            cap *= 2;
        if (cap > TBT_OUT_CAP)
            return -ENOSPC;
        {
            u8 *p = realloc(f->out, cap);

            if (!p)
                return -ENOMEM;
            f->out = p;
            f->out_cap = cap;
        }
    }
    memcpy(f->out + f->out_len, data, len);
    f->out_len += len;
    flow_out_flush(f);
    return 0;
}

static void flow_sample_block(struct flow *f, struct rx_group *g, u16 real)
{
    u16 e;
    u16 s;
    bool ok = g->decoded;

    if (g->sampled || !f->ctl_ready)
        return;
    g->sampled = true;
    e = g->ndata >= real ? 0 : (u16)(real - g->ndata);
    s = (u16)(g->ndata + g->nparity > real ? g->ndata + g->nparity - real
                                           : 0);
    tbt_fec_adapt_sclc_block_sample(&f->ctl, e, s, ok, real);
}

static void rx_group_try(struct flow *f, struct rx_block *b, u16 group)
{
    struct rx_group *g = &b->g[group];
    u8 *data_ptrs[TBT_FEC_MAX_SHARES];
    u8 *parity_ptrs[TBT_FEC_MAX_SHARES];
    u8 dp[TBT_FEC_MAX_SHARES];
    u8 pp[TBT_FEC_MAX_SHARES];
    u16 n = b->n;
    u16 m = b->m;
    u16 real;
    u16 i;
    bool ok = true;

    if (g->decoded || g->marker)
        return;
    real = g->real_set ? g->real : n;
    if (!real || real > n)
        return;
    if (g->ndata >= real)
    {
        /* A systematic block with every DATA share present needs no
         * decode: the shares already carry the application bytes. */
        g->decoded = true;
        g->real = real;
        g->real_set = true;
        flow_sample_block(f, g, real);
        return;
    }
    if (!g->real_set)
        return;
    {
        u16 extra = f->rx_groups > 1
                        ? TBT_FEC_SCLC_WAKE_EXTRA_UP
                        : tbt_fec_sclc_wake_extra_down(n);

        if ((u32)g->ndata + g->nparity < (u32)real + extra)
            return;
    }
    {
        struct tbt_fec_grouped *codec = flow_rx_codec(f, n, m);

        if (!codec)
            return;
        for (i = 0; i < n; i++)
        {
            data_ptrs[i] = g->buf + (size_t)i * TBT_FEC_S;
            dp[i] = g->present[i] ? 1 : 0;
        }
        for (i = 0; i < m; i++)
        {
            parity_ptrs[i] = g->buf + (size_t)(n + i) * TBT_FEC_S;
            pp[i] = g->present[n + i] ? 1 : 0;
        }
        (void)tbt_fec_grouped_decode(codec, group, data_ptrs, parity_ptrs, dp,
                                     pp, n);
        for (i = 0; i < real; i++)
            if (!tbt_fec_grouped_is_known(codec, group, i))
                ok = false;
    }
    if (ok)
    {
        g->decoded = true;
        g->real = real;
        g->real_set = true;
        g->epoch = g->present[real - 1] ? g->plen[real - 1] : g->epoch;
        flow_sample_block(f, g, real);
    }
}

static void rx_bitmap_str(const u8 *present, u16 start, u16 count, char *out,
                          size_t cap)
{
    size_t o = 0;
    u16 i;

    for (i = 0; i < count && o + 1 < cap; i++)
        out[o++] = present[start + i] ? '1' : '0';
    out[o] = '\0';
}

/* A head block that stayed incomplete for 200 ms. The bitmaps separate
 * "the record never arrived" from "a record arrived under a different
 * shape"; the record log below prints the same h.n/h.m and match flag. */
static void flow_rx_dump_stall(struct flow *f, struct rx_block *b, u64 now)
{
    char dbits[TBT_FEC_MAX_SHARES + 1];
    char pbits[TBT_FEC_MAX_SHARES + 1];
    u16 g;

    logmsg(f->e,
           "tbt_client: rx STALL sid=%llu block=%llu n=%u m=%u groups=%u next=%llu deliver=%llu age_ms=%llu\n",
           (unsigned long long)f->sid, (unsigned long long)b->id, b->n, b->m,
           b->groups, (unsigned long long)f->rx_next_block,
           (unsigned long long)f->rx_next_deliver,
           (unsigned long long)((now - b->first_ns) / 1000000ull));
    for (g = 0; g < b->groups; g++)
    {
        struct rx_group *rg = &b->g[g];
        u16 real = rg->marker ? 0 : (rg->real_set ? rg->real : b->n);

        rx_bitmap_str(rg->present, 0, b->n, dbits, sizeof(dbits));
        rx_bitmap_str(rg->present, b->n, b->m, pbits, sizeof(pbits));
        logmsg(f->e,
               "tbt_client: rx STALL group=%u marker=%d real=%u/%u ndata=%u nparity=%u data=%s parity=%s\n",
               g, rg->marker ? 1 : 0, real, b->n, rg->ndata, rg->nparity,
               dbits, pbits);
    }
}

static void flow_rx_check(struct flow *f)
{
    for (;;)
    {
        u32 slot = (u32)(f->rx_next_block % TBT_RX_SLOTS);
        struct rx_block *b = &f->rx[slot];
        u16 group;
        u64 blk_pkts = 0;
        bool ready = true;

        if (!b->used || b->id != f->rx_next_block)
            return;
        for (group = 0; group < b->groups; group++)
        {
            struct rx_group *g = &b->g[group];

            if (!g->decoded && !g->marker)
            {
                ready = false;
                break;
            }
        }
        if (!ready)
            return;
        for (group = 0; group < b->groups; group++)
        {
            struct rx_group *g = &b->g[group];
            u16 real = g->marker ? 0 : (g->real_set ? g->real : b->n);
            u16 s;

            blk_pkts += real;
            for (s = 0; s < real; s++)
            {
                u16 len = g->plen[s]
                              ? g->plen[s]
                              : ((s + 1 == real && g->epoch) ? g->epoch
                                                             : TBT_FEC_S);

                if (flow_out_append(f, g->buf + (size_t)s * TBT_FEC_S, len))
                {
                    f->dead = true;
                    return;
                }
                f->rx_deliver_bytes += len;
            }
        }
        /* The delivery frontier counts real packets, not block-aligned
         * slots: a marker block adds 0 and a short block adds the sum of its
         * group win_len values. Using b->seq0 here made marker blocks jump
         * the frontier and left a later NACK naming a block already passed. */
        if (!f->rx_deliver_started)
        {
            f->rx_next_deliver = b->seq0;
            f->rx_ack_frontier = b->seq0;
            f->rx_deliver_started = true;
        }
        f->rx_next_deliver += blk_pkts;
        /* Credit is spent in whole blocks on the sender side. A short or
         * marker block still advances the sender's block origin by
         * groups*n, so the ACK frontier must do the same even though the
         * byte/packet delivery cursor advances only by real packets. */
        f->rx_ack_frontier = b->seq0 + (u64)b->groups * b->n;
        f->rx_next_block = b->id + 1;
        rx_block_release(f, b);
        flow_fin_maybe(f);
        (void)flow_send_ack(f);
    }
}

static void flow_rx_store(struct flow *f, const struct tbt_fec_hdr *h,
                          const u8 *payload)
{
    u16 group = h->reserved;
    u16 share = h->share_index;
    struct rx_block *b;
    struct rx_group *g;
    u16 shares = (u16)(h->n + h->m);

    {
        u16 committed_n = f->rx_n;
        u16 committed_m = f->rx_m;
        bool match = (h->n == committed_n && h->m == committed_m);

        if (!match)
            f->rx_shape_mismatch++;
        b = rx_block_get(f, h, group, share);
        if (h->block_id == f->rx_next_block)
            logmsg(f->e,
                   "tbt_client: rx rec block=%llu type=%u group=%u share=%u n=%u m=%u committed=%u/%u match=%d accepted=%d\n",
                   (unsigned long long)h->block_id, h->type, group, share,
                   h->n, h->m, committed_n, committed_m, match ? 1 : 0,
                   b ? 1 : 0);
        if (!b)
            return;
        if (b->id >= f->rx_shape_block)
        {
            f->rx_shape_block = b->id;
            f->rx_n = b->n;
            f->rx_m = b->m;
        }
    }
    g = &b->g[group];
    if (share >= shares)
        return;
    if (g->present[share])
        return;
    if (!g->buf)
    {
        g->buf = calloc(shares, TBT_FEC_S);
        if (!g->buf)
            return;
    }
    memcpy(g->buf + (size_t)share * TBT_FEC_S, payload, h->payload_len);
    g->present[share] = 1;
    g->plen[share] = (h->flags & TBT_FEC_FF_PADDED) ? h->win_len
                                                    : h->payload_len;
    if (share < h->n)
        g->ndata++;
    else
        g->nparity++;
    if (h->type == TBT_FEC_PARITY)
    {
        if (h->win_len == 0)
        {
            g->marker = true;
            g->real = 0;
            g->real_set = true;
        }
        else if (!g->real_set)
        {
            g->real = h->win_len;
            g->epoch = h->epoch;
            g->real_set = true;
        }
    }
    rx_group_try(f, b, group);
    flow_rx_check(f);
    {
        u16 inflight = f->rx_used;
        u32 head = (u32)(f->rx_next_block % TBT_RX_SLOTS);

        if (inflight && f->rx[head].used &&
            f->rx[head].id == f->rx_next_block)
            inflight--;
        if (inflight > TBT_RX_SLOTS - 1)
            f->rx_inflight_bad++;
    }
}

static void flow_send_nack_group(struct flow *f, struct rx_block *b,
                                 u16 group, u64 now)
{
    struct engine *e = f->e;
    struct tbt_fec_nack n;
    struct rx_group *g = &b->g[group];
    u8 body[TBT_FEC_NACK_LEN];
    u16 real = g->real_set ? g->real : b->n;
    u16 s;
    bool any = false;

    if (b->id < f->rx_next_block)
    {
        /* The caller must only NACK the window head or a block inside the
         * 8-slot ring. Reaching this branch is the stale-request deadlock
         * the counters exist to expose. */
        f->rx_stale_nack++;
        return;
    }
    memset(&n, 0, sizeof(n));
    tbt_fec_bitmap_set(n.group_mask, group);
    for (s = 0; s < real; s++)
    {
        if (!g->present[s])
        {
            tbt_fec_bitmap_set(n.share_mask, s);
            any = true;
        }
    }
    if (!g->real_set)
    {
        /* The parity that names win_len is missing: ask for every parity
         * share so the real length and the epoch arrive. */
        for (s = b->n; s < b->n + b->m; s++)
        {
            if (!g->present[s])
            {
                tbt_fec_bitmap_set(n.share_mask, s);
                any = true;
            }
        }
    }
    if (!any)
        return;
    if (tbt_fec_nack_pack(&n, body) < 0)
        return;
    b->last_nack_ns = now;
    b->nack_tries++;
    (void)send_ctl_record(e, f, TBT_FEC_NACK, f->sid, 0, b->id, body, sizeof(body));
}

static void flow_rx_nack(struct flow *f, u64 now)
{
    u32 off;

    /* The window head is the only NACK anchor. Scanning by slot let a stale
     * block id below rx_next_block stay in the ring while the delivery
     * frontier moved, so the pair kept requesting a block already passed. */
    for (off = 0; off < TBT_RX_SLOTS; off++)
    {
        u64 id = f->rx_next_block + off;
        struct rx_block *b = &f->rx[id % TBT_RX_SLOTS];
        u16 group;

        if (!b->used || b->id != id)
            continue;
        if (now - b->first_ns < TBT_NACK_FIRST_NS)
            continue;
        if (now - b->last_nack_ns < TBT_NACK_RETRY_NS)
            continue;
        for (group = 0; group < b->groups; group++)
        {
            struct rx_group *g = &b->g[group];

            if (g->decoded || g->marker)
                continue;
            flow_send_nack_group(f, b, group, now);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Session handshake records
 * ------------------------------------------------------------------ */

static int hello_crypto(struct engine *e, const u8 *body, size_t len)
{
    bool enabled = false;
    u8 kid = 0;
    u8 peer_nonce[TBT_CRYPTO_NONCE_LEN];
    u8 hello_key[TBT_CRYPTO_KEY_LEN];
    u8 key[TBT_CRYPTO_KEY_LEN];
    bool local_wins;
    int err;

    if (tbt_crypto_hello_parse(body, len, &enabled, &kid, peer_nonce,
                               hello_key))
        return -EPROTO;
    if (enabled != e->crypto_on)
        return -EPROTO;
    if (!e->crypto_on)
        return 0;
    local_wins = e->key_owned && !e->key_peer;
    err = tbt_crypto_resolve_key(local_wins, e->key, enabled, kid, hello_key,
                                 key);
    if (err)
        return err;
    if (!e->key_owned)
    {
        memcpy(e->key, key, sizeof(e->key));
        e->key_owned = true;
        e->key_peer = true;
        if (tbt_crypto_init(&e->crypto.tx, e->key, e->tx_nonce, 0))
            return -EPROTO;
    }
    else if (memcmp(e->key, key, sizeof(key)))
    {
        return -EPROTO;
    }
    if (!e->rx_ready)
    {
        memcpy(e->rx_nonce, peer_nonce, sizeof(e->rx_nonce));
        if (tbt_crypto_init(&e->crypto.rx, e->key, peer_nonce, 0))
            return -EPROTO;
        e->rx_ready = true;
    }
    else if (memcmp(e->rx_nonce, peer_nonce, sizeof(e->rx_nonce)))
    {
        return -EPROTO;
    }
    return 0;
}

static int handle_hello(struct engine *e, struct carrier *c,
                        const struct tbt_rec *rec)
{
    u8 want = e->near ? TBT_ROLE_FAR : TBT_ROLE_NEAR;
    int err;

    if (rec->hdr.type != TBT_F_HELLO)
        return -EPROTO;
    if (!tbt_hello_admit(true, rec->body_len, rec->body, want))
        return -EPROTO;
    if ((rec->body[22] & 3) != (e->cfg.cfg.profile & 3))
        return -EPROTO;
    if (tbt_le16(rec->body + 18) != TBT_PAYLOAD_MAX ||
        tbt_le16(rec->body + 20) != TBT_CHUNK_MAX)
        return -EPROTO;
    /* The session nonce and the carrier index are admission checks, not
     * state. Run them before hello_crypto(), which commits the peer key and
     * the RX nonce: a HELLO refused here must leave no trace, or one
     * wrong-nonce connection can poison the far's crypto state for every
     * later HELLO. The far's first HELLO adopts the nonce only after every
     * check has passed. */
    if (e->near)
    {
        if (rec->hdr.path_id != c->index)
            return -EPROTO;
        if (memcmp(rec->body + 2, e->session_nonce, 16))
            return -EPROTO;
    }
    else if (rec->hdr.path_id >= e->n_car ||
             (e->session_nonce_set &&
              memcmp(rec->body + 2, e->session_nonce, 16)))
    {
        return -EPROTO;
    }
    if (hello_crypto(e, rec->body, rec->body_len))
        return -EPROTO;
    if (!e->near)
    {
        u16 index = rec->hdr.path_id;

        if (!e->session_nonce_set)
        {
            memcpy(e->session_nonce, rec->body + 2, 16);
            e->session_nonce_set = true;
        }
        if (e->car[index] && e->car[index] != c)
            carrier_close(e, e->car[index]);
        c->index = (int)index;
        e->car[index] = c;
    }
    c->hello_recv = true;
    if (!c->hello_sent)
    {
        err = carrier_send_hello(e, c);
        if (err)
            return err;
    }
    carrier_up(e, c);
    return 0;
}

static int handle_open(struct engine *e, struct flow *f, const u8 *body,
                       u16 blen, u64 sid)
{
    u16 port;

    if (blen != TBT_OPEN_BODY_LEN)
        return -EPROTO;
    if (e->near)
    {
        if (f)
            f->open_acked = true;
        return 0;
    }
    if (f)
        return 0;
    f = flow_alloc(e, sid, -1, false);
    if (!f)
        return -ENOMEM;
    port = tbt_le16(body + TBT_OPEN_TARGET_PORT);
    if (!port)
        port = tbt_port_host16(e->cfg.cfg.target_port);
    f->open_acked = true;
    return flow_target_connect(f, port);
}

/* The peer's FIN names the packet after its last DATA packet. The target
 * write must stay open until every block up to that frontier is delivered;
 * a FIN on the control carrier can overtake the last DATA block on the data
 * carriers, and shutting down early silently drops that block. */
static void flow_fin_maybe(struct flow *f)
{
    if (!f || f->dead || !f->fin_recv || f->fin_applied)
        return;
    if (f->peer_fin_end_set && f->rx_next_deliver < f->peer_fin_end)
        return;
    if (f->out_len != f->out_off)
        return;
    if (f->app_fd >= 0)
        (void)shutdown(f->app_fd, SHUT_WR);
    f->fin_applied = true;
}

static int handle_fin(struct engine *e, struct flow *f, const u8 *body,
                      u16 blen)
{
    if (!f)
        return 0;
    if (blen >= 8)
    {
        f->peer_fin_end = tbt_le64(body);
        f->peer_fin_end_set = true;
    }
    f->fin_recv = true;
    flow_fin_maybe(f);
    (void)e;
    return 0;
}

static int handle_fec(struct engine *e, struct carrier *c, const u8 *rec,
                      size_t len)
{
    struct tbt_fec_hdr h;
    const u8 *payload;
    u8 clear[TBT_FEC_PAYLOAD_MAX];
    struct flow *f = NULL;
    int i;

    (void)c;
    if (tbt_fec_rec_check(rec, len, TBT_FEC_PAYLOAD_MAX, &h))
    {
        logmsg(e, "tbt_client: fec rec_check failed len=%zu type=%02x\n", len,
               len ? rec[0] : 0);
        return -EPROTO;
    }
    payload = rec + TBT_FEC_HDR_LEN;
    if (e->crypto_on &&
        (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY))
    {
        if (!e->rx_ready)
            return 0;
        if (h.payload_len > sizeof(clear))
            return -EPROTO;
        memcpy(clear, payload, h.payload_len);
        if (tbt_crypto_body_xor_flow(&e->crypto.rx, h.sid, h.seq,
                                     h.type == TBT_FEC_PARITY,
                                     h.share_index, clear, h.payload_len))
            return -EPROTO;
        payload = clear;
    }
    if (tbt_fec_hdr_verify(&h, payload))
    {
        logmsg(e, "tbt_client: fec verify failed type=%u sid=%llu seq=%llu\n",
               h.type, (unsigned long long)h.sid,
               (unsigned long long)h.seq);
        return -EBADMSG;
    }
    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (e->flow[i] && e->flow[i]->sid == h.sid)
            f = e->flow[i];
    if (!f)
    {
        /* A DATA share that outran its OPEN is not repairable here; the
         * sender's NACK/replay path will resend it once the flow exists.
         * Count it so a silent sid change or stream rebuild is visible. */
        if (h.type == TBT_FEC_DATA || h.type == TBT_FEC_PARITY)
            e->rx_unknown_sid++;
        return 0;
    }
    switch (h.type)
    {
    case TBT_FEC_DATA:
    case TBT_FEC_PARITY:
        flow_rx_store(f, &h, payload);
        break;
    case TBT_FEC_CFG:
        if (h.payload_len != TBT_FEC_CFG_LEN)
            return -EPROTO;
        if (handle_cfg(f, payload))
            return -EPROTO;
        break;
    case TBT_FEC_NACK:
        if (h.payload_len != TBT_FEC_NACK_LEN)
            return -EPROTO;
        handle_nack(f, &h, payload);
        break;
    case TBT_FEC_ACK:
    {
        struct tbt_fec_ack a;

        if (h.payload_len != TBT_FEC_ACK_LEN)
            return -EPROTO;
        if (!tbt_fec_ack_unpack(payload, &a))
        {
            if (a.next_deliver > f->tx_ack_next)
                f->tx_ack_next = a.next_deliver;
            f->tx_credit_shares = a.credit_packets;
        }
        break;
    }
    case TBT_FEC_FBK:
        if (h.payload_len != TBT_FEC_FBK_LEN)
            return -EPROTO;
        handle_fbk(f, payload);
        break;
    default:
        return -EPROTO;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Flow callbacks and feedback
 * ------------------------------------------------------------------ */

static int flow_target_connect(struct flow *f, u16 port);

static int handle_cfg(struct flow *f, const u8 *body)
{
    struct engine *e = f->e;
    struct tbt_fec_cfg c;
    struct tbt_fec_sclc_shape sh;
    u16 extra;
    u8 variant;

    if (tbt_fec_cfg_unpack(body, e->n_car, &c))
    {
        logmsg(e, "tbt_client: cfg unpack failed sid=%llu\n",
               (unsigned long long)f->sid);
        return -EPROTO;
    }
    if (!(c.flags & TBT_FEC_CFG_F_INIT))
    {
        logmsg(e, "tbt_client: cfg flags=%u\n", c.flags);
        return -EPROTO;
    }
    if (f->rx_shape_set && f->cfg_epoch == c.epoch)
        return flow_send_fbk(f, TBT_FEC_FBK_F_CFG_ACK, c.epoch);
    if (tbt_fec_sclc_shape_of_cfg(&c, e->n_car, &sh))
    {
        logmsg(e, "tbt_client: cfg shape failed n=%u m=%u groups=%u L=%u block=%u rate=%u\n",
               c.n, c.m, c.groups, c.L, c.block, c.rate_ppm);
        (void)flow_send_fbk(f, TBT_FEC_FBK_F_CFG_NACK, c.epoch);
        return -EPROTO;
    }
    {
        int i;

        for (i = 0; i < TBT_RX_CODECS; i++)
        {
            if (f->rx_codec[i].used)
                tbt_fec_grouped_free(f->rx_codec[i].codec);
            memset(&f->rx_codec[i], 0, sizeof(f->rx_codec[i]));
        }
    }
    logmsg(e, "tbt_client: flow %llu cfg rx n=%u m=%u groups=%u block=%u L=%u\n",
           (unsigned long long)f->sid, sh.n, sh.m, sh.groups, sh.block,
           sh.L);
    f->rx_shape = sh;
    f->rx_shape_set = true;
    f->rx_seed = c.seed;
    f->rx_n = sh.n;
    f->rx_m = sh.m;
    f->rx_groups = sh.groups;
    f->rx_shares = (u16)(sh.n + sh.m);
    (void)flow_rx_credit_update(f);
    f->rx_shape_block = 0;
    f->cfg_epoch = c.epoch;
    if (!flow_rx_codec(f, sh.n, sh.m))
    {
        logmsg(e, "tbt_client: cfg codec build failed n=%u m=%u groups=%u L=%u block=%u\n",
               sh.n, sh.m, sh.groups, sh.L, sh.block);
        (void)flow_send_fbk(f, TBT_FEC_FBK_F_CFG_NACK, c.epoch);
        return -ENOMEM;
    }
    variant = sh.groups > 1 ? TBT_FEC_SCLC_UP : TBT_FEC_SCLC_DOWN;
    extra = sh.groups > 1 ? TBT_FEC_SCLC_WAKE_EXTRA_UP
                          : tbt_fec_sclc_wake_extra_down(sh.n);
    if (e->cfg.cfg.adaptive &&
        !tbt_fec_adapt_sclc_init(&f->ctl, (u16)(sh.n + sh.m), sh.groups,
                                 extra, sh.n, variant))
        f->ctl_ready = true;
    return flow_send_fbk(f, TBT_FEC_FBK_F_CFG_ACK, c.epoch);
}

static void handle_nack(struct flow *f, const struct tbt_fec_hdr *h,
                        const u8 *body)
{
    struct engine *e = f->e;
    struct tbt_fec_nack n;
    struct replay_block *r;
    u16 group, share;

    if (tbt_fec_nack_unpack(body, &n))
        return;
    r = &f->replay[h->block_id % TBT_REPLAY_BLOCKS];
    if (!r->used || r->id != h->block_id)
        return;
    for (group = 0; group < r->groups; group++)
    {
        if (!tbt_fec_bitmap_test(n.group_mask, group))
            continue;
        for (share = 0; share < (u16)(r->n + r->m); share++)
        {
            u32 idx;
            u16 carrier;

            if (!tbt_fec_bitmap_test(n.share_mask, share))
                continue;
            idx = (u32)group * (r->n + r->m) + share;
            if (!r->lens[idx])
                continue;
            carrier = (u16)(group * (r->n + r->m) + share);
            /* A replay is a response to an observed loss, so it bypasses
             * the test-only drop injector. */
            (void)send_record(e, carrier_at(e, carrier),
                              r->recs + (size_t)idx * TBT_FEC_REC_MAX,
                              r->lens[idx], false);
        }
    }
}

static void handle_fbk(struct flow *f, const u8 *body)
{
    struct tbt_fec_fbk fb;

    if (tbt_fec_fbk_unpack(body, &fb))
        return;
    if (fb.flags & TBT_FEC_FBK_F_CFG_ACK)
        f->tx_ready_at_ns = 0;
    if (fb.flags & TBT_FEC_FBK_F_CFG_NACK)
        f->dead = true;
    f->peer_m_target = fb.m_target;
    f->peer_n_target = fb.n_current;
}

static int flow_target_connect(struct flow *f, u16 port)
{
    struct engine *e = f->e;
    struct sockaddr_storage ss;
    socklen_t slen;
    int fd;
    int one = 1;

    if (!tbt_addr_set(&e->cfg.cfg.target))
        return -EINVAL;
    if (addr_to_ss(&e->cfg.cfg.target, tbt_port_be16(port), &ss, &slen))
        return -EAFNOSUPPORT;
    fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    f->app_fd = fd;
    f->ep.kind = EP_FLOW_APP;
    f->ep.p = f;
    if (ep_add(e, fd, EPOLLOUT | EPOLLRDHUP, &f->ep))
    {
        close(fd);
        f->app_fd = -1;
        return -errno;
    }
    if (connect(fd, (struct sockaddr *)&ss, slen) == 0)
    {
        f->tx_ready_at_ns = now_ns() + TBT_OPEN_DELAY_NS;
        return 0;
    }
    if (errno == EINPROGRESS)
    {
        f->target_connecting = true;
        return 0;
    }
    ep_del(e, fd);
    close(fd);
    f->app_fd = -1;
    return -errno;
}

/* Seal the current block. Idle uses finish=false: the short share has no
 * LAST flag and the stream continues in the next block. EOF uses finish=true:
 * the short share is LAST and the FIN follows. An empty block is never
 * emitted; callers check the partial state first. */
static bool flow_tx_seal_block(struct flow *f, bool finish)
{
    u16 g;
    bool had_data = f->tx_cur_len || f->tx_share || f->tx_group;

    if (!had_data)
        return false;
    logmsg(f->e,
           "tbt_client: flow %llu seal%s cur=%zu group=%u share=%u last=%llu\n",
           (unsigned long long)f->sid, finish ? " finish" : "",
           f->tx_cur_len, f->tx_group, f->tx_share,
           (unsigned long long)f->tx_last_len);
    if (f->tx_cur_len)
    {
        u8 *plane = f->tx_plane +
                    ((size_t)f->tx_group * (f->n + f->m) + f->tx_share) *
                        TBT_FEC_S;
        u16 len = (u16)f->tx_cur_len;

        memcpy(plane, f->tx_cur, len);
        (void)flow_send_data(f, f->tx_group, f->tx_share,
                             (u16)f->tx_cur_len, finish);
        f->tx_cur_len = 0;
        f->tx_share++;
    }
    if (f->tx_share == f->n)
    {
        (void)flow_send_parity(f, f->tx_group, f->n, f->tx_last_len);
        f->tx_share = 0;
        f->tx_group++;
    }
    else if (f->tx_share)
    {
        (void)flow_send_parity(f, f->tx_group, f->tx_share,
                               f->tx_last_len);
        f->tx_share = 0;
        f->tx_group++;
    }
    for (g = f->tx_group; g < f->groups; g++)
        (void)flow_send_marker(f, g);
    f->tx_group = 0;
    f->tx_share = 0;
    f->tx_cur_len = 0;
    f->tx_last_len = 0;
    f->tx_block++;
    f->tx_blk_seq0 += (u64)f->groups * f->n;
    f->tx_activity_ns = 0;
    /* The parity covers the whole plane, so an unused share from the old
     * block would make it agree with bytes the peer never received. */
    memset(f->tx_plane, 0,
           (size_t)f->groups * (f->n + f->m) * TBT_FEC_S);
    return true;
}

static void flow_tx_finish(struct flow *f)
{
    if (f->tx_finished)
        return;
    logmsg(f->e, "tbt_client: flow %llu finish cur=%zu group=%u share=%u last=%llu\n",
           (unsigned long long)f->sid, f->tx_cur_len, f->tx_group,
           f->tx_share, (unsigned long long)f->tx_last_len);
    (void)flow_tx_seal_block(f, true);
    f->tx_finished = true;
}

static int flow_tx_ready(struct flow *f, u64 now)
{
    if (f->dead || f->app_fd < 0 || !f->open_sent)
        return 0;
    if (now < f->tx_ready_at_ns)
        return 0;
    if (f->near && !f->open_acked)
        return 0;
    return 1;
}

/* The peer's ACK credit is in shares. One block is groups*(n+m) shares and
 * groups*n DATA packets; convert it to a packet window over the ACK
 * frontier. The local window is additionally capped at four blocks: that cap
 * is a deliberate conservative bound for this reference client (bounded
 * userspace buffers, a short loopback, and no kernel qdisc), not a copy of
 * the module's policy, which lets the peer grant up to K-2 blocks. The
 * unacknowledged block count is bounded separately by the replay ring in
 * flow_tx_block_credit_ok(), and the exact frontier here needs no extra
 * block of headroom because the window is a whole number of blocks. A
 * missing grant still allows one block so the first ACK can arrive, and a
 * one-block grant is stop-and-wait. */
static bool flow_tx_credit_ok(const struct flow *f)
{
    u64 block_shares = (u64)f->groups * (f->n + f->m);
    u64 data_block = (u64)f->groups * f->n;
    u64 credit_blocks;
    u64 peer_window;
    u64 window;

    if (!block_shares || !data_block)
        return false;
    credit_blocks = f->tx_credit_shares / block_shares;
    if (!credit_blocks)
        credit_blocks = 1;
    peer_window = credit_blocks * data_block;
    window = peer_window < 4u * data_block ? peer_window : 4u * data_block;
    return f->tx_seq <= f->tx_ack_next + window;
}

/* Block billing, not packet billing: a short block costs one replay slot.
 * The sender may hold at most TBT_REPLAY_BLOCKS-1 unacknowledged blocks,
 * further capped by the peer's credit_blocks. A block that already started
 * is not interrupted here; the caller stops before opening the next one. */
static bool flow_tx_block_credit_ok(const struct flow *f)
{
    u64 block_shares = (u64)f->groups * (f->n + f->m);
    u64 span = (u64)f->groups * f->n;
    u64 ack_blocks = 0;
    u64 outstanding;
    u64 peer_blocks;
    u64 limit = TBT_REPLAY_BLOCKS - 1;

    if (!block_shares || !span)
        return false;
    if (f->tx_ack_next > f->tx_pkt_base)
        ack_blocks = (f->tx_ack_next - f->tx_pkt_base) / span;
    if (ack_blocks > f->tx_block)
        ack_blocks = f->tx_block;
    outstanding = f->tx_block - ack_blocks;
    peer_blocks = f->tx_credit_shares / block_shares;
    if (!peer_blocks)
        peer_blocks = 1;
    if (peer_blocks < limit)
        limit = peer_blocks;
    if (!limit)
        limit = 1;
    return outstanding < limit;
}

/* Close the local application socket without discarding the FEC flow: the
 * peer may still NACK a block this side sent. The replay ring lives until
 * the ACK frontier or the close timeout. */
static void flow_app_close(struct flow *f)
{
    struct engine *e = f->e;

    if (f->app_fd >= 0)
    {
        ep_del(e, f->app_fd);
        close(f->app_fd);
        f->app_fd = -1;
    }
    f->app_eof = true;
    flow_tx_finish(f);
    (void)flow_send_fin(f);
}

static void flow_app_read(struct flow *f)
{
    struct engine *e = f->e;

    if (f->dead || f->app_fd < 0 || f->app_eof)
        return;
    for (;;)
    {
        ssize_t n;

        if (!flow_tx_credit_ok(f) || !flow_tx_block_credit_ok(f))
            return;
        n = read(f->app_fd, f->rbuf, sizeof(f->rbuf));

        if (n > 0)
        {
            if (flow_tx_feed(f, f->rbuf, (size_t)n))
            {
                (void)flow_send_rst(f);
                flow_free(e, f);
                return;
            }
            continue;
        }
        if (n == 0)
        {
            if (!f->app_eof)
            {
                f->app_eof = true;
                logmsg(e,
                       "tbt_client: flow %llu app eof cur=%zu group=%u share=%u\n",
                       (unsigned long long)f->sid, f->tx_cur_len,
                       f->tx_group, f->tx_share);
                flow_tx_finish(f);
                (void)flow_send_fin(f);
            }
            (void)ep_mod(e, f->app_fd, EPOLLOUT, &f->ep);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        flow_app_close(f);
        return;
    }
}

static void flow_target_event(struct flow *f, uint32_t events)
{
    struct engine *e = f->e;

    if (f->target_connecting && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
    {
        int soerr = 0;
        socklen_t slen = sizeof(soerr);

        if (getsockopt(f->app_fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) ||
            soerr)
        {
            flow_free(e, f);
            return;
        }
        f->target_connecting = false;
        f->tx_ready_at_ns = now_ns() + TBT_OPEN_DELAY_NS;
        (void)ep_mod(e, f->app_fd, EPOLLOUT | EPOLLRDHUP, &f->ep);
        return;
    }
    if (events & EPOLLOUT)
        flow_out_flush(f);
    if (events & (EPOLLIN | EPOLLRDHUP))
        flow_app_read(f);
    if (events & (EPOLLERR | EPOLLHUP))
    {
        if (f->target_connecting)
            flow_free(e, f);
        else
            flow_app_close(f);
    }
}

static void flow_tick(struct flow *f, u64 now)
{
    struct engine *e = f->e;

    if (f->dead)
        return;
    if (!f->open_sent && e->up_count && (e->near || f->app_fd >= 0))
    {
        (void)flow_send_open(f, !e->near);
        (void)flow_send_cfg(f);
    }
    if (flow_tx_ready(f, now))
    {
        if (f->target_connecting)
            ;
        else if (f->app_fd >= 0 && !f->app_eof)
        {
            (void)ep_mod(e, f->app_fd, EPOLLIN | EPOLLRDHUP, &f->ep);
            if (f->out_len > f->out_off)
                (void)ep_mod(e, f->app_fd,
                             EPOLLIN | EPOLLOUT | EPOLLRDHUP, &f->ep);
        }
    }
    if (f->rx_shape_set)
    {
        flow_rx_nack(f, now);
        if (now - f->rx_last_ack_ns >= TBT_ACK_NS)
            (void)flow_send_ack(f);
        if (now - f->last_fbk_ns >= TBT_FBK_NS)
            (void)flow_send_fbk(f, 0, 1);
    }
    flow_out_flush(f);
    flow_fin_maybe(f);
    if (!f->tx_finished && f->tx_activity_ns &&
        now - f->tx_activity_ns >= TBT_TX_IDLE_SEAL_NS &&
        (f->tx_cur_len || f->tx_share || f->tx_group) &&
        flow_tx_block_credit_ok(f))
    {
        if (flow_tx_seal_block(f, false))
            logmsg(e,
                   "tbt_client: flow %llu idle seal tx_seq=%llu block=%llu\n",
                   (unsigned long long)f->sid,
                   (unsigned long long)f->tx_seq,
                   (unsigned long long)f->tx_block);
    }
    if (f->rx_shape_set)
    {
        u32 hs = (u32)(f->rx_next_block % TBT_RX_SLOTS);
        struct rx_block *hb = &f->rx[hs];

        if (hb->used && hb->id == f->rx_next_block)
        {
            u16 g;
            bool ready = true;

            for (g = 0; g < hb->groups; g++)
                if (!hb->g[g].decoded && !hb->g[g].marker)
                {
                    ready = false;
                    break;
                }
            if (!ready && now - hb->first_ns >= 200000000ull &&
                !hb->stall_reported)
            {
                hb->stall_reported = true;
                f->rx_block_stalled++;
                flow_rx_dump_stall(f, hb, now);
            }
        }
    }
    if (e->cfg.verbose && now - f->last_stat_ns >= 1000000000ull)
    {
        u64 rx_block_shares = (u64)f->rx_groups * (f->rx_n + f->rx_m);
        u32 rx_credit_blocks =
            rx_block_shares ? (u32)(f->rx_credit_shares / rx_block_shares) : 0;

        f->last_stat_ns = now;
        logmsg(e,
               "tbt_client: flow %llu near=%d tx_seq=%llu ack_next=%llu "
               "rx_next=%llu rx_bytes=%llu rx_used=%u rx_late=%llu "
               "rx_win_drop=%llu rx_stale=%llu rx_inflight_bad=%llu "
               "rx_stalled=%llu rx_shape_mismatch=%llu "
               "rx_credit_shares=%u rx_credit_blocks=%u "
               "rx_credit_bad=%llu rx_unknown_sid=%llu out=%zu/%zu up=%u\n",
               (unsigned long long)f->sid, f->near,
               (unsigned long long)f->tx_seq,
               (unsigned long long)f->tx_ack_next,
               (unsigned long long)f->rx_next_block,
               (unsigned long long)f->rx_deliver_bytes, f->rx_used,
               (unsigned long long)f->rx_late_share,
               (unsigned long long)f->rx_window_drop,
               (unsigned long long)f->rx_stale_nack,
               (unsigned long long)f->rx_inflight_bad,
               (unsigned long long)f->rx_block_stalled,
               (unsigned long long)f->rx_shape_mismatch,
               f->rx_credit_shares, rx_credit_blocks,
               (unsigned long long)f->rx_credit_bad,
               (unsigned long long)e->rx_unknown_sid, f->out_len,
               f->out_off, e->up_count);
    }
    if (f->fin_sent && f->fin_applied)
    {
        /* Keep the replay ring alive until the peer acknowledged every byte
         * this side sent. A FIN on the control carrier can arrive before the
         * last DATA block, and a NACK for that block must still find it. The
         * timeout is the liveness bound when the final ACK was lost. */
        if (!f->close_at_ns)
            f->close_at_ns = now + 10ull * 1000 * 1000 * 1000;
        if (f->tx_ack_next >= f->tx_seq || now >= f->close_at_ns)
            flow_free(e, f);
    }
}

/* ------------------------------------------------------------------ *
 * Record dispatch and carrier reads
 * ------------------------------------------------------------------ */

static struct flow *flow_find(struct engine *e, u64 sid)
{
    int i;

    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (e->flow[i] && e->flow[i]->sid == sid)
            return e->flow[i];
    return NULL;
}

static int handle_record(struct engine *e, struct carrier *c, const u8 *buf,
                         size_t len)
{
    struct tbt_rec rec;
    int err = 0;

    if (len && tbt_fec_record_type(buf[0]))
        return handle_fec(e, c, buf, len);
    if (!tbt_rec_scan(buf, len, &rec, &err) || err != TBT_REC_OK)
        return -EPROTO;
    switch (rec.hdr.type)
    {
    case TBT_F_HELLO:
        return handle_hello(e, c, &rec);
    case TBT_F_OPEN:
        return handle_open(e, flow_find(e, tbt_hdr_sid(&rec.hdr)), rec.body,
                           rec.body_len, tbt_hdr_sid(&rec.hdr));
    case TBT_F_FIN:
        return handle_fin(e, flow_find(e, tbt_hdr_sid(&rec.hdr)), rec.body,
                          rec.body_len);
    case TBT_F_RST:
    {
        struct flow *f = flow_find(e, tbt_hdr_sid(&rec.hdr));

        if (f)
            flow_free(e, f);
        return 0;
    }
    case TBT_F_PAD:
        return 0;
    default:
        return -EPROTO;
    }
}

static int carrier_process(struct engine *e, struct carrier *c)
{
    size_t off = 0;

    while (off < c->rlen)
    {
        if (!c->http_hdr_done)
        {
            int err = TBT_HTTP_NEED_MORE;
            size_t hl;

            if (e->near)
            {
                struct tbt_http_resp resp;

                hl = tbt_http_resp_parse((const char *)c->rbuf + off,
                                         c->rlen - off, &resp, &err);
            }
            else
            {
                struct tbt_http_req req;

                hl = tbt_http_req_parse((const char *)c->rbuf + off,
                                        c->rlen - off, &req, &err);
            }
            if (err == TBT_HTTP_NEED_MORE)
                break;
            if (err != TBT_HTTP_OK || !hl)
                return -EPROTO;
            c->http_hdr_done = true;
            off += hl;
            if (!e->near)
            {
                int werr = carrier_send_http_header(e, c);

                if (werr)
                    return werr;
            }
            continue;
        }
        {
            const u8 *rec = NULL;
            size_t rec_len = 0;
            int err = TBT_RX_NEED_MORE;
            size_t used;

            used = tbt_http_fec_frame_decode(&c->rx, c->rbuf + off,
                                             c->rlen - off, &rec, &rec_len,
                                             &err);
            off += used;
            if (err == TBT_RX_OK && rec)
            {
                c->rx_records++;
                if (handle_record(e, c, rec, rec_len))
                {
                    logmsg(e,
                           "tbt_client: carrier %d record failed type=%02x len=%zu\n",
                           c->index, rec[0], rec_len);
                    return -EPROTO;
                }
                if (c->dead)
                    return 0;
                continue;
            }
            if (err == TBT_RX_NEED_MORE)
                break;
            return -EPROTO;
        }
    }
    if (off)
    {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
    return 0;
}

static void carrier_read(struct engine *e, struct carrier *c)
{
    for (;;)
    {
        ssize_t n;

        if (c->rlen == sizeof(c->rbuf))
        {
            carrier_close(e, c);
            return;
        }
        n = read(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen);
        if (n > 0)
        {
            c->rlen += (size_t)n;
            c->last_rx_ns = now_ns();
            if (carrier_process(e, c))
            {
                logmsg(e, "tbt_client: carrier %d process failed rlen=%zu\n",
                       c->index, c->rlen);
                carrier_close(e, c);
                return;
            }
            continue;
        }
        if (n == 0)
        {
            carrier_close(e, c);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        carrier_close(e, c);
        return;
    }
}

static void carrier_event(struct engine *e, struct carrier *c,
                          uint32_t events)
{
    if (c->dead)
        return;
    if (c->connecting && (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
    {
        int soerr = 0;
        socklen_t slen = sizeof(soerr);

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) || soerr)
        {
            carrier_close(e, c);
            return;
        }
        c->connecting = false;
        if (carrier_connected(e, c))
        {
            carrier_close(e, c);
            return;
        }
    }
    if (events & EPOLLOUT)
        (void)carrier_flush(c);
    if (events & (EPOLLIN | EPOLLRDHUP))
        carrier_read(e, c);
    if (!c->dead && (events & (EPOLLERR | EPOLLHUP)))
        carrier_close(e, c);
}

/* ------------------------------------------------------------------ *
 * Accept paths and the event loop
 * ------------------------------------------------------------------ */

static void engine_accept_app(struct engine *e)
{
    for (;;)
    {
        int fd = accept4(e->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        struct flow *f;
        int one = 1;

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        f = flow_alloc(e, e->next_sid++, fd, true);
        if (!f)
        {
            close(fd);
            continue;
        }
        if (ep_add(e, fd, EPOLLRDHUP, &f->ep))
        {
            flow_free(e, f);
            continue;
        }
        logmsg(e, "tbt_client: flow %llu accepted\n",
               (unsigned long long)f->sid);
    }
}

static void engine_accept_carrier(struct engine *e)
{
    for (;;)
    {
        int fd = accept4(e->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        struct carrier *c;

        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }
        set_carrier_sockopts(fd);
        c = carrier_new(e, fd, -1, false);
        if (!c)
        {
            close(fd);
            continue;
        }
        c->connected = true;
        c->connect_at_ns = now_ns();
        if (ep_add(e, fd, EPOLLIN | EPOLLRDHUP, &c->ep))
            carrier_free(e, c);
    }
}

static void engine_tick(struct engine *e)
{
    u64 now = now_ns();
    int i;

    pending_flush(e);
    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (e->flow[i])
            flow_tick(e->flow[i], now);
    if (e->near)
    {
        for (i = 0; i < e->n_car; i++)
        {
            struct carrier *c = e->car[i];

            if (!c || !c->dead || now < c->retry_at_ns)
                continue;
            if (carrier_connect(e, c))
                carrier_close(e, c);
        }
    }
    {
        /* Free far-side carriers that ended since the last batch. A retained
         * near carrier stays for its retry. Running this here, after the
         * epoll batch, is what makes the free safe. */
        struct carrier *c = e->car_list;

        while (c)
        {
            struct carrier *next = c->next;

            if (c->dead && !c->retained)
                carrier_free(e, c);
            c = next;
        }
    }
    if (e->stop_at_ns && now >= e->stop_at_ns)
        e->running = false;
}

static void engine_init_session_nonce(struct engine *e)
{
    if (e->cfg.cfg.nonce_set)
    {
        memcpy(e->session_nonce, e->cfg.cfg.nonce, sizeof(e->session_nonce));
        e->session_nonce_set = true;
    }
    else if (e->near)
    {
        fill_random(e, e->session_nonce, sizeof(e->session_nonce));
        e->session_nonce_set = true;
    }
}

static int engine_listen(struct engine *e)
{
    struct sockaddr_storage ss;
    socklen_t slen;
    int fd;
    int one = 1;
    int backlog = e->near ? 16 : 512;

    if (addr_to_ss(&e->cfg.cfg.listen, e->cfg.cfg.listen_port, &ss, &slen))
    {
        fprintf(stderr, "tbt_client: listen address is not set\n");
        return -EINVAL;
    }
    fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&ss, slen) || listen(fd, backlog))
    {
        int err = -errno;

        close(fd);
        return err;
    }
    e->listen_fd = fd;
    e->listen_ep.kind = e->near ? EP_LISTEN_APP : EP_LISTEN_CARRIER;
    e->listen_ep.p = e;
    if (ep_add(e, fd, EPOLLIN, &e->listen_ep))
    {
        close(fd);
        e->listen_fd = -1;
        return -errno;
    }
    return 0;
}

static int engine_start_carriers(struct engine *e)
{
    u16 i;

    for (i = 0; i < e->n_car; i++)
    {
        struct carrier *c = carrier_new(e, -1, i, true);

        if (!c)
            return -ENOMEM;
        e->car[i] = c;
        if (carrier_connect(e, c))
            carrier_close(e, c);
    }
    return 0;
}

static void engine_cleanup(struct engine *e)
{
    int i;

    for (i = 0; i < TBT_FLOW_MAX; i++)
        if (e->flow[i])
            flow_free(e, e->flow[i]);
    while (e->car_list)
    {
        struct carrier *c = e->car_list;

        carrier_free(e, c);
    }
    if (e->listen_fd >= 0)
    {
        ep_del(e, e->listen_fd);
        close(e->listen_fd);
    }
    if (e->timerfd >= 0)
        close(e->timerfd);
    if (e->epfd >= 0)
        close(e->epfd);
    tbt_crypto_pair_wipe(&e->crypto);
}

int tbt_engine_run(const struct tbt_engine_cfg *cfg)
{
    struct engine *e;
    struct itimerspec its;
    int err = 0;

    if (!cfg)
        return -EINVAL;
    if (cfg->cfg.role != TBT_ROLE_NEAR && cfg->cfg.role != TBT_ROLE_FAR)
        return -EINVAL;
    if (!cfg->cfg.carriers || cfg->cfg.carriers > TBT_CAR_MAX)
        return -EINVAL;
    if (cfg->cfg.mode != 3)
    {
        fprintf(stderr,
                "tbt_client: only mode=sc-ldpc is implemented in the userspace client\n");
        return -EINVAL;
    }
    e = calloc(1, sizeof(*e));
    if (!e)
        return -ENOMEM;
    e->cfg = *cfg;
    e->near = cfg->cfg.role == TBT_ROLE_NEAR;
    e->n_car = cfg->cfg.carriers;
    e->drop_ppm = cfg->drop_ppm > TBT_DROP_PPM_MAX ? TBT_DROP_PPM_MAX
                                                   : cfg->drop_ppm;
    e->listen_fd = -1;
    e->epfd = -1;
    e->timerfd = -1;
    e->next_sid = 1;
    e->start_ns = now_ns();
    if (cfg->run_ms)
        e->stop_at_ns = e->start_ns + (u64)cfg->run_ms * 1000000ull;
    signal(SIGPIPE, SIG_IGN);
    rng_seed(e);

    if (cfg->cfg.carriers != 256)
    {
        fprintf(stderr,
                "tbt_client: the 0.8 SC-LDPC client needs carriers=256 (8x32 up, 1x256 down)\n");
        free(e);
        return -EINVAL;
    }
    if (tbt_fec_sclc_shape_of(8, 24, 256, TBT_FEC_SCLC_BLOCK_UP,
                              TBT_FEC_SCLC_L_UP, &e->up_shape))
    {
        fprintf(stderr, "tbt_client: cannot build the 8x32 uplink shape\n");
        free(e);
        return -EINVAL;
    }
    {
        struct tbt_fec_sclc_shape cfg_shape;

        if (tbt_fec_sclc_shape_of(cfg->cfg.n, cfg->cfg.m, cfg->cfg.carriers,
                                  cfg->cfg.w, cfg->cfg.L, &cfg_shape))
        {
            fprintf(stderr,
                    "tbt_client: not an SC-LDPC shape: n=%u m=%u carriers=%u\n",
                    cfg->cfg.n, cfg->cfg.m, cfg->cfg.carriers);
            free(e);
            return -EINVAL;
        }
        if (cfg_shape.variant == TBT_FEC_SCLC_DOWN)
            e->down_shape = cfg_shape;
        else if (tbt_fec_sclc_shape_of(128, 128, 256, 256, 8,
                                       &e->down_shape))
        {
            fprintf(stderr, "tbt_client: cannot build the 1x256 downlink shape\n");
            free(e);
            return -EINVAL;
        }
        if (!e->near && cfg_shape.variant != TBT_FEC_SCLC_DOWN)
        {
            fprintf(stderr,
                    "tbt_client: far role needs a downlink n/m (1x256), got n=%u m=%u\n",
                    cfg->cfg.n, cfg->cfg.m);
            free(e);
            return -EINVAL;
        }
    }
    e->tx_shape = e->near ? e->up_shape : e->down_shape;
    e->tx_n = e->tx_shape.n;
    e->tx_m = e->tx_shape.m;
    e->tx_groups = e->tx_shape.groups;
    e->tx_seed = rng_next(e) | 1;

    e->crypto_on = cfg->cfg.crypto != 0;
    if (e->crypto_on)
    {
        if (cfg->cfg.crypto_key_set)
        {
            memcpy(e->key, cfg->cfg.crypto_key, sizeof(e->key));
            e->key_owned = true;
            e->key_id = TBT_CRYPTO_KEY_ID_NONE;
        }
        else if (e->near)
        {
            fill_random(e, e->key, sizeof(e->key));
            e->key_owned = true;
            e->key_id = TBT_CRYPTO_KEY_ID_HELLO;
        }
        fill_random(e, e->tx_nonce, sizeof(e->tx_nonce));
        if (e->key_owned &&
            tbt_crypto_init(&e->crypto.tx, e->key, e->tx_nonce, 0))
        {
            free(e);
            return -EINVAL;
        }
    }
    engine_init_session_nonce(e);

    e->epfd = epoll_create1(EPOLL_CLOEXEC);
    e->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (e->epfd < 0 || e->timerfd < 0)
    {
        err = -errno;
        engine_cleanup(e);
        free(e);
        return err;
    }
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 5000000;
    its.it_interval.tv_nsec = 5000000;
    timerfd_settime(e->timerfd, 0, &its, NULL);
    e->timer_ep.kind = EP_TIMER;
    e->timer_ep.p = e;
    ep_add(e, e->timerfd, EPOLLIN, &e->timer_ep);

    err = engine_listen(e);
    if (err)
    {
        fprintf(stderr, "tbt_client: listen: %s\n", strerror(-err));
        engine_cleanup(e);
        free(e);
        return err;
    }
    if (e->near)
    {
        err = engine_start_carriers(e);
        if (err)
        {
            engine_cleanup(e);
            free(e);
            return err;
        }
    }
    logmsg(e,
           "tbt_client: role=%s carriers=%u down_tx n=%u m=%u groups=%u block=%u L=%u up_tx n=%u m=%u groups=%u block=%u L=%u crypto=%d drop_ppm=%u\n",
           e->near ? "near" : "far", e->n_car, e->down_shape.n,
           e->down_shape.m, e->down_shape.groups, e->down_shape.block,
           e->down_shape.L, e->up_shape.n, e->up_shape.m,
           e->up_shape.groups, e->up_shape.block, e->up_shape.L,
           e->crypto_on, e->drop_ppm);

    e->running = true;
    while (e->running)
    {
        struct epoll_event evs[64];
        int n = epoll_wait(e->epfd, evs, 64, 5);
        int i;

        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            err = -errno;
            break;
        }
        for (i = 0; i < n; i++)
        {
            struct ep_src *src = evs[i].data.ptr;
            uint32_t events = evs[i].events;

            if (!src)
                continue;
            switch (src->kind)
            {
            case EP_CARRIER:
                carrier_event(e, src->p, events);
                break;
            case EP_LISTEN_APP:
                engine_accept_app(e);
                break;
            case EP_LISTEN_CARRIER:
                engine_accept_carrier(e);
                break;
            case EP_FLOW_APP:
                flow_target_event(src->p, events);
                break;
            case EP_TIMER:
            {
                u64 ticks;
                ssize_t got;
                int rerr;

                do
                    got = read(e->timerfd, &ticks, sizeof(ticks));
                while (got < 0 && errno == EINTR);
                rerr = errno;
                if (got < 0)
                {
                    if (rerr != EAGAIN && rerr != EWOULDBLOCK)
                        logmsg(e, "tbt_client: timerfd read: %s\n",
                               strerror(rerr));
                }
                else if ((size_t)got != sizeof(ticks))
                {
                    logmsg(e, "tbt_client: timerfd short read %zd\n", got);
                }
                break;
            }
            default:
                break;
            }
        }
        engine_tick(e);
    }
    if (e->cfg.verbose)
        fprintf(stderr,
                "tbt_client: stop up=%u drops=%llu flows=%d\n", e->up_count,
                (unsigned long long)e->drops,
                e->flow[0] ? 1 : 0);
    engine_cleanup(e);
    free(e);
    return err;
}
