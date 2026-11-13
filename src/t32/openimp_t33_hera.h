/*
 * OpenIMP T33 HERA v1.6 H.264 command lists (#391, planned; not a release gate).
 *
 * The T33 (PRJ008) encoder is the T32's HERA in revision 1.6.  Vendor libimp
 * 2.0.2.1 (QEMU -M ingenic-t33 FIXQP traces) writes a list of the T32 kind
 * (pairs, checksum, TERM) with these deltas against libimp 2.2.0 on the T32:
 *   - VMAU moved from 0x80000 to 0x40400 (context states 0x40448, MD_CFG0
 *     0x40410, frame size 0x4040c, MD_CFG7 0x4042c), its tail to 0x70000;
 *     a new block at 0x38000; I 776 pairs, P 811 (T32: 736 / 913, the MCE
 *     step tables 0x30900 / 0x30a00 are gone)
 *   - the reconstruction is compressed: one picture-sized area per plane
 *     (t33_hera_ring_layout) plus two sets of compression headers, written
 *     (0x10020 / 0x10024) and read (0x28008 / 0x2800c) in turn; the lists
 *     give the reconstruction address of an I picture only (0x10008 /
 *     0x1000c, 0 on P) and the reference address of the P picture after it
 *     only (0x28014 / 0x28018, 0x30070 / 0x30074; 0 on later P pictures)
 *   - the core adds the emulation prevention bytes (SDE 0x68018 bit 7):
 *     libimp copies the slice data as it comes
 * Register order and the defaults of the other registers are the vendor's
 * (openimp_t33_hera_tab.h, tools/t32/hera_h264_table.py --t33).
 */
#ifndef OPENIMP_T33_HERA_H
#define OPENIMP_T33_HERA_H

#include <stddef.h>
#include <stdint.h>

#include "t32/openimp_t32_hera.h"

#define T33_HERA_LIST_PAIRS_I 776u
#define T33_HERA_LIST_PAIRS_P 811u

/* the compressed reconstruction area of a picture: sizes in bytes */
typedef struct {
    uint32_t y, c;                      /* data area per plane */
    uint32_t hy, hc;                    /* one set of compression headers per plane */
} T33HeraRing;

void t33_hera_ring_layout(uint32_t width, uint32_t height, T33HeraRing *r);
/* bytes of the whole area: data, then two header sets (Y, C, Y, C) */
uint32_t t33_hera_ring_bytes(uint32_t width, uint32_t height);

typedef struct {
    T32HeraH264Pic pic;                 /* ring_* = the data areas; recon / ref unused */
    uint32_t hdr_wy, hdr_wc;            /* the header set this picture writes */
    uint32_t hdr_ry, hdr_rc;            /* the set its reference wrote */
    int ref_after_idr;                  /* P: the reference is the I picture */
} T33HeraH264Pic;

/* the list into @list (T32_HERA_LIST_MAX_PAIRS * 2 words): pairs written, 0 on bad input */
size_t t33_hera_h264_list(const T33HeraH264Pic *p, uint32_t *list);

#endif
