/* T31 IMPISPAEAttr (72 bytes) <-> driver AE control block (0x98 bytes).
 * From the stock T31 1.1.6 IMP_ISP_Tuning_Get/SetAeAttr: public word i is
 * block word t31_ae_attr_map[i]. */
#ifndef OPENIMP_T31_AE_ATTR_MAP_H
#define OPENIMP_T31_AE_ATTR_MAP_H
#include <stdint.h>

#define T31_AE_ATTR_WORDS 18
#define T31_AE_BLOCK_WORDS (0x98 / 4)

static const uint8_t t31_ae_attr_map[T31_AE_ATTR_WORDS] = {
    0, 13, 3, 14, 1, 36, 2, 15, 4, 24, 16, 19, 17, 18, 37, 25, 26, 27,
};

#endif
