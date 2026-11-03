#!/bin/sh
# Boot headless, dismiss the "improper shutdown" alert if present, wait for the
# guest to power itself off (or time out), then print the given log file.
S="$(cd "$(dirname "$0")" && pwd)"; cd "${EMU_DIR:-$S/../../emu}"
rm -f qemu.pid; setsid "$S"/run.sh > qemu.log 2>&1 &
sleep 2
alive() { [ -f qemu.pid ] && kill -0 "$(cat qemu.pid)" 2>/dev/null; }
sleep 45; "$S"/mon.sh "sendkey ret" >/dev/null 2>&1
t=0; while alive && [ $t -lt ${TIMEOUT:-300} ]; do sleep 5; t=$((t+5)); done
if alive; then
  "$S"/mon.sh "screendump $PWD/shot.ppm" >/dev/null; python3 -c "from PIL import Image; Image.open('shot.ppm').save('shot.png')"
  "$S"/mon.sh quit >/dev/null; echo "TIMEOUT (screenshot in emu/shot.png)"; sleep 1
fi
[ -n "$1" ] && HOME=${TMPDIR:-/tmp} sh -c "hmount hd.raw 1 >/dev/null && hcopy -t \"$1\" - | tr '\r' '\n'; humount >/dev/null"
