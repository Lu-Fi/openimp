/*
 * OpenIMP T32 ISP-drawn OSD (#336, planned; open-tx-isp driver/t32 draws it
 * through the MSCA OSD windows, masks and lines).
 *
 * Two parts, as in the vendor 2.2.0 library:
 *
 * 1. Picture regions (isp_osd.h, IMP_OSD_*_ISP and the IMP_ISP_Tuning_*OsdRgn
 *    wrappers).  Each sensor channel (0..4) has 8 region slots; a slot's
 *    picture is copied into one rmem pool (IMP_ISP_Tuning_SetOsdPoolSize,
 *    allocated by IMP_System_Init) and handed to the ISP with
 *    IMP_ISP_Tuning_SetOSDAttr + IMP_ISP_Tuning_SetOSDBlock (isp-m0 tuning ids
 *    0x080000a1 / 0x080000a2, openimp_t32_tuning.c).  The vendor flow kept:
 *    - CreateRgn takes the slot at the head of the channel's free list (a
 *      destroyed slot goes to its tail) and appends it to the channel's list;
 *      its picture number is 9 (shown nowhere) until SetRgnAttr;
 *    - SetRgnAttr_PicISP checks the window against FrameSource channel
 *      chn * 3, copies the attribute and the picture (stride * height bytes,
 *      a 4-byte multiple; zeros for a NULL image) and clears osd_enable; it
 *      sends nothing to the driver;
 *    - ShowRgn_ISP sets osd_enable, hides any later shown slot of the same
 *      picture group (pinum 0-3 / 4-7) whose window touches this one, then
 *      sends every slot of the channel whose pinum equals its handle, in list
 *      order: SetOSDAttr(group format) and SetOSDBlock(window, physical
 *      address of the pool copy);
 *    - DestroyRgn_ISP hides a shown slot first (ShowRgn 0), then frees it;
 *    - an out-of-range handle returns 0 (and does nothing), as the vendor's.
 *
 * 2. Line / rectangle and cover regions (imp_osd.h IMP_OSD_SetRgnAttr_ISP /
 *    GetRgnAttr_ISP): OSD_REG_ISP_LINE_RECT goes to IMP_ISP_Tuning_SetDrawBlock
 *    (id 0x080000a0), OSD_REG_ISP_COVER to IMP_ISP_Tuning_SetMaskBlock (id
 *    0x08000074), OSD_REG_ISP_PIC is refused; the show flag is only stored.
 *    The getter reads the draw block, the OSD attribute (into the picture
 *    block's place, 32 bytes, as the vendor) and the mask block.
 *
 * Request sequence and payloads equal to vendor libimp 2.2.0 in QEMU
 * (tools/t32/t32_trace_sample.c isposd, #336), addresses aside.  Built
 * against the vendor T32 2.2.0 headers (T32_HEADERS) like the tuning layer.
 * No vendor code copied.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>
/* isp_osd.h prototypes (that header pulls in imp_log.h, whose IMP_LOG
 * macros clash with OpenIMP's imp_log_int.h) */
int IMP_OSD_Init_ISP(void);
int IMP_OSD_SetPoolSize_ISP(int size);
int IMP_OSD_CreateRgn_ISP(int chn, IMPIspOsdAttrAsm *pIspOsdAttr);
int IMP_OSD_SetRgnAttr_PicISP(int chn, int handle, IMPIspOsdAttrAsm *pIspOsdAttr);
int IMP_OSD_GetRgnAttr_ISPPic(int chn, int handle, IMPIspOsdAttrAsm *pIspOsdAttr);
int IMP_OSD_ShowRgn_ISP(int chn, int handle, int showFlag);
int IMP_OSD_DestroyRgn_ISP(int chn, int handle);
void IMP_OSD_Exit_ISP(void);
int IMP_OSD_UpdateRgnAttrData_ISP(int chn, int handle, IMPIspOsdAttrAsm *attr);

#include "dma_alloc.h"
#include "imp_log_int.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_isp_osd.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ISPOSD"

#define ISPOSD_CHNS     5
#define ISPOSD_PICS     8
#define ISPOSD_HIDDEN   9       /* pinum of a slot that has no picture yet */
#define ISPOSD_POOL_MAX 64      /* pool blocks */

#define E_SENSOR ((int)0x80020001)
#define E_NOINIT ((int)0x80020008)
#define E_NULL   ((int)0x80020010)
#define E_PARAM  ((int)0x80020040)

_Static_assert(sizeof(IMPIspOsdAttrAsm) == 56, "T32 IMPIspOsdAttrAsm");
_Static_assert(sizeof(IMPISPOSDAttr) == 32, "T32 IMPISPOSDAttr");
_Static_assert(sizeof(IMPISPOSDBlockAttr) == 20, "T32 IMPISPOSDBlockAttr");
_Static_assert(sizeof(IMPOSDRgnAttr) == 272, "T32 IMPOSDRgnAttr");
_Static_assert(__builtin_offsetof(IMPOSDRgnAttr, osdispdraw) == 48, "T32 osdispdraw");

typedef struct IspOsdSlot {
    int chn;
    int handle;
    int refs;
    int show;
    int busy;
    struct IspOsdSlot *prev, *next;
    IMPIspOsdAttrAsm attr;      /* pic.osd_image: the pool copy (virtual) */
} IspOsdSlot;

typedef struct {
    IspOsdSlot *head, *tail;
} IspOsdList;

typedef struct {
    uint32_t off, size;
} IspOsdBlock;

static pthread_mutex_t isp_osd_lock = PTHREAD_MUTEX_INITIALIZER;
static int isp_osd_pool_size = 1;               /* vendor default; samples set 512 KiB */
static int isp_osd_ready;
static int isp_osd_show;                        /* IMP_OSD_SetRgnAttr_ISP's flag */
static int isp_osd_npics;
static IspOsdSlot isp_osd_slot[ISPOSD_CHNS][ISPOSD_PICS];
static IspOsdList isp_osd_free[ISPOSD_CHNS], isp_osd_used[ISPOSD_CHNS];
static IMPDMABufferInfo isp_osd_pool;
static IspOsdBlock isp_osd_blk[ISPOSD_POOL_MAX];
static int isp_osd_nblk;

/* ---- the picture pool: first fit over the blocks in address order ------ */
static uint8_t *pool_alloc(uint32_t size)
{
    uint32_t at = 0;
    int i;

    if (size & 3 || isp_osd_nblk >= ISPOSD_POOL_MAX)
        return NULL;
    for (i = 0; i <= isp_osd_nblk; i++) {
        uint32_t end = i < isp_osd_nblk ? isp_osd_blk[i].off : isp_osd_pool.size;

        if (end >= at && end - at >= size) {
            memmove(&isp_osd_blk[i + 1], &isp_osd_blk[i], (size_t)(isp_osd_nblk - i) * sizeof(isp_osd_blk[0]));
            isp_osd_blk[i].off = at;
            isp_osd_blk[i].size = size;
            isp_osd_nblk++;
            return (uint8_t *)(uintptr_t)isp_osd_pool.virt_addr + at;
        }
        if (i < isp_osd_nblk)
            at = isp_osd_blk[i].off + isp_osd_blk[i].size;
    }
    return NULL;
}

static int pool_free(const void *p)
{
    uint32_t off = (uint32_t)((uintptr_t)p - isp_osd_pool.virt_addr);
    int i;

    for (i = 0; i < isp_osd_nblk; i++) {
        if (isp_osd_blk[i].off == off) {
            memmove(&isp_osd_blk[i], &isp_osd_blk[i + 1], (size_t)(isp_osd_nblk - i - 1) * sizeof(isp_osd_blk[0]));
            isp_osd_nblk--;
            return 0;
        }
    }
    return -1;
}

static uint32_t pool_phys(const void *p)
{
    return (uint32_t)((uintptr_t)p - isp_osd_pool.virt_addr) + isp_osd_pool.phys_addr;
}

/* ---- slot lists ----------------------------------------------------------- */
static void list_append(IspOsdList *l, IspOsdSlot *s)
{
    s->next = NULL;
    s->prev = l->tail;
    if (l->tail)
        l->tail->next = s;
    else
        l->head = s;
    l->tail = s;
}

static void list_unlink(IspOsdList *l, IspOsdSlot *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        l->head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    else
        l->tail = s->prev;
    s->prev = s->next = NULL;
}

static int handle_bad(int handle)
{
    return handle < 0 || handle >= isp_osd_npics;
}

/* the pictures of a channel's list to the ISP (vendor OsdnotifyIsp) */
static void notify_isp(IspOsdSlot *s)
{
    for (; s; s = s->next) {
        IMPISPOSDSingleAttr *a = &s->attr.stsinglepicAttr;
        char *virt = a->pic.osd_image;

        if (s->handle != a->pic.pinum)
            continue;
        a->pic.osd_image = (char *)(uintptr_t)pool_phys(virt);
        if (IMP_ISP_Tuning_SetOSDAttr((IMPVI_NUM)s->chn, &a->chnOSDAttr) < 0) {
            IMP_LOG_ERR(TAG, "SetOSDAttr failed (handle %d)", s->handle);
            a->pic.osd_image = virt;
            return;
        }
        if (IMP_ISP_Tuning_SetOSDBlock((IMPVI_NUM)s->chn, &a->pic) < 0) {
            IMP_LOG_ERR(TAG, "SetOSDBlock failed (handle %d)", s->handle);
            a->pic.osd_image = virt;
            return;
        }
        a->pic.osd_image = virt;
    }
}

/* a shown picture hides the later shown ones of its group that it touches
 * (vendor IspOsdAdjust_Pic; 16-bit window arithmetic as there) */
static void adjust_pics(IspOsdSlot *s)
{
    for (; s; s = s->next) {
        const IMPISPOSDBlockAttr *p = &s->attr.stsinglepicAttr.pic;
        IspOsdSlot *o;

        if (!p->osd_enable)
            continue;
        if (!s->next)
            return;
        for (o = s->next; o; o = o->next) {
            IMPISPOSDBlockAttr *q = &o->attr.stsinglepicAttr.pic;
            uint16_t lo, hi;

            if (!q->osd_enable || (p->pinum >= 4) != (q->pinum >= 4) || p->pinum == q->pinum)
                continue;
            lo = p->osd_left > q->osd_left ? p->osd_left : q->osd_left;
            hi = (uint16_t)(p->osd_left + p->osd_width);
            if ((uint16_t)(q->osd_left + q->osd_width) < hi)
                hi = (uint16_t)(q->osd_left + q->osd_width);
            if (hi < lo)
                continue;
            lo = p->osd_top > q->osd_top ? p->osd_top : q->osd_top;
            hi = (uint16_t)(p->osd_top + p->osd_height);
            if ((uint16_t)(q->osd_top + q->osd_height) < hi)
                hi = (uint16_t)(q->osd_top + q->osd_height);
            if (hi < lo)
                continue;
            IMP_LOG_WARN(TAG, "handle %d overlaps handle %d of its group: hidden", o->handle, s->handle);
            q->osd_enable = 0;
            o->show = 0;
        }
    }
}

/* ---- region API ---------------------------------------------------------- */
int IMP_OSD_SetPoolSize_ISP(int size)
{
    if (size < 1) {
        IMP_LOG_ERR(TAG, "pool size %d", size);
        return -1;
    }
    isp_osd_pool_size = size;
    return 0;
}

int IMP_OSD_Init_ISP(void)
{
    int c, h;

    pthread_mutex_lock(&isp_osd_lock);
    if (isp_osd_ready) {
        pthread_mutex_unlock(&isp_osd_lock);
        return 0;
    }
    memset(&isp_osd_pool, 0, sizeof(isp_osd_pool));
    if (DMA_AllocDescriptor(&isp_osd_pool, isp_osd_pool_size, "IspOsdMem") < 0) {
        IMP_LOG_ERR(TAG, "pool of %d bytes not allocated", isp_osd_pool_size);
        pthread_mutex_unlock(&isp_osd_lock);
        return -1;
    }
    isp_osd_pool.size = (uint32_t)isp_osd_pool_size;
    isp_osd_nblk = 0;
    isp_osd_npics = ISPOSD_PICS;
    memset(isp_osd_slot, 0, sizeof(isp_osd_slot));
    for (c = 0; c < ISPOSD_CHNS; c++) {
        isp_osd_free[c].head = isp_osd_free[c].tail = NULL;
        isp_osd_used[c].head = isp_osd_used[c].tail = NULL;
        for (h = 0; h < isp_osd_npics; h++) {
            isp_osd_slot[c][h].handle = h;
            list_append(&isp_osd_free[c], &isp_osd_slot[c][h]);
        }
    }
    isp_osd_ready = 1;
    pthread_mutex_unlock(&isp_osd_lock);
    return 0;
}

void IMP_OSD_Exit_ISP(void)
{
    pthread_mutex_lock(&isp_osd_lock);
    if (isp_osd_ready) {
        DMA_FreePhys(isp_osd_pool.phys_addr);
        memset(&isp_osd_pool, 0, sizeof(isp_osd_pool));
        isp_osd_ready = 0;
    }
    pthread_mutex_unlock(&isp_osd_lock);
}

int IMP_OSD_CreateRgn_ISP(int chn, IMPIspOsdAttrAsm *attr)
{
    IspOsdSlot *s;

    if ((unsigned)chn >= ISPOSD_CHNS) {
        IMP_LOG_ERR(TAG, "CreateRgn: channel %d", chn);
        return -1;
    }
    if (attr) {
        IMP_LOG_ERR(TAG, "CreateRgn: an attribute is not supported, pass NULL");
        return -1;
    }
    pthread_mutex_lock(&isp_osd_lock);
    if (!isp_osd_ready || !(s = isp_osd_free[chn].head)) {
        pthread_mutex_unlock(&isp_osd_lock);
        IMP_LOG_ERR(TAG, "CreateRgn: %s", isp_osd_ready ? "no free region" : "not initialised");
        return -1;
    }
    list_unlink(&isp_osd_free[chn], s);
    list_append(&isp_osd_used[chn], s);
    s->busy = 1;
    s->refs++;
    s->attr.stsinglepicAttr.pic.pinum = ISPOSD_HIDDEN;
    pthread_mutex_unlock(&isp_osd_lock);
    return s->handle;
}

int IMP_OSD_SetRgnAttr_PicISP(int chn, int handle, IMPIspOsdAttrAsm *attr)
{
    IMPFSChnAttr fs;
    IspOsdSlot *s;
    const IMPISPOSDBlockAttr *p;
    uint32_t old, size;
    uint8_t *buf;

    if (!isp_osd_ready)
        return -1;
    if (handle_bad(handle))
        return 0;
    if ((unsigned)chn >= ISPOSD_CHNS || !attr) {
        IMP_LOG_ERR(TAG, "SetRgnAttr: channel %d / attribute %p", chn, (void *)attr);
        return -1;
    }
    if (IMP_FrameSource_GetChnAttr(chn * 3, &fs) < 0) {
        IMP_LOG_ERR(TAG, "SetRgnAttr: no frame parameters of channel %d", chn * 3);
        return -1;
    }
    p = &attr->stsinglepicAttr.pic;
    if ((uint32_t)fs.picHeight < (uint32_t)p->osd_top + p->osd_height ||
        (uint32_t)fs.picWidth < (uint32_t)p->osd_left + p->osd_width) {
        IMP_LOG_ERR(TAG, "SetRgnAttr: window %u,%u %ux%u outside the frame %dx%d", p->osd_left,
                    p->osd_top, p->osd_width, p->osd_height, fs.picWidth, fs.picHeight);
        return -1;
    }
    s = &isp_osd_slot[chn][handle];
    if (!s->busy) {
        IMP_LOG_ERR(TAG, "SetRgnAttr: channel %d region %d not created", chn, handle);
        return -1;
    }
    pthread_mutex_lock(&isp_osd_lock);
    old = (uint32_t)s->attr.stsinglepicAttr.pic.osd_stride * s->attr.stsinglepicAttr.pic.osd_height;
    size = (uint32_t)p->osd_stride * p->osd_height;
    if (!size || attr->type != ISP_OSD_REG_PIC) {
        pthread_mutex_unlock(&isp_osd_lock);
        IMP_LOG_ERR(TAG, "SetRgnAttr: type %d size %u", (int)attr->type, size);
        return -1;
    }
    buf = (uint8_t *)s->attr.stsinglepicAttr.pic.osd_image;
    if (old != size) {
        if (old || !(buf = pool_alloc(size))) {
            pthread_mutex_unlock(&isp_osd_lock);
            IMP_LOG_ERR(TAG, "SetRgnAttr: picture of %u bytes (had %u): %s", size, old,
                        old ? "destroy and recreate the region to change its size" : "no pool space");
            return -1;
        }
    }
    s->attr = *attr;
    if (p->osd_image)
        memcpy(buf, p->osd_image, size);
    else
        memset(buf, 0, size);
    s->attr.stsinglepicAttr.pic.osd_enable = 0;
    s->attr.stsinglepicAttr.pic.osd_image = (char *)buf;
    pthread_mutex_unlock(&isp_osd_lock);
    return 0;
}

int IMP_OSD_GetRgnAttr_ISPPic(int chn, int handle, IMPIspOsdAttrAsm *attr)
{
    if (!isp_osd_ready)
        return -1;
    if (handle_bad(handle))
        return 0;
    if ((unsigned)chn >= ISPOSD_CHNS || !attr)
        return -1;
    *attr = isp_osd_slot[chn][handle].attr;
    return 0;
}

int IMP_OSD_ShowRgn_ISP(int chn, int handle, int showFlag)
{
    IspOsdSlot *s;

    if (!isp_osd_ready)
        return -1;
    if (handle_bad(handle))
        return 0;
    if ((unsigned)chn >= ISPOSD_CHNS) {
        IMP_LOG_ERR(TAG, "ShowRgn: channel %d", chn);
        return -1;
    }
    s = &isp_osd_slot[chn][handle];
    if (!s->busy) {
        IMP_LOG_ERR(TAG, "ShowRgn: channel %d region %d not created", chn, handle);
        return -1;
    }
    pthread_mutex_lock(&isp_osd_lock);
    s->attr.stsinglepicAttr.pic.osd_enable = (uint8_t)showFlag;
    s->show = (uint8_t)showFlag;
    s->chn = chn;
    adjust_pics(isp_osd_used[chn].head);
    notify_isp(isp_osd_used[chn].head);
    pthread_mutex_unlock(&isp_osd_lock);
    return 0;
}

int IMP_OSD_UpdateRgnAttrData_ISP(int chn, int handle, IMPIspOsdAttrAsm *attr)
{
    IMPISPOSDSingleAttr *a;
    IspOsdSlot *s;
    uint32_t size;
    char *virt;
    int rc;

    if ((unsigned)chn >= ISPOSD_CHNS || !isp_osd_ready)
        return -1;
    if (handle_bad(handle))
        return 0;
    if (!attr)
        return -1;
    s = &isp_osd_slot[chn][handle];
    if (!s->busy)
        return -1;
    pthread_mutex_lock(&isp_osd_lock);
    a = &s->attr.stsinglepicAttr;
    size = (uint32_t)attr->stsinglepicAttr.pic.osd_stride * attr->stsinglepicAttr.pic.osd_height;
    if (!size || size != (uint32_t)a->pic.osd_stride * a->pic.osd_height) {
        pthread_mutex_unlock(&isp_osd_lock);
        IMP_LOG_ERR(TAG, "UpdateRgnAttrData: size %u, region has %u", size,
                    (uint32_t)a->pic.osd_stride * a->pic.osd_height);
        return -1;
    }
    memcpy(a->pic.osd_image, attr->stsinglepicAttr.pic.osd_image, size);
    /* as the vendor: the group format again (the window keeps its address) */
    virt = a->pic.osd_image;
    a->pic.osd_image = (char *)(uintptr_t)pool_phys(virt);
    rc = IMP_ISP_Tuning_SetOSDAttr((IMPVI_NUM)s->chn, &a->chnOSDAttr);
    a->pic.osd_image = virt;
    pthread_mutex_unlock(&isp_osd_lock);
    return rc < 0 ? -1 : 0;
}

int IMP_OSD_DestroyRgn_ISP(int chn, int handle)
{
    IspOsdSlot *s;
    int rc = 0;

    if ((unsigned)chn >= ISPOSD_CHNS || !isp_osd_ready)
        return -1;
    if (handle_bad(handle))
        return 0;
    s = &isp_osd_slot[chn][handle];
    if (s->attr.stsinglepicAttr.pic.osd_enable)
        IMP_OSD_ShowRgn_ISP(chn, handle, 0);
    pthread_mutex_lock(&isp_osd_lock);
    if (s->busy == 1 && s->refs == 1) {
        list_unlink(&isp_osd_used[chn], s);
        list_append(&isp_osd_free[chn], s);
        s->busy = 0;
        s->refs--;
        if (s->attr.stsinglepicAttr.pic.osd_image && pool_free(s->attr.stsinglepicAttr.pic.osd_image) < 0)
            rc = -1;
        else
            memset(&s->attr, 0, sizeof(s->attr));
    }
    pthread_mutex_unlock(&isp_osd_lock);
    return rc;
}

/* IMP_ISP_Tuning_*OsdRgn: the vendor's tail calls */
int IMP_ISP_Tuning_SetOsdPoolSize(int size)
{
    return IMP_OSD_SetPoolSize_ISP(size);
}

int IMP_ISP_Tuning_CreateOsdRgn(int chn, IMPIspOsdAttrAsm *pIspOsdAttr)
{
    return IMP_OSD_CreateRgn_ISP(chn, pIspOsdAttr);
}

int IMP_ISP_Tuning_SetOsdRgnAttr(int chn, int handle, IMPIspOsdAttrAsm *pIspOsdAttr)
{
    return IMP_OSD_SetRgnAttr_PicISP(chn, handle, pIspOsdAttr);
}

int IMP_ISP_Tuning_GetOsdRgnAttr(int chn, int handle, IMPIspOsdAttrAsm *pIspOsdAttr)
{
    return IMP_OSD_GetRgnAttr_ISPPic(chn, handle, pIspOsdAttr);
}

int IMP_ISP_Tuning_ShowOsdRgn(int chn, int handle, int showFlag)
{
    return IMP_OSD_ShowRgn_ISP(chn, handle, showFlag);
}

int IMP_ISP_Tuning_DestroyOsdRgn(int chn, int handle)
{
    return IMP_OSD_DestroyRgn_ISP(chn, handle);
}

/* ---- line / rectangle and cover regions ---------------------------------- */
int IMP_OSD_SetRgnAttr_ISP(int sensornum, IMPOSDRgnAttr *prAttr, int bosdshow)
{
    int rc = 0;

    if ((unsigned)sensornum >= ISPOSD_CHNS)
        return E_SENSOR;
    if (!prAttr)
        return E_NULL;
    if (prAttr->type != OSD_REG_ISP_PIC && prAttr->type != OSD_REG_ISP_LINE_RECT &&
        prAttr->type != OSD_REG_ISP_COVER) {
        IMP_LOG_ERR(TAG, "SetRgnAttr_ISP: type %d is not an ISP region", (int)prAttr->type);
        return E_PARAM;
    }
    if (!isp_osd_ready)
        return E_NOINIT;
    pthread_mutex_lock(&isp_osd_lock);
    isp_osd_show = bosdshow != 0;
    if (prAttr->type == OSD_REG_ISP_LINE_RECT) {
        if (IMP_ISP_Tuning_SetDrawBlock((IMPVI_NUM)sensornum, &prAttr->osdispdraw.stDrawAttr) < 0)
            rc = E_PARAM;
    } else if (prAttr->type == OSD_REG_ISP_PIC) {
        IMP_LOG_ERR(TAG, "SetRgnAttr_ISP: pictures go through IMP_OSD_SetRgnAttr_PicISP");
        rc = E_PARAM;
    } else if (IMP_ISP_Tuning_SetMaskBlock((IMPVI_NUM)sensornum, &prAttr->osdispdraw.stCoverAttr) < 0) {
        rc = -1;
    }
    pthread_mutex_unlock(&isp_osd_lock);
    return rc;
}

int IMP_OSD_GetRgnAttr_ISP(int sensornum, IMPOSDRgnAttr *prAttr, int *pbosdshow)
{
    if ((unsigned)sensornum >= ISPOSD_CHNS)
        return E_SENSOR;
    if (!prAttr || !pbosdshow)
        return E_NULL;
    if (!isp_osd_ready)
        return E_NOINIT;
    *pbosdshow = isp_osd_show;
    if (IMP_ISP_Tuning_GetDrawBlock((IMPVI_NUM)sensornum, &prAttr->osdispdraw.stDrawAttr) < 0)
        return E_PARAM;
    /* the vendor reads the OSD attribute (32 B) into the picture block's place */
    if (IMP_ISP_Tuning_GetOSDAttr((IMPVI_NUM)sensornum, (IMPISPOSDAttr *)(void *)&prAttr->osdispdraw.stpicAttr) < 0)
        return E_PARAM;
    if (IMP_ISP_Tuning_GetMaskBlock((IMPVI_NUM)sensornum, &prAttr->osdispdraw.stCoverAttr) < 0)
        return E_PARAM;
    return 0;
}
