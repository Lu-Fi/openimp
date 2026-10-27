/* IMP_ISP_SetDefaultBinPath -> IMP_ISP_AddSensor (host-only, no camera; issue
 * 116): the stored path reaches the driver as TISP_VIDIOC_SET_DEFAULT_BIN_PATH
 * right after REGISTER_SENSOR, in the record the driver copies.  The T41
 * driver copies 132 bytes {vinum, path[128]} (tx_isp_set_default_bin_path,
 * T41 1.2.5 tx-isp-t41.ko); the T40 record is 68 bytes {vinum, path[64]}.
 * Built for PLATFORM_T40 and PLATFORM_T41. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define ioctl test_ioctl
#include "../../src/t40/openimp_p1.c"
#undef ioctl

#if defined(PLATFORM_T41)
#define DRIVER_RECORD 132U
#else
#define DRIVER_RECORD 68U
#endif

#define MAX_CALLS 16
static struct {
    unsigned long cmd[MAX_CALLS];
    int calls;
    unsigned char bin[DRIVER_RECORD];
    unsigned long fail_cmd;
} io;

int test_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    void *arg;

    va_start(args, command);
    arg = va_arg(args, void *);
    va_end(args);
    (void)fd;
    assert(io.calls < MAX_CALLS);
    io.cmd[io.calls++] = command;
    if (command == 0xc004542aUL)
        memcpy(io.bin, arg, DRIVER_RECORD);   /* what the driver copies */
    if (command == 0x800c5410UL)              /* GET_MDNS_BUF_INFO: no buffer */
        ((struct tisp_buf_info *)arg)->size = 0;
    if (io.fail_cmd && command == io.fail_cmd) {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

int openimp_t31_ivs_source_active(int fs_chn) { (void)fs_chn; return 0; }
void openimp_t31_ivs_capture(int fs_chn, const void *frame) { (void)fs_chn; (void)frame; }
/* link stubs for the rest of the P1 unit, never reached here */
int DMA_AllocDescriptor(IMPDMABufferInfo *i, int size, const char *tag)
{ (void)i; (void)size; (void)tag; abort(); }
int OpenIMP_P2_DMAState(uint32_t *a, uint32_t *b) { (void)a; (void)b; abort(); }
int OpenIMP_P2_DMARegion(uint32_t *a, uint32_t *b, void **c)
{ (void)a; (void)b; (void)c; abort(); }
int DMA_FreePhys(uint32_t p) { (void)p; abort(); }
#if defined(PLATFORM_T41)
void *IMP_MemPool_GetById(int id) { (void)id; return NULL; }
int DMA_PoolAllocDescriptor(int id, IMPDMABufferInfo *i, int size, const char *tag)
{ (void)id; (void)i; (void)size; (void)tag; abort(); }
#endif
int DMA_RmemFlushCache(void *a, uint32_t n, int d) { (void)a; (void)n; (void)d; abort(); }
OpenIMPProfileStamp openimp_profile_begin(void) { OpenIMPProfileStamp z; memset(&z, 0, sizeof z); return z; }
void openimp_profile_end(OpenIMPProfileStage st, OpenIMPProfileStamp t) { (void)st; (void)t; }
void openimp_profile_count(OpenIMPProfileCounter c, uint64_t n) { (void)c; (void)n; }
volatile uint32_t openimp_video_drop_frames;
int openimp_video_drop_set(void (*cb)(void)) { (void)cb; return 0; }
int64_t IMP_System_GetTimeStamp(void) { return 0; }
int64_t OpenIMP_P0_NormalizeMonotonicTimeStamp(uint64_t t) { return (int64_t)t; }

static int ok;
#define CHECK(c) do { assert(c); ok++; } while (0)

static void reopen(void)
{
    memset(&p1, 0, sizeof(p1));
    prepare_p1();
    p1.isp_open = 1;
    p1.isp_fd = 77;
    memset(&io, 0, sizeof(io));
}

int main(void)
{
    static const char bin[] = "/etc/sensor/sc5336-t41.bin";
    char long_path[sizeof(((struct tisp_bin_path *)0)->path) + 1];
    char got[64];
    IMPSensorInfo info;

    memset(&info, 0, sizeof(info));
    /* runtime checks: the build drops _Static_assert (32-bit ABI asserts in
     * openimp_p1.c do not hold on the 64-bit host) */
    CHECK(TISP_VIDIOC_SET_DEFAULT_BIN_PATH == 0xc004542aU);
    CHECK(sizeof(struct tisp_bin_path) == DRIVER_RECORD);
    CHECK(offsetof(struct tisp_bin_path, path) == 4);

    /* the issue 116 sequence: Open, Get (-1), Set, AddSensor */
    reopen();
    CHECK(OpenIMP_P1_GetDefaultBinPath(IMPVI_MAIN, got, sizeof(got)) == -1);
    CHECK(OpenIMP_P1_SetDefaultBinPath(IMPVI_MAIN, bin) == 0);
    CHECK(io.calls == 0);                       /* stored, sent with AddSensor */
    CHECK(IMP_ISP_AddSensor(IMPVI_MAIN, &info) == 0);
    CHECK(io.cmd[0] == 0x80645405UL);           /* REGISTER_SENSOR */
    CHECK(io.cmd[1] == 0xc004542aUL);           /* then the bin path */
    CHECK(io.cmd[2] == 0xc0085404UL);           /* then S_INPUT */
    CHECK(*(int32_t *)(void *)io.bin == IMPVI_MAIN);
    CHECK(strcmp((const char *)io.bin + 4, bin) == 0);
    CHECK(io.bin[DRIVER_RECORD - 1] == 0);      /* terminated inside the record */
    CHECK(OpenIMP_P1_GetDefaultBinPath(IMPVI_MAIN, got, sizeof(got)) == 0);
    CHECK(strcmp(got, bin) == 0);
    CHECK(OpenIMP_P1_SetDefaultBinPath(IMPVI_MAIN, bin) == -1); /* after AddSensor */

    /* without a path AddSensor does not send the ioctl */
    reopen();
    CHECK(IMP_ISP_AddSensor(IMPVI_MAIN, &info) == 0);
    CHECK(io.cmd[1] == 0xc0085404UL);

    /* the longest path the record holds, and one byte more */
    reopen();
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    long_path[sizeof(long_path) - 2] = 0;
    CHECK(OpenIMP_P1_SetDefaultBinPath(IMPVI_MAIN, long_path) == 0);
    CHECK(IMP_ISP_AddSensor(IMPVI_MAIN, &info) == 0);
    CHECK(strcmp((const char *)io.bin + 4, long_path) == 0);
    reopen();
    long_path[sizeof(long_path) - 2] = 'a';
    long_path[sizeof(long_path) - 1] = 0;
    CHECK(OpenIMP_P1_SetDefaultBinPath(IMPVI_MAIN, long_path) == -1);

    /* a driver that refuses the bin path fails AddSensor */
    reopen();
    CHECK(OpenIMP_P1_SetDefaultBinPath(IMPVI_MAIN, bin) == 0);
    io.fail_cmd = 0xc004542aUL;
    CHECK(IMP_ISP_AddSensor(IMPVI_MAIN, &info) == -1);
    CHECK(p1.sensor_added == 0);

    printf("bin path (%u-byte record): %d checks passed\n", DRIVER_RECORD, ok);
    return 0;
}
