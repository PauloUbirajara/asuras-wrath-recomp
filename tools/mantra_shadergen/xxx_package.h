#pragma once
// Reader for Asura's Wrath (UE3 / CyberConnect2) .xxx packages: each
// compressed chunk is a UE3 compressed-chunk header followed by 0x20000-byte
// blocks of XDK-framed LZX ([u16 size][bitstream] segments, a 0xFF-prefixed
// final segment). Known limitation: 5 STE stream packages use LZX uncompressed
// blocks that libmspack decodes differently and fail.

#include <cstdint>
#include <vector>

namespace mantra_shadergen {

struct PackageStats {
  uint32_t chunks = 0;
  uint32_t failed_chunks = 0;
};

// Appends the decompressed body of every compressed chunk in `file` to `out`.
// Returns false if any chunk failed (the successful ones are still appended).
bool DecompressPackage(const std::vector<uint8_t>& file, std::vector<uint8_t>& out,
                       PackageStats& stats);

// UE3 compressed shader code (FShaderCache's per-type code chunks) inside a
// decompressed package body: [u32 uncompressed][u32 compressed][framed LZX].
// Appends every chunk that decodes to `out`; returns how many did.
uint32_t DecompressEmbeddedChunks(const std::vector<uint8_t>& body, std::vector<uint8_t>& out);

}  // namespace mantra_shadergen
