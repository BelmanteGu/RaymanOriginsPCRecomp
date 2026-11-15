#!/usr/bin/env python3
"""Turns a tool's text output into a terminal-style JPEG for docs/media (the "screenshot" of a step that has no window).

What it does: reads text (a file or stdin), draws it in a monospace font on a dark terminal background with a title
bar showing the command, and saves a JPEG. Long outputs can be cut with --max-lines; long lines are clipped.
What it does not do: run the command itself; pipe the real output into it so the picture is what the tool printed.

usage: <tool> | .venv/bin/python -I tools/term_shot.py --title "<command>" --out docs/media/<name>.jpg [--max-lines N]
"""
import argparse
import sys

from PIL import Image, ImageDraw, ImageFont

FONTS = ["/usr/share/fonts/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"]
BG, FG, BAR, DIM, ACCENT = (24, 26, 32), (220, 223, 228), (44, 47, 56), (140, 146, 158), (120, 200, 140)


def load_font(size):
    for f in FONTS:
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", required=True, help="the command shown in the title bar")
    ap.add_argument("--out", required=True)
    ap.add_argument("--input", help="text file (default: stdin)")
    ap.add_argument("--max-lines", type=int, default=70)
    ap.add_argument("--max-cols", type=int, default=150)
    ap.add_argument("--size", type=int, default=15)
    a = ap.parse_args()

    text = open(a.input, encoding="utf-8").read() if a.input else sys.stdin.read()
    lines = text.rstrip("\n").split("\n")
    if len(lines) > a.max_lines:
        cut = len(lines) - a.max_lines
        lines = lines[: a.max_lines] + ["... (%d more lines)" % cut]
    lines = [l if len(l) <= a.max_cols else l[: a.max_cols - 3] + "..." for l in lines]

    font = load_font(a.size)
    cw = font.getbbox("M")[2]
    lh = int(a.size * 1.45)
    pad, bar_h = 18, 34
    width = pad * 2 + cw * max(60, max(len(l) for l in lines + ["$ " + a.title]))
    height = bar_h + pad * 2 + lh * (len(lines) + 1)
    img = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, width, bar_h], fill=BAR)
    for i, c in enumerate([(237, 106, 94), (245, 191, 79), (98, 197, 84)]):
        d.ellipse([12 + i * 20, 11, 24 + i * 20, 23], fill=c)
    d.text((80, 8), "RaymanOrigins-PC", font=load_font(a.size - 1), fill=DIM)
    y = bar_h + pad
    d.text((pad, y), "$ " + a.title, font=font, fill=ACCENT)
    for l in lines:
        y += lh
        d.text((pad, y), l, font=font, fill=FG)
    img.save(a.out, "JPEG", quality=88)
    print("wrote %s (%dx%d)" % (a.out, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
