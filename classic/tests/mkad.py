#!/usr/bin/env python3
"""Write a netatalk-style AppleDouble sidecar ("._name").

usage: mkad.py OUT TYPE CREATOR [RSRC_FILE | -] [FLAGS_HEX]
The layout matches what afpd 4.x writes with "ea = sys": 82-byte header,
filler "Netatalk", Finder info at 50, resource fork at 82.
"""
import struct
import sys

out, ftype, creator = sys.argv[1], sys.argv[2], sys.argv[3]
rsrc = b""
if len(sys.argv) > 4 and sys.argv[4] != "-":
    rsrc = open(sys.argv[4], "rb").read()
flags = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0
finfo = (ftype.encode("mac_roman") + creator.encode("mac_roman") +
         struct.pack(">Hhh", flags, -1, -1)).ljust(32, b"\0")
hdr = struct.pack(">II16sH", 0x00051607, 0x00020000, b"Netatalk        ", 2)
hdr += struct.pack(">III", 9, 50, 32) + struct.pack(">III", 2, 82, len(rsrc))
open(out, "wb").write(hdr + finfo + rsrc)
