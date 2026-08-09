#!/usr/bin/env python3
"""Convert an ImageDisk .IMD file to a raw sector image."""
import sys

SIZES = [128, 256, 512, 1024, 2048, 4096, 8192]


def convert(path_in, path_out):
    d = open(path_in, "rb").read()
    end = d.index(b"\x1a")
    print("comment:", d[:end].decode("latin-1").strip().replace("\r\n", " | "))
    p = end + 1

    tracks = {}
    while p < len(d):
        mode = d[p]; cyl = d[p + 1]; head_raw = d[p + 2]
        nsec = d[p + 3]; ssize = d[p + 4]
        p += 5
        head = head_raw & 0x0f
        smap = list(d[p:p + nsec]); p += nsec
        if head_raw & 0x80:  # cylinder map
            p += nsec
        if head_raw & 0x40:  # head map
            p += nsec
        if ssize == 0xff:
            raise SystemExit("per-sector size maps not handled")
        size = SIZES[ssize]

        sectors = {}
        for s in smap:
            t = d[p]; p += 1
            if t == 0:
                sectors[s] = b"\xf6" * size
            elif t in (1, 3, 5, 7):
                sectors[s] = d[p:p + size]; p += size
            elif t in (2, 4, 6, 8):
                sectors[s] = bytes([d[p]]) * size; p += 1
            else:
                raise SystemExit("bad sector type %d at %d" % (t, p))
        tracks[(cyl, head)] = (sorted(sectors), sectors, size, mode, nsec)

    cyls = max(c for c, h in tracks) + 1
    heads = max(h for c, h in tracks) + 1
    out = bytearray()
    for c in range(cyls):
        for h in range(heads):
            ids, sectors, size, mode, nsec = tracks[(c, h)]
            for s in ids:
                out += sectors[s]
    open(path_out, "wb").write(out)

    ids, sectors, size, mode, nsec = tracks[(0, 0)]
    print("geometry: %d cyl x %d head x %d sec x %d bytes = %d bytes" %
          (cyls, heads, nsec, size, len(out)))
    print("boot sector OEM:", bytes(out[3:11]))


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
