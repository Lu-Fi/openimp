/*
 * OpenIMP T32 ISP open / sensor lifecycle (#288, planned; not a release gate).
 *
 * PRJ007 on the vendor 2.x kernel ABI (open-tx-isp driver/t32, requests in
 * openimp_t32_ioctl.h).  The flow follows what vendor libimp 2.2.0 issues in
 * QEMU -M ingenic-t32 (reference tests/t32/vendor-2.2.0-ioctl-seq-isp.txt,
 * recorded with tools/t32/qemu-ioctl-trace.sh isp); no vendor code copied.
 *
 *   IMP_ISP_Open        open /dev/tx-isp, DRIVER_VERSION handshake (SoC and
 *                       driver version must be the ones this library speaks)
 *   IMP_ISP_AddSensor   sensor_id = VI number, REGISTER_SENSOR (116 B), find
 *                       the input by name (ENUMINPUT until -EINVAL), S_INPUT
 *                       (driver answers w << 16 | h), IPC memory (none on
 *                       PRJ007: SET with size 0), ISP working memory from the
 *                       reserved arena (GET size, allocate, SET pa)
 *   IMP_ISP_EnableSensor  G_INPUT, STREAMON, CREATE_SUBDEV_LINKS, LINKS_STREAMON
 *   IMP_ISP_DisableSensor G_INPUT, LINKS_STREAMOFF, DESTROY_SUBDEV_LINKS, STREAMOFF
 *   IMP_ISP_DelSensor   S_INPUT off, RELEASE_SENSOR, free the ISP memory
 *   IMP_System_Init     IVDC direct-mode query (system core and FrameSource
 *                       module each ask; 0 = not in direct mode)
 *
 * Not here (later T32 issues): the userspace IQ-bin loader (DEFAULT_BIN,
 * MEMORY_MALLOC/FREE, SET_TIZIANO_PAR between REGISTER_SENSOR and ENUMINPUT,
 * #289), tuning on /dev/isp-m0, FrameSource, bind.
 *
 * The ISP memory comes from the shared reserved-arena allocator
 * (src/dma_alloc.c, as on T21/T23/T30/T31).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_isp.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "t32/openimp_t32_ioctl.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_isp.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ISP"

/* what this library speaks; the driver answers DRIVER_VERSION with these */
#define T32_DRIVER_SOC     "PRJ007"
#define T32_DRIVER_VERSION "20251107a"

#define T32_MAX_INPUTS 4        /* ENUMINPUT walk bound (driver: 1-2 VIs) */

static struct {
    pthread_mutex_t lock;
    int isp_fd;
    int open;
    int system_init;
    int sensor_added;
    int sensor_enabled;
    IMPVI_NUM vinum;
    int input_index;
    uint32_t width, height;
    T32IocBufInfo isp_mem;      /* pa/size handed to SET_ISP_MEM_INFO */
    T32IocBufInfo ipc_mem;
    IMPSensorInfo sensor;
} t32 = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .isp_fd = -1,
    .input_index = -1,
};

static int t32_ioctl(int fd, uint32_t request, void *arg, const char *name)
{
    int rc = ioctl(fd, request, arg);

    if (rc < 0)
        IMP_LOG_ERR(TAG, "%s (0x%08x) failed: %s", name, request, strerror(errno));
    return rc;
}

#define T32_IOCTL(fd, req, arg) t32_ioctl((fd), OI_T32_IOC_##req, (arg), #req)

/* ---- helpers for the later T32 modules (openimp_t32_isp.h) ------------- */
int OpenIMP_T32_IspFd(void)
{
    return t32.open ? t32.isp_fd : -1;
}

int OpenIMP_T32_SensorSize(uint32_t *width, uint32_t *height)
{
    int rc = -1;

    pthread_mutex_lock(&t32.lock);
    if (t32.sensor_added && width && height) {
        *width = t32.width;
        *height = t32.height;
        rc = 0;
    }
    pthread_mutex_unlock(&t32.lock);
    return rc;
}

int OpenIMP_T32_IvdcDirect(void)
{
    int32_t direct = 0;
    int fd = open("/dev/misc-ivdc", O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return 0;               /* no IVDC node: never direct */
    if (T32_IOCTL(fd, GET_DIRECT, &direct) < 0)
        direct = 0;
    close(fd);
    return direct != 0;
}

/* ---- System ------------------------------------------------------------ */
int IMP_System_Init(void)
{
    pthread_mutex_lock(&t32.lock);
    if (!t32.system_init) {
        /* system core and FrameSource module both learn whether the ISP
         * feeds the encoder directly (IVDC); PRJ007 driver: never */
        if (OpenIMP_T32_IvdcDirect() || OpenIMP_T32_IvdcDirect())
            IMP_LOG_WARN(TAG, "IVDC direct mode reported; not supported yet, using memory mode");
        t32.system_init = 1;
    }
    pthread_mutex_unlock(&t32.lock);
    return 0;
}

int IMP_System_Exit(void)
{
    pthread_mutex_lock(&t32.lock);
    t32.system_init = 0;
    pthread_mutex_unlock(&t32.lock);
    return 0;
}

/* ---- ISP --------------------------------------------------------------- */
static int t32_version_ok(const T32IocDriverVersion *v)
{
    if (strncmp(v->soc, T32_DRIVER_SOC, sizeof(v->soc)) ||
        strncmp(v->version, T32_DRIVER_VERSION, sizeof(v->version))) {
        IMP_LOG_ERR(TAG, "ISP driver %.32s:%.32s does not match this library (%s:%s)",
                    v->soc, v->version, T32_DRIVER_SOC, T32_DRIVER_VERSION);
        return 0;
    }
    return 1;
}

int IMP_ISP_Open(void)
{
    T32IocDriverVersion ver;
    int fd;

    pthread_mutex_lock(&t32.lock);
    if (t32.open) {
        pthread_mutex_unlock(&t32.lock);
        return 0;
    }
    fd = open("/dev/tx-isp", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        IMP_LOG_ERR(TAG, "open /dev/tx-isp: %s", strerror(errno));
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    memset(&ver, 0, sizeof(ver));
    if (T32_IOCTL(fd, DRIVER_VERSION, &ver) < 0 || !t32_version_ok(&ver)) {
        close(fd);
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    t32.isp_fd = fd;
    t32.open = 1;
    pthread_mutex_unlock(&t32.lock);
    return 0;
}

int IMP_ISP_Close(void)
{
    pthread_mutex_lock(&t32.lock);
    if (t32.sensor_added || t32.sensor_enabled) {
        IMP_LOG_ERR(TAG, "IMP_ISP_Close: sensor still added");
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    if (t32.open) {
        close(t32.isp_fd);
        t32.isp_fd = -1;
        t32.open = 0;
    }
    pthread_mutex_unlock(&t32.lock);
    return 0;
}

/* walk every input (the driver ends the list with -EINVAL) and keep the one
 * whose name is the sensor's */
static int t32_find_input(const char *name)
{
    T32IocEnumInput in;
    int index, found = -1;

    for (index = 0; index < T32_MAX_INPUTS; index++) {
        memset(&in, 0, sizeof(in));
        in.index = index;
        if (ioctl(t32.isp_fd, OI_T32_IOC_ENUMINPUT, &in) < 0)
            break;
        if (found < 0 && !strncmp(in.name, name, sizeof(in.name)))
            found = index;
    }
    if (found < 0)
        IMP_LOG_ERR(TAG, "sensor %.32s is not among the %d ISP inputs", name, index);
    return found;
}

static void t32_free_mem(T32IocBufInfo *m)
{
    if (m->paddr)
        DMA_FreePhys(m->paddr);
    memset(m, 0, sizeof(*m));
}

/* GET the size the driver wants, allocate it from the reserved arena, SET
 * {vinum, pa, size}; size 0 = none (still SET, as the vendor library does) */
static int t32_setup_mem(uint32_t get_req, uint32_t set_req, const char *what,
                         T32IocBufInfo *out)
{
    T32IocBufInfo m;
    IMPDMABufferInfo dma;

    memset(&m, 0, sizeof(m));
    m.word0 = (uint32_t)t32.vinum;
    if (t32_ioctl(t32.isp_fd, get_req, &m, what) < 0)
        return -1;
    m.word0 = (uint32_t)t32.vinum;
    m.paddr = 0;
    if (m.size) {
        if (DMA_AllocDescriptor(&dma, (int)m.size, what) < 0) {
            IMP_LOG_ERR(TAG, "%s: cannot allocate %u bytes", what, m.size);
            return -1;
        }
        m.paddr = dma.phys_addr;
    }
    if (t32_ioctl(t32.isp_fd, set_req, &m, what) < 0) {
        t32_free_mem(&m);
        return -1;
    }
    *out = m;
    return 0;
}

int IMP_ISP_AddSensor(IMPVI_NUM num, IMPSensorInfo *info)
{
    T32IocInitArg in;
    int index;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT || !info)
        return -1;
    pthread_mutex_lock(&t32.lock);
    if (!t32.open || t32.sensor_added) {
        IMP_LOG_ERR(TAG, "IMP_ISP_AddSensor: ISP %s", t32.open ? "already has a sensor" : "not open");
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    /* the VI number travels in sensor_id of the caller's object (vendor 2.x) */
    info->sensor_id = (unsigned short)num;
    if (T32_IOCTL(t32.isp_fd, REGISTER_SENSOR, info) < 0)
        goto fail;
    t32.vinum = num;
    /* #289: IQ bin (DEFAULT_BIN, MEMORY_MALLOC, SET_TIZIANO_PAR) goes here */
    index = t32_find_input(info->name);
    if (index < 0)
        goto fail_release;
    in.enable = 1;
    in.vinum = index;
    if (T32_IOCTL(t32.isp_fd, S_INPUT, &in) < 0)
        goto fail_release;
    t32.width = (uint32_t)in.vinum >> 16;
    t32.height = (uint32_t)in.vinum & 0xffffu;
    t32.input_index = index;
    if (t32_setup_mem(OI_T32_IOC_GET_ISP_IPC_MEM_INFO, OI_T32_IOC_SET_ISP_IPC_MEM_INFO,
                      "isp-ipc", &t32.ipc_mem) < 0)
        goto fail_input;
    if (t32_setup_mem(OI_T32_IOC_GET_ISP_MEM_INFO, OI_T32_IOC_SET_ISP_MEM_INFO,
                      "isp-mem", &t32.isp_mem) < 0)
        goto fail_ipc;
    t32.sensor = *info;
    t32.sensor_added = 1;
    pthread_mutex_unlock(&t32.lock);
    return 0;

fail_ipc:
    t32_free_mem(&t32.ipc_mem);
fail_input:
    in.enable = 0;
    in.vinum = 0;
    T32_IOCTL(t32.isp_fd, S_INPUT, &in);
fail_release:
    T32_IOCTL(t32.isp_fd, RELEASE_SENSOR, info);
fail:
    t32.width = t32.height = 0;
    t32.input_index = -1;
    pthread_mutex_unlock(&t32.lock);
    return -1;
}

int IMP_ISP_DelSensor(IMPVI_NUM num, IMPSensorInfo *info)
{
    T32IocInitArg in;
    int rc;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT || !info)
        return -1;
    pthread_mutex_lock(&t32.lock);
    if (!t32.sensor_added || t32.sensor_enabled || num != t32.vinum) {
        IMP_LOG_ERR(TAG, "IMP_ISP_DelSensor: sensor %s", t32.sensor_enabled ? "still enabled" : "not added");
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    in.enable = 0;
    in.vinum = 0;
    rc = T32_IOCTL(t32.isp_fd, S_INPUT, &in);
    if (rc == 0)
        rc = T32_IOCTL(t32.isp_fd, RELEASE_SENSOR, info);
    if (rc == 0) {
        /* the driver dropped its pa with the sensor; the memory is ours again */
        t32_free_mem(&t32.isp_mem);
        t32_free_mem(&t32.ipc_mem);
        memset(&t32.sensor, 0, sizeof(t32.sensor));
        t32.width = t32.height = 0;
        t32.input_index = -1;
        t32.sensor_added = 0;
    }
    pthread_mutex_unlock(&t32.lock);
    return rc < 0 ? -1 : 0;
}

int IMP_ISP_EnableSensor(IMPVI_NUM num, IMPSensorInfo *info)
{
    T32IocInitArg in;
    int rc;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT || !info)
        return -1;
    pthread_mutex_lock(&t32.lock);
    if (!t32.sensor_added || num != t32.vinum) {
        IMP_LOG_ERR(TAG, "IMP_ISP_EnableSensor: no sensor on VI %d", (int)num);
        pthread_mutex_unlock(&t32.lock);
        return -1;
    }
    if (t32.sensor_enabled) {
        pthread_mutex_unlock(&t32.lock);
        return 0;
    }
#define T32_ON(req) (in.enable = 1, in.vinum = (int32_t)num, T32_IOCTL(t32.isp_fd, req, &in))
    rc = T32_ON(G_INPUT);
    if (rc == 0)
        rc = T32_ON(STREAMON);
    if (rc == 0) {
        rc = T32_ON(CREATE_SUBDEV_LINKS);
        if (rc == 0) {
            rc = T32_ON(LINKS_STREAMON);
            if (rc < 0) {
                in.enable = 0;
                T32_IOCTL(t32.isp_fd, DESTROY_SUBDEV_LINKS, &in);
            }
        }
        if (rc < 0) {
            in.enable = 0;
            T32_IOCTL(t32.isp_fd, STREAMOFF, &in);
        }
    }
#undef T32_ON
    if (rc == 0)
        t32.sensor_enabled = 1;
    pthread_mutex_unlock(&t32.lock);
    return rc < 0 ? -1 : 0;
}

int IMP_ISP_DisableSensor(IMPVI_NUM num)
{
    T32IocInitArg in;
    int rc;

    if (num < IMPVI_MAIN || num >= IMPVI_BUTT)
        return -1;
    pthread_mutex_lock(&t32.lock);
    if (!t32.sensor_enabled || num != t32.vinum) {
        pthread_mutex_unlock(&t32.lock);
        return 0;
    }
#define T32_OFF(req) (in.enable = 0, in.vinum = (int32_t)num, T32_IOCTL(t32.isp_fd, req, &in))
    rc = T32_OFF(G_INPUT);
    if (rc == 0)
        rc = T32_OFF(LINKS_STREAMOFF);
    if (rc == 0)
        rc = T32_OFF(DESTROY_SUBDEV_LINKS);
    if (rc == 0)
        rc = T32_OFF(STREAMOFF);
#undef T32_OFF
    if (rc == 0)
        t32.sensor_enabled = 0;
    pthread_mutex_unlock(&t32.lock);
    return rc < 0 ? -1 : 0;
}
