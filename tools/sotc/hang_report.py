#!/usr/bin/env python3
"""Readable thread dump from a run log (BB_HEARTBEAT dumps threads once frames stop).

Prints each thread that runs guest code or waits inside bb-probe, its frames resolved to
function and line with llvm-symbolizer (bb-probe.exe image base 0x140000000).

    hang_report.py <run log> [--exe out/bb-probe.exe] [--all]
"""
import argparse
import os
import re
import subprocess

NOISE = re.compile(r"'(bb:DrawPrep\d|shadPS4:GpuSched\w*|bb:Copy\d|pool-\d+|SDL\w*|DeviceHotplugThread|bb:mouse|bb:window)'")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log')
    parser.add_argument('--exe', default='out/bb-probe.exe')
    parser.add_argument('--all', action='store_true', help='also threads without guest or probe frames')
    args = parser.parse_args()
    text = open(args.log, encoding='utf-8', errors='replace').read()
    if 'Thread dump:' not in text:
        raise SystemExit('no thread dump in this log')
    threads, current = [], None
    for line in text.split('Thread dump:', 1)[1].splitlines():
        if line.startswith('  thread '):
            current = [line.strip(), []]
            threads.append(current)
        elif line.startswith('    #') and current:
            current[1].append(line.strip())
    addresses = sorted({int(m, 16) for _, frames in threads for f in frames
                        for m in re.findall(r'bb-probe\.exe\+0x([0-9a-f]+)', f)})
    symbolizer = os.path.join(os.environ.get('MSYS2_ROOT', r'C:\msys64'), 'clang64', 'bin', 'llvm-symbolizer.exe')
    output = subprocess.run([symbolizer, f'--obj={args.exe}', '--functions=short', '--no-inlines'] +
 [hex(0x140000000 + a) for a in addresses],
                            capture_output=True, text=True).stdout.strip().split('\n\n')
    names = {}
    for address, block in zip(addresses, output):
        parts = block.split('\n')
        location = parts[1].replace('\\', '/').split('/')[-1] if len(parts) > 1 else '?'
        names[address] = f'{parts[0]} ({location})'
    for title, frames in threads:
        interesting = any('guest offset' in f or 'bb-probe.exe' in f for f in frames)
        if NOISE.search(title) or (not interesting and not args.all):
            continue
        print(title)
        for f in frames:
            m = re.search(r'bb-probe\.exe\+0x([0-9a-f]+)', f)
            if 'ntdll' in f or 'KERNELBASE' in f or 'libwinpthread' in f or 'libc++' in f:
                continue
            print('   ', f + (f'  {names[int(m.group(1), 16)]}' if m else ''))


if __name__ == '__main__':
    main()
