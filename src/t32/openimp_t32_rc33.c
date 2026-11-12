/*
 * OpenIMP T32 H.264 rate control, eprc T33 revision (#295, planned): the
 * vendor 2.2.0 i264e side of the controller (openimp_t32_rc33.h).
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

/* i264e_ratecontrol_start (0x78f0c) */
int t32_rc33_start(T32Rc33 *c, uint32_t since_idr)
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
