/*
 * inject_devtest - device proof of IMP_FrameSource_ExternInject_* with
 * DequeueBuffer/QueueBuffer: an injected NV12 channel bound to an H.264
 * encoder channel, like a sensor channel.  Built against the VENDOR headers
 * (T41 1.2.6 / T40 1.3.1) as an application would be.
 *
 *   inject_devtest W H N   N frames of the yuv_devtest pattern, H.264 to stdout
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_encoder.h>
#include <imp/imp_framesource.h>
#include <imp/imp_system.h>

static int W, H, N, CH = 1;

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

static void *feeder(void *arg)
{
    int i, waits = 0;

    (void)arg;
    for (i = 0; i < N + 4; i++) {
        IMPFrameInfo *f = NULL;

        while (IMP_FrameSource_DequeueBuffer(CH, &f) != 0) {
            usleep(5000);
            if (++waits > 2000)
                return NULL;
        }
        pattern((uint8_t *)(uintptr_t)f->virAddr, W, H,
                (int)(f->size / ((unsigned)W * 3 / 2)), i);
        f->timeStamp = (int64_t)(i + 1) * 40000;
        if (i == 0)
            fprintf(stderr, "dequeued frame: index %d %ux%u size %u phys 0x%08x\n",
                    f->index, f->width, f->height, f->size, f->phyAddr);
        if (IMP_FrameSource_QueueBuffer(CH, f) != 0)
            fprintf(stderr, "QueueBuffer %d failed\n", i);
    }
    fprintf(stderr, "feeder: %d frames queued, %d waits\n", i, waits);
    return NULL;
}

int main(int argc, char **argv)
{
    IMPFSChnAttr fs;
    IMPEncoderChnAttr enc;
    IMPCell src = { DEV_ID_FS, 1, 0 }, dst = { DEV_ID_ENC, 0, 0 };
    pthread_t thread;
    int i, got = 0, r;
    long bytes = 0;

    if (argc < 4)
        return 2;
    W = atoi(argv[1]);
    H = atoi(argv[2]);
    N = atoi(argv[3]);
    memset(&fs, 0, sizeof(fs));
    fs.pixFmt = PIX_FMT_NV12;
    fs.picWidth = W;
    fs.picHeight = H;
    fs.outFrmRateNum = 25;
    fs.outFrmRateDen = 1;
    fs.nrVBs = 3;
    fprintf(stderr, "ExternInject_CreateChn(7): %d (expected -1)\n",
            IMP_FrameSource_ExternInject_CreateChn(7, &fs));
    r = IMP_FrameSource_ExternInject_CreateChn(CH, &fs);
    fprintf(stderr, "ExternInject_CreateChn(%d): %d\n", CH, r);
    if (r || IMP_FrameSource_ExternInject_EnableChn(CH)) {
        fprintf(stderr, "inject setup failed\n");
        return 1;
    }
    if (IMP_Encoder_CreateGroup(0) ||
        IMP_Encoder_SetDefaultParam(&enc, IMP_ENC_PROFILE_AVC_HIGH,
                                    IMP_ENC_RC_MODE_CBR, W, H, 25, 1, 25, 2,
                                    -1, 4000) ||
        IMP_Encoder_CreateChn(0, &enc) || IMP_Encoder_RegisterChn(0, 0) ||
        IMP_System_Bind(&src, &dst) || IMP_Encoder_StartRecvPic(0)) {
        fprintf(stderr, "encoder setup failed\n");
        return 1;
    }
    pthread_create(&thread, NULL, feeder, NULL);
    for (i = 0; got < N && i < N * 4; i++) {
        IMPEncoderStream s;
        uint32_t k;

        if (IMP_Encoder_PollingStream(0, 2000) != 0)
            continue;
        if (IMP_Encoder_GetStream(0, &s, 1) != 0)
            continue;
        for (k = 0; k < s.packCount; k++) {
            uint32_t off = s.pack[k].offset, len = s.pack[k].length;
            uint32_t first = off + len > s.streamSize ? s.streamSize - off : len;

            fwrite((uint8_t *)(uintptr_t)s.virAddr + off, 1, first, stdout);
            if (first < len)
                fwrite((uint8_t *)(uintptr_t)s.virAddr, 1, len - first, stdout);
            bytes += len;
        }
        if (got < 2 || got == N - 1)
            fprintf(stderr, "stream %d: %u packs, ts %lld\n", got, s.packCount,
                    (long long)s.pack[0].timestamp);
        IMP_Encoder_ReleaseStream(0, &s);
        got++;
    }
    fprintf(stderr, "encoded %d streams, %ld bytes\n", got, bytes);
    IMP_Encoder_StopRecvPic(0);
    pthread_join(thread, NULL);
    IMP_System_UnBind(&src, &dst);
    IMP_Encoder_UnRegisterChn(0);
    IMP_Encoder_DestroyChn(0);
    IMP_Encoder_DestroyGroup(0);
    fprintf(stderr, "DisableChn %d DestroyChn %d\n",
            IMP_FrameSource_ExternInject_DisableChn(CH),
            IMP_FrameSource_ExternInject_DestroyChn(CH));
    return got == N ? 0 : 1;
}
