/*
 * su_sysutils.c - the vendor libsysutils functions of the T40 backlog (#70) inside libimp:
 * RTC time / alarm, reboot, suspend, ADC, key events, LED.  Ported from the vendor libsysutils
 * 1.3.1 disassembly (sysutils/base/base.c, misc/adc.c, misc/key.c, misc/led.c); the open stack
 * ships no libsysutils.so, so a program that links the SU_* names finds them here.
 *
 *   SU_Base_GetTime/SetTime       /dev/rtc0 RTC_RD_TIME / RTC_SET_TIME, month 1..12, year as is
 *   SU_Base_GetAlarm/SetAlarm     RTC_ALM_READ / RTC_ALM_SET
 *   SU_Base_EnableAlarm           refuses (-1) unless the alarm lies after the current time,
 *                                 then RTC_AIE_ON; SU_Base_DisableAlarm RTC_AIE_OFF
 *   SU_Base_PollingAlarm(ms)      select() on the RTC fd; the vendor returns 0 also on timeout
 *   SU_Base_Reboot                sync(), SIGTERM to init (busybox: reboot), returns 0
 *   SU_Base_Suspend               writes "mem" to /sys/power/state
 *   SU_ADC_*                      six channels /dev/ingenic_adc_aux_0..5: ioctl 1 enable, 2 disable, read(4) value
 *   SU_Key_*                      the input device named "gpio_keys"; Enable/DisableEvent edit the gpio_keys
 *                                 sysfs lists (keys, disabled_keys)
 *   SU_LED_Command(n, cmd)        sysfs GPIO n: export, active_low 0, direction out, value, unexport for cmd 2
 *   SU_CIPHER_*                   the vendor cipher.c flow: /dev/aes (GET_PBUFF, /dev/mem 2 MB mapping,
 *                                 SET_PARA, START_EN/DE) and /dev/jz-des (driver self tests), su_cipher.h codes
 *
 * Differences to the vendor library (documented): a NULL argument is -1 instead of a fault; the
 * key lookup returns -1 when no gpio_keys device exists (the vendor opens an uninitialised path);
 * every handle the vendor leaks on an error path is closed; no imp_log output (stderr "openimp-su").
 * Host tests replace the device nodes and the ioctl through the OPENIMP_SU_* macros.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef OPENIMP_SU_IOCTL
#define OPENIMP_SU_IOCTL ioctl
#endif
#ifndef OPENIMP_SU_KILL
#define OPENIMP_SU_KILL kill
#endif
#ifndef OPENIMP_SU_SYNC
#define OPENIMP_SU_SYNC sync
#endif
#ifndef OPENIMP_SU_RTC
#define OPENIMP_SU_RTC "/dev/rtc0"
#endif
#ifndef OPENIMP_SU_POWER_STATE
#define OPENIMP_SU_POWER_STATE "/sys/power/state"
#endif
#ifndef OPENIMP_SU_ADC_PREFIX
#define OPENIMP_SU_ADC_PREFIX "/dev/ingenic_adc_aux_"
#endif
#ifndef OPENIMP_SU_INPUT_DIR
#define OPENIMP_SU_INPUT_DIR "/dev/input"
#endif
#ifndef OPENIMP_SU_KEYS
#define OPENIMP_SU_KEYS "/sys/devices/platform/gpio_keys/keys"
#endif
#ifndef OPENIMP_SU_DISABLED_KEYS
#define OPENIMP_SU_DISABLED_KEYS "/sys/devices/platform/gpio_keys/disabled_keys"
#endif
#ifndef OPENIMP_SU_GPIO
#define OPENIMP_SU_GPIO "/sys/class/gpio/"
#endif

#define SU_ERR(...) do { fprintf(stderr, "openimp-su: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* SUTime of sysutils/su_base.h: sec, min, hour, mday, mon (1..12), year (>1900) */
typedef struct { int sec, min, hour, mday, mon, year; } su_time;

/* struct rtc_time of linux/rtc.h (36 bytes) and the ioctl numbers of /dev/rtc */
struct su_rtc_time { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
#define SU_RTC_AIE_ON 0x20007001U
#define SU_RTC_AIE_OFF 0x20007002U
#define SU_RTC_ALM_SET 0x80247007U
#define SU_RTC_ALM_READ 0x40247008U
#define SU_RTC_RD_TIME 0x40247009U
#define SU_RTC_SET_TIME 0x8024700aU

static int rtc_read(unsigned int cmd, su_time *t, const char *what)
{
    struct su_rtc_time r;
    int fd, rc;

    if (!t)
        return -1;
    fd = open(OPENIMP_SU_RTC, O_RDONLY);
    if (fd < 0) {
        SU_ERR("%s: open %s: %s", what, OPENIMP_SU_RTC, strerror(errno));
        return -1;
    }
    memset(&r, 0, sizeof(r));
    rc = OPENIMP_SU_IOCTL(fd, cmd, &r);
    close(fd);
    if (rc < 0) {
        SU_ERR("%s: ioctl failed: %s", what, strerror(errno));
        return -1;
    }
    t->sec = r.sec; t->min = r.min; t->hour = r.hour; t->mday = r.mday;
    t->mon = r.mon + 1; t->year = r.year + 1900;
    return 0;
}

static int rtc_write(unsigned int cmd, const su_time *t, const char *what)
{
    struct su_rtc_time r;
    int fd, rc;

    if (!t)
        return -1;
    fd = open(OPENIMP_SU_RTC, O_RDONLY);
    if (fd < 0) {
        SU_ERR("%s: open %s: %s", what, OPENIMP_SU_RTC, strerror(errno));
        return -1;
    }
    memset(&r, 0, sizeof(r));
    r.sec = t->sec; r.min = t->min; r.hour = t->hour; r.mday = t->mday;
    r.mon = t->mon - 1; r.year = t->year - 1900;
    rc = OPENIMP_SU_IOCTL(fd, cmd, &r);
    if (rc == -1)
        SU_ERR("%s: ioctl failed: %s", what, strerror(errno));
    close(fd);
    return rc;
}

int SU_Base_GetTime(su_time *t)
{
    return rtc_read(SU_RTC_RD_TIME, t, "SU_Base_GetTime");
}

int SU_Base_SetTime(su_time *t)
{
    return rtc_write(SU_RTC_SET_TIME, t, "SU_Base_SetTime");
}

int SU_Base_GetAlarm(su_time *t)
{
    return rtc_read(SU_RTC_ALM_READ, t, "SU_Base_GetAlarm");
}

int SU_Base_SetAlarm(su_time *t)
{
    return rtc_write(SU_RTC_ALM_SET, t, "SU_Base_SetAlarm");
}

static int su_time_after(const su_time *a, const su_time *b)     /* a later than b */
{
    if (a->year != b->year) return a->year > b->year;
    if (a->mon != b->mon) return a->mon > b->mon;
    if (a->mday != b->mday) return a->mday > b->mday;
    if (a->hour != b->hour) return a->hour > b->hour;
    if (a->min != b->min) return a->min > b->min;
    return a->sec > b->sec;
}

static int rtc_ctl(unsigned int cmd, const char *what)
{
    int fd = open(OPENIMP_SU_RTC, O_RDONLY), rc;

    if (fd < 0) {
        SU_ERR("%s: open %s: %s", what, OPENIMP_SU_RTC, strerror(errno));
        return -1;
    }
    rc = OPENIMP_SU_IOCTL(fd, cmd, 0);
    if (rc == -1)
        SU_ERR("%s: ioctl failed: %s", what, strerror(errno));
    close(fd);
    return rc;
}

int SU_Base_EnableAlarm(void)
{
    su_time now, alarm;

    if (SU_Base_GetTime(&now) < 0 || SU_Base_GetAlarm(&alarm) < 0)
        return -1;
    if (!su_time_after(&alarm, &now)) {
        SU_ERR("SU_Base_EnableAlarm: the alarm time is not after the current time");
        return -1;
    }
    return rtc_ctl(SU_RTC_AIE_ON, "SU_Base_EnableAlarm");
}

int SU_Base_DisableAlarm(void)
{
    return rtc_ctl(SU_RTC_AIE_OFF, "SU_Base_DisableAlarm");
}

int SU_Base_PollingAlarm(uint32_t timeoutMsec)
{
    fd_set set;
    struct timeval tv;
    int fd = open(OPENIMP_SU_RTC, O_RDONLY), rc;

    if (fd < 0) {
        SU_ERR("SU_Base_PollingAlarm: open %s: %s", OPENIMP_SU_RTC, strerror(errno));
        return -1;
    }
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = timeoutMsec / 1000;
    tv.tv_usec = (timeoutMsec % 1000) * 1000;
    rc = select(fd + 1, &set, NULL, NULL, &tv);
    if (rc < 0)
        SU_ERR("SU_Base_PollingAlarm: select error: %s", strerror(errno));
    else if (rc == 0)
        SU_ERR("SU_Base_PollingAlarm: select timeout");
    close(fd);
    return 0;               /* the vendor returns 0 also after an error or a timeout */
}

int SU_Base_Reboot(void)
{
    OPENIMP_SU_SYNC();
    OPENIMP_SU_KILL(1, SIGTERM);
    return 0;
}

static pthread_mutex_t su_suspend_lock = PTHREAD_MUTEX_INITIALIZER;

int SU_Base_Suspend(void)
{
    int fd, rc = 0;

    pthread_mutex_lock(&su_suspend_lock);
    fd = open(OPENIMP_SU_POWER_STATE, O_RDWR);
    if (fd < 0) {
        SU_ERR("SU_Base_Suspend: open %s: %s", OPENIMP_SU_POWER_STATE, strerror(errno));
        rc = -1;
    } else {
        if (write(fd, "mem", 3) != 3) {
            SU_ERR("SU_Base_Suspend: write failed: %s", strerror(errno));
            rc = -1;
        }
        close(fd);
    }
    pthread_mutex_unlock(&su_suspend_lock);
    return rc;
}

/* ---- ADC ---- */
#define SU_ADC_CHANNELS 6
static int su_adc_fd[SU_ADC_CHANNELS];
static int su_adc_ready;

int SU_ADC_Init(void)
{
    int fds[SU_ADC_CHANNELS], i;
    char path[256];

    for (i = 0; i < SU_ADC_CHANNELS; i++) {
        snprintf(path, sizeof(path), "%s%d", OPENIMP_SU_ADC_PREFIX, i);
        fds[i] = open(path, O_RDONLY);
        if (fds[i] < 0) {
            SU_ERR("SU_ADC_Init: open path %s error: %s", path, strerror(errno));
            while (i--)
                close(fds[i]);
            return -1;
        }
    }
    memcpy(su_adc_fd, fds, sizeof(fds));
    su_adc_ready = 1;
    return 0;
}

int SU_ADC_Exit(void)
{
    int i;

    if (su_adc_ready)
        for (i = 0; i < SU_ADC_CHANNELS; i++)
            close(su_adc_fd[i]);
    su_adc_ready = 0;
    return 0;
}

static int su_adc_ctl(uint32_t chn, int cmd, const char *what)
{
    if (!su_adc_ready) {
        SU_ERR("%s: adc is NULL !", what);
        return -1;
    }
    if (chn >= SU_ADC_CHANNELS) {
        SU_ERR("%s: chn_num error !", what);
        return -1;
    }
    if (OPENIMP_SU_IOCTL(su_adc_fd[chn], cmd, 0) < 0) {
        SU_ERR("%s: chn_num=%u, ioctl(%d, %d): %s", what, chn, su_adc_fd[chn], cmd, strerror(errno));
        return -1;
    }
    return 0;
}

int SU_ADC_EnableChn(uint32_t chn)
{
    return su_adc_ctl(chn, 1, "SU_ADC_EnableChn");
}

int SU_ADC_DisableChn(uint32_t chn)
{
    return su_adc_ctl(chn, 2, "SU_ADC_DisableChn");
}

int SU_ADC_GetChnValue(uint32_t chn, int *value)
{
    if (!su_adc_ready) {
        SU_ERR("SU_ADC_GetChnValue: adc is NULL !");
        return -1;
    }
    if (chn >= SU_ADC_CHANNELS) {
        SU_ERR("SU_ADC_GetChnValue: chn_num error !");
        return -1;
    }
    if (!value) {
        SU_ERR("SU_ADC_GetChnValue: value error !");
        return -1;
    }
    if (read(su_adc_fd[chn], value, 4) < 0) {
        SU_ERR("SU_ADC_GetChnValue: chn_num=%u, read(%d) failed: %s", chn, su_adc_fd[chn], strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- keys ---- */
#define SU_KEY_MAX 32
#define SU_EVIOCGNAME79 0x404f4506U

struct su_key { uint16_t code, disabled; };
static struct su_key su_keys[SU_KEY_MAX];
static int su_nkeys;
static int su_keys_loaded;

int SU_Key_OpenEvent(void)
{
    DIR *d = opendir(OPENIMP_SU_INPUT_DIR);
    struct dirent *e;
    char path[512];
    int found = 0;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char name[80];
        int fd = openat(dirfd(d), e->d_name, O_RDONLY);

        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        if (OPENIMP_SU_IOCTL(fd, SU_EVIOCGNAME79, name) <= 0) {
            close(fd);
            continue;
        }
        close(fd);
        if (!strcmp(name, "gpio_keys")) {
            snprintf(path, sizeof(path), "%s/%s", OPENIMP_SU_INPUT_DIR, e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    if (!found)
        return -1;
    return open(path, O_RDONLY);
}

int SU_Key_CloseEvent(int fd)
{
    return close(fd);
}

int SU_Key_ReadEvent(int fd, int *keyCode, int *event)
{
    struct { struct timeval tv; uint16_t type, code; int32_t value; } ev;      /* struct input_event, 16 bytes on MIPS32 */

    if (!keyCode || !event)
        return -1;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));

        if (n < 1) {
            SU_ERR("SU_Key_ReadEvent: read key event error: %s", strerror(errno));
            return -1;
        }
        if (n == (ssize_t)sizeof(ev) && ev.type == 1)       /* EV_KEY */
            break;
    }
    *event = ev.value != 0 ? 1 : 0;                         /* KEY_RELEASED 0, KEY_PRESSED 1 */
    *keyCode = ev.code & 0xff;
    return 0;
}

static int su_read_trim(int fd, char *buf, size_t size)
{
    char *p;
    ssize_t n = read(fd, buf, size - 1);

    if (n < 0)
        return -1;
    buf[n] = 0;
    if ((p = strrchr(buf, '\n')) != NULL) *p = 0;
    if ((p = strrchr(buf, '\r')) != NULL) *p = 0;
    return 0;
}

static int su_load_keys(void)
{
    char buf[128], *save = NULL, *tok;
    int fd = open(OPENIMP_SU_KEYS, O_RDONLY), n = 0;

    if (fd < 0) {
        SU_ERR("open keys error: %s", strerror(errno));
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    if (su_read_trim(fd, buf, sizeof(buf)) < 0) {
        SU_ERR("read keys error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (n == SU_KEY_MAX) {
            SU_ERR("Too many keys");
            return -1;
        }
        su_keys[n].code = (uint16_t)atoi(tok);
        su_keys[n].disabled = 0;
        n++;
    }
    su_nkeys = n;
    su_keys_loaded = 1;
    return 0;
}

/* one pass of the vendor update_enabled_keys / update_disabled_keys: `want` is the comma list of the keys
 * whose state changed; enabling removes them from the disabled_keys file content, disabling appends the
 * ones that are not in it.  The vendor writes strlen(want) + 1 bytes of the new content. */
static int su_update_keys(int disable)
{
    char want[128], cur[128], tmp[16];
    char *tok, *comma, *p;
    int fd, i, changed = 0;

    memset(want, 0, sizeof(want));
    memset(cur, 0, sizeof(cur));
    for (i = 0; i < su_nkeys; i++) {
        if ((su_keys[i].disabled != 0) == (disable != 0)) {
            snprintf(tmp, sizeof(tmp), "%d,", su_keys[i].code);
            if (strlen(want) + strlen(tmp) < sizeof(want))
                strcat(want, tmp);
        }
    }
    if (!want[0])
        return 0;
    if ((p = strrchr(want, ',')) != NULL)
        *p = 0;
    fd = open(OPENIMP_SU_DISABLED_KEYS, O_RDWR);
    if (fd < 0) {
        SU_ERR("open %s error: %s", OPENIMP_SU_DISABLED_KEYS, strerror(errno));
        return -1;
    }
    if (su_read_trim(fd, cur, sizeof(cur)) < 0) {
        SU_ERR("read disabled_keys error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    tok = want;
    if (!disable) {
        if (!cur[0]) {
            close(fd);
            return 0;
        }
        do {
            char *hit;

            comma = strchr(tok, ',');
            if (comma && *comma)
                *comma = 0;
            hit = strstr(cur, tok);
            if (hit) {
                char *next = strchr(hit, ',');

                *hit = 0;
                if (next)
                    strcat(cur, next + 1);
                changed++;
            }
            tok = comma ? comma + 1 : NULL;
        } while (tok);
        if (!changed) {
            close(fd);
            return 0;
        }
    } else if (!cur[0]) {
        strcat(cur, want);
    } else {
        do {
            comma = strchr(tok, ',');
            if (comma && *comma)
                *comma = 0;
            if (!strstr(cur, tok)) {
                strcat(cur, ",");
                changed++;
                strcat(cur, tok);
            }
            tok = comma ? comma + 1 : NULL;
        } while (tok);
        if (!changed) {
            close(fd);
            return 0;
        }
    }
    if (write(fd, cur, strlen(want) + 1) < 0) {
        SU_ERR("write disabled_keys error: %s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int su_key_set(int keyCode, int disable)
{
    int i;

    if (!su_keys_loaded && su_load_keys() < 0)
        return -1;
    for (i = 0; i < su_nkeys; i++)
        if (su_keys[i].code == (uint16_t)keyCode) {
            su_keys[i].disabled = (uint16_t)disable;
            break;
        }
    return su_update_keys(disable);
}

int SU_Key_EnableEvent(int keyCode)
{
    return su_key_set(keyCode, 0);
}

int SU_Key_DisableEvent(int keyCode)
{
    return su_key_set(keyCode, 1);
}

/* ---- LED (sysfs GPIO) ---- */
static int su_sysfs_write(const char *path, const char *value, const char *what)
{
    char buf[64];
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        SU_ERR("SU_LED_Command: open() %s error ! (%s)", what, path);
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%s", value);
    (void)!write(fd, buf, sizeof(buf));         /* the vendor does not look at the result either */
    close(fd);
    return 0;
}

int SU_LED_Command(int ledNum, int cmd)
{
    char path[512], val[16];

    snprintf(val, sizeof(val), "%d", ledNum);
    snprintf(path, sizeof(path), "%sexport", OPENIMP_SU_GPIO);
    if (su_sysfs_write(path, val, "export") < 0)
        return -1;
    snprintf(path, sizeof(path), "%sgpio%d/active_low", OPENIMP_SU_GPIO, ledNum);
    if (su_sysfs_write(path, "0", "active_low") < 0)
        return -1;
    snprintf(path, sizeof(path), "%sgpio%d/direction", OPENIMP_SU_GPIO, ledNum);
    if (su_sysfs_write(path, "out", "direction") < 0)
        return -1;
    snprintf(path, sizeof(path), "%sgpio%d/value", OPENIMP_SU_GPIO, ledNum);
    snprintf(val, sizeof(val), "%d", cmd > 1 ? (cmd != 2) : cmd);
    if (su_sysfs_write(path, val, "value") < 0)
        return -1;
    if (cmd != 2)
        return 0;
    snprintf(path, sizeof(path), "%sunexport", OPENIMP_SU_GPIO);
    snprintf(val, sizeof(val), "%d", ledNum);
    return su_sysfs_write(path, val, "unexport");
}

/* ---- cipher: the vendor libsysutils 1.3.x cipher.c flow ----
 * AES: /dev/aes (ioctl GET_PBUFF gives the physical address of a 2 MB DMA buffer, mapped through
 * /dev/mem; per handle 1 MB in / 1 MB out are shared by all 16 handle slots), SET_PARA hands the
 * slot of the handle to the driver, START_EN/DE_PROCESSING runs it.  DES: /dev/jz-des, only the
 * driver self tests (ioctl 'B' 0x73..0x82).  Return codes are the ones of sysutils/su_cipher.h.
 * Differences to the vendor (documented, all on error paths): every mutex is released on error,
 * Init closes what it opened when a later step fails, Exit / DES_Exit forget the closed fds (the
 * vendor answers REINIT to the next Init), handle numbers are range-checked (the vendor indexes
 * its slot array unchecked), CreateHandle stops at 16 slots, DES_Test answers UNINIT before
 * DES_Init (the vendor runs the ioctls on fd -1 and reports every self test as passed). */
#ifndef OPENIMP_SU_AES
#define OPENIMP_SU_AES "/dev/aes"
#endif
#ifndef OPENIMP_SU_DES
#define OPENIMP_SU_DES "/dev/jz-des"
#endif
#ifndef OPENIMP_SU_MEM
#define OPENIMP_SU_MEM "/dev/mem"
#endif
#ifndef OPENIMP_SU_MMAP
#define OPENIMP_SU_MMAP mmap
#endif
#ifndef OPENIMP_SU_MUNMAP
#define OPENIMP_SU_MUNMAP munmap
#endif

#define SU_CIPHER_REINIT (-10)
#define SU_CIPHER_INIT_FAILED (-11)
#define SU_CIPHER_FAILED_GETHANDLE (-12)
#define SU_CIPHER_INVALID_PARA (-13)
#define SU_CIPHER_SET_PARA_FAILED (-14)
#define SU_CIPHER_FAILURE (-15)
#define SU_CIPHER_SET_DATALEN_ERR (-16)
#define SU_CIPHER_EXIT_ERR (-17)
#define SU_CIPHER_UNINIT (-18)
#define SU_CIPHER_FAILED_DESHANDLE (-19)

#define SU_AES_GET_PBUFF 0x2000416eU          /* _IO('A', 110) */
#define SU_AES_SET_PARA 0x2000416fU           /* _IO('A', 111) */
#define SU_AES_START_EN 0x20004170U           /* _IO('A', 112) */
#define SU_AES_START_DE 0x20004171U           /* _IO('A', 113) */
#define SU_CIPHER_SLOTS 16
#define SU_CIPHER_MAP_LEN 0x200000u
#define SU_CIPHER_HALF 0x100000u

/* IN_UNF_CIPHER_CTRL of sysutils/su_cipher.h (13 words) */
typedef struct {
    unsigned int key[4], iv[4], len, alg, bitwidth, mode, keylen;
} su_cipher_ctrl;

/* one handle slot, 76 bytes on the camera: the record SET_PARA passes to the driver */
struct su_cipher_slot {
    unsigned int pbuff;                        /* GET_PBUFF writes the buffer address here (slot 0) */
    unsigned int used;
    unsigned int key[4], iv[4];
    unsigned int src_p;
    unsigned char *src_v;
    unsigned int dst_p;
    unsigned char *dst_v;
    unsigned int alg, bitwidth, mode, keylen;
    unsigned int datalen;
};
#if defined(__mips__) && !defined(__LP64__)
typedef char su_cipher_slot_size_check[sizeof(struct su_cipher_slot) == 0x4c ? 1 : -1];
#endif

static int su_aes_fd = -1, su_des_fd = -1, su_mem_fd = -1, su_cipher_last = -1, su_cipher_count;
static unsigned char *su_cipher_map;
static struct su_cipher_slot su_cipher_para[SU_CIPHER_SLOTS];
static pthread_mutex_t su_cipher_handle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t su_cipher_mutex = PTHREAD_MUTEX_INITIALIZER;

int SU_CIPHER_Init(void)
{
    unsigned int phys;
    void *m;
    int i;

    if (su_aes_fd >= 0) {
        SU_ERR("SU_CIPHER_Init: you have already init cipher before!");
        return SU_CIPHER_REINIT;
    }
    su_aes_fd = open(OPENIMP_SU_AES, O_RDONLY);
    if (su_aes_fd < 0) {
        SU_ERR("SU_CIPHER_Init: open %s: %s", OPENIMP_SU_AES, strerror(errno));
        return SU_CIPHER_INIT_FAILED;
    }
    if (OPENIMP_SU_IOCTL(su_aes_fd, SU_AES_GET_PBUFF, &su_cipher_para[0]) != 0) {
        SU_ERR("SU_CIPHER_Init: ioctl IOCTL_AES_GET_PBUFF error");
        goto fail;
    }
    su_mem_fd = open(OPENIMP_SU_MEM, O_RDWR | O_DSYNC);    /* vendor flags 0x12 (MIPS O_RDWR|O_DSYNC) */
    if (su_mem_fd < 0) {
        SU_ERR("SU_CIPHER_Init: open %s: %s", OPENIMP_SU_MEM, strerror(errno));
        goto fail;
    }
    phys = su_cipher_para[0].pbuff;
    m = OPENIMP_SU_MMAP(NULL, SU_CIPHER_MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, su_mem_fd,
                        (off_t)(phys & 0x1fffffffu));
    if (m == MAP_FAILED) {                     /* the vendor does not check the mapping */
        SU_ERR("SU_CIPHER_Init: mmap failed: %s", strerror(errno));
        goto fail;
    }
    su_cipher_map = m;
    for (i = 0; i < SU_CIPHER_SLOTS; i++) {
        su_cipher_para[i].src_p = phys;
        su_cipher_para[i].src_v = su_cipher_map;
        su_cipher_para[i].dst_p = phys + SU_CIPHER_HALF;
        su_cipher_para[i].dst_v = su_cipher_map + SU_CIPHER_HALF;
    }
    return 0;
fail:
    if (su_mem_fd >= 0)
        close(su_mem_fd);
    su_mem_fd = -1;
    close(su_aes_fd);
    su_aes_fd = -1;
    return SU_CIPHER_INIT_FAILED;
}

int SU_CIPHER_Exit(void)
{
    if (su_aes_fd < 0) {
        SU_ERR("SU_CIPHER_Exit: you aren't init cipher before!");
        return SU_CIPHER_UNINIT;
    }
    if (su_cipher_count != 0) {
        SU_ERR("SU_CIPHER_Exit: Cipher module are used to other module now!");
        return SU_CIPHER_EXIT_ERR;
    }
    close(su_aes_fd);
    close(su_mem_fd);
    OPENIMP_SU_MUNMAP(su_cipher_map, SU_CIPHER_MAP_LEN);
    su_cipher_map = NULL;
    su_aes_fd = su_mem_fd = -1;
    return 0;
}

int SU_CIPHER_DES_Init(void)
{
    if (su_des_fd >= 0) {
        SU_ERR("SU_CIPHER_DES_Init: you have already init cipher des before!");
        return SU_CIPHER_REINIT;
    }
    su_des_fd = open(OPENIMP_SU_DES, O_RDONLY);
    if (su_des_fd < 0) {
        SU_ERR("SU_CIPHER_DES_Init: open %s: %s", OPENIMP_SU_DES, strerror(errno));
        return SU_CIPHER_INIT_FAILED;
    }
    return 0;
}

int SU_CIPHER_DES_Exit(void)
{
    if (su_des_fd < 0) {
        SU_ERR("SU_CIPHER_DES_Exit: you aren't init cipher des before!");
        return SU_CIPHER_UNINIT;
    }
    close(su_des_fd);
    su_des_fd = -1;
    return 0;
}

/* the driver self tests in the vendor order; the vendor counts a non-zero ioctl answer as "pass" */
int SU_CIPHER_DES_Test(void)
{
    static const struct { unsigned char nr; const char *name; } t[] = {
        {0x73, "DES_EBC_CPU_EN"}, {0x74, "DES_EBC_CPU_DE"}, {0x75, "DES_CBC_CPU_EN"},
        {0x76, "DES_CBC_CPU_DE"}, {0x7b, "DES_EBC_CPU_EN_TDES"}, {0x7c, "DES_EBC_CPU_DE_TDES"},
        {0x7d, "DES_CBC_CPU_EN_TDES"}, {0x7e, "DES_CBC_CPU_DE_TDES"}, {0x77, "DES_EBC_DMA_EN"},
        {0x78, "DES_EBC_DMA_DE"}, {0x79, "DES_CBC_DMA_EN"}, {0x7a, "DES_CBC_DMA_DE"},
        {0x7f, "DES_EBC_DMA_EN_TDES"}, {0x80, "DES_EBC_DMA_DE_TDES"}, {0x81, "DES_CBC_DMA_EN_TDES"},
        {0x82, "DES_CBC_DMA_DE_TDES"},
    };
    unsigned int i;

    if (su_des_fd < 0) {
        SU_ERR("SU_CIPHER_DES_Test: you aren't init cipher des before!");
        return SU_CIPHER_UNINIT;
    }
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (OPENIMP_SU_IOCTL(su_des_fd, 0x20004200U | t[i].nr, NULL) == 0) {
            printf("%s failed!\n", t[i].name);
            return -1;
        }
        printf("%s pass!\n", t[i].name);
    }
    return 0;
}

int SU_CIPHER_CreateHandle(void)
{
    int i;

    pthread_mutex_lock(&su_cipher_handle_mutex);
    if (su_aes_fd < 0) {
        pthread_mutex_unlock(&su_cipher_handle_mutex);
        SU_ERR("SU_CIPHER_CreateHandle: %s not open, CreateHandle error", OPENIMP_SU_AES);
        return SU_CIPHER_FAILED_GETHANDLE;
    }
    for (i = 0; i <= su_cipher_last; i++)
        if (!su_cipher_para[i].used)
            break;
    if (i > su_cipher_last) {
        if (su_cipher_last + 1 >= SU_CIPHER_SLOTS) {
            pthread_mutex_unlock(&su_cipher_handle_mutex);
            SU_ERR("SU_CIPHER_CreateHandle: all %d handles in use", SU_CIPHER_SLOTS);
            return SU_CIPHER_FAILED_GETHANDLE;
        }
        i = ++su_cipher_last;
    }
    su_cipher_para[i].used = 1;
    su_cipher_count++;
    pthread_mutex_unlock(&su_cipher_handle_mutex);
    return i;
}

int SU_CIPHER_DestroyHandle(int fd)
{
    pthread_mutex_lock(&su_cipher_handle_mutex);
    if (su_cipher_count == 0) {
        pthread_mutex_unlock(&su_cipher_handle_mutex);
        SU_ERR("SU_CIPHER_DestroyHandle: There is no handle need destroy!");
        return SU_CIPHER_FAILED_DESHANDLE;
    }
    if (fd < 0 || fd >= SU_CIPHER_SLOTS || !su_cipher_para[fd].used) {
        pthread_mutex_unlock(&su_cipher_handle_mutex);
        SU_ERR("SU_CIPHER_DestroyHandle: handle %d is not open", fd);
        return SU_CIPHER_FAILED_DESHANDLE;
    }
    su_cipher_count--;
    su_cipher_para[fd].used = 0;
    pthread_mutex_unlock(&su_cipher_handle_mutex);
    return 0;
}

int SU_CIPHER_ConfigHandle(int h, void *ctrl)
{
    const su_cipher_ctrl *c = ctrl;
    struct su_cipher_slot *s;

    if (!c) {
        SU_ERR("SU_CIPHER_ConfigHandle: para is NULL, set para error!");
        return SU_CIPHER_SET_PARA_FAILED;
    }
    if (h < 0 || h >= SU_CIPHER_SLOTS) {
        SU_ERR("SU_CIPHER_ConfigHandle: invalid handle %d", h);
        return SU_CIPHER_INVALID_PARA;
    }
    s = &su_cipher_para[h];
    memcpy(s->key, c->key, sizeof(s->key));
    memcpy(s->iv, c->iv, sizeof(s->iv));
    s->alg = c->alg;
    s->bitwidth = c->bitwidth;
    s->mode = c->mode;
    s->keylen = c->keylen;
    s->datalen = c->len;
    return 0;
}

static int su_cipher_run(int h, const unsigned int *src, unsigned int *dst, unsigned int len,
                         unsigned int cmd, const char *what)
{
    struct su_cipher_slot *s;
    int rc = 0;

    if (len - 16u >= 0xffff1u) {               /* 16 <= len <= 1 MB */
        SU_ERR("%s: dataLen is not suitable, 16 <= dataLen <= 1024*1024", what);
        return SU_CIPHER_SET_DATALEN_ERR;
    }
    if (h < 0 || h >= SU_CIPHER_SLOTS) {
        SU_ERR("%s: invalid handle %d", what, h);
        return SU_CIPHER_INVALID_PARA;
    }
    pthread_mutex_lock(&su_cipher_mutex);
    s = &su_cipher_para[h];
    s->datalen = len;
    if (OPENIMP_SU_IOCTL(su_aes_fd, SU_AES_SET_PARA, s) != 0) {
        SU_ERR("%s: ioctl IOCTL_AES_SET_PARA error", what);
        rc = SU_CIPHER_SET_PARA_FAILED;
    } else if (!src || !dst) {
        rc = SU_CIPHER_INVALID_PARA;
    } else {
        memcpy(s->src_v, src, len);
        if (OPENIMP_SU_IOCTL(su_aes_fd, cmd, NULL) != 0) {
            SU_ERR("%s: ioctl processing error", what);
            rc = SU_CIPHER_FAILURE;
        } else {
            memcpy(dst, s->dst_v, len);
        }
    }
    pthread_mutex_unlock(&su_cipher_mutex);
    return rc;
}

int SU_CIPHER_Encrypt(int h, unsigned int *src, unsigned int *dst, unsigned int len)
{
    return su_cipher_run(h, src, dst, len, SU_AES_START_EN, "SU_CIPHER_Encrypt");
}

int SU_CIPHER_Decrypt(int h, unsigned int *src, unsigned int *dst, unsigned int len)
{
    return su_cipher_run(h, src, dst, len, SU_AES_START_DE, "SU_CIPHER_Decrypt");
}
