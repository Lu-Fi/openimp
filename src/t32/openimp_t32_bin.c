/*
 * OpenIMP T32 userspace IQ-bin loader (#289, planned; not a release gate).
 *
 * On the vendor 2.x kernel ABI the library, not the driver, reads the
 * sensor's IQ bin and hands it over (driver: open-tx-isp driver/t32).  Flow
 * as vendor libimp 2.2.0 issues it in QEMU -M ingenic-t32 (reference
 * tests/t32/vendor-2.2.0-ioctl-seq-{isp,bin}.txt); no vendor code copied.
 *
 *   IMP_ISP_Open        /dev/mem (O_SYNC: uncached view of the bin buffers)
 *   IMP_ISP_AddSensor   after REGISTER_SENSOR: DEFAULT_BIN (880 B: kernel
 *                       fast-boot bins at byte 860, not used on PRJ007),
 *                       default path of each mode, /etc/sensor/<sensor>-PRJ007-
 *                       {day,night,day-wdr,night-wdr}.bin (else /tmp/...),
 *                       kept only when the file passes the check below; then
 *                       the bin of the current mode is loaded
 *   check               bin bytes 0-7 = format version, bytes 8..0x9f header
 *                       (SoC at 0x98); DEFAULT_BIN gives the driver's version
 *                       (byte 863) and SoC (byte 871); both must match
 *   load                two MEMORY_MALLOC buffers of 0x32000 B mapped through
 *                       /dev/mem: a shared spare (first) and the VI's own; the
 *                       first load goes into the VI buffer, a later switch into
 *                       the spare and the two swap after SET_TIZIANO_PAR {pa,
 *                       size, wdr, night, update_csccr, vinum}
 *   day / night         SetISPRunningMode / SwitchBin load the bin of the new
 *                       mode; StartNightMode (before AddSensor) starts at night
 *   IMP_ISP_DelSensor   after RELEASE_SENSOR: MEMORY_FREE of the VI buffer, and
 *                       of the spare once no sensor is left
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_isp.h>

#include "imp_log_int.h"
#include "t32/openimp_t32_ioctl.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_bin.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-BIN"

#define T32_BIN_VI        4
#define T32_BIN_MODES     IMPISP_BIN_MODE_BUTT
#define T32_BIN_PATH      128
#ifdef PLATFORM_T33
#define T32_BIN_CAPACITY  0x25800u  /* PRJ008 buffer of the vendor T33 libimp 2.0.2.1 (#390) */
#define T32_BIN_SOC_TAG   "-PRJ008-"
#else
#define T32_BIN_CAPACITY  0x32000u  /* PRJ007 buffer (vendor: PRJ008 0x25800) */
#define T32_BIN_SOC_TAG   "-PRJ007-"
#endif
#define T32_BIN_VER_LEN   8         /* bin bytes 0-7 */
#define T32_BIN_HDR_LEN   0x98      /* bin bytes 8..0x9f */
#define T32_BIN_HDR_SOC   0x90      /* SoC in that header (bin byte 0x98) */
#define T32_DBIN_MODE     856       /* DEFAULT_BIN: running mode (out) */
#define T32_DBIN_FASTBOOT 860       /* DEFAULT_BIN: kernel already holds the bins */
#define T32_DBIN_VI       862       /* DEFAULT_BIN: VI asked about (in) */
#define T32_DBIN_VER      863
#define T32_DBIN_SOC      871

typedef struct {
    void *vaddr;                /* /dev/mem mapping */
    uint32_t paddr, size, kva;  /* MEMORY_MALLOC answer */
} T32BinBuf;

typedef struct {
    int added;                          /* AddSensor done (paths fixed) */
    int start_night;                    /* StartNightMode was called */
    uint32_t mode;                      /* IMPISPBinMode of the loaded bin */
    char def_path[T32_BIN_MODES][T32_BIN_PATH];
    char act_path[T32_BIN_MODES][T32_BIN_PATH];
    T32BinBuf buf;
    pthread_mutex_t lock;
} T32BinVi;

static struct {
    pthread_mutex_t lock;
    int mem_fd;
    int sensors;                        /* VIs holding a bin */
    T32BinBuf spare;
    T32BinVi vi[T32_BIN_VI];
} tb = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .mem_fd = -1,
};

static const char *const t32_bin_mode_name[T32_BIN_MODES] = {
    "day", "night", "day-wdr", "night-wdr",
};

/* ---- open / close (called from IMP_ISP_Open / Close) ------------------- */
int OpenIMP_T32_BinOpen(void)
{
    int i;

    pthread_mutex_lock(&tb.lock);
    if (tb.mem_fd < 0) {
        tb.mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (tb.mem_fd < 0) {
            IMP_LOG_ERR(TAG, "open /dev/mem: %s", strerror(errno));
            pthread_mutex_unlock(&tb.lock);
            return -1;
        }
        for (i = 0; i < T32_BIN_VI; i++) {
            memset(&tb.vi[i], 0, sizeof(tb.vi[i]));
            pthread_mutex_init(&tb.vi[i].lock, NULL);
        }
    }
    pthread_mutex_unlock(&tb.lock);
    return 0;
}

void OpenIMP_T32_BinClose(void)
{
    pthread_mutex_lock(&tb.lock);
    if (tb.mem_fd >= 0)
        close(tb.mem_fd);
    tb.mem_fd = -1;
    pthread_mutex_unlock(&tb.lock);
}

/* ---- driver buffers ----------------------------------------------------- */
static int t32_bin_alloc(int fd, T32BinBuf *b, uint32_t size)
{
    T32IocBufInfo m;
    void *va;

    memset(&m, 0, sizeof(m));
    m.size = size;
    if (ioctl(fd, OI_T32_IOC_MEMORY_MALLOC, &m) < 0) {
        IMP_LOG_ERR(TAG, "MEMORY_MALLOC %u: %s", size, strerror(errno));
        return -1;
    }
    va = mmap(NULL, m.size, PROT_READ | PROT_WRITE, MAP_SHARED, tb.mem_fd, (off_t)m.paddr);
    if (va == MAP_FAILED) {
        IMP_LOG_ERR(TAG, "map bin buffer pa 0x%08x: %s", m.paddr, strerror(errno));
        m.paddr = 0;
        ioctl(fd, OI_T32_IOC_MEMORY_FREE, &m);
        return -1;
    }
    b->vaddr = va;
    b->kva = m.word0;
    b->paddr = m.paddr;
    b->size = m.size;
    return 0;
}

static void t32_bin_free(int fd, T32BinBuf *b)
{
    T32IocBufInfo m;

    if (!b->vaddr)
        return;
    munmap(b->vaddr, b->size);
    memset(&m, 0, sizeof(m));
    m.word0 = b->kva;
    if (fd < 0 || ioctl(fd, OI_T32_IOC_MEMORY_FREE, &m) < 0)
        IMP_LOG_WARN(TAG, "MEMORY_FREE 0x%08x: %s", b->kva, strerror(errno));
    memset(b, 0, sizeof(*b));
}

/* ---- bin check: format version and SoC must be the driver's ------------- */
static int t32_bin_check(const char *path)
{
    T32IocDefaultBin db;
    char ver[T32_BIN_VER_LEN + 1], hdr[T32_BIN_HDR_LEN + 1];
    int fd = OpenIMP_T32_IspFd();
    FILE *f;
    int ok;

    if (!path || !path[0])
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;              /* quiet: probing the default locations */
    memset(ver, 0, sizeof(ver));
    memset(hdr, 0, sizeof(hdr));
    ok = fread(ver, 1, T32_BIN_VER_LEN, f) == T32_BIN_VER_LEN &&
         fread(hdr, T32_BIN_HDR_LEN, 1, f) == 1;
    fclose(f);
    if (!ok) {
        IMP_LOG_ERR(TAG, "%s: not an IQ bin (short header)", path);
        return -1;
    }
    memset(&db, 0, sizeof(db));
    if (fd < 0 || ioctl(fd, OI_T32_IOC_DEFAULT_BIN, &db) < 0) {
        IMP_LOG_ERR(TAG, "DEFAULT_BIN: %s", strerror(errno));
        return -1;
    }
    db.bytes[sizeof(db.bytes) - 1] = 0;
    if (strncmp(hdr + T32_BIN_HDR_SOC, (const char *)db.bytes + T32_DBIN_SOC,
                T32_BIN_HDR_LEN - T32_BIN_HDR_SOC)) {
        IMP_LOG_ERR(TAG, "%s: bin SoC %.8s, driver %.8s", path, hdr + T32_BIN_HDR_SOC,
                    (const char *)db.bytes + T32_DBIN_SOC);
        return -1;
    }
    if (strncmp(ver, (const char *)db.bytes + T32_DBIN_VER, T32_DBIN_SOC - T32_DBIN_VER)) {
        IMP_LOG_ERR(TAG, "%s: bin version %.8s not supported (driver %.8s)", path, ver,
                    (const char *)db.bytes + T32_DBIN_VER);
        return -1;
    }
    return 0;
}

/* ---- load the bin of a mode (vendor JZ_ISP_BinUpdate flow) --------------- */
static int t32_bin_update(IMPVI_NUM num, uint32_t mode)
{
    T32BinVi *v = &tb.vi[num];
    T32BinBuf *dst, tmp;
    T32IocTizianoPar par;
    int fd = OpenIMP_T32_IspFd();
    uint32_t prev_night;
    long size = -1;
    FILE *f;

    if (mode >= T32_BIN_MODES || fd < 0)
        return -1;
    if (!v->act_path[mode][0]) {
        if (!v->def_path[mode][0] || access(v->def_path[mode], F_OK)) {
            IMP_LOG_ERR(TAG, "no %s bin for VI %d (%s)", t32_bin_mode_name[mode], (int)num,
                        v->def_path[mode][0] ? v->def_path[mode] : "not found");
            return -1;
        }
        memcpy(v->act_path[mode], v->def_path[mode], T32_BIN_PATH);
    }
    f = fopen(v->act_path[mode], "rb");
    if (!f) {
        IMP_LOG_ERR(TAG, "open %s: %s", v->act_path[mode], strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || (uint32_t)size > T32_BIN_CAPACITY) {
        IMP_LOG_ERR(TAG, "%s: size %ld (max %u)", v->act_path[mode], size, T32_BIN_CAPACITY);
        fclose(f);
        return -1;
    }
    rewind(f);

    pthread_mutex_lock(&v->lock);
    if (!tb.spare.vaddr && t32_bin_alloc(fd, &tb.spare, T32_BIN_CAPACITY) < 0)
        goto fail;
    if (!v->buf.vaddr) {
        if (t32_bin_alloc(fd, &v->buf, T32_BIN_CAPACITY) < 0)
            goto fail;
        dst = &v->buf;          /* first bin of this VI */
    } else {
        dst = &tb.spare;        /* switch: load beside the running bin */
    }
    if (fread(dst->vaddr, 1, (size_t)size, f) != (size_t)size) {
        IMP_LOG_ERR(TAG, "read %s: short", v->act_path[mode]);
        goto fail;
    }
    fclose(f);
    f = NULL;

    prev_night = v->mode & 1u;
    memset(&par, 0, sizeof(par));
    par.paddr = dst->paddr;
    par.size = (uint32_t)size;
    par.wdr = mode >= IMPISP_BIN_MODE_DAY_WDR;
    par.night = mode & 1u;
    par.update_csccr = par.night ? (!prev_night || v->start_night) : prev_night;
    par.vinum = (uint8_t)num;
    if (ioctl(fd, OI_T32_IOC_SET_TIZIANO_PAR, &par) < 0) {
        IMP_LOG_ERR(TAG, "SET_TIZIANO_PAR %s: %s", v->act_path[mode], strerror(errno));
        goto fail;
    }
    if (dst == &tb.spare) {     /* the old bin's buffer is the new spare */
        tmp = v->buf;
        v->buf = tb.spare;
        tb.spare = tmp;
    }
    pthread_mutex_unlock(&v->lock);
    v->mode = mode;
    IMP_LOG_INFO(TAG, "VI %d %s bin %s (%ld B) at pa 0x%08x", (int)num, t32_bin_mode_name[mode],
                 v->act_path[mode], size, par.paddr);
    return 0;

fail:
    pthread_mutex_unlock(&v->lock);
    if (f)
        fclose(f);
    return -1;
}

/* ---- AddSensor / DelSensor (called from openimp_t32_isp.c) -------------- */
int OpenIMP_T32_BinAddSensor(IMPVI_NUM num, const char *sensor)
{
    T32IocDefaultBin db;
    T32BinVi *v;
    char path[T32_BIN_PATH];
    int fd = OpenIMP_T32_IspFd(), m, rc;

    if (num < IMPVI_MAIN || num >= T32_BIN_VI || fd < 0 || tb.mem_fd < 0)
        return -1;
    v = &tb.vi[num];
    memset(&db, 0, sizeof(db));
    if (ioctl(fd, OI_T32_IOC_DEFAULT_BIN, &db) < 0) {
        IMP_LOG_ERR(TAG, "DEFAULT_BIN: %s", strerror(errno));
        return -1;
    }
    if (db.bytes[T32_DBIN_FASTBOOT]) {
        IMP_LOG_ERR(TAG, "driver holds fast-boot IQ bins (kernel loaded); not supported");
        return -1;
    }
    for (m = 0; m < T32_BIN_MODES; m++) {
        if (v->def_path[m][0])
            continue;           /* IMP_ISP_SetDefaultBinPath */
        snprintf(path, sizeof(path), "/etc/sensor/%s" T32_BIN_SOC_TAG "%s.bin", sensor,
                 t32_bin_mode_name[m]);
        if (t32_bin_check(path)) {
            snprintf(path, sizeof(path), "/tmp/%s" T32_BIN_SOC_TAG "%s.bin", sensor,
                     t32_bin_mode_name[m]);
            if (t32_bin_check(path))
                continue;
        }
        memcpy(v->def_path[m], path, T32_BIN_PATH);
    }
    v->added = 1;
    pthread_mutex_lock(&tb.lock);
    tb.sensors++;
    pthread_mutex_unlock(&tb.lock);
    rc = t32_bin_update(num, v->mode);
    if (rc < 0) {
        IMP_LOG_ERR(TAG, "IQ bin update failed for %s", sensor);
        OpenIMP_T32_BinDelSensor(num);
        return -1;
    }
    return 0;
}

/* VI buffer first, the shared spare with the last sensor (vendor order) */
void OpenIMP_T32_BinDelSensor(IMPVI_NUM num)
{
    T32BinVi *v;
    int fd = OpenIMP_T32_IspFd();

    if (num < IMPVI_MAIN || num >= T32_BIN_VI)
        return;
    v = &tb.vi[num];
    pthread_mutex_lock(&v->lock);
    t32_bin_free(fd, &v->buf);
    pthread_mutex_lock(&tb.lock);
    if (v->added && tb.sensors > 0)
        tb.sensors--;
    if (!tb.sensors)
        t32_bin_free(fd, &tb.spare);
    pthread_mutex_unlock(&tb.lock);
    pthread_mutex_unlock(&v->lock);
    /* a new AddSensor starts again from the defaults and the day bin */
    memset(v->def_path, 0, sizeof(v->def_path));
    memset(v->act_path, 0, sizeof(v->act_path));
    v->mode = IMPISP_BIN_MODE_DAY;
    v->start_night = 0;
    v->added = 0;
}

/* ---- public API ------------------------------------------------------- */
static int t32_bin_vi_ok(IMPVI_NUM num)
{
    return num >= IMPVI_MAIN && num < T32_BIN_VI && tb.mem_fd >= 0;
}

int IMP_ISP_SetDefaultBinPath(IMPVI_NUM num, IMPISPDefaultBinAttr *attr)
{
    T32BinVi *v;

    if (!attr || !t32_bin_vi_ok(num) || (unsigned)attr->mode >= T32_BIN_MODES)
        return -1;
    v = &tb.vi[num];
    if (v->added) {
        IMP_LOG_WARN(TAG, "SetDefaultBinPath after AddSensor: ignored");
        return 0;
    }
    if (t32_bin_check(attr->bname)) {
        IMP_LOG_ERR(TAG, "SetDefaultBinPath: %.128s is not a usable bin", attr->bname);
        return -1;
    }
    memcpy(v->def_path[attr->mode], attr->bname, T32_BIN_PATH);
    v->def_path[attr->mode][T32_BIN_PATH - 1] = 0;
    return 0;
}

int OpenIMP_T32_BinDefaultPath(int num, int mode, char *path)
{
    if (!path || !t32_bin_vi_ok((IMPVI_NUM)num) || (unsigned)mode >= T32_BIN_MODES)
        return -1;
    memcpy(path, tb.vi[num].def_path[mode], T32_BIN_PATH);
    path[T32_BIN_PATH - 1] = 0;
    return 0;
}

int OpenIMP_T32_BinSetWdr(int num, int on)
{
    T32BinVi *v;
    uint32_t next;

    if (!t32_bin_vi_ok((IMPVI_NUM)num))
        return -1;
    v = &tb.vi[num];
    next = (v->mode & ~2u) | (on ? 2u : 0u);    /* IMPISP_BIN_MODE_*_WDR = mode | 2 */
    if (next == v->mode)
        return 0;
    if (!v->added) {
        v->mode = next;         /* the first bin AddSensor loads */
        return 0;
    }
    return t32_bin_update((IMPVI_NUM)num, next);
}

int IMP_ISP_StartNightMode(IMPVI_NUM num)
{
    T32BinVi *v;

    if (!t32_bin_vi_ok(num))
        return -1;
    v = &tb.vi[num];
    if (v->added)
        return 0;               /* vendor: only before AddSensor, else no-op */
    v->mode = v->mode >= IMPISP_BIN_MODE_DAY_WDR ? IMPISP_BIN_MODE_NIGHT_WDR : IMPISP_BIN_MODE_NIGHT;
    v->start_night = 1;
    return 0;
}

/*
 * The vendor additionally refuses these before IMP_ISP_EnableTuning; T32
 * tuning is a later issue, so here the sensor must be added (bin loaded).
 */
int IMP_ISP_Tuning_SetISPRunningMode(IMPVI_NUM num, IMPISPRunningMode *mode)
{
    T32BinVi *v;
    uint32_t next;

    if (!mode || !t32_bin_vi_ok(num) || (unsigned)*mode > IMPISP_RUNNING_MODE_NIGHT)
        return -1;
    v = &tb.vi[num];
    if (!v->added)
        return -1;
    next = (v->mode & ~1u) | ((uint32_t)*mode & 1u);
    if (next != v->mode && t32_bin_update(num, next) < 0)
        return -1;
    return 0;
}

/* the driver reports the running mode: DEFAULT_BIN with the VI at byte 862,
 * answer at 856 (vendor flow; open-tx-isp driver/t32 answers 0 so far) */
int IMP_ISP_Tuning_GetISPRunningMode(IMPVI_NUM num, IMPISPRunningMode *pmode)
{
    T32IocDefaultBin db;
    uint32_t mode;
    int fd = OpenIMP_T32_IspFd();

    if (!pmode || !t32_bin_vi_ok(num) || !tb.vi[num].added || fd < 0)
        return -1;
    memset(&db, 0, sizeof(db));
    db.bytes[T32_DBIN_VI] = (uint8_t)num;
    if (ioctl(fd, OI_T32_IOC_DEFAULT_BIN, &db) < 0) {
        IMP_LOG_ERR(TAG, "DEFAULT_BIN (running mode): %s", strerror(errno));
        return -1;
    }
    memcpy(&mode, db.bytes + T32_DBIN_MODE, sizeof(mode));
    *pmode = (IMPISPRunningMode)mode;
    return 0;
}

int IMP_ISP_Tuning_SwitchBin(IMPVI_NUM num, IMPISPBinAttr *attr)
{
    T32BinVi *v;

    if (!attr || !t32_bin_vi_ok(num) || (unsigned)attr->mode >= T32_BIN_MODES)
        return -1;
    v = &tb.vi[num];
    if (!v->added)
        return -1;
    if (attr->enable == IMPISP_TUNING_OPS_MODE_ENABLE) {
        if (t32_bin_check(attr->bname)) {
            IMP_LOG_ERR(TAG, "SwitchBin: %.128s is not a usable bin", attr->bname);
            return -1;
        }
        memcpy(v->act_path[attr->mode], attr->bname, T32_BIN_PATH);
        v->act_path[attr->mode][T32_BIN_PATH - 1] = 0;
        return t32_bin_update(num, attr->mode);
    }
    /* back to the default bin; reload when it is the running mode */
    memset(v->act_path[attr->mode], 0, T32_BIN_PATH);
    if (v->mode == (uint32_t)attr->mode)
        return t32_bin_update(num, attr->mode);
    return 0;
}
