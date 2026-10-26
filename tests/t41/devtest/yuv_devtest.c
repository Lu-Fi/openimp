/*
 * yuv_devtest - device proof of the T40/T41 unbound encoder and JPEG calls.
 *
 *   yuv_devtest pattern W H N          host: the N reference NV12 frames
 *   yuv_devtest avc|hevc W H N [kbps]  device: N frames through
 *                                      IMP_Encoder_YuvInit/YuvEncode, the
 *                                      Annex-B stream to stdout
 *   yuv_devtest packed W H N           as avc, packed NV12 (chroma at W*H)
 *   yuv_devtest jpeg W H               device (T41): IMP_Encoder_InputJpege of
 *                                      frame 0 to stdout
 *   yuv_devtest yjpeg W H              device: JPEG through YuvInit type JPEG
 *
 * The pattern is deterministic (gradients, bars, a moving box), so the host
 * builds the same frames and compares with ffmpeg's SSIM.  Diagnostics go to
 * stderr; nothing is written to the camera's /tmp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(HOST_PATTERN)
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#endif

/* NV12 frame n, luma pitch W, luma lines `lines` (>= H), chroma after them */
static void pattern(uint8_t *buf, int w, int h, int lines, int n)
{
    int x, y;
    uint8_t *uv = buf + (size_t)w * lines;
    int bx = (n * 8) % (w - 64), by = (n * 4) % (h - 64);

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
    for (i = 0; i < n; i++) {
        pattern(buf, w, h, h, i);
        fwrite(buf, 1, (size_t)w * h * 3 / 2, stdout);
    }
    return 0;
}
#else
int main(int argc, char **argv)
{
    const char *mode;
    int w, h, n, i, kbps, packed, lines;
    uint32_t size, cap;
    uint8_t *pic, *out;
    void *enc = NULL;
    IMPEncoderYuvIn in;
    IMPEncoderYuvOut eo;
    IMPFrameInfo f;
    long total = 0;

    if (argc < 4)
        return 2;
    mode = argv[1];
    w = atoi(argv[2]);
    h = atoi(argv[3]);
    n = argc > 4 ? atoi(argv[4]) : 1;
    kbps = argc > 5 ? atoi(argv[5]) : 4000;
    packed = !strcmp(mode, "packed");
    lines = packed ? h : (h + 15) & ~15;
    size = (uint32_t)w * lines * 3 / 2;
    pic = IMP_Encoder_VbmAlloc(size, 4096);
    if (!pic) {
        fprintf(stderr, "VbmAlloc %u failed\n", size);
        return 1;
    }
    fprintf(stderr, "VbmAlloc %p phys 0x%08lx P2V ok %d\n", (void *)pic,
            (unsigned long)IMP_Encoder_VbmV2P((intptr_t)pic),
            IMP_Encoder_VbmP2V(IMP_Encoder_VbmV2P((intptr_t)pic)) ==
                (intptr_t)pic);
    cap = (uint32_t)w * h * 2 + 0x10000;
    out = malloc(cap);
#if defined(PLATFORM_T41)
    if (!strcmp(mode, "jpeg")) {
        int len = 0, r;

        pattern(pic, w, h, h, 0);       /* InputJpege: chroma at w*h */
        r = IMP_Encoder_InputJpege(pic, out, w, h, 90, &len);
        fprintf(stderr, "InputJpege %dx%d -> %d len %d\n", w, h, r, len);
        if (r == 0)
            fwrite(out, 1, (size_t)len, stdout);
        IMP_Encoder_VbmFree(pic);
        return r != 0;
    }
#endif
    memset(&in, 0, sizeof(in));
    in.type = !strcmp(mode, "hevc") ? IMP_ENC_TYPE_HEVC :
              !strcmp(mode, "yjpeg") ? IMP_ENC_TYPE_JPEG : IMP_ENC_TYPE_AVC;
    in.mode = IMP_ENC_RC_MODE_CBR;
    in.frameRate = 25;
    in.gopLength = 25;
    in.targetBitrate = (uint32_t)kbps;
    in.maxBitrate = (uint32_t)kbps * 4 / 3;
    in.initQp = in.type == IMP_ENC_TYPE_JPEG ? 90 : -1;
    in.minQp = 15;
    in.maxQp = 45;
    if (IMP_Encoder_YuvInit(&enc, w, h, &in) != 0 || !enc) {
        fprintf(stderr, "YuvInit %dx%d type %d failed\n", w, h, (int)in.type);
        return 1;
    }
    for (i = 0; i < n; i++) {
        pattern(pic, w, h, lines, i);
        memset(&f, 0, sizeof(f));
        f.width = (uint32_t)w;
        f.height = (uint32_t)h;
        f.pixfmt = 0x3231564e;
        f.size = packed ? (uint32_t)w * h * 3 / 2 : size;
        f.phyAddr = (uint32_t)IMP_Encoder_VbmV2P((intptr_t)pic);
        f.virAddr = (uint32_t)(uintptr_t)pic;
        f.timeStamp = (int64_t)i * 40000;
        eo.outAddr = out;
        eo.outLen = cap;
        if (IMP_Encoder_YuvEncode(enc, f, &eo) != 0) {
            fprintf(stderr, "YuvEncode frame %d failed\n", i);
            break;
        }
        total += eo.outLen;
        if (i < 3 || i == n - 1)
            fprintf(stderr, "frame %d: %u bytes, nal 0x%02x\n", i, eo.outLen,
                    eo.outLen > 4 ? out[4] : 0);
        fwrite(out, 1, eo.outLen, stdout);
    }
    fprintf(stderr, "%s %dx%d: %d frames, %ld bytes (%.0f kbit/s at 25 fps)\n",
            mode, w, h, i, total, i ? total * 8.0 * 25 / i / 1000 : 0.0);
    fprintf(stderr, "YuvExit %d, YuvExit(NULL) %d\n", IMP_Encoder_YuvExit(enc),
            IMP_Encoder_YuvExit(NULL));
    IMP_Encoder_VbmFree(pic);
    free(out);
    return i == n ? 0 : 1;
}
#endif
