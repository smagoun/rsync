/*
 * time.c: HFS dates <-> Unix time.
 *
 * HFS dates count seconds since 1904-01-01 in the Mac's *local* time.  The
 * offset to UTC comes from the Date & Time control panel's location
 * (ReadLocation: gmtDelta, a signed 24-bit number of seconds).  The
 * offset can be pinned with the RSYNC_GMT_OFFSET environment variable
 * (seconds east of UTC), e.g. for a Mac that never switches to DST.
 */
#include "mpint.h"

#include <stdlib.h>

#define MAC_EPOCH_DELTA	2082844800UL	/* 1904-01-01 .. 1970-01-01 */

/* ReadLocation() is _ReadXPRam of 12 bytes at 0xE4 (glue not in headers). */
static void
read_location(unsigned char loc[12])
{
	register void *a0 asm("a0") = loc;
	register long d0 asm("d0") = (12L << 16) | 0xE4;

	asm volatile(".short 0xA051" : "+r"(a0), "+r"(d0) : :
	    "d1", "d2", "a1", "cc", "memory");
}

static long
gmt_offset(void)
{
	static int have;
	static long off;
	unsigned char loc[12];
	const char *env;
	long d;

	if (have)
		return off;
	if ((env = getenv("RSYNC_GMT_OFFSET")) != NULL && *env != '\0') {
		off = strtol(env, NULL, 10);
	} else {
		read_location(loc);
		d = ((long)loc[9] << 16) | ((long)loc[10] << 8) | loc[11];
		if (d & 0x800000)
			d |= ~0xFFFFFFL;
		off = d;
	}
	have = 1;
	return off;
}

time_t
mp_mac_to_unix(unsigned long macsecs)
{
	if (macsecs == 0)
		return 0;
	return (time_t)(macsecs - MAC_EPOCH_DELTA) - gmt_offset();
}

unsigned long
mp_unix_to_mac(time_t t)
{
	if (t == 0)
		return 0;
	return (unsigned long)(t + gmt_offset()) + MAC_EPOCH_DELTA;
}
