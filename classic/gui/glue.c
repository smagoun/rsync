/*
 * glue.c: TEFromScrap/TEToScrap and DialogCut/Copy/Paste/Delete, written
 * from Inside Macintosh.  They're library glue, not traps, and Retro68's
 * Multiversal headers declare them with nothing behind them.  Our own
 * names (gui_*) keep them from clashing with Apple's Universal Interfaces,
 * which do provide them.
 */
#include "gui.h"

/* The TextEdit private scrap <-> the desk scrap ('TEXT'). */
OSErr
gui_TEFromScrap(void)
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

OSErr
gui_TEToScrap(void)
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

void
gui_DialogCut(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		TECut(te);
		ZeroScrap();
		gui_TEToScrap();
	}
}

void
gui_DialogCopy(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		TECopy(te);
		ZeroScrap();
		gui_TEToScrap();
	}
}

void
gui_DialogPaste(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL) {
		gui_TEFromScrap();
		TEPaste(te);
	}
}

void
gui_DialogDelete(DialogPtr d)
{
	TEHandle te = dialog_te(d);

	if (te != NULL)
		TEDelete(te);
}
