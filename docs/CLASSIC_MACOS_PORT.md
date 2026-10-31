# Plan: rsync for Classic Mac OS 8.1 with resource-fork support

Status: **plan, revision 2** (it now covers the owner's answers). Nothing is implemented yet.

## 0. Decisions so far

| Topic | Decision |
|---|---|
| Target | 68040 with 32 MB RAM, Mac OS 8.1, HFS disk. Built as a classic 68k app (not CFM-68K) |
| Toolchain | **Retro68 only** for rsync itself (see §7 on why THINK C 5 won't work) |
| Servers | (a) a modern Mac, or (b) Linux running **netatalk 4.x**. Optionally (c) any AFP server mounted on the classic Mac |
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

The owner didn't know where netatalk 4 keeps resource forks. Here is what the documentation and reports say:

* netatalk 4's `afp.conf` option `ea` defaults to auto. It tries `sys` (filesystem extended attributes) first and falls back to `ad` (AppleDouble v2 files in `.AppleDouble/` folders) if the filesystem has no xattrs.
* With `ea = sys`, the Finder info, dates and AFP metadata go in the xattr `user.org.netatalk.Metadata`.
* Reports disagree on where the resource fork goes with `ea = sys`. Some say a Mac-OS-X-style `._foo` file next to the data file, others say an xattr (`org.netatalk.ResourceFork`, used on ZFS/Solaris and in newer builds).

**Step one of Phase 0 is to check this on a real netatalk 4 volume.** The design below handles all three layouts, so the answer changes only which reader code runs.

A stock rsync 3.x server can't send any of this. openrsync only speaks protocol 29, which has no xattr support, and even rsync 3's `-X` sends raw xattrs that would need netatalk-specific decoding on the Mac. So the plan uses **one wire format, Apple's `-E`**, and makes every server speak it:

| Server | What runs on it | How forks are read |
|---|---|---|
| Modern Mac | Apple's stock `/usr/bin/rsync --daemon` (openrsync, or rsync 2.6.9 on macOS ≤12), run with `-E` | Native, through `copyfile()` |
| Linux + netatalk 4 | **`rsync-netatalk`**: this repo's openrsync built for Linux, with a new `platform_netatalk.c` hook layer | Builds the `._foo` AppleDouble from `user.org.netatalk.Metadata` (Finder info, dates) plus the resource fork, taken from `._foo`, the `org.netatalk.ResourceFork` xattr, or `.AppleDouble/foo`, whichever exists |
| Any AFP server (real Mac, netatalk, AppleShare) | Nothing. The volume is mounted on the classic Mac (G1b) | The File Manager on the Mac reads them directly |

`rsync-netatalk` should be read-only toward the netatalk metadata at first. The stretch goal (push to the Mac) doesn't need it to write, and pulling from Linux never writes to the server.

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
* **Server fixtures**, on a netatalk 4 container and a Mac (or the `rsync-netatalk` daemon). Each test tree includes:
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
| **0. Groundwork** | Build Retro68 (m68k) in Docker and CI. Bring up QEMU q800 with 8.1, OT and networking. Write a hello-world TCP client using OT (or MacTCP as fallback). Set up a netatalk 4 test server and **find out where it stores forks** (§3). Measure MD4 speed and TCP throughput on the emulator | Hello-world connects from the emulator. Fork storage is documented |
| **1. Prepare openrsync** | Put file, process and network calls behind `compat_*`. Add `__MACOS_CLASSIC__` guards. Make the daemon-client pull path free of fork. Abstract local mode over "thread or fork". Keep the Linux and macOS builds green | Existing tests pass |
| **2. `rsync-netatalk` server** | `platform_netatalk.c`: build `._foo` AppleDouble from netatalk storage, and reuse Apple's `-E` code path. Test against Apple's macOS rsync running as the client | A macOS `rsync -E` pull from the Linux daemon recreates forks and Finder info exactly |
| **3. Mac layers** | `macposix`, `macnet`, name mapping, time conversion, an `fts` replacement. Unit-test app in the emulator | Can list, create, swap and delete a tree, and do TCP echo |
| **4. MVP: pull with forks (G1)** | `platform_classic.c` unpack and pack, `--delete`, `--whole-file` by default. Test against a macOS daemon and the `rsync-netatalk` daemon | Fixture tree matches exactly. A second run transfers 0 files. Editing one fork transfers only that file |
| **5. Front end** | Prompt, `$(VAR)`, profiles, drag and drop, Cmd-., progress. `--mac-types` and `--mac-text` options | Your workflow works end to end on QEMU, then on the real 68040 |
| **6. Local/AFP mode (G1b)** | Thread Manager server and fake socketpair | Sync from a mounted netatalk or AppleShare volume |
| 7. Stretch: push (G2) | OT listener, `daemon.c` without fork (one connection at a time), sender-side traversal, `.rsync-names` | A modern host pushes a tree to the Mac |
| 8. Optional: TLS | BearSSL client (§8) | `rsync-ssl`-style transfer works |

## 10. Remaining risks

* **OT glue libraries for 68k under Retro68** (the MacTCP fallback is identified).
* **netatalk 4 fork storage** varies by version and `ea` mode. The reader handles all three layouts, but this must be checked in Phase 0.
* **Stability of the `._foo` quick check** (§4.4), and **DST shifts** in mtimes (§4.2).
* **Heap fragmentation** in a fixed partition on very large trees. Mitigations: grow the SIZE resource, and later borrow memory with `TempNewHandle`.
