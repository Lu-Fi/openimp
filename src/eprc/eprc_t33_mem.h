/*
 * Memory model of the T33 eprc controller (#295, planned).
 *
 * The T33 controller keeps its state in blocks that point at each other
 * (the controller block inside the i264e parameter block, a calloc'ed state
 * block, the encoder's slice block).  OpenIMP keeps that layout, byte for
 * byte, in an arena of 32-bit addresses, so that the state can be compared
 * with the vendor controller run under an emulator (tools/t32/eprc33): a
 * "pointer" is a 32-bit arena address and every access goes through the
 * accessors below.  Up to three regions are mapped; an access outside them
 * reads zero and is counted (E33.fault), it never touches host memory.
 */
#ifndef OPENIMP_EPRC_T33_MEM_H
#define OPENIMP_EPRC_T33_MEM_H

#include <stdint.h>
#include <string.h>

typedef struct E33 {
    uint8_t *r[3];          /* host memory of the regions */
    uint32_t rb[3];         /* arena address of each region */
    uint32_t rs[3];         /* region sizes */
    uint32_t frame;         /* scratch area (arena address) */
    uint32_t calloc_addr;   /* where the state block goes */
    int fault;              /* accesses outside the regions */
} E33;

static inline uint8_t *e33_at(E33 *X, uint32_t a, uint32_t n)
{
    static uint8_t sink[8];

    for (int i = 0; i < 3; i++)
        if (a - X->rb[i] < X->rs[i] && X->rs[i] - (a - X->rb[i]) >= n)
            return X->r[i] + (a - X->rb[i]);
    X->fault++;
    memset(sink, 0, sizeof(sink));
    return sink;
}

#define E33_ACC(N, T)                                                                   \
    static inline T e33_ld_##N(E33 *X, uint32_t a)                                     \
    {                                                                                   \
        T v;                                                                            \
        memcpy(&v, e33_at(X, a, sizeof(T)), sizeof(T));                                \
        return v;                                                                       \
    }                                                                                   \
    static inline void e33_st_##N(E33 *X, uint32_t a, T v) { memcpy(e33_at(X, a, sizeof(T)), &v, sizeof(T)); }
E33_ACC(U8, uint8_t)
E33_ACC(S8, int8_t)
E33_ACC(U16, uint16_t)
E33_ACC(S16, int16_t)
E33_ACC(U32, uint32_t)
E33_ACC(S32, int32_t)
E33_ACC(U64, uint64_t)
E33_ACC(S64, int64_t)
E33_ACC(F32, float)
E33_ACC(F64, double)
#undef E33_ACC

/* memset over arena addresses (one mapped region: host memset, else byte by
 * byte, so that unmapped bytes count as faults like single stores) */
static inline uint32_t e33_fill(E33 *X, uint32_t a, uint8_t v, uint32_t n)
{
    for (int i = 0; i < 3; i++)
        if (a - X->rb[i] < X->rs[i] && X->rs[i] - (a - X->rb[i]) >= n) {
            memset(X->r[i] + (a - X->rb[i]), v, n);
            return a;
        }
    for (uint32_t i = 0; i < n; i++)
        e33_st_U8(X, a + i, v);
    return a;
}

/* the controller's one allocation (the state block): placed by the caller
 * at X->calloc_addr, zeroed like calloc */
static inline uint32_t e33_calloc(E33 *X, uint32_t n, uint32_t m)
{
    e33_fill(X, X->calloc_addr, 0, n * m);
    return X->calloc_addr;
}

/* accessors with the context X implied */
#define LD_U8(a)  e33_ld_U8(X, (uint32_t)(a))
#define LD_S8(a)  e33_ld_S8(X, (uint32_t)(a))
#define LD_U16(a) e33_ld_U16(X, (uint32_t)(a))
#define LD_S16(a) e33_ld_S16(X, (uint32_t)(a))
#define LD_U32(a) e33_ld_U32(X, (uint32_t)(a))
#define LD_S32(a) e33_ld_S32(X, (uint32_t)(a))
#define LD_U64(a) e33_ld_U64(X, (uint32_t)(a))
#define LD_S64(a) e33_ld_S64(X, (uint32_t)(a))
#define LD_F32(a) e33_ld_F32(X, (uint32_t)(a))
#define LD_F64(a) e33_ld_F64(X, (uint32_t)(a))
#define ST_U8(a, v)  e33_st_U8(X, (uint32_t)(a), (uint8_t)(v))
#define ST_S8(a, v)  e33_st_S8(X, (uint32_t)(a), (int8_t)(v))
#define ST_U16(a, v) e33_st_U16(X, (uint32_t)(a), (uint16_t)(v))
#define ST_S16(a, v) e33_st_S16(X, (uint32_t)(a), (int16_t)(v))
#define ST_U32(a, v) e33_st_U32(X, (uint32_t)(a), (uint32_t)(v))
#define ST_S32(a, v) e33_st_S32(X, (uint32_t)(a), (int32_t)(v))
#define ST_U64(a, v) e33_st_U64(X, (uint32_t)(a), (uint64_t)(v))
#define ST_S64(a, v) e33_st_S64(X, (uint32_t)(a), (int64_t)(v))
#define ST_F32(a, v) e33_st_F32(X, (uint32_t)(a), (float)(v))
#define ST_F64(a, v) e33_st_F64(X, (uint32_t)(a), (double)(v))

#endif
