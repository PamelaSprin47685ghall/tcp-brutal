/* tbt_client_engine.h - userspace tbtun client session engine.
 *
 * One engine is one tunnel endpoint: a near side that accepts application
 * TCP and dials the carrier pool, or a far side that accepts the carrier
 * pool and connects the configured target. It links the same pure C units
 * the module runs (tbt_parse, tbt_proto, tbt_rec, tbt_http, tbt_fec,
 * tbt_fec_batch, tbt_crypto, tbt_fec_adapt_sclc, tbt_fec_codec_cache and
 * the SC-LDPC codecs), so the wire bytes are produced by the module's own
 * encoders.
 *
 * The engine is single-threaded: one epoll set and one timerfd. It owns no
 * global state, so a test can run a near and a far engine in separate
 * processes without a kernel module.
 *
 * The configuration is the same line grammar as /proc/net/tbtun/config.
 * tbt_client_main.c turns argv into one line and calls tbt_parse_line().
 */
#ifndef _TBT_CLIENT_ENGINE_H
#define _TBT_CLIENT_ENGINE_H

#include "tbt_parse.h"

/* Userspace-only knobs, kept out of the kernel grammar so a line copied
 * from /proc still parses. */
struct tbt_engine_cfg
{
    struct tbt_cfg cfg;
    u32 drop_ppm;  /* drop this share of first-flight DATA/PARITY records */
    u32 run_ms;    /* stop after this long; 0 = run until a fatal error */
    u32 local_queue_bytes; /* app-socket buffer bound; 0 = default */
    int verbose;
};

/* Run one endpoint. Returns 0 when run_ms elapsed or the peer closed every
 * stream and the engine went idle, or a negative errno on a fatal setup
 * error. The engine does not daemonize and does not fork. */
int tbt_engine_run(const struct tbt_engine_cfg *cfg);

#endif /* _TBT_CLIENT_ENGINE_H */
