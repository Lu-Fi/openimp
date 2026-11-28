/* apitest: Encoder area (H.264 / H.265 / JPEG) and the stream verifier */
#include <dlfcn.h>
#include <time.h>
#include "apitest.h"
/* T20/T21: OpenIMP refuses (-1) settings its Helix/NVPU encoder cannot apply, instead of storing
 * them without effect (the vendor stores them): report that as N/A, not as a failed function. */
#if defined(PLATFORM_T20) || defined(PLATFORM_T21)
# define REFUSES_UNSUPPORTED 1
#else
# define REFUSES_UNSUPPORTED 0
#endif
#include <errno.h>
#include <poll.h>

#define EW SUB_W
#define EH SUB_H
#define EFPS 15
#define EGOP 30
#define GRP 0
#define CODEC_H264 0
#define CODEC_H265 1
#define CODEC_JPEG 2
#if defined(PLATFORM_T41) && !defined(HAS_IMP_Encoder_SetChnAttrRcMode)
/* the T41 1.2.0 header lacks it, vendor libimp 1.0.1 and 1.2.6 export it (timps and raptor call it) */
extern int IMP_Encoder_SetChnAttrRcMode(int encChn, const IMPEncoderAttrRcMode *pstRcModeCfg) __attribute__((weak));
# define HAS_IMP_Encoder_SetChnAttrRcMode 1
#endif
#if defined(PLATFORM_T23) && !HAS_IMP_Encoder_PollingModuleStream
/* the T23 1.3.0 header lacks it, the vendor libimp exports it (signature as on T21/T31) */
extern int IMP_Encoder_PollingModuleStream(uint32_t *bitmap, uint32_t timeout_ms) __attribute__((weak));
# undef HAS_IMP_Encoder_PollingModuleStream
# define HAS_IMP_Encoder_PollingModuleStream 1
#endif
#define STREAMCAP (512 * 1024)
#if defined(ENC_OLD) && defined(HAVE_H265)
# define T_H265 PT_H265
#else
# define T_H265 PT_H264        /* no H.265 on this SoC: never used */
#endif

/* The rate-control attributes (outBitRate / uTargetBitRate) are in kbit/s on every SoC (the vendor
 * samples pass 2000 for 720p).  Only IMP_Encoder_SetChnBitRate of the T31/T41 family takes bit/s. */
#define BR_UNIT 1
#ifdef ENC_NEW
# define BR_SET_UNIT 1000      /* IMP_Encoder_SetChnBitRate: bit/s */
# define YUV_BITRATE 512       /* IMPEncoderYuvIn.targetBitrate/maxBitrate: kbit/s (vendor T41 1.2.6 YuvInit multiplies both by 1000) */
#else
# define BR_SET_UNIT 1
# define YUV_BITRATE BITRATE
#endif
#define BITRATE (512 * BR_UNIT)

static const char *cname[] = { "H264", "H265", "JPEG" };

/* T41 vendor libimp: the RC/GOP/frame-rate setters only store a pending value; the encoder applies it
 * before the next picture and the getters show it from then on (audit T41_RCPARAMS_1024: vendor
 * GetChnAttrRcMode right after SetChnQpBoundsPerFrame still returned the old window).  OpenIMP keeps the
 * same deferred read-back (rb_hold), so the read-back is checked after two more pictures. */
#if defined(PLATFORM_T41) && !defined(APITEST_T40)   /* the T40 build uses the T41 API family */
# define RB_SETTLE(chn, codec) pull((chn), (codec), 2, 0, 0, &si, 1500)
/* T41 vendor read-back of the GOP length (#475, vendor 1.2.6 run on the .32): the IDR distance of the glue.
 * First picture: float seconds, not floored; a changed run-time length gets one full second added unless
 * it is whole seconds; then length * same-scene rounded up to a multiple of the rate (ValidateGopParam). */
static unsigned t41_gop_rb(unsigned len, unsigned same, unsigned active)
{
    volatile float s, t;
    unsigned frames = len, g, idr;

    if (len != active) {                       /* SetChnGopLength / SetChnGopAttr: pending != active */
        s = (float)frames; s = s / (float)EFPS;
        if (frames % EFPS) s = s + 1.0f;
        t = s * (float)EFPS; t = t / 1.0f;
    } else {                                   /* first picture */
        s = (float)frames; s = s / (float)EFPS; t = s * (float)EFPS; t = t / 1.0f;
    }
    g = (unsigned)t;
    idr = g * (same > 1 ? same : 1);
    if (idr > 1000 - EFPS) idr = 1000 - EFPS;
    return (idr + EFPS - 1) / EFPS * EFPS;
}
# define GOP_RB(len, same) t41_gop_rb((len), (same), (len))
# define GOP_SET_RB_T41(len, same, active) t41_gop_rb((len), (same), (active))
#elif defined(PLATFORM_T31)
/* T31 vendor 1.1.6 (#866): the encoder thread applies a pending rate / GOP before the picture it encodes next
 * (a frame-rate change rescales the stored GOP length, SetChnFrmRate 10/1 then 15/1 reads 20 until pictures
 * pass), so the GOP read-back is checked after a few pictures; the created mode is DEFAULT (2). */
# define GOP_RB(len, same) (len)
/* run-time length 25 at 15 fps reads back 39 on the vendor (glue: float seconds plus one second unless whole) */
static unsigned t31_gop_set_rb(unsigned len)
{
    volatile float s, t;

    s = (float)len; s = s / (float)EFPS;
    if (len % EFPS) s = s + 1.0f;
    t = s * (float)EFPS; t = t / 1.0f;
    return (unsigned)t & 0xffffu;
}
# undef GOP_RB_SET
# define GOP_RB_SET(len) t31_gop_set_rb(len)
# define GOP_SETTLE(chn, codec) pull((chn), (codec), 6, 0, 0, &si, 2000)
#else
# define GOP_RB(len, same) (len)
#endif
#if defined(PLATFORM_T31) && !defined(APITEST_T40)
# define GOP_MODE_OK(g) ((g)->uGopCtrlMode == IMP_ENC_GOP_CTRL_MODE_DEFAULT)
#else
# define GOP_MODE_OK(g) 1
#endif
#ifndef GOP_RB_SET
# define GOP_RB_SET(len) (len)
#endif
#ifndef GOP_SETTLE
# define GOP_SETTLE(chn, codec) RB_SETTLE(chn, codec)
#endif
/* T10/T20/T21/T30 (#367, #421): the vendor IMP glue latches SetChnFrmRate / SetGOPSize to the next picture,
 * GetChnFrmRate / GetGOPSize answer the running values and a requested GOP is rounded up to a multiple of the
 * reduced frame rate num*den.  The base case reads back after two pictures (latch timing: case gopfps). */
#if !defined(ENC_NEW) && (defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30))
# define RB_SETTLE(chn, codec) pull((chn), (codec), 2, 0, 0, &si, 1500)
# define GOP_SET_RB(g) (((g) + EFPS - 1) / EFPS * EFPS)
#else
# ifndef RB_SETTLE
#  define RB_SETTLE(chn, codec) do { } while (0)
# endif
# define GOP_SET_RB(g) (g)
#endif

/* T32 vendor 2.2.0: GetChnAttrRcMode reads the encoder's live parameters (i264e_reconfig_rc_get);
 * SetChnAttrRcMode / SetChnBitRate change them only at the next IDR (i264e_idr_reconfig), so the
 * read-back is checked after a requested IDR, the value before it is shown (the old one) */
#if defined(APITEST_T32) && HAS_IMP_Encoder_RequestIDR
# define RB_IDR(chn, codec) do { IMP_Encoder_RequestIDR(chn); RB_SETTLE(chn, codec); } while (0)
# define RB_AT_IDR 1
#else
# define RB_IDR(chn, codec) do { } while (0)
# define RB_AT_IDR 0
#endif

/* quiet-after-first reporting: first=1 prints the line, later calls print only failures */
#if defined(APITEST_T32)
#define ENC_POOL_ID 3       /* requested by the encoder tests (#447) */
#else
#define ENC_POOL_ID 0
#endif
#define QR0(f, r, first, ...) do { if ((first) || (r) != 0) RET0(f, r, __VA_ARGS__); } while (0)

/* ----------------------------------------------------------- stream parse */
typedef struct {
    int frames, bytes_max;
    unsigned long bytes;
    int vps, sps, pps, idr, sei, other;
    int w, h, profile, level;           /* from SPS (H.264) or SOF (JPEG) */
    int soi, eoi, jpeg_ok;              /* JPEG frames with SOI..EOI */
    int dqt0;                           /* first luma DQT entry of the last JPEG frame (-1 none) */
    int user;                           /* user data string seen */
    int trunc;
    int first_idr_frame;                /* frame index of the first IDR (-1 none) */
    int last_frame_idr;
    int idr_at[4], idr_n;               /* frame indices of the first 4 IDRs (#804 IDR distance) */
} SI;

static unsigned char *g_sbuf;           /* gathered frame */

static size_t gather(const IMPEncoderStream *s, unsigned char *buf, size_t cap, int *trunc)
{
    size_t n = 0;
    uint32_t i;

    for (i = 0; i < s->packCount && s->pack; i++) {
        const IMPEncoderPack *p = &s->pack[i];
        size_t len = p->length, take;
#ifdef ENC_NEW
        const unsigned char *base = (const unsigned char *)(uintptr_t)s->virAddr;
        size_t rem = s->streamSize > p->offset ? s->streamSize - p->offset : 0;

        if (!len) continue;
        if (rem && rem < len) {
            take = rem; if (n + take > cap) { take = cap - n; *trunc = 1; }
            memcpy(buf + n, base + p->offset, take); n += take;
            take = len - rem; if (n + take > cap) { take = cap - n; *trunc = 1; }
            memcpy(buf + n, base, take); n += take;
        } else {
            take = len; if (n + take > cap) { take = cap - n; *trunc = 1; }
            memcpy(buf + n, base + p->offset, take); n += take;
        }
#else
        if (!len) continue;
        take = len; if (n + take > cap) { take = cap - n; *trunc = 1; }
        memcpy(buf + n, (const void *)(uintptr_t)p->virAddr, take); n += take;
#endif
    }
    return n;
}

/* the frame of a stream as one buffer (cases that dump a stream: padframe_t23) */
size_t at_stream_gather(const IMPEncoderStream *s, unsigned char *buf, size_t cap, int *trunc)
{
    return gather(s, buf, cap, trunc);
}

/* --- tiny H.264 SPS reader (resolution, profile, level) */
typedef struct { const unsigned char *b; size_t n, pos; } BR;
static unsigned br_bit(BR *r) { unsigned v; if (r->pos >= r->n * 8) return 0; v = (r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1; r->pos++; return v; }
static unsigned br_bits(BR *r, int k) { unsigned v = 0; while (k--) v = (v << 1) | br_bit(r); return v; }
static unsigned br_ue(BR *r) { int z = 0; while (!br_bit(r) && z < 32 && r->pos < r->n * 8) z++; return (1u << z) - 1 + br_bits(r, z); }

static void parse_sps(const unsigned char *p, size_t n, SI *si)
{
    unsigned char u[256];
    size_t i, m = 0;
    BR r;
    unsigned profile, chroma = 1, wmb, hmu, fmo, crop = 0, cl = 0, cr = 0, ct = 0, cb = 0, poc;

    for (i = 0; i < n && m < sizeof(u); i++) {            /* drop emulation prevention bytes */
        if (i >= 2 && p[i] == 3 && p[i - 1] == 0 && p[i - 2] == 0) continue;
        u[m++] = p[i];
    }
    r.b = u; r.n = m; r.pos = 0;
    profile = br_bits(&r, 8); br_bits(&r, 8); si->level = (int)br_bits(&r, 8); br_ue(&r);
    si->profile = (int)profile;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128) {
        chroma = br_ue(&r);
        if (chroma == 3) br_bit(&r);
        br_ue(&r); br_ue(&r); br_bit(&r);
        if (br_bit(&r)) {                                 /* seq_scaling_matrix_present: skip the lists */
            unsigned li, lj, lists = chroma != 3 ? 8 : 12;

            for (li = 0; li < lists; li++) {
                if (!br_bit(&r)) continue;
                {
                    int last = 8, next = 8;
                    unsigned size = li < 6 ? 16 : 64;

                    for (lj = 0; lj < size; lj++) {
                        if (next != 0) {
                            unsigned k = br_ue(&r);
                            int delta = (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2);

                            next = (last + delta + 256) % 256;
                        }
                        last = next == 0 ? last : next;
                    }
                }
            }
        }
    }
    br_ue(&r);                                            /* log2_max_frame_num */
    poc = br_ue(&r);
    if (poc == 0) br_ue(&r); else if (poc == 1) return;
    br_ue(&r); br_bit(&r);
    wmb = br_ue(&r); hmu = br_ue(&r); fmo = br_bit(&r);
    if (!fmo) br_bit(&r);
    br_bit(&r);
    crop = br_bit(&r);
    if (crop) { cl = br_ue(&r); cr = br_ue(&r); ct = br_ue(&r); cb = br_ue(&r); }
    si->w = (int)((wmb + 1) * 16 - 2 * (cl + cr));
    si->h = (int)((hmu + 1) * 16 * (2 - fmo) - 2 * (2 - fmo) * (ct + cb));
}

static void scan_frame(const unsigned char *b, size_t n, int codec, SI *si)
{
    size_t i;
    int idr_here = 0;

    if (codec == CODEC_JPEG) {
        int ok = n > 4 && b[0] == 0xFF && b[1] == 0xD8;
        size_t j = 2;

        si->soi += ok;
        while (ok && j + 9 < n && b[j] == 0xFF) {
            unsigned m = b[j + 1], len = (b[j + 2] << 8) | b[j + 3];
            if (m == 0xC0 || m == 0xC2) { si->h = (b[j + 5] << 8) | b[j + 6]; si->w = (b[j + 7] << 8) | b[j + 8]; }
            if (m == 0xDB && len >= 67) si->dqt0 = b[j + 5];
            if (m == 0xDA) break;
            j += 2 + len;
        }
        for (i = n > 16 ? n - 16 : 0; i + 1 < n; i++)
            if (b[i] == 0xFF && b[i + 1] == 0xD9) { si->eoi++; if (ok) si->jpeg_ok++; break; }
        return;
    }
    for (i = 0; i + 3 < n; i++) {
        unsigned nal, t;

        if (b[i] || b[i + 1] || b[i + 2] != 1) continue;
        nal = b[i + 3];
        if (codec == CODEC_H264) {
            t = nal & 31;
            if (t == 7) { si->sps++; if (!si->w) parse_sps(b + i + 4, n - i - 4 > 200 ? 200 : n - i - 4, si); }
            else if (t == 8) si->pps++;
            else if (t == 5) { si->idr++; idr_here = 1; }
            else if (t == 6) si->sei++;
            else si->other++;
        } else {
            t = (nal >> 1) & 63;
            if (t == 32) si->vps++;
            else if (t == 33) si->sps++;
            else if (t == 34) si->pps++;
            else if (t == 19 || t == 20) { si->idr++; idr_here = 1; }
            else if (t == 39 || t == 40) si->sei++;
            else si->other++;
        }
        i += 3;
    }
    si->last_frame_idr = idr_here;
    if (idr_here && si->first_idr_frame < 0) si->first_idr_frame = si->frames;
    if (idr_here && si->idr_n < 4) si->idr_at[si->idr_n++] = si->frames;
    if (n >= 8) {                                        /* user data (InsertUserData) */
        for (i = 0; i + 7 <= n; i++) if (!memcmp(b + i, "apitest", 7)) { si->user++; break; }
    }
}

/* Pull up to maxf frames from the channel (polling + get + release). rp: report
 * the Polling/GetStream/ReleaseStream lines for the first frame. stop: SI field
 * condition checked each frame: 0 = none, 1 = until IDR seen, 2 = sps+pps+idr */
static FILE *g_dump;        /* APITEST_CROP_DUMP=<file>: raw stream of the cropped window (picture check off the camera) */
static int pull(int chn, int codec, int maxf, int stop, int rp, SI *si, int tmo_ms)
{
    int k;

    memset(si, 0, sizeof(*si));
    si->first_idr_frame = -1;
    si->dqt0 = -1;
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < maxf; k++) {
        G(IMPEncoderStream, s);
        int r, trunc = 0;
        size_t n;

        if (IMP_Encoder_PollingStream) {
            r = IMP_Encoder_PollingStream(chn, tmo_ms);
            if (rp && k == 0) CHECK(IMP_Encoder_PollingStream, r, 1, "%s chn %d, timeout %d ms -> %d (0 = frame ready)", cname[codec], chn, tmo_ms, r);
            if (r != 0) { gfree(s); return -1; }
        } else if (rp && k == 0) rep_na(FN(IMP_Encoder_PollingStream));
        r = IMP_Encoder_GetStream(chn, s, true);
        gchk(s);
        if (r != 0) {
            if (rp && k == 0) RET0(IMP_Encoder_GetStream, r, "%s chn %d", cname[codec], chn);
            gfree(s);
            return -1;
        }
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (g_dump && !trunc) fwrite(g_sbuf, 1, n, g_dump);
        si->trunc |= trunc;
        scan_frame(g_sbuf, n, codec, si);
        si->frames++;
        si->bytes += n;
        if ((int)n > si->bytes_max) si->bytes_max = (int)n;
        if (getenv("APITEST_SIZES"))        /* opt-in trace: every picture size, '*' = IDR picture */
            printf("[I] size %s chn %d pic %d: %zu%s\n", cname[codec], chn, k, n, si->last_frame_idr ? " *" : "");
        if (rp && k == 0) {
            rep(FN(IMP_Encoder_GetStream), 0, (s->packCount > 0 && n > 0) ? V_PASS : V_FAIL,
                "%s chn %d: %u packs, %zu bytes%s", cname[codec], chn, s->packCount, n, trunc ? " (truncated by test buffer)" : "");
        }
        r = IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        if (rp && k == 0) RET0(IMP_Encoder_ReleaseStream, r, "%s chn %d", cname[codec], chn);
        gfree(s);
        if (stop == 1 && si->idr) return 0;
        if (stop == 2 && si->idr && si->sps && si->pps && (codec != CODEC_H265 || si->vps)) return 0;
    }
    return 0;
}

/* ----------------------------------------------------- group / bind chain */
static int g_osd;           /* OSD between FS and ENC */

/* the FrameSource channel that feeds the encoder: ch1 (scaled), or ch0 with HSKIP_MAIN=1 (case hskip: a library whose scaled channel the
 * driver does not serve, e.g. the T21 vendor libimp with the open tx-isp, codes the sensor-size channel) */
static int enc_fs_ch(void) { return getenv("HSKIP_MAIN") ? 0 : CH_SUB; }

static int grp_up(int with_osd, int first)
{
    IMPCell fs = { DEV_ID_FS, enc_fs_ch(), 0 }, enc = { DEV_ID_ENC, GRP, 0 }, osd = { DEV_ID_OSD, GRP, 0 };
    int r, ok = 0;

    r = IMP_Encoder_CreateGroup(GRP);
    QR0(IMP_Encoder_CreateGroup, r, first, "group %d", GRP);
    if (r) return -1;
    if (with_osd) {
        NEED(IMP_OSD_CreateGroup) {
            r = IMP_OSD_CreateGroup(GRP);
            QR0(IMP_OSD_CreateGroup, r, first, "group %d", GRP);
        }
        r = IMP_System_Bind(&fs, &osd);
        QR0(IMP_System_Bind, r, first, "FS ch%d -> OSD group %d", enc_fs_ch(), GRP);
        if (r) return -1;
        r = IMP_System_Bind(&osd, &enc);
        QR0(IMP_System_Bind, r, 0, "OSD -> ENC group %d", GRP);
        if (r) return -1;
        g_osd = 1;
    } else {
        r = IMP_System_Bind(&fs, &enc);
        QR0(IMP_System_Bind, r, first, "FS ch%d -> ENC group %d", enc_fs_ch(), GRP);
        if (r) return -1;
    }
    NEED(IMP_System_GetBindbyDest) {
        G(IMPCell, src);
        IMPCell *want = with_osd ? &osd : &fs;

        /* the source recorded for the encoder group must be the cell bound to it */
        r = IMP_System_GetBindbyDest(&enc, src);
        gchk(src);
        ok = src->deviceID == want->deviceID && src->groupID == want->groupID;
        if (first) CHECK(IMP_System_GetBindbyDest, r, ok, "ENC group %d <- device %d group %d output %d (expected device %d group %d)",
                         GRP, (int)src->deviceID, src->groupID, src->outputID, (int)want->deviceID, want->groupID);
        else if (r || !ok) CHECK(IMP_System_GetBindbyDest, r, ok, "device %d group %d", (int)src->deviceID, src->groupID);
        gfree(src);
    }
    return 0;
}

static void grp_down(int first)
{
    IMPCell fs = { DEV_ID_FS, enc_fs_ch(), 0 }, enc = { DEV_ID_ENC, GRP, 0 }, osd = { DEV_ID_OSD, GRP, 0 };
    int r;

    if (g_osd) {
        r = IMP_System_UnBind(&osd, &enc); QR0(IMP_System_UnBind, r, 0, "OSD -> ENC");
        r = IMP_System_UnBind(&fs, &osd); QR0(IMP_System_UnBind, r, first, "FS -> OSD");
        if (IMP_OSD_DestroyGroup) { r = IMP_OSD_DestroyGroup(GRP); QR0(IMP_OSD_DestroyGroup, r, first, "group %d", GRP); }
        g_osd = 0;
    } else {
        r = IMP_System_UnBind(&fs, &enc); QR0(IMP_System_UnBind, r, first, "FS ch%d -> ENC group %d", enc_fs_ch(), GRP);
    }
    r = IMP_Encoder_DestroyGroup(GRP);
    QR0(IMP_Encoder_DestroyGroup, r, first, "group %d", GRP);
}

/* ------------------------------------------------------------ channel set up */

#ifdef ENC_NEW
typedef IMPEncoderChnAttr EAttr;
#else
typedef IMPEncoderCHNAttr EAttr;
#endif

static void fill_attr(EAttr *a, int codec, int gop)
{
    memset(a, 0, sizeof(*a));
#ifdef ENC_NEW
    {
        IMPEncoderProfile prof = codec == CODEC_H264 ? IMP_ENC_PROFILE_AVC_HIGH :
                                 codec == CODEC_H265 ? IMP_ENC_PROFILE_HEVC_MAIN : IMP_ENC_PROFILE_JPEG;
        IMPEncoderRcMode rc = codec == CODEC_JPEG ? IMP_ENC_RC_MODE_FIXQP : IMP_ENC_RC_MODE_CBR;
        int r;

        if (IMP_Encoder_SetDefaultParam) {
            r = IMP_Encoder_SetDefaultParam(a, prof, rc, EW, EH, EFPS, 1, codec == CODEC_JPEG ? 0 : gop, 2,
                                            codec == CODEC_JPEG ? 75 : -1, codec == CODEC_JPEG ? 0 : BITRATE);
            CHECK(IMP_Encoder_SetDefaultParam, r, a->encAttr.uWidth == EW && a->encAttr.uHeight == EH && a->rcAttr.attrRcMode.rcMode == rc &&
                  a->rcAttr.outFrmRate.frmRateNum == EFPS,
                  "%s %dx%d rc %d fps %u/%u gop %u", cname[codec], a->encAttr.uWidth, a->encAttr.uHeight, (int)a->rcAttr.attrRcMode.rcMode,
                  a->rcAttr.outFrmRate.frmRateNum, a->rcAttr.outFrmRate.frmRateDen, a->gopAttr.uGopLength);
        }
        else {
            /* no SetDefaultParam exported: fill by hand */
            a->encAttr.eProfile = prof; a->encAttr.uLevel = 51; a->encAttr.uWidth = EW; a->encAttr.uHeight = EH;
            a->encAttr.ePicFormat = IMP_ENC_PIC_FORMAT_420_8BITS;
            a->rcAttr.attrRcMode.rcMode = rc;
            a->rcAttr.outFrmRate.frmRateNum = EFPS; a->rcAttr.outFrmRate.frmRateDen = 1;
            a->gopAttr.uGopCtrlMode = IMP_ENC_GOP_CTRL_MODE_DEFAULT; a->gopAttr.uGopLength = gop;
        }
        if (codec != CODEC_JPEG) {
            a->rcAttr.attrRcMode.attrCbr.uTargetBitRate = BITRATE;
            a->rcAttr.attrRcMode.attrCbr.iMinQP = 20; a->rcAttr.attrRcMode.attrCbr.iMaxQP = 45;
        }
    }
#elif defined(APITEST_T32)
    /* T32: the T23-family IMPEncoderCHNAttr with the 2.x rc unions, filled by SetDefaultParam (as the T32 stream samples) */
    {
        IMPEncoderProfile prof = codec == CODEC_H264 ? IMP_ENC_PROFILE_AVC_MAIN :
                                 codec == CODEC_H265 ? IMP_ENC_PROFILE_HEVC_MAIN : IMP_ENC_PROFILE_JPEG;
        IMPEncoderRcMode rc = codec == CODEC_JPEG ? ENC_RC_MODE_FIXQP : ENC_RC_MODE_CBR;
        NEED(IMP_Encoder_SetDefaultParam) {
            int r = IMP_Encoder_SetDefaultParam(a, prof, rc, EW, EH, EFPS, 1, codec == CODEC_JPEG ? 0 : gop, 0, -1,
                                                codec == CODEC_JPEG ? 0 : BITRATE);
            CHECK(IMP_Encoder_SetDefaultParam, r, a->encAttr.picWidth == EW && a->encAttr.picHeight == EH &&
                  a->rcAttr.attrRcMode.rcMode == rc, "%s %ux%u rc %d", cname[codec], a->encAttr.picWidth,
                  a->encAttr.picHeight, (int)a->rcAttr.attrRcMode.rcMode);
        }
    }
#else
    a->encAttr.picWidth = EW; a->encAttr.picHeight = EH; a->encAttr.bufSize = 0;
    if (codec == CODEC_JPEG) { a->encAttr.enType = PT_JPEG; a->encAttr.profile = 2; return; }
    a->encAttr.enType = codec == CODEC_H264 ? PT_H264 : T_H265;
    a->encAttr.profile = 1;
    a->encAttr.userData.maxUserDataCnt = 2;
    a->encAttr.userData.maxUserDataSize = 64;
    a->rcAttr.outFrmRate.frmRateNum = EFPS; a->rcAttr.outFrmRate.frmRateDen = 1;
    a->rcAttr.maxGop = gop;
    a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_CBR;
    if (codec == CODEC_H264) {
        a->rcAttr.attrRcMode.attrH264Cbr.maxQp = 45; a->rcAttr.attrRcMode.attrH264Cbr.minQp = 15;
        a->rcAttr.attrRcMode.attrH264Cbr.outBitRate = BITRATE;
        a->rcAttr.attrRcMode.attrH264Cbr.iBiasLvl = 0; a->rcAttr.attrRcMode.attrH264Cbr.frmQPStep = 3;
        a->rcAttr.attrRcMode.attrH264Cbr.gopQPStep = 15; a->rcAttr.attrRcMode.attrH264Cbr.adaptiveMode = false;
        a->rcAttr.attrRcMode.attrH264Cbr.gopRelation = false;
    }
# ifdef HAVE_H265
    else {
        a->rcAttr.attrRcMode.attrH265Cbr.maxQp = 45; a->rcAttr.attrRcMode.attrH265Cbr.minQp = 15;
        a->rcAttr.attrRcMode.attrH265Cbr.staticTime = 2; a->rcAttr.attrRcMode.attrH265Cbr.outBitRate = BITRATE;
        a->rcAttr.attrRcMode.attrH265Cbr.iBiasLvl = 0; a->rcAttr.attrRcMode.attrH265Cbr.frmQPStep = 3;
        a->rcAttr.attrRcMode.attrH265Cbr.gopQPStep = 15; a->rcAttr.attrRcMode.attrH265Cbr.flucLvl = 2;
    }
# endif
    a->rcAttr.attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
    a->rcAttr.attrHSkip.hSkipAttr.m = gop - 1;
    a->rcAttr.attrHSkip.hSkipAttr.n = 1;
    a->rcAttr.attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
#endif
}

static int attr_matches(const EAttr *a, int codec)
{
#ifdef ENC_NEW
    return a->encAttr.uWidth == EW && a->encAttr.uHeight == EH &&
           (int)(a->encAttr.eProfile >> 24) == (codec == CODEC_H264 ? IMP_ENC_TYPE_AVC : codec == CODEC_H265 ? IMP_ENC_TYPE_HEVC : IMP_ENC_TYPE_JPEG);
#else
    return a->encAttr.picWidth == EW && a->encAttr.picHeight == EH &&
           a->encAttr.enType == (codec == CODEC_H264 ? PT_H264 : codec == CODEC_H265 ? T_H265 : PT_JPEG);
#endif
}

/* bitrate field of an rc attr (unit BR_UNIT) */
static uint32_t *rc_bitrate(IMPEncoderAttrRcMode *rc, int codec)
{
#ifdef ENC_NEW
    (void)codec;
    return &rc->attrCbr.uTargetBitRate;
#else
# ifdef HAVE_H265
    if (codec == CODEC_H265) return &rc->attrH265Cbr.outBitRate;
# endif
    (void)codec;
    return &rc->attrH264Cbr.outBitRate;
#endif
}

#ifdef ENC_NEW
typedef IMPEncoderChnStat EStat;
#else
typedef IMPEncoderCHNStat EStat;
#endif

/* create + register (+ start). first: print lines. returns 0 on success */
static int chn_up(int chn, int codec, int first, int start)
{
    G(EAttr, a);
    G(EAttr, out);
    int r, ok;

    fill_attr(a, codec, EGOP);
#if HAS_IMP_Encoder_SetPool
# if defined(APITEST_T32)
    /* T32 2.2.0 / T33 2.0.2.1 (disassembly, #447): the pool must exist (MemPoolRequest), else -1.
     * APITEST_NOPOOL=1: no pool binding (the vendor encoder then allocates from rmem; the 64 KB test pool is far too small
     * for the real T33 vendor encoder on silicon: hwicodec "malloc buf_base size:489216 failed, pool id:0") */
    if (!getenv("APITEST_NOPOOL")) NEED(IMP_Encoder_SetPool) {
        static int pool_req;
        if (!pool_req && IMP_System_MemPoolRequest) pool_req = IMP_System_MemPoolRequest(ENC_POOL_ID, 65536, "apitest-enc") == 0 ? 1 : -1;
        if (first) { int rn = IMP_Encoder_SetPool(chn, 30); rep(LBL("IMP_Encoder_SetPool (pool never requested)"), rn, rn == -1 ? V_PASS : V_FAIL, "chn %d pool 30: %d (vendor -1)", chn, rn); }
        r = IMP_Encoder_SetPool(chn, ENC_POOL_ID);
        if (r == -1 && IMP_Encoder_GetPool && IMP_Encoder_GetPool(chn) == ENC_POOL_ID) r = 0;   /* bound in an earlier round: once per channel, as the vendor */
        QR0(IMP_Encoder_SetPool, r, first, "chn %d pool %d (requested: %d, before CreateChn)", chn, ENC_POOL_ID, pool_req);
    }
# else
    NEED(IMP_Encoder_SetPool) { r = IMP_Encoder_SetPool(chn, 0); QR0(IMP_Encoder_SetPool, r, first, "chn %d pool 0 (before CreateChn)", chn); }
# endif
#endif
#if HAS_IMP_Encoder_SetStreamBufSize
    NEED(IMP_Encoder_SetStreamBufSize) { r = IMP_Encoder_SetStreamBufSize(chn, 262144); QR0(IMP_Encoder_SetStreamBufSize, r, first, "chn %d 256 KB (before CreateChn)", chn); }
#endif
    r = IMP_Encoder_CreateChn(chn, a);
    QR0(IMP_Encoder_CreateChn, r, first, "%s chn %d %dx%d", cname[codec], chn, EW, EH);
    if (r) { gfree(a); gfree(out); return -1; }
    r = IMP_Encoder_GetChnAttr(chn, out);
    gchk(out);
    ok = attr_matches(out, codec);
    if (first) CHECK(IMP_Encoder_GetChnAttr, r, ok, "%s read back matches create attr", cname[codec]);
    else if (r || !ok) CHECK(IMP_Encoder_GetChnAttr, r, ok, "%s read back differs", cname[codec]);
    r = IMP_Encoder_RegisterChn(GRP, chn);
    QR0(IMP_Encoder_RegisterChn, r, first, "group %d chn %d", GRP, chn);
    if (!r && start) {
        r = IMP_Encoder_StartRecvPic(chn);
        QR0(IMP_Encoder_StartRecvPic, r, first, "chn %d", chn);
    }
    gfree(a); gfree(out);
    return r;
}

static void chn_down(int chn, int first)
{
    int r;

    r = IMP_Encoder_StopRecvPic(chn); QR0(IMP_Encoder_StopRecvPic, r, first, "chn %d", chn);
    r = IMP_Encoder_UnRegisterChn(chn); QR0(IMP_Encoder_UnRegisterChn, r, first, "chn %d", chn);
    r = IMP_Encoder_DestroyChn(chn); QR0(IMP_Encoder_DestroyChn, r, first, "chn %d", chn);
}

/* ChnStat before/after start */
static void test_query(int chn, int started, int first)
{
    NEED(IMP_Encoder_Query) {
        G(EStat, st);
        int r = IMP_Encoder_Query(chn, st);
        gchk(st);
        if (first) CHECK(IMP_Encoder_Query, r, st->registered, "chn %d %s: registered %d leftPics %u leftBytes %u leftFrames %u curPacks %u work_done %u",
                         chn, started ? "running" : "stopped", st->registered, st->leftPics, st->leftStreamBytes, st->leftStreamFrames, st->curPacks, st->work_done);
        gfree(st);
    }
}

/* --------------------------------------------------- live H.264/H.265 tests */
static void live_common(int chn, int codec)
{
    SI si;
    int r, i;

    /* RequestIDR: the next frames must contain an IDR again */
    NEED(IMP_Encoder_RequestIDR) {
        pull(chn, codec, 3, 0, 0, &si, 2000);      /* let a few P frames pass */
        r = IMP_Encoder_RequestIDR(chn);
        pull(chn, codec, 40, 1, 0, &si, 2000);
        CHECK(IMP_Encoder_RequestIDR, r, si.idr > 0, "%s: IDR %s within %d frames", cname[codec], si.idr ? "seen" : "NOT seen", si.frames);
    }
    /* frame rate (APITEST_SKIP_FRMRATE=1 leaves it out; T30 SIGFPE after a frame-rate change before #366) */
    if (!getenv("APITEST_SKIP_FRMRATE")) NEED(IMP_Encoder_SetChnFrmRate) {
        G(IMPEncoderFrmRate, f);
        IMPEncoderFrmRate nf = { 10, 1 }, of = { EFPS, 1 };
        int r2;
        int64_t t0, t1;

        r = IMP_Encoder_SetChnFrmRate(chn, &nf);
        RB_SETTLE(chn, codec);
        NEED(IMP_Encoder_GetChnFrmRate) {
            r2 = IMP_Encoder_GetChnFrmRate(chn, f);
            gchk(f);
            CHECK(IMP_Encoder_SetChnFrmRate, r, r2 == 0 && f->frmRateNum == 10 && f->frmRateDen == 1, "set 10/1, read back %u/%u", f->frmRateNum, f->frmRateDen);
            CHECK(IMP_Encoder_GetChnFrmRate, r2, f->frmRateNum == 10, "%u/%u", f->frmRateNum, f->frmRateDen);
        }
        /* the output rate follows (skip the first frames after the change) */
        pull(chn, codec, 4, 0, 0, &si, 2000);
        t0 = now_us();
        pull(chn, codec, 20, 0, 0, &si, 2000);
        t1 = now_us();
        rep(LBL("encoder output rate after SetChnFrmRate 10/1"), 0, (si.frames == 20 && (t1 - t0) > 1500000 && (t1 - t0) < 2600000) ? V_PASS : V_FAIL,
            "%d frames in %lld ms = %.1f fps (expect ~10)", si.frames, (long long)((t1 - t0) / 1000), si.frames * 1e6 / (double)(t1 - t0));
#if defined(PLATFORM_T31) && HAS_IMP_Encoder_GetChnGopAttr
        {   /* vendor 1.1.6 glue (#868): at 10/1 the stored GOP length is rescaled (30 at 15 fps -> 20), back at 15/1 -> 30 */
            IMPEncoderGopAttr ga;
            unsigned at10;

            memset(&ga, 0, sizeof ga); IMP_Encoder_GetChnGopAttr(chn, &ga); at10 = ga.uGopLength;
            IMP_Encoder_SetChnFrmRate(chn, &of);
            pull(chn, codec, 6, 0, 0, &si, 2000);
            memset(&ga, 0, sizeof ga); IMP_Encoder_GetChnGopAttr(chn, &ga);
            rep(LBL("GOP length rescaled by SetChnFrmRate 10/1 and back"), 0,
                (at10 == EGOP * 10 / EFPS && ga.uGopLength == EGOP) ? V_PASS : V_FAIL,
                "at 10/1 %u (expect %d), back at %d/1 %u (expect %d)", at10, EGOP * 10 / EFPS, EFPS, ga.uGopLength, EGOP);
        }
#else
        IMP_Encoder_SetChnFrmRate(chn, &of);
#endif
        gfree(f);
    }
    /* rate control attribute get/set */
    NEED(IMP_Encoder_GetChnAttrRcMode) {
        G(IMPEncoderAttrRcMode, rc);
        r = IMP_Encoder_GetChnAttrRcMode(chn, rc);
        gchk(rc);
        CHECK(IMP_Encoder_GetChnAttrRcMode, r, *rc_bitrate(rc, codec) == (uint32_t)BITRATE, "rcMode %d bitrate %u (created with %d)", (int)rc->rcMode, *rc_bitrate(rc, codec), BITRATE);
#if HAS_IMP_Encoder_SetChnAttrRcMode
        NEED(IMP_Encoder_SetChnAttrRcMode) {
            G(IMPEncoderAttrRcMode, rc2);
            uint32_t want = (uint32_t)(384 * BR_UNIT);
            int r2;

            uint32_t before;

            *rc_bitrate(rc, codec) = want;
            r2 = IMP_Encoder_SetChnAttrRcMode(chn, rc);
            RB_SETTLE(chn, codec);
            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
            before = *rc_bitrate(rc2, codec);
            RB_IDR(chn, codec);
            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
            if (RB_AT_IDR)
                CHECK(IMP_Encoder_SetChnAttrRcMode, r2, r == 0 && *rc_bitrate(rc2, codec) == want, "bitrate 384 kbit/s, read back %u before the next IDR, %u after it", before, *rc_bitrate(rc2, codec));
            else
                CHECK(IMP_Encoder_SetChnAttrRcMode, r2, r == 0 && *rc_bitrate(rc2, codec) == want, "bitrate 384 kbit/s, read back %u", *rc_bitrate(rc2, codec));
            *rc_bitrate(rc, codec) = (uint32_t)BITRATE;
            IMP_Encoder_SetChnAttrRcMode(chn, rc);
            RB_IDR(chn, codec);
            gfree(rc2);
        }
#endif
#if HAS_IMP_Encoder_SetChnBitRate
        NEED(IMP_Encoder_SetChnBitRate) {
            G(IMPEncoderAttrRcMode, rc2);
            int r2 = IMP_Encoder_SetChnBitRate(chn, 400 * BR_SET_UNIT, 400 * BR_SET_UNIT);

            uint32_t before;

            RB_SETTLE(chn, codec);
            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
            before = *rc_bitrate(rc2, codec);
            RB_IDR(chn, codec);
            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
            if (RB_AT_IDR)
                CHECK(IMP_Encoder_SetChnBitRate, r2, r == 0 && before == (uint32_t)BITRATE && *rc_bitrate(rc2, codec) == (uint32_t)(400 * BR_UNIT),
                      "400 kbit/s, read back %u before the next IDR (vendor: the old %d), %u after it", before, BITRATE, *rc_bitrate(rc2, codec));
            else
                CHECK(IMP_Encoder_SetChnBitRate, r2, r == 0 && *rc_bitrate(rc2, codec) == (uint32_t)(400 * BR_UNIT), "400 kbit/s, read back %u", *rc_bitrate(rc2, codec));
            IMP_Encoder_SetChnBitRate(chn, 512 * BR_SET_UNIT, 512 * BR_SET_UNIT);
            gfree(rc2);
        }
#endif
        gfree(rc);
    }
    /* GOP */
#if HAS_IMP_Encoder_SetGOPSize
    NEED(IMP_Encoder_SetGOPSize) {
        G(IMPEncoderGOPSizeCfg, g);
        IMPEncoderGOPSizeCfg n; int r2;

        int want = 25;

        n.gopsize = 25;
        r = IMP_Encoder_SetGOPSize(chn, &n);
#if defined(APITEST_T32) && HAS_IMP_Encoder_GetChnFrmRate
        /* T32 vendor 2.2.0: the IMP glue sends the GOP before the next picture (encoder_get_fps_mask),
         * in whole seconds of the encoder's rate: ceil(den * 25 / num) * num / den */
        {
            IMPEncoderFrmRate fr;

            RB_SETTLE(chn, codec);
            if (IMP_Encoder_GetChnFrmRate(chn, &fr) == 0 && fr.frmRateNum && fr.frmRateDen)
                want = (int)(((fr.frmRateDen * 25u + fr.frmRateNum - 1u) / fr.frmRateNum) * fr.frmRateNum / fr.frmRateDen);
        }
#else
        RB_SETTLE(chn, codec);
        want = GOP_SET_RB(25);
#endif
        NEED(IMP_Encoder_GetGOPSize) {
            r2 = IMP_Encoder_GetGOPSize(chn, g); gchk(g);
            CHECK(IMP_Encoder_SetGOPSize, r, r2 == 0 && g->gopsize == want, "set 25, read back %d (expect %d)", g->gopsize, want);
            CHECK(IMP_Encoder_GetGOPSize, r2, g->gopsize == want, "%d", g->gopsize);
        }
        n.gopsize = EGOP; IMP_Encoder_SetGOPSize(chn, &n);
        gfree(g);
    }
#endif
#if HAS_IMP_Encoder_GetChnGopAttr
    NEED(IMP_Encoder_GetChnGopAttr) {
        G(IMPEncoderGopAttr, g);
        G(IMPEncoderGopAttr, g2);
        int r2;

        GOP_SETTLE(chn, codec);   /* pending values of the RC tests above */
        r = IMP_Encoder_GetChnGopAttr(chn, g); gchk(g);
        CHECK(IMP_Encoder_GetChnGopAttr, r, g->uGopLength == GOP_RB(EGOP, g->uMaxSameSenceCnt) && GOP_MODE_OK(g), "mode 0x%x length %u (created %d) maxSameSence %u", (unsigned)g->uGopCtrlMode, g->uGopLength, EGOP, g->uMaxSameSenceCnt);
        NEED(IMP_Encoder_SetChnGopLength) {
            r2 = IMP_Encoder_SetChnGopLength(chn, 25);
            GOP_SETTLE(chn, codec);
            r = IMP_Encoder_GetChnGopAttr(chn, g2); gchk(g2);
#if defined(PLATFORM_T41) && !defined(APITEST_T40)
            CHECK(IMP_Encoder_SetChnGopLength, r2, r == 0 && g2->uGopLength == GOP_SET_RB_T41(25, g2->uMaxSameSenceCnt, g->uGopLength), "25, read back %u (maxSameSence %u)", g2->uGopLength, g2->uMaxSameSenceCnt);
#else
            CHECK(IMP_Encoder_SetChnGopLength, r2, r == 0 && g2->uGopLength == GOP_RB_SET(25), "25, read back %u (maxSameSence %u)", g2->uGopLength, g2->uMaxSameSenceCnt);
#endif
#if defined(PLATFORM_T31)
            /* #804: the run-time length must reach the encoder. Vendor 1.1.6 glue: length in seconds, +1 s unless whole
             * seconds, IDR distance rounded up to whole seconds (GOP 25 at 15 fps -> length 39, IDR every 45).  Count the
             * distance between the 2nd and 3rd IDR after the setter (the 1st may still follow the old schedule); the IDR distance
             * is that length rounded to whole seconds times uMaxSameSenceCnt (vendor 1.1.6 device run 2026-11-24: IDR at 0 and 90). */
            if (r2 == 0) {
                SI sg;
                double sec = 25.0 / EFPS;
                int len = (int)((sec == (int)sec ? sec : sec + 1.0) * EFPS);
                int exp = (len + EFPS - 1) / EFPS * EFPS * (g2->uMaxSameSenceCnt > 1 ? (int)g2->uMaxSameSenceCnt : 1); /* x maxSameSence (vendor 1.1.6 on .224: 90 at GOP 25, maxSameSence 2) */
                int d;

                pull(chn, codec, 3 * exp + 10, 0, 0, &sg, 1000);
                d = sg.idr_n >= 3 ? sg.idr_at[2] - sg.idr_at[1] : -1;
                CHECK(IMP_Encoder_SetChnGopLength, 0, d == exp, "25 at %d fps: IDR distance %d frames (expected %d, %d IDRs in %d frames, at %d %d %d %d)", EFPS, d, exp, sg.idr_n, sg.frames, sg.idr_at[0], sg.idr_at[1], sg.idr_at[2], sg.idr_at[3]);
            }
#endif
        }
        NEED(IMP_Encoder_SetChnGopAttr) {
#if defined(PLATFORM_T41) && !defined(APITEST_T40)
            /* restore the created length: the read-back is the product, the setter takes the length */
            if (g->uMaxSameSenceCnt > 1)
                g->uGopLength /= g->uMaxSameSenceCnt;
            unsigned active = g2->uGopLength;      /* running length after the SetChnGopLength above */
#endif
            r2 = IMP_Encoder_SetChnGopAttr(chn, g);
            GOP_SETTLE(chn, codec);
#if defined(PLATFORM_T41) && !defined(APITEST_T40)
            g->uGopLength = GOP_SET_RB_T41(g->uGopLength, g->uMaxSameSenceCnt, active);
#else
            g->uGopLength = GOP_RB(g->uGopLength, g->uMaxSameSenceCnt);
#endif
            r = IMP_Encoder_GetChnGopAttr(chn, g2); gchk(g2);
            CHECK(IMP_Encoder_SetChnGopAttr, r2, r == 0 && g2->uGopLength == g->uGopLength && g2->uGopCtrlMode == g->uGopCtrlMode, "restore attr, length %u", g2->uGopLength);
        }
        gfree(g); gfree(g2);
    }
#endif
    /* QP bounds etc. (new API) */
#if HAS_IMP_Encoder_SetChnQpBounds && !defined(APITEST_T32)
    NEED(IMP_Encoder_SetChnQpBounds) {
        r = IMP_Encoder_SetChnQpBounds(chn, 22, 44);
        RB_SETTLE(chn, codec);
#if !defined(PLATFORM_T41)
        {
            G(IMPEncoderAttrRcMode, rc);
            int r2 = IMP_Encoder_GetChnAttrRcMode ? IMP_Encoder_GetChnAttrRcMode(chn, rc) : -1;
            gchk(rc);
            CHECK(IMP_Encoder_SetChnQpBounds, r, r2 == 0 && rc->attrCbr.iMinQP == 22 && rc->attrCbr.iMaxQP == 44, "22..44, read back %d..%d", rc->attrCbr.iMinQP, rc->attrCbr.iMaxQP);
            gfree(rc);
        }
#else
        {
            G(IMPEncoderAttrRcMode, rc);
            int r2 = IMP_Encoder_GetChnAttrRcMode ? IMP_Encoder_GetChnAttrRcMode(chn, rc) : -1;
            gchk(rc);
            CHECK(IMP_Encoder_SetChnQpBounds, r, r2 == 0 && rc->attrCbr.iMinQP == 22 && rc->attrCbr.iMaxQP == 44, "22..44, read back %d..%d", rc->attrCbr.iMinQP, rc->attrCbr.iMaxQP);
            gfree(rc);
        }
#endif
        IMP_Encoder_SetChnQpBounds(chn, 20, 45);
    }
#endif
#if HAS_IMP_Encoder_SetChnQpBounds && defined(APITEST_T32) && HAS_IMP_Encoder_GetChnAttrRcMode
    /* T32 / T33 (2.x rc unions): the bounds reach the controller at the next IDR (i264e_set_param 0x12 / I265e
     * reconfig); read back through GetChnAttrRcMode, effect: low bounds 15..20 vs high bounds 40..48, mean
     * picture size over 30 pictures after a requested IDR must drop to less than half */
    if (codec != CODEC_JPEG) NEED(IMP_Encoder_SetChnQpBounds) {
        G(IMPEncoderAttrRcMode, rc);
        SI sl, sh;
        int r2, r3, mn0 = -1, mx0 = -1, mn = -1, mx = -1;
        long al, ah;

        if (IMP_Encoder_GetChnAttrRcMode(chn, rc) == 0) {
            gchk(rc);
# ifdef HAVE_H265
            if (codec == CODEC_H265) { mn0 = rc->attrH265Cbr.minQp; mx0 = rc->attrH265Cbr.maxQp; } else
# endif
            { mn0 = rc->attrH264Cbr.minQp; mx0 = rc->attrH264Cbr.maxQp; }
        }
        /* APITEST_QPB_DUMP=<prefix>: the two pulled streams as <prefix>-<codec>-lo / -hi (slice QP check off the camera, #882) */
        if (getenv("APITEST_QPB_DUMP")) { char fn[256]; snprintf(fn, sizeof fn, "%s-%s-lo", getenv("APITEST_QPB_DUMP"), cname[codec]); g_dump = fopen(fn, "wb"); }
        r = IMP_Encoder_SetChnQpBounds(chn, 15, 20);
        RB_IDR(chn, codec); IMP_Encoder_RequestIDR(chn); pull(chn, codec, 30, 0, 0, &sl, 3000);
        if (g_dump) { char fn[256]; fclose(g_dump); snprintf(fn, sizeof fn, "%s-%s-hi", getenv("APITEST_QPB_DUMP"), cname[codec]); g_dump = fopen(fn, "wb"); }
        {   /* APITEST_QPB_HI=min,max: other high bounds for the size check off the camera (#882; the verdict wants 40..48) */
            int hmn = 40, hmx = 48;
            if (getenv("APITEST_QPB_HI")) sscanf(getenv("APITEST_QPB_HI"), "%d,%d", &hmn, &hmx);
            r2 = IMP_Encoder_SetChnQpBounds(chn, hmn, hmx);
        }
        RB_IDR(chn, codec); IMP_Encoder_RequestIDR(chn); pull(chn, codec, 30, 0, 0, &sh, 3000);
        if (g_dump) { fclose(g_dump); g_dump = NULL; }
        r3 = IMP_Encoder_GetChnAttrRcMode(chn, rc); gchk(rc);
# ifdef HAVE_H265
        if (codec == CODEC_H265) { mn = rc->attrH265Cbr.minQp; mx = rc->attrH265Cbr.maxQp; } else
# endif
        { mn = rc->attrH264Cbr.minQp; mx = rc->attrH264Cbr.maxQp; }
        al = sl.frames ? (long)(sl.bytes / sl.frames) : 0;
        ah = sh.frames ? (long)(sh.bytes / sh.frames) : 0;
        CHECK(IMP_Encoder_SetChnQpBounds, r | r2, r3 == 0 && mn == 40 && mx == 48 && sl.frames > 10 && sh.frames > 10 && ah * 2 < al,
              "%s: 15..20 mean %ld B (%d pictures), 40..48 mean %ld B (%d pictures), read back %d..%d (created %d..%d)",
              cname[codec], al, sl.frames, ah, sh.frames, mn, mx, mn0, mx0);
        if (mn0 > 0 && mx0 >= mn0) IMP_Encoder_SetChnQpBounds(chn, mn0, mx0);
        RB_IDR(chn, codec);
        gfree(rc);
    }
#endif
#if HAS_IMP_Encoder_SetChnQpBoundsPerFrame
    NEED(IMP_Encoder_SetChnQpBoundsPerFrame) { r = IMP_Encoder_SetChnQpBoundsPerFrame(chn, 20, 44, 22, 45); RET0(IMP_Encoder_SetChnQpBoundsPerFrame, r, "I 20..44 P 22..45 (no getter)"); }
#endif
#if HAS_IMP_Encoder_SetChnQpIPDelta
    NEED(IMP_Encoder_SetChnQpIPDelta) { r = IMP_Encoder_SetChnQpIPDelta(chn, 3); RET0(IMP_Encoder_SetChnQpIPDelta, r, "delta 3 (no getter)"); }
#endif
#if HAS_IMP_Encoder_SetChnQp
    NEED(IMP_Encoder_SetChnQp) { r = IMP_Encoder_SetChnQp(chn, 30); RET0(IMP_Encoder_SetChnQp, r, "qp 30 (no getter; FIXQP mode function)"); }
#endif
#if HAS_IMP_Encoder_SetChnEntropyMode
    if (codec == CODEC_H264) NEED(IMP_Encoder_SetChnEntropyMode) { r = IMP_Encoder_SetChnEntropyMode(chn, IMP_ENC_ENTROPY_MODE_CABAC); RET0(IMP_Encoder_SetChnEntropyMode, r, "CABAC (no getter)"); }
#endif
#if HAS_IMP_Encoder_SetFrameRelease
    NEED(IMP_Encoder_SetFrameRelease) { r = IMP_Encoder_SetFrameRelease(chn, 1, 1); RET0(IMP_Encoder_SetFrameRelease, r, "1/1 (no getter)"); }
#endif
#if HAS_IMP_Encoder_SetChnResizeMode
    NEED(IMP_Encoder_SetChnResizeMode) { r = IMP_Encoder_SetChnResizeMode(chn, 0); RET0(IMP_Encoder_SetChnResizeMode, r, "off (no getter)"); }
#endif
#if !HAS_IMP_Encoder_SetChnResizeMode && defined(PLATFORM_T30)
    {   /* T30 1.0.5: exported without a header; any non-zero value = on, a valid channel is never refused (0xc44c8), channel >= 6 -> -1 */
        int (*rm)(int, int) = (int (*)(int, int))dlsym(dlopen(NULL, RTLD_NOW), "IMP_Encoder_SetChnResizeMode");

        if (rm && codec == CODEC_H264) {
            int r1 = GATE("resize_on", rm(chn, 1)), r0 = GATE("resize_off", rm(chn, 0)), r5 = GATE("resize_5", rm(chn, 5)), rb = GATE("resize_chn99", rm(99, 0));

            rm(chn, 0);
            rep(LBL("IMP_Encoder_SetChnResizeMode"), r1 | r0 | r5, (r1 == 0 && r0 == 0 && r5 == 0 && rb == -1) ? V_PASS : V_FAIL,
                "on %d, off %d, value 5 %d (any non-zero = on), chn 99 %d (no getter)", r1, r0, r5, rb);
        } else if (!rm) rep_na(LBL("IMP_Encoder_SetChnResizeMode"));
    }
#endif
    /* ROI */
#if HAS_IMP_Encoder_SetChnROI && !defined(APITEST_T32)
    NEED(IMP_Encoder_SetChnROI) {
        G(IMPEncoderROICfg, g);
        IMPEncoderROICfg c;
        int r2;

        memset(&c, 0, sizeof(c));
        c.u32Index = 0; c.bEnable = true; c.bRelatedQp = true; c.s32Qp = -4;
        c.rect.p0.x = 64; c.rect.p0.y = 64; c.rect.p1.x = 319; c.rect.p1.y = 255;
        r = IMP_Encoder_SetChnROI(chn, &c);
        NEED(IMP_Encoder_GetChnROI) {
            g->u32Index = 0;
            r2 = IMP_Encoder_GetChnROI(chn, g); gchk(g);
            if (r != 0 && REFUSES_UNSUPPORTED) {
                rep(FN(IMP_Encoder_SetChnROI), r, V_NA, "enabling a region is refused: no macroblock QP map in this SoC's OpenIMP encoder");
                rep(FN(IMP_Encoder_GetChnROI), r2, V_NA, "nothing to read back (SetChnROI refused)");
            } else {
            /* T10/T20/T21/T30 vendor (T20 3.12.0 0x4899c/0x48c3c, T21 1.0.33, T23 1.3.0 0x53ea0/0x54164, T30 1.0.5 0xc1ec0/0xc22e8: sra 4 in Set, sll 4 in Get): Set stores the corners /16 (truncating) as
             * macroblock bytes and Get returns them x16, so the read-back is the rect rounded down to 16 */
#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
            CHECK(IMP_Encoder_SetChnROI, r, r2 == 0 && g->bEnable && g->rect.p0.x == 64 && g->rect.p0.y == 64 && g->rect.p1.x == 304 && g->rect.p1.y == 240 && g->s32Qp == -4,
#else
            CHECK(IMP_Encoder_SetChnROI, r, r2 == 0 && g->bEnable && g->rect.p0.x == 64 && g->rect.p1.x == 319 && g->s32Qp == -4,
#endif
                  "roi 0 (64,64)-(319,255) dQP -4, read back en %d (%d,%d)-(%d,%d) qp %d", g->bEnable, g->rect.p0.x, g->rect.p0.y, g->rect.p1.x, g->rect.p1.y, g->s32Qp);
            CHECK(IMP_Encoder_GetChnROI, r2, g->bEnable, "enabled %d", g->bEnable);
            }
        }
        pull(chn, codec, 3, 0, 0, &si, 2000);
        rep(LBL("stream with ROI enabled"), 0, si.frames == 3 ? V_PASS : V_FAIL, "%d frames after enabling the ROI", si.frames);
        c.bEnable = false; IMP_Encoder_SetChnROI(chn, &c);
        gfree(g);
    }
#endif
#if HAS_IMP_Encoder_SetChnRoiAttr
    NEED(IMP_Encoder_SetChnRoiAttr) {
        G(IMPEncoderRoiAttr, g);
        G(IMPEncoderRoiAttr, c);
        int r2;

        c->st_roi[0].enable = true; c->st_roi[0].rect.x = 64; c->st_roi[0].rect.y = 64; c->st_roi[0].rect.w = 128; c->st_roi[0].rect.h = 96;
        c->st_roi[0].mode = IMP_ROI_QPMODE_DELTA; c->st_roi[0].qp = -4;
        r = IMP_Encoder_SetChnRoiAttr(chn, c);
        NEED(IMP_Encoder_GetChnRoiAttr) {
            r2 = IMP_Encoder_GetChnRoiAttr(chn, g); gchk(g);
#ifdef PLATFORM_T41
            if (r == -1 && !(getenv("OPENIMP_T41_ROI") && getenv("OPENIMP_T41_ROI")[0] == '1')) {
                rep(FN(IMP_Encoder_SetChnRoiAttr), r, V_NA, "refused without OPENIMP_T41_ROI=1 (experimental ROI table, docs/ROI.md)");
                rep(FN(IMP_Encoder_GetChnRoiAttr), r2, V_NA, "nothing to read back (SetChnRoiAttr refused)");
            } else {
#endif
            CHECK(IMP_Encoder_SetChnRoiAttr, r, r2 == 0 && g->st_roi[0].enable && g->st_roi[0].rect.w == 128 && g->st_roi[0].qp == -4,
                  "win0 (64,64) 128x96 dQP -4, read back en %d %ux%u qp %d", g->st_roi[0].enable, g->st_roi[0].rect.w, g->st_roi[0].rect.h, g->st_roi[0].qp);
            CHECK(IMP_Encoder_GetChnRoiAttr, r2, g->st_roi[0].enable, "win0 enabled %d", g->st_roi[0].enable);
#ifdef PLATFORM_T41
            }
#endif
        }
        pull(chn, codec, 3, 0, 0, &si, 2000);
        rep(LBL("stream with ROI enabled"), 0, si.frames == 3 ? V_PASS : V_FAIL, "%d frames after enabling the ROI", si.frames);
        c->st_roi[0].enable = false; IMP_Encoder_SetChnRoiAttr(chn, c);
        gfree(g); gfree(c);
    }
#if defined(PLATFORM_T41) && HAS_IMP_Encoder_SetChnMapRoi
    /* map: one byte per 16x16 block of the channel (low 2 bits mode: 2 = relative, high 6 bits a two's complement delta); a map of
     * the wrong size, the absolute mode and a quality outside -26..25 are refused (vendor 1.2.6/1.3.1 checks); NULL/0 removes it */
    NEED(IMP_Encoder_SetChnMapRoi) {
        if (codec == CODEC_H264) {
            int cols = (EW + 15) / 16, rows = (EH + 15) / 16, n = cols * rows, i, rbad, rmode, rq, rok, rnull;
            IMPEncoderMapRoiAttr m;
            uint8_t *map = (uint8_t *)calloc(n + 1, 1);

            for (i = 0; i < n; i++) map[i] = (uint8_t)((((i % cols) < cols / 2 ? ((-8) & 63) : 6) << 2) | 2);      /* left half -8, right half +6 */
            memset(&m, 0, sizeof(m)); m.map = map; m.mapSize = n; m.mode = IMP_ROI_QPMODE_DELTA; m.Quality = 0;
            r = IMP_Encoder_SetChnMapRoi(chn, &m);
            pull(chn, codec, 3, 0, 0, &si, 2000);
            rok = si.frames == 3;
            m.mapSize = n - 1; rbad = IMP_Encoder_SetChnMapRoi(chn, &m);
            m.mapSize = n; m.mode = IMP_ROI_QPMODE_FIXED_QP; rmode = IMP_Encoder_SetChnMapRoi(chn, &m);
            m.mode = IMP_ROI_QPMODE_DELTA; m.Quality = 26; rq = IMP_Encoder_SetChnMapRoi(chn, &m);
            m.map = NULL; m.mapSize = 0; m.Quality = 0; rnull = IMP_Encoder_SetChnMapRoi(chn, &m);
            pull(chn, codec, 3, 0, 0, &si, 2000);
            CHECK(IMP_Encoder_SetChnMapRoi, r, rok && rbad != 0 && rmode != 0 && rq != 0 && rnull == 0 && si.frames == 3,
                  "%dx%d blocks (%d bytes) stored, stream flows %d; size n-1 -> %d, absolute mode -> %d, quality 26 -> %d (all refused); NULL map removes it -> %d, stream flows %d",
                  cols, rows, n, rok, rbad, rmode, rq, rnull, si.frames);
            free(map);
        }
    }
#else
    rep(FN(IMP_Encoder_SetChnMapRoi), 0, V_SKIP, "needs a per-macroblock QP map sized for the channel");
#endif
#endif
    /* MaxPictureSize */
#if HAS_IMP_Encoder_GetChnMaxPictureSize
    NEED(IMP_Encoder_GetChnMaxPictureSize) {
        G(uint32_t, pi);
        G(uint32_t, pp);
        r = IMP_Encoder_GetChnMaxPictureSize(chn, pi, pp); gchk(pi); gchk(pp);
        RET0(IMP_Encoder_GetChnMaxPictureSize, r, "I %u P %u", *pi, *pp);
        NEED(IMP_Encoder_SetChnMaxPictureSize) {
            uint32_t a = 60000, b = 40000, oi = *pi, op = *pp;
            int r2 = IMP_Encoder_SetChnMaxPictureSize(chn, a, b);
            r = IMP_Encoder_GetChnMaxPictureSize(chn, pi, pp); gchk(pi); gchk(pp);
#ifdef PLATFORM_T23
            /* stock T23: one frame-loss threshold from the I value, both getters report it */
            CHECK(IMP_Encoder_SetChnMaxPictureSize, r2, r == 0 && *pi == a && *pp == a, "I 60000 P 40000, read back %u/%u (T23: one shared threshold from I)", *pi, *pp);
#else
            CHECK(IMP_Encoder_SetChnMaxPictureSize, r2, r == 0 && *pi == a && *pp == b, "I 60000 P 40000, read back %u/%u", *pi, *pp);
#endif
            IMP_Encoder_SetChnMaxPictureSize(chn, oi, op);
        }
        gfree(pi); gfree(pp);
    }
#elif HAS_IMP_Encoder_SetChnMaxPictureSize
    NEED(IMP_Encoder_SetChnMaxPictureSize) {
        r = IMP_Encoder_SetChnMaxPictureSize(chn, 60000, 40000);
#if defined(PLATFORM_T41)
        /* T41 hardware rate control limits each picture to ~95 % of uMaxPictureSize (kbit): the biggest picture after the call is
         * checked against the limit and against the biggest picture before it (IDR requested both times) */
        if (codec != CODEC_JPEG) {
            SI s0, s1;
            int r2, lim_kbit;

            IMP_Encoder_RequestIDR(chn); pull(chn, codec, 30, 0, 0, &s0, 3000);
            lim_kbit = s0.bytes_max * 8 / 1000 / 2;            /* half of the biggest picture so far (dark scenes give small pictures) */
            if (lim_kbit < 6) lim_kbit = 6;
            r2 = IMP_Encoder_SetChnMaxPictureSize(chn, (uint32_t)lim_kbit, (uint32_t)lim_kbit);
            RB_SETTLE(chn, codec);
            IMP_Encoder_RequestIDR(chn); pull(chn, codec, 30, 0, 0, &s1, 3000);
            IMP_Encoder_SetChnMaxPictureSize(chn, 60000, 40000);
#if defined(APITEST_T32) && !defined(APITEST_T33)
            /* T32 vendor 2.2.0 (#295): no cap.  i264e_reconfig_maxpicsize_set 0x717ec stores I/P (+0x2bc4/+0x2bc8, flag
             * 0x100000), i264e_idr_reconfig takes them at the next IDR, the controller passes them to the HERA as the
             * picture's size target (FRAME_START 0x19b1b4/0x19b1bc read e+0x85c/+0x860) and never re-encodes
             * (JZ_VPU_RC_FRAME_REPEATE_JUDGE_H264_T33 0x19d44c returns 0).  On the PRJ007 module the vendor lib gives the
             * same result as OpenIMP: biggest picture 2.2x (H.264) / 2.8x (H.265) of the limit, above the one before.
             * Checked: both calls accepted and the stream keeps flowing; the sizes are reported only. */
            CHECK(IMP_Encoder_SetChnMaxPictureSize, r | r2, r == 0 && r2 == 0 && s1.frames > 10,
                  "%s: I and P %d kbit (%d B) as the picture size target (T32: no cap, as vendor 2.2.0): biggest picture %d B, mean %lu B (before: %d B, mean %lu B), %d pictures",
                  cname[codec], lim_kbit, lim_kbit * 125, s1.bytes_max, s1.frames ? s1.bytes / (unsigned long)s1.frames : 0UL,
                  s0.bytes_max, s0.frames ? s0.bytes / (unsigned long)s0.frames : 0UL, s1.frames);
#else
            CHECK(IMP_Encoder_SetChnMaxPictureSize, r | r2, r == 0 && r2 == 0 && s1.frames > 10 && s1.bytes_max <= lim_kbit * 125 * 115 / 100 && s1.bytes_max < s0.bytes_max,
                  "%s: I and P limited to %d kbit (%d B): biggest picture %d B (before: %d B), limit +15 %%", cname[codec], lim_kbit, lim_kbit * 125, s1.bytes_max, s0.bytes_max);
#endif
        } else
#endif
        RET0(IMP_Encoder_SetChnMaxPictureSize, r, "I 60000 P 40000 (no getter)");
    }
#endif
    (void)i;
}

/* ---------------------------------------------- old-API (T10..T23) live tests */
#define RT_INT(SETF, GETF, VAL, LABEL) \
    NEED(SETF) { \
        G(int, g); int r2, r3, was = 0; \
        if (GETF) { r3 = GETF(chn, g); gchk(g); was = *g; } \
        r2 = SETF(chn, VAL); \
        r3 = GETF ? GETF(chn, g) : -1; gchk(g); \
        CHECK(SETF, r2, r3 == 0 && *g == (VAL), LABEL " set %d, read back %d", (int)(VAL), *g); \
        if (GETF) rep(FN(GETF), r3, r3 == 0 ? V_PASS : V_FAIL, LABEL " = %d", *g); \
        SETF(chn, was); gfree(g); }

static void live_old(int chn, int codec)
{
#ifdef ENC_OLD
    SI si;
    int r;

# if HAS_IMP_Encoder_SetChnFrmUsedMode
    NEED(IMP_Encoder_SetChnFrmUsedMode) {
        G(IMPEncoderAttrFrmUsed, g); G(IMPEncoderAttrFrmUsed, o);
        IMPEncoderAttrFrmUsed n; int r2, r3;

        r3 = IMP_Encoder_GetChnFrmUsedMode ? IMP_Encoder_GetChnFrmUsedMode(chn, o) : -1; gchk(o);
        n.enable = true; n.frmUsedMode = ENC_FRM_BYPASS; n.frmUsedTimes = 2;
        r2 = IMP_Encoder_SetChnFrmUsedMode(chn, &n);
        r = IMP_Encoder_GetChnFrmUsedMode ? IMP_Encoder_GetChnFrmUsedMode(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetChnFrmUsedMode, r2, r == 0 && g->enable && g->frmUsedMode == ENC_FRM_BYPASS && g->frmUsedTimes == 2,
              "enable bypass x2, read back en %d mode %d times %u", g->enable, (int)g->frmUsedMode, g->frmUsedTimes);
        rep(FN(IMP_Encoder_GetChnFrmUsedMode), r3, r3 == 0 ? V_PASS : V_FAIL, "initial en %d mode %d times %u", o->enable, (int)o->frmUsedMode, o->frmUsedTimes);
        IMP_Encoder_SetChnFrmUsedMode(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetSuperFrameCfg
    NEED(IMP_Encoder_SetSuperFrameCfg) {
        G(IMPEncoderSuperFrmCfg, g); G(IMPEncoderSuperFrmCfg, o);
        IMPEncoderSuperFrmCfg n; int r2, r3;

        r3 = IMP_Encoder_GetSuperFrameCfg ? IMP_Encoder_GetSuperFrameCfg(chn, o) : -1; gchk(o);
        memset(&n, 0, sizeof(n));
#if defined(PLATFORM_T20) || defined(PLATFORM_T10)
        n.superFrmMode = IMP_RC_SUPERFRM_REENCODE; n.superIFrmBitsThr = 400000; n.superPFrmBitsThr = 200000; n.superBFrmBitsThr = 100000;   /* T10/T20: the OEM i264e has re-encode only, no discard */
#else
        n.superFrmMode = IMP_RC_SUPERFRM_DISCARD; n.superIFrmBitsThr = 400000; n.superPFrmBitsThr = 200000; n.superBFrmBitsThr = 100000;
        n.rcPriority = IMP_RC_PRIORITY_BITRATE_FIRST;
#endif
        n.rcPriority = IMP_RC_PRIORITY_BITRATE_FIRST;
#if defined(PLATFORM_T21)
        /* T21: the Helix eprc controller has no super-frame control: mode NONE (thresholds stored) is the supported setting, any other
         * mode is refused with -1 and leaves the stored configuration untouched */
        {
            IMPEncoderSuperFrmCfg bad; int rb, rg;

            memset(&bad, 0, sizeof(bad)); bad.superFrmMode = IMP_RC_SUPERFRM_DISCARD; bad.superIFrmBitsThr = 1; bad.rcPriority = IMP_RC_PRIORITY_BITRATE_FIRST;
            n.superFrmMode = IMP_RC_SUPERFRM_NONE;
            r2 = IMP_Encoder_SetSuperFrameCfg(chn, &n);
            r = IMP_Encoder_GetSuperFrameCfg ? IMP_Encoder_GetSuperFrameCfg(chn, g) : -1; gchk(g);
            rb = IMP_Encoder_SetSuperFrameCfg(chn, &bad);
            { IMPEncoderSuperFrmCfg *g2 = (IMPEncoderSuperFrmCfg *)gnew(sizeof(*g2));
              rg = IMP_Encoder_GetSuperFrameCfg(chn, g2); gchk(g2);
              CHECK(IMP_Encoder_SetSuperFrameCfg, r2, r == 0 && rb != 0 && rg == 0 && g->superFrmMode == IMP_RC_SUPERFRM_NONE && g->superIFrmBitsThr == n.superIFrmBitsThr && g->rcPriority == n.rcPriority &&
                    g2->superFrmMode == IMP_RC_SUPERFRM_NONE && g2->superIFrmBitsThr == n.superIFrmBitsThr,
                    "mode NONE I %u P %u prio %d stored and read back; mode DISCARD refused (ret %d), stored config unchanged (mode %d I %u)",
                    n.superIFrmBitsThr, n.superPFrmBitsThr, (int)g->rcPriority, rb, (int)g2->superFrmMode, g2->superIFrmBitsThr);
              gfree(g2); }
        }
#else
        r2 = IMP_Encoder_SetSuperFrameCfg(chn, &n);
#if defined(PLATFORM_T30)
        /* vendor T30 1.0.5: i264e_super_rc_set only stages the values, the controller takes them with the next picture
         * (a Get right after the Set shows the old ones, camera A/B 2026-11-03): read after a few pictures */
        pull(chn, codec, 3, 0, 0, &si, 2000);
#elif defined(APITEST_T32)
        /* vendor T33 2.0.2.1 and T32: taken at the next IDR (case t33e1, QEMU); the T32 build (APITEST_T32 without
         * APITEST_T33) read the old values right after the Set (QEMU T32 2026-11-24, #99) */
        IMP_Encoder_RequestIDR(chn);
        pull(chn, codec, 3, 0, 0, &si, 2000);
#endif
        r = IMP_Encoder_GetSuperFrameCfg ? IMP_Encoder_GetSuperFrameCfg(chn, g) : -1; gchk(g);
        if (r2 != 0 && REFUSES_UNSUPPORTED)
            rep(FN(IMP_Encoder_SetSuperFrameCfg), r2, V_NA, "mode %d refused: the Helix eprc controller of OpenIMP has no frame discard (T20: re-encode only, T21: none)", (int)n.superFrmMode);
        else
        CHECK(IMP_Encoder_SetSuperFrameCfg, r2, r == 0 && g->superFrmMode == n.superFrmMode && g->superIFrmBitsThr == n.superIFrmBitsThr && g->rcPriority == n.rcPriority,
              "mode %d I %u P %u, read back mode %d I %u P %u prio %d", (int)n.superFrmMode, n.superIFrmBitsThr, n.superPFrmBitsThr, (int)g->superFrmMode, g->superIFrmBitsThr, g->superPFrmBitsThr, (int)g->rcPriority);
#endif
        rep(FN(IMP_Encoder_GetSuperFrameCfg), r3, r3 == 0 ? V_PASS : V_FAIL, "initial mode %d I %u P %u", (int)o->superFrmMode, o->superIFrmBitsThr, o->superPFrmBitsThr);
        IMP_Encoder_SetSuperFrameCfg(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetChnDenoise
    NEED(IMP_Encoder_SetChnDenoise) {
        G(IMPEncoderAttrDenoise, g); G(IMPEncoderAttrDenoise, o);
        IMPEncoderAttrDenoise n; int r2, r3;

        r3 = IMP_Encoder_GetChnDenoise ? IMP_Encoder_GetChnDenoise(chn, o) : -1; gchk(o);
        memset(&n, 0, sizeof(n)); n.enable = true; n.dnType = 1; n.dnIQp = 2; n.dnPQp = 3;
        r2 = IMP_Encoder_SetChnDenoise(chn, &n);
        r = IMP_Encoder_GetChnDenoise ? IMP_Encoder_GetChnDenoise(chn, g) : -1; gchk(g);
#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21)
        /* vendor T10/T20 3.12.0, T21 1.0.33 (i264e_reconfig_dn_set): the CreateChn enable switch stays as created
         * (the test channel is created with denoise off), so dnType is forced to 0, IQp/PQp are kept and Get leaves
         * .enable of the caller's struct untouched */
        CHECK(IMP_Encoder_SetChnDenoise, r2, r == 0 && !g->enable && g->dnType == 0 && g->dnIQp == 2 && g->dnPQp == 3,
              "en type 1 IQp 2 PQp 3 on a channel created with denoise off (vendor: type forced 0, QPs kept), read back en %d type %d %d/%d", g->enable, g->dnType, g->dnIQp, g->dnPQp);
#else
        CHECK(IMP_Encoder_SetChnDenoise, r2, r == 0 && g->enable && g->dnType == 1 && g->dnIQp == 2 && g->dnPQp == 3,
              "en type 1 IQp 2 PQp 3, read back en %d type %d %d/%d", g->enable, g->dnType, g->dnIQp, g->dnPQp);
#endif
        rep(FN(IMP_Encoder_GetChnDenoise), r3, r3 == 0 ? V_PASS : V_FAIL, "initial en %d type %d %d/%d", o->enable, o->dnType, o->dnIQp, o->dnPQp);
        IMP_Encoder_SetChnDenoise(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetChnHSkip
    NEED(IMP_Encoder_SetChnHSkip) {
        G(IMPEncoderAttrHSkip, g); G(IMPEncoderAttrHSkip, o);
        int r2, r3;

        r3 = IMP_Encoder_GetChnHSkip ? IMP_Encoder_GetChnHSkip(chn, o) : -1; gchk(o);
        r2 = IMP_Encoder_SetChnHSkip(chn, o);
        r = IMP_Encoder_GetChnHSkip ? IMP_Encoder_GetChnHSkip(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetChnHSkip, r2, r == 0 && g->skipType == o->skipType && g->m == o->m && g->n == o->n,
              "same attr (type %d m %d n %d), read back type %d m %d n %d", (int)o->skipType, o->m, o->n, (int)g->skipType, g->m, g->n);
        rep(FN(IMP_Encoder_GetChnHSkip), r3, r3 == 0 ? V_PASS : V_FAIL, "type %d m %d n %d maxSameScene %d", (int)o->skipType, o->m, o->n, o->maxSameSceneCnt);
        NEED(IMP_Encoder_SetChnHSkipBlackEnhance) { r = IMP_Encoder_SetChnHSkipBlackEnhance(chn, 0); RET0(IMP_Encoder_SetChnHSkipBlackEnhance, r, "off (no getter)"); }
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetChnDemask
    NEED(IMP_Encoder_SetChnDemask) {
        G(IMPEncoderAttrDemask, g); G(IMPEncoderAttrDemask, o);
        IMPEncoderAttrDemask n; int r2, r3;

        r3 = IMP_Encoder_GetChnDemask ? IMP_Encoder_GetChnDemask(chn, o) : -1; gchk(o);
        memset(&n, 0, sizeof(n)); n.enable = true; n.isAutoMode = false; n.demaskCnt = 3; n.demaskThresd = 5;
        r2 = IMP_Encoder_SetChnDemask(chn, &n);
        r = IMP_Encoder_GetChnDemask ? IMP_Encoder_GetChnDemask(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetChnDemask, r2, r == 0 && g->enable && g->demaskCnt == 3 && g->demaskThresd == 5, "cnt 3 thr 5, read back en %d cnt %d thr %d", g->enable, g->demaskCnt, g->demaskThresd);
        rep(FN(IMP_Encoder_GetChnDemask), r3, r3 == 0 ? V_PASS : V_FAIL, "initial en %d cnt %d thr %d", o->enable, o->demaskCnt, o->demaskThresd);
        IMP_Encoder_SetChnDemask(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetChnColor2Grey
    NEED(IMP_Encoder_SetChnColor2Grey) {
        G(IMPEncoderColor2GreyCfg, g); G(IMPEncoderColor2GreyCfg, o);
        IMPEncoderColor2GreyCfg n; int r2, r3;

        r3 = IMP_Encoder_GetChnColor2Grey ? IMP_Encoder_GetChnColor2Grey(chn, o) : -1; gchk(o);
        n.enable = true;
        r2 = IMP_Encoder_SetChnColor2Grey(chn, &n);
        r = IMP_Encoder_GetChnColor2Grey ? IMP_Encoder_GetChnColor2Grey(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetChnColor2Grey, r2, r == 0 && g->enable, "enable, read back %d", g->enable);
        rep(FN(IMP_Encoder_GetChnColor2Grey), r3, r3 == 0 ? V_PASS : V_FAIL, "initial %d", o->enable);
        pull(chn, codec, 3, 0, 0, &si, 2000);
        rep(LBL("stream with Color2Grey enabled"), 0, si.frames == 3 ? V_PASS : V_FAIL, "%d frames", si.frames);
        IMP_Encoder_SetChnColor2Grey(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetMbRC
    RT_INT(IMP_Encoder_SetMbRC, IMP_Encoder_GetMbRC, 1, "MbRC")
# endif
# if HAS_IMP_Encoder_SetChangeRef
#  if defined(PLATFORM_T23) || defined(PLATFORM_T21) || defined(PLATFORM_T30)
    /* stock T23 1.3.0, T21 1.0.33 (0x48ecc) and T30 1.0.5 (same code, #177): the getter has no read-back and fails on a created channel */
    NEED(IMP_Encoder_SetChangeRef) {
        int r2 = IMP_Encoder_SetChangeRef(chn, 1);
        int g = 0, r3 = IMP_Encoder_GetChangeRef ? IMP_Encoder_GetChangeRef(chn, &g) : -1;
        RET0(IMP_Encoder_SetChangeRef, r2, "ChangeRef set 1 (T21/T23/T30 stock: no read-back)");
        rep(FN(IMP_Encoder_GetChangeRef), r3, r3 == -1 ? V_PASS : V_FAIL, "stock T21/T23/T30 getter fails on a created channel: ret %d", r3);
        IMP_Encoder_SetChangeRef(chn, 0);
    }
#  else
    RT_INT(IMP_Encoder_SetChangeRef, IMP_Encoder_GetChangeRef, 1, "ChangeRef")
#  endif
# endif
# if HAS_IMP_Encoder_SetQpgMode && !defined(APITEST_T32)
    NEED(IMP_Encoder_SetQpgMode) {
        G(IMPEncoderQpgMode, g); G(IMPEncoderQpgMode, o);
        IMPEncoderQpgMode n = ENC_QPG_CRP; int r2, r3;

        r3 = IMP_Encoder_GetQpgMode ? IMP_Encoder_GetQpgMode(chn, o) : -1; gchk(o);
        r2 = IMP_Encoder_SetQpgMode(chn, &n);
        r = IMP_Encoder_GetQpgMode ? IMP_Encoder_GetQpgMode(chn, g) : -1; gchk(g);
        if (r2 != 0 && REFUSES_UNSUPPORTED)
            rep(FN(IMP_Encoder_SetQpgMode), r2, V_NA, "CRP refused: only CLOSE exists in the eprc controller of OpenIMP (macroblock RC is SetMbRC)");
        else
        CHECK(IMP_Encoder_SetQpgMode, r2, r == 0 && *g == n, "CRP (%d), read back %d", (int)n, (int)*g);
        rep(FN(IMP_Encoder_GetQpgMode), r3, r3 == 0 ? V_PASS : V_FAIL, "initial %d", (int)*o);
        IMP_Encoder_SetQpgMode(chn, o);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetH264TransCfg
    if (codec == CODEC_H264) NEED(IMP_Encoder_SetH264TransCfg) {
        G(IMPEncoderH264TransCfg, g); G(IMPEncoderH264TransCfg, o);
        int r2, r3;

        r3 = IMP_Encoder_GetH264TransCfg ? IMP_Encoder_GetH264TransCfg(chn, o) : -1; gchk(o);
        r2 = IMP_Encoder_SetH264TransCfg(chn, o);
        r = IMP_Encoder_GetH264TransCfg ? IMP_Encoder_GetH264TransCfg(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetH264TransCfg, r2, r == 0 && !memcmp(g, o, sizeof(*g)), "same cfg written back, chroma_qp_index_offset %d", g->chroma_qp_index_offset);
        rep(FN(IMP_Encoder_GetH264TransCfg), r3, r3 == 0 ? V_PASS : V_FAIL, "chroma_qp_index_offset %d", o->chroma_qp_index_offset);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_SetH265TransCfg
    if (codec == CODEC_H265) NEED(IMP_Encoder_SetH265TransCfg) {
        G(IMPEncoderH265TransCfg, g); G(IMPEncoderH265TransCfg, o);
        int r2, r3;

        r3 = IMP_Encoder_GetH265TransCfg ? IMP_Encoder_GetH265TransCfg(chn, o) : -1; gchk(o);
        r2 = IMP_Encoder_SetH265TransCfg(chn, o);
        r = IMP_Encoder_GetH265TransCfg ? IMP_Encoder_GetH265TransCfg(chn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetH265TransCfg, r2, r == 0 && !memcmp(g, o, sizeof(*g)), "same cfg written back, cr %d cb %d", g->chroma_cr_qp_offset, g->chroma_cb_qp_offset);
        rep(FN(IMP_Encoder_GetH265TransCfg), r3, r3 == 0 ? V_PASS : V_FAIL, "cr %d cb %d", o->chroma_cr_qp_offset, o->chroma_cb_qp_offset);
        gfree(g); gfree(o);
    }
# endif
# if HAS_IMP_Encoder_InsertUserData
    NEED(IMP_Encoder_InsertUserData) {
        char ud[] = "apitest-user-data";

#  if defined(APITEST_T32)
        /* vendor T33 2.0.2.1: a channel created without user-data buffers (encAttr.userData) refuses with 0x80040080; the SEI
         * path with buffers is case t33e1.  The T32 library shares this path (src/t32/openimp_t32_enc_e1.c, "T32 2.2.0 entry
         * points are the same"): the T32 build reported the refusal as a missing SEI (T32 PRJ007 device + QEMU 2026-11-24, #99) */
        r = IMP_Encoder_InsertUserData(chn, ud, (uint32_t)strlen(ud));
        rep(FN(IMP_Encoder_InsertUserData), r, r == (int)0x80040080 ? V_PASS : V_FAIL, "no user-data buffers: 0x%x (vendor 0x80040080)", (unsigned)r);
#  else
        r = IMP_Encoder_InsertUserData(chn, ud, (uint32_t)strlen(ud));
        pull(chn, codec, 30, 0, 0, &si, 2000);
        CHECK(IMP_Encoder_InsertUserData, r, si.user > 0, "\"%s\": %s in the next %d frames (SEI)", ud, si.user ? "found" : "NOT found", si.frames);
#  endif
    }
# endif
# if HAS_IMP_Encoder_GetGDRCfg
    NEED(IMP_Encoder_GetGDRCfg) {
        G(IMPEncoderGDRCfg, g); G(IMPEncoderGDRCfg, g2);
        int r2;

        r = IMP_Encoder_GetGDRCfg(chn, g); gchk(g);
        RET0(IMP_Encoder_GetGDRCfg, r, "en %d cycle %d frames %d", g->enable, g->gdrCycle, g->gdrFrames);
        NEED(IMP_Encoder_SetGDRCfg) {
            r2 = IMP_Encoder_SetGDRCfg(chn, g);
            r = IMP_Encoder_GetGDRCfg(chn, g2); gchk(g2);
            CHECK(IMP_Encoder_SetGDRCfg, r2, r == 0 && !memcmp(g, g2, sizeof(*g)), "same cfg written back");
        }
        NEED(IMP_Encoder_RequestGDR) { r = IMP_Encoder_RequestGDR(chn, 5); RET0(IMP_Encoder_RequestGDR, r, "5 frames"); }
        gfree(g); gfree(g2);
    }
# endif
# if HAS_IMP_Encoder_Getframelossthd
    NEED(IMP_Encoder_Getframelossthd) {
        G(uint32_t, g);
        uint32_t was;
        int r2;

        r = IMP_Encoder_Getframelossthd(chn, g); gchk(g);
        RET0(IMP_Encoder_Getframelossthd, r, "%u", *g);
        was = *g;
        NEED(IMP_Encoder_Setframelossthd) {
            r2 = IMP_Encoder_Setframelossthd(chn, was + 1);
            r = IMP_Encoder_Getframelossthd(chn, g); gchk(g);
            CHECK(IMP_Encoder_Setframelossthd, r2, r == 0 && *g == was + 1, "%u, read back %u", was + 1, *g);
            IMP_Encoder_Setframelossthd(chn, was);
        }
        gfree(g);
    }
# endif
# if HAS_IMP_Encoder_GetChnCrop
    NEED(IMP_Encoder_GetChnCrop) {
        G(IMPEncoderCropCfg, g); G(IMPEncoderCropCfg, g2);
        int r2;

        r = IMP_Encoder_GetChnCrop(chn, g); gchk(g);
        RET0(IMP_Encoder_GetChnCrop, r, "en %d %u,%u %ux%u", g->enable, g->x, g->y, g->w, g->h);
        NEED(IMP_Encoder_SetChnCrop) {
            r2 = IMP_Encoder_SetChnCrop(chn, g);
            r = IMP_Encoder_GetChnCrop(chn, g2); gchk(g2);
            CHECK(IMP_Encoder_SetChnCrop, r2, r == 0 && !memcmp(g, g2, sizeof(*g)), "same cfg written back");
        }
        gfree(g); gfree(g2);
    }
# endif
# if !HAS_IMP_Encoder_GetChnCrop && (defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T30))
    {   /* IMP_Encoder_Get/SetChnCrop: exported by the T10/T20/T21 libimp without a header, so resolved with dlsym.  The window (multiples of 16) is the visible
         * rectangle of the stream (SPS frame cropping); the check reads the SPS size back from the stream */
        typedef struct { unsigned char enable, pad[3]; uint32_t x, y, w, h; } CropCfg;
        int (*cset)(int, const CropCfg *) = (int (*)(int, const CropCfg *))dlsym(dlopen(NULL, RTLD_NOW), "IMP_Encoder_SetChnCrop");
        int (*cget)(int, CropCfg *) = (int (*)(int, CropCfg *))dlsym(dlopen(NULL, RTLD_NOW), "IMP_Encoder_GetChnCrop");

        if (cset && cget && codec == CODEC_H264) {
            CropCfg *g = (CropCfg *)gnew(sizeof(CropCfg)), *g2 = (CropCfg *)gnew(sizeof(CropCfg)), *b1 = (CropCfg *)gnew(sizeof(CropCfg));
            SI si;
            int k, rs, rg, bad1, bad2, bad3, bad4, w0, h0, on_ok, off_ok, s_w, s_h;

            IMP_Encoder_RequestIDR(chn); pull(chn, codec, 40, 2, 0, &si, 3000); w0 = si.w; h0 = si.h;
            g->enable = 1; g->x = 16; g->y = 16; g->w = 576; g->h = 320;
            b1->enable = 1; b1->x = 8; b1->y = 0; b1->w = 576; b1->h = 320;
            bad1 = GATE("crop_x8", cset(chn, b1));                                              /* not a multiple of 16 */
            b1->x = 64; b1->w = 1600;
            bad2 = GATE("crop_outside", cset(chn, b1));                                              /* outside the picture */
            bad3 = GATE("crop_null", cset(chn, NULL));
            bad4 = GATE("crop_chn99", cset(99, g));                                                /* invalid channel */
            rs = GATE("crop_set", cset(chn, g));
            rg = GATE("crop_get", cget(chn, g2));
            for (k = 0; k < 6; k++) { IMP_Encoder_RequestIDR(chn); pull(chn, codec, 40, 2, 0, &si, 3000); if (si.w == 576 && si.h == 320) break; }
            s_w = si.w; s_h = si.h;
            if (getenv("APITEST_CROP_DUMP") && s_w == 576 && s_h == 320 && (g_dump = fopen(getenv("APITEST_CROP_DUMP"), "wb"))) {
                IMP_Encoder_RequestIDR(chn); pull(chn, codec, 45, 0, 0, &si, 3000);
                fclose(g_dump); g_dump = NULL;
            }
            on_ok = rs == 0 && rg == 0 && g2->enable && g2->x == 16 && g2->y == 16 && g2->w == 576 && g2->h == 320 && bad1 == -1 && bad2 == -1 && bad3 == -1 && bad4 == -1 && s_w == 576 && s_h == 320;
            memset(g, 0, sizeof(*g));
            rs = cset(chn, g);                                                 /* off */
            for (k = 0; k < 6; k++) { IMP_Encoder_RequestIDR(chn); pull(chn, codec, 40, 2, 0, &si, 3000); if (si.w == w0 && si.h == h0) break; }
            rg = cget(chn, g2);
            off_ok = rs == 0 && rg == 0 && !g2->enable && si.w == w0 && si.h == h0;
            rep(LBL("IMP_Encoder_SetChnCrop"), rs, (on_ok && off_ok) ? V_PASS : V_FAIL,
                "crop 16,16 576x320: read back, SPS visible size %dx%d (was %dx%d); x=8 -> %d, outside the picture -> %d, NULL -> %d, chn 99 -> %d; crop off: enable %d, SPS size %dx%d",
                s_w, s_h, w0, h0, bad1, bad2, bad3, bad4, g2->enable, si.w, si.h);
            rep(LBL("IMP_Encoder_GetChnCrop"), rg, (on_ok && off_ok) ? V_PASS : V_FAIL, "window read back while on (%u,%u %ux%u) and off (enable %d)", 16u, 16u, 576u, 320u, g2->enable);
            gfree(g); gfree(g2); gfree(b1);
        } else if (!cset || !cget) { rep_na(LBL("IMP_Encoder_SetChnCrop")); rep_na(LBL("IMP_Encoder_GetChnCrop")); }
    }
# endif
# if HAS_IMP_Encoder_SetChnInitQP
    NEED(IMP_Encoder_SetChnInitQP) { r = IMP_Encoder_SetChnInitQP(chn, 30); RET0(IMP_Encoder_SetChnInitQP, r, "qp 30 (no getter)"); }
# endif
# if HAS_IMP_Encoder_SetMultiSectionMode && !defined(PLATFORM_T23)
    rep(FN(IMP_Encoder_SetMultiSectionMode), 0, V_SKIP, "global slice mode of all channels");
# endif
#else
    (void)chn; (void)codec;
#endif
}

/* functions that exist on every family */
static void live_misc(int chn, int codec)
{
    SI si;
    int r;

    NEED(IMP_Encoder_GetMaxStreamCnt) {
        G(int, g);
        int was, r2;

        r = IMP_Encoder_GetMaxStreamCnt(chn, g); gchk(g);
        RET0(IMP_Encoder_GetMaxStreamCnt, r, "%d", *g);
        was = *g;
        NEED(IMP_Encoder_SetMaxStreamCnt) {
#ifdef ENC_NEW
            /* stock T31/T41: the buffer count is fixed when the channel is created; on a created
             * channel the call fails and the count stays */
            r2 = IMP_Encoder_SetMaxStreamCnt(chn, was > 3 ? 3 : was + 1);
            r = IMP_Encoder_GetMaxStreamCnt(chn, g); gchk(g);
            rep(FN(IMP_Encoder_SetMaxStreamCnt), r2, (r2 == -1 && r == 0 && *g == was) ? V_PASS : V_FAIL,
                "refused on a created channel (before CreateChn only): ret %d, count stays %d (was %d)", r2, *g, was);
#elif defined(APITEST_T32)
            /* vendor T33 2.0.2.1 and T32 (#856): a created channel refuses the call (0x80040080) and keeps its count; T33 default 2
             * (H.264/H.265; the T32 default is not checked) */
            r2 = IMP_Encoder_SetMaxStreamCnt(chn, was > 3 ? 3 : was + 1);
            r = IMP_Encoder_GetMaxStreamCnt(chn, g); gchk(g);
#  ifdef APITEST_T33
#   define AT_MSC_DEF_OK (codec == CODEC_JPEG || was == 2)
#  else
#   define AT_MSC_DEF_OK 1
#  endif
            rep(FN(IMP_Encoder_SetMaxStreamCnt), r2, (r2 == (int)0x80040080 && r == 0 && *g == was && AT_MSC_DEF_OK) ? V_PASS : V_FAIL,
                "refused on a created channel (before CreateChn only): ret 0x%x, count stays %d (was %d, vendor default 2 video / 1 JPEG)", (unsigned)r2, *g, was);
            {   /* channel 15 was never created: Set stores (0), Get stays 0x80040008; channel 16: 0x80040001 */
                int a1 = IMP_Encoder_SetMaxStreamCnt(15, 4), a2 = IMP_Encoder_GetMaxStreamCnt(15, g);
                int a3 = IMP_Encoder_SetMaxStreamCnt(16, 4), a4 = IMP_Encoder_GetMaxStreamCnt(16, g);

                gchk(g);
                IMP_Encoder_SetMaxStreamCnt(15, 0);
                rep(LBL("IMP_Encoder_SetMaxStreamCnt (not created)"), a1, (a1 == 0 && a2 == (int)0x80040008 && a3 == (int)0x80040001 && a4 == (int)0x80040001) ? V_PASS : V_FAIL,
                    "chn 15 set %d get 0x%x, chn 16 set 0x%x get 0x%x", a1, (unsigned)a2, (unsigned)a3, (unsigned)a4);
            }
#else
            r2 = IMP_Encoder_SetMaxStreamCnt(chn, was > 3 ? 3 : was + 1);
            r = IMP_Encoder_GetMaxStreamCnt(chn, g); gchk(g);
            CHECK(IMP_Encoder_SetMaxStreamCnt, r2, r == 0 && *g == (was > 3 ? 3 : was + 1), "set %d, read back %d", was > 3 ? 3 : was + 1, *g);
            IMP_Encoder_SetMaxStreamCnt(chn, was);
#endif
        }
        gfree(g);
    }
#if HAS_IMP_Encoder_SetFisheyeEnableStatus
    /* T20/T21 (disassembly of libimp 3.12.0 / 1.0.33), T23/T31: a creation-time option,
     * SetFisheyeEnableStatus on a created channel fails; on an idle channel it is a plain flag */
    if (codec != CODEC_JPEG) NEED(IMP_Encoder_SetFisheyeEnableStatus) {
        int r2 = IMP_Encoder_SetFisheyeEnableStatus(chn, 1);
        rep(FN(IMP_Encoder_SetFisheyeEnableStatus), r2, r2 == -1 ? V_PASS : V_FAIL, "stock refuses it on a created channel: ret %d", r2);
        NEED(IMP_Encoder_GetFisheyeEnableStatus) {
            int idle = 5, v1 = -1, v2 = -1, rs, rg, rs0;
            rs = IMP_Encoder_SetFisheyeEnableStatus(idle, 1);
            rg = IMP_Encoder_GetFisheyeEnableStatus(idle, &v1);
            rs0 = IMP_Encoder_SetFisheyeEnableStatus(idle, 0);
            IMP_Encoder_GetFisheyeEnableStatus(idle, &v2);
            rep(LBL("IMP_Encoder_SetFisheyeEnableStatus idle channel"), rs, (rs == 0 && rg == 0 && v1 == 1 && rs0 == 0 && v2 == 0) ? V_PASS : V_FAIL,
                "idle channel %d: set 1 -> get %d, set 0 -> get %d", idle, v1, v2);
        }
    }
#endif
    NEED(IMP_Encoder_FlushStream) {
        r = IMP_Encoder_FlushStream(chn);
        pull(chn, codec, 3, 0, 0, &si, 4000);       /* the IDR after the flush: 2 s was too tight for a loaded QEMU host (1 run in 5) */
        CHECK(IMP_Encoder_FlushStream, r, si.frames == 3, "%d frames after the flush", si.frames);
    }
#if HAS_IMP_Encoder_GetFd
    NEED(IMP_Encoder_GetFd) {
        int fd = IMP_Encoder_GetFd(chn);
        struct pollfd p;
        int pr = -1;

        if (fd >= 0) { p.fd = fd; p.events = POLLIN; p.revents = 0; pr = poll(&p, 1, 1000); }
        rep(FN(IMP_Encoder_GetFd), fd, fd >= 0 ? V_PASS : V_FAIL, "fd %d, poll(POLLIN, 1 s) = %d revents 0x%x", fd, pr, fd >= 0 ? p.revents : 0);
    }
#endif
#if HAS_IMP_Encoder_GetChnEncType
    NEED(IMP_Encoder_GetChnEncType) {
        G(int, g);
        int want;

# ifdef ENC_NEW
        want = codec == CODEC_H264 ? IMP_ENC_TYPE_AVC : codec == CODEC_H265 ? IMP_ENC_TYPE_HEVC : IMP_ENC_TYPE_JPEG;
# else
        want = codec == CODEC_H264 ? PT_H264 : codec == CODEC_H265 ? T_H265 : PT_JPEG;
# endif
        r = IMP_Encoder_GetChnEncType(chn, (void *)g); gchk(g);
        CHECK(IMP_Encoder_GetChnEncType, r, *g == want, "%s: type %d (expected %d)", cname[codec], *g, want);
        gfree(g);
    }
#endif
#if HAS_IMP_Encoder_GetPool
    NEED(IMP_Encoder_GetPool) { r = IMP_Encoder_GetPool(chn); rep(FN(IMP_Encoder_GetPool), r, r == ENC_POOL_ID ? V_PASS : V_FAIL, "chn %d pool %d (set %d)", chn, r, ENC_POOL_ID); }
#endif
#if HAS_IMP_Encoder_GetStreamBufSize
    NEED(IMP_Encoder_GetStreamBufSize) {
        G(uint32_t, g);
        r = IMP_Encoder_GetStreamBufSize(chn, g); gchk(g);
        CHECK(IMP_Encoder_GetStreamBufSize, r, *g == 262144, "%u (set 262144)", *g);
        gfree(g);
    }
#endif
#if defined(PLATFORM_T41) && !defined(APITEST_T40) && !HAS_IMP_Encoder_PollingModuleStream
/* the T41 1.2.6 header dropped the declaration, the 1.2.5 header and the library still have it (weak: N/A on a library without it) */
int IMP_Encoder_PollingModuleStream(uint32_t *encChnBitmap, uint32_t timeoutMsec);
# pragma weak IMP_Encoder_PollingModuleStream
# undef HAS_IMP_Encoder_PollingModuleStream
# define HAS_IMP_Encoder_PollingModuleStream 1
#endif
#if HAS_IMP_Encoder_PollingModuleStream
    NEED(IMP_Encoder_PollingModuleStream) {
        G(uint32_t, bm);
        *bm = 1u << chn;
        r = IMP_Encoder_PollingModuleStream(bm, 2000); gchk(bm);
        CHECK(IMP_Encoder_PollingModuleStream, r, (*bm & (1u << chn)) != 0, "bitmap in 0x%x out 0x%x", 1u << chn, *bm);
        if (r == 0 && (*bm & (1u << chn)) && IMP_Encoder_GetStream) {       /* the ready stream waits for its reader */
            G(IMPEncoderStream, s);
            if (IMP_Encoder_GetStream(chn, s, true) == 0) IMP_Encoder_ReleaseStream(chn, s);
            gfree(s);
        }
        gfree(bm);
    }
#endif
#if HAS_IMP_Encoder_GetChnAveBitrate || HAS_IMP_Encoder_GetChnEvalInfo
    {
        G(IMPEncoderStream, s);
        int r2 = IMP_Encoder_GetStream ? IMP_Encoder_GetStream(chn, s, true) : -1;

        gchk(s);
        if (r2 == 0) {
# if HAS_IMP_Encoder_GetChnAveBitrate
            NEED(IMP_Encoder_GetChnAveBitrate) {
                G(double, br);
                r = IMP_Encoder_GetChnAveBitrate(chn, s, 5, br); gchk(br);
                CHECK(IMP_Encoder_GetChnAveBitrate, r < 0 ? r : 0, r >= 0 && *br >= 0, "ret %d, average %.1f kbit/s over 5 frames (after GetStream)", r, *br);
                gfree(br);
            }
# endif
# if HAS_IMP_Encoder_GetChnEvalInfo
            NEED(IMP_Encoder_GetChnEvalInfo) {
                void *info = gnew(4096);
                int e;

                errno = 0;
                r = IMP_Encoder_GetChnEvalInfo(chn, info); e = errno;
                gchk(info);
                if (r < 0 && e == ENOTSUP) rep(FN(IMP_Encoder_GetChnEvalInfo), r, V_NA, "ENOTSUP: eval info not provided by this libimp");
# if defined(PLATFORM_T31)
                else {
                    /* OpenIMP record = IMPEncoderStreamInfo of the header (36 bytes): iNumBytes @0, six counters @4..27,
                     * int16 slice QP / min QP / max QP @28/30/32.  The vendor 1.1.6 record is shifted by 4 bytes (starts at
                     * uNumIntra, QPs @24/26/28, no iNumBytes): a vendor run fails the iNumBytes / QP-field checks here.
                     * JPEG (8 bytes): iNumBytes @0, int16 quality factor @4. */
                    const uint8_t *b = (const uint8_t *)info;
                    IMPEncoderStreamInfo si;
                    uint32_t au = 0; int i;

                    memcpy(&si, b, sizeof(si));
                    for (i = 0; i < (int)s->packCount; i++) au += s->pack[i].length;
                    if (codec == CODEC_JPEG) {
                        int16_t ql; uint8_t z = 0;
                        memcpy(&ql, b + 4, 2);
                        for (i = 6; i < 8; i++) z |= b[i];
                        CHECK(IMP_Encoder_GetChnEvalInfo, r < 0 ? r : 0, r == 0 && si.iNumBytes > 0 && ql >= 1 && ql <= 100 && !z,
                              "JPEG record (8 bytes): ret %d, iNumBytes %d (stream %u), quality factor %d, bytes 6..7 %s", r, si.iNumBytes, au, ql, z ? "NOT zero" : "zero");
                    } else if (codec == CODEC_H264) {
                        CHECK(IMP_Encoder_GetChnEvalInfo, r < 0 ? r : 0, r == 0 && gtouched(info) && si.iNumBytes > 0 && (uint32_t)si.iNumBytes <= au &&
                              si.iSliceQP >= 1 && si.iSliceQP <= 51 && si.iMinQP >= 0 && si.iMinQP <= si.iMaxQP && si.iMaxQP <= 51 &&
                              si.iMinQP <= si.iSliceQP && si.iSliceQP <= si.iMaxQP && !b[34] && !b[35],
                              "ret %d, header layout: iNumBytes %d (access unit %u), intra %u skip %u cu8 %u cu16 %u cu32 %u cu64 %u, slice QP %d, min QP %d, max QP %d",
                              r, si.iNumBytes, au, si.uNumIntra, si.uNumSkip, si.uNumCU8x8, si.uNumCU16x16, si.uNumCU32x32, si.uNumCU64x64,
                              si.iSliceQP, si.iMinQP, si.iMaxQP);
                    } else
                        CHECK(IMP_Encoder_GetChnEvalInfo, r < 0 ? r : 0, r == 0 && gtouched(info), "ret %d, 36 bytes written (this codec's counters are not filled): iNumBytes %d (access unit %u), counters %u %u %u %u %u %u, QP %d/%d/%d",
                              r, si.iNumBytes, au, si.uNumIntra, si.uNumSkip, si.uNumCU8x8, si.uNumCU16x16, si.uNumCU32x32, si.uNumCU64x64, si.iSliceQP, si.iMinQP, si.iMaxQP);
                }
# else
                else CHECK(IMP_Encoder_GetChnEvalInfo, r < 0 ? r : 0, r >= 0, "ret %d, info buffer %s (4 KB guarded)", r, gtouched(info) ? "written" : "untouched");
# endif
                gfree(info);
            }
# endif
            IMP_Encoder_ReleaseStream(chn, s);
        } else {
# if HAS_IMP_Encoder_GetChnAveBitrate
            rep(FN(IMP_Encoder_GetChnAveBitrate), r2, V_SKIP, "no stream to test with");
# endif
# if HAS_IMP_Encoder_GetChnEvalInfo
            rep(FN(IMP_Encoder_GetChnEvalInfo), r2, V_SKIP, "no stream to test with");
# endif
        }
        gfree(s);
    }
#endif
    (void)si;
}

/* ------------------------------------------------------------------ JPEG */
static int g_jpeg_dqt0 = -1;            /* luma DQT[0] of the last jpeg_avg() frame */
static unsigned long jpeg_avg(int chn, int n)
{
    SI si;

    g_jpeg_dqt0 = -1;
    /* one retry: a slow emulated target can starve the JPEG channel of frames for a moment */
    if ((pull(chn, CODEC_JPEG, n, 0, 0, &si, 2000) < 0 || si.frames < n || si.jpeg_ok < n) &&
        (pull(chn, CODEC_JPEG, n, 0, 0, &si, 2000) < 0 || si.frames < n || si.jpeg_ok < n)) return 0;
    g_jpeg_dqt0 = si.dqt0;
    return si.bytes / (unsigned)n;
}

/* The JPEG channel is fed through the group of the H.264 channel (the frame is lent by the
 * channel that dequeues it), as in the streamer, where the H.264 stream is always consumed.
 * Without a reader on the H.264 channel its stream queue fills, it stops taking frames and
 * the JPEG channel never gets one: drain it in the background while the JPEG test runs. */
static volatile int g_drain_run;
static void *drain_thread(void *arg)
{
    int chn = (int)(intptr_t)arg;
    IMPEncoderStream *s = calloc(1, sizeof(*s));

    while (s && g_drain_run) {
        if (IMP_Encoder_PollingStream(chn, 100) != 0) continue;
        memset(s, 0, sizeof(*s));
        if (IMP_Encoder_GetStream(chn, s, true) == 0) IMP_Encoder_ReleaseStream(chn, s);
    }
    free(s);
    return NULL;
}

static void test_jpeg_body(int jchn, int share_with);
static void test_jpeg(int jchn, int share_with)
{
    pthread_t drain;
    int draining = 0;
    int r;
    SI si;
    (void)si;
    if (IMP_Encoder_PollingStream) {
        g_drain_run = 1;
        draining = pthread_create(&drain, NULL, drain_thread, (void *)(intptr_t)0) == 0;
    }
    test_jpeg_body(jchn, share_with);
    if (draining) { g_drain_run = 0; pthread_join(drain, NULL); }
    (void)r;
}

static void test_jpeg_body(int jchn, int share_with)
{
    SI si;
    int r;

#if HAS_IMP_Encoder_SetbufshareChn
    NEED(IMP_Encoder_SetbufshareChn) {
        r = IMP_Encoder_SetbufshareChn(jchn, share_with);
        RET0(IMP_Encoder_SetbufshareChn, r, "JPEG chn %d shares the buffer of chn %d (before CreateChn)", jchn, share_with);
    }
#else
    (void)share_with;
#endif
    if (chn_up(jchn, CODEC_JPEG, 1, 1) != 0) return;
    test_query(jchn, 1, 1);
    pull(jchn, CODEC_JPEG, 6, 0, 1, &si, 2000);
    rep(LBL("JPEG stream marker check"), 0, (si.frames == 6 && si.soi == 6 && si.eoi == 6 && si.jpeg_ok == 6 && si.w == EW && si.h == EH) ? V_PASS : V_FAIL,
        "%d frames, SOI %d EOI %d, %dx%d (SOF), max %d bytes", si.frames, si.soi, si.eoi, si.w, si.h, si.bytes_max);

#if HAS_IMP_Encoder_SetJpegeQl
    NEED(IMP_Encoder_SetJpegeQl) {
        G(IMPEncoderJpegeQl, o); G(IMPEncoderJpegeQl, n); G(IMPEncoderJpegeQl, g);
        unsigned long hi, lo;
        int dqt_hi, dqt_lo;
        int r2, r3, i, n_t = (int)sizeof(n->qmem_table) < 128 ? (int)sizeof(n->qmem_table) : 128;

        r3 = IMP_Encoder_GetJpegeQl ? IMP_Encoder_GetJpegeQl(jchn, o) : -1; gchk(o);
        rep(FN(IMP_Encoder_GetJpegeQl), r3, r3 == 0 ? V_PASS : V_FAIL, "initial user_ql_en %d table[0] %u [64] %u", o->user_ql_en, o->qmem_table[0], o->qmem_table[64]);
        n->user_ql_en = true;
        for (i = 0; i < n_t; i++) n->qmem_table[i] = 2;            /* fine quantizer: big pictures */
        r2 = IMP_Encoder_SetJpegeQl(jchn, n);
        pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000);
        hi = jpeg_avg(jchn, 4);
        dqt_hi = g_jpeg_dqt0;
        r = IMP_Encoder_GetJpegeQl ? IMP_Encoder_GetJpegeQl(jchn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetJpegeQl, r2, r == 0 && g->user_ql_en && g->qmem_table[0] == 2 && g->qmem_table[64] == 2, "table all 2, read back en %d [0] %u [64] %u", g->user_ql_en, g->qmem_table[0], g->qmem_table[64]);
        for (i = 0; i < n_t; i++) n->qmem_table[i] = 90;           /* coarse quantizer: small pictures */
        r2 = IMP_Encoder_SetJpegeQl(jchn, n);
        pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000);
        lo = jpeg_avg(jchn, 4);
        dqt_lo = g_jpeg_dqt0;
        /* a textured scene: table 2 is clearly bigger.  A smooth scene (the QEMU ramp, whose pictures differ from frame
         * to frame) is mostly header and DC, so the sizes barely differ there: it passes when table 2 is still bigger and
         * the DQT of the stream follows the tables (2 then 90, the quantizers are applied to the stream). */
        rep(LBL("JPEG size follows SetJpegeQl live"), r2,
            (hi && lo && (hi > lo + lo / 3 || (hi > lo && dqt_hi == 2 && dqt_lo == 90))) ? V_PASS : V_FAIL,
            "table 2 -> avg %lu bytes (DQT[0] %d), table 90 -> avg %lu bytes (DQT[0] %d) (needs table 2 clearly bigger, or bigger with the DQT following)", hi, dqt_hi, lo, dqt_lo);
        IMP_Encoder_SetJpegeQl(jchn, o);
        gfree(o); gfree(n); gfree(g);
    }
#endif
#if HAS_IMP_Encoder_SetAvpuJpegQp
    NEED(IMP_Encoder_SetAvpuJpegQp) {
        unsigned long a, b;

        r = IMP_Encoder_SetAvpuJpegQp(jchn, 20); pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000); a = jpeg_avg(jchn, 4);
        IMP_Encoder_SetAvpuJpegQp(jchn, 90); pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000); b = jpeg_avg(jchn, 4);
        RET0(IMP_Encoder_SetAvpuJpegQp, r, "qp 20 -> avg %lu bytes, qp 90 -> avg %lu bytes", a, b);
    }
#endif
    live_misc(jchn, CODEC_JPEG);
    test_query(jchn, 1, 0);
    chn_down(jchn, 1);
    test_query(jchn, 0, 0);
}

/* ------------------------------------------- software JPEG / YUV encoder API */
static void test_swenc(void)
{
#if HAS_IMP_Encoder_VbmAlloc && !defined(APITEST_T32)
    size_t nv12 = (size_t)EW * EH * 3 / 2;
    unsigned char *src = NULL, *dst = NULL;
    unsigned char *snap = gnew(nv12);
    G(IMPFrameInfo, fi);
    int r;

    if (IMP_FrameSource_SnapFrame) IMP_FrameSource_SnapFrame(CH_SUB, PIX_FMT_NV12, EW, EH, snap, fi);
    NEED(IMP_Encoder_VbmAlloc) {
        src = IMP_Encoder_VbmAlloc((uint32_t)nv12, 64);
        dst = IMP_Encoder_VbmAlloc((uint32_t)nv12, 64);
        rep(FN(IMP_Encoder_VbmAlloc), src && dst ? 0 : -1, src && dst ? V_PASS : V_FAIL, "2 x %zu bytes, align 64: %p %p", nv12, (void *)src, (void *)dst);
    }
    if (src && dst) {
        memcpy(src, snap, nv12);
        NEED(IMP_Encoder_VbmV2P) NEED(IMP_Encoder_VbmP2V) {
            intptr_t p = IMP_Encoder_VbmV2P((intptr_t)src), v = IMP_Encoder_VbmP2V(p);
            rep(FN(IMP_Encoder_VbmV2P), 0, p != 0 ? V_PASS : V_FAIL, "virt %p -> phys 0x%lx", (void *)src, (unsigned long)p);
            rep(FN(IMP_Encoder_VbmP2V), 0, v == (intptr_t)src ? V_PASS : V_FAIL, "phys 0x%lx -> virt %p (expected %p)", (unsigned long)p, (void *)v, (void *)src);
        }
# if HAS_IMP_Encoder_InputJpege
        NEED(IMP_Encoder_InputJpege) {
            G(int, len);
            SI si;

            memset(&si, 0, sizeof(si)); si.first_idr_frame = -1;
            r = IMP_Encoder_InputJpege(src, dst, EW, EH, 75, len); gchk(len);
            if (r == 0 && *len > 4 && (size_t)*len <= nv12) scan_frame(dst, (size_t)*len, CODEC_JPEG, &si);
            CHECK(IMP_Encoder_InputJpege, r, si.jpeg_ok == 1 && si.w == EW && si.h == EH, "q 75: %d bytes, SOI %d EOI %d %dx%d", *len, si.soi, si.eoi, si.w, si.h);
            gfree(len);
        }
# endif
# if HAS_IMP_Encoder_YuvInit
        NEED(IMP_Encoder_YuvInit) {
            void **h = (void **)gnew(sizeof(void *));
            IMPEncoderYuvIn *in = (IMPEncoderYuvIn *)gnew(sizeof(IMPEncoderYuvIn));
            IMPEncoderYuvOut *out = (IMPEncoderYuvOut *)gnew(sizeof(IMPEncoderYuvOut));
            SI si;

            memset(&si, 0, sizeof(si)); si.first_idr_frame = -1;
#  ifdef PLATFORM_T41
            in->type = IMP_ENC_TYPE_AVC; in->mode = IMP_ENC_RC_MODE_CBR; in->frameRate = EFPS; in->gopLength = EGOP;
            in->targetBitrate = YUV_BITRATE; in->maxBitrate = YUV_BITRATE; in->initQp = -1; in->minQp = 20; in->maxQp = 45;
#  else
            in->type = PT_H264; in->mode.rcMode = ENC_RC_MODE_CBR;
            in->mode.attrH264Cbr.maxQp = 45; in->mode.attrH264Cbr.minQp = 15; in->mode.attrH264Cbr.outBitRate = BITRATE;
            in->mode.attrH264Cbr.frmQPStep = 3; in->mode.attrH264Cbr.gopQPStep = 15;
            in->outFrmRate.frmRateNum = EFPS; in->outFrmRate.frmRateDen = 1; in->maxGop = EGOP;
#  endif
            r = IMP_Encoder_YuvInit(h, EW, EH, in);
            gchk(h); gchk(in);
            RET0(IMP_Encoder_YuvInit, r, "%dx%d H.264 CBR", EW, EH);
            if (r == 0) {
                NEED(IMP_Encoder_YuvEncode) {
                    IMPFrameInfo f;
                    int r2;

                    memset(&f, 0, sizeof(f));
                    f.width = EW; f.height = EH; f.size = (uint32_t)nv12; f.pixfmt = PIX_FMT_NV12;
                    f.virAddr = (uint32_t)(uintptr_t)src; f.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)src);
                    out->outAddr = (void *)dst; out->outLen = (uint32_t)nv12;   /* caller's buffer and its capacity */
                    r2 = IMP_Encoder_YuvEncode(*h, f, out);
                    gchk(out);
                    if (r2 == 0 && out->outAddr && out->outLen > 4) scan_frame(out->outAddr, out->outLen, CODEC_H264, &si);
                    CHECK(IMP_Encoder_YuvEncode, r2, si.idr > 0 && si.sps > 0, "%u bytes, SPS %d PPS %d IDR %d, %dx%d", out->outLen, si.sps, si.pps, si.idr, si.w, si.h);
                }
#  if HAS_IMP_Encoder_YuvRequestIDR
                NEED(IMP_Encoder_YuvRequestIDR) { int r2 = IMP_Encoder_YuvRequestIDR(*h); RET0(IMP_Encoder_YuvRequestIDR, r2, ""); }
#  endif
#  if HAS_IMP_Encoder_YuvGetCrop
                NEED(IMP_Encoder_YuvGetCrop) {
                    G(IMPEncoderCropCfg, c);
                    int r2 = IMP_Encoder_YuvGetCrop(*h, c); gchk(c);
                    RET0(IMP_Encoder_YuvGetCrop, r2, "en %d %u,%u %ux%u", c->enable, c->x, c->y, c->w, c->h);
                    NEED(IMP_Encoder_YuvSetCrop) {
#  ifdef PLATFORM_T23
                        /* even values inside the picture, every margin below 510: a disabled 0x0 window is not valid */
                        c->enable = 1; c->x = 16; c->y = 16; c->w = EW - 32; c->h = EH - 32;
#  endif
                        int r3 = IMP_Encoder_YuvSetCrop(*h, c);
                        RET0(IMP_Encoder_YuvSetCrop, r3, "%s", c->enable ? "16 px margin" : "same value");
                    }
                    gfree(c);
                }
#  endif
                NEED(IMP_Encoder_YuvExit) { int r2 = IMP_Encoder_YuvExit(*h); RET0(IMP_Encoder_YuvExit, r2, ""); }
            }
            gfree(h); gfree(in); gfree(out);
        }
# endif
        NEED(IMP_Encoder_VbmFree) { IMP_Encoder_VbmFree(src); IMP_Encoder_VbmFree(dst); rep(FN(IMP_Encoder_VbmFree), 0, V_PASS, "2 buffers freed (void)"); }
    }
    gfree(snap); gfree(fi);
#endif
#if HAS_IMP_Encoder_VbmAlloc_Ex && defined(PLATFORM_T41) && !defined(APITEST_T32)
    /* T41 _Ex set: same flow as above through the second VPU instance's entry points (VbmAlloc_Ex -> InputJpege_Ex JPEG, YuvInit_Ex ->
     * YuvEncode_Ex H.264 IDR -> YuvExit_Ex, VbmFree_Ex); the frame is a snapshot of ch1 */
    {
        size_t nvx = (size_t)EW * EH * 3 / 2;
        unsigned char *sx = NULL, *dx = NULL;
        unsigned char *snapx = gnew(nvx);
        G(IMPFrameInfo, fix);
        int rx;

        if (IMP_FrameSource_SnapFrame) IMP_FrameSource_SnapFrame(CH_SUB, PIX_FMT_NV12, EW, EH, snapx, fix);
        NEED(IMP_Encoder_VbmAlloc_Ex) {
            sx = (unsigned char *)IMP_Encoder_VbmAlloc_Ex((uint32_t)nvx, 64);
            dx = (unsigned char *)IMP_Encoder_VbmAlloc_Ex((uint32_t)nvx, 64);
            rep(FN(IMP_Encoder_VbmAlloc_Ex), sx && dx ? 0 : -1, sx && dx ? V_PASS : V_FAIL, "2 x %zu bytes, align 64: %p %p", nvx, (void *)sx, (void *)dx);
        }
        if (sx && dx) {
            memcpy(sx, snapx, nvx);
            NEED(IMP_Encoder_InputJpege_Ex) {
                /* vendor _Ex doc: the height is a multiple of 16 (the 640x360 snapshot is cut to 640x352: Y rows 0..351, UV rows 0..175; 360 is refused) */
                const int jh = EH & ~15;
                size_t ysz = (size_t)EW * EH, ysz2 = (size_t)EW * jh;
                G(int, len);
                SI si;
                int rbad;

                memset(&si, 0, sizeof(si)); si.first_idr_frame = -1;
                memmove(sx + ysz2, sx + ysz, (size_t)EW * (jh / 2));
                rbad = IMP_Encoder_InputJpege_Ex(sx, dx, EW, EH, 75, len);
                rx = IMP_Encoder_InputJpege_Ex(sx, dx, EW, jh, 75, len); gchk(len);
                if (rx == 0 && *len > 4 && (size_t)*len <= nvx) scan_frame(dx, (size_t)*len, CODEC_JPEG, &si);
                CHECK(IMP_Encoder_InputJpege_Ex, rx, si.jpeg_ok == 1 && si.w == EW && si.h == jh && (EH % 16 == 0 || rbad != 0), "q 75: %d bytes, SOI %d EOI %d %dx%d; height %d (not a multiple of 16) -> %d", *len, si.soi, si.eoi, si.w, si.h, EH, rbad);
                gfree(len);
                memcpy(sx, snapx, nvx);
            }
            NEED(IMP_Encoder_YuvInit_Ex) {
                int *h = (int *)gnew(sizeof(int));
                IMPEncoderYuvIn *in = (IMPEncoderYuvIn *)gnew(sizeof(IMPEncoderYuvIn));
                IMPEncoderYuvOut *out = (IMPEncoderYuvOut *)gnew(sizeof(IMPEncoderYuvOut));
                SI si;

                memset(&si, 0, sizeof(si)); si.first_idr_frame = -1;
                in->type = IMP_ENC_TYPE_AVC; in->mode = IMP_ENC_RC_MODE_CBR; in->frameRate = EFPS; in->gopLength = EGOP;
                in->targetBitrate = YUV_BITRATE; in->maxBitrate = YUV_BITRATE; in->initQp = -1; in->minQp = 20; in->maxQp = 45;
                *h = -1;
                rx = IMP_Encoder_YuvInit_Ex(h, EW, EH, in);
                gchk(h); gchk(in);
                RET0(IMP_Encoder_YuvInit_Ex, rx, "%dx%d H.264 CBR, handle %d", EW, EH, *h);
                if (rx == 0) {
                    NEED(IMP_Encoder_YuvEncode_Ex) {
                        IMPFrameInfo f;
                        int r2;

                        memset(&f, 0, sizeof(f));
                        f.width = EW; f.height = EH; f.size = (uint32_t)nvx; f.pixfmt = PIX_FMT_NV12;
                        f.virAddr = (uint32_t)(uintptr_t)sx; f.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)sx);
                        out->outAddr = (void *)dx; out->outLen = (uint32_t)nvx;
                        r2 = IMP_Encoder_YuvEncode_Ex(*h, f, out);
                        gchk(out);
                        if (r2 == 0 && out->outAddr && out->outLen > 4) scan_frame(out->outAddr, out->outLen, CODEC_H264, &si);
                        CHECK(IMP_Encoder_YuvEncode_Ex, r2, si.idr > 0 && si.sps > 0, "%u bytes, SPS %d PPS %d IDR %d, %dx%d", out->outLen, si.sps, si.pps, si.idr, si.w, si.h);
                    }
                    NEED(IMP_Encoder_YuvExit_Ex) { int r2 = IMP_Encoder_YuvExit_Ex(*h); RET0(IMP_Encoder_YuvExit_Ex, r2, ""); }
                }
                gfree(h); gfree(in); gfree(out);
            }
            NEED(IMP_Encoder_VbmFree_Ex) { IMP_Encoder_VbmFree_Ex(sx); IMP_Encoder_VbmFree_Ex(dx); rep(FN(IMP_Encoder_VbmFree_Ex), 0, V_PASS, "2 buffers freed (void)"); }
        }
        gfree(snapx); gfree(fix);
    }
#elif HAS_IMP_Encoder_VbmAlloc_Ex
    rep(FN(IMP_Encoder_VbmAlloc_Ex), 0, V_SKIP, "_Ex variants (second VPU instance set) not exercised");
    rep(FN(IMP_Encoder_VbmFree_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_InputJpege_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvInit_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvEncode_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvExit_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
#endif
#if HAS_IMP_Encoder_SetAvpuBsShare && !defined(PLATFORM_T41)
    rep(FN(IMP_Encoder_SetAvpuBsShare), 0, V_SKIP, "global bitstream-buffer setup before the first channel");
    rep(FN(IMP_Encoder_SetIvpuBsSize), 0, V_SKIP, "global bitstream-buffer setup before the first channel");
#endif
}

/* ---------------------------------------------------------------- driver */
void t_enc(void)
{
    SI si;
    int r;

    rep_area("enc");
    /* group bound to FS ch1: H.264 chn 0 and JPEG chn 2 live in the same group (as the streamer does) */
    if (grp_up(0, 1) == 0) {
        if (chn_up(0, CODEC_H264, 1, 1) == 0) {
            test_query(0, 1, 1);
            pull(0, CODEC_H264, 60, 2, 1, &si, 3000);
            rep(LBL("H.264 stream NAL check"), 0, (si.sps && si.pps && si.idr && si.w == EW && si.h == EH) ? V_PASS : V_FAIL,
                "%d frames: SPS %d PPS %d IDR %d SEI %d other %d, SPS says %dx%d profile %d level %d%s", si.frames, si.sps, si.pps, si.idr, si.sei, si.other,
                si.w, si.h, si.profile, si.level, si.trunc ? " (a frame exceeded the 512 KB test buffer)" : "");
            live_common(0, CODEC_H264);
            live_old(0, CODEC_H264);
            live_misc(0, CODEC_H264);
            test_jpeg(2, 0);
            /* the H.264 channel keeps running after the JPEG channel is gone */
            pull(0, CODEC_H264, 5, 0, 0, &si, 3000);
            rep(LBL("H.264 stream after JPEG channel teardown"), 0, si.frames == 5 ? V_PASS : V_FAIL, "%d of 5 frames", si.frames);
            test_swenc();
            test_query(0, 1, 0);
            chn_down(0, 1);
            test_query(0, 0, 0);
        }
        grp_down(1);
    }
#if defined(PLATFORM_T41) && !defined(APITEST_T40) && HAS_IMP_Encoder_SetAvpuBsShare && HAS_IMP_Encoder_SetIvpuBsSize
    /* T41 global bitstream-buffer setup: SetIvpuBsSize(0) -> 0x80040002, after System_Init -> 0x80040040 (vendor 1.2.6); SetAvpuBsShare(on, size) before
     * CreateChn: the channel created while it is on takes the shared AVPU buffer and must still stream H.264; off after DestroyChn frees it (0) */
    {
        int r0 = IMP_Encoder_SetIvpuBsSize(0), r1 = IMP_Encoder_SetIvpuBsSize(262144), ron, roff = -1, up = 0;
        SI sb;

        memset(&sb, 0, sizeof(sb)); sb.first_idr_frame = -1;
        ron = IMP_Encoder_SetAvpuBsShare(true, 524288);
        if (ron == 0 && grp_up(0, 0) == 0) {
            if (chn_up(0, CODEC_H264, 1, 1) == 0) {
                up = 1;
                pull(0, CODEC_H264, 20, 0, 0, &sb, 3000);
                chn_down(0, 0);
            }
            grp_down(0);
        }
        roff = IMP_Encoder_SetAvpuBsShare(false, 0);
        rep(FN(IMP_Encoder_SetIvpuBsSize), r0, (r0 == (int)0x80040002u && r1 == (int)0x80040040u) ? V_PASS : V_FAIL,
            "size 0 -> 0x%x (0x80040002 expected), size 262144 after System_Init -> 0x%x (0x80040040 expected)", (unsigned)r0, (unsigned)r1);
        rep(FN(IMP_Encoder_SetAvpuBsShare), ron | roff, (ron == 0 && roff == 0 && up && sb.frames == 20 && sb.idr > 0 && sb.sps > 0) ? V_PASS : V_FAIL,
            "on (512 KB) -> %d, H.264 channel created while on: %d of 20 frames, SPS %d IDR %d; off -> %d", ron, sb.frames, sb.sps, sb.idr, roff);
    }
#endif
#ifdef HAVE_H265
# if defined(PLATFORM_T21) || defined(PLATFORM_T23)
    /* the Helix encoder of T21/T23 is H.264/JPEG only: the headers declare H.265 types, the silicon has no HEVC core */
    rep(LBL("H.265 stream"), -1, V_NA, "no HEVC encoder in the T21/T23 silicon (OpenIMP CreateChn refuses it so callers fall back to H.264)");
# else
# if defined(PLATFORM_T30)
    /* T30: the Radix encoder of OpenIMP is opt-in (OPENIMP_T30_H265=1) until its device proof */
    if (!getenv("OPENIMP_T30_H265")) {
        rep(LBL("H.265 stream"), -1, V_NA, "T30 Radix encoder not enabled (OPENIMP_T30_H265=1)");
    } else
# endif
    if (grp_up(0, 0) == 0) {
        if (chn_up(0, CODEC_H265, 1, 1) == 0) {
            /* APITEST_ENC_DUMP=<file>: the pulled H.265 stream (decode check off the camera) */
            if (getenv("APITEST_ENC_DUMP")) g_dump = fopen(getenv("APITEST_ENC_DUMP"), "wb");
            pull(0, CODEC_H265, getenv("APITEST_H265_FRAMES") ? atoi(getenv("APITEST_H265_FRAMES")) : 60,
                 getenv("APITEST_H265_FRAMES") ? 0 : 2, 0, &si, 3000);
            if (g_dump) { fclose(g_dump); g_dump = NULL; }
            rep(LBL("H.265 stream NAL check"), 0, (si.vps && si.sps && si.pps && si.idr) ? V_PASS : V_FAIL,
                "%d frames: VPS %d SPS %d PPS %d IDR %d SEI %d other %d", si.frames, si.vps, si.sps, si.pps, si.idr, si.sei, si.other);
            live_common(0, CODEC_H265);
            live_old(0, CODEC_H265);
            chn_down(0, 0);
        }
        grp_down(0);
    }
# endif
#endif
    (void)r;
}


/* ------------------------------------------------------------------------------------------
 * Explicit case "hskip" (APITEST_CASES=hskip, issues #3 #5 #7): H.264 chn 0 with IMP_Encoder_SetChnHSkip / SetChangeRef /
 * SetChnHSkipBlackEnhance from the environment; every picture is logged as "pic <n> <bytes> <nal type:ref_idc ...>" and the
 * whole stream is dumped for the host (ffmpeg -bsf:v trace_headers shows frame_num, ref list modification, MMCO).
 *   HSKIP_TYPE (0..6, default 0)  HSKIP_M  HSKIP_N  HSKIP_SAME (maxSameSceneCnt)  HSKIP_SCENECUT  HSKIP_BLACK  HSKIP_REF (ChangeRef)
 *   HSKIP_FLICKER=N (brightness jump at picture 3 and back at 13 of every N; HSKIP_FLICKPH shifts both, HSKIP_FLICKHI/LO are the two values, default 255/128)
 *   HSKIP_MAIN=1 (the encoder reads FrameSource ch0 at the sensor size, ch1 is not set up)  HSKIP_SMART=1 (SMART rate control, HSKIP_BITRATE = maximum kbit/s)  HSKIP_MAXTYPE (maxHSkipType at CreateChn, default 6)  HSKIP_RT=1 (set through IMP_Encoder_SetChnHSkip after the first IDR
 *   instead of at CreateChn)  HSKIP_GOP (default 30)  HSKIP_FRAMES (default 240)  HSKIP_DUMP=<file>  HSKIP_BITRATE (kbit/s)
 * ------------------------------------------------------------------------------------------ */
#if !defined(ENC_NEW) && HAS_IMP_Encoder_SetChnHSkip && !defined(APITEST_T32)
static int hs_env(const char *n, int d) { const char *v = getenv(n); return v && *v ? atoi(v) : d; }

static void hs_nals(const unsigned char *b, size_t n, char *out, size_t cap)
{
    size_t i, o = 0;

    out[0] = 0;
    for (i = 0; i + 4 < n; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && (b[i + 2] == 1 || (b[i + 2] == 0 && b[i + 3] == 1))) {
            size_t h = i + (b[i + 2] == 1 ? 3 : 4);

            if (h < n && o + 12 < cap) o += snprintf(out + o, cap - o, " %d:%d", b[h] & 31, (b[h] >> 5) & 3);
        }
    }
}

int enc_hskip_run(void)
{
    G(EAttr, a);
    int chn = 0, r, k, frames = hs_env("HSKIP_FRAMES", 240), gop = hs_env("HSKIP_GOP", 30), rt = hs_env("HSKIP_RT", 0);
    IMPEncoderAttrHSkip hs;
    FILE *dump = NULL;
    char nals[96];
    unsigned long total = 0;
    int flick = hs_env("HSKIP_FLICKER", 0);
    int fph = hs_env("HSKIP_FLICKPH", 0), fhi = hs_env("HSKIP_FLICKHI", 255), flo = hs_env("HSKIP_FLICKLO", 128);   /* phase (pictures) and the two brightness values of the flicker */
    int (*sbright)(unsigned char) = flick > 0 ? (int (*)(unsigned char))dlsym((void *)0, "IMP_ISP_Tuning_SetBrightness") : NULL;

    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
    fill_attr(a, CODEC_H264, gop);
    if (getenv("HSKIP_MAIN")) { a->encAttr.picWidth = g_sw; a->encAttr.picHeight = g_sh; }
    if (hs_env("HSKIP_BITRATE", 0)) a->rcAttr.attrRcMode.attrH264Cbr.outBitRate = hs_env("HSKIP_BITRATE", 0);
    if (hs_env("HSKIP_SMART", 0)) {                 /* HSKIP_SMART=1: SMART rate control (the OEM method 3), HSKIP_BITRATE = max kbit/s */
        IMPEncoderAttrH264Smart *sm = &a->rcAttr.attrRcMode.attrH264Smart;

        memset(&a->rcAttr.attrRcMode, 0, sizeof(a->rcAttr.attrRcMode));
        a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_SMART;
        sm->maxQp = 45; sm->minQp = 15; sm->staticTime = 2; sm->maxBitRate = hs_env("HSKIP_BITRATE", 512);
        sm->iBiasLvl = 0; sm->changePos = 80; sm->qualityLvl = 2; sm->frmQPStep = 3; sm->gopQPStep = 15; sm->gopRelation = false;
    }
    memset(&hs, 0, sizeof(hs));
    hs.skipType = (IMPSkipType)hs_env("HSKIP_TYPE", 0);
    hs.m = hs_env("HSKIP_M", gop - 1);
    hs.n = hs_env("HSKIP_N", 1);
    hs.maxSameSceneCnt = hs_env("HSKIP_SAME", 0);
    hs.bEnableScenecut = hs_env("HSKIP_SCENECUT", 0);
    hs.bBlackEnhance = hs_env("HSKIP_BLACK", 0);
    a->rcAttr.attrHSkip.maxHSkipType = (IMPSkipType)hs_env("HSKIP_MAXTYPE", IMP_Encoder_STYPE_H1M_TRUE);
    a->rcAttr.attrHSkip.hSkipAttr = rt ? a->rcAttr.attrHSkip.hSkipAttr : hs;
    r = IMP_Encoder_CreateChn(chn, a);
    printf("hskip: CreateChn %d (type %d m %d n %d same %d scenecut %d black %d, maxtype %d, rt %d, gop %d)\n", r, hs.skipType, hs.m, hs.n,
           hs.maxSameSceneCnt, hs.bEnableScenecut, hs.bBlackEnhance, a->rcAttr.attrHSkip.maxHSkipType, rt, gop);
    if (r) { gfree(a); grp_down(0); return -1; }
    r = IMP_Encoder_RegisterChn(GRP, chn);
    if (rt) {
        r = IMP_Encoder_SetChnHSkip(chn, &hs);
        printf("hskip: SetChnHSkip %d\n", r);
    }
    if (getenv("HSKIP_BLACK") && IMP_Encoder_SetChnHSkipBlackEnhance) printf("hskip: SetChnHSkipBlackEnhance %d\n", IMP_Encoder_SetChnHSkipBlackEnhance(chn, hs.bBlackEnhance));
    if (IMP_Encoder_GetChnHSkip) {      /* the read-back of the stored words (T30 #24: vendor vs open) */
        IMPEncoderAttrHSkip g;

        memset(&g, 0x5a, sizeof(g));
        r = IMP_Encoder_GetChnHSkip(chn, &g);
        printf("hskip: GetChnHSkip %d type %d m %d n %d same %d scenecut %d black %d\n", r, (int)g.skipType, g.m, g.n,
               g.maxSameSceneCnt, (int)g.bEnableScenecut, (int)g.bBlackEnhance);
    }
    if (getenv("HSKIP_REF") && IMP_Encoder_SetChangeRef) printf("hskip: SetChangeRef %d\n", IMP_Encoder_SetChangeRef(chn, hs_env("HSKIP_REF", 0)));
    r = IMP_Encoder_StartRecvPic(chn);
    printf("hskip: StartRecvPic %d\n", r);
    if (getenv("HSKIP_DUMP")) dump = fopen(getenv("HSKIP_DUMP"), "wb");
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < frames; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0;
        size_t n;

        if (flick > 0 && sbright && ((k + flick - fph % flick) % flick == 3 || (k + flick - fph % flick) % flick == 13)) {
            /* a scene change that is the same for both libraries (HSKIP_FLICKER=N, N > 13): the ISP brightness jumps to white at
             * picture 3 and back at picture 13 of every N; the change reaches the encoder about two pictures later, so the
             * large picture after the way back is picture 15 (a whole second at 15 fps, a base picture of the H1M types) */
            sbright((k + flick - fph % flick) % flick == 3 ? fhi : flo);
        }
        if (IMP_Encoder_PollingStream(chn, 3000) != 0) { gfree(s); printf("hskip: poll timeout at picture %d\n", k); break; }
        if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); printf("hskip: GetStream failed at picture %d\n", k); break; }
        gchk(s);
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (dump && !trunc) fwrite(g_sbuf, 1, n, dump);
        hs_nals(g_sbuf, n, nals, sizeof(nals));
        printf("pic %d %zu%s\n", k, n, nals);
        total += n;
        IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        gfree(s);
    }
    if (dump) fclose(dump);
    printf("hskip: %d pictures, %lu bytes\n", k, total);
    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    gfree(a);
    grp_down(0);
    return 0;
}
#else
int enc_hskip_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "demask" (APITEST_CASES=demask, issues #3 #5, T10/T20): H.264 chn 0 with IMP_Encoder_SetChnDemask.  The vendor
 * steps the ISP sharpness per picture from the QP of the previous picture; the per-picture ISP calls are recorded on the host
 * side (LD_PRELOAD tools/apitest/sctrl_trace.c, VIDIOC_S_CTRL id 0x98091b) and the picture QPs come from the stream dump
 * (tools/hskip_trace.py --qp).  Every picture is logged as "pic <n> <bytes>", attribute calls as "demask: ...".
 *   DEMASK_CNT (demaskCnt, default 3)  DEMASK_THR (demaskThresd, default 30)  DEMASK_QP (>0: FIXQP at that QP, else CBR)
 *   DEMASK_FRAMES (default 90)  DEMASK_RT=1 (enable through SetChnDemask after StartRecvPic instead of at CreateChn)
 *   DEMASK_SWITCH=<pic>:<cnt>:<thr> (or <pic>/<cnt>/<thr>; SetChnDemask again before picture <pic>)  DEMASK_OFF=<pic> (disable before picture <pic>)
 *   DEMASK_ON=<pic> (enable again before picture <pic>, DEMASK_CNT/THR)  DEMASK_SHARP (IMP_ISP_Tuning_SetSharpness before the
 *   stream: the application's own value the vendor restores)  DEMASK_GOP (default 30)  DEMASK_BITRATE (kbit/s)  DEMASK_DUMP=<file>
 *   DEMASK_OLDNAME=1 (call the 3.9.0 names IMP_Encoder_Set/GetChnH264Demask through dlsym instead; the same functions in OpenIMP)
 * ------------------------------------------------------------------------------------------ */
#if !defined(ENC_NEW) && HAS_IMP_Encoder_SetChnDemask
static int dm_env(const char *n, int d) { const char *v = getenv(n); return v && *v ? atoi(v) : d; }

static int dm_set(int chn, int en, int cnt, int thr, int pic)
{
    IMPEncoderAttrDemask d;
    int (*oldset)(int, const IMPEncoderAttrDemask *) = dm_env("DEMASK_OLDNAME", 0) ?
        (int (*)(int, const IMPEncoderAttrDemask *))dlsym((void *)0, "IMP_Encoder_SetChnH264Demask") : NULL;
    int r;

    memset(&d, 0, sizeof(d));
    d.enable = en != 0; d.isAutoMode = false; d.demaskCnt = cnt; d.demaskThresd = thr;
    r = oldset ? oldset(chn, &d) : IMP_Encoder_SetChnDemask(chn, &d);
    printf("demask: SetChnDemask en %d cnt %d thr %d before pic %d -> %d\n", en, cnt, thr, pic, r);
    return r;
}

int enc_demask_run(void)
{
    G(EAttr, a);
    int chn = 0, r, k, frames = dm_env("DEMASK_FRAMES", 90), gop = dm_env("DEMASK_GOP", 30), rt = dm_env("DEMASK_RT", 0);
    int cnt = dm_env("DEMASK_CNT", 3), thr = dm_env("DEMASK_THR", 30), fixqp = dm_env("DEMASK_QP", 0);
    int sw_pic = -1, sw_cnt = 0, sw_thr = 0, off_pic = dm_env("DEMASK_OFF", -1), on_pic = dm_env("DEMASK_ON", -1);
    int (*ssharp)(unsigned char) = (int (*)(unsigned char))dlsym((void *)0, "IMP_ISP_Tuning_SetSharpness");
    int (*gsharp)(unsigned char *) = (int (*)(unsigned char *))dlsym((void *)0, "IMP_ISP_Tuning_GetSharpness");
    FILE *dump = NULL;
    unsigned long total = 0;

    if (getenv("DEMASK_SWITCH") && sscanf(getenv("DEMASK_SWITCH"), "%d:%d:%d", &sw_pic, &sw_cnt, &sw_thr) != 3 &&
        sscanf(getenv("DEMASK_SWITCH"), "%d/%d/%d", &sw_pic, &sw_cnt, &sw_thr) != 3) sw_pic = -1;      /* "/" for qemu_run.py's case:ENV syntax */
    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
    if (getenv("DEMASK_SHARP") && ssharp) printf("demask: SetSharpness %d -> %d\n", dm_env("DEMASK_SHARP", 128), ssharp((unsigned char)dm_env("DEMASK_SHARP", 128)));
    fill_attr(a, CODEC_H264, gop);
    if (dm_env("DEMASK_BITRATE", 0)) a->rcAttr.attrRcMode.attrH264Cbr.outBitRate = dm_env("DEMASK_BITRATE", 0);
    if (fixqp > 0) {
        memset(&a->rcAttr.attrRcMode, 0, sizeof(a->rcAttr.attrRcMode));
        a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
        a->rcAttr.attrRcMode.attrH264FixQp.qp = fixqp;
    }
    if (!rt) {
        a->rcAttr.attrDemask.enable = true; a->rcAttr.attrDemask.isAutoMode = false;
        a->rcAttr.attrDemask.demaskCnt = cnt; a->rcAttr.attrDemask.demaskThresd = thr;
    }
    {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);        /* the ISP controls of this channel come after this time (sctrl_trace.c) */
        r = IMP_Encoder_CreateChn(chn, a);
        printf("demask: CreateChn %d (cnt %d thr %d rt %d fixqp %d gop %d frames %d) t=%llu\n", r, cnt, thr, rt, fixqp, gop, frames,
               (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull);
    }
    if (r) { gfree(a); grp_down(0); return -1; }
    r = IMP_Encoder_RegisterChn(GRP, chn);
    r = IMP_Encoder_StartRecvPic(chn);
    printf("demask: StartRecvPic %d\n", r);
    if (rt) dm_set(chn, 1, cnt, thr, 0);
    if (getenv("DEMASK_DUMP")) dump = fopen(getenv("DEMASK_DUMP"), "wb");
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < frames; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0;
        size_t n;

        if (k == sw_pic) dm_set(chn, 1, sw_cnt, sw_thr, k);
        if (k == off_pic) dm_set(chn, 0, cnt, thr, k);
        if (k == on_pic) dm_set(chn, 1, cnt, thr, k);
        if (IMP_Encoder_PollingStream(chn, 3000) != 0) { gfree(s); printf("demask: poll timeout at picture %d\n", k); break; }
        if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); printf("demask: GetStream failed at picture %d\n", k); break; }
        gchk(s);
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (dump && !trunc) fwrite(g_sbuf, 1, n, dump);
        printf("pic %d %zu\n", k, n);
        total += n;
        IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        gfree(s);
    }
    if (dump) fclose(dump);
    if (gsharp) { unsigned char v = 0; r = gsharp(&v); printf("demask: GetSharpness -> %d value %d\n", r, v); }
    {
        G(IMPEncoderAttrDemask, g);
        int (*oldget)(int, IMPEncoderAttrDemask *) = dm_env("DEMASK_OLDNAME", 0) ?
            (int (*)(int, IMPEncoderAttrDemask *))dlsym((void *)0, "IMP_Encoder_GetChnH264Demask") : NULL;

        r = oldget ? oldget(chn, g) : IMP_Encoder_GetChnDemask(chn, g); gchk(g);
        printf("demask: %s -> %d en %d auto %d cnt %d thr %d\n", oldget ? "GetChnH264Demask" : "GetChnDemask", r, g->enable, g->isAutoMode, g->demaskCnt, g->demaskThresd);
        gfree(g);
    }
    printf("demask: %d pictures, %lu bytes\n", k, total);
    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    gfree(a);
    grp_down(0);
    return 0;
}
#else
int enc_demask_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "fisheye" (APITEST_CASES=fisheye, issues #3 #5 #7, T10/T20/T21; #357 also T23/T30/T31/T40): H.264 chn 0 with IMP_Encoder_SetFisheyeEnableStatus
 * before CreateChn.  The vendor encoder (i264e_icodec_enc) clears the ROI table of every picture and codes the top-left 64x32
 * pixels at QP <= 15 on I slices and at whole seconds of the frame rate; the macroblock QPs are read from the stream dump on the
 * host (tools/fisheye_mbqp.c).  Every picture is logged as "pic <n> <bytes>".  On every family the vendor also rewrites the
 * top-left 64x16 source luma of each I picture (the watermark, src/watermark): decode the dump and run
 * tools/watermark_check.py on the I pictures.
 *   FISHEYE_ON (default 1)  FISHEYE_QP (FIXQP at that QP, default 30; 0: CBR)  FISHEYE_GOP (default 40)  FISHEYE_FRAMES (default 100)
 *   FISHEYE_H265=1 (T31/T40: an H.265 channel)
 *   FISHEYE_ROI=1 (an application ROI, delta QP -6 over most of the picture, set before the first picture)  FISHEYE_DUMP=<file>
 *   HSKIP_MAIN=1 (T21: the encoder reads FrameSource ch0 at the sensor size, ch1 is not set up)
 *   FISHEYE_LOSSTHD=<bytes> (T23: IMP_Encoder_Setframelossthd after CreateChn; the delivered pictures are logged with
 *   their stream seq, so a dropped picture shows as a seq gap; the IDR QP after the drop is read from the dump)
 * ------------------------------------------------------------------------------------------ */
#if HAS_IMP_Encoder_SetFisheyeEnableStatus
int enc_fisheye_run(void)
{
    G(EAttr, a);
    const char *e;
    int chn = 0, r, k, on = (e = getenv("FISHEYE_ON")) && *e ? atoi(e) : 1;
    int fixqp = (e = getenv("FISHEYE_QP")) && *e ? atoi(e) : 30;
    int gop = (e = getenv("FISHEYE_GOP")) && *e ? atoi(e) : 40;
    int frames = (e = getenv("FISHEYE_FRAMES")) && *e ? atoi(e) : 100;
    int roi = (e = getenv("FISHEYE_ROI")) && *e ? atoi(e) : 0, fe = -1;
    FILE *dump = NULL;
    unsigned long total = 0;

    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
#ifdef ENC_NEW
    /* FISHEYE_H265=1 (T31/T40, #402): the same case on an H.265 channel */
    fill_attr(a, (e = getenv("FISHEYE_H265")) && *e == '1' ? CODEC_H265 : CODEC_H264, gop);
#else
    fill_attr(a, CODEC_H264, gop);
#endif
#ifdef ENC_NEW
    /* T31/T40 (#357): the Allegro encoders mark the I pictures with the vendor luma watermark */
    if (fixqp > 0) {
        a->rcAttr.attrRcMode.rcMode = IMP_ENC_RC_MODE_FIXQP;
        a->rcAttr.attrRcMode.attrFixQp.iInitialQP = (int16_t)fixqp;
    }
    (void)roi;
#else
    if (getenv("HSKIP_MAIN")) { a->encAttr.picWidth = g_sw; a->encAttr.picHeight = g_sh; }   /* ch0 at the sensor size (T21: no scaled channel) */
    if (fixqp > 0) {
        memset(&a->rcAttr.attrRcMode, 0, sizeof(a->rcAttr.attrRcMode));
        a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
        a->rcAttr.attrRcMode.attrH264FixQp.qp = fixqp;
    }
#endif
    r = IMP_Encoder_SetFisheyeEnableStatus(chn, on);
    printf("fisheye: SetFisheyeEnableStatus %d before CreateChn -> %d\n", on, r);
    r = IMP_Encoder_CreateChn(chn, a);
    printf("fisheye: CreateChn %d (qp %d gop %d frames %d)\n", r, fixqp, gop, frames);
    if (r) { gfree(a); grp_down(0); return -1; }
    r = IMP_Encoder_SetFisheyeEnableStatus(chn, !on);
    printf("fisheye: SetFisheyeEnableStatus %d after CreateChn -> %d\n", !on, r);
    r = IMP_Encoder_GetFisheyeEnableStatus(chn, &fe);
    printf("fisheye: GetFisheyeEnableStatus -> %d value %d\n", r, fe);
#if HAS_IMP_Encoder_Setframelossthd
    if ((e = getenv("FISHEYE_LOSSTHD")) && *e) {
        uint32_t thd = 0;

        r = IMP_Encoder_Setframelossthd(chn, (uint32_t)strtoul(e, NULL, 0));
        printf("fisheye: Setframelossthd %s -> %d\n", e, r);
        r = IMP_Encoder_Getframelossthd(chn, &thd);
        printf("fisheye: Getframelossthd -> %d value %u\n", r, thd);
    }
#endif
    r = IMP_Encoder_RegisterChn(GRP, chn);
#if HAS_IMP_Encoder_SetChnROI && !defined(ENC_NEW)
    if (roi) {
        IMPEncoderROICfg c;

        memset(&c, 0, sizeof(c));
        c.u32Index = 0; c.bEnable = true; c.bRelatedQp = true; c.s32Qp = -6;
        c.rect.p0.x = 0; c.rect.p0.y = 0;
        c.rect.p1.x = (getenv("HSKIP_MAIN") ? g_sw : EW) - 1; c.rect.p1.y = (getenv("HSKIP_MAIN") ? g_sh : EH) - 1;
        r = IMP_Encoder_SetChnROI(chn, &c);
        printf("fisheye: SetChnROI (delta -6, whole picture) -> %d\n", r);
    }
#endif
    r = IMP_Encoder_StartRecvPic(chn);
    printf("fisheye: StartRecvPic %d\n", r);
    if (getenv("FISHEYE_DUMP")) dump = fopen(getenv("FISHEYE_DUMP"), "wb");
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < frames; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0;
        size_t n;

        if (IMP_Encoder_PollingStream(chn, 3000) != 0) { gfree(s); printf("fisheye: poll timeout at picture %d\n", k); break; }
        if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); printf("fisheye: GetStream failed at picture %d\n", k); break; }
        gchk(s);
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (dump && !trunc) fwrite(g_sbuf, 1, n, dump);
        printf("pic %d %zu seq %u\n", k, n, (unsigned)s->seq);
        total += n;
        IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        gfree(s);
    }
    if (dump) fclose(dump);
    printf("fisheye: %d pictures, %lu bytes\n", k, total);
    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    gfree(a);
    grp_down(0);
    return 0;
}
#else
int enc_fisheye_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "transcfg" (APITEST_CASES=transcfg, issues #3 #5 #7, legacy encoder API): IMP_Encoder_Set/GetH264TransCfg (chroma QP
 * index offset) on an H.264 chn 0, FIXQP.  Prints every return code and the value read back, before and after CreateChn and on a JPEG
 * channel, and dumps the stream; the host reads the PPS chroma_qp_index_offset (ffmpeg trace_headers) and the chroma statistics.
 *   TRANS_PRE=<n> (a Set before CreateChn)  TRANS_OFFSETS=a,b,c (Set at picture TRANS_AT, TRANS_AT + TRANS_STEP, ...; default 5 and -4)
 *   TRANS_NOPRE=1 (no Set before CreateChn: the T10/T20 vendor library reads an invalid handle there and dies)
 *   TRANS_QP (default 30)  TRANS_GOP (default 15)  TRANS_FRAMES (default 100)  TRANS_JPEG=1 (Set/Get on a JPEG channel 1)  TRANS_DUMP=<file>
 *   HSKIP_MAIN=1 (ch0 at the sensor size)
 * ------------------------------------------------------------------------------------------ */
#if !defined(ENC_NEW) && HAS_IMP_Encoder_SetH264TransCfg && !defined(APITEST_T32)
static void trans_set(int chn, int v, const char *when)
{
    IMPEncoderH264TransCfg c, o;
    int r, g;

    memset(&c, 0, sizeof(c)); memset(&o, 0x5a, sizeof(o));
    c.chroma_qp_index_offset = v;
    r = IMP_Encoder_SetH264TransCfg(chn, &c);
    g = IMP_Encoder_GetH264TransCfg(chn, &o);
    printf("transcfg: %s Set %d -> %d, Get -> %d value %d\n", when, v, r, g, (int)o.chroma_qp_index_offset);
}

int enc_transcfg_run(void)
{
    G(EAttr, a);
    const char *e;
    int chn = 0, r, k, fixqp = (e = getenv("TRANS_QP")) && *e ? atoi(e) : 30;
    int gop = (e = getenv("TRANS_GOP")) && *e ? atoi(e) : 15;
    int frames = (e = getenv("TRANS_FRAMES")) && *e ? atoi(e) : 100;
    int at = (e = getenv("TRANS_AT")) && *e ? atoi(e) : 5, step = (e = getenv("TRANS_STEP")) && *e ? atoi(e) : 15;
    int offs[16], noff = 0;
    FILE *dump = NULL;
    unsigned long total = 0;

    if ((e = getenv("TRANS_OFFSETS")) && *e) {
        const char *q = e;

        while (*q && noff < 16) { offs[noff++] = atoi(q); while (*q && *q != ',') q++; if (*q) q++; }
    } else { offs[0] = 5; offs[1] = -4; noff = 2; }
    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
    fill_attr(a, CODEC_H264, gop);
    if (getenv("HSKIP_MAIN")) { a->encAttr.picWidth = g_sw; a->encAttr.picHeight = g_sh; }
    memset(&a->rcAttr.attrRcMode, 0, sizeof(a->rcAttr.attrRcMode));
    a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
    a->rcAttr.attrRcMode.attrH264FixQp.qp = fixqp;
    if ((e = getenv("TRANS_PRE")) && *e) trans_set(chn, atoi(e), "before CreateChn");
    else if (!getenv("TRANS_NOPRE")) trans_set(chn, 0, "before CreateChn");
    r = IMP_Encoder_CreateChn(chn, a);
    printf("transcfg: CreateChn %d (qp %d gop %d frames %d)\n", r, fixqp, gop, frames);
    if (r) { gfree(a); grp_down(0); return -1; }
    {
        IMPEncoderH264TransCfg o;

        memset(&o, 0x5a, sizeof(o));
        r = IMP_Encoder_GetH264TransCfg(chn, &o);
        printf("transcfg: after CreateChn Get -> %d value %d\n", r, (int)o.chroma_qp_index_offset);
    }
    r = IMP_Encoder_RegisterChn(GRP, chn);
    r = IMP_Encoder_StartRecvPic(chn);
    printf("transcfg: StartRecvPic %d\n", r);
    if (getenv("TRANS_DUMP")) dump = fopen(getenv("TRANS_DUMP"), "wb");
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < frames; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0, i;
        size_t n;

        for (i = 0; i < noff; i++)
            if (k == at + i * step) { char w[32]; snprintf(w, sizeof(w), "picture %d", k); trans_set(chn, offs[i], w); }
        if (IMP_Encoder_PollingStream(chn, 3000) != 0) { gfree(s); printf("transcfg: poll timeout at picture %d\n", k); break; }
        if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); printf("transcfg: GetStream failed at picture %d\n", k); break; }
        gchk(s);
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (dump && !trunc) fwrite(g_sbuf, 1, n, dump);
        printf("pic %d %zu\n", k, n);
        total += n;
        IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        gfree(s);
        if (k % 10 == 9) {   /* what Get reports while the stream runs (the T10/T20 vendor Get never shows the offset) */
            IMPEncoderH264TransCfg o;
            int g;

            memset(&o, 0x5a, sizeof(o));
            g = IMP_Encoder_GetH264TransCfg(chn, &o);
#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
            printf("transcfg: picture %d Get -> %d chroma %d\n", k, g, (int)o.chroma_qp_index_offset);
#else
            printf("transcfg: picture %d Get -> %d chroma %d intra %u inter %u scaling %d\n", k, g, (int)o.chroma_qp_index_offset,
                   (unsigned)o.intraTransMode, (unsigned)o.interTransMode, (int)o.bScalingListValid);
#endif
        }
    }
    if (dump) fclose(dump);
    printf("transcfg: %d pictures, %lu bytes\n", k, total);
    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    if (getenv("TRANS_JPEG")) {
        G(EAttr, j);
        IMPEncoderH264TransCfg o;

        fill_attr(j, CODEC_JPEG, 0);
        if (getenv("HSKIP_MAIN")) { j->encAttr.picWidth = g_sw; j->encAttr.picHeight = g_sh; }
        trans_set(1, 3, "JPEG channel before CreateChn");
        r = IMP_Encoder_CreateChn(1, j);
        printf("transcfg: JPEG CreateChn %d\n", r);
        if (r == 0) { trans_set(1, 3, "JPEG channel"); IMP_Encoder_DestroyChn(1); }
        trans_set(chn, 3, "destroyed H.264 channel");
        trans_set(6, 3, "channel 6");
        printf("transcfg: Set NULL -> %d, Get NULL -> %d, Set ch -1 -> %d\n", IMP_Encoder_SetH264TransCfg(chn, NULL),
               IMP_Encoder_GetH264TransCfg(chn, NULL), IMP_Encoder_SetH264TransCfg(-1, &o));
        gfree(j);
    }
    gfree(a);
    grp_down(0);
    return 0;
}
#else
int enc_transcfg_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "rctrig" (APITEST_CASES=rctrig, issues #3 #5, T10/T20): H.264 CBR chn 0 on a starved bit rate with
 * IMP_Encoder_SetChnRcTrigLevel(chn, level, mode).  The vendor controller lifts the QP cap from the application's max QP to
 * "mode" (default 51) while a GOP uses more than level (default 3.0) times its budget; the picture QPs and sizes are compared
 * between the vendor library and OpenIMP on the host (tools/fisheye_mbqp.py).  Every picture is logged as "pic <n> <bytes>".
 *   RCTRIG_LEVEL (float; unset: SetChnRcTrigLevel is not called)  RCTRIG_MODE (new max QP, default 51)
 *   RCTRIG_SMART=1 (SMART instead of CBR, RCTRIG_BITRATE = max kbit/s)  RCTRIG_BITRATE (kbit/s, default 20)  RCTRIG_MAXQP (default 40)  RCTRIG_MINQP (default 15)  RCTRIG_GOP (default 30)
 *   RCTRIG_FRAMES (default 120)  RCTRIG_AT (>=0: call SetChnRcTrigLevel before that picture instead of before the first one)
 *   RCTRIG_DUMP=<file>
 * ------------------------------------------------------------------------------------------ */
#if !defined(ENC_NEW) && defined(PLATFORM_T20) || !defined(ENC_NEW) && defined(PLATFORM_T10)
extern int IMP_Encoder_SetChnRcTrigLevel(int chn, uint32_t level, int mode);

static int rt_set(int chn, float lv, int mode, int pic)
{
    union { float f; uint32_t u; } u;
    int r;

    u.f = lv;
    r = IMP_Encoder_SetChnRcTrigLevel(chn, u.u, mode);
    printf("rctrig: SetChnRcTrigLevel %.3f mode %d before pic %d -> %d\n", lv, mode, pic, r);
    return r;
}

int enc_rctrig_run(void)
{
    G(EAttr, a);
    const char *e;
    int chn = 0, r, k, mode = (e = getenv("RCTRIG_MODE")) && *e ? atoi(e) : 51;
    int br = (e = getenv("RCTRIG_BITRATE")) && *e ? atoi(e) : 20;
    int maxqp = (e = getenv("RCTRIG_MAXQP")) && *e ? atoi(e) : 40;
    int minqp = (e = getenv("RCTRIG_MINQP")) && *e ? atoi(e) : 15;
    int gop = (e = getenv("RCTRIG_GOP")) && *e ? atoi(e) : 30;
    int frames = (e = getenv("RCTRIG_FRAMES")) && *e ? atoi(e) : 120;
    int at = (e = getenv("RCTRIG_AT")) && *e ? atoi(e) : -1;
    int have = (e = getenv("RCTRIG_LEVEL")) && *e;
    float lv = have ? (float)atof(e) : 0.0f;
    FILE *dump = NULL;
    unsigned long total = 0;

    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
    fill_attr(a, CODEC_H264, gop);
    a->rcAttr.attrRcMode.attrH264Cbr.outBitRate = br;
    a->rcAttr.attrRcMode.attrH264Cbr.maxQp = maxqp;
    a->rcAttr.attrRcMode.attrH264Cbr.minQp = minqp;
    if ((e = getenv("RCTRIG_SMART")) && atoi(e)) {      /* RCTRIG_SMART=1: SMART (OEM method 3), RCTRIG_BITRATE = max kbit/s */
        IMPEncoderAttrH264Smart *sm = &a->rcAttr.attrRcMode.attrH264Smart;

        memset(&a->rcAttr.attrRcMode, 0, sizeof(a->rcAttr.attrRcMode));
        a->rcAttr.attrRcMode.rcMode = ENC_RC_MODE_SMART;
        sm->maxQp = maxqp; sm->minQp = minqp; sm->staticTime = 2; sm->maxBitRate = br;
        sm->iBiasLvl = 0; sm->changePos = 80; sm->qualityLvl = 2; sm->frmQPStep = 3; sm->gopQPStep = 15; sm->gopRelation = false;
    }
    r = IMP_Encoder_CreateChn(chn, a);
    printf("rctrig: CreateChn %d (bitrate %d maxqp %d minqp %d gop %d frames %d)\n", r, br, maxqp, minqp, gop, frames);
    if (r) { gfree(a); grp_down(0); return -1; }
    r = IMP_Encoder_RegisterChn(GRP, chn);
    if (have && at < 0) rt_set(chn, lv, mode, 0);
    r = IMP_Encoder_StartRecvPic(chn);
    printf("rctrig: StartRecvPic %d\n", r);
    if (getenv("RCTRIG_DUMP")) dump = fopen(getenv("RCTRIG_DUMP"), "wb");
    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < frames; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0;
        size_t n;

        if (have && k == at) rt_set(chn, lv, mode, k);
        if (IMP_Encoder_PollingStream(chn, 3000) != 0) { gfree(s); printf("rctrig: poll timeout at picture %d\n", k); break; }
        if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); printf("rctrig: GetStream failed at picture %d\n", k); break; }
        gchk(s);
        n = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (dump && !trunc) fwrite(g_sbuf, 1, n, dump);
        printf("pic %d %zu\n", k, n);
        total += n;
        IMP_Encoder_ReleaseStream(chn, s);
        gchk(s);
        gfree(s);
    }
    if (dump) fclose(dump);
    printf("rctrig: %d pictures, %lu bytes\n", k, total);
    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    gfree(a);
    grp_down(0);
    return 0;
}
#else
int enc_rctrig_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "gopfps" (APITEST_CASES=gopfps, #367, T10/T20/T21): the vendor IMP glue latches IMP_Encoder_SetChnFrmRate /
 * SetGOPSize, rounds the GOP to the rate, forces an IDR on the next picture and GetGOPSize / GetChnFrmRate answer the running
 * values.  Fixed scenarios (source 15 fps); output per scenario "SC <name> <num>/<den> gop <g>", "E <pic> r <n> <d> ret",
 * "E <pic> g <gop> ret", "G <pic> <why> ret gop", "R <pic> <why> ret n/d", "I <picture indexes of IDR pictures>" (host: tools/gop_t2x.py).
 * ------------------------------------------------------------------------------------------ */
#if !defined(ENC_NEW) && HAS_IMP_Encoder_SetGOPSize && HAS_IMP_Encoder_GetGOPSize && \
    (defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T10))
typedef struct { int pic; char kind; int a, b; } GfEv;
typedef struct { const char *name; int num, den, gop, pics; GfEv ev[4]; } GfSc;

static int gf_idr(const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i + 4 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int t = p[i + 3] & 0x1f;

            if (t == 5) return 1;
            if (t >= 1 && t <= 5) return 0;
        }
    return 0;
}

static void gf_get(int chn, int pic, const char *why)
{
    IMPEncoderGOPSizeCfg g;
    IMPEncoderFrmRate f;
    int r, r2;

    g.gopsize = -7; f.frmRateNum = f.frmRateDen = 0xffff;
    r = IMP_Encoder_GetGOPSize(chn, &g);
    r2 = IMP_Encoder_GetChnFrmRate(chn, &f);
    printf("G %d %s %d %d\nR %d %s %d %u/%u\n", pic, why, r, g.gopsize, pic, why, r2, f.frmRateNum, f.frmRateDen);
}

int enc_gopfps_run(void)
{
    static const GfSc sc[] = {
        { "create",  15, 1, 20,  70, { { -1 } } },                                    /* GOP 20 at 15/1: CreateChn rounds up */
        { "fps",     15, 1, 30, 100, { { 20, 'r', 5, 1 }, { 60, 'r', 15, 1 } } },     /* rate change: GOP rescaled, IDR forced */
        { "gop",     15, 1, 30, 150, { { 15, 'g', 20, 0 }, { 70, 'g', 50, 0 } } },    /* GOP asked for: rounded to the rate */
        { "same",    15, 1, 20,  70, { { 10, 'g', 20, 0 } } },                         /* the GOP the channel asked for already */
        { "half",    15, 2, 40,  80, { { -1 } } },                                    /* 15/2: T20 rounds to 30, T21 to 15 */
        { "both",    15, 1, 30,  60, { { 10, 'r', 5, 1 }, { 10, 'g', 20, 0 } } },
    };
    unsigned int si;
    int chn = 0;
    unsigned char *buf = malloc(STREAMCAP);

    if (!buf) return -1;
    for (si = 0; si < sizeof(sc) / sizeof(sc[0]); si++) {
        const GfSc *c = &sc[si];
        G(EAttr, a);
        int r, k, e, idrs[400], nidr = 0;

        if (grp_up(0, 0) != 0) { gfree(a); break; }
        fill_attr(a, CODEC_H264, c->gop);
        a->rcAttr.outFrmRate.frmRateNum = c->num; a->rcAttr.outFrmRate.frmRateDen = c->den;
        a->rcAttr.maxGop = c->gop;
        r = IMP_Encoder_CreateChn(chn, a);
        printf("SC %s %d/%d gop %d CreateChn %d\n", c->name, c->num, c->den, c->gop, r);
        if (r) { gfree(a); grp_down(0); continue; }
        IMP_Encoder_RegisterChn(GRP, chn);
        gf_get(chn, -1, "create");
        IMP_Encoder_StartRecvPic(chn);
        for (k = 0; k < c->pics; k++) {
            G(IMPEncoderStream, s);
            int trunc = 0;
            size_t n;

            for (e = 0; e < 4; e++) {
                if (c->ev[e].pic != k || c->ev[e].kind == 0) continue;
                if (c->ev[e].kind == 'r') {
                    IMPEncoderFrmRate fr = { (uint32_t)c->ev[e].a, (uint32_t)c->ev[e].b };

                    printf("E %d r %d %d %d\n", k, c->ev[e].a, c->ev[e].b, IMP_Encoder_SetChnFrmRate(chn, &fr));
                } else {
                    IMPEncoderGOPSizeCfg g = { c->ev[e].a };

                    printf("E %d g %d %d\n", k, c->ev[e].a, IMP_Encoder_SetGOPSize(chn, &g));
                }
                gf_get(chn, k, "after-event");
            }
            if (IMP_Encoder_PollingStream(chn, 4000) != 0) { gfree(s); printf("poll timeout at picture %d\n", k); break; }
            if (IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); break; }
            gchk(s);
            n = gather(s, buf, STREAMCAP, &trunc);
            if (gf_idr(buf, n) && nidr < 400) idrs[nidr++] = k;
            IMP_Encoder_ReleaseStream(chn, s);
            gfree(s);
            if (k % 10 == 9 || k == 0) gf_get(chn, k, "later");
        }
        printf("I");
        for (e = 0; e < nidr; e++) printf(" %d", idrs[e]);
        printf("\n");
        fflush(stdout);
        IMP_Encoder_StopRecvPic(chn);
        IMP_Encoder_UnRegisterChn(chn);
        IMP_Encoder_DestroyChn(chn);
        gfree(a);
        grp_down(0);
    }
    free(buf);
    return 0;
}
#else
int enc_gopfps_run(void) { return -1; }
#endif

int enc_chain_up(void)          /* H.264 chn 0 + group 0 with an OSD group in between (used by the OSD test) */
{
    if (grp_up(1, 0) != 0) return -1;
    if (chn_up(0, CODEC_H264, 0, 1) != 0) { grp_down(0); return -1; }
    return 0;
}

void enc_chain_down(void)
{
    chn_down(0, 0);
    grp_down(0);
}

#if defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) || defined(PLATFORM_T31) || (defined(PLATFORM_T41) && !defined(APITEST_T40))
/* IMP_OSD_AttachToGroup (deprecated vendor call): FS -> ENC is bound, the call inserts the OSD group between them
 * (src->to becomes src->from->to); the encoder then runs through the OSD group.  Returns 0 when the call was exercised. */
int enc_attach_osd(void)
{
    IMPCell fs = { DEV_ID_FS, enc_fs_ch(), 0 }, enc = { DEV_ID_ENC, GRP, 0 }, osd = { DEV_ID_OSD, GRP, 0 };
    int r, ok1 = 0, ok2 = 0, frames;
    SI si;

    if (!IMP_OSD_AttachToGroup || !IMP_OSD_CreateGroup) return -1;
    if (grp_up(0, 0) != 0) { rep(FN(IMP_OSD_AttachToGroup), -1, V_FAIL, "FS -> ENC chain could not be built"); return 0; }
    r = IMP_OSD_CreateGroup(GRP);
    if (r) { rep(FN(IMP_OSD_AttachToGroup), r, V_FAIL, "OSD CreateGroup %d failed", GRP); grp_down(0); return 0; }
    r = IMP_OSD_AttachToGroup(&osd, &enc);
    g_osd = (r == 0);                    /* grp_down then unbinds FS -> OSD -> ENC; a failed attach leaves FS -> ENC bound */
    if (r == 0) {
        G(IMPCell, a); G(IMPCell, b);
        ok1 = IMP_System_GetBindbyDest(&enc, a) == 0 && a->deviceID == DEV_ID_OSD && a->groupID == GRP;
        ok2 = IMP_System_GetBindbyDest(&osd, b) == 0 && b->deviceID == DEV_ID_FS && b->groupID == enc_fs_ch();
        gfree(a); gfree(b);
        frames = 0;
        if (chn_up(0, CODEC_H264, 0, 1) == 0) { pull(0, CODEC_H264, 5, 0, 0, &si, 3000); frames = si.frames; chn_down(0, 0); }
        rep(FN(IMP_OSD_AttachToGroup), r, (ok1 && ok2 && frames > 0) ? V_PASS : V_FAIL,
            "FS -> ENC became FS -> OSD -> ENC (ENC<-OSD %d, OSD<-FS %d), %d frames through the OSD group", ok1, ok2, frames);
    } else {
        rep(FN(IMP_OSD_AttachToGroup), r, V_FAIL, "attach of OSD group %d to ENC group %d refused (%d)", GRP, GRP, r);
    }
    grp_down(0);
    if (r) IMP_OSD_DestroyGroup(GRP);
    (void)fs;
    return 0;
}
#else
int enc_attach_osd(void) { return -1; }
#endif

int enc_pull_h264(int n)        /* frames still flowing? */
{
    SI si;

    pull(0, CODEC_H264, n, 0, 0, &si, 3000);
    return si.frames;
}

/* channel helpers of case t33e1 (at_t33.c): FS ch1 -> group 0 -> channel @chn (codec 0 H.264, 1 H.265) started; frames to a callback */
int enc_e1_up_ud(int chn, int codec, int ud_cnt, int ud_size)
{
    G(EAttr, a);
    int r;

    if (grp_up(0, 0) != 0) { gfree(a); return -1; }
    fill_attr(a, codec, EGOP);                  /* no SetPool: the encoder takes its buffers from rmem as a streamer does */
#if defined(APITEST_T33)
    a->encAttr.userData.maxUserDataCnt = (uint32_t)ud_cnt; a->encAttr.userData.maxUserDataSize = (uint32_t)ud_size;
#else
    (void)ud_cnt; (void)ud_size;
#endif
    r = IMP_Encoder_CreateChn(chn, a);
    if (!r) r = IMP_Encoder_RegisterChn(GRP, chn);
    if (!r) r = IMP_Encoder_StartRecvPic(chn);
    gfree(a);
    if (r) { rep("IMP_Encoder_CreateChn", r, V_FAIL, "t33e1: %s chn %d up 0x%x", cname[codec], chn, (unsigned)r); IMP_Encoder_DestroyChn(chn); grp_down(0); return -1; }
    return 0;
}

int enc_e1_up(int chn, int codec) { return enc_e1_up_ud(chn, codec, 0, 0); }

void enc_e1_down(int chn)
{
    chn_down(chn, 0);
    grp_down(0);
}

/* up to @n frames: @cb(frame bytes, length, frame index, arg) for each; returns the frames seen */
int enc_e1_pull(int chn, int n, void (*cb)(const unsigned char *, size_t, int, void *), void *arg)
{
    int k;

    if (!g_sbuf) g_sbuf = malloc(STREAMCAP);
    for (k = 0; k < n; k++) {
        G(IMPEncoderStream, s);
        int trunc = 0;
        size_t len;

        if (IMP_Encoder_PollingStream(chn, 3000) != 0 || IMP_Encoder_GetStream(chn, s, true) != 0) { gfree(s); break; }
        len = gather(s, g_sbuf, STREAMCAP, &trunc);
        if (cb) cb(g_sbuf, len, k, arg);
        IMP_Encoder_ReleaseStream(chn, s);
        gfree(s);
    }
    return k;
}

/* ------------------------------------------------------------------------------------------
 * Explicit case "push" (APITEST_CASES=push, #916 #917 #918, T10/T20, #932 T21/T30/T31): the push model of the encoder next to the vendor.
 * chn 0 (group 0, FS ch1 640x360) and with PUSH_2CH=1 (default) chn 1 (group 1, FS ch0 sensor size), H.264 at PUSH_FPS
 * (default the sensor rate), one drain thread per channel for PUSH_SEC (30) s: pictures and capture-timestamp gaps
 * > 1.5 output intervals; PUSH_GF=1: a thread calls IMP_FrameSource_GetFrame(1) as fast as frames come (after
 * SetFrameDepth(1, PUSH_DEPTH=1)); PUSH_HOLD_MS: the drain of chn 0 holds each stream that long before ReleaseStream
 * (#917, a slow streamer); then PUSH_CYCLES (20) StopRecvPic / StartRecvPic rounds of both channels, each must give
 * 5 pictures within 2 s.  Lines "push: ..." carry the numbers.  #962 (T23 too): "push: chn N dts ..." is the
 * histogram of the pack timestamp steps in 10 ms bins (the keep pattern of an output rate below the sensor rate). */
#if defined(PLATFORM_T20) || defined(PLATFORM_T10) || defined(PLATFORM_T21) || defined(PLATFORM_T30) || \
    defined(PLATFORM_T31) || defined(PLATFORM_T23)
#include <pthread.h>
static int push_env(const char *n, int d) { const char *v = getenv(n); return v && *v ? atoi(v) : d; }
typedef struct { int chn; volatile int run; int pics, gaps, maxgap_ms, hold_ms; int64_t last_ts; int interval_us;
                 int dts[41], dts_neg; FILE *dump; unsigned char *buf;
                 int64_t first_ts, t_start, t_up, early_ts[4], early_age[4]; int ngap, gap_pic[4], gap_ms[4], gap_at_ms[4], gap_wall_ms[4]; } PushDrain;
static volatile int g_push_gf_run;
static int g_push_gf_ok, g_push_gf_fail;

static void *push_drain(void *arg)
{
    PushDrain *d = arg;

    while (d->run) {
        IMPEncoderStream s;

        if (IMP_Encoder_PollingStream(d->chn, 500) != 0) continue;
        memset(&s, 0, sizeof(s));
        if (IMP_Encoder_GetStream(d->chn, &s, 1) != 0) continue;
        if (d->dump && d->buf) {     /* PUSH_DUMP=<dir>: chn 0 stream to <dir>/push0.h264 (#962 picture check) */
            int trunc = 0;
            size_t len = gather(&s, d->buf, STREAMCAP, &trunc);

            if (!trunc) fwrite(d->buf, 1, len, d->dump);
        }
        if (s.packCount > 0 && s.pack) {
            int64_t ts = s.pack[0].timestamp;

            if (!d->first_ts) d->first_ts = ts;
            if (d->pics < 4) {     /* #974: capture time of the first pictures vs StartRecvPic, and their age at GetStream */
                d->early_ts[d->pics] = ts;
                d->early_age[d->pics] = (int64_t)IMP_System_GetTimeStamp() - ts;
            }
            if (d->last_ts) {
                int64_t st = ts - d->last_ts;

                if (st <= 0) d->dts_neg++;
                else d->dts[st >= 405000 ? 40 : (int)((st + 5000) / 10000)]++;
            }
            if (d->last_ts && ts > d->last_ts && (ts - d->last_ts) * 2 > (int64_t)d->interval_us * 3) {
                int ms = (int)((ts - d->last_ts) / 1000);

                d->gaps++;
                if (ms > d->maxgap_ms) d->maxgap_ms = ms;
                if (d->ngap < 4) {     /* #974: where the gap is (picture index, stream time, wall time since the drain start) */
                    d->gap_pic[d->ngap] = d->pics; d->gap_ms[d->ngap] = ms;
                    d->gap_at_ms[d->ngap] = (int)((d->last_ts - d->first_ts) / 1000);
                    d->gap_wall_ms[d->ngap] = (int)((now_us() - d->t_start) / 1000);
                    d->ngap++;
                }
            }
            d->last_ts = ts;
        }
        d->pics++;
        if (d->hold_ms) usleep((useconds_t)d->hold_ms * 1000u);
        IMP_Encoder_ReleaseStream(d->chn, &s);
    }
    return NULL;
}

static void *push_getframe(void *arg)
{
    (void)arg;
    while (g_push_gf_run) {
        IMPFrameInfo *fr = NULL;

        if (IMP_FrameSource_GetFrame(CH_SUB, &fr) == 0 && fr) {
            g_push_gf_ok++;
            IMP_FrameSource_ReleaseFrame(CH_SUB, fr);
        } else {
            g_push_gf_fail++;
            usleep(5000);
        }
    }
    return NULL;
}

static int push_chn_up(int grp, int fs_ch, int chn, int w, int h, int fps)
{
    IMPCell fs = { DEV_ID_FS, fs_ch, 0 }, enc = { DEV_ID_ENC, grp, 0 };
    G(EAttr, a);
    int r;

    r = IMP_Encoder_CreateGroup(grp);
    if (!r) r = IMP_System_Bind(&fs, &enc);
    fill_attr(a, CODEC_H264, fps * 2);
#if defined(PLATFORM_T31)
    a->encAttr.uWidth = w;
    a->encAttr.uHeight = h;
#else
    a->encAttr.picWidth = w;
    a->encAttr.picHeight = h;
#endif
    a->rcAttr.outFrmRate.frmRateNum = fps;
    a->rcAttr.outFrmRate.frmRateDen = 1;
    if (!r) r = IMP_Encoder_CreateChn(chn, a);
    if (!r) r = IMP_Encoder_RegisterChn(grp, chn);
    if (!r) r = IMP_Encoder_StartRecvPic(chn);
    printf("push: chn %d group %d fs %d %dx%d %d fps up: %d\n", chn, grp, fs_ch, w, h, fps, r);
    gfree(a);
    return r;
}

static void push_chn_down(int grp, int fs_ch, int chn)
{
    IMPCell fs = { DEV_ID_FS, fs_ch, 0 }, enc = { DEV_ID_ENC, grp, 0 };

    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    IMP_System_UnBind(&fs, &enc);
    IMP_Encoder_DestroyGroup(grp);
}

int enc_push_run(void)
{
    int sec = push_env("PUSH_SEC", 30), two = push_env("PUSH_2CH", 1), gf = push_env("PUSH_GF", 0), cycles = push_env("PUSH_CYCLES", 20);
    int fps = push_env("PUSH_FPS", g_fps > 0 ? g_fps : 25), n = two ? 2 : 1, i, k, cyc_ok = 0, r;
    PushDrain d[2];
    pthread_t th[2], gth;
    int64_t t0;

    wdog("cases", sec + cycles * 4 + 60);
    memset(d, 0, sizeof(d));
    uint32_t sn0 = 0, sd0 = 0;
    int sensor_set = push_env("PUSH_SENSOR_FPS", 0);    /* #962: SetSensorFPS(N, 1) first, the old rate back at the end */

    IMP_ISP_Tuning_GetSensorFPS(&sn0, &sd0);
    if (sensor_set > 0)
        printf("push: SetSensorFPS(%d, 1) %d\n", sensor_set, IMP_ISP_Tuning_SetSensorFPS((uint32_t)sensor_set, 1));
    {
        uint32_t sn = 0, sd = 0;
        int sr = IMP_ISP_Tuning_GetSensorFPS(&sn, &sd);

        printf("push: sensor fps %u/%u (%d), encoder %d fps\n", sn, sd, sr, fps);
    }
    if (gf) printf("push: SetFrameDepth(%d, %d) %d\n", CH_SUB, push_env("PUSH_DEPTH", 1),
                   IMP_FrameSource_SetFrameDepth(CH_SUB, push_env("PUSH_DEPTH", 1)));
    if (push_chn_up(0, CH_SUB, 0, SUB_W, SUB_H, fps)) return -1;
    d[0].t_up = (int64_t)IMP_System_GetTimeStamp();
    if (two && push_chn_up(1, CH_MAIN, 1, g_sw, g_sh, fps)) { push_chn_down(0, CH_SUB, 0); return -1; }
    d[1].t_up = (int64_t)IMP_System_GetTimeStamp();
    for (i = 0; i < n; i++) {
        d[i].chn = i; d[i].run = 1; d[i].interval_us = 1000000 / fps;
        d[i].hold_ms = i == 0 ? push_env("PUSH_HOLD_MS", 0) : 0;
        if (i == 0 && getenv("PUSH_DUMP") && *getenv("PUSH_DUMP")) {     /* the sub stream only (camera RAM) */
            char fn[256];

            snprintf(fn, sizeof(fn), "%s/push%d.h264", getenv("PUSH_DUMP"), i);
            d[i].dump = fopen(fn, "wb");
            d[i].buf = malloc(STREAMCAP);
        }
        d[i].t_start = now_us();
        pthread_create(&th[i], NULL, push_drain, &d[i]);
    }
    if (gf) { g_push_gf_run = 1; pthread_create(&gth, NULL, push_getframe, NULL); }
    t0 = now_us();
    sleep((unsigned)sec);
    if (gf) { g_push_gf_run = 0; pthread_join(gth, NULL); }
    for (i = 0; i < n; i++) {
        d[i].run = 0;
        pthread_join(th[i], NULL);
        if (d[i].dump) fclose(d[i].dump);
        free(d[i].buf);
    }
    t0 = now_us() - t0;
    for (i = 0; i < n; i++)
        printf("push: chn %d pictures %d in %.1f s (%.2f fps, want %d) gaps %d maxgap %d ms\n", i, d[i].pics, t0 / 1e6,
               d[i].pics * 1e6 / t0, fps, d[i].gaps, d[i].maxgap_ms);
    for (i = 0; i < n; i++) {
        printf("push: chn %d dts", i);
        for (k = 0; k <= 40; k++)
            if (d[i].dts[k]) printf(" %s%dms:%d", k == 40 ? ">=" : "", k * 10, d[i].dts[k]);
        printf(" nonpos:%d\n", d[i].dts_neg);
        printf("push: chn %d first pictures (capture ts - StartRecvPic ms / age at GetStream ms):", i);
        for (k = 0; k < 4 && k < d[i].pics; k++)
            printf(" %lld/%lld", (long long)((d[i].early_ts[k] - d[i].t_up) / 1000), (long long)(d[i].early_age[k] / 1000));
        printf("\n");
        for (k = 0; k < d[i].ngap; k++)
            printf("push: chn %d gap %d ms before picture %d (stream t+%d ms, wall t+%d ms)\n", i, d[i].gap_ms[k], d[i].gap_pic[k],
                   d[i].gap_at_ms[k], d[i].gap_wall_ms[k]);
    }
    if (n == 2 && d[0].first_ts && d[1].first_ts)
        printf("push: first pack ts chn1 - chn0 %lld ms\n", (long long)((d[1].first_ts - d[0].first_ts) / 1000));
    if (gf) printf("push: getframe ok %d (%.2f fps) fail %d\n", g_push_gf_ok, g_push_gf_ok * 1e6 / t0, g_push_gf_fail);
    for (k = 0; k < cycles; k++) {
        int good = 1;

        for (i = 0; i < n; i++) IMP_Encoder_StopRecvPic(i);
        usleep(100000);
        for (i = 0; i < n; i++) IMP_Encoder_StartRecvPic(i);
        for (i = 0; i < n; i++) {
            int got = 0;
            int64_t until = now_us() + 2000000;

            while (got < 5 && now_us() < until) {
                IMPEncoderStream s;

                if (IMP_Encoder_PollingStream(i, 500) != 0) continue;
                memset(&s, 0, sizeof(s));
                if (IMP_Encoder_GetStream(i, &s, 1) != 0) continue;
                IMP_Encoder_ReleaseStream(i, &s);
                got++;
            }
            if (got < 5) good = 0;
        }
        cyc_ok += good;
    }
    printf("push: stop/start cycles %d/%d gave 5 pictures per channel\n", cyc_ok, cycles);
    if (sensor_set > 0 && sn0 && sd0)
        printf("push: SetSensorFPS(%u, %u) back %d\n", sn0, sd0, IMP_ISP_Tuning_SetSensorFPS(sn0, sd0));
    if (two) push_chn_down(1, CH_MAIN, 1);
    push_chn_down(0, CH_SUB, 0);
    r = cyc_ok == cycles ? 0 : -1;
    return r;
}
#else
int enc_push_run(void) { return -1; }
#endif

/* ------------------------------------------------------------------------------------------
 * Explicit case "carrier" (APITEST_CASES=carrier, #931): usage patterns of the vendor reference streamer (carrier-server, SDK 2.1.0).
 *   A2 IMP_Encoder_GetFd + poll() over two channels, then PollingStream(10) + non-blocking GetStream
 *   A3 IMP_Encoder_SetMaxStreamCnt 1/2/3 (before CreateChn) with a slow consumer (streams held, never a blocking GetStream)
 *   A5 FlushStream (never started / running / stopped chn) + drain + RequestIDR before StartRecvPic: first frame after the restart is an IDR
 *   A7 resolution change: StopRecvPic, DisableChn, SetChnAttr on a disabled bound channel, EnableChn, new encoder size
 *   A9 FS GetFrame + IMP_Encoder_InputJpege (no JPEG channel)
 * Lines "carrier: ..." carry only timing-independent values, so the vendor library and OpenIMP can be diffed (QEMU, fresh boot each).
 * CARRIER_PARTS=a2,a3,a5,a7,a9 selects (default all). Never a blocking GetStream on a channel without a frame (vendor hangs).
 * ------------------------------------------------------------------------------------------ */
#if HAS_IMP_Encoder_GetFd && HAS_IMP_Encoder_SetMaxStreamCnt && HAS_IMP_Encoder_GetMaxStreamCnt && HAS_IMP_Encoder_FlushStream
#ifdef ENC_NEW
# define CAR_SIZE(a, w_, h_) do { (a)->encAttr.uWidth = (w_); (a)->encAttr.uHeight = (h_); } while (0)
#else
# define CAR_SIZE(a, w_, h_) do { (a)->encAttr.picWidth = (w_); (a)->encAttr.picHeight = (h_); } while (0)
#endif
static int car_on(const char *p)
{
    const char *v = getenv("CARRIER_PARTS"), *c = getenv("APITEST_CASES");
    char nm[24];

    snprintf(nm, sizeof(nm), "carrier_%s", p);       /* case alias carrier_a3: one part, own log in qemu_run */
    if (c) {                                          /* exact token: carrier_a1 must not match carrier_a10 */
        const char *q;

        for (q = c; (q = strstr(q, nm)) != NULL; q += strlen(nm))
            if (q[strlen(nm)] == 0 || q[strlen(nm)] == ',') return 1;
    }
    if (c && strstr(c, "carrier_") && !strstr(c, "carrier,") && !(strlen(c) >= 7 && !strcmp(c + strlen(c) - 7, "carrier")) ) return 0;
    return !v || !*v || strstr(v, p);
}

static int car_up(int grp, int fs_ch, int chn, int w, int h, int maxstream, int start)
{
    IMPCell fs = { DEV_ID_FS, fs_ch, 0 }, enc = { DEV_ID_ENC, grp, 0 };
    G(EAttr, a);
    int r;

    r = IMP_Encoder_CreateGroup(grp);
    if (!r) r = IMP_System_Bind(&fs, &enc);
    fill_attr(a, CODEC_H264, EGOP);
    CAR_SIZE(a, w, h);
#if defined(APITEST_T32)
    if (!r && IMP_Encoder_SetPool && !getenv("APITEST_NOPOOL")) {
        static int pool_req[3];
        int pid = ENC_POOL_ID + 1 + (chn & 1);
        if (!pool_req[chn & 1] && IMP_System_MemPoolRequest) pool_req[chn & 1] = IMP_System_MemPoolRequest(pid, 8u << 20, "carrier-enc") == 0 ? 1 : -1;
        if (getenv("CARRIER_V")) printf("carrier-v: pool %d req %d SetPool chn %d -> %d\n", pid, pool_req[chn & 1], chn, IMP_Encoder_SetPool(chn, pid));
        else IMP_Encoder_SetPool(chn, pid);
    }
#endif
    if (!r && maxstream > 0 && IMP_Encoder_SetMaxStreamCnt) r = IMP_Encoder_SetMaxStreamCnt(chn, maxstream);
    if (!r) r = IMP_Encoder_CreateChn(chn, a);
    if (!r) r = IMP_Encoder_RegisterChn(grp, chn);
    if (!r && start) r = IMP_Encoder_StartRecvPic(chn);
    gfree(a);
    return r;
}

static void car_down(int grp, int fs_ch, int chn)
{
    IMPCell fs = { DEV_ID_FS, fs_ch, 0 }, enc = { DEV_ID_ENC, grp, 0 };

    IMP_Encoder_StopRecvPic(chn);
    IMP_Encoder_UnRegisterChn(chn);
    IMP_Encoder_DestroyChn(chn);
    IMP_System_UnBind(&fs, &enc);
    IMP_Encoder_DestroyGroup(grp);
}

/* one non-blocking fetch after a poll wake: 0 got+released a frame, else -1 / -2 (Polling failed) */
static int car_fetch(int chn, int poll_ms, int hold, IMPEncoderStream **keep)
{
    G(IMPEncoderStream, s);
    int r = IMP_Encoder_PollingStream(chn, poll_ms);

    if (r) { gfree(s); return -2; }
    r = IMP_Encoder_GetStream(chn, s, false);
    if (r) { gfree(s); return -1; }
    if (hold && keep) { *keep = s; return 0; }
    IMP_Encoder_ReleaseStream(chn, s);
    gfree(s);
    return 0;
}

static void car_a2(void)
{
    int f0, f1, i, n0 = 0, n1 = 0, wake_bad = 0, get_bad = 0, tmo = 0, r0, r1, empty, fs2;
    struct pollfd pf[2];

    if (!IMP_Encoder_GetFd || !IMP_Encoder_PollingStream) { rep_na(FN(IMP_Encoder_GetFd)); return; }
    {   /* second 640x360 source: FS ch2 like ch1 (the QEMU vendor encoder cannot do the 1080p main channel) */
        G(IMPFSChnAttr, fa);
        IMP_FrameSource_GetChnAttr(CH_SUB, fa);
        fs2 = IMP_FrameSource_CreateChn(2, fa);
        if (!fs2) fs2 = IMP_FrameSource_EnableChn(2);
        gfree(fa);
    }
    r0 = car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1);
    r1 = fs2 ? fs2 : car_up(1, 2, 1, SUB_W, SUB_H, 0, 1);
    f0 = IMP_Encoder_GetFd(0);
    f1 = IMP_Encoder_GetFd(1);
    for (i = 0; i < 80 && (n0 < 10 || n1 < 10); i++) {
        int k;

        pf[0].fd = f0; pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = f1; pf[1].events = POLLIN; pf[1].revents = 0;
        if (poll(pf, 2, 2000) <= 0) { tmo++; continue; }
        for (k = 0; k < 2; k++) {
            if (!(pf[k].revents & POLLIN)) continue;
            switch (car_fetch(k, 10, 0, NULL)) {
            case 0: if (k) n1++; else n0++; break;
            case -2: wake_bad++; break;
            default: get_bad++; break;
            }
        }
    }
    printf("carrier: A2 FS2 %d up %d/%d fd0>=0 %d fd1>=0 %d distinct %d frames chn0>=10 %d chn1>=10 %d poll-timeouts %d, Polling after wake failed %d, GetStream(nonblock) after wake failed %d\n",
           fs2, r0, r1, f0 >= 0, f1 >= 0, f0 != f1, n0 >= 10, n1 >= 10, tmo > 0, wake_bad, get_bad);
    /* empty channel: stop chn 0, drain, non-blocking GetStream must return at once (no hang) */
    IMP_Encoder_StopRecvPic(0);
    if (getenv("CARRIER_V")) { printf("carrier-v: A2 stopped, draining\n"); fflush(stdout); }
    for (i = 0; i < 40 && car_fetch(0, 100, 0, NULL) == 0; i++) ;
    if (getenv("CARRIER_V")) { printf("carrier-v: A2 drained after %d, GetStream(nonblock)\n", i); fflush(stdout); }
    empty = -1;
    if (getenv("CARRIER_EMPTY")) {    /* opt-in: the VENDOR library blocks forever in GetStream(block=false) on a drained stopped chn (QEMU, 2026-11-26) */
        G(IMPEncoderStream, s);
        int64_t t0 = now_us();
        empty = IMP_Encoder_GetStream(0, s, false);
        printf("carrier: A2 GetStream(nonblock) on a drained stopped chn: rc %d, returned at once %d\n", empty, now_us() - t0 < 1000000);
        gfree(s);
    }
    {
        int pr = IMP_Encoder_PollingStream(0, 10);
        printf("carrier: A2 PollingStream(10) on a drained stopped chn: rc %d\n", pr);
    }
    rep(FN(IMP_Encoder_GetFd), f0, (!r0 && !r1 && f0 >= 0 && f1 >= 0 && f0 != f1 && n0 >= 10 && n1 >= 10 && !wake_bad && !get_bad) ? V_PASS : V_FAIL,
        "poll over 2 chns: chn0 %d, chn1 %d frames, Polling-after-wake failures %d, GetStream failures %d, empty GetStream %d", n0, n1, wake_bad, get_bad, empty);
    car_down(0, CH_SUB, 0);
    car_down(1, 2, 1);
    IMP_FrameSource_DisableChn(2);
    IMP_FrameSource_DestroyChn(2);
}

static void car_a3(void)
{
    int mx, ok_all = 1;

    if (!IMP_Encoder_SetMaxStreamCnt || !IMP_Encoder_GetMaxStreamCnt) { rep_na(FN(IMP_Encoder_SetMaxStreamCnt)); return; }
    for (mx = 1; mx <= 3; mx++) {
        IMPEncoderStream *held[8];
        int nh = 0, rg, cnt = -7, i, after, r = car_up(0, CH_SUB, 0, SUB_W, SUB_H, mx, 1), rc;
        SI si;

        rg = IMP_Encoder_GetMaxStreamCnt(0, &cnt);
        memset(held, 0, sizeof(held));
        for (i = 0; i < 6; i++) {            /* slow consumer: hold every stream we get, never release; stop at the first refusal */
            if (car_fetch(0, 700, 1, &held[nh]) != 0) break;
            nh++;
        }
        for (i = 0; i < nh; i++) { IMP_Encoder_ReleaseStream(0, held[i]); gfree(held[i]); }
        pull(0, CODEC_H264, 8, 0, 0, &si, 3000);
        after = si.frames;
        rc = IMP_Encoder_SetMaxStreamCnt(0, mx);      /* after CreateChn */
        printf("carrier: A3 max %d: up %d, GetMaxStreamCnt rc %d value %d, held streams %d (%s), 8 frames after release: %d, Set after CreateChn rc %d\n",
               mx, r, rg, cnt, nh, nh >= 1 ? "some" : "none", after, rc);
        if (r || rg || cnt != mx || nh < 1 || after < 8) ok_all = 0;
        car_down(0, CH_SUB, 0);
    }
    {   /* range checks */
        int rz, r4, r9;
        car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 0);
        rz = IMP_Encoder_SetMaxStreamCnt(0, 0); r4 = IMP_Encoder_SetMaxStreamCnt(0, 4); r9 = IMP_Encoder_SetMaxStreamCnt(0, 99);
        printf("carrier: A3 range: 0 -> %d, 4 -> %d, 99 -> %d\n", rz, r4, r9);
        car_down(0, CH_SUB, 0);
    }
    rep(FN(IMP_Encoder_SetMaxStreamCnt), 0, ok_all ? V_PASS : V_FAIL, "max 1/2/3 before CreateChn: read back, slow consumer holds streams, frames flow again after release");
}

static void car_a5(void)
{
    int rf_new, rf_bad, rf_run, rf_stop, rq, rs, ndrain = 0, i, left0 = -1, left1 = -1, idr0, idr1, nstale;
    SI si, si2;
    EStat q;

    if (!IMP_Encoder_FlushStream || !IMP_Encoder_RequestIDR) { rep_na(FN(IMP_Encoder_FlushStream)); return; }
    if (car_up(0, CH_SUB, 0, SUB_W, SUB_H, 2, 0)) { rep(FN(IMP_Encoder_FlushStream), -1, V_FAIL, "chn up failed"); return; }
    rf_new = IMP_Encoder_FlushStream(0);                 /* created, never started */
    rf_bad = IMP_Encoder_FlushStream(7);                 /* never created */
    rq = IMP_Encoder_RequestIDR(0);                      /* before StartRecvPic (carrier order) */
    rs = IMP_Encoder_StartRecvPic(0);
    pull(0, CODEC_H264, 10, 0, 0, &si, 3000);
    idr0 = si.first_idr_frame;
    rf_run = IMP_Encoder_FlushStream(0);                 /* running */
    /* idle stop with unconsumed frames queued, then restart the carrier way: Flush, drain, RequestIDR, Start */
    usleep(400000);
    IMP_Encoder_StopRecvPic(0);
    memset(&q, 0xff, sizeof(q));
    IMP_Encoder_Query(0, &q); left0 = (int)q.leftStreamFrames > 0;
    rf_stop = IMP_Encoder_FlushStream(0);                /* stopped */
    memset(&q, 0xff, sizeof(q));
    IMP_Encoder_Query(0, &q); left1 = (int)q.leftStreamFrames;
    for (i = 0; i < 6 && car_fetch(0, 100, 0, NULL) == 0; i++) ndrain++;
    IMP_Encoder_RequestIDR(0);
    rs = IMP_Encoder_StartRecvPic(0);
    pull(0, CODEC_H264, 10, 0, 0, &si2, 3000);
    idr1 = si2.first_idr_frame;
    printf("carrier: A5 Flush on never-started %d, on unknown chn 7 %d, RequestIDR before Start %d, Start %d; first frame IDR at index %d (10 frames %d)\n",
           rf_new, rf_bad, rq, rs, idr0, si.frames);
    printf("carrier: A5 Flush on running %d, on stopped %d; frames queued before flush %d, after flush %d, drained after flush %d; after restart first IDR at index %d (10 frames %d)\n",
           rf_run, rf_stop, left0, left1, ndrain, idr1, si2.frames);
    /* control: restart WITHOUT flush/drain, informational (stale frames show up here) */
    usleep(300000);
    IMP_Encoder_StopRecvPic(0);
    usleep(400000);
    IMP_Encoder_RequestIDR(0);
    IMP_Encoder_StartRecvPic(0);
    pull(0, CODEC_H264, 10, 0, 0, &si, 3000);
    nstale = si.first_idr_frame;
    printf("carrier: A5 control restart without flush: first IDR at index %d (0 = no stale frame in front)\n", nstale);
    rep(FN(IMP_Encoder_FlushStream), rf_stop, (rf_stop == 0 && rf_run == 0 && left1 == 0 && ndrain == 0 && idr1 == 0 && si2.frames >= 10) ? V_PASS : V_FAIL,
        "flush stopped %d running %d, queued after flush %d, drained %d, first frame after restart IDR index %d", rf_stop, rf_run, left1, ndrain, idr1);
    (void)nstale;
    car_down(0, CH_SUB, 0);
}

static void car_a7(void)
{
    G(IMPFSChnAttr, at);
    G(IMPFSChnAttr, rd);
    int rst, rg, rd_ = -9, rset_en, rset, ren, ok = 1, w0, h0, nw = 480, nh = 272, rr;
    SI si, si2;
    char sz[64] = "";

    if (car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1)) { rep(FN(IMP_FrameSource_SetChnAttr), -1, V_FAIL, "chn up failed"); gfree(at); gfree(rd); return; }
    pull(0, CODEC_H264, 5, 0, 0, &si, 3000);
    rg = IMP_FrameSource_GetChnAttr(CH_SUB, at);
    w0 = at->picWidth; h0 = at->picHeight;
    rst = IMP_Encoder_StopRecvPic(0);
    at->picWidth = nw; at->picHeight = nh; at->scaler.enable = 1; at->scaler.outwidth = nw; at->scaler.outheight = nh;
    rset_en = IMP_FrameSource_SetChnAttr(CH_SUB, at);          /* channel still enabled and bound */
    rd_ = IMP_FrameSource_DisableChn(CH_SUB);
    rset = IMP_FrameSource_SetChnAttr(CH_SUB, at);             /* disabled, still bound */
    memset(rd, 0, sizeof(*rd));
    rr = IMP_FrameSource_GetChnAttr(CH_SUB, rd);
    ren = IMP_FrameSource_EnableChn(CH_SUB);
    /* the encoder follows at the new size */
    IMP_Encoder_UnRegisterChn(0); IMP_Encoder_DestroyChn(0);
    {
        G(EAttr, a);
        int rc, rreg, rsr;

        fill_attr(a, CODEC_H264, EGOP);
        CAR_SIZE(a, nw, nh);
        rc = IMP_Encoder_CreateChn(0, a);
        rreg = rc ? rc : IMP_Encoder_RegisterChn(0, 0);
        rsr = rreg ? rreg : IMP_Encoder_StartRecvPic(0);
        pull(0, CODEC_H264, 10, 0, 0, &si2, 3000);
        snprintf(sz, sizeof(sz), "%dx%d", si2.w, si2.h);
        printf("carrier: A7 GetChnAttr %d, size %dx%d; Stop %d; SetChnAttr while enabled -> %d; Disable %d; SetChnAttr disabled -> %d, read back %dx%d (rc %d); Enable %d; encoder at new size: create %d reg %d start %d, SPS %s, frames %d\n",
               rg, w0, h0, rst, rset_en, rd_, rset, rd->picWidth, rd->picHeight, rr, ren, rc, rreg, rsr, sz, si2.frames);
        if (rc || rreg || rsr) ok = 0;
        gfree(a);
    }
    if (rset || ren || si2.w != nw || si2.h != nh || si2.frames < 10) ok = 0;
    /* back to the original size, same way */
    IMP_Encoder_StopRecvPic(0); IMP_Encoder_UnRegisterChn(0); IMP_Encoder_DestroyChn(0);
    IMP_FrameSource_DisableChn(CH_SUB);
    at->picWidth = w0; at->picHeight = h0; at->scaler.outwidth = w0; at->scaler.outheight = h0;
    IMP_FrameSource_SetChnAttr(CH_SUB, at);
    IMP_FrameSource_EnableChn(CH_SUB);
    {
        G(EAttr, a);
        fill_attr(a, CODEC_H264, EGOP);
        IMP_Encoder_CreateChn(0, a); IMP_Encoder_RegisterChn(0, 0); IMP_Encoder_StartRecvPic(0);
        pull(0, CODEC_H264, 10, 0, 0, &si, 3000);
        printf("carrier: A7 back to %dx%d: SPS %dx%d, frames %d\n", w0, h0, si.w, si.h, si.frames);
        if (si.w != w0 || si.h != h0 || si.frames < 10) ok = 0;
        gfree(a);
    }
    rep(FN(IMP_FrameSource_SetChnAttr), rset, ok ? V_PASS : V_FAIL, "SetChnAttr on a disabled bound channel %dx%d -> %dx%d and back, encoder follows, SPS checked", w0, h0, nw, nh);
    car_down(0, CH_SUB, 0);
    gfree(at); gfree(rd);
}

#if !HAS_IMP_Encoder_InputJpege
static void car_a9(void) { }
#else
static void car_a9(void)
{
    IMPFrameInfo *fr = NULL;
    unsigned char *dst;
    int rd, rg, len = -7, r, rbad_w, rbad_h, soi = 0, eoi = 0, w, h, rrel;

    if (!IMP_Encoder_InputJpege) { rep_na(FN(IMP_Encoder_InputJpege)); return; }
    rd = IMP_FrameSource_SetFrameDepth(CH_SUB, 1);
    usleep(300000);
    rg = IMP_FrameSource_GetFrame(CH_SUB, &fr);
    if (rg || !fr) { rep(FN(IMP_Encoder_InputJpege), rg, V_FAIL, "SetFrameDepth %d, GetFrame %d: no frame", rd, rg); IMP_FrameSource_SetFrameDepth(CH_SUB, 0); return; }
    w = fr->width; h = fr->height;
    dst = malloc((size_t)w * h);
    len = -7;
    r = IMP_Encoder_InputJpege((uint8_t *)(uintptr_t)fr->virAddr, dst, w, h, 25, &len);
    if (r == 0 && len > 4) { soi = dst[0] == 0xff && dst[1] == 0xd8; eoi = dst[len - 2] == 0xff && dst[len - 1] == 0xd9; }
    {
        int l2 = -7;
        rbad_w = IMP_Encoder_InputJpege((uint8_t *)(uintptr_t)fr->virAddr, dst, w - 8, h, 25, &l2);
        rbad_h = IMP_Encoder_InputJpege((uint8_t *)(uintptr_t)fr->virAddr, dst, w, h - 1, 25, &l2);
    }
    rrel = IMP_FrameSource_ReleaseFrame(CH_SUB, fr);
    printf("carrier: A9 SetFrameDepth %d, GetFrame %d %ux%u; InputJpege rc 0x%x len>0 %d SOI %d EOI %d; width-8 rc 0x%x, height-1 rc 0x%x; Release %d, frames flow %d\n",
           rd, rg, w, h, (unsigned)r, len > 0, soi, eoi, (unsigned)rbad_w, (unsigned)rbad_h, rrel, enc_pull_h264(0) >= 0);
    rep(FN(IMP_Encoder_InputJpege), r, (r == 0 && len > 4 && soi && eoi) ? V_PASS : V_FAIL,
        "GetFrame %ux%u -> InputJpege q25: len %d SOI %d EOI %d; no JPEG channel", w, h, len, soi, eoi);
    free(dst);
    IMP_FrameSource_SetFrameDepth(CH_SUB, 0);
}
#endif

/* ---- batch 2 (#931): A1 A4 A6 A8 A10 A11 A12 ---- */

/* frames and the largest packet count of one frame (slice packets with a multi-section mode) */
static void car_packs(int chn, int n, int *nfr, int *maxp, int *minp)
{
    int i;

    *nfr = 0; *maxp = 0; *minp = 1 << 20;
    for (i = 0; i < n * 4 && *nfr < n; i++) {
        G(IMPEncoderStream, s);

        if (IMP_Encoder_PollingStream(chn, 2000) || IMP_Encoder_GetStream(chn, s, false)) { gfree(s); continue; }
        if ((int)s->packCount > *maxp) *maxp = (int)s->packCount;
        if ((int)s->packCount < *minp) *minp = (int)s->packCount;
        (*nfr)++;
        IMP_Encoder_ReleaseStream(chn, s);
        gfree(s);
    }
    if (!*nfr) *minp = 0;
}

static int car_pre_ms = -77, car_pre_jms = -77, car_pre_osd = -77, car_pre_isp = -77;
static int car_pre_done;

/* called by main() right before IMP_System_Init (A4 + A10 need the calls BEFORE the init; CARRIER_PRE=0 skips them as a control) */
void enc_carrier_pre_sysinit(void)
{
    const char *c = getenv("APITEST_CASES"), *pre = getenv("CARRIER_PRE");

    if (!c || !strstr(c, "carrier") || (pre && !strcmp(pre, "0"))) return;
    car_pre_done = 1;
#if HAS_IMP_Encoder_SetMultiSectionMode
    if (IMP_Encoder_SetMultiSectionMode) car_pre_ms = IMP_Encoder_SetMultiSectionMode(1, 100, 2);
#endif
#if HAS_IMP_Encoder_SetJpegMultiSectionMode
    if (IMP_Encoder_SetJpegMultiSectionMode) car_pre_jms = IMP_Encoder_SetJpegMultiSectionMode(1, 50, 2);
#endif
#if HAS_IMP_OSD_SetPoolSize
    if (IMP_OSD_SetPoolSize) car_pre_osd = IMP_OSD_SetPoolSize(512 * 1024);
#endif
#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_SetOsdPoolSize
    if (IMP_ISP_Tuning_SetOsdPoolSize) car_pre_isp = IMP_ISP_Tuning_SetOsdPoolSize(512 * 1024);
#endif
    printf("carrier: pre-init SetMultiSectionMode(1,100,2) %d, SetJpegMultiSectionMode(1,50,2) %d, OSD_SetPoolSize(512K) %d, ISP_Tuning_SetOsdPoolSize(512K) %d (-77 = no such API)\n",
           car_pre_ms, car_pre_jms, car_pre_osd, car_pre_isp);
    fflush(stdout);
}

/* A1: FS + encoder stay running, the client side repeats StartRecvPic and attaches/detaches without any per-session stop */
static void car_a1(void)
{
    int rs2, rs3, ren, sess, ndrain = 0, ok = 1, idr[3] = { -9, -9, -9 }, fr[3] = { 0, 0, 0 }, rf[3] = { 0, 0, 0 }, rq[3] = { 0, 0, 0 };
    SI si;

    if (car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1)) { rep(FN(IMP_Encoder_StartRecvPic), -1, V_FAIL, "chn up failed"); return; }
    pull(0, CODEC_H264, 5, 0, 0, &si, 3000);
    rs2 = IMP_Encoder_StartRecvPic(0);                  /* already started */
    rs3 = IMP_Encoder_StartRecvPic(0);
    ren = IMP_FrameSource_EnableChn(CH_SUB);            /* FS already enabled */
    for (sess = 0; sess < 3; sess++) {
        rf[sess] = IMP_Encoder_FlushStream(0);          /* attach: Flush, drain, RequestIDR, (Start again) */
        { int i; for (i = 0; i < 6 && car_fetch(0, 50, 0, NULL) == 0; i++) ndrain++; }
        rq[sess] = IMP_Encoder_RequestIDR(0);
        IMP_Encoder_StartRecvPic(0);
        pull(0, CODEC_H264, 8, 0, 0, &si, 3000);
        idr[sess] = si.first_idr_frame; fr[sess] = si.frames;
        usleep(500000);                                 /* detach: nobody consumes, no StopRecvPic */
        if (idr[sess] != 0 || fr[sess] < 8 || rf[sess] || rq[sess]) ok = 0;
    }
    (void)ndrain;
    printf("carrier: A1 StartRecvPic on a started chn %d / %d, FS EnableChn again %d; 3 sessions (Flush rc %d %d %d, RequestIDR rc %d %d %d): first IDR index %d %d %d, frames %d %d %d\n",
           rs2, rs3, ren, rf[0], rf[1], rf[2], rq[0], rq[1], rq[2], idr[0], idr[1], idr[2], fr[0], fr[1], fr[2]);
    rep(FN(IMP_Encoder_StartRecvPic), rs2 | rs3, ok ? V_PASS : V_FAIL, "repeated StartRecvPic %d %d, FS enable again %d, 3 attach/detach sessions: IDR first, 8 frames each", rs2, rs3, ren);
    car_down(0, CH_SUB, 0);
}

/* A4: multi-section modes before IMP_System_Init (enc_carrier_pre_sysinit), later calls, slice packets of one frame */
static void car_a4(void)
{
    int late = -77, latej = -77, bad0 = -77, bad1 = -77, nfr, maxp, minp;

    if (!car_pre_done) { rep_na(FN(IMP_Encoder_GetStream)); printf("carrier: A4 skipped (CARRIER_PRE=0 or no carrier case)\n"); }
#if HAS_IMP_Encoder_SetMultiSectionMode
    if (IMP_Encoder_SetMultiSectionMode) { bad0 = IMP_Encoder_SetMultiSectionMode(0, 100, 2); bad1 = IMP_Encoder_SetMultiSectionMode(1, 0, 2); }
#endif
    if (car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1)) { rep(FN(IMP_Encoder_GetStream), -1, V_FAIL, "chn up failed"); return; }
#if HAS_IMP_Encoder_SetMultiSectionMode
    if (IMP_Encoder_SetMultiSectionMode) late = IMP_Encoder_SetMultiSectionMode(1, 100, 2);        /* encoder module already exists */
#endif
#if HAS_IMP_Encoder_SetJpegMultiSectionMode
    if (IMP_Encoder_SetJpegMultiSectionMode) latej = IMP_Encoder_SetJpegMultiSectionMode(1, 50, 2);
#endif
    car_packs(0, 10, &nfr, &maxp, &minp);
    printf("carrier: A4 pre-init rc ms %d jms %d; after init: ms(1,100,2) %d, ms(0,100,2) %d, ms(1,0,2) %d, jms %d; 10 frames %d, packets per frame min>=1 %d, max>1 %d\n",
           car_pre_ms, car_pre_jms, late, bad0, bad1, latej, nfr, minp >= 1, maxp > 1);
    rep(FN(IMP_Encoder_GetStream), nfr >= 10 ? 0 : -1, nfr >= 10 ? V_PASS : V_FAIL, "multi-section set before init (rc %d), frames flow, packets per frame max %d", car_pre_ms, maxp);
    car_down(0, CH_SUB, 0);
}

/* A6: hot channel-layer rebuild with the system layer kept: FS ch2 + encoder destroyed and recreated at alternating sizes */
static int car_memfree_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char l[128];
    int v = -1;

    if (!f) return -1;
    while (fgets(l, sizeof(l), f)) if (sscanf(l, "MemFree: %d kB", &v) == 1) break;
    fclose(f);
    return v;
}

static void car_a6(void)
{
    int loops = getenv("CARRIER_LOOPS") ? atoi(getenv("CARRIER_LOOPS")) : 6, i, bad = 0, badsize = 0, badflow = 0, m0, m1, rfs = 0, rd = 0;
    static const int sz[2][2] = { { SUB_W, SUB_H }, { 480, 272 } };

    car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1);
    car_down(0, CH_SUB, 0);                       /* warm up: first create/destroy allocates the pools */
    m0 = car_memfree_kb();
    for (i = 0; i < loops; i++) {
        G(IMPFSChnAttr, fa);
        SI si;
        int w = sz[i & 1][0], h = sz[i & 1][1], rc;

        IMP_FrameSource_GetChnAttr(CH_SUB, fa);
        fa->picWidth = w; fa->picHeight = h; fa->scaler.enable = 1; fa->scaler.outwidth = w; fa->scaler.outheight = h;
        rfs = IMP_FrameSource_CreateChn(2, fa);
        if (!rfs) rfs = IMP_FrameSource_EnableChn(2);
        rc = rfs ? rfs : car_up(1, 2, 1, w, h, 0, 1);
        memset(&si, 0, sizeof(si));
        if (!rc) pull(1, CODEC_H264, 6, 0, 0, &si, 3000);
        if (rc || rfs) bad++;
        if (si.w != w || si.h != h) badsize++;
        if (si.frames < 6) badflow++;
        if (!rc) car_down(1, 2, 1);
        IMP_FrameSource_DisableChn(2);
        rd = IMP_FrameSource_DestroyChn(2);
        if (rd) bad++;
        gfree(fa);
    }
    m1 = car_memfree_kb();
    if (getenv("CARRIER_V")) printf("carrier-v: A6 MemFree %d -> %d kB\n", m0, m1);
    printf("carrier: A6 %d rebuilds of FS ch2 + encoder (System_Init kept) at alternating sizes: create/destroy errors %d, wrong SPS size %d, too few frames %d, MemFree loss < 2 MB %d\n",
           loops, bad, badsize, badflow, m0 < 0 || m0 - m1 < 2048);
    rep(FN(IMP_FrameSource_DestroyChn), bad, (!bad && !badsize && !badflow && (m0 < 0 || m0 - m1 < 2048)) ? V_PASS : V_FAIL, "%d hot rebuilds, SPS follows the size, no leak > 2 MB", loops);
}

/* A8: encoder on the FS channel while GetFrame / SnapFrame run on the same channel (#922, #932) */
static void car_a8(void)
{
    int i, nstream = 0, ngf = 0, rdepth, rcopy = -77, rsnap = -77, rrel = 0, second = -77, rd0a = 0, rd0b = 0, nsnap_nz = 0;
    IMPFrameInfo *f1 = NULL, *f2 = NULL;
    SI si;

    if (car_up(0, CH_SUB, 0, SUB_W, SUB_H, 0, 1)) { rep(FN(IMP_FrameSource_GetFrame), -1, V_FAIL, "chn up failed"); return; }
#if HAS_IMP_FrameSource_SetFrameDepthCopyType
    if (IMP_FrameSource_SetFrameDepthCopyType) rcopy = IMP_FrameSource_SetFrameDepthCopyType(CH_SUB, 0);
#endif
    rdepth = IMP_FrameSource_SetFrameDepth(CH_SUB, 1);
    for (i = 0; i < 30; i++) {
        if (car_fetch(0, 2000, 0, NULL) == 0) nstream++;
        if (i % 3 == 0) {
            int rg = IMP_FrameSource_GetFrame(CH_SUB, &f1);

            if (rg == 0 && f1) {
                ngf++;
                if (i == 0) second = IMP_FrameSource_GetFrame(CH_SUB, &f2);        /* a second one before ReleaseFrame (vendor: refused) */
                if (IMP_FrameSource_ReleaseFrame(CH_SUB, f1)) rrel++;
            }
            f1 = NULL;
        }
    }
    {
        size_t nv = (size_t)SUB_W * SUB_H * 3 / 2, k;
        unsigned char *buf = malloc(nv);
        G(IMPFrameInfo, fi);

#if HAS_IMP_FrameSource_SnapFrame
        if (buf && IMP_FrameSource_SnapFrame) {
            memset(buf, 0, nv);
            rsnap = IMP_FrameSource_SnapFrame(CH_SUB, PIX_FMT_NV12, SUB_W, SUB_H, buf, fi);
            for (k = 0; k < nv; k += 61) if (buf[k]) nsnap_nz++;
        }
#endif
        free(buf); gfree(fi);
    }
    rd0a = IMP_FrameSource_SetFrameDepth(CH_SUB, 0);
    rd0b = IMP_FrameSource_SetFrameDepth(CH_SUB, 0);                /* repeated, vendor: explicit "no depth" state */
    pull(0, CODEC_H264, 10, 0, 0, &si, 3000);
    printf("carrier: A8 SetFrameDepthCopyType(0) %d, SetFrameDepth(1) %d; 30 rounds: stream frames %d >= 28 %d, GetFrame %d >= 8 %d, Release errors %d, second GetFrame before Release rc %d; SnapFrame NV12 rc %d content %d; SetFrameDepth(0) %d %d; encoder 10 frames after %d\n",
           rcopy, rdepth, nstream, nstream >= 28, ngf, ngf >= 8, rrel, second, rsnap, nsnap_nz > 100, rd0a, rd0b, si.frames);
    rep(FN(IMP_FrameSource_GetFrame), rdepth, (rdepth == 0 && nstream >= 28 && ngf >= 8 && !rrel && rd0a == 0 && si.frames >= 10) ? V_PASS : V_FAIL,
        "encoder + GetFrame on one channel: %d stream frames, %d GetFrame, depth back to 0, encoder %d frames", nstream, ngf, si.frames);
    car_down(0, CH_SUB, 0);
}

/* A10: IPU/ISP OSD pools sized before the init (pre-init calls above), then an ISP OSD picture and a pool-exhausting one */
static void car_a10(void)
{
    int late = -77;

#if HAS_IMP_OSD_SetPoolSize
    if (IMP_OSD_SetPoolSize) late = IMP_OSD_SetPoolSize(256 * 1024);       /* after the init */
#endif
    printf("carrier: A10 pre-init OSD_SetPoolSize %d, ISP_Tuning_SetOsdPoolSize %d; OSD_SetPoolSize after init %d\n", car_pre_osd, car_pre_isp, late);
#if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_CreateOsdRgn && HAS_IMP_ISP_Tuning_SetOsdRgnAttr && HAS_IMP_ISP_Tuning_DestroyOsdRgn
    if (IMP_ISP_Tuning_CreateOsdRgn && IMP_ISP_Tuning_SetOsdRgnAttr && IMP_ISP_Tuning_DestroyOsdRgn) {
        static const int dim[2][2] = { { 128, 64 }, { 512, 512 } };        /* 32 KiB fits a 512 KiB pool, 1 MiB does not */
        int rs[2] = { -99, -99 }, h[2] = { -99, -99 }, rd[2] = { -99, -99 }, k;

        for (k = 0; k < 2; k++) {
            IMPIspOsdAttrAsm *a = (IMPIspOsdAttrAsm *)calloc(1, sizeof(*a));
            uint8_t *pic = (uint8_t *)calloc((size_t)dim[k][0] * dim[k][1], 4);

            if (!a || !pic) { free(a); free(pic); continue; }
            h[k] = IMP_ISP_Tuning_CreateOsdRgn(0, NULL);
            if (h[k] >= 0) {
                a->type = ISP_OSD_REG_PIC;
                a->stsinglepicAttr.chnOSDAttr.osd_type = IMP_ISP_PIC_ARGB_8888;
                a->stsinglepicAttr.chnOSDAttr.osd_argb_type = IMP_ISP_ARGB_TYPE_ARGB;
                a->stsinglepicAttr.chnOSDAttr.osd_pixel_alpha_disable = IMPISP_TUNING_OPS_MODE_DISABLE;
                a->stsinglepicAttr.pic.pinum = (uint8_t)h[k]; a->stsinglepicAttr.pic.osd_enable = 1;
                a->stsinglepicAttr.pic.osd_left = 64; a->stsinglepicAttr.pic.osd_top = 64;
                a->stsinglepicAttr.pic.osd_width = dim[k][0]; a->stsinglepicAttr.pic.osd_height = dim[k][1];
                a->stsinglepicAttr.pic.osd_image = (char *)pic; a->stsinglepicAttr.pic.osd_stride = dim[k][0] * 4;
                rs[k] = IMP_ISP_Tuning_SetOsdRgnAttr(0, h[k], a);
                rd[k] = IMP_ISP_Tuning_DestroyOsdRgn(0, h[k]);
            }
            free(a); free(pic);
        }
        printf("carrier: A10 ISP OSD 128x64: handle>=0 %d SetRgnAttr %d Destroy %d; 512x512 (1 MiB): handle>=0 %d SetRgnAttr %d Destroy %d\n",
               h[0] >= 0, rs[0], rd[0], h[1] >= 0, rs[1], rd[1]);
        rep(FN(IMP_ISP_Tuning_SetOsdRgnAttr), rs[0], (h[0] >= 0 && rs[0] == 0 && rd[0] == 0 && rs[1] != 0) ? V_PASS : V_FAIL,
            "pool 512 KiB sized before init: 128x64 picture accepted, 1 MiB picture refused (%d)", rs[1]);
    }
#endif
}

/* A12: day/night = SetISPRunningMode (ircut is external), then the AE state and AWB statistics are read again */
#if defined(PLATFORM_T41)
# define CAR_RM_SET(m) IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, &(m))
#else
# define CAR_RM_SET(m) IMP_ISP_Tuning_SetISPRunningMode(m)
#endif
static void car_a12(void)
{
#if HAS_IMP_ISP_Tuning_SetISPRunningMode && HAS_IMP_ISP_Tuning_GetISPRunningMode
    IMPISPRunningMode night = IMPISP_RUNNING_MODE_NIGHT, day = IMPISP_RUNNING_MODE_DAY;
    G(IMPISPRunningMode, rb);
    int rn, rbn, rdy, rbd, rae = -77, rawb = -77, stable = -1, awbnz = 0, i, reached = 0;

    rn = CAR_RM_SET(night);
    rbn = IMP_ISP_Tuning_GetISPRunningMode(V0 rb); gchk(rb);
    rbn |= *rb != IMPISP_RUNNING_MODE_NIGHT;
# if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetAeOnlyReadAttr
    {
        G(IMPISPAeOnlyReadAttr, ae);

        for (i = 0; i < 40; i++) {                      /* wait for the AE stable stage >= 2 (QEMU sensors may never get there: informational) */
            rae = IMP_ISP_Tuning_GetAeOnlyReadAttr(IMPVI_MAIN, ae);
            if (rae == 0) stable = ae->stable;
            if (rae == 0 && stable >= 2) { reached = 1; break; }
            usleep(250000);
        }
        gfree(ae);
    }
# endif
# if defined(PLATFORM_T41) && HAS_IMP_ISP_Tuning_GetAwbStatistics
    {
        IMPISPAWBStatisInfo *aw = calloc(1, sizeof(*aw));
        int z;

        if (aw) {
            rawb = IMP_ISP_Tuning_GetAwbStatistics(IMPVI_MAIN, aw);
            for (z = 0; z < 32 * 32; z++) if (aw->awb_r.zone[z / 32][z % 32] || aw->awb_g.zone[z / 32][z % 32]) awbnz++;
            free(aw);
        }
    }
# endif
    rdy = CAR_RM_SET(day);
    rbd = IMP_ISP_Tuning_GetISPRunningMode(V0 rb); gchk(rb);
    rbd |= *rb != IMPISP_RUNNING_MODE_DAY;
    printf("carrier: A12 SetISPRunningMode night %d read back mismatch %d; GetAeOnlyReadAttr rc %d (stable>=2 reached %d, last stable %d); GetAwbStatistics rc %d (any zone non-zero %d); day %d mismatch %d; frames flow %d\n",
           rn, rbn, rae, reached, stable, rawb, awbnz > 0, rdy, rbd, enc_pull_h264(0) >= 0);
    rep(FN(IMP_ISP_Tuning_SetISPRunningMode), rn | rdy, (!rn && !rdy && !rbn && !rbd) ? V_PASS : V_FAIL, "night and back to day, read back equal");
    gfree(rb);
#else
    rep_na(FN(IMP_ISP_Tuning_SetISPRunningMode));
#endif
}

int enc_carrier_run(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);      /* a vendor abort must not eat the lines before it */
    wdog("cases", 240);
    if (car_on("a2")) car_a2();
    wdog("cases", 240);
    if (car_on("a3")) car_a3();
    wdog("cases", 240);
    if (car_on("a5")) car_a5();
    wdog("cases", 240);
    if (car_on("a7")) car_a7();
    wdog("cases", 240);
    if (car_on("a9")) car_a9();
    wdog("cases", 240);
    if (car_on("a1")) car_a1();
    wdog("cases", 240);
    if (car_on("a4")) car_a4();
    wdog("cases", 240);
    if (car_on("a6")) car_a6();
    wdog("cases", 240);
    if (car_on("a8")) car_a8();
    wdog("cases", 240);
    if (car_on("a10")) car_a10();
    wdog("cases", 240);
    if (car_on("a11")) { extern void enc_carrier_a11(void); enc_carrier_a11(); }
    wdog("cases", 240);
    if (car_on("a12")) car_a12();
    return 0;
}
#else
void enc_carrier_pre_sysinit(void) { }
int enc_carrier_run(void) { return -1; }     /* the SoC API has no GetFd / SetMaxStreamCnt / FlushStream */
#endif
