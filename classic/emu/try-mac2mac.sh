#!/bin/sh
# Mac to Mac: boot the server VM running "rsync --daemon" (module "incoming"
# at /<its volume>/Incoming), then boot the client VM to run the given
# commands, then stop the server.  QEMU's user networking keeps each VM on
# its own network, so the client reaches the server through the server's
# port forward on the host: rsync://10.0.2.2:8730/incoming/.
#   SERVER_DIR=emu CLIENT_DIR=emu/s71 classic/emu/try-mac2mac.sh \
#       'rsync -a "25 MB:Data:" rsync://10.0.2.2:8730/incoming/data/'
# Prints the client's log, then the server's.
S="$(cd "$(dirname "$0")" && pwd)"
SRV=$(cd "${SERVER_DIR:-$S/../../emu}" && pwd)
CLI=$(cd "${CLIENT_DIR:-$S/../../emu/s71}" && pwd)
APP=${RSYNC_APP:-$S/../../../Retro68-build/classic/rsync/rsync.bin}
SI=":System Folder:Startup Items"
export HOME=${TMPDIR:-/tmp}

# Server: the app, a config, and a one-line script.
T=$(mktemp -d)
cd "$SRV" || exit 1
VOL=$(hmount hd.raw 1 | sed -n 's/^Volume name is "\(.*\)"$/\1/p')
printf '[incoming]\r    path = /%s/Incoming\r    read only = no\r    use chroot = no\r' \
    "$VOL" > "$T/conf"
printf 'rsync --daemon -v\r' > "$T/script"
echo x > "$T/aq"
for f in rsync "rsync script" "rsync log" "rsync autoquit" rsyncd.conf; do
	hdel "$SI:$f" 2>/dev/null
done
hmkdir ":Incoming" 2>/dev/null
hcopy -m "$APP" "$SI:"
hcopy -r "$T/conf" "$SI:rsyncd.conf"
hcopy -r "$T/script" "$SI:rsync script"
hcopy -r "$T/aq" "$SI:rsync autoquit"
for f in rsyncd.conf "rsync script" "rsync autoquit"; do
	hattrib -t TEXT -c rSnc +i "$SI:$f"	# hidden from 7.1's Finder
done
humount >/dev/null
rm -rf "$T"

rm -f qemu.pid
EMU_DIR=$SRV RSYNC_FWD_PORT=8730 setsid "$S"/run.sh > qemu.log 2>&1 &
sleep 2
alive() { [ -f "$SRV/qemu.pid" ] && kill -0 "$(cat "$SRV/qemu.pid")" 2>/dev/null; }
mon() { EMU_DIR=$SRV "$S"/mon.sh "$@" >/dev/null 2>&1; }
sleep 40; mon "sendkey ret"		# the improper-shutdown alert, if any
t=0
until timeout 10 rsync rsync://127.0.0.1:8730/ >/dev/null 2>&1; do
	sleep 5; t=$((t + 5))
	if [ $t -gt 120 ] || ! alive; then echo "server did not come up"; break; fi
done
echo "== server ($VOL) up; client:"

EMU_DIR=$CLI RSYNC_FWD_PORT=8731 TIMEOUT=${TIMEOUT:-900} "$S"/try-rsync.sh "$@"

echo "== server log:"
t=0; while alive && [ $t -lt 90 ]; do
	mon "sendkey meta_l-dot 200"; sleep 5; t=$((t + 5))
done
if alive; then mon "quit"; echo "server TIMEOUT"; sleep 1; fi
cd "$SRV" && hmount hd.raw 1 >/dev/null &&
    hcopy -t "$SI:rsync log" - | tr '\r' '\n' | tail -n +8
humount >/dev/null
