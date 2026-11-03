/*
 * dir.c: opendir/readdir on HFS folders.
 *
 * Names are converted back to UTF-8 with hfs_names_reverse(), so mangled
 * names of files the server sent map back to the server's spelling.
 * Invisible items (Finder flag) and "Icon\r" files are hidden: they are
 * Finder/system bookkeeping (Desktop DB, custom icons, Temporary Items)
 * that --delete must never remove.
 */
#include "mpint.h"

#include <stdlib.h>
#include <string.h>

#define kIsInvisible	0x4000

struct mp_dir {
	int		fd;
	short		index;
	struct dirent	ent;
};

int
mp_dir_index(short vRefNum, long dirID, short index, CInfoPBRec *pb,
    Str63 name)
{
	memset(pb, 0, sizeof(*pb));
	pb->hFileInfo.ioNamePtr = name;
	pb->hFileInfo.ioVRefNum = vRefNum;
	pb->hFileInfo.ioDirID = dirID;
	pb->hFileInfo.ioFDirIndex = index;
	return PBGetCatInfoSync(pb);
}

DIR *
mp_fdopendir(int fd)
{
	struct mp_fd *f;
	DIR *d;

	if ((f = mp_fdget(fd)) == NULL)
		return NULL;
	if (f->type != MPF_DIR) {
		errno = ENOTDIR;
		return NULL;
	}
	if ((d = calloc(1, sizeof(*d))) == NULL)
		return NULL;
	d->fd = fd;
	return d;
}

DIR *
mp_opendir(const char *path)
{
	DIR *d;
	int fd;

	if ((fd = mp_open(path, O_RDONLY | O_DIRECTORY)) == -1)
		return NULL;
	if ((d = mp_fdopendir(fd)) == NULL)
		mp_close(fd);
	return d;
}

static int
hidden(const CInfoPBRec *pb, const Str63 name)
{
	unsigned short flags;

	if (name[0] == 5 && memcmp(name + 1, "Icon\r", 5) == 0)
		return 1;
	if (pb->hFileInfo.ioFlAttrib & ioDirMask)
		flags = pb->dirInfo.ioDrUsrWds.frFlags;
	else
		flags = pb->hFileInfo.ioFlFndrInfo.fdFlags;
	return (flags & kIsInvisible) != 0;
}

struct dirent *
mp_readdir(DIR *d)
{
	struct mp_fd *f;
	struct mp_node *n;
	CInfoPBRec pb;
	Str63 name;
	char volname[256];
	HParamBlockRec vpb;
	const char *dirpath;

	if ((f = mp_fdget(d->fd)) == NULL)
		return NULL;
	n = f->node;

	if (n->vRefNum == 0) {		/* "/": list the volumes */
		memset(&vpb, 0, sizeof(vpb));
		vpb.volumeParam.ioNamePtr = name;
		vpb.volumeParam.ioVolIndex = ++d->index;
		if (PBHGetVInfoSync(&vpb) != noErr)
			return NULL;
		hfs_decode_name(name + 1, name[0], volname, sizeof(volname));
		snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", volname);
		d->ent.d_type = DT_DIR;
		d->ent.d_ino = (ino_t)(unsigned short)vpb.volumeParam.ioVRefNum;
		d->ent.d_namlen = strlen(d->ent.d_name);
		return &d->ent;
	}

	for (;;) {
		if (mp_dir_index(n->vRefNum, n->dirID, ++d->index, &pb, name)
		    != noErr)
			return NULL;	/* fnfErr: end of folder */
		if (!hidden(&pb, name))
			break;
	}
	/* hfs_names_reverse wants the folder path without the leading "/". */
	dirpath = n->path[0] == '/' ? n->path : n->path;
	hfs_names_reverse(dirpath, name, d->ent.d_name, sizeof(d->ent.d_name));
	d->ent.d_namlen = strlen(d->ent.d_name);
	if (pb.hFileInfo.ioFlAttrib & ioDirMask) {
		d->ent.d_type = DT_DIR;
		d->ent.d_ino = (ino_t)pb.dirInfo.ioDrDirID;
	} else {
		d->ent.d_type = DT_REG;
		d->ent.d_ino = (ino_t)pb.hFileInfo.ioDirID;
	}
	return &d->ent;
}

void
mp_rewinddir(DIR *d)
{
	d->index = 0;
}

int
mp_closedir(DIR *d)
{
	int rc;

	rc = mp_close(d->fd);
	free(d);
	return rc;
}

int
mp_dirfd(DIR *d)
{
	return d->fd;
}
