/*
 * meta.c: --netatalk storage backend on HFS (see openrsync/netatalk.h).
 *
 *	resource fork		the file's real resource fork
 *	Finder info		FInfo + FXInfo (folders: DInfo + DXInfo)
 *	sidecar mtime		the HFS backup date (unused on a client)
 *
 * Writing the fork leaves the data fork's modification date unchanged.
 */
#include "mpint.h"

#include <string.h>

#include "netatalk.h"

#define kHasBeenInited	0x0100

const char *const ntb_rules[] = { NULL };

static int
node(int dirfd, const char *path, struct mp_node *n)
{
	if (mp_resolve(dirfd, path, n) == -1)
		return -1;
	if (!n->exists) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

int
ntb_get(int dirfd, const char *path, struct nt_meta *m)
{
	struct mp_node n;
	unsigned long bk;

	memset(m, 0, sizeof(*m));
	if (node(dirfd, path, &n) == -1)
		return -1;
	if (n.vRefNum == 0 || n.parID == fsRtParID)
		return 0;
	if (n.isdir) {
		memcpy(m->finfo, &n.cat.dirInfo.ioDrUsrWds, 16);
		memcpy(m->finfo + 16, &n.cat.dirInfo.ioDrFndrInfo, 16);
		bk = n.cat.dirInfo.ioDrBkDat;
	} else {
		memcpy(m->finfo, &n.cat.hFileInfo.ioFlFndrInfo, 16);
		memcpy(m->finfo + 16, &n.cat.hFileInfo.ioFlXFndrInfo, 16);
		m->rsrc_len = (uint32_t)n.cat.hFileInfo.ioFlRLgLen;
		bk = n.cat.hFileInfo.ioFlBkDat;
	}
	m->sidecar_mtime = bk ? mp_mac_to_unix(bk) : 0;
	return 0;
}

/* One cached open resource fork for sequential reads. */
static struct {
	short	refNum;
	short	vRefNum;
	long	parID;
	Str63	name;
} rcache;

static void
rcache_close(void)
{
	if (rcache.refNum) {
		FSClose(rcache.refNum);
		rcache.refNum = 0;
	}
}

ssize_t
ntb_read_rsrc(int dirfd, const char *path, void *buf, size_t len, off_t off)
{
	struct mp_node n;
	FSSpec spec;
	long cnt = (long)len;
	OSErr err;

	if (node(dirfd, path, &n) == -1)
		return -1;
	if (!rcache.refNum || rcache.vRefNum != n.vRefNum ||
	    rcache.parID != n.parID ||
	    memcmp(rcache.name, n.name, n.name[0] + 1) != 0) {
		rcache_close();
		mp_node_spec(&n, &spec);
		if ((err = FSpOpenRF(&spec, fsRdPerm, &rcache.refNum)) != noErr) {
			rcache.refNum = 0;
			return mp_oserr(err);
		}
		rcache.vRefNum = n.vRefNum;
		rcache.parID = n.parID;
		memcpy(rcache.name, n.name, n.name[0] + 1);
	}
	if ((err = SetFPos(rcache.refNum, fsFromStart, (long)off)) != noErr &&
	    err != eofErr)
		return mp_oserr(err);
	err = FSRead(rcache.refNum, &cnt, buf);
	if (err != noErr && err != eofErr)
		return mp_oserr(err);
	return cnt;
}

/* Write Finder info, keeping the modification date; set the backup date. */
static int
set_info(struct mp_node *n, const uint8_t finfo[32], unsigned long mddat,
    time_t sidecar_mtime)
{
	OSErr err;

	if (mp_refresh(n) == -1)
		return -1;
	n->cat.hFileInfo.ioNamePtr = n->name;
	n->cat.hFileInfo.ioDirID = n->parID;
	if (n->isdir) {
		memcpy(&n->cat.dirInfo.ioDrUsrWds, finfo, 16);
		memcpy(&n->cat.dirInfo.ioDrFndrInfo, finfo + 16, 16);
		n->cat.dirInfo.ioDrUsrWds.frFlags &= ~kHasBeenInited;
		n->cat.dirInfo.ioDrBkDat = mp_unix_to_mac(sidecar_mtime);
		if (mddat)
			n->cat.dirInfo.ioDrMdDat = mddat;
	} else {
		memcpy(&n->cat.hFileInfo.ioFlFndrInfo, finfo, 16);
		memcpy(&n->cat.hFileInfo.ioFlXFndrInfo, finfo + 16, 16);
		/* Let the Finder register the icons (BNDL) of a new app. */
		n->cat.hFileInfo.ioFlFndrInfo.fdFlags &= ~kHasBeenInited;
		n->cat.hFileInfo.ioFlBkDat = mp_unix_to_mac(sidecar_mtime);
		if (mddat)
			n->cat.hFileInfo.ioFlMdDat = mddat;
	}
	if ((err = PBSetCatInfoSync(&n->cat)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
ntb_set(int dirfd, const char *path, const uint8_t finfo[AD_FINDERI_LEN],
    int srcfd, off_t rsrc_off, uint32_t rsrc_len, time_t sidecar_mtime)
{
	static char buf[8192];
	struct mp_node n;
	FSSpec spec;
	unsigned long mddat;
	uint32_t done;
	ssize_t got;
	long cnt;
	short ref;
	OSErr err;

	rcache_close();
	if (mp_resolve(dirfd, path, &n) == -1)
		return -1;
	mp_node_spec(&n, &spec);
	if (!n.exists) {
		/* The sidecar may arrive for a base we do not have yet. */
		if ((err = FSpCreate(&spec, 0, 0, smSystemScript))
		    != noErr)
			return mp_oserr(err);
		if (mp_refresh(&n) == -1)
			return -1;
	}
	mddat = n.isdir ? n.cat.dirInfo.ioDrMdDat : n.cat.hFileInfo.ioFlMdDat;

	if (!n.isdir) {
		if ((err = FSpOpenRF(&spec, fsRdWrPerm, &ref)) != noErr)
			return mp_oserr(err);
		err = SetEOF(ref, (long)rsrc_len);
		for (done = 0; err == noErr && done < rsrc_len; done += got) {
			got = mp_pread(srcfd, buf, rsrc_len - done < sizeof(buf) ?
			    rsrc_len - done : sizeof(buf), rsrc_off + done);
			if (got <= 0) {
				err = ioErr;
				break;
			}
			cnt = got;
			err = FSWrite(ref, &cnt, buf);
			mp_yield();
		}
		if (FSClose(ref) != noErr && err == noErr)
			err = ioErr;
		if (err != noErr)
			return mp_oserr(err);
	}
	return set_info(&n, finfo, mddat, sidecar_mtime);
}

int
ntb_set_sidecar_mtime(int dirfd, const char *path, time_t t)
{
	struct mp_node n;
	OSErr err;

	if (node(dirfd, path, &n) == -1)
		return -1;
	n.cat.hFileInfo.ioNamePtr = n.name;
	n.cat.hFileInfo.ioDirID = n.parID;
	if (n.isdir)
		n.cat.dirInfo.ioDrBkDat = mp_unix_to_mac(t);
	else
		n.cat.hFileInfo.ioFlBkDat = mp_unix_to_mac(t);
	if ((err = PBSetCatInfoSync(&n.cat)) != noErr)
		return mp_oserr(err);
	return 0;
}

int
ntb_clear(int dirfd, const char *path)
{
	static const uint8_t zero[32];
	struct mp_node n;
	FSSpec spec;
	unsigned long mddat;
	short ref;
	OSErr err;

	rcache_close();
	if (node(dirfd, path, &n) == -1)
		return -1;
	mddat = n.isdir ? n.cat.dirInfo.ioDrMdDat : n.cat.hFileInfo.ioFlMdDat;
	if (!n.isdir && n.cat.hFileInfo.ioFlRLgLen > 0) {
		mp_node_spec(&n, &spec);
		if ((err = FSpOpenRF(&spec, fsRdWrPerm, &ref)) != noErr)
			return mp_oserr(err);
		err = SetEOF(ref, 0);
		FSClose(ref);
		if (err != noErr)
			return mp_oserr(err);
	}
	return set_info(&n, zero, mddat, 0);
}

void
ntb_base_unlinked(int dirfd, const char *path)
{
	/* HFS deletes both forks and the catalog entry together. */
	(void)dirfd;
	(void)path;
	rcache_close();
}
