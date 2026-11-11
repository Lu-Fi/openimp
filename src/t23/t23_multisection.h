/*
 * T23 IMP_Encoder_SetMultiSectionMode: how the OEM libimp 1.3.0 hands a
 * coded H.264 picture to the application in sections, as pure C (host
 * test: tests/t23/multisection_test.c).
 *
 * OEM (reverse-engineered reference only):
 *  - SetMultiSectionMode(mode, size, cnt) stores size << 10 in the module
 *    word that IMP_Encoder_SetPoolSize also writes (the encoder bitstream
 *    pool, EncoderInit "vpuBs"), a divisor that is always 1 (cnt is only
 *    range-checked) and the mode (1, 2; larger values become 1).
 *  - CreateChn of an H.264 channel with a mode > 0 enables the
 *    bitstream-full stop of the encoder (i264e handle +0x3741) with a
 *    window of (pool word / 1) >> 10 KiB (+0x3744) and gives the mode to
 *    i264e (parameter +0x1e8).
 *  - i264e_slice_write then waits for the soc_vpu bitstream-full interrupt
 *    (ioctl 0xc058630b, result 4 = window full, 3 = picture done).  Each
 *    full window is one section of window bytes; the last one is the rest.
 *    A picture that fits in one window is handed out as usual.
 *    mode 1: the sections are copied back into the picture's bitstream
 *            and the hardware is restarted at once (ioctl 0xc058630c); the
 *            application still gets whole pictures.
 *    mode 2: every section is handed out on its own
 *            (i264e_slice_write_segment): the first one carries the NAL
 *            units written before the slice data (SPS, PPS, SEI) and the
 *            start of the slice NAL unit, every further one is a single
 *            pack with the next bytes of that NAL unit.  The stream thread
 *            (update_h264_one_frmstrm) re-fills the picture's one stream
 *            record per section: same seq and timestamp, the last pack's
 *            frameEnd = 1 (when there is more than one), and pack[0]'s
 *            frameEnd = "picture complete", so it is 0 on every section
 *            but the last.  IMP_Encoder_ReleaseStream of a stream whose
 *            pack[0].frameEnd is 0 only lets the hardware go on
 *            (i264e_release_bitstream_bsfull) and keeps the record.
 *
 * OpenIMP codes the whole picture first (the Helix window covers the
 * picture) and then hands it out in the same sections: GetStream /
 * ReleaseStream behave as above, only the first section is not available
 * before the picture is complete.  Section bytes are counted from the
 * slice NAL unit's first byte after its header byte.
 */
#ifndef OPENIMP_T23_MULTISECTION_H
#define OPENIMP_T23_MULTISECTION_H

#include <stdint.h>

/* OEM: mode 1/2 as given, larger as 1; 0 = off. */
static inline int t23_ms_mode(int mode)
{
    return mode <= 0 ? 0 : (mode < 3 ? mode : 1);
}

/* OEM CreateChn: the window in bytes from the pool word ((word / 1) >> 10
 * KiB); 0 when the mode does not split the stream the application sees. */
static inline uint32_t t23_ms_section_bytes(int mode, int32_t pool_word)
{
    if (mode != 2 || pool_word < 1024)
        return 0;
    return ((uint32_t)pool_word >> 10) << 10;
}

/* Offset of the first H.264 slice NAL unit (type 1 or 5, 4-byte start
 * code) in an Annex B access unit, or length when there is none. */
static inline uint32_t t23_ms_slice_offset(const uint8_t *data,
                                           uint32_t length)
{
    uint32_t i;

    for (i = 0; i + 4u < length; i++) {
        if (data[i] || data[i + 1u] || data[i + 2u] || data[i + 3u] != 1u)
            continue;
        if ((data[i + 4u] & 0x1fu) == 1u || (data[i + 4u] & 0x1fu) == 5u)
            return i;
        i += 3u;
    }
    return length;
}

/* Number of sections of an access unit (1 = handed out whole). */
static inline uint32_t t23_ms_sections(const uint8_t *data, uint32_t length,
                                       uint32_t section)
{
    uint32_t slice = t23_ms_slice_offset(data, length);
    uint32_t origin = slice + 5u;

    if (!section || slice >= length || origin >= length ||
        length - origin <= section)
        return 1;
    return (length - origin + section - 1u) / section;
}

/* Byte range [*begin, *end) of section index (0 based, < sections). */
static inline void t23_ms_range(const uint8_t *data, uint32_t length,
                                uint32_t section, uint32_t index,
                                uint32_t *begin, uint32_t *end)
{
    uint32_t count = t23_ms_sections(data, length, section);
    uint32_t origin = t23_ms_slice_offset(data, length) + 5u;
    uint64_t stop;

    if (count <= 1u) {
        *begin = 0;
        *end = length;
        return;
    }
    if (index >= count)
        index = count - 1u;
    *begin = index ? origin + index * section : 0u;
    stop = (uint64_t)origin + (uint64_t)(index + 1u) * section;
    *end = index + 1u == count || stop > length ? length : (uint32_t)stop;
}

#endif
