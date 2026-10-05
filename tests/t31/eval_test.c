/* IMP_Encoder_GetChnEvalInfo (T31 libimp 1.1.6): the 36-byte record built
 * from the Allegro status block (src/t31/openimp_t31_eval.h).  Offsets are
 * those of EncodingStatusRegsToSliceStatus (status regs 0x114 .. 0x138 ->
 * slice status +0x1c .. +0x40) and AL_EncChannel_EndEncoding (slice status
 * -> stream info +0x164 ..). */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "t31/openimp_t31_eval.h"

int main(void)
{
    uint8_t regs[0x168];
    T31EvalInfo ev;
    uint32_t w;
    uint16_t h;
    unsigned int i;

    _Static_assert(sizeof(T31EvalInfo) == 36, "GetChnEvalInfo returns 36 bytes");
    assert(T31_EVAL_INFO_SIZE == 36u && T31_EVAL_INFO_JPEG_SIZE == 8u);
    /* IMPEncoderStreamInfo without iNumBytes: 6 words, 3 halfwords */
    assert(offsetof(T31EvalInfo, num_intra) == 0);
    assert(offsetof(T31EvalInfo, num_skip) == 4);
    assert(offsetof(T31EvalInfo, num_cu8x8) == 8);
    assert(offsetof(T31EvalInfo, num_cu16x16) == 12);
    assert(offsetof(T31EvalInfo, num_cu32x32) == 16);
    assert(offsetof(T31EvalInfo, num_cu64x64) == 20);
    assert(offsetof(T31EvalInfo, slice_qp) == 24);
    assert(offsetof(T31EvalInfo, min_qp) == 26);
    assert(offsetof(T31EvalInfo, max_qp) == 28);
    assert(offsetof(T31EvalInfo, pad) == 30);

    for (i = 0; i < sizeof(regs); i++)
        regs[i] = (uint8_t)(0xa0u + i);
    w = 0x11111111u; memcpy(regs + 0x114, &w, 4);
    w = 0x22222222u; memcpy(regs + 0x11c, &w, 4);
    w = 0x33333333u; memcpy(regs + 0x120, &w, 4);
    w = 0x44444444u; memcpy(regs + 0x124, &w, 4);
    w = 0x55555555u; memcpy(regs + 0x128, &w, 4);
    h = 0x1234u;     memcpy(regs + 0x12c, &h, 2);
    h = 0xbeefu;     memcpy(regs + 0x12e, &h, 2);   /* other half: not used */
    w = 0xcafe2a0fu; memcpy(regs + 0x138, &w, 4);   /* qp bytes 0x0f / 0x2a */
    openimp_t31_eval_from_status(&ev, regs, 31u);
    assert(ev.num_intra == 0x11111111u && ev.num_skip == 0x22222222u &&
           ev.num_cu8x8 == 0x33333333u && ev.num_cu16x16 == 0x44444444u &&
           ev.num_cu32x32 == 0x55555555u);
    assert(ev.num_cu64x64 == 0x1234u);          /* 16 bit, zero extended */
    assert(ev.slice_qp == 31u);                 /* residue in the OEM */
    assert(ev.min_qp == 0x0fu && ev.max_qp == 0x2au);
    for (i = 0; i < sizeof(ev.pad); i++)
        assert(ev.pad[i] == 0);
    /* nothing but those fields: the record is cleared first */
    {
        T31EvalInfo again;

        memset(&again, 0xff, sizeof(again));
        openimp_t31_eval_from_status(&again, regs, 31u);
        assert(!memcmp(&again, &ev, sizeof(ev)));
    }
    puts("T31 eval info record ok");
    return 0;
}
