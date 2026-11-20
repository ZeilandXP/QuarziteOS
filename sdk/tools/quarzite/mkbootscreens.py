#!/usr/bin/env python3
#
# PROJECT:     QuarziteOS
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Convert the QuarziteOS boot/shutdown artwork (640x480 PNG) into
#              the 16-color RLE4 bitmaps used by bootvid (ntoskrnl/inbv).
#
# Inputs  (ntoskrnl/inbv/resources/quarzite/):
#   boot.png      boot splash; contains the progress bar in its "full" state
#   shutdown.png  "It is now safe to turn off your computer." screen
#
# Outputs (ntoskrnl/inbv/resources/):
#   qz_boot.bmp      boot splash with the progress bar area cleared to black
#   qz_bootbar.bmp   the progress bar alone (same palette as qz_boot.bmp);
#                    the kernel reveals it left to right as boot progresses
#   qz_shutdown.bmp  shutdown screen (own palette)
#
# bootvid drives 640x480 VGA in 16 colors with one palette per screen. Index
# 0 must be black (used for fills) and index 15 white (used for text).
#
# Requires Pillow. Usage: python3 sdk/tools/quarzite/mkbootscreens.py [--preview DIR]
#
import os
import struct
import sys

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RES = os.path.join(ROOT, "ntoskrnl", "inbv", "resources")
SRC = os.path.join(RES, "quarzite")

# Progress bar rectangle inside boot.png -- keep in sync with
# VID_QZ_PROGRESS_BAR_* in ntoskrnl/inbv/logo.h
BAR_LEFT, BAR_TOP, BAR_WIDTH, BAR_HEIGHT = 225, 355, 190, 28


def quantize16(img):
    """Quantize to 16 colors with black at index 0 and white at index 15."""
    rgb = img.convert("RGB")
    # Pick the palette first, then remap with error diffusion: Pillow only
    # dithers when quantizing against a given palette.
    try:
        palimg = rgb.quantize(colors=16, method=Image.Quantize.LIBIMAGEQUANT)
    except (ValueError, OSError):
        palimg = rgb.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    q = rgb.quantize(palette=palimg, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette()[:48]
    pal += [0] * (48 - len(pal))
    colors = [tuple(pal[i * 3:i * 3 + 3]) for i in range(16)]
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in colors]
    darkest = min(range(16), key=lambda i: lum[i])
    brightest = max(range(16), key=lambda i: lum[i])

    order = [i for i in range(16) if i not in (darkest, brightest)]
    order = [darkest] + order + [brightest]          # new index -> old index
    remap = {old: new for new, old in enumerate(order)}

    new_colors = [colors[old] for old in order]
    new_colors[0] = (0, 0, 0)
    new_colors[15] = (255, 255, 255)

    w, h = q.size
    src = q.load()
    pixels = [[remap[src[x, y]] for x in range(w)] for y in range(h)]
    return pixels, new_colors


def rle4_encode(pixels):
    """BI_RLE4, encoded mode only (always valid; good on flat/dithered art)."""
    out = bytearray()
    for row in reversed(pixels):                     # bottom-up DIB
        x, w = 0, len(row)
        while x < w:
            a = row[x]
            b = row[x + 1] if x + 1 < w else a
            n = 1
            while n < 255 and x + n < w and row[x + n] == (a if n % 2 == 0 else b):
                n += 1
            out += bytes([n, (a << 4) | b])
            x += n
        out += b"\x00\x00"                           # end of line
    out[-2:] = b"\x00\x01"                           # end of bitmap
    return bytes(out)


def rle4_decode(data, w, h):
    """Mirror of bootvid's RleBitBlt (encoded mode + escapes)."""
    img = [[0] * w for _ in range(h)]
    y, x, i = h - 1, 0, 0
    while i < len(data):
        n = data[i]
        if n:
            c1, c2 = data[i + 1] >> 4, data[i + 1] & 15
            for k in range(n):
                if x < w and 0 <= y < h:
                    img[y][x] = c1 if k % 2 == 0 else c2
                x += 1
            i += 2
            continue
        code = data[i + 1]
        i += 2
        if code == 0:
            x, y = 0, y - 1
        elif code == 1:
            break
        else:
            raise ValueError("unexpected escape in RLE4 data")
    return img


def write_bmp(path, pixels, colors):
    h, w = len(pixels), len(pixels[0])
    data = rle4_encode(pixels)
    assert rle4_decode(data, w, h) == pixels, "RLE4 round trip failed"
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 4, 2, len(data),
                       2835, 2835, 16, 16)          # BI_RLE4 = 2
    palette = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in colors)
    offset = 14 + len(info) + len(palette)
    header = struct.pack("<2sIHHI", b"BM", offset + len(data), 0, 0, offset)
    with open(path, "wb") as f:
        f.write(header + info + palette + data)
    return len(data)


def preview(path, pixels, colors):
    h, w = len(pixels), len(pixels[0])
    im = Image.new("RGB", (w, h))
    im.putdata([colors[p] for row in pixels for p in row])
    im.save(path)


def main():
    preview_dir = None
    if "--preview" in sys.argv:
        preview_dir = sys.argv[sys.argv.index("--preview") + 1]
        os.makedirs(preview_dir, exist_ok=True)

    # Boot screen + progress bar share one palette
    boot = Image.open(os.path.join(SRC, "boot.png"))
    assert boot.size == (640, 480), "boot.png must be 640x480"
    pixels, colors = quantize16(boot)

    bar = [row[BAR_LEFT:BAR_LEFT + BAR_WIDTH] for row in pixels[BAR_TOP:BAR_TOP + BAR_HEIGHT]]
    splash = [row[:] for row in pixels]
    for y in range(BAR_TOP, BAR_TOP + BAR_HEIGHT):
        for x in range(BAR_LEFT, BAR_LEFT + BAR_WIDTH):
            splash[y][x] = 0

    n1 = write_bmp(os.path.join(RES, "qz_boot.bmp"), splash, colors)
    n2 = write_bmp(os.path.join(RES, "qz_bootbar.bmp"), bar, colors)

    shut = Image.open(os.path.join(SRC, "shutdown.png"))
    assert shut.size == (640, 480), "shutdown.png must be 640x480"
    spixels, scolors = quantize16(shut)
    n3 = write_bmp(os.path.join(RES, "qz_shutdown.bmp"), spixels, scolors)

    print(f"qz_boot.bmp {n1} bytes, qz_bootbar.bmp {n2} bytes, qz_shutdown.bmp {n3} bytes")

    if preview_dir:
        preview(os.path.join(preview_dir, "boot-16color.png"), pixels, colors)
        preview(os.path.join(preview_dir, "shutdown-16color.png"), spixels, scolors)


if __name__ == "__main__":
    main()
