/*
 * #805: the T33 custom AE / AWB workers of src/t40/openimp_p1_algo.c (PLATFORM_T33) against a fake driver.
 * The open-tx-isp T33 driver blocks a get until the next statistics frame and answers EAGAIN after its timeout
 * (EINTR on a signal): the worker must ask again (no exit, no handle call), call handle once per delivered info,
 * send the result only with change != 0, and leave on any other error (EINVAL = the library was closed).
 */
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
static int fake_ioctl(int fd, unsigned long cmd, void *arg);
#define ioctl(fd, cmd, arg) fake_ioctl(fd, cmd, arg)
#include "../../src/t40/openimp_p1_algo.c"
#undef ioctl

static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, m); } } while (0)

static int script[8], nscript, pos, gets, sets, handles, change;
static int fake_ioctl(int fd, unsigned long cmd, void *arg)
{
	uint32_t *w = arg;

	(void)fd;
	if (cmd == TISP_AE_SET || cmd == TISP_AWB_SET) {
		sets++;
		CHECK(w[0] == ALGO_COOKIE && w[1] == 0 && w[2] == 1, "set request: cookie, num, change");
		return 0;
	}
	CHECK(cmd == TISP_AE_GET || cmd == TISP_AWB_GET, "only get / set");
	CHECK(w[0] == ALGO_COOKIE && w[1] == 0, "get request header");
	gets++;
	if (pos >= nscript) { errno = EINVAL; return -1; }
	if (script[pos]) { errno = script[pos++]; return -1; }
	pos++;
	return 0;
}

static void ae_h(void *p, IMPISPAeInfo *i, IMPISPAeAttr *a) { (void)p; (void)i; handles++; a->change = change; a->AeIntegrationTimeUnit = ISP_CORE_EXPR_UNIT_LINE; }
static void awb_h(void *p, IMPISPAwbInfo *i, IMPISPAwbAttr *a) { (void)p; (void)i; handles++; a->change = change; a->r_gain = 300; a->b_gain = 400; }

static void run(int awb, const int *s, int n, int chg)
{
	int k;

	for (k = 0; k < n; k++)
		script[k] = s[k];
	nscript = n;
	pos = gets = sets = handles = 0;
	change = chg;
	memset(&algo[0], 0, sizeof(algo[0]));
	algo[0].fd = 3;
	algo[0].ae.handle = ae_h;
	algo[0].awb.handle = awb_h;
	if (awb)
		awb_worker(&algo[0]);
	else
		ae_worker(&algo[0]);
}

int main(void)
{
	static const int s1[] = { EAGAIN, EINTR, 0, EAGAIN, 0 };
	static const int s2[] = { 0, EIO, 0 };

	run(0, s1, 5, 1);
	CHECK(gets == 6 && handles == 2 && sets == 2 && algo[0].ae_done == 1, "AE: EAGAIN / EINTR ask again, EINVAL ends");
	run(1, s1, 5, 1);
	CHECK(gets == 6 && handles == 2 && sets == 2 && algo[0].awb_done == 1, "AWB: EAGAIN / EINTR ask again, EINVAL ends");
	run(0, s1, 5, 0);
	CHECK(handles == 2 && sets == 0, "AE: change 0 sends no result (vendor 0x101a40)");
	run(1, s2, 3, 1);
	CHECK(gets == 2 && handles == 1 && algo[0].awb_done == 1, "AWB: another error ends the worker");
	printf(fails ? "t33-algo805-test: %d FAIL\n" : "t33-algo805-test: ok\n", fails);
	return fails != 0;
}
