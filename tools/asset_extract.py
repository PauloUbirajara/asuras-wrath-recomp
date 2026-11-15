#!/usr/bin/env python3
"""Asura's Wrath UE3 (v841, big-endian) package reader for asset extraction.

Input: plain package bodies from xxx_unpack (out/assets/upk/*.upk).

  asset_extract.py list  <pkg.upk> [class filter]
  asset_extract.py dump  <pkg.upk> <out dir> [class filter]   raw export blobs
  asset_extract.py textures <pkg.upk> <out dir> [name filter]  Texture2D -> PNG
  asset_extract.py swf   <pkg.upk> <out dir>                    SwfMovie -> .gfx
  asset_extract.py font  <pkg.upk> <Font path> <out.awf>        UE3 bitmap Font -> .awf

.awf (mantra UI bitmap font, little-endian), read by src/ui/game_font.cpp:
  char[4] 'AWF1'; u16 page_count, glyph_count; i16 kerning; u16 line_height;
  i16 ascent; u16 reserved
  page_count x { u16 width, height }
  glyph_count x { u32 codepoint; u16 page, x, y, w, h; i16 y_offset }
  page_count x width*height u8 alpha
"""
import os
import struct
import sys


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.p)[0]; self.p += 4; return v

    def u64(self):
        v = struct.unpack_from('>Q', self.d, self.p)[0]; self.p += 8; return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ''
        if n > 0:
            s = self.d[self.p:self.p + n].rstrip(b'\0').decode('latin-1'); self.p += n
        else:
            s = self.d[self.p:self.p - n * 2].decode('utf-16-be').rstrip('\0'); self.p += -n * 2
        return s


class Package:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        r = Reader(self.data)
        assert r.u32() == 0x9E2A83C1, 'not a UE3 package'
        ver = r.u32()
        self.version, self.licensee = ver & 0xFFFF, ver >> 16
        self.header_size = r.i32()
        self.folder = r.fstring()
        self.flags = r.u32()
        name_count, name_offset = r.i32(), r.i32()
        export_count, export_offset = r.i32(), r.i32()
        import_count, import_offset = r.i32(), r.i32()

        r.p = name_offset
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstring()); r.u64()

        r.p = import_offset
        self.imports = []
        for _ in range(import_count):
            cpkg = self.name(r.i32(), r.i32()); cls = self.name(r.i32(), r.i32())
            outer = r.i32(); obj = self.name(r.i32(), r.i32())
            self.imports.append({'class_package': cpkg, 'class': cls, 'outer': outer, 'name': obj})

        r.p = export_offset
        self.exports = []
        for _ in range(export_count):
            e = {'class_index': r.i32(), 'super': r.i32(), 'outer': r.i32(),
                 'name': self.name(r.i32(), r.i32()), 'archetype': r.i32(),
                 'object_flags': r.u64(), 'size': r.i32(), 'offset': r.i32(),
                 'export_flags': r.u32()}
            net = r.i32(); r.p += 4 * net
            r.p += 16 + 4  # package guid + flags
            self.exports.append(e)

    def name(self, index, number):
        n = self.names[index] if 0 <= index < len(self.names) else f'?{index}'
        return n if number == 0 else f'{n}_{number - 1}'

    def obj_name(self, index):
        if index > 0:
            return self.exports[index - 1]['name']
        if index < 0:
            return self.imports[-index - 1]['name']
        return 'Class'

    def class_name(self, e):
        return self.obj_name(e['class_index'])

    def full_path(self, e):
        parts, outer = [e['name']], e['outer']
        while outer > 0:
            o = self.exports[outer - 1]; parts.append(o['name']); outer = o['outer']
        return '.'.join(reversed(parts))

    def blob(self, e):
        return self.data[e['offset']:e['offset'] + e['size']]


# --- Texture2D -----------------------------------------------------------------

# format: (bytes per block, block size in pixels, DDS FourCC or None)
TEXTURE_FORMATS = {
    'PF_DXT1': (8, 4, b'DXT1'),
    'PF_DXT3': (16, 4, b'DXT3'),
    'PF_DXT5': (16, 4, b'DXT5'),
    'PF_A8R8G8B8': (4, 1, None),
    'PF_G8': (1, 1, None),
}


def tiled_offset(x, y, pitch, log_bpb):
    """Xbox 360 2D tiled byte offset of block (x, y); pitch in blocks (32-aligned)."""
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log_bpb + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def untile(data, width_blocks, height_blocks, bpb):
    log_bpb = bpb.bit_length() - 1
    pitch = (width_blocks + 31) & ~31
    out = bytearray(width_blocks * height_blocks * bpb)
    for y in range(height_blocks):
        for x in range(width_blocks):
            src = tiled_offset(x, y, pitch, log_bpb)
            dst = (y * width_blocks + x) * bpb
            out[dst:dst + bpb] = data[src:src + bpb]
    return bytes(out)


def swap16(data):
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]
    return bytes(b)


def read_bulk(data, r):
    flags, count, size, offset = r.u32(), r.i32(), r.i32(), r.i32()
    if flags & 0x1 or flags & 0x20 or size <= 0:  # separate file / unused
        return flags, None
    blob = data[r.p:r.p + size]; r.p += size
    return flags, blob


def decode_texture(pkg, e):
    """Returns a PIL image of mip 0, or raises."""
    import io
    from PIL import Image
    data = pkg.blob(e)
    props, end = read_properties(pkg, data, 4)
    fmt = props.get('Format')
    if fmt not in TEXTURE_FORMATS:
        raise ValueError(f'unsupported format {fmt}')
    bpb, block, fourcc = TEXTURE_FORMATS[fmt]
    r = Reader(data, end)
    read_bulk(data, r)  # source art
    mip_count = r.i32()
    if mip_count < 1:
        raise ValueError('no mips')
    flags, mip = read_bulk(data, r)
    w, h = r.i32(), r.i32()
    if mip is None:
        raise ValueError(f'mip 0 not inline (bulk flags {flags:#x})')
    wb, hb = (w + block - 1) // block, (h + block - 1) // block
    # Tiled surfaces are padded to 32x32-block tiles.
    wb_pad, hb_pad = (wb + 31) & ~31, (hb + 31) & ~31
    if len(mip) < wb_pad * hb_pad * bpb:
        raise ValueError(f'mip too small ({len(mip)} < {wb_pad}x{hb_pad}x{bpb}); packed mip tail?')
    linear = untile(mip, wb_pad, hb_pad, bpb)
    if fourcc:
        linear = swap16(linear)
        dds = bytearray(128)
        struct.pack_into('<4sIIIIIII', dds, 0, b'DDS ', 124, 0x1007 | 0x80000, hb_pad * block,
                         wb_pad * block, len(linear), 0, 1)
        struct.pack_into('<II4s', dds, 76, 32, 0x4, fourcc)
        struct.pack_into('<I', dds, 108, 0x1000)
        img = Image.open(io.BytesIO(bytes(dds) + linear)); img.load()
        img = img.convert('RGBA')
    elif fmt == 'PF_A8R8G8B8':
        img = Image.frombytes('RGBA', (wb_pad, hb_pad), linear, 'raw', 'ARGB')
    else:
        img = Image.frombytes('L', (wb_pad, hb_pad), linear)
    return img.crop((0, 0, props.get('SizeX', w), props.get('SizeY', h))), props


def swf_bytes(pkg, e):
    """The GFx movie stored in a SwfMovie export's RawData."""
    data = pkg.blob(e)
    props, _ = read_properties(pkg, data, 4)
    raw = props['RawData'][2]
    n = struct.unpack_from('>i', raw)[0]
    return raw[4:4 + n]


def export_font(pkg, path, out_path):
    """UE3 UFont (bitmap pages + per-character rects) -> .awf."""
    font = next(e for e in pkg.exports if pkg.full_path(e) == path and pkg.class_name(e) == 'Font')
    data = pkg.blob(font)
    props, end = read_properties(pkg, data, 4)
    raw = props['Characters'][2]
    r = Reader(raw)
    chars = []
    for _ in range(r.i32()):
        u, v, w, h = r.i32(), r.i32(), r.i32(), r.i32()
        page = raw[r.p]; r.p += 1
        yoff = r.i32()
        chars.append((u, v, w, h, page, yoff))
    tex_raw = props['Textures'][2]
    tex_count = struct.unpack_from('>i', tex_raw)[0]
    tex_refs = struct.unpack_from(f'>{tex_count}i', tex_raw, 4)
    pages = []
    for ref in tex_refs:
        img, _ = decode_texture(pkg, pkg.exports[ref - 1])
        pages.append(img.getchannel('A'))
    # Native CharRemap (TMap<u16 char, u16 index>) follows the properties.
    r = Reader(data, end)
    remap = {}
    if props.get('IsRemapped'):
        for _ in range(r.i32()):
            k, v = struct.unpack_from('>HH', data, r.p); r.p += 4
            remap[k] = v
    else:
        remap = {c: c for c in range(len(chars))}
    glyphs = []
    for cp, idx in sorted(remap.items()):
        if idx >= len(chars):
            continue
        u, v, w, h, page, yoff = chars[idx]
        glyphs.append((cp, page, u, v, w, h, yoff))
    line_height = max((g[5] + g[6]) for g in glyphs if g[5] > 0)
    ascent = round(line_height * props['Ascent'] / (props['Ascent'] + props['Descent']))
    out = bytearray(b'AWF1')
    out += struct.pack('<HHhHhH', len(pages), len(glyphs), props.get('Kerning', 0), line_height,
                       ascent, 0)
    for pg in pages:
        out += struct.pack('<HH', *pg.size)
    for cp, page, u, v, w, h, yoff in glyphs:
        out += struct.pack('<IHHHHHh', cp, page, u, v, w, h, yoff)
    for pg in pages:
        out += pg.tobytes()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(out)
    print(f'{path}: {len(glyphs)} glyphs, {len(pages)} pages, line height {line_height}, '
          f'ascent {ascent}, kerning {props.get("Kerning", 0)} -> {out_path}')


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    pkg = Package(path)
    if cmd == 'list':
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else None
        print(f'v{pkg.version}/{pkg.licensee} names {len(pkg.names)} imports {len(pkg.imports)} '
              f'exports {len(pkg.exports)}')
        for e in pkg.exports:
            c = pkg.class_name(e)
            if flt and flt not in c.lower():
                continue
            print(f'{c:24} {e["size"]:9} @{e["offset"]:#010x} {pkg.full_path(e)}')
    elif cmd == 'dump':
        out, flt = sys.argv[3], (sys.argv[4].lower() if len(sys.argv) > 4 else None)
        os.makedirs(out, exist_ok=True)
        for e in pkg.exports:
            c = pkg.class_name(e)
            if flt and flt not in c.lower():
                continue
            with open(os.path.join(out, f'{pkg.full_path(e)}.{c}'), 'wb') as f:
                f.write(pkg.blob(e))
    elif cmd == 'textures':
        out, flt = sys.argv[3], (sys.argv[4].lower() if len(sys.argv) > 4 else None)
        os.makedirs(out, exist_ok=True)
        for e in pkg.exports:
            if pkg.class_name(e) != 'Texture2D':
                continue
            path = pkg.full_path(e)
            if flt and flt not in path.lower():
                continue
            try:
                img, props = decode_texture(pkg, e)
                img.save(os.path.join(out, path + '.png'))
                print(f'{path}: {img.size[0]}x{img.size[1]} {props.get("Format")}')
            except Exception as ex:  # keep going; report
                print(f'{path}: skipped ({ex})')
    elif cmd == 'font':
        export_font(pkg, sys.argv[3], sys.argv[4])
    elif cmd == 'swf':
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        for e in pkg.exports:
            if pkg.class_name(e) == 'SwfMovie':
                b = swf_bytes(pkg, e)
                with open(os.path.join(out, pkg.full_path(e) + '.gfx'), 'wb') as f:
                    f.write(b)
                print(f'{pkg.full_path(e)}: {len(b)} bytes, signature {b[:3]!r} v{b[3]}')


def read_properties(pkg, data, pos):
    """UE3 tagged properties starting at `pos`; returns (dict, end position)."""
    r = Reader(data, pos)
    props = {}
    while True:
        name = pkg.name(r.i32(), r.i32())
        if name == 'None':
            return props, r.p
        ptype = pkg.name(r.i32(), r.i32())
        size, index = r.i32(), r.i32()
        extra = None
        if ptype == 'BoolProperty':
            props[name] = bool(data[r.p]); r.p += 1
            continue
        if ptype in ('StructProperty', 'ByteProperty'):
            extra = pkg.name(r.i32(), r.i32())
        raw = data[r.p:r.p + size]; r.p += size
        if ptype in ('IntProperty', 'ObjectProperty') and size == 4:
            val = struct.unpack('>i', raw)[0]
        elif ptype == 'FloatProperty':
            val = struct.unpack('>f', raw)[0]
        elif ptype == 'NameProperty':
            val = pkg.name(*struct.unpack('>ii', raw))
        elif ptype == 'ByteProperty' and size == 8:
            val = pkg.name(*struct.unpack('>ii', raw))
        elif ptype == 'ByteProperty' and size == 1:
            val = raw[0]
        elif ptype == 'StrProperty':
            val = Reader(raw).fstring()
        else:
            val = (ptype, extra, raw)
        props[name if index == 0 else f'{name}[{index}]'] = val


if __name__ == '__main__':
    main()
