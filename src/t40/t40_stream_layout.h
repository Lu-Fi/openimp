#ifndef OPENIMP_T40_STREAM_LAYOUT_H
#define OPENIMP_T40_STREAM_LAYOUT_H

#include <stdint.h>

#define OPENIMP_T40_PAYLOAD_SIZE_MASK 0x3fffffffu

typedef struct {
    uint32_t payload_size;
    uint32_t payload_end;
    uint32_t access_unit_size;
} OpenIMPT40StreamLayout;

/*
 * Validate T40's entropy-byte count from AVPU status register 0x8304 and
 * derive the DMA extent and compacted Annex-B access-unit size.
 */
int openimp_t40_stream_layout(uint32_t capacity, uint32_t payload_offset,
                              uint32_t header_size, uint32_t payload_status,
                              OpenIMPT40StreamLayout *layout);

/*
 * IMP_Encoder_SetChnSeiAttr (vendor T40 1.3.1): a user_data_unregistered SEI
 * (payload type 5: 16-byte uuid + user data, at most 128 bytes) goes in front
 * of the slice NAL, behind SPS/PPS on an IDR picture.  _build writes the
 * complete Annex-B NAL with emulation prevention (AVC type 6, HEVC prefix SEI
 * type 39) and returns its length, 0 on bad arguments or a too small buffer.
 * _insert puts it before the last start code of the header region
 * buf[0..pos) (the slice NAL; 00 00 01 cannot occur inside an escaped NAL)
 * and returns the new header length, 0 when there is no start code or the
 * result would not fit capacity.
 */
#define OPENIMP_T40_SEI_USER_MAX 128u
#define OPENIMP_T40_SEI_MAX_NAL  192u
uint32_t openimp_t40_user_sei_build(int hevc, const uint8_t uuid[16],
                                    const uint8_t *data, uint32_t size,
                                    uint8_t *out, uint32_t capacity);
uint32_t openimp_t40_sei_insert(uint8_t *buf, uint32_t capacity, uint32_t pos,
                                const uint8_t *sei, uint32_t sei_len);

#endif
