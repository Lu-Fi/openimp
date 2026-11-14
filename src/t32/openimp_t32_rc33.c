/*
 * OpenIMP T32 H.264 / H.265 rate control, eprc T33 revision (#295, planned):
 * the vendor 2.2.0 i264e and i265e sides of the controller
 * (openimp_t32_rc33.h; H.265 at the end of this file).
 * Reimplemented from i264e_param_default (0x6a5e0), i264e_ratecontrol_init
 * (0x78a14), _start (0x78f0c), _end (0x79478) and _is_reenc (0x7966c);
 * comments give vendor offsets of the i264e parameter block (P), the
 * encoder handle (H) and its per-picture block (F = H+0x3f0).
 */
#include <stdlib.h>
#include <string.h>

#include "t32/openimp_t32_rc33.h"

/* arena layout (region 0); the controller only sees these addresses */
#define R0 0x10000000u
#define A_P (R0)                /* i264e parameter block (0x3090 used) */
#define A_H (R0 + 0x4000)       /* encoder handle (+0x1540 -> P) */
#define A_F (R0 + 0x5800)       /* per-picture encoder block (H+0x3f0) */
#define A_S (R0 + 0x5a00)       /* picture statistics (H+0x400) */
#define A_TB (R0 + 0x5c00)      /* engine statistics read list (H+0xbac), 8 bytes per entry */
#define A_REG (R0 + 0x6000)     /* the HERA register window (P+0xc24) */
#define A_SL (R0 + 0x7000)      /* slice block (H+0xf9c -> P+0x6cc) */
#define A_MAPS (R0 + 0x8000)    /* the eight per-MB maps (P+0x1444 ..) */
#define R1 0x20000000u          /* the controller's state block */
#define R2 0x200c4cu            /* md_ctrl_set (vendor .data), patched by the controller */

#define E (A_P + 0x360)

#define P8(o) e33_ld_U8(X, A_P + (o))
#define PS8(o) e33_ld_S8(X, A_P + (o))
#define P32(o) e33_ld_U32(X, A_P + (o))
#define PS32(o) e33_ld_S32(X, A_P + (o))
#define SP8(o, v) e33_st_U8(X, A_P + (o), (uint8_t)(v))
#define SP16(o, v) e33_st_U16(X, A_P + (o), (uint16_t)(v))
#define SP32(o, v) e33_st_U32(X, A_P + (o), (uint32_t)(v))

/* i264e_param_default: the non-zero words of the parameter block */
static const uint32_t t33_pdef[][2] = {
    { 0x4, 1 }, { 0xc, 1 }, { 0x10, 1 }, { 0x14, 1 }, { 0x24, 0xffffffffu }, { 0x28, 1 },
    { 0x3c, 1280 }, { 0x40, 720 }, { 0x44, 4 }, { 0x48, 100 }, { 0x4c, 0xffffffffu }, { 0x5c, 5 },
    { 0x60, 0xffffffffu }, { 0x64, 2 }, { 0x68, 2 }, { 0x6c, 0xffffffffu }, { 0x78, 1 }, { 0x88, 1 },
    { 0x8c, 1 }, { 0x98, 1 }, { 0xa4, 1 }, { 0xac, 0xffffffffu }, { 0xb0, 7 }, { 0xb4, 1 },
    { 0xd8, 0xffffffffu }, { 0xdc, 1 }, { 0xe0, 35 }, { 0xe4, 35 }, { 0xec, 15 }, { 0xf0, 45 },
    { 0xf4, 1 }, { 0xf8, 35 }, { 0xfc, 45 }, { 0x100, 15 }, { 0x104, 0x3f800000u }, { 0x108, 0x3fb33333u },
    { 0x10c, 0x3fa66666u }, { 0x110, 2000 }, { 0x114, 1 }, { 0x118, 1500 }, { 0x120, 8 }, { 0x124, 3 },
    { 0x128, 15 }, { 0x130, 2 }, { 0x134, 80 }, { 0x138, 6 }, { 0x13c, 2 }, { 0x140, 0x40400000u },
    { 0x144, 51 }, { 0x148, 1 }, { 0x14c, 1 }, { 0x150, 100 }, { 0x154, 1 }, { 0x158, 2400 },
    { 0x15c, 1440 }, { 0x160, 25 }, { 0x164, 35 }, { 0x16c, 5 }, { 0x170, 2 }, { 0x174, 20 },
    { 0x178, 1600 }, { 0x17c, 1200 }, { 0x180, 2 }, { 0x184, 60 }, { 0x188, 5 }, { 0x1a0, 19660800 },
    { 0x1a4, 14043429 }, { 0x1a8, 10802638 }, { 0x1b0, 1 }, { 0x1b8, 24 }, { 0x1bc, 26 }, { 0x26c, 0x200 },
    { 0x294, 1 }, { 0x298, 1 }, { 0x2a4, 25 }, { 0x2a8, 1 }, { 0x2b4, 206832 }, { 0x2b8, 1853520 },
    { 0x2bc, 2 }, { 0x2c0, 1 }, { 0x308, 0x10000100u }, { 0x30c, 0x1001003fu }, { 0x310, 0x10000014u },
    { 0x31c, 0x11111101u }, { 0x330, 0x10100 }, { 0x334, 63 }, { 0x338, 500 }, { 0x33c, 500 },
    { 0x340, 0x1000100 }, { 0x348, 1 }, { 0x34c, 1 },
};

static uint32_t mb_map(uint32_t w, uint32_t h)
{
    return ((((w + 15) >> 4) * ((h + 15) >> 4)) + 0xfffu) & ~0xfffu;
}

int t32_rc33_open(T32Rc33 *c, uint32_t width, uint32_t height)
{
    uint32_t map = mb_map(width, height), mbs = ((width + 15) >> 4) * ((height + 15) >> 4);
    uint32_t wb = A_MAPS + 8 * map, size = wb - R0 + 0x4000 + mbs * 64;
    uint32_t u = (((width + 0x1f) >> 5) * ((height + 0x1f) >> 5) * 4 + 0xfu) & ~0xfu;
    uint32_t ssize = u * 2 + 0x7b0;
    E33 *X;

    memset(c, 0, sizeof(*c));
    c->mem = calloc(1, size);
    c->state = calloc(1, ssize);
    if (!c->mem || !c->state) {
        t32_rc33_close(c);
        return -1;
    }
    X = &c->X;
    X->r[0] = c->mem;
    X->rb[0] = R0;
    X->rs[0] = size;
    X->r[1] = c->state;
    X->rb[1] = R1;
    X->rs[1] = ssize;
    X->r[2] = c->tpl;
    X->rb[2] = R2;
    X->rs[2] = sizeof(c->tpl);
    X->calloc_addr = R1;
    for (unsigned i = 0; i < sizeof(t33_pdef) / sizeof(t33_pdef[0]); i++)
        SP32(t33_pdef[i][0], t33_pdef[i][1]);
    SP32(T33P_WIDTH, width);
    SP32(T33P_HEIGHT, height);
    SP32(T33P_REV, 6);
    SP32(0xc20, 1);             /* the register window (vendor: /dev/mem at 0x13240000) */
    SP32(0xc24, A_REG);
    SP32(0x3088, wb);           /* the VPU work buffer the slice block lays its maps out in */
    return 0;
}

/* the i264e parameter words (< 0x360, as i264e_init copies them) the
 * controller and this glue read: the IMP mapping must make them vendor-equal */
static const uint16_t t33_pwords[] = {
    0x4, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x48, 0xdc, 0xe0, 0xe4, 0xec, 0xf0, 0xf4, 0xf8, 0xfc, 0x100,
    0x110, 0x11c, 0x120, 0x124, 0x130, 0x134, 0x138, 0x13c, 0x150, 0x154, 0x158, 0x15c, 0x160, 0x164,
    0x168, 0x16c, 0x170, 0x174, 0x178, 0x17c, 0x180, 0x184, 0x188, 0x194, 0x198, 0x19c, 0x1a0, 0x1a4,
    0x1b0, 0x1c0, 0x1c4, 0x1c8, 0x1cc, 0x1d0, 0x1d4, 0x1d8, 0x1dc, 0x1e0, 0x1e4, 0x1e8, 0x1ec, 0x1f0,
    0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220, 0x224, 0x228,
    0x22c, 0x230, 0x234, 0x238, 0x23c, 0x240, 0x244, 0x248, 0x24c, 0x250, 0x254, 0x258, 0x25c, 0x260,
    0x264, 0x26c, 0x2a4, 0x2a8, 0x304, 0x308, 0x30c, 0x310, 0x314, 0x318, 0x31c, 0x320, 0x324, 0x328,
    0x32c, 0x330, 0x344, 0x348, 0x35c,
};

unsigned t32_rc33_param_words(const uint16_t **words)
{
    *words = t33_pwords;
    return sizeof(t33_pwords) / sizeof(t33_pwords[0]);
}

void t32_rc33_close(T32Rc33 *c)
{
    free(c->mem);
    free(c->state);
    c->mem = NULL;
    c->state = NULL;
}

void t32_rc33_set(T32Rc33 *c, uint32_t off, uint32_t v)
{
    E33 *X = &c->X;

    SP32(off, v);
}

void t32_rc33_set8(T32Rc33 *c, uint32_t off, uint8_t v)
{
    E33 *X = &c->X;

    SP8(off, v);
}

uint32_t t32_rc33_get(T32Rc33 *c, uint32_t off)
{
    E33 *X = &c->X;

    return P32(off);
}

static int32_t clip3(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* i264e_validate_parameters, the clamps of the words the controller reads
 * (checked against the vendor with tests/t32/rc_imp_test.c) */
void t32_rc33_validate(T32Rc33 *c)
{
#define G(o) ((int32_t)t32_rc33_get(c, (o)))
#define S(o, v) t32_rc33_set(c, (o), (uint32_t)(v))
#define CL(o, lo, hi) S((o), clip3(G(o), (lo), (hi)))
    int32_t w = G(T33P_WIDTH), h = G(T33P_HEIGHT), area, lo, k = w < 0x321 ? 3 : 6;
    union { uint32_t u; float f; } r1, r2, qc;

    CL(0x118, 1, 0x40000000);
    CL(T33P_FIX_IQP, 0, 51);
    CL(T33P_FIX_PQP, 0, 51);
    CL(0xe8, 0, 51);
    CL(T33P_MAX_QP, 0, 51);
    CL(T33P_MIN_QP, 0, G(T33P_MAX_QP));
    CL(T33P_INIT_QP, 0, 51);
    CL(T33P_MAX_IQP, 0, 51);
    CL(T33P_MIN_IQP, 0, 51);
    CL(0x11c, -10, 10);
    CL(0x120, 0, 15);
    CL(0x124, 0, 15);
    CL(0x128, 2, 51);
    if (G(T33P_KBPS) < 0x80)
        S(T33P_KBPS, 0x80);
    if (G(0x130) < 1)
        S(0x130, 1);
    CL(0x134, 50, 100);
    CL(0x13c, 0, 8);
    CL(0x138, 0, 7);
    CL(0x150, 0, 100);
    CL(0x154, 0, G(0x150));
    CL(0x160, 1, 100);
    CL(0x164, 0, 51);
    CL(0x16c, 0, 10);
    CL(0x170, 0, 3);
    CL(0x174, 1, 100);
    if (G(0x180) < 1)
        S(0x180, 1);
    if (G(0x184) < 1)
        S(0x184, 1);
    CL(0x188, 0, 1000);
    qc.u = t32_rc33_get(c, 0x140);
    qc.f = (float)clip3((int32_t)qc.f, 2, 10);
    S(0x140, qc.u);
    CL(0x144, G(T33P_MAX_QP), 51);
    /* VBV: between 3/50 and 12/3 (12/6 above 800 pixels wide) bits per pixel */
    area = (int32_t)(((uint32_t)w + 15u) & ~15u) * (int32_t)(((uint32_t)h + 15u) & ~15u);
    lo = area * 3 / 50;
    CL(0x19c, 0, 3);
    CL(0x1a0, lo, area * 12 / k);
    r1.u = t32_rc33_get(c, 0x108);
    r2.u = t32_rc33_get(c, 0x10c);
    CL(0x1a4, lo, (int32_t)((float)G(0x1a0) / r1.f));
    CL(0x1a8, lo, (int32_t)((float)G(0x1a4) / r2.f));
    CL(0x114, 1, G(0x118));
#undef CL
#undef S
#undef G
}

/* i264e_ratecontrol_init (0x78a14), without the /dev/mem mapping */
void t32_rc33_init(T32Rc33 *c)
{
    E33 *X = &c->X;
    int32_t w, h;
    int8_t mode;
    uint32_t fps;

    SP8(0x36c, 0);
    SP8(0x394, P32(T33P_REV));
    if (P32(T33P_REV) - 4u > 3u)
        return;
    SP8(0xb84, P32(T33P_MODE));
    eprc_t33_h264_default_set(X, E);
    w = PS32(T33P_WIDTH);
    h = PS32(T33P_HEIGHT);
    SP16(0x36a, (h + 0xf < 0 ? h + 0x1e : h + 0xf) >> 4);
    fps = P32(T33P_FPS_NUM);
    SP8(0x37c, P8(T33P_MAX_QP));
    SP8(0x391, 2);
    SP8(0x392, P8(0x194));
    SP16(0x364, w);
    SP16(0x366, h);
    SP16(0x368, (w + 0xf < 0 ? w + 0x1e : w + 0xf) >> 4);
    SP32(0x370, P32(T33P_GOP));
    SP32(0x378, fps);
    SP32(0x374, P32(T33P_FPS_DEN));
    SP8(0x37d, P32(T33P_MIN_QP));
    SP8(0x37e, P8(0xf4));
    SP8(0x390, 0);
    SP8(0xbea, P32(0x48));
    SP8(0xb85, P32(T33P_INIT_QP));
    SP32(0xbbc, P32(0x158) << 10);
    SP32(0xbc0, P32(0x15c) << 10);
    SP8(0xb87, 0);
    SP8(0xb86, P32(T33P_MODE) == 3 ? 5 : 0);
    mode = PS8(0xb84);
    if (mode == 3) {
        SP8(0xb87, 1);
        SP8(0xbb4, P8(0x120));
        SP8(0xbb5, P8(0x120));
        SP8(0xbb6, P32(0x124));
        if (fps == 0)
            return;             /* the vendor traps */
        SP32(0xbb0, (P32(T33P_GOP) * P32(0xdd8) * P32(T33P_FPS_DEN)) / fps);
    } else {
        SP8(0xbb0, P32(0x120));
        SP8(0xbb1, P32(0x124));
    }
    SP32(0xc0c, P32(0x1a4));
    SP32(0xc10, P32(0x1a4));
    SP8(0xbf9, 1);
    SP8(0xc04, P32(0x19c));
    SP32(0xc08, P32(0x1a0));
    SP8(0xbf8, 0);
    SP32(0xbfc, 0xffffffffu);
    SP32(0xc00, 2);
    switch (mode) {
    case 0:
        SP8(0xb8c, P32(T33P_FIX_IQP));
        SP8(0xb8d, P32(T33P_FIX_PQP));
        break;
    case 1:
        SP32(0xb8c, P32(0x130));
        SP32(0xb90, P32(T33P_KBPS));
        SP8(0xb94, P32(0x13c));
        SP8(0xba0, P32(0x150));
        SP8(0xba1, P32(0x154));
        SP8(0xba4, P32(0x138));
        SP8(0xba2, P32(T33P_MAX_IQP));
        SP8(0xba3, P32(T33P_MIN_IQP));
        SP8(0xba6, P32(0x11c));
        SP8(0xba5, P32(0x1b0));
        break;
    case 2:
        SP32(0xb8c, P32(0x130));
        SP32(0xb90, P32(T33P_KBPS));
        SP8(0xba0, P32(0x134));
        SP8(0xba1, P32(0x150));
        SP8(0xba2, P32(0x154));
        SP8(0xba3, P32(0x138));
        SP8(0xba5, P32(0x11c));
        SP8(0xba6, P32(T33P_MAX_IQP));
        SP8(0xba7, P32(T33P_MIN_IQP));
        SP8(0xba4, P32(0x1b0));
        break;
    case 3:
    case 5:
        SP8(0xbae, P32(0x170));
        SP32(0xb8c, P32(0x130));
        SP32(0xb90, P32(T33P_KBPS));
        SP8(0xba0, P32(0x134));
        SP8(0xba1, P32(0x150));
        SP8(0xba2, P32(0x154));
        SP8(0xba3, P32(0x138));
        SP8(0xba5, P32(0x11c));
        SP8(0xba6, P32(0x160));
        SP8(0xba7, P32(0x164));
        SP8(0xba8, P32(T33P_MAX_IQP));
        SP8(0xba9, P32(T33P_MIN_IQP));
        SP8(0xbaa, P8(0x26c));
        SP8(0xbab, P8(0x26d));
        SP8(0xbac, P8(0x168));
        SP8(0xbad, P32(0x16c));
        SP8(0xbaf, P32(0x174));
        SP8(0xba4, P32(0x1b0));
        break;
    case 4:
        SP32(0xb8c, P32(0x180));
        SP32(0xb90, P32(T33P_KBPS));
        SP32(0xb94, P32(0x184));
        SP32(0xb98, P32(T33P_LONG_MAX_KBPS));
        SP32(0xb9c, P32(T33P_LONG_MIN_KBPS));
        SP8(0xba0, P32(0x134));
        SP8(0xba1, P32(0x150));
        SP8(0xba2, P32(0x154));
        SP8(0xba3, P32(0x138));
        SP8(0xba5, P32(0x11c));
        SP16(0xba6, P32(0x188));
        SP16(0xba8, 1);
        SP8(0xbaa, P32(T33P_MAX_IQP));
        SP8(0xbab, P32(T33P_MIN_IQP));
        SP8(0xba4, P32(0x1b0));
        break;
    default:
        return;                 /* no VIDEO_CFG */
    }
    eprc_t33_h264_video_cfg(X, E);

    /* what the encoder wires up after init (i264e_init): the handle, the
     * per-picture blocks, the slice block and the per-MB maps */
    {
        uint32_t map = mb_map(P32(T33P_WIDTH), P32(T33P_HEIGHT));
        static const uint16_t mo[8] = { 0x1444, 0x1448, 0x1a0c, 0x1a10, 0x1b7c, 0x1b80, 0x1cec, 0x1cf0 };

        e33_st_U32(X, A_H + 0x1540, A_P);
        e33_st_U32(X, A_H + 0x3f0, A_F);
        e33_st_U32(X, A_H + 0x400, A_S);
        e33_st_U32(X, A_H + 0xbac, A_TB);
        e33_st_U32(X, A_F + 0x4c, 0xffffffffu);         /* no FRAME_REPEATE_JUDGE (no re-encoding) */
        e33_st_U32(X, A_H + 0xf9c, A_SL);
        for (int i = 0; i < 8; i++)
            SP32(mo[i], A_MAPS + map * (uint32_t)i);
    }
    /* the motion mode of every picture (i264e_update_fenc: F+0x164 = 2) */
    e33_st_U8(X, A_F + 0x164, 2);
}

void t32_rc33_set_motion(T32Rc33 *c, uint8_t mode)
{
    e33_st_U8(&c->X, A_F + 0x164, mode);
}

int t32_rc33_set_param(T32Rc33 *c, unsigned id, const uint32_t *a)
{
    T33Pending *p = &c->pend;

    switch (id) {
    case 0x11:                  /* i264e_reconfig_bitrate_set: by the running mode */
        p->kbps = t32_rc33_get(c, T33P_MODE) == 1u ? a[0] : a[1];
        p->flags |= T33_SET_BITRATE;
        return 0;
    case 0x12:                  /* i264e_reconfig_qpbound_set */
        p->max_qp = a[0];
        p->min_qp = a[1];
        p->flags |= T33_SET_QPBOUND;
        return 0;
    case 0x13:                  /* i264e_reconfig_perqpbound_set */
        p->max_iqp = a[0];
        p->min_iqp = a[1];
        p->max_pqp = a[2];
        p->min_pqp = a[3];
        p->flags |= T33_SET_IQPBOUND;
        return 0;
    case 0x14:                  /* i264e_reconfig_maxpicsize_set */
        p->pic_i = a[0];
        p->pic_p = a[1];
        p->flags |= T33_SET_PICSIZE;
        return 0;
    }
    return -1;
}

/* i264e_idr_reconfig (0x7efac), flags 0x20000..0x100000 */
uint32_t t32_rc33_take_pending(T32Rc33 *c, const T33Pending *p)
{
    E33 *X = &c->X;
    uint32_t f = p->flags;

    if (f & T33_SET_BITRATE)
        SP32(T33P_KBPS, p->kbps);
    if (f & T33_SET_QPBOUND) {
        SP32(T33P_MAX_QP, p->max_qp);
        SP32(T33P_MIN_QP, p->min_qp);
    }
    if (f & T33_SET_IQPBOUND) {
        SP32(T33P_MAX_IQP, p->max_iqp);
        SP32(T33P_MIN_IQP, p->min_iqp);
        if (P32(T33P_MODE) == 0) {
            SP32(T33P_FIX_IQP, p->max_iqp);
            SP32(T33P_FIX_PQP, p->max_pqp);
        }
    }
    if (f & T33_SET_PICSIZE) {
        SP32(0x158, p->pic_i);
        SP32(0x15c, p->pic_p);
    }
    return f & (T33_SET_BITRATE | T33_SET_QPBOUND | T33_SET_IQPBOUND | T33_SET_PICSIZE);
}

static int t33_start(T32Rc33 *c, uint32_t since_idr);

int t32_rc33_start(T32Rc33 *c, uint32_t since_idr)
{
    int qp = t33_start(c, since_idr);

    if (since_idr == 0 && c->pend.flags) {
        /* i264e_encode: at the IDR, after the start, i264e_idr_reconfig */
        t32_rc33_take_pending(c, &c->pend);
        memset(&c->pend, 0, sizeof(c->pend));
        t32_rc33_validate(c);
        qp = t33_start(c, since_idr);
    }
    return qp;
}

/* i264e_ratecontrol_start (0x78f0c) */
static int t33_start(T32Rc33 *c, uint32_t since_idr)
{
    E33 *X = &c->X;
    uint32_t gop = P32(T33P_GOP), f = A_F;
    int32_t pos;
    int idr;
    int8_t mode, type;

    e33_st_U32(X, A_H, since_idr);
    if (gop == 0)
        return -1;              /* the vendor traps */
    pos = (int32_t)since_idr % (int32_t)gop;
    if (P32(T33P_REV) - 4u > 3u)
        return -1;
    SP32(0x6cc, e33_ld_U32(X, A_H + 0xf9c));
    SP32(0x7dc, P32(0x3088));
    idr = pos == 0 && since_idr == 0;
    for (uint32_t o = 0; o < 0x90; o += 4)
        SP32(0x740 + o, P32(0x1c0 + o));
    SP32(0x7d0, P32(0x250));
    SP32(0x7d4, P32(0x254));
    SP32(0x7d8, P32(0x258));
    SP8(0x808, P8(0x32c));
    SP8(0x814, P8(0x32e));
    SP8(0x815, P8(0x330));
    for (uint32_t o = 0; o < 0x24; o += 4)
        SP32(0xb18 + o, P32(0x308 + o));
    SP8(0xb45, P8(0x305));
    SP8(0xb44, P32(0x34));
    SP8(0xb49, P8(0x344));
    SP8(0xb48, P32(0x30) == 0 ? 1 : 2);
    SP32(0xb4c, P32(0x348));
    SP8(0xb46, P32(0x38));
    {
        static const uint16_t mo[8] = { 0x1444, 0x1448, 0x1a0c, 0x1a10, 0x1b7c, 0x1b80, 0x1cec, 0x1cf0 };

        for (int i = 0; i < 8; i++)
            SP32(0x7e0 + 4 * i, P32(mo[i]));
    }
    if (P32(0x34) == 0) {
        SP32(0x800, P32(0x115c));
        SP32(0x804, P32(0x1160));
    } else {
        SP32(0x800, P32(0xe7c));
        SP32(0x804, P32(0xe80));
    }
    SP32(0x3a8, idr);
    SP8(0x3b1, P32(0xe20));
    SP8(0xb50, e33_ld_U8(X, f + 0x151));
    SP8(0xb51, e33_ld_U8(X, f + 0x152));
    SP8(0xb58, e33_ld_U8(X, f + 0x153));
    SP8(0xb43, 0);
    SP32(0xb54, e33_ld_U32(X, f + 0x154));
    SP32(0xb5c, e33_ld_U32(X, f + 0x158));
    SP32(0xb60, e33_ld_U32(X, f + 0x15c));
    SP8(0xb42, 4);
    SP8(0xb41, P32(0x264) == 0 ? 12 : P32(T33P_WIDTH) / P32(0x264));
    SP32(0xb3c, P32(0x260));
    SP8(0xb40, P8(0x25c) != 0 && (P32(0x35c) != 0 || P32(0x260) != 0));
    if (idr || (e33_ld_S8(X, f + 0x151) != 0 && (e33_ld_S8(X, f + 0x152) != 0 || e33_ld_S8(X, f + 0x153) != 0))) {
        /* new QP limits and rates take effect at an IDR (or a refresh start) */
        SP8(0x37c, P32(T33P_MAX_QP));
        SP8(0x37d, P32(T33P_MIN_QP));
        SP32(0xbbc, P32(0x158) << 10);
        SP32(0xbc0, P32(0x15c) << 10);
        mode = PS8(0xb84);
        if (mode == 1) {
            SP32(0xb90, P32(T33P_KBPS));
            SP8(0xba2, P32(T33P_MAX_IQP));
            SP8(0xba3, P32(T33P_MIN_IQP));
        } else if (mode == 2) {
            SP32(0xb90, P32(T33P_KBPS));
            SP8(0xba6, P32(T33P_MAX_IQP));
            SP8(0xba7, P32(T33P_MIN_IQP));
        } else if (mode == 3 || mode == 5) {
            SP32(0xb90, P32(T33P_KBPS));
            SP8(0xba8, P32(T33P_MAX_IQP));
            SP8(0xba9, P32(T33P_MIN_IQP));
            /* SMART rule / level only with the still map on */
            SP8(0xbaa, P8(0xb40) ? P8(0x26c) : 0);
            SP8(0xbab, P8(0xb40) ? P8(0x26d) : 0);
        } else if (mode == 4) {
            SP32(0xb90, P32(T33P_KBPS));
            SP8(0xbaa, P32(T33P_MAX_IQP));
            SP8(0xbab, P32(T33P_MIN_IQP));
        }
    }
    SP32(0x260, 0);             /* the still map is handed over once */
    SP32(0xb64, e33_ld_U32(X, f + 0x160));
    SP8(0xb68, e33_ld_U8(X, f + 0x164));
    SP8(0xb69, e33_ld_U8(X, f + 0x165));
    SP8(0xb6a, e33_ld_U8(X, f + 0x166));
    SP32(0xb6c, e33_ld_U32(X, f + 0x168));
    eprc_t33_h264_frame_start(X, E);
    type = PS8(0xb71);
    c->type = type;
    if ((idr && type != 2) || (!idr && type != 0))
        return -1;              /* the vendor logs; the QP stays */
    c->qp = P8(0xb70);
    e33_st_U32(X, A_H + 0x354, (uint32_t)c->qp);
    SP32(0xb64, P32(0x198));
    return c->qp;
}

/* i264e_ratecontrol_end (0x79478): fold the engine statistics */
static void t33_rc_end(E33 *X)
{
    uint32_t s = A_S, tb = A_TB, rev = P32(T33P_REV), bits;
#define R(i) e33_ld_U32(X, tb + 8 * (i) + 4)

    if (rev - 4u < 4u) {
        for (int i = 0; i < 8; i++)
            e33_st_U32(X, s + 0xd4 + 4 * i, R(i));
        e33_st_U32(X, s + 0xf4, (R(8) & 0xffff) + (R(9) & 0xffff0000u));
        e33_st_U32(X, s + 0xf8, (R(10) & 0xffff) + (R(11) & 0xffff0000u));
        e33_st_U32(X, s + 0x114, R(12));
        e33_st_U32(X, s + 0x118, R(13));
        e33_st_U32(X, s + 0x11c, R(14));
        e33_st_U32(X, s + 0x90, R(15));
        e33_st_U32(X, s + 0x120, R(16));
        e33_st_U32(X, s + 0x124, R(17));
        e33_st_U32(X, s + 0x128, R(18));
        e33_st_U32(X, s + 0x12c, R(19));
        e33_st_U32(X, s + 0x9c, (R(20) & 0x7ffffff) + (R(21) & 0x7ffffff));
        e33_st_U32(X, s + 0x98, ((R(21) >> 16) & 0x3fff) + (R(22) & 0x3fff));
        e33_st_U32(X, s + 0x138, R(23) & 0x7fff);
        e33_st_U32(X, s + 0x130, (R(23) >> 16) & 0x7fff);
        e33_st_U32(X, s + 0x134, R(24) & 0x7fff);
        for (int i = 0; i < 4; i++) {
            SP16(0x484 + 4 * i, R(39 + i));
            SP16(0x486 + 4 * i, R(39 + i) >> 16);
        }
        if (rev == 6) {
            SP32(0x5b0, R(43) + R(44));
        } else if (rev == 7) {
            uint32_t v = R(44) & 0xffffff;

            SP32(0x5b0, R(43));
            SP32(0x5b4, (R(44) << 8) & 0x80000000u ? v - 0x1000000u : v);
            SP32(0x6c4, R(45));
            SP32(0x6c8, R(46));
            SP32(0x5b8, R(47) & 0x7fffffff);
        }
    }
#undef R
    bits = e33_ld_U32(X, A_H + 0xfa8) << 3;
    e33_st_U32(X, s + 0x94, bits);
    if (e33_ld_S32(X, A_H + 0x350) == 1) {
        SP32(0xc28, bits);
        SP32(0xc2c, e33_ld_U32(X, A_H + 0x354));
    }
}

unsigned t32_rc33_idr_rows(T32Rc33 *c)
{
    E33 *X = &c->X;
    int32_t n = PS32(T33P_HEIGHT) >> 6;

    if (P32(T33P_REV) != 6 || P32(0x3a8) == 0 || n <= 0)
        return 0;
    return n < T33_RC_ROWS ? (unsigned)n : T33_RC_ROWS;
}

/* i264e_ratecontrol_is_reenc (0x7966c), FRAME_REPEATE_JUDGE skipped (F+0x4c < 0) */
void t32_rc33_end_rows(T32Rc33 *c, uint32_t bytes, const uint32_t *stats, const uint32_t *rows, unsigned nrows)
{
    E33 *X = &c->X;
    uint32_t rev, reg = A_REG;

    for (int i = 0; i < T33_RC_STATS; i++)
        e33_st_U32(X, A_TB + 8 * i + 4, stats ? stats[i] : 0);
    /* the HERA window the per-row loop below reads (vendor: /dev/mem) */
    for (unsigned i = 0; i < nrows && i < T33_RC_ROWS; i++)
        e33_st_U32(X, reg + (0x201u + 2u * i) * 4u, rows[i]);
    e33_st_U32(X, A_H + 0xfa8, bytes);
    e33_st_U32(X, A_H + 0x350, 0);
    c->n++;
    t33_rc_end(X);
    rev = P32(T33P_REV);
    if (P32(T33P_MODE) > 5 || rev - 4u > 3u)
        return;
    SP32(0x6cc, e33_ld_U32(X, A_H + 0xf9c));
    SP32(0x7dc, P32(0x3088));
    SP32(0x3ac, e33_ld_U32(X, A_S + 0x94));
    e33_st_U32(X, A_H + 0x1514, 0);
    if (rev == 6) {
        /* after an IDR: the per-row slice sizes (indexed register reads) */
        if (P32(0x3a8) != 0) {
            int32_t n = PS32(T33P_HEIGHT) >> 6;
            uint32_t sum = 0;

            SP32(0x6bc, 0);
            for (int32_t i = 0; i < n; i++) {
                uint32_t v;

                e33_st_U32(X, reg + 0x44, 0x13240000u + (0x201u + 2u * (uint32_t)i) * 4u);
                v = e33_ld_U32(X, reg + (0x201u + 2u * (uint32_t)i) * 4u);
                SP32(0x5bc + 4 * i, v);
                sum += v;
                SP32(0x6bc, sum);
            }
        }
    } else if (rev == 7) {
        int32_t h = PS32(T33P_HEIGHT), n = (h + 0x3f < 0 ? h + 0x7e : h + 0x3f) >> 6;
        uint32_t sum = 0;

        if (h > 0x800)
            n = (n + 1) / 2;
        SP32(0x6c0, 0);
        for (int32_t i = 0; i < 2 * n; i += 2) {
            uint32_t lo = e33_ld_U32(X, reg + 0x400 + 4 * (uint32_t)i);
            uint32_t hi = e33_ld_U32(X, reg + 0x404 + 4 * (uint32_t)i);
            uint32_t v = (hi & 0x3fff) * 0x4000 + (lo & 0x3fff);

            e33_st_U32(X, reg + 0x3b0, 4u * (uint32_t)(i + 1));
            SP32(0x5bc + 4 * (i >> 1), v);
            sum += v;
            SP32(0x6c0, sum);
        }
    }
    eprc_t33_h264_frame_end(X, E);
}

void t32_rc33_end(T32Rc33 *c, uint32_t bytes, const uint32_t *stats)
{
    t32_rc33_end_rows(c, bytes, stats, NULL, 0);
}

const uint8_t *t32_rc33_slice(T32Rc33 *c)
{
    return c->mem ? e33_at(&c->X, A_SL, T33_SLICE_BYTES) : NULL;
}

const uint8_t *t32_rc33_map(T32Rc33 *c, int which, uint32_t *len)
{
    E33 *X = &c->X;
    uint32_t s = A_SL, n, a, vb;
    int on;

    *len = 0;
    if (!c->mem)
        return NULL;
    n = (uint32_t)e33_ld_U16(X, s + 0xc) * e33_ld_U16(X, s + 0xe);
    switch (which) {
    case T33_MAP_SMART:
        on = e33_ld_U8(X, s + 0x159) != 0;
        a = e33_ld_U32(X, s + 0x24c);
        break;
    case T33_MAP_ROI:
        on = e33_ld_U8(X, s + 0x112) != 0;
        a = e33_ld_U32(X, s + 0x254);
        n *= 4;
        break;
    case T33_MAP_MB:
        /* vb+0xa4: the controller wrote flags (it clears the map at the
         * next FRAME_START, which must reach the engine once as well) */
        vb = P32(0xbec);
        on = e33_ld_U8(X, vb + 0xa4) != 0;
        if (!on && !c->mb_flags)
            return NULL;
        c->mb_flags = on;
        on = 1;
        a = e33_ld_U32(X, s + 0x250);
        n *= 2;
        break;
    default:
        return NULL;
    }
    if (!on || !n || a - R0 >= X->rs[0] || X->rs[0] - (a - R0) < n)
        return NULL;
    *len = n;
    return c->mem + (a - R0);
}

/* ------------------------------------------------------------------ H.265 */

/* Arena layout (region 0): the i265e handle (parameters at +0, the rate
 * control at +0x2030), the current picture block (vendor H+0x390 +0x28),
 * the statistics read list, the register window, the slice block, six VPU
 * buffers, four per-CTU maps and the work buffer VPI_set_sliceinfo lays its
 * maps out in (the first VPU buffer of the vendor) */
#define A5_RC (R0 + 0x2030)     /* rate control (param_1 of I265eRatectrl_*) */
#define E5 (A5_RC + 0x58)       /* the controller block */
#define A5_F (R0 + 0x2800)      /* current picture: +0 the parameters, +0x778 type, +0x77c QP */
#define A5_TB (R0 + 0x4c00)     /* statistics read list (F+0xd8c), 8 bytes per entry */
#define A5_REG (R0 + 0x5000)    /* the HERA register window (rc+0x490) */
#define A5_SL (R0 + 0x6000)     /* slice block (F+0xd9c -> rc+0x234) */
#define A5_BUF (R0 + 0x7000)    /* VPU buffers 2..7 (F+0x237c.. -> rc+0x348..) */
#define A5_MAPS (R0 + 0xd000)   /* per-CTU maps (F+0x2210.. -> rc+0x90..) */
#define R2_65 0x2014b4u         /* cnst_pmd_qp_thrd+8 (vendor .data), patched by the controller */

#define RC8(o, v) e33_st_U8(X, A5_RC + (o), (uint8_t)(v))
#define RC16(o, v) e33_st_U16(X, A5_RC + (o), (uint16_t)(v))
#define RC32(o, v) e33_st_U32(X, A5_RC + (o), (uint32_t)(v))
#define RCS8(o) e33_ld_S8(X, A5_RC + (o))
#define RCU8(o) e33_ld_U8(X, A5_RC + (o))
#define F8(o) e33_ld_U8(X, A5_F + (o))
#define F32(o) e33_ld_U32(X, A5_F + (o))
#define T5(o) e33_ld_U32(X, tb + (o))

/* i265e_param_default: the non-zero words of the parameter block (the
 * pointers at +0x34c.. left out), then the bytes and the doubles */
static const uint32_t t65_pdef[][2] = {
    { 0x4, 6 }, { 0xc, 5 }, { 0x10, 4 }, { 0x18, 25 }, { 0x1c, 1 }, { 0x20, 50 }, { 0x28, 4 }, { 0x4c, 35 },
    { 0x50, 35 }, { 0x54, 35 }, { 0x58, 1 }, { 0x5c, 35 }, { 0x60, 2000 }, { 0x80, 8 }, { 0x84, 3 },
    { 0x88, 15 }, { 0xa8, 16 }, { 0xac, 45 }, { 0xb0, 45 }, { 0xb4, 15 }, { 0xb8, 15 }, { 0xc0, 2 },
    { 0xc4, 2 }, { 0xcc, 80 }, { 0xd0, 6 }, { 0xd4, 100 }, { 0xd8, 1 }, { 0xdc, 2400 }, { 0xe0, 1440 },
    { 0xe4, 25 }, { 0xe8, 35 }, { 0xf0, 5 }, { 0xf4, 2 }, { 0xf8, 20 }, { 0xfc, 1600 }, { 0x100, 1200 },
    { 0x104, 2 }, { 0x108, 60 }, { 0x10c, 5 }, { 0x1c4, 0xffffffffu }, { 0x1dc, 1 }, { 0x1f0, 5 },
    { 0x1f8, 2 }, { 0x1fc, 2 }, { 0x200, 2 }, { 0x224, 0x01010100u }, { 0x23c, 8 }, { 0x240, 1 },
    { 0x244, 0x00010101u }, { 0x250, 1 }, { 0x254, 1 }, { 0x258, 0x100 }, { 0x260, 250 }, { 0x264, 64 },
    { 0x268, 8 }, { 0x270, 32 }, { 0x274, 1 }, { 0x278, 1 }, { 0x28c, 2 }, { 0x290, 3 }, { 0x298, 1 },
    { 0x29c, 2 }, { 0x2a0, 57 }, { 0x2a4, 0x01000000u }, { 0x2b4, 2 }, { 0x2c8, 0x03000000u },
    { 0x2e4, 0x200 }, { 0x318, 0x30300100u }, { 0x31c, 0x1001003fu }, { 0x320, 0x1c },
    { 0x32c, 0x11111111u }, { 0x33c, 0x20000 }, { 0x358, 2 },
};
/* the doubles: frame-rate step 0.6, VBV ratios 1.4f / 1.3f, 0.9 */
static const struct { uint16_t o; double v; } t65_pdbl[] = {
    { 0x68, 0.6 }, { 0x70, (double)1.4f }, { 0x78, (double)1.3f }, { 0xa0, 0.9 },
};

static uint32_t al4k(uint32_t v)
{
    return (v + 0xfffu) & ~0xfffu;
}

/* the coded size I265e_configure leaves (multiples of 8, at least 256 x 128) */
static uint32_t t65_dim(uint32_t v, uint32_t lo)
{
    v = v < lo ? lo : v > 0xf00u ? 0xf00u : v;
    return (v + 7u) & ~7u;
}

static uint32_t t65_map_bytes(uint32_t w, uint32_t h)
{
    uint32_t m = al4k(((w + 15) >> 4) * ((h + 15) >> 4) * 4);

    return m < 0x4000u ? 0x4000u : m;
}

int t32_rc65_open(T32Rc65 *c, uint32_t width, uint32_t height)
{
    uint32_t w = t65_dim(width, 0x100), h = t65_dim(height, 0x80);
    uint32_t n32 = ((w + 0x1f) >> 5) * ((h + 0x1f) >> 5), map = t65_map_bytes(w, h);
    uint32_t wb = A5_MAPS + 4 * map, size = wb - R0 + al4k(n32 * 0x100u + 0x4000u);
    E33 *X;

    memset(c, 0, sizeof(*c));
    c->mem = calloc(1, size);
    if (!c->mem)
        return -1;
    X = &c->X;
    X->r[0] = c->mem;
    X->rb[0] = R0;
    X->rs[0] = size;
    X->r[2] = c->tpl;
    X->rb[2] = R2_65;
    X->rs[2] = sizeof(c->tpl);
    X->calloc_addr = R1;
    /* the table words before any patch: 0, 24, 0, 40 */
    c->tpl[1] = 24;
    c->tpl[3] = 40;
    for (unsigned i = 0; i < sizeof(t65_pdef) / sizeof(t65_pdef[0]); i++)
        SP32(t65_pdef[i][0], t65_pdef[i][1]);
    for (unsigned i = 0; i < sizeof(t65_pdbl) / sizeof(t65_pdbl[0]); i++)
        e33_st_F64(X, A_P + t65_pdbl[i].o, t65_pdbl[i].v);
    /* channel_i265e_encoder_init: the words of every channel */
    SP8(0x8, 1);
    SP32(0xc, 1);
    SP32(0x10, 1);
    SP32(0x28, 4);
    SP32(T65P_WIDTH, width);
    SP32(0x34, width);
    SP32(T65P_HEIGHT, height);
    SP32(0x38, height);
    SP32(0x1c4, 1);             /* skip type < 2 */
    SP32(0x2cc, 1);             /* g_encoder_bsfull_*: mode 1, count 2, on, 250 */
    SP32(0x2d0, 2);
    SP8(0x343, 1);
    SP32(0x344, 250);
    SP16(0x348, width);
    SP16(0x34a, height);
    /* the rate control: register window open (vendor: /dev/mem at 0x13240000) */
    RC32(0x48c, 1);
    RC32(0x490, A5_REG);
    /* what i265e wires up per picture: the picture block, its statistics
     * list, slice block, maps and VPU buffers (the first one the work buffer) */
    e33_st_U32(X, R0 + 0x390 + 0x28, A5_F);
    e33_st_U32(X, A5_F, A_P);
    e33_st_U32(X, A5_F + 0x363 * 4, A5_TB);
    e33_st_U32(X, A5_F + 0x367 * 4, A5_SL);
    for (uint32_t k = 0; k < 4; k++)
        e33_st_U32(X, A5_F + (0x884 + k) * 4, A5_MAPS + map * k);
    e33_st_U32(X, A5_F + 0x8de * 4, wb);
    for (uint32_t k = 1; k < 7; k++)
        e33_st_U32(X, A5_F + (0x8de + k) * 4, A5_BUF + 0x1000 * (k - 1));
    return 0;
}

void t32_rc65_close(T32Rc65 *c)
{
    free(c->mem);
    free(c->state);
    c->mem = NULL;
    c->state = NULL;
}

void t32_rc65_set(T32Rc65 *c, uint32_t off, uint32_t v)
{
    E33 *X = &c->X;

    SP32(off, v);
}

void t32_rc65_set8(T32Rc65 *c, uint32_t off, uint8_t v)
{
    E33 *X = &c->X;

    SP8(off, v);
}

uint32_t t32_rc65_get(T32Rc65 *c, uint32_t off)
{
    E33 *X = &c->X;

    return P32(off);
}

/* the parameter words I265eRatectrl_rcInit / _rcStart / _rcEnd read */
static const uint16_t t65_pwords[] = {
    0x4, 0x18, 0x1c, 0x20, 0x2c, 0x30, 0x48, 0x4c, 0x50, 0x58, 0x5c, 0x60, 0x80, 0x84, 0xac, 0xb0, 0xb4, 0xb8,
    0xbc, 0xc0, 0xc4, 0xc8, 0xcc, 0xd0, 0xd4, 0xd8, 0xdc, 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8, 0xfc,
    0x100, 0x104, 0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128, 0x12c, 0x130, 0x134,
    0x138, 0x13c, 0x140, 0x144, 0x148, 0x14c, 0x150, 0x154, 0x158, 0x15c, 0x160, 0x164, 0x168, 0x16c,
    0x170, 0x174, 0x178, 0x17c, 0x180, 0x184, 0x188, 0x18c, 0x190, 0x194, 0x198, 0x19c, 0x1a0, 0x1a4,
    0x1a8, 0x1ac, 0x1b8, 0x1c8, 0x1cc, 0x1d0, 0x1d4, 0x1dc, 0x2c8, 0x2d4, 0x2d8, 0x2dc, 0x2e4, 0x2fc,
    0x300, 0x304, 0x308, 0x30c, 0x310, 0x314, 0x318, 0x31c, 0x320, 0x324, 0x328, 0x32c, 0x330, 0x334,
    0x338, 0x33c, 0x340, 0x344, 0x35c,
};

unsigned t32_rc65_param_words(const uint16_t **words)
{
    *words = t65_pwords;
    return sizeof(t65_pwords) / sizeof(t65_pwords[0]);
}

/* I265e_configure (0x7e840): the clamps of the words above and of the
 * words they come from (keyint, coded size, frame rate) */
void t32_rc65_configure(T32Rc65 *c)
{
#define G(o) ((int32_t)t32_rc65_get(c, (o)))
#define U(o) t32_rc65_get(c, (o))
#define S(o, v) t32_rc65_set(c, (o), (uint32_t)(v))
#define CL(o, lo, hi) S((o), clip3(G(o), (lo), (hi)))
    E33 *X = &c->X;
    uint32_t fn, fd, k, vbv, kd, w, h;
    int32_t a;
    double fps;

    CL(T65P_REV, 6, 7);
    if (!U(0xc))
        S(0xc, 1);
    if (!U(0x10))
        S(0x10, 1);
    fn = U(T65P_FPS_NUM);
    fd = U(T65P_FPS_DEN);
    if (!fn || !fd) {
        fps = 25.0;
        S(T65P_FPS_NUM, 25000);
        S(T65P_FPS_DEN, 1000);
    } else {
        fps = (double)fn / (double)fd;
    }
    /* the minimum keyint: the frame rate (at most a tenth of the maximum) */
    if (!U(0x260))
        S(0x260, 0x7fffffff);
    k = U(0x25c);
    if (k == 0 || (uint32_t)(int32_t)fps < k) {
        uint32_t m = U(0x260) / 10u;

        k = fps < 2147483648.0 ? (uint32_t)fps : (uint32_t)(fps - 2147483648.0) | 0x80000000u;
        if (m <= k)
            k = m;
        if (k == 0)
            k = 1;
    }
    S(0x25c, k);
    if (U(T65P_GOP) < k)
        S(T65P_GOP, k);
    S(0x28, 4);
    if (U(T65P_REV) == 7) {
        CL(T65P_WIDTH, 0x100, 0xc80);
        CL(T65P_HEIGHT, 0x80, 0xc80);
    } else {
        CL(T65P_WIDTH, 0x100, 0xf00);
        CL(T65P_HEIGHT, 0x80, 0xf00);
    }
    if (U(0x34) < U(T65P_WIDTH))
        S(0x34, U(T65P_WIDTH));
    if (U(0x38) < U(T65P_HEIGHT))
        S(0x38, U(T65P_HEIGHT));
    CL(T65P_MODE, 0, 5);
    CL(T65P_FIX_IQP, 0, 51);
    CL(T65P_FIX_PQP, 0, 51);
    CL(0x54, 0, 51);
    CL(T65P_MAX_QP, 0, 51);
    CL(T65P_MIN_QP, 0, G(T65P_MAX_QP));
    CL(T65P_INIT_QP, 0, 51);
    CL(T65P_MAX_IQP, 0, 51);
    CL(T65P_MIN_IQP, 0, G(T65P_MAX_IQP));
    CL(0x80, 0, 15);
    CL(0x84, 0, 15);
    CL(0xbc, -10, 10);
    if (!U(0xc0))
        S(0xc0, 1);
    CL(0xc4, 0, 8);
    CL(0xcc, 50, 100);
    CL(0xd0, 0, 7);
    CL(0xd4, 1, 100);
    CL(0xd8, 1, G(0xd4));
    CL(0xe4, 1, 100);
    CL(0xe8, 1, 51);
    CL(0xf0, 0, 10);
    CL(0xf4, 0, 3);
    CL(0xf8, 1, 100);
    /* skip type: none with the scene cut / IVDC flags (+0x33d, +0x341) */
    if (P8(0x33d) || P8(0x341))
        S(T65P_SKIP_TYPE, 0);
    CL(T65P_SKIP_TYPE, 0, 6);
    /* SMART: the still-scene count divides the static time */
    if (G(T65P_MODE) == 3 && U(0x1bc) != 0) {
        S(0x1bc, 1);
        CL(0x1b8, 1, 100);
        if (U(0xc0) % U(0x1b8))
            S(0xc0, U(0xc0) / U(0x1b8) * U(0x1b8));
    } else {
        S(0x1bc, 0);
        S(0x1b8, 0);
    }
    CL(0x1b8, 1, 100);
    /* bit rate between the frame rate and 12/6 (12/10 above 800 pixels wide)
     * bits per pixel and picture; the VBV sizes from it */
    w = U(T65P_WIDTH);
    h = U(T65P_HEIGHT);
    kd = w < 0x321 ? 6u : 10u;
    vbv = (((h + 15u) & ~15u) * ((w + 15u) & ~15u) * 12u) / kd >> 10;
    fn = U(T65P_FPS_NUM);
    fd = U(T65P_FPS_DEN);
    S(T65P_KBPS, clip3(G(T65P_KBPS), (int32_t)(fn / fd), (int32_t)(vbv * fn / fd)));
    CL(0x1c8, 0, 2);
    if (!U(T65P_VBV))
        S(T65P_VBV, vbv << 10);
    if (!U(0x1d0))
        S(0x1d0, vbv << 10);
    if (!U(0x1d4))
        S(0x1d4, vbv << 10);
    CL(T65P_VBV, 0x400, (int32_t)(vbv << 10));
    a = (int32_t)((double)G(T65P_VBV) / e33_ld_F64(X, A_P + 0x70));
    CL(0x1d0, 0x400, a);
    a = (int32_t)((double)G(0x1d0) / e33_ld_F64(X, A_P + 0x78));
    CL(0x1d4, 0x400, a);
    if (G(T65P_MODE) == 0) {
        S(0x8c, 0);
        S(T65P_KBPS, 0);
        S(0x90, 0);
        S(0x94, 0);
    }
    /* the coded size: a multiple of 8 (+0x268) */
    k = U(0x268);
    if (e33_ld_U16(X, A_P + 0x348) & (k - 1u))
        S(T65P_WIDTH, (k + U(T65P_WIDTH) - 1u) & -k);
    if (e33_ld_U16(X, A_P + 0x34a) & (k - 1u))
        S(T65P_HEIGHT, (k + U(T65P_HEIGHT) - 1u) & -k);
#undef CL
#undef S
#undef U
#undef G
}

/* I265eRatectrl_rcInit (0x83770), the register window already open */
int t32_rc65_init(T32Rc65 *c)
{
    E33 *X = &c->X;
    uint32_t w = P32(T65P_WIDTH), h = P32(T65P_HEIGHT), u;
    int8_t mode;

    /* the state block VIDEO_CFG callocs (u * 2 + 0x7b0 bytes) */
    u = (((w + 0x1f) >> 5) * ((h + 0x1f) >> 5) * 4 + 0xfu) & ~0xfu;
    free(c->state);
    c->state = calloc(1, u * 2 + 0x7b0);
    if (!c->state)
        return -1;
    X->r[1] = c->state;
    X->rb[1] = R1;
    X->rs[1] = u * 2 + 0x7b0;
    RC8(0x64, 1);
    RC8(0x8c, P32(T65P_REV));
    RC8(0x3ec, P32(T65P_MODE));
    eprc_t33_h265_default_set(X, E5);
    RC8(0x8a, P32(0xc8));
    RC8(0x3ed, P32(T65P_INIT_QP));
    RC8(0x74, P32(T65P_MAX_QP));
    RC8(0x75, P32(T65P_MIN_QP));
    RC8(0x88, 0);
    RC8(0x89, 2);
    RC8(0x76, P32(0x58));
    RC8(0x3ef, 0);
    RC16(0x5c, w);
    RC16(0x5e, h);
    RC16(0x60, (w + 0xf) >> 4);
    RC8(0x3ee, P32(T65P_SKIP_TYPE));
    RC16(0x62, (h + 0xf) >> 4);
    RC32(0x68, P32(T65P_GOP));
    RC32(0x70, P32(T65P_FPS_NUM));
    RC32(0x6c, P32(T65P_FPS_DEN));
    RC32(0x424, P32(0xdc) << 10);
    RC32(0x428, P32(0xe0) << 10);
    mode = RCS8(0x3ec);
    if (mode == 3) {
        RC8(0x3ef, 1);
        if (P32(T65P_FPS_NUM) == 0)
            return -1;          /* the vendor traps */
        RC32(0x418, (P32(T65P_GOP) * P32(T65P_FPS_DEN) * P32(0x1b8)) / P32(T65P_FPS_NUM));
        RC8(0x41c, P32(0x80));
        RC8(0x41d, P32(0x80));
        RC8(0x41e, P32(0x84));
    } else {
        RC8(0x418, P32(0x80));
        RC8(0x419, P32(0x84));
    }
    RC8(0x470, P32(0x1c8));
    RC32(0x47c, P32(0x1d4));
    RC8(0x465, 1);
    RC32(0x474, P32(T65P_VBV));
    RC32(0x478, P32(0x1d0));
    RC8(0x464, 0);
    RC32(0x468, 0xffffffffu);
    RC32(0x46c, 2);
    switch (mode) {
    case 0:
        RC8(0x3f4, P32(T65P_FIX_IQP));
        RC8(0x3f5, P32(T65P_FIX_PQP));
        break;
    case 1:
        RC32(0x3f4, P32(0xc0));
        RC32(0x3f8, P32(T65P_KBPS));
        RC8(0x3fc, P32(0xc4));
        RC8(0x408, P32(0xd4));
        RC8(0x409, P32(0xd8));
        RC8(0x40c, P32(0xd0));
        RC8(0x40a, P32(T65P_MAX_IQP));
        RC8(0x40b, P32(T65P_MIN_IQP));
        RC8(0x40e, P32(0xbc));
        RC8(0x40d, P32(0x1dc));
        break;
    case 2:
        RC32(0x3f4, P32(0xc0));
        RC32(0x3f8, P32(T65P_KBPS));
        RC8(0x408, P32(0xcc));
        RC8(0x409, P32(0xd4));
        RC8(0x40a, P32(0xd8));
        RC8(0x40b, P32(0xd0));
        RC8(0x40d, P32(0xbc));
        RC8(0x40e, P32(T65P_MAX_IQP));
        RC8(0x40f, P32(T65P_MIN_IQP));
        RC8(0x40c, P32(0x1dc));
        break;
    case 3:
    case 5:
        RC32(0x3f4, P32(0xc0));
        RC32(0x3f8, P32(T65P_KBPS));
        RC8(0x408, P32(0xcc));
        RC8(0x409, P32(0xd4));
        RC8(0x40a, P32(0xd8));
        RC8(0x40b, P32(0xd0));
        RC8(0x40d, P32(0xbc));
        RC8(0x40e, P32(0xe4));
        RC8(0x40f, P32(0xe8));
        RC8(0x410, P32(T65P_MAX_IQP));
        RC8(0x411, P32(T65P_MIN_IQP));
        RC8(0x412, P8(0x2e4));
        RC8(0x413, P8(0x2e5));
        RC8(0x414, P32(0xec));
        RC8(0x415, P32(0xf0));
        RC8(0x416, P32(0xf4));
        RC8(0x417, P32(0xf8));
        RC8(0x40c, P32(0x1dc));
        break;
    case 4:
        RC32(0x3f4, P32(0x104));
        RC32(0x3f8, P32(T65P_KBPS));
        RC32(0x3fc, P32(0x108));
        RC32(0x400, P32(T65P_LONG_MAX_KBPS));
        RC32(0x404, P32(T65P_LONG_MIN_KBPS));
        RC8(0x408, P32(0xcc));
        RC8(0x409, P32(0xd4));
        RC8(0x40a, P32(0xd8));
        RC8(0x40b, P32(0xd0));
        RC8(0x40d, P32(0xbc));
        RC16(0x410, 1);
        RC16(0x40e, P32(0x10c));
        RC8(0x412, P32(T65P_MAX_IQP));
        RC8(0x413, P32(T65P_MIN_IQP));
        RC8(0x40c, P32(0x1dc));
        break;
    default:
        break;
    }
    eprc_t33_h265_video_cfg(X, E5);
    return 0;
}

/* I265eRatectrl_rcStart (0x83ba0) */
int t32_rc65_start(T32Rc65 *c, int idr)
{
    E33 *X = &c->X;
    uint32_t type = idr ? 2u : 1u, d;
    uint8_t c1, c2;
    int8_t mode;
    int upd, keep;

    e33_st_U32(X, A5_F + 0x1de * 4, type);
    if ((F8(0x22d8) == 0 && F8(0x22d9) == 0) ? F8(0x22da) != 0 : 1)
        SP8(0x341, 0);
    RC8(0xaa, F32(0x8af * 4));
    RC8(0x3b5, F8(0x22d8));
    c1 = F8(0x22d9);
    RC8(0x3b6, c1);
    c2 = F8(0x22da);
    RC32(0xa0, type == 2);
    for (uint32_t k = 0; k < 7; k++)
        RC32(0x344 + 4 * k, F32((0x8de + k) * 4));
    RC8(0x3bc, c2);
    RC32(0x3b8, F32(0x8b7 * 4));
    RC32(0x3c0, F32(0x8b8 * 4));
    RC32(0x3c4, F32(0x8b9 * 4));
    d = P32(0x2dc);
    RC8(0x3b1, d == 0 ? 0xcu : P32(T65P_WIDTH) / d);
    RC8(0x3b2, 4);
    RC8(0x3b3, 0);
    RC8(0x3b4, 0);
    RC32(0x3ac, P32(0x2d8));
    /* the still map (+0x2d4) with its flags: the SMART rule / level go in;
     * new QP limits and rates at a refresh (+0x22ed, +0x22d8..) */
    keep = 0;
    if (P8(0x2d4) != 0) {
        keep = P32(0x2d8) != 0;
        if (P8(0x30a) != 0)
            keep = 1;
    }
    RC8(0x3b0, keep);
    upd = F8(0x22ed) != 0 || (F8(0x22d8) != 0 && (c1 != 0 || c2 != 0));
    if (upd) {
        mode = RCS8(0x3ec);
        RC8(0x74, P32(T65P_MAX_QP));
        RC8(0x75, P32(T65P_MIN_QP));
        RC32(0x424, P32(0xdc) << 10);
        RC32(0x428, P32(0xe0) << 10);
        if (mode == 1) {
            RC32(0x3f8, P32(T65P_KBPS));
            RC8(0x40a, P32(T65P_MAX_IQP));
            RC8(0x40b, P32(T65P_MIN_IQP));
        } else if (mode == 2) {
            RC32(0x3f8, P32(T65P_KBPS));
            RC8(0x40e, P32(T65P_MAX_IQP));
            RC8(0x40f, P32(T65P_MIN_IQP));
        } else if (mode == 3 || mode == 5) {
            RC32(0x3f8, P32(T65P_KBPS));
            RC8(0x410, P32(T65P_MAX_IQP));
            RC8(0x411, P32(T65P_MIN_IQP));
            RC8(0x412, keep ? P8(0x2e4) : 0);
            RC8(0x413, keep ? P8(0x2e5) : 0);
        } else if (mode == 4) {
            RC32(0x3f8, P32(T65P_KBPS));
            RC8(0x412, P32(T65P_MAX_IQP));
            RC8(0x413, P32(T65P_MIN_IQP));
        }
    }
    for (uint32_t o = 0; o < 0x4c; o += 4)
        RC32(0x360 + o, P32(0x2fc + o));
    for (uint32_t o = 0; o < 0x9c; o += 4)
        RC32(0x2a8 + o, P32(0x110 + o));
    RC32(0x234, F32(0x367 * 4));
    RC32(0x94, F32(0x885 * 4));
    RC32(0x9c, F32(0x887 * 4));
    RC8(0xac, 1);
    RC32(0x90, F32(0x884 * 4));
    RC32(0x98, F32(0x886 * 4));
    for (uint32_t k = 0; k < 4; k++)
        RC8(0x452 + k, P8(0x2c8 + k));
    RC32(0x3c8, F32(0x8ba * 4));
    RC8(0x3cc, F32(0x8bb * 4));
    RC8(0x3cd, F8(0x22ee));
    RC8(0x3ce, F8(0x22ef));
    RC32(0x3d0, F32(0x8bc * 4));
    RC8(0x3d4, P8(0x35c) ? F32(0x8bd * 4) : 0u);
    RC8(0x3d5, F8(0x22f5));
    eprc_t33_h265_frame_start(X, E5);
    SP32(0x2d8, 0);
    e33_st_U32(X, A5_F + 0x1df * 4, RCU8(0x3d8));
    c->type = RCU8(0x3d9);
    if ((uint32_t)c->type != type)
        return -1;              /* the vendor logs and aborts */
    c->qp = RCU8(0x3d8);
    return c->qp;
}

uint32_t t32_rc65_rc_word(T32Rc65 *c, uint32_t off)
{
    return e33_ld_U32(&c->X, A5_RC + off);
}

unsigned t32_rc65_idr_rows(T32Rc65 *c)
{
    E33 *X = &c->X;
    uint32_t n = P32(T65P_HEIGHT) >> 6;

    if (RCU8(0x8c) != 6 || F32(0x1de * 4) != 2 || n == 0)
        return 0;
    return n < T33_RC_ROWS ? n : T33_RC_ROWS;
}

/* I265eRatectrl_rcEnd (0x84040) */
void t32_rc65_end_rows(T32Rc65 *c, uint32_t bytes, const uint32_t *stats, const uint32_t *rows, unsigned nrows)
{
    E33 *X = &c->X;
    uint32_t tb = A5_TB, reg = A5_REG, type, u3, u16, u4, v;

    for (int i = 0; i < T65_RC_STATS; i++)
        e33_st_U32(X, tb + 8 * i + 4, stats ? stats[i] : 0);
    for (unsigned i = 0; i < nrows && i < T33_RC_ROWS; i++)
        e33_st_U32(X, reg + (0x201u + 2u * i) * 4u, rows[i]);
    e33_st_U32(X, A5_F + 0x448 * 4, bytes);
    c->n++;
    type = F32(0x1de * 4);
    RC32(0xa4, bytes * 8u + (type == 2 ? 800u : 100u));
    RC32(0xbc, T5(4));
    RC32(0xc0, T5(0x14));
    RC32(0xc4, T5(0xc));
    v = T5(0x1c);
    RC16(0xb4, v);
    RC16(0xb6, T5(0x24));
    RC16(0xb8, v >> 16);
    for (uint32_t k = 0; k < 4; k++) {
        v = T5(0x2c + 8 * k);
        RC16(0xc8 + 4 * k, v);
        RC16(0xca + 4 * k, v >> 16);
    }
    u3 = T5(0x104);
    u16 = u3 & 0x3fffffffu;
    u4 = u16 >> 11;
    RC8(0xa9, u4 == 0 ? 0xffu : (((T5(0x10c) & 0x3fffffu) * 4u + (u3 >> 30) + (u16 >> 12)) / u4));
    if (RCU8(0x8c) == 6) {
        RC32(0x114, T5(0xec) + T5(0xf4));
        if (type == 2) {
            uint32_t n = P32(T65P_HEIGHT) >> 6, sum = 0;

            /* the coded size of every 64-line row (indexed register reads) */
            RC32(0x224, 0);
            for (uint32_t i = 0; i < n; i++) {
                e33_st_U32(X, reg + 0x44, 0x13240000u + (0x201u + 2u * i) * 4u);
                v = e33_ld_U32(X, reg + (0x201u + 2u * i) * 4u);
                RC32(0x124 + 4 * i, v);
                sum += v;
                RC32(0x224, sum);
            }
        }
    } else if (RCU8(0x8c) == 7) {
        uint32_t h = P32(T65P_HEIGHT), n = (h + 0x3f) >> 6, sum = 0;

        RC32(0x114, T5(0xec));
        v = T5(0xf4) & 0xffffffu;
        RC32(0x118, (T5(0xf4) << 8) & 0x80000000u ? v - 0x1000000u : v);
        RC32(0x22c, T5(0x114));
        RC32(0x230, T5(0x11c));
        RC32(0x228, (T5(0x124) & 0x1ffffffu) << 3);
        RC32(0x11c, T5(0x12c) & 0x7fffffffu);
        if (h > 0x800)
            n = (n + 1) >> 1;
        RC32(0x120, 0);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t lo, hi;

            e33_st_U32(X, reg + 0x3b0, 8u * i);
            lo = e33_ld_U32(X, reg + 0x400 + 8u * i);
            e33_st_U32(X, reg + 0x3b0, 4u + 8u * i);
            hi = e33_ld_U32(X, reg + 0x404 + 8u * i);
            v = (hi & 0x3fffu) * 0x4000u + (lo & 0x3fffu);
            RC32(0x124 + 4 * i, v);
            sum += v;
            RC32(0x120, sum);
        }
    }
    eprc_t33_h265_frame_end(X, E5);
}
