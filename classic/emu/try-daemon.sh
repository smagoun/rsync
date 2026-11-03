#!/bin/sh
# Test the Mac-side daemon: boot the VM with rsync running "rsync --daemon",
# wait until it answers on localhost:$RSYNC_FWD_PORT (QEMU forwards it to
# the Mac's port 873), run the given shell commands (pushes from this
# host), stop the daemon with Cmd-. and print "rsync log".
#   classic/emu/try-daemon.sh 'rsync -av tree/ rsync://127.0.0.1:8730/incoming/'
# rsyncd.conf for the Mac comes from $RSYNCD_CONF (default: one writable
# module "incoming" at /<boot volume>/Incoming).
S="$(cd "$(dirname "$0")" && pwd)"
EMU=${EMU_DIR:-$S/../../emu}
APP=${RSYNC_APP:-$S/../../../Retro68-build/classic/rsync/rsync.bin}
PORT=${RSYNC_FWD_PORT:-8730}
SI=":System Folder:Startup Items"
export HOME=${TMPDIR:-/tmp}
T=$(mktemp -d)
VOL=$(cd "$EMU" && hmount hd.raw 1 | sed -n 's/^Volume name is "\(.*\)"$/\1/p'; humount >/dev/null)
if [ -n "$RSYNCD_CONF" ]; then cp "$RSYNCD_CONF" "$T/conf"; else
	printf '[incoming]\n    path = /%s/Incoming\n    read only = no\n    use chroot = no\n' "$VOL" > "$T/conf"
fi
printf '%s\r' "${DAEMON_CMD:-rsync --daemon -v}" > "$T/script"
echo x > "$T/aq"
cd "$EMU" || exit 1
hmount hd.raw 1 >/dev/null || exit 1
for f in rsync "rsync script" "rsync log" "rsync autoquit" rsyncd.conf; do
	hdel "$SI:$f" 2>/dev/null
done
hmkdir ":Incoming" 2>/dev/null
hcopy -m "$APP" "$SI:"
tr '\n' '\r' < "$T/conf" > "$T/conf.cr"; hcopy -r "$T/conf.cr" "$SI:rsyncd.conf"
hcopy -r "$T/script" "$SI:rsync script"
hcopy -r "$T/aq" "$SI:rsync autoquit"
# Invisible, so the Finder (7.1 opens every file in Startup Items) skips them.
for f in rsyncd.conf "rsync script" "rsync autoquit"; do
	hattrib -t TEXT -c rSnc +i "$SI:$f"
done
humount >/dev/null
rm -rf "$T"

rm -f qemu.pid
setsid "$S"/run.sh > qemu.log 2>&1 &
sleep 2
alive() { [ -f qemu.pid ] && kill -0 "$(cat qemu.pid)" 2>/dev/null; }
sleep 40; "$S"/mon.sh "sendkey ret" >/dev/null 2>&1
t=0
until timeout 10 rsync "rsync://127.0.0.1:$PORT/" >/dev/null 2>&1; do
	sleep 5; t=$((t + 5))
	if [ $t -gt 120 ] || ! alive; then echo "daemon did not come up"; break; fi
done
for cmd in "$@"; do
	echo "\$ $cmd"
	sh -c "$cmd" 2>&1
	echo "[host exit $?]"
done
# Cmd-. stops the daemon; a synthetic key press can be missed, so repeat.
t=0; while alive && [ $t -lt 90 ]; do
	"$S"/mon.sh "sendkey meta_l-dot 200" >/dev/null 2>&1
	sleep 5; t=$((t+5))
done
if alive; then
	"$S"/mon.sh "screendump $PWD/shot.ppm" >/dev/null
	python3 -c "from PIL import Image; Image.open('shot.ppm').save('shot.png')"
	"$S"/mon.sh quit >/dev/null; echo "TIMEOUT (screenshot in emu/shot.png)"; sleep 1
fi
hmount hd.raw 1 >/dev/null && hcopy -t "$SI:rsync log" - | tr '\r' '\n' | tail -n +8
humount >/dev/null
