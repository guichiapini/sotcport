#!/usr/bin/env python3
"""Convert BB_FRAME_DUMP_TRIGGER dumps (<name>_<index>_<w>x<h>.raw, 32-bit pixels) to PNG.

The presented image is B8G8R8A8; --rgba reads R8G8B8A8 instead. --scale N keeps every Nth
pixel (smaller files to look at). Alpha is dropped.
"""
import argparse
import re
import struct
import zlib
from pathlib import Path


def write_png(path, width, height, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + row for row in rows)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def convert(source, rgba, scale):
    w, h = map(int, re.search(r'_(\d+)x(\d+)\.raw$', source.name).groups())
    data = source.read_bytes()
    order = (0, 1, 2) if rgba else (2, 1, 0)
    rows = []
    for y in range(0, h, scale):
        line = data[y * w * 4:(y + 1) * w * 4]
        row = bytearray()
        for x in range(0, w, scale):
            p = line[x * 4:x * 4 + 4]
            row += bytes((p[order[0]], p[order[1]], p[order[2]]))
        rows.append(bytes(row))
    target = source.with_suffix('.png')
    write_png(target, len(rows[0]) // 3, len(rows), rows)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=Path, nargs='+')
    parser.add_argument('--rgba', action='store_true')
    parser.add_argument('--scale', type=int, default=2)
    args = parser.parse_args()
    for raw in args.raw:
        print(convert(raw, args.rgba, args.scale))
