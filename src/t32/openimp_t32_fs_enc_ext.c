/*
 * OpenIMP T32/T33 FrameSource / encoder state calls of the 2.x API without a hardware path of
 * their own (#56 #57 T33, #41 #42 T32; batch Q2 of audit t33backlog/PLAN.md).
 *
 * Return codes and the order of the checks as vendor libimp 2.0.2.1 (T33) shows them in QEMU
 * -M ingenic-t33 with the same apitest (case t33pass, vendor library on the vendor module =
 * reference).  No vendor code copied.  Built against the vendor 2.x headers (T32 2.2.0 /
 * T33 2.0.2.1) like openimp_t32_tuning.c: the joint, frame-used and alignment types are theirs.
 *
 * FrameSource (0x800100NN): channel >= 12 01, NULL 08, depth already set 20; joints are library
 * bookkeeping (a handle per board; up to 12).  Encoder (0x800400NN): channel >= 16 01, value 02,
 * not created 08, NULL 10, before CreateChn only 80 (the *Enable calls 04); the global buffer and
 * multi-section settings only before the encoder module is up (IMP_System_Init), else 80.
 *
 * What is stored but not yet used by the T32/T33 encoder / FrameSource (matrix cell "cache"):
 * fisheye, read-buffer share, motion quality, QP-group AI, resize mode, frame-used mode, the
 * HERA / JPEG bitstream sizes and multi-section modes, the YUV alignment, the depth copy type,
 * and the joint composition (the vendor composes joined channels in its frame path; the API
 * state is here, the composition is not).
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_encoder.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "t32/openimp_t32_isp.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_fs_enc_ext.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-EXT"

#define FS_CHNS  12
#define ENC_CHNS 16

#define E_FS_CHN    ((int)0x80010001)
#define E_FS_NOMEM  ((int)0x80010100)
#define E_FS_BUSY   ((int)0x80010004)
#define E_FS_NULL   ((int)0x80010008)
#define E_FS_STATE  ((int)0x80010020)

#define E_ENC_CHN   ((int)0x80040001)
#define E_ENC_PARAM ((int)0x80040002)
#define E_ENC_CRE4  ((int)0x80040004)
#define E_ENC_NOCHN ((int)0x80040008)
#define E_ENC_NULL  ((int)0x80040010)
#define E_ENC_STATE ((int)0x80040080)

int OpenIMP_T32_FsChnState(int chn, int *depth);   /* openimp_t32_fs.c */
int OpenIMP_T32_EncChnCreated(int encChn);           /* openimp_t32_encoder.c */
int OpenIMP_T32_FsChnSize(int chn, int *w, int *h);  /* openimp_t32_fs.c */

static pthread_mutex_t ext_lock = PTHREAD_MUTEX_INITIALIZER;

/* ======================================================== FrameSource == */
static struct {
    int nocopy_depth;
    IMPFrameAlign align;
} fs_ext[FS_CHNS];

int IMP_FrameSource_SetFrameDepthCopyType(int chnNum, int b_nocopy_depth)
{
    int depth;

    if (chnNum >= FS_CHNS || chnNum < 0)
        return E_FS_CHN;
    (void)OpenIMP_T32_FsChnState(chnNum, &depth);
    if (depth != 0) {
        IMP_LOG_ERR(TAG, "SetFrameDepthCopyType chn %d: frame depth %d already set", chnNum, depth);
        return E_FS_STATE;
    }
    pthread_mutex_lock(&ext_lock);
    fs_ext[chnNum].nocopy_depth = b_nocopy_depth != 0;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_FrameSource_SetYuvAlign(int chnNum, IMPFrameAlign *param)
{
    if ((unsigned)chnNum >= FS_CHNS)
        return E_FS_CHN;
    if (!param)
        return E_FS_NULL;
    pthread_mutex_lock(&ext_lock);
    fs_ext[chnNum].align = *param;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

/* the pool-id tables are allocated only by the multi-pool path, which this library keeps in
 * the FrameSource / encoder channel state: nothing to free, 0 as the vendor */
int IMP_FrameSource_ClearPoolId(void)
{
    return 0;
}

#define JOINTS 12
struct fs_joint {
    IMPFSJointAttr attr;        /* the board as created, width / height filled in */
    sem_t sem;
};
static struct fs_joint *joints[JOINTS];
static uint32_t joint_bg;

static int joint_find_locked(int32_t h)
{
    int i;

    for (i = 0; i < JOINTS; i++)
        if (joints[i] && (int32_t)(intptr_t)joints[i] == h)
            return i;
    return -1;
}

int IMP_FrameSource_CreateJoint(IMPFSJointAttr *joint_attr)
{
    struct fs_joint *j;
    int r, c, slot, w = 0, h = 0;

    if (!joint_attr)
        return E_FS_NULL;
    j = calloc(1, sizeof(*j));
    if (!j)
        return E_FS_NOMEM;
    /* rows of channel numbers from the top left, 0xff ends a row / the board; a channel already
     * in another joint is refused (vendor 0x80010004) */
    for (r = 0; r < 12 && joint_attr->position[r][0] != 0xff; r++) {
        int rw = 0, rh = 0;

        for (c = 0; c < 12 && joint_attr->position[r][c] != 0xff; c++) {
            unsigned ch = joint_attr->position[r][c];
            int cw, chh;

            if (ch >= FS_CHNS) {
                free(j);
                return E_FS_BUSY;
            }
            if (OpenIMP_T32_FsChnSize((int)ch, &cw, &chh) == 0) {
                rw += cw;
                if (chh > rh)
                    rh = chh;
            }
        }
        if (rw > w)
            w = rw;
        h += rh;
    }
    pthread_mutex_lock(&ext_lock);
    for (slot = 0; slot < JOINTS && joints[slot]; slot++)
        ;
    if (slot == JOINTS) {
        pthread_mutex_unlock(&ext_lock);
        free(j);
        return E_FS_NOMEM;
    }
    memcpy(j->attr.position, joint_attr->position, sizeof(j->attr.position));
    j->attr.width = (int16_t)w;
    j->attr.height = (int16_t)h;
    j->attr.handler = (int32_t)(intptr_t)j;
    sem_init(&j->sem, 0, 0);
    joints[slot] = j;
    pthread_mutex_unlock(&ext_lock);
    joint_attr->width = (int16_t)w;
    joint_attr->height = (int16_t)h;
    joint_attr->handler = j->attr.handler;
    return 0;
}

int IMP_FrameSource_DestroyJoint(int32_t handler)
{
    struct fs_joint *j;
    int i;

    pthread_mutex_lock(&ext_lock);
    i = joint_find_locked(handler);
    if (i < 0) {
        pthread_mutex_unlock(&ext_lock);
        return E_FS_NULL;
    }
    j = joints[i];
    joints[i] = NULL;
    pthread_mutex_unlock(&ext_lock);
    sem_destroy(&j->sem);
    free(j);
    return 0;
}

int IMP_FrameSource_GetJointByHandler(IMPFSJointAttr *joint_attr)
{
    int i;

    if (!joint_attr)
        return E_FS_NULL;
    pthread_mutex_lock(&ext_lock);
    i = joint_find_locked(joint_attr->handler);
    if (i >= 0)
        *joint_attr = joints[i]->attr;
    pthread_mutex_unlock(&ext_lock);
    return i < 0 ? E_FS_NULL : 0;
}

int IMP_FrameSource_SetJointBackground(uint32_t color)
{
    pthread_mutex_lock(&ext_lock);
    joint_bg = color;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

/* ============================================================ Encoder == */
static struct {
    int fisheye, rdbufshare, motion_quality, qpg_ai, resize;
    IMPEncoderAttrFrmUsed frm_used;
} enc_ext[ENC_CHNS];
static struct {
    uint32_t ivpu_bs_size;
    int ms_on, ms_size, ms_cnt;          /* SetMultiSectionMode */
    int jms_on, jms_size;                /* SetJpegMultiSectionMode */
    int qpg_ai_yuv, mq_yuv, mq_yuv_mode;
} enc_glob;

/* channel number check of the per-channel calls: >= 16 (or negative) 0x80040001 */
#define ENC_CHN_OK(n) ((unsigned)(n) < ENC_CHNS)

/* setting that only counts before CreateChn: stored while the channel does not exist */
static int enc_pre_create(int encChn, int *field, int value, int busy_code)
{
    if (!ENC_CHN_OK(encChn))
        return E_ENC_CHN;
    if (OpenIMP_T32_EncChnCreated(encChn))
        return busy_code;
    pthread_mutex_lock(&ext_lock);
    *field = value != 0;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_Encoder_SetFisheyeEnableStatus(int encChn, int enable)
{
    return enc_pre_create(encChn, ENC_CHN_OK(encChn) ? &enc_ext[encChn].fisheye : NULL, enable, E_ENC_STATE);
}

int IMP_Encoder_GetFisheyeEnableStatus(int encChn, int *enable)
{
    if (!ENC_CHN_OK(encChn))
        return E_ENC_CHN;
    if (!OpenIMP_T32_EncChnCreated(encChn))
        return E_ENC_NOCHN;
    if (!enable)
        return E_ENC_NULL;
    pthread_mutex_lock(&ext_lock);
    *enable = enc_ext[encChn].fisheye;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_Encoder_SetRdBufShare(int encChn, int bEnable)
{
    return enc_pre_create(encChn, ENC_CHN_OK(encChn) ? &enc_ext[encChn].rdbufshare : NULL, bEnable, E_ENC_STATE);
}

int IMP_Encoder_SetMotionQualityEnable(int encChn, int enable)
{
    return enc_pre_create(encChn, ENC_CHN_OK(encChn) ? &enc_ext[encChn].motion_quality : NULL, enable, E_ENC_CRE4);
}

/* the two pre-create switches for openimp_t32_enc_e1.inc (#480) */
int OpenIMP_T32_EncMotionQualityOn(int encChn)
{
    int on;

    if (!ENC_CHN_OK(encChn))
        return 0;
    pthread_mutex_lock(&ext_lock);
    on = enc_ext[encChn].motion_quality;
    pthread_mutex_unlock(&ext_lock);
    return on;
}

int OpenIMP_T32_EncQpgAIOn(int encChn)
{
    int on;

    if (!ENC_CHN_OK(encChn))
        return 0;
    pthread_mutex_lock(&ext_lock);
    on = enc_ext[encChn].qpg_ai;
    pthread_mutex_unlock(&ext_lock);
    return on;
}

int IMP_Encoder_SetChnQpgAIEnable(int encChn, int enable)
{
    return enc_pre_create(encChn, ENC_CHN_OK(encChn) ? &enc_ext[encChn].qpg_ai : NULL, enable, E_ENC_CRE4);
}

int IMP_Encoder_SetChnResizeMode(int encChn, int en)
{
    if (!ENC_CHN_OK(encChn))
        return E_ENC_CHN;
    if (!OpenIMP_T32_EncChnCreated(encChn))
        return E_ENC_NOCHN;
    pthread_mutex_lock(&ext_lock);
    enc_ext[encChn].resize = en != 0;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_Encoder_SetChnFrmUsedMode(int encChn, const IMPEncoderAttrFrmUsed *pfrmUsedAttr)
{
    if (!ENC_CHN_OK(encChn))
        return E_ENC_CHN;
    if (!pfrmUsedAttr)
        return E_ENC_NULL;
    if (!OpenIMP_T32_EncChnCreated(encChn))
        return E_ENC_NOCHN;
    pthread_mutex_lock(&ext_lock);
    enc_ext[encChn].frm_used = *pfrmUsedAttr;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_Encoder_GetChnFrmUsedMode(int encChn, IMPEncoderAttrFrmUsed *pfrmUsedAttr)
{
    if (!ENC_CHN_OK(encChn))
        return E_ENC_CHN;
    if (!pfrmUsedAttr)
        return E_ENC_NULL;
    if (!OpenIMP_T32_EncChnCreated(encChn))
        return E_ENC_NOCHN;
    pthread_mutex_lock(&ext_lock);
    *pfrmUsedAttr = enc_ext[encChn].frm_used;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

/* global settings: only before IMP_System_Init brings the encoder module up */
int IMP_Encoder_SetIvpuBsSize(uint32_t ivpuBsSize)
{
    if (OpenIMP_T32_SystemInitDone())
        return E_ENC_STATE;
    if (!ivpuBsSize)
        return E_ENC_PARAM;
    enc_glob.ivpu_bs_size = ivpuBsSize;
    return 0;
}

int IMP_Encoder_SetMultiSectionMode(int mode, int size, int cnt)
{
    if (OpenIMP_T32_SystemInitDone())
        return E_ENC_STATE;
    if ((unsigned)mode >= 4)
        return E_ENC_PARAM;
    pthread_mutex_lock(&ext_lock);
    enc_glob.ms_on = mode != 0;
    enc_glob.ms_size = size;
    enc_glob.ms_cnt = cnt < 2 ? 2 : cnt;        /* the vendor keeps at least two sections */
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

int IMP_Encoder_SetJpegMultiSectionMode(int mode, int size, int cnt)
{
    (void)cnt;
    if (OpenIMP_T32_SystemInitDone())
        return E_ENC_STATE;
    if ((unsigned)mode >= 4)
        return E_ENC_PARAM;
    pthread_mutex_lock(&ext_lock);
    enc_glob.jms_on = mode != 0;
    enc_glob.jms_size = size;
    pthread_mutex_unlock(&ext_lock);
    return 0;
}

/* the YUV (unbound) encoder flags: void / 0, stored only */
void IMP_Encoder_SetChnQpgAIEnable_Yuv(int encChn, int enable)
{
    (void)encChn;
    enc_glob.qpg_ai_yuv = enable != 0;
}

void IMP_Encoder_SetMotionQualityEnable_Yuv(int encChn, int enable)
{
    (void)encChn;
    enc_glob.mq_yuv = enable != 0;
}

int IMP_Encoder_SetMotionQuality_Yuv(int *mode)
{
    if (mode && *mode == 2)
        enc_glob.mq_yuv_mode = 3;
    return 0;
}

/* table remap of a map: in = pointer to {uint8_t *map, uint32_t len}, out = {uint8_t *table,
 * uint32_t len (<= 16)}; every map byte (<= 16) is replaced by its table entry */
struct enc_map { uint8_t *map; uint32_t len; };

int IMP_Encoder_MappingTransform(void *in, int *out)
{
    struct enc_map *m, *t = (struct enc_map *)out;
    uint32_t i;

    if (!out)
        return E_ENC_NULL;
    if (t->len > 16)
        return E_ENC_PARAM;
    m = in ? *(struct enc_map **)in : NULL;
    if (!m)
        return E_ENC_NULL;
    for (i = 0; i < m->len; i++) {
        if (m->map[i] > 16) {
            IMP_LOG_ERR(TAG, "MappingTransform: map entry %u = %u (0..16)", i, m->map[i]);
            return E_ENC_PARAM;
        }
        m->map[i] = t->map[m->map[i]];
    }
    return 0;
}

int IMP_Encoder_ClearPoolId(void)
{
    return 0;
}

/* ---- rmem helpers (16 entries as the vendor) ---------------------------- */
#define VBM_SLOTS 16
static struct {
    uint32_t phys;              /* the allocation */
    void *virt;                 /* the aligned address handed out */
} vbm[VBM_SLOTS];

void *IMP_Encoder_VbmAlloc(uint32_t size, uint32_t align)
{
    IMPDMABufferInfo info;
    uintptr_t v;
    int i;

    if (!size || (align & (align - 1u)))
        return NULL;
    pthread_mutex_lock(&ext_lock);
    for (i = 0; i < VBM_SLOTS && vbm[i].virt; i++)
        ;
    if (i == VBM_SLOTS) {
        pthread_mutex_unlock(&ext_lock);
        IMP_LOG_ERR(TAG, "VbmAlloc: all %d slots used", VBM_SLOTS);
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    if (DMA_AllocDescriptor(&info, (int)(size + align), "vbm") != 0 || !info.virt_addr) {
        pthread_mutex_unlock(&ext_lock);
        IMP_LOG_ERR(TAG, "VbmAlloc: %u bytes: no memory", size);
        return NULL;
    }
    v = info.virt_addr;
    if (align)
        v = (v + align - 1u) & ~(uintptr_t)(align - 1u);
    vbm[i].phys = info.phys_addr;
    vbm[i].virt = (void *)v;
    pthread_mutex_unlock(&ext_lock);
    return (void *)v;
}

void IMP_Encoder_VbmFree(void *vaddr)
{
    int i;

    if (!vaddr)
        return;
    pthread_mutex_lock(&ext_lock);
    for (i = 0; i < VBM_SLOTS; i++)
        if (vbm[i].virt == vaddr) {
            DMA_FreePhys(vbm[i].phys);
            vbm[i].virt = NULL;
            vbm[i].phys = 0;
            break;
        }
    pthread_mutex_unlock(&ext_lock);
}

intptr_t IMP_Encoder_VbmV2P(intptr_t vaddr)
{
    return (intptr_t)DMA_VirtToPhys((const void *)vaddr);
}

intptr_t IMP_Encoder_VbmP2V(intptr_t paddr)
{
    return (intptr_t)DMA_PhysToVirt((uint32_t)paddr);
}
