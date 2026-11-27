/*
 * T30 NCU engine internals shared by t30_ncu.c and the generated per-gain download
 * t30_ncu_par104.c (Lu-Fi/openimp #701).
 */
#ifndef OPENIMP_T30_NCU_INTERNAL_H
#define OPENIMP_T30_NCU_INTERNAL_H

#include <stdint.h>

/* NCU register access relative to 0x13090000 (vendor ncu_reg_write / ncu_reg_read). */
void t30_ncu_wr(uint32_t reg, uint32_t val);
uint32_t t30_ncu_rd(uint32_t reg);

/* vendor ncu_func_linear_insert / ncu_func_non_linear_insert: 9 values, 9 gain points, gain */
int32_t t30_ncu_lin(int32_t v0, int32_t v1, int32_t v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6,
                    int32_t v7, int32_t v8, int32_t g0, int32_t g1, int32_t g2, int32_t g3, int32_t g4,
                    int32_t g5, int32_t g6, int32_t g7, int32_t g8, int32_t gain);
int32_t t30_ncu_nlin(int32_t v0, int32_t v1, int32_t v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6,
                     int32_t v7, int32_t v8, int32_t g0, int32_t g1, int32_t g2, int32_t g3, int32_t g4,
                     int32_t g5, int32_t g6, int32_t g7, int32_t g8, int32_t gain);

/* vendor ncu_image_size_width / _height (ncu_func_config) */
extern int t30_ncu_img_w;
extern int t30_ncu_img_h;

/* vendor 1.0.4 ncu_func_par_download: pb = active parameter set, param_1 = total gain */
int t30_ncu_par104_download(const uint8_t *pb, int32_t param_1);

/* vendor 1.0.5 ncu_func_par_download (nm 0xd86d8): pb = active 0x760-byte set; -1 on a zero divisor
 * (the vendor traps) */
int t30_ncu_par105_download(const uint8_t *pb, int32_t param_1);

#ifdef T30_NCU_GENERATED
/* names used by the generated transcription (t30_ncu_par104.c only) */
typedef uint32_t uint;
typedef uint32_t undefined4;
typedef uint8_t undefined;
typedef uint8_t byte;
#define W(r, v) t30_ncu_wr((uint32_t)(r), (uint32_t)(v))
#define R(r) t30_ncu_rd((uint32_t)(r))
#define LIN t30_ncu_lin
#define NLIN t30_ncu_nlin
#define P(o) (*(const uint32_t *)(const void *)(pb + (o)))
#define ncu_img_w t30_ncu_img_w
#define ncu_img_h t30_ncu_img_h
#endif

#endif
