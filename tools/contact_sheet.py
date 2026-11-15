#!/usr/bin/env python3
"""Puts the frames a run captured (USF_SHOT, .ppm) side by side in one JPEG, two per row, for checking a run at a
glance and for the docs.
usage: .venv/bin/python -I tools/contact_sheet.py <dir with frame_*.ppm> <out.jpg> [width of each frame]
"""
import glob
import sys

from PIL import Image

d, out = sys.argv[1], sys.argv[2]
w = int(sys.argv[3]) if len(sys.argv) > 3 else 480
ims = []
for p in sorted(glob.glob(d + "/frame_*.ppm")):
    im = Image.open(p).convert("RGB")
    ims.append(im.resize((w, w * im.height // im.width)))
h = ims[0].height
sheet = Image.new("RGB", (2 * w, h * ((len(ims) + 1) // 2)))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % 2) * w, (i // 2) * h))
sheet.save(out, quality=82)
print("wrote", out, len(ims), "frames")
