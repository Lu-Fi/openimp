/* apitest: sysutils (SU_*) functions.  They live in libsysutils.so, resolved with dlsym
 * (type-checked against the vendor headers). Never called: SU_Base_Shutdown/Reboot/Suspend. */
#include "apitest.h"

static void *g_su;
#define SUSYM(name) ((__typeof__(&name))dlsym(g_su, #name))
/* resolve into 'f'; N/A line when missing */
#define SUNEED(name) __typeof__(&name) f_##name = g_su ? SUSYM(name) : NULL; if (!f_##name) { rep_na(FN(name)); } else

void t_su(void)
{
    int r;

    rep_area("su");
    g_su = dlopen("libsysutils.so", RTLD_NOW);
    if (!g_su) {
        rep(LBL("libsysutils.so"), -1, V_NA, "dlopen failed: %s", dlerror());
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
    SUNEED(SU_Base_GetTime) {
        G(SUTime, t); G(SUTime, t2);
        G(uint32_t, raw); G(SUTime, back);
        r = f_SU_Base_GetTime(t); gchk(t);
        CHECK(SU_Base_GetTime, r, t->year >= 1970 && t->mon >= 1 && t->mon <= 12 && t->mday >= 1 && t->mday <= 31 && t->hour < 24 && t->min < 60 && t->sec < 62,
              "%04d-%02d-%02d %02d:%02d:%02d", t->year, t->mon, t->mday, t->hour, t->min, t->sec);
        SUNEED(SU_Base_SUTime2Raw) {
            int r2 = f_SU_Base_SUTime2Raw(t, raw); gchk(raw);
            RET0(SU_Base_SUTime2Raw, r2, "-> %u", *raw);
            SUNEED(SU_Base_Raw2SUTime) {
                int r3 = f_SU_Base_Raw2SUTime(raw, back); gchk(back);
                CHECK(SU_Base_Raw2SUTime, r3, back->year == t->year && back->mon == t->mon && back->mday == t->mday && back->hour == t->hour && back->min == t->min && back->sec == t->sec,
                      "%u -> %04d-%02d-%02d %02d:%02d:%02d (round trip)", *raw, back->year, back->mon, back->mday, back->hour, back->min, back->sec);
            }
        }
        SUNEED(SU_Base_SetTime) {
            int r2 = f_SU_Base_SetTime(t);       /* writes the time just read back: no change */
            f_SU_Base_GetTime(t2); gchk(t2);
            CHECK(SU_Base_SetTime, r2, t2->year == t->year && t2->mon == t->mon && t2->mday == t->mday, "same time written back, now %04d-%02d-%02d %02d:%02d:%02d", t2->year, t2->mon, t2->mday, t2->hour, t2->min, t2->sec);
        }
        gfree(t); gfree(t2); gfree(raw); gfree(back);
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
    rep(FN(SU_Base_EnableAlarm), 0, V_SKIP, "an enabled RTC alarm can wake/power the camera");
    rep(FN(SU_Base_PollingAlarm), 0, V_SKIP, "blocks until the alarm fires");
    rep(FN(SU_Base_Shutdown), 0, V_SKIP, "powers the camera off");
    rep(FN(SU_Base_Reboot), 0, V_SKIP, "reboots the camera");
    rep(FN(SU_Base_Suspend), 0, V_SKIP, "suspends the camera");
#if HAS_SU_Base_SetWkupMode
    rep(FN(SU_Base_SetWkupMode), 0, V_SKIP, "changes the wake-up configuration");
#endif

    /* misc / ADC / battery / cipher / key / LED */
#if HAS_SU_ADC_Init
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
#endif
#if HAS_SU_Battery_GetCapacity
    SUNEED(SU_Battery_GetCapacity) { r = f_SU_Battery_GetCapacity(); rep(FN(SU_Battery_GetCapacity), r, (r >= 0 && r <= 100) ? V_PASS : V_FAIL, "%d %% (negative = no battery)", r); }
    SUNEED(SU_Battery_GetVoltageUV) { r = f_SU_Battery_GetVoltageUV(); rep(FN(SU_Battery_GetVoltageUV), r, r >= 0 ? V_PASS : V_FAIL, "%d uV", r); }
    SUNEED(SU_Battery_GetStatus) { G(SUBatStatus, v); r = f_SU_Battery_GetStatus(v); gchk(v); RET0(SU_Battery_GetStatus, r, "status %d", (int)*v); gfree(v); }
    rep(FN(SU_Battery_GetEvent), 0, V_SKIP, "blocks until a battery event");
#endif
    SUNEED(SU_Key_OpenEvent) {
        int fd = f_SU_Key_OpenEvent();

        rep(FN(SU_Key_OpenEvent), fd, fd >= 0 ? V_PASS : V_FAIL, "event fd %d (FAIL without an input device is normal on boards without keys)", fd);
        SUNEED(SU_Key_CloseEvent) { r = fd >= 0 ? f_SU_Key_CloseEvent(fd) : -1; RET0(SU_Key_CloseEvent, r, "fd %d", fd); }
    }
    rep(FN(SU_Key_ReadEvent), 0, V_SKIP, "blocks until a key is pressed");
    rep(FN(SU_Key_EnableEvent), 0, V_SKIP, "masks a hardware key in the input driver");
    rep(FN(SU_Key_DisableEvent), 0, V_SKIP, "masks a hardware key in the input driver");
    rep(FN(SU_LED_Command), 0, V_SKIP, "drives the board LED");
    rep(FN(SU_CIPHER_Init), 0, V_SKIP, "hardware AES/DES DMA engine, not part of the base/misc set");
    rep(FN(SU_CIPHER_Exit), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_CreateHandle), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_DestroyHandle), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_ConfigHandle), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_Encrypt), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_Decrypt), 0, V_SKIP, "see CIPHER_Init");
#if HAS_SU_CIPHER_DES_Init
    rep(FN(SU_CIPHER_DES_Init), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_DES_Exit), 0, V_SKIP, "see CIPHER_Init");
    rep(FN(SU_CIPHER_DES_Test), 0, V_SKIP, "see CIPHER_Init");
#endif
    if (g_su) dlclose(g_su);
}
