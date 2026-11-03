#!/bin/sh
# Send one command to the QEMU monitor: ./mon.sh "screendump shot.ppm"
S="$(cd "$(dirname "$0")" && pwd)"; cd "${EMU_DIR:-$S/../../emu}"
python3 - "$*" <<'PY'
import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect("monitor.sock"); s.settimeout(1)
def rd():
    out=b""
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            out+=d
    except socket.timeout: pass
    return out
rd(); s.sendall(sys.argv[1].encode()+b"\n"); time.sleep(0.5); print(rd().decode(errors="replace"))
PY
