/*
 * ops.c: path operations (stat, mkdir, unlink, rename, times, ...).
 */
#include "mpint.h"

#include <stdlib.h>
#include <string.h>

void	mp_dircache_flush(void);
int	mp_fd_pending_mtime(int, unsigned long);
int	mp_fd_for_node(const struct mp_node *);

static int
lookup(int dirfd, const char *path, struct mp_node *n)
{
	if (mp_resolve(dirfd, path, n) == -1)
		return -1;
	if (!n->exists) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

/* ---- stat ------------------------------------------------------------ */

int
mp_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	struct mp_node n;

	MPTRACE("[fstatat %d %s]\n", dirfd, path);

	(void)flags;	/* no symlinks on HFS (aliases are plain files) */
	if (lookup(dirfd, path, &n) == -1)
		return -1;
	mp_cat_to_stat(&n, st);
	return 0;
}

int
mp_stat(const char *path, struct stat *st)
{
	return mp_fstatat(AT_FDCWD, path, st, 0);
}

int
mp_lstat(const char *path, struct stat *st)
{
	return mp_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int
mp_faccessat(int dirfd, const char *path, int mode, int flags)
{
	struct mp_node n;

	(void)flags;
	if (lookup(dirfd, path, &n) == -1)
		return -1;
	if ((mode & W_OK) && !n.isdir && (n.cat.hFileInfo.ioFlAttrib & 0x01)) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

int
mp_access(const char *path, int mode)
{
	return mp_faccessat(AT_FDCWD, path, mode, 0);
}

/* ---- create and delete ---------------------------------------------- */

int
mp_mkdirat(int dirfd, const char *path, mode_t mode)
{
	struct mp_node n;
	FSSpec spec;
	long newid;
	OSErr err;

	MPTRACE("[mkdirat %d %s]\n", dirfd, path);

	(void)mode;
	if (mp_resolve(dirfd, path, &n) == -1)
		return -1;
	if (n.exists) {
		errno = EEXIST;
		return -1;
	}
	mp_node_spec(&n, &spec);
	if ((err = FSpDirCreate(&spec, smSystemScript, &newid)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
mp_mkdir(const char *path, mode_t mode)
{
	return mp_mkdirat(AT_FDCWD, path, mode);
}

int
mp_unlinkat(int dirfd, const char *path, int flags)
{
	struct mp_node n;
	FSSpec spec;
	OSErr err;
	int fd;

	MPTRACE("[unlinkat %d %s]\n", dirfd, path);

	if (lookup(dirfd, path, &n) == -1)
		return -1;
	if (n.isdir && !(flags & AT_REMOVEDIR)) {
		errno = EISDIR;	/* POSIX says EPERM; EISDIR is clearer */
		return -1;
	}
	if (!n.isdir && (flags & AT_REMOVEDIR)) {
		errno = ENOTDIR;
		return -1;
	}
	mp_node_spec(&n, &spec);
	err = FSpDelete(&spec);
	if (err == fBsyErr && n.isdir) {
		errno = ENOTEMPTY;
		return -1;
	}
	if (err == fBsyErr && (fd = mp_fd_for_node(&n)) != -1) {
		/*
		 * POSIX lets an open file be unlinked; HFS does not.  We
		 * opened it ourselves (rsync unlinks temp files it still
		 * holds), so delete it when the last descriptor closes.
		 */
		mp_fdget(fd)->delete_on_close = 1;
		return 0;
	}
	if (n.isdir)
		mp_dircache_flush();
	return mp_oserr(err);
}

int
mp_unlink(const char *path)
{
	return mp_unlinkat(AT_FDCWD, path, 0);
}

int
mp_rmdir(const char *path)
{
	return mp_unlinkat(AT_FDCWD, path, AT_REMOVEDIR);
}

/* ---- rename ---------------------------------------------------------- */

/* Copy the resource fork of `from` to `to` (both closed files). */
static int
copy_rsrc(const FSSpec *from, const FSSpec *to)
{
	static char buf[8192];
	short in, out;
	long len, done, cnt;
	OSErr err;

	if ((err = FSpOpenRF(from, fsRdPerm, &in)) != noErr)
		return mp_oserr(err);
	if ((err = FSpOpenRF(to, fsRdWrPerm, &out)) != noErr) {
		FSClose(in);
		return mp_oserr(err);
	}
	GetEOF(in, &len);
	err = SetEOF(out, len);
	for (done = 0; err == noErr && done < len; done += cnt) {
		cnt = len - done < (long)sizeof(buf) ? len - done :
		    (long)sizeof(buf);
		if ((err = FSRead(in, &cnt, buf)) == noErr)
			err = FSWrite(out, &cnt, buf);
		mp_yield();
	}
	FSClose(in);
	if (FSClose(out) != noErr && err == noErr)
		err = ioErr;
	return mp_oserr(err);
}

/*
 * Replace the data fork of existing file `to` with that of `from`, then
 * delete `from`.  The target keeps its resource fork, Finder info, file
 * ID and backup date (the --netatalk sidecar date); it takes `from`'s
 * modification date.  This is how rsync's "write a temp file, rename it
 * over the old one" lands on HFS without losing the resource fork.
 */
static int
replace_file(struct mp_node *from, struct mp_node *to)
{
	FSSpec fs, ts;
	unsigned long mddat;
	OSErr err;

	mp_node_spec(from, &fs);
	mp_node_spec(to, &ts);
	mddat = from->cat.hFileInfo.ioFlMdDat;
	if (to->cat.hFileInfo.ioFlRLgLen > 0 &&
	    copy_rsrc(&ts, &fs) == -1)
		return -1;
	if ((err = FSpExchangeFiles(&fs, &ts)) != noErr)
		return mp_oserr(err);
	/* `from` now holds the old contents. */
	if ((err = FSpDelete(&fs)) != noErr)
		return mp_oserr(err);
	if (mp_refresh(to) == -1)
		return -1;
	to->cat.hFileInfo.ioFlMdDat = mddat;
	to->cat.hFileInfo.ioDirID = to->parID;
	if ((err = PBSetCatInfoSync(&to->cat)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
mp_renameat(int ofd, const char *opath, int nfd, const char *npath)
{
	struct mp_node from, to;
	FSSpec fs, dir;
	Str63 tmpname;
	OSErr err;

	MPTRACE("[rename %s -> %s]\n", opath, npath);

	if (lookup(ofd, opath, &from) == -1)
		return -1;
	if (mp_resolve(nfd, npath, &to) == -1)
		return -1;
	if (from.vRefNum != to.vRefNum) {
		errno = EXDEV;
		return -1;
	}
	if (to.exists) {
		if (from.parID == to.parID &&
		    hfs_name_equal(from.name, to.name)) {
			/* Same item (perhaps a case change only). */
			if (memcmp(from.name, to.name, from.name[0] + 1) == 0)
				return 0;
			mp_node_spec(&from, &fs);
			return mp_oserr(FSpRename(&fs, to.name));
		}
		if (!from.isdir && !to.isdir)
			return replace_file(&from, &to);
		if (from.isdir && to.isdir) {
			if (mp_unlinkat(nfd, npath, AT_REMOVEDIR) == -1)
				return -1;
		} else {
			errno = from.isdir ? ENOTDIR : EISDIR;
			return -1;
		}
	}

	mp_node_spec(&from, &fs);
	if (from.parID == to.parID)
		err = FSpRename(&fs, to.name);
	else {
		/*
		 * Move, then rename.  If the old name is taken in the new
		 * folder, rename to a scratch name first.
		 */
		if (memcmp(from.name, to.name, from.name[0] + 1) != 0) {
			snprintf((char *)tmpname + 1, 32, "rsync-mv.%lx",
			    (unsigned long)TickCount());
			tmpname[0] = (unsigned char)strlen((char *)tmpname + 1);
			if ((err = FSpRename(&fs, tmpname)) != noErr)
				return mp_oserr(err);
			memcpy(fs.name, tmpname, tmpname[0] + 1);
		}
		dir.vRefNum = to.vRefNum;
		dir.parID = to.parID;
		dir.name[0] = 0;
		if ((err = FSpCatMove(&fs, &dir)) == noErr &&
		    memcmp(fs.name, to.name, to.name[0] + 1) != 0) {
			fs.parID = to.parID;
			err = FSpRename(&fs, to.name);
		}
	}
	if (from.isdir)
		mp_dircache_flush();
	return mp_oserr(err);
}

int
mp_rename(const char *from, const char *to)
{
	return mp_renameat(AT_FDCWD, from, AT_FDCWD, to);
}

/* ---- times ----------------------------------------------------------- */

static int
set_times(struct mp_node *n, time_t mtime)
{
	unsigned long mddat = mp_unix_to_mac(mtime);
	OSErr err;
	int fd;

	/* An open file would get "now" stamped on close; defer. */
	if (!n->isdir && (fd = mp_fd_for_node(n)) != -1)
		return mp_fd_pending_mtime(fd, mddat);
	if (n->isdir)
		n->cat.dirInfo.ioDrMdDat = mddat;
	else
		n->cat.hFileInfo.ioFlMdDat = mddat;
	n->cat.hFileInfo.ioDirID = n->parID;
	n->cat.hFileInfo.ioNamePtr = n->name;
	if ((err = PBSetCatInfoSync(&n->cat)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
mp_utimensat(int dirfd, const char *path, const struct timespec ts[2],
    int flags)
{
	struct mp_node n;
	time_t t;

	MPTRACE("[utimensat %d %s]\n", dirfd, path);

	(void)flags;
	if (lookup(dirfd, path, &n) == -1)
		return -1;
	if (n.vRefNum == 0 || n.parID == fsRtParID)
		return 0;
	if (ts == NULL || ts[1].tv_nsec == UTIME_NOW)
		t = time(NULL);
	else if (ts[1].tv_nsec == UTIME_OMIT)
		return 0;
	else
		t = ts[1].tv_sec;
	return set_times(&n, t);
}

int
mp_futimens(int fd, const struct timespec ts[2])
{
	struct mp_fd *f;
	time_t t;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_FILE && f->type != MPF_DIR)
		return 0;
	if (ts == NULL || ts[1].tv_nsec == UTIME_NOW)
		t = time(NULL);
	else if (ts[1].tv_nsec == UTIME_OMIT)
		return 0;
	else
		t = ts[1].tv_sec;
	if (f->type == MPF_FILE)
		return mp_fd_pending_mtime(fd, mp_unix_to_mac(t));
	return set_times(f->node, t);
}

int
mp_utimes(const char *path, const struct timeval tv[2])
{
	struct timespec ts[2];

	if (tv == NULL)
		return mp_utimensat(AT_FDCWD, path, NULL, 0);
	ts[0].tv_sec = tv[0].tv_sec;
	ts[0].tv_nsec = 0;
	ts[1].tv_sec = tv[1].tv_sec;
	ts[1].tv_nsec = 0;
	return mp_utimensat(AT_FDCWD, path, ts, 0);
}

/* ---- modes, owners, links: HFS has none ------------------------------ */

int
mp_fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
	struct mp_node n;

	(void)mode;
	(void)flags;
	return lookup(dirfd, path, &n);
}

int
mp_chmod(const char *path, mode_t mode)
{
	return mp_fchmodat(AT_FDCWD, path, mode, 0);
}

int
mp_fchmod(int fd, mode_t mode)
{
	(void)mode;
	return mp_fdget(fd) == NULL ? -1 : 0;
}

int
mp_fchownat(int dirfd, const char *path, uid_t u, gid_t g, int flags)
{
	(void)u;
	(void)g;
	return mp_fchmodat(dirfd, path, 0, flags);
}

int
mp_fchown(int fd, uid_t u, gid_t g)
{
	(void)u;
	(void)g;
	return mp_fdget(fd) == NULL ? -1 : 0;
}

int
mp_lchown(const char *path, uid_t u, gid_t g)
{
	return mp_fchownat(AT_FDCWD, path, u, g, 0);
}

#define NOTSUP(proto, call)						\
proto									\
{									\
	call;								\
	errno = ENOTSUP;						\
	return -1;							\
}

NOTSUP(int mp_linkat(int a, const char *b, int c, const char *d, int e),
    (void)a; (void)b; (void)c; (void)d; (void)e)
NOTSUP(int mp_link(const char *a, const char *b), (void)a; (void)b)
NOTSUP(int mp_symlinkat(const char *a, int b, const char *c),
    (void)a; (void)b; (void)c)
NOTSUP(int mp_symlink(const char *a, const char *b), (void)a; (void)b)
NOTSUP(ssize_t mp_readlinkat(int a, const char *b, char *c, size_t d),
    (void)a; (void)b; (void)c; (void)d)
NOTSUP(ssize_t mp_readlink(const char *a, char *b, size_t c),
    (void)a; (void)b; (void)c)
NOTSUP(int mp_mknodat(int a, const char *b, mode_t c, dev_t d),
    (void)a; (void)b; (void)c; (void)d)
NOTSUP(int mp_mknod(const char *a, mode_t b, dev_t c),
    (void)a; (void)b; (void)c)
NOTSUP(int mp_mkfifoat(int a, const char *b, mode_t c),
    (void)a; (void)b; (void)c)
NOTSUP(int mp_mkfifo(const char *a, mode_t b), (void)a; (void)b)
