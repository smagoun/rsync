/*
 * engine.c: running rsync commands inside the Classic Mac OS application.
 *
 * Shared by the console front end (front.c) and the GUI (classic/gui):
 * a typed command line is split into arguments (quotes and backslashes as
 * in a shell), $(NAME) is replaced from the "rsync prefs" file, HFS paths
 * ("Disk:Folder:") become the POSIX form the port uses ("/Disk/Folder/"),
 * and openrsync's main() runs.  exit() inside rsync returns here.
 */
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Multiverse.h>

#include "hfsname.h"
#include "engine.h"

#define MAXARGS	128
#define LINEMAX	2048

int	rsync_main(int, char *[]);
void	rules_reset(void);
void	mactypes_reset(void);
extern int verbose;
void	rsync_exit(int) __attribute__((noreturn));
int	mp_abspath(const char *, char *);

/* ---- exit() back to the prompt ----------------------------------------- */

/*
 * exit() targets: the command running at the prompt, and inside it any
 * classic_guarded() call (the daemon serves each connection that way, so
 * a failing transfer ends that connection, not the daemon).
 */
static jmp_buf *exit_target;
static int exit_status;
static int in_rsync;
int engine_busy;

void
rsync_exit(int status)
{
	fflush(stdout);
	fflush(stderr);
	if (!in_rsync || exit_target == NULL)
		exit(status);
	exit_status = status;
	longjmp(*exit_target, 1);
}

int
classic_guarded(int (*fn)(void *), void *arg)
{
	jmp_buf env, *outer = exit_target;
	volatile int rc;

	exit_target = &env;
	if (setjmp(env) == 0)
		rc = fn(arg);
	else
		rc = exit_status;
	exit_target = outer;
	return rc;
}

/* ---- catch abort()/_exit() from deep inside, report, return to prompt -- */

void	__real_abort(void) __attribute__((noreturn));
void	__real__exit(int) __attribute__((noreturn));

void
__wrap_abort(void)
{
	fprintf(stderr, "rsync: internal error (abort)\n");
	rsync_exit(134);
}

void
__wrap__exit(int status)
{
	if (in_rsync) {
		fprintf(stderr, "rsync: _exit(%d)\n", status);
		rsync_exit(status);
	}
	__real__exit(status);
}

/*
 * rsync prints UTF-8; the console window and "rsync log" are MacRoman.
 * Convert on the way out (a sequence may be split across calls).
 */
size_t
engine_utf8_to_macroman(const unsigned char *in, size_t len, char *out)
{
	static unsigned char pend[4];
	static size_t npend;
	unsigned char seq[4], mr[HFS_NAMEMAX + 1];
	char tmp[8];
	size_t i, n = 0, need;

	for (i = 0; i < len; i++) {
		if (npend == 0 && in[i] < 0x80) {
			out[n++] = (char)in[i];
			continue;
		}
		pend[npend++] = in[i];
		need = pend[0] >= 0xF0 ? 4 : pend[0] >= 0xE0 ? 3 :
		    pend[0] >= 0xC0 ? 2 : 1;
		if (npend < need)
			continue;
		memcpy(seq, pend, npend);
		memcpy(tmp, seq, npend);
		tmp[npend] = '\0';
		npend = 0;
		/* hfs_encode_name swaps ':' and '/', irrelevant for one char. */
		if (need > 1 && hfs_encode_name(tmp, mr) == 0 && mr[0] == 1)
			out[n++] = (char)mr[1];
		else
			out[n++] = '?';
	}
	return n;
}

/* ---- memory: a bigger stack than the 68k default (~24 KB) -------------- */

void
engine_grow_stack(long extra)
{
	register Ptr a0 asm("a0") = LMGetApplLimit() - extra;

	asm volatile(".short 0xA02D" : "+r"(a0) : : "d0", "d1", "a1", "cc");
	asm volatile(".short 0xA063" : : : "d0", "d1", "a0", "a1", "cc");
}

/* ---- text conversion ---------------------------------------------------- */

/* The console speaks MacRoman; rsync speaks UTF-8. */
void
engine_macroman_to_utf8(const char *in, char *out, size_t outsz)
{
	unsigned char c[2];
	char tmp[8];
	size_t n = 0, k;

	for (; *in != '\0'; in++) {
		if ((unsigned char)*in < 0x80) {
			tmp[0] = *in;
			k = 1;
		} else {
			/* hfs_decode_name swaps '/' and ':', irrelevant here. */
			c[0] = (unsigned char)*in;
			hfs_decode_name(c, 1, tmp, sizeof(tmp));
			k = strlen(tmp);
		}
		if (n + k + 1 >= outsz)
			break;
		memcpy(out + n, tmp, k);
		n += k;
	}
	out[n] = '\0';
}

/* ---- prefs: "NAME = value" lines for $(NAME) ----------------------------- */

static char *
pref(const char *name)
{
	static char value[512];
	char line[600], *eq, *k, *v, *e;
	FILE *f;

	if ((f = fopen("rsync prefs", "r")) == NULL)
		return NULL;
	while (fgets(line, sizeof(line), f) != NULL) {
		if ((eq = strchr(line, '=')) == NULL || line[0] == '#')
			continue;
		*eq = '\0';
		for (k = line; isspace((unsigned char)*k); k++)
			continue;
		for (e = eq; e > k && isspace((unsigned char)e[-1]); e--)
			continue;
		*e = '\0';
		if (strcmp(k, name) != 0)
			continue;
		for (v = eq + 1; isspace((unsigned char)*v); v++)
			continue;
		for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--)
			continue;
		*e = '\0';
		snprintf(value, sizeof(value), "%s", v);
		fclose(f);
		return value;
	}
	fclose(f);
	return NULL;
}

static int
expand_vars(const char *in, char *out, size_t outsz)
{
	char name[64];
	const char *end, *v;
	size_t n = 0, k;

	while (*in != '\0') {
		if (in[0] == '$' && in[1] == '(' &&
		    (end = strchr(in + 2, ')')) != NULL &&
		    (size_t)(end - in - 2) < sizeof(name)) {
			memcpy(name, in + 2, end - in - 2);
			name[end - in - 2] = '\0';
			if ((v = pref(name)) == NULL) {
				printf("rsync: $(%s) is not set in \"rsync prefs\"\n",
				    name);
				return -1;
			}
			k = strlen(v);
			if (n + k + 1 >= outsz)
				return -1;
			memcpy(out + n, v, k);
			n += k;
			in = end + 1;
			continue;
		}
		if (n + 2 >= outsz)
			return -1;
		out[n++] = *in++;
	}
	out[n] = '\0';
	return 0;
}

/* ---- arguments ----------------------------------------------------------- */

static int
split(char *line, char **argv, int max)
{
	char *r = line, *w = line;
	int argc = 0, quote;

	for (;;) {
		while (isspace((unsigned char)*r))
			r++;
		if (*r == '\0')
			break;
		if (argc == max - 1)
			return -1;
		argv[argc++] = w;
		quote = 0;
		while (*r != '\0' && (quote || !isspace((unsigned char)*r))) {
			if (*r == '\\' && r[1] != '\0' && quote != '\'') {
				*w++ = r[1];
				r += 2;
			} else if ((*r == '"' || *r == '\'') && !quote) {
				quote = *r++;
			} else if (*r == quote) {
				quote = 0;
				r++;
			} else
				*w++ = *r++;
		}
		if (*r != '\0')
			r++;
		*w++ = '\0';
	}
	argv[argc] = NULL;
	return argc;
}

static int
is_volume(const char *utf8)
{
	unsigned char want[HFS_NAMEMAX + 1], have[HFS_NAMEMAX + 1];
	HParamBlockRec pb;
	Str255 name;
	short i;

	if (hfs_encode_name(utf8, want) < 0)
		return 0;
	for (i = 1; ; i++) {
		memset(&pb, 0, sizeof(pb));
		pb.volumeParam.ioNamePtr = name;
		pb.volumeParam.ioVolIndex = i;
		if (PBHGetVInfoSync(&pb) != noErr)
			return 0;
		memcpy(have, name, name[0] + 1);
		if (hfs_name_equal(want, have))
			return 1;
	}
}

/*
 * "Disk:Folder:file" -> "/Disk/Folder/file"; ":Folder:" -> "Folder/".
 * A '/' inside an HFS name becomes ':' (the port's POSIX names keep ':'
 * for what HFS shows as '/').  Remote specs (host::module, rsync://,
 * host:path with a host that is not a mounted disk) are left alone.
 */
char *
engine_hfs_arg(const char *arg)
{
	char *out, *o, first[256];
	const char *c, *colon;
	size_t len;

	if (arg[0] == '-' || strncmp(arg, "rsync://", 8) == 0 ||
	    strstr(arg, "::") != NULL || (colon = strchr(arg, ':')) == NULL)
		return NULL;
	if (colon != arg) {
		len = (size_t)(colon - arg);
		if (len >= sizeof(first))
			return NULL;
		memcpy(first, arg, len);
		first[len] = '\0';
		if (!is_volume(first))
			return NULL;	/* host:path */
	}
	if ((out = malloc(strlen(arg) + 2)) == NULL)
		return NULL;
	o = out;
	c = arg;
	if (*c == ':')
		c++;		/* relative */
	else
		*o++ = '/';
	for (; *c != '\0'; c++)
		*o++ = *c == ':' ? '/' : (*c == '/' ? ':' : *c);
	*o = '\0';
	return out;
}

/* ---- running a command ---------------------------------------------------- */

static void
close_leaked_fds(void)
{
	int fd;

	for (fd = 3; fd < 64; fd++)
		close(fd);
}

static int run_argc;

static int
run_main(void *argv)
{
	return rsync_main(run_argc, argv);
}

int
engine_run_argv(int argc, char **argv)
{
	int rc;

	/*
	 * openrsync expects a fresh process; reset what outlives a run:
	 * getopt, the verbosity counter and the filter rules.
	 */
	run_argc = argc;
	optreset = 1;		/* BSD getopt (macposix/getopt_long.c) */
	optind = 1;
	verbose = 0;
	rules_reset();
	mactypes_reset();
	in_rsync = 1;
	engine_busy = 1;
	mp_interrupted = 0;
	rc = classic_guarded(run_main, argv);
	in_rsync = 0;
	engine_busy = 0;
	if (mp_interrupted) {
		printf("rsync: interrupted (Cmd-.)\n");
		mp_interrupted = 0;
	}
	fflush(stdout);
	fflush(stderr);
	close_leaked_fds();
	hfs_names_reset();
	return rc;
}

int
engine_run_line(const char *typed)
{
	static char utf8[LINEMAX], line[LINEMAX];
	char *argv[MAXARGS + 2], *conv[MAXARGS];
	int argc, i, nconv = 0, rc;

	engine_macroman_to_utf8(typed, utf8, sizeof(utf8));
	if (expand_vars(utf8, line, sizeof(line)) == -1)
		return 1;
	argv[0] = "rsync";
	if ((argc = split(line, argv + 1, MAXARGS)) == -1) {
		printf("rsync: too many arguments\n");
		return 1;
	}
	if (argc == 0)
		return 0;
	if (strcmp(argv[1], "rsync") == 0)
		memmove(argv, argv + 1, sizeof(char *) * (size_t)argc--);
	argc++;
	argv[argc] = NULL;
	for (i = 1; i < argc; i++)
		if ((conv[nconv] = engine_hfs_arg(argv[i])) != NULL)
			argv[i] = conv[nconv++];
	rc = engine_run_argv(argc, argv);
	for (i = 0; i < nconv; i++)
		free(conv[i]);
	return rc;
}

/* Called by the receiver with the server's file list. */
struct flist;
void
classic_register_names(const char *root, const struct flist *fl,
    size_t flsz)
{
	/* Defined in register.c, which knows struct flist. */
	extern void classic_register_names_impl(const char *,
	    const struct flist *, size_t);

	classic_register_names_impl(root, fl, flsz);
}

/* ---- built-in commands ---------------------------------------------------- */

/*
 * "trace on|off" and "set NAME=VALUE" (environment, e.g. RSYNC_GMT_OFFSET).
 * Returns 1 if the line was one of these.
 */
int
engine_builtin(const char *line)
{
	char name[64];
	const char *eq;

	if (strncmp(line, "trace ", 6) == 0) {
		mp_tracing = strcmp(line + 6, "on") == 0;
		return 1;
	}
	if (strncmp(line, "set ", 4) == 0 && (eq = strchr(line, '=')) != NULL &&
	    (size_t)(eq - line - 4) < sizeof(name)) {
		memcpy(name, line + 4, eq - line - 4);
		name[eq - line - 4] = '\0';
		if (eq[1] == '\0')
			unsetenv(name);
		else
			setenv(name, eq + 1, 1);
		return 1;
	}
	return 0;
}
