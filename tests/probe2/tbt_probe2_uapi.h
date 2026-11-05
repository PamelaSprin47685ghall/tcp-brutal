/* tbt_probe2_uapi.h - ioctl ABI between tbt_probe2.ko and probe2_user.c.
 *
 * Probe only; not part of tbtun's operator contract.
 *
 * Field names avoid every sk_* macro from <net/sock.h>: that header defines
 * e.g. `sk_rmem_alloc` as a token sequence, so a struct member of the same
 * name silently rewrites the declaration that contains it.
 */
#ifndef _TBT_PROBE2_UAPI_H
#define _TBT_PROBE2_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct tbt_p2_inject
{
    __u32 len;
    __u8 byte;
    __u8 pad[3];
};

struct tbt_p2_claim
{
    __u32 tgid;
};

struct tbt_p2_status
{
    __u32 rcv_nxt;
    __u32 copied_seq;
    __u32 rcv_wup;
    __u32 rcv_wnd;
    __u32 rcv_ssthresh;
    __u32 rcv_mss;
    __u32 rcvbuf;
    __u32 mem_alloc;
    __u32 state;
    __u32 sock_done;
    __u32 shutdown;
    __s32 sk_err;
    __u32 qlen;
    __u32 urg_data;
    __u32 repair;
    __u32 repair_queue;
    __u32 snd_wnd;
    __u32 snd_nxt;
    __u32 snd_una;
    __u32 write_seq;
    __u32 packets_out;
    __u32 retrans;
    __u32 wmem_queued;
    __u32 tcp_header_len;
    __u32 app_tgid;
    __u64 inject_calls;
    __u64 inject_bytes;
    __u64 inject_errors;
    __u64 bytes_received;
};

/* The head of sk_receive_queue, i.e. what tcp_recvmsg_locked will look at. */
struct tbt_p2_skbinfo
{
    __u32 seq;
    __u32 end_seq;
    __u32 ack_seq;
    __u32 rcv_nxt;
    __u32 copied_seq;
    __u32 urg_data;
    __u32 qlen;
    __u32 tcp_flags;
    __u32 len;
    __u32 data_len;
    __u32 truesize;
    __u32 users;
    __u32 offset_from_copied; /* seq - copied_seq, as recvmsg computes it */
};

#define TBT_P2_INJECT _IOW('P', 1, struct tbt_p2_inject)
#define TBT_P2_STATUS _IOR('P', 2, struct tbt_p2_status)
#define TBT_P2_CLOSE _IO('P', 3)
#define TBT_P2_FORCE_SEND _IO('P', 6)
#define TBT_P2_GETFD _IO('P', 7)
#define TBT_P2_SKBINFO _IOR('P', 8, struct tbt_p2_skbinfo)
#define TBT_P2_DISARM _IO('P', 9)
#define TBT_P2_ARM _IO('P', 11)
#define TBT_P2_CLAIM _IOW('P', 10, struct tbt_p2_claim)

#endif
