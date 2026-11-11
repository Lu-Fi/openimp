/*
 * OpenIMP T32 JPEG decoder helpers (#301, planned): the header parse and the
 * engine job of the vendor libimp 2.0.0 ijpegd / hwicodec_pf_jpegd_t32p
 * (openimp_t32_decoder.c).  No device access: host-tested
 * (tests/t32/dec_test.c).
 */
#ifndef OPENIMP_T32_JPEGD_H
#define OPENIMP_T32_JPEGD_H

#include <stdint.h>

#include "t32/openimp_t32_ioctl.h"

typedef struct {
    uint32_t qt[4][64];         /* DQT by Tq, file (zigzag) order */
    uint32_t width, height, comps;
    uint32_t restart;           /* DRI, 0 for none */
    uint32_t scan_len;          /* bytes copied, with the EOI appended */
    int sof, sos;
} T32DecHeader;

/* markers up to the first scan; the scan (stuffing and RSTn as they are,
 * until another marker) copied to @scan with FF D9 appended: 0 or -1 */
int OpenIMP_T32_DecParse(const uint8_t *p, uint32_t len, T32DecHeader *h, uint8_t *scan, uint32_t scan_max);

/* the job of T32P_JPEGD_SliceInit for an engine geometry @w x @hgt
 * (align16(maxWidth) x align8(maxHeight)) and decode_mode @mode */
void OpenIMP_T32_DecBuildJob(T32IocJpegDecRegInfo *j, const T32DecHeader *h, uint32_t w, uint32_t hgt,
                             uint32_t mode, uint32_t out_pa, uint32_t scan_pa);

#endif
