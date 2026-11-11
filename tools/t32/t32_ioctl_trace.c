/*
 * LD_PRELOAD ioctl recorder for the T32 trace oracle (#287).
 *
 * One line per ioctl on a device node, in call order:
 *   I <seq> <tid> <path> <req> <rc> <errno>
 * and, with T32_TRACE_DUMP=<bytes>, the payload words before (W/RW) and
 * after the call:  D <seq> in|out <hex words>.
 * oi_trace_mark(text) (looked up by the sample with dlsym) writes
 *   M <seq> <tid> <text>
 * so the log carries the API step that issued each request.
 * With T32_TRACE_VPU=<bytes>, a /dev/soc_vpu CHANNEL_RUN / CHANNEL_START
 * also logs the job's register image (channel_node.dma_addr, a physical
 * address; read through /dev/mem before the call):  V <seq> <pa> <words>.
 * Output: $T32_TRACE_LOG (default /tmp/t32-ioctl.log).  Diagnostic tool,
 * never part of libimp.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static FILE *log_file;
static unsigned dump_bytes;
static unsigned vpu_bytes;
static unsigned long seq;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int safe_copy(void *dst, const void *src, unsigned n);
static int (*real_ioctl)(int, unsigned long, ...);
static int copy_pipe[2] = { -1, -1 };

static void trace_init(void)
{
    const char *path = getenv("T32_TRACE_LOG");
    const char *dump = getenv("T32_TRACE_DUMP");

    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    log_file = fopen(path ? path : "/tmp/t32-ioctl.log", "w");
    if (log_file)
        setvbuf(log_file, NULL, _IOLBF, 0);
    dump_bytes = dump ? (unsigned)strtoul(dump, NULL, 0) : 0;
    if (dump_bytes > 1024)
        dump_bytes = 1024;
    dump = getenv("T32_TRACE_VPU");
    vpu_bytes = dump ? (unsigned)strtoul(dump, NULL, 0) : 0;
    if (vpu_bytes > 16384)              /* HERA command lists: ~1200 pairs (#293) */
        vpu_bytes = 16384;
}

/* the soc_vpu job's register image: channel_node word 9 is its pa */
static void dump_vpu_job(unsigned long s, const void *arg)
{
    unsigned char node[64];
    static unsigned char buf[16384];      /* under the lock */
    unsigned pa, i, vpu_bytes_now;
    int fd;

    if ((unsigned long)arg < 0x1000 || safe_copy(node, arg, sizeof node))
        return;
    memcpy(&pa, node + 36, 4);
    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0)
        return;
    /* mmap: read() of /dev/mem stops at the end of kernel RAM (rmem is past it) */
    {
        /* never past the end of RAM (128 MiB in QEMU -m 128M): a HERA list at
         * the top of rmem is shorter than the dump size, and reading the
         * unassigned space behind it is a bus error (#293) */
        unsigned want = vpu_bytes;
        const char *ram = getenv("T32_TRACE_RAM_END");
        unsigned long end = ram ? strtoul(ram, NULL, 0) : 0x8000000UL;
        if (pa >= end)
            want = 0;
        else if (pa + want > end)
            want = (unsigned)(end - pa);
        vpu_bytes_now = want;
    }
    if (vpu_bytes_now) {
        unsigned off = pa & 4095u, vpu_bytes = vpu_bytes_now;
        void *m = mmap(NULL, off + vpu_bytes, PROT_READ, MAP_SHARED, fd, (off_t)(pa - off));
        if (m != MAP_FAILED) {
            memcpy(buf, (const unsigned char *)m + off, vpu_bytes);
            munmap(m, off + vpu_bytes);
            fprintf(log_file, "V %lu %08x", s, pa);
            for (i = 0; i + 4 <= vpu_bytes; i += 4) {
                unsigned v;
                memcpy(&v, buf + i, 4);
                fprintf(log_file, " %08x", v);
            }
            fputc('\n', log_file);
        }
    }
    close(fd);
}

/* copy through a pipe: a bad pointer gives EFAULT instead of a crash */
static int safe_copy(void *dst, const void *src, unsigned n)
{
    if (copy_pipe[0] < 0 && pipe(copy_pipe))
        return -1;
    if (write(copy_pipe[1], src, n) != (ssize_t)n)
        return -1;
    return read(copy_pipe[0], dst, n) == (ssize_t)n ? 0 : -1;
}

static void dump_words(unsigned long s, const char *tag, const void *arg, unsigned n)
{
    unsigned char buf[1024];
    unsigned i;

    if (!n || (unsigned long)arg < 0x1000 || safe_copy(buf, arg, n))
        return;
    fprintf(log_file, "D %lu %s", s, tag);
    for (i = 0; i + 4 <= n; i += 4) {
        unsigned v;
        memcpy(&v, buf + i, 4);
        fprintf(log_file, " %08x", v);
    }
    fputc('\n', log_file);
}

void oi_trace_mark(const char *text)
{
    pthread_once(&once, trace_init);
    pthread_mutex_lock(&lock);
    if (log_file)
        fprintf(log_file, "M %lu %ld %s\n", ++seq, (long)syscall(SYS_gettid), text);
    pthread_mutex_unlock(&lock);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    char link[32], path[64];
    ssize_t len;
    unsigned long s = 0;
    unsigned dir = (req >> 29) & 7, size = (req >> 16) & 0x1fff, n;
    int rc, err, traced;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    pthread_once(&once, trace_init);
    if (!real_ioctl) {
        errno = ENOSYS;
        return -1;
    }
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    len = readlink(link, path, sizeof path - 1);
    path[len > 0 ? len : 0] = 0;
    /* device nodes only; the vendor log device is noise */
    traced = log_file && !strncmp(path, "/dev/", 5) && strcmp(path, "/dev/log_main");
    n = size;
    if (req == 0x80045401UL)            /* DRIVER_VERSION: 64-byte buffer */
        n = 64;
    else if (req == 0xc0045402UL)       /* ENUMINPUT: 36-byte buffer */
        n = 36;
    else if (req == 0xc00454c9UL)       /* DEFAULT_BIN: 880-byte buffer */
        n = 880;
    else if (req == 0xc00454c8UL)       /* SET_TIZIANO_PAR: 20-byte switch-bin attr */
        n = 20;
    else if (req == 0x2000496aUL)       /* jz_ipu_v13 IOCTL_IPU_START (_IO): T32 200-byte ipu_param (#297) */
        n = 200;
    else if (req == 0x20004976UL)       /* IOCTL_IPU_BUF_FLUSH_CACHE (_IO): { addr, size } */
        n = 8;
    if (n > dump_bytes)
        n = dump_bytes;
    if (traced) {
        pthread_mutex_lock(&lock);
        s = ++seq;
        if ((dir & 4) || req == 0x2000496aUL || req == 0x20004976UL)
            dump_words(s, "in", arg, n);
        if (vpu_bytes && (req == 0xc0406302UL || req == 0xc0406303UL))
            dump_vpu_job(s, arg);
        pthread_mutex_unlock(&lock);
    }
    rc = real_ioctl(fd, req, arg);
    err = errno;
    if (traced) {
        pthread_mutex_lock(&lock);
        fprintf(log_file, "I %lu %ld %s %08lx %d %d\n", s, (long)syscall(SYS_gettid),
                path, req, rc, rc < 0 ? err : 0);
        dump_words(s, "out", arg, n);    /* _IOW requests are written back too */
        pthread_mutex_unlock(&lock);
    }
    errno = err;
    return rc;
}
