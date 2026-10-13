#!/usr/bin/env python3
"""List or extract files of a PSARC archive (CUSA08809 packs its data in pak*.psarc).

Format: big-endian header (magic PSAR, version, compression, TOC length, entry size, entry
count, block size, flags), entries (MD5, first block, 40-bit length, 40-bit offset), then a
table of compressed block sizes (0 = a full uncompressed block). Entry 0 is the manifest:
the paths of the other entries, NUL- or newline-separated. Read-only: never writes into the archive.

    psarc.py list <archive> [substring]
    psarc.py extract <archive> <path> <output file>
"""
import hashlib
import struct
import sys
import zlib


class Psarc:
    def __init__(self, path):
        self.file = open(path, 'rb')
        (magic, _, self.compression, toc_length, entry_size, count, self.block_size,
         flags) = struct.unpack('>4sI4sIIIII', self.file.read(32))
        if magic != b'PSAR':
            raise ValueError('not a PSARC archive')
        toc = self.file.read(toc_length - 32)
        self.entries, by_md5 = [], {}
        for i in range(count):
            e = toc[i * entry_size:(i + 1) * entry_size]
            self.entries.append((struct.unpack('>I', e[16:20])[0], int.from_bytes(e[20:25], 'big'),
                                 int.from_bytes(e[25:30], 'big')))
            by_md5[e[:16]] = i
        width = 2 if self.block_size <= 0x10000 else 3 if self.block_size <= 0x1000000 else 4
        table = toc[count * entry_size:]
        self.blocks = [int.from_bytes(table[i:i + width], 'big') for i in range(0, len(table) - width + 1, width)]
        # Entries are not in manifest order here: each one is found by the MD5 of its path.
        manifest = by_md5.get(bytes(16), 0)
        names = self.read(manifest).decode('utf-8', 'replace').replace('\0', '\n').split()
        self.names = {}
        for name in names:
            index = by_md5.get(hashlib.md5(name.lstrip('/').encode()).digest())
            if index is not None:
                self.names['/' + name.lstrip('/')] = index

    def read(self, index):
        block, length, offset = self.entries[index]
        out = bytearray()
        self.file.seek(offset)
        while len(out) < length:
            size = self.blocks[block] or self.block_size
            data = self.file.read(size)
            want = min(self.block_size, length - len(out))
            if size == want or data[:2] not in (b'\x78\x9c', b'\x78\xda', b'\x78\x01', b'\x78\x5e'):
                out += data[:want]
            else:
                out += zlib.decompress(data)
            block += 1
        return bytes(out[:length])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    archive = Psarc(sys.argv[2])
    if sys.argv[1] == 'list':
        pattern = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for name, index in sorted(archive.names.items()):
            if pattern in name.lower():
                print(f'{archive.entries[index][1]:>12}  {name}')
    elif sys.argv[1] == 'extract':
        with open(sys.argv[4], 'wb') as f:
            f.write(archive.read(archive.names[sys.argv[3]]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
