/* T33 encoder-side scaling (#99), see openimp_t33_resize.c */
#ifndef OPENIMP_T33_RESIZE_H
#define OPENIMP_T33_RESIZE_H

#include <stdint.h>

/* uint16 entries of the table for a dw x dh_al destination */
#define T33_RESIZE_TAB(dw, dh) (4u * (uint32_t)(dw) + 4u * (uint32_t)(dh))

/* vendor c_resize_c arguments: source and its pitch p3 (= width) / 16-aligned height p4, destination of
 * p5 x p6 (16-aligned height); crop on: p8 = pitch * top + left, p9 / p10 = width / height of the cut-out */
void openimp_t33_resize_c(const uint8_t *src, uint8_t *dst, int p3, int p4, int p5, int p6, int crop, int p8,
                          int p9, int p10, uint16_t *tab);

#endif
