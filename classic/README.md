# rsync for Classic Mac OS (68k, Mac OS 8.1)

This is a port of `openrsync/` (Apple's current rsync, protocol 29) to Classic Mac OS, built with [Retro68](https://github.com/autc04/Retro68). It pulls a tree from an rsync daemon and keeps resource forks and Finder info. The design and the reasons behind it are in [`docs/CLASSIC_MACOS_PORT.md`](../docs/CLASSIC_MACOS_PORT.md).

## Using it

There are two applications:

* **rsync**, the windowed app (`classic/gui`):
  * **rsync Bookmarks:** one window listing your saved syncs, like an FTP client's bookmarks. New… creates one. Double-clicking (or Edit…) opens it for review. Sync runs it, and Preview does a dry run.
  * **The editor window:** has every option, a Choose… folder button, the equivalent command line, and To Console, which copies that command to the console.
  * **rsync Console:** the command line, with history on the Up and Down arrows. `help` lists the built-ins: `bookmarks`, `sync NAME`, `preview NAME`, `server on|off`, `set`, `trace`, `clear`, `quit`.
  * **The server:** starts at launch when an `rsyncd.conf` is next to the app (Server > Start Server at Launch turns that off). A sync started from the GUI pauses the server between connections, and it resumes afterwards.
  * **Storage:** bookmarks are kept in the text file `rsync Bookmarks` next to the app.
* **rsync console**, the same engine with only a console window (`classic/rsync`). It's used for scripting and the emulator tests.

At the prompt in either app:

```
rsync -av --delete 192.168.1.10::src/MyApp/ "Macintosh HD:Projects:MyApp:"
rsync -av --delete $(SRV)::src/MyApp/ :MyApp:
```

* **Remote side:** an rsync daemon (`host::module/path` or `rsync://host/module/path`). The Mac has no ssh, so `host:path` doesn't work.
* **Local side:** an HFS path (`Disk:Folder:`, or `:Folder:` relative to the app). A trailing colon means "the folder's contents", like a trailing `/` in rsync.
* **`$(NAME)`:** taken from a text file `rsync prefs` next to the app, which holds lines like `SRV = 192.168.1.10`.
* **Resource forks:** on by default (`--netatalk`), described below. Turn off with `--no-netatalk`.
* **`--mac-types[=FILE]`:** files that arrive without a `._` sidecar get a type and creator chosen by name. The built-in table maps source/text files to `TEXT/ttxt` (SimpleText), `.rsrc` to `rsrc/RSED`, and so on. FILE adds or overrides entries, one per line: `.c TEXT KAHL` (THINK C), `Makefile TEXT MPS ` or `README TEXT R*ch`. A sidecar's own Finder info always wins.
* **`--mac-text[=EXTS]`:** files with these extensions are stored with Mac (CR) line endings. The default list is `.c .h .cp .cpp .cc .hpp .r .p .pas .a .s .asm .txt .make .mk`; give your own as `--mac-text=.c,.h,.r`. LF and CR are swapped as bytes, so rsync still sees the server's exact contents: unchanged files are skipped, `--checksum` agrees, and changes go as deltas. A file with CRLF ends up with LF-CR on the Mac and round-trips exactly. Off by default.
* **DST:** HFS dates are local time, so when daylight saving time starts or ends every local date shifts by an hour. A file whose size matches and whose date is off by exactly one hour (± `--modify-window`) is treated as unchanged. On by default in the Mac build; `--no-dst-window` turns it off. On Unix, `--dst-window` turns it on.
* **Host names** are looked up through the `MacTCP DNR` resolver in the System Folder (TCP/IP control panel's name server).
* **Built-in commands:**
  * `trace on` / `trace off` logs every file and network call.
  * `set NAME=VALUE` sets an environment variable. For example, `set RSYNC_GMT_OFFSET=-18000` pins the time zone (seconds east of UTC) for a Mac that never switches to daylight saving time.
  * `quit` leaves the app.

### The Mac as a server (another computer pushes to it)

Type `rsync --daemon` at the prompt. It serves the modules in a text file `rsyncd.conf` next to the app, one connection at a time, until Cmd-. stops it. Folder paths in the config use the POSIX form, `/Disk/Folder`:

```
[incoming]
    path = /Macintosh HD/Incoming
    read only = no
    use chroot = no
```

From the other computer:

```bash
rsync -av --delete dir/ rsync://192.168.1.20/incoming/dir/
```

* **Resource forks from a Mac running OS X** come across with its stock rsync's `-E` (`--extended-attributes`): `rsync -av -E dir/ rsync://classicmac/incoming/dir/`.
  * The Mac daemon reads Apple's format and turns the `._name` entries into resource forks and Finder info.
  * macOS's rsync reports those entries with the data fork's size, so forks are re-checked on every push. Delta transfer keeps that cheap.
* **From Linux**, a netatalk tree's `._name` files are enough; no option is needed.
* **Verified in QEMU:** pushes from rsync 3.2.7 and openrsync, `-E` pushes from the Linux build (which now also sends Apple's format), updates, `--delete` and a no-op run.

### Server setup (Linux + netatalk 4)

Run a stock rsync daemon on the netatalk volume, with a module like this in `rsyncd.conf`:

```
[src]
    path = /srv/afp/Projects
    read only = yes
    uid = <share owner>
    gid = <share owner>
```

netatalk keeps each file's resource fork and Finder info in a `._name` sidecar file, and the daemon sends those like any other file. The Mac side presents every local file's fork and Finder info as a virtual `._name` in the same format:
* The quick check (size and mtime) works on forks, so unchanged forks are not re-sent.
* Delta transfer works on forks.
* Received sidecars are unpacked into the real resource fork and Finder info.
* `--delete` and removed sidecars are handled.
* The sidecar's mtime is kept in the file's HFS backup date, so a fork-only change on the server re-sends only the fork.

## Layout

| Path | What |
|---|---|
| `macposix/` | POSIX on the File Manager and MacTCP: paths, descriptors, directories, `rename` that keeps resource forks, sockets/`poll`, the `--netatalk` storage backend, C99 `printf`, stubs |
| `gui/` | The windowed application: `app.c` (event loop, menus, run queue, server), `bookmarks.c`, `editor.c`, `console.c`, `glue.c` |
| `rsync/` | The command engine shared by both apps (`engine.c`), the console app (`front.c`), 68k `config.h`, `SIZE` resource |
| `../openrsync/netatalk.c` | `--netatalk` mode (the virtual `._name` layer), shared with the Unix build |
| `../openrsync/hfsname.c` | UTF-8 to HFS names: MacRoman, 31-byte limit with hash-suffixed mangling, `:` to `/`, case collisions |
| `mptest/`, `nettest/` | Self-tests that run inside Mac OS |
| `tests/` | Host tests (`make -C classic/tests check`): `--netatalk` and `--mac-types`/`--mac-text`/`--dst-window` end to end against a stock rsync daemon, name rules, printf |
| `emu/` | QEMU q800 scripts (`try-rsync.sh` for a client run, `try-daemon.sh` boots the Mac running the daemon and pushes to it through a forwarded port): boot unattended, run a test app or an rsync script, read the log back with `hfsutils`; `make-dist.sh` packages the app as a 1.44 MB disk image and MacBinary |

## Building

```bash
# Retro68 (68k only), once:
git clone --recursive https://github.com/autc04/Retro68.git
mkdir Retro68-build && cd Retro68-build && ../Retro68/build-toolchain.bash --no-ppc --no-carbon
```

```bash
cmake -S classic -B build-classic \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
make -C build-classic        # build-classic/rsync/rsync.bin (MacBinary) and rsync.dsk
```

The Unix build of openrsync, which the host tests use, is `cd openrsync && ./configure && make`. Run it in a copy, because `configure` rewrites the checked-in `config.h`.

## Testing in QEMU

See [`emu/README.md`](emu/README.md) for the VM. The ROM and disk images are not in the repo. Then:

```bash
classic/emu/try-rsync.sh "rsync -av --delete rsync://10.0.2.2:8873/src/ :dest:"
```

This installs the built app in Startup Items, runs the command at boot, powers the Mac off, and prints `rsync log`. From QEMU's network, the host is 10.0.2.2.

## Status

Working in QEMU q800 with Mac OS 8.1, against rsync 3.2.7:
* pull with resource forks and Finder info
* a second run transfers nothing
* fork-only and data-only changes are re-sent separately
* `--delete`
* MacRoman and long file names

Also working, in QEMU and in the host tests: host names via MacTCP DNR, `--mac-types`, `--mac-text`, and DST tolerance (checked on the Mac by shifting the clock an hour with `set RSYNC_GMT_OFFSET`).

Not done yet:

* **Local copies** (two Mac paths, e.g. from a mounted AFP volume) need a thread-based server (plan §3.6).
* **Real hardware.** Not yet run on a 68040 or against the owner's netatalk server, and speed has not been measured.
* **Repeated commands.** Several commands in one session reset getopt, verbosity and filter rules. Other global state in openrsync may still carry over, so if a second command misbehaves, quit and relaunch.
