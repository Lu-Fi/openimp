#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* apitest: sysutils (SU_*) functions.  They live in libsysutils.so, resolved with dlsym
 * (type-checked against the vendor headers). Never called: SU_Base_Shutdown/Reboot/Suspend (Shutdown signals init to power off; host test: tests/core/su_base_shutdown_test.c). */
#include <fcntl.h>
#include <errno.h>
#include <errno.h>
#include <unistd.h>
#include "apitest.h"

static void *g_su;
/* the SU_* of libimp itself (T40: ported into the open libimp) win over a vendor libsysutils.so; the rest comes from libsysutils.so */
#define SUSYM(name) ((__typeof__(&name))(dlsym(RTLD_DEFAULT, #name) ? dlsym(RTLD_DEFAULT, #name) : (g_su ? dlsym(g_su, #name) : NULL)))
#define SUSYMN(nm) (dlsym(RTLD_DEFAULT, nm) ? dlsym(RTLD_DEFAULT, nm) : (g_su ? dlsym(g_su, nm) : NULL))
/* resolve into 'f'; N/A line when missing */
#define SUNEED(name) __typeof__(&name) f_##name = SUSYM(name); if (!f_##name) { rep_na(FN(name)); } else

/* is there an input device named gpio_keys (the one SU_Key_OpenEvent looks for)? */
static int have_gpio_keys(void)
{
    char path[160], name[64];
    int i, found = 0;

    for (i = 0; i < 16 && !found; i++) {
        FILE *f;

        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof(name), f) && strstr(name, "gpio_keys")) found = 1;
        fclose(f);
    }
    return found;
}

/* QEMU guests (APITEST_QEMU=1, qemu_run.py) or an explicit APITEST_SU_HW=1 run the tests that change the RTC, arm its alarm or drive the
 * AES/DES engine: harmless in a model, never on a camera by default (an armed alarm can wake/power it) */
static int su_hw_tests(void)
{
    const char *q = getenv("APITEST_QEMU"), *h = getenv("APITEST_SU_HW");

    return (q && q[0] == '1') || (h && h[0] == '1');
}

static double su_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long su_to_epoch(const SUTime *t)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    tm.tm_sec = t->sec; tm.tm_min = t->min; tm.tm_hour = t->hour; tm.tm_mday = t->mday; tm.tm_mon = t->mon - 1; tm.tm_year = t->year - 1900;
    return (long)timegm(&tm);
}

static void su_from_epoch(long e, SUTime *t)
{
    time_t te = e;
    struct tm tm;

    gmtime_r(&te, &tm);
    t->sec = tm.tm_sec; t->min = tm.tm_min; t->hour = tm.tm_hour; t->mday = tm.tm_mday; t->mon = tm.tm_mon + 1; t->year = tm.tm_year + 1900;
}

/* AES known-answer vectors: FIPS-197 C.1 (ECB) and NIST SP 800-38A F.2.1 (CBC, two blocks) */
static const unsigned char kat_ecb_key[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const unsigned char kat_ecb_pt[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
static const unsigned char kat_ecb_ct[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
static const unsigned char kat_cbc_key[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
static const unsigned char kat_cbc_iv[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const unsigned char kat_cbc_pt[32] = { 0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
                                              0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51 };
static const unsigned char kat_cbc_ct[32] = { 0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
                                              0x50,0x86,0xcb,0x9b,0x50,0x72,0x19,0xee,0x95,0xdb,0x11,0x3a,0x91,0x76,0x78,0x4f };

/* one AES run through the SU_CIPHER_* calls: key/IV/data are handed over as bytes (swap = 1: every 4 bytes reversed, the byte order of a
 * little-endian word interface).  Returns 0, or the step that failed (1 create, 2 config, 3 encrypt, 4 config, 5 decrypt) with its return
 * code in *step_rc; *match = the ciphertext equals 'exp', *roundtrip = decrypt gave the plaintext back. */
static int su_aes_run(int mode, const unsigned char *key, const unsigned char *iv, const unsigned char *pt, const unsigned char *exp,
                      unsigned len, int swap, int *match, int *roundtrip, int *step_rc, unsigned char *first)
{
    __typeof__(&SU_CIPHER_CreateHandle) cr = SUSYM(SU_CIPHER_CreateHandle);
    __typeof__(&SU_CIPHER_DestroyHandle) de = SUSYM(SU_CIPHER_DestroyHandle);
    __typeof__(&SU_CIPHER_ConfigHandle) cf = SUSYM(SU_CIPHER_ConfigHandle);
    __typeof__(&SU_CIPHER_Encrypt) en = SUSYM(SU_CIPHER_Encrypt);
    __typeof__(&SU_CIPHER_Decrypt) dc = SUSYM(SU_CIPHER_Decrypt);
    IN_UNF_CIPHER_CTRL ctrl;
    unsigned int *src = aligned_alloc(32, 256), *dst = aligned_alloc(32, 256), *back = aligned_alloc(32, 256), *ref = aligned_alloc(32, 256);
    int h, i, rc = 0;

    *match = *roundtrip = 0; *step_rc = 0;
    memset(src, 0, 256); memset(dst, 0xa5, 256); memset(back, 0x5a, 256);
    memcpy(src, pt, len); memcpy(ref, pt, len);
    memset(&ctrl, 0, sizeof(ctrl));
    memcpy(ctrl.key, key, 16);
    if (iv) memcpy(ctrl.IV, iv, 16);
    /* swap bit 0: key and IV words byte-reversed (bit 2: IV left as is), bit 1: the input data words byte-reversed, bit 3: the output words are in plain byte order */
    if (swap & 1) for (i = 0; i < 4; i++) { ctrl.key[i] = __builtin_bswap32(ctrl.key[i]); if (!(swap & 4)) ctrl.IV[i] = __builtin_bswap32(ctrl.IV[i]); }
    if (swap & 2) for (i = 0; i < (int)len / 4; i++) src[i] = __builtin_bswap32(src[i]);
    ctrl.enDataLen = len;
    ctrl.enAlg = IN_UNF_CIPHER_ALG_AES;
    ctrl.enBitWidth = IN_UNF_CIPHER_BIT_WIDTH_128BIT;
    ctrl.enWorkMode = mode;
    ctrl.enKeyLen = IN_UNF_CIPHER_KEY_AES_128BIT;
    h = cr();
    if (h < 0) { *step_rc = h; rc = 1; goto out; }
    if ((*step_rc = cf(h, &ctrl)) != 0) { rc = 2; goto done; }
    if ((*step_rc = en(h, src, dst, len)) != 0) { rc = 3; goto done; }
    {
        unsigned char got[64];

        memcpy(got, dst, len);
        if (first) memcpy(first, got, 16);
        if ((swap & 2) && !(swap & 8)) for (i = 0; i < (int)len / 4; i++) ((unsigned int *)got)[i] = __builtin_bswap32(((unsigned int *)got)[i]);
        *match = memcmp(got, exp, len) == 0;
    }
    /* decrypt the ciphertext again: a fresh config (the CBC chain restarts from the IV) */
    if ((*step_rc = cf(h, &ctrl)) != 0) { rc = 4; goto done; }
    if ((*step_rc = dc(h, dst, back, len)) != 0) { rc = 5; goto done; }
    *roundtrip = memcmp(back, (swap & 2) ? (void *)src : (void *)ref, len) == 0;
done:
    de(h);
out:
    free(src); free(dst); free(back); free(ref);
    return rc;
}

void t_su(void)
{
    int r;

    rep_area("su");
    g_su = dlopen("libsysutils.so", RTLD_NOW);
    if (!g_su) {
        rep(LBL("libsysutils.so"), -1, V_NA, "dlopen failed: %s (the SU_* that libimp exports are used)", dlerror());
    }

    SUNEED(SU_Base_GetModelNumber) {
        G(SUModelNum, m);
        r = f_SU_Base_GetModelNumber(m); gchk(m);
        CHECK(SU_Base_GetModelNumber, r, m->chr[0] != 0, "\"%.16s\"", m->chr);
        gfree(m);
    }
    SUNEED(SU_Base_GetVersion) {
        G(SUVersion, v);
        r = f_SU_Base_GetVersion(v); gchk(v);
        CHECK(SU_Base_GetVersion, r, v->chr[0] != 0, "\"%.60s\"", v->chr);
        gfree(v);
    }
    SUNEED(SU_Base_GetDevID) {
        G(SUDevID, d);
        r = f_SU_Base_GetDevID(d); gchk(d);
        CHECK(SU_Base_GetDevID, r, gtouched(d), "%02x%02x%02x%02x%02x%02x%02x%02x...", d->hex[0], d->hex[1], d->hex[2], d->hex[3], d->hex[4], d->hex[5], d->hex[6], d->hex[7]);
        gfree(d);
    }
    /* The RTC (/dev/rtc0) and the ADC (/dev/ingenic_adc_aux_*, /dev/jz_adc_aux_*) are optional
     * hardware: the vendor libsysutils opens the same nodes and fails the same way when the
     * kernel has no such driver (checked on the T20 jxf23 board: no rtc class, no ADC node). */
    int has_rtc = access("/dev/rtc0", F_OK) == 0;
    const char *no_rtc = "no /dev/rtc0 (no RTC driver in this kernel/board; the vendor libsysutils opens the same node)";
    /* the time conversions are pure arithmetic (no device): fixed time 2001-09-09 01:46:40 UTC = 1000000000 */
    SUNEED(SU_Base_SUTime2Raw) {
        G(SUTime, ft); G(uint32_t, raw); G(SUTime, back);
        ft->year = 2001; ft->mon = 9; ft->mday = 9; ft->hour = 1; ft->min = 46; ft->sec = 40;
        int r2 = f_SU_Base_SUTime2Raw(ft, raw); gchk(raw);
        CHECK(SU_Base_SUTime2Raw, r2, *raw == 1000000000u, "2001-09-09 01:46:40 -> %u (expected 1000000000)", *raw);
        SUNEED(SU_Base_Raw2SUTime) {
            int r3 = f_SU_Base_Raw2SUTime(raw, back); gchk(back);
            CHECK(SU_Base_Raw2SUTime, r3, back->year == 2001 && back->mon == 9 && back->mday == 9 && back->hour == 1 && back->min == 46 && back->sec == 40,
                  "%u -> %04d-%02d-%02d %02d:%02d:%02d (round trip)", *raw, back->year, back->mon, back->mday, back->hour, back->min, back->sec);
        }
        gfree(ft); gfree(raw); gfree(back);
    }
    if (!has_rtc) {
        rep(FN(SU_Base_GetTime), -1, V_NA, "%s", no_rtc);
        rep(FN(SU_Base_SetTime), -1, V_NA, "%s", no_rtc);
        rep(FN(SU_Base_SetAlarm), -1, V_NA, "%s", no_rtc);
        rep(FN(SU_Base_GetAlarm), -1, V_NA, "%s", no_rtc);
        rep(FN(SU_Base_DisableAlarm), -1, V_NA, "%s", no_rtc);
    } else {
    SUNEED(SU_Base_GetTime) {
        G(SUTime, t); G(SUTime, t2);
        int r1 = f_SU_Base_GetTime(t); gchk(t);
        CHECK(SU_Base_GetTime, r1, t->year >= 1970 && t->mon >= 1 && t->mon <= 12 && t->mday >= 1 && t->mday <= 31 && t->hour < 24 && t->min < 60 && t->sec < 62,
              "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->mon, t->mday, t->hour, t->min, t->sec);
        SUNEED(SU_Base_SetTime) {
            int r2 = f_SU_Base_SetTime(t);       /* writes the time just read back: no change */
            f_SU_Base_GetTime(t2); gchk(t2);
            CHECK(SU_Base_SetTime, r2, t2->year == t->year && t2->mon == t->mon && t2->mday == t->mday, "same time written back, now %04d-%02d-%02d %02d:%02d:%02d", t2->year, t2->mon, t2->mday, t2->hour, t2->min, t2->sec);
        }
        gfree(t); gfree(t2);
    }
    SUNEED(SU_Base_SetAlarm) {
        G(SUTime, a); G(SUTime, g);
        int r2;

        a->year = 2030; a->mon = 1; a->mday = 1; a->hour = 12; a->min = 0; a->sec = 0;
        r2 = f_SU_Base_SetAlarm(a);
        RET0(SU_Base_SetAlarm, r2, "2030-01-01 12:00:00");
        SUNEED(SU_Base_GetAlarm) {
            int r3 = f_SU_Base_GetAlarm(g); gchk(g);
            CHECK(SU_Base_GetAlarm, r3, g->year == 2030 && g->mon == 1 && g->mday == 1 && g->hour == 12, "%04d-%02d-%02d %02d:%02d:%02d", g->year, g->mon, g->mday, g->hour, g->min, g->sec);
        }
        SUNEED(SU_Base_DisableAlarm) { int r3 = f_SU_Base_DisableAlarm(); RET0(SU_Base_DisableAlarm, r3, ""); }
        gfree(a); gfree(g);
    }
    }
    if (has_rtc && su_hw_tests()) {
        /* QEMU / APITEST_SU_HW=1: set a different time, read it back after 2 s, arm an alarm 3 s ahead, wait for it, restore the clock */
        __typeof__(&SU_Base_GetTime) f_SU_Base_GetTime = SUSYM(SU_Base_GetTime);
        __typeof__(&SU_Base_SetTime) f_SU_Base_SetTime = SUSYM(SU_Base_SetTime);
        __typeof__(&SU_Base_SetAlarm) f_SU_Base_SetAlarm = SUSYM(SU_Base_SetAlarm);
        __typeof__(&SU_Base_GetAlarm) f_SU_Base_GetAlarm = SUSYM(SU_Base_GetAlarm);
        __typeof__(&SU_Base_EnableAlarm) f_SU_Base_EnableAlarm = SUSYM(SU_Base_EnableAlarm);
        __typeof__(&SU_Base_DisableAlarm) f_SU_Base_DisableAlarm = SUSYM(SU_Base_DisableAlarm);
        __typeof__(&SU_Base_PollingAlarm) f_SU_Base_PollingAlarm = SUSYM(SU_Base_PollingAlarm);
        if (f_SU_Base_GetTime && f_SU_Base_SetTime && f_SU_Base_SetAlarm && f_SU_Base_GetAlarm && f_SU_Base_EnableAlarm && f_SU_Base_DisableAlarm && f_SU_Base_PollingAlarm) {
            G(SUTime, t0); G(SUTime, t1); G(SUTime, t2);
            long e0, e1;
            double ta;

            f_SU_Base_GetTime(t0); gchk(t0);
            e0 = su_to_epoch(t0);
            su_from_epoch(1900000000L, t1);          /* 2030-03-17 17:46:40 UTC */
            ta = su_now();
            r = f_SU_Base_SetTime(t1);
            sleep(2);
            f_SU_Base_GetTime(t2); gchk(t2);
            e1 = su_to_epoch(t2) - 1900000000L;
            rep(LBL("SU_Base_SetTime(2030-03-17 17:46:40)"), r, (r == 0 && e1 >= 1 && e1 <= 4) ? V_PASS : V_FAIL,
                "read back %04d-%02d-%02d %02d:%02d:%02d, %ld s after the set (expected 2 s, %.1f s elapsed)", t2->year, t2->mon, t2->mday, t2->hour, t2->min, t2->sec, e1, su_now() - ta);
            {
                int rs, rg, re, rp, rd, rpast;
                double t_start, el;

                f_SU_Base_GetTime(t2); e1 = su_to_epoch(t2);
                su_from_epoch(e1 - 5, t1);           /* an alarm in the past: the vendor function refuses to arm it */
                f_SU_Base_SetAlarm(t1);
                rpast = f_SU_Base_EnableAlarm();
                f_SU_Base_DisableAlarm();
                su_from_epoch(e1 + 3, t1);
                rs = f_SU_Base_SetAlarm(t1);
                rg = f_SU_Base_GetAlarm(t2); gchk(t2);
                re = f_SU_Base_EnableAlarm();
                t_start = su_now();
                rp = f_SU_Base_PollingAlarm(8000);
                el = su_now() - t_start;
                rd = f_SU_Base_DisableAlarm();
                rep(FN(SU_Base_SetAlarm), rs, rs == 0 ? V_PASS : V_FAIL, "alarm now+3 s");
                rep(FN(SU_Base_GetAlarm), rg, (rg == 0 && su_to_epoch(t2) == su_to_epoch(t1)) ? V_PASS : V_FAIL, "read back %04d-%02d-%02d %02d:%02d:%02d", t2->year, t2->mon, t2->mday, t2->hour, t2->min, t2->sec);
                rep(FN(SU_Base_EnableAlarm), re, (re == 0 && rpast != 0) ? V_PASS : V_FAIL, "arms an alarm 3 s ahead (0), refuses one in the past (%d)", rpast);
                rep(FN(SU_Base_PollingAlarm), rp, (rp == 0 && el > 1.0 && el < 6.0) ? V_PASS : V_FAIL, "alarm fired after %.1f s (timeout 8 s: %s)", el, el < 7.5 ? "woken by the alarm" : "timed out");
                rep(FN(SU_Base_DisableAlarm), rd, rd == 0 ? V_PASS : V_FAIL, "after the alarm");
            }
            /* restore the clock: the time read at the start plus the time the test took */
            su_from_epoch(e0 + (long)(su_now() - ta), t1);
            f_SU_Base_SetTime(t1);
            gfree(t0); gfree(t1); gfree(t2);
        }
    } else if (!has_rtc && su_hw_tests())
        rep(FN(SU_Base_EnableAlarm), -1, V_NA, "%s", no_rtc);
    else
        rep(FN(SU_Base_EnableAlarm), 0, V_SKIP, "an enabled RTC alarm can wake/power the camera (QEMU: automatic; elsewhere APITEST_SU_HW=1)");
    if (has_rtc && !su_hw_tests()) {
        /* the vendor function returns 0 after the timeout as well (it only logs "select timeout"): a 50 ms poll is safe */
        SUNEED(SU_Base_PollingAlarm) { int r4 = f_SU_Base_PollingAlarm(50); RET0(SU_Base_PollingAlarm, r4, "50 ms poll"); }
    } else if (!has_rtc)
        rep(FN(SU_Base_PollingAlarm), -1, V_NA, "%s", no_rtc);
    rep(FN(SU_Base_Shutdown), 0, V_SKIP, "powers the camera off");
    rep(FN(SU_Base_Reboot), 0, V_SKIP, "reboots the camera");
    rep(FN(SU_Base_Suspend), 0, V_SKIP, "suspends the camera");
#if HAS_SU_Base_SetWkupMode
    /* modes 1 and 2 arm an RTC wake-up: never sent.  A mode outside 1..2 must be refused without touching the hardware; the symbol found first
     * (OpenIMP's libimp when it exports one, else the board's libsysutils) answers, the detail names the library. */
    {
        __typeof__(&SU_Base_SetWkupMode) fw = (__typeof__(&SU_Base_SetWkupMode))dlsym(RTLD_DEFAULT, "SU_Base_SetWkupMode");
        Dl_info di;

        if (!fw && g_su) fw = SUSYM(SU_Base_SetWkupMode);
        if (!fw) rep_na(FN(SU_Base_SetWkupMode));
        else {
            const char *lib = dladdr((void *)fw, &di) && di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            r = fw(0);
            rep(FN(SU_Base_SetWkupMode), r, r == -1 ? V_PASS : V_FAIL, "mode 0 refused (%d) by %s; modes 1/2 not sent (arm an RTC wake-up)", r, lib ? lib + 1 : "?");
        }
    }
#endif

    /* misc / ADC / battery / cipher / key / LED */
#if HAS_SU_ADC_Init
    int has_adc = access("/dev/ingenic_adc_aux_0", F_OK) == 0 || access("/dev/jz_adc_aux_0", F_OK) == 0;
    if (!has_adc) rep(FN(SU_ADC_Init), -1, V_NA, "no /dev/ingenic_adc_aux_0 or /dev/jz_adc_aux_0 (no ADC driver; the vendor libsysutils opens the same nodes)");
    else {
    SUNEED(SU_ADC_Init) {
        r = f_SU_ADC_Init(); RET0(SU_ADC_Init, r, "");
        if (r == 0) {
            SUNEED(SU_ADC_EnableChn) {
                r = f_SU_ADC_EnableChn(0); RET0(SU_ADC_EnableChn, r, "chn 0");
                SUNEED(SU_ADC_GetChnValue) {
                    G(int, v);
                    int r2 = f_SU_ADC_GetChnValue(0, v); gchk(v);
                    RET0(SU_ADC_GetChnValue, r2, "chn 0 = %d", *v);
                    gfree(v);
                }
                SUNEED(SU_ADC_DisableChn) { r = f_SU_ADC_DisableChn(0); RET0(SU_ADC_DisableChn, r, "chn 0"); }
            }
            SUNEED(SU_ADC_Exit) { r = f_SU_ADC_Exit(); RET0(SU_ADC_Exit, r, ""); }
        }
    }
    }
#endif
#if HAS_SU_Battery_GetCapacity
    SUNEED(SU_Battery_GetCapacity) { r = f_SU_Battery_GetCapacity(); rep(FN(SU_Battery_GetCapacity), r, (r >= 0 && r <= 100) ? V_PASS : V_FAIL, "%d %% (negative = no battery)", r); }
    SUNEED(SU_Battery_GetVoltageUV) { r = f_SU_Battery_GetVoltageUV(); rep(FN(SU_Battery_GetVoltageUV), r, r >= 0 ? V_PASS : V_FAIL, "%d uV", r); }
    SUNEED(SU_Battery_GetStatus) { G(SUBatStatus, v); r = f_SU_Battery_GetStatus(v); gchk(v); RET0(SU_Battery_GetStatus, r, "status %d", (int)*v); gfree(v); }
    rep(FN(SU_Battery_GetEvent), 0, V_SKIP, "blocks until a battery event");
#endif
    SUNEED(SU_Key_OpenEvent) {
        int fd = f_SU_Key_OpenEvent();

        int nodev = fd < 0 && !have_gpio_keys();   /* no gpio_keys input device on this board: the vendor lookup fails as well */

        rep(FN(SU_Key_OpenEvent), fd, fd >= 0 ? V_PASS : nodev ? V_NA : V_FAIL, "event fd %d%s", fd, nodev ? " (no gpio_keys input device on this board)" : "");
        SUNEED(SU_Key_CloseEvent) {
            if (nodev) rep(FN(SU_Key_CloseEvent), 0, V_NA, "no input device, nothing to close");
            else { r = fd >= 0 ? f_SU_Key_CloseEvent(fd) : -1; RET0(SU_Key_CloseEvent, r, "fd %d", fd); }
        }
    }
    rep(FN(SU_Key_ReadEvent), 0, V_SKIP, "blocks until a key is pressed");
    {
        /* APITEST_KEYMASK=1: mask the first key of the gpio-keys driver (sysfs disabled_keys), check the node, unmask, check the node is back to
         * the original text. Needs /sys/devices/platform/gpio-keys/{keys,disabled_keys}; N/A without them (QEMU). Default: SKIP (masks a hardware key). */
        const char *kd = "/sys/devices/platform/gpio-keys/disabled_keys", *kk = "/sys/devices/platform/gpio-keys/keys";
        char orig[256] = "", cur[256] = "", keys[256] = "";
        int code = -1, have = 0;
        FILE *fp = fopen(kk, "r");
        if (fp) { if (fgets(keys, sizeof(keys), fp)) code = atoi(keys); fclose(fp); }
        fp = fopen(kd, "r");
        if (fp) { have = 1; if (!fgets(orig, sizeof(orig), fp)) orig[0] = 0; fclose(fp); }
        orig[strcspn(orig, "\n")] = 0;
        if (!getenv("APITEST_KEYMASK") || strcmp(getenv("APITEST_KEYMASK"), "1")) {
            rep(FN(SU_Key_EnableEvent), 0, V_SKIP, "masks a hardware key in the input driver (APITEST_KEYMASK=1 masks and unmasks the first gpio-keys key)");
            rep(FN(SU_Key_DisableEvent), 0, V_SKIP, "masks a hardware key in the input driver (APITEST_KEYMASK=1 masks and unmasks the first gpio-keys key)");
        } else if (!have || code <= 0 || orig[0]) {
            const char *why = !have ? "no /sys/devices/platform/gpio-keys/disabled_keys on this board" : code <= 0 ? "no key code in gpio-keys/keys" : "a key is masked already, not touched";
            rep(FN(SU_Key_EnableEvent), -1, V_NA, "%s", why);
            rep(FN(SU_Key_DisableEvent), -1, V_NA, "%s", why);
        } else if (({ int pfd = open(kd, O_WRONLY), pw = -1, pe = 0; char cs[24]; snprintf(cs, sizeof(cs), "%d\n", code);
                      if (pfd >= 0) { pw = write(pfd, cs, strlen(cs)); pe = errno; if (pw >= 0) { lseek(pfd, 0, SEEK_SET); (void)!write(pfd, "\n", 1); } close(pfd); }
                      errno = pe; pw < 0; })) {
            /* the kernel's gpio-keys driver masks only buttons with can_disable set: the sysfs write is refused (EINVAL) on this board, so the SU_Key calls answer -1 */
            rep(FN(SU_Key_EnableEvent), -1, V_NA, "the kernel refuses to mask key %d (sysfs disabled_keys write: %s): not a maskable button on this board", code, strerror(errno));
            rep(FN(SU_Key_DisableEvent), -1, V_NA, "the kernel refuses to mask key %d (sysfs disabled_keys write: %s): not a maskable button on this board", code, strerror(errno));
        } else {
            SUNEED(SU_Key_DisableEvent) {
                char want[32];
                __typeof__(&SU_Key_OpenEvent) fo = SUSYM(SU_Key_OpenEvent); __typeof__(&SU_Key_CloseEvent) fc = SUSYM(SU_Key_CloseEvent);
                int efd = fo ? fo() : -1;       /* the key table is loaded by OpenEvent (Disable/Enable answer -1 without it) */
                int rd = f_SU_Key_DisableEvent(code);
                snprintf(want, sizeof(want), "%d", code);
                fp = fopen(kd, "r"); cur[0] = 0;
                if (fp) { if (!fgets(cur, sizeof(cur), fp)) cur[0] = 0; fclose(fp); }
                cur[strcspn(cur, "\n")] = 0;
                rep(FN(SU_Key_DisableEvent), rd, (rd == 0 && strstr(cur, want)) ? V_PASS : V_FAIL, "key %d masked, disabled_keys = \"%s\"", code, cur);
                SUNEED(SU_Key_EnableEvent) {
                    int re = f_SU_Key_EnableEvent(code);
                    fp = fopen(kd, "r"); cur[0] = 0;
                    if (fp) { if (!fgets(cur, sizeof(cur), fp)) cur[0] = 0; fclose(fp); }
                    cur[strcspn(cur, "\n")] = 0;
                    rep(FN(SU_Key_EnableEvent), re, (re == 0 && !cur[0]) ? V_PASS : V_FAIL, "key %d unmasked, disabled_keys = \"%s\" (was \"%s\")", code, cur, orig);
                    if (cur[0]) { fp = fopen(kd, "w"); if (fp) { fputs("\n", fp); fclose(fp); } }   /* never leave a key masked */
                    if (efd >= 0 && fc) fc(efd);
                }
            }
        }
    }
    rep(FN(SU_LED_Command), 0, V_SKIP, "drives the board LED");
#ifdef APITEST_T40
    /* T40 open stack: no AES/DES kernel driver, the SU_CIPHER_* of libimp answer -1 / ENOTSUP (a vendor libsysutils.so is not loaded first) */
#define CIPHER_ENOTSUP(call, name) do { errno = 0; int rc_ = (call); \
        rep(FN(name), rc_, (rc_ == -1 && errno == ENOTSUP) ? V_PASS : V_FAIL, "-1 / ENOTSUP expected (no /dev/aes in the open stack), got %d errno %d", rc_, errno); } while (0)
    SUNEED(SU_CIPHER_Init) { CIPHER_ENOTSUP(f_SU_CIPHER_Init(), SU_CIPHER_Init); }
    SUNEED(SU_CIPHER_Exit) { CIPHER_ENOTSUP(f_SU_CIPHER_Exit(), SU_CIPHER_Exit); }
    SUNEED(SU_CIPHER_CreateHandle) { CIPHER_ENOTSUP(f_SU_CIPHER_CreateHandle(), SU_CIPHER_CreateHandle); }
    SUNEED(SU_CIPHER_DestroyHandle) { CIPHER_ENOTSUP(f_SU_CIPHER_DestroyHandle(-1), SU_CIPHER_DestroyHandle); }
    SUNEED(SU_CIPHER_ConfigHandle) { CIPHER_ENOTSUP(f_SU_CIPHER_ConfigHandle(-1, NULL), SU_CIPHER_ConfigHandle); }
    SUNEED(SU_CIPHER_Encrypt) { CIPHER_ENOTSUP(f_SU_CIPHER_Encrypt(-1, NULL, NULL, 0), SU_CIPHER_Encrypt); }
    SUNEED(SU_CIPHER_Decrypt) { CIPHER_ENOTSUP(f_SU_CIPHER_Decrypt(-1, NULL, NULL, 0), SU_CIPHER_Decrypt); }
    SUNEED(SU_CIPHER_DES_Init) { CIPHER_ENOTSUP(f_SU_CIPHER_DES_Init(), SU_CIPHER_DES_Init); }
    SUNEED(SU_CIPHER_DES_Exit) { CIPHER_ENOTSUP(f_SU_CIPHER_DES_Exit(), SU_CIPHER_DES_Exit); }
    SUNEED(SU_CIPHER_DES_Test) { CIPHER_ENOTSUP(f_SU_CIPHER_DES_Test(), SU_CIPHER_DES_Test); }
#else
    if (!su_hw_tests() || access("/dev/aes", F_OK) != 0) {
        const char *why = !su_hw_tests() ? "hardware AES/DES DMA engine, runs in QEMU (APITEST_QEMU) or with APITEST_SU_HW=1" : "no /dev/aes (no AES driver; the vendor libsysutils opens the same node)";
        int na = su_hw_tests();     /* a run that wanted the engine but has no node: N/A, else SKIP */
        int v = na ? V_NA : V_SKIP;

        rep(FN(SU_CIPHER_Init), na ? -1 : 0, v, "%s", why);
        rep(FN(SU_CIPHER_Exit), na ? -1 : 0, v, "see CIPHER_Init");
        rep(FN(SU_CIPHER_CreateHandle), na ? -1 : 0, v, "see CIPHER_Init");
        rep(FN(SU_CIPHER_DestroyHandle), na ? -1 : 0, v, "see CIPHER_Init");
        rep(FN(SU_CIPHER_ConfigHandle), na ? -1 : 0, v, "see CIPHER_Init");
        rep(FN(SU_CIPHER_Encrypt), na ? -1 : 0, v, "see CIPHER_Init");
        rep(FN(SU_CIPHER_Decrypt), na ? -1 : 0, v, "see CIPHER_Init");
    } else {
        /* AES through /dev/aes: known-answer vectors (FIPS-197 ECB, SP 800-38A CBC) and the encrypt/decrypt round trip */
        SUNEED(SU_CIPHER_Init) {
            int m1 = 0, m2 = 0, rt1 = 0, rt2 = 0, st1 = 0, st2 = 0, sw1 = 0, rc1 = 0, rc2 = 0;

            int (*mal)(int) = (int (*)(int))SUSYMN("SU_CIPHER_Malloc_Buffer"), (*fre)(void) = (int (*)(void))SUSYMN("SU_CIPHER_Free_Buffer");

            r = f_SU_CIPHER_Init(); RET0(SU_CIPHER_Init, r, "/dev/aes opened");
            if (r == 0) {
                if (mal) mal(4096);      /* the vendor library stages the data in this buffer; without it SU_CIPHER_Encrypt faults */
                unsigned char fst[16] = { 0 }, cbcgot[16] = { 0 };
                int cbcvar = 0;
                static const int order[4] = { 0, 3, 1, 2 };
                int k;

                /* the byte order the driver wants for key/data words is not documented: try natural, both swapped, key only, data only */
                for (k = 0; k < 4; k++) {
                    sw1 = order[k];
                    rc1 = su_aes_run(IN_UNF_CIPHER_WORK_MODE_ECB, kat_ecb_key, NULL, kat_ecb_pt, kat_ecb_ct, 16, sw1, &m1, &rt1, &st1, k == 0 ? fst : NULL);
                    if (rc1 != 0 || m1) break;
                }
                rc2 = su_aes_run(IN_UNF_CIPHER_WORK_MODE_CBC, kat_cbc_key, kat_cbc_iv, kat_cbc_pt, kat_cbc_ct, 32, sw1, &m2, &rt2, &st2, cbcgot);
                if (rc2 == 0 && !m2) {      /* CBC: the vendor driver may use another word order for the IV and the output than for ECB */
                    static const int var[3] = { 4, 8, 12 };
                    int v;

                    for (v = 0; v < 3 && !m2; v++) {
                        int m3 = 0, rt3 = 0, st3 = 0;

                        if (su_aes_run(IN_UNF_CIPHER_WORK_MODE_CBC, kat_cbc_key, kat_cbc_iv, kat_cbc_pt, kat_cbc_ct, 32, sw1 | var[v], &m3, &rt3, &st3, NULL) == 0 && m3) { m2 = 1; rt2 = rt3; cbcvar = var[v]; }
                    }
                }
                SUNEED(SU_CIPHER_CreateHandle) rep(FN(SU_CIPHER_CreateHandle), rc1 == 1 ? st1 : 0, (rc1 != 1 && rc2 != 1) ? V_PASS : V_FAIL, "handle created and destroyed (ECB and CBC runs)");
                SUNEED(SU_CIPHER_DestroyHandle) rep(FN(SU_CIPHER_DestroyHandle), 0, (rc1 != 1 && rc2 != 1) ? V_PASS : V_FAIL, "closed after each run");
                SUNEED(SU_CIPHER_ConfigHandle) rep(FN(SU_CIPHER_ConfigHandle), (rc1 == 2 || rc2 == 2) ? st1 : 0, (rc1 != 1 && rc1 != 2 && rc2 != 1 && rc2 != 2) ? V_PASS : V_FAIL, "AES-128 ECB and CBC (+IV) configured");
                SUNEED(SU_CIPHER_Encrypt) rep(FN(SU_CIPHER_Encrypt), (rc1 == 3 || rc2 == 3) ? st1 : 0, (rc1 != 3 && rc2 != 3 && m1 && m2) ? V_PASS : V_FAIL,
                                              "FIPS-197 ECB vector %s, SP 800-38A CBC 2 blocks %s%s%s (steps %d/%d rc %d/%d)", m1 ? "ok" : "WRONG", m2 ? "ok" : "WRONG", cbcvar == 4 ? " (CBC: IV words not swapped)" : cbcvar == 8 ? " (CBC: output words in plain order)" : cbcvar == 12 ? " (CBC: IV not swapped, output plain)" : "", sw1 == 0 ? "" : sw1 == 3 ? " (key+data words byte-swapped)" : sw1 == 1 ? " (key words byte-swapped)" : " (data words byte-swapped)", rc1, rc2, st1, st2);
                if (!m2) printf("[T] AES CBC block 0 raw result %02x%02x%02x%02x%02x%02x%02x%02x... (expected 7649abac8119b246...)\n", cbcgot[0], cbcgot[1], cbcgot[2], cbcgot[3], cbcgot[4], cbcgot[5], cbcgot[6], cbcgot[7]);
                if (!m1) printf("[T] AES ECB natural-order result %02x%02x%02x%02x%02x%02x%02x%02x... (expected 69c4e0d86a7b0430...)\n", fst[0], fst[1], fst[2], fst[3], fst[4], fst[5], fst[6], fst[7]);
                SUNEED(SU_CIPHER_Decrypt) rep(FN(SU_CIPHER_Decrypt), (rc1 == 5 || rc2 == 5) ? st1 : 0, (rc1 == 0 && rc2 == 0 && rt1 && rt2) ? V_PASS : V_FAIL,
                                              "decrypt of the ciphertext gives the plaintext again: ECB %s, CBC %s", rt1 ? "yes" : "NO", rt2 ? "yes" : "NO");
            }
            if (fre) fre();
            SUNEED(SU_CIPHER_Exit) { r = f_SU_CIPHER_Exit(); RET0(SU_CIPHER_Exit, r, ""); }
        }
    }
#if HAS_SU_CIPHER_DES_Init
    if (su_hw_tests() && access("/dev/jz-des", F_OK) == 0) {
        SUNEED(SU_CIPHER_DES_Init) {
            r = f_SU_CIPHER_DES_Init(); RET0(SU_CIPHER_DES_Init, r, "/dev/jz-des opened");
            if (r == 0) {
                SUNEED(SU_CIPHER_DES_Test) { int rt = f_SU_CIPHER_DES_Test(); RET0(SU_CIPHER_DES_Test, rt, "DES handle open"); }
                SUNEED(SU_CIPHER_DES_Exit) { int rx = f_SU_CIPHER_DES_Exit(); RET0(SU_CIPHER_DES_Exit, rx, ""); }
            }
        }
    } else {
        int na = su_hw_tests();

        rep(FN(SU_CIPHER_DES_Init), na ? -1 : 0, na ? V_NA : V_SKIP, na ? "no /dev/jz-des (no DES driver)" : "see CIPHER_Init");
        rep(FN(SU_CIPHER_DES_Exit), na ? -1 : 0, na ? V_NA : V_SKIP, "see DES_Init");
        rep(FN(SU_CIPHER_DES_Test), na ? -1 : 0, na ? V_NA : V_SKIP, "see DES_Init");
    }
#endif
#endif
    if (g_su) dlclose(g_su);
}
