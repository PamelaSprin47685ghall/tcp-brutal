/* test_rcvq_plan.c - host specification test for the pure rcvq planner.
 *
 * The kernel calls two decisions before it injects recovered bytes into a
 * carrier's TCP receive queue.  This test fixes both against tables:
 *
 *   tbt_rcvq_pick_carrier()
 *     unique missing carrier, first/last slot, n == 0, multiple missing,
 *     all arrived, empty range, an arrived carrier outside the range,
 *     duplicates, the 256-bit edges, and the argument caps.
 *
 *   tbt_rcvq_plan()
 *     no hole (equal and behind), whole-record hole without a record start,
 *     short/long hole without one, delta 0/positive/negative/past the record,
 *     inject truncation by the hole and by the record tail, the 32-bit
 *     sequence wrap, a 65535-byte record, and zero-length records.
 *
 * A skip must also clear delta and inject_len, so a caller can never read a
 * stale range.  The file exports no main(): host/test_rcvq.c owns the runner
 * and calls test_rcvq_plan().  Success prints PLAN_TEST_PASS and returns 0;
 * any failure prints the detail to stderr and returns 1.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tbt_rcvq.h"

static int checks;
static int failures;

/* The runner in host/test_rcvq.c declares the same prototype. */
int test_rcvq_plan(void);

#define CHECK(cond, ...)                                                \
    do                                                                  \
    {                                                                   \
        checks++;                                                       \
        if (!(cond))                                                    \
        {                                                               \
            failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                               \
            fputc('\n', stderr);                                        \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* tbt_rcvq_pick_carrier()                                             */
/* ------------------------------------------------------------------ */

struct pick_case
{
    const char *name;
    const u8 *src;
    u16 n;
    u16 base;
    u16 span;
    int want_ret;
    u8 want_carrier;
    bool check_carrier;
};

static const u8 pick_unique[] = { 0, 1, 3, TBT_RCVQ_NO_CARRIER };
static const u8 pick_first[] = { TBT_RCVQ_NO_CARRIER, 9, 10, 11 };
static const u8 pick_last[] = { 8, 9, 10, TBT_RCVQ_NO_CARRIER };
static const u8 pick_two_missing[] = { 0, TBT_RCVQ_NO_CARRIER,
                                       TBT_RCVQ_NO_CARRIER, 3 };
static const u8 pick_all[] = { 0, 1, 2, 3 };
static const u8 pick_below[] = { 0, 1, 2 };
static const u8 pick_above[] = { 1, 2, 3 };
static const u8 pick_duplicate[] = { 1, 1, 3, 2 };
static const u8 pick_none[] = { TBT_RCVQ_NO_CARRIER, TBT_RCVQ_NO_CARRIER };
static const u8 pick_edge[] = { 250, 251, 252, 253, 254 };
static const u8 pick_wide[300] = { 0 };

static const struct pick_case pick_cases[] = {
    {
        .name = "unique-missing-middle",
        .src = pick_unique, .n = 4, .base = 0, .span = 4,
        .want_ret = 0, .want_carrier = 2, .check_carrier = true,
    },
    {
        .name = "unique-missing-base",
        .src = pick_first, .n = 4, .base = 8, .span = 4,
        .want_ret = 0, .want_carrier = 8, .check_carrier = true,
    },
    {
        .name = "unique-missing-last",
        .src = pick_last, .n = 4, .base = 8, .span = 4,
        .want_ret = 0, .want_carrier = 11, .check_carrier = true,
    },
    {
        .name = "n-zero-one-slot",
        .src = NULL, .n = 0, .base = 7, .span = 1,
        .want_ret = 0, .want_carrier = 7, .check_carrier = true,
    },
    {
        .name = "multiple-missing",
        .src = pick_two_missing, .n = 4, .base = 0, .span = 4,
        .want_ret = -ENOENT,
    },
    {
        .name = "n-zero-many-slots",
        .src = NULL, .n = 0, .base = 0, .span = 4,
        .want_ret = -ENOENT,
    },
    {
        .name = "all-arrived",
        .src = pick_all, .n = 4, .base = 0, .span = 4,
        .want_ret = -ENOENT,
    },
    {
        .name = "span-zero",
        .src = NULL, .n = 0, .base = 5, .span = 0,
        .want_ret = -ENOENT,
    },
    {
        .name = "arrived-below-range",
        .src = pick_below, .n = 3, .base = 1, .span = 3,
        .want_ret = -EINVAL,
    },
    {
        .name = "arrived-above-range",
        .src = pick_above, .n = 3, .base = 0, .span = 3,
        .want_ret = -EINVAL,
    },
    {
        .name = "duplicate-name",
        .src = pick_duplicate, .n = 4, .base = 0, .span = 4,
        .want_ret = 0, .want_carrier = 0, .check_carrier = true,
    },
    {
        .name = "none-arrived",
        .src = pick_none, .n = 2, .base = 0, .span = 4,
        .want_ret = -ENOENT,
    },
    {
        /* Slot 255 is the only unnamed carrier.  It is also the value of
         * TBT_RCVQ_NO_CARRIER in src_carrier[], so a group that contains
         * carrier 255 is representable only while its share has not arrived.
         * The planner reports it as the missing slot; the caller's hole check
         * is what keeps that ambiguity harmless. */
        .name = "carrier-space-edge",
        .src = pick_edge, .n = 5, .base = 250, .span = 6,
        .want_ret = 0, .want_carrier = 255, .check_carrier = true,
    },
    {
        .name = "n-too-large",
        .src = pick_wide, .n = 257, .base = 0, .span = 1,
        .want_ret = -EINVAL,
    },
    {
        .name = "span-too-large",
        .src = NULL, .n = 0, .base = 0, .span = 257,
        .want_ret = -EINVAL,
    },
    {
        .name = "range-past-255",
        .src = NULL, .n = 0, .base = 255, .span = 2,
        .want_ret = -EINVAL,
    },
    {
        .name = "null-src",
        .src = NULL, .n = 1, .base = 0, .span = 1,
        .want_ret = -EINVAL,
    },
};

static void test_pick_carrier(void)
{
    size_t i;

    for (i = 0; i < sizeof(pick_cases) / sizeof(pick_cases[0]); i++)
    {
        const struct pick_case *t = &pick_cases[i];
        u8 got = 0xaa;
        int ret;

        ret = tbt_rcvq_pick_carrier(t->src, t->n, t->base, t->span, &got);
        CHECK(ret == t->want_ret, "pick %s: ret %d, want %d", t->name, ret,
              t->want_ret);
        if (t->check_carrier)
            CHECK(ret == 0 && got == t->want_carrier,
                  "pick %s: carrier %u, want %u", t->name, got,
                  t->want_carrier);
        else
            CHECK(got == 0xaa, "pick %s: wrote carrier %u on failure", t->name,
                  got);
    }

    CHECK(tbt_rcvq_pick_carrier(NULL, 0, 0, 1, NULL) == -EINVAL,
          "pick null-carrier: NULL output accepted");
}

static void test_pick_full_group(void)
{
    u8 src[255];
    u8 got = 0;
    u16 i;
    int ret;

    /* The widest legal range: all 256 carrier bits.  Shares on carriers
     * 0..254 arrived, so slot 255 is the set difference. */
    for (i = 0; i < 255; i++)
        src[i] = (u8)i;
    ret = tbt_rcvq_pick_carrier(src, 255, 0, 256, &got);
    CHECK(ret == 0 && got == 255,
          "pick span-256: ret %d carrier %u, want 0/255", ret, got);
}

/* ------------------------------------------------------------------ */
/* tbt_rcvq_plan()                                                     */
/* ------------------------------------------------------------------ */

struct plan_case
{
    const char *name;
    u32 rcv_nxt;
    u32 ooo_seq;
    u32 record_seq;
    u16 wire_len;
    u8 carrier;
    u8 expect_carrier;
    u8 gen_ok;
    u8 has_record_seq;
    enum tbt_rcvq_skip want_skip;
    u16 want_delta;
    u16 want_inject;
};

static const struct plan_case plan_cases[] = {
    {
        .name = "gen-mismatch",
        .rcv_nxt = 1000, .ooo_seq = 1200, .record_seq = 1000,
        .wire_len = 200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 0, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_CARRIER,
    },
    {
        .name = "carrier-mismatch",
        .rcv_nxt = 1000, .ooo_seq = 1200, .record_seq = 1000,
        .wire_len = 200, .carrier = 0, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_CARRIER,
    },
    {
        /* The carrier check runs before the hole check. */
        .name = "carrier-before-nohole",
        .rcv_nxt = 1000, .ooo_seq = 1000, .record_seq = 1000,
        .wire_len = 200, .carrier = 0, .expect_carrier = 1,
        .gen_ok = 0, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_CARRIER,
    },
    {
        .name = "no-hole",
        .rcv_nxt = 1000, .ooo_seq = 1000, .record_seq = 1000,
        .wire_len = 200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_NOHOLE,
    },
    {
        .name = "ooo-head-behind",
        .rcv_nxt = 1000, .ooo_seq = 900, .record_seq = 900,
        .wire_len = 200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_NOHOLE,
    },
    {
        /* A distance of exactly 2^31 is a negative s32: behind, not a hole. */
        .name = "ooo-half-space",
        .rcv_nxt = 0, .ooo_seq = 0x80000000u, .record_seq = 0,
        .wire_len = 200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_NOHOLE,
    },
    {
        .name = "whole-record-no-seq",
        .rcv_nxt = 5000, .ooo_seq = 6200, .record_seq = 0,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 0,
        .want_skip = TBT_RCVQ_OK, .want_delta = 0, .want_inject = 1200,
    },
    {
        .name = "whole-record-one-byte",
        .rcv_nxt = 7, .ooo_seq = 8, .record_seq = 0,
        .wire_len = 1, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 0,
        .want_skip = TBT_RCVQ_OK, .want_delta = 0, .want_inject = 1,
    },
    {
        .name = "short-hole-no-seq",
        .rcv_nxt = 5000, .ooo_seq = 6000, .record_seq = 0,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 0,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "long-hole-no-seq",
        .rcv_nxt = 5000, .ooo_seq = 6400, .record_seq = 0,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 0,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "delta-zero-truncated",
        .rcv_nxt = 5000, .ooo_seq = 5500, .record_seq = 5000,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 0, .want_inject = 500,
    },
    {
        .name = "delta-zero-whole",
        .rcv_nxt = 5000, .ooo_seq = 6200, .record_seq = 5000,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 0, .want_inject = 1200,
    },
    {
        .name = "delta-positive-full",
        .rcv_nxt = 5000, .ooo_seq = 6500, .record_seq = 4800,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 200, .want_inject = 1000,
    },
    {
        .name = "delta-truncated-by-hole",
        .rcv_nxt = 5000, .ooo_seq = 5300, .record_seq = 4800,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 200, .want_inject = 300,
    },
    {
        .name = "delta-exact-remaining",
        .rcv_nxt = 5000, .ooo_seq = 5800, .record_seq = 4800,
        .wire_len = 1000, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 200, .want_inject = 800,
    },
    {
        .name = "delta-one-byte",
        .rcv_nxt = 5000, .ooo_seq = 5001, .record_seq = 4999,
        .wire_len = 2, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 1, .want_inject = 1,
    },
    {
        .name = "delta-last-byte",
        .rcv_nxt = 5000, .ooo_seq = 5001, .record_seq = 3801,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 1199, .want_inject = 1,
    },
    {
        .name = "delta-negative",
        .rcv_nxt = 5000, .ooo_seq = 5500, .record_seq = 5001,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_DELTA,
    },
    {
        .name = "delta-equals-wire",
        .rcv_nxt = 5000, .ooo_seq = 5500, .record_seq = 3800,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "delta-past-wire",
        .rcv_nxt = 5000, .ooo_seq = 5500, .record_seq = 3799,
        .wire_len = 1200, .carrier = 1, .expect_carrier = 1,
        .gen_ok = 1, .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "wrap-hole",
        .rcv_nxt = 0xffffff00u, .ooo_seq = 0x00000100u,
        .record_seq = 0xfffffe00u, .wire_len = 1000,
        .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 256, .want_inject = 512,
    },
    {
        .name = "wrap-delta",
        .rcv_nxt = 0x10, .ooo_seq = 0x74, .record_seq = 0xfffffff0u,
        .wire_len = 100, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 32, .want_inject = 68,
    },
    {
        .name = "wrap-whole-no-seq",
        .rcv_nxt = 0xfffffff0u, .ooo_seq = 0x10, .record_seq = 0,
        .wire_len = 32, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 0,
        .want_skip = TBT_RCVQ_OK, .want_delta = 0, .want_inject = 32,
    },
    {
        .name = "wire-len-max",
        .rcv_nxt = 100000, .ooo_seq = 170000, .record_seq = 99000,
        .wire_len = 65535, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 1,
        .want_skip = TBT_RCVQ_OK, .want_delta = 1000, .want_inject = 64535,
    },
    {
        .name = "wire-zero-no-seq",
        .rcv_nxt = 1000, .ooo_seq = 1100, .record_seq = 0,
        .wire_len = 0, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 0,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "wire-zero-with-seq",
        .rcv_nxt = 1000, .ooo_seq = 1100, .record_seq = 1000,
        .wire_len = 0, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 1,
        .want_skip = TBT_RCVQ_SKIP_OVERLAP,
    },
    {
        .name = "no-hole-zero-wire",
        .rcv_nxt = 1000, .ooo_seq = 1000, .record_seq = 0,
        .wire_len = 0, .carrier = 1, .expect_carrier = 1, .gen_ok = 1,
        .has_record_seq = 0,
        .want_skip = TBT_RCVQ_SKIP_NOHOLE,
    },
};

static void test_plan(void)
{
    size_t i;

    for (i = 0; i < sizeof(plan_cases) / sizeof(plan_cases[0]); i++)
    {
        const struct plan_case *t = &plan_cases[i];
        struct tbt_rcvq_plan_in in;
        struct tbt_rcvq_plan out;
        enum tbt_rcvq_skip ret;

        memset(&in, 0, sizeof(in));
        in.rcv_nxt = t->rcv_nxt;
        in.ooo_seq = t->ooo_seq;
        in.record_seq = t->record_seq;
        in.wire_len = t->wire_len;
        in.carrier = t->carrier;
        in.expect_carrier = t->expect_carrier;
        in.gen_ok = t->gen_ok;
        in.has_record_seq = t->has_record_seq;

        out.delta = 0xaaaa;
        out.inject_len = 0xbbbb;
        out.skip = TBT_RCVQ_OK;

        ret = tbt_rcvq_plan(&in, &out);
        CHECK(ret == t->want_skip, "plan %s: ret %d, want %d", t->name, ret,
              t->want_skip);
        CHECK(out.skip == t->want_skip, "plan %s: out.skip %d, want %d",
              t->name, out.skip, t->want_skip);
        if (t->want_skip == TBT_RCVQ_OK)
        {
            CHECK(out.delta == t->want_delta,
                  "plan %s: delta %u, want %u", t->name, out.delta,
                  t->want_delta);
            CHECK(out.inject_len == t->want_inject,
                  "plan %s: inject %u, want %u", t->name, out.inject_len,
                  t->want_inject);
        }
        else
        {
            CHECK(out.delta == 0 && out.inject_len == 0,
                  "plan %s: stale range delta %u inject %u", t->name,
                  out.delta, out.inject_len);
        }
    }
}

int test_rcvq_plan(void)
{
    checks = 0;
    failures = 0;

    test_pick_carrier();
    test_pick_full_group();
    test_plan();

    if (failures)
    {
        fprintf(stderr, "PLAN_TEST_FAIL %d of %d checks\n", failures, checks);
        return 1;
    }

    printf("PLAN_TEST_PASS\n");
    return 0;
}
