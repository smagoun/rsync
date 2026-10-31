# Plan: rsync for Classic Mac OS (8.1) with resource-fork support

Status: **draft plan, nothing implemented yet.** Open questions are collected at the end.
Toolchain: [Retro68](https://github.com/autc04/Retro68) (68k and/or PowerPC/CFM).

## 1. Goals

| # | Goal | Command run on… |
|---|------|-----------------|
| G1 | **Pull**: copy a source tree from a server to the classic Mac, keeping resource forks and Finder info, and copying only what changed | Classic Mac: `rsync -av --delete host::module/dir "Macintosh HD:Projects:dir"` |
| G2 (stretch) | **Push**: a modern host sends a tree to the classic Mac | Modern host: `rsync -av --delete dir/ rsync://classicmac/module/dir/` |

The app is a console app. You launch it, type a command line, and it runs that command.

## 2. What is in this repository

The repo holds two rsync implementations, and both have Apple's resource-fork support:

| | `rsync/` (Apple rsync 2.6.9) | `openrsync/` (Apple's current `/usr/bin/rsync`) |
|---|---|---|
| License | GPLv3 | ISC |
| Wire protocol | 29 | 29 (works with any server that speaks ≥27) |
| Resource forks (`-E`) | Through `copyfile()`. Each file with metadata gets an extra synthetic AppleDouble entry named `._name`, plus one extra byte per flist entry | The same wire behaviour, kept inside **`platform.c` hooks** (`platform_flist_modify`, `platform_flist_received`, `platform_move_file`, `platform_finish_transfer`, …) |
| Process model | Forks the receiver from the generator, uses pipes between them and `select()`, and forks again for local copies and remote shells | The **receiver is one process** running a `poll()` loop over the uploader and downloader. The sender is one process too. It forks only to start a remote shell (ssh) or a local server, and in daemon mode |
| File I/O | Path-based POSIX calls | `*at()` calls (`openat`, `fstatat`, `mkdirat`, `renameat`, `unlinkat`, …), `fts`, and `mmap` with a buffered-read fallback (`FT_BUFIO`) |

### Recommendation: base the port on `openrsync/`

1. **No fork in the main pull path.** As a client of an rsync daemon, openrsync connects a socket and runs `rsync_receiver()` in the same process. Classic Mac OS has no `fork`, `exec` or pipes, so this removes the hardest part of porting 2.6.9.
2. **Resource-fork support sits behind a hook layer.** We write `platform_classic.c`, which implements the same hooks with the File Manager instead of `copyfile()`. The wire format stays identical to what Apple's rsync sends with `-E`.
3. **The `*at()` API matches HFS well.** A directory "fd" becomes a `(vRefNum, dirID)` pair, and `openat(dirfd, "name")` becomes `FSMakeFSSpec(vRefNum, dirID, name)`. This avoids the 255-character limit on HFS full paths.
4. `mmap` already has a buffered fallback, so nothing has to change there.
5. The ISC license makes it simple to ship a binary.

The fallback is to port 2.6.9. That means replacing its forks with Thread Manager threads, which costs more and carries more risk.

## 3. Architecture of the port

```
+---------------------------------------------------------------+
|  Front end (RetroConsole window): command prompt, profiles,   |
|  drag-a-folder-to-insert-path, progress, Cmd-. to abort       |
+---------------------------------------------------------------+
|  openrsync core (mostly unchanged; #ifdef __MACOS_CLASSIC__)  |
|  client.c receiver.c uploader.c downloader.c sender.c flist.c |
+-------------------+--------------------+----------------------+
| platform_classic.c| macposix/          | macnet/              |
| AppleDouble <->   | *at() file API     | Open Transport TCP   |
| rsrc fork,        | over File Manager, | as socket()/connect()|
| FInfo/FXInfo,     | fts replacement,   | read/write/poll(),   |
| type/creator map, | stat synthesis,    | yields to other apps |
| CR/LF translation | name/charset map   | through WaitNextEvent|
+-------------------+--------------------+----------------------+
|        Retro68: newlib + Toolbox/OS (Multiversal or Universal |
|        Interfaces), Thread Manager, Open Transport 1.3        |
+---------------------------------------------------------------+
```

### 3.1 Transport

* **Classic Mac OS has no ssh.** So the single-colon form `host:dir` (remote shell) can't work as-is. The plan is to use the **rsync daemon protocol** over TCP: `host::module/path` or `rsync://host/module/path`. The server runs `rsync --daemon` with an `rsyncd.conf` module that points at the source tree. Authentication is the daemon's challenge/response, and the data itself is sent unencrypted. That is acceptable on a LAN.
* **Reuse the single colon for local paths.** Remote shells are unsupported anyway, so on classic Mac OS a single-colon argument means an **HFS path** (`Macintosh HD:Projects:foo`), not `host:path`. This one-line change in `fargs`/argument parsing makes HFS paths behave naturally.
* **Networking uses Open Transport 1.3** (it ships with Mac OS 8.1). The `macnet/` layer offers `socket/connect/read/write/close/poll` for TCP endpoints only.
  * It uses OT synchronous, blocking mode with a sync-idle notifier. The notifier calls `WaitNextEvent`/`YieldToAnyThread`, so the machine stays responsive and Cmd-. can abort.
  * `poll()` only needs to handle one or two network endpoints plus "always ready" local files. That makes it simple to write.
  * MacTCP support is out of scope unless you need it on 68k machines without OT.
* **If "AppleTalk server" means a mounted AFP volume,** see §3.6. That mode needs no networking code at all.

### 3.2 File system layer (`macposix/`)

This layer implements only the subset that openrsync uses:

* **Directory handles.** A small table maps an `int` fd to `{vRefNum, dirID}` for directories, or to `{refNum, FSSpec}` for open files. `openat`, `fstatat`, `mkdirat`, `unlinkat` and `renameat` run on top of `PBGetCatInfo`, `PBDirCreate`, `FSpDelete`, `FSpRename` and `FSpCatMove`.
* **Replacing an existing file.** HFS `rename` fails if the target exists. To replace a file, swap the contents with `FSpExchangeFiles`, then delete the temp file. That keeps the replacement atomic, and Finder info and the file ID stay attached to the old file.
* **`stat` synthesis:**
  * Mode is 0644 for files and 0755 for folders, with an optional "locked" mapping to read-only.
  * uid and gid are 0, nlink is 1, and size is the data fork length.
  * mtime comes from `ioFlMdDat`.
* **Times.**
  * Mac dates count seconds since 1904 in **local time**. Convert them to and from the Unix epoch in UTC using the GMT delta from `ReadLocation()`.
  * After a DST change every mtime would differ by an hour. Recommend `--modify-window`, or pin the GMT delta in prefs.
* **Directory traversal** needs a minimal `fts_open/fts_read` replacement built on indexed `PBGetCatInfo`. This is only needed for the push/sender side and for `--delete` scanning.
* **Unsupported file types** (symlinks, hard links, devices, ownership):
  * Symlinks on the server are skipped with a warning, or, with `-L`, the files they point to are copied.
  * Later we could map symlinks to Finder alias files.
  * `chown` and `chmod` do nothing.
* **Names:**
  * Translate between UTF-8 on the wire (in NFC or NFD form) and MacRoman in HFS.
  * HFS names allow at most **31 characters** and cannot contain `:`. Names that break these rules are mangled in a predictable way: shortened with a hash suffix, and `:` becomes `/`, which is the Finder's convention.
  * The reverse mapping is kept in an invisible per-folder map file, so later runs and `--delete` behave correctly.
  * HFS is case-insensitive. Two server files whose names differ only by case cause a reported conflict.
* **File size.** Each fork is limited to 2 GB. That is not a concern for source trees.

### 3.3 Resource forks and Finder metadata (`platform_classic.c`)

When `-E` is used, Apple's rsync sends, for each file `foo` that has metadata, a synthetic **AppleDouble v2** file `._foo`, as `copyfile(COPYFILE_PACK)` produces it. openrsync orders `foo` before `._foo`, and applies `._foo` as metadata in `platform_move_file()` instead of storing it as a file.

On classic Mac OS:

* **Unpack (receiving).** Parse the AppleDouble header and apply its entries:
  * Entry 2 (resource fork) is written with `FSpOpenRF`.
  * Entry 9 (Finder info) is applied as the 16-byte FInfo with `FSpSetFInfo` and the 16-byte FXInfo with `PBSetCatInfo`. The extended-attribute area that `copyfile` adds after the Finder info is ignored.
  * Entry 8 (dates) sets the creation date.
  * Folders get their Finder info through `PBSetCatInfo`.
* **Pack (sending, and as the basis for delta transfer).** Build an AppleDouble file from the local resource fork and Finder info. The layout should match `copyfile`'s byte for byte as far as possible. Then size and mtime match between runs, the quick check passes, and a `._foo` entry is not re-checked every time.
  * **Risk:** if the server file has xattrs that HFS can't store (for example `com.apple.quarantine`), the sizes never match. Each run would then do a cheap but non-zero delta check of the metadata. Fixes: strip those xattrs on the server (`xattr -c`), or keep a small local cache of xattr blobs.
* **Servers without Apple's `-E` extension** (Linux rsync 3.x, netatalk trees) need a compatibility mode, `--appledouble-files`. In this mode the client does not send `-E`, and treats ordinary `._foo` files that exist on the server as AppleDouble companions of `foo`. It uses the same unpack code with no wire extension. A variant for netatalk 2's `.AppleDouble/foo` layout can come later.
* **Type and creator** for files that arrive **without** Finder info, such as a plain git checkout on Linux:
  * Map them by file extension through **Internet Config** (`ICMapFilename`, built into 8.1).
  * An override table in a prefs file can map, for example, `.c .h .r → TEXT/CWIE`.

### 3.4 Optional: line-ending translation for text files

MPW (and some other tools) need CR line endings. Swapping CR and LF **keeps the length of the file**. That means translation can happen in the macposix read/write layer for files chosen by extension or by type `TEXT`, and the rsync algorithm still works: block offsets, sizes and checksums all line up because the generator checksums the translated-back bytes. Files with CRLF line endings are left alone, because converting CRLF to CR would change their length. This feature is off by default, and turned on with `--mac-text=<ext-list>` or in prefs.

### 3.5 Front end

* A RetroConsole window shows a `rsync>` prompt. A small tokenizer handles quotes and backslashes and builds `argv`.
* Variable substitution `$(NAME)` reads from a `rsync prefs` text file next to the app, or in the Preferences folder. Saved "profiles" let you re-run a sync in one step.
* Dropping a folder or file on the window inserts its HFS path. Dropping a profile document on the app runs it.
* Progress (`-v`, `--progress`) goes to the window. Cmd-. cancels cleanly, removing temp files.
* The app partition is set through the SIZE resource, with 8–16 MB suggested. openrsync keeps the whole file list in memory, at about 150 bytes plus the path length per file, so 20k files take about 4–5 MB.

### 3.6 Local mode (both paths on the Mac, for example a mounted AFP volume)

openrsync runs a local copy by forking a "server" connected through a `socketpair`. On classic Mac OS that server runs instead in a **Thread Manager cooperative thread**, connected to the client through an in-memory duplex buffer (a fake `socketpair`). `poll()` treats the buffer as ready when it has data, and yields otherwise.

The delta algorithm doesn't help much over AFP, because it has to read the whole file to checksum it. The real gain is skipping unchanged files by size and mtime, and `--delete`. That is cheap catalog work and much faster than copying in the Finder. Resource forks come straight from the File Manager on both sides, so no AppleDouble step is needed.

### 3.7 Stretch: push from a modern host (G2)

The classic Mac runs `rsync --daemon` on port 873 through an OT listener and serves **one connection at a time**, with no fork. It reads a minimal `rsyncd.conf` with modules mapped to HFS folders. The modern host then runs `rsync -av -E --delete dir/ rsync://classicmac/module/dir/`, with `-E` only when the host runs Apple's rsync. On the classic side the receiver code is the same as in G1. The extra work is the OT listener, the daemon greeting and authentication (`daemon.c` without fork, privilege dropping or chroot), and the sender-side fts/pack code that `--delete` and bidirectional use need.

## 4. Phases

| Phase | Work | Exit criterion |
|---|---|---|
| 0. Toolchain and test bed | Build Retro68 in Docker/CI (68k and/or PPC). Set up an emulator: Basilisk II for 68k, or SheepShaver/QEMU-ppc for PPC with 8.1, OT and a shared network. Write a "hello OT" TCP client | Hello-world connects to a Linux host from the emulator |
| 1. Make openrsync portable on Linux | Put all filesystem, process and network calls behind a thin `compat_*` interface. Add `__MACOS_CLASSIC__` guards. Make the daemon-client pull path free of fork, and the local mode able to use threads. Run the existing regression tests on Linux | Linux build is green, with no behaviour change |
| 2. `macposix` and `macnet` | File Manager backend, OT backend, `poll()`, time and name mapping. A unit-test app runs in the emulator | Can list, create, rename and delete a tree, and do TCP echo |
| 3. **MVP: pull, data forks only** | `rsync -rt --delete host::mod/dir HFS:path` against an rsync 3.x and Apple rsync daemon | Second run transfers 0 bytes, and an edited file is delta-transferred |
| 4. Resource forks | `platform_classic.c` unpack, `-E` against Apple rsync, `--appledouble-files` against Linux, Internet Config type/creator | A CodeWarrior project and `.rsrc` files arrive intact and open/build |
| 5. Front end and polish | Prompt, profiles, `$(VAR)`, drag and drop, Cmd-., progress, optional CR/LF translation | Your use case end to end |
| 6. Local/AFP mode | Thread-based local server and fake socketpair | Sync from a mounted AppleShare volume to a local disk |
| 7. Stretch: daemon on classic | OT listener, `daemon.c` without fork, sender-side pack | A modern host pushes to the classic Mac |

Testing approach:

* A Linux "golden" server holds fixture trees with long, non-ASCII, case-colliding names, resource-fork-only files, and empty and large files.
* A script in the emulator runs syncs and dumps a manifest of names, data and resource fork hashes, type/creator, and dates. The script compares that manifest with the server.

## 5. Risks

* **Performance on 68k:** MD4 and rolling checksums on a 25–40 MHz 68030/040 cost more than an LC's Ethernet throughput. Default to `--whole-file` on slow CPUs, and keep delta for big files that change little.
* **Retro68 headers:** Open Transport may need Apple's Universal Interfaces rather than Multiversal Interfaces. Check this in phase 0.
* **Quick-check stability of `._` entries** (see §3.3) and **DST shifts in mtimes** (see §3.2).
* **Stack and heap:** the openrsync code puts `PATH_MAX` buffers on the stack. Give the threads stacks of at least 64 KB, and keep an eye on heap fragmentation in a fixed partition.

## 6. Open questions

See the list in the conversation, or the PR description.
