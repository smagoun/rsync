/*
 * portable.c: implementations behind portable.h (see there).
 */
#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "portable.h"

#ifdef PORTABLE_NEED_SBUF
struct sbuf *
sbuf_new_auto(void)
{
	struct sbuf *s;

	if ((s = calloc(1, sizeof(*s))) == NULL)
		return NULL;
	s->s_size = 128;
	if ((s->s_buf = malloc(s->s_size)) == NULL) {
		free(s);
		return NULL;
	}
	s->s_buf[0] = '\0';
	return s;
}

static int
sbuf_reserve(struct sbuf *s, size_t add)
{
	size_t nsize;
	char *nbuf;

	if (s->s_error)
		return -1;
	if (s->s_len + add + 1 <= s->s_size)
		return 0;
	nsize = s->s_size;
	while (s->s_len + add + 1 > nsize)
		nsize *= 2;
	if ((nbuf = realloc(s->s_buf, nsize)) == NULL) {
		s->s_error = ENOMEM;
		return -1;
	}
	s->s_buf = nbuf;
	s->s_size = nsize;
	return 0;
}

int
sbuf_bcat(struct sbuf *s, const void *p, size_t len)
{
	if (sbuf_reserve(s, len) == -1)
		return -1;
	memcpy(s->s_buf + s->s_len, p, len);
	s->s_len += len;
	s->s_buf[s->s_len] = '\0';
	return 0;
}

int
sbuf_cat(struct sbuf *s, const char *str)
{
	return sbuf_bcat(s, str, strlen(str));
}

int
sbuf_putc(struct sbuf *s, int c)
{
	char ch = (char)c;

	return sbuf_bcat(s, &ch, 1);
}

int
sbuf_vprintf(struct sbuf *s, const char *fmt, va_list ap)
{
	va_list cp;
	int n;

	va_copy(cp, ap);
	n = vsnprintf(NULL, 0, fmt, cp);
	va_end(cp);
	if (n < 0) {
		s->s_error = EINVAL;
		return -1;
	}
	if (sbuf_reserve(s, (size_t)n) == -1)
		return -1;
	vsnprintf(s->s_buf + s->s_len, (size_t)n + 1, fmt, ap);
	s->s_len += (size_t)n;
	return 0;
}

int
sbuf_printf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = sbuf_vprintf(s, fmt, ap);
	va_end(ap);
	return rc;
}

int
sbuf_finish(struct sbuf *s)
{
	s->s_finished = 1;
	if (s->s_error) {
		errno = s->s_error;
		return -1;
	}
	return 0;
}

char *
sbuf_data(struct sbuf *s)
{
	return s->s_buf;
}

ssize_t
sbuf_len(struct sbuf *s)
{
	if (s->s_error) {
		errno = s->s_error;
		return -1;
	}
	return (ssize_t)s->s_len;
}

void
sbuf_clear(struct sbuf *s)
{
	s->s_len = 0;
	s->s_buf[0] = '\0';
	s->s_error = 0;
	s->s_finished = 0;
}

void
sbuf_delete(struct sbuf *s)
{
	if (s == NULL)
		return;
	free(s->s_buf);
	free(s);
}
#endif /* PORTABLE_NEED_SBUF */

#ifdef PORTABLE_NEED_HUMANIZE
/* After FreeBSD/NetBSD humanize_number(3). */
int
humanize_number(char *buf, size_t len, int64_t quotient, const char *suffix,
    int scale, int flags)
{
	const char *prefixes, *sep;
	int i, r, remainder, s1, s2, sign, divisordeccut;
	int64_t divisor, max;
	size_t baselen;

	if (flags & HN_DIVISOR_1000) {
		divisor = 1000;
		divisordeccut = 950;
		prefixes = (flags & HN_B) ? "B\0\0k\0\0M\0\0G\0\0T\0\0P\0\0E" :
		    "\0\0\0k\0\0M\0\0G\0\0T\0\0P\0\0E";
	} else {
		divisor = 1024;
		divisordeccut = 973;
		if (flags & HN_IEC_PREFIXES)
			prefixes = (flags & HN_B) ?
			    "B\0\0Ki\0Mi\0Gi\0Ti\0Pi\0Ei" :
			    "\0\0\0Ki\0Mi\0Gi\0Ti\0Pi\0Ei";
		else
			prefixes = (flags & HN_B) ?
			    "B\0\0K\0\0M\0\0G\0\0T\0\0P\0\0E" :
			    "\0\0\0K\0\0M\0\0G\0\0T\0\0P\0\0E";
	}
#define SCALE2PREFIX(scale)	(&prefixes[(scale) * 3])

	if (buf == NULL || suffix == NULL)
		return -1;
	if (scale < 0 || (scale >= 7 &&
	    (scale & (HN_AUTOSCALE | HN_GETSCALE)) == 0))
		return -1;
	if (len > 0)
		buf[0] = '\0';

	remainder = 0;
	if (quotient < 0) {
		sign = -1;
		quotient = -quotient;
		baselen = 3;
	} else {
		sign = 1;
		baselen = 2;
	}
	if (flags & HN_NOSPACE)
		sep = "";
	else {
		sep = " ";
		baselen++;
	}
	baselen += strlen(suffix);
	if (flags & HN_IEC_PREFIXES)
		baselen++;
	if (len < baselen + 1)
		return -1;

	if (scale & (HN_AUTOSCALE | HN_GETSCALE)) {
		for (max = 1, i = (int)(len - baselen); i-- > 0;)
			max *= 10;
		for (i = 0; (quotient >= max || (quotient == max - 1 &&
		    remainder >= divisordeccut)) && i < 6; i++) {
			remainder = (int)(quotient % divisor);
			quotient /= divisor;
		}
		if (scale & HN_GETSCALE)
			return i;
	} else {
		for (i = 0; i < scale && i < 6; i++) {
			remainder = (int)(quotient % divisor);
			quotient /= divisor;
		}
	}

	if (((quotient == 9 && remainder < divisordeccut) || quotient < 9) &&
	    i > 0 && (flags & HN_DECIMAL)) {
		s1 = (int)quotient + ((remainder * 10 + (int)divisor / 2) /
		    (int)divisor / 10);
		s2 = ((remainder * 10 + (int)divisor / 2) / (int)divisor) % 10;
		r = snprintf(buf, len, "%d.%d%s%s%s", sign * s1, s2, sep,
		    SCALE2PREFIX(i), suffix);
	} else
		r = snprintf(buf, len, "%" PRId64 "%s%s%s",
		    sign * (quotient + (remainder + divisor / 2) / divisor),
		    sep, SCALE2PREFIX(i), suffix);
	return r;
}
#endif /* PORTABLE_NEED_HUMANIZE */
