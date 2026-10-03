/*
 * openimp_caps.h - IMP_ISP_QueryCaps(), an OpenIMP extension (no vendor
 * equivalent).
 *
 * The vendor SDK headers say which IMP_ISP_Tuning_Set* calls EXIST on a SoC;
 * they do not say whether the hardware acts on them. Several setters exist,
 * return 0 and change nothing (stored but ignored), others return -EINVAL from
 * the driver. OpenIMP ships the open driver too, so it knows: this call
 * reports, per tuning feature, whether a Set on THIS build/SoC really reaches
 * the hardware.
 *
 * ABI rules (stable, extensible):
 *  - the caller zeroes the struct and sets .size = sizeof(IMPISPCaps); the
 *    library fills at most .size bytes and never writes past it;
 *  - bit numbers below are fixed forever; new features only get new bits;
 *  - .known: the bits the library has an opinion about. A bit NOT in .known
 *    means "no statement" - keep whatever the caller assumed before;
 *  - .applied (subset of .known): the setter for that feature is acted on by
 *    the hardware. known && !applied = exists but has no effect / is rejected.
 *  - new fields may be appended after .applied; .size tells the library how
 *    much the caller has room for, .version tells the caller what was filled.
 *
 * A caller builds its capability list from the vendor headers and then
 * RESTRICTS it (known && !applied). It may also EXTEND it with an applied
 * feature the vendor header lacks - the open driver doing more than the
 * vendor - but only where it can call the setter safely: bind it weakly
 * under its own prototype (identical to the other SoCs' SDK signature),
 * check the symbol is non-NULL, and keep the vendor value range. A bit may
 * be applied while this SoC's vendor header has no setter for it.
 *
 * Cheap: a static per-SoC table plus, on some SoCs, one sysfs read of a
 * module parameter. Safe to call before IMP_System_Init. Returns 0 on success,
 * -1 if caps is NULL or .size is smaller than the version-1 struct.
 */
#ifndef OPENIMP_CAPS_H
#define OPENIMP_CAPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMP_ISP_CAPS_VERSION 1

enum {
    IMP_ISP_CAP_BRIGHTNESS   = 0,   /* SetBrightness */
    IMP_ISP_CAP_CONTRAST     = 1,   /* SetContrast */
    IMP_ISP_CAP_SATURATION   = 2,   /* SetSaturation */
    IMP_ISP_CAP_SHARPNESS    = 3,   /* SetSharpness */
    IMP_ISP_CAP_HUE          = 4,   /* SetBcshHue */
    IMP_ISP_CAP_HFLIP        = 5,   /* SetISPHflip / SetHVFLIP */
    IMP_ISP_CAP_VFLIP        = 6,   /* SetISPVflip / SetHVFLIP */
    IMP_ISP_CAP_RUNNING_MODE = 7,   /* SetISPRunningMode (day/night) */
    IMP_ISP_CAP_ANTIFLICKER  = 8,   /* SetAntiFlickerAttr */
    IMP_ISP_CAP_AE_COMP      = 9,   /* SetAeComp */
    IMP_ISP_CAP_MAX_AGAIN    = 10,  /* SetMaxAgain */
    IMP_ISP_CAP_MAX_DGAIN    = 11,  /* SetMaxDgain */
    IMP_ISP_CAP_SINTER       = 12,  /* SetSinterStrength */
    IMP_ISP_CAP_TEMPER       = 13,  /* SetTemperStrength */
    IMP_ISP_CAP_DPC          = 14,  /* SetDPC_Strength */
    IMP_ISP_CAP_DEFOG        = 15,  /* SetDefog_Strength */
    IMP_ISP_CAP_DRC          = 16,  /* SetDRC_Strength */
    IMP_ISP_CAP_HILIGHT      = 17,  /* SetHiLightDepress */
    IMP_ISP_CAP_BACKLIGHT    = 18,  /* SetBacklightComp */
    IMP_ISP_CAP_COLORFX      = 19,  /* SetColorfxMode */
    IMP_ISP_CAP_SCENE        = 20,  /* SetSceneMode */
    IMP_ISP_CAP_WB           = 21,  /* SetWB (mode + manual gains) */
    IMP_ISP_CAP_AE_IT_MAX    = 22,  /* SetAe_IT_MAX / SetIntegrationTime range */
    IMP_ISP_CAP_COUNT_V1     = 23
};

#define IMP_ISP_CAP_BIT(n) (1ULL << (n))

typedef struct {
    uint32_t size;      /* in: sizeof(IMPISPCaps) of the caller */
    uint32_t version;   /* out: IMP_ISP_CAPS_VERSION of the library */
    uint64_t known;     /* out: bits the library makes a statement about */
    uint64_t applied;   /* out: subset of known the hardware acts on */
} IMPISPCaps;

int IMP_ISP_QueryCaps(IMPISPCaps *caps);

#ifdef __cplusplus
}
#endif

#endif /* OPENIMP_CAPS_H */
