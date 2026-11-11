/*
 * Test pattern mode 5 (ISP-middle picture) on the T40 and T41: platform hooks for src/openimp_testpattern.c.
 *
 * The T40/T41 3D noise reducer (MDNS, ISP core 0x13300000 + 0xf000, ID 0x20191209) keeps its reference frame in
 * DDR as two planes: Y at +0x40 (stride +0x44), UV at +0x48 (stride +0x4c), each followed by a 1/32 room, the
 * frame height = +0x68 / (stride / 16) (tisp_mdns_reg_cfg writes h * blocks16 there).  Every frame the block
 * blends the new frame with that reference and writes the result back, which the channels then scale.
 *
 * With the tuning values the reference is dropped where it differs from the frame: a per-pixel gate (+0x120
 * bytes 0/2, 0x0a/0x80 night), the still/moving weight limits (+0x150 bytes 0/1, 0x08/0xd2 night; the chroma
 * twin +0x234), the difference ramps (+0x174/+0x178; chroma +0x264/+0x26c) and the block motion limits
 * (+0x198 bytes 0..2; chroma +0x210, +0x278); in the colour bank also the weights by block age (+0x154 to
 * +0x16c) and the output ramps (+0x180 to +0x190).  With those fields at 0xff the block keeps a picture written
 * into the reference (Vanhua T55A gc5603 2026-11-11: a luma step of 40 decays to 0 in one frame with the
 * tuning values and stays with the fields held; Y SSIM 0.93 against the injected picture at night).
 *
 * The reference is 2880x1620 NV12 (7 MB) on the T55A, more than the CPU copies in a frame time and too much
 * RAM to keep a copy of on a 32 MB board: the picture is drawn once straight into the planes (cached /dev/mem
 * mapping, written back) and the block holds it by itself.  The frame tick checks the held fields; the driver
 * rewrites them on a gain step or a day/night switch (tisp_mdns_reg_cfg), and then the tick sets them again
 * and wakes a worker thread that draws the picture again (0.5 s on a T41LQ: not on the frame path).  On the
 * T41 the open driver's parameter mdns_ref_hold makes its writer keep the bits itself (set while the mode
 * runs): with the AE moving it rewrites the block on nearly every gain update, and each rewrite let live
 * picture into the reference for a frame.  The tick also reads 64 points of the reference (uncached) and has the
 * picture drawn again when most of them changed for three frames: timps stops the stream between clients and
 * the restart programs the block and its reference anew (T41LQ: the first mode-5 start after a stream restart
 * showed live picture without it).  Leaving the mode puts the tuning bytes back (the driver's next gain step refreshes
 * them anyway).
 *
 * T40: the same block (channel 0; the second pipeline's is at +0xf800 and stays as it is) writes its reference
 * as a coded stream while bit 24 of +0x24 (the function enables, tisp_mdns_func_en par[8]) is set, which the
 * T40XP tuning sets; with the bit clear it writes plain NV12 planes into the same buffer (Eufy T40XP sc830ai
 * 2026-11-11: luma bytes 0x40-0x50 in a dark scene instead of random words).  Mode 5 clears the bit while it
 * runs and restores it after.
 */
#if defined(PLATFORM_T41) || defined(PLATFORM_T40)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/cachectl.h>
#include <sys/mman.h>

#include "../openimp_testpattern_gen.h"

#define T41_MDNS_PAGE 0x1330f000u
#define T41_MDNS_ID   0x20191209u

/* MDNS-relative register, the bits mode 5 holds at 1 */
static const struct { uint16_t reg; uint32_t mask; } t41_mid_hold[] = {
    { 0x120, 0x00ff00ffu },     /* Y per-pixel gate */
    { 0x150, 0x0000ffffu },     /* Y weight limits */
    { 0x154, 0xffffffffu },     /* weights by block age (three tables) */
    { 0x158, 0xffffffffu },
    { 0x15c, 0xffffffffu },
    { 0x160, 0xffffffffu },
    { 0x164, 0xffffffffu },
    { 0x168, 0xffffffffu },
    { 0x16c, 0xffffffffu },
    { 0x174, 0xffffffffu },     /* Y difference ramp */
    { 0x178, 0xffffffffu },
    { 0x180, 0xffffffffu },     /* output ramps of the colour bank (a night bank has them flat) */
    { 0x184, 0xffffffffu },
    { 0x18c, 0xffffffffu },
    { 0x190, 0xffffffffu },
    { 0x198, 0x00ffffffu },     /* Y block motion limits */
    { 0x210, 0x0000ffffu },     /* chroma block limits */
    { 0x234, 0x0000ffffu },     /* chroma weight limits */
    { 0x264, 0xffffffffu },     /* chroma difference ramp */
    { 0x26c, 0xffffffffu },
    { 0x278, 0x0000ffffu },     /* chroma gate */
};
#define T41_MID_N (sizeof(t41_mid_hold) / sizeof(t41_mid_hold[0]))

static int mid_fd_reg = -1, mid_fd_mem = -1;
static volatile uint32_t *mid_r;
static uint8_t *mid_py, *mid_pu;
static volatile const uint8_t *mid_pyu;  /* the Y plane uncached (mid_fd_reg): checks the picture is still there */
#define MID_SAMPLES 64
static uint8_t mid_sample[MID_SAMPLES];
static int mid_painting, mid_lost;
static uint32_t mid_y, mid_uv, mid_st, mid_h, mid_ylen, mid_ulen, mid_yoff, mid_uoff;
static uint32_t mid_saved[T41_MID_N];
static int mid_saved_ok;
static int mid_drv_hold;                /* the driver's mdns_ref_hold is set (no rewrite on gain steps) */

/* open-tx-isp T41 since 2026-11-11: the MDNS writer keeps these bits itself while the parameter is 1, so a
 * gain step does not let live picture into the reference for a frame.  Absent: the tick alone. */
static int mid_drv_param(const char *v)
{
#if defined(PLATFORM_T41)
    int fd = open("/sys/module/tx_isp_t41/parameters/mdns_ref_hold", O_WRONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, v, strlen(v));
    close(fd);
    return n == (ssize_t)strlen(v) ? 0 : -1;
#else
    (void)v;
    return -1;
#endif
}
#if defined(PLATFORM_T40)
#define T40_MDNS_CODED (1u << 24)        /* +0x24 bit 24: the reference is written coded */
static uint32_t mid_saved_fun;
#endif

static inline volatile uint32_t *mreg(uint32_t off)
{
    return mid_r + (off >> 2);
}

static uint32_t round_1k(uint32_t v)
{
    return (v + 0x3ffu) & ~0x3ffu;
}

/* Current reference geometry; 0 when it is a plain NV12 pair this file can write. */
static int mid_read_geometry(uint32_t *y, uint32_t *uv, uint32_t *st, uint32_t *h)
{
    uint32_t s = *mreg(0x44), n = *mreg(0x68), hh;

    if (*mreg(0x000) != T41_MDNS_ID || s < 64 || s > 8192 || (s & 15) || *mreg(0x4c) != s || n % (s / 16))
        return -1;
    hh = n / (s / 16);
    if (hh < 16 || hh > 4320 || (hh & 1))
        return -1;
    *y = *mreg(0x40);
    *uv = *mreg(0x48);
    if (*y == 0 || *uv - *y != round_1k(s * hh + (s * hh >> 5)))
        return -1;
    *st = s;
    *h = hh;
    return 0;
}

/* Redraw worker: draws when the tick asks; stopped (joined) before the planes are unmapped. */
static pthread_t mid_thr;
static int mid_thr_on, mid_wreq, mid_wstop;
static uint32_t mid_wlogo;
static pthread_mutex_t mid_wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mid_wcond = PTHREAD_COND_INITIALIZER;

static uint32_t mid_sample_off(unsigned i)
{
    return (mid_h / 9u + (i / 8u) * (mid_h * 7u / 72u)) * mid_st + mid_st / 17u + (i % 8u) * (mid_st / 9u);
}

static void mid_sample_take(void)
{
    unsigned i;

    for (i = 0; i < MID_SAMPLES; i++)
        mid_sample[i] = mid_pyu[mid_yoff + mid_sample_off(i)];
}

/* 1 when most samples moved away from the drawn picture: the stream restarted (the driver programs the block
 * and its reference anew when the first consumer comes back, e.g. timps between clients) or live picture got
 * in.  The picture's own noise is 0. */
static int mid_sample_lost(void)
{
    unsigned i, bad = 0;

    for (i = 0; i < MID_SAMPLES; i++) {
        int d = (int)mid_pyu[mid_yoff + mid_sample_off(i)] - (int)mid_sample[i];

        if (d > 24 || d < -24)
            bad++;
    }
    return bad > MID_SAMPLES / 4;
}

/* Two passes: the block reads a row of the old picture, and writes it back a moment later, also over a row
 * drawn in between (the drawing takes about 12 frames); the second pass puts those few rows back (one pass
 * left thin streaks of the old picture). */
static void mid_paint(uint32_t logo)
{
    int pass;

    for (pass = 0; pass < 2; pass++) {
        openimp_testpattern_nv12_logo(mid_py + mid_yoff, mid_pu + mid_uoff, mid_st, mid_h, mid_st, logo);
        (void)cacheflush(mid_py, (int)(mid_ylen + mid_yoff), DCACHE);
        (void)cacheflush(mid_pu, (int)(mid_ulen + mid_uoff), DCACHE);
    }
    mid_sample_take();
}

static void *mid_worker(void *arg)
{
    uint32_t logo;

    (void)arg;
    pthread_mutex_lock(&mid_wlock);
    for (;;) {
        while (!mid_wreq && !mid_wstop)
            pthread_cond_wait(&mid_wcond, &mid_wlock);
        if (mid_wstop)
            break;
        mid_wreq = 0;
        logo = mid_wlogo;
        __atomic_store_n(&mid_painting, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&mid_wlock);
        mid_paint(logo);
        pthread_mutex_lock(&mid_wlock);
        __atomic_store_n(&mid_painting, 0, __ATOMIC_RELEASE);
        mid_lost = 0;
    }
    pthread_mutex_unlock(&mid_wlock);
    return NULL;
}

static void mid_worker_stop(void)
{
    if (!mid_thr_on)
        return;
    pthread_mutex_lock(&mid_wlock);
    mid_wstop = 1;
    pthread_cond_signal(&mid_wcond);
    pthread_mutex_unlock(&mid_wlock);
    pthread_join(mid_thr, NULL);
    mid_thr_on = 0;
}

static void mid_unmap(void)
{
    mid_worker_stop();
    if (mid_py)
        munmap(mid_py, mid_ylen + mid_yoff);
    if (mid_pyu)
        munmap((void *)mid_pyu, mid_ylen + mid_yoff);
    mid_pyu = NULL;
    if (mid_pu)
        munmap(mid_pu, mid_ulen + mid_uoff);
    mid_py = mid_pu = NULL;
}

static void mid_close(void)
{
    mid_unmap();
    if (mid_r)
        munmap((void *)mid_r, 0x1000);
    mid_r = NULL;
    if (mid_fd_reg >= 0)
        close(mid_fd_reg);
    if (mid_fd_mem >= 0)
        close(mid_fd_mem);
    mid_fd_reg = mid_fd_mem = -1;
}

/* Sets the held bits; returns 1 when one was not set (the reference may hold live picture now). */
static int mid_assert(void)
{
    unsigned i;
    int changed = 0;

    for (i = 0; i < T41_MID_N; i++) {
        uint32_t v = *mreg(t41_mid_hold[i].reg);

        if ((v & t41_mid_hold[i].mask) != t41_mid_hold[i].mask) {
            *mreg(t41_mid_hold[i].reg) = v | t41_mid_hold[i].mask;
            changed = 1;
        }
    }
#if defined(PLATFORM_T40)
    if (*mreg(0x024) & T40_MDNS_CODED) {
        *mreg(0x024) &= ~T40_MDNS_CODED;
        changed = 1;
    }
#endif
    if (changed)
        *mreg(0x004) = 1;               /* tisp_mdns_reg_trig */
    return changed;
}

int openimp_tp_ispmid_on(uint32_t *w, uint32_t *h, uint32_t *stride)
{
    void *m;
    unsigned i;

    if (mid_r == NULL) {
        mid_fd_reg = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        mid_fd_mem = open("/dev/mem", O_RDWR | O_CLOEXEC);          /* cached: the picture is drawn here */
        if (mid_fd_reg < 0 || mid_fd_mem < 0) {
            mid_close();
            errno = ENODEV;
            return -1;
        }
        m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mid_fd_reg, T41_MDNS_PAGE);
        if (m == MAP_FAILED) {
            mid_close();
            errno = ENODEV;
            return -1;
        }
        mid_r = (volatile uint32_t *)m;
    }
    mid_unmap();
    if (mid_read_geometry(&mid_y, &mid_uv, &mid_st, &mid_h) != 0) {
        mid_close();
        errno = EIO;                    /* no stream yet, or a layout this file does not write */
        return -1;
    }
    mid_ylen = mid_st * mid_h;
    mid_ulen = mid_st * mid_h / 2u;
    mid_yoff = mid_y & 0xfffu;
    mid_uoff = mid_uv & 0xfffu;
    m = mmap(NULL, mid_ylen + mid_yoff, PROT_READ | PROT_WRITE, MAP_SHARED, mid_fd_mem, mid_y - mid_yoff);
    mid_py = m == MAP_FAILED ? NULL : (uint8_t *)m;
    m = mmap(NULL, mid_ylen + mid_yoff, PROT_READ, MAP_SHARED, mid_fd_reg, mid_y - mid_yoff);
    mid_pyu = m == MAP_FAILED ? NULL : (volatile const uint8_t *)m;
    m = mmap(NULL, mid_ulen + mid_uoff, PROT_READ | PROT_WRITE, MAP_SHARED, mid_fd_mem, mid_uv - mid_uoff);
    mid_pu = m == MAP_FAILED ? NULL : (uint8_t *)m;
    if (!mid_py || !mid_pu || !mid_pyu) {
        mid_close();
        errno = ENOMEM;
        return -1;
    }
    if (!mid_saved_ok) {
        for (i = 0; i < T41_MID_N; i++)
            mid_saved[i] = *mreg(t41_mid_hold[i].reg);
#if defined(PLATFORM_T40)
        mid_saved_fun = *mreg(0x024);
#endif
        mid_saved_ok = 1;
        mid_drv_hold = mid_drv_param("1") == 0;
    }
    *w = *stride = mid_st;
    *h = mid_h;
    return 0;
}

/* Draws the picture into the reference planes (the generator of mode 1), with the fields held; starts the
 * redraw worker. */
int openimp_tp_ispmid_draw(uint32_t logo)
{
    if (!mid_py || !mid_pu) {
        errno = ENODEV;
        return -1;
    }
    (void)mid_assert();
    mid_paint(logo);
    pthread_mutex_lock(&mid_wlock);
    mid_wlogo = logo;
    mid_wreq = 0;
    mid_wstop = 0;
    pthread_mutex_unlock(&mid_wlock);
    if (!mid_thr_on && pthread_create(&mid_thr, NULL, mid_worker, NULL) == 0)
        mid_thr_on = 1;
    return 0;
}

/* Frame tick: nv12 is NULL (the picture lives in the reference).  0: held (a field the driver had rewritten is
 * set again and the worker draws the picture again), -1: the reference moved (rebuild). */
int openimp_tp_ispmid_put(const uint8_t *nv12, uint32_t stride, uint32_t h)
{
    uint32_t y, uv, st, hh;

    (void)nv12; (void)stride; (void)h;
    if (!mid_r || mid_read_geometry(&y, &uv, &st, &hh) != 0 || y != mid_y || uv != mid_uv || st != mid_st ||
        hh != mid_h)
        return -1;
    if (mid_assert())
        mid_lost = 3;                   /* fields were rewritten: draw again */
    else if (!__atomic_load_n(&mid_painting, __ATOMIC_ACQUIRE) && mid_lost < 3)
        mid_lost = mid_sample_lost() ? mid_lost + 1 : 0;   /* three frames in a row */
    if (mid_lost >= 3 && mid_thr_on && !__atomic_load_n(&mid_painting, __ATOMIC_ACQUIRE)) {
        mid_lost = 4;                   /* asked; the worker clears it */
        pthread_mutex_lock(&mid_wlock);
        mid_wreq = 1;
        pthread_cond_signal(&mid_wcond);
        pthread_mutex_unlock(&mid_wlock);
    }
    return 0;
}

/* The ISP flips (IMP_ISP_Tuning_SetISPHflip) are not checked against the MDNS position on the T41 yet. */
int openimp_tp_ispmid_hmirror(void)
{
    return 0;
}

void openimp_tp_ispmid_off(void)
{
    unsigned i;

    mid_worker_stop();
    if (mid_r && mid_saved_ok) {
        for (i = 0; i < T41_MID_N; i++) {
            uint32_t m = t41_mid_hold[i].mask, v = *mreg(t41_mid_hold[i].reg);

            *mreg(t41_mid_hold[i].reg) = (v & ~m) | (mid_saved[i] & m);
        }
#if defined(PLATFORM_T40)
        *mreg(0x024) = (*mreg(0x024) & ~T40_MDNS_CODED) | (mid_saved_fun & T40_MDNS_CODED);
#endif
        *mreg(0x004) = 1;
    }
    if (mid_drv_hold)
        (void)mid_drv_param("0");
    mid_drv_hold = 0;
    mid_saved_ok = 0;
    mid_close();
}
#endif /* PLATFORM_T41 || PLATFORM_T40 */
