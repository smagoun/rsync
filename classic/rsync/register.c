/*
 * register.c: hand the server's file list to hfsname.c so mangled and
 * case-colliding names map consistently both ways (see hfsname.c).
 */
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "extern.h"
#include "hfsname.h"

void	classic_register_names_impl(const char *, const struct flist *,
	    size_t);

void
classic_register_names_impl(const char *root, const struct flist *fl,
    size_t flsz)
{
	char base[PATH_MAX];
	char **paths;
	size_t i, n = 0, blen;

	if (mp_abspath(root, base) == -1)
		return;
	blen = strlen(base);
	if ((paths = calloc(flsz ? flsz : 1, sizeof(*paths))) == NULL)
		return;
	for (i = 0; i < flsz; i++) {
		if (strcmp(fl[i].path, ".") == 0)
			continue;
		if ((paths[n] = malloc(blen + strlen(fl[i].path) + 2)) == NULL)
			break;
		strcpy(paths[n], base);
		if (blen > 1)
			paths[n][blen] = '/', strcpy(paths[n] + blen + 1,
			    fl[i].path);
		else
			strcpy(paths[n] + blen, fl[i].path);
		n++;
	}
	hfs_names_register((const char *const *)paths, n);
	for (i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
}
