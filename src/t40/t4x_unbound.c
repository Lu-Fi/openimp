/* T40/T41 unbound-encoder helpers: the IMP_Encoder_Vbm* buffers and (T41)
 * IMP_Encoder_InputJpege.  The encoder itself (IMP_Encoder_Yuv*) and the
 * _Ex variants are in openimp_p2_encoder.c. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <imp/imp_encoder.h>

#include "dma_alloc.h"
#include "hw_encoder.h"
#include "imp_log_int.h"

/* Physically contiguous buffers from the OpenIMP rmem allocator (vendor:
 * IMP_Alloc + its own list; VbmV2P/P2V are IMP_Virt_to_Phys and
 * IMP_Phys_to_Virt). align above a page is refused, a page is what every
 * allocation gets. */
void *IMP_Encoder_VbmAlloc(uint32_t size, uint32_t align)
{
    IMPDMABufferInfo info;

    if (!size || size > INT32_MAX || align > 4096u ||
        (align & (align - 1u)))
        return NULL;
    memset(&info, 0, sizeof(info));
    if (DMA_AllocDescriptor(&info, (int)size, "vbm") != 0 ||
        !info.virt_addr || !info.phys_addr)
        return NULL;
    if (align && (info.virt_addr & (align - 1u))) {
        DMA_FreePhys(info.phys_addr);
        return NULL;
    }
    return (void *)(uintptr_t)info.virt_addr;
}

void IMP_Encoder_VbmFree(void *vaddr)
{
    uint32_t physical;

    if (!vaddr)
        return;
    physical = DMA_VirtToPhys(vaddr);
    if (physical && physical != (uint32_t)(uintptr_t)vaddr)
        DMA_FreePhys(physical);
}

intptr_t IMP_Encoder_VbmV2P(intptr_t vaddr)
{
    uint32_t physical;

    if (!vaddr)
        return 0;
    physical = DMA_VirtToPhys((const void *)vaddr);
    /* DMA_VirtToPhys echoes addresses it does not own */
    return physical == (uint32_t)vaddr ? 0 : (intptr_t)physical;
}

intptr_t IMP_Encoder_VbmP2V(intptr_t paddr)
{
    return paddr ? (intptr_t)DMA_PhysToVirt((uint32_t)paddr) : 0;
}

#if defined(PLATFORM_T41)
uint32_t OpenIMP_P2_IvpuBsSize(void);

/*
 * IMP_Encoder_InputJpege (vendor 0xafe4): NV12 with the chroma plane right
 * after src_w * src_h luma bytes, width a multiple of 32 and height of 8
 * (else -1), coded by the IVPU into dst; q is "not supported" there.
 * OpenIMP codes it with the software baseline JPEG encoder (hw_encoder.c)
 * and honours q 1..100 (other values: 75), beyond the vendor.
 */
int IMP_Encoder_InputJpege(uint8_t *src, uint8_t *dst, int src_w, int src_h,
                           int q, int *stream_length)
{
    HWFrameBuffer picture;
    HWStreamBuffer out;
    uint8_t *padded = NULL;
    size_t luma, chroma;
    int result;

    if (stream_length)
        *stream_length = 0;
    if (!src || !dst || !stream_length)
        return -1;
    if (src_w <= 0 || src_h <= 0 || (src_w & 0x1f) || (src_h & 0x7)) {
        IMP_LOG_ERR("Encoder", "InputJpege: %dx%d, width must be a multiple "
                    "of 32 and height of 8\n", src_w, src_h);
        return -1;
    }
    luma = (size_t)src_w * (size_t)src_h;
    chroma = luma / 2u;
    memset(&picture, 0, sizeof(picture));
    picture.width = (uint32_t)src_w;
    picture.height = (uint32_t)src_h;
    picture.pixfmt = 0x3231564eu;
    if (src_h % 16) {
        /* the encoder reads chroma after a 16-line luma plane */
        size_t padded_luma = (size_t)src_w * (((size_t)src_h + 15u) & ~15u);

        padded = malloc(padded_luma + chroma);
        if (!padded)
            return -1;
        memcpy(padded, src, luma);
        memset(padded + luma, 0x10, padded_luma - luma);
        memcpy(padded + padded_luma, src + luma, chroma);
        picture.virt_addr = (uint32_t)(uintptr_t)padded;
        picture.size = (uint32_t)(padded_luma + chroma);
    } else {
        picture.virt_addr = (uint32_t)(uintptr_t)src;
        picture.size = (uint32_t)(luma + chroma);
    }
    memset(&out, 0, sizeof(out));
    result = HW_Encoder_Encode_NV12_JPEG(&picture, &out,
                                         q >= 1 && q <= 100 ? (uint32_t)q : 75u);
    free(padded);
    if (result != 0 || !out.virt_addr || !out.length ||
        out.length > INT32_MAX ||
        (OpenIMP_P2_IvpuBsSize() &&
         out.length > OpenIMP_P2_IvpuBsSize())) {
        free((void *)(uintptr_t)out.virt_addr);
        return -1;
    }
    memcpy(dst, (const void *)(uintptr_t)out.virt_addr, out.length);
    *stream_length = (int)out.length;
    free((void *)(uintptr_t)out.virt_addr);
    return 0;
}
#endif

/* ---- IMP_Decoder ------------------------------------------------------- */
/*
 * IMP_Decoder_* (vendor T40 1.3.1 / T41 imp_decoder.h: "only support JPEG
 * decode for now", hardware JPEG decoder; in the export union of the vendor
 * T40/T41 builds, not in the T41 1.2.6 libimp.so).  OpenIMP has no JPEG
 * decoder on T40/T41 (T23 runs the OEM one in its Helix helper): every call
 * fails with -1 and errno ENOTSUP, so an application sees the missing
 * feature at CreateChn instead of failing to link.
 */
#include <errno.h>

static int t4x_decoder_unsupported(const char *function)
{
    IMP_LOG_LIMITED(LOG_ERR, "Decoder", "%s: no JPEG decoder in OpenIMP on "
                    "this SoC", function);
    errno = ENOTSUP;
    return -1;
}

int IMP_Decoder_CreateChn(int decChn, const void *attr);
int IMP_Decoder_CreateChn(int decChn, const void *attr)
{
    (void)decChn;
    (void)attr;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_DestroyChn(int decChn);
int IMP_Decoder_DestroyChn(int decChn)
{
    (void)decChn;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_StartRecvPic(int decChn);
int IMP_Decoder_StartRecvPic(int decChn)
{
    (void)decChn;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_StopRecvPic(int decChn);
int IMP_Decoder_StopRecvPic(int decChn)
{
    (void)decChn;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_SendStreamTimeout(int decChn, void *stream,
                                  uint32_t timeoutMsec);
int IMP_Decoder_SendStreamTimeout(int decChn, void *stream,
                                  uint32_t timeoutMsec)
{
    (void)decChn;
    (void)stream;
    (void)timeoutMsec;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_PollingFrame(int decChn, uint32_t timeoutMsec);
int IMP_Decoder_PollingFrame(int decChn, uint32_t timeoutMsec)
{
    (void)decChn;
    (void)timeoutMsec;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_GetFrame(int decChn, void **frame);
int IMP_Decoder_GetFrame(int decChn, void **frame)
{
    (void)decChn;
    if (frame)
        *frame = NULL;
    return t4x_decoder_unsupported(__func__);
}

int IMP_Decoder_ReleaseFrame(int decChn, void *frame);
int IMP_Decoder_ReleaseFrame(int decChn, void *frame)
{
    (void)decChn;
    (void)frame;
    return t4x_decoder_unsupported(__func__);
}
