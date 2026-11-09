/* tbt_rx_budget.h - how much one carrier RX work item may consume.
 *
 * tbt_car_rx_work() drains a carrier's whole receive queue with one
 * tcp_read_sock(). The queue can hold two rcvbufs (up to 2 MiB), and every FEC
 * record in it costs an HTTP deframe, a header CRC, a ChaCha8 XOR, a payload
 * CRC and the copy into the FEC block. On the 1 GiB gates the far side logged
 *
 *   workqueue: tbt_car_rx_work [tbtun] hogged CPU for >10000us N times
 *
 * because one work item could parse that whole queue before it returned.
 *
 * The budget bounds one item to TBT_RX_RUN_NS of wall time, checked between
 * TBT_RX_RUN_SLICE-byte pieces. A short actor return is legal:
 * tcp_read_sock() advances copied_seq by the used bytes and calls the actor
 * again; a 0-byte return stops the walk without consuming anything. The body
 * then sees copied_seq != rcv_nxt and re-arms, so the next work item reads
 * the remainder. No byte is dropped, duplicated, or re-parsed.
 *
 * Pure code: no kernel headers, no state of its own beyond the caller's
 * struct. host/test_rx_budget.c compiles this file and drives the same stop
 * condition the module runs.
 */
#ifndef _TBT_RX_BUDGET_H
#define _TBT_RX_BUDGET_H

#include "tbt_uapi.h"

/* One work item runs at most this long. The far kernel reports a hogged
 * worker at 10 ms; 2 ms leaves room for one slice past the deadline and for
 * the rest of the body (pause scan, pending-record retry). */
#define TBT_RX_RUN_NS 2000000ULL

/* The actor stops only between pieces of this size. 8 KiB is one parser
 * fill (TBT_PARSER_BUF is 8240): a piece can complete at most one 8 KiB FEC
 * share, so the overshoot past the deadline is one record's deframe, two
 * CRCs, decrypt and copy (measured at 20 us on a host with the table CRC;
 * a 10x slower target still leaves >400x margin to 10 ms). On the raw
 * profile a piece holds at most 256 minimal records, whose per-record work
 * is a header scan. */
#define TBT_RX_RUN_SLICE 8192u

/* Threshold in the far dmesg line. The host test asserts the budget plus one
 * slice stays below it, so a later tuning change cannot silently reintroduce
 * the warning. */
#define TBT_RX_WQ_HOG_NS 10000000ULL

struct tbt_rx_budget
{
    u64 deadline_ns;
    size_t bytes; /* handed to the parser in this run */
};

static inline void tbt_rx_budget_start(struct tbt_rx_budget *b, u64 now_ns)
{
    b->deadline_ns = now_ns + TBT_RX_RUN_NS;
    b->bytes = 0;
}

static inline void tbt_rx_budget_charge(struct tbt_rx_budget *b, size_t bytes)
{
    b->bytes += bytes;
}

/* The actor may stop only after it has consumed something this run. Without
 * the bytes test, a run that spent its deadline waiting for the socket lock
 * would return 0 before the first skb, and the body would read a zero drain.
 * A run always makes progress; the first piece is bounded by one slice. */
static inline bool tbt_rx_budget_stopped(const struct tbt_rx_budget *b,
                                         u64 now_ns)
{
    return b->bytes != 0 && now_ns >= b->deadline_ns;
}

/* What one finished RX work item does next. Shared with the host test so the
 * re-arm rule the budget depends on has one definition:
 *
 *   - unread socket bytes => arm now. This is the budgeted-yield path and the
 *     ordinary "bytes arrived during the run" path; they must not diverge.
 *   - parser holds a record but the socket is empty => wait for the 50 ms
 *     poll or sk_data_ready. Arming here would re-run a parser that cannot
 *     make progress, which is the kworker spin the comment in
 *     tbt_car_rx_body() warns about.
 *   - a stopping carrier or a missing socket owns no wake.
 */
enum tbt_rx_resume
{
    TBT_RX_RESUME_IDLE = 0,
    TBT_RX_RESUME_ARM,
    TBT_RX_RESUME_WAIT,
};

static inline enum tbt_rx_resume
tbt_rx_resume_plan(bool stopping, bool sk_present, bool socket_unread,
                   bool parser_pending)
{
    if (stopping || !sk_present)
        return TBT_RX_RESUME_IDLE;
    if (socket_unread)
        return TBT_RX_RESUME_ARM;
    if (parser_pending)
        return TBT_RX_RESUME_WAIT;
    return TBT_RX_RESUME_IDLE;
}

#endif /* _TBT_RX_BUDGET_H */
