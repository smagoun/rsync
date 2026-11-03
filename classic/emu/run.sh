#!/bin/sh
# Boot the Mac OS 8.1 test VM headless. Monitor on emu/monitor.sock.
S="$(cd "$(dirname "$0")" && pwd)"; cd "${EMU_DIR:-$S/../../emu}"
exec qemu-system-m68k -M q800,audiodev=snd0 -audiodev none,id=snd0 -m 32 -bios q800.rom \
  -drive file=pram.qcow2,format=qcow2,if=mtd \
  -device scsi-hd,scsi-id=0,drive=hd0 -drive file=hd.raw,format=raw,if=none,id=hd0 \
  -nic user,model=dp83932,mac=EA:CE:E5:3F:69:97 \
  -rtc base=localtime -display none \
  -monitor unix:monitor.sock,server,nowait -serial none -pidfile qemu.pid "$@"
