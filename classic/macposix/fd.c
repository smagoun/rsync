/*
 * fd.c: descriptor table and file I/O on HFS data forks.
 *
 * Descriptors 0-2 are Retro68's console window.  The File Manager keeps
 * one mark (file position) per access path, which is what POSIX wants for
 * dup()ed descriptors as well.  Modification times set while a file is
 * open are applied after FSClose, which would otherwise stamp "now".
 */
#include "mpint.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct mp_fd console = { MPF_CONSOLE, 1000, O_RDWR, 0, NULL, 0, NULL };
static struct mp_fd *fds[MP_OPEN_MAX] = { &console, &console, &console };

/* Pending modification dates per open file (applied on close). */
struct pending {
	short		refNum;
	unsigned long	mddat;
};
static struct pending pend[MP_OPEN_MAX];

int
mp_oserr(OSErr err)
{
	switch (err) {
	case noErr:
		return 0;
	case fnfErr:
	case dirNFErr:
	case nsvErr:
		errno = ENOENT;
		break;
	case dupFNErr:
		errno = EEXIST;
		break;
	case fBsyErr:
	case opWrErr:
	case permErr:
	case afpAccessDenied:
		errno = EBUSY;
		break;
	case wPrErr:
	case vLckdErr:
		errno = EROFS;
		break;
	case fLckdErr:
		errno = EACCES;
		break;
	case dskFulErr:
		errno = ENOSPC;
		break;
	case bdNamErr:
		errno = EINVAL;
		break;
	case tmfoErr:
		errno = EMFILE;
		break;
	case eofErr:
	case posErr:
		errno = EINVAL;
		break;
	case memFullErr:
		errno = ENOMEM;
		break;
	case badMovErr:
		errno = EINVAL;
		break;
	case diffVolErr:
		errno = EXDEV;
		break;
	default:
		errno = EIO;
		break;
	}
	return -1;
}

struct mp_fd *
mp_fdget(int fd)
{
	if (fd < 0 || fd >= MP_OPEN_MAX || fds[fd] == NULL) {
		errno = EBADF;
		return NULL;
	}
	return fds[fd];
}

int
mp_fdalloc(struct mp_fd **out)
{
	struct mp_fd *f;
	int i;

	for (i = 3; i < MP_OPEN_MAX; i++)
		if (fds[i] == NULL)
			break;
	if (i == MP_OPEN_MAX) {
		errno = EMFILE;
		return -1;
	}
	if ((f = calloc(1, sizeof(*f))) == NULL)
		return -1;
	f->refs = 1;
	fds[i] = f;
	*out = f;
	return i;
}

void
mp_fdrelease(int fd)
{
	if (fd >= 3 && fd < MP_OPEN_MAX && fds[fd] != NULL &&
	    --fds[fd]->refs == 0) {
		free(fds[fd]->node);
		free(fds[fd]);
	}
	if (fd >= 0 && fd < MP_OPEN_MAX)
		fds[fd] = NULL;
}

/* ---- open/close ----------------------------------------------------- */

static int
open_node(struct mp_node *n, int flags, mode_t mode)
{
	struct mp_fd *f;
	FSSpec spec;
	SInt8 perm;
	short ref;
	OSErr err;
	int fd;

	(void)mode;
	if (n->exists && (flags & O_CREAT) && (flags & O_EXCL)) {
		errno = EEXIST;
		return -1;
	}
	if (n->exists && n->isdir) {
		if ((flags & O_ACCMODE) != O_RDONLY) {
			errno = EISDIR;
			return -1;
		}
		if ((fd = mp_fdalloc(&f)) == -1)
			return -1;
		f->type = MPF_DIR;
		f->flags = flags;
		if ((f->node = malloc(sizeof(*n))) == NULL) {
			mp_fdrelease(fd);
			return -1;
		}
		*f->node = *n;
		return fd;
	}
	if (flags & O_DIRECTORY) {
		errno = n->exists ? ENOTDIR : ENOENT;
		return -1;
	}
	mp_node_spec(n, &spec);
	if (!n->exists) {
		if (!(flags & O_CREAT)) {
			errno = ENOENT;
			return -1;
		}
		/* Blank Finder info: "no metadata" for --netatalk (see meta.c). */
		if ((err = FSpCreate(&spec, 0, 0, smSystemScript))
		    != noErr)
			return mp_oserr(err);
		if (mp_refresh(n) == -1)
			return -1;
	}
	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		perm = fsRdPerm;
		break;
	default:
		perm = fsRdWrPerm;
		break;
	}
	if ((err = FSpOpenDF(&spec, perm, &ref)) != noErr)
		return mp_oserr(err);
	if ((flags & O_TRUNC) && perm != fsRdPerm && (err = SetEOF(ref, 0))
	    != noErr) {
		FSClose(ref);
		return mp_oserr(err);
	}
	if ((fd = mp_fdalloc(&f)) == -1) {
		FSClose(ref);
		return -1;
	}
	f->type = MPF_FILE;
	f->flags = flags;
	f->refNum = ref;
	if ((f->node = malloc(sizeof(*n))) == NULL) {
		FSClose(ref);
		mp_fdrelease(fd);
		return -1;
	}
	*f->node = *n;
	return fd;
}

int
mp_openat(int dirfd, const char *path, int flags, ...)
{
	struct mp_node n;
	mode_t mode = 0;
	va_list ap;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	int fd;

	MPTRACE("[openat %d %s 0x%x]", dirfd, path, flags);
	if (mp_resolve(dirfd, path, &n) == -1)
		return -1;
	fd = open_node(&n, flags, mode);
	MPTRACE("[=%d e%d]\n", fd, fd < 0 ? errno : 0);
	return fd;
}

int
mp_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	return mp_openat(AT_FDCWD, path, flags, mode);
}

/* Set the modification date of a closed file. */
static int
set_mddat(const struct mp_node *n, unsigned long mddat)
{
	CInfoPBRec pb;
	Str63 name;
	OSErr err;

	memcpy(name, n->name, n->name[0] + 1);
	memset(&pb, 0, sizeof(pb));
	pb.hFileInfo.ioNamePtr = name;
	pb.hFileInfo.ioVRefNum = n->vRefNum;
	pb.hFileInfo.ioDirID = n->parID;
	if ((err = PBGetCatInfoSync(&pb)) != noErr)
		return mp_oserr(err);
	pb.hFileInfo.ioDirID = n->parID;
	if (pb.hFileInfo.ioFlAttrib & ioDirMask)
		pb.dirInfo.ioDrMdDat = mddat;
	else
		pb.hFileInfo.ioFlMdDat = mddat;
	if ((err = PBSetCatInfoSync(&pb)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
mp_close(int fd)
{
	struct mp_fd *f;
	FSSpec spec;
	OSErr err = noErr;
	int i;

	MPTRACE("[close %d]\n", fd);
	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type == MPF_CONSOLE) {
		fds[fd] = NULL;
		return 0;
	}
	if (f->refs > 1) {
		f->refs--;
		fds[fd] = NULL;
		return 0;
	}
	if (f->type == MPF_FILE) {
		err = FSClose(f->refNum);
		for (i = 0; i < MP_OPEN_MAX; i++)
			if (pend[i].refNum == f->refNum && pend[i].mddat) {
				set_mddat(f->node, pend[i].mddat);
				pend[i].refNum = 0;
				pend[i].mddat = 0;
			}
		if (f->delete_on_close) {
			mp_node_spec(f->node, &spec);
			FSpDelete(&spec);
		}
	} else if (f->type == MPF_SOCK) {
		mp_sock_close(f->sock);
	}
	mp_fdrelease(fd);
	return err == noErr ? 0 : mp_oserr(err);
}

/* ---- read/write ------------------------------------------------------ */

ssize_t
mp_read(int fd, void *buf, size_t len)
{
	struct mp_fd *f;
	long cnt = (long)len;
	OSErr err;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	switch (f->type) {
	case MPF_CONSOLE:
		return _consoleread(fd, buf, len);
	case MPF_SOCK:
		return mp_sock_read(f->sock, buf, len);
	case MPF_DIR:
		errno = EISDIR;
		return -1;
	default:
		break;
	}
	if (len == 0)
		return 0;
	err = FSRead(f->refNum, &cnt, buf);
	if (err != noErr && err != eofErr)
		return mp_oserr(err);
	return cnt;
}

ssize_t
mp_write(int fd, const void *buf, size_t len)
{
	struct mp_fd *f;
	long cnt = (long)len;
	OSErr err;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	switch (f->type) {
	case MPF_CONSOLE:
		return _consolewrite(fd, buf, len);
	case MPF_SOCK:
		return mp_sock_write(f->sock, buf, len);
	case MPF_DIR:
		errno = EISDIR;
		return -1;
	default:
		break;
	}
	if ((f->flags & O_ACCMODE) == O_RDONLY) {
		errno = EBADF;
		return -1;
	}
	if (f->flags & O_APPEND)
		SetFPos(f->refNum, fsFromLEOF, 0);
	if (len == 0)
		return 0;
	if ((err = FSWrite(f->refNum, &cnt, buf)) != noErr)
		return mp_oserr(err);
	return cnt;
}

ssize_t
mp_pread(int fd, void *buf, size_t len, off_t off)
{
	struct mp_fd *f;
	long mark;
	ssize_t n;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE) {
		errno = ESPIPE;
		return -1;
	}
	GetFPos(f->refNum, &mark);
	if (mp_lseek(fd, off, SEEK_SET) == -1)
		return -1;
	n = mp_read(fd, buf, len);
	SetFPos(f->refNum, fsFromStart, mark);
	return n;
}

ssize_t
mp_pwrite(int fd, const void *buf, size_t len, off_t off)
{
	struct mp_fd *f;
	long mark;
	ssize_t n;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE) {
		errno = ESPIPE;
		return -1;
	}
	GetFPos(f->refNum, &mark);
	if (mp_lseek(fd, off, SEEK_SET) == -1)
		return -1;
	n = mp_write(fd, buf, len);
	SetFPos(f->refNum, fsFromStart, mark);
	return n;
}

off_t
mp_lseek(int fd, off_t off, int whence)
{
	struct mp_fd *f;
	long eof, cur, target;
	OSErr err;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE) {
		errno = ESPIPE;
		return -1;
	}
	GetEOF(f->refNum, &eof);
	GetFPos(f->refNum, &cur);
	switch (whence) {
	case SEEK_SET:
		target = (long)off;
		break;
	case SEEK_CUR:
		target = cur + (long)off;
		break;
	case SEEK_END:
		target = eof + (long)off;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if (target < 0 || off > 0x7fffffffL) {
		errno = EINVAL;
		return -1;
	}
	if (target > eof) {
		/* POSIX allows seeking past the end; HFS needs the space. */
		if ((f->flags & O_ACCMODE) == O_RDONLY) {
			SetFPos(f->refNum, fsFromLEOF, 0);
			return target;	/* reads will return 0 */
		}
		if ((err = SetEOF(f->refNum, target)) != noErr)
			return mp_oserr(err);
	}
	if ((err = SetFPos(f->refNum, fsFromStart, target)) != noErr)
		return mp_oserr(err);
	return target;
}

int
mp_ftruncate(int fd, off_t len)
{
	struct mp_fd *f;
	OSErr err;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE) {
		errno = EINVAL;
		return -1;
	}
	if ((err = SetEOF(f->refNum, (long)len)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
mp_fsync(int fd)
{
	struct mp_fd *f;
	ParamBlockRec pb;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE)
		return 0;
	memset(&pb, 0, sizeof(pb));
	pb.ioParam.ioRefNum = f->refNum;
	return mp_oserr(PBFlushFileSync(&pb));
}

int
mp_fstat(int fd, struct stat *st)
{
	struct mp_fd *f;
	long eof;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	switch (f->type) {
	case MPF_CONSOLE:
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFCHR | 0600;
		return 0;
	case MPF_SOCK:
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFSOCK | 0600;
		return 0;
	default:
		break;
	}
	if (mp_refresh(f->node) == -1)
		return -1;
	mp_cat_to_stat(f->node, st);
	if (f->type == MPF_FILE && GetEOF(f->refNum, &eof) == noErr)
		st->st_size = eof;
	return 0;
}

/* Remember a modification date to apply when fd's file is closed. */
int
mp_fd_pending_mtime(int fd, unsigned long mddat)
{
	struct mp_fd *f;
	int i, slot = -1;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	for (i = 0; i < MP_OPEN_MAX; i++) {
		if (pend[i].refNum == f->refNum) {
			slot = i;
			break;
		}
		if (slot == -1 && pend[i].refNum == 0)
			slot = i;
	}
	if (slot == -1) {
		errno = ENOMEM;
		return -1;
	}
	pend[slot].refNum = f->refNum;
	pend[slot].mddat = mddat;
	return 0;
}

/* If an open file is node n, return its descriptor (for pending dates). */
int
mp_fd_for_node(const struct mp_node *n)
{
	int i;

	for (i = 3; i < MP_OPEN_MAX; i++)
		if (fds[i] != NULL && fds[i]->type == MPF_FILE &&
		    fds[i]->node->vRefNum == n->vRefNum &&
		    fds[i]->node->parID == n->parID &&
		    hfs_name_equal(fds[i]->node->name, n->name))
			return i;
	return -1;
}

/* ---- dup, fcntl ------------------------------------------------------ */

int
mp_dup(int fd)
{
	struct mp_fd *f;
	int i;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	for (i = 3; i < MP_OPEN_MAX; i++)
		if (fds[i] == NULL) {
			fds[i] = f;
			f->refs++;
			return i;
		}
	errno = EMFILE;
	return -1;
}

int
mp_dup2(int fd, int to)
{
	struct mp_fd *f;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (to < 0 || to >= MP_OPEN_MAX) {
		errno = EBADF;
		return -1;
	}
	if (fd == to)
		return to;
	if (fds[to] != NULL)
		mp_close(to);
	fds[to] = f;
	f->refs++;
	return to;
}

int
mp_fcntl(int fd, int cmd, ...)
{
	struct mp_fd *f;
	va_list ap;
	int arg;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	va_start(ap, cmd);
	arg = va_arg(ap, int);
	va_end(ap);
	switch (cmd) {
	case F_GETFL:
		return f->flags;
	case F_SETFL:
		f->flags = (f->flags & O_ACCMODE) | (arg & ~O_ACCMODE);
		return 0;
	case F_GETFD:
	case F_SETFD:
		return 0;
	case F_DUPFD:
		return mp_dup(fd);
	default:
		errno = EINVAL;
		return -1;
	}
}

int
mp_isatty(int fd)
{
	struct mp_fd *f;

	if ((f = mp_fdget(fd)) == NULL)
		return 0;
	return f->type == MPF_CONSOLE;
}

int
mp_ioctl(int fd, unsigned long req, ...)
{
	struct mp_fd *f;
	va_list ap;
	int *nread;
	short rev;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (req != FIONREAD) {
		errno = ENOTTY;
		return -1;
	}
	va_start(ap, req);
	nread = va_arg(ap, int *);
	va_end(ap);
	*nread = 0;
	if (f->type == MPF_SOCK)
		*nread = mp_sock_poll(f->sock, POLLIN, &rev);
	return 0;
}

/* ---- temporary files ------------------------------------------------- */

static const char tmpchars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

int
mp_mkstempat(int dirfd, char *tmpl)
{
	size_t len = strlen(tmpl), i, x;
	static unsigned long seed;
	int fd, tries;

	for (x = 0; x < len && tmpl[len - 1 - x] == 'X'; x++)
		continue;
	if (x < 6) {
		errno = EINVAL;
		return -1;
	}
	if (seed == 0)
		seed = TickCount() ^ (unsigned long)&tmpl;
	for (tries = 0; tries < 100; tries++) {
		for (i = len - x; i < len; i++) {
			seed = seed * 1103515245UL + 12345UL;
			tmpl[i] = tmpchars[(seed >> 16) % (sizeof(tmpchars) - 1)];
		}
		fd = mp_openat(dirfd, tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd != -1 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

int
mp_mkstemp(char *tmpl)
{
	return mp_mkstempat(AT_FDCWD, tmpl);
}

/*
 * An anonymous scratch file (deleted on close).  It lives in the current
 * directory, which is the application's folder unless changed.
 */
int
mp_tmpfd(void)
{
	char path[32];
	struct mp_fd *f;
	int fd;

	snprintf(path, sizeof(path), "rsync-tmp.XXXXXX");
	if ((fd = mp_mkstempat(AT_FDCWD, path)) == -1)
		return -1;
	f = mp_fdget(fd);
	f->delete_on_close = 1;
	return fd;
}
