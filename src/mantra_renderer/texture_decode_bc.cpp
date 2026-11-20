// BC1-BC5 block decoders (see texture_decode_bc.h). Interpolation follows the
// D3D10 BC rules: thirds for colors, sevenths/fifths for alpha, rounded.
#include "texture_decode_bc.h"

#include <cstring>

namespace mantra::bc {

namespace {

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Load32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

void Expand565(uint16_t c, uint8_t rgb[3]) {
  const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  rgb[0] = uint8_t((r << 3) | (r >> 2));
  rgb[1] = uint8_t((g << 2) | (g >> 4));
  rgb[2] = uint8_t((b << 3) | (b >> 2));
}

// The color half of BC1-BC3: 4 RGBA palette entries. BC2/BC3 always use the
// 4-color mode; BC1 switches to 3 colors + transparent black when c0 <= c1.
void ColorPalette(const uint8_t* block, bool allow_3_color, uint8_t palette[4][4]) {
  const uint16_t c0 = Load16(block), c1 = Load16(block + 2);
  Expand565(c0, palette[0]);
  Expand565(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255;
  if (c0 > c1 || !allow_3_color) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i] + 1) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((palette[0][i] + palette[1][i] + 1) / 2);
      palette[3][i] = 0;
    }
    palette[2][3] = 255;
    palette[3][3] = 0;
  }
}

void DecodeColor(const uint8_t* block, bool allow_3_color, uint8_t* out, size_t row_pitch) {
  uint8_t palette[4][4];
  ColorPalette(block, allow_3_color, palette);
  const uint32_t indices = Load32(block + 4);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t i = (indices >> (2 * (y * 4 + x))) & 3;
      std::memcpy(out + y * row_pitch + x * 4, palette[i], 4);
    }
  }
}

// An 8-byte BC3/BC4 alpha block -> 16 values, each written at
// out + y * row_pitch + x * stride.
void DecodeAlpha(const uint8_t* block, uint8_t* out, size_t row_pitch, size_t stride) {
  const uint32_t a0 = block[0], a1 = block[1];
  uint8_t values[8];
  values[0] = uint8_t(a0);
  values[1] = uint8_t(a1);
  if (a0 > a1) {
    for (uint32_t i = 1; i < 7; ++i) {
      values[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
    }
  } else {
    for (uint32_t i = 1; i < 5; ++i) {
      values[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
    }
    values[6] = 0;
    values[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
  for (uint32_t y = 0; y < 4; ++y) {
    for (uint32_t x = 0; x < 4; ++x) {
      out[y * row_pitch + x * stride] = values[(indices >> (3 * (y * 4 + x))) & 7];
    }
  }
}

}  // namespace

void DecodeBC1(const uint8_t* block, uint8_t* out, size_t row_pitch) {
  DecodeColor(block, true, out, row_pitch);
}

void DecodeBC2(const uint8_t* block, uint8_t* out, size_t row_pitch) {
  DecodeColor(block + 8, false, out, row_pitch);
  for (uint32_t y = 0; y < 4; ++y) {
    const uint16_t row = Load16(block + 2 * y);
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t a = (row >> (4 * x)) & 15;
      out[y * row_pitch + x * 4 + 3] = uint8_t(a * 17);
    }
  }
}

void DecodeBC3(const uint8_t* block, uint8_t* out, size_t row_pitch) {
  DecodeColor(block + 8, false, out, row_pitch);
  DecodeAlpha(block, out + 3, row_pitch, 4);
}

void DecodeBC4(const uint8_t* block, uint8_t* out, size_t row_pitch) {
  DecodeAlpha(block, out, row_pitch, 1);
}

void DecodeBC5(const uint8_t* block, uint8_t* out, size_t row_pitch) {
  DecodeAlpha(block, out, row_pitch, 2);
  DecodeAlpha(block + 8, out + 1, row_pitch, 2);
}

}  // namespace mantra::bc
