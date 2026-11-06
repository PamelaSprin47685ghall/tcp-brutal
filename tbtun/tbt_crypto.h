/* tbt_crypto.h - ChaCha8 body-only obfuscation for the tbtun v6 carriers.
 *
 * Contract: review/goal-v6-interfaces.md (T0, master 12afe48) §2.
 *
 * Scope. Only the FEC payload of DATA and PARITY shares is transformed. The
 * 40-byte FEC header, the HTTP request/response headers, the chunk size lines
 * and every control frame (ACK/NACK/FEC_CFG/FEC_FBK/PAD) stay in clear text.
 * The receiver needs the framing and the scheduler fields before it can pick
 * a key or a counter, so encrypting more would move the cipher below the
 * layer that routes the record.
 *
 * TX computes payload_crc32c over the clear payload, then calls
 * tbt_crypto_body_xor(); RX calls it first and verifies payload_crc32c after
 * (T0 §1.3, §2.2). A payload CRC failure therefore means a damaged ciphertext
 * or a wrong key, and the share is dropped without touching the carrier.
 *
 * It is obfuscation, not confidentiality: the session key travels in clear
 * text in the HELLO v6 body, and there is no authentication. Its job is to
 * keep application bytes out of naive DPI signatures. WireGuard is the answer
 * when real confidentiality is needed.
 *
 * Core. A C99 port of tiny-chacha by 0xNullll (MIT, commit 890a5f0) with the
 * 64-bit counter and 8-byte nonce layout of D. J. Bernstein's public-domain
 * eSTREAM chacha/ref. The full MIT notice and the list of changes are in
 * tbt_crypto.c. The round count is a parameter on the low-level entry points
 * so the KAT can pin the shared permutation; the session path is ChaCha8.
 *
 * State layout (Bernstein, NOT RFC 8439):
 *
 *   state[0..3]   constants ("expand 32-byte k" or "expand 16-byte k")
 *   state[4..11]  key words, little-endian; a 16-byte key is repeated
 *   state[12..13] 64-bit block counter, little-endian (low word first)
 *   state[14..15] 8-byte nonce, little-endian
 *
 * Counter space (T0 §2.4):
 *
 *   counter = (seq << 8) | (parity << 7) | block_index
 *
 * A share payload is at most 8192 bytes = 128 blocks, so block_index is
 * 0..127 and fits below bit 7. seq < 2^56 keeps the whole counter in 64 bits;
 * tbt_crypto_body_xor() rejects a larger seq instead of wrapping. Every share
 * has its own (seq, parity) pair, so a retransmit or an FEC share that arrives
 * out of order decrypts at the same position, and no state is shared between
 * carriers.
 *
 * Key. A session key is 32 random bytes. With `crypto_key=` unset, the near
 * side generates it and the HELLO v6 body carries it in clear text
 * (key_id=1); the far side adopts it. With `crypto_key=` set on both ends,
 * key_id=0 and the HELLO key field is zero; a mismatch is detected as a
 * payload CRC failure. The helpers at the bottom of this header define the
 * HELLO crypto field layout and the resolve rules, so the HELLO builder and
 * parser do not re-derive them.
 *
 * Direction separation. The counter has no direction bit, and both directions
 * start seq at 0. Each side therefore generates an independent 8-byte
 * crypto_nonce for its TX direction and advertises it in its own HELLO body
 * +26; RX uses the peer's HELLO nonce. A session keeps a separate tx and rx
 * context: never seal and open both directions with one context, and never
 * share one nonce between them. The key may be shared; the nonces must not
 * be. A new session or direction restart generates a new nonce.
 *
 * Host build. This header and tbt_crypto.c compile unchanged into
 * host/test_crypto.c, like tbt_http.c and tbt_parse.c.
 */
#ifndef _TBT_CRYPTO_H
#define _TBT_CRYPTO_H

#include "tbt_uapi.h"

#define TBT_CRYPTO_KEY_LEN 32   /* 256-bit session key */
#define TBT_CRYPTO_NONCE_LEN 8  /* Bernstein layout, 64-bit counter */
#define TBT_CRYPTO_BLOCK_LEN 64
#define TBT_CRYPTO_ROUNDS 8     /* ChaCha8 */

/* One share payload is at most 8192 bytes = 128 blocks; the counter formula
 * reserves bits 0..6 for the block index and bit 7 for parity. */
#define TBT_CRYPTO_BODY_MAX 8192
#define TBT_CRYPTO_SEQ_MAX ((1ULL << 56) - 1)

/* T0 §2.1 fields first; block/pos/rounds/wrapped are the streaming state the
 * functions need and are not part of the wire contract. B allocates this
 * struct in the session and never inspects it. */
struct tbt_crypto_ctx
{
    u32 state[16];
    u64 counter; /* next block number */
    u8 nonce[TBT_CRYPTO_NONCE_LEN];
    bool ready;

    u8 block[TBT_CRYPTO_BLOCK_LEN]; /* current keystream block */
    u32 pos;                        /* bytes used; 64 = none buffered */
    u32 rounds;
    u32 wrapped; /* 64-bit counter wrapped: stop */
};

/* Install a 256-bit key, this direction's 8-byte nonce and the initial block
 * counter. `nonce` is the TX nonce on a TX context and the peer's HELLO nonce
 * on an RX context; it must never be shared between the two directions.
 * `key == NULL` means crypto is off: the context becomes not-ready and every
 * transform is a no-op. Returns 0, or -EINVAL for a NULL context. */
int tbt_crypto_init(struct tbt_crypto_ctx *c, const u8 key[TBT_CRYPTO_KEY_LEN],
                    const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter);

/* The two directions of one session (T0 §2.3/§2.4). This is a convenience
 * for the session layer, not part of the frozen wire ABI: it adds no field
 * and no allocation. The counter has no direction bit and both sides start
 * seq at 0, so the directions must not share a nonce or a context. `tx` is
 * sealed with this side's HELLO nonce; `rx` is opened with the peer's HELLO
 * nonce. */
struct tbt_crypto_pair
{
    struct tbt_crypto_ctx tx;
    struct tbt_crypto_ctx rx;
};

/* Install both directions. `tx_nonce` is this side's TX nonce and `rx_nonce`
 * is the peer's HELLO nonce. Equal nonces would repeat the keystream for the
 * same (seq, parity, block), so they are rejected with -EINVAL. `key == NULL`
 * means crypto is off: both contexts become not-ready and the nonces are
 * ignored. Returns 0, -EINVAL, or the tbt_crypto_init() error. */
int tbt_crypto_pair_init(struct tbt_crypto_pair *p,
                         const u8 key[TBT_CRYPTO_KEY_LEN],
                         const u8 tx_nonce[TBT_CRYPTO_NONCE_LEN],
                         const u8 rx_nonce[TBT_CRYPTO_NONCE_LEN]);

/* Erase both directions. */
void tbt_crypto_pair_wipe(struct tbt_crypto_pair *p);

/* Set the next block number and discard any buffered partial block. */
void tbt_crypto_set_counter(struct tbt_crypto_ctx *c, u64 counter);

/* The next block number. After a partial block was consumed this is the block
 * after the buffered one, exactly like a stream position rounded up. */
u64 tbt_crypto_get_counter(const struct tbt_crypto_ctx *c);

/* XOR len bytes in place, advancing the block counter. len is arbitrary; a
 * partial block is buffered across calls. Returns 0, or -EINVAL when the
 * context is not ready or the 64-bit counter would wrap. */
int tbt_crypto_xor(struct tbt_crypto_ctx *c, u8 *data, size_t len);

/* XOR at an absolute block counter without changing c->counter or its
 * buffered block. Starts at a block boundary. Returns 0 or -EINVAL. */
int tbt_crypto_xor_at(struct tbt_crypto_ctx *c, u64 counter, u8 *data, size_t len);

/* The body-only hook. For a DATA or PARITY share with global sequence `seq`,
 * use counter = (seq << 8) | (parity << 7) and XOR the payload. The caller
 * passes the clear payload on TX and the ciphertext on RX; the operation is
 * its own inverse. It is implemented with tbt_crypto_xor_at(), so it does not
 * change c->counter or the buffered block: several carriers may call it
 * concurrently on the same rx context. `len` must be <= TBT_CRYPTO_BODY_MAX
 * and `seq` <= TBT_CRYPTO_SEQ_MAX. Returns 0 when crypto is off (no-op),
 * -EINVAL when the arguments would overflow the counter space. */
int tbt_crypto_body_xor(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                        u8 *payload, u16 len);

/* Per-share variant for the FEC data plane. DATA shares are unique by seq, so
 * this is exactly tbt_crypto_body_xor(). PARITY shares of one block share the
 * same (seq, parity) pair and differ only in share_index, so mixing
 * share_index into the nonce is required to avoid reusing one keystream for
 * every parity share. Both ends know share_index from the clear 40-byte
 * header; the counter formula and the wire are unchanged. */
int tbt_crypto_body_xor_share(struct tbt_crypto_ctx *c, u64 seq, bool parity,
                              u16 share_index, u8 *payload, u16 len);

/* Erase the schedule, keystream and key material with volatile stores. */
void tbt_crypto_wipe(struct tbt_crypto_ctx *c);

/* ------------------------------------------------------------------------- */
/* Low-level core, also used by the KAT.                                     */

/* key_len must be 16 or 32; rounds must be 8, 12 or 20. The session API above
 * fixes 32 bytes and 8 rounds. Returns 0 or -EINVAL. */
int tbt_chacha8_init(struct tbt_crypto_ctx *c, const u8 *key, size_t key_len,
                     const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter,
                     int rounds);

/* One-shot key setup + XOR + wipe. Same return contract as
 * tbt_crypto_xor(). */
int tbt_chacha8_xor(const u8 *key, size_t key_len,
                    const u8 nonce[TBT_CRYPTO_NONCE_LEN], u64 counter,
                    int rounds, u8 *data, size_t len);

/* ------------------------------------------------------------------------- */
/* HELLO v6 crypto fields (T0 §2.3).                                        */

/* Offsets from the first byte of the HELLO body. The fixed body is 66 bytes:
 * +23 crypto, +24 key_id, +25 reserved, +26 crypto_nonce[8], +34 key[32]. */
#define TBT_CRYPTO_HELLO_LEN 66
#define TBT_CRYPTO_HELLO_OFF 23
#define TBT_CRYPTO_HELLO_CRYPTO 23
#define TBT_CRYPTO_HELLO_KEY_ID 24
#define TBT_CRYPTO_HELLO_RESERVED 25
#define TBT_CRYPTO_HELLO_NONCE 26
#define TBT_CRYPTO_HELLO_KEY 34

#define TBT_CRYPTO_HELLO_ENABLED 0x01     /* crypto bit0 */
#define TBT_CRYPTO_HELLO_KEY_IN_HELLO 0x02 /* crypto bit1 */
#define TBT_CRYPTO_KEY_ID_NONE 0          /* pre-shared key or crypto off */
#define TBT_CRYPTO_KEY_ID_HELLO 1         /* key follows at +34 */

/* Write the crypto fields of a 66-byte HELLO body. `hello` is the whole body;
 * bytes outside +23..+65 are left alone. `key` is written only when
 * key_id == TBT_CRYPTO_KEY_ID_HELLO, and is zeroed otherwise; `crypto_nonce`
 * is always written. `crypto` is bit0 = enabled, bit1 = key in HELLO. */
void tbt_crypto_hello_fill(u8 hello[TBT_CRYPTO_HELLO_LEN], bool enabled,
                           u8 key_id,
                           const u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN],
                           const u8 key[TBT_CRYPTO_KEY_LEN]);

/* Parse and validate the crypto fields of a HELLO body. Rejects a body
 * shorter than 66 bytes, a nonzero reserved byte, an unknown key_id, and a
 * key_id/key_in_hello mismatch. On success fills the outputs (key is zeroed
 * when key_id is 0) and returns 0; otherwise returns -EPROTO without writing
 * them. */
int tbt_crypto_hello_parse(const u8 *hello, size_t len, bool *enabled,
                           u8 *key_id,
                           u8 crypto_nonce[TBT_CRYPTO_NONCE_LEN],
                           u8 key[TBT_CRYPTO_KEY_LEN]);

/* Resolve the session key under the T0 §2.3 rules:
 *   - crypto off: out is zeroed, return 0;
 *   - `local_key_wins`: this side owns the key -- either an operator
 *     `crypto_key=`, or a key this side generated and announced in its own
 *     HELLO with key_id 1. The peer learned it from us, so its HELLO must
 *     carry key_id 0; key_id 1 means the peer generated a different key,
 *     which conflicts. out is our key;
 *   - otherwise: the key is the peer's. The peer must have key_id 1 and out
 *     is the HELLO key. The caller adopts it on the first HELLO and checks
 *     every later HELLO against it, so the flag stays false for the whole
 *     session once a key was adopted -- an adopted key is not a local one.
 * Returns 0 or -EPROTO. */
int tbt_crypto_resolve_key(bool local_key_wins,
                           const u8 local_key[TBT_CRYPTO_KEY_LEN],
                           bool enabled, u8 key_id,
                           const u8 hello_key[TBT_CRYPTO_KEY_LEN],
                           u8 out[TBT_CRYPTO_KEY_LEN]);

#endif /* _TBT_CRYPTO_H */
