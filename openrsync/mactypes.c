/*
 * mactypes.c: --mac-types and --mac-text (Classic Mac OS conveniences).
 *
 * --mac-types[=FILE]: files the server sends *without* a netatalk "._"
 * sidecar get a type and creator chosen by name.  The built-in table
 * covers common source and data files; FILE (lines ".ext TYPE CREATOR" or
 * "Name TYPE CREATOR", '#' comments) adds entries or overrides them.
 *
 * --mac-text[=EXTS]: files matching EXTS (".c,.h,..." or the built-in list
 * of source extensions) are stored with Mac line endings.  LF and CR are
 * swapped when the file is read or written (netatalk.c).  The swap maps
 * every byte one-to-one, so lengths, offsets, checksums and delta
 * transfer still match the server's copy exactly.
 */
#define NETATALK_NO_REDIRECT 1
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "extern.h"

#if HAVE_NETATALK

int mac_types_active;
int mac_text_active;

struct maptype {
	char	name[32];	/* ".ext" or a whole file name */
	char	type[4];
	char	creator[4];
};

static const struct maptype builtin_types[] = {
	{ ".c", "TEXT", "ttxt" }, { ".h", "TEXT", "ttxt" },
	{ ".cp", "TEXT", "ttxt" }, { ".cpp", "TEXT", "ttxt" },
	{ ".cc", "TEXT", "ttxt" }, { ".hpp", "TEXT", "ttxt" },
	{ ".r", "TEXT", "ttxt" }, { ".p", "TEXT", "ttxt" },
	{ ".pas", "TEXT", "ttxt" }, { ".a", "TEXT", "ttxt" },
	{ ".s", "TEXT", "ttxt" }, { ".asm", "TEXT", "ttxt" },
	{ ".txt", "TEXT", "ttxt" }, { ".md", "TEXT", "ttxt" },
	{ ".make", "TEXT", "ttxt" }, { ".mk", "TEXT", "ttxt" },
	{ ".html", "TEXT", "ttxt" }, { ".htm", "TEXT", "ttxt" },
	{ ".xml", "TEXT", "ttxt" }, { ".json", "TEXT", "ttxt" },
	{ ".sh", "TEXT", "ttxt" }, { ".py", "TEXT", "ttxt" },
	{ ".pl", "TEXT", "ttxt" }, { ".cfg", "TEXT", "ttxt" },
	{ "Makefile", "TEXT", "ttxt" }, { "README", "TEXT", "ttxt" },
	{ ".rsrc", "rsrc", "RSED" },
	{ ".pdf", "PDF ", "CARO" },
	{ ".gif", "GIFf", "ogle" }, { ".jpg", "JPEG", "ogle" },
	{ ".jpeg", "JPEG", "ogle" }, { ".png", "PNGf", "ogle" },
	{ ".pict", "PICT", "ttxt" }, { ".pct", "PICT", "ttxt" },
	{ ".sit", "SIT!", "SITx" }, { ".hqx", "TEXT", "SITx" },
	{ ".bin", "BINA", "SITx" }, { ".zip", "ZIP ", "SITx" },
	{ ".gz", "Gzip", "SITx" }, { ".tar", "TARF", "SITx" },
};

static struct maptype *user_types;
static size_t nuser_types;

static const char *const builtin_text[] = {
	".c", ".h", ".cp", ".cpp", ".cc", ".hpp", ".r", ".p", ".pas", ".a",
	".s", ".asm", ".txt", ".make", ".mk", NULL
};
static char **text_exts;

static void
pad4(char out[4], const char *in)
{
	size_t i, n = strlen(in);

	for (i = 0; i < 4; i++)
		out[i] = i < n ? in[i] : ' ';
}

/* Forget --mac-types/--mac-text (the Mac front end runs many commands). */
void
mactypes_reset(void)
{
	size_t i;

	mac_types_active = mac_text_active = 0;
	free(user_types);
	user_types = NULL;
	nuser_types = 0;
	if (text_exts != NULL) {
		for (i = 0; text_exts[i] != NULL; i++)
			free(text_exts[i]);
		free(text_exts);
		text_exts = NULL;
	}
}

int
mactypes_load(const char *file)
{
	char line[256], name[64], type[16], creator[16];
	struct maptype *nt;
	FILE *f;
	int lineno = 0;

	mac_types_active = 1;
	if (file == NULL || *file == '\0')
		return 0;
	if ((f = fopen(file, "r")) == NULL) {
		ERR("%s: --mac-types", file);
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		lineno++;
		if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line))
			continue;
		if (sscanf(line, "%63s %15s %15s", name, type, creator) != 3 ||
		    strlen(type) > 4 || strlen(creator) > 4) {
			ERRX("%s:%d: expected \"name TYPE CREATOR\"", file,
			    lineno);
			fclose(f);
			return -1;
		}
		nt = realloc(user_types, (nuser_types + 1) * sizeof(*nt));
		if (nt == NULL) {
			fclose(f);
			return -1;
		}
		user_types = nt;
		snprintf(nt[nuser_types].name, sizeof(nt->name), "%s", name);
		pad4(nt[nuser_types].type, type);
		pad4(nt[nuser_types].creator, creator);
		nuser_types++;
	}
	fclose(f);
	return 0;
}

static int
name_matches(const char *pat, const char *base)
{
	size_t bl, pl;

	if (pat[0] != '.')
		return strcasecmp(pat, base) == 0;
	bl = strlen(base);
	pl = strlen(pat);
	return bl > pl && strcasecmp(base + bl - pl, pat) == 0;
}

/* Type and creator for a file name; 0 if none applies. */
int
mactypes_lookup(const char *path, uint8_t finfo[8])
{
	const char *base;
	size_t i;

	if (!mac_types_active)
		return 0;
	base = strrchr(path, '/');
	base = base == NULL ? path : base + 1;
	/* User entries win; the last matching one counts. */
	for (i = nuser_types; i > 0; i--)
		if (name_matches(user_types[i - 1].name, base)) {
			memcpy(finfo, user_types[i - 1].type, 4);
			memcpy(finfo + 4, user_types[i - 1].creator, 4);
			return 1;
		}
	for (i = 0; i < sizeof(builtin_types) / sizeof(builtin_types[0]); i++)
		if (name_matches(builtin_types[i].name, base)) {
			memcpy(finfo, builtin_types[i].type, 4);
			memcpy(finfo + 4, builtin_types[i].creator, 4);
			return 1;
		}
	return 0;
}

int
mactext_set(const char *list)
{
	char *copy, *p, *tok;
	size_t n = 0;

	mac_text_active = 1;
	if (list == NULL || *list == '\0')
		return 0;
	if ((copy = strdup(list)) == NULL)
		return -1;
	for (p = copy; (tok = strsep(&p, ", ")) != NULL; )
		if (*tok != '\0') {
			text_exts = reallocarray(text_exts, n + 2,
			    sizeof(*text_exts));
			if (text_exts == NULL)
				return -1;
			if (tok[0] == '.')
				text_exts[n++] = strdup(tok);
			else if (asprintf(&text_exts[n++], ".%s", tok) == -1)
				return -1;
			text_exts[n] = NULL;
		}
	free(copy);
	return 0;
}

/*
 * Does --mac-text apply to this path?  rsync's temporary files are named
 * ".<name>.XXXXXXXXXX"; they count as <name>.
 */
int
mactext_match(const char *path)
{
	const char *const *list;
	char name[256];
	const char *base;
	size_t len, i;

	if (!mac_text_active)
		return 0;
	base = strrchr(path, '/');
	base = base == NULL ? path : base + 1;
	snprintf(name, sizeof(name), "%s", base);
	len = strlen(name);
	if (name[0] == '.' && len > 12 && name[len - 11] == '.') {
		for (i = len - 10; i < len && isalnum((unsigned char)name[i]);
		    i++)
			continue;
		if (i == len) {
			name[len - 11] = '\0';
			memmove(name, name + 1, len - 11);
		}
	}
	list = text_exts != NULL ? (const char *const *)text_exts :
	    builtin_text;
	for (; *list != NULL; list++)
		if (name_matches(*list, name))
			return 1;
	return 0;
}

#endif /* HAVE_NETATALK */
