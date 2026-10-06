/* Host test: imp_log_fun/IMP_Log_Get_Option/IMP_Log_Set_Option. Build: see
 * Makefile-free one-liner
 *   cc -I src tests/core/imp_log_test.c src/core/imp_log.c -o /tmp/imp_log_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "imp_log_fun.h"

/* Run one imp_log_fun call with stderr captured, return the bytes seen. */
static size_t capture(char *out, size_t size, int option, int level)
{
    int fd[2], save = dup(2);
    ssize_t n;

    out[0] = 0;
    if (pipe(fd) || save < 0) return 0;
    dup2(fd[1], 2);
    imp_log_fun(level, option, IMP_LOG_OUT_SERVER, "Device", "/a/b/device.c",
                0x1d, "alloc_device", "bad %d %s\n", 7, "x");
    fflush(stderr);
    dup2(save, 2);
    close(fd[1]);
    n = read(fd[0], out, size - 1);
    close(fd[0]);
    if (n <= 0) {
        out[0] = 0;
        return 0;
    }
    out[n] = 0;
    return (size_t)n;
}

int main(void)
{
    char out[256];

    /* the level gate must not depend on the ambient environment */
    unsetenv("OPENIMP_DEBUG_TRACE");

    /* default option (IMP_LOG_OP_DEFAULT): full source location, ERR prints */
    capture(out, sizeof(out), IMP_Log_Get_Option(), 6);
    if (!strstr(out, "[Device] device.c:29 alloc_device: bad 7 x\n")) {
        printf("FAIL default prefix: %s", out);
        return 1;
    }
    /* DBG stays hidden while OPENIMP_DEBUG_TRACE is unset */
    capture(out, sizeof(out), IMP_Log_Get_Option(), 3);
    if (strstr(out, "bad 7 x")) {
        printf("FAIL level gate: %s", out);
        return 1;
    }
    /* the option is a field mask, not a sink selector */
    if (IMP_Log_Get_Option() != IMP_LOG_OP_DEFAULT) return 1;
    IMP_Log_Set_Option(IMP_LOG_OP_NONE);
    if (IMP_Log_Get_Option() != IMP_LOG_OP_NONE) return 1;
    capture(out, sizeof(out), IMP_Log_Get_Option(), 6);
    if (!strstr(out, "bad 7 x\n") || strstr(out, "alloc_device")) {
        printf("FAIL OP_NONE: %s", out);
        return 1;
    }
    IMP_Log_Set_Option(IMP_LOG_OP_FILE | IMP_LOG_OP_LINE);
    capture(out, sizeof(out), IMP_Log_Get_Option(), 6);
    if (!strstr(out, "device.c:29") || strstr(out, "alloc_device")) {
        printf("FAIL OP_FILE|OP_LINE: %s", out);
        return 1;
    }
    IMP_Log_Set_Option(IMP_LOG_OP_DEFAULT);
    puts("imp_log_test OK");
    return 0;
}
