#!/usr/bin/env python3
"""MacBinary II helper for the emulator tests (hfsutils' hcopy -m format).

    macbin.py make OUT.bin NAME TYPE CREATOR DATAFILE|- RSRCFILE|-
    macbin.py sum [-m] FILE.bin...  one line per file: name type/creator
                                  data and resource fork sizes and MD5s;
                                  -m ignores resource fork bytes 48-125, where
                                  System 7.1 stores its own record

NAME is MacRoman-encoded from UTF-8; "-" means an empty fork.
"""
import hashlib, os, struct, sys


def crc16(b):
    crc = 0
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def pad(b):
    return b + bytes(-len(b) % 128)


def make(out, name, ftype, creator, dpath, rpath):
    data = b"" if dpath == "-" else open(dpath, "rb").read()
    rsrc = b"" if rpath == "-" else open(rpath, "rb").read()
    n = name.encode("mac_roman")
    h = bytearray(128)
    h[1] = len(n)
    h[2:2 + len(n)] = n
    h[65:69] = ftype.encode("mac_roman")
    h[69:73] = creator.encode("mac_roman")
    struct.pack_into(">II", h, 83, len(data), len(rsrc))
    t = int(os.environ.get("MACBIN_DATE", "0xB0000000"), 0)  # 1997
    struct.pack_into(">II", h, 91, t, t)
    h[122] = h[123] = 129
    struct.pack_into(">H", h, 124, crc16(bytes(h[:124])))
    open(out, "wb").write(bytes(h) + pad(data) + pad(rsrc))


def summary(path, mask=False):
    b = open(path, "rb").read()
    n = b[2:2 + b[1]].decode("mac_roman")
    dl, rl = struct.unpack(">II", b[83:91])
    data = b[128:128 + dl]
    rsrc = b[128 + (dl + 127) // 128 * 128:][:rl]
    if mask and len(rsrc) > 48:
        rsrc = rsrc[:48] + bytes(min(78, len(rsrc) - 48)) + rsrc[126:]
    md5 = lambda x: hashlib.md5(x).hexdigest()[:8]
    return "%-32s %s/%s data %7d %s rsrc %7d %s" % (
        n, b[65:69].decode("mac_roman"), b[69:73].decode("mac_roman"),
        dl, md5(data), rl, md5(rsrc))


if __name__ == "__main__":
    if len(sys.argv) == 8 and sys.argv[1] == "make":
        make(*sys.argv[2:])
    elif len(sys.argv) > 2 and sys.argv[1] == "sum":
        mask = sys.argv[2] == "-m"
        for p in sys.argv[2 + mask:]:
            print(summary(p, mask))
    else:
        sys.exit(__doc__)
