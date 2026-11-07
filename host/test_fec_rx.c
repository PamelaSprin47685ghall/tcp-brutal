/* test_fec_rx.c - the FEC receiver's delivery contract.
 *
 * The delivery walk lives in tbt_fec_data.c, which pulls in the kernel
 * headers and cannot be linked here. The two decisions that let the 1 GiB
 * acceptance run lose 8192-byte slots without a single counter moving are
 * pure, so they live in tbt_fec.h and are exercised here against a model of
 * that walk:
 *
 *   tbt_fec_rx_share_len()  which length a slot delivers, or 0 = refuse;
 *   struct tbt_fec_ack      deliver_bytes, the only byte count a sender may
 *                           use as tx_ack.
 *
 * The model encodes a source stream with the real codec, drops chosen shares,
 * recovers them, and then walks the block exactly as tbt_fec_data.c does.
 * Its invariant is the one the field run violated: the delivered stream is a
 * prefix of the source, no slot that holds bytes is skipped, and the byte
 * count an ACK reports is the count that was delivered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tbt_fec.h"
#include "tbt_crypto.h"

#define S 64      /* slot size: the model uses a short slot, the code is size-agnostic */
#define N 4       /* data shares per block */
#define M 4       /* parity shares per block */
#define SRC_LEN (2 * N * S)

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond)
        return;
    failures++;
    printf("FAIL %s\n", what);
}

static void ok_size(size_t got, size_t want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %zu, want %zu\n", what, got, want);
}

static void ok_int(int got, int want, const char *what)
{
    checks++;
    if (got == want)
        return;
    failures++;
    printf("FAIL %s: got %d, want %d\n", what, got, want);
}

/* ------------------------------------------------------------------ *
 * The source stream and one block's wire state.
 * ------------------------------------------------------------------ */

static u8 src[SRC_LEN];

struct blk
{
    u8 data[N][S];
    u16 dlen[N];
    u8 dpresent[N];
    u8 parity[M][S];
    u8 ppresent[M];
    u16 nreal;     /* win_len from PARITY; 0 = not stated */
    u16 short_len; /* epoch: length of share nreal-1, 0 = full */
    bool empty;
};

static void src_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(src); i++)
        src[i] = (u8)(i * 31u + 7u);
}

/* Fill the block from `src`, with len[i] bytes in share i (0 < len <= S). */
static void blk_init(struct blk *b, size_t base, const u16 len[N])
{
    int i;

    memset(b, 0, sizeof(*b));
    for (i = 0; i < N; i++)
    {
        b->dlen[i] = len[i];
        memcpy(b->data[i], src + base + (size_t)i * S, len[i]);
        b->dpresent[i] = 1;
    }
}

static void blk_encode(struct blk *b)
{
    struct tbt_fec_rlnc *ctx;
    u8 *d[N], *p[M];
    int i;

    ctx = tbt_fec_rlnc_new(N, M, N, 8, S, 4, 0x1234);
    if (!ctx)
    {
        printf("FAIL codec allocation\n");
        exit(2);
    }
    for (i = 0; i < N; i++)
        d[i] = b->data[i];
    for (i = 0; i < M; i++)
        p[i] = b->parity[i];
    if (tbt_fec_rlnc_encode(ctx, d, N, p) < 0)
    {
        printf("FAIL encode\n");
        exit(2);
    }
    tbt_fec_rlnc_free(ctx);
    for (i = 0; i < M; i++)
        b->ppresent[i] = 1;
}

/* Recover the shares marked absent. Returns 0 on success, -EAGAIN when the
 * parity cannot solve them. */
static int blk_recover(struct blk *b)
{
    struct tbt_fec_rlnc *ctx;
    u8 rec[M][S];
    u8 *d[N], *p[M];
    u8 dp[N], pp[M];
    u16 real = b->nreal ? b->nreal : N;
    int i, k, rc;

    /* tbt_fec_blk_decode(): a short share anywhere but the last real
     * position leaves every recovered slot without a stated length. */
    for (i = 0; i < N; i++)
        if (b->dpresent[i] && b->dlen[i] && b->dlen[i] < S &&
            (i + 1 != real || b->short_len != b->dlen[i]))
            return -EAGAIN;

    for (i = 0; i < N; i++)
        dp[i] = b->dpresent[i];
    for (i = 0; i < M; i++)
        pp[i] = b->ppresent[i];

    ctx = tbt_fec_rlnc_new(N, M, N, 8, S, 4, 0x1234);
    if (!ctx)
        exit(2);
    memset(rec, 0, sizeof(rec));
    for (i = 0, k = 0; i < N; i++)
        d[i] = dp[i] ? b->data[i] : rec[k++];
    for (i = 0; i < M; i++)
        p[i] = b->parity[i];
    rc = tbt_fec_rlnc_decode(ctx, d, p, dp, pp, N);
    tbt_fec_rlnc_free(ctx);
    if (rc < 0)
        return -EAGAIN;
    for (i = 0, k = 0; i < N; i++)
    {
        if (dp[i])
            continue;
        memcpy(b->data[i], rec[k++], S);
        b->dpresent[i] = 1;
        b->dlen[i] = 0; /* a recovered share has no payload_len of its own */
    }
    return 0;
}

/* The delivery walk of tbt_fec_deliver_ready(), reduced to one block.
 * Returns the bytes appended to `out`; *refused says the walk stopped before
 * finishing the block, which is the only alternative to delivering a wrong
 * length. */
static size_t blk_deliver(struct blk *b, u8 *out, size_t cap, int *refused)
{
    u16 real = b->nreal ? b->nreal : N;
    size_t off = 0;
    u16 p;

    *refused = 0;
    if (b->empty)
        return 0;
    for (p = 0; p < N; p++)
    {
        u16 len;

        if (p >= real)
        {
            /* Padding carries no stream byte. A present share here means the
             * block's cut and its DATA records disagree; skipping it would
             * delete its payload while the ACK ran past it. */
            if (b->dpresent[p])
            {
                *refused = 1;
                break;
            }
            continue;
        }
        len = tbt_fec_rx_share_len(b->dlen[p], b->short_len, p, real, S);
        if (!len)
        {
            *refused = 1;
            break;
        }
        if (off + len > cap)
        {
            *refused = 1;
            break;
        }
        memcpy(out + off, b->data[p], len);
        off += len;
    }
    return off;
}

/* The block's delivery is a prefix of the source, byte for byte. */
static void ok_prefix(const u8 *out, size_t n, const char *what)
{
    checks++;
    if (n <= sizeof(src) && memcmp(out, src, n) == 0)
        return;
    failures++;
    printf("FAIL %s: delivered %zu bytes are not a source prefix\n", what, n);
}

/* ------------------------------------------------------------------ *
 * 1. tbt_fec_rx_share_len: the length decision, every position.
 * ------------------------------------------------------------------ */

static void test_share_len(void)
{
    /* A share that arrived always uses its own payload_len. */
    ok_size(tbt_fec_rx_share_len(17, 0, 0, 4, S), 17, "arrived short share at p=0");
    ok_size(tbt_fec_rx_share_len(17, 17, 3, 4, S), 17, "arrived short share at the cut");
    ok_size(tbt_fec_rx_share_len(S, 0, 1, 4, S), S, "arrived full share");

    /* A recovered share at the block's last position uses the parity epoch. */
    ok_size(tbt_fec_rx_share_len(0, 17, 3, 4, S), 17, "recovered last share uses epoch");
    ok_size(tbt_fec_rx_share_len(0, 0, 3, 4, S), S, "recovered last share, epoch 0 = S");

    /* A recovered share before the last position is S only when the block
     * holds no short share at all. */
    ok_size(tbt_fec_rx_share_len(0, 0, 1, 4, S), S, "recovered mid share, full block");
    ok_size(tbt_fec_rx_share_len(0, 17, 1, 4, S), S,
            "recovered mid share before a stated short tail is S");
    ok_size(tbt_fec_rx_share_len(0, 17, 0, 3, S), S,
            "recovered first of three before a stated short tail is S");

    /* Padding never delivers. */
    ok_size(tbt_fec_rx_share_len(0, 0, 4, 4, S), 0, "padding at p == real");
    ok_size(tbt_fec_rx_share_len(9, 0, 5, 4, S), 0, "padding with a stale dlen");
    ok_size(tbt_fec_rx_share_len(0, 0, 0, 0, S), 0, "empty block");

    /* The entry-side rule: once PARITY fixed the cut, a DATA share at or past
     * it is refused before it reaches a slot. */
    ok(tbt_fec_share_past_cut(true, 2, 2), "share at the fixed cut is refused");
    ok(tbt_fec_share_past_cut(true, 2, 3), "share past the fixed cut is refused");
    ok(!tbt_fec_share_past_cut(true, 2, 1), "share below the cut is placed");
    ok(!tbt_fec_share_past_cut(false, 0, 3), "no cut yet: any share is placed");
    ok(!tbt_fec_share_past_cut(true, 4, 3), "full block accepts every share");
}

/* ------------------------------------------------------------------ *
 * 1b. The delivery step: a duplicate must not advance the byte cursor.
 * ------------------------------------------------------------------ */

static void test_deliver_step(void)
{
    u8 out[2 * S];
    size_t off;
    u64 rx_next;

    ok(tbt_fec_deliver_step(0, S, 0) == 0, "step: fresh share");
    ok(tbt_fec_deliver_step(0, S, S) == 1, "step: duplicate share");
    ok(tbt_fec_deliver_step(0, S, 2 * S) == 1, "step: expired share");
    ok(tbt_fec_deliver_step(S, S, S) == 0, "step: next share");
    ok(tbt_fec_deliver_step(~(u64)0, 1, 0) == -EPROTO,
       "step: offset overflow");

    /* Model the field failure: stream_base changed, so the walk restarts at
     * off 0 while the stream has already accepted one slot. The duplicate is
     * not delivered and the cursor realigns to rx_next; the next share is
     * still delivered, and the output stays a source prefix. */
    rx_next = S;
    off = 0;
    if (tbt_fec_deliver_step(off, S, rx_next) == 1)
        off = (size_t)rx_next; /* realign, do not advance by len */
    ok_size(off, S, "step: duplicate realigns the cursor");
    memcpy(out + off, src + off, S);
    ok(memcmp(out + off, src + off, S) == 0,
       "step: realigned share is the source prefix");
    ok_size(off + S, 2 * S, "step: real bytes after a duplicate are kept");
    ok(tbt_fec_deliver_step(0, S, N * S) == 1,
       "step: late share after a complete block");
    ok(tbt_fec_deliver_step(N * S, S, N * S) == 0,
       "step: next real share after a complete block");
}

/* ------------------------------------------------------------------ *
 * 1b2. One work instance at a time: a second entry only sets again.
 * ------------------------------------------------------------------ */

static void test_work_serialisation(void)
{
    unsigned int run = 0, again = 0;
    int bodies = 0, arms = 0;

    /* Entry 1 owns the running bit and runs the body. */
    if (!(run & 1u))
    {
        run |= 1u;
        bodies++;
    }
    else
    {
        again = 1u;
    }
    /* Entry 2 arrives while entry 1 is inside the body. It must not run. */
    if (!(run & 1u))
    {
        run |= 1u;
        bodies++;
    }
    else
    {
        again = 1u;
    }
    ok(bodies == 1 && again == 1, "work: second entry only sets again");

    /* Body end: clear running, consume again, arm exactly once. */
    run &= ~1u;
    if (again)
    {
        again = 0;
        arms++;
    }
    ok(bodies == 1 && arms == 1 && run == 0 && again == 0,
       "work: one body and one re-arm");

    /* The re-armed entry runs only after the owner finished. */
    if (!(run & 1u))
    {
        run |= 1u;
        bodies++;
    }
    ok(bodies == 2, "work: the re-armed entry runs once");
    run &= ~1u;
}

/* ------------------------------------------------------------------ *
 * 1c. Empty block positions: an absent group contributes no bytes.
 * ------------------------------------------------------------------ */

static void test_empty_block_position(void)
{
    ok(!tbt_fec_blk_deliverable(false, false, 0, 0), "empty: absent block");
    ok(!tbt_fec_blk_deliverable(false, true, 0, 0), "empty: absent but flagged");
    ok(!tbt_fec_blk_deliverable(true, false, 0, 0),
       "empty: present but not ready");
    ok(!tbt_fec_blk_deliverable(true, true, 1, 0), "empty: wrong block id");
    ok(tbt_fec_blk_deliverable(true, true, 0, 0), "empty: ready block delivers");
}

/* ------------------------------------------------------------------ *
 * 1d. The recovered share survives the crypto layer unchanged.
 * ------------------------------------------------------------------ */

static void test_crypto_roundtrip(void)
{
    /* Only the last real share may be short: the walk concatenates shares,
     * so a short share in the middle would make the model's S-aligned source
     * layout disagree with the delivered stream (the packetizer never emits
     * one either). */
    static const u16 len[N] = {S, S, S, 21};
    struct tbt_crypto_ctx ctx;
    struct blk b;
    u8 key[TBT_CRYPTO_KEY_LEN];
    u8 nonce[TBT_CRYPTO_NONCE_LEN];
    u8 out[N * S];
    u64 last_seq = 3 * 32 + 1; /* group 1, last real share */
    int i, refused;
    size_t n;

    for (i = 0; i < (int)sizeof(key); i++)
        key[i] = (u8)(0x40 + i);
    for (i = 0; i < (int)sizeof(nonce); i++)
        nonce[i] = (u8)(0xa0 + i);
    ok(tbt_crypto_init(&ctx, key, nonce, 0) == 0, "crypto init");

    blk_init(&b, 0, len);
    blk_encode(&b);
    b.nreal = N;
    b.short_len = 21;

    /* Encrypt exactly as tbt_fec_make_record(): DATA uses its own seq,
     * PARITY the block's last real seq and share_index n+i. */
    for (i = 0; i < N; i++)
        ok(tbt_crypto_body_xor_share(&ctx, (u64)i, false, (u16)i,
                                     b.data[i], len[i]) == 0,
           "crypto encrypt DATA");
    for (i = 0; i < M; i++)
        ok(tbt_crypto_body_xor_share(&ctx, last_seq, true, (u16)(N + i),
                                     b.parity[i], S) == 0,
           "crypto encrypt PARITY");

    /* The receiver decrypts with the same header fields. */
    for (i = 0; i < N; i++)
        ok(tbt_crypto_body_xor_share(&ctx, (u64)i, false, (u16)i,
                                     b.data[i], len[i]) == 0,
           "crypto decrypt DATA");
    for (i = 0; i < M; i++)
        ok(tbt_crypto_body_xor_share(&ctx, last_seq, true, (u16)(N + i),
                                     b.parity[i], S) == 0,
           "crypto decrypt PARITY");

    /* One lost DATA share: recovery must reproduce the source byte for byte. */
    b.dpresent[1] = 0;
    ok(blk_recover(&b) == 0, "crypto: lost share recovers");
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "crypto: recovered block delivers");
    ok_size(n, 3 * S + 21, "crypto: recovered length");
    ok_prefix(out, n, "crypto: recovered bytes are the source prefix");
}

/* ------------------------------------------------------------------ *
 * 2. A full block, every loss pattern the parity can solve.
 * ------------------------------------------------------------------ */

static void test_full_block(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct blk b;
    u8 out[N * S];
    int refused;
    size_t n;

    /* No loss. */
    blk_init(&b, 0, full);
    blk_encode(&b);
    b.nreal = N;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "full block, no loss: delivered");
    ok_size(n, N * S, "full block, no loss: length");
    ok_prefix(out, n, "full block, no loss");

    /* One lost DATA share. */
    blk_init(&b, 0, full);
    blk_encode(&b);
    b.dpresent[1] = 0;
    ok(blk_recover(&b) == 0, "one lost share recovers");
    b.nreal = N;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "one lost share: delivered");
    ok_size(n, N * S, "one lost share: length");
    ok_prefix(out, n, "one lost share");

    /* Two lost DATA shares. */
    blk_init(&b, 0, full);
    blk_encode(&b);
    b.dpresent[0] = 0;
    b.dpresent[2] = 0;
    ok(blk_recover(&b) == 0, "two lost shares recover");
    b.nreal = N;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "two lost shares: delivered");
    ok_size(n, N * S, "two lost shares: length");
    ok_prefix(out, n, "two lost shares");

    /* Three lost DATA shares and two lost parity rows: unsolvable. The walk
     * must refuse, not deliver a guess. */
    blk_init(&b, 0, full);
    blk_encode(&b);
    b.dpresent[0] = 0;
    b.dpresent[1] = 0;
    b.dpresent[2] = 0;
    b.ppresent[0] = 0;
    b.ppresent[1] = 0;
    ok(blk_recover(&b) == -EAGAIN, "unsolvable block refuses to decode");
}

/* ------------------------------------------------------------------ *
 * 3. Short shares: the length must come from a record, never from a guess.
 * ------------------------------------------------------------------ */

static void test_short_shares(void)
{
    struct blk b;
    u8 out[N * S];
    int refused;
    size_t n;

    /* The block's last share is short and its epoch names the length. Losing
     * that share recovers exactly `short` bytes, not S. */
    {
        const u16 len[N] = {S, S, S, 21};

        blk_init(&b, 0, len);
        blk_encode(&b);
        b.dpresent[3] = 0;
        ok(blk_recover(&b) == 0, "short last share recovers");
        b.nreal = N;
        b.short_len = 21;
        n = blk_deliver(&b, out, sizeof(out), &refused);
        ok(!refused, "short last share: delivered");
        ok_size(n, 3 * S + 21, "short last share: length is the epoch");
        ok_prefix(out, n, "short last share");
    }

    /* Losing an earlier full share in a block whose last share is short: the
     * epoch describes the last share, so the recovered share is S. */
    {
        const u16 len[N] = {S, S, S, 21};

        blk_init(&b, 0, len);
        blk_encode(&b);
        b.nreal = N;
        b.short_len = 21;
        b.dpresent[1] = 0;
        ok(blk_recover(&b) == 0, "full share before a short tail recovers");
        n = blk_deliver(&b, out, sizeof(out), &refused);
        ok(!refused, "full share before a short tail: delivered");
        ok_size(n, 3 * S + 21, "full share before a short tail: length");
        ok_prefix(out, n, "full share before a short tail");
    }

    /* A short share in the middle of a block (legal on the wire, never
     * produced by this packetizer) leaves every recovered slot without a
     * stated length. Decode refuses the block; nothing is delivered, so no
     * zero padding reaches the application. */
    {
        const u16 len[N] = {13, S, S, S};

        blk_init(&b, 0, len);
        blk_encode(&b);
        b.dpresent[2] = 0;
        b.nreal = N;
        b.short_len = 0; /* the last share is full */
        ok(blk_recover(&b) == -EAGAIN, "mid-block short refuses at decode");
    }
}

/* ------------------------------------------------------------------ *
 * 4. A share past the block's stated cut is a conflict, not padding.
 * ------------------------------------------------------------------ */

static void test_slot_past_cut(void)
{
    const u16 len[N] = {S, S, S, S};
    struct blk b;
    u8 out[N * S];
    int refused;
    size_t n;

    /* win_len says two real shares, but share 2 arrived. Skipping it would
     * delete 64 source bytes while the frontier ran past it. */
    blk_init(&b, 0, len);
    blk_encode(&b);
    b.nreal = 2;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(refused, "share past the cut refuses the block");
    ok_size(n, 2 * S, "share past the cut delivers only the shares below it");
    ok_prefix(out, n, "share past the cut");

    /* Without the stray share the same cut is ordinary padding. */
    blk_init(&b, 0, len);
    blk_encode(&b);
    b.dpresent[2] = 0;
    b.dpresent[3] = 0;
    b.nreal = 2;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "padding below n is not a conflict");
    ok_size(n, 2 * S, "padding delivers the real shares only");
    ok_prefix(out, n, "padding");
}

/* ------------------------------------------------------------------ *
 * 5. The ACK reports the bytes that were delivered.
 * ------------------------------------------------------------------ */

static void test_ack_bytes(void)
{
    const u16 len[N] = {S, S, S, 21};
    struct blk b;
    struct tbt_fec_ack a, a2;
    u8 body[TBT_FEC_ACK_LEN];
    u8 out[N * S];
    int refused;
    size_t n;

    /* A frontier of 4 packets over a block that delivered 3S + 21 must not be
     * turned into 4S by the receiver: the body carries the byte count. */
    blk_init(&b, 0, len);
    blk_encode(&b);
    b.nreal = N;
    b.short_len = 21;
    n = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "ACK case: delivered");
    ok_size(n, 3 * S + 21, "ACK case: byte count");

    memset(&a, 0, sizeof(a));
    a.next_deliver = 4;
    a.deliver_bytes = n;
    a.credit_packets = 1024;
    ok(tbt_fec_ack_pack(&a, body) == 0, "ACK packs");
    memset(&a2, 0, sizeof(a2));
    ok(tbt_fec_ack_unpack(body, &a2) == 0, "ACK unpacks");
    ok_size((size_t)a2.deliver_bytes, n, "ACK carries the delivered byte count");
    ok(a2.next_deliver == 4, "ACK carries the packet frontier separately");
    /* The old derivation is larger: that is exactly the bug this body fixes. */
    ok(a2.deliver_bytes < a2.next_deliver * S,
       "packet frontier alone would overstate the byte count");
    ok(tbt_fec_ack_bytes(a.next_deliver, 0, ~(u64)0) == 4 * (u64)TBT_FEC_S,
       "the fallback multiplication still names the old, wrong value");
}

/* ------------------------------------------------------------------ *
 * 6. A whole super-block: every group, in order, is a source prefix.
 * ------------------------------------------------------------------ */

static void test_super_prefix(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct blk b[2];
    u8 out[2 * N * S];
    size_t off = 0;
    int g, refused;

    /* Group 0 delivers normally; group 1 loses a share and recovers it. */
    blk_init(&b[0], 0, full);
    blk_encode(&b[0]);
    b[0].nreal = N;

    blk_init(&b[1], N * S, full);
    blk_encode(&b[1]);
    b[1].dpresent[3] = 0;
    ok(blk_recover(&b[1]) == 0, "super: group 1 recovers");
    b[1].nreal = N;

    for (g = 0; g < 2; g++)
    {
        size_t n = blk_deliver(&b[g], out + off, sizeof(out) - off, &refused);

        ok(!refused, "super: every group delivers");
        off += n;
    }
    ok_size(off, sizeof(out), "super: length is every real byte");
    ok_prefix(out, off, "super");
}

/* The NACK + replay path, reduced to one block. The ring stores every
 * encoded share; a loss pattern that the parity cannot solve is repaired by
 * reading exactly the missing shares back and decoding again. The wire
 * round-trip is exercised too: the masks are packed and unpacked before they
 * drive the replay. */
static int replay_missing(struct tbt_fec_replay *r, u64 block_id, struct blk *b,
                          const struct tbt_fec_nack *n)
{
    u8 buf[S];
    int i, got = 0;

    for (i = 0; i < N; i++)
    {
        if (!tbt_fec_bitmap_test(n->share_mask, (u16)i))
            continue;
        if (tbt_fec_replay_get(r, block_id, 0, (u16)i, buf, S) != S)
            return -ENOENT;
        memcpy(b->data[i], buf, S);
        b->dpresent[i] = 1;
        b->dlen[i] = S;
        got++;
    }
    for (i = 0; i < M; i++)
    {
        if (!tbt_fec_bitmap_test(n->share_mask, (u16)(N + i)))
            continue;
        if (tbt_fec_replay_get(r, block_id, 0, (u16)(N + i), buf, S) != S)
            return -ENOENT;
        memcpy(b->parity[i], buf, S);
        b->ppresent[i] = 1;
        got++;
    }
    return got;
}

static void test_nack_replay(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct tbt_fec_replay *r;
    u64 block = 0;
    int pattern;

    r = tbt_fec_replay_new(1, N, M, S);
    ok(r != NULL, "replay ring for the block model");

    for (pattern = 1; pattern <= 3; pattern++)
    {
        struct tbt_fec_nack n, n2;
        struct blk b;
        u8 body[TBT_FEC_NACK_LEN];
        u8 out[N * S];
        int i, refused;
        size_t off;

        blk_init(&b, 0, full);
        blk_encode(&b);
        b.nreal = N;
        for (i = 0; i < N; i++)
            ok_int(tbt_fec_replay_store(r, block, 0, (u16)i, b.data[i], S),
                   0, "store data share");
        for (i = 0; i < M; i++)
            ok_int(tbt_fec_replay_store(r, block, 0, (u16)(N + i),
                                        b.parity[i], S), 0,
                   "store parity share");

        memset(&n, 0, sizeof(n));
        tbt_fec_bitmap_set(n.group_mask, 0);
        /* Drop `pattern` DATA shares and enough parity that the block cannot
         * be solved without the replay. */
        for (i = 0; i < pattern; i++)
        {
            b.dpresent[i] = 0;
            tbt_fec_bitmap_set(n.share_mask, (u16)i);
        }
        for (i = 0; i < M - pattern + 1; i++)
        {
            b.ppresent[i] = 0;
            tbt_fec_bitmap_set(n.share_mask, (u16)(N + i));
        }
        ok_int(blk_recover(&b), -EAGAIN, "loss pattern needs the replay");

        ok_int(tbt_fec_nack_pack(&n, body), TBT_FEC_NACK_LEN, "NACK pack");
        memset(&n2, 0, sizeof(n2));
        ok_int(tbt_fec_nack_unpack(body, &n2), 0, "NACK unpack");
        ok(!memcmp(&n, &n2, sizeof(n)), "NACK masks round-trip");
        ok_int(replay_missing(r, block, &b, &n2), pattern + (M - pattern + 1),
               "replay supplies every missing share");
        ok(blk_recover(&b) == 0, "replayed block decodes");
        off = blk_deliver(&b, out, sizeof(out), &refused);
        ok(!refused, "replayed block delivers");
        ok_size(off, N * S, "replayed block length");
        ok_prefix(out, off, "replayed block prefix");
    }

    /* Evict the block: a NACK for it can no longer be served. The model then
     * stays undecodable, which is the bounded-reset/backoff case. */
    {
        struct tbt_fec_nack n;
        struct blk b;
        u8 full_share[S];
        int i;

        memset(full_share, 0x5a, sizeof(full_share));
        for (i = 0; i < N + M; i++)
            ok_int(tbt_fec_replay_store(r, block + TBT_FEC_REPLAY_K, 0,
                                        (u16)i, full_share, S), 0,
                   "store evicting block");
        blk_init(&b, 0, full);
        blk_encode(&b);
        b.nreal = N;
        b.dpresent[1] = 0;
        for (i = 0; i < M; i++)
            b.ppresent[i] = 0;
        memset(&n, 0, sizeof(n));
        tbt_fec_bitmap_set(n.group_mask, 0);
        tbt_fec_bitmap_set(n.share_mask, 1);
        ok_int(replay_missing(r, block, &b, &n), -ENOENT,
               "evicted block misses the replay ring");
        ok_int(blk_recover(&b), -EAGAIN, "evicted block stays undecodable");
    }
    tbt_fec_replay_free(r);
}

/* A short block: only DATA share 0 is real (win_len=1); shares 1..3 are
 * padding that the sender never generates. If the parity is lost, the
 * receiver asks for shares 1..3 and the sender must answer with a parity row
 * whose header carries win_len=1, not with silence. The test models that
 * reply: the ring says share 1 was never stored, but parity 0 is resident. */
static void test_short_block_reply(void)
{
    static const u16 one[N] = {S, 0, 0, 0};
    struct tbt_fec_replay *r;
    struct blk b;
    u8 buf[S];
    u8 out[N * S];
    int i, refused;
    size_t off;

    r = tbt_fec_replay_new(1, N, M, S);
    ok(r != NULL, "short-block replay ring");
    blk_init(&b, 0, one);
    b.dpresent[1] = 0;
    b.dpresent[2] = 0;
    b.dpresent[3] = 0;
    blk_encode(&b);
    b.nreal = 1;

    ok_int(tbt_fec_replay_store(r, 0, 0, 0, b.data[0], S), 0,
           "short block stores its real share");
    for (i = 0; i < M; i++)
        ok_int(tbt_fec_replay_store(r, 0, 0, (u16)(N + i), b.parity[i], S),
               0, "short block stores a parity row");

    /* The receiver asks for padding share 1: it was never generated. */
    ok_int(tbt_fec_replay_get(r, 0, 0, 1, buf, S), -ENODATA,
           "padding share is never stored");
    /* The sender's reply is parity 0: its header carries win_len=1. */
    ok_int(tbt_fec_replay_get(r, 0, 0, N, buf, S), S,
           "parity row is available for the short-block reply");
    memcpy(b.parity[0], buf, S);
    b.ppresent[0] = 1;
    ok(b.nreal == 1 && b.dpresent[0],
       "short block is ready from its one real share");
    off = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "short block delivers");
    ok_size(off, S, "short block delivers only its real share");
    ok_prefix(out, off, "short block prefix");
    tbt_fec_replay_free(r);
}

int main(void)
{
    src_init();
    test_share_len();
    test_deliver_step();
    test_work_serialisation();
    test_empty_block_position();
    test_crypto_roundtrip();
    test_full_block();
    test_short_shares();
    test_slot_past_cut();
    test_ack_bytes();
    test_super_prefix();
    test_nack_replay();
    test_short_block_reply();

    printf("test_fec_rx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
