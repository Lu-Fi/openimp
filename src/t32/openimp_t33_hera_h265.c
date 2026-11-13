/*
 * OpenIMP T33 HERA v1.6 H.265 command lists (#391, planned).  See
 * openimp_t33_hera_h265.h; register names as in openimp_t32_hera_h265.c.
 */
#include <string.h>

#include "t32/openimp_t33_hera.h"
#include "t32/openimp_t33_hera_h265.h"
#include "t30/radix/t30_radix_cabac.h"

#if !defined(PLATFORM_T33)
#error "openimp_t33_hera_h265.c is the PLATFORM_T33 build only"
#endif

typedef struct {
    uint32_t reg, vi, vp;               /* offset, I default, P default */
    uint16_t n;                         /* >1: a run of n (context states) */
    uint8_t when;                       /* 1 I, 2 P, 3 both */
    uint16_t band_i, band_p;            /* 1-based t33_hera_h265_band index, 0 none */
} T32Hera5Ent;

typedef struct {
    uint8_t qp;                         /* first QP of the run */
    uint32_t v;
} T32HeraBand;

#include "t32/openimp_t33_hera_h265_tab.h"

#define BUF_SHARE       (1u << 31)
#define DMA_BIT30       (1u << 30)
#define JRFD_BIT27      (1u << 27)

#define BC_CTRL         0x60000u
#define BC_QP           0x60004u
#define BC_CTX          0x61000u
#define SDE_CTRL        0x68000u
#define SDE_CTRL_EPB    (1u << 28)      /* the core adds emulation prevention */
#define SDE_SIZE        0x68004u
#define SDE_BS          0x68008u
#define SDE_CTX         0x69000u
#define IPRED_CTX       0x70004u
#define IPRED_SIZE      0x7000cu
#define MCE_CTRL        0x30000u
#define MCE_FRM_SIZE    0x30060u
#define MCE_FRM_STRD    0x30064u
#define MCE_BFSH_ADRY   0x30070u
#define MCE_BFSH_ADRC   0x30074u
#define MCE_END_Y       0x30078u        /* T32: BOTM_ID; v1.6: the areas' ends */
#define MCE_END_C       0x3007cu
#define MCE_RLUT_Y      0x30800u
#define MCE_RLUT_C      0x30804u
#define MCE_SIZE2       0x3807cu
#define MD_CTRL         0x40004u
#define MD_CTB          0x40008u
#define MD_QP           0x40010u
#define MD_LAMBDA       0x40280u
#define DBLK_SIZE       0x50000u
#define SAO_SIZE        0x58028u
#define STC_SIZE        0x78000u
#define STC_CTB         0x78004u
#define STC_CTX         0x79004u
#define EFE_CTRL        0x20000u
#define EFE_FRM_SIZE    0x20008u
#define EFE_RAWY        0x2000cu
#define EFE_RAWC        0x20010u
#define EFE_RAW_STRD    0x20014u
#define EFE_FRM_QP      0x2001cu
#define EFE_CPLX_QP0    0x2003cu
#define EFE_CPLX_QP1    0x20040u
#define JRFD_CTRL       0x28004u
#define JRFD_HDR_Y      0x28008u
#define JRFD_HDR_C      0x2800cu
#define JRFD_REF_Y      0x28014u
#define JRFD_REF_C      0x28018u
#define JRFD_RING_Y     0x28020u
#define JRFD_RING_END   0x2802cu
#define ODMA_CTRL       0x10004u
#define ODMA_BDYA       0x10008u
#define ODMA_BDCA       0x1000cu
#define ODMA_HDR_Y      0x10020u
#define ODMA_HDR_C      0x10024u
#define ODMA_SIZE_Y     0x10028u        /* I: area sizes; P: the areas' ends */
#define ODMA_SIZE_C     0x1002cu
#define ODMA_RING_Y     0x10030u
#define ODMA_RING_END   0x1003cu
#define ODMA_HDR_RY     0x10174u        /* the header set read (as JRFD 0x28008) */
#define ODMA_BASE_Y     0x10178u
#define ODMA_BASE_C     0x1017cu
#define ODMA_REF_Y      0x10180u        /* as JRFD 0x28014 */
#define ODMA_CMP        0x10184u        /* 75/128 of the luma picture (vendor, 640x368..1920x1088) */
#define ODMA_FRM        0x10188u        /* I: the NV12 picture's bytes; P: the luma base */
#define EMC_CTRL        0x90000u
#define EMC_BS0         0x90008u
#define EMC_CPS         0x9000cu
#define EMC_AI          0x90018u
#define EMC_BS1         0x9001cu
#define EMC_A24         0x90024u
#define EMC_QPT         0x90028u
#define EMC_A2C         0x9002cu
#define EMC_IPC         0x90030u        /* 0 on v1.6 H.265 */
#define EMC_AI_PREV     0x90034u
#define EMC_RRS0        0x90038u
#define EMC_AI2         0x90050u
#define EMC_BSFULL_EN   0x9005cu
#define EMC_BSFULL_SIZE 0x90064u

/* BC context run (33): per position the T30 Radix HEVC state index, found
 * by matching the vendor's I and P lists at QP 0..51 */
static const uint8_t bc33_ctx[33] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  12,  13,
     14,  19,  20,  21,  22,  23,  26,  27, 150,  24,  20,  34,
     35,  36,  10,  28,  29,  30,  31,  32,  33
};

void t33_hera_h265_hdr(uint32_t width, uint32_t height, uint32_t hdr, unsigned set,
                       uint32_t *y, uint32_t *c)
{
    T33HeraRing r;

    t33_hera_ring_layout(width, height, &r);
    *y = hdr + (set & 1u) * r.hy;
    *c = hdr + 2u * r.hy + (set & 1u) * r.hc;
}

static uint32_t band(unsigned b, int qp, uint32_t def)
{
    const T32HeraBand *t;

    if (!b)
        return def;
    t = &t33_hera_h265_band[b - 1];
    while (t[1].qp != 0 && (int)t[1].qp <= qp)
        t++;
    return t->v;
}

static uint32_t t33_h265_field(const T33HeraH265Pic *t, uint32_t reg, uint32_t v, const uint8_t *st,
                               unsigned idx)
{
    const T32HeraH265Pic *p = &t->pic;
    uint32_t w = p->width, h = p->height, h16 = (h + 15u) & ~15u, qp = (uint32_t)p->qp;
    uint32_t ctb_w = (w + 63u) / 64u, ctb_h = (h + 63u) / 64u;
    uint32_t ft = p->p_frame ? 1u : 2u;     /* HEVC slice type */
    int named = !p->p_frame || t->ref_after_idr;    /* the list names the area */

    switch (reg) {
    case BC_CTRL:                       /* cu_qp_delta, slice type, size in 8x8 units */
        return 1u << 30 | 1u << 21 | 1u << 20 | ft << 18 | ((h / 8u - 1u) & 0x1ffu) << 9 |
               ((w / 8u - 1u) & 0x1ffu);
    case BC_QP:                         /* two merge candidates, QP */
        return 2u << 7 | qp << 1;
    case BC_CTX:
        return st[bc33_ctx[idx]];
    case SDE_CTRL:                      /* core EPB, dqp depth 2, bit 23, dqp, slice type, start */
        return SDE_CTRL_EPB | 2u << 18 | 1u << 23 | 1u << 4 | ft << 2 | 1u;
    case SDE_SIZE: case DBLK_SIZE: case SAO_SIZE:
        return w << 16 | h;
    case SDE_CTX:                       /* states 24 and 25 moved to the end (one of them) */
        return st[idx < 24u ? idx : idx < 151u ? idx + 2u : 24u];
    case IPRED_CTX:
        return st[13];
    case IPRED_SIZE: case MCE_FRM_SIZE:
        return (h - 1u) << 16 | (w - 1u);
    case MCE_SIZE2: case EFE_FRM_SIZE:
        return h << 16 | w;
    case MD_CTRL:                       /* the T30 rule: partial CTB rows / columns */
        return (p->p_frame ? 0x08077771u : 0x08077702u) | (h % 16u ? 1u << 23 : 0u) |
               (w % 16u ? 1u << 22 : 0u);
    case MD_CTB:
        return (ctb_h - 1u) << 24 | ((ctb_w - 1u) << 8 & 0xffffu);
    case MD_QP:                         /* dqp depth 2, bit 6, QP */
        return 1u << 8 | 1u << 6 | qp;
    case MD_LAMBDA:
        return 0xb0000000u | qp << 20;
    case STC_SIZE:
        return (h & 0x1fffu) << 13 | (w & 0x1fffu);
    case STC_CTB:
        return 0x55u << 16 | (ctb_h & 0xffu) << 8 | (ctb_w & 0xffu);
    case STC_CTX:
        return 1u << 31 | st[152];
    case EFE_CTRL:                      /* P: reference and analysis reads */
        return p->p_frame ? 0x2c1eu : 0x2816u;
    case EFE_RAWY:
        return p->src_y;
    case EFE_RAWC:
        return p->src_c;
    case EFE_RAW_STRD:
        return p->src_stride << 16 | p->src_stride;
    case EFE_FRM_QP:                    /* max 51, min 1, QP */
        return 51u << 24 | 1u << 16 | qp << 8;
    case EFE_CPLX_QP0:
        return t32_hera_cplx_qp(qp, 0);
    case EFE_CPLX_QP1:
        return t32_hera_cplx_qp(qp, 1);
    case MCE_CTRL:
        return 0xf8080001u | (p->after_p ? 1u << 11 : 0u);
    case MCE_FRM_STRD:
        return w << 16 | w;
    case MCE_RLUT_Y: case ODMA_BASE_Y:
        return p->ring_y;
    case MCE_RLUT_C: case ODMA_BASE_C:
        return p->ring_c;
    case MCE_BFSH_ADRY:
        return t->ref_after_idr ? p->ring_y : 0u;
    case MCE_BFSH_ADRC:
        return t->ref_after_idr ? p->ring_c : 0u;
    case MCE_END_Y:
        return p->ring_y_end;
    case MCE_END_C:
        return p->ring_c_end;
    case JRFD_CTRL:
        return BUF_SHARE | JRFD_BIT27 | h16 << 14 | w;
    case ODMA_CTRL:
        return BUF_SHARE | DMA_BIT30 | h << 14 | w;
    case JRFD_REF_Y: case ODMA_REF_Y:
        return named ? p->ring_y : 0u;
    case JRFD_REF_C:
        return named ? p->ring_c : 0u;
    case JRFD_HDR_Y: case ODMA_HDR_RY:
        return t->hdr_ry;
    case JRFD_HDR_C:
        return t->hdr_rc;
    case ODMA_HDR_Y:
        return t->hdr_wy;
    case ODMA_HDR_C:
        return t->hdr_wc;
    case JRFD_RING_Y: case ODMA_RING_Y:
        return p->ring_y;
    case JRFD_RING_Y + 4: case ODMA_RING_Y + 4:
        return p->ring_c;
    case JRFD_RING_Y + 8: case ODMA_RING_Y + 8:
        return p->ring_y_end;
    case JRFD_RING_END: case ODMA_RING_END:
        return p->ring_c_end;
    case ODMA_BDYA:
        return p->p_frame ? 0u : p->ring_y;
    case ODMA_BDCA:
        return p->p_frame ? 0u : p->ring_c;
    case ODMA_SIZE_Y:
        return p->p_frame ? p->ring_y_end : p->ring_y_end - p->ring_y;
    case ODMA_SIZE_C:
        return p->p_frame ? p->ring_c_end : p->ring_c_end - p->ring_c;
    case ODMA_CMP:
        return w * h16 / 128u * 75u + (w * h16 % 128u) * 75u / 128u;
    case ODMA_FRM:
        return p->p_frame ? p->ring_y : w * h16 * 3u / 2u;
    case EMC_CTRL:
        return p->p_frame ? 0x637u : 0x37u;
    case EMC_BS0: case SDE_BS:
        return p->bs0;
    case EMC_BS1:
        return p->bs1;
    case EMC_BSFULL_EN:
        return p->section_kb ? 1u : 0u;
    case EMC_BSFULL_SIZE:
        return p->section_kb;
    case EMC_CPS: case EMC_RRS0:
        return p->aux[T32_HERA5_CPS];
    case EMC_A24:
        return p->aux[T32_HERA5_A24];
    case EMC_QPT:
        return p->aux[T32_HERA5_QPT];
    case EMC_AI: case EMC_AI2:
        return p->aux[T32_HERA5_AI];
    case EMC_AI_PREV:
        return p->p_frame ? p->aux[T32_HERA5_AI] : 0u;
    case EMC_A2C:
        return p->aux[T32_HERA5_A2C];
    case EMC_IPC:
        return 0u;
    }
    return v;
}

size_t t33_hera_h265_list(const T33HeraH265Pic *t, uint32_t *list)
{
    static const size_t ents = sizeof(t33_hera_h265_tab) / sizeof(t33_hera_h265_tab[0]);
    const T32HeraH265Pic *p = t ? &t->pic : NULL;
    uint8_t st[T30_RADIX_CABAC_CONTEXTS];
    unsigned want, i, k;
    size_t n = 0;

    if (!p || !list || p->width < 256u || p->height < 64u || p->width % 16u || p->height % 8u ||
        p->width > 4096u || p->height > 2304u || p->qp < 0 || p->qp > 51)
        return 0;
    t30_radix_cabac_init(st, p->p_frame ? 1 : 2, p->qp);
    want = p->p_frame ? 2u : 1u;
    for (i = 0; i < ents; i++) {
        const T32Hera5Ent *e = &t33_hera_h265_tab[i];
        uint32_t def = p->p_frame ? band(e->band_p, p->qp, e->vp) : band(e->band_i, p->qp, e->vi);

        if (!(e->when & want))
            continue;
        for (k = 0; k < e->n; k++) {
            uint32_t reg = e->n > 1 ? e->reg + 4u * k : e->reg;
            uint32_t v = t33_h265_field(t, e->reg, def, st, k);

            if (n >= T32_HERA5_LIST_MAX_PAIRS)
                return 0;
            list[2 * n] = v;
            list[2 * n + 1] = t32_hera_cmd(reg, v, i + 1 == ents);
            n++;
        }
    }
    return n;
}
