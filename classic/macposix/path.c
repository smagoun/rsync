/*
 * path.c: POSIX paths -> HFS items.
 *
 * "/Volume/Folder/name" is absolute; anything else is relative to the
 * current directory (or to a directory descriptor for the *at calls).
 * Every component is mapped to its HFS name through hfsname.c, using the
 * absolute UTF-8 path so that names registered from the server's file
 * list (mangled or case-colliding ones) resolve consistently.
 */
#include "mpint.h"

#include <stdlib.h>
#include <string.h>

/* Current directory; set lazily to the application's folder. */
static struct {
	int	valid;
	short	vRefNum;
	long	dirID;
	char	path[PATH_MAX];
} cwd;

/* Small cache of resolved folders: absolute path -> (vRefNum, dirID). */
#define DIRCACHE 16
static struct {
	char	path[PATH_MAX];
	short	vRefNum;
	long	dirID;
	unsigned long stamp;
} dircache[DIRCACHE];
static unsigned long dirstamp;

void
mp_dircache_flush(void)
{
	memset(dircache, 0, sizeof(dircache));
}

static int
dircache_get(const char *path, short *vref, long *dirid)
{
	int i;

	for (i = 0; i < DIRCACHE; i++)
		if (dircache[i].stamp && strcmp(dircache[i].path, path) == 0) {
			*vref = dircache[i].vRefNum;
			*dirid = dircache[i].dirID;
			dircache[i].stamp = ++dirstamp;
			return 1;
		}
	return 0;
}

static void
dircache_put(const char *path, short vref, long dirid)
{
	int i, victim = 0;

	if (strlen(path) >= PATH_MAX)
		return;
	for (i = 0; i < DIRCACHE; i++) {
		if (dircache[i].stamp == 0) {
			victim = i;
			break;
		}
		if (dircache[i].stamp < dircache[victim].stamp)
			victim = i;
	}
	strcpy(dircache[victim].path, path);
	dircache[victim].vRefNum = vref;
	dircache[victim].dirID = dirid;
	dircache[victim].stamp = ++dirstamp;
}

/* Name of a volume as UTF-8. */
static int
volume_name(short index, short *vref, char *utf8, size_t sz)
{
	HParamBlockRec pb;
	Str255 name;

	memset(&pb, 0, sizeof(pb));
	pb.volumeParam.ioNamePtr = name;
	pb.volumeParam.ioVolIndex = index;
	if (PBHGetVInfoSync(&pb) != noErr)
		return -1;
	*vref = pb.volumeParam.ioVRefNum;
	hfs_decode_name(name + 1, name[0], utf8, sz);
	return 0;
}

static int
find_volume(const char *utf8, short *vref)
{
	unsigned char want[HFS_NAMEMAX + 1], have[HFS_NAMEMAX + 1];
	char name[256];
	short i, v;

	if (hfs_encode_name(utf8, want) < 0)
		return -1;
	for (i = 1; volume_name(i, &v, name, sizeof(name)) == 0; i++) {
		if (hfs_encode_name(name, have) < 0)
			continue;
		if (hfs_name_equal(want, have)) {
			*vref = v;
			return 0;
		}
	}
	return -1;
}

static int
init_cwd(void)
{
	ProcessSerialNumber psn;
	ProcessInfoRec info;
	FSSpec app;
	CInfoPBRec pb;
	Str63 name;
	char comp[256], tmp[PATH_MAX];
	long id;

	if (cwd.valid)
		return 0;
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(info);
	info.processAppSpec = &app;
	psn.highLongOfPSN = 0;
	psn.lowLongOfPSN = kCurrentProcess;
	if (GetProcessInformation(&psn, &info) != noErr)
		return mp_oserr(fnfErr);
	cwd.vRefNum = app.vRefNum;
	cwd.dirID = app.parID;

	/* Build the absolute path by walking up to the root. */
	cwd.path[0] = '\0';
	for (id = app.parID; ; ) {
		memset(&pb, 0, sizeof(pb));
		pb.dirInfo.ioNamePtr = name;
		pb.dirInfo.ioVRefNum = app.vRefNum;
		pb.dirInfo.ioFDirIndex = -1;
		pb.dirInfo.ioDrDirID = id;
		if (PBGetCatInfoSync(&pb) != noErr)
			return mp_oserr(pb.dirInfo.ioResult);
		hfs_decode_name(name + 1, name[0], comp, sizeof(comp));
		snprintf(tmp, sizeof(tmp), "/%s%s", comp, cwd.path);
		strcpy(cwd.path, tmp);
		if (id == fsRtDirID)
			break;
		id = pb.dirInfo.ioDrParID;
	}
	cwd.valid = 1;
	return 0;
}

/* Join base and rel into out, resolving ".", ".." and "//". */
static int
normalize(const char *base, const char *rel, char *out)
{
	char buf[PATH_MAX * 2];
	char *comp[256];
	char *p, *s;
	int n = 0, i;
	size_t len;

	if (rel[0] == '/')
		snprintf(buf, sizeof(buf), "%s", rel);
	else
		snprintf(buf, sizeof(buf), "%s/%s", base, rel);
	for (p = buf; (s = strsep(&p, "/")) != NULL; ) {
		if (*s == '\0' || strcmp(s, ".") == 0)
			continue;
		if (strcmp(s, "..") == 0) {
			if (n > 1)	/* cannot leave the volume */
				n--;
			continue;
		}
		if (n == 256) {
			errno = ENAMETOOLONG;
			return -1;
		}
		comp[n++] = s;
	}
	len = 0;
	out[0] = '\0';
	for (i = 0; i < n; i++) {
		if (len + strlen(comp[i]) + 2 >= PATH_MAX) {
			errno = ENAMETOOLONG;
			return -1;
		}
		out[len++] = '/';
		strcpy(out + len, comp[i]);
		len += strlen(comp[i]);
	}
	if (n == 0)
		strcpy(out, "/");
	return 0;
}

static int
getcat(short vref, long dirid, ConstStr255Param name, CInfoPBRec *pb)
{
	memset(pb, 0, sizeof(*pb));
	pb->hFileInfo.ioNamePtr = (StringPtr)name;
	pb->hFileInfo.ioVRefNum = vref;
	pb->hFileInfo.ioDirID = dirid;
	pb->hFileInfo.ioFDirIndex = 0;
	return PBGetCatInfoSync(pb);
}

int
mp_resolve(int dirfd, const char *path, struct mp_node *n)
{
	char prefix[PATH_MAX], *slash, *comp, *next;
	const char *base;
	unsigned char hname[HFS_NAMEMAX + 1];
	struct mp_fd *f;
	CInfoPBRec pb;
	short vref;
	long dirid;
	size_t plen;
	OSErr err;

	if (path == NULL || *path == '\0') {
		errno = ENOENT;
		return -1;
	}
	if (init_cwd() == -1)
		return -1;
	if (dirfd == AT_FDCWD || path[0] == '/')
		base = cwd.path;
	else {
		if ((f = mp_fdget(dirfd)) == NULL)
			return -1;
		if (f->type != MPF_DIR) {
			errno = ENOTDIR;
			return -1;
		}
		base = f->node->path;
	}

	memset(n, 0, sizeof(*n));
	if (normalize(base, path, n->path) == -1)
		return -1;

	/* "/" is a pseudo folder holding the volumes. */
	if (strcmp(n->path, "/") == 0) {
		n->exists = n->isdir = 1;
		n->vRefNum = 0;
		n->dirID = 1;
		return 0;
	}

	/* Volume. */
	comp = n->path + 1;
	slash = strchr(comp, '/');
	plen = slash ? (size_t)(slash - n->path) : strlen(n->path);
	memcpy(prefix, n->path, plen);
	prefix[plen] = '\0';
	if (find_volume(comp[0] ? prefix + 1 : "", &vref) == -1) {
		errno = ENOENT;
		return -1;
	}
	n->vRefNum = vref;
	if (slash == NULL) {
		n->exists = n->isdir = 1;
		n->parID = fsRtParID;
		n->dirID = fsRtDirID;
		hfs_encode_name(prefix + 1, n->name);
		return getcat(vref, fsRtDirID, "\p", &n->cat) == noErr ? 0 :
		    mp_oserr(n->cat.hFileInfo.ioResult);
	}

	/* Walk the folders, using the cache for the longest known prefix. */
	dirid = fsRtDirID;
	comp = slash + 1;
	for (;;) {
		next = strchr(comp, '/');
		plen = (next ? (size_t)(next - n->path) : strlen(n->path));
		memcpy(prefix, n->path, plen);
		prefix[plen] = '\0';
		if (hfs_names_forward(prefix, hname) == -1) {
			errno = EINVAL;
			return -1;
		}
		if (next == NULL)
			break;
		if (!dircache_get(prefix, &vref, &dirid)) {
			err = getcat(n->vRefNum, dirid, hname, &pb);
			if (err == fnfErr || err == dirNFErr) {
				errno = ENOENT;
				return -1;
			}
			if (err != noErr)
				return mp_oserr(err);
			if (!(pb.hFileInfo.ioFlAttrib & MP_ATTRIB_DIR)) {
				errno = ENOTDIR;
				return -1;
			}
			dirid = pb.dirInfo.ioDrDirID;
			dircache_put(prefix, n->vRefNum, dirid);
		}
		comp = next + 1;
	}

	n->parID = dirid;
	memcpy(n->name, hname, hname[0] + 1);
	return mp_refresh(n);
}

int
mp_refresh(struct mp_node *n)
{
	OSErr err;

	if (n->vRefNum == 0 || n->parID == fsRtParID)
		return 0;	/* pseudo root, volume */
	err = getcat(n->vRefNum, n->parID, n->name, &n->cat);
	if (err == fnfErr) {
		n->exists = n->isdir = 0;
		return 0;
	}
	if (err != noErr)
		return mp_oserr(err);
	n->exists = 1;
	n->isdir = (n->cat.hFileInfo.ioFlAttrib & MP_ATTRIB_DIR) != 0;
	if (n->isdir)
		n->dirID = n->cat.dirInfo.ioDrDirID;
	/* The catalog has the exact (case-preserved) name. */
	return 0;
}

void
mp_node_spec(const struct mp_node *n, FSSpec *spec)
{
	spec->vRefNum = n->vRefNum;
	spec->parID = n->parID;
	memcpy(spec->name, n->name, n->name[0] + 1);
}

void
mp_cat_to_stat(const struct mp_node *n, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = (dev_t)(unsigned short)n->vRefNum;
	st->st_nlink = 1;
	st->st_blksize = 4096;
	if (n->vRefNum == 0) {			/* pseudo root */
		st->st_mode = S_IFDIR | 0555;
		st->st_ino = 1;
		return;
	}
	if (n->isdir) {
		st->st_mode = S_IFDIR | 0755;
		st->st_ino = (ino_t)n->dirID;
		st->st_nlink = 2;
		st->st_mtime = mp_mac_to_unix(n->cat.dirInfo.ioDrMdDat);
		st->st_ctime = st->st_mtime;
		st->st_atime = st->st_mtime;
		return;
	}
	st->st_mode = S_IFREG |
	    ((n->cat.hFileInfo.ioFlAttrib & 0x01) ? 0444 : 0644);
	st->st_ino = (ino_t)n->cat.hFileInfo.ioDirID;	/* file number */
	st->st_size = n->cat.hFileInfo.ioFlLgLen;
	st->st_blocks = (n->cat.hFileInfo.ioFlPyLen +
	    n->cat.hFileInfo.ioFlRPyLen) / 512;
	st->st_mtime = mp_mac_to_unix(n->cat.hFileInfo.ioFlMdDat);
	st->st_ctime = st->st_mtime;
	st->st_atime = st->st_mtime;
}

/* Absolute, normalized form of path (need not exist). */
int
mp_abspath(const char *path, char *out)
{
	if (init_cwd() == -1)
		return -1;
	return normalize(cwd.path, path, out);
}

/* ---- current directory ---------------------------------------------- */

int
mp_chdir(const char *path)
{
	struct mp_node n;

	if (mp_resolve(AT_FDCWD, path, &n) == -1)
		return -1;
	if (!n.exists) {
		errno = ENOENT;
		return -1;
	}
	if (!n.isdir || n.vRefNum == 0) {
		errno = ENOTDIR;
		return -1;
	}
	cwd.vRefNum = n.vRefNum;
	cwd.dirID = n.dirID;
	strcpy(cwd.path, n.path);
	return 0;
}

int
mp_fchdir(int fd)
{
	struct mp_fd *f;

	if ((f = mp_fdget(fd)) == NULL)
		return -1;
	if (f->type != MPF_DIR) {
		errno = ENOTDIR;
		return -1;
	}
	return mp_chdir(f->node->path);
}

char *
mp_getcwd(char *buf, size_t size)
{
	if (init_cwd() == -1)
		return NULL;
	if (buf == NULL) {
		if ((buf = malloc(PATH_MAX)) == NULL)
			return NULL;
		size = PATH_MAX;
	}
	if (strlen(cwd.path) + 1 > size) {
		errno = ERANGE;
		return NULL;
	}
	strcpy(buf, cwd.path);
	return buf;
}

char *
mp_realpath(const char *path, char *resolved)
{
	struct mp_node n;

	if (mp_resolve(AT_FDCWD, path, &n) == -1)
		return NULL;
	if (!n.exists) {
		errno = ENOENT;
		return NULL;
	}
	if (resolved == NULL && (resolved = malloc(PATH_MAX)) == NULL)
		return NULL;
	strcpy(resolved, n.path);
	return resolved;
}
