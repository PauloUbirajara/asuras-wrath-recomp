#pragma once
// BC1-BC5 (DXT1/3/5, DXT5A, DXN) blocks to plain texels, for GPUs that can't
// sample BC formats (most Android ones). Each call decodes one 4x4 block
// (little-endian, as the host BC formats take it) into 4 rows of 4 texels,
// `row_pitch` bytes apart.
#include <cstddef>
#include <cstdint>

namespace mantra::bc {

// BC1 -> RGBA8 (1-bit alpha: transparent black in 3-color mode).
void DecodeBC1(const uint8_t* block, uint8_t* out, size_t row_pitch);
// BC2 (explicit 4-bit alpha) -> RGBA8.
void DecodeBC2(const uint8_t* block, uint8_t* out, size_t row_pitch);
// BC3 (interpolated alpha) -> RGBA8.
void DecodeBC3(const uint8_t* block, uint8_t* out, size_t row_pitch);
// BC4 -> R8.
void DecodeBC4(const uint8_t* block, uint8_t* out, size_t row_pitch);
// BC5 -> R8G8.
void DecodeBC5(const uint8_t* block, uint8_t* out, size_t row_pitch);

}  // namespace mantra::bc
