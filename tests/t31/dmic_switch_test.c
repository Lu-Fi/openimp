/*
 * dmic_switch_test - #342: the DMIC AEC / reference switches of the shared
 * OpenIMP DMIC (src/t31/openimp_t31_dmic.c, built for T31 and for the OSS3
 * T40/T41 variant) must return while the channel captures.  A fake /dev/dsp
 * serves a frame every 2 ms; a reader thread takes and releases frames while
 * the main thread toggles IMP_DMIC_Enable/DisableAec and
 * IMP_DMIC_Enable/DisableAecRefFrame.  Before the fix dmic_wait_idle() waited
 * for "reading == 0" under dmic.lock, which the record thread never shows
 * while frames flow (it sets reading again before it drops the lock), so the
 * Disable calls hung.  Also checks that the AEC data path is unchanged: AEC
 * frames while enabled, none after DisableAec, AEC process never called
 * after its handle was freed.
 */
#include "../../src/t31/openimp_t31_dmic.c"

#include <stdarg.h>
#include <stdio.h>
#include <signal.h>

#define FAKE_FD 91
#define SWITCHES 40
#define MAX_SWITCH_NS (500LL * 1000 * 1000)   /* one frame is 2 ms */

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static volatile int frames_served;
static volatile int aec_live;          /* fake handle allocated */
static volatile int aec_after_free;    /* process called on a freed handle */
static volatile int aec_frames;
static volatile int reader_stop;
static int aec_handle_storage;

int64_t IMP_System_GetTimeStamp(void) { return 123456; }

static void *fake_aec_create(int rate, const char *path)
{
    (void)rate;
    (void)path;
    aec_live = 1;
    return &aec_handle_storage;
}

static int fake_aec_process(void *handle, void *frame)
{
    (void)frame;
    if (handle != &aec_handle_storage || !aec_live)
        aec_after_free = 1;
    usleep(500);
    return 0;
}

static int fake_aec_free(void *handle)
{
    (void)handle;
    aec_live = 0;
    return 0;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/dev/dsp"))
        return FAKE_FD;
    return __real_open(path, flags);
}

int __real_close(int fd);
int __wrap_close(int fd)
{
    return fd == FAKE_FD ? 0 : __real_close(fd);
}

int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (fd != FAKE_FD)
        return __real_ioctl(fd, request, arg);
    if (request == DMIC_GET_STREAM) {
        DmicStream *st = arg;
        int16_t *p = st->data;
        uint32_t i;

        usleep(2000);
        for (i = 0; i < st->size / 2; i++)
            p[i] = (int16_t)(1000 + (frames_served & 0xff));
        frames_served++;
    }
    return 0;
}

static void *reader_main(void *argument)
{
    IMPDmicChnFrame frm;

    (void)argument;
    while (!reader_stop) {
        if (IMP_DMIC_GetFrame(0, 0, &frm, BLOCK) != 0)
            continue;
        if (frm.aecFrame.len)
            aec_frames++;
        (void)IMP_DMIC_ReleaseFrame(0, 0, &frm);
    }
    return NULL;
}

static long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] =
        "FAIL dmic_switch_test: switch call hung while capturing (#342)\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    _exit(2);
}

static void wait_frames(int count)
{
    int start = frames_served;

    while (frames_served - start < count)
        usleep(1000);
}

int main(void)
{
    IMPDmicAttr attr = { .samplerate = DMIC_SAMPLE_RATE_16000,
                         .bitwidth = DMIC_BIT_WIDTH_16,
                         .soundmode = DMIC_SOUND_MODE_MONO,
                         .frmNum = 4, .numPerFrm = 160, .chnCnt = 2 };
    IMPDmicChnParam prm = { .usrFrmDepth = 3 };
    pthread_t reader;
    long long worst = 0;
    int i;

    signal(SIGALRM, on_alarm);
    /* libaudioProcess stand-in (dmic_load_aec() skips dlopen when set) */
    dmic.library = &aec_handle_storage;
    dmic.aec_create = fake_aec_create;
    dmic.aec_process = fake_aec_process;
    dmic.aec_free = fake_aec_free;

    CHECK(IMP_DMIC_SetPubAttr(0, &attr) == 0);
    CHECK(IMP_DMIC_SetUserInfo(0, 0, 1) == 0);
    CHECK(IMP_DMIC_Enable(0) == 0);
    CHECK(IMP_DMIC_SetChnParam(0, 0, &prm) == 0);
    CHECK(IMP_DMIC_EnableChn(0, 0) == 0);
    CHECK(pthread_create(&reader, NULL, reader_main, NULL) == 0);
    wait_frames(5);

    alarm(20);
    for (i = 0; i < SWITCHES; i++) {
        long long t;
        int before;

        CHECK(IMP_DMIC_EnableAec(0, 0, 0, 0) == 0);
        wait_frames(3);
        before = aec_frames;
        wait_frames(4);
        CHECK(aec_frames > before);              /* AEC frames delivered */
        t = now_ns();
        CHECK(IMP_DMIC_DisableAec(0, 0) == 0);
        t = now_ns() - t;
        if (t > worst)
            worst = t;
        CHECK(!aec_live && !dmic.aec && !dmic.ref);

        CHECK(IMP_DMIC_EnableAecRefFrame(0, 0, 0, 0) == 0);
        wait_frames(2);
        t = now_ns();
        CHECK(IMP_DMIC_DisableAecRefFrame(0, 0, 0, 0) == 0);
        t = now_ns() - t;
        if (t > worst)
            worst = t;
        CHECK(!dmic.ref && !dmic.ref_enabled);
    }
    /* no AEC frames once it is off (frames already queued may still carry
     * one; wait for the queue to turn over) */
    wait_frames(8);
    {
        int before = aec_frames;

        wait_frames(8);
        CHECK(aec_frames == before);
    }
    alarm(0);
    CHECK(worst < MAX_SWITCH_NS);
    CHECK(!aec_after_free);

    reader_stop = 1;
    CHECK(IMP_DMIC_DisableChn(0, 0) == 0);
    pthread_join(reader, NULL);
    CHECK(IMP_DMIC_Disable(0) == 0);

    if (failures) {
        fprintf(stderr, "dmic_switch_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("dmic_switch_test: PASS (%d switches, worst %.2f ms, %d frames)\n",
           2 * SWITCHES, worst / 1e6, frames_served);
    return 0;
}
