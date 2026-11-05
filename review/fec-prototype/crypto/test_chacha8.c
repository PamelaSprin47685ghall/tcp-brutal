/*
 * test_chacha8.c - known-answer and structural tests for chacha8.c.
 *
 * Vectors:
 *   TC1 (ChaCha8, 128-bit zero key, zero IV): secworks/chacha_testvectors,
 *       commit a56fff0, BSD-2; also draft-irtf-cfrg-chacha20-poly1305.
 *   Zero-key ChaCha12/ChaCha20 and the 256-bit zero-key ChaCha8 blocks:
 *       the same file.
 *   RFC 8439 sections 2.3.2 and 2.4.2: ChaCha20 block and encryption.
 *   Generated 256-bit ChaCha8 vector: Bernstein eSTREAM chacha8/ref,
 *       tiny-chacha and Linux chacha_block_generic(..., 8) agree; see
 *       review/chacha8-reference.md section 7.2.
 *
 * Build:
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -o test_chacha8 \
 *      test_chacha8.c chacha8.c
 * Run:
 *   ./test_chacha8
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chacha8.h"

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

static int from_hex(const char *hex, uint8_t *out, size_t out_len)
{
    size_t i;

    if (strlen(hex) != out_len * 2)
        return -1;

    for (i = 0; i < out_len; i++) {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void print_hex(const char *prefix, const uint8_t *p, size_t n)
{
    size_t i;

    printf("%s", prefix);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static void check_bytes(const char *name, const uint8_t *got,
                        const char *want_hex, size_t n)
{
    uint8_t want[256];

    if (n > sizeof(want) || from_hex(want_hex, want, n) != 0) {
        printf("FAIL %s: malformed test vector\n", name);
        failures++;
        return;
    }

    if (memcmp(got, want, n) != 0) {
        printf("FAIL %s\n", name);
        print_hex("  got  ", got, n);
        print_hex("  want ", want, n);
        failures++;
        return;
    }

    printf("PASS %s\n", name);
}

static void check_keystream(const char *name,
                            const uint8_t *key, size_t key_len,
                            const uint8_t *nonce, uint64_t counter,
                            int rounds, size_t len, const char *want_hex)
{
    uint8_t in[256] = {0};
    uint8_t out[256];
    chacha8_ctx ctx;

    if (len > sizeof(in)) {
        printf("FAIL %s: vector too long\n", name);
        failures++;
        return;
    }

    if (chacha8_init(&ctx, key, key_len, nonce, counter, rounds) != 0
        || chacha8_crypt(&ctx, in, len, out) != 0) {
        printf("FAIL %s: init/crypt error\n", name);
        failures++;
        return;
    }

    check_bytes(name, out, want_hex, len);
}

static void test_tc1(void)
{
    static const char *want =
        "e28a5fa4a67f8c5defed3e6fb7303486aa8427d31419a729572d777953491120"
        "b64ab8e72b8deb85cd6aea7cb6089a101824beeb08814a428aab1fa2c816081b"
        "8a26af448a1ba906368fd8c83831c18cec8ced811a028e675b8d2be8fce08116"
        "5ceae9f1d1b7a975497749480569ceb83de6a0a587d4984f19925f5d338e430d";
    uint8_t key[16] = {0};
    uint8_t nonce[8] = {0};

    check_keystream("TC1 ChaCha8 128-bit zero key, blocks 0-1",
                    key, sizeof(key), nonce, 0, 8, 128, want);
}

static void test_zero_key_256(void)
{
    static const uint8_t key[32] = {0};
    static const uint8_t nonce[8] = {0};

    check_keystream("ChaCha8 256-bit zero key, block 0",
                    key, sizeof(key), nonce, 0, 8, 64,
                    "3e00ef2f895f40d67f5bb8e81f09a5a1"
                    "2c840ec3ce9a7f3b181be188ef711a1e"
                    "984ce172b9216f419f445367456d5619"
                    "314a42a3da86b001387bfdb80e0cfe42");

    check_keystream("ChaCha12 256-bit zero key, block 0",
                    key, sizeof(key), nonce, 0, 12, 64,
                    "9bf49a6a0755f953811fce125f2683d5"
                    "0429c3bb49e074147e0089a52eae155f"
                    "0564f879d27ae3c02ce82834acfa8c79"
                    "3a629f2ca0de6919610be82f411326be");

    check_keystream("ChaCha20 256-bit zero key, block 0",
                    key, sizeof(key), nonce, 0, 20, 64,
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
    uint8_t key[32];
    uint8_t nonce[8];
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (uint8_t)(0xa0 + i);

    check_keystream("ChaCha8 key 00..1f, nonce a0..a7, 128 bytes",
                    key, sizeof(key), nonce, 0, 8, 128, want);
}

static void test_rfc8439_block(void)
{
    static const char *want =
        "10f1e7e4d13b5915500fdd1fa32071c4"
        "c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2"
        "b5129cd1de164eb9cbd083e8a2503c4e";
    uint8_t key[32];
    uint8_t nonce[8] = {0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;

    /*
     * RFC 8439 state has state[12] = 1 and state[13..15] = nonce
     * 00:00:00:09:00:00:00:4a:00:00:00:00. Our 64-bit counter takes the
     * high word from nonce[0..3]: counter = 0x09000000_00000001, and the
     * 8-byte nonce is 00:00:00:4a:00:00:00:00.
     */
    check_keystream("RFC 8439 2.3.2 ChaCha20 block, counter 1",
                    key, sizeof(key), nonce, 0x0900000000000001ULL,
                    20, 64, want);
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
    uint8_t key[32];
    uint8_t nonce[8] = {0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    uint8_t plain[114];
    uint8_t cipher[114];
    uint8_t out[114];
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;
    if (from_hex(plain_hex, plain, sizeof(plain)) != 0
        || from_hex(cipher_hex, cipher, sizeof(cipher)) != 0) {
        printf("FAIL RFC 8439 2.4.2: malformed vector\n");
        failures++;
        return;
    }

    if (chacha8_xor(key, sizeof(key), nonce, 1, 20,
                    plain, sizeof(plain), out) != 0) {
        printf("FAIL RFC 8439 2.4.2: crypt error\n");
        failures++;
        return;
    }

    check_bytes("RFC 8439 2.4.2 ChaCha20 encryption", out, cipher_hex,
                sizeof(cipher));
}

static void test_counter_positions(void)
{
    uint8_t key[32];
    uint8_t nonce[8];
    uint8_t zero[128] = {0};
    uint8_t stream0[128];
    uint8_t stream1[64];
    uint8_t carry0[128];
    uint8_t carry1[64];
    chacha8_ctx ctx;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)(0x11 * i + 5);
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (uint8_t)(0x80 + i);

    /* Blocks at counter 0 and counter 1 must be consecutive. */
    if (chacha8_init(&ctx, key, sizeof(key), nonce, 0, 8) != 0
        || chacha8_crypt(&ctx, zero, sizeof(stream0), stream0) != 0
        || chacha8_init(&ctx, key, sizeof(key), nonce, 1, 8) != 0
        || chacha8_crypt(&ctx, zero, sizeof(stream1), stream1) != 0) {
        printf("FAIL counter start: crypt error\n");
        failures++;
        return;
    }
    if (memcmp(stream1, stream0 + 64, 64) != 0) {
        printf("FAIL counter start: block mismatch\n");
        failures++;
    } else {
        printf("PASS counter start: counter 1 matches block 1 of counter 0\n");
    }

    /* Low-word wrap must carry into the high word. */
    if (chacha8_init(&ctx, key, sizeof(key), nonce, 0xffffffffULL, 8) != 0
        || chacha8_crypt(&ctx, zero, sizeof(carry0), carry0) != 0
        || chacha8_init(&ctx, key, sizeof(key), nonce, 0x100000000ULL, 8) != 0
        || chacha8_crypt(&ctx, zero, sizeof(carry1), carry1) != 0) {
        printf("FAIL counter carry: crypt error\n");
        failures++;
        return;
    }
    if (memcmp(carry1, carry0 + 64, 64) != 0) {
        printf("FAIL counter carry: 0xffffffff+1 mismatch\n");
        failures++;
    } else {
        printf("PASS counter carry: 0xffffffff + 1 == 0x100000000\n");
    }
}

static void test_streaming(void)
{
    uint8_t key[32];
    uint8_t nonce[8];
    uint8_t buf[512];
    uint8_t one[512];
    uint8_t chunked[512];
    uint8_t inplace[512];
    uint8_t block0[64];
    uint8_t zero64[64] = {0};
    static const size_t chunks[] = {1, 63, 64, 65, 100, 7, 212};
    chacha8_ctx ctx;
    size_t off;
    size_t i;
    int rc;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)(i * 7 + 3);
    for (i = 0; i < sizeof(nonce); i++)
        nonce[i] = (uint8_t)(0xf0 - i);
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 31 + 11);

    if (chacha8_xor(key, sizeof(key), nonce, 12345, 8,
                    buf, sizeof(buf), one) != 0) {
        printf("FAIL streaming: one-shot error\n");
        failures++;
        return;
    }

    if (chacha8_init(&ctx, key, sizeof(key), nonce, 12345, 8) != 0) {
        printf("FAIL streaming: init error\n");
        failures++;
        return;
    }
    off = 0;
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        size_t n = chunks[i];
        if (off + n > sizeof(buf)) {
            printf("FAIL streaming: chunk list too long\n");
            failures++;
            return;
        }
        if (chacha8_crypt(&ctx, buf + off, n, chunked + off) != 0) {
            printf("FAIL streaming: chunk error\n");
            failures++;
            return;
        }
        off += n;
    }
    if (off != sizeof(buf)) {
        printf("FAIL streaming: chunk list covers %zu of %zu bytes\n",
               off, sizeof(buf));
        failures++;
    } else if (memcmp(chunked, one, sizeof(one)) != 0) {
        printf("FAIL streaming: chunked output differs\n");
        failures++;
    } else {
        printf("PASS streaming: 7 chunks match one-shot\n");
    }

    memcpy(inplace, buf, sizeof(buf));
    if (chacha8_init(&ctx, key, sizeof(key), nonce, 12345, 8) != 0
        || chacha8_crypt(&ctx, inplace, sizeof(inplace), inplace) != 0) {
        printf("FAIL in-place: crypt error\n");
        failures++;
    } else if (memcmp(inplace, one, sizeof(one)) != 0) {
        printf("FAIL in-place: output differs\n");
        failures++;
    } else {
        printf("PASS in-place: in == out matches one-shot\n");
    }

    if (chacha8_init(&ctx, key, sizeof(key), nonce, 0, 8) != 0
        || chacha8_crypt(&ctx, zero64, sizeof(zero64), block0) != 0) {
        printf("FAIL partial lengths: setup error\n");
        failures++;
        return;
    }

    /*
     * Every partial length must match the prefix of a full block. The input
     * must be the same zero buffer that produced block0: with any other input
     * the output is input ^ keystream, not the keystream itself.
     */
    rc = 0;
    for (i = 1; i <= 64; i++) {
        uint8_t part[64];
        if (chacha8_init(&ctx, key, sizeof(key), nonce, 0, 8) != 0
            || chacha8_crypt(&ctx, zero64, i, part) != 0
            || memcmp(part, block0, i) != 0) {
            rc = -1;
            break;
        }
    }
    if (rc != 0) {
        printf("FAIL partial lengths: mismatch at %zu bytes\n", i);
        failures++;
    } else {
        printf("PASS partial lengths: 1..64 bytes match prefix\n");
    }
}

static void test_counter_wrap(void)
{
    uint8_t key[32] = {0};
    uint8_t nonce[8] = {0};
    uint8_t in[65] = {0};
    uint8_t out[65];
    chacha8_ctx ctx;
    int rc;

    if (chacha8_init(&ctx, key, sizeof(key), nonce, UINT64_MAX, 8) != 0) {
        printf("FAIL counter wrap: init error\n");
        failures++;
        return;
    }

    if (chacha8_crypt(&ctx, in, 64, out) != 0) {
        printf("FAIL counter wrap: last block rejected\n");
        failures++;
        return;
    }

    rc = chacha8_crypt(&ctx, in, 1, out);
    if (rc != -1) {
        printf("FAIL counter wrap: post-wrap call returned %d\n", rc);
        failures++;
    } else {
        printf("PASS counter wrap: post-wrap call rejected\n");
    }

    if (chacha8_init(&ctx, key, sizeof(key), nonce, UINT64_MAX, 8) != 0) {
        printf("FAIL counter wrap: re-init error\n");
        failures++;
        return;
    }
    rc = chacha8_crypt(&ctx, in, 65, out);
    if (rc != -1) {
        printf("FAIL counter wrap: 65-byte call returned %d\n", rc);
        failures++;
    } else {
        printf("PASS counter wrap: 65-byte call fails after first block\n");
    }
}

static void test_invalid_args(void)
{
    uint8_t key[32] = {0};
    uint8_t nonce[8] = {0};
    uint8_t buf[16] = {0};
    chacha8_ctx ctx;
    int bad;

    bad = 0;
    bad += chacha8_init(NULL, key, 32, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, NULL, 32, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 32, NULL, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 0, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 15, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 24, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 33, nonce, 0, 8) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 0) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 7) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 9) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 19) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 21) == -1;
    bad += chacha8_crypt(NULL, buf, 1, buf) == -1;
    bad += chacha8_init(&ctx, key, 32, nonce, 0, 8) == 0;
    bad += chacha8_crypt(&ctx, NULL, 1, buf) == -1;
    bad += chacha8_crypt(&ctx, buf, 1, NULL) == -1;
    bad += chacha8_crypt(&ctx, NULL, 0, NULL) == 0;
    bad += chacha8_xor(NULL, 32, nonce, 0, 8, buf, 16, buf) == -1;
    bad += chacha8_xor(key, 32, nonce, 0, 8, buf, 16, NULL) == -1;

    if (bad != 19) {
        printf("FAIL invalid args: %d of 19 checks passed\n", bad);
        failures++;
    } else {
        printf("PASS invalid args: 19 rejection checks\n");
    }
}

static void test_wipe(void)
{
    uint8_t key[32];
    uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t buf[64] = {0};
    uint8_t out[64];
    chacha8_ctx ctx;
    const volatile uint8_t *p = (const volatile uint8_t *)&ctx;
    size_t i;

    for (i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)(i + 1);

    if (chacha8_init(&ctx, key, sizeof(key), nonce, 42, 8) != 0
        || chacha8_crypt(&ctx, buf, sizeof(buf), out) != 0) {
        printf("FAIL wipe: setup error\n");
        failures++;
        return;
    }

    chacha8_wipe(&ctx);
    for (i = 0; i < sizeof(ctx); i++) {
        if (p[i] != 0) {
            printf("FAIL wipe: byte %zu not zero\n", i);
            failures++;
            return;
        }
    }
    printf("PASS wipe: context zeroed\n");
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
    test_counter_wrap();
    test_invalid_args();
    test_wipe();

    if (failures != 0) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }

    printf("\nall checks passed\n");
    return 0;
}
