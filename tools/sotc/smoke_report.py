#!/usr/bin/env python3
"""Summary of a smoke run: smoke_report.py <run log> <audio dump dir> <capture dir>.

Frames per heartbeat, the last frame statistics, readbacks, the read step-over, Ngs2 racks and
the parameter IDs the game sent (against the IDs runtime_sotc_ngs2.c assumes), the level of
the audio each AudioOut port played per 10 s, and how the run ended."""
import array
import collections
import glob
import math
import os
import re
import sys

log_path, audio_dir, capture_dir = sys.argv[1:4]
text = open(log_path, encoding='utf-8', errors='replace').read()
lines = text.splitlines()

print('== Frames')
beats = re.findall(r'Heartbeat (\d+)s: frames=(\d+) \(\+(\d+)\)', text)
print('  ' + ' '.join(f'{t}s:{int(d) / 10:.0f}fps' for t, _, d in beats))
for l in [l for l in lines if l.startswith('Frame stats')][-3:]:
    print('  ' + l[:160])

print('== GPU time (last profile)')
starts = [i for i, l in enumerate(lines) if l.startswith('GPU profile:') and 'ms/frame' in l]
if starts:
    print('  ' + lines[starts[-1]])
    for l in lines[starts[-1] + 1:starts[-1] + 13]:
        if not l.startswith('  '):
            break
        print('  ' + l[:150])
print('== Readbacks')
for l in [l for l in lines if l.startswith('Read faults:')][-3:]:
    print('  ' + l[:160])
print('  step-over active' if 'step over page protection' in text else '  step-over never used')

print('== Ngs2')
for l in lines:
    if l.startswith('Runtime: Ngs2 rack') or l.startswith('Runtime: Ngs2 render buffer') or \
       l.startswith('Runtime: Ngs2 system'):
        print('  ' + l)
known = {1, 2, 3, 4, 5, 6, 7, 0x10000001, 0x10000002, 0x10000003, 0x10000004, 0x10000005,
         0x10000006, 0x20000001, 0x30000001, 0x30000006}
ids = collections.Counter()
sizes = {}
racks = collections.defaultdict(set)
for m in re.finditer(r'Ngs2 param: rack (0x[0-9a-f]+) voice \d+ id (0x[0-9a-f]+) size (\d+)', text):
    i = int(m.group(2), 16)
    ids[i] += 1
    sizes.setdefault(i, set()).add(int(m.group(3)))
    racks[i].add(m.group(1))
for i, n in sorted(ids.items()):
    print(f'  id {i:#010x} x{n:<4} sizes {sorted(sizes[i])} racks {sorted(racks[i])}'
          f'{"" if i in known else "   <- not handled"}')
events = collections.Counter(m.group(1) for m in re.finditer(
    r'id 0x6 size \d+ next -?\d+: ([0-9a-f]{8})', text))
if events:
    print('  events: ' + ', '.join(f'{int(e, 16)}x{n}' for e, n in sorted(events.items())))

print('== Audio output')
for path in sorted(glob.glob(os.path.join(audio_dir, '*'))):
    name = os.path.basename(path)
    m = re.search(r'\.(\d+)ch\.(f32|s16)$', name)
    if not m:
        continue
    channels, fmt = int(m.group(1)), m.group(2)
    data = array.array('f' if fmt == 'f32' else 'h')
    with open(path, 'rb') as f:
        data.frombytes(f.read()[:(os.path.getsize(path) // data.itemsize) * data.itemsize])
    scale = 1.0 if fmt == 'f32' else 32768.0
    window = 48000 * 10 * channels
    levels = []
    for start in range(0, len(data), window):
        chunk = data[start:start + window]
        rms = math.sqrt(sum(x * x for x in chunk) / max(len(chunk), 1)) / scale
        levels.append(f'{20 * math.log10(rms) if rms > 0 else -120:.0f}dB')
    print(f'  {name}: {len(data) / channels / 48000:.0f} s, rms per 10 s: {" ".join(levels)}')

print('== End')
for l in lines:
    if l.startswith(('Fault:', 'STOP:', 'Device lost', 'run_sotc: game exited', 'Semaphore stall')):
        print('  ' + l[:160])
print(f'  frames: {os.path.join(capture_dir, "sheet.png")}')
