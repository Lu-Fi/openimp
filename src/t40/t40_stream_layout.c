#include "t40_stream_layout.h"

#include <stddef.h>
#include <string.h>

int openimp_t40_stream_layout(uint32_t capacity, uint32_t payload_offset,
                              uint32_t header_size, uint32_t payload_status,
                              OpenIMPT40StreamLayout *layout)
{
    uint32_t payload_size =
        payload_status & OPENIMP_T40_PAYLOAD_SIZE_MASK;

    if (!layout || payload_size == 0u || payload_offset > capacity ||
        header_size > payload_offset ||
        payload_size > capacity - payload_offset)
        return -1;

    layout->payload_size = payload_size;
    layout->payload_end = payload_offset + payload_size;
    layout->access_unit_size = header_size + payload_size;
    return 0;
}

uint32_t openimp_t40_user_sei_build(int hevc, const uint8_t uuid[16],
                                    const uint8_t *data, uint32_t size,
                                    uint8_t *out, uint32_t capacity)
{
    uint8_t rbsp[2 + 16 + OPENIMP_T40_SEI_USER_MAX + 1];
    uint32_t rbsp_len = 0, pos = 0, i;
    unsigned int zeros = 0;

    if (!uuid || !out || (size && !data) || size > OPENIMP_T40_SEI_USER_MAX)
        return 0;
    rbsp[rbsp_len++] = 5;                       /* user_data_unregistered */
    rbsp[rbsp_len++] = (uint8_t)(16u + size);   /* < 255: no extension */
    memcpy(rbsp + rbsp_len, uuid, 16);
    rbsp_len += 16;
    if (size)
        memcpy(rbsp + rbsp_len, data, size);
    rbsp_len += size;
    rbsp[rbsp_len++] = 0x80;                    /* rbsp_trailing_bits */

    if (capacity < 4u + 2u)
        return 0;
    out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
    if (hevc) {
        out[pos++] = 0x4e;                      /* PREFIX_SEI, layer 0 */
        out[pos++] = 0x01;                      /* temporal id plus one */
    } else {
        out[pos++] = 0x06;                      /* nal_ref_idc 0, SEI */
    }
    for (i = 0; i < rbsp_len; i++) {
        uint8_t b = rbsp[i];

        if (zeros >= 2u && b <= 3u) {
            if (pos >= capacity)
                return 0;
            out[pos++] = 3;
            zeros = 0;
        }
        if (pos >= capacity)
            return 0;
        out[pos++] = b;
        zeros = b == 0 ? zeros + 1u : 0u;
    }
    return pos;
}

uint32_t openimp_t40_sei_insert(uint8_t *buf, uint32_t capacity, uint32_t pos,
                                const uint8_t *sei, uint32_t sei_len)
{
    uint32_t at;
    int found = 0;

    if (!buf || !sei || sei_len == 0u || pos < 4u || pos > capacity ||
        sei_len > capacity - pos)
        return 0;
    for (at = pos - 2u; at-- > 0u;) {
        if (buf[at] == 0 && buf[at + 1] == 0 && buf[at + 2] == 1) {
            found = 1;
            break;
        }
    }
    if (!found)
        return 0;
    if (at > 0u && buf[at - 1] == 0)            /* 4-byte start code */
        at--;
    memmove(buf + at + sei_len, buf + at, pos - at);
    memcpy(buf + at, sei, sei_len);
    return pos + sei_len;
}
