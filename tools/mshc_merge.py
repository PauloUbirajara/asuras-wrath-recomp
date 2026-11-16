#!/usr/bin/env python3
"""Debugging: mshc_merge.py <base.mshc> <patch.mshc> <out.mshc>

Writes base with the translated entries of patch (e.g. from
`mantra_shadergen --only <hash>` with MANTRA_HLSL_PATCH_FILE) replacing the
base entries of the same exact hash. Blobs are appended at the end."""
import struct
import sys

HDR = struct.Struct('<4sIII')
ENTRY = struct.Struct('<QQ12I')  # 64 bytes, see shader_cache_format.h
SPIRV_OFF, SPIRV_SIZE, VE_OFF, VE_COUNT, EXPORTS = 2, 3, 8, 9, 11  # indices into the 12 uints


def load(path):
    data = open(path, 'rb').read()
    magic, version, count, _ = HDR.unpack_from(data)
    entries = [list(ENTRY.unpack_from(data, HDR.size + i * ENTRY.size)) for i in range(count)]
    blobs = data[HDR.size + count * ENTRY.size:]
    return magic, version, entries, blobs


def main():
    base_path, patch_path, out_path = sys.argv[1:4]
    magic, version, entries, blobs = load(base_path)
    pmagic, pversion, pentries, pblobs = load(patch_path)
    assert (magic, version) == (pmagic, pversion), 'cache versions differ'
    blobs = bytearray(blobs)
    index = {e[0]: i for i, e in enumerate(entries)}
    for e in pentries:
        if e[0] not in index or e[2 + SPIRV_SIZE] == 0:  # unknown hash or no SPIR-V
            continue
        u = e[2:]
        spirv = pblobs[u[SPIRV_OFF]:u[SPIRV_OFF] + u[SPIRV_SIZE]]
        # Vertex elements, then (v6) the interpolator export words.
        ve = pblobs[u[VE_OFF]:u[VE_OFF] + (u[VE_COUNT] + (u[EXPORTS] if pversion >= 6 else 0)) * 4]
        u[SPIRV_OFF] = len(blobs); blobs += spirv
        u[VE_OFF] = len(blobs); blobs += ve
        entries[index[e[0]]] = e[:2] + u
        print(f'replaced {e[0]:016X}')
    with open(out_path, 'wb') as f:
        f.write(HDR.pack(magic, version, len(entries), 0))
        for e in entries:
            f.write(ENTRY.pack(*e))
        f.write(blobs)


if __name__ == '__main__':
    main()
