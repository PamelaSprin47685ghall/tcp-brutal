/* gen_golden.c - deterministic tbtun 0.8.0 golden vectors for an independent
 * F# implementation. Host-only: it links the shipping C units and writes
 * JSON/binary pairs under tests/golden_v080/.
 *
 * Everything here is derived from the real implementation, never hand-copied:
 * the FEC header CRCs, the HTTP chunk framing, ChaCha8, the mux subframes and
 * both SC-LDPC codecs run the same code the module uses.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tbt_crypto.h"
#include "tbt_fec.h"
#include "tbt_fec_batch.h"
#include "tbt_http.h"
#include "tbt_mux.h"
#include "tbt_proto.h"
#include "tbt_sclc_uplink.h"
#include "tbt_uapi.h"

#define OUT "tests/golden_v080"

static void die(const char *what)
{
    fprintf(stderr, "gen_golden: %s\n", what);
    exit(1);
}

static void fill(u8 *p, size_t len, u8 seed)
{
    size_t i;

    for (i = 0; i < len; i++)
        p[i] = (u8)(seed + i * 37u + (i >> 8));
}

static void write_file(const char *name, const void *buf, size_t len)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), OUT "/%s", name);
    f = fopen(path, "wb");
    if (!f)
        die(path);
    if (len && fwrite(buf, 1, len, f) != len)
        die(path);
    if (fclose(f))
        die(path);
}

static FILE *open_json(const char *name)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), OUT "/%s", name);
    f = fopen(path, "w");
    if (!f)
        die(path);
    return f;
}

static void hex(FILE *f, const u8 *p, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
        fprintf(f, "%02x", p[i]);
}

static void crc_vectors(void)
{
    static const u8 nine[] = "123456789";
    u8 buf[1024];
    FILE *f = open_json("crc32c.json");
    u32 c;

    fill(buf, sizeof(buf), 0x5a);
    fprintf(f, "{\n  \"note\": \"CRC32C Castagnoli, reflected, init ~0, final xor. seed=0 for a fresh CRC.\",\n");
    fprintf(f, "  \"check_123456789\": %u,\n", tbt_fec_crc32c(0, nine, 9));
    fprintf(f, "  \"empty\": %u,\n", tbt_fec_crc32c(0, NULL, 0));
    fprintf(f, "  \"pattern1024_seed5a_crc\": %u,\n",
            tbt_fec_crc32c(0, buf, sizeof(buf)));
    fprintf(f, "  \"chained\": {\"first\": %u, \"next\": %u}\n",
            tbt_fec_crc32c(0, nine, 4),
            tbt_fec_crc32c(tbt_fec_crc32c(0, nine, 4), nine + 4, 5));
    fprintf(f, "}\n");
    fclose(f);

    c = tbt_fec_crc32c(0, nine, 9);
    if (c != 0xE3069283u)
        die("CRC32C check value drifted");
}

static void chacha_vectors(void)
{
    u8 key[32], nonce[8], plain[64], cipher[64];
    u8 body_plain[1024], body_cipher[1024];
    struct tbt_crypto_ctx ctx;
    FILE *f;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)i;
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0x20 + i);
    fill(plain, sizeof(plain), 0x40);
    memcpy(cipher, plain, sizeof(cipher));
    if (tbt_chacha8_xor(key, sizeof(key), nonce, 0, TBT_CRYPTO_ROUNDS,
                        cipher, sizeof(cipher)))
        die("chacha8");

    fill(body_plain, sizeof(body_plain), 0x80);
    memcpy(body_cipher, body_plain, sizeof(body_cipher));
    if (tbt_crypto_init(&ctx, key, nonce, 0))
        die("crypto init");
    if (tbt_crypto_body_xor_flow(&ctx, 0x0102030405ULL, 7, false, 0,
                                 body_cipher, sizeof(body_cipher)))
        die("crypto body");

    write_file("chacha8_body_cipher.bin", body_cipher, sizeof(body_cipher));
    f = open_json("chacha8.json");
    fprintf(f, "{\n  \"rounds\": 8,\n  \"key_hex\": \"");
    hex(f, key, sizeof(key));
    fprintf(f, "\",\n  \"nonce8_hex\": \"");
    hex(f, nonce, sizeof(nonce));
    fprintf(f, "\",\n  \"raw_counter\": 0,\n  \"raw_plain_hex\": \"");
    hex(f, plain, sizeof(plain));
    fprintf(f, "\",\n  \"raw_cipher_hex\": \"");
    hex(f, cipher, sizeof(cipher));
    fprintf(f, "\",\n  \"body\": {\"sid\": 4328719365, \"seq\": 7, \"parity\": false, \"share\": 0, \"len\": 1024, ");
    fprintf(f, "\"plain_hex\": \"");
    hex(f, body_plain, sizeof(body_plain));
    fprintf(f, "\",\n    \"cipher_file\": \"chacha8_body_cipher.bin\", \"cipher_crc32c\": %u}\n",
            tbt_fec_crc32c(0, body_cipher, sizeof(body_cipher)));
    fprintf(f, "}\n");
    fclose(f);
}

static void hello_vector(void)
{
    u8 body[TBT_HELLO_BODY_LEN_V6];
    u8 frame[TBT_HDR_LEN + TBT_HELLO_BODY_LEN_V6];
    u8 key[32], nonce8[8];
    struct tbt_hdr h;
    FILE *f;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)(0x20 + i);
    for (i = 0; i < sizeof(nonce8); i++)
        nonce8[i] = (u8)(0x10 + i);

    memset(body, 0, sizeof(body));
    body[0] = TBT_WIRE_VERSION_V6; /* 8 */
    body[1] = TBT_ROLE_NEAR;       /* 2 */
    for (i = 0; i < 16; i++)
        body[2 + i] = (u8)(0xa0 + i);
    tbt_put_le16(body + 18, TBT_PAYLOAD_MAX);   /* 8192 */
    tbt_put_le16(body + 20, TBT_CHUNK_MAX);     /* 8228 */
    body[22] = 1; /* profile=http-kal, generation 0 */
    tbt_crypto_hello_fill(body, true, TBT_CRYPTO_KEY_ID_HELLO, nonce8, key);

    memset(&h, 0, sizeof(h));
    h.type = TBT_F_HELLO;
    h.body_len = sizeof(body);
    h.seq = 1;
    h.conv = TBT_CONV;
    tbt_hdr_set_sid(&h, 0x0102030405ULL);
    if (tbt_hdr_check(&h))
        die("hello header");
    tbt_hdr_encode(frame, &h);
    memcpy(frame + TBT_HDR_LEN, body, sizeof(body));

    write_file("hello_v8_body.bin", body, sizeof(body));
    write_file("hello_v8_frame.bin", frame, sizeof(frame));
    f = open_json("hello_v8.json");
    fprintf(f, "{\n  \"conv\": %u,\n  \"wire_version\": 8,\n  \"body_len\": 66,\n",
            TBT_CONV);
    fprintf(f, "  \"role\": 2,\n  \"session_nonce_hex\": \"");
    hex(f, body + 2, 16);
    fprintf(f, "\",\n  \"payload_max\": %u,\n  \"chunk_max\": %u,\n",
            TBT_PAYLOAD_MAX, TBT_CHUNK_MAX);
    fprintf(f, "  \"profile_gen_byte\": %u,\n  \"crypto\": 3,\n  \"key_id\": 1,\n  \"crypto_nonce8_hex\": \"",
            body[22]);
    hex(f, body + TBT_CRYPTO_HELLO_NONCE, 8);
    fprintf(f, "\",\n  \"key_hex\": \"");
    hex(f, body + TBT_CRYPTO_HELLO_KEY, 32);
    fprintf(f, "\",\n  \"body_file\": \"hello_v8_body.bin\",\n  \"frame_file\": \"hello_v8_frame.bin\",\n  \"body_crc32c\": %u\n}\n",
            tbt_fec_crc32c(0, body, sizeof(body)));
    fclose(f);
}

static void fec_record(u8 *rec, u16 *rec_len, u8 type, u16 share, u64 sid,
                       u16 win_len, u16 epoch, const u8 *payload,
                       u16 payload_len)
{
    struct tbt_fec_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = type;
    h.flags = (type == TBT_FEC_DATA && win_len && win_len < TBT_FEC_S)
                  ? TBT_FEC_FF_PADDED
                  : 0;
    h.share_index = share;
    h.n = 64;
    h.m = 192;
    h.block_id = 1;
    h.seq = type == TBT_FEC_DATA ? share : 0;
    h.win_len = win_len;
    h.payload_len = payload_len;
    h.epoch = epoch;
    h.conv = TBT_FEC_CONV;
    h.sid = sid;
    h.reserved = 0;
    if (tbt_fec_hdr_check(&h, TBT_FEC_S))
    {
        fprintf(stderr, "hdr type=%u flags=%u share=%u n=%u m=%u win=%u plen=%u epoch=%u seq=%llu\n",
                h.type, h.flags, h.share_index, h.n, h.m, h.win_len,
                h.payload_len, h.epoch, (unsigned long long)h.seq);
        die("FEC header check");
    }
    if (tbt_fec_hdr_seal(&h, payload))
        die("FEC seal");
    if (tbt_fec_hdr_pack(&h, rec))
        die("FEC pack");
    memcpy(rec + TBT_FEC_HDR_LEN, payload, payload_len);
    *rec_len = (u16)(TBT_FEC_HDR_LEN + payload_len);
}

static void http_vectors(void)
{
    u8 data[TBT_FEC_S], parity[TBT_FEC_S];
    u8 data_rec[TBT_FEC_REC_MAX], parity_rec[TBT_FEC_REC_MAX];
    char data_chunk[TBT_HTTP_FEC_FRAME_MAX], parity_chunk[TBT_HTTP_FEC_FRAME_MAX];
    u16 data_len, parity_len;
    size_t dc, pc;
    FILE *f;

    memset(data, 0, sizeof(data));
    fill(data, 1000, 0x30); /* 1000 real bytes, 24 zero pad */
    fill(parity, sizeof(parity), 0x90);

    fec_record(data_rec, &data_len, TBT_FEC_DATA, 0, 0x0102030405ULL,
               1000, 0, data, TBT_FEC_S);
    fec_record(parity_rec, &parity_len, TBT_FEC_PARITY, 64, 0x0102030405ULL,
               1, 1000, parity, TBT_FEC_S);
    dc = tbt_http_fec_chunk_encode(data_chunk, sizeof(data_chunk), data_rec,
                                   data_len);
    pc = tbt_http_fec_chunk_encode(parity_chunk, sizeof(parity_chunk),
                                   parity_rec, parity_len);
    if (!dc || !pc)
        die("HTTP chunk encode");
    if (dc != 1079 || pc != 1079)
        die("HTTP chunk is not 1079 bytes");

    write_file("http_padded_data.record.bin", data_rec, data_len);
    write_file("http_padded_data.chunk.bin", data_chunk, dc);
    write_file("http_parity.record.bin", parity_rec, parity_len);
    write_file("http_parity.chunk.bin", parity_chunk, pc);

    f = open_json("http_padded.json");
    fprintf(f, "{\n  \"data\": {\"record_file\": \"http_padded_data.record.bin\", \"record_len\": %u, \"chunk_file\": \"http_padded_data.chunk.bin\", \"chunk_len\": %zu, \"chunk_hex\": \"",
            data_len, dc);
    hex(f, (const u8 *)data_chunk, dc);
    fprintf(f, "\",\n    \"header\": {\"type\": 48, \"flags\": 2, \"n\": 64, \"m\": 192, \"share\": 0, \"block_id\": 1, \"seq\": 0, \"win_len\": 1000, \"payload_len\": 1024, \"conv\": 4107, \"group\": 0, \"sid\": 4328719365}},\n");
    fprintf(f, "  \"parity\": {\"record_file\": \"http_parity.record.bin\", \"record_len\": %u, \"chunk_file\": \"http_parity.chunk.bin\", \"chunk_len\": %zu, \"chunk_hex\": \"",
            parity_len, pc);
    hex(f, (const u8 *)parity_chunk, pc);
    fprintf(f, "\",\n    \"header\": {\"type\": 49, \"flags\": 0, \"n\": 64, \"m\": 192, \"share\": 64, \"block_id\": 1, \"seq\": 0, \"win_len\": 1, \"payload_len\": 1024, \"epoch\": 1000, \"conv\": 4107, \"group\": 0, \"sid\": 4328719365}},\n");
    fprintf(f, "  \"pad_note\": \"PADDED DATA: real bytes 0..999, bytes 1000..1023 are zero before the payload CRC and ChaCha8.\"\n}\n");
    fclose(f);
}

static void mux_vector(void)
{
    u8 stream[512];
    u8 d1[3] = {0x11, 0x22, 0x33};
    u8 d2[2] = {0x44, 0x55};
    size_t off = 0;
    int n;
    FILE *f;

    n = tbt_mux_encode(TBT_MUX_OPEN, 2, 0x01020304u, NULL, 0,
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux open");
    off += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x01020304u, d1, sizeof(d1),
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux data1");
    off += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_WIN, 0, 0x01020304u, NULL, 4096,
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux win");
    off += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_OPEN, 1, 0x0a0b0c0du, NULL, 0,
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux open2");
    off += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_DATA, 0, 0x0a0b0c0du, d2, sizeof(d2),
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux data2");
    off += (size_t)n;
    n = tbt_mux_encode(TBT_MUX_FIN, 0, 0x01020304u, NULL, 0,
                       stream + off, sizeof(stream) - off);
    if (n < 0)
        die("mux fin");
    off += (size_t)n;

    write_file("mux_stream.bin", stream, off);
    f = open_json("mux_stream.json");
    fprintf(f, "{\n  \"byte_order\": \"big-endian for len and stream_id\",\n  \"stream_file\": \"mux_stream.bin\",\n  \"stream_len\": %zu,\n  \"stream_hex\": \"", off);
    hex(f, stream, off);
    fprintf(f, "\",\n  \"frames\": [\n");
    fprintf(f, "    {\"cmd\": 1, \"slot\": 2, \"len\": 0, \"stream_id\": 16909060},\n");
    fprintf(f, "    {\"cmd\": 2, \"slot\": 0, \"len\": 3, \"stream_id\": 16909060, \"payload_hex\": \"112233\"},\n");
    fprintf(f, "    {\"cmd\": 5, \"slot\": 0, \"len\": 4096, \"stream_id\": 16909060},\n");
    fprintf(f, "    {\"cmd\": 1, \"slot\": 1, \"len\": 0, \"stream_id\": 168496141},\n");
    fprintf(f, "    {\"cmd\": 2, \"slot\": 0, \"len\": 2, \"stream_id\": 168496141, \"payload_hex\": \"4455\"},\n");
    fprintf(f, "    {\"cmd\": 3, \"slot\": 0, \"len\": 0, \"stream_id\": 16909060}\n");
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void write_sc_json(const char *name, const char *codec, u16 n, u16 m,
                          u16 block, u16 L, u64 nonce,
                          const char *data_file, const char *parity_file,
                          const u8 *dp, const u8 *pp,
                          int decoded, const char *recovered_file)
{
    FILE *f = open_json(name);

    fprintf(f, "{\n  \"codec\": \"%s\",\n  \"n\": %u,\n  \"m\": %u,\n  \"block\": %u,\n  \"L\": %u,\n  \"S\": %u,\n  \"seed\": %llu,\n",
            codec, n, m, block, L, TBT_FEC_S, (unsigned long long)nonce);
    fprintf(f, "  \"data_file\": \"%s\",\n  \"parity_file\": \"%s\",\n", data_file, parity_file);
    fprintf(f, "  \"data_present_hex\": \"");
    hex(f, dp, (size_t)(n + 7) / 8);
    fprintf(f, "\",\n  \"parity_present_hex\": \"");
    hex(f, pp, (size_t)(m + 7) / 8);
    fprintf(f, "\",\n  \"decoded_data_shares\": %d", decoded);
    if (recovered_file)
        fprintf(f, ",\n  \"recovered_file\": \"%s\"", recovered_file);
    fprintf(f, "\n}\n");
    fclose(f);
}

static void sclc_downlink(void)
{
    const u16 n = 128, m = 128;
    const u64 nonce = 17;
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc;
    void *ctx = ops->new(n, m, 500, 256, TBT_FEC_S, 0, nonce);
    u8 *data[256], *parity[256], *data2[256], *parity2[256];
    u8 *dbuf, *pbuf, *dbuf2, *pbuf2, *recovered;
    u8 dp[256], pp[256];
    FILE *f;
    u16 i;
    int rc;

    if (!ctx)
        die("downlink codec");
    dbuf = calloc((size_t)n, TBT_FEC_S);
    pbuf = calloc((size_t)m, TBT_FEC_S);
    dbuf2 = calloc((size_t)n, TBT_FEC_S);
    pbuf2 = calloc((size_t)m, TBT_FEC_S);
    recovered = calloc((size_t)n, TBT_FEC_S);
    if (!dbuf || !pbuf || !dbuf2 || !pbuf2 || !recovered)
        die("downlink alloc");
    for (i = 0; i < n; i++)
    {
        data[i] = dbuf + (size_t)i * TBT_FEC_S;
        data2[i] = dbuf2 + (size_t)i * TBT_FEC_S;
        fill(data[i], TBT_FEC_S, (u8)(0x10 + i));
        memcpy(data2[i], data[i], TBT_FEC_S);
    }
    for (i = 0; i < m; i++)
    {
        parity[i] = pbuf + (size_t)i * TBT_FEC_S;
        parity2[i] = pbuf2 + (size_t)i * TBT_FEC_S;
        memset(parity[i], 0, TBT_FEC_S);
        memset(parity2[i], 0, TBT_FEC_S);
    }
    if (ops->encode(ctx, data, n, parity))
        die("downlink encode");
    memcpy(pbuf2, pbuf, (size_t)m * TBT_FEC_S);

    memset(dp, 0xff, sizeof(dp));
    memset(pp, 0xff, sizeof(pp));
    dp[3] = 0; /* one byte per share: share 3 is erased */
    memset(data2[3], 0, TBT_FEC_S);

    rc = ops->decode(ctx, data2, parity2, dp, pp, n);
    if (rc < 1)
        die("downlink decode did not recover the erased share");
    for (i = 0; i < n; i++)
        if (memcmp(data2[i], data[i], TBT_FEC_S))
            die("downlink recovered bytes differ");
    for (i = 0; i < n; i++)
        memcpy(recovered + (size_t)i * TBT_FEC_S, data2[i], TBT_FEC_S);

    write_file("sclc_downlink_data.bin", dbuf, (size_t)n * TBT_FEC_S);
    write_file("sclc_downlink_parity.bin", pbuf, (size_t)m * TBT_FEC_S);
    write_file("sclc_downlink_recovered.bin", recovered, (size_t)n * TBT_FEC_S);
    write_sc_json("sclc_downlink.json", "sc-ldpc-binary", n, m, 256, 8,
                  nonce, "sclc_downlink_data.bin", "sclc_downlink_parity.bin",
                  dp, pp, rc, "sclc_downlink_recovered.bin");
    f = open_json("sclc_downlink_meta.json");
    fprintf(f, "{\"data_crc32c\": %u, \"parity_crc32c\": %u, \"recovered_crc32c\": %u}\n",
            tbt_fec_crc32c(0, dbuf, (size_t)n * TBT_FEC_S),
            tbt_fec_crc32c(0, pbuf, (size_t)m * TBT_FEC_S),
            tbt_fec_crc32c(0, recovered, (size_t)n * TBT_FEC_S));
    fclose(f);

    ops->free(ctx);
    free(dbuf);
    free(pbuf);
    free(dbuf2);
    free(pbuf2);
    free(recovered);
}

static void sclc_uplink(void)
{
    const u16 n = 8, m = 24;
    const u64 nonce = 1;
    const struct tbt_fec_ops *ops = &tbt_fec_ops_sclc_gf16;
    void *ctx = ops->new(n, m, 64, 8, TBT_FEC_S, 64, nonce);
    u8 *data[32], *parity[32], *data2[32], *parity2[32];
    u8 *dbuf, *pbuf, *dbuf2, *pbuf2, *recovered;
    u8 dp[256], pp[256];
    u16 i;
    int rc;

    if (!ctx)
        die("uplink codec");
    dbuf = calloc(n, TBT_FEC_S);
    pbuf = calloc(m, TBT_FEC_S);
    dbuf2 = calloc(n, TBT_FEC_S);
    pbuf2 = calloc(m, TBT_FEC_S);
    recovered = calloc(n, TBT_FEC_S);
    if (!dbuf || !pbuf || !dbuf2 || !pbuf2 || !recovered)
        die("uplink alloc");
    for (i = 0; i < n; i++)
    {
        data[i] = dbuf + (size_t)i * TBT_FEC_S;
        data2[i] = dbuf2 + (size_t)i * TBT_FEC_S;
        fill(data[i], TBT_FEC_S, (u8)(0x60 + i));
        memcpy(data2[i], data[i], TBT_FEC_S);
    }
    for (i = 0; i < m; i++)
    {
        parity[i] = pbuf + (size_t)i * TBT_FEC_S;
        parity2[i] = pbuf2 + (size_t)i * TBT_FEC_S;
        memset(parity[i], 0, TBT_FEC_S);
        memset(parity2[i], 0, TBT_FEC_S);
    }
    if (ops->encode(ctx, data, n, parity))
        die("uplink encode");
    memcpy(pbuf2, pbuf, (size_t)m * TBT_FEC_S);

    memset(dp, 0xff, sizeof(dp));
    memset(pp, 0xff, sizeof(pp));
    dp[0] = 0; /* one byte per share: data share 0 is erased */
    memset(data2[0], 0, TBT_FEC_S);

    rc = ops->decode(ctx, data2, parity2, dp, pp, n);
    if (rc < 1)
        die("uplink decode did not recover the erased share");
    for (i = 0; i < n; i++)
        if (memcmp(data2[i], data[i], TBT_FEC_S))
            die("uplink recovered bytes differ");
    for (i = 0; i < n; i++)
        memcpy(recovered + (size_t)i * TBT_FEC_S, data2[i], TBT_FEC_S);

    write_file("sclc_uplink_data.bin", dbuf, (size_t)n * TBT_FEC_S);
    write_file("sclc_uplink_parity.bin", pbuf, (size_t)m * TBT_FEC_S);
    write_file("sclc_uplink_recovered.bin", recovered, (size_t)n * TBT_FEC_S);
    write_sc_json("sclc_uplink.json", "sc-ldpc-gf16", n, m, 64, 8, nonce,
                  "sclc_uplink_data.bin", "sclc_uplink_parity.bin", dp, pp, rc,
                  "sclc_uplink_recovered.bin");

    ops->free(ctx);
    free(dbuf);
    free(pbuf);
    free(dbuf2);
    free(pbuf2);
    free(recovered);
}

static void manifest(void)
{
    static const char *files[] = {
        "crc32c.json", "chacha8.json", "chacha8_body_cipher.bin",
        "hello_v8.json", "hello_v8_body.bin", "hello_v8_frame.bin",
        "http_padded.json", "http_padded_data.record.bin",
        "http_padded_data.chunk.bin", "http_parity.record.bin",
        "http_parity.chunk.bin", "mux_stream.json", "mux_stream.bin",
        "sclc_downlink.json", "sclc_downlink_meta.json",
        "sclc_downlink_data.bin", "sclc_downlink_parity.bin",
        "sclc_downlink_recovered.bin", "sclc_uplink.json",
        "sclc_uplink_data.bin", "sclc_uplink_parity.bin",
        "sclc_uplink_recovered.bin",
    };
    FILE *f = open_json("manifest.json");
    size_t i;

    fprintf(f, "{\n  \"version\": \"0.8.0\",\n  \"S\": %u,\n  \"http_chunk_len\": 1079,\n  \"raw_record_len\": 1072,\n  \"files\": [\n",
            TBT_FEC_S);
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        char path[256];
        FILE *in;
        u8 *buf;
        long sz;
        struct stat st;

        snprintf(path, sizeof(path), OUT "/%s", files[i]);
        if (stat(path, &st))
            die(path);
        in = fopen(path, "rb");
        if (!in)
            die(path);
        sz = (long)st.st_size;
        buf = malloc((size_t)(sz ? sz : 1));
        if (!buf || (sz && fread(buf, 1, (size_t)sz, in) != (size_t)sz))
            die(path);
        fclose(in);
        fprintf(f, "    {\"name\": \"%s\", \"bytes\": %ld, \"crc32c\": %u}%s\n",
                files[i], sz, tbt_fec_crc32c(0, buf, (size_t)sz),
                i + 1 < sizeof(files) / sizeof(files[0]) ? "," : "");
        free(buf);
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

int main(void)
{
    if (mkdir(OUT, 0777) && errno != EEXIST)
        die("mkdir " OUT);
    crc_vectors();
    chacha_vectors();
    hello_vector();
    http_vectors();
    mux_vector();
    sclc_downlink();
    sclc_uplink();
    manifest();
    printf("gen_golden: wrote %s\n", OUT);
    return 0;
}
