/*
 * netatalk.h: --netatalk mode.
 *
 * A netatalk 3/4 volume ("ea = sys") keeps each file's resource fork and
 * Finder info in an AppleDouble sidecar "._name" beside the data file.  A
 * stock rsync daemon sends those sidecars as ordinary files.  In --netatalk
 * mode the receiver presents every local file's resource fork and Finder
 * info as a *virtual* "._name" file in exactly that layout, so the normal
 * rsync quick check, delta transfer and --delete logic work unchanged:
 *
 *  - fstatat("._foo") reports size 82 + resource fork length and, as mtime,
 *    the sidecar's server mtime (kept separately from foo's own mtime);
 *  - openat("._foo", O_RDONLY) returns the packed AppleDouble (delta basis);
 *  - a received "._foo" is unpacked into foo's resource fork and Finder
 *    info instead of being stored (platform_move_file);
 *  - unlinkat("._foo") clears foo's resource fork and Finder info.
 *
 * Where forks are stored is up to a backend (struct nt_meta functions
 * below): netatalk_posix.c keeps them in Basilisk II / Retro68 style
 * ".rsrc/" and ".finf/" folders for testing on Unix; the Classic Mac OS
 * build uses the real HFS resource fork, FInfo/FXInfo and backup date.
 */
#ifndef NETATALK_H
#define NETATALK_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

/* AppleDouble v2 as written by netatalk 3/4 ("._name"). */
#define AD_MAGIC		0x00051607
#define AD_VERSION2		0x00020000
#define AD_ENTRY_RFORK		2
#define AD_ENTRY_FINDERI	9
#define AD_FINDERI_LEN		32
#define AD_NETATALK_HDRLEN	82	/* 26 + 2 * 12 + 32 */

struct ad_info {
	uint8_t		finfo[AD_FINDERI_LEN];
	int		has_finfo;
	uint32_t	rsrc_off;
	uint32_t	rsrc_len;
};

/* Parse an AppleDouble header from buf (len bytes, file size fsize). */
int	ad_parse(const uint8_t *buf, size_t len, off_t fsize,
	    struct ad_info *);
/* Write the 82-byte netatalk-style header for the given fork length. */
void	ad_build_header(uint8_t out[AD_NETATALK_HDRLEN],
	    const uint8_t finfo[AD_FINDERI_LEN], uint32_t rsrc_len);

/* Metadata of one local file, as provided by a backend. */
struct nt_meta {
	uint8_t		finfo[AD_FINDERI_LEN];
	uint32_t	rsrc_len;
	time_t		sidecar_mtime;	/* 0: unknown, use base mtime */
};

/*
 * Backend interface (netatalk_posix.c or the Classic Mac OS layer).
 * Paths are relative to dirfd and name the *base* file ("dir/foo").
 * All return 0 on success, -1 with errno set on failure.
 */
int	ntb_get(int dirfd, const char *path, struct nt_meta *);
ssize_t	ntb_read_rsrc(int dirfd, const char *path, void *, size_t, off_t);
int	ntb_set(int dirfd, const char *path,
	    const uint8_t finfo[AD_FINDERI_LEN], int srcfd, off_t rsrc_off,
	    uint32_t rsrc_len, time_t sidecar_mtime);
int	ntb_set_sidecar_mtime(int dirfd, const char *path, time_t);
int	ntb_clear(int dirfd, const char *path);
void	ntb_base_unlinked(int dirfd, const char *path);
/* Extra filter rules the backend needs (NULL-terminated), e.g. protect. */
extern const char *const ntb_rules[];

/* Mode switch; set from --netatalk. */
extern int netatalk_active;

int	netatalk_is_sidecar(const char *path);
int	netatalk_add_rules(void);

/* Virtual "._name" file-system entry points (see netatalk_vfs.h). */
int	nt_fstatat(int, const char *, struct stat *, int);
int	nt_openat(int, const char *, int, ...);
int	nt_unlinkat(int, const char *, int);
int	nt_utimensat(int, const char *, const struct timespec [2], int);
int	nt_fchmodat(int, const char *, mode_t, int);
int	nt_fchownat(int, const char *, uid_t, gid_t, int);

/* --mac-text: LF/CR swapped on read and write of matching files. */
ssize_t	nt_read(int, void *, size_t);
ssize_t	nt_pread(int, void *, size_t, off_t);
ssize_t	nt_write(int, const void *, size_t);
ssize_t	nt_pwrite(int, const void *, size_t, off_t);
int	nt_close(int);

/* mactypes.c: --mac-types and --mac-text. */
extern int mac_types_active;
extern int mac_text_active;
int	mactypes_load(const char *);
void	mactypes_reset(void);
int	mactypes_lookup(const char *, uint8_t [8]);
int	mactext_set(const char *);
int	mactext_match(const char *);

/* Platform hooks used by platform.c when netatalk_active. */
struct sess;
struct flist;
struct fl;
int	netatalk_flist_modify(const struct sess *, struct fl *);
void	netatalk_flist_received(struct sess *, struct flist *, size_t);
void	netatalk_clear_stale(struct sess *, int, struct flist *, size_t);
int	netatalk_move_file(const struct sess *, struct flist *, int,
	    const char *, int, const char *, int, int);

#endif /* NETATALK_H */
