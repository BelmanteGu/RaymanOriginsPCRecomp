#!/usr/bin/env python3
"""The on-screen controls' drawings (runtime/assets/touch/*.png, the ones made for the Xbox 360 port) as one raw block
the runtime embeds (runtime/src/touch_art.c, .incbin): private/include/touch_art.rgba.

What it does: each PNG, in the order of ART_NAMES below, scaled to SIZE x SIZE, RGBA with premultiplied alpha, one
after the other. Run by runtime/build.sh and tools/android/build_native.sh when the file is missing or older.
usage: .venv/bin/python -I tools/touch_art.py
"""
import os
import sys

from PIL import Image

SIZE = 256
ART_NAMES = ["btn_jump", "btn_attack", "btn_back", "btn_run", "btn_pause", "stick_base", "stick_knob"]  # touch.c ART_*
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    out = os.path.join(ROOT, "private", "include", "touch_art.rgba")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = bytearray()
    for name in ART_NAMES:
        im = Image.open(os.path.join(ROOT, "runtime", "assets", "touch", name + ".png")).convert("RGBA")
        im = im.resize((SIZE, SIZE), Image.LANCZOS)
        px = im.tobytes()
        for i in range(0, len(px), 4):
            r, g, b, a = px[i:i + 4]
            data += bytes(((r * a + 127) // 255, (g * a + 127) // 255, (b * a + 127) // 255, a))
    with open(out, "wb") as f:
        f.write(data)
    print(f"{out}: {len(ART_NAMES)} drawings, {SIZE}x{SIZE}, {len(data)} bytes")


if __name__ == "__main__":
    sys.exit(main())
