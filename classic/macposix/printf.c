/*
 * printf.c: a C99 printf engine for the printf family.
 *
 * Retro68's newlib is built without C99 formats and without long long
 * support, so "%zu", "%jd" and "%lld" (used throughout openrsync for sizes
 * and offsets) print literally and misalign the following arguments.
 * The printf family is routed here with the linker's --wrap (see
 * classic/CMakeLists.txt); newlib's own internal uses are unaffected.
 *
 * Supported: flags "-+ #0", width and precision (including '*'), length
 * modifiers hh h l ll j z t L q, conversions d i u o x X c s p % and the
 * floating conversions f F e E g G (accurate to ~15 digits).
 */
#ifndef MP_PRINTF_HOSTTEST
#define MACPOSIX_IMPL 1
#include "macposix.h"
#else
#include <unistd.h>
#define mp_write write
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct out {
	char	*buf;
	size_t	 size;
	size_t	 len;	/* would-be length */
};

static void
put(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void
puts_n(struct out *o, const char *s, size_t n)
{
	while (n-- > 0)
		put(o, *s++);
}

static void
pad(struct out *o, char c, int n)
{
	while (n-- > 0)
		put(o, c);
}

#define F_LEFT	0x01
#define F_PLUS	0x02
#define F_SPACE	0x04
#define F_ALT	0x08
#define F_ZERO	0x10

static void
emit_number(struct out *o, const char *digits, int ndig, const char *prefix,
    int flags, int width, int prec)
{
	int plen = (int)strlen(prefix), zeros = 0, total;

	if (prec >= 0) {
		if (prec > ndig)
			zeros = prec - ndig;
		flags &= ~F_ZERO;
	}
	total = plen + zeros + ndig;
	if (!(flags & F_LEFT) && !(flags & F_ZERO))
		pad(o, ' ', width - total);
	puts_n(o, prefix, (size_t)plen);
	if (!(flags & F_LEFT) && (flags & F_ZERO))
		pad(o, '0', width - total);
	pad(o, '0', zeros);
	puts_n(o, digits, (size_t)ndig);
	if (flags & F_LEFT)
		pad(o, ' ', width - total);
}

static void
fmt_int(struct out *o, uintmax_t v, int neg, int base, int upper, int flags,
    int width, int prec)
{
	char buf[72], prefix[4];
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, p = 0;

	if (!(prec == 0 && v == 0))
		do {
			buf[sizeof(buf) - 1 - n++] = dig[v % (unsigned)base];
			v /= (unsigned)base;
		} while (v != 0);
	if (neg)
		prefix[p++] = '-';
	else if (flags & F_PLUS)
		prefix[p++] = '+';
	else if (flags & F_SPACE)
		prefix[p++] = ' ';
	if ((flags & F_ALT) && base == 8 && (n == 0 ||
	    buf[sizeof(buf) - n] != '0'))
		buf[sizeof(buf) - 1 - n++] = '0';
	if ((flags & F_ALT) && base == 16 && n > 0) {
		prefix[p++] = '0';
		prefix[p++] = upper ? 'X' : 'x';
	}
	prefix[p] = '\0';
	emit_number(o, buf + sizeof(buf) - n, n, prefix, flags, width, prec);
}

/* Floating point: enough for progress rates and percentages. */
static void
fmt_float(struct out *o, double v, char conv, int flags, int width, int prec)
{
	char buf[400], prefix[2] = "";
	int n = 0, exp10 = 0, i, upper, gstyle;
	double scaled, ip, frac;
	long double r;
	uint64_t ipart, fpart, pow10;

	upper = (conv == 'F' || conv == 'E' || conv == 'G');
	if (prec < 0)
		prec = 6;
	if (v != v) {
		emit_number(o, upper ? "NAN" : "nan", 3, "", flags & ~F_ZERO,
		    width, -1);
		return;
	}
	if (v < 0) {
		prefix[0] = '-';
		v = -v;
	} else if (flags & F_PLUS)
		prefix[0] = '+';
	else if (flags & F_SPACE)
		prefix[0] = ' ';
	if (v > 1.7e308) {
		emit_number(o, upper ? "INF" : "inf", 3, prefix, flags & ~F_ZERO,
		    width, -1);
		return;
	}

	gstyle = (conv == 'g' || conv == 'G');
	if (v != 0) {
		for (scaled = v; scaled >= 10; scaled /= 10)
			exp10++;
		for (; scaled < 1; scaled *= 10)
			exp10--;
	}
	if (gstyle) {
		if (prec == 0)
			prec = 1;
		if (exp10 < -4 || exp10 >= prec) {
			conv = upper ? 'E' : 'e';
			prec--;
		} else {
			conv = upper ? 'F' : 'f';
			prec = prec - 1 - exp10;
		}
		if (prec > 17)
			prec = 17;
	}
	if (prec > 17)
		prec = 17;

	if (conv == 'e' || conv == 'E') {
		scaled = v;
		for (i = 0; i < exp10; i++)
			scaled /= 10;
		for (i = 0; i > exp10; i--)
			scaled *= 10;
	} else
		scaled = v;

	for (pow10 = 1, i = 0; i < prec; i++)
		pow10 *= 10;
	ip = (double)(uint64_t)scaled;
	if (scaled >= 1.8e19) {		/* beyond uint64: rough */
		n = snprintf(buf, sizeof(buf), "%s", "huge");
		emit_number(o, buf, n, prefix, flags & ~F_ZERO, width, -1);
		return;
	}
	ipart = (uint64_t)scaled;
	frac = scaled - ip;
	/* Round to nearest, ties to even (as glibc does for exact halves). */
	r = (long double)frac * (long double)pow10;	/* exact for small prec */
	fpart = (uint64_t)r;
	if (r - (long double)fpart > 0.5L ||
	    (r - (long double)fpart == 0.5L && (prec > 0 ? (fpart & 1) :
	    (ipart & 1))))
		fpart++;
	if (fpart >= pow10) {
		fpart -= pow10;
		ipart++;
		if ((conv == 'e' || conv == 'E') && ipart >= 10) {
			ipart = 1;
			exp10++;
		}
	}
	/* integer part */
	{
		char tmp[24];
		int t = 0;

		do {
			tmp[t++] = (char)('0' + ipart % 10);
			ipart /= 10;
		} while (ipart != 0);
		while (t > 0)
			buf[n++] = tmp[--t];
	}
	if (prec > 0 || (flags & F_ALT)) {
		buf[n++] = '.';
		for (i = prec - 1; i >= 0; i--) {
			buf[n + i] = (char)('0' + fpart % 10);
			fpart /= 10;
		}
		n += prec;
	}
	if (gstyle && !(flags & F_ALT) && prec > 0) {
		while (n > 0 && buf[n - 1] == '0')
			n--;
		if (n > 0 && buf[n - 1] == '.')
			n--;
	}
	if (conv == 'e' || conv == 'E') {
		buf[n++] = conv;
		buf[n++] = exp10 < 0 ? '-' : '+';
		if (exp10 < 0)
			exp10 = -exp10;
		if (exp10 >= 100)
			buf[n++] = (char)('0' + exp10 / 100);
		buf[n++] = (char)('0' + (exp10 / 10) % 10);
		buf[n++] = (char)('0' + exp10 % 10);
	}
	emit_number(o, buf, n, prefix, flags, width, -1);
}

int
mp_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };
	int flags, width, prec, len, slen;
	const char *s;
	uintmax_t u;
	intmax_t d;
	char c;

	for (; *fmt != '\0'; fmt++) {
		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		flags = 0;
		for (;; fmt++) {
			if (*fmt == '-')
				flags |= F_LEFT;
			else if (*fmt == '+')
				flags |= F_PLUS;
			else if (*fmt == ' ')
				flags |= F_SPACE;
			else if (*fmt == '#')
				flags |= F_ALT;
			else if (*fmt == '0')
				flags |= F_ZERO;
			else
				break;
		}
		width = 0;
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		prec = -1;
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				if (prec < 0)
					prec = -1;
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
		}
		len = 0;	/* 1 h, 2 hh, 3 l, 4 ll, 5 j, 6 z, 7 t, 8 L */
		switch (*fmt) {
		case 'h':
			len = 1;
			if (*++fmt == 'h') {
				len = 2;
				fmt++;
			}
			break;
		case 'l':
			len = 3;
			if (*++fmt == 'l') {
				len = 4;
				fmt++;
			}
			break;
		case 'q':
			len = 4;
			fmt++;
			break;
		case 'j':
			len = 5;
			fmt++;
			break;
		case 'z':
			len = 6;
			fmt++;
			break;
		case 't':
			len = 7;
			fmt++;
			break;
		case 'L':
			len = 8;
			fmt++;
			break;
		}
		switch (c = *fmt) {
		case 'd':
		case 'i':
			switch (len) {
			case 4: d = va_arg(ap, long long); break;
			case 5: d = va_arg(ap, intmax_t); break;
			case 6: d = va_arg(ap, ssize_t); break;
			case 7: d = va_arg(ap, ptrdiff_t); break;
			case 3: d = va_arg(ap, long); break;
			case 2: d = (signed char)va_arg(ap, int); break;
			case 1: d = (short)va_arg(ap, int); break;
			default: d = va_arg(ap, int); break;
			}
			fmt_int(&o, d < 0 ? (uintmax_t)0 - (uintmax_t)d :
			    (uintmax_t)d, d < 0, 10, 0, flags, width, prec);
			break;
		case 'u':
		case 'o':
		case 'x':
		case 'X':
			switch (len) {
			case 4: u = va_arg(ap, unsigned long long); break;
			case 5: u = va_arg(ap, uintmax_t); break;
			case 6: u = va_arg(ap, size_t); break;
			case 7: u = (uintmax_t)va_arg(ap, ptrdiff_t); break;
			case 3: u = va_arg(ap, unsigned long); break;
			case 2: u = (unsigned char)va_arg(ap, unsigned); break;
			case 1: u = (unsigned short)va_arg(ap, unsigned); break;
			default: u = va_arg(ap, unsigned); break;
			}
			fmt_int(&o, u, 0, c == 'u' ? 10 : c == 'o' ? 8 : 16,
			    c == 'X', flags & ~(F_PLUS | F_SPACE), width, prec);
			break;
		case 'p':
			fmt_int(&o, (uintptr_t)va_arg(ap, void *), 0, 16, 0,
			    F_ALT, width, prec);
			break;
		case 'c':
			if (!(flags & F_LEFT))
				pad(&o, ' ', width - 1);
			put(&o, (char)va_arg(ap, int));
			if (flags & F_LEFT)
				pad(&o, ' ', width - 1);
			break;
		case 's':
			if ((s = va_arg(ap, const char *)) == NULL)
				s = "(null)";
			for (slen = 0; s[slen] != '\0' &&
			    (prec < 0 || slen < prec); slen++)
				continue;
			if (!(flags & F_LEFT))
				pad(&o, ' ', width - slen);
			puts_n(&o, s, (size_t)slen);
			if (flags & F_LEFT)
				pad(&o, ' ', width - slen);
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
			if (len == 8)
				fmt_float(&o, (double)va_arg(ap, long double),
				    c, flags, width, prec);
			else
				fmt_float(&o, va_arg(ap, double), c, flags,
				    width, prec);
			break;
		case 'n':
			*va_arg(ap, int *) = (int)o.len;
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':
			fmt--;
			break;
		default:
			put(&o, '%');
			put(&o, c);
			break;
		}
	}
	if (size > 0)
		buf[o.len < size ? o.len : size - 1] = '\0';
	return (int)o.len;
}

/* ---- the family, via --wrap -------------------------------------------- */

int
__wrap_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	return mp_vsnprintf(buf, size, fmt, ap);
}

int
__wrap_snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = mp_vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int
__wrap_vsprintf(char *buf, const char *fmt, va_list ap)
{
	return mp_vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int
__wrap_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = __wrap_vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

int
__wrap_vasprintf(char **ret, const char *fmt, va_list ap)
{
	va_list cp;
	int n;

	va_copy(cp, ap);
	n = mp_vsnprintf(NULL, 0, fmt, cp);
	va_end(cp);
	if (n < 0 || (*ret = malloc((size_t)n + 1)) == NULL) {
		*ret = NULL;
		return -1;
	}
	mp_vsnprintf(*ret, (size_t)n + 1, fmt, ap);
	return n;
}

int
__wrap_asprintf(char **ret, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = __wrap_vasprintf(ret, fmt, ap);
	va_end(ap);
	return n;
}

int
__wrap_vfprintf(FILE *fp, const char *fmt, va_list ap)
{
	char small[512], *p = small;
	va_list cp;
	int n;

	va_copy(cp, ap);
	n = mp_vsnprintf(small, sizeof(small), fmt, cp);
	va_end(cp);
	if (n >= (int)sizeof(small)) {
		if ((p = malloc((size_t)n + 1)) == NULL)
			return -1;
		mp_vsnprintf(p, (size_t)n + 1, fmt, ap);
	}
	if (n > 0 && fwrite(p, 1, (size_t)n, fp) != (size_t)n)
		n = -1;
	if (p != small)
		free(p);
	return n;
}

int
__wrap_fprintf(FILE *fp, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = __wrap_vfprintf(fp, fmt, ap);
	va_end(ap);
	return n;
}

int
__wrap_vprintf(const char *fmt, va_list ap)
{
	return __wrap_vfprintf(stdout, fmt, ap);
}

int
__wrap_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = __wrap_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}

int
__wrap_vdprintf(int fd, const char *fmt, va_list ap)
{
	char *p;
	int n;

	if ((n = __wrap_vasprintf(&p, fmt, ap)) < 0)
		return -1;
	n = (int)mp_write(fd, p, (size_t)n);
	free(p);
	return n;
}

int
__wrap_dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = __wrap_vdprintf(fd, fmt, ap);
	va_end(ap);
	return n;
}
