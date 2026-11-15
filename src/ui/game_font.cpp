#include "game_font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <rex/logging.h>

namespace asura::ui {
namespace {

struct Glyph {
  uint16_t page, x, y, w, h;
  int16_t y_offset;
};

struct Page {
  uint16_t width, height;
  const uint8_t* alpha;
};

struct FontData {
  int16_t kerning = 0;
  uint16_t line_height = 1;
  int16_t ascent = 0;
  std::vector<Page> pages;
  std::unordered_map<uint32_t, Glyph> glyphs;
};

template <typename T>
bool Read(const uint8_t*& p, const uint8_t* end, T& out) {
  if (size_t(end - p) < sizeof(T)) return false;
  std::memcpy(&out, p, sizeof(T));
  p += sizeof(T);
  return true;
}

// Parses an .awf blob (owned by the atlas source, so pages point into it).
bool Parse(const uint8_t* data, size_t size, FontData& font) {
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  if (size < 16 || std::memcmp(p, "AWF1", 4) != 0) return false;
  p += 4;
  uint16_t page_count, glyph_count, reserved;
  if (!Read(p, end, page_count) || !Read(p, end, glyph_count) || !Read(p, end, font.kerning) ||
      !Read(p, end, font.line_height) || !Read(p, end, font.ascent) || !Read(p, end, reserved)) {
    return false;
  }
  font.pages.resize(page_count);
  for (Page& page : font.pages) {
    if (!Read(p, end, page.width) || !Read(p, end, page.height)) return false;
  }
  for (uint16_t i = 0; i < glyph_count; ++i) {
    uint32_t codepoint;
    Glyph g;
    if (!Read(p, end, codepoint) || !Read(p, end, g.page) || !Read(p, end, g.x) ||
        !Read(p, end, g.y) || !Read(p, end, g.w) || !Read(p, end, g.h) ||
        !Read(p, end, g.y_offset)) {
      return false;
    }
    font.glyphs[codepoint] = g;
  }
  for (Page& page : font.pages) {
    const size_t bytes = size_t(page.width) * page.height;
    if (size_t(end - p) < bytes) return false;
    page.alpha = p;
    p += bytes;
  }
  for (const auto& [codepoint, g] : font.glyphs) {
    if (g.page >= font.pages.size() || g.x + g.w > font.pages[g.page].width ||
        g.y + g.h > font.pages[g.page].height) {
      return false;
    }
  }
  return font.line_height > 0;
}

FontData* Data(ImFontConfig* src) { return static_cast<FontData*>(src->FontLoaderData); }

bool FontSrcInit(ImFontAtlas*, ImFontConfig* src) {
  auto* font = new FontData();
  if (!Parse(static_cast<const uint8_t*>(src->FontData), size_t(src->FontDataSize), *font)) {
    delete font;
    return false;
  }
  src->FontLoaderData = font;
  return true;
}

void FontSrcDestroy(ImFontAtlas*, ImFontConfig* src) {
  delete Data(src);
  src->FontLoaderData = nullptr;
}

bool FontSrcContainsGlyph(ImFontAtlas*, ImFontConfig* src, ImWchar codepoint) {
  return Data(src)->glyphs.count(codepoint) != 0;
}

bool FontBakedInit(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
  if (!src->MergeMode) {
    const FontData& font = *Data(src);
    const float scale = baked->Size / font.line_height;
    baked->Ascent = std::ceil(font.ascent * scale);
    baked->Descent = -std::floor((font.line_height - font.ascent) * scale);
  }
  return true;
}

// Area-filtered resample of a glyph's alpha to (dst_w, dst_h).
void Resample(const Page& page, const Glyph& g, int dst_w, int dst_h, uint8_t* out) {
  const float sx = float(g.w) / dst_w, sy = float(g.h) / dst_h;
  constexpr int kTaps = 4;
  for (int dy = 0; dy < dst_h; ++dy) {
    for (int dx = 0; dx < dst_w; ++dx) {
      float sum = 0.0f;
      for (int ty = 0; ty < kTaps; ++ty) {
        for (int tx = 0; tx < kTaps; ++tx) {
          // Bilinear tap inside the destination pixel's footprint.
          const float fx = (dx + (tx + 0.5f) / kTaps) * sx - 0.5f;
          const float fy = (dy + (ty + 0.5f) / kTaps) * sy - 0.5f;
          const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
          const float ax = fx - x0, ay = fy - y0;
          auto at = [&](int x, int y) {
            x = std::clamp(x, 0, int(g.w) - 1);
            y = std::clamp(y, 0, int(g.h) - 1);
            return float(page.alpha[size_t(g.y + y) * page.width + g.x + x]);
          };
          sum += (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) +
                 (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
        }
      }
      out[size_t(dy) * dst_w + dx] = uint8_t(std::clamp(sum / (kTaps * kTaps), 0.0f, 255.0f));
    }
  }
}

bool FontBakedLoadGlyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*,
                        ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x) {
  const FontData& font = *Data(src);
  const auto it = font.glyphs.find(codepoint);
  if (it == font.glyphs.end()) return false;
  const Glyph& g = it->second;
  const float scale = baked->Size / font.line_height;
  const float advance = std::max(0.0f, float(g.w + font.kerning) * scale);
  if (out_advance_x) {
    *out_advance_x = advance;
    return true;
  }
  out_glyph->Codepoint = codepoint;
  out_glyph->AdvanceX = advance;
  if (!g.w || !g.h) return true;  // e.g. space

  const float density = src->RasterizerDensity * baked->RasterizerDensity;
  const int w = std::max(1, int(std::ceil(g.w * scale * density)));
  const int h = std::max(1, int(std::ceil(g.h * scale * density)));
  const ImFontAtlasRectId pack_id = ImFontAtlasPackAddRect(atlas, w, h);
  if (pack_id == ImFontAtlasRectId_Invalid) return false;
  ImTextureRect* r = ImFontAtlasPackGetRect(atlas, pack_id);
  std::vector<uint8_t> pixels(size_t(w) * h);
  Resample(font.pages[g.page], g, w, h, pixels.data());

  out_glyph->X0 = 0.0f;
  out_glyph->Y0 = std::round(g.y_offset * scale);
  out_glyph->X1 = w / density;
  out_glyph->Y1 = out_glyph->Y0 + h / density;
  out_glyph->Visible = true;
  out_glyph->PackId = pack_id;
  ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, r, pixels.data(),
                                     ImTextureFormat_Alpha8, w);
  return true;
}

const ImFontLoader* Loader() {
  static const ImFontLoader loader = [] {
    ImFontLoader l;
    l.Name = "asura_awf";
    l.FontSrcInit = FontSrcInit;
    l.FontSrcDestroy = FontSrcDestroy;
    l.FontSrcContainsGlyph = FontSrcContainsGlyph;
    l.FontBakedInit = FontBakedInit;
    l.FontBakedLoadGlyph = FontBakedLoadGlyph;
    return l;
  }();
  return &loader;
}

}  // namespace

std::filesystem::path GameUiDir() {
  if (const char* dir = std::getenv("ASURA_UI_DIR"); dir && *dir) return dir;
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "asura_wrath" / "ui";
  }
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".cache" / "asura_wrath" / "ui";
}

ImFont* AddGameFont(ImFontAtlas* atlas, const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_INFO("UI font {} not found (make it with tools/asset_extract.py font)", path.string());
    return nullptr;
  }
  std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), {});
  FontData check;
  if (!Parse(blob.data(), blob.size(), check)) {
    REXLOG_WARN("UI font {} is not a valid .awf file", path.string());
    return nullptr;
  }
  ImFontConfig config;
  config.FontData = blob.data();
  config.FontDataSize = int(blob.size());
  config.FontDataOwnedByAtlas = false;  // AddFont copies it
  config.FontLoader = Loader();
  config.SizePixels = float(check.line_height);
  std::snprintf(config.Name, sizeof(config.Name), "%s", path.stem().string().c_str());
  return atlas->AddFont(&config);
}

}  // namespace asura::ui
