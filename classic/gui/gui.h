/*
 * gui.h: the rsync application's windows (Classic Mac OS Toolbox).
 *
 *   app.c        Toolbox set-up, menus, the event loop, the run queue and
 *                the always-on server
 *   console.c    the command window: output, an "rsync>" line, history
 *   bookmarks.c  bookmark model (the "rsync Bookmarks" file) and window
 *   editor.c     the window that reviews and edits one bookmark
 *
 * rsync itself runs synchronously from the main loop (engine.c); while it
 * waits, macposix's mp_yield() hands every event to app_event(), so the
 * windows keep working.  Nothing reached from app_event() may start
 * another rsync run: it queues one with app_queue_*() instead.
 */
#ifndef GUI_H
#define GUI_H

#include "mactoolbox.h"

/* ---- bookmarks ---------------------------------------------------------- */

struct bookmark {
	char	name[64];
	int	push;			/* 0: server -> Mac, 1: Mac -> server */
	char	host[128];
	int	port;
	char	module[64];
	char	path[256];		/* inside the module */
	char	user[64];
	char	pass[64];
	char	local[256];		/* HFS path, MacRoman: "Disk:Folder:" */
	int	netatalk;		/* resource forks via ._ files */
	int	xattrs;			/* Apple -E (server is a Mac) */
	int	del;			/* --delete */
	int	mactypes;
	int	mactext;
	char	mactext_exts[64];
	int	dst;			/* tolerate DST shifts */
	int	compress;
	int	checksum;
	char	exclude[256];		/* space-separated patterns */
	char	last[64];		/* "Today 9:41, 3 files" */
};

#define BM_MAX	64
extern struct bookmark	bookmarks[BM_MAX];
extern int		nbookmarks;
extern int		pref_start_server;

void	bm_defaults(struct bookmark *);
int	bm_load(void);
int	bm_save(void);
/* Arguments for running bm (preview: dry run with itemized output). */
int	bm_argv(const struct bookmark *, int preview, char **argv, int max);
/* The same as one typed line, for display and for the console. */
void	bm_cmdline(const struct bookmark *, int preview, char *out, size_t);

void	bookmarks_open(void);
void	bookmarks_refresh(void);
void	bookmarks_server_changed(void);
int	bookmarks_is(WindowPtr);
void	bookmarks_event(EventRecord *);
void	bookmarks_activate(int);
void	bookmarks_update(void);
void	bookmarks_grow(Point);
int	bookmarks_selected(void);	/* index or -1 */

/* ---- editor ------------------------------------------------------------- */

void	editor_open(int index);		/* -1: new bookmark */
int	editor_is(WindowPtr);
void	editor_event(EventRecord *);
void	editor_close(void);
int	editor_index(void);

/* ---- console ------------------------------------------------------------ */

void	console_open(void);
void	console_show(void);
int	console_is(WindowPtr);
void	console_event(EventRecord *);
void	console_activate(int);
void	console_update(void);
void	console_grow(Point);
void	console_idle(void);
void	console_write(const char *, size_t);	/* MacRoman text */
void	console_printf(const char *, ...);
void	console_edit(int menuitem);		/* Cut/Copy/Paste/Clear/All */

/* ---- app ---------------------------------------------------------------- */

void	app_event(EventRecord *);
void	app_queue_line(const char *);		/* a console command */
int	app_builtin(const char *);		/* run now if built in */
void	app_queue_bookmark(int index, int preview);
void	app_set_server(int on);
int	app_server_running(void);
void	app_update_menus(void);
void	app_note(const char *);			/* small alert */

/* Dialog item lists built in memory (no resources needed). */
enum { DI_BUTTON = 4, DI_CHECK = 5, DI_RADIO = 6, DI_STATIC = 8 | 128,
    DI_EDIT = 16, DI_USER = 0 | 128 };
Handle	ditl_new(void);
int	ditl_add(Handle, int type, short top, short left, short bottom,
	    short right, const char *text);	/* returns the item number */
/* Modal alert built from a DITL: returns the button pressed (1 = OK). */
int	app_ask(const char *msg, const char *ok, const char *cancel);

/* glue.c: scrap and dialog Edit-menu helpers (Inside Macintosh glue). */
OSErr	gui_TEFromScrap(void);
OSErr	gui_TEToScrap(void);
void	gui_DialogCut(DialogPtr);
void	gui_DialogCopy(DialogPtr);
void	gui_DialogPaste(DialogPtr);
void	gui_DialogDelete(DialogPtr);

/* Pascal string helpers. */
void	c2p(const char *, Str255);
void	p2c(const unsigned char *, char *, size_t);

#endif /* GUI_H */
