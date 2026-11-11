/*
 * T32 IMP_Decoder probe for QEMU -M ingenic-t32 (#301, planned).  Built like
 * t32_trace_sample.c (PLATFORM_T32, OpenIMP public headers) and installed
 * under that name by tools/t32/qemu-ioctl-trace.sh (SAMPLE_SRC=this file).
 * The IMP_Decoder_* entry points are looked up at run time (dlsym), so the
 * same binary runs on the vendor libimp 2.0.0 (the T32Pro SDK, the only
 * vendor T32 libimp with a decoder; 2.2.0 has none) and on OpenIMP.
 *
 *   t32_trace_sample dec N   ISP up + System_Init (T32_DEC_NOISP=1: System
 *                 only), decoder channel 0 (T32_DEC_W x T32_DEC_H, default
 *                 1920x1080; T32_DEC_FMT IMPPixelFormat, default 31 =
 *                 BAYER_RGGB8; nrKeepStream T32_DEC_KEEP, default 2), then the
 *                 files /usr/share/t32/dec-<i>.jpg, i = 0..N-1, each sent,
 *                 polled, got and released.  Lines "T32D frame <i> ..." with
 *                 the frame fields and the CRC-32 of the decoded rows
 *                 (2 * height rows of width bytes at the frame's stride:
 *                 align128(width)); T32_STREAM_DUMP=<file>: the first
 *                 T32_DEC_DUMPN (default 2) frames from T32_DEC_DUMP0 (default
 *                 0) appended to the file
 *                 and copied to physical 0x7800000 after the teardown
 *                 (pmemsave on the host; pictures stay local).
 *   t32_trace_sample deccheck  argument checks: return codes only
 *                 ("T32D chk <name> <rc>")
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>

typedef struct {                /* vendor 2.0.0 imp_decoder.h */
    int decType;
    uint32_t maxWidth, maxHeight;
    int pixelFormat;
    uint32_t nrKeepStream, frmRateNum, frmRateDen;
} DecAttr;
typedef struct {
    int i_payload;
    uint8_t *p_payload;
    int64_t timeStamp;
} DecStream;

static int (*dCreate)(int, const DecAttr *);
static int (*dDestroy)(int);
static int (*dStart)(int);
static int (*dStop)(int);
static int (*dSend)(int, DecStream *, uint32_t);
static int (*dPoll)(int, uint32_t);
static int (*dGet)(int, IMPFrameInfo **);
static int (*dRelease)(int, IMPFrameInfo *);

#define STEP(x) step(#x, (x))
static int step(const char *what, int rc)
{
    printf("T32S %s = %d\n", what, rc);
    fflush(stdout);
    return rc;
}

static int envi(const char *name, int def)
{
    const char *v = getenv(name);
    return v ? (int)strtol(v, NULL, 0) : def;
}

static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static int lookup(void)
{
#define L(v, n) if (!(*(void **)&v = dlsym(RTLD_DEFAULT, n))) { printf("T32D no %s\n", n); return -1; }
    L(dCreate, "IMP_Decoder_CreateChn") L(dDestroy, "IMP_Decoder_DestroyChn")
    L(dStart, "IMP_Decoder_StartRecvPic") L(dStop, "IMP_Decoder_StopRecvPic")
    L(dSend, "IMP_Decoder_SendStreamTimeout") L(dPoll, "IMP_Decoder_PollingFrame")
    L(dGet, "IMP_Decoder_GetFrame") L(dRelease, "IMP_Decoder_ReleaseFrame")
#undef L
    return 0;
}

static IMPSensorInfo si;
static int isp_up(void)
{
    if (getenv("T32_DEC_NOISP"))
        return STEP(IMP_System_Init());
    memset(&si, 0, sizeof(si));
    snprintf(si.name, sizeof si.name, "cv8001");
    si.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(si.i2c.type, sizeof si.i2c.type, "cv8001");
    si.i2c.addr = 0x35;
    si.rst_gpio = si.pwdn_gpio = si.power_gpio = -1;
    si.switch_gpio = -1;
    if (STEP(IMP_ISP_Open()) || STEP(IMP_ISP_AddSensor(IMPVI_MAIN, &si)) ||
        STEP(IMP_ISP_EnableSensor(IMPVI_MAIN, &si)) || STEP(IMP_System_Init()))
        return -1;
    return 0;
}

static void isp_down(void)
{
    STEP(IMP_System_Exit());
    if (getenv("T32_DEC_NOISP"))
        return;
    STEP(IMP_ISP_DisableSensor(IMPVI_MAIN));
    STEP(IMP_ISP_DelSensor(IMPVI_MAIN, &si));
    STEP(IMP_ISP_Close());
}

static void copy_out(void)
{
    const char *path = getenv("T32_STREAM_DUMP");
    FILE *f = path ? fopen(path, "rb") : NULL;
    long size;
    int fd;
    void *m;

    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    m = fd < 0 || size <= 0 ? MAP_FAILED :
        mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x7800000);
    if (m != MAP_FAILED) {
        size_t n = fread(m, 1, (size_t)size, f);
        msync(m, (size_t)size, MS_SYNC);
        munmap(m, (size_t)size);
        printf("T32J out %ld copied %zu\n", size, n);
    }
    if (fd >= 0)
        close(fd);
    fclose(f);
}

static uint8_t *load(int i, int *len)
{
    char path[64];
    FILE *f;
    uint8_t *b;
    long n;

    snprintf(path, sizeof path, "/usr/share/t32/dec-%d.jpg", i);
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (int)n;
    return b;
}

static DecAttr attr_default(void)
{
    DecAttr a = { 0, (uint32_t)envi("T32_DEC_W", 1920), (uint32_t)envi("T32_DEC_H", 1080),
                  envi("T32_DEC_FMT", PIX_FMT_BAYER_RGGB8), (uint32_t)envi("T32_DEC_KEEP", 2), 25, 1 };
    return a;
}

static int dec(int n)
{
    DecAttr a = attr_default();
    FILE *dump = getenv("T32_STREAM_DUMP") ? fopen(getenv("T32_STREAM_DUMP"), "wb") : NULL;
    int dumpn = envi("T32_DEC_DUMPN", 2), dump0 = envi("T32_DEC_DUMP0", 0), ok = 0;

    if (STEP(dCreate(0, &a)) || STEP(dStart(0)))
        goto out;
    for (int i = 0; i < n; i++) {
        IMPFrameInfo *f = NULL;
        DecStream st;
        uint8_t *b = load(i, &st.i_payload);
        uint32_t crc = 0, w, h, stride;

        if (!b) {
            printf("T32D no file %d\n", i);
            break;
        }
        st.p_payload = b;
        st.timeStamp = 1000000LL * (i + 1);
        int rs = dSend(0, &st, 2000);
        free(b);
        int rp = rs ? -9 : dPoll(0, 5000);
        int rg = rp ? -9 : dGet(0, &f);
        printf("T32D send %d %d poll %d get %d\n", i, rs, rp, rg);
        if (rg || !f)
            continue;
        w = f->width;   /* the vendor's: align128 */
        h = f->height;
        stride = (w + 127) & ~127u;
        uint32_t vw = (uint32_t)envi("T32_DEC_VW", (int)a.maxWidth);
        for (uint32_t l = 0; l < 2 * h; l++)
            crc = crc32_upd(crc, (const uint8_t *)(uintptr_t)f->virAddr + (size_t)l * stride, vw);
        printf("T32D frame %d idx %d pool %d w %u h %u fmt %u size %u pa %s ts %lld crc %08x\n",
               i, f->index, f->pool_idx, f->width, f->height, f->pixfmt, f->size,
               f->phyAddr ? "set" : "0", (long long)f->timeStamp, crc);
        if (dump && i >= dump0 && i < dump0 + dumpn)
            for (uint32_t l = 0; l < 2 * h; l++)
                fwrite((const uint8_t *)(uintptr_t)f->virAddr + (size_t)l * stride, 1, vw, dump);
        STEP(dRelease(0, f));
        ok++;
    }
    STEP(dStop(0));
    STEP(dDestroy(0));
out:
    if (dump)
        fclose(dump);
    return ok;
}

static void check(void)
{
    DecAttr a = attr_default(), b;
    IMPFrameInfo *f = NULL;
    DecStream st = { 16, (uint8_t *)"0123456789abcdef", 0 };

    /* the vendor leaves a failed CreateChn's channel half set up (later calls
     * see it as created, GetFrame crashes): the refused creates come last */
#define C(name, x) printf("T32D chk %s %d\n", name, (x)); fflush(stdout)
    C("create", dCreate(0, &a));
    C("create-again", dCreate(0, &a));
    C("poll-empty", dPoll(0, 10));
    C("get-empty", dGet(0, &f));
    C("send-short", dSend(0, &st, 10));
    C("poll-short", dPoll(0, 3000));
    C("start", dStart(0));
    C("poll-short2", dPoll(0, 3000));
    C("get-short", dGet(0, &f));
    if (f) {
        printf("T32D chk frame w %u h %u fmt %u size %u\n", f->width, f->height, f->pixfmt, f->size);
        C("release", dRelease(0, f));
    }
    C("stop", dStop(0));
    C("stop-again", dStop(0));
    C("destroy", dDestroy(0));
    C("destroy-again", dDestroy(0));
    C("start-nochn", dStart(0));
    C("stop-nochn", dStop(0));
    C("send-nochn", dSend(0, &st, 10));
    C("poll-nochn", dPoll(0, 10));
    C("get-nochn", dGet(0, &f));
    C("release-nochn", dRelease(0, f));
    C("start-chn1", dStart(1));
    C("create-null", dCreate(0, NULL));
    C("create-chn1", dCreate(1, &a));
    b = a; b.decType = 1;  C("create-h264", dCreate(0, &b));
    b = a; b.pixelFormat = PIX_FMT_NV12; C("create-nv12", dCreate(0, &b));
    C("create-after-refused", dCreate(0, &a));
#undef C
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "dec";
    int n = argc > 2 ? atoi(argv[2]) : 1, ok = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (lookup() || isp_up()) {
        printf("T32S end ok=0\n");
        return 1;
    }
    if (!strcmp(mode, "deccheck")) {
        check();
        ok = 1;
    } else {
        ok = dec(n) == n;
    }
    isp_down();
    copy_out();
    printf("T32S end ok=%d\n", ok);
    return 0;
}
