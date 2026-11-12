/*
 * Host test (#321): SU_Base_SUTime2Raw / SU_Base_Raw2SUTime of the vendor
 * libsysutils 1.3.0 (T40 SDK). vectors.txt is produced by running the vendor
 * code under unicorn (tools/su_time_oracle.py), 8000 inputs incl. month/day
 * overflow, leap years and the epoch limits 0 and 0xffffffff.
 *
 * The two functions below are the same code as the image's libsysutils
 * (ingenic-system-libs-neo libsysutils/su_base.c). OpenIMP itself does not
 * export SU_Base_Raw2SUTime/SUTime2Raw; vendor behaviour:
 *  - SUTime2Raw: kernel mktime() formula in unsigned 32-bit arithmetic, never
 *    fails, never normalises (month 13, day 0 ... flow through the formula).
 *  - Raw2SUTime: plain struct tm fields (mon 0..11, year-1900), so it is NOT
 *    the inverse of SUTime2Raw (which reads mon 1..12, year > 1900).
 * No undefined range was found: Raw2SUTime equals gmtime() over the whole
 * uint32 range; SUTime2Raw has a defined (wrapping) answer for every input.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int sec, min, hour, mday, mon, year; } SUTime;

static int SU_Base_SUTime2Raw(SUTime *suTime, uint32_t *rawTime)
{
	if (!suTime || !rawTime) return -1;
	int32_t mon = (int32_t)suTime->mon - 2;
	uint32_t year = (uint32_t)suTime->year;
	if (mon <= 0) { mon += 12; year -= 1; }
	uint32_t days = (year >> 2) + year / 400u + (uint32_t)suTime->mday;
	days += year * 365u;
	days += (uint32_t)(mon * 367) / 12u;
	days -= year / 100u;
	uint32_t t = ((days * 24u + (uint32_t)suTime->hour) * 60u +
		      (uint32_t)suTime->min) * 60u + (uint32_t)suTime->sec;
	*rawTime = t + 0x86b1bf80u;
	return 0;
}

static int SU_Base_Raw2SUTime(uint32_t *rawTime, SUTime *suTime)
{
	if (!rawTime || !suTime) return -1;
	uint32_t raw = *rawTime;
	uint32_t days = raw / 86400u, rem = raw % 86400u;
	int64_t z = (int64_t)days + 719468;
	int64_t era = z / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int64_t d = doy - (153 * mp + 2) / 5 + 1;
	int64_t m = mp < 10 ? mp + 3 : mp - 9;
	int64_t y = yoe + era * 400 + (m <= 2);
	suTime->sec = (int)(rem % 60);
	suTime->min = (int)(rem / 60 % 60);
	suTime->hour = (int)(rem / 3600);
	suTime->mday = (int)d;
	suTime->mon = (int)(m - 1);
	suTime->year = (int)(y - 1900);
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argc > 1 ? argv[1] : "tests/su_time/vectors.txt", "r");
	char kind;
	int nr = 0, ns = 0, bad = 0;
	SUTime t, e;
	uint32_t raw, eraw;
	int rc, erc;

	if (!f) { perror("vectors"); return 1; }
	for (;;) {
		int c = fscanf(f, " %c", &kind);
		if (c != 1) break;
		if (kind == 'R') {
			if (fscanf(f, "%u %d %d %d %d %d %d %d", &raw, &erc, &e.sec, &e.min,
				   &e.hour, &e.mday, &e.mon, &e.year) != 8) return 1;
			memset(&t, 0xaa, sizeof(t));
			rc = SU_Base_Raw2SUTime(&raw, &t);
			nr++;
			if (rc != erc || memcmp(&t, &e, sizeof(t))) {
				if (bad++ < 10) printf("R %u: got %d/%d-%d-%d %d:%d:%d want %d/%d-%d-%d %d:%d:%d\n", raw, rc,
					t.year, t.mon, t.mday, t.hour, t.min, t.sec, erc, e.year, e.mon, e.mday, e.hour, e.min, e.sec);
			}
		} else if (kind == 'S') {
			if (fscanf(f, "%d %d %d %d %d %d %d %u", &t.sec, &t.min, &t.hour, &t.mday,
				   &t.mon, &t.year, &erc, &eraw) != 8) return 1;
			raw = 0;
			rc = SU_Base_SUTime2Raw(&t, &raw);
			ns++;
			if (rc != erc || raw != eraw) {
				if (bad++ < 10) printf("S %d %d %d %d %d %d: got %d/%u want %d/%u\n", t.sec, t.min,
					t.hour, t.mday, t.mon, t.year, rc, raw, erc, eraw);
			}
		} else return 1;
	}
	fclose(f);
	/* NULL arguments: -1 (vendor 1.3.0 logs and returns -1) */
	if (SU_Base_SUTime2Raw(NULL, &raw) != -1 || SU_Base_Raw2SUTime(NULL, &t) != -1 ||
	    SU_Base_SUTime2Raw(&t, NULL) != -1 || SU_Base_Raw2SUTime(&raw, NULL) != -1) bad++;
	printf("su_time: %d Raw2SUTime + %d SUTime2Raw vectors, %d mismatches\n", nr, ns, bad);
	return bad || nr < 1000 || ns < 4000;
}
