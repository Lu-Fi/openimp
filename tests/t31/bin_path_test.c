/* IMP_ISP_SetDefaultBinPath / GetDefaultBinPath: the path is data, never a
 * printf format (the log call had no tag argument, so the user's path became
 * the format; the copy used it as a format too).  Built with FORTIFY_SOURCE (aborts on a %n format). */
#include <stdio.h>
#include <string.h>

#include "core/globals.h"
#include "imp/imp_isp.h"
#include "isp/isp_tseries_dev.h"

extern int IMP_ISP_SetDefaultBinPath(const char *path);
extern int IMP_ISP_GetDefaultBinPath(char *path);

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    (void)fd; (void)req;
    return 0;
}

ISPDevice *gISP;
static ISPDevice dev;
static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

int main(void)
{
    static const char *const paths[] = { "/etc/sensor/%s%n%s.bin", "/a/%x%x%x/b.bin", "%s", "/plain.bin" };
    char out[64];
    unsigned i;

    memset(&dev, 0, sizeof(dev));
    dev.opened = 1;
    gISP = &dev;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        CHECK(IMP_ISP_SetDefaultBinPath(paths[i]) == 0, "set");
        memset(out, 0, sizeof(out));
        IMP_ISP_GetDefaultBinPath(out);     /* driver is faked: only no crash */
    }
    printf(failures ? "bin_path_test FAILED\n" : "bin_path_test OK\n");
    return failures != 0;
}
