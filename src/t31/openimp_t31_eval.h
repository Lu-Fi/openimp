#ifndef OPENIMP_T31_EVAL_H
#define OPENIMP_T31_EVAL_H

/* T31 1.1.6 IMP_Encoder_GetChnEvalInfo: the per-picture statistics of the
 * Allegro status registers (the T31 header's IMPEncoderStreamInfo,
 * IMPEncoderJpegInfo for JPEG).  libimp copies them
 * in AL_EncChannel_EndEncoding (stream info +0x164 .. +0x188) from the
 * merged slice status (EncodingStatusRegsToSliceStatus + MergeEncodingStatus)
 * and update_one_frmstrm moves them to channel record +0x2e0 when the
 * channel thread takes the stream; GetChnEvalInfo returns 36 bytes (an H.264
 * or H.265 channel) or 8 bytes (JPEG) of it. */

#include <stdint.h>
#include <string.h>

#define T31_EVAL_INFO_SIZE      36u
#define T31_EVAL_INFO_JPEG_SIZE 8u

typedef struct {
    /* The T31 header's IMPEncoderStreamInfo without its leading iNumBytes:
     * libimp takes the 36 bytes from stream info +0x164, the field after
     * iNumBytes (+0x160), so the record starts with uNumIntra.  Slice
     * status +0x1c, +0x20, +0x24, +0x28, +0x2c, +0x30, from the status
     * registers 0x114, 0x11c, 0x120, 0x124, 0x128 and 0x12c (16 bit), summed
     * over the picture's slices. */
    uint32_t num_intra;         /* uNumIntra:  8x8 blocks coded intra */
    uint32_t num_skip;          /* uNumSkip:   8x8 blocks coded skip */
    uint32_t num_cu8x8;         /* uNumCU8x8 */
    uint32_t num_cu16x16;       /* uNumCU16x16 */
    uint32_t num_cu32x32;       /* uNumCU32x32 */
    uint32_t num_cu64x64;       /* uNumCU64x64 */
    /* slice status +0x3c, +0x3e, +0x40 */
    uint16_t slice_qp;          /* iSliceQP: libimp writes it on one of its
                                 * two completion paths only and leaves
                                 * stack residue on the other; here always
                                 * the QP the picture was coded at */
    uint16_t min_qp;            /* iMinQP: lowest of the byte pair in status
                                 * register 0x138 */
    uint16_t max_qp;            /* iMaxQP: highest */
    /* stream info +0x182 .. +0x187: never written, zero (the block is
     * cleared with memset before each picture) */
    uint8_t pad[6];
} T31EvalInfo;
_Static_assert(sizeof(T31EvalInfo) == T31_EVAL_INFO_SIZE,
               "IMP_Encoder_GetChnEvalInfo record");

/* regs: the 0x168-byte status block of a completed picture (one slice). */
static inline void openimp_t31_eval_from_status(T31EvalInfo *out,
                                                const uint8_t *regs,
                                                uint32_t slice_qp)
{
    uint32_t qp;
    uint16_t half;

    memset(out, 0, sizeof(*out));
    memcpy(&out->num_intra, regs + 0x114u, sizeof(uint32_t));
    memcpy(&out->num_skip, regs + 0x11cu, sizeof(uint32_t));
    memcpy(&out->num_cu8x8, regs + 0x120u, sizeof(uint32_t));
    memcpy(&out->num_cu16x16, regs + 0x124u, sizeof(uint32_t));
    memcpy(&out->num_cu32x32, regs + 0x128u, sizeof(uint32_t));
    memcpy(&half, regs + 0x12cu, sizeof(half));
    out->num_cu64x64 = half;
    memcpy(&qp, regs + 0x138u, sizeof(qp));
    out->slice_qp = (uint16_t)slice_qp;
    out->min_qp = (uint16_t)(qp & 0xffu);
    out->max_qp = (uint16_t)((qp >> 8) & 0xffu);
}

#endif
