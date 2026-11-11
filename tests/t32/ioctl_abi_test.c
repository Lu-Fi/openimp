/*
 * T32 ioctl ABI table host test (#287).
 *
 * Always: the header's compile-time asserts (request literal == encoding of
 * dir/type/nr/sizeof(payload), IMPSensorInfo == REGISTER_SENSOR payload),
 * requests unique, flags consistent.  "--table" prints the table.
 * With -DOI_T32_DRV_CROSSCHECK and the open-tx-isp driver/t32 directory on
 * the include path: every request of tx_isp_t32_abi.h equals ours, its
 * payload structs have our sizes, and our OI_T32_F_DRV rows are exactly the
 * driver's requests.
 */
#include <stdio.h>
#include <string.h>

#include "t32/openimp_t32_ioctl.h"

#ifdef OI_T32_DRV_CROSSCHECK
#include "tx_isp_t32_abi.h"

#define DRV_PAIRS(X) \
    X(T32_IOC_DRIVER_VERSION, OI_T32_IOC_DRIVER_VERSION) \
    X(T32_IOC_ENUMINPUT, OI_T32_IOC_ENUMINPUT) \
    X(T32_IOC_G_INPUT, OI_T32_IOC_G_INPUT) \
    X(T32_IOC_S_INPUT, OI_T32_IOC_S_INPUT) \
    X(T32_IOC_REGISTER_SENSOR, OI_T32_IOC_REGISTER_SENSOR) \
    X(T32_IOC_RELEASE_SENSOR, OI_T32_IOC_RELEASE_SENSOR) \
    X(T32_IOC_STREAMON, OI_T32_IOC_STREAMON) \
    X(T32_IOC_STREAMOFF, OI_T32_IOC_STREAMOFF) \
    X(T32_IOC_CREATE_SUBDEV_LINKS, OI_T32_IOC_CREATE_SUBDEV_LINKS) \
    X(T32_IOC_DESTROY_SUBDEV_LINKS, OI_T32_IOC_DESTROY_SUBDEV_LINKS) \
    X(T32_IOC_LINKS_STREAMON, OI_T32_IOC_LINKS_STREAMON) \
    X(T32_IOC_LINKS_STREAMOFF, OI_T32_IOC_LINKS_STREAMOFF) \
    X(T32_IOC_SET_SENSOR_REGISTER, OI_T32_IOC_SET_SENSOR_REGISTER) \
    X(T32_IOC_GET_SENSOR_REGISTER, OI_T32_IOC_GET_SENSOR_REGISTER) \
    X(T32_IOC_SET_ISP_MEM_INFO, OI_T32_IOC_SET_ISP_MEM_INFO) \
    X(T32_IOC_GET_ISP_MEM_INFO, OI_T32_IOC_GET_ISP_MEM_INFO) \
    X(T32_IOC_SET_ISP_IPC_MEM_INFO, OI_T32_IOC_SET_ISP_IPC_MEM_INFO) \
    X(T32_IOC_GET_ISP_IPC_MEM_INFO, OI_T32_IOC_GET_ISP_IPC_MEM_INFO) \
    X(T32_IOC_MEMORY_MALLOC, OI_T32_IOC_MEMORY_MALLOC) \
    X(T32_IOC_MEMORY_FREE, OI_T32_IOC_MEMORY_FREE) \
    X(T32_IOC_SET_TIZIANO_PAR, OI_T32_IOC_SET_TIZIANO_PAR) \
    X(T32_IOC_DEFAULT_BIN, OI_T32_IOC_DEFAULT_BIN) \
    X(T32_IOC_TUNING_DEFAULT, OI_T32_IOC_DEFAULT_CMD_ISP_TUNING) \
    X(T32_IOC_IVDC_GET_DIRECT, OI_T32_IOC_GET_DIRECT) \
    X(T32_IOC_IVDC_SET_DIRECT, OI_T32_IOC_SET_DIRECT) \
    X(T32_IOC_FS_SET_FORMAT, OI_T32_IOC_SET_FRAME_FORMAT) \
    X(T32_IOC_FS_GET_FORMAT, OI_T32_IOC_GET_FRAME_FORMAT) \
    X(T32_IOC_FS_REQBUFS, OI_T32_IOC_REQBUFS) \
    X(T32_IOC_FS_QUERYBUF, OI_T32_IOC_QUERYBUF) \
    X(T32_IOC_FS_QBUF, OI_T32_IOC_QBUF) \
    X(T32_IOC_FS_DQBUF, OI_T32_IOC_DQBUF) \
    X(T32_IOC_FS_STREAMON, OI_T32_IOC_FRAME_STREAMON) \
    X(T32_IOC_FS_STREAMOFF, OI_T32_IOC_FRAME_STREAMOFF) \
    X(T32_IOC_FS_SET_BANKS, OI_T32_IOC_SET_BANKS) \
    X(T32_IOC_FS_LISTEN_BUF, OI_T32_IOC_LISTEN_BUF)

#define DRV_ASSERT(drv, oi) _Static_assert((drv) == (oi), #drv " != " #oi);
DRV_PAIRS(DRV_ASSERT)
_Static_assert(sizeof(struct t32_initarg) == sizeof(T32IocInitArg), "drv initarg");
_Static_assert(sizeof(struct t32_buf_info) == sizeof(T32IocBufInfo), "drv buf_info");
_Static_assert(sizeof(struct t32_sensor_reginfo) == sizeof(T32IocSensorReg), "drv sensor reginfo");
#endif

struct row {
    int dev;
    const char *name;
    unsigned req, size, payload, flags;
};

#define ROW(dev, req, dir, type, nr, size, payload, flags) \
    { OI_T32_DEV_##dev, #req, req, size, payload, flags },
static const struct row rows[] = { OPENIMP_T32_IOCTLS(ROW) };
#define NROWS (sizeof rows / sizeof rows[0])

int main(int argc, char **argv)
{
    static const char *const dev[] = OI_T32_DEV_PATHS;
    unsigned i, j, seen = 0, drv = 0;
    int fail = 0;

    for (i = 0; i < NROWS; i++) {
        for (j = i + 1; j < NROWS; j++)
            if (rows[i].req == rows[j].req) {
                printf("duplicate request %08x: %s %s\n", rows[i].req, rows[i].name, rows[j].name);
                fail = 1;
            }
        if (OI_T32_IOC_SIZE(rows[i].req) != rows[i].size) {
            printf("size field mismatch: %s\n", rows[i].name);
            fail = 1;
        }
        if ((rows[i].flags & OI_T32_F_LATER) && (rows[i].flags & OI_T32_F_SEEN)) {
            printf("LATER row seen in the reference: %s\n", rows[i].name);
            fail = 1;
        }
        seen += !!(rows[i].flags & OI_T32_F_SEEN);
        drv += !!(rows[i].flags & OI_T32_F_DRV);
    }
#ifdef OI_T32_DRV_CROSSCHECK
    {
#define DRV_REQ(drv_req, oi) drv_req,
        static const unsigned drv_reqs[] = { DRV_PAIRS(DRV_REQ) };
        unsigned n = sizeof drv_reqs / sizeof drv_reqs[0];
        if (n != drv) {
            printf("driver serves %u requests, table flags %u OI_T32_F_DRV\n", n, drv);
            fail = 1;
        }
        for (i = 0; i < NROWS; i++) {
            int in_drv = 0;
            for (j = 0; j < n; j++)
                in_drv |= drv_reqs[j] == rows[i].req;
            if (in_drv != !!(rows[i].flags & OI_T32_F_DRV)) {
                printf("OI_T32_F_DRV wrong for %s\n", rows[i].name);
                fail = 1;
            }
        }
        printf("driver cross-check: %u requests, payload structs equal\n", n);
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "--table"))
        for (i = 0; i < NROWS; i++)
            printf("%-15s %08x nr=%-3u size=%-3u payload=%-3u %c%c%c %s\n", dev[rows[i].dev],
                   rows[i].req, OI_T32_IOC_NR(rows[i].req), rows[i].size, rows[i].payload,
                   rows[i].flags & OI_T32_F_SEEN ? 'S' : '-', rows[i].flags & OI_T32_F_DRV ? 'D' : '-',
                   rows[i].flags & OI_T32_F_LATER ? 'L' : '-', rows[i].name);
    printf("t32 ioctl table: %u requests, %u seen in the vendor trace, %u served by driver/t32: %s\n",
           (unsigned)NROWS, seen, drv, fail ? "FAILED" : "ok");
    return fail;
}
