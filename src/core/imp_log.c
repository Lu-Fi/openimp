/*
 * imp_log.c -- built-in implementation of the three logging symbols the OEM
 * libimp imports from libalog.so / libsysutils.so:
 *
 *   int  IMP_Log_Get_Option(void);
 *   void IMP_Log_Set_Option(int option);
 *   int  imp_log_fun(int level, int option, int type, const char *tag,
 *                    const char *file, int line, const char *func,
 *                    const char *fmt, ...);
 *
 * OpenIMP's own code calls these (OEM call shape), so exporting them from
 * libimp.so removes the need for any vendor libalog.so / libsysutils.so.
 *
 * libalog.so (T31 1.1.6, uclibc/5.4.0) keeps the live option in a 4-byte
 * .bss word (`log_option`, 0x16750, initial 0) and the live level in a
 * .data word (`log_level`, 0x16584, initial 3 = IMP_LOG_LEVEL_DEBUG).
 * IMP_Log_Get_Option @0x2af0 is `return log_option;` and IMP_Log_Set_Option
 * @0x2b08 is `log_option = option;` -- nothing more.  The option is the
 * field mask IMP_LOG_OP_* from imp_log_fun.h, not a sink selector; the sink
 * is the third argument of imp_log_fun() (IMP_LOG_OUT_*).  libalog renders
 * a record only when `level >= log_level`, then prints the fields the mask
 * selects (it emits `[%s]:` for FILE, `[%d]` for LINE, `[%s]` for FUNC).
 *
 * Levels (OEM): 3 DBG, 4 INFO, 5 WARN, 6 ERR.  OpenIMP deviates on purpose:
 * WARN/ERR always go to stderr and DBG/INFO only when OPENIMP_DEBUG_TRACE is
 * set, so the library stays quiet on a camera unless asked.  OpenIMP also
 * has no logcat backend, so both sinks end up on stderr.  Syslog is off by
 * default and enabled with OPENIMP_LOG_SYSLOG=1.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

#include <stdlib.h>

#include "../imp_log_fun.h"
#include "../trace_control.h"

/* Live printing options (IMP_LOG_OP_* in imp_log_fun.h).  libalog starts
 * this word at IMP_LOG_OP_NONE; OpenIMP defaults to the full mask so its
 * own OEM-shaped call sites keep printing `[tag] file:line func: text`.
 * IMP_Log_Set_Option(IMP_LOG_OP_NONE) gives the terse OEM line. */
static int imp_log_option = IMP_LOG_OP_DEFAULT;

/* Append a printf fragment to the log line and return the new used length,
 * clamped to `size` so the caller can fall back to the bare message. */
static size_t imp_log_add(char *buf, size_t size, size_t used,
                          const char *fmt, ...)
{
    va_list ap;
    int n;

    if (used + 1 >= size)
        return size;
    va_start(ap, fmt);
    n = vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
    if (n < 0)
        return size;
    used += (size_t)n;
    return used >= size ? size : used;
}

static int imp_log_syslog_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char *e = getenv("OPENIMP_LOG_SYSLOG");

        cached = e && e[0] == '1';
    }
    return cached;
}

int IMP_Log_Get_Option(void)
{
    return imp_log_option;
}

void IMP_Log_Set_Option(int option)
{
    imp_log_option = option;
}

/* Sink 2: the vendor imp_log_fun calls __android_log_write(prio = level, tag,
 * text) of the bundled Android liblog (libalog.so, logd_write.c), which writes
 * one record to the logger device "/dev/log_main" (T40 1.3.1 libalog string):
 * writev {priority byte, tag + NUL, message + NUL}.  OpenIMP does the same;
 * OPENIMP_LOGGER_DEV overrides the device (host tests).  Returns 0 on success,
 * -1 when no logger device can be opened (caller falls back to stderr). */
static int imp_log_logcat(int level, const char *tag, const char *msg)
{
    static int fd = -2;
    struct iovec v[3];
    unsigned char prio = (unsigned char)level;

    if (fd == -2) {
        const char *dev = getenv("OPENIMP_LOGGER_DEV");

        fd = open(dev && dev[0] ? dev : "/dev/log_main", O_WRONLY | O_CLOEXEC);
        if (fd < 0 && !(dev && dev[0]))
            fd = open("/dev/log/main", O_WRONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return -1;
    v[0].iov_base = &prio;           v[0].iov_len = 1;
    v[1].iov_base = (void *)tag;     v[1].iov_len = strlen(tag) + 1;
    v[2].iov_base = (void *)msg;     v[2].iov_len = strlen(msg) + 1;
    return writev(fd, v, 3) < 0 ? -1 : 0;
}

int imp_log_fun(int level, int option, int type, ...)
{
    va_list ap;
    const char *tag, *file, *func, *fmt, *base;
    int line, prio, n;
    size_t len;
    char buf[512];
    static int inited;

    if (level < 5 && !openimp_debug_trace_enabled())
        return 0;

    va_start(ap, type);
    tag = va_arg(ap, const char *);
    file = va_arg(ap, const char *);
    line = va_arg(ap, int);
    func = va_arg(ap, const char *);
    fmt = va_arg(ap, const char *);
    if (!fmt) {
        va_end(ap);
        return 0;
    }
    base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : (file ? file : "?");
    /* OEM semantics: the option selects which fields get rendered, the
     * message itself always does.  The tag stays unconditional -- it is the
     * only identifier left once every field bit is clear. */
    len = 0;
    len = imp_log_add(buf, sizeof(buf), len, "[%s]", tag ? tag : "IMP");
    if (option & IMP_LOG_OP_FILE)
        len = imp_log_add(buf, sizeof(buf), len, " %s", base);
    if (option & IMP_LOG_OP_LINE)
        len = imp_log_add(buf, sizeof(buf), len, ":%d", line);
    if (option & IMP_LOG_OP_FUNC)
        len = imp_log_add(buf, sizeof(buf), len, " %s", func ? func : "?");
    len = imp_log_add(buf, sizeof(buf), len, ": ");
    if (len >= sizeof(buf))
        len = 0;    /* prefix did not fit: log the bare message */
    n = (int)len;
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    /* The sink is `type` in the OEM ABI (libalog imp_log_fun, T40 1.3.1):
     * 0 IMP_LOG_OUT_STDOUT     -> imp_log_print_buf: fputs to the process output,
     * 1 IMP_LOG_OUT_LOCAL_FILE -> imp_log_buf: fopen("timp.log", "a+") once,
     *                             fputs (OpenIMP: $OPENIMP_LOG_FILE overrides the
     *                             name; open failure falls back to stderr),
     * 2 IMP_LOG_OUT_SERVER     -> __android_log_write(level, tag, text): logger
     *                             device record, stderr when there is no device;
     *                             stdout is the libalog `stdout` (GOT symbol). */
    len = strlen(buf);
    {
        static FILE *lf;
        FILE *out = stderr;

        if (type == 2) {
            const char *m = buf + (tag ? strlen(tag) : 3) + 2;   /* skip "[tag]" */
            char msg[512];
            size_t ml;

            if (*m == ' ')
                m++;
            snprintf(msg, sizeof(msg), "%s", m);
            ml = strlen(msg);
            while (ml && msg[ml - 1] == '\n')
                msg[--ml] = 0;
            if (imp_log_logcat(level, tag ? tag : "IMP", msg) == 0)
                out = NULL;
        } else if (type == 0) {
            out = stdout;
        } else if (type == 1) {
            if (!lf) {
                const char *fn = getenv("OPENIMP_LOG_FILE");

                lf = fopen(fn && fn[0] ? fn : "timp.log", "a+");
            }
            if (lf)
                out = lf;
        }
        if (out) {
            fputs(buf, out);
            if (!len || buf[len - 1] != '\n')
                fputc('\n', out);
            if (out == lf)
                fflush(out);
        }
    }
    if (imp_log_syslog_enabled()) {
        if (!inited) {
            openlog("libimp", LOG_PID | LOG_NDELAY, LOG_USER);
            inited = 1;
        }
        prio = level >= 6 ? LOG_ERR : level == 5 ? LOG_WARNING
             : level == 4 ? LOG_INFO : LOG_DEBUG;
        syslog(prio, "%s", buf);
    }
    return 0;
}
