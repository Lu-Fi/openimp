/*
 * OpenIMP T33 HERA v1.6 H.265 command lists (#391, planned; not a release gate).
 *
 * The T32 H.265 list (openimp_t32_hera_h265.h) in the form vendor libimp
 * 2.0.2.1 writes it on the T33 (QEMU -M ingenic-t33 FIXQP traces, QP 0..51):
 * I 591 pairs, P 626 (T32: 692 / 840).  Deltas against the T32:
 *   - BC CABAC context run of 33 states (T32: 145), the SDE run unchanged
 *   - the compressed reconstruction area of the H.264 path
 *     (t33_hera_ring_layout), its two header sets laid out Y, Y, C, C and
 *     written / read in turn (ODMA 0x10020 / 0x10024, JRFD 0x28008 /
 *     0x2800c, ODMA 0x10174); new ODMA words 0x10170..0x10188
 *   - the I list names the area, the P list after it the reference
 *     (JRFD 0x28014 / 0x28018, MCE 0x30070 / 0x30074, ODMA 0x10180)
 *   - SDE 0x68000: core emulation prevention (bit 28), no QP field
 *   - MCE step tables gone; analysis / mode-decision defaults per QP band
 *     from the vendor's sweep (openimp_t33_hera_h265_tab.h)
 */
#ifndef OPENIMP_T33_HERA_H265_H
#define OPENIMP_T33_HERA_H265_H

#include <stddef.h>
#include <stdint.h>

#include "t32/openimp_t32_hera_h265.h"

typedef struct {
    T32HeraH265Pic pic;                 /* ring_* = the data areas; recon / ref unused */
    uint32_t hdr_wy, hdr_wc;            /* the header set this picture writes */
    uint32_t hdr_ry, hdr_rc;            /* the set its reference wrote */
    int ref_after_idr;                  /* P: the reference is the I picture */
} T33HeraH265Pic;

/* the header sets of an H.265 area of @width x @height after @hdr: set @set's
 * luma and chroma headers (Y A, Y B, C A, C B) */
void t33_hera_h265_hdr(uint32_t width, uint32_t height, uint32_t hdr, unsigned set,
                       uint32_t *y, uint32_t *c);

/* the list into @list (T32_HERA5_LIST_MAX_PAIRS * 2 words): pairs written, 0 on bad input */
size_t t33_hera_h265_list(const T33HeraH265Pic *p, uint32_t *list);

#endif
