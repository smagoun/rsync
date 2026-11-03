/*
 * hfsname.h: rsync (UTF-8) <-> HFS (MacRoman, 31 bytes) file names.
 * See hfsname.c for the rules.  HFS names here are Pascal strings
 * (length byte first), as the File Manager takes them.
 */
#ifndef HFSNAME_H
#define HFSNAME_H

#include <stddef.h>

#define HFS_NAMEMAX	31

/* One name, no context.  Returns 0 if exact, 1 if mangled, -1 if empty. */
int	hfs_encode_name(const char *utf8, unsigned char out[HFS_NAMEMAX + 1]);
void	hfs_decode_name(const unsigned char *mac, size_t len, char *utf8,
	    size_t utf8sz);
/* Case-insensitive HFS comparison of two Pascal strings. */
int	hfs_name_equal(const unsigned char *, const unsigned char *);

/* Record the server's paths (rsync sort order) for rules 4-5. */
int	hfs_names_register(const char *const *paths, size_t n);
void	hfs_names_reset(void);
/* HFS name for the last component of a UTF-8 path ("dir/sub/name"). */
int	hfs_names_forward(const char *utf8path,
	    unsigned char out[HFS_NAMEMAX + 1]);
/* UTF-8 name for an HFS name found in directory utf8dir ("" or "dir/sub"). */
void	hfs_names_reverse(const char *utf8dir, const unsigned char *hfs,
	    char *utf8, size_t utf8sz);

#endif /* HFSNAME_H */
