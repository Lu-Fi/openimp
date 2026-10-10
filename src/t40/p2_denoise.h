#ifndef OPENIMP_P2_DENOISE_H
#define OPENIMP_P2_DENOISE_H

#include <stdbool.h>

/* Vendor T10/T20/T21 libimp (IMP_Encoder_SetChnDenoise -> i264e_set_param 8 ->
 * i264e_reconfig_dn_set): the CreateChn enable switch stays as created; with
 * it clear dnType is forced to 0; dnType >= 3 is refused (-1, nothing
 * stored).  Only type/IQp/PQp are kept; Get returns those three and leaves
 * .enable of the caller's struct untouched.
 *
 * hw_active: whether the vendor encoder really runs the denoise pre-pass.
 * Not on T21: libimp 1.0.33 (channel_i264e_encoder_init) picks the encoder
 * mode from get_cpu_id(); every T21 id (11..14) gets mode 4, and
 * i264e_validate_parameters clears the denoise enable and type for every
 * mode but 1.  Measured on a T21 (jxf23, cpu id 12): created with enable 1 and
 * dnType 1, Get reads dnType 0, IQp/PQp are kept, one Helix job per picture.
 * With hw_active false the type is stored as 0 (after the -1 check). */
typedef struct {
    int type;
    int iqp;
    int pqp;
} P2DenoiseCache;

static inline int p2_denoise_apply(P2DenoiseCache *cache, bool created_enable,
                                   bool hw_active, int type, int iqp, int pqp)
{
    if (!created_enable)
        type = 0;
    if (type < 0 || type >= 3)
        return -1;
    cache->type = hw_active ? type : 0;
    cache->iqp = iqp;
    cache->pqp = pqp;
    return 0;
}

#endif
