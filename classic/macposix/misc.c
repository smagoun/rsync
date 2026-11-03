/*
 * misc.c: the rest of the POSIX surface openrsync touches.  Classic Mac OS
 * runs one process for us, has one user and no signals, so most of this
 * is honest "not supported" or harmless constants.
 */
#include "mpint.h"

#include <glob.h>
#include <reent.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* ---- tracing ---------------------------------------------------------- */

int mp_tracing;

void
mp_trace(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	long n;

	va_start(ap, fmt);
	n = mp_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > (long)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	_consolewrite(2, buf, (size_t)n);
}

/* ---- entropy ------------------------------------------------------------ */

/*
 * newlib's arc4random() (used for temp file names) needs getentropy(),
 * which Retro68 stubs out with ENOSYS; arc4random then raises SIGKILL,
 * which ends the application silently.  Provide something: the clock,
 * the tick count and a counter, mixed.  Nothing here needs to be
 * cryptographically strong (rsync uses it for temporary file names).
 */
int
__wrap__getentropy_r(struct _reent *r, void *buf, size_t len)
{
	static uint32_t state, counter;
	unsigned char *p = buf;
	size_t i;

	(void)r;
	if (state == 0) {
		GetDateTime((unsigned long *)&state);
		state ^= TickCount() * 2654435761u;
	}
	for (i = 0; i < len; i++) {
		state ^= TickCount() + (++counter * 0x9E3779B9u);
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		p[i] = (unsigned char)(state >> 11);
	}
	return 0;
}

/* ---- yielding -------------------------------------------------------- */

/*
 * Give other applications time (cooperative multitasking) at most every
 * few ticks; WaitNextEvent with an empty mask consumes no events, so the
 * console window keeps its own.
 */
void
mp_yield(void)
{
	static unsigned long last;
	EventRecord ev;

	if (TickCount() - last < 6)
		return;
	last = TickCount();
	WaitNextEvent(0, &ev, 0, NULL);
}

/* ---- processes --------------------------------------------------------- */

pid_t
mp_getpid(void)
{
	return 1;
}

pid_t
mp_fork(void)
{
	errno = ENOSYS;
	return -1;
}

int
mp_execvp(const char *file, char *const argv[])
{
	(void)file;
	(void)argv;
	errno = ENOSYS;
	return -1;
}

pid_t
mp_waitpid(pid_t pid, int *status, int opts)
{
	(void)pid;
	(void)status;
	(void)opts;
	errno = ECHILD;
	return -1;
}

int
mp_kill(pid_t pid, int sig)
{
	(void)sig;
	if (pid == 1 || pid == 0)
		return 0;
	errno = ESRCH;
	return -1;
}

int
mp_pipe(int fds[2])
{
	(void)fds;
	errno = ENOSYS;
	return -1;
}

int
mp_setsid(void)
{
	return 1;
}

int
mp_chroot(const char *path)
{
	(void)path;
	errno = EPERM;
	return -1;
}

mode_t
mp_umask(mode_t m)
{
	static mode_t cur = 022;
	mode_t old = cur;

	cur = m;
	return old;
}

unsigned
mp_sleep(unsigned secs)
{
	unsigned long until = TickCount() + secs * 60UL;

	while (TickCount() < until)
		mp_yield();
	return 0;
}

int
mp_usleep(useconds_t us)
{
	unsigned long until = TickCount() + (us * 60UL + 999999UL) / 1000000UL;

	while (TickCount() < until)
		mp_yield();
	return 0;
}

/* ---- users ----------------------------------------------------------- */

uid_t mp_getuid(void)	{ return 0; }
uid_t mp_geteuid(void)	{ return 0; }
gid_t mp_getgid(void)	{ return 0; }
gid_t mp_getegid(void)	{ return 0; }

int setuid(uid_t u)		{ (void)u; return 0; }
int seteuid(uid_t u)		{ (void)u; return 0; }
int setgid(gid_t g)		{ (void)g; return 0; }
int setreuid(uid_t a, uid_t b)	{ (void)a; (void)b; return 0; }
int setregid(gid_t a, gid_t b)	{ (void)a; (void)b; return 0; }

struct passwd *getpwnam(const char *n)	{ (void)n; return NULL; }
struct passwd *getpwuid(uid_t u)	{ (void)u; return NULL; }
struct group *getgrnam(const char *n)	{ (void)n; return NULL; }
struct group *getgrgid(gid_t g)		{ (void)g; return NULL; }

/* ---- signals: none on Classic Mac OS ----------------------------------- */

int
sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
	(void)sig;
	(void)act;
	if (old != NULL)
		memset(old, 0, sizeof(*old));
	return 0;
}

int
sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
	(void)how;
	(void)set;
	if (old != NULL)
		memset(old, 0, sizeof(*old));
	return 0;
}

/* ---- memory mapping: none; openrsync falls back to buffered reads ------ */

void *
mp_mmap(void *a, size_t l, int p, int f, int fd, off_t o)
{
	(void)a;
	(void)l;
	(void)p;
	(void)f;
	(void)fd;
	(void)o;
	errno = ENOMEM;
	return MAP_FAILED;
}

int
mp_munmap(void *a, size_t l)
{
	(void)a;
	(void)l;
	return 0;
}

int
getpagesize(void)
{
	return 4096;
}

/* ---- glob: daemon configuration only --------------------------------- */

int
glob(const char *pat, int flags, int (*errfn)(const char *, int), glob_t *g)
{
	(void)pat;
	(void)flags;
	(void)errfn;
	memset(g, 0, sizeof(*g));
	return GLOB_NOMATCH;
}

void
globfree(glob_t *g)
{
	(void)g;
}

/* ---- syslog: to the console ------------------------------------------ */

static const char *logident = "rsync";

void
mp_openlog(const char *ident, int opt, int fac)
{
	(void)opt;
	(void)fac;
	if (ident != NULL)
		logident = ident;
}

void
mp_vsyslog(int pri, const char *fmt, va_list ap)
{
	(void)pri;
	fprintf(stderr, "%s: ", logident);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void
mp_syslog(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	mp_vsyslog(pri, fmt, ap);
	va_end(ap);
}

void
mp_closelog(void)
{
}

/* ---- program name ----------------------------------------------------- */

static const char *progname = "rsync";

const char *
getprogname(void)
{
	return progname;
}

void
setprogname(const char *p)
{
	const char *s = strrchr(p, '/');

	progname = s ? s + 1 : p;
}

/* ---- line input -------------------------------------------------------- */

ssize_t
getdelim(char **lineptr, size_t *n, int delim, FILE *fp)
{
	size_t len = 0;
	char *nb;
	int c;

	if (*lineptr == NULL || *n == 0) {
		*n = 128;
		if ((*lineptr = malloc(*n)) == NULL)
			return -1;
	}
	while ((c = getc(fp)) != EOF) {
		if (len + 2 > *n) {
			if ((nb = realloc(*lineptr, *n * 2)) == NULL)
				return -1;
			*lineptr = nb;
			*n *= 2;
		}
		(*lineptr)[len++] = (char)c;
		if (c == delim)
			break;
	}
	if (len == 0)
		return -1;
	(*lineptr)[len] = '\0';
	return (ssize_t)len;
}

ssize_t
getline(char **lineptr, size_t *n, FILE *fp)
{
	return getdelim(lineptr, n, '\n', fp);
}

/* ---- passwords ---------------------------------------------------------- */

char *
mp_readpassphrase(const char *prompt, char *buf, size_t len, int flags)
{
	size_t n = 0;
	char c;

	(void)flags;
	if (len == 0) {
		errno = EINVAL;
		return NULL;
	}
	/* The console window echoes; there is no way to turn that off. */
	fputs(prompt, stdout);
	fflush(stdout);
	while (_consoleread(0, &c, 1) == 1 && c != '\n' && c != '\r') {
		if (n + 1 < len)
			buf[n++] = c;
	}
	buf[n] = '\0';
	return buf;
}
