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
#include <sched.h>
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
#include "t32/t32_gop33.h"
#include "t32/openimp_t32_rc.h"
#include "t32/openimp_t32_hera_h265.h"
#include "t32/openimp_t32_encq.h"
#ifdef PLATFORM_T33
#include "t32/openimp_t33_hera.h"
#include "t32/openimp_t33_hera_h265.h"
#include "t32/openimp_t33_sections.h"
#endif
int OpenIMP_T32_EncTakeRdBufShareOff(int encChn);    /* openimp_t32_fs_enc_ext.c: SetRdBufShare(0) */
int OpenIMP_T32_EncJpegMultiSection(void);           /* openimp_t32_fs_enc_ext.c: SetJpegMultiSectionMode */
void OpenIMP_T32_EncSections(int jpeg, uint32_t jpeg_bs, uint32_t *section_kb, uint32_t *bytes);   /* #784 group 3 */
#include "t32/openimp_t32_ivdc.h"
#include "t32/openimp_t32_vpu.h"
#include "t32/openimp_t32_enc_e1.h"
#ifndef PLATFORM_T33
int OpenIMP_T32_EncQpgAIOn(int encChn);         /* SetChnQpgAIEnable (openimp_t32_fs_enc_ext.c) */
#endif
#ifdef PLATFORM_T33
#include "t32/openimp_t33_resize.h"
#endif
#include "t32/openimp_t32_gdr.h"

#if !defined(PLATFORM_T32)
#error "openimp_t32_encoder.c is the PLATFORM_T32 build only"
#endif

#define TAG "T32-ENC"

#define T32_ENC_GROUPS      6
#define T32_ENC_CHNS        9
#define T32_ENC_MAX_STREAMS T32_ENCQ_MAX
#define T32_ENC_DEF_STREAMS 3
#define T32_JPEG_DEF_QP     40          /* vendor: initial QP out of 0..98 */
#define T32_JPEG_MAX_QP     98
#define T32_JPEG_RETRY_STEP 8           /* quality index step after an over-limit picture */
#define T32_JPEG_BS_1080P   512000u     /* vendor default stream buffer / limit at 1920x1080 */
#define T32_JPEG_VPU_ID     (OI_T32_VPU_HELIX_ID | 1u)
#define T32_JPEG_VERSION_REG 0x132f0200u

#define T32_HERA_VPU_ID     OI_T32_VPU_RADIX_ID  /* the vendor's START: no instance bit */
#define T32_HERA_SECTION_KB 250u        /* vendor: two halves of 250 KiB */
#define T32_H264_DEF_QP     30          /* an initial QP outside 0..51 */
#define T32_H264_PACKS      6           /* (VPS,) SPS, PPS, up to 2 user-data SEIs, slice */
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

struct t32_enc_slot {
    T32VpuBuf bs;
    IMPEncoderPack pack[T32_H264_PACKS];
    uint32_t npacks, length;
    IMPRefType ref;
    uint32_t seq;
};

struct t32_h264 {                       /* H.264 / H.265 channel state */
    uint8_t mq;                         /* #784: H.265 MotionQuality from the last IDR on */
    T32VpuBuf list, ring, ebs, aux[T32_HERA5_AUX];
    T32VpuBuf grey;                     /* #788: Color2Grey chroma plane (0x80), T33 */
    T32H264Seq seq;
    T32H265Seq seq5;
    uint8_t vps[T32_H265_PARAM_MAX], sps[T32_H265_PARAM_MAX], pps[T32_H265_PARAM_MAX];
    size_t vps_len, sps_len, pps_len;
    int last_p;                         /* H.265: the last picture was a P */
    uint32_t pictures;                  /* pictures coded (H.264 and H.265) */
    int qp_i, qp_p;
    uint32_t gop, gop_pos, idr_pic_id, section_kb;
    uint32_t bs_bytes;                  /* the section buffer: 2 x section_kb KiB, or one picture buffer at section_kb 0 (#784) */
    int32_t gdr_d;                      /* H.264: pictures since the last RequestGDR, -1 none (#784) */
    int have_ref, idr_req;
    uint32_t ref_y, ref_c;              /* the last reconstruction */
    uint32_t sections, overflows;
    uint32_t stats[T32_HERA_STATS];     /* the engine's statistics of the last picture */
    T32Rc rc;                           /* rate control (#295), off at FIXQP */
    int rc_restart;                     /* SetChnAttrRcMode: restart at the next IDR */
    uint32_t set_mask;                  /* run-time setters not yet handed to the controller (bit id - 0x11) */
    uint32_t set_args[4][4];            /* their i264e_set_param arguments */
#ifdef PLATFORM_T33
    uint32_t hdr_set;                   /* HERA v1.6: the header set the next picture writes */
    uint32_t odma_y, odma_c;            /* HERA ODMA write position after the last picture */
    uint32_t recon_y, recon_c;          /* the last picture's reconstruction (the next reference) */
#endif
    int noshare;                        /* SetRdBufShare(0) before CreateChn (#736; T32: #99 row 37) */
    uint32_t pp;                        /* no share: the picture buffer the next picture writes */
    int bs_engine;                      /* ebs is the engine's shared buffer: 1 HERA, 2 JPEG */
    /* vendor GetChnAttrRcMode reads the encoder's live parameters (i264e_get_param 3 =
     * i264e_reconfig_rc_get, +0xdc..), which the run-time setters change only at the next IDR
     * (i264e_idr_reconfig): until then the getters show @rb, the attribute before the setter */
    int rb_hold;
    IMPEncoderAttrRcMode rb;
    T32Gop33 glue;                      /* IMP glue: rate and GOP sent to the encoder (encoder_get_fps_mask) */
    int rate_pend;                      /* a rate / GOP was sent: the encoder takes it at the next IDR */
    uint32_t rc_num, rc_den;            /* the rate the encoder (rate control, SPS) runs with */
};

struct t32_enc_chn {
    int created, registered, recv, quit;
    int grp;
    IMPEncoderCHNAttr attr;
    int bs_shared;                      /* H.26x: the engine's shared section buffer (not with OPENIMP_T32_HERA_SECTION_KB) */
    int qp;                             /* jpegQp 0..98 */
    int h264;                           /* PT_H264 or PT_H265 on HERA */
    int hevc;                           /* PT_H265 */
    struct t32_h264 avc;
    T32VpuChannel vpu;
    T32VpuBuf job;
    struct t32_enc_slot slot[T32_ENC_MAX_STREAMS];
    int nslots, max_streams;
    uint32_t bs_size;
    struct t32_encq q;                  /* slot states + FIFO of coded streams (openimp_t32_encq.h) */
    uint32_t seq;
    int pending, busy;                  /* a frame of the group is waiting / in work */
    uint64_t rate_acc;                  /* frame-rate drop */
    int rate_due;                       /* the drop let a frame through, not coded yet */
    int src_fs;
    uint32_t src_num, src_den;
    pthread_t thread;
    int thread_up;
    pthread_cond_t work, done;
    int pipefd[2];
    uint32_t frames, encoded, skipped, retries, errors;
    int size_logged;
    int ivdc;                           /* fed by the IVDC direct channel (#301) */
    int yuv;                            /* stand-alone context (YuvInit / InputJpege, #752): no channel number */
    int yuv_mq;                         /* #784: MotionQuality_Yuv on for the next picture (H.265 P lists) */
    /* IMP_Encoder_Query of vendor 2.0.2.1 / 2.2.0 (#738; T32 H.264 / H.265: #99 row 44): the encoder thread writes
     * the channel's status words after every picture (0xdd6bc / 0xdebf4:
     * packs and the stream buffer size minus the stream's length), work_done
     * 0 while a picture is coded and 1 otherwise (0xdd140 / 0xdd36c, CreateChn
     * 0xe2ac0); GetStream / ReleaseStream leave them.  JPEG (#741): the
     * JPEG thread writes packs, bytes and leftStreamFrames (sem_getvalue of
     * the stream semaphore) only after its stream-ready notify (bal 0xdb1e0,
     * words at 0xdc504..0xdc520), and never sets work_done back to 1 */
    uint32_t q_packs, q_bytes, q_frames;
    int q_done, q_post;                 /* q_post: JPEG picture notified, words pending (T33) */
    /* #99: a FrameSource picture of another size than the channel is scaled on the CPU like the vendor
     * input thread (0xedba8, c_resize_c 0x34540): scratch picture in rmem + the coefficient table */
    T32VpuBuf scl;
    uint16_t *scl_tab;
    uint32_t scl_pw, scl_ph;
    int scl_cur;                        /* the picture being coded is the scaled one: its padding rows are computed */
#endif
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

/* the channel number of @e for the per-channel tables; -1 for a stand-alone context (#752), which the
 * tables (E1 SEI / ROI / VUI, SetRdBufShare) do not know */
static int chn_no(const struct t32_enc_chn *e)
{
    return e >= enc_chn && e < enc_chn + T32_ENC_CHNS ? (int)(e - enc_chn) : -1;
}

/*
 * T33 (#391), T32 (#743): one 2 x 250 KiB section buffer per engine, shared by all of
 * its channels as vendor libimp 2.0.2.1 / 2.2.0 have them (rmem "ivpuBs" for HERA,
 * "jpegBs" for JPEG); a channel holds the engine's lock from START until
 * the last part is copied out.  A channel with another section size
 * (OPENIMP_T32_HERA_SECTION_KB, test only) keeps its own buffer.
 */
static struct {
    pthread_mutex_t lock;
    T32VpuBuf buf;
    int refs;
    uint32_t bytes;                     /* the pool size (#784: multi-section size x 2 or Ivpu / JpegBsSize) */
} t33_bs[2] = { { PTHREAD_MUTEX_INITIALIZER, { { "", "", 0, 0, 0, 0, 0 }, 0 }, 0, 0 },
                { PTHREAD_MUTEX_INITIALIZER, { { "", "", 0, 0, 0, 0, 0 }, 0 }, 0, 0 } };
static pthread_mutex_t t33_bs_refs = PTHREAD_MUTEX_INITIALIZER;

/*
 * Test only (#742): OPENIMP_T33_BS_GUARD=<KiB> puts a guard area of that
 * size behind each shared section buffer, filled with T33_BS_GUARD_BYTE;
 * after every picture a changed guard byte (the engine wrote past the two
 * halves into rmem) is logged and the area filled again.
 */
#define T33_BS_GUARD_BYTE 0x5au
static uint32_t t33_bs_guard;

static void t33_bs_guard_fill(const T32VpuBuf *b, uint32_t bytes)
{
    uint8_t *g = (uint8_t *)t32_vpu_buf_va(b) + bytes;

    memset(g, T33_BS_GUARD_BYTE, t33_bs_guard);
    OpenIMP_T32_VpuFlush(g, t33_bs_guard, OI_T32_VPU_FLUSH_WBACK);
}

static void t33_bs_guard_check(struct t32_enc_chn *e)
{
    const T32VpuBuf *b = &t33_bs[e->avc.bs_engine - 1].buf;
    const uint8_t *g = (const uint8_t *)t32_vpu_buf_va(b) + t33_bs[e->avc.bs_engine - 1].bytes;
    uint32_t i, bad = 0, first = 0;

    if (!t33_bs_guard)
        return;
    OpenIMP_T32_VpuFlush(g, t33_bs_guard, OI_T32_VPU_FLUSH_INV);
    for (i = 0; i < t33_bs_guard; i++)
        if (g[i] != T33_BS_GUARD_BYTE && !bad++)
            first = i;
    if (bad) {
        IMP_LOG_ERR(TAG, "section buffer guard: %u of %u bytes changed (first at +%u)", bad, t33_bs_guard, first);
        t33_bs_guard_fill(b, t33_bs[e->avc.bs_engine - 1].bytes);
    }
}

static int t33_bs_get(struct t32_enc_chn *e)
{
    int k = e->h264 ? 0 : 1, rc = 0;

    pthread_mutex_lock(&t33_bs_refs);
    if (!t33_bs[k].refs) {
        const char *g = getenv("OPENIMP_T33_BS_GUARD");

        t33_bs_guard = g ? (uint32_t)atoi(g) * 1024u : 0u;
        t33_bs[k].bytes = e->avc.bs_bytes;
        rc = e->avc.bs_bytes ? OpenIMP_T32_VpuBufAlloc(&t33_bs[k].buf, e->avc.bs_bytes + t33_bs_guard,
                                                        k ? "t33_jpeg_bs" : "t33_hera_bs") : -1;
        if (rc == 0 && t33_bs_guard)
            t33_bs_guard_fill(&t33_bs[k].buf, t33_bs[k].bytes);
    }
    if (rc == 0) {
        t33_bs[k].refs++;
        e->avc.ebs = t33_bs[k].buf;
        e->avc.bs_engine = k + 1;
    }
    pthread_mutex_unlock(&t33_bs_refs);
    return rc;
}

static void t33_bs_put(struct t32_enc_chn *e)
{
    int k = e->avc.bs_engine - 1;

    if (k < 0)
        return;
    pthread_mutex_lock(&t33_bs_refs);
    if (--t33_bs[k].refs == 0)
        OpenIMP_T32_VpuBufFree(&t33_bs[k].buf);
    pthread_mutex_unlock(&t33_bs_refs);
    memset(&e->avc.ebs, 0, sizeof(e->avc.ebs));
    e->avc.bs_engine = 0;
}

static struct t32_enc_chn *chn_get(int n)
{
    return n >= 0 && n < T32_ENC_CHNS && enc_chn[n].created ? &enc_chn[n] : NULL;
}

/* #480: what openimp_t32_enc_e1.c needs to know of a channel */
static void chn_info(int n, const struct t32_enc_chn *e, T32EncChnInfo *ci)
{
    (void)n;
    memset(ci, 0, sizeof(*ci));
    ci->codec = e->hevc ? 2 : e->h264 ? 1 : 0;
    ci->recv = e->recv;
    ci->width = e->attr.encAttr.picWidth;
    ci->height = e->attr.encAttr.picHeight;
    ci->fps_num = e->attr.rcAttr.outFrmRate.frmRateNum;
    ci->fps_den = e->attr.rcAttr.outFrmRate.frmRateDen;
    ci->ud_cnt = e->attr.encAttr.userData.maxUserDataCnt;
    ci->ud_size = e->attr.encAttr.userData.maxUserDataSize;
}

int OpenIMP_T32_EncChnQuery(int encChn, T32EncChnInfo *ci)
{
    struct t32_enc_chn *e;

    pthread_mutex_lock(&enc_lock);
    e = encChn >= 0 && encChn <= 15 ? chn_get(encChn) : NULL;
    if (e)
        chn_info(encChn, e, ci);
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/* an IDR: settings deferred to it (VUI into the SPS); user-data SEI before a slice (#480) */
static void e1_at_idr(struct t32_enc_chn *e)
{
    struct t32_h264 *a = &e->avc;
    T32Vui v;
    int n = chn_no(e);
    int vui = OpenIMP_T32_E1AtIdr(n, &v), chg = vui;
    T32E1Hdr hd;

    /* #784: SetChnCrop (SPS frame cropping / conformance window), SetH264/H265TransCfg (PPS chroma QP offset) and
     * MotionQuality from this IDR on, as vendor 2.0.2.1 (QEMU t33-cache runs vfx / v265) and, checked against the
     * vendor 2.2.0 library in QEMU (#99, 2026-11-28: apitest t33e1 SPS / PPS bytes equal), on the T32 */
    OpenIMP_T32_E1Hdr(n, &hd);
    if (e->hevc) {
        /* the vendor keeps conformance_window_flag 1 with a zero window once a crop was switched
         * off again (apitest t33e1: SPS ..0169fe5a..); decoders show the full picture either way */
        if (!hd.crop && a->seq5.crop)
            hd.crop = 2;
        if (a->seq5.crop != hd.crop || a->seq5.crop_x != hd.crop_x || a->seq5.crop_y != hd.crop_y ||
            a->seq5.crop_w != hd.crop_w || a->seq5.crop_h != hd.crop_h) {
            a->seq5.crop = hd.crop;
            a->seq5.crop_x = hd.crop_x;
            a->seq5.crop_y = hd.crop_y;
            a->seq5.crop_w = hd.crop_w;
            a->seq5.crop_h = hd.crop_h;
            chg = 1;
        }
        if (a->seq5.chroma_qp_offset != hd.chroma265) {
            a->seq5.chroma_qp_offset = hd.chroma265;
            a->pps_len = t32_h265_pps(&a->seq5, a->pps);
        }
        a->mq = hd.motion_quality;      /* #784: P lists from this IDR on (vendor QEMU v5mq1 / v5mq0) */
    } else {
        if (a->seq.crop != hd.crop || a->seq.crop_x != hd.crop_x || a->seq.crop_y != hd.crop_y ||
            a->seq.crop_w != hd.crop_w || a->seq.crop_h != hd.crop_h) {
            a->seq.crop = hd.crop;
            a->seq.crop_x = hd.crop_x;
            a->seq.crop_y = hd.crop_y;
            a->seq.crop_w = hd.crop_w;
            a->seq.crop_h = hd.crop_h;
            chg = 1;
        }
        if (a->seq.chroma_qp_offset != hd.chroma264) {
            a->seq.chroma_qp_offset = hd.chroma264;
            a->pps_len = t32_h264_pps(&a->seq, a->pps);
        }
    }
    if (!chg)
        return;
    if (e->hevc) {
        if (vui)
            a->seq5.vui = v;
        a->sps_len = t32_h265_sps(&a->seq5, a->sps);
    } else {
        if (vui)
            a->seq.vui = v;
        a->sps_len = t32_h264_sps(&a->seq, a->sps);
    }
}

static size_t e1_sei(struct t32_enc_chn *e, uint8_t *out, size_t cap)
{
    return OpenIMP_T32_E1Sei(chn_no(e), e->hevc, out, cap);
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
/*
 * H.264 codes whole macroblocks, so the engine reads the source down to the
 * 16-aligned height (360 -> 368).  The capture driver writes only the picture
 * rows; the rows below hold whatever the buffer held before (#882: on the T32
 * module 0x80 fill or an older picture, different in each capture buffer, so
 * every P picture coded the last macroblock row again: 1.7-2.5x the vendor's
 * P sizes at the same QP).  Vendor libimp 2.2.0 hands the engine the buffer
 * with the last picture row repeated into those rows, luma and chroma
 * (device dump .136: at DQBUF rows 360..367 still hold the buffer's previous
 * content, at the HERA RUN they equal row 359 of the new picture).  Done here
 * the same way: copy the last row down, write the cache lines back.
 */
static void enc_pad_rows(const IMPFrameInfo *f, uint32_t stride)
{
    uint32_t h = f->height, h16 = ALIGN16(f->height), r;
    uint8_t *y = (uint8_t *)(uintptr_t)f->virAddr, *uv;

    if (!y || h < 2u || h == h16 || !stride)
        return;
    uv = y + stride * h16;
    for (r = h; r < h16; r++)
        memcpy(y + r * stride, y + (h - 1u) * stride, stride);
    for (r = h / 2u; r < h16 / 2u; r++)
        memcpy(uv + r * stride, uv + (h / 2u - 1u) * stride, stride);
    OpenIMP_T32_VpuFlush(y + h * stride, (h16 - h) * stride, OI_T32_VPU_FLUSH_WBACK);
    OpenIMP_T32_VpuFlush(uv + h / 2u * stride, (h16 / 2u - h / 2u) * stride, OI_T32_VPU_FLUSH_WBACK);
}

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
        /* a caller's NV12 (YuvEncode / InputJpege, #752): UV after the even height, vendor 2.0.2.1
         * YuvEncode 0x104b4c job +0x64 = ((h + 1) & ~1) * ALIGN16(w) + Y, InputJpege 0x105430 C = Y + w * h */
        *c = f->phyAddr + *stride * (e->yuv ? (f->height + 1u) & ~1u : ALIGN16(f->height));
        if (e->h264 && !e->yuv
#ifdef PLATFORM_T33
            && !e->scl_cur      /* c_resize_c computes the rows below the picture (QEMU A/B, crop run sc-v) */
#endif
            )
            enc_pad_rows(f, *stride);
    }
    e->vpu.node.frame_type = ivdc ? OI_T32_FRAME_TYPE_IVDC : 0;
    e->vpu.node.sensor_id = 0;          /* one sensor (direct mode 1) */
    /* the vendor's vtype (2.2.0 and 2.0.2.1 traces): H.264 0x91, H.265
     * 0x80, JPEG 0; the radix driver writes it to HERA CFGC 0x0c (#391) */
    e->vpu.node.vtype = e->hevc ? 0x80u : e->h264 ? 0x91u : 0u;
}

/* #788 (beyond vendor, T33): Color2Grey.  Vendor 2.0.2.1 writes a user-space address into the HERA chroma
 * source EFE_RAWC; here the engine reads a real DMA plane of 0x80 (neutral chroma), stride = the source
 * stride, height/2 rows, so the coded picture is grey whatever the source chroma holds. */
static void enc_grey_source(struct t32_enc_chn *e, uint32_t stride, uint32_t h, uint32_t *c)
{
#ifdef PLATFORM_T33
    uint32_t need = stride * (h / 2u);

    if (!OpenIMP_T32_E1GreyChroma(chn_no(e)))
        return;
    if (e->avc.grey.size < need) {
        OpenIMP_T32_VpuBufFree(&e->avc.grey);
        if (OpenIMP_T32_VpuBufAlloc(&e->avc.grey, need, "t33_grey_c") != 0)
            return;                     /* no buffer: keep the source chroma */
        memset(t32_vpu_buf_va(&e->avc.grey), 0x80, e->avc.grey.size);
    }
    *c = t32_vpu_buf_pa(&e->avc.grey);
#else
    (void)e; (void)stride; (void)h; (void)c;
#endif
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

static inline int sec_take(struct t32_enc_chn *e, uint8_t *out, uint32_t cap, uint32_t *at,
                           uint32_t from, uint32_t to, int half);

#ifdef PLATFORM_T33
/*
 * T33 (#391): vendor libimp 2.0.2.1 runs the JPEG engine in sections:
 * the job's stream address is the 2 x 250 KiB section buffer, no length
 * limit (limit_bs_len 0, bsfull_jpeg_intr_size 250), START, then
 * WAIT_BSFULL_COMPLETE until the end of the picture (SET_BSFULL_REG after
 * each full half); the parts are copied into the slot.  A picture over the
 * slot is coded again with a lower quality, as on the T32.
 */
static int jpeg_encode_sections(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s,
                                uint32_t *len)
{
    uint32_t index = (uint32_t)(T32_JPEG_MAX_QP - e->qp);
    T32VpuResult r;

    for (;;) {
        T32IocJpegRegInfo *j = t32_vpu_buf_va(&e->job);
        uint32_t at = 0, k = 0, sec = e->avc.section_kb * 1024u;
        int full = 0;

        jpeg_fill_job(e, f, &e->avc.ebs, index);
        /* control word as vendor 2.0.2.1 on the T33L silicon (job dump,
         * #510): no length limit (bit 16) and no over-limit stop (bit 26),
         * bit 29 set (sections); with the T32 word the engine ended every
         * job with status 0x3 and no data */
        /* SetJpegMultiSectionMode(0) (#784, QEMU vjm0 vs vendor): the
         * limit and over-limit stop (bits 16 / 26), limit = SetJpegBsSize,
         * no BSFULL fields (src/t32/openimp_t33_sections.h) */
        j->jpeg_ctrl = t33_jpeg_ctrl(j->jpeg_ctrl, e->avc.section_kb);
        j->limit_bs_len = t33_jpeg_limit(e->avc.section_kb, e->avc.bs_bytes);
        j->bsfull_jpeg_intr_size = e->avc.section_kb;
        OpenIMP_T32_VpuFlush(j, T32_JPEG_JOB_BYTES, OI_T32_VPU_FLUSH_WBACK);
        if (OpenIMP_T32_VpuStart(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job)) != 0)
            return -1;
        for (;;) {
            if (OpenIMP_T32_VpuWaitBsFull(&e->vpu, &r) != 0)
                return -1;
            if (r.rc != OI_T32_VPU_RET_HERA_BSFULL)
                break;
            if (!sec) {                 /* no section stop is set (#784): not expected */
                full = 1;
                break;
            }
            /* a full half, as vendor ijpege_encode (#742) */
            if (r.output_len >= (k + 1u) * sec) {
                if (sec_take(e, t32_vpu_buf_va(&s->bs), s->bs.size, &at, k * sec, (k + 1u) * sec,
                             (int)(k & 1u)) != 0)
                    full = 1;
                k++;
                e->avc.sections++;
            }
            if (OpenIMP_T32_VpuSetBsFull(&e->vpu) != 0)
                return -1;
        }
        if (r.rc == 0 && r.output_len > 0 && !full &&
            sec_take(e, t32_vpu_buf_va(&s->bs), s->bs.size, &at, k * sec, r.output_len, (int)(k & 1u)) == 0) {
            *len = at;
            return 0;
        }
        if (r.rc != 0 || !r.output_len || index >= T32_JPEG_MAX_QP) {
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

/* the JPEG engine's shared section buffer is held for the whole picture */
static int jpeg_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s,
                       uint32_t *len)
{
    int rc;

    pthread_mutex_lock(&t33_bs[1].lock);
    rc = jpeg_encode_sections(e, f, s, len);
    if (e->avc.bs_engine)
        t33_bs_guard_check(e);
    pthread_mutex_unlock(&t33_bs[1].lock);
    return rc;
}
#else
/*
 * T32 (#743): the PRJ007 helix driver has no JPEG section mode (wait_bs_complete / set_bsfull_reg only
 * with CONFIG_SOC_PRJ008; vendor 2.2.0 ijpege_encode 0x7b578 takes its whole-picture path, codec
 * buffer + 1320 / limit + 1312), so the engine writes into the channel's rmem output buffer (ebs) and
 * the picture is copied into the stream slot (process memory, as the vendor's malloc'd one).  A
 * picture over the buffer is coded again with a lower quality.
 */
static int jpeg_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s,
                       uint32_t *len)
{
    uint32_t index = (uint32_t)(T32_JPEG_MAX_QP - e->qp);
    T32VpuResult r;

    for (;;) {
        jpeg_fill_job(e, f, &e->avc.ebs, index);
        OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&e->job), T32_JPEG_JOB_BYTES, OI_T32_VPU_FLUSH_WBACK);
        if (vpu_test_mode()) {          /* the two-step path: START, WAIT_COMPLETE */
            if (OpenIMP_T32_VpuStart(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job)) != 0 ||
                OpenIMP_T32_VpuWait(&e->vpu, &r) != 0)
                return -1;
        } else if (OpenIMP_T32_VpuRun(&e->vpu, T32_JPEG_VPU_ID, t32_vpu_buf_pa(&e->job), &r) != 0) {
            return -1;
        }
        if (r.rc == 0 && r.output_len > 0 && r.output_len <= e->avc.ebs.size && r.output_len <= s->bs.size) {
            OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&e->avc.ebs), r.output_len, OI_T32_VPU_FLUSH_INV);
            memcpy(t32_vpu_buf_va(&s->bs), t32_vpu_buf_va(&e->avc.ebs), r.output_len);
            *len = r.output_len;
            return 0;
        }
        if ((r.rc != OI_T32_VPU_RET_JPEG_BSLESS && r.rc != 0) || index >= T32_JPEG_MAX_QP) {
            IMP_LOG_ERR(TAG, "JPEG job: result 0x%lx status 0x%x length %u", r.rc, r.status,
                        r.output_len);
            return -1;
        }
        index = index + T32_JPEG_RETRY_STEP > T32_JPEG_MAX_QP ? T32_JPEG_MAX_QP :
                index + T32_JPEG_RETRY_STEP;
        e->retries++;
        IMP_LOG_WARN(TAG, "picture over the %u byte buffer, again at quality index %u",
                     e->avc.ebs.size, index);
    }
}
#endif

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

/* the engine's next part (bytes @from..@to of the picture, in half @half) copied onto the slot as it is:
 * 0, -1 when the slot is full (JPEG; T33 HERA v1.6) */
static inline int sec_take(struct t32_enc_chn *e, uint8_t *out, uint32_t cap, uint32_t *at,
                           uint32_t from, uint32_t to, int half)
{
    uint32_t half_bytes = e->avc.section_kb ? e->avc.section_kb * 1024u : e->avc.bs_bytes, n = to - from;
    const uint8_t *src = (const uint8_t *)t32_vpu_buf_va(&e->avc.ebs) + (half ? half_bytes : 0u);

    if (to < from || n > half_bytes || *at + n > cap)
        return -1;
    OpenIMP_T32_VpuFlush(src, n, OI_T32_VPU_FLUSH_INV);
    memcpy(out + *at, src, n);
    *at += n;
    return 0;
}

/* the same for H.264 / H.265; the T32 HERA leaves the emulation prevention to us */
static int h264_take(struct t32_enc_chn *e, uint8_t *out, uint32_t cap, uint32_t *at,
                     uint32_t from, uint32_t to, int half, int *esc)
{
#ifdef PLATFORM_T33
    /* HERA v1.6: the core added the emulation prevention bytes (SDE bit 7) */
    (void)esc;
    return sec_take(e, out, cap, at, from, to, half);
#else
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
#endif
}

static void h264_pack(struct t32_enc_slot *s, uint32_t off, uint32_t len, int type, int64_t ts)
{
    IMPEncoderPack *k = &s->pack[s->npacks++];

    memset(k, 0, sizeof(*k));
    k->phyAddr = t32_vpu_buf_pa(&s->bs) ? t32_vpu_buf_pa(&s->bs) + off : 0u;  /* T33: process memory */
    k->virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&s->bs) + off;
    k->length = len;
    k->timestamp = ts;
    k->dataType.h264Type = (IMPEncoderH264NaluType)type;
}

static int hera_run(struct t32_enc_chn *e, uint32_t pairs, uint8_t *out, uint32_t cap, uint32_t *at,
                    int *esc, const uint32_t *stat_regs, unsigned nstats);

/* hera_run under the HERA section buffer's lock while it is shared (T33 #391, T32 #743) */
static int hera_run_shared(struct t32_enc_chn *e, uint32_t pairs, uint8_t *out, uint32_t cap, uint32_t *at,
                           int *esc, const uint32_t *stat_regs, unsigned nstats)
{
    int rc;

    if (e->avc.bs_engine)
        pthread_mutex_lock(&t33_bs[e->avc.bs_engine - 1].lock);
    rc = hera_run(e, pairs, out, cap, at, esc, stat_regs, nstats);
    if (e->avc.bs_engine) {
        t33_bs_guard_check(e);
        pthread_mutex_unlock(&t33_bs[e->avc.bs_engine - 1].lock);
    }
    return rc;
}

/* start the channel's rate control from @m (#295); @again: a rate control
 * set at run time (SetChnAttrRcMode), taken on top of the current one as
 * the vendor's i264e does */
static void rc_setup(struct t32_enc_chn *e, const IMPEncoderAttrRcMode *m, int again)
{
    struct t32_h264 *a = &e->avc;
    const IMPEncoderCHNAttr *at = &e->attr;
    int r = (again ? t32_rc_reconfig : t32_rc_init)(&a->rc, m, at->encAttr.picWidth, at->encAttr.picHeight,
                                                    at->encAttr.bufSize, a->gop, a->rc_num, a->rc_den, e->hevc);

#ifndef PLATFORM_T33
    /* #977: a new controller keeps the SetChnMapRoi / QpgAI maps (vendor: P+0x250.. survive
     * i264e_ratecontrol_init; H.265 i265e+0x1a0.., +0x2d4) */
    if (r == 0) {
        const uint8_t *mp;
        uint32_t ms;

        T32E1QpgAI qa;

        (void)OpenIMP_T32_E1MapTake(chn_no(e), 0, &mp, &ms);
        t32_rc_map_roi(&a->rc, mp, ms);
        (void)OpenIMP_T32_E1QpgAITake(chn_no(e), 0, &qa);
        if (e->hevc)
            qa.map = NULL;              /* I265e: rcStart clears +0x2d8 after the picture it went into */
        t32_rc_qpgai(&a->rc, OpenIMP_T32_EncQpgAIOn(chn_no(e)), qa.map, qa.size, qa.width, qa.height, qa.fore,
                     qa.mark);
    }
#endif
    if (r != 0)
        IMP_LOG_WARN(TAG, "rate control init failed: fixed QP %d", a->qp_p);
    else if (a->rc.on)
        IMP_LOG_INFO(TAG, "%s rate control: mode %d -> eprc %u, %u kbit/s, qp [%u,%u], gop %u",
                     e->hevc ? "H.265" : "H.264", (int)m->rcMode, a->rc.params.rc_mode,
                     a->rc.params.rc_mode == EPRC_MODE_CBR ? a->rc.params.bitrate : a->rc.params.max_bitrate,
                     a->rc.params.min_qp, a->rc.params.max_qp, a->gop);
}

/* before every coded picture: the frame rate and GOP the IMP glue sends to
 * the encoder (vendor 2.2.0 encoder_get_fps_mask, src/t32/t32_gop33.h); 1:
 * the picture is an IDR.  The channel attribute shows what was sent. */
static int glue_picture(struct t32_enc_chn *e)
{
    struct t32_h264 *a = &e->avc;
    uint32_t sn, sd;
    unsigned r;

    pthread_mutex_lock(&enc_lock);
    sn = e->src_num;
    sd = e->src_den;
    if (!sn || !sd) {                   /* the source rate not known: the channel's */
        sn = a->glue.user_num;
        sd = a->glue.user_den;
    }
    r = t32_gop33_step(&a->glue, sn, sd);
    if (r & (T32_GOP33_RATE | T32_GOP33_GOP)) {
        a->rate_pend = 1;
        e->attr.rcAttr.outFrmRate.frmRateNum = a->glue.num;
        e->attr.rcAttr.outFrmRate.frmRateDen = a->glue.den;
        e->attr.rcAttr.maxGop = a->glue.gop;
    }
    pthread_mutex_unlock(&enc_lock);
    if (r & T32_GOP33_ZERO)
        IMP_LOG_WARN(TAG, "GOP under one second of %u/%u fps: kept at %u pictures (the vendor traps here)",
                     a->glue.num, a->glue.den, a->glue.gop);
    return (r & T32_GOP33_IDR) != 0;
}

/* at the IDR: a new rate / GOP into the encoder (i264e_idr_reconfig: fps,
 * keyint, the parameter sets, i264e_ratecontrol_init); 1: the rate control
 * restarts */
static int rate_take(struct t32_enc_chn *e)
{
    struct t32_h264 *a = &e->avc;
    uint32_t g, n, d;
    int take;

    pthread_mutex_lock(&enc_lock);
    take = a->rate_pend;
    a->rate_pend = 0;
    g = a->glue.gop;
    n = a->glue.num;
    d = a->glue.den;
    pthread_mutex_unlock(&enc_lock);
    if (!take || (g == a->gop && n == a->rc_num && d == a->rc_den))
        return 0;
    a->gop = g;
    if (n != a->rc_num || d != a->rc_den) {
        a->rc_num = n;
        a->rc_den = d;
        if (e->hevc) {
            a->seq5.fps_num = n;
            a->seq5.fps_den = d;
            a->vps_len = t32_h265_vps(&a->seq5, a->vps);
            a->sps_len = t32_h265_sps(&a->seq5, a->sps);
            a->pps_len = t32_h265_pps(&a->seq5, a->pps);
        } else {
            a->seq.fps_num = n;
            a->seq.fps_den = d;
            a->sps_len = t32_h264_sps(&a->seq, a->sps);
            a->pps_len = t32_h264_pps(&a->seq, a->pps);
        }
    }
    IMP_LOG_INFO(TAG, "rate %u/%u, GOP %u from this IDR on", n, d, g);
    return 1;
}

/* the picture's QP: the controller's, else the fixed / initial QP */
static int rc_picture_qp(struct t32_enc_chn *e, int idr)
{
    struct t32_h264 *a = &e->avc;
    int qp = idr ? a->qp_i : a->qp_p, q;

    {
        uint32_t mask, args[4][4];
        unsigned k;

        /* run-time setters (H.264; T33 H.265 too, #718): to the controller, which takes them at the next IDR */
        pthread_mutex_lock(&enc_lock);
        mask = a->set_mask;
        memcpy(args, a->set_args, sizeof(args));
        a->set_mask = 0;
        pthread_mutex_unlock(&enc_lock);
        for (k = 0; k < 4; k++)
            if (mask & 1u << k) {
#ifdef PLATFORM_T33
                if (t32_rc_set_param(&a->rc, 0x11u + k, args[k]) != 0 && e->hevc) {
                    pthread_mutex_lock(&enc_lock);
                    a->rc_restart = 1;  /* H.265 without a controller */
                    pthread_mutex_unlock(&enc_lock);
                }
#else
                (void)t32_rc_set_param(&a->rc, 0x11u + k, args[k]);
#endif
            }
    }
    /* #784 group 2: QpgMode before every picture, RcTrigLevel / SuperFrameCfg at the IDR
     * (vendor 2.0.2.1 i264e_reconfig / i264e_idr_reconfig: controller restart); on the T32 (#99, QEMU A/B against the vendor 2.2.0
     * library, CBR 640x368: Color2Grey, HSkip, SetChnCrop and SetH264TransCfg restart the controller at the next IDR) */
    T32E1Rc rcp;
    int rc_flags = OpenIMP_T32_E1RcTake(chn_no(e), idr, &rcp);
    int rc_again = t32_rc_idr_params(&a->rc, (unsigned)rc_flags, rcp.trig, rcp.sf, rcp.qpg);
    if (idr) {
        IMPEncoderAttrRcMode m;
        int restart;

        pthread_mutex_lock(&enc_lock);
        restart = a->rc_restart;
        if (rc_again && a->rc.on)
            restart = 1;
        a->rc_restart = 0;
        a->rb_hold = 0;                 /* the setters are live from this IDR on */
        m = e->attr.rcAttr.attrRcMode;
        pthread_mutex_unlock(&enc_lock);
        if (rate_take(e) && a->rc.on)
            restart = 1;                /* i264e_ratecontrol_init with the new rate / GOP */
        if (restart)
            rc_setup(e, &m, 1);
        /* FIXQP: a per-picture QP bound sets the fixed QPs (i264e_idr_reconfig) */
        if (t32_rc_fixqp_take(&a->rc, &a->qp_i, &a->qp_p))
            qp = a->qp_i;
    }
    /* ROI regions (#726, T32 #734) into the parameter block's ROI table:
     * H.265 before the controller's start (I265e::reconfig, then rcStart),
     * H.264 after it (i264e_encode: ratecontrol_start, then i264e_reconfig,
     * the same in vendor 2.2.0 0x7ed74), so the next start takes them */
    uint8_t roi[16][9];
    int roi_new = OpenIMP_T32_E1RoiTake(chn_no(e), roi);

    if (roi_new && e->hevc)
        t32_rc_roi(&a->rc, roi);
#ifndef PLATFORM_T33
    /* #977 SetChnMapRoi / SetChnQpgAI, H.265: I265eReconfig_ReconfigPFrame (flags 4 / 0x400000) before rcStart */
    if (e->hevc) {
        const uint8_t *mp;
        uint32_t ms;
        T32E1QpgAI qa;

        if (OpenIMP_T32_E1MapTake(chn_no(e), 1, &mp, &ms))
            t32_rc_map_roi(&a->rc, mp, ms);
        if (OpenIMP_T32_E1QpgAITake(chn_no(e), 1, &qa) && qa.map)
            t32_rc_qpgai(&a->rc, OpenIMP_T32_EncQpgAIOn(chn_no(e)), qa.map, qa.size, qa.width, qa.height,
                         qa.fore, qa.mark);
    }
#endif
    if (a->rc.on && (q = t32_rc_start(&a->rc, idr, a->gop_pos)) >= 0)
        qp = q;
    if (roi_new && !e->hevc)
        t32_rc_roi(&a->rc, roi);
#ifndef PLATFORM_T33
    /* #977 SetChnMapRoi: the same i264e_reconfig flag 4 after the start, for the next picture */
    if (!e->hevc) {
        const uint8_t *mp;
        uint32_t ms;

        T32E1QpgAI qa;

        if (OpenIMP_T32_E1MapTake(chn_no(e), 1, &mp, &ms))
            t32_rc_map_roi(&a->rc, mp, ms);
        if (OpenIMP_T32_E1QpgAITake(chn_no(e), 1, &qa) && qa.map)     /* flag 0x200000 */
            t32_rc_qpgai(&a->rc, OpenIMP_T32_EncQpgAIOn(chn_no(e)), qa.map, qa.size, qa.width, qa.height,
                         qa.fore, qa.mark);
    }
#endif
    return qp;
}

/* a picture the engine did not finish: accounted as the full stream window
 * (the section buffer, #784) without statistics (as the T23 Helix path does) */
static void rc_picture_lost(struct t32_enc_chn *e)
{
    if (e->avc.rc.on)
        t32_rc_end(&e->avc.rc, e->avc.bs_bytes, NULL, 0);
}

#ifdef PLATFORM_T33
/*
 * HERA v1.6 (#391): the compressed reconstruction area (data per plane, then
 * two header sets written and read in turn) instead of the T32 ring; the
 * I picture names the area, the P picture after it the reference, later P
 * pictures neither (the engine keeps them by the area's base), as vendor
 * libimp 2.0.2.1 does.  Pairs written, 0 on bad input.
 */
static void t33_recon_addrs(struct t32_h264 *a, const T32HeraH264Pic *ring, int idr,
                            uint32_t *recon_y, uint32_t *recon_c, uint32_t *ref_y, uint32_t *ref_c);

/*
 * SetRdBufShare(0) (#736): vendor 2.0.2.1 (QEMU, 1280x720) gives up the
 * shared compressed area and writes every picture into one of two whole
 * pictures (luma w x h, chroma after it), in turn, the reference being the
 * other one; the header sets follow the two pictures.  The pictures of this
 * one and of its reference; returns where the header sets start.
 */
static uint32_t t33_noshare_bytes(uint32_t w, uint32_t h)
{
    return ALIGN16(w) * ALIGN16(h) * 3u / 2u;
}

static uint32_t t33_noshare_pics(const struct t32_h264 *a, uint32_t w, uint32_t h, uint32_t *recon_y,
                                 uint32_t *recon_c, uint32_t *ref_y, uint32_t *ref_c)
{
    uint32_t base = t32_vpu_buf_pa(&a->ring), pic = t33_noshare_bytes(w, h), ysz = ALIGN16(w) * ALIGN16(h);
    uint32_t cur = base + (a->pp & 1u) * pic, ref = base + ((a->pp & 1u) ^ 1u) * pic;

    *recon_y = cur;
    *recon_c = cur + ysz;
    *ref_y = ref;
    *ref_c = ref + ysz;
    return base + 2u * pic;
}

static uint32_t t33_h264_list(struct t32_enc_chn *e, const T32HeraH264Pic *p, uint32_t w, uint32_t h, int idr)
{
    struct t32_h264 *a = &e->avc;
    T33HeraH264Pic t;
    T33HeraRing r;
    uint32_t base = t32_vpu_buf_pa(&a->ring), hdr, set;
    size_t n;

    t33_hera_ring_layout(w, h, &r);
    memset(&t, 0, sizeof(t));
    t.pic = *p;
    if (e->yuv) {                       /* stand-alone (#752): Main, the source's real height */
        t.no8x8 = 1;
        t.src_height = e->attr.encAttr.picHeight;
    }
    if (a->noshare) {
        /* SetRdBufShare(0) (#736): no shared area, two whole pictures */
        t.noshare = 1;
        t.pic.ring_y = t.pic.ring_y_end = t.pic.ring_c = t.pic.ring_c_end = 0;
        hdr = t33_noshare_pics(a, w, h, &t.recon_y, &t.recon_c, &t.ref_y, &t.ref_c);
    } else {
        t.pic.ring_y = base;
        t.pic.ring_y_end = t.pic.ring_c = base + r.y;
        t.pic.ring_c_end = t.pic.ring_c + r.c;
        hdr = t.pic.ring_c_end;
    }
    set = a->hdr_set & 1u;
    t.hdr_wy = hdr + set * (r.hy + r.hc);
    t.hdr_wc = t.hdr_wy + r.hy;
    t.hdr_ry = hdr + (set ^ 1u) * (r.hy + r.hc);
    t.hdr_rc = t.hdr_ry + r.hy;
    if (!a->noshare)
        t33_recon_addrs(a, &t.pic, idr, &t.recon_y, &t.recon_c, &t.ref_y, &t.ref_c);
    n = t33_hera_h264_list(&t, t32_vpu_buf_va(&a->list));
    if (n) {
        a->hdr_set = set ^ 1u;
        a->recon_y = idr && !a->noshare ? t.pic.ring_y : t.recon_y;
        a->recon_c = idr && !a->noshare ? t.pic.ring_c : t.recon_c;
        a->pp ^= 1u;
    }
    return (uint32_t)n;
}

#endif
#ifdef PLATFORM_T33
/* HERA v1.6 ODMA write position after a picture (vendor libimp 2.0.2.1 reads
 * both after every picture, openimp #393) */
#define T33_HERA_ODMA_POS_Y 0x13210168u
#define T33_HERA_ODMA_POS_C 0x1321016cu

/*
 * The reconstruction of this picture and its reference, as vendor libimp
 * 2.0.2.1 on the T33L silicon (#393): an I picture writes from the areas'
 * bases; a P picture from where the engine stopped (the ODMA read-back after
 * the last picture) and refers to the last picture's reconstruction.  A
 * read-back outside the area (QEMU reads 0; an error) restarts at the base:
 * a P list with 0 there made the engine write over physical address 0.
 */
static void t33_recon_addrs(struct t32_h264 *a, const T32HeraH264Pic *ring, int idr,
                            uint32_t *recon_y, uint32_t *recon_c, uint32_t *ref_y, uint32_t *ref_c)
{
    uint32_t y = a->odma_y, c = a->odma_c;

    if (idr || y < ring->ring_y || y >= ring->ring_y_end || c < ring->ring_c || c >= ring->ring_c_end ||
        (y & 0xfu) || (c & 0xfu)) {
        static unsigned warned;

        if (!idr && warned < 3) {
            warned++;
            IMP_LOG_WARN(TAG, "HERA ODMA read-back %08x / %08x outside the area: from the base", y, c);
        }
        y = ring->ring_y;
        c = ring->ring_c;
    }
    *recon_y = y;
    *recon_c = c;
    *ref_y = a->recon_y ? a->recon_y : ring->ring_y;
    *ref_c = a->recon_c ? a->recon_c : ring->ring_c;
}
#endif
/* P-skip (#784): the encode loop's per-picture fields (0xde428 H.264,
 * 0xdf4cc H.265); the threshold scales the channel's bit rate (the vendor
 * chn+0x50 = rate attribute +8: outBitRate / maxBitRate in rcMode 1..5) */
static void pskip_take(struct t32_enc_chn *e, T32Rc *rc)
{
    const IMPEncoderAttrRcMode *m = &e->attr.rcAttr.attrRcMode;
    T32E1Pskip ps;
    uint32_t br;
    int mode;

    pthread_mutex_lock(&enc_lock);
    mode = (int)m->rcMode;
    br = m->attrH264Cbr.outBitRate;     /* +8 in every rate mode (asserted in imp_encoder.h) */
    pthread_mutex_unlock(&enc_lock);
    OpenIMP_T32_E1PskipTake(chn_no(e), mode, br, &ps);
    t32_rc_pskip(rc, ps.thr, ps.en, ps.req, ps.max_frames);
}

#ifndef PLATFORM_T33
/* SetRdBufShare(0), vendor libimp 2.2.0 (QEMU lists, 1920x1080 H.264 and H.265, #99 row 37): no shared
 * ring; every picture is written into one of two whole pictures (luma ALIGN16 w x ALIGN16 h, chroma
 * after it) in turn, the other one is the reference, the ring registers are 0. */
static void t32_noshare_pics(const struct t32_h264 *a, uint32_t w, uint32_t h, uint32_t *ring_y, uint32_t *ring_y_end,
                             uint32_t *ring_c, uint32_t *ring_c_end, uint32_t *recon_y, uint32_t *recon_c,
                             uint32_t *ref_y, uint32_t *ref_c)
{
    uint32_t base = t32_vpu_buf_pa(&a->ring), ysz = ALIGN16(w) * ALIGN16(h), pic = ysz * 3u / 2u;
    uint32_t cur = base + (a->pp & 1u) * pic, ref = base + ((a->pp & 1u) ^ 1u) * pic;

    *ring_y = *ring_y_end = *ring_c = *ring_c_end = 0;
    *recon_y = cur;
    *recon_c = cur + ysz;
    *ref_y = ref;
    *ref_c = ref + ysz;
}
#endif

static int h264_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s)
{
    struct t32_h264 *a = &e->avc;
    uint32_t w = ALIGN16(f->width), h = ALIGN16(f->height);
    uint32_t rs = t32_hera_ring_stride(w), rows = t32_hera_ring_rows(f->height);
    uint32_t aux[4], at = 0, cap = s->bs.size, n, i;
    uint8_t *out = t32_vpu_buf_va(&s->bs);
    int glue_idr = glue_picture(e);
    int idr = glue_idr || a->idr_req || !a->have_ref, esc = 0;
    T32HeraH264Pic p;
    T32H264Slice sl;
    T32E1Gdr g;

    /* intra refresh (#784): the encode loop's per-picture fields, then the
     * picture type as i264e_decide_slice_type_and_rd decides it */
    OpenIMP_T32_E1AutoVui(chn_no(e), 0);
    OpenIMP_T32_E1GdrTake(chn_no(e), &g);
    if (!idr)
        idr = t32_gdr_slice_type(&a->gop_pos, &a->gdr_d, a->gop, g.req, g.en);
    if (idr) {
        a->gop_pos = 0;
        a->gdr_d = -1;
    }
    t32_rc_gdr(&a->rc, g.gate, g.req, g.en, g.req_frames, g.frames, g.cycle);
    pskip_take(e, &a->rc);
    if (idr)
        e1_at_idr(e);   /* deferred settings: this IDR's headers and list (chroma QP offset, #784) */
    memset(&p, 0, sizeof(p));
    p.no8x8 = e->yuv;                   /* stand-alone: Main (#752) */
    p.width = w;
    p.height = h;
    p.chroma_qp_offset = a->seq.chroma_qp_offset;
    p.p_frame = !idr;
    p.first = a->pictures == 0;         /* #295: only the channel's first picture has the QP..QP window */
    p.qp = rc_picture_qp(e, idr);
    enc_source(e, f, &p.src_y, &p.src_c, &p.src_stride);
    enc_grey_source(e, p.src_stride, h, &p.src_c);
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
#ifndef PLATFORM_T33
    if (a->noshare) {
        uint32_t ry, rc2;

        p.noshare = 1;
        t32_noshare_pics(a, w, h, &p.ring_y, &p.ring_y_end, &p.ring_c, &p.ring_c_end, &p.recon_y, &p.recon_c, &ry, &rc2);
        p.ref_y = idr ? 0 : ry;
        p.ref_c = idr ? 0 : rc2;
    }
#endif
    p.bs0 = t32_vpu_buf_pa(&a->ebs);
    p.bs1 = p.bs0 + a->section_kb * 1024u;
    p.section_kb = a->section_kb;
    for (i = 0; i < 4; i++)
        aux[i] = t32_vpu_buf_pa(&a->aux[i]);
    p.cps = aux[0];
    p.ai = aux[1];
    p.qpt = aux[2];
    p.ipc = aux[3];
    /* rate control (T33 controller): the slice block goes into the list,
     * its per-MB maps (SMART, ROI, MB flags) into the buffers the list names,
     * as the vendor engine reads them from the controller's memory (T33:
     * revision 7, #706) */
    p.rc_slice = t32_rc_slice(&a->rc);
    {
        static const int map_aux[3][2] = { { T33_MAP_SMART, 2 }, { T33_MAP_ROI, 1 }, { T33_MAP_MB, 3 } };

        for (i = 0; i < 3; i++) {
            uint32_t len;
            const uint8_t *m = t32_rc_map(&a->rc, map_aux[i][0], &len);
            T32VpuBuf *b = &a->aux[map_aux[i][1]];

            if (m && len) {
                len = len < b->size ? len : b->size;
                memcpy(t32_vpu_buf_va(b), m, len);
                OpenIMP_T32_VpuFlush(t32_vpu_buf_va(b), len, OI_T32_VPU_FLUSH_WBACK);
            }
        }
    }
#ifdef PLATFORM_T33
    n = t33_h264_list(e, &p, w, h, idr);
#else
    n = (uint32_t)t32_hera_h264_list(&p, t32_vpu_buf_va(&a->list));
#endif
    if (!n) {
        IMP_LOG_ERR(TAG, "HERA list: %ux%u qp %d refused", w, h, p.qp);
        rc_picture_lost(e);
        a->idr_req |= idr;              /* a lost IDR comes again with the next picture */
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
    {
        size_t k;                                   /* InsertUserData (#480): every queued entry */

        while (s->npacks + 1 < T32_H264_PACKS && (k = e1_sei(e, out + at, cap - at)) != 0) {
            h264_pack(s, at, (uint32_t)k, IMP_H264_NAL_SEI, f->timeStamp);
            at += (uint32_t)k;
        }
    }
    memset(&sl, 0, sizeof(sl));
    sl.idr = idr;
    sl.frame_num = idr ? 0u : a->gop_pos % (1u << T32_H264_LOG2_MAX_FRAME_NUM);
    sl.idr_pic_id = a->idr_pic_id;
    sl.qp = p.qp;
    i = at;
    at += (uint32_t)t32_h264_slice_head(&a->seq, &sl, out + at, &esc);

    if (hera_run_shared(e, n, out, cap, &at, &esc, hera_stat_regs, T32_HERA_STATS) != 0) {
        rc_picture_lost(e);
        a->idr_req |= idr;
        return -1;
    }
    /* FRAME_END: the slice's size and the statistics the engine left; after
     * an IDR also the per-row sizes, read the vendor way (select, then read) */
    {
        uint32_t rows[T33_RC_ROWS];
        unsigned nrows = t32_rc_idr_rows(&a->rc), r;

        for (r = 0; r < nrows; r++) {
            uint32_t sel, sv, reg;

            rows[r] = 0;
            t32_rc_row_regs(&a->rc, r, &sel, &sv, &reg);
            if (OpenIMP_T32_VpuWriteReg(sel, sv) != 0 || OpenIMP_T32_VpuReadReg(reg, &rows[r]) != 0)
                rows[r] = 0;
        }
        t32_rc_end_rows(&a->rc, at - i, a->stats, T32_HERA_STATS, rows, nrows);
    }
    h264_pack(s, i, at - i, idr ? IMP_H264_NAL_SLICE_IDR : IMP_H264_NAL_SLICE, f->timeStamp);
    s->pack[s->npacks - 1].frameEnd = 1;
    s->length = at;
    s->ref = idr ? IMP_Encoder_FSTYPE_IDR : IMP_Encoder_FSTYPE_SBASE;   /* vendor 2.2.0: P = SBASE */
    a->ref_y = p.recon_y;
    a->ref_c = p.recon_c;
#ifndef PLATFORM_T33
    a->pp ^= 1u;                        /* no share (SetRdBufShare(0)): the other whole picture next */
#endif
    a->have_ref = 1;
    a->pictures++;
    if (idr) {
        a->idr_req = 0;
        a->idr_pic_id = (a->idr_pic_id + 1u) & 0xffffu;
    }
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
    uint32_t k = 0, sec = a->section_kb * 1024u, n;
    int full = 0;
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
        if (!sec) {                     /* no section stop is set (#784): not expected */
            full = 1;
            break;
        }
        /* vendor i264e (#742): the length at a BSFULL stop already counts
         * the first bytes of the next half (T33L: 256033 at the first
         * 256000 byte stop), so each stop takes exactly one full half;
         * a stop before the half is full takes nothing */
        if (r.output_len >= (k + 1u) * sec) {
            if (h264_take(e, out, cap, at, k * sec, (k + 1u) * sec, (int)(k & 1u), esc) != 0)
                full = 1;
            k++;
            a->sections++;
        }
        if (OpenIMP_T32_VpuSetBsFull(&e->vpu) != 0)
            return -1;
    }
    for (n = 0; n < nstats; n++)
        OpenIMP_T32_VpuReadReg(stat_regs[n], &a->stats[n]);
#ifdef PLATFORM_T33
    /* the ODMA write position for the next P picture (#393): from the
     * statistics when they hold it (H.264), read otherwise */
    a->odma_y = a->odma_c = 0;
    for (n = 0; n < nstats; n++) {
        if (stat_regs[n] == T33_HERA_ODMA_POS_Y)
            a->odma_y = a->stats[n];
        else if (stat_regs[n] == T33_HERA_ODMA_POS_C)
            a->odma_c = a->stats[n];
    }
    if (!a->odma_y)
        OpenIMP_T32_VpuReadReg(T33_HERA_ODMA_POS_Y, &a->odma_y);
    if (!a->odma_c)
        OpenIMP_T32_VpuReadReg(T33_HERA_ODMA_POS_C, &a->odma_c);
#endif
    if (r.rc != 0 || !r.output_len || full ||
        h264_take(e, out, cap, at, k * sec, r.output_len, (int)(k & 1u), esc) != 0) {
        if (full || r.rc == 0)
            a->overflows++;
        IMP_LOG_ERR(TAG, "HERA picture: result 0x%lx status 0x%x length %u%s", r.rc, r.status,
                    r.output_len, full || r.rc == 0 ? " (stream slot full)" : "");
        return -1;
    }
    return 0;
}

#ifdef PLATFORM_T33
/* HERA v1.6 H.265 (#391): the H.264 path's compressed area, the header sets
 * laid out Y, Y, C, C; pairs written, 0 on bad input */
static uint32_t t33_h265_list(struct t32_enc_chn *e, const T32HeraH265Pic *p, int idr)
{
    struct t32_h264 *a = &e->avc;
    uint32_t base = t32_vpu_buf_pa(&a->ring), set = a->hdr_set & 1u;
    T33HeraH265Pic t;
    T33HeraRing r;
    size_t n;

    t33_hera_ring_layout(p->width, p->height, &r);
    memset(&t, 0, sizeof(t));
    t.pic = *p;
    t.rc_slice = t32_rc_slice(&a->rc);  /* rate control (revision 7, #714) */
    if (a->noshare) {
        /* SetRdBufShare(0) (#736): two whole pictures, as H.264 */
        uint32_t hdr = t33_noshare_pics(a, p->width, p->height, &t.recon_y, &t.recon_c, &t.ref_y, &t.ref_c);

        t.noshare = 1;
        t.pic.ring_y = t.pic.ring_y_end = t.pic.ring_c = t.pic.ring_c_end = 0;
        t33_hera_h265_hdr(p->width, p->height, hdr, set, &t.hdr_wy, &t.hdr_wc);
        t33_hera_h265_hdr(p->width, p->height, hdr, set ^ 1u, &t.hdr_ry, &t.hdr_rc);
        n = t33_hera_h265_list(&t, t32_vpu_buf_va(&a->list));
        if (n) {
            a->hdr_set = set ^ 1u;
            a->recon_y = t.recon_y;
            a->recon_c = t.recon_c;
            a->pp ^= 1u;
        }
        return (uint32_t)n;
    }
    t.pic.ring_y = base;
    t.pic.ring_y_end = t.pic.ring_c = base + r.y;
    t.pic.ring_c_end = t.pic.ring_c + r.c;
    t33_hera_h265_hdr(p->width, p->height, t.pic.ring_c_end, set, &t.hdr_wy, &t.hdr_wc);
    t33_hera_h265_hdr(p->width, p->height, t.pic.ring_c_end, set ^ 1u, &t.hdr_ry, &t.hdr_rc);
    {
        T32HeraH264Pic ring;            /* the area as the H.264 helper takes it */

        memset(&ring, 0, sizeof(ring));
        ring.ring_y = t.pic.ring_y;
        ring.ring_y_end = t.pic.ring_y_end;
        ring.ring_c = t.pic.ring_c;
        ring.ring_c_end = t.pic.ring_c_end;
        t33_recon_addrs(a, &ring, idr, &t.recon_y, &t.recon_c, &t.ref_y, &t.ref_c);
    }
    n = t33_hera_h265_list(&t, t32_vpu_buf_va(&a->list));
    if (n) {
        a->hdr_set = set ^ 1u;
        a->recon_y = idr ? t.pic.ring_y : t.recon_y;
        a->recon_c = idr ? t.pic.ring_c : t.recon_c;
    }
    return (uint32_t)n;
}

#endif
static int h265_encode(struct t32_enc_chn *e, const IMPFrameInfo *f, struct t32_enc_slot *s)
{
    struct t32_h264 *a = &e->avc;
    uint32_t w = ALIGN16(f->width), h = (f->height + 7u) & ~7u;
    uint32_t rs = t32_hera_ring_stride(w), rows = t32_hera_ring_rows(h);
    uint32_t at = 0, cap = s->bs.size, n, i;
    uint8_t *out = t32_vpu_buf_va(&s->bs);
    int glue_idr = glue_picture(e);
    int idr = glue_idr || a->idr_req || !a->have_ref, esc = 0;
    T32HeraH265Pic p;
    T32H265Slice sl;
    T32E1Gdr g;

    /* intra refresh (#784): as H.264 (the vendor H.265 loop 0xdf48c fills the
     * same fields; no periodic IDR while GDR runs) */
    OpenIMP_T32_E1AutoVui(chn_no(e), 1);
    OpenIMP_T32_E1GdrTake(chn_no(e), &g);
    if (!idr)
        idr = t32_gdr_slice_type(&a->gop_pos, &a->gdr_d, a->gop, g.req, g.en);
    if (idr) {
        a->gop_pos = 0;
        a->gdr_d = -1;
    }
    t32_rc_gdr(&a->rc, g.gate, g.req, g.en, g.req_frames, g.frames, g.cycle);
    pskip_take(e, &a->rc);
    if (idr)
        e1_at_idr(e);   /* deferred settings: this IDR's headers and list (chroma QP offset, #784) */
    memset(&p, 0, sizeof(p));
    p.chroma_qp_offset = a->seq5.chroma_qp_offset;
    p.motion_quality = e->yuv ? e->yuv_mq : a->mq;
    p.width = w;
    p.height = h;
    p.p_frame = !idr;
    p.after_p = !idr && a->last_p;
    p.first = a->pictures == 0;
    p.qp = rc_picture_qp(e, idr);
    enc_source(e, f, &p.src_y, &p.src_c, &p.src_stride);
    enc_grey_source(e, p.src_stride, h, &p.src_c);
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
#ifndef PLATFORM_T33
    if (a->noshare) {
        uint32_t ry, rc2;

        p.noshare = 1;
        t32_noshare_pics(a, w, h, &p.ring_y, &p.ring_y_end, &p.ring_c, &p.ring_c_end, &p.recon_y, &p.recon_c, &ry, &rc2);
        p.ref_y = idr ? 0 : ry;
        p.ref_c = idr ? 0 : rc2;
    }
#endif
    p.bs0 = t32_vpu_buf_pa(&a->ebs);
    p.bs1 = p.bs0 + a->section_kb * 1024u;
    p.section_kb = a->section_kb;
    for (i = 0; i < T32_HERA5_AUX; i++)
        p.aux[i] = t32_vpu_buf_pa(&a->aux[i]);
#ifdef PLATFORM_T33
    n = t33_h265_list(e, &p, idr);
#else
    p.rc_slice = t32_rc_slice(&a->rc);  /* rate control (revision 6, #720) */
    n = (uint32_t)t32_hera_h265_list(&p, t32_vpu_buf_va(&a->list));
#endif
    if (!n) {
        IMP_LOG_ERR(TAG, "HERA H.265 list: %ux%u qp %d refused", w, h, p.qp);
        rc_picture_lost(e);
        a->idr_req |= idr;
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
    {
        size_t k;                                   /* InsertUserData (#480): every queued entry */

        while (s->npacks + 1 < T32_H264_PACKS && (k = e1_sei(e, out + at, cap - at)) != 0) {
            h265_pack(s, at, (uint32_t)k, IMP_H265_NAL_PREFIX_SEI, f->timeStamp);
            at += (uint32_t)k;
        }
    }
    memset(&sl, 0, sizeof(sl));
    sl.idr = idr;
    sl.poc = idr ? 0u : a->gop_pos;
    sl.qp = p.qp;
    i = at;
    at += (uint32_t)t32_h265_slice_head(&a->seq5, &sl, out + at, &esc);
    if (hera_run_shared(e, n, out, cap, &at, &esc, hera5_stat_regs, T32_HERA5_STATS) != 0) {
        rc_picture_lost(e);
        a->idr_req |= idr;
        return -1;
    }
    /* rcEnd: the slice's size, the 38 statistics the engine left and after
     * an I picture the per-row sizes (read as for H.264) */
    {
        uint32_t rows[T33_RC_ROWS];
        unsigned nrows = t32_rc_idr_rows(&a->rc), r;

        for (r = 0; r < nrows; r++) {
            rows[r] = 0;
#ifdef PLATFORM_T33
            {                           /* revision 7: the T33 row window (#714) */
                uint32_t sel, sv, reg;

                t32_rc_row_regs(&a->rc, r, &sel, &sv, &reg);
                if (OpenIMP_T32_VpuWriteReg(sel, sv) != 0 || OpenIMP_T32_VpuReadReg(reg, &rows[r]) != 0)
                    rows[r] = 0;
            }
#else
            if (OpenIMP_T32_VpuWriteReg(T33_HERA_ROW_SEL, T33_HERA_ROW(r)) != 0 ||
                OpenIMP_T32_VpuReadReg(T33_HERA_ROW(r), &rows[r]) != 0)
                rows[r] = 0;
#endif
        }
        t32_rc_end_rows(&a->rc, at - i, a->stats, T32_HERA5_STATS, rows, nrows);
    }
    h265_pack(s, i, at - i, idr ? IMP_H265_NAL_SLICE_IDR_W_RADL : IMP_H265_NAL_SLICE_TRAIL_R, f->timeStamp);
    s->pack[s->npacks - 1].frameEnd = 1;
    s->length = at;
    s->ref = idr ? IMP_Encoder_FSTYPE_IDR : IMP_Encoder_FSTYPE_SBASE;   /* vendor 2.2.0: P = SBASE */
    a->ref_y = p.recon_y;
    a->ref_c = p.recon_c;
#ifndef PLATFORM_T33
    a->pp ^= 1u;                        /* no share (SetRdBufShare(0)): the other whole picture next */
#endif
    a->have_ref = 1;
    a->last_p = !idr;
    a->pictures++;
    if (idr)
        a->idr_req = 0;
    return 0;
}

/* ---- channel thread ------------------------------------------------------- */
static int slot_free_locked(struct t32_enc_chn *e)
{
    return t32_encq_free(&e->q);
}

#ifdef PLATFORM_T33
/* The vendor encoder channel takes a picture of another size than the channel (FrameSource channel larger than
 * picWidth x picHeight) through c_resize_c with the attribute crop window; ResizeMode (SetChnResizeMode) only
 * decides where the vendor puts the result (1: into the capture buffer itself, 0: an rmem buffer), the picture is
 * the same.  The scaled picture is built in the channel's own rmem buffer here.  *out is the picture handed to
 * the codec (time stamp of the capture); NULL when it cannot be made. */
static const IMPFrameInfo *t33_scale_frame(struct t32_enc_chn *e, const IMPFrameInfo *f, IMPFrameInfo *out)
{
    uint32_t pw = e->attr.encAttr.picWidth, ph = e->attr.encAttr.picHeight;
    uint32_t sh16 = ALIGN16(f->height), ph16 = ALIGN16(ph), need = pw * ph16 * 3u / 2u;
    const IMPEncoderCropCfg *c = &e->attr.encAttr.crop;
    int crop = c->enable ? 1 : 0;

    if (!f->virAddr || f->width < 4u || f->height < 4u || pw < 2u || ph < 2u)
        return NULL;
    if (crop && (c->w < 2u || c->h < 2u || c->x + c->w > f->width || c->y + c->h > sh16))
        return NULL;                    /* the vendor reads outside the picture here */
    if (e->scl_pw != pw || e->scl_ph != ph) {
        OpenIMP_T32_VpuBufFree(&e->scl);
        free(e->scl_tab);
        e->scl_tab = NULL;
        e->scl_pw = e->scl_ph = 0;
        if (OpenIMP_T32_VpuBufAlloc(&e->scl, need, "enc-scale"))
            return NULL;
        e->scl_tab = calloc(T33_RESIZE_TAB(pw, ph16), sizeof(uint16_t));
        if (!e->scl_tab) {
            OpenIMP_T32_VpuBufFree(&e->scl);
            return NULL;
        }
        e->scl_pw = pw;
        e->scl_ph = ph;
    }
    /* the capture was written by the engine: drop the CPU's stale lines of it before reading (vendor: IMP_FlushCache) */
    OpenIMP_T32_VpuFlush((const void *)(uintptr_t)f->virAddr, f->width * sh16 * 3u / 2u, OI_T32_VPU_FLUSH_INV);
    openimp_t33_resize_c((const uint8_t *)(uintptr_t)f->virAddr, t32_vpu_buf_va(&e->scl), (int)f->width, (int)sh16,
                         (int)pw, (int)ph16, crop, crop ? (int)(f->width * c->y + c->x) : 0,
                         crop ? (int)((c->w + 1u) & ~1u) : 0, crop ? (int)((c->h + 1u) & ~1u) : 0, e->scl_tab);
    OpenIMP_T32_VpuFlush(t32_vpu_buf_va(&e->scl), need, OI_T32_VPU_FLUSH_WBACK);
    *out = *f;
    out->width = pw;
    out->height = ph;
    out->phyAddr = t32_vpu_buf_pa(&e->scl);
    out->virAddr = (uint32_t)(uintptr_t)t32_vpu_buf_va(&e->scl);
    return out;
}
#endif

static void *enc_thread(void *arg)
{
    struct t32_enc_chn *e = arg;
    openimp_thread_setnamef(e->hevc ? "oimp-h265-%d" : e->h264 ? "oimp-h264-%d" : "oimp-jpeg-%d",
                            chn_no(e));
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
        e->q_done = 0;
        si = slot_free_locked(e);       /* reserved by the sink */
        pthread_mutex_unlock(&enc_lock);

        {
            const IMPFrameInfo *ef = frame;
#ifdef PLATFORM_T33
            IMPFrameInfo sf;

            if (frame && !e->yuv && (frame->width != e->attr.encAttr.picWidth ||
                                     frame->height != e->attr.encAttr.picHeight))
                ef = t33_scale_frame(e, frame, &sf);
            e->scl_cur = ef != frame;
#endif
            if (e->hevc)
                ok = si >= 0 && ef && h265_encode(e, ef, &e->slot[si]) == 0;
            else if (e->h264)
                ok = si >= 0 && ef && h264_encode(e, ef, &e->slot[si]) == 0;
            else
                ok = si >= 0 && ef && jpeg_encode(e, ef, &e->slot[si], &len) == 0;
        }

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
#ifdef PLATFORM_T33
            if (e->h264) {
#else
            {                           /* T32 2.2.0 (QEMU): the JPEG words are there at the first poll, as H.264 */
#endif
                e->q_packs = (uint32_t)s->npacks;
                e->q_bytes = s->length < e->bs_size ? e->bs_size - s->length : 0u;
            }
            t32_encq_ready(&e->q, si);
            e->encoded++;
#ifdef PLATFORM_T33
            if (!e->h264) {
                /* vendor 2.0.2.1 JPEG (#741): queue (0xdc46c __jz_Fifo_Queue)
                 * and notify (0xdc4b0 bal 0xdb1e0) outside the status lock,
                 * then sem_getvalue + lock + words (0xdc504..0xdc520) + unlock:
                 * a reader woken by the notify can Query, get and release
                 * before the words of this picture are written */
                uint32_t packs = (uint32_t)s->npacks, slen = s->length;

                e->q_post = 1;                  /* coded: no longer a left picture */
                pthread_cond_broadcast(&e->done);   /* 0xdb1e0: sem_post / cond_signal */
                pthread_mutex_unlock(&enc_lock);
                if (write(e->pipefd[1], "s", 1) != 1)
                    IMP_LOG_WARN(TAG, "stream notify pipe full");
                /* the vendor poller and JPEG thread run SCHED_RR at max
                 * priority (PollingStream 0xe62b0, #747); the woken reader
                 * runs here before the words: yield for the same order */
                sched_yield();
                pthread_mutex_lock(&enc_lock);
                e->q_post = 0;
                e->q_packs = packs;
                e->q_bytes = slen < e->bs_size ? e->bs_size - slen : 0u;
                e->q_frames = (uint32_t)e->q.rcount;
            } else
#endif
            if (write(e->pipefd[1], "s", 1) != 1)
                IMP_LOG_WARN(TAG, "stream notify pipe full");
        } else {
            e->errors++;
        }
        e->busy = 0;
#ifdef PLATFORM_T33
        if (e->h264)
            e->q_done = 1;              /* JPEG: no +212 store (#741) */
#else
        e->q_done = 1;                  /* T32 2.2.0: work_done 1 after JPEG pictures too (QEMU) */
#endif
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
    int grp = (int)(intptr_t)ctx, i, refs = 0;
    struct t32_enc_grp *g = &enc_grp[grp];
    uint32_t sn = 0, sd = 0;

    /* source rate for the drop, outside the lock (#921): the sensor rate the frame came with, as the vendor
     * (FrameSource passes every sensor frame on; 2.2.0 encoder_get_fps_mask 0xe6728 (Ghidra) takes the frame's
     * rate words +0x30 / +0x34, which the T33 2.0.2.1 frame update fills at 0x1353a8 from the sensor fps less the
     * ISP frame drop), not the FrameSource channel's outFrmRate */
    if (OpenIMP_FrameSource_SrcRate(fs_chn, &sn, &sd) != 0) {
        IMPFSChnAttr fa;                /* not a FrameSource capture channel: its attribute rate */

        if (IMP_FrameSource_GetChnAttr(fs_chn, &fa) == 0 && fa.outFrmRateNum > 0 && fa.outFrmRateDen > 0) {
            sn = (uint32_t)fa.outFrmRateNum;
            sd = (uint32_t)fa.outFrmRateDen;
        }
    }
    if (sn && sd) {
        pthread_mutex_lock(&enc_lock);
        for (i = 0; i < T32_ENC_CHNS; i++) {
            struct t32_enc_chn *e = &enc_chn[i];
            if (e->created && e->registered && e->grp == grp) {
                e->src_fs = fs_chn;
                e->src_num = sn;
                e->src_den = sd;
            }
        }
        pthread_mutex_unlock(&enc_lock);
    }

    pthread_mutex_lock(&enc_lock);
    /* The frame-rate drop counts every source frame, also the ones dropped
     * below while the group still codes the last frame: a low-rate channel
     * (1 fps JPEG next to H.264) otherwise waits for many more source frames
     * than its rate says.  A frame let through stays due until a later
     * frame can be coded (#517). */
    for (i = 0; i < T32_ENC_CHNS; i++) {
        struct t32_enc_chn *e = &enc_chn[i];

        if (!e->created || !e->registered || e->grp != grp || !e->recv)
            continue;
        e->frames++;
        if (!e->rate_due && rate_pass_locked(e))
            e->rate_due = 1;
    }
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
        if (!e->rate_due)
            continue;
#ifdef PLATFORM_T33
        /* vendor 2.0.2.1 input thread (0xedba8): pad frames on and no pad frame in the queue (it cannot be filled,
         * see IMP_Encoder_PadFrame): the capture is not coded (log 0x370) */
        if (e->h264 && OpenIMP_T33_EncPadOn(chn_no(e))) {
            e->skipped++;
            continue;
        }
#endif
        if ((frame->width != e->attr.encAttr.picWidth || frame->height != e->attr.encAttr.picHeight)
#ifdef PLATFORM_T33
            && !(frame->virAddr && !e->ivdc && !e->yuv)     /* scaled in the encoder thread (#99) */
#endif
            ) {
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
        e->rate_due = 0;
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
    /* vendor 2.2.0: H.264 keeps the profile index 2 (High; the channel maps
     * 0/1/2 to profile_idc 66/77/100), H.265 the profile as given */
    attr->encAttr.profile = attr->encAttr.enType == PT_H264 ? 2u : (uint32_t)profile;
    attr->encAttr.picWidth = (uint16_t)width;
    attr->encAttr.picHeight = (uint16_t)height;
    attr->rcAttr.outFrmRate.frmRateNum = (uint32_t)fpsNum;
    attr->rcAttr.outFrmRate.frmRateDen = (uint32_t)fpsDen;
    attr->rcAttr.maxGop = (uint32_t)gopLen;
    rc = &attr->rcAttr.attrRcMode;
    rc->rcMode = rcMode;
    if (attr->encAttr.enType == PT_JPEG && rcMode != IMP_ENC_RC_MODE_FIXQP) {
        IMP_LOG_ERR(TAG, "JPEG takes FIXQP only");
        return -1;
    }
    /* the vendor's full set of mode defaults (JPEG: the QP as given, -1 too) */
    t32_rc_default_attr(rc, attr->encAttr.enType, iInitialQP, (uint32_t)bitrate);
    return 0;
}

/*
 * Vendor 2.0.2.1 CreateChn (#740), T32 2.2.0 CreateChn 0xdbe7c the same (#743: 801 / 601 0xdc22c / 0xdc60c,
 * 0x4100 0xdc658, 1921 / 1089 0xdc6e4, 4-byte round 0xdc26c; the buffer is malloc'd, 0xd6cc0): the channel's stream buffer size (channel table word +0xc, what Query's
 * leftStreamBytes counts down from).  No encAttr.bufSize: w x h x 3 x 0.5 + 8192 + 128 for every type.  An
 * H.264 / H.265 bufSize is raised to (w x h x 3 x 0.5 / f) x 0.5 + 8320, at least 0x4100, f = 2.0 up to
 * 1920x1088 (H.264, RdBufShare on) or 800x600 (H.264 RdBufShare off, H.265), else 2.5 / 3.0.  JPEG: capped at
 * the JPEG limit (SetJpegBsSize, default 512000) only with the multi-section mode off (system_get_cpu_type()
 * 1 and data byte +0x2ff0 0; the byte is 1 until SetJpegMultiSectionMode(0)).  Rounded up to 4 bytes; the
 * double arithmetic is the vendor's (trunc toward zero).
 */
static uint32_t t33_bs_size(const IMPEncoderCHNAttr *a, int share)
{
    uint32_t w = a->encAttr.picWidth, h = a->encAttr.picHeight, size = a->encAttr.bufSize;
    uint32_t cap = enc_jpeg_bs_size ? enc_jpeg_bs_size : T32_JPEG_BS_1080P;
    double area = (double)w * (double)h * 3.0 * 0.5;
    int jpeg = a->encAttr.enType == PT_JPEG;
#ifdef PLATFORM_T33
    int jcap = jpeg && !OpenIMP_T32_EncJpegMultiSection();
#else
    /* T32 2.2.0 CreateChn 0xdc240: the system variable at 0x205118 is 0 here, so the JPEG limit applies whatever the
     * multi-section byte (QEMU, vendor 2.2.0: jms=1, jms=0 and default all give 512000, jbs=200000 gives 200000; #99 row 44) */
    int jcap = jpeg;
#endif

    if (!size) {
        size = (uint32_t)(area + 8192.0 + 128.0);
        if (jcap && size > cap)
            size = cap;
    } else if (jpeg) {
        if (jcap && size > cap)
            size = cap;
    } else {
        double f = a->encAttr.enType == PT_H264 && share ? (w < 1921u && h < 1089u ? 2.0 : 2.5) :
                   (w < 801u && h < 601u ? 2.0 : 3.0);
        int min = (int)(area / f * 0.5 + 8192.0 + 128.0);

        if (min < 0x4100)
            min = 0x4100;
        if (size < (uint32_t)min)
            size = (uint32_t)min;
    }
    return (size + 3u) & ~3u;
}

#ifndef PLATFORM_T33
/* T32 (#743): the JPEG engine's output buffer in rmem (the old stream buffer size: bufSize, SetJpegBsSize or
 * 512000 scaled from 1920x1080, at least 64 KiB) */
static uint32_t t32_jpeg_out_size(const IMPEncoderCHNAttr *a)
{
    uint64_t area = (uint64_t)a->encAttr.picWidth * a->encAttr.picHeight, size;

    if (a->encAttr.bufSize)
        return (a->encAttr.bufSize + 4095u) & ~4095u;
    if (enc_jpeg_bs_size)
        return (enc_jpeg_bs_size + 4095u) & ~4095u;
    size = (T32_JPEG_BS_1080P * area + 1920u * 1080u - 1u) / (1920u * 1080u);
    size = (size + 4095u) & ~(uint64_t)4095u;
    return size < 65536u ? 65536u : (uint32_t)size;
}
#endif

/* the HERA section buffer: the engine's shared one (T33 #391, T32 #743); per channel only with a test section size */
static int ebs_alloc(struct t32_enc_chn *e)
{
    if (e->bs_shared)
        return t33_bs_get(e);
    return OpenIMP_T32_VpuBufAlloc(&e->avc.ebs, e->avc.bs_bytes, "t32_hera_bs");
}

static void h264_free(struct t32_enc_chn *e)
{
    int i;

#ifdef PLATFORM_T33
    OpenIMP_T32_VpuBufFree(&e->scl);
    free(e->scl_tab);
    e->scl_tab = NULL;
    e->scl_pw = e->scl_ph = 0;
#endif
    OpenIMP_T32_VpuBufFree(&e->avc.list);
    OpenIMP_T32_VpuBufFree(&e->avc.ring);
    OpenIMP_T32_VpuBufFree(&e->avc.grey);
    t33_bs_put(e);                      /* the engine's shared buffer: not ours to free */
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
    rc_setup(e, &at->rcAttr.attrRcMode, 0);
    return 0;
}

/* the H.264 / H.265 channel's engine buffers and parameter sets: 0 or -1 */
static int h264_setup(struct t32_enc_chn *e)
{
    static const char *const aux_tag[4] = { "t32_hera_cps", "t32_hera_ai", "t32_hera_qpt", "t32_hera_ipc" };
    struct t32_h264 *a = &e->avc;
    const IMPEncoderCHNAttr *at = &e->attr;
    uint32_t w = ALIGN16(at->encAttr.picWidth), sizes[4];
    uint32_t ring_bytes = t32_hera_ring_stride(w) * t32_hera_ring_rows(at->encAttr.picHeight) * 3u / 2u;
    const char *env = getenv("OPENIMP_T32_HERA_SECTION_KB");
    int i;

    a->section_kb = T32_HERA_SECTION_KB;
    a->bs_bytes = 2u * T32_HERA_SECTION_KB * 1024u;
    e->bs_shared = 1;
#ifdef PLATFORM_T33
    OpenIMP_T32_EncSections(0, 0, &a->section_kb, &a->bs_bytes);
#endif
    if (env && atoi(env) > 0 && atoi(env) <= 4096) {
        a->section_kb = (uint32_t)atoi(env);
        a->bs_bytes = 2u * a->section_kb * 1024u;
        e->bs_shared = 0;
    }
    a->gop = at->rcAttr.maxGop;
    {
        uint32_t n = at->rcAttr.outFrmRate.frmRateNum, d = at->rcAttr.outFrmRate.frmRateDen;

        if (!e->hevc) {
            /* vendor 2.2.0 channel_i264e_encoder_init: the IDR interval is a
             * multiple of the frame rate numerator (0: the frame rate) */
            t32_rc_vendor_rate(&n, &d, &a->gop);
        } else {
            t32_gop33_reduce(&n, &d);
        }
        a->rc_num = n;
        a->rc_den = d;
        /* CreateChn: the glue asks for this GOP again before the first picture */
        t32_gop33_create(&a->glue, n, d, a->gop);
        a->rate_pend = 0;
    }
    h264_set_qp(e, &at->rcAttr.attrRcMode);
#ifdef PLATFORM_T33
    /* HERA v1.6 compressed area (#391); H.265 heights are multiples of 8.
     * SetRdBufShare(0) (#736): two whole pictures and the header sets */
    a->noshare = OpenIMP_T32_EncTakeRdBufShareOff(chn_no(e));
    a->pp = 0;
    ring_bytes = t33_hera_ring_bytes(w, ALIGN16(at->encAttr.picHeight));
    if (a->noshare) {
        T33HeraRing r;

        t33_hera_ring_layout(w, ALIGN16(at->encAttr.picHeight), &r);
        ring_bytes = 2u * t33_noshare_bytes(w, at->encAttr.picHeight) + 2u * (r.hy + r.hc);
    }
#else
    /* SetRdBufShare(0) (#99 row 37, vendor 2.2.0): two whole pictures instead of the shared ring */
    a->noshare = OpenIMP_T32_EncTakeRdBufShareOff(chn_no(e));
    a->pp = 0;
    if (a->noshare)
        ring_bytes = 2u * ALIGN16(w) * ALIGN16(at->encAttr.picHeight) * 3u / 2u;
#endif
    if (OpenIMP_T32_VpuBufAlloc(&a->list, T32_HERA_LIST_BYTES, "t32_hera_list") != 0 ||
        OpenIMP_T32_VpuBufAlloc(&a->ring, ring_bytes, "t32_hera_ring") != 0 ||
        ebs_alloc(e) != 0)
        return -1;
    if (e->hevc)
        return h265_setup(e);
    t32_hera_aux_sizes(w, ALIGN16(at->encAttr.picHeight), sizes);
    for (i = 0; i < 4; i++)
        if (OpenIMP_T32_VpuBufAlloc(&a->aux[i], sizes[i], aux_tag[i]) != 0)
            return -1;
    a->seq.width = at->encAttr.picWidth;
    a->seq.height = at->encAttr.picHeight;
    a->seq.main_profile = e->yuv;       /* stand-alone: Main as vendor YuvInit (#752); the list drops 8x8 */
    a->seq.fps_num = at->rcAttr.outFrmRate.frmRateNum;
    a->seq.fps_den = at->rcAttr.outFrmRate.frmRateDen;
    a->seq.bitrate = at->rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_CBR ?
                     at->rcAttr.attrRcMode.attrH264Cbr.outBitRate * 1000u : 0u;
    /* vendor i264e_pps_init (#502): pic_init_qp 26 unless the rate control is CQP (FIXQP), then
     * the constant QP clipped to 51; slice QPs are coded relative to it */
    a->seq.init_qp = at->rcAttr.attrRcMode.rcMode == IMP_ENC_RC_MODE_FIXQP ?
                     (a->qp_i > 51 ? 51 : a->qp_i) : 26;
    a->sps_len = t32_h264_sps(&a->seq, a->sps);
    a->pps_len = t32_h264_pps(&a->seq, a->pps);
    rc_setup(e, &at->rcAttr.attrRcMode, 0);
    return 0;
}

/*
 * T33 (#391), T32 (#743): the engines write into the 2 x 250 KiB section
 * buffer only and every picture is copied out of it, so the stream slots
 * are process memory, as vendor libimp 2.0.2.1 / 2.2.0 (malloc at T32
 * 0xd6cc0, pointer chn+500, size chn+504) have them (packs with phyAddr 0):
 * rmem holds the engines' buffers alone.
 */
static int slot_buf_alloc(T32VpuBuf *b, uint32_t size)
{
    void *va = calloc(1, size);

    memset(b, 0, sizeof(*b));
    if (!va)
        return -1;
    b->dma.virt_addr = (uint32_t)(uintptr_t)va;
    b->size = size;
    return 0;
}

static void slot_buf_free(T32VpuBuf *b)
{
    free(t32_vpu_buf_va(b));
    memset(b, 0, sizeof(*b));
}

/* IMP_Encoder_SetMaxStreamCnt before CreateChn (vendor: the channel table field survives the CreateChn memset) */
static int t33_streams[T32_ENC_CHNS];

#ifdef PLATFORM_T33
/* Stream buffers behind a requested count: never fewer than the T32 default (the app may hold a stream while the
 * next picture is encoded) and at most the slot table; the count the getter reports is the requested one. */
static int alloc_streams(int cnt)
{
    return cnt < T32_ENC_DEF_STREAMS ? T32_ENC_DEF_STREAMS : cnt > T32_ENC_MAX_STREAMS ? T32_ENC_MAX_STREAMS : cnt;
}
#else
/* T32 2.2.0: exactly the stored count (a slow consumer can hold that many streams, #936) */
static int alloc_streams(int cnt)
{
    return cnt < 1 ? 1 : cnt > T32_ENC_MAX_STREAMS ? T32_ENC_MAX_STREAMS : cnt;
}
#endif

static void chn_free_bufs(struct t32_enc_chn *e)
{
    int i;

    for (i = 0; i < e->nslots; i++)
        slot_buf_free(&e->slot[i].bs);
    e->nslots = 0;
    t32_encq_init(&e->q, 0);
}

static int chn_alloc_bufs(struct t32_enc_chn *e, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        T32VpuBuf *b = &e->slot[i].bs;

        if (slot_buf_alloc(b, e->bs_size) != 0) {
            e->nslots = i;
            chn_free_bufs(e);
            return -1;
        }
        e->nslots = i + 1;
    }
    t32_encq_init(&e->q, e->nslots);
    return 0;
}

int IMP_Encoder_CreateChn(int encChn, IMPEncoderCHNAttr *attr)
{
    struct t32_enc_chn *e;
    T32EncChnInfo ci;
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
    e->q_done = 1;                      /* vendor CreateChn 0xe2ac0: work_done 1 */
    e->attr = *attr;
    e->h264 = attr->encAttr.enType == PT_H264 || attr->encAttr.enType == PT_H265;
    e->hevc = attr->encAttr.enType == PT_H265;
    e->qp = e->h264 ? 0 : jpeg_qp_of((int)attr->rcAttr.attrRcMode.attrJPEGFixQp.qp);
    /* vendor T33 2.0.2.1 and T32 2.2.0 (libimp 98071ecd, CreateChn 0xdbe7c: field +500 saved over the memset at 0xdc0a8/0xdc130,
     * default 1 for JPEG / 2 for H.264, H.265 at 0xdca60-0xdca7c): the count SetMaxStreamCnt stored before the call, else the
     * default; the vendor keeps the value in the channel table over Destroy/Create */
    e->max_streams = t33_streams[encChn] > 0 ? t33_streams[encChn] : e->h264 ? 2 : 1;
    t33_streams[encChn] = e->max_streams;
    e->pipefd[0] = e->pipefd[1] = -1;
    e->src_fs = -1;
    chn_info(encChn, e, &ci);
    pthread_mutex_unlock(&enc_lock);
    OpenIMP_T32_E1Reset(encChn, &ci);

    if (OpenIMP_T32_VpuOpen() != 0)
        goto fail_open;
    if (OpenIMP_T32_VpuRequest(&e->vpu, e->hevc ? OI_T32_VPU_CODEC_H265ENC :
                               e->h264 ? OI_T32_VPU_CODEC_H264ENC : OI_T32_VPU_CODEC_JPEGENC) != 0)
        goto fail_req;
    if (e->h264 ? h264_setup(e) != 0 :
        OpenIMP_T32_VpuBufAlloc(&e->job, T32_JPEG_JOB_BYTES, "t32_jpeg_job") != 0)
        goto fail_bufs;
#ifdef PLATFORM_T33
    /* after h264_setup: it took the SetRdBufShare switch (#736) */
    e->bs_size = t33_bs_size(attr, !(e->h264 && e->avc.noshare));
#else
    e->bs_size = t33_bs_size(attr, 1);
#endif
#ifdef PLATFORM_T33
    /* JPEG out through 2 x 250 KiB sections, as the vendor (jpegBs, #391);
     * H.26x keeps h264_setup's (the OPENIMP_T32_HERA_SECTION_KB test knob, #746) */
    if (!e->h264) {
        OpenIMP_T32_EncSections(1, enc_jpeg_bs_size ? enc_jpeg_bs_size : T32_JPEG_BS_1080P,
                                &e->avc.section_kb, &e->avc.bs_bytes);
        if (t33_bs_get(e) != 0)
            goto fail_bufs;
    }
#else
    /* JPEG out through the channel's whole-picture rmem buffer (#743) */
    if (!e->h264 && OpenIMP_T32_VpuBufAlloc(&e->avc.ebs, t32_jpeg_out_size(attr), "t32_jpeg_bs") != 0)
        goto fail_bufs;
#endif
    if (chn_alloc_bufs(e, alloc_streams(e->max_streams)) != 0)
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

/* @n dropped streams leave with their notify bytes (IMP_Encoder_GetFd) */
static void chn_notify_drop_locked(struct t32_enc_chn *e, int n)
{
    char c;

    while (n-- > 0)
        if (read(e->pipefd[0], &c, 1) != 1)
            break;
}

/*
 * Vendor StopRecvPic (T33 2.0.2.1 0xf6520, T32 2.2.0, T31 1.1.6 0x9591c):
 * stop taking frames, wait for the ones in work, then drain every queued
 * stream (PollingStream / GetStream / ReleaseStream).  The next
 * StartRecvPic thus starts with the current picture, not with the streams
 * coded before the stop while GetStream was behind (#691).
 */
static void chn_stop_locked(struct t32_enc_chn *e)
{
    int was = e->recv;

    e->recv = 0;
    while (e->pending || e->busy)
        pthread_cond_wait(&e->done, &enc_lock);
    if (was)
        chn_notify_drop_locked(e, t32_encq_stop(&e->q));
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
        if (e->avc.rb_hold)
            attr->rcAttr.attrRcMode = e->avc.rb;
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

/* vendor 2.0.2.1 codes (#56): channel >= 16 0x80040001, qp > 98 0x80040002, not created
 * 0x80040008, not a JPEG channel 0x80040080 (Get: NULL after the channel checks 0x80040002) */
#define T32_ENC_API_CHNS 16
#define E_ENC_CHN     ((int)0x80040001)
#define E_ENC_PARAM   ((int)0x80040002)
#define E_ENC_UNEXIST ((int)0x80040008)
#define E_ENC_STATE   ((int)0x80040080)

static int jpeg_chn_locked(int encChn, struct t32_enc_chn **out)
{
    struct t32_enc_chn *e = chn_get(encChn);

    if (!e)
        return E_ENC_UNEXIST;
    if (e->h264)
        return E_ENC_STATE;
    *out = e;
    return 0;
}

int IMP_Encoder_SetJpegQp(int encChn, int jpegQp)
{
    struct t32_enc_chn *e = NULL;
    int rc;

    if (encChn >= T32_ENC_API_CHNS)
        return E_ENC_CHN;
    if ((unsigned)jpegQp > T32_JPEG_MAX_QP || encChn < 0)
        return E_ENC_PARAM;
    pthread_mutex_lock(&enc_lock);
    rc = jpeg_chn_locked(encChn, &e);
    if (rc == 0)
        e->qp = jpegQp;
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetJpegQp(int encChn, int *jpegQp)
{
    struct t32_enc_chn *e = NULL;
    int rc;

    if (encChn >= T32_ENC_API_CHNS)
        return E_ENC_CHN;
    if (encChn < 0)
        return E_ENC_PARAM;
    pthread_mutex_lock(&enc_lock);
    rc = jpeg_chn_locked(encChn, &e);
    if (rc == 0) {
        if (jpegQp)
            *jpegQp = jpeg_qp_of(e->qp);
        else
            rc = E_ENC_PARAM;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int OpenIMP_T32_SystemInitDone(void);   /* openimp_t32_isp.c */

/* vendor: only before the encoder module is up (IMP_System_Init), 0 refused */
int IMP_Encoder_SetJpegBsSize(uint32_t jpegBsSize)
{
    if (OpenIMP_T32_SystemInitDone())
        return E_ENC_STATE;
    if (!jpegBsSize)
        return E_ENC_PARAM;
    enc_jpeg_bs_size = jpegBsSize;
    return 0;
}

/* for openimp_t32_enc_ext.c (#56 #57): the channel exists */
int OpenIMP_T32_EncChnCreated(int encChn)
{
    int c;

    pthread_mutex_lock(&enc_lock);
    c = chn_get(encChn) != NULL;
    pthread_mutex_unlock(&enc_lock);
    return c;
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
        if (!e->avc.rb_hold) {          /* live (and shown) from the next IDR on, as i264e_reconfig_rc_set */
            e->avc.rb = e->attr.rcAttr.attrRcMode;
            e->avc.rb_hold = 1;
        }
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

/*
 * Run-time rate-control setters (vendor 2.2.0): the argument checks and
 * error codes of the vendor IMP layer, the channel attribute updated as the
 * vendor does, the change handed to the encoder (i264e_set_param 0x11..0x14),
 * which takes it at the next IDR (t32_rc_set_param).  H.265 channels (T23
 * controller revision) restart their controller at the next IDR instead.
 */
#define T32_ENC_ERR_CHN     ((int)0x80040001)
#define T32_ENC_ERR_PARAM   ((int)0x80040002)
#define T32_ENC_ERR_NOCHN   ((int)0x80040008)
#define T32_ENC_ERR_MODE    ((int)0x80040080)
#define T32_ENC_ERR_TIMEOUT ((int)0x80044000)

static int rc_setter(int encChn, unsigned id, const uint32_t a[4], int fixqp_ok)
{
    struct t32_enc_chn *e;
    IMPEncoderAttrRcMode *m;
    int rc = 0;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    m = e ? &e->attr.rcAttr.attrRcMode : NULL;
    if (!e || !e->created)
        rc = T32_ENC_ERR_NOCHN;
    else if (!e->h264 || (!fixqp_ok && m->rcMode == IMP_ENC_RC_MODE_FIXQP))
        rc = T32_ENC_ERR_MODE;
    else if (id == 0x13 && m->rcMode == IMP_ENC_RC_MODE_FIXQP && (a[0] != a[1] || a[2] != a[3]))
        rc = T32_ENC_ERR_PARAM;
    if (rc) {
        pthread_mutex_unlock(&enc_lock);
        return rc;
    }
    /* the getters keep the attribute as it was until the next IDR */
    if (!e->avc.rb_hold) {
        e->avc.rb = *m;
        e->avc.rb_hold = 1;
    }
    /* the attribute (the QP bytes and the rate sit at the same place in every mode but FIXQP) */
    switch (id) {
    case 0x11:
        if (m->rcMode == IMP_ENC_RC_MODE_CBR)
            m->attrH264Cbr.outBitRate = a[0];
        else if (m->rcMode >= IMP_ENC_RC_MODE_VBR && m->rcMode <= IMP_ENC_RC_MODE_AVBR)
            m->attrH264Vbr.maxBitRate = a[1];
        break;
    case 0x12:
        if (m->rcMode >= IMP_ENC_RC_MODE_CBR && m->rcMode <= IMP_ENC_RC_MODE_AVBR) {
            m->attrH264Cbr.maxQp = (uint8_t)a[0];
            m->attrH264Cbr.minQp = (uint8_t)a[1];
        }
        break;
    case 0x13:
        if (m->rcMode == IMP_ENC_RC_MODE_FIXQP) {
            m->attrH264FixQp.IQp = a[0];
            m->attrH264FixQp.PQp = a[3];
        } else if (m->rcMode <= IMP_ENC_RC_MODE_AVBR) {
            m->attrH264Cbr.maxIQp = (uint8_t)a[0];
            m->attrH264Cbr.minIQp = (uint8_t)a[1];
        }
        break;
    case 0x14:
        switch (m->rcMode) {
        case IMP_ENC_RC_MODE_CBR:
            m->attrH264Cbr.maxIPictureSize = a[0];
            m->attrH264Cbr.maxPPictureSize = a[1];
            break;
        case IMP_ENC_RC_MODE_VBR:
            m->attrH264Vbr.maxIPictureSize = a[0];
            m->attrH264Vbr.maxPPictureSize = a[1];
            break;
        case IMP_ENC_RC_MODE_SMART:
            m->attrH264Smart.maxIPictureSize = a[0];
            m->attrH264Smart.maxPPictureSize = a[1];
            break;
        case IMP_ENC_RC_MODE_CVBR:
            m->attrH264CVbr.maxIPictureSize = a[0];
            m->attrH264CVbr.maxPPictureSize = a[1];
            break;
        case IMP_ENC_RC_MODE_AVBR:
            m->attrH264AVbr.maxIPictureSize = a[0];
            m->attrH264AVbr.maxPPictureSize = a[1];
            break;
        default:
            break;
        }
        break;
    }
#ifdef PLATFORM_T33
    /* H.265 too: to the controller, which takes them at the next IDR
     * (vendor 2.0.2.1 I265eReconfig, #718); without one (FIXQP) the
     * picture loop starts the rate control again */
    memcpy(e->avc.set_args[id - 0x11], a, sizeof(e->avc.set_args[0]));
    e->avc.set_mask |= 1u << (id - 0x11);
#else
    if (e->hevc) {
        e->avc.rc_restart = 1;
    } else {
        memcpy(e->avc.set_args[id - 0x11], a, sizeof(e->avc.set_args[0]));
        e->avc.set_mask |= 1u << (id - 0x11);
    }
#endif
    pthread_mutex_unlock(&enc_lock);
    return 0;
}

int IMP_Encoder_SetChnBitRate(int encChn, int bitrate, int bitrateMax)
{
    uint32_t a[4] = { (uint32_t)bitrate, (uint32_t)bitrateMax, 0, 0 };

    if (encChn > 15)
        return T32_ENC_ERR_CHN;
    if (bitrate < 1 || bitrateMax < 1)
        return T32_ENC_ERR_PARAM;
    return rc_setter(encChn, 0x11, a, 0);
}

int IMP_Encoder_SetChnQpBounds(int encChn, int minQp, int maxQp)
{
    uint32_t a[4] = { (uint32_t)maxQp, (uint32_t)minQp, 0, 0 };

    if (encChn > 15)
        return T32_ENC_ERR_CHN;
    if (minQp < 1 || maxQp < 1 || maxQp < minQp)
        return T32_ENC_ERR_PARAM;
    return rc_setter(encChn, 0x12, a, 0);
}

int IMP_Encoder_SetChnQpBoundsPerFrame(int encChn, int minQpI, int maxQpI, int minQpP, int maxQpP)
{
    uint32_t a[4] = { (uint32_t)maxQpI, (uint32_t)minQpI, (uint32_t)maxQpP, (uint32_t)minQpP };

    if (encChn > 15)
        return T32_ENC_ERR_CHN;
    if (minQpI < 1 || maxQpI < 1 || maxQpI < minQpI || minQpP < 1 || maxQpP < 1 || maxQpP < minQpP)
        return T32_ENC_ERR_PARAM;
    return rc_setter(encChn, 0x13, a, 1);
}

int IMP_Encoder_SetChnMaxPictureSize(int encChn, uint32_t maxPictureSizeI, uint32_t maxPictureSizeP)
{
    uint32_t a[4] = { maxPictureSizeI, maxPictureSizeP, 0, 0 };

    if (encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!maxPictureSizeI || !maxPictureSizeP)
        return T32_ENC_ERR_PARAM;
    return rc_setter(encChn, 0x14, a, 1);
}

int IMP_Encoder_GetChnAttrRcMode(int encChn, IMPEncoderAttrRcMode *rcMode)
{
    struct t32_enc_chn *e;

    if (!rcMode)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *rcMode = e->avc.rb_hold ? e->avc.rb : e->attr.rcAttr.attrRcMode;
        if (!e->h264)
            rcMode->attrJPEGFixQp.qp = (uint32_t)e->qp;
    }
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : -1;
}

/*
 * Frame rate and GOP (vendor 2.2.0): the setters store the request (the rate
 * reduced); before the next picture the IMP glue sends it to the encoder
 * (glue_picture, t32_gop33.h) with an IDR, and the getters read what was sent
 * (i264e_reconfig_fps_get / _gop_get).  JPEG channels keep the attribute.
 */
int IMP_Encoder_SetChnFrmRate(int encChn, const IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!rate || !rate->frmRateNum || !rate->frmRateDen)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->created) {
        if (e->h264) {
            t32_gop33_set_rate(&e->avc.glue, rate->frmRateNum, rate->frmRateDen);
        } else {
            e->attr.rcAttr.outFrmRate = *rate;
            e->rate_acc = 0;
        }
    }
    pthread_mutex_unlock(&enc_lock);
    return e && e->created ? 0 : T32_ENC_ERR_NOCHN;
}

int IMP_Encoder_GetChnFrmRate(int encChn, IMPEncoderFrmRate *rate)
{
    struct t32_enc_chn *e;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!rate)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->created) {
        if (e->h264) {
            rate->frmRateNum = e->avc.glue.num;
            rate->frmRateDen = e->avc.glue.den;
        } else {
            *rate = e->attr.rcAttr.outFrmRate;
        }
    }
    pthread_mutex_unlock(&enc_lock);
    return e && e->created ? 0 : T32_ENC_ERR_NOCHN;
}

int IMP_Encoder_SetGOPSize(int encChn, const IMPEncoderGOPSizeCfg *gop)
{
    struct t32_enc_chn *e;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!gop || gop->gopsize < 1)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->created && e->h264)
        t32_gop33_set_gop(&e->avc.glue, (uint32_t)gop->gopsize);
    pthread_mutex_unlock(&enc_lock);
    return e && e->created ? 0 : T32_ENC_ERR_NOCHN;
}

int IMP_Encoder_GetGOPSize(int encChn, IMPEncoderGOPSizeCfg *gop)
{
    struct t32_enc_chn *e;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!gop)
        return T32_ENC_ERR_PARAM;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e && e->created && e->h264)
        gop->gopsize = (int)e->avc.glue.gop;
    pthread_mutex_unlock(&enc_lock);
    return e && e->created ? 0 : T32_ENC_ERR_NOCHN;
}

/* Vendor T33 2.0.2.1 (libimp 0xe7a14 / 0xe7bc4) and T32 (PRJ007 vendor libimp afe092fb, 0xdb4d8 / 0xdb684, #856): a created
 * channel refuses Set with 0x80040080 and keeps its count; a channel >= 16 is 0x80040001.  A channel that is not created: the
 * T33 stores the count in the channel table (taken by CreateChn) and returns 0; the T32 only logs and returns 0 (no store, all
 * of its stores in that function are stack stores).  Get of a channel that is not created is 0x80040008, else the count. */
int IMP_Encoder_SetMaxStreamCnt(int encChn, int cnt)
{
    int rc = 0;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&enc_lock);
    if (chn_get(encChn))
        rc = T32_ENC_ERR_MODE;
    else
        t33_streams[encChn] = cnt;
    pthread_mutex_unlock(&enc_lock);
    return rc;
}

int IMP_Encoder_GetMaxStreamCnt(int encChn, int *cnt)
{
    struct t32_enc_chn *e;
    int rc = T32_ENC_ERR_NOCHN;

    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    if (!cnt)
        return -1;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e) {
        *cnt = e->max_streams;
        rc = 0;
    }
    pthread_mutex_unlock(&enc_lock);
    return rc;
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
        if (!e->recv)
            e->rate_due = 1;            /* the first frame after a start is coded at once */
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

    if (e->q.rcount)
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
    while (!e->q.rcount && e->created && !e->quit) {
        if (timeout_ms < 0)
            pthread_cond_wait(&e->done, &enc_lock);
        else if (pthread_cond_timedwait(&e->done, &enc_lock, &t) == ETIMEDOUT)
            break;
    }
    return e->q.rcount ? 0 : -1;
}

int IMP_Encoder_PollingStream(int encChn, uint32_t timeoutMsec)
{
    struct t32_enc_chn *e;
    int rc = -1;

    /* vendor T32 2.2.0 (libimp 98071ecd) and T33 2.1.0 (SDK libimp, PollingStream 0xe654c: slti/bltz paths to 0x80040001 at 0xe6754,
     * 0x80040008 at 0xe6748, 0x80044000 at 0xe675c) (#937): PollingStream 0xdf2d0 returns 0x80040001 for a channel >= 16 (0xdf54c), 0x80040008
     * for a channel that is not created (0xdf4d4) and 0x80044000 when the wait ends without a picture (0xdf4e0) */
    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        rc = wait_ready_locked(e, (int64_t)timeoutMsec);
    else
        rc = T32_ENC_ERR_NOCHN;
    if (rc == -1)
        rc = T32_ENC_ERR_TIMEOUT;
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
    si = t32_encq_take(&e->q);
    s = &e->slot[si];
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
        if (stream->pack == e->slot[i].pack) {
            rc = t32_encq_release(&e->q, i);
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

    /* vendor T32 2.2.0 (libimp 98071ecd) and T33 2.1.0 (SDK libimp, FlushStream 0xe7784: slti a0,16 at 0xe7794, codes 0x80040001 at
     * 0xe7a5c, 0x80040008 at 0xe7ac4) (#937): FlushStream 0xe0508 returns 0x80040001 for a channel >= 16 (0xe07e0) and
     * 0x80040008 for a channel that is not created (0xe0848) */
    if (encChn < 0 || encChn > 15)
        return T32_ENC_ERR_CHN;
    pthread_mutex_lock(&enc_lock);
    e = chn_get(encChn);
    if (e)
        chn_notify_drop_locked(e, t32_encq_drop_ready(&e->q));
    pthread_mutex_unlock(&enc_lock);
    return e ? 0 : T32_ENC_ERR_NOCHN;
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
        for (i = 0; i < e->q.rcount; i++)
            stat->leftStreamBytes += e->slot[t32_encq_peek(&e->q, i)].length;
        stat->leftStreamFrames = (uint32_t)e->q.rcount;
        stat->curPacks = e->q.rcount ? e->slot[t32_encq_peek(&e->q, 0)].npacks : 0;
        stat->work_done = e->recv ? 0 : 1;
        /* vendor 2.0.2.1 / 2.2.0 (#738, #741): the words of the last coded picture */
        stat->leftStreamBytes = e->q_bytes;
        stat->curPacks = e->q_packs;
        stat->work_done = (uint32_t)e->q_done;
#ifdef PLATFORM_T33
        if (!e->h264) {
            stat->leftStreamFrames = e->q_frames;
            if (e->q_post)
                stat->leftPics = (uint32_t)e->pending;
        }
#endif
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

/* ---- stand-alone encoder contexts (#752) -------------------------------------------------------
 * The back end of IMP_Encoder_YuvInit / YuvEncode / YuvExit and IMP_Encoder_InputJpege
 * (openimp_t32_enc_e3.c).  Vendor 2.0.2.1 / 2.2.0 runs a second i264e / i265e / ijpege instance on the
 * caller's buffer (YuvInit 0x1040c4: i264e_init on its own parameters, YuvEncode 0x104b4c:
 * i264e_encode + i264e_get_bitstream; InputJpege 0x105430: ijpege_init / encode / deinit per call), no
 * FrameSource channel.  Here a context is a hidden channel: the same set-up (h264_setup, the JPEG job
 * and output buffers), one stream slot, the same h264_encode / h265_encode / jpeg_encode, called in the
 * caller's thread; the coded picture is copied to the caller's buffer.
 */
#include "t32/openimp_t32_yuv.h"

static pthread_mutex_t yuv_lock = PTHREAD_MUTEX_INITIALIZER;   /* one stand-alone picture at a time */

static void yuv_free(struct t32_enc_chn *e, int opened)
{
    chn_free_bufs(e);
    OpenIMP_T32_VpuBufFree(&e->job);
    h264_free(e);
    OpenIMP_T32_VpuRelease(&e->vpu);
    if (opened)
        OpenIMP_T32_VpuClose();
    free(e);
}

void *OpenIMP_T32_EncYuvOpen(const T32YuvCfg *c)
{
    struct t32_enc_chn *e;
    IMPEncoderCHNAttr *a;

    if (!c || c->codec < T32_YUV_JPEG || c->codec > T32_YUV_H265 || !c->width || !c->height ||
        c->width > 8192u || c->height > 8192u)
        return NULL;
    if (c->codec != T32_YUV_JPEG && (!c->rc || c->rc_size != sizeof(IMPEncoderAttrRcMode)))
        return NULL;
    if (c->codec == T32_YUV_H265 && (c->width < 256u || c->width > 4096u || c->height < 64u ||
                                     c->height > 2304u)) {
        IMP_LOG_ERR(TAG, "stand-alone H.265 %ux%u outside 256..4096 x 64..2304", c->width, c->height);
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->created = 1;
    e->yuv = 1;
    e->src_fs = -1;
    e->pipefd[0] = e->pipefd[1] = -1;
    a = &e->attr;
    a->encAttr.enType = c->codec == T32_YUV_H265 ? PT_H265 : c->codec == T32_YUV_H264 ? PT_H264 : PT_JPEG;
    a->encAttr.profile = c->codec == T32_YUV_H264 ? 1u : 0u;   /* vendor YuvInit: i264e profile 0x4d (Main) */
    a->encAttr.picWidth = c->width;
    a->encAttr.picHeight = c->height;
    a->rcAttr.outFrmRate.frmRateNum = c->fps_num ? c->fps_num : 25u;
    a->rcAttr.outFrmRate.frmRateDen = c->fps_den ? c->fps_den : 1u;
    a->rcAttr.maxGop = c->gop;
    if (c->codec != T32_YUV_JPEG)
        memcpy(&a->rcAttr.attrRcMode, c->rc, sizeof(a->rcAttr.attrRcMode));
    if (c->codec == T32_YUV_H264)
        t32_rc_yuv_h264_defaults(&a->rcAttr.attrRcMode);
    else if (c->codec == T32_YUV_JPEG)
        /* JPEG only (#912): attrJPEGFixQp.qp shares the union's first word with attrH265FixQp.IQp; writing
         * it for H.265 too set the IDR QP to jpeg_qp_of(0) = 0 (vendor 2.0.2.1 YuvInit 0x1040c4, H.265 branch:
         * FUN_000e780c maps the caller's rate control unchanged; QEMU 0731 IDR BC_CFG1 0x60004 = 0x13c, QP 30) */
        a->rcAttr.attrRcMode.attrJPEGFixQp.qp = (uint32_t)jpeg_qp_of(c->jpeg_qp);
    e->h264 = c->codec != T32_YUV_JPEG;
    e->hevc = c->codec == T32_YUV_H265;
    e->qp = e->h264 ? 0 : jpeg_qp_of(c->jpeg_qp);
    e->max_streams = 1;

    if (OpenIMP_T32_VpuOpen() != 0) {
        free(e);
        return NULL;
    }
    if (OpenIMP_T32_VpuRequest(&e->vpu, e->hevc ? OI_T32_VPU_CODEC_H265ENC :
                               e->h264 ? OI_T32_VPU_CODEC_H264ENC : OI_T32_VPU_CODEC_JPEGENC) != 0) {
        OpenIMP_T32_VpuClose();
        free(e);
        return NULL;
    }
    if (e->h264 ? h264_setup(e) != 0 :
        OpenIMP_T32_VpuBufAlloc(&e->job, T32_JPEG_JOB_BYTES, "t32_yuv_jpeg_job") != 0)
        goto fail;
#ifdef PLATFORM_T33
    e->bs_size = t33_bs_size(a, !(e->h264 && e->avc.noshare));
    if (!e->h264) {
        OpenIMP_T32_EncSections(1, enc_jpeg_bs_size ? enc_jpeg_bs_size : T32_JPEG_BS_1080P,
                                &e->avc.section_kb, &e->avc.bs_bytes);
        if (t33_bs_get(e) != 0)
            goto fail;
    }
#else
    e->bs_size = t33_bs_size(a, 1);
    if (!e->h264 && OpenIMP_T32_VpuBufAlloc(&e->avc.ebs, t32_jpeg_out_size(a), "t32_yuv_jpeg_bs") != 0)
        goto fail;
#endif
    if (chn_alloc_bufs(e, 1) != 0)
        goto fail;
    IMP_LOG_INFO(TAG, "stand-alone %s %ux%u, stream buffer %u bytes, vpu channel %u",
                 e->hevc ? "H.265" : e->h264 ? "H.264" : "JPEG", c->width, c->height, e->bs_size,
                 e->vpu.node.channel_id);
    return e;
fail:
    yuv_free(e, 1);
    return NULL;
}

/* #784: vendor 2.0.2.1 YuvEncode (0x104b4c) sets the picture's motion-quality flag (+0x1ec) from the
 * SetMotionQuality_Yuv global, and YuvInit (0x1040c4) the encoder's (+0x35c) from SetMotionQualityEnable_Yuv;
 * QEMU 0731: only both together change the H.265 P lists (the batch 2 registers), H.264 nothing */
void OpenIMP_T32_EncYuvSetMq(void *ctx, int on)
{
    struct t32_enc_chn *e = ctx;

    if (e && e->yuv)
        e->yuv_mq = on != 0;
}

int OpenIMP_T32_EncYuvFrame(void *ctx, const void *y_va, uint32_t y_pa, uint32_t w, uint32_t h, int64_t ts,
                            void *out, uint32_t cap, uint32_t *len)
{
    struct t32_enc_chn *e = ctx;
    struct t32_enc_slot *s;
    IMPFrameInfo f;
    uint32_t n = 0;
    int ok;

    if (!e || !e->yuv || !out || !y_pa)
        return T32_YUV_E_CODE;
    memset(&f, 0, sizeof(f));
    f.width = w;
    f.height = h;
    f.pixfmt = PIX_FMT_NV12;
    f.phyAddr = y_pa;
    f.size = ALIGN16(w) * ((h + 1u) & ~1u) * 3u / 2u;
    f.timeStamp = ts;
    /* vendor YuvEncode / InputJpege: IMP_FlushCache(src, h * 3 * ALIGN16(w) / 2, 1) */
    if (y_va)
        OpenIMP_T32_VpuFlush(y_va, f.size, OI_T32_VPU_FLUSH_WBACK);
    pthread_mutex_lock(&yuv_lock);
    s = &e->slot[0];
    if (e->hevc)
        ok = h265_encode(e, &f, s) == 0;
    else if (e->h264)
        ok = h264_encode(e, &f, s) == 0;
    else
        ok = jpeg_encode(e, &f, s, &n) == 0;
    if (ok && e->h264)
        n = s->length;
    if (ok && cap && n > cap) {
        IMP_LOG_ERR(TAG, "stand-alone picture of %u bytes over the %u byte output buffer", n, cap);
        pthread_mutex_unlock(&yuv_lock);
        return T32_YUV_E_ROOM;
    }
    if (ok) {
        memcpy(out, t32_vpu_buf_va(&s->bs), n);
        e->encoded++;
        *len = n;
    } else {
        e->errors++;
    }
    pthread_mutex_unlock(&yuv_lock);
    return ok ? 0 : T32_YUV_E_CODE;
}

void OpenIMP_T32_EncYuvClose(void *ctx)
{
    struct t32_enc_chn *e = ctx;

    if (!e || !e->yuv)
        return;
    IMP_LOG_INFO(TAG, "stand-alone %s off: %u coded, %u over-limit retries, %u errors",
                 e->hevc ? "H.265" : e->h264 ? "H.264" : "JPEG", e->encoded, e->retries, e->errors);
    yuv_free(e, 1);
}
