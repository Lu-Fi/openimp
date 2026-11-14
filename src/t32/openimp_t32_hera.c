/*
 * OpenIMP T32 HERA H.264 command lists (#293, planned).  See openimp_t32_hera.h.
 *
 * Module bases (offset >> 15): CFGC 0, VDMA 1, ODMA 2, TMC 3, EFE 4, JRFD 5,
 * MCE 6, TFM 7, MD 8, DT 9, DBLK 10, SAO 11, BC 12, SDE 13, IPRED 14,
 * STC 15, VMAU 16, EMC 18.  Field layouts as the 2.0.0 SDK kernel headers
 * name them (RE reference only); the values written for one picture are
 * checked against the vendor's lists by tests/t32/hera_list_test.c.
 */
#include <string.h>

#include "t32/openimp_t32_hera.h"
#include "t30/h264enc/cabac.h"

typedef struct {
    uint32_t reg, vi, vp;               /* offset, I default, P default */
    uint16_t n;                         /* >1: a run of n (context tables) */
    uint8_t when;                       /* 1 I, 2 P, 3 both */
} T32HeraEnt;

#include "t32/openimp_t32_hera_tab.h"

#define CL_VLD  (1u << 31)
#define CL_TERM (1u << 30)

/* EFE: front end (source, picture QP, analysis) */
#define EFE_FRM_SIZE    0x20008u
#define EFE_RAWY        0x2000cu
#define EFE_RAWC        0x20010u
#define EFE_RAW_STRD    0x20014u
#define EFE_FRM_QP      0x2001cu
#define EFE_CPLX_QP0    0x2003cu
#define EFE_CPLX_QP1    0x20040u
#define EFE_ROI_INFOA   0x20054u        /* ..D 0x20060 */
#define EFE_ROI_INFOD   0x20060u
#define EFE_QPG_DLT     0x20300u
#define EFE_MBQP_CFG    0x2029cu        /* MB QP clamp and map enables (rate control) */
/* RC: the controller's row budgets */
#define RC_ROW_CFG      0x40100u        /* rows, MBs a row, enables */
#define RC_ROW_SHIFT    0x40104u
#define RC_ROW_TGT      0x40108u        /* 0x40108, 0x40114 .. 0x40124: six words */
#define RC_ROW_TGT_END  0x40124u
#define RC_ROW_BITS     0x40900u        /* + 4 i: the cumulative target of row i */
#define RC_ROWS_MAX     64u
#define EFE_STAT        0x20004u
/* JRFD: reference read */
#define JRFD_CTRL       0x28004u
#define JRFD_REF_Y      0x28014u
#define JRFD_REF_C      0x28018u
#define JRFD_STRD       0x2801cu
#define JRFD_RING_Y     0x28020u        /* Y base, C base, Y beyond, C beyond */
#define JRFD_RING_END   0x2802cu
/* MCE: motion search (P) */
#define MCE_FRM_SIZE    0x30060u
#define MCE_FRM_STRD    0x30064u
#define MCE_BFSH_ADRY   0x30070u
#define MCE_BFSH_ADRC   0x30074u
#define MCE_BOTM_ID     0x30078u
#define MCE_RLUT_Y      0x30800u
#define MCE_RLUT_C      0x30804u
/* VMAU: mode decision */
#define VMAU_CTX        0x8002cu
#define VMAU_MD_CFG0    0x80030u
#define VMAU_FRM_SIZE   0x80054u
#define VMAU_MD_CFG7    0x80114u
/* SDE: CABAC */
#define SDE_SL_GEOM     0x68008u
#define SDE_CFG1        0x68018u
#define SDE_CTX_TBL     0x6a000u
/* EMC: stream and analysis outputs */
#define EMC_BS0         0x90008u
#define EMC_CPS         0x9000cu
#define EMC_BS1         0x9001cu
#define EMC_QPT         0x90028u
#define EMC_IPC         0x90030u
#define EMC_RRS0        0x90038u
#define EMC_AI          0x90050u
#define EMC_BSFULL_EN   0x9005cu
#define EMC_BSFULL_SIZE 0x90064u
/* ODMA: reconstruction write */
#define ODMA_CTRL       0x10004u
#define ODMA_BDYA       0x10008u
#define ODMA_BDCA       0x1000cu
#define ODMA_RING_Y     0x10030u        /* Y base, C base, Y beyond, C beyond */
#define ODMA_RING_END   0x1003cu

#define BUF_SHARE       (1u << 31)
#define JRFD_BIT30      (1u << 30)      /* set by the vendor on every list */

/*
 * The SDE's CABAC range table: per pStateIdx the four rangeTabLPS entries
 * in the engine's packing (bit 0 is the context's valMPS).  Hardware data,
 * as every vendor list carries it.
 */
static const uint32_t hera_lps_range[64] = {
    0xeeceaefc, 0xe1c3a5fc, 0xd6b99cfc, 0xcbb094f2, 0xc1a78ce4, 0xb79e85da, 0xad967ece, 0xa48e78c4,
    0x9c8772ba, 0x94806cb0, 0x8c7966a6, 0x8573619e, 0x7e6d5c96, 0x7867578e, 0x72625386, 0x6c5d4e80,
    0x66584a78, 0x61544672, 0x5c4f436c, 0x574b3f66, 0x53473c62, 0x4e43395c, 0x4a403658, 0x463d3352,
    0x4339304e, 0x3f362e4a, 0x3c342b46, 0x39312942, 0x362e273e, 0x332c253c, 0x30292338, 0x2e272136,
    0x2b251f32, 0x29231d30, 0x27211c2c, 0x251f1a2a, 0x231e1928, 0x211c1826, 0x1f1b1624, 0x1d191522,
    0x1c181420, 0x1a17131e, 0x1915121c, 0x1714111a, 0x16131018, 0x15120f18, 0x14110e16, 0x13100d14,
    0x120f0c14, 0x110e0c12, 0x100d0b12, 0x0f0d0a10, 0x0e0c0a10, 0x0d0b090e, 0x0c0a090e, 0x0c0a080c,
    0x0b09070c, 0x0a09070a, 0x0a08070a, 0x0908060a, 0x09070608, 0x08070508, 0x07060508, 0x00000000,
};

uint32_t t32_hera_cmd(uint32_t reg, uint32_t value, int term)
{
    uint32_t sum = (value & 0xffu) + (value >> 8 & 0xffu) + (value >> 16 & 0xffu) +
                   (value >> 24) + (reg & 0xffu) + (reg >> 8 & 0xffu);

    return CL_VLD | (term ? CL_TERM : 0u) | (sum & 0x3ffu) << 20 | (reg & 0xffffcu);
}

/* the SDE context table word of a CABAC state 1..126 (T33 builder, #391) */
uint32_t t32_hera_sde_ctx(unsigned state)
{
    return hera_lps_range[state <= 63u ? 63u - state : (state - 64u) & 63u] | (state >> 6 & 1u);
}

static uint32_t hera_qpg_dlt(uint32_t qp);

uint32_t t32_hera_qpg_dlt(uint32_t qp)
{
    return hera_qpg_dlt(qp);
}

void t32_hera_cabac_states(int p_frame, int qp, uint8_t state[460])
{
    static h264_cabac_t cb;             /* 1 KiB of states: not on a thread's stack */
    static int ready;

    if (!ready) {
        h264_cabac_init();
        ready = 1;
    }
    h264_cabac_context_init(&cb, p_frame ? 0 : 2, qp, 0);       /* SLICE_TYPE_P 0, I 2 */
    memcpy(state, cb.state, 460);
}

void t32_hera_aux_sizes(uint32_t width, uint32_t height, uint32_t size[4])
{
    uint32_t mbs = ((width + 15u) / 16u) * ((height + 15u) / 16u);

    size[0] = (mbs * 24u + 255u) & ~255u;       /* cps: 24 bytes a macroblock */
    size[1] = (mbs * 4u + 255u) & ~255u;        /* ai mark */
    size[2] = (mbs + 255u) & ~255u;             /* qpt: a QP a macroblock */
    size[3] = (mbs * 4u + 255u) & ~255u;        /* ipc */
}

uint32_t t32_hera_ring_prev(uint32_t addr, uint32_t base, uint32_t end, uint32_t rows_bytes)
{
    return addr - base >= rows_bytes ? addr - rows_bytes : end - (rows_bytes - (addr - base));
}

/*
 * The analysis QP tables of the FIXQP defaults (the engine's adaptive QP
 * is off: qpg_en 0); their values follow the picture QP.
 */
static int hera_roi_qp(int qp)            /* ROI QP of the (disabled) ROI slots */
{
    return qp - 9 < 1 ? 1 : qp - 9 > 26 ? 26 : qp - 9;
}

/* complexity QP offsets (signed bytes) by QP band, measured QP 0..51 */
uint32_t t32_hera_cplx_qp(uint32_t qp, int which)
{
    if (which)
        return qp < 27u ? 0x04040302u : qp < 39u ? 0x04040303u : qp < 42u ? 0x04030201u : 0x04030200u;
    return qp < 20u ? 0x01fffbf6u : qp < 27u ? 0x01fef9f6u : qp < 39u ? 0x01fdf8f6u :
           qp < 42u ? 0x00fdf8f6u : qp < 46u ? 0xfffcf8f6u : 0xfefcf8f6u;
}

static uint32_t hera_qpg_dlt(uint32_t qp)   /* four 6-bit QP delta thresholds */
{
    return qp < 30u ? 0x00208205u : qp < 42u ? 0x00145145u : 0x00145148u;
}

uint32_t t32_hera_roi_info(int qp)
{
    uint32_t b = (uint32_t)(hera_roi_qp(qp) & 0x3f) << 2 | 1u;

    return b | b << 8 | b << 16 | b << 24;
}

static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint32_t hera_field(const T32HeraH264Pic *p, uint32_t reg, uint32_t v, const uint8_t *st,
                           unsigned idx)
{
    uint32_t w = p->width, h = p->height, mbw = w / 16u, mbh = h / 16u;
    uint32_t qp = (uint32_t)p->qp;

    if (p->rc_slice && p->rc_slice[0x874] == 6u) {
        const uint8_t *sl = p->rc_slice;

        switch (reg) {
        case EFE_FRM_QP:                /* QP, MB QP min, max (vendor: s+0x153; the slice header's QP) */
            return (sl[0x18] & 0x1fu) | qp << 8 | (uint32_t)sl[0x826] << 16 | (uint32_t)sl[0x825] << 24;
        case EFE_MBQP_CFG:
            return (sl[0x52a] & 1u) << 22 | (sl[0x76f] & 0x3fu) << 16 | (sl[0x76e] & 0x3fu) << 10 |
                   (sl[0x76d] & 1u) << 9 | (sl[0x112] & 1u) << 8 | (sl[0x113] & 0xfu) << 4 |
                   (sl[0x15a] & 1u) << 3 | (sl[0x158] & 1u) << 2 | (sl[0x18c] & 1u);
        case RC_ROW_CFG:
            return (((rd32(sl + 0x52c) - 1u) & 0xffffu) << 4) + (sl[0x52a] & 1u) + 0x500000u +
                   ((uint32_t)sl[0x530] << 23) + ((uint32_t)sl[0x76d] << 31);
        case RC_ROW_SHIFT:
            return (sl[0x769] & 0x1fu) << 5 | (sl[0x768] & 0x1fu) | (sl[0x76a] & 0x1fu) << 10 |
                   (sl[0x76b] & 7u) << 15 | (sl[0x76c] & 7u) << 18 | (sl[0x534] & 7u) << 21;
        case RC_ROW_TGT:
            return rd32(sl + 0x536);
        case RC_ROW_TGT + 0xc: case RC_ROW_TGT + 0x10: case RC_ROW_TGT + 0x14: case RC_ROW_TGT + 0x18:
        case RC_ROW_TGT_END:
            return rd32(sl + 0x53a + (reg - (RC_ROW_TGT + 0xc)));
        }
    }
    switch (reg) {
    case EFE_FRM_SIZE:
    case VMAU_FRM_SIZE:
        return h << 16 | w;
    case EFE_RAWY:
        return p->src_y;
    case EFE_RAWC:
        return p->src_c;
    case EFE_RAW_STRD:
        return p->src_stride << 16 | p->src_stride;
    case EFE_FRM_QP:                    /* base QP, min, max (I: all the picture QP) */
        return p->p_frame ? qp << 8 | 1u << 16 | 51u << 24 : qp << 8 | qp << 16 | qp << 24;
    case EFE_CPLX_QP0:
        return t32_hera_cplx_qp(qp, 0);
    case EFE_CPLX_QP1:
        return t32_hera_cplx_qp(qp, 1);
    case EFE_ROI_INFOA: case EFE_ROI_INFOA + 4: case EFE_ROI_INFOA + 8: case EFE_ROI_INFOD:
        return t32_hera_roi_info(p->qp);
    case EFE_QPG_DLT:
        return hera_qpg_dlt(qp);
    case JRFD_CTRL:
        return BUF_SHARE | JRFD_BIT30 | h << 14 | w;
    case ODMA_CTRL:
        return BUF_SHARE | h << 14 | w;
    case JRFD_REF_Y:                    /* an I list names the ring's base */
        return p->p_frame ? p->ref_y : p->ring_y;
    case JRFD_REF_C:
        return p->p_frame ? p->ref_c : p->ring_c;
    case JRFD_STRD:
        return t32_hera_ring_stride(w) << 16 | t32_hera_ring_stride(w) / 2u;
    case JRFD_RING_Y: case ODMA_RING_Y:
        return p->ring_y;
    case JRFD_RING_Y + 4: case ODMA_RING_Y + 4:
        return p->ring_c;
    case JRFD_RING_Y + 8: case ODMA_RING_Y + 8:
        return p->ring_y_end;
    case JRFD_RING_END: case ODMA_RING_END:
        return p->ring_c_end;
    case MCE_FRM_SIZE:
        return (h - 1u) << 16 | (w - 1u);
    case MCE_FRM_STRD:
        return t32_hera_ring_stride(w) << 16 | t32_hera_ring_stride(w);
    case MCE_RLUT_Y:
        return p->ring_y;
    case MCE_RLUT_C:
        return p->ring_c;
    case MCE_BFSH_ADRY:
        return p->ref_y;
    case MCE_BFSH_ADRC:
        return p->ref_c;
    case MCE_BOTM_ID: {                 /* 64-row units from the reference to the ring's end, - 1 */
        uint32_t rows = (p->ring_y_end - p->ref_y) / t32_hera_ring_stride(w);
        return rows / 64u - 1u;
    }
    case VMAU_MD_CFG0:                  /* last macroblock row / column, bit 15, slice type + 1 */
        return (mbh - 1u) << 24 | (mbw - 1u) << 16 | 1u << 15 | (p->p_frame ? 2u : 1u);
    case VMAU_MD_CFG7:                  /* base QP; size_x 4, size_y 4 */
        return (qp & 0x3fu) << 17 | 0x44u;
    case VMAU_CTX:                      /* 42 states: mb_type, then mvd / cbp / ... */
        return st[idx < 10 ? (p->p_frame ? idx + 14 : idx + 3) :
                  idx < 22 ? idx - 10 + 73 : idx < 28 ? idx - 22 + 64 : idx - 28 + 40];
    case SDE_SL_GEOM:
        return mbh << 24 | mbw << 16;
    case SDE_CFG1:                      /* QP, 8x8 transform, skip, slice type + 1 */
        return qp << 8 | 1u << 5 | 1u << 4 | (p->p_frame ? 2u : 1u);
    case SDE_CTX_TBL: {
        unsigned s = st[idx];
        return hera_lps_range[s <= 63 ? 63 - s : s - 64] | (s >> 6 & 1u);
    }
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
    case EMC_AI:
        return p->ai;
    case EMC_QPT:
        return p->qpt;
    case EMC_IPC:
        return p->ipc;
    case ODMA_BDYA:
        return p->recon_y;
    case ODMA_BDCA:
        return p->recon_c;
    }
    return v;
}

size_t t32_hera_h264_list(const T32HeraH264Pic *p, uint32_t *list)
{
    uint8_t st[460];
    unsigned want, i, k;
    size_t n = 0;

    if (!p || !list || !p->width || !p->height || p->width % 16u || p->height % 16u ||
        p->width > 4096u || p->height > 4096u || p->qp < 0 || p->qp > 51)
        return 0;
    t32_hera_cabac_states(p->p_frame, p->qp, st);
    want = p->p_frame ? 2u : 1u;
    for (i = 0; i < sizeof(t32_hera_h264_tab) / sizeof(t32_hera_h264_tab[0]); i++) {
        const T32HeraEnt *e = &t32_hera_h264_tab[i];
        uint32_t def = p->p_frame ? e->vp : e->vi;

        if (!(e->when & want))
            continue;
        for (k = 0; k < e->n; k++) {
            uint32_t reg = e->reg == SDE_CTX_TBL ? e->reg + 4u * k : e->reg;
            uint32_t v = hera_field(p, e->reg, def, st, k);
            int last = i + 1 == sizeof(t32_hera_h264_tab) / sizeof(t32_hera_h264_tab[0]);

            if (n >= T32_HERA_LIST_MAX_PAIRS)
                return 0;
            list[2 * n] = v;
            list[2 * n + 1] = t32_hera_cmd(reg, v, last);
            n++;
        }
        /* rate control: the row budgets follow the row block (vendor order) */
        if (e->reg == RC_ROW_TGT_END && p->rc_slice && p->rc_slice[0x874] == 6u) {
            uint32_t rows = rd32(p->rc_slice + 0x530);

            for (k = 0; k < rows && k < RC_ROWS_MAX; k++) {
                uint32_t v = rd32(p->rc_slice + 0x568 + 4u * k);

                if (n >= T32_HERA_LIST_MAX_PAIRS)
                    return 0;
                list[2 * n] = v;
                list[2 * n + 1] = t32_hera_cmd(RC_ROW_BITS + 4u * k, v, 0);
                n++;
            }
        }
    }
    return n;
}
