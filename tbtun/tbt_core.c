/* tbt_core.c - module lifecycle, global state, module parameters.
 *
 * Owns:
 *   - module_init/module_exit and their strict ordering: /proc first (so a
 *     failed later step leaves nothing behind), teardown in reverse.
 *   - the global tunnel list and tbt_lock, which every /proc writer takes.
 *   - module params (read-only after load, DESIGN.md §8.4.2).
 *   - the load-time capability check and the compile-time frame assertions.
 *
 * Must never:
 *   - touch a packet, a socket, or a frame. The data path lives in
 *     tbt_listen.c (C2+), tbt_tx.c, tbt_rx.c.
 *   - allocate per-tunnel memory; that is tbt_session.c.
 *   - sleep while holding a lock that the data path can take.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/vmstat.h>
#include <linux/vmalloc.h>

#include "tbtun.h"

/* Approximate MemAvailable in KiB: free pages plus reclaimable page cache
 * and slab, minus a small watermark reserve. si_mem_available() is exported
 * on the far kernel but its declaration and unit are not part of the header
 * payload this module builds against; the counters below are stable ABI. */
long tbt_mem_avail_kb(void)
{
    unsigned long free_pages = global_zone_page_state(NR_FREE_PAGES);
    unsigned long file_pages = global_node_page_state(NR_FILE_PAGES);
    unsigned long reserve = totalram_pages() / 32;
    long pages = (long)free_pages + (long)file_pages - (long)reserve;

    if (pages < 0)
        pages = 0;
    return pages * (long)(PAGE_SIZE / 1024);
}

LIST_HEAD(tbt_tunnels);
DEFINE_MUTEX(tbt_lock);

unsigned int debug;
module_param(debug, uint, 0444);
MODULE_PARM_DESC(debug, "Debug verbosity (0..3)");

unsigned int max_tunnels = 64;
module_param(max_tunnels, uint, 0444);
MODULE_PARM_DESC(max_tunnels, "Maximum configured tunnels (default 64)");

unsigned int max_carriers = TBT_MAX_CARRIERS_LIMIT;
module_param(max_carriers, uint, 0444);
MODULE_PARM_DESC(max_carriers, "Carrier sockets per session, 1..256 (default 256)");

/* Debug-only loss injection gate. `carrier <n> drop_ppm <ppm>` is refused
 * unless the module was loaded with debug_loss=1, so a production module
 * cannot be talked into dropping frames through /proc. Read-only after load
 * (DESIGN.md §8.4.2). */
bool debug_loss;
module_param(debug_loss, bool, 0444);
MODULE_PARM_DESC(debug_loss, "Allow carrier drop_ppm (frame loss injection) writes; 0 refuses them");

/* Peripheral BBR hold: after 200 ms of carrier idleness the 50 ms poll saves
 * snd_cwnd and sk_pacing_rate, then restores both while the session stays
 * idle and TCP has nothing in flight, so BBR does not start the next burst
 * from an idle-collapsed window. The snapshot is bounded by snd_wnd/mss,
 * snd_cwnd_clamp, sk_sndbuf/mss and sk_max_pacing_rate. A non-PAD write
 * cancels the hold before it queues bytes, so BBR's first real ACK owns the
 * next decision. Default on; write 0 to
 * /sys/module/tbtun/parameters/hold_enable to opt out. It writes tcp_sock
 * fields directly, which is not a stable kernel API. */
bool tbt_hold_enable = true;
module_param_named(hold_enable, tbt_hold_enable, bool, 0644);
MODULE_PARM_DESC(hold_enable, "Hold BBR cwnd/pacing across idle periods (default 1; 0=off)");

/* Borrowed-shell retransmission experiment. Off by default and read-only
 * after load. When enabled, a carrier socket caps its GSO size at the
 * current MSS and marks every carrier write MSG_EOR, so one encoded FEC
 * chunk occupies exactly one skb. The substitution engine itself is gated
 * by the same flag; this switch also turns on its counters. */
bool tbt_shell_retx;
module_param_named(shell_retx, tbt_shell_retx, bool, 0444);
MODULE_PARM_DESC(shell_retx, "Borrowed-shell retransmission experiment (default 0)");

/* Retained for configuration compatibility. 0.7.0 carries one FEC flow per
 * session and that flow owns every group, so this value is ignored. */
unsigned int tbt_fec_groups_per_flow;
module_param_named(fec_groups_per_flow, tbt_fec_groups_per_flow, uint, 0444);
MODULE_PARM_DESC(fec_groups_per_flow, "Ignored: 0.7.0 runs one FEC flow that owns all groups");

// The wire header is defined field by field in DESIGN.md §3.3; a compiler that
// pads it differently would produce bytes that no peer can parse. Checked at
// build time, so a layout change cannot reach a running module.
static void tbt_wire_layout_check(void)
{
    BUILD_BUG_ON(TBT_HDR_LEN + TBT_PAYLOAD_MAX != TBT_REC_MAX);
    BUILD_BUG_ON(TBT_REC_MAX > TBT_CHUNK_MAX); // a full record fits in a chunk
    /* The FEC batch buffer must stay in the kmalloc-32k class (order-3 on
     * 4K pages). Eight 8260 B frames crossed 64 KiB and needed order-5
     * pages; that allocation failed on the 955 MB far host and stalled the
     * drain in direct reclaim. */
    BUILD_BUG_ON(TBT_CAR_PART_MAX > 32768);
    /* Every field offset of DESIGN.md §3.3, checked against both the TBT_H_*
     * constants and the struct layout, so a "cleanup" of either cannot change
     * the bytes on the wire without failing the build. */
    BUILD_BUG_ON(TBT_H_TYPE != 0);
    BUILD_BUG_ON(TBT_H_FLAGS != 1);
    BUILD_BUG_ON(TBT_H_PATH != 2);
    BUILD_BUG_ON(TBT_H_ACKC != 3);
    BUILD_BUG_ON(TBT_H_ACKM != 4);
    BUILD_BUG_ON(TBT_H_BODYLEN != 6);
    BUILD_BUG_ON(TBT_H_SEQ != 8);
    BUILD_BUG_ON(TBT_H_UACK != 16);
    BUILD_BUG_ON(TBT_H_SIDLO != 24);
    BUILD_BUG_ON(TBT_H_SIDHI != 28);
    BUILD_BUG_ON(TBT_H_RES2 != 29);
    BUILD_BUG_ON(TBT_H_CONV != 30);
    BUILD_BUG_ON(TBT_HDR_LEN != 32);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, type) != TBT_H_TYPE);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, flags) != TBT_H_FLAGS);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, path_id) != TBT_H_PATH);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, path_ack_count) != TBT_H_ACKC);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, path_ack_mask) != TBT_H_ACKM);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, body_len) != TBT_H_BODYLEN);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, seq) != TBT_H_SEQ);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, uack) != TBT_H_UACK);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, sid_lo) != TBT_H_SIDLO);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, sid_hi) != TBT_H_SIDHI);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, reserved2) != TBT_H_RES2);
    BUILD_BUG_ON(offsetof(struct tbt_hdr, conv) != TBT_H_CONV);
    BUILD_BUG_ON(sizeof(struct tbt_hdr) != TBT_HDR_LEN);
    BUILD_BUG_ON(TBT_LEGACY_MAX_CARRIERS > 16); // path_ack_mask is u16
    BUILD_BUG_ON(TBT_LINE_MAX < 256);          // room for the longest documented line
}

bool tbt_debug_loss(void)
{
    return READ_ONCE(debug_loss);
}

/* The module needs no unexported kernel symbol. Every function it calls is
 * EXPORT_SYMBOL or EXPORT_SYMBOL_GPL: tcp_sendmsg_locked, tcp_read_sock,
 * sock_create_kern, kernel_bind, kernel_connect, kernel_listen, kernel_accept,
 * sock_release, kernel_sock_shutdown, kernel_sendmsg. That is deliberate: the
 * earlier design resolved tcp_send_rcvq through a one-shot kprobe to inject
 * bytes into an accepted socket's receive queue. Measurement showed that queue
 * is readable only by the module, never by the application (whose fd is the
 * client socket), so delivery uses kernel_sendmsg on the socket the stream owns
 * and the kprobe is gone (DESIGN.md §8.5, §8.8). */

static int __init tbtun_init(void)
{
    int ret, err;

    if (!max_carriers || max_carriers > TBT_MAX_CARRIERS_LIMIT)
    {
        pr_err("tbtun: max_carriers must be 1..%d\n", TBT_MAX_CARRIERS_LIMIT);
        return -EINVAL;
    }
    if (!max_tunnels)
    {
        pr_err("tbtun: max_tunnels must be >= 1\n");
        return -EINVAL;
    }

    tbt_wire_layout_check();

    /* The evidence file and /proc identity must name the build that is
     * running, not the .ko that was copied. A stale module made the first
     * 0.7.1 FIN probes look absent. */
    pr_info("tbtun: probe build 071-final-1\n");

    ret = tbt_fec_data_module_init();
    if (ret)
    {
        pr_err("tbtun: cannot create the FEC free workqueue: %d\n", ret);
        return ret;
    }

    ret = tbt_procfs_init();
    if (ret)
    {
        pr_err("tbtun: cannot create /proc/net/tbtun: %d\n", ret);
        tbt_fec_data_module_exit();
        return ret;
    }

    err = tbt_carrier_init();
    if (err)
    {
        tbt_procfs_exit();
        tbt_fec_data_module_exit();
        return err;
    }
    pr_info("tbtun %u.%u.%u loaded: %u tunnels max, %u carriers max\n",
            TBT_VERSION_MAJOR, TBT_VERSION_MINOR, TBT_VERSION_PATCH,
            max_tunnels, max_carriers);
    return 0;
}

static void __exit tbtun_exit(void)
{
    /* Stop new /proc calls and wait for every in-flight fop.  remove_proc_entry()
     * waits for pde users and closes pde_openers, including tbt_cfg_release(). */
    tbt_procfs_exit();
    /* tbt_tunnel_shutdown() detaches the list under tbt_lock and tears the
     * sessions down outside it; a destroy that waits for work must not pin
     * the global lock. */
    tbt_tunnel_shutdown();
    /* Every carrier socket is closed by the teardown above, so the wrapper
     * congestion control has no user left. */
    tbt_shell_retx_exit();
    /* No flow can be put after the session teardown. Drain and destroy the
     * module-owned flow free workqueue: session destroy queues the last puts
     * there, and synchronize_rcu()/rcu_barrier() do not wait for work items.
     * Without this barrier a tbt_fec_flow_free_work queued by the last put
     * would run after the module text is unmapped. */
    tbt_fec_data_module_exit();
    /* No module callback or work item is reachable now.  Wait for readers that
     * started before the teardown, then for the module-text RCU callbacks.
     * flush_workqueue(system_wq) would not be a barrier: it does not stop new
     * scheduling, does not flush delayed work, and does not cover
     * TBT_WQ (w_kill, w_fbk, w_pump; system_dfl_wq on 6.5+, system_unbound_wq on 6.1). */
    synchronize_rcu();
    rcu_barrier();
    /* Every carrier released its port during the tunnel teardown; the bitmap
     * itself is module state and goes away last. */
    tbt_portmap_exit();
    pr_info("tbtun: unloaded\n");
}

module_init(tbtun_init);
module_exit(tbtun_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tbtun: TCP tunnel over N+M HTTP/1.1-ish carrier connections");
MODULE_VERSION(__stringify(TBT_VERSION_MAJOR) "." __stringify(TBT_VERSION_MINOR) "."
               __stringify(TBT_VERSION_PATCH));
MODULE_AUTHOR("tbtun contributors");
