#!/usr/bin/env python3
"""The pad button icons drawn in the game's texts (runtime/src/pad_icons.c): Xbox-style balls in place of the PC
build's keyboard keys.

Why: the PC build's atlas world/common/ui/ui_menu/consoles_buttons/x360/x360_pad_buttons.tga (512x256, 6 x 3 cells,
named in gameconfig/uitextconfig.isg in this order: B A RB STICK STICK_TILT_L STICK_TILT_R / X Y LB REDUCTION STEP
EMPTY_1 / EMPTY_2 START LT RT BACK DPAD) holds keyboard pictures ("DELETE", "F1", arrows), so with a pad the hints
showed keys. The runtime puts these balls into that texture when the game loads it (d3dx9.c); the cells it does not
own (REDUCTION, STEP, EMPTY_1, EMPTY_2) keep the game's pictures.

What it does:
  --draw   draws runtime/assets/pad_icons.png (512x256, transparent where the game's cells stay): every icon a perfect
           circle with the same frame (dark blue rim, light blue ring, thin dark line); inside, the balls of the touch
           controls' drawings (runtime/assets/touch: A, B, X, RT, cut as exact circles) and, in the same style, Y, LB,
           RB, LT, Start, Back, the stick, its tilts and the d-pad (a bold font of the system).
  (none)   makes private/include/pad_icons.rgba for the build: the PNG and its mip levels down to 1x1, RGBA, the
           layout of the game's texture (A8B8G8R8, 10 levels).
usage: .venv/bin/python -I tools/pad_icons_art.py [--draw]
"""
import colorsys
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNG = os.path.join(ROOT, "runtime", "assets", "pad_icons.png")
OUT = os.path.join(ROOT, "private", "include", "pad_icons.rgba")
W, H, S = 512, 256, 4                      # the atlas; drawn S times larger
CELL = W / 6
FONT = os.path.join(ROOT, "private", "deps", "fonts", "Fredoka.ttf")   # Fredoka (OFL), Google Fonts
CELLS = {0: "B", 1: "A", 2: "RB", 3: "STICK", 4: "TILT_R", 5: "TILT_L", 6: "X", 7: "Y", 8: "LB",
         13: "START", 14: "LT", 15: "RT", 16: "BACK", 17: "DPAD"}   # 4 / 5: the game's right / left arrows
NAVY, RING_HI, RING_LO, LINE = (16, 52, 88), (168, 222, 248), (70, 156, 212), (12, 34, 58)
# (light, dark, letter outline): the face buttons in the touch drawings' colours, everything else one graphite
COLORS = {"A": ((150, 222, 60), (40, 130, 16), (14, 70, 6)), "B": ((255, 110, 96), (170, 16, 16), (90, 4, 4)),
          "X": ((96, 150, 236), (14, 50, 140), (6, 22, 70)), "Y": ((255, 224, 90), (214, 150, 0), (120, 74, 0)),
          "DARK": ((92, 100, 118), (26, 30, 42), (8, 10, 18))}


def disc_mask(size, r_px, cx, cy):
    m = Image.new("L", (size[0], size[1]), 0)
    ImageDraw.Draw(m).ellipse([cx - r_px, cy - r_px, cx + r_px, cy + r_px], fill=255)
    return m


def radial(size, inner, outer, cx, cy, r):
    """a disc whose colour goes from inner (upper left) to outer (edge)"""
    im = Image.new("RGBA", size, (0, 0, 0, 0))
    px = im.load()
    for y in range(int(cy - r), int(cy + r) + 1):
        for x in range(int(cx - r), int(cx + r) + 1):
            d = math.hypot(x - (cx - r * 0.3), y - (cy - r * 0.35)) / (r * 1.45)
            t = min(1.0, d)
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3)) + (255,)
    return Image.composite(im, Image.new("RGBA", size, (0, 0, 0, 0)), disc_mask(size, r, cx, cy))


def gloss(im, cx, cy, r):
    g = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(g).ellipse([cx - r * 0.62, cy - r * 0.82, cx + r * 0.42, cy - r * 0.18], fill=(255, 255, 255, 70))
    return Image.alpha_composite(im, g)


def label(im, text, cx, cy, r, stroke_col):
    d = ImageDraw.Draw(im)
    size = int(r * (1.45 if len(text) == 1 else 1.0))
    f = ImageFont.truetype(FONT, size)
    f.set_variation_by_name("Bold")
    d.text((cx, cy + r * 0.02), text, font=f, anchor="mm", fill=(250, 250, 246, 255), stroke_width=max(2, int(r * 0.07)),
           stroke_fill=stroke_col + (255,))


def symbol(im, name, cx, cy, r):
    """what goes on a button, centred on (cx, cy), about r in radius: cream with a dark outline, like the touch art"""
    d = ImageDraw.Draw(im)
    cream, ink = (248, 246, 238, 255), (16, 22, 34, 255)
    w = max(3, round(r * 0.12))
    if name in ("A", "B", "X", "Y", "LB", "RB", "LT", "RT"):
        f = ImageFont.truetype(FONT, int(r * (1.75 if len(name) == 1 else 1.2)))
        f.set_variation_by_name("Bold")
        d.text((cx, cy + r * 0.04), name, font=f, anchor="mm", fill=cream, stroke_width=w, stroke_fill=ink)
        return
    def shape(draw_fn):                                         # the outline first, the cream on top
        draw_fn(ink, w)
        draw_fn(cream, 0)
    if name == "START":
        shape(lambda col, e: [d.rounded_rectangle([cx - r * 0.62 - e, cy + k * r * 0.42 - r * 0.12 - e, cx + r * 0.62 + e,
                                                    cy + k * r * 0.42 + r * 0.12 + e], radius=r * 0.12 + e, fill=col) for k in (-1, 0, 1)])
    elif name == "BACK":
        shape(lambda col, e: d.rounded_rectangle([cx - r * 0.7 - e, cy - r * 0.6 - e, cx + r * 0.2 + e, cy + r * 0.2 + e], radius=r * 0.14 + e, fill=col))
        d.rounded_rectangle([cx - r * 0.52, cy - r * 0.42, cx + r * 0.02, cy + r * 0.02], radius=r * 0.06, fill=ink)
        shape(lambda col, e: d.rounded_rectangle([cx - r * 0.2 - e, cy - r * 0.2 - e, cx + r * 0.7 + e, cy + r * 0.7 + e], radius=r * 0.14 + e, fill=col))
    elif name == "DPAD":
        a, b = r * 0.28, r * 0.85
        shape(lambda col, e: (d.rounded_rectangle([cx - a - e, cy - b - e, cx + a + e, cy + b + e], radius=r * 0.12 + e, fill=col),
                              d.rounded_rectangle([cx - b - e, cy - a - e, cx + b + e, cy + a + e], radius=r * 0.12 + e, fill=col)))
    else:                                                       # the stick: a ring and a dot; tilted: an arrow
        k = 0.78
        d.ellipse([cx - r * k - w, cy - r * k - w, cx + r * k + w, cy + r * k + w], fill=ink)
        d.ellipse([cx - r * k, cy - r * k, cx + r * k, cy + r * k], fill=cream)
        d.ellipse([cx - r * (k - 0.2), cy - r * (k - 0.2), cx + r * (k - 0.2), cy + r * (k - 0.2)], fill=ink)
        d.ellipse([cx - r * (k - 0.2) + w * 0.6, cy - r * (k - 0.2) + w * 0.6, cx + r * (k - 0.2) - w * 0.6, cy + r * (k - 0.2) - w * 0.6], fill=(0, 0, 0, 0))
        shape(lambda col, e: d.ellipse([cx - r * 0.25 - e, cy - r * 0.25 - e, cx + r * 0.25 + e, cy + r * 0.25 + e], fill=col))
        if name.startswith("TILT"):
            s = 1 if name == "TILT_R" else -1
            tip, base = cx + s * r * 1.25, cx + s * r * 0.85
            shape(lambda col, e: d.polygon([(tip + s * e, cy), (base - s * e * 0.3, cy - r * 0.38 - e), (base - s * e * 0.3, cy + r * 0.38 + e)], fill=col))


ORB = os.path.join(ROOT, "private", "padicons", "gen", "orb_green.webp")   # the empty button, green (generated)
# the ball's colour for each icon: (hue in degrees or None for graphite, saturation factor, value factor)
TINT = {"A": (None, 1, 1), "B": (2, 1.0, 0.95), "X": (218, 1.0, 0.9), "Y": (46, 1.0, 1.08)}
_orb = {}


def orb(n):
    """the empty button cut as a perfect circle (its dark rim's outer edge), n x n"""
    if n not in _orb:
        src = Image.open(ORB).convert("RGB")
        px = src.load()
        xs, ys = [], []
        for y in range(0, src.height, 2):
            for x in range(0, src.width, 2):
                p = px[x, y]
                if p[0] < 120 and p[2] > p[0]:                       # the dark blue rim
                    xs.append(x); ys.append(y)
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, min(x1 - x0, y1 - y0) / 2 - 2
        im = src.crop((round(cx - r), round(cy - r), round(cx + r), round(cy + r))).resize((n, n), Image.LANCZOS).convert("RGBA")
        im.putalpha(disc_mask((n * 4, n * 4), n * 2 - 2, n * 2, n * 2).resize((n, n), Image.LANCZOS))
        _orb[n] = im
    return _orb[n]


def ball_centre(n):
    """the middle of the coloured ball (the generated button's ball is not exactly in the middle of its ring)"""
    im = orb(n)
    px = im.load()
    xs, ys = [], []
    for y in range(n):
        for x in range(n):
            r, g, b, a = px[x, y]
            if a > 200 and g > r + 40 and g > b + 40:                 # the green
                xs.append(x); ys.append(y)
    return (min(xs) + max(xs) + 1) / 2, (min(ys) + max(ys) + 1) / 2   # the middle of its extent (not of its
                                                                      # pixels: the shine at the top is not green)


def tint(im, name):
    """the green of the ball becomes this icon's colour (the blue ring, the dark lines and the shine stay)"""
    hue, sf, vf = TINT.get(name, (-1, 0.12, 0.42))
    if hue is None:
        return im
    out = im.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            h, sat, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if 0.17 < h < 0.45 and sat > 0.15:                        # green
                if hue < 0:                                           # graphite: keep the light, lose the colour
                    sat, v = sat * sf, min(1.0, v * vf + 0.04)
                else:
                    h, sat, v = hue / 360, min(1.0, sat * sf), min(1.0, v * vf)
                r, g, b = (round(c * 255) for c in colorsys.hsv_to_rgb(h, sat, v))
                px[x, y] = (r, g, b, a)
    return out


GLYPHS = os.path.join(ROOT, "private", "padicons", "gen", "glyphs.webp")   # 4 x 4 symbols on white (generated)
GLYPH_CELL = {"A": 0, "B": 1, "X": 2, "Y": 3, "LB": 4, "RB": 5, "LT": 6, "RT": 7, "START": 8, "BACK": 9, "DPAD": 10,
              "STICK": 11, "TILT_L": 12, "TILT_R": 13, "PAUSE": 14, "GEAR": 15}
_glyph = {}


def glyph(name):
    """one symbol of the sheet without the white around it (what the cell's edge reaches without crossing the dark
    outline); everything inside the outline stays; cropped to the symbol"""
    if name not in _glyph:
        sheet = Image.open(GLYPHS).convert("RGB")
        k = GLYPH_CELL[name]
        cw, ch = sheet.width / 4, sheet.height / 4
        im = sheet.crop((round(k % 4 * cw), round(k // 4 * ch), round((k % 4 + 1) * cw), round((k // 4 + 1) * ch))).convert("RGBA")
        px = im.load()
        w, h = im.size
        light = lambda p: p[0] + p[1] + p[2] > 600                     # the white and the outline's light edge
        seen = [[False] * w for _ in range(h)]
        stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
        while stack:
            x, y = stack.pop()
            if 0 <= x < w and 0 <= y < h and not seen[y][x] and light(px[x, y]):
                seen[y][x] = True
                stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
        for y in range(h):
            for x in range(w):
                if seen[y][x]:
                    px[x, y] = (0, 0, 0, 0)
        # small pieces of the neighbouring symbols at the cell's edges go too (the symbol itself is the big piece)
        total = sum(1 for y in range(h) for x in range(w) if px[x, y][3])
        done = [[False] * w for _ in range(h)]
        for sx, sy in [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]:
            if done[sy][sx] or not px[sx, sy][3]:
                continue
            piece, stack = [], [(sx, sy)]
            while stack:
                x, y = stack.pop()
                if 0 <= x < w and 0 <= y < h and not done[y][x] and px[x, y][3]:
                    done[y][x] = True
                    piece.append((x, y))
                    stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
            if len(piece) < total * 0.15:
                for x, y in piece:
                    px[x, y] = (0, 0, 0, 0)
        a = im.getchannel("A").point(lambda v: 255 if v else 0)
        _glyph[name] = im.crop(a.getbbox())
    return _glyph[name]


def icon(name):
    """one cell, S times larger: the button in its colour and its symbol"""
    n = round(CELL * S)
    pad = round(n * 0.03)
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    im.alpha_composite(tint(orb(n - 2 * pad), name), (pad, pad))
    g = glyph(name)
    cx, cy = ball_centre(n - 2 * pad)
    box = (n - 2 * pad) * (0.56 if name in ("A", "B", "X", "Y") else 0.6)   # inside the ball's dark line
    k = box / max(g.width, g.height)
    g = g.resize((max(1, round(g.width * k)), max(1, round(g.height * k))), Image.LANCZOS)
    im.alpha_composite(g, (round(pad + cx - g.width / 2), round(pad + cy - g.height / 2)))
    return im


def draw():
    atlas = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    for idx, name in CELLS.items():
        col, row = idx % 6, idx // 6
        atlas.alpha_composite(icon(name), (round(col * CELL * S), round(row * CELL * S)))
    atlas.resize((W, H), Image.LANCZOS).save(PNG, optimize=True)
    print(PNG)


def build():
    im = Image.open(PNG).convert("RGBA")
    data = bytearray()
    w, h = W, H
    while True:
        lv = im if (w, h) == (W, H) else im.resize((w, h), Image.LANCZOS)
        data += lv.tobytes()                                  # R G B A: the game's texture is A8B8G8R8
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"{OUT}: {len(data)} bytes")


if __name__ == "__main__":
    draw() if "--draw" in sys.argv else build()
