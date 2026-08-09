#!/usr/bin/env python3
"""Count lit pixels in a MAME snapshot, with no third party modules.

Used by run.sh to decide whether the emulated MAD-1 actually reached the DOS
directory listing: the power-on messages alone light up a few hundred pixels,
a full DIR listing lights up thousands.
"""
import struct
import sys
import zlib


def lit_pixels(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s: not a PNG" % path)

    pos, idat, width = 8, bytearray(), None
    while pos < len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack_from(">IIBB", body, 0)
            if depth != 8 or colour not in (2, 6):
                raise SystemExit("unexpected PNG format: depth %d colour %d" % (depth, colour))
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += length + 12

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    prev = bytearray(stride)
    out = 0
    at = 0
    for _ in range(height):
        filt = raw[at]; at += 1
        line = bytearray(raw[at:at + stride]); at += stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xff
            elif filt == 2:
                line[i] = (line[i] + b) & 0xff
            elif filt == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        for i in range(0, stride, channels):
            if line[i] or line[i + 1] or line[i + 2]:
                out += 1
        prev = line
    return out


if __name__ == "__main__":
    n = lit_pixels(sys.argv[1])
    print(n)
    if len(sys.argv) > 2:
        sys.exit(0 if n >= int(sys.argv[2]) else 1)
