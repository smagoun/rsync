/*
 * mpint.h: internals shared by the macposix sources.
 */
#ifndef MPINT_H
#define MPINT_H

#define MACPOSIX_IMPL 1
#include "macposix.h"

#include <Multiverse.h>
#include <errno.h>

#include "hfsname.h"

/* Constants missing from the Multiversal Interfaces. */
#define fsRtParID		1L
#define fsRtDirID		2L
#define kOnSystemDisk		((short)0x8000)
#define kDontCreateFolder	false
#define afpAccessDenied		(-5000)

/* ---- tracing (front end: "trace on") ---- */

extern int mp_tracing;
void	mp_trace(const char *, ...);
#define MPTRACE(...) do { if (mp_tracing) mp_trace(__VA_ARGS__); } while (0)

/* ---- errors ---------------------------------------------------------- */

/* Set errno from an OSErr and return -1 (or the OSErr if 0 -> 0). */
int	mp_oserr(OSErr);

/* Flush and close a resource fork we wrote (fd.c explains why). */
OSErr	mp_rf_close(short);

/* ---- resolved paths -------------------------------------------------- */

struct mp_node {
	short		vRefNum;
	long		parID;		/* folder that holds the item */
	Str63		name;		/* HFS name in that folder (Pascal) */
	int		exists;
	int		isdir;
	long		dirID;		/* if isdir */
	char		path[PATH_MAX];	/* absolute, normalized, UTF-8 */
	CInfoPBRec	cat;		/* catalog info if exists */
};

/*
 * Resolve a POSIX path (relative to dirfd, or AT_FDCWD) to an HFS item.
 * The parent folder must exist; the item itself need not (exists = 0).
 * Returns 0 or -1 with errno.
 */
int	mp_resolve(int dirfd, const char *path, struct mp_node *);
/* Re-read catalog info of a node (after it was created or changed). */
int	mp_refresh(struct mp_node *);
/* Fill struct stat from catalog info. */
void	mp_cat_to_stat(const struct mp_node *, struct stat *);
/* FSSpec for a node. */
void	mp_node_spec(const struct mp_node *, FSSpec *);

/* ---- descriptors ----------------------------------------------------- */

enum mp_fdtype { MPF_FREE, MPF_CONSOLE, MPF_FILE, MPF_DIR, MPF_SOCK };

struct mp_sock;

struct mp_fd {
	enum mp_fdtype	 type;
	int		 refs;		/* dup() shares the object */
	int		 flags;		/* O_* from open */
	short		 refNum;	/* MPF_FILE */
	struct mp_node	*node;		/* MPF_FILE, MPF_DIR */
	int		 delete_on_close; /* mp_tmpfd() */
	int		 detached;	/* HFS path closed by rename */
	struct mp_sock	*sock;		/* MPF_SOCK */
};

/* Shared object per descriptor slot (dup() points at the same one). */
struct mp_fd	*mp_fdget(int fd);		/* NULL + EBADF if bad */
int		 mp_fdalloc(struct mp_fd **);	/* lowest free >= 3 */
void		 mp_fdrelease(int fd);

/* ---- time ------------------------------------------------------------ */

time_t	mp_mac_to_unix(unsigned long macsecs);
unsigned long mp_unix_to_mac(time_t);

/* ---- directories ----------------------------------------------------- */

/* Item by index in a folder (1-based); fills node->name etc. */
int	mp_dir_index(short vRefNum, long dirID, short index, CInfoPBRec *,
	    Str63 name);

/* ---- network --------------------------------------------------------- */

ssize_t	mp_sock_read(struct mp_sock *, void *, size_t);
ssize_t	mp_sock_write(struct mp_sock *, const void *, size_t);
int	mp_sock_close(struct mp_sock *);
int	mp_sock_poll(struct mp_sock *, short events, short *revents);

/* Low-level console I/O provided by Retro68 (fd 0-2). */
ssize_t	_consolewrite(int, const void *, size_t);
ssize_t	_consoleread(int, void *, size_t);

#endif /* MPINT_H */
