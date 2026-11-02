# Plan: rsync for Classic Mac OS 8.1 with resource-fork support

Status: **plan, revision 3** (adds the netatalk `.AppleDouble` findings). Nothing is implemented yet.

## 0. Decisions so far

| Topic | Decision |
|---|---|
| Target | 68040 with 32 MB RAM, Mac OS 8.1, HFS disk. Built as a classic 68k app (not CFM-68K) |
| Toolchain | **Retro68 only** for rsync itself (see §7 on why THINK C 5 won't work) |
| Servers | (a) a modern Mac, or (b) Linux running **netatalk 4.x** (owner's: afpd 4.5.0dev, `ea = ad`, `.AppleDouble/` sidecars) with a **stock** rsync daemon. Optionally (c) any AFP server mounted on the classic Mac |
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

### 3.1 What the owner's netatalk server does (afpd 4.5.0dev)

The owner looked at a file that has a resource fork and no data fork:

```
'Civilization™ 1.1.2'                  0 bytes       <- data fork
'.AppleDouble/Civilization™ 1.1.2'    688339 bytes  <- AppleDouble v2 sidecar
```

So this volume uses **`ea = ad`**: netatalk's AppleDouble v2 "sidecar" layout. The sidecar layout is fixed by netatalk's `include/atalk/adouble.h`:

* **Header:**
  * magic `0x00051607`, version `0x00020000`, filler `"Netatalk        "`
  * 13 entries, making a **fixed 741-byte header** (`AD_DATASZ2`)
  * then the resource fork, which runs to the end of the file
* **Entries:**
  * name (3), comment (4), file dates (8), Finder info (9, 32 bytes), short name (13)
  * AFP file info (14), ProDOS info (11), DID (15)
  * netatalk private entries 16–19: device, inode, sync stamp, CNID
  * resource fork (2)
* **Size:** the sidecar is 741 bytes plus the resource fork length. For Civilization that means a 687,598-byte resource fork. *Check this against the Mac's Get Info.*
* **Folders:** a folder's Finder info lives in `<folder>/.AppleDouble/.Parent`.
* **Names:** the name on disk is UTF-8 (`™`), and netatalk translates it to MacRoman (`0xAA`) for classic clients. Our client-side mapping (§4.3) does the same, so names match what the Finder shows when it mounts the share.
* **Other modes, for completeness:**
  * With `ea = sys`, Finder info goes into the `user.org.netatalk.Metadata` xattr.
  * The resource fork then goes into a Mac-OS-X-style `._foo` file: 82-byte header, filler `"Mac OS X"`, entries 9 and 2.

### 3.2 Consequence: a **stock rsync daemon** on the Linux host is enough

With `ea = ad`, all metadata lives in ordinary files. Any rsync 3.x daemon on the Linux host sends `.AppleDouble/` folders like any other file. It speaks protocol 29, which openrsync uses. **No custom server software is needed.**

The classic client handles netatalk storage in a mode called `--netatalk` (`-N`), using only rsync's standard wire format. The mapping happens in `platform_flist_received()`, right after the file list arrives. File transfers are requested by list index, not by name, so renaming entries on the client side doesn't affect the server:

| Entry on the wire | What the client does |
|---|---|
| `dir/.AppleDouble/` (folder) | Removes it from the local view. No such folder is created on HFS |
| `dir/.AppleDouble/foo` | Renamed to a synthetic `dir/._foo` linked to `dir/foo`. After transfer it is unpacked by the same code as Apple's `-E` (§4.4): entry 2 becomes the resource fork, entry 9 the Finder info, entry 8 the creation date. Entries 3, 4, 11, 13–19 are ignored |
| `dir/.AppleDouble/.Parent` | The folder's Finder info (window position, view, flags) |
| `._foo` with filler `"Mac OS X"` (the `ea = sys` layout) | Same as above, for the fork. Finder info is used only if entry 9 is present |
| `.AppleDB/`, `.AppleDesktop/`, `Network Trash Folder`, `Temporary Items`, `.DS_Store`, `:2eDS_Store` | Excluded by a built-in filter list in `--netatalk` mode |

**How the quick check stays stable** (unchanged files must not be re-sent):

* **Size.** The local "size" of a virtual `.AppleDouble/foo` is computed as 741 plus the local resource fork length. That is exactly the netatalk sidecar size, so sizes match without packing anything.
* **Modification time.** HFS keeps *one* modification date per file, but the server has two: one for the data file and one for the sidecar. In `platform_flist_received()` the client gives both entries `max(data mtime, sidecar mtime)` and sets the HFS modification date to that value.
  * Unchanged files then match on both entries.
  * If either side changes, both entries are re-checked. In `--whole-file` mode both are re-sent, which is cheap for source files.
* **Basis for delta mode.** The local file is packed into a netatalk-layout v2 AppleDouble. The private entries are zeroed, but that only matters inside changed blocks.
* **`--delete`.**
  * When a sidecar disappears on the server, the client clears the local resource fork and Finder info.
  * When a data file disappears, the client deletes the HFS file, both forks together.

### 3.3 Server options

| Server | What runs on it | How forks reach the Mac |
|---|---|---|
| **Linux + netatalk, `ea = ad`** (owner's setup) | Stock `rsync --daemon` (3.x) with a module pointing at the netatalk share. Use `read only = yes` and `uid` set to the share owner | Client `--netatalk` mapping (§3.2) |
| Linux + netatalk, `ea = sys` | Stock rsync daemon **if** Finder info is optional, because forks come from `._foo` but type/creator lives in an xattr that protocol 29 can't send. Otherwise **`rsync-netatalk`**: this repo's openrsync built for Linux with a `platform_netatalk.c` that sends Apple's `-E` format | `--netatalk` or `-E` |
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

Because `--netatalk` needs no special server, `rsync-netatalk` drops out of the critical path. It is built only if `ea = sys` volumes with type/creator turn out to matter.

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
  * 68k apps reach OT through static glue libraries from Apple's Universal Interfaces/MPW. Retro68's `ConvertObj` can convert `OpenTransport.o`.
  * **Phase 0 checks this.** If the OT glue is a problem, the fallback is the MacTCP API, which OT supports for compatibility and which Multiversal Interfaces declares. Several Retro68 projects already use it.
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
  * A DST change would shift every mtime by an hour and force a re-check. To avoid that, pin the offset in prefs (`gmt-offset`), or use `--modify-window=3601` for that one run.
* **Directory traversal.** Write an `fts` replacement on top of indexed `PBGetCatInfo`. The `--delete` scan needs it, and so do local mode and push.
* **What doesn't exist on HFS:**
  * Symlinks: skip with a warning, or follow them with `-L` (`--copy-links`).
  * Hard links, devices, ownership and permission bits: ignored.

### 4.3 File names

The wire carries UTF-8 names. HFS holds names of at most 31 bytes, in MacRoman, without `:`, and case-insensitive. Names are mapped with **a deterministic rule that needs no stored state**:

1. **Normalize** to NFC. macOS servers may send decomposed (NFD) names.
2. **Convert to MacRoman** with a table compiled into the app. Text Encoding Converter isn't guaranteed on 8.1.
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
  * **Risk:** if the server packs extra data, such as Linux xattrs or macOS quarantine, the sizes never match. The `._foo` then gets a cheap re-check on every run. The fix is to make `rsync-netatalk` pack exactly the layout the Mac's packer produces, and on a Mac server to strip stray xattrs (`xattr -c`) or filter them out.
* **Type and creator (option, off by default).** `--mac-types` assigns type and creator to files that arrive *without* Finder info:
  * Mappings come from **Internet Config** (`ICMapFilename`, built into 8.1).
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
* **Server fixtures**, on a netatalk 4 (`ea = ad`) host with a stock rsync daemon, and on a Mac. Each test tree includes:
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

| Phase | Work | Exit criterion |
|---|---|---|
| **0. Groundwork** | Build Retro68 (m68k) in Docker and CI. Bring up QEMU q800 with 8.1, OT and networking. Write a hello-world TCP client using OT (or MacTCP as fallback). Set up a netatalk 4 (`ea = ad`) test server mirroring the owner's (§3.1). Measure MD4 speed and TCP throughput on the emulator | Hello-world connects from the emulator. Fork storage is documented |
| **1. Prepare openrsync** | Put file, process and network calls behind `compat_*`. Add `__MACOS_CLASSIC__` guards. Make the daemon-client pull path free of fork. Abstract local mode over "thread or fork". Keep the Linux and macOS builds green | Existing tests pass |
| **2. Netatalk mapping (Linux-testable)** | `--netatalk` file-list rewriting, a v2 and "Mac OS X" AppleDouble parser/packer, and the size and mtime rules from §3.2. Test on Linux with a mock `platform_classic` that writes forks as `._` files, against a stock rsync daemon serving a copy of the owner's netatalk share | Second run against a fixed tree transfers 0 files. Fork and Finder-info bytes round-trip exactly |
| **3. Mac layers** | `macposix`, `macnet`, name mapping, time conversion, an `fts` replacement. Unit-test app in the emulator | Can list, create, swap and delete a tree, and do TCP echo |
| **4. MVP: pull with forks (G1)** | `platform_classic.c` unpack and pack, `--delete`, `--whole-file` by default. Test against a stock rsync daemon on the netatalk host (`--netatalk`) and a macOS daemon (`-E`) | Fixture tree matches exactly. A second run transfers 0 files. Editing one fork transfers only that file |
| **5. Front end** | Prompt, `$(VAR)`, profiles, drag and drop, Cmd-., progress. `--mac-types` and `--mac-text` options | Your workflow works end to end on QEMU, then on the real 68040 |
| **6. Local/AFP mode (G1b)** | Thread Manager server and fake socketpair | Sync from a mounted netatalk or AppleShare volume |
| 7. Stretch: push (G2) | OT listener, `daemon.c` without fork (one connection at a time), sender-side traversal, `.rsync-names` | A modern host pushes a tree to the Mac |
| 8. Optional: TLS | BearSSL client (§8) | `rsync-ssl`-style transfer works |

## 10. Remaining risks

* **OT glue libraries for 68k under Retro68** (the MacTCP fallback is identified).
* **netatalk fork storage** is confirmed as `ea = ad` (`.AppleDouble/` v2) on the owner's afpd 4.5.0dev. If the owner later switches to `ea = sys`, or netatalk converts the volume (`convert appledouble`), the client must use the `._` mapping or `rsync-netatalk` (§3.3).
* **The netatalk name encoding** for characters the Mac allows but Linux doesn't (`/`), or for names netatalk can't convert, must be checked against afpd's on-disk encoding and reversed in §4.3.
* **Sidecar mtime behaviour:** check that afpd only touches the `.AppleDouble/foo` mtime when the metadata or fork really changes, not on every Finder open (for example when it rewrites the CNID or sync stamp). If it touches it too often, many files get needless re-checks. The fallback is to compare sidecars by size only.
* **Stability of the `._foo` quick check** (§4.4), and **DST shifts** in mtimes (§4.2).
* **Heap fragmentation** in a fixed partition on very large trees. Mitigations: grow the SIZE resource, and later borrow memory with `TempNewHandle`.
