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
 * Output: $T32_TRACE_LOG (default /tmp/t32-ioctl.log).  Diagnostic tool,
 * never part of libimp.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static FILE *log_file;
static unsigned dump_bytes;
static unsigned long seq;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
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
    if (n > dump_bytes)
        n = dump_bytes;
    if (traced) {
        pthread_mutex_lock(&lock);
        s = ++seq;
        if (dir & 4)
            dump_words(s, "in", arg, n);
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
