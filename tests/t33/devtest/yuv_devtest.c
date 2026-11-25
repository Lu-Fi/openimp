/*
 * yuv_devtest - QEMU / device proof of the T32/T33 stand-alone encoder (#752), vendor 2.x headers.
 *
 *   yuv_devtest pattern W H N             host: the N reference NV12 frames (packed, UV at W*H)
 *   yuv_devtest avc|hevc W H N [qp|-kbps] device: N frames through IMP_Encoder_YuvInit / YuvEncode /
 *                                         YuvExit, the Annex-B stream to stdout (qp: FIXQP, -kbps: CBR)
 *   yuv_devtest jpeg W H [q]              device: IMP_Encoder_InputJpege of frame 0 to stdout
 *   yuv_devtest codes                     device: the argument-check return codes, one line each
 *
 * The caller's NV12 is laid out as the vendor's YuvEncode / InputJpege read it: stride W (W % 16 == 0),
 * UV right after H rows.  The pattern is deterministic (gradients, checkers, a moving box), so the host
 * builds the same frames and measures PSNR / SSIM with ffmpeg.  Diagnostics go to stderr.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(HOST_PATTERN)
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_system.h>
/* exported by vendor 2.0.2.1 / 2.2.0 (0x1048ac), not in their header */
int IMP_Encoder_SetChnQpgAI_Yuv(void *h, IMPEncoderQpgAICfg *q);
void IMP_Encoder_SetChnQpgAIEnable_Yuv(int encChn, int enable);
#endif

static void pattern(uint8_t *buf, int w, int h, int n)
{
    uint8_t *uv = buf + (size_t)w * h;
    int bx = (n * 8) % (w - 64), by = (n * 4) % (h - 64), x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int v = (x * 255) / w;

            if (y > h / 2)
                v = ((x / 32 + y / 32) & 1) ? 200 : 40;
            if (y > 3 * h / 4)
                v = 16 + (x % 220);
            if (x >= bx && x < bx + 64 && y >= by && y < by + 64)
                v = 235;
            buf[(size_t)y * w + x] = (uint8_t)v;
        }
    for (y = 0; y < h / 2; y++)
        for (x = 0; x < w / 2; x++) {
            uv[(size_t)y * w + 2 * x] = (uint8_t)(64 + (x * 128) / (w / 2));
            uv[(size_t)y * w + 2 * x + 1] = (uint8_t)(64 + (y * 128) / (h / 2));
        }
}

#if defined(HOST_PATTERN)
int main(int argc, char **argv)
{
    int w, h, n, i;
    uint8_t *buf;

    if (argc < 5)
        return 2;
    w = atoi(argv[2]);
    h = atoi(argv[3]);
    n = atoi(argv[4]);
    buf = calloc(1, (size_t)w * h * 3 / 2);
    for (i = 0; buf && i < n; i++) {
        pattern(buf, w, h, i);
        fwrite(buf, 1, (size_t)w * h * 3 / 2, stdout);
    }
    return 0;
}
#else
static void codes(void)
{
    IMPEncoderYuvIn in;
    void *h = NULL;
    int r;

    memset(&in, 0, sizeof(in));
    in.type = PT_H264;
    in.mode.rcMode = ENC_RC_MODE_FIXQP;
    in.mode.attrH264FixQp.IQp = in.mode.attrH264FixQp.PQp = 30;
    in.outFrmRate.frmRateNum = 25;
    in.outFrmRate.frmRateDen = 1;
    in.maxGop = 25;
    r = IMP_Encoder_YuvInit(&h, 330, 192, &in);
    printf("CODE YuvInit w%%16 0x%x\n", (unsigned)r);
    r = IMP_Encoder_YuvInit(&h, 320, 191, &in);
    printf("CODE YuvInit odd-h 0x%x\n", (unsigned)r);
    in.type = PT_JPEG;
    r = IMP_Encoder_YuvInit(&h, 320, 192, &in);
    printf("CODE YuvInit jpeg 0x%x\n", (unsigned)r);
    in.type = PT_H264;
    in.mode.rcMode = (IMPEncoderRcMode)6;
    r = IMP_Encoder_YuvInit(&h, 320, 192, &in);
    printf("CODE YuvInit rcmode6 0x%x\n", (unsigned)r);
    printf("CODE YuvEncode NULL 0x%x\n", (unsigned)IMP_Encoder_YuvEncode(NULL, (IMPFrameInfo){0}, NULL));
    printf("CODE YuvExit NULL 0x%x\n", (unsigned)IMP_Encoder_YuvExit(NULL));
    printf("CODE InputJpege w%%16 0x%x\n", (unsigned)IMP_Encoder_InputJpege(NULL, NULL, 330, 192, 40, NULL));
    printf("CODE InputJpege odd-h 0x%x\n", (unsigned)IMP_Encoder_InputJpege(NULL, NULL, 320, 191, 40, NULL));
    in.mode.rcMode = ENC_RC_MODE_FIXQP;
    r = IMP_Encoder_YuvInit(&h, 320, 192, &in);
    printf("CODE YuvInit ok 0x%x\n", (unsigned)r);
    if (!r) {
        IMPFrameInfo f;
        IMPEncoderYuvOut eo;
        IMPEncoderQpgAICfg q;
        uint8_t map[20 * 12];

        memset(&f, 0, sizeof(f));
        memset(&eo, 0, sizeof(eo));
        f.width = 320;
        f.height = 180;
        eo.outAddr = map;
        eo.outLen = sizeof(map);
        printf("CODE YuvEncode size 0x%x\n", (unsigned)IMP_Encoder_YuvEncode(h, f, &eo));
        memset(&q, 0, sizeof(q));
        memset(map, 0, sizeof(map));
        q.map = map;
        q.width = 20;
        q.height = 12;
        q.foreBackMode = 1;
        printf("CODE SetChnQpgAI_Yuv 0x%x\n", (unsigned)IMP_Encoder_SetChnQpgAI_Yuv(h, &q));
        printf("CODE YuvExit ok 0x%x\n", (unsigned)IMP_Encoder_YuvExit(h));
    }
}

/* #782: the stand-alone encoder through the multi-process server (vendor 2.0.2.1 _Ex clients) */
static int ex_client(const char *mode, int w, int h, int n, int arg)
{
    uint32_t size = (uint32_t)w * (uint32_t)h * 3u / 2u, cap = size + 65536u;
    uint8_t *pic = IMP_Encoder_VbmAlloc_Ex(size + (uint32_t)w * 16u, 256), *out = IMP_Encoder_VbmAlloc_Ex(cap, 256);
    IMPEncoderYuvIn in;
    IMPEncoderYuvOut eo;
    IMPFrameInfo f;
    int enc = -1, i, r = 0;

    if (!pic || !out) {
        fprintf(stderr, "VbmAlloc_Ex failed\n");
        return 1;
    }
    memset(pic + size, 0, (size_t)w * 16u);
    if (!strcmp(mode, "exjpeg")) {
        int len = 0;

        pattern(pic, w, h, 0);
        r = IMP_Encoder_InputJpege_Ex(pic, out, w, h, n, &len);
        fprintf(stderr, "InputJpege_Ex %dx%d: 0x%x, %d bytes\n", w, h, (unsigned)r, len);
        if (!r && len > 0)
            fwrite(out, 1, (size_t)len, stdout);
        goto done;
    }
    memset(&in, 0, sizeof(in));
    in.type = PT_H264;
    in.mode.rcMode = ENC_RC_MODE_FIXQP;
    in.mode.attrH264FixQp.IQp = in.mode.attrH264FixQp.PQp = (uint32_t)arg;
    in.outFrmRate.frmRateNum = 25;
    in.outFrmRate.frmRateDen = 1;
    in.maxGop = 10;
    r = IMP_Encoder_YuvInit_Ex(&enc, w, h, &in);
    fprintf(stderr, "YuvInit_Ex %dx%d: 0x%x handle %d\n", w, h, (unsigned)r, enc);
    if (r)
        goto done;
    for (i = 0; i < n; i++) {
        pattern(pic, w, h, i);
        memset(&f, 0, sizeof(f));
        f.width = (uint32_t)w;
        f.height = (uint32_t)h;
        f.pixfmt = PIX_FMT_NV12;
        f.size = size;
        f.virAddr = (uint32_t)(uintptr_t)pic;
        f.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)pic);
        f.timeStamp = (int64_t)i * 40000;
        eo.outAddr = out;
        eo.outLen = cap;
        r = IMP_Encoder_YuvEncode_Ex(enc, f, &eo);
        fprintf(stderr, "YuvEncode_Ex %d: 0x%x, %u bytes\n", i, (unsigned)r, eo.outLen);
        if (r)
            break;
        fwrite(out, 1, eo.outLen, stdout);
    }
    fprintf(stderr, "YuvExit_Ex 0x%x\n", (unsigned)IMP_Encoder_YuvExit_Ex(enc));
done:
    IMP_Encoder_VbmFree_Ex(out);
    IMP_Encoder_VbmFree_Ex(pic);
    return r ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    int w = argc > 2 ? atoi(argv[2]) : 640, h = argc > 3 ? atoi(argv[3]) : 360;
    int n = argc > 4 ? atoi(argv[4]) : 10, arg = argc > 5 ? atoi(argv[5]) : 30, i, r;
    uint32_t size = (uint32_t)w * (uint32_t)h * 3u / 2u, cap = size + 65536u;
    uint8_t *pic, *out;
    void *enc = NULL;
    IMPEncoderYuvIn in;
    IMPEncoderYuvOut eo;
    IMPFrameInfo f;

    /* #782 multi-process: 'exsrv SECONDS' = the server (MultiProcessInit, then wait); 'exavc W H N ARG'
     * and 'exjpeg W H Q' = a client process through the _Ex calls (no IMP_System_Init of its own) */
    if (!strcmp(mode, "exsrv")) {
        /* MultiProcessInit before the encoder module is up (after IMP_System_Init it answers 0x80040080) */
        int mp = IMP_Encoder_MultiProcessInit();

        r = IMP_System_Init();
        fprintf(stderr, "MultiProcessInit 0x%x, IMP_System_Init %d\n", (unsigned)mp, r);
        sleep((unsigned)(argc > 2 ? atoi(argv[2]) : 60));
        IMP_Encoder_MultiProcessExit();
        IMP_System_Exit();
        return 0;
    }
    if (!strcmp(mode, "exavc") || !strcmp(mode, "exjpeg"))
        return ex_client(mode, w, h, n, arg);
    r = IMP_System_Init();
    fprintf(stderr, "IMP_System_Init %d\n", r);
    if (!strcmp(mode, "codes")) {
        codes();
        IMP_System_Exit();
        return 0;
    }
    /* #899: 16 zeroed rows behind the picture: the encoder reads whole macroblock rows (a 360-line
     * picture as 368), so what follows the buffer must not change between runs and libraries */
    pic = IMP_Encoder_VbmAlloc(size + (uint32_t)w * 16u, 256);
    if (pic)
        memset(pic + size, 0, (size_t)w * 16u);
    out = IMP_Encoder_VbmAlloc(cap, 256);
    if (!pic || !out) {
        fprintf(stderr, "VbmAlloc failed\n");
        return 1;
    }
    if (!strcmp(mode, "jpeg")) {
        int len = 0;

        pattern(pic, w, h, 0);
        r = IMP_Encoder_InputJpege(pic, out, w, h, argc > 4 ? atoi(argv[4]) : 40, &len);
        fprintf(stderr, "InputJpege %dx%d: 0x%x, %d bytes\n", w, h, (unsigned)r, len);
        if (!r && len > 0)
            fwrite(out, 1, (size_t)len, stdout);
        IMP_System_Exit();
        return r ? 1 : 0;
    }
    /* #781: argv[6] flags: 's' SMART instead of CBR (arg < 0), 'q' QpgAI on with a map
     * (left half of the MB grid foreground 1, right half 0), foreBackMode 1, markLvl 3 */
    const char *fl = argc > 6 ? argv[6] : "";
    if (strchr(fl, 'q'))
        IMP_Encoder_SetChnQpgAIEnable_Yuv(0, 1);
    memset(&in, 0, sizeof(in));
    in.type = !strcmp(mode, "hevc") ? PT_H265 : PT_H264;
    if (arg >= 0) {
        in.mode.rcMode = ENC_RC_MODE_FIXQP;
        in.mode.attrH264FixQp.IQp = in.mode.attrH264FixQp.PQp = (uint32_t)arg;
    } else {
        IMPEncoderCHNAttr ca;           /* the CBR attribute a caller gets from SetDefaultParam */

        memset(&ca, 0, sizeof(ca));
        IMP_Encoder_SetDefaultParam(&ca, in.type == PT_H265 ? IMP_ENC_PROFILE_HEVC_MAIN : IMP_ENC_PROFILE_AVC_MAIN,
                                    strchr(fl, 's') ? ENC_RC_MODE_SMART : ENC_RC_MODE_CBR,
                                    (uint16_t)w, (uint16_t)h, 25, 1, 10, 0, -1,
                                    (uint32_t)-arg);
        in.mode = ca.rcAttr.attrRcMode;
    }
    in.outFrmRate.frmRateNum = 25;
    in.outFrmRate.frmRateDen = 1;
    in.maxGop = 10;
    r = IMP_Encoder_YuvInit(&enc, w, h, &in);
    fprintf(stderr, "YuvInit %s %dx%d: 0x%x\n", mode, w, h, (unsigned)r);
    if (r)
        return 1;
    if (strchr(fl, 'q')) {
        static uint8_t qmap[(1920 / 16) * (1088 / 16)];
        IMPEncoderQpgAICfg q;
        int mw = (w + 15) / 16, mh = (h + 15) / 16, x, y;

        for (y = 0; y < mh; y++)
            for (x = 0; x < mw; x++)
                qmap[y * mw + x] = x < mw / 2;
        memset(&q, 0, sizeof(q));
        q.map = qmap;
        q.width = (uint32_t)mw;
        q.height = (uint32_t)mh;
        q.foreBackMode = 1;
        q.markLvl = 3;
        fprintf(stderr, "SetChnQpgAI_Yuv %dx%d: 0x%x\n", mw, mh, (unsigned)IMP_Encoder_SetChnQpgAI_Yuv(enc, &q));
    }
    for (i = 0; i < n; i++) {
        pattern(pic, w, h, i);
        memset(&f, 0, sizeof(f));
        f.width = (uint32_t)w;
        f.height = (uint32_t)h;
        f.pixfmt = PIX_FMT_NV12;
        f.size = size;
        f.virAddr = (uint32_t)(uintptr_t)pic;
        f.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)pic);
        f.timeStamp = (int64_t)i * 40000;
        eo.outAddr = out;
        eo.outLen = cap;
        r = IMP_Encoder_YuvEncode(enc, f, &eo);
        fprintf(stderr, "YuvEncode %d: 0x%x, %u bytes\n", i, (unsigned)r, eo.outLen);
        if (r)
            break;
        fwrite(out, 1, eo.outLen, stdout);
    }
    fprintf(stderr, "YuvExit 0x%x\n", (unsigned)IMP_Encoder_YuvExit(enc));
    IMP_Encoder_VbmFree(out);
    IMP_Encoder_VbmFree(pic);
    IMP_System_Exit();
    return r ? 1 : 0;
}
#endif
