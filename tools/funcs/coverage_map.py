#!/usr/bin/env python3
"""Picture of Rayman Origins.exe's .text after function discovery (M2): what every byte was classified as.

What it does: reads private/analysis/functions.json and text_owner.bin (written by discover.py) and draws .text as a
grid, one pixel per --bytes bytes, left to right and top to bottom: function code in two alternating blues (so the
function boundaries show), switch-table data in amber, padding in grey, unclaimed bytes in red. Saves a JPEG.
What it does not do: run the discovery; run discover.py first.

usage: .venv/bin/python -I tools/funcs/coverage_map.py [--analysis private/analysis] --out docs/media/m2-coverage-map.jpg
"""
import argparse
import bisect
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

TEXT_START = 0x00401000
CODE_A, CODE_B = (52, 120, 200), (90, 160, 235)
TABLE, PAD, UNCLAIMED, BG, FG = (240, 170, 40), (70, 74, 82), (235, 60, 60), (24, 26, 32), (220, 223, 228)
FONTS = ["/usr/share/fonts/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf"]


def font(size):
    for f in FONTS:
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analysis", default="private/analysis")
    ap.add_argument("--out", required=True)
    ap.add_argument("--bytes", type=int, default=16, help="bytes per pixel")
    ap.add_argument("--width", type=int, default=600, help="pixels per row")
    a = ap.parse_args()

    owner = open(os.path.join(a.analysis, "text_owner.bin"), "rb").read()
    data = json.load(open(os.path.join(a.analysis, "functions.json")))
    starts = sorted(f["start"] for f in data["functions"])
    stats = data["stats"]

    npx = (len(owner) + a.bytes - 1) // a.bytes
    rows = (npx + a.width - 1) // a.width
    scale = 2
    grid = Image.new("RGB", (a.width, rows), BG)
    px = grid.load()
    for p in range(npx):
        lo = p * a.bytes
        chunk = owner[lo: lo + a.bytes]
        counts = [chunk.count(k) for k in (0, 1, 2, 3)]
        kind = max(range(4), key=lambda k: counts[k])
        if counts[0] and counts[0] * 4 >= len(chunk):
            kind = 0                     # show unclaimed bytes even when they are a minority of the pixel
        if kind == 1:
            i = bisect.bisect_right(starts, TEXT_START + lo) - 1
            color = CODE_A if i % 2 == 0 else CODE_B
        else:
            color = {0: UNCLAIMED, 2: TABLE, 3: PAD}[kind]
        px[p % a.width, p // a.width] = color
    grid = grid.resize((a.width * scale, rows * scale), Image.NEAREST)

    pad, legend_h, title_h = 20, 150, 44
    img = Image.new("RGB", (grid.width + 2 * pad, grid.height + title_h + legend_h + pad), BG)
    img.paste(grid, (pad, title_h))
    d = ImageDraw.Draw(img)
    d.text((pad, 12), "Rayman Origins.exe .text (0x00401000, %.1f MB): function discovery, %d bytes per pixel" %
           (len(owner) / 1e6, a.bytes), font=font(17), fill=FG)
    cov = stats["coverage_bytes"]
    tot = cov["total"]
    y = title_h + grid.height + 16
    items = [(CODE_A, "function code (alternating shades = function boundaries)", cov["code"]),
             (TABLE, "switch-table data", cov["table"]), (PAD, "padding (int3 / nop)", cov["padding"]),
             (UNCLAIMED, "unclaimed", cov["unclaimed"])]
    for i, (c, label, n) in enumerate(items):
        yy = y + i * 24
        d.rectangle([pad, yy + 3, pad + 16, yy + 19], fill=c)
        d.text((pad + 26, yy), "%-58s %9d bytes  %6.2f%%" % (label, n, 100.0 * n / tot), font=font(15), fill=FG)
    d.text((pad, y + 4 * 24 + 8), "%d functions, %d instructions, %d switch tables, 0 undecodable paths" %
           (stats["functions"], stats["instructions"], stats["switch_tables"]) if stats["undecodable_paths"] == 0 else
           "%d functions, %d undecodable paths" % (stats["functions"], stats["undecodable_paths"]), font=font(15), fill=FG)
    img.save(a.out, "JPEG", quality=90)
    print("wrote %s (%dx%d)" % (a.out, img.width, img.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
