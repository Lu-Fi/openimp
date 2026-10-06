/* apitest - device test: every non-image vendor IMP/SU function that libimp exports.
 *
 *   apitest SENSOR I2C_ADDR W H [areas]       areas = comma list of
 *                                              sys,fs,enc,osd,ivs,audio,isp,su (default all)
 *   apitest --list                            vendor functions this build exercises
 *
 * One line per function:   [A] <function> ret=<r> <PASS|FAIL|N/A|SKIP> <reason>
 * "FAIL OVERWRITE" = the function wrote outside the vendor struct of this SoC
 * (guard bytes around every out buffer).  Run with the streamer stopped.
 */
#include "apitest.h"

int g_pass, g_fail, g_na, g_skip;
int g_overwrites;
int g_sw, g_sh, g_mode_night;
IMPSensorInfo g_sensor;

/* ---------------------------------------------------------------- guards */
#define GUARD 64
#define GHDR 16
static int g_ovr_pending;

void *gnew(size_t sz)
{
    unsigned char *b = calloc(1, GHDR + GUARD + sz + GUARD);

    if (!b) { fprintf(stderr, "[E] out of memory\n"); exit(2); }
    *(size_t *)b = sz;
    memset(b + GHDR, 0xA5, GUARD);
    memset(b + GHDR + GUARD + sz, 0xA5, GUARD);
    return b + GHDR + GUARD;
}

static int gcheck(void *p)
{
    unsigned char *b = (unsigned char *)p - GUARD - GHDR;
    size_t sz = *(size_t *)b, i;
    int bad = 0;

    for (i = 0; i < GUARD; i++) {
        if (b[GHDR + i] != 0xA5) bad |= 1;
        if (b[GHDR + GUARD + sz + i] != 0xA5) bad |= 2;
    }
    return bad;
}

int gchk(void *p)
{
    int bad = gcheck(p);

    if (bad) {
        g_ovr_pending |= bad;
        g_overwrites++;
        /* restore so the same buffer reports once */
        { unsigned char *b = (unsigned char *)p - GUARD - GHDR; size_t sz = *(size_t *)b;
          memset(b + GHDR, 0xA5, GUARD); memset(b + GHDR + GUARD + sz, 0xA5, GUARD); }
    }
    return !bad;
}

void gfree(void *p)
{
    if (!p) return;
    gchk(p);
    free((unsigned char *)p - GUARD - GHDR);
}

int gtouched(void *p)
{
    unsigned char *b = (unsigned char *)p - GUARD - GHDR, *u = p;
    size_t sz = *(size_t *)b, i;

    for (i = 0; i < sz; i++) if (u[i]) return 1;
    return 0;
}

/* ---------------------------------------------------------------- report */
#define MAXFN 1200
static struct { const char *name; unsigned char tested; } g_fn[MAXFN];
static int g_nfn;
static const char *g_area = "init";
static struct { const char *name; int p, f, n, s; } g_ar[16];
static int g_nar;
static const char *g_last = "start";

static void note(const char *fn, int v)
{
    int i;

    for (i = 0; i < g_nfn; i++) if (!strcmp(g_fn[i].name, fn)) break;
    if (i == g_nfn) {
        if (g_nfn >= MAXFN) return;
        g_fn[g_nfn].name = fn; g_fn[g_nfn].tested = 0; g_nfn++;
    }
    if (v == V_PASS || v == V_FAIL) g_fn[i].tested = 1;
}

void rep_area(const char *name)
{
    g_area = name;
    if (g_nar < 16) { g_ar[g_nar].name = name; g_nar++; }
    printf("[T] area %s\n", name);
    fflush(stdout);
}

static void count(int v)
{
    int *c = v == V_PASS ? &g_pass : v == V_FAIL ? &g_fail : v == V_NA ? &g_na : &g_skip;

    (*c)++;
    if (g_nar) {
        if (v == V_PASS) g_ar[g_nar - 1].p++;
        else if (v == V_FAIL) g_ar[g_nar - 1].f++;
        else if (v == V_NA) g_ar[g_nar - 1].n++;
        else g_ar[g_nar - 1].s++;
    }
}

void rep(const char *fn, long ret, int v, const char *fmt, ...)
{
    static const char *vs[] = { "PASS", "FAIL", "N/A", "SKIP" };
    char msg[300];
    va_list ap;

    va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    if (g_ovr_pending) {
        printf("[A] %s ret=%ld FAIL OVERWRITE (guard %s hit) %s\n", fn, ret,
               g_ovr_pending == 3 ? "front+back" : g_ovr_pending == 1 ? "front" : "back", msg);
        g_ovr_pending = 0;
        v = V_FAIL;
    } else {
        printf("[A] %s ret=%ld %s %s\n", fn, ret, vs[v], msg);
    }
    fflush(stdout);
    note(fn, v);
    count(v);
    g_last = fn;
    alarm(60);                         /* progress */
}

void rep_na(const char *fn)
{
    printf("[A] %s ret=- N/A not exported by this libimp\n", fn);
    fflush(stdout);
    note(fn, V_NA);
    count(V_NA);
    g_last = fn;
    alarm(60);
}

int64_t now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void on_alarm(int sig)
{
    static const char m[] = "\n[E] watchdog: no progress for 60 s, last function: ";
    (void)sig;
    if (write(1, m, sizeof(m) - 1) < 0) _exit(3);
    if (write(1, g_last, strlen(g_last)) < 0) _exit(3);
    if (write(1, "\n", 1) < 0) _exit(3);
    _exit(3);
}

void wdog(const char *what, int sec)
{
    if (what) g_last = what;
    alarm(sec);
}

/* ----------------------------------------------------------------- misc */
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

extern char __start_apifn[], __stop_apifn[];

static void list_fns(int summary)
{
    const char **b = (const char **)__start_apifn, **e = (const char **)__stop_apifn, **u;
    int n = (int)(e - b), i, cnt = 0;

    u = malloc(sizeof(char *) * (n + 1));
    memcpy(u, b, sizeof(char *) * n);
    qsort(u, n, sizeof(char *), cmpstr);
    for (i = 0; i < n; i++) {
        if (i && !strcmp(u[i], u[i - 1])) continue;
        cnt++;
        if (!summary) printf("%s\n", u[i]);
    }
    printf("[T] %s: %d distinct vendor functions in this build's test table\n", SOC, cnt);
    free(u);
}

static int want(const char *areas, const char *a)
{
    return !areas || strstr(areas, a) != NULL;
}

int main(int argc, char **argv)
{
    const char *areas = argc > 5 ? argv[5] : NULL;
    int ret, i, tested = 0, na_only = 0;

    if (argc == 2 && !strcmp(argv[1], "--list")) { list_fns(0); return 0; }
    if (argc < 5) {
        fprintf(stderr, "apitest (%s) usage: %s sensor i2c_addr sensor_w sensor_h [areas]   |   %s --list\n"
                "  areas: sys,fs,enc,osd,ivs,audio,isp,su (comma list, default all)\n", SOC, argv[0], argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_sw = atoi(argv[3]); g_sh = atoi(argv[4]);
    g_mode_night = (getenv("APITEST_MODE") && !strcmp(getenv("APITEST_MODE"), "night"));
    signal(SIGALRM, on_alarm);
    signal(SIGPIPE, SIG_IGN);
    alarm(60);

    memset(&g_sensor, 0, sizeof(g_sensor));
    strncpy(g_sensor.name, argv[1], sizeof(g_sensor.name) - 1);
    g_sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    strncpy(g_sensor.i2c.type, argv[1], sizeof(g_sensor.i2c.type) - 1);
    g_sensor.i2c.addr = (int)strtol(argv[2], NULL, 0);
    g_sensor.rst_gpio = g_sensor.pwdn_gpio = g_sensor.power_gpio = -1;

    printf("[T] apitest %s sensor %s %dx%d areas %s\n", SOC, argv[1], g_sw, g_sh, areas ? areas : "all");

    if (!IMP_ISP_Open || !IMP_System_Init) { printf("[E] libimp does not export IMP_ISP_Open / IMP_System_Init (not linked?)\n"); return 1; }
    rep_area("isp-init");
    ret = 0;
#ifdef PLATFORM_T41
    ret = IMP_ISP_Open();
    rep(FN(IMP_ISP_Open), ret, ret < 0 ? V_FAIL : V_PASS, "");
    if (ret >= 0) { ret = IMP_ISP_AddSensor(IMPVI_MAIN, &g_sensor); RET0(IMP_ISP_AddSensor, ret, "sensor %s", g_sensor.name); }
    if (ret >= 0) { ret = IMP_ISP_EnableSensor(IMPVI_MAIN, &g_sensor); RET0(IMP_ISP_EnableSensor, ret, ""); }
#else
    ret = IMP_ISP_Open();
    rep(FN(IMP_ISP_Open), ret, ret < 0 ? V_FAIL : V_PASS, "");
    if (ret >= 0) { ret = IMP_ISP_AddSensor(&g_sensor); RET0(IMP_ISP_AddSensor, ret, "sensor %s", g_sensor.name); }
    if (ret >= 0) { ret = IMP_ISP_EnableSensor(); RET0(IMP_ISP_EnableSensor, ret, ""); }
#endif
    if (ret < 0) { printf("[E] ISP open/add/enable sensor failed\n"); return 1; }

    rep_area("sys");
    ret = IMP_System_Init();
    RET0(IMP_System_Init, ret, "");
    if (ret < 0) return 1;
    ret = IMP_ISP_EnableTuning();
    RET0(IMP_ISP_EnableTuning, ret, "");
    if (ret < 0) return 1;
    { unsigned m = g_mode_night; int r;
#ifdef PLATFORM_T41
      r = IMP_ISP_Tuning_SetISPRunningMode(IMPVI_MAIN, (void *)&m);
#else
      r = IMP_ISP_Tuning_SetISPRunningMode(m);
#endif
      (void)r; }

    if (want(areas, "sys")) t_system();
    t_fs_pre();
    if (want(areas, "fs")) t_fs();
    if (want(areas, "enc")) t_enc();
    if (want(areas, "osd")) t_osd();
    if (want(areas, "ivs")) t_ivs();
    if (want(areas, "audio")) t_audio();
    if (want(areas, "isp")) t_isp();
    if (want(areas, "su")) t_su();
    rep_area("teardown");
    t_fs_post();
    t_system_exit(0);

    for (i = 0; i < g_nfn; i++) { if (g_fn[i].tested) tested++; else na_only++; }
    printf("[T] summary %s: PASS %d FAIL %d N/A %d SKIP %d, OVERWRITE %d\n", SOC, g_pass, g_fail, g_na, g_skip, g_overwrites);
    printf("[T] functions: %d reported, %d exercised (PASS/FAIL), %d only N/A or SKIP\n", g_nfn, tested, na_only);
    for (i = 0; i < g_nar; i++)
        printf("[T] area %-8s PASS %d FAIL %d N/A %d SKIP %d\n", g_ar[i].name, g_ar[i].p, g_ar[i].f, g_ar[i].n, g_ar[i].s);
    alarm(0);
    return g_fail ? 1 : 0;
}
