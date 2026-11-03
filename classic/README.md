# rsync for Classic Mac OS (68k, Mac OS 8.1)

This is a port of `openrsync/` (Apple's current rsync, protocol 29) to Classic Mac OS, built with [Retro68](https://github.com/autc04/Retro68). It pulls a tree from an rsync daemon and keeps resource forks and Finder info. The design and the reasons behind it are in [`docs/CLASSIC_MACOS_PORT.md`](../docs/CLASSIC_MACOS_PORT.md).

## Using it

The app opens a console window with an `rsync>` prompt.

```
rsync -av --delete 192.168.1.10::src/MyApp/ "Macintosh HD:Projects:MyApp:"
rsync -av --delete $(SRV)::src/MyApp/ :MyApp:
```

* **Remote side:** an rsync daemon (`host::module/path` or `rsync://host/module/path`). The Mac has no ssh, so `host:path` doesn't work.
* **Local side:** an HFS path (`Disk:Folder:`, or `:Folder:` relative to the app). A trailing colon means "the folder's contents", like a trailing `/` in rsync.
* **`$(NAME)`:** taken from a text file `rsync prefs` next to the app, which holds lines like `SRV = 192.168.1.10`.
* **Resource forks:** on by default (`--netatalk`), described below. Turn off with `--no-netatalk`.
* **Debugging:** `trace on` / `trace off` logs every file and network call. `quit` leaves the app.

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
| `rsync/` | The application: front end (`front.c`), 68k `config.h`, `SIZE` resource |
| `../openrsync/netatalk.c` | `--netatalk` mode (the virtual `._name` layer), shared with the Unix build |
| `../openrsync/hfsname.c` | UTF-8 to HFS names: MacRoman, 31-byte limit with hash-suffixed mangling, `:` to `/`, case collisions |
| `mptest/`, `nettest/` | Self-tests that run inside Mac OS |
| `tests/` | Host tests (`make -C classic/tests check`): `--netatalk` end to end against a stock rsync daemon, name rules, printf |
| `emu/` | QEMU q800 scripts: boot unattended, run a test app or an rsync script, read the log back with `hfsutils` |

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

Not done yet:

* **Host names.** The MacTCP DNR resolver doesn't load yet, so use IP addresses.
* **DST.** No tolerance yet for the one-hour shift after a change: plan §4.2 calls for treating a difference of exactly ±3600 s, with equal size, as unchanged.
* **`--mac-types` and `--mac-text`** (type/creator mapping and CR/LF translation; plan §4.4).
* **Local copies** (two Mac paths, e.g. from a mounted AFP volume) need a thread-based server (plan §3.6).
* **Daemon mode** on the Mac (push from another host) is not implemented.
* **Cmd-.** to abort a transfer.
* **Real hardware.** Not yet run on a 68040 or against the owner's netatalk server, and speed has not been measured.
* **Repeated commands.** Several commands in one session reset getopt, verbosity and filter rules. Other global state in openrsync may still carry over, so if a second command misbehaves, quit and relaunch.
