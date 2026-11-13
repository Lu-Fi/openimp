/*
 * OpenIMP T32/T33 encoder calls of batch E3 (#56 T33 / #41 T32; audit t33backlog/PLAN.md):
 * kernel encoder (/dev/kern_enc), the multi-process encoder server and its clients (*_Ex).
 *
 * As vendor libimp T33 2.0.2.1 / T32 2.2.0 (disassembly; no vendor code copied):
 *
 * KernEnc_*: /dev/kern_enc (O_RDWR) per call; open or request failure 0x80048000.
 *   Stop()                  0x20006314 (no argument)
 *   GetStatus(chn, *enable) 0xcc186317 {chn, ...}: *enable = word 1
 *   Release(chn)            chn < 16 (0x80040001), 0xcc186316 {chn, ...}
 *   GetStream(chn, *out)    chn < 16 (0x80040001), 0xcc186315
 *   No vendor kernel module of the T32/T33 cameras creates /dev/kern_enc (checked: the T33 camera
 *   kernel and every .ko of its firmware), so the vendor answers 0x80048000 there; OpenIMP does the
 *   same.  Behind an existing node only Stop / GetStatus / Release are issued; GetStream's mapping
 *   of the kernel stream buffer is not implemented (0x80048000, logged).
 *
 * Multi-process encoder (SysV message queues, msgget without IPC_CREAT on the client side):
 *   VBM queue 0x12345678   client message type 2 {op 0 alloc / 1 free, phys, size, align, pid,
 *                          status}, answer type 1 {op, phys, size, align, status}
 *   YUV queue 0x24861357   type 4 {op 0 init / 1 encode / 2 exit, handle, w, h, IMPEncoderYuvIn,
 *                          frame, out phys, out length, status, pid}, answer type 3
 *   JPEG queue 0x12354645  type 6 {w, h, quality, src phys, dst phys, length, status}, answer type 5
 *   Client calls without a server: msgget fails -> VbmAlloc_Ex NULL, VbmFree_Ex nothing, the YUV /
 *   JPEG calls -1.
 *   MultiProcessInit(): 0x80040080 once the encoder module is up (after IMP_System_Init), else the
 *   server threads (queues created fresh); a thread that cannot start: 0x80042000.
 *   MultiProcessExit(): stops them.
 *   Server here: VBM allocation for the clients (IMP_Encoder_VbmAlloc / VbmFree, 16 entries) and
 *   the release of allocations whose client process is gone (/proc/<pid>, every 100 ms), as the
 *   vendor.  Not as the vendor: the YUV and JPEG requests are answered "failed" (status 0): the
 *   unbound YUV / JPEG encoder (IMP_Encoder_YuvInit / YuvEncode / InputJpege) is not on T32/T33
 *   yet.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>

#include "imp_log_int.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_enc_e3.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ENC-E3"

#define E_ENC_CHN    ((int)0x80040001)
#define E_ENC_STATE  ((int)0x80040080)
#define E_ENC_THREAD ((int)0x80042000)
#define E_ENC_DEV    ((int)0x80048000)

#define KERN_ENC_NODE "/dev/kern_enc"
#define IOC_KE_STOP   0x20006314u
#define IOC_KE_STREAM 0xcc186315u
#define IOC_KE_REL    0xcc186316u
#define IOC_KE_STATUS 0xcc186317u
#define KE_CHANNELS   16
#define KE_BLOCK      0xc18         /* the request block size coded in the command */

#define Q_VBM  0x12345678
#define Q_YUV  0x24861357
#define Q_JPEG 0x12354645

/* ---- kernel encoder ------------------------------------------------------ */
static int ke_open(void)
{
    int fd = open(KERN_ENC_NODE, O_RDWR);

    if (fd < 0)
        IMP_LOG_ERR(TAG, "open %s: %s", KERN_ENC_NODE, strerror(errno));
    return fd;
}

static int ke_request(uint32_t req, void *arg)
{
    int fd = ke_open(), rc;

    if (fd < 0)
        return E_ENC_DEV;
    rc = ioctl(fd, req, arg);
    if (rc < 0)
        IMP_LOG_ERR(TAG, "%s 0x%08x: %s", KERN_ENC_NODE, req, strerror(errno));
    close(fd);
    return rc < 0 ? E_ENC_DEV : 0;
}

int IMP_Encoder_KernEnc_Stop(void)
{
    return ke_request(IOC_KE_STOP, NULL);
}

int IMP_Encoder_KernEnc_GetStatus(int encChn, int *enable)
{
    static uint32_t blk[KE_BLOCK / 4];
    int rc;

    memset(blk, 0, sizeof(blk));
    blk[0] = (uint32_t)encChn;
    rc = ke_request(IOC_KE_STATUS, blk);
    if (rc == 0 && enable)
        *enable = (int)blk[1];
    return rc;
}

int IMP_Encoder_KernEnc_Release(int encChn)
{
    static uint32_t blk[KE_BLOCK / 4];

    if (encChn >= KE_CHANNELS)
        return E_ENC_CHN;
    memset(blk, 0, sizeof(blk));
    blk[0] = (uint32_t)encChn;
    return ke_request(IOC_KE_REL, blk);
}

int IMP_Encoder_KernEnc_GetStream(int encChn, IMPEncoderKernEncOut *encOut)
{
    int fd;

    if (encChn >= KE_CHANNELS)
        return E_ENC_CHN;
    (void)encOut;
    fd = ke_open();
    if (fd < 0)
        return E_ENC_DEV;
    close(fd);
    IMP_LOG_ERR(TAG, "KernEnc_GetStream: kernel stream mapping not implemented");
    return E_ENC_DEV;
}

/* ---- multi-process clients ------------------------------------------------ */
struct vbm_msg {
    long mtype;
    int32_t op;
    uint32_t phys, size, align;
    int32_t pid;
    int32_t status;
};

struct yuv_msg {
    long mtype;
    uint8_t body[0xac];
};
#define YUV_OP     0x00
#define YUV_HANDLE 0x04
#define YUV_W      0x08
#define YUV_H      0x0c
#define YUV_IN     0x10
#define YUV_FRAME  0x64
#define YUV_OUTPA  0x94
#define YUV_OUTLEN 0x98
#define YUV_STATUS 0x9c
#define YUV_PID    0xa0
#define YUV_FRAME_BYTES 0x30

struct jpeg_msg {
    long mtype;
    uint32_t w, h, q, src, dst, len;
    int32_t status;
};

static uint32_t yuv_get(const struct yuv_msg *m, int off)
{
    uint32_t v;

    memcpy(&v, m->body + off, 4);
    return v;
}

static void yuv_put(struct yuv_msg *m, int off, uint32_t v)
{
    memcpy(m->body + off, &v, 4);
}

void *IMP_Encoder_VbmAlloc_Ex(uint32_t size, uint32_t align)
{
    struct vbm_msg m;
    int q;

    memset(&m, 0, sizeof(m));
    m.mtype = 2;
    m.op = 0;
    m.size = size;
    m.align = align;
    m.pid = (int32_t)getpid();
    q = msgget(Q_VBM, 0664);
    if (q < 0) {
        IMP_LOG_ERR(TAG, "VbmAlloc_Ex: no encoder server (msgget: %s)", strerror(errno));
        return NULL;
    }
    msgsnd(q, &m, sizeof(m) - sizeof(long), 0);
    msgrcv(q, &m, sizeof(m) - sizeof(long), 1, 0);
    if (m.status == 1)
        return NULL;
    return (void *)IMP_Encoder_VbmP2V((intptr_t)m.phys);
}

void IMP_Encoder_VbmFree_Ex(void *vaddr)
{
    struct vbm_msg m;
    int q;

    memset(&m, 0, sizeof(m));
    m.mtype = 2;
    m.op = 1;
    m.phys = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)vaddr);
    q = msgget(Q_VBM, 0664);
    if (q < 0)
        return;
    msgsnd(q, &m, sizeof(m) - sizeof(long), 0);
    msgrcv(q, &m, sizeof(m) - sizeof(long), 1, 0);
}

static int yuv_call(struct yuv_msg *m)
{
    int q = msgget(Q_YUV, 0664);

    if (q < 0) {
        IMP_LOG_ERR(TAG, "Yuv*_Ex: no encoder server (msgget: %s)", strerror(errno));
        return -1;
    }
    m->mtype = 4;
    msgsnd(q, m, sizeof(m->body), 0);
    msgrcv(q, m, sizeof(m->body), 3, 0);
    return yuv_get(m, YUV_STATUS) ? 0 : -1;
}

int IMP_Encoder_YuvInit_Ex(int *h, int inWidth, int inHeight, IMPEncoderYuvIn *encIn)
{
    struct yuv_msg m;

    memset(&m, 0, sizeof(m));
    yuv_put(&m, YUV_OP, 0);
    yuv_put(&m, YUV_W, (uint32_t)inWidth);
    yuv_put(&m, YUV_H, (uint32_t)inHeight);
    if (encIn)
        memcpy(m.body + YUV_IN, encIn, sizeof(*encIn) < 0x50 ? sizeof(*encIn) : 0x50);
    yuv_put(&m, YUV_PID, (uint32_t)getpid());
    if (yuv_call(&m) != 0)
        return -1;
    if (h)
        *h = (int)yuv_get(&m, YUV_HANDLE);
    return 0;
}

int IMP_Encoder_YuvEncode_Ex(int h, IMPFrameInfo frame, IMPEncoderYuvOut *encOut)
{
    struct yuv_msg m;

    memset(&m, 0, sizeof(m));
    yuv_put(&m, YUV_OP, 1);
    yuv_put(&m, YUV_HANDLE, (uint32_t)h);
    memcpy(m.body + YUV_FRAME, &frame, YUV_FRAME_BYTES);
    if (encOut) {
        yuv_put(&m, YUV_OUTPA, (uint32_t)IMP_Encoder_VbmV2P((intptr_t)encOut->outAddr));
        yuv_put(&m, YUV_OUTLEN, encOut->outLen);
    }
    if (yuv_call(&m) != 0)
        return -1;
    if (encOut)
        encOut->outLen = yuv_get(&m, YUV_OUTLEN);
    return 0;
}

int IMP_Encoder_YuvExit_Ex(int h)
{
    struct yuv_msg m;

    memset(&m, 0, sizeof(m));
    yuv_put(&m, YUV_OP, 2);
    yuv_put(&m, YUV_HANDLE, (uint32_t)h);
    return yuv_call(&m);
}

int IMP_Encoder_InputJpege_Ex(uint8_t *src, uint8_t *dst, int src_w, int src_h, int q,
                              int *stream_length)
{
    struct jpeg_msg m;
    int id;

    memset(&m, 0, sizeof(m));
    m.mtype = 6;
    m.w = (uint32_t)src_w;
    m.h = (uint32_t)src_h;
    m.q = (uint32_t)q;
    m.src = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)src);
    m.dst = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)dst);
    id = msgget(Q_JPEG, 0664);
    if (id < 0) {
        IMP_LOG_ERR(TAG, "InputJpege_Ex: no encoder server (msgget: %s)", strerror(errno));
        return -1;
    }
    msgsnd(id, &m, sizeof(m) - sizeof(long), 0);
    msgrcv(id, &m, sizeof(m) - sizeof(long), 5, 0);
    if (!m.status)
        return -1;
    if (stream_length)
        *stream_length = (int)m.len;
    return 0;
}

/* ---- multi-process server --------------------------------------------------- */
#define MP_VBM_SLOTS 16
static struct {
    pthread_t vbm, pidchk, yuv, jpeg;
    int on_vbm, on_pidchk, on_yuv, on_jpeg;
    pthread_mutex_t lock;
    int32_t pid[MP_VBM_SLOTS];
    uint32_t phys[MP_VBM_SLOTS];
} mp = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* a fresh queue (an old one of a previous server is removed first) */
static int mp_queue(key_t key)
{
    int q = msgget(key, 0664);

    if (q >= 0)
        msgctl(q, IPC_RMID, NULL);
    q = msgget(key, 0664 | IPC_CREAT);
    if (q < 0)
        IMP_LOG_ERR(TAG, "server queue 0x%08x: %s", (unsigned)key, strerror(errno));
    return q;
}

static void *mp_vbm_thread(void *arg)
{
    int q = mp_queue(Q_VBM);

    (void)arg;
    if (q < 0)
        return NULL;
    for (;;) {
        struct vbm_msg m;
        struct {
            long mtype;
            int32_t op;
            uint32_t phys, size, align;
            int32_t status;
        } a;
        ssize_t n;
        int i;

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        n = msgrcv(q, &m, sizeof(m) - sizeof(long), 2, 0);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (n < 0) {
            if (errno == EIDRM || errno == EINVAL)
                return NULL;
            continue;
        }
        m.status = 0;
        if (m.op == 0) {
            void *v = IMP_Encoder_VbmAlloc(m.size, m.align);

            if (v) {
                m.phys = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)v);
                pthread_mutex_lock(&mp.lock);
                for (i = 0; i < MP_VBM_SLOTS; i++)
                    if (!mp.pid[i]) {
                        mp.pid[i] = m.pid;
                        mp.phys[i] = m.phys;
                        break;
                    }
                pthread_mutex_unlock(&mp.lock);
            } else {
                m.status = 1;
            }
        } else if (m.op == 1) {
            IMP_Encoder_VbmFree((void *)IMP_Encoder_VbmP2V((intptr_t)m.phys));
            pthread_mutex_lock(&mp.lock);
            for (i = 0; i < MP_VBM_SLOTS; i++)
                if (mp.phys[i] == m.phys && mp.pid[i]) {
                    mp.pid[i] = 0;
                    mp.phys[i] = 0;
                    break;
                }
            pthread_mutex_unlock(&mp.lock);
        }
        a.mtype = 1;
        a.op = m.op;
        a.phys = m.phys;
        a.size = m.size;
        a.align = m.align;
        a.status = m.status;
        msgsnd(q, &a, sizeof(a) - sizeof(long), 0);
    }
    return NULL;
}

static void *mp_pid_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int i;

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        pthread_mutex_lock(&mp.lock);
        for (i = 0; i < MP_VBM_SLOTS; i++) {
            char path[32];

            if (!mp.pid[i])
                continue;
            snprintf(path, sizeof(path), "/proc/%d", (int)mp.pid[i]);
            if (access(path, F_OK) == 0)
                continue;
            IMP_Encoder_VbmFree((void *)IMP_Encoder_VbmP2V((intptr_t)mp.phys[i]));
            mp.pid[i] = 0;
            mp.phys[i] = 0;
        }
        pthread_mutex_unlock(&mp.lock);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        usleep(100000);
    }
    return NULL;
}

static void *mp_yuv_thread(void *arg)
{
    int q = mp_queue(Q_YUV);

    (void)arg;
    if (q < 0)
        return NULL;
    for (;;) {
        struct yuv_msg m;
        ssize_t n;

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        n = msgrcv(q, &m, sizeof(m.body), 4, 0);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (n < 0) {
            if (errno == EIDRM || errno == EINVAL)
                return NULL;
            continue;
        }
        IMP_LOG_ERR(TAG, "multi-process YUV request %u: unbound YUV encoder not available on "
                    "T32/T33", yuv_get(&m, YUV_OP));
        yuv_put(&m, YUV_STATUS, 0);
        m.mtype = 3;
        msgsnd(q, &m, sizeof(m.body), 0);
    }
    return NULL;
}

static void *mp_jpeg_thread(void *arg)
{
    int q = mp_queue(Q_JPEG);

    (void)arg;
    if (q < 0)
        return NULL;
    for (;;) {
        struct jpeg_msg m;
        ssize_t n;

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        n = msgrcv(q, &m, sizeof(m) - sizeof(long), 6, 0);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (n < 0) {
            if (errno == EIDRM || errno == EINVAL)
                return NULL;
            continue;
        }
        IMP_LOG_ERR(TAG, "multi-process JPEG request: unbound JPEG encoder not available on "
                    "T32/T33");
        m.status = 0;
        m.len = 0;
        m.mtype = 5;
        msgsnd(q, &m, sizeof(m) - sizeof(long), 0);
    }
    return NULL;
}

static void mp_stop(pthread_t *t, int *on)
{
    if (!*on)
        return;
    pthread_cancel(*t);
    pthread_join(*t, NULL);
    *on = 0;
}

void IMP_Encoder_MultiProcessExit(void)
{
    mp_stop(&mp.vbm, &mp.on_vbm);
    mp_stop(&mp.pidchk, &mp.on_pidchk);
    mp_stop(&mp.yuv, &mp.on_yuv);
    mp_stop(&mp.jpeg, &mp.on_jpeg);
}

int IMP_Encoder_MultiProcessInit(void)
{
    if (OpenIMP_T32_SystemInitDone())
        return E_ENC_STATE;
    if (mp.on_vbm)
        return 0;
    mp.on_vbm = pthread_create(&mp.vbm, NULL, mp_vbm_thread, NULL) == 0;
    if (mp.on_vbm)
        mp.on_pidchk = pthread_create(&mp.pidchk, NULL, mp_pid_thread, NULL) == 0;
    if (mp.on_pidchk)
        mp.on_yuv = pthread_create(&mp.yuv, NULL, mp_yuv_thread, NULL) == 0;
    if (mp.on_yuv)
        mp.on_jpeg = pthread_create(&mp.jpeg, NULL, mp_jpeg_thread, NULL) == 0;
    if (!mp.on_jpeg) {
        IMP_Encoder_MultiProcessExit();
        return E_ENC_THREAD;
    }
    return 0;
}
