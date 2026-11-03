#!/bin/sh
# Package the built app for download:
#   <out>/rsync-classic.img   1.44 MB HFS disk image (UTM/QEMU, or dd to a floppy)
#   <out>/rsync.bin           the application as MacBinary (StuffIt Expander)
#   <out>/rsync-console.bin   the console-only version, as MacBinary
# usage: classic/emu/make-dist.sh <out-dir> [gui.bin] [console.bin]
set -e
S="$(cd "$(dirname "$0")" && pwd)"
OUT=$1
APP=${2:-$S/../../../Retro68-build/classic/gui/rsyncgui.bin}
CLI=${3:-$S/../../../Retro68-build/classic/rsync/rsync.bin}
mkdir -p "$OUT"
T=$(mktemp -d)
export HOME=$T
cat > "$T/readme" <<'TXT'
rsync for Mac OS (68k)

"rsync" opens two windows:

  rsync Bookmarks   your saved syncs.  New... makes one; double-click
                    (or Edit...) opens one for review; Sync runs it,
                    Preview shows what would change (dry run).
  rsync Console     the command line: type rsync commands, Up and Down
                    arrows recall earlier ones, "help" lists built-ins.

Each bookmark names a server (an rsync daemon), a module and path on
it, a folder on this Mac, and the options: resource forks (netatalk
"._" files), Apple -E (when the server is a Mac running OS X), delete
extras, type/creator by name, Mac line endings, daylight-saving
tolerance, compression.  Bookmarks are kept in the text file
"rsync Bookmarks" next to the app.

Server: when "rsyncd.conf" is next to the app, rsync also serves its
folders to other computers, so they can push files here:

    rsync -av -E dir/ rsync://<this Mac's address>/incoming/

(-E carries resource forks from a Mac running OS X.)  The status line
in the Bookmarks window shows the address.  Server > Start Server at
Launch turns the automatic start off.  A sample rsyncd.conf is on this
disk.

Local paths in console commands are HFS paths: Disk:Folder: or
:Folder: (relative to the app).  $(NAME) in a command comes from an
"rsync prefs" file next to the app (lines like  SRV = 192.168.1.10).

"rsync console" is the same engine without windows (a plain command
prompt), for scripting.

Give rsync at least 8 MB (Get Info > Memory) for large trees.
TXT
printf '# rsync prefs: NAME = value, used as $(NAME) in commands\n# SRV = 192.168.1.10\n' > "$T/prefs"
dd if=/dev/zero of="$OUT/rsync-classic.img" bs=1024 count=1440 2>/dev/null
hformat -l "rsync" "$OUT/rsync-classic.img" >/dev/null
hmount "$OUT/rsync-classic.img" >/dev/null
hcopy -m "$APP" ":rsync"
hcopy -m "$CLI" ":rsync console"
printf '# rsyncd.conf: folders this Mac serves (paths as /Disk/Folder)\n[incoming]\n    path = /Macintosh HD/Incoming\n    read only = no\n    use chroot = no\n' | tr '\n' '\r' > "$T/conf"
hcopy -r "$T/conf" ":rsyncd.conf (sample)"
hattrib -t TEXT -c ttxt ":rsyncd.conf (sample)"
tr '\n' '\r' < "$T/readme" > "$T/readme.cr"; hcopy -r "$T/readme.cr" ":Read Me"
hattrib -t TEXT -c ttxt ":Read Me"
tr '\n' '\r' < "$T/prefs" > "$T/prefs.cr"; hcopy -r "$T/prefs.cr" ":rsync prefs (sample)"
hattrib -t TEXT -c ttxt ":rsync prefs (sample)"
hls -l
humount >/dev/null
cp "$APP" "$OUT/rsync.bin"
cp "$CLI" "$OUT/rsync-console.bin"
rm -rf "$T"
