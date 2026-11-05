/* tbt_probe_uapi.h - ioctl numbers shared by tbt_probe.ko and probe_user.c.
 *
 * A probe header only; nothing here is part of tbtun's operator contract.
 */
#ifndef _TBT_PROBE_UAPI_H
#define _TBT_PROBE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct tbt_probe_stats
{
    __u64 recvmsg_calls; /* times our .recvmsg ran */
    __u64 poll_calls;    /* times our .poll ran */
    __u64 wakes;         /* times the module called wake_up_interruptible_poll */
    __u64 bytes_served;  /* payload offset consumed by readers */
    __u32 ready;         /* payload released */
    __u32 eof;           /* payload drained (or shut down) */
    __u32 handed_out;    /* accepted socket was installed as an fd */
};

#define TBT_PROBE_GETFD _IO('t', 1)
#define TBT_PROBE_STATS _IOR('t', 2, struct tbt_probe_stats)
#define TBT_PROBE_SHUTDOWN _IO('t', 3)

#endif
