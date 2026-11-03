/*
 * hfsname.c: map rsync (UTF-8) file names to HFS names and back.
 *
 * HFS names are at most 31 bytes of MacRoman, may not contain ':' and are
 * compared case-insensitively.  The rules (docs/CLASSIC_MACOS_PORT.md
 * section 4.3) are deterministic, so no state is kept on disk:
 *
 *  1. decomposed (NFD) accents are composed;
 *  2. the name is converted to MacRoman;
 *  3. ':' becomes '/' (the Finder's own convention; netatalk does the
 *     reverse when storing Mac names on Unix);
 *  4. a name with characters MacRoman lacks, or longer than 31 bytes,
 *     becomes "<prefix>#XXXX<.ext>", XXXX being a hash of the full UTF-8
 *     name and .ext the original extension (up to 5 bytes);
 *  5. names in one folder that collide case-insensitively: the first in
 *     rsync's sort order keeps its name, the others are mangled as in 4.
 *
 * Rule 5 and the reverse mapping of mangled names need the server's file
 * list, which hfs_names_register() records; the receiver calls it as soon
 * as the list arrives.  Local names not in the list decode plainly, so
 * --delete removes stale mangled files as it should.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfsname.h"

/* MacRoman 0x80-0xFF to Unicode (Apple ROMAN.TXT; 0xDB is the euro sign). */
static const uint16_t mr_to_uni[128] = {
	0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
	0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
	0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
	0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
	0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
	0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
	0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
	0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
	0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
	0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
	0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
	0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
	0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
	0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
	0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
	0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/* MacRoman case folding (to lower case), for HFS name comparison. */
static const uint8_t mr_fold[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
	0x8A, 0x8C, 0x8D, 0x8E, 0x96, 0x9A, 0x9F, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
	0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xBE, 0xBF,
	0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
	0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0x88, 0x8B, 0x9B, 0xCF, 0xCF,
	0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD8, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
	0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0x89, 0x90, 0x87, 0x91, 0x8F, 0x92, 0x94, 0x95, 0x93, 0x97, 0x99,
	0xF0, 0x98, 0x9C, 0x9E, 0x9D, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

/* Base + combining mark -> precomposed, for characters MacRoman has. */
static const struct { uint16_t base, mark, comp; } compose_tab[] = {
	{ 0x0041, 0x0308, 0x00C4 },
	{ 0x0041, 0x030A, 0x00C5 },
	{ 0x0043, 0x0327, 0x00C7 },
	{ 0x0045, 0x0301, 0x00C9 },
	{ 0x004E, 0x0303, 0x00D1 },
	{ 0x004F, 0x0308, 0x00D6 },
	{ 0x0055, 0x0308, 0x00DC },
	{ 0x0061, 0x0301, 0x00E1 },
	{ 0x0061, 0x0300, 0x00E0 },
	{ 0x0061, 0x0302, 0x00E2 },
	{ 0x0061, 0x0308, 0x00E4 },
	{ 0x0061, 0x0303, 0x00E3 },
	{ 0x0061, 0x030A, 0x00E5 },
	{ 0x0063, 0x0327, 0x00E7 },
	{ 0x0065, 0x0301, 0x00E9 },
	{ 0x0065, 0x0300, 0x00E8 },
	{ 0x0065, 0x0302, 0x00EA },
	{ 0x0065, 0x0308, 0x00EB },
	{ 0x0069, 0x0301, 0x00ED },
	{ 0x0069, 0x0300, 0x00EC },
	{ 0x0069, 0x0302, 0x00EE },
	{ 0x0069, 0x0308, 0x00EF },
	{ 0x006E, 0x0303, 0x00F1 },
	{ 0x006F, 0x0301, 0x00F3 },
	{ 0x006F, 0x0300, 0x00F2 },
	{ 0x006F, 0x0302, 0x00F4 },
	{ 0x006F, 0x0308, 0x00F6 },
	{ 0x006F, 0x0303, 0x00F5 },
	{ 0x0075, 0x0301, 0x00FA },
	{ 0x0075, 0x0300, 0x00F9 },
	{ 0x0075, 0x0302, 0x00FB },
	{ 0x0075, 0x0308, 0x00FC },
	{ 0x003D, 0x0338, 0x2260 },
	{ 0x0041, 0x0300, 0x00C0 },
	{ 0x0041, 0x0303, 0x00C3 },
	{ 0x004F, 0x0303, 0x00D5 },
	{ 0x0079, 0x0308, 0x00FF },
	{ 0x0059, 0x0308, 0x0178 },
	{ 0x0041, 0x0302, 0x00C2 },
	{ 0x0045, 0x0302, 0x00CA },
	{ 0x0041, 0x0301, 0x00C1 },
	{ 0x0045, 0x0308, 0x00CB },
	{ 0x0045, 0x0300, 0x00C8 },
	{ 0x0049, 0x0301, 0x00CD },
	{ 0x0049, 0x0302, 0x00CE },
	{ 0x0049, 0x0308, 0x00CF },
	{ 0x0049, 0x0300, 0x00CC },
	{ 0x004F, 0x0301, 0x00D3 },
	{ 0x004F, 0x0302, 0x00D4 },
	{ 0x004F, 0x0300, 0x00D2 },
	{ 0x0055, 0x0301, 0x00DA },
	{ 0x0055, 0x0302, 0x00DB },
	{ 0x0055, 0x0300, 0x00D9 },
};

/* ---------------------------------------------------------------------
 * Character conversion
 */

/* Decode one UTF-8 sequence; returns bytes used, 0 at end, -1 if bad. */
static int
utf8_next(const unsigned char *s, uint32_t *cp)
{
	if (s[0] == 0)
		return 0;
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		return *cp >= 0x80 ? 2 : -1;
	}
	if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x0F) << 12) |
		    ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		return *cp >= 0x800 ? 3 : -1;
	}
	if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((uint32_t)(s[0] & 0x07) << 18) |
		    ((uint32_t)(s[1] & 0x3F) << 12) |
		    ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		return *cp >= 0x10000 && *cp <= 0x10FFFF ? 4 : -1;
	}
	return -1;
}

static size_t
utf8_put(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	out[0] = (char)(0xE0 | (cp >> 12));
	out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[2] = (char)(0x80 | (cp & 0x3F));
	return 3;
}

/* Unicode -> MacRoman byte, or -1. */
static int
uni_to_mr(uint32_t cp)
{
	int i;

	if (cp < 0x80)
		return (int)cp;
	if (cp == 0x00A4)		/* currency sign: 0xDB before Mac OS 8.5 */
		return 0xDB;
	for (i = 0; i < 128; i++)
		if (mr_to_uni[i] == cp)
			return 0x80 + i;
	return -1;
}

static uint32_t
compose(uint32_t base, uint32_t mark)
{
	size_t i;

	for (i = 0; i < sizeof(compose_tab) / sizeof(compose_tab[0]); i++)
		if (compose_tab[i].base == base && compose_tab[i].mark == mark)
			return compose_tab[i].comp;
	return 0;
}

static uint32_t
fnv1a(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s != '\0')
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

/*
 * Convert a UTF-8 name to MacRoman (rules 1-3).  Unrepresentable
 * characters become '_' and set *lossy.  Returns the MacRoman length,
 * which may exceed outsz (output is then truncated).
 */
static size_t
to_macroman(const char *utf8, unsigned char *out, size_t outsz, int *lossy)
{
	const unsigned char *s = (const unsigned char *)utf8;
	uint32_t cp, next, c;
	size_t n = 0;
	int len, len2, b;

	*lossy = 0;
	while ((len = utf8_next(s, &cp)) != 0) {
		if (len < 0) {
			*lossy = 1;
			cp = '_';
			len = 1;
		} else if ((len2 = utf8_next(s + len, &next)) > 0 &&
		    (c = compose(cp, next)) != 0) {
			cp = c;
			len += len2;
		}
		s += len;
		if (cp == ':')
			b = '/';
		else if ((b = uni_to_mr(cp)) == -1 || cp < 0x20) {
			*lossy = 1;
			b = '_';
		}
		if (n < outsz)
			out[n] = (unsigned char)b;
		n++;
	}
	return n;
}

/* Rule 4, with an optional salt to step past collisions. */
static void
mangle(const char *utf8, const unsigned char *mr, size_t mrlen,
    unsigned salt, unsigned char out[HFS_NAMEMAX + 1])
{
	char tag[6];
	size_t extlen = 0, prefix, i;
	uint32_t h;

	h = fnv1a(utf8) + salt * 0x9E3779B9u;
	snprintf(tag, sizeof(tag), "#%04X", (unsigned)((h ^ (h >> 16)) & 0xFFFF));
	for (i = mrlen; i > 0 && mrlen - i < 6; i--)
		if (mr[i - 1] == '.') {
			extlen = mrlen - (i - 1);
			break;
		}
	if (extlen > 5 || extlen == mrlen)
		extlen = 0;
	prefix = HFS_NAMEMAX - 5 - extlen;
	if (prefix > mrlen - extlen)
		prefix = mrlen - extlen;
	memcpy(out + 1, mr, prefix);
	memcpy(out + 1 + prefix, tag, 5);
	memcpy(out + 1 + prefix + 5, mr + mrlen - extlen, extlen);
	out[0] = (unsigned char)(prefix + 5 + extlen);
}

int
hfs_encode_name(const char *utf8, unsigned char out[HFS_NAMEMAX + 1])
{
	unsigned char mr[256 * 3];
	size_t n;
	int lossy;

	n = to_macroman(utf8, mr, sizeof(mr), &lossy);
	if (n > sizeof(mr))
		n = sizeof(mr);
	if (n == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!lossy && n <= HFS_NAMEMAX) {
		memcpy(out + 1, mr, n);
		out[0] = (unsigned char)n;
		return 0;
	}
	mangle(utf8, mr, n, 0, out);
	return 1;
}

void
hfs_decode_name(const unsigned char *mac, size_t len, char *utf8,
    size_t utf8sz)
{
	char buf[3];
	size_t i, n = 0, k;
	uint32_t cp;

	for (i = 0; i < len; i++) {
		cp = mac[i] < 0x80 ? mac[i] : mr_to_uni[mac[i] - 0x80];
		if (cp == '/')
			cp = ':';
		k = utf8_put(cp, buf);
		if (n + k >= utf8sz)
			break;
		memcpy(utf8 + n, buf, k);
		n += k;
	}
	if (utf8sz > 0)
		utf8[n] = '\0';
}

int
hfs_name_equal(const unsigned char *a, const unsigned char *b)
{
	size_t i;

	if (a[0] != b[0])
		return 0;
	for (i = 1; i <= a[0]; i++)
		if (mr_fold[a[i]] != mr_fold[b[i]])
			return 0;
	return 1;
}

/* ---------------------------------------------------------------------
 * Names registered from the server's file list (rules 4-5 in context)
 */

struct reg {
	char		*path;		/* full UTF-8 path of the entry */
	size_t		 dirlen;	/* length of its directory part */
	unsigned char	 hfs[HFS_NAMEMAX + 1];
};

static struct reg *regs;
static size_t nregs;
/* Open-addressing tables of indexes into regs (+1; 0 is empty). */
static size_t *fwd, *rev, tabsz;

static uint32_t
hash_fold(const char *dir, size_t dirlen, const unsigned char *hfs)
{
	uint32_t h = 2166136261u;
	size_t i;

	for (i = 0; i < dirlen; i++)
		h = (h ^ (unsigned char)dir[i]) * 16777619u;
	h = (h ^ '/') * 16777619u;
	for (i = 1; i <= hfs[0]; i++)
		h = (h ^ mr_fold[hfs[i]]) * 16777619u;
	return h;
}

static size_t
rev_find(const char *dir, size_t dirlen, const unsigned char *hfs)
{
	size_t i, r;

	if (tabsz == 0)
		return 0;
	i = hash_fold(dir, dirlen, hfs) & (tabsz - 1);
	while ((r = rev[i]) != 0) {
		if (regs[r - 1].dirlen == dirlen &&
		    strncmp(regs[r - 1].path, dir, dirlen) == 0 &&
		    hfs_name_equal(regs[r - 1].hfs, hfs))
			return r;
		i = (i + 1) & (tabsz - 1);
	}
	return 0;
}

static size_t
fwd_find(const char *path)
{
	size_t i, r;

	if (tabsz == 0)
		return 0;
	i = fnv1a(path) & (tabsz - 1);
	while ((r = fwd[i]) != 0) {
		if (strcmp(regs[r - 1].path, path) == 0)
			return r;
		i = (i + 1) & (tabsz - 1);
	}
	return 0;
}

void
hfs_names_reset(void)
{
	size_t i;

	for (i = 0; i < nregs; i++)
		free(regs[i].path);
	free(regs);
	free(fwd);
	free(rev);
	regs = NULL;
	fwd = rev = NULL;
	nregs = tabsz = 0;
}

static const char *
last_slash(const char *p)
{
	return strrchr(p, '/');
}

int
hfs_names_register(const char *const *paths, size_t n)
{
	unsigned char mr[256 * 3];
	struct reg *r;
	const char *sl, *base;
	size_t i, j, len;
	unsigned salt;
	int lossy;

	hfs_names_reset();
	for (tabsz = 16; tabsz < n * 2 + 1; tabsz *= 2)
		continue;
	regs = calloc(n ? n : 1, sizeof(*regs));
	fwd = calloc(tabsz, sizeof(*fwd));
	rev = calloc(tabsz, sizeof(*rev));
	if (regs == NULL || fwd == NULL || rev == NULL) {
		hfs_names_reset();
		errno = ENOMEM;
		return -1;
	}

	/* Paths arrive in rsync's sort order, so "first wins" is stable. */
	for (i = 0; i < n; i++) {
		if (fwd_find(paths[i]) != 0)
			continue;
		r = &regs[nregs];
		if ((r->path = strdup(paths[i])) == NULL) {
			hfs_names_reset();
			errno = ENOMEM;
			return -1;
		}
		sl = last_slash(r->path);
		r->dirlen = sl == NULL ? 0 : (size_t)(sl - r->path);
		base = sl == NULL ? r->path : sl + 1;
		if (*base == '\0' || strcmp(base, ".") == 0) {
			free(r->path);
			r->path = NULL;
			continue;
		}
		hfs_encode_name(base, r->hfs);
		for (salt = 1; rev_find(r->path, r->dirlen, r->hfs) != 0;
		    salt++) {
			len = to_macroman(base, mr, sizeof(mr), &lossy);
			mangle(base, mr, len < sizeof(mr) ? len : sizeof(mr),
			    salt, r->hfs);
		}
		nregs++;
		j = fnv1a(r->path) & (tabsz - 1);
		while (fwd[j] != 0)
			j = (j + 1) & (tabsz - 1);
		fwd[j] = nregs;
		j = hash_fold(r->path, r->dirlen, r->hfs) & (tabsz - 1);
		while (rev[j] != 0)
			j = (j + 1) & (tabsz - 1);
		rev[j] = nregs;
	}
	return 0;
}

int
hfs_names_forward(const char *utf8path, unsigned char out[HFS_NAMEMAX + 1])
{
	const char *sl;
	size_t r;

	if ((r = fwd_find(utf8path)) != 0) {
		memcpy(out, regs[r - 1].hfs, regs[r - 1].hfs[0] + 1);
		return 0;
	}
	sl = last_slash(utf8path);
	return hfs_encode_name(sl == NULL ? utf8path : sl + 1, out) < 0 ?
	    -1 : 0;
}

void
hfs_names_reverse(const char *utf8dir, const unsigned char *hfs,
    char *utf8, size_t utf8sz)
{
	const char *base;
	size_t r, dirlen;

	dirlen = strlen(utf8dir);
	if (dirlen == 1 && utf8dir[0] == '.')
		dirlen = 0;
	if ((r = rev_find(utf8dir, dirlen, hfs)) != 0) {
		base = regs[r - 1].path + regs[r - 1].dirlen +
		    (regs[r - 1].dirlen ? 1 : 0);
		snprintf(utf8, utf8sz, "%s", base);
		return;
	}
	hfs_decode_name(hfs + 1, hfs[0], utf8, utf8sz);
}
