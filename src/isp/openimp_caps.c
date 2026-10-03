/*
 * IMP_ISP_QueryCaps - which ISP tuning setters the open driver really acts on
 * (OpenIMP extension, see include/openimp/openimp_caps.h).
 *
 * The tables are per build platform and come from what open-tx-isp
 * (claude/open-tx-isp-all-14) implements plus the 2026-10-03 device tests
 * (openimp-docs FEATURE_MATRIX.md). Rules:
 *  - a bit goes into .known only when we can say yes or no for this build;
 *  - .applied means the hardware acts on the setter (not just "returns 0",
 *    not "stored but ignored", not "-EINVAL from the driver").
 * A bit that moves must also move in docs/OPENIMP_BEYOND_VENDOR.md
 * ("Capability query") in openimp-docs.
 */
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openimp/openimp_caps.h"

#define B(n) IMP_ISP_CAP_BIT(IMP_ISP_CAP_##n)

#define CAPS_ALL_V1 ((1ULL << IMP_ISP_CAP_COUNT_V1) - 1ULL)
#define CAPS_BCSH (B(BRIGHTNESS) | B(CONTRAST) | B(SATURATION) | B(SHARPNESS))
#define CAPS_FLIP (B(HFLIP) | B(VFLIP))

/* The T23 driver runs either the AE lifted from the stock module
 * (source_ae_oem=1, default) or the older HLIL substitute AE. Only the lifted
 * one honours AE compensation, backlight and highlight. */
#define CAPS_T23_LIFTED_AE (B(AE_COMP) | B(BACKLIGHT) | B(HILIGHT))

#if defined(PLATFORM_T23)
/* 1 = parameter reads N/0, 0 = Y/1 or unreadable (driver default is 1). */
static int caps_module_param_off(const char *path)
{
    char c = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    if (read(fd, &c, 1) != 1)
        c = 0;
    close(fd);
    return c == 'N' || c == 'n' || c == '0';
}
#endif

#if defined(PLATFORM_T20)
/* The shared T10/T20 libimp runs on either; the loaded ISP module says which.
 * access() only, no ISP or /dev/mem access. */
#if defined(OPENIMP_CAPS_TEST)
const char *openimp_caps_t10_module =
#else
static const char *const openimp_caps_t10_module =
#endif
    "/sys/module/tx_isp_t10";
#endif

#if defined(PLATFORM_T23)
#if defined(OPENIMP_CAPS_TEST)
/* host test (tests/caps): points this at a temporary file */
const char *openimp_caps_t23_ae_param =
#else
static const char *const openimp_caps_t23_ae_param =
#endif
    "/sys/module/tx_isp_t23/parameters/source_ae_oem";
#endif

static void caps_table(uint64_t *known, uint64_t *applied)
{
#if defined(PLATFORM_T20)
    /* One libimp for T10 and T20 (SoC picked at run time), both drivers built
     * from the Ingenic Apical tuning core. Its control switch has no case for
     * the hue, DPC-ratio, defog-strength or backlight CIDs (-EINVAL). DRC: the
     * T20 driver takes DRC_ATTR.strength, the T10 one has no DRC handler and
     * neither is device-verified, so no statement. Sinter/temper act through
     * the open driver's table scale (claude/t10-t20-nr-wdr); AE IT max via
     * the legacy SetIntegrationTime range mode (device-tested 2026-10-03).
     * Scene/colorfx: the T20 driver's apical tuning handles both V4L2
     * controls; the T10 driver (open-tx-isp driver/t10) has no handler. */
    *known = CAPS_ALL_V1 & ~B(DRC);
    *applied = CAPS_BCSH | CAPS_FLIP | B(RUNNING_MODE) | B(ANTIFLICKER) |
               B(AE_COMP) | B(MAX_AGAIN) | B(MAX_DGAIN) | B(SINTER) |
               B(TEMPER) | B(HILIGHT) | B(COLORFX) | B(SCENE) | B(WB) |
               B(AE_IT_MAX);
    if (access(openimp_caps_t10_module, F_OK) == 0)
        *applied &= ~(B(COLORFX) | B(SCENE));
#elif defined(PLATFORM_T21)
    /* Driver lifted from the vendor module. The legacy integration-time range
     * (SetIntegrationTime mode 2) is accepted but the AE ignores it (device
     * test 2026-10-03: max IT unchanged). Hue, AE comp, DPC, defog and
     * backlight: no setter in the T21 SDK, not device-tested -> no statement. */
    *known = CAPS_BCSH | CAPS_FLIP | B(RUNNING_MODE) | B(ANTIFLICKER) |
             B(MAX_AGAIN) | B(MAX_DGAIN) | B(SINTER) | B(TEMPER) | B(DRC) |
             B(HILIGHT) | B(COLORFX) | B(SCENE) | B(WB) | B(AE_IT_MAX);
    *applied = *known & ~B(AE_IT_MAX);
#elif defined(PLATFORM_T23)
    *known = CAPS_ALL_V1;
    *applied = CAPS_ALL_V1;
    if (caps_module_param_off(openimp_caps_t23_ae_param))
        *applied &= ~CAPS_T23_LIFTED_AE;
#elif defined(PLATFORM_T31)
    *known = CAPS_ALL_V1;
    *applied = CAPS_ALL_V1;
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /* Reworked tuning API: only BCSH, hue, flip, running mode and
     * anti-flicker have setters; everything else does not exist. */
    *known = CAPS_ALL_V1;
    *applied = CAPS_BCSH | B(HUE) | CAPS_FLIP | B(RUNNING_MODE) |
               B(ANTIFLICKER);
#if defined(PLATFORM_T41)
    /* The T41 tuning dispatcher has no HVFLIP route: the call returns but
     * the picture does not flip (cam-F, 2026-10-03). */
    *applied &= ~CAPS_FLIP;
#endif
#else
    /* T30 and unknown builds: no statement */
    *known = 0;
    *applied = 0;
#endif
}

int IMP_ISP_QueryCaps(IMPISPCaps *caps)
{
    IMPISPCaps out;

    if (caps == NULL || caps->size < sizeof(IMPISPCaps))
        return -1;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    out.version = IMP_ISP_CAPS_VERSION;
    caps_table(&out.known, &out.applied);
    {
        /* Debug-only: OPENIMP_CAPS_DROP=<hex mask> marks those bits known and
         * not applied (restrict only), to test a streamer's handling of
         * restricted keys on any SoC. */
        const char *drop = getenv("OPENIMP_CAPS_DROP");
        if (drop && *drop) {
            uint64_t m = strtoull(drop, NULL, 16) &
                         ((1ULL << IMP_ISP_CAP_COUNT_V1) - 1ULL);
            out.known |= m;
            out.applied &= ~m;
        }
    }
    out.applied &= out.known;
    /* a newer caller may pass a bigger struct: fill what we know, leave the
     * rest zero (callers zero it; .version says what was filled) */
    memcpy(caps, &out, sizeof(out));
    return 0;
}
