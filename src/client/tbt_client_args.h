/* tbt_client_args.h - userspace-only command-line keys for tbt_client.
 *
 * The kernel grammar owns every tunnel key. Four keys belong to the host
 * process alone and must never reach it:
 *
 *     drop_ppm=  run_ms=  local_queue_bytes=  verbose=
 *
 * This unit splits one invocation into the kernel line and those four
 * values: inline `key=value ...`, or a tunnel line from `--config FILE`
 * plus trailing userspace keys. It allocates nothing and holds no state,
 * so the host test drives exactly the code the binary runs.
 */
#ifndef _TBT_CLIENT_ARGS_H
#define _TBT_CLIENT_ARGS_H

#include <stddef.h>

#include "tbt_parse.h"

struct tbt_client_us
{
    u32 drop_ppm;
    u32 run_ms;
    u32 local_queue_bytes;
    u32 verbose;
};

/* One exact key=<u32> token. Returns 1 when tok is `key`, 0 when it is not
 * this key, and -1 when the key matches but the value is not a u32. */
int tbt_client_us_key(const char *tok, const char *key, u32 *out);

/* Remove every userspace key from one kernel line in place. Returns 0, or
 * -1 when such a key has a malformed value; `bad` then names the token. */
int tbt_client_us_strip(char *line, size_t cap, struct tbt_client_us *us,
                        char *bad, size_t badcap);

/* Apply argv[start..] to us. Every token must be a userspace key. Returns
 * -1 when all were consumed, else the index of the first bad token. */
int tbt_client_us_argv(int argc, char **argv, int start,
                       struct tbt_client_us *us);

/* Read the first non-blank, non-comment line of path into line, with its
 * CR/LF terminator removed. Returns 0, -ENODATA when the file holds no line,
 * -E2BIG when a line does not fit, -EINVAL when cap cannot hold a line, or
 * another negative errno for an open or read failure. */
int tbt_client_config_read(const char *path, char *line, size_t cap);

/* Join argv[start..] into the kernel line and remove userspace keys. Returns
 * 0, -E2BIG when the line does not fit, or -EINVAL for a malformed userspace
 * value. */
int tbt_client_line_build(int argc, char **argv, int start, char *line,
                          size_t cap, struct tbt_client_us *us);

/* Append ` role=near` when line has no whitespace-delimited token that
 * begins with `role=`. A `role` inside another key or value does not count.
 * Returns 0, or -E2BIG when the append does not fit cap (line unchanged). */
int tbt_client_role_default(char *line, size_t cap);

/* Reject a parsed tunnel line the userspace engine cannot honor. It always
 * runs the fec=on SC-LDPC data plane and has no map=, srcaddrs=, dev=,
 * portblock= or tunnel_rate= support; profile= is left to the engine's
 * framing. Returns 0 when the line can run, or -1 with *why naming the first
 * unsupported value (a static string). A NULL `why` is allowed. */
int tbt_client_cfg_check(const struct tbt_cfg *cfg, const char **why);

#endif /* _TBT_CLIENT_ARGS_H */
