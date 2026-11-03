/*
 * Host test for classic/macposix/printf.c: compares mp_vsnprintf with the
 * host C library on a range of formats.
 * cc -DMP_PRINTF_HOSTTEST printf_test.c ../macposix/printf.c
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

int mp_vsnprintf(char *, size_t, const char *, va_list);
static int fails, n;

static void
check(const char *fmt, ...)
{
	char a[256], b[256];
	va_list ap, cp;

	va_start(ap, fmt);
	va_copy(cp, ap);
	vsnprintf(a, sizeof(a), fmt, ap);
	mp_vsnprintf(b, sizeof(b), fmt, cp);
	va_end(cp);
	va_end(ap);
	n++;
	if (strcmp(a, b) != 0) {
		printf("FAIL - \"%s\": libc \"%s\" mine \"%s\"\n", fmt, a, b);
		fails++;
	}
}

int
main(void)
{
	char small[8];

	check("%d %i %u", -42, 17, 3000000000u);
	check("%zu %zd", (size_t)123456789, (ssize_t)-5);
	check("%jd %ju", (intmax_t)-9000000000LL, (uintmax_t)18000000000ULL);
	check("%lld %llu %lli", -1234567890123LL, 1234567890123ULL, 7LL);
	check("%ld %lu %lx", -70000L, 70000UL, 0xdeadbeefUL);
	check("%hd %hu %hhd %hhu", (short)-3, (unsigned short)65535,
	    (signed char)-1, (unsigned char)255);
	check("[%5d] [%-5d] [%05d] [%+d] [% d]", 42, 42, 42, 42, 42);
	check("[%.3d] [%8.3d] [%-8.3d] [%.0d]", 7, 7, 7, 0);
	check("%x %X %#x %#o %o", 255, 255, 255, 8, 8);
	check("[%s] [%10s] [%-10s] [%.2s]", "abc", "abc", "abc", "abc");
	check("[%*d] [%-*d] [%.*s]", 6, 1, 6, 1, 2, "xyz");
	check("%c%c%%", 'h', 'i');
	check("%.2f %.1f %.0f %f", 3.14159, 2.25, 2.5, 1.0 / 3);
	check("%8.2f|%-8.2f|%08.2f", 12.345, 12.345, -1.5);
	check("%.2f %.2f", 999.999, 0.005);
	check("%e %.2E", 12345.678, 0.000123);
	check("%g %g %g %.3g", 0.0001, 123456.0, 1e-5, 3.14159);
	check("%s=%zu, %s=%jd, rate %.2f kB/s", "files", (size_t)6,
	    "size", (intmax_t)10746, 111.82);
	check("%lld:%s", (long long)1 << 40, "after");
	check("%5.1f%%", 99.95);
	check("%p", (void *)0x1234);
	check("%-3c|", 'x');
	check("%10.4s|", "truncate");
	if (snprintf(small, sizeof(small), "%s", "toolong") != 7) {
		printf("FAIL - snprintf return value\n");
		fails++;
	}
	printf("%d checks, %d failed\n", n, fails);
	return fails != 0;
}
