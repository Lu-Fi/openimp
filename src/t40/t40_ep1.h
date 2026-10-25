#ifndef OPENIMP_T40_EP1_H
#define OPENIMP_T40_EP1_H

#include <stddef.h>

int openimp_t40_init_ep1(void *ep1, size_t size, int use_fixqp_lda);
int openimp_t41_init_ep1(void *ep1, size_t size);
/* T40 FixQP: lambda words for the next IDR (is_idr) or P picture. */
int openimp_t40_update_fixqp_ep1(void *ep1, size_t size, int is_idr);
/* T31 HEVC: HEVC default lambda table, no scaling list. */
int openimp_t31_init_hevc_ep1(void *ep1, size_t size);

/* Rewrite only T41's 52 AVC lambda words for the next picture type.
 * AL_GetLambda indexes the four-component default table by slice type:
 * B=0, P=1 and I=2.  Component 3 remains the second hardware lane. */
int openimp_t41_update_ep1_lambda(void *ep1, size_t size,
                                  unsigned int picture_type);
/* The same T41 layout with the AVC (hevc = 0) or HEVC default table;
 * T41 AL_GetLambda selects HEVC_NEW_DEFAULT_LDA_TABLE for codec 1. */
/* T41 FixQP (vendor libimp 1.2.6 runs AL_GetLambda in its dynamic mode
 * when the channel is constant-QP): lanes = isqrt(L(qp) * w) with
 * L(qp) = round(16 * 2^(qp/3)); I pictures w = 51 / 90, P pictures 90 / 90
 * times clip((qp - 12) / 6, 2, 4) unless full_weight (IDR and every
 * uGopLength-th P since the IDR).  Returns 0, or -1 for a bad buffer. */
int openimp_t41_update_ep1_lambda_fixqp(void *ep1, size_t size, int intra,
                                        int full_weight);
int openimp_t41_update_ep1_lambda_codec(void *ep1, size_t size,
                                        unsigned int picture_type,
                                        unsigned int hevc);
/* T41 HEVC: HEVC lambda words for an I picture, no scaling list. */
int openimp_t41_init_hevc_ep1(void *ep1, size_t size);

#endif
