/*
 * chacha8.h - ChaCha8/ChaCha12/ChaCha20 stream core for the FEC prototype.
 *
 * Derived from tiny-chacha by 0xNullll (MIT, commit 890a5f0):
 *   https://github.com/0xNullll/tiny-chacha
 * The full MIT notice is reproduced at the top of chacha8.c.
 *
 * State layout (D. J. Bernstein eSTREAM chacha/ref, NOT RFC 8439):
 *
 *   state[0..3]   constants ("expand 32-byte k" or "expand 16-byte k")
 *   state[4..11]  key words, little-endian; a 16-byte key is repeated
 *   state[12..13] 64-bit block counter, little-endian (low word first)
 *   state[14..15] 8-byte nonce, little-endian
 *
 * tiny-chacha uses a 96-bit IV plus a 32-bit counter, and always uses the
 * 256-bit sigma constants (it repeats a 16-byte key). This port follows
 * Bernstein's 128-bit key schedule (tau constants), because the official
 * ChaCha8 test vector TC1 is defined for a 16-byte key. The 256-bit key
 * path is bit-identical to tiny-chacha.
 *
 * C99, no allocation, no I/O. Host-only prototype; see review/chacha8.md.
 */

#ifndef FEC_PROTOTYPE_CHACHA8_H
#define FEC_PROTOTYPE_CHACHA8_H

#include <stddef.h>
#include <stdint.h>

#define CHACHA8_BLOCK_SIZE 64u
#define CHACHA8_KEY_SIZE_128 16u
#define CHACHA8_KEY_SIZE_256 32u
#define CHACHA8_NONCE_SIZE 8u

#define CHACHA8_ROUNDS_8 8
#define CHACHA8_ROUNDS_12 12
#define CHACHA8_ROUNDS_20 20

typedef struct {
    uint32_t state[16];                  /* schedule; counter advances here */
    uint8_t block[CHACHA8_BLOCK_SIZE];   /* current keystream block */
    uint32_t pos;                        /* bytes used from block; 64 = empty */
    int rounds;
    int wrapped;                         /* 64-bit counter wrapped: stop */
} chacha8_ctx;

/*
 * Set up a context. key_len must be 16 or 32; rounds must be 8, 12 or 20.
 * Returns 0 on success, -1 on invalid arguments.
 */
int chacha8_init(chacha8_ctx *ctx,
                 const uint8_t *key, size_t key_len,
                 const uint8_t nonce[CHACHA8_NONCE_SIZE],
                 uint64_t counter, int rounds);

/*
 * XOR len bytes from in with the keystream into out. in == out is allowed.
 * Returns 0 on success. Returns -1 if the 64-bit block counter would wrap
 * (after 2^64 blocks); the caller must discard any partial output.
 */
int chacha8_crypt(chacha8_ctx *ctx, const uint8_t *in, size_t len,
                  uint8_t *out);

/*
 * One-shot init + crypt + wipe. Same return contract as chacha8_crypt.
 */
int chacha8_xor(const uint8_t *key, size_t key_len,
                const uint8_t nonce[CHACHA8_NONCE_SIZE],
                uint64_t counter, int rounds,
                const uint8_t *in, size_t len, uint8_t *out);

/*
 * Erase the schedule and keystream. Uses volatile stores so the compiler
 * does not elide the wipe.
 */
void chacha8_wipe(chacha8_ctx *ctx);

#endif /* FEC_PROTOTYPE_CHACHA8_H */
