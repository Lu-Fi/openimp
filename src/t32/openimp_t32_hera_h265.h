/*
 * OpenIMP T32 HERA H.265 command lists (#294, planned; not a release gate).
 *
 * Same list format as H.264 (openimp_t32_hera.h: value / VLD | TERM |
 * checksum | offset pairs, EFE STAT = 1 then CFGC TERM).  The picture is
 * coded in CTBs of 64 by the engine's HEVC path (BC / SDE / IPRED / MD /
 * TFM / DBLK / SAO / STC blocks); libimp loads the CABAC context states
 * into BC 0x61000 (145) and SDE 0x69000 (152).
 *
 * t32_hera_h265_list() writes the list of one picture.  The register order
 * and the defaults are vendor libimp 2.2.0's at FIXQP (generated table,
 * tools/t32/hera_h265_table.py; QP bands for the analysis defaults that
 * follow the QP); sizes, addresses, QP fields and the CABAC states are
 * computed here (states: the T30 Radix HEVC init, src/t30/radix).  Fixed
 * QP: the hardware QP generator stays off.
 *
 * Reconstruction / reference: the H.264 ring (t32_hera_ring_*): an I
 * picture at the ring's base, each P picture T32_HERA_RING_EXTRA rows
 * before its reference.  A P picture after a P picture also reads the
 * analysis output of the last one (EMC 0x90034, the AI buffer).
 */
#ifndef OPENIMP_T32_HERA_H265_H
#define OPENIMP_T32_HERA_H265_H

#include <stddef.h>
#include <stdint.h>

#include "t32/openimp_t32_hera.h"

#define T32_HERA5_LIST_MAX_PAIRS 1024u  /* I 692, P 840 */
#define T32_HERA5_LIST_BYTES     (T32_HERA5_LIST_MAX_PAIRS * 8u)

/* analysis buffers: EMC 0x9000c (+ 0x90038), 0x90024, 0x90028, 0x90018 (+ 0x90050;
 * the last picture's at 0x90034), 0x9002c */
enum { T32_HERA5_CPS, T32_HERA5_A24, T32_HERA5_QPT, T32_HERA5_AI, T32_HERA5_A2C, T32_HERA5_AUX };

typedef struct {
    uint32_t width, height;             /* picture size: width multiple of 16, height of 8 */
    int p_frame;                        /* 0 I (IDR), 1 P (one reference) */
    int after_p;                        /* P: the picture before was a P */
    int first;                          /* the channel's first picture (an I) */
    int qp;                             /* 0..51 */
    uint32_t src_y, src_c, src_stride;  /* NV12 source */
    uint32_t ring_y, ring_y_end;        /* reconstruction ring, luma: base, beyond */
    uint32_t ring_c, ring_c_end;        /* chroma */
    uint32_t recon_y, recon_c;          /* this picture's reconstruction in the ring */
    uint32_t ref_y, ref_c;              /* P: the reference in the ring */
    uint32_t bs0, bs1;                  /* stream: one buffer (bs1 0) or two halves */
    uint32_t section_kb;                /* 0: no sections; else BSFULL after this many KiB */
    uint32_t aux[T32_HERA5_AUX];        /* analysis buffers (t32_hera_h265_aux_sizes) */
} T32HeraH265Pic;

/* sizes of the analysis buffers the engine reads and writes */
void t32_hera_h265_aux_sizes(uint32_t width, uint32_t height, uint32_t size[T32_HERA5_AUX]);

/* the list into @list (T32_HERA5_LIST_MAX_PAIRS * 2 words): pairs written, 0 on bad input */
size_t t32_hera_h265_list(const T32HeraH265Pic *p, uint32_t *list);

#endif
