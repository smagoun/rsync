/*
 * glue.c: Toolbox "glue" routines that are library code rather than traps
 * in Apple's interfaces, so the Multiversal headers declare them but no
 * library defines them.  Written from Inside Macintosh.
 */
#include <Multiverse.h>

/* The TextEdit private scrap <-> the desk scrap ('TEXT'). */
pascal OSErr
TEFromScrap(void)
{
	long off, len;
	Handle h = LMGetTEScrpHandle();

	if (h == NULL)
		return noErr;
	len = GetScrap(h, 'TEXT', &off);
	if (len < 0) {
		LMSetTEScrpLength(0);
		return (OSErr)len;
	}
	LMSetTEScrpLength((INTEGER)len);
	return noErr;
}

pascal OSErr
TEToScrap(void)
{
	Handle h = LMGetTEScrpHandle();
	long rc;

	if (h == NULL)
		return noErr;
	HLock(h);
	rc = PutScrap(LMGetTEScrpLength(), 'TEXT', *h);
	HUnlock(h);
	return rc < 0 ? (OSErr)rc : noErr;
}

/* Edit menu commands on a dialog's current edit text item. */
static TEHandle
dialog_te(DialogPtr d)
{
	return ((DialogPeek)d)->editField >= 0 ? ((DialogPeek)d)->textH : NULL;
}

pascal void
DialogCut(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		TECut(te);
		ZeroScrap();
		TEToScrap();
	}
}

pascal void
DialogCopy(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		TECopy(te);
		ZeroScrap();
		TEToScrap();
	}
}

pascal void
DialogPaste(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		TEFromScrap();
		TEPaste(te);
	}
}

pascal void
DialogDelete(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL)
		TEDelete(te);
}
