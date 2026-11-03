/*
 * netatalk.c: --netatalk mode, see netatalk.h for the overview.
 */
#define NETATALK_NO_REDIRECT 1
#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "extern.h"

#if HAVE_NETATALK

int netatalk_active;

/* ---------------------------------------------------------------------
 * AppleDouble codec
 */

static uint32_t
be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | p[3];
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/*
 * Accepts any AppleDouble v2 header (netatalk "._" files, netatalk v2
 * ".AppleDouble" files, macOS "._" files whose Finder-info entry carries a
 * trailing xattr block).  buf holds at least the header and entry table.
 */
int
ad_parse(const uint8_t *buf, size_t len, off_t fsize, struct ad_info *ai)
{
	uint16_t n, i;
	uint32_t id, off, elen;

	memset(ai, 0, sizeof(*ai));
	if (len < 26 || be32(buf) != AD_MAGIC || be32(buf + 4) != AD_VERSION2)
		return -1;
	n = (buf[24] << 8) | buf[25];
	if ((size_t)26 + (size_t)n * 12 > len)
		return -1;
	for (i = 0; i < n; i++) {
		id = be32(buf + 26 + i * 12);
		off = be32(buf + 30 + i * 12);
		elen = be32(buf + 34 + i * 12);
		if ((off_t)off + (off_t)elen > fsize)
			return -1;
		if (id == AD_ENTRY_RFORK) {
			ai->rsrc_off = off;
			ai->rsrc_len = elen;
		} else if (id == AD_ENTRY_FINDERI && elen >= AD_FINDERI_LEN) {
			if ((size_t)off + AD_FINDERI_LEN > len)
				return -1;
			memcpy(ai->finfo, buf + off, AD_FINDERI_LEN);
			ai->has_finfo = 1;
		}
	}
	return 0;
}

void
ad_build_header(uint8_t out[AD_NETATALK_HDRLEN],
    const uint8_t finfo[AD_FINDERI_LEN], uint32_t rsrc_len)
{
	memset(out, 0, AD_NETATALK_HDRLEN);
	put32(out, AD_MAGIC);
	put32(out + 4, AD_VERSION2);
	memcpy(out + 8, "Netatalk        ", 16);
	out[25] = 2;
	put32(out + 26, AD_ENTRY_FINDERI);
	put32(out + 30, 50);
	put32(out + 34, AD_FINDERI_LEN);
	put32(out + 38, AD_ENTRY_RFORK);
	put32(out + 42, AD_NETATALK_HDRLEN);
	put32(out + 46, rsrc_len);
	memcpy(out + 50, finfo, AD_FINDERI_LEN);
}

/* ---------------------------------------------------------------------
 * Names
 */

/* Is the last component of path "._<something>"? */
int
netatalk_is_sidecar(const char *path)
{
	const char *base;

	base = strrchr(path, '/');
	base = base == NULL ? path : base + 1;
	return base[0] == '.' && base[1] == '_' && base[2] != '\0';
}

/* "dir/._foo" -> "dir/foo" in buf. */
static int
base_of(const char *path, char *buf, size_t bufsz)
{
	const char *base;
	size_t dlen;

	base = strrchr(path, '/');
	base = base == NULL ? path : base + 1;
	dlen = base - path;
	if (snprintf(buf, bufsz, "%.*s%s", (int)dlen, path, base + 2) >=
	    (int)bufsz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int
meta_present(const struct nt_meta *m)
{
	static const uint8_t zero[AD_FINDERI_LEN];

	return m->rsrc_len > 0 || memcmp(m->finfo, zero, sizeof(zero)) != 0;
}

static int
virtual(const char *path)
{
	return netatalk_active && netatalk_is_sidecar(path);
}

/*
 * Rules added to the user's filters.  netatalk's own databases and
 * trash never belong on the client, the old ".AppleDouble" layout is not
 * supported, and backend storage must survive --delete.
 */
static const char *const nt_rules[] = {
	"- .AppleDB/",
	"- .AppleDesktop/",
	"- .AppleDouble/",
	"- Network Trash Folder/",
	"- Temporary Items/",
	"- .DS_Store",
	"- ._.DS_Store",
	"- :2eDS_Store",
	NULL
};

int
netatalk_add_rules(void)
{
	const char *const *r;

	for (r = nt_rules; *r != NULL; r++)
		if (parse_rule(*r, RULE_NONE, 0) == -1)
			return -1;
	for (r = ntb_rules; *r != NULL; r++)
		if (parse_rule(*r, RULE_NONE, 0) == -1)
			return -1;
	return 0;
}

/* ---------------------------------------------------------------------
 * Virtual "._name" files
 */

int
nt_fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
	char base[PATH_MAX];
	struct nt_meta m;

	if (!virtual(path))
		return fstatat(dirfd, path, st, flags);
	if (base_of(path, base, sizeof(base)) == -1)
		return -1;
	if (fstatat(dirfd, base, st, flags) == -1)
		return -1;
	if (ntb_get(dirfd, base, &m) == -1)
		return -1;
	if (!meta_present(&m)) {
		errno = ENOENT;
		return -1;
	}
	st->st_mode = S_IFREG | 0644;
	st->st_nlink = 1;
	st->st_size = AD_NETATALK_HDRLEN + (off_t)m.rsrc_len;
	if (m.sidecar_mtime != 0) {
		st->st_mtime = m.sidecar_mtime;
#if defined(__linux__)
		st->st_mtim.tv_nsec = 0;
#endif
	}
	return 0;
}

/* An anonymous read-write scratch file, positioned at 0. */
static int
scratch_fd(void)
{
#ifdef __MACOS_CLASSIC__
	return mp_tmpfd();
#else
	FILE *tf;
	int fd;

	if ((tf = tmpfile()) == NULL)
		return -1;
	fd = dup(fileno(tf));
	fclose(tf);
	return fd;
#endif
}

static void mark_text_fd(int, int);

/* Build the packed AppleDouble in an anonymous temporary file. */
static int
open_packed(int dirfd, const char *base)
{
	struct nt_meta m;
	uint8_t hdr[AD_NETATALK_HDRLEN];
	char buf[8192];
	off_t off;
	ssize_t n;
	int fd;

	if (ntb_get(dirfd, base, &m) == -1)
		return -1;
	if (!meta_present(&m)) {
		errno = ENOENT;
		return -1;
	}
	if ((fd = scratch_fd()) == -1)
		return -1;
	ad_build_header(hdr, m.finfo, m.rsrc_len);
	if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr))
		goto fail;
	for (off = 0; off < (off_t)m.rsrc_len; off += n) {
		n = ntb_read_rsrc(dirfd, base, buf, sizeof(buf), off);
		if (n <= 0) {
			if (n == 0)
				errno = EIO;
			goto fail;
		}
		if (write(fd, buf, (size_t)n) != n)
			goto fail;
	}
	if (lseek(fd, 0, SEEK_SET) == -1)
		goto fail;
	return fd;
fail:
	close(fd);
	return -1;
}

int
nt_openat(int dirfd, const char *path, int flags, ...)
{
	char base[PATH_MAX];
	mode_t mode = 0;
	va_list ap;
	int fd;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (!virtual(path)) {
		fd = openat(dirfd, path, flags, mode);
		mark_text_fd(fd, fd != -1 && mactext_match(path));
		return fd;
	}
	if ((flags & O_ACCMODE) != O_RDONLY) {
		/* Sidecars are only ever written via temp files. */
		errno = EPERM;
		return -1;
	}
	if (base_of(path, base, sizeof(base)) == -1)
		return -1;
	return open_packed(dirfd, base);
}

int
nt_unlinkat(int dirfd, const char *path, int flags)
{
	char base[PATH_MAX];

	if (!virtual(path)) {
		if (unlinkat(dirfd, path, flags) == -1)
			return -1;
		if (netatalk_active && !(flags & AT_REMOVEDIR))
			ntb_base_unlinked(dirfd, path);
		return 0;
	}
	if (base_of(path, base, sizeof(base)) == -1)
		return -1;
	return ntb_clear(dirfd, base);
}

int
nt_utimensat(int dirfd, const char *path, const struct timespec ts[2],
    int flags)
{
	char base[PATH_MAX];

	if (!virtual(path))
		return utimensat(dirfd, path, ts, flags);
	if (base_of(path, base, sizeof(base)) == -1)
		return -1;
	if (ts == NULL || ts[1].tv_nsec == UTIME_OMIT)
		return 0;
	return ntb_set_sidecar_mtime(dirfd, base,
	    ts[1].tv_nsec == UTIME_NOW ? time(NULL) : ts[1].tv_sec);
}

int
nt_fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
	if (!virtual(path))
		return fchmodat(dirfd, path, mode, flags);
	return 0;
}

int
nt_fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags)
{
	if (!virtual(path))
		return fchownat(dirfd, path, uid, gid, flags);
	return 0;
}

/* ---------------------------------------------------------------------
 * Platform hooks
 */

static const char *
basename_of(const char *path)
{
	const char *b;

	b = strrchr(path, '/');
	return b == NULL ? path : b + 1;
}

/* flist flag: this regular file has a "._" sidecar in the list. */
#define NT_HAS_SIDECAR	FLSTAT_PLATFORM_BIT1

/*
 * After the file list arrives, put each "foo" before its "._foo" so the
 * base exists when the sidecar is unpacked (rsync sorts "._foo" first),
 * and mark the bases that have one.
 */
void
netatalk_flist_received(struct sess *sess, struct flist *fl, size_t flsz)
{
	struct flist temp;
	size_t i, j, prefix;
	const char *sbase;

	for (i = 0; i + 1 < flsz; i++) {
		if (!S_ISREG(fl[i].st.mode) || !netatalk_is_sidecar(fl[i].path))
			continue;
		sbase = basename_of(fl[i].path);
		prefix = sbase - fl[i].path;
		for (j = i + 1; j < flsz; j++) {
			if (strncmp(fl[j].path, fl[i].path, prefix) != 0)
				break;
			if (strcmp(fl[j].path + prefix, sbase + 2) != 0)
				continue;
			temp = fl[j];
			fl[j] = fl[i];
			fl[i] = temp;
			fl[i].st.flags |= NT_HAS_SIDECAR;	/* the base */
			break;
		}
	}
}

/*
 * A regular file with no "._foo" on the server must have no resource fork
 * locally either, and blank Finder info, or with --mac-types the type and
 * creator its name maps to.  Called by the receiver once the destination
 * root is open (rootfd); files that do not exist yet are handled when
 * they arrive (netatalk_move_file).
 */
void
netatalk_clear_stale(struct sess *sess, int rootfd, struct flist *fl,
    size_t flsz)
{
	uint8_t want[AD_FINDERI_LEN];
	struct nt_meta m;
	size_t i;
	int mapped;

	if (!netatalk_active || rootfd == -1 || sess->opts->dry_run)
		return;
	for (i = 0; i < flsz; i++) {
		if (!S_ISREG(fl[i].st.mode) || netatalk_is_sidecar(fl[i].path) ||
		    (fl[i].st.flags & NT_HAS_SIDECAR))
			continue;
		if (ntb_get(rootfd, fl[i].path, &m) == -1)
			continue;
		memset(want, 0, sizeof(want));
		mapped = mactypes_lookup(fl[i].path, want);
		if (m.rsrc_len == 0 && (mapped ?
		    memcmp(m.finfo, want, 8) == 0 : !meta_present(&m)))
			continue;
		if (mapped) {
			LOG1("%s: setting type/creator %.4s/%.4s", fl[i].path,
			    (char *)want, (char *)want + 4);
			if (ntb_set(rootfd, fl[i].path, want, -1, 0, 0, 0) == -1)
				ERR("%s: set type/creator", fl[i].path);
			continue;
		}
		LOG1("%s: clearing resource fork and Finder info",
		    fl[i].path);
		if (ntb_clear(rootfd, fl[i].path) == -1)
			ERR("%s: clear metadata", fl[i].path);
	}
}

/* Unpack a received "._foo" (temp file fromfd/fname) into toname's base. */
static int
unpack(const struct sess *sess, struct flist *f, int fromfd,
    const char *fname, int tofd, const char *toname)
{
	char base[PATH_MAX];
	uint8_t hdr[4096];
	struct ad_info ai;
	struct stat st;
	ssize_t n;
	int fd, rc;

	if (base_of(toname, base, sizeof(base)) == -1) {
		ERR("%s", toname);
		return 0;
	}
	if ((fd = openat(fromfd, fname, O_RDONLY)) == -1) {
		ERR("%s: openat", fname);
		return 0;
	}
	if (fstat(fd, &st) == -1 || (n = pread(fd, hdr, sizeof(hdr), 0)) < 0) {
		ERR("%s: read", fname);
		close(fd);
		return 0;
	}
	if (ad_parse(hdr, (size_t)n, st.st_size, &ai) == -1) {
		WARNX("%s: not an AppleDouble file, storing as-is", f->path);
		close(fd);
		return move_file(fromfd, fname, tofd, toname, 1, 1) == 0;
	}
	rc = ntb_set(tofd, base, ai.finfo, fd, ai.rsrc_off, ai.rsrc_len,
	    f->st.mtime);
	close(fd);
	if (rc == -1) {
		ERR("%s: apply resource fork and Finder info", base);
		return 0;
	}
	if (unlinkat(fromfd, fname, 0) == -1)
		ERR("%s: unlink", fname);
	f->flstate |= FLIST_SKIP_METADATA;
	return 1;
}

int
netatalk_move_file(const struct sess *sess, struct flist *fl, int fromfd,
    const char *fname, int tofd, const char *toname, int final,
    int skip_metadata)
{
	if (final && netatalk_is_sidecar(toname))
		return unpack(sess, fl, fromfd, fname, tofd, toname);
	uint8_t want[AD_FINDERI_LEN];

	if (move_file(fromfd, fname, tofd, toname, final, skip_metadata) != 0) {
		ERR("%s: move_file: %s", fname, toname);
		return 0;
	}
	/* A plain file without a sidecar: --mac-types names its type. */
	memset(want, 0, sizeof(want));
	if (final && S_ISREG(fl->st.mode) && !(fl->st.flags & NT_HAS_SIDECAR) &&
	    mactypes_lookup(toname, want) &&
	    ntb_set(tofd, toname, want, -1, 0, 0, 0) == -1)
		ERR("%s: set type/creator", toname);
	return 1;
}

/* ---------------------------------------------------------------------
 * --mac-text: swap LF and CR in the data of matching files (mactypes.c)
 */

#define TEXT_FD_MAX	4096
static unsigned char text_fd[TEXT_FD_MAX / 8];

static int
is_text_fd(int fd)
{
	return fd >= 0 && fd < TEXT_FD_MAX &&
	    (text_fd[fd / 8] & (1 << (fd % 8)));
}

static void
mark_text_fd(int fd, int on)
{
	if (fd < 0 || fd >= TEXT_FD_MAX)
		return;
	if (on)
		text_fd[fd / 8] |= 1 << (fd % 8);
	else
		text_fd[fd / 8] &= ~(1 << (fd % 8));
}

static void
swap_eol(void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] == '\n')
			p[i] = '\r';
		else if (p[i] == '\r')
			p[i] = '\n';
}

ssize_t
nt_read(int fd, void *buf, size_t len)
{
	ssize_t n = read(fd, buf, len);

	if (n > 0 && is_text_fd(fd))
		swap_eol(buf, (size_t)n);
	return n;
}

ssize_t
nt_pread(int fd, void *buf, size_t len, off_t off)
{
	ssize_t n = pread(fd, buf, len, off);

	if (n > 0 && is_text_fd(fd))
		swap_eol(buf, (size_t)n);
	return n;
}

static ssize_t
write_swapped(int fd, const void *buf, size_t len, off_t off, int positional)
{
	char tmp[8192];
	size_t done = 0, chunk;
	ssize_t w;

	while (done < len) {
		chunk = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
		memcpy(tmp, (const char *)buf + done, chunk);
		swap_eol(tmp, chunk);
		w = positional ? pwrite(fd, tmp, chunk, off + (off_t)done) :
		    write(fd, tmp, chunk);
		if (w <= 0)
			return done > 0 ? (ssize_t)done : w;
		done += (size_t)w;
		if ((size_t)w < chunk)
			break;
	}
	return (ssize_t)done;
}

ssize_t
nt_write(int fd, const void *buf, size_t len)
{
	if (!is_text_fd(fd))
		return write(fd, buf, len);
	return write_swapped(fd, buf, len, 0, 0);
}

ssize_t
nt_pwrite(int fd, const void *buf, size_t len, off_t off)
{
	if (!is_text_fd(fd))
		return pwrite(fd, buf, len, off);
	return write_swapped(fd, buf, len, off, 1);
}

int
nt_close(int fd)
{
	mark_text_fd(fd, 0);
	return close(fd);
}

#endif /* HAVE_NETATALK */
