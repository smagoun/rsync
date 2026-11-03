#!/bin/sh
# Package the built app for download:
#   <out>/rsync-classic.img   1.44 MB HFS disk image (UTM/QEMU, or dd to a floppy)
#   <out>/rsync.bin           the application as MacBinary (StuffIt Expander)
# usage: classic/emu/make-dist.sh <out-dir> [path/to/rsync.bin]
set -e
S="$(cd "$(dirname "$0")" && pwd)"
OUT=$1
APP=${2:-$S/../../../Retro68-build/classic/rsync/rsync.bin}
mkdir -p "$OUT"
T=$(mktemp -d)
export HOME=$T
cat > "$T/readme" <<'TXT'
rsync for Mac OS (68k)

Open "rsync" and type a command at the rsync> prompt, for example:

  rsync -av --delete 192.168.1.10::src/MyApp/ "Macintosh HD:Projects:MyApp:"

- The server must run an rsync daemon (host::module/path or
  rsync://host/module/path).  There is no ssh on the Mac.
- Local paths are HFS paths: Disk:Folder: or :Folder: (relative to the
  app).  A trailing colon means "the contents of this folder".
- Resource forks and Finder info come from netatalk "._name" files on the
  server (on by default; --no-netatalk turns it off).
- $(NAME) in a command is taken from the "rsync prefs" file next to the
  app, which has lines like:  SRV = 192.168.1.10
- --mac-types gives files without Finder info on the server a type and
  creator by name (e.g. .c -> TEXT/ttxt).  --mac-types=FILE adds your
  own, one per line:  .c TEXT KAHL
- --mac-text stores source/text files with Mac (CR) line endings;
  --mac-text=.c,.h,.r picks the extensions.  Off by default.
- Dates shifted by exactly one hour (daylight saving time) count as
  unchanged; --no-dst-window turns that off.
- Host names work if TCP/IP has a name server configured.
- "trace on" logs file and network calls; "set NAME=VALUE" sets an
  environment variable (RSYNC_GMT_OFFSET=seconds pins the time zone);
  "quit" leaves.

Give rsync at least 8 MB (Get Info > Memory) for large trees.
TXT
printf '# rsync prefs: NAME = value, used as $(NAME) in commands\n# SRV = 192.168.1.10\n' > "$T/prefs"
dd if=/dev/zero of="$OUT/rsync-classic.img" bs=1024 count=1440 2>/dev/null
hformat -l "rsync" "$OUT/rsync-classic.img" >/dev/null
hmount "$OUT/rsync-classic.img" >/dev/null
hcopy -m "$APP" ":"
tr '\n' '\r' < "$T/readme" > "$T/readme.cr"; hcopy -r "$T/readme.cr" ":Read Me"
hattrib -t TEXT -c ttxt ":Read Me"
tr '\n' '\r' < "$T/prefs" > "$T/prefs.cr"; hcopy -r "$T/prefs.cr" ":rsync prefs (sample)"
hattrib -t TEXT -c ttxt ":rsync prefs (sample)"
hls -l
humount >/dev/null
cp "$APP" "$OUT/rsync.bin"
rm -rf "$T"
