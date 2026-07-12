#!/usr/bin/env python3
"""Generates the app brand art: icon.png (48x48 SMDH) and cia/banner.png
(256x128, transparent 3DS-style). Pixel cartridge motif in the RomM violet
family; wordmark in Pixelify Sans (OFL, vendored next to this script).

Run from the repo root:  python3 tools/brand/gen-brand.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
FONT = os.path.join(os.path.dirname(__file__), "PixelifySans.ttf")

INK = (27, 20, 64)
VIOLET = (85, 62, 152)      # RomM brand
GLOW = (124, 92, 255)       # app accent
LILAC = (190, 164, 225)     # RomM brand
PAPER = (237, 229, 248)     # RomM brand
DARK = (40, 29, 86)


def cart_pixels(body, label, shine):
    """16x16-grid GBA-style cartridge."""
    px = {}
    for y in range(2, 14):
        for x in range(3, 13):
            px[(x, y)] = body
    for c in [(11, 2), (12, 2), (12, 3)]:       # shoulder cut
        px.pop(c, None)
    for y in (5, 6):                             # side grips
        px[(3, y)] = DARK
        px[(12, y)] = DARK
    px.pop((3, 13), None)
    px.pop((12, 13), None)                       # rounded feet
    dark_edge = tuple(max(0, c - 46) for c in body)
    for x in range(4, 12):                       # connector edge
        px[(x, 13)] = dark_edge
    for y in range(5, 10):                       # label window
        for x in range(5, 11):
            px[(x, y)] = label
    px[(5, 5)] = shine
    px[(6, 5)] = shine
    for x in range(3, 11):                       # top highlight
        if (x, 2) in px:
            px[(x, 2)] = tuple(min(255, c + 34) for c in body)
    for y in range(4, 13):                       # right shade
        if (12, y) in px and px[(12, y)] == body:
            px[(12, y)] = tuple(max(0, c - 28) for c in body)
    return px


def draw_cart(img, ox, oy, s, body, label, shine=PAPER):
    d = ImageDraw.Draw(img)
    for (x, y), c in cart_pixels(body, label, shine).items():
        d.rectangle([ox + x * s, oy + y * s, ox + (x + 1) * s - 1, oy + (y + 1) * s - 1], fill=c)


def vgrad(size, top, bottom):
    w, h = size
    im = Image.new("RGB", size)
    for y in range(h):
        t = y / (h - 1)
        im.paste(tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)), (0, y, w, y + 1))
    return im


def outlined(d, pos, txt, font, fill):
    x, y = pos
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx or dy:
                d.text((x + dx, y + dy), txt, font=font, fill=INK + (255,))
    d.text((x, y), txt, font=font, fill=fill)


def main():
    # icon: one cartridge on a violet gradient
    icon = vgrad((48, 48), (52, 37, 110), (18, 13, 44))
    draw_cart(icon, 0, 0, 3, GLOW, LILAC)
    icon.save(os.path.join(ROOT, "icon.png"))

    # banner: transparent, three-cart shelf + Pixelify Sans wordmark
    bnr = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(bnr)
    draw_cart(bnr, 4, 22, 5, (58, 43, 116, 255), (104, 86, 150, 255), (150, 134, 190, 255))
    draw_cart(bnr, 62, 14, 5, VIOLET + (255,), LILAC + (255,))
    draw_cart(bnr, 32, 30, 5, GLOW + (255,), PAPER + (255,))
    size = 32
    while size > 12:
        fb = ImageFont.truetype(FONT, size)
        if d.textlength("romm3ds", font=fb) <= 106:
            break
        size -= 1
    fb = ImageFont.truetype(FONT, size)
    zone_x, zone_w = 142, 108
    w1 = d.textlength("romm", font=fb)
    w = d.textlength("romm3ds", font=fb)
    tx = int(zone_x + (zone_w - w) // 2)
    ty = 64 - size
    outlined(d, (tx, ty), "romm", fb, PAPER + (255,))
    outlined(d, (tx + int(w1), ty), "3ds", fb, GLOW + (255,))
    fs = ImageFont.truetype(FONT, 12)
    cap = "RomM on your 3DS"
    cw = d.textlength(cap, font=fs)
    outlined(d, (int(zone_x + (zone_w - cw) // 2), ty + size + 12), cap, fs, LILAC + (255,))
    bnr.save(os.path.join(ROOT, "cia", "banner.png"))
    print(f"brand art written (wordmark {size}px)")


if __name__ == "__main__":
    main()
