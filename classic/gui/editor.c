/*
 * editor.c: the window that reviews and edits one bookmark.
 *
 * Double-clicking a bookmark opens it here for review; nothing runs until
 * Sync or Preview is clicked.  It is a modeless dialog whose item list is
 * built in memory (ditl_add), so the Dialog Manager handles the text
 * fields, Tab, and the clipboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "engine.h"

void	console_set_input(const char *);

enum {
	I_SAVE = 1, I_CANCEL, I_SYNC, I_PREVIEW,
	I_NAME_L, I_NAME, I_PULL, I_PUSH,
	I_HOST_L, I_HOST, I_PORT_L, I_PORT,
	I_MODULE_L, I_MODULE, I_PATH_L, I_PATH,
	I_USER_L, I_USER, I_PASS_L, I_PASS,
	I_LOCAL_L, I_LOCAL, I_CHOOSE,
	I_NETATALK, I_XATTRS, I_DELETE, I_TYPES, I_TEXT, I_EXTS, I_DST,
	I_COMPRESS, I_CHECKSUM, I_EXCL_L, I_EXCL, I_CMD, I_TOCONSOLE
};

static DialogPtr dlg;
static int editing;		/* index into bookmarks, or -1 for new */

int
editor_is(WindowPtr w)
{
	return w != NULL && w == (WindowPtr)dlg;
}

int
editor_index(void)
{
	return dlg != NULL ? editing : -2;
}

/* ---- field access --------------------------------------------------------- */

static Handle
item_h(int item)
{
	short type;
	Handle h;
	Rect r;

	GetDialogItem(dlg, item, &type, &h, &r);
	return h;
}

static void
get_text(int item, char *out, size_t sz)
{
	Str255 t;

	GetDialogItemText(item_h(item), t);
	p2c(t, out, sz);
}

static void
set_text(int item, const char *s)
{
	Str255 t;

	c2p(s, t);
	SetDialogItemText(item_h(item), t);
}

static int
get_check(int item)
{
	return GetControlValue((ControlHandle)item_h(item)) != 0;
}

static void
set_check(int item, int on)
{
	SetControlValue((ControlHandle)item_h(item), on ? 1 : 0);
}

static void
read_fields(struct bookmark *b)
{
	char port[16];

	get_text(I_NAME, b->name, sizeof(b->name));
	b->push = get_check(I_PUSH);
	get_text(I_HOST, b->host, sizeof(b->host));
	get_text(I_PORT, port, sizeof(port));
	b->port = atoi(port) > 0 ? atoi(port) : 873;
	get_text(I_MODULE, b->module, sizeof(b->module));
	get_text(I_PATH, b->path, sizeof(b->path));
	get_text(I_USER, b->user, sizeof(b->user));
	get_text(I_PASS, b->pass, sizeof(b->pass));
	get_text(I_LOCAL, b->local, sizeof(b->local));
	b->netatalk = get_check(I_NETATALK);
	b->xattrs = get_check(I_XATTRS);
	b->del = get_check(I_DELETE);
	b->mactypes = get_check(I_TYPES);
	b->mactext = get_check(I_TEXT);
	get_text(I_EXTS, b->mactext_exts, sizeof(b->mactext_exts));
	b->dst = get_check(I_DST);
	b->compress = get_check(I_COMPRESS);
	b->checksum = get_check(I_CHECKSUM);
	get_text(I_EXCL, b->exclude, sizeof(b->exclude));
}

static void
write_fields(const struct bookmark *b)
{
	char port[16];

	set_text(I_NAME, b->name);
	set_check(I_PULL, !b->push);
	set_check(I_PUSH, b->push);
	set_text(I_HOST, b->host);
	snprintf(port, sizeof(port), "%d", b->port);
	set_text(I_PORT, port);
	set_text(I_MODULE, b->module);
	set_text(I_PATH, b->path);
	set_text(I_USER, b->user);
	set_text(I_PASS, b->pass);
	set_text(I_LOCAL, b->local);
	set_check(I_NETATALK, b->netatalk);
	set_check(I_XATTRS, b->xattrs);
	set_check(I_DELETE, b->del);
	set_check(I_TYPES, b->mactypes);
	set_check(I_TEXT, b->mactext);
	set_text(I_EXTS, b->mactext_exts);
	set_check(I_DST, b->dst);
	set_check(I_COMPRESS, b->compress);
	set_check(I_CHECKSUM, b->checksum);
	set_text(I_EXCL, b->exclude);
}

static void
show_command(void)
{
	struct bookmark b;
	char cmd[600];

	bm_defaults(&b);
	read_fields(&b);
	bm_cmdline(&b, 0, cmd, sizeof(cmd));
	if (strlen(cmd) > 250)
		strcpy(cmd + 247, "...");
	set_text(I_CMD, cmd);
}

/* ---- building the window -------------------------------------------------- */

static Handle
build_items(void)
{
	Handle h = ditl_new();
	short y;

	/* Order must match the enum. */
	ditl_add(h, DI_BUTTON, 418, 396, 438, 466, "Save");
	ditl_add(h, DI_BUTTON, 418, 316, 438, 386, "Cancel");
	ditl_add(h, DI_BUTTON, 418, 96, 438, 166, "Sync");
	ditl_add(h, DI_BUTTON, 418, 14, 438, 86, "Preview");

	y = 12;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "Name:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 466, "");
	y += 26;
	ditl_add(h, DI_RADIO, y, 110, y + 18, 260, "Pull from server");
	ditl_add(h, DI_RADIO, y, 270, y + 18, 420, "Push to server");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "Server:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 340, "");
	ditl_add(h, DI_STATIC, y, 352, y + 16, 394, "Port:");
	ditl_add(h, DI_EDIT, y, 400, y + 16, 466, "");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "Module:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 220, "");
	ditl_add(h, DI_STATIC, y, 232, y + 16, 274, "Path:");
	ditl_add(h, DI_EDIT, y, 280, y + 16, 466, "");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "User:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 220, "");
	ditl_add(h, DI_STATIC, y, 232, y + 16, 304, "Password:");
	ditl_add(h, DI_EDIT, y, 310, y + 16, 466, "");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "Local folder:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 380, "");
	ditl_add(h, DI_BUTTON, y - 2, 390, y + 18, 466, "Choose\xc9");

	y += 32;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 466,
	    "Resource forks and Finder info (netatalk ._ files)");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 466,
	    "Server is a Mac running OS X (Apple -E)");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 466,
	    "Delete files that are not in the source");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 466,
	    "Set type and creator by name (--mac-types)");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 220, "Mac line endings for:");
	ditl_add(h, DI_EDIT, y + 1, 226, y + 17, 466, "");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 466,
	    "Ignore 1-hour (daylight saving) differences");
	y += 20;
	ditl_add(h, DI_CHECK, y, 14, y + 18, 150, "Compress");
	ditl_add(h, DI_CHECK, y, 160, y + 18, 466, "Compare contents (slow)");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 16, 104, "Exclude:");
	ditl_add(h, DI_EDIT, y, 110, y + 16, 466, "");
	y += 26;
	ditl_add(h, DI_STATIC, y, 14, y + 32, 380, "");
	ditl_add(h, DI_BUTTON, y + 4, 390, y + 24, 466, "To Console");
	return h;
}

void
editor_open(int index)
{
	struct bookmark b;
	Rect r;
	Str255 t;

	if (dlg != NULL) {
		if (editing == index) {
			SelectWindow((WindowPtr)dlg);
			return;
		}
		editor_close();
	}
	editing = index;
	if (index >= 0)
		b = bookmarks[index];
	else {
		bm_defaults(&b);
		strcpy(b.name, "New bookmark");
	}
	SetRect(&r, 60, 44, 60 + 480, 44 + 450);
	c2p(index >= 0 ? b.name : "New Bookmark", t);
	dlg = NewDialog(NULL, &r, t, true, noGrowDocProc, (WindowPtr)-1, true,
	    2, build_items());
	SetPort(dlg);
	write_fields(&b);
	show_command();
	SelectDialogItemText(dlg, I_HOST, 0, 32767);
}

void
editor_close(void)
{
	if (dlg == NULL)
		return;
	DisposeDialog(dlg);
	dlg = NULL;
	app_update_menus();
}

/* ---- actions ---------------------------------------------------------------- */

/* "Disk:Folder:Sub:" for a folder (vRefNum, dirID). */
static void
folder_path(short vref, long dirid, char *out, size_t sz)
{
	char path[512] = "", part[64], tmp[512];
	CInfoPBRec pb;
	Str63 name;

	for (;;) {
		memset(&pb, 0, sizeof(pb));
		pb.dirInfo.ioNamePtr = name;
		pb.dirInfo.ioVRefNum = vref;
		pb.dirInfo.ioFDirIndex = -1;
		pb.dirInfo.ioDrDirID = dirid;
		if (PBGetCatInfoSync(&pb) != noErr)
			break;
		p2c(name, part, sizeof(part));
		snprintf(tmp, sizeof(tmp), "%s:%s", part, path);
		strcpy(path, tmp);
		if (dirid == 2)		/* fsRtDirID */
			break;
		dirid = pb.dirInfo.ioDrParID;
	}
	snprintf(out, sz, "%s", path);
}

static void
choose_folder(void)
{
	StandardFileReply reply;
	char path[256];

	StandardPutFile("\pOpen the folder to sync, then click Save:",
	    "\p(this folder)", &reply);
	if (!reply.sfGood)
		return;
	folder_path(reply.sfFile.vRefNum, reply.sfFile.parID, path,
	    sizeof(path));
	set_text(I_LOCAL, path);
	show_command();
}

/* Validate and store; returns the bookmark's index, or -1. */
static int
save(void)
{
	struct bookmark b;
	char *posix, utf8[600];
	const char *missing = NULL;

	bm_defaults(&b);
	if (editing >= 0)
		b = bookmarks[editing];
	read_fields(&b);
	if (b.name[0] == '\0')
		missing = "a name";
	else if (b.host[0] == '\0')
		missing = "the server";
	else if (b.module[0] == '\0')
		missing = "the module";
	else if (b.local[0] == '\0')
		missing = "the local folder";
	if (missing != NULL) {
		char msg[96];

		snprintf(msg, sizeof(msg), "Please fill in %s.", missing);
		app_note(msg);
		return -1;
	}
	engine_macroman_to_utf8(b.local, utf8, sizeof(utf8));
	if ((posix = engine_hfs_arg(strchr(utf8, ':') ? utf8 : "x")) == NULL) {
		app_note("The local folder must be a full path such as "
		    "\"Macintosh HD:Projects:MyApp:\" (use Choose...).");
		return -1;
	}
	free(posix);
	if (editing < 0) {
		if (nbookmarks == BM_MAX) {
			app_note("There are too many bookmarks.");
			return -1;
		}
		editing = nbookmarks++;
	}
	bookmarks[editing] = b;
	bm_save();
	bookmarks_refresh();
	return editing;
}

void
editor_event(EventRecord *ev)
{
	DialogPtr which;
	short item;
	char key, cmd[600];
	struct bookmark b;
	int idx;

	if ((ev->what == keyDown || ev->what == autoKey) &&
	    !(ev->modifiers & cmdKey)) {
		key = (char)(ev->message & charCodeMask);
		if (key == '\r' || key == 0x03) {
			if (save() >= 0)
				editor_close();
			return;
		}
		if (key == 0x1B) {	/* Escape */
			editor_close();
			return;
		}
	}
	if (!DialogSelect(ev, &which, &item) || which != dlg)
		return;
	switch (item) {
	case I_SAVE:
		if (save() >= 0)
			editor_close();
		return;
	case I_CANCEL:
		editor_close();
		return;
	case I_SYNC:
	case I_PREVIEW:
		if ((idx = save()) >= 0) {
			app_queue_bookmark(idx, item == I_PREVIEW);
			editor_close();
		}
		return;
	case I_PULL:
	case I_PUSH:
		set_check(I_PULL, item == I_PULL);
		set_check(I_PUSH, item == I_PUSH);
		break;
	case I_NETATALK: case I_XATTRS: case I_DELETE: case I_TYPES:
	case I_TEXT: case I_DST: case I_COMPRESS: case I_CHECKSUM:
		set_check(item, !get_check(item));
		break;
	case I_CHOOSE:
		choose_folder();
		break;
	case I_TOCONSOLE:
		bm_defaults(&b);
		read_fields(&b);
		bm_cmdline(&b, 0, cmd, sizeof(cmd));
		console_set_input(cmd);
		return;
	}
	show_command();
}
