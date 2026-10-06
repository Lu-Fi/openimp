/* apitest: Encoder area (H.264 / H.265 / JPEG) and the stream verifier */
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
# define YUV_BITRATE 512000    /* IMPEncoderYuvIn: bit/s */
#else
# define BR_SET_UNIT 1
# define YUV_BITRATE BITRATE
#endif
#define BITRATE (512 * BR_UNIT)

static const char *cname[] = { "H264", "H265", "JPEG" };

/* quiet-after-first reporting: first=1 prints the line, later calls print only failures */
#define QR0(f, r, first, ...) do { if ((first) || (r) != 0) RET0(f, r, __VA_ARGS__); } while (0)

/* ----------------------------------------------------------- stream parse */
typedef struct {
    int frames, bytes_max;
    unsigned long bytes;
    int vps, sps, pps, idr, sei, other;
    int w, h, profile, level;           /* from SPS (H.264) or SOF (JPEG) */
    int soi, eoi, jpeg_ok;              /* JPEG frames with SOI..EOI */
    int user;                           /* user data string seen */
    int trunc;
    int first_idr_frame;                /* frame index of the first IDR (-1 none) */
    int last_frame_idr;
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
    if (n >= 8) {                                        /* user data (InsertUserData) */
        for (i = 0; i + 7 <= n; i++) if (!memcmp(b + i, "apitest", 7)) { si->user++; break; }
    }
}

/* Pull up to maxf frames from the channel (polling + get + release). rp: report
 * the Polling/GetStream/ReleaseStream lines for the first frame. stop: SI field
 * condition checked each frame: 0 = none, 1 = until IDR seen, 2 = sps+pps+idr */
static int pull(int chn, int codec, int maxf, int stop, int rp, SI *si, int tmo_ms)
{
    int k;

    memset(si, 0, sizeof(*si));
    si->first_idr_frame = -1;
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
        si->trunc |= trunc;
        scan_frame(g_sbuf, n, codec, si);
        si->frames++;
        si->bytes += n;
        if ((int)n > si->bytes_max) si->bytes_max = (int)n;
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

static int grp_up(int with_osd, int first)
{
    IMPCell fs = { DEV_ID_FS, CH_SUB, 0 }, enc = { DEV_ID_ENC, GRP, 0 }, osd = { DEV_ID_OSD, GRP, 0 };
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
        QR0(IMP_System_Bind, r, first, "FS ch%d -> OSD group %d", CH_SUB, GRP);
        if (r) return -1;
        r = IMP_System_Bind(&osd, &enc);
        QR0(IMP_System_Bind, r, 0, "OSD -> ENC group %d", GRP);
        if (r) return -1;
        g_osd = 1;
    } else {
        r = IMP_System_Bind(&fs, &enc);
        QR0(IMP_System_Bind, r, first, "FS ch%d -> ENC group %d", CH_SUB, GRP);
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
    IMPCell fs = { DEV_ID_FS, CH_SUB, 0 }, enc = { DEV_ID_ENC, GRP, 0 }, osd = { DEV_ID_OSD, GRP, 0 };
    int r;

    if (g_osd) {
        r = IMP_System_UnBind(&osd, &enc); QR0(IMP_System_UnBind, r, 0, "OSD -> ENC");
        r = IMP_System_UnBind(&fs, &osd); QR0(IMP_System_UnBind, r, first, "FS -> OSD");
        if (IMP_OSD_DestroyGroup) { r = IMP_OSD_DestroyGroup(GRP); QR0(IMP_OSD_DestroyGroup, r, first, "group %d", GRP); }
        g_osd = 0;
    } else {
        r = IMP_System_UnBind(&fs, &enc); QR0(IMP_System_UnBind, r, first, "FS ch%d -> ENC group %d", CH_SUB, GRP);
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
    NEED(IMP_Encoder_SetPool) { r = IMP_Encoder_SetPool(chn, 0); QR0(IMP_Encoder_SetPool, r, first, "chn %d pool 0 (before CreateChn)", chn); }
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
    /* frame rate */
    NEED(IMP_Encoder_SetChnFrmRate) {
        G(IMPEncoderFrmRate, f);
        IMPEncoderFrmRate nf = { 10, 1 }, of = { EFPS, 1 };
        int r2;
        int64_t t0, t1;

        r = IMP_Encoder_SetChnFrmRate(chn, &nf);
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
        IMP_Encoder_SetChnFrmRate(chn, &of);
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

            *rc_bitrate(rc, codec) = want;
            r2 = IMP_Encoder_SetChnAttrRcMode(chn, rc);
            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
            CHECK(IMP_Encoder_SetChnAttrRcMode, r2, r == 0 && *rc_bitrate(rc2, codec) == want, "bitrate 384 kbit/s, read back %u", *rc_bitrate(rc2, codec));
            *rc_bitrate(rc, codec) = (uint32_t)BITRATE;
            IMP_Encoder_SetChnAttrRcMode(chn, rc);
            gfree(rc2);
        }
#endif
#if HAS_IMP_Encoder_SetChnBitRate
        NEED(IMP_Encoder_SetChnBitRate) {
            G(IMPEncoderAttrRcMode, rc2);
            int r2 = IMP_Encoder_SetChnBitRate(chn, 400 * BR_SET_UNIT, 400 * BR_SET_UNIT);

            r = IMP_Encoder_GetChnAttrRcMode(chn, rc2); gchk(rc2);
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

        n.gopsize = 25;
        r = IMP_Encoder_SetGOPSize(chn, &n);
        NEED(IMP_Encoder_GetGOPSize) {
            r2 = IMP_Encoder_GetGOPSize(chn, g); gchk(g);
            CHECK(IMP_Encoder_SetGOPSize, r, r2 == 0 && g->gopsize == 25, "set 25, read back %d", g->gopsize);
            CHECK(IMP_Encoder_GetGOPSize, r2, g->gopsize == 25, "%d", g->gopsize);
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

        r = IMP_Encoder_GetChnGopAttr(chn, g); gchk(g);
        CHECK(IMP_Encoder_GetChnGopAttr, r, g->uGopLength == EGOP, "mode 0x%x length %u (created %d) maxSameSence %u", (unsigned)g->uGopCtrlMode, g->uGopLength, EGOP, g->uMaxSameSenceCnt);
        NEED(IMP_Encoder_SetChnGopLength) {
            r2 = IMP_Encoder_SetChnGopLength(chn, 25);
            r = IMP_Encoder_GetChnGopAttr(chn, g2); gchk(g2);
            CHECK(IMP_Encoder_SetChnGopLength, r2, r == 0 && g2->uGopLength == 25, "25, read back %u", g2->uGopLength);
        }
        NEED(IMP_Encoder_SetChnGopAttr) {
            r2 = IMP_Encoder_SetChnGopAttr(chn, g);
            r = IMP_Encoder_GetChnGopAttr(chn, g2); gchk(g2);
            CHECK(IMP_Encoder_SetChnGopAttr, r2, r == 0 && g2->uGopLength == g->uGopLength && g2->uGopCtrlMode == g->uGopCtrlMode, "restore attr, length %u", g2->uGopLength);
        }
        gfree(g); gfree(g2);
    }
#endif
    /* QP bounds etc. (new API) */
#if HAS_IMP_Encoder_SetChnQpBounds
    NEED(IMP_Encoder_SetChnQpBounds) {
        r = IMP_Encoder_SetChnQpBounds(chn, 22, 44);
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
    /* ROI */
#if HAS_IMP_Encoder_SetChnROI
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
            CHECK(IMP_Encoder_SetChnROI, r, r2 == 0 && g->bEnable && g->rect.p0.x == 64 && g->rect.p1.x == 319 && g->s32Qp == -4,
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
            CHECK(IMP_Encoder_SetChnRoiAttr, r, r2 == 0 && g->st_roi[0].enable && g->st_roi[0].rect.w == 128 && g->st_roi[0].qp == -4,
                  "win0 (64,64) 128x96 dQP -4, read back en %d %ux%u qp %d", g->st_roi[0].enable, g->st_roi[0].rect.w, g->st_roi[0].rect.h, g->st_roi[0].qp);
            CHECK(IMP_Encoder_GetChnRoiAttr, r2, g->st_roi[0].enable, "win0 enabled %d", g->st_roi[0].enable);
        }
        pull(chn, codec, 3, 0, 0, &si, 2000);
        rep(LBL("stream with ROI enabled"), 0, si.frames == 3 ? V_PASS : V_FAIL, "%d frames after enabling the ROI", si.frames);
        c->st_roi[0].enable = false; IMP_Encoder_SetChnRoiAttr(chn, c);
        gfree(g); gfree(c);
    }
    rep(FN(IMP_Encoder_SetChnMapRoi), 0, V_SKIP, "needs a per-macroblock QP map sized for the channel");
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
    NEED(IMP_Encoder_SetChnMaxPictureSize) { r = IMP_Encoder_SetChnMaxPictureSize(chn, 60000, 40000); RET0(IMP_Encoder_SetChnMaxPictureSize, r, "I 60000 P 40000 (no getter)"); }
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
#if defined(PLATFORM_T20)
        n.superFrmMode = IMP_RC_SUPERFRM_REENCODE; n.superIFrmBitsThr = 400000; n.superPFrmBitsThr = 200000; n.superBFrmBitsThr = 100000;   /* T20 has no discard */
#else
        n.superFrmMode = IMP_RC_SUPERFRM_DISCARD; n.superIFrmBitsThr = 400000; n.superPFrmBitsThr = 200000; n.superBFrmBitsThr = 100000;
        n.rcPriority = IMP_RC_PRIORITY_BITRATE_FIRST;
#endif
        n.rcPriority = IMP_RC_PRIORITY_BITRATE_FIRST;
        r2 = IMP_Encoder_SetSuperFrameCfg(chn, &n);
        r = IMP_Encoder_GetSuperFrameCfg ? IMP_Encoder_GetSuperFrameCfg(chn, g) : -1; gchk(g);
        if (r2 != 0 && REFUSES_UNSUPPORTED)
            rep(FN(IMP_Encoder_SetSuperFrameCfg), r2, V_NA, "mode %d refused: the Helix eprc controller of OpenIMP has no frame discard (T20: re-encode only, T21: none)", (int)n.superFrmMode);
        else
        CHECK(IMP_Encoder_SetSuperFrameCfg, r2, r == 0 && g->superFrmMode == n.superFrmMode && g->superIFrmBitsThr == n.superIFrmBitsThr && g->rcPriority == n.rcPriority,
              "mode %d I %u P %u, read back mode %d I %u P %u prio %d", (int)n.superFrmMode, n.superIFrmBitsThr, n.superPFrmBitsThr, (int)g->superFrmMode, g->superIFrmBitsThr, g->superPFrmBitsThr, (int)g->rcPriority);
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
        CHECK(IMP_Encoder_SetChnDenoise, r2, r == 0 && g->enable && g->dnType == 1 && g->dnIQp == 2 && g->dnPQp == 3,
              "en type 1 IQp 2 PQp 3, read back en %d type %d %d/%d", g->enable, g->dnType, g->dnIQp, g->dnPQp);
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
#  ifdef PLATFORM_T23
    /* stock T23 1.3.0: the getter has no read-back and fails on a created channel */
    NEED(IMP_Encoder_SetChangeRef) {
        int r2 = IMP_Encoder_SetChangeRef(chn, 1);
        int g = 0, r3 = IMP_Encoder_GetChangeRef ? IMP_Encoder_GetChangeRef(chn, &g) : -1;
        RET0(IMP_Encoder_SetChangeRef, r2, "ChangeRef set 1 (T23 stock: no read-back)");
        rep(FN(IMP_Encoder_GetChangeRef), r3, r3 == -1 ? V_PASS : V_FAIL, "stock T23 getter fails on a created channel: ret %d", r3);
        IMP_Encoder_SetChangeRef(chn, 0);
    }
#  else
    RT_INT(IMP_Encoder_SetChangeRef, IMP_Encoder_GetChangeRef, 1, "ChangeRef")
#  endif
# endif
# if HAS_IMP_Encoder_SetQpgMode
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

        r = IMP_Encoder_InsertUserData(chn, ud, (uint32_t)strlen(ud));
        pull(chn, codec, 30, 0, 0, &si, 2000);
        CHECK(IMP_Encoder_InsertUserData, r, si.user > 0, "\"%s\": %s in the next %d frames (SEI)", ud, si.user ? "found" : "NOT found", si.frames);
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
# if HAS_IMP_Encoder_SetChnInitQP
    NEED(IMP_Encoder_SetChnInitQP) { r = IMP_Encoder_SetChnInitQP(chn, 30); RET0(IMP_Encoder_SetChnInitQP, r, "qp 30 (no getter)"); }
# endif
# if HAS_IMP_Encoder_SetMultiSectionMode
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
# if defined(PLATFORM_T23) || defined(ENC_NEW)
    /* stock T23/T31: a creation-time option, SetFisheyeEnableStatus on a created channel fails */
    if (codec != CODEC_JPEG) NEED(IMP_Encoder_SetFisheyeEnableStatus) {
        int r2 = IMP_Encoder_SetFisheyeEnableStatus(chn, 1);
        rep(FN(IMP_Encoder_SetFisheyeEnableStatus), r2, r2 == -1 ? V_PASS : V_FAIL, "stock refuses it on a created channel: ret %d", r2);
    }
# else
    if (codec != CODEC_JPEG) { RT_INT(IMP_Encoder_SetFisheyeEnableStatus, IMP_Encoder_GetFisheyeEnableStatus, 1, "fisheye") }
# endif
#endif
    NEED(IMP_Encoder_FlushStream) {
        r = IMP_Encoder_FlushStream(chn);
        pull(chn, codec, 3, 0, 0, &si, 2000);
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
    NEED(IMP_Encoder_GetPool) { r = IMP_Encoder_GetPool(chn); rep(FN(IMP_Encoder_GetPool), r, r == 0 ? V_PASS : V_FAIL, "chn %d pool %d (set 0)", chn, r); }
#endif
#if HAS_IMP_Encoder_GetStreamBufSize
    NEED(IMP_Encoder_GetStreamBufSize) {
        G(uint32_t, g);
        r = IMP_Encoder_GetStreamBufSize(chn, g); gchk(g);
        CHECK(IMP_Encoder_GetStreamBufSize, r, *g == 262144, "%u (set 262144)", *g);
        gfree(g);
    }
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
                else CHECK(IMP_Encoder_GetChnEvalInfo, r < 0 ? r : 0, r >= 0, "ret %d, info buffer %s (4 KB guarded)", r, gtouched(info) ? "written" : "untouched");
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
static unsigned long jpeg_avg(int chn, int n)
{
    SI si;

    if (pull(chn, CODEC_JPEG, n, 0, 0, &si, 2000) < 0 || si.frames < n || si.jpeg_ok < n) return 0;
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
        int r2, r3, i, n_t = (int)sizeof(n->qmem_table) < 128 ? (int)sizeof(n->qmem_table) : 128;

        r3 = IMP_Encoder_GetJpegeQl ? IMP_Encoder_GetJpegeQl(jchn, o) : -1; gchk(o);
        rep(FN(IMP_Encoder_GetJpegeQl), r3, r3 == 0 ? V_PASS : V_FAIL, "initial user_ql_en %d table[0] %u [64] %u", o->user_ql_en, o->qmem_table[0], o->qmem_table[64]);
        n->user_ql_en = true;
        for (i = 0; i < n_t; i++) n->qmem_table[i] = 2;            /* fine quantizer: big pictures */
        r2 = IMP_Encoder_SetJpegeQl(jchn, n);
        pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000);
        hi = jpeg_avg(jchn, 4);
        r = IMP_Encoder_GetJpegeQl ? IMP_Encoder_GetJpegeQl(jchn, g) : -1; gchk(g);
        CHECK(IMP_Encoder_SetJpegeQl, r2, r == 0 && g->user_ql_en && g->qmem_table[0] == 2 && g->qmem_table[64] == 2, "table all 2, read back en %d [0] %u [64] %u", g->user_ql_en, g->qmem_table[0], g->qmem_table[64]);
        for (i = 0; i < n_t; i++) n->qmem_table[i] = 90;           /* coarse quantizer: small pictures */
        r2 = IMP_Encoder_SetJpegeQl(jchn, n);
        pull(jchn, CODEC_JPEG, 2, 0, 0, &si, 2000);
        lo = jpeg_avg(jchn, 4);
        rep(LBL("JPEG size follows SetJpegeQl live"), r2, (hi && lo && hi > lo + lo / 3) ? V_PASS : V_FAIL,
            "table 2 -> avg %lu bytes, table 90 -> avg %lu bytes (needs table 2 clearly bigger)", hi, lo);
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
#if HAS_IMP_Encoder_VbmAlloc
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
#if HAS_IMP_Encoder_VbmAlloc_Ex
    rep(FN(IMP_Encoder_VbmAlloc_Ex), 0, V_SKIP, "_Ex variants (second VPU instance set) not exercised");
    rep(FN(IMP_Encoder_VbmFree_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_InputJpege_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvInit_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvEncode_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
    rep(FN(IMP_Encoder_YuvExit_Ex), 0, V_SKIP, "see VbmAlloc_Ex");
#endif
#if HAS_IMP_Encoder_SetAvpuBsShare
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
#ifdef HAVE_H265
# if defined(PLATFORM_T21) || defined(PLATFORM_T23)
    /* the Helix encoder of T21/T23 is H.264/JPEG only: the headers declare H.265 types, the silicon has no HEVC core */
    rep(LBL("H.265 stream"), -1, V_NA, "no HEVC encoder in the T21/T23 silicon (OpenIMP CreateChn refuses it so callers fall back to H.264)");
# else
    if (grp_up(0, 0) == 0) {
        if (chn_up(0, CODEC_H265, 1, 1) == 0) {
            pull(0, CODEC_H265, 60, 2, 0, &si, 3000);
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

int enc_pull_h264(int n)        /* frames still flowing? */
{
    SI si;

    pull(0, CODEC_H264, n, 0, 0, &si, 3000);
    return si.frames;
}
