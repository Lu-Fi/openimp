/*
 * IMPISPAEAttr (18 words) <-> the 0x98-byte AE attribute block of the T31
 * tuning control 0x8000035.
 *
 * The T31 kernel exchanges tisp_ae_ctrls[0..37] (152 bytes) with the caller;
 * the vendor IMPISPAEAttr is 72 bytes.  The OEM T31 1.1.6 libimp
 * (IMP_ISP_Tuning_SetAeAttr 0x97558, IMP_ISP_Tuning_GetAeAttr 0x9775c) goes
 * through a 152-byte local block and moves 18 words between the two, in the
 * order of IMPISPAEAttr:
 *
 *   AeFreezenEn, AeItManualEn, AeIt, AeAGainManualEn, AeAGain,
 *   AeDGainManualEn, AeDGain, AeIspDGainManualEn, AeIspDGain, then the same
 *   nine for the short WDR frame.
 *
 * Passing the caller's pointer through, as OpenIMP did, let a GetAeAttr
 * write 80 bytes past the caller's struct and a SetAeAttr read 80 bytes past
 * it into the AE control words.
 *
 * The OEM leaves the unmapped words of its local block as stack contents;
 * they are zeroed here (the kernel only reads the mapped words).
 */
#ifndef ISP_AE_ATTR_H
#define ISP_AE_ATTR_H

#include <stdint.h>
#include <string.h>

#define ISP_AE_ATTR_WORDS 18
#define ISP_AE_KERNEL_WORDS 38

/* user word i lives at kernel word isp_ae_attr_kidx(i) (from the OEM
 * disassembly: sp+48 is kernel word 0) */
static inline int isp_ae_attr_kidx(int i)
{
    static const uint8_t map[ISP_AE_ATTR_WORDS] = {
        0, 13, 3, 14, 1, 36, 2, 15, 4,
        24, 16, 19, 17, 18, 37, 25, 26, 27
    };

    return map[i];
}

static inline void isp_ae_attr_to_kernel(uint32_t *kernel, const uint32_t *user)
{
    int i;

    memset(kernel, 0, ISP_AE_KERNEL_WORDS * sizeof(uint32_t));
    for (i = 0; i < ISP_AE_ATTR_WORDS; i++)
        kernel[isp_ae_attr_kidx(i)] = user[i];
}

static inline void isp_ae_attr_from_kernel(uint32_t *user, const uint32_t *kernel)
{
    int i;

    for (i = 0; i < ISP_AE_ATTR_WORDS; i++)
        user[i] = kernel[isp_ae_attr_kidx(i)];
}

#endif
