/* Host test for IMP_ISP_QueryCaps (src/isp/openimp_caps.c): ABI checks and
 * the per-SoC expectations documented in openimp-docs
 * docs/OPENIMP_BEYOND_VENDOR.md "Capability query". Built once per platform
 * (tests/caps/Makefile); argv[1] names the platform. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openimp/openimp_caps.h"

#define B(n) IMP_ISP_CAP_BIT(IMP_ISP_CAP_##n)
#define ALL ((1ULL << IMP_ISP_CAP_COUNT_V1) - 1ULL)

#if defined(PLATFORM_T23)
extern const char *openimp_caps_t23_ae_param;
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static IMPISPCaps query(void)
{
    IMPISPCaps c;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c);
    CHECK(IMP_ISP_QueryCaps(&c) == 0);
    CHECK(c.version == IMP_ISP_CAPS_VERSION);
    CHECK((c.applied & ~c.known) == 0);
    CHECK((c.known & ~ALL) == 0);
    return c;
}

int main(int argc, char **argv)
{
    const char *p = argc > 1 ? argv[1] : "?";
    IMPISPCaps c;

    /* ABI: fixed layout, fixed bit numbers */
    CHECK(sizeof(IMPISPCaps) == 24);
    CHECK(IMP_ISP_CAP_BRIGHTNESS == 0 && IMP_ISP_CAP_AE_IT_MAX == 22);
    /* bad arguments */
    CHECK(IMP_ISP_QueryCaps(NULL) == -1);
    memset(&c, 0, sizeof(c));
    c.size = 8;
    CHECK(IMP_ISP_QueryCaps(&c) == -1);
    /* a bigger (future) caller struct: only the v1 part is written */
    {
        unsigned char big[64];
        memset(big, 0xa5, sizeof(big));
        ((IMPISPCaps *)(void *)big)->size = sizeof(big);
        CHECK(IMP_ISP_QueryCaps((IMPISPCaps *)(void *)big) == 0);
        CHECK(big[sizeof(IMPISPCaps)] == 0xa5 && big[63] == 0xa5);
    }

    c = query();
#if defined(PLATFORM_T20)
    CHECK(!(c.known & B(DRC)));
    CHECK((c.known & B(HUE)) && !(c.applied & B(HUE)));
    CHECK(!(c.applied & (B(DPC) | B(DEFOG) | B(BACKLIGHT))));
    CHECK((c.applied & (B(AE_IT_MAX) | B(SINTER) | B(COLORFX) | B(WB))) ==
          (B(AE_IT_MAX) | B(SINTER) | B(COLORFX) | B(WB)));
#elif defined(PLATFORM_T21)
    CHECK((c.known & B(AE_IT_MAX)) && !(c.applied & B(AE_IT_MAX)));
    CHECK(c.applied & B(DRC));
    CHECK(!(c.known & (B(HUE) | B(AE_COMP) | B(DPC) | B(DEFOG) | B(BACKLIGHT))));
#elif defined(PLATFORM_T23)
    {
        char path[] = "/tmp/openimp-caps-XXXXXX";
        int fd = mkstemp(path);
        CHECK(fd >= 0);
        openimp_caps_t23_ae_param = "/nonexistent/source_ae_oem";
        c = query();                     /* unreadable -> driver default (lifted AE) */
        CHECK(c.applied == ALL);
        CHECK(write(fd, "N\n", 2) == 2);
        close(fd);
        openimp_caps_t23_ae_param = path;
        c = query();                     /* HLIL AE: no comp/backlight/highlight */
        CHECK(c.known == ALL);
        CHECK(c.applied == (ALL & ~(B(AE_COMP) | B(BACKLIGHT) | B(HILIGHT))));
        unlink(path);
    }
#elif defined(PLATFORM_T31)
    CHECK(c.known == ALL && c.applied == ALL);
#elif defined(PLATFORM_T40) || defined(PLATFORM_T41)
    CHECK(c.known == ALL);
    CHECK(!(c.applied & (B(WB) | B(AE_COMP) | B(DRC) | B(AE_IT_MAX))));
    CHECK(c.applied & B(HUE));
#if defined(PLATFORM_T41)
    CHECK(!(c.applied & (B(HFLIP) | B(VFLIP))));
#else
    CHECK((c.applied & (B(HFLIP) | B(VFLIP))) == (B(HFLIP) | B(VFLIP)));
#endif
#else
    CHECK(c.known == 0 && c.applied == 0);
#endif
    /* debug drop mask: restricts, never extends */
    {
        IMPISPCaps d, base = query();
        setenv("OPENIMP_CAPS_DROP", "60", 1);       /* hflip + vflip */
        d = query();
        CHECK(!(d.applied & (B(HFLIP) | B(VFLIP))));
        CHECK((d.known & (B(HFLIP) | B(VFLIP))) == (B(HFLIP) | B(VFLIP)));
        CHECK((d.applied & ~base.applied) == 0);
        unsetenv("OPENIMP_CAPS_DROP");
    }
    printf("caps %-3s known=%06llx applied=%06llx %s\n", p,
           (unsigned long long)c.known, (unsigned long long)c.applied,
           fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
