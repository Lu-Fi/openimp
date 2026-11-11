/*
 * OpenIMP T32 encoder: JPEG on the JPEG v1.2 engine (#292), H.264 (#293)
 * and H.265 (#294) on HERA (planned, not release gates).  T23-family API
 * with the T32 2.x layouts (imp_encoder.h: IMPEncoderCHNAttr 184 bytes,
 * profile type byte JPEG 0 / AVC 1 / HEVC 2).
 *
 * Flow (as vendor libimp 2.2.0 in the QEMU trace, own code):
 *   CreateGroup        registers the group as a FrameSource sink (bind target)
 *   CreateChn          soc_vpu CHANNEL_REQ (codec JPEGENC), job + stream buffers
 *   frame (sink)       a receiving channel of the bound group keeps the frame;
 *                      the channel thread builds the jpeg_reg_info, FLUSH_CACHE
 *                      (write back the job), CHANNEL_RUN on helix0, FLUSH_CACHE
 *                      (invalidate the bitstream), gives the frame back
 *   GetStream          one pack: the engine's file (SOI .. EOI, its own DQT/
 *                      DHT/SOF0/SOS; no APP0), as the vendor hands it over
 *   DestroyChn         CHANNEL_REL
 *
 * Register image (vendor values in tests/t32/vendor-2.2.0-jpeg-job.txt):
 * base tables = ITU-T T.81 Annex K luma / chroma in zigzag order, quality
 * index = 98 - jpegQp (IMP_Encoder_SetJpegQp range 0..98; an initial QP
 * outside it, e.g. SetDefaultParam's -1, gives jpegQp 40 as the vendor
 * does), limit = stream buffer size, frame-end + over-limit interrupts.
 * A picture over the limit is coded again with a lower quality (index + 8)
 * instead of the vendor's assert at the end of its range.
 *
 * H.264 (vendor 2.2.0's flow in the QEMU trace, own code):
 *   CreateChn          CHANNEL_REQ (H264ENC); command list, reconstruction ring,
 *                      analysis outputs, a 2 x 250 KiB engine stream buffer,
 *                      stream slots; SPS / PPS (openimp_t32_h264.c)
 *   frame              the picture's command list (openimp_t32_hera.c),
 *                      FLUSH_CACHE (write back), CHANNEL_START on radix0, then
 *                      CHANNEL_WAIT_BSFULL_COMPLETE: each full 250 KiB half
 *                      (VPU_RET_HERA_BSFULL) is escaped into the slot and
 *                      CHANNEL_SET_BSFULL_REG lets the engine go on, the end
 *                      of the picture gives the last part; the engine's 48
 *                      statistics registers are read back (WOR_VPU_REG) as the
 *                      vendor does and fed to the rate control (#295)
 *   GetStream          IDR: SPS, PPS, IDR slice (3 packs); P: the slice
 * GOP = rcAttr.maxGop, I then P with one reference; one QP a picture (FIXQP
 * IQp / PQp; CBR / VBR / SMART / CVBR / AVBR / QVBR: the QP of the rate
 * control, openimp_t32_rc.c, docs/T32_RC.md; no re-encoding, no macroblock
 * QP).  OPENIMP_T32_HERA_SECTION_KB=<n> sets the half size (test: BSFULL).
 *
 * H.265 (#294) runs the same flow: CHANNEL_REQ H265ENC, the picture's CTB-64
 * list (openimp_t32_hera_h265.c), VPS / SPS / PPS (openimp_t32_h265.c) in
 * front of the IDR slice (4 packs), the vendor's 38 statistics registers
 * read back; POC = pictures since the IDR, one reference.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#include "dma_alloc.h"
#include "imp_log_int.h"
#include "core/thread_name.h"
#include "t32/openimp_t32_fs.h"
#include "t32/openimp_t32_h264.h"
#include "t32/openimp_t32_h265.h"
#include "t32/openimp_t32_hera.h"
#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_hera_h265.h"
#include "t32/openimp_t32_ivdc.h"
#include "t32/openimp_t32_vpu.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_encoder.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ENC"

#define T32_ENC_GROUPS      6
#define T32_ENC_CHNS        9
#define T32_ENC_MAX_STREAMS 8
#define T32_ENC_DEF_STREAMS 3
#define T32_JPEG_DEF_QP     40          /* vendor: initial QP out of 0..98 */
#define T32_JPEG_MAX_QP     98
#define T32_JPEG_RETRY_STEP 8           /* quality index step after an over-limit picture */
#define T32_JPEG_BS_1080P   512000u     /* vendor default stream buffer / limit at 1920x1080 */
#define T32_JPEG_BS_MIN     65536u
#define T32_JPEG_VPU_ID     (OI_T32_VPU_HELIX_ID | 1u)
#define T32_JPEG_VERSION_REG 0x132f0200u

#define T32_HERA_VPU_ID     OI_T32_VPU_RADIX_ID  /* the vendor's START: no instance bit */
#define T32_HERA_SECTION_KB 250u        /* vendor: two halves of 250 KiB */
#define T32_H264_DEF_QP     30          /* an initial QP outside 0..51 */
#define T32_H264_PACKS      4           /* (VPS,) SPS, PPS, slice */
#define T32_HERA_STATS      48          /* H.264 48, H.265 38 */
#define T32_HERA5_STATS     38

#define ALIGN16(x) (((x) + 15u) & ~15u)

/* ITU-T T.81 Annex K tables K.1 / K.2, zigzag order */
static const uint8_t jpeg_base_luma[64] = {
    16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
    26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
    56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
};
static const uint8_t jpeg_base_chroma[64] = {
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

enum { SLOT_FREE, SLOT_READY, SLOT_USER };

struct t32_enc_slot {
    T32VpuBuf bs;
    IMPEncoderPack pack[T32_H264_PACKS];
    uint32_t npacks, length;
    IMPRefType ref;
    uint32_t seq;
    int state;
};

struct t32_h264 {                       /* H.264 / H.265 channel state */
    T32VpuBuf list, ring, ebs, aux[T32_HERA5_AUX];
    T32H264Seq seq;
    T32H265Seq seq5;
    uint8_t vps[T32_H265_PARAM_MAX], sps[T32_H265_PARAM_MAX], pps[T32_H265_PARAM_MAX];
    size_t vps_len, sps_len, pps_len;
    int last_p;                         /* H.265: the last picture was a P */
    uint32_t pictures;                  /* H.265: pictures coded */
    int qp_i, qp_p;
    uint32_t gop, gop_pos, idr_pic_id, section_kb;
    int have_ref, idr_req;
    uint32_t ref_y, ref_c;              /* the last reconstruction */
    uint32_t sections, overflows;
    uint32_t stats[T32_HERA_STATS];     /* the engine's statistics of the last picture */
    T32Rc rc;                           /* rate control (#295), off at FIXQP */
    int rc_restart;                     /* SetChnAttrRcMode: restart at the next IDR */
};

struct t32_enc_chn {
    int created, registered, recv, quit;
    int grp;
    IMPEncoderCHNAttr attr;
    int qp;                             /* jpegQp 0..98 */
    int h264;                           /* PT_H264 or PT_H265 on HERA */
    int hevc;                           /* PT_H265 */
    struct t32_h264 avc;
    T32VpuChannel vpu;
    T32VpuBuf job;
    struct t32_enc_slot slot[T32_ENC_MAX_STREAMS];
    int nslots, max_streams;
    uint32_t bs_size;
    int ready[T32_ENC_MAX_STREAMS];     /* FIFO of SLOT_READY slots */
    int rhead, rcount;
    uint32_t seq;
    int pending, busy;                  /* a frame of the group is waiting / in work */
    uint64_t rate_acc;                  /* frame-rate drop */
    int src_fs;
    uint32_t src_num, src_den;
    pthread_t thread;
    int thread_up;
    pthread_cond_t work, done;
    int pipefd[2];
    uint32_t frames, encoded, skipped, retries, errors;
    int size_logged;
    int ivdc;                           /* fed by the IVDC direct channel (#301) */
};

struct t32_enc_grp {
    int created;
    IMPFrameInfo *frame;                /* the kept frame while channels code it */
    int fs_chn, refs;
};

static pthread_mutex_t enc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct t32_enc_grp enc_grp[T32_ENC_GROUPS];
static struct t32_enc_chn enc_chn[T32_ENC_CHNS];
static uint32_t enc_jpeg_bs_size;       /* IMP_Encoder_SetJpegBsSize, 0 = default */

static struct t32_enc_chn *chn_get(int n)
{
    return n >= 0 && n < T32_ENC_CHNS && enc_chn[n].created ? &enc_chn[n] : NULL;
}

static int jpeg_qp_of(int qp)
{
    if (qp < 0)
        return T32_JPEG_DEF_QP;
    return qp > T32_JPEG_MAX_QP ? T32_JPEG_MAX_QP : qp;
}

/* OPENIMP_T32_VPU_TEST=1 (#291 test): jobs as START + WAIT_COMPLETE instead
 * of RUN, and the engine's version register read once per channel
 * (WOR_VPU_REG); the default path is the vendor's (RUN only) */
static int vpu_test_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *v = getenv("OPENIMP_T32_VPU_TEST");
        mode = v && v[0] == '1';
    }
    return mode;
}

/* ---- the picture's source ------------------------------------------------- */
/* NV12 as FrameSource lays it out (UV after ALIGN16(height) rows), or, for a
 * frame over IVDC (#301), the IVDC's addresses; the soc_vpu node says which
 * (the kernel's radix / helix driver starts the IVDC reader for the job) */
static void enc_source(struct t32_enc_chn *e, const IMPFrameInfo *f, uint32_t *y, uint32_t *c,
                       uint32_t *stride)
{
    int ivdc = f->phyAddr == OI_T32_IVDC_SRC_Y;

    if (ivdc) {
        *y = OI_T32_IVDC_SRC_Y;
        *c = OI_T32_IVDC_SRC_C;
        *stride = OI_T32_IVDC_STRIDE;
    } else {
        *stride = ALIGN16(f->width);
        *y = f->phyAddr;
        *c = f->phyAddr + *stride * ALIGN16(f->height);
    }
    e->vpu.node.frame_type = ivdc ? OI_T32_FRAME_TYPE_IVDC : 0;
    e->vpu.node.sensor_id = 0;          /* one sensor (direct mode 1) */
}

/* ---- the JPEG job --------------------------------------------------------- */
static void jpeg_fill_job(struct t32_enc_chn *e, const IMPFrameInfo *f, const T32VpuBuf *bs,
                          uint32_t index)
{
    T32IocJpegRegInfo *j = t32_vpu_buf_va(&e->job);
    uint32_t w = f->width, h = f->height, stride, y, c, i;

    enc_source(e, f, &y, &c, &stride);
    memset(j, 0, T32_JPEG_JOB_BYTES);
    for (i = 0; i < 16; i++) {
        j->qmem[i] = (uint32_t)jpeg_base_luma[4 * i] | (uint32_t)jpeg_base_luma[4 * i + 1] << 8 |
                     (uint32_t)jpeg_base_luma[4 * i + 2] << 16 | (uint32_t)jpeg_base_luma[4 * i + 3] << 24;
        j->qmem[16 + i] = (uint32_t)jpeg_base_chroma[4 * i] | (uint32_t)jpeg_base_chroma[4 * i + 1] << 8 |
                          (uint32_t)jpeg_base_chroma[4 * i + 2] << 16 | (uint32_t)jpeg_base_chroma[4 * i + 3] << 24;
    }
    j->jpeg_ctrl = 1u << 4 | 1u << 5 | 1u << 6 |       /* U: DC/AC/quant table 1 */
                   1u << 8 | 1u << 9 | 1u << 10 |       /* V: the same */
                   1u << 16 |                           /* limit_bs_len enabled */
                   (index & 0x7fu) << 18 |              /* quality index */
                   1u << 25 | 1u << 26;                 /* frame end, over limit */
    j->nmcu = (ALIGN16(w) / 16u) * (ALIGN16(h) / 16u) - 1u;
    j->frm_info = h << 16 | w;
    j->stride = stride;
    j->y_addr = y;
    j->c_addr = c;
    j->bs_addr = t32_vpu_buf_pa(bs);
    j->pixel_num = ALIGN16(w) * ALIGN16(h) * 3u / 2u;
    j->limit_bs_len = bs->size;
}

/* one picture into a free slot: 0 with the length, -1 dropped */
static int jpeg_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s,
                       uint32_t *len)
{
    uint32_t index = (uint32_t)(T32_JPEG_MAX_QP - e->qp);
    T32VpuResult r;

    for (;;) {
        jpeg_fill_job(e, f, &s->bs, index);
        OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&e->job), T32_JPEG_JOB_BYTES, OI_T32_VPU_FLUSH_WBACK);
        if (vpu_test_mode()) {          /* the two-step path: START, WAIT_COMPLETE */
            if (OpenIMP_T32_VpuStart(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job)) != 0 ||
                OpenIMP_T32_VpuWait(&e->vpu, &r) != 0)
                return -1;
        } else if (OpenIMP_T32_VpuRun(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job), &r) != 0) {
            return -1;
        }
        if (r.rc == 0 && r.output_len > 0 && r.output_len <= s->bs.size) {
            OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&s->bs), r.output_len, OI_T32_VPU_FLUSH_INV);
            *len = r.output_len;
            return 0;
        }
        if (r.rc != OI_T32_VPU_RET_JPEG_BSLESS || index >= T32_JPEG_MAX_QP) {
            IMP_LOG_ERR(TAG, "JPEG job: result 0x%lx status 0x%x length %u", r.rc, r.status,
                        r.output_len);
            return -1;
        }
        index = index + T32_JPEG_RETRY_STEP > T32_JPEG_MAX_QP ? T32_JPEG_MAX_QP :
                index + T32_JPEG_RETRY_STEP;
        e->retries++;
        IMP_LOG_WARN(TAG, "picture over the %u byte buffer, again at quality index %u",
                     s->bs.size, index);
    }
}

/* ---- H.264 on HERA ---------------------------------------------------------- */
/* the statistics registers vendor 2.2.0 reads after each picture (VMAU, DBLK,
 * MD, EFE, ODMA: bits, SSE, QP sums, analysis counts), in its order */
static const uint32_t hera_stat_regs[T32_HERA_STATS] = {
    0x13280120, 0x13280124, 0x13280128, 0x1328012c, 0x13280130, 0x13280134, 0x13280138, 0x1328013c,
    0x13280140, 0x13280144, 0x13280148, 0x1328014c, 0x132500e8, 0x132500ec, 0x132500f0, 0x13280080,
    0x13240094, 0x13240098, 0x1324009c, 0x132400a0, 0x132800e8, 0x132800ec, 0x132800e4, 0x132800e0,
    0x13280168, 0x13280114, 0x13280118, 0x1328011c, 0x13280034, 0x132800d8, 0x132800dc, 0x13280084,
    0x13280088, 0x1328008c, 0x13280090, 0x13280094, 0x132800f0, 0x132800f4, 0x132800f8, 0x13220140,
    0x13220144, 0x13220148, 0x1322014c, 0x132203a4, 0x132203a8, 0x13210168, 0x1321016c, 0x132203ac,
};

/* H.265: the 38 registers vendor 2.2.0 reads after each picture, in its order */
static const uint32_t hera5_stat_regs[T32_HERA5_STATS] = {
    0x13220124, 0x13220128, 0x1322012c, 0x13220130, 0x13220134, 0x13220140, 0x13220144, 0x13220148,
    0x1322014c, 0x13220110, 0x13400114, 0x13400118, 0x1340011c, 0x13400120, 0x13240080, 0x13240084,
    0x13240088, 0x1324008c, 0x13240090, 0x13240094, 0x13240098, 0x1324009c, 0x132400a0, 0x132400a4,
    0x132400a8, 0x132400ac, 0x132400b0, 0x132400b4, 0x132400b8, 0x132203a4, 0x132203a8, 0x13240350,
    0x13240354, 0x13240358, 0x13210168, 0x1321016c, 0x13268080, 0x132203ac,
};

static int h264_qp(uint32_t qp)
{
    return qp <= 51u ? (int)qp : T32_H264_DEF_QP;
}

static void h264_set_qp(struct t32_enc_chn *e, const IMPEncoderAttrRcMode *rc)
{
    switch (rc->rcMode) {
    case IMP_ENC_RC_MODE_FIXQP:
        e->avc.qp_i = h264_qp(rc->attrH264FixQp.IQp);
        e->avc.qp_p = h264_qp(rc->attrH264FixQp.PQp);
        break;
    case IMP_ENC_RC_MODE_CBR:
        e->avc.qp_i = e->avc.qp_p = h264_qp(rc->attrH264Cbr.initialQp);
        break;
    default:
        e->avc.qp_i = e->avc.qp_p = h264_qp(rc->attrH264Vbr.initialQp);
        break;
    }
}

/* the engine's next part (bytes @from..@to of the picture, in half @half)
 * escaped onto the slot: 0, -1 when the slot is full */
static int h264_take(struct t32_enc_chn *e, uint8_t *out, uint32_t cap, uint32_t *at,
                     uint32_t from, uint32_t to, int half, int *esc)
{
    uint32_t half_bytes = e->avc.section_kb * 1024u, n = to - from;
    const uint8_t *src = (const uint8_t *)t32_vpu_buf_va(&e->avc.ebs) + (half ? half_bytes : 0u);

    if (to < from || n > half_bytes)
        return -1;
    if (!n)
        return 0;
    if (*at + n + n / 2u + 1u > cap)
        return -1;
    OpenIMP_T32_VpuFlush(src, n, OI_T32_VPU_FLUSH_INV);
    *at += (uint32_t)t32_h264_escape(out + *at, src, n, esc);
    return 0;
}

static void h264_pack(struct t32_enc_slot *s, uint32_t off, uint32_t len, int type, int64_t ts)
{
    IMPEncoderPack *k = &s->pack[s->npacks++];

    memset(k, 0, sizeof(*k));
    k->phyAddr = t32_vpu_buf_pa(&s->bs) + off;
    k->virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&s->bs) + off;
    k->length = len;
    k->timestamp = ts;
    k->dataType.h264Type = (IMPEncoderH264NaluType)type;
}

static int hera_run(struct t32_enc_chn *e, uint32_t pairs, uint8_t *out, uint32_t cap, uint32_t *at,
                    int *esc, const uint32_t *stat_regs, unsigned nstats);

/* (re)start the channel's rate control from @m (#295) */
static void rc_setup(struct t32_enc_chn *e, const IMPEncoderAttrRcMode *m)
{
    struct t32_h264 *a = &e->avc;
    const IMPEncoderCHNAttr *at = &e->attr;

    if (t32_rc_init(&a->rc, m, ALIGN16(at->encAttr.picWidth), ALIGN16(at->encAttr.picHeight), a->gop,
                    at->rcAttr.outFrmRate.frmRateNum, at->rcAttr.outFrmRate.frmRateDen, e->hevc) != 0)
        IMP_LOG_WARN(TAG, "rate control init failed: fixed QP %d", a->qp_p);
    else if (a->rc.on)
        IMP_LOG_INFO(TAG, "%s rate control: mode %d -> eprc %u, %u kbit/s, qp [%u,%u], gop %u",
                     e->hevc ? "H.265" : "H.264", (int)m->rcMode, a->rc.params.rc_mode,
                     a->rc.params.rc_mode == EPRC_MODE_CBR ? a->rc.params.bitrate : a->rc.params.max_bitrate,
                     a->rc.params.min_qp, a->rc.params.max_qp, a->gop);
}

/* the picture's QP: the controller's, else the fixed / initial QP */
static int rc_picture_qp(struct t32_enc_chn *e, int idr)
{
    struct t32_h264 *a = &e->avc;
    int qp = idr ? a->qp_i : a->qp_p, q;

    if (idr) {
        IMPEncoderAttrRcMode m;
        int restart;

        pthread_mutex_lock(&enc_lock);
        restart = a->rc_restart;
        a->rc_restart = 0;
        m = e->attr.rcAttr.attrRcMode;
        pthread_mutex_unlock(&enc_lock);
        if (restart)
            rc_setup(e, &m);
    }
    if (a->rc.on && (q = t32_rc_start(&a->rc, idr, a->gop_pos)) >= 0)
        qp = q;
    return qp;
}

/* a picture the engine did not finish: accounted as the full stream window
 * without statistics (as the T23 Helix path does) */
static void rc_picture_lost(struct t32_enc_chn *e)
{
    if (e->avc.rc.on)
        t32_rc_end(&e->avc.rc, 2u * e->avc.section_kb * 1024u, NULL, 0);
}

static int h264_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s)
{
    struct t32_h264 *a = &e->avc;
    uint32_t w = ALIGN16(f->width), h = ALIGN16(f->height);
    uint32_t rs = t32_hera_ring_stride(w), rows = t32_hera_ring_rows(f->height);
    uint32_t aux[4], at = 0, cap = s->bs.size, n, i;
    uint8_t *out = t32_vpu_buf_va(&s->bs);
    int idr = a->idr_req || !a->have_ref || a->gop_pos == 0, esc = 0;
    T32HeraH264Pic p;
    T32H264Slice sl;

    memset(&p, 0, sizeof(p));
    p.width = w;
    p.height = h;
    p.p_frame = !idr;
    p.qp = rc_picture_qp(e, idr);
    enc_source(e, f, &p.src_y, &p.src_c, &p.src_stride);
    p.ring_y = t32_vpu_buf_pa(&a->ring);
    p.ring_y_end = p.ring_c = p.ring_y + rs * rows;
    p.ring_c_end = p.ring_c + rs * rows / 2u;
    if (idr) {
        p.recon_y = p.ring_y;
        p.recon_c = p.ring_c;
    } else {
        p.ref_y = a->ref_y;
        p.ref_c = a->ref_c;
        p.recon_y = t32_hera_ring_prev(a->ref_y, p.ring_y, p.ring_y_end, rs * T32_HERA_RING_EXTRA);
        p.recon_c = t32_hera_ring_prev(a->ref_c, p.ring_c, p.ring_c_end, rs * T32_HERA_RING_EXTRA / 2u);
    }
    p.bs0 = t32_vpu_buf_pa(&a->ebs);
    p.bs1 = p.bs0 + a->section_kb * 1024u;
    p.section_kb = a->section_kb;
    for (i = 0; i < 4; i++)
        aux[i] = t32_vpu_buf_pa(&a->aux[i]);
    p.cps = aux[0];
    p.ai = aux[1];
    p.qpt = aux[2];
    p.ipc = aux[3];
    n = (uint32_t)t32_hera_h264_list(&p, t32_vpu_buf_va(&a->list));
    if (!n) {
        IMP_LOG_ERR(TAG, "HERA list: %ux%u qp %d refused", w, h, p.qp);
        rc_picture_lost(e);
        return -1;
    }

    /* headers first: the engine's slice data follows them */
    s->npacks = 0;
    if (idr) {
        memcpy(out, a->sps, a->sps_len);
        h264_pack(s, 0, (uint32_t)a->sps_len, IMP_H264_NAL_SPS, f->timeStamp);
        memcpy(out + a->sps_len, a->pps, a->pps_len);
        h264_pack(s, (uint32_t)a->sps_len, (uint32_t)a->pps_len, IMP_H264_NAL_PPS, f->timeStamp);
        at = (uint32_t)(a->sps_len + a->pps_len);
    }
    memset(&sl, 0, sizeof(sl));
    sl.idr = idr;
    sl.frame_num = idr ? 0u : a->gop_pos % (1u << T32_H264_LOG2_MAX_FRAME_NUM);
    sl.idr_pic_id = a->idr_pic_id;
    sl.qp = p.qp;
    i = at;
    at += (uint32_t)t32_h264_slice_head(&a->seq, &sl, out + at, &esc);

    if (hera_run(e, n, out, cap, &at, &esc, hera_stat_regs, T32_HERA_STATS) != 0) {
        rc_picture_lost(e);
        return -1;
    }
    /* FRAME_END: the slice's size and the statistics the engine left */
    t32_rc_end(&a->rc, at - i, a->stats, T32_HERA_STATS);
    h264_pack(s, i, at - i, idr ? IMP_H264_NAL_SLICE_IDR : IMP_H264_NAL_SLICE, f->timeStamp);
    s->pack[s->npacks - 1].frameEnd = 1;
    s->length = at;
    s->ref = idr ? IMP_Encoder_FSTYPE_IDR : IMP_Encoder_FSTYPE_LBASE;
    a->ref_y = p.recon_y;
    a->ref_c = p.recon_c;
    a->have_ref = 1;
    if (idr) {
        a->idr_req = 0;
        a->gop_pos = 0;
        a->idr_pic_id = (a->idr_pic_id + 1u) & 0xffffu;
    }
    a->gop_pos = a->gop ? (a->gop_pos + 1u) % a->gop : a->gop_pos + 1u;
    return 0;
}

static void h265_pack(struct t32_enc_slot *s, uint32_t off, uint32_t len, int type, int64_t ts)
{
    h264_pack(s, off, len, 0, ts);
    s->pack[s->npacks - 1].dataType.h265Type = (IMPEncoderH265NaluType)type;
}

/* run the engine on a written list: the parts escaped onto the slot after
 * @at; 0, -1 */
static int hera_run(struct t32_enc_chn *e, uint32_t pairs, uint8_t *out, uint32_t cap, uint32_t *at,
                    int *esc, const uint32_t *stat_regs, unsigned nstats)
{
    struct t32_h264 *a = &e->avc;
    uint32_t taken = 0, n;
    int half = 0, full = 0;
    T32VpuResult r;

    OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&a->list), pairs * 8u, OI_T32_VPU_FLUSH_WBACK);
    a->have_ref = 0;                    /* the reconstruction overwrites the reference */
    if (OpenIMP_T32_VpuStart(&e->vpu, T32_HERA_VPU_ID, t32_vpu_buf_pa(&a->list)) != 0)
        return -1;
    for (;;) {
        if (OpenIMP_T32_VpuWaitBsFull(&e->vpu, &r) != 0)
            return -1;
        if (r.rc != OI_T32_VPU_RET_HERA_BSFULL)
            break;
        if (h264_take(e, out, cap, at, taken, r.output_len, half, esc) != 0)
            full = 1;
        taken = r.output_len;
        half ^= 1;
        a->sections++;
        if (OpenIMP_T32_VpuSetBsFull(&e->vpu) != 0)
            return -1;
    }
    for (n = 0; n < nstats; n++)
        OpenIMP_T32_VpuReadReg(stat_regs[n], &a->stats[n]);
    if (r.rc != 0 || !r.output_len || full ||
        h264_take(e, out, cap, at, taken, r.output_len, half, esc) != 0) {
        if (full || r.rc == 0)
            a->overflows++;
        IMP_LOG_ERR(TAG, "HERA picture: result 0x%lx status 0x%x length %u%s", r.rc, r.status,
                    r.output_len, full || r.rc == 0 ? " (stream slot full)" : "");
        return -1;
    }
    return 0;
}

static int h265_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s)
{
    struct t32_h264 *a = &e->avc;
    uint32_t w = ALIGN16(f->width), h = (f->height + 7u) & ~7u;
    uint32_t rs = t32_hera_ring_stride(w), rows = t32_hera_ring_rows(h);
    uint32_t at = 0, cap = s->bs.size, n, i;
    uint8_t *out = t32_vpu_buf_va(&s->bs);
    int idr = a->idr_req || !a->have_ref || a->gop_pos == 0, esc = 0;
    T32HeraH265Pic p;
    T32H265Slice sl;

    memset(&p, 0, sizeof(p));
    p.width = w;
    p.height = h;
    p.p_frame = !idr;
    p.after_p = !idr && a->last_p;
    p.first = a->pictures == 0;
    p.qp = rc_picture_qp(e, idr);
    enc_source(e, f, &p.src_y, &p.src_c, &p.src_stride);
    p.ring_y = t32_vpu_buf_pa(&a->ring);
    p.ring_y_end = p.ring_c = p.ring_y + rs * rows;
    p.ring_c_end = p.ring_c + rs * rows / 2u;
    if (idr) {
        p.recon_y = p.ring_y;
        p.recon_c = p.ring_c;
    } else {
        p.ref_y = a->ref_y;
        p.ref_c = a->ref_c;
        p.recon_y = t32_hera_ring_prev(a->ref_y, p.ring_y, p.ring_y_end, rs * T32_HERA_RING_EXTRA);
        p.recon_c = t32_hera_ring_prev(a->ref_c, p.ring_c, p.ring_c_end, rs * T32_HERA_RING_EXTRA / 2u);
    }
    p.bs0 = t32_vpu_buf_pa(&a->ebs);
    p.bs1 = p.bs0 + a->section_kb * 1024u;
    p.section_kb = a->section_kb;
    for (i = 0; i < T32_HERA5_AUX; i++)
        p.aux[i] = t32_vpu_buf_pa(&a->aux[i]);
    n = (uint32_t)t32_hera_h265_list(&p, t32_vpu_buf_va(&a->list));
    if (!n) {
        IMP_LOG_ERR(TAG, "HERA H.265 list: %ux%u qp %d refused", w, h, p.qp);
        rc_picture_lost(e);
        return -1;
    }

    s->npacks = 0;
    if (idr) {
        memcpy(out, a->vps, a->vps_len);
        h265_pack(s, 0, (uint32_t)a->vps_len, IMP_H265_NAL_VPS, f->timeStamp);
        at = (uint32_t)a->vps_len;
        memcpy(out + at, a->sps, a->sps_len);
        h265_pack(s, at, (uint32_t)a->sps_len, IMP_H265_NAL_SPS, f->timeStamp);
        at += (uint32_t)a->sps_len;
        memcpy(out + at, a->pps, a->pps_len);
        h265_pack(s, at, (uint32_t)a->pps_len, IMP_H265_NAL_PPS, f->timeStamp);
        at += (uint32_t)a->pps_len;
    }
    memset(&sl, 0, sizeof(sl));
    sl.idr = idr;
    sl.poc = idr ? 0u : a->gop_pos;
    sl.qp = p.qp;
    i = at;
    at += (uint32_t)t32_h265_slice_head(&a->seq5, &sl, out + at, &esc);
    if (hera_run(e, n, out, cap, &at, &esc, hera5_stat_regs, T32_HERA5_STATS) != 0) {
        rc_picture_lost(e);
        return -1;
    }
    /* H.265 reads no Helix-family statistics: the size only */
    t32_rc_end(&a->rc, at - i, NULL, 0);
    h265_pack(s, i, at - i, idr ? IMP_H265_NAL_SLICE_IDR_W_RADL : IMP_H265_NAL_SLICE_TRAIL_R, f->timeStamp);
    s->pack[s->npacks - 1].frameEnd = 1;
    s->length = at;
    s->ref = idr ? IMP_Encoder_FSTYPE_IDR : IMP_Encoder_FSTYPE_LBASE;
    a->ref_y = p.recon_y;
    a->ref_c = p.recon_c;
    a->have_ref = 1;
    a->last_p = !idr;
    a->pictures++;
    if (idr) {
        a->idr_req = 0;
        a->gop_pos = 0;
    }
    a->gop_pos = a->gop ? (a->gop_pos + 1u) % a->gop : a->gop_pos + 1u;
    return 0;
}

/* ---- channel thread ------------------------------------------------------- */
static int slot_free_locked(struct t32_enc_chn *e)
{
    int i;

    for (i = 0; i < e->nslots; i++)
        if (e->slot[i].state == SLOT_FREE)
            return i;
    return -1;
}

static void *enc_thread(void *arg)
{
    struct t32_enc_chn *e = arg;
    openimp_thread_setnamef(e->hevc ? "oimp-h265-%d" : e->h264 ? "oimp-h264-%d" : "oimp-jpeg-%d",
                            (int)(e - enc_chn));
    pthread_mutex_lock(&enc_lock);
    for (;;) {
        struct t32_enc_grp *g;
        IMPFrameInfo *frame;
        int fs, si, ok;
        uint32_t len = 0;

        while (!e->quit && !e->pending)
            pthread_cond_wait(&e->work, &enc_lock);
        if (e->quit)
            break;
        g = &enc_grp[e->grp];
        frame = g->frame;
        fs = g->fs_chn;
        e->pending = 0;
        e->busy = 1;
        si = slot_free_locked(e);       /* reserved by the sink */
        pthread_mutex_unlock(&enc_lock);

        if (e->hevc)
            ok = si >= 0 && frame && h265_encode(e, frame, &e->slot[si]) == 0;
        else if (e->h264)
            ok = si >= 0 && frame && h264_encode(e, frame, &e->slot[si]) == 0;
        else
            ok = si >= 0 && frame && jpeg_encode(e, frame, &e->slot[si], &len) == 0;

        pthread_mutex_lock(&enc_lock);
        if (ok) {
            struct t32_enc_slot *s = &e->slot[si];

            if (!e->h264) {
                memset(&s->pack[0], 0, sizeof(s->pack[0]));
                s->pack[0].phyAddr = t32_vpu_buf_pa(&s->bs);
                s->pack[0].virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&s->bs);
                s->pack[0].length = len;
                s->pack[0].timestamp = frame->timeStamp;
                s->pack[0].frameEnd = 1;
                s->npacks = 1;
                s->length = len;
                s->ref = IMP_Encoder_FSTYPE_IDR;
            }
            s->seq = e->seq++;
            s->state = SLOT_READY;
            e->ready[(e->rhead + e->rcount) % T32_ENC_MAX_STREAMS] = si;
            e->rcount++;
            e->encoded++;
            if (write(e->pipefd[1], "s", 1) != 1)
                IMP_LOG_WARN(TAG, "stream notify pipe full");
        } else {
            e->errors++;
        }
        e->busy = 0;
        if (--g->refs == 0) {
            g->frame = NULL;
            pthread_mutex_unlock(&enc_lock);
            OpenIMP_FrameSource_ReleaseSinkFrame(fs, frame);
            pthread_mutex_lock(&enc_lock);
        }
        pthread_cond_broadcast(&e->done);
    }
    pthread_mutex_unlock(&enc_lock);
    return NULL;
}

/* frame-rate drop: the channel rate out of the source rate */
static int rate_pass_locked(struct t32_enc_chn *e)
{
    uint64_t num = e->attr.rcAttr.outFrmRate.frmRateNum, den = e->attr.rcAttr.outFrmRate.frmRateDen;
    uint64_t step, full;

    if (!num || !den || !e->src_num || !e->src_den)
        return 1;
    step = num * e->src_den;            /* channel fps x source fps' denominators */
    full = (uint64_t)e->src_num * den;
    if (step >= full)
        return 1;
    e->rate_acc += step;
    if (e->rate_acc < full)
        return 0;
    e->rate_acc -= full;
    return 1;
}

/* FrameSource sink of an encoder group (runs in the channel's frame thread) */
static int enc_sink(void *ctx, int fs_chn, IMPFrameInfo *frame)
{
    int grp = (int)(intptr_t)ctx, i, refs = 0, need_rate = 0;
    struct t32_enc_grp *g = &enc_grp[grp];

    pthread_mutex_lock(&enc_lock);
    for (i = 0; i < T32_ENC_CHNS; i++) {
        struct t32_enc_chn *e = &enc_chn[i];
        if (e->created && e->registered && e->grp == grp && e->recv && e->src_fs != fs_chn)
            need_rate = 1;
    }
    pthread_mutex_unlock(&enc_lock);
    if (need_rate) {                    /* source rate for the drop, outside the lock */
        IMPFSChnAttr fa;
        if (IMP_FrameSource_GetChnAttr(fs_chn, &fa) == 0) {
            pthread_mutex_lock(&enc_lock);
            for (i = 0; i < T32_ENC_CHNS; i++) {
                struct t32_enc_chn *e = &enc_chn[i];
                if (e->created && e->registered && e->grp == grp) {
                    e->src_fs = fs_chn;
                    e->src_num = fa.outFrmRateNum;
                    e->src_den = fa.outFrmRateDen;
                }
            }
            pthread_mutex_unlock(&enc_lock);
        }
    }

    pthread_mutex_lock(&enc_lock);
    if (!g->created || g->frame) {      /* still coding the last one: drop */
        for (i = 0; i < T32_ENC_CHNS; i++)
            if (enc_chn[i].created && enc_chn[i].registered && enc_chn[i].grp == grp &&
                enc_chn[i].recv)
                enc_chn[i].skipped++;
        pthread_mutex_unlock(&enc_lock);
        return 0;
    }
    for (i = 0; i < T32_ENC_CHNS; i++) {
        struct t32_enc_chn *e = &enc_chn[i];

        if (!e->created || !e->registered || e->grp != grp || !e->recv)
            continue;
        e->frames++;
        if (!rate_pass_locked(e))
            continue;
        if (frame->width != e->attr.encAttr.picWidth || frame->height != e->attr.encAttr.picHeight) {
            if (!e->size_logged++)
                IMP_LOG_ERR(TAG, "chn %d: frame %ux%u, channel %ux%u (no scaling in the encoder)",
                            i, frame->width, frame->height, e->attr.encAttr.picWidth,
                            e->attr.encAttr.picHeight);
            e->skipped++;
            continue;
        }
        if (e->pending || e->busy || slot_free_locked(e) < 0) {
            e->skipped++;               /* no free stream buffer: GetStream is behind */
            continue;
        }
        e->pending = 1;
        refs++;
        pthread_cond_signal(&e->work);
    }
    if (refs) {
        g->frame = frame;
        g->fs_chn = fs_chn;
        g->refs = refs;
    }
    pthread_mutex_unlock(&enc_lock);
    return refs ? 1 : 0;
}

/* ---- groups --------------------------------------------------------------- */
int IMP_Encoder_CreateGroup(int encGroup)
{
    int rc;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    if (enc_grp[encGroup].created) {
        pthread_mutex_unlock(&enc_lock);
        IMP_LOG_ERR(TAG, "group %d exists", encGroup);
        return -1;
    }
    memset(&enc_grp[encGroup], 0, sizeof(enc_grp[encGroup]));
    enc_grp[encGroup].created = 1;
    pthread_mutex_unlock(&enc_lock);
    rc = OpenIMP_FrameSource_RegisterSink(DEV_ID_ENC, encGroup, enc_sink, (void *)(intptr_t)encGroup);
    if (rc) {
        pthread_mutex_lock(&enc_lock);
        enc_grp[encGroup].created = 0;
        pthread_mutex_unlock(&enc_lock);
    }
    return rc;
}

int IMP_Encoder_DestroyGroup(int encGroup)
{
    int i;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    if (!enc_grp[encGroup].created) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    for (i = 0; i < T32_ENC_CHNS; i++)
        if (enc_chn[i].created && enc_chn[i].registered && enc_chn[i].grp == encGroup) {
            pthread_mutex_unlock(&enc_lock);
            IMP_LOG_ERR(TAG, "group %d: channel %d still registered", encGroup, i);
            return -1;
        }
    enc_grp[encGroup].created = 0;
    pthread_mutex_unlock(&enc_lock);
    OpenIMP_FrameSource_RegisterSink(DEV_ID_ENC, encGroup, NULL, NULL);
    return 0;
}

/* ---- channel attributes ----------------------------------------------------- */
int IMP_Encoder_SetDefaultParam(IMPEncoderChnAttr *attr, IMPEncoderProfile profile,
                                IMPEncoderRcMode rcMode, int width, int height,
                                int fpsNum, int fpsDen, int gopLen,
                                int bufSize, int iInitialQP, int bitrate)
{
    IMPEncoderAttrRcMode *rc;

    if (!attr || !fpsDen)
        return -1;
    memset(attr, 0, sizeof(*attr));
    attr->encAttr.enType = (IMPPayloadType)(((uint32_t)profile >> 24) & 0xffu);
    attr->encAttr.bufSize = (uint32_t)bufSize;
    attr->encAttr.profile = (uint32_t)profile;
    attr->encAttr.picWidth = (uint16_t)width;
    attr->encAttr.picHeight = (uint16_t)height;
    attr->rcAttr.outFrmRate.frmRateNum = (uint32_t)fpsNum;
    attr->rcAttr.outFrmRate.frmRateDen = (uint32_t)fpsDen;
    attr->rcAttr.maxGop = (uint32_t)gopLen;
    rc = &attr->rcAttr.attrRcMode;
    rc->rcMode = rcMode;
    if (attr->encAttr.enType == PT_JPEG) {
        if (rcMode != IMP_ENC_RC_MODE_FIXQP) {
            IMP_LOG_ERR(TAG, "JPEG takes FIXQP only");
            return -1;
        }
        rc->attrJPEGFixQp.qp = (uint32_t)iInitialQP;  /* kept as given, -1 too */
        return 0;
    }
    /* H.264 / H.265 (HERA, later issue): the basic fields */
    switch (rcMode) {
    case IMP_ENC_RC_MODE_FIXQP:
        rc->attrH264FixQp.IQp = rc->attrH264FixQp.PQp = rc->attrH264FixQp.BQp = (uint32_t)iInitialQP;
        break;
    case IMP_ENC_RC_MODE_CBR:
        rc->attrH264Cbr.outBitRate = (uint32_t)bitrate;
        rc->attrH264Cbr.initialQp = (uint8_t)iInitialQP;
        rc->attrH264Cbr.maxQp = 48;
        rc->attrH264Cbr.minQp = 15;
        break;
    default:
        rc->attrH264Vbr.maxBitRate = (uint32_t)bitrate;
        rc->attrH264Vbr.initialQp = (uint8_t)iInitialQP;
        rc->attrH264Vbr.maxQp = 48;
        rc->attrH264Vbr.minQp = 15;
        break;
    }
    return 0;
}

static uint32_t jpeg_bs_size(const IMPEncoderCHNAttr *a)
{
    uint64_t area = (uint64_t)a->encAttr.picWidth * a->encAttr.picHeight, size;

    if (a->encAttr.bufSize)
        return (a->encAttr.bufSize + 4095u) & ~4095u;
    if (enc_jpeg_bs_size)
        return (enc_jpeg_bs_size + 4095u) & ~4095u;
    size = (T32_JPEG_BS_1080P * area + 1920u * 1080u - 1u) / (1920u * 1080u);
    size = (size + 4095u) & ~(uint64_t)4095u;
    return size < T32_JPEG_BS_MIN ? T32_JPEG_BS_MIN : (uint32_t)size;
}

static uint32_t h264_bs_size(const IMPEncoderCHNAttr *a)
{
    uint32_t size = a->encAttr.bufSize ? a->encAttr.bufSize :
                    (uint32_t)ALIGN16(a->encAttr.picWidth) * ALIGN16(a->encAttr.picHeight);

    size = (size + 4095u) & ~4095u;
    return size < 262144u ? 262144u : size;
}

static void h264_free(struct t32_enc_chn *e)
{
    int i;

    OpenIMP_T32_VpuBufFree(&e->avc.list);
    OpenIMP_T32_VpuBufFree(&e->avc.ring);
    OpenIMP_T32_VpuBufFree(&e->avc.ebs);
    for (i = 0; i < T32_HERA5_AUX; i++)
        OpenIMP_T32_VpuBufFree(&e->avc.aux[i]);
    t32_rc_free(&e->avc.rc);
}

/* the H.265 channel's analysis buffers and parameter sets: 0 or -1 */
static int h265_setup(struct t32_enc_chn *e)
{
    static const char *const aux_tag[T32_HERA5_AUX] = {
        "t32_hera5_cps", "t32_hera5_a24", "t32_hera5_qpt", "t32_hera5_ai", "t32_hera5_a2c" };
    struct t32_h264 *a = &e->avc;
    const IMPEncoderCHNAttr *at = &e->attr;
    uint32_t sizes[T32_HERA5_AUX];
    int i;

    t32_hera_h265_aux_sizes(ALIGN16(at->encAttr.picWidth), (at->encAttr.picHeight + 7u) & ~7u, sizes);
    for (i = 0; i < T32_HERA5_AUX; i++)
        if (OpenIMP_T32_VpuBufAlloc(&a->aux[i], sizes[i], aux_tag[i]) != 0)
            return -1;
    a->seq5.width = at->encAttr.picWidth;
    a->seq5.height = at->encAttr.picHeight;
    a->seq5.fps_num = at->rcAttr.outFrmRate.frmRateNum;
    a->seq5.fps_den = at->rcAttr.outFrmRate.frmRateDen;
    a->seq5.rc_mode = at->rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_FIXQP ? 0u : 1u;
    a->seq5.bitrate = at->rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_CBR ?
                      at->rcAttr.attrRcMode.attrH265Cbr.outBitRate : 0u;
    a->vps_len = t32_h265_vps(&a->seq5, a->vps);
    a->sps_len = t32_h265_sps(&a->seq5, a->sps);
    a->pps_len = t32_h265_pps(&a->seq5, a->pps);
    if (!a->vps_len || !a->sps_len || !a->pps_len)
        return -1;
    rc_setup(e, &at->rcAttr.attrRcMode);
    return 0;
}

/* the H.264 / H.265 channel's engine buffers and parameter sets: 0 or -1 */
static int h264_setup(struct t32_enc_chn *e)
{
    static const char *const aux_tag[4] = { "t32_hera_cps", "t32_hera_ai", "t32_hera_qpt", "t32_hera_ipc" };
    struct t32_h264 *a = &e->avc;
    const IMPEncoderCHNAttr *at = &e->attr;
    uint32_t w = ALIGN16(at->encAttr.picWidth), sizes[4];
    const char *env = getenv("OPENIMP_T32_HERA_SECTION_KB");
    int i;

    a->section_kb = T32_HERA_SECTION_KB;
    if (env && atoi(env) > 0 && atoi(env) <= 4096)
        a->section_kb = (uint32_t)atoi(env);
    a->gop = at->rcAttr.maxGop;
    h264_set_qp(e, &at->rcAttr.attrRcMode);
    if (OpenIMP_T32_VpuBufAlloc(&a->list, T32_HERA_LIST_BYTES, "t32_hera_list") != 0 ||
        OpenIMP_T32_VpuBufAlloc(&a->ring, t32_hera_ring_stride(w) * t32_hera_ring_rows(at->encAttr.picHeight) * 3u / 2u,
                                "t32_hera_ring") != 0 ||
        OpenIMP_T32_VpuBufAlloc(&a->ebs, 2u * a->section_kb * 1024u, "t32_hera_bs") != 0)
        return -1;
    if (e->hevc)
        return h265_setup(e);
    t32_hera_aux_sizes(w, ALIGN16(at->encAttr.picHeight), sizes);
    for (i = 0; i < 4; i++)
        if (OpenIMP_T32_VpuBufAlloc(&a->aux[i], sizes[i], aux_tag[i]) != 0)
            return -1;
    a->seq.width = at->encAttr.picWidth;
    a->seq.height = at->encAttr.picHeight;
    a->seq.fps_num = at->rcAttr.outFrmRate.frmRateNum;
    a->seq.fps_den = at->rcAttr.outFrmRate.frmRateDen;
    a->seq.bitrate = at->rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_CBR ?
                     at->rcAttr.attrRcMode.attrH264Cbr.outBitRate * 1000u : 0u;
    a->seq.init_qp = a->qp_i;
    a->sps_len = t32_h264_sps(&a->seq, a->sps);
    a->pps_len = t32_h264_pps(&a->seq, a->pps);
    rc_setup(e, &at->rcAttr.attrRcMode);
    return 0;
}

static void chn_free_bufs(struct t32_enc_chn *e)
{
    int i;

    for (i = 0; i < e->nslots; i++)
        OpenIMP_T32_VpuBufFree(&e->slot[i].bs);
    e->nslots = 0;
    e->rhead = e->rcount = 0;
}

static int chn_alloc_bufs(struct t32_enc_chn *e, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        T32VpuBuf *b = &e->slot[i].bs;

        e->slot[i].state = SLOT_FREE;
        if (OpenIMP_T32_VpuBufAlloc(b, e->bs_size, "t32_jpeg_bs") != 0) {
            e->nslots = i;
            chn_free_bufs(e);
            return -1;
        }
        /* the CPU never writes a stream buffer again: no dirty line may
         * land on the engine's output later */
        DMA_RmemFlushCache(t32_vpu_buf_va(b), b->size, 1);
        e->nslots = i + 1;
    }
    return 0;
}

int IMP_Encoder_CreateChn(int encChn, IMPEncoderCHNAttr *attr)
{
    struct t32_enc_chn *e;
    uint32_t version = 0;

    if (encChn < 0 || encChn >= T32_ENC_CHNS || !attr)
        return -1;
    if (attr->encAttr.enType != PT_JPEG && attr->encAttr.enType != PT_H264 &&
        attr->encAttr.enType != PT_H265) {
        IMP_LOG_ERR(TAG, "chn %d: payload %d not supported on T32", encChn, (int)attr->encAttr.enType);
        return -1;
    }
    if (attr->encAttr.enType == PT_H265 &&
        (attr->encAttr.picWidth < 256 || attr->encAttr.picWidth > 4096 || attr->encAttr.picHeight < 64 ||
         attr->encAttr.picHeight > 2304)) {
        IMP_LOG_ERR(TAG, "chn %d: H.265 picture %ux%u outside 256..4096 x 64..2304", encChn,
                    attr->encAttr.picWidth, attr->encAttr.picHeight);
        return -1;
    }
    if (!attr->encAttr.picWidth || !attr->encAttr.picHeight ||
        attr->encAttr.picWidth > 8192 || attr->encAttr.picHeight > 8192) {
        IMP_LOG_ERR(TAG, "chn %d: picture %ux%u", encChn, attr->encAttr.picWidth, attr->encAttr.picHeight);
        return -1;
    }
    if (attr->encAttr.crop.enable)
        IMP_LOG_WARN(TAG, "chn %d: crop is not implemented on T32 yet, ignored", encChn);
    e = &enc_chn[encChn];
    pthread_mutex_lock(&enc_lock);
    if (e->created) {
        pthread_mutex_unlock(&enc_lock);
        IMP_LOG_ERR(TAG, "chn %d exists", encChn);
        return -1;
    }
    memset(e, 0, sizeof(*e));
    e->created = 1;                     /* reserved; the rest outside the lock */
    e->attr = *attr;
    e->h264 = attr->encAttr.enType == PT_H264 || attr->encAttr.enType == PT_H265;
    e->hevc = attr->encAttr.enType == PT_H265;
    e->qp = e->h264 ? 0 : jpeg_qp_of((int)attr->rcAttr.attrRcMode.attrJPEGFixQp.qp);
    e->max_streams = T32_ENC_DEF_STREAMS;
    e->bs_size = e->h264 ? h264_bs_size(attr) : jpeg_bs_size(attr);
    e->pipefd[0] = e->pipefd[1] = -1;
    e->src_fs = -1;
    pthread_mutex_unlock(&enc_lock);

    if (OpenIMP_T32_VpuOpen() != 0)
        goto fail_open;
    if (OpenIMP_T32_VpuRequest(&e->vpu, e->hevc ? OI_T32_VPU_CODEC_H265ENC :
                               e->h264 ? OI_T32_VPU_CODEC_H264ENC : OI_T32_VPU_CODEC_JPEGENC) != 0)
        goto fail_req;
    if (e->h264 ? h264_setup(e) != 0 :
        OpenIMP_T32_VpuBufAlloc(&e->job, T32_JPEG_JOB_BYTES, "t32_jpeg_job") != 0)
        goto fail_bufs;
    if (chn_alloc_bufs(e, e->max_streams) != 0)
        goto fail_bufs;
    if (pipe2(e->pipefd, O_CLOEXEC | O_NONBLOCK) != 0)
        goto fail_bufs;
    pthread_cond_init(&e->work, NULL);
    pthread_cond_init(&e->done, NULL);
    if (pthread_create(&e->thread, NULL, enc_thread, e) != 0)
        goto fail_thread;
    e->thread_up = 1;
    if (e->h264) {
        IMP_LOG_INFO(TAG, "chn %d: %s %ux%u QP I %d P %d GOP %u, %d stream buffers of %u bytes, "
                     "%u KiB sections, vpu channel %u", encChn, e->hevc ? "H.265" : "H.264", attr->encAttr.picWidth,
                     attr->encAttr.picHeight, e->avc.qp_i, e->avc.qp_p, e->avc.gop, e->nslots,
                     e->bs_size, e->avc.section_kb, e->vpu.node.channel_id);
        return 0;
    }
    if (vpu_test_mode() && OpenIMP_T32_VpuReadReg(T32_JPEG_VERSION_REG, &version) == 0)
        IMP_LOG_INFO(TAG, "JPEG engine version register 0x%08x", version);
    IMP_LOG_INFO(TAG, "chn %d: JPEG %ux%u jpegQp %d, %d stream buffers of %u bytes, vpu channel %u",
                 encChn, attr->encAttr.picWidth, attr->encAttr.picHeight, e->qp, e->nslots,
                 e->bs_size, e->vpu.node.channel_id);
    return 0;

fail_thread:
    pthread_cond_destroy(&e->work);
    pthread_cond_destroy(&e->done);
    close(e->pipefd[0]);
    close(e->pipefd[1]);
fail_bufs:
    chn_free_bufs(e);
    OpenIMP_T32_VpuBufFree(&e->job);
    h264_free(e);
    OpenIMP_T32_VpuRelease(&e->vpu);
fail_req:
    OpenIMP_T32_VpuClose();
fail_open:
    pthread_mutex_lock(&enc_lock);
    e->created = 0;
    pthread_mutex_unlock(&enc_lock);
    return -1;
}

static int chn_ivdc_codec(const struct t32_enc_chn *e)
{
    return e->hevc ? OI_T32_IVDC_H265 : e->h264 ? OI_T32_IVDC_H264 : OI_T32_IVDC_JPEG;
}

static void chn_stop_locked(struct t32_enc_chn *e)
{
    int was = e->recv;

    e->recv = 0;
    while (e->pending || e->busy)
        pthread_cond_wait(&e->done, &enc_lock);
    if (was && OpenIMP_T32_IvdcMode())
        OpenIMP_T32_IvdcEncStop(chn_ivdc_codec(e), e->ivdc);
    if (was)
        e->ivdc = 0;
}

int IMP_Encoder_DestroyChn(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (!e) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    chn_stop_locked(e);
    e->registered = 0;
    e->quit = 1;
    pthread_cond_signal(&e->work);
    pthread_mutex_unlock(&enc_lock);
    if (e->thread_up)
        pthread_join(e->thread, NULL);
    chn_free_bufs(e);
    OpenIMP_T32_VpuBufFree(&e->job);
    h264_free(e);
    OpenIMP_T32_VpuRelease(&e->vpu);
    OpenIMP_T32_VpuClose();
    close(e->pipefd[0]);
    close(e->pipefd[1]);
    pthread_cond_destroy(&e->work);
    pthread_cond_destroy(&e->done);
    IMP_LOG_INFO(TAG, "chn %d off: %u frames, %u coded, %u skipped, %u over-limit retries, %u errors"
                 ", %u BSFULL sections, %u stream overflows", encChn, e->frames, e->encoded, e->skipped,
                 e->retries, e->errors, e->avc.sections, e->avc.overflows);
    pthread_mutex_lock(&enc_lock);
    memset(e, 0, sizeof(*e));
    pthread_mutex_unlock(&enc_lock);
    return 0;
}

int IMP_Encoder_GetChnAttr(int encChn, IMPEncoderChnAttr *attr)
{
    struct t32_enc_chn *e;

    if (!attr)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *attr = e->attr;
        if (!e->h264)
            attr->rcAttr.attrRcMode.attrJPEGFixQp.qp = (uint32_t)e->qp;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetChnEncType(int encChn, IMPPayloadType *encType)
{
    struct t32_enc_chn *e;

    if (!encType)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *encType = e->attr.encAttr.enType;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetJpegQp(int encChn, int jpegQp)
{
    struct t32_enc_chn *e;

    if (jpegQp < 0 || jpegQp > T32_JPEG_MAX_QP)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        e->qp = jpegQp;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetJpegQp(int encChn, int *jpegQp)
{
    struct t32_enc_chn *e;

    if (!jpegQp)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *jpegQp = e->qp;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetJpegBsSize(uint32_t jpegBsSize)
{
    enc_jpeg_bs_size = jpegBsSize;
    return 0;
}

int IMP_Encoder_SetChnAttrRcMode(int encChn, IMPEncoderAttrRcMode *rcMode)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (!rcMode)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->h264) {
        e->attr.rcAttr.attrRcMode = *rcMode;
        h264_set_qp(e, rcMode);
        e->avc.rc_restart = 1;          /* the controller restarts at the next IDR, as i264e */
        rc = 0;
    } else if (e && rcMode->rcMode == IMP_ENC_RC_MODE_FIXQP) {
        e->attr.rcAttr.attrRcMode = *rcMode;
        e->qp = jpeg_qp_of((int)rcMode->attrJPEGFixQp.qp);
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetChnAttrRcMode(int encChn, IMPEncoderAttrRcMode *rcMode)
{
    struct t32_enc_chn *e;

    if (!rcMode)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *rcMode = e->attr.rcAttr.attrRcMode;
        if (!e->h264)
            rcMode->attrJPEGFixQp.qp = (uint32_t)e->qp;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetChnFrmRate(int encChn, const IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (!rate || !rate->frmRateNum || !rate->frmRateDen)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        e->attr.rcAttr.outFrmRate = *rate;
        e->rate_acc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetChnFrmRate(int encChn, IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (!rate)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *rate = e->attr.rcAttr.outFrmRate;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_SetMaxStreamCnt(int encChn, int cnt)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (cnt < 1 || cnt > T32_ENC_MAX_STREAMS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && !e->recv && !e->busy && !e->pending && !e->rcount) {
        int i, held = 0;
        for (i = 0; i < e->nslots; i++)
            held |= e->slot[i].state != SLOT_FREE;
        if (!held) {
            chn_free_bufs(e);
            e->max_streams = cnt;
            rc = chn_alloc_bufs(e, cnt);
        }
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetMaxStreamCnt(int encChn, int *cnt)
{
    struct t32_enc_chn *e;

    if (!cnt)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        *cnt = e->nslots;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/* ---- registration and receiving ---------------------------------------------- */
int IMP_Encoder_RegisterChn(int encGroup, int encChn)
{
    struct t32_enc_chn *e;
    int rc = -1;

    if (encGroup < 0 || encGroup >= T32_ENC_GROUPS)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && !e->registered && enc_grp[encGroup].created) {
        e->grp = encGroup;
        e->registered = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "RegisterChn(%d, %d) failed", encGroup, encChn);
    return rc;
}

int IMP_Encoder_UnRegisterChn(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->registered) {
        chn_stop_locked(e);
        e->registered = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_StartRecvPic(int encChn)
{
    struct t32_enc_chn *e;
    int rc = -1, grp = -1, fs = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->registered)
        grp = e->grp;
    pthread_mutex_unlock(&enc_lock);
    /* IVDC direct mode (#301): a channel fed by FrameSource channel 0
     * (also through OSD) codes its pictures over IVDC */
    if (grp >= 0 && OpenIMP_T32_IvdcMode())
        fs = OpenIMP_T32_FsSourceOf(DEV_ID_ENC, grp);

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->registered) {
        if (!e->recv && OpenIMP_T32_IvdcMode()) {
            e->ivdc = OpenIMP_T32_IvdcMode() == 1 && fs == 0;
            OpenIMP_T32_IvdcEncStart(chn_ivdc_codec(e), e->attr.encAttr.picWidth,
                                     e->attr.encAttr.picHeight, e->ivdc);
            if (e->ivdc)
                IMP_LOG_INFO(TAG, "chn %d: pictures over IVDC (direct mode)", encChn);
        }
        e->recv = 1;
        e->rate_acc = 0;
        e->src_fs = -1;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_StopRecvPic(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        chn_stop_locked(e);
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/* ---- streams ------------------------------------------------------------------ */
static int wait_ready_locked(struct t32_enc_chn *e, int64_t timeout_ms)
{
    struct timespec t;

    if (e->rcount)
        return 0;
    if (timeout_ms == 0)
        return -1;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += timeout_ms / 1000;
        t.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) {
            t.tv_sec++;
            t.tv_nsec -= 1000000000L;
        }
    }
    while (!e->rcount && e->created && !e->quit) {
        if (timeout_ms < 0)
            pthread_cond_wait(&e->done, &enc_lock);
        else if (pthread_cond_timedwait(&e->done, &enc_lock, &t) == ETIMEDOUT)
            break;
    }
    return e->rcount ? 0 : -1;
}

int IMP_Encoder_PollingStream(int encChn, uint32_t timeoutMsec)
{
    struct t32_enc_chn *e;
    int rc = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        rc = wait_ready_locked(e, (int64_t)timeoutMsec);
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetStream(int encChn, IMPEncoderStream *stream, int block)
{
    struct t32_enc_chn *e;
    struct t32_enc_slot *s;
    char c;
    int si;

    if (!stream)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (!e || wait_ready_locked(e, block ? -1 : 0) != 0) {
        pthread_mutex_unlock(&enc_lock);
        return -1;
    }
    si = e->ready[e->rhead];
    e->rhead = (e->rhead + 1) % T32_ENC_MAX_STREAMS;
    e->rcount--;
    s = &e->slot[si];
    s->state = SLOT_USER;
    if (read(e->pipefd[0], &c, 1) != 1)
        IMP_LOG_DBG(TAG, "stream notify pipe empty");
    memset(stream, 0, sizeof(*stream));
    stream->pack = s->pack;
    stream->packCount = s->npacks;
    stream->seq = s->seq;
    stream->refType = s->ref;
    pthread_mutex_unlock(&enc_lock);
    return 0;
}

int IMP_Encoder_ReleaseStream(int encChn, IMPEncoderStream *stream)
{
    struct t32_enc_chn *e;
    int i, rc = -1;

    if (!stream || !stream->pack)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    for (i = 0; e && i < e->nslots; i++)
        if (stream->pack == e->slot[i].pack && e->slot[i].state == SLOT_USER) {
            e->slot[i].state = SLOT_FREE;
            rc = 0;
            break;
        }
    pthread_mutex_unlock(&enc_lock);
    if (rc)
        IMP_LOG_ERR(TAG, "ReleaseStream(%d): not a stream of this channel", encChn);
    return rc;
}

int IMP_Encoder_FlushStream(int encChn)
{
    struct t32_enc_chn *e;
    char c;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    while (e && e->rcount) {
        e->slot[e->ready[e->rhead]].state = SLOT_FREE;
        e->rhead = (e->rhead + 1) % T32_ENC_MAX_STREAMS;
        e->rcount--;
        if (read(e->pipefd[0], &c, 1) != 1)
            break;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_Query(int encChn, IMPEncoderCHNStat *stat)
{
    struct t32_enc_chn *e;
    int i;

    if (!stat)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        memset(stat, 0, sizeof(*stat));
        stat->registered = e->registered;
        stat->leftPics = (uint32_t)(e->pending + e->busy);
        for (i = 0; i < e->rcount; i++)
            stat->leftStreamBytes += e->slot[e->ready[(e->rhead + i) % T32_ENC_MAX_STREAMS]].length;
        stat->leftStreamFrames = (uint32_t)e->rcount;
        stat->curPacks = e->rcount ? e->slot[e->ready[e->rhead]].npacks : 0;
        stat->work_done = e->recv ? 0 : 1;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

int IMP_Encoder_GetFd(int encChn)
{
    struct t32_enc_chn *e;
    int fd = -1;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        fd = e->pipefd[0];              /* readable while a stream is ready */
    pthread_mutex_unlock(&enc_lock);
    return fd;
}

int IMP_Encoder_RequestIDR(int encChn)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->h264)                   /* every JPEG picture stands alone */
        e->avc.idr_req = 1;
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}
