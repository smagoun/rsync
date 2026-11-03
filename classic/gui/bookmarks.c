/*
 * bookmarks.c: the bookmark list (like an FTP client's bookmarks).
 *
 * All bookmarks live in one text file, "rsync Bookmarks", next to the
 * application:
 *
 *	start server = 1
 *
 *	[MyApp sources]
 *	direction = pull
 *	host = buildbox.local
 *	module = src
 *	...
 *
 * It is plain MacRoman text with CR line ends, so it can be read and
 * edited in SimpleText.  Passwords are scrambled, not encrypted.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"
#include "engine.h"

#define BM_FILE	"rsync Bookmarks"

struct bookmark	bookmarks[BM_MAX];
int		nbookmarks;
int		pref_start_server = 1;

/* ---- model ---------------------------------------------------------------- */

void
bm_defaults(struct bookmark *b)
{
	memset(b, 0, sizeof(*b));
	strcpy(b->name, "untitled");
	b->port = 873;
	b->netatalk = 1;
	b->dst = 1;
	strcpy(b->mactext_exts, ".c .h .r .p");
}

static void
trim(char *s)
{
	char *e = s + strlen(s);

	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
}

static void
scramble(const char *in, char *out, size_t sz)
{
	size_t i, n = 0;

	for (i = 0; in[i] != '\0' && n + 3 < sz; i++)
		n += (size_t)snprintf(out + n, sz - n, "%02x",
		    (unsigned char)in[i] ^ 0x5a);
	out[n] = '\0';
}

static void
unscramble(const char *in, char *out, size_t sz)
{
	size_t n = 0;
	unsigned v;

	while (in[0] && in[1] && n + 1 < sz && sscanf(in, "%2x", &v) == 1) {
		out[n++] = (char)(v ^ 0x5a);
		in += 2;
	}
	out[n] = '\0';
}

static void
set_field(struct bookmark *b, const char *k, const char *v)
{
#define S(f)	snprintf(b->f, sizeof(b->f), "%s", v)
#define I(f)	(b->f = atoi(v))
	if (!strcmp(k, "direction"))		b->push = !strcmp(v, "push");
	else if (!strcmp(k, "host"))		S(host);
	else if (!strcmp(k, "port"))		I(port);
	else if (!strcmp(k, "module"))		S(module);
	else if (!strcmp(k, "path"))		S(path);
	else if (!strcmp(k, "user"))		S(user);
	else if (!strcmp(k, "password"))	unscramble(v, b->pass,
						    sizeof(b->pass));
	else if (!strcmp(k, "local"))		S(local);
	else if (!strcmp(k, "resource forks"))	I(netatalk);
	else if (!strcmp(k, "apple -E"))	I(xattrs);
	else if (!strcmp(k, "delete"))		I(del);
	else if (!strcmp(k, "mac types"))	I(mactypes);
	else if (!strcmp(k, "mac text"))	I(mactext);
	else if (!strcmp(k, "mac text extensions")) S(mactext_exts);
	else if (!strcmp(k, "dst window"))	I(dst);
	else if (!strcmp(k, "compress"))	I(compress);
	else if (!strcmp(k, "checksum"))	I(checksum);
	else if (!strcmp(k, "exclude"))		S(exclude);
	else if (!strcmp(k, "last"))		S(last);
#undef S
#undef I
}

int
bm_load(void)
{
	char *line = NULL, *eq, *k, *v;
	size_t cap = 0;
	struct bookmark *cur = NULL;
	FILE *f;

	nbookmarks = 0;
	if ((f = fopen(BM_FILE, "r")) == NULL)
		return 0;
	while (getline(&line, &cap, f) != -1) {
		trim(line);
		for (k = line; isspace((unsigned char)*k); k++)
			continue;
		if (*k == '\0' || *k == '#')
			continue;
		if (*k == '[') {
			if (nbookmarks == BM_MAX)
				break;
			cur = &bookmarks[nbookmarks++];
			bm_defaults(cur);
			snprintf(cur->name, sizeof(cur->name), "%s", k + 1);
			if ((eq = strrchr(cur->name, ']')) != NULL)
				*eq = '\0';
			continue;
		}
		if ((eq = strchr(k, '=')) == NULL)
			continue;
		*eq = '\0';
		v = eq + 1;
		trim(k);
		while (isspace((unsigned char)*v))
			v++;
		if (cur == NULL) {
			if (!strcmp(k, "start server"))
				pref_start_server = atoi(v);
		} else
			set_field(cur, k, v);
	}
	free(line);
	fclose(f);
	return 0;
}

int
bm_save(void)
{
	struct bookmark *b;
	char pw[sizeof(b->pass) * 2 + 1];
	FILE *f;
	int i;

	if ((f = fopen(BM_FILE, "w")) == NULL) {
		app_note("Could not save the bookmarks file \"" BM_FILE "\".");
		return -1;
	}
	fprintf(f, "# rsync bookmarks (edit with care)\r");
	fprintf(f, "start server = %d\r", pref_start_server);
	for (i = 0; i < nbookmarks; i++) {
		b = &bookmarks[i];
		scramble(b->pass, pw, sizeof(pw));
		fprintf(f, "\r[%s]\rdirection = %s\rhost = %s\rport = %d\r"
		    "module = %s\rpath = %s\ruser = %s\rpassword = %s\r"
		    "local = %s\rresource forks = %d\rapple -E = %d\r"
		    "delete = %d\rmac types = %d\rmac text = %d\r"
		    "mac text extensions = %s\rdst window = %d\r"
		    "compress = %d\rchecksum = %d\rexclude = %s\rlast = %s\r",
		    b->name, b->push ? "push" : "pull", b->host, b->port,
		    b->module, b->path, b->user, pw, b->local, b->netatalk,
		    b->xattrs, b->del, b->mactypes, b->mactext,
		    b->mactext_exts, b->dst, b->compress, b->checksum,
		    b->exclude, b->last);
	}
	fclose(f);
	return 0;
}

/* ---- turning a bookmark into a command ------------------------------------ */

static void
remote_url(const struct bookmark *b, char *out, size_t sz)
{
	const char *p = b->path;
	size_t n;

	while (*p == '/')
		p++;
	n = (size_t)snprintf(out, sz, "rsync://%s%s%s", b->user,
	    b->user[0] ? "@" : "", b->host);
	if (b->port != 0 && b->port != 873)
		n += (size_t)snprintf(out + n, sz - n, ":%d", b->port);
	n += (size_t)snprintf(out + n, sz - n, "/%s/%s", b->module, p);
	if (n > 0 && n < sz - 1 && out[n - 1] != '/')
		strcpy(out + n, "/");
}

static void
local_hfs(const struct bookmark *b, char *out, size_t sz)
{
	size_t n = (size_t)snprintf(out, sz, "%s", b->local);

	if (n > 0 && n < sz - 1 && out[n - 1] != ':')
		strcpy(out + n, ":");
}

/* Options common to the argv and the display form. */
static int
options(const struct bookmark *b, int preview, const char **o, char *exts,
    size_t extsz)
{
	int n = 0;
	char *c;

	o[n++] = "-av";
	if (preview) {
		o[n++] = "-n";
		o[n++] = "-i";
	}
	if (b->del)
		o[n++] = "--delete";
	if (!b->netatalk)
		o[n++] = "--no-netatalk";
	if (b->xattrs)
		o[n++] = "--extended-attributes";
	if (b->mactypes)
		o[n++] = "--mac-types";
	if (b->mactext) {
		snprintf(exts, extsz, "--mac-text=%s", b->mactext_exts);
		for (c = exts + 11; *c; c++)
			if (*c == ' ')
				*c = ',';
		o[n++] = exts;
	}
	if (!b->dst)
		o[n++] = "--no-dst-window";
	if (b->compress)
		o[n++] = "-z";
	if (b->checksum)
		o[n++] = "-c";
	return n;
}

int
bm_argv(const struct bookmark *b, int preview, char **argv, int max)
{
	static char pool[4096];
	char hfs[300], utf8[1024], *posix, exts[80], *p;
	const char *o[16], *pat, *end;
	size_t used = 0, len;
	int argc = 0, n, i;

#define KEEP(s) do {							\
	len = strlen(s) + 1;						\
	if (used + len > sizeof(pool) || argc >= max - 3)		\
		return -1;						\
	memcpy(pool + used, (s), len);					\
	argv[argc++] = pool + used;					\
	used += len;							\
} while (0)

	argv[argc++] = "rsync";
	n = options(b, preview, o, exts, sizeof(exts));
	for (i = 0; i < n; i++)
		KEEP(o[i]);
	for (pat = b->exclude; *pat; pat = end) {
		while (*pat == ' ')
			pat++;
		if (*pat == '\0')
			break;
		for (end = pat; *end && *end != ' '; end++)
			continue;
		snprintf(hfs, sizeof(hfs), "--exclude=%.*s", (int)(end - pat),
		    pat);
		engine_macroman_to_utf8(hfs, utf8, sizeof(utf8));
		KEEP(utf8);
	}

	/* Local folder: HFS (MacRoman) -> POSIX (UTF-8). */
	local_hfs(b, hfs, sizeof(hfs));
	engine_macroman_to_utf8(hfs, utf8, sizeof(utf8));
	if ((posix = engine_hfs_arg(utf8)) == NULL)
		return -2;
	/* Remote URL, UTF-8. */
	remote_url(b, hfs, sizeof(hfs));
	engine_macroman_to_utf8(hfs, utf8, sizeof(utf8));
	if (b->push) {
		p = posix;
		KEEP(p);
		KEEP(utf8);
	} else {
		KEEP(utf8);
		p = posix;
		KEEP(p);
	}
	free(posix);
	argv[argc] = NULL;

	if (b->pass[0])
		setenv("RSYNC_PASSWORD", b->pass, 1);
	else
		unsetenv("RSYNC_PASSWORD");
	return argc;
#undef KEEP
}

void
bm_cmdline(const struct bookmark *b, int preview, char *out, size_t sz)
{
	char url[400], loc[300], exts[80];
	const char *o[16], *pat, *end;
	size_t n;
	int i, cnt;

	remote_url(b, url, sizeof(url));
	local_hfs(b, loc, sizeof(loc));
	n = (size_t)snprintf(out, sz, "rsync");
	cnt = options(b, preview, o, exts, sizeof(exts));
	for (i = 0; i < cnt && n < sz; i++)
		n += (size_t)snprintf(out + n, sz - n, " %s", o[i]);
	for (pat = b->exclude; *pat && n < sz; pat = end) {
		while (*pat == ' ')
			pat++;
		if (*pat == '\0')
			break;
		for (end = pat; *end && *end != ' '; end++)
			continue;
		n += (size_t)snprintf(out + n, sz - n, " --exclude=%.*s",
		    (int)(end - pat), pat);
	}
	if (n < sz) {
		if (b->push)
			snprintf(out + n, sz - n, " \"%s\" %s", loc, url);
		else
			snprintf(out + n, sz - n, " %s \"%s\"", url, loc);
	}
}

/* ---- the window ------------------------------------------------------------ */

enum { B_NEW, B_EDIT, B_DELETE, B_PREVIEW, B_SYNC, B_SERVER, NBUTTONS };

static WindowPtr win;
static ListHandle list;
static ControlHandle buttons[NBUTTONS];
static const char *const button_titles[NBUTTONS] = {
	"New\xc9", "Edit\xc9", "Delete", "Preview", "Sync", "Stop Server"
};

int
bookmarks_is(WindowPtr w)
{
	return w != NULL && w == win;
}

int
bookmarks_selected(void)
{
	Cell c;

	if (list == NULL)
		return -1;
	c.h = c.v = 0;
	if (!LGetSelect(true, &c, list))
		return -1;
	return c.v < nbookmarks ? c.v : -1;
}

static void
layout(void)
{
	Rect r = win->portRect, lr, br;
	short x, w, bottom = r.bottom - 15, i;
	Str255 t;

	/* List: leave room for the scroll bar (15) and two button rows. */
	lr = r;
	lr.top += 10;
	lr.left += 10;
	lr.right -= 10 + 15;
	lr.bottom -= 78;
	if (list != NULL) {
		LSize(lr.right - lr.left, lr.bottom - lr.top, list);
		/* One column as wide as the list (fixed at LNew otherwise). */
		(*list)->cellSize.h = lr.right - lr.left;
	}
	/* First button row: New, Edit, Delete ... Preview, Sync. */
	x = 10;
	for (i = B_NEW; i <= B_SYNC; i++) {
		if (buttons[i] == NULL)
			continue;
		w = i == B_SYNC || i == B_PREVIEW ? 72 : 66;
		if (i == B_PREVIEW)
			x = r.right - 10 - 72 - 8 - 72;
		SetRect(&br, x, bottom - 50, x + w, bottom - 30);
		MoveControl(buttons[i], br.left, br.top);
		x += w + 8;
	}
	if (buttons[B_SERVER] != NULL)
		MoveControl(buttons[B_SERVER], r.right - 10 - 96, bottom - 18);
	(void)t;
}

static void
set_cell(int row, const struct bookmark *b)
{
	char text[300];
	Cell c;

	snprintf(text, sizeof(text), "%s  %s   %s%s%s::%s/%s%s%s",
	    b->push ? "\xc8" : "\xc7",		/* MacRoman >> and << */
	    b->name, b->user, b->user[0] ? "@" : "", b->host, b->module,
	    b->path, b->last[0] ? "   " : "", b->last);
	c.h = 0;
	c.v = row;
	LSetCell(text, (short)strlen(text), c, list);
}

void
bookmarks_refresh(void)
{
	int i, sel = bookmarks_selected();
	GrafPtr old;
	Cell c;

	if (win == NULL)
		return;
	GetPort(&old);
	SetPort(win);
	LSetDrawingMode(false, list);
	LDelRow(0, 0, list);
	if (nbookmarks > 0)
		LAddRow(nbookmarks, 0, list);
	for (i = 0; i < nbookmarks; i++)
		set_cell(i, &bookmarks[i]);
	if (sel >= nbookmarks)
		sel = nbookmarks - 1;
	if (sel >= 0) {
		c.h = 0;
		c.v = sel;
		LSetSelect(true, c, list);
	}
	LSetDrawingMode(true, list);
	InvalRect(&win->portRect);
	SetPort(old);
	app_update_menus();
}

void
bookmarks_server_changed(void)
{
	Str255 t;
	GrafPtr old;

	if (win == NULL)
		return;
	c2p(app_server_running() ? "Stop Server" : "Start Server", t);
	SetControlTitle(buttons[B_SERVER], t);
	GetPort(&old);
	SetPort(win);
	InvalRect(&win->portRect);
	SetPort(old);
}

void
bookmarks_open(void)
{
	Rect r, dataBounds;
	Point cell;
	Str255 t;
	int i;

	if (win != NULL) {
		ShowWindow(win);
		SelectWindow(win);
		return;
	}
	SetRect(&r, 10, 42, 10 + 520, 42 + 270);
	c2p("rsync Bookmarks", t);
	win = NewWindow(NULL, &r, t, true, documentProc, (WindowPtr)-1, true,
	    0);
	SetPort(win);
	TextFont(1);	/* application font (Geneva) */
	TextSize(10);
	SetRect(&r, 10, 10, 100, 100);
	SetRect(&dataBounds, 0, 0, 1, 0);
	cell.h = 0;
	cell.v = 0;
	list = LNew(&r, &dataBounds, cell, 0, win, true, false, false, true);
	(*list)->selFlags = lOnlyOne;
	for (i = 0; i < NBUTTONS; i++) {
		c2p(button_titles[i], t);
		SetRect(&r, 0, 0, i == B_SERVER ? 96 : (i >= B_PREVIEW ? 72 :
		    66), 20);
		buttons[i] = NewControl(win, &r, t, true, 0, 0, 1, pushButProc,
		    i);
	}
	layout();
	bookmarks_refresh();
	bookmarks_server_changed();
}

/*
 * The size box alone: DrawGrowIcon also draws the outlines of scroll bars
 * along the right and bottom edges, which this window doesn't have (the
 * list has its own), so clip to the box.
 */
static void
draw_grow_box(void)
{
	RgnHandle save = NewRgn();
	Rect r = win->portRect;

	GetClip(save);
	r.left = r.right - 15;
	r.top = r.bottom - 15;
	ClipRect(&r);
	DrawGrowIcon(win);
	SetClip(save);
	DisposeRgn(save);
}

static void
draw_status(void)
{
	Rect r = win->portRect;
	char s[160];
	Str255 t;
	unsigned long ip;

	if (app_server_running()) {
		ip = mp_local_ip();
		snprintf(s, sizeof(s), "Server on: rsync://%lu.%lu.%lu.%lu/",
		    ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	} else
		snprintf(s, sizeof(s), "Server off");
	c2p(s, t);
	MoveTo(12, r.bottom - 15 - 5);
	DrawString(t);
	/* Separator above the status line. */
	MoveTo(10, r.bottom - 15 - 24);
	LineTo(r.right - 10, r.bottom - 15 - 24);
}

void
bookmarks_update(void)
{
	GrafPtr old;
	Rect lr;

	GetPort(&old);
	SetPort(win);
	BeginUpdate(win);
	EraseRect(&win->portRect);
	lr = (*list)->rView;
	InsetRect(&lr, -1, -1);
	FrameRect(&lr);
	LUpdate(win->visRgn, list);
	DrawControls(win);
	draw_grow_box();
	draw_status();
	EndUpdate(win);
	SetPort(old);
}

void
bookmarks_activate(int on)
{
	GrafPtr old;
	int i;

	GetPort(&old);
	SetPort(win);
	LActivate(on, list);
	for (i = 0; i < NBUTTONS; i++)
		HiliteControl(buttons[i], on ? 0 : 255);
	draw_grow_box();
	SetPort(old);
}

void
bookmarks_grow(Point where)
{
	Rect limits;
	long size;

	SetRect(&limits, 420, 200, 2000, 2000);
	size = GrowWindow(win, where, &limits);
	if (size == 0)
		return;
	SetPort(win);
	SizeWindow(win, LoWord(size), HiWord(size), true);
	layout();
	InvalRect(&win->portRect);
}

static void
do_button(int which)
{
	int sel = bookmarks_selected();

	switch (which) {
	case B_NEW:
		editor_open(-1);
		break;
	case B_EDIT:
		if (sel >= 0)
			editor_open(sel);
		break;
	case B_DELETE:
		if (sel >= 0 && app_ask("Delete this bookmark?", "Delete",
		    "Cancel") == 1) {
			memmove(&bookmarks[sel], &bookmarks[sel + 1],
			    sizeof(bookmarks[0]) * (size_t)(nbookmarks - sel -
			    1));
			nbookmarks--;
			bm_save();
			bookmarks_refresh();
		}
		break;
	case B_PREVIEW:
	case B_SYNC:
		if (sel >= 0)
			app_queue_bookmark(sel, which == B_PREVIEW);
		break;
	case B_SERVER:
		app_set_server(!app_server_running());
		break;
	}
}

static void
move_selection(int delta)
{
	int sel = bookmarks_selected(), next;
	Cell c;

	if (nbookmarks == 0)
		return;
	next = sel < 0 ? 0 : sel + delta;
	if (next < 0 || next >= nbookmarks)
		return;
	c.h = 0;
	if (sel >= 0) {
		c.v = sel;
		LSetSelect(false, c, list);
	}
	c.v = next;
	LSetSelect(true, c, list);
	LAutoScroll(list);
	app_update_menus();
}

void
bookmarks_event(EventRecord *ev)
{
	ControlHandle ch;
	Point p;
	char key;

	SetPort(win);
	if (ev->what == mouseDown) {
		p = ev->where;
		GlobalToLocal(&p);
		if (FindControl(p, win, &ch) != 0) {
			if (TrackControl(ch, p, NULL) != 0)
				do_button((int)GetControlReference(ch));
			return;
		}
		if (LClick(p, ev->modifiers, list)) {
			/* Double-click: open for review (not run). */
			if (bookmarks_selected() >= 0)
				editor_open(bookmarks_selected());
		}
		app_update_menus();
		return;
	}
	if (ev->what == keyDown || ev->what == autoKey) {
		key = (char)(ev->message & charCodeMask);
		if (key == 0x1E)
			move_selection(-1);
		else if (key == 0x1F)
			move_selection(1);
		else if (key == '\r' || key == 0x03)
			do_button(B_EDIT);
		else
			SysBeep(1);
	}
}
