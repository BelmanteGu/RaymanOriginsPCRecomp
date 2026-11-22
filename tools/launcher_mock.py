#!/usr/bin/env python3
"""A launcher mock-up in the game's own look: private/launcher/launcher.html (one file, everything embedded).

What it is for: trying the new Android launcher's design on the PC before it is written in the app. The home screen is
the game's title menu (the scrolling red bars over the jungle), the settings screen is the game's Options screen
(Control / Display / Sound panels, Accept / Cancel), both with the port's own entries.
Where the pictures come from: the user's own copy of the game, never the repository. The jungle and the bars are
captures of the game running in this port (USF_HIDE_TEX skips the menu's atlases to get the scene behind it, see
docs/PROGRESS.md), the panels, fields, arrows, sliders and the font are the game's textures and font02.fnt from
bundle_PC.ipk, all under private/launcher_study (made by the study commands in docs/PROGRESS.md).
usage: python3 -I tools/launcher_mock.py   (a Python with numpy and Pillow)
"""
import base64
import io
import json
import os
import re

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = os.path.join(ROOT, "private", "launcher_study")
PNG = os.path.join(S, "png")
OUT = os.path.join(ROOT, "private", "launcher", "launcher.html")


def uri(im, fmt="PNG", q=88):
    b = io.BytesIO()
    im.save(b, fmt, quality=q) if fmt == "JPEG" else im.save(b, fmt, optimize=True)
    return "data:image/%s;base64,%s" % (fmt.lower(), base64.b64encode(b.getvalue()).decode())


def opaque_box(im):
    a = np.asarray(im.convert("RGBA"))[..., 3] > 128
    ys, xs = np.nonzero(a.any(1))[0], np.nonzero(a.any(0))[0]
    return xs.min(), ys.min(), xs.max() + 1, ys.max() + 1


def place(name, box):
    """CSS box for texture `name` so that its opaque part covers `box` (x0, y0, x1, y1) on the 2340x1080 stage."""
    im = Image.open(os.path.join(PNG, name + ".png"))
    ox0, oy0, ox1, oy1 = opaque_box(im)
    sx, sy = (box[2] - box[0]) / (ox1 - ox0), (box[3] - box[1]) / (oy1 - oy0)
    return [round(box[0] - ox0 * sx, 1), round(box[1] - oy0 * sy, 1), round(im.width * sx, 1), round(im.height * sy, 1)]


def font():
    chars = {}
    for line in open(os.path.join(S, "misc", "fonts", "font02.fnt"), encoding="utf-8"):
        if line.startswith("char "):
            v = dict(re.findall(r"(\w+)=(-?\d+)", line))
            chars[int(v["id"])] = [int(v[k]) for k in ("x", "y", "width", "height", "xoffset", "yoffset", "xadvance")]
    a = Image.open(os.path.join(PNG, "font02_0.png")).convert("RGBA")
    return {"chars": chars, "line": 110, "base": 89}, uri(a)


def main():
    tex = {n: uri(Image.open(os.path.join(PNG, n + ".png"))) for n in (
        "settingsarrowleft", "settingsarrowleftsel", "settingsarrowright", "settingsarrowrightsel", "settingsbackbut",
        "settingscontrolbut", "settingscontrolbutsel", "settingscontrolsbg1", "settingscontrolsbg2", "settingscontrolsbg3",
        "settingsdisplaybg", "settingsplayerbut", "settingsplayerbutsel", "settingsslider", "settingssliderbg",
        "settingssliderbgsel", "settingsslidersel", "settingssoundbg", "settingswindowbut", "settingswindowbutsel")}
    slots = json.load(open(os.path.join(PNG, "slots.json")))
    slot_img = {k: uri(Image.open(os.path.join(PNG, "slot_%s.png" % k))) for k in slots}
    bg = Image.open(os.path.join(S, "cap6", "frame_01300.ppm")).convert("RGB")
    geo = {   # measured on the game's Options screen at 2340x1080 (private/launcher_study/cap4)
        "ctrlhead": place("settingscontrolsbg1", (448, 184, 1094, 314)),
        "ctrllabels": place("settingscontrolsbg2", (444, 334, 658, 798)),
        "ctrlvalues": place("settingscontrolsbg3", (672, 336, 1092, 798)),
        "display": place("settingsdisplaybg", (1144, 178, 1882, 404)),
        "sound": place("settingssoundbg", (1140, 428, 1878, 696)),
        "accept": place("settingsbackbut", (1158, 720, 1490, 796)),
        "cancel": place("settingsbackbut", (1544, 722, 1862, 794)),
        "field": [place("settingscontrolbut", (742, 342 + i * 50.75, 1020, 382 + i * 50.75)) for i in range(9)],
        "fieldsel": [place("settingscontrolbutsel", (742, 342 + i * 50.75, 1020, 382 + i * 50.75)) for i in range(9)],
        "tabs": [place("settingsplayerbut", (624 + i * 114, 252, 728 + i * 114, 294)) for i in range(4)],
        "tabsel": [place("settingsplayerbutsel", (624 + i * 114, 252, 728 + i * 114, 294)) for i in range(4)],
        "resfield": place("settingscontrolbutsel", (1396, 258, 1852, 332)),
        "resfield_off": place("settingscontrolbut", (1396, 258, 1852, 332)),
        "arrowl": place("settingsarrowleft", (1413, 282, 1465, 315)),
        "arrowr": place("settingsarrowright", (1780, 282, 1830, 315)),
        "check": place("settingswindowbut", (1774, 356, 1822, 390)),
        "checkon": place("settingswindowbutsel", (1774, 352, 1834, 392)),
        "slider": [place("settingssliderbg", (1474, 560 + i * 72, 1820, 570 + i * 72)) for i in range(2)],
        "slidersel": [place("settingssliderbgsel", (1474, 560 + i * 72, 1820, 570 + i * 72)) for i in range(2)],
        "knob": [place("settingsslider", (0, 542 + i * 72, 20, 582 + i * 72)) for i in range(2)],
    }
    fnt, fnt_png = font()
    html = open(os.path.join(ROOT, "tools", "launcher_mock.html")).read()
    for k, v in {"__BG__": uri(bg, "JPEG", 90), "__FONT__": json.dumps(fnt), "__FONTPNG__": fnt_png,
                 "__TEX__": json.dumps(tex), "__SLOTS__": json.dumps(slots), "__SLOTIMG__": json.dumps(slot_img),
                 "__GEO__": json.dumps(geo)}.items():
        html = html.replace(k, v)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write(html)
    print(OUT, len(html) // 1024, "KB")


if __name__ == "__main__":
    main()
