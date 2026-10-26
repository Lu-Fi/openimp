/*
 * MXU2 (XBurst1 128-bit SIMD) in userspace: op layer and runtime selection.
 *
 * Which parts run MXU2 in a user process (device-checked 2026-10-26):
 *   T31X (.21), T20X (.107), T10L (.31): yes.
 *   T31L (.26): no - the kernel answers the first MXU2 instruction of a
 *   task with SIGILL on this part. T21: no (MXU2 opcodes reserved). T23:
 *   no MXU. Our Buildroot gcc has no MXU2 intrinsics; binutils 2.45 has
 *   the mnemonics (".set mxu2"), so the kernels are inline asm.
 *
 * Every SIMD kernel keeps a plain C reference and is only used after
 * openimp_mxu2_select() has run its bit-exact selftest against that
 * reference on the running CPU, with SIGILL caught. OPENIMP_MXU=0 in the
 * environment keeps the C code.
 *
 * The op layer (V* macros) is inline asm on a MIPS build and a lane model
 * of the same instruction on the host, so the host tests run the very same
 * kernel code; only the encodings are left to the device selftest.
 */
#ifndef OPENIMP_MXU2_H
#define OPENIMP_MXU2_H

#include <stdint.h>

#if defined(__mips__) && !defined(OPENIMP_MXU2_MODEL)

#define OPENIMP_MXU2_ASM 1
#define OPENIMP_V_PRE  ".set push\n\t.set mxu2\n\t"
#define OPENIMP_V_POST "\n\t.set pop"

/* lu1q/su1q: 16 bytes, any alignment (device-checked) */
#define VLOAD(d, p)  __asm__ volatile(OPENIMP_V_PRE "lu1q $vr%0, 0(%1)" \
                     OPENIMP_V_POST :: "i"(d), "r"(p) : "memory")
#define VSTORE(s, p) __asm__ volatile(OPENIMP_V_PRE "su1q $vr%0, 0(%1)" \
                     OPENIMP_V_POST :: "i"(s), "r"(p) : "memory")
/* SHUFV vd, vs, vt, vr: op[2i] = vt[i], op[2i+1] = vs[i];
 * vd[k] = vr[k] & 0x80 ? 0 : op[vr[k] & 31] */
#define VSHUF(d, s, t, r) __asm__ volatile(OPENIMP_V_PRE \
                     "shufv $vr%0, $vr%1, $vr%2, $vr%3" OPENIMP_V_POST \
                     :: "i"(d), "i"(s), "i"(t), "i"(r))
#define VXOR(d, a, b) __asm__ volatile(OPENIMP_V_PRE \
                     "xorv $vr%0, $vr%1, $vr%2" OPENIMP_V_POST \
                     :: "i"(d), "i"(a), "i"(b))

#else /* host model: 32 registers x 16 bytes */

#define OPENIMP_MXU2_ASM 0
struct openimp_vreg { uint8_t b[16]; };
extern struct openimp_vreg openimp_vr[32];
/* host tests: corrupt one SHUFV result byte when set (selftest check) */
extern int openimp_mxu2_model_fault;

#define VLOAD(d, p)  do { int l_; for (l_ = 0; l_ < 16; l_++) \
        openimp_vr[d].b[l_] = ((const uint8_t *)(p))[l_]; } while (0)
#define VSTORE(s, p) do { int l_; for (l_ = 0; l_ < 16; l_++) \
        ((uint8_t *)(p))[l_] = openimp_vr[s].b[l_]; } while (0)
#define VSHUF(d, s, t, r) do { \
        struct openimp_vreg S_ = openimp_vr[s], T_ = openimp_vr[t]; \
        struct openimp_vreg R_ = openimp_vr[r]; int l_; \
        for (l_ = 0; l_ < 16; l_++) { \
            unsigned k_ = R_.b[l_]; \
            openimp_vr[d].b[l_] = (k_ & 0x80u) ? 0 : \
                ((k_ & 1u) ? S_.b[(k_ & 31u) >> 1] : T_.b[(k_ & 31u) >> 1]); \
        } \
        if (openimp_mxu2_model_fault) openimp_vr[d].b[3] ^= 1; \
    } while (0)
#define VXOR(d, a, b) do { int l_; for (l_ = 0; l_ < 16; l_++) \
        openimp_vr[d].b[l_] = openimp_vr[a].b[l_] ^ openimp_vr[b].b[l_]; } while (0)
#endif

/*
 * Runs selftest() (returns 1 when the SIMD kernel matched the C reference
 * bit for bit) with SIGILL caught. Returns 1 when the SIMD kernel may be
 * used, and logs the decision (stderr, like the other FrameSource
 * notices). The caller caches the result (one call per kernel and
 * process). Calls are serialised; selftest must not block.
 */
int openimp_mxu2_select(const char *name, int (*selftest)(void));

/* 0 when OPENIMP_MXU=0 (or "off") is set in the environment */
int openimp_mxu_env_allowed(void);

#endif
