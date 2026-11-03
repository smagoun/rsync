# A GUI for rsync on Classic Mac OS

Status: **design proposal.** Nothing here is built yet. Today the app is a console with an `rsync>` prompt (see `classic/README.md`).

## 1. What the GUI is for

* **Make the everyday case a double-click.** You set up a sync once: server, module, local folder and options. After that, syncing is one click, and a dry-run preview shows what will change before anything is deleted.
* **Look and behave like a Mac OS 8 application:**
  * standard windows, menus and dialogs, drawn in the system's Platinum appearance
  * documents in the Finder
  * drag and drop of folders
  * Cmd-. to stop
  * Apple Events, so AppleScript and Startup Items can drive it
* **Keep the command line within reach.** Every sync set can show its equivalent `rsync` command, and the console stays available from the Window menu.

## 2. The pieces

### 2.1 Sync sets (documents)

A *sync set* is a small document (type `RSet`, creator `rSnc`) holding:

| Field | Example | Maps to |
|---|---|---|
| Name | "MyApp sources" | window title / file name |
| Direction | Pull from server / Push to server | argument order |
| Server | `buildbox.local`, port 873 | `rsync://host:port/` |
| Module and path | `src` / `MyApp/` | `module/path` |
| User and password | `steve`, stored scrambled in the set, or asked each time | `RSYNC_PASSWORD` |
| Local folder | `Macintosh HD:Projects:MyApp:` (alias record, so a moved folder is still found) | local path |
| Options | delete extras, forks via netatalk, Apple `-E`, type/creator mapping, Mac line endings, DST tolerance, compress, checksum, whole files, excludes | `--delete`, `--netatalk`, `--extended-attributes`, `--mac-types`, `--mac-text`, `--no-dst-window`, `-z`, `-c`, `-W`, `--exclude` |

Double-clicking a set in the Finder opens it and, if the *Sync when opened* checkbox is set, runs it. An alias to such a set in Startup Items gives an automatic sync at boot.

### 2.2 Windows

#### Sync Sets (the main window)

```
┌─□───────────────── Sync Sets ─────────────────────┐
│  Name               Direction  Last sync           │
│ ▸ MyApp sources      ← Pull    Today 9:41  ✓ 3 files│
│   Shared docs        → Push    Yesterday   ✓ none  │
│   Big archive        ← Pull    Never               │
│                                                    │
│ [New…] [Edit…] [Preview] [Sync Now]                │
└────────────────────────────────────────────────────┘
```

* A List Manager list of the sets in the "rsync Sets" folder next to the app. Sets saved elsewhere can be added with drag and drop.
* Double-click a set to edit it. Return runs **Sync Now**.

#### Sync set editor

```
┌─□───────────── MyApp sources ─────────────────────┐
│ Direction:  (•) Pull from server  ( ) Push to server│
│                                                    │
│ Server:  [buildbox.local      ]  Port: [873 ]      │
│ Module:  [src            ▾] [Get List]             │
│ Path:    [MyApp/              ]                    │
│ User:    [steve   ]  Password: [••••••]  [ ] Ask   │
│                                                    │
│ Local folder:  ┌──────────────────────────┐        │
│                │ 📁 Macintosh HD:Projects: │ [Choose…]
│                │    MyApp:                 │        │
│                └──────────────────────────┘ (drop a folder here)
│                                                    │
│ ▾ Options                                          │
│   [x] Resource forks and Finder info (netatalk)    │
│   [ ] Server is a Mac (Apple -E)                   │
│   [x] Delete files that are not on the server  ⚠   │
│   [x] Set type/creator by name   [Edit Types…]     │
│   [ ] Mac line endings for  [.c .h .r .p      ]    │
│   [x] Ignore 1-hour (daylight saving) differences  │
│   [ ] Compress   [ ] Compare contents (slow)       │
│   Exclude: [*.o           ] [+] [–]                │
│                                                    │
│ ▸ Command:  rsync -av --delete --mac-types …       │
│                                                    │
│ [x] Sync when opened         [Cancel] [Preview] [Sync]│
└────────────────────────────────────────────────────┘
```

* **Get List** connects and fills the Module pop-up (`#list`).
* **Choose…** uses Standard File with a *Select "Folder"* button. Navigation Services needs Mac OS 8.5, so it can't be used on 8.1. The folder box also accepts a folder dragged from the Finder.
* **Delete files that are not on the server** shows a warning icon. Turning it on makes the first sync of the set run as a preview.
* **Command** (disclosure triangle) shows the exact `rsync` command line, selectable for copying into the console.

#### Preview (dry run)

```
┌─□────────── Preview: MyApp sources ───────────────┐
│ 12 to update, 3 new, 2 to delete  (1.2 MB)         │
│ ┌────────────────────────────────────────────────┐ │
│ │ + src:NewThing.c                     4 K       │ │
│ │ ~ src:main.c                         12 K      │ │
│ │ ~ MyApp.rsrc            (resource fork) 80 K   │ │
│ │ – old:Unused.c                                 │ │
│ └────────────────────────────────────────────────┘ │
│                              [Cancel]  [Sync Now]  │
└────────────────────────────────────────────────────┘
```

* This is a `-n -i` run. Each itemized line becomes a row with an icon: `+` new, `~` changed, `–` delete.
* A `._name` row is shown as "(resource fork)" on its file.

#### Progress

```
┌─□──────────── Syncing MyApp sources ──────────────┐
│ Receiving src:main.c                               │
│ ▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░  file 9 of 15            │
│ 412 K of 1.2 MB · 38 K/s · about 20 seconds left   │
│ ▸ Details                                   [Stop] │
└────────────────────────────────────────────────────┘
```

* **Details** expands into the scrolling log (a TextEdit view, last ~200 lines). The full text goes to a "rsync Log" file.
* **Stop** and Cmd-. use the same interrupt as the console.
* In the background, the app uses the Notification Manager (a diamond mark on its Application menu item and an optional sound) when a sync finishes or fails.

#### Server (the daemon)

```
┌─□──────────────────── Server ─────────────────────┐
│ Status: ● On — accepting rsync on port 873         │
│         This Mac: 192.168.1.20                     │
│                                    [Stop Server]   │
│ Shared folders:                                    │
│   Name       Folder                     Access     │
│   incoming   Macintosh HD:Incoming:     Read/Write │
│   projects   Macintosh HD:Projects:     Read only  │
│ [Add…] [Remove] [Edit…]                            │
│                                                    │
│ [x] Start server when rsync opens                  │
│ Recent: 9:41 buildbox.local pushed 3 files to incoming
└────────────────────────────────────────────────────┘
```

* Like the File Sharing control panel.
* The module list is stored in `rsyncd.conf` (a text file next to the app), so power users can still edit it.
* **Add…** chooses a folder and asks for a name, read-only or read/write, and an optional user and password (`auth users` / `secrets file`).
* The window shows the exact command a host can use: `rsync -av -E dir/ rsync://192.168.1.20/incoming/`.

#### Preferences

* Defaults for new sets.
* Time zone override (`RSYNC_GMT_OFFSET`) for Macs that never switch to daylight saving time.
* Log verbosity, and whether to keep a log per set.
* The type/creator table editor: a list of `.ext → TYPE/CREATOR` rows showing each creator application's icon (looked up in the Desktop Database with `PBDTGetIcon`), plus an *Add from file…* button that copies the type and creator of an existing file.

### 2.3 Menus

| Menu | Items |
|---|---|
| Apple | About rsync… |
| File | New Sync Set ⌘N · Open… ⌘O · Close ⌘W · Save ⌘S · Save As… · Quit ⌘Q |
| Edit | Undo ⌘Z · Cut ⌘X · Copy ⌘C · Paste ⌘V · Clear · Select All ⌘A · Preferences… |
| Sync | Sync Now ⌘R · Preview ⌘P · Stop ⌘. · Show Command · Edit Types… |
| Server | Start Server / Stop Server · Shared Folders… |
| Window | Sync Sets ⌘1 · Server ⌘2 · Log ⌘3 · Console ⌘4 |

### 2.4 Apple Events

* **Required events:** `oapp`, `odoc` (open a sync set, and run it if it says so), `quit`.
* **Custom event `rSnc`/`sync`:** with a set alias, run it and reply with counts and an error number. AppleScript example:

  ```applescript
  tell application "rsync" to sync file "Macintosh HD:rsync Sets:MyApp sources"
  ```

## 3. How it would be built

### 3.1 Same engine, new front end

* The GUI replaces `classic/rsync/front.c` but keeps everything below it.
* Running a set means building an `argv` from the set and calling `rsync_main` through `classic_guarded()`, exactly as the console does today. `exit()` returns to the GUI, and per-run state is reset the same way.
* The console stays as a window (Window → Console), still reading the `rsync>` prompt.

### 3.2 Staying responsive: a Thread Manager thread

* rsync's code is long, synchronous loops. It runs in a cooperative Thread Manager thread with a 256 KB stack, and the main thread runs the event loop.
* `mp_yield()`, which every waiting loop already calls, becomes `YieldToAnyThread()` while a transfer runs.
* While a transfer runs, the main thread handles:
  * updates and window dragging
  * the Stop button and Cmd-., which set `mp_interrupted`
  * menus, limited to what's safe mid-transfer
* The Thread Manager is built into Mac OS 8.1. The calls needed are declared by hand, because Multiversal lacks them. The same thread is the plan for local (two-folder) syncs and for keeping the server running while the GUI stays usable.

### 3.3 Progress and results

* Output already passes through the `_consolewrite` wrapper. The GUI sends it to a line parser instead of the console:
  * `-i` itemized lines give per-file rows (preview, progress)
  * "Transfer starting: N files" gives the total
  * the `--stats` summary gives the counts
* Later, a small Classic-only callback in openrsync (`progress_hook(path, bytes, total)`) would make the progress bar exact without parsing text.

### 3.4 Toolbox pieces

* **Resources (Rez):**
  * `MENU`, `WIND`, `DLOG`/`DITL`, `ALRT`, `CNTL`, `STR#`, `vers`
  * `BNDL`, `FREF`, `ICN#`/`icl8`/`ics#` for the app and the `RSet` document
  * `SIZE` at 12 MB preferred
* **Standard controls:** buttons, checkboxes, radio buttons and pop-up menus (`popupMenuProc` CDEF). On Mac OS 8 they draw in Platinum without needing the Appearance Manager API.
* **Managers:** the List Manager for lists, TextEdit for the log, the Drag Manager for folder drops, and Standard File for folder choice (no Navigation Services on 8.1).
* **Storage:**
  * Sync sets are a small `'RSet'` resource or a text block in the data fork. A text block (key = value lines) also stays readable in SimpleText.
  * The local folder is stored as an alias (`NewAlias`/`ResolveAlias`). It's converted to the POSIX path form only when building `argv`.
  * Passwords use a simple reversible scramble (no Keychain before Mac OS 8.6), or *Ask each time*.

### 3.5 Phases

| Step | Delivers |
|---|---|
| 1 | Sync set documents, the editor, Sync Now running in a thread, a log window |
| 2 | Progress window, preview window, Stop, notifications |
| 3 | Server window (start/stop, modules ↔ `rsyncd.conf`) |
| 4 | Apple Events, Preferences, the type/creator editor, Get List for modules |

## 4. Open questions for the owner

1. Is "sync sets as documents" the right model, or do you prefer one window holding all sets, like an FTP bookmarks list?
2. Should *Sync when opened* be the default, so double-clicking a set just syncs it?
3. Should the server start automatically when the app opens, so the Mac can be pushed to at any time?
