/*
 * Minimal ecrypt-sync.h shim for Bernstein's public-domain ChaCha reference
 * sources (chacha-ref.c version 20080118).  Only the types, the context and
 * the four little-endian helpers those files use are provided.
 */
#ifndef ECRYPT_SYNC_H
#define ECRYPT_SYNC_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct {
	u32 input[16];
} ECRYPT_ctx;

#define ROTL32(v, c) (((v) << (c)) | ((v) >> (32 - (c))))
#define U32V(v) ((u32)(v))

#define U8TO32_LITTLE(p) \
	((u32)(p)[0] | ((u32)(p)[1] << 8) | ((u32)(p)[2] << 16) | \
	 ((u32)(p)[3] << 24))
#define U32TO8_LITTLE(p, v)                     \
	do {                                    \
		(p)[0] = (u8)(v);               \
		(p)[1] = (u8)((v) >> 8);        \
		(p)[2] = (u8)((v) >> 16);       \
		(p)[3] = (u8)((v) >> 24);       \
	} while (0)

void ECRYPT_init(void);
void ECRYPT_keysetup(ECRYPT_ctx *x, const u8 *k, u32 kbits, u32 ivbits);
void ECRYPT_ivsetup(ECRYPT_ctx *x, const u8 *iv);
void ECRYPT_encrypt_bytes(ECRYPT_ctx *x, const u8 *m, u8 *c, u32 bytes);
void ECRYPT_decrypt_bytes(ECRYPT_ctx *x, const u8 *c, u8 *m, u32 bytes);
void ECRYPT_keystream_bytes(ECRYPT_ctx *x, u8 *stream, u32 bytes);

#endif
