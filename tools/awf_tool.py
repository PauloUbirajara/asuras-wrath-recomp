#!/usr/bin/env python3
"""Use the game's .awf bitmap fonts (tools/asset_extract.py font) outside the
game, e.g. to draw text for art in an image editor.

  awf_tool.py render <font.awf> "<text>" <out.png> [--height PX] [--color RRGGBB]
                     [--spacing PX] [--outline PX] [--outline-color RRGGBB]
      Text on a transparent background, cropped to the text. Use \\n for
      several lines. --height scales to that line height (the font's own
      size is printed); the font is a bitmap, so it is sharpest at its size.
  awf_tool.py pages  <font.awf> <out dir>
      Every glyph page as a white-on-transparent PNG, plus glyphs.txt
      (codepoint, character, page, x, y, w, h, y offset).

The fonts are in ~/.cache/asura_wrath_recomp/ui (MediumFont.awf,
TitleFont.awf and their _ITA/_ESN/_JPN copies for accents and Japanese).
Only characters the game uses are in them.
"""
import argparse
import os
import struct
import sys

from PIL import Image


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'AWF1':
        sys.exit(f'{path}: not an .awf font')
    page_count, glyph_count, kerning, line_height, ascent, _ = struct.unpack_from('<HHhHhH', d, 4)
    p = 16
    sizes = []
    for _ in range(page_count):
        sizes.append(struct.unpack_from('<HH', d, p)); p += 4
    glyphs = {}
    for _ in range(glyph_count):
        cp, page, x, y, w, h, yoff = struct.unpack_from('<IHHHHHh', d, p); p += 16
        glyphs[cp] = (page, x, y, w, h, yoff)
    pages = []
    for w, h in sizes:
        pages.append(Image.frombytes('L', (w, h), d[p:p + w * h])); p += w * h
    return dict(glyphs=glyphs, pages=pages, kerning=kerning, line_height=line_height, ascent=ascent)


def colour(text):
    text = text.lstrip('#')
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def render(font, text, spacing):
    lines = text.replace('\\n', '\n').split('\n')
    lh = font['line_height']
    space = font['glyphs'].get(32, (0, 0, 0, lh // 3, 0, 0))[3]
    missing = set()
    placed = []  # (x, y, glyph)
    width = 0
    for row, line in enumerate(lines):
        x = 0
        for ch in line:
            g = font['glyphs'].get(ord(ch))
            if g is None or (g[3] == 0 and ch != ' '):
                if ch != ' ':
                    missing.add(ch)
                x += space + spacing
                continue
            if g[3] and g[4]:
                placed.append((x, row * lh + g[5], g))
            x += g[3] + font['kerning'] + spacing
        width = max(width, x)
    if missing:
        print('not in this font: ' + ''.join(sorted(missing)), file=sys.stderr)
    alpha = Image.new('L', (max(width, 1), max(len(lines) * lh, 1)), 0)
    for x, y, (page, gx, gy, w, h, _) in placed:
        glyph = font['pages'][page].crop((gx, gy, gx + w, gy + h))
        alpha.paste(Image.composite(glyph, alpha.crop((x, y, x + w, y + h)), glyph), (x, y))
    return alpha


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('render')
    r.add_argument('font'); r.add_argument('text'); r.add_argument('out')
    r.add_argument('--height', type=int, default=0)
    r.add_argument('--color', default='ffffff')
    r.add_argument('--spacing', type=int, default=0)
    r.add_argument('--outline', type=int, default=0)
    r.add_argument('--outline-color', default='000000')
    p = sub.add_parser('pages')
    p.add_argument('font'); p.add_argument('out')
    a = ap.parse_args()

    font = load(a.font)
    if a.cmd == 'pages':
        os.makedirs(a.out, exist_ok=True)
        for i, page in enumerate(font['pages']):
            img = Image.new('RGBA', page.size, (255, 255, 255, 0))
            img.putalpha(page)
            img.save(os.path.join(a.out, f'page{i}.png'))
        with open(os.path.join(a.out, 'glyphs.txt'), 'w', encoding='utf-8') as f:
            f.write(f'# line height {font["line_height"]}, ascent {font["ascent"]}, '
                    f'kerning {font["kerning"]}\n# codepoint char page x y w h y_offset\n')
            for cp, (page, x, y, w, h, yoff) in sorted(font['glyphs'].items()):
                ch = chr(cp) if cp > 32 else ' '
                f.write(f'{cp:#06x} {ch} {page} {x} {y} {w} {h} {yoff}\n')
        print(f'{len(font["pages"])} pages, {len(font["glyphs"])} glyphs -> {a.out}')
        return

    alpha = render(font, a.text, a.spacing)
    out = Image.new('RGBA', alpha.size, colour(a.color) + (0,))
    out.putalpha(alpha)
    if a.outline > 0:
        o = a.outline
        grown = Image.new('L', (alpha.width + 2 * o, alpha.height + 2 * o), 0)
        for dy in range(-o, o + 1):
            for dx in range(-o, o + 1):
                if dx * dx + dy * dy <= o * o:
                    grown.paste(alpha, (o + dx, o + dy), alpha)
        base = Image.new('RGBA', grown.size, colour(a.outline_color) + (0,))
        base.putalpha(grown)
        base.alpha_composite(out, (o, o))
        out = base
    box = out.getchannel('A').getbbox()
    if box:
        out = out.crop(box)
    if a.height and a.height != font['line_height']:
        k = a.height / font['line_height']
        out = out.resize((max(1, round(out.width * k)), max(1, round(out.height * k))), Image.LANCZOS)
    out.save(a.out)
    print(f'{a.out}: {out.width}x{out.height} (font line height {font["line_height"]})')


if __name__ == '__main__':
    main()
