/*
 * test_crypto.c - known-answer, streaming and body-only tests for
 * tbt_crypto.c.
 *
 * Vectors:
 *   TC1 (ChaCha8, 128-bit zero key, zero IV): secworks/chacha_testvectors,
 *       commit a56fff0, BSD-2; also the draft ChaCha vectors.
 *   Zero-key ChaCha8/12/20 and the generated 256-bit ChaCha8 vector:
 *       cross-checked by Bernstein eSTREAM chacha8/ref, tiny-chacha and
 *       Linux chacha_block_generic(..., 8); see
 *       review/chacha8-reference.md section 7.
 *   RFC 8439 sections 2.3.2 and 2.4.2: ChaCha20 block and encryption, used
 *       here to prove the 64-bit counter/nonce mapping is the Bernstein one.
 *
 * The body-only tests pin the T0 §2.4 counter formula and the HELLO v6 key
 * rules. Build: make host-test.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tbt_crypto.h"

static int failures;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int from_hex(const char *hex, u8 *out, size_t out_len)
{
    size_t i;

    if (strlen(hex) != out_len * 2)
        return -1;

    for (i = 0; i < out_len; i++)
    {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);

        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (u8)((hi << 4) | lo);
    }
    return 0;
}

static void print_hex(const char *prefix, const u8 *p, size_t n)
{
    size_t i;

    printf("%s", prefix);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static void check_bytes(const char *name, const u8 *got, const char *want_hex,
                        size_t n)
{
    u8 want[256];

    if (n > sizeof(want) || from_hex(want_hex, want, n) != 0)
    {
        printf("FAIL %s: malformed test vector\n", name);
        failures++;
        return;
    }

    if (memcmp(got, want, n) != 0)
    {
        printf("FAIL %s\n", name);
        print_hex("  got  ", got, n);
        print_hex("  want ", want, n);
        failures++;
        return;
    }

    printf("PASS %s\n", name);
}

static void check_keystream(const char *name, const u8 *key, size_t key_len,
                            const u8 *nonce, u64 counter, int rounds,
                            size_t len, const char *want_hex)
{
    u8 buf[256] = {0};

    if (len > sizeof(buf))
    {
        printf("FAIL %s: vector too long\n", name);
        failures++;
        return;
    }

    if (tbt_chacha8_xor(key, key_len, nonce, counter, rounds, buf, len) != 0)
    {
        printf("FAIL %s: xor error\n", name);
        failures++;
        return;
    }

    check_bytes(name, buf, want_hex, len);
}

static void test_tc1(void)
{
    static const char *want =
        "e28a5fa4a67f8c5defed3e6fb7303486aa8427d31419a729572d777953491120"
        "b64ab8e72b8deb85cd6aea7cb6089a101824beeb08814a428aab1fa2c816081b"
        "8a26af448a1ba906368fd8c83831c18cec8ced811a028e675b8d2be8fce08116"
        "5ceae9f1d1b7a975497749480569ceb83de6a0a587d4984f19925f5d338e430d";
    u8 key[16] = {0};
    u8 nonce[8] = {0};

    check_keystream("TC1 ChaCha8 128-bit zero key, blocks 0-1", key,
                    sizeof(key), nonce, 0, 8, 128, want);
}

static void test_zero_key_256(void)
{
    static const u8 key[32] = {0};
    static const u8 nonce[8] = {0};

    check_keystream("ChaCha8 256-bit zero key, block 0", key, sizeof(key),
                    nonce, 0, 8, 64,
                    "3e00ef2f895f40d67f5bb8e81f09a5a1"
                    "2c840ec3ce9a7f3b181be188ef711a1e"
                    "984ce172b9216f419f445367456d5619"
                    "314a42a3da86b001387bfdb80e0cfe42");

    check_keystream("ChaCha12 256-bit zero key, block 0", key, sizeof(key),
                    nonce, 0, 12, 64,
                    "9bf49a6a0755f953811fce125f2683d5"
                    "0429c3bb49e074147e0089a52eae155f"
                    "0564f879d27ae3c02ce82834acfa8c79"
                    "3a629f2ca0de6919610be82f411326be");

    check_keystream("ChaCha20 256-bit zero key, block 0", key, sizeof(key),
                    nonce, 0, 20, 64,
                    "76b8e0ada0f13d90405d6ae55386bd28"
                    "bdd219b8a08ded1aa836efcc8b770dc7"
                    "da41597c5157488d7724e03fb8d84a37"
                    "6a43b8f41518a11cc387b669b2ee6586");
}

static void test_generated_256(void)
{
    static const char *want =
        "9a1e949f0d71a3a307da58730300dea725342ee45cf066d8350095e3c443cee2"
        "68db83e2ce8d899d90e96aef644b1c8493c228cdaa420c5603d745f14127b4ba"
        "09d530f41df22ff51b7224cfb6fd90530a43b09fdb3ddfed8838cb908d28e129"
        "b6e178a6abe6fea7d3876e9e681f63ec23b90e2eb1ddc251b6266796e415e88b";
    u8 key[32];
    u8 nonce[8];
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)i;
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0xa0 + i);

    check_keystream("ChaCha8 key 00..1f, nonce a0..a7, 128 bytes", key,
                    sizeof(key), nonce, 0, 8, 128, want);
}

static void test_rfc8439_block(void)
{
    static const char *want = "10f1e7e4d13b5915500fdd1fa32071c4"
                              "c7d1f4c733c068030422aa9ac3d46c4e"
                              "d2826446079faa0914c2d705d98b02a2"
                              "b5129cd1de164eb9cbd083e8a2503c4e";
    u8 key[32];
    u8 nonce[8] = {0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)i;

    /*
     * RFC 8439 state has state[12] = 1 and state[13..15] = nonce
     * 00:00:00:09:00:00:00:4a:00:00:00:00. Our 64-bit counter takes the
     * high word from nonce[0..3]: counter = 0x09000000_00000001, and the
     * 8-byte nonce is 00:00:00:4a:00:00:00:00.
     */
    check_keystream("RFC 8439 2.3.2 ChaCha20 block, counter 1", key,
                    sizeof(key), nonce, 0x0900000000000001ULL, 20, 64, want);
}

static void test_rfc8439_encrypt(void)
{
    static const char *plain_hex =
        "4c616469657320616e642047656e746c656d656e206f662074686520636c6173"
        "73206f66202739393a204966204920636f756c64206f6666657220796f75206f"
        "6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"
        "637265656e20776f756c642062652069742e";
    static const char *cipher_hex =
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d";
    u8 key[32];
    u8 nonce[8] = {0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    u8 buf[114];
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)i;
    if (from_hex(plain_hex, buf, sizeof(buf)) != 0)
    {
        printf("FAIL RFC 8439 2.4.2: malformed vector\n");
        failures++;
        return;
    }

    if (tbt_chacha8_xor(key, sizeof(key), nonce, 1, 20, buf, sizeof(buf)) != 0)
    {
        printf("FAIL RFC 8439 2.4.2: crypt error\n");
        failures++;
        return;
    }

    check_bytes("RFC 8439 2.4.2 ChaCha20 encryption", buf, cipher_hex,
                sizeof(buf));
}

static void test_counter_positions(void)
{
    u8 key[32];
    u8 nonce[8];
    u8 stream0[128] = {0};
    u8 stream1[64] = {0};
    u8 carry0[128] = {0};
    u8 carry1[64] = {0};
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)(0x11 * i + 5);
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0x80 + i);

    if (tbt_chacha8_xor(key, sizeof(key), nonce, 0, 8, stream0,
                        sizeof(stream0)) != 0 ||
        tbt_chacha8_xor(key, sizeof(key), nonce, 1, 8, stream1,
                        sizeof(stream1)) != 0)
    {
        printf("FAIL counter start: crypt error\n");
        failures++;
        return;
    }
    if (memcmp(stream1, stream0 + 64, 64) != 0)
    {
        printf("FAIL counter start: block mismatch\n");
        failures++;
    }
    else
    {
        printf("PASS counter start: counter 1 matches block 1 of counter 0\n");
    }

    if (tbt_chacha8_xor(key, sizeof(key), nonce, 0xffffffffULL, 8, carry0,
                        sizeof(carry0)) != 0 ||
        tbt_chacha8_xor(key, sizeof(key), nonce, 0x100000000ULL, 8, carry1,
                        sizeof(carry1)) != 0)
    {
        printf("FAIL counter carry: crypt error\n");
        failures++;
        return;
    }
    if (memcmp(carry1, carry0 + 64, 64) != 0)
    {
        printf("FAIL counter carry: 0xffffffff+1 mismatch\n");
        failures++;
    }
    else
    {
        printf("PASS counter carry: 0xffffffff + 1 == 0x100000000\n");
    }
}

static void test_streaming(void)
{
    u8 key[32];
    u8 nonce[8];
    u8 buf[512];
    u8 one[512];
    u8 chunked[512];
    static const size_t chunks[] = {1, 63, 64, 65, 100, 7, 212};
    struct tbt_crypto_ctx ctx;
    size_t off;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)(i * 7 + 3);
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0xf0 - i);
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (u8)(i * 31 + 11);

    memcpy(one, buf, sizeof(buf));
    if (tbt_chacha8_xor(key, sizeof(key), nonce, 12345, 8, one,
                        sizeof(one)) != 0)
    {
        printf("FAIL streaming: one-shot error\n");
        failures++;
        return;
    }

    if (tbt_crypto_init(&ctx, key, nonce, 12345) != 0)
    {
        printf("FAIL streaming: init error\n");
        failures++;
        return;
    }
    off = 0;
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++)
    {
        size_t n = chunks[i];

        memcpy(chunked + off, buf + off, n);
        if (tbt_crypto_xor(&ctx, chunked + off, n) != 0)
        {
            printf("FAIL streaming: chunk error\n");
            failures++;
            return;
        }
        off += n;
    }
    if (off != sizeof(buf))
    {
        printf("FAIL streaming: chunk list covers %zu of %zu bytes\n", off,
               sizeof(buf));
        failures++;
    }
    else if (memcmp(chunked, one, sizeof(one)) != 0)
    {
        printf("FAIL streaming: chunked output differs\n");
        failures++;
    }
    else
    {
        printf("PASS streaming: 7 chunks match one-shot\n");
    }

    /* A partial block is buffered: 64 + 1 + 63 must equal 128 at once. */
    if (tbt_crypto_init(&ctx, key, nonce, 0) != 0)
    {
        printf("FAIL streaming: re-init error\n");
        failures++;
        return;
    }
    memcpy(chunked, buf, 128);
    if (tbt_crypto_xor(&ctx, chunked, 64) != 0 ||
        tbt_crypto_xor(&ctx, chunked + 64, 1) != 0 ||
        tbt_crypto_xor(&ctx, chunked + 65, 63) != 0)
    {
        printf("FAIL streaming: split error\n");
        failures++;
        return;
    }
    memcpy(one, buf, 128);
    tbt_chacha8_xor(key, sizeof(key), nonce, 0, 8, one, 128);
    if (memcmp(chunked, one, 128) != 0)
    {
        printf("FAIL streaming: split across a block differs\n");
        failures++;
    }
    else
    {
        printf("PASS streaming: a split inside a block is buffered\n");
    }
}

static void test_xor_at(void)
{
    u8 key[32];
    u8 nonce[8];
    u8 stream[256] = {0};
    u8 at[64];
    struct tbt_crypto_ctx ctx;
    u64 counter;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)(i + 9);
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (u8)(0x30 + i);

    tbt_chacha8_xor(key, sizeof(key), nonce, 0, 8, stream, sizeof(stream));

    if (tbt_crypto_init(&ctx, key, nonce, 0) != 0)
    {
        printf("FAIL xor_at: init error\n");
        failures++;
        return;
    }
    tbt_crypto_set_counter(&ctx, 11);
    counter = tbt_crypto_get_counter(&ctx);

    memset(at, 0, sizeof(at));
    if (tbt_crypto_xor_at(&ctx, 3, at, sizeof(at)) != 0)
    {
        printf("FAIL xor_at: call error\n");
        failures++;
        return;
    }
    if (memcmp(at, stream + 3 * 64, sizeof(at)) != 0)
    {
        printf("FAIL xor_at: output does not match block 3\n");
        failures++;
    }
    else
    {
        printf("PASS xor_at: block 3 matches the stream\n");
    }
    if (tbt_crypto_get_counter(&ctx) != counter)
    {
        printf("FAIL xor_at: counter advanced\n");
        failures++;
    }
    else
    {
        printf("PASS xor_at: counter is unchanged\n");
    }

    /* The buffered-block state must survive xor_at. */
    {
        u8 a[100] = {0};
        u8 b[100] = {0};
        u8 c[100] = {0};

        tbt_crypto_set_counter(&ctx, 20);
        tbt_crypto_xor(&ctx, a, 10); /* buffer block 20, 10 bytes used */
        tbt_crypto_xor_at(&ctx, 99, b, sizeof(b));
        tbt_crypto_xor(&ctx, c + 10, 90); /* continue block 20 */
        memcpy(c, a, 10);
        {
            u8 want[100] = {0};
            struct tbt_crypto_ctx ref;

            tbt_crypto_init(&ref, key, nonce, 20);
            tbt_crypto_xor(&ref, want, sizeof(want));
            if (memcmp(c, want, sizeof(c)) != 0)
            {
                printf("FAIL xor_at: it disturbed the buffered block\n");
                failures++;
            }
            else
            {
                printf("PASS xor_at: the buffered block is undisturbed\n");
            }
        }
    }
}

static void test_counter_wrap(void)
{
    u8 key[32] = {0};
    u8 nonce[8] = {0};
    u8 buf[65] = {0};
    struct tbt_crypto_ctx ctx;
    int rc;

    if (tbt_crypto_init(&ctx, key, nonce, ~(u64)0) != 0)
    {
        printf("FAIL counter wrap: init error\n");
        failures++;
        return;
    }

    if (tbt_crypto_xor(&ctx, buf, 64) != 0)
    {
        printf("FAIL counter wrap: last block rejected\n");
        failures++;
        return;
    }

    rc = tbt_crypto_xor(&ctx, buf, 1);
    if (rc != -EINVAL)
    {
        printf("FAIL counter wrap: post-wrap call returned %d\n", rc);
        failures++;
    }
    else
    {
        printf("PASS counter wrap: post-wrap call rejected\n");
    }

    if (tbt_crypto_init(&ctx, key, nonce, ~(u64)0) != 0)
    {
        printf("FAIL counter wrap: re-init error\n");
        failures++;
        return;
    }
    rc = tbt_crypto_xor(&ctx, buf, 65);
    if (rc != -EINVAL)
    {
        printf("FAIL counter wrap: 65-byte call returned %d\n", rc);
        failures++;
    }
    else
    {
        printf("PASS counter wrap: 65-byte call fails after first block\n");
    }
}

static void test_invalid_args(void)
{
    u8 key[32] = {0};
    u8 nonce[8] = {0};
    u8 buf[16] = {0};
    struct tbt_crypto_ctx ctx;
    int bad;

    bad = 0;
    bad += tbt_chacha8_init(NULL, key, 32, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, NULL, 32, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, NULL, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 0, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 15, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 24, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 33, nonce, 0, 8) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, nonce, 0, 0) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, nonce, 0, 7) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, nonce, 0, 9) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, nonce, 0, 19) == -EINVAL;
    bad += tbt_chacha8_init(&ctx, key, 32, nonce, 0, 21) == -EINVAL;
    bad += tbt_crypto_xor(NULL, buf, 1) == -EINVAL;
    bad += tbt_crypto_init(&ctx, key, nonce, 0) == 0;
    bad += tbt_crypto_xor(&ctx, NULL, 1) == -EINVAL;
    bad += tbt_crypto_xor(&ctx, NULL, 0) == 0;
    bad += tbt_crypto_xor_at(&ctx, 0, NULL, 1) == -EINVAL;
    bad += tbt_chacha8_xor(NULL, 32, nonce, 0, 8, buf, 16) == -EINVAL;
    bad += tbt_crypto_init(&ctx, NULL, nonce, 0) == 0;
    bad += tbt_crypto_xor(&ctx, buf, 1) == -EINVAL;

    if (bad != 20)
    {
        printf("FAIL invalid args: %d of 20 checks passed\n", bad);
        failures++;
    }
    else
    {
        printf("PASS invalid args: 20 rejection checks\n");
    }
}

static void test_wipe(void)
{
    u8 key[32];
    u8 nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    u8 buf[64] = {0};
    struct tbt_crypto_ctx ctx;
    const volatile u8 *p = (const volatile u8 *)&ctx;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (u8)(i + 1);

    if (tbt_crypto_init(&ctx, key, nonce, 42) != 0 ||
        tbt_crypto_xor(&ctx, buf, sizeof(buf)) != 0)
    {
        printf("FAIL wipe: setup error\n");
        failures++;
        return;
    }

    tbt_crypto_wipe(&ctx);
    for (i = 0; i < sizeof(ctx); i++)
    {
        if (p[i] != 0)
        {
            printf("FAIL wipe: byte %zu not zero\n", i);
            failures++;
            return;
        }
    }
    printf("PASS wipe: context zeroed\n");
}

/* ------------------------------------------------------------------------- */
/* Body-only hook and HELLO v6 key rules                                     */

static void fill(u8 *p, size_t n, u8 seed)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (u8)(seed + i * 13);
}

static void test_body_xor(void)
{
    u8 key[32];
    u8 nonce[8];
    u8 payload[8192];
    u8 sealed[8192];
    u8 direct[8192];
    u8 opened[8192];
    struct tbt_crypto_ctx ctx;
    u64 seq = 0x123456789aULL;

    fill(key, sizeof(key), 1);
    fill(nonce, sizeof(nonce), 2);
    fill(payload, sizeof(payload), 3);

    if (tbt_crypto_init(&ctx, key, nonce, 0) != 0)
    {
        printf("FAIL body: init error\n");
        failures++;
        return;
    }

    memcpy(sealed, payload, sizeof(payload));
    if (tbt_crypto_body_xor(&ctx, seq, false, sealed, sizeof(sealed)) != 0)
    {
        printf("FAIL body: xor error\n");
        failures++;
        return;
    }
    if (memcmp(sealed, payload, sizeof(payload)) == 0)
    {
        printf("FAIL body: payload unchanged\n");
        failures++;
    }

    memcpy(opened, sealed, sizeof(opened));
    if (tbt_crypto_body_xor(&ctx, seq, false, opened, sizeof(opened)) != 0 ||
        memcmp(opened, payload, sizeof(payload)) != 0)
    {
        printf("FAIL body: round trip did not restore the payload\n");
        failures++;
    }
    else
    {
        printf("PASS body: 8192-byte round trip\n");
    }

    /* Independent construction: counter = (seq << 8) | (parity << 7). */
    memcpy(direct, payload, sizeof(direct));
    if (tbt_chacha8_xor(key, sizeof(key), nonce, seq << 8, 8, direct,
                        sizeof(direct)) != 0)
    {
        printf("FAIL body: direct xor error\n");
        failures++;
        return;
    }
    if (memcmp(sealed, direct, sizeof(direct)) != 0)
    {
        printf("FAIL body: counter is not (seq << 8) | (parity << 7)\n");
        failures++;
    }
    else
    {
        printf("PASS body: counter is (seq << 8) | (parity << 7)\n");
    }

    /* Parity and seq both separate the keystream. */
    memcpy(direct, payload, sizeof(direct));
    tbt_chacha8_xor(key, sizeof(key), nonce, (seq << 8) | 0x80u, 8, direct,
                    sizeof(direct));
    if (memcmp(sealed, direct, sizeof(direct)) == 0)
    {
        printf("FAIL body: parity does not separate the keystream\n");
        failures++;
    }
    else
    {
        printf("PASS body: parity separates the keystream\n");
    }

    memcpy(direct, payload, sizeof(direct));
    tbt_crypto_body_xor(&ctx, seq + 1, false, direct, sizeof(direct));
    if (memcmp(sealed, direct, sizeof(direct)) == 0)
    {
        printf("FAIL body: seq does not separate the keystream\n");
        failures++;
    }
    else
    {
        printf("PASS body: seq separates the keystream\n");
    }

    /* Bounds: one byte over the share limit and one over the seq limit. */
    if (tbt_crypto_body_xor(&ctx, seq, false, payload,
                            TBT_CRYPTO_BODY_MAX + 1) != -EINVAL)
    {
        printf("FAIL body: oversized payload accepted\n");
        failures++;
    }
    else
    {
        printf("PASS body: oversized payload rejected\n");
    }
    if (tbt_crypto_body_xor(&ctx, TBT_CRYPTO_SEQ_MAX + 1, false, payload, 1) !=
        -EINVAL)
    {
        printf("FAIL body: seq overflow accepted\n");
        failures++;
    }
    else
    {
        printf("PASS body: seq overflow rejected\n");
    }

    /* crypto=off: init with a NULL key is a no-op. */
    {
        u8 off[64];

        fill(off, sizeof(off), 4);
        memcpy(direct, off, sizeof(off));
        if (tbt_crypto_init(&ctx, NULL, nonce, 0) != 0 ||
            tbt_crypto_body_xor(&ctx, seq, false, off, sizeof(off)) != 0 ||
            memcmp(off, direct, sizeof(off)) != 0)
        {
            printf("FAIL body: crypto=off transformed the payload\n");
            failures++;
        }
        else
        {
            printf("PASS body: crypto=off is a no-op\n");
        }
    }

    /* A retransmit is byte-identical. */
    if (tbt_crypto_init(&ctx, key, nonce, 0) != 0)
    {
        printf("FAIL body: re-init error\n");
        failures++;
        return;
    }
    memcpy(opened, payload, sizeof(opened));
    tbt_crypto_body_xor(&ctx, seq, false, opened, sizeof(opened));
    memcpy(sealed, payload, sizeof(sealed));
    tbt_crypto_body_xor(&ctx, seq, false, sealed, sizeof(sealed));
    if (memcmp(opened, sealed, sizeof(sealed)) != 0)
    {
        printf("FAIL body: retransmit differs\n");
        failures++;
    }
    else
    {
        printf("PASS body: retransmit is identical\n");
    }
}

static void test_hello(void)
{
    u8 body[66];
    u8 nonce[8];
    u8 key[32];
    u8 got_nonce[8];
    u8 got_key[32];
    u8 out[32];
    u8 local[32];
    bool enabled = false;
    u8 key_id = 0xff;
    size_t i;

    fill(body, sizeof(body), 0xa5);
    fill(nonce, sizeof(nonce), 1);
    fill(key, sizeof(key), 2);

    tbt_crypto_hello_fill(body, true, TBT_CRYPTO_KEY_ID_HELLO, nonce, key);

    if (body[TBT_CRYPTO_HELLO_CRYPTO] !=
            (TBT_CRYPTO_HELLO_ENABLED | TBT_CRYPTO_HELLO_KEY_IN_HELLO) ||
        body[TBT_CRYPTO_HELLO_KEY_ID] != TBT_CRYPTO_KEY_ID_HELLO ||
        body[TBT_CRYPTO_HELLO_RESERVED] != 0)
    {
        printf("FAIL hello: header bytes are wrong\n");
        failures++;
    }
    else
    {
        printf("PASS hello: crypto/key_id/reserved offsets\n");
    }

    if (memcmp(body + TBT_CRYPTO_HELLO_NONCE, nonce, sizeof(nonce)) != 0 ||
        memcmp(body + TBT_CRYPTO_HELLO_KEY, key, sizeof(key)) != 0)
    {
        printf("FAIL hello: nonce or key offset is wrong\n");
        failures++;
    }
    else
    {
        printf("PASS hello: nonce at +26 and key at +34\n");
    }

    memset(got_nonce, 0, sizeof(got_nonce));
    memset(got_key, 0, sizeof(got_key));
    if (tbt_crypto_hello_parse(body, sizeof(body), &enabled, &key_id, got_nonce, got_key) !=
            0 ||
        !enabled || key_id != TBT_CRYPTO_KEY_ID_HELLO ||
        memcmp(got_nonce, nonce, sizeof(nonce)) != 0 ||
        memcmp(got_key, key, sizeof(key)) != 0)
    {
        printf("FAIL hello: parse did not recover the fields\n");
        failures++;
    }
    else
    {
        printf("PASS hello: parse recovers the fields\n");
    }

    /* key_id=0 carries no key. */
    fill(body, sizeof(body), 0xa5);
    tbt_crypto_hello_fill(body, true, TBT_CRYPTO_KEY_ID_NONE, nonce, key);
    {
        u8 zero[32] = {0};

        if (memcmp(body + TBT_CRYPTO_HELLO_KEY, zero, sizeof(zero)) != 0 ||
            body[TBT_CRYPTO_HELLO_CRYPTO] != TBT_CRYPTO_HELLO_ENABLED)
        {
            printf("FAIL hello: key_id=0 still carries a key\n");
            failures++;
        }
        else
        {
            printf("PASS hello: key_id=0 zeroes the key field\n");
        }
    }

    /* Inconsistent or unknown fields are rejected. */
    body[TBT_CRYPTO_HELLO_RESERVED] = 1;
    if (tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) != -EPROTO)
    {
        printf("FAIL hello: nonzero reserved accepted\n");
        failures++;
    }
    else
    {
        printf("PASS hello: nonzero reserved rejected\n");
    }
    body[TBT_CRYPTO_HELLO_RESERVED] = 0;
    body[TBT_CRYPTO_HELLO_KEY_ID] = 2;
    if (tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) != -EPROTO)
    {
        printf("FAIL hello: unknown key_id accepted\n");
        failures++;
    }
    else
    {
        printf("PASS hello: unknown key_id rejected\n");
    }

    /* A truncated body cannot carry the frozen +26..+65 fields. */
    if (tbt_crypto_hello_parse(body, TBT_CRYPTO_HELLO_LEN - 1, NULL, NULL,
                               NULL, NULL) != -EPROTO)
    {
        printf("FAIL hello: truncated body accepted\n");
        failures++;
    }
    else
    {
        printf("PASS hello: truncated body rejected\n");
    }

    /* crypto=off writes a zero crypto byte and no key material. */
    fill(body, sizeof(body), 0xa5);
    tbt_crypto_hello_fill(body, false, TBT_CRYPTO_KEY_ID_NONE, NULL, NULL);
    {
        u8 zero[32] = {0};
        u8 zero_nonce[8] = {0};

        if (body[TBT_CRYPTO_HELLO_CRYPTO] != 0 ||
            body[TBT_CRYPTO_HELLO_KEY_ID] != TBT_CRYPTO_KEY_ID_NONE ||
            memcmp(body + TBT_CRYPTO_HELLO_NONCE, zero_nonce,
                   sizeof(zero_nonce)) != 0 ||
            memcmp(body + TBT_CRYPTO_HELLO_KEY, zero, sizeof(zero)) != 0)
        {
            printf("FAIL hello: crypto=off wrote fields\n");
            failures++;
        }
        else if (tbt_crypto_hello_parse(body, sizeof(body), &enabled, &key_id,
                                        got_nonce, got_key) != 0 ||
                 enabled || key_id != TBT_CRYPTO_KEY_ID_NONE)
        {
            printf("FAIL hello: crypto=off parse error\n");
            failures++;
        }
        else
        {
            printf("PASS hello: crypto=off is zero and parses\n");
        }
    }
    fill(body, sizeof(body), 0xa5);
    tbt_crypto_hello_fill(body, true, TBT_CRYPTO_KEY_ID_NONE, nonce, NULL);

    body[TBT_CRYPTO_HELLO_KEY_ID] = TBT_CRYPTO_KEY_ID_HELLO;
    body[TBT_CRYPTO_HELLO_CRYPTO] = TBT_CRYPTO_HELLO_ENABLED;
    if (tbt_crypto_hello_parse(body, sizeof(body), NULL, NULL, NULL, NULL) != -EPROTO)
    {
        printf("FAIL hello: key_id/key_in_hello mismatch accepted\n");
        failures++;
    }
    else
    {
        printf("PASS hello: key_id/key_in_hello mismatch rejected\n");
    }

    /* Resolve rules. */
    fill(local, sizeof(local), 7);
    if (tbt_crypto_resolve_key(true, local, true, TBT_CRYPTO_KEY_ID_NONE, key,
                               out) != 0 ||
        memcmp(out, local, sizeof(out)) != 0)
    {
        printf("FAIL key: local key did not win\n");
        failures++;
    }
    else
    {
        printf("PASS key: a configured crypto_key= wins\n");
    }
    if (tbt_crypto_resolve_key(false, NULL, true, TBT_CRYPTO_KEY_ID_HELLO, key,
                               out) != 0 ||
        memcmp(out, key, sizeof(out)) != 0)
    {
        printf("FAIL key: HELLO key was not adopted\n");
        failures++;
    }
    else
    {
        printf("PASS key: the HELLO key is adopted\n");
    }
    if (tbt_crypto_resolve_key(true, local, true, TBT_CRYPTO_KEY_ID_HELLO, key,
                               out) != -EPROTO)
    {
        printf("FAIL key: a local key plus a HELLO key was accepted\n");
        failures++;
    }
    else
    {
        printf("PASS key: local key plus HELLO key is a conflict\n");
    }
    if (tbt_crypto_resolve_key(false, NULL, true, TBT_CRYPTO_KEY_ID_NONE, key,
                               out) != -EPROTO)
    {
        printf("FAIL key: no key at all was accepted\n");
        failures++;
    }
    else
    {
        printf("PASS key: no key at all is rejected\n");
    }
    memset(out, 0xa5, sizeof(out));
    if (tbt_crypto_resolve_key(false, NULL, false, TBT_CRYPTO_KEY_ID_NONE,
                               NULL, out) != 0)
    {
        printf("FAIL key: crypto=off resolve error\n");
        failures++;
    }
    else
    {
        for (i = 0; i < sizeof(out); i++)
        {
            if (out[i] != 0)
            {
                printf("FAIL key: crypto=off did not zero the key\n");
                failures++;
                return;
            }
        }
        printf("PASS key: crypto=off resolves to a zero key\n");
    }
}

/* The full key path the session will use: near fills HELLO v6, far parses
 * it, both derive the same stream, and one share round-trips. */
static void test_direction_nonce(void)
{
    u8 key[32];
    u8 near_nonce[8];
    u8 far_nonce[8];
    u8 payload[512];
    u8 near_wire[512];
    u8 far_wire[512];
    u8 tmp[512];
    struct tbt_crypto_ctx near_tx;
    struct tbt_crypto_ctx near_rx;
    struct tbt_crypto_ctx far_tx;
    struct tbt_crypto_ctx far_rx;

    fill(key, sizeof(key), 0x51);
    fill(near_nonce, sizeof(near_nonce), 0x61);
    fill(far_nonce, sizeof(far_nonce), 0x91);
    fill(payload, sizeof(payload), 0x71);

    if (memcmp(near_nonce, far_nonce, sizeof(near_nonce)) == 0)
    {
        printf("FAIL direction: test nonces are equal\n");
        failures++;
        return;
    }

    /* T0 §2.3: tx uses this side's nonce, rx uses the peer's. */
    if (tbt_crypto_init(&near_tx, key, near_nonce, 0) != 0 ||
        tbt_crypto_init(&near_rx, key, far_nonce, 0) != 0 ||
        tbt_crypto_init(&far_tx, key, far_nonce, 0) != 0 ||
        tbt_crypto_init(&far_rx, key, near_nonce, 0) != 0)
    {
        printf("FAIL direction: init error\n");
        failures++;
        return;
    }

    /* Same key, same (seq, parity, block), different nonce: the keystream
     * must differ. */
    memcpy(near_wire, payload, sizeof(payload));
    memcpy(far_wire, payload, sizeof(payload));
    if (tbt_crypto_body_xor(&near_tx, 7, false, near_wire, sizeof(near_wire)) != 0 ||
        tbt_crypto_body_xor(&far_tx, 7, false, far_wire, sizeof(far_wire)) != 0)
    {
        printf("FAIL direction: body xor error\n");
        failures++;
        return;
    }
    if (memcmp(near_wire, far_wire, sizeof(payload)) == 0)
    {
        printf("FAIL direction: both directions reused the keystream\n");
        failures++;
    }
    else
    {
        printf("PASS direction: different nonces give different keystreams\n");
    }

    /* Each RX context opens its peer's stream, and only its peer's. */
    memcpy(tmp, near_wire, sizeof(tmp));
    if (tbt_crypto_body_xor(&far_rx, 7, false, tmp, sizeof(tmp)) != 0 ||
        memcmp(tmp, payload, sizeof(payload)) != 0)
    {
        printf("FAIL direction: far rx did not open near tx\n");
        failures++;
    }
    else
    {
        printf("PASS direction: far rx opens near tx\n");
    }

    memcpy(tmp, far_wire, sizeof(tmp));
    if (tbt_crypto_body_xor(&near_rx, 7, false, tmp, sizeof(tmp)) != 0 ||
        memcmp(tmp, payload, sizeof(payload)) != 0)
    {
        printf("FAIL direction: near rx did not open far tx\n");
        failures++;
    }
    else
    {
        printf("PASS direction: near rx opens far tx\n");
    }

    memcpy(tmp, near_wire, sizeof(tmp));
    tbt_crypto_body_xor(&near_rx, 7, false, tmp, sizeof(tmp));
    if (memcmp(tmp, payload, sizeof(payload)) == 0)
    {
        printf("FAIL direction: near rx accepted its own tx stream\n");
        failures++;
    }
    else
    {
        printf("PASS direction: a rx context rejects its own tx stream\n");
    }

    memcpy(tmp, far_wire, sizeof(tmp));
    tbt_crypto_body_xor(&far_rx, 7, false, tmp, sizeof(tmp));
    if (memcmp(tmp, payload, sizeof(payload)) == 0)
    {
        printf("FAIL direction: far rx accepted its own tx stream\n");
        failures++;
    }
    else
    {
        printf("PASS direction: far rx rejects its own tx stream\n");
    }
}

/* The pair helper keeps both directions in one object: TX seals with this
 * side's nonce, RX opens with the peer's. */
static void test_pair(void)
{
    u8 key[32];
    u8 near_nonce[8];
    u8 far_nonce[8];
    u8 payload[256];
    u8 near_wire[256];
    u8 far_wire[256];
    u8 tmp[256];
    struct tbt_crypto_pair near;
    struct tbt_crypto_pair far;
    struct tbt_crypto_pair bad;

    fill(key, sizeof(key), 0x19);
    fill(near_nonce, sizeof(near_nonce), 0x29);
    fill(far_nonce, sizeof(far_nonce), 0x39);
    fill(payload, sizeof(payload), 0x49);

    /* One nonce for both directions is the keystream reuse T0 §2.4 forbids. */
    if (tbt_crypto_pair_init(&bad, key, near_nonce, near_nonce) != -EINVAL)
    {
        printf("FAIL pair: equal nonces accepted\n");
        failures++;
    }
    else
    {
        printf("PASS pair: equal nonces rejected\n");
    }
    if (tbt_crypto_pair_init(NULL, key, near_nonce, far_nonce) != -EINVAL ||
        tbt_crypto_pair_init(&bad, key, NULL, far_nonce) != -EINVAL ||
        tbt_crypto_pair_init(&bad, key, near_nonce, NULL) != -EINVAL)
    {
        printf("FAIL pair: NULL argument accepted\n");
        failures++;
    }
    else
    {
        printf("PASS pair: NULL argument rejected\n");
    }

    /* near: tx = near_nonce, rx = far_nonce; far is the mirror image. */
    if (tbt_crypto_pair_init(&near, key, near_nonce, far_nonce) != 0 ||
        tbt_crypto_pair_init(&far, key, far_nonce, near_nonce) != 0)
    {
        printf("FAIL pair: init error\n");
        failures++;
        return;
    }

    memcpy(near_wire, payload, sizeof(payload));
    memcpy(far_wire, payload, sizeof(payload));
    if (tbt_crypto_body_xor(&near.tx, 9, true, near_wire,
                            sizeof(near_wire)) != 0 ||
        tbt_crypto_body_xor(&far.tx, 9, true, far_wire,
                            sizeof(far_wire)) != 0)
    {
        printf("FAIL pair: body xor error\n");
        failures++;
        return;
    }
    if (memcmp(near_wire, far_wire, sizeof(payload)) == 0)
    {
        printf("FAIL pair: both directions produced the same stream\n");
        failures++;
    }

    memcpy(tmp, near_wire, sizeof(tmp));
    if (tbt_crypto_body_xor(&far.rx, 9, true, tmp, sizeof(tmp)) != 0 ||
        memcmp(tmp, payload, sizeof(payload)) != 0)
    {
        printf("FAIL pair: far rx did not open near tx\n");
        failures++;
    }
    else
    {
        printf("PASS pair: far rx opens near tx\n");
    }

    memcpy(tmp, far_wire, sizeof(tmp));
    if (tbt_crypto_body_xor(&near.rx, 9, true, tmp, sizeof(tmp)) != 0 ||
        memcmp(tmp, payload, sizeof(payload)) != 0)
    {
        printf("FAIL pair: near rx did not open far tx\n");
        failures++;
    }
    else
    {
        printf("PASS pair: near rx opens far tx\n");
    }

    /* A wrong pairing (near tx against far tx) must not decrypt. */
    memcpy(tmp, near_wire, sizeof(tmp));
    tbt_crypto_body_xor(&far.tx, 9, true, tmp, sizeof(tmp));
    if (memcmp(tmp, payload, sizeof(payload)) == 0)
    {
        printf("FAIL pair: near tx stream opened by far tx\n");
        failures++;
    }
    else
    {
        printf("PASS pair: a mismatched pairing does not decrypt\n");
    }

    /* crypto=off: both directions are no-ops. */
    if (tbt_crypto_pair_init(&bad, NULL, NULL, NULL) != 0)
    {
        printf("FAIL pair: crypto=off init error\n");
        failures++;
        return;
    }
    memcpy(tmp, payload, sizeof(tmp));
    if (tbt_crypto_body_xor(&bad.tx, 9, false, tmp, sizeof(tmp)) != 0 ||
        tbt_crypto_body_xor(&bad.rx, 9, false, tmp, sizeof(tmp)) != 0 ||
        memcmp(tmp, payload, sizeof(payload)) != 0)
    {
        printf("FAIL pair: crypto=off transformed the payload\n");
        failures++;
    }
    else
    {
        printf("PASS pair: crypto=off is a no-op in both directions\n");
    }

    tbt_crypto_pair_wipe(&near);
    if (near.tx.ready || near.rx.ready)
    {
        printf("FAIL pair: wipe left a ready context\n");
        failures++;
    }
    else
    {
        printf("PASS pair: wipe clears both directions\n");
    }
}

/* Parity shares of one block share (seq, parity) and differ only by
 * share_index; the per-share variant must mix it into the nonce. */
static void test_body_share(void)
{
    u8 key[32];
    u8 nonce[8];
    u8 payload[1024];
    u8 a[1024];
    u8 b[1024];
    struct tbt_crypto_ctx ctx;
    u64 seq = 77;

    fill(key, sizeof(key), 0x81);
    fill(nonce, sizeof(nonce), 0x82);
    fill(payload, sizeof(payload), 0x83);

    if (tbt_crypto_init(&ctx, key, nonce, 0) != 0)
    {
        printf("FAIL share: init error\n");
        failures++;
        return;
    }

    memcpy(a, payload, sizeof(a));
    memcpy(b, payload, sizeof(b));
    if (tbt_crypto_body_xor_share(&ctx, seq, true, 0, a, sizeof(a)) != 0 ||
        tbt_crypto_body_xor_share(&ctx, seq, true, 1, b, sizeof(b)) != 0)
    {
        printf("FAIL share: xor error\n");
        failures++;
        return;
    }
    if (memcmp(a, b, sizeof(a)) == 0)
    {
        printf("FAIL share: parity shares reused the keystream\n");
        failures++;
    }
    else
    {
        printf("PASS share: parity share_index separates the keystream\n");
    }

    if (tbt_crypto_body_xor_share(&ctx, seq, true, 0, a, sizeof(a)) != 0 ||
        memcmp(a, payload, sizeof(a)) != 0 ||
        tbt_crypto_body_xor_share(&ctx, seq, true, 1, b, sizeof(b)) != 0 ||
        memcmp(b, payload, sizeof(b)) != 0)
    {
        printf("FAIL share: parity round trip failed\n");
        failures++;
    }
    else
    {
        printf("PASS share: parity shares round trip\n");
    }

    /* DATA is unique by seq; the share argument must not change it. */
    memcpy(a, payload, sizeof(a));
    memcpy(b, payload, sizeof(b));
    if (tbt_crypto_body_xor_share(&ctx, seq, false, 3, a, sizeof(a)) != 0 ||
        tbt_crypto_body_xor(&ctx, seq, false, b, sizeof(b)) != 0 ||
        memcmp(a, b, sizeof(a)) != 0)
    {
        printf("FAIL share: DATA path changed with share_index\n");
        failures++;
    }
    else
    {
        printf("PASS share: DATA path is independent of share_index\n");
    }

    /* share_index 0 is the base parity stream. */
    memcpy(a, payload, sizeof(a));
    memcpy(b, payload, sizeof(b));
    tbt_crypto_body_xor_share(&ctx, seq, true, 0, a, sizeof(a));
    tbt_crypto_body_xor(&ctx, seq, true, b, sizeof(b));
    if (memcmp(a, b, sizeof(a)) != 0)
    {
        printf("FAIL share: share 0 differs from the base parity stream\n");
        failures++;
    }
    else
    {
        printf("PASS share: share 0 is the base parity stream\n");
    }
}

/* The full key path the session will use: each side fills its own HELLO v6
 * with its own TX nonce, parses the peer's, and keeps tx/rx contexts. */
static void test_session_path(void)
{
    u8 key[32];
    u8 near_nonce[8];
    u8 far_nonce[8];
    u8 hello[66];
    u8 got_nonce[8];
    u8 hello_key[32];
    u8 session_key[32];
    u8 payload[1024];
    u8 near_wire[1024];
    u8 far_wire[1024];
    struct tbt_crypto_ctx near_tx;
    struct tbt_crypto_ctx near_rx;
    struct tbt_crypto_ctx far_tx;
    struct tbt_crypto_ctx far_rx;
    bool enabled = false;
    u8 key_id = 0xff;

    fill(key, sizeof(key), 0x11);
    fill(near_nonce, sizeof(near_nonce), 0x22);
    fill(far_nonce, sizeof(far_nonce), 0x44);
    fill(payload, sizeof(payload), 0x33);

    /* Near's HELLO: key plus near's TX nonce. */
    memset(hello, 0, sizeof(hello));
    tbt_crypto_hello_fill(hello, true, TBT_CRYPTO_KEY_ID_HELLO, near_nonce,
                          key);

    if (tbt_crypto_hello_parse(hello, sizeof(hello), &enabled, &key_id,
                               got_nonce, hello_key) != 0 ||
        !enabled || key_id != TBT_CRYPTO_KEY_ID_HELLO ||
        memcmp(got_nonce, near_nonce, sizeof(near_nonce)) != 0)
    {
        printf("FAIL session: HELLO parse error\n");
        failures++;
        return;
    }
    if (tbt_crypto_resolve_key(false, NULL, enabled, key_id, hello_key,
                               session_key) != 0)
    {
        printf("FAIL session: far key resolve error\n");
        failures++;
        return;
    }

    /* Far's own HELLO carries its own TX nonce; the key field is empty
     * because the far side adopted the near key. */
    memset(hello, 0, sizeof(hello));
    tbt_crypto_hello_fill(hello, true, TBT_CRYPTO_KEY_ID_NONE, far_nonce, NULL);

    if (tbt_crypto_init(&near_tx, key, near_nonce, 0) != 0 ||
        tbt_crypto_init(&near_rx, key, far_nonce, 0) != 0 ||
        tbt_crypto_init(&far_tx, session_key, far_nonce, 0) != 0 ||
        tbt_crypto_init(&far_rx, session_key, near_nonce, 0) != 0)
    {
        printf("FAIL session: init error\n");
        failures++;
        return;
    }

    /* near -> far, seq 42. */
    memcpy(near_wire, payload, sizeof(payload));
    if (tbt_crypto_body_xor(&near_tx, 42, false, near_wire,
                            sizeof(near_wire)) != 0 ||
        tbt_crypto_body_xor(&far_rx, 42, false, near_wire,
                            sizeof(near_wire)) != 0)
    {
        printf("FAIL session: near->far body xor error\n");
        failures++;
        return;
    }
    if (memcmp(near_wire, payload, sizeof(payload)) != 0)
    {
        printf("FAIL session: HELLO key did not decrypt the share\n");
        failures++;
        return;
    }
    printf("PASS session: near key -> HELLO -> far decrypt\n");

    /* far -> near, the same seq: a different nonce, so a different stream. */
    memcpy(far_wire, payload, sizeof(payload));
    if (tbt_crypto_body_xor(&far_tx, 42, false, far_wire,
                            sizeof(far_wire)) != 0)
    {
        printf("FAIL session: far tx error\n");
        failures++;
        return;
    }
    {
        u8 near_cipher[1024];

        memcpy(near_cipher, payload, sizeof(payload));
        tbt_crypto_body_xor(&near_tx, 42, false, near_cipher,
                            sizeof(near_cipher));
        if (memcmp(far_wire, near_cipher, sizeof(payload)) == 0)
        {
            printf("FAIL session: both directions produced the same cipher\n");
            failures++;
            return;
        }
    }
    if (tbt_crypto_body_xor(&near_rx, 42, false, far_wire,
                            sizeof(far_wire)) != 0 ||
        memcmp(far_wire, payload, sizeof(payload)) != 0)
    {
        printf("FAIL session: far tx did not decrypt at near rx\n");
        failures++;
    }
    else
    {
        printf("PASS session: per-direction nonce, both ways\n");
    }

    /* Pre-shared crypto_key= on both ends: key_id 0, no key in HELLO, but
     * each direction still uses its own nonce. */
    memset(hello, 0, sizeof(hello));
    tbt_crypto_hello_fill(hello, true, TBT_CRYPTO_KEY_ID_NONE, near_nonce,
                          NULL);
    if (tbt_crypto_hello_parse(hello, sizeof(hello), &enabled, &key_id,
                               got_nonce, hello_key) != 0 ||
        tbt_crypto_resolve_key(true, key, enabled, key_id, hello_key,
                               session_key) != 0 ||
        memcmp(session_key, key, sizeof(key)) != 0)
    {
        printf("FAIL session: pre-shared key path\n");
        failures++;
    }
    else
    {
        printf("PASS session: crypto_key= pre-shared path\n");
    }
}

int main(void)
{
    test_tc1();
    test_zero_key_256();
    test_generated_256();
    test_rfc8439_block();
    test_rfc8439_encrypt();
    test_counter_positions();
    test_streaming();
    test_xor_at();
    test_counter_wrap();
    test_invalid_args();
    test_wipe();
    test_body_xor();
    test_body_share();
    test_hello();
    test_direction_nonce();
    test_pair();
    test_session_path();

    if (failures != 0)
    {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
