#!/usr/bin/env python3
"""Make the launcher's bundled Japanese font: the characters the launcher's
own text uses (src/ui/launcher_text.cpp) plus kana and Japanese punctuation,
rendered from Noto Serif CJK JP into an .awf bitmap font (the format of the
game's fonts, see tools/awf_tool.py). The full CJK font is ~26 MB; this is a
small subset, used with assets/ui/NotoSerif-*.ttf when the game's own fonts
aren't available yet. Rerun after adding Japanese text to the launcher:

  make_launcher_font.py [--cjk NotoSerifCJK-Regular.ttc] [--index 0]
                        [--latin NotoSerif-Regular.ttf] [--line-height 40]
                        [--text src/ui/launcher_text.cpp]
                        [--out assets/ui/NotoSerifJP-Launcher.awf]

The glyphs sit on Noto Serif's baseline at Noto Serif's em size, so they line
up when the launcher merges them into the Latin font. Both fonts are under
the SIL Open Font License 1.1 (assets/ui/OFL.txt).
"""
import argparse
import re
import struct

from PIL import Image, ImageDraw, ImageFont

PAGE_WIDTH = 1024


def launcher_characters(path):
    src = open(path, encoding='utf-8').read()
    return {c for c in src if ord(c) >= 0x3000}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cjk', default='/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc')
    ap.add_argument('--index', type=int, default=0, help='face in the .ttc (0 = JP)')
    ap.add_argument('--latin', default='/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf')
    ap.add_argument('--line-height', type=int, default=40)
    ap.add_argument('--text', default='src/ui/launcher_text.cpp')
    ap.add_argument('--out', default='assets/ui/NotoSerifJP-Launcher.awf')
    a = ap.parse_args()

    chars = launcher_characters(a.text)
    chars |= {chr(c) for c in range(0x3000, 0x3040)}  # CJK punctuation
    chars |= {chr(c) for c in range(0x3041, 0x3097)}  # hiragana
    chars |= {chr(c) for c in range(0x30A1, 0x3100)}  # katakana
    chars |= {chr(c) for c in range(0xFF01, 0xFF5F)}  # full-width ASCII
    chars = sorted(chars)

    # Noto Serif's metrics (units per em 1000): ImGui sizes a .ttf so that
    # ascent + descent = the font size.
    latin = ImageFont.truetype(a.latin, 1000)
    asc, desc = latin.getmetrics()
    line = a.line_height
    ascent = round(line * asc / (asc + desc))
    em = round(line * 1000 / (asc + desc))
    font = ImageFont.truetype(a.cjk, em, index=a.index)

    glyphs = []  # (codepoint, image, y_offset)
    for ch in chars:
        advance = max(1, round(font.getlength(ch)))
        left, top, right, bottom = font.getbbox(ch, anchor='ls')
        top, bottom = max(top, -ascent), min(bottom, line - ascent)
        height = max(0, bottom - top)
        img = Image.new('L', (advance, max(height, 1)), 0)
        if height:
            ImageDraw.Draw(img).text((0, -top), ch, font=font, fill=255, anchor='ls')
        glyphs.append((ord(ch), img if height else None, advance, ascent + top))

    # Shelf packing into PAGE_WIDTH-wide pages of at most 1024 rows.
    pages, placed = [], []
    x = y = shelf = 0
    page = 0
    for cp, img, advance, yoff in glyphs:
        h = img.height if img else 0
        if x + advance > PAGE_WIDTH:
            x, y, shelf = 0, y + shelf + 1, 0
        if y + h > 1024:
            pages.append(y)
            page, x, y, shelf = page + 1, 0, 0, 0
        placed.append((cp, page, x, y, advance, h, yoff, img))
        x += advance + 1
        shelf = max(shelf, h)
    pages.append(y + shelf)

    images = [Image.new('L', (PAGE_WIDTH, max(4, (h + 3) // 4 * 4)), 0) for h in pages]
    for cp, p, gx, gy, w, h, yoff, img in placed:
        if img:
            images[p].paste(img, (gx, gy))

    out = bytearray(b'AWF1')
    out += struct.pack('<HHhHhH', len(images), len(placed), 0, line, ascent, 0)
    for img in images:
        out += struct.pack('<HH', img.width, img.height)
    for cp, p, gx, gy, w, h, yoff, img in placed:
        out += struct.pack('<IHHHHHh', cp, p, gx, gy, w, h, yoff)
    for img in images:
        out += img.tobytes()
    open(a.out, 'wb').write(out)
    print(f'{a.out}: {len(placed)} glyphs, {len(images)} page(s), {len(out) // 1024} KB '
          f'(line height {line}, ascent {ascent})')


if __name__ == '__main__':
    main()
