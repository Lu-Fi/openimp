/*
 * T30 NCU (noise cancel unit) engine, Lu-Fi/openimp #701.
 *
 * The NCU (registers 0x13090000) sits between the ISP core and the scaler; the kernel
 * driver only streams it (open-tx-isp tx_isp_t30_ncu_stream_on: strides, frame size,
 * start).  Its denoise function is configured entirely from user space by the vendor
 * libimp through /dev/mem register writes, from /etc/sensor/ncu_<sensor>.bin.  This file
 * follows the stock ABUS firmware libimp 1.0.4 (md5 962be4bc, nm addresses):
 *   ncu_hal_init 0xe29fc, ncu_config 0xe5068, ncu_load_param 0xe2910 (fread 0x1548),
 *   ncu_set_params 0xd8cf8, ncu_ctrl_update_param 0xd8e70, ncu_ctrl_update 0xd8db0,
 *   ncu_ctrl_update_mode, ncu_func_bypass 0xd911c, ncu_all_buf_size 0xe5280,
 *   ncu_all_buf_alloc 0xe52e0, ncu_all_buf_config 0xe4fc8, ncu_func_linear_insert 0xd9278,
 *   ncu_func_non_linear_insert 0xd9420, ncu_func_par_download 0xd94b0 (t30_ncu_par104.c).
 * The caller side (bin probe, buffer allocation, day/night switch, the 1 s gain update)
 * is in src/isp/isp_tseries.c.  Details: docs/T30_NCU.md.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "t30_ncu.h"
#include "t30_ncu_internal.h"


/* vendor ncuinfo (0x108 bytes): the fields the 1.0.4 engine uses */
struct t30_ncu_info {
    uint32_t w, h;           /* +0x30 / +0x34 ncu_set_frame_size */
    uint32_t sta_a, sta_b;   /* +0x48 / +0x4c statistics strides */
    uint32_t psn_a, psn_b;   /* +0x50 / +0x54 */
    uint32_t buf_virt;       /* +0x78 / +0x80 */
    uint32_t ref_size;       /* +0x7c w*h*3/2 */
    uint32_t ref_virt_uv;    /* +0x88 */
    uint32_t ref_phys;       /* +0x84 */
    uint32_t ref_phys_uv;    /* +0x8c */
    uint32_t sta_virt;       /* +0xb0 / +0xc0 */
    uint32_t sta_size;       /* +0xb4 */
    uint32_t sta_rows;       /* +0xb8 */
    uint32_t sta_phys0;      /* +0xcc */
    uint32_t sta_phys1;      /* +0xd0 (never set by the vendor: 0) */
    uint32_t sta_phys2;      /* +0xd4 (never set by the vendor: 0) */
    uint32_t sta_count;      /* +0xd8 = 50 */
};

static struct {
    pthread_mutex_t lock;     /* vendor mutex_ncu_ctrl */
    struct t30_ncu_info info;
    uint32_t params[T30_NCU_BIN104_SIZE / 4];   /* vendor g_ncu_t30_params (bin as read) */
    uint32_t active[T30_NCU_SET104_SIZE / 4];   /* vendor g_ncu_t30_param (day or night set) */
    int layout;               /* T30_NCU_BIN_104 or T30_NCU_BIN_105 (the bin that was loaded) */
    int inited;               /* set by ncu_ctrl_update_param */
    int night;                /* ncu_ctrl_update_mode: 0 day set, 1 night set */
    int32_t total_gain;       /* vendor g_total_gain */
    t30_ncu_write_fn wr;
    t30_ncu_read_fn rd;
} g_ncu = { .lock = PTHREAD_MUTEX_INITIALIZER };

int t30_ncu_img_w;
int t30_ncu_img_h;
static const char *t30_ncu_bin_dir = "/etc/sensor";

void t30_ncu_set_bin_dir(const char *dir)
{
    t30_ncu_bin_dir = dir != NULL ? dir : "/etc/sensor";
}

void t30_ncu_set_reg_ops(t30_ncu_write_fn wr, t30_ncu_read_fn rd)
{
    g_ncu.wr = wr;
    g_ncu.rd = rd;
}

/* vendor write_reg_32 / read_reg_32 (regrw): one O_SYNC /dev/mem page mapping per access */
static int t30_ncu_regrw(uint32_t addr, uint32_t *val, int write)
{
    long page = sysconf(_SC_PAGESIZE);
    uint32_t base, off;
    void *m;
    int fd;

    if (page <= 0)
        page = 4096;
    base = addr & ~((uint32_t)page - 1u);
    off = addr - base;
    fd = open("/dev/mem", (write ? O_RDWR : O_RDONLY) | O_SYNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    m = mmap(NULL, (size_t)page, write ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED, fd, (off_t)base);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    if (write)
        *(volatile uint32_t *)((uint8_t *)m + off) = *val;
    else
        *val = *(volatile uint32_t *)((uint8_t *)m + off);
    munmap(m, (size_t)page);
    return 0;
}

int t30_phys_write32(uint32_t addr, uint32_t val)
{
    if (g_ncu.wr != NULL) {
        g_ncu.wr(addr, val);
        return 0;
    }
    return t30_ncu_regrw(addr, &val, 1);
}

int t30_phys_read32(uint32_t addr, uint32_t *val)
{
    if (g_ncu.rd != NULL) {
        *val = g_ncu.rd(addr);
        return 0;
    }
    *val = 0;
    return t30_ncu_regrw(addr, val, 0);
}

/* vendor reg_ncu_write / reg_ncu_read: hal_write_reg(0x13090000 + reg) = write_reg_32 */
void t30_ncu_wr(uint32_t reg, uint32_t val)
{
    t30_phys_write32(T30_NCU_REG_BASE + reg, val);
}

uint32_t t30_ncu_rd(uint32_t reg)
{
    uint32_t v = 0;

    t30_phys_read32(T30_NCU_REG_BASE + reg, &v);
    return v;
}

/* vendor ncu_func_linear_insert (0xd9278): piecewise linear over the 9 gain points; at or
 * below g0 the first value, above g8 the last one. */
int32_t t30_ncu_lin(int32_t v0, int32_t v1, int32_t v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6,
                    int32_t v7, int32_t v8, int32_t g0, int32_t g1, int32_t g2, int32_t g3, int32_t g4,
                    int32_t g5, int32_t g6, int32_t g7, int32_t g8, int32_t gain)
{
    const int32_t v[9] = { v0, v1, v2, v3, v4, v5, v6, v7, v8 };
    const int32_t gp[9] = { g0, g1, g2, g3, g4, g5, g6, g7, g8 };
    int32_t lo_v, hi_v, lo_g, hi_g;
    double d;
    int i;

    if (!(g0 < gain))
        return (int32_t)((0.0 / (double)(g1 - g0)) * (double)(gain - g0) + (double)v0);
    for (i = 1; i < 8 && gp[i] < gain; i++)
        ;
    if (i == 8 && g8 < gain)   /* vendor: above the last point, slope 0 from v8 */
        return (int32_t)((0.0 / (double)(g8 - g7)) * (double)(gain - g7) + (double)v8);
    lo_v = v[i - 1];
    hi_v = v[i];
    lo_g = gp[i - 1];
    hi_g = gp[i];
    if (hi_v < lo_v)
        return (int32_t)(((double)(lo_v - hi_v) / (double)(hi_g - lo_g)) * (double)(gain - lo_g) +
                         (double)hi_v);
    d = (double)(hi_v - lo_v);
    return (int32_t)((d / (double)(hi_g - lo_g)) * (double)(gain - lo_g) + (double)lo_v);
}

/* vendor ncu_func_non_linear_insert (0xd9420): step function over the gain points */
int32_t t30_ncu_nlin(int32_t v0, int32_t v1, int32_t v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6,
                     int32_t v7, int32_t v8, int32_t g0, int32_t g1, int32_t g2, int32_t g3, int32_t g4,
                     int32_t g5, int32_t g6, int32_t g7, int32_t g8, int32_t gain)
{
    (void)g8;
    if (!(g0 < gain))
        return v0;
    if (!(g1 < gain))
        return v1;
    if (!(g2 < gain))
        return v2;
    if (!(g3 < gain))
        return v3;
    if (!(g4 < gain))
        return v4;
    if (!(g5 < gain))
        return v5;
    if (!(g6 < gain))
        return v6;
    return gain <= g7 ? v7 : v8;
}

int t30_ncu_bin_probe(const char *path, uint32_t *mode)
{
    uint32_t hdr[2];
    long size;
    FILE *f;

    if (access(path, R_OK) != 0)
        return T30_NCU_BIN_NONE;
    f = fopen(path, "rb");
    if (f == NULL)
        return T30_NCU_BIN_INVALID;
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return T30_NCU_BIN_INVALID;
    }
    size = ftell(f);
    fclose(f);
    if (size == (long)T30_NCU_BIN104_SIZE && hdr[0] == T30_NCU_BIN104_SIZE) {
        if (mode != NULL)
            *mode = (hdr[1] >> 16) & 3;   /* vendor 1.0.4 AddSensor: flags >> 16 & 3 */
        return T30_NCU_BIN_104;
    }
    if (size == (long)T30_NCU_BIN105_SIZE && hdr[0] == T30_NCU_BIN105_SIZE && (hdr[1] >> 24) == 1) {
        if (mode != NULL)
            *mode = (hdr[1] & 0x3ffffu) >> 16;   /* vendor 1.0.5 EnableSensor: (flags & 0x3ffff) >> 16 */
        return T30_NCU_BIN_105;
    }
    return T30_NCU_BIN_INVALID;
}

/* vendor ncu_func_par_download through ncu_func_update (lock / unlock are no-ops) */
static void ncu_func_update(int32_t gain)
{
    if (g_ncu.layout == T30_NCU_BIN_105)
        t30_ncu_par105_download((const uint8_t *)g_ncu.active, gain);
    else
        t30_ncu_par104_download((const uint8_t *)g_ncu.active, gain);
}

/* vendor ncu_ctrl_update_param: copy the day or night set and download it */
static void ncu_ctrl_update_param_locked(void)
{
    /* 1.0.4: day set at +4, night set 0xaa0 bytes later; 1.0.5: sets of 0x760 bytes (0xec8 bin) */
    const size_t set_words = (g_ncu.layout == T30_NCU_BIN_105 ? T30_NCU_SET105_SIZE : T30_NCU_SET104_SIZE) / 4;
    const uint32_t *set = &g_ncu.params[1 + (g_ncu.night ? set_words : 0)];

    memcpy(g_ncu.active, set, set_words * 4);
    g_ncu.inited = 1;
    ncu_func_update(g_ncu.total_gain);
}

/* vendor ncu_config (0xe5068) with the hal_init defaults (funcs 0) */
static void ncu_config(uint32_t w, uint32_t h)
{
    struct t30_ncu_info *in = &g_ncu.info;

    in->sta_b = (((w + 0xfu) >> 4) + 0xfu) & ~0xfu;
    in->sta_a = ((w + 0x1bu) / 0x1cu + 0xfu) & ~0xfu;
    in->w = w;   /* ncu_set_frame_size */
    in->h = h;
    in->psn_a = ((w >> 1) + 0xfu) & ~0xfu;
    in->psn_b = ((w >> 2) + 0xfu) & ~0xfu;
    /* ncu_set_psn_stride */
    t30_ncu_wr(0x68, in->psn_a);
    t30_ncu_wr(0x74, in->psn_b);
    t30_ncu_wr(0x80, in->psn_b);
    t30_ncu_wr(0x6c, 0);
    t30_ncu_wr(0x78, 0);
    t30_ncu_wr(0x84, 0);
    /* ncu_set_sta_stride(sta_a, sta_b, sta_b) */
    t30_ncu_wr(0x90, in->sta_a);
    t30_ncu_wr(0x9c, in->sta_b);
    t30_ncu_wr(0xa8, in->sta_b);
    t30_ncu_wr(0x94, 0);
    t30_ncu_wr(0xa0, 0);
    t30_ncu_wr(0xac, 0);
    /* ncu_fun_ctrl(0): keep the driver's low byte */
    t30_ncu_wr(0x24, t30_ncu_rd(0x24) & 0xffu);
}

int t30_ncu_hal_init(const char *sensor, int w, int h)
{
    char path[128];
    FILE *f;

    pthread_mutex_lock(&g_ncu.lock);
    memset(&g_ncu.info, 0, sizeof(g_ncu.info));
    memset(g_ncu.active, 0, sizeof(g_ncu.active));
    g_ncu.inited = 0;
    g_ncu.night = 0;
    g_ncu.layout = T30_NCU_BIN_104;
    ncu_config((uint32_t)w, (uint32_t)h);
    t30_ncu_img_w = w;   /* ncu_func_config */
    t30_ncu_img_h = h;
    snprintf(path, sizeof(path), "%s/ncu_%s.bin", t30_ncu_bin_dir, sensor);
    if (access(path, R_OK) == 0) {
        /* the vendor reads its own layout only (1.0.4 0x1548, 1.0.5 0xec8 bytes); this port reads both */
        g_ncu.layout = t30_ncu_bin_probe(path, NULL) == T30_NCU_BIN_105 ? T30_NCU_BIN_105 : T30_NCU_BIN_104;
        f = fopen(path, "rb");
        if (f != NULL && fread(g_ncu.params, g_ncu.layout == T30_NCU_BIN_105 ? T30_NCU_BIN105_SIZE : T30_NCU_BIN104_SIZE,
                               1, f) == 1) {
            ncu_ctrl_update_param_locked();
        } else {
            printf("error: ncubin file %s, read error\n", path);
        }
        if (f != NULL)
            fclose(f);
    } else {
        printf("warn: do not have %s\n", path);
    }
    pthread_mutex_unlock(&g_ncu.lock);
    return 0;
}

/* vendor ncu_all_buf_size (0xe5280) */
uint32_t t30_ncu_hal_buf_size(void)
{
    struct t30_ncu_info *in = &g_ncu.info;

    in->sta_count = 0x32;
    return in->sta_a * 0x32u * ((in->h + 0x1bu) / 0x1cu + 1u) + ((in->h * in->w * 3u) >> 1);
}

/* vendor ncu_all_buf_alloc (0xe52e0) + ncu_all_buf_config (0xe4fc8); like the vendor the
 * registers are written even when the buffer is too small (alloc result ignored). */
int t30_ncu_hal_buf_init(uint32_t virt, uint32_t phys, uint32_t size)
{
    struct t30_ncu_info *in = &g_ncu.info;
    uint32_t n, ref, rows, sta;
    int ret = 0;

    pthread_mutex_lock(&g_ncu.lock);
    n = in->h * in->w;
    ref = (n * 3u) >> 1;
    if (size < ref) {
        printf("%s,%d: ncu buf too small\n", "ncu_all_buf_alloc", 0x3e);
        ret = -1;
    } else {
        in->ref_phys_uv = phys + n;
        in->sta_count = 0x32;
        in->buf_virt = virt;
        in->ref_size = ref;
        in->ref_virt_uv = virt + n;
        rows = ((in->h + 0x1bu) / 0x1cu + 1u) * in->sta_a;
        sta = rows * 0x32u;
        in->ref_phys = phys;
        if (size < sta + ref) {
            printf("%s,%d: ncu buf too small\n", "ncu_all_buf_alloc", 0x55);
            ret = -1;
        } else {
            in->sta_phys0 = phys + ref;
            in->sta_virt = virt + ref;
            in->sta_size = sta;
            in->sta_rows = rows;
        }
    }
    t30_ncu_wr(0x38, in->ref_phys);      /* ncu_set_ref0_buffer */
    t30_ncu_wr(0x48, in->ref_phys_uv);
    t30_ncu_wr(0x50, in->ref_phys);      /* ncu_set_ref1_buffer */
    t30_ncu_wr(0x58, in->ref_phys_uv);
    t30_ncu_wr(0x88, in->sta_count - 1u);
    t30_ncu_wr(0x94, in->sta_rows);
    t30_ncu_wr(0x8c, in->sta_phys0);     /* ncu_set_sta_buffer */
    t30_ncu_wr(0x98, in->sta_phys1);
    t30_ncu_wr(0xa4, in->sta_phys2);
    pthread_mutex_unlock(&g_ncu.lock);
    return ret;
}

/* vendor ncu_func_bypass (0xd911c) */
void t30_ncu_ctrl_bypass(int bypass)
{
    pthread_mutex_lock(&g_ncu.lock);
    if (bypass == 0) {
        /* 1.0.4 reads the engine-enable word at +0x5a4 of its set, 1.0.5 (0x760 byte set) at +0x30 */
        uint32_t f = g_ncu.active[(g_ncu.layout == T30_NCU_BIN_105 ? 0x30 : 0x5a4) / 4];
        uint32_t m = g_ncu.active[1];
        uint32_t e = g_ncu.active[0x2c / 4];
        uint32_t fc = t30_ncu_rd(0x24);

        t30_ncu_wr(4, t30_ncu_rd(4) | 2u);
        t30_ncu_wr(0x24, (fc & 0xffu) | ((m & 0xffff35u) << 8) | ((f & 1u) << 14) | ((f & 1u) << 11) |
                             ((f & 1u) << 15) | ((e & 1u) << 9));
    } else {
        t30_ncu_wr(0x24, 0);
    }
    t30_ncu_wr(0xc, 5);
    pthread_mutex_unlock(&g_ncu.lock);
}

/* vendor ncu_ctrl_update_mode: select the day (0) or night (1) set and download it */
void t30_ncu_ctrl_update_mode(int night)
{
    pthread_mutex_lock(&g_ncu.lock);
    g_ncu.night = night;
    ncu_ctrl_update_param_locked();
    pthread_mutex_unlock(&g_ncu.lock);
}

/* vendor ncu_ctrl_update (0xd8db0): the 1 s gain update of the tuning daemon */
void t30_ncu_ctrl_update(int32_t total_gain)
{
    pthread_mutex_lock(&g_ncu.lock);
    if (!g_ncu.inited)
        printf("ncu ctrl not init\n");
    else if ((g_ncu.active[0] & 1u) == 0)
        ncu_func_update(total_gain);
    pthread_mutex_unlock(&g_ncu.lock);
}

void t30_ncu_set_total_gain(int32_t total_gain)
{
    pthread_mutex_lock(&g_ncu.lock);
    g_ncu.total_gain = total_gain;
    pthread_mutex_unlock(&g_ncu.lock);
}

void t30_ncu_deinit(void)
{
    pthread_mutex_lock(&g_ncu.lock);
    g_ncu.inited = 0;
    pthread_mutex_unlock(&g_ncu.lock);
}

int t30_ncu_active(void)
{
    int a;

    pthread_mutex_lock(&g_ncu.lock);
    a = g_ncu.inited;
    pthread_mutex_unlock(&g_ncu.lock);
    return a;
}
