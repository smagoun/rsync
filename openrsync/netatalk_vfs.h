/*
 * netatalk_vfs.h: route the *at() calls the receiver uses through the
 * --netatalk virtual "._name" layer (netatalk.c).  Included at the end of
 * extern.h; netatalk*.c define NETATALK_NO_REDIRECT to reach the real calls.
 * Each nt_*() is a plain pass-through unless --netatalk is active and the
 * path names a "._" sidecar.
 */
#if HAVE_NETATALK
#include "netatalk.h"
#endif
#if HAVE_NETATALK && !defined(NETATALK_NO_REDIRECT) && !defined(NETATALK_VFS_H)
#define NETATALK_VFS_H
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define fstatat(d, p, s, f)	nt_fstatat((d), (p), (s), (f))
#define openat(...)		nt_openat(__VA_ARGS__)
#define unlinkat(d, p, f)	nt_unlinkat((d), (p), (f))
#define utimensat(d, p, t, f)	nt_utimensat((d), (p), (t), (f))
#define fchmodat(d, p, m, f)	nt_fchmodat((d), (p), (m), (f))
#define fchownat(d, p, u, g, f)	nt_fchownat((d), (p), (u), (g), (f))
#endif
