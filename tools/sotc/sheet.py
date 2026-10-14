#!/usr/bin/env python3
"""Contact sheet of the output_NNN frames of a capture folder: sheet.py <dump dir> [cols]."""
import glob
import os
import sys

from PIL import Image, ImageDraw

folder = sys.argv[1]
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 3
files = sorted(glob.glob(os.path.join(folder, 'output_*.png')))
w, h = 640, 360
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * h))
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((w, h))
    ImageDraw.Draw(im).text((8, 8), os.path.basename(f)[7:10], fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
out = os.path.join(folder, 'sheet.png')
sheet.save(out)
print(out)
