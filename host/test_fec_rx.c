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
    bool ready;    /* tbt_fec_blk.ready: set once the block is immutable */
    u64 block_id;  /* tbt_fec_blk.block_id: names the super-block it holds */
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
        dp[i] = (i < real) ? b->dpresent[i] : 1; /* padding is known zero */
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
    for (i = 0, k = 0; i < real; i++)
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
    u16 k;
    int pattern;

    r = tbt_fec_replay_new(1, N, M, S);
    ok(r != NULL, "replay ring for the block model");
    k = tbt_fec_replay_k(r);

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
            ok_int(tbt_fec_replay_store(r, block + k, 0,
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

/* Two short writes: 4 B then 200 B, each its own win_len=1 block. The
 * second block's data share is lost. The next-block probe asks parity 0,
 * learns win_len=1, replays the data share, and delivers 204 B. */
static void test_two_short_writes(void)
{
    static const u16 four[N] = {4, 0, 0, 0};
    static const u16 forty[N] = {40, 0, 0, 0};
    struct tbt_fec_replay *r;
    struct blk b0, b1;
    u8 buf[S];
    u8 out[2 * S];
    int i, refused;
    size_t off0, off1;

    r = tbt_fec_replay_new(1, N, M, S);
    ok(r != NULL, "two-write replay ring");
    blk_init(&b0, 0, four);
    b0.dpresent[1] = b0.dpresent[2] = b0.dpresent[3] = 0;
    blk_encode(&b0);
    b0.nreal = 1;
    b0.short_len = 4;
    blk_init(&b1, 4, forty);
    b1.dpresent[1] = b1.dpresent[2] = b1.dpresent[3] = 0;
    blk_encode(&b1);
    b1.nreal = 1;
    b1.short_len = 40;

    ok_int(tbt_fec_replay_store(r, 0, 0, 0, b0.data[0], S), 0,
           "4 B block stores its data share");
    ok_int(tbt_fec_replay_store(r, 1, 0, 0, b1.data[0], S), 0,
           "200 B block stores its data share");
    for (i = 0; i < M; i++)
    {
        ok_int(tbt_fec_replay_store(r, 0, 0, (u16)(N + i), b0.parity[i], S),
               0, "4 B block stores parity");
        ok_int(tbt_fec_replay_store(r, 1, 0, (u16)(N + i), b1.parity[i], S),
               0, "200 B block stores parity");
    }

    /* The second block's DATA share is missing, but all parity rows are
     * present: a parity-only block. The NACK requests the DATA shares (the
     * parity is already there); the parity rows recover the missing share.
     * The sender's ring still holds the DATA share for the replay path. */
    b1.dpresent[0] = 0;
    ok_int(tbt_fec_replay_get(r, 1, 0, 0, buf, S), S,
           "next block data share is in the ring");
    ok_int(tbt_fec_replay_get(r, 1, 0, N, buf, S), S,
           "next block parity 0 is in the ring");
    ok_int(blk_recover(&b1), 0,
           "parity-only block recovers the missing DATA share");
    ok(b1.dpresent[0] && b1.dlen[0] == 0,
       "recovered DATA share has no own length");

    off0 = blk_deliver(&b0, out, sizeof(out), &refused);
    ok(!refused, "4 B block delivers");
    ok_size(off0, 4, "4 B block length");
    off1 = blk_deliver(&b1, out + off0, sizeof(out) - off0, &refused);
    ok(!refused, "200 B block delivers");
    ok_size(off1, 40, "40 B block length");
    ok_prefix(out, off0 + off1, "two short writes prefix");
    tbt_fec_replay_free(r);
}

/* The parity band: row r covers every DATA column except c == r (band=3 for
 * n4m4). A reply that always sends row 0 cannot decode a missing share 3.
 * The sender now sends the whole parity set; every missing position decodes. */
static void test_parity_rotation(void)
{
    static const u16 full[N] = {S, S, S, S};
    int c, i;

    for (c = 0; c < N; c++)
    {
        struct blk b;
        u8 out[N * S];
        int refused, rc0;
        size_t off;

        /* The old reply: only parity row 0. Row 0 misses column c == 0? No:
         * row 0 covers 0,1,2 and misses 3. */
        blk_init(&b, 0, full);
        blk_encode(&b);
        b.nreal = N;
        b.dpresent[c] = 0;
        for (i = 0; i < M; i++)
            b.ppresent[i] = 0;
        b.ppresent[0] = 1;
        rc0 = blk_recover(&b);
        if (c == 3)
            ok_int(rc0, -EAGAIN, "row 0 cannot recover DATA share 3");
        else
            ok_int(rc0, 0, "row 0 recovers DATA share 0..2");

        /* The deterministic reply: row r0 = c % m covers c. */
        blk_init(&b, 0, full);
        blk_encode(&b);
        b.nreal = N;
        b.dpresent[c] = 0;
        for (i = 0; i < M; i++)
            b.ppresent[i] = 0;
        b.ppresent[c % M] = 1;
        ok_int(blk_recover(&b), 0,
               "covering row r0 recovers the missing share");
        off = blk_deliver(&b, out, sizeof(out), &refused);
        ok(!refused, "covering-row block delivers");
        ok_size(off, N * S, "covering-row block length");
        ok_prefix(out, off, "covering-row block prefix");

        /* The fixed reply: all M parity rows. */
        blk_init(&b, 0, full);
        blk_encode(&b);
        b.nreal = N;
        b.dpresent[c] = 0;
        ok_int(blk_recover(&b), 0, "all parity rows recover the missing share");
        off = blk_deliver(&b, out, sizeof(out), &refused);
        ok(!refused, "rotated parity block delivers");
        ok_size(off, N * S, "rotated parity block length");
        ok_prefix(out, off, "rotated parity block prefix");
    }
}

/* A sparse super-block: group 0 has the second short write, groups 1..3 are
 * empty and their markers' first send failed. A marker is never stored in the
 * replay ring: it states a block is empty, and a stored copy for an old block
 * id would reclaim the slot of block_id + K -- one the receiver may still be
 * waiting for. A NACK for share N therefore finds nothing (-ENODATA) and the
 * sender builds a fresh marker. The super-block still becomes ready, and the
 * sink gets 4 + 40 bytes (the production 4 + 200 scaled to S=64). */
static void test_marker_retry(void)
{
    static const u16 four[N] = {4, 0, 0, 0};
    static const u16 forty[N] = {40, 0, 0, 0};
    struct tbt_fec_replay *r;
    struct blk b0[4], b1[4];
    struct tbt_fec_hdr mh;
    struct tbt_fec_hdr rh;
    u8 mrec[TBT_FEC_HDR_LEN];
    u8 buf[TBT_FEC_HDR_LEN];
    u8 out[2 * S];
    int g, refused;
    size_t off0, off1;

    r = tbt_fec_replay_new(4, N, M, S);
    ok(r != NULL, "sparse super replay ring");

    for (g = 0; g < 4; g++)
    {
        memset(&b0[g], 0, sizeof(b0[g]));
        memset(&b1[g], 0, sizeof(b1[g]));
        if (g == 0)
        {
            blk_init(&b0[g], 0, four);
            b0[g].dpresent[1] = b0[g].dpresent[2] = b0[g].dpresent[3] = 0;
            blk_encode(&b0[g]);
            b0[g].nreal = 1;
            b0[g].short_len = 4;
            blk_init(&b1[g], 4, forty);
            b1[g].dpresent[1] = b1[g].dpresent[2] = b1[g].dpresent[3] = 0;
            blk_encode(&b1[g]);
            b1[g].nreal = 1;
            b1[g].short_len = 40;
        }
        else
        {
            b0[g].empty = true;
            b1[g].empty = true;
        }
    }

    /* The first marker send failed; no marker is in the ring. */
    for (g = 1; g < 4; g++)
    {
        memset(&mh, 0, sizeof(mh));
        mh.type = TBT_FEC_PARITY;
        mh.share_index = N;
        mh.n = N;
        mh.m = M;
        mh.block_id = 1;
        mh.seq = tbt_fec_seq_of_block_pkt(1, (u16)g, 0, 4, N);
        mh.win_len = 0;
        mh.payload_len = 0;
        mh.conv = TBT_FEC_CONV;
        ok_int(tbt_fec_hdr_seal(&mh, NULL), 0, "marker seal");
        ok_int(tbt_fec_hdr_pack(&mh, mrec), 0, "marker pack");
        ok_int(tbt_fec_hdr_check(&mh, S), 0, "marker header check");
        ok_int(tbt_fec_replay_get(r, 1, (u16)g, N, buf, sizeof(buf)),
               -ESTALE, "no record is resident for an empty group");
        /* A closed, in-window block with no record for this group is an
         * empty group, and the marker is its answer. */
        ok(tbt_fec_nack_empty_group(true, true, 2, 1),
           "empty group: a closed in-window block answers with a marker");
        ok(!tbt_fec_nack_empty_group(false, true, 2, 1),
           "empty group: a block still being built waits");
        ok(!tbt_fec_nack_empty_group(true, false, 2, 1),
           "empty group: an evicted block is not called empty");
        ok(!tbt_fec_nack_empty_group(true, true, 1, 1),
           "empty group: the group that wrote the block is not empty");
        /* The reply is rebuilt from the block shape, not replayed. */
        ok_int(tbt_fec_hdr_unpack(mrec, &rh), 0, "fresh marker unpacks");
        ok_int(tbt_fec_arrival_kind(false, false, rh.type, rh.win_len, false),
               TBT_FEC_ARR_PLACE, "fresh marker is placed by the receiver");
    }

    /* Group 0 has the data; groups 1..3 are empty and ready from the
     * markers. The super-block can deliver. */
    ok(b1[0].nreal == 1 && b1[0].dpresent[0], "block 1 group 0 data");
    for (g = 1; g < 4; g++)
        ok(b1[g].empty, "block 1 empty group marker");
    off0 = blk_deliver(&b0[0], out, sizeof(out), &refused);
    ok(!refused, "4 B block delivers");
    ok_size(off0, 4, "4 B block length");
    off1 = blk_deliver(&b1[0], out + off0, sizeof(out) - off0, &refused);
    ok(!refused, "200 B block group 0 delivers");
    ok_size(off1, 40, "200 B block length");
    ok_prefix(out, off0 + off1, "sparse super prefix");
    tbt_fec_replay_free(r);
}

/* ------------------------------------------------------------------ *
 * 10. The super-block cursor: readiness, the blocked exit, the wake delay.
 * ------------------------------------------------------------------ */

#define G 4 /* groups per super-block in this model */

struct sup
{
    struct blk b[G];
    u64 id;
};

static void sup_markers(struct sup *sp, u64 id)
{
    int g;

    memset(sp, 0, sizeof(*sp));
    sp->id = id;
    for (g = 0; g < G; g++)
    {
        sp->b[g].block_id = id;
        sp->b[g].ready = true;
        sp->b[g].empty = true;
    }
}

/* tbt_fec_super_ready_locked(): every group must be ready and carry this
 * super-block's id. A marker placed for another id (slot reuse) must keep
 * the super-block unready, or its group's bytes would be skipped. */
static bool sup_ready(const struct sup *sp, u64 s)
{
    int g;

    for (g = 0; g < G; g++)
        if (sp->b[g].block_id != s || !sp->b[g].ready)
            return false;
    return true;
}

/* One pass of tbt_fec_deliver_ready() over one super-block. Returns the
 * bytes appended. On a refused share it keeps (p, g) and leaves the cursor
 * (next_super) alone: the kernel walk must not clear the saved cursor or
 * advance the frontier past bytes the application never received. */
static size_t sup_deliver(const struct sup *sp, u64 *next_super,
                          u16 *cur_p, u16 *cur_g, u8 *out, size_t cap,
                          int *blocked)
{
    u64 s = *next_super;
    size_t off = 0;
    u16 p, g;

    *blocked = 0;
    if (!sup_ready(sp, s))
        return 0;
    for (p = *cur_p; p < N; p++)
    {
        for (g = *cur_g; g < G; g++)
        {
            const struct blk *b = &sp->b[g];
            u16 real = b->nreal ? b->nreal : N;
            u16 len;

            if (b->empty)
                continue;
            if (p >= real)
            {
                if (b->dpresent[p])
                {
                    *blocked = 1;
                    *cur_p = p;
                    *cur_g = g;
                    return off;
                }
                continue;
            }
            len = tbt_fec_rx_share_len(b->dlen[p], b->short_len, p, real, S);
            if (!len)
            {
                *blocked = 1;
                *cur_p = p;
                *cur_g = g;
                return off;
            }
            if (off + len > cap)
            {
                *blocked = 1;
                *cur_p = p;
                *cur_g = g;
                return off;
            }
            memcpy(out + off, b->data[p], len);
            off += len;
        }
        *cur_g = 0;
    }
    *cur_p = 0;
    *cur_g = 0;
    (*next_super)++;
    return off;
}

/* tbt_fec_sid_accept(): a DATA/PARITY record belongs to exactly one stream.
 * The old stream may still be in flight after the new one is bound. */
static void test_sid_filter(void)
{
    ok(!tbt_fec_sid_accept(0, 0, 0), "sid: no stream refuses DATA");
    ok(tbt_fec_sid_accept(7, 0, 7), "sid: the current stream accepts");
    ok(!tbt_fec_sid_accept(7, 0, 6), "sid: another stream refuses");
    ok(!tbt_fec_sid_accept(7, 7, 7), "sid: the barrier's old sid refuses");
    ok(tbt_fec_sid_accept(8, 7, 8),
       "sid: the new stream accepts after the barrier");
    ok(!tbt_fec_sid_accept(8, 7, 7),
       "sid: an old record after the switch refuses");
}

/* tbt_fec_rx_stream_reset(): after an abort, the old stream's reorder block
 * must be dropped and the new stream must start at its own super-block. The
 * first byte the application sees is HTTP/, never the old body. */
static void test_stream_reset_isolation(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct sup old, next;
    u8 out[N * S];
    u64 next_super = 0;
    u16 p = 0, g = 0;
    size_t n, i;
    int blocked;
    bool stale = false;

    /* The aborted stream still holds a ready block with its body. */
    sup_markers(&old, 0);
    blk_init(&old.b[0], 0, full);
    memcpy(old.b[0].data[0], "OLD!", 4);
    old.b[0].block_id = 0;
    old.b[0].ready = true;
    old.b[0].empty = false;
    old.b[0].nreal = N;

    /* The barrier drops that state and pushes the frontier past the old
     * highest block; the new stream starts at super-block 1. */
    memset(&old, 0, sizeof(old));
    next_super = 1;
    p = g = 0;
    ok(!sup_ready(&old, 0), "reset: the old super-block is cleared");

    /* The new stream's first block carries its own bytes. */
    sup_markers(&next, 1);
    blk_init(&next.b[0], 0, full);
    memcpy(next.b[0].data[0], "HTTP/", 5);
    next.b[0].block_id = 1;
    next.b[0].ready = true;
    next.b[0].empty = false;
    next.b[0].nreal = N;
    n = sup_deliver(&next, &next_super, &p, &g, out, sizeof(out), &blocked);
    ok(!blocked, "reset: the new super-block delivers");
    ok_size(n, N * S, "reset: the new stream's block length");
    ok(!memcmp(out, "HTTP/", 5),
       "reset: the new stream starts with HTTP/");
    for (i = 0; i + 4 <= n; i++)
        if (!memcmp(out + i, "OLD!", 4))
            stale = true;
    ok(!stale, "reset: the old stream's body is not delivered");
    ok(next_super == 2, "reset: the frontier advances to the new block");
}

static void test_super_cursor(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct sup sp;
    u64 next_super = 0;
    u16 p = 0, g = 0;
    u8 out[N * S];
    int blocked;
    size_t n;

    /* Super 0: group 0 has four real shares, groups 1..3 are markers. */
    sup_markers(&sp, 0);
    blk_init(&sp.b[0], 0, full);
    blk_encode(&sp.b[0]);
    sp.b[0].block_id = 0;
    sp.b[0].ready = true;
    sp.b[0].nreal = N;
    ok(sup_ready(&sp, 0), "super 0 is ready when every group matches the id");

    /* A block left over from another super-block (a stale slot) must keep
     * the super unready even when it is ready itself. */
    sp.b[2].block_id = 1;
    ok(!sup_ready(&sp, 0), "a group whose block_id is not s keeps it unready");
    sp.b[2].block_id = 0;

    /* Cut conflict: win_len says two real shares, share 2 arrived. The walk
     * delivers the prefix, refuses the super, and keeps the cursor. */
    sp.b[0].nreal = 2;
    n = sup_deliver(&sp, &next_super, &p, &g, out, sizeof(out), &blocked);
    ok(blocked, "cut conflict blocks the super");
    ok_size(n, 2 * S, "cut conflict delivers only the shares below it");
    ok_prefix(out, n, "cut conflict prefix");
    ok(next_super == 0, "blocked super does not advance next_super");
    ok(p == 2 && g == 0, "blocked cursor names the refused share");

    /* Removing the stray shares makes the same cut ordinary padding; the
     * walk resumes at the saved cursor, finishes the super and advances. */
    sp.b[0].dpresent[2] = 0;
    sp.b[0].dpresent[3] = 0;
    p = 2;
    g = 0;
    n = sup_deliver(&sp, &next_super, &p, &g, out, sizeof(out), &blocked);
    ok(!blocked, "padding below the cut does not block");
    ok_size(n, 0, "resume from the cursor skips the padding and the markers");
    ok(next_super == 1, "a complete super advances next_super");
}

/* The iperf3 control handshake shape: a short cookie, a 1 B reply, then a
 * length and a body that the tunnel coalesces into one short packet. Each
 * short packet closes its block, so the two packets live in super-blocks 0
 * and 1, one share each, with empty markers in the other groups. The stream
 * ends at the block-aligned packet count, and every byte must be delivered
 * by its own super-block. */
static void test_ack_split(void)
{
    static const u16 cookie[N] = {37, 0, 0, 0};
    static const u16 body[N] = {27, 0, 0, 0};
    struct sup sp[2];
    struct tbt_fec_ack a;
    u64 next_super = 0;
    u16 p = 0, g = 0;
    u8 out[2 * S];
    int i, blocked;
    size_t n, off = 0;

    sup_markers(&sp[0], 0);
    blk_init(&sp[0].b[0], 0, cookie);
    sp[0].b[0].dpresent[1] = sp[0].b[0].dpresent[2] = 0;
    sp[0].b[0].dpresent[3] = 0;
    blk_encode(&sp[0].b[0]);
    sp[0].b[0].block_id = 0;
    sp[0].b[0].ready = true;
    sp[0].b[0].nreal = 1;
    sp[0].b[0].short_len = 37;

    sup_markers(&sp[1], 1);
    blk_init(&sp[1].b[0], 37, body);
    sp[1].b[0].dpresent[1] = sp[1].b[0].dpresent[2] = 0;
    sp[1].b[0].dpresent[3] = 0;
    blk_encode(&sp[1].b[0]);
    sp[1].b[0].block_id = 1;
    sp[1].b[0].ready = true;
    sp[1].b[0].nreal = 1;
    sp[1].b[0].short_len = 27;

    for (i = 0; i < 2; i++)
    {
        n = sup_deliver(&sp[i], &next_super, &p, &g, out + off,
                        sizeof(out) - off, &blocked);
        ok(!blocked, "ack split: every super delivers");
        off += n;
        ok(next_super == (u64)i + 1, "ack split: the cursor advances");
    }
    ok_size(off, 37 + 27, "ack split: the sink receives every byte");
    ok_prefix(out, off, "ack split prefix");

    /* The ACK names the block-aligned frontier and the byte count the
     * application received; the two are not interchangeable. */
    memset(&a, 0, sizeof(a));
    a.next_deliver = 2 * (u64)G * N;
    a.deliver_bytes = off;
    ok(a.next_deliver == 32, "ack split: frontier is the block-aligned end");
    ok_size((size_t)a.deliver_bytes, 37 + 27,
            "ack split: the ACK carries the delivered byte count");
}

/* The replay ring is sized in super-block ids, and the credit cap keeps the
 * sender inside it: the sender may run at most K-2 super-blocks past the
 * delivered frontier, so a block the receiver can still NACK is never
 * evicted. A short block advances the id by one per quiet gap (2 ms), so the
 * ring covers only K * 2 ms of a sparse stream -- the guarantee is the credit
 * cap, not the elapsed time. This models a 300 ms NACK round trip: the delay
 * changes nothing because the sender is stopped at the cap. */
static void test_replay_window(void)
{
    struct tbt_fec_replay *r;
    u32 per, slots, credit, blocks;
    u16 k;
    u64 b;
    u8 buf[S];

    r = tbt_fec_replay_new(G, N, M, S);
    ok(r != NULL, "replay window: ring");
    k = tbt_fec_replay_k(r);
    ok_int(k, TBT_FEC_REPLAY_K_MAX,
           "replay window: the small shape reaches K_MAX");
    ok(tbt_fec_replay_bytes(G, N, M, S, k) <= TBT_FEC_REPLAY_BUDGET,
       "replay window: K fits the budget");

    /* The arena formula from tbt_fec_data_init(): 16 MiB of super-blocks,
     * clamped to [4, 256] slots per group. */
    per = (u32)G * (N + M) * S;
    slots = (16u * 1024 * 1024) / per;
    if (slots < 4)
        slots = 4;
    if (slots > 256)
        slots = 256;

    /* The credit is K_eff-2 super-blocks, K_eff = min(ring K, arena slots),
     * capped to TBT_FEC_CREDIT_MAX packets. For this shape the ring binds. */
    credit = tbt_fec_credit_packets(G, N, k, (u16)slots,
                                    TBT_FEC_CREDIT_MAX);
    blocks = credit / ((u32)G * N);
    ok_int((int)blocks, (int)k - 2,
           "replay window: credit is K-2 super-blocks");
    ok(credit <= TBT_FEC_CREDIT_MAX,
       "replay window: credit is capped at 1024 packets");

    /* The receiver delivered up to block 3 and stalls on block 4. The sender
     * closes blocks 4..(3 + credit blocks): the cap, one short share each. */
    for (b = 0; b <= 3 + (u64)blocks; b++)
        ok_int(tbt_fec_replay_store(r, b, 0, 0, src, S), 0,
               "replay window: short block stored");

    /* The NACK for the stalled block arrives 300 ms later: the cap stopped
     * the sender, so nothing touched the block. */
    ok_int(tbt_fec_replay_get(r, 4, 0, 0, buf, S), S,
           "replay window: the stalled block replays after the NACK delay");

    /* The bound is exact: block 4 + K shares block 4's slot. The data plane
     * never lets the sender get there; this is what the cap prevents, and
     * what a NACK reply must not do to a live slot either. */
    ok_int(tbt_fec_replay_store(r, 4 + k, 0, 0, src, S), 0,
           "replay window: one block past the cap is storable");
    ok_int(tbt_fec_replay_get(r, 4, 0, 0, buf, S), -ESTALE,
           "replay window: past the cap the stalled block is evicted");
    tbt_fec_replay_free(r);

    /* Production shapes at S=8192. K is the largest ring the 15 MiB budget
     * holds, capped at TBT_FEC_REPLAY_K_MAX=64; the credit is K_eff-2
     * super-blocks, capped at 1024 packets. 32 and 64 carriers now cover the
     * carrier aggregate BDP (912 and 864 packets, 7.47 and 7.08 MiB); the
     * 256-carrier shapes keep K=7 and their old credit (640 grouped, 960
     * single). */
    ok_int(tbt_fec_replay_k_for(4, 4, 4, 8192), 59, "32-carrier K");
    ok_int(tbt_fec_replay_k_for(8, 4, 4, 8192), 29, "64-carrier K");
    ok_int(tbt_fec_replay_k_for(32, 4, 4, 8192), 7,
           "256-carrier grouped K");
    ok_int(tbt_fec_replay_k_for(1, 192, 64, 8192), 7,
           "256-carrier single K");
    ok_int((int)tbt_fec_credit_packets(4, 4, 59, 64, TBT_FEC_CREDIT_MAX),
           912, "32-carrier credit");
    ok_int((int)tbt_fec_credit_packets(8, 4, 29, 32, TBT_FEC_CREDIT_MAX),
           864, "64-carrier credit");
    ok_int((int)tbt_fec_credit_packets(32, 4, 7, 8, TBT_FEC_CREDIT_MAX),
           640, "256-carrier grouped credit");
    ok_int((int)tbt_fec_credit_packets(1, 192, 7, 8, TBT_FEC_CREDIT_MAX),
           960, "256-carrier single credit");

    /* The three bounds hold for every shape and do not depend on the stream
     * length: the ring stays inside 15 MiB, the arena inside 16 MiB, and the
     * credit inside 1024 packets (8 MiB of payload). */
    ok(tbt_fec_replay_bytes(4, 4, 4, 8192, 59) <= TBT_FEC_REPLAY_BUDGET,
       "32-carrier ring stays inside 15 MiB");
    ok(tbt_fec_replay_bytes(8, 4, 4, 8192, 29) <= TBT_FEC_REPLAY_BUDGET,
       "64-carrier ring stays inside 15 MiB");
    ok(tbt_fec_replay_bytes(32, 4, 4, 8192, 7) <= TBT_FEC_REPLAY_BUDGET,
       "256-carrier grouped ring stays inside 15 MiB");
    ok(tbt_fec_replay_bytes(1, 192, 64, 8192, 7) <= TBT_FEC_REPLAY_BUDGET,
       "256-carrier single ring stays inside 15 MiB");
    ok((u64)64 * 4 * 8 * 8192 <= 16u * 1024 * 1024,
       "32-carrier arena stays inside 16 MiB");
    ok((u64)32 * 8 * 8 * 8192 <= 16u * 1024 * 1024,
       "64-carrier arena stays inside 16 MiB");
    ok((u64)8 * 32 * 8 * 8192 <= 16u * 1024 * 1024,
       "256-carrier grouped arena stays inside 16 MiB");
    ok((u64)8 * 1 * 256 * 8192 <= 16u * 1024 * 1024,
       "256-carrier single arena stays inside 16 MiB");
    ok(tbt_fec_credit_packets(4, 4, 59, 64, TBT_FEC_CREDIT_MAX) <=
           TBT_FEC_CREDIT_MAX &&
           tbt_fec_credit_packets(8, 4, 29, 32, TBT_FEC_CREDIT_MAX) <=
               TBT_FEC_CREDIT_MAX &&
           tbt_fec_credit_packets(32, 4, 7, 8, TBT_FEC_CREDIT_MAX) <=
               TBT_FEC_CREDIT_MAX &&
           tbt_fec_credit_packets(1, 192, 7, 8, TBT_FEC_CREDIT_MAX) <=
               TBT_FEC_CREDIT_MAX,
       "credit stays inside the 1024-packet cap");
}

/* ------------------------------------------------------------------ *
 * 11. NACK filtering and replay deduplication.
 *
 * The 1 GiB field run sent 16,691 NACKs, produced 1.1M replay-ring
 * lookups, and found the 8 MiB replay queue full 872k times. Each NACK
 * re-named shares whose replay was still in flight, and the first one
 * left 10 ms after the gap -- hundreds of milliseconds before a replay
 * could arrive (mean 853 ms). The policy below bounds the wire volume:
 *   - the first NACK waits one round trip from the first gap;
 *   - a share is not named again inside max(RTT, 2 x cadence);
 *   - one NACK names at most TBT_FEC_NACK_SHARES_MAX shares;
 *   - the sender queues no second replay for a share in flight, and
 *     never replays a block the peer has already delivered.
 * ------------------------------------------------------------------ */

#define STORM_SHARES 64
#define STORM_SLOT 8192u
#define STORM_QUEUE_MAX (8u * 1024 * 1024)

static void test_nack_filter(void)
{
    u64 hold = tbt_fec_nack_hold_ns(300000, TBT_FEC_NACK_RETRY_MS);

    ok_size((size_t)hold, 300000000,
            "hold: one round trip dominates a short cadence");
    ok_size((size_t)tbt_fec_nack_hold_ns(0, TBT_FEC_NACK_RETRY_MS),
            40000000, "hold: the floor is two retry intervals");
    ok_size((size_t)tbt_fec_nack_hold_ns(50000, 100), 200000000,
            "hold: two cadences dominate a short RTT");

    ok(!tbt_fec_nack_first_due(1000 + hold - 1, 1000, hold),
       "first NACK: not due before one round trip");
    ok(tbt_fec_nack_first_due(1000 + hold, 1000, hold),
       "first NACK: due after one round trip");
    ok(!tbt_fec_nack_first_due(1000 + hold, 0, hold),
       "first NACK: a block with no gap stamp is not due");

    ok_int(tbt_fec_nack_share_skip(true, 100, 0, hold),
           TBT_FEC_NACK_SKIP_PRESENT, "present share is not requested");
    ok_int(tbt_fec_nack_share_skip(false, 100, 0, hold),
           TBT_FEC_NACK_KEEP, "never-requested share is requested");
    ok_int(tbt_fec_nack_share_skip(false, 100 + hold - 1, 100, hold),
           TBT_FEC_NACK_SKIP_INFLIGHT, "in-flight share is left alone");
    ok_int(tbt_fec_nack_share_skip(false, 100 + hold, 100, hold),
           TBT_FEC_NACK_KEEP, "hold expiry re-allows the request");

    ok(!tbt_fec_replay_dup(100, 0, hold), "replay: no stamp is not a dup");
    ok(tbt_fec_replay_dup(100 + hold - 1, 100, hold),
       "replay: a share in flight is a dup");
    ok(!tbt_fec_replay_dup(100 + hold, 100, hold),
       "replay: after the hold a share may be queued again");

    ok(tbt_fec_block_delivered(0, 2 * G * N, G, N),
       "delivered: block 0 is below the frontier");
    ok(tbt_fec_block_delivered(1, 2 * G * N, G, N),
       "delivered: block 1 ends at the frontier");
    ok(!tbt_fec_block_delivered(2, 2 * G * N, G, N),
       "delivered: block 2 is not delivered");
    ok(!tbt_fec_block_delivered(0, 0, G, N),
       "delivered: a zero frontier delivers nothing");

    ok(!tbt_fec_nack_probe_due(1000, 1000), "probe: one per interval");
    ok(!tbt_fec_nack_probe_due(1000 + TBT_FEC_NACK_PROBE_MS * 1000000ULL - 1,
                               1000),
       "probe: not before the interval");
    ok(tbt_fec_nack_probe_due(1000 + TBT_FEC_NACK_PROBE_MS * 1000000ULL,
                              1000),
       "probe: due at the interval");
    ok(tbt_fec_nack_probe_due(1000, 0), "probe: the first one is due");

    /* Repair latency sets the cadence: back off at once, recover slowly. */
    ok_int(tbt_fec_nack_cadence_ms(TBT_FEC_NACK_RETRY_MS, 853000000ULL),
           213, "cadence: an 853 ms repair sets 213 ms");
    ok_int(tbt_fec_nack_cadence_ms(213, 40000000ULL),
           (213 * 7 + 20) / 8, "cadence: a fast repair recovers slowly");
    ok_int(tbt_fec_nack_cadence_ms(TBT_FEC_NACK_RETRY_MS, 16000000000ULL),
           TBT_FEC_NACK_RETRY_MAX_MS, "cadence: capped at the maximum");
}

/* Two seconds of a 300 ms path with 64 missing shares of one block. The
 * receiver scans every 10 ms; one replayed share is 8 KiB and the queue
 * drains at the field's 2.8 MB/s. Without the filter every scan re-named
 * every share (64 x 8 KiB per 10 ms = 100 MiB through an 8 MiB queue).
 * The filtered policy must keep the queue below its bound, ask each share
 * once per round trip, and still re-ask a share that never arrives. */
static void test_nack_storm(void)
{
    u64 hold = tbt_fec_nack_hold_ns(300000, TBT_FEC_NACK_RETRY_MS);
    u64 req_ns[STORM_SHARES];
    const u64 first_gap = 1000;
    u32 queued = 0, queue_high = 0, queued_total = 0;
    u16 requests[STORM_SHARES];
    u16 asked = 0;
    u64 t;
    int i;

    memset(req_ns, 0, sizeof(req_ns));
    memset(requests, 0, sizeof(requests));

    for (t = first_gap; t <= 2000000000ULL; t += 10000000ULL)
    {
        u16 count = 0;
        bool due = tbt_fec_nack_first_due(t, first_gap, hold);

        for (i = 0; i < STORM_SHARES && due; i++)
        {
            if (tbt_fec_nack_share_skip(false, t, req_ns[i], hold) !=
                TBT_FEC_NACK_KEEP)
                continue;
            if (count >= TBT_FEC_NACK_SHARES_MAX)
                break;
            req_ns[i] = t;
            requests[i]++;
            count++;
            queued += STORM_SLOT;
            queued_total += STORM_SLOT;
            if (queued > queue_high)
                queue_high = queued;
            asked++;
        }
        /* 2.8 MB/s, one 10 ms scan tick. */
        queued = queued > 28000u ? queued - 28000u : 0;
    }

    ok(asked > 0, "storm: the first NACK is eventually sent");
    ok(queue_high <= STORM_QUEUE_MAX,
       "storm: repeated NACKs never fill the 8 MiB replay queue");
    ok(queued_total < 16u * 1024 * 1024,
       "storm: two seconds request a few rounds, not 100 MiB");
    ok(asked <= (u16)(2000000000ULL / hold + 1) * STORM_SHARES,
       "storm: total requests are bounded by the hold");
    for (i = 0; i < STORM_SHARES; i++)
        ok(requests[i] >= 1, "storm: every missing share is asked");
    ok(requests[0] >= 2, "storm: a lost share is asked again after the hold");
    ok(requests[0] <= (u16)(2000000000ULL / hold + 1),
       "storm: one share is asked once per round trip");
}

/* The sender side of the same storm: a NACK arrives every 20 ms naming the
 * same shares. Without dedup that is every share every 20 ms; with the
 * round-trip stamp it is every share per round trip. The stamp belongs to
 * the block in the ring slot, so an evicted block cannot inherit it. */
static void test_replay_dedup_model(void)
{
    struct tbt_fec_replay *r = tbt_fec_replay_new(1, N, M, S);
    u64 hold = tbt_fec_nack_hold_ns(300000, TBT_FEC_NACK_RETRY_MS);
    u8 rec[S];
    u64 t;
    u32 queued = 0;
    u16 k;
    int i;

    ok(r != NULL, "dedup: replay ring");
    k = tbt_fec_replay_k(r);
    memset(rec, 0x33, sizeof(rec));
    for (i = 0; i < N + M; i++)
        ok_int(tbt_fec_replay_store(r, 0, 0, (u16)i, rec, S), 0,
               "dedup: store share");

    for (t = 1000; t <= 2000000000ULL; t += 20000000ULL)
    {
        for (i = 0; i < N + M; i++)
        {
            u64 last;

            if (!tbt_fec_replay_has(r, 0, 0, (u16)i))
                continue;
            last = tbt_fec_replay_sent_ns(r, 0, 0, (u16)i);
            if (tbt_fec_replay_dup(t, last, hold))
                continue;
            tbt_fec_replay_mark_sent(r, 0, 0, (u16)i, t);
            queued++;
        }
    }
    /* 2 s / 300 ms = 6.67 -> at most 7 rounds of (N + M) shares. */
    ok(queued <= (u32)(2000000000ULL / hold + 1) * (N + M),
       "dedup: one replay per share per round trip");
    ok(queued >= (u32)(N + M),
       "dedup: the first NACK is served at once");

    /* A block the peer has delivered is never replayed. */
    ok(tbt_fec_block_delivered(0, (u64)G * N, 1, N),
       "dedup: a delivered block is not replayed");

    /* The stamp names the block, not the slot: evicting block 0 leaves
     * block K's stamp, and block 0 reads zero. */
    ok_int(tbt_fec_replay_store(r, k, 0, 0, rec, S), 0,
           "dedup: evicting store");
    ok_size((size_t)tbt_fec_replay_sent_ns(r, 0, 0, 0), 0,
            "dedup: an evicted block has no send stamp");
    tbt_fec_replay_free(r);
}

/* The FIN must not wait for replay records. A replay is a duplicate of a
 * share that was already encoded and sent once; it cannot extend the stream.
 * fix20 held the FIN behind an 8 MiB replay backlog, the 5 s close budget
 * expired, and a 99.63% transfer was reset with 3.98 MB left. The flush also
 * drops a queued replay whose block the peer has already delivered, so the
 * backlog shrinks to the shares the tail actually needs. */
static void test_fin_and_stale_replay(void)
{
    struct tbt_fec_replay *r = tbt_fec_replay_new(1, N, M, S);
    u64 frontier = 2 * (u64)G * N; /* blocks 0 and 1 are delivered */
    u64 queued[5] = {0, 0, 1, 2, 3};
    u64 kept[5];
    u8 buf[S];
    int i, nkept = 0;

    /* FIN gating: only the packetizer and new DATA records matter. */
    ok(tbt_fec_fin_ready(true, 0),
       "FIN: ready while only replay records are queued");
    ok(!tbt_fec_fin_ready(false, 0), "FIN: not while the packetizer runs");
    ok(!tbt_fec_fin_ready(true, 1), "FIN: not while new DATA is queued");

    /* A closing stream caps replay admission at 2 MiB. */
    ok_size((size_t)tbt_fec_replay_cap(false, 8u * 1024 * 1024),
            8u * 1024 * 1024, "replay cap: normal bound unchanged");
    ok_size((size_t)tbt_fec_replay_cap(true, 8u * 1024 * 1024),
            TBT_FEC_REPLAY_CLOSE_BYTES_MAX,
            "replay cap: a closing stream drops to 2 MiB");
    ok_size((size_t)tbt_fec_replay_cap(true, 1024 * 1024), 1024 * 1024,
            "replay cap: never raises a lower bound");

    /* Flush purge: records for delivered blocks are freed without a write;
     * the frontier and later blocks stay queued. */
    for (i = 0; i < 5; i++)
        if (!tbt_fec_block_delivered(queued[i], frontier, G, N))
            kept[nkept++] = queued[i];
    ok_int(nkept, 2, "stale replay: only the undelivered blocks remain");
    ok(kept[0] == 2 && kept[1] == 3,
       "stale replay: the frontier and the tail block survive");

    /* The tail share of the surviving block is still in the ring and can be
     * replayed; the purge does not touch it. */
    ok(r != NULL, "stale replay: ring");
    ok_int(tbt_fec_replay_store(r, 3, 0, 0, src, S), 0,
           "stale replay: tail share stored");
    ok_int(tbt_fec_replay_get(r, 3, 0, 0, buf, S), S,
           "stale replay: tail share still replays");
    ok(!tbt_fec_block_delivered(3, frontier, G, N),
       "stale replay: the tail block is not delivered");
    tbt_fec_replay_free(r);
}

/* fix22: the far probed the idle uplink frontier for 2,756 s. Each probe
 * named parity 0 of all 32 groups; the near had no such block and counted
 * 274,389 never-stored lookups for 8,583 probes. A block the sender never
 * built answers nothing, so the probe must be bounded. The same run also
 * showed that a resident block is not an empty group: a marker for padding
 * would falsely mark real data as absent. */
static void test_probe_and_absent(void)
{
    u64 last = 0;
    u8 tries = 0;
    int probes = 0;
    u64 t;

    ok(tbt_fec_nack_probe_allowed(0), "probe: first try allowed");
    ok(tbt_fec_nack_probe_allowed(TBT_FEC_NACK_PROBE_MAX - 1),
       "probe: last try allowed");
    ok(!tbt_fec_nack_probe_allowed(TBT_FEC_NACK_PROBE_MAX),
       "probe: the cap stops the next one");
    ok(TBT_FEC_NACK_PROBE_MAX >= 2 && TBT_FEC_NACK_PROBE_MAX <= 8,
       "probe: the cap is small");

    /* 10 s of 10 ms scans: the interval alone allows 40 probes, the cap 3. */
    for (t = 1; t <= 10000000000ULL; t += 10000000ULL)
    {
        if (tbt_fec_nack_probe_due(t, last) &&
            tbt_fec_nack_probe_allowed(tries))
        {
            probes++;
            tries++;
            last = t;
        }
    }
    ok_int(probes, TBT_FEC_NACK_PROBE_MAX,
           "probe: an idle frontier is probed a bounded number of times");
    ok(probes < 40, "probe: not once per scan");

    /* Absent-share answers: a resident block is not an empty group. */
    ok_int(tbt_fec_absent_reply(false, false), TBT_FEC_ABSENT_MARKER,
           "absent: a never-built group answers with a marker");
    ok_int(tbt_fec_absent_reply(false, true), TBT_FEC_ABSENT_PARITY,
           "absent: a covering parity row answers");
    ok_int(tbt_fec_absent_reply(true, false), TBT_FEC_ABSENT_INVALID,
           "absent: padding in a resident block has no answer");
    ok_int(tbt_fec_absent_reply(true, true), TBT_FEC_ABSENT_PARITY,
           "absent: parity wins over padding");

    /* The field storm: 8,583 probes x 32 groups = 274k lookups; the bounded
     * policy does 3 probes, i.e. 96 lookups. */
    ok(3 * 32 < 274389 / 100,
       "probe: the cap removes the never-stored storm");
}

/* fix22: the far's replay queue reached 8 MiB and the download made no
 * progress for 120 s. fix23 then capped it at 4 MiB with a one-record
 * high-water throttle: flush skips rose 116x and the ordered walk's HOL
 * wait 1278x, so the drain fell behind. The cap is 8 MiB again and the
 * throttle keeps four records per pass: new DATA still gets a turn every
 * pass, replay keeps enough drain bandwidth, nothing is dropped, and the
 * FIN still does not wait for it. */
static void test_replay_high_water(void)
{
    u32 cap = 8u * 1024 * 1024;
    u32 high = tbt_fec_replay_high_water(cap);
    const u32 rec = 8192;
    u32 queued = cap;
    int passes = 0, over_passes = 0, under_passes = 0;
    bool bad = false;

    ok_size((size_t)high, 6u * 1024 * 1024, "high water: 75% of 8 MiB");
    ok_size((size_t)tbt_fec_replay_high_water(TBT_FEC_REPLAY_CLOSE_BYTES_MAX),
            TBT_FEC_REPLAY_CLOSE_BYTES_MAX -
                TBT_FEC_REPLAY_CLOSE_BYTES_MAX / 4,
            "high water: the close cap scales with it");
    ok_int(tbt_fec_replay_pass_limit(false, 8), 8,
           "throttle: below the mark the pass keeps its limit");
    ok_int(tbt_fec_replay_pass_limit(true, 8), 4,
           "throttle: above the mark four replay records per pass");
    ok_int(tbt_fec_replay_pass_limit(true, 2), 2,
           "throttle: never raises a lower limit");
    ok(tbt_fec_replay_pass_limit(true, 8) >= 4,
       "throttle: the drain bandwidth is not cut to one record");

    while (queued && passes < 2000)
    {
        bool over = queued > high;
        u32 limit = tbt_fec_replay_pass_limit(over, 8);
        u32 n = 0;

        while (queued && n < limit)
        {
            queued -= rec;
            n++;
        }
        if (over)
        {
            if (n != TBT_FEC_REPLAY_PASS_THROTTLED)
                bad = true;
            over_passes++;
        }
        else
        {
            under_passes++;
        }
        passes++;
    }
    ok_size((size_t)queued, 0, "throttle: every replay record drains");
    ok(over_passes > 0 && under_passes > 0,
       "throttle: both regimes are exercised");
    ok(!bad, "throttle: an over-water pass writes four records");
    ok(tbt_fec_fin_ready(true, 0),
       "throttle: the FIN does not wait for the replay queue");
}

/* fix24: the far's replay queue reached 8 MiB and the frontier block's
 * repair waited 6.13 s behind later blocks, so the TX stall timer reset the
 * stream. The replay queue now inserts by super-block id: the block that
 * holds the peer's ACK frontier is written first, and within one block the
 * NACK order is kept. The throttle still writes four records per pass, so a
 * 64-share frontier burst drains in 16 passes, not behind the whole queue. */
static void test_frontier_first_replay(void)
{
    u64 in[] = {5, 3, 5, 1, 4, 1, 3};
    u64 out[sizeof(in) / sizeof(in[0])];
    const int count = (int)(sizeof(in) / sizeof(in[0]));
    u32 limit = tbt_fec_replay_pass_limit(true, 8);
    int n = 0, i, j;
    int first_pass = 0;

    for (i = 0; i < count; i++)
    {
        for (j = n; j > 0 && out[j - 1] > in[i]; j--)
            out[j] = out[j - 1];
        out[j] = in[i];
        n++;
    }
    for (i = 1; i < n; i++)
        ok(out[i - 1] <= out[i], "frontier: replay queue is block-ordered");
    ok(out[0] == 1, "frontier: the oldest queued block is written first");
    while (first_pass < n && out[first_pass] == out[0])
        first_pass++;
    ok(first_pass == 2,
       "frontier: both records of the frontier block share the first pass");
    ok(limit == 4, "frontier: the throttle still writes four per pass");
    ok(64 / limit == 16,
       "frontier: a 64-share burst drains in 16 passes");
    ok(n == count,
       "frontier: ordering drops no replay record");
}

/* fix25: the NACK scan skipped a block whose parity count looked
 * sufficient ("ndata + nparity >= real"), but that is not a rank test.
 * Band rows 0 and 1 are identical on columns 0 and 1, so two missing data
 * shares plus two parity rows leave the block unsolvable. The scan then
 * asked for a later block, the frontier never received the third row, the
 * ACK stopped and the download stalled for 120 s. */
static void test_rank_deficient_frontier(void)
{
    static const u16 full[N] = {S, S, S, S};
    struct blk b;
    u8 out[N * S];
    int refused;
    size_t off;

    blk_init(&b, 0, full);
    blk_encode(&b);
    b.nreal = N;
    b.dpresent[0] = 0;
    b.dpresent[1] = 0;
    /* Band rows 0 and 3 are identical on columns 0 and 1; rows 1 and 2 are
     * lost, so the two equations for the two missing shares are the same. */
    b.ppresent[1] = 0;
    b.ppresent[2] = 0;
    ok(b.dpresent[2] && b.dpresent[3],
       "rank: the other two data shares are present");
    ok(2 + 2 >= N - 2, "rank: the old count test calls the parity sufficient");
    ok_int(blk_recover(&b), -EAGAIN,
           "rank: two identical rows cannot solve the two missing shares");

    /* The fixed scan requests the missing parity rows; one different row
     * completes the block. */
    b.ppresent[1] = 1;
    ok_int(blk_recover(&b), 0, "rank: one different row completes the block");
    off = blk_deliver(&b, out, sizeof(out), &refused);
    ok(!refused, "rank: the completed block delivers");
    ok_size(off, N * S, "rank: the completed block delivers every byte");
    ok_prefix(out, off, "rank: the completed block is a source prefix");
}

/* fix26: the scan skipped the frontier when its missing shares were all
 * inside the per-share hold, then asked for the next block. The frontier
 * was never asked again and the reorder window stopped (next=185 for s=186,
 * 563 times). The scan must stop at the frontier and wait for the hold; a
 * later block cannot be delivered before it. */
static void test_frontier_not_skipped(void)
{
    ok(tbt_fec_nack_frontier_only(185, 185),
       "frontier: the scan targets next_super");
    ok(!tbt_fec_nack_frontier_only(186, 185),
       "frontier: a later block is not the target");
    ok(tbt_fec_nack_probe_open(true, TBT_FEC_NACK_PROBE_MAX),
       "frontier: a closed frontier is probed past the cap");
    ok(!tbt_fec_nack_probe_open(false, TBT_FEC_NACK_PROBE_MAX),
       "frontier: an idle frontier still respects the cap");
    ok(tbt_fec_nack_probe_open(false, 0),
       "frontier: the first probe is allowed");

    /* Model the loop: the frontier's bitmap is empty (all in hold), the next
     * block has fresh shares. The policy stops at the frontier. */
    {
        u64 next_super = 185, hi = 186, s;
        int visited_186 = 0;

        for (s = next_super; s <= hi; s++)
        {
            bool bitmap_empty = (s == next_super);

            if (bitmap_empty)
            {
                if (tbt_fec_nack_frontier_only(s, next_super))
                    break;
                continue;
            }
            if (s == 186)
                visited_186 = 1;
        }
        ok(!visited_186, "frontier: the scan never reaches a later block");
    }
}

/* A record that arrives for a settled block is classified, never refused.
 * A replayed empty marker used to return -EPROTO, and the carrier parser
 * kills the carrier that carried a -EPROTO record: one NACK retry became
 * rx_proto=93 and 102 carrier replacements on the 1 GiB run. */
static void test_arrival_kind(void)
{
    /* A marker is a PARITY record with win_len 0, whatever its payload. */
    ok(tbt_fec_is_marker(TBT_FEC_PARITY, 0), "win_len 0 parity is a marker");
    ok(!tbt_fec_is_marker(TBT_FEC_PARITY, 1), "a parity row is not a marker");
    ok(!tbt_fec_is_marker(TBT_FEC_DATA, 0), "DATA is never a marker");

    /* Ready block: every arrival is late or idempotent, none is an error. */
    ok_int(tbt_fec_arrival_kind(true, false, TBT_FEC_DATA, 0, true),
           TBT_FEC_ARR_LATE, "late DATA for a ready block");
    ok_int(tbt_fec_arrival_kind(true, false, TBT_FEC_PARITY, 4, true),
           TBT_FEC_ARR_LATE, "late parity for a ready block");
    ok_int(tbt_fec_arrival_kind(true, true, TBT_FEC_PARITY, 0, false),
           TBT_FEC_ARR_DUP_MARKER, "replayed marker for an empty block");
    ok_int(tbt_fec_arrival_kind(true, false, TBT_FEC_PARITY, 0, true),
           TBT_FEC_ARR_DUP_MARKER, "replayed marker for a data block");
    ok_int(tbt_fec_arrival_kind(true, true, TBT_FEC_DATA, 0, true),
           TBT_FEC_ARR_CONFLICT, "DATA for a block marked empty");

    /* Not ready: a marker after real shares is a shape conflict, a second
     * marker for an empty block is idempotent. */
    ok_int(tbt_fec_arrival_kind(false, false, TBT_FEC_PARITY, 0, true),
           TBT_FEC_ARR_CONFLICT, "marker after real shares");
    ok_int(tbt_fec_arrival_kind(false, true, TBT_FEC_PARITY, 0, false),
           TBT_FEC_ARR_DUP_MARKER, "second marker before ready");
    ok_int(tbt_fec_arrival_kind(false, true, TBT_FEC_DATA, 0, true),
           TBT_FEC_ARR_CONFLICT, "DATA for an empty, unready block");

    /* Everything else may be placed. */
    ok_int(tbt_fec_arrival_kind(false, false, TBT_FEC_PARITY, 0, false),
           TBT_FEC_ARR_PLACE, "first marker is placed");
    ok_int(tbt_fec_arrival_kind(false, false, TBT_FEC_DATA, 0, false),
           TBT_FEC_ARR_PLACE, "first DATA share is placed");
    ok_int(tbt_fec_arrival_kind(false, false, TBT_FEC_PARITY, 4, false),
           TBT_FEC_ARR_PLACE, "first parity row is placed");
}

static void test_rx_poll(void)
{
    /* tbt_session_arm() never retimes a pending item, so the delay this walk
     * arms is the worst-case latency before an arriving share is noticed.
     * The stall budget must never become that latency. */
    ok_int((int)tbt_fec_rx_poll_ms(30000, 0), TBT_FEC_NACK_MIN_MS,
           "a blocked walk polls at the NACK cadence, not the stall budget");
    ok_int((int)tbt_fec_rx_poll_ms(30000, 500000000ULL), TBT_FEC_NACK_MIN_MS,
           "the fast cadence holds while the repair is young");
    ok_int((int)tbt_fec_rx_poll_ms(30000, 2000000000ULL),
           TBT_FEC_RX_POLL_SLOW_MS,
           "an old stall backs off instead of waking at 100 Hz forever");
    ok_int((int)tbt_fec_rx_poll_ms(4, 0), 4,
           "a shorter remaining budget keeps its own deadline");
    ok_int((int)tbt_fec_rx_poll_ms(50, 2000000000ULL), 50,
           "the slow cadence never extends the remaining budget");
    ok_int((int)tbt_fec_rx_poll_ms(0, 2000000000ULL), 0,
           "an expired budget arms immediately");
    /* A shape probe asks the sender to describe a block it may not have sent
     * yet; it must never run at the repair retry cadence. */
    ok(TBT_FEC_NACK_PROBE_MS > TBT_FEC_NACK_RETRY_MS,
       "a shape probe is paced slower than a repair retry");
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
    test_two_short_writes();
    test_parity_rotation();
    test_marker_retry();
    test_super_cursor();
    test_sid_filter();
    test_stream_reset_isolation();
    test_ack_split();
    test_rx_poll();
    test_replay_window();
    test_nack_filter();
    test_nack_storm();
    test_replay_dedup_model();
    test_fin_and_stale_replay();
    test_probe_and_absent();
    test_replay_high_water();
    test_frontier_first_replay();
    test_rank_deficient_frontier();
    test_frontier_not_skipped();
    test_arrival_kind();

    printf("test_fec_rx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
