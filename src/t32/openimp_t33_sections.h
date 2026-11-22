/*
 * T33 bitstream sections (#784 group 3) as vendor libimp 2.0.2.1 sets them
 * up, pure C (host test: tests/t33/ms784_test.c).
 *
 * Vendor (reverse-engineered reference only, objdump addresses):
 *  - module words (data +0x2fe0.., initial values from the data section):
 *    JpegBsSize +0x2fe0 = 512000, IvpuBsSize +0x2fe4 = 8294400, JPEG
 *    multi-section count / size KiB / on +0x2fe8 / 0x2fec / 0x2ff0 = 2 / 250
 *    / 1, H.26x multi-section count / size KiB / on +0x2ff4 / 0x2ff8 / 0x2ffc
 *    (+ byte 0x3000) = 2 / 250 / 1.  SetMultiSectionMode (0xf7208) and
 *    SetJpegMultiSectionMode (0xf9140) store on = (mode != 0), the size and
 *    count 2 whatever cnt is; all four setters answer 0x80040080 once the
 *    encoder module is up.
 *  - EncoderInit (0xe09d0..0xe0b48, cpu type 1 path 0xe0eb0..0xe0f00): the
 *    "ivpuBs" (HERA) pool is size KiB x 2 while the H.26x mode is on, else
 *    IvpuBsSize bytes; the "jpegBs" pool likewise from the JPEG words.
 *  - HERA (QEMU vms1 / vms0 / v5ms0 vs vb4 / v5b4): mode on writes EMC
 *    0x9001c (second half), 0x9005c = 1 and 0x90064 = size KiB; mode off
 *    writes none of them (the whole pool is one picture buffer).
 *  - JPEG CreateChn (0xe39b0 / 0xe3d84) and the job (QEMU vjb / vjm1 / vjm0):
 *    mode on: control bit 29, no limit, 0x44 second half, 0x48 = 1, 0x4c =
 *    size KiB; mode off: control bits 16 / 26 (limit, over-limit stop), limit
 *    0x2c = JpegBsSize, no BSFULL fields.
 */
#ifndef OPENIMP_T33_SECTIONS_H
#define OPENIMP_T33_SECTIONS_H

#include <stdint.h>

#define T33_BS_IVPU_DEFAULT 8294400u    /* data +0x2fe4 */
#define T33_BS_JPEG_DEFAULT 512000u     /* data +0x2fe0 */
#define T33_MS_KB_DEFAULT   250         /* data +0x2fec / +0x2ff8 */
#define T33_MS_CNT          2           /* data +0x2fe8 / +0x2ff4, set by the setters */

/* @section_kb: BSFULL stop size (0: off), @bytes: the pool */
static inline void t33_sections(int on, int size_kb, uint32_t bs_size, uint32_t *section_kb, uint32_t *bytes)
{
    if (on) {
        *section_kb = (uint32_t)size_kb;
        *bytes = ((uint32_t)size_kb << 10) * (uint32_t)T33_MS_CNT;
    } else {
        *section_kb = 0;
        *bytes = bs_size;
    }
}

/* the JPEG job's control word / limit for @section_kb from the base word (limit + over-limit stop set) */
static inline uint32_t t33_jpeg_ctrl(uint32_t base, uint32_t section_kb)
{
    return section_kb ? (base & ~(1u << 16 | 1u << 26)) | 1u << 29 : base;
}

static inline uint32_t t33_jpeg_limit(uint32_t section_kb, uint32_t bytes)
{
    return section_kb ? 0u : bytes;
}

#endif
