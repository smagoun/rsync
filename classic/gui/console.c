/*
 * console.c: the command window.
 *
 * Output (everything rsync prints, converted from UTF-8 to MacRoman) goes
 * into a read-only TextEdit area with a scroll bar.  Below it is the
 * "rsync>" line: Return runs the command, Up and Down arrows step through
 * the history.  Text can be selected in either part and copied.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gui.h"
#include "engine.h"

#define OUT_MAX		24000	/* TextEdit holds 32K; keep the newest 24K */
#define HIST_MAX	50
#define LINE_H		22	/* input line area */
#define PROMPT		"rsync>"

static WindowPtr win;
static TEHandle out, in;
static TEHandle focus;		/* which one the Edit menu acts on */
static ControlHandle bar;
static char *history[HIST_MAX];
static int nhist, hpos;

int
console_is(WindowPtr w)
{
	return w != NULL && w == win;
}

/* ---- layout and scrolling ---------------------------------------------- */

static short
line_h(void)
{
	return (*out)->lineHeight > 0 ? (*out)->lineHeight : 12;
}

static int
top_line(void)
{
	return ((*out)->viewRect.top - (*out)->destRect.top) / line_h();
}

static int
visible_lines(void)
{
	return ((*out)->viewRect.bottom - (*out)->viewRect.top) / line_h();
}

static void
set_bar(void)
{
	int max = (*out)->nLines - visible_lines();

	if (max < 0)
		max = 0;
	SetControlMaximum(bar, (short)max);
	SetControlValue(bar, (short)top_line());
}

static void
scroll_to(int line)
{
	int max = GetControlMaximum(bar);

	if (line > max)
		line = max;
	if (line < 0)
		line = 0;
	TEScroll(0, (short)((top_line() - line) * line_h()), out);
	SetControlValue(bar, (short)line);
}

static void
layout(void)
{
	Rect r = win->portRect, vr, ir;

	vr = r;
	vr.right -= 15;
	vr.bottom -= LINE_H + 2;
	InsetRect(&vr, 4, 2);
	(*out)->viewRect = vr;
	(*out)->destRect.left = vr.left;
	(*out)->destRect.right = vr.right;
	TECalText(out);

	SetRect(&ir, r.left + 4 + 48, r.bottom - LINE_H + 4,
	    r.right - 18, r.bottom - 4);
	(*in)->viewRect = ir;
	(*in)->destRect = ir;
	(*in)->destRect.right = 4000;	/* one long line */
	TECalText(in);

	MoveControl(bar, r.right - 15, r.top - 1);
	SizeControl(bar, 16, r.bottom - r.top - LINE_H - 2 + 1);
	set_bar();
}

/* ---- output -------------------------------------------------------------- */

void
console_write(const char *text, size_t len)
{
	char buf[512];
	size_t i, n, cut;
	int pinned;
	GrafPtr old;
	Handle h;

	if (win == NULL)
		console_open();
	GetPort(&old);
	SetPort(win);
	pinned = GetControlValue(bar) >= GetControlMaximum(bar) - 1;
	while (len > 0) {
		n = len < sizeof(buf) ? len : sizeof(buf);
		for (i = 0; i < n; i++)
			buf[i] = text[i] == '\n' ? '\r' : text[i];
		if ((*out)->teLength + (long)n > OUT_MAX) {
			/* Drop the oldest third, at a line boundary. */
			h = (*out)->hText;
			for (cut = OUT_MAX / 3; cut < (size_t)(*out)->teLength &&
			    (*h)[cut] != '\r'; cut++)
				continue;
			TESetSelect(0, (long)cut + 1, out);
			TEDelete(out);
		}
		TESetSelect((*out)->teLength, (*out)->teLength, out);
		TEInsert(buf, (long)n, out);
		text += n;
		len -= n;
	}
	set_bar();
	if (pinned)
		scroll_to(GetControlMaximum(bar));
	SetPort(old);
}

void
console_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	if (n > 0)
		console_write(buf, (size_t)n);
}

/*
 * Everything printed through stdio (rsync's own output) arrives here.
 * script_log, when set by app.c, also receives a copy (unattended runs).
 */
short script_log;

ssize_t
_consolewrite(int fd, const void *buf, size_t len)
{
	char conv[512];
	const unsigned char *p = buf;
	size_t chunk, n, i;
	long cnt;

	(void)fd;
	while (len > 0) {
		chunk = len > sizeof(conv) ? sizeof(conv) : len;
		n = engine_utf8_to_macroman(p, chunk, conv);
		console_write(conv, n);
		if (script_log != 0) {
			for (i = 0; i < n; i++)
				if (conv[i] == '\n')
					conv[i] = '\r';
			cnt = (long)n;
			FSWrite(script_log, &cnt, conv);
			FlushVol(NULL, 0);
		}
		p += chunk;
		len -= chunk;
	}
	return (ssize_t)(p - (const unsigned char *)buf);
}

/* No keyboard input for rsync itself (passwords come from bookmarks). */
ssize_t
_consoleread(int fd, void *buf, size_t len)
{
	(void)fd;
	(void)buf;
	(void)len;
	return 0;
}

/* ---- the window ----------------------------------------------------------- */

void
console_open(void)
{
	Rect r, vr;
	Str255 t;

	if (win != NULL)
		return;
	SetRect(&r, 10, 340, 10 + 620, 340 + 250);
	c2p("rsync Console", t);
	win = NewWindow(NULL, &r, t, true, documentProc, (WindowPtr)-1, true,
	    1);
	SetPort(win);
	TextFont(4);	/* Monaco */
	TextSize(9);
	SetRect(&vr, 4, 2, 100, 100);
	out = TENew(&vr, &vr);
	in = TENew(&vr, &vr);
	SetRect(&r, 0, 0, 16, 100);
	bar = NewControl(win, &r, "\p", true, 0, 0, 0, scrollBarProc, 0);
	focus = in;
	layout();
	TEActivate(in);
	console_printf("rsync for Mac OS.  Type a command, e.g.\r"
	    "  rsync -av server::module/dir/ \"Macintosh HD:Projects:\"\r"
	    "Up and Down arrows recall earlier commands.  \"help\" lists "
	    "the built-in ones.\r");
}

void
console_show(void)
{
	if (win == NULL)
		console_open();
	ShowWindow(win);
	SelectWindow(win);
}

void
console_update(void)
{
	GrafPtr old;
	Rect r;

	GetPort(&old);
	SetPort(win);
	BeginUpdate(win);
	EraseRect(&win->portRect);
	TEUpdate(&win->portRect, out);
	r = win->portRect;
	/* The input line: a rule above it and the prompt. */
	MoveTo(r.left, r.bottom - LINE_H - 1);
	LineTo(r.right - 16, r.bottom - LINE_H - 1);
	MoveTo(r.left + 6, r.bottom - 8);
	DrawString("\p" PROMPT);
	TEUpdate(&win->portRect, in);
	DrawControls(win);
	DrawGrowIcon(win);
	EndUpdate(win);
	SetPort(old);
}

void
console_activate(int on)
{
	GrafPtr old;

	GetPort(&old);
	SetPort(win);
	if (on) {
		TEActivate(focus);
		HiliteControl(bar, 0);
	} else {
		TEDeactivate(focus);
		HiliteControl(bar, 255);
	}
	DrawGrowIcon(win);
	SetPort(old);
}

void
console_grow(Point where)
{
	Rect limits;
	long size;

	SetRect(&limits, 260, 120, 2000, 2000);
	if ((size = GrowWindow(win, where, &limits)) == 0)
		return;
	SetPort(win);
	SizeWindow(win, LoWord(size), HiWord(size), true);
	layout();
	InvalRect(&win->portRect);
}

void
console_idle(void)
{
	if (win != NULL && FrontWindow() == win)
		TEIdle(focus);
}

static void
set_input(const char *s)
{
	TESetText((Ptr)s, (long)strlen(s), in);
	TESetSelect(32767, 32767, in);
	SetPort(win);
	InvalRect(&(*in)->viewRect);
}

/* Builtins handled by the window itself. */
static int
console_builtin(const char *line)
{
	if (strcmp(line, "clear") == 0) {
		TESetSelect(0, 32767, out);
		TEDelete(out);
		set_bar();
		return 1;
	}
	if (strcmp(line, "help") == 0) {
		console_printf(
		    "Built-in commands:\r"
		    "  rsync ...            run rsync (the word rsync is optional)\r"
		    "  bookmarks            list the bookmarks\r"
		    "  sync NAME            run a bookmark (preview NAME: dry run)\r"
		    "  server on|off        start or stop the server\r"
		    "  set NAME=VALUE       environment, e.g. RSYNC_GMT_OFFSET\r"
		    "  trace on|off         log file and network calls\r"
		    "  memory               free memory and the low point since last time\r"
		    "  clear                clear this window\r"
		    "  quit                 quit rsync\r");
		return 1;
	}
	return 0;
}

static void
enter(void)
{
	char line[512];
	Handle h = (*in)->hText;
	long len = (*in)->teLength;

	if (len > (long)sizeof(line) - 1)
		len = sizeof(line) - 1;
	memcpy(line, *h, (size_t)len);
	line[len] = '\0';
	set_input("");
	if (line[0] == '\0')
		return;
	if (nhist == 0 || strcmp(history[nhist - 1], line) != 0) {
		if (nhist == HIST_MAX) {
			free(history[0]);
			memmove(history, history + 1,
			    sizeof(history[0]) * (HIST_MAX - 1));
			nhist--;
		}
		history[nhist++] = strdup(line);
	}
	hpos = nhist;
	console_printf(PROMPT " %s\r", line);
	if (!console_builtin(line) && !app_builtin(line))
		app_queue_line(line);
}

static void
recall(int delta)
{
	if (nhist == 0)
		return;
	hpos += delta;
	if (hpos < 0)
		hpos = 0;
	if (hpos >= nhist) {
		hpos = nhist;
		set_input("");
		return;
	}
	set_input(history[hpos]);
}

/* Put text on the input line (e.g. "Copy command to console"). */
void
console_set_input(const char *s)
{
	console_show();
	set_input(s);
}

static pascal void
scroll_action(ControlHandle c, short part)
{
	int page = visible_lines() - 1;

	if (page < 1)
		page = 1;
	switch (part) {
	case inUpButton:
		scroll_to(GetControlValue(c) - 1);
		break;
	case inDownButton:
		scroll_to(GetControlValue(c) + 1);
		break;
	case inPageUp:
		scroll_to(GetControlValue(c) - page);
		break;
	case inPageDown:
		scroll_to(GetControlValue(c) + page);
		break;
	}
}

static void
set_focus(TEHandle te)
{
	if (focus == te)
		return;
	TEDeactivate(focus);
	focus = te;
	TEActivate(focus);
}

void
console_event(EventRecord *ev)
{
	ControlHandle ch;
	short part;
	Point p;
	char key;

	SetPort(win);
	if (ev->what == mouseDown) {
		p = ev->where;
		GlobalToLocal(&p);
		if ((part = FindControl(p, win, &ch)) != 0) {
			if (part == inThumb) {
				TrackControl(ch, p, NULL);
				scroll_to(GetControlValue(ch));
			} else
				TrackControl(ch, p, (ControlActionUPP)scroll_action);
			return;
		}
		if (PtInRect(p, &(*out)->viewRect)) {
			set_focus(out);
			TEClick(p, (ev->modifiers & shiftKey) != 0, out);
		} else {
			set_focus(in);
			TEClick(p, (ev->modifiers & shiftKey) != 0, in);
		}
		return;
	}
	if (ev->what != keyDown && ev->what != autoKey)
		return;
	key = (char)(ev->message & charCodeMask);
	set_focus(in);
	switch (key) {
	case '\r':
	case 0x03:			/* Enter */
		enter();
		break;
	case 0x1E:			/* Up arrow */
		recall(-1);
		break;
	case 0x1F:			/* Down arrow */
		recall(1);
		break;
	default:
		TEKey(key, in);
		break;
	}
}

void
console_edit(int item)
{
	SetPort(win);
	switch (item) {
	case 3:	/* Cut */
		if (focus == in)
			TECut(in);
		else
			TECopy(out);
		break;
	case 4:	/* Copy */
		TECopy(focus);
		break;
	case 5:	/* Paste */
		set_focus(in);
		TEFromScrap();
		TEPaste(in);
		return;
	case 6:	/* Clear */
		if (focus == in)
			TEDelete(in);
		break;
	case 7:	/* Select All */
		TESetSelect(0, 32767, focus);
		return;
	}
	ZeroScrap();	/* Cut, Copy: publish to the desk scrap */
	TEToScrap();
}
