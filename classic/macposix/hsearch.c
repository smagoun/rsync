/*
 * hsearch.c: hcreate/hsearch/hdestroy for Classic Mac OS.
 *
 * openrsync's --delete scan hashes every name in the file list.  newlib's
 * implementation (on top of its db hash) left about 32 bytes per entry
 * behind after hdestroy() -- harmless in a Unix process that exits,
 * a steady leak in the Mac application that runs many transfers.  This is
 * a plain open-addressing table that frees everything in hdestroy().
 * Like the POSIX interface, it does not own the keys.
 */
#define MACPOSIX_IMPL 1
#include "macposix.h"

#include <errno.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>

static ENTRY	*tab;
static size_t	 tabsz, used;

static size_t
hash(const char *s)
{
	unsigned long h = 2166136261UL;

	while (*s != '\0')
		h = (h ^ (unsigned char)*s++) * 16777619UL;
	return (size_t)h;
}

int
hcreate(size_t n)
{
	if (tab != NULL)
		return 0;
	for (tabsz = 16; tabsz < n * 2 + 1; tabsz *= 2)
		continue;
	if ((tab = calloc(tabsz, sizeof(*tab))) == NULL) {
		tabsz = 0;
		return 0;
	}
	used = 0;
	return 1;
}

void
hdestroy(void)
{
	free(tab);
	tab = NULL;
	tabsz = used = 0;
}

ENTRY *
hsearch(ENTRY item, ACTION action)
{
	size_t i;

	if (tab == NULL) {
		errno = ESRCH;
		return NULL;
	}
	for (i = hash(item.key) & (tabsz - 1); tab[i].key != NULL;
	    i = (i + 1) & (tabsz - 1))
		if (strcmp(tab[i].key, item.key) == 0)
			return &tab[i];
	if (action == FIND) {
		errno = ESRCH;
		return NULL;
	}
	if (used + 1 >= tabsz) {
		errno = ENOMEM;
		return NULL;
	}
	tab[i] = item;
	used++;
	return &tab[i];
}
