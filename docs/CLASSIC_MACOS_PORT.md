# Plan: rsync for Classic Mac OS 8.1 with resource-fork support

Status: **plan, revision 6.** Phases 0–3 are done, and most of Phase 4 works in QEMU. See [`classic/README.md`](../classic/README.md) for what runs today and what's missing.

## 0. Decisions so far

| Topic | Decision |
|---|---|
| Target | 68040 with 32 MB RAM, Mac OS 8.1, HFS disk. Built as a classic 68k app (not CFM-68K) |
| Toolchain | **Retro68 only** for rsync itself (see §7 on why THINK C 5 won't work) |
| Servers | (a) a modern Mac, or (b) Linux running **netatalk 4.x** (owner's: afpd 4.5.0dev, `ea = sys`, `._name` sidecars that also carry Finder info), using a **stock rsync daemon** (§3.3). Optionally (c) any AFP server mounted on the classic Mac |
| Resource forks | Required from the first version |
| Encryption | Not needed for now. Options are in §8 |
| Type/creator mapping, CR/LF translation | Options exist for both. Both are **off by default** |
| Testing | **QEMU `q800`**, which emulates a Quadra 800 (68040) and boots Mac OS 7.1–8.1 |
| Push to the classic Mac | Stretch goal, deferred |

## 1. Goals

| # | Goal | Example command, typed on the classic Mac |
|---|---|---|
| G1 | Pull a tree from a server to the classic Mac, keeping resource forks and Finder info, and copying only files that changed. `--delete` must work | `rsync -av --delete $(SRV)::src/MyApp "Macintosh HD:Projects:MyApp"` |
| G1b | The same, from an AFP volume mounted in the Finder (no server software needed) | `rsync -av --delete "Server Vol:MyApp" "Macintosh HD:Projects:MyApp"` |
| G2 (stretch) | A modern host pushes to the classic Mac | `rsync -av --delete dir/ rsync://classicmac/proj/dir/`, typed on the modern host |

## 2. Base code: `openrsync/` (Apple's current rsync), not `rsync/` (2.6.9)

The repo holds both. Both send resource forks the same way when `-E` is given:

* For each file `foo` that has metadata, the sender adds an extra AppleDouble v2 entry named `._foo` to the file list.
* Each file-list entry carries one extra byte that says whether it is a synthetic `._` entry.

openrsync is the better base for four reasons:

* **Pulling needs no `fork`.** As a daemon client it runs the receiver as one process: a `poll()` loop over the uploader and downloader (`receiver.c`). Rsync 2.6.9 forks the generator and receiver on every transfer.
* **The resource-fork code sits behind small hooks.** These are `platform_flist_modify`, `platform_flist_received`, `platform_move_file` and `platform_finish_transfer` in `platform.c`. We write a `platform_classic.c` with the same hooks, and the wire format stays the same.
* **Its file calls take a directory handle.** Calls like `openat`, `fstatat` and `mkdirat` map directly to HFS `(vRefNum, dirID)` pairs, which avoids the 255-character limit on HFS full paths.
* **It already reads files without `mmap`.** The `FT_BUFIO` fallback in `fmap.c` does buffered reads. The license is ISC.

## 3. Server side: where resource forks live, and how they get onto the wire

### 3.1 What the owner's netatalk server does (afpd 4.5.0dev), as measured

The owner copied Civilization 1.1.2 from a Mac OS 8.1 client over AppleShare over IP into `foo/` (Get Info: resource fork 687,608 bytes, no data fork), then examined the server:

```
foo/Civilization™ 1.1.2      0 bytes       mtime = the Mac's mod date (2026-06-02 17:10:30)
foo/._Civilization™ 1.1.2    687690 bytes  mtime = server time of last fork/metadata write
  + xattr user.org.netatalk.Metadata on the data file
```

`xxd` of the `._` file:

```
0005 1607  0002 0000  "Netatalk        "  0002          magic, v2, filler, 2 entries
0000 0009  0000 0032  0000 0020                         Finder info  @50, 32 bytes
0000 0002  0000 0052  000a 7df8                         resource fork @82, 687,608 bytes
@50: 4150504c 43764d4d 2100 ffff ffff 0000 ...          'APPL' 'CvMM' flags 0x2100, location -1,-1
@82: 0000 0100 000a 3bce 000a 3ace ...                  start of the resource fork (its own header)
```

Findings:

1. **Layout.** The volume stores files in `ea = sys` layout.
   * The resource fork is in `._name` next to the data file. The file is 82 bytes of header plus the fork: 82 + 687,608 = 687,690, which matches.
   * The filler is `"Netatalk        "`, *not* `"Mac OS X"`. So the parser identifies these files by their entry table, not by the filler.
2. **Finder info is copied into the `._` header.** The 32 bytes at offset 50 (`APPL`/`CvMM`, flags, location) are identical to entry 9 in the `user.org.netatalk.Metadata` xattr.
   * **A stock rsync daemon therefore carries type, creator and Finder flags.** No custom server is needed.
   * The xattr also holds the comment (4), dates (8), AFP info (14) and netatalk's private dev/ino/sync/CNID entries. These are not needed on the Mac. Creation date is the only loss (see 5).
3. **Mtime behaviour:**
   * Opening a file over AFP changes only the data file's **ctime**. No mtime or size changes.
   * **Editing only the resource fork (same length) bumps the `._` mtime but *not* the data file's mtime.** So the client can't rely on the data mtime to catch fork changes. The client keeps the two mtimes separately (§3.2).
   * The data file's mtime is the Mac's own modification date, which AFP sets. It stayed in UTC across the Mac's DST change.
4. **Older `.AppleDouble/` v2 sidecars** (741-byte header) seen earlier may be corrupt or leftovers. The client still parses them, at low priority. The parser is generic over the entry table, so it costs almost nothing extra.
5. **Still to check:**
   * *(a)* Does a file **with no resource fork** (a TEXT file with type/creator only) get a `._` file at all? If not, its type and creator exist only in the xattr, and a stock daemon can't send them. **Fallback:** `--mac-types` extension mapping on the client, which is usually fine for source files. **Deferred by the owner:** no custom server, so these files get `--mac-types` or nothing for now.
   * *(b)* Does a **Finder-info-only change** (label, type/creator edit, move icon) rewrite the `._` header and bump its mtime?
   * *(c)* The creation date is only in the xattr, so it isn't preserved. The HFS creation date is set to the modification date.

### 3.2 The client's `--netatalk` mode

Netatalk's resource forks are ordinary files on disk, so any rsync daemon sends them with the rest of the tree. The classic client gets a mode called `--netatalk` (`-N`) that works with rsync's standard wire format.

The mapping happens in `platform_flist_received()`, right after the file list arrives. Transfers are requested by list index, not by name, so the client can rename entries locally without affecting the server.

| Entry on the wire | What the client does |
|---|---|
| `dir/._foo` (AppleDouble, filler "Netatalk" or "Mac OS X") | Becomes the metadata entry for `dir/foo`. After transfer it is unpacked by the same code as Apple's `-E` (§4.4): entry 2 becomes the resource fork, entry 9 the Finder info. If entry 9 is longer than 32 bytes, as in macOS-written files with an `ATTR` xattr block, the extra is ignored. Location `-1,-1` means "let the Finder place it" |
| `dir/.AppleDouble/foo` (old v2) | The same, plus entry 8 (dates). Entries 3, 4, 11 and 13–19 are ignored |
| `dir/.AppleDouble/` and `.AppleDouble/.Parent` | Hidden from the local view. `.Parent` supplies folder Finder info |
| Both `._foo` and `.AppleDouble/foo` exist | The newer one wins, by mtime, and a warning is printed |
| `.AppleDB/`, `.AppleDesktop/`, `Network Trash Folder`, `Temporary Items`, `.DS_Store`, `:2eDS_Store` | Excluded by a built-in filter list |
| `foo` with no metadata entry | Plain data file. Type and creator come from `--mac-types`, if given |

**How the quick check stays stable** (unchanged files must not be re-sent):

* **Size.** The local "size" of a `._foo` entry is 82 plus the local resource fork length. For a `.AppleDouble/foo` entry it is 741 plus that length. Nothing is packed to work this out. The first transfer checks that the header the server sent has the expected size. If not, the actual size is used.
* **Modification time (implemented).** The server's two mtimes are kept **separately** on the client:
  * the data file's mtime becomes the HFS **modification date**, which is the Mac's true date (fact 3);
  * the sidecar's mtime is stored in the HFS **backup date**. Nothing else on a client machine uses that field; the Linux test backend keeps it as the mtime of `.finf/NAME`.

  The virtual `._foo` reports the backup date as its mtime. A fork-only change on the server re-sends only `._foo`, and a data-only change re-sends only `foo`. `classic/tests/netatalk_test.sh` checks both.

  *Trade-off:* if someone edits a resource fork **on the Mac** without changing the fork's length, the next sync won't notice and won't restore the server's version. Use `--checksum` to force a full comparison.
* **Basis for delta mode.** The local fork and Finder info are packed in netatalk's 82-byte `._` layout, so the basis matches the server byte for byte except where it really changed.
* **`--delete`:**
  * When the sidecar disappears on the server, the client clears the local fork and Finder info.
  * When the data file disappears, the client deletes the HFS file, both forks together.
  * A `._foo` with no `foo` (orphaned) is ignored with a warning.

### 3.3 Server options

| Server | What runs on it | How forks reach the Mac |
|---|---|---|
| **Linux + netatalk (owner's setup)** | **Stock `rsync --daemon` (3.x)**, with a module on the share: `read only = yes`, `uid`/`gid` = the share owner | Client `--netatalk` (§3.2) |
| *(Not planned)* | The owner decided against a custom server. Fork-less files with type/creator only in the xattr are out of scope for now (§3.1, 5a). `--mac-types` covers common cases | — |
| Modern Mac | Apple's stock `rsync --daemon`, run with `-E` | Apple `-E` format (§4.4) |
| Any AFP server, mounted on the classic Mac | Nothing | The File Manager on the Mac reads forks directly (G1b) |

A server-side example (`/etc/rsyncd.conf` on the netatalk host):

```
[src]
    path = /srv/afp/Projects
    read only = yes
    uid = user
    gid = user
    # optional: hosts allow = 192.168.1.0/24
```

On the Mac you then type: `rsync -av --netatalk --delete $(SRV)::src/MyApp "Macintosh HD:Projects:MyApp"`

## 4. Client architecture (classic Mac)

```
+---------------------------------------------------------------+
|  Front end: RetroConsole window, "rsync>" prompt, $(VAR)      |
|  substitution from a prefs file, saved profiles, drag a       |
|  folder to insert its path, Cmd-. to abort                    |
+---------------------------------------------------------------+
|  openrsync core (#ifdef __MACOS_CLASSIC__ where needed)       |
+------------------+------------------+-------------------------+
| platform_classic | macposix/        | macnet/   +  mthread/   |
| AppleDouble pack | *at() over the   | TCP via   |  Thread Mgr |
| and unpack,      | File Manager,    | Open      |  for local  |
| type/creator,    | fts, stat, names,| Transport |  mode, fake |
| CR/LF option     | times            |           |  socketpair |
+------------------+------------------+-------------------------+
| Retro68 (m68k-apple-macos): newlib, Toolbox, OT 1.3, Thread Mgr|
+---------------------------------------------------------------+
```

### 4.1 Transport

* **No ssh on the Mac** (see §8). Remote paths use the rsync daemon protocol: `host::module/path` or `rsync://host/module/path`.
* **A single colon means an HFS path.** Remote shells aren't supported, so on the Mac `Macintosh HD:Projects` is a local HFS path, not `host:path`.
* **TCP comes from Open Transport 1.3**, which ships with 8.1. `macnet/` offers `socket/connect/read/write/close/poll` for TCP only.
  * It runs synchronously, with a sync-idle callback that calls `WaitNextEvent`. That keeps the Finder alive and lets Cmd-. abort.
  * **Decision (Phase 0): use the MacTCP API, not native OT calls.**
    * Retro68's open-source Multiversal Interfaces declare all of MacTCP (`.IPP` driver `PBControl` calls and `MacTCP DNR`).
    * They have **no** Open Transport, Thread Manager, Internet Config or Text Encoding Converter headers.
    * 68k OT clients would also need Apple's proprietary Universal Interfaces plus static glue libraries.
    * OT on 8.1 supports MacTCP calls for compatibility, and the owner's image has `MacTCP DNR` in the System Folder.
    * The API is asynchronous and parameter-block based. `macnet/` hides that behind blocking calls that poll and call `WaitNextEvent`.
    * Native OT stays an optional later optimisation, if throughput is poor.
  * **Thread Manager** (needed only for local mode) is a single trap, `_ThreadDispatch` (0xABF2). The few calls needed are declared by hand.
* **Local mode (G1b):** openrsync forks a local "server" connected by a `socketpair()`. On the Mac:
  * That server runs in a **Thread Manager cooperative thread**, connected through an in-memory duplex buffer.
  * `poll()` on that buffer yields to the other thread until data is ready.
  * Thread stacks are at least 128 KB, because openrsync puts `PATH_MAX` buffers on the stack.

### 4.2 File system (`macposix/`)

* **Directory handles.** A table maps each fd to `{vRefNum, dirID}` for a directory, or `{refNum, FSSpec}` for an open file. These calls are built on the File Manager:

  | POSIX call | File Manager call |
  |---|---|
  | `openat` | `FSMakeFSSpec` + `FSpOpenDF` |
  | `fstatat` | `PBGetCatInfo` |
  | `mkdirat` | `PBDirCreate` |
  | `unlinkat` | `FSpDelete` |
  | `renameat` | `FSpRename` + `FSpCatMove` |
* **Replacing a file.** HFS `rename` can't overwrite an existing file, so the finished temp file is swapped in with `FSpExchangeFiles` and the temp is then deleted. The swap is atomic, and the file keeps its ID, so aliases to it still work.
* **`stat` synthesis:**
  * Mode is 0644 for files and 0755 for folders. A locked file maps to read-only.
  * uid and gid are 0, nlink is 1, and size is the data fork length.
  * mtime comes from `ioFlMdDat`.
* **Times.** Mac dates count seconds since 1904 in *local* time.
  * Convert them to and from UTC Unix time with the GMT delta from `ReadLocation()`.
  * **DST.** HFS dates are local time, so when the Mac's clock moves on or off DST, every local mtime appears to shift by exactly one hour. (The owner's Mac just did this. The server's UTC mtimes did not move.) To avoid a full re-transfer twice a year:
    * By default the client treats an mtime difference of **exactly ±3600 s, with equal size**, as unchanged. Pass `--strict-times` to turn this off.
    * When a file is written, the client converts using the *current* `MachineLocation` offset, including the `dlsDelta` flag.
    * A `gmt-offset` in prefs can pin a fixed offset for anyone who keeps the Mac on standard time all year.
* **Directory traversal.** Write an `fts` replacement on top of indexed `PBGetCatInfo`. The `--delete` scan needs it, and so do local mode and push.
* **What doesn't exist on HFS:**
  * Symlinks: skip with a warning, or follow them with `-L` (`--copy-links`).
  * Hard links, devices, ownership and permission bits: ignored.

### 4.3 File names

The wire carries UTF-8 names. HFS holds names of at most 31 bytes, in MacRoman, without `:`, and case-insensitive. Names are mapped with **a deterministic rule that needs no stored state**:

1. **Normalize** to NFC. macOS servers may send decomposed (NFD) names.
2. **Convert to MacRoman** with a table compiled into the app. Retro68's headers don't cover Text Encoding Converter, and a 128-entry table is simpler anyway.
3. **Translate `:` to `/`.** HFS allows `/` in names, and the Finder uses the same swap.
4. **Mangle names that don't fit.** If a name has characters MacRoman lacks, or is longer than 31 bytes, it becomes `<prefix>#XXXX<.ext>`:
   * `XXXX` is 4 hex digits of a hash of the full UTF-8 name.
   * The extension is kept (up to 5 characters), so type/creator mapping still works.
   * Example: `ThisIsAVeryLongSourceFileName_Helpers.c` → `ThisIsAVeryLongSourceF#3A9C.c`.
   * netatalk shows the same file to classic AFP clients as `…#<CNID>`, which is a different name. The names on the server don't change, so this difference doesn't matter.
5. **Case collisions.** If two names in a folder differ only by case, the first in rsync's sorted order keeps its name, the others are mangled as in rule 4, and a warning is printed.

The rule is deterministic, so later runs and `--delete` find the same local names with no lookup table. The reverse mapping only matters when the Mac is the *sender* (the stretch goal). It can then be kept in an invisible `.rsync-names` file per folder.

### 4.4 Resource forks and Finder info (`platform_classic.c`)

* **Receiving (G1).** openrsync sends `foo` before its synthetic `._foo` (see `platform_flist_received`). When `._foo` finishes, `platform_move_file()` unpacks it instead of storing it:
  * Entry 2 (resource fork) is written with `FSpOpenRF`.
  * Entry 9 (Finder info) is applied: FInfo with `FSpSetFInfo`, and FXInfo through `PBSetCatInfo`. The xattr area that `copyfile` appends is skipped.
  * Entry 8 (dates) sets the creation date.
  * A folder's Finder info is set through `PBSetCatInfo`.
  * If a local file has a fork or Finder info but the server sends no `._foo`, the fork and Finder info are cleared. This matches `FLAG_CLEAR_METADATA` in Apple's code.
* **Packing.** This builds an AppleDouble from a local file's fork and Finder info. It is needed:
  * as the basis for delta transfer of `._foo`,
  * for the quick check, and
  * on the sender side in local mode and push.

  Byte-for-byte equality with the server's AppleDouble isn't required. Both sides only need to compare the same `._foo` the same way:
  * The local "size" of `._foo` is defined as the size the packer produces.
  * The server's synthetic entries carry the base file's mtime.
  * **Risk:** if the server packs extra data, such as Linux xattrs or macOS quarantine, the sizes never match. The `._foo` then gets a cheap re-check on every run. On a Mac server, the fix is to strip stray xattrs (`xattr -c`) or filter them out.
* **Type and creator (option, off by default).** `--mac-types` assigns type and creator to files that arrive *without* Finder info:
  * Mappings come from a **built-in extension table**, which prefs can extend. Internet Config isn't in Retro68's headers and isn't worth the glue code.
  * Prefs can override them, for example `.c .h .r .p → TEXT/KAHL` (THINK C) or `TEXT/CWIE` (CodeWarrior).
  * Files that arrived with Finder info are left alone.
* **CR/LF translation (option, off by default).** `--mac-text[=exts]` translates line endings:
  * Swapping LF and CR keeps the file the same length, so offsets, sizes and checksums still match. It is done in the read/write layer for the chosen files.
  * The generator checksums the swapped-back bytes, so delta transfer keeps working.
  * Files that contain CRLF are left alone, because converting them would change their length.

### 4.5 Front end

* **RetroConsole window** with an `rsync>` prompt. A tokenizer handles quotes and backslashes and builds `argv`.
* **`$(NAME)` variables** come from `rsync prefs` (a TEXT file in the Preferences folder). An example line is `SRV = 192.168.1.10`.
* **Saved profiles:** `profile name = <command line>`, run with `rsync> run name`. Dropping a profile document on the app runs it.
* **Dropping a folder** on the window inserts its HFS path, in quotes.
* **Progress and abort.** `-v` and `--progress` print to the window. Cmd-. aborts cleanly and removes temp files. The exit code is shown.
* **Memory.** The SIZE resource asks for 8 MB preferred and 4 MB minimum. openrsync keeps the whole file list in memory, at about 150 bytes plus path length per entry. With `-E` the entry count roughly doubles, so 20k files need about 6 MB.

## 5. Are delta ("partial") transfers worth it on a 68040?

**Short answer: not over Ethernet, for source trees. Default to `--whole-file`, and allow delta mode with `--no-whole-file`.**

What delta mode costs on the Mac, which is the receiver:

* For each changed file, the receiver reads its existing copy and computes a rolling checksum plus an MD4 per block, then sends those to the server. The server does the expensive search.
* The receiver then rebuilds the file from the old copy plus the literal bytes it receives.
* It also costs an extra round trip per file.

Rough numbers (Phase 0 measures them):

| | Throughput |
|---|---|
| MD4 on a 33–40 MHz 68040 | on the order of a few MB/s |
| Open Transport on a Quadra's built-in Ethernet | a few hundred KB/s to about 1 MB/s |
| LocalTalk (AFP over AppleTalk on serial ports) | about 20–30 KB/s |

So:

* **Most of the gain comes from the quick check, not delta.** rsync skips every file whose size and mtime match, and that works the same with `--whole-file`. Your case is a large tree where few files change, and that is exactly what the quick check covers.
* **Source files are small** (KB to tens of KB). Over Ethernet a whole-file copy takes milliseconds. Delta mode would spend about the same time on reading, checksums and the round trip, and save almost nothing.
* **Delta pays off for large files that change a little** (for example a multi-MB `.rsrc`, a project file or a library) over a **slow link** (LocalTalk, or a WAN). There, network time dominates and delta can be 10–100× faster.
* **Resource forks** go through the same logic. A small edit in a big fork is a good case for delta over a slow link.
* **In local mode (G1b) delta never helps.** The sender has to read the whole file over AFP to checksum it anyway. openrsync already defaults to `--whole-file` for local copies.

## 6. Testing on QEMU `q800`

* **Emulator:** QEMU `-M q800` (Quadra 800, 68040) boots Mac OS 7.1–8.1. That support was merged upstream in 2023, so use QEMU 8.2 or newer. Its SONIC Ethernet works with tap or bridged networking.
* **Needs from the owner:**
  * a Quadra 800 ROM image,
  * Mac OS 8.1 install media, which isn't freely licensed, so it can't go into CI from this repo.
  * Both are expected at a local path or as CI secrets.
* **Headless automation:**
  1. The test build of the app autoruns from Startup Items.
  2. It reads a script file from a second HFS disk image, runs the syncs, writes a manifest (names, data and fork hashes, type/creator, dates) to that disk, and shuts down.
  3. Linux reads the disk back with `hfsutils` (`hcopy -m` keeps forks) and compares the manifest with the fixtures on the server.
* **Server fixtures**, on a netatalk 4 host (both sidecar layouts) and on a Mac. Each test tree includes:
  * files with only a resource fork, and files with both forks
  * custom type/creator and folder Finder info
  * long, non-ASCII, `:`-containing and case-colliding names
  * empty, large, deleted and renamed files
* **Fast tests without the emulator:**
  * `platform_classic.c` pack/unpack and the name mapping are plain C, so they are unit-tested on Linux.
  * `macposix` is tested against a small fake File Manager on Linux.
  * openrsync's existing regression tests keep running on Linux.

## 7. Why THINK C 5 won't work for rsync itself

* openrsync is C99. It uses `//` comments, declarations mixed with code, `stdint.h`, `<stdbool.h>`, variadic macros and designated initializers.
* It needs **64-bit integers** for file sizes and offsets, because the protocol sends int64 values. THINK C 5 (1991) has none of this.
* The 2.6.9 code also needs `long long`. A THINK C port would mean rewriting large parts of the code.
* Retro68's GCC builds a normal 68k application that runs alongside THINK C on the target. THINK C stays the compiler for *your* project, and `--mac-types` can stamp synced files as `TEXT/KAHL` so they open in it.

## 8. What encryption would involve

* **Today:** the daemon's password check is challenge/response, so the password never crosses the wire in clear. The file data and names do.
* **Option A: TLS (recommended if needed).** This is the `rsync-ssl` / `rsync://` over TLS approach.
  * The server wraps the daemon in `stunnel` or HAProxy, or uses rsync 3.2's TLS support.
  * The Mac gets a small TLS 1.2 client. BearSSL is the best fit: small, no `malloc`, portable C.
  * The handshake (ECDHE plus RSA or ECDSA verification) takes a few seconds on a 68040. Bulk ChaCha20-Poly1305 or AES-GCM runs at roughly 0.5–2 MB/s, which is close to Ethernet speed.
  * Effort is about 2–3 weeks, mostly porting and testing, plus storing a CA or pinned key in prefs.
* **Option B: ssh** (a libssh2 or dropbear client port). This allows plain `host:path` with no daemon on the server, but it is several times the work of TLS and much heavier on a 68040.
* **Option C: keep the network trusted.** Use a LAN segment, or a VPN or WireGuard tunnel ending on a router or Pi in front of the Mac. This needs no code.

## 9. Phases (revised)

Progress so far:

| Phase | State |
|---|---|
| 0 | **Done.** Retro68 (68k) builds. QEMU q800 boots the owner's Mac OS 8.1 image unattended. `nettest` connects to an rsync daemon over MacTCP. Not yet measured: MD4 speed and TCP throughput |
| 1 | **Done.** openrsync builds on Linux (`portable.c`) and for 68k (`classic/rsync/config.h`, macposix). The daemon-client pull path needs no fork. Not done: local mode over threads |
| 2 | **Done.** `--netatalk` is implemented as a virtual `._name` layer, not as file-list rewriting (§3.2). `classic/tests/netatalk_test.sh` passes against a stock rsync 3.2.7 daemon. Out of scope: the `.AppleDouble/` v2 layout |
| 3 | **Done.** macposix covers files, directories, rename that keeps forks, times, MacTCP sockets, `poll` and MacTCP DNR host names. `hfsname.c` handles names. `mptest` passes 52/52 in QEMU |
| 4 | **Working in QEMU.** Pull with forks and Finder info, no-op second run, fork-only and data-only updates, replacing files, `--delete`, MacRoman and long names, DST tolerance. Not done: tests on real hardware and on the owner's netatalk server |
| 5 | **Mostly done.** The prompt, `$(VAR)` from `rsync prefs`, `set`, HFS path arguments, MacRoman console output, `--mac-types` and `--mac-text` work. Not done: profiles, drag and drop, Cmd-. |
| 6–8 | Not started |


| Phase | Work | Exit criterion |
|---|---|---|
| **0. Groundwork** | Build Retro68 (m68k) in Docker and CI. Bring up QEMU q800 with 8.1, OT and networking. Write a hello-world TCP client using OT (or MacTCP as fallback). Set up a netatalk 4 test server that mirrors the owner's, and answer the open checks in §3.1 item 5. Measure MD4 speed and TCP throughput on the emulator | Hello-world connects from the emulator. Fork storage is documented |
| **1. Prepare openrsync** | Put file, process and network calls behind `compat_*`. Add `__MACOS_CLASSIC__` guards. Make the daemon-client pull path free of fork. Abstract local mode over "thread or fork". Keep the Linux and macOS builds green | Existing tests pass |
| **2. Netatalk mapping (Linux-testable)** | `--netatalk` file-list rewriting, a v2 and "Mac OS X" AppleDouble parser/packer, and the size and mtime rules from §3.2. Test on Linux with a mock `platform_classic` that writes forks as `._` files, against a stock rsync daemon serving a copy of the owner's netatalk share.  | Second run against a fixed tree transfers 0 files. Fork and Finder-info bytes round-trip exactly |
| **3. Mac layers** | `macposix`, `macnet`, name mapping, time conversion, an `fts` replacement. Unit-test app in the emulator | Can list, create, swap and delete a tree, and do TCP echo |
| **4. MVP: pull with forks (G1)** | `platform_classic.c` unpack and pack, `--delete`, `--whole-file` by default. Test against the netatalk host (stock daemon, `--netatalk`) and a macOS daemon (`-E`) | Fixture tree matches exactly. A second run transfers 0 files. Editing one fork transfers only that file |
| **5. Front end** | Prompt, `$(VAR)`, profiles, drag and drop, Cmd-., progress. `--mac-types` and `--mac-text` options | Your workflow works end to end on QEMU, then on the real 68040 |
| **6. Local/AFP mode (G1b)** | Thread Manager server and fake socketpair | Sync from a mounted netatalk or AppleShare volume |
| 7. Stretch: push (G2) | OT listener, `daemon.c` without fork (one connection at a time), sender-side traversal, `.rsync-names` | A modern host pushes a tree to the Mac |
| 8. Optional: TLS | BearSSL client (§8) | `rsync-ssl`-style transfer works |

## 10. Remaining risks

* **MacTCP-over-OT throughput** on the emulator and on the real 68040 is measured in Phase 0.
* **netatalk metadata coverage:** type and creator of files *without* a resource fork may exist only in the xattr (§3.1, 5a). Deferred by the owner. The mitigation is `--mac-types`. A custom server is ruled out.
* **The netatalk name encoding** for characters the Mac allows but Linux doesn't (`/`), or for names netatalk can't convert, must be checked against afpd's on-disk encoding and reversed in §4.3.
* **Sidecar mtime behaviour:** measured (§3.1, 3). Handled by storing the sidecar mtime in the HFS backup date (§3.2).
* **Stability of the `._foo` quick check** (§3.2, §4.4). **DST shifts** are handled by the ±3600 s rule (§4.2).
* **Heap fragmentation** in a fixed partition on very large trees. Mitigations: grow the SIZE resource, and later borrow memory with `TempNewHandle`.
