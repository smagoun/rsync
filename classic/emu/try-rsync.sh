#!/bin/sh
# Install the freshly built rsync app in the VM's Startup Items, give it an
# "rsync script" (arguments: one command per argument), boot unattended and
# print "rsync log".  Example:
#   classic/emu/try-rsync.sh "rsync -av rsync://10.0.2.2:8873/src/ :dest:"
S="$(cd "$(dirname "$0")" && pwd)"
EMU=${EMU_DIR:-$S/../../emu}
APP=${RSYNC_APP:-$S/../../../Retro68-build/classic/rsync/rsync.bin}
SI=":System Folder:Startup Items"
export HOME=${TMPDIR:-/tmp}
T=$(mktemp -d)
for cmd in "$@"; do printf '%s\r' "$cmd"; done > "$T/script"
echo x > "$T/aq"
cd "$EMU" || exit 1
hmount hd.raw 1 >/dev/null || exit 1
for f in rsync "rsync script" "rsync log" "rsync autoquit"; do
	hdel "$SI:$f" 2>/dev/null
done
hcopy -m "$APP" "$SI:" && hcopy -r "$T/script" "$SI:rsync script" &&
    hcopy -r "$T/aq" "$SI:rsync autoquit"
# Invisible, so the Finder (7.1 opens every file in Startup Items) skips them.
hattrib -t TEXT -c rSnc +i "$SI:rsync script"
hattrib -t TEXT -c rSnc +i "$SI:rsync autoquit"
humount >/dev/null
rm -rf "$T"
TIMEOUT=${TIMEOUT:-300} "$S/boot-test.sh" "$SI:rsync log" | tail -n +7
