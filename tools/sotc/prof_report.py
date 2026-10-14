#!/usr/bin/env python3
"""Names the "Profile" lines of a run log (BB_PROFILE): prof_report.py <log> [--exe out/bb-probe.exe]."""
import argparse
import os
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log')
parser.add_argument('--exe', default='out/bb-probe.exe')
args = parser.parse_args()
lines = [l.rstrip() for l in open(args.log, encoding='utf-8', errors='replace') if l.startswith('Profile')]
addresses = sorted({int(m, 16) for l in lines for m in re.findall(r'bb-probe\.exe\+0x([0-9a-f]+)', l)})
symbolizer = os.path.join(os.environ.get('MSYS2_ROOT', r'C:\msys64'), 'clang64', 'bin', 'llvm-symbolizer.exe')
out = subprocess.run([symbolizer, f'--obj={args.exe}', '--functions=short', '--inlines'] +
                     [hex(0x140000000 + a) for a in addresses], capture_output=True, text=True).stdout
blocks = out.strip().split('\n\n')
names = {}
for address, block in zip(addresses, blocks):
    parts = block.split('\n')
    chain = []
    for i in range(0, len(parts) - 1, 2):
        chain.append(f"{parts[i]} ({parts[i + 1].replace(chr(92), '/').split('/')[-1]})")
    names[address] = ' <- '.join(chain[:3])
for l in lines:
    m = re.search(r'bb-probe\.exe\+0x([0-9a-f]+)', l)
    print(l if not m else f"{l[:l.index('bb-probe')]}{names.get(int(m.group(1), 16), '?')}")
