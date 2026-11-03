/*
 * netatalk_posix.c: --netatalk storage backend for Unix hosts, used to
 * test the Classic Mac OS code path.  Forks are kept the way Basilisk II
 * and Retro68 keep them on a host file system:
 *
 *	dir/foo			data fork
 *	dir/.rsrc/foo		resource fork
 *	dir/.finf/foo		32 bytes of FInfo + FXInfo; its mtime holds the
 *				server mtime of the "._foo" sidecar (on HFS that
 *				is the file's backup date)
 *
 * The Classic Mac OS build replaces this file with one that uses the
 * File Manager.
 */
#define NETATALK_NO_REDIRECT 1
#include "config.h"

#if !defined(__MACOS_CLASSIC__)

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"

#if HAVE_NETATALK

const char *const ntb_rules[] = {
	"P .rsrc/",
	"P .finf/",
	"- .rsrc/",
	"- .finf/",
	NULL
};

/* "a/b/foo" -> "a/b/<sub>/foo"; also returns the "a/b/<sub>" dir. */
static int
side_path(const char *path, const char *sub, char *out, size_t outsz,
    char *dir, size_t dirsz)
{
	const char *b;
	int dlen;

	b = strrchr(path, '/');
	dlen = b == NULL ? 0 : (int)(b - path + 1);
	b = b == NULL ? path : b + 1;
	if (snprintf(out, outsz, "%.*s%s/%s", dlen, path, sub, b) >=
	    (int)outsz ||
	    snprintf(dir, dirsz, "%.*s%s", dlen, path, sub) >= (int)dirsz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int
ntb_get(int dirfd, const char *path, struct nt_meta *m)
{
	char p[PATH_MAX], d[PATH_MAX];
	struct stat st;
	int fd;

	memset(m, 0, sizeof(*m));
	if (fstatat(dirfd, path, &st, 0) == -1)
		return -1;
	if (side_path(path, ".rsrc", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if (fstatat(dirfd, p, &st, 0) == 0)
		m->rsrc_len = (uint32_t)st.st_size;
	if (side_path(path, ".finf", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if ((fd = openat(dirfd, p, O_RDONLY)) != -1) {
		if (read(fd, m->finfo, sizeof(m->finfo)) < 0 ||
		    fstat(fd, &st) == -1) {
			close(fd);
			return -1;
		}
		m->sidecar_mtime = st.st_mtime;
		close(fd);
	}
	return 0;
}

ssize_t
ntb_read_rsrc(int dirfd, const char *path, void *buf, size_t len, off_t off)
{
	char p[PATH_MAX], d[PATH_MAX];
	ssize_t n;
	int fd;

	if (side_path(path, ".rsrc", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if ((fd = openat(dirfd, p, O_RDONLY)) == -1)
		return -1;
	n = pread(fd, buf, len, off);
	close(fd);
	return n;
}

static int
set_mtime(int dirfd, const char *p, time_t t)
{
	struct timespec ts[2];

	ts[0].tv_sec = ts[1].tv_sec = t;
	ts[0].tv_nsec = ts[1].tv_nsec = 0;
	return utimensat(dirfd, p, ts, 0);
}

int
ntb_set(int dirfd, const char *path, const uint8_t finfo[AD_FINDERI_LEN],
    int srcfd, off_t rsrc_off, uint32_t rsrc_len, time_t sidecar_mtime)
{
	char p[PATH_MAX], d[PATH_MAX], buf[8192];
	struct stat st;
	uint32_t done;
	ssize_t n;
	int fd;

	/* The sidecar may arrive for a base we do not have yet. */
	if (fstatat(dirfd, path, &st, 0) == -1) {
		if (errno != ENOENT)
			return -1;
		if ((fd = openat(dirfd, path, O_WRONLY | O_CREAT, 0644)) == -1)
			return -1;
		close(fd);
	}

	if (side_path(path, ".rsrc", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if (rsrc_len == 0) {
		if (unlinkat(dirfd, p, 0) == -1 && errno != ENOENT)
			return -1;
	} else {
		if (mkdirat(dirfd, d, 0755) == -1 && errno != EEXIST)
			return -1;
		fd = openat(dirfd, p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd == -1)
			return -1;
		for (done = 0; done < rsrc_len; done += (uint32_t)n) {
			n = pread(srcfd, buf, rsrc_len - done < sizeof(buf) ?
			    rsrc_len - done : sizeof(buf), rsrc_off + done);
			if (n <= 0 || write(fd, buf, n) != n) {
				if (n == 0)
					errno = EIO;
				close(fd);
				return -1;
			}
		}
		if (close(fd) == -1)
			return -1;
	}

	if (side_path(path, ".finf", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if (mkdirat(dirfd, d, 0755) == -1 && errno != EEXIST)
		return -1;
	if ((fd = openat(dirfd, p, O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1)
		return -1;
	if (write(fd, finfo, AD_FINDERI_LEN) != AD_FINDERI_LEN) {
		close(fd);
		return -1;
	}
	if (close(fd) == -1)
		return -1;
	return set_mtime(dirfd, p, sidecar_mtime);
}

int
ntb_set_sidecar_mtime(int dirfd, const char *path, time_t t)
{
	char p[PATH_MAX], d[PATH_MAX];

	if (side_path(path, ".finf", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	return set_mtime(dirfd, p, t);
}

int
ntb_clear(int dirfd, const char *path)
{
	char p[PATH_MAX], d[PATH_MAX];

	if (side_path(path, ".rsrc", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if (unlinkat(dirfd, p, 0) == -1 && errno != ENOENT)
		return -1;
	if (side_path(path, ".finf", p, sizeof(p), d, sizeof(d)) == -1)
		return -1;
	if (unlinkat(dirfd, p, 0) == -1 && errno != ENOENT)
		return -1;
	return 0;
}

void
ntb_base_unlinked(int dirfd, const char *path)
{
	(void)ntb_clear(dirfd, path);
}

#endif /* HAVE_NETATALK */
#endif /* !__MACOS_CLASSIC__ */
