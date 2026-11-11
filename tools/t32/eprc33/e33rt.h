/* arena runtime for the generated T33 eprc code (development) */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

typedef struct E33 {
    uint8_t *r[3];       /* regions */
    uint32_t rb[3], rs[3];
    uint32_t frame;      /* scratch for address-taken locals */
    uint32_t calloc_addr;
    int fault;
} E33;

static uint8_t e33_dummy[16];
static inline uint8_t *E33_P(E33 *X, uint32_t a)
{
    for (int i = 0; i < 3; i++)
        if (a - X->rb[i] < X->rs[i])
            return X->r[i] + (a - X->rb[i]);
    if (X->fault < 8) fprintf(stderr, "FAULT %x\n", a);
    X->fault++;
    memset(e33_dummy, 0, sizeof(e33_dummy));
    return e33_dummy;
}
#define DEFLD(N, T) static inline T ld_##N(E33 *X, uint32_t a) { T v; memcpy(&v, E33_P(X, a), sizeof(T)); return v; } \
    static inline void st_##N(E33 *X, uint32_t a, T v) { memcpy(E33_P(X, a), &v, sizeof(T)); }
DEFLD(U8, uint8_t) DEFLD(S8, int8_t) DEFLD(U16, uint16_t) DEFLD(S16, int16_t)
DEFLD(U32, uint32_t) DEFLD(S32, int32_t) DEFLD(U64, uint64_t) DEFLD(S64, int64_t)
DEFLD(F32, float) DEFLD(F64, double)
#define LD_U8(a) ld_U8(X, (a))
#define LD_S8(a) ld_S8(X, (a))
#define LD_U16(a) ld_U16(X, (a))
#define LD_S16(a) ld_S16(X, (a))
#define LD_U32(a) ld_U32(X, (a))
#define LD_S32(a) ld_S32(X, (a))
#define LD_U64(a) ld_U64(X, (a))
#define LD_S64(a) ld_S64(X, (a))
#define LD_F32(a) ld_F32(X, (a))
#define LD_F64(a) ld_F64(X, (a))
#include <stdio.h>
static uint32_t e33_watch;
#define WCH(a, v) do { if (e33_watch && (uint32_t)(a) - e33_watch < 4) fprintf(stderr, "W %s:%d %x <- %lld\n", __func__, __LINE__, (uint32_t)(a), (long long)(v)); } while (0)
#define ST_U8(a, v) do { WCH(a, v); st_U8(X, (a), (v)); } while (0)
#define ST_S8(a, v) do { WCH(a, v); st_S8(X, (a), (v)); } while (0)
#define ST_U16(a, v) do { WCH(a, v); st_U16(X, (a), (v)); } while (0)
#define ST_S16(a, v) do { WCH(a, v); st_S16(X, (a), (v)); } while (0)
#define ST_U32(a, v) do { WCH(a, v); st_U32(X, (a), (v)); } while (0)
#define ST_S32(a, v) do { WCH(a, v); st_S32(X, (a), (v)); } while (0)
#define ST_U64(a, v) do { WCH(a, v); st_U64(X, (a), (v)); } while (0)
#define ST_S64(a, v) do { WCH(a, v); st_S64(X, (a), (v)); } while (0)
#define ST_F32(a, v) do { WCH(a, v); st_F32(X, (a), (v)); } while (0)
#define ST_F64(a, v) do { WCH(a, v); st_F64(X, (a), (v)); } while (0)

#define CONCAT44(h, l) ((uint64_t)(uint32_t)(h) << 32 | (uint32_t)(l))
#define CONCAT11(h, l) ((uint32_t)(((uint8_t)(h) << 8) | (uint8_t)(l)))
#define ZEXT48(x) ((uint64_t)(uint32_t)(x))
#define OEM_TRAP() ((void)0)
#define OEM_IGNORE_assert() 0
#define OEM_IGNORE_exit() 0
#define OEM_IGNORE_fclose() 0
#define OEM_IGNORE_fopen() 0
#define OEM_IGNORE_fread() 0
#define OEM_IGNORE_free() 0
#define OEM_IGNORE_fseeko() 0
#define OEM_IGNORE_printf() 0
#define OEM_IGNORE_puts() 0

#define E33_NARG(...) E33_NARG_(__VA_ARGS__, 4, 3, 2, 1)
#define E33_NARG_(a, b, c, d, n, ...) n
#define E33_CAT(a, b) E33_CAT_(a, b)
#define E33_CAT_(a, b) a##b
static inline int64_t e33_div2(int64_t a, int64_t b) { return b ? a / b : 0; }
static inline int64_t e33_div4(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh)
{ return e33_div2((int64_t)CONCAT44(ah, al), (int64_t)CONCAT44(bh, bl)); }
static inline uint64_t e33_udiv2(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint64_t e33_udiv4(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh)
{ return e33_udiv2(CONCAT44(ah, al), CONCAT44(bh, bl)); }
#define OEM_divdi3(...) E33_CAT(e33_div, E33_NARG(__VA_ARGS__))(__VA_ARGS__)
#define OEM_udivdi3(...) E33_CAT(e33_udiv, E33_NARG(__VA_ARGS__))(__VA_ARGS__)
static inline float e33_fdisf1(int64_t v) { return (float)v; }
static inline float e33_fdisf2(uint32_t l, uint32_t h) { return (float)(int64_t)CONCAT44(h, l); }
static inline double e33_fdidf1(int64_t v) { return (double)v; }
static inline double e33_fdidf2(uint32_t l, uint32_t h) { return (double)(int64_t)CONCAT44(h, l); }
#define OEM_floatdisf(...) E33_CAT(e33_fdisf, E33_NARG(__VA_ARGS__))(__VA_ARGS__)
#define OEM_floatdidf(...) E33_CAT(e33_fdidf, E33_NARG(__VA_ARGS__))(__VA_ARGS__)
static inline int64_t OEM_fixsfdi(float f) { return (int64_t)f; }
static inline int64_t OEM_fixdfdi(double f) { return (int64_t)f; }
static inline uint32_t OEM_memset(E33 *X, uint32_t a, uint32_t v, uint32_t n)
{ for (uint32_t i = 0; i < n; i++) ST_U8(a + i, v); return a; }
static inline uint32_t OEM_calloc(E33 *X, uint32_t n, uint32_t m)
{ OEM_memset(X, X->calloc_addr, 0, n * m); return X->calloc_addr; }
static inline uint32_t OEM_malloc(E33 *X, uint32_t n) { (void)n; return X->calloc_addr; }
