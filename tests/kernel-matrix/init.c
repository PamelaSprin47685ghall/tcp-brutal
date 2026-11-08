/*
 * init.c - PID 1 for the kernel-matrix load check. No libc, no busybox.
 *
 * The guest image carries exactly two files: /init (this program) and
 * /tbtun.ko.  /init mounts procfs, loads the module, checks its name in
 * /proc/modules, unloads it, checks that it is gone, loads and unloads it a
 * second time, then powers the machine off.  Every step prints one
 * "KMATRIX ..." line to /dev/console; qemu's serial log is the artifact.
 *
 * Build (scripts/kernel-matrix.sh does this):
 *   gcc -static -nostdlib -ffreestanding -O2 -o init_x86_64 init.c
 *   aarch64-linux-gnu-gcc -static -nostdlib -ffreestanding -O2 -o init_aarch64 init.c
 *
 * Exit status is irrelevant to qemu: the host judges the console markers, so
 * a guest that dies before printing KMATRIX_LOAD_PASS is a failure.
 */
typedef unsigned long u64;
typedef long i64;

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_openat 257
#define SYS_reboot 169
#define SYS_mount 165
#define SYS_delete_module 176
#define SYS_init_module 175
#define SYS_finit_module 313
#define SYS_exit_group 231

#if defined(__aarch64__)
#undef SYS_read
#undef SYS_write
#undef SYS_close
#undef SYS_openat
#undef SYS_reboot
#undef SYS_mount
#undef SYS_delete_module
#undef SYS_init_module
#undef SYS_finit_module
#undef SYS_exit_group
#define SYS_read 63
#define SYS_write 64
#define SYS_close 57
#define SYS_openat 56
#define SYS_reboot 142
#define SYS_mount 40
#define SYS_delete_module 106
#define SYS_init_module 105
#define SYS_finit_module 273
#define SYS_exit_group 94
#endif

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CLOEXEC 02000000

#define LINUX_REBOOT_MAGIC1 0xfee1deadUL
#define LINUX_REBOOT_MAGIC2 672274793UL
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedcUL

#define KO_MAX (16UL * 1024 * 1024)

static i64 syscall6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
#if defined(__x86_64__)
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    register i64 r9 __asm__("r9") = f;
    i64 ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                       "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
#elif defined(__aarch64__)
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    register i64 x4 __asm__("x4") = e;
    register i64 x5 __asm__("x5") = f;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
#else
#error "unsupported architecture"
#endif
}

static i64 sys(i64 n, i64 a, i64 b, i64 c)
{
    return syscall6(n, a, b, c, 0, 0, 0);
}

static u64 slen(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

static void puts_fd(i64 fd, const char *s)
{
    sys(SYS_write, fd, (i64) s, (i64) slen(s));
}

static void put_dec(i64 fd, u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    if (!v)
        b[--i] = '0';
    while (v) {
        b[--i] = (char) ('0' + (v % 10));
        v /= 10;
    }
    puts_fd(fd, &b[i]);
}

static void fail(i64 fd, const char *what, i64 rc)
{
    puts_fd(fd, "KMATRIX_LOAD_FAIL ");
    puts_fd(fd, what);
    puts_fd(fd, " rc=");
    put_dec(fd, (u64) rc);
    puts_fd(fd, "\n");
}

/* Read a whole file into buf.  Returns bytes read, or -1. */
static i64 read_file(const char *path, char *buf, u64 cap)
{
    i64 fd = syscall6(SYS_openat, AT_FDCWD, (i64) path, O_RDONLY | O_CLOEXEC,
                      0, 0, 0);
    u64 off = 0;
    if (fd < 0)
        return -1;
    while (off < cap) {
        i64 n = sys(SYS_read, fd, (i64) (buf + off), (i64) (cap - off));
        if (n < 0) {
            sys(SYS_close, fd, 0, 0);
            return -1;
        }
        if (n == 0)
            break;
        off += (u64) n;
    }
    sys(SYS_close, fd, 0, 0);
    return (i64) off;
}

/* /proc/modules has one "name size refs ..." line per loaded module. */
static int modules_have(const char *buf, i64 len, const char *name)
{
    i64 i = 0;
    u64 n = slen(name);
    while (i < len) {
        i64 start = i;
        while (i < len && buf[i] != '\n')
            i++;
        if ((u64) (i - start) > n && buf[start + (i64) n] == ' ') {
            u64 j;
            for (j = 0; j < n; j++)
                if (buf[start + (i64) j] != name[j])
                    break;
            if (j == n)
                return 1;
        }
        i++;
    }
    return 0;
}

/*
 * insmod uses finit_module; keep init_module as the fallback so the diagnostic
 * names the system call that the kernel actually rejected.
 */
static i64 load_fd(const char *ko_path)
{
    i64 fd = syscall6(SYS_openat, AT_FDCWD, (i64) ko_path, O_RDONLY | O_CLOEXEC,
                      0, 0, 0);
    i64 rc;
    if (fd < 0)
        return fd;
    rc = syscall6(SYS_finit_module, fd, (i64) "", 0, 0, 0, 0);
    sys(SYS_close, fd, 0, 0);
    return rc;
}

static i64 load_buf(const char *ko, u64 len)
{
    return syscall6(SYS_init_module, (i64) ko, (i64) len, (i64) "", 0, 0, 0);
}

static void poweroff(void)
{
    syscall6(SYS_reboot, (i64) LINUX_REBOOT_MAGIC1, (i64) LINUX_REBOOT_MAGIC2,
             (i64) LINUX_REBOOT_CMD_POWER_OFF, 0, 0, 0);
    /* The reboot syscall does not return on success. */
    for (;;)
        sys(SYS_exit_group, 1, 0, 0);
}

static char ko[KO_MAX];
static char procbuf[256 * 1024];

void _start(void)
{
    i64 fd = syscall6(SYS_openat, AT_FDCWD, (i64) "/dev/console",
                      O_WRONLY | O_CLOEXEC, 0, 0, 0);
    i64 rc;
    i64 n;
    i64 modlen;

    if (fd < 0)
        fd = 1;

    rc = syscall6(SYS_mount, (i64) "proc", (i64) "/proc", (i64) "proc", 0, 0,
                  0);
    if (rc < 0) {
        fail(fd, "mount_proc", rc);
        poweroff();
    }

    n = read_file("/tbtun.ko", ko, KO_MAX);
    if (n <= 0) {
        fail(fd, "read_ko", n);
        poweroff();
    }
    puts_fd(fd, "KMATRIX module_bytes=");
    put_dec(fd, (u64) n);
    puts_fd(fd, "\n");

    rc = load_fd("/tbtun.ko");
    puts_fd(fd, "KMATRIX finit_module_1=");
    put_dec(fd, (u64) rc);
    puts_fd(fd, "\n");
    if (rc < 0) {
        rc = load_buf(ko, (u64) n);
        puts_fd(fd, "KMATRIX init_module_1=");
        put_dec(fd, (u64) rc);
        puts_fd(fd, "\n");
    }
    if (rc < 0) {
        fail(fd, "insmod_1", rc);
        poweroff();
    }
    puts_fd(fd, "KMATRIX insmod_1=0\n");

    modlen = read_file("/proc/modules", procbuf, sizeof(procbuf));
    if (modlen < 0 || !modules_have(procbuf, modlen, "tbtun")) {
        fail(fd, "proc_modules_present", modlen);
        poweroff();
    }
    puts_fd(fd, "KMATRIX proc_modules_has_tbtun=1\n");

    rc = syscall6(SYS_delete_module, (i64) "tbtun", 0, 0, 0, 0, 0);
    if (rc < 0) {
        fail(fd, "rmmod_1", rc);
        poweroff();
    }
    puts_fd(fd, "KMATRIX rmmod_1=0\n");

    modlen = read_file("/proc/modules", procbuf, sizeof(procbuf));
    if (modlen < 0 || modules_have(procbuf, modlen, "tbtun")) {
        fail(fd, "proc_modules_absent", modlen);
        poweroff();
    }
    puts_fd(fd, "KMATRIX proc_modules_absent=1\n");

    /* A second cycle catches a module whose exit path leaves poisoned state. */
    rc = load_buf(ko, (u64) n);
    if (rc < 0) {
        fail(fd, "insmod_2", rc);
        poweroff();
    }
    rc = syscall6(SYS_delete_module, (i64) "tbtun", 0, 0, 0, 0, 0);
    if (rc < 0) {
        fail(fd, "rmmod_2", rc);
        poweroff();
    }
    puts_fd(fd, "KMATRIX insmod_2=0 rmmod_2=0\n");
    puts_fd(fd, "KMATRIX_LOAD_PASS\n");
    poweroff();
}
