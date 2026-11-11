/*
 * IMPEncoderProfile numbering (#302), compile-time only: T32 uses the 2.x
 * IMPPayloadType type byte (JPEG 0, AVC 1, HEVC 2); every other SoC keeps
 * AVC 0, HEVC 1, JPEG 4.  tests/t32/Makefile compiles it with -m32
 * -ffreestanding -fsyntax-only (the public headers assert the 32-bit MIPS
 * layouts) for PLATFORM_T32 and PLATFORM_T31.
 */
#include <imp/imp_encoder.h>

#if defined(PLATFORM_T32)
#define EXP_JPEG 0x00000000u
#define EXP_AVC  0x0100004du
#define EXP_HEVC 0x02000001u
#else
#define EXP_JPEG 0x04000000u
#define EXP_AVC  0x0000004du
#define EXP_HEVC 0x01000001u
#endif
_Static_assert((unsigned)IMP_ENC_PROFILE_JPEG == EXP_JPEG, "JPEG profile");
_Static_assert((unsigned)IMP_ENC_PROFILE_AVC_MAIN == EXP_AVC, "AVC main profile");
_Static_assert((unsigned)IMP_ENC_PROFILE_HEVC_MAIN == EXP_HEVC, "HEVC main profile");
