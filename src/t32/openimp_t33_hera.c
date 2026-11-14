/*
 * OpenIMP T33 HERA v1.6 H.264 command lists (#391, planned).  See
 * openimp_t33_hera.h; register names as in openimp_t32_hera.c.
 */
#include <string.h>

#include "t32/openimp_t33_hera.h"

#if !defined(PLATFORM_T33)
#error "openimp_t33_hera.c is the PLATFORM_T33 build only"
#endif

typedef struct {
    uint32_t reg, vi, vp;               /* offset, I default, P default */
    uint16_t n;                         /* >1: a run of n (context tables) */
    uint8_t when;                       /* 1 I, 2 P, 3 both */
} T32HeraEnt;

#include "t32/openimp_t33_hera_tab.h"

#define BUF_SHARE       (1u << 31)
#define DMA_BIT30       (1u << 30)      /* set by the vendor on ODMA and JRFD */
#define JRFD_BIT27      (1u << 27)

/* ODMA: reconstruction write */
#define ODMA_CTRL       0x10004u
#define ODMA_BDYA       0x10008u
#define ODMA_BDCA       0x1000cu
#define ODMA_HDR_Y      0x10020u        /* compression headers written: Y, C */
#define ODMA_HDR_C      0x10024u
#define ODMA_SIZE_Y     0x10028u        /* I: area sizes; P: the areas' ends */
#define ODMA_SIZE_C     0x1002cu
#define ODMA_RING_Y     0x10030u        /* Y base, C base, Y beyond, C beyond */
#define ODMA_RING_END   0x1003cu
/* EFE: source, sizes, QP */
#define EFE_FRM_SIZE    0x20008u
#define EFE_RAWY        0x2000cu
#define EFE_RAWC        0x20010u
#define EFE_RAW_STRD    0x20014u
#define EFE_FRM_QP      0x2001cu
#define EFE_CPLX_QP0    0x2003cu
#define EFE_CPLX_QP1    0x20040u
#define EFE_QPG_DLT     0x20300u
/* JRFD: reference read */
#define JRFD_CTRL       0x28004u
#define JRFD_HDR_Y      0x28008u        /* compression headers read: Y, C */
#define JRFD_HDR_C      0x2800cu
#define JRFD_REF_Y      0x28014u
#define JRFD_REF_C      0x28018u
#define JRFD_STRD       0x2801cu
#define JRFD_RING_Y     0x28020u
#define JRFD_RING_END   0x2802cu
/* MCE */
#define MCE_FRM_SIZE    0x30060u
#define MCE_FRM_STRD    0x30064u
#define MCE_BFSH_ADRY   0x30070u
#define MCE_BFSH_ADRC   0x30074u
#define MCE_END_Y       0x30078u        /* T32: BOTM_ID; v1.6: the areas' ends */
#define MCE_END_C       0x3007cu
#define MCE_RLUT_Y      0x30800u
#define MCE_RLUT_C      0x30804u
/* VMAU (v1.6 at 0x40400) and its tail */
#define VMAU_FRM_SIZE   0x4040cu
#define VMAU_MD_CFG0    0x40410u
#define VMAU_MD_CFG7    0x4042cu
#define VMAU_CTX        0x40448u
#define DBLK_FRM_SIZE   0x50000u        /* width << 16 | height */
#define TAIL_FRM_SIZE   0x7000cu
/* SDE */
#define SDE_SL_GEOM     0x68008u
#define SDE_CFG1        0x68018u
#define SDE_CFG1_EPB    (1u << 7)       /* the core adds emulation prevention */
#define SDE_CTX_TBL     0x6a000u
/* EMC: stream and analysis outputs */
#define EMC_BS0         0x90008u
#define EMC_CPS         0x9000cu
#define EMC_BS1         0x9001cu
#define EMC_QPT         0x90028u
#define EMC_AI2         0x9002cu        /* v1.6: the AI address twice */
#define EMC_IPC         0x90030u
#define EMC_RRS0        0x90038u
#define EMC_AI          0x90050u
#define EMC_BSFULL_EN   0x9005cu
#define EMC_BSFULL_SIZE 0x90064u

static uint32_t t33_field(const T33HeraH264Pic *t, uint32_t reg, uint32_t v, const uint8_t *st, unsigned idx)
{
    const T32HeraH264Pic *p = &t->pic;
    uint32_t w = p->width, h = p->height, mbw = w / 16u, mbh = h / 16u;
    uint32_t qp = (uint32_t)p->qp;

    switch (reg) {
    case EFE_FRM_SIZE:
    case VMAU_FRM_SIZE:
    case TAIL_FRM_SIZE:
        return h << 16 | w;
    case DBLK_FRM_SIZE:
        return w << 16 | h;
    case EFE_RAWY:
        return p->src_y;
    case EFE_RAWC:
        return p->src_c;
    case EFE_RAW_STRD:
        return p->src_stride << 16 | p->src_stride;
    case EFE_FRM_QP:                    /* base QP, min 1, max 51 on I and P */
        return qp << 8 | 1u << 16 | 51u << 24;
    case EFE_CPLX_QP0:
        return t32_hera_cplx_qp(qp, 0);
    case EFE_CPLX_QP1:
        return t32_hera_cplx_qp(qp, 1);
    case EFE_QPG_DLT:
        return t32_hera_qpg_dlt(qp);
    case ODMA_CTRL:
        return BUF_SHARE | DMA_BIT30 | h << 14 | w;
    case JRFD_CTRL:
        return BUF_SHARE | DMA_BIT30 | JRFD_BIT27 | h << 14 | w;
    case ODMA_BDYA:                     /* I: the area's base; P: the engine's read-back */
        return p->p_frame ? t->recon_y : p->ring_y;
    case ODMA_BDCA:
        return p->p_frame ? t->recon_c : p->ring_c;
    case ODMA_HDR_Y:
        return t->hdr_wy;
    case ODMA_HDR_C:
        return t->hdr_wc;
    case ODMA_SIZE_Y:                   /* what is left to the area's end */
        return p->ring_y_end - (p->p_frame ? t->recon_y : p->ring_y);
    case ODMA_SIZE_C:
        return p->ring_c_end - (p->p_frame ? t->recon_c : p->ring_c);
    case JRFD_HDR_Y:
        return t->hdr_ry;
    case JRFD_HDR_C:
        return t->hdr_rc;
    case JRFD_REF_Y:                    /* I: the base; P: the reference's reconstruction */
        return p->p_frame ? t->ref_y : p->ring_y;
    case JRFD_REF_C:
        return p->p_frame ? t->ref_c : p->ring_c;
    case MCE_BFSH_ADRY:
        return p->p_frame ? t->ref_y : 0u;
    case MCE_BFSH_ADRC:
        return p->p_frame ? t->ref_c : 0u;
    case JRFD_STRD:
        return w << 16 | w / 2u;
    case JRFD_RING_Y: case ODMA_RING_Y: case MCE_RLUT_Y:
        return p->ring_y;
    case JRFD_RING_Y + 4: case ODMA_RING_Y + 4: case MCE_RLUT_C:
        return p->ring_c;
    case JRFD_RING_Y + 8: case ODMA_RING_Y + 8: case MCE_END_Y:
        return p->ring_y_end;
    case JRFD_RING_END: case ODMA_RING_END: case MCE_END_C:
        return p->ring_c_end;
    case MCE_FRM_SIZE:
        return (h - 1u) << 16 | (w - 1u);
    case MCE_FRM_STRD:
        return w << 16 | w;
    case VMAU_MD_CFG0:                  /* last macroblock row / column, bit 15, slice type + 1 */
        return (mbh - 1u) << 24 | (mbw - 1u) << 16 | 1u << 15 | (p->p_frame ? 2u : 1u);
    case VMAU_MD_CFG7:                  /* base QP; size_x 4, size_y 4 */
        return (qp & 0x3fu) << 17 | 0x44u;
    case VMAU_CTX:                      /* 42 states: mb_type, then mvd / cbp / ... */
        return st[idx < 10 ? (p->p_frame ? idx + 14 : idx + 3) :
                  idx < 22 ? idx - 10 + 73 : idx < 28 ? idx - 22 + 64 : idx - 28 + 40];
    case SDE_SL_GEOM:
        return mbh << 24 | mbw << 16;
    case SDE_CFG1:                      /* QP, core EPB, 8x8 transform, slice type + 1 */
        return qp << 8 | SDE_CFG1_EPB | 1u << 5 | (p->p_frame ? 2u : 1u);
    case SDE_CTX_TBL:
        return t32_hera_sde_ctx(st[idx]);
    case EMC_BSFULL_EN:
        return p->section_kb ? 1u : 0u;
    case EMC_BSFULL_SIZE:
        return p->section_kb;
    case EMC_BS0:
        return p->bs0;
    case EMC_BS1:
        return p->bs1;
    case EMC_CPS: case EMC_RRS0:
        return p->cps;
    case EMC_AI: case EMC_AI2:
        return p->ai;
    case EMC_QPT:
        return p->qpt;
    case EMC_IPC:
        return p->ipc;
    }
    return v;
}

size_t t33_hera_h264_list(const T33HeraH264Pic *t, uint32_t *list)
{
    const T32HeraH264Pic *p = t ? &t->pic : NULL;
    const size_t ents = sizeof(t33_hera_h264_tab) / sizeof(t33_hera_h264_tab[0]);
    uint8_t st[460];
    unsigned want, k;
    size_t n = 0, i;

    if (!p || !list || !p->width || !p->height || p->width % 16u || p->height % 16u ||
        p->width > 4096u || p->height > 4096u || p->qp < 0 || p->qp > 51)
        return 0;
    t32_hera_cabac_states(p->p_frame, p->qp, st);
    want = p->p_frame ? 2u : 1u;
    for (i = 0; i < ents; i++) {
        const T32HeraEnt *e = &t33_hera_h264_tab[i];
        uint32_t def = p->p_frame ? e->vp : e->vi;

        if (!(e->when & want))
            continue;
        for (k = 0; k < e->n; k++) {
            uint32_t reg = e->reg == SDE_CTX_TBL ? e->reg + 4u * k : e->reg;
            uint32_t v = t33_field(t, e->reg, def, st, k);

            if (n >= T32_HERA_LIST_MAX_PAIRS)
                return 0;
            list[2 * n] = v;
            list[2 * n + 1] = t32_hera_cmd(reg, v, i + 1 == ents);
            n++;
        }
    }
    return n;
}

/*
 * The vendor's area sizes (2.0.2.1 lists at 640x368 .. 1920x1088): per
 * plane the picture's rows plus 64, plus 16 KiB (luma) / 8 KiB (chroma);
 * a header set 16 bytes per 64 x 16 block, both counts rounded up to 4
 * (chroma half of it, each part on 256 bytes).
 */
void t33_hera_ring_layout(uint32_t width, uint32_t height, T33HeraRing *r)
{
    uint32_t w = (width + 15u) & ~15u, h = (height + 15u) & ~15u;

    r->y = w * (h + 64u) + 16384u;
    r->c = r->y / 2u;
    r->hy = 16u * (((w + 63u) / 64u + 3u) & ~3u) * ((h / 16u + 3u) & ~3u);
    r->hc = (r->hy / 2u + 255u) & ~255u;  /* engine addresses: 256-byte steps */
}

uint32_t t33_hera_ring_bytes(uint32_t width, uint32_t height)
{
    T33HeraRing r;

    t33_hera_ring_layout(width, height, &r);
    return r.y + r.c + 2u * (r.hy + r.hc);
}
