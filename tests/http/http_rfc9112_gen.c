/* http_rfc9112_gen.c - drive the real tbt_http.c encoder over many
 * keep-alive cycles and write one stream per carrier direction.
 *
 * This is a test generator, not a second encoder: every HTTP header, every
 * chunk size line, every record frame and every probe comes from tbt_http.c.
 * The generator only decides the sequence (ARQ HELLO/OPEN/DATA, a three-record
 * FEC batch, PAD/drip probes, terminating 0-chunk) and the rotation of the
 * request id, host and date.
 *
 * Output layout, consumed by tests/http_rfc9112_audit.py:
 *
 *   manifest.txt
 *   up_cN.bin      near -> far: request header + one chunked body per round
 *   down_cN.bin    far -> near: response header + one chunked body per round
 *
 * Each carrier file is one TCP connection kept alive across every round, so
 * the audit parser must consume the whole byte stream with no gap, no repeated
 * message boundary and no trailing byte.
 *
 * usage: http_rfc9112_gen OUTDIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_http.h"

/* Six carrier connections, the production N+M = 4+2 shape: each is its own
 * keep-alive HTTP connection. The audit proves each connection carries several
 * complete request/response cycles and that the six rotate together. */
#define GEN_CARRIERS 6
#define GEN_ROUNDS 3
#define GEN_FEC_N 4
#define GEN_FEC_M 2
#define GEN_FEC_DATA_PAYLOAD 64
#define GEN_FEC_PARITY_PAYLOAD TBT_FEC_PAYLOAD_MAX
#define GEN_FEC_BATCH 3

static int emit_fail(const char *what)
{
    fprintf(stderr, "http_rfc9112_gen: %s\n", what);
    return 1;
}

static int put_bytes(FILE *f, const void *p, size_t n)
{
    if (!n)
        return 0;
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

/* 26 characters from [a-z2-7]. The fixed prefix keeps the value readable in a
 * capture; the six trailing base32 digits make it unique per carrier/round. */
static void make_reqid(char out[TBT_HTTP_REQID_LEN + 1], unsigned carrier,
                       unsigned round)
{
    static const char b32[] = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned v = carrier * GEN_ROUNDS + round + 1;
    unsigned i;

    memset(out, 'a', TBT_HTTP_REQID_LEN);
    for (i = 0; i < 6; i++)
        out[TBT_HTTP_REQID_LEN - 6 + i] =
            b32[(v >> (5 * (5 - i))) & 31u];
    out[TBT_HTTP_REQID_LEN] = '\0';
}

static int arq_emit(FILE *f, unsigned carrier, u64 sid, unsigned type,
                    u64 seq, const u8 *body, size_t body_len)
{
    struct tbt_hdr h = {0};
    struct tbt_http_out out;
    char chunk[TBT_HTTP_FRAME_MAX];
    size_t n;

    h.type = (u8)type;
    h.flags = type == TBT_F_DATA ? TBT_FF_HEAD_OF_RUN : 0;
    h.path_id = (u8)carrier;
    h.body_len = (u16)body_len;
    h.seq = seq;
    tbt_hdr_set_sid(&h, sid);
    h.conv = TBT_CONV;
    if (tbt_hdr_check(&h))
        return emit_fail("ARQ header rejected by tbt_hdr_check");
    out.hdr = &h;
    out.body = body;
    n = tbt_http_frame_encode(chunk, sizeof(chunk), &out);
    if (!n)
        return emit_fail("tbt_http_frame_encode returned 0");
    return put_bytes(f, chunk, n);
}

static int fec_emit(FILE *f, unsigned carrier, unsigned round, unsigned type,
                    u16 share, u16 payload_len)
{
    u8 payload[TBT_FEC_PAYLOAD_MAX];
    u8 rec[TBT_FEC_REC_MAX];
    struct tbt_fec_hdr h = {0};
    char chunk[TBT_HTTP_FEC_FRAME_MAX];
    size_t rec_len, n, i;

    if ((size_t)payload_len > sizeof(payload))
        return emit_fail("FEC payload length out of range");
    for (i = 0; i < (size_t)payload_len; i++)
        payload[i] = (u8)(carrier * 17u + round * 31u + type * 7u + i);

    h.type = (u8)type;
    h.share_index = share;
    h.n = GEN_FEC_N;
    h.m = GEN_FEC_M;
    h.block_id = round + 1;
    h.seq = (u64)round * 100u + share;
    h.win_len = type == TBT_FEC_PARITY ? GEN_FEC_N : 0;
    h.payload_len = payload_len;
    h.conv = TBT_FEC_CONV;
    h.sid = 0x510000u + carrier;

    if (tbt_fec_hdr_check(&h, TBT_FEC_PAYLOAD_MAX))
        return emit_fail("FEC header rejected by tbt_fec_hdr_check");
    if (tbt_fec_hdr_seal(&h, payload))
        return emit_fail("tbt_fec_hdr_seal failed");
    if (tbt_fec_hdr_verify(&h, payload))
        return emit_fail("tbt_fec_hdr_verify failed");
    if (tbt_fec_hdr_pack(&h, rec))
        return emit_fail("tbt_fec_hdr_pack failed");
    memcpy(rec + TBT_FEC_HDR_LEN, payload, payload_len);
    rec_len = TBT_FEC_HDR_LEN + payload_len;

    n = tbt_http_fec_chunk_encode(chunk, sizeof(chunk), rec, rec_len);
    if (!n)
        return emit_fail("tbt_http_fec_chunk_encode returned 0");
    return put_bytes(f, chunk, n);
}

static int fec_batch_emit(FILE *f, unsigned carrier, unsigned round)
{
    if (fec_emit(f, carrier, round, TBT_FEC_DATA, 0, GEN_FEC_DATA_PAYLOAD) ||
        fec_emit(f, carrier, round, TBT_FEC_DATA, 1, GEN_FEC_DATA_PAYLOAD) ||
        fec_emit(f, carrier, round, TBT_FEC_PARITY, GEN_FEC_N,
                 GEN_FEC_PARITY_PAYLOAD))
        return -1;
    return 0;
}

static int probes_emit(FILE *f, unsigned round)
{
    char chunk[TBT_HTTP_PAD_CHUNK_LEN];
    size_t n;

    if (round == 0 || round == 2)
    {
        n = tbt_http_pad_chunk(chunk, sizeof(chunk));
        if (n != TBT_HTTP_PAD_CHUNK_LEN)
            return emit_fail("tbt_http_pad_chunk did not emit 38 bytes");
        if (put_bytes(f, chunk, n))
            return -1;
    }
    if (round == 1 || round == 2)
    {
        n = tbt_http_drip_chunk(chunk, sizeof(chunk));
        if (n != TBT_HTTP_DRIP_CHUNK_LEN)
            return emit_fail("tbt_http_drip_chunk did not emit 6 bytes");
        if (put_bytes(f, chunk, n))
            return -1;
    }
    return 0;
}

static int terminator_emit(FILE *f)
{
    char chunk[TBT_HTTP_CHUNK_HDR_MAX];
    size_t n;

    n = tbt_http_chunk_hdr(chunk, sizeof(chunk), 0);
    if (!n)
        return emit_fail("tbt_http_chunk_hdr(0) returned 0");
    if (put_bytes(f, chunk, n))
        return -1;
    n = tbt_http_chunk_tail(chunk, sizeof(chunk));
    if (n != 2)
        return emit_fail("tbt_http_chunk_tail returned 0");
    return put_bytes(f, chunk, n);
}

static int hello_emit(FILE *f, unsigned carrier, u64 sid, unsigned role)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    unsigned i;

    memset(body, 0, sizeof(body));
    body[0] = TBT_WIRE_VERSION_V6;
    body[1] = (u8)role;
    for (i = 0; i < 16; i++)
        body[2 + i] = (u8)(0x40u + carrier + i);
    body[18] = (u8)TBT_PAYLOAD_MAX;
    body[19] = (u8)(TBT_PAYLOAD_MAX >> 8);
    body[20] = (u8)TBT_CHUNK_MAX;
    body[21] = (u8)(TBT_CHUNK_MAX >> 8);
    body[22] = 1; /* http-kal */
    return arq_emit(f, carrier, sid, TBT_F_HELLO, 0, body, sizeof(body));
}

static int open_emit(FILE *f, unsigned carrier, u64 sid, u64 seq)
{
    u8 body[TBT_OPEN_BODY_LEN];

    memset(body, 0, sizeof(body));
    body[carrier / 8] |= (u8)(1u << (carrier % 8));
    return arq_emit(f, carrier, sid, TBT_F_OPEN, seq, body, sizeof(body));
}

static int data_emit(FILE *f, unsigned carrier, u64 sid, u64 seq)
{
    u8 body[37];
    unsigned i;

    for (i = 0; i < sizeof(body); i++)
        body[i] = (u8)(0x80u + carrier * 9u + seq + i);
    return arq_emit(f, carrier, sid, TBT_F_DATA, seq, body, sizeof(body));
}

static int message_emit(FILE *f, int request, unsigned carrier, unsigned round,
                        const char *host, const char *reqid, u64 date, u64 sid)
{
    char hdr[TBT_HTTP_REQ_MAX];
    size_t n;

    if (request)
    {
        struct tbt_http_cfg cfg;

        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.host, sizeof(cfg.host), "%s", host);
        snprintf(cfg.request_id, sizeof(cfg.request_id), "%s", reqid);
        n = tbt_http_req_header(hdr, sizeof(hdr), &cfg);
    }
    else
    {
        struct tbt_http_resp_cfg cfg = {.unix_secs = date};

        n = tbt_http_resp_header(hdr, sizeof(hdr), &cfg);
    }
    if (!n)
        return emit_fail("HTTP header encoder returned 0");
    if (put_bytes(f, hdr, n))
        return -1;

    if (round == 0 && hello_emit(f, carrier, sid,
                                 request ? TBT_ROLE_NEAR : TBT_ROLE_FAR))
        return -1;
    if (open_emit(f, carrier, sid, 1u + round))
        return -1;
    if (data_emit(f, carrier, sid, 1u + round))
        return -1;
    if (fec_batch_emit(f, carrier, round))
        return -1;
    if (probes_emit(f, round))
        return -1;
    return terminator_emit(f);
}

static int carrier_emit(const char *dir, unsigned carrier, FILE *manifest,
                        unsigned rounds)
{
    char up_path[512], down_path[512];
    char host[TBT_HTTP_HOST_MAX + 1];
    char reqids[GEN_ROUNDS][TBT_HTTP_REQID_LEN + 1];
    u64 dates[GEN_ROUNDS];
    u64 sid = 0x510000u + carrier;
    FILE *up, *down;
    unsigned r;

    if (carrier % 2 == 0)
        snprintf(host, sizeof(host), "203.0.113.%u", 10u + carrier);
    else
        snprintf(host, sizeof(host), "203.0.113.%u:%u", 10u + carrier,
                 8443u + carrier);
    snprintf(up_path, sizeof(up_path), "%s/up_c%u.bin", dir, carrier);
    snprintf(down_path, sizeof(down_path), "%s/down_c%u.bin", dir, carrier);

    up = fopen(up_path, "wb");
    down = fopen(down_path, "wb");
    if (!up || !down)
    {
        if (up)
            fclose(up);
        if (down)
            fclose(down);
        return emit_fail("cannot open a carrier stream");
    }

    for (r = 0; r < rounds; r++)
    {
        make_reqid(reqids[r], carrier, r);
        dates[r] = 1700000000ull + (u64)carrier * 100000ull + (u64)r * 60ull;
        if (message_emit(up, 1, carrier, r, host, reqids[r], dates[r], sid) ||
            message_emit(down, 0, carrier, r, host, reqids[r], dates[r], sid))
        {
            fclose(up);
            fclose(down);
            return -1;
        }
    }
    {
        int up_err = fclose(up);
        int down_err = fclose(down);

        if (up_err || down_err)
            return emit_fail("short write on a carrier stream");
    }

    fprintf(manifest, "carrier %u up_c%u.bin down_c%u.bin %s", carrier, carrier,
            carrier, host);
    for (r = 0; r < rounds; r++)
        fprintf(manifest, " %s", reqids[r]);
    for (r = 0; r < rounds; r++)
        fprintf(manifest, " %llu", (unsigned long long)dates[r]);
    fprintf(manifest, "\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char manifest_path[512];
    FILE *manifest;
    unsigned carrier;

    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", dir);
    manifest = fopen(manifest_path, "wb");
    if (!manifest)
        return emit_fail("cannot open manifest.txt");
    fprintf(manifest, "version 1\ncarriers %u\nrounds %u\n", GEN_CARRIERS,
            GEN_ROUNDS);
    for (carrier = 0; carrier < GEN_CARRIERS; carrier++)
    {
        if (carrier_emit(dir, carrier, manifest, GEN_ROUNDS))
        {
            fclose(manifest);
            return 1;
        }
    }
    if (fclose(manifest))
        return emit_fail("short write on manifest.txt");
    return 0;
}
