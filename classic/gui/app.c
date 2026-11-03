/*
 * app.c: the rsync application (Classic Mac OS).
 *
 * Main loop:
 *   1. a queued job (console command or bookmark) runs, start to finish;
 *   2. otherwise, if the server is wanted, "rsync --daemon" runs until
 *      something is queued (mp_listen_pause makes it return between
 *      connections) or the server is switched off;
 *   3. otherwise, ordinary WaitNextEvent.
 * While rsync runs (1 or 2), every event arrives through mp_yield() ->
 * app_event(), so the windows keep working.
 *
 * Unattended mode for tests: a file "rsync script" next to the app is run
 * line by line as console commands with output copied to "rsync log";
 * with "rsync autoquit" present too, the Mac shuts down afterwards.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gui.h"
#include "engine.h"

void	console_set_input(const char *);
extern short script_log;

enum { M_APPLE = 128, M_FILE, M_EDIT, M_SYNC, M_SERVER, M_WINDOW };
enum { F_NEW = 1, F_OPEN, F_SEP1, F_CLOSE, F_SEP2, F_QUIT };
enum { E_UNDO = 1, E_SEP, E_CUT, E_COPY, E_PASTE, E_CLEAR, E_ALL };
enum { S_SYNC = 1, S_PREVIEW, S_STOP, S_SEP, S_TOCONSOLE };
enum { V_TOGGLE = 1, V_LAUNCH };
enum { W_BOOKMARKS = 1, W_CONSOLE };

static MenuHandle menus[6];

/* ---- jobs ------------------------------------------------------------------ */

struct job {
	int	bookmark;	/* -1: a console line */
	int	preview;
	char	line[512];
};

#define NJOBS 8
static struct job jobs[NJOBS];
static int njobs;
static int job_running;		/* a user job (not the server) runs */
static int server_wanted, server_running, quit_requested;

static int
push_job(const struct job *j)
{
	if (njobs == NJOBS) {
		SysBeep(1);
		return -1;
	}
	jobs[njobs++] = *j;
	if (server_running)
		mp_listen_pause = 1;	/* let the server step aside */
	return 0;
}

void
app_queue_line(const char *line)
{
	struct job j;

	memset(&j, 0, sizeof(j));
	j.bookmark = -1;
	snprintf(j.line, sizeof(j.line), "%s", line);
	push_job(&j);
}

void
app_queue_bookmark(int index, int preview)
{
	struct job j;

	memset(&j, 0, sizeof(j));
	j.bookmark = index;
	j.preview = preview;
	push_job(&j);
}

void
app_set_server(int on)
{
	server_wanted = on;
	if (!on && server_running)
		mp_listen_pause = 1;
	app_update_menus();
}

int
app_server_running(void)
{
	return server_running;
}

/* "Today 9:41, exit 0" for the bookmark list. */
static void
stamp(struct bookmark *b, int rc, int preview)
{
	DateTimeRec d;
	unsigned long secs;

	GetDateTime(&secs);
	SecondsToDate(secs, &d);
	snprintf(b->last, sizeof(b->last), "%s %d/%d %d:%02d%s",
	    preview ? "previewed" : (rc == 0 ? "synced" : "failed"),
	    d.month, d.day, d.hour, d.minute, rc == 0 ? "" : " (see console)");
}

static int
run_bookmark_named(const char *name, int preview)
{
	int i;

	for (i = 0; i < nbookmarks; i++)
		if (strcmp(bookmarks[i].name, name) == 0) {
			app_queue_bookmark(i, preview);
			return 1;
		}
	console_printf("No bookmark named \"%s\".\r", name);
	return 0;
}

/*
 * Built-in commands that need the app (the console handles the rest).
 * None of them runs rsync, so the console calls this straight away,
 * without queueing (and without pausing the server).
 */
int
app_builtin(const char *line)
{
	int i;

	if (strcmp(line, "bookmarks") == 0) {
		for (i = 0; i < nbookmarks; i++)
			console_printf("  %s\r", bookmarks[i].name);
		if (nbookmarks == 0)
			console_printf("  (none)\r");
		return 1;
	}
	if (strncmp(line, "sync ", 5) == 0)
		return run_bookmark_named(line + 5, 0) || 1;
	if (strncmp(line, "preview ", 8) == 0)
		return run_bookmark_named(line + 8, 1) || 1;
	if (strcmp(line, "server on") == 0) {
		app_set_server(1);
		return 1;
	}
	if (strcmp(line, "server off") == 0) {
		app_set_server(0);
		return 1;
	}
	if (strcmp(line, "quit") == 0) {
		quit_requested = 1;
		mp_listen_pause = 1;
		return 1;
	}
	return engine_builtin(line);	/* set NAME=VALUE, trace on|off */
}

static void
run_job(struct job *j)
{
	char *argv[64], cmd[600];
	struct bookmark *b;
	int argc, rc;

	job_running = 1;
	app_update_menus();
	if (j->bookmark < 0) {
		if (!app_builtin(j->line))	/* scripts queue builtins too */
			engine_run_line(j->line);
	} else if (j->bookmark < nbookmarks) {
		b = &bookmarks[j->bookmark];
		bm_cmdline(b, j->preview, cmd, sizeof(cmd));
		console_show();
		console_printf("\r%s %s\r%s\r", j->preview ? "Preview of" :
		    "Syncing", b->name, cmd);
		argc = bm_argv(b, j->preview, argv, 64);
		if (argc == -2)
			console_printf("The local folder \"%s\" was not found.\r",
			    b->local);
		else if (argc < 0)
			console_printf("The bookmark is too long to run.\r");
		else {
			rc = engine_run_argv(argc, argv);
			console_printf("[%s %s: exit %d]\r", b->name,
			    j->preview ? "preview" : "sync", rc);
			stamp(b, rc, j->preview);
			bm_save();
			bookmarks_refresh();
		}
	}
	job_running = 0;
	app_update_menus();
}

static void
run_server(void)
{
	struct stat st;
	unsigned long t0;
	int rc;

	if (stat("rsyncd.conf", &st) != 0) {
		console_printf("Server: no \"rsyncd.conf\" next to the "
		    "application, so nothing to serve.\r");
		server_wanted = 0;
		app_update_menus();
		return;
	}
	server_running = 1;
	mp_listen_pause = 0;
	bookmarks_server_changed();
	app_update_menus();
	t0 = TickCount();
	rc = engine_run_line("rsync --daemon");
	server_running = 0;
	mp_listen_pause = 0;
	/* Failing straight away (e.g. port in use): don't spin on it. */
	if (rc != 0 && TickCount() - t0 < 120) {
		console_printf("Server: could not start (exit %d).\r", rc);
		server_wanted = 0;
	}
	bookmarks_server_changed();
	app_update_menus();
}

/* ---- menus ------------------------------------------------------------------ */

static void
make_menus(void)
{
	menus[0] = NewMenu(M_APPLE, "\p\x14");
	AppendMenu(menus[0], "\pAbout rsync\xc9;(-");
	AppendResMenu(menus[0], 'DRVR');
	menus[1] = NewMenu(M_FILE, "\pFile");
	AppendMenu(menus[1], "\pNew Bookmark/N;Open Bookmark/O;(-;Close/W;(-;"
	    "Quit/Q");
	menus[2] = NewMenu(M_EDIT, "\pEdit");
	AppendMenu(menus[2], "\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;"
	    "Select All/A");
	menus[3] = NewMenu(M_SYNC, "\pSync");
	AppendMenu(menus[3], "\pSync Now/R;Preview/P;Stop/.;(-;"
	    "Copy Command to Console");
	menus[4] = NewMenu(M_SERVER, "\pServer");
	AppendMenu(menus[4], "\pStart Server;Start Server at Launch");
	menus[5] = NewMenu(M_WINDOW, "\pWindow");
	AppendMenu(menus[5], "\pBookmarks/1;Console/2");
	for (int i = 0; i < 6; i++)
		InsertMenu(menus[i], 0);
	DrawMenuBar();
}

static void
enable(MenuHandle m, int item, int on)
{
	if (on)
		EnableItem(m, item);
	else
		DisableItem(m, item);
}

void
app_update_menus(void)
{
	int sel = bookmarks_selected();
	Str255 t;

	if (menus[3] == NULL)
		return;
	enable(menus[3], S_SYNC, sel >= 0 && !job_running);
	enable(menus[3], S_PREVIEW, sel >= 0 && !job_running);
	enable(menus[3], S_STOP, job_running);
	enable(menus[3], S_TOCONSOLE, sel >= 0);
	enable(menus[1], F_OPEN, sel >= 0);
	c2p(server_wanted ? "Stop Server" : "Start Server", t);
	SetMenuItemText(menus[4], V_TOGGLE, t);
	CheckMenuItem(menus[4], V_LAUNCH, pref_start_server);
}

static void
close_front(void)
{
	WindowPtr w = FrontWindow();

	if (w == NULL)
		return;
	if (editor_is(w))
		editor_close();
	else if (((WindowPeek)w)->windowKind < 0)
		CloseDeskAcc(((WindowPeek)w)->windowKind);
	else
		HideWindow(w);
}

static void
about(void)
{
	app_ask("rsync for Mac OS\r\rBookmarks pull or push folders with "
	    "an rsync server, keeping resource forks.  The console takes "
	    "rsync commands; the server lets other computers push to this "
	    "Mac.\r\rBased on openrsync (ISC licence).", "OK", NULL);
}

static void
do_menu(long choice)
{
	short menu = HiWord(choice), item = LoWord(choice);
	WindowPtr front = FrontWindow();
	char cmd[600];
	Str255 name;
	int sel = bookmarks_selected();

	switch (menu) {
	case M_APPLE:
		if (item == 1)
			about();
		else {
			GetMenuItemText(menus[0], item, name);
			OpenDeskAcc(name);
		}
		break;
	case M_FILE:
		if (item == F_NEW)
			editor_open(-1);
		else if (item == F_OPEN && sel >= 0)
			editor_open(sel);
		else if (item == F_CLOSE)
			close_front();
		else if (item == F_QUIT) {
			quit_requested = 1;
			mp_listen_pause = 1;
			if (job_running)
				mp_interrupted = 1;
		}
		break;
	case M_EDIT:
		if (SystemEdit(item - 1))
			break;
		if (editor_is(front)) {
			if (item >= E_CUT && item <= E_CLEAR)
				editor_edit(item - E_CUT);
		} else if (console_is(front))
			console_edit(item);
		break;
	case M_SYNC:
		if ((item == S_SYNC || item == S_PREVIEW) && sel >= 0)
			app_queue_bookmark(sel, item == S_PREVIEW);
		else if (item == S_STOP && job_running)
			mp_interrupted = 1;
		else if (item == S_TOCONSOLE && sel >= 0) {
			bm_cmdline(&bookmarks[sel], 0, cmd, sizeof(cmd));
			console_set_input(cmd);
		}
		break;
	case M_SERVER:
		if (item == V_TOGGLE)
			app_set_server(!server_wanted);
		else if (item == V_LAUNCH) {
			pref_start_server = !pref_start_server;
			bm_save();
			app_update_menus();
		}
		break;
	case M_WINDOW:
		if (item == W_BOOKMARKS)
			bookmarks_open();
		else
			console_show();
		break;
	}
	HiliteMenu(0);
}

/* ---- events ----------------------------------------------------------------- */

static void
window_event(WindowPtr w, EventRecord *ev)
{
	if (bookmarks_is(w))
		bookmarks_event(ev);
	else if (console_is(w))
		console_event(ev);
	else if (editor_is(w))
		editor_event(ev);
}

static void
activate(WindowPtr w, int on)
{
	if (bookmarks_is(w))
		bookmarks_activate(on);
	else if (console_is(w))
		console_activate(on);
	app_update_menus();
}

void
app_event(EventRecord *ev)
{
	WindowPtr w;
	short part;
	char key;

	switch (ev->what) {
	case nullEvent:
		console_idle();
		if (editor_is(FrontWindow()))
			editor_event(ev);	/* caret blink */
		break;
	case mouseDown:
		part = FindWindow(ev->where, &w);
		switch (part) {
		case inMenuBar:
			app_update_menus();
			do_menu(MenuSelect(ev->where));
			break;
		case inSysWindow:
			SystemClick(ev, w);
			break;
		case inDrag:
			DragWindow(w, ev->where, &qd.screenBits.bounds);
			break;
		case inGoAway:
			if (TrackGoAway(w, ev->where)) {
				if (editor_is(w))
					editor_close();
				else
					HideWindow(w);
			}
			break;
		case inGrow:
			if (bookmarks_is(w))
				bookmarks_grow(ev->where);
			else if (console_is(w))
				console_grow(ev->where);
			break;
		case inContent:
			if (w != FrontWindow())
				SelectWindow(w);
			else
				window_event(w, ev);
			break;
		}
		break;
	case keyDown:
	case autoKey:
		key = (char)(ev->message & charCodeMask);
		if (ev->modifiers & cmdKey) {
			if (key == '.') {
				/* Stops a sync; the idle server is not touched. */
				if (job_running)
					mp_interrupted = 1;
				break;
			}
			if (ev->what == keyDown) {
				app_update_menus();
				do_menu(MenuKey(key));
			}
			break;
		}
		if (FrontWindow() != NULL)
			window_event(FrontWindow(), ev);
		break;
	case updateEvt:
		w = (WindowPtr)ev->message;
		if (bookmarks_is(w))
			bookmarks_update();
		else if (console_is(w))
			console_update();
		else if (editor_is(w))
			editor_event(ev);
		break;
	case activateEvt:
		w = (WindowPtr)ev->message;
		if (editor_is(w))
			editor_event(ev);
		else
			activate(w, (ev->modifiers & activeFlag) != 0);
		break;
	case osEvt:
		/* Suspend/resume: treat as (de)activating the front window. */
		if (((ev->message >> 24) & 0xFF) == 1 && FrontWindow() != NULL) {
			if (ev->message & 1)
				gui_TEFromScrap();
			activate(FrontWindow(), (ev->message & 1) != 0);
		}
		break;
	}
}

/* ---- small dialogs ------------------------------------------------------------ */

Handle
ditl_new(void)
{
	Handle h = NewHandle(2);

	(*(short **)h)[0] = -1;		/* item count - 1 */
	return h;
}

int
ditl_add(Handle h, int type, short top, short left, short bottom,
    short right, const char *text)
{
	size_t len = text ? strlen(text) : 0, old = (size_t)GetHandleSize(h);
	size_t add;
	unsigned char *p;
	short *s;

	if (len > 255)
		len = 255;
	/* placeholder, rect, type, length byte, data padded to even */
	add = 4 + 8 + 1 + 1 + len + (len & 1);
	SetHandleSize(h, (Size)(old + add));
	p = (unsigned char *)*h + old;
	memset(p, 0, add);
	s = (short *)(p + 4);
	s[0] = top;
	s[1] = left;
	s[2] = bottom;
	s[3] = right;
	p[12] = (unsigned char)type;
	p[13] = (unsigned char)len;
	if (len)
		memcpy(p + 14, text, len);
	return ++(*(short **)h)[0] + 1;
}

int
app_ask(const char *msg, const char *ok, const char *cancel)
{
	Handle items = ditl_new();
	DialogPtr d;
	short hit = 0, w = 340, h = 130;
	Rect r;
	Str255 t;

	ditl_add(items, DI_BUTTON, h - 32, w - 86, h - 12, w - 14, ok);
	if (cancel != NULL)
		ditl_add(items, DI_BUTTON, h - 32, w - 172, h - 12, w - 100,
		    cancel);
	ditl_add(items, DI_STATIC, 12, 14, h - 40, w - 14, msg);
	r.left = (qd.screenBits.bounds.right - w) / 2;
	r.top = 80;
	r.right = r.left + w;
	r.bottom = r.top + h;
	d = NewDialog(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0,
	    items);
	SetDialogDefaultItem(d, 1);
	if (cancel != NULL)
		SetDialogCancelItem(d, 2);
	(void)t;
	while (hit != 1 && hit != 2) {
		ModalDialog(NULL, &hit);
		if (cancel == NULL && hit != 1)
			hit = 0;
	}
	DisposeDialog(d);
	return hit;
}

void
app_note(const char *msg)
{
	app_ask(msg, "OK", NULL);
}

void
c2p(const char *c, Str255 p)
{
	size_t n = strlen(c);

	if (n > 255)
		n = 255;
	p[0] = (unsigned char)n;
	memcpy(p + 1, c, n);
}

void
p2c(const unsigned char *p, char *c, size_t sz)
{
	size_t n = p[0] < sz ? p[0] : sz - 1;

	memcpy(c, p + 1, n);
	c[n] = '\0';
}

/* ---- main ----------------------------------------------------------------------- */

static int
read_line(FILE *f, char *buf, size_t len)
{
	size_t n = 0;
	int c;

	while ((c = getc(f)) != EOF && c != '\r' && c != '\n')
		if (n + 1 < len)
			buf[n++] = (char)c;
	buf[n] = '\0';
	return (c == EOF && n == 0) ? -1 : 0;
}

int
main(void)
{
	EventRecord ev;
	struct stat st;
	FILE *script;
	char line[512];
	int scripted = 0;

	engine_grow_stack(256L * 1024);
	InitGraf(&qd.thePort);
	InitFonts();
	InitWindows();
	InitMenus();
	TEInit();
	InitDialogs(NULL);
	InitCursor();
	FlushEvents(everyEvent, 0);
	gui_TEFromScrap();

	make_menus();
	bm_load();
	console_open();
	bookmarks_open();
	server_wanted = pref_start_server;
	app_update_menus();
	mp_event_hook = app_event;

	if ((script = fopen("rsync script", "r")) != NULL) {
		scripted = 1;
		FSDelete("\prsync log", 0);
		if (Create("\prsync log", 0, 'ttxt', 'TEXT') != noErr ||
		    FSOpen("\prsync log", 0, &script_log) != noErr)
			script_log = 0;
		while (read_line(script, line, sizeof(line)) != -1)
			if (line[0] != '\0' && line[0] != '#')
				app_queue_line(line);
		fclose(script);
		server_wanted = 0;	/* a script says "server on" if it wants */
	}

	while (!quit_requested) {
		if (njobs > 0) {
			struct job j = jobs[0];

			memmove(jobs, jobs + 1, sizeof(jobs[0]) * (size_t)--njobs);
			if (j.bookmark < 0 && scripted)
				console_printf("rsync> %s\r", j.line);
			run_job(&j);
			continue;
		}
		if (scripted && script_log != 0 && !server_wanted) {
			FSClose(script_log);
			script_log = 0;
			FlushVol(NULL, 0);
			if (stat("rsync autoquit", &st) == 0)
				ShutDwnPower();
		}
		if (server_wanted) {
			run_server();
			continue;
		}
		if (WaitNextEvent(everyEvent, &ev, 20, NULL) ||
		    ev.what == nullEvent)
			app_event(&ev);
	}
	return 0;
}
